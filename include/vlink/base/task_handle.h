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
 * @file task_handle.h
 * @brief Observable handle returned by tracked task-posting APIs of @c MessageLoop and @c ThreadPool.
 *
 * @details
 * @c vlink::TaskHandle is a value-typed observer backed by a refcounted shared state
 * block.  Copying or moving a handle is cheap and every copy refers to the same task.
 * Destroying every handle does @b not cancel the underlying task: the dispatcher retains
 * its own reference for the duration of the task lifecycle.
 *
 * Lifecycle of a tracked task:
 *
 * @verbatim
 *    created ---> kQueued -----> kRunning -----> kCompleted  (terminal)
 *      |             |             |          \-> kFailed    (terminal)
 *      |             v             v
 *      |          kCancelled    kCancelled (cancel observed)
 *      |          kDropped      (terminal)
 *      v
 *    kInvalid / kRejected (terminal)
 * @endverbatim
 *
 * Types defined here:
 *
 * | Type                  | Role                                                                  |
 * | --------------------- | --------------------------------------------------------------------- |
 * | @c TaskExecutionState | Lifecycle stage of a tracked task (queued / running / terminal).      |
 * | @c TaskOverflowPolicy | Per-post override for bounded-queue overflow behaviour.               |
 * | @c TaskDropPolicy     | Whether an accepted task may be discarded by drop-oldest paths.       |
 * | @c PostTaskOptions    | Aggregate of optional knobs for tracked @c post_task overloads.       |
 * | @c TaskHandle         | Observer object handed to the caller.                                 |
 *
 * @par Lock ordering
 * @c TaskHandle's internal state mutex is the innermost mutex in the dispatching layer.
 * The expected order is
 * @c MessageLoop::AliveState::mtx -> @c MessageLoop::Impl::mtx -> @c TaskHandle::State::mtx.
 * @c TaskHandle::cancel() acquires the cancellation source's internal mutex only after
 * releasing the handle mutex, so the cancellation source is never nested inside any
 * dispatching-layer mutex.  Callbacks added through @c cancellation_token() fire outside
 * @c TaskHandle::State::mtx, so they may safely re-enter the handle.
 *
 * @par Example
 * @code
 * vlink::PostTaskOptions opts;
 * opts.cancellation_token = parent.token();
 * auto handle = loop.post_task_handle([] { work(); }, opts);
 *
 * if (giving_up) {
 *   handle.cancel();
 * }
 *
 * handle.wait();
 *
 * if (handle.state() == vlink::TaskExecutionState::kFailed) {
 *   inspect_logs();
 * }
 * @endcode
 *
 * @note Destroying a @c TaskHandle does not cancel the task; pair with @c cancel() when
 *       cancellation is required.
 */

#pragma once

#include <cstdint>
#include <memory>

#include "./cancellation.h"
#include "./functional.h"
#include "./macros.h"

namespace vlink {

class MessageLoop;
class ThreadPool;

/**
 * @enum TaskExecutionState
 * @brief Observable lifecycle stage of a tracked task.
 *
 * @details
 * Non-terminal states (@c kInvalid, @c kQueued, @c kRunning) may transition further;
 * terminal states (@c kCompleted, @c kCancelled, @c kDropped, @c kRejected, @c kFailed)
 * are stable and unblock all waiters on @c TaskHandle::wait().
 */
enum class TaskExecutionState : uint8_t {
  kInvalid = 0,  ///< Non-terminal: empty handle or no associated task yet.
  kQueued,       ///< Non-terminal: dispatcher accepted the task but has not yet run it.
  kRunning,      ///< Non-terminal: task callback is currently executing.
  kCompleted,    ///< Terminal: callback returned normally.
  kCancelled,    ///< Terminal: task was cancelled before its callback executed.
  kDropped,      ///< Terminal: task was removed from a bounded queue before execution.
  kRejected,     ///< Terminal: dispatcher refused the submission (quitting / queue full).
  kFailed,       ///< Terminal: task callback raised an exception.
};

/**
 * @enum TaskOverflowPolicy
 * @brief Per-post override for the bounded-queue overflow strategy.
 *
 * @details
 * @c kUseDispatcherStrategy inherits the dispatcher's configured behaviour; the other
 * values temporarily force @c kReject or @c kBlock for a single submission.
 */
enum class TaskOverflowPolicy : uint8_t {
  kUseDispatcherStrategy = 0,  ///< Inherit the dispatcher-configured overflow strategy.
  kReject,                     ///< Return a rejected handle immediately when the queue is full.
  kBlock,                      ///< Block until queue space appears or the dispatcher quits.
};

/**
 * @enum TaskDropPolicy
 * @brief Whether an accepted task may later be discarded by drop-oldest overflow paths.
 *
 * @details
 * Even after admission, drop-oldest dispatchers may evict older tasks to make room for
 * newer work.  Mark a task @c kProtected to keep it out of that selection.
 */
enum class TaskDropPolicy : uint8_t {
  kDroppable = 0,  ///< The task is a valid drop candidate.
  kProtected,      ///< The task must not be dropped after admission.  (Lock-free dispatchers do not honour this; see
                   ///< @c MessageLoop::post_task documentation.)
};

/**
 * @struct PostTaskOptions
 * @brief Bundle of optional knobs accepted by tracked task-posting APIs.
 *
 * @details
 * Every field has a safe default so a default-constructed @c PostTaskOptions matches the
 * dispatcher's behaviour for an unprotected, non-cancellable task.
 */
struct VLINK_EXPORT PostTaskOptions final {
  /**
   * @brief Parent cancellation token that can request abort of this submission.
   *
   * @details
   * An empty token (the default) means only the handle's own
   * @c cancellation_token() may request cancellation.  When the supplied parent is
   * already cancelled at submission time the returned handle is immediately marked
   * @c TaskExecutionState::kCancelled.  Cancellation observed while the task is queued
   * causes the task to be skipped on dequeue.
   */
  CancellationToken cancellation_token;

  /**
   * @brief Queue-full strategy applied to this single submission.
   */
  TaskOverflowPolicy overflow_policy{TaskOverflowPolicy::kUseDispatcherStrategy};

  /**
   * @brief Whether the dispatcher is permitted to drop this task to make room for newer work.
   */
  TaskDropPolicy drop_policy{TaskDropPolicy::kDroppable};
};

/**
 * @class TaskHandle
 * @brief Shared observable handle returned by tracked posting APIs.
 *
 * @details
 * Backed by a @c std::shared_ptr to an internal state block, so copies and moves all
 * point at the same task.  Provides cooperative cancellation, blocking wait on the
 * terminal state, and a task-local @c CancellationToken that running callbacks can poll.
 */
class VLINK_EXPORT TaskHandle final {
 public:
  /**
   * @brief Sentinel timeout value for @c wait() meaning wait indefinitely.
   */
  static constexpr int kInfinite{-1};

  /**
   * @brief Constructs an invalid handle that references no task.
   *
   * @details
   * @c valid() returns @c false and @c state() returns @c TaskExecutionState::kInvalid.
   */
  TaskHandle() noexcept;

  /**
   * @brief Destroys the handle reference.
   *
   * @note Destruction does not cancel the associated task; the dispatcher retains its own
   *       reference to the state block.
   */
  ~TaskHandle();

  /**
   * @brief Copy-constructs a handle that shares state with @p other.
   */
  TaskHandle(const TaskHandle&) noexcept;

  /**
   * @brief Copy-assigns from another handle; both handles share state afterwards.
   *
   * @return Reference to @c *this.
   */
  TaskHandle& operator=(const TaskHandle&) noexcept;

  /**
   * @brief Move-constructs a handle, transferring the shared state from the source.
   */
  TaskHandle(TaskHandle&&) noexcept;

  /**
   * @brief Move-assigns from another handle, transferring its shared state.
   *
   * @return Reference to @c *this.
   */
  TaskHandle& operator=(TaskHandle&&) noexcept;

  /**
   * @brief Reports whether this handle refers to a live task state block.
   *
   * @return @c false when default-constructed or moved-from.
   */
  [[nodiscard]] bool valid() const noexcept;

  /**
   * @brief Returns the current lifecycle stage of the task.
   *
   * @return Latest @c TaskExecutionState; @c kInvalid when the handle is not valid.
   */
  [[nodiscard]] TaskExecutionState state() const noexcept;

  /**
   * @brief Reports whether the task has reached a terminal state.
   *
   * @return @c true once @c state() is one of @c kCompleted, @c kCancelled, @c kDropped,
   *         @c kRejected or @c kFailed.
   */
  [[nodiscard]] bool is_done() const noexcept;

  /**
   * @brief Returns the task-local cancellation token associated with this handle.
   *
   * @details
   * Running callbacks may poll this token for cooperative abort.  Callbacks registered
   * on the token fire outside the handle mutex, so they may safely re-enter the handle.
   *
   * @return Associated token; an empty token when the handle is not valid.
   */
  [[nodiscard]] CancellationToken cancellation_token() const noexcept;

  /**
   * @brief Requests cooperative cancellation of the task.
   *
   * @details
   * No-op when the handle is invalid or already terminal.  When the task has not started
   * running (@c kInvalid or @c kQueued) this call transitions the state to @c kCancelled;
   * in any non-terminal case it also flips the cancellation source so running callbacks
   * polling the token observe the request.
   *
   * @return @c true when this call either transitioned the state or flipped the source;
   *         @c false when the handle was invalid or already terminal.
   */
  bool cancel() const;

  /**
   * @brief Blocks until the task reaches a terminal state or the timeout elapses.
   *
   * @details
   * Never throws on cancellation; inspect @c state() after the call to distinguish the
   * possible outcomes.
   *
   * @param timeout_ms  Maximum wait in milliseconds; @c kInfinite waits forever.
   * @return @c true when a terminal state was reached before the deadline.
   */
  bool wait(int timeout_ms = kInfinite) const;

 private:
  struct State;

  explicit TaskHandle(std::shared_ptr<State> state) noexcept;

  static TaskHandle make_task_handle(const CancellationToken& parent_token = {});

  static MoveFunction<void()> make_tracked_task(TaskHandle handle, MoveFunction<void()>&& callback);

  static void mark_task_queued(const TaskHandle& handle);

  static void mark_task_rejected(const TaskHandle& handle);

  static bool begin_task_execution(const TaskHandle& handle);

  static void complete_task_execution(const TaskHandle& handle);

  static void fail_task_execution(const TaskHandle& handle);

  static void drop_task_if_queued(const TaskHandle& handle);

  static bool request_cancel(std::shared_ptr<State> state);

  std::shared_ptr<State> state_;

  friend class MessageLoop;
  friend class ThreadPool;
  friend class TrackedTask;
};

}  // namespace vlink
