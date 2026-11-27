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

#include <vlink/base/condition_variable.h>
#include <vlink/base/elapsed_timer.h>
#include <vlink/base/helpers.h>
#include <vlink/base/logger.h>
#include <vlink/base/macros.h>
#include <vlink/base/multi_loop.h>
#include <vlink/base/thread_pool.h>
#include <vlink/base/timer.h>
#include <vlink/base/utils.h>
#include <vlink/base/uuid.h>
#include <vlink/base/wheel_timer.h>
#include <vlink/extension/security.h>
#include <vlink/external/launcher_server.h>
#include <vlink/publisher.h>
#include <vlink/server.h>
#include <vlink/setter.h>
#include <vlink/version.h>

#if !defined(_WIN32) && !defined(_WIN64)
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../launcher_common.h"
#include "./component_host.h"
#include "./launcher_config.h"
#include "./notify_server.h"
#include "./process_supervisor.h"

namespace vlink {

static std::string make_session_id() {
  const std::string hex = Uuid::random_hex();
  const std::string short_id = (hex.size() >= 8U) ? hex.substr(0U, 8U) : hex;
  return "s_" + Utils::get_pid_str() + "_" + short_id;
}

static bool is_active_state(launcher::UnitState s) noexcept {
  return s == launcher::UnitState::kStarting || s == launcher::UnitState::kRunning || s == launcher::UnitState::kReady;
}

static const char* queue_type_env_name(MessageLoop::Type t) noexcept {
  switch (t) {
    case MessageLoop::kLockfreeType:
      return "lockfree";
    case MessageLoop::kPriorityType:
      return "priority";
    case MessageLoop::kNormalType:
    default:
      return "normal";
  }
}

static const char* queue_strategy_env_name(MessageLoop::Strategy s) noexcept {
  switch (s) {
    case MessageLoop::kPopStrategy:
      return "pop";
    case MessageLoop::kBlockStrategy:
      return "block";
    case MessageLoop::kOptimizationStrategy:
    default:
      return "optimization";
  }
}

static bool write_private_spill_file(const std::string& path, const std::string& content) {
#if defined(_WIN32) || defined(_WIN64)
  std::ofstream ofs(std::filesystem::path(Helpers::string_to_wstring(path)), std::ios::binary | std::ios::trunc);

  if (!ofs) {
    return false;
  }

  ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
  ofs.flush();
  const bool ok = ofs.good();
  ofs.close();

  if (!ok) {
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(Helpers::string_to_wstring(path)), ec);
  }

  return ok;
#else
  const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);

  if (fd < 0) {
    return false;
  }

  size_t total = 0U;

  while (total < content.size()) {
    const ssize_t n = ::write(fd, content.data() + total, content.size() - total);

    if (n > 0) {
      total += static_cast<size_t>(n);
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }

  const bool ok = (total == content.size()) && (::close(fd) == 0);

  if (!ok) {
    (void)::unlink(path.c_str());
  }

  return ok;
#endif
}

static void remove_spill_file_path(const std::string& path) noexcept {
  if (path.empty()) {
    return;
  }

  std::error_code ec;
#if defined(_WIN32) || defined(_WIN64)
  std::filesystem::remove(std::filesystem::path(Helpers::string_to_wstring(path)), ec);
#else
  std::filesystem::remove(path, ec);
#endif
}

struct UnitRuntime final {
  launcher::UnitStatus status;
  bool autostart{true};
  uint32_t delay_ms{0};
  uint32_t stop_timeout_ms{5000};
  uint32_t watchdog_ms{0};
  launcher::RespawnConfig respawn;
  launcher::ReadyProbeConfig ready;
  std::vector<std::string> req_preds;
  std::vector<std::string> order_preds;
  launcher::ProcessSpec proc_spec;
  launcher::ComponentSpec comp_spec;

  std::atomic<uint64_t> generation{0};
  uint64_t start_steady_ms{0};
  uint64_t watchdog_armed_ms{0};
  uint64_t last_watchdog_ping_ms{0};
  uint64_t window_start_ms{0};
  uint32_t window_attempts{0};
  uint32_t backoff_streak{0};

  int64_t delay_timer_key{-1};
  int64_t ready_timer_key{-1};
  int64_t respawn_timer_key{-1};
  std::mutex spill_mtx;
  std::string params_spill_file;
  uint64_t params_spill_gen{0};
  std::mutex unit_op_mtx;

  ~UnitRuntime() { clear_spill_file(); }

  void set_spill_file(std::string path, uint64_t gen) {
    std::string old_path;

    {
      std::lock_guard lock(spill_mtx);
      old_path = std::move(params_spill_file);
      params_spill_file = std::move(path);
      params_spill_gen = gen;
    }

    remove_spill_file_path(old_path);
  }

  void clear_spill_file(std::optional<uint64_t> expected_gen = std::nullopt) noexcept {
    std::string old_path;

    {
      std::lock_guard lock(spill_mtx);

      if (params_spill_file.empty()) {
        return;
      }

      if (expected_gen.has_value() && params_spill_gen != *expected_gen) {
        return;
      }

      old_path = std::move(params_spill_file);
      params_spill_file.clear();
      params_spill_gen = 0U;
    }

    remove_spill_file_path(old_path);
  }
};

struct LauncherServer::Impl final {
  using PlainServer = Server<launcher::CommandRequest, launcher::CommandResponse>;
  using PlainSetter = Setter<launcher::SystemSnapshot>;
  using PlainEventPub = Publisher<launcher::EventRecord>;
  using PlainHbPub = Publisher<launcher::HeartbeatMsg>;

#ifdef VLINK_ENABLE_SECURITY
  using SecServer = SecurityServer<launcher::CommandRequest, launcher::CommandResponse>;
  using SecSetter = SecuritySetter<launcher::SystemSnapshot>;
  using SecEventPub = SecurityPublisher<launcher::EventRecord>;
  using SecHbPub = SecurityPublisher<launcher::HeartbeatMsg>;
#endif

  explicit Impl(LauncherServer* owner_ptr, const Config& cfg)
      : owner(owner_ptr),
        config(cfg),
        wheel_timer(std::make_unique<WheelTimer>(128U, 20U)),
        alive_token(std::make_shared<std::atomic_bool>(true)),
        cached_host(Utils::get_host_name()),
        cached_pid(Utils::get_pid()) {}

  ~Impl() {
    launcher::wipe_secret_string(active_key);
    launcher::wipe_secret_string(manifest.security_key);
    launcher::wipe_secret_string(config.security_key_override);
  }

  LauncherServer* owner{nullptr};
  Config config;
  launcher::LaunchManifest manifest;
  bool manifest_loaded{false};

  std::unique_ptr<WheelTimer> wheel_timer;
  std::unique_ptr<Timer> housekeep_timer;
  std::shared_ptr<MultiLoop> ctrl_loop;
  std::unique_ptr<ThreadPool> recovery_pool;
  std::shared_ptr<std::atomic_bool> alive_token;

  launcher::ComponentHost component_host;
  launcher::ProcessSupervisor process_supervisor;
  launcher::NotifyServer notify_server;

  std::shared_ptr<PlainServer> ctrl_server;
  std::shared_ptr<PlainSetter> state_setter;
  std::shared_ptr<PlainEventPub> event_pub;
  std::shared_ptr<PlainHbPub> hb_pub;
#ifdef VLINK_ENABLE_SECURITY
  std::shared_ptr<SecServer> sec_ctrl_server;
  std::shared_ptr<SecSetter> sec_state_setter;
  std::shared_ptr<SecEventPub> sec_event_pub;
  std::shared_ptr<SecHbPub> sec_hb_pub;
#endif

  std::string cached_host;
  int64_t cached_pid{0};
  std::string session_id;
  std::string active_scheme;
  std::string active_ns;
  std::string active_key;
  uint32_t active_domain{0};
  bool use_security{false};
  uint64_t server_start_steady_ms{0};
  uint64_t last_hb_pub_ms{0};

  mutable std::mutex start_mtx;
  mutable std::mutex state_mtx;
  std::mutex stop_mtx;
  std::mutex timer_mtx;
  std::mutex cb_mtx;
  mutable std::mutex ctrl_mtx;
  std::recursive_mutex op_mtx;
  ConditionVariable stop_cv;

  std::shared_ptr<EventCallback> local_event_cb;
  std::vector<std::string> topo_order;
  std::unordered_map<std::string, std::shared_ptr<UnitRuntime>> units;
  std::unordered_map<std::string, std::vector<std::string>> group_members;

  std::atomic_bool running{false};
  std::atomic_bool stopping{false};
  std::atomic_bool stop_executed{false};
  std::atomic_bool schedule_queued{false};
  std::atomic<int32_t> exit_code{0};

  bool post_loop(MessageLoop::Callback&& cb) const {
    if VUNLIKELY (owner == nullptr || !owner->MessageLoop::is_running()) {
      return false;
    }

    return owner->post_task(std::move(cb));
  }

  std::shared_ptr<UnitRuntime> find_unit(const std::string& name) const {
    std::lock_guard lock(state_mtx);
    auto it = units.find(name);
    return (it == units.end()) ? nullptr : it->second;
  }

  std::vector<std::string> expand_target(const std::string& target) const {
    std::lock_guard lock(state_mtx);

    if (!target.empty() && target[0] == '@') {
      auto git = group_members.find(target.substr(1U));
      return (git == group_members.end()) ? std::vector<std::string>{} : git->second;
    }

    if (units.count(target) != 0U) {
      return {target};
    }

    return {};
  }

  bool cancel_unit_timers(const std::shared_ptr<UnitRuntime>& unit) {
    int64_t dk = -1;
    int64_t rk = -1;
    int64_t sk = -1;

    {
      std::lock_guard lock(state_mtx);
      dk = unit->delay_timer_key;
      unit->delay_timer_key = -1;
      rk = unit->ready_timer_key;
      unit->ready_timer_key = -1;
      sk = unit->respawn_timer_key;
      unit->respawn_timer_key = -1;
    }

    if (wheel_timer) {
      if (dk >= 0) {
        (void)wheel_timer->remove(dk);
      }

      if (rk >= 0) {
        (void)wheel_timer->remove(rk);
      }

      if (sk >= 0) {
        (void)wheel_timer->remove(sk);
      }
    }

    return dk >= 0 || rk >= 0 || sk >= 0;
  }

  void publish_event(const launcher::EventRecord& ev) {
    {
      std::lock_guard lock(ctrl_mtx);
#ifdef VLINK_ENABLE_SECURITY
      if (use_security) {
        if (sec_event_pub) {
          (void)sec_event_pub->publish(ev);
        }
      } else
#endif
      {
        if (event_pub) {
          (void)event_pub->publish(ev);
        }
      }
    }

    std::shared_ptr<EventCallback> cb;

    {
      std::lock_guard lock(cb_mtx);
      cb = local_event_cb;
    }

    if (cb && *cb) {
      (*cb)(ev);
    }
  }

  launcher::SystemSnapshot build_snapshot() const {
    launcher::SystemSnapshot snap;
    const uint64_t now_steady = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);
    const uint64_t now_sys = ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMilli);

    std::lock_guard lock(state_mtx);
    snap.session_id = session_id;
    snap.version = VLINK_VERSION;
    snap.host = cached_host;
    snap.server_pid = cached_pid;
    snap.timestamp_ms = now_sys;
    snap.uptime_ms = (server_start_steady_ms > 0U && now_steady >= server_start_steady_ms)
                         ? (now_steady - server_start_steady_ms)
                         : 0U;
    snap.config_file = manifest.source_file;
    snap.units.reserve(topo_order.size());

    for (const auto& name : topo_order) {
      auto it = units.find(name);

      if (it == units.end()) {
        continue;
      }

      auto st = it->second->status;
      st.generation = it->second->generation.load(std::memory_order_acquire);

      if (is_active_state(st.state) && it->second->start_steady_ms > 0U && now_steady >= it->second->start_steady_ms) {
        st.uptime_ms = now_steady - it->second->start_steady_ms;
      } else {
        st.uptime_ms = 0U;
      }

      snap.units.push_back(std::move(st));
    }

    return snap;
  }

  void publish_snapshot() {
    const auto snap = build_snapshot();
    std::lock_guard lock(ctrl_mtx);
#ifdef VLINK_ENABLE_SECURITY
    if (use_security) {
      if (sec_state_setter) {
        sec_state_setter->set(snap);
      }

      return;
    }
#endif
    if (state_setter) {
      state_setter->set(snap);
    }
  }

  void publish_heartbeat() {
    launcher::HeartbeatMsg hb;
    const uint64_t now_steady = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);
    hb.timestamp_ms = ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMilli);

    {
      std::lock_guard lock(state_mtx);
      hb.session_id = session_id;
      hb.uptime_ms = (server_start_steady_ms > 0U && now_steady >= server_start_steady_ms)
                         ? (now_steady - server_start_steady_ms)
                         : 0U;
      hb.unit_count = static_cast<uint32_t>(units.size());

      for (const auto& [k, u] : units) {
        (void)k;

        if (u->status.state == launcher::UnitState::kReady) {
          ++hb.ready_count;
        } else if (u->status.state == launcher::UnitState::kFailed) {
          ++hb.failed_count;
        }
      }
    }

    std::lock_guard lock(ctrl_mtx);
#ifdef VLINK_ENABLE_SECURITY
    if (use_security) {
      if (sec_hb_pub) {
        (void)sec_hb_pub->publish(hb);
      }

      return;
    }
#endif
    if (hb_pub) {
      (void)hb_pub->publish(hb);
    }
  }

  void transition_unit(const std::shared_ptr<UnitRuntime>& unit, launcher::UnitState next_state,
                       std::string_view event_name, std::string_view detail) {
    launcher::EventRecord ev;
    const uint64_t now_steady = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);

    {
      std::lock_guard lock(state_mtx);
      ev.timestamp_ms = ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMilli);
      ev.unit_name = unit->status.name;
      ev.unit_type = unit->status.type;
      ev.old_state = unit->status.state;
      ev.new_state = next_state;
      ev.event_name = std::string(event_name);
      ev.message = std::string(detail);

      unit->status.state = next_state;
      unit->status.detail = std::string(detail);

      if (next_state == launcher::UnitState::kStarting) {
        unit->start_steady_ms = now_steady;
        unit->status.start_time_ms = ev.timestamp_ms;
        unit->status.healthy = true;
        unit->status.failed_reason.clear();
      } else if (next_state == launcher::UnitState::kReady) {
        if (unit->watchdog_ms > 0U) {
          unit->watchdog_armed_ms = now_steady;
        }
      } else if (next_state == launcher::UnitState::kStopped || next_state == launcher::UnitState::kFailed) {
        unit->status.pid = (unit->status.type == launcher::UnitType::kComponent) ? cached_pid : -1;
      }
    }

    publish_event(ev);
    publish_snapshot();
  }

  void rebuild_unit_table() {
    std::lock_guard lock(state_mtx);
    units.clear();
    group_members.clear();
    topo_order.clear();

    std::vector<std::string> all_names;

    for (const auto& ps : manifest.processes) {
      auto u = std::make_shared<UnitRuntime>();
      u->status.name = ps.name;
      u->status.ns = ps.ns;
      u->status.group = ps.group;
      u->status.type = launcher::UnitType::kProcess;
      u->status.required = ps.required;
      u->status.dependencies = ps.depends_on;
      u->autostart = ps.autostart;
      u->delay_ms = ps.delay_ms;
      u->stop_timeout_ms = ps.stop_timeout_ms;
      u->watchdog_ms = ps.watchdog_ms;
      u->respawn = ps.respawn;
      u->ready = ps.ready;
      u->req_preds = ps.depends_on;
      u->order_preds = ps.depends_on;
      u->order_preds.insert(u->order_preds.end(), ps.after.begin(), ps.after.end());
      u->proc_spec = ps;

      units[ps.name] = u;
      all_names.push_back(ps.name);
    }

    for (const auto& cs : manifest.components) {
      auto u = std::make_shared<UnitRuntime>();
      u->status.name = cs.name;
      u->status.ns = cs.ns;
      u->status.group = cs.group;
      u->status.type = launcher::UnitType::kComponent;
      u->status.pid = Utils::get_pid();
      u->status.required = cs.required;
      u->status.dependencies = cs.depends_on;
      u->autostart = cs.autostart;
      u->delay_ms = cs.delay_ms;
      u->stop_timeout_ms = cs.stop_timeout_ms;
      u->watchdog_ms = cs.watchdog_ms;
      u->respawn = cs.respawn;
      u->ready = cs.ready;
      u->req_preds = cs.depends_on;
      u->order_preds = cs.depends_on;
      u->order_preds.insert(u->order_preds.end(), cs.after.begin(), cs.after.end());
      u->comp_spec = cs;

      units[cs.name] = u;
      all_names.push_back(cs.name);
    }

    std::unordered_map<std::string, size_t> indeg;
    std::unordered_map<std::string, std::vector<std::string>> adj;

    for (const auto& n : all_names) {
      indeg[n] = 0U;
    }

    for (const auto& n : all_names) {
      const auto& u = units[n];
      std::set<std::string> uniq(u->order_preds.begin(), u->order_preds.end());

      for (const auto& dep : uniq) {
        if (units.count(dep) != 0U) {
          adj[dep].push_back(n);
          ++indeg[n];
        }
      }
    }

    std::queue<std::string> q;

    for (const auto& n : all_names) {
      if (indeg[n] == 0U) {
        q.push(n);
      }
    }

    while (!q.empty()) {
      std::string cur = std::move(q.front());
      q.pop();
      topo_order.push_back(cur);

      for (const auto& nxt : adj[cur]) {
        if (--indeg[nxt] == 0U) {
          q.push(nxt);
        }
      }
    }

    for (const auto& n : all_names) {
      if (std::find(topo_order.begin(), topo_order.end(), n) == topo_order.end()) {
        topo_order.push_back(n);
      }
    }

    for (const auto& n : topo_order) {
      const auto& u = units[n];

      if (!u->status.group.empty()) {
        group_members[u->status.group].push_back(n);
      }
    }

    auto resolve_target_set = [&](const std::vector<std::string>& specs) {
      std::unordered_set<std::string> res;

      for (const auto& spec : specs) {
        if (!spec.empty() && spec[0] == '@') {
          auto git = group_members.find(spec.substr(1U));

          if (git != group_members.end()) {
            res.insert(git->second.begin(), git->second.end());
          }
        } else if (units.count(spec) != 0U) {
          res.insert(spec);
        }
      }

      return res;
    };

    if (!config.only_targets.empty()) {
      std::unordered_set<std::string> keep = resolve_target_set(config.only_targets);
      std::queue<std::string> bfs;

      for (const auto& k : keep) {
        bfs.push(k);
      }

      while (!bfs.empty()) {
        const std::string cur = std::move(bfs.front());
        bfs.pop();
        auto it = units.find(cur);

        if (it == units.end()) {
          continue;
        }

        for (const auto& dep : it->second->req_preds) {
          if (units.count(dep) != 0U && keep.insert(dep).second) {
            bfs.push(dep);
          }
        }
      }

      for (const auto& n : topo_order) {
        units[n]->autostart = (keep.count(n) != 0U);
      }
    }

    if (!config.skip_targets.empty()) {
      std::unordered_set<std::string> skipped = resolve_target_set(config.skip_targets);

      for (const auto& n : topo_order) {
        if (skipped.count(n) != 0U) {
          units[n]->autostart = false;
          continue;
        }

        for (const auto& dep : units[n]->req_preds) {
          if (skipped.count(dep) != 0U) {
            skipped.insert(n);
            units[n]->autostart = false;
            break;
          }
        }
      }
    }
  }

  void request_async_stop(int32_t code) {
    if (stopping.exchange(true, std::memory_order_acq_rel)) {
      return;
    }

    exit_code.store(code, std::memory_order_release);
    std::weak_ptr<std::atomic_bool> weak_alive = alive_token;
    const bool posted = post_loop([this, weak_alive]() {
      auto guard = weak_alive.lock();

      if (guard && guard->load(std::memory_order_acquire)) {
        do_stop();
      }
    });

    if VUNLIKELY (!posted && owner != nullptr) {
      do_stop();
    }
  }

  void enqueue_schedule() {
    if (stopping.load(std::memory_order_acquire)) {
      return;
    }

    if (schedule_queued.exchange(true, std::memory_order_acq_rel)) {
      return;
    }

    std::weak_ptr<std::atomic_bool> weak_alive = alive_token;

    if (!post_loop([this, weak_alive]() {
          schedule_queued.store(false, std::memory_order_release);
          auto g = weak_alive.lock();

          if (g && g->load(std::memory_order_acquire)) {
            schedule_ready_dependents();
          }
        })) {
      schedule_queued.store(false, std::memory_order_release);
    }
  }

  void schedule_ready_dependents() {
    if (stopping.load(std::memory_order_acquire)) {
      return;
    }

    std::vector<std::shared_ptr<UnitRuntime>> to_launch;
    std::vector<std::shared_ptr<UnitRuntime>> to_fail;

    {
      std::lock_guard lock(state_mtx);
      std::unordered_set<std::string> failed_now;

      for (const auto& name : topo_order) {
        auto u = units[name];

        if (!u->autostart || u->status.state != launcher::UnitState::kPending || u->delay_timer_key >= 0) {
          continue;
        }

        bool dep_failed = false;
        bool all_ready = true;

        for (const auto& dep : u->req_preds) {
          auto dit = units.find(dep);

          if (dit == units.end()) {
            continue;
          }

          const auto dst = dit->second->status.state;

          if (dst == launcher::UnitState::kFailed || failed_now.count(dep) != 0U) {
            dep_failed = true;
            break;
          }

          if (dst != launcher::UnitState::kReady) {
            all_ready = false;
          }
        }

        if (dep_failed) {
          failed_now.insert(name);
          to_fail.push_back(u);
          continue;
        }

        if (!all_ready) {
          continue;
        }

        for (const auto& ord : u->order_preds) {
          auto oit = units.find(ord);

          if (oit == units.end()) {
            continue;
          }

          const auto ost = oit->second->status.state;

          if (ost != launcher::UnitState::kReady && ost != launcher::UnitState::kStopped &&
              ost != launcher::UnitState::kFailed && failed_now.count(ord) == 0U) {
            all_ready = false;
            break;
          }
        }

        if (all_ready) {
          to_launch.push_back(u);
        }
      }
    }

    for (const auto& u : to_fail) {
      {
        std::lock_guard lock(state_mtx);

        if (u->status.state != launcher::UnitState::kPending) {
          continue;
        }

        u->status.failed_reason = "dependency_failed";
      }

      transition_unit(u, launcher::UnitState::kFailed, "on_dep_failed", "required dependency failed");

      if (u->status.required) {
        request_async_stop(1);
      }
    }

    for (const auto& u : to_launch) {
      if (stopping.load(std::memory_order_acquire)) {
        return;
      }

      uint64_t expected_gen = 0U;

      {
        std::lock_guard lock(state_mtx);

        if (u->status.state != launcher::UnitState::kPending || u->delay_timer_key >= 0) {
          continue;
        }

        expected_gen = u->generation.load(std::memory_order_acquire);
      }

      if (u->delay_ms > 0U && wheel_timer && wheel_timer->is_running()) {
        std::weak_ptr<std::atomic_bool> weak_alive = alive_token;
        const std::string uname = u->status.name;
        const int64_t key = wheel_timer->add(u->delay_ms, [this, weak_alive, uname, expected_gen](WheelTimer::Key k) {
          auto g = weak_alive.lock();

          if (!g || !g->load(std::memory_order_acquire)) {
            return;
          }

          (void)post_loop([this, weak_alive, uname, expected_gen, k]() {
            auto g2 = weak_alive.lock();

            if (!g2 || !g2->load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire)) {
              return;
            }

            auto target = find_unit(uname);

            if (!target) {
              return;
            }

            {
              std::lock_guard lock(state_mtx);

              if (target->status.state != launcher::UnitState::kPending ||
                  target->generation.load(std::memory_order_acquire) != expected_gen ||
                  target->delay_timer_key != static_cast<int64_t>(k)) {
                return;
              }

              target->delay_timer_key = -1;
            }

            std::string err;
            (void)launch_unit_now(target, &err);
          });
        });

        bool stale = false;

        {
          std::lock_guard lock(state_mtx);

          if (stopping.load(std::memory_order_acquire) || u->status.state != launcher::UnitState::kPending ||
              u->generation.load(std::memory_order_acquire) != expected_gen || u->delay_timer_key >= 0) {
            stale = true;
          } else {
            u->delay_timer_key = key;
          }
        }

        if (stale) {
          (void)wheel_timer->remove(key);
        }
      } else {
        std::string err;
        (void)launch_unit_now(u, &err);
      }
    }
  }

  void finish_readiness(const std::shared_ptr<UnitRuntime>& unit, uint64_t gen, bool ok, std::string_view detail) {
    int64_t old_ready_key = -1;
    bool valid = false;
    bool detail_update_only = false;

    {
      std::lock_guard lock(state_mtx);

      if (unit->generation.load(std::memory_order_acquire) == gen) {
        if (unit->status.state == launcher::UnitState::kStarting ||
            unit->status.state == launcher::UnitState::kRunning) {
          old_ready_key = unit->ready_timer_key;
          unit->ready_timer_key = -1;
          valid = true;
        } else if (ok && unit->status.state == launcher::UnitState::kReady &&
                   unit->ready.type == launcher::ReadyProbeType::kNone && !detail.empty()) {
          unit->status.detail = std::string(detail);
          detail_update_only = true;
        }
      }
    }

    if (old_ready_key >= 0 && wheel_timer) {
      (void)wheel_timer->remove(old_ready_key);
    }

    if (detail_update_only) {
      publish_snapshot();
      return;
    }

    if (!valid) {
      return;
    }

    if (ok) {
      transition_unit(unit, launcher::UnitState::kReady, "on_ready", detail);
      enqueue_schedule();
      return;
    }

    unit->generation.fetch_add(1U, std::memory_order_acq_rel);

    if (unit->status.type == launcher::UnitType::kProcess) {
      notify_server.unregister_unit(unit->status.name, gen);
    }

    {
      std::lock_guard lock(state_mtx);
      unit->status.failed_reason = "ready_timeout";
    }

    transition_unit(unit, launcher::UnitState::kStopping, "on_ready_timeout", detail);
    offload_fault_recovery(unit, gen, "ready_timeout", std::string(detail));
  }

  void arm_readiness_probe(const std::shared_ptr<UnitRuntime>& unit, uint64_t gen) {
    if (unit->ready.type == launcher::ReadyProbeType::kNone) {
      finish_readiness(unit, gen, true, "ready");
      return;
    }

    if (!wheel_timer || !wheel_timer->is_running()) {
      return;
    }

    std::weak_ptr<std::atomic_bool> weak_alive = alive_token;
    const std::string uname = unit->status.name;

    if (unit->ready.type == launcher::ReadyProbeType::kDelay) {
      const uint32_t dms = std::max<uint32_t>(1U, unit->ready.delay_ms);
      const int64_t key = wheel_timer->add(dms, [this, weak_alive, uname, gen](WheelTimer::Key) {
        auto g = weak_alive.lock();

        if (!g || !g->load(std::memory_order_acquire)) {
          return;
        }

        (void)post_loop([this, weak_alive, uname, gen]() {
          auto g2 = weak_alive.lock();

          if (!g2 || !g2->load(std::memory_order_acquire)) {
            return;
          }

          auto u = find_unit(uname);

          if (u) {
            finish_readiness(u, gen, true, "delay elapsed");
          }
        });
      });

      bool stale = false;

      {
        std::lock_guard lock(state_mtx);

        if (unit->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire) ||
            (unit->status.state != launcher::UnitState::kStarting &&
             unit->status.state != launcher::UnitState::kRunning)) {
          stale = true;
        } else {
          unit->ready_timer_key = key;
        }
      }

      if (stale) {
        (void)wheel_timer->remove(key);
      }

      return;
    }

    const int64_t key = wheel_timer->add(unit->ready.timeout_ms, [this, weak_alive, uname, gen](WheelTimer::Key) {
      auto g = weak_alive.lock();

      if (!g || !g->load(std::memory_order_acquire)) {
        return;
      }

      (void)post_loop([this, weak_alive, uname, gen]() {
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

    bool remove_new = false;

    {
      std::lock_guard lock(state_mtx);

      if (unit->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire) ||
          (unit->status.state != launcher::UnitState::kStarting &&
           unit->status.state != launcher::UnitState::kRunning)) {
        remove_new = true;
      } else if (unit->ready_timer_key >= 0) {
        remove_new = true;
      } else {
        unit->ready_timer_key = key;
      }
    }

    if (remove_new) {
      (void)wheel_timer->remove(key);
    }
  }

  bool launch_unit_now(const std::shared_ptr<UnitRuntime>& unit, std::string* error_out) {
    if (!unit || stopping.load(std::memory_order_acquire)) {
      return false;
    }

    std::lock_guard unit_lock(unit->unit_op_mtx);

    if (stopping.load(std::memory_order_acquire)) {
      return false;
    }

    cancel_unit_timers(unit);
    const uint64_t gen = unit->generation.fetch_add(1U, std::memory_order_acq_rel) + 1U;

    {
      std::lock_guard lock(state_mtx);
      unit->last_watchdog_ping_ms = 0U;
      unit->watchdog_armed_ms = 0U;
    }

    transition_unit(unit, launcher::UnitState::kStarting, "on_starting", "starting");

    if (unit->status.type == launcher::UnitType::kComponent) {
      launcher::ComponentSpec spec_copy;

      {
        std::lock_guard lock(state_mtx);
        spec_copy = unit->comp_spec;
      }

      std::string comp_err;

      if (!component_host.start_component(spec_copy, gen, &comp_err)) {
        if (unit->generation.load(std::memory_order_acquire) != gen) {
          return false;
        }

        {
          std::lock_guard lock(state_mtx);
          unit->status.failed_reason = "component_start_failed";
        }

        transition_unit(unit, launcher::UnitState::kFailed, "on_start_failed", comp_err);

        if (error_out != nullptr) {
          *error_out = comp_err;
        }

        enqueue_schedule();

        if (unit->status.required) {
          request_async_stop(1);
        }

        return false;
      }

      bool stale = false;
      bool already_ready = false;

      {
        std::lock_guard lock(state_mtx);

        if (unit->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire)) {
          stale = true;
        } else {
          already_ready = (unit->status.state == launcher::UnitState::kReady);
        }
      }

      if (stale) {
        (void)component_host.stop_component(unit->status.name, unit->stop_timeout_ms, gen);
        return false;
      }

      if (!already_ready) {
        transition_unit(unit, launcher::UnitState::kRunning, "on_running", "running");
        arm_readiness_probe(unit, gen);
      }

      return true;
    }

    launcher::ProcessLaunchRequest req;
    req.name = unit->status.name;
    req.generation = gen;
    req.program = unit->proc_spec.program;
    req.args = unit->proc_spec.args;
    req.cwd = unit->proc_spec.cwd;
    req.env = unit->proc_spec.env;
    req.inherit_env = unit->proc_spec.inherit_env;
    req.output = unit->proc_spec.output;
    req.stop_timeout_ms = unit->stop_timeout_ms;

    if (unit->ready.type == launcher::ReadyProbeType::kStdout) {
      req.ready_stdout_pattern = unit->ready.pattern;
    }

    req.env["VLINK_LAUNCHER_SECURITY_KEY"] = "";
    req.env["VLINK_LAUNCHER_UNIT_NAME"] = unit->status.name;
    req.env["VLINK_LAUNCHER_UNIT_GEN"] = std::to_string(gen);
    req.env["VLINK_LAUNCHER_UNIT_NS"] = unit->proc_spec.ns;
    req.env["VLINK_LAUNCHER_WATCHDOG_MS"] = std::to_string(unit->watchdog_ms);
    req.env["VLINK_LAUNCHER_REMAPS_JSON"] = launcher::string_map_to_json(unit->proc_spec.remaps);
    req.env["VLINK_LAUNCHER_SCHEDULER"] =
        (unit->proc_spec.scheduler == launcher::SchedulerMode::kMulti) ? "multi" : "dedicated";
    req.env["VLINK_LAUNCHER_THREADS"] =
        (unit->proc_spec.scheduler == launcher::SchedulerMode::kMulti) ? std::to_string(unit->proc_spec.threads) : "1";
    req.env["VLINK_LAUNCHER_QUEUE"] = queue_type_env_name(unit->proc_spec.queue_type);
    req.env["VLINK_LAUNCHER_QUEUE_STRATEGY"] = queue_strategy_env_name(unit->proc_spec.queue_strategy);

    if (!manifest.plugin_dirs.empty()) {
#if defined(_WIN32) || defined(_WIN64)
      constexpr char kPathSep = ';';
#else
      constexpr char kPathSep = ':';
#endif
      std::string joined_dirs;

      for (size_t i = 0U; i < manifest.plugin_dirs.size(); ++i) {
        if (i > 0U) {
          joined_dirs.push_back(kPathSep);
        }

        joined_dirs.append(manifest.plugin_dirs[i]);
      }

      req.env["VLINK_LAUNCHER_PLUGIN_DIRS"] = std::move(joined_dirs);
    }

    static constexpr size_t kInlineParamsLimit = 16384U;
    static std::atomic<uint64_t> spill_seq{1U};
    unit->clear_spill_file();
    const std::string params_json = launcher::params_map_to_json(unit->proc_spec.params);

    if (params_json.size() > kInlineParamsLimit) {
      const uint64_t seq = spill_seq.fetch_add(1U, std::memory_order_relaxed);
      const std::string spill_path = Helpers::path_to_string(
          std::filesystem::path(Utils::get_tmp_dir()) /
          ("vlink_lp_" + session_id + "_" + std::to_string(seq) + "_" + std::to_string(gen) + ".json"));

      if (write_private_spill_file(spill_path, params_json)) {
        unit->set_spill_file(spill_path, gen);
        req.env["VLINK_LAUNCHER_PARAMS_FILE"] = spill_path;
        req.env["VLINK_LAUNCHER_PARAMS_JSON"] = "";
      } else {
        VLOG_W("LauncherServer: Failed to write params spill file '", spill_path,
               "'; falling back to inline environment.");
        req.env["VLINK_LAUNCHER_PARAMS_FILE"] = "";
        req.env["VLINK_LAUNCHER_PARAMS_JSON"] = params_json;
      }
    } else {
      req.env["VLINK_LAUNCHER_PARAMS_FILE"] = "";
      req.env["VLINK_LAUNCHER_PARAMS_JSON"] = params_json;
    }

    if (notify_server.is_running()) {
      const std::string tok = notify_server.register_unit(unit->status.name, gen);

      if (!tok.empty()) {
        req.env["VLINK_LAUNCHER_NOTIFY_ADDR"] = notify_server.address();
        req.env["VLINK_LAUNCHER_NOTIFY_TOKEN"] = tok;
      } else {
        req.env["VLINK_LAUNCHER_NOTIFY_ADDR"] = "";
        req.env["VLINK_LAUNCHER_NOTIFY_TOKEN"] = "";
      }
    } else {
      req.env["VLINK_LAUNCHER_NOTIFY_ADDR"] = "";
      req.env["VLINK_LAUNCHER_NOTIFY_TOKEN"] = "";
    }

    int64_t pid = -1;
    std::string proc_err;

    if (!process_supervisor.start_process(req, &pid, &proc_err)) {
      notify_server.unregister_unit(unit->status.name, gen);
      unit->clear_spill_file(gen);

      if (unit->generation.load(std::memory_order_acquire) != gen) {
        return false;
      }

      {
        std::lock_guard lock(state_mtx);
        unit->status.failed_reason = "exec_failed";
      }

      transition_unit(unit, launcher::UnitState::kFailed, "on_start_failed", proc_err);

      if (error_out != nullptr) {
        *error_out = proc_err;
      }

      enqueue_schedule();

      if (unit->status.required) {
        request_async_stop(1);
      }

      return false;
    }

    bool already_ready = false;
    bool already_exited = false;
    bool stale = false;

    {
      std::lock_guard lock(state_mtx);

      if (unit->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire)) {
        stale = true;
      } else if (unit->status.state == launcher::UnitState::kReady) {
        unit->status.pid = pid;
        already_ready = true;
      } else if (unit->status.state != launcher::UnitState::kStarting) {
        already_exited = true;
      } else {
        unit->status.pid = pid;
      }
    }

    if (stale) {
      notify_server.unregister_unit(unit->status.name, gen);
      (void)process_supervisor.stop_process(unit->status.name, 500U, gen);
      unit->clear_spill_file(gen);
      return false;
    }

    if (already_ready) {
      publish_snapshot();
      return true;
    }

    if (already_exited) {
      return true;
    }

    transition_unit(unit, launcher::UnitState::kRunning, "on_running", "running");
    arm_readiness_probe(unit, gen);
    return true;
  }

  bool stop_unit_internal(const std::shared_ptr<UnitRuntime>& unit, uint32_t timeout_override_ms = 0U) {
    if (!unit) {
      return false;
    }

    std::lock_guard unit_lock(unit->unit_op_mtx);
    const bool cancelled_timer = cancel_unit_timers(unit);
    unit->generation.fetch_add(1U, std::memory_order_acq_rel);

    if (unit->status.type == launcher::UnitType::kProcess) {
      notify_server.unregister_unit(unit->status.name);
    }

    bool inactive = false;

    {
      std::lock_guard lock(state_mtx);
      const auto st = unit->status.state;
      inactive = (st == launcher::UnitState::kStopped || st == launcher::UnitState::kPending ||
                  st == launcher::UnitState::kFailed);
    }

    if (inactive) {
      if (cancelled_timer) {
        transition_unit(unit, launcher::UnitState::kStopped, "on_stopped", "cancelled");
        return true;
      }

      return false;
    }

    transition_unit(unit, launcher::UnitState::kStopping, "on_stopping", "stopping");
    const uint32_t stop_ms =
        (timeout_override_ms > 0U) ? std::min(unit->stop_timeout_ms, timeout_override_ms) : unit->stop_timeout_ms;

    if (unit->status.type == launcher::UnitType::kComponent) {
      (void)component_host.stop_component(unit->status.name, stop_ms);
    } else {
      (void)process_supervisor.stop_process(unit->status.name, stop_ms);
    }

    transition_unit(unit, launcher::UnitState::kStopped, "on_stopped", "stopped");
    unit->clear_spill_file();
    return true;
  }

  void schedule_respawn(const std::shared_ptr<UnitRuntime>& unit, uint64_t expected_gen) {
    if (config.disable_respawn || stopping.load(std::memory_order_acquire)) {
      return;
    }

    if (!wheel_timer || !wheel_timer->is_running()) {
      {
        std::lock_guard lock(state_mtx);
        unit->status.failed_reason = "timer_unavailable";
      }

      transition_unit(unit, launcher::UnitState::kFailed, "on_fault", "respawn timer unavailable");
      enqueue_schedule();

      if (unit->status.required) {
        request_async_stop(1);
      }

      return;
    }

    const uint64_t now = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);
    uint32_t delay_ms = unit->respawn.delay_ms;
    bool burst_exceeded = false;

    {
      std::lock_guard lock(state_mtx);

      if (unit->generation.load(std::memory_order_acquire) != expected_gen) {
        return;
      }

      if (unit->respawn.start_burst > 0U) {
        if (unit->window_start_ms == 0U || (now - unit->window_start_ms) > unit->respawn.start_window_ms) {
          unit->window_start_ms = now;
          unit->window_attempts = 1U;
        } else {
          ++unit->window_attempts;
        }

        if (unit->window_attempts > unit->respawn.start_burst) {
          burst_exceeded = true;
          unit->status.failed_reason = "start_limit";
        }
      }

      if (!burst_exceeded) {
        ++unit->status.restart_count;
        const uint32_t shift = std::min<uint32_t>(unit->backoff_streak, 5U);
        const uint64_t scaled = static_cast<uint64_t>(unit->respawn.delay_ms) << shift;
        delay_ms = static_cast<uint32_t>(std::min<uint64_t>(scaled, unit->respawn.max_delay_ms));
        ++unit->backoff_streak;
      }
    }

    if (burst_exceeded) {
      transition_unit(unit, launcher::UnitState::kFailed, "on_start_limit", "restart burst limit exceeded");
      enqueue_schedule();

      if (unit->status.required) {
        request_async_stop(1);
      }

      return;
    }

    std::weak_ptr<std::atomic_bool> weak_alive = alive_token;
    const std::string uname = unit->status.name;
    const int64_t key = wheel_timer->add(delay_ms, [this, weak_alive, uname, expected_gen](WheelTimer::Key k) {
      auto g = weak_alive.lock();

      if (!g || !g->load(std::memory_order_acquire)) {
        return;
      }

      (void)post_loop([this, weak_alive, uname, expected_gen, k]() {
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

          if (u->generation.load(std::memory_order_acquire) != expected_gen ||
              u->respawn_timer_key != static_cast<int64_t>(k)) {
            return;
          }

          u->respawn_timer_key = -1;
        }

        std::string err;
        (void)launch_unit_now(u, &err);
      });
    });

    bool stale = false;

    {
      std::lock_guard lock(state_mtx);

      if (stopping.load(std::memory_order_acquire) ||
          unit->generation.load(std::memory_order_acquire) != expected_gen || unit->respawn_timer_key >= 0) {
        stale = true;
      } else {
        unit->respawn_timer_key = key;
      }
    }

    if (stale) {
      (void)wheel_timer->remove(key);
    }
  }

  void offload_fault_recovery(const std::shared_ptr<UnitRuntime>& unit, uint64_t old_gen, std::string reason,
                              std::string detail) {
    const uint64_t rec_gen = old_gen + 1U;
    std::weak_ptr<std::atomic_bool> weak_alive = alive_token;

    if (!recovery_pool) {
      {
        std::lock_guard lock(state_mtx);
        unit->status.failed_reason = reason;
      }

      transition_unit(unit, launcher::UnitState::kFailed, "on_fault", detail);
      enqueue_schedule();
      return;
    }

    (void)recovery_pool->post_task(
        [this, weak_alive, unit, old_gen, rec_gen, reason = std::move(reason), detail = std::move(detail)]() mutable {
          auto g = weak_alive.lock();

          if (!g || !g->load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire)) {
            return;
          }

          const uint32_t budget = std::min<uint32_t>(unit->stop_timeout_ms, 2000U);

          if (unit->status.type == launcher::UnitType::kComponent) {
            (void)component_host.stop_component(unit->status.name, budget, old_gen);
          } else {
            (void)process_supervisor.stop_process(unit->status.name, budget, old_gen);
            unit->clear_spill_file(old_gen);
          }

          (void)post_loop([this, weak_alive, unit, rec_gen, reason = std::move(reason), detail = std::move(detail)]() {
            auto g2 = weak_alive.lock();

            if (!g2 || !g2->load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire)) {
              return;
            }

            if (unit->generation.load(std::memory_order_acquire) != rec_gen) {
              return;
            }

            if (unit->respawn.enabled() && !config.disable_respawn) {
              transition_unit(unit, launcher::UnitState::kStopped, "on_stopped", "restarting after " + reason);
              schedule_respawn(unit, rec_gen);
              return;
            }

            {
              std::lock_guard lock(state_mtx);
              unit->status.failed_reason = reason;
            }

            transition_unit(unit, launcher::UnitState::kFailed, "on_fault", detail);
            enqueue_schedule();

            if (unit->status.required) {
              request_async_stop(1);
            }
          });
        });
  }

  void handle_process_exit(const std::string& name, uint64_t gen, int32_t code, Process::ExitStatus status,
                           bool expected_stop) {
    auto unit = find_unit(name);

    if (!unit) {
      return;
    }

    process_supervisor.reap_exited(name, gen);
    notify_server.unregister_unit(name, gen);
    unit->clear_spill_file(gen);

    if (unit->generation.load(std::memory_order_acquire) != gen) {
      return;
    }

    cancel_unit_timers(unit);

    {
      std::lock_guard lock(state_mtx);
      unit->status.exit_code = code;
      unit->status.pid = -1;
    }

    if (expected_stop || stopping.load(std::memory_order_acquire)) {
      return;
    }

    const bool normal_ok = (status == Process::kNormalExitStatus && code == 0);
    const bool want_restart =
        !config.disable_respawn && (unit->respawn.policy == launcher::RestartPolicy::kAlways ||
                                    (unit->respawn.policy == launcher::RestartPolicy::kOnFailure && !normal_ok));

    if (want_restart) {
      transition_unit(unit, launcher::UnitState::kStopped, "on_exited", "restarting");
      schedule_respawn(unit, gen);
      return;
    }

    if (normal_ok) {
      transition_unit(unit, launcher::UnitState::kStopped, "on_exited", "exited cleanly");
    } else {
      {
        std::lock_guard lock(state_mtx);
        unit->status.failed_reason = "exit_error";
      }

      transition_unit(unit, launcher::UnitState::kFailed, "on_exited", "exited with code " + std::to_string(code));

      if (unit->status.required) {
        request_async_stop(1);
      }
    }

    enqueue_schedule();
  }

  void handle_notify_event(const launcher::NotifyEvent& ev) {
    if (stopping.load(std::memory_order_acquire)) {
      return;
    }

    auto unit = find_unit(ev.unit_name);

    if (!unit || unit->status.type != launcher::UnitType::kProcess) {
      return;
    }

    const uint64_t now = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);
    bool dirty = false;
    bool should_ready = false;
    bool should_extend = false;
    bool claimed_trigger = false;
    bool emit_reload = false;
    bool emit_stopping = false;
    launcher::EventRecord reload_ev;
    launcher::EventRecord stopping_ev;
    launcher::EventRecord trigger_ev;
    std::string ready_detail;
    std::string trigger_detail;

    {
      std::lock_guard lock(state_mtx);
      const auto st = unit->status.state;

      if (unit->generation.load(std::memory_order_acquire) != ev.generation || !is_active_state(st)) {
        return;
      }

      if (ev.watchdog_ping) {
        unit->last_watchdog_ping_ms = now;
      }

      if (ev.has_health) {
        unit->status.healthy = ev.healthy;
        dirty = true;
      }

      if (ev.has_status) {
        unit->status.detail = ev.status;
        dirty = true;
      }

      if (ev.reloading) {
        emit_reload = true;
        reload_ev.timestamp_ms = ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMilli);
        reload_ev.unit_name = unit->status.name;
        reload_ev.unit_type = unit->status.type;
        reload_ev.old_state = st;
        reload_ev.new_state = st;
        reload_ev.event_name = "on_reloading";
        reload_ev.message = ev.has_status && !ev.status.empty() ? ev.status : "reloading";
      }

      if (ev.stopping) {
        emit_stopping = true;
        stopping_ev.timestamp_ms = ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMilli);
        stopping_ev.unit_name = unit->status.name;
        stopping_ev.unit_type = unit->status.type;
        stopping_ev.old_state = st;
        stopping_ev.new_state = st;
        stopping_ev.event_name = "on_stopping";
        stopping_ev.message = ev.has_status && !ev.status.empty() ? ev.status : "stopping";
      }

      if (ev.watchdog_trigger) {
        unit->generation.fetch_add(1U, std::memory_order_acq_rel);
        unit->status.state = launcher::UnitState::kStopping;
        unit->status.healthy = false;
        unit->status.failed_reason = "watchdog_trigger";
        trigger_detail = ev.has_status && !ev.status.empty() ? ev.status : "watchdog triggered";
        unit->status.detail = trigger_detail;

        trigger_ev.timestamp_ms = ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMilli);
        trigger_ev.unit_name = unit->status.name;
        trigger_ev.unit_type = unit->status.type;
        trigger_ev.old_state = st;
        trigger_ev.new_state = launcher::UnitState::kStopping;
        trigger_ev.event_name = "on_watchdog_trigger";
        trigger_ev.message = trigger_detail;
        claimed_trigger = true;
      } else {
        const bool in_startup = (st == launcher::UnitState::kStarting || st == launcher::UnitState::kRunning);

        if (ev.has_extend_timeout && in_startup && unit->ready.type != launcher::ReadyProbeType::kNone &&
            unit->ready.type != launcher::ReadyProbeType::kDelay) {
          should_extend = true;
        }

        if (ev.ready && in_startup &&
            (unit->ready.type == launcher::ReadyProbeType::kNotify ||
             unit->ready.type == launcher::ReadyProbeType::kNone)) {
          should_ready = true;
          ready_detail = ev.has_status && !ev.status.empty() ? ev.status : "notify ready";
        }
      }
    }

    if (emit_reload) {
      publish_event(reload_ev);
    }

    if (emit_stopping) {
      publish_event(stopping_ev);
    }

    if (claimed_trigger) {
      notify_server.unregister_unit(unit->status.name, ev.generation);
      cancel_unit_timers(unit);
      publish_event(trigger_ev);
      publish_snapshot();
      offload_fault_recovery(unit, ev.generation, "watchdog_trigger", std::move(trigger_detail));
      return;
    }

    if (should_extend && wheel_timer && wheel_timer->is_running()) {
      std::weak_ptr<std::atomic_bool> weak_alive = alive_token;
      const std::string uname = unit->status.name;
      const uint64_t gen = ev.generation;
      const int64_t new_key = wheel_timer->add(ev.extend_timeout_ms, [this, weak_alive, uname, gen](WheelTimer::Key) {
        auto g = weak_alive.lock();

        if (!g || !g->load(std::memory_order_acquire)) {
          return;
        }

        (void)post_loop([this, weak_alive, uname, gen]() {
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

      int64_t prev_key = -1;
      bool stale = false;

      {
        std::lock_guard lock(state_mtx);
        const auto st = unit->status.state;

        if (unit->generation.load(std::memory_order_acquire) != gen || stopping.load(std::memory_order_acquire) ||
            (st != launcher::UnitState::kStarting && st != launcher::UnitState::kRunning)) {
          stale = true;
        } else {
          prev_key = unit->ready_timer_key;
          unit->ready_timer_key = new_key;
        }
      }

      if (prev_key >= 0) {
        (void)wheel_timer->remove(prev_key);
      }

      if (stale) {
        (void)wheel_timer->remove(new_key);
      }
    }

    if (should_ready) {
      finish_readiness(unit, ev.generation, true, ready_detail);
    } else if (dirty) {
      publish_snapshot();
    }
  }

  void run_housekeeping() {
    if (stopping.load(std::memory_order_acquire)) {
      return;
    }

    const uint64_t now = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);
    std::vector<std::shared_ptr<UnitRuntime>> all_units;

    {
      std::lock_guard lock(state_mtx);
      all_units.reserve(topo_order.size());

      for (const auto& name : topo_order) {
        auto it = units.find(name);

        if (it != units.end()) {
          all_units.push_back(it->second);
        }
      }
    }

    for (const auto& u : all_units) {
      launcher::UnitState st = launcher::UnitState::kPending;
      uint64_t gen = 0U;
      uint64_t armed_ms = 0U;
      uint64_t last_ping = 0U;

      {
        std::lock_guard lock(state_mtx);
        st = u->status.state;
        gen = u->generation.load(std::memory_order_acquire);
        armed_ms = u->watchdog_armed_ms;
        last_ping = u->last_watchdog_ping_ms;

        if (st == launcher::UnitState::kReady && u->backoff_streak > 0U && u->start_steady_ms > 0U &&
            now > u->start_steady_ms && (now - u->start_steady_ms) >= u->respawn.start_window_ms) {
          u->backoff_streak = 0U;
        }
      }

      if (u->watchdog_ms == 0U || st != launcher::UnitState::kReady) {
        continue;
      }

      if (u->status.type == launcher::UnitType::kComponent) {
        last_ping = std::max(last_ping, component_host.get_last_watchdog_ms(u->status.name));
      }

      const uint64_t ref = std::max(last_ping, armed_ms);

      if (ref > 0U && now > ref && (now - ref) > u->watchdog_ms) {
        bool claimed = false;
        launcher::EventRecord wd_ev;

        {
          std::lock_guard lock(state_mtx);

          if (u->generation.load(std::memory_order_acquire) == gen && u->status.state == launcher::UnitState::kReady) {
            u->generation.fetch_add(1U, std::memory_order_acq_rel);
            u->status.state = launcher::UnitState::kStopping;
            u->status.healthy = false;
            u->status.failed_reason = "watchdog";
            u->status.detail = "watchdog timeout";

            wd_ev.timestamp_ms = ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMilli);
            wd_ev.unit_name = u->status.name;
            wd_ev.unit_type = u->status.type;
            wd_ev.old_state = launcher::UnitState::kReady;
            wd_ev.new_state = launcher::UnitState::kStopping;
            wd_ev.event_name = "on_watchdog_timeout";
            wd_ev.message = "watchdog timeout";
            claimed = true;
          }
        }

        if (claimed) {
          if (u->status.type == launcher::UnitType::kProcess) {
            notify_server.unregister_unit(u->status.name, gen);
          }

          cancel_unit_timers(u);
          publish_event(wd_ev);
          publish_snapshot();
          offload_fault_recovery(u, gen, "watchdog", "watchdog timeout");
        }
      }
    }

    if (now >= last_hb_pub_ms + launcher::kHeartbeatIntervalMs) {
      last_hb_pub_ms = now;
      publish_heartbeat();
      publish_snapshot();
    }
  }

  launcher::CommandResponse handle_rpc(const launcher::CommandRequest& req) const {
    launcher::CommandResponse resp;
    resp.proto_version = launcher::kProtocolVersion;

    if VUNLIKELY (req.proto_version != launcher::kProtocolVersion) {
      resp.result.code = launcher::ErrorCode::kVersionMismatch;
      resp.result.message = "protocol version mismatch";
      return resp;
    }

    switch (req.op) {
      case launcher::CommandOp::kGetStatus:
        resp.result.code = launcher::ErrorCode::kOk;
        resp.result.snapshot = owner->get_snapshot();
        resp.has_snapshot = true;
        break;
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
      case launcher::CommandOp::kGetParams:
        resp.result = owner->get_params(req.target);
        break;
      case launcher::CommandOp::kSetParam:
        resp.result = owner->set_param(req.target, req.param);
        break;
      case launcher::CommandOp::kShutdown: {
        if (wheel_timer && wheel_timer->is_running()) {
          std::weak_ptr<std::atomic_bool> weak_alive = alive_token;
          (void)wheel_timer->add(50U, [this, weak_alive](WheelTimer::Key) {
            auto g = weak_alive.lock();

            if (g && g->load(std::memory_order_acquire) && owner != nullptr) {
              owner->request_stop(0);
            }
          });
        } else {
          owner->request_stop(0);
        }

        resp.result.code = launcher::ErrorCode::kOk;
        resp.result.message = "shutdown requested";
        break;
      }
      default:
        resp.result.code = launcher::ErrorCode::kInvalidArgument;
        resp.result.message = "unsupported command operation";
        break;
    }

    return resp;
  }

  bool init_control_plane() {
    if (!config.enable_control_plane || !manifest.control_enabled) {
      return true;
    }

    active_scheme = config.control_scheme_override.empty() ? manifest.control_scheme : config.control_scheme_override;

    if (active_scheme.empty()) {
      active_scheme = std::string(launcher::default_scheme());
    }

    active_ns =
        config.control_namespace_override.empty() ? manifest.control_namespace : config.control_namespace_override;
    active_domain = (config.control_domain_override >= 0) ? static_cast<uint32_t>(config.control_domain_override)
                                                          : manifest.control_domain;
    active_key = config.security_key_override;

    if (active_key.empty()) {
      active_key = Utils::get_env("VLINK_LAUNCHER_SECURITY_KEY");
    }

    if (active_key.empty()) {
      active_key = manifest.security_key;
    }

    const std::string cmd_url = launcher::build_channel_url(active_scheme, active_ns, "ctrl", "method", active_domain);
    const std::string state_url =
        launcher::build_channel_url(active_scheme, active_ns, "state", "field", active_domain);
    const std::string event_url =
        launcher::build_channel_url(active_scheme, active_ns, "events", "event", active_domain);
    const std::string hb_url = launcher::build_channel_url(active_scheme, active_ns, "hb", "clock", active_domain);

    auto new_ctrl_loop = std::make_shared<MultiLoop>(2U);
    new_ctrl_loop->set_name("vl_ctrl_rpc");
    (void)new_ctrl_loop->async_run();

    auto rpc_cb = [this](const launcher::CommandRequest& req, launcher::CommandResponse& resp) {
      resp = handle_rpc(req);
    };

    std::lock_guard lock(ctrl_mtx);
    ctrl_loop = new_ctrl_loop;
#ifdef VLINK_ENABLE_SECURITY
    use_security = !active_key.empty() && active_scheme != "intra";

    if (use_security) {
      auto make_sec_cfg = [this](const char* aad) {
        Security::Config sc;
        sc.key = active_key;
        sc.advanced.aad_context = aad;
        return sc;
      };

      sec_ctrl_server =
          std::make_shared<SecServer>(cmd_url, make_sec_cfg("vlink.launcher.ctrl"), InitType::kWithoutInit);
      sec_state_setter =
          std::make_shared<SecSetter>(state_url, make_sec_cfg("vlink.launcher.state"), InitType::kWithoutInit);
      sec_event_pub =
          std::make_shared<SecEventPub>(event_url, make_sec_cfg("vlink.launcher.events"), InitType::kWithoutInit);
      sec_hb_pub = std::make_shared<SecHbPub>(hb_url, make_sec_cfg("vlink.launcher.hb"), InitType::kWithoutInit);

      if (!sec_ctrl_server->init() || !sec_state_setter->init() || !sec_event_pub->init() || !sec_hb_pub->init()) {
        return false;
      }

      (void)sec_ctrl_server->attach(new_ctrl_loop.get());
      return sec_ctrl_server->listen(std::move(rpc_cb));
    }
#else
    if (!active_key.empty() && active_scheme != "intra") {
      VLOG_E("LauncherServer: security_key configured but VLink was built without VLINK_ENABLE_SECURITY.");
      return false;
    }
#endif

    ctrl_server = std::make_shared<PlainServer>(cmd_url, InitType::kWithoutInit);
    state_setter = std::make_shared<PlainSetter>(state_url, InitType::kWithoutInit);
    event_pub = std::make_shared<PlainEventPub>(event_url, InitType::kWithoutInit);
    hb_pub = std::make_shared<PlainHbPub>(hb_url, InitType::kWithoutInit);

    if (!ctrl_server->init() || !state_setter->init() || !event_pub->init() || !hb_pub->init()) {
      return false;
    }

    (void)ctrl_server->attach(new_ctrl_loop.get());
    return ctrl_server->listen(std::move(rpc_cb));
  }

  bool is_in_ctrl_thread() const {
    std::shared_ptr<MultiLoop> loop;

    {
      std::lock_guard lock(ctrl_mtx);
      loop = ctrl_loop;
    }

    return loop && loop->is_in_same_thread();
  }

  void shutdown_control_plane() {
    std::shared_ptr<MultiLoop> old_ctrl_loop;

    {
      std::lock_guard lock(ctrl_mtx);
      old_ctrl_loop.swap(ctrl_loop);
      ctrl_server.reset();
      state_setter.reset();
      event_pub.reset();
      hb_pub.reset();
#ifdef VLINK_ENABLE_SECURITY
      sec_ctrl_server.reset();
      sec_state_setter.reset();
      sec_event_pub.reset();
      sec_hb_pub.reset();
#endif
    }

    if (old_ctrl_loop) {
      old_ctrl_loop->quit(true);
      old_ctrl_loop->wait_for_quit();
      old_ctrl_loop.reset();
    }
  }

  void do_stop() {
    if (stop_executed.exchange(true, std::memory_order_acq_rel)) {
      return;
    }

    stopping.store(true, std::memory_order_release);
    shutdown_control_plane();

    {
      std::unique_ptr<Timer> hk;

      {
        std::lock_guard lock(timer_mtx);
        hk.swap(housekeep_timer);
      }

      if (hk) {
        hk->stop();
        hk.reset();
      }
    }

    if (wheel_timer) {
      wheel_timer->stop();
    }

    if (recovery_pool) {
      recovery_pool->shutdown();
      recovery_pool.reset();
    }

    std::vector<std::string> rev_order;

    {
      std::lock_guard lock(state_mtx);
      rev_order.assign(topo_order.rbegin(), topo_order.rend());
    }

    const uint64_t shutdown_deadline_ms =
        ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli) + manifest.shutdown_timeout_ms;

    for (const auto& name : rev_order) {
      auto u = find_unit(name);

      if (u) {
        const uint64_t now_ms = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);
        const uint32_t rem_ms = (shutdown_deadline_ms > now_ms)
                                    ? static_cast<uint32_t>(std::max<uint64_t>(200ULL, shutdown_deadline_ms - now_ms))
                                    : 200U;
        (void)stop_unit_internal(u, rem_ms);
      }
    }

    process_supervisor.stop_all();
    notify_server.stop();
    component_host.stop_all();
    launcher::wipe_secret_string(active_key);

    if (alive_token) {
      alive_token->store(false, std::memory_order_release);
    }

    {
      std::lock_guard lock(stop_mtx);
      running.store(false, std::memory_order_release);
    }

    stop_cv.notify_all();

    if (owner != nullptr && owner->MessageLoop::is_running()) {
      owner->quit(false);
    }
  }
};

LauncherServer::LauncherServer(const Config& config) : impl_(std::make_unique<Impl>(this, config)) {}

LauncherServer::LauncherServer() : LauncherServer(Config{}) {}

LauncherServer::~LauncherServer() { stop(); }

bool LauncherServer::load_file(const std::string& file_path, std::string* error_out) {
  std::lock_guard lock(impl_->start_mtx);

  if (impl_->running.load(std::memory_order_acquire)) {
    if (error_out != nullptr) {
      *error_out = "Cannot load manifest while LauncherServer is running.";
    }

    return false;
  }

  launcher::LaunchManifest parsed;
  std::string err;

  if (!launcher::load_manifest_file(file_path, parsed, err, impl_->config.args_override)) {
    if (error_out != nullptr) {
      *error_out = err;
    }

    return false;
  }

  impl_->manifest = std::move(parsed);
  impl_->manifest_loaded = true;
  impl_->rebuild_unit_table();
  return true;
}

bool LauncherServer::load_string(const std::string& toml_content, const std::string& base_dir, std::string* error_out) {
  std::lock_guard lock(impl_->start_mtx);

  if (impl_->running.load(std::memory_order_acquire)) {
    if (error_out != nullptr) {
      *error_out = "Cannot load manifest while LauncherServer is running.";
    }

    return false;
  }

  launcher::LaunchManifest parsed;
  std::string err;

  if (!launcher::load_manifest_string(toml_content, base_dir, parsed, err, impl_->config.args_override)) {
    if (error_out != nullptr) {
      *error_out = err;
    }

    return false;
  }

  impl_->manifest = std::move(parsed);
  impl_->manifest_loaded = true;
  impl_->rebuild_unit_table();
  return true;
}

std::string LauncherServer::validate(bool check_programs) const {
  std::lock_guard lock(impl_->start_mtx);

  if (!impl_->manifest_loaded) {
    return "No manifest loaded.";
  }

  auto rep = launcher::validate_manifest(impl_->manifest, check_programs);
  auto check_filter_targets = [&](const std::vector<std::string>& specs, std::string_view flag_name) {
    std::lock_guard slock(impl_->state_mtx);

    for (const auto& spec : specs) {
      if (!spec.empty() && spec[0] == '@') {
        if (impl_->group_members.count(spec.substr(1U)) == 0U) {
          rep.errors.push_back(std::string(flag_name) + " references unknown group '" + spec + "'.");
        }
      } else if (impl_->units.count(spec) == 0U) {
        rep.errors.push_back(std::string(flag_name) + " references unknown unit '" + spec + "'.");
      }
    }
  };

  check_filter_targets(impl_->config.only_targets, "--only");
  check_filter_targets(impl_->config.skip_targets, "--skip");
  return rep.format();
}

void LauncherServer::register_event_callback(EventCallback&& callback) {
  auto next = callback ? std::make_shared<EventCallback>(std::move(callback)) : nullptr;
  std::shared_ptr<EventCallback> old;

  {
    std::lock_guard lock(impl_->cb_mtx);
    old = std::move(impl_->local_event_cb);
    impl_->local_event_cb = std::move(next);
  }
}

bool LauncherServer::start() {
  std::lock_guard lock(impl_->start_mtx);

  if (!impl_->manifest_loaded) {
    VLOG_E("LauncherServer: No manifest loaded.");
    return false;
  }

  if (impl_->running.load(std::memory_order_acquire)) {
    return true;
  }

  const std::string verr = launcher::validate_manifest(impl_->manifest, false).format();

  if (!verr.empty()) {
    VLOG_E("LauncherServer: Manifest validation failed:\n", verr);
    return false;
  }

  if (!is_in_same_thread() && MessageLoop::is_running()) {
    wait_for_quit();
  }

  if (impl_->alive_token) {
    impl_->alive_token->store(false, std::memory_order_release);
  }

  impl_->alive_token = std::make_shared<std::atomic_bool>(true);
  impl_->rebuild_unit_table();
  impl_->stopping.store(false, std::memory_order_release);
  impl_->stop_executed.store(false, std::memory_order_release);
  impl_->exit_code.store(0, std::memory_order_release);

  {
    std::lock_guard slock(impl_->state_mtx);
    impl_->session_id = make_session_id();
    impl_->server_start_steady_ms = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);
    impl_->last_hb_pub_ms = 0U;
  }

  impl_->component_host.set_plugin_dirs(impl_->manifest.plugin_dirs);
  impl_->recovery_pool = std::make_unique<ThreadPool>(2U);
  impl_->recovery_pool->set_name("vl_recovery");

  std::weak_ptr<std::atomic_bool> weak_alive = impl_->alive_token;

  impl_->component_host.set_callbacks(
      [this, weak_alive](const std::string& name, uint64_t gen, const std::string& detail) {
        auto g = weak_alive.lock();

        if (!g || !g->load(std::memory_order_acquire)) {
          return;
        }

        (void)impl_->post_loop([this, weak_alive, name, gen, detail]() {
          auto g2 = weak_alive.lock();

          if (!g2 || !g2->load(std::memory_order_acquire)) {
            return;
          }

          auto u = impl_->find_unit(name);

          if (u && (u->ready.type == launcher::ReadyProbeType::kComponent ||
                    u->ready.type == launcher::ReadyProbeType::kNone)) {
            impl_->finish_readiness(u, gen, true, detail.empty() ? "component ready" : detail);
          }
        });
      },
      [this, weak_alive](const std::string& name, uint64_t gen, bool healthy, const std::string& detail) {
        auto g = weak_alive.lock();

        if (!g || !g->load(std::memory_order_acquire)) {
          return;
        }

        (void)impl_->post_loop([this, weak_alive, name, gen, healthy, detail]() {
          auto g2 = weak_alive.lock();

          if (!g2 || !g2->load(std::memory_order_acquire)) {
            return;
          }

          auto u = impl_->find_unit(name);

          if (!u) {
            return;
          }

          {
            std::lock_guard slock(impl_->state_mtx);

            if (u->generation.load(std::memory_order_acquire) != gen) {
              return;
            }

            u->status.healthy = healthy;

            if (!detail.empty()) {
              u->status.detail = detail;
            }
          }

          impl_->publish_snapshot();
        });
      },
      [this, weak_alive](const std::string& name, uint64_t gen) {
        auto g = weak_alive.lock();

        if (!g || !g->load(std::memory_order_acquire)) {
          return;
        }

        (void)impl_->post_loop([this, weak_alive, name, gen]() {
          auto g2 = weak_alive.lock();

          if (!g2 || !g2->load(std::memory_order_acquire)) {
            return;
          }

          auto u = impl_->find_unit(name);

          if (u && u->generation.load(std::memory_order_acquire) == gen) {
            (void)impl_->stop_unit_internal(u);
            impl_->enqueue_schedule();
          }
        });
      });

  impl_->process_supervisor.set_callbacks(
      [this, weak_alive](const std::string& name, uint64_t gen, int32_t code, Process::ExitStatus status,
                         bool expected_stop) {
        auto g = weak_alive.lock();

        if (!g || !g->load(std::memory_order_acquire)) {
          return;
        }

        (void)impl_->post_loop([this, weak_alive, name, gen, code, status, expected_stop]() {
          auto g2 = weak_alive.lock();

          if (g2 && g2->load(std::memory_order_acquire)) {
            impl_->handle_process_exit(name, gen, code, status, expected_stop);
          }
        });
      },
      [this, weak_alive](const std::string& name, uint64_t gen) {
        auto g = weak_alive.lock();

        if (!g || !g->load(std::memory_order_acquire)) {
          return;
        }

        (void)impl_->post_loop([this, weak_alive, name, gen]() {
          auto g2 = weak_alive.lock();

          if (!g2 || !g2->load(std::memory_order_acquire)) {
            return;
          }

          auto u = impl_->find_unit(name);

          if (u) {
            impl_->finish_readiness(u, gen, true, "stdout matched");
          }
        });
      });

  if (!impl_->notify_server.start("", [this, weak_alive](launcher::NotifyEvent&& ev) {
        auto g = weak_alive.lock();

        if (!g || !g->load(std::memory_order_acquire)) {
          return;
        }

        (void)impl_->post_loop([this, weak_alive, ev = std::move(ev)]() {
          auto g2 = weak_alive.lock();

          if (g2 && g2->load(std::memory_order_acquire)) {
            impl_->handle_notify_event(ev);
          }
        });
      })) {
    VLOG_W("LauncherServer: NotifyServer failed to start; process notify/watchdog channel disabled.");
  }

  if (!impl_->manifest.name.empty()) {
    set_name(impl_->manifest.name.substr(0U, 15U));
  }

  impl_->wheel_timer->start();

  auto rollback_start = [&]() {
    if (impl_->alive_token) {
      impl_->alive_token->store(false, std::memory_order_release);
    }

    impl_->component_host.set_callbacks(nullptr, nullptr, nullptr);
    impl_->process_supervisor.set_callbacks(nullptr, nullptr);
    impl_->shutdown_control_plane();
    impl_->notify_server.stop();
    impl_->wheel_timer->stop();

    if (impl_->recovery_pool) {
      impl_->recovery_pool->shutdown();
      impl_->recovery_pool.reset();
    }

    {
      std::lock_guard slock(impl_->stop_mtx);
      impl_->running.store(false, std::memory_order_release);
    }

    impl_->stop_cv.notify_all();
  };

  if (!impl_->init_control_plane()) {
    rollback_start();
    return false;
  }

  {
    std::lock_guard slock(impl_->stop_mtx);
    impl_->running.store(true, std::memory_order_release);
  }

  if (!async_run()) {
    rollback_start();
    return false;
  }

  if (impl_->stopping.load(std::memory_order_acquire)) {
    quit(false);
    wait_for_quit();
    return false;
  }

  impl_->publish_heartbeat();
  impl_->publish_snapshot();
  impl_->enqueue_schedule();

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
    if (!is_in_same_thread() && !impl_->is_in_ctrl_thread() && MessageLoop::is_running()) {
      quit(false);
      wait_for_quit();
    }

    return;
  }

  if (is_in_same_thread() || impl_->is_in_ctrl_thread()) {
    impl_->request_async_stop(0);
    return;
  }

  impl_->request_async_stop(0);
  (void)wait(-1);

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
    impl_->stop_cv.wait(lock, [this]() { return !impl_->running.load(std::memory_order_acquire); });
    return true;
  }

  return impl_->stop_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                 [this]() { return !impl_->running.load(std::memory_order_acquire); });
}

bool LauncherServer::is_active() const noexcept { return impl_->running.load(std::memory_order_acquire); }

int32_t LauncherServer::get_exit_code() const noexcept { return impl_->exit_code.load(std::memory_order_acquire); }

launcher::SystemSnapshot LauncherServer::get_snapshot() const { return impl_->build_snapshot(); }

std::vector<std::string> LauncherServer::startup_order() const {
  std::lock_guard lock(impl_->state_mtx);
  std::vector<std::string> out;
  out.reserve(impl_->topo_order.size());

  for (const auto& name : impl_->topo_order) {
    auto it = impl_->units.find(name);

    if (it != impl_->units.end() && it->second->autostart) {
      out.push_back(name);
    }
  }

  return out;
}

launcher::CommandResult LauncherServer::start_unit(const std::string& target) {
  std::lock_guard lock(impl_->op_mtx);

  if (!impl_->running.load(std::memory_order_acquire) || impl_->stopping.load(std::memory_order_acquire)) {
    return {launcher::ErrorCode::kNotRunning, "LauncherServer is not running.", std::nullopt, {}};
  }

  const auto targets = impl_->expand_target(target);

  if (targets.empty()) {
    return {launcher::ErrorCode::kNotFound, "Unit or group '" + target + "' not found.", std::nullopt, {}};
  }

  for (const auto& name : targets) {
    auto u = impl_->find_unit(name);

    if (!u) {
      continue;
    }

    {
      std::lock_guard slock(impl_->state_mtx);

      if (is_active_state(u->status.state)) {
        if (targets.size() == 1U) {
          return {launcher::ErrorCode::kAlreadyRunning, "Unit '" + name + "' is already running.", std::nullopt, {}};
        }

        continue;
      }

      u->backoff_streak = 0U;
      u->window_attempts = 0U;
      u->window_start_ms = 0U;
    }

    std::string err;

    if (!impl_->launch_unit_now(u, &err)) {
      return {launcher::ErrorCode::kInternalError, err.empty() ? "Failed to start unit." : err, std::nullopt, {}};
    }
  }

  return {launcher::ErrorCode::kOk, "started", std::nullopt, {}};
}

launcher::CommandResult LauncherServer::stop_unit(const std::string& target) {
  std::lock_guard lock(impl_->op_mtx);

  if (!impl_->running.load(std::memory_order_acquire) || impl_->stopping.load(std::memory_order_acquire)) {
    return {launcher::ErrorCode::kNotRunning, "LauncherServer is not running.", std::nullopt, {}};
  }

  const auto targets = impl_->expand_target(target);

  if (targets.empty()) {
    return {launcher::ErrorCode::kNotFound, "Unit or group '" + target + "' not found.", std::nullopt, {}};
  }

  bool any_stopped = false;

  for (auto it = targets.rbegin(); it != targets.rend(); ++it) {
    auto u = impl_->find_unit(*it);

    if (u && impl_->stop_unit_internal(u)) {
      any_stopped = true;
    }
  }

  if (targets.size() == 1U && !any_stopped) {
    return {launcher::ErrorCode::kNotRunning, "Unit '" + targets[0] + "' is not running.", std::nullopt, {}};
  }

  impl_->enqueue_schedule();
  return {launcher::ErrorCode::kOk, "stopped", std::nullopt, {}};
}

launcher::CommandResult LauncherServer::restart_unit(const std::string& target) {
  std::lock_guard lock(impl_->op_mtx);

  if (!impl_->running.load(std::memory_order_acquire) || impl_->stopping.load(std::memory_order_acquire)) {
    return {launcher::ErrorCode::kNotRunning, "LauncherServer is not running.", std::nullopt, {}};
  }

  auto stop_res = stop_unit(target);

  if (!stop_res.ok() && stop_res.code != launcher::ErrorCode::kNotRunning) {
    return stop_res;
  }

  return start_unit(target);
}

launcher::CommandResult LauncherServer::reload_unit(const std::string& target) {
  std::lock_guard lock(impl_->op_mtx);

  if (!impl_->running.load(std::memory_order_acquire) || impl_->stopping.load(std::memory_order_acquire)) {
    return {launcher::ErrorCode::kNotRunning, "LauncherServer is not running.", std::nullopt, {}};
  }

  const auto targets = impl_->expand_target(target);

  if (targets.empty()) {
    return {launcher::ErrorCode::kNotFound, "Unit or group '" + target + "' not found.", std::nullopt, {}};
  }

  for (const auto& name : targets) {
    auto u = impl_->find_unit(name);

    if (!u) {
      continue;
    }

    launcher::UnitState cur_state = launcher::UnitState::kPending;

    {
      std::lock_guard slock(impl_->state_mtx);
      cur_state = u->status.state;
    }

    if (!is_active_state(cur_state)) {
      if (targets.size() == 1U) {
        return {launcher::ErrorCode::kNotRunning, "Unit '" + name + "' is not running.", std::nullopt, {}};
      }

      continue;
    }

    if (u->status.type == launcher::UnitType::kComponent) {
      std::string err;

      if (!impl_->component_host.reload_component(name, &err)) {
        return {launcher::ErrorCode::kInternalError, err, std::nullopt, {}};
      }

      launcher::EventRecord ev;
      ev.timestamp_ms = ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMilli);
      ev.unit_name = name;
      ev.unit_type = launcher::UnitType::kComponent;
      ev.old_state = cur_state;
      ev.new_state = cur_state;
      ev.event_name = "on_reloaded";
      ev.message = "reloaded";
      impl_->publish_event(ev);
    } else {
      if (!impl_->process_supervisor.reload_process(name)) {
        return {launcher::ErrorCode::kUnsupported,
                "Failed to send reload signal to process '" + name + "'.",
                std::nullopt,
                {}};
      }
    }
  }

  return {launcher::ErrorCode::kOk, "reloaded", std::nullopt, {}};
}

launcher::CommandResult LauncherServer::get_params(const std::string& unit_name) {
  std::lock_guard lock(impl_->op_mtx);
  launcher::CommandResult res;
  res.code = launcher::ErrorCode::kOk;

  auto collect = [&](const std::string& uname, const std::string& prefix) {
    auto pmap = impl_->component_host.get_params(uname);

    if (!pmap.has_value()) {
      auto u = impl_->find_unit(uname);

      if (u) {
        std::lock_guard slock(impl_->state_mtx);

        if (u->status.type == launcher::UnitType::kComponent) {
          pmap = u->comp_spec.params;
        } else if (u->status.type == launcher::UnitType::kProcess) {
          pmap = u->proc_spec.params;
        }
      }
    }

    if (!pmap.has_value()) {
      return false;
    }

    for (const auto& [k, v] : *pmap) {
      res.params.push_back(launcher::param_value_to_entry(prefix + k, v));
    }

    return true;
  };

  if (!unit_name.empty()) {
    if (!collect(unit_name, "")) {
      return {launcher::ErrorCode::kNotFound, "Unit '" + unit_name + "' not found.", std::nullopt, {}};
    }

    return res;
  }

  std::vector<std::string> names;

  {
    std::lock_guard slock(impl_->state_mtx);
    names = impl_->topo_order;
  }

  for (const auto& n : names) {
    (void)collect(n, n + ".");
  }

  return res;
}

launcher::CommandResult LauncherServer::set_param(const std::string& unit_name, const launcher::ParamEntry& entry) {
  std::lock_guard lock(impl_->op_mtx);

  if (!impl_->running.load(std::memory_order_acquire) || impl_->stopping.load(std::memory_order_acquire)) {
    return {launcher::ErrorCode::kNotRunning, "LauncherServer is not running.", std::nullopt, {}};
  }

  auto u = impl_->find_unit(unit_name);

  if (!u) {
    return {launcher::ErrorCode::kNotFound, "Unit '" + unit_name + "' not found.", std::nullopt, {}};
  }

  if (u->status.type != launcher::UnitType::kComponent) {
    return {launcher::ErrorCode::kUnsupported,
            "Live set_param is supported for in-process components only.",
            std::nullopt,
            {}};
  }

  LauncherParamValue val;

  if (!launcher::entry_to_param_value(entry, val)) {
    return {
        launcher::ErrorCode::kInvalidArgument, "Invalid parameter value for '" + entry.key + "'.", std::nullopt, {}};
  }

  std::string err;

  if (!impl_->component_host.set_param(unit_name, entry.key, val, &err)) {
    return {launcher::ErrorCode::kInternalError, err, std::nullopt, {}};
  }

  {
    std::lock_guard slock(impl_->state_mtx);
    u->comp_spec.params[entry.key] = std::move(val);
  }

  return {launcher::ErrorCode::kOk, "updated", std::nullopt, {}};
}

void LauncherServer::on_begin() {
  std::lock_guard lock(impl_->timer_mtx);

  if (!impl_->stopping.load(std::memory_order_acquire)) {
    impl_->housekeep_timer =
        std::make_unique<Timer>(this, 500, Timer::kInfinite, [this]() { impl_->run_housekeeping(); });
    impl_->housekeep_timer->start();
  }
}

void LauncherServer::on_end() { impl_->do_stop(); }

}  // namespace vlink
