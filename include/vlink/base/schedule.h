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
 * @file schedule.h
 * @brief Fluent task-scheduling wrapper used by @c MessageLoop::exec_task() and family.
 *
 * @details
 * @c vlink::Schedule is a non-instantiable utility struct that wraps a user callable in a
 * @c Config envelope and produces a @c Status (or @c RetStatus for bool-returning callbacks)
 * RAII handle.  The handle lets callers attach continuation callbacks in a fluent style,
 * including success/failure branches, exception handling, and scheduling/execution timeout
 * hooks.
 *
 * Categories of options carried by @c Schedule::Config:
 *
 * | Field                    | Purpose                                                                |
 * | ------------------------ | ---------------------------------------------------------------------- |
 * | @c delay_ms              | Wait before posting via a one-shot Timer.                              |
 * | @c priority              | Dispatch priority for priority-aware message loops.                    |
 * | @c schedule_timeout_ms   | Maximum queue-wait budget after the delay.  Triggered once when the    |
 * |                          | task is dequeued.  Drops the task on expiry.                           |
 * | @c execution_timeout_ms  | Maximum execution budget per callback in the chain.  Reported once     |
 * |                          | after each callback returns; never interrupts a running callback.      |
 *
 * Status surface exposed to the caller:
 *
 * @verbatim
 *   exec_task() -> Status / RetStatus
 *                    +--> on_schedule_timeout(cb)
 *                    +--> on_execution_timeout(cb)
 *                    +--> on_catch(cb)
 *                    +--> [RetStatus] on_then(cb)
 *                    +--> [RetStatus] on_else(cb)
 * @endverbatim
 *
 * @note
 * - @c Status is move-only and may be returned by value while it is being configured.
 * - The wrapped task is not posted to its dispatcher until the configuration handle is committed
 *   (when it is destroyed, i.e. the end of the fluent expression).  This guarantees that every
 *   continuation attached through the chain is registered before the task runs, so a fast
 *   dispatcher can never win a race against the caller and drop a continuation.
 * - Callers that store the handle instead of consuming it inline must call @c dispatch() to post
 *   the task immediately; otherwise it is posted only when the stored handle is destroyed.
 * - All continuation callbacks run on the dispatcher thread that ran the wrapped task.
 *
 * @par Example
 * @code
 * vlink::MessageLoop loop;
 * loop.async_run();
 *
 * loop.exec_task(vlink::Schedule::Config{100},
 *                [] { do_work(); })
 *     .on_execution_timeout([] { VLOG_W("slow"); });
 *
 * loop.exec_task(vlink::Schedule::Config{},
 *                []() -> bool { return try_connect(); })
 *     .on_then([] { start_session(); })
 *     .on_else([] { schedule_retry(); });
 * @endcode
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "./functional.h"
#include "./macros.h"

namespace vlink {

class MessageLoop;

/**
 * @struct Schedule
 * @brief Non-instantiable container for task-scheduling types and the @c process() entry points.
 *
 * @details
 * Provides the @c Config envelope, the @c Status / @c RetStatus handles, and the
 * @c process / @c process_with_ret static functions used by @c MessageLoop::exec_task().
 */
struct VLINK_EXPORT Schedule final {
  /**
   * @brief Callback signature for void tasks and lifecycle hooks.
   */
  using Callback = MoveFunction<void()>;

  /**
   * @brief Callback signature for tasks that return a boolean indicating success.
   */
  using RetCallback = MoveFunction<bool()>;

  /**
   * @brief Callback signature for exception handlers attached via @c on_catch().
   */
  using CatchCallback = MoveFunction<void(std::exception&)>;

  /**
   * @struct Config
   * @brief Scheduling parameters captured at the call to @c exec_task().
   *
   * @details
   * All fields default to zero, which corresponds to immediate dispatch with no timeouts.
   */
  struct VLINK_EXPORT Config final {
    /**
     * @brief Constructs a default @c Config with every field zero-initialised.
     */
    Config();

    /**
     * @brief Constructs a fully populated @c Config.
     *
     * @param _delay_ms              Delay before posting in milliseconds.
     * @param _priority              Dispatch priority for priority-aware loops.
     * @param _schedule_timeout_ms   Maximum queue-wait budget after the delay.
     * @param _execution_timeout_ms  Maximum execution budget per callback in the chain.
     */
    explicit Config(uint32_t _delay_ms, uint16_t _priority = 0, uint32_t _schedule_timeout_ms = 0,
                    uint32_t _execution_timeout_ms = 0);

    uint32_t delay_ms{0};              ///< Delay before posting; @c 0 posts immediately.
    uint16_t priority{0};              ///< Dispatch priority hint; higher fires sooner.
    uint32_t schedule_timeout_ms{0};   ///< Queue-wait budget after the delay; @c 0 disables.
    uint32_t execution_timeout_ms{0};  ///< Execution budget per callback; @c 0 disables.
  };

  /**
   * @class Status
   * @brief RAII handle returned by @c exec_task() when the wrapped callback returns @c void.
   *
   * @details
   * Holds a shared reference to the underlying task state.  Continuation callbacks may be attached
   * in any order before the configuration is committed; the wrapped task is not dispatched until
   * commit, which happens when the originating handle is destroyed (i.e. the end of the fluent
   * expression) or when @c dispatch() is called explicitly.  Every callback attached before commit
   * is therefore guaranteed to be registered before the task runs; attachments after commit are
   * ignored.  Committing does not cancel the task; it posts it.
   */
  class VLINK_EXPORT Status {
   public:
    /**
     * @brief Constructs a fresh handle backed by a newly allocated task state.
     */
    Status();

    /**
     * @brief Destroys the handle reference, committing the configuration so the task may dispatch.
     *
     * @details
     * Commits the pending configuration: once the originating handle is gone no further
     * continuation callbacks can be attached, so the dispatcher is released to run the task
     * with the full callback chain in place.  Does not cancel the task.
     */
    ~Status();

    Status(const Status&) = delete;

    Status& operator=(const Status&) = delete;

    /**
     * @brief Move-constructs from @p status, transferring its task state.
     *
     * @param status  Source handle to move from.
     */
    Status(Status&& status) noexcept;

    /**
     * @brief Move-assigns from @p status, transferring its task state.
     *
     * @param status  Source handle to move from.
     * @return Reference to @c *this.
     */
    Status& operator=(Status&& status) noexcept;

    /**
     * @brief Marks whether the handle's underlying task was successfully posted.
     *
     * @param valid  @c true once the task is queued.
     */
    void set_valid(bool valid);

    /**
     * @brief Reports whether the wrapped task was successfully posted.
     *
     * @return @c true when the handle refers to a queued task.
     */
    [[nodiscard]] bool is_valid() const;

    /**
     * @brief Commits the configuration immediately and reports whether the task was posted.
     *
     * @details
     * The task is otherwise posted when the handle is destroyed.  A caller that stores the handle
     * instead of consuming it inline calls @c dispatch() to post it now and learn the result
     * synchronously.  Attach all continuations first; idempotent with the destructor.
     *
     * @return @c true when the task was accepted by its dispatcher.
     */
    bool dispatch();

    /**
     * @brief Installs the callback fired when the task missed its scheduling deadline.
     *
     * @details
     * Triggered once when the task is dequeued and the elapsed time since posting exceeds
     * @c delay_ms + @c schedule_timeout_ms.  Only one schedule-timeout callback may be
     * registered; late registrations are dropped.
     *
     * @param callback  Hook invoked on the dispatcher thread.
     * @return Reference to @c *this for fluent chaining.
     */
    Status& on_schedule_timeout(Callback&& callback);

    /**
     * @brief Installs the callback fired when a chained callback exceeds @c execution_timeout_ms.
     *
     * @details
     * Triggered after each callback in the chain returns.  Only one execution-timeout
     * callback may be registered; late registrations are dropped.  Does not interrupt a
     * running callback.
     *
     * @param callback  Hook invoked on the dispatcher thread.
     * @return Reference to @c *this for fluent chaining.
     */
    Status& on_execution_timeout(Callback&& callback);

    /**
     * @brief Installs the callback fired when the task throws a @c std::exception.
     *
     * @details
     * Only @c std::exception-derived failures are caught; other exception types are
     * allowed to propagate.  Only one catch callback may be registered.
     *
     * @param callback  Hook receiving the caught exception.
     * @return Reference to @c *this for fluent chaining.
     */
    Status& on_catch(CatchCallback&& callback);

   protected:
    friend Schedule;
    friend MessageLoop;

    struct Impl final {
      std::atomic_bool is_valid{false};
      std::atomic_bool dispatched{false};
      std::recursive_mutex mtx;
      Callback schedule_timeout_callback;
      Callback execution_timeout_callback;
      CatchCallback catch_callback;
      Callback else_callback;
      std::vector<RetCallback> then_callback_list;
    };

    /**
     * @brief Runs the installed launcher exactly once, posting the wrapped task to its dispatcher.
     *
     * @details
     * Posting is deferred until the handle is committed so every fluent continuation is registered
     * before the task is enqueued.  The launcher lives in the handle (not the shared state), so the
     * post payload is released with the handle and forms no reference cycle.  Idempotent and safe on
     * a moved-from handle.
     */
    void commit() noexcept;

    std::shared_ptr<Impl> impl_;
    Callback launcher_;
    bool committed_{false};
  };

  /**
   * @class RetStatus
   * @brief RAII handle returned by @c exec_task() when the wrapped callback returns @c bool.
   *
   * @details
   * Extends @c Status with the @c on_then chain and the @c on_else fallback so callers can
   * express success/failure branches inline.
   */
  class VLINK_EXPORT RetStatus final : public Status {
   public:
    using Status::Status;

    /**
     * @brief Installs the callback fired when the wrapped task returns @c false.
     *
     * @details
     * Only one else callback may be registered; late registrations are dropped.
     *
     * @param callback  Hook invoked on the dispatcher thread when @c false is returned.
     * @return Reference to the base @c Status for further chaining.
     */
    Status& on_else(Callback&& callback);

    /**
     * @brief Appends a continuation that runs only when the previous callback returned @c true.
     *
     * @details
     * Multiple @c on_then callbacks may be chained.  Each is invoked in registration order
     * until one returns @c false, at which point the chain stops and the registered
     * @c on_else (if any) fires.
     *
     * @param callback  Continuation taking no arguments and returning @c bool.
     * @return Reference to @c *this for further chaining.
     */
    RetStatus& on_then(RetCallback&& callback);
  };

  Schedule() = delete;

  Schedule(const Schedule&) = delete;

  Schedule& operator=(const Schedule&) = delete;

  Schedule(Schedule&&) = delete;

  Schedule& operator=(Schedule&&) = delete;

  /**
   * @brief Wraps a void callback in a @c Config envelope and produces the task wrapper for the dispatcher.
   *
   * @details
   * Called internally by @c MessageLoop::exec_task().  Allocates the @c Status state and
   * fills @p wrapper_callback with the closure that the dispatcher will eventually run.
   *
   * @param config            Scheduling configuration.
   * @param callback          Void callable to execute.
   * @param wrapper_callback  Out parameter receiving the dispatcher-ready wrapper.
   * @return Fresh @c Status handle for fluent chaining.
   */
  [[nodiscard]] static Status process(const Config& config, Callback&& callback, Callback& wrapper_callback);

  /**
   * @brief Wraps a bool-returning callback in a @c Config envelope and produces the task wrapper for the dispatcher.
   *
   * @details
   * Called internally by @c MessageLoop::exec_task().  Allocates the @c RetStatus state and
   * fills @p wrapper_callback with the closure that the dispatcher will eventually run.
   *
   * @param config            Scheduling configuration.
   * @param callback          Bool-returning callable to execute.
   * @param wrapper_callback  Out parameter receiving the dispatcher-ready wrapper.
   * @return Fresh @c RetStatus handle for fluent chaining.
   */
  [[nodiscard]] static RetStatus process_with_ret(const Config& config, RetCallback&& callback,
                                                  Callback& wrapper_callback);

 private:
  static RetStatus internal_process_with_ret(const Config& config, RetCallback&& callback, Callback& wrapper_callback);
};

}  // namespace vlink
