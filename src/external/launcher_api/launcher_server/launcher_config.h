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

#include <vlink/base/logger.h>
#include <vlink/base/message_loop.h>
#include <vlink/external/launcher_component.h>
#include <vlink/external/launcher_server.h>
//
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vlink {

namespace launcher {

enum class OutputMode : uint8_t {
  kScreen = 0,
  kLog = 1,
  kBoth = 2,
  kNone = 3,
};

enum class ReadyProbeType : uint8_t {
  kNone = 0,
  kComponent = 1,
  kDelay = 2,
  kStdout = 3,
  kFile = 4,
  kTopic = 5,
};

enum class OnExitAction : uint8_t {
  kIgnore = 0,
  kShutdown = 1,
  kRestart = 2,
};

enum class RestartPolicy : uint8_t {
  kNo = 0,
  kOnFailure = 1,
  kOnAbnormal = 2,
  kAlways = 3,
};

enum class KillMode : uint8_t {
  kProcess = 0,
  kControlGroup = 1,
  kMixed = 2,
};

enum class StopSignal : uint8_t {
  kSigTerm = 0,
  kSigInt = 1,
  kSigHup = 2,
  kSigKill = 3,
};

enum class ReloadMode : uint8_t {
  kNone = 0,
  kSigHup = 1,
  kComponent = 2,
};

enum class StartupMode : uint8_t {
  kParallel = 0,
  kSerial = 1,
  kMaxParallel = 2,
};

enum class GroupExecutionMode : uint8_t {
  kParallel = 0,
  kSerial = 1,
};

enum class SchedulerMode : uint8_t {
  kDedicated = 0,
  kShared = 1,
  kMulti = 2,
  kChoreography = 3,
  kThreadPool = 4,
};

enum class SchedPolicy : uint8_t {
  kDefault = 0,
  kOther = 1,
  kFifo = 2,
  kRr = 3,
};

enum class TaskKind : uint8_t {
  kOneshot = 0,
  kPeriodic = 1,
};

struct ArgSpec final {
  std::string name;
  std::string default_value;
  std::string resolved_value;
  std::string description;
  std::vector<std::string> choices;
};

struct ResourceLimits final {
  std::string slice;
  std::optional<uint32_t> cpu_quota_percent;
  std::optional<uint32_t> cpu_weight;
  std::optional<uint64_t> memory_max_mb;
  std::optional<uint64_t> memory_high_mb;
  std::optional<uint32_t> pids_max;
  std::optional<int32_t> oom_score_adj;
  std::optional<int32_t> nice;
  std::vector<uint32_t> cpu_affinity;
  std::optional<uint64_t> rlimit_nofile;
  std::optional<uint64_t> rlimit_as_mb;
  std::optional<uint64_t> rlimit_core_mb;
  bool strict{false};

  [[nodiscard]] bool has_cgroup_limits() const noexcept;

  [[nodiscard]] bool empty() const noexcept;
};

struct SliceConfig final {
  std::string name;
  ResourceLimits limits;
};

struct GroupConfig final {
  std::string name;
  std::string ns;
  GroupExecutionMode mode{GroupExecutionMode::kParallel};
  std::string slice;
  bool condition{true};
  bool autostart{true};
  std::vector<std::string> members;
  std::vector<std::string> after;
  std::vector<std::string> before;
  std::unordered_map<std::string, std::string> env;
  std::unordered_map<std::string, std::string> urls;
  std::vector<std::pair<std::string, std::string>> remaps;
  LauncherParamMap params;
};

struct ReadyProbeConfig final {
  ReadyProbeType type{ReadyProbeType::kNone};
  std::string pattern;
  std::string path;
  std::string topic;
  uint32_t delay_ms{0};
  uint32_t timeout_ms{10000};
};

struct LivenessProbeConfig final {
  bool enabled{false};
  ReadyProbeType type{ReadyProbeType::kNone};
  std::string path;
  std::string topic;
  uint32_t interval_ms{5000};
  uint32_t failure_threshold{3};
  bool restart_on_failure{true};
};

struct RespawnConfig final {
  bool enabled{false};
  RestartPolicy policy{RestartPolicy::kNo};
  uint32_t delay_ms{1000};
  uint32_t max_delay_ms{30000};
  double backoff{1.5};
  uint32_t max_restarts{5};
  uint32_t window_ms{60000};
  uint32_t start_limit_burst{5};
  uint32_t start_limit_interval_ms{60000};
  bool has_legacy_limits{false};
  bool has_start_limits{false};
  std::vector<int32_t> success_exit_codes;
  std::vector<int32_t> restart_prevent_exit_codes;
};

struct DependencySpec final {
  std::vector<std::string> depends_on;
  std::vector<std::string> after;
  std::vector<std::string> before;
  std::vector<std::string> requires_units;
  std::vector<std::string> wants;
  std::vector<std::string> binds_to;
  std::vector<std::string> part_of;
  std::vector<std::string> conflicts;
};

struct LoggerConfig final {
  std::string dir;
  Logger::Level console_level{Logger::kInfo};
  Logger::Level file_level{Logger::kDebug};
  size_t max_file_size{10U * 1024U * 1024U};
  size_t max_files{10};
  uint32_t flush_delay_ms{500};
  size_t history_max_records{4096};
  bool fixed_filename{false};
  bool append{false};
  bool utc{false};
  bool console_fmt{false};
  bool capture_to_file{true};
  bool strip_ansi{true};
  std::string plugin;
  bool plugin_strict{false};
  bool plugin_capture{false};
};

struct RecordConfig final {
  bool enabled{false};
  std::string dir;
  std::string format{"vcap"};
  std::string tag;
  bool force_publish{false};
};

struct ProxyConfig final {
  bool enabled{false};
  std::string name{"vlink_proxy"};
  std::string program{"vlink-proxy"};
  std::string dds_impl;
  uint32_t domain_id{0};
  bool use_iox{false};
  int32_t iox_strategy{0};
  std::string iox_config;
  std::string config_file;
  std::vector<std::string> extra_args;
  int32_t stage{-100};
  std::string task_group;
  DependencySpec deps;
  RespawnConfig respawn;
  ResourceLimits resources;
  KillMode kill_mode{KillMode::kProcess};
  StopSignal stop_signal{StopSignal::kSigTerm};
  ReloadMode reload_mode{ReloadMode::kNone};
  bool required{false};
  bool condition{true};
  bool autostart{true};
  OutputMode output{OutputMode::kLog};
  uint32_t ready_delay_ms{300};
  uint32_t sigterm_timeout_ms{5000};
  std::unordered_map<std::string, std::string> env;
};

struct SchedulerConfig final {
  std::string name;
  SchedulerMode mode{SchedulerMode::kShared};
  MessageLoop::Type queue_type{MessageLoop::kNormalType};
  MessageLoop::Strategy strategy{MessageLoop::kOptimizationStrategy};
  size_t threads{1};
  std::vector<uint32_t> cpu_affinity;
  SchedPolicy sched_policy{SchedPolicy::kDefault};
  int32_t thread_priority{0};
  uint32_t max_elapsed_ms{0};
};

struct ComponentSpec final {
  std::string name;
  std::string ns;
  std::string task_group;
  std::string library;
  std::string class_name;
  std::string scheduler_group;
  std::string compute_pool;
  std::string container;
  int32_t stage{0};
  int32_t worker_index{-1};
  uint16_t task_priority{0};
  uint32_t watchdog_ms{0};
  ReloadMode reload_mode{ReloadMode::kNone};
  bool legacy_runnable{false};
  bool condition{true};
  bool autostart{true};
  bool start_inactive{false};
  bool required{false};
  uint32_t delay_ms{0};
  DependencySpec deps;
  RespawnConfig respawn;
  LauncherParamMap params;
  std::unordered_map<std::string, std::string> urls;
  std::unordered_map<std::string, std::string> inputs;
  std::unordered_map<std::string, std::string> outputs;
  std::vector<std::pair<std::string, std::string>> remaps;
  ReadyProbeConfig ready;
  LivenessProbeConfig liveness;
  std::vector<std::string> on_start_trigger;
  std::vector<std::string> on_ready_trigger;
  std::vector<std::string> on_exit_trigger;
  std::vector<std::string> on_error_trigger;
  std::optional<bool> record_enabled;
  std::string record_path;
  std::optional<bool> record_force_publish;
};

struct ContainerSpec final {
  std::string name;
  std::string ns;
  std::string task_group;
  int32_t stage{0};
  bool condition{true};
  bool autostart{true};
  bool required{false};
  uint32_t delay_ms{0};
  uint32_t sigterm_timeout_ms{5000};
  OutputMode output{OutputMode::kScreen};
  KillMode kill_mode{KillMode::kControlGroup};
  StopSignal stop_signal{StopSignal::kSigTerm};
  RespawnConfig respawn;
  ResourceLimits resources;
  OnExitAction on_exit{OnExitAction::kIgnore};
  DependencySpec deps;
  std::unordered_map<std::string, std::string> env;
  bool inherit_env{true};
  std::string cwd;
  std::string launch_prefix;
  std::vector<std::string> on_start_trigger;
  std::vector<std::string> on_ready_trigger;
  std::vector<std::string> on_exit_trigger;
  std::vector<std::string> on_error_trigger;
  std::optional<bool> record_enabled;
  std::string record_path;
};

struct ProcessSpec final {
  std::string name;
  std::string ns;
  std::string task_group;
  int32_t stage{0};
  std::string program;
  std::vector<std::string> args;
  std::string launch_prefix;
  std::string cwd;
  std::unordered_map<std::string, std::string> env;
  bool inherit_env{true};
  bool condition{true};
  bool autostart{true};
  bool required{false};
  uint32_t delay_ms{0};
  uint32_t sigterm_timeout_ms{5000};
  OutputMode output{OutputMode::kScreen};
  KillMode kill_mode{KillMode::kProcess};
  StopSignal stop_signal{StopSignal::kSigTerm};
  ReloadMode reload_mode{ReloadMode::kNone};
  RespawnConfig respawn;
  ResourceLimits resources;
  OnExitAction on_exit{OnExitAction::kIgnore};
  DependencySpec deps;
  std::unordered_map<std::string, std::string> urls;
  std::unordered_map<std::string, std::string> inputs;
  std::unordered_map<std::string, std::string> outputs;
  std::vector<std::pair<std::string, std::string>> remaps;
  ReadyProbeConfig ready;
  LivenessProbeConfig liveness;
  std::vector<std::string> on_start_trigger;
  std::vector<std::string> on_ready_trigger;
  std::vector<std::string> on_exit_trigger;
  std::vector<std::string> on_error_trigger;
  std::optional<bool> record_enabled;
  std::string record_path;
};

struct TaskSpec final {
  std::string name;
  std::string ns;
  std::string task_group;
  int32_t stage{0};
  TaskKind kind{TaskKind::kOneshot};
  std::string program;
  std::vector<std::string> args;
  std::string launch_prefix;
  std::string cwd;
  std::unordered_map<std::string, std::string> env;
  bool inherit_env{true};
  bool condition{true};
  bool autostart{true};
  bool required{false};
  uint32_t delay_ms{0};
  uint32_t interval_ms{5000};
  uint32_t timeout_ms{10000};
  OutputMode output{OutputMode::kScreen};
  KillMode kill_mode{KillMode::kProcess};
  ResourceLimits resources;
  std::vector<int32_t> success_exit_codes;
  DependencySpec deps;
  std::unordered_map<std::string, std::string> urls;
  std::unordered_map<std::string, std::string> inputs;
  std::unordered_map<std::string, std::string> outputs;
  std::vector<std::pair<std::string, std::string>> remaps;
  std::vector<std::string> on_start_trigger;
  std::vector<std::string> on_ready_trigger;
  std::vector<std::string> on_exit_trigger;
  std::vector<std::string> on_error_trigger;
};

struct DataflowEdge final {
  std::string from;
  std::string to;
  std::string output_key;
  std::string input_key;
  std::string url;
};

struct LaunchManifest final {
  std::string name{"vlink-launcher"};
  std::string source_file;
  std::string ns;
  StartupMode startup_mode{StartupMode::kParallel};
  uint32_t max_parallel{0};
  std::string default_target;
  uint32_t shutdown_timeout_ms{5000};
  std::string control_scheme;
  std::string control_namespace{"launcher"};
  uint32_t control_domain{0};
  bool control_enabled{true};
  std::string security_key;
  std::string bind_ip;
  bool native_mode{false};
  bool allow_remote_load{false};
  std::vector<std::string> plugin_dirs;

  LoggerConfig logger;
  RecordConfig record;
  ProxyConfig proxy;
  std::unordered_map<std::string, std::string> args;
  std::vector<ArgSpec> arg_specs;
  std::unordered_map<std::string, std::string> global_env;
  std::unordered_map<std::string, std::string> global_urls;
  std::vector<std::pair<std::string, std::string>> global_remaps;
  LauncherParamMap global_params;

  std::vector<SliceConfig> slices;
  std::vector<GroupConfig> groups;
  std::vector<SchedulerConfig> schedulers;
  std::vector<ComponentSpec> components;
  std::vector<ContainerSpec> containers;
  std::vector<ProcessSpec> processes;
  std::vector<TaskSpec> tasks;
};

[[nodiscard]] Logger::Level parse_log_level_strict(std::string_view raw, bool& ok) noexcept;

[[nodiscard]] std::vector<std::string> split_command_prefix(std::string_view prefix);

[[nodiscard]] std::string resolve_unit_url(std::string_view url, std::string_view ns,
                                           const std::vector<std::pair<std::string, std::string>>& remaps);

void inject_url_env(const std::unordered_map<std::string, std::string>& urls,
                    const std::unordered_map<std::string, std::string>& inputs,
                    const std::unordered_map<std::string, std::string>& outputs, std::string_view ns,
                    const std::vector<std::pair<std::string, std::string>>& remaps,
                    std::unordered_map<std::string, std::string>& env);

[[nodiscard]] std::vector<DataflowEdge> compute_dataflow_edges(const LaunchManifest& manifest);

[[nodiscard]] bool parse_manifest_file(const std::string& file_path,
                                       const std::unordered_map<std::string, std::string>& arg_overrides,
                                       LaunchManifest& out, std::string* error_out);

[[nodiscard]] bool parse_manifest_string(std::string_view toml_content, const std::string& base_dir,
                                         const std::unordered_map<std::string, std::string>& arg_overrides,
                                         LaunchManifest& out, std::string* error_out);

[[nodiscard]] LauncherServer::ValidationReport validate_manifest(const LaunchManifest& manifest,
                                                                 bool check_programs = false);

[[nodiscard]] std::string format_manifest_json(const LaunchManifest& manifest);

[[nodiscard]] std::string format_proxy_config_toml(const ProxyConfig& spec);

[[nodiscard]] std::string format_process_spec_toml(const ProcessSpec& ps);

[[nodiscard]] std::string format_component_spec_toml(const ComponentSpec& cs);

[[nodiscard]] std::string format_container_spec_toml(const ContainerSpec& cs);

[[nodiscard]] std::string format_task_spec_toml(const TaskSpec& ts);

[[nodiscard]] std::string format_unit_config_toml(const LaunchManifest& manifest, const std::string& unit_name);

[[nodiscard]] std::string serialize_container_manifest(const LaunchManifest& manifest, const ContainerSpec& container,
                                                       const std::vector<ComponentSpec>& container_components,
                                                       const std::string& control_scheme,
                                                       const std::string& control_namespace, uint32_t control_domain,
                                                       const std::string& bind_ip, bool native_mode,
                                                       bool control_enabled, bool allow_remote_load = false);

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline bool ResourceLimits::has_cgroup_limits() const noexcept {
  return cpu_quota_percent.has_value() || cpu_weight.has_value() || memory_max_mb.has_value() ||
         memory_high_mb.has_value() || pids_max.has_value() || !slice.empty();
}

inline bool ResourceLimits::empty() const noexcept {
  return !has_cgroup_limits() && !oom_score_adj.has_value() && !nice.has_value() && cpu_affinity.empty() &&
         !rlimit_nofile.has_value() && !rlimit_as_mb.has_value() && !rlimit_core_mb.has_value();
}

}  // namespace launcher

}  // namespace vlink
