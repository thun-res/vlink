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
 * @file context.h
 * @brief Runtime execution context ABI (@c LauncherContext) injected into Launcher components.
 *
 * @details
 * Exposes typed parameter lookups, scheduler @c MessageLoop / @c ThreadPool handles, cooperative
 * cancellation, URL resolution, lifecycle active-state queries, readiness/health/watchdog hooks,
 * unload-guarded task/timer/schedule/coroutine helpers, and host extension queries.
 *
 * @c LauncherContext maintains a strictly C++17-invariant virtual method table across
 * @c VLINK_ENABLE_COROUTINE build configurations.  To avoid pulling pub/sub and serialisation
 * headers into components or hosts that do not open ports directly, @c LauncherPublisher and
 * @c LauncherSubscriber are forward-declared here; include @c launcher/port.h or the umbrella
 * header @c launcher_component.h when calling @c create_publisher() or @c create_subscriber().
 *
 * @see param.h, port.h, component.h, launcher_component.h
 */

#pragma once

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#ifdef VLINK_ENABLE_COROUTINE
#include <exception>
#include <future>
#endif
#include <locale>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "../../base/cancellation.h"
#include "../../base/coroutine.h"
#include "../../base/functional.h"
#include "../../base/logger.h"
#include "../../base/macros.h"
#include "../../base/memory_resource.h"
#include "../../base/message_loop.h"
#include "../../base/schedule.h"
#include "../../base/task_handle.h"
#include "../../base/thread_pool.h"
#include "../../base/timer.h"
#include "./param.h"

namespace vlink {

template <typename MsgT>
class LauncherPublisher;

template <typename MsgT>
class LauncherSubscriber;

#ifdef VLINK_ENABLE_COROUTINE
/**
 * @struct LauncherCoroutineOps
 * @brief Host-resident C++20 coroutine dispatch table queried via @c LauncherContext::query_extension().
 *
 * @details
 * Allows a C++20-enabled host to keep outer coroutine wrapper frames resident in the host
 * binary (preventing use-after-free during dynamic @c dlclose) without adding conditional
 * virtual slots to @c LauncherContext.
 */
struct LauncherCoroutineOps final {
  /**
   * @brief Extension lookup identifier.
   */
  static constexpr std::string_view kExtensionId = "vlink.launcher.coroutine.v1";

  bool (*spawn)(void* host_ctx, Co::Task<void>&& task, uint16_t priority){nullptr};  ///< Host coroutine spawner hook.
  Co::Task<bool> (*sleep)(void* host_ctx, uint32_t ms, uint16_t priority){nullptr};  ///< Host stop-aware sleep hook.
  Co::Task<void> (*await_stop)(void* host_ctx, uint16_t priority){nullptr};          ///< Host stop-signal awaiter hook.
  void* host_ctx{nullptr};                                                           ///< Opaque host context pointer.
};
#endif

/**
 * @class LauncherContext
 * @brief Runtime execution context injected into each @c LauncherComponent instance.
 *
 * @details
 * Exposes typed parameter lookups (with scalar string coercion for CLI overrides), the
 * component's assigned @c MessageLoop and optional @c ThreadPool, a cooperative
 * @c CancellationToken, URL remapping and managed pub/sub factories, readiness signalling,
 * health and watchdog keepalive hooks, unload-guarded task/timer/schedule/coroutine helpers,
 * and per-component CPU utilisation.  The virtual method table is strictly C++17-invariant
 * across @c VLINK_ENABLE_COROUTINE build configurations.  The host keeps the context valid
 * from @c on_configure() through @c on_cleanup().
 */
class LauncherContext {
 public:
  /**
   * @brief Virtual destructor.
   */
  virtual ~LauncherContext() = default;

  /**
   * @brief Returns the unique instance name assigned by the host.
   *
   * @return Component instance name.
   */
  [[nodiscard]] virtual const std::string& name() const noexcept = 0;

  /**
   * @brief Returns the logical namespace prefix assigned to this component.
   *
   * @return Namespace string (may be empty).
   */
  [[nodiscard]] virtual const std::string& ns() const noexcept = 0;

  /**
   * @brief Returns the scheduler group name hosting this component.
   *
   * @return Scheduler group identifier.
   */
  [[nodiscard]] virtual const std::string& scheduler_group() const noexcept = 0;

  /**
   * @brief Returns the @c MessageLoop assigned to dispatch this component's work.
   *
   * @return Reference to an active @c MessageLoop owned by the host scheduler.
   */
  [[nodiscard]] virtual MessageLoop& loop() noexcept = 0;

  /**
   * @brief Returns the @c ThreadPool associated with this component, if any.
   *
   * @details
   * Non-null when the component's @c compute_pool or assigned @c scheduler_group is a
   * @c mode="pool" scheduler group; @c nullptr otherwise.
   *
   * @return Pointer to an active @c ThreadPool, or @c nullptr.
   */
  [[nodiscard]] virtual ThreadPool* thread_pool() noexcept = 0;

  /**
   * @brief Returns a cooperative cancellation token signalled when the component is stopping.
   *
   * @return Cancellation token bound to this component's run lifecycle.
   */
  [[nodiscard]] virtual CancellationToken cancellation_token() const noexcept = 0;

  /**
   * @brief Resolves a logical VLink URL through this component's namespace and remap rules.
   *
   * @param url  Input VLink URL.
   * @return     Remapped URL when a rule matches; otherwise @p url (namespace-resolved if relative).
   */
  [[nodiscard]] virtual std::string resolve_url(std::string_view url) const = 0;

  /**
   * @brief Returns all resolved named URLs declared for this component (@c urls, @c inputs, and @c outputs).
   *
   * @return Map from logical endpoint name to resolved VLink URL.
   */
  [[nodiscard]] virtual std::unordered_map<std::string, std::string> urls() const;

  /**
   * @brief Returns all resolved input (subscribed) URLs declared in @c [component.inputs].
   *
   * @return Map from logical input name to resolved VLink URL.
   */
  [[nodiscard]] virtual std::unordered_map<std::string, std::string> input_urls() const;

  /**
   * @brief Returns all resolved output (published) URLs declared in @c [component.outputs].
   *
   * @return Map from logical output name to resolved VLink URL.
   */
  [[nodiscard]] virtual std::unordered_map<std::string, std::string> output_urls() const;

  /**
   * @brief Looks up a declared URL by logical name (searching @c urls, @c inputs, then @c outputs).
   *
   * @param key          Logical endpoint name declared in configuration.
   * @param default_url  Fallback VLink URL when @p key is undeclared (passed through @c resolve_url()).
   * @return             Resolved VLink URL, or empty string if neither declared nor provided.
   */
  [[nodiscard]] virtual std::string get_url(std::string_view key, std::string_view default_url = "") const;

  /**
   * @brief Looks up a declared input URL by logical name (searching @c inputs, then @c urls).
   *
   * @param key          Logical input name declared in @c [component.inputs].
   * @param default_url  Fallback VLink URL when @p key is undeclared.
   * @return             Resolved VLink URL, or empty string if neither declared nor provided.
   */
  [[nodiscard]] virtual std::string get_input_url(std::string_view key, std::string_view default_url = "") const;

  /**
   * @brief Looks up a declared output URL by logical name (searching @c outputs, then @c urls).
   *
   * @param key          Logical output name declared in @c [component.outputs].
   * @param default_url  Fallback VLink URL when @p key is undeclared.
   * @return             Resolved VLink URL, or empty string if neither declared nor provided.
   */
  [[nodiscard]] virtual std::string get_output_url(std::string_view key, std::string_view default_url = "") const;

  /**
   * @brief Returns a snapshot of all parameters currently bound to this component.
   *
   * @return Copy of the flat dot-delimited parameter map.
   */
  [[nodiscard]] virtual LauncherParamMap params() const = 0;

  /**
   * @brief Reports whether a parameter with key @p key exists on this component.
   *
   * @param key  Dot-delimited parameter key.
   * @return     @c true when the parameter is present.
   */
  [[nodiscard]] virtual bool has_param(std::string_view key) const = 0;

  /**
   * @brief Looks up a single parameter by dot-delimited key.
   *
   * @param key  Parameter key.
   * @param out  Receives the parameter value when found.
   * @return     @c true when the parameter exists.
   */
  [[nodiscard]] virtual bool get_param(std::string_view key, LauncherParamValue& out) const = 0;

  /**
   * @brief Reports whether this component is currently in the active lifecycle state.
   *
   * @details
   * Becomes @c true immediately before @c on_activate() is invoked (allowing initial messages
   * during activation), reverts to @c false if @c on_activate() fails, and becomes @c false
   * immediately upon entering deactivation or stop.
   *
   * @return @c true when active; @c false while in inactive standby or stopping.
   */
  [[nodiscard]] virtual bool is_active() const noexcept;

  /**
   * @brief Returns a shared atomic view of this component's active lifecycle flag, if host-backed.
   *
   * @details
   * Used by @c LauncherPublisher and @c LauncherSubscriber so copied handles remain
   * lifetime-safe even if they outlive the component context.
   *
   * @return Shared pointer to the host-owned atomic active flag, or @c nullptr for custom contexts.
   */
  [[nodiscard]] virtual std::shared_ptr<const std::atomic_bool> active_flag() const noexcept;

  /**
   * @brief Creates a lifecycle-gated, shared-ownership @c LauncherPublisher for an output port.
   *
   * @details
   * Resolves @p port_key via @c get_output_url(), wraps @c vlink::Publisher\<MsgT\> so
   * publishes are suppressed while @c is_active() is @c false, and integrates active
   * @c record_path() and dynamic @c ctl @c record updates.
   *
   * @tparam MsgT        Published message type.
   * @param port_key     Logical output port key from @c [component.outputs] (falls back to @c urls).
   * @param default_url  Fallback VLink URL when @p port_key is undeclared.
   * @return             Valid @c LauncherPublisher\<MsgT\> handle, or invalid if URL is empty or @c init() fails.
   *
   * @note Include @c launcher/port.h or @c launcher_component.h to instantiate this template.
   */
  template <typename MsgT>
  [[nodiscard]] LauncherPublisher<MsgT> create_publisher(std::string_view port_key,
                                                         std::string_view default_url = "") const;

  /**
   * @brief Creates a lifecycle-gated, unload-safe @c LauncherSubscriber for an input port.
   *
   * @details
   * Resolves @p port_key via @c get_input_url(), enables @c set_safety_quit(true), attaches
   * the subscriber to @c loop() when @p attach_loop is @c true (serialising delivery with
   * lifecycle and parameter hooks on single-thread modes @c dedicated / @c shared / @c pinned;
   * @c multi mode dispatches concurrently across worker threads), gates delivery on
   * @c is_active() and @c acquire_task_guard(), scopes component log attribution and CPU
   * profiling, verifies @c listen(), and integrates active @c record_path() updates.
   *
   * @tparam MsgT        Subscribed message type.
   * @tparam CallbackT   Callable invoked as @c void(const MsgT&).
   * @param port_key     Logical input port key from @c [component.inputs] (falls back to @c urls).
   * @param callback     Message handler invoked while the component is active.
   * @param default_url  Fallback VLink URL when @p port_key is undeclared.
   * @param attach_loop  When @c true, attaches subscriber delivery to @c loop().
   * @return             Valid @c LauncherSubscriber\<MsgT\> handle, or invalid on empty URL, @c init(), or @c listen()
   * failure.
   *
   * @note Include @c launcher/port.h or @c launcher_component.h to instantiate this template.
   */
  template <typename MsgT, typename CallbackT>
  [[nodiscard]] LauncherSubscriber<MsgT> create_subscriber(std::string_view port_key, CallbackT&& callback,
                                                           std::string_view default_url = "", bool attach_loop = true);

  /**
   * @brief Returns the active bag recording file path configured for this component, if any.
   *
   * @return Resolved bag path (@c .vdb / @c .vdbx / @c .vcap / @c .vcapx), or empty string when disabled.
   */
  [[nodiscard]] virtual std::string record_path() const;

  /**
   * @brief Reports whether managed publishers should force serialisation when recording without subscribers.
   *
   * @return @c true when forced publish is active.
   */
  [[nodiscard]] virtual bool record_force_publish() const noexcept;

  /**
   * @brief Returns a shared atomic mirror of @c record_force_publish() for @c LauncherPublisher.
   *
   * @return Shared atomic boolean flag, or @c nullptr if unused.
   */
  [[nodiscard]] virtual std::shared_ptr<const std::atomic_bool> record_force_publish_flag() const noexcept;

  /**
   * @brief Registers a managed-endpoint callback notified when runtime @c ctl @c record toggles recording.
   *
   * @details
   * If a non-empty @c record_path() is already active at registration time, @p hook is invoked
   * once synchronously before returning.  Returning @c false signals that the endpoint expired
   * and may be pruned.
   *
   * @param hook  Callback receiving the new bag path (empty disables recording); returns @c false when expired.
   */
  virtual void register_record_hook(Function<bool(const std::string&)>&& hook) const;

  /**
   * @brief Registers a teardown hook invoked before this context is destroyed during component cleanup.
   *
   * @details
   * Used by @c create_subscriber() so any subscriber handle that outlives the component instance
   * is cleanly de-initialised before the host destroys @c LauncherContext.
   *
   * @param hook  Teardown callback executed on cleanup.
   */
  virtual void register_cleanup_hook(Function<void()>&& hook) const;

  /**
   * @brief Notifies the host orchestrator that this component is ready to serve dependents.
   *
   * @details
   * Safe to call from any thread; idempotent after the first call per start cycle.
   */
  virtual void mark_ready() noexcept = 0;

  /**
   * @brief Reports a non-fatal health state or degraded detail string to the host.
   *
   * @param healthy  @c true when healthy; @c false when degraded.
   * @param detail   Optional diagnostic message.
   */
  virtual void report_health(bool healthy, std::string_view detail = "") noexcept = 0;

  /**
   * @brief Refreshes this component's software watchdog timestamp.
   *
   * @details
   * Components configured with @c watchdog_ms @c > @c 0 should call @c watchdog_ping()
   * periodically (for example inside @c on_tick() or a coroutine loop) to prove liveness.
   */
  virtual void watchdog_ping() noexcept = 0;

  /**
   * @brief Returns recent active CPU utilisation percentage measured for this component.
   *
   * @return Utilisation percentage, or @c 0.0 when profiling has no samples yet.
   */
  [[nodiscard]] virtual double get_cpu_usage() const noexcept = 0;

  /**
   * @brief Posts a tracked task onto the component's assigned @c MessageLoop.
   *
   * @details
   * Automatically links the component's @c CancellationToken and holds an unload task guard
   * while the task runs.
   *
   * @param task      Callable to run on the assigned loop.
   * @param priority  Optional dispatch priority for @c kPriorityType loops.
   * @return          @c true when the task was accepted.
   */
  virtual bool post_task(MessageLoop::Callback&& task, uint16_t priority = MessageLoop::kNoPriority) = 0;

  /**
   * @brief Posts a tracked task and returns a @c TaskHandle for cancellation or completion waits.
   *
   * @param task      Callable to run on the assigned loop.
   * @param priority  Optional dispatch priority for @c kPriorityType loops.
   * @param options   Optional overflow, drop, and cancellation overrides.
   * @return          Observable @c TaskHandle bound to the posted task.
   */
  [[nodiscard]] virtual TaskHandle post_task_handle(MessageLoop::Callback&& task,
                                                    uint16_t priority = MessageLoop::kNoPriority,
                                                    const PostTaskOptions& options = {}) = 0;

  /**
   * @brief Offloads a tracked parallel compute task to @c thread_pool() (falling back to @c loop()).
   *
   * @param task     Callable to run on a worker thread.
   * @param options  Optional overflow, drop, and cancellation overrides.
   * @return         Observable @c TaskHandle bound to the submitted task.
   */
  [[nodiscard]] virtual TaskHandle post_compute_task(ThreadPool::Callback&& task,
                                                     const PostTaskOptions& options = {}) = 0;

  /**
   * @brief Acquires an RAII lifetime guard preventing hot-unload until released.
   *
   * @details
   * Returns @c nullptr once the component has entered shutdown, allowing timers, scheduled
   * chains, and coroutines to avoid starting new work after @c on_stop() begins.
   *
   * @return Opaque refcounted lifetime guard, or @c nullptr when shutting down.
   */
  [[nodiscard]] virtual std::shared_ptr<void> acquire_task_guard() noexcept = 0;

  /**
   * @brief Enters a host-tracked callback execution scope (log attribution and CPU profiling).
   *
   * @details
   * Called by header-only wrappers (@c create_subscriber(), @c create_timer(), @c exec_task())
   * via RAII so logs emitted inside callbacks carry the component name and CPU usage is
   * attributed safely even under concurrent or nested invocations.  Must be paired with
   * @c exit_callback_scope().
   *
   * @return Opaque token passed verbatim to @c exit_callback_scope().
   */
  [[nodiscard]] virtual uintptr_t enter_callback_scope() const noexcept;

  /**
   * @brief Leaves a host-tracked callback execution scope started by @c enter_callback_scope().
   *
   * @param token  Opaque token returned by the matching @c enter_callback_scope() call.
   */
  virtual void exit_callback_scope(uintptr_t token) const noexcept;

  /**
   * @brief Queries an optional ABI-stable host extension table by identifier.
   *
   * @details
   * Keeps the @c LauncherContext vtable strictly C++17-invariant while allowing C++20-enabled
   * hosts to expose host-resident operations (such as @c LauncherCoroutineOps).
   *
   * @param extension_id  Versioned extension identifier string.
   * @return              Pointer to the host-owned extension table, or @c nullptr if unsupported.
   */
  [[nodiscard]] virtual const void* query_extension(std::string_view extension_id) const noexcept;

  /**
   * @brief Creates a @c vlink::Timer attached to @c loop() and guarded against unload.
   *
   * @param interval_ms  Tick interval in milliseconds.
   * @param callback     Callback invoked on each tick while the component is active.
   * @param loop_count   Number of ticks; @c Timer::kInfinite repeats until stopped.
   * @param priority     Dispatch priority for @c kPriorityType loops.
   * @return             Owned @c Timer instance (caller must call @c start() and @c stop()).
   */
  [[nodiscard]] std::unique_ptr<Timer> create_timer(uint32_t interval_ms, Timer::Callback&& callback,
                                                    int32_t loop_count = Timer::kInfinite,
                                                    uint16_t priority = MessageLoop::kTimerPriority);

  /**
   * @brief Schedules a @c void-returning task via @c MessageLoop::exec_task() guarded against unload.
   *
   * @details
   * Holds an unload task guard across the @c Schedule execution so the component stays alive
   * until completion.  Returns an invalid @c Schedule::Status without posting if the component
   * is already stopping; skips @p callback if cancelled before dispatch.
   *
   * @tparam CallbackT  Callable returning @c void.
   * @param config      Scheduling envelope (delay, priority, schedule/execution timeouts).
   * @param callback    Callable to schedule.
   * @return            Chainable @c Schedule::Status handle (@c is_valid() is @c false if stopping).
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename CallbackT, typename = std::enable_if_t<!std::is_convertible_v<CallbackT, Schedule::RetCallback>>>
  Schedule::Status exec_task(const Schedule::Config& config, CallbackT&& callback);

  /**
   * @brief Schedules a @c bool-returning task via @c MessageLoop::exec_task() guarded against unload.
   *
   * @details
   * Holds an unload task guard across the full @c Schedule execution chain so the component
   * stays alive until completion.  Returns an invalid @c Schedule::RetStatus without posting if
   * the component is already stopping; if cancelled before execution, skips @p callback and
   * returns @c false (which triggers any registered @c on_else() continuation).
   *
   * @tparam CallbackT  Callable returning @c bool.
   * @param config      Scheduling envelope.
   * @param callback    Callable to schedule.
   * @return            Chainable @c Schedule::RetStatus handle (@c is_valid() is @c false if stopping).
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename CallbackT, typename = std::enable_if_t<std::is_convertible_v<CallbackT, Schedule::RetCallback>>>
  Schedule::RetStatus exec_task(const Schedule::Config& config, CallbackT&& callback);

#ifdef VLINK_ENABLE_COROUTINE
  /**
   * @brief Spawns a lifetime-guarded C++20 coroutine onto the component's assigned @c MessageLoop.
   *
   * @details
   * Dispatches through @c LauncherCoroutineOps when the host provides C++20 resident coroutine
   * frames, otherwise runs the inline guarded coroutine on @c loop().  Returns @c false without
   * starting if the loop is not running or the component is already stopping.
   *
   * @param task      Coroutine task to launch.
   * @param priority  Optional dispatch priority for @c kPriorityType loops.
   * @return          @c true when the coroutine was spawned.
   */
  bool co_spawn(Co::Task<void>&& task, uint16_t priority = MessageLoop::kNoPriority);

  /**
   * @brief Suspends a coroutine for @p ms milliseconds on the component's loop, reporting cancellation.
   *
   * @param ms        Sleep duration in milliseconds.
   * @param priority  Optional resume priority for @c kPriorityType loops.
   * @return          @c true when the full interval elapsed without cancellation; @c false if stopping.
   */
  [[nodiscard]] Co::Task<bool> co_sleep(uint32_t ms, uint16_t priority = MessageLoop::kNoPriority);

  /**
   * @brief Suspends a coroutine without polling until this component's @c CancellationToken fires.
   *
   * @param priority  Reserved parameter kept for API symmetry with @c co_sleep().
   * @return          Awaitable task that resolves once @c on_stop() cancels the token.
   */
  [[nodiscard]] Co::Task<void> co_await_stop(uint16_t priority = MessageLoop::kNoPriority);

  /**
   * @brief Awaits a scheduled callable on the component's @c MessageLoop using @c vlink::Co::exec().
   *
   * @details
   * Does not acquire a separate unload task guard; @c co_await it inside a guarded coroutine
   * started via @c on_coro_run() or @c co_spawn().
   *
   * @tparam CallbackT  Callable executed under @p config.
   * @param config      Scheduling envelope (delay, priority, timeouts).
   * @param callback    Callable to execute.
   * @return            Coroutine task yielding the callable's result.
   */
  template <typename CallbackT>
  [[nodiscard]] auto co_exec(const Schedule::Config& config, CallbackT&& callback);
#endif

  /**
   * @brief Reads a boolean parameter (coercing @c "true" / @c "false" / @c "1" / @c "0") with fallback default.
   *
   * @param key            Dot-delimited parameter key.
   * @param default_value  Value returned when missing or unparseable.
   * @return               Parameter value or @p default_value.
   */
  [[nodiscard]] bool get_param_bool(std::string_view key, bool default_value = false) const;

  /**
   * @brief Reads a 64-bit signed integer parameter (coercing decimal strings) with fallback default.
   *
   * @param key            Dot-delimited parameter key.
   * @param default_value  Value returned when missing or unparseable.
   * @return               Parameter value or @p default_value.
   */
  [[nodiscard]] int64_t get_param_int(std::string_view key, int64_t default_value = 0) const;

  /**
   * @brief Reads a double-precision parameter (promoting integers and numeric strings) with fallback default.
   *
   * @param key            Dot-delimited parameter key.
   * @param default_value  Value returned when missing or unparseable.
   * @return               Parameter value or @p default_value.
   */
  [[nodiscard]] double get_param_double(std::string_view key, double default_value = 0.0) const;

  /**
   * @brief Reads a string parameter with fallback default.
   *
   * @param key            Dot-delimited parameter key.
   * @param default_value  Value returned when missing or type-mismatched.
   * @return               Parameter value or @p default_value.
   */
  [[nodiscard]] std::string get_param_string(std::string_view key, std::string_view default_value = "") const;

  /**
   * @brief Reads a boolean list parameter with fallback default.
   *
   * @param key            Dot-delimited parameter key.
   * @param default_value  Fallback list returned when missing or mismatched.
   * @return               Resolved boolean list or @p default_value.
   */
  [[nodiscard]] std::vector<bool> get_param_bool_list(std::string_view key,
                                                      const std::vector<bool>& default_value = {}) const;

  /**
   * @brief Reads an integer list parameter with fallback default.
   *
   * @param key            Dot-delimited parameter key.
   * @param default_value  Fallback list returned when missing or mismatched.
   * @return               Resolved integer list or @p default_value.
   */
  [[nodiscard]] std::vector<int64_t> get_param_int_list(std::string_view key,
                                                        const std::vector<int64_t>& default_value = {}) const;

  /**
   * @brief Reads a double list parameter (promoting integer lists) with fallback default.
   *
   * @param key            Dot-delimited parameter key.
   * @param default_value  Fallback list returned when missing or mismatched.
   * @return               Resolved double list or @p default_value.
   */
  [[nodiscard]] std::vector<double> get_param_double_list(std::string_view key,
                                                          const std::vector<double>& default_value = {}) const;

  /**
   * @brief Reads a string list parameter with fallback default.
   *
   * @param key            Dot-delimited parameter key.
   * @param default_value  Fallback list returned when missing or mismatched.
   * @return               Resolved string list or @p default_value.
   */
  [[nodiscard]] std::vector<std::string> get_param_string_list(
      std::string_view key, const std::vector<std::string>& default_value = {}) const;

 protected:
  LauncherContext() = default;

 private:
  struct CallbackScope final {
    explicit CallbackScope(const LauncherContext* ctx) noexcept;

    ~CallbackScope();

    CallbackScope(const CallbackScope&) = delete;
    CallbackScope& operator=(const CallbackScope&) = delete;

   private:
    const LauncherContext* ctx_{nullptr};
    uintptr_t token_{0U};
  };

#ifdef VLINK_ENABLE_COROUTINE
  static Co::Task<void> run_guarded_coro(std::shared_ptr<void> guard, Co::Task<void> task);

  static Co::Task<bool> co_sleep_impl(MessageLoop* target_loop, CancellationToken token, uint32_t ms,
                                      uint16_t priority);

  static Co::Task<void> co_await_stop_impl(MessageLoop* target_loop, CancellationToken token, uint16_t priority);
#endif

  VLINK_DISALLOW_COPY_AND_ASSIGN(LauncherContext)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline std::unordered_map<std::string, std::string> LauncherContext::urls() const { return {}; }

inline std::unordered_map<std::string, std::string> LauncherContext::input_urls() const { return {}; }

inline std::unordered_map<std::string, std::string> LauncherContext::output_urls() const { return {}; }

inline std::string LauncherContext::get_url(std::string_view key, std::string_view default_url) const {
  (void)key;
  return default_url.empty() ? std::string() : resolve_url(default_url);
}

inline std::string LauncherContext::get_input_url(std::string_view key, std::string_view default_url) const {
  return get_url(key, default_url);
}

inline std::string LauncherContext::get_output_url(std::string_view key, std::string_view default_url) const {
  return get_url(key, default_url);
}

inline bool LauncherContext::is_active() const noexcept { return true; }

inline std::shared_ptr<const std::atomic_bool> LauncherContext::active_flag() const noexcept { return nullptr; }

inline std::string LauncherContext::record_path() const { return {}; }

inline bool LauncherContext::record_force_publish() const noexcept { return false; }

inline std::shared_ptr<const std::atomic_bool> LauncherContext::record_force_publish_flag() const noexcept {
  return nullptr;
}

inline void LauncherContext::register_record_hook(Function<bool(const std::string&)>&& hook) const { (void)hook; }

inline void LauncherContext::register_cleanup_hook(Function<void()>&& hook) const { (void)hook; }

inline uintptr_t LauncherContext::enter_callback_scope() const noexcept { return 0U; }

inline void LauncherContext::exit_callback_scope(uintptr_t token) const noexcept { (void)token; }

inline const void* LauncherContext::query_extension(std::string_view extension_id) const noexcept {
  (void)extension_id;
  return nullptr;
}

inline std::unique_ptr<Timer> LauncherContext::create_timer(uint32_t interval_ms, Timer::Callback&& callback,
                                                            int32_t loop_count, uint16_t priority) {
  auto token = cancellation_token();
  auto active = active_flag();
  auto wrapped = [this, token = std::move(token), active = std::move(active), cb = std::move(callback)]() mutable {
    if VUNLIKELY (token.is_cancellation_requested()) {
      return;
    }

    if VUNLIKELY (active != nullptr ? !active->load(std::memory_order_acquire) : !is_active()) {
      return;
    }

    auto guard = acquire_task_guard();

    if VUNLIKELY (!guard || !cb) {
      return;
    }

    CallbackScope scope(this);
    cb();
  };
  auto timer = std::make_unique<Timer>(&loop(), interval_ms, loop_count, std::move(wrapped));
  timer->set_priority(priority);
  return timer;
}

template <typename CallbackT, typename>
inline Schedule::Status LauncherContext::exec_task(const Schedule::Config& config, CallbackT&& callback) {
  auto guard = acquire_task_guard();

  if VUNLIKELY (!guard) {
    Schedule::Status invalid;
    invalid.set_valid(false);
    return invalid;
  }

  auto token = cancellation_token();
  return loop().exec_task(config, [this, token = std::move(token), guard = std::move(guard),
                                   cb = std::forward<CallbackT>(callback)]() mutable {
    (void)guard;

    if VUNLIKELY (token.is_cancellation_requested()) {
      return;
    }

    CallbackScope scope(this);
    cb();
  });
}

template <typename CallbackT, typename>
inline Schedule::RetStatus LauncherContext::exec_task(const Schedule::Config& config, CallbackT&& callback) {
  auto guard = acquire_task_guard();

  if VUNLIKELY (!guard) {
    Schedule::RetStatus invalid;
    invalid.set_valid(false);
    return invalid;
  }

  auto token = cancellation_token();
  return loop().exec_task(config,
                          [this, token = std::move(token), guard = std::move(guard),
                           cb = std::forward<CallbackT>(callback)]() mutable -> bool {
                            (void)guard;

                            if VUNLIKELY (token.is_cancellation_requested()) {
                              return false;
                            }

                            CallbackScope scope(this);
                            return cb();
                          });
}

#ifdef VLINK_ENABLE_COROUTINE
inline bool LauncherContext::co_spawn(Co::Task<void>&& task, uint16_t priority) {
  const auto* ops = static_cast<const LauncherCoroutineOps*>(query_extension(LauncherCoroutineOps::kExtensionId));

  if VLIKELY (ops != nullptr && ops->spawn != nullptr) {
    return ops->spawn(ops->host_ctx, std::move(task), priority);
  }

  if VUNLIKELY (!loop().is_running() || loop().is_ready_to_quit()) {
    return false;
  }

  auto guard = acquire_task_guard();

  if VUNLIKELY (!guard) {
    return false;
  }

  Co::co_spawn_with_priority(loop(), run_guarded_coro(std::move(guard), std::move(task)), priority);
  return true;
}

inline Co::Task<bool> LauncherContext::co_sleep(uint32_t ms, uint16_t priority) {
  const auto* ops = static_cast<const LauncherCoroutineOps*>(query_extension(LauncherCoroutineOps::kExtensionId));

  if VLIKELY (ops != nullptr && ops->sleep != nullptr) {
    return ops->sleep(ops->host_ctx, ms, priority);
  }

  return co_sleep_impl(&loop(), cancellation_token(), ms, priority);
}

inline Co::Task<void> LauncherContext::co_await_stop(uint16_t priority) {
  const auto* ops = static_cast<const LauncherCoroutineOps*>(query_extension(LauncherCoroutineOps::kExtensionId));

  if VLIKELY (ops != nullptr && ops->await_stop != nullptr) {
    return ops->await_stop(ops->host_ctx, priority);
  }

  return co_await_stop_impl(&loop(), cancellation_token(), priority);
}

template <typename CallbackT>
inline auto LauncherContext::co_exec(const Schedule::Config& config, CallbackT&& callback) {
  return Co::exec(loop(), config, std::forward<CallbackT>(callback));
}
#endif

inline bool LauncherContext::get_param_bool(std::string_view key, bool default_value) const {
  LauncherParamValue val;

  if (get_param(key, val)) {
    const auto* ptr = std::get_if<bool>(&val);

    if (ptr != nullptr) {
      return *ptr;
    }

    const auto* sptr = std::get_if<std::string>(&val);

    if (sptr != nullptr) {
      if (*sptr == "true" || *sptr == "1") {
        return true;
      }

      if (*sptr == "false" || *sptr == "0") {
        return false;
      }
    }
  }

  return default_value;
}

inline int64_t LauncherContext::get_param_int(std::string_view key, int64_t default_value) const {
  LauncherParamValue val;

  if (get_param(key, val)) {
    const auto* ptr = std::get_if<int64_t>(&val);

    if (ptr != nullptr) {
      return *ptr;
    }

    const auto* sptr = std::get_if<std::string>(&val);

    if (sptr != nullptr && !sptr->empty() && std::isspace(static_cast<unsigned char>(sptr->front())) == 0 &&
        std::isspace(static_cast<unsigned char>(sptr->back())) == 0) {
      errno = 0;
      char* end = nullptr;
      const long long parsed = std::strtoll(sptr->c_str(), &end, 10);  // NOLINT(runtime/int,google-runtime-int)

      if (errno != ERANGE && end == sptr->c_str() + sptr->size()) {
        return static_cast<int64_t>(parsed);
      }
    }
  }

  return default_value;
}

inline double LauncherContext::get_param_double(std::string_view key, double default_value) const {
  LauncherParamValue val;

  if (get_param(key, val)) {
    const auto* ptr = std::get_if<double>(&val);

    if (ptr != nullptr) {
      return *ptr;
    }

    const auto* iptr = std::get_if<int64_t>(&val);

    if (iptr != nullptr) {
      return static_cast<double>(*iptr);
    }

    const auto* sptr = std::get_if<std::string>(&val);

    if (sptr != nullptr && !sptr->empty() && std::isspace(static_cast<unsigned char>(sptr->front())) == 0 &&
        std::isspace(static_cast<unsigned char>(sptr->back())) == 0) {
      std::istringstream iss(*sptr);
      iss.imbue(std::locale::classic());
      double parsed = 0.0;
      iss >> parsed;

      if (!iss.fail() && iss.eof() && std::isfinite(parsed)) {
        return parsed;
      }
    }
  }

  return default_value;
}

inline std::string LauncherContext::get_param_string(std::string_view key, std::string_view default_value) const {
  LauncherParamValue val;

  if (get_param(key, val)) {
    auto* ptr = std::get_if<std::string>(&val);

    if (ptr != nullptr) {
      return std::move(*ptr);
    }
  }

  return std::string(default_value);
}

inline std::vector<bool> LauncherContext::get_param_bool_list(std::string_view key,
                                                              const std::vector<bool>& default_value) const {
  LauncherParamValue val;

  if (get_param(key, val)) {
    auto* ptr = std::get_if<std::vector<bool>>(&val);

    if (ptr != nullptr) {
      return std::move(*ptr);
    }
  }

  return default_value;
}

inline std::vector<int64_t> LauncherContext::get_param_int_list(std::string_view key,
                                                                const std::vector<int64_t>& default_value) const {
  LauncherParamValue val;

  if (get_param(key, val)) {
    auto* ptr = std::get_if<std::vector<int64_t>>(&val);

    if (ptr != nullptr) {
      return std::move(*ptr);
    }
  }

  return default_value;
}

inline std::vector<double> LauncherContext::get_param_double_list(std::string_view key,
                                                                  const std::vector<double>& default_value) const {
  LauncherParamValue val;

  if (get_param(key, val)) {
    auto* ptr = std::get_if<std::vector<double>>(&val);

    if (ptr != nullptr) {
      return std::move(*ptr);
    }

    const auto* iptr = std::get_if<std::vector<int64_t>>(&val);

    if (iptr != nullptr) {
      std::vector<double> out;
      out.reserve(iptr->size());

      for (const int64_t item : *iptr) {
        out.push_back(static_cast<double>(item));
      }

      return out;
    }
  }

  return default_value;
}

inline std::vector<std::string> LauncherContext::get_param_string_list(
    std::string_view key, const std::vector<std::string>& default_value) const {
  LauncherParamValue val;

  if (get_param(key, val)) {
    auto* ptr = std::get_if<std::vector<std::string>>(&val);

    if (ptr != nullptr) {
      return std::move(*ptr);
    }
  }

  return default_value;
}

inline LauncherContext::CallbackScope::CallbackScope(const LauncherContext* ctx) noexcept
    : ctx_(ctx), token_(ctx != nullptr ? ctx->enter_callback_scope() : 0U) {}

inline LauncherContext::CallbackScope::~CallbackScope() {
  if (ctx_ != nullptr) {
    ctx_->exit_callback_scope(token_);
  }
}

#ifdef VLINK_ENABLE_COROUTINE
inline Co::Task<void> LauncherContext::run_guarded_coro(std::shared_ptr<void> guard, Co::Task<void> task) {
  auto g = std::move(guard);
  auto t = std::move(task);

  try {
    co_await t;
  } catch (const std::exception& e) {
    VLOG_E("LauncherContext: Component coroutine threw exception: ", e.what(), ".");
  } catch (...) {
    VLOG_E("LauncherContext: Component coroutine threw unknown exception.");
  }

  t = {};
  g.reset();
}

inline Co::Task<bool> LauncherContext::co_sleep_impl(MessageLoop* target_loop, CancellationToken token, uint32_t ms,
                                                     uint16_t priority) {
  if (token.is_cancellation_requested()) {
    co_return false;
  }

  auto prom = MemoryResource::make_shared<std::promise<bool>>();
  auto done = MemoryResource::make_shared<std::atomic_bool>(false);
  auto fut = prom->get_future();

  auto reg = token.register_callback([prom, done]() mutable {
    if (!done->exchange(true, std::memory_order_acq_rel)) {
      prom->set_value(false);
    }
  });

  Timer timer(ms, 1, [prom, done]() mutable {
    if (!done->exchange(true, std::memory_order_acq_rel)) {
      prom->set_value(true);
    }
  });

  if (priority > 0U) {
    timer.set_priority(priority);
  }

  if (!done->load(std::memory_order_acquire)) {
    if (timer.attach(target_loop)) {
      timer.start();
    } else if (!done->exchange(true, std::memory_order_acq_rel)) {
      prom->set_value(false);
    }
  }

  const bool elapsed = co_await Co::await_future(*target_loop, std::move(fut));
  timer.detach();
  reg.reset();
  co_return elapsed && !token.is_cancellation_requested();
}

inline Co::Task<void> LauncherContext::co_await_stop_impl(MessageLoop* target_loop, CancellationToken token,
                                                          uint16_t priority) {
  (void)priority;

  if VUNLIKELY (!token.valid() || token.is_cancellation_requested()) {
    co_return;
  }

  auto prom = MemoryResource::make_shared<std::promise<void>>();
  auto fut = prom->get_future();

  auto reg = token.register_callback([prom = std::move(prom)]() mutable { prom->set_value(); });

  co_await Co::await_future(*target_loop, std::move(fut));
  reg.reset();
}
#endif

}  // namespace vlink
