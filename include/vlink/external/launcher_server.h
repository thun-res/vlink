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
 * @file launcher_server.h
 * @brief Daemon-side orchestration engine for VLink processes, components, and containers.
 *
 * @details
 * @c LauncherServer is the embedded and standalone runtime half of the VLink launcher
 * subsystem.  It inherits @c MessageLoop (mirroring @c ProxyServer) so orchestration
 * callbacks, readiness timers, watchdog checks, and shutdown sequencing execute inside the
 * server's own event loop once @c start() brings the runtime online.
 *
 * The server is responsible for the following duties:
 *
 * -# Parses and validates multi-file TOML manifests (@c [[include]] with cycle guards,
 *    @c [args], @c [env], @c [urls], @c [remaps], @c [params], @c [[group]], @c [[slice]],
 *    @c [[scheduler]], @c [[process]], @c [[component]], @c [[container]], @c [[task]],
 *    and @c [proxy]), resolving @c ${...} substitutions and boolean conditions statically.
 * -# Executes the unit startup DAG with orthogonal ordering (@c after, @c before, @c stage)
 *    and dependency semantics (@c requires, @c wants, @c binds_to, @c part_of, @c conflicts),
 *    honouring parallel, serial, and @c max_parallel policies and reverse-topological stop.
 * -# Hosts in-process @c LauncherComponent plugins (@c LauncherModuleInterface and legacy
 *    @c RunablePluginInterface) across @c dedicated, @c shared, @c multi, @c pinned, and
 *    @c pool schedulers, and manages isolated child @c [[container]] processes.
 * -# Supervises external processes and tasks with readiness probes, liveness / watchdog
 *    checks, exponential restart backoff, start-limit circuit breakers, platform resource
 *    limits (cgroup v2, @c prlimit, @c nice, affinity, JobObject), and ring-buffered logs.
 * -# Exposes a native VLink control plane (RPC @c Server, Field @c Setter, and Event
 *    @c Publisher streams) consumed by @c LauncherAPI clients and @c vlink-launcher @c ctl.
 *
 * @par Architecture Diagram
 * @verbatim
 *  +------------------------+              +------------------------+
 *  | LauncherAPI (CLI / UI) |              | Local C++ Embedder     |
 *  +-----------+------------+              +-----------+------------+
 *              |  RPC / Field / Event (optional AES)   | direct calls
 *              v                                       v
 *  +----------------------------------------------------------------+
 *  |                      LauncherServer (this)                     |
 *  |  +----------------+   +------------------+   +--------------+  |
 *  |  | Manifest & DAG |   | ComponentHost /  |   | LogManager & |  |
 *  |  | Orchestrator   |   | SchedulerManager |   | Resources    |  |
 *  |  +-------+--------+   +--------+---------+   +------+-------+  |
 *  |          |                     |                    |          |
 *  +----------|---------------------|--------------------|----------+
 *             v                     v                    v
 *     [External Processes]   [In-process Plugins]   [Container Procs]
 * @endverbatim
 *
 * @par Lifecycle and Orchestration
 * Load a manifest with @c load_file() or @c load_string() before calling @c start().
 * Unlike @c ProxyServer, @c start() launches the inherited @c MessageLoop automatically,
 * brings up schedulers, logging, resource governance, and the control plane, and begins
 * cold-boot DAG dispatch.  Call @c wait() to block until shutdown, @c request_stop() to
 * trigger asynchronous teardown from any thread or callback, or @c stop() to tear down
 * synchronously.
 *
 * @par Control Plane and Security
 * When @c Config::enable_control_plane is @c true, the server binds VLink control endpoints
 * under the resolved scheme, domain, and namespace prefix.  Setting @c Config::security_key
 * enables AES-128-GCM encryption on every control endpoint.  Remote dynamic shared-library
 * loading via the @c load_component() RPC over a non-@c intra, non-loopback transport
 * without @c security_key is rejected unless @c Config::allow_remote_load or the manifest
 * @c [launcher].remote_load option is explicitly enabled.
 *
 * @par Example
 * @code
 * vlink::LauncherServer::Config cfg;
 * cfg.control_scheme = "dds";
 * cfg.control_domain_id = 0;
 *
 * vlink::LauncherServer server(cfg);
 * std::string err;
 * if (server.load_file("system.toml", &err) && server.start()) {
 *   server.wait();
 * }
 * @endcode
 *
 * @note
 * - @c load_file() and @c load_string() must be called while the server is stopped.
 * - Destruction calls @c stop() automatically so managed processes, containers, and plugins
 *   are torn down before transport handles are released.
 *
 * @see launcher_api.h, launcher_component.h, extension/runnable_plugin_interface.h
 */

#pragma once

/// @cond
#undef VLINK_LAUNCHER_SERVER_EXPORT
#ifdef VLINK_LAUNCHER_SERVER_LIBRARY_STATIC
#define VLINK_LAUNCHER_SERVER_EXPORT
#elif defined(_WIN32) || defined(__CYGWIN__)
#ifdef VLINK_LAUNCHER_SERVER_LIBRARY
#define VLINK_LAUNCHER_SERVER_EXPORT __declspec(dllexport)
#else
#define VLINK_LAUNCHER_SERVER_EXPORT __declspec(dllimport)
#endif
#else
#define VLINK_LAUNCHER_SERVER_EXPORT __attribute__((visibility("default")))
#endif
/// @endcond

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../base/functional.h"
#include "../base/macros.h"
#include "../base/message_loop.h"
#include "./launcher_api.h"

namespace vlink {

/**
 * @class LauncherServer
 * @brief In-process and standalone VLink orchestration daemon backed by a @c MessageLoop.
 *
 * @details
 * Owns manifest compilation, the unit dependency state machine, in-process component hosting,
 * child process and container supervision, resource governance, log aggregation, and the
 * VLink control plane.  Call @c load_file() or @c load_string(), then @c start() to run the
 * inherited @c MessageLoop and dispatch the cold-boot DAG.
 */
class VLINK_LAUNCHER_SERVER_EXPORT LauncherServer final : public MessageLoop {
 public:
  /**
   * @struct Config
   * @brief Construction-time host overrides applied on top of the loaded TOML manifest.
   *
   * @details
   * Empty strings and negative integer overrides preserve the corresponding manifest setting
   * (or built-in default when omitted from the manifest).
   *
   * | Field                    | Default | Description                                                          |
   * | ------------------------ | ------- | -------------------------------------------------------------------- |
   * | @c control_scheme        | ""      | Control-plane transport scheme override; empty = TOML or auto.       |
   * | @c control_namespace     | ""      | Control-plane topic prefix override; empty = TOML or "launcher".     |
   * | @c control_domain_id     | -1      | Control-plane domain override; negative = TOML or 0.                 |
   * | @c security_key          | ""      | Optional AES-128-GCM key for control-plane channels.                 |
   * | @c bind_ip               | ""      | Optional transport bind IP for control-plane channels.               |
   * | @c native_mode           | false   | Restrict control-plane transport and discovery to local host.        |
   * | @c enable_control_plane  | true    | Expose VLink RPC/Field/Event control-plane endpoints.                |
   * | @c publish_logs          | true    | Publish captured unit logs onto the control-plane log stream.        |
   * | @c manage_console        | true    | Route host @c Logger console output through the launcher mux.        |
   * | @c color_output          | true    | Emit ANSI colour escapes when stdout is attached to a TTY.           |
   * | @c container_mode        | false   | Run as an isolated child component container process.                |
   * | @c container_name        | ""      | Container unit name when @c container_mode is @c true.               |
   * | @c log_dir_override      | ""      | Override session log directory path.                                 |
   * | @c log_level_override    | ""      | Override console log level (trace/debug/info/warn/error/fatal/off).  |
   * | @c log_plugin_override   | ""      | Override @c LoggerPluginInterface (e.g. "logd") library or path.     |
   * | @c record_dir_override   | ""      | Override active bag recording output directory and enable recording. |
   * | @c launcher_executable   | ""      | Path to @c vlink-launcher binary used when spawning containers.      |
   * | @c startup_mode_override | ""      | Override startup mode ("parallel" or "serial").                      |
   * | @c max_parallel_override | -1      | Override concurrent start limit; negative keeps manifest value.      |
   * | @c disable_respawn       | false   | Disable automatic respawn on unit failures.                          |
   * | @c pause_on_fail         | false   | Pause further cold-boot dispatches after the first failure.          |
   * | @c allow_remote_load     | false   | Allow unauthenticated remote dynamic shared-library loads.           |
   * | @c only_units            | {}      | Start only listed units / @c "\@group" selectors plus dependencies.  |
   * | @c skip_units            | {}      | Skip listed units / @c "\@group" selectors during cold boot.         |
   * | @c debug_prefixes        | {}      | Per-unit @c prefix debug overrides (e.g. gdb, valgrind).             |
   * | @c arg_overrides         | {}      | Manifest @c key=value argument overrides.                            |
   */
  struct Config final {
    std::string control_scheme;           ///< Control-plane scheme override (empty uses TOML or auto).
    std::string control_namespace;        ///< Control-plane prefix override (empty uses TOML or @c "launcher").
    int32_t control_domain_id{-1};        ///< Control-plane domain override (@c <0 uses TOML or @c 0).
    std::string security_key;             ///< Optional AES-128-GCM key for control-plane channels.
    std::string bind_ip;                  ///< Optional transport bind IP for control-plane channels.
    bool native_mode{false};              ///< Restrict control-plane transport and discovery to local host.
    bool enable_control_plane{true};      ///< Expose VLink RPC/Field/Event control endpoints.
    bool publish_logs{true};              ///< Publish captured unit logs onto the control-plane log topic.
    bool manage_console{true};            ///< Route host @c Logger console output through the launcher mux.
    bool color_output{true};              ///< Emit ANSI colour escapes when stdout is a TTY.
    bool container_mode{false};           ///< Run as an isolated component container child process.
    std::string container_name;           ///< Container name when @c container_mode is @c true.
    std::string log_dir_override;         ///< Override session log directory.
    std::string log_level_override;       ///< Override console log level (trace/debug/info/warn/error/fatal/off).
    std::string log_plugin_override;      ///< Override @c LoggerPluginInterface (@c logd) library name or path.
    std::string record_dir_override;      ///< Override active bag recording output directory (enables recording).
    std::string launcher_executable;      ///< Path to @c vlink-launcher binary for spawning containers.
    std::string startup_mode_override;    ///< Override startup mode (@c "parallel" or @c "serial").
    int32_t max_parallel_override{-1};    ///< Override max concurrent unit starts (@c <0 keeps TOML value).
    bool disable_respawn{false};          ///< Disable automatic respawn on unit failures.
    bool pause_on_fail{false};            ///< Pause further startup dispatches after the first failure.
    bool allow_remote_load{false};        ///< Allow unauthenticated remote dynamic shared-library loads.
    std::vector<std::string> only_units;  ///< Start only listed units / @c "\@group" selectors plus deps.
    std::vector<std::string> skip_units;  ///< Skip listed units / @c "\@group" selectors during cold boot.
    std::unordered_map<std::string, std::string> debug_prefixes;  ///< Per-unit @c prefix debug overrides.
    std::unordered_map<std::string, std::string> arg_overrides;   ///< Launch @c key=value argument overrides.
  };

  /**
   * @enum GraphFormat
   * @brief Output format selector for @c export_graph().
   */
  enum class GraphFormat : uint8_t {
    kDot = 0,      ///< Graphviz DOT syntax.
    kAscii = 1,    ///< Terminal-friendly layered ASCII diagram.
    kMermaid = 2,  ///< Mermaid @c flowchart @c TD syntax.
  };

  /**
   * @struct ValidationReport
   * @brief Structured diagnostics and unit-count summary returned by @c validate().
   */
  struct ValidationReport final {
    bool valid{false};                  ///< @c true when the manifest passed all static checks.
    std::vector<std::string> errors;    ///< Fatal configuration, conflict, or DAG errors.
    std::vector<std::string> warnings;  ///< Non-fatal advisories (e.g. cgroup delegation or transport warnings).
    size_t scheduler_group_count{0};    ///< Total configured scheduler groups.
    size_t slice_count{0};              ///< Total configured resource slices.
    size_t group_count{0};              ///< Total configured logical task groups.
    size_t process_count{0};            ///< Total configured processes.
    size_t component_count{0};          ///< Total configured components.
    size_t container_count{0};          ///< Total configured containers.
    size_t task_count{0};               ///< Total configured orchestrated tasks.
    bool has_proxy{false};              ///< Whether a managed proxy sidecar is configured.
  };

  /**
   * @struct PluginInfo
   * @brief Exported interface and version metadata returned by @c inspect_plugin().
   */
  struct PluginInfo final {
    std::string path;                     ///< Requested library stem or filesystem path.
    bool loaded{false};                   ///< @c true when the library loaded as a valid VLink plugin.
    bool is_launcher_module{false};       ///< @c true when @c LauncherModuleInterface is exported.
    bool is_legacy_runnable{false};       ///< @c true when legacy @c RunablePluginInterface is exported.
    bool is_logger_plugin{false};         ///< @c true when @c LoggerPluginInterface is exported.
    uint16_t version_major{0};            ///< Exported plugin ABI major version.
    uint16_t version_minor{0};            ///< Exported plugin ABI minor version.
    std::vector<std::string> components;  ///< Registered @c LauncherComponent class names.
    std::string error;                    ///< Diagnostic error message on load failure.
  };

  /**
   * @brief Callback invoked on unit lifecycle transitions.
   *
   * @param event  Emitted unit transition record.
   */
  using EventCallback = MoveFunction<void(const launcher::EventRecord& event)>;

  /**
   * @brief Callback invoked on captured unit log records.
   *
   * @param record  Captured log line metadata and text.
   */
  using LogCallback = MoveFunction<void(const launcher::LogRecord& record)>;

  /**
   * @brief Constructs a @c LauncherServer with default @c Config values.
   */
  LauncherServer();

  /**
   * @brief Constructs a @c LauncherServer with explicit host-level overrides.
   *
   * @param config  Runtime, control-plane, logging, recording, and debug overrides.
   */
  explicit LauncherServer(const Config& config);

  /**
   * @brief Stops all managed units, tears down the control plane, and destroys the server.
   *
   * @details
   * Calls @c stop() if the server is still active so child processes, containers, in-process
   * components, and control endpoints are released in deterministic reverse order.
   */
  ~LauncherServer() override;

  /**
   * @brief Inspects a plugin shared library and returns its exported VLink metadata.
   *
   * @details
   * Probes @c LauncherModuleInterface, legacy @c RunablePluginInterface, and
   * @c LoggerPluginInterface in turn using @c vlink::Plugin without starting any component
   * instances.
   *
   * @param library_path  Shared library stem or filesystem path.
   * @return              Populated @c PluginInfo structure describing the library.
   */
  [[nodiscard]] static PluginInfo inspect_plugin(const std::string& library_path);

  /**
   * @brief Loads, compiles, and validates a TOML launch manifest from @p file_path.
   *
   * @details
   * Resolves recursive @c [[include]] directives, @c ${...} substitutions, @c for_each /
   * @c count expansions, and group inheritance, then runs static validation (without checking
   * host executables) and builds the internal unit table.  Fails if the server is running.
   *
   * @param file_path  Path to the root TOML launch file.
   * @param error_out  Optional error string populated on parse or validation failure.
   * @return           @c true when the manifest parsed and validated cleanly.
   */
  [[nodiscard]] bool load_file(const std::string& file_path, std::string* error_out = nullptr);

  /**
   * @brief Loads, compiles, and validates an inline TOML launch manifest.
   *
   * @details
   * Behaves identically to @c load_file(), resolving relative @c [[include]] and file paths
   * against @p base_dir.  Fails if the server is running.
   *
   * @param toml_content  Inline TOML manifest document.
   * @param base_dir      Base directory used to resolve relative @c [[include]] and config paths.
   * @param error_out     Optional error string populated on parse or validation failure.
   * @return              @c true when the manifest parsed and validated cleanly.
   */
  [[nodiscard]] bool load_string(std::string_view toml_content, const std::string& base_dir = ".",
                                 std::string* error_out = nullptr);

  /**
   * @brief Validates the loaded manifest, dependency graph, conflicts, slices, and URLs.
   *
   * @param check_programs  When @c true, also verifies referenced process and task executables exist on host.
   * @return                Structured @c ValidationReport containing errors, warnings, and summary counts.
   */
  [[nodiscard]] ValidationReport validate(bool check_programs = false) const;

  /**
   * @brief Exports the startup dependency DAG and inferred dataflow topology in @p format.
   *
   * @param format  Target output syntax (@c kDot, @c kAscii, or @c kMermaid).
   * @return        Rendered topology text, or empty string if no manifest is loaded.
   */
  [[nodiscard]] std::string export_graph(GraphFormat format = GraphFormat::kDot) const;

  /**
   * @brief Exports the startup dependency DAG and dataflow topology in Graphviz DOT format.
   *
   * @return Graphviz DOT text, or empty string if no manifest is loaded.
   */
  [[nodiscard]] std::string export_dot() const;

  /**
   * @brief Returns a JSON representation of the resolved manifest, arguments, units, and dataflow edges.
   *
   * @return Formatted JSON document string.
   */
  [[nodiscard]] std::string inspect_manifest_json() const;

  /**
   * @brief Registers a local observer for unit lifecycle transition events.
   *
   * @details
   * Events are dispatched asynchronously on the server's inherited @c MessageLoop.  Pass an
   * empty callback to unregister.
   *
   * @param callback  Observer invoked on unit transitions; empty unregisters.
   */
  void register_event_callback(EventCallback&& callback);

  /**
   * @brief Registers a local observer for captured unit log lines.
   *
   * @details
   * Pass an empty callback to unregister.  Callbacks should be lightweight and must not block
   * on server shutdown.
   *
   * @param callback  Observer invoked on captured log lines; empty unregisters.
   */
  void register_log_callback(LogCallback&& callback);

  /**
   * @brief Starts the orchestrator, scheduler groups, control plane, and cold-boot DAG.
   *
   * @details
   * Initialises logging, resource governance, and scheduler loops for the loaded manifest,
   * starts the inherited @c MessageLoop via @c async_run(), brings up the control plane (if
   * enabled), publishes the initial snapshot, and begins cold-boot dispatch.  Returns @c true
   * immediately if already running.
   *
   * @return @c true when startup succeeded; @c false if no manifest is loaded or logging /
   *         control-plane initialisation fails.
   */
  [[nodiscard]] bool start();

  /**
   * @brief Requests asynchronous graceful shutdown of the running server.
   *
   * @details
   * Safe to invoke from any thread or callback without blocking.  Callers may subsequently
   * wait for completion via @c wait().
   *
   * @param exit_code  Exit code reported by @c get_exit_code() once shutdown completes.
   */
  void request_stop(int32_t exit_code = 0);

  /**
   * @brief Synchronously stops all managed units, containers, and control-plane endpoints.
   *
   * @details
   * Tears down active units in reverse topological order bounded by the configured stop
   * timeout, releases control-plane endpoints, shuts down schedulers and logging, and waits
   * for the inherited @c MessageLoop to exit.
   */
  void stop();

  /**
   * @brief Blocks until the server stops or @p timeout_ms elapses.
   *
   * @param timeout_ms  Maximum wait in milliseconds (@c -1 waits indefinitely).
   * @return            @c true when the server stopped before the timeout; @c false on timeout.
   */
  bool wait(int32_t timeout_ms = -1);

  /**
   * @brief Reports whether the server is currently running.
   *
   * @return @c true while active.
   */
  [[nodiscard]] bool is_active() const noexcept;

  /**
   * @brief Returns the exit code recorded when the server stopped.
   *
   * @return Process exit status code.
   */
  [[nodiscard]] int32_t get_exit_code() const noexcept;

  /**
   * @brief Captures a point-in-time snapshot of all managed units and host metrics.
   *
   * @return Copy of the current @c SystemSnapshot.
   */
  [[nodiscard]] launcher::SystemSnapshot get_snapshot() const;

  /**
   * @brief Starts @p target unit or @c "\@group" along with its required dependency closure.
   *
   * @param target  Unit name or @c "\@group" selector.
   * @return        Command outcome.
   */
  [[nodiscard]] launcher::CommandResult start_unit(const std::string& target);

  /**
   * @brief Stops @p target unit or @c "\@group" and any lifecycle-bound dependents.
   *
   * @param target  Unit name or @c "\@group" selector.
   * @return        Command outcome.
   */
  [[nodiscard]] launcher::CommandResult stop_unit(const std::string& target);

  /**
   * @brief Restarts @p target unit or @c "\@group".
   *
   * @param target  Unit name or @c "\@group" selector.
   * @return        Command outcome.
   */
  [[nodiscard]] launcher::CommandResult restart_unit(const std::string& target);

  /**
   * @brief Reloads @p target unit or @c "\@group" in place (@c SIGHUP or @c on_reload()).
   *
   * @param target  Unit name or @c "\@group" selector.
   * @return        Command outcome.
   */
  [[nodiscard]] launcher::CommandResult reload_unit(const std::string& target);

  /**
   * @brief Activates a standby (@c kInactive) component or @c "\@group".
   *
   * @param target  Component name or @c "\@group" selector.
   * @return        Command outcome.
   */
  [[nodiscard]] launcher::CommandResult activate_unit(const std::string& target);

  /**
   * @brief Suspends an active component or @c "\@group" into @c kInactive standby.
   *
   * @param target  Component name or @c "\@group" selector.
   * @return        Command outcome.
   */
  [[nodiscard]] launcher::CommandResult deactivate_unit(const std::string& target);

  /**
   * @brief Clears failed state and restart-limit counters for @p target (or all units when empty).
   *
   * @param target  Optional unit name or @c "\@group" selector; empty resets all units.
   * @return        Command outcome.
   */
  [[nodiscard]] launcher::CommandResult reset_failed(const std::string& target = "");

  /**
   * @brief Isolates @p group_name by stopping active units outside its closure and starting members.
   *
   * @param group_name  Target group name (with or without leading @c "\@").
   * @return            Command outcome.
   */
  [[nodiscard]] launcher::CommandResult isolate_group(const std::string& group_name);

  /**
   * @brief Returns the effective runtime configuration text for @p unit_name.
   *
   * @details
   * Reflects resolved substitutions, live parameter updates, and dynamically loaded components.
   *
   * @param unit_name  Target unit name.
   * @return           Command outcome carrying TOML configuration text in @c message.
   */
  [[nodiscard]] launcher::CommandResult get_unit_config(const std::string& unit_name) const;

  /**
   * @brief Queries captured session logs matching @p query from the server ring buffer.
   *
   * @param query  Filter criteria (unit, grep, sequence watermark, severity, limit).
   * @return       Command outcome carrying matched records in @c logs.
   */
  [[nodiscard]] launcher::CommandResult query_logs(const launcher::LogQuery& query) const;

  /**
   * @brief Dynamically loads a component shared library into this server or a managed container.
   *
   * @param request  Component specification, ports, scheduler, and initial parameters.
   * @return         Command outcome.
   */
  [[nodiscard]] launcher::CommandResult load_component(const launcher::LoadComponentRequest& request);

  /**
   * @brief Stops and unloads a dynamically loaded component by @p unit_name.
   *
   * @param unit_name  Target component unit name.
   * @return           Command outcome.
   */
  [[nodiscard]] launcher::CommandResult unload_component(const std::string& unit_name);

  /**
   * @brief Returns live parameters for @p unit_name (or global parameters when empty).
   *
   * @param unit_name  Optional target component name; empty queries global parameters.
   * @return           Command outcome carrying parameter entries in @c params.
   */
  [[nodiscard]] launcher::CommandResult get_params(const std::string& unit_name = "") const;

  /**
   * @brief Updates a live parameter on @p unit_name (or global parameters when empty).
   *
   * @param unit_name  Optional target component name; empty updates global parameters.
   * @param entry      Typed key/value parameter update.
   * @return           Command outcome.
   */
  [[nodiscard]] launcher::CommandResult set_param(const std::string& unit_name, const launcher::ParamEntry& entry);

  /**
   * @brief Dynamically starts or stops active bag recording on @p target component(s) or @c "\@group".
   *
   * @details
   * Updates recording hooks on in-process and container-hosted components.  External
   * @c [[process]] units do not support dynamic runtime toggling and return @c kUnsupported.
   *
   * @param target  Component name, @c "\@group" selector, or empty string for all managed components.
   * @param enable  @c true to start or switch recording; @c false to stop recording.
   * @param path    Optional explicit bag path (@c .vdb / @c .vdbx / @c .vcap / @c .vcapx); empty uses default.
   * @return        Command outcome.
   */
  [[nodiscard]] launcher::CommandResult set_record(const std::string& target, bool enable,
                                                   const std::string& path = "");

  /**
   * @brief Resumes cold-boot dispatch after @c pause_on_fail paused startup.
   *
   * @return Command outcome.
   */
  [[nodiscard]] launcher::CommandResult resume_startup();

  /**
   * @brief Returns the active session log directory path.
   *
   * @return Session log directory string.
   */
  [[nodiscard]] std::string get_log_dir() const;

 protected:
  /**
   * @brief Returns the maximum queued task capacity for the orchestrator @c MessageLoop.
   *
   * @return Maximum pending task count.
   */
  [[nodiscard]] size_t get_max_task_count() const override;

  /**
   * @brief Returns the maximum task queue latency threshold in milliseconds.
   *
   * @return Maximum elapsed time threshold in milliseconds.
   */
  [[nodiscard]] uint32_t get_max_elapsed_time() const override;

  /**
   * @brief Logs queued task latency exceeding @c get_max_elapsed_time() and still executes the lifecycle task.
   *
   * @param callback      Timed-out task callback.
   * @param elapsed_time  Actual queue wait latency in milliseconds.
   */
  void on_task_timeout(Callback&& callback, uint32_t elapsed_time) override;

  /**
   * @brief Lifecycle hook invoked when the orchestrator @c MessageLoop thread starts.
   */
  void on_begin() override;

  /**
   * @brief Lifecycle hook invoked when the orchestrator @c MessageLoop thread exits.
   */
  void on_end() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(LauncherServer)
};

}  // namespace vlink
