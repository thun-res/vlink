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

#include <vlink/external/launcher_server.h>
//
#include <vlink/base/condition_variable.h>
#include <vlink/base/deadline_timer.h>
#include <vlink/base/elapsed_timer.h>
#include <vlink/base/graph_task.h>
#include <vlink/base/helpers.h>
#include <vlink/base/logger.h>
#include <vlink/base/logger_plugin_interface.h>
#include <vlink/base/memory_resource.h>
#include <vlink/base/multi_loop.h>
#include <vlink/base/plugin.h>
#include <vlink/base/thread_pool.h>
#include <vlink/base/timer.h>
#include <vlink/base/utils.h>
#include <vlink/base/uuid.h>
#include <vlink/base/wheel_timer.h>
#include <vlink/extension/discovery_viewer.h>
#include <vlink/extension/runnable_plugin_interface.h>
#include <vlink/extension/security.h>
#include <vlink/external/launcher_api.h>
#include <vlink/impl/url_parser.h>
#include <vlink/publisher.h>
#include <vlink/server.h>
#include <vlink/setter.h>
#include <vlink/subscriber.h>
#include <vlink/version.h>
//
#include <nlohmann/json.hpp>
//
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../launcher_common.h"
#include "./component_host.h"
#include "./launcher_config.h"
#include "./log_manager.h"
#include "./process_supervisor.h"
#include "./resource_control.h"
#include "./scheduler_manager.h"

namespace vlink {

static std::string make_session_id() {
  const std::string hex = Uuid::random_hex();
  const std::string short_id = (hex.size() >= 8U) ? hex.substr(0, 8U) : hex;
  return "s_" + Utils::get_pid_str() + "_" + short_id;
}

static bool is_active_state(launcher::UnitState s) noexcept {
  return s == launcher::UnitState::kStarting || s == launcher::UnitState::kRunning || s == launcher::UnitState::kReady;
}

static std::string find_launcher_executable(const std::string& configured) {
  std::error_code ec;

  if (!configured.empty() && std::filesystem::exists(configured, ec)) {
    return configured;
  }

  const std::string app_name = Utils::get_app_name();

  if (app_name == "vlink-launcher" || app_name == "vlink-launcher.exe" || app_name == "launcher" ||
      app_name == "launcher.exe") {
    const std::string app_path = Utils::get_app_path();

    if (!app_path.empty()) {
      return app_path;
    }
  }

  const std::string app_dir = Utils::get_app_dir();
#if defined(_WIN32) || defined(_WIN64)
  const std::string bin_name = "vlink-launcher.exe";
  const char path_sep = ';';
#else
  const std::string bin_name = "vlink-launcher";
  const char path_sep = ':';
#endif

  if (!app_dir.empty()) {
    const auto sibling = std::filesystem::path(app_dir) / bin_name;

    if (std::filesystem::exists(sibling, ec)) {
      return Helpers::path_to_string(sibling);
    }
  }

  const std::string path_env = Utils::get_env("PATH");

  for (const auto& dir : Helpers::split_view(path_env, path_sep)) {
    const auto candidate = std::filesystem::path(dir) / bin_name;

    if (std::filesystem::exists(candidate, ec)) {
      return Helpers::path_to_string(candidate);
    }
  }

  return "";
}

static bool contains_str(const std::vector<std::string>& list, std::string_view value) noexcept {
  for (const auto& item : list) {
    if (item == value) {
      return true;
    }
  }

  return false;
}

static void push_unique(std::vector<std::string>& list, const std::string& value) {
  if (!value.empty() && !contains_str(list, value)) {
    list.push_back(value);
  }
}

static bool url_matches_topic(std::string_view discovered_url, std::string_view expected) noexcept {
  if VUNLIKELY (expected.empty() || discovered_url.empty()) {
    return false;
  }

  if (discovered_url == expected) {
    return true;
  }

  try {
    const UrlParser disc{std::string(discovered_url)};
    const UrlParser exp{std::string(expected)};

    if (disc.get_transport() != exp.get_transport() || disc.get_host() != exp.get_host() ||
        disc.get_port() != exp.get_port() || disc.get_path() != exp.get_path()) {
      return false;
    }

    const auto& dq = disc.get_query_dictionary();
    const auto& eq = exp.get_query_dictionary();
    const bool has_domain = launcher::is_dds_scheme_name(exp.get_transport()) || exp.get_transport() == "zenoh" ||
                            exp.get_transport() == "shm" || exp.get_transport() == "shm2" ||
                            exp.get_transport() == "mqtt";

    if (has_domain) {
      const auto dit = dq.find("domain");
      const auto eit = eq.find("domain");
      const std::string_view ddom = (dit != dq.end() && !dit->second.empty()) ? std::string_view(dit->second) : "0";
      const std::string_view edom = (eit != eq.end() && !eit->second.empty()) ? std::string_view(eit->second) : "0";

      if (ddom != edom) {
        return false;
      }
    }

    const auto dit = dq.find("event");
    const auto eit = eq.find("event");
    const std::string_view dev = (dit != dq.end()) ? std::string_view(dit->second) : std::string_view();
    const std::string_view eev = (eit != eq.end()) ? std::string_view(eit->second) : std::string_view();

    if (dev != eev) {
      return false;
    }

    return true;
  } catch (const std::exception&) {
    const size_t dq = discovered_url.find('?');
    const size_t eq = expected.find('?');
    const std::string_view dbase = (dq == std::string_view::npos) ? discovered_url : discovered_url.substr(0U, dq);
    const std::string_view ebase = (eq == std::string_view::npos) ? expected : expected.substr(0U, eq);
    return dbase == ebase;
  }
}

template <typename NodeT, typename... ArgsT>
static std::shared_ptr<NodeT> make_server_node(std::string_view scheme, std::string_view bind_ip, bool native_mode,
                                               ArgsT&&... args) {
  auto node = MemoryResource::make_shared<NodeT>(std::forward<ArgsT>(args)..., InitType::kWithoutInit);
  node->set_discovery_enabled(false);
  node->set_safety_quit(true);
  launcher::apply_control_node_properties(*node, scheme, bind_ip, native_mode);
  node->init();
  return node;
}

struct UnitRuntime final {
  launcher::UnitStatus status;
  bool condition{true};
  bool autostart{true};
  bool start_inactive{false};
  bool selected_for_boot{true};
  uint32_t delay_ms{0};
  uint32_t sigterm_timeout_ms{5000};
  launcher::RespawnConfig respawn;
  launcher::OnExitAction on_exit{launcher::OnExitAction::kIgnore};
  launcher::ReadyProbeConfig ready;
  launcher::LivenessProbeConfig liveness;
  launcher::ResourceLimits resources;
  launcher::KillMode kill_mode{launcher::KillMode::kProcess};
  launcher::StopSignal stop_signal{launcher::StopSignal::kSigTerm};
  launcher::ReloadMode reload_mode{launcher::ReloadMode::kNone};
  uint32_t watchdog_ms{0};

  std::vector<std::string> order_preds;
  std::vector<std::string> order_succs;
  std::vector<std::string> req_preds;
  std::vector<std::string> want_preds;
  std::vector<std::string> binds_preds;
  std::vector<std::string> part_of_preds;
  std::vector<std::string> conflict_units;

  std::vector<std::string> on_start_trigger;
  std::vector<std::string> on_ready_trigger;
  std::vector<std::string> on_exit_trigger;
  std::vector<std::string> on_error_trigger;

  launcher::ComponentSpec comp_spec;
  launcher::ContainerSpec container_spec;
  launcher::ProcessSpec proc_spec;
  launcher::TaskSpec task_spec;
  launcher::ProxyConfig proxy_spec;

  std::string container_ctrl_ns;
  std::shared_ptr<LauncherAPI> container_client;
  std::string active_record_path;

  std::atomic<uint64_t> generation{0};
  std::atomic_bool holds_start_slot{false};
  bool cold_boot_recorded{false};
  bool boot_participant{false};
  bool explicit_boot_dep{false};
  bool boot_failed{false};
  bool boot_enqueued{false};
  bool boot_terminal{false};
  bool oneshot_completed_ok{false};
  size_t remaining_boot_preds{0};
  std::string last_unblocker;

  uint64_t start_limit_window_start_ms{0};
  uint32_t start_limit_attempts{0};
  uint32_t backoff_streak{0};
  uint64_t ready_since_steady_ms{0};
  std::deque<uint64_t> legacy_restart_window;

  uint64_t last_liveness_check_ms{0};
  uint64_t watchdog_armed_ms{0};

  int64_t delay_timer_key{-1};
  int64_t ready_timer_key{-1};
  int64_t probe_poll_timer_key{-1};
  int64_t respawn_timer_key{-1};
  int64_t periodic_timer_key{-1};
};

struct LauncherServer::Impl final {
  using PlainServer = Server<launcher::CommandRequest, launcher::CommandResponse>;
  using PlainSetter = Setter<launcher::SystemSnapshot>;
  using PlainEventPub = Publisher<launcher::EventRecord>;
  using PlainLogPub = Publisher<launcher::LogRecord>;
  using PlainLogdSub = Subscriber<launcher::LogRecord>;
  using PlainHbPub = Publisher<launcher::HeartbeatMsg>;

#ifdef VLINK_ENABLE_SECURITY
  using SecServer = SecurityServer<launcher::CommandRequest, launcher::CommandResponse>;
  using SecSetter = SecuritySetter<launcher::SystemSnapshot>;
  using SecEventPub = SecurityPublisher<launcher::EventRecord>;
  using SecLogPub = SecurityPublisher<launcher::LogRecord>;
  using SecLogdSub = SecuritySubscriber<launcher::LogRecord>;
  using SecHbPub = SecurityPublisher<launcher::HeartbeatMsg>;
#endif

  Impl(LauncherServer* owner_ptr, const Config& cfg)
      : owner(owner_ptr),
        wheel_timer(std::make_unique<WheelTimer>(128U, 20U)),
        alive_token(MemoryResource::make_shared<std::atomic_bool>(true)),
        config(cfg),
        component_host(scheduler),
        process_supervisor(log_manager, resource_ctrl) {}

  LauncherServer* owner{nullptr};
  uint64_t server_start_steady_ms{0};
  std::atomic<uint64_t> boot_duration_ms{0};
  std::unique_ptr<WheelTimer> wheel_timer;
  std::unique_ptr<Timer> housekeep_timer;
  std::unique_ptr<MultiLoop> ctrl_rpc_loop;
  size_t in_flight_starts{0};
  size_t pending_boot_units{0};
  size_t active_stage_idx{0};

  std::shared_ptr<std::atomic_bool> alive_token;
  std::shared_ptr<DiscoveryViewer> discovery_viewer;
  std::shared_ptr<EventCallback> local_event_cb;
  std::shared_ptr<LogCallback> local_log_cb;
  std::shared_ptr<PlainServer> ctrl_server;
  std::shared_ptr<PlainSetter> state_setter;
  std::shared_ptr<PlainEventPub> event_pub;
  std::shared_ptr<PlainLogPub> log_pub;
  std::shared_ptr<PlainLogdSub> logd_sub;
  std::shared_ptr<PlainHbPub> hb_pub;

#ifdef VLINK_ENABLE_SECURITY
  std::shared_ptr<SecServer> sec_ctrl_server;
  std::shared_ptr<SecSetter> sec_state_setter;
  std::shared_ptr<SecEventPub> sec_event_pub;
  std::shared_ptr<SecLogPub> sec_log_pub;
  std::shared_ptr<SecLogdSub> sec_logd_sub;
  std::shared_ptr<SecHbPub> sec_hb_pub;
#endif

  std::vector<std::string> unit_order;
  std::vector<int32_t> sorted_stages;
  std::string session_id;
  std::string manifest_source_file;
  std::string active_ctrl_scheme;
  std::string active_ctrl_ns;
  std::string active_security_key;
  std::string active_bind_ip;
  std::string active_record_dir;
  std::string active_record_format{"vcap"};
  std::string active_record_tag;

  mutable std::mutex viewer_mtx;
  mutable std::mutex start_mtx;
  std::mutex stop_mtx;
  mutable std::mutex state_mtx;
  std::mutex cb_mtx;
  mutable std::mutex ctrl_mtx;
  std::mutex timer_mtx;
  std::recursive_mutex op_mtx;
  ConditionVariable stop_cv;
  ConditionVariable state_cv;

  std::unordered_map<std::string, std::shared_ptr<UnitRuntime>> units;
  std::unordered_map<int32_t, size_t> stage_remaining;
  std::unordered_map<int32_t, std::vector<std::string>> stage_units;
  std::unordered_map<std::string, std::vector<std::string>> group_members;
  std::unordered_map<std::string, std::vector<launcher::ComponentSpec>> container_components;
  std::unordered_map<std::string, std::string> component_to_container;
  std::unordered_map<std::string, std::unordered_set<std::string>> dynamic_container_components;
  std::unordered_map<std::string, std::string> remote_record_paths;
  LauncherParamMap global_params;
  std::deque<std::string> startup_queue;

  Config config;
  launcher::LaunchManifest manifest;
  launcher::LoggerConfig effective_logger_cfg;
  launcher::LogManager log_manager;
  launcher::ResourceController resource_ctrl;
  launcher::SchedulerManager scheduler;
  launcher::ComponentHost component_host;
  launcher::ProcessSupervisor process_supervisor;

  uint32_t active_ctrl_domain{0};
  uint32_t effective_max_parallel{0};
  std::atomic<int32_t> exit_code{0};
  std::atomic<uint64_t> record_segment_seq{0};
  mutable std::atomic<double> cached_host_cpu{-1.0};
  mutable std::atomic<double> cached_host_mem{-1.0};
  std::atomic_bool snapshot_dirty{false};
  std::atomic_bool snapshot_flush_pending{false};
  std::atomic_bool stop_executed{false};
  std::mutex fallback_stop_mtx;
  std::thread fallback_stop_thread;

  launcher::StartupMode effective_startup_mode{launcher::StartupMode::kParallel};
  bool manifest_loaded{false};
  bool active_native_mode{false};
  std::atomic_bool running{false};
  std::atomic_bool stopping{false};
  std::atomic_bool paused_on_fail{false};
  bool cold_boot_complete{false};
  std::atomic_bool control_active{false};
  bool use_security{false};
  bool active_record_enabled{false};
  bool active_record_force_publish{false};

  void rebuild_unit_table();

  std::vector<std::string> expand_target_list(const std::string& target) const;

  std::shared_ptr<UnitRuntime> find_unit(const std::string& name) const;

  void cancel_unit_timers(const std::shared_ptr<UnitRuntime>& unit);

  void release_start_slot(const std::shared_ptr<UnitRuntime>& unit);

  void transition_unit(const std::shared_ptr<UnitRuntime>& unit, launcher::UnitState next_state,
                       std::string_view event_name, std::string_view detail);

  bool transition_unit_if(const std::shared_ptr<UnitRuntime>& unit, launcher::UnitState expected,
                          launcher::UnitState next_state, std::string_view event_name, std::string_view detail);

  bool check_and_record_start_limit(const std::shared_ptr<UnitRuntime>& unit, std::string* err_out);

  void mark_unit_boot_terminal(const std::shared_ptr<UnitRuntime>& unit, bool succeeded);

  void enqueue_ready_unit(const std::shared_ptr<UnitRuntime>& unit);

  void pump_startup_queue();

  void arm_readiness_probe(const std::shared_ptr<UnitRuntime>& unit, uint64_t gen);

  void finish_readiness(const std::shared_ptr<UnitRuntime>& unit, uint64_t gen, bool ok, std::string_view detail);

  bool launch_unit_now(const std::shared_ptr<UnitRuntime>& unit, std::string* error_out);

  [[nodiscard]] std::string normalize_record_path(const std::string& p_str) const;

  [[nodiscard]] bool check_cross_proc_record_collision(const std::string& target_domain, const std::string& resolved_p,
                                                       std::string& conflict_out) const;

  bool post_protected(MessageLoop::Callback&& cb) const;

  std::shared_ptr<DiscoveryViewer> ensure_discovery_viewer();

  std::vector<std::string> compute_topo_order_for_set(const std::unordered_set<std::string>& subset) const;

  void cascade_stop_bound_dependents(const std::string& unit_name);

  bool stop_unit_cascade(const std::string& name, std::unordered_set<std::string>& visited, bool propagate_part_of);

  bool stop_unit_internal(const std::shared_ptr<UnitRuntime>& unit,
                          std::optional<uint32_t> timeout_override_ms = std::nullopt);

  void handle_process_exit(const std::string& name, uint64_t gen, int32_t code, Process::ExitStatus status,
                           bool expected_stop);

  void schedule_respawn(const std::shared_ptr<UnitRuntime>& unit);

  void trigger_units(const std::vector<std::string>& names);

  void arm_periodic_run_timeout(const std::shared_ptr<UnitRuntime>& unit, uint64_t gen);

  void schedule_periodic_task(const std::shared_ptr<UnitRuntime>& unit, uint64_t gen);

  void run_housekeeping();

  void join_fallback_stop_thread();

  static uint32_t calc_ready_budget_ms(const std::shared_ptr<UnitRuntime>& unit) noexcept;

  bool wait_unit_ready(const std::shared_ptr<UnitRuntime>& unit, uint32_t wait_ms);

  bool has_matching_topic(const std::string& expected_topic);

  std::string build_proxy_config_file(const launcher::ProxyConfig& proxy) const;

  std::string build_container_manifest_file(const std::shared_ptr<UnitRuntime>& unit) const;

  std::string resolve_unit_record_path(const std::string& unit_name, launcher::UnitType type,
                                       const std::string& explicit_path, uint64_t generation,
                                       uint64_t segment = 0U) const;

  std::shared_ptr<LauncherAPI> get_or_connect_container_client(const std::shared_ptr<UnitRuntime>& container_unit,
                                                               uint32_t wait_ms);

  std::vector<std::vector<std::string>> compute_reverse_topological_layers() const;

  void begin_cold_boot();

  bool init_control_plane();

  void shutdown_control_plane();

  void shutdown_logd_collector();

  void publish_snapshot();

  void schedule_snapshot_publish();

  void publish_event(const launcher::EventRecord& ev);

  void publish_log(const launcher::LogRecord& rec);

  void publish_heartbeat();

  launcher::CommandResponse handle_command(const launcher::CommandRequest& req);

  launcher::SystemSnapshot build_snapshot() const;

  void request_async_stop(int32_t code = 0);

  void do_stop();
};

void LauncherServer::Impl::rebuild_unit_table() {
  std::lock_guard lock(state_mtx);
  manifest_source_file = manifest.source_file;
  unit_order.clear();
  units.clear();
  group_members.clear();
  container_components.clear();
  component_to_container.clear();
  global_params = manifest.global_params;
  startup_queue.clear();
  in_flight_starts = 0U;
  pending_boot_units = 0U;
  cold_boot_complete = false;

  for (const auto& g : manifest.groups) {
    group_members[g.name] = g.members;
  }

  auto make_base = [&](const std::string& name, const std::string& ns, const std::string& tgrp, int32_t stage,
                       launcher::UnitType type, bool cond, bool autostart, bool required, uint32_t delay_ms) {
    auto u = MemoryResource::make_shared<UnitRuntime>();
    u->status.name = name;
    u->status.ns = ns;
    u->status.task_group = tgrp;
    u->status.stage = stage;
    u->status.type = type;
    u->status.state = launcher::UnitState::kPending;
    u->status.required = required;
    u->condition = cond;
    u->autostart = autostart;
    u->delay_ms = delay_ms;
    unit_order.push_back(name);
    units.emplace(name, u);
    return u;
  };

  if (manifest.proxy.enabled) {
    auto u =
        make_base(manifest.proxy.name, "", manifest.proxy.task_group, manifest.proxy.stage, launcher::UnitType::kProxy,
                  manifest.proxy.condition, manifest.proxy.autostart, manifest.proxy.required, 0U);
    u->sigterm_timeout_ms = manifest.proxy.sigterm_timeout_ms;
    u->respawn = manifest.proxy.respawn;
    u->resources = manifest.proxy.resources;
    u->status.slice = manifest.proxy.resources.slice;
    u->kill_mode = manifest.proxy.kill_mode;
    u->stop_signal = manifest.proxy.stop_signal;
    u->reload_mode = manifest.proxy.reload_mode;
    u->proxy_spec = manifest.proxy;

    if (manifest.proxy.ready_delay_ms > 0U) {
      u->ready.type = launcher::ReadyProbeType::kDelay;
      u->ready.delay_ms = manifest.proxy.ready_delay_ms;
      u->ready.timeout_ms = manifest.proxy.ready_delay_ms + 2000U;
    }
  }

  for (const auto& cs : manifest.containers) {
    auto u = make_base(cs.name, cs.ns, cs.task_group, cs.stage, launcher::UnitType::kContainer, cs.condition,
                       cs.autostart, cs.required, cs.delay_ms);
    u->sigterm_timeout_ms = cs.sigterm_timeout_ms;
    u->respawn = cs.respawn;
    u->resources = cs.resources;
    u->status.slice = cs.resources.slice;
    u->kill_mode = cs.kill_mode;
    u->stop_signal = cs.stop_signal;
    u->on_exit = cs.on_exit;
    u->on_start_trigger = cs.on_start_trigger;
    u->on_ready_trigger = cs.on_ready_trigger;
    u->on_exit_trigger = cs.on_exit_trigger;
    u->on_error_trigger = cs.on_error_trigger;
    u->container_spec = cs;
  }

  for (const auto& cs : manifest.components) {
    if (!cs.container.empty() && !config.container_mode) {
      container_components[cs.container].push_back(cs);
      component_to_container[cs.name] = cs.container;
      continue;
    }

    auto u = make_base(cs.name, cs.ns, cs.task_group, cs.stage, launcher::UnitType::kComponent, cs.condition,
                       cs.autostart, cs.required, cs.delay_ms);
    u->status.group = cs.scheduler_group.empty() ? "default" : cs.scheduler_group;
    u->status.worker_index = cs.worker_index;
    u->status.pid = Utils::get_pid();
    u->start_inactive = cs.start_inactive;
    u->watchdog_ms = cs.watchdog_ms;
    u->reload_mode = cs.reload_mode;
    u->respawn = cs.respawn;
    u->ready = cs.ready;

    if (u->ready.type == launcher::ReadyProbeType::kTopic && !u->ready.topic.empty()) {
      u->ready.topic = launcher::resolve_unit_url(u->ready.topic, cs.ns, cs.remaps);
    }

    u->liveness = cs.liveness;

    if (u->liveness.enabled && u->liveness.type == launcher::ReadyProbeType::kTopic && !u->liveness.topic.empty()) {
      u->liveness.topic = launcher::resolve_unit_url(u->liveness.topic, cs.ns, cs.remaps);
    }

    u->on_start_trigger = cs.on_start_trigger;
    u->on_ready_trigger = cs.on_ready_trigger;
    u->on_exit_trigger = cs.on_exit_trigger;
    u->on_error_trigger = cs.on_error_trigger;
    u->comp_spec = cs;
  }

  for (const auto& ps : manifest.processes) {
    auto u = make_base(ps.name, ps.ns, ps.task_group, ps.stage, launcher::UnitType::kProcess, ps.condition,
                       ps.autostart, ps.required, ps.delay_ms);
    u->sigterm_timeout_ms = ps.sigterm_timeout_ms;
    u->respawn = ps.respawn;
    u->resources = ps.resources;
    u->status.slice = ps.resources.slice;
    u->kill_mode = ps.kill_mode;
    u->stop_signal = ps.stop_signal;
    u->reload_mode = ps.reload_mode;
    u->on_exit = ps.on_exit;
    u->ready = ps.ready;

    if (u->ready.type == launcher::ReadyProbeType::kTopic && !u->ready.topic.empty()) {
      u->ready.topic = launcher::resolve_unit_url(u->ready.topic, ps.ns, ps.remaps);
    }

    u->liveness = ps.liveness;

    if (u->liveness.enabled && u->liveness.type == launcher::ReadyProbeType::kTopic && !u->liveness.topic.empty()) {
      u->liveness.topic = launcher::resolve_unit_url(u->liveness.topic, ps.ns, ps.remaps);
    }

    u->on_start_trigger = ps.on_start_trigger;
    u->on_ready_trigger = ps.on_ready_trigger;
    u->on_exit_trigger = ps.on_exit_trigger;
    u->on_error_trigger = ps.on_error_trigger;
    u->proc_spec = ps;

    auto dbg_it = config.debug_prefixes.find(ps.name);

    if (dbg_it != config.debug_prefixes.end()) {
      u->proc_spec.launch_prefix = dbg_it->second;
    }
  }

  for (const auto& ts : manifest.tasks) {
    auto u = make_base(ts.name, ts.ns, ts.task_group, ts.stage, launcher::UnitType::kTask, ts.condition, ts.autostart,
                       ts.required, ts.delay_ms);
    u->sigterm_timeout_ms = ts.timeout_ms;
    u->resources = ts.resources;
    u->status.slice = ts.resources.slice;
    u->kill_mode = ts.kill_mode;
    u->on_start_trigger = ts.on_start_trigger;
    u->on_ready_trigger = ts.on_ready_trigger;
    u->on_exit_trigger = ts.on_exit_trigger;
    u->on_error_trigger = ts.on_error_trigger;
    u->task_spec = ts;
  }

  auto resolve_ref = [&](const std::string& ref) -> std::vector<std::string> {
    std::vector<std::string> res;

    if (ref.empty()) {
      return res;
    }

    if (ref[0] == '@') {
      auto git = group_members.find(ref.substr(1U));

      if (git != group_members.end()) {
        for (const auto& m : git->second) {
          if (units.count(m) != 0U) {
            res.push_back(m);
          } else {
            auto cit = component_to_container.find(m);

            if (cit != component_to_container.end() && units.count(cit->second) != 0U) {
              res.push_back(cit->second);
            }
          }
        }
      }

      return res;
    }

    if (units.count(ref) != 0U) {
      res.push_back(ref);
      return res;
    }

    auto cit = component_to_container.find(ref);

    if (cit != component_to_container.end() && units.count(cit->second) != 0U) {
      res.push_back(cit->second);
    }

    return res;
  };

  auto add_order = [&](const std::string& pred, const std::string& succ) {
    if (pred == succ || units.count(pred) == 0U || units.count(succ) == 0U) {
      return;
    }

    push_unique(units[succ]->order_preds, pred);
    push_unique(units[pred]->order_succs, succ);
    push_unique(units[succ]->status.dependencies, pred);
  };

  auto wire_deps = [&](const std::string& name, const launcher::DependencySpec& d) {
    auto it = units.find(name);

    if (it == units.end()) {
      return;
    }

    auto& u = *it->second;

    for (const auto& ref : d.depends_on) {
      for (const auto& t : resolve_ref(ref)) {
        if (t != name) {
          add_order(t, name);
          push_unique(u.req_preds, t);
        }
      }
    }

    for (const auto& ref : d.after) {
      for (const auto& t : resolve_ref(ref)) {
        if (t != name) {
          add_order(t, name);
        }
      }
    }

    for (const auto& ref : d.before) {
      for (const auto& t : resolve_ref(ref)) {
        if (t != name) {
          add_order(name, t);
        }
      }
    }

    for (const auto& ref : d.requires_units) {
      for (const auto& t : resolve_ref(ref)) {
        if (t != name) {
          push_unique(u.req_preds, t);
          push_unique(u.status.dependencies, t);
        }
      }
    }

    for (const auto& ref : d.wants) {
      for (const auto& t : resolve_ref(ref)) {
        if (t != name) {
          push_unique(u.want_preds, t);
          push_unique(u.status.wants, t);
        }
      }
    }

    for (const auto& ref : d.binds_to) {
      for (const auto& t : resolve_ref(ref)) {
        if (t != name) {
          push_unique(u.req_preds, t);
          push_unique(u.binds_preds, t);
          push_unique(u.status.binds_to, t);
        }
      }
    }

    for (const auto& ref : d.part_of) {
      for (const auto& t : resolve_ref(ref)) {
        if (t != name) {
          push_unique(u.part_of_preds, t);
        }
      }
    }

    for (const auto& ref : d.conflicts) {
      for (const auto& t : resolve_ref(ref)) {
        if (t != name) {
          push_unique(u.conflict_units, t);
          push_unique(u.status.conflicts, t);
          push_unique(units[t]->conflict_units, name);
          push_unique(units[t]->status.conflicts, name);
        }
      }
    }
  };

  if (manifest.proxy.enabled) {
    wire_deps(manifest.proxy.name, manifest.proxy.deps);
  }

  for (const auto& cs : manifest.containers) {
    wire_deps(cs.name, cs.deps);
  }

  for (const auto& cs : manifest.components) {
    if (cs.container.empty() || config.container_mode) {
      wire_deps(cs.name, cs.deps);
    } else {
      wire_deps(cs.container, cs.deps);
    }
  }

  for (const auto& ps : manifest.processes) {
    wire_deps(ps.name, ps.deps);
  }

  for (const auto& ts : manifest.tasks) {
    wire_deps(ts.name, ts.deps);
  }

  for (const auto& gc : manifest.groups) {
    std::vector<std::string> resolved_members;

    for (const auto& m : gc.members) {
      std::string host_unit = m;

      if (units.count(host_unit) == 0U) {
        auto cit = component_to_container.find(m);

        if (cit != component_to_container.end() && units.count(cit->second) != 0U) {
          host_unit = cit->second;
        } else {
          continue;
        }
      }

      if (resolved_members.empty() || resolved_members.back() != host_unit) {
        push_unique(resolved_members, host_unit);
      }
    }

    if (gc.mode == launcher::GroupExecutionMode::kSerial && resolved_members.size() >= 2U) {
      for (size_t i = 1U; i < resolved_members.size(); ++i) {
        add_order(resolved_members[i - 1U], resolved_members[i]);
      }
    }

    for (const auto& aref : gc.after) {
      for (const auto& p : resolve_ref(aref)) {
        for (const auto& m : resolved_members) {
          add_order(p, m);
        }
      }
    }

    for (const auto& bref : gc.before) {
      for (const auto& s : resolve_ref(bref)) {
        for (const auto& m : resolved_members) {
          add_order(m, s);
        }
      }
    }
  }

  std::set<int32_t> stages;

  for (const auto& name : unit_order) {
    stages.insert(units[name]->status.stage);
  }

  sorted_stages.assign(stages.begin(), stages.end());
  stage_remaining.clear();
  active_stage_idx = 0U;

  std::unordered_set<std::string> selected;

  if (!config.only_units.empty() || !manifest.default_target.empty()) {
    std::queue<std::string> bfs;
    auto seed = [&](const std::string& tok) {
      for (const auto& u : resolve_ref(tok)) {
        if (selected.insert(u).second) {
          bfs.push(u);
        }
      }
    };

    if (!config.only_units.empty()) {
      for (const auto& tok : config.only_units) {
        seed(tok);
      }
    } else {
      seed(manifest.default_target);
    }

    while (!bfs.empty()) {
      const std::string cur = bfs.front();
      bfs.pop();
      auto it = units.find(cur);

      if (it == units.end()) {
        continue;
      }

      for (const auto& dep : it->second->req_preds) {
        if (selected.insert(dep).second) {
          bfs.push(dep);
        }
      }

      for (const auto& dep : it->second->want_preds) {
        if (selected.insert(dep).second) {
          bfs.push(dep);
        }
      }
    }

    for (const auto& name : unit_order) {
      const bool in_sel = (selected.count(name) != 0U);
      units[name]->selected_for_boot = in_sel;
      units[name]->explicit_boot_dep = in_sel;
    }
  } else {
    std::unordered_set<std::string> reached;
    std::queue<std::string> bfs;

    for (const auto& name : unit_order) {
      auto& u = units[name];

      if (u->condition && u->autostart && u->selected_for_boot && reached.insert(name).second) {
        bfs.push(name);
      }
    }

    while (!bfs.empty()) {
      const std::string cur = bfs.front();
      bfs.pop();
      auto it = units.find(cur);

      if (it == units.end()) {
        continue;
      }

      for (const auto& dep : it->second->req_preds) {
        if (reached.insert(dep).second) {
          bfs.push(dep);
        }
      }

      for (const auto& dep : it->second->want_preds) {
        if (reached.insert(dep).second) {
          bfs.push(dep);
        }
      }
    }

    for (const auto& name : reached) {
      auto it = units.find(name);

      if (it != units.end() && !it->second->autostart) {
        it->second->explicit_boot_dep = true;
      }
    }
  }

  if (!config.skip_units.empty()) {
    std::unordered_set<std::string> skipped;

    for (const auto& tok : config.skip_units) {
      for (const auto& u : resolve_ref(tok)) {
        skipped.insert(u);
      }
    }

    for (const auto& name : skipped) {
      auto it = units.find(name);

      if (it != units.end()) {
        it->second->selected_for_boot = false;
        it->second->explicit_boot_dep = false;
      }
    }
  }
}

bool LauncherServer::Impl::post_protected(MessageLoop::Callback&& cb) const {
  PostTaskOptions opts;
  opts.drop_policy = TaskDropPolicy::kProtected;
  const auto handle = owner->post_task_handle(std::move(cb), opts);
  const auto st = handle.state();
  return st != TaskExecutionState::kRejected && st != TaskExecutionState::kCancelled &&
         st != TaskExecutionState::kInvalid;
}

std::shared_ptr<DiscoveryViewer> LauncherServer::Impl::ensure_discovery_viewer() {
  std::lock_guard lock(viewer_mtx);

  if (!discovery_viewer && !stopping.load(std::memory_order_acquire)) {
    const auto filter = active_native_mode ? DiscoveryViewer::kFilterNative : DiscoveryViewer::kFilterAvailable;
    discovery_viewer = MemoryResource::make_shared<DiscoveryViewer>(filter);
    discovery_viewer->async_run();
  }

  return discovery_viewer;
}

std::vector<std::string> LauncherServer::Impl::compute_topo_order_for_set(
    const std::unordered_set<std::string>& subset) const {
  std::lock_guard lock(state_mtx);
  std::unordered_map<std::string, size_t> in_degree;
  std::unordered_map<std::string, std::vector<std::string>> adj;
  std::vector<std::string> ordered;
  ordered.reserve(subset.size());

  for (const auto& name : unit_order) {
    if (subset.count(name) != 0U) {
      in_degree[name] = 0U;
    }
  }

  for (const auto& name : unit_order) {
    if (in_degree.count(name) == 0U) {
      continue;
    }

    const auto& u = units.at(name);

    for (const auto& pred : u->order_preds) {
      if (in_degree.count(pred) != 0U) {
        ++in_degree[name];
        adj[pred].push_back(name);
      }
    }
  }

  std::deque<std::string> ready;

  for (const auto& name : unit_order) {
    auto it = in_degree.find(name);

    if (it != in_degree.end() && it->second == 0U) {
      ready.push_back(name);
    }
  }

  std::unordered_set<std::string> emitted;

  while (!ready.empty()) {
    const std::string cur = std::move(ready.front());
    ready.pop_front();
    emitted.insert(cur);
    ordered.push_back(cur);

    auto ait = adj.find(cur);

    if (ait != adj.end()) {
      for (const auto& nxt : ait->second) {
        auto dit = in_degree.find(nxt);

        if (dit != in_degree.end() && dit->second > 0U) {
          --dit->second;

          if (dit->second == 0U) {
            ready.push_back(nxt);
          }
        }
      }
    }
  }

  for (const auto& name : unit_order) {
    if (in_degree.count(name) != 0U && emitted.count(name) == 0U) {
      ordered.push_back(name);
    }
  }

  return ordered;
}

std::vector<std::string> LauncherServer::Impl::expand_target_list(const std::string& target) const {
  std::lock_guard lock(state_mtx);
  std::vector<std::string> res;

  if (target.empty()) {
    return res;
  }

  if (target[0] == '@') {
    auto it = group_members.find(target.substr(1U));

    if (it != group_members.end()) {
      res = it->second;
    }

    return res;
  }

  res.push_back(target);
  return res;
}

std::shared_ptr<UnitRuntime> LauncherServer::Impl::find_unit(const std::string& name) const {
  std::lock_guard lock(state_mtx);
  auto it = units.find(name);
  return (it == units.end()) ? nullptr : it->second;
}

void LauncherServer::Impl::cancel_unit_timers(const std::shared_ptr<UnitRuntime>& unit) {
  if (!unit || !wheel_timer) {
    return;
  }

  int64_t k_delay = -1;
  int64_t k_ready = -1;
  int64_t k_poll = -1;
  int64_t k_respawn = -1;
  int64_t k_periodic = -1;

  {
    std::lock_guard lock(state_mtx);
    k_delay = unit->delay_timer_key;
    k_ready = unit->ready_timer_key;
    k_poll = unit->probe_poll_timer_key;
    k_respawn = unit->respawn_timer_key;
    k_periodic = unit->periodic_timer_key;
    unit->delay_timer_key = -1;
    unit->ready_timer_key = -1;
    unit->probe_poll_timer_key = -1;
    unit->respawn_timer_key = -1;
    unit->periodic_timer_key = -1;
  }

  for (const int64_t k : {k_delay, k_ready, k_poll, k_respawn, k_periodic}) {
    if (k >= 0) {
      (void)wheel_timer->remove(k);
    }
  }
}

void LauncherServer::Impl::release_start_slot(const std::shared_ptr<UnitRuntime>& unit) {
  if (!unit || !unit->holds_start_slot.exchange(false, std::memory_order_acq_rel)) {
    return;
  }

  {
    std::lock_guard lock(state_mtx);

    if (in_flight_starts > 0U) {
      --in_flight_starts;
    }
  }

  pump_startup_queue();
}

void LauncherServer::Impl::transition_unit(const std::shared_ptr<UnitRuntime>& unit, launcher::UnitState next_state,
                                           std::string_view event_name, std::string_view detail) {
  launcher::EventRecord ev;
  ev.timestamp_ms = ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMilli);

  {
    std::lock_guard lock(state_mtx);
    ev.unit_name = unit->status.name;
    ev.unit_type = unit->status.type;
    ev.old_state = unit->status.state;
    ev.new_state = next_state;
    ev.event_name = std::string(event_name);
    ev.message = std::string(detail);

    unit->status.state = next_state;
    unit->status.detail = std::string(detail);

    const uint64_t now_steady = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);
    const uint64_t offset = (now_steady >= server_start_steady_ms) ? (now_steady - server_start_steady_ms) : 0U;

    if (next_state == launcher::UnitState::kStarting) {
      unit->status.start_time_ms = ev.timestamp_ms;
      unit->status.failed_reason.clear();

      if (!unit->cold_boot_recorded) {
        unit->status.started_offset_ms = offset;
      }
    } else if (next_state == launcher::UnitState::kReady) {
      if (!unit->cold_boot_recorded) {
        unit->status.ready_offset_ms = offset;
        unit->status.startup_duration_ms =
            (offset >= unit->status.started_offset_ms) ? (offset - unit->status.started_offset_ms) : 0U;
        unit->cold_boot_recorded = true;
      }

      unit->ready_since_steady_ms = now_steady;

      if (unit->watchdog_ms > 0U) {
        unit->watchdog_armed_ms = now_steady;
      }
    } else if (next_state == launcher::UnitState::kStopped || next_state == launcher::UnitState::kFailed ||
               next_state == launcher::UnitState::kSkipped) {
      if (!unit->cold_boot_recorded) {
        unit->status.ready_offset_ms = offset;
        unit->status.startup_duration_ms =
            (unit->status.started_offset_ms > 0U && offset >= unit->status.started_offset_ms)
                ? (offset - unit->status.started_offset_ms)
                : 0U;
        unit->cold_boot_recorded = true;
      }

      unit->ready_since_steady_ms = 0U;
      unit->status.pid = (unit->status.type == launcher::UnitType::kComponent) ? Utils::get_pid() : -1;
      unit->status.cgroup_path.clear();
      unit->status.uptime_ms = 0U;
    } else {
      unit->ready_since_steady_ms = 0U;
    }
  }

  state_cv.notify_all();
  publish_event(ev);
  schedule_snapshot_publish();
}

bool LauncherServer::Impl::transition_unit_if(const std::shared_ptr<UnitRuntime>& unit, launcher::UnitState expected,
                                              launcher::UnitState next_state, std::string_view event_name,
                                              std::string_view detail) {
  launcher::EventRecord ev;

  {
    std::lock_guard lock(state_mtx);

    if (unit->status.state != expected) {
      return false;
    }

    ev.timestamp_ms = ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMilli);
    ev.unit_name = unit->status.name;
    ev.unit_type = unit->status.type;
    ev.old_state = unit->status.state;
    ev.new_state = next_state;
    ev.event_name = std::string(event_name);
    ev.message = std::string(detail);

    unit->status.state = next_state;
    unit->status.detail = std::string(detail);

    const uint64_t now_steady = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);
    const uint64_t offset = (now_steady >= server_start_steady_ms) ? (now_steady - server_start_steady_ms) : 0U;

    if (next_state == launcher::UnitState::kStarting) {
      unit->status.start_time_ms = ev.timestamp_ms;
      unit->status.failed_reason.clear();
      unit->ready_since_steady_ms = 0U;

      if (!unit->cold_boot_recorded) {
        unit->status.started_offset_ms = offset;
      }
    } else if (next_state == launcher::UnitState::kReady) {
      if (!unit->cold_boot_recorded) {
        unit->status.ready_offset_ms = offset;
        unit->status.startup_duration_ms =
            (offset >= unit->status.started_offset_ms) ? (offset - unit->status.started_offset_ms) : 0U;
        unit->cold_boot_recorded = true;
      }

      unit->ready_since_steady_ms = now_steady;

      if (unit->watchdog_ms > 0U) {
        unit->watchdog_armed_ms = now_steady;
      }
    } else if (next_state == launcher::UnitState::kStopped || next_state == launcher::UnitState::kFailed ||
               next_state == launcher::UnitState::kSkipped) {
      if (!unit->cold_boot_recorded) {
        unit->status.ready_offset_ms = offset;
        unit->status.startup_duration_ms =
            (unit->status.started_offset_ms > 0U && offset >= unit->status.started_offset_ms)
                ? (offset - unit->status.started_offset_ms)
                : 0U;
        unit->cold_boot_recorded = true;
      }

      unit->ready_since_steady_ms = 0U;
      unit->status.pid = (unit->status.type == launcher::UnitType::kComponent) ? Utils::get_pid() : -1;
      unit->status.cgroup_path.clear();
      unit->status.uptime_ms = 0U;
    } else {
      unit->ready_since_steady_ms = 0U;
    }
  }

  state_cv.notify_all();
  publish_event(ev);
  schedule_snapshot_publish();
  return true;
}

bool LauncherServer::Impl::check_and_record_start_limit(const std::shared_ptr<UnitRuntime>& unit,
                                                        std::string* err_out) {
  std::lock_guard lock(state_mtx);

  if (unit->respawn.start_limit_interval_ms == 0U || unit->respawn.start_limit_burst == 0U) {
    return true;
  }

  const uint64_t now = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);

  if (unit->start_limit_window_start_ms == 0U ||
      (now > unit->start_limit_window_start_ms &&
       (now - unit->start_limit_window_start_ms) > unit->respawn.start_limit_interval_ms)) {
    unit->start_limit_window_start_ms = now;
    unit->start_limit_attempts = 1U;
    return true;
  }

  if (unit->start_limit_attempts >= unit->respawn.start_limit_burst) {
    unit->status.failed_reason = "start_limit";

    if (err_out != nullptr) {
      *err_out =
          "Start request repeated too quickly (start_limit_burst=" + std::to_string(unit->respawn.start_limit_burst) +
          ").";
    }

    return false;
  }

  ++unit->start_limit_attempts;
  return true;
}

void LauncherServer::Impl::mark_unit_boot_terminal(const std::shared_ptr<UnitRuntime>& unit, bool succeeded) {
  std::vector<std::shared_ptr<UnitRuntime>> ready_succs;
  bool dag_just_finished = false;
  bool should_pause = false;

  {
    std::lock_guard lock(state_mtx);

    if (unit->boot_terminal) {
      return;
    }

    unit->boot_terminal = true;

    if (!succeeded) {
      unit->boot_failed = true;
    }

    if (unit->boot_participant && pending_boot_units > 0U) {
      --pending_boot_units;

      auto sit = stage_remaining.find(unit->status.stage);

      if (sit != stage_remaining.end() && sit->second > 0U) {
        --sit->second;
      }

      if (pending_boot_units == 0U && !cold_boot_complete) {
        cold_boot_complete = true;
        dag_just_finished = true;
      }
    }

    if (!succeeded && config.pause_on_fail && !stopping.load(std::memory_order_acquire) &&
        !paused_on_fail.exchange(true, std::memory_order_acq_rel)) {
      should_pause = true;
    }

    while (active_stage_idx < sorted_stages.size()) {
      const int32_t cur_st = sorted_stages[active_stage_idx];
      auto sit = stage_remaining.find(cur_st);

      if (sit != stage_remaining.end() && sit->second > 0U) {
        break;
      }

      ++active_stage_idx;

      if (active_stage_idx < sorted_stages.size()) {
        const int32_t next_st = sorted_stages[active_stage_idx];
        auto suit = stage_units.find(next_st);

        if (suit != stage_units.end()) {
          for (const auto& uname : suit->second) {
            auto uit = units.find(uname);

            if (uit == units.end()) {
              continue;
            }

            auto& cand = uit->second;

            if (!cand->boot_terminal && !cand->boot_enqueued && cand->remaining_boot_preds == 0U) {
              cand->boot_enqueued = true;
              cand->status.blocker_unit = unit->status.name;
              ready_succs.push_back(cand);
            }
          }
        }
      }
    }

    const bool stage_open = (active_stage_idx < sorted_stages.size());
    const int32_t open_stage = stage_open ? sorted_stages[active_stage_idx] : 0;

    for (const auto& sname : unit->order_succs) {
      auto it = units.find(sname);

      if (it == units.end() || it->second->boot_terminal) {
        continue;
      }

      auto& succ = it->second;

      if (succ->remaining_boot_preds > 0U) {
        --succ->remaining_boot_preds;
        succ->last_unblocker = unit->status.name;

        if (succ->remaining_boot_preds == 0U && !succ->boot_enqueued &&
            (!stage_open || succ->status.stage <= open_stage)) {
          succ->boot_enqueued = true;
          succ->status.blocker_unit = succ->last_unblocker;
          ready_succs.push_back(succ);
        }
      }
    }
  }

  if (should_pause) {
    log_manager.emit_launcher_message(unit->status.name, Logger::kWarn,
                                      "Startup paused (--pause_on_fail); invoke 'ctl resume' to continue.");
    launcher::EventRecord ev;
    ev.timestamp_ms = ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMilli);
    ev.unit_name = "launcher";
    ev.event_name = "on_startup_paused";
    ev.message = "paused after failure of " + unit->status.name;
    publish_event(ev);
  }

  for (const auto& succ : ready_succs) {
    enqueue_ready_unit(succ);
  }

  if (dag_just_finished && !stopping.load(std::memory_order_acquire)) {
    const uint64_t now = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);
    const uint64_t dur = (now >= server_start_steady_ms) ? (now - server_start_steady_ms) : 0U;
    boot_duration_ms.store(dur, std::memory_order_release);

    launcher::EventRecord ev;
    ev.timestamp_ms = ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMilli);
    ev.unit_name = "launcher";
    ev.event_name = "on_dag_complete";
    ev.message = "startup DAG completed in " + std::to_string(dur) + "ms";
    publish_event(ev);
    publish_snapshot();
  }
}

void LauncherServer::Impl::enqueue_ready_unit(const std::shared_ptr<UnitRuntime>& unit) {
  if (!unit || stopping.load(std::memory_order_acquire)) {
    return;
  }

  const uint64_t now = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);

  {
    std::lock_guard lock(state_mtx);

    if (!unit->cold_boot_recorded) {
      unit->status.queued_offset_ms = (now >= server_start_steady_ms) ? (now - server_start_steady_ms) : 0U;
    }
  }

  if (!unit->condition || !unit->selected_for_boot || (!unit->autostart && !unit->explicit_boot_dep)) {
    const char* reason = "filtered";

    if (!unit->condition) {
      reason = "condition false";
    } else if (!unit->autostart) {
      reason = "autostart false";
    }

    transition_unit(unit, launcher::UnitState::kSkipped, "on_skip", reason);
    mark_unit_boot_terminal(unit, true);
    return;
  }

  std::string failed_dep;

  {
    std::lock_guard lock(state_mtx);

    for (const auto& dep_name : unit->req_preds) {
      auto it = units.find(dep_name);

      if (it == units.end()) {
        continue;
      }

      if (it->second->status.state == launcher::UnitState::kFailed || it->second->boot_failed) {
        failed_dep = dep_name;
        unit->boot_failed = true;
        unit->status.failed_reason = "dependency_failed";
        break;
      }
    }
  }

  if (!failed_dep.empty()) {
    transition_unit(unit, launcher::UnitState::kSkipped, "on_dep_failed", "required dependency failed: " + failed_dep);
    mark_unit_boot_terminal(unit, false);
    return;
  }

  if (unit->delay_ms > 0U && wheel_timer && wheel_timer->is_running()) {
    std::weak_ptr<std::atomic_bool> weak_alive = alive_token;
    const std::string uname = unit->status.name;
    const int64_t key = wheel_timer->add(unit->delay_ms, [this, weak_alive, uname](WheelTimer::Key) {
      auto guard = weak_alive.lock();

      if (!guard || !guard->load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire)) {
        return;
      }

      (void)post_protected([this, weak_alive, uname] {
        auto g2 = weak_alive.lock();

        if (!g2 || !g2->load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire)) {
          return;
        }

        auto u = find_unit(uname);

        if (!u) {
          return;
        }

        {
          std::lock_guard lock(state_mtx);
          u->delay_timer_key = -1;
          startup_queue.push_back(uname);
        }

        pump_startup_queue();
      });
    });

    std::lock_guard lock(state_mtx);
    unit->delay_timer_key = key;
    return;
  }

  {
    std::lock_guard lock(state_mtx);
    startup_queue.push_back(unit->status.name);
  }

  pump_startup_queue();
}

void LauncherServer::Impl::pump_startup_queue() {
  if (stopping.load(std::memory_order_acquire) || paused_on_fail.load(std::memory_order_acquire)) {
    return;
  }

  std::vector<std::shared_ptr<UnitRuntime>> to_launch;

  {
    std::lock_guard lock(state_mtx);

    while (!startup_queue.empty()) {
      if (effective_max_parallel > 0U && in_flight_starts >= effective_max_parallel) {
        break;
      }

      const std::string uname = std::move(startup_queue.front());
      startup_queue.pop_front();
      auto it = units.find(uname);

      if (it == units.end() || it->second->boot_terminal) {
        continue;
      }

      const auto st = it->second->status.state;

      if (is_active_state(st) || st == launcher::UnitState::kStopping || st == launcher::UnitState::kStopped) {
        continue;
      }

      it->second->holds_start_slot.store(true, std::memory_order_release);
      ++in_flight_starts;
      to_launch.push_back(it->second);
    }
  }

  for (const auto& u : to_launch) {
    std::weak_ptr<std::atomic_bool> weak_alive = alive_token;
    const uint64_t expected_gen = u->generation.load(std::memory_order_acquire);

    if VUNLIKELY (!post_protected([this, weak_alive, u, expected_gen] {
                    auto guard = weak_alive.lock();

                    if (!guard || !guard->load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire) ||
                        u->generation.load(std::memory_order_acquire) != expected_gen) {
                      release_start_slot(u);
                      return;
                    }

                    std::string err;

                    if (!launch_unit_now(u, &err)) {
                      bool cancelled = stopping.load(std::memory_order_acquire);

                      if (!cancelled) {
                        std::lock_guard lock(state_mtx);
                        cancelled =
                            (u->status.state == launcher::UnitState::kStopped ||
                             u->status.state == launcher::UnitState::kStopping || is_active_state(u->status.state));
                      }

                      if (cancelled) {
                        mark_unit_boot_terminal(u, false);
                        release_start_slot(u);
                        return;
                      }

                      log_manager.emit_launcher_message(u->status.name, Logger::kError, err);
                      mark_unit_boot_terminal(u, false);
                      release_start_slot(u);
                      trigger_units(u->on_error_trigger);

                      if (u->status.required) {
                        request_async_stop(1);
                      }
                    }
                  })) {
      mark_unit_boot_terminal(u, false);
      release_start_slot(u);
    }
  }
}

bool LauncherServer::Impl::has_matching_topic(const std::string& expected_topic) {
  std::shared_ptr<DiscoveryViewer> viewer;

  {
    std::lock_guard lock(viewer_mtx);
    viewer = discovery_viewer;
  }

  if (!viewer) {
    return false;
  }

  static constexpr uint32_t kProducerMask = static_cast<uint32_t>(ImplType::kPublisher) |
                                            static_cast<uint32_t>(ImplType::kServer) |
                                            static_cast<uint32_t>(ImplType::kSetter);

  for (const auto& info : viewer->get_info_list()) {
    if ((info.type & kProducerMask) != 0U && url_matches_topic(info.url, expected_topic)) {
      return true;
    }
  }

  return false;
}

void LauncherServer::Impl::finish_readiness(const std::shared_ptr<UnitRuntime>& unit, uint64_t gen, bool ok,
                                            std::string_view detail) {
  if (!unit) {
    return;
  }

  launcher::EventRecord ready_ev;

  {
    std::lock_guard lock(state_mtx);

    if (unit->generation.load(std::memory_order_acquire) != gen) {
      return;
    }

    if (unit->ready_timer_key >= 0 && wheel_timer) {
      (void)wheel_timer->remove(unit->ready_timer_key);
      unit->ready_timer_key = -1;
    }

    if (unit->probe_poll_timer_key >= 0 && wheel_timer) {
      (void)wheel_timer->remove(unit->probe_poll_timer_key);
      unit->probe_poll_timer_key = -1;
    }

    const auto st = unit->status.state;

    if (st != launcher::UnitState::kStarting && st != launcher::UnitState::kRunning) {
      return;
    }

    if (ok) {
      const uint64_t now_steady = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);
      const uint64_t offset = (now_steady >= server_start_steady_ms) ? (now_steady - server_start_steady_ms) : 0U;

      ready_ev.timestamp_ms = ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMilli);
      ready_ev.unit_name = unit->status.name;
      ready_ev.unit_type = unit->status.type;
      ready_ev.old_state = st;
      ready_ev.new_state = launcher::UnitState::kReady;
      ready_ev.event_name = "on_ready";
      ready_ev.message = std::string(detail);

      unit->status.state = launcher::UnitState::kReady;
      unit->status.detail = std::string(detail);
      unit->ready_since_steady_ms = now_steady;

      if (!unit->cold_boot_recorded) {
        unit->status.ready_offset_ms = offset;
        unit->status.startup_duration_ms =
            (offset >= unit->status.started_offset_ms) ? (offset - unit->status.started_offset_ms) : 0U;
        unit->cold_boot_recorded = true;
      }

      if (unit->watchdog_ms > 0U) {
        unit->watchdog_armed_ms = now_steady;
      }
    } else {
      unit->generation.fetch_add(1U, std::memory_order_acq_rel);
      unit->status.state = launcher::UnitState::kStopping;
      unit->status.failed_reason = "ready_timeout";
      unit->ready_since_steady_ms = 0U;
    }
  }

  if (ok) {
    state_cv.notify_all();
    publish_event(ready_ev);
    schedule_snapshot_publish();
    release_start_slot(unit);
    mark_unit_boot_terminal(unit, true);
    trigger_units(unit->on_ready_trigger);
  } else {
    if (unit->status.type == launcher::UnitType::kComponent) {
      (void)component_host.stop_component(unit->status.name);
    } else {
      if (unit->status.type == launcher::UnitType::kContainer) {
        std::shared_ptr<LauncherAPI> cli;

        {
          std::lock_guard lock(state_mtx);
          cli = std::move(unit->container_client);
        }

        if (cli) {
          cli->quit(true);
          cli->wait_for_quit(500);
        }
      }

      (void)process_supervisor.stop_process(unit->status.name, 500U);
    }

    {
      std::lock_guard lock(state_mtx);
      unit->active_record_path.clear();
    }

    const bool should_restart =
        !config.disable_respawn && !stopping.load(std::memory_order_acquire) &&
        (unit->on_exit == launcher::OnExitAction::kRestart ||
         (unit->respawn.enabled && (unit->respawn.policy == launcher::RestartPolicy::kAlways ||
                                    unit->respawn.policy == launcher::RestartPolicy::kOnFailure ||
                                    unit->respawn.policy == launcher::RestartPolicy::kOnAbnormal)));

    if (should_restart) {
      cascade_stop_bound_dependents(unit->status.name);
      schedule_respawn(unit);
      release_start_slot(unit);
      return;
    }

    transition_unit(unit, launcher::UnitState::kFailed, "on_ready_failed", detail);
    mark_unit_boot_terminal(unit, false);
    release_start_slot(unit);
    trigger_units(unit->on_error_trigger);

    if (unit->status.required) {
      request_async_stop(1);
    }
  }
}

void LauncherServer::Impl::arm_readiness_probe(const std::shared_ptr<UnitRuntime>& unit, uint64_t gen) {
  if (unit->status.type == launcher::UnitType::kContainer) {
    if (!config.enable_control_plane || !manifest.control_enabled) {
      finish_readiness(unit, gen, true, "container started");
      return;
    }

    std::weak_ptr<std::atomic_bool> weak_alive = alive_token;
    const std::string uname = unit->status.name;
    const uint32_t timeout_ms = 5000U;
    const uint64_t deadline = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli) + timeout_ms;
    const int64_t poll_key = wheel_timer->add(
        50U,
        [this, weak_alive, uname, gen, deadline](WheelTimer::Key k) {
          auto guard = weak_alive.lock();

          if (!guard || !guard->load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire)) {
            if (wheel_timer) {
              (void)wheel_timer->remove(k);
            }

            return;
          }

          (void)post_protected([this, weak_alive, uname, gen, deadline, k] {
            auto g2 = weak_alive.lock();

            if (!g2 || !g2->load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire)) {
              if (wheel_timer) {
                (void)wheel_timer->remove(k);
              }

              return;
            }

            auto u = find_unit(uname);

            if (!u || u->generation.load(std::memory_order_acquire) != gen) {
              if (wheel_timer) {
                (void)wheel_timer->remove(k);
              }

              return;
            }

            auto cli = get_or_connect_container_client(u, 10U);

            if (cli && cli->is_connected()) {
              finish_readiness(u, gen, true, "container control ready");
              return;
            }

            if (ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli) >= deadline) {
              finish_readiness(u, gen, false, "container control plane connection timed out");
            }
          });
        },
        50U);

    bool stale = false;

    {
      std::lock_guard lock(state_mtx);

      if (unit->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire)) {
        stale = true;
      } else {
        if (unit->probe_poll_timer_key >= 0 && wheel_timer) {
          (void)wheel_timer->remove(unit->probe_poll_timer_key);
        }

        unit->probe_poll_timer_key = poll_key;
      }
    }

    if (stale && wheel_timer) {
      (void)wheel_timer->remove(poll_key);
    }

    return;
  }

  const auto probe = unit->ready;

  if (probe.type == launcher::ReadyProbeType::kNone) {
    finish_readiness(unit, gen, true, "ready");
    return;
  }

  std::weak_ptr<std::atomic_bool> weak_alive = alive_token;
  const std::string uname = unit->status.name;

  if (probe.type == launcher::ReadyProbeType::kDelay) {
    const uint32_t dms = std::max<uint32_t>(1U, probe.delay_ms);
    const int64_t key = wheel_timer->add(dms, [this, weak_alive, uname, gen](WheelTimer::Key) {
      auto guard = weak_alive.lock();

      if (!guard || !guard->load(std::memory_order_acquire)) {
        return;
      }

      (void)post_protected([this, weak_alive, uname, gen] {
        auto g2 = weak_alive.lock();

        if (!g2 || !g2->load(std::memory_order_acquire)) {
          return;
        }

        auto u = find_unit(uname);

        if (u) {
          finish_readiness(u, gen, true, "delay probe elapsed");
        }
      });
    });

    bool stale = false;

    {
      std::lock_guard lock(state_mtx);

      if (unit->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire)) {
        stale = true;
      } else {
        if (unit->ready_timer_key >= 0 && wheel_timer) {
          (void)wheel_timer->remove(unit->ready_timer_key);
        }

        unit->ready_timer_key = key;
      }
    }

    if (stale && wheel_timer) {
      (void)wheel_timer->remove(key);
    }

    return;
  }

  const int64_t timeout_key = wheel_timer->add(probe.timeout_ms, [this, weak_alive, uname, gen](WheelTimer::Key) {
    auto guard = weak_alive.lock();

    if (!guard || !guard->load(std::memory_order_acquire)) {
      return;
    }

    (void)post_protected([this, weak_alive, uname, gen] {
      auto g2 = weak_alive.lock();

      if (!g2 || !g2->load(std::memory_order_acquire)) {
        return;
      }

      auto u = find_unit(uname);

      if (u) {
        finish_readiness(u, gen, false, "readiness probe timed out");
      }
    });
  });

  {
    bool stale = false;

    {
      std::lock_guard lock(state_mtx);

      if (unit->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire)) {
        stale = true;
      } else {
        if (unit->ready_timer_key >= 0 && wheel_timer) {
          (void)wheel_timer->remove(unit->ready_timer_key);
        }

        unit->ready_timer_key = timeout_key;
      }
    }

    if (stale && wheel_timer) {
      (void)wheel_timer->remove(timeout_key);
      return;
    }
  }

  if (probe.type == launcher::ReadyProbeType::kFile || probe.type == launcher::ReadyProbeType::kTopic) {
    if (probe.type == launcher::ReadyProbeType::kTopic) {
      (void)ensure_discovery_viewer();
    }

    const int64_t poll_key = wheel_timer->add(
        50U,
        [this, weak_alive, uname, gen, probe](WheelTimer::Key k) {
          auto guard = weak_alive.lock();

          if (!guard || !guard->load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire)) {
            if (wheel_timer) {
              (void)wheel_timer->remove(k);
            }

            return;
          }

          auto u_chk = find_unit(uname);

          if (!u_chk || u_chk->generation.load(std::memory_order_acquire) != gen) {
            if (wheel_timer) {
              (void)wheel_timer->remove(k);
            }

            return;
          }

          bool matched = false;

          if (probe.type == launcher::ReadyProbeType::kFile) {
            std::error_code ec;
            matched = std::filesystem::exists(probe.path, ec);
          } else {
            matched = has_matching_topic(probe.topic);
          }

          if (!matched) {
            return;
          }

          (void)post_protected([this, weak_alive, uname, gen, probe, k] {
            auto g2 = weak_alive.lock();

            if (!g2 || !g2->load(std::memory_order_acquire)) {
              if (wheel_timer) {
                (void)wheel_timer->remove(k);
              }

              return;
            }

            auto u = find_unit(uname);

            if (!u || u->generation.load(std::memory_order_acquire) != gen) {
              if (wheel_timer) {
                (void)wheel_timer->remove(k);
              }

              return;
            }

            finish_readiness(u, gen, true,
                             probe.type == launcher::ReadyProbeType::kFile ? "file ready" : "topic ready");
          });
        },
        50U);

    bool stale = false;

    {
      std::lock_guard lock(state_mtx);

      if (unit->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire)) {
        stale = true;
      } else {
        if (unit->probe_poll_timer_key >= 0 && wheel_timer) {
          (void)wheel_timer->remove(unit->probe_poll_timer_key);
        }

        unit->probe_poll_timer_key = poll_key;
      }
    }

    if (stale && wheel_timer) {
      (void)wheel_timer->remove(poll_key);
    }
  }
}

std::string LauncherServer::Impl::build_proxy_config_file(const launcher::ProxyConfig& proxy) const {
  if (!proxy.config_file.empty()) {
    return proxy.config_file;
  }

  nlohmann::ordered_json cfg;
  cfg["domain_id"] = proxy.domain_id;

  if (!proxy.dds_impl.empty()) {
    cfg["dds_impl"] = proxy.dds_impl;
  }

  if (proxy.use_iox) {
    if (!proxy.iox_config.empty()) {
      cfg["iox_config"] = proxy.iox_config;
    } else if (proxy.iox_strategy > 0) {
      cfg["iox_strategy"] = proxy.iox_strategy;
    }
  }

  std::string path = log_manager.tmp_dir() + "/" + launcher::sanitize_identifier(proxy.name) + "_proxy.json";
  std::ofstream ofs(path, std::ios::out | std::ios::trunc);

  if (!ofs.is_open()) {
    return {};
  }

  ofs << cfg.dump(2) << '\n';
  ofs.flush();

  if (!ofs.good()) {
    return {};
  }

  return path;
}

std::string LauncherServer::Impl::normalize_record_path(const std::string& p_str) const {
  if (p_str.empty()) {
    return {};
  }

  std::filesystem::path p(p_str);

  if (p.is_relative() && !active_record_dir.empty()) {
    p = std::filesystem::path(active_record_dir) / p;
  }

  std::error_code ec;
  const auto abs_p = std::filesystem::absolute(p, ec);
  const std::filesystem::path base_p = ec ? p : abs_p;
  std::error_code can_ec;
  const auto canon_p = std::filesystem::weakly_canonical(base_p, can_ec);
  const std::filesystem::path norm_p = (!can_ec && !canon_p.empty()) ? canon_p : base_p.lexically_normal();
  return Helpers::path_to_string(norm_p);
}

bool LauncherServer::Impl::check_cross_proc_record_collision(const std::string& target_domain,
                                                             const std::string& resolved_p,
                                                             std::string& conflict_out) const {
  const std::string norm = normalize_record_path(resolved_p);

  if (norm.empty()) {
    return true;
  }

  std::lock_guard lock(state_mtx);

  for (const auto& [uname, unit_ptr] : units) {
    if (unit_ptr->status.type == launcher::UnitType::kComponent) {
      const bool comp_rec = unit_ptr->comp_spec.record_enabled.value_or(!unit_ptr->comp_spec.record_path.empty());
      const bool matches_static = comp_rec && !unit_ptr->comp_spec.record_path.empty() &&
                                  normalize_record_path(unit_ptr->comp_spec.record_path) == norm;
      const bool matches_active =
          !unit_ptr->active_record_path.empty() && normalize_record_path(unit_ptr->active_record_path) == norm;

      if (target_domain != "__host__" && (matches_static || matches_active)) {
        conflict_out = "Component '" + uname + "'";
        return false;
      }
    } else if (unit_ptr->status.type == launcher::UnitType::kContainer) {
      const std::string dom = "container:" + uname;
      const bool cont_rec =
          unit_ptr->container_spec.record_enabled.value_or(!unit_ptr->container_spec.record_path.empty());
      const bool matches_static = cont_rec && !unit_ptr->container_spec.record_path.empty() &&
                                  normalize_record_path(unit_ptr->container_spec.record_path) == norm;
      const bool matches_active =
          !unit_ptr->active_record_path.empty() && normalize_record_path(unit_ptr->active_record_path) == norm;

      if (target_domain != dom && is_active_state(unit_ptr->status.state) && (matches_static || matches_active)) {
        conflict_out = "Container '" + uname + "'";
        return false;
      }
    } else if (unit_ptr->status.type == launcher::UnitType::kProcess) {
      const std::string dom = "process:" + uname;
      const bool proc_rec = unit_ptr->proc_spec.record_enabled.value_or(!unit_ptr->proc_spec.record_path.empty());
      const bool matches_static = proc_rec && !unit_ptr->proc_spec.record_path.empty() &&
                                  normalize_record_path(unit_ptr->proc_spec.record_path) == norm;
      const bool matches_active =
          !unit_ptr->active_record_path.empty() && normalize_record_path(unit_ptr->active_record_path) == norm;

      if (target_domain != dom && is_active_state(unit_ptr->status.state) && (matches_static || matches_active)) {
        conflict_out = "Process '" + uname + "'";
        return false;
      }
    }
  }

  for (const auto& [comp_name, rec_p] : remote_record_paths) {
    auto cit = component_to_container.find(comp_name);

    if (cit != component_to_container.end()) {
      const std::string dom = "container:" + cit->second;

      if (target_domain != dom && !rec_p.empty() && normalize_record_path(rec_p) == norm) {
        conflict_out = "Component '" + comp_name + "' in container '" + cit->second + "'";
        return false;
      }
    }
  }

  return true;
}

std::string LauncherServer::Impl::resolve_unit_record_path(const std::string& unit_name, launcher::UnitType type,
                                                           const std::string& explicit_path, uint64_t generation,
                                                           uint64_t segment) const {
  std::string base_dir = active_record_dir;

  if (base_dir.empty()) {
    base_dir = log_manager.session_dir().empty() ? "./bags" : (log_manager.session_dir() + "/bags");
  }

  std::error_code ec;
  const auto abs_base = std::filesystem::absolute(std::filesystem::path(base_dir), ec);

  if (!ec && !abs_base.empty()) {
    base_dir = Helpers::path_to_string(abs_base);
  }

  std::filesystem::path target_path;

  if (!explicit_path.empty()) {
    target_path = std::filesystem::path(explicit_path);

    if (target_path.is_relative()) {
      target_path = std::filesystem::path(base_dir) / target_path;
    }

    if ((type == launcher::UnitType::kProcess || type == launcher::UnitType::kContainer) && generation > 1U) {
      const std::string stem = target_path.stem().string();
      const std::string ext = target_path.extension().string();
      target_path = target_path.parent_path() / (stem + "_g" + std::to_string(generation) + ext);
    }
  } else {
    const char* sub_dir = "processes";

    if (type == launcher::UnitType::kComponent) {
      sub_dir = "components";
    } else if (type == launcher::UnitType::kContainer) {
      sub_dir = "containers";
    }

    std::string file_name = launcher::sanitize_identifier(unit_name);

    if (type == launcher::UnitType::kProcess || type == launcher::UnitType::kContainer || generation > 1U) {
      file_name.append("_g").append(std::to_string(generation));
    }

    if (segment > 0U) {
      file_name.append("_s").append(std::to_string(segment));
    }

    const std::string fmt = active_record_format.empty() ? "vcap" : active_record_format;
    file_name.push_back('.');
    file_name.append(fmt);
    target_path = std::filesystem::path(base_dir) / sub_dir / file_name;
  }

  const auto abs_target = std::filesystem::absolute(target_path, ec);

  if (!ec && !abs_target.empty()) {
    target_path = abs_target;
  }

  if (target_path.has_parent_path()) {
    std::filesystem::create_directories(target_path.parent_path(), ec);
  }

  return Helpers::path_to_string(target_path);
}

std::string LauncherServer::Impl::build_container_manifest_file(const std::shared_ptr<UnitRuntime>& unit) const {
  std::vector<launcher::ComponentSpec> comps;

  {
    std::lock_guard lock(state_mtx);
    auto it = container_components.find(unit->status.name);

    if (it != container_components.end()) {
      comps = it->second;
    }
  }

  const bool container_proc_rec =
      unit->container_spec.record_enabled.value_or(!unit->container_spec.record_path.empty());
  launcher::LaunchManifest container_manifest = manifest;
  container_manifest.logger = effective_logger_cfg;
  container_manifest.record.enabled = !container_proc_rec && active_record_enabled;
  container_manifest.record.dir = active_record_dir;
  container_manifest.record.format = active_record_format;
  container_manifest.record.tag = active_record_tag;
  container_manifest.record.force_publish = active_record_force_publish;

  const bool ctrl_on = config.enable_control_plane && manifest.control_enabled;
  const std::string toml_str = launcher::serialize_container_manifest(
      container_manifest, unit->container_spec, comps, active_ctrl_scheme, unit->container_ctrl_ns, active_ctrl_domain,
      active_bind_ip, active_native_mode, ctrl_on, config.allow_remote_load || manifest.allow_remote_load);

  std::string path = log_manager.tmp_dir() + "/" + launcher::sanitize_identifier(unit->status.name) + "_container.toml";
  std::ofstream ofs(path, std::ios::out | std::ios::trunc);

  if (!ofs.is_open()) {
    return {};
  }

  ofs << toml_str;
  ofs.flush();

  if (!ofs.good()) {
    return {};
  }

  return path;
}

std::shared_ptr<LauncherAPI> LauncherServer::Impl::get_or_connect_container_client(
    const std::shared_ptr<UnitRuntime>& container_unit, uint32_t wait_ms) {
  if (!container_unit) {
    return nullptr;
  }

  std::shared_ptr<LauncherAPI> cli;

  {
    std::lock_guard lock(state_mtx);

    if (container_unit->container_ctrl_ns.empty()) {
      return nullptr;
    }

    cli = container_unit->container_client;

    if (!cli) {
      LauncherAPI::Config cfg;
      cfg.scheme = active_ctrl_scheme;
      cfg.namespace_prefix = container_unit->container_ctrl_ns;
      cfg.domain_id = active_ctrl_domain;
      cfg.security_key = active_security_key;
      cfg.bind_ip = active_bind_ip;
      cfg.native_mode = active_native_mode;
      cfg.subscribe_events = false;
      cfg.subscribe_logs = false;
      cfg.default_timeout_ms = 3000U;
      cli = MemoryResource::make_shared<LauncherAPI>(cfg);
      cli->async_run();
      container_unit->container_client = cli;
    }
  }

  if (wait_ms > 0U && !cli->is_connected()) {
    (void)cli->wait_for_connected(wait_ms);
  }

  return cli;
}

bool LauncherServer::Impl::launch_unit_now(const std::shared_ptr<UnitRuntime>& unit, std::string* error_out) {
  if (!unit || stopping.load(std::memory_order_acquire)) {
    if (error_out != nullptr) {
      *error_out = "Server is stopping.";
    }

    return false;
  }

  std::vector<std::string> conflicts_to_stop;
  launcher::EventRecord start_ev;
  uint64_t gen = 0U;

  {
    std::lock_guard lock(state_mtx);
    const auto cur_st = unit->status.state;

    if (is_active_state(cur_st) || cur_st == launcher::UnitState::kStopping ||
        cur_st == launcher::UnitState::kInactive) {
      if (error_out != nullptr) {
        *error_out = "Unit '" + unit->status.name + "' is already active or stopping.";
      }

      return false;
    }

    gen = unit->generation.fetch_add(1U, std::memory_order_acq_rel) + 1U;
    unit->status.generation = gen;
    unit->status.state = launcher::UnitState::kStarting;
    unit->status.detail = "starting";
    unit->status.healthy = true;
    unit->status.liveness_failures = 0U;
    unit->status.failed_reason.clear();
    unit->status.start_time_ms = ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMilli);
    unit->ready_since_steady_ms = 0U;
    unit->oneshot_completed_ok = false;
    unit->active_record_path.clear();

    if (!unit->cold_boot_recorded && unit->status.started_offset_ms == 0U && server_start_steady_ms > 0U) {
      const uint64_t now_s = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);
      unit->status.started_offset_ms = (now_s >= server_start_steady_ms) ? (now_s - server_start_steady_ms) : 0U;
    }

    start_ev.timestamp_ms = unit->status.start_time_ms;
    start_ev.unit_name = unit->status.name;
    start_ev.unit_type = unit->status.type;
    start_ev.old_state = cur_st;
    start_ev.new_state = launcher::UnitState::kStarting;
    start_ev.event_name = "on_start";
    start_ev.message = "starting";

    for (const auto& cname : unit->conflict_units) {
      auto it = units.find(cname);

      if (it != units.end()) {
        const auto st = it->second->status.state;
        const bool has_pending =
            (it->second->respawn_timer_key >= 0 || it->second->delay_timer_key >= 0 ||
             st == launcher::UnitState::kRestarting || is_active_state(st) || st == launcher::UnitState::kInactive);

        if (has_pending) {
          conflicts_to_stop.push_back(cname);
        }
      }
    }
  }

  state_cv.notify_all();
  publish_event(start_ev);
  schedule_snapshot_publish();
  cancel_unit_timers(unit);

  for (const auto& cname : conflicts_to_stop) {
    std::unordered_set<std::string> visited;
    (void)stop_unit_cascade(cname, visited, false);
  }

  if (unit->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire)) {
    if (error_out != nullptr) {
      *error_out = "Launch cancelled before start.";
    }

    return false;
  }

  if (!check_and_record_start_limit(unit, error_out)) {
    transition_unit(unit, launcher::UnitState::kFailed, "on_start_limit",
                    error_out != nullptr ? *error_out : "start limit exceeded");
    return false;
  }

  trigger_units(unit->on_start_trigger);

  if (unit->status.type == launcher::UnitType::kComponent) {
    std::string comp_err;
    const bool initial_active = !unit->start_inactive;
    launcher::ComponentSpec effective_spec;

    {
      std::lock_guard lock(state_mtx);
      effective_spec = unit->comp_spec;
    }

    const bool rec_on =
        effective_spec.record_enabled.value_or(!effective_spec.record_path.empty() || active_record_enabled);

    if (rec_on) {
      effective_spec.record_path =
          resolve_unit_record_path(unit->status.name, launcher::UnitType::kComponent, effective_spec.record_path, gen);
    } else {
      effective_spec.record_path.clear();
    }

    effective_spec.record_force_publish =
        rec_on ? effective_spec.record_force_publish.value_or(active_record_force_publish) : false;

    {
      std::lock_guard lock(state_mtx);
      unit->active_record_path = effective_spec.record_path;
    }

    if (!component_host.start_component(effective_spec, &comp_err, initial_active)) {
      if (unit->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire)) {
        if (error_out != nullptr) {
          *error_out = comp_err;
        }

        return false;
      }

      {
        std::lock_guard lock(state_mtx);
        unit->active_record_path.clear();
        unit->status.failed_reason = "component_start_failed";
      }

      transition_unit(unit, launcher::UnitState::kFailed, "on_start_failed", comp_err);

      if (error_out != nullptr) {
        *error_out = comp_err;
      }

      return false;
    }

    if (!initial_active) {
      transition_unit(unit, launcher::UnitState::kInactive, "on_configured", "configured (inactive standby)");
      mark_unit_boot_terminal(unit, true);
      release_start_slot(unit);
      return true;
    }

    const bool moved_running = transition_unit_if(unit, launcher::UnitState::kStarting, launcher::UnitState::kRunning,
                                                  "on_running", "component active");

    if (moved_running) {
      arm_readiness_probe(unit, gen);
    }

    return true;
  }

  launcher::ManagedProcessLaunch launch;
  launch.name = unit->status.name;
  launch.generation = gen;
  launch.resources = unit->resources;
  launch.kill_mode = unit->kill_mode;
  launch.stop_signal = unit->stop_signal;
  launch.sigterm_timeout_ms = unit->sigterm_timeout_ms;
  std::string active_rec_path;

  if (unit->status.type == launcher::UnitType::kProxy) {
    const std::string proxy_cfg_path = build_proxy_config_file(unit->proxy_spec);

    if VUNLIKELY (proxy_cfg_path.empty()) {
      const std::string err = "Failed to write proxy config file for '" + unit->status.name + "'.";
      {
        std::lock_guard lock(state_mtx);
        unit->status.failed_reason = "config_write_failed";
      }

      transition_unit(unit, launcher::UnitState::kFailed, "on_start_failed", err);

      if (error_out != nullptr) {
        *error_out = err;
      }

      return false;
    }

    launch.program = unit->proxy_spec.program;
    launch.args.emplace_back("--config");
    launch.args.push_back(proxy_cfg_path);

    for (const auto& a : unit->proxy_spec.extra_args) {
      launch.args.push_back(a);
    }

    launch.env = unit->proxy_spec.env;
    launch.env["VLINK_BAG_PATH"] = "";
    launch.env["VLINK_LAUNCHER_SECURITY_KEY"] = "";
    launch.output = unit->proxy_spec.output;
    launch.inherit_env = true;
  } else if (unit->status.type == launcher::UnitType::kContainer) {
    const std::string launcher_bin = find_launcher_executable(config.launcher_executable);

    if VUNLIKELY (launcher_bin.empty()) {
      const std::string err =
          "Cannot locate 'vlink-launcher' executable to spawn container '" + unit->status.name + "'.";
      {
        std::lock_guard lock(state_mtx);
        unit->status.failed_reason = "missing_launcher_binary";
      }

      transition_unit(unit, launcher::UnitState::kFailed, "on_start_failed", err);

      if (error_out != nullptr) {
        *error_out = err;
      }

      return false;
    }

    {
      std::lock_guard lock(state_mtx);
      unit->container_ctrl_ns = active_ctrl_ns + "/c/" + launcher::sanitize_identifier(unit->status.name);
    }

    const std::string cfg_path = build_container_manifest_file(unit);

    if VUNLIKELY (cfg_path.empty()) {
      const std::string err = "Failed to write container manifest file for '" + unit->status.name + "'.";
      {
        std::lock_guard lock(state_mtx);
        unit->status.failed_reason = "config_write_failed";
      }

      transition_unit(unit, launcher::UnitState::kFailed, "on_start_failed", err);

      if (error_out != nullptr) {
        *error_out = err;
      }

      return false;
    }

    launch.program = launcher_bin;
    launch.args = {"_container", "--name", unit->status.name, "--config", cfg_path};
    launch.launch_prefix = unit->container_spec.launch_prefix;
    launch.cwd = unit->container_spec.cwd;
    launch.env = unit->container_spec.env;

    {
      std::lock_guard lock(state_mtx);
      auto dyn_it = dynamic_container_components.find(unit->status.name);

      if (dyn_it != dynamic_container_components.end()) {
        for (const auto& stale_comp : dyn_it->second) {
          component_to_container.erase(stale_comp);
          remote_record_paths.erase(stale_comp);

          for (auto& [gname, members] : group_members) {
            (void)gname;
            members.erase(std::remove(members.begin(), members.end(), stale_comp), members.end());
          }
        }

        dyn_it->second.clear();
      }
    }

    if (!active_security_key.empty()) {
      launch.env["VLINK_LAUNCHER_SECURITY_KEY"] = active_security_key;
    }

    const bool cont_proc_rec = unit->container_spec.record_enabled.value_or(!unit->container_spec.record_path.empty());

    if (cont_proc_rec) {
      active_rec_path = resolve_unit_record_path(unit->status.name, launcher::UnitType::kContainer,
                                                 unit->container_spec.record_path, gen);
      std::string rec_conflict;

      if VUNLIKELY (!check_cross_proc_record_collision("container:" + unit->status.name, active_rec_path,
                                                       rec_conflict)) {
        const std::string err =
            "Record path '" + active_rec_path + "' conflicts with " + rec_conflict + " in another OS process.";
        {
          std::lock_guard lock(state_mtx);
          unit->status.failed_reason = "record_path_conflict";
        }

        transition_unit(unit, launcher::UnitState::kFailed, "on_start_failed", err);

        if (error_out != nullptr) {
          *error_out = err;
        }

        return false;
      }

      launch.env["VLINK_BAG_PATH"] = active_rec_path;

      if (!active_record_tag.empty() && launch.env.count("VLINK_BAG_TAG") == 0U) {
        launch.env["VLINK_BAG_TAG"] = active_record_tag;
      }
    } else {
      launch.env["VLINK_BAG_PATH"] = "";
    }

    launch.inherit_env = unit->container_spec.inherit_env;
    launch.is_container = true;
    launch.output = unit->container_spec.output;
  } else if (unit->status.type == launcher::UnitType::kProcess) {
    launch.program = unit->proc_spec.program;
    launch.args = unit->proc_spec.args;
    launch.launch_prefix = unit->proc_spec.launch_prefix;
    launch.cwd = unit->proc_spec.cwd;
    launch.env = unit->proc_spec.env;
    launch.env["VLINK_LAUNCHER_SECURITY_KEY"] = "";
    launcher::inject_url_env(unit->proc_spec.urls, unit->proc_spec.inputs, unit->proc_spec.outputs, unit->proc_spec.ns,
                             unit->proc_spec.remaps, launch.env);

    const bool proc_rec =
        unit->proc_spec.record_enabled.value_or(!unit->proc_spec.record_path.empty() || active_record_enabled);

    if (proc_rec) {
      active_rec_path =
          resolve_unit_record_path(unit->status.name, launcher::UnitType::kProcess, unit->proc_spec.record_path, gen);
      std::string rec_conflict;

      if VUNLIKELY (!check_cross_proc_record_collision("process:" + unit->status.name, active_rec_path, rec_conflict)) {
        const std::string err =
            "Record path '" + active_rec_path + "' conflicts with " + rec_conflict + " in another OS process.";
        {
          std::lock_guard lock(state_mtx);
          unit->status.failed_reason = "record_path_conflict";
        }

        transition_unit(unit, launcher::UnitState::kFailed, "on_start_failed", err);

        if (error_out != nullptr) {
          *error_out = err;
        }

        return false;
      }

      launch.env["VLINK_BAG_PATH"] = active_rec_path;

      if (!active_record_tag.empty() && launch.env.count("VLINK_BAG_TAG") == 0U) {
        launch.env["VLINK_BAG_TAG"] = active_record_tag;
      }
    } else if (active_record_enabled || unit->proc_spec.record_enabled.has_value()) {
      launch.env["VLINK_BAG_PATH"] = "";
    }

    launch.inherit_env = unit->proc_spec.inherit_env;
    launch.output = unit->proc_spec.output;
    launch.remaps = unit->proc_spec.remaps;

    if (unit->ready.type == launcher::ReadyProbeType::kStdout) {
      launch.ready_stdout_pattern = unit->ready.pattern;
    }
  } else if (unit->status.type == launcher::UnitType::kTask) {
    launch.program = unit->task_spec.program;
    launch.args = unit->task_spec.args;
    launch.launch_prefix = unit->task_spec.launch_prefix;
    launch.cwd = unit->task_spec.cwd;
    launch.env = unit->task_spec.env;
    launch.env["VLINK_LAUNCHER_SECURITY_KEY"] = "";

    if (active_record_enabled && launch.env.count("VLINK_BAG_PATH") == 0U) {
      launch.env["VLINK_BAG_PATH"] = "";
    }

    launcher::inject_url_env(unit->task_spec.urls, unit->task_spec.inputs, unit->task_spec.outputs, unit->task_spec.ns,
                             unit->task_spec.remaps, launch.env);
    launch.inherit_env = unit->task_spec.inherit_env;
    launch.output = unit->task_spec.output;
    launch.remaps = unit->task_spec.remaps;
  }

  int64_t pid = -1;
  std::string cg_path;
  std::string proc_err;

  if (!process_supervisor.start_process(launch, &pid, &cg_path, &proc_err)) {
    if (unit->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire)) {
      if (error_out != nullptr) {
        *error_out = proc_err;
      }

      return false;
    }

    {
      std::lock_guard lock(state_mtx);
      unit->active_record_path.clear();
      unit->status.failed_reason = "exec_failed";
    }

    transition_unit(unit, launcher::UnitState::kFailed, "on_start_failed", proc_err);

    if (error_out != nullptr) {
      *error_out = proc_err;
    }

    return false;
  }

  bool stale_after_start = false;

  {
    std::lock_guard lock(state_mtx);

    if (unit->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire)) {
      stale_after_start = true;
    } else {
      unit->status.pid = pid;
      unit->status.cgroup_path = cg_path;
      unit->active_record_path = active_rec_path;
    }
  }

  if (stale_after_start) {
    (void)process_supervisor.stop_process(launch.name, 500U, gen);

    if (error_out != nullptr) {
      *error_out = "Unit '" + launch.name + "' was stopped during startup.";
    }

    return false;
  }

  const bool moved_running =
      transition_unit_if(unit, launcher::UnitState::kStarting, launcher::UnitState::kRunning, "on_running", "running");

  if (unit->status.type == launcher::UnitType::kTask) {
    if (unit->task_spec.kind == launcher::TaskKind::kPeriodic) {
      finish_readiness(unit, gen, true, "periodic task armed");
      arm_periodic_run_timeout(unit, gen);
      schedule_periodic_task(unit, gen);
    } else if (unit->task_spec.timeout_ms > 0U && wheel_timer && wheel_timer->is_running()) {
      std::weak_ptr<std::atomic_bool> weak_alive = alive_token;
      const std::string uname = unit->status.name;
      const int64_t tkey =
          wheel_timer->add(unit->task_spec.timeout_ms, [this, weak_alive, uname, gen](WheelTimer::Key) {
            auto guard = weak_alive.lock();

            if (!guard || !guard->load(std::memory_order_acquire)) {
              return;
            }

            (void)post_protected([this, weak_alive, uname, gen] {
              auto g2 = weak_alive.lock();

              if (!g2 || !g2->load(std::memory_order_acquire)) {
                return;
              }

              auto u = find_unit(uname);

              if (!u || u->generation.load(std::memory_order_acquire) != gen) {
                return;
              }

              if (process_supervisor.is_running(uname)) {
                u->generation.fetch_add(1U, std::memory_order_acq_rel);
                (void)process_supervisor.stop_process(uname, 500U, gen);
                {
                  std::lock_guard lock(state_mtx);
                  u->status.failed_reason = "timeout";
                }

                transition_unit(u, launcher::UnitState::kFailed, "on_timeout", "oneshot task timed out");
                mark_unit_boot_terminal(u, false);
                release_start_slot(u);
                trigger_units(u->on_error_trigger);

                if (u->status.required) {
                  request_async_stop(1);
                }
              }
            });
          });

      bool stale_timer = false;

      {
        std::lock_guard lock(state_mtx);

        if (unit->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire)) {
          stale_timer = true;
        } else {
          if (unit->ready_timer_key >= 0 && wheel_timer) {
            (void)wheel_timer->remove(unit->ready_timer_key);
          }

          unit->ready_timer_key = tkey;
        }
      }

      if (stale_timer && wheel_timer) {
        (void)wheel_timer->remove(tkey);
      }
    }

    return true;
  }

  if (moved_running) {
    arm_readiness_probe(unit, gen);
  }

  return true;
}

bool LauncherServer::Impl::stop_unit_internal(const std::shared_ptr<UnitRuntime>& unit,
                                              std::optional<uint32_t> timeout_override_ms) {
  if (!unit) {
    return false;
  }

  cancel_unit_timers(unit);
  unit->generation.fetch_add(1U, std::memory_order_acq_rel);
  release_start_slot(unit);

  bool already_inactive = false;
  bool settle_boot = false;

  {
    std::lock_guard lock(state_mtx);
    startup_queue.erase(std::remove(startup_queue.begin(), startup_queue.end(), unit->status.name),
                        startup_queue.end());
    unit->active_record_path.clear();
    const auto st = unit->status.state;
    settle_boot = unit->boot_participant && !unit->boot_terminal;

    if (st == launcher::UnitState::kStopped || st == launcher::UnitState::kPending ||
        st == launcher::UnitState::kSkipped) {
      already_inactive = true;
    }
  }

  if (already_inactive) {
    if (settle_boot) {
      mark_unit_boot_terminal(unit, false);
    }

    return true;
  }

  transition_unit(unit, launcher::UnitState::kStopping, "on_stopping", "stopping");

  const uint32_t eff_timeout = timeout_override_ms.has_value() ? *timeout_override_ms : unit->sigterm_timeout_ms;

  if (unit->status.type == launcher::UnitType::kComponent) {
    (void)component_host.stop_component(unit->status.name);
  } else {
    if (unit->status.type == launcher::UnitType::kContainer) {
      std::shared_ptr<LauncherAPI> cli;

      {
        std::lock_guard lock(state_mtx);
        cli = std::move(unit->container_client);
      }

      if (cli) {
        if (cli->is_connected() && eff_timeout > 50U) {
          (void)cli->shutdown_server(std::min<uint32_t>(1500U, eff_timeout));
        }

        cli->quit(true);
        cli->wait_for_quit(static_cast<int>(std::min<uint32_t>(1000U, std::max<uint32_t>(50U, eff_timeout))));
      }
    }

    (void)process_supervisor.stop_process(unit->status.name, eff_timeout);
  }

  transition_unit(unit, launcher::UnitState::kStopped, "on_stopped", "stopped");

  if (settle_boot) {
    mark_unit_boot_terminal(unit, false);
  }

  return true;
}

void LauncherServer::Impl::cascade_stop_bound_dependents(const std::string& unit_name) {
  std::vector<std::string> bound_dependents;

  {
    std::lock_guard lock(state_mtx);

    for (const auto& [oname, ounit] : units) {
      const auto st = ounit->status.state;
      const bool stoppable =
          is_active_state(st) || st == launcher::UnitState::kInactive || st == launcher::UnitState::kRestarting;

      if (oname != unit_name && stoppable && contains_str(ounit->binds_preds, unit_name)) {
        bound_dependents.push_back(oname);
      }
    }
  }

  for (const auto& dep : bound_dependents) {
    std::unordered_set<std::string> visited{unit_name};
    (void)stop_unit_cascade(dep, visited, false);
  }
}

bool LauncherServer::Impl::stop_unit_cascade(const std::string& name, std::unordered_set<std::string>& visited,
                                             bool propagate_part_of) {
  if (!visited.insert(name).second) {
    return true;
  }

  auto unit = find_unit(name);

  if (!unit) {
    return false;
  }

  std::vector<std::string> dependents_to_stop;

  {
    std::lock_guard lock(state_mtx);

    for (const auto& [other_name, other_unit] : units) {
      const auto st = other_unit->status.state;
      const bool stoppable =
          is_active_state(st) || st == launcher::UnitState::kInactive || st == launcher::UnitState::kRestarting;

      if (other_name == name || !stoppable) {
        continue;
      }

      if (contains_str(other_unit->binds_preds, name) ||
          (propagate_part_of &&
           (contains_str(other_unit->part_of_preds, name) || contains_str(other_unit->req_preds, name)))) {
        dependents_to_stop.push_back(other_name);
      }
    }
  }

  for (const auto& dep : dependents_to_stop) {
    (void)stop_unit_cascade(dep, visited, propagate_part_of);
  }

  return stop_unit_internal(unit);
}

void LauncherServer::Impl::handle_process_exit(const std::string& name, uint64_t gen, int32_t code,
                                               Process::ExitStatus status, bool expected_stop) {
  std::weak_ptr<std::atomic_bool> weak_alive = alive_token;
  (void)post_protected([this, weak_alive, name, gen, code, status, expected_stop] {
    auto guard = weak_alive.lock();

    if (!guard || !guard->load(std::memory_order_acquire)) {
      return;
    }

    auto unit = find_unit(name);

    if (!unit || unit->generation.load(std::memory_order_acquire) != gen) {
      return;
    }

    std::shared_ptr<LauncherAPI> stale_container_cli;

    {
      std::lock_guard lock(state_mtx);
      unit->status.exit_code = code;
      unit->status.pid = -1;
      unit->status.cgroup_path.clear();
      unit->active_record_path.clear();

      if (unit->status.type == launcher::UnitType::kContainer) {
        stale_container_cli = std::move(unit->container_client);
      }

      if (unit->ready_timer_key >= 0 && wheel_timer) {
        (void)wheel_timer->remove(unit->ready_timer_key);
        unit->ready_timer_key = -1;
      }

      if (unit->probe_poll_timer_key >= 0 && wheel_timer) {
        (void)wheel_timer->remove(unit->probe_poll_timer_key);
        unit->probe_poll_timer_key = -1;
      }
    }

    if (stale_container_cli) {
      stale_container_cli->quit(true);
      stale_container_cli->wait_for_quit(500);
    }

    process_supervisor.reap_exited(name, gen);

    if (expected_stop || stopping.load(std::memory_order_acquire)) {
      release_start_slot(unit);
      return;
    }

    const bool is_normal = (status == Process::kNormalExitStatus);
    const std::vector<int32_t>& success_codes = (unit->status.type == launcher::UnitType::kTask)
                                                    ? unit->task_spec.success_exit_codes
                                                    : unit->respawn.success_exit_codes;
    const bool is_success =
        is_normal && (code == 0 || std::find(success_codes.begin(), success_codes.end(), code) != success_codes.end());

    if (unit->status.type == launcher::UnitType::kTask) {
      if (unit->task_spec.kind == launcher::TaskKind::kPeriodic) {
        if (!is_success) {
          log_manager.emit_launcher_message(name, Logger::kWarn,
                                            "Periodic task exited with code " + std::to_string(code));
        }

        return;
      }

      if (is_success) {
        {
          std::lock_guard lock(state_mtx);
          unit->oneshot_completed_ok = true;
        }

        transition_unit(unit, launcher::UnitState::kStopped, "on_exit",
                        "completed (exit=" + std::to_string(code) + ")");
        mark_unit_boot_terminal(unit, true);
        release_start_slot(unit);
        trigger_units(unit->on_exit_trigger);
      } else {
        {
          std::lock_guard lock(state_mtx);
          unit->status.failed_reason = "exit_code_" + std::to_string(code);
        }

        transition_unit(unit, launcher::UnitState::kFailed, "on_exit",
                        "oneshot failed (exit=" + std::to_string(code) + ")");
        mark_unit_boot_terminal(unit, false);
        release_start_slot(unit);
        trigger_units(unit->on_error_trigger);

        if (unit->status.required) {
          request_async_stop(1);
        }
      }

      return;
    }

    const auto& prev_codes = unit->respawn.restart_prevent_exit_codes;
    const bool prevented = is_normal && std::find(prev_codes.begin(), prev_codes.end(), code) != prev_codes.end();

    bool should_restart = false;

    if (!config.disable_respawn && !prevented) {
      if (unit->on_exit == launcher::OnExitAction::kRestart) {
        should_restart = true;
      } else if (unit->respawn.enabled) {
        switch (unit->respawn.policy) {
          case launcher::RestartPolicy::kAlways:
            should_restart = true;
            break;
          case launcher::RestartPolicy::kOnFailure:
            should_restart = !is_success;
            break;
          case launcher::RestartPolicy::kOnAbnormal:
            should_restart = !is_normal;
            break;
          case launcher::RestartPolicy::kNo:
          default:
            should_restart = false;
            break;
        }
      }
    }

    if (unit->on_exit == launcher::OnExitAction::kShutdown) {
      transition_unit(unit, is_success ? launcher::UnitState::kStopped : launcher::UnitState::kFailed, "on_exit",
                      "exit triggered system shutdown (code=" + std::to_string(code) + ")");
      mark_unit_boot_terminal(unit, is_success);
      release_start_slot(unit);
      cascade_stop_bound_dependents(name);
      trigger_units(unit->on_exit_trigger);
      request_async_stop(is_success ? 0 : 1);
      return;
    }

    if (should_restart) {
      cascade_stop_bound_dependents(name);
      schedule_respawn(unit);
      release_start_slot(unit);
      trigger_units(unit->on_exit_trigger);
      return;
    }

    if (is_success) {
      transition_unit(unit, launcher::UnitState::kStopped, "on_exit",
                      "exited cleanly (code=" + std::to_string(code) + ")");
    } else {
      {
        std::lock_guard lock(state_mtx);
        unit->status.failed_reason = is_normal ? ("exit_code_" + std::to_string(code)) : "signal_crash";
      }

      transition_unit(unit, launcher::UnitState::kFailed, "on_exit",
                      "exited unexpectedly (code=" + std::to_string(code) + ")");
    }

    mark_unit_boot_terminal(unit, is_success);
    release_start_slot(unit);
    cascade_stop_bound_dependents(name);
    trigger_units(unit->on_exit_trigger);

    if (!is_success) {
      trigger_units(unit->on_error_trigger);

      if (unit->status.required) {
        request_async_stop(1);
      }
    }
  });
}

void LauncherServer::Impl::schedule_respawn(const std::shared_ptr<UnitRuntime>& unit) {
  if (!unit || stopping.load(std::memory_order_acquire) || config.disable_respawn) {
    return;
  }

  const uint64_t now = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);
  uint32_t delay_ms = unit->respawn.delay_ms;
  bool budget_exhausted = false;

  {
    std::lock_guard lock(state_mtx);

    if (unit->respawn.has_legacy_limits && unit->respawn.max_restarts > 0U) {
      while (!unit->legacy_restart_window.empty() && unit->respawn.window_ms > 0U &&
             (now - unit->legacy_restart_window.front()) > unit->respawn.window_ms) {
        unit->legacy_restart_window.pop_front();
      }

      if (unit->legacy_restart_window.size() >= unit->respawn.max_restarts) {
        unit->status.failed_reason = "max_restarts";
        budget_exhausted = true;
      } else {
        unit->legacy_restart_window.push_back(now);
      }
    }

    if (!budget_exhausted) {
      const uint32_t exp_power = std::min<uint32_t>(unit->backoff_streak, 10U);
      const double scaled = static_cast<double>(unit->respawn.delay_ms) * std::pow(unit->respawn.backoff, exp_power);
      delay_ms = static_cast<uint32_t>(std::min<double>(scaled, static_cast<double>(unit->respawn.max_delay_ms)));
      ++unit->backoff_streak;
      ++unit->status.restart_count;
    }
  }

  if (budget_exhausted) {
    transition_unit(unit, launcher::UnitState::kFailed, "on_respawn_exhausted", "max_restarts budget exceeded");
    mark_unit_boot_terminal(unit, false);
    release_start_slot(unit);
    trigger_units(unit->on_error_trigger);

    if (unit->status.required) {
      request_async_stop(1);
    }

    return;
  }

  transition_unit(unit, launcher::UnitState::kRestarting, "on_restarting",
                  "restarting in " + std::to_string(delay_ms) + "ms");

  std::weak_ptr<std::atomic_bool> weak_alive = alive_token;
  const std::string uname = unit->status.name;
  const uint64_t gen = unit->generation.load(std::memory_order_acquire);
  const int64_t rkey =
      wheel_timer->add(std::max<uint32_t>(10U, delay_ms), [this, weak_alive, uname, gen](WheelTimer::Key) {
        auto guard = weak_alive.lock();

        if (!guard || !guard->load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire)) {
          return;
        }

        (void)post_protected([this, weak_alive, uname, gen] {
          auto g2 = weak_alive.lock();

          if (!g2 || !g2->load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire)) {
            return;
          }

          auto u = find_unit(uname);

          if (!u) {
            return;
          }

          {
            std::lock_guard lock(state_mtx);

            if (u->generation.load(std::memory_order_acquire) != gen ||
                u->status.state != launcher::UnitState::kRestarting) {
              return;
            }

            u->respawn_timer_key = -1;
          }

          std::string err;

          if (!launch_unit_now(u, &err)) {
            log_manager.emit_launcher_message(uname, Logger::kError, err);
            mark_unit_boot_terminal(u, false);
            release_start_slot(u);
            trigger_units(u->on_error_trigger);

            if (u->status.required) {
              request_async_stop(1);
            }
          }
        });
      });

  bool stale = false;

  {
    std::lock_guard lock(state_mtx);

    if (unit->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire)) {
      stale = true;
    } else {
      if (unit->respawn_timer_key >= 0 && wheel_timer) {
        (void)wheel_timer->remove(unit->respawn_timer_key);
      }

      unit->respawn_timer_key = rkey;
    }
  }

  if (stale && wheel_timer) {
    (void)wheel_timer->remove(rkey);
  }
}

void LauncherServer::Impl::trigger_units(const std::vector<std::string>& names) {
  if (stopping.load(std::memory_order_acquire)) {
    return;
  }

  std::vector<std::string> targets;

  {
    std::lock_guard lock(state_mtx);
    targets = names;
  }

  if (targets.empty()) {
    return;
  }

  std::weak_ptr<std::atomic_bool> weak_alive = alive_token;

  for (const auto& target : targets) {
    for (const auto& uname : expand_target_list(target)) {
      (void)owner->post_task([this, weak_alive, uname] {
        auto guard = weak_alive.lock();

        if (!guard || !guard->load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire)) {
          return;
        }

        auto u = find_unit(uname);

        if (!u) {
          return;
        }

        {
          std::lock_guard lock(state_mtx);

          if (is_active_state(u->status.state) || u->status.state == launcher::UnitState::kInactive) {
            return;
          }
        }

        std::string err;
        (void)launch_unit_now(u, &err);
      });
    }
  }
}

void LauncherServer::Impl::arm_periodic_run_timeout(const std::shared_ptr<UnitRuntime>& unit, uint64_t gen) {
  if (!unit || unit->task_spec.timeout_ms == 0U || !wheel_timer || !wheel_timer->is_running()) {
    return;
  }

  std::weak_ptr<std::atomic_bool> weak_alive = alive_token;
  const std::string uname = unit->status.name;
  const int64_t tkey = wheel_timer->add(unit->task_spec.timeout_ms, [this, weak_alive, uname, gen](WheelTimer::Key) {
    auto g3 = weak_alive.lock();

    if (!g3 || !g3->load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire)) {
      return;
    }

    (void)post_protected([this, weak_alive, uname, gen] {
      auto g4 = weak_alive.lock();

      if (!g4 || !g4->load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire)) {
        return;
      }

      auto tu = find_unit(uname);

      if (!tu || tu->generation.load(std::memory_order_acquire) != gen) {
        return;
      }

      if (process_supervisor.is_running(uname)) {
        log_manager.emit_launcher_message(uname, Logger::kWarn, "Periodic task run timed out; terminating.");
        (void)process_supervisor.stop_process(uname, 500U, gen);
      }
    });
  });

  bool stale = false;

  {
    std::lock_guard lock(state_mtx);

    if (unit->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire)) {
      stale = true;
    } else {
      if (unit->ready_timer_key >= 0 && wheel_timer) {
        (void)wheel_timer->remove(unit->ready_timer_key);
      }

      unit->ready_timer_key = tkey;
    }
  }

  if (stale && wheel_timer) {
    (void)wheel_timer->remove(tkey);
  }
}

void LauncherServer::Impl::schedule_periodic_task(const std::shared_ptr<UnitRuntime>& unit, uint64_t gen) {
  if (!unit || !wheel_timer || !wheel_timer->is_running()) {
    return;
  }

  std::weak_ptr<std::atomic_bool> weak_alive = alive_token;
  const std::string uname = unit->status.name;
  const uint32_t interval = std::max<uint32_t>(50U, unit->task_spec.interval_ms);
  const int64_t key = wheel_timer->add(
      interval,
      [this, weak_alive, uname, gen](WheelTimer::Key k) {
        auto guard = weak_alive.lock();

        if (!guard || !guard->load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire)) {
          if (wheel_timer) {
            (void)wheel_timer->remove(k);
          }

          return;
        }

        (void)owner->post_task([this, weak_alive, uname, gen, k] {
          auto g2 = weak_alive.lock();

          if (!g2 || !g2->load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire)) {
            if (wheel_timer) {
              (void)wheel_timer->remove(k);
            }

            return;
          }

          auto u = find_unit(uname);

          if (!u || u->generation.load(std::memory_order_acquire) != gen) {
            if (wheel_timer) {
              (void)wheel_timer->remove(k);
            }

            return;
          }

          if (process_supervisor.is_running(uname)) {
            return;
          }

          launcher::ManagedProcessLaunch launch;
          launch.name = uname;
          launch.generation = gen;
          launch.program = u->task_spec.program;
          launch.args = u->task_spec.args;
          launch.launch_prefix = u->task_spec.launch_prefix;
          launch.cwd = u->task_spec.cwd;
          launch.env = u->task_spec.env;
          launch.env["VLINK_LAUNCHER_SECURITY_KEY"] = "";

          if (active_record_enabled && launch.env.count("VLINK_BAG_PATH") == 0U) {
            launch.env["VLINK_BAG_PATH"] = "";
          }

          launcher::inject_url_env(u->task_spec.urls, u->task_spec.inputs, u->task_spec.outputs, u->task_spec.ns,
                                   u->task_spec.remaps, launch.env);
          launch.inherit_env = u->task_spec.inherit_env;
          launch.output = u->task_spec.output;
          launch.remaps = u->task_spec.remaps;
          launch.resources = u->resources;
          launch.kill_mode = u->kill_mode;
          launch.stop_signal = u->stop_signal;
          launch.sigterm_timeout_ms = u->sigterm_timeout_ms;
          int64_t pid = -1;
          std::string cg_path;

          if (!process_supervisor.start_process(launch, &pid, &cg_path, nullptr)) {
            return;
          }

          bool stale_after_start = false;

          {
            std::lock_guard lock(state_mtx);

            if (u->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire)) {
              stale_after_start = true;
            } else {
              u->status.pid = pid;
              u->status.cgroup_path = cg_path;

              if (u->ready_timer_key >= 0 && wheel_timer) {
                (void)wheel_timer->remove(u->ready_timer_key);
                u->ready_timer_key = -1;
              }
            }
          }

          if (stale_after_start) {
            (void)process_supervisor.stop_process(uname, 500U, gen);
            return;
          }

          arm_periodic_run_timeout(u, gen);
        });
      },
      interval);

  bool stale = false;

  {
    std::lock_guard lock(state_mtx);

    if (unit->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire)) {
      stale = true;
    } else {
      if (unit->periodic_timer_key >= 0 && wheel_timer) {
        (void)wheel_timer->remove(unit->periodic_timer_key);
      }

      unit->periodic_timer_key = key;
    }
  }

  if (stale && wheel_timer) {
    (void)wheel_timer->remove(key);
  }
}

void LauncherServer::Impl::run_housekeeping() {
  if (stopping.load(std::memory_order_acquire)) {
    return;
  }

  cached_host_cpu.store(Utils::get_cpu_usage(), std::memory_order_relaxed);
  cached_host_mem.store(Utils::get_memory_usage(), std::memory_order_relaxed);

  const uint64_t now = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);
  std::vector<std::shared_ptr<UnitRuntime>> all_units;

  {
    std::lock_guard lock(state_mtx);
    all_units.reserve(units.size());

    for (const auto& name : unit_order) {
      all_units.push_back(units[name]);
    }
  }

  for (const auto& u : all_units) {
    launcher::UnitState st = launcher::UnitState::kPending;
    uint64_t captured_gen = 0U;

    {
      std::lock_guard lock(state_mtx);
      st = u->status.state;
      captured_gen = u->generation.load(std::memory_order_acquire);

      if (st == launcher::UnitState::kReady && u->backoff_streak > 0U && u->ready_since_steady_ms > 0U) {
        const uint64_t stable_ms = (u->respawn.window_ms > 0U) ? static_cast<uint64_t>(u->respawn.window_ms) : 60000ULL;

        if (now >= u->ready_since_steady_ms && (now - u->ready_since_steady_ms) >= stable_ms) {
          u->backoff_streak = 0U;
        }
      }
    }

    if (st != launcher::UnitState::kReady && st != launcher::UnitState::kRunning) {
      continue;
    }

    if (u->status.type == launcher::UnitType::kComponent) {
      const double cpu = component_host.get_cpu_usage(u->status.name);

      {
        std::lock_guard lock(state_mtx);
        u->status.cpu_usage = cpu;
      }

      if (u->watchdog_ms > 0U && st == launcher::UnitState::kReady) {
        const uint64_t last_ping = component_host.get_last_watchdog_ms(u->status.name);
        uint64_t armed_ms = 0U;

        {
          std::lock_guard lock(state_mtx);
          armed_ms = u->watchdog_armed_ms;
        }

        const uint64_t ref = std::max(last_ping, armed_ms);

        if (ref > 0U && now > ref && (now - ref) > u->watchdog_ms) {
          bool claimed_timeout = false;

          {
            std::lock_guard lock(state_mtx);

            if (u->generation.load(std::memory_order_acquire) == captured_gen &&
                u->status.state == launcher::UnitState::kReady) {
              u->generation.fetch_add(1U, std::memory_order_acq_rel);
              u->status.state = launcher::UnitState::kStopping;
              u->ready_since_steady_ms = 0U;
              u->active_record_path.clear();
              u->status.healthy = false;
              ++u->status.liveness_failures;
              u->status.failed_reason = "watchdog";
              claimed_timeout = true;
            }
          }

          if (!claimed_timeout) {
            continue;
          }

          cancel_unit_timers(u);
          log_manager.emit_launcher_message(u->status.name, Logger::kError, "Component watchdog timeout exceeded.");
          (void)component_host.stop_component(u->status.name);
          cascade_stop_bound_dependents(u->status.name);

          if (!config.disable_respawn && u->respawn.enabled) {
            schedule_respawn(u);
          } else {
            transition_unit(u, launcher::UnitState::kFailed, "on_watchdog_timeout", "watchdog timeout");
            trigger_units(u->on_error_trigger);

            if (u->status.required) {
              request_async_stop(1);
            }
          }

          continue;
        }
      }
    } else {
      const auto metrics = process_supervisor.sample_metrics(u->status.name);
      std::lock_guard lock(state_mtx);
      u->status.memory_rss_kb = metrics.memory_rss_kb;
      u->status.cgroup_memory_kb = metrics.cgroup_memory_kb;
      u->status.thread_count = metrics.thread_count;
      u->status.cpu_usage = metrics.cpu_usage;
    }

    if (u->liveness.enabled && st == launcher::UnitState::kReady &&
        now >= u->last_liveness_check_ms + u->liveness.interval_ms) {
      u->last_liveness_check_ms = now;
      bool alive = true;

      if (u->liveness.type == launcher::ReadyProbeType::kFile) {
        std::error_code ec;
        alive = std::filesystem::exists(u->liveness.path, ec);
      } else if (u->liveness.type == launcher::ReadyProbeType::kTopic) {
        (void)ensure_discovery_viewer();
        alive = has_matching_topic(u->liveness.topic);
      }

      uint32_t fails = 0U;
      bool still_current = false;

      {
        std::lock_guard lock(state_mtx);
        still_current = (u->generation.load(std::memory_order_acquire) == captured_gen &&
                         u->status.state == launcher::UnitState::kReady);

        if (still_current) {
          if (alive) {
            u->status.liveness_failures = 0U;
            u->status.healthy = true;
          } else {
            ++u->status.liveness_failures;
            fails = u->status.liveness_failures;
            u->status.healthy = false;
          }
        }
      }

      if (still_current && !alive && fails >= u->liveness.failure_threshold) {
        log_manager.emit_launcher_message(u->status.name, Logger::kError,
                                          "Liveness probe failed " + std::to_string(fails) + " times.");

        if (u->liveness.restart_on_failure && !config.disable_respawn) {
          cascade_stop_bound_dependents(u->status.name);
          (void)stop_unit_internal(u);
          schedule_respawn(u);
        }
      }
    }
  }

  publish_heartbeat();
  publish_snapshot();
}

std::vector<std::vector<std::string>> LauncherServer::Impl::compute_reverse_topological_layers() const {
  std::lock_guard lock(state_mtx);
  std::vector<std::vector<std::string>> layers;

  std::set<int32_t> stage_set;

  for (const auto& name : unit_order) {
    stage_set.insert(units.at(name)->status.stage);
  }

  std::vector<int32_t> desc_stages(stage_set.begin(), stage_set.end());
  std::reverse(desc_stages.begin(), desc_stages.end());

  for (const int32_t cur_stage : desc_stages) {
    std::vector<std::string> stage_units;

    for (const auto& name : unit_order) {
      if (units.at(name)->status.stage == cur_stage) {
        stage_units.push_back(name);
      }
    }

    std::unordered_map<std::string, size_t> out_degree;
    std::unordered_map<std::string, std::vector<std::string>> rev_edges;

    for (const auto& name : stage_units) {
      out_degree[name] = 0U;
    }

    for (const auto& name : stage_units) {
      const auto& u = units.at(name);

      for (const auto& pred : u->order_preds) {
        if (out_degree.count(pred) != 0U) {
          ++out_degree[pred];
          rev_edges[name].push_back(pred);
        }
      }
    }

    std::unordered_set<std::string> emitted;

    while (emitted.size() < stage_units.size()) {
      std::vector<std::string> layer;

      for (size_t idx = stage_units.size(); idx > 0U; --idx) {
        const auto& name = stage_units[idx - 1U];

        if (emitted.count(name) == 0U && out_degree[name] == 0U) {
          layer.push_back(name);
        }
      }

      if (layer.empty()) {
        for (size_t idx = stage_units.size(); idx > 0U; --idx) {
          const auto& name = stage_units[idx - 1U];

          if (emitted.count(name) == 0U) {
            layer.push_back(name);
          }
        }
      }

      for (const auto& n : layer) {
        emitted.insert(n);

        for (const auto& pred : rev_edges[n]) {
          if (out_degree[pred] > 0U) {
            --out_degree[pred];
          }
        }
      }

      layers.push_back(std::move(layer));
    }
  }

  return layers;
}

void LauncherServer::Impl::begin_cold_boot() {
  std::vector<std::shared_ptr<UnitRuntime>> roots;

  {
    std::lock_guard lock(state_mtx);
    pending_boot_units = unit_order.size();
    cold_boot_complete = unit_order.empty();
    stage_remaining.clear();
    stage_units.clear();
    active_stage_idx = 0U;

    const bool has_stages = !sorted_stages.empty();
    const int32_t first_stage = has_stages ? sorted_stages[0] : 0;

    for (const auto& name : unit_order) {
      auto& u = units[name];
      u->remaining_boot_preds = u->order_preds.size();
      u->boot_participant = true;
      u->boot_failed = false;
      u->boot_terminal = false;
      u->boot_enqueued = false;
      ++stage_remaining[u->status.stage];
      stage_units[u->status.stage].push_back(name);

      if (u->remaining_boot_preds == 0U && (!has_stages || u->status.stage == first_stage)) {
        u->boot_enqueued = true;
        roots.push_back(u);
      }
    }
  }

  if (roots.empty()) {
    if (cold_boot_complete) {
      boot_duration_ms.store(0U, std::memory_order_release);
    } else {
      log_manager.emit_launcher_message("launcher", Logger::kError,
                                        "Cold boot deadlock: no runnable root units found for remaining DAG.");
      std::lock_guard lock(state_mtx);
      cold_boot_complete = true;
    }

    return;
  }

  for (const auto& u : roots) {
    enqueue_ready_unit(u);
  }
}

bool LauncherServer::Impl::init_control_plane() {
  const bool ctrl_enabled = config.enable_control_plane && manifest.control_enabled;
  const bool logd_enabled = log_manager.is_builtin_logd();

  if (!ctrl_enabled && !logd_enabled) {
    return true;
  }

  active_ctrl_scheme =
      launcher::resolve_scheme(!config.control_scheme.empty() ? config.control_scheme : manifest.control_scheme);

  if (!config.control_namespace.empty()) {
    active_ctrl_ns = config.control_namespace;
  } else if (!manifest.control_namespace.empty()) {
    active_ctrl_ns = manifest.control_namespace;
  } else {
    active_ctrl_ns = "launcher";
  }

  active_ctrl_domain =
      (config.control_domain_id >= 0) ? static_cast<uint32_t>(config.control_domain_id) : manifest.control_domain;
  active_security_key = !config.security_key.empty() ? config.security_key : manifest.security_key;

  if (active_security_key.empty()) {
    active_security_key = Utils::get_env("VLINK_LAUNCHER_SECURITY_KEY");
  }

  active_bind_ip = !config.bind_ip.empty() ? config.bind_ip : manifest.bind_ip;
  active_native_mode = config.native_mode || manifest.native_mode;
  use_security = !active_security_key.empty();

  if (use_security && active_ctrl_scheme == "intra") {
    VLOG_E("LauncherServer: Security key is not supported on intra://; cannot initialize control plane.");
    return false;
  }

#ifndef VLINK_ENABLE_SECURITY
  if (use_security) {
    VLOG_E("LauncherServer: Security key configured, but VLink was built without ENABLE_SECURITY.");
    return false;
  }
#endif

  std::weak_ptr<std::atomic_bool> weak_alive = alive_token;
  auto logd_handler = [this, weak_alive](const launcher::LogRecord& rec) {
    auto guard = weak_alive.lock();

    if VUNLIKELY (!guard || !guard->load(std::memory_order_acquire)) {
      return;
    }

    bool from_container = false;
    bool record_to_history = true;

    if (!rec.unit_name.empty()) {
      auto u = find_unit(rec.unit_name);

      if (u) {
        from_container = (u->status.type == launcher::UnitType::kContainer);

        auto child_console_muted = [&](const std::unordered_map<std::string, std::string>& env) -> bool {
          Logger::Level eff_lv = effective_logger_cfg.console_level;
          auto eit = env.find("VLINK_LOG_CONSOLE_LEVEL");

          if (eit != env.end() && !eit->second.empty()) {
            bool ok = false;
            const auto parsed = launcher::parse_log_level_strict(eit->second, ok);

            if (ok) {
              eff_lv = parsed;
            }
          }

          return static_cast<uint8_t>(rec.level) < static_cast<uint8_t>(eff_lv) || eff_lv >= Logger::kOff;
        };

        if (u->status.type == launcher::UnitType::kProcess) {
          record_to_history =
              (u->proc_spec.output == launcher::OutputMode::kNone) || child_console_muted(u->proc_spec.env);
        } else if (u->status.type == launcher::UnitType::kProxy) {
          record_to_history =
              (u->proxy_spec.output == launcher::OutputMode::kNone) || child_console_muted(u->proxy_spec.env);
        } else if (u->status.type == launcher::UnitType::kTask) {
          record_to_history =
              (u->task_spec.output == launcher::OutputMode::kNone) || child_console_muted(u->task_spec.env);
        }
      }
    }

    log_manager.ingest_logd_record(rec, from_container, record_to_history);
  };

  std::unique_ptr<MultiLoop> new_rpc_loop;
  std::shared_ptr<PlainServer> new_ctrl_server;
  std::shared_ptr<PlainSetter> new_state_setter;
  std::shared_ptr<PlainEventPub> new_event_pub;
  std::shared_ptr<PlainLogPub> new_log_pub;
  std::shared_ptr<PlainLogdSub> new_logd_sub;
  std::shared_ptr<PlainHbPub> new_hb_pub;

#ifdef VLINK_ENABLE_SECURITY
  std::shared_ptr<SecServer> new_sec_ctrl_server;
  std::shared_ptr<SecSetter> new_sec_state_setter;
  std::shared_ptr<SecEventPub> new_sec_event_pub;
  std::shared_ptr<SecLogPub> new_sec_log_pub;
  std::shared_ptr<SecLogdSub> new_sec_logd_sub;
  std::shared_ptr<SecHbPub> new_sec_hb_pub;
#endif

  auto cleanup_new_nodes = [&]() {
    new_logd_sub.reset();
    new_ctrl_server.reset();
    new_state_setter.reset();
    new_event_pub.reset();
    new_log_pub.reset();
    new_hb_pub.reset();
#ifdef VLINK_ENABLE_SECURITY
    new_sec_logd_sub.reset();
    new_sec_ctrl_server.reset();
    new_sec_state_setter.reset();
    new_sec_event_pub.reset();
    new_sec_log_pub.reset();
    new_sec_hb_pub.reset();
#endif

    if (new_rpc_loop) {
      new_rpc_loop->quit(true);
      (void)new_rpc_loop->wait_for_quit(1000);
      new_rpc_loop.reset();
    }
  };

  try {
    if (ctrl_enabled) {
      const std::string ctrl_url =
          launcher::build_channel_url(active_ctrl_scheme, active_ctrl_ns, "ctrl", "method", active_ctrl_domain);
      const std::string state_url =
          launcher::build_channel_url(active_ctrl_scheme, active_ctrl_ns, "state", "field", active_ctrl_domain);
      const std::string event_url =
          launcher::build_channel_url(active_ctrl_scheme, active_ctrl_ns, "events", "event", active_ctrl_domain);
      const std::string log_url =
          launcher::build_channel_url(active_ctrl_scheme, active_ctrl_ns, "logs", "log", active_ctrl_domain);
      const std::string hb_url =
          launcher::build_channel_url(active_ctrl_scheme, active_ctrl_ns, "hb", "clock", active_ctrl_domain);

      auto cmd_handler = [this](const launcher::CommandRequest& req, launcher::CommandResponse& resp) {
        resp = handle_command(req);
      };

      new_rpc_loop = std::make_unique<MultiLoop>(4U);
      new_rpc_loop->set_name("LauncherRpc");
      new_rpc_loop->async_run();

      bool ctrl_listen_ok = false;

      if (use_security) {
#ifdef VLINK_ENABLE_SECURITY
        Security::Config sec_cfg;
        sec_cfg.key = active_security_key;

        new_sec_ctrl_server =
            make_server_node<SecServer>(active_ctrl_scheme, active_bind_ip, active_native_mode, ctrl_url, sec_cfg);
        (void)new_sec_ctrl_server->attach(new_rpc_loop.get());
        ctrl_listen_ok = new_sec_ctrl_server->listen(std::move(cmd_handler));
        new_sec_state_setter =
            make_server_node<SecSetter>(active_ctrl_scheme, active_bind_ip, active_native_mode, state_url, sec_cfg);
        new_sec_event_pub =
            make_server_node<SecEventPub>(active_ctrl_scheme, active_bind_ip, active_native_mode, event_url, sec_cfg);

        if (config.publish_logs) {
          new_sec_log_pub =
              make_server_node<SecLogPub>(active_ctrl_scheme, active_bind_ip, active_native_mode, log_url, sec_cfg);
        }

        new_sec_hb_pub =
            make_server_node<SecHbPub>(active_ctrl_scheme, active_bind_ip, active_native_mode, hb_url, sec_cfg);
#endif
      } else {
        new_ctrl_server =
            make_server_node<PlainServer>(active_ctrl_scheme, active_bind_ip, active_native_mode, ctrl_url);
        (void)new_ctrl_server->attach(new_rpc_loop.get());
        ctrl_listen_ok = new_ctrl_server->listen(std::move(cmd_handler));
        new_state_setter =
            make_server_node<PlainSetter>(active_ctrl_scheme, active_bind_ip, active_native_mode, state_url);
        new_event_pub =
            make_server_node<PlainEventPub>(active_ctrl_scheme, active_bind_ip, active_native_mode, event_url);

        if (config.publish_logs) {
          new_log_pub = make_server_node<PlainLogPub>(active_ctrl_scheme, active_bind_ip, active_native_mode, log_url);
        }

        new_hb_pub = make_server_node<PlainHbPub>(active_ctrl_scheme, active_bind_ip, active_native_mode, hb_url);
      }

      if VUNLIKELY (!ctrl_listen_ok) {
        cleanup_new_nodes();
        VLOG_E("LauncherServer: Failed to listen on control channel '", ctrl_url, "'.");
        return false;
      }
    }

    if (logd_enabled) {
      const std::string logd_url =
          launcher::build_channel_url(active_ctrl_scheme, active_ctrl_ns, "logd", "log", active_ctrl_domain);
      const std::string logd_key = use_security ? Uuid::random_hex() : std::string();
      bool listen_ok = false;

      if (use_security) {
#ifdef VLINK_ENABLE_SECURITY
        Security::Config logd_sec_cfg;
        logd_sec_cfg.key = logd_key;
        new_sec_logd_sub = make_server_node<SecLogdSub>(active_ctrl_scheme, active_bind_ip, active_native_mode,
                                                        logd_url, logd_sec_cfg);
        listen_ok = new_sec_logd_sub->listen(std::move(logd_handler));
#endif
      } else {
        new_logd_sub = make_server_node<PlainLogdSub>(active_ctrl_scheme, active_bind_ip, active_native_mode, logd_url);
        listen_ok = new_logd_sub->listen(std::move(logd_handler));
      }

      if VUNLIKELY (!listen_ok) {
        cleanup_new_nodes();
        VLOG_E("LauncherServer: Failed to listen on built-in logd channel '", logd_url, "'.");
        return false;
      }

      log_manager.set_logd_endpoint(logd_url, active_ctrl_scheme, active_bind_ip, active_native_mode, logd_key);
    }
  } catch (const std::exception& e) {
    cleanup_new_nodes();
    VLOG_E("LauncherServer: Failed to initialize control plane: ", e.what(), ".");
    return false;
  }

  {
    std::lock_guard lock(ctrl_mtx);
    ctrl_rpc_loop = std::move(new_rpc_loop);
    ctrl_server = std::move(new_ctrl_server);
    state_setter = std::move(new_state_setter);
    event_pub = std::move(new_event_pub);
    log_pub = std::move(new_log_pub);
    logd_sub = std::move(new_logd_sub);
    hb_pub = std::move(new_hb_pub);
#ifdef VLINK_ENABLE_SECURITY
    sec_ctrl_server = std::move(new_sec_ctrl_server);
    sec_state_setter = std::move(new_sec_state_setter);
    sec_event_pub = std::move(new_sec_event_pub);
    sec_log_pub = std::move(new_sec_log_pub);
    sec_logd_sub = std::move(new_sec_logd_sub);
    sec_hb_pub = std::move(new_sec_hb_pub);
#endif
  }

  if (ctrl_enabled) {
    control_active.store(true, std::memory_order_release);
  }

  return true;
}

void LauncherServer::Impl::shutdown_control_plane() {
  control_active.store(false, std::memory_order_release);

  std::unique_ptr<MultiLoop> rpc_loop;
  std::shared_ptr<PlainServer> p_srv;
  std::shared_ptr<PlainSetter> p_set;
  std::shared_ptr<PlainEventPub> p_ev;
  std::shared_ptr<PlainLogPub> p_log;
  std::shared_ptr<PlainHbPub> p_hb;

#ifdef VLINK_ENABLE_SECURITY
  std::shared_ptr<SecServer> s_srv;
  std::shared_ptr<SecSetter> s_set;
  std::shared_ptr<SecEventPub> s_ev;
  std::shared_ptr<SecLogPub> s_log;
  std::shared_ptr<SecHbPub> s_hb;
#endif

  {
    std::lock_guard lock(ctrl_mtx);
    rpc_loop.swap(ctrl_rpc_loop);
    p_srv.swap(ctrl_server);
    p_set.swap(state_setter);
    p_ev.swap(event_pub);
    p_log.swap(log_pub);
    p_hb.swap(hb_pub);
#ifdef VLINK_ENABLE_SECURITY
    s_srv.swap(sec_ctrl_server);
    s_set.swap(sec_state_setter);
    s_ev.swap(sec_event_pub);
    s_log.swap(sec_log_pub);
    s_hb.swap(sec_hb_pub);
#endif
  }

  if (rpc_loop && !rpc_loop->is_in_same_thread()) {
    rpc_loop->quit(false);
    (void)rpc_loop->wait_for_quit(1000);
  }

  p_hb.reset();
  p_log.reset();
  p_ev.reset();
  p_set.reset();
  p_srv.reset();

#ifdef VLINK_ENABLE_SECURITY
  s_hb.reset();
  s_log.reset();
  s_ev.reset();
  s_set.reset();
  s_srv.reset();
#endif

  if (rpc_loop) {
    rpc_loop->quit(true);

    if (!rpc_loop->is_in_same_thread()) {
      (void)rpc_loop->wait_for_quit(1000);
    }
  }
}

void LauncherServer::Impl::shutdown_logd_collector() {
  std::shared_ptr<PlainLogdSub> p_logd;
#ifdef VLINK_ENABLE_SECURITY
  std::shared_ptr<SecLogdSub> s_logd;
#endif

  {
    std::lock_guard lock(ctrl_mtx);
    p_logd.swap(logd_sub);
#ifdef VLINK_ENABLE_SECURITY
    s_logd.swap(sec_logd_sub);
#endif
  }

  p_logd.reset();
#ifdef VLINK_ENABLE_SECURITY
  s_logd.reset();
#endif
}

void LauncherServer::Impl::schedule_snapshot_publish() {
  if (!control_active.load(std::memory_order_acquire)) {
    return;
  }

  snapshot_dirty.store(true, std::memory_order_release);

  if (!wheel_timer || !wheel_timer->is_running()) {
    if (snapshot_dirty.exchange(false, std::memory_order_acq_rel)) {
      publish_snapshot();
    }

    return;
  }

  if (snapshot_flush_pending.exchange(true, std::memory_order_acq_rel)) {
    return;
  }

  std::weak_ptr<std::atomic_bool> weak_alive = alive_token;
  (void)wheel_timer->add(50U, [this, weak_alive](WheelTimer::Key) {
    auto guard = weak_alive.lock();

    if (!guard || !guard->load(std::memory_order_acquire)) {
      return;
    }

    (void)post_protected([this, weak_alive] {
      auto g2 = weak_alive.lock();

      if (!g2 || !g2->load(std::memory_order_acquire)) {
        return;
      }

      snapshot_flush_pending.store(false, std::memory_order_release);

      if (snapshot_dirty.exchange(false, std::memory_order_acq_rel)) {
        publish_snapshot();
      }

      if (snapshot_dirty.load(std::memory_order_acquire)) {
        schedule_snapshot_publish();
      }
    });
  });
}

void LauncherServer::Impl::publish_snapshot() {
  snapshot_dirty.store(false, std::memory_order_release);

  if (!control_active.load(std::memory_order_acquire)) {
    return;
  }

  const auto snap = build_snapshot();
  std::shared_ptr<PlainSetter> p_set;
#ifdef VLINK_ENABLE_SECURITY
  std::shared_ptr<SecSetter> s_set;
#endif

  {
    std::lock_guard lock(ctrl_mtx);
    p_set = state_setter;
#ifdef VLINK_ENABLE_SECURITY
    s_set = sec_state_setter;
#endif
  }

  if (use_security) {
#ifdef VLINK_ENABLE_SECURITY
    if (s_set) {
      s_set->set(snap);
    }
#endif
  } else if (p_set) {
    p_set->set(snap);
  }
}

void LauncherServer::Impl::publish_event(const launcher::EventRecord& ev) {
  std::weak_ptr<std::atomic_bool> weak_alive = alive_token;

  if VLIKELY (owner->is_running()) {
    (void)owner->post_task([this, weak_alive, ev] {
      auto guard = weak_alive.lock();

      if VUNLIKELY (!guard || !guard->load(std::memory_order_acquire)) {
        return;
      }

      std::shared_ptr<EventCallback> cb;

      {
        std::lock_guard lock(cb_mtx);
        cb = local_event_cb;
      }

      if (cb && *cb) {
        (*cb)(ev);
      }
    });
  }

  if VUNLIKELY (!control_active.load(std::memory_order_acquire)) {
    return;
  }

  std::shared_ptr<PlainEventPub> p_ev;
#ifdef VLINK_ENABLE_SECURITY
  std::shared_ptr<SecEventPub> s_ev;
#endif

  {
    std::lock_guard lock(ctrl_mtx);
    p_ev = event_pub;
#ifdef VLINK_ENABLE_SECURITY
    s_ev = sec_event_pub;
#endif
  }

  if (use_security) {
#ifdef VLINK_ENABLE_SECURITY
    if (s_ev) {
      (void)s_ev->publish(ev);
    }
#endif
  } else if (p_ev) {
    (void)p_ev->publish(ev);
  }
}

void LauncherServer::Impl::publish_log(const launcher::LogRecord& rec) {
  static thread_local bool in_pub_log = false;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

  if VUNLIKELY (in_pub_log) {
    return;
  }

  struct PubLogGuard final {
    bool& flag;

    explicit PubLogGuard(bool& f) noexcept : flag(f) { flag = true; }

    ~PubLogGuard() noexcept { flag = false; }
  };

  PubLogGuard log_guard(in_pub_log);
  std::weak_ptr<std::atomic_bool> weak_alive = alive_token;

  std::shared_ptr<LogCallback> local_cb;

  {
    std::lock_guard lock(cb_mtx);
    local_cb = local_log_cb;
  }

  if (local_cb && *local_cb && owner->is_running()) {
    (void)owner->post_task([weak_alive, cb = std::move(local_cb), rec] {
      auto guard = weak_alive.lock();

      if VUNLIKELY (!guard || !guard->load(std::memory_order_acquire)) {
        return;
      }

      (*cb)(rec);
    });
  }

  if VLIKELY (control_active.load(std::memory_order_acquire) && config.publish_logs) {
    std::shared_ptr<PlainLogPub> p_log;
#ifdef VLINK_ENABLE_SECURITY
    std::shared_ptr<SecLogPub> s_log;
#endif

    {
      std::lock_guard lock(ctrl_mtx);
      p_log = log_pub;
#ifdef VLINK_ENABLE_SECURITY
      s_log = sec_log_pub;
#endif
    }

    if (use_security) {
#ifdef VLINK_ENABLE_SECURITY
      if (s_log) {
        (void)s_log->publish(rec);
      }
#endif
    } else if (p_log) {
      (void)p_log->publish(rec);
    }
  }
}

void LauncherServer::Impl::publish_heartbeat() {
  if (!control_active.load(std::memory_order_acquire)) {
    return;
  }

  launcher::HeartbeatMsg hb;
  hb.timestamp_ms = ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMilli);
  const uint64_t now_s = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);

  {
    std::lock_guard lock(state_mtx);
    hb.session_id = session_id;
    hb.uptime_ms =
        (server_start_steady_ms > 0U && now_s >= server_start_steady_ms) ? (now_s - server_start_steady_ms) : 0U;
    hb.unit_count = static_cast<uint32_t>(units.size());

    for (const auto& [_, u] : units) {
      if (u->status.state == launcher::UnitState::kReady) {
        ++hb.ready_count;
      } else if (u->status.state == launcher::UnitState::kFailed) {
        ++hb.failed_count;
      }
    }
  }

  std::shared_ptr<PlainHbPub> p_hb;
#ifdef VLINK_ENABLE_SECURITY
  std::shared_ptr<SecHbPub> s_hb;
#endif

  {
    std::lock_guard lock(ctrl_mtx);
    p_hb = hb_pub;
#ifdef VLINK_ENABLE_SECURITY
    s_hb = sec_hb_pub;
#endif
  }

  if (use_security) {
#ifdef VLINK_ENABLE_SECURITY
    if (s_hb) {
      (void)s_hb->publish(hb);
    }
#endif
  } else if (p_hb) {
    (void)p_hb->publish(hb);
  }
}

launcher::SystemSnapshot LauncherServer::Impl::build_snapshot() const {
  launcher::SystemSnapshot snap;
  snap.version = VLINK_VERSION;
  snap.host = Utils::get_host_name();
  snap.server_pid = Utils::get_pid();
  snap.timestamp_ms = ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMilli);
  const uint64_t now_s = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);
  snap.boot_duration_ms = boot_duration_ms.load(std::memory_order_acquire);
  double host_cpu = cached_host_cpu.load(std::memory_order_relaxed);
  double host_mem = cached_host_mem.load(std::memory_order_relaxed);

  if VUNLIKELY (host_cpu < 0.0 || host_mem < 0.0) {
    host_cpu = Utils::get_cpu_usage();
    host_mem = Utils::get_memory_usage();
    cached_host_cpu.store(host_cpu, std::memory_order_relaxed);
    cached_host_mem.store(host_mem, std::memory_order_relaxed);
  }

  snap.cpu_usage = host_cpu;
  snap.memory_usage = host_mem;
  snap.log_dir = log_manager.session_dir();

  std::vector<std::pair<std::string, std::shared_ptr<LauncherAPI>>> container_clients;

  {
    std::lock_guard lock(state_mtx);
    snap.session_id = session_id;
    snap.config_file = manifest_source_file;
    snap.uptime_ms =
        (server_start_steady_ms > 0U && now_s >= server_start_steady_ms) ? (now_s - server_start_steady_ms) : 0U;

    if (effective_startup_mode == launcher::StartupMode::kSerial) {
      snap.startup_mode = "serial";
    } else if (effective_startup_mode == launcher::StartupMode::kMaxParallel) {
      snap.startup_mode = "max_parallel";
    } else {
      snap.startup_mode = "parallel";
    }

    snap.max_parallel = effective_max_parallel;
    snap.units.reserve(unit_order.size());

    for (const auto& name : unit_order) {
      auto it = units.find(name);

      if (it == units.end()) {
        continue;
      }

      auto st = it->second->status;

      if (is_active_state(st.state) && st.start_time_ms > 0U && snap.timestamp_ms >= st.start_time_ms) {
        st.uptime_ms = snap.timestamp_ms - st.start_time_ms;
      }

      if (st.type == launcher::UnitType::kContainer && it->second->container_client) {
        container_clients.emplace_back(st.name, it->second->container_client);
      }

      snap.units.push_back(std::move(st));
    }
  }

  for (const auto& [cname, cli] : container_clients) {
    auto csnap = cli->get_cached_snapshot();

    if (csnap.has_value()) {
      for (auto& u : csnap->units) {
        u.container = cname;
        snap.units.push_back(std::move(u));
      }
    }
  }

  return snap;
}

launcher::CommandResponse LauncherServer::Impl::handle_command(const launcher::CommandRequest& req) {
  launcher::CommandResponse resp;

  if (req.proto_version != launcher::kProtocolVersion) {
    resp.result.code = launcher::ErrorCode::kVersionMismatch;
    resp.result.message = "Protocol version mismatch.";
    return resp;
  }

  if (stopping.load(std::memory_order_acquire) && req.op != launcher::CommandOp::kGetStatus &&
      req.op != launcher::CommandOp::kQueryLogs) {
    resp.result.code = launcher::ErrorCode::kNotRunning;
    resp.result.message = "LauncherServer is shutting down.";
    return resp;
  }

  switch (req.op) {
    case launcher::CommandOp::kGetStatus: {
      resp.result.code = launcher::ErrorCode::kOk;
      resp.result.snapshot = build_snapshot();
      resp.has_snapshot = true;
      break;
    }
    case launcher::CommandOp::kStartUnit:
      resp.result = owner->start_unit(req.target);
      break;
    case launcher::CommandOp::kStopUnit:
      resp.result = owner->stop_unit(req.target);
      break;
    case launcher::CommandOp::kRestartUnit:
      resp.result = owner->restart_unit(req.target);
      break;
    case launcher::CommandOp::kReloadUnit:
      resp.result = owner->reload_unit(req.target);
      break;
    case launcher::CommandOp::kActivateUnit:
      resp.result = owner->activate_unit(req.target);
      break;
    case launcher::CommandOp::kDeactivateUnit:
      resp.result = owner->deactivate_unit(req.target);
      break;
    case launcher::CommandOp::kResetFailed:
      resp.result = owner->reset_failed(req.target);
      break;
    case launcher::CommandOp::kIsolateGroup:
      resp.result = owner->isolate_group(req.target);
      break;
    case launcher::CommandOp::kGetUnitConfig:
      resp.result = owner->get_unit_config(req.target);
      break;
    case launcher::CommandOp::kQueryLogs:
      resp.result = owner->query_logs(req.log_query);
      break;
    case launcher::CommandOp::kLoadComponent: {
      const bool is_dds_scheme = launcher::is_dds_scheme_name(active_ctrl_scheme);
      const bool loopback_bound = is_dds_scheme && (active_bind_ip == "127.0.0.1" || active_bind_ip == "::1");
      const bool allowed = use_security || active_ctrl_scheme == "intra" || loopback_bound ||
                           config.allow_remote_load || manifest.allow_remote_load;

      if VUNLIKELY (!allowed) {
        resp.result.code = launcher::ErrorCode::kUnauthorized;
        resp.result.message =
            "Remote dynamic component loading requires security_key, intra://, explicit DDS loopback bind_ip, or "
            "allow_remote_load=true.";
        break;
      }

      resp.result = owner->load_component(req.load_req);
      break;
    }
    case launcher::CommandOp::kUnloadComponent:
      resp.result = owner->unload_component(req.target);
      break;
    case launcher::CommandOp::kGetParams:
      resp.result = owner->get_params(req.target);
      break;
    case launcher::CommandOp::kSetParam:
      resp.result = owner->set_param(req.target, req.param);
      break;
    case launcher::CommandOp::kResumeStartup:
      resp.result = owner->resume_startup();
      break;
    case launcher::CommandOp::kSetRecord:
      resp.result = owner->set_record(req.target, req.record_enable, req.record_path);
      break;
    case launcher::CommandOp::kShutdown:
      resp.result.code = launcher::ErrorCode::kOk;
      resp.result.message = "Shutdown requested.";
      request_async_stop(0);
      break;
    default:
      resp.result.code = launcher::ErrorCode::kUnsupported;
      resp.result.message = "Unknown command op.";
      break;
  }

  return resp;
}

void LauncherServer::Impl::request_async_stop(int32_t code) {
  if (code != 0) {
    exit_code.store(code, std::memory_order_release);
  }

  if (stopping.exchange(true, std::memory_order_acq_rel)) {
    return;
  }

  std::weak_ptr<std::atomic_bool> weak_alive = alive_token;

  if (!post_protected([this, weak_alive] {
        auto guard = weak_alive.lock();

        if (guard && guard->load(std::memory_order_acquire)) {
          do_stop();
        }
      })) {
    bool on_rpc_thread = false;

    {
      std::lock_guard lock(ctrl_mtx);
      on_rpc_thread = (ctrl_rpc_loop && ctrl_rpc_loop->is_in_same_thread());
    }

    if (!on_rpc_thread) {
      do_stop();
    } else {
      std::lock_guard lock(fallback_stop_mtx);

      if (!fallback_stop_thread.joinable()) {
        fallback_stop_thread = std::thread([this, weak_alive] {
          auto guard = weak_alive.lock();

          if (guard && guard->load(std::memory_order_acquire)) {
            do_stop();
          }
        });
      }
    }
  }
}

void LauncherServer::Impl::do_stop() {
  if (stop_executed.exchange(true, std::memory_order_acq_rel)) {
    return;
  }

  stopping.store(true, std::memory_order_release);
  state_cv.notify_all();
  shutdown_control_plane();

  std::unique_ptr<Timer> hk_timer;

  {
    std::lock_guard lock(timer_mtx);
    hk_timer.swap(housekeep_timer);
  }

  if (hk_timer) {
    hk_timer->stop();
    hk_timer.reset();
  }

  if (wheel_timer) {
    wheel_timer->stop();
  }

  const auto layers = compute_reverse_topological_layers();
  DeadlineTimer budget(static_cast<int64_t>(manifest.shutdown_timeout_ms), ElapsedTimer::kMilli);

  auto compute_stop_timeout = [&](uint32_t unit_default) -> uint32_t {
    const int64_t rem = budget.remaining_time();

    if (rem <= 0) {
      return 1U;
    }

    return static_cast<uint32_t>(std::min<int64_t>(rem, static_cast<int64_t>(unit_default)));
  };

  size_t max_layer_width = 0U;

  for (const auto& layer : layers) {
    max_layer_width = std::max(max_layer_width, layer.size());
  }

  std::unique_ptr<ThreadPool> stop_pool;

  if (max_layer_width > 1U) {
    stop_pool = std::make_unique<ThreadPool>(std::min<size_t>(max_layer_width, 8U));
  }

  for (const auto& layer : layers) {
    if (layer.size() <= 1U || !stop_pool) {
      for (const auto& name : layer) {
        auto u = find_unit(name);

        if (u) {
          (void)stop_unit_internal(u, compute_stop_timeout(u->sigterm_timeout_ms));
        }
      }
    } else {
      std::vector<TaskHandle> handles;
      handles.reserve(layer.size());

      for (const auto& name : layer) {
        auto u = find_unit(name);

        if (!u) {
          continue;
        }

        handles.push_back(stop_pool->post_task_handle([this, u, &compute_stop_timeout] {
          (void)stop_unit_internal(u, compute_stop_timeout(u->sigterm_timeout_ms));
        }));
      }

      for (const auto& h : handles) {
        (void)h.wait();
      }
    }
  }

  if (stop_pool) {
    stop_pool->shutdown();
    stop_pool.reset();
  }

  process_supervisor.stop_all();
  component_host.stop_all();
  shutdown_logd_collector();

  std::shared_ptr<DiscoveryViewer> viewer;

  {
    std::lock_guard lock(viewer_mtx);
    viewer.swap(discovery_viewer);
  }

  if (viewer) {
    viewer->quit(true);
    viewer->wait_for_quit(1000);
  }

  scheduler.shutdown();
  resource_ctrl.shutdown();
  log_manager.shutdown();

  {
    std::lock_guard lock(stop_mtx);
    running.store(false, std::memory_order_release);
  }

  stop_cv.notify_all();
  owner->quit(false);
}

void LauncherServer::Impl::join_fallback_stop_thread() {
  std::thread join_th;

  {
    std::lock_guard lock(fallback_stop_mtx);

    if (fallback_stop_thread.joinable() && fallback_stop_thread.get_id() != std::this_thread::get_id()) {
      join_th = std::move(fallback_stop_thread);
    }
  }

  if (join_th.joinable()) {
    join_th.join();
  }
}

uint32_t LauncherServer::Impl::calc_ready_budget_ms(const std::shared_ptr<UnitRuntime>& unit) noexcept {
  uint32_t base_timeout = unit->ready.timeout_ms;

  if (unit->status.type == launcher::UnitType::kTask) {
    base_timeout = unit->task_spec.timeout_ms;
  } else if (unit->ready.type == launcher::ReadyProbeType::kDelay) {
    base_timeout = std::max<uint32_t>(unit->ready.timeout_ms, unit->ready.delay_ms);
  }

  return std::max<uint32_t>(base_timeout + 1000U, 5000U);
}

bool LauncherServer::Impl::wait_unit_ready(const std::shared_ptr<UnitRuntime>& unit, uint32_t wait_ms) {
  std::unique_lock lock(state_mtx);
  const bool done = state_cv.wait_for(lock, std::chrono::milliseconds(wait_ms), [&] {
    const auto st = unit->status.state;
    return stopping.load(std::memory_order_acquire) || st == launcher::UnitState::kReady ||
           st == launcher::UnitState::kInactive || st == launcher::UnitState::kFailed ||
           st == launcher::UnitState::kStopped;
  });

  if (!done || stopping.load(std::memory_order_acquire)) {
    return false;
  }

  const auto st = unit->status.state;

  if (st == launcher::UnitState::kReady || st == launcher::UnitState::kInactive) {
    return true;
  }

  if (st == launcher::UnitState::kStopped && unit->status.type == launcher::UnitType::kTask) {
    return unit->oneshot_completed_ok;
  }

  return false;
}

LauncherServer::LauncherServer() : LauncherServer(Config{}) {}

LauncherServer::LauncherServer(const Config& config) : impl_(std::make_unique<Impl>(this, config)) {
  set_name("LauncherServer");
}

LauncherServer::~LauncherServer() {
  stop();
  impl_->alive_token->store(false, std::memory_order_release);
  impl_->join_fallback_stop_thread();

  if (MessageLoop::is_running()) {
    quit(true);
    wait_for_quit();
  }
}

LauncherServer::PluginInfo LauncherServer::inspect_plugin(const std::string& library_path) {
  PluginInfo info;
  info.path = library_path;
  Plugin loader;
  loader.set_log_level(Logger::kOff);

  auto mod = loader.load<LauncherModuleInterface>(library_path, LauncherModuleInterface::kVersionMajor,
                                                  LauncherModuleInterface::kVersionMinor);

  if (mod) {
    info.loaded = true;
    info.is_launcher_module = true;
    info.version_major = LauncherModuleInterface::kVersionMajor;
    info.version_minor = LauncherModuleInterface::kVersionMinor;
    info.components = mod->list_components();
    std::sort(info.components.begin(), info.components.end());
    mod.reset();
    loader.clear();
    return info;
  }

  auto legacy = loader.load<RunablePluginInterface>(library_path, 1, 0);

  if (legacy) {
    info.loaded = true;
    info.is_legacy_runnable = true;
    info.version_major = 1;
    info.version_minor = 0;
    legacy.reset();
    loader.clear();
    return info;
  }

  auto log_plugin = loader.load<LoggerPluginInterface>(library_path, 1, 0);

  if (log_plugin) {
    info.loaded = true;
    info.is_logger_plugin = true;
    info.version_major = 1;
    info.version_minor = 0;
    log_plugin.reset();
    loader.clear();
    return info;
  }

  info.error =
      "Library does not export LauncherModuleInterface, RunablePluginInterface or LoggerPluginInterface (or failed "
      "to load).";
  return info;
}

bool LauncherServer::load_file(const std::string& file_path, std::string* error_out) {
  std::lock_guard start_lock(impl_->start_mtx);

  if (impl_->running.load(std::memory_order_acquire)) {
    if (error_out != nullptr) {
      *error_out = "Cannot call load_file() while LauncherServer is running.";
    }

    return false;
  }

  launcher::LaunchManifest parsed;

  if (!launcher::parse_manifest_file(file_path, impl_->config.arg_overrides, parsed, error_out)) {
    return false;
  }

  const auto rep = launcher::validate_manifest(parsed, false);

  if (!rep.valid) {
    if (error_out != nullptr) {
      std::string joined;

      for (size_t i = 0; i < rep.errors.size(); ++i) {
        if (i > 0U) {
          joined.append("; ");
        }

        joined.append(rep.errors[i]);
      }

      *error_out = joined;
    }

    return false;
  }

  impl_->manifest = std::move(parsed);
  impl_->manifest_loaded = true;
  impl_->rebuild_unit_table();
  return true;
}

bool LauncherServer::load_string(std::string_view toml_content, const std::string& base_dir, std::string* error_out) {
  std::lock_guard start_lock(impl_->start_mtx);

  if (impl_->running.load(std::memory_order_acquire)) {
    if (error_out != nullptr) {
      *error_out = "Cannot call load_string() while LauncherServer is running.";
    }

    return false;
  }

  launcher::LaunchManifest parsed;

  if (!launcher::parse_manifest_string(toml_content, base_dir, impl_->config.arg_overrides, parsed, error_out)) {
    return false;
  }

  const auto rep = launcher::validate_manifest(parsed, false);

  if (!rep.valid) {
    if (error_out != nullptr) {
      std::string joined;

      for (size_t i = 0; i < rep.errors.size(); ++i) {
        if (i > 0U) {
          joined.append("; ");
        }

        joined.append(rep.errors[i]);
      }

      *error_out = joined;
    }

    return false;
  }

  impl_->manifest = std::move(parsed);
  impl_->manifest_loaded = true;
  impl_->rebuild_unit_table();
  return true;
}

LauncherServer::ValidationReport LauncherServer::validate(bool check_programs) const {
  std::lock_guard start_lock(impl_->start_mtx);

  if (!impl_->manifest_loaded) {
    ValidationReport rep;
    rep.errors.emplace_back("No manifest loaded.");
    return rep;
  }

  return launcher::validate_manifest(impl_->manifest, check_programs);
}

std::string LauncherServer::export_graph(GraphFormat format) const {
  std::lock_guard start_lock(impl_->start_mtx);

  if (!impl_->manifest_loaded) {
    return "";
  }

  std::lock_guard lock(impl_->state_mtx);

  if (format == GraphFormat::kDot) {
    auto root = GraphTask::create("launcher_start", [] {});
    std::unordered_map<std::string, std::shared_ptr<GraphTask>> nodes;

    for (const auto& name : impl_->unit_order) {
      const auto& u = impl_->units.at(name);
      auto node = GraphTask::create(name, [] {});
      node->set_group_name(u->status.task_group.empty() ? launcher::unit_type_to_string(u->status.type)
                                                        : u->status.task_group);
      nodes[name] = std::move(node);
    }

    for (const auto& name : impl_->unit_order) {
      const auto& u = impl_->units.at(name);

      if (u->order_preds.empty()) {
        root->precede(nodes[name]);
      } else {
        for (const auto& p : u->order_preds) {
          if (nodes.count(p) != 0U) {
            nodes[p]->precede(nodes[name]);
          }
        }
      }
    }

    std::string dot = root->export_to_dot();
    const auto df_edges = launcher::compute_dataflow_edges(impl_->manifest);

    if (!df_edges.empty()) {
      const size_t close_pos = dot.rfind('}');

      if (close_pos != std::string::npos) {
        std::string extra;

        for (const auto& e : df_edges) {
          extra.append("  \"")
              .append(e.from)
              .append("\" -> \"")
              .append(e.to)
              .append(R"(" [style=dashed, color="#1976d2", label=")")
              .append(e.url)
              .append("\"];\n");
        }

        dot.insert(close_pos, extra);
      }
    }

    return dot;
  }

  if (format == GraphFormat::kMermaid) {
    std::string out = "flowchart TD\n";

    for (const auto& name : impl_->unit_order) {
      const auto& u = impl_->units.at(name);
      const std::string id = launcher::sanitize_identifier(name);
      out.append("  ")
          .append(id)
          .append("[\"")
          .append(name)
          .append(" (")
          .append(launcher::unit_type_to_string(u->status.type))
          .append(")\"]\n");
    }

    for (const auto& name : impl_->unit_order) {
      const auto& u = impl_->units.at(name);
      const std::string tid = launcher::sanitize_identifier(name);

      for (const auto& p : u->order_preds) {
        out.append("  ").append(launcher::sanitize_identifier(p)).append(" --> ").append(tid).append("\n");
      }

      for (const auto& w : u->want_preds) {
        if (!contains_str(u->order_preds, w)) {
          out.append("  ").append(launcher::sanitize_identifier(w)).append(" -. wants .-> ").append(tid).append("\n");
        }
      }
    }

    for (const auto& e : launcher::compute_dataflow_edges(impl_->manifest)) {
      out.append("  ")
          .append(launcher::sanitize_identifier(e.from))
          .append(" ==>|\"")
          .append(e.url)
          .append("\"| ")
          .append(launcher::sanitize_identifier(e.to))
          .append("\n");
    }

    return out;
  }

  std::string out = "Startup Dependency Graph (" + impl_->manifest.name + ")\n";
  std::unordered_map<std::string, size_t> indeg;

  for (const auto& name : impl_->unit_order) {
    indeg[name] = impl_->units.at(name)->order_preds.size();
  }

  std::unordered_set<std::string> done;
  size_t layer_idx = 0U;

  while (done.size() < impl_->unit_order.size()) {
    std::vector<std::string> layer;

    for (const auto& name : impl_->unit_order) {
      if (done.count(name) == 0U && indeg[name] == 0U) {
        layer.push_back(name);
      }
    }

    if (layer.empty()) {
      break;
    }

    out.append("  Layer ").append(std::to_string(layer_idx++)).append(":\n");

    for (const auto& name : layer) {
      done.insert(name);
      const auto& u = impl_->units.at(name);
      out.append("    - ").append(name).append(" [").append(launcher::unit_type_to_string(u->status.type));

      if (!u->status.task_group.empty()) {
        out.append(" @").append(u->status.task_group);
      }

      if (u->status.stage != 0) {
        out.append(" stage=").append(std::to_string(u->status.stage));
      }

      out.append("]");

      if (!u->order_preds.empty()) {
        out.append("  <- after(");

        for (size_t i = 0; i < u->order_preds.size(); ++i) {
          if (i > 0U) {
            out.append(", ");
          }

          out.append(u->order_preds[i]);
        }

        out.append(")");
      }

      if (!u->selected_for_boot || !u->condition || !u->autostart) {
        out.append("  [skipped]");
      }

      out.append("\n");

      for (const auto& s : u->order_succs) {
        if (indeg[s] > 0U) {
          --indeg[s];
        }
      }
    }
  }

  const auto df_edges = launcher::compute_dataflow_edges(impl_->manifest);

  if (!df_edges.empty()) {
    out.append("\nDataflow Topology:\n");

    for (const auto& e : df_edges) {
      out.append("  - ")
          .append(e.from)
          .append(".")
          .append(e.output_key)
          .append(" --[")
          .append(e.url)
          .append("]--> ")
          .append(e.to)
          .append(".")
          .append(e.input_key)
          .append("\n");
    }
  }

  return out;
}

std::string LauncherServer::export_dot() const { return export_graph(GraphFormat::kDot); }

std::string LauncherServer::inspect_manifest_json() const {
  std::lock_guard start_lock(impl_->start_mtx);

  if (!impl_->manifest_loaded) {
    return "{}";
  }

  return launcher::format_manifest_json(impl_->manifest);
}

void LauncherServer::register_event_callback(EventCallback&& callback) {
  std::shared_ptr<EventCallback> old_cb;
  auto new_cb = callback ? std::make_shared<EventCallback>(std::move(callback)) : nullptr;

  {
    std::lock_guard lock(impl_->cb_mtx);
    old_cb = std::move(impl_->local_event_cb);
    impl_->local_event_cb = std::move(new_cb);
  }
}

void LauncherServer::register_log_callback(LogCallback&& callback) {
  std::shared_ptr<LogCallback> old_cb;
  auto new_cb = callback ? std::make_shared<LogCallback>(std::move(callback)) : nullptr;

  {
    std::lock_guard lock(impl_->cb_mtx);
    old_cb = std::move(impl_->local_log_cb);
    impl_->local_log_cb = std::move(new_cb);
  }
}

bool LauncherServer::start() {
  std::unique_lock start_lock(impl_->start_mtx);

  if (!impl_->manifest_loaded) {
    start_lock.unlock();
    VLOG_E("LauncherServer: No manifest loaded.");
    return false;
  }

  if (impl_->running.load(std::memory_order_acquire)) {
    return true;
  }

  impl_->join_fallback_stop_thread();

  if (!is_in_same_thread() && MessageLoop::is_running()) {
    wait_for_quit();
  }

  impl_->rebuild_unit_table();

  {
    std::lock_guard lock(impl_->state_mtx);
    impl_->dynamic_container_components.clear();
    impl_->remote_record_paths.clear();

    for (const auto& [cname, comps] : impl_->container_components) {
      (void)cname;

      for (const auto& cs : comps) {
        if (!cs.record_path.empty() && cs.record_enabled.value_or(true)) {
          impl_->remote_record_paths[cs.name] = cs.record_path;
        }
      }
    }
  }

  impl_->stopping.store(false, std::memory_order_release);
  impl_->stop_executed.store(false, std::memory_order_release);
  impl_->snapshot_dirty.store(false, std::memory_order_release);
  impl_->snapshot_flush_pending.store(false, std::memory_order_release);
  impl_->cached_host_cpu.store(Utils::get_cpu_usage(), std::memory_order_relaxed);
  impl_->cached_host_mem.store(Utils::get_memory_usage(), std::memory_order_relaxed);
  impl_->paused_on_fail.store(false, std::memory_order_release);
  impl_->exit_code.store(0, std::memory_order_release);
  impl_->boot_duration_ms.store(0U, std::memory_order_release);
  launcher::StartupMode next_startup_mode = impl_->manifest.startup_mode;
  uint32_t next_max_parallel = impl_->manifest.max_parallel;

  if (!impl_->config.startup_mode_override.empty()) {
    if (impl_->config.startup_mode_override == "serial") {
      next_startup_mode = launcher::StartupMode::kSerial;
      next_max_parallel = 1U;
    } else if (impl_->config.startup_mode_override == "parallel") {
      next_startup_mode = launcher::StartupMode::kParallel;
      next_max_parallel = 0U;
    }
  }

  if (impl_->config.max_parallel_override >= 0) {
    next_max_parallel = static_cast<uint32_t>(impl_->config.max_parallel_override);

    if (next_max_parallel == 1U) {
      next_startup_mode = launcher::StartupMode::kSerial;
    } else if (next_max_parallel > 1U) {
      next_startup_mode = launcher::StartupMode::kMaxParallel;
    }
  }

  {
    std::lock_guard lock(impl_->state_mtx);
    impl_->session_id = make_session_id();
    impl_->server_start_steady_ms = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);
    impl_->effective_startup_mode = next_startup_mode;
    impl_->effective_max_parallel = next_max_parallel;
  }

  auto log_cfg = impl_->manifest.logger;

  if (!impl_->config.log_level_override.empty()) {
    bool ok = false;
    const auto lv = launcher::parse_log_level_strict(impl_->config.log_level_override, ok);

    if (ok) {
      log_cfg.console_level = lv;
    }
  }

  if (!impl_->config.log_plugin_override.empty()) {
    log_cfg.plugin = impl_->config.log_plugin_override;
  }

  const bool manage_file_logger =
      impl_->config.manage_console || impl_->config.container_mode || !log_cfg.plugin.empty();
  const bool builtin_logd_collector =
      !impl_->config.container_mode && launcher::LogManager::is_builtin_logd_name(log_cfg.plugin);
  const std::string app_name = impl_->config.container_mode && !impl_->config.container_name.empty()
                                   ? impl_->config.container_name
                                   : impl_->manifest.name;

  if (!impl_->log_manager.init(impl_->session_id, log_cfg, impl_->config.log_dir_override, impl_->config.manage_console,
                               manage_file_logger, impl_->config.color_output, impl_->manifest.plugin_dirs, app_name,
                               builtin_logd_collector)) {
    {
      std::lock_guard lock(impl_->stop_mtx);
      impl_->running.store(false, std::memory_order_release);
    }

    impl_->stop_cv.notify_all();
    return false;
  }

  impl_->effective_logger_cfg = std::move(log_cfg);
  impl_->effective_logger_cfg.plugin =
      impl_->log_manager.is_plugin_active() ? impl_->log_manager.effective_plugin() : std::string();
  impl_->log_manager.set_stream_callback([this](const launcher::LogRecord& rec) { impl_->publish_log(rec); });

  impl_->active_record_enabled = !impl_->config.record_dir_override.empty() || impl_->manifest.record.enabled;
  impl_->active_record_force_publish = impl_->manifest.record.force_publish;
  impl_->active_record_format = impl_->manifest.record.format.empty() ? "vcap" : impl_->manifest.record.format;
  impl_->active_record_tag = impl_->manifest.record.tag.empty() ? impl_->manifest.name : impl_->manifest.record.tag;

  if (!impl_->config.record_dir_override.empty()) {
    impl_->active_record_dir = impl_->config.record_dir_override;
  } else if (!impl_->manifest.record.dir.empty()) {
    impl_->active_record_dir = impl_->manifest.record.dir;
  } else {
    impl_->active_record_dir = impl_->log_manager.session_dir() + "/bags";
  }

  {
    std::error_code rec_ec;
    const auto abs_rec = std::filesystem::absolute(std::filesystem::path(impl_->active_record_dir), rec_ec);

    if (!rec_ec && !abs_rec.empty()) {
      impl_->active_record_dir = Helpers::path_to_string(abs_rec);
    }
  }

  impl_->resource_ctrl.init(impl_->session_id, impl_->manifest.slices);
  impl_->scheduler.init(impl_->manifest.schedulers);
  impl_->component_host.set_plugin_dirs(impl_->manifest.plugin_dirs);
  impl_->process_supervisor.reset_stopped();

  std::weak_ptr<std::atomic_bool> weak_alive = impl_->alive_token;

  impl_->component_host.set_callbacks(
      [this, weak_alive](const std::string& name) {
        auto guard = weak_alive.lock();

        if (!guard || !guard->load(std::memory_order_acquire)) {
          return;
        }

        auto u_snap = impl_->find_unit(name);

        if (!u_snap) {
          return;
        }

        const uint64_t captured_gen = u_snap->generation.load(std::memory_order_acquire);

        (void)impl_->post_protected([this, weak_alive, name, captured_gen] {
          auto g2 = weak_alive.lock();

          if (!g2 || !g2->load(std::memory_order_acquire)) {
            return;
          }

          auto u = impl_->find_unit(name);

          if (u && (u->ready.type == launcher::ReadyProbeType::kComponent ||
                    u->ready.type == launcher::ReadyProbeType::kNone)) {
            impl_->finish_readiness(u, captured_gen, true, "component ready");
          }
        });
      },
      [this, weak_alive](const std::string& name, bool healthy, const std::string& detail) {
        auto guard = weak_alive.lock();

        if (!guard || !guard->load(std::memory_order_acquire)) {
          return;
        }

        (void)post_task([this, weak_alive, name, healthy, detail] {
          auto g2 = weak_alive.lock();

          if (!g2 || !g2->load(std::memory_order_acquire)) {
            return;
          }

          auto u = impl_->find_unit(name);

          if (!u) {
            return;
          }

          {
            std::lock_guard lock(impl_->state_mtx);
            u->status.healthy = healthy;

            if (!detail.empty()) {
              u->status.detail = detail;
            }
          }

          impl_->publish_snapshot();
        });
      },
      nullptr);

  impl_->process_supervisor.set_callbacks(
      [this, weak_alive](const std::string& name, uint64_t gen, int32_t code, Process::ExitStatus status,
                         bool expected_stop) {
        auto guard = weak_alive.lock();

        if (guard && guard->load(std::memory_order_acquire)) {
          impl_->handle_process_exit(name, gen, code, status, expected_stop);
        }
      },
      [this, weak_alive](const std::string& name, uint64_t gen) {
        auto guard = weak_alive.lock();

        if (!guard || !guard->load(std::memory_order_acquire)) {
          return;
        }

        (void)impl_->post_protected([this, weak_alive, name, gen] {
          auto g2 = weak_alive.lock();

          if (!g2 || !g2->load(std::memory_order_acquire)) {
            return;
          }

          auto u = impl_->find_unit(name);

          if (u) {
            impl_->finish_readiness(u, gen, true, "stdout pattern matched");
          }
        });
      });

  impl_->wheel_timer->start();

  bool loop_started = false;

  try {
    loop_started = async_run();
  } catch (...) {
    impl_->wheel_timer->stop();
    impl_->scheduler.shutdown();
    impl_->resource_ctrl.shutdown();
    impl_->log_manager.shutdown();

    {
      std::lock_guard lock(impl_->stop_mtx);
      impl_->running.store(false, std::memory_order_release);
    }

    impl_->stop_cv.notify_all();
    throw;
  }

  if VUNLIKELY (!loop_started) {
    impl_->wheel_timer->stop();
    impl_->scheduler.shutdown();
    impl_->resource_ctrl.shutdown();
    impl_->log_manager.shutdown();

    {
      std::lock_guard lock(impl_->stop_mtx);
      impl_->running.store(false, std::memory_order_release);
    }

    impl_->stop_cv.notify_all();
    return false;
  }

  if VUNLIKELY (!impl_->init_control_plane()) {
    impl_->wheel_timer->stop();

    if (MessageLoop::is_running()) {
      quit(true);
      wait_for_quit();
    }

    impl_->scheduler.shutdown();
    impl_->resource_ctrl.shutdown();
    impl_->log_manager.shutdown();

    {
      std::lock_guard lock(impl_->stop_mtx);
      impl_->running.store(false, std::memory_order_release);
    }

    impl_->stop_cv.notify_all();
    return false;
  }

  if (impl_->log_manager.is_builtin_logd() && impl_->active_ctrl_scheme == "intra") {
    impl_->effective_logger_cfg.plugin.clear();
  }

  impl_->running.store(true, std::memory_order_release);
  impl_->publish_snapshot();
  impl_->begin_cold_boot();
  return true;
}

void LauncherServer::request_stop(int32_t exit_code) {
  if (!impl_->running.load(std::memory_order_acquire)) {
    return;
  }

  impl_->request_async_stop(exit_code);
}

void LauncherServer::stop() {
  if (!impl_->running.load(std::memory_order_acquire)) {
    impl_->join_fallback_stop_thread();

    if (!is_in_same_thread() && MessageLoop::is_running()) {
      quit(false);
      wait_for_quit();
    }

    return;
  }

  if (is_in_same_thread()) {
    impl_->request_async_stop(0);
    return;
  }

  impl_->request_async_stop(0);

  for (int i = 0; i < 20 && !wait(50); ++i) {
    if (!MessageLoop::is_running() && !impl_->stop_executed.load(std::memory_order_acquire)) {
      impl_->do_stop();
      break;
    }
  }

  (void)wait(-1);
  impl_->join_fallback_stop_thread();

  if (MessageLoop::is_running()) {
    quit(false);
    wait_for_quit();
  }
}

bool LauncherServer::wait(int32_t timeout_ms) {
  std::unique_lock lock(impl_->stop_mtx);

  if (!impl_->running.load(std::memory_order_acquire)) {
    return true;
  }

  if (timeout_ms < 0) {
    impl_->stop_cv.wait(lock, [this] { return !impl_->running.load(std::memory_order_acquire); });
    return true;
  }

  return impl_->stop_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                 [this] { return !impl_->running.load(std::memory_order_acquire); });
}

bool LauncherServer::is_active() const noexcept { return impl_->running.load(std::memory_order_acquire); }

int32_t LauncherServer::get_exit_code() const noexcept { return impl_->exit_code.load(std::memory_order_acquire); }

launcher::SystemSnapshot LauncherServer::get_snapshot() const { return impl_->build_snapshot(); }

launcher::CommandResult LauncherServer::start_unit(const std::string& target) {
  std::lock_guard op_lock(impl_->op_mtx);
  launcher::CommandResult res;

  if (!impl_->running.load(std::memory_order_acquire)) {
    res.code = launcher::ErrorCode::kInvalidArgument;
    res.message = "LauncherServer is not running; call start() first.";
    return res;
  }

  const auto names = impl_->expand_target_list(target);

  if (names.empty()) {
    res.code = launcher::ErrorCode::kNotFound;
    res.message = "Target '" + target + "' not found.";
    return res;
  }

  std::unordered_set<std::string> local_closure;
  std::unordered_set<std::string> required_set;
  std::vector<std::pair<std::string, std::string>> remote_comps;

  {
    std::lock_guard lock(impl_->state_mtx);
    std::queue<std::pair<std::string, bool>> bfs;

    for (const auto& name : names) {
      if (impl_->units.count(name) != 0U) {
        if (names.size() == 1U && is_active_state(impl_->units[name]->status.state)) {
          res.code = launcher::ErrorCode::kAlreadyRunning;
          res.message = "Unit '" + name + "' is already running.";
          return res;
        }

        if (local_closure.insert(name).second) {
          required_set.insert(name);
          bfs.emplace(name, true);
        }
      } else {
        auto cit = impl_->component_to_container.find(name);

        if (cit != impl_->component_to_container.end()) {
          remote_comps.emplace_back(name, cit->second);

          if (impl_->units.count(cit->second) != 0U && local_closure.insert(cit->second).second) {
            required_set.insert(cit->second);
            bfs.emplace(cit->second, true);
          }
        } else {
          res.code = launcher::ErrorCode::kNotFound;
          res.message = "Unit '" + name + "' not found.";
          return res;
        }
      }
    }

    while (!bfs.empty()) {
      const auto [cur, cur_req] = bfs.front();
      bfs.pop();
      auto it = impl_->units.find(cur);

      if (it == impl_->units.end()) {
        continue;
      }

      for (const auto& dep : it->second->req_preds) {
        const bool promoted = cur_req && required_set.insert(dep).second;
        const bool newly_added = local_closure.insert(dep).second;

        if (newly_added || promoted) {
          bfs.emplace(dep, cur_req);
        }
      }

      for (const auto& dep : it->second->want_preds) {
        if (local_closure.insert(dep).second) {
          bfs.emplace(dep, false);
        }
      }
    }
  }

  const auto ordered = impl_->compute_topo_order_for_set(local_closure);
  std::unordered_set<std::string> failed_units;

  for (const auto& name : ordered) {
    auto u = impl_->find_unit(name);

    if (!u) {
      continue;
    }

    const bool has_failed_req_pred = std::any_of(u->req_preds.begin(), u->req_preds.end(),
                                                 [&](const std::string& dep) { return failed_units.count(dep) != 0U; });

    if (has_failed_req_pred) {
      failed_units.insert(name);

      if (required_set.count(name) != 0U) {
        res.code = launcher::ErrorCode::kDependencyFailed;
        res.message = "Required dependency for '" + name + "' failed.";
        return res;
      }

      continue;
    }

    launcher::UnitState cur_st = launcher::UnitState::kPending;

    {
      std::lock_guard lock(impl_->state_mtx);
      cur_st = u->status.state;
    }

    const bool hosts_remote =
        std::any_of(remote_comps.begin(), remote_comps.end(),
                    [&](const std::pair<std::string, std::string>& rc) { return rc.second == name; });
    const bool has_downstream_in_closure =
        hosts_remote || std::any_of(u->order_succs.begin(), u->order_succs.end(),
                                    [&](const std::string& s) { return local_closure.count(s) != 0U; });

    if (cur_st == launcher::UnitState::kReady || cur_st == launcher::UnitState::kInactive) {
      continue;
    }

    if (cur_st == launcher::UnitState::kStarting || cur_st == launcher::UnitState::kRunning) {
      if (has_downstream_in_closure && !impl_->wait_unit_ready(u, Impl::calc_ready_budget_ms(u))) {
        failed_units.insert(name);

        if (required_set.count(name) != 0U) {
          res.code = launcher::ErrorCode::kDependencyFailed;
          res.message = "Required dependency '" + name + "' did not reach ready state.";
          return res;
        }
      }

      continue;
    }

    std::string err;

    if (!impl_->launch_unit_now(u, &err)) {
      failed_units.insert(name);

      if (required_set.count(name) != 0U) {
        const bool is_target = (std::find(names.begin(), names.end(), name) != names.end());
        res.code = is_target ? launcher::ErrorCode::kInternalError : launcher::ErrorCode::kDependencyFailed;

        if (is_target) {
          res.message = std::move(err);
        } else {
          res.message = "Failed to start required dependency '";
          res.message.append(name).append("': ").append(err);
        }

        return res;
      }

      continue;
    }

    if (has_downstream_in_closure &&
        (u->ready.type != launcher::ReadyProbeType::kNone || u->status.type == launcher::UnitType::kTask ||
         u->status.type == launcher::UnitType::kContainer) &&
        !impl_->wait_unit_ready(u, Impl::calc_ready_budget_ms(u))) {
      failed_units.insert(name);

      if (required_set.count(name) != 0U) {
        res.code = launcher::ErrorCode::kDependencyFailed;
        res.message = "Required dependency '" + name + "' failed readiness probe.";
        return res;
      }
    }
  }

  const bool is_multi_target = (names.size() > 1U) || (!target.empty() && target[0] == '@');

  for (const auto& [comp_name, cname] : remote_comps) {
    auto cu = impl_->find_unit(cname);
    auto cli = impl_->get_or_connect_container_client(cu, 2000U);

    if (!cli || !cli->is_connected()) {
      res.code = launcher::ErrorCode::kDisconnected;
      res.message = "Container '" + cname + "' control plane unavailable.";
      return res;
    }

    auto fwd = cli->start_unit(comp_name);

    if (!fwd.ok() && !(is_multi_target && fwd.code == launcher::ErrorCode::kAlreadyRunning)) {
      return fwd;
    }
  }

  res.code = launcher::ErrorCode::kOk;
  res.message = "Started " + target;
  return res;
}

launcher::CommandResult LauncherServer::stop_unit(const std::string& target) {
  std::lock_guard op_lock(impl_->op_mtx);
  launcher::CommandResult res;
  const auto names = impl_->expand_target_list(target);

  if (names.empty()) {
    res.code = launcher::ErrorCode::kNotFound;
    res.message = "Target '" + target + "' not found.";
    return res;
  }

  std::unordered_set<std::string> visited;

  for (const auto& name : names) {
    if (!impl_->find_unit(name)) {
      std::string cname;

      {
        std::lock_guard lock(impl_->state_mtx);
        auto it = impl_->component_to_container.find(name);

        if (it != impl_->component_to_container.end()) {
          cname = it->second;
        }
      }

      if (!cname.empty()) {
        auto cu = impl_->find_unit(cname);
        auto cli = impl_->get_or_connect_container_client(cu, 2000U);

        if (!cli) {
          res.code = launcher::ErrorCode::kDisconnected;
          res.message = "Container '" + cname + "' control plane unavailable.";
          return res;
        }

        auto fwd = cli->stop_unit(name);

        if (!fwd.ok()) {
          return fwd;
        }

        continue;
      }

      res.code = launcher::ErrorCode::kNotFound;
      res.message = "Unit '" + name + "' not found.";
      return res;
    }

    (void)impl_->stop_unit_cascade(name, visited, true);
  }

  res.code = launcher::ErrorCode::kOk;
  res.message = "Stopped " + target;
  return res;
}

launcher::CommandResult LauncherServer::restart_unit(const std::string& target) {
  std::lock_guard op_lock(impl_->op_mtx);
  const auto target_names = impl_->expand_target_list(target);
  std::unordered_set<std::string> part_of_restore;

  {
    std::lock_guard lock(impl_->state_mtx);
    std::unordered_set<std::string> base_set(target_names.begin(), target_names.end());
    std::queue<std::string> bfs;

    for (const auto& t : target_names) {
      bfs.push(t);
    }

    while (!bfs.empty()) {
      const std::string cur = bfs.front();
      bfs.pop();

      for (const auto& [oname, ounit] : impl_->units) {
        if (base_set.count(oname) == 0U && is_active_state(ounit->status.state) &&
            contains_str(ounit->part_of_preds, cur)) {
          if (part_of_restore.insert(oname).second) {
            bfs.push(oname);
          }
        }
      }
    }
  }

  auto stop_res = stop_unit(target);

  if (!stop_res.ok() && stop_res.code != launcher::ErrorCode::kNotRunning) {
    return stop_res;
  }

  auto start_res = start_unit(target);

  if (!start_res.ok()) {
    return start_res;
  }

  if (!part_of_restore.empty()) {
    for (const auto& dep_name : impl_->compute_topo_order_for_set(part_of_restore)) {
      (void)start_unit(dep_name);
    }
  }

  return start_res;
}

launcher::CommandResult LauncherServer::reload_unit(const std::string& target) {
  std::lock_guard op_lock(impl_->op_mtx);
  launcher::CommandResult res;
  const auto names = impl_->expand_target_list(target);

  if (names.empty()) {
    res.code = launcher::ErrorCode::kNotFound;
    res.message = "Target '" + target + "' not found.";
    return res;
  }

  for (const auto& name : names) {
    auto u = impl_->find_unit(name);

    if (!u) {
      std::string cname;

      {
        std::lock_guard lock(impl_->state_mtx);
        auto it = impl_->component_to_container.find(name);

        if (it != impl_->component_to_container.end()) {
          cname = it->second;
        }
      }

      if (!cname.empty()) {
        auto cu = impl_->find_unit(cname);
        auto cli = impl_->get_or_connect_container_client(cu, 2000U);

        if (!cli) {
          res.code = launcher::ErrorCode::kDisconnected;
          res.message = "Container '" + cname + "' control plane unavailable.";
          return res;
        }

        auto fwd = cli->reload_unit(name);

        if (!fwd.ok()) {
          return fwd;
        }

        continue;
      }

      res.code = launcher::ErrorCode::kNotFound;
      res.message = "Unit '" + name + "' not found.";
      return res;
    }

    bool is_active = false;

    {
      std::lock_guard lock(impl_->state_mtx);
      is_active = is_active_state(u->status.state);
    }

    if (!is_active) {
      res.code = launcher::ErrorCode::kNotRunning;
      res.message = "Unit '" + name + "' is not running.";
      return res;
    }

    if (u->status.type == launcher::UnitType::kComponent) {
      if (u->reload_mode == launcher::ReloadMode::kNone) {
        res.code = launcher::ErrorCode::kUnsupported;
        res.message = "Unit '" + name + "' has reload disabled (reload='none').";
        return res;
      }

      std::string err;

      if (!impl_->component_host.reload_component(name, &err)) {
        res.code = launcher::ErrorCode::kInternalError;
        res.message = err;
        return res;
      }
    } else if (u->reload_mode == launcher::ReloadMode::kSigHup) {
      if (!impl_->process_supervisor.reload_process(name)) {
        res.code = launcher::ErrorCode::kUnsupported;
        res.message = "Failed to send SIGHUP to '" + name + "'.";
        return res;
      }
    } else {
      res.code = launcher::ErrorCode::kUnsupported;
      res.message = "Unit '" + name + "' does not enable reload (set reload='sighup' or 'component').";
      return res;
    }
  }

  res.code = launcher::ErrorCode::kOk;
  res.message = "Reloaded " + target;
  return res;
}

launcher::CommandResult LauncherServer::activate_unit(const std::string& target) {
  std::lock_guard op_lock(impl_->op_mtx);
  launcher::CommandResult res;
  const auto names = impl_->expand_target_list(target);

  if (names.empty()) {
    res.code = launcher::ErrorCode::kNotFound;
    res.message = "Target '" + target + "' not found.";
    return res;
  }

  for (const auto& name : names) {
    auto u = impl_->find_unit(name);

    if (!u) {
      std::string cname;

      {
        std::lock_guard lock(impl_->state_mtx);
        auto it = impl_->component_to_container.find(name);

        if (it != impl_->component_to_container.end()) {
          cname = it->second;
        }
      }

      if (!cname.empty()) {
        auto cu = impl_->find_unit(cname);
        auto cli = impl_->get_or_connect_container_client(cu, 2000U);

        if (!cli) {
          res.code = launcher::ErrorCode::kDisconnected;
          res.message = "Container '" + cname + "' control plane unavailable.";
          return res;
        }

        auto fwd = cli->activate_unit(name);

        if (!fwd.ok()) {
          return fwd;
        }

        continue;
      }

      res.code = launcher::ErrorCode::kNotFound;
      res.message = "Unit '" + name + "' not found.";
      return res;
    }

    if (u->status.type != launcher::UnitType::kComponent) {
      res.code = launcher::ErrorCode::kUnsupported;
      res.message = "Unit '" + name + "' does not support lifecycle activate (component only).";
      return res;
    }

    launcher::UnitState st = launcher::UnitState::kPending;

    {
      std::lock_guard lock(impl_->state_mtx);
      st = u->status.state;
    }

    if (st == launcher::UnitState::kReady) {
      continue;
    }

    if (st != launcher::UnitState::kInactive) {
      res.code = launcher::ErrorCode::kNotRunning;
      res.message =
          "Unit '" + name + "' is not in inactive state (current=" + launcher::unit_state_to_string(st) + ").";
      return res;
    }

    const uint64_t gen = u->generation.fetch_add(1U, std::memory_order_acq_rel) + 1U;
    const bool has_probe = (u->ready.type != launcher::ReadyProbeType::kNone);

    if (has_probe && !impl_->transition_unit_if(u, launcher::UnitState::kInactive, launcher::UnitState::kRunning,
                                                "on_activating", "activating (readiness probe)")) {
      res.code = launcher::ErrorCode::kNotRunning;
      res.message = "Unit '" + name + "' state changed concurrently before activation.";
      return res;
    }

    std::string err;

    if (!impl_->component_host.activate_component(name, &err)) {
      if (has_probe) {
        u->generation.fetch_add(1U, std::memory_order_acq_rel);
        impl_->cancel_unit_timers(u);

        if (!impl_->transition_unit_if(u, launcher::UnitState::kRunning, launcher::UnitState::kInactive,
                                       "on_activate_failed", err)) {
          (void)impl_->transition_unit_if(u, launcher::UnitState::kReady, launcher::UnitState::kInactive,
                                          "on_activate_failed", err);
        }
      }

      res.code = launcher::ErrorCode::kInternalError;
      res.message = err;
      return res;
    }

    if (has_probe) {
      launcher::UnitState post_st = launcher::UnitState::kRunning;

      {
        std::lock_guard lock(impl_->state_mtx);
        post_st = u->status.state;
      }

      if (post_st == launcher::UnitState::kRunning) {
        impl_->arm_readiness_probe(u, gen);
      }
    } else if (impl_->transition_unit_if(u, launcher::UnitState::kInactive, launcher::UnitState::kReady, "on_activated",
                                         "activated")) {
      impl_->trigger_units(u->on_ready_trigger);
    }
  }

  res.code = launcher::ErrorCode::kOk;
  res.message = "Activated " + target;
  return res;
}

launcher::CommandResult LauncherServer::deactivate_unit(const std::string& target) {
  std::lock_guard op_lock(impl_->op_mtx);
  launcher::CommandResult res;
  const auto names = impl_->expand_target_list(target);

  if (names.empty()) {
    res.code = launcher::ErrorCode::kNotFound;
    res.message = "Target '" + target + "' not found.";
    return res;
  }

  for (const auto& name : names) {
    auto u = impl_->find_unit(name);

    if (!u) {
      std::string cname;

      {
        std::lock_guard lock(impl_->state_mtx);
        auto it = impl_->component_to_container.find(name);

        if (it != impl_->component_to_container.end()) {
          cname = it->second;
        }
      }

      if (!cname.empty()) {
        auto cu = impl_->find_unit(cname);
        auto cli = impl_->get_or_connect_container_client(cu, 2000U);

        if (!cli) {
          res.code = launcher::ErrorCode::kDisconnected;
          res.message = "Container '" + cname + "' control plane unavailable.";
          return res;
        }

        auto fwd = cli->deactivate_unit(name);

        if (!fwd.ok()) {
          return fwd;
        }

        continue;
      }

      res.code = launcher::ErrorCode::kNotFound;
      res.message = "Unit '" + name + "' not found.";
      return res;
    }

    if (u->status.type != launcher::UnitType::kComponent) {
      res.code = launcher::ErrorCode::kUnsupported;
      res.message = "Unit '" + name + "' does not support lifecycle deactivate (component only).";
      return res;
    }

    launcher::UnitState st = launcher::UnitState::kPending;

    {
      std::lock_guard lock(impl_->state_mtx);
      st = u->status.state;
    }

    if (st == launcher::UnitState::kInactive) {
      continue;
    }

    if (st != launcher::UnitState::kReady) {
      res.code = launcher::ErrorCode::kNotRunning;
      res.message = "Unit '" + name +
                    "' must be in ready state to deactivate (current=" + launcher::unit_state_to_string(st) + ").";
      return res;
    }

    std::string err;

    if (!impl_->component_host.deactivate_component(name, &err)) {
      res.code = launcher::ErrorCode::kInternalError;
      res.message = err;
      return res;
    }

    (void)impl_->transition_unit_if(u, launcher::UnitState::kReady, launcher::UnitState::kInactive, "on_deactivated",
                                    "inactive standby");
  }

  res.code = launcher::ErrorCode::kOk;
  res.message = "Deactivated " + target;
  return res;
}

launcher::CommandResult LauncherServer::reset_failed(const std::string& target) {
  std::lock_guard op_lock(impl_->op_mtx);
  launcher::CommandResult res;
  std::vector<std::string> targets;

  if (target.empty()) {
    std::lock_guard lock(impl_->state_mtx);
    targets = impl_->unit_order;
  } else {
    targets = impl_->expand_target_list(target);

    if (targets.empty()) {
      res.code = launcher::ErrorCode::kNotFound;
      res.message = "Target '" + target + "' not found.";
      return res;
    }
  }

  size_t reset_count = 0U;
  std::vector<std::shared_ptr<UnitRuntime>> to_transition;
  std::vector<std::pair<std::string, std::string>> remote_targets;
  std::vector<std::string> all_containers;

  {
    std::lock_guard lock(impl_->state_mtx);

    if (target.empty()) {
      for (const auto& name : impl_->unit_order) {
        auto it = impl_->units.find(name);

        if (it != impl_->units.end() && it->second->status.type == launcher::UnitType::kContainer &&
            is_active_state(it->second->status.state)) {
          all_containers.push_back(name);
        }
      }
    }

    for (const auto& name : targets) {
      auto it = impl_->units.find(name);

      if (it == impl_->units.end()) {
        auto cit = impl_->component_to_container.find(name);

        if (cit != impl_->component_to_container.end()) {
          remote_targets.emplace_back(name, cit->second);
          continue;
        }

        if (!target.empty()) {
          res.code = launcher::ErrorCode::kNotFound;
          res.message = "Unit '" + name + "' not found.";
          return res;
        }

        continue;
      }

      auto& u = it->second;
      u->start_limit_window_start_ms = 0U;
      u->start_limit_attempts = 0U;
      u->backoff_streak = 0U;
      u->legacy_restart_window.clear();
      u->status.liveness_failures = 0U;
      u->status.failed_reason.clear();

      if (u->status.state == launcher::UnitState::kFailed) {
        to_transition.push_back(u);
      }
    }
  }

  for (const auto& u : to_transition) {
    if (impl_->transition_unit_if(u, launcher::UnitState::kFailed, launcher::UnitState::kStopped, "on_reset",
                                  "reset")) {
      {
        std::lock_guard lock(impl_->state_mtx);
        u->status.healthy = true;
      }

      ++reset_count;
    }
  }

  for (const auto& cname : all_containers) {
    auto cu = impl_->find_unit(cname);
    auto cli = impl_->get_or_connect_container_client(cu, 2000U);

    if (cli && cli->is_connected()) {
      auto fwd = cli->reset_failed("");

      if (fwd.ok()) {
        ++reset_count;
      }
    }
  }

  for (const auto& [comp_name, cname] : remote_targets) {
    auto cu = impl_->find_unit(cname);
    auto cli = impl_->get_or_connect_container_client(cu, 2000U);

    if (!cli || !cli->is_connected()) {
      res.code = launcher::ErrorCode::kDisconnected;
      res.message = "Container '" + cname + "' control plane unavailable.";
      return res;
    }

    auto fwd = cli->reset_failed(comp_name);

    if (!fwd.ok()) {
      return fwd;
    }

    ++reset_count;
  }

  impl_->publish_snapshot();
  res.code = launcher::ErrorCode::kOk;
  res.message = "Reset failed state (" + std::to_string(reset_count) + " units cleared).";
  return res;
}

launcher::CommandResult LauncherServer::isolate_group(const std::string& group_name) {
  std::lock_guard op_lock(impl_->op_mtx);
  launcher::CommandResult res;

  if (!impl_->running.load(std::memory_order_acquire)) {
    res.code = launcher::ErrorCode::kInvalidArgument;
    res.message = "LauncherServer is not running; call start() first.";
    return res;
  }

  const std::string clean = (!group_name.empty() && group_name[0] == '@') ? group_name.substr(1U) : group_name;
  std::unordered_set<std::string> keep_set;
  std::unordered_set<std::string> required_keep;
  std::unordered_set<std::string> stop_set;
  std::unordered_set<std::string> start_set;
  std::unordered_set<std::string> explicit_keep_containers;
  std::vector<std::pair<std::string, std::string>> remote_comps;

  {
    std::lock_guard lock(impl_->state_mtx);
    auto git = impl_->group_members.find(clean);

    if (git == impl_->group_members.end()) {
      res.code = launcher::ErrorCode::kNotFound;
      res.message = "Group '@" + clean + "' not found.";
      return res;
    }

    std::queue<std::pair<std::string, bool>> bfs;

    auto add_member = [&](const std::string& m, bool is_req, bool direct_member) {
      auto uit = impl_->units.find(m);

      if (uit != impl_->units.end()) {
        if (direct_member && uit->second->status.type == launcher::UnitType::kContainer) {
          explicit_keep_containers.insert(m);
        }

        const bool promoted = is_req && required_keep.insert(m).second;
        const bool added = keep_set.insert(m).second;

        if (added || promoted) {
          bfs.emplace(m, is_req);
        }
      } else {
        auto cit = impl_->component_to_container.find(m);

        if (cit != impl_->component_to_container.end()) {
          remote_comps.emplace_back(m, cit->second);

          if (impl_->units.count(cit->second) != 0U) {
            const bool promoted = is_req && required_keep.insert(cit->second).second;
            const bool added = keep_set.insert(cit->second).second;

            if (added || promoted) {
              bfs.emplace(cit->second, is_req);
            }
          }
        }
      }
    };

    for (const auto& m : git->second) {
      add_member(m, true, true);
    }

    while (!bfs.empty()) {
      const auto [cur, cur_req] = bfs.front();
      bfs.pop();
      auto it = impl_->units.find(cur);

      if (it == impl_->units.end()) {
        continue;
      }

      for (const auto& dep : it->second->req_preds) {
        add_member(dep, cur_req, false);
      }

      for (const auto& dep : it->second->want_preds) {
        add_member(dep, false, false);
      }
    }

    for (const auto& name : impl_->unit_order) {
      const auto& u = impl_->units[name];
      const auto st = u->status.state;

      if (keep_set.count(name) == 0U) {
        const bool pending_or_active = is_active_state(st) || st == launcher::UnitState::kInactive ||
                                       st == launcher::UnitState::kRestarting || u->respawn_timer_key >= 0 ||
                                       u->delay_timer_key >= 0;

        if (pending_or_active) {
          stop_set.insert(name);
        }
      } else if (!is_active_state(st) && st != launcher::UnitState::kInactive) {
        start_set.insert(name);
      }
    }
  }

  auto stop_order = impl_->compute_topo_order_for_set(stop_set);
  std::reverse(stop_order.begin(), stop_order.end());

  for (const auto& name : stop_order) {
    auto u = impl_->find_unit(name);

    if (u) {
      (void)impl_->stop_unit_internal(u);
    }
  }

  const auto start_order = impl_->compute_topo_order_for_set(start_set);
  std::unordered_set<std::string> failed_start;

  for (const auto& name : start_order) {
    auto u = impl_->find_unit(name);

    if (!u) {
      continue;
    }

    for (const auto& dep : u->order_preds) {
      if (keep_set.count(dep) == 0U || start_set.count(dep) != 0U || failed_start.count(dep) != 0U) {
        continue;
      }

      auto dep_u = impl_->find_unit(dep);

      if (!dep_u) {
        continue;
      }

      launcher::UnitState dep_st = launcher::UnitState::kPending;

      {
        std::lock_guard lock(impl_->state_mtx);
        dep_st = dep_u->status.state;
      }

      if ((dep_st == launcher::UnitState::kStarting ||
           (dep_st == launcher::UnitState::kRunning &&
            (dep_u->ready.type != launcher::ReadyProbeType::kNone || dep_u->status.type == launcher::UnitType::kTask ||
             dep_u->status.type == launcher::UnitType::kContainer))) &&
          !impl_->wait_unit_ready(dep_u, Impl::calc_ready_budget_ms(dep_u))) {
        failed_start.insert(dep);
      }
    }

    const bool has_failed_pred = std::any_of(u->req_preds.begin(), u->req_preds.end(),
                                             [&](const std::string& dep) { return failed_start.count(dep) != 0U; });

    if (has_failed_pred) {
      failed_start.insert(name);

      if (required_keep.count(name) != 0U) {
        res.code = launcher::ErrorCode::kDependencyFailed;
        res.message = "Isolate required dependency for '" + name + "' failed.";
        return res;
      }

      continue;
    }

    std::string err;

    if (!impl_->launch_unit_now(u, &err)) {
      failed_start.insert(name);

      if (required_keep.count(name) != 0U) {
        res.code = launcher::ErrorCode::kInternalError;
        res.message = "Isolate failed starting '";
        res.message.append(name).append("': ").append(err);
        return res;
      }

      continue;
    }

    const bool has_downstream = (u->status.type == launcher::UnitType::kContainer) ||
                                std::any_of(u->order_succs.begin(), u->order_succs.end(),
                                            [&](const std::string& s) { return start_set.count(s) != 0U; });

    if (has_downstream &&
        (u->ready.type != launcher::ReadyProbeType::kNone || u->status.type == launcher::UnitType::kTask ||
         u->status.type == launcher::UnitType::kContainer) &&
        !impl_->wait_unit_ready(u, Impl::calc_ready_budget_ms(u))) {
      failed_start.insert(name);

      if (required_keep.count(name) != 0U) {
        res.code = launcher::ErrorCode::kDependencyFailed;
        res.message = "Isolate dependency '" + name + "' failed readiness probe.";
        return res;
      }
    }
  }

  std::unordered_set<std::string> remote_keep_names;
  std::vector<std::pair<std::string, std::string>> remote_stop_comps;

  {
    std::lock_guard lock(impl_->state_mtx);
    std::queue<std::string> rbfs;

    auto keep_remote_ref = [&](const std::string& ref) {
      if (ref.empty()) {
        return;
      }

      auto check_one = [&](const std::string& cand) {
        auto cit = impl_->component_to_container.find(cand);

        if (cit != impl_->component_to_container.end() && remote_keep_names.insert(cand).second) {
          remote_comps.emplace_back(cand, cit->second);
          rbfs.push(cand);
        }
      };

      if (ref[0] == '@') {
        auto git = impl_->group_members.find(ref.substr(1U));

        if (git != impl_->group_members.end()) {
          for (const auto& m : git->second) {
            check_one(m);
          }
        }
      } else {
        check_one(ref);
      }
    };

    auto scan_dep_spec = [&](const launcher::DependencySpec& d) {
      for (const auto& r : d.depends_on) {
        keep_remote_ref(r);
      }

      for (const auto& r : d.requires_units) {
        keep_remote_ref(r);
      }

      for (const auto& r : d.wants) {
        keep_remote_ref(r);
      }

      for (const auto& r : d.binds_to) {
        keep_remote_ref(r);
      }
    };

    std::unordered_set<std::string> component_pulled_containers;

    for (const auto& [comp_name, cname] : remote_comps) {
      component_pulled_containers.insert(cname);

      if (remote_keep_names.insert(comp_name).second) {
        rbfs.push(comp_name);
      }
    }

    for (const auto& name : keep_set) {
      auto uit = impl_->units.find(name);

      if (uit == impl_->units.end()) {
        continue;
      }

      const auto& u = uit->second;

      if (u->status.type == launcher::UnitType::kComponent) {
        scan_dep_spec(u->comp_spec.deps);
      } else if (u->status.type == launcher::UnitType::kProcess) {
        scan_dep_spec(u->proc_spec.deps);
      } else if (u->status.type == launcher::UnitType::kTask) {
        scan_dep_spec(u->task_spec.deps);
      } else if (u->status.type == launcher::UnitType::kProxy) {
        scan_dep_spec(u->proxy_spec.deps);
      } else if (u->status.type == launcher::UnitType::kContainer) {
        scan_dep_spec(u->container_spec.deps);

        if (explicit_keep_containers.count(name) != 0U || component_pulled_containers.count(name) == 0U) {
          auto ccomp_it = impl_->container_components.find(name);

          if (ccomp_it != impl_->container_components.end()) {
            for (const auto& cs : ccomp_it->second) {
              if (remote_keep_names.insert(cs.name).second) {
                if (cs.condition && cs.autostart) {
                  remote_comps.emplace_back(cs.name, name);
                }

                rbfs.push(cs.name);
              }
            }
          }
        }
      }
    }

    while (!rbfs.empty()) {
      const std::string rname = std::move(rbfs.front());
      rbfs.pop();

      for (const auto& [cname, comps] : impl_->container_components) {
        (void)cname;

        for (const auto& cs : comps) {
          if (cs.name == rname) {
            scan_dep_spec(cs.deps);
            break;
          }
        }
      }
    }

    for (const auto& [comp_name, cname] : impl_->component_to_container) {
      if (keep_set.count(cname) != 0U && remote_keep_names.count(comp_name) == 0U) {
        remote_stop_comps.emplace_back(comp_name, cname);
      }
    }
  }

  for (const auto& [comp_name, cname] : remote_stop_comps) {
    auto cu = impl_->find_unit(cname);
    auto cli = impl_->get_or_connect_container_client(cu, 2000U);

    if (!cli || !cli->is_connected()) {
      res.code = launcher::ErrorCode::kDisconnected;
      res.message = "Isolate failed: container '" + cname + "' control plane unavailable.";
      return res;
    }

    auto fwd = cli->stop_unit(comp_name);

    if (!fwd.ok() && fwd.code != launcher::ErrorCode::kNotRunning && fwd.code != launcher::ErrorCode::kNotFound) {
      return fwd;
    }
  }

  for (const auto& [comp_name, cname] : remote_comps) {
    auto cu = impl_->find_unit(cname);
    auto cli = impl_->get_or_connect_container_client(cu, 2000U);

    if (!cli || !cli->is_connected()) {
      res.code = launcher::ErrorCode::kDisconnected;
      res.message = "Isolate failed: container '" + cname + "' control plane unavailable.";
      return res;
    }

    auto fwd = cli->start_unit(comp_name);

    if (!fwd.ok() && fwd.code != launcher::ErrorCode::kAlreadyRunning) {
      return fwd;
    }
  }

  res.code = launcher::ErrorCode::kOk;
  res.message = "Isolated @" + clean + " (stopped " + std::to_string(stop_order.size()) + ", started " +
                std::to_string(start_order.size()) + ").";
  return res;
}

launcher::CommandResult LauncherServer::get_unit_config(const std::string& unit_name) const {
  launcher::CommandResult res;
  std::string toml_text;
  auto u = impl_->find_unit(unit_name);

  if (u) {
    switch (u->status.type) {
      case launcher::UnitType::kProxy:
        toml_text = launcher::format_proxy_config_toml(u->proxy_spec);
        break;
      case launcher::UnitType::kProcess:
        toml_text = launcher::format_process_spec_toml(u->proc_spec);
        break;
      case launcher::UnitType::kComponent: {
        launcher::ComponentSpec live_spec;

        {
          std::lock_guard lock(impl_->state_mtx);
          live_spec = u->comp_spec;
        }

        LauncherParamMap live_params;

        if (impl_->component_host.get_params(unit_name, live_params)) {
          live_spec.params = std::move(live_params);
        }

        toml_text = launcher::format_component_spec_toml(live_spec);
        break;
      }
      case launcher::UnitType::kContainer:
        toml_text = launcher::format_container_spec_toml(u->container_spec);
        break;
      case launcher::UnitType::kTask:
        toml_text = launcher::format_task_spec_toml(u->task_spec);
        break;
      default:
        break;
    }
  } else {
    std::string cname;

    {
      std::lock_guard lock(impl_->state_mtx);
      auto it = impl_->component_to_container.find(unit_name);

      if (it != impl_->component_to_container.end()) {
        cname = it->second;
      }
    }

    if (!cname.empty()) {
      auto cu = impl_->find_unit(cname);
      auto cli = impl_->get_or_connect_container_client(cu, 2000U);

      if (cli && cli->is_connected()) {
        auto fwd = cli->get_unit_config(unit_name);

        if (fwd.ok()) {
          return fwd;
        }
      }

      toml_text = launcher::format_unit_config_toml(impl_->manifest, unit_name);
    }
  }

  if (toml_text.empty()) {
    res.code = launcher::ErrorCode::kNotFound;
    res.message = "Unit '" + unit_name + "' not found.";
    return res;
  }

  res.code = launcher::ErrorCode::kOk;
  res.message = std::move(toml_text);
  return res;
}

launcher::CommandResult LauncherServer::query_logs(const launcher::LogQuery& query) const {
  launcher::CommandResult res;
  std::unordered_set<std::string> allowed;

  if (!query.unit_filter.empty()) {
    for (const auto& name : impl_->expand_target_list(query.unit_filter)) {
      allowed.insert(name);
    }

    if (allowed.empty()) {
      allowed.insert(query.unit_filter);
    }
  }

  res.code = launcher::ErrorCode::kOk;
  res.logs = impl_->log_manager.query_history(query, allowed);
  return res;
}

launcher::CommandResult LauncherServer::load_component(const launcher::LoadComponentRequest& request) {
  std::lock_guard op_lock(impl_->op_mtx);
  launcher::CommandResult res;

  if (request.name.empty() || request.library.empty()) {
    res.code = launcher::ErrorCode::kInvalidArgument;
    res.message = "Component name and library are required.";
    return res;
  }

  if (!request.container.empty()) {
    auto cu = impl_->find_unit(request.container);

    if (!cu || cu->status.type != launcher::UnitType::kContainer) {
      res.code = launcher::ErrorCode::kNotFound;
      res.message = "Container '" + request.container + "' not found.";
      return res;
    }

    auto cli = impl_->get_or_connect_container_client(cu, 3000U);

    if (!cli || !cli->is_connected()) {
      res.code = launcher::ErrorCode::kDisconnected;
      res.message = "Container '" + request.container + "' control plane is not connected.";
      return res;
    }

    {
      std::lock_guard lock(impl_->state_mtx);

      if (impl_->units.count(request.name) != 0U || impl_->component_to_container.count(request.name) != 0U) {
        res.code = launcher::ErrorCode::kAlreadyExists;
        res.message = "Unit '" + request.name + "' already exists.";
        return res;
      }

      impl_->component_to_container.emplace(request.name, request.container);
    }

    auto fwd_req = request;
    fwd_req.container.clear();
    auto fwd = cli->load_component(fwd_req);

    if (!fwd.ok()) {
      std::lock_guard lock(impl_->state_mtx);
      auto it = impl_->component_to_container.find(request.name);

      if (it != impl_->component_to_container.end() && it->second == request.container) {
        impl_->component_to_container.erase(it);
      }
    } else {
      std::lock_guard lock(impl_->state_mtx);
      impl_->dynamic_container_components[request.container].insert(request.name);
    }

    return fwd;
  }

  const std::string sched_group = request.scheduler_group.empty() ? "default" : request.scheduler_group;

  if (!impl_->scheduler.has_group(sched_group)) {
    res.code = launcher::ErrorCode::kInvalidArgument;
    res.message = "Unknown scheduler group '" + sched_group + "'.";
    return res;
  }

  if (!request.compute_pool.empty() && impl_->scheduler.acquire_thread_pool(request.compute_pool) == nullptr) {
    res.code = launcher::ErrorCode::kInvalidArgument;
    res.message = "Compute pool '" + request.compute_pool + "' is not a configured pool scheduler.";
    return res;
  }

  launcher::ComponentSpec spec;
  spec.name = request.name;
  spec.ns = request.ns;
  spec.library = request.library;
  spec.class_name = request.class_name;
  spec.scheduler_group = sched_group;
  spec.compute_pool = request.compute_pool;
  spec.worker_index = request.worker_index;
  spec.task_priority = request.task_priority;
  spec.legacy_runnable = request.legacy_runnable;
  spec.urls = request.urls;
  spec.inputs = request.inputs;
  spec.outputs = request.outputs;

  {
    std::lock_guard lock(impl_->state_mtx);
    spec.params = impl_->global_params;
  }

  for (const auto& entry : request.params) {
    LauncherParamValue val;

    if (entry.key.empty() || !launcher::decode_param_entry(entry, val)) {
      res.code = launcher::ErrorCode::kInvalidArgument;
      res.message = "Invalid parameter entry '" + entry.key + "'.";
      return res;
    }

    spec.params[entry.key] = std::move(val);
  }

  for (const auto& [src, dst] : request.remaps) {
    spec.remaps.emplace_back(src, dst);
  }

  auto u = MemoryResource::make_shared<UnitRuntime>();
  u->status.name = spec.name;
  u->status.ns = spec.ns;
  u->status.group = sched_group;
  u->status.worker_index = spec.worker_index;
  u->status.type = launcher::UnitType::kComponent;
  u->status.state = launcher::UnitState::kPending;
  u->status.pid = Utils::get_pid();
  u->comp_spec = spec;

  {
    std::lock_guard lock(impl_->state_mtx);

    if (impl_->units.count(spec.name) != 0U || impl_->component_to_container.count(spec.name) != 0U) {
      res.code = launcher::ErrorCode::kAlreadyExists;
      res.message = "Unit '" + spec.name + "' already exists.";
      return res;
    }

    impl_->unit_order.push_back(spec.name);
    impl_->units.emplace(spec.name, u);
  }

  if (request.auto_start) {
    std::string err;

    if (!impl_->launch_unit_now(u, &err)) {
      {
        std::lock_guard lock(impl_->state_mtx);
        impl_->units.erase(spec.name);
        impl_->unit_order.erase(std::remove(impl_->unit_order.begin(), impl_->unit_order.end(), spec.name),
                                impl_->unit_order.end());
      }

      res.code = launcher::ErrorCode::kInternalError;
      res.message = err;
      return res;
    }
  }

  impl_->publish_snapshot();
  res.code = launcher::ErrorCode::kOk;
  res.message = "Loaded component '" + spec.name + "'.";
  return res;
}

launcher::CommandResult LauncherServer::unload_component(const std::string& unit_name) {
  std::lock_guard op_lock(impl_->op_mtx);
  launcher::CommandResult res;
  auto u = impl_->find_unit(unit_name);

  if (!u) {
    std::string cname;

    {
      std::lock_guard lock(impl_->state_mtx);
      auto it = impl_->component_to_container.find(unit_name);

      if (it != impl_->component_to_container.end()) {
        cname = it->second;
      }
    }

    if (!cname.empty()) {
      auto cu = impl_->find_unit(cname);
      auto cli = impl_->get_or_connect_container_client(cu, 2000U);

      if (!cli || !cli->is_connected()) {
        res.code = launcher::ErrorCode::kDisconnected;
        res.message = "Container '" + cname + "' control plane unavailable.";
        return res;
      }

      auto fwd = cli->unload_component(unit_name);

      if (fwd.ok()) {
        std::lock_guard lock(impl_->state_mtx);
        impl_->component_to_container.erase(unit_name);
        impl_->remote_record_paths.erase(unit_name);
        auto dyn_it = impl_->dynamic_container_components.find(cname);

        if (dyn_it != impl_->dynamic_container_components.end()) {
          dyn_it->second.erase(unit_name);
        }

        auto ccomp_it = impl_->container_components.find(cname);

        if (ccomp_it != impl_->container_components.end()) {
          auto& comps = ccomp_it->second;
          comps.erase(std::remove_if(comps.begin(), comps.end(),
                                     [&](const launcher::ComponentSpec& cs) { return cs.name == unit_name; }),
                      comps.end());
        }

        for (auto& [gname, members] : impl_->group_members) {
          (void)gname;
          members.erase(std::remove(members.begin(), members.end(), unit_name), members.end());
        }
      }

      return fwd;
    }

    res.code = launcher::ErrorCode::kNotFound;
    res.message = "Component '" + unit_name + "' not found.";
    return res;
  }

  if (u->status.type != launcher::UnitType::kComponent) {
    res.code = launcher::ErrorCode::kUnsupported;
    res.message = "Unit '" + unit_name + "' is not a component.";
    return res;
  }

  impl_->cascade_stop_bound_dependents(unit_name);
  (void)impl_->stop_unit_internal(u);

  {
    std::lock_guard lock(impl_->state_mtx);
    impl_->units.erase(unit_name);
    impl_->unit_order.erase(std::remove(impl_->unit_order.begin(), impl_->unit_order.end(), unit_name),
                            impl_->unit_order.end());
    impl_->remote_record_paths.erase(unit_name);

    for (auto& [gname, members] : impl_->group_members) {
      (void)gname;
      members.erase(std::remove(members.begin(), members.end(), unit_name), members.end());
    }

    auto erase_name = [&](std::vector<std::string>& vec) {
      vec.erase(std::remove(vec.begin(), vec.end(), unit_name), vec.end());
    };

    for (auto& [other_name, other_unit] : impl_->units) {
      (void)other_name;
      erase_name(other_unit->order_preds);
      erase_name(other_unit->order_succs);
      erase_name(other_unit->req_preds);
      erase_name(other_unit->want_preds);
      erase_name(other_unit->binds_preds);
      erase_name(other_unit->part_of_preds);
      erase_name(other_unit->conflict_units);
      erase_name(other_unit->on_start_trigger);
      erase_name(other_unit->on_ready_trigger);
      erase_name(other_unit->on_exit_trigger);
      erase_name(other_unit->on_error_trigger);
      erase_name(other_unit->status.dependencies);
      erase_name(other_unit->status.wants);
      erase_name(other_unit->status.binds_to);
      erase_name(other_unit->status.conflicts);
    }

    for (auto& [st_id, st_list] : impl_->stage_units) {
      (void)st_id;
      erase_name(st_list);
    }
  }

  impl_->publish_snapshot();
  res.code = launcher::ErrorCode::kOk;
  res.message = "Unloaded component '" + unit_name + "'.";
  return res;
}

launcher::CommandResult LauncherServer::get_params(const std::string& unit_name) const {
  launcher::CommandResult res;
  LauncherParamMap params;

  if (unit_name.empty()) {
    std::lock_guard lock(impl_->state_mtx);
    params = impl_->global_params;
  } else if (!impl_->component_host.get_params(unit_name, params)) {
    std::string cname;

    {
      std::lock_guard lock(impl_->state_mtx);
      auto it = impl_->component_to_container.find(unit_name);

      if (it != impl_->component_to_container.end()) {
        cname = it->second;
      }
    }

    if (!cname.empty()) {
      auto cu = impl_->find_unit(cname);
      auto cli = impl_->get_or_connect_container_client(cu, 2000U);

      if (cli) {
        return cli->get_params(unit_name);
      }
    }

    auto u = impl_->find_unit(unit_name);

    if (!u || u->status.type != launcher::UnitType::kComponent) {
      res.code = launcher::ErrorCode::kNotFound;
      res.message = "Component '" + unit_name + "' not found or has no params.";
      return res;
    }

    {
      std::lock_guard lock(impl_->state_mtx);
      params = u->comp_spec.params;
    }
  }

  res.params.reserve(params.size());

  for (const auto& [k, v] : params) {
    res.params.push_back(launcher::encode_param_entry(k, v));
  }

  std::sort(res.params.begin(), res.params.end(),
            [](const launcher::ParamEntry& a, const launcher::ParamEntry& b) { return a.key < b.key; });
  res.code = launcher::ErrorCode::kOk;
  return res;
}

launcher::CommandResult LauncherServer::set_param(const std::string& unit_name, const launcher::ParamEntry& entry) {
  launcher::CommandResult res;
  LauncherParamValue val;

  if (entry.key.empty() || !launcher::decode_param_entry(entry, val)) {
    res.code = launcher::ErrorCode::kInvalidArgument;
    res.message = "Invalid parameter key or value encoding.";
    return res;
  }

  if (unit_name.empty()) {
    std::lock_guard lock(impl_->state_mtx);
    impl_->global_params[entry.key] = val;
    res.code = launcher::ErrorCode::kOk;
    res.message = "Updated global parameter '" + entry.key + "'.";
    return res;
  }

  std::string err;

  if (impl_->component_host.set_param(unit_name, entry.key, val, &err)) {
    auto u = impl_->find_unit(unit_name);

    if (u) {
      std::lock_guard lock(impl_->state_mtx);
      u->comp_spec.params[entry.key] = val;
    }

    res.code = launcher::ErrorCode::kOk;
    res.message = "Updated parameter '" + entry.key + "' on '" + unit_name + "'.";
    return res;
  }

  std::string cname;

  {
    std::lock_guard lock(impl_->state_mtx);
    auto it = impl_->component_to_container.find(unit_name);

    if (it != impl_->component_to_container.end()) {
      cname = it->second;
    }
  }

  if (!cname.empty()) {
    auto cu = impl_->find_unit(cname);
    auto cli = impl_->get_or_connect_container_client(cu, 2000U);

    if (cli) {
      return cli->set_param(unit_name, entry);
    }
  }

  res.code = launcher::ErrorCode::kInternalError;
  res.message = err.empty() ? ("Failed to set parameter on '" + unit_name + "'.") : err;
  return res;
}

launcher::CommandResult LauncherServer::set_record(const std::string& target, bool enable, const std::string& path) {
  launcher::CommandResult res;

  if (enable && !path.empty()) {
    std::string low = path;

    for (char& ch : low) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }

    const bool valid_ext = Helpers::has_endwith(low, ".vdb") || Helpers::has_endwith(low, ".vdbx") ||
                           Helpers::has_endwith(low, ".vcap") || Helpers::has_endwith(low, ".vcapx");

    if (!valid_ext) {
      res.code = launcher::ErrorCode::kInvalidArgument;
      res.message = "Record path '" + path + "' must end with .vdb, .vdbx, .vcap, or .vcapx.";
      return res;
    }
  }

  std::lock_guard op_lock(impl_->op_mtx);

  std::vector<std::string> names;
  const bool wildcard = target.empty();
  const bool is_group = (!target.empty() && target[0] == '@');

  if (wildcard) {
    std::lock_guard lock(impl_->state_mtx);

    for (const auto& name : impl_->unit_order) {
      auto it = impl_->units.find(name);

      if (it != impl_->units.end() && it->second->status.type == launcher::UnitType::kComponent) {
        names.push_back(name);
      }
    }

    for (const auto& [comp_name, cname] : impl_->component_to_container) {
      (void)cname;
      names.push_back(comp_name);
    }
  } else {
    names = impl_->expand_target_list(target);
  }

  if (names.empty()) {
    res.code = wildcard ? launcher::ErrorCode::kOk : launcher::ErrorCode::kNotFound;
    res.message = wildcard ? "No components to update." : ("Target '" + target + "' not found.");
    return res;
  }

  if (enable && !path.empty() && names.size() > 1U) {
    std::unordered_set<std::string> proc_domains;

    {
      std::lock_guard lock(impl_->state_mtx);

      for (const auto& name : names) {
        auto it = impl_->units.find(name);

        if (it != impl_->units.end() && it->second->status.type == launcher::UnitType::kComponent) {
          proc_domains.insert("__host__");
        } else {
          auto cit = impl_->component_to_container.find(name);

          if (cit != impl_->component_to_container.end()) {
            proc_domains.insert("container:" + cit->second);
          }
        }
      }
    }

    if (proc_domains.size() > 1U) {
      res.code = launcher::ErrorCode::kInvalidArgument;
      res.message =
          "Cannot apply a single record_path across components in distinct OS processes; omit path to auto-allocate.";
      return res;
    }
  }

  size_t updated_count = 0U;

  for (const auto& name : names) {
    auto u = impl_->find_unit(name);

    if (!u) {
      std::string cname;

      {
        std::lock_guard lock(impl_->state_mtx);
        auto it = impl_->component_to_container.find(name);

        if (it != impl_->component_to_container.end()) {
          cname = it->second;
        }
      }

      if (!cname.empty()) {
        auto cu = impl_->find_unit(cname);
        auto cli = impl_->get_or_connect_container_client(cu, 2000U);

        if (!cli || !cli->is_connected()) {
          res.code = launcher::ErrorCode::kDisconnected;
          res.message = "Container '" + cname + "' control plane unavailable.";
          return res;
        }

        const uint64_t cgen = cu ? std::max<uint64_t>(1U, cu->generation.load(std::memory_order_acquire)) : 1U;
        const uint64_t seg =
            (enable && path.empty()) ? (impl_->record_segment_seq.fetch_add(1U, std::memory_order_relaxed) + 1U) : 0U;
        const std::string resolved_path =
            enable ? impl_->resolve_unit_record_path(name, launcher::UnitType::kComponent, path, cgen, seg) : "";

        if (enable) {
          std::string conflict;

          if (!impl_->check_cross_proc_record_collision("container:" + cname, resolved_path, conflict)) {
            res.code = launcher::ErrorCode::kInvalidArgument;
            res.message = "record_path '";
            res.message.append(resolved_path)
                .append("' conflicts with active recording on ")
                .append(conflict)
                .append(" in another OS process.");
            return res;
          }
        }

        auto fwd = cli->set_record(name, enable, resolved_path);

        if (!fwd.ok()) {
          return fwd;
        }

        {
          std::lock_guard lock(impl_->state_mtx);

          if (enable) {
            impl_->remote_record_paths[name] = resolved_path;
          } else {
            impl_->remote_record_paths.erase(name);
          }
        }

        ++updated_count;
        continue;
      }

      res.code = launcher::ErrorCode::kNotFound;
      res.message = "Unit '" + name + "' not found.";
      return res;
    }

    if (u->status.type != launcher::UnitType::kComponent) {
      if (!wildcard && !is_group) {
        res.code = launcher::ErrorCode::kUnsupported;
        res.message =
            "Runtime dynamic recording is supported for components only; configure [record] or "
            "record_path for external processes.";
        return res;
      }

      continue;
    }

    const uint64_t gen = u->generation.load(std::memory_order_acquire);
    const uint64_t seg =
        (enable && path.empty()) ? (impl_->record_segment_seq.fetch_add(1U, std::memory_order_relaxed) + 1U) : 0U;
    const std::string resolved_path =
        enable ? impl_->resolve_unit_record_path(name, launcher::UnitType::kComponent, path, gen, seg) : "";

    if (enable) {
      std::string conflict;

      if (!impl_->check_cross_proc_record_collision("__host__", resolved_path, conflict)) {
        res.code = launcher::ErrorCode::kInvalidArgument;
        res.message = "record_path '";
        res.message.append(resolved_path)
            .append("' conflicts with active recording on ")
            .append(conflict)
            .append(" in another OS process.");
        return res;
      }
    }

    bool force_pub = false;

    {
      std::lock_guard lock(impl_->state_mtx);
      force_pub = enable ? u->comp_spec.record_force_publish.value_or(impl_->active_record_force_publish) : false;
    }

    if (impl_->component_host.has_component(name)) {
      std::string err;

      if (!impl_->component_host.set_component_record(name, resolved_path, force_pub, &err)) {
        res.code = launcher::ErrorCode::kInternalError;
        res.message = err;
        return res;
      }
    }

    {
      std::lock_guard lock(impl_->state_mtx);
      u->comp_spec.record_enabled = enable;
      u->comp_spec.record_path = resolved_path;
      u->active_record_path = resolved_path;
    }

    ++updated_count;
  }

  res.code = launcher::ErrorCode::kOk;
  res.message = std::string(enable ? "Started" : "Stopped") + " recording for " + std::to_string(updated_count) +
                " component(s).";
  return res;
}

launcher::CommandResult LauncherServer::resume_startup() {
  launcher::CommandResult res;
  impl_->paused_on_fail.store(false, std::memory_order_release);
  impl_->pump_startup_queue();
  res.code = launcher::ErrorCode::kOk;
  res.message = "Startup resumed.";
  return res;
}

std::string LauncherServer::get_log_dir() const { return impl_->log_manager.session_dir(); }

size_t LauncherServer::get_max_task_count() const { return 8192U; }

uint32_t LauncherServer::get_max_elapsed_time() const { return 10000U; }

void LauncherServer::on_task_timeout(Callback&& callback, uint32_t elapsed_time) {
  VLOG_W("LauncherServer: Task queued for ", elapsed_time, "ms before execution.");

  if (callback) {
    callback();
  }
}

void LauncherServer::on_begin() {
  MessageLoop::on_begin();

  std::lock_guard lock(impl_->timer_mtx);

  if (!impl_->stopping.load(std::memory_order_acquire)) {
    impl_->housekeep_timer = std::make_unique<Timer>(this, launcher::kHeartbeatIntervalMs, Timer::kInfinite,
                                                     [this] { impl_->run_housekeeping(); });
    impl_->housekeep_timer->start();
  }
}

void LauncherServer::on_end() {
  std::unique_ptr<Timer> hk_timer;

  {
    std::lock_guard lock(impl_->timer_mtx);
    hk_timer.swap(impl_->housekeep_timer);
  }

  if (hk_timer) {
    hk_timer->stop();
    hk_timer.reset();
  }

  MessageLoop::on_end();
}

}  // namespace vlink
