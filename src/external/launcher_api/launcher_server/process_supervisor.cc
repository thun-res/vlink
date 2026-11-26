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

#include "./process_supervisor.h"

#include <vlink/base/logger.h>
#include <vlink/base/memory_resource.h>
//
#include <nlohmann/json.hpp>
//
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../launcher_common.h"

namespace vlink {

namespace launcher {

ProcessSupervisor::ProcessSupervisor(LogManager& log_manager, ResourceController& resource_ctrl)
    : log_manager_(log_manager), resource_ctrl_(resource_ctrl) {}

ProcessSupervisor::~ProcessSupervisor() { stop_all(); }

void ProcessSupervisor::set_callbacks(ExitCallback&& on_exit, StdoutReadyCallback&& on_stdout_ready) {
  std::lock_guard lock(mtx_);
  on_exit_ = std::move(on_exit);
  on_stdout_ready_ = std::move(on_stdout_ready);
}

void ProcessSupervisor::reset_stopped() {
  std::lock_guard lock(mtx_);
  stopped_ = false;
  cancelled_starting_.clear();
}

bool ProcessSupervisor::start_process(const ManagedProcessLaunch& spec, int64_t* pid_out, std::string* cgroup_out,
                                      std::string* error_out) {
  if (spec.program.empty()) {
    if (error_out != nullptr) {
      *error_out = "Process '" + spec.name + "' has empty program.";
    }

    return false;
  }

  std::unique_ptr<Entry> stale_entry;

  {
    std::lock_guard lock(mtx_);

    if (stopped_) {
      if (error_out != nullptr) {
        *error_out = "ProcessSupervisor is stopped.";
      }

      return false;
    }

    if (starting_.count(spec.name) != 0U) {
      if (error_out != nullptr) {
        *error_out = "Process '" + spec.name + "' is already starting.";
      }

      return false;
    }

    auto it = entries_.find(spec.name);

    if (it != entries_.end()) {
      if (it->second && it->second->proc && it->second->proc->is_running()) {
        if (error_out != nullptr) {
          *error_out = "Process '" + spec.name + "' is already running.";
        }

        return false;
      }

      stale_entry = std::move(it->second);
      entries_.erase(it);
    }

    starting_[spec.name] = spec.generation;
    cancelled_starting_.erase(spec.name);
  }

  auto release_starting = [&]() {
    {
      std::lock_guard lock(mtx_);
      auto sit = starting_.find(spec.name);

      if (sit != starting_.end() && sit->second == spec.generation) {
        starting_.erase(sit);
      }

      auto cit = cancelled_starting_.find(spec.name);

      if (cit != cancelled_starting_.end() && (cit->second == 0U || cit->second == spec.generation)) {
        cancelled_starting_.erase(cit);
      }
    }

    starting_cv_.notify_all();
  };

  if (stale_entry) {
    destroy_entry(std::move(stale_entry), 1000U);
  }

  std::string effective_program = spec.program;
  std::vector<std::string> effective_args;

  if (!spec.launch_prefix.empty()) {
    auto prefix_tokens = split_command_prefix(spec.launch_prefix);

    if (!prefix_tokens.empty()) {
      effective_program = std::move(prefix_tokens[0]);
      effective_args.reserve(prefix_tokens.size() + spec.args.size());

      for (size_t i = 1U; i < prefix_tokens.size(); ++i) {
        effective_args.push_back(std::move(prefix_tokens[i]));
      }

      effective_args.push_back(spec.program);
      effective_args.insert(effective_args.end(), spec.args.begin(), spec.args.end());
    } else {
      effective_args = spec.args;
    }
  } else {
    effective_args = spec.args;
  }

  auto entry = std::make_unique<Entry>();
  entry->state = MemoryResource::make_shared<CallbackState>();
  entry->state->spec = spec;
  entry->proc = std::make_unique<Process>();
  entry->state->proc = entry->proc.get();
  entry->proc->set_inherit_environment(spec.inherit_env);

  if (!spec.cwd.empty()) {
    entry->proc->set_working_directory(spec.cwd);
  }

  auto env = log_manager_.build_child_log_env(spec.name, spec.is_container, spec.inherit_env);

  for (const auto& [k, v] : spec.env) {
    if (k == "VLINK_PLUGIN_DIR" && env.count(k) != 0U && !env[k].empty() && !v.empty()) {
      env[k] = v + "," + env[k];
    } else {
      env[k] = v;
    }
  }

  if (!write_remap_file(spec.name, spec.remaps, entry->remap_file)) {
    release_starting();

    if (error_out != nullptr) {
      *error_out = "Failed to write URL remap file for process '" + spec.name + "'.";
    }

    return false;
  }

  if (!entry->remap_file.empty()) {
    env["VLINK_URL_REMAP"] = entry->remap_file;
  }

  entry->proc->set_environment(env);
  entry->proc->set_process_mode(Process::kSeparateMode);

  std::weak_ptr<CallbackState> weak_state = entry->state;

  entry->proc->register_ready_read_stdout_callback([this, weak_state]() {
    auto locked = weak_state.lock();

    if (locked) {
      drain_stream(locked, false);
    }
  });

  entry->proc->register_ready_read_stderr_callback([this, weak_state]() {
    auto locked = weak_state.lock();

    if (locked) {
      drain_stream(locked, true);
    }
  });

  entry->proc->register_finished_callback([this, weak_state](int exit_code, Process::ExitStatus status) {
    auto locked = weak_state.lock();

    if (!locked) {
      return;
    }

    drain_stream(locked, false);
    drain_stream(locked, true);

    std::string rem_out;
    std::string rem_err;

    {
      std::lock_guard read_lock(locked->read_mtx);
      rem_out = std::move(locked->stdout_buf);
      rem_err = std::move(locked->stderr_buf);
      locked->stdout_buf.clear();
      locked->stderr_buf.clear();

      if (locked->proc != nullptr) {
        std::string extra_out;
        std::string extra_err;

        if (locked->proc->read_all_output(extra_out)) {
          rem_out.append(extra_out);
        }

        if (locked->proc->read_all_error(extra_err)) {
          rem_err.append(extra_err);
        }
      }
    }

    if (!rem_out.empty()) {
      log_manager_.emit_unit_output(locked->spec.name, rem_out, false, locked->spec.output, locked->spec.is_container);

      if (!locked->spec.ready_stdout_pattern.empty() && !locked->ready_matched.load(std::memory_order_acquire) &&
          rem_out.find(locked->spec.ready_stdout_pattern) != std::string::npos &&
          !locked->ready_matched.exchange(true, std::memory_order_acq_rel)) {
        StdoutReadyCallback rcb;

        {
          std::lock_guard lock(mtx_);
          rcb = on_stdout_ready_;
        }

        if (rcb) {
          rcb(locked->spec.name, locked->spec.generation);
        }
      }
    }

    if (!rem_err.empty()) {
      log_manager_.emit_unit_output(locked->spec.name, rem_err, true, locked->spec.output, locked->spec.is_container);
    }

    std::string cg_copy;

    {
      std::lock_guard read_lock(locked->read_mtx);
      cg_copy = locked->cgroup_path;
    }

    if (!cg_copy.empty() &&
        (locked->spec.kill_mode == KillMode::kMixed || locked->spec.kill_mode == KillMode::kControlGroup)) {
      (void)resource_ctrl_.force_kill(-1, cg_copy, locked->spec.kill_mode);
      resource_ctrl_.cleanup_unit(locked->spec.name, cg_copy);
    }

    ExitCallback exit_cb;

    {
      std::lock_guard lock(mtx_);
      exit_cb = on_exit_;
    }

    if (exit_cb) {
      exit_cb(locked->spec.name, locked->spec.generation, static_cast<int32_t>(exit_code), status,
              locked->expected_stop.load(std::memory_order_acquire));
    }
  });

  Process* raw_proc = entry->proc.get();
  raw_proc->start(effective_program, effective_args);

  if (!raw_proc->wait_for_started(Process::kDefaultWaitTimeoutMs)) {
    const auto err = raw_proc->get_error();
    destroy_entry(std::move(entry), 1000U);
    release_starting();

    if (error_out != nullptr) {
      *error_out = "Failed to start process '" + spec.name + "' (" + effective_program +
                   "), error=" + std::to_string(static_cast<int>(err));
    }

    return false;
  }

  const int64_t pid = raw_proc->get_process_id();
  const auto apply_res =
      resource_ctrl_.apply_to_process(spec.name, spec.generation, pid, spec.resources, spec.kill_mode);
  entry->cgroup_path = apply_res.cgroup_path;

  {
    std::lock_guard read_lock(entry->state->read_mtx);
    entry->state->cgroup_path = apply_res.cgroup_path;
  }

  if (!apply_res.ok) {
    destroy_entry(std::move(entry), 1000U);
    release_starting();

    if (error_out != nullptr) {
      *error_out = apply_res.error;
    }

    return false;
  }

  if (!apply_res.warning.empty()) {
    log_manager_.emit_launcher_message(spec.name, Logger::kWarn, apply_res.warning);
  }

  std::unique_ptr<Entry> abort_entry;
  bool cancelled_by_stop = false;

  {
    std::lock_guard lock(mtx_);
    auto cit = cancelled_starting_.find(spec.name);

    if (cit != cancelled_starting_.end() && (cit->second == 0U || cit->second == spec.generation)) {
      cancelled_by_stop = true;
      cancelled_starting_.erase(cit);
    }

    if (stopped_ || cancelled_by_stop) {
      abort_entry = std::move(entry);
    } else {
      auto sit = starting_.find(spec.name);

      if (sit != starting_.end() && sit->second == spec.generation) {
        starting_.erase(sit);
      }

      entries_[spec.name] = std::move(entry);
    }
  }

  if (abort_entry) {
    destroy_entry(std::move(abort_entry), 1000U);

    {
      std::lock_guard lock(mtx_);
      auto sit = starting_.find(spec.name);

      if (sit != starting_.end() && sit->second == spec.generation) {
        starting_.erase(sit);
      }
    }

    starting_cv_.notify_all();

    if (error_out != nullptr) {
      *error_out = cancelled_by_stop ? ("Process '" + spec.name + "' stop requested during startup.")
                                     : "ProcessSupervisor stopped during startup.";
    }

    return false;
  }

  starting_cv_.notify_all();

  if (pid_out != nullptr) {
    *pid_out = pid;
  }

  if (cgroup_out != nullptr) {
    *cgroup_out = apply_res.cgroup_path;
  }

  return true;
}

bool ProcessSupervisor::stop_process(const std::string& name, uint32_t sigterm_timeout_ms, uint64_t only_generation) {
  std::unique_ptr<Entry> entry;

  {
    std::unique_lock lock(mtx_);
    auto it = entries_.find(name);

    if (it == entries_.end()) {
      auto sit = starting_.find(name);

      if (sit != starting_.end()) {
        const uint64_t starting_gen = sit->second;

        if (only_generation != 0U && starting_gen != only_generation) {
          return false;
        }

        cancelled_starting_[name] = only_generation;
        (void)starting_cv_.wait_for(lock, std::chrono::milliseconds(std::max<uint32_t>(sigterm_timeout_ms, 5000U)),
                                    [&] {
                                      auto cur = starting_.find(name);
                                      return cur == starting_.end() || cur->second != starting_gen;
                                    });
        it = entries_.find(name);

        if (it == entries_.end()) {
          return true;
        }
      } else {
        return false;
      }
    }

    if (only_generation != 0U && it->second && it->second->state &&
        it->second->state->spec.generation != only_generation) {
      return false;
    }

    entry = std::move(it->second);
    entries_.erase(it);
  }

  destroy_entry(std::move(entry), sigterm_timeout_ms);
  return true;
}

void ProcessSupervisor::reap_exited(const std::string& name, uint64_t generation) {
  std::unique_ptr<Entry> entry;

  {
    std::lock_guard lock(mtx_);
    auto it = entries_.find(name);

    if (it == entries_.end() || !it->second || !it->second->state || it->second->state->spec.generation != generation ||
        (it->second->proc && it->second->proc->is_running())) {
      return;
    }

    entry = std::move(it->second);
    entries_.erase(it);
  }

  destroy_entry(std::move(entry), 0U);
}

bool ProcessSupervisor::reload_process(const std::string& name) {
  int64_t pid = -1;

  {
    std::lock_guard lock(mtx_);
    auto it = entries_.find(name);

    if (it == entries_.end() || !it->second || !it->second->proc || !it->second->proc->is_running()) {
      return false;
    }

    pid = it->second->proc->get_process_id();
  }

  return ResourceController::send_reload_signal(pid);
}

void ProcessSupervisor::stop_all() {
  std::vector<std::unique_ptr<Entry>> to_stop;

  {
    std::unique_lock lock(mtx_);
    stopped_ = true;
    cancelled_starting_.clear();

    for (const auto& sname : starting_) {
      cancelled_starting_.insert(sname);
    }

    (void)starting_cv_.wait_for(lock, std::chrono::milliseconds(5000), [&] { return starting_.empty(); });
    to_stop.reserve(entries_.size());

    for (auto& item : entries_) {
      to_stop.push_back(std::move(item.second));
    }

    entries_.clear();
  }

  for (auto& entry : to_stop) {
    const uint32_t timeout_ms = (entry && entry->state) ? entry->state->spec.sigterm_timeout_ms : 5000U;
    destroy_entry(std::move(entry), timeout_ms);
  }
}

bool ProcessSupervisor::is_running(const std::string& name) const {
  std::lock_guard lock(mtx_);
  auto it = entries_.find(name);
  return it != entries_.end() && it->second && it->second->proc && it->second->proc->is_running();
}

ProcessMetrics ProcessSupervisor::sample_metrics(const std::string& name) const {
  int64_t pid = -1;
  std::string cg;

  {
    std::lock_guard lock(mtx_);
    auto it = entries_.find(name);

    if (it == entries_.end() || !it->second || !it->second->proc || !it->second->proc->is_running()) {
      return ProcessMetrics{};
    }

    pid = it->second->proc->get_process_id();
    cg = it->second->cgroup_path;
  }

  return resource_ctrl_.sample_metrics(pid, cg);
}

bool ProcessSupervisor::write_remap_file(const std::string& name,
                                         const std::vector<std::pair<std::string, std::string>>& remaps,
                                         std::string& path_out) const {
  path_out.clear();

  if (remaps.empty()) {
    return true;
  }

  nlohmann::ordered_json doc = nlohmann::ordered_json::object();

  for (const auto& [src, dst] : remaps) {
    doc[src] = dst;
  }

  std::string path = log_manager_.tmp_dir() + "/" + launcher::sanitize_identifier(name, false, "proc") + "_remap.json";
  std::ofstream ofs(path, std::ios::out | std::ios::trunc);

  if (!ofs.is_open()) {
    return false;
  }

  ofs << doc.dump(2) << '\n';
  ofs.flush();
  const bool write_ok = ofs.good();
  ofs.close();

  if (!write_ok || ofs.fail()) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return false;
  }

  path_out = std::move(path);
  return true;
}

void ProcessSupervisor::drain_stream(const std::shared_ptr<CallbackState>& state, bool is_stderr) {
  std::lock_guard lock(state->read_mtx);

  if (state->proc == nullptr) {
    return;
  }

  StdoutReadyCallback ready_cb;

  {
    std::lock_guard cb_lock(mtx_);
    ready_cb = on_stdout_ready_;
  }

  auto consume_line = [&](const std::string& raw) {
    log_manager_.emit_unit_output(state->spec.name, raw, is_stderr, state->spec.output, state->spec.is_container);

    if (!is_stderr && !state->spec.ready_stdout_pattern.empty() &&
        !state->ready_matched.load(std::memory_order_acquire) &&
        raw.find(state->spec.ready_stdout_pattern) != std::string::npos) {
      if (!state->ready_matched.exchange(true, std::memory_order_acq_rel) && ready_cb) {
        ready_cb(state->spec.name, state->spec.generation);
      }
    }
  };

  std::string chunk;
  const bool got = is_stderr ? state->proc->read_all_error(chunk) : state->proc->read_all_output(chunk);

  if VUNLIKELY (!got || chunk.empty()) {
    return;
  }

  static constexpr size_t kMaxLineBufferBytes = 65536U;
  std::string& buf = is_stderr ? state->stderr_buf : state->stdout_buf;
  size_t pos = 0U;

  while (pos < chunk.size()) {
    const size_t nl = chunk.find('\n', pos);

    if VUNLIKELY (nl == std::string::npos) {
      buf.append(chunk, pos, chunk.size() - pos);

      if VUNLIKELY (buf.size() >= kMaxLineBufferBytes) {
        consume_line(buf);
        buf.clear();
      }

      break;
    }

    size_t len = nl - pos;

    if (len > 0U && chunk[pos + len - 1U] == '\r') {
      --len;
    }

    if VLIKELY (buf.empty()) {
      consume_line(chunk.substr(pos, len));
    } else {
      buf.append(chunk, pos, len);

      if (!buf.empty() && buf.back() == '\r') {
        buf.pop_back();
      }

      consume_line(buf);
      buf.clear();
    }

    pos = nl + 1U;
  }
}

void ProcessSupervisor::destroy_entry(std::unique_ptr<Entry> entry, uint32_t sigterm_timeout_ms) {
  if (!entry) {
    return;
  }

  if (entry->state) {
    entry->state->expected_stop.store(true, std::memory_order_release);
  }

  if (entry->proc) {
    const StopSignal sig = entry->state ? entry->state->spec.stop_signal : StopSignal::kSigTerm;
    const KillMode kmode = entry->state ? entry->state->spec.kill_mode : KillMode::kProcess;

    if (entry->proc->is_running()) {
      const int64_t live_pid = entry->proc->get_process_id();
      const bool kill_group = (kmode == KillMode::kMixed || kmode == KillMode::kControlGroup);
      std::vector<DescendantPid> fallback_descendants;

      if (live_pid > 0) {
        if (kill_group && entry->cgroup_path.empty()) {
          fallback_descendants = ResourceController::snapshot_descendants(live_pid);
        }

        if (sig != StopSignal::kSigTerm || kmode == KillMode::kControlGroup) {
          const int64_t target_pid = (!entry->cgroup_path.empty() && kmode == KillMode::kControlGroup) ? -1 : live_pid;

          if (entry->proc->is_running() &&
              !ResourceController::send_stop_signal(target_pid, entry->cgroup_path, sig, kmode) &&
              entry->proc->is_running()) {
            entry->proc->terminate();
          }
        } else {
          entry->proc->terminate();
        }

        const uint32_t clamped_timeout = std::min<uint32_t>(sigterm_timeout_ms == 0U ? 1000U : sigterm_timeout_ms,
                                                            static_cast<uint32_t>(std::numeric_limits<int>::max()));
        const int wait_ms = static_cast<int>(clamped_timeout);

        if (!entry->proc->wait_for_finished(wait_ms)) {
          if (entry->proc->is_running()) {
            const int64_t kill_pid = !entry->cgroup_path.empty() ? -1 : entry->proc->get_process_id();
            (void)resource_ctrl_.force_kill(kill_pid, entry->cgroup_path, kmode);
          } else if (!entry->cgroup_path.empty() && kill_group) {
            (void)resource_ctrl_.force_kill(-1, entry->cgroup_path, kmode);
          }

          ResourceController::kill_pids(fallback_descendants);
          entry->proc->kill();
          (void)entry->proc->wait_for_finished(1000);
        } else if (kill_group) {
          if (!entry->cgroup_path.empty()) {
            (void)resource_ctrl_.force_kill(-1, entry->cgroup_path, kmode);
          }

          ResourceController::kill_pids(fallback_descendants);
        }
      } else if (kill_group && !entry->cgroup_path.empty()) {
        (void)resource_ctrl_.force_kill(-1, entry->cgroup_path, kmode);
      }
    } else if (!entry->cgroup_path.empty() && (kmode == KillMode::kMixed || kmode == KillMode::kControlGroup)) {
      (void)resource_ctrl_.force_kill(-1, entry->cgroup_path, kmode);
    }

    entry->proc->register_ready_read_stdout_callback(nullptr);
    entry->proc->register_ready_read_stderr_callback(nullptr);
    entry->proc->register_finished_callback(nullptr);
    entry->proc.reset();
  }

  if (entry->state) {
    std::lock_guard lock(entry->state->read_mtx);
    entry->state->proc = nullptr;
    log_manager_.close_unit_backend(entry->state->spec.name);
    resource_ctrl_.cleanup_unit(entry->state->spec.name, entry->cgroup_path);
  }

  if (!entry->remap_file.empty()) {
    std::error_code ec;
    std::filesystem::remove(entry->remap_file, ec);
  }
}

}  // namespace launcher

}  // namespace vlink
