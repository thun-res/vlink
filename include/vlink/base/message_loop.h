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
 * @file message_loop.h
 * @brief Single-threaded task dispatcher with three queue backends, timers and scheduling envelopes.
 *
 * @details
 * @c MessageLoop is VLink's serial task executor.  It owns a bounded task queue and the timer
 * registry attached to it; tasks posted from any thread execute in order on the loop's own
 * thread.  Three queue implementations are available; the choice is fixed at construction.
 *
 * @par Dispatch mode table
 *
 * | Queue type        | Backend                              | Capacity | Properties                  |
 * | ----------------- | ------------------------------------ | -------- | --------------------------- |
 * | @c kNormalType    | @c mutex @c + @c std::deque (or pmr) |   10000  | FIFO, lock-based (default)  |
 * | @c kLockfreeType  | @c MpmcQueue<Callback>               |   10000  | Lock-free MPMC              |
 * | @c kPriorityType  | Two pmr priority queues              |   10000  | Droppable / protected split |
 *
 * Back-pressure on a full queue is selected by @c Strategy: @c kOptimizationStrategy retries
 * up to ten times with 1 ms sleeps before dropping an eligible task, @c kPopStrategy drops
 * immediately, @c kBlockStrategy retries indefinitely.  Idle dispatch is always condition-variable
 * driven and independent of @c Strategy.
 *
 * @par Lifecycle diagram
 *
 * @verbatim
 *                          run() / async_run()
 *   +--------+                        |
 *   |  idle  | ----- async_run -----> | spawn thread
 *   +--------+                        |
 *      ^                              v
 *      |                          +--------+
 *      |   wait_for_quit          | active |  <-- post_task / exec_task
 *      |       (joined)           +--------+
 *      |                              |
 *      |                       quit() | quit(true)
 *      |                              v
 *      |                          +--------+
 *      +------- on_end() -------- | drain  | -- pending tasks --> dropped
 *                                 +--------+
 * @endverbatim
 *
 * @par Priority levels
 *
 * | Symbol                  | Value | Notes                          |
 * | ----------------------- | ----- | ------------------------------ |
 * | @c kNoPriority          | 0     | FIFO sentinel                  |
 * | @c kLowestPriority      | 1     | Lowest real priority           |
 * | @c kTimerPriority       | 50    | Reserved for timer callbacks   |
 * | @c kNormalPriority      | 100   | Default user-task priority     |
 * | @c kHighestPriority     | 65535 | Highest priority               |
 *
 * @par Example
 * @code
 *   vlink::MessageLoop loop;
 *   loop.async_run();
 *
 *   loop.post_task([] { do_work(); });
 *   loop.post_task_with_priority([] { urgent(); }, vlink::MessageLoop::kHighestPriority);
 *
 *   auto fut = loop.invoke_task([]() -> int { return compute(); });
 *   int result = fut.get();   // wait from a different thread
 *
 *   loop.quit();
 *   loop.wait_for_quit();
 * @endcode
 *
 * @note Maximum queue depth is @c 10000 (@c kMaxTaskSize) and maximum timer count is @c 100
 *       (@c kMaxTimerSize).  Blocking @c .get() on the future returned by @c invoke_task from
 *       inside the loop thread deadlocks; always call it from another thread.
 */

#pragma once

#include <atomic>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>

#include "./functional.h"
#include "./memory_resource.h"
#include "./schedule.h"
#include "./task_handle.h"
#include "./timer.h"

namespace vlink {

/**
 * @class MessageLoop
 * @brief Serial task dispatcher with selectable queue backend and bounded timer registry.
 *
 * @details
 * Owns a thread that pulls tasks from a bounded queue and fires timer callbacks.  Other threads
 * may post via @c post_task, @c post_task_with_priority, @c invoke_task or @c exec_task from
 * any context.  The class is the building block of @c MultiLoop and the engine target of
 * @c GraphTask::execute.
 */
class VLINK_EXPORT MessageLoop {
 public:
  /**
   * @brief Callback type for tasks and event handlers.
   *
   * @details
   * Move-only so move-only targets such as @c std::packaged_task or lambdas holding
   * @c std::unique_ptr can be posted directly without a shared trampoline.
   */
  using Callback = MoveFunction<void()>;

  /**
   * @brief Internal queue implementation type.
   */
  enum Type : uint8_t {
    kNormalType = 0,    ///< Mutex-protected FIFO queue (default).
    kLockfreeType = 1,  ///< Lock-free MPMC queue.
    kPriorityType = 2,  ///< Priority-ordered queue with droppable / protected split.
  };

  /**
   * @brief Back-pressure strategy applied when the bounded queue is at capacity.
   *
   * @details
   * Idle dispatch is unconditionally cv-driven; this enum only affects the push side.
   */
  enum Strategy : uint8_t {
    kOptimizationStrategy = 0,  ///< Retry up to ten times, then drop one eligible task and push.
    kPopStrategy = 1,           ///< Drop one eligible task immediately and push.
    kBlockStrategy = 2,         ///< Retry indefinitely until space appears.
  };

  /**
   * @brief Built-in priority levels for @c kPriorityType loops; higher values dispatch first.
   */
  enum Priority : uint16_t {
    kNoPriority = 0,                                         ///< FIFO sentinel.
    kLowestPriority = 1,                                     ///< Lowest real priority.
    kTimerPriority = 50,                                     ///< Reserved for timer callbacks.
    kNormalPriority = 100,                                   ///< Default user-task priority.
    kHighestPriority = std::numeric_limits<uint16_t>::max()  ///< Highest priority.
  };

  /**
   * @brief Lifetime gate shared with cross-thread observers.
   *
   * @details
   * The destructor of @c MessageLoop flips @c alive to @c false under @c mtx as its very first
   * step, so a caller that holds @c mtx and observes @c alive @c == @c true is guaranteed the
   * loop is still safe to touch.  Obtain via @c get_alive_state.
   */
  struct AliveState final {
    std::mutex mtx;
    std::atomic_bool alive{true};
  };

  /**
   * @brief Constructs a loop with the default @c kNormalType queue.
   */
  MessageLoop();

  /**
   * @brief Constructs a loop with the given queue type.
   *
   * @param type  Queue implementation.
   */
  explicit MessageLoop(Type type);

  /**
   * @brief Destructor; requests quit and joins the dispatcher thread if needed.
   */
  virtual ~MessageLoop();

  /**
   * @brief Sets a human-readable name visible to profiling tools.
   *
   * @param name  Loop name.
   */
  void set_name(const std::string& name);

  /**
   * @brief Returns the loop name set via @c set_name.
   *
   * @return Reference to the stored name.
   */
  [[nodiscard]] const std::string& get_name() const;

  /**
   * @brief Returns the queue type this loop was constructed with.
   *
   * @return Queue type.
   */
  [[nodiscard]] Type get_type() const;

  /**
   * @brief Returns the active back-pressure strategy.
   *
   * @return Current strategy.
   */
  [[nodiscard]] Strategy get_strategy() const;

  /**
   * @brief Replaces the back-pressure strategy.
   *
   * @param strategy  New strategy; takes effect on the next full-queue push.
   */
  void set_strategy(Strategy strategy);

  /**
   * @brief Registers a callback fired once at loop thread startup.
   *
   * @details
   * Must be registered before @c run / @c async_run; later calls are ignored.
   *
   * @param callback  Startup handler.
   */
  void register_begin_handler(Callback&& callback);

  /**
   * @brief Registers a callback fired once when the loop thread exits.
   *
   * @details
   * Must be registered before @c run / @c async_run; later calls are ignored.
   *
   * @param callback  Shutdown handler.
   */
  void register_end_handler(Callback&& callback);

  /**
   * @brief Registers a callback fired every time the queue becomes empty.
   *
   * @details
   * Must be registered before @c run / @c async_run; later calls are ignored.
   *
   * @param callback  Idle handler.
   */
  void register_idle_handler(Callback&& callback);

  /**
   * @brief Runs the loop on the calling thread until @c quit is requested.
   *
   * @return @c true after a normal exit; @c false when the loop is already running.
   */
  bool run();

  /**
   * @brief Starts the loop on a new background thread.
   *
   * @return @c true when the thread was started; @c false when the loop is already running.
   */
  bool async_run();

  /**
   * @brief Alias of @c run blocking the calling thread.
   *
   * @return Same return value as @c run.
   */
  bool spin();

  /**
   * @brief Processes one batch of pending tasks and timers on the calling thread.
   *
   * @param block  When @c true (default) blocks if the queue is empty; when @c false returns immediately.
   * @return @c true after a normal processing cycle; @c false when quitting or the call came from a
   *         thread that does not own the loop and is unrelated to @c run / @c async_run.
   */
  bool spin_once(bool block = true);

  /**
   * @brief Requests the loop to exit.
   *
   * @details
   * With @p force @c == @c false the current batch finishes and the loop exits afterwards.  With
   * @p force @c == @c true the in-flight batch is also aborted.  Tasks queued after the request
   * are rejected.  Returns @c false when @c quit had already been called (only meaningful when
   * @p force is @c false).
   *
   * @param force  When @c true, also discards the in-flight batch.  Default: @c false.
   * @return @c true when the quit signal was accepted.
   */
  bool quit(bool force = false);

  /**
   * @brief Waits until the loop has fully exited.
   *
   * @param ms     Maximum wait in milliseconds; @c Timer::kInfinite means no limit.
   * @param check  When @c true (default) rejects calls from the loop's own thread.
   * @return @c true if the loop exited before the timeout.
   */
  bool wait_for_quit(int ms = Timer::kInfinite, bool check = true);

  /**
   * @brief Posts a task for execution on the loop thread.
   *
   * @details
   * Thread-safe.  Returns @c false when the loop is quitting or the configured @c Strategy
   * cannot make room for the new task.
   *
   * @param callback  Task to post.
   * @return @c true when the task was enqueued.
   */
  bool post_task(Callback&& callback);

  /**
   * @brief Tracked variant of @c post_task returning a @c TaskHandle.
   *
   * @param callback  Task to post.
   * @param options   Optional overflow / drop / cancellation policy.
   * @return Handle bound to the posted task; valid even when posting was rejected.
   */
  [[nodiscard]] TaskHandle post_task_handle(Callback&& callback, const PostTaskOptions& options = {});

  /**
   * @brief Posts a task with an explicit priority on a @c kPriorityType loop.
   *
   * @details
   * Higher @p priority values dispatch first.  Non-priority loops reject the call and return
   * @c false.
   *
   * @param callback  Task to post.
   * @param priority  Dispatch priority in @c [kLowestPriority, @c kHighestPriority].
   * @return @c true when the task was enqueued.
   */
  bool post_task_with_priority(Callback&& callback, uint16_t priority);

  /**
   * @brief Tracked variant of @c post_task_with_priority returning a @c TaskHandle.
   *
   * @param callback  Task to post.
   * @param priority  Dispatch priority.
   * @param options   Optional overflow / drop / cancellation policy.
   * @return Handle bound to the posted task.
   */
  [[nodiscard]] TaskHandle post_task_with_priority_handle(Callback&& callback, uint16_t priority,
                                                          const PostTaskOptions& options = {});

  /**
   * @brief Posts a scheduled @c void-returning callable and returns a chainable @c Schedule::Status.
   *
   * @details
   * @c Schedule::Config carries the delay, priority and timeout fields.  The returned status
   * supports @c on_then / @c on_else / @c on_catch / @c on_schedule_timeout chaining.
   *
   * @tparam CallbackT  Callable returning @c void.
   * @param config    Scheduling envelope.
   * @param callback  Callable to schedule.
   * @return Chainable status object.
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename CallbackT, typename = std::enable_if_t<!std::is_convertible_v<CallbackT, Schedule::RetCallback>>>
  Schedule::Status exec_task(const Schedule::Config& config, CallbackT&& callback);

  /**
   * @brief Posts a scheduled @c bool-returning callable and returns a chainable @c Schedule::RetStatus.
   *
   * @details
   * @c on_then fires when the callable returns @c true; @c on_else fires when it returns @c false.
   *
   * @tparam CallbackT  Callable returning @c bool.
   * @param config    Scheduling envelope.
   * @param callback  Callable to schedule.
   * @return Chainable status object.
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename CallbackT, typename = std::enable_if_t<std::is_convertible_v<CallbackT, Schedule::RetCallback>>>
  Schedule::RetStatus exec_task(const Schedule::Config& config, CallbackT&& callback);

  /**
   * @brief Wakes the loop thread if it is suspended in its idle wait.
   *
   * @details
   * Concurrent calls coalesce; only the first one after the previous wakeup actually signals.
   *
   * @return @c true when a wakeup is pending (newly issued or already in flight).
   */
  bool wakeup();

  /**
   * @brief Recreates the lock-free queue, clearing all queued tasks and counters.
   *
   * @details
   * Applies only to @c kLockfreeType loops; on other types the call is a no-op.  Must be invoked
   * while the loop is stopped; calls made while it is running are logged and skipped.
   */
  void reset_lockfree_capacity();

  /**
   * @brief Reports whether the loop is currently running.
   *
   * @return @c true between a successful start and the corresponding @c quit.
   */
  [[nodiscard]] bool is_running() const;

  /**
   * @brief Reports whether @c quit has been requested and the loop is winding down.
   *
   * @return @c true once @c quit has been observed.
   */
  [[nodiscard]] bool is_ready_to_quit() const;

  /**
   * @brief Reports whether the loop is currently executing a task.
   *
   * @return @c true while a callback is on the call stack inside the loop.
   */
  [[nodiscard]] bool is_busy() const;

  /**
   * @brief Returns the current pending task count.
   *
   * @return Number of tasks waiting in the queue.
   */
  [[nodiscard]] size_t get_task_count() const;

  /**
   * @brief Waits until the loop has drained its queue and is not executing a task.
   *
   * @param ms     Maximum wait in milliseconds; @c Timer::kInfinite means no limit.
   * @param check  When @c true (default) rejects calls from the loop's own thread.
   * @return @c true when the idle condition was reached within @p ms.
   */
  virtual bool wait_for_idle(int ms = Timer::kInfinite, bool check = true);

  /**
   * @brief Returns the maximum queue depth.
   *
   * @return @c kMaxTaskSize (@c 10000) by default.
   */
  [[nodiscard]] virtual size_t get_max_task_count() const;

  /**
   * @brief Returns the maximum number of timers that can be attached.
   *
   * @return @c kMaxTimerSize (@c 100) by default.
   */
  [[nodiscard]] virtual size_t get_max_timer_count() const;

  /**
   * @brief Returns the maximum allowed task execution time in milliseconds.
   *
   * @details
   * Tasks exceeding this duration trigger @c on_task_timeout.  Zero disables the check.
   *
   * @return Maximum execution time in milliseconds.
   */
  [[nodiscard]] virtual uint32_t get_max_elapsed_time() const;

  /**
   * @brief Reports whether the calling thread is owned by this loop.
   *
   * @details
   * Detects callbacks that re-enter the loop synchronously.  For @c MultiLoop this also covers
   * worker threads of the underlying pool.
   *
   * @return @c true when called from a thread belonging to this loop.
   */
  [[nodiscard]] virtual bool is_in_same_thread() const;

  /**
   * @brief Returns the shared lifetime flag used by cross-thread bridges.
   *
   * @details
   * The returned @c AliveState outlives this loop.  Adapters that need to post continuations
   * back to this loop should lock @c mtx, re-check @c alive, and only call back while still
   * holding the lock.
   *
   * @return Shared handle; never null while the loop object is alive.
   */
  [[nodiscard]] std::shared_ptr<AliveState> get_alive_state() const;

  /**
   * @brief Dispatches a callable to the loop thread and returns a @c std::future for the result.
   *
   * @details
   * Thread-safe.  When posting fails the returned future becomes ready with
   * @c std::future_error / @c broken_promise.
   *
   * @warning Do not call @c .get() on the future from the loop's own thread; doing so deadlocks.
   *
   * @tparam FunctionT  Callable type.
   * @tparam ArgsT      Argument types.
   * @tparam ResultT    Return type (deduced).
   * @param function  Callable to dispatch.
   * @param args      Arguments forwarded to @p function.
   * @return Future that becomes ready after the callable completes.
   */
  template <class FunctionT, class... ArgsT, typename ResultT = std::invoke_result_t<FunctionT, ArgsT...>>
  [[nodiscard]] std::future<ResultT> invoke_task(FunctionT&& function, ArgsT&&... args);

  /**
   * @brief Priority variant of @c invoke_task; requires a @c kPriorityType loop.
   *
   * @tparam FunctionT  Callable type.
   * @tparam ArgsT      Argument types.
   * @tparam ResultT    Return type (deduced).
   * @param function  Callable to dispatch.
   * @param priority  Dispatch priority.
   * @param args      Arguments forwarded to @p function.
   * @return Future that becomes ready after the callable completes.
   */
  template <class FunctionT, class... ArgsT, typename ResultT = std::invoke_result_t<FunctionT, ArgsT...>>
  [[nodiscard]] std::future<ResultT> invoke_task_with_priority(FunctionT&& function, uint16_t priority,
                                                               ArgsT&&... args);

 protected:
  /**
   * @brief Hook invoked once on the loop thread before the first task runs.
   *
   * @details
   * Subclasses override to perform per-thread initialisation.
   */
  virtual void on_begin();

  /**
   * @brief Hook invoked once on the loop thread after the last task runs.
   *
   * @details
   * Subclasses override to perform per-thread cleanup.
   */
  virtual void on_end();

  /**
   * @brief Hook invoked on the loop thread each time the queue becomes empty.
   *
   * @details
   * Subclasses override to perform idle bookkeeping.
   */
  virtual void on_idle();

  /**
   * @brief Dispatches a ready task on the loop thread.
   *
   * @details
   * The default implementation invokes @p callback inline.  Subclasses such as @c MultiLoop
   * override to redirect the call to a worker pool; overrides that do not forward must invoke
   * @p callback themselves or the task is silently dropped.
   *
   * @param callback    Task to dispatch.
   * @param start_time  Millisecond @c steady_clock timestamp captured at enqueue time, or @c 0
   *                    when elapsed tracking is disabled.
   */
  virtual void on_task_changed(Callback&& callback, uint32_t start_time);

  /**
   * @brief Hook invoked when a task exceeds @c get_max_elapsed_time().
   *
   * @param callback      Task that timed out.
   * @param elapsed_time  Actual execution time in milliseconds.
   */
  virtual void on_task_timeout(Callback&& callback, uint32_t elapsed_time);

 private:
  static uint64_t get_current_nano_time();

  bool add_timer(Timer* timer);

  bool remove_timer(Timer* timer);

  bool push_task(Callback&& callback, uint16_t priority, bool droppable = true,
                 TaskOverflowPolicy overflow_policy = TaskOverflowPolicy::kUseDispatcherStrategy,
                 const TaskHandle* submit_handle = nullptr);

  bool drop_one_normal_task();

  bool drop_one_lockfree_task(bool keep_reserved = false);

  bool drop_one_priority_task();

  bool reserve_lockfree_task();

  void release_lockfree_task();

  void push_normal_task(Callback&& callback, bool droppable = true);

  bool push_lockfree_task(Callback&& callback);

  void push_priority_task(Callback&& callback, uint16_t priority, bool droppable = true);

  void do_consume();

  bool process_normal_task(bool block);

  bool process_lockfree_task(bool block);

  bool process_priority_task(bool block);

  bool process_timer_task(int64_t& next_sleep_time);

  void drop_pending_tasks();

  friend Timer;
  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(MessageLoop)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

template <typename CallbackT, typename>
Schedule::Status MessageLoop::exec_task(const Schedule::Config& config, CallbackT&& callback) {
  Schedule::Callback wrapper_callback;

  // NOLINTNEXTLINE(bugprone-move-forwarding-reference)
  auto status = Schedule::process(config, std::move(callback), wrapper_callback);

  bool post_ret = false;

  if (config.delay_ms > 0) {
    post_ret = Timer::call_once(this, config.delay_ms, std::move(wrapper_callback), config.priority);
  } else {
    if (get_type() == kPriorityType && config.priority != kNoPriority) {
      post_ret = post_task_with_priority(std::move(wrapper_callback), config.priority);
    } else {
      post_ret = post_task(std::move(wrapper_callback));
    }
  }

  if VUNLIKELY (!post_ret) {
    status.set_valid(false);
  }

  return status;
}

template <typename CallbackT, typename>
Schedule::RetStatus MessageLoop::exec_task(const Schedule::Config& config, CallbackT&& callback) {
  Schedule::Callback wrapper_callback;

  // NOLINTNEXTLINE(bugprone-move-forwarding-reference)
  auto status = Schedule::process_with_ret(config, std::move(callback), wrapper_callback);

  bool post_ret = false;

  if (config.delay_ms > 0) {
    post_ret = Timer::call_once(this, config.delay_ms, std::move(wrapper_callback), config.priority);
  } else {
    if (get_type() == kPriorityType && config.priority != kNoPriority) {
      post_ret = post_task_with_priority(std::move(wrapper_callback), config.priority);
    } else {
      post_ret = post_task(std::move(wrapper_callback));
    }
  }

  if VUNLIKELY (!post_ret) {
    status.set_valid(false);
  }

  return status;
}

template <class FunctionT, class... ArgsT, typename ResultT>
inline std::future<ResultT> MessageLoop::invoke_task(FunctionT&& function, ArgsT&&... args) {
  auto bound =
      std::bind(std::forward<FunctionT>(function), std::forward<ArgsT>(args)...);  // NOLINT(modernize-avoid-bind)

  if constexpr (kIsSupportMoveFunction) {
    std::packaged_task<ResultT()> task(std::move(bound));
    auto res = task.get_future();

    if VUNLIKELY (!post_task([task = std::move(task)]() mutable { task(); })) {
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

template <class FunctionT, class... ArgsT, typename ResultT>
inline std::future<ResultT> MessageLoop::invoke_task_with_priority(FunctionT&& function, uint16_t priority,
                                                                   ArgsT&&... args) {
  auto bound =
      std::bind(std::forward<FunctionT>(function), std::forward<ArgsT>(args)...);  // NOLINT(modernize-avoid-bind)

  if constexpr (kIsSupportMoveFunction) {
    std::packaged_task<ResultT()> task(std::move(bound));
    auto res = task.get_future();

    if VUNLIKELY (!post_task_with_priority([task = std::move(task)]() mutable { task(); }, priority)) {
    }

    return res;
  } else {
    auto task = MemoryResource::make_shared<std::packaged_task<ResultT()>>(std::move(bound));
    auto res = task->get_future();

    if VUNLIKELY (!post_task_with_priority([task]() mutable { (*task)(); }, priority)) {
      task.reset();
    }

    return res;
  }
}

}  // namespace vlink
