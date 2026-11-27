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
 * @brief Unified component interface, runtime context, plugin export macros, and standalone runner.
 *
 * @details
 * A single @c LauncherComponent implementation can run in two deployment modes with zero code
 * changes:
 * - **In-process**: Export via @c VLINK_LAUNCHER_MODULE_BEGIN / @c VLINK_LAUNCHER_EXPORT_COMPONENT
 *   (or @c VLINK_LAUNCHER_EXPORT_SINGLE_COMPONENT) and load through a @c [[component]] entry in
 *   @c LauncherServer.
 * - **Standalone process**: Link against @c vlink::launcher_api with @c VLINK_LAUNCHER_COMPONENT_MAIN
 *   (or launch the shared library via @c vlink-launcher @c exec).  When spawned by
 *   @c LauncherServer as a @c [[process]] unit, @c LauncherContext transparently bridges
 *   @c mark_ready(), @c report_health(), @c watchdog_ping(), parameters, and @c SIGHUP reload to
 *   @c LauncherNotify.
 *
 * @par Example
 * @code
 * #include <vlink/external/launcher_component.h>
 *
 * class SensorNode final : public vlink::LauncherComponent {
 *  public:
 *   bool on_start(vlink::LauncherContext& ctx) override {
 *     const int64_t rate = ctx.get_param_int("rate_hz", 10);
 *     (void)rate;
 *     ctx.mark_ready("sensor ready");
 *     return true;
 *   }
 *
 *   void on_stop(vlink::LauncherContext& ctx) override { (void)ctx; }
 * };
 *
 * // Build as shared plugin:
 * // VLINK_LAUNCHER_EXPORT_SINGLE_COMPONENT(SensorNode)
 * //
 * // Or build as standalone executable (links vlink::launcher_api):
 * // VLINK_LAUNCHER_COMPONENT_MAIN(SensorNode)
 * @endcode
 *
 * @see launcher_notify.h, launcher_api.h, launcher_server.h
 */

#pragma once

/// @cond
#undef VLINK_LAUNCHER_API_EXPORT
#ifdef VLINK_LAUNCHER_API_LIBRARY_STATIC
#define VLINK_LAUNCHER_API_EXPORT
#elif defined(_WIN32) || defined(__CYGWIN__)
#ifdef VLINK_LAUNCHER_API_LIBRARY
#define VLINK_LAUNCHER_API_EXPORT __declspec(dllexport)
#else
#define VLINK_LAUNCHER_API_EXPORT __declspec(dllimport)
#endif
#else
#define VLINK_LAUNCHER_API_EXPORT __attribute__((visibility("default")))
#endif
/// @endcond

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "../base/cancellation.h"
#include "../base/coroutine.h"
#include "../base/functional.h"
#include "../base/macros.h"
#include "../base/message_loop.h"
#include "../base/plugin.h"

#ifdef VLINK_ENABLE_COROUTINE
#include <chrono>
#include <exception>
#include <mutex>
#include <shared_mutex>

#include "../base/condition_variable.h"
#include "../base/logger.h"
#endif

namespace vlink {

/**
 * @brief Strongly-typed parameter value exchanged between @c LauncherServer and components.
 */
using LauncherParamValue = std::variant<bool, int64_t, double, std::string, std::vector<bool>, std::vector<int64_t>,
                                        std::vector<double>, std::vector<std::string>>;

/**
 * @brief Key-value map of typed component parameters.
 */
using LauncherParamMap = std::unordered_map<std::string, LauncherParamValue>;

/**
 * @class LauncherContext
 * @brief Runtime context passed to @c LauncherComponent lifecycle hooks.
 *
 * @details
 * Exposes unit metadata, the bound @c MessageLoop, typed parameter accessors, and unified
 * readiness / health / watchdog reporting that work identically whether hosted in-process by
 * @c LauncherServer or executed inside a standalone process runner.
 */
class VLINK_LAUNCHER_API_EXPORT LauncherContext final {
 public:
  /**
   * @brief Host hook bundle used by in-process hosts and standalone runners to back @c LauncherContext.
   */
  struct Hooks final {
    std::string name;            ///< Logical unit name.
    std::string ns;              ///< Optional unit namespace.
    MessageLoop* loop{nullptr};  ///< Bound message loop (serial for 1 worker, concurrent MultiLoop for >1).
    LauncherParamMap params;     ///< Initial typed parameter map.
    std::unordered_map<std::string, std::string> remaps;   ///< URL remapping table (source URL -> target URL).
    uint32_t watchdog_ms{0};                               ///< Configured watchdog timeout (ms).
    uint32_t worker_threads{1};                            ///< Worker thread count (>1 when backed by MultiLoop).
    bool standalone{false};                                ///< True in standalone runner mode.
    MoveFunction<void(std::string_view)> on_ready;         ///< Readiness hook.
    MoveFunction<void(bool, std::string_view)> on_health;  ///< Health reporting hook.
    MoveFunction<void()> on_watchdog;                      ///< Watchdog heartbeat hook.
    MoveFunction<void()> on_request_stop;                  ///< Self-stop request hook.
  };

  /**
   * @brief Constructs a runtime context backed by @p hooks.
   *
   * @param hooks  Host callbacks, metadata, and initial parameters.
   */
  explicit LauncherContext(Hooks&& hooks);

  /**
   * @brief Destroys the context.
   */
  ~LauncherContext();

  /**
   * @brief Returns the logical unit name.
   */
  [[nodiscard]] const std::string& name() const noexcept;

  /**
   * @brief Returns the logical namespace (may be empty).
   */
  [[nodiscard]] const std::string& ns() const noexcept;

  /**
   * @brief Returns the @c MessageLoop associated with this component.
   */
  [[nodiscard]] MessageLoop& loop() const noexcept;

  /**
   * @brief Returns @c true once shutdown has been requested for this component.
   */
  [[nodiscard]] bool is_stopping() const noexcept;

  /**
   * @brief Returns @c true when running via the standalone process runner.
   */
  [[nodiscard]] bool is_standalone() const noexcept;

  /**
   * @brief Returns the configured watchdog timeout in milliseconds (@c 0 when disabled).
   */
  [[nodiscard]] uint32_t watchdog_interval_ms() const noexcept;

  /**
   * @brief Returns the queue implementation backing @c loop().
   */
  [[nodiscard]] MessageLoop::Type queue_type() const noexcept;

  /**
   * @brief Returns the back-pressure strategy configured on @c loop().
   */
  [[nodiscard]] MessageLoop::Strategy queue_strategy() const noexcept;

  /**
   * @brief Returns the worker concurrency backing @c loop() (@c 1 for single-threaded, @c >1 for @c MultiLoop).
   */
  [[nodiscard]] uint32_t worker_threads() const noexcept;

  /**
   * @brief Returns a @c CancellationToken automatically signalled when shutdown is requested.
   */
  [[nodiscard]] CancellationToken cancellation_token() const;

  /**
   * @brief Checks whether parameter @p key is present.
   */
  [[nodiscard]] bool has_param(const std::string& key) const;

  /**
   * @brief Returns the typed parameter value for @p key, or @c std::nullopt if absent.
   */
  [[nodiscard]] std::optional<LauncherParamValue> get_param(const std::string& key) const;

  /**
   * @brief Reads a boolean parameter (accepts numeric / string fallbacks) or returns @p default_val.
   */
  [[nodiscard]] bool get_param_bool(const std::string& key, bool default_val = false) const;

  /**
   * @brief Reads a 64-bit integer parameter or returns @p default_val.
   */
  [[nodiscard]] int64_t get_param_int(const std::string& key, int64_t default_val = 0) const;

  /**
   * @brief Reads a double parameter or returns @p default_val.
   */
  [[nodiscard]] double get_param_double(const std::string& key, double default_val = 0.0) const;

  /**
   * @brief Reads a string parameter or returns @p default_val.
   */
  [[nodiscard]] std::string get_param_string(const std::string& key, const std::string& default_val = "") const;

  /**
   * @brief Returns a snapshot copy of all current parameters.
   */
  [[nodiscard]] LauncherParamMap params() const;

  /**
   * @brief Returns the configured URL remapping table.
   */
  [[nodiscard]] const std::unordered_map<std::string, std::string>& remaps() const noexcept;

  /**
   * @brief Resolves a VLink URL with configured @c remaps and runtime deployment mode.
   *
   * @details
   * If @p url matches an entry in @c remaps, the remapped URL is returned verbatim. Otherwise,
   * when running in-process (@c !is_standalone()) and @p inproc_scheme is non-empty, the scheme
   * prefix before @c "://" is rewritten to @p inproc_scheme (default @c "intra") while preserving
   * the rest of the URL so the same component switches between in-process and standalone transport
   * without code changes. Inside a @c vlink-launcher @c host subprocess, pass @p inproc_scheme = ""
   * or configure an explicit remap entry for URLs communicating across processes.
   */
  [[nodiscard]] std::string resolve_url(std::string_view url, std::string_view inproc_scheme = "intra") const;

  /**
   * @brief Reports that the component has finished initialisation and is ready.
   *
   * @param detail  Optional human-readable status message.
   */
  void mark_ready(std::string_view detail = "");

  /**
   * @brief Reports the component's current health state.
   *
   * @param healthy  @c true if healthy, @c false if degraded.
   * @param detail   Optional diagnostic message.
   */
  void report_health(bool healthy, std::string_view detail = "");

  /**
   * @brief Refreshes the software watchdog timestamp.
   */
  void watchdog_ping();

  /**
   * @brief Requests graceful shutdown of this component instance.
   */
  void request_stop();

  /// @cond
  void set_stopping(bool stopping) noexcept;
  void update_param_internal(const std::string& key, LauncherParamValue value);
  /// @endcond

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(LauncherContext)
};

/**
 * @class LauncherComponent
 * @brief Base class for reusable VLink nodes runnable both in-process and standalone.
 */
class VLINK_LAUNCHER_API_EXPORT LauncherComponent {
 public:
  /**
   * @brief Virtual destructor.
   */
  virtual ~LauncherComponent() = default;

  /**
   * @brief Starts the component and acquires its resources.
   *
   * @param ctx  Runtime context bound to this component instance.
   * @return     @c true on success, @c false to abort startup.
   */
  virtual bool on_start(LauncherContext& ctx) = 0;

  /**
   * @brief Stops the component and releases all resources.
   *
   * @param ctx  Runtime context bound to this component instance.
   */
  virtual void on_stop(LauncherContext& ctx) = 0;

  /**
   * @brief Optional live reload hook invoked on @c reload_unit() or @c SIGHUP.
   *
   * @param ctx  Runtime context bound to this component instance.
   * @return     @c true on success.
   */
  virtual bool on_reload(LauncherContext& ctx);

  /**
   * @brief Optional live parameter update hook invoked on @c set_param().
   *
   * @param ctx    Runtime context bound to this component instance.
   * @param key    Parameter name.
   * @param value  New typed parameter value.
   * @return       @c true to accept the parameter update, @c false to reject it.
   */
  virtual bool on_param_changed(LauncherContext& ctx, const std::string& key, const LauncherParamValue& value);

  /**
   * @brief Optional off-loop drain hook invoked after @c on_stop() and before component destruction.
   *
   * @details
   * Allows asynchronous components (such as @c LauncherCoroutineComponent) to finish draining
   * pending frames before derived class members are destroyed. Returning @c false tells the host
   * that asynchronous work is still in flight so destruction must be deferred.
   */
  virtual bool on_drain_stop(uint32_t timeout_ms) noexcept;

 protected:
  LauncherComponent() = default;

 private:
  VLINK_DISALLOW_COPY_AND_ASSIGN(LauncherComponent)
};

/**
 * @class LauncherTimerComponent
 * @brief Thin periodic @c LauncherComponent backed directly by @c vlink::Timer on @c ctx.loop().
 *
 * @details
 * Reads an optional @c period_ms parameter on startup, fires @c on_tick() periodically (with
 * automatic @c watchdog_ping() when enabled), and supports live @c period_ms updates via
 * @c set_param() in both in-process and standalone deployment modes.
 */
class VLINK_LAUNCHER_API_EXPORT LauncherTimerComponent : public LauncherComponent {
 public:
  /**
   * @brief Constructs a periodic component.
   *
   * @param default_period_ms  Fallback tick period in milliseconds when @c period_ms param is unset.
   * @param auto_watchdog      Automatically call @c ctx.watchdog_ping() on every tick when @c true.
   * @param auto_ready         Automatically call @c ctx.mark_ready() after timer startup when @c true.
   */
  explicit LauncherTimerComponent(uint32_t default_period_ms = 1000U, bool auto_watchdog = true,
                                  bool auto_ready = true);

  ~LauncherTimerComponent() override;

  bool on_start(LauncherContext& ctx) final;
  void on_stop(LauncherContext& ctx) final;
  bool on_param_changed(LauncherContext& ctx, const std::string& key, const LauncherParamValue& value) override;

  /**
   * @brief Returns the active tick period in milliseconds.
   */
  [[nodiscard]] uint32_t period_ms() const noexcept;

 protected:
  /**
   * @brief Optional initialisation hook invoked before the periodic timer starts.
   */
  virtual bool on_timer_init(LauncherContext& ctx);

  /**
   * @brief Periodic callback executed on @c ctx.loop().
   */
  virtual void on_tick(LauncherContext& ctx) = 0;

  /**
   * @brief Optional cleanup hook invoked after the periodic timer has stopped.
   */
  virtual void on_timer_stop(LauncherContext& ctx);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

#ifdef VLINK_ENABLE_COROUTINE
/**
 * @class LauncherCoroutineComponent
 * @brief Thin C++20 coroutine @c LauncherComponent backed directly by @c vlink::Coroutine.
 *
 * @details
 * Spawns @c run(ctx) onto @c ctx.loop() via @c Co::co_spawn. Shutdown automatically cancels
 * @c ctx.cancellation_token(), and @c on_drain_stop() drains coroutine completion off-loop
 * before derived-class destruction to guarantee zero dangling accesses in both in-process and
 * standalone modes.
 */
class LauncherCoroutineComponent : public LauncherComponent {
 public:
  explicit LauncherCoroutineComponent(bool auto_ready = true, uint32_t drain_timeout_ms = 3000U) noexcept
      : auto_ready_(auto_ready), drain_timeout_ms_(drain_timeout_ms) {}

  ~LauncherCoroutineComponent() override { (void)on_drain_stop(drain_timeout_ms_); }

  bool on_start(LauncherContext& ctx) final {
    if (!on_coro_init(ctx)) {
      return false;
    }

    loop_raw_ = &ctx.loop();
    loop_alive_ = ctx.loop().get_alive_state();
    coro_state_ = std::make_shared<CoroState>();
    Co::co_spawn(ctx.loop(), run_wrapper(this, &ctx, ctx.name(), DoneGuard{coro_state_}));

    if (auto_ready_) {
      ctx.mark_ready("coroutine ready");
    }

    return true;
  }

  void on_stop(LauncherContext& ctx) final { on_coro_stop(ctx); }

  bool on_drain_stop(uint32_t timeout_ms) noexcept override {
    if (!coro_state_) {
      return true;
    }

    if (loop_alive_ && loop_raw_ != nullptr) {
      std::shared_lock alive_lock(loop_alive_->mtx);

      if (loop_alive_->alive.load(std::memory_order_acquire) && loop_raw_->is_in_same_thread()) {
        std::lock_guard lock(coro_state_->mtx);
        return coro_state_->done;
      }
    }

    std::unique_lock lock(coro_state_->mtx);
    return coro_state_->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                    [this]() { return coro_state_->done; });
  }

 protected:
  virtual bool on_coro_init(LauncherContext& ctx) {
    (void)ctx;
    return true;
  }

  virtual Co::Task<void> run(LauncherContext& ctx) = 0;

  virtual void on_coro_stop(LauncherContext& ctx) { (void)ctx; }

 private:
  struct CoroState final {
    std::mutex mtx;
    ConditionVariable cv;
    bool done{false};
  };

  struct DoneGuard final {
    std::shared_ptr<CoroState> st;

    explicit DoneGuard(std::shared_ptr<CoroState> state) noexcept : st(std::move(state)) {}
    DoneGuard(DoneGuard&& other) noexcept = default;
    DoneGuard& operator=(DoneGuard&& other) noexcept = default;
    DoneGuard(const DoneGuard&) = delete;
    DoneGuard& operator=(const DoneGuard&) = delete;

    ~DoneGuard() {
      if (st) {
        {
          std::lock_guard lock(st->mtx);
          st->done = true;
        }

        st->cv.notify_all();
      }
    }
  };

  static Co::Task<void> run_wrapper(LauncherCoroutineComponent* self, LauncherContext* ctx, std::string unit_name,
                                    DoneGuard guard) {
    (void)guard;

    try {
      co_await self->run(*ctx);
    } catch (const Exception::OperationCancelled&) {
      // Cooperative shutdown cancellation.
    } catch (const std::exception& e) {
      VLOG_E("LauncherCoroutineComponent '", unit_name, "' exception: ", e.what());
    } catch (...) {
      VLOG_E("LauncherCoroutineComponent '", unit_name, "' unknown exception.");
    }
  }

  bool auto_ready_{true};
  uint32_t drain_timeout_ms_{3000U};
  MessageLoop* loop_raw_{nullptr};
  std::shared_ptr<MessageLoop::AliveState> loop_alive_;
  std::shared_ptr<CoroState> coro_state_;
};
#endif

/**
 * @class LauncherModuleInterface
 * @brief Factory plugin interface exported by shared libraries hosting @c LauncherComponent types.
 */
class LauncherModuleInterface {
  VLINK_PLUGIN_REGISTER_BY_ID(LauncherModuleInterface, "vlink.launcher.module")

 public:
  static constexpr uint16_t kVersionMajor = 1;  ///< Required ABI major version.
  static constexpr uint16_t kVersionMinor = 0;  ///< Minimum ABI minor version.

  virtual ~LauncherModuleInterface() = default;

  /**
   * @brief Lists all component class names exported by this shared library.
   */
  [[nodiscard]] virtual std::vector<std::string> list_components() const = 0;

  /**
   * @brief Instantiates a component registered under @p class_name.
   */
  [[nodiscard]] virtual LauncherComponent* create_component(std::string_view class_name) const = 0;

  /**
   * @brief Destroys a component previously created by @c create_component().
   */
  virtual void destroy_component(LauncherComponent* component) const noexcept = 0;

 protected:
  LauncherModuleInterface() = default;

 private:
  VLINK_DISALLOW_COPY_AND_ASSIGN(LauncherModuleInterface)
};

/**
 * @brief Runs @p component as a standalone OS process with automatic @c LauncherNotify bridging.
 *
 * @details
 * Parses @c VLINK_LAUNCHER_PARAMS_JSON and CLI arguments (@c --name, @c --ns, @c --param @c k=v),
 * binds a @c MessageLoop, installs @c SIGINT / @c SIGTERM / @c SIGHUP handlers, and bridges
 * @c LauncherContext calls to @c LauncherNotify when managed by @c LauncherServer.
 *
 * @param argc          Argument count from @c main().
 * @param argv          Argument vector from @c main().
 * @param component     Component instance to execute.
 * @param default_name  Fallback unit name when neither environment nor @c --name specifies one.
 * @return              Process exit code (@c 0 on clean shutdown, non-zero on startup failure).
 */
VLINK_LAUNCHER_API_EXPORT int run_launcher_component(int argc, char** argv, LauncherComponent& component,
                                                     std::string_view default_name = "");

/**
 * @brief Loads @p class_name from shared library @p library and runs it as a standalone process.
 *
 * @param argc          Argument count from @c main().
 * @param argv          Argument vector from @c main().
 * @param library       Shared library name or filesystem path.
 * @param class_name    Registered component class name (empty uses sole exported class).
 * @param default_name  Fallback unit name.
 * @return              Process exit code.
 */
VLINK_LAUNCHER_API_EXPORT int run_launcher_component_from_library(int argc, char** argv, std::string_view library,
                                                                  std::string_view class_name = "",
                                                                  std::string_view default_name = "");

/**
 * @brief Instantiates @p ComponentT and executes it via @c run_launcher_component().
 *
 * @tparam ComponentT   Default-constructible @c LauncherComponent subclass.
 * @param argc          Argument count from @c main().
 * @param argv          Argument vector from @c main().
 * @param default_name  Fallback unit name.
 * @return              Process exit code.
 */
template <typename ComponentT>
int run_launcher_component(int argc, char** argv, std::string_view default_name = "");

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

template <typename ComponentT>
int run_launcher_component(int argc, char** argv, std::string_view default_name) {
  ComponentT component;
  return run_launcher_component(argc, argv, component, default_name);
}

namespace detail {

class LauncherModuleBase : public LauncherModuleInterface {
 public:
  using FactoryFn = LauncherComponent* (*)();

  [[nodiscard]] std::vector<std::string> list_components() const override {
    std::vector<std::string> names;
    names.reserve(factories_.size());

    for (const auto& entry : factories_) {
      names.push_back(entry.first);
    }

    return names;
  }

  [[nodiscard]] LauncherComponent* create_component(std::string_view class_name) const override {
    if (class_name.empty() && factories_.size() == 1U) {
      return factories_.begin()->second();
    }

    const auto iter = factories_.find(std::string(class_name));

    if (iter == factories_.end()) {
      return nullptr;
    }

    return iter->second();
  }

  void destroy_component(LauncherComponent* component) const noexcept override { delete component; }

 protected:
  std::unordered_map<std::string, FactoryFn> factories_;
};

}  // namespace detail

}  // namespace vlink

// NOLINTBEGIN
#define VLINK_LAUNCHER_MODULE_BEGIN(VersionMajor, VersionMinor)                                                   \
  class VLinkLauncherModuleImpl final : public vlink::detail::LauncherModuleBase {                                \
   public:                                                                                                        \
    static constexpr uint16_t kModuleMajor = (VersionMajor);                                                      \
    static constexpr uint16_t kModuleMinor = (VersionMinor);                                                      \
    static_assert(kModuleMajor == vlink::LauncherModuleInterface::kVersionMajor,                                  \
                  "Launcher module major ABI version must match vlink::LauncherModuleInterface::kVersionMajor."); \
    VLinkLauncherModuleImpl() { register_all(); }                                                                 \
                                                                                                                  \
   private:                                                                                                       \
    void register_all();                                                                                          \
  };                                                                                                              \
  inline void VLinkLauncherModuleImpl::register_all() {
#define VLINK_LAUNCHER_EXPORT_COMPONENT(ClassName, ComponentType) \
  factories_.emplace((ClassName), []() -> vlink::LauncherComponent* { return new ComponentType(); });

#define VLINK_LAUNCHER_MODULE_END()                                                    \
  }                                                                                    \
  VLINK_PLUGIN_DECLARE(VLinkLauncherModuleImpl, VLinkLauncherModuleImpl::kModuleMajor, \
                       VLinkLauncherModuleImpl::kModuleMinor)

#define VLINK_LAUNCHER_EXPORT_SINGLE_COMPONENT(ComponentType)    \
  VLINK_LAUNCHER_MODULE_BEGIN(1, 0)                              \
  VLINK_LAUNCHER_EXPORT_COMPONENT(#ComponentType, ComponentType) \
  VLINK_LAUNCHER_MODULE_END()

#define VLINK_LAUNCHER_COMPONENT_MAIN(ComponentType) \
  int main(int argc, char** argv) { return vlink::run_launcher_component<ComponentType>(argc, argv, #ComponentType); }
// NOLINTEND
