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

#include "./base/multi_loop.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <utility>

#include "./base/condition_variable.h"
#include "./base/logger.h"
#include "./base/memory_resource.h"
#include "./base/thread_pool.h"
#include "./base/utils.h"

namespace vlink {

// MultiLoopGlobal
struct MultiLoopGlobal final {
  std::atomic<int> instance_index{0};

  static MultiLoopGlobal& get() {
    static MultiLoopGlobal instance;

    return instance;
  }

 private:
  MultiLoopGlobal() = default;
};

// MultiLoop::Impl
struct MultiLoop::Impl final {
  struct SharedState final {
    mutable std::mutex pool_mtx;
    ConditionVariable idle_cv;
    std::atomic_size_t pending_tasks{0U};
    std::atomic_bool alive{true};
    bool shutdown_by_destructor{false};
    std::shared_ptr<ThreadPool> thread_pool;
  };

  std::shared_ptr<SharedState> state{MemoryResource::make_shared<SharedState>()};
  size_t thread_num{0};
};

struct MultiLoop::PendingTask final {
  MultiLoop* loop;
  std::shared_ptr<MultiLoop::Impl::SharedState> state;
  MultiLoop::Callback callback;
  uint32_t start_time;

  PendingTask(MultiLoop* target_loop, std::shared_ptr<MultiLoop::Impl::SharedState> target_state,
              MultiLoop::Callback&& target_callback, uint32_t target_start_time) noexcept
      : loop(target_loop),
        state(std::move(target_state)),
        callback(std::move(target_callback)),
        start_time(target_start_time) {
    state->pending_tasks.fetch_add(1U, std::memory_order_acq_rel);
  }

  PendingTask(const PendingTask&) = delete;

  PendingTask& operator=(const PendingTask&) = delete;

  ~PendingTask() {
    callback = nullptr;

    if VLIKELY (state->pending_tasks.fetch_sub(1U, std::memory_order_acq_rel) != 1U) {
      return;
    }

    {
      std::lock_guard lock(state->pool_mtx);
    }

    state->idle_cv.notify_all();
  }
};

// MultiLoop
MultiLoop::MultiLoop(size_t thread_num) : impl_(std::make_unique<Impl>()) {
  impl_->thread_num = thread_num;

  set_name("MultiLoop_" +
           std::to_string(MultiLoopGlobal::get().instance_index.fetch_add(1, std::memory_order_relaxed)));
}

MultiLoop::MultiLoop(size_t thread_num, Type type) : MessageLoop(type), impl_(std::make_unique<Impl>()) {
  impl_->thread_num = thread_num;

  set_name("MultiLoop_" +
           std::to_string(MultiLoopGlobal::get().instance_index.fetch_add(1, std::memory_order_relaxed)));

  // if VUNLIKELY (type == MultiLoop::kPriorityType) {
  //   CLOG_F("MultiLoop not support priority type(%s).", get_name().c_str());
  // }
}

MultiLoop::~MultiLoop() {
#ifdef _WIN32
  if VUNLIKELY (Utils::is_terminating()) {
    return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }
#endif

  const bool in_dispatcher_thread = MessageLoop::is_in_same_thread();
  const bool in_worker_thread = !in_dispatcher_thread && MultiLoop::is_in_same_thread();
  std::shared_ptr<ThreadPool> detached_pool;

  if VUNLIKELY (in_worker_thread) {
    {
      std::lock_guard lock(impl_->state->pool_mtx);
      detached_pool = impl_->state->thread_pool;
      impl_->state->shutdown_by_destructor = true;
    }

    impl_->state->idle_cv.notify_all();
  }

  quit(!is_running());

  if VUNLIKELY (detached_pool) {
    detached_pool->shutdown();
  }

  if VLIKELY (!in_dispatcher_thread) {
    wait_for_quit(Timer::kInfinite, false);
  }

  std::shared_ptr<ThreadPool> leftover_pool;

  {
    std::lock_guard lock(impl_->state->pool_mtx);
    leftover_pool = std::move(impl_->state->thread_pool);
  }

  leftover_pool.reset();

  impl_->state->alive.store(false, std::memory_order_release);

  {
    std::lock_guard lock(impl_->state->pool_mtx);
  }

  impl_->state->idle_cv.notify_all();

  if VLIKELY (!in_dispatcher_thread) {
    auto alive_state = get_alive_state();
    std::lock_guard lock(alive_state->mtx);
    alive_state->alive.store(false, std::memory_order_release);
  }
}

bool MultiLoop::is_in_same_thread() const {
  if (MessageLoop::is_in_same_thread()) {
    return true;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  std::shared_ptr<ThreadPool> thread_pool;

  {
    std::lock_guard lock(impl_->state->pool_mtx);
    thread_pool = impl_->state->thread_pool;
  }

  if VUNLIKELY (!thread_pool) {
    return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  return thread_pool->is_in_work_thread();
}

bool MultiLoop::wait_for_idle(int ms, bool check) {
  const auto start_time = std::chrono::steady_clock::now();
  auto state = impl_->state;

  auto idle_predicate = [&state]() -> bool {
    return !state->alive.load(std::memory_order_acquire) || state->pending_tasks.load(std::memory_order_acquire) == 0U;
  };

  auto remaining_ms = [ms, start_time]() -> int {
    if (ms == Timer::kInfinite) {
      return Timer::kInfinite;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();

    if (elapsed >= ms) {
      return 0;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    return static_cast<int>(ms - elapsed);
  };

  bool first_check = check;

  while (true) {
    const int dispatcher_wait_ms = remaining_ms();

    if (!MessageLoop::wait_for_idle(dispatcher_wait_ms, first_check)) {
      return false;
    }

    first_check = false;

    std::unique_lock lock(state->pool_mtx);

    if (ms == Timer::kInfinite) {
      state->idle_cv.wait(lock, idle_predicate);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    } else {
      const int worker_wait_ms = remaining_ms();

      if (worker_wait_ms <= 0) {
        const bool worker_idle = idle_predicate();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        lock.unlock();                              // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        return worker_idle && (!state->alive.load(std::memory_order_acquire) ||
                               MessageLoop::wait_for_idle(0, false));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      if (!state->idle_cv.wait_for(lock, std::chrono::milliseconds(worker_wait_ms),
                                   idle_predicate)) {  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        return false;                                  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }
    }

    lock.unlock();

    if (!state->alive.load(std::memory_order_acquire) || MessageLoop::wait_for_idle(0, false)) {
      return true;
    }
  }
}

void MultiLoop::on_begin() {
  std::shared_ptr<ThreadPool> thread_pool;

  if (get_type() == MultiLoop::kNormalType) {
    thread_pool = MemoryResource::make_shared<ThreadPool>(impl_->thread_num, ThreadPool::kNormalType);
  } else if (get_type() == MultiLoop::kLockfreeType) {
    thread_pool = MemoryResource::make_shared<ThreadPool>(impl_->thread_num, ThreadPool::kLockfreeType);
  } else if (get_type() == MultiLoop::kPriorityType) {
    thread_pool = MemoryResource::make_shared<ThreadPool>(impl_->thread_num, ThreadPool::kNormalType);
  }

  {
    std::lock_guard lock(impl_->state->pool_mtx);
    impl_->state->shutdown_by_destructor = false;
    impl_->state->thread_pool = std::move(thread_pool);
  }

  MessageLoop::on_begin();
}

void MultiLoop::on_end() {
  std::shared_ptr<ThreadPool> thread_pool;
  const auto state = impl_->state;

  {
    std::unique_lock lock(state->pool_mtx);
    state->idle_cv.wait(lock, [&state] {
      return state->shutdown_by_destructor || state->pending_tasks.load(std::memory_order_acquire) == 0U;
    });

    if VLIKELY (!state->shutdown_by_destructor) {
      thread_pool = std::move(state->thread_pool);
    }
  }

  if (thread_pool) {
    thread_pool->shutdown();
  }

  MessageLoop::on_end();
}

void MultiLoop::on_task_changed(Callback&& callback, uint32_t start_time) {
  std::shared_ptr<ThreadPool> thread_pool;

  {
    std::lock_guard lock(impl_->state->pool_mtx);

    if VLIKELY (!impl_->state->shutdown_by_destructor) {
      thread_pool = impl_->state->thread_pool;
    }
  }

  std::shared_ptr<PendingTask> task;
  Callback dropped_task;
  bool posted = false;

  if VLIKELY (thread_pool) {
    task = MemoryResource::make_shared<PendingTask>(this, impl_->state, std::move(callback), start_time);

    posted = thread_pool->push_task(
        [task]() mutable {  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          if VLIKELY (task->callback && task->state->alive.load(std::memory_order_acquire)) {
            task->loop->MessageLoop::on_task_changed(std::move(task->callback), task->start_time);
          }
        },
        is_current_task_droppable(), TaskOverflowPolicy::kUseDispatcherStrategy, nullptr, &dropped_task);
  }

  if VUNLIKELY (!posted) {
    if (task) {
      if (task->callback) {
        MessageLoop::on_task_changed(std::move(task->callback), task->start_time);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }
    } else if (callback) {
      MessageLoop::on_task_changed(std::move(callback), start_time);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
  }
}

}  // namespace vlink
