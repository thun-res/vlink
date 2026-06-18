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

#include "./base/message_loop.h"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "./base/condition_variable.h"
#include "./base/logger.h"
#include "./base/memory_pool.h"
#include "./base/memory_resource.h"
#include "./base/mpmc_queue.h"
#include "./base/utils.h"

#ifdef _WIN32
#include <Windows.h>
#undef max
#endif

namespace vlink {

static constexpr size_t kMaxTaskSize = 10000U;
static constexpr size_t kMaxTimerSize = 100U;
static constexpr uint32_t kMaxElapsedTime = 0U;
static constexpr int kMaxLockfreePushRetry = 32;

template <typename TypeT, typename TimeT, typename ReturnT>
static ReturnT get_current_time() noexcept {
  const auto& duration = std::chrono::duration_cast<TimeT>(TypeT::now().time_since_epoch());

  return static_cast<ReturnT>(duration.count());
}

// MessageLoopGlobal
struct MessageLoopGlobal final {
  std::atomic<int> instance_index{0};

  static MessageLoopGlobal& get() {
    static MessageLoopGlobal instance;

    return instance;
  }

 private:
  MessageLoopGlobal() = default;
};

// MessageLoop::Impl
struct MessageLoop::Impl final {  // NOLINT(clang-analyzer-optin.performance.Padding)
  using NormalTaskTuple = std::tuple<uint32_t, bool, MessageLoop::Callback>;
  using LockfreeTaskTuple = std::tuple<uint32_t, MessageLoop::Callback>;
  using PriorityTaskTuple = std::tuple<uint32_t, uint32_t, uint32_t, bool, MessageLoop::Callback>;

  struct PriorityCompare final {
    bool operator()(const PriorityTaskTuple& lhs, const PriorityTaskTuple& rhs) const {
      return priority_key(lhs) > priority_key(rhs);
    }
  };

#ifdef VLINK_ENABLE_BASE_MEMORY_RESOURCE
  using NormalQueue = std::pmr::deque<NormalTaskTuple>;
  using LockfreeQueue = MpmcQueue<LockfreeTaskTuple>;
  using PriorityQueue = std::priority_queue<PriorityTaskTuple, std::pmr::vector<PriorityTaskTuple>, PriorityCompare>;
#else
  using NormalQueue = std::deque<NormalTaskTuple>;
  using LockfreeQueue = MpmcQueue<LockfreeTaskTuple>;
  using PriorityQueue = std::priority_queue<PriorityTaskTuple, std::vector<PriorityTaskTuple>, PriorityCompare>;
#endif

  static uint64_t priority_key(const PriorityTaskTuple& task) {
    return (static_cast<uint64_t>(std::get<0>(task)) << 32) | std::get<1>(task);
  }

  static bool priority_before(const PriorityTaskTuple& lhs, const PriorityTaskTuple& rhs) {
    return priority_key(lhs) < priority_key(rhs);
  }

  alignas(64) std::atomic_bool is_running{false};
  alignas(64) std::atomic_bool quit_flag{false};
  alignas(64) std::atomic_bool force_quit_flag{false};
  alignas(64) std::atomic_bool is_busy{false};
  alignas(64) std::atomic_bool wakeup_pending{false};
  alignas(64) std::atomic_bool lockfree_needs_reset{false};
  std::shared_ptr<MessageLoop::AliveState> alive_state{MemoryResource::make_shared<MessageLoop::AliveState>()};

  std::atomic<std::thread::id> thread_id;
  alignas(64) std::atomic_size_t lockfree_task_count{0U};
  alignas(64) std::atomic_size_t lockfree_producer_count{0U};

#ifdef _WIN32
  std::atomic<HANDLE> thread_handle{nullptr};
#endif

  std::string name;
  MessageLoop::Type type{MessageLoop::kNormalType};
  std::atomic<MessageLoop::Strategy> strategy{MessageLoop::kOptimizationStrategy};

  uint32_t task_seq{0};
  std::optional<NormalQueue> normal_queue;
  std::optional<LockfreeQueue> lockfree_queue;
  std::optional<PriorityQueue> priority_droppable_queue;
  std::optional<PriorityQueue> priority_protected_queue;

  MessageLoop::Callback begin_callback;
  MessageLoop::Callback end_callback;
  MessageLoop::Callback idle_callback;

  std::thread thread;
  std::unordered_set<Timer*> timer_set;
  std::mutex mtx;
  ConditionVariable cv;
};

// MessageLoop
MessageLoop::MessageLoop() : impl_(std::make_unique<Impl>()) {
  impl_->name = "MessageLoop_" + std::to_string(MessageLoopGlobal::get().instance_index++);

  MemoryPool::global_instance();

#ifdef VLINK_ENABLE_BASE_MEMORY_RESOURCE
  impl_->normal_queue.emplace(&MemoryResource::global_instance());
#else
  impl_->normal_queue.emplace();
#endif
}

MessageLoop::MessageLoop(Type type) : impl_(std::make_unique<Impl>()) {
  impl_->name = "MessageLoop_" + std::to_string(MessageLoopGlobal::get().instance_index++);
  impl_->type = type;

  MemoryPool::global_instance();

  if (impl_->type == kNormalType) {
#ifdef VLINK_ENABLE_BASE_MEMORY_RESOURCE
    impl_->normal_queue.emplace(&MemoryResource::global_instance());
#else
    impl_->normal_queue.emplace();
#endif
  } else if (impl_->type == kLockfreeType) {
    size_t max_task_size = get_max_task_count();  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
    impl_->lockfree_queue.emplace(max_task_size);
  } else if (impl_->type == kPriorityType) {
#ifdef VLINK_ENABLE_BASE_MEMORY_RESOURCE
    impl_->priority_droppable_queue.emplace(&MemoryResource::global_instance());
    impl_->priority_protected_queue.emplace(&MemoryResource::global_instance());
#else
    impl_->priority_droppable_queue.emplace();
    impl_->priority_protected_queue.emplace();
#endif
  }
}

MessageLoop::~MessageLoop() {
  // NOLINTBEGIN
  {
    std::lock_guard lock(impl_->alive_state->mtx);
    impl_->alive_state->alive.store(false, std::memory_order_release);
  }

  if VUNLIKELY (impl_->is_running) {
    CLOG_W("MessageLoop is still running(%s).", impl_->name.c_str());
    quit();
    wait_for_quit(1000, false);
  }

  if (impl_->thread.joinable()) {
    impl_->thread.join();
  }

#ifdef _WIN32
  {
    std::unique_lock lock(impl_->mtx);
    HANDLE thread_handle = impl_->thread_handle.exchange(nullptr);

    if (thread_handle != nullptr) {
      ::CloseHandle(thread_handle);
    }
  }
#endif

  std::vector<Timer*> timers_to_delete;

  {
    std::unique_lock lock(impl_->mtx);

    for (auto iter = impl_->timer_set.begin(); iter != impl_->timer_set.end();) {
      Timer* timer = *iter;
      timer->clear();

      if (timer->is_once_type()) {
        timers_to_delete.emplace_back(timer);
        iter = impl_->timer_set.erase(iter);
      } else {
        ++iter;
      }
    }
  }

  for (auto* timer : timers_to_delete) {
    timer->~Timer();
    MemoryPool::global_instance().deallocate(timer, sizeof(Timer), alignof(Timer));
  }
  // NOLINTEND
}

MessageLoop::Type MessageLoop::get_type() const { return impl_->type; }

void MessageLoop::set_name(const std::string& name) { impl_->name = name; }

const std::string& MessageLoop::get_name() const { return impl_->name; }

MessageLoop::Strategy MessageLoop::get_strategy() const { return impl_->strategy.load(std::memory_order_acquire); }

void MessageLoop::set_strategy(Strategy strategy) { impl_->strategy.store(strategy, std::memory_order_release); }

void MessageLoop::register_begin_handler(Callback&& callback) {
  if VUNLIKELY (impl_->is_running) {
    CLOG_E("MessageLoop is running and cannot be registered(%s).", impl_->name.c_str());
    return;
  }

  impl_->begin_callback = std::move(callback);
}

void MessageLoop::register_end_handler(Callback&& callback) {
  if VUNLIKELY (impl_->is_running) {
    CLOG_E("MessageLoop is running and cannot be registered(%s).", impl_->name.c_str());
    return;
  }

  impl_->end_callback = std::move(callback);
}

void MessageLoop::register_idle_handler(Callback&& callback) {
  if VUNLIKELY (impl_->is_running) {
    CLOG_E("MessageLoop is running and cannot be registered(%s).", impl_->name.c_str());
    return;
  }

  impl_->idle_callback = std::move(callback);
}

bool MessageLoop::run() {
  bool expected = false;

  if VUNLIKELY (!impl_->is_running.compare_exchange_strong(expected, true)) {
    CLOG_W("MessageLoop has already run(%s).", impl_->name.c_str());
    return false;
  }

#ifdef _WIN32
  {
    std::unique_lock lock(impl_->mtx);
    HANDLE thread_handle = impl_->thread_handle.exchange(nullptr);

    if (thread_handle != nullptr) {
      ::CloseHandle(thread_handle);
    }
  }
#endif

  if (impl_->type == kLockfreeType && impl_->lockfree_needs_reset.exchange(false, std::memory_order_acq_rel)) {
    std::lock_guard lock(impl_->mtx);
    const auto max_task_count = get_max_task_count();
    impl_->lockfree_queue.emplace(max_task_count);
    impl_->lockfree_task_count.store(0U, std::memory_order_release);
  }

  impl_->quit_flag = false;
  impl_->force_quit_flag = false;

  do_consume();

  return true;
}

bool MessageLoop::async_run() {
  bool expected = false;

  if VUNLIKELY (!impl_->is_running.compare_exchange_strong(expected, true)) {
    CLOG_W("MessageLoop has already run(%s).", impl_->name.c_str());
    return false;
  }

  if (impl_->thread.joinable()) {
    impl_->thread.join();
  }

#ifdef _WIN32
  {
    std::unique_lock lock(impl_->mtx);
    HANDLE thread_handle = impl_->thread_handle.exchange(nullptr);

    if (thread_handle != nullptr) {
      ::CloseHandle(thread_handle);
    }
  }
#endif

  if (impl_->type == kLockfreeType && impl_->lockfree_needs_reset.exchange(false, std::memory_order_acq_rel)) {
    std::lock_guard lock(impl_->mtx);
    const auto max_task_count = get_max_task_count();
    impl_->lockfree_queue.emplace(max_task_count);
    impl_->lockfree_task_count.store(0U, std::memory_order_release);
  }

  impl_->quit_flag = false;
  impl_->force_quit_flag = false;

  impl_->thread = std::thread([this]() { do_consume(); });

  if (!impl_->name.empty()) {
    Utils::set_thread_name(impl_->name, &impl_->thread);
  }

  return true;
}

bool MessageLoop::spin() { return run(); }

bool MessageLoop::spin_once(bool block) {
  if VUNLIKELY (!is_in_same_thread() && impl_->thread_id != std::thread::id()) {
    CLOG_E("MessageLoop spin_once called from different thread than run/async_run (%s).", impl_->name.c_str());
    return false;
  }

  if (impl_->type == kNormalType) {
    return process_normal_task(block);
  } else if (impl_->type == kLockfreeType) {
    return process_lockfree_task(block);
  } else if (impl_->type == kPriorityType) {
    return process_priority_task(block);
  } else {
    return false;
  }
}

bool MessageLoop::quit(bool force) {
  if VUNLIKELY (!force && (!impl_->is_running || impl_->quit_flag)) {
    return false;
  }

  {
    std::lock_guard lock(impl_->mtx);
    impl_->quit_flag = true;
    impl_->force_quit_flag = force;
  }

  if (impl_->type == kLockfreeType) {
    std::unique_lock lock(impl_->mtx);
    impl_->cv.wait(lock, [this] { return impl_->lockfree_producer_count.load(std::memory_order_acquire) == 0U; });
  }

  drop_pending_tasks();

  if (impl_->type == kLockfreeType) {
    impl_->lockfree_queue->notify_to_quit();
    impl_->lockfree_needs_reset.store(true, std::memory_order_release);
  }

  impl_->cv.notify_all();

  return true;
}

bool MessageLoop::wait_for_quit(int ms, bool check) {
  std::unique_lock lock(impl_->mtx);

#ifdef _WIN32
  HANDLE thread_handle = impl_->thread_handle.load();

  if (thread_handle != nullptr) {
    DWORD thread_status = STILL_ACTIVE;
    if (::GetExitCodeThread(thread_handle, &thread_status) && thread_status != STILL_ACTIVE) {
      impl_->is_running = false;
      impl_->is_busy = false;
      return true;
    }
  }
#endif

  if VUNLIKELY (check && is_in_same_thread()) {
    CLOG_E("MessageLoop wait_for_quit in work thread(%s).", impl_->name.c_str());
    return false;
  }

  auto predicate = [this]() -> bool { return !impl_->is_running; };

  if (ms == Timer::kInfinite) {
    impl_->cv.wait(lock, std::move(predicate));
    return true;
  }

  return impl_->cv.wait_for(lock, std::chrono::milliseconds(ms), std::move(predicate));
}

bool MessageLoop::post_task(Callback&& callback) { return push_task(std::move(callback), kNoPriority); }

TaskHandle MessageLoop::post_task_handle(Callback&& callback, const PostTaskOptions& options) {
  auto handle = TaskHandle::make_task_handle(options.cancellation_token);
  TaskHandle::mark_task_queued(handle);

  if (handle.state() == TaskExecutionState::kCancelled) {
    return handle;
  }

  if VUNLIKELY (impl_->type == kLockfreeType && options.drop_policy == TaskDropPolicy::kProtected) {
    CLOG_W("MessageLoop: TaskDropPolicy::kProtected is ignored by lock-free queues (%s).", impl_->name.c_str());
  }

  auto tracked = TaskHandle::make_tracked_task(handle, std::move(callback));
  const bool droppable = options.drop_policy == TaskDropPolicy::kDroppable;

  if VUNLIKELY (!push_task(std::move(tracked), kNoPriority, droppable, options.overflow_policy, &handle) &&
                !handle.is_done()) {
    TaskHandle::mark_task_rejected(handle);
  }

  return handle;
}

bool MessageLoop::post_task_with_priority(Callback&& callback, uint16_t priority) {
  if VUNLIKELY (priority == kNoPriority) {
    CLOG_E("MessageLoop: Task priority cannot be zero (%s).", impl_->name.c_str());
    return false;
  }

  if VUNLIKELY (impl_->type != kPriorityType) {
    CLOG_E("MessageLoop: Task priority is not supported (%s).", impl_->name.c_str());
    return false;
  }

  return push_task(std::move(callback), priority);
}

TaskHandle MessageLoop::post_task_with_priority_handle(Callback&& callback, uint16_t priority,
                                                       const PostTaskOptions& options) {
  auto handle = TaskHandle::make_task_handle(options.cancellation_token);
  TaskHandle::mark_task_queued(handle);

  if (handle.state() == TaskExecutionState::kCancelled) {
    return handle;
  }

  if VUNLIKELY (priority == kNoPriority) {
    CLOG_E("MessageLoop: Task priority cannot be zero (%s).", impl_->name.c_str());
    TaskHandle::mark_task_rejected(handle);
    return handle;
  }

  if VUNLIKELY (impl_->type != kPriorityType) {
    CLOG_E("MessageLoop: Task priority is not supported (%s).", impl_->name.c_str());
    TaskHandle::mark_task_rejected(handle);
    return handle;
  }

  auto tracked = TaskHandle::make_tracked_task(handle, std::move(callback));
  const bool droppable = options.drop_policy == TaskDropPolicy::kDroppable;

  if VUNLIKELY (!push_task(std::move(tracked), priority, droppable, options.overflow_policy, &handle) &&
                !handle.is_done()) {
    TaskHandle::mark_task_rejected(handle);
  }

  return handle;
}

bool MessageLoop::wakeup() {
  if VUNLIKELY (!impl_->is_running) {
    return false;
  }

  bool expected = false;

  if (!impl_->wakeup_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
    return true;
  }

  // Pair the first pending wakeup with the wait mutex so a concurrent waiter cannot miss the notify.
  {
    std::lock_guard lock(impl_->mtx);
  }
  impl_->cv.notify_all();

  return true;
}

void MessageLoop::reset_lockfree_capacity() {
  if (impl_->type != kLockfreeType) {
    return;
  }

  std::lock_guard lock(impl_->mtx);

  if VUNLIKELY (impl_->is_running) {
    CLOG_E("MessageLoop: reset_lockfree_capacity called while running (%s).", impl_->name.c_str());
    return;
  }

  size_t max_task_size = get_max_task_count();
  impl_->lockfree_queue.emplace(max_task_size);
  impl_->lockfree_task_count.store(0U, std::memory_order_release);
  impl_->lockfree_needs_reset.store(false, std::memory_order_release);
}

bool MessageLoop::is_running() const { return impl_->is_running; }

bool MessageLoop::is_ready_to_quit() const { return impl_->quit_flag; }

bool MessageLoop::is_busy() const { return impl_->is_busy; }

size_t MessageLoop::get_task_count() const {
  std::lock_guard lock(impl_->mtx);

  if (impl_->type == kNormalType) {
    return impl_->normal_queue->size();
  } else if (impl_->type == kLockfreeType) {
    return impl_->lockfree_task_count.load(std::memory_order_acquire);
  } else if (impl_->type == kPriorityType) {
    return impl_->priority_droppable_queue->size() + impl_->priority_protected_queue->size();
  }

  return 0;
}

bool MessageLoop::wait_for_idle(int ms, bool check) {
  std::unique_lock lock(impl_->mtx);

#ifdef _WIN32
  HANDLE thread_handle = impl_->thread_handle.load();

  if (thread_handle != nullptr) {
    DWORD thread_status = STILL_ACTIVE;
    if (::GetExitCodeThread(thread_handle, &thread_status) && thread_status != STILL_ACTIVE) {
      impl_->is_running = false;
      impl_->is_busy = false;
      return true;
    }
  }
#endif

  if VUNLIKELY (check && is_in_same_thread()) {
    CLOG_E("MessageLoop wait_for_idle in work thread(%s).", impl_->name.c_str());
    return false;
  }

  auto predicate = [this]() -> bool {
    if (impl_->type == kNormalType) {
      return !impl_->is_running ? impl_->normal_queue->empty() : !impl_->is_busy && impl_->normal_queue->empty();
    } else if (impl_->type == kLockfreeType) {
      const bool empty = impl_->lockfree_task_count.load(std::memory_order_acquire) == 0U;
      return !impl_->is_running ? empty : !impl_->is_busy && empty;
    } else if (impl_->type == kPriorityType) {
      const bool empty = impl_->priority_droppable_queue->empty() && impl_->priority_protected_queue->empty();
      return !impl_->is_running ? empty : !impl_->is_busy && empty;
    }

    return false;
  };

  if (ms == Timer::kInfinite) {
    impl_->cv.wait(lock, std::move(predicate));
    return true;
  }

  return impl_->cv.wait_for(lock, std::chrono::milliseconds(ms), std::move(predicate));
}

size_t MessageLoop::get_max_task_count() const { return kMaxTaskSize; }

size_t MessageLoop::get_max_timer_count() const { return kMaxTimerSize; }

uint32_t MessageLoop::get_max_elapsed_time() const { return kMaxElapsedTime; }

bool MessageLoop::is_in_same_thread() const { return impl_->thread_id == std::this_thread::get_id(); }

std::shared_ptr<MessageLoop::AliveState> MessageLoop::get_alive_state() const { return impl_->alive_state; }

void MessageLoop::on_begin() {
  if (impl_->begin_callback) {
    impl_->begin_callback();
  }
}

void MessageLoop::on_end() {
  if (impl_->end_callback) {
    impl_->end_callback();
  }
}

void MessageLoop::on_idle() {
  if (impl_->idle_callback) {
    impl_->idle_callback();
  }
}

void MessageLoop::on_task_changed(Callback&& callback, uint32_t start_time) {
  if (start_time > 0 && get_max_elapsed_time() > 0) {
    uint32_t elapsed_time =
        get_current_time<std::chrono::steady_clock, std::chrono::milliseconds, uint32_t>() - start_time;

    if VUNLIKELY (elapsed_time > get_max_elapsed_time()) {
      on_task_timeout(std::move(callback), elapsed_time);
    } else {
      callback();
    }
  } else {
    callback();
  }
}

void MessageLoop::on_task_timeout(Callback&& callback, uint32_t elapsed_time) {
  (void)callback;
  CLOG_W("MessageLoop: Task timed out after %ums (%s).", elapsed_time, impl_->name.c_str());
}

uint64_t MessageLoop::get_current_nano_time() {
  return get_current_time<std::chrono::steady_clock, std::chrono::nanoseconds, uint64_t>();
}

bool MessageLoop::add_timer(Timer* timer) {
  std::lock_guard lock(impl_->mtx);

  if VUNLIKELY (impl_->quit_flag) {
    return false;
  }

  if VUNLIKELY (impl_->timer_set.size() >= get_max_timer_count()) {
    CLOG_W("MessageLoop: Timer is full (%s).", impl_->name.c_str());
    return false;
  }

  return impl_->timer_set.emplace(timer).second;
}

bool MessageLoop::remove_timer(Timer* timer) {
  std::lock_guard lock(impl_->mtx);

  return impl_->timer_set.erase(timer) != 0;
}

bool MessageLoop::drop_one_normal_task() {
  for (auto iter = impl_->normal_queue->begin(); iter != impl_->normal_queue->end(); ++iter) {
    if (std::get<1>(*iter)) {
      impl_->normal_queue->erase(iter);

      return true;
    }
  }

  return false;
}

bool MessageLoop::drop_one_lockfree_task(bool keep_reserved) {
  Impl::LockfreeTaskTuple task;

  if (!impl_->lockfree_queue->try_pop<Impl::LockfreeQueue::kNoBehavior>(task)) {
    return false;
  }

  if (!keep_reserved) {
    release_lockfree_task();
  }

  return true;
}

bool MessageLoop::drop_one_priority_task() {
  if (impl_->priority_droppable_queue->empty()) {
    return false;
  }

  impl_->priority_droppable_queue->pop();

  return true;
}

bool MessageLoop::reserve_lockfree_task() {
  auto count = impl_->lockfree_task_count.load(std::memory_order_acquire);
  const auto max_count = get_max_task_count();

  while (count < max_count) {
    if (impl_->lockfree_task_count.compare_exchange_weak(count, count + 1U, std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
      return true;
    }
  }

  return false;
}

void MessageLoop::release_lockfree_task() { impl_->lockfree_task_count.fetch_sub(1U, std::memory_order_acq_rel); }

bool MessageLoop::push_task(Callback&& callback, uint16_t priority, bool droppable, TaskOverflowPolicy overflow_policy,
                            const TaskHandle* submit_handle) {
  auto is_cancelled = [submit_handle]() -> bool {
    return submit_handle != nullptr && submit_handle->state() == TaskExecutionState::kCancelled;
  };

  auto reject = [submit_handle]() -> bool {
    if (submit_handle != nullptr && !submit_handle->is_done()) {
      TaskHandle::mark_task_rejected(*submit_handle);
    }

    return false;
  };

  if VUNLIKELY (impl_->quit_flag) {
    return reject();
  }

  bool is_full = false;
  int retry_cnt = 0;

  if (impl_->type == kNormalType) {
    do {
      {
        std::lock_guard lock(impl_->mtx);

        if VUNLIKELY (impl_->quit_flag) {
          return reject();
        }

        if VUNLIKELY (is_cancelled()) {
          return false;
        }

        is_full = impl_->normal_queue->size() >= get_max_task_count();

        if VLIKELY (!is_full) {
          push_normal_task(std::move(callback), droppable);
        } else if (overflow_policy == TaskOverflowPolicy::kReject) {
          return reject();
        } else if (impl_->strategy.load(std::memory_order_acquire) == kPopStrategy &&
                   overflow_policy != TaskOverflowPolicy::kBlock) {
          if (!drop_one_normal_task()) {
            return reject();
          }

          push_normal_task(std::move(callback), droppable);

          is_full = false;

          break;
        }
      }

      if VUNLIKELY (is_full) {
        if (impl_->strategy.load(std::memory_order_acquire) == kOptimizationStrategy &&
            overflow_policy != TaskOverflowPolicy::kBlock) {
          if (++retry_cnt > 10) {
            {
              std::lock_guard lock(impl_->mtx);

              if VUNLIKELY (impl_->quit_flag) {
                return reject();
              }

              if VUNLIKELY (is_cancelled()) {
                return false;
              }

              if (!drop_one_normal_task()) {
                return reject();
              }

              push_normal_task(std::move(callback), droppable);

              is_full = false;
            }

            CLOG_W("MessageLoop: Task is full, removed top data (%s).", impl_->name.c_str());
            break;
          }
        }

        if VUNLIKELY (is_cancelled()) {
          return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    } while (is_full);

    wakeup();

    return !is_full;

  } else if (impl_->type == kLockfreeType) {
    struct ProducerGuard final {
      explicit ProducerGuard(Impl& impl) noexcept : impl_ref(impl) {
        impl_ref.lockfree_producer_count.fetch_add(1U, std::memory_order_acq_rel);
      }

      ~ProducerGuard() {
        if (impl_ref.lockfree_producer_count.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
          std::lock_guard lock(impl_ref.mtx);
          impl_ref.cv.notify_all();
        }
      }

      Impl& impl_ref;
    } producer_guard(*impl_);

    auto push_reserved_lockfree_task = [this, &is_cancelled, &reject, &callback]() -> bool {
      if VUNLIKELY (impl_->quit_flag) {
        release_lockfree_task();
        return reject();
      }

      if VUNLIKELY (is_cancelled()) {
        release_lockfree_task();
        return false;
      }

      if VUNLIKELY (!push_lockfree_task(std::move(callback))) {
        release_lockfree_task();
        return reject();
      }

      return true;
    };

    do {
      if VUNLIKELY (impl_->quit_flag) {
        return reject();
      }

      if VUNLIKELY (is_cancelled()) {
        return false;
      }

      is_full = !reserve_lockfree_task();

      if VLIKELY (!is_full) {
        if VUNLIKELY (!push_reserved_lockfree_task()) {
          return false;
        }

        break;
      }

      if (overflow_policy == TaskOverflowPolicy::kReject) {
        return reject();
      }

      if (impl_->strategy.load(std::memory_order_acquire) == kPopStrategy &&
          overflow_policy != TaskOverflowPolicy::kBlock) {
        if (!drop_one_lockfree_task(true)) {
          return reject();
        }

        if VUNLIKELY (!push_reserved_lockfree_task()) {
          return false;
        }

        is_full = false;

        break;
      }

      if VUNLIKELY (is_full) {
        if (impl_->strategy.load(std::memory_order_acquire) == kOptimizationStrategy &&
            overflow_policy != TaskOverflowPolicy::kBlock) {
          if (++retry_cnt > 10) {
            if VUNLIKELY (impl_->quit_flag) {
              return reject();
            }

            if VUNLIKELY (is_cancelled()) {
              return false;
            }

            if (!drop_one_lockfree_task(true)) {
              return reject();
            }

            if VUNLIKELY (!push_reserved_lockfree_task()) {
              return false;
            }

            is_full = false;
            CLOG_W("MessageLoop: Task is full, removed top data (%s).", impl_->name.c_str());
            break;
          }
        }

        if VUNLIKELY (is_cancelled()) {
          return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    } while (is_full);

    wakeup();

    return !is_full;
  } else if (impl_->type == kPriorityType) {
    do {
      {
        std::lock_guard lock(impl_->mtx);

        if VUNLIKELY (impl_->quit_flag) {
          return reject();
        }

        if VUNLIKELY (is_cancelled()) {
          return false;
        }

        is_full =
            impl_->priority_droppable_queue->size() + impl_->priority_protected_queue->size() >= get_max_task_count();

        if VLIKELY (!is_full) {
          push_priority_task(std::move(callback), priority, droppable);
        } else if (overflow_policy == TaskOverflowPolicy::kReject) {
          return reject();
        } else if (impl_->strategy.load(std::memory_order_acquire) == kPopStrategy &&
                   overflow_policy != TaskOverflowPolicy::kBlock) {
          if (!drop_one_priority_task()) {
            return reject();
          }

          push_priority_task(std::move(callback), priority, droppable);

          is_full = false;

          break;
        }
      }

      if VUNLIKELY (is_full) {
        if (impl_->strategy.load(std::memory_order_acquire) == kOptimizationStrategy &&
            overflow_policy != TaskOverflowPolicy::kBlock) {
          if (++retry_cnt > 10) {
            {
              std::lock_guard lock(impl_->mtx);

              if VUNLIKELY (impl_->quit_flag) {
                return reject();
              }

              if VUNLIKELY (is_cancelled()) {
                return false;
              }

              if (!drop_one_priority_task()) {
                return reject();
              }

              push_priority_task(std::move(callback), priority, droppable);
              is_full = false;
            }

            CLOG_W("MessageLoop: Task is full, removed top data (%s).", impl_->name.c_str());
            break;
          }
        }

        if VUNLIKELY (is_cancelled()) {
          return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    } while (is_full);

    wakeup();

    return !is_full;
  }

  return reject();
}

void MessageLoop::push_normal_task(Callback&& callback, bool droppable) {
  uint32_t start_time = 0;

  if (get_max_elapsed_time() > 0) {
    start_time = get_current_time<std::chrono::steady_clock, std::chrono::milliseconds, uint32_t>();
  }

  impl_->normal_queue->emplace_back(start_time, droppable, std::move(callback));
}

bool MessageLoop::push_lockfree_task(Callback&& callback) {
  uint32_t start_time = 0;

  if (get_max_elapsed_time() > 0) {
    start_time = get_current_time<std::chrono::steady_clock, std::chrono::milliseconds, uint32_t>();
  }

  for (int retry = 0; retry < kMaxLockfreePushRetry; ++retry) {
    if VLIKELY (impl_->lockfree_queue->try_push<Impl::LockfreeQueue::kNoBehavior>(
                    std::forward_as_tuple(start_time, std::move(callback)))) {
      return true;
    }

    Utils::yield_cpu();
  }

  CLOG_E("MessageLoop: Failed to push lockfree task after %d retries (%s).", kMaxLockfreePushRetry,
         impl_->name.c_str());
  return false;
}

void MessageLoop::push_priority_task(Callback&& callback, uint16_t priority, bool droppable) {
  uint32_t start_time = 0;

  if (get_max_elapsed_time() > 0) {
    start_time = get_current_time<std::chrono::steady_clock, std::chrono::milliseconds, uint32_t>();
  }

  auto& queue = droppable ? impl_->priority_droppable_queue : impl_->priority_protected_queue;
  const uint16_t effective_priority = priority == kNoPriority ? static_cast<uint16_t>(kNormalPriority) : priority;
  queue->emplace(std::numeric_limits<uint16_t>::max() - effective_priority, impl_->task_seq, start_time, droppable,
                 std::move(callback));
  ++impl_->task_seq;
}

void MessageLoop::do_consume() {
  std::unique_lock lock(impl_->mtx);

  impl_->is_running = true;
  impl_->is_busy = true;
  impl_->thread_id = std::this_thread::get_id();

#ifdef _WIN32
  impl_->thread_handle.store(::OpenThread(THREAD_ALL_ACCESS, FALSE, ::GetCurrentThreadId()));
#endif

  lock.unlock();

  on_begin();

  if (impl_->type == kNormalType) {
    while (process_normal_task(true)) {
    }
  } else if (impl_->type == kLockfreeType) {
    while (process_lockfree_task(true)) {
    }
  } else if (impl_->type == kPriorityType) {
    while (process_priority_task(true)) {
    }
  }

  on_end();

  lock.lock();

  impl_->thread_id = std::thread::id();
  impl_->is_running = false;
  impl_->is_busy = false;

  lock.unlock();

  impl_->cv.notify_all();
}

bool MessageLoop::process_normal_task(bool block) {
  impl_->is_busy = true;

  [[maybe_unused]] bool is_timeout = true;
  int64_t sleep_time = -1;

#ifdef VLINK_ENABLE_BASE_MEMORY_RESOURCE
  Impl::NormalQueue temp_queue(&MemoryResource::global_instance());
#else
  Impl::NormalQueue temp_queue;
#endif

  std::unique_lock lock(impl_->mtx);

  impl_->task_seq = 0;
  temp_queue.swap(impl_->normal_queue.value());

  lock.unlock();

  while (!temp_queue.empty() && !impl_->force_quit_flag) {
    auto&& [start_time, droppable, task] = std::move(const_cast<Impl::NormalTaskTuple&>(temp_queue.front()));
    (void)droppable;
    on_task_changed(std::move(task), start_time);
    temp_queue.pop_front();
  }

  on_idle();

  lock.lock();

  if VUNLIKELY (impl_->quit_flag) {
    lock.unlock();
    drop_pending_tasks();
    return false;
  }

  if (!impl_->timer_set.empty()) {
    process_timer_task(sleep_time);
  }

  impl_->is_busy = false;
  impl_->cv.notify_all();

  if (block) {
    auto predicate = [this]() -> bool {
      return impl_->quit_flag || impl_->is_busy || !impl_->normal_queue->empty() || impl_->wakeup_pending;
    };

    if (sleep_time < 0) {
      impl_->cv.wait(lock, std::move(predicate));
      is_timeout = false;
    } else if (sleep_time > 0) {
      is_timeout = !impl_->cv.wait_for(lock, std::chrono::nanoseconds(sleep_time), std::move(predicate));
    }

    impl_->wakeup_pending = false;
  }

  return true;
}

bool MessageLoop::process_lockfree_task(bool block) {
  impl_->is_busy = true;

  [[maybe_unused]] bool is_timeout = true;

  while (!impl_->force_quit_flag) {
    Impl::LockfreeTaskTuple temp_task;

    if (!impl_->lockfree_queue->try_pop<Impl::LockfreeQueue::kNoBehavior>(temp_task)) {
      break;
    }

    release_lockfree_task();

    auto&& [start_time, task] = std::move(temp_task);

    on_task_changed(std::move(task), start_time);
  }

  on_idle();

  std::unique_lock lock(impl_->mtx);

  int64_t sleep_time = -1;

  if VUNLIKELY (impl_->quit_flag) {
    lock.unlock();
    drop_pending_tasks();
    return false;
  }

  if (!impl_->timer_set.empty()) {
    process_timer_task(sleep_time);
  }

  impl_->is_busy = false;
  impl_->cv.notify_all();

  if (block) {
    auto predicate = [this]() -> bool {
      return impl_->quit_flag || impl_->is_busy || impl_->lockfree_task_count.load(std::memory_order_acquire) != 0U ||
             impl_->wakeup_pending;
    };

    if (sleep_time < 0) {
      impl_->cv.wait(lock, std::move(predicate));
      is_timeout = false;
    } else if (sleep_time > 0) {
      is_timeout = !impl_->cv.wait_for(lock, std::chrono::nanoseconds(sleep_time), std::move(predicate));
    }

    impl_->wakeup_pending = false;
  }

  return true;
}

bool MessageLoop::process_priority_task(bool block) {
  impl_->is_busy = true;

  [[maybe_unused]] bool is_timeout = true;
  int64_t sleep_time = -1;

#ifdef VLINK_ENABLE_BASE_MEMORY_RESOURCE
  Impl::PriorityQueue temp_queue(&MemoryResource::global_instance());
  Impl::PriorityQueue temp_protected_queue(&MemoryResource::global_instance());
#else
  Impl::PriorityQueue temp_queue;
  Impl::PriorityQueue temp_protected_queue;
#endif

  std::unique_lock lock(impl_->mtx);

  impl_->task_seq = 0;
  temp_queue.swap(impl_->priority_droppable_queue.value());
  temp_protected_queue.swap(impl_->priority_protected_queue.value());

  lock.unlock();

  while ((!temp_queue.empty() || !temp_protected_queue.empty()) && !impl_->force_quit_flag) {
    const bool drain_protected =
        temp_queue.empty() ||
        (!temp_protected_queue.empty() && !Impl::priority_before(temp_queue.top(), temp_protected_queue.top()));
    auto& selected_queue = drain_protected ? temp_protected_queue : temp_queue;
    auto&& [priority, seq, start_time, droppable, task] =
        std::move(const_cast<Impl::PriorityTaskTuple&>(selected_queue.top()));
    (void)droppable;

    on_task_changed(std::move(task), start_time);

    selected_queue.pop();
  }

  on_idle();

  lock.lock();

  if VUNLIKELY (impl_->quit_flag) {
    lock.unlock();
    drop_pending_tasks();
    return false;
  }

  if (!impl_->timer_set.empty()) {
    process_timer_task(sleep_time);
  }

  impl_->is_busy = false;
  impl_->cv.notify_all();

  if (block) {
    auto predicate = [this]() -> bool {
      return impl_->quit_flag || impl_->is_busy || !impl_->priority_droppable_queue->empty() ||
             !impl_->priority_protected_queue->empty() || impl_->wakeup_pending;
    };

    if (sleep_time < 0) {
      impl_->cv.wait(lock, std::move(predicate));
      is_timeout = false;
    } else if (sleep_time > 0) {
      is_timeout = !impl_->cv.wait_for(lock, std::chrono::nanoseconds(sleep_time), std::move(predicate));
    }

    impl_->wakeup_pending = false;
  }

  return true;
}

bool MessageLoop::process_timer_task(int64_t& next_sleep_time) {
  int64_t invoke_count = 0;
  int64_t remain_loop_count = 0;
  int64_t interval_time = 0;
  int64_t remain_time = 0;
  uint64_t processed_invoke_count = 0;
  bool has_erase = false;
  bool has_processed = false;

  next_sleep_time = -1;

  for (auto iter = impl_->timer_set.begin(); iter != impl_->timer_set.end();) {
    Timer* timer = *iter;

    if (!timer->is_active()) {
      ++iter;
      continue;
    }

    if VUNLIKELY (!timer->has_callback()) {
      timer->stop();
      ++iter;
      continue;
    }

    interval_time =
        timer->get_interval() == 0 ? Timer::kMinInterval : static_cast<uint64_t>(timer->get_interval()) * 1000'000U;

    uint64_t start_time = timer->get_start_time();
    uint64_t current_time = get_current_nano_time();

    if VUNLIKELY (current_time < start_time) {
      remain_time = static_cast<int64_t>((start_time - current_time) + static_cast<uint64_t>(interval_time) -
                                         Timer::kMinInterval);

      if (remain_time < 0) {
        remain_time = 0;
      }

      if (next_sleep_time < 0 || next_sleep_time > remain_time) {
        next_sleep_time = remain_time;
      }

      ++iter;
      continue;
    }

    processed_invoke_count = timer->get_invoke_count();
    invoke_count = (current_time - start_time + Timer::kMinInterval) / interval_time;

    remain_loop_count = invoke_count - processed_invoke_count;

    if (remain_loop_count > 0) {
      has_erase = false;
      bool capacity_blocked = false;

      auto alive_flag = timer->get_alive_flag();
      auto run_timer_callback = [this, timer, alive_flag]() {
        if VUNLIKELY (!alive_flag->load()) {
          return;
        }

        {
          std::lock_guard timer_lock(impl_->mtx);

          if VUNLIKELY (!alive_flag->load() || impl_->timer_set.count(timer) == 0) {
            return;
          }

          timer->begin_in_flight();
        }

        timer->run_callback();
        timer->end_in_flight();
      };

      for (int64_t i = 0; timer->get_remain_loop_count() != 0 && i < remain_loop_count; ++i) {
        if (impl_->type == kNormalType) {
          if VUNLIKELY (impl_->normal_queue->size() >= get_max_task_count()) {
            if (!drop_one_normal_task()) {
              CLOG_W("MessageLoop: Timer task is full and no task can be dropped (%s).", impl_->name.c_str());
              capacity_blocked = true;
              break;
            }

            CLOG_W("MessageLoop: Timer task is full, removed top data (%s).", impl_->name.c_str());

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }

          if (timer->is_once_type()) {
            push_normal_task(timer->take_callback());
          } else {
            push_normal_task(run_timer_callback);
          }

          ++processed_invoke_count;
          has_processed = true;
        } else if (impl_->type == kLockfreeType) {
          if VUNLIKELY (!reserve_lockfree_task()) {
            if (!drop_one_lockfree_task(true)) {
              CLOG_W("MessageLoop: Timer task is full and no task can be dropped (%s).", impl_->name.c_str());
              capacity_blocked = true;
              break;
            }

            CLOG_W("MessageLoop: Timer task is full, removed top data (%s).", impl_->name.c_str());
          }

          bool pushed = false;

          if (timer->is_once_type()) {
            pushed = push_lockfree_task(timer->take_callback());
          } else {
            pushed = push_lockfree_task(run_timer_callback);
          }

          if VUNLIKELY (!pushed) {
            release_lockfree_task();
            capacity_blocked = true;
            break;
          }

          ++processed_invoke_count;
          has_processed = true;
        } else if (impl_->type == kPriorityType) {
          if VUNLIKELY (impl_->priority_droppable_queue->size() + impl_->priority_protected_queue->size() >=
                        get_max_task_count()) {
            if (!drop_one_priority_task()) {
              CLOG_W("MessageLoop: Timer task is full and no droppable task exists (%s).", impl_->name.c_str());
              capacity_blocked = true;
              break;
            }

            CLOG_W("MessageLoop: Timer task is full, removed top data (%s).", impl_->name.c_str());

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }

          if (timer->is_once_type()) {
            push_priority_task(timer->take_callback(), timer->get_priority());
          } else {
            push_priority_task(run_timer_callback, timer->get_priority());
          }

          ++processed_invoke_count;
          has_processed = true;
        }

        if (timer->is_once_type()) {
          iter = impl_->timer_set.erase(iter);
          timer->~Timer();
          MemoryPool::global_instance().deallocate(timer, sizeof(Timer), alignof(Timer));
          has_erase = true;
          break;
        }

        timer->sub_remain_loop_count();

        if (!timer->is_strict()) {
          break;
        }
      }

      if VUNLIKELY (has_erase) {
        continue;
      }

      if (timer->get_remain_loop_count() == 0) {
        timer->stop();
      } else {
        timer->set_invoke_count(capacity_blocked ? processed_invoke_count : static_cast<uint64_t>(invoke_count));
      }

      next_sleep_time = 0;

    } else {
      remain_time = interval_time - (current_time - start_time) % interval_time - Timer::kMinInterval;

      if (remain_time < 0) {
        remain_time = 0;
      }

      if (next_sleep_time < 0 || next_sleep_time > remain_time) {
        next_sleep_time = remain_time;
      }
    }

    ++iter;
  }

  return has_processed;
}

void MessageLoop::drop_pending_tasks() {
#ifdef VLINK_ENABLE_BASE_MEMORY_RESOURCE
  Impl::NormalQueue normal_queue(&MemoryResource::global_instance());
  Impl::PriorityQueue priority_droppable_queue(&MemoryResource::global_instance());
  Impl::PriorityQueue priority_protected_queue(&MemoryResource::global_instance());
#else
  Impl::NormalQueue normal_queue;
  Impl::PriorityQueue priority_droppable_queue;
  Impl::PriorityQueue priority_protected_queue;
#endif
  std::vector<Impl::LockfreeTaskTuple> lockfree_tasks;

  {
    std::lock_guard lock(impl_->mtx);

    if (impl_->type == kNormalType) {
      normal_queue.swap(impl_->normal_queue.value());
    } else if (impl_->type == kLockfreeType) {
      Impl::LockfreeTaskTuple task;
      while (impl_->lockfree_queue->try_pop<Impl::LockfreeQueue::kNoBehavior>(task)) {
        release_lockfree_task();
        lockfree_tasks.emplace_back(std::move(task));
      }
    } else if (impl_->type == kPriorityType) {
      priority_droppable_queue.swap(impl_->priority_droppable_queue.value());
      priority_protected_queue.swap(impl_->priority_protected_queue.value());
    }
  }
}

}  // namespace vlink
