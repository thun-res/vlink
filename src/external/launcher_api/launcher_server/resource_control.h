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

#include <vlink/base/macros.h>
//
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "./launcher_config.h"

namespace vlink {

namespace launcher {

struct ProcessMetrics final {
  uint64_t memory_rss_kb{0};
  uint64_t cgroup_memory_kb{0};
  uint32_t thread_count{0};
  double cpu_usage{-1.0};
};

struct DescendantPid final {
  int64_t pid{0};
  uint64_t start_time{0};
};

struct ResourceApplyResult final {
  bool ok{true};
  std::string cgroup_path;
  std::string warning;
  std::string error;
};

class ResourceController final {
 public:
  ResourceController();

  ~ResourceController();

  void init(const std::string& session_id, const std::vector<SliceConfig>& slices);

  void shutdown();

  [[nodiscard]] ResourceApplyResult apply_to_process(std::string_view unit_name, uint64_t generation, int64_t pid,
                                                     const ResourceLimits& limits,
                                                     KillMode kill_mode = KillMode::kProcess);

  static bool send_stop_signal(int64_t pid, const std::string& cgroup_path, StopSignal sig, KillMode mode);

  bool force_kill(int64_t pid, const std::string& cgroup_path, KillMode mode);

  [[nodiscard]] static std::vector<DescendantPid> snapshot_descendants(int64_t pid);

  static void kill_pids(const std::vector<DescendantPid>& pids);

  static bool send_reload_signal(int64_t pid);

  void cleanup_unit(std::string_view unit_name, const std::string& cgroup_path);

  [[nodiscard]] ProcessMetrics sample_metrics(int64_t pid, const std::string& cgroup_path);

 private:
  struct CpuSample final {
    uint64_t proc_time_us{0};
    uint64_t wall_time_us{0};
    uint64_t start_time{0};
  };

  double record_cpu_sample(int64_t pid, uint64_t proc_time_us, uint64_t start_time);

  struct Impl;
  std::unique_ptr<Impl> impl_;

  mutable std::mutex mtx_;
  std::string session_id_;
  std::string cgroup_root_;
  bool cgroup_writable_{false};
  std::unordered_map<std::string, ResourceLimits> slice_limits_;
  std::unordered_map<int64_t, CpuSample> cpu_samples_;
  std::vector<std::string> created_cgroups_;
  uint64_t last_prune_us_{0};

  VLINK_DISALLOW_COPY_AND_ASSIGN(ResourceController)
};

}  // namespace launcher

}  // namespace vlink
