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
 * @file coroutine.h
 * @brief Stackless C++20 coroutine layer that resumes work onto @c MessageLoop threads.
 *
 * @details
 * Adds an additive coroutine binding atop @c MessageLoop, @c GraphTask and @c Schedule.  Every
 * awaiter eventually re-enters the target loop through @c MessageLoop::post_task or
 * @c MessageLoop::exec_task; the loop itself is unchanged.  The layer compiles in only when
 * @c VLINK_ENABLE_COROUTINE is defined (auto-enabled when @c VLINK_ENABLE_CXX_STD_20 is set and
 * the toolchain advertises @c __cpp_impl_coroutine and @c __cpp_lib_coroutine); otherwise the
 * file is empty.  The canonical namespace is @c vlink::Coroutine with @c vlink::Co as a short
 * alias.
 *
 * @par State machine of a coroutine resume
 *
 * @verbatim
 *   +----------+    co_spawn /    +-----------+   awaiter   +-------------+
 *   | created  | -- co_await ---> | suspended | -- ready -> | resumed on  |
 *   +----------+                  +-----------+             | target loop |
 *        |                             ^                    +-------------+
 *        |                             |                          |
 *        |    queue full (kRetry)      |                          | co_return
 *        |  +--------------------------+                          v
 *        |  |                                              +-------------+
 *        |  | helper thread retries every ~1ms             |  finished   |
 *        v  v                                              +-------------+
 *   +-----------+    loop closed (kClosed)
 *   |  failed   |  --> await_resume throws runtime_error / OperationCancelled
 *   +-----------+
 * @endverbatim
 *
 * @par Awaitable surface
 *
 * | Category      | Symbol                            | Purpose                              |
 * | ------------- | --------------------------------- | ------------------------------------ |
 * | Core type     | @c Task<TypeT>                    | Lazily started awaitable             |
 * | Awaiter       | @c schedule(loop, prio)           | Hop onto @p loop 's thread           |
 * | Awaiter       | @c yield(loop, prio)              | Re-post to the back of the queue     |
 * | Awaiter       | @c delay_ms(loop, ms, prio)       | Non-blocking sleep                   |
 * | Awaiter       | @c await_future(loop, fut)        | Wait on @c std::future               |
 * | Entry point   | @c co_spawn(loop, task)           | Fire-and-forget @c Task<void>        |
 * | Entry point   | @c co_spawn(loop, task, on_done)  | Spawn with completion callback       |
 * | Entry point   | @c co_spawn_with_priority(...)    | Same overloads with priority         |
 * | Bridge        | @c exec(loop, config, fn)         | Wrap @c MessageLoop::exec_task       |
 * | Bridge        | @c await_graph(loop, graph)       | Wait on @c GraphTask DAG             |
 * | Orchestration | @c when_all(loop, tasks)          | Join all, collect results            |
 * | Orchestration | @c when_any(loop, tasks)          | First success and its index          |
 * | Orchestration | @c sequence(loop, tasks)          | Run in order                         |
 *
 * @par Frame allocation
 * Coroutine frames flow through @c vlink::MemoryPool::global_instance() via the
 * @c operator @c new / @c operator @c delete overloads on @c TaskPromise and
 * @c DetachedTask::promise_type.  The allocator throws @c std::bad_alloc on pool exhaustion;
 * there is no custom-allocator template parameter.
 *
 * @par Lock order
 * @c MessageLoop::AliveState::mtx -> @c MessageLoop::Impl::mtx -> @c TaskHandle::State::mtx ->
 * awaiter shared-state mutex.  Each awaiter owns its own state object that holds the suspended
 * coroutine handle.  When the target loop closes mid-resume the handle is released on the
 * @c FutureWaitLoop helper thread via a queued retry; callers must therefore not assume
 * @c await_resume runs on the original loop.
 *
 * @par Cancellation contract
 *  - @c await_future rethrows the promise's stored exception via @c future.get().  When the
 *    target loop closes before the ready future can be posted back, @c await_resume throws
 *    @c vlink::Exception::OperationCancelled.
 *  - @c await_graph normally returns @c void; on the late-closure path it throws
 *    @c vlink::Exception::OperationCancelled.
 *  - @c schedule / @c yield / @c delay_ms retry posting through the @c FutureWaitLoop helper on
 *    @c kRetry; once @c detail::kMaxResumePostRetry (@c 30000 ticks of approximately one
 *    millisecond each) is exhausted, or the loop reports @c kClosed, @c await_resume throws
 *    @c std::runtime_error with a descriptive message.
 *
 * @par FutureWaitLoop
 * A process-wide helper thread polls every registered awaiter at a roughly one-millisecond
 * cadence.  Concurrent use of @c await_future shares this thread; latency is bounded by the
 * cadence plus the target loop's task dispatch delay.
 *
 * @par Priority remap on full-queue loops
 * On @c kPriorityType loops the post helper remaps @c MessageLoop::kNoPriority to
 * @c MessageLoop::kNormalPriority so default-priority resumes do not sink to the queue tail.
 *
 * @par Alive-state mutex hold time
 * @c post_callback_if_alive acquires @c MessageLoop::AliveState::mtx only briefly; the inner
 * @c post_callback path uses @c TaskOverflowPolicy::kReject so the alive-state mutex is never
 * held across a sleep or backoff.
 *
 * @par Example
 * @code
 *   vlink::MessageLoop loop;
 *   loop.async_run();
 *
 *   vlink::Co::Task<> my_routine(vlink::MessageLoop& loop, int* counter) {
 *     co_await vlink::Co::delay_ms(loop, 100);
 *     ++(*counter);
 *     co_await vlink::Co::yield(loop);
 *     ++(*counter);
 *   }
 *
 *   int counter = 0;
 *   vlink::Co::co_spawn(loop, my_routine(loop, &counter));
 * @endcode
 *
 * @warning Passing a coroutine lambda as a temporary, for example
 *          @code
 *          co_spawn(loop, []() -> Task<> { co_await ... ; }());
 *          @endcode
 *          destroys the lambda object at the end of the full-expression -- long before the
 *          coroutine body runs.  Any captured state (including @c [&] and @c [=]) becomes a
 *          dangling reference.  Always thread captures through function parameters so the
 *          compiler copies them into the coroutine frame.
 */

#pragma once

#include "../version.h"

#ifdef VLINK_ENABLE_CXX_STD_20
#if __has_include(<coroutine>)
#include <coroutine>
#endif
#if defined(__cpp_lib_coroutine)
#ifndef VLINK_ENABLE_COROUTINE
#define VLINK_ENABLE_COROUTINE
#endif
#endif
#endif

#ifdef VLINK_ENABLE_COROUTINE

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "./exception.h"
#include "./functional.h"
#include "./graph_task.h"
#include "./macros.h"
#include "./memory_resource.h"
#include "./message_loop.h"
#include "./schedule.h"

namespace vlink {

/**
 * @namespace vlink::Coroutine
 * @brief Container namespace for the VLink coroutine integration layer.
 *
 * @details
 * Holds the @c Task template, every awaiter, the @c co_spawn entry points and the orchestrators
 * (@c when_all / @c when_any / @c sequence).  Aliased as @c vlink::Co.
 */
namespace Coroutine {  // NOLINT(readability-identifier-naming)

template <typename TypeT = void>
class Task;

/**
 * @namespace vlink::Coroutine::detail
 * @brief Internal helpers for the coroutine layer.
 *
 * @details
 * Hosts promise types, the detached-task adapter, the @c ResumePostResult enum, the awaiter
 * shared state and the helpers that bridge coroutine handles back onto @c MessageLoop.  Not part
 * of the public API surface.
 */
namespace detail {

template <typename TypeT>
struct TaskPromise;

/**
 * @brief Allocates a coroutine frame through the process-wide @c MemoryPool.
 *
 * @param size  Frame size requested by the compiler.
 * @return Pointer to the freshly allocated frame.
 * @throws std::bad_alloc when the underlying pool is exhausted.
 */
VLINK_EXPORT void* allocate_frame(size_t size);

/**
 * @brief Returns a coroutine frame to the process-wide @c MemoryPool.
 *
 * @param ptr   Frame previously obtained from @c allocate_frame.  @c nullptr is a no-op.
 * @param size  Original frame size; must match the @c allocate_frame call.
 */
VLINK_EXPORT void deallocate_frame(void* ptr, size_t size) noexcept;

/**
 * @struct FinalAwaiter
 * @brief Final-suspend awaiter for @c Task promises; performs symmetric transfer.
 *
 * @details
 * On final suspension the promise hands its stored continuation back to the dispatcher so the
 * outer coroutine resumes immediately.  When no continuation is set (the @c Task was never
 * awaited) the awaiter returns @c std::noop_coroutine and the dispatcher unwinds normally.
 */
struct FinalAwaiter final {
  /**
   * @brief Always returns @c false; the final suspension is unconditional.
   *
   * @return @c false.
   */
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  bool await_ready() const noexcept;

  /**
   * @brief Returns the parent coroutine handle for symmetric transfer.
   *
   * @tparam PromiseT  Promise type of the finishing coroutine.
   * @param handle     Handle to the finishing coroutine; its promise carries
   *                   the @c continuation to resume.
   * @return The stored continuation, or @c std::noop_coroutine() if none.
   */
  template <typename PromiseT>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<PromiseT> handle) noexcept;

  /**
   * @brief No-op resume; the final awaiter never produces a value.
   */
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  void await_resume() const noexcept;
};

/**
 * @struct TaskPromiseBase
 * @brief Shared storage and suspension hooks reused by every @c Task promise specialisation.
 *
 * @details
 * Holds the parent continuation handle resumed at final suspension and an
 * @c std::exception_ptr capturing any unhandled exception thrown by the coroutine body.
 *
 * @tparam TypeT  Result type of the owning @c Task.
 */
template <typename TypeT>
struct TaskPromiseBase {
  std::coroutine_handle<> continuation;  ///< Awaiter coroutine to resume on final suspension.
  std::exception_ptr exception;          ///< Captured unhandled exception, if any.

  /**
   * @brief Initial-suspend hook; always suspends so the task starts lazily.
   *
   * @return @c std::suspend_always.
   */
  std::suspend_always initial_suspend() noexcept;

  /**
   * @brief Final-suspend hook; returns a @c FinalAwaiter for symmetric transfer.
   *
   * @return @c FinalAwaiter instance bound to this promise.
   */
  FinalAwaiter final_suspend() noexcept;

  /**
   * @brief Captures the currently active exception into the @c exception slot.
   */
  void unhandled_exception() noexcept;
};

/**
 * @struct TaskPromise
 * @brief Promise type for value-returning @c Task<TypeT>.
 *
 * @details
 * Allocates frames from the global @c MemoryPool and stores the @c co_return value in
 * @c result so @c Task::await_resume can consume it.
 *
 * @tparam TypeT  Result type produced by the coroutine body.
 */
template <typename TypeT>
struct TaskPromise final : TaskPromiseBase<TypeT> {
  std::optional<TypeT> result;  ///< Storage for the @c co_return value.

  /**
   * @brief Allocates the coroutine frame from the global memory pool.
   *
   * @param size  Frame size requested by the compiler.
   * @return Pointer to the allocated frame.
   */
  static void* operator new(size_t size);

  /**
   * @brief Returns the coroutine frame to the global memory pool.
   *
   * @param ptr   Frame previously allocated by @c operator @c new.
   * @param size  Original frame size.
   */
  static void operator delete(void* ptr, size_t size) noexcept;

  /**
   * @brief Builds the @c Task object that owns this coroutine.
   *
   * @return @c Task<TypeT> wrapping the handle for this promise.
   */
  Task<TypeT> get_return_object() noexcept;

  /**
   * @brief Captures the @c co_return value into @c result.
   *
   * @tparam UpT  Forwarded value type.
   * @param v     Value produced by @c co_return.
   */
  template <typename UpT>
  void return_value(UpT&& v) noexcept(std::is_nothrow_constructible_v<TypeT, UpT&&>);
};

/**
 * @struct TaskPromise<void>
 * @brief Specialisation of @c TaskPromise for @c Task<void>.
 *
 * @details
 * Carries no result slot; @c return_void is a no-op aside from satisfying the
 * coroutine promise contract.
 */
template <>
struct TaskPromise<void> final : TaskPromiseBase<void> {
  /**
   * @brief Allocates the coroutine frame from the global memory pool.
   *
   * @param size  Frame size requested by the compiler.
   * @return Pointer to the allocated frame.
   */
  static void* operator new(size_t size);

  /**
   * @brief Returns the coroutine frame to the global memory pool.
   *
   * @param ptr   Frame previously allocated by @c operator @c new.
   * @param size  Original frame size.
   */
  static void operator delete(void* ptr, size_t size) noexcept;

  /**
   * @brief Builds the @c Task<void> that owns this coroutine.
   *
   * @return @c Task<void> wrapping the handle for this promise.
   */
  Task<void> get_return_object() noexcept;

  /**
   * @brief No-op @c co_return for void tasks.
   */
  void return_void() noexcept;
};

/**
 * @struct DetachedTask
 * @brief Fire-and-forget coroutine wrapper used internally by @c co_spawn.
 *
 * @details
 * Holds a @c std::coroutine_handle for a coroutine whose frame is owned by the
 * task itself: the promise type does not suspend at the final point and the
 * frame is destroyed automatically when the body completes.  Exceptions thrown
 * by the inner coroutine are caught and logged at error level so that a stray
 * throw cannot tear down the owning @c MessageLoop.
 */
struct VLINK_EXPORT DetachedTask final {
  /**
   * @struct promise_type
   * @brief Promise type that powers @c DetachedTask coroutines.
   *
   * @details
   * Frames are allocated through the global @c MemoryPool.  The coroutine
   * suspends initially (so the caller can decide where to resume it) and
   * never suspends at the final point (the frame self-destructs).
   */
  struct VLINK_EXPORT promise_type final {  // NOLINT(readability-identifier-naming)
    /**
     * @brief Allocates the coroutine frame from the global memory pool.
     *
     * @param size  Frame size requested by the compiler.
     * @return Pointer to the allocated frame.
     */
    static void* operator new(size_t size);

    /**
     * @brief Returns the coroutine frame to the global memory pool.
     *
     * @param ptr   Frame previously allocated by @c operator @c new.
     * @param size  Original frame size.
     */
    static void operator delete(void* ptr, size_t size) noexcept;

    /**
     * @brief Builds the @c DetachedTask wrapper that holds this promise's handle.
     *
     * @return @c DetachedTask wrapping the new coroutine handle.
     */
    DetachedTask get_return_object() noexcept;

    /**
     * @brief Initial suspension; the caller decides when to resume.
     *
     * @return @c std::suspend_always.
     */
    std::suspend_always initial_suspend() noexcept;

    /**
     * @brief Final suspension; never suspends so the frame self-destructs.
     *
     * @return @c std::suspend_never.
     */
    std::suspend_never final_suspend() noexcept;

    /**
     * @brief No-op @c co_return for detached coroutines.
     */
    void return_void() noexcept;

    /**
     * @brief Catches and logs unhandled exceptions thrown by the body.
     *
     * @details
     * Exceptions are intentionally swallowed; the detached frame must not
     * propagate failures back into the owning @c MessageLoop.
     */
    void unhandled_exception() noexcept;
  };

  using Handle = std::coroutine_handle<promise_type>;

  /**
   * @brief Constructs an empty detached task with no associated frame.
   */
  DetachedTask() noexcept;

  /**
   * @brief Destroys the detached task.  Does not own the frame after release.
   */
  ~DetachedTask();

  /**
   * @brief Constructs a detached task that owns @p h.
   *
   * @param h  Coroutine handle to take ownership of.
   */
  explicit DetachedTask(Handle h) noexcept;

  /**
   * @brief Move-constructs a detached task, transferring frame ownership.
   *
   * @param other  Source task; left empty after the move.
   */
  DetachedTask(DetachedTask&& other) noexcept;

  /**
   * @brief Move-assigns a detached task, transferring frame ownership.
   *
   * @param other  Source task; left empty after the move.
   * @return Reference to @c *this.
   */
  DetachedTask& operator=(DetachedTask&& other) noexcept;

  Handle handle;  ///< Coroutine handle; @c {} when the task has been released.

  VLINK_DISALLOW_COPY_AND_ASSIGN(DetachedTask)
};

/**
 * @brief Bridges a @c Task<void> into a @c DetachedTask coroutine.
 *
 * @param task  Task to await inside the detached coroutine.
 * @return @c DetachedTask owning the new frame.
 */
VLINK_EXPORT DetachedTask co_spawn_void_impl(Task<void> task);

/**
 * @brief Bridges a value-returning @c Task<TypeT> with a completion callback.
 *
 * @tparam TypeT      Result type produced by the inner task.
 * @tparam CallbackT  Callable invoked with the task's result on success.
 * @param task         Inner task to await.
 * @param on_complete  Callback invoked on the loop thread on successful completion.
 * @return @c DetachedTask owning the new frame.
 */
template <typename TypeT, typename CallbackT>
DetachedTask co_spawn_value_impl(Task<TypeT> task, CallbackT on_complete);

/**
 * @brief Bridges a @c Task<void> with a completion callback.
 *
 * @tparam CallbackT  Callable invoked after the inner task completes.
 * @param task         Inner task to await.
 * @param on_complete  Callback invoked on the loop thread on successful completion.
 * @return @c DetachedTask owning the new frame.
 */
template <typename CallbackT>
DetachedTask co_spawn_void_with_cb_impl(Task<void> task, CallbackT on_complete);

/**
 * @enum ResumePostResult
 * @brief Outcome of attempting to post a coroutine resume back onto a loop.
 *
 * @details
 * Returned by @c post_callback_if_alive (and by the awaiter helpers built on
 * it).  Drives the retry logic in @c ScheduleAwaiter, @c YieldAwaiter,
 * @c DelayAwaiter, @c FutureAwaiter and @c GraphAwaiter.
 */
enum class ResumePostResult : uint8_t {
  kPosted = 0,  ///< Resume callback was accepted by the target loop's queue.
  kRetry,       ///< Target loop is alive but currently full; caller should retry later.
  kClosed,      ///< Target loop has been (or is about to be) quit; resume cannot be delivered.
};

struct AwaiterResumeState;

/**
 * @brief Maximum number of @c kRetry passes before a queued resume is reported as failed.
 *
 * @details
 * Each retry tick is approximately 1 ms (the @c FutureWaitLoop poll cadence),
 * giving a total bound of roughly 30 seconds before the awaiter converts a
 * stuck retry sequence into either an @c OperationCancelled or an
 * @c std::runtime_error, depending on the awaiter type.
 */
inline constexpr uint32_t kMaxResumePostRetry = 30000U;

/**
 * @brief Posts a callback onto @p loop while honouring the alive-state gate.
 *
 * @details
 * If @p loop or @p alive_state is null, or if the loop has been signalled to
 * quit, @p drop_callback is invoked and @c ResumePostResult::kClosed is
 * returned.  Otherwise the function takes @p alive_state's mutex briefly and
 * attempts to post @p resume_callback under @c TaskOverflowPolicy::kReject; on
 * success it returns @c kPosted, on a full queue it returns @c kRetry without
 * having held the mutex across any sleep.  On @c kPriorityType loops the
 * @c MessageLoop::kNoPriority value is remapped to
 * @c MessageLoop::kNormalPriority so default-priority coroutines do not sink to
 * the bottom of the priority queue.
 *
 * @param loop              Target loop to post onto.
 * @param alive_state       Loop's alive-state gate; obtained via @c MessageLoop::get_alive_state.
 * @param resume_callback   Callable invoked on the loop thread on success.
 * @param drop_callback     Callable invoked when the loop is closed or the task is dropped.
 * @param priority          Schedule priority; honoured only on @c kPriorityType loops.
 * @return One of @c kPosted, @c kRetry, @c kClosed.
 */
VLINK_EXPORT ResumePostResult post_callback_if_alive(MessageLoop* loop,
                                                     const std::shared_ptr<MessageLoop::AliveState>& alive_state,
                                                     MoveFunction<void()>&& resume_callback,
                                                     MoveFunction<void()>&& drop_callback, uint16_t priority = 0);

/**
 * @brief Posts the initial resume of a detached coroutine onto @p loop.
 *
 * @details
 * Used by the top-level @c co_spawn entry points.  If the loop is already
 * closed or its queue cannot accept the resume after @c kMaxResumePostRetry
 * attempts, the handle is destroyed instead of being resumed.
 *
 * @param loop      Target loop on which the coroutine should start running.
 * @param handle    Detached coroutine handle taking ownership of the frame.
 * @param priority  Schedule priority; honoured only on @c kPriorityType loops.
 */
VLINK_EXPORT void co_spawn_detached_handle(MessageLoop& loop, DetachedTask::Handle handle, uint16_t priority = 0);

/**
 * @brief Enqueues a poll closure on the shared @c FutureWaitLoop helper thread.
 *
 * @details
 * The closure is invoked repeatedly at the helper's poll cadence (~1 ms).  It
 * returns @c true when it has finished and may be removed from the queue, or
 * @c false to be re-polled on the next pass.
 *
 * @param poll  Poll closure registered with the helper thread.
 */
VLINK_EXPORT void register_future_wait(MoveFunction<bool()>&& poll);

}  // namespace detail

/**
 * @class Task
 * @brief Coroutine return type; lazily started, awaitable from another coroutine.
 *
 * @details
 * The coroutine body does not run until the @c Task is either awaited from
 * another coroutine or passed to @c co_spawn().  Moving the @c Task transfers
 * ownership; destroying a still-suspended @c Task destroys its coroutine frame.
 *
 * @tparam TypeT  Result type produced by @c co_return.  Use @c void for tasks
 *                that produce no value.
 */
template <typename TypeT>
class Task final {
 public:
  using promise_type = detail::TaskPromise<TypeT>;
  using Handle = std::coroutine_handle<promise_type>;

  /**
   * @brief Constructs an empty @c Task that does not own a coroutine frame.
   */
  Task() noexcept = default;

  /**
   * @brief Constructs a @c Task that takes ownership of @p handle.
   *
   * @param handle  Coroutine handle produced by @c TaskPromise::get_return_object.
   */
  explicit Task(Handle handle) noexcept;

  /**
   * @brief Move-constructs a @c Task, transferring frame ownership.
   *
   * @param other  Source task; left empty after the move.
   */
  Task(Task&& other) noexcept;

  /**
   * @brief Move-assigns a @c Task, destroying any previously owned frame.
   *
   * @param other  Source task; left empty after the move.
   * @return Reference to @c *this.
   */
  Task& operator=(Task&& other) noexcept;

  /**
   * @brief Destroys the owned coroutine frame, if any.
   */
  ~Task();

  /**
   * @brief Tests whether this task owns a coroutine frame.
   *
   * @return @c true if the task is non-empty.
   */
  [[nodiscard]] bool valid() const noexcept;

  /**
   * @brief Tests whether the owned frame has finished executing.
   *
   * @return @c true if the task is valid and the coroutine has run to completion.
   */
  [[nodiscard]] bool done() const noexcept;

  /**
   * @brief Coroutine awaiter readiness hook.
   *
   * @return @c true if the task is empty or already done; otherwise @c false.
   */
  bool await_ready() const noexcept;

  /**
   * @brief Coroutine awaiter suspension hook; performs symmetric transfer.
   *
   * @param awaiter  Coroutine handle awaiting this task's completion.
   * @return The handle of the inner coroutine to resume.
   */
  std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiter) noexcept;

  /**
   * @brief Coroutine awaiter resume hook; extracts the result or rethrows.
   *
   * @details
   * Throws @c std::logic_error when called on an invalid task.  If the inner
   * coroutine captured an unhandled exception, it is rethrown here.
   *
   * @return The value stored by @c co_return for non-void tasks.
   */
  TypeT await_resume();

  /**
   * @brief Releases ownership of the underlying coroutine handle.
   *
   * @return Handle owned by this task; the task is empty after the call.
   */
  Handle release() noexcept;

  /**
   * @brief Returns the underlying coroutine handle without releasing ownership.
   *
   * @return Current coroutine handle (may be @c {} for an empty task).
   */
  [[nodiscard]] Handle native_handle() const noexcept;

 private:
  Handle handle_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(Task)
};

/**
 * @struct ScheduleAwaiter
 * @brief Awaiter that resumes the coroutine on @p loop's worker thread.
 *
 * @details
 * Built on top of @c detail::post_callback_if_alive.  After @c co_await
 * @c schedule(loop), the remainder of the coroutine runs on @p loop's thread.
 * If the target loop's queue is currently full, the awaiter retries via the
 * shared @c FutureWaitLoop helper at a ~1 ms cadence, up to
 * @c detail::kMaxResumePostRetry attempts.
 *
 * @warning If @p loop is already quitting or retries are exhausted,
 *          @c await_resume() throws @c std::runtime_error.  The failure resume
 *          is delivered on the @c FutureWaitLoop helper thread, not on the
 *          target loop thread.
 */
struct VLINK_EXPORT ScheduleAwaiter final {
  /**
   * @brief Constructs an empty awaiter with no target loop.
   */
  ScheduleAwaiter() noexcept;

  /**
   * @brief Constructs an awaiter bound to a target loop and resume state.
   *
   * @param loop          Target loop on which to resume.
   * @param alive_state   Target loop's alive-state gate.
   * @param state         Shared awaiter resume state.
   * @param priority      Schedule priority for the resume.
   */
  ScheduleAwaiter(MessageLoop* loop, std::shared_ptr<MessageLoop::AliveState> alive_state,
                  std::shared_ptr<detail::AwaiterResumeState> state, uint16_t priority) noexcept;

  /**
   * @brief Move-constructs the awaiter.
   */
  ScheduleAwaiter(ScheduleAwaiter&&) noexcept;

  /**
   * @brief Move-assigns the awaiter.
   */
  ScheduleAwaiter& operator=(ScheduleAwaiter&&) noexcept;

  MessageLoop* loop{nullptr};                            ///< Target loop on which to resume.
  std::shared_ptr<MessageLoop::AliveState> alive_state;  ///< Target loop's alive-state gate.
  std::shared_ptr<detail::AwaiterResumeState> state;     ///< Shared resume state holding the handle.
  uint16_t priority{0};                                  ///< Schedule priority for the resume.
  bool failed{false};                                    ///< Set to @c true if posting ultimately failed.

  /**
   * @brief Awaiter readiness hook.
   *
   * @return Always @c false; the hop must be posted asynchronously.
   */
  static bool await_ready() noexcept;

  /**
   * @brief Destructor; abandons any pending retry registered on the helper thread.
   */
  ~ScheduleAwaiter();

  /**
   * @brief Posts the coroutine resume onto the target loop.
   *
   * @details
   * On @c kRetry the awaiter registers a retry closure on the shared
   * @c FutureWaitLoop helper; on @c kClosed the @c failed flag is raised so
   * that @c await_resume can throw.
   *
   * @param handle  Coroutine handle to resume on success.
   * @return @c true; the awaiter always stays suspended until a resume fires.
   */
  bool await_suspend(std::coroutine_handle<> handle);

  /**
   * @brief Verifies that the hop succeeded.
   *
   * @throws std::runtime_error if the target loop closed or retries were exhausted.
   */
  void await_resume();

  VLINK_DISALLOW_COPY_AND_ASSIGN(ScheduleAwaiter)
};

/**
 * @struct YieldAwaiter
 * @brief Awaiter that re-posts the coroutine to the back of @p loop's queue.
 *
 * @details
 * Useful for cooperative yielding inside a long-running coroutine running on
 * the loop thread, allowing other queued tasks a chance to execute.  Built on
 * top of the same @c post_callback_if_alive machinery as @c ScheduleAwaiter.
 *
 * @warning If @p loop is already quitting or retries are exhausted,
 *          @c await_resume() throws @c std::runtime_error.  The failure resume
 *          is delivered on the @c FutureWaitLoop helper thread.
 */
struct VLINK_EXPORT YieldAwaiter final {
  /**
   * @brief Constructs an empty awaiter with no target loop.
   */
  YieldAwaiter() noexcept;

  /**
   * @brief Constructs an awaiter bound to a target loop and resume state.
   *
   * @param loop          Target loop on which to re-post.
   * @param alive_state   Target loop's alive-state gate.
   * @param state         Shared awaiter resume state.
   * @param priority      Schedule priority for the re-post.
   */
  YieldAwaiter(MessageLoop* loop, std::shared_ptr<MessageLoop::AliveState> alive_state,
               std::shared_ptr<detail::AwaiterResumeState> state, uint16_t priority) noexcept;

  /**
   * @brief Move-constructs the awaiter.
   */
  YieldAwaiter(YieldAwaiter&&) noexcept;

  /**
   * @brief Move-assigns the awaiter.
   */
  YieldAwaiter& operator=(YieldAwaiter&&) noexcept;

  MessageLoop* loop{nullptr};                            ///< Target loop to re-post onto.
  std::shared_ptr<MessageLoop::AliveState> alive_state;  ///< Target loop's alive-state gate.
  std::shared_ptr<detail::AwaiterResumeState> state;     ///< Shared resume state holding the handle.
  uint16_t priority{0};                                  ///< Schedule priority for the re-post.
  bool failed{false};                                    ///< Set to @c true if posting ultimately failed.

  /**
   * @brief Awaiter readiness hook.
   *
   * @return Always @c false; the yield must complete a queue round-trip.
   */
  static bool await_ready() noexcept;

  /**
   * @brief Destructor; abandons any pending retry registered on the helper thread.
   */
  ~YieldAwaiter();

  /**
   * @brief Re-posts the coroutine resume onto the target loop.
   *
   * @param handle  Coroutine handle to resume on success.
   * @return @c true; the awaiter always stays suspended until a resume fires.
   */
  bool await_suspend(std::coroutine_handle<> handle);

  /**
   * @brief Verifies that the yield round-trip succeeded.
   *
   * @throws std::runtime_error if the target loop closed or retries were exhausted.
   */
  void await_resume();

  VLINK_DISALLOW_COPY_AND_ASSIGN(YieldAwaiter)
};

/**
 * @struct DelayAwaiter
 * @brief Awaiter that suspends the coroutine for @p ms milliseconds.
 *
 * @details
 * On success the coroutine resumes on @p loop's thread, even for @p ms == 0.
 * Delay completion is observed by the shared @c FutureWaitLoop helper thread
 * at a ~1 ms polling cadence.  When the deadline is reached the awaiter posts
 * its resume through @c post_callback_if_alive; full-queue conditions retry up
 * to @c detail::kMaxResumePostRetry times before failing.
 *
 * @warning If @p loop is already quitting or retries are exhausted,
 *          @c await_resume() throws @c std::runtime_error.  The failure resume
 *          is delivered on the @c FutureWaitLoop helper thread.
 */
struct VLINK_EXPORT DelayAwaiter final {
  /**
   * @brief Constructs an empty awaiter with no target loop.
   */
  DelayAwaiter() noexcept;

  /**
   * @brief Constructs an awaiter bound to a target loop and delay.
   *
   * @param loop          Target loop on which to resume.
   * @param alive_state   Target loop's alive-state gate.
   * @param state         Shared awaiter resume state.
   * @param ms            Delay in milliseconds before the resume is posted.
   * @param priority      Schedule priority for the resume.
   */
  DelayAwaiter(MessageLoop* loop, std::shared_ptr<MessageLoop::AliveState> alive_state,
               std::shared_ptr<detail::AwaiterResumeState> state, uint32_t ms, uint16_t priority) noexcept;

  /**
   * @brief Move-constructs the awaiter.
   */
  DelayAwaiter(DelayAwaiter&&) noexcept;

  /**
   * @brief Move-assigns the awaiter.
   */
  DelayAwaiter& operator=(DelayAwaiter&&) noexcept;

  MessageLoop* loop{nullptr};                            ///< Target loop to resume on.
  std::shared_ptr<MessageLoop::AliveState> alive_state;  ///< Target loop's alive-state gate.
  std::shared_ptr<detail::AwaiterResumeState> state;     ///< Shared resume state holding the handle.
  uint32_t ms{0};                                        ///< Delay in milliseconds.
  uint16_t priority{0};                                  ///< Schedule priority for the resume.
  bool failed{false};                                    ///< Set to @c true if scheduling failed.

  /**
   * @brief Awaiter readiness hook.
   *
   * @return Always @c false; the delay must elapse asynchronously.
   */
  static bool await_ready() noexcept;

  /**
   * @brief Destructor; abandons any pending retry registered on the helper thread.
   */
  ~DelayAwaiter();

  /**
   * @brief Registers the delayed resume on the shared @c FutureWaitLoop helper.
   *
   * @param handle  Coroutine handle to resume once the delay has elapsed.
   * @return @c true; the awaiter always stays suspended until a resume fires.
   */
  bool await_suspend(std::coroutine_handle<> handle);

  /**
   * @brief Verifies that the delay completed and the resume was posted.
   *
   * @throws std::runtime_error if the target loop closed or retries were exhausted.
   */
  void await_resume();

  VLINK_DISALLOW_COPY_AND_ASSIGN(DelayAwaiter)
};

/**
 * @class FutureAwaiter
 * @brief Awaiter that suspends until a @c std::future becomes ready.
 *
 * @details
 * Backed by the process-wide @c FutureWaitLoop helper thread (see
 * @c detail::register_future_wait).  A single long-lived helper thread services
 * all pending @c await_future calls: it polls each registered future at a
 * ~1 ms cadence using a non-blocking @c wait_for(0) and posts the resume back
 * to @p loop as soon as the future is ready.  The future is held in shared
 * waiter state so a destroyed awaiter can retire the pending poll without
 * leaving a dangling frame pointer in the helper thread.
 *
 * @note No per-await thread is spawned.  Resume latency is bounded by the
 *       helper's ~1 ms cadence plus the target loop's task dispatch delay.
 *
 * @warning If @p loop is closed before the ready future can be posted back,
 *          the coroutine resumes on the helper thread and @c await_resume()
 *          throws @c OperationCancelled.  On the normal path, exceptions
 *          stored in the future propagate via @c future.get().
 *
 * @tparam TypeT  Result type produced by the awaited future.
 */
template <typename TypeT>
class FutureAwaiter final {
 public:
  /**
   * @brief Constructs an awaiter that bridges @p fut into the coroutine layer.
   *
   * @param loop  Target loop on which to resume once @p fut is ready.
   * @param fut   Future to await; ownership is transferred into the awaiter.
   */
  FutureAwaiter(MessageLoop* loop, std::future<TypeT> fut) noexcept;

  /**
   * @brief Move-constructs the awaiter.
   */
  FutureAwaiter(FutureAwaiter&&) noexcept = default;

  /**
   * @brief Move-assigns the awaiter.
   */
  FutureAwaiter& operator=(FutureAwaiter&&) noexcept = default;

  /**
   * @brief Destructor; abandons the pending helper poll.
   */
  ~FutureAwaiter();

  /**
   * @brief Short-circuits when the future is already in a ready state.
   *
   * @return @c true if the future is ready or invalid; otherwise @c false.
   */
  bool await_ready() const noexcept;

  /**
   * @brief Registers a poll closure on the shared @c FutureWaitLoop helper.
   *
   * @param handle  Coroutine handle to resume once the future is ready.
   */
  void await_suspend(std::coroutine_handle<> handle);

  /**
   * @brief Extracts the value from the future or rethrows its exception.
   *
   * @return Future result for non-void instantiations.
   * @throws OperationCancelled if the target loop closed before the resume could be posted.
   * @note Exceptions stored in the future are rethrown by @c future.get().
   */
  TypeT await_resume();

 private:
  struct State;

  MessageLoop* loop_{nullptr};
  std::shared_ptr<State> state_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(FutureAwaiter)
};

/**
 * @class GraphAwaiter
 * @brief Awaiter that suspends until a @c GraphTask reaches @c kStatusDone.
 *
 * @details
 * Installs a single-shot status callback on the graph; on @c kStatusDone the
 * coroutine resume is posted back onto @p loop.  If the graph is already done
 * at the await point, @c await_ready returns @c true and no callback is
 * registered.  Cancelled or never-started graphs leave the coroutine suspended
 * indefinitely; the caller must ensure the graph eventually runs.  The status
 * subscription is unregistered automatically once the awaiter resumes, so
 * multiple coroutines awaiting the same graph are safe.
 *
 * @warning If @p loop closes after the graph becomes done but before the resume
 *          can be posted, @c await_resume() throws @c OperationCancelled.  The
 *          failure resume is delivered on the @c FutureWaitLoop helper thread,
 *          not on the target loop thread.
 */
class VLINK_EXPORT GraphAwaiter final {
 public:
  /**
   * @brief Constructs an awaiter that monitors @p graph for completion.
   *
   * @param loop   Loop on which to resume the coroutine.
   * @param graph  Graph task to monitor.
   */
  GraphAwaiter(MessageLoop* loop, GraphTaskPtr graph) noexcept;

  /**
   * @brief Move-constructs the awaiter.
   */
  GraphAwaiter(GraphAwaiter&&) noexcept;

  /**
   * @brief Move-assigns the awaiter.
   */
  GraphAwaiter& operator=(GraphAwaiter&&) noexcept;

  /**
   * @brief Destructor; unregisters the status callback and abandons any pending resume.
   */
  ~GraphAwaiter();

  /**
   * @brief Short-circuits when the graph is empty or already complete.
   *
   * @return @c true if @c graph_ is null or already @c kStatusDone.
   */
  [[nodiscard]] bool await_ready() const noexcept;

  /**
   * @brief Registers a single-shot status callback waiting for @c kStatusDone.
   *
   * @param handle  Coroutine handle to resume on completion.
   */
  void await_suspend(std::coroutine_handle<> handle);

  /**
   * @brief Verifies that the resume reached the target loop.
   *
   * @throws OperationCancelled if the target loop closed before the resume could be posted.
   */
  void await_resume() const;

 private:
  struct State;

  MessageLoop* loop_{nullptr};
  GraphTaskPtr graph_;
  std::shared_ptr<State> state_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(GraphAwaiter)
};

/**
 * @brief Returns an awaiter that hops the coroutine onto @p loop's thread.
 *
 * @details
 * Use @c co_await @c schedule(loop) to migrate the remainder of a coroutine to
 * @p loop's thread.  On @c kPriorityType loops the @c MessageLoop::kNoPriority
 * sentinel is internally remapped to @c MessageLoop::kNormalPriority so default
 * coroutines do not sink to the bottom of the queue.
 *
 * @param loop      Target loop to resume on.
 * @param priority  Schedule priority.  Values come from @c MessageLoop::Priority
 *                  (@c kNoPriority is FIFO, @c kNormalPriority is 100, etc.).
 *                  Honoured only on @c kPriorityType loops.
 * @return @c ScheduleAwaiter ready to be @c co_await-ed.
 */
VLINK_EXPORT ScheduleAwaiter schedule(MessageLoop& loop, uint16_t priority = MessageLoop::kNoPriority) noexcept;

/**
 * @brief Returns an awaiter that yields once back to @p loop's queue.
 *
 * @param loop      Loop owning the coroutine.
 * @param priority  Re-post priority from @c MessageLoop::Priority; honoured only
 *                  on @c kPriorityType loops.
 * @return @c YieldAwaiter ready to be @c co_await-ed.
 */
VLINK_EXPORT YieldAwaiter yield(MessageLoop& loop, uint16_t priority = MessageLoop::kNoPriority) noexcept;

/**
 * @brief Returns an awaiter that suspends for @p ms milliseconds.
 *
 * @param loop      Loop on which the coroutine resumes after the delay.
 * @param ms        Delay in milliseconds.  Zero is allowed and still produces a
 *                  resume hop through the helper thread.
 * @param priority  Resume priority from @c MessageLoop::Priority; honoured only
 *                  on @c kPriorityType loops.
 * @return @c DelayAwaiter ready to be @c co_await-ed.
 */
VLINK_EXPORT DelayAwaiter delay_ms(MessageLoop& loop, uint32_t ms,
                                   uint16_t priority = MessageLoop::kNoPriority) noexcept;

/**
 * @brief Returns an awaiter that suspends until @p fut is ready, resuming on @p loop.
 *
 * @tparam TypeT  Result type stored in the future.
 * @param loop  Loop on which the coroutine resumes once @p fut is ready.
 * @param fut   Future to await; ownership is transferred into the awaiter.
 * @return @c FutureAwaiter ready to be @c co_await-ed.
 */
template <typename TypeT>
FutureAwaiter<TypeT> await_future(MessageLoop& loop, std::future<TypeT> fut) noexcept;

/**
 * @brief Schedules a top-level @c Task<void> on @p loop (fire-and-forget).
 *
 * @details
 * The coroutine begins executing on @p loop's thread.  Ownership of the task
 * frame is transferred to an internal @c DetachedTask which destroys the frame
 * once the body completes.  Exceptions thrown from the task are caught by
 * @c DetachedTask::unhandled_exception, logged at error level and swallowed:
 * the loop continues running.
 *
 * @par Joining from synchronous code
 * Use the three-argument @c co_spawn overload with a callback to bridge to a
 * @c std::promise / @c std::future pair, then wait on the future:
 * @code
 *   std::promise<void> p;
 *   auto fut = p.get_future();
 *   Co::co_spawn(loop, my_void_task(), [&p]() { p.set_value(); });
 *   fut.wait();
 * @endcode
 *
 * @note If @c my_void_task throws, the callback is not invoked and @c fut
 *       never becomes ready.  Either catch inside @c my_void_task or use a
 *       value-returning task that converts errors into a sentinel result.
 *
 * @param loop  Loop on which the coroutine starts running.
 * @param task  Task to spawn; ownership is transferred into the call.
 */
VLINK_EXPORT void co_spawn(MessageLoop& loop, Task<void>&& task);

/**
 * @brief Same as @c co_spawn, with an explicit dispatch priority.
 *
 * @param loop      Loop on which the coroutine starts running.
 * @param task      Task to spawn; ownership is transferred into the call.
 * @param priority  Schedule priority; honoured only on @c kPriorityType loops.
 */
VLINK_EXPORT void co_spawn_with_priority(MessageLoop& loop, Task<void>&& task, uint16_t priority);

/**
 * @brief Schedules a top-level @c Task<TypeT> with a completion callback.
 *
 * @details
 * The coroutine begins executing on @p loop's thread.  When the task completes
 * successfully, @p on_complete is invoked on the loop thread with the result.
 *
 * @warning Exceptions thrown by the task are swallowed: they are logged via
 *          @c DetachedTask::unhandled_exception and @p on_complete is NOT
 *          invoked.  This API is unsuitable for code that must observe task
 *          failure.  To observe failure either:
 *          - Catch inside the task body and convert to a sentinel result, or
 *          - Use @c when_all / @c when_any / @c sequence, which capture the
 *            original exception via the runner+guard machinery, or
 *          - @c co_await the @c Task directly from a parent coroutine.
 *
 * @par Example
 * @code
 * auto pp = std::make_shared<std::promise<TypeT>>();
 * auto fut = pp->get_future();
 * Co::co_spawn(loop, task_that_may_throw(),
 *              [pp](TypeT v) { pp->set_value(std::move(v)); });
 * try {
 *   TypeT result = co_await Co::await_future(loop, std::move(fut));
 * } catch (...) {
 *   // The future still does not see the original exception.
 * }
 * @endcode
 *
 * @tparam TypeT      Result type produced by the task.
 * @tparam CallbackT  Callable invoked with the task's result on success.
 * @param loop         Loop on which the coroutine starts running.
 * @param task         Task to spawn; ownership is transferred into the call.
 * @param on_complete  Completion callback invoked on the loop thread on success.
 */
template <typename TypeT, typename CallbackT,
          // NOLINTNEXTLINE(modernize-use-constraints)
          typename = std::enable_if_t<!std::is_integral_v<std::remove_reference_t<CallbackT>>>>
void co_spawn(MessageLoop& loop, Task<TypeT>&& task, CallbackT&& on_complete);

/**
 * @brief Same as the value-returning @c co_spawn, with an explicit dispatch priority.
 *
 * @tparam TypeT      Result type produced by the task.
 * @tparam CallbackT  Callable invoked with the task's result on success.
 * @param loop         Loop on which the coroutine starts running.
 * @param task         Task to spawn; ownership is transferred into the call.
 * @param on_complete  Completion callback invoked on the loop thread on success.
 * @param priority     Schedule priority; honoured only on @c kPriorityType loops.
 */
template <typename TypeT, typename CallbackT,
          // NOLINTNEXTLINE(modernize-use-constraints)
          typename = std::enable_if_t<!std::is_integral_v<std::remove_reference_t<CallbackT>>>>
void co_spawn_with_priority(MessageLoop& loop, Task<TypeT>&& task, CallbackT&& on_complete, uint16_t priority);

/**
 * @brief Returns an awaiter that suspends until @p graph reaches @c kStatusDone.
 *
 * @param loop   Loop on which to resume the coroutine.
 * @param graph  Graph task to monitor; must be non-null.
 * @return @c GraphAwaiter ready to be @c co_await-ed.
 * @throws std::invalid_argument if @p graph is null.
 */
VLINK_EXPORT GraphAwaiter await_graph(MessageLoop& loop, GraphTaskPtr graph);

/**
 * @brief Coroutine wrapper around @c MessageLoop::exec_task.
 *
 * @details
 * Posts @p callback under the given @c Schedule::Config envelope and suspends
 * until the callback has run.  Exceptions thrown by @p callback are stored in
 * an internal @c std::promise and rethrown from the awaiting @c co_await.  If
 * the underlying @c exec_task post fails (queue full or loop closed), the
 * coroutine resumes with an @c std::runtime_error carrying a descriptive
 * message.
 *
 * @tparam CallbackT  Callable returning @c void.
 * @param loop      Loop on which the callback is scheduled.
 * @param config    Schedule envelope (delay, priority, timeouts).
 * @param callback  Callable to execute on the loop thread.
 * @return @c Task<void> that completes after @p callback returns.
 */
template <typename CallbackT>
Task<void> exec(MessageLoop& loop, const Schedule::Config& config, CallbackT&& callback);

/**
 * @brief Awaits every @c Task<TypeT> in @p tasks and returns their results.
 *
 * @details
 * Each task is dispatched via @c co_spawn on @p loop.  Results are stored in
 * the order of the input vector.  @c when_all waits for every sub-task to
 * finish (success or failure) before completing.  If any sub-task throws, the
 * first exception observed is rethrown from the awaiting @c co_await with its
 * original type preserved; subsequent exceptions from sibling tasks are
 * dropped.
 *
 * @note @p TypeT must be default-constructible; the results vector is
 *       pre-sized via @c std::vector<TypeT>(count).
 *
 * @tparam TypeT  Result type produced by every sub-task.
 * @param loop   Loop on which sub-tasks are spawned and the caller resumes.
 * @param tasks  Sub-tasks to await; ownership is transferred into the call.
 * @return @c Task that resolves to a vector of results in input order.
 */
template <typename TypeT>
Task<std::vector<TypeT>> when_all(MessageLoop& loop, std::vector<Task<TypeT>> tasks);

/**
 * @brief Awaits every @c Task<void> in @p tasks to complete.
 *
 * @details
 * Waits for every sub-task to finish.  If any sub-task throws, the first
 * exception observed is rethrown from the awaiting @c co_await with its
 * original type preserved; subsequent exceptions are dropped.
 *
 * @param loop   Loop on which sub-tasks are spawned and the caller resumes.
 * @param tasks  Sub-tasks to await; ownership is transferred into the call.
 * @return @c Task<void> that completes once every sub-task has finished.
 */
VLINK_EXPORT Task<void> when_all(MessageLoop& loop, std::vector<Task<void>> tasks);

/**
 * @brief Records the index and result of the first @c Task<TypeT> to complete successfully.
 *
 * @details
 * The winner (first successful completion) is selected by an atomic
 * compare-exchange; losing tasks continue running until completion and their
 * results are dropped.  @c when_any does not return until every sub-task has
 * finished — this is required to avoid leaking orphaned sub-task frames, since
 * this layer offers no cross-coroutine cancellation.  If "abandon losers"
 * semantics with bounded latency are required, each sub-task itself must
 * respect a deadline.  If every sub-task throws, the first exception observed
 * is rethrown from the awaiting @c co_await.
 *
 * @tparam TypeT  Result type produced by every sub-task.
 * @param loop   Loop on which sub-tasks are spawned and the caller resumes.
 * @param tasks  Sub-tasks to race; ownership is transferred into the call.
 * @return @c Task resolving to @c {winning_index, winning_result}.
 * @throws std::invalid_argument if @p tasks is empty; observed at the awaiting
 *         @c co_await site (since @c when_any is itself a coroutine).
 */
template <typename TypeT>
Task<std::pair<size_t, TypeT>> when_any(MessageLoop& loop, std::vector<Task<TypeT>> tasks);

/**
 * @brief Records the index of the first @c Task<void> to complete successfully.
 *
 * @details
 * The winner (first successful completion) is selected by an atomic
 * compare-exchange; losing tasks continue running until completion.  @c when_any
 * does not return until every sub-task has finished — this is required to
 * avoid leaking orphaned sub-task frames (no cross-coroutine cancellation is
 * provided).  If every sub-task throws, the first exception observed is
 * rethrown from the awaiting @c co_await.
 *
 * @param loop   Loop on which sub-tasks are spawned and the caller resumes.
 * @param tasks  Sub-tasks to race; ownership is transferred into the call.
 * @return @c Task resolving to the winning sub-task's index.
 * @throws std::invalid_argument if @p tasks is empty; observed at the awaiting
 *         @c co_await site (since @c when_any is itself a coroutine).
 */
VLINK_EXPORT Task<size_t> when_any(MessageLoop& loop, std::vector<Task<void>> tasks);

/**
 * @brief Runs @p tasks sequentially, awaiting each one before starting the next.
 *
 * @details
 * Convenience for composing a linear pipeline of @c Task<void>.  Each task is
 * spawned on @p loop via the same runner+guard machinery used by @c when_all
 * and joined through @c await_future, so both the success path and any
 * exception thrown by a sub-task resume the caller on @p loop's thread.  If a
 * task throws, the exception is rethrown from the awaiting @c co_await with
 * its original type preserved and the remaining tasks are skipped.
 *
 * @param loop   Loop on which sub-tasks are spawned and the caller resumes.
 * @param tasks  Sub-tasks to run in order; ownership is transferred into the call.
 * @return @c Task<void> that completes once every sub-task has finished or one threw.
 */
VLINK_EXPORT Task<void> sequence(MessageLoop& loop, std::vector<Task<void>> tasks);

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

namespace detail {

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
inline bool FinalAwaiter::await_ready() const noexcept { return false; }

template <typename PromiseT>
inline std::coroutine_handle<> FinalAwaiter::await_suspend(std::coroutine_handle<PromiseT> handle) noexcept {
  auto next = handle.promise().continuation;
  return next ? next : std::noop_coroutine();
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
inline void FinalAwaiter::await_resume() const noexcept {}

template <typename TypeT>
inline std::suspend_always TaskPromiseBase<TypeT>::initial_suspend() noexcept {
  return {};
}

template <typename TypeT>
inline FinalAwaiter TaskPromiseBase<TypeT>::final_suspend() noexcept {
  return {};
}

template <typename TypeT>
inline void TaskPromiseBase<TypeT>::unhandled_exception() noexcept {
  exception = std::current_exception();
}

template <typename TypeT>
inline void* TaskPromise<TypeT>::operator new(size_t size) {
  return detail::allocate_frame(size);
}

template <typename TypeT>
inline void TaskPromise<TypeT>::operator delete(void* ptr, size_t size) noexcept {
  detail::deallocate_frame(ptr, size);
}

template <typename TypeT>
inline Task<TypeT> TaskPromise<TypeT>::get_return_object() noexcept {
  return Task<TypeT>{std::coroutine_handle<TaskPromise<TypeT>>::from_promise(*this)};
}

template <typename TypeT>
template <typename UpT>
inline void TaskPromise<TypeT>::return_value(UpT&& v) noexcept(std::is_nothrow_constructible_v<TypeT, UpT&&>) {
  result.emplace(std::forward<UpT>(v));
}

inline void* TaskPromise<void>::operator new(size_t size) { return detail::allocate_frame(size); }

inline void TaskPromise<void>::operator delete(void* ptr, size_t size) noexcept { detail::deallocate_frame(ptr, size); }

inline Task<void> TaskPromise<void>::get_return_object() noexcept {
  return Task<void>{std::coroutine_handle<TaskPromise<void>>::from_promise(*this)};
}

inline void TaskPromise<void>::return_void() noexcept {}

template <typename TypeT, typename CallbackT>
inline DetachedTask co_spawn_value_impl(Task<TypeT> task, CallbackT on_complete) {
  on_complete(co_await std::move(task));
}

template <typename CallbackT>
inline DetachedTask co_spawn_void_with_cb_impl(Task<void> task, CallbackT on_complete) {
  co_await std::move(task);
  on_complete();
}

}  // namespace detail

template <typename TypeT>
inline Task<TypeT>::Task(Handle handle) noexcept : handle_(handle) {}

template <typename TypeT>
inline Task<TypeT>::Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

template <typename TypeT>
inline Task<TypeT>& Task<TypeT>::operator=(Task&& other) noexcept {
  if VLIKELY (this != &other) {
    if (handle_) {
      handle_.destroy();
    }
    handle_ = std::exchange(other.handle_, {});
  }

  return *this;
}

template <typename TypeT>
inline Task<TypeT>::~Task() {
  if (handle_) {
    handle_.destroy();
  }
}

template <typename TypeT>
inline bool Task<TypeT>::valid() const noexcept {
  return handle_ != nullptr;
}

template <typename TypeT>
inline bool Task<TypeT>::done() const noexcept {
  return handle_ && handle_.done();
}

template <typename TypeT>
inline bool Task<TypeT>::await_ready() const noexcept {
  if VUNLIKELY (!handle_) {
    return true;
  }

  return handle_.done();
}

template <typename TypeT>
inline std::coroutine_handle<> Task<TypeT>::await_suspend(std::coroutine_handle<> awaiter) noexcept {
  handle_.promise().continuation = awaiter;
  return handle_;
}

template <typename TypeT>
inline TypeT Task<TypeT>::await_resume() {
  if VUNLIKELY (!handle_) {
    throw std::logic_error("vlink::Coroutine::Task::await_resume on invalid Task (moved-from or default-constructed)");
  }

  if VUNLIKELY (handle_.promise().exception) {
    std::rethrow_exception(handle_.promise().exception);
  }

  if constexpr (!std::is_void_v<TypeT>) {
    return std::move(*handle_.promise().result);
  }
}

template <typename TypeT>
inline typename Task<TypeT>::Handle Task<TypeT>::release() noexcept {
  return std::exchange(handle_, {});
}

template <typename TypeT>
inline typename Task<TypeT>::Handle Task<TypeT>::native_handle() const noexcept {
  return handle_;
}

template <typename TypeT>
struct FutureAwaiter<TypeT>::State final {
  explicit State(std::future<TypeT> future) noexcept : fut(std::move(future)) {}

  void set_handle(std::coroutine_handle<> h) {
    std::lock_guard lock(mtx);

    if (!abandoned.load(std::memory_order_acquire)) {
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

    if (h) {
      h.resume();
    }
  }

  void cancel_and_resume() {
    target_closed.store(true, std::memory_order_release);
    auto h = take_handle();

    if (h) {
      h.resume();
    }
  }

  std::future<TypeT> fut;
  std::mutex mtx;
  std::coroutine_handle<> handle;
  std::atomic_bool abandoned{false};
  std::atomic_bool target_closed{false};
};

template <typename TypeT>
inline FutureAwaiter<TypeT>::FutureAwaiter(MessageLoop* loop, std::future<TypeT> fut) noexcept
    : loop_(loop), state_(MemoryResource::make_shared<State>(std::move(fut))) {}

template <typename TypeT>
inline FutureAwaiter<TypeT>::~FutureAwaiter() {
  if (state_) {
    state_->abandoned.store(true, std::memory_order_release);
    state_->clear_handle();
  }
}

template <typename TypeT>
inline bool FutureAwaiter<TypeT>::await_ready() const noexcept {
  return !state_->fut.valid() || state_->fut.wait_for(std::chrono::nanoseconds::zero()) == std::future_status::ready;
}

template <typename TypeT>
inline void FutureAwaiter<TypeT>::await_suspend(std::coroutine_handle<> handle) {
  auto* loop_ptr = loop_;
  auto loop_alive = loop_ptr->get_alive_state();
  auto state = state_;
  state->set_handle(handle);

  detail::register_future_wait(
      MoveFunction<bool()>([loop_ptr, loop_alive, state = std::move(state), retry_count = 0U]() mutable -> bool {
        if VUNLIKELY (state->abandoned.load(std::memory_order_acquire)) {
          return true;
        }

        if (state->fut.valid() && state->fut.wait_for(std::chrono::nanoseconds::zero()) != std::future_status::ready) {
          return false;
        }

        const auto result = detail::post_callback_if_alive(
            loop_ptr, loop_alive, MoveFunction<void()>([state]() { state->resume_ready(); }),
            MoveFunction<void()>([state]() {
              detail::register_future_wait(MoveFunction<bool()>([state]() {
                if (!state->abandoned.load(std::memory_order_acquire)) {
                  state->cancel_and_resume();
                }

                return true;
              }));
            }));

        if (result == detail::ResumePostResult::kPosted) {
          return true;
        }

        if (result == detail::ResumePostResult::kRetry) {
          if (++retry_count >= detail::kMaxResumePostRetry) {
            detail::register_future_wait(MoveFunction<bool()>([state]() {
              if (!state->abandoned.load(std::memory_order_acquire)) {
                state->cancel_and_resume();
              }
              return true;
            }));
            return true;
          }

          return false;
        }

        return true;
      }));
}

template <typename TypeT>
inline TypeT FutureAwaiter<TypeT>::await_resume() {
  state_->abandoned.store(true, std::memory_order_release);
  state_->clear_handle();

  if VUNLIKELY (state_->target_closed.load(std::memory_order_acquire)) {
    throw Exception::OperationCancelled{};
  }

  if constexpr (std::is_void_v<TypeT>) {
    state_->fut.get();
  } else {
    return state_->fut.get();
  }
}

template <typename TypeT>
inline FutureAwaiter<TypeT> await_future(MessageLoop& loop, std::future<TypeT> fut) noexcept {
  return FutureAwaiter<TypeT>{&loop, std::move(fut)};
}

template <typename TypeT, typename CallbackT, typename>
inline void co_spawn(MessageLoop& loop, Task<TypeT>&& task, CallbackT&& on_complete) {
  co_spawn_with_priority<TypeT, CallbackT>(loop, std::move(task), std::forward<CallbackT>(on_complete), 0);
}

template <typename TypeT, typename CallbackT, typename>
inline void co_spawn_with_priority(MessageLoop& loop, Task<TypeT>&& task, CallbackT&& on_complete, uint16_t priority) {
  if constexpr (std::is_void_v<TypeT>) {
    auto detached = detail::co_spawn_void_with_cb_impl<std::decay_t<CallbackT>>(std::move(task),
                                                                                std::forward<CallbackT>(on_complete));
    auto handle = detached.handle;
    detached.handle = {};
    detail::co_spawn_detached_handle(loop, handle, priority);
  } else {
    auto detached = detail::co_spawn_value_impl<TypeT, std::decay_t<CallbackT>>(std::move(task),
                                                                                std::forward<CallbackT>(on_complete));
    auto handle = detached.handle;
    detached.handle = {};
    detail::co_spawn_detached_handle(loop, handle, priority);
  }
}

template <typename CallbackT>
inline Task<void> exec(MessageLoop& loop, const Schedule::Config& config, CallbackT&& callback) {
  auto promise_ptr = MemoryResource::make_shared<std::promise<void>>();
  auto fut = promise_ptr->get_future();

  auto status = loop.exec_task(config, [cb = std::forward<CallbackT>(callback), promise_ptr]() mutable {
    try {
      cb();
      promise_ptr->set_value();
    } catch (...) {
      promise_ptr->set_exception(std::current_exception());
    }
  });

  if VUNLIKELY (!status.is_valid()) {
    promise_ptr->set_exception(std::make_exception_ptr(std::runtime_error("Coroutine::exec: exec_task post failed")));
  }

  co_await await_future(loop, std::move(fut));

  co_return;
}

namespace detail {

template <typename TypeT>
struct WhenAllState final {
  std::vector<TypeT> results;
  std::atomic<size_t> remaining{0};
  std::mutex exc_mtx;
  std::exception_ptr first_exc;
  std::promise<void> promise;
};

template <typename TypeT>
struct WhenAllGuard final {
  std::shared_ptr<WhenAllState<TypeT>> state;
  bool completed_normally{false};

  explicit WhenAllGuard(std::shared_ptr<WhenAllState<TypeT>> s) noexcept : state(std::move(s)) {}

  WhenAllGuard(WhenAllGuard&& other) noexcept
      : state(std::move(other.state)), completed_normally(other.completed_normally) {}

  WhenAllGuard(const WhenAllGuard&) = delete;

  WhenAllGuard& operator=(const WhenAllGuard&) = delete;

  WhenAllGuard& operator=(WhenAllGuard&&) = delete;

  ~WhenAllGuard() {
    if (!state) {
      return;
    }

    if (!completed_normally) {
      std::lock_guard lock(state->exc_mtx);

      if (!state->first_exc) {
        state->first_exc = std::make_exception_ptr(
            std::runtime_error("vlink::Coroutine::when_all: sub-task aborted before completion"));
      }
    }

    if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      if (state->first_exc) {
        state->promise.set_exception(state->first_exc);
      } else {
        state->promise.set_value();
      }
    }
  }
};

template <typename TypeT>
inline Task<void> when_all_runner(Task<TypeT> task, size_t i, WhenAllGuard<TypeT> guard) {
  try {
    auto value = co_await std::move(task);
    guard.state->results[i] = std::move(value);
  } catch (...) {
    std::lock_guard lock(guard.state->exc_mtx);

    if (!guard.state->first_exc) {
      guard.state->first_exc = std::current_exception();
    }
  }

  guard.completed_normally = true;
}

template <typename TypeT>
struct WhenAnyState final {
  std::atomic_bool fired{false};
  std::atomic<size_t> remaining{0};
  std::mutex exc_mtx;
  std::exception_ptr first_exc;
  std::optional<std::pair<size_t, TypeT>> winner;
  std::promise<std::pair<size_t, TypeT>> promise;
};

template <typename TypeT>
struct WhenAnyGuard final {
  std::shared_ptr<WhenAnyState<TypeT>> state;
  bool completed_normally{false};

  explicit WhenAnyGuard(std::shared_ptr<WhenAnyState<TypeT>> s) noexcept : state(std::move(s)) {}

  WhenAnyGuard(WhenAnyGuard&& other) noexcept
      : state(std::move(other.state)), completed_normally(other.completed_normally) {}

  WhenAnyGuard(const WhenAnyGuard&) = delete;

  WhenAnyGuard& operator=(const WhenAnyGuard&) = delete;

  WhenAnyGuard& operator=(WhenAnyGuard&&) = delete;

  ~WhenAnyGuard() {
    if (!state) {
      return;
    }

    if (!completed_normally) {
      std::lock_guard lock(state->exc_mtx);

      if (!state->first_exc) {
        state->first_exc = std::make_exception_ptr(
            std::runtime_error("vlink::Coroutine::when_any: sub-task aborted before completion"));
      }
    }

    if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      if (state->winner) {
        state->promise.set_value(std::move(*state->winner));
      } else {
        state->promise.set_exception(state->first_exc);
      }
    }
  }
};

template <typename TypeT>
inline Task<void> when_any_runner(Task<TypeT> task, size_t i, WhenAnyGuard<TypeT> guard) {
  try {
    auto value = co_await std::move(task);
    bool expected = false;

    if (guard.state->fired.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      guard.state->winner.emplace(i, std::move(value));
    }
  } catch (...) {
    std::lock_guard lock(guard.state->exc_mtx);

    if (!guard.state->first_exc) {
      guard.state->first_exc = std::current_exception();
    }
  }

  guard.completed_normally = true;
}

}  // namespace detail

template <typename TypeT>
inline Task<std::vector<TypeT>> when_all(MessageLoop& loop, std::vector<Task<TypeT>> tasks) {
  static_assert(std::is_default_constructible_v<TypeT>,
                "Co::when_all<T>: T must be default-constructible (results vector pre-sized).");

  if VUNLIKELY (tasks.empty()) {
    co_return std::vector<TypeT>{};
  }

  const size_t count = tasks.size();
  auto state = MemoryResource::make_shared<detail::WhenAllState<TypeT>>();
  state->results.resize(count);
  state->remaining.store(count, std::memory_order_relaxed);
  auto fut = state->promise.get_future();

  for (size_t i = 0; i < count; ++i) {
    co_spawn(loop, detail::when_all_runner<TypeT>(std::move(tasks[i]), i, detail::WhenAllGuard<TypeT>{state}));
  }

  co_await await_future(loop, std::move(fut));

  co_return std::move(state->results);
}

template <typename TypeT>
inline Task<std::pair<size_t, TypeT>> when_any(MessageLoop& loop, std::vector<Task<TypeT>> tasks) {
  if VUNLIKELY (tasks.empty()) {
    throw std::invalid_argument("vlink::Coroutine::when_any: tasks must not be empty");
  }

  auto state = MemoryResource::make_shared<detail::WhenAnyState<TypeT>>();
  state->remaining.store(tasks.size(), std::memory_order_relaxed);
  auto fut = state->promise.get_future();

  for (size_t i = 0; i < tasks.size(); ++i) {
    co_spawn(loop, detail::when_any_runner<TypeT>(std::move(tasks[i]), i, detail::WhenAnyGuard<TypeT>{state}));
  }

  co_return co_await await_future(loop, std::move(fut));
}

}  // namespace Coroutine

/**
 * @namespace vlink::Co
 * @brief Short alias for @c vlink::Coroutine.
 *
 * @details
 * Lets user code write @c vlink::Co::Task / @c vlink::Co::yield(loop) etc.
 * @c vlink::Co and @c vlink::Coroutine refer to the same namespace.
 */
namespace Co = Coroutine;  // NOLINT(readability-identifier-naming)

}  // namespace vlink

#endif  // VLINK_ENABLE_COROUTINE
