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
 * @c LauncherAPI connects to a running @c LauncherServer over VLink's native communication
 * primitives and exposes synchronous unit-control RPCs alongside asynchronous connectivity,
 * snapshot, and lifecycle event callbacks.
 *
 * @see launcher_server.h, launcher_component.h, launcher_notify.h
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
#include <string_view>
#include <vector>

#include "../base/functional.h"
#include "../base/macros.h"
#include "../base/message_loop.h"
#include "./launcher_component.h"

namespace vlink {

namespace launcher {

/**
 * @enum UnitType
 * @brief Execution carrier category managed by @c LauncherServer.
 */
enum class UnitType : uint8_t {
  kProcess = 0,    ///< Managed external OS process.
  kComponent = 1,  ///< In-process dynamically loaded @c LauncherComponent.
};

/**
 * @enum UnitState
 * @brief Lifecycle state of a unit managed by @c LauncherServer.
 */
enum class UnitState : uint8_t {
  kPending = 0,   ///< Waiting for predecessor units or startup delay.
  kStarting = 1,  ///< Launching process or invoking @c on_start().
  kRunning = 2,   ///< Active, awaiting readiness probe when configured.
  kReady = 3,     ///< Ready to serve downstream dependents.
  kStopping = 4,  ///< Graceful shutdown in progress.
  kStopped = 5,   ///< Cleanly stopped.
  kFailed = 6,    ///< Failed to start, timed out, or exceeded restart budget.
};

/**
 * @enum ErrorCode
 * @brief Status code returned by @c LauncherAPI and @c LauncherServer control operations.
 */
enum class ErrorCode : uint8_t {
  kOk = 0,                ///< Operation succeeded.
  kInvalidArgument = 1,   ///< Missing or malformed request field.
  kNotFound = 2,          ///< Target unit, group, or parameter does not exist.
  kAlreadyRunning = 3,    ///< Target unit is already active.
  kNotRunning = 4,        ///< Target unit is not active.
  kDependencyFailed = 5,  ///< Required predecessor failed.
  kUnsupported = 6,       ///< Operation is not supported for this unit type.
  kTimeout = 7,           ///< Operation or RPC timed out.
  kDisconnected = 8,      ///< Control channel is not connected to @c LauncherServer.
  kVersionMismatch = 9,   ///< Control-plane protocol mismatch.
  kUnauthorized = 10,     ///< Control-plane authentication failed.
  kInternalError = 11,    ///< Unexpected server-side error.
};

/**
 * @enum ParamType
 * @brief Wire discriminant for typed parameters in control-plane payloads.
 */
enum class ParamType : uint8_t {
  kBool = 0,
  kInt = 1,
  kDouble = 2,
  kString = 3,
  kBoolList = 4,
  kIntList = 5,
  kDoubleList = 6,
  kStringList = 7,
};

/**
 * @struct ParamEntry
 * @brief Wire-serialisable key-value entry carrying a typed component parameter.
 */
struct ParamEntry final {
  std::string key;
  ParamType type{ParamType::kString};
  std::string value;
};

/**
 * @struct UnitStatus
 * @brief Point-in-time status of a single unit managed by @c LauncherServer.
 */
struct UnitStatus final {
  std::string name;
  std::string ns;
  std::string group;
  UnitType type{UnitType::kProcess};
  UnitState state{UnitState::kPending};
  int64_t pid{-1};
  int32_t exit_code{0};
  uint32_t restart_count{0};
  uint64_t generation{0};
  uint64_t start_time_ms{0};
  uint64_t uptime_ms{0};
  bool healthy{true};
  bool required{false};
  std::string failed_reason;
  std::string detail;
  std::vector<std::string> dependencies;
};

/**
 * @struct SystemSnapshot
 * @brief Aggregate status snapshot of the @c LauncherServer and all managed units.
 */
struct SystemSnapshot final {
  std::string session_id;
  std::string version;
  std::string host;
  int64_t server_pid{-1};
  uint64_t timestamp_ms{0};
  uint64_t uptime_ms{0};
  std::string config_file;
  std::vector<UnitStatus> units;
};

/**
 * @struct EventRecord
 * @brief Lifecycle event emitted when a unit transitions state or reports a lifecycle signal.
 */
struct EventRecord final {
  uint64_t timestamp_ms{0};
  std::string unit_name;
  UnitType unit_type{UnitType::kProcess};
  UnitState old_state{UnitState::kPending};
  UnitState new_state{UnitState::kPending};
  std::string event_name;
  std::string message;
};

/**
 * @struct CommandResult
 * @brief Unified result returned by @c LauncherAPI and @c LauncherServer control operations.
 */
struct CommandResult final {
  ErrorCode code{ErrorCode::kOk};
  std::string message;
  std::optional<SystemSnapshot> snapshot;
  std::vector<ParamEntry> params;

  [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::kOk; }
};

/**
 * @brief Converts @p type to its canonical lowercase name.
 */
[[nodiscard]] VLINK_LAUNCHER_API_EXPORT std::string_view unit_type_name(UnitType type) noexcept;

/**
 * @brief Converts @p state to its canonical lowercase name.
 */
[[nodiscard]] VLINK_LAUNCHER_API_EXPORT std::string_view unit_state_name(UnitState state) noexcept;

/**
 * @brief Converts @p code to its canonical lowercase name.
 */
[[nodiscard]] VLINK_LAUNCHER_API_EXPORT std::string_view error_code_name(ErrorCode code) noexcept;

/**
 * @brief Encodes a typed @c LauncherParamValue into a wire @c ParamEntry.
 */
[[nodiscard]] VLINK_LAUNCHER_API_EXPORT ParamEntry param_value_to_entry(const std::string& key,
                                                                        const LauncherParamValue& value);

/**
 * @brief Decodes a wire @c ParamEntry back into a typed @c LauncherParamValue.
 */
[[nodiscard]] VLINK_LAUNCHER_API_EXPORT bool entry_to_param_value(const ParamEntry& entry, LauncherParamValue& out);

}  // namespace launcher

/**
 * @class LauncherAPI
 * @brief Client SDK for querying and controlling a running @c LauncherServer.
 */
class VLINK_LAUNCHER_API_EXPORT LauncherAPI : public MessageLoop {
 public:
  using ErrorCode = launcher::ErrorCode;
  using ConnectCallback = MoveFunction<void(bool connected)>;
  using SnapshotCallback = MoveFunction<void(const launcher::SystemSnapshot&)>;
  using EventCallback = MoveFunction<void(const launcher::EventRecord&)>;

  /**
   * @struct Config
   * @brief Connection configuration for @c LauncherAPI.
   */
  struct Config final {
    std::string scheme;
    std::string namespace_prefix{"launcher"};
    uint32_t domain_id{0};
    std::string security_key;
    uint32_t default_timeout_ms{3000};
  };

  /**
   * @brief Constructs a @c LauncherAPI bound to @p config.
   */
  explicit LauncherAPI(const Config& config);

  /**
   * @brief Constructs a @c LauncherAPI with default configuration.
   */
  LauncherAPI();

  /**
   * @brief Destroys the client and shuts down its control channels.
   */
  ~LauncherAPI() override;

  /**
   * @brief Registers a callback notified when server connectivity changes.
   */
  void register_connect_callback(ConnectCallback&& callback);

  /**
   * @brief Registers a callback notified whenever a fresh @c SystemSnapshot arrives.
   */
  void register_snapshot_callback(SnapshotCallback&& callback);

  /**
   * @brief Registers a callback notified for every streamed @c EventRecord.
   */
  void register_event_callback(EventCallback&& callback);

  /**
   * @brief Returns @c true when the client is currently connected to a live @c LauncherServer.
   */
  [[nodiscard]] bool is_connected() const noexcept;

  /**
   * @brief Blocks until connected to @c LauncherServer or @p timeout_ms elapses.
   */
  bool wait_for_connected(uint32_t timeout_ms = 3000);

  /**
   * @brief Returns the active server session identifier, or empty if disconnected.
   */
  [[nodiscard]] std::string session_id() const;

  /**
   * @brief Returns the most recently cached @c SystemSnapshot, if any.
   */
  [[nodiscard]] std::optional<launcher::SystemSnapshot> latest_snapshot() const;

  /**
   * @brief Fetches a fresh @c SystemSnapshot via synchronous RPC.
   */
  [[nodiscard]] launcher::CommandResult get_status(uint32_t timeout_ms = 0);

  /**
   * @brief Starts a unit or @c \@group.
   */
  launcher::CommandResult start_unit(const std::string& target, uint32_t timeout_ms = 0);

  /**
   * @brief Stops a unit or @c \@group.
   */
  launcher::CommandResult stop_unit(const std::string& target, uint32_t timeout_ms = 0);

  /**
   * @brief Restarts a unit or @c \@group.
   */
  launcher::CommandResult restart_unit(const std::string& target, uint32_t timeout_ms = 0);

  /**
   * @brief Reloads a unit (@c on_reload() for components, @c SIGHUP for processes).
   */
  launcher::CommandResult reload_unit(const std::string& target, uint32_t timeout_ms = 0);

  /**
   * @brief Queries typed parameters for @p unit_name (empty queries all components).
   */
  [[nodiscard]] launcher::CommandResult get_params(const std::string& unit_name = "", uint32_t timeout_ms = 0);

  /**
   * @brief Updates a live parameter on component @p unit_name.
   */
  launcher::CommandResult set_param(const std::string& unit_name, const launcher::ParamEntry& entry,
                                    uint32_t timeout_ms = 0);

  /**
   * @brief Requests graceful shutdown of the remote @c LauncherServer.
   */
  launcher::CommandResult shutdown_server(uint32_t timeout_ms = 0);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  void on_begin() override;
  void on_end() override;

  VLINK_DISALLOW_COPY_AND_ASSIGN(LauncherAPI)
};

}  // namespace vlink
