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

#include <vlink/base/condition_variable.h>
#include <vlink/base/functional.h>
#include <vlink/base/macros.h>
#include <vlink/base/process.h>
//
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "./launcher_config.h"
#include "./log_manager.h"
#include "./resource_control.h"

namespace vlink {

namespace launcher {

struct ManagedProcessLaunch final {
  std::string name;
  uint64_t generation{0};
  std::string program;
  std::vector<std::string> args;
  std::string launch_prefix;
  std::string cwd;
  std::unordered_map<std::string, std::string> env;
  bool inherit_env{true};
  bool is_container{false};
  OutputMode output{OutputMode::kScreen};
  uint32_t sigterm_timeout_ms{5000};
  StopSignal stop_signal{StopSignal::kSigTerm};
  KillMode kill_mode{KillMode::kProcess};
  ResourceLimits resources;
  std::vector<std::pair<std::string, std::string>> remaps;
  std::string ready_stdout_pattern;
};

class ProcessSupervisor final {
 public:
  using ExitCallback = Function<void(const std::string& name, uint64_t generation, int32_t exit_code,
                                     Process::ExitStatus status, bool expected_stop)>;
  using StdoutReadyCallback = Function<void(const std::string& name, uint64_t generation)>;

  ProcessSupervisor(LogManager& log_manager, ResourceController& resource_ctrl);

  ~ProcessSupervisor();

  void set_callbacks(ExitCallback&& on_exit, StdoutReadyCallback&& on_stdout_ready);

  void reset_stopped();

  [[nodiscard]] bool start_process(const ManagedProcessLaunch& spec, int64_t* pid_out, std::string* cgroup_out,
                                   std::string* error_out);

  bool stop_process(const std::string& name, uint32_t sigterm_timeout_ms = 5000, uint64_t only_generation = 0U);

  void reap_exited(const std::string& name, uint64_t generation);

  bool reload_process(const std::string& name);

  void stop_all();

  [[nodiscard]] bool is_running(const std::string& name) const;

  [[nodiscard]] ProcessMetrics sample_metrics(const std::string& name) const;

 private:
  struct CallbackState final {
    ManagedProcessLaunch spec;
    Process* proc{nullptr};
    std::string cgroup_path;
    std::string stdout_buf;
    std::string stderr_buf;
    std::atomic_bool expected_stop{false};
    std::atomic_bool ready_matched{false};
    std::mutex read_mtx;
  };

  struct Entry final {
    std::shared_ptr<CallbackState> state;
    std::unique_ptr<Process> proc;
    std::string cgroup_path;
    std::string remap_file;
  };

  bool write_remap_file(const std::string& name, const std::vector<std::pair<std::string, std::string>>& remaps,
                        std::string& path_out) const;

  void drain_stream(const std::shared_ptr<CallbackState>& state, bool is_stderr);

  void destroy_entry(std::unique_ptr<Entry> entry, uint32_t sigterm_timeout_ms);

  LogManager& log_manager_;
  ResourceController& resource_ctrl_;
  mutable std::mutex mtx_;
  ConditionVariable starting_cv_;
  bool stopped_{false};
  std::unordered_map<std::string, uint64_t> starting_;
  std::unordered_map<std::string, uint64_t> cancelled_starting_;
  std::unordered_map<std::string, std::unique_ptr<Entry>> entries_;
  ExitCallback on_exit_;
  StdoutReadyCallback on_stdout_ready_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(ProcessSupervisor)
};

}  // namespace launcher

}  // namespace vlink
