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
 * @file launcher_api.h
 * @brief Client-side C++ SDK and control-plane data model for connecting to a @c LauncherServer.
 *
 * @details
 * @c LauncherAPI is the consumer-facing half of the VLink launcher subsystem.  It connects to
 * a running @c LauncherServer over VLink's native communication primitives and exposes both
 * synchronous RPC commands (unit lifecycle, group isolation, dynamic component load/unload,
 * live parameter updates, dynamic recording, log queries, and graceful server shutdown) and
 * asynchronous observers for connectivity, system snapshots, lifecycle events, and log streams.
 * The class inherits @c MessageLoop so heartbeat timeout detection and callback delivery run
 * serialised on the loop thread once @c async_run() or @c run() has been started.
 *
 * @par Control-Plane Channels
 *
 * | Channel    | VLink Primitive                   | Purpose                                                |
 * | ---------- | --------------------------------- | ------------------------------------------------------ |
 * | Command    | RPC @c Client / @c Server         | Synchronous control RPCs returning @c CommandResult.   |
 * | State      | Field @c Getter / @c Setter       | Cached latest @c SystemSnapshot delivery.              |
 * | Heartbeat  | Event @c Subscriber / @c Publisher | 1-second server heartbeat and session change tracking. |
 * | Events     | Event @c Subscriber / @c Publisher | Streamed @c EventRecord unit lifecycle transitions.    |
 * | Logs       | Event @c Subscriber / @c Publisher | Streamed @c LogRecord lines (when @c subscribe_logs).  |
 *
 * @par Lifecycle States
 *
 * | @c UnitState   | Value | Description                                                         |
 * | -------------- | ----- | ------------------------------------------------------------------- |
 * | @c kPending    |   0   | Waiting for DAG predecessors, concurrency slot, or startup delay.   |
 * | @c kStarting   |   1   | Launching process or invoking @c on_configure() / @c on_start().    |
 * | @c kRunning    |   2   | Active, awaiting readiness probe when configured.                   |
 * | @c kReady      |   3   | Ready to serve downstream dependents.                               |
 * | @c kStopping   |   4   | Graceful shutdown in progress.                                      |
 * | @c kStopped    |   5   | Cleanly stopped or completed.                                       |
 * | @c kRestarting |   6   | Waiting in respawn backoff window before relaunch.                  |
 * | @c kFailed     |   7   | Failed to start or exceeded restart budget.                         |
 * | @c kSkipped    |   8   | Skipped because condition evaluated false or dependency failed.     |
 * | @c kInactive   |   9   | Configured in standby; may be activated without re-configuring.     |
 *
 * @par Error Codes
 *
 * | @c ErrorCode         | Value | Description                                                   |
 * | -------------------- | ----- | ------------------------------------------------------------- |
 * | @c kOk               |   0   | Operation succeeded.                                          |
 * | @c kInvalidArgument  |   1   | Missing or malformed request field.                           |
 * | @c kNotFound         |   2   | Target unit, group, or parameter does not exist.              |
 * | @c kAlreadyExists    |   3   | Unit name is already registered.                              |
 * | @c kAlreadyRunning   |   4   | Target unit is already active.                                |
 * | @c kNotRunning       |   5   | Target unit is not active.                                    |
 * | @c kDependencyFailed |   6   | Required predecessor failed or did not become ready.          |
 * | @c kUnsupported      |   7   | Operation is not supported for the target unit type.          |
 * | @c kTimeout          |   8   | Operation or RPC invocation timed out.                        |
 * | @c kDisconnected     |   9   | Control channel is not connected to @c LauncherServer.        |
 * | @c kVersionMismatch  |  10   | Control-plane protocol or VLink version mismatch.             |
 * | @c kUnauthorized     |  11   | Security key or remote component-loading policy rejected RPC. |
 * | @c kInternalError    |  12   | Unexpected server-side failure.                               |
 *
 * @par Connectivity and Heartbeat
 * The server broadcasts a 1-second heartbeat carrying its session ID and protocol version.
 * Once the inherited @c MessageLoop is running, @c LauncherAPI monitors heartbeat freshness
 * and fires @c ConnectCallback with @c connected=false after five seconds of silence.  When a
 * server restarts with a new session ID, @c ConnectCallback fires again with @c connected=true.
 * Passing @c Config::security_key wraps every control-plane primitive in @c Security* AES-128-GCM
 * channels; both client and server must agree on the key, scheme, domain, and namespace prefix.
 *
 * @par Example
 * @code
 * vlink::LauncherAPI::Config cfg;
 * cfg.scheme = "dds";
 * cfg.namespace_prefix = "launcher";
 * cfg.domain_id = 0;
 *
 * vlink::LauncherAPI api(cfg);
 * api.register_snapshot_callback([](const vlink::launcher::SystemSnapshot& snap) {
 *   // inspect snap.units
 * });
 * api.async_run();
 *
 * if (api.wait_for_connected(3000)) {
 *   auto res = api.get_status();
 *   if (res.ok()) {
 *     // use res.snapshot
 *   }
 * }
 * @endcode
 *
 * @note
 * - Start the inherited @c MessageLoop with @c async_run() or @c run() to enable heartbeat
 *   timeout detection and serialised callback dispatch.
 * - All RPC helpers accept an optional @c timeout_ms; passing @c 0 uses
 *   @c Config::default_timeout_ms.
 *
 * @see launcher_server.h, launcher_component.h
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

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../base/functional.h"
#include "../base/macros.h"
#include "../base/message_loop.h"
#include "launcher_component.h"

namespace vlink {

namespace launcher {

/**
 * @enum UnitType
 * @brief Execution carrier category managed by @c LauncherServer.
 */
enum class UnitType : uint8_t {
  kProcess = 0,    ///< Managed external child process.
  kComponent = 1,  ///< In-process dynamically loaded plugin component.
  kContainer = 2,  ///< Dedicated child process hosting a group of plugin components.
  kTask = 3,       ///< Oneshot or periodic orchestrated task.
  kProxy = 4,      ///< Managed @c vlink-proxy sidecar process.
};

/**
 * @enum UnitState
 * @brief Lifecycle state of a unit managed by @c LauncherServer.
 */
enum class UnitState : uint8_t {
  kPending = 0,     ///< Waiting for DAG predecessors, concurrency slot, or startup delay.
  kStarting = 1,    ///< Launching binary or invoking @c on_configure() / @c on_start().
  kRunning = 2,     ///< Active, waiting for readiness probe when configured.
  kReady = 3,       ///< Ready to serve downstream dependents.
  kStopping = 4,    ///< Graceful shutdown in progress.
  kStopped = 5,     ///< Cleanly stopped or completed.
  kRestarting = 6,  ///< Waiting in respawn backoff window before relaunch.
  kFailed = 7,      ///< Failed to start or crashed beyond restart budget.
  kSkipped = 8,     ///< Skipped because condition evaluated false or dependency failed.
  kInactive = 9,    ///< Configured in standby; can be activated without re-configuring.
};

/**
 * @enum ErrorCode
 * @brief Wire-level status code returned by launcher control-plane RPCs.
 */
enum class ErrorCode : uint8_t {
  kOk = 0,                ///< Operation succeeded.
  kInvalidArgument = 1,   ///< Missing or malformed request field.
  kNotFound = 2,          ///< Target unit, group, or parameter does not exist.
  kAlreadyExists = 3,     ///< Unit name already registered.
  kAlreadyRunning = 4,    ///< Target unit is already active.
  kNotRunning = 5,        ///< Target unit is not active.
  kDependencyFailed = 6,  ///< Required predecessor is unavailable or failed.
  kUnsupported = 7,       ///< Operation not supported for this unit type.
  kTimeout = 8,           ///< Operation or RPC timed out.
  kDisconnected = 9,      ///< Control channel is not connected to server.
  kVersionMismatch = 10,  ///< Protocol or VLink version mismatch.
  kUnauthorized = 11,     ///< Control-plane security or remote component-loading policy rejected the request.
  kInternalError = 12,    ///< Unexpected server-side failure.
};

/**
 * @enum ParamType
 * @brief Discriminator for serialised parameter entries on the control plane.
 */
enum class ParamType : uint8_t {
  kBool = 0,        ///< Scalar boolean value.
  kInt = 1,         ///< 64-bit signed integer value.
  kDouble = 2,      ///< 64-bit IEEE floating-point value.
  kString = 3,      ///< UTF-8 string value.
  kBoolList = 4,    ///< Homogeneous boolean array.
  kIntList = 5,     ///< Homogeneous 64-bit integer array.
  kDoubleList = 6,  ///< Homogeneous 64-bit floating-point array.
  kStringList = 7,  ///< Homogeneous UTF-8 string array.
};

/**
 * @struct ParamEntry
 * @brief Typed key-value parameter entry exchanged across the control plane.
 *
 * @details
 * Scalar values are encoded canonically as strings (@c "true" / @c "false", decimal integers,
 * 17-digit round-trip doubles, or raw UTF-8).  List types are encoded as JSON arrays.  Use
 * @c encode_param_entry() and @c decode_param_entry() to convert to and from
 * @c LauncherParamValue.
 */
struct ParamEntry final {
  std::string key;                     ///< Dot-delimited parameter key.
  ParamType type{ParamType::kString};  ///< Value type discriminator.
  std::string value;                   ///< Canonical string or JSON literal encoding of the value.
};

/**
 * @struct UnitStatus
 * @brief Runtime telemetry, dependency, resource, and startup-timing snapshot for a single unit.
 */
struct UnitStatus final {
  std::string name;                       ///< Unique unit identifier.
  std::string ns;                         ///< Logical namespace.
  std::string group;                      ///< Scheduler group name (components) or execution group.
  std::string task_group;                 ///< Logical @c [[group]] target name (e.g. @c "perception").
  std::string slice;                      ///< Resource @c [[slice]] name (cgroup slice).
  std::string cgroup_path;                ///< Active Linux cgroup v2 path (empty when inactive).
  std::string container;                  ///< Owning container unit name (empty when hosted in root server).
  UnitType type{UnitType::kProcess};      ///< Unit execution carrier category.
  UnitState state{UnitState::kPending};   ///< Current lifecycle state.
  int32_t stage{0};                       ///< Startup stage order (lower stages start earlier).
  int64_t pid{-1};                        ///< OS PID for process/container/proxy, or host PID for component.
  int32_t exit_code{0};                   ///< Most recent process exit code.
  uint32_t restart_count{0};              ///< Number of automatic restarts performed.
  uint32_t liveness_failures{0};          ///< Consecutive liveness / watchdog failure counter.
  uint32_t thread_count{0};               ///< Live OS thread count (when available).
  int32_t worker_index{-1};               ///< Pinned worker slot for @c pinned scheduler (@c -1 = auto).
  uint64_t start_time_ms{0};              ///< Epoch milliseconds when unit last started.
  uint64_t uptime_ms{0};                  ///< Elapsed runtime in milliseconds while active.
  uint64_t queued_offset_ms{0};           ///< Monotonic offset from server start when queued.
  uint64_t started_offset_ms{0};          ///< Monotonic offset from server start when launch began.
  uint64_t ready_offset_ms{0};            ///< Monotonic offset from server start when ready was reached.
  uint64_t startup_duration_ms{0};        ///< Startup activation latency (@c ready_offset_ms - @c started_offset_ms).
  uint64_t generation{0};                 ///< Monotonic launch generation counter.
  uint64_t memory_rss_kb{0};              ///< Resident memory (KiB) of primary process.
  uint64_t cgroup_memory_kb{0};           ///< Cgroup @c memory.current in KiB (@c 0 when unavailable).
  double cpu_usage{-1.0};                 ///< Measured CPU percentage (@c <0 when unavailable).
  bool healthy{true};                     ///< Latest health status.
  bool required{false};                   ///< Whether unexpected failure triggers system shutdown.
  std::string failed_reason;              ///< Structured failure cause (e.g. @c "start_limit", @c "watchdog").
  std::string blocker_unit;               ///< Last predecessor unit that unblocked cold-boot startup.
  std::string detail;                     ///< Human-readable status or last error message.
  std::vector<std::string> dependencies;  ///< Effective ordering/requirement dependencies (@c requires / @c after).
  std::vector<std::string> wants;         ///< Weak dependencies (@c wants).
  std::vector<std::string> binds_to;      ///< Lifecycle-bound units (@c binds_to).
  std::vector<std::string> conflicts;     ///< Mutually exclusive units (@c conflicts).
};

/**
 * @struct SystemSnapshot
 * @brief Full point-in-time status report of a @c LauncherServer instance.
 */
struct SystemSnapshot final {
  std::string session_id;         ///< Unique server session identifier.
  std::string version;            ///< VLink version string.
  std::string host;               ///< Host machine name.
  int64_t server_pid{-1};         ///< OS PID of the launcher server process.
  uint64_t timestamp_ms{0};       ///< Snapshot generation timestamp (epoch ms).
  uint64_t uptime_ms{0};          ///< Launcher server uptime in milliseconds.
  uint64_t boot_duration_ms{0};   ///< Elapsed ms until initial DAG reached steady state (@c 0 while booting).
  double cpu_usage{-1.0};         ///< Host system CPU usage percentage (@c <0 when unavailable).
  double memory_usage{-1.0};      ///< Host system memory usage percentage (@c <0 when unavailable).
  std::string startup_mode;       ///< Active startup mode (@c "parallel", @c "serial", or @c "max_parallel").
  uint32_t max_parallel{0};       ///< Effective startup concurrency limit (@c 0 = unlimited).
  std::string log_dir;            ///< Active session log root directory.
  std::string config_file;        ///< Loaded root TOML configuration path (empty for in-memory load).
  std::vector<UnitStatus> units;  ///< Status list for all managed units (including container components).
};

/**
 * @struct EventRecord
 * @brief Asynchronous lifecycle or system notification emitted by @c LauncherServer.
 */
struct EventRecord final {
  uint64_t timestamp_ms{0};                  ///< Event timestamp (epoch ms).
  std::string unit_name;                     ///< Source unit name (or @c "launcher" for system events).
  UnitType unit_type{UnitType::kProcess};    ///< Source unit type.
  UnitState old_state{UnitState::kPending};  ///< Previous lifecycle state.
  UnitState new_state{UnitState::kPending};  ///< New lifecycle state.
  std::string event_name;                    ///< Event tag (e.g. @c "on_start", @c "on_ready", @c "on_exit").
  std::string message;                       ///< Diagnostic detail.
};

/**
 * @struct LogRecord
 * @brief Streaming or historical log entry emitted by a managed unit or the launcher host.
 */
struct LogRecord final {
  uint64_t seq{0};           ///< Monotonic per-session log sequence number.
  uint64_t timestamp_ms{0};  ///< Log timestamp (epoch ms).
  std::string unit_name;     ///< Source unit name.
  uint8_t level{2};          ///< @c Logger::Level numeric value (@c 0..5).
  bool is_stderr{false};     ///< @c true when captured from child stderr.
  std::string text;          ///< Single log line with ANSI sequences stripped.
};

/**
 * @struct LogQuery
 * @brief Filter specification for querying recent logs from the server ring buffer.
 */
struct LogQuery final {
  std::string unit_filter;  ///< Unit name or @c "\@group" filter (empty = all units).
  std::string grep;         ///< Substring match filter (empty = disabled).
  uint64_t after_seq{0};    ///< Return records strictly after @c after_seq (@c 0 = tail mode).
  uint8_t min_level{0};     ///< Minimum @c Logger::Level numeric value (@c 0..5).
  bool stderr_only{false};  ///< Return only stderr records when @c true.
  uint32_t limit{100};      ///< Maximum records to return (clamped server-side).
};

/**
 * @struct LoadComponentRequest
 * @brief Request payload for dynamically loading an in-process component into a running server.
 */
struct LoadComponentRequest final {
  std::string name;                                      ///< Unique component instance name.
  std::string ns;                                        ///< Optional namespace.
  std::string library;                                   ///< Shared library stem or filesystem path.
  std::string class_name;                                ///< Registered @c LauncherComponent class name.
  std::string scheduler_group;                           ///< Target scheduler group name (empty selects default).
  std::string compute_pool;                              ///< Optional @c pool scheduler group name.
  std::string container;                                 ///< Target container unit name (empty loads in server).
  int32_t worker_index{-1};                              ///< Worker slot for @c pinned scheduler (@c -1 = auto).
  uint16_t task_priority{0};                             ///< Default dispatch priority hint.
  bool legacy_runnable{false};                           ///< @c true to load legacy @c RunablePluginInterface.
  bool auto_start{true};                                 ///< @c true to configure and start immediately after load.
  std::vector<ParamEntry> params;                        ///< Initial component parameters.
  std::unordered_map<std::string, std::string> urls;     ///< Named general/RPC endpoint URLs.
  std::unordered_map<std::string, std::string> inputs;   ///< Named input (subscribed) endpoint URLs.
  std::unordered_map<std::string, std::string> outputs;  ///< Named output (published) endpoint URLs.
  std::unordered_map<std::string, std::string> remaps;   ///< Per-component URL remap table.
};

/**
 * @struct CommandResult
 * @brief Outcome returned by synchronous launcher control commands and RPCs.
 */
struct CommandResult final {
  ErrorCode code{ErrorCode::kOk};  ///< Wire status code.
  std::string message;             ///< Human-readable message, unit config text, or error detail.
  std::vector<ParamEntry> params;  ///< Returned parameters for parameter query commands.
  std::vector<LogRecord> logs;     ///< Returned log history for @c query_logs().
  SystemSnapshot snapshot;         ///< Returned snapshot for status queries.

  /**
   * @brief Reports whether the command succeeded.
   *
   * @return @c true when @c code equals @c ErrorCode::kOk.
   */
  [[nodiscard]] VLINK_LAUNCHER_API_EXPORT bool ok() const noexcept;
};

/**
 * @brief Converts @c UnitType to a lowercase string label.
 *
 * @param type  Unit carrier type enum value.
 * @return      Null-terminated static string label.
 */
[[nodiscard]] VLINK_LAUNCHER_API_EXPORT const char* unit_type_to_string(UnitType type) noexcept;

/**
 * @brief Converts @c UnitState to a lowercase string label.
 *
 * @param state  Unit lifecycle state enum value.
 * @return       Null-terminated static string label.
 */
[[nodiscard]] VLINK_LAUNCHER_API_EXPORT const char* unit_state_to_string(UnitState state) noexcept;

/**
 * @brief Converts @c ErrorCode to a lowercase string label.
 *
 * @param code  Control-plane error code enum value.
 * @return      Null-terminated static string label.
 */
[[nodiscard]] VLINK_LAUNCHER_API_EXPORT const char* error_code_to_string(ErrorCode code) noexcept;

/**
 * @brief Formats a @c LauncherParamValue into a wire @c ParamEntry.
 *
 * @details
 * Preserves lossless 17-digit IEEE-754 double precision and encodes list variants as JSON
 * arrays.
 *
 * @param key    Dot-delimited parameter key name.
 * @param value  Typed parameter value.
 * @return       Serialised @c ParamEntry.
 */
[[nodiscard]] VLINK_LAUNCHER_API_EXPORT ParamEntry encode_param_entry(std::string_view key,
                                                                      const LauncherParamValue& value);

/**
 * @brief Parses a wire @c ParamEntry back into a typed @c LauncherParamValue.
 *
 * @param entry  Input serialised parameter entry.
 * @param out    Destination variant populated on success.
 * @return       @c true when parsing succeeded; @c false on malformed payload.
 */
[[nodiscard]] VLINK_LAUNCHER_API_EXPORT bool decode_param_entry(const ParamEntry& entry, LauncherParamValue& out);

}  // namespace launcher

/**
 * @class LauncherAPI
 * @brief Client-side launcher monitoring and control surface backed by a @c MessageLoop.
 *
 * @details
 * Connects to a @c LauncherServer over VLink's native RPC, Field, and Event primitives and
 * exposes callback registration for connectivity, snapshots, lifecycle events, and log lines
 * alongside synchronous control commands.  Inherits @c MessageLoop; start the loop with
 * @c async_run() or @c run() so heartbeat timeout tracking and serialised callback dispatch
 * execute on the client's loop thread.
 */
class VLINK_LAUNCHER_API_EXPORT LauncherAPI : public MessageLoop {
 public:
  /**
   * @struct Config
   * @brief Construction-time connection configuration for @c LauncherAPI.
   *
   * @details
   * @c scheme, @c namespace_prefix, @c domain_id, and @c security_key must match the target
   * @c LauncherServer control-plane configuration.
   *
   * | Field                 | Default    | Description                                                       |
   * | --------------------- | ---------- | ----------------------------------------------------------------- |
   * | @c scheme             | ""         | Transport scheme; empty auto-probes dds/ddsc/ddsr/zenoh/intra.    |
   * | @c namespace_prefix   | "launcher" | Control-plane topic prefix; must match server.                    |
   * | @c domain_id          | 0          | Transport domain ID; must match server.                           |
   * | @c security_key       | ""         | Optional AES-128-GCM symmetric key (@c Security::Config::key).    |
   * | @c bind_ip            | ""         | Optional DDS/Zenoh local bind IP.                                 |
   * | @c native_mode        | false      | Restrict discovery and supported transports to local host.        |
   * | @c subscribe_events   | true       | Subscribe to the unit lifecycle event stream.                     |
   * | @c subscribe_logs     | false      | Subscribe to the live unit log stream.                            |
   * | @c default_timeout_ms | 5000       | Default RPC wait/invoke timeout in milliseconds.                  |
   */
  struct Config final {
    std::string scheme;  ///< Transport scheme (empty = auto @c dds / @c ddsc / @c ddsr / @c zenoh / @c intra).
    std::string namespace_prefix{"launcher"};  ///< Control-plane topic prefix (must match server).
    uint32_t domain_id{0};                     ///< Transport domain ID.
    std::string security_key;                  ///< Optional AES-128-GCM symmetric key (@c Security::Config::key).
    std::string bind_ip;                       ///< Optional DDS/Zenoh bind IP.
    bool native_mode{false};                   ///< Restrict discovery and supported transports to local host.
    bool subscribe_events{true};               ///< Subscribe to lifecycle event stream.
    bool subscribe_logs{false};                ///< Subscribe to unit log stream.
    uint32_t default_timeout_ms{5000};         ///< Default RPC wait/invoke timeout in milliseconds.
  };

  /**
   * @brief Callback invoked when server connectivity transitions.
   *
   * @details
   * Fires with @p connected = @c true on the first valid heartbeat or upon server session
   * change, and with @p connected = @c false after five seconds of heartbeat silence.
   *
   * @param connected  @c true once connected; @c false on heartbeat timeout or disconnect.
   */
  using ConnectCallback = MoveFunction<void(bool connected)>;

  /**
   * @brief Callback invoked when a new system status snapshot arrives.
   *
   * @param snapshot  Received system snapshot.
   */
  using SnapshotCallback = MoveFunction<void(const launcher::SystemSnapshot& snapshot)>;

  /**
   * @brief Callback invoked on streamed unit lifecycle transition events.
   *
   * @param event  Received lifecycle transition record.
   */
  using EventCallback = MoveFunction<void(const launcher::EventRecord& event)>;

  /**
   * @brief Callback invoked on streamed unit log records.
   *
   * @param record  Received log line record.
   */
  using LogCallback = MoveFunction<void(const launcher::LogRecord& record)>;

  /**
   * @brief Constructs a @c LauncherAPI client and initialises its control-plane handles.
   *
   * @details
   * Resolves the effective transport scheme and creates the RPC client, state getter,
   * heartbeat subscriber, and optional event/log subscribers.  The inherited @c MessageLoop
   * is @b not started here -- call @c async_run() or @c run() to arm the heartbeat watchdog
   * timer and serialise callback dispatch.
   *
   * @param config  Control-plane transport, namespace, domain, security, and subscription settings.
   */
  explicit LauncherAPI(const Config& config);

  /**
   * @brief Destroys the client, releasing transport handles and stopping the event loop.
   */
  ~LauncherAPI() override;

  /**
   * @brief Returns the configuration bound to this client.
   *
   * @return Const reference to the internal @c Config aggregate.
   */
  [[nodiscard]] const Config& get_current_config() const noexcept;

  /**
   * @brief Registers a callback for server connectivity transitions.
   *
   * @details
   * If the client is already connected at registration time, @p callback is scheduled (or
   * invoked immediately when the loop is not running) with @c connected=true.  Pass an empty
   * callback to unregister.
   *
   * @param callback  Observer callback; empty unregisters.
   */
  void register_connect_callback(ConnectCallback&& callback);

  /**
   * @brief Registers a callback for incoming system status snapshots.
   *
   * @details
   * If a snapshot is already cached at registration time, @p callback is scheduled (or invoked
   * immediately when the loop is not running) with the latest cached snapshot.  Pass an empty
   * callback to unregister.
   *
   * @param callback  Observer callback; empty unregisters.
   */
  void register_snapshot_callback(SnapshotCallback&& callback);

  /**
   * @brief Registers a callback for streamed unit lifecycle events.
   *
   * @details
   * Requires @c Config::subscribe_events to be @c true.  There is no immediate invocation at
   * registration time.  Pass an empty callback to unregister.
   *
   * @param callback  Observer callback; empty unregisters.
   */
  void register_event_callback(EventCallback&& callback);

  /**
   * @brief Registers a callback for streamed unit log lines.
   *
   * @details
   * Requires @c Config::subscribe_logs to be @c true.  There is no immediate invocation at
   * registration time.  Pass an empty callback to unregister.
   *
   * @param callback  Observer callback; empty unregisters.
   */
  void register_log_callback(LogCallback&& callback);

  /**
   * @brief Reports whether the client is connected via live server heartbeat or RPC transport.
   *
   * @return @c true when connected; @c false otherwise.
   */
  [[nodiscard]] bool is_connected() const noexcept;

  /**
   * @brief Blocks until the server RPC endpoint connects or the timeout elapses.
   *
   * @param timeout_ms  Wait timeout in milliseconds (@c 0 uses @c Config::default_timeout_ms).
   * @return            @c true when connected before timeout; @c false otherwise.
   */
  [[nodiscard]] bool wait_for_connected(uint32_t timeout_ms = 0);

  /**
   * @brief Returns the latest cached system snapshot received from the server, if any.
   *
   * @return Cached @c SystemSnapshot, or @c std::nullopt before the first update.
   */
  [[nodiscard]] std::optional<launcher::SystemSnapshot> get_cached_snapshot() const;

  /**
   * @brief Requests an up-to-date system snapshot from the server via RPC.
   *
   * @param timeout_ms  RPC timeout in milliseconds (@c 0 uses @c Config::default_timeout_ms).
   * @return            Command outcome carrying the snapshot on success.
   */
  [[nodiscard]] launcher::CommandResult get_status(uint32_t timeout_ms = 0);

  /**
   * @brief Starts a unit or task group (@c "\@group").
   *
   * @param target      Unit name or @c "\@group" selector.
   * @param timeout_ms  RPC timeout in milliseconds (@c 0 uses @c Config::default_timeout_ms).
   * @return            Command outcome.
   */
  [[nodiscard]] launcher::CommandResult start_unit(const std::string& target, uint32_t timeout_ms = 0);

  /**
   * @brief Stops a unit or task group (@c "\@group").
   *
   * @param target      Unit name or @c "\@group" selector.
   * @param timeout_ms  RPC timeout in milliseconds (@c 0 uses @c Config::default_timeout_ms).
   * @return            Command outcome.
   */
  [[nodiscard]] launcher::CommandResult stop_unit(const std::string& target, uint32_t timeout_ms = 0);

  /**
   * @brief Restarts a unit or task group (@c "\@group").
   *
   * @param target      Unit name or @c "\@group" selector.
   * @param timeout_ms  RPC timeout in milliseconds (@c 0 uses @c Config::default_timeout_ms).
   * @return            Command outcome.
   */
  [[nodiscard]] launcher::CommandResult restart_unit(const std::string& target, uint32_t timeout_ms = 0);

  /**
   * @brief Reloads a running unit or group in place (@c SIGHUP for processes, @c on_reload() for components).
   *
   * @param target      Unit name or @c "\@group" selector.
   * @param timeout_ms  RPC timeout in milliseconds (@c 0 uses @c Config::default_timeout_ms).
   * @return            Command outcome.
   */
  [[nodiscard]] launcher::CommandResult reload_unit(const std::string& target, uint32_t timeout_ms = 0);

  /**
   * @brief Activates a configured standby component or group (@c on_activate()).
   *
   * @param target      Component name or @c "\@group" selector.
   * @param timeout_ms  RPC timeout in milliseconds (@c 0 uses @c Config::default_timeout_ms).
   * @return            Command outcome.
   */
  [[nodiscard]] launcher::CommandResult activate_unit(const std::string& target, uint32_t timeout_ms = 0);

  /**
   * @brief Suspends an active component or group into @c kInactive standby without releasing configured resources.
   *
   * @param target      Component name or @c "\@group" selector.
   * @param timeout_ms  RPC timeout in milliseconds (@c 0 uses @c Config::default_timeout_ms).
   * @return            Command outcome.
   */
  [[nodiscard]] launcher::CommandResult deactivate_unit(const std::string& target, uint32_t timeout_ms = 0);

  /**
   * @brief Clears failed state and restart-limit counters for a unit, group, or all units.
   *
   * @param target      Optional unit name or @c "\@group" selector; empty resets all units.
   * @param timeout_ms  RPC timeout in milliseconds (@c 0 uses @c Config::default_timeout_ms).
   * @return            Command outcome.
   */
  [[nodiscard]] launcher::CommandResult reset_failed(const std::string& target = "", uint32_t timeout_ms = 0);

  /**
   * @brief Isolates a task group by stopping active units outside its dependency closure and starting members.
   *
   * @param group_name  Target group name (with or without leading @c "\@").
   * @param timeout_ms  RPC timeout in milliseconds (@c 0 uses @c Config::default_timeout_ms).
   * @return            Command outcome.
   */
  [[nodiscard]] launcher::CommandResult isolate_group(const std::string& group_name, uint32_t timeout_ms = 0);

  /**
   * @brief Retrieves the effective runtime configuration text of a unit.
   *
   * @param unit_name   Target unit name.
   * @param timeout_ms  RPC timeout in milliseconds (@c 0 uses @c Config::default_timeout_ms).
   * @return            Command outcome carrying TOML configuration text in @c message.
   */
  [[nodiscard]] launcher::CommandResult get_unit_config(const std::string& unit_name, uint32_t timeout_ms = 0);

  /**
   * @brief Queries recent historical logs from the server ring buffer.
   *
   * @param query       Filter criteria (unit, grep, sequence watermark, severity, limit).
   * @param timeout_ms  RPC timeout in milliseconds (@c 0 uses @c Config::default_timeout_ms).
   * @return            Command outcome carrying matched log records in @c logs.
   */
  [[nodiscard]] launcher::CommandResult query_logs(const launcher::LogQuery& query, uint32_t timeout_ms = 0);

  /**
   * @brief Dynamically loads a shared-library component into the server or a target container.
   *
   * @param request     Component specification, ports, scheduler, and initial parameters.
   * @param timeout_ms  RPC timeout in milliseconds (@c 0 uses @c Config::default_timeout_ms).
   * @return            Command outcome.
   */
  [[nodiscard]] launcher::CommandResult load_component(const launcher::LoadComponentRequest& request,
                                                       uint32_t timeout_ms = 0);

  /**
   * @brief Stops and unloads a dynamically loaded component instance.
   *
   * @param unit_name   Target component unit name.
   * @param timeout_ms  RPC timeout in milliseconds (@c 0 uses @c Config::default_timeout_ms).
   * @return            Command outcome.
   */
  [[nodiscard]] launcher::CommandResult unload_component(const std::string& unit_name, uint32_t timeout_ms = 0);

  /**
   * @brief Queries live parameters of @p unit_name (or global parameters when empty).
   *
   * @param unit_name   Optional target component name; empty queries global parameters.
   * @param timeout_ms  RPC timeout in milliseconds (@c 0 uses @c Config::default_timeout_ms).
   * @return            Command outcome carrying parameter entries in @c params.
   */
  [[nodiscard]] launcher::CommandResult get_params(const std::string& unit_name = "", uint32_t timeout_ms = 0);

  /**
   * @brief Updates a single live parameter on @p unit_name (or global parameters when empty).
   *
   * @param unit_name   Optional target component name; empty updates global parameters.
   * @param entry       Typed key/value parameter update.
   * @param timeout_ms  RPC timeout in milliseconds (@c 0 uses @c Config::default_timeout_ms).
   * @return            Command outcome.
   */
  [[nodiscard]] launcher::CommandResult set_param(const std::string& unit_name, const launcher::ParamEntry& entry,
                                                  uint32_t timeout_ms = 0);

  /**
   * @brief Dynamically starts or stops active bag recording on in-process or container component(s).
   *
   * @param target      Component name, @c "\@group" selector, or empty string for all managed components.
   * @param enable      @c true to start or switch recording; @c false to stop recording.
   * @param path        Optional explicit bag path (@c .vdb / @c .vdbx / @c .vcap / @c .vcapx); empty uses default.
   * @param timeout_ms  RPC timeout in milliseconds (@c 0 uses @c Config::default_timeout_ms).
   * @return            Command outcome.
   */
  [[nodiscard]] launcher::CommandResult set_record(const std::string& target, bool enable, const std::string& path = "",
                                                   uint32_t timeout_ms = 0);

  /**
   * @brief Resumes a startup sequence paused by @c --pause_on_fail.
   *
   * @param timeout_ms  RPC timeout in milliseconds (@c 0 uses @c Config::default_timeout_ms).
   * @return            Command outcome.
   */
  [[nodiscard]] launcher::CommandResult resume_startup(uint32_t timeout_ms = 0);

  /**
   * @brief Requests graceful shutdown of the connected @c LauncherServer.
   *
   * @param timeout_ms  RPC timeout in milliseconds (@c 0 uses @c Config::default_timeout_ms).
   * @return            Command outcome.
   */
  [[nodiscard]] launcher::CommandResult shutdown_server(uint32_t timeout_ms = 0);

 protected:
  /**
   * @brief Returns the maximum queued task capacity for the client @c MessageLoop.
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
   * @brief Lifecycle hook invoked when the client @c MessageLoop thread starts.
   */
  void on_begin() override;

  /**
   * @brief Lifecycle hook invoked when the client @c MessageLoop thread exits.
   */
  void on_end() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(LauncherAPI)
};

}  // namespace vlink
