/*
 * Copyright (C) 2026 by Thun Lu. All rights reserved.
 * Author: Thun Lu <thun.lu@zohomail.cn>
 * Repo:   https://github.com/thun-res/vlink
 *  _    __   __      _           __
 * | |  / /  / /     (_) ____    / /__
 * | | / /  / /     / / / __ \  / //_/
 * | |/ /  / /___  / / / / / / / ,<
 * |___/  /_____/ /_/ /_/ /_/ /_/|_|
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "./base/coroutine.h"

#ifdef VLINK_ENABLE_COROUTINE

#include <chrono>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "./base/condition_variable.h"
#include "./base/graph_task.h"
#include "./base/logger.h"
#include "./base/memory_pool.h"
#include "./base/memory_resource.h"
#include "./base/message_loop.h"

namespace vlink {

namespace Coroutine {  // NOLINT(readability-identifier-naming)

// PostedCallbackState
struct PostedCallbackState final {
  explicit PostedCallbackState(MoveFunction<void()>&& callback) noexcept : drop_callback(std::move(callback)) {}

  void drop_once() {
    bool expected = false;

    if VUNLIKELY (!dropped.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      return;
    }

    if VLIKELY (drop_callback) {
      drop_callback();
    }
  }

  MoveFunction<void()> drop_callback;
  std::atomic_bool armed{false};
  std::atomic_bool executed{false};
  std::atomic_bool drop_requested{false};
  std::atomic_bool dropped{false};
};

// PostedCallback
class PostedCallback final {
 public:
  PostedCallback(std::shared_ptr<PostedCallbackState> state, MoveFunction<void()>&& callback) noexcept
      : state_(std::move(state)), callback_(std::move(callback)) {}

  PostedCallback(PostedCallback&& other) noexcept
      : state_(std::move(other.state_)), callback_(std::move(other.callback_)) {}

  PostedCallback& operator=(PostedCallback&& other) noexcept {
    if VUNLIKELY (this == &other) {
      return *this;
    }

    drop_if_needed();

    state_ = std::move(other.state_);
    callback_ = std::move(other.callback_);

    return *this;
  }

  ~PostedCallback() { drop_if_needed(); }

  void operator()() {
    if VLIKELY (state_) {
      state_->executed.store(true, std::memory_order_release);
    }

    if VLIKELY (callback_) {
      callback_();
    }
  }

 private:
  void drop_if_needed() noexcept {
    if VUNLIKELY (!state_) {
      return;
    }

    if VLIKELY (state_->executed.load(std::memory_order_acquire)) {
      return;
    }

    if VUNLIKELY (!state_->armed.load(std::memory_order_acquire)) {
      state_->drop_requested.store(true, std::memory_order_release);

      return;
    }

    state_->drop_once();
  }

  std::shared_ptr<PostedCallbackState> state_;
  MoveFunction<void()> callback_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(PostedCallback)
};

// FutureWaitLoop
class FutureWaitLoop final {
 public:
  static FutureWaitLoop& instance() {
    static FutureWaitLoop holder;
    return holder;
  }

  static void register_poll(MoveFunction<bool()>&& poll) { instance().enqueue(std::move(poll)); }

 private:
  FutureWaitLoop() : thread_([this]() { poll_loop(); }) {}

  ~FutureWaitLoop() {
    {
      std::lock_guard lock(mtx_);
      stopping_ = true;
      ++generation_;
    }

    cv_.notify_all();

    if (thread_.joinable()) {
      thread_.join();
    }
  }

  void enqueue(MoveFunction<bool()>&& poll) {
    if VUNLIKELY (!poll) {
      return;
    }

    {
      std::lock_guard lock(mtx_);

      if VUNLIKELY (stopping_) {
        return;
      }

      pending_.push_back(std::move(poll));

      ++generation_;
    }

    cv_.notify_one();
  }

  void poll_loop() {
    for (;;) {
      std::vector<MoveFunction<bool()>> snapshot;

      {
        std::unique_lock lock(mtx_);

        cv_.wait(lock, [this] { return stopping_ || !pending_.empty(); });

        if VUNLIKELY (stopping_) {
          return;
        }

        snapshot.swap(pending_);
      }

      poll_snapshot(std::move(snapshot));
    }
  }

  void poll_snapshot(std::vector<MoveFunction<bool()>>&& snapshot) {
    std::vector<MoveFunction<bool()>> not_done;
    not_done.reserve(snapshot.size());

    for (auto& poll : snapshot) {
      bool done = false;

      try {
        done = poll();
      } catch (const std::exception& e) {
        CLOG_E("FutureWaitLoop: poll closure threw an exception: %s.", e.what());
        done = true;
      } catch (...) {
        CLOG_E("FutureWaitLoop: poll closure threw a non-std exception.");
        done = true;
      }

      if (!done) {
        not_done.push_back(std::move(poll));
      }
    }

    {
      std::unique_lock lock(mtx_);

      const bool has_new_polls = !pending_.empty();

      for (auto& poll : not_done) {
        pending_.push_back(std::move(poll));
      }

      if VUNLIKELY (stopping_ || pending_.empty() || has_new_polls) {
        return;
      }

      const size_t generation = generation_;

      cv_.wait_for(lock, std::chrono::milliseconds(1),
                   [this, generation] { return stopping_ || generation_ != generation; });
    }
  }

  std::mutex mtx_;
  ConditionVariable cv_;
  std::vector<MoveFunction<bool()>> pending_;
  std::thread thread_;
  size_t generation_{0};
  bool stopping_{false};
};

// GraphAwaiter::State
struct GraphAwaiter::State final {
  void set_handle(std::coroutine_handle<> h) {
    std::lock_guard lock(mtx);

    if VLIKELY (!abandoned.load(std::memory_order_acquire)) {
      handle = h;
    }
  }

  std::coroutine_handle<> take_handle() {
    std::lock_guard lock(mtx);

    return std::exchange(handle, {});
  }

  void clear_handle() {
    std::lock_guard lock(mtx);

    handle = {};
  }

  void resume_ready() {
    auto h = take_handle();

    if VLIKELY (h) {
      h.resume();
    }
  }

  void cancel_and_resume() {
    target_closed.store(true, std::memory_order_release);

    auto h = take_handle();

    if VLIKELY (h) {
      h.resume();
    }
  }

  std::mutex mtx;
  std::coroutine_handle<> handle;
  std::atomic_bool fired{false};
  std::atomic_bool abandoned{false};
  std::atomic_bool target_closed{false};
  std::atomic<uint32_t> id{0};
};

// WhenAllVoidState
struct WhenAllVoidState final {
  std::atomic<size_t> remaining{0};
  std::mutex exc_mtx;
  std::exception_ptr first_exc;
  std::promise<void> promise;
};

// WhenAllVoidGuard
struct WhenAllVoidGuard final {
  std::shared_ptr<WhenAllVoidState> state;
  bool completed_normally{false};

  explicit WhenAllVoidGuard(std::shared_ptr<WhenAllVoidState> s) noexcept : state(std::move(s)) {}

  WhenAllVoidGuard(WhenAllVoidGuard&& other) noexcept
      : state(std::move(other.state)), completed_normally(other.completed_normally) {}

  WhenAllVoidGuard(const WhenAllVoidGuard&) = delete;
  WhenAllVoidGuard& operator=(const WhenAllVoidGuard&) = delete;
  WhenAllVoidGuard& operator=(WhenAllVoidGuard&&) = delete;

  ~WhenAllVoidGuard() {
    if VUNLIKELY (!state) {
      return;
    }

    if VUNLIKELY (!completed_normally) {
      std::lock_guard lock(state->exc_mtx);

      if VLIKELY (!state->first_exc) {
        state->first_exc = std::make_exception_ptr(
            std::runtime_error("vlink::Coroutine::when_all: sub-task aborted before completion"));
      }
    }

    if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      if VUNLIKELY (state->first_exc) {
        state->promise.set_exception(state->first_exc);
      } else {
        state->promise.set_value();
      }
    }
  }
};

// WhenAnyVoidState
struct WhenAnyVoidState final {
  std::atomic_bool fired{false};
  std::atomic<size_t> remaining{0};
  std::mutex exc_mtx;
  std::exception_ptr first_exc;
  size_t winner_idx{0};
  bool has_winner{false};
  std::promise<size_t> promise;
};

// WhenAnyVoidGuard
struct WhenAnyVoidGuard final {
  std::shared_ptr<WhenAnyVoidState> state;
  bool completed_normally{false};

  explicit WhenAnyVoidGuard(std::shared_ptr<WhenAnyVoidState> s) noexcept : state(std::move(s)) {}

  WhenAnyVoidGuard(WhenAnyVoidGuard&& other) noexcept
      : state(std::move(other.state)), completed_normally(other.completed_normally) {}

  WhenAnyVoidGuard(const WhenAnyVoidGuard&) = delete;
  WhenAnyVoidGuard& operator=(const WhenAnyVoidGuard&) = delete;
  WhenAnyVoidGuard& operator=(WhenAnyVoidGuard&&) = delete;

  ~WhenAnyVoidGuard() {
    if VUNLIKELY (!state) {
      return;
    }

    if VUNLIKELY (!completed_normally) {
      std::lock_guard lock(state->exc_mtx);

      if VLIKELY (!state->first_exc) {
        state->first_exc = std::make_exception_ptr(
            std::runtime_error("vlink::Coroutine::when_any: sub-task aborted before completion"));
      }
    }

    if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      if VLIKELY (state->has_winner) {
        state->promise.set_value(state->winner_idx);
      } else {
        state->promise.set_exception(state->first_exc);
      }
    }
  }
};

// detail::AwaiterResumeState
struct detail::AwaiterResumeState final {
  void set_handle(std::coroutine_handle<> h) {
    std::lock_guard lock(mtx);

    if VLIKELY (!abandoned.load(std::memory_order_acquire)) {
      handle = h;
    }
  }

  std::coroutine_handle<> take_handle() {
    std::lock_guard lock(mtx);

    return std::exchange(handle, {});
  }

  void clear_handle() {
    std::lock_guard lock(mtx);

    handle = {};
  }

  void resume_ready() {
    auto h = take_handle();

    if VLIKELY (h) {
      h.resume();
    }
  }

  void fail_and_resume() {
    failed.store(true, std::memory_order_release);

    auto h = take_handle();

    if VLIKELY (h) {
      h.resume();
    }
  }

  std::mutex mtx;
  std::coroutine_handle<> handle;
  std::atomic_bool abandoned{false};
  std::atomic_bool failed{false};
};

static void resume_failed_later(std::shared_ptr<detail::AwaiterResumeState> state) {
  detail::register_future_wait(MoveFunction<bool()>([state = std::move(state)]() {
    if VLIKELY (!state->abandoned.load(std::memory_order_acquire)) {
      state->fail_and_resume();
    }

    return true;
  }));
}

static detail::ResumePostResult post_awaiter_resume(MessageLoop* loop,
                                                    const std::shared_ptr<MessageLoop::AliveState>& alive_state,
                                                    const std::shared_ptr<detail::AwaiterResumeState>& state,
                                                    uint16_t priority) {
  return detail::post_callback_if_alive(loop, alive_state, MoveFunction<void()>([state]() { state->resume_ready(); }),
                                        MoveFunction<void()>([state]() { resume_failed_later(state); }), priority);
}

static void retry_awaiter_resume(MessageLoop* loop, std::shared_ptr<MessageLoop::AliveState> alive_state,
                                 std::shared_ptr<detail::AwaiterResumeState> state, uint16_t priority) {
  detail::register_future_wait(
      MoveFunction<bool()>([loop, alive_state = std::move(alive_state), state = std::move(state), priority,
                            retry_count = 0U]() mutable -> bool {
        if VUNLIKELY (state->abandoned.load(std::memory_order_acquire)) {
          return true;
        }

        const auto result = post_awaiter_resume(loop, alive_state, state, priority);

        if VLIKELY (result != detail::ResumePostResult::kRetry) {
          return true;
        }

        if VUNLIKELY (++retry_count >= detail::kMaxResumePostRetry) {
          resume_failed_later(state);

          return true;
        }

        return false;
      }));
}

static bool post_callback(MessageLoop& loop, MoveFunction<void()>&& resume_callback,
                          MoveFunction<void()>&& drop_callback, uint16_t priority) {
  PostTaskOptions options;
  options.drop_policy = TaskDropPolicy::kProtected;
  options.overflow_policy = TaskOverflowPolicy::kReject;

  auto state = MemoryResource::make_shared<PostedCallbackState>(std::move(drop_callback));
  auto callback = PostedCallback{state, std::move(resume_callback)};

  TaskHandle task_handle;

  if VUNLIKELY (loop.get_type() == MessageLoop::kPriorityType) {
    const uint16_t effective_priority =
        priority == MessageLoop::kNoPriority ? static_cast<uint16_t>(MessageLoop::kNormalPriority) : priority;

    task_handle = loop.post_task_with_priority_handle(std::move(callback), effective_priority, options);
  } else {
    task_handle = loop.post_task_handle(std::move(callback), options);
  }

  state->armed.store(true, std::memory_order_release);

  const auto task_state = task_handle.state();

  const bool posted = task_state == TaskExecutionState::kQueued || task_state == TaskExecutionState::kRunning ||
                      task_state == TaskExecutionState::kCompleted;
  const bool dropped =
      task_state == TaskExecutionState::kDropped || (posted && state->drop_requested.load(std::memory_order_acquire));

  if VUNLIKELY (dropped) {
    state->drop_once();

    return true;
  }

  return posted;
}

static bool post_owned_resume(MessageLoop& loop, std::coroutine_handle<> handle, uint16_t priority) {
  return post_callback(loop, MoveFunction<void()>([handle]() { handle.resume(); }),
                       MoveFunction<void()>([handle]() { handle.destroy(); }), priority);
}

static void retry_detached_resume(MessageLoop* loop, std::shared_ptr<MessageLoop::AliveState> alive_state,
                                  detail::DetachedTask::Handle handle, uint16_t priority) {
  detail::register_future_wait(MoveFunction<bool()>(
      [loop, alive_state = std::move(alive_state), handle, priority, retry_count = 0U]() mutable -> bool {
        std::lock_guard lock(alive_state->mtx);

        if VUNLIKELY (!alive_state->alive.load(std::memory_order_acquire) || loop->is_ready_to_quit()) {
          handle.destroy();
          handle = {};

          return true;
        }

        if VLIKELY (post_owned_resume(*loop, handle, priority)) {
          handle = {};

          return true;
        }

        if VUNLIKELY (++retry_count >= detail::kMaxResumePostRetry) {
          handle.destroy();
          handle = {};

          return true;
        }

        return false;
      }));
}

static Task<void> when_all_void_runner(Task<void> task, WhenAllVoidGuard guard) {
  try {
    co_await std::move(task);
  } catch (...) {
    std::lock_guard lock(guard.state->exc_mtx);

    if VLIKELY (!guard.state->first_exc) {
      guard.state->first_exc = std::current_exception();
    }
  }

  guard.completed_normally = true;
}

static Task<void> when_any_void_runner(Task<void> task, size_t i, WhenAnyVoidGuard guard) {
  try {
    co_await std::move(task);

    bool expected = false;

    if VLIKELY (guard.state->fired.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      guard.state->winner_idx = i;
      guard.state->has_winner = true;
    }
  } catch (...) {
    std::lock_guard lock(guard.state->exc_mtx);

    if VLIKELY (!guard.state->first_exc) {
      guard.state->first_exc = std::current_exception();
    }
  }

  guard.completed_normally = true;
}

void* detail::allocate_frame(size_t size) {
  void* ptr = MemoryPool::global_instance().allocate(size);

  if VLIKELY (ptr != nullptr) {
    return ptr;
  }

  throw std::bad_alloc{};
}

void detail::deallocate_frame(void* ptr, size_t size) noexcept {
  if VUNLIKELY (ptr == nullptr) {
    return;
  }

  MemoryPool::global_instance().deallocate(ptr, size);
}

// DetachedTask
detail::DetachedTask::DetachedTask() noexcept = default;

detail::DetachedTask::~DetachedTask() = default;

detail::DetachedTask::DetachedTask(Handle h) noexcept : handle(h) {}

detail::DetachedTask::DetachedTask(DetachedTask&& other) noexcept : handle(std::exchange(other.handle, {})) {}

detail::DetachedTask& detail::DetachedTask::operator=(DetachedTask&& other) noexcept {
  handle = std::exchange(other.handle, {});
  return *this;
}

void* detail::DetachedTask::promise_type::operator new(size_t size) { return detail::allocate_frame(size); }

void detail::DetachedTask::promise_type::operator delete(void* ptr, size_t size) noexcept {
  detail::deallocate_frame(ptr, size);
}

detail::DetachedTask detail::DetachedTask::promise_type::get_return_object() noexcept {
  return DetachedTask{std::coroutine_handle<promise_type>::from_promise(*this)};
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
std::suspend_always detail::DetachedTask::promise_type::initial_suspend() noexcept { return {}; }

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
std::suspend_never detail::DetachedTask::promise_type::final_suspend() noexcept { return {}; }

void detail::DetachedTask::promise_type::return_void() noexcept {}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void detail::DetachedTask::promise_type::unhandled_exception() noexcept {
  try {
    auto eptr = std::current_exception();

    if VUNLIKELY (!eptr) {
      return;
    }

    std::rethrow_exception(eptr);
  } catch (const std::exception& e) {
    CLOG_E("DetachedTask: coroutine threw an exception: %s.", e.what());
  } catch (...) {
    CLOG_E("DetachedTask: coroutine threw a non-std exception.");
  }
}

detail::DetachedTask detail::co_spawn_void_impl(Task<void> task) { co_await std::move(task); }

detail::ResumePostResult detail::post_callback_if_alive(MessageLoop* loop,
                                                        const std::shared_ptr<MessageLoop::AliveState>& alive_state,
                                                        MoveFunction<void()>&& resume_callback,
                                                        MoveFunction<void()>&& drop_callback, uint16_t priority) {
  if VUNLIKELY (loop == nullptr || !alive_state) {
    if VLIKELY (drop_callback) {
      drop_callback();
    }

    return ResumePostResult::kClosed;
  }

  {
    std::lock_guard lock(alive_state->mtx);

    if VLIKELY (alive_state->alive.load(std::memory_order_acquire) && !loop->is_ready_to_quit()) {
      return post_callback(*loop, std::move(resume_callback), std::move(drop_callback), priority)
                 ? ResumePostResult::kPosted
                 : ResumePostResult::kRetry;
    }
  }

  if VLIKELY (drop_callback) {
    drop_callback();
  }

  return ResumePostResult::kClosed;
}

void detail::co_spawn_detached_handle(MessageLoop& loop, DetachedTask::Handle handle, uint16_t priority) {
  auto alive_state = loop.get_alive_state();

  {
    std::lock_guard lock(alive_state->mtx);

    if VUNLIKELY (!alive_state->alive.load(std::memory_order_acquire) || loop.is_ready_to_quit()) {
      handle.destroy();

      return;
    }

    if VLIKELY (post_owned_resume(loop, handle, priority)) {
      return;
    }
  }

  retry_detached_resume(&loop, std::move(alive_state), handle, priority);
}

void detail::register_future_wait(MoveFunction<bool()>&& poll) { FutureWaitLoop::register_poll(std::move(poll)); }

// ScheduleAwaiter
ScheduleAwaiter::ScheduleAwaiter() noexcept = default;

ScheduleAwaiter::ScheduleAwaiter(MessageLoop* loop, std::shared_ptr<MessageLoop::AliveState> alive_state,
                                 std::shared_ptr<detail::AwaiterResumeState> state, uint16_t priority) noexcept
    : loop(loop), alive_state(std::move(alive_state)), state(std::move(state)), priority(priority) {}

ScheduleAwaiter::ScheduleAwaiter(ScheduleAwaiter&&) noexcept = default;

ScheduleAwaiter& ScheduleAwaiter::operator=(ScheduleAwaiter&&) noexcept = default;

bool ScheduleAwaiter::await_ready() noexcept { return false; }

ScheduleAwaiter::~ScheduleAwaiter() {
  if VLIKELY (state) {
    state->abandoned.store(true, std::memory_order_release);

    state->clear_handle();
  }
}

bool ScheduleAwaiter::await_suspend(std::coroutine_handle<> handle) {
  if VUNLIKELY (!state) {
    state = MemoryResource::make_shared<detail::AwaiterResumeState>();
  }

  state->set_handle(handle);

  const auto result = post_awaiter_resume(loop, alive_state, state, priority);

  if VUNLIKELY (result == detail::ResumePostResult::kRetry) {
    retry_awaiter_resume(loop, alive_state, state, priority);
  }

  return true;
}

// NOLINTNEXTLINE(readability-make-member-function-const)
void ScheduleAwaiter::await_resume() {
  if VLIKELY (state) {
    failed = state->failed.load(std::memory_order_acquire);

    state->abandoned.store(true, std::memory_order_release);
    state->clear_handle();
  }

  if VUNLIKELY (failed) {
    throw std::runtime_error("vlink::Coroutine::schedule: post_task to loop failed");
  }
}

// YieldAwaiter
YieldAwaiter::YieldAwaiter() noexcept = default;

YieldAwaiter::YieldAwaiter(MessageLoop* loop, std::shared_ptr<MessageLoop::AliveState> alive_state,
                           std::shared_ptr<detail::AwaiterResumeState> state, uint16_t priority) noexcept
    : loop(loop), alive_state(std::move(alive_state)), state(std::move(state)), priority(priority) {}

YieldAwaiter::YieldAwaiter(YieldAwaiter&&) noexcept = default;

YieldAwaiter& YieldAwaiter::operator=(YieldAwaiter&&) noexcept = default;

bool YieldAwaiter::await_ready() noexcept { return false; }

YieldAwaiter::~YieldAwaiter() {
  if VLIKELY (state) {
    state->abandoned.store(true, std::memory_order_release);

    state->clear_handle();
  }
}

bool YieldAwaiter::await_suspend(std::coroutine_handle<> handle) {
  if VUNLIKELY (!state) {
    state = MemoryResource::make_shared<detail::AwaiterResumeState>();
  }

  state->set_handle(handle);

  const auto result = post_awaiter_resume(loop, alive_state, state, priority);

  if VUNLIKELY (result == detail::ResumePostResult::kRetry) {
    retry_awaiter_resume(loop, alive_state, state, priority);
  }

  return true;
}

// NOLINTNEXTLINE(readability-make-member-function-const)
void YieldAwaiter::await_resume() {
  if VLIKELY (state) {
    failed = state->failed.load(std::memory_order_acquire);

    state->abandoned.store(true, std::memory_order_release);
    state->clear_handle();
  }

  if VUNLIKELY (failed) {
    throw std::runtime_error("vlink::Coroutine::yield: post_task to loop failed");
  }
}

// DelayAwaiter
DelayAwaiter::DelayAwaiter() noexcept = default;

DelayAwaiter::DelayAwaiter(MessageLoop* loop, std::shared_ptr<MessageLoop::AliveState> alive_state,
                           std::shared_ptr<detail::AwaiterResumeState> state, uint32_t ms, uint16_t priority) noexcept
    : loop(loop), alive_state(std::move(alive_state)), state(std::move(state)), ms(ms), priority(priority) {}

DelayAwaiter::DelayAwaiter(DelayAwaiter&&) noexcept = default;

DelayAwaiter& DelayAwaiter::operator=(DelayAwaiter&&) noexcept = default;

bool DelayAwaiter::await_ready() noexcept { return false; }

DelayAwaiter::~DelayAwaiter() {
  if VLIKELY (state) {
    state->abandoned.store(true, std::memory_order_release);

    state->clear_handle();
  }
}

bool DelayAwaiter::await_suspend(std::coroutine_handle<> handle) {
  if VUNLIKELY (!state) {
    state = MemoryResource::make_shared<detail::AwaiterResumeState>();
  }

  state->set_handle(handle);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);

  auto* loop_ptr = loop;
  auto loop_alive = alive_state;
  auto resume_state = state;

  const auto resume_priority = priority;

  detail::register_future_wait(
      MoveFunction<bool()>([loop_ptr, loop_alive = std::move(loop_alive), resume_state = std::move(resume_state),
                            resume_priority, deadline, retry_count = 0U]() mutable -> bool {
        if VUNLIKELY (resume_state->abandoned.load(std::memory_order_acquire)) {
          return true;
        }

        if VLIKELY (std::chrono::steady_clock::now() < deadline) {
          return false;
        }

        const auto result = post_awaiter_resume(loop_ptr, loop_alive, resume_state, resume_priority);

        if VLIKELY (result != detail::ResumePostResult::kRetry) {
          return true;
        }

        if VUNLIKELY (++retry_count >= detail::kMaxResumePostRetry) {
          resume_failed_later(resume_state);

          return true;
        }

        return false;
      }));

  return true;
}

// NOLINTNEXTLINE(readability-make-member-function-const)
void DelayAwaiter::await_resume() {
  if VLIKELY (state) {
    failed = state->failed.load(std::memory_order_acquire);

    state->abandoned.store(true, std::memory_order_release);
    state->clear_handle();
  }

  if VUNLIKELY (failed) {
    throw std::runtime_error("vlink::Coroutine::delay_ms: post_task to loop failed");
  }
}

// GraphAwaiter
GraphAwaiter::GraphAwaiter(MessageLoop* loop, GraphTaskPtr graph) noexcept
    : loop_(loop), graph_(std::move(graph)), state_(MemoryResource::make_shared<State>()) {}

GraphAwaiter::GraphAwaiter(GraphAwaiter&&) noexcept = default;

GraphAwaiter& GraphAwaiter::operator=(GraphAwaiter&&) noexcept = default;

GraphAwaiter::~GraphAwaiter() {
  if VUNLIKELY (!state_) {
    return;
  }

  state_->abandoned.store(true, std::memory_order_release);
  state_->clear_handle();

  const uint32_t id = state_->id.load(std::memory_order_acquire);

  if VLIKELY (id != 0 && graph_) {
    (void)graph_->unregister_status_callback(id);
  }
}

bool GraphAwaiter::await_ready() const noexcept {
  if VUNLIKELY (!graph_) {
    return true;
  }

  return graph_->get_status() == GraphTask::kStatusDone;
}

void GraphAwaiter::await_suspend(std::coroutine_handle<> handle) {
  auto* loop = loop_;
  auto loop_alive = loop->get_alive_state();
  auto graph = graph_;

  std::weak_ptr<GraphTask> weak_graph = graph;

  auto state = state_;

  state->set_handle(handle);

  auto resume_or_cancel = [loop, loop_alive, state, retry_count = 0U]() mutable -> bool {
    const auto result = detail::post_callback_if_alive(
        loop, loop_alive, MoveFunction<void()>([state]() { state->resume_ready(); }), MoveFunction<void()>([state]() {
          detail::register_future_wait(MoveFunction<bool()>([state]() {
            if VLIKELY (!state->abandoned.load(std::memory_order_acquire)) {
              state->cancel_and_resume();
            }

            return true;
          }));
        }));

    if VLIKELY (result == detail::ResumePostResult::kPosted) {
      return true;
    }

    if VUNLIKELY (result == detail::ResumePostResult::kRetry) {
      if VUNLIKELY (++retry_count >= detail::kMaxResumePostRetry) {
        detail::register_future_wait(MoveFunction<bool()>([state]() {
          if VLIKELY (!state->abandoned.load(std::memory_order_acquire)) {
            state->cancel_and_resume();
          }

          return true;
        }));

        return true;
      }

      return false;
    }

    return true;
  };

  uint32_t id = graph->register_status_callback(
      [state, weak_graph, resume_or_cancel](const std::string& /*name*/, GraphTask::Status status) mutable {
        if VLIKELY (status != GraphTask::kStatusDone) {
          return;
        }

        if VUNLIKELY (state->abandoned.load(std::memory_order_acquire)) {
          return;
        }

        bool expected = false;

        if VUNLIKELY (!state->fired.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
          return;
        }

        uint32_t my_id = state->id.load(std::memory_order_acquire);

        if VLIKELY (my_id != 0) {
          if VLIKELY (auto locked = weak_graph.lock()) {
            (void)locked->unregister_status_callback(my_id);
          }
        }

        if VUNLIKELY (!resume_or_cancel()) {
          detail::register_future_wait(MoveFunction<bool()>(
              [resume_or_cancel = std::move(resume_or_cancel)]() mutable -> bool { return resume_or_cancel(); }));
        }
      });

  state->id.store(id, std::memory_order_release);

  if VUNLIKELY (state->fired.load(std::memory_order_acquire)) {
    graph->unregister_status_callback(id);

    return;
  }

  if VUNLIKELY (graph->get_status() == GraphTask::kStatusDone) {
    bool expected = false;

    if VLIKELY (state->fired.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      graph->unregister_status_callback(id);

      if VUNLIKELY (!resume_or_cancel()) {
        detail::register_future_wait(MoveFunction<bool()>(
            [resume_or_cancel = std::move(resume_or_cancel)]() mutable { return resume_or_cancel(); }));
      }
    }
  }
}

void GraphAwaiter::await_resume() const {
  if VLIKELY (state_) {
    state_->abandoned.store(true, std::memory_order_release);
    state_->clear_handle();

    if VUNLIKELY (state_->target_closed.load(std::memory_order_acquire)) {
      throw Exception::OperationCancelled{};
    }
  }
}

ScheduleAwaiter schedule(MessageLoop& loop, uint16_t priority) noexcept {
  return ScheduleAwaiter{&loop, loop.get_alive_state(), MemoryResource::make_shared<detail::AwaiterResumeState>(),
                         priority};
}

YieldAwaiter yield(MessageLoop& loop, uint16_t priority) noexcept {
  return YieldAwaiter{&loop, loop.get_alive_state(), MemoryResource::make_shared<detail::AwaiterResumeState>(),
                      priority};
}

DelayAwaiter delay_ms(MessageLoop& loop, uint32_t ms, uint16_t priority) noexcept {
  return DelayAwaiter{&loop, loop.get_alive_state(), MemoryResource::make_shared<detail::AwaiterResumeState>(), ms,
                      priority};
}

void co_spawn(MessageLoop& loop, Task<void>&& task) { co_spawn_with_priority(loop, std::move(task), 0); }

void co_spawn_with_priority(MessageLoop& loop, Task<void>&& task, uint16_t priority) {
  auto detached = detail::co_spawn_void_impl(std::move(task));

  auto handle = detached.handle;
  detached.handle = {};

  detail::co_spawn_detached_handle(loop, handle, priority);
}

GraphAwaiter await_graph(MessageLoop& loop, GraphTaskPtr graph) {
  if VUNLIKELY (!graph) {
    throw std::invalid_argument("vlink::Coroutine::await_graph: graph must not be null");
  }

  return GraphAwaiter{&loop, std::move(graph)};
}

Task<void> when_all(MessageLoop& loop, std::vector<Task<void>> tasks) {
  if VUNLIKELY (tasks.empty()) {
    co_return;
  }

  auto state = MemoryResource::make_shared<WhenAllVoidState>();

  state->remaining.store(tasks.size(), std::memory_order_relaxed);

  auto fut = state->promise.get_future();

  for (auto& task : tasks) {
    co_spawn(loop, when_all_void_runner(std::move(task), WhenAllVoidGuard{state}));
  }

  co_await await_future(loop, std::move(fut));

  co_return;
}

Task<size_t> when_any(MessageLoop& loop, std::vector<Task<void>> tasks) {
  if VUNLIKELY (tasks.empty()) {
    throw std::invalid_argument("vlink::Coroutine::when_any: tasks must not be empty");
  }

  auto state = MemoryResource::make_shared<WhenAnyVoidState>();

  state->remaining.store(tasks.size(), std::memory_order_relaxed);

  auto fut = state->promise.get_future();

  for (size_t i = 0; i < tasks.size(); ++i) {
    co_spawn(loop, when_any_void_runner(std::move(tasks[i]), i, WhenAnyVoidGuard{state}));
  }

  co_return co_await await_future(loop, std::move(fut));
}

Task<void> sequence(MessageLoop& loop, std::vector<Task<void>> tasks) {
  for (auto& task : tasks) {
    if VUNLIKELY (!task.valid()) {
      continue;
    }

    auto state = MemoryResource::make_shared<WhenAllVoidState>();

    state->remaining.store(1, std::memory_order_relaxed);

    auto fut = state->promise.get_future();

    co_spawn(loop, when_all_void_runner(std::move(task), WhenAllVoidGuard{state}));

    co_await await_future(loop, std::move(fut));
  }

  co_return;
}

}  // namespace Coroutine

}  // namespace vlink

#endif  // VLINK_ENABLE_COROUTINE
