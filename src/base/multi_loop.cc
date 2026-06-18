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
#include <optional>
#include <utility>

#include "./base/condition_variable.h"
#include "./base/logger.h"
#include "./base/thread_pool.h"

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
  mutable std::mutex pool_mtx;
  mutable std::mutex idle_mtx;
  ConditionVariable idle_cv;
  std::atomic_size_t pending_tasks{0U};
  std::optional<ThreadPool> thread_pool;
  size_t thread_num{0};
};

// MultiLoop
MultiLoop::MultiLoop(size_t thread_num) : impl_(std::make_unique<Impl>()) {
  impl_->thread_num = thread_num;

  set_name("MultiLoop_" + std::to_string(MultiLoopGlobal::get().instance_index++));
}

MultiLoop::MultiLoop(size_t thread_num, Type type) : MessageLoop(type), impl_(std::make_unique<Impl>()) {
  impl_->thread_num = thread_num;

  set_name("MultiLoop_" + std::to_string(MultiLoopGlobal::get().instance_index++));

  // if VUNLIKELY (type == MultiLoop::kPriorityType) {
  //   CLOG_F("MultiLoop not support priority type(%s).", get_name().c_str());
  // }
}

MultiLoop::~MultiLoop() = default;

bool MultiLoop::is_in_same_thread() const {
  if (MessageLoop::is_in_same_thread()) {
    return true;
  }

  std::lock_guard lock(impl_->pool_mtx);

  if VUNLIKELY (!impl_->thread_pool) {
    return false;
  }

  return impl_->thread_pool->is_in_work_thread();
}

bool MultiLoop::wait_for_idle(int ms, bool check) {
  const auto start_time = std::chrono::steady_clock::now();

  auto idle_predicate = [this]() -> bool { return impl_->pending_tasks.load(std::memory_order_acquire) == 0U; };

  auto remaining_ms = [ms, start_time]() -> int {
    if (ms == Timer::kInfinite) {
      return Timer::kInfinite;
    }

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();

    if (elapsed >= ms) {
      return 0;
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

    std::unique_lock lock(impl_->idle_mtx);

    if (ms == Timer::kInfinite) {
      impl_->idle_cv.wait(lock, idle_predicate);
    } else {
      const int worker_wait_ms = remaining_ms();

      if (worker_wait_ms <= 0) {
        return idle_predicate() && MessageLoop::wait_for_idle(0, false);
      }

      if (!impl_->idle_cv.wait_for(lock, std::chrono::milliseconds(worker_wait_ms), idle_predicate)) {
        return false;
      }
    }

    lock.unlock();

    if (MessageLoop::wait_for_idle(0, false)) {
      return true;
    }
  }
}

void MultiLoop::on_begin() {
  {
    std::lock_guard lock(impl_->pool_mtx);

    if (get_type() == MultiLoop::kNormalType) {
      impl_->thread_pool.emplace(impl_->thread_num, ThreadPool::kNormalType);
    } else if (get_type() == MultiLoop::kLockfreeType) {
      impl_->thread_pool.emplace(impl_->thread_num, ThreadPool::kLockfreeType);
    } else if (get_type() == MultiLoop::kPriorityType) {
      impl_->thread_pool.emplace(impl_->thread_num, ThreadPool::kNormalType);
    }
  }

  MessageLoop::on_begin();
}

void MultiLoop::on_end() {
  {
    std::lock_guard lock(impl_->pool_mtx);

    if (impl_->thread_pool) {
      impl_->thread_pool->shutdown();
      impl_->thread_pool.reset();
    }
  }

  MessageLoop::on_end();
}

void MultiLoop::on_task_changed(Callback&& callback, uint32_t start_time) {
  auto task = std::make_shared<Callback>(std::move(callback));

  bool posted = false;

  {
    std::lock_guard lock(impl_->pool_mtx);

    if VLIKELY (impl_->thread_pool) {
      impl_->pending_tasks.fetch_add(1U, std::memory_order_acq_rel);

      std::shared_ptr<Impl> pending_token(impl_.get(), [](Impl* impl) noexcept {
        bool should_notify = false;

        {
          std::lock_guard<std::mutex> lock(impl->idle_mtx);
          should_notify = impl->pending_tasks.fetch_sub(1U, std::memory_order_acq_rel) == 1U;
        }

        if (should_notify) {
          impl->idle_cv.notify_all();
        }
      });

      posted =
          impl_->thread_pool->post_task([this, task, pending_token = std::move(pending_token), start_time]() mutable {
            if VLIKELY (*task) {
              MessageLoop::on_task_changed(std::move(*task), start_time);
            }

            (void)pending_token;
          });
    }
  }

  if VUNLIKELY (!posted && *task) {
    MessageLoop::on_task_changed(std::move(*task), start_time);
  }
}

}  // namespace vlink
