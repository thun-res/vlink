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

#include <nlohmann/json.hpp>
//
#include <vlink/base/condition_variable.h>
#include <vlink/base/deadline_timer.h>
#include <vlink/base/elapsed_timer.h>
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
#include <vlink/external/launcher/module.h>
#include <vlink/external/launcher_api.h>
#include <vlink/external/launcher_server.h>
#include <vlink/impl/url_parser.h>
#include <vlink/publisher.h>
#include <vlink/server.h>
#include <vlink/setter.h>
#include <vlink/subscriber.h>
#include <vlink/version.h>
//
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
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

#if defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
//
#include <wincrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "../launcher_common.h"
#include "./component_host.h"
#include "./launcher_config.h"
#include "./log_manager.h"
#include "./process_supervisor.h"
#include "./resource_control.h"
#include "./scheduler_manager.h"

namespace vlink {

[[nodiscard]] static std::string make_session_id() {
  const std::string hex = Uuid::random_hex();
  const std::string short_id = (hex.size() >= 8U) ? hex.substr(0, 8U) : hex;
  return "s_" + Utils::get_pid_str() + "_" + short_id;
}

[[nodiscard]] static bool is_active_state(launcher::UnitState s) noexcept {
  return s == launcher::UnitState::kStarting || s == launcher::UnitState::kRunning || s == launcher::UnitState::kReady;
}

[[nodiscard]] static bool contains_str(const std::vector<std::string>& list, std::string_view value) noexcept {
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

[[nodiscard]] static std::string find_launcher_executable(const std::string& configured);

[[nodiscard]] static bool url_matches_topic(std::string_view discovered_url, std::string_view expected) noexcept;

#ifdef VLINK_ENABLE_SECURITY
[[nodiscard]] static bool generate_csprng_hex(size_t byte_count, std::string& hex_out);

[[nodiscard]] static Security::Config make_channel_sec_cfg(const std::string& key, std::string_view aad) {
  Security::Config cfg;
  cfg.key = key;
  cfg.advanced.aad_context = std::string(aad);
  return cfg;
}
#endif

template <typename NodeT, typename... ArgsT>
[[nodiscard]] static std::shared_ptr<NodeT> make_server_node(std::string_view scheme, std::string_view bind_ip,
                                                             bool native_mode, ArgsT&&... args) {
  auto node = MemoryResource::make_shared<NodeT>(std::forward<ArgsT>(args)..., InitType::kWithoutInit);
  node->set_discovery_enabled(false);
  node->set_safety_quit(true);
  launcher::apply_control_node_properties(*node, scheme, bind_ip, native_mode);

  if VUNLIKELY (!node->init()) {
    return nullptr;
  }

  return node;
}

struct RemoteDepRef final {
  std::string container;
  std::string component;

  bool operator==(const RemoteDepRef& other) const noexcept {
    return container == other.container && component == other.component;
  }
};

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

  std::vector<RemoteDepRef> remote_order_preds;
  std::vector<RemoteDepRef> remote_req_preds;
  std::vector<RemoteDepRef> remote_binds_preds;

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
  uint64_t container_client_gen{0};
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

  uint64_t last_liveness_check_ms{0};
  uint64_t watchdog_armed_ms{0};
  uint64_t remote_wait_deadline_ms{0};

  int64_t delay_timer_key{-1};
  int64_t ready_timer_key{-1};
  int64_t probe_poll_timer_key{-1};
  int64_t respawn_timer_key{-1};
  int64_t periodic_timer_key{-1};
  int64_t remote_wait_timer_key{-1};
};

struct LauncherServer::Impl final {
  enum class RemoteDepStatus : uint8_t {
    kPending = 0,
    kReady = 1,
    kFailed = 2,
  };

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
  std::unique_ptr<ThreadPool> recovery_pool;
  size_t in_flight_starts{0};
  size_t pending_boot_units{0};
  size_t active_stage_idx{0};
  std::atomic<uint64_t> record_segment_seq{0};
  mutable std::atomic<double> cached_host_cpu{-1.0};
  mutable std::atomic<double> cached_host_mem{-1.0};
  std::thread fallback_stop_thread;

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
  std::mutex recovery_mtx;
  std::recursive_mutex op_mtx;
  std::mutex fallback_stop_mtx;
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
  std::atomic_bool has_local_event_cb{false};
  std::atomic_bool has_local_log_cb{false};
  std::atomic_bool snapshot_dirty{false};
  std::atomic_bool snapshot_flush_pending{false};
  std::atomic_bool stop_executed{false};

  launcher::StartupMode effective_startup_mode{launcher::StartupMode::kParallel};
  bool manifest_loaded{false};
  bool active_native_mode{false};
  std::atomic_bool running{false};
  std::atomic_bool stopping{false};
  std::atomic_bool paused_on_fail{false};
  bool cold_boot_complete{false};
  std::atomic_bool control_active{false};
  std::atomic_bool control_ready{false};
  bool use_security{false};
  bool active_record_enabled{false};
  bool active_record_force_publish{false};

  void rebuild_unit_table();

  std::vector<std::string> expand_target_list(const std::string& target) const;

  std::shared_ptr<UnitRuntime> find_unit(const std::string& name) const;

  [[nodiscard]] size_t total_unit_count_locked() const noexcept;

  [[nodiscard]] RemoteDepStatus evaluate_remote_dep(const std::string& container_name, const std::string& comp_name);

  [[nodiscard]] RemoteDepStatus evaluate_remote_preds(const std::vector<RemoteDepRef>& reqs,
                                                      const std::vector<RemoteDepRef>& orders, std::string& failed_out);

  [[nodiscard]] std::string find_container_for_component(const std::string& comp_name) const;

  std::shared_ptr<LauncherAPI> connect_container_or_error(const std::string& container_name, uint32_t wait_ms,
                                                          launcher::CommandResult& err_out,
                                                          const char* err_prefix = nullptr);

  template <typename OpFnT>
  bool try_forward_remote_component(const std::string& name, launcher::CommandResult& out, OpFnT&& fn) {
    const std::string cname = find_container_for_component(name);

    if (cname.empty()) {
      return false;
    }

    auto cli = connect_container_or_error(cname, 2000U, out);

    if (!cli) {
      return true;
    }

    out = fn(*cli);
    return true;
  }

  void detach_container_client(const std::shared_ptr<UnitRuntime>& unit, bool graceful_shutdown, uint32_t timeout_ms,
                               std::optional<uint64_t> expected_gen = std::nullopt);

  bool commit_parsed_manifest(launcher::LaunchManifest&& parsed, std::string* error_out);

  template <typename CbT, typename PayloadT>
  void post_local_callback(std::shared_ptr<CbT> Impl::* member, const std::atomic_bool& has_flag,
                           const PayloadT& payload) {
    if VLIKELY (!has_flag.load(std::memory_order_acquire) || !owner->is_running()) {
      return;
    }

    std::weak_ptr<std::atomic_bool> weak_alive = alive_token;
    (void)owner->post_task([this, weak_alive, member, payload] {
      auto guard = weak_alive.lock();

      if VUNLIKELY (!guard || !guard->load(std::memory_order_acquire)) {
        return;
      }

      std::shared_ptr<CbT> cb;

      {
        std::lock_guard lock(cb_mtx);
        cb = this->*member;
      }

      if (cb && *cb) {
        (*cb)(payload);
      }
    });
  }

  [[nodiscard]] uint32_t calc_remote_dep_wait_ms(const std::string& container_name, const std::string& comp_name) const;

  [[nodiscard]] bool wait_remote_dep_ready(const std::string& container_name, const std::string& comp_name,
                                           uint32_t wait_ms);

  void arm_remote_dep_wait(const std::shared_ptr<UnitRuntime>& unit);

  void cancel_unit_timers(const std::shared_ptr<UnitRuntime>& unit);

  void release_start_slot(const std::shared_ptr<UnitRuntime>& unit);

  void transition_unit(const std::shared_ptr<UnitRuntime>& unit, launcher::UnitState next_state,
                       std::string_view event_name, std::string_view detail);

  bool transition_unit_if(const std::shared_ptr<UnitRuntime>& unit, launcher::UnitState expected,
                          launcher::UnitState next_state, std::string_view event_name, std::string_view detail);

  bool transition_unit_if_gen(const std::shared_ptr<UnitRuntime>& unit, uint64_t expected_gen,
                              std::optional<launcher::UnitState> expected_state, launcher::UnitState next_state,
                              std::string_view event_name, std::string_view detail,
                              std::optional<std::string_view> failed_reason = std::nullopt);

  bool ensure_and_wait_remote_dep(const std::string& container_name, const std::string& comp_name, uint32_t wait_ms,
                                  bool allow_auto_start);

  bool check_and_record_start_limit(const std::shared_ptr<UnitRuntime>& unit, std::string* err_out);

  void mark_unit_boot_terminal(const std::shared_ptr<UnitRuntime>& unit, bool succeeded);

  void enqueue_ready_unit(const std::shared_ptr<UnitRuntime>& unit);

  void pump_startup_queue();

  void arm_readiness_probe(const std::shared_ptr<UnitRuntime>& unit, uint64_t gen);

  void finish_readiness(const std::shared_ptr<UnitRuntime>& unit, uint64_t gen, bool ok, std::string_view detail);

  bool launch_unit_now(const std::shared_ptr<UnitRuntime>& unit, std::string* error_out,
                       std::optional<bool> start_inactive_override = std::nullopt);

  [[nodiscard]] std::string normalize_record_path(const std::string& p_str) const;

  [[nodiscard]] bool check_cross_proc_record_collision(const std::string& target_domain, const std::string& resolved_p,
                                                       std::string& conflict_out) const;

  bool post_protected(MessageLoop::Callback&& cb) const;

  bool post_recovery_task(ThreadPool::Callback&& cb);

  void offload_fault_recovery(const std::shared_ptr<UnitRuntime>& unit, uint64_t captured_gen, bool is_watchdog,
                              std::string reason, std::string detail);

  std::shared_ptr<DiscoveryViewer> ensure_discovery_viewer();

  std::vector<std::string> compute_topo_order_for_set(const std::unordered_set<std::string>& subset) const;

  std::vector<std::string> collect_stoppable_dependents_locked(const std::string& unit_name,
                                                               bool propagate_part_of) const;

  void cascade_stop_bound_dependents(const std::string& unit_name);

  bool stop_unit_cascade(const std::string& name, std::unordered_set<std::string>& visited, bool propagate_part_of);

  bool stop_unit_internal(const std::shared_ptr<UnitRuntime>& unit,
                          std::optional<uint32_t> timeout_override_ms = std::nullopt);

  void handle_process_exit(const std::string& name, uint64_t gen, int32_t code, Process::ExitStatus status,
                           bool expected_stop);

  void schedule_respawn(const std::shared_ptr<UnitRuntime>& unit, std::optional<uint64_t> expected_gen = std::nullopt,
                        std::optional<launcher::UnitState> expected_state = std::nullopt);

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

  [[nodiscard]] bool is_remote_privileged_allowed() const noexcept;

  launcher::CommandResponse handle_command(const launcher::CommandRequest& req);

  launcher::SystemSnapshot build_snapshot() const;

  void request_async_stop(int32_t code = 0);

  void do_stop();
};

static void push_unique_remote(std::vector<RemoteDepRef>& list, const std::string& container,
                               const std::string& component) {
  if (container.empty() || component.empty()) {
    return;
  }

  RemoteDepRef ref{container, component};

  for (const auto& item : list) {
    if (item == ref) {
      return;
    }
  }

  list.push_back(std::move(ref));
}

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

  auto resolve_remote_refs = [&](const std::string& ref) -> std::vector<RemoteDepRef> {
    std::vector<RemoteDepRef> res;

    if (ref.empty()) {
      return res;
    }

    auto check_one = [&](const std::string& cand) {
      if (units.count(cand) != 0U) {
        return;
      }

      auto cit = component_to_container.find(cand);

      if (cit != component_to_container.end() && units.count(cit->second) != 0U) {
        push_unique_remote(res, cit->second, cand);
      }
    };

    if (ref[0] == '@') {
      auto git = group_members.find(ref.substr(1U));

      if (git != group_members.end()) {
        for (const auto& m : git->second) {
          check_one(m);
        }
      }
    } else {
      check_one(ref);
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

      for (const auto& r : resolve_remote_refs(ref)) {
        if (r.container != name) {
          push_unique_remote(u.remote_order_preds, r.container, r.component);
          push_unique_remote(u.remote_req_preds, r.container, r.component);
        }
      }
    }

    for (const auto& ref : d.after) {
      for (const auto& t : resolve_ref(ref)) {
        if (t != name) {
          add_order(t, name);
        }
      }

      for (const auto& r : resolve_remote_refs(ref)) {
        if (r.container != name) {
          push_unique_remote(u.remote_order_preds, r.container, r.component);
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

      for (const auto& r : resolve_remote_refs(ref)) {
        if (r.container != name) {
          push_unique_remote(u.remote_req_preds, r.container, r.component);
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

      for (const auto& r : resolve_remote_refs(ref)) {
        if (r.container != name) {
          push_unique_remote(u.remote_req_preds, r.container, r.component);
          push_unique_remote(u.remote_binds_preds, r.container, r.component);
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
    std::unordered_set<std::string> seen_preds;

    for (const auto& pred : u->order_preds) {
      if (pred != name && in_degree.count(pred) != 0U && seen_preds.insert(pred).second) {
        ++in_degree[name];
        adj[pred].push_back(name);
      }
    }

    for (const auto& r : u->remote_req_preds) {
      if (r.container != name && in_degree.count(r.container) != 0U && seen_preds.insert(r.container).second) {
        ++in_degree[name];
        adj[r.container].push_back(name);
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

size_t LauncherServer::Impl::total_unit_count_locked() const noexcept {
  return units.size() + component_to_container.size();
}

LauncherServer::Impl::RemoteDepStatus LauncherServer::Impl::evaluate_remote_preds(
    const std::vector<RemoteDepRef>& reqs, const std::vector<RemoteDepRef>& orders, std::string& failed_out) {
  bool any_pending = false;

  for (const auto& r : reqs) {
    const auto st = evaluate_remote_dep(r.container, r.component);

    if (st == RemoteDepStatus::kFailed) {
      failed_out = r.component + " (" + r.container + ")";
      return RemoteDepStatus::kFailed;
    }

    if (st == RemoteDepStatus::kPending) {
      any_pending = true;
    }
  }

  for (const auto& r : orders) {
    if (evaluate_remote_dep(r.container, r.component) == RemoteDepStatus::kPending) {
      any_pending = true;
    }
  }

  return any_pending ? RemoteDepStatus::kPending : RemoteDepStatus::kReady;
}

std::string LauncherServer::Impl::find_container_for_component(const std::string& comp_name) const {
  std::lock_guard lock(state_mtx);
  auto it = component_to_container.find(comp_name);
  return (it != component_to_container.end()) ? it->second : std::string{};
}

std::shared_ptr<LauncherAPI> LauncherServer::Impl::connect_container_or_error(const std::string& container_name,
                                                                              uint32_t wait_ms,
                                                                              launcher::CommandResult& err_out,
                                                                              const char* err_prefix) {
  auto cu = find_unit(container_name);
  auto cli = get_or_connect_container_client(cu, wait_ms);

  if (!cli || !cli->is_connected()) {
    err_out.code = launcher::ErrorCode::kDisconnected;
    err_out.message = (err_prefix != nullptr ? std::string(err_prefix) : std::string{}) + "Container '" +
                      container_name + "' control plane unavailable.";
    return nullptr;
  }

  return cli;
}

void LauncherServer::Impl::detach_container_client(const std::shared_ptr<UnitRuntime>& unit, bool graceful_shutdown,
                                                   uint32_t timeout_ms, std::optional<uint64_t> expected_gen) {
  if (!unit || unit->status.type != launcher::UnitType::kContainer) {
    return;
  }

  std::shared_ptr<LauncherAPI> cli;

  {
    std::lock_guard lock(state_mtx);

    if (!expected_gen.has_value() || unit->generation.load(std::memory_order_acquire) == *expected_gen) {
      cli = std::move(unit->container_client);
      unit->container_client_gen = 0U;
    }
  }

  if (!cli) {
    return;
  }

  if (graceful_shutdown && cli->is_connected() && timeout_ms > 50U) {
    (void)cli->shutdown_server(std::min<uint32_t>(1500U, timeout_ms));
  }

  cli->quit(true);
  cli->wait_for_quit(static_cast<int>(timeout_ms));
}

bool LauncherServer::Impl::commit_parsed_manifest(launcher::LaunchManifest&& parsed, std::string* error_out) {
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

      *error_out = std::move(joined);
    }

    return false;
  }

  manifest = std::move(parsed);
  manifest_loaded = true;
  rebuild_unit_table();
  return true;
}

LauncherServer::Impl::RemoteDepStatus LauncherServer::Impl::evaluate_remote_dep(const std::string& container_name,
                                                                                const std::string& comp_name) {
  auto cu = find_unit(container_name);

  if (!cu) {
    return RemoteDepStatus::kFailed;
  }

  launcher::UnitState cstate = launcher::UnitState::kPending;
  int64_t expected_pid = -1;
  bool has_launch_prefix = false;
  std::shared_ptr<LauncherAPI> cli;

  {
    std::lock_guard lock(state_mtx);
    cstate = cu->status.state;
    expected_pid = cu->status.pid;
    has_launch_prefix = !cu->container_spec.launch_prefix.empty();
    cli = cu->container_client;
  }

  if (cstate == launcher::UnitState::kFailed || cstate == launcher::UnitState::kStopped ||
      cstate == launcher::UnitState::kSkipped) {
    return RemoteDepStatus::kFailed;
  }

  if (cstate != launcher::UnitState::kReady) {
    return RemoteDepStatus::kPending;
  }

  if (!cli) {
    cli = get_or_connect_container_client(cu, 0U);
  }

  if (!cli) {
    return RemoteDepStatus::kPending;
  }

  auto snap = cli->get_cached_snapshot();

  if (!snap.has_value() || snap->session_id.empty()) {
    return RemoteDepStatus::kPending;
  }

  if (!has_launch_prefix && expected_pid > 0 && snap->server_pid > 0 && snap->server_pid != expected_pid) {
    return RemoteDepStatus::kPending;
  }

  for (const auto& u : snap->units) {
    if (u.name == comp_name) {
      if (u.state == launcher::UnitState::kReady || u.state == launcher::UnitState::kInactive) {
        return RemoteDepStatus::kReady;
      }

      if (u.state == launcher::UnitState::kFailed || u.state == launcher::UnitState::kStopped ||
          u.state == launcher::UnitState::kSkipped) {
        return RemoteDepStatus::kFailed;
      }

      return RemoteDepStatus::kPending;
    }
  }

  return RemoteDepStatus::kFailed;
}

uint32_t LauncherServer::Impl::calc_remote_dep_wait_ms(const std::string& container_name,
                                                       const std::string& comp_name) const {
  uint32_t budget = 30000U;

  for (const auto& cs : manifest.components) {
    if (cs.name == comp_name && (container_name.empty() || cs.container == container_name)) {
      const uint64_t comp_budget =
          static_cast<uint64_t>(cs.ready.timeout_ms) + static_cast<uint64_t>(cs.delay_ms) + 5000ULL;
      budget = static_cast<uint32_t>(std::max<uint64_t>(budget, std::min<uint64_t>(comp_budget, 3600000ULL)));
      break;
    }
  }

  return budget;
}

bool LauncherServer::Impl::wait_remote_dep_ready(const std::string& container_name, const std::string& comp_name,
                                                 uint32_t wait_ms) {
  const uint64_t deadline = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli) + std::max<uint32_t>(wait_ms, 1000U);

  while (!stopping.load(std::memory_order_acquire)) {
    const auto st = evaluate_remote_dep(container_name, comp_name);

    if (st == RemoteDepStatus::kReady) {
      return true;
    }

    if (st == RemoteDepStatus::kFailed) {
      return false;
    }

    const uint64_t now = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);

    if (now >= deadline) {
      return false;
    }

    std::unique_lock lock(state_mtx);
    (void)state_cv.wait_for(lock, std::chrono::milliseconds(std::min<uint64_t>(100ULL, deadline - now)));
  }

  return false;
}

void LauncherServer::Impl::arm_remote_dep_wait(const std::shared_ptr<UnitRuntime>& unit) {
  if (!unit || !wheel_timer || !wheel_timer->is_running()) {
    return;
  }

  std::weak_ptr<std::atomic_bool> weak_alive = alive_token;
  const std::string uname = unit->status.name;
  const uint64_t now = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);

  {
    std::lock_guard lock(state_mtx);

    if (unit->remote_wait_timer_key >= 0) {
      return;
    }

    if (unit->remote_wait_deadline_ms == 0U) {
      uint32_t wait_budget = 30000U;

      for (const auto& r : unit->remote_req_preds) {
        wait_budget = std::max(wait_budget, calc_remote_dep_wait_ms(r.container, r.component));
      }

      for (const auto& r : unit->remote_order_preds) {
        wait_budget = std::max(wait_budget, calc_remote_dep_wait_ms(r.container, r.component));
      }

      unit->remote_wait_deadline_ms = now + static_cast<uint64_t>(wait_budget);
    }
  }

  const int64_t key = wheel_timer->add(
      50U,
      [this, weak_alive, uname](WheelTimer::Key k) {
        auto guard = weak_alive.lock();

        if (!guard || !guard->load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire)) {
          if (wheel_timer) {
            (void)wheel_timer->remove(k);
          }

          return;
        }

        (void)post_protected([this, weak_alive, uname, k] {
          auto g2 = weak_alive.lock();

          if (!g2 || !g2->load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire)) {
            if (wheel_timer) {
              (void)wheel_timer->remove(k);
            }

            return;
          }

          auto u = find_unit(uname);

          if (!u) {
            if (wheel_timer) {
              (void)wheel_timer->remove(k);
            }

            return;
          }

          std::vector<RemoteDepRef> order_refs;
          std::vector<RemoteDepRef> req_refs;
          uint64_t deadline = 0U;

          bool stale = false;

          {
            std::lock_guard lock(state_mtx);

            if (u->boot_terminal || is_active_state(u->status.state)) {
              if (u->remote_wait_timer_key == k) {
                u->remote_wait_timer_key = -1;
              }

              stale = true;
            } else {
              order_refs = u->remote_order_preds;
              req_refs = u->remote_req_preds;
              deadline = u->remote_wait_deadline_ms;
            }
          }

          if (stale) {
            if (wheel_timer) {
              (void)wheel_timer->remove(k);
            }

            return;
          }

          std::string failed_comp;
          const auto remote_st = evaluate_remote_preds(req_refs, order_refs, failed_comp);
          const bool any_pending = (remote_st == RemoteDepStatus::kPending);
          const bool timed_out =
              (failed_comp.empty() && any_pending && ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli) >= deadline);

          if (!failed_comp.empty() || timed_out || !any_pending) {
            {
              std::lock_guard lock(state_mtx);

              if (u->remote_wait_timer_key == k) {
                u->remote_wait_timer_key = -1;
              }

              u->remote_wait_deadline_ms = 0U;

              if (!failed_comp.empty() || timed_out) {
                u->boot_failed = true;
                u->status.failed_reason = "dependency_failed";
              }
            }

            if (wheel_timer) {
              (void)wheel_timer->remove(k);
            }

            if (!failed_comp.empty()) {
              transition_unit(u, launcher::UnitState::kSkipped, "on_dep_failed",
                              "required container component failed: " + failed_comp);
              mark_unit_boot_terminal(u, false);
              return;
            }

            if (timed_out) {
              transition_unit(u, launcher::UnitState::kSkipped, "on_dep_failed",
                              "timed out waiting for container component dependency");
              mark_unit_boot_terminal(u, false);
              return;
            }

            enqueue_ready_unit(u);
          }
        });
      },
      50U);

  bool stale = false;

  {
    std::lock_guard lock(state_mtx);

    if (unit->boot_terminal || stopping.load(std::memory_order_acquire) || unit->remote_wait_timer_key >= 0) {
      stale = true;
    } else {
      unit->remote_wait_timer_key = key;
    }
  }

  if (stale && wheel_timer) {
    (void)wheel_timer->remove(key);
  }
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
  std::vector<RemoteDepRef> remote_reqs;
  std::vector<RemoteDepRef> remote_orders;

  {
    std::lock_guard lock(state_mtx);

    for (const auto& dep_name : unit->req_preds) {
      auto it = units.find(dep_name);

      if (it == units.end()) {
        continue;
      }

      const auto dep_st = it->second->status.state;
      const bool dep_bad = (dep_st == launcher::UnitState::kFailed || dep_st == launcher::UnitState::kSkipped ||
                            it->second->boot_failed);

      if (dep_bad) {
        failed_dep = dep_name;
        unit->boot_failed = true;
        unit->status.failed_reason = "dependency_failed";
        break;
      }
    }

    remote_reqs = unit->remote_req_preds;
    remote_orders = unit->remote_order_preds;
  }

  if (!failed_dep.empty()) {
    transition_unit(unit, launcher::UnitState::kSkipped, "on_dep_failed", "required dependency failed: " + failed_dep);
    mark_unit_boot_terminal(unit, false);
    return;
  }

  if (!remote_reqs.empty() || !remote_orders.empty()) {
    std::string failed_remote;
    const auto remote_st = evaluate_remote_preds(remote_reqs, remote_orders, failed_remote);

    if (remote_st == RemoteDepStatus::kFailed) {
      {
        std::lock_guard lock(state_mtx);
        unit->boot_failed = true;
        unit->status.failed_reason = "dependency_failed";
      }

      transition_unit(unit, launcher::UnitState::kSkipped, "on_dep_failed",
                      "required container component failed: " + failed_remote);
      mark_unit_boot_terminal(unit, false);
      return;
    }

    if (remote_st == RemoteDepStatus::kPending) {
      arm_remote_dep_wait(unit);
      return;
    }
  }

  if (unit->delay_ms > 0U && wheel_timer && wheel_timer->is_running()) {
    std::weak_ptr<std::atomic_bool> weak_alive = alive_token;
    const std::string uname = unit->status.name;
    const uint64_t expected_gen = unit->generation.load(std::memory_order_acquire);
    const int64_t key = wheel_timer->add(unit->delay_ms, [this, weak_alive, uname, expected_gen](WheelTimer::Key k) {
      auto guard = weak_alive.lock();

      if (!guard || !guard->load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire)) {
        return;
      }

      (void)post_protected([this, weak_alive, uname, expected_gen, k] {
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

          if (u->generation.load(std::memory_order_acquire) != expected_gen || u->boot_terminal ||
              u->status.state != launcher::UnitState::kPending) {
            return;
          }

          if (u->delay_timer_key == k) {
            u->delay_timer_key = -1;
          }

          startup_queue.push_back(uname);
        }

        pump_startup_queue();
      });
    });

    bool stale = false;
    int64_t prev_key = -1;

    {
      std::lock_guard lock(state_mtx);

      if (unit->generation.load(std::memory_order_acquire) != expected_gen || unit->boot_terminal ||
          unit->status.state != launcher::UnitState::kPending || stopping.load(std::memory_order_acquire)) {
        stale = true;
      } else {
        prev_key = unit->delay_timer_key;
        unit->delay_timer_key = key;
      }
    }

    if (prev_key >= 0 && wheel_timer) {
      (void)wheel_timer->remove(prev_key);
    }

    if (stale && wheel_timer) {
      (void)wheel_timer->remove(key);
    }

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
                      cascade_stop_bound_dependents(u->status.name);
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

std::vector<std::vector<std::string>> LauncherServer::Impl::compute_reverse_topological_layers() const {
  std::lock_guard lock(state_mtx);
  std::vector<std::vector<std::string>> layers;

  std::vector<int32_t> desc_stages = sorted_stages;
  std::reverse(desc_stages.begin(), desc_stages.end());

  for (const int32_t cur_stage : desc_stages) {
    std::vector<std::string> stage_list;

    for (const auto& name : unit_order) {
      if (units.at(name)->status.stage == cur_stage) {
        stage_list.push_back(name);
      }
    }

    std::unordered_map<std::string, size_t> out_degree;
    std::unordered_map<std::string, std::vector<std::string>> rev_edges;

    for (const auto& name : stage_list) {
      out_degree[name] = 0U;
    }

    for (const auto& name : stage_list) {
      const auto& u = units.at(name);

      for (const auto& pred : u->order_preds) {
        if (out_degree.count(pred) != 0U) {
          ++out_degree[pred];
          rev_edges[name].push_back(pred);
        }
      }
    }

    std::unordered_set<std::string> emitted;

    while (emitted.size() < stage_list.size()) {
      std::vector<std::string> layer;

      for (size_t idx = stage_list.size(); idx > 0U; --idx) {
        const auto& name = stage_list[idx - 1U];

        if (emitted.count(name) == 0U && out_degree[name] == 0U) {
          layer.push_back(name);
        }
      }

      if (layer.empty()) {
        for (size_t idx = stage_list.size(); idx > 0U; --idx) {
          const auto& name = stage_list[idx - 1U];

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

bool LauncherServer::Impl::post_protected(MessageLoop::Callback&& cb) const {
  PostTaskOptions opts;
  opts.drop_policy = TaskDropPolicy::kProtected;
  const auto handle = owner->post_task_handle(std::move(cb), opts);
  const auto st = handle.state();
  return st != TaskExecutionState::kRejected && st != TaskExecutionState::kCancelled &&
         st != TaskExecutionState::kInvalid;
}

bool LauncherServer::Impl::post_recovery_task(ThreadPool::Callback&& cb) {
  if (!cb) {
    return false;
  }

  std::lock_guard lock(recovery_mtx);

  if (recovery_pool && !stopping.load(std::memory_order_acquire)) {
    return recovery_pool->post_task(std::move(cb));
  }

  return false;
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

void LauncherServer::Impl::cancel_unit_timers(const std::shared_ptr<UnitRuntime>& unit) {
  if (!unit || !wheel_timer) {
    return;
  }

  int64_t k_delay = -1;
  int64_t k_ready = -1;
  int64_t k_poll = -1;
  int64_t k_respawn = -1;
  int64_t k_periodic = -1;
  int64_t k_remote = -1;

  {
    std::lock_guard lock(state_mtx);
    k_delay = unit->delay_timer_key;
    k_ready = unit->ready_timer_key;
    k_poll = unit->probe_poll_timer_key;
    k_respawn = unit->respawn_timer_key;
    k_periodic = unit->periodic_timer_key;
    k_remote = unit->remote_wait_timer_key;
    unit->delay_timer_key = -1;
    unit->ready_timer_key = -1;
    unit->probe_poll_timer_key = -1;
    unit->respawn_timer_key = -1;
    unit->periodic_timer_key = -1;
    unit->remote_wait_timer_key = -1;
    unit->remote_wait_deadline_ms = 0U;
  }

  for (const int64_t k : {k_delay, k_ready, k_poll, k_respawn, k_periodic, k_remote}) {
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

static void apply_transition_locked(const std::shared_ptr<UnitRuntime>& unit, uint64_t server_start_steady_ms,
                                    launcher::UnitState next_state, std::string_view event_name,
                                    std::string_view detail, launcher::EventRecord& ev_out) {
  ev_out.timestamp_ms = ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMilli);
  ev_out.unit_name = unit->status.name;
  ev_out.unit_type = unit->status.type;
  ev_out.old_state = unit->status.state;
  ev_out.new_state = next_state;
  ev_out.event_name = std::string(event_name);
  ev_out.message = std::string(detail);

  unit->status.state = next_state;
  unit->status.detail = std::string(detail);

  const uint64_t now_steady = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);
  const uint64_t offset = (now_steady >= server_start_steady_ms) ? (now_steady - server_start_steady_ms) : 0U;

  if (next_state == launcher::UnitState::kStarting) {
    unit->status.start_time_ms = ev_out.timestamp_ms;
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

void LauncherServer::Impl::transition_unit(const std::shared_ptr<UnitRuntime>& unit, launcher::UnitState next_state,
                                           std::string_view event_name, std::string_view detail) {
  launcher::EventRecord ev;

  {
    std::lock_guard lock(state_mtx);
    apply_transition_locked(unit, server_start_steady_ms, next_state, event_name, detail, ev);
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

    apply_transition_locked(unit, server_start_steady_ms, next_state, event_name, detail, ev);
  }

  state_cv.notify_all();
  publish_event(ev);
  schedule_snapshot_publish();
  return true;
}

bool LauncherServer::Impl::transition_unit_if_gen(const std::shared_ptr<UnitRuntime>& unit, uint64_t expected_gen,
                                                  std::optional<launcher::UnitState> expected_state,
                                                  launcher::UnitState next_state, std::string_view event_name,
                                                  std::string_view detail,
                                                  std::optional<std::string_view> failed_reason) {
  launcher::EventRecord ev;

  {
    std::lock_guard lock(state_mtx);

    if (unit->generation.load(std::memory_order_acquire) != expected_gen) {
      return false;
    }

    if (expected_state.has_value() && unit->status.state != *expected_state) {
      return false;
    }

    if (failed_reason.has_value()) {
      unit->status.failed_reason = std::string(*failed_reason);
    }

    apply_transition_locked(unit, server_start_steady_ms, next_state, event_name, detail, ev);
  }

  state_cv.notify_all();
  publish_event(ev);
  schedule_snapshot_publish();
  return true;
}

bool LauncherServer::Impl::ensure_and_wait_remote_dep(const std::string& container_name, const std::string& comp_name,
                                                      uint32_t wait_ms, bool allow_auto_start) {
  const uint64_t deadline = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli) + static_cast<uint64_t>(wait_ms);
  bool start_attempted = false;

  while (!stopping.load(std::memory_order_acquire)) {
    const auto st = evaluate_remote_dep(container_name, comp_name);

    if (st == RemoteDepStatus::kReady) {
      return true;
    }

    if (allow_auto_start && !start_attempted) {
      auto cu = find_unit(container_name);
      launcher::UnitState cstate = launcher::UnitState::kPending;

      if (cu) {
        std::lock_guard lock(state_mtx);
        cstate = cu->status.state;
      }

      if (cstate == launcher::UnitState::kReady) {
        auto cli = get_or_connect_container_client(cu, 2000U);

        if (cli && cli->is_connected()) {
          auto csnap = cli->get_cached_snapshot();

          if (csnap.has_value()) {
            for (const auto& cu_st : csnap->units) {
              if (cu_st.name == comp_name &&
                  (cu_st.state == launcher::UnitState::kStopped || cu_st.state == launcher::UnitState::kSkipped ||
                   cu_st.state == launcher::UnitState::kPending)) {
                start_attempted = true;

                if (cli->start_unit(comp_name).ok()) {
                  const uint64_t sync_deadline =
                      std::min<uint64_t>(deadline, ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli) + 1000ULL);

                  while (!stopping.load(std::memory_order_acquire) &&
                         evaluate_remote_dep(container_name, comp_name) == RemoteDepStatus::kFailed) {
                    const uint64_t now_s = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);

                    if (now_s >= sync_deadline) {
                      break;
                    }

                    std::unique_lock lk(state_mtx);
                    (void)state_cv.wait_for(lk,
                                            std::chrono::milliseconds(std::min<uint64_t>(50ULL, sync_deadline - now_s)),
                                            [&]() { return stopping.load(std::memory_order_acquire); });
                  }
                }

                break;
              }
            }
          }
        }
      }
    }

    if (evaluate_remote_dep(container_name, comp_name) == RemoteDepStatus::kReady) {
      return true;
    }

    if (evaluate_remote_dep(container_name, comp_name) == RemoteDepStatus::kFailed) {
      return false;
    }

    const uint64_t now_ms = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);

    if (now_ms >= deadline) {
      return false;
    }

    std::unique_lock lk(state_mtx);
    (void)state_cv.wait_for(lk, std::chrono::milliseconds(std::min<uint64_t>(50ULL, deadline - now_ms)),
                            [&]() { return stopping.load(std::memory_order_acquire); });
  }

  return false;
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
  int64_t old_ready_key = -1;
  int64_t old_poll_key = -1;
  bool should_continue = false;

  {
    std::lock_guard lock(state_mtx);

    if (unit->generation.load(std::memory_order_acquire) == gen) {
      old_ready_key = unit->ready_timer_key;
      unit->ready_timer_key = -1;
      old_poll_key = unit->probe_poll_timer_key;
      unit->probe_poll_timer_key = -1;

      const auto st = unit->status.state;

      if (st == launcher::UnitState::kStarting || st == launcher::UnitState::kRunning) {
        should_continue = true;

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
    }
  }

  if (old_ready_key >= 0 && wheel_timer) {
    (void)wheel_timer->remove(old_ready_key);
  }

  if (old_poll_key >= 0 && wheel_timer) {
    (void)wheel_timer->remove(old_poll_key);
  }

  if (!should_continue) {
    return;
  }

  if (ok) {
    state_cv.notify_all();
    publish_event(ready_ev);
    schedule_snapshot_publish();
    release_start_slot(unit);
    mark_unit_boot_terminal(unit, true);
    trigger_units(unit->on_ready_trigger);
  } else {
    const uint64_t recovery_gen = gen + 1U;
    std::weak_ptr<std::atomic_bool> weak_alive = alive_token;
    std::string detail_copy(detail);
    const bool posted = post_recovery_task([this, weak_alive, unit, gen, recovery_gen, detail_copy] {
      auto guard = weak_alive.lock();

      if (!guard || !guard->load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire)) {
        release_start_slot(unit);
        return;
      }

      launcher::ComponentStopResult stop_res = launcher::ComponentStopResult::kStoppedClean;

      if (unit->status.type == launcher::UnitType::kComponent) {
        stop_res =
            component_host.stop_component(unit->status.name, std::min<uint32_t>(unit->sigterm_timeout_ms, 2000U), gen);
      } else {
        detach_container_client(unit, false, 500U, recovery_gen);
        (void)process_supervisor.stop_process(unit->status.name, 500U, gen);
      }

      bool stale = false;

      {
        std::lock_guard lock(state_mtx);

        if (unit->generation.load(std::memory_order_acquire) != recovery_gen ||
            unit->status.state != launcher::UnitState::kStopping) {
          stale = true;
        } else {
          unit->active_record_path.clear();
        }
      }

      if (stale) {
        release_start_slot(unit);
        return;
      }

      const bool poisoned_shared = (unit->status.type == launcher::UnitType::kComponent &&
                                    stop_res == launcher::ComponentStopResult::kTimedOutQuarantined &&
                                    !scheduler.is_dedicated_group(unit->status.group));

      const bool should_restart =
          !poisoned_shared && !config.disable_respawn && !stopping.load(std::memory_order_acquire) &&
          (unit->on_exit == launcher::OnExitAction::kRestart ||
           (unit->respawn.enabled && (unit->respawn.policy == launcher::RestartPolicy::kAlways ||
                                      unit->respawn.policy == launcher::RestartPolicy::kOnFailure ||
                                      unit->respawn.policy == launcher::RestartPolicy::kOnAbnormal)));

      cascade_stop_bound_dependents(unit->status.name);

      if (should_restart) {
        schedule_respawn(unit, recovery_gen, launcher::UnitState::kStopping);
        release_start_slot(unit);
        return;
      }

      const std::optional<std::string_view> fail_reason =
          poisoned_shared ? std::optional<std::string_view>("watchdog_executor_hung") : std::nullopt;

      if (!transition_unit_if_gen(unit, recovery_gen, launcher::UnitState::kStopping, launcher::UnitState::kFailed,
                                  "on_ready_failed", detail_copy, fail_reason)) {
        release_start_slot(unit);
        return;
      }

      mark_unit_boot_terminal(unit, false);
      release_start_slot(unit);
      trigger_units(unit->on_error_trigger);

      if (unit->status.required) {
        request_async_stop(1);
      }
    });

    if VUNLIKELY (!posted) {
      if (transition_unit_if_gen(unit, recovery_gen, launcher::UnitState::kStopping, launcher::UnitState::kFailed,
                                 "on_ready_failed", detail)) {
        mark_unit_boot_terminal(unit, false);
      }

      release_start_slot(unit);
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
            bool active_probe = false;

            if (u) {
              std::lock_guard lock(state_mtx);
              const auto st = u->status.state;
              active_probe = (u->generation.load(std::memory_order_acquire) == gen) &&
                             (st == launcher::UnitState::kStarting || st == launcher::UnitState::kRunning);
            }

            if (!active_probe) {
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
    int64_t prev_poll_key = -1;

    {
      std::lock_guard lock(state_mtx);
      const auto st = unit->status.state;

      if (unit->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire) ||
          (st != launcher::UnitState::kStarting && st != launcher::UnitState::kRunning)) {
        stale = true;
      } else {
        prev_poll_key = unit->probe_poll_timer_key;
        unit->probe_poll_timer_key = poll_key;
      }
    }

    if (prev_poll_key >= 0 && wheel_timer) {
      (void)wheel_timer->remove(prev_poll_key);
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
    int64_t prev_ready_key = -1;

    {
      std::lock_guard lock(state_mtx);
      const auto st = unit->status.state;

      if (unit->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire) ||
          (st != launcher::UnitState::kStarting && st != launcher::UnitState::kRunning)) {
        stale = true;
      } else {
        prev_ready_key = unit->ready_timer_key;
        unit->ready_timer_key = key;
      }
    }

    if (prev_ready_key >= 0 && wheel_timer) {
      (void)wheel_timer->remove(prev_ready_key);
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
    int64_t prev_ready_key = -1;

    {
      std::lock_guard lock(state_mtx);
      const auto st = unit->status.state;

      if (unit->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire) ||
          (st != launcher::UnitState::kStarting && st != launcher::UnitState::kRunning)) {
        stale = true;
      } else {
        prev_ready_key = unit->ready_timer_key;
        unit->ready_timer_key = timeout_key;
      }
    }

    if (prev_ready_key >= 0 && wheel_timer) {
      (void)wheel_timer->remove(prev_ready_key);
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
          bool active_probe = false;

          if (u_chk) {
            std::lock_guard lock(state_mtx);
            const auto st = u_chk->status.state;
            active_probe = (u_chk->generation.load(std::memory_order_acquire) == gen) &&
                           (st == launcher::UnitState::kStarting || st == launcher::UnitState::kRunning);
          }

          if (!active_probe) {
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
    int64_t prev_poll_key = -1;

    {
      std::lock_guard lock(state_mtx);
      const auto st = unit->status.state;

      if (unit->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire) ||
          (st != launcher::UnitState::kStarting && st != launcher::UnitState::kRunning)) {
        stale = true;
      } else {
        prev_poll_key = unit->probe_poll_timer_key;
        unit->probe_poll_timer_key = poll_key;
      }
    }

    if (prev_poll_key >= 0 && wheel_timer) {
      (void)wheel_timer->remove(prev_poll_key);
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
  std::string norm_str = Helpers::path_to_string(norm_p);
#if defined(_WIN32) || defined(_WIN64)
  for (char& ch : norm_str) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
#endif
  return norm_str;
}

bool LauncherServer::Impl::check_cross_proc_record_collision(const std::string& target_domain,
                                                             const std::string& resolved_p,
                                                             std::string& conflict_out) const {
  const std::string norm = normalize_record_path(resolved_p);

  if (norm.empty()) {
    return true;
  }

  struct RecordCandidate final {
    std::string label;
    std::string path;
  };

  std::vector<RecordCandidate> candidates;

  {
    std::lock_guard lock(state_mtx);
    candidates.reserve(units.size() + remote_record_paths.size());

    for (const auto& [uname, unit_ptr] : units) {
      std::string dom;
      std::string label;
      std::string static_path;
      bool allow_static = false;
      bool check_unit = false;

      if (unit_ptr->status.type == launcher::UnitType::kComponent) {
        dom = "__host__";
        label = "Component '" + uname + "'";
        static_path = unit_ptr->comp_spec.record_path;
        allow_static = unit_ptr->comp_spec.record_enabled.value_or(!static_path.empty());
        check_unit = (target_domain != dom);
      } else if (unit_ptr->status.type == launcher::UnitType::kContainer) {
        dom = "container:" + uname;
        label = "Container '" + uname + "'";
        static_path = unit_ptr->container_spec.record_path;
        allow_static = unit_ptr->container_spec.record_enabled.value_or(!static_path.empty());
        check_unit = (target_domain != dom && is_active_state(unit_ptr->status.state));
      } else if (unit_ptr->status.type == launcher::UnitType::kProcess) {
        dom = "process:" + uname;
        label = "Process '" + uname + "'";
        static_path = unit_ptr->proc_spec.record_path;
        allow_static = unit_ptr->proc_spec.record_enabled.value_or(!static_path.empty());
        check_unit = (target_domain != dom && is_active_state(unit_ptr->status.state));
      }

      if (!check_unit) {
        continue;
      }

      if (allow_static && !static_path.empty()) {
        candidates.push_back(RecordCandidate{label, std::move(static_path)});
      }

      if (!unit_ptr->active_record_path.empty()) {
        candidates.push_back(RecordCandidate{std::move(label), unit_ptr->active_record_path});
      }
    }

    for (const auto& [comp_name, rec_p] : remote_record_paths) {
      const auto cit = component_to_container.find(comp_name);

      if (cit != component_to_container.end() && !rec_p.empty() && target_domain != ("container:" + cit->second)) {
        candidates.push_back(
            RecordCandidate{"Component '" + comp_name + "' in container '" + cit->second + "'", rec_p});
      }
    }
  }

  for (const auto& cand : candidates) {
    if (normalize_record_path(cand.path) == norm) {
      conflict_out = cand.label;
      return false;
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
  launcher::RecordConfig eff_record;
  eff_record.enabled = !container_proc_rec && active_record_enabled;
  eff_record.dir = active_record_dir;
  eff_record.format = active_record_format;
  eff_record.tag = active_record_tag;
  eff_record.force_publish = active_record_force_publish;

  const bool ctrl_on = config.enable_control_plane && manifest.control_enabled;
  const std::string toml_str = launcher::serialize_container_manifest(
      manifest, unit->container_spec, comps, active_ctrl_scheme, unit->container_ctrl_ns, active_ctrl_domain,
      active_bind_ip, active_native_mode, ctrl_on, config.allow_remote_load || manifest.allow_remote_load,
      &effective_logger_cfg, &eff_record);

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
  if (!container_unit || stopping.load(std::memory_order_acquire)) {
    return nullptr;
  }

  std::shared_ptr<LauncherAPI> cli;
  std::string ctrl_ns;
  uint64_t expected_gen = 0U;

  {
    std::lock_guard lock(state_mtx);

    if (container_unit->container_ctrl_ns.empty()) {
      return nullptr;
    }

    expected_gen = container_unit->generation.load(std::memory_order_acquire);

    if (container_unit->container_client && container_unit->container_client_gen == expected_gen) {
      cli = container_unit->container_client;
    } else {
      ctrl_ns = container_unit->container_ctrl_ns;
    }
  }

  if (!cli) {
    LauncherAPI::Config cfg;
    cfg.scheme = active_ctrl_scheme;
    cfg.namespace_prefix = ctrl_ns;
    cfg.domain_id = active_ctrl_domain;
    cfg.security_key = active_security_key;
    cfg.bind_ip = active_bind_ip;
    cfg.native_mode = active_native_mode;
    cfg.subscribe_events = false;
    cfg.subscribe_logs = false;
    cfg.default_timeout_ms = 3000U;

    auto candidate = MemoryResource::make_shared<LauncherAPI>(cfg);
    std::weak_ptr<std::atomic_bool> weak_alive = alive_token;
    candidate->register_snapshot_callback([this, weak_alive](const launcher::SystemSnapshot&) {
      auto guard = weak_alive.lock();

      if (guard && guard->load(std::memory_order_acquire)) {
        state_cv.notify_all();
      }
    });
    candidate->async_run();
    std::shared_ptr<LauncherAPI> discarded;

    {
      std::lock_guard lock(state_mtx);

      if (stopping.load(std::memory_order_acquire) ||
          container_unit->generation.load(std::memory_order_acquire) != expected_gen) {
        discarded = std::move(candidate);
      } else if (container_unit->container_client && container_unit->container_client_gen == expected_gen) {
        cli = container_unit->container_client;
        discarded = std::move(candidate);
      } else {
        discarded = std::move(container_unit->container_client);
        container_unit->container_client = candidate;
        container_unit->container_client_gen = expected_gen;
        cli = std::move(candidate);
      }
    }

    if (discarded) {
      discarded->quit(true);
      discarded->wait_for_quit(500);
    }
  }

  if (cli && wait_ms > 0U && !cli->is_connected()) {
    (void)cli->wait_for_connected(wait_ms);
  }

  return cli;
}

bool LauncherServer::Impl::launch_unit_now(const std::shared_ptr<UnitRuntime>& unit, std::string* error_out,
                                           std::optional<bool> start_inactive_override) {
  if (!unit || stopping.load(std::memory_order_acquire)) {
    if (error_out != nullptr) {
      *error_out = "Server is stopping.";
    }

    return false;
  }

  if (!check_and_record_start_limit(unit, error_out)) {
    transition_unit(unit, launcher::UnitState::kFailed, "on_start_limit",
                    error_out != nullptr ? *error_out : "start limit exceeded");
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
        const bool has_pending = (it->second->respawn_timer_key >= 0 || it->second->delay_timer_key >= 0 ||
                                  st == launcher::UnitState::kRestarting || st == launcher::UnitState::kStopping ||
                                  is_active_state(st) || st == launcher::UnitState::kInactive);

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

  trigger_units(unit->on_start_trigger);

  if (unit->status.type == launcher::UnitType::kComponent) {
    std::string comp_err;
    const bool start_inactive = start_inactive_override.value_or(unit->start_inactive);
    const bool initial_active = !start_inactive;
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

    const uint32_t start_budget = std::max<uint32_t>(5000U, unit->ready.timeout_ms);

    if (!component_host.start_component(effective_spec, gen, &comp_err, initial_active, start_budget)) {
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

    if (unit->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire)) {
      (void)component_host.stop_component(effective_spec.name, 500U, gen);

      if (error_out != nullptr) {
        *error_out = "Component '" + effective_spec.name + "' was stopped during startup.";
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
  bool already_exited = false;

  {
    std::lock_guard lock(state_mtx);

    if (unit->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire)) {
      stale_after_start = true;
    } else if (unit->status.state != launcher::UnitState::kStarting) {
      already_exited = true;
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

  if (already_exited) {
    return true;
  }

  const bool moved_running =
      transition_unit_if(unit, launcher::UnitState::kStarting, launcher::UnitState::kRunning, "on_running", "running");

  if (unit->status.type == launcher::UnitType::kTask) {
    if (unit->task_spec.kind == launcher::TaskKind::kPeriodic) {
      finish_readiness(unit, gen, true, "periodic task armed");
      arm_periodic_run_timeout(unit, gen);
      schedule_periodic_task(unit, gen);
    } else if (moved_running && unit->task_spec.timeout_ms > 0U && wheel_timer && wheel_timer->is_running()) {
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

              if (!u) {
                return;
              }

              uint64_t expected = gen;

              if (process_supervisor.is_running(uname) &&
                  u->generation.compare_exchange_strong(expected, gen + 1U, std::memory_order_acq_rel)) {
                (void)process_supervisor.stop_process(uname, 500U, gen);

                {
                  std::lock_guard lock(state_mtx);
                  u->status.failed_reason = "timeout";
                }

                transition_unit(u, launcher::UnitState::kFailed, "on_timeout", "oneshot task timed out");
                cascade_stop_bound_dependents(uname);
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
      int64_t prev_ready_key = -1;

      {
        std::lock_guard lock(state_mtx);

        if (unit->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire) ||
            unit->status.state != launcher::UnitState::kRunning) {
          stale_timer = true;
        } else {
          prev_ready_key = unit->ready_timer_key;
          unit->ready_timer_key = tkey;
        }
      }

      if (prev_ready_key >= 0 && wheel_timer) {
        (void)wheel_timer->remove(prev_ready_key);
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
    (void)component_host.stop_component(unit->status.name, eff_timeout);
  } else {
    detach_container_client(unit, true, std::min<uint32_t>(1000U, std::max<uint32_t>(50U, eff_timeout)));
    (void)process_supervisor.stop_process(unit->status.name, eff_timeout);
  }

  transition_unit(unit, launcher::UnitState::kStopped, "on_stopped", "stopped");

  if (settle_boot) {
    mark_unit_boot_terminal(unit, false);
  }

  return true;
}

std::vector<std::string> LauncherServer::Impl::collect_stoppable_dependents_locked(const std::string& unit_name,
                                                                                   bool propagate_part_of) const {
  std::vector<std::string> deps;

  for (const auto& [other_name, other_unit] : units) {
    const auto st = other_unit->status.state;
    const bool stoppable = is_active_state(st) || st == launcher::UnitState::kInactive ||
                           st == launcher::UnitState::kRestarting || st == launcher::UnitState::kStopping;

    if (other_name == unit_name || !stoppable) {
      continue;
    }

    if (contains_str(other_unit->binds_preds, unit_name) ||
        (propagate_part_of &&
         (contains_str(other_unit->part_of_preds, unit_name) || contains_str(other_unit->req_preds, unit_name)))) {
      deps.push_back(other_name);
    }
  }

  return deps;
}

void LauncherServer::Impl::cascade_stop_bound_dependents(const std::string& unit_name) {
  std::vector<std::string> bound_dependents;

  {
    std::lock_guard lock(state_mtx);
    bound_dependents = collect_stoppable_dependents_locked(unit_name, false);
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
    dependents_to_stop = collect_stoppable_dependents_locked(name, propagate_part_of);
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

    if (!unit) {
      return;
    }

    std::shared_ptr<LauncherAPI> stale_container_cli;
    int64_t rkey = -1;
    int64_t pkey = -1;

    {
      std::lock_guard lock(state_mtx);

      if (unit->generation.load(std::memory_order_acquire) != gen) {
        return;
      }

      unit->status.exit_code = code;
      unit->status.pid = -1;
      unit->status.cgroup_path.clear();
      unit->active_record_path.clear();

      if (unit->status.type == launcher::UnitType::kContainer) {
        stale_container_cli = std::move(unit->container_client);
        unit->container_client_gen = 0U;
      }

      rkey = unit->ready_timer_key;
      unit->ready_timer_key = -1;
      pkey = unit->probe_poll_timer_key;
      unit->probe_poll_timer_key = -1;
    }

    if (rkey >= 0 && wheel_timer) {
      (void)wheel_timer->remove(rkey);
    }

    if (pkey >= 0 && wheel_timer) {
      (void)wheel_timer->remove(pkey);
    }

    if (stale_container_cli) {
      stale_container_cli->quit(true);
      stale_container_cli->wait_for_quit(500);
    }

    process_supervisor.reap_exited(name, gen);

    if (unit->generation.load(std::memory_order_acquire) != gen) {
      return;
    }

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

          if (unit->generation.load(std::memory_order_acquire) != gen) {
            return;
          }

          unit->oneshot_completed_ok = true;
        }

        if (!transition_unit_if_gen(unit, gen, std::nullopt, launcher::UnitState::kStopped, "on_exit",
                                    "completed (exit=" + std::to_string(code) + ")")) {
          return;
        }

        mark_unit_boot_terminal(unit, true);
        release_start_slot(unit);
        trigger_units(unit->on_exit_trigger);
      } else {
        if (!transition_unit_if_gen(unit, gen, std::nullopt, launcher::UnitState::kFailed, "on_exit",
                                    "oneshot failed (exit=" + std::to_string(code) + ")",
                                    "exit_code_" + std::to_string(code))) {
          return;
        }

        cascade_stop_bound_dependents(name);
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
      if (!transition_unit_if_gen(unit, gen, std::nullopt,
                                  is_success ? launcher::UnitState::kStopped : launcher::UnitState::kFailed, "on_exit",
                                  "exit triggered system shutdown (code=" + std::to_string(code) + ")")) {
        return;
      }

      mark_unit_boot_terminal(unit, is_success);
      release_start_slot(unit);
      cascade_stop_bound_dependents(name);
      trigger_units(unit->on_exit_trigger);
      request_async_stop(is_success ? 0 : 1);
      return;
    }

    if (should_restart) {
      cascade_stop_bound_dependents(name);
      schedule_respawn(unit, gen);
      release_start_slot(unit);
      trigger_units(unit->on_exit_trigger);
      return;
    }

    if (is_success) {
      if (!transition_unit_if_gen(unit, gen, std::nullopt, launcher::UnitState::kStopped, "on_exit",
                                  "exited cleanly (code=" + std::to_string(code) + ")")) {
        return;
      }
    } else {
      const std::string reason = is_normal ? ("exit_code_" + std::to_string(code)) : "signal_crash";

      if (!transition_unit_if_gen(unit, gen, std::nullopt, launcher::UnitState::kFailed, "on_exit",
                                  "exited unexpectedly (code=" + std::to_string(code) + ")", reason)) {
        return;
      }
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

void LauncherServer::Impl::schedule_respawn(const std::shared_ptr<UnitRuntime>& unit,
                                            std::optional<uint64_t> expected_gen,
                                            std::optional<launcher::UnitState> expected_state) {
  if (!unit || stopping.load(std::memory_order_acquire) || config.disable_respawn) {
    return;
  }

  uint32_t delay_ms = 0U;
  uint64_t gen = 0U;
  launcher::EventRecord ev;

  {
    std::lock_guard lock(state_mtx);
    gen = unit->generation.load(std::memory_order_acquire);

    if (expected_gen.has_value() && gen != *expected_gen) {
      return;
    }

    if (expected_state.has_value() && unit->status.state != *expected_state) {
      return;
    }

    const uint32_t exp_power = std::min<uint32_t>(unit->backoff_streak, 10U);
    const double scaled = static_cast<double>(unit->respawn.delay_ms) * std::pow(unit->respawn.backoff, exp_power);
    delay_ms = static_cast<uint32_t>(std::min<double>(scaled, static_cast<double>(unit->respawn.max_delay_ms)));
    ++unit->backoff_streak;
    ++unit->status.restart_count;

    if (!wheel_timer || !wheel_timer->is_running()) {
      apply_transition_locked(unit, server_start_steady_ms, launcher::UnitState::kFailed, "on_respawn_unavailable",
                              "timer stopped before respawn", ev);
      gen = 0U;
    } else {
      apply_transition_locked(unit, server_start_steady_ms, launcher::UnitState::kRestarting, "on_restarting",
                              "restarting in " + std::to_string(delay_ms) + "ms", ev);
    }
  }

  state_cv.notify_all();
  publish_event(ev);
  schedule_snapshot_publish();

  if (gen == 0U) {
    return;
  }

  std::weak_ptr<std::atomic_bool> weak_alive = alive_token;
  const std::string uname = unit->status.name;
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
            bool cancelled = stopping.load(std::memory_order_acquire);

            if (!cancelled) {
              std::lock_guard lock(state_mtx);
              cancelled = (u->status.state == launcher::UnitState::kStopped ||
                           u->status.state == launcher::UnitState::kStopping || is_active_state(u->status.state));
            }

            if (cancelled) {
              mark_unit_boot_terminal(u, false);
              release_start_slot(u);
              return;
            }

            log_manager.emit_launcher_message(uname, Logger::kError, err);
            cascade_stop_bound_dependents(uname);
            mark_unit_boot_terminal(u, false);
            release_start_slot(u);
            trigger_units(u->on_error_trigger);

            if (u->status.required) {
              request_async_stop(1);
            }
          }
        });
      });

  if VUNLIKELY (rkey < 0) {
    (void)transition_unit_if_gen(unit, gen, launcher::UnitState::kRestarting, launcher::UnitState::kFailed,
                                 "on_respawn_unavailable", "failed to arm respawn timer");
    return;
  }

  bool stale = false;
  int64_t prev_respawn_key = -1;

  {
    std::lock_guard lock(state_mtx);

    if (unit->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire) ||
        unit->status.state != launcher::UnitState::kRestarting) {
      stale = true;
    } else {
      prev_respawn_key = unit->respawn_timer_key;
      unit->respawn_timer_key = rkey;
    }
  }

  if (prev_respawn_key >= 0 && wheel_timer) {
    (void)wheel_timer->remove(prev_respawn_key);
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
  post_recovery_task([this, weak_alive, targets = std::move(targets)] {
    auto guard = weak_alive.lock();

    if (!guard || !guard->load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire)) {
      return;
    }

    for (const auto& target : targets) {
      if (stopping.load(std::memory_order_acquire)) {
        break;
      }

      const auto res = owner->start_unit(target);

      if (!res.ok() && res.code != launcher::ErrorCode::kAlreadyRunning) {
        log_manager.emit_launcher_message(target, Logger::kWarn, "Event-triggered start failed: " + res.message);
      }
    }
  });
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
  int64_t prev_ready_key = -1;

  {
    std::lock_guard lock(state_mtx);

    if (unit->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire)) {
      stale = true;
    } else {
      prev_ready_key = unit->ready_timer_key;
      unit->ready_timer_key = tkey;
    }
  }

  if (prev_ready_key >= 0 && wheel_timer) {
    (void)wheel_timer->remove(prev_ready_key);
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
          int64_t prev_ready_key = -1;

          {
            std::lock_guard lock(state_mtx);

            if (u->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire)) {
              stale_after_start = true;
            } else {
              u->status.pid = pid;
              u->status.cgroup_path = cg_path;
              prev_ready_key = u->ready_timer_key;
              u->ready_timer_key = -1;
            }
          }

          if (prev_ready_key >= 0 && wheel_timer) {
            (void)wheel_timer->remove(prev_ready_key);
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
  int64_t prev_periodic_key = -1;

  {
    std::lock_guard lock(state_mtx);

    if (unit->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire)) {
      stale = true;
    } else {
      prev_periodic_key = unit->periodic_timer_key;
      unit->periodic_timer_key = key;
    }
  }

  if (prev_periodic_key >= 0 && wheel_timer) {
    (void)wheel_timer->remove(prev_periodic_key);
  }

  if (stale && wheel_timer) {
    (void)wheel_timer->remove(key);
  }
}

void LauncherServer::Impl::offload_fault_recovery(const std::shared_ptr<UnitRuntime>& unit, uint64_t captured_gen,
                                                  bool is_watchdog, std::string reason, std::string detail) {
  const uint64_t recovery_gen = captured_gen + 1U;
  std::weak_ptr<std::atomic_bool> weak_alive = alive_token;
  post_recovery_task([this, weak_alive, unit, captured_gen, recovery_gen, is_watchdog, reason = std::move(reason),
                      detail = std::move(detail)]() mutable {
    auto guard = weak_alive.lock();

    if (!guard || !guard->load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire)) {
      return;
    }

    const uint32_t stop_budget =
        std::min<uint32_t>(unit->sigterm_timeout_ms == 0U ? 2000U : unit->sigterm_timeout_ms, 2000U);
    launcher::ComponentStopResult stop_res = launcher::ComponentStopResult::kStoppedClean;

    if (unit->status.type == launcher::UnitType::kComponent) {
      stop_res = component_host.stop_component(unit->status.name, stop_budget, captured_gen);
    } else {
      detach_container_client(unit, false, 500U, recovery_gen);
      (void)process_supervisor.stop_process(unit->status.name, stop_budget, captured_gen);
    }

    {
      std::lock_guard lock(state_mtx);

      if (unit->generation.load(std::memory_order_acquire) != recovery_gen ||
          unit->status.state != launcher::UnitState::kStopping) {
        return;
      }
    }

    cascade_stop_bound_dependents(unit->status.name);

    {
      std::lock_guard lock(state_mtx);

      if (unit->generation.load(std::memory_order_acquire) != recovery_gen ||
          unit->status.state != launcher::UnitState::kStopping) {
        return;
      }
    }

    const bool hung_executor = (unit->status.type == launcher::UnitType::kComponent &&
                                stop_res == launcher::ComponentStopResult::kTimedOutQuarantined);
    const bool dedicated =
        (unit->status.type == launcher::UnitType::kComponent && scheduler.is_dedicated_group(unit->status.group));

    if (hung_executor && !dedicated) {
      if (!transition_unit_if_gen(
              unit, recovery_gen, launcher::UnitState::kStopping, launcher::UnitState::kFailed,
              is_watchdog ? "on_watchdog_timeout" : "on_liveness_failed",
              detail + " (executor hung on shared/multi loop; use [[container]] for hard isolation)",
              "watchdog_executor_hung")) {
        return;
      }

      trigger_units(unit->on_error_trigger);

      if (unit->status.required) {
        request_async_stop(1);
      }

      return;
    }

    const bool should_respawn = !config.disable_respawn && !stopping.load(std::memory_order_acquire) &&
                                (is_watchdog ? unit->respawn.enabled : unit->liveness.restart_on_failure);

    if (should_respawn) {
      schedule_respawn(unit, recovery_gen, launcher::UnitState::kStopping);
      return;
    }

    if (!transition_unit_if_gen(
            unit, recovery_gen, launcher::UnitState::kStopping, launcher::UnitState::kFailed,
            is_watchdog ? "on_watchdog_timeout" : "on_liveness_failed",
            hung_executor ? (detail + " (executor hung; isolated in quarantine)") : detail,
            hung_executor ? std::string_view("watchdog_executor_hung") : std::string_view(reason))) {
      return;
    }

    trigger_units(unit->on_error_trigger);

    if (unit->status.required) {
      request_async_stop(1);
    }
  });
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
      auto it = units.find(name);

      if (it != units.end()) {
        all_units.push_back(it->second);
      }
    }
  }

  for (const auto& u : all_units) {
    launcher::UnitState st = launcher::UnitState::kPending;
    uint64_t captured_gen = 0U;
    std::vector<RemoteDepRef> remote_binds;

    {
      std::lock_guard lock(state_mtx);
      st = u->status.state;
      captured_gen = u->generation.load(std::memory_order_acquire);
      remote_binds = u->remote_binds_preds;

      if (st == launcher::UnitState::kReady && u->backoff_streak > 0U && u->ready_since_steady_ms > 0U) {
        const uint64_t stable_ms = (u->respawn.start_limit_interval_ms > 0U)
                                       ? static_cast<uint64_t>(u->respawn.start_limit_interval_ms)
                                       : 60000ULL;

        if (now >= u->ready_since_steady_ms && (now - u->ready_since_steady_ms) >= stable_ms) {
          u->backoff_streak = 0U;
        }
      }
    }

    if (st != launcher::UnitState::kReady && st != launcher::UnitState::kRunning) {
      continue;
    }

    if (!remote_binds.empty()) {
      std::string failed_bind;

      for (const auto& ref : remote_binds) {
        if (evaluate_remote_dep(ref.container, ref.component) == RemoteDepStatus::kFailed) {
          failed_bind = ref.component + " (" + ref.container + ")";
          break;
        }
      }

      if (!failed_bind.empty()) {
        if (!transition_unit_if(u, st, launcher::UnitState::kStopping, "on_dep_failed",
                                "bound container component failed: " + failed_bind)) {
          continue;
        }

        log_manager.emit_launcher_message(u->status.name, Logger::kWarn,
                                          "Bound container component failed: " + failed_bind + "; stopping unit.");
        std::weak_ptr<std::atomic_bool> weak_alive = alive_token;
        const std::string uname = u->status.name;
        post_recovery_task([this, weak_alive, uname] {
          auto guard = weak_alive.lock();

          if (!guard || !guard->load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire)) {
            return;
          }

          std::unordered_set<std::string> visited;
          (void)stop_unit_cascade(uname, visited, false);
        });
        continue;
      }
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
          offload_fault_recovery(u, captured_gen, true, "watchdog", "watchdog timeout");
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
      bool claimed_liveness_fail = false;

      {
        std::lock_guard lock(state_mtx);
        const bool still_current = (u->generation.load(std::memory_order_acquire) == captured_gen &&
                                    u->status.state == launcher::UnitState::kReady);

        if (still_current) {
          if (alive) {
            u->status.liveness_failures = 0U;
            u->status.healthy = true;
          } else {
            ++u->status.liveness_failures;
            fails = u->status.liveness_failures;
            u->status.healthy = false;

            if (fails >= u->liveness.failure_threshold && u->liveness.restart_on_failure && !config.disable_respawn) {
              u->generation.fetch_add(1U, std::memory_order_acq_rel);
              u->status.state = launcher::UnitState::kStopping;
              u->ready_since_steady_ms = 0U;
              u->active_record_path.clear();
              u->status.failed_reason = "liveness_failed";
              claimed_liveness_fail = true;
            }
          }
        }
      }

      if (!alive && fails >= u->liveness.failure_threshold) {
        log_manager.emit_launcher_message(u->status.name, Logger::kError,
                                          "Liveness probe failed " + std::to_string(fails) + " times.");

        if (claimed_liveness_fail) {
          cancel_unit_timers(u);
          offload_fault_recovery(u, captured_gen, false, "liveness_failed", "liveness probe failed");
        }
      }
    }
  }

  publish_heartbeat();
  publish_snapshot();
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

#ifdef VLINK_ENABLE_SECURITY
static bool generate_csprng_hex(size_t byte_count, std::string& hex_out) {
  hex_out.clear();

  if (byte_count == 0U) {
    return true;
  }

  std::vector<uint8_t> buf(byte_count, 0U);
#if defined(_WIN32) || defined(_WIN64)
  HCRYPTPROV prov = 0;

  if (::CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) == FALSE) {
    return false;
  }

  const BOOL ok = ::CryptGenRandom(prov, static_cast<DWORD>(byte_count), buf.data());
  ::CryptReleaseContext(prov, 0);

  if (ok == FALSE) {
    return false;
  }
#else
  const int fd = ::open("/dev/urandom", O_RDONLY);

  if (fd < 0) {
    return false;
  }

  size_t total = 0U;

  while (total < byte_count) {
    const ssize_t n = ::read(fd, buf.data() + total, byte_count - total);

    if (n > 0) {
      total += static_cast<size_t>(n);
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      ::close(fd);
      return false;
    }
  }

  ::close(fd);
#endif

  static constexpr char kHex[] = "0123456789abcdef";
  hex_out.reserve(byte_count * 2U);

  for (const uint8_t b : buf) {
    hex_out.push_back(kHex[(b >> 4U) & 0x0FU]);
    hex_out.push_back(kHex[b & 0x0FU]);
  }

  volatile uint8_t* p = buf.data();

  for (size_t i = 0U; i < buf.size(); ++i) {
    p[i] = 0U;
  }

  return true;
}
#endif

bool LauncherServer::Impl::is_remote_privileged_allowed() const noexcept {
  const bool supports_loopback = launcher::is_dds_scheme_name(active_ctrl_scheme) || active_ctrl_scheme == "zenoh";
  const bool loopback_bound = supports_loopback && (active_bind_ip == "127.0.0.1" || active_bind_ip == "::1");
  return use_security || active_ctrl_scheme == "intra" || loopback_bound || config.allow_remote_load ||
         manifest.allow_remote_load;
}

bool LauncherServer::Impl::init_control_plane() {
  const bool ctrl_enabled = config.enable_control_plane && manifest.control_enabled;
  const bool logd_enabled = log_manager.is_builtin_logd();

  if (!ctrl_enabled && !logd_enabled) {
    return true;
  }

  active_ctrl_scheme =
      launcher::resolve_scheme(!config.control_scheme.empty() ? config.control_scheme : manifest.control_scheme);

  for (char& ch : active_ctrl_scheme) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }

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

      std::weak_ptr<std::atomic_bool> weak_alive = alive_token;
      auto cmd_handler = [this, weak_alive](const launcher::CommandRequest& req, launcher::CommandResponse& resp) {
        auto guard = weak_alive.lock();

        if (!guard || !guard->load(std::memory_order_acquire)) {
          resp.result.code = launcher::ErrorCode::kNotRunning;
          resp.result.message = "LauncherServer is shutting down.";
          return;
        }

        resp = handle_command(req);
      };

      new_rpc_loop = std::make_unique<MultiLoop>(4U);
      new_rpc_loop->set_name("LauncherRpc");
      new_rpc_loop->async_run();

      bool ctrl_listen_ok = false;

      if (use_security) {
#ifdef VLINK_ENABLE_SECURITY
        new_sec_ctrl_server =
            make_server_node<SecServer>(active_ctrl_scheme, active_bind_ip, active_native_mode, ctrl_url,
                                        make_channel_sec_cfg(active_security_key, "launcher.ctrl"));
        new_sec_state_setter =
            make_server_node<SecSetter>(active_ctrl_scheme, active_bind_ip, active_native_mode, state_url,
                                        make_channel_sec_cfg(active_security_key, "launcher.state"));
        new_sec_event_pub =
            make_server_node<SecEventPub>(active_ctrl_scheme, active_bind_ip, active_native_mode, event_url,
                                          make_channel_sec_cfg(active_security_key, "launcher.events"));

        if (config.publish_logs) {
          new_sec_log_pub = make_server_node<SecLogPub>(active_ctrl_scheme, active_bind_ip, active_native_mode, log_url,
                                                        make_channel_sec_cfg(active_security_key, "launcher.logs"));
        }

        new_sec_hb_pub = make_server_node<SecHbPub>(active_ctrl_scheme, active_bind_ip, active_native_mode, hb_url,
                                                    make_channel_sec_cfg(active_security_key, "launcher.hb"));

        if (new_sec_ctrl_server && new_sec_state_setter && new_sec_event_pub && new_sec_hb_pub &&
            (!config.publish_logs || new_sec_log_pub)) {
          (void)new_sec_ctrl_server->attach(new_rpc_loop.get());
          ctrl_listen_ok = new_sec_ctrl_server->listen(std::move(cmd_handler));
        }
#endif
      } else {
        new_ctrl_server =
            make_server_node<PlainServer>(active_ctrl_scheme, active_bind_ip, active_native_mode, ctrl_url);
        new_state_setter =
            make_server_node<PlainSetter>(active_ctrl_scheme, active_bind_ip, active_native_mode, state_url);
        new_event_pub =
            make_server_node<PlainEventPub>(active_ctrl_scheme, active_bind_ip, active_native_mode, event_url);

        if (config.publish_logs) {
          new_log_pub = make_server_node<PlainLogPub>(active_ctrl_scheme, active_bind_ip, active_native_mode, log_url);
        }

        new_hb_pub = make_server_node<PlainHbPub>(active_ctrl_scheme, active_bind_ip, active_native_mode, hb_url);

        if (new_ctrl_server && new_state_setter && new_event_pub && new_hb_pub &&
            (!config.publish_logs || new_log_pub)) {
          (void)new_ctrl_server->attach(new_rpc_loop.get());
          ctrl_listen_ok = new_ctrl_server->listen(std::move(cmd_handler));
        }
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
      std::string logd_key;
#ifdef VLINK_ENABLE_SECURITY
      if (use_security && !generate_csprng_hex(32U, logd_key)) {
        cleanup_new_nodes();
        VLOG_E("LauncherServer: Failed to generate CSPRNG key for built-in logd channel.");
        return false;
      }
#endif

      bool listen_ok = false;

      if (use_security) {
#ifdef VLINK_ENABLE_SECURITY
        new_sec_logd_sub = make_server_node<SecLogdSub>(active_ctrl_scheme, active_bind_ip, active_native_mode,
                                                        logd_url, make_channel_sec_cfg(logd_key, "launcher.logd"));

        if (new_sec_logd_sub) {
          listen_ok = new_sec_logd_sub->listen(std::move(logd_handler));
        }
#endif
      } else {
        new_logd_sub = make_server_node<PlainLogdSub>(active_ctrl_scheme, active_bind_ip, active_native_mode, logd_url);

        if (new_logd_sub) {
          listen_ok = new_logd_sub->listen(std::move(logd_handler));
        }
      }

      if VUNLIKELY (!listen_ok) {
        launcher::wipe_secret_string(logd_key);
        cleanup_new_nodes();
        VLOG_E("LauncherServer: Failed to listen on built-in logd channel '", logd_url, "'.");
        return false;
      }

      log_manager.set_logd_endpoint(logd_url, active_ctrl_scheme, active_bind_ip, active_native_mode, logd_key);
      launcher::wipe_secret_string(logd_key);
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
  control_ready.store(false, std::memory_order_release);
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
    rpc_loop->quit(false);

    if (!rpc_loop->is_in_same_thread()) {
      (void)rpc_loop->wait_for_quit(1500);
    }

    rpc_loop->quit(true);

    if (!rpc_loop->is_in_same_thread()) {
      (void)rpc_loop->wait_for_quit(500);
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
  const int64_t key = wheel_timer->add(50U, [this, weak_alive](WheelTimer::Key) {
    auto guard = weak_alive.lock();

    if (!guard || !guard->load(std::memory_order_acquire)) {
      return;
    }

    const bool posted = post_protected([this, weak_alive] {
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

    if VUNLIKELY (!posted) {
      snapshot_flush_pending.store(false, std::memory_order_release);
    }
  });

  if VUNLIKELY (key < 0) {
    snapshot_flush_pending.store(false, std::memory_order_release);
  }
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
  post_local_callback(&Impl::local_event_cb, has_local_event_cb, ev);

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
  post_local_callback(&Impl::local_log_cb, has_local_log_cb, rec);

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
    snap.units.reserve(std::min<size_t>(unit_order.size(), launcher::kMaxUnitCount));

    for (const auto& name : unit_order) {
      if (snap.units.size() >= launcher::kMaxUnitCount) {
        break;
      }

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
    if (snap.units.size() >= launcher::kMaxUnitCount) {
      break;
    }

    auto csnap = cli->get_cached_snapshot();

    if (csnap.has_value()) {
      for (auto& u : csnap->units) {
        if (snap.units.size() >= launcher::kMaxUnitCount) {
          break;
        }

        u.container = cname;
        snap.units.push_back(std::move(u));
      }
    }
  }

  return snap;
}

static bool has_path_traversal(std::string_view raw_path) {
  const std::filesystem::path p(raw_path);

  if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) {
    return true;
  }

  for (const auto& comp : p) {
    if (comp == "..") {
      return true;
    }
  }

  return false;
}

launcher::CommandResponse LauncherServer::Impl::handle_command(const launcher::CommandRequest& req) {
  launcher::CommandResponse resp;

  if (req.proto_version != launcher::kProtocolVersion) {
    resp.result.code = launcher::ErrorCode::kVersionMismatch;
    resp.result.message = "Protocol version mismatch.";
    return resp;
  }

  if (!control_ready.load(std::memory_order_acquire)) {
    resp.result.code = launcher::ErrorCode::kNotRunning;
    resp.result.message = "LauncherServer is still initializing.";
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
      if VUNLIKELY (!is_remote_privileged_allowed()) {
        resp.result.code = launcher::ErrorCode::kUnauthorized;
        resp.result.message =
            "Remote dynamic component loading requires security_key, intra://, explicit loopback bind_ip, or "
            "[launcher].remote_load=true.";
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
    case launcher::CommandOp::kSetRecord: {
      if (req.record_enable && !req.record_path.empty() && !is_remote_privileged_allowed() &&
          has_path_traversal(req.record_path)) {
        resp.result.code = launcher::ErrorCode::kUnauthorized;
        resp.result.message =
            "Remote absolute or parent-traversal record_path requires security_key, intra://, loopback bind_ip, or "
            "[launcher].remote_load=true.";
        break;
      }

      resp.result = owner->set_record(req.target, req.record_enable, req.record_path);
      break;
    }
    case launcher::CommandOp::kShutdown: {
      resp.result.code = launcher::ErrorCode::kOk;
      resp.result.message = "Shutdown requested.";
      std::weak_ptr<std::atomic_bool> weak_alive = alive_token;

      if (wheel_timer && wheel_timer->is_running()) {
        const int64_t tkey = wheel_timer->add(200U, [this, weak_alive](WheelTimer::Key) {
          auto guard = weak_alive.lock();

          if (guard && guard->load(std::memory_order_acquire)) {
            request_async_stop(0);
          }
        });

        if (tkey < 0) {
          request_async_stop(0);
        }
      } else {
        request_async_stop(0);
      }

      break;
    }
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

  std::unique_ptr<ThreadPool> old_recovery_pool;

  {
    std::lock_guard lock(recovery_mtx);
    old_recovery_pool.swap(recovery_pool);
  }

  if (old_recovery_pool) {
    old_recovery_pool->shutdown();
    old_recovery_pool.reset();
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
  const uint32_t comp_rem = static_cast<uint32_t>(std::clamp<int64_t>(budget.remaining_time(), 200, 3000));
  component_host.stop_all(comp_rem);
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

  const uint32_t sched_rem = static_cast<uint32_t>(std::clamp<int64_t>(budget.remaining_time(), 200, 3000));
  scheduler.shutdown(sched_rem);
  resource_ctrl.shutdown();
  log_manager.shutdown();
  launcher::wipe_secret_string(active_security_key);

  if (alive_token) {
    alive_token->store(false, std::memory_order_release);
  }

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

LauncherServer::LauncherServer() : LauncherServer(Config{}) {}

LauncherServer::LauncherServer(const Config& config) : impl_(std::make_unique<Impl>(this, config)) {
  set_name("LauncherServer");
}

LauncherServer::~LauncherServer() {
  stop();

  if (impl_->alive_token) {
    impl_->alive_token->store(false, std::memory_order_release);
  }

  impl_->join_fallback_stop_thread();

  if (MessageLoop::is_running()) {
    quit(true);
    wait_for_quit();
  }

  launcher::wipe_secret_string(impl_->config.security_key);
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

  return impl_->commit_parsed_manifest(std::move(parsed), error_out);
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

  return impl_->commit_parsed_manifest(std::move(parsed), error_out);
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
    std::string dot = "digraph launcher {\n  \"launcher_start\" [shape=box];\n";

    for (const auto& name : impl_->unit_order) {
      const auto& u = impl_->units.at(name);
      const std::string grp =
          u->status.task_group.empty() ? launcher::unit_type_to_string(u->status.type) : u->status.task_group;
      dot.append("  \"")
          .append(launcher::escape_dot_label(name))
          .append("\" [label=\"")
          .append(launcher::escape_dot_label(name))
          .append("\\n(")
          .append(launcher::escape_dot_label(grp))
          .append(")\"];\n");
    }

    for (const auto& name : impl_->unit_order) {
      const auto& u = impl_->units.at(name);
      const std::string dst = launcher::escape_dot_label(name);

      if (u->order_preds.empty()) {
        dot.append(R"(  "launcher_start" -> ")").append(dst).append("\";\n");
      } else {
        for (const auto& p : u->order_preds) {
          if (impl_->units.count(p) != 0U) {
            dot.append("  \"").append(launcher::escape_dot_label(p)).append("\" -> \"").append(dst).append("\";\n");
          }
        }
      }
    }

    for (const auto& e : launcher::compute_dataflow_edges(impl_->manifest)) {
      dot.append("  \"")
          .append(launcher::escape_dot_label(e.from))
          .append("\" -> \"")
          .append(launcher::escape_dot_label(e.to))
          .append(R"(" [style=dashed, color="#1976d2", label=")")
          .append(launcher::escape_dot_label(e.url))
          .append(R"("];)")
          .push_back('\n');
    }

    dot.append("}\n");
    return dot;
  }

  if (format == GraphFormat::kMermaid) {
    std::string out = "flowchart TD\n";

    for (const auto& name : impl_->unit_order) {
      const auto& u = impl_->units.at(name);
      const std::string id = launcher::mermaid_node_id(name);
      out.append("  ")
          .append(id)
          .append("[\"")
          .append(launcher::escape_mermaid_label(name))
          .append(" (")
          .append(launcher::unit_type_to_string(u->status.type))
          .append(")\"]\n");
    }

    for (const auto& name : impl_->unit_order) {
      const auto& u = impl_->units.at(name);
      const std::string tid = launcher::mermaid_node_id(name);

      for (const auto& p : u->order_preds) {
        out.append("  ").append(launcher::mermaid_node_id(p)).append(" --> ").append(tid).append("\n");
      }

      for (const auto& w : u->want_preds) {
        if (!contains_str(u->order_preds, w)) {
          out.append("  ").append(launcher::mermaid_node_id(w)).append(" -. wants .-> ").append(tid).append("\n");
        }
      }
    }

    std::unordered_set<std::string> declared_nodes(impl_->unit_order.begin(), impl_->unit_order.end());
    const auto df_edges = launcher::compute_dataflow_edges(impl_->manifest);

    auto ensure_mermaid_node = [&](const std::string& node_name) {
      if (declared_nodes.insert(node_name).second) {
        out.append("  ")
            .append(launcher::mermaid_node_id(node_name))
            .append("[\"")
            .append(launcher::escape_mermaid_label(node_name))
            .append(" (component)\"]\n");
      }
    };

    for (const auto& e : df_edges) {
      ensure_mermaid_node(e.from);
      ensure_mermaid_node(e.to);
      out.append("  ")
          .append(launcher::mermaid_node_id(e.from))
          .append(" ==>|\"")
          .append(launcher::escape_mermaid_label(e.url))
          .append("\"| ")
          .append(launcher::mermaid_node_id(e.to))
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

      if (!u->selected_for_boot || !u->condition || (!u->autostart && !u->explicit_boot_dep)) {
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
  auto new_cb = callback ? MemoryResource::make_shared<EventCallback>(std::move(callback)) : nullptr;

  {
    std::lock_guard lock(impl_->cb_mtx);
    old_cb = std::move(impl_->local_event_cb);
    impl_->local_event_cb = std::move(new_cb);
    impl_->has_local_event_cb.store(static_cast<bool>(impl_->local_event_cb), std::memory_order_release);
  }
}

void LauncherServer::register_log_callback(LogCallback&& callback) {
  std::shared_ptr<LogCallback> old_cb;
  auto new_cb = callback ? MemoryResource::make_shared<LogCallback>(std::move(callback)) : nullptr;

  {
    std::lock_guard lock(impl_->cb_mtx);
    old_cb = std::move(impl_->local_log_cb);
    impl_->local_log_cb = std::move(new_cb);
    impl_->has_local_log_cb.store(static_cast<bool>(impl_->local_log_cb), std::memory_order_release);
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

  if (impl_->alive_token) {
    impl_->alive_token->store(false, std::memory_order_release);
  }

  impl_->alive_token = MemoryResource::make_shared<std::atomic_bool>(true);
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
  impl_->control_ready.store(false, std::memory_order_release);
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

  {
    std::lock_guard lock(impl_->recovery_mtx);
    impl_->recovery_pool = std::make_unique<ThreadPool>(2U);
    impl_->recovery_pool->set_name("vl_recovery");
  }

  std::weak_ptr<std::atomic_bool> weak_alive = impl_->alive_token;

  impl_->component_host.set_callbacks(
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

          if (u && (u->ready.type == launcher::ReadyProbeType::kComponent ||
                    u->ready.type == launcher::ReadyProbeType::kNone)) {
            impl_->finish_readiness(u, gen, true, "component ready");
          }
        });
      },
      [this, weak_alive](const std::string& name, uint64_t gen, bool healthy, const std::string& detail) {
        auto guard = weak_alive.lock();

        if (!guard || !guard->load(std::memory_order_acquire)) {
          return;
        }

        (void)post_task([this, weak_alive, name, gen, healthy, detail] {
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

            if (u->generation.load(std::memory_order_acquire) != gen) {
              return;
            }

            u->status.healthy = healthy;

            if (!detail.empty()) {
              u->status.detail = detail;
            }
          }

          impl_->schedule_snapshot_publish();
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

  auto rollback_start = [&]() {
    impl_->wheel_timer->stop();

    if (MessageLoop::is_running()) {
      quit(true);
      wait_for_quit();
    }

    {
      std::lock_guard lock(impl_->recovery_mtx);

      if (impl_->recovery_pool) {
        impl_->recovery_pool->shutdown();
        impl_->recovery_pool.reset();
      }
    }

    impl_->scheduler.shutdown();
    impl_->resource_ctrl.shutdown();
    impl_->log_manager.shutdown();

    {
      std::lock_guard lock(impl_->stop_mtx);
      impl_->running.store(false, std::memory_order_release);
    }

    impl_->stop_cv.notify_all();
  };

  bool loop_started = false;

  try {
    loop_started = async_run();
  } catch (...) {
    rollback_start();
    throw;
  }

  if VUNLIKELY (!loop_started) {
    rollback_start();
    return false;
  }

  if VUNLIKELY (!impl_->init_control_plane()) {
    rollback_start();
    return false;
  }

  if (impl_->log_manager.is_builtin_logd() && impl_->active_ctrl_scheme == "intra") {
    impl_->effective_logger_cfg.plugin.clear();
  }

  impl_->running.store(true, std::memory_order_release);
  impl_->control_ready.store(true, std::memory_order_release);
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
        const auto cur_st = impl_->units[name]->status.state;

        if (names.size() == 1U && is_active_state(cur_st)) {
          res.code = launcher::ErrorCode::kAlreadyRunning;
          res.message = "Unit '" + name + "' is already running.";
          return res;
        }

        if (names.size() == 1U && cur_st == launcher::UnitState::kInactive) {
          res.code = launcher::ErrorCode::kAlreadyRunning;
          res.message = "Unit '" + name + "' is already loaded in inactive standby; use activate to transition it.";
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

    std::vector<RemoteDepRef> remote_reqs;
    std::vector<RemoteDepRef> remote_orders;

    {
      std::lock_guard lock(impl_->state_mtx);
      remote_reqs = u->remote_req_preds;
      remote_orders = u->remote_order_preds;
    }

    for (const auto& r : remote_reqs) {
      const uint32_t remote_wait = impl_->calc_remote_dep_wait_ms(r.container, r.component);

      if (!impl_->ensure_and_wait_remote_dep(r.container, r.component, remote_wait, true)) {
        failed_units.insert(name);

        if (required_set.count(name) != 0U) {
          res.code = launcher::ErrorCode::kDependencyFailed;
          res.message =
              "Required container component '" + r.component + "' in '" + r.container + "' failed or timed out.";
          return res;
        }

        break;
      }
    }

    if (failed_units.count(name) != 0U) {
      continue;
    }

    for (const auto& r : remote_orders) {
      const uint32_t remote_wait = impl_->calc_remote_dep_wait_ms(r.container, r.component);
      (void)impl_->wait_remote_dep_ready(r.container, r.component, remote_wait);
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
    auto cli = impl_->connect_container_or_error(cname, 2000U, res);

    if (!cli) {
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
      launcher::CommandResult fwd;

      const bool forwarded =
          impl_->try_forward_remote_component(name, fwd, [&](LauncherAPI& cli) { return cli.stop_unit(name); });

      if (forwarded) {
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
  std::unordered_map<std::string, bool> part_of_restore_was_inactive;
  std::unordered_set<std::string> part_of_restore_set;

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
        const auto st = ounit->status.state;
        const bool should_restore = is_active_state(st) || st == launcher::UnitState::kInactive;

        if (base_set.count(oname) == 0U && should_restore && contains_str(ounit->part_of_preds, cur)) {
          if (part_of_restore_set.insert(oname).second) {
            part_of_restore_was_inactive[oname] = (st == launcher::UnitState::kInactive);
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

  if (!part_of_restore_set.empty()) {
    std::vector<std::string> restore_errors;

    for (const auto& dep_name : impl_->compute_topo_order_for_set(part_of_restore_set)) {
      const bool was_inactive = part_of_restore_was_inactive[dep_name];
      auto dep_u = impl_->find_unit(dep_name);

      if (was_inactive && dep_u && dep_u->status.type == launcher::UnitType::kComponent) {
        std::string launch_err;

        if (!impl_->launch_unit_now(dep_u, &launch_err, true)) {
          std::string err_entry;
          err_entry.reserve(dep_name.size() + 2U + launch_err.size());
          err_entry.append(dep_name).append(": ").append(launch_err);
          restore_errors.push_back(std::move(err_entry));
        }
      } else {
        auto r = start_unit(dep_name);

        if (!r.ok() && r.code != launcher::ErrorCode::kAlreadyRunning) {
          std::string err_entry;
          err_entry.reserve(dep_name.size() + 2U + r.message.size());
          err_entry.append(dep_name).append(": ").append(r.message);
          restore_errors.push_back(std::move(err_entry));
        }
      }
    }

    if (!restore_errors.empty()) {
      launcher::CommandResult partial_res;
      partial_res.code = launcher::ErrorCode::kInternalError;
      partial_res.message = "Restarted " + target + ", but part_of recovery failed for: ";

      for (size_t i = 0U; i < restore_errors.size(); ++i) {
        if (i > 0U) {
          partial_res.message.append("; ");
        }

        partial_res.message.append(restore_errors[i]);
      }

      return partial_res;
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
      launcher::CommandResult fwd;

      const bool forwarded =
          impl_->try_forward_remote_component(name, fwd, [&](LauncherAPI& cli) { return cli.reload_unit(name); });

      if (forwarded) {
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
      launcher::CommandResult fwd;

      const bool forwarded =
          impl_->try_forward_remote_component(name, fwd, [&](LauncherAPI& cli) { return cli.activate_unit(name); });

      if (forwarded) {
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

    const uint64_t gen = u->generation.load(std::memory_order_acquire);
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
      launcher::CommandResult fwd;

      const bool forwarded =
          impl_->try_forward_remote_component(name, fwd, [&](LauncherAPI& cli) { return cli.deactivate_unit(name); });

      if (forwarded) {
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

    if (!impl_->transition_unit_if(u, launcher::UnitState::kReady, launcher::UnitState::kStopping, "on_deactivating",
                                   "deactivating")) {
      res.code = launcher::ErrorCode::kNotRunning;
      res.message = "Unit '" + name + "' state changed concurrently before deactivation.";
      return res;
    }

    std::string err;

    if (!impl_->component_host.deactivate_component(name, &err)) {
      (void)impl_->transition_unit_if(u, launcher::UnitState::kStopping, launcher::UnitState::kReady,
                                      "on_deactivate_failed", err);
      res.code = launcher::ErrorCode::kInternalError;
      res.message = err;
      return res;
    }

    if (!impl_->transition_unit_if(u, launcher::UnitState::kStopping, launcher::UnitState::kInactive, "on_deactivated",
                                   "inactive standby")) {
      res.code = launcher::ErrorCode::kInternalError;
      res.message = "Unit '" + name + "' state changed concurrently during deactivation.";
      return res;
    }
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
    auto cli = impl_->connect_container_or_error(cname, 2000U, res);

    if (!cli) {
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
                                       st == launcher::UnitState::kRestarting || st == launcher::UnitState::kStopping ||
                                       u->respawn_timer_key >= 0 || u->delay_timer_key >= 0 ||
                                       u->remote_wait_timer_key >= 0;

        if (pending_or_active) {
          stop_set.insert(name);
        }
      } else if (!is_active_state(st) && st != launcher::UnitState::kInactive) {
        start_set.insert(name);
      }
    }

    impl_->startup_queue.erase(std::remove_if(impl_->startup_queue.begin(), impl_->startup_queue.end(),
                                              [&](const std::string& qname) { return keep_set.count(qname) == 0U; }),
                               impl_->startup_queue.end());
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

    std::vector<RemoteDepRef> remote_reqs;
    std::vector<RemoteDepRef> remote_orders;

    {
      std::lock_guard lock(impl_->state_mtx);
      remote_reqs = u->remote_req_preds;
      remote_orders = u->remote_order_preds;
    }

    for (const auto& r : remote_reqs) {
      const uint32_t remote_wait = impl_->calc_remote_dep_wait_ms(r.container, r.component);

      if (!impl_->ensure_and_wait_remote_dep(r.container, r.component, remote_wait, true)) {
        failed_start.insert(name);

        if (required_keep.count(name) != 0U) {
          res.code = launcher::ErrorCode::kDependencyFailed;
          res.message = "Isolate required container component '" + r.component + "' in '" + r.container +
                        "' failed or timed out.";
          return res;
        }

        break;
      }
    }

    if (failed_start.count(name) != 0U) {
      continue;
    }

    for (const auto& r : remote_orders) {
      const uint32_t remote_wait = impl_->calc_remote_dep_wait_ms(r.container, r.component);
      (void)impl_->wait_remote_dep_ready(r.container, r.component, remote_wait);
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

    std::unordered_set<std::string> component_pulled_containers;

    auto keep_remote_ref = [&](const std::string& ref) {
      if (ref.empty()) {
        return;
      }

      auto check_one = [&](const std::string& cand) {
        auto cit = impl_->component_to_container.find(cand);

        if (cit != impl_->component_to_container.end()) {
          component_pulled_containers.insert(cit->second);

          if (remote_keep_names.insert(cand).second) {
            remote_comps.emplace_back(cand, cit->second);
            rbfs.push(cand);
          }
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
      }
    }

    for (const auto& name : keep_set) {
      auto uit = impl_->units.find(name);

      if (uit == impl_->units.end() || uit->second->status.type != launcher::UnitType::kContainer) {
        continue;
      }

      const auto& u = uit->second;
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
    auto cli = impl_->connect_container_or_error(cname, 2000U, res, "Isolate failed: ");

    if (!cli) {
      return res;
    }

    auto fwd = cli->stop_unit(comp_name);

    if (!fwd.ok() && fwd.code != launcher::ErrorCode::kNotRunning && fwd.code != launcher::ErrorCode::kNotFound) {
      return fwd;
    }
  }

  for (const auto& [comp_name, cname] : remote_comps) {
    auto cli = impl_->connect_container_or_error(cname, 2000U, res, "Isolate failed: ");

    if (!cli) {
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
    const std::string cname = impl_->find_container_for_component(unit_name);

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

  if (!impl_->running.load(std::memory_order_acquire) || impl_->stopping.load(std::memory_order_acquire)) {
    res.code = launcher::ErrorCode::kNotRunning;
    res.message = "LauncherServer is not running; call start() first.";
    return res;
  }

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

      if (impl_->total_unit_count_locked() >= launcher::kMaxUnitCount) {
        res.code = launcher::ErrorCode::kInvalidArgument;
        res.message = "Maximum unit capacity (" + std::to_string(launcher::kMaxUnitCount) + ") reached.";
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

    if (impl_->total_unit_count_locked() >= launcher::kMaxUnitCount) {
      res.code = launcher::ErrorCode::kInvalidArgument;
      res.message = "Maximum unit capacity (" + std::to_string(launcher::kMaxUnitCount) + ") reached.";
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
    const std::string cname = impl_->find_container_for_component(unit_name);

    if (!cname.empty()) {
      auto cli = impl_->connect_container_or_error(cname, 2000U, res);

      if (!cli) {
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
    const std::string cname = impl_->find_container_for_component(unit_name);

    if (!cname.empty()) {
      auto cu = impl_->find_unit(cname);
      auto cli = impl_->get_or_connect_container_client(cu, 2000U);

      if (cli && cli->is_connected()) {
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

  launcher::CommandResult fwd;

  if (impl_->try_forward_remote_component(unit_name, fwd,
                                          [&](LauncherAPI& cli) { return cli.set_param(unit_name, entry); })) {
    return fwd;
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
      const std::string cname = impl_->find_container_for_component(name);

      if (!cname.empty()) {
        auto cu = impl_->find_unit(cname);
        auto cli = impl_->connect_container_or_error(cname, 2000U, res);

        if (!cli) {
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

        std::string forward_path = resolved_path;

        if (enable && !resolved_path.empty() && !impl_->active_record_dir.empty()) {
          std::error_code rel_ec;
          const auto rel = std::filesystem::relative(std::filesystem::path(resolved_path),
                                                     std::filesystem::path(impl_->active_record_dir), rel_ec);

          if (!rel_ec && !rel.empty()) {
            const std::string rel_str = Helpers::path_to_string(rel);

            if (!has_path_traversal(rel_str)) {
              forward_path = rel_str;
            }
          }
        }

        auto fwd = cli->set_record(name, enable, forward_path);

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
