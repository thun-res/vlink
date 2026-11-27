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
 * @brief Embedded VLink process and component orchestrator server.
 *
 * @details
 * @c LauncherServer loads a TOML manifest describing @c [[process]] and @c [[component]] units,
 * starts them in DAG dependency order, monitors readiness and watchdogs (both in-process and via
 * @c LauncherNotify), handles restart backoff/circuit breaking, and optionally exposes a VLink
 * control plane consumable by @c LauncherAPI and @c vlink-launcher CLI.
 *
 * @see launcher_api.h, launcher_component.h, launcher_notify.h
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

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../base/functional.h"
#include "../base/macros.h"
#include "../base/message_loop.h"
#include "./launcher_api.h"

namespace vlink {

/**
 * @class LauncherServer
 * @brief Lightweight orchestrator managing @c [[process]] and @c [[component]] units.
 */
class VLINK_LAUNCHER_SERVER_EXPORT LauncherServer : public MessageLoop {
 public:
  using EventCallback = MoveFunction<void(const launcher::EventRecord&)>;

  /**
   * @struct Config
   * @brief Runtime configuration overrides for @c LauncherServer.
   */
  struct Config final {
    bool enable_control_plane{true};
    std::string control_scheme_override;
    std::string control_namespace_override;
    int32_t control_domain_override{-1};
    std::string security_key_override;
    bool disable_respawn{false};
    std::unordered_map<std::string, std::string> args_override;
    std::vector<std::string> only_targets;
    std::vector<std::string> skip_targets;
  };

  /**
   * @brief Constructs a @c LauncherServer with @p config.
   */
  explicit LauncherServer(const Config& config);

  /**
   * @brief Constructs a @c LauncherServer with default configuration.
   */
  LauncherServer();

  /**
   * @brief Destroys the server, stopping all managed units if still active.
   */
  ~LauncherServer() override;

  /**
   * @brief Loads and parses a TOML manifest from filesystem path @p file_path.
   */
  bool load_file(const std::string& file_path, std::string* error_out = nullptr);

  /**
   * @brief Loads and parses a TOML manifest from memory string @p toml_content.
   */
  bool load_string(const std::string& toml_content, const std::string& base_dir = ".",
                   std::string* error_out = nullptr);

  /**
   * @brief Runs static validation on the loaded manifest.
   *
   * @param check_programs  When @c true, verifies that @c [[process]] executables exist on host.
   * @return                Empty string when valid, or newline-separated diagnostic errors.
   */
  [[nodiscard]] std::string validate(bool check_programs = false) const;

  /**
   * @brief Registers a local callback notified for every unit lifecycle event.
   */
  void register_event_callback(EventCallback&& callback);

  /**
   * @brief Starts the orchestrator, notify server, control plane, and autostart units.
   */
  bool start();

  /**
   * @brief Asynchronously requests graceful shutdown with exit code @p exit_code.
   */
  void request_stop(int32_t exit_code = 0);

  /**
   * @brief Stops all managed units in reverse dependency order and waits for exit.
   */
  void stop();

  /**
   * @brief Waits until the server stops or @p timeout_ms elapses (@c -1 waits indefinitely).
   */
  bool wait(int32_t timeout_ms = -1);

  /**
   * @brief Returns @c true while the server is actively running.
   */
  [[nodiscard]] bool is_active() const noexcept;

  /**
   * @brief Returns the exit code recorded when shutdown was requested.
   */
  [[nodiscard]] int32_t get_exit_code() const noexcept;

  /**
   * @brief Returns a point-in-time @c SystemSnapshot of all managed units.
   */
  [[nodiscard]] launcher::SystemSnapshot get_snapshot() const;

  /**
   * @brief Starts a unit or @c \@group.
   */
  launcher::CommandResult start_unit(const std::string& target);

  /**
   * @brief Stops a unit or @c \@group.
   */
  launcher::CommandResult stop_unit(const std::string& target);

  /**
   * @brief Restarts a unit or @c \@group.
   */
  launcher::CommandResult restart_unit(const std::string& target);

  /**
   * @brief Reloads a unit (@c on_reload() or @c SIGHUP).
   */
  launcher::CommandResult reload_unit(const std::string& target);

  /**
   * @brief Returns the resolved DAG startup order of active units (after @c only/skip filtering).
   */
  [[nodiscard]] std::vector<std::string> startup_order() const;

  /**
   * @brief Queries typed parameters for component @p unit_name (empty returns all components).
   */
  [[nodiscard]] launcher::CommandResult get_params(const std::string& unit_name = "");

  /**
   * @brief Updates a live parameter on component @p unit_name.
   */
  launcher::CommandResult set_param(const std::string& unit_name, const launcher::ParamEntry& entry);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  void on_begin() override;
  void on_end() override;

  VLINK_DISALLOW_COPY_AND_ASSIGN(LauncherServer)
};

}  // namespace vlink
