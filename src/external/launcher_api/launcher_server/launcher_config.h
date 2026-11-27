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

#pragma once

#include <vlink/external/launcher_api.h>
#include <vlink/external/launcher_component.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace vlink::launcher {

enum class OutputMode : uint8_t {
  kScreen = 0,
  kNone = 1,
};

enum class ReadyProbeType : uint8_t {
  kNone = 0,
  kComponent = 1,
  kNotify = 2,
  kStdout = 3,
  kDelay = 4,
};

enum class RestartPolicy : uint8_t {
  kNo = 0,
  kOnFailure = 1,
  kAlways = 2,
};

enum class SchedulerMode : uint8_t {
  kShared = 0,
  kDedicated = 1,
  kMulti = 2,
};

struct ReadyProbeConfig final {
  ReadyProbeType type{ReadyProbeType::kNone};
  std::string pattern;
  uint32_t delay_ms{0};
  uint32_t timeout_ms{10000};
};

struct RespawnConfig final {
  RestartPolicy policy{RestartPolicy::kNo};
  uint32_t delay_ms{1000};
  uint32_t max_delay_ms{30000};
  uint32_t start_burst{5};
  uint32_t start_window_ms{60000};

  [[nodiscard]] bool enabled() const noexcept { return policy != RestartPolicy::kNo; }
};

struct ProcessSpec final {
  std::string name;
  std::string ns;
  std::string group;
  std::string program;
  std::vector<std::string> args;
  std::string cwd;
  std::unordered_map<std::string, std::string> env;
  LauncherParamMap params;
  std::unordered_map<std::string, std::string> remaps;
  SchedulerMode scheduler{SchedulerMode::kDedicated};
  uint32_t threads{2};
  MessageLoop::Type queue_type{MessageLoop::kNormalType};
  MessageLoop::Strategy queue_strategy{MessageLoop::kOptimizationStrategy};
  bool inherit_env{true};
  bool autostart{true};
  bool required{false};
  uint32_t delay_ms{0};
  uint32_t stop_timeout_ms{5000};
  uint32_t watchdog_ms{0};
  OutputMode output{OutputMode::kScreen};
  RespawnConfig respawn;
  ReadyProbeConfig ready;
  std::vector<std::string> depends_on;
  std::vector<std::string> after;
};

struct ComponentSpec final {
  std::string name;
  std::string ns;
  std::string group;
  std::string library;
  std::string class_name;
  bool dedicated{false};
  SchedulerMode scheduler{SchedulerMode::kShared};
  uint32_t threads{2};
  MessageLoop::Type queue_type{MessageLoop::kNormalType};
  MessageLoop::Strategy queue_strategy{MessageLoop::kOptimizationStrategy};
  LauncherParamMap params;
  std::unordered_map<std::string, std::string> remaps;
  bool autostart{true};
  bool required{false};
  uint32_t delay_ms{0};
  uint32_t stop_timeout_ms{5000};
  uint32_t watchdog_ms{0};
  RespawnConfig respawn;
  ReadyProbeConfig ready;
  std::vector<std::string> depends_on;
  std::vector<std::string> after;
};

struct LaunchManifest final {
  std::string name{"vlink-launcher"};
  std::string ns;
  std::string source_file;
  bool control_enabled{true};
  std::string control_scheme;
  std::string control_namespace{"launcher"};
  uint32_t control_domain{0};
  std::string security_key;
  uint32_t shutdown_timeout_ms{10000};
  std::vector<std::string> plugin_dirs;
  std::vector<ProcessSpec> processes;
  std::vector<ComponentSpec> components;
};

struct ValidationReport final {
  std::vector<std::string> errors;

  [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
  [[nodiscard]] std::string format() const;
};

bool load_manifest_file(const std::string& file_path, LaunchManifest& out, std::string& error_out,
                        const std::unordered_map<std::string, std::string>& args_override = {});

bool load_manifest_string(const std::string& toml_content, const std::string& base_dir, LaunchManifest& out,
                          std::string& error_out,
                          const std::unordered_map<std::string, std::string>& args_override = {});

ValidationReport validate_manifest(const LaunchManifest& manifest, bool check_programs = false);

std::string params_map_to_json(const LauncherParamMap& params);

std::string string_map_to_json(const std::unordered_map<std::string, std::string>& kv);

}  // namespace vlink::launcher
