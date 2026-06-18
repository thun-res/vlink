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

/**
 * @file thread_pool.h
 * @brief Fixed-size worker pool for parallel task execution.
 *
 * @details
 * @c vlink::ThreadPool launches a configurable number of worker threads at construction
 * and dispatches submitted tasks to whichever worker is idle.  Unlike @c MessageLoop, the
 * pool has no built-in timer mechanism or loop lifecycle; it is started immediately and
 * torn down with @c shutdown().  A pool created with zero workers is left in the
 * shutdown state and rejects every submission.
 *
 * Architecture:
 *
 * @verbatim
 *     post_task() ----->  +---------------------------+
 *                         | shared dispatcher queue   |
 *                         +-----+----------+----------+
 *                               |          |          |
 *                               v          v          v
 *                            worker 1   worker 2 ... worker N
 *                               |          |          |
 *                               v          v          v
 *                            user task  user task  user task
 * @endverbatim
 *
 * Queue implementations:
 *
 * | Type              | Backing queue                                    | Notes                           |
 * | ----------------- | ------------------------------------------------ | ------------------------------- |
 * | @c kNormalType    | Mutex-protected @c std::deque (or @c std::pmr)   | Default; honours drop policy    |
 * | @c kLockfreeType  | @c MpmcQueue (lock-free multi-producer/consumer) | Lower contention overhead       |
 *
 * Push-side back-pressure strategies are identical to @c MessageLoop::Strategy.  They
 * only affect submission when the queue is full; worker wake-up is always driven by a
 * condition variable.
 *
 * @note
 * - Tasks may run concurrently; protect shared state externally.
 * - @c invoke_task() returns a @c std::future; blocking on it from a pool worker can
 *   deadlock if every worker is busy.
 * - @c is_in_work_thread() can detect re-entrant submissions.
 *
 * @par Example
 * @code
 * vlink::ThreadPool pool(8);
 * pool.post_task([] { heavy_work(); });
 *
 * auto fut = pool.invoke_task([]() -> int { return compute(); });
 * int result = fut.get();
 *
 * pool.shutdown();
 * @endcode
 */

#pragma once

#include <functional>
#include <future>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "./functional.h"
#include "./macros.h"
#include "./memory_resource.h"
#include "./task_handle.h"

namespace vlink {

/**
 * @class ThreadPool
 * @brief Fixed worker pool that consumes a shared task queue.
 *
 * @details
 * Worker threads are created during construction and joined during @c shutdown() or
 * destruction.  The pool object itself is non-copyable.
 */
class VLINK_EXPORT ThreadPool {
 public:
  /**
   * @brief Callback signature used for submitted tasks.
   *
   * @details
   * Move-only (@c MoveFunction<void()>); see @c MessageLoop::Callback for rationale.
   */
  using Callback = MoveFunction<void()>;

  /**
   * @enum Type
   * @brief Queue implementation backing the pool.
   */
  enum Type : uint8_t {
    kNormalType = 0,    ///< Default mutex-protected FIFO queue.
    kLockfreeType = 1,  ///< Lock-free MPMC queue.
  };

  /**
   * @enum Strategy
   * @brief Submission-side strategy applied when the bounded queue is full.
   *
   * @details
   * Worker wake-up is independent of this enum.  It controls only how @c post_task and
   * @c invoke_task react when capacity is reached.
   */
  enum Strategy : uint8_t {
    kOptimizationStrategy = 0,  ///< Retry up to 10 times with 1 ms sleep; then drop one eligible task and push.
    kPopStrategy = 1,           ///< Immediately drop one eligible task and push the new one.
    kBlockStrategy = 2,         ///< Retry indefinitely with 1 ms sleep until capacity frees up.
  };

  /**
   * @brief Constructs a pool with @p thread_count workers and the default @c kNormalType queue.
   *
   * @param thread_count  Worker thread count.  Default: @c 4.  Zero leaves the pool in
   *                      the shutdown state.
   */
  explicit ThreadPool(size_t thread_count = 4U);

  /**
   * @brief Constructs a pool with a custom queue implementation.
   *
   * @param thread_count  Worker thread count.  Zero leaves the pool in the shutdown state.
   * @param type          Queue implementation type.
   */
  explicit ThreadPool(size_t thread_count, Type type);

  /**
   * @brief Destructor.  Calls @c shutdown() and joins worker threads.
   */
  virtual ~ThreadPool();

  /**
   * @brief Assigns a human-readable name to the pool and its workers (used by debuggers).
   *
   * @param name  Display name.
   */
  void set_name(const std::string& name);

  /**
   * @brief Returns the display name assigned via @c set_name().
   *
   * @return Reference to the stored name string.
   */
  [[nodiscard]] const std::string& get_name() const;

  /**
   * @brief Returns the queue implementation used by this pool.
   *
   * @return Queue type.
   */
  [[nodiscard]] Type get_type() const;

  /**
   * @brief Returns the current submission-side back-pressure strategy.
   *
   * @return Current strategy.
   */
  [[nodiscard]] Strategy get_strategy() const;

  /**
   * @brief Updates the submission-side back-pressure strategy.
   *
   * @param strategy  New strategy.
   */
  void set_strategy(Strategy strategy);

  /**
   * @brief Marks the pool as quitting, drains in-flight tasks, and joins workers.
   *
   * @details
   * After @c shutdown() returns, further submissions are rejected.  Workers complete the
   * task they are currently running plus any already-queued tasks before exiting.  When
   * called from a worker thread the calling worker's handle is detached instead of
   * joined because a thread cannot join itself; the @c Impl block is kept alive via
   * @c std::shared_ptr so the detached worker continues to see a valid pool state until
   * it returns.
   *
   * @return @c true on the first successful shutdown; @c false on subsequent calls.
   */
  bool shutdown();

  /**
   * @brief Submits a task to the queue for execution by a worker thread.
   *
   * @details
   * Thread-safe.  Returns @c false when the pool is already shut down or when overflow
   * handling cannot make room for the new task.  Overflow behaviour depends on the
   * configured @c Strategy:
   *
   *  - @c kOptimizationStrategy: retry up to 10 times with a 1 ms sleep, then drop one
   *    eligible task and push the new one.
   *  - @c kPopStrategy: drop one eligible task immediately and push the new one.
   *  - @c kBlockStrategy: retry indefinitely with 1 ms sleep until space is available.
   *
   * @note Drop-policy semantics:
   *  - @c kNormalType respects @c TaskDropPolicy::kProtected; protected tasks are never
   *    selected as eviction victims, and if every queued task is protected the post
   *    fails and returns @c false.
   *  - @c kLockfreeType does not track per-task drop policy; overflow drop simply removes
   *    one queued task regardless of how it was submitted.
   *
   * @param callback  Task to execute.
   * @return @c true when the task was eventually enqueued.
   */
  bool post_task(Callback&& callback);

  /**
   * @brief Submits a task that produces an observable @c TaskHandle.
   *
   * @details
   * Tracked counterpart of @c post_task().  The returned handle allows callers to wait
   * for completion, request cooperative cancellation, and observe whether the task was
   * rejected or dropped before execution.
   *
   * @param callback  Task to execute.
   * @param options   Optional overflow, drop, and cancellation policy.
   * @return Handle observing the posted task; the handle remains valid even when the
   *         post is rejected so callers can inspect @c state().
   */
  [[nodiscard]] TaskHandle post_task_handle(Callback&& callback, const PostTaskOptions& options = {});

  /**
   * @brief Returns the current number of tasks waiting in the queue.
   *
   * @return Pending task count.
   */
  [[nodiscard]] size_t get_task_count() const;

  /**
   * @brief Reports whether the calling thread is one of this pool's workers.
   *
   * @return @c true when called from a worker.
   */
  [[nodiscard]] bool is_in_work_thread() const;

  /**
   * @brief Returns the maximum queue capacity.
   *
   * @return Maximum number of tasks that may be queued at the same time.
   */
  [[nodiscard]] virtual size_t get_max_task_count() const;

  /**
   * @brief Submits a callable to a worker thread and returns a @c std::future for the result.
   *
   * @details
   * Thread-safe.  The future is satisfied once the callable returns.  When posting fails
   * the future becomes ready with a @c broken_promise / @c future_error result.
   *
   * @warning Do not block on the returned future from a pool worker thread while all
   *          workers are busy; doing so deadlocks the pool.
   *
   * @tparam FunctionT  Callable type.
   * @tparam ArgsT      Argument types forwarded to the callable.
   * @tparam ResultT    Deduced result type.
   * @param function  Callable to dispatch.
   * @param args      Arguments to forward.
   * @return Future resolved with the callable's result.
   */
  template <class FunctionT, class... ArgsT, typename ResultT = std::invoke_result_t<FunctionT, ArgsT...>>
  [[nodiscard]] std::future<ResultT> invoke_task(FunctionT&& function, ArgsT&&... args);

 private:
  void init();

  bool push_task(Callback&& callback, bool droppable,
                 TaskOverflowPolicy overflow_policy = TaskOverflowPolicy::kUseDispatcherStrategy,
                 const TaskHandle* submit_handle = nullptr);

  bool drop_one_normal_task();

  bool drop_one_lockfree_task(bool keep_reserved = false);

  bool reserve_lockfree_task(bool* was_empty = nullptr);

  void release_lockfree_task();

  bool push_lockfree_task(Callback&& callback);

  struct Impl;
  std::shared_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(ThreadPool)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

template <class FunctionT, class... ArgsT, typename ResultT>
inline std::future<ResultT> ThreadPool::invoke_task(FunctionT&& function, ArgsT&&... args) {
  auto bound =
      std::bind(std::forward<FunctionT>(function), std::forward<ArgsT>(args)...);  // NOLINT(modernize-avoid-bind)

  if constexpr (kIsSupportMoveFunction) {
    std::packaged_task<ResultT()> task(std::move(bound));
    auto res = task.get_future();

    if VUNLIKELY (!post_task([task = std::move(task)]() mutable { task(); })) {
      // Destroying the unposted packaged_task makes the returned future ready with broken_promise.
    }

    return res;
  } else {
    auto task = MemoryResource::make_shared<std::packaged_task<ResultT()>>(std::move(bound));
    auto res = task->get_future();

    if VUNLIKELY (!post_task([task]() mutable { (*task)(); })) {
      task.reset();
    }

    return res;
  }
}

}  // namespace vlink
