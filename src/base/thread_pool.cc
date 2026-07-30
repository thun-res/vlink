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

#include "./base/thread_pool.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "./base/condition_variable.h"
#include "./base/logger.h"
#include "./base/memory_pool.h"
#include "./base/memory_resource.h"
#include "./base/mpmc_queue.h"
#include "./base/utils.h"

namespace vlink {

static constexpr size_t kMaxTaskSize = 10000U;
static constexpr int kMaxLockfreePushRetry = 32;

// ThreadPoolGlobal
struct ThreadPoolGlobal final {
  std::atomic<int> instance_index{0};

  static ThreadPoolGlobal& get() {
    static ThreadPoolGlobal instance;

    return instance;
  }

 private:
  ThreadPoolGlobal() = default;
};

// ThreadPool::Impl
struct ThreadPool::Impl final {  // NOLINT(clang-analyzer-optin.performance.Padding)
  static thread_local const Impl* current_thread_pool_impl_;

#ifdef VLINK_ENABLE_BASE_MEMORY_RESOURCE
  using NormalTaskTuple = std::tuple<bool, ThreadPool::Callback>;
  using LockfreeTaskTuple = std::tuple<ThreadPool::Callback>;
  using NormalQueue = std::pmr::deque<NormalTaskTuple>;
  using LockfreeQueue = MpmcQueue<LockfreeTaskTuple>;
#else
  using NormalTaskTuple = std::tuple<bool, ThreadPool::Callback>;
  using LockfreeTaskTuple = std::tuple<ThreadPool::Callback>;
  using NormalQueue = std::deque<NormalTaskTuple>;
  using LockfreeQueue = MpmcQueue<LockfreeTaskTuple>;
#endif

  std::atomic_bool quit_flag{false};
  alignas(64) std::atomic_size_t lockfree_task_count{0U};
  alignas(64) std::atomic_size_t lockfree_producer_count{0U};

  std::string name;
  size_t thread_count{0};
  ThreadPool::Type type{ThreadPool::kNormalType};
  std::atomic<ThreadPool::Strategy> strategy{ThreadPool::kOptimizationStrategy};
  std::vector<std::thread> threads;

  std::optional<NormalQueue> normal_queue;
  std::optional<LockfreeQueue> lockfree_queue;

  ConditionVariable cv;
  std::mutex mtx;
};

thread_local const ThreadPool::Impl* ThreadPool::Impl::current_thread_pool_impl_ = nullptr;

// ThreadPool
ThreadPool::ThreadPool(size_t thread_count) : impl_(MemoryResource::make_shared<Impl>()) {
  impl_->name =
      "ThreadPool_" + std::to_string(ThreadPoolGlobal::get().instance_index.fetch_add(1, std::memory_order_relaxed));
  impl_->thread_count = thread_count;

  MemoryPool::global_instance();

  init();
}

ThreadPool::ThreadPool(size_t thread_count, Type type) : impl_(MemoryResource::make_shared<Impl>()) {
  impl_->name =
      "ThreadPool_" + std::to_string(ThreadPoolGlobal::get().instance_index.fetch_add(1, std::memory_order_relaxed));
  impl_->thread_count = thread_count;
  impl_->type = type;

  MemoryPool::global_instance();

  init();
}

ThreadPool::~ThreadPool() { shutdown(); }

void ThreadPool::set_name(const std::string& name) { impl_->name = name; }

const std::string& ThreadPool::get_name() const { return impl_->name; }

ThreadPool::Type ThreadPool::get_type() const { return impl_->type; }

ThreadPool::Strategy ThreadPool::get_strategy() const { return impl_->strategy.load(std::memory_order_acquire); }

void ThreadPool::set_strategy(Strategy strategy) { impl_->strategy.store(strategy, std::memory_order_release); }

bool ThreadPool::shutdown() {
  {
    std::lock_guard lock(impl_->mtx);

    if VUNLIKELY (impl_->quit_flag.load(std::memory_order_acquire)) {
      return false;
    }

    impl_->quit_flag.store(true, std::memory_order_release);
  }

  if (impl_->type == kLockfreeType) {
    std::unique_lock lock(impl_->mtx);
    impl_->cv.wait(lock, [this] { return impl_->lockfree_producer_count.load(std::memory_order_acquire) == 0U; });
  }

  impl_->cv.notify_all();

  const auto self_id = std::this_thread::get_id();

  for (auto& thread : impl_->threads) {
    if (thread.joinable()) {
      if (thread.get_id() == self_id) {
        thread.detach();
      } else {
        thread.join();
      }
    }
  }

  return true;
}

bool ThreadPool::post_task(Callback&& callback) {
  return push_task(std::move(callback), true, TaskOverflowPolicy::kUseDispatcherStrategy);
}

TaskHandle ThreadPool::post_task_handle(Callback&& callback, const PostTaskOptions& options) {
  auto handle = TaskHandle::make_task_handle(options.cancellation_token);

  if (handle.state() == TaskExecutionState::kCancelled) {
    return handle;
  }

  if VUNLIKELY (impl_->type == kLockfreeType && options.drop_policy == TaskDropPolicy::kProtected) {
    CLOG_W("ThreadPool: TaskDropPolicy::kProtected is ignored by lock-free queues (%s).",  // LCOV_EXCL_LINE
                                                                                           // GCOVR_EXCL_LINE
           impl_->name.c_str());  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  auto tracked = TaskHandle::make_tracked_task(handle, std::move(callback));
  const bool droppable = options.drop_policy == TaskDropPolicy::kDroppable;

  if VUNLIKELY (!push_task(std::move(tracked), droppable, options.overflow_policy, &handle) && !handle.is_done()) {
    TaskHandle::mark_task_rejected(handle);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  return handle;
}

size_t ThreadPool::get_task_count() const {
  if (impl_->type == kNormalType) {
    std::lock_guard lock(impl_->mtx);
    return impl_->normal_queue->size();
  } else if (impl_->type == kLockfreeType) {
    return impl_->lockfree_task_count.load(std::memory_order_acquire);
  } else {
    return 0U;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }
}

bool ThreadPool::is_in_work_thread() const { return Impl::current_thread_pool_impl_ == impl_.get(); }

size_t ThreadPool::get_max_task_count() const { return kMaxTaskSize; }

void ThreadPool::init() {
  if (impl_->type == kNormalType) {
    impl_->normal_queue.emplace();
  } else if (impl_->type == kLockfreeType) {
    const auto max_task_count = get_max_task_count();  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
    impl_->lockfree_queue.emplace(max_task_count);
    impl_->lockfree_task_count.store(0U, std::memory_order_release);
  }

  if VUNLIKELY (impl_->thread_count == 0) {
    VLOG_E("ThreadPool: Thread count is zero.");
    impl_->quit_flag.store(true, std::memory_order_release);
    return;
  }

  impl_->threads.reserve(impl_->thread_count);

  for (size_t i = 0; i < impl_->thread_count; ++i) {
    if (impl_->type == kNormalType) {
      auto impl = impl_;
      std::thread thread([impl] {
        Impl::current_thread_pool_impl_ = impl.get();

        for (;;) {
          Callback task;

          {
            std::unique_lock lock(impl->mtx);
            impl->cv.wait(lock, [impl] {
              return !impl->normal_queue->empty() || impl->quit_flag.load(std::memory_order_acquire);
            });

            if VUNLIKELY (impl->normal_queue->empty() && impl->quit_flag.load(std::memory_order_acquire)) {
              break;
            }

            task = std::move(std::get<1>(impl->normal_queue->front()));

            impl->normal_queue->pop_front();
          }

          if VLIKELY (task) {
            task();
          }
        }

        Impl::current_thread_pool_impl_ = nullptr;
      });

      impl_->threads.emplace_back(std::move(thread));
    } else if (impl_->type == kLockfreeType) {
      auto impl = impl_;
      std::thread thread([impl] {
        Impl::current_thread_pool_impl_ = impl.get();

        for (;;) {
          Impl::LockfreeTaskTuple task_tuple;

          const bool has_task = impl->lockfree_queue->try_pop(task_tuple);

          if (!has_task) {
            if VUNLIKELY (impl->quit_flag.load(std::memory_order_acquire) &&
                          impl->lockfree_task_count.load(std::memory_order_acquire) == 0U) {
              if (impl->lockfree_producer_count.load(std::memory_order_acquire) ==
                  0U) {  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                break;
              }

              std::this_thread::yield();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
              continue;
            }

            if (impl->lockfree_task_count.load(std::memory_order_acquire) != 0U) {
              std::this_thread::yield();
              continue;
            }

            std::unique_lock lock(impl->mtx);
            impl->cv.wait(lock, [impl] {
              return impl->lockfree_task_count.load(std::memory_order_acquire) != 0U ||
                     impl->quit_flag.load(std::memory_order_acquire);
            });

            continue;
          }

          impl->lockfree_task_count.fetch_sub(1U, std::memory_order_acq_rel);

          auto& task = std::get<0>(task_tuple);

          if VLIKELY (task) {
            task();
          }
        }

        Impl::current_thread_pool_impl_ = nullptr;
      });

      impl_->threads.emplace_back(std::move(thread));
    }
  }
}

bool ThreadPool::push_task(Callback&& callback, bool droppable, TaskOverflowPolicy overflow_policy,
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

  if VUNLIKELY (impl_->quit_flag.load(std::memory_order_acquire)) {
    return reject();
  }

  bool is_full = false;
  int retry_cnt = 0;

  if (impl_->type == kNormalType) {
    do {
      {
        std::lock_guard lock(impl_->mtx);

        if VUNLIKELY (impl_->quit_flag.load(std::memory_order_acquire)) {
          return reject();
        }

        if VUNLIKELY (is_cancelled()) {
          return false;
        }

        is_full = impl_->normal_queue->size() >= get_max_task_count();

        if VLIKELY (!is_full) {
          impl_->normal_queue->emplace_back(droppable, std::move(callback));
        } else if (overflow_policy == TaskOverflowPolicy::kReject) {
          return reject();
        } else if (impl_->strategy.load(std::memory_order_acquire) == kPopStrategy &&
                   overflow_policy != TaskOverflowPolicy::kBlock) {
          if (!drop_one_normal_task()) {
            return reject();
          }

          impl_->normal_queue->emplace_back(droppable, std::move(callback));
          is_full = false;

          break;
        }
      }

      if VUNLIKELY (is_full) {
        if (impl_->strategy.load(std::memory_order_acquire) == kOptimizationStrategy &&
            overflow_policy != TaskOverflowPolicy::kBlock) {
          // LCOV_EXCL_START GCOVR_EXCL_START
          if (++retry_cnt > 10) {
            {
              std::lock_guard lock(impl_->mtx);

              if VUNLIKELY (impl_->quit_flag.load(std::memory_order_acquire)) {
                return reject();
              }

              if VUNLIKELY (is_cancelled()) {
                return false;
              }

              if (!drop_one_normal_task()) {
                return reject();
              }

              impl_->normal_queue->emplace_back(droppable, std::move(callback));
              is_full = false;
            }

            CLOG_W("ThreadPool: Task is full, removed top data (%s).", impl_->name.c_str());
            break;
          }
          // LCOV_EXCL_STOP GCOVR_EXCL_STOP
        }

        if VUNLIKELY (is_cancelled()) {
          return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    } while (is_full);
  } else if (impl_->type == kLockfreeType) {
    bool notify_waiter = false;

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

    auto push_reserved_lockfree_task = [this, &reject, &is_cancelled, &callback]() -> bool {
      if VUNLIKELY (impl_->quit_flag.load(std::memory_order_acquire)) {
        release_lockfree_task();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        return reject();          // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      if VUNLIKELY (is_cancelled()) {
        release_lockfree_task();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        return false;             // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      if VUNLIKELY (!push_lockfree_task(std::move(callback))) {
        release_lockfree_task();
        return reject();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      return true;
    };

    do {
      if VUNLIKELY (impl_->quit_flag.load(std::memory_order_acquire)) {
        return reject();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      if VUNLIKELY (is_cancelled()) {
        return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      is_full = !reserve_lockfree_task(&notify_waiter);

      if VLIKELY (!is_full) {
        if VUNLIKELY (!push_reserved_lockfree_task()) {
          return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }

        break;
      }

      if (overflow_policy == TaskOverflowPolicy::kReject) {
        return reject();
      }

      if (impl_->strategy.load(std::memory_order_acquire) == kPopStrategy &&
          overflow_policy != TaskOverflowPolicy::kBlock) {
        if (!drop_one_lockfree_task(true)) {
          return reject();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }

        if VUNLIKELY (!push_reserved_lockfree_task()) {
          return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }

        is_full = false;
        break;
      }

      if VUNLIKELY (is_full) {
        if (impl_->strategy.load(std::memory_order_acquire) == kOptimizationStrategy &&
            overflow_policy != TaskOverflowPolicy::kBlock) {
          // LCOV_EXCL_START GCOVR_EXCL_START
          if (++retry_cnt > 10) {
            if VUNLIKELY (impl_->quit_flag.load(std::memory_order_acquire)) {
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
            CLOG_W("ThreadPool: Task is full, removed top data (%s).", impl_->name.c_str());
            break;
          }
          // LCOV_EXCL_STOP GCOVR_EXCL_STOP
        }

        if VUNLIKELY (is_cancelled()) {
          return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    } while (is_full);

    if (notify_waiter) {
      // Pair the empty-to-non-empty transition with the wait mutex so workers cannot miss the notify.
      {
        std::lock_guard lock(impl_->mtx);
      }
      impl_->cv.notify_one();
    }
  }

  if (impl_->type != kLockfreeType) {
    impl_->cv.notify_one();
  }

  return !is_full;
}

bool ThreadPool::drop_one_normal_task() {
  for (auto iter = impl_->normal_queue->begin(); iter != impl_->normal_queue->end(); ++iter) {
    if (std::get<0>(*iter)) {
      impl_->normal_queue->erase(iter);
      return true;
    }
  }

  return false;
}

bool ThreadPool::drop_one_lockfree_task(bool keep_reserved) {
  Impl::LockfreeTaskTuple task;

  if (!impl_->lockfree_queue->try_pop(task)) {
    return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if (!keep_reserved) {
    release_lockfree_task();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  return true;
}

bool ThreadPool::reserve_lockfree_task(bool* was_empty) {
  auto count = impl_->lockfree_task_count.load(std::memory_order_acquire);
  const auto max_count = get_max_task_count();

  while (count < max_count) {
    if (impl_->lockfree_task_count.compare_exchange_weak(count, count + 1U, std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
      if (was_empty != nullptr) {
        *was_empty = count == 0U;
      }

      return true;
    }
  }

  return false;
}

void ThreadPool::release_lockfree_task() {
  impl_->lockfree_task_count.fetch_sub(1U, std::memory_order_acq_rel);
}  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

bool ThreadPool::push_lockfree_task(Callback&& callback) {
  for (int retry = 0; retry < kMaxLockfreePushRetry; ++retry) {
    if VLIKELY (impl_->lockfree_queue->try_push(std::forward_as_tuple(std::move(callback)))) {
      return true;
    }

    // LCOV_EXCL_START GCOVR_EXCL_START
    Utils::yield_cpu();
  }

  CLOG_E("ThreadPool: Failed to push lockfree task after %d retries (%s).", kMaxLockfreePushRetry, impl_->name.c_str());

  return false;
  // LCOV_EXCL_STOP GCOVR_EXCL_STOP
}

}  // namespace vlink
