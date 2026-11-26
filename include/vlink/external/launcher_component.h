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
 * @file launcher_component.h
 * @brief Header-only in-process component plugin ABI hosted by @c LauncherServer and containers.
 *
 * @details
 * Defines the lightweight, dependency-free contract implemented by dynamic component plugins
 * loaded into @c LauncherServer or isolated @c [[container]] child processes.  A single shared
 * library may export any number of named @c LauncherComponent, periodic
 * @c LauncherTimerComponent, and C++20 @c LauncherCoroutineComponent classes via
 * @c VLINK_LAUNCHER_MODULE_BEGIN / @c VLINK_LAUNCHER_EXPORT_COMPONENT /
 * @c VLINK_LAUNCHER_MODULE_END.  Plugin libraries only need to include this header and link
 * @c vlink::vlink; they never depend on @c tomlplusplus or @c vlink::launcher_server.
 *
 * @par Component Lifecycle
 * @verbatim
 *   Plugin::load<LauncherModuleInterface>      (once per shared library)
 *        |
 *        v
 *   module->create_component(class_name)       (per configured instance)
 *        |
 *        v
 *   on_configure(ctx) ---> on_start(ctx) ---> on_activate(ctx) ---> [kReady / active]
 *        |                      ^                   ^                     |
 *        | init_state=          | first activate    | re-activate         | on_deactivate()
 *        | inactive             +---- [kInactive standby] <---------------+
 *        +-------------------------------^     |
 *                                              v
 *                                   (if started) on_stop() ---> on_cleanup()
 * @endverbatim
 *
 * @par Component Base Classes
 *
 * | Base Class                   | Execution Model                                                |
 * | ---------------------------- | -------------------------------------------------------------- |
 * | @c LauncherComponent         | General lifecycle hooks driven on the assigned @c MessageLoop. |
 * | @c LauncherTimerComponent    | Periodic @c on_tick() driven by a managed @c vlink::Timer.     |
 * | @c LauncherCoroutineComponent | C++20 coroutine @c on_coro_run() driven by @c vlink::Co.      |
 *
 * @par Registration Contract
 *
 * | Macro                                                        | Purpose                                         |
 * | ------------------------------------------------------------ | ----------------------------------------------- |
 * | @c VLINK_LAUNCHER_MODULE_BEGIN(VersionMajor, VersionMinor)   | Opens a @c LauncherModuleInterface block.       |
 * | @c VLINK_LAUNCHER_EXPORT_COMPONENT(ClassName, ComponentType) | Registers @c ComponentType under @c ClassName.  |
 * | @c VLINK_LAUNCHER_MODULE_END()                               | Closes the block and emits @c VLINK_PLUGIN_*.   |
 *
 * @par Threading, ABI Stability, and Unload Safety
 * Lifecycle hooks execute on the component's assigned scheduler @c MessageLoop.  On single-thread
 * scheduler modes (@c dedicated, @c shared, @c pinned) attached subscriber callbacks, timers,
 * and lifecycle/parameter hooks are serialised on that loop; on @c multi mode, callbacks may
 * execute concurrently across worker threads and components must synchronise shared state.
 * @c LauncherContext maintains a fixed C++17 vtable layout independent of
 * @c VLINK_ENABLE_COROUTINE so C++17 and C++20 builds remain binary-compatible across the
 * plugin boundary.  Managed handles, timers, scheduled tasks, compute tasks, and coroutines
 * coordinate with the host's active flag and unload task guard so dynamic unloading drains
 * in-flight work before calling @c destroy_component().
 *
 * @par Example
 * @code
 * class EchoComponent final : public vlink::LauncherTimerComponent {
 *  public:
 *   EchoComponent() : vlink::LauncherTimerComponent(100U) {}
 *
 *  protected:
 *   bool on_timer_start(vlink::LauncherContext& ctx) override {
 *     pub_ = ctx.create_publisher<std::string>("out", "intra://demo/echo");
 *     return pub_.valid();
 *   }
 *
 *   bool on_tick(vlink::LauncherContext& ctx) override {
 *     (void)ctx;
 *     return pub_.publish("hello");
 *   }
 *
 *   void on_timer_stop() override { pub_.reset(); }
 *
 *  private:
 *   vlink::LauncherPublisher<std::string> pub_;
 * };
 *
 * VLINK_LAUNCHER_MODULE_BEGIN(1, 0)
 *   VLINK_LAUNCHER_EXPORT_COMPONENT("Echo", EchoComponent)
 * VLINK_LAUNCHER_MODULE_END()
 * @endcode
 *
 * @note
 * - Keep component constructors trivial and acquire live VLink primitives inside
 *   @c on_configure() or @c on_start(), releasing them symmetrically in @c on_stop() or
 *   @c on_cleanup().
 * - @c LauncherCoroutineComponent and coroutine helpers on @c LauncherContext are available
 *   when @c VLINK_ENABLE_COROUTINE is defined.
 * - Legacy @c RunablePluginInterface plugins are also supported by @c LauncherServer via
 *   @c legacy = true in TOML manifests.
 *
 * @see launcher_server.h, launcher_api.h, base/plugin.h, extension/runnable_plugin_interface.h
 */

#pragma once

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "../base/bytes.h"
#include "../base/cancellation.h"
#include "../base/coroutine.h"
#include "../base/functional.h"
#include "../base/logger.h"
#include "../base/macros.h"
#include "../base/memory_resource.h"
#include "../base/message_loop.h"
#include "../base/plugin.h"
#include "../base/schedule.h"
#include "../base/task_handle.h"
#include "../base/thread_pool.h"
#include "../base/timer.h"
#include "../publisher.h"
#include "../subscriber.h"

namespace vlink {

class LauncherContext;

/**
 * @class LauncherPublisher
 * @brief Lifecycle-aware, shared-ownership publisher handle bound to a declared output port.
 *
 * @details
 * Wraps @c vlink::Publisher\<MsgT\> together with a shared view of the owning component's
 * lifecycle active flag.  Components offloading work onto a @c ThreadPool may copy a
 * @c LauncherPublisher handle into compute tasks safely without risking use-after-free on stop,
 * while deactivation immediately suppresses outbound traffic from stale tasks.
 *
 * @tparam MsgT  Published message type.
 */
template <typename MsgT>
class LauncherPublisher final {
 public:
  /**
   * @brief Constructs an empty, invalid publisher handle.
   */
  LauncherPublisher() = default;

  /**
   * @brief Constructs a managed publisher handle bound to an initialised @c Publisher and lifecycle flags.
   *
   * @param pub                 Shared underlying @c Publisher\<MsgT\> instance.
   * @param active_flag         Optional shared atomic active flag from @c LauncherContext::active_flag().
   * @param url                 Resolved VLink URL string.
   * @param force_publish_flag  Optional shared flag enabling forced publish during active recording.
   */
  LauncherPublisher(std::shared_ptr<Publisher<MsgT>> pub, std::shared_ptr<const std::atomic_bool> active_flag,
                    std::string url, std::shared_ptr<const std::atomic_bool> force_publish_flag = nullptr);

  /**
   * @brief Reports whether this handle holds an initialised publisher.
   *
   * @return @c true when usable; @c false when empty.
   */
  [[nodiscard]] bool valid() const noexcept;

  /**
   * @brief Explicit boolean conversion equivalent to @c valid().
   *
   * @return @c true when usable; @c false when empty.
   */
  explicit operator bool() const noexcept;

  /**
   * @brief Returns the resolved VLink URL bound to this publisher.
   *
   * @return Resolved URL string.
   */
  [[nodiscard]] const std::string& url() const noexcept;

  /**
   * @brief Publishes @p msg when the owning component is in the active lifecycle state.
   *
   * @param msg    Payload to publish.
   * @param force  When @c true, forces serialisation/recording even with no subscriber attached.
   * @return       @c true when handed to the transport; @c false if inactive or invalid.
   */
  bool publish(const MsgT& msg, bool force = false) const;

  /**
   * @brief Publishes an owned @c Bytes buffer by move when the owning component is active.
   *
   * @tparam U     SFINAE guard restricting this overload to @c MsgT == @c Bytes.
   * @param msg    Owned payload moved into the transport without deep copy.
   * @param force  When @c true, forces serialisation/recording even with no subscriber attached.
   * @return       @c true when handed to the transport; @c false if inactive or invalid.
   */
  template <typename U = MsgT, std::enable_if_t<std::is_same_v<U, Bytes>, int> = 0>
  bool publish(Bytes&& msg, bool force = false) const;

  /**
   * @brief Enables or disables per-node active bag recording on this publisher.
   *
   * @details
   * Non-empty @p path delegates to @c Publisher::set_record_path(); an empty path stops
   * recording.  @c intra:// publishers are skipped with a warning because VLink's per-node
   * recorder does not support @c intra://.
   *
   * @param path  Target bag path (@c .vdb / @c .vdbx / @c .vcap / @c .vcapx) or empty string to disable.
   * @return      @c true when applied to the underlying publisher; @c false if invalid or unsupported.
   */
  bool set_record_path(const std::string& path) const;

  /**
   * @brief Returns the underlying @c Publisher pointer for advanced operations (e.g. zero-copy @c loan()).
   *
   * @return Raw publisher pointer, or @c nullptr when invalid.
   */
  [[nodiscard]] Publisher<MsgT>* raw() const noexcept;

  /**
   * @brief Releases this handle's reference to the underlying publisher and flags.
   */
  void reset() noexcept;

 private:
  std::shared_ptr<Publisher<MsgT>> pub_;
  std::shared_ptr<const std::atomic_bool> active_flag_;
  std::shared_ptr<const std::atomic_bool> force_publish_flag_;
  std::string url_;
};

/**
 * @class LauncherSubscriber
 * @brief Lifecycle-aware subscriber handle bound to a declared input port.
 *
 * @details
 * Wraps @c vlink::Subscriber\<MsgT\> with @c set_safety_quit(true), optional @c MessageLoop
 * attachment, @c listen() error propagation, lifecycle active-state gating, unload task-guard
 * tracking, and deterministic @c deinit() before @c detach() teardown.
 *
 * @tparam MsgT  Subscribed message type.
 */
template <typename MsgT>
class LauncherSubscriber final {
 public:
  /**
   * @brief Constructs an empty, invalid subscriber handle.
   */
  LauncherSubscriber() = default;

  /**
   * @brief Constructs a managed subscriber handle taking ownership of @p sub.
   *
   * @param sub       Initialised and listening @c Subscriber\<MsgT\> instance.
   * @param url       Resolved VLink URL string.
   * @param attached  Whether @p sub is attached to a @c MessageLoop.
   */
  LauncherSubscriber(std::unique_ptr<Subscriber<MsgT>> sub, std::string url, bool attached);

  /**
   * @brief Constructs a managed subscriber handle sharing lifetime with runtime record hooks.
   *
   * @param sub       Initialised and listening @c Subscriber\<MsgT\> instance.
   * @param url       Resolved VLink URL string.
   * @param attached  Whether @p sub is attached to a @c MessageLoop.
   */
  LauncherSubscriber(std::shared_ptr<Subscriber<MsgT>> sub, std::string url, bool attached);

  /**
   * @brief Move-constructs a subscriber handle, transferring ownership from @p other.
   *
   * @param other  Source handle left empty after the move.
   */
  LauncherSubscriber(LauncherSubscriber&& other) noexcept;

  /**
   * @brief Move-assigns a subscriber handle, releasing any previously owned subscriber first.
   *
   * @param other  Source handle left empty after the move.
   * @return       Reference to @c *this.
   */
  LauncherSubscriber& operator=(LauncherSubscriber&& other) noexcept;

  /**
   * @brief Stops transport delivery, detaches if attached, and destroys the subscriber.
   */
  ~LauncherSubscriber();

  LauncherSubscriber(const LauncherSubscriber&) = delete;
  LauncherSubscriber& operator=(const LauncherSubscriber&) = delete;

  /**
   * @brief Reports whether the subscriber bound and installed its listener successfully.
   *
   * @return @c true when active and listening; @c false when empty.
   */
  [[nodiscard]] bool valid() const noexcept;

  /**
   * @brief Explicit boolean conversion equivalent to @c valid().
   *
   * @return @c true when active and listening; @c false when empty.
   */
  explicit operator bool() const noexcept;

  /**
   * @brief Returns the resolved VLink URL bound to this subscriber.
   *
   * @return Resolved URL string.
   */
  [[nodiscard]] const std::string& url() const noexcept;

  /**
   * @brief Enables or disables per-node active bag recording on this subscriber.
   *
   * @param path  Target bag path (@c .vdb / @c .vdbx / @c .vcap / @c .vcapx) or empty string to disable.
   * @return      @c true when applied to the underlying subscriber; @c false if invalid or unsupported.
   */
  bool set_record_path(const std::string& path) const;

  /**
   * @brief Returns the underlying @c Subscriber pointer.
   *
   * @return Raw subscriber pointer, or @c nullptr when invalid.
   */
  [[nodiscard]] Subscriber<MsgT>* raw() const noexcept;

  /**
   * @brief Stops transport delivery, detaches if attached, and destroys the underlying subscriber.
   */
  void reset() noexcept;

 private:
  std::shared_ptr<Subscriber<MsgT>> sub_;
  std::string url_;
  bool attached_{false};
};

/**
 * @brief Scalar or homogeneous-list value stored in a launcher component parameter map.
 */
using LauncherParamValue = std::variant<bool, int64_t, double, std::string, std::vector<bool>, std::vector<int64_t>,
                                        std::vector<double>, std::vector<std::string>>;

/**
 * @brief Flat dot-delimited parameter table (e.g. @c "sensor.rate_hz").
 */
using LauncherParamMap = std::unordered_map<std::string, LauncherParamValue>;

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
  /// Extension lookup identifier.
  static constexpr std::string_view kExtensionId = "vlink.launcher.coroutine.v1";

  /// Host coroutine spawner function pointer.
  bool (*spawn)(void* host_ctx, Co::Task<void>&& task, uint16_t priority){nullptr};
  /// Host stop-aware coroutine sleep function pointer.
  Co::Task<bool> (*sleep)(void* host_ctx, uint32_t ms, uint16_t priority){nullptr};
  /// Host stop-signal awaiter function pointer.
  Co::Task<void> (*await_stop)(void* host_ctx, uint16_t priority){nullptr};
  /// Opaque host context pointer passed to coroutine hooks.
  void* host_ctx{nullptr};
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
   * @return             Valid @c LauncherPublisher\<MsgT\> handle, or an invalid handle if URL is empty.
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
   * @return             Valid @c LauncherSubscriber\<MsgT\> handle, or invalid on empty URL / @c listen() failure.
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

/**
 * @class LauncherComponent
 * @brief Base class for in-process components hosted by @c LauncherServer or isolated containers.
 *
 * @details
 * Lifecycle hooks execute on the component's assigned scheduler @c MessageLoop unless noted
 * otherwise.  Components should keep constructors cheap and acquire live VLink primitives inside
 * @c on_configure() or @c on_start(), releasing them symmetrically in @c on_stop() or
 * @c on_cleanup().
 */
class LauncherComponent {
 public:
  /**
   * @brief Virtual destructor.
   */
  virtual ~LauncherComponent() = default;

  /**
   * @brief Configures the component from injected parameters prior to starting.
   *
   * @param ctx  Runtime context providing parameters, URL remap, and scheduler handles.
   * @return     @c true on success; @c false aborts startup of this component.
   */
  virtual bool on_configure(LauncherContext& ctx);

  /**
   * @brief Starts the component (creates publishers/subscribers/timers or spawns coroutines).
   *
   * @param ctx  Runtime context.
   * @return     @c true on success; @c false marks startup failed.
   */
  virtual bool on_start(LauncherContext& ctx) = 0;

  /**
   * @brief Stops active work, timers, and communication endpoints before teardown.
   */
  virtual void on_stop() = 0;

  /**
   * @brief Releases remaining resources after @c on_stop().
   */
  virtual void on_cleanup();

  /**
   * @brief Transitions a configured standby component into active operation.
   *
   * @details
   * Optional hook invoked after @c on_start() during normal launch, or upon @c ctl @c activate
   * when resuming from @c UnitState::kInactive standby (on the first activation from
   * @c init_state="inactive", the host invokes @c on_start() before @c on_activate()).  Default
   * implementation is a no-op returning @c true.
   *
   * @param ctx  Runtime context.
   * @return     @c true on success; @c false keeps the component inactive.
   */
  virtual bool on_activate(LauncherContext& ctx);

  /**
   * @brief Suspends active publishing and processing while retaining configured resources.
   *
   * @details
   * Optional hook invoked upon @c ctl @c deactivate (entering @c UnitState::kInactive) and
   * before @c on_stop() when tearing down an active component.  Default implementation is a
   * no-op.
   */
  virtual void on_deactivate();

  /**
   * @brief Invoked on a live @c reload command for in-place reconfiguration.
   *
   * @param ctx  Runtime context with updated parameters.
   * @return     @c true when reload succeeded.
   */
  virtual bool on_reload(LauncherContext& ctx);

  /**
   * @brief Invoked when a parameter is updated dynamically through the control plane.
   *
   * @param key    Dot-delimited parameter name.
   * @param value  Proposed new value.
   * @return       @c true to accept and commit the new value; @c false to reject the update.
   */
  virtual bool on_param_changed(std::string_view key, const LauncherParamValue& value);

 protected:
  LauncherComponent() = default;

 private:
  VLINK_DISALLOW_COPY_AND_ASSIGN(LauncherComponent)
};

/**
 * @class LauncherTimerComponent
 * @brief Periodic timer-driven component base class.
 *
 * @details
 * Reads the @c interval_ms (or @c period_ms) parameter during @c on_configure(), binds a
 * @c vlink::Timer to the component's assigned @c MessageLoop during @c on_start(), calls
 * @c on_tick() on each period while refreshing the watchdog, supports live interval updates
 * via @c on_param_changed(), and stops/detaches the timer in @c on_deactivate() and
 * @c on_stop().
 */
class LauncherTimerComponent : public LauncherComponent {
 public:
  /**
   * @brief Constructs a periodic component with a fallback default interval.
   *
   * @param default_interval_ms  Default tick interval in milliseconds when not configured.
   */
  explicit LauncherTimerComponent(uint32_t default_interval_ms = 1000U) noexcept;

  /**
   * @brief Virtual destructor.
   */
  ~LauncherTimerComponent() override = default;

  /**
   * @brief Parses @c interval_ms / @c period_ms and delegates to @c on_timer_configure().
   *
   * @param ctx  Component execution context.
   * @return     @c true when the interval is in @c [1, UINT32_MAX] and @c on_timer_configure() succeeded.
   */
  bool on_configure(LauncherContext& ctx) override;

  /**
   * @brief Runs @c on_timer_start(), arms the periodic timer, pings the watchdog, and marks ready.
   *
   * @param ctx  Component execution context.
   * @return     @c true on success; @c false if @c on_timer_start() fails.
   */
  bool on_start(LauncherContext& ctx) override;

  /**
   * @brief Re-arms the periodic timer when resuming from @c kInactive standby and marks ready.
   *
   * @param ctx  Component execution context.
   * @return     @c true on success.
   */
  bool on_activate(LauncherContext& ctx) override;

  /**
   * @brief Stops and releases the periodic timer while entering @c kInactive standby.
   */
  void on_deactivate() override;

  /**
   * @brief Stops the periodic timer and invokes @c on_timer_stop().
   */
  void on_stop() override;

  /**
   * @brief Applies live updates to @c interval_ms / @c period_ms and forwards other keys.
   *
   * @param key    Parameter key.
   * @param value  Candidate parameter value.
   * @return       @c true when accepted; @c false when rejected.
   */
  bool on_param_changed(std::string_view key, const LauncherParamValue& value) override;

  /**
   * @brief Returns the active tick interval in milliseconds.
   *
   * @return Current timer interval.
   */
  [[nodiscard]] uint32_t interval_ms() const noexcept;

 protected:
  /**
   * @brief Optional configuration hook invoked during @c on_configure() after interval parsing.
   *
   * @param ctx  Component execution context.
   * @return     @c true on success.
   */
  virtual bool on_timer_configure(LauncherContext& ctx);

  /**
   * @brief Optional startup hook invoked during @c on_start() before the periodic timer is armed.
   *
   * @param ctx  Component execution context.
   * @return     @c true on success.
   */
  virtual bool on_timer_start(LauncherContext& ctx);

  /**
   * @brief Periodic business tick invoked on the component's @c MessageLoop while active.
   *
   * @param ctx  Component execution context.
   * @return     @c true if healthy; @c false to report degraded health via @c report_health().
   */
  virtual bool on_tick(LauncherContext& ctx) = 0;

  /**
   * @brief Optional stop hook invoked during @c on_stop() after the periodic timer has stopped.
   */
  virtual void on_timer_stop();

  /**
   * @brief Optional dynamic parameter hook invoked for keys other than @c interval_ms / @c period_ms.
   *
   * @param key    Parameter key.
   * @param value  Candidate parameter value.
   * @return       @c true to accept the update; @c false to reject it.
   */
  virtual bool on_timer_param_changed(std::string_view key, const LauncherParamValue& value);

 private:
  void start_timer_internal(LauncherContext& ctx);

  void stop_timer_internal();

  uint32_t default_interval_ms_{1000U};
  uint32_t interval_ms_{1000U};
  LauncherContext* ctx_{nullptr};
  std::unique_ptr<Timer> timer_;
};

#ifdef VLINK_ENABLE_COROUTINE
/**
 * @class LauncherCoroutineComponent
 * @brief Declarative C++20 coroutine component base class driven by @c vlink::Co.
 *
 * @details
 * Spawns @c on_coro_run(ctx) onto the component's assigned @c MessageLoop with an automatic
 * unload guard.  When the host stops or unloads the component, the cancellation token fires
 * immediately (waking any @c ctx.co_sleep() or @c ctx.co_await_stop()) and host teardown waits
 * for the coroutine frame to unwind before invoking @c destroy_component().
 */
class LauncherCoroutineComponent : public LauncherComponent {
 public:
  /**
   * @brief Virtual destructor.
   */
  ~LauncherCoroutineComponent() override = default;

  /**
   * @brief Invokes @c on_coro_start() and spawns @c on_coro_run() onto @c ctx.loop().
   *
   * @param ctx  Component execution context.
   * @return     @c true when setup succeeded and the coroutine was spawned.
   */
  bool on_start(LauncherContext& ctx) override;

  /**
   * @brief Delegates shutdown cleanup to @c on_coro_stop().
   */
  void on_stop() override;

 protected:
  LauncherCoroutineComponent() = default;

  /**
   * @brief Optional synchronous setup hook invoked during @c on_start() before spawning @c on_coro_run().
   *
   * @param ctx  Component execution context.
   * @return     @c true on success.
   */
  virtual bool on_coro_start(LauncherContext& ctx);

  /**
   * @brief Main coroutine body executed on the component's @c MessageLoop.
   *
   * @param ctx  Runtime context providing @c co_sleep(), @c co_await_stop(), and @c co_exec().
   * @return     Coroutine task.
   */
  virtual Co::Task<void> on_coro_run(LauncherContext& ctx) = 0;

  /**
   * @brief Optional stop hook invoked during @c on_stop() after cancellation has been requested.
   */
  virtual void on_coro_stop();

 private:
  static Co::Task<void> drive_coro(LauncherCoroutineComponent* self, LauncherContext* ctx);
};
#endif

/**
 * @class LauncherModuleInterface
 * @brief Factory plugin interface exported by shared libraries hosting @c LauncherComponent types.
 *
 * @details
 * Loaded once per shared library via @c vlink::Plugin::load\<LauncherModuleInterface\>().  The
 * host queries @c list_components() and calls @c create_component() / @c destroy_component()
 * to spawn any number of independent component instances from the same library.
 */
class LauncherModuleInterface {
  VLINK_PLUGIN_REGISTER_BY_ID(LauncherModuleInterface, "vlink.launcher.module")

 public:
  /**
   * @brief Major interface ABI version required by the host.
   */
  static constexpr uint16_t kVersionMajor = 1;

  /**
   * @brief Minimum minor interface ABI version required by the host.
   */
  static constexpr uint16_t kVersionMinor = 0;

  /**
   * @brief Virtual destructor.
   */
  virtual ~LauncherModuleInterface() = default;

  /**
   * @brief Lists all component class names exported by this shared library.
   *
   * @return Vector of registered class names.
   */
  [[nodiscard]] virtual std::vector<std::string> list_components() const = 0;

  /**
   * @brief Instantiates a component registered under @p class_name.
   *
   * @param class_name  Registered component class name; empty instantiates the sole class when unique.
   * @return            Raw owning pointer to release via @c destroy_component(), or @c nullptr on miss.
   */
  [[nodiscard]] virtual LauncherComponent* create_component(std::string_view class_name) const = 0;

  /**
   * @brief Destroys a component instance previously created by @c create_component().
   *
   * @param component  Instance pointer; @c nullptr is a no-op.
   */
  virtual void destroy_component(LauncherComponent* component) const noexcept = 0;

 protected:
  LauncherModuleInterface() = default;

 private:
  VLINK_DISALLOW_COPY_AND_ASSIGN(LauncherModuleInterface)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

template <typename MsgT>
inline LauncherPublisher<MsgT>::LauncherPublisher(std::shared_ptr<Publisher<MsgT>> pub,
                                                  std::shared_ptr<const std::atomic_bool> active_flag, std::string url,
                                                  std::shared_ptr<const std::atomic_bool> force_publish_flag)
    : pub_(std::move(pub)),
      active_flag_(std::move(active_flag)),
      force_publish_flag_(std::move(force_publish_flag)),
      url_(std::move(url)) {}

template <typename MsgT>
inline bool LauncherPublisher<MsgT>::valid() const noexcept {
  return pub_ != nullptr;
}

template <typename MsgT>
inline LauncherPublisher<MsgT>::operator bool() const noexcept {
  return valid();
}

template <typename MsgT>
inline const std::string& LauncherPublisher<MsgT>::url() const noexcept {
  return url_;
}

template <typename MsgT>
inline bool LauncherPublisher<MsgT>::publish(const MsgT& msg, bool force) const {
  if VUNLIKELY (!pub_ || (active_flag_ != nullptr && !active_flag_->load(std::memory_order_acquire))) {
    return false;
  }

  const bool eff_force =
      force || (force_publish_flag_ != nullptr && force_publish_flag_->load(std::memory_order_acquire));
  return pub_->publish(msg, eff_force);
}

template <typename MsgT>
template <typename U, std::enable_if_t<std::is_same_v<U, Bytes>, int>>
inline bool LauncherPublisher<MsgT>::publish(Bytes&& msg, bool force) const {
  if VUNLIKELY (!pub_ || (active_flag_ != nullptr && !active_flag_->load(std::memory_order_acquire))) {
    return false;
  }

  const bool eff_force =
      force || (force_publish_flag_ != nullptr && force_publish_flag_->load(std::memory_order_acquire));
  return pub_->publish(std::move(msg), eff_force);
}

template <typename MsgT>
inline bool LauncherPublisher<MsgT>::set_record_path(const std::string& path) const {
  if VUNLIKELY (!pub_) {
    return false;
  }

  if (pub_->get_transport_type() == TransportType::kIntra) {
    if (!path.empty()) {
      VLOG_W("LauncherPublisher: Per-node set_record_path() is not supported on intra:// (", url_, ").");
    }

    return false;
  }

  pub_->set_record_path(path);
  return true;
}

template <typename MsgT>
inline Publisher<MsgT>* LauncherPublisher<MsgT>::raw() const noexcept {
  return pub_.get();
}

template <typename MsgT>
inline void LauncherPublisher<MsgT>::reset() noexcept {
  pub_.reset();
  active_flag_.reset();
  force_publish_flag_.reset();
  url_.clear();
}

template <typename MsgT>
inline LauncherSubscriber<MsgT>::LauncherSubscriber(std::unique_ptr<Subscriber<MsgT>> sub, std::string url,
                                                    bool attached)
    : sub_(std::move(sub)), url_(std::move(url)), attached_(attached) {}

template <typename MsgT>
inline LauncherSubscriber<MsgT>::LauncherSubscriber(std::shared_ptr<Subscriber<MsgT>> sub, std::string url,
                                                    bool attached)
    : sub_(std::move(sub)), url_(std::move(url)), attached_(attached) {}

template <typename MsgT>
inline LauncherSubscriber<MsgT>::LauncherSubscriber(LauncherSubscriber&& other) noexcept
    : sub_(std::move(other.sub_)), url_(std::move(other.url_)), attached_(other.attached_) {
  other.attached_ = false;
}

template <typename MsgT>
inline LauncherSubscriber<MsgT>& LauncherSubscriber<MsgT>::operator=(LauncherSubscriber&& other) noexcept {
  if (this != &other) {
    reset();
    sub_ = std::move(other.sub_);
    url_ = std::move(other.url_);
    attached_ = other.attached_;
    other.attached_ = false;
  }

  return *this;
}

template <typename MsgT>
inline LauncherSubscriber<MsgT>::~LauncherSubscriber() {
  reset();
}

template <typename MsgT>
inline bool LauncherSubscriber<MsgT>::valid() const noexcept {
  return sub_ != nullptr;
}

template <typename MsgT>
inline LauncherSubscriber<MsgT>::operator bool() const noexcept {
  return valid();
}

template <typename MsgT>
inline const std::string& LauncherSubscriber<MsgT>::url() const noexcept {
  return url_;
}

template <typename MsgT>
inline bool LauncherSubscriber<MsgT>::set_record_path(const std::string& path) const {
  if VUNLIKELY (!sub_) {
    return false;
  }

  if (sub_->get_transport_type() == TransportType::kIntra) {
    if (!path.empty()) {
      VLOG_W("LauncherSubscriber: Per-node set_record_path() is not supported on intra:// (", url_, ").");
    }

    return false;
  }

  sub_->set_record_path(path);
  return true;
}

template <typename MsgT>
inline Subscriber<MsgT>* LauncherSubscriber<MsgT>::raw() const noexcept {
  return sub_.get();
}

template <typename MsgT>
inline void LauncherSubscriber<MsgT>::reset() noexcept {
  if (sub_) {
    sub_->deinit();

    if (attached_) {
      (void)sub_->detach();
      attached_ = false;
    }

    sub_.reset();
  }

  url_.clear();
}

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

template <typename MsgT>
inline LauncherPublisher<MsgT> LauncherContext::create_publisher(std::string_view port_key,
                                                                 std::string_view default_url) const {
  const std::string resolved = get_output_url(port_key, default_url);

  if VUNLIKELY (resolved.empty()) {
    return {};
  }

  auto pub = MemoryResource::make_shared<Publisher<MsgT>>(resolved);
  LauncherPublisher<MsgT> handle(pub, active_flag(), resolved, record_force_publish_flag());

  std::weak_ptr<Publisher<MsgT>> weak_pub(pub);
  register_record_hook([weak_pub = std::move(weak_pub), resolved](const std::string& path) -> bool {
    auto locked = weak_pub.lock();

    if (!locked) {
      return false;
    }

    if (locked->get_transport_type() == TransportType::kIntra) {
      if (!path.empty()) {
        VLOG_W("LauncherPublisher: Per-node set_record_path() is not supported on intra:// (", resolved, ").");
      }

      return true;
    }

    locked->set_record_path(path);
    return true;
  });

  return handle;
}

template <typename MsgT, typename CallbackT>
inline LauncherSubscriber<MsgT> LauncherContext::create_subscriber(std::string_view port_key, CallbackT&& callback,
                                                                   std::string_view default_url, bool attach_loop) {
  const std::string resolved = get_input_url(port_key, default_url);

  if VUNLIKELY (resolved.empty()) {
    return {};
  }

  auto sub = MemoryResource::make_shared<Subscriber<MsgT>>(resolved, InitType::kWithoutInit);
  sub->set_safety_quit(true);
  bool attached = false;

  if (attach_loop) {
    attached = sub->attach(&loop());
  }

  sub->init();

  auto flag = active_flag();
  auto wrapped = [this, flag = std::move(flag), cb = std::forward<CallbackT>(callback)](const MsgT& msg) mutable {
    if VUNLIKELY (flag != nullptr ? !flag->load(std::memory_order_acquire) : !is_active()) {
      return;
    }

    auto guard = acquire_task_guard();

    if VUNLIKELY (!guard) {
      return;
    }

    CallbackScope scope(this);
    cb(msg);
  };

  if VUNLIKELY (!sub->listen(std::move(wrapped))) {
    sub->deinit();

    if (attached) {
      (void)sub->detach();
    }

    return {};
  }

  LauncherSubscriber<MsgT> handle(sub, resolved, attached);

  std::weak_ptr<Subscriber<MsgT>> weak_sub_cleanup(sub);
  register_cleanup_hook([weak_sub_cleanup = std::move(weak_sub_cleanup), attached]() {
    auto locked = weak_sub_cleanup.lock();

    if (locked) {
      locked->deinit();

      if (attached) {
        (void)locked->detach();
      }
    }
  });

  std::weak_ptr<Subscriber<MsgT>> weak_sub(sub);
  register_record_hook([weak_sub = std::move(weak_sub), resolved](const std::string& path) -> bool {
    auto locked = weak_sub.lock();

    if (!locked) {
      return false;
    }

    if (locked->get_transport_type() == TransportType::kIntra) {
      if (!path.empty()) {
        VLOG_W("LauncherSubscriber: Per-node set_record_path() is not supported on intra:// (", resolved, ").");
      }

      return true;
    }

    locked->set_record_path(path);
    return true;
  });

  return handle;
}

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

    if (sptr != nullptr && !sptr->empty()) {
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

    if (sptr != nullptr && !sptr->empty()) {
      errno = 0;
      char* end = nullptr;
      const double parsed = std::strtod(sptr->c_str(), &end);

      if (errno != ERANGE && end == sptr->c_str() + sptr->size()) {
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

  if (token.is_cancellation_requested()) {
    co_return;
  }

  auto prom = MemoryResource::make_shared<std::promise<void>>();
  auto done = MemoryResource::make_shared<std::atomic_bool>(false);
  auto fut = prom->get_future();

  auto reg = token.register_callback([prom, done]() mutable {
    if (!done->exchange(true, std::memory_order_acq_rel)) {
      prom->set_value();
    }
  });

  co_await Co::await_future(*target_loop, std::move(fut));
  reg.reset();
}
#endif

inline bool LauncherComponent::on_configure(LauncherContext& ctx) {
  (void)ctx;
  return true;
}

inline void LauncherComponent::on_cleanup() {}

inline bool LauncherComponent::on_activate(LauncherContext& ctx) {
  (void)ctx;
  return true;
}

inline void LauncherComponent::on_deactivate() {}

inline bool LauncherComponent::on_reload(LauncherContext& ctx) {
  (void)ctx;
  return true;
}

inline bool LauncherComponent::on_param_changed(std::string_view key, const LauncherParamValue& value) {
  (void)key;
  (void)value;
  return true;
}

inline LauncherTimerComponent::LauncherTimerComponent(uint32_t default_interval_ms) noexcept
    : default_interval_ms_(default_interval_ms), interval_ms_(default_interval_ms) {}

inline bool LauncherTimerComponent::on_configure(LauncherContext& ctx) {
  auto configured = static_cast<int64_t>(default_interval_ms_);

  if (ctx.has_param("interval_ms")) {
    configured = ctx.get_param_int("interval_ms", configured);
  } else if (ctx.has_param("period_ms")) {
    configured = ctx.get_param_int("period_ms", configured);
  }

  if (configured <= 0 || configured > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
    return false;
  }

  interval_ms_ = static_cast<uint32_t>(configured);
  return on_timer_configure(ctx);
}

inline bool LauncherTimerComponent::on_start(LauncherContext& ctx) {
  ctx_ = &ctx;

  if (!on_timer_start(ctx)) {
    ctx_ = nullptr;
    return false;
  }

  start_timer_internal(ctx);
  ctx.watchdog_ping();
  ctx.mark_ready();
  return true;
}

inline bool LauncherTimerComponent::on_activate(LauncherContext& ctx) {
  ctx_ = &ctx;

  if (!timer_) {
    start_timer_internal(ctx);
  }

  ctx.watchdog_ping();
  ctx.mark_ready();
  return true;
}

inline void LauncherTimerComponent::on_deactivate() { stop_timer_internal(); }

inline void LauncherTimerComponent::on_stop() {
  stop_timer_internal();
  on_timer_stop();
  ctx_ = nullptr;
}

inline bool LauncherTimerComponent::on_param_changed(std::string_view key, const LauncherParamValue& value) {
  if (key == "interval_ms" || key == "period_ms") {
    const auto* iptr = std::get_if<int64_t>(&value);

    if (iptr == nullptr || *iptr <= 0 || *iptr > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
      return false;
    }

    interval_ms_ = static_cast<uint32_t>(*iptr);

    if (timer_) {
      timer_->set_interval(interval_ms_);
    }

    return true;
  }

  return on_timer_param_changed(key, value);
}

inline uint32_t LauncherTimerComponent::interval_ms() const noexcept { return interval_ms_; }

inline bool LauncherTimerComponent::on_timer_configure(LauncherContext& ctx) {
  (void)ctx;
  return true;
}

inline bool LauncherTimerComponent::on_timer_start(LauncherContext& ctx) {
  (void)ctx;
  return true;
}

inline void LauncherTimerComponent::on_timer_stop() {}

inline bool LauncherTimerComponent::on_timer_param_changed(std::string_view key, const LauncherParamValue& value) {
  (void)key;
  (void)value;
  return true;
}

inline void LauncherTimerComponent::start_timer_internal(LauncherContext& ctx) {
  timer_ = ctx.create_timer(interval_ms_, [this] {
    if VUNLIKELY (ctx_ == nullptr) {
      return;
    }

    ctx_->watchdog_ping();

    if (!on_tick(*ctx_)) {
      ctx_->report_health(false, "timer tick returned false");
    }
  });
  timer_->start();
}

inline void LauncherTimerComponent::stop_timer_internal() {
  if (timer_) {
    timer_->stop();
    timer_.reset();
  }
}

#ifdef VLINK_ENABLE_COROUTINE
inline bool LauncherCoroutineComponent::on_start(LauncherContext& ctx) {
  if (!on_coro_start(ctx)) {
    return false;
  }

  return ctx.co_spawn(drive_coro(this, &ctx));
}

inline void LauncherCoroutineComponent::on_stop() { on_coro_stop(); }

inline bool LauncherCoroutineComponent::on_coro_start(LauncherContext& ctx) {
  (void)ctx;
  return true;
}

inline void LauncherCoroutineComponent::on_coro_stop() {}

inline Co::Task<void> LauncherCoroutineComponent::drive_coro(LauncherCoroutineComponent* self, LauncherContext* ctx) {
  co_await self->on_coro_run(*ctx);
}
#endif

}  // namespace vlink

/**
 * @def VLINK_LAUNCHER_MODULE_BEGIN(VersionMajor, VersionMinor)
 * @brief Opens a @c LauncherModuleInterface factory registration block inside a plugin @c .cc file.
 *
 * @details
 * Must appear at most once per shared library at namespace or global scope; paired with
 * @c VLINK_LAUNCHER_MODULE_END() to emit the @c VLINK_PLUGIN_DECLARE entry points.
 *
 * @param VersionMajor  Exported interface ABI major version (must equal @c LauncherModuleInterface::kVersionMajor).
 * @param VersionMinor  Exported interface ABI minor version (must be @c >= @c LauncherModuleInterface::kVersionMinor).
 */
// NOLINTBEGIN
#define VLINK_LAUNCHER_MODULE_BEGIN(VersionMajor, VersionMinor)                                                   \
  class VLinkLauncherModuleImpl final : public vlink::LauncherModuleInterface {                                   \
   public:                                                                                                        \
    static constexpr uint16_t kModuleMajor = (VersionMajor);                                                      \
    static constexpr uint16_t kModuleMinor = (VersionMinor);                                                      \
    static_assert(kModuleMajor == vlink::LauncherModuleInterface::kVersionMajor,                                  \
                  "Launcher module major ABI version must match vlink::LauncherModuleInterface::kVersionMajor."); \
    using FactoryFn = vlink::LauncherComponent* (*)();                                                            \
    VLinkLauncherModuleImpl();                                                                                    \
    std::vector<std::string> list_components() const override;                                                    \
    vlink::LauncherComponent* create_component(std::string_view class_name) const override;                       \
    void destroy_component(vlink::LauncherComponent* component) const noexcept override;                          \
                                                                                                                  \
   private:                                                                                                       \
    std::unordered_map<std::string, FactoryFn> factories_;                                                        \
    void register_all();                                                                                          \
  };                                                                                                              \
  inline VLinkLauncherModuleImpl::VLinkLauncherModuleImpl() { register_all(); }                                   \
  inline std::vector<std::string> VLinkLauncherModuleImpl::list_components() const {                              \
    std::vector<std::string> names;                                                                               \
    names.reserve(factories_.size());                                                                             \
    for (const auto& entry : factories_) {                                                                        \
      names.push_back(entry.first);                                                                               \
    }                                                                                                             \
    return names;                                                                                                 \
  }                                                                                                               \
  inline vlink::LauncherComponent* VLinkLauncherModuleImpl::create_component(std::string_view class_name) const { \
    if (class_name.empty() && factories_.size() == 1U) {                                                          \
      return factories_.begin()->second();                                                                        \
    }                                                                                                             \
    auto iter = factories_.find(std::string(class_name));                                                         \
    if (iter == factories_.end()) {                                                                               \
      return nullptr;                                                                                             \
    }                                                                                                             \
    return iter->second();                                                                                        \
  }                                                                                                               \
  inline void VLinkLauncherModuleImpl::destroy_component(vlink::LauncherComponent* component) const noexcept {    \
    delete component;                                                                                             \
  }                                                                                                               \
  inline void VLinkLauncherModuleImpl::register_all() {
// NOLINTEND

/**
 * @def VLINK_LAUNCHER_EXPORT_COMPONENT(ClassName, ComponentType)
 * @brief Registers @p ComponentType under @p ClassName inside a @c VLINK_LAUNCHER_MODULE_BEGIN block.
 *
 * @param ClassName      String literal class name referenced by TOML @c class or dynamic load requests.
 * @param ComponentType  Default-constructible concrete class deriving from @c vlink::LauncherComponent.
 */
// NOLINTBEGIN
#define VLINK_LAUNCHER_EXPORT_COMPONENT(ClassName, ComponentType) \
  factories_.emplace((ClassName), []() -> vlink::LauncherComponent* { return new ComponentType(); });
// NOLINTEND

/**
 * @def VLINK_LAUNCHER_MODULE_END()
 * @brief Closes the module registration block and exports the standard @c VLINK_PLUGIN_DECLARE ABI.
 */
// NOLINTBEGIN
#define VLINK_LAUNCHER_MODULE_END()                                                    \
  }                                                                                    \
  VLINK_PLUGIN_DECLARE(VLinkLauncherModuleImpl, VLinkLauncherModuleImpl::kModuleMajor, \
                       VLinkLauncherModuleImpl::kModuleMinor)
// NOLINTEND
