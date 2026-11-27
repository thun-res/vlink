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

#include <vlink/base/functional.h>
#include <vlink/base/process.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "launcher_config.h"

namespace vlink::launcher {

struct ProcessLaunchRequest final {
  std::string name;
  uint64_t generation{0};
  std::string program;
  std::vector<std::string> args;
  std::string cwd;
  std::unordered_map<std::string, std::string> env;
  bool inherit_env{true};
  OutputMode output{OutputMode::kScreen};
  std::string ready_stdout_pattern;
  uint32_t stop_timeout_ms{5000};
};

class ProcessSupervisor final {
 public:
  using ExitCallback = MoveFunction<void(const std::string& name, uint64_t gen, int32_t exit_code,
                                         Process::ExitStatus status, bool expected_stop)>;
  using StdoutReadyCallback = MoveFunction<void(const std::string& name, uint64_t gen)>;

  ProcessSupervisor() = default;
  ~ProcessSupervisor();

  ProcessSupervisor(const ProcessSupervisor&) = delete;
  ProcessSupervisor& operator=(const ProcessSupervisor&) = delete;

  void set_callbacks(ExitCallback on_exit, StdoutReadyCallback on_stdout_ready);

  bool start_process(const ProcessLaunchRequest& req, int64_t* pid_out = nullptr, std::string* error_out = nullptr);

  bool stop_process(const std::string& name, uint32_t timeout_ms = 5000,
                    std::optional<uint64_t> expected_gen = std::nullopt);

  bool reload_process(const std::string& name);

  void reap_exited(const std::string& name, uint64_t generation);

  [[nodiscard]] bool is_running(const std::string& name) const;

  void stop_all();

 private:
  struct Callbacks final {
    ExitCallback on_exit;
    StdoutReadyCallback on_stdout_ready;
  };

  struct SharedCallbacks final {
    std::mutex mtx;
    std::shared_ptr<Callbacks> callbacks;
  };

  struct EntryState final {
    std::string name;
    uint64_t generation{0};
    std::atomic<int64_t> pid{-1};
    OutputMode output{OutputMode::kScreen};
    std::string ready_pattern;
    std::string pending_stdout;
    std::atomic_bool ready_fired{false};
    std::atomic_bool expected_stop{false};
  };

  struct Entry final {
    std::shared_ptr<EntryState> state;
    std::unique_ptr<Process> proc;
  };

  static void stop_entry(const std::shared_ptr<Entry>& entry, uint32_t timeout_ms);

  mutable std::mutex mtx_;
  std::shared_ptr<SharedCallbacks> shared_cbs_{std::make_shared<SharedCallbacks>()};
  std::unordered_map<std::string, std::shared_ptr<Entry>> entries_;
};

}  // namespace vlink::launcher
