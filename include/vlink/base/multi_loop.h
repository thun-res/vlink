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
 * @file multi_loop.h
 * @brief Multi-threaded variant of @c MessageLoop that forwards tasks to an internal @c ThreadPool.
 *
 * @details
 * @c MultiLoop reuses every @c MessageLoop posting API but offloads execution to a pool of
 * worker threads.  The dispatcher thread still dequeues tasks; instead of running each task
 * inline it hands them to the worker pool through the @c on_task_changed hook.  Callers post
 * via @c post_task / @c exec_task exactly as with a single-threaded loop.
 *
 * @par Balancing strategies (delegated to the underlying @c ThreadPool)
 *
 * | Strategy        | Behaviour                                                            |
 * | --------------- | -------------------------------------------------------------------- |
 * | Round-robin     | Dispatcher hands the next task to the next available worker          |
 * | Least-loaded    | Dispatcher picks the worker with the smallest in-flight count        |
 * | Hash            | Dispatcher pins tasks by hash key (used by ordered subscribers)      |
 *
 * @par Worker diagram
 *
 * @verbatim
 *             post_task / exec_task
 *                       |
 *                       v
 *      +---------------------------------+
 *      |       MessageLoop queue         |
 *      +----------------+----------------+
 *                       |
 *                       v
 *      +---------------------------------+
 *      |       dispatcher thread         |
 *      +-------+--------+--------+-------+
 *              |        |        |
 *              v        v        v
 *        +-------+  +-------+  +-------+
 *        | Wkr 1 |  | Wkr 2 |  | Wkr N |
 *        +-------+  +-------+  +-------+
 * @endverbatim
 *
 * @par Differences vs @c MessageLoop
 *  - Tasks may run concurrently on multiple worker threads; execution order is unspecified.
 *  - @c is_in_same_thread returns @c true for the dispatcher and every worker thread.
 *  - @c on_begin / @c on_end are still invoked once on the dispatcher; they construct and tear
 *    down the internal pool.
 *  - When the pool rejects a forwarded task (for example a zero-worker pool) the base
 *    @c MessageLoop::on_task_changed runs the callback inline on the dispatcher.
 *
 * @par Example
 * @code
 *   vlink::MultiLoop loop(4);
 *   loop.async_run();
 *
 *   for (int i = 0; i < 100; ++i) {
 *     loop.post_task([i] { process(i); });
 *   }
 *
 *   loop.wait_for_idle();
 *   loop.quit();
 *   loop.wait_for_quit();
 * @endcode
 *
 * @note Shared state touched inside callbacks must be protected externally.  Timers attached to
 *       a @c MultiLoop fire as queue tasks; the dispatcher forwards them to a worker.  The
 *       destructor is defaulted; always call @c quit and @c wait_for_quit before destruction.
 */

#pragma once

#include <memory>

#include "./message_loop.h"

namespace vlink {

/**
 * @class MultiLoop
 * @brief @c MessageLoop subclass that forwards every task to an internal @c ThreadPool.
 *
 * @details
 * Inherits every posting API from @c MessageLoop.  The dispatcher pulls tasks from the queue in
 * FIFO order; the worker pool runs them concurrently so execution order is not guaranteed even
 * though enqueue order is.
 */
class VLINK_EXPORT MultiLoop : public MessageLoop {
 public:
  /**
   * @brief Constructs a @c MultiLoop with the default @c kNormalType queue and @p thread_num workers.
   *
   * @param thread_num  Worker count.  Default: @c 4.  A zero-worker pool rejects forwarded posts,
   *                    so tasks fall back to inline execution on the dispatcher.
   */
  explicit MultiLoop(size_t thread_num = 4U);

  /**
   * @brief Constructs a @c MultiLoop with a specific queue type.
   *
   * @param thread_num  Worker count.
   * @param type        Queue implementation type from @c MessageLoop::Type.
   */
  explicit MultiLoop(size_t thread_num, Type type);

  /**
   * @brief Defaulted destructor.
   *
   * @warning Call @c quit and @c wait_for_quit before destruction; the base
   *          @c MessageLoop destructor runs after @c MultiLoop's members are already torn down
   *          and cannot guarantee the worker pool is reset.
   */
  ~MultiLoop() override;

  /**
   * @brief Reports whether the calling thread belongs to this loop.
   *
   * @return @c true on the dispatcher or any worker thread of the internal pool.
   */
  [[nodiscard]] bool is_in_same_thread() const override;

  /**
   * @brief Waits until both the dispatcher queue and the worker pool reach idle.
   *
   * @param ms     Maximum wait in milliseconds; @c Timer::kInfinite means no limit.
   * @param check  When @c true, rejects calls from threads owned by this loop.
   * @return @c true when both queues drained before the timeout.
   */
  bool wait_for_idle(int ms = Timer::kInfinite, bool check = true) override;

 protected:
  /**
   * @brief Hook invoked once on the dispatcher when the loop starts; constructs the worker pool.
   */
  void on_begin() override;

  /**
   * @brief Hook invoked once on the dispatcher when the loop exits; shuts the worker pool down.
   */
  void on_end() override;

  /**
   * @brief Forwards a task to the internal pool, falling back to the inline base implementation
   *        when the pool rejects the post.
   *
   * @param callback    Task to dispatch.
   * @param start_time  Millisecond @c steady_clock timestamp captured at enqueue time.
   */
  void on_task_changed(Callback&& callback, uint32_t start_time) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(MultiLoop)
};

}  // namespace vlink
