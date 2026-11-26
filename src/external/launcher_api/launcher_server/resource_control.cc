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

#include "./resource_control.h"

#include <vlink/base/elapsed_timer.h>
#include <vlink/base/helpers.h>
#include <vlink/base/logger.h>
#include <vlink/base/utils.h>
//
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../launcher_common.h"

#if defined(__linux__)
#include <sched.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <mach/mach_time.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>
#elif defined(__QNX__) || defined(__QNXNTO__)
#include <devctl.h>
#include <fcntl.h>
#include <sys/procfs.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>
#elif !defined(_WIN32) && !defined(_WIN64)
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2
#endif
#include <windows.h>
//
#include <psapi.h>
#include <tlhelp32.h>
#endif

namespace vlink {

namespace launcher {

struct ResourceController::Impl final {
#if defined(_WIN32) || defined(_WIN64)
  std::unordered_map<std::string, HANDLE> job_handles;
#endif
};

#if defined(__linux__) || defined(_WIN32) || defined(_WIN64)
static bool mb_to_bytes_checked(uint64_t mb, uint64_t& bytes_out) noexcept {
  static constexpr uint64_t kBytesPerMiB = 1024ULL * 1024ULL;

  if (mb > std::numeric_limits<uint64_t>::max() / kBytesPerMiB) {
    return false;
  }

  bytes_out = mb * kBytesPerMiB;
  return true;
}
#endif

#if defined(__linux__)
static constexpr uint32_t kMaxSupportedCpuIndex = 65535U;

static bool write_sysfs_file(const std::string& path, std::string_view content) {
  std::ofstream ofs(path, std::ios::out | std::ios::trunc);

  if (!ofs.is_open()) {
    return false;
  }

  ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
  ofs.flush();
  return ofs.good();
}

static bool read_first_line(const std::string& path, std::string& out) {
  std::ifstream ifs(path);

  if (!ifs.is_open()) {
    return false;
  }

  return static_cast<bool>(std::getline(ifs, out));
}

static std::vector<pid_t> read_cgroup_pids(const std::string& cg_dir) {
  std::vector<pid_t> pids;
  std::ifstream ifs(cg_dir + "/cgroup.procs");
  int64_t pid = 0;

  while (ifs >> pid) {
    if (pid > 0) {
      pids.push_back(static_cast<pid_t>(pid));
    }
  }

  return pids;
}

static std::unordered_set<std::string> read_cgroup_controllers(const std::string& dir) {
  std::unordered_set<std::string> out;
  std::string controllers;

  if (!read_first_line(dir + "/cgroup.controllers", controllers) || controllers.empty()) {
    return out;
  }

  for (const auto& token : Helpers::split_view(controllers, ' ')) {
    if (!token.empty()) {
      out.emplace(token);
    }
  }

  return out;
}

static void enable_available_controllers(const std::string& dir) {
  const auto controllers = read_cgroup_controllers(dir);

  for (const char* name : {"cpu", "cpuset", "memory", "pids"}) {
    if (controllers.count(name) != 0U) {
      std::string cmd = "+";
      cmd.append(name);
      (void)write_sysfs_file(dir + "/cgroup.subtree_control", cmd);
    }
  }
}

static bool ensure_parent_subtree_controllers(const std::string& base_dir, bool allow_self_scope_migration) {
  std::string current_sub;
  (void)read_first_line(base_dir + "/cgroup.subtree_control", current_sub);

  if (!current_sub.empty()) {
    enable_available_controllers(base_dir);
    return true;
  }

  if (allow_self_scope_migration && !Helpers::has_endwith(base_dir, "/launcher.scope")) {
    const auto base_pids = read_cgroup_pids(base_dir);
    const pid_t self_pid = ::getpid();

    if (base_pids.size() == 1U && base_pids[0] == self_pid) {
      const std::string scope_dir = base_dir + "/launcher.scope";
      std::error_code ec;

      if (std::filesystem::create_directories(scope_dir, ec) || std::filesystem::is_directory(scope_dir, ec)) {
        if (write_sysfs_file(scope_dir + "/cgroup.procs", std::to_string(self_pid))) {
          enable_available_controllers(base_dir);
          return true;
        }

        std::filesystem::remove(scope_dir, ec);
      }
    }
  }

  enable_available_controllers(base_dir);
  return false;
}

static std::string detect_writable_cgroup_v2_root(const std::string& session_id) {
  struct Candidate final {
    std::string dir;
    bool from_self_cgroup{false};
  };

  std::vector<Candidate> candidates;
  const std::string env_root = Utils::get_env("VLINK_LAUNCHER_CGROUP_ROOT");

  if (!env_root.empty()) {
    candidates.push_back(Candidate{env_root, false});
  }

  std::ifstream cg_ifs("/proc/self/cgroup");
  std::string line;
  std::error_code exist_ec;

  while (std::getline(cg_ifs, line)) {
    if (Helpers::has_startwith(line, "0::")) {
      std::string rel = line.substr(3U);

      if (!rel.empty() && rel != "/") {
        std::filesystem::path p("/sys/fs/cgroup" + rel);

        if (std::filesystem::exists(p / "cgroup.controllers", exist_ec) && !exist_ec) {
          candidates.push_back(Candidate{p.string(), env_root.empty()});
        }
      }

      break;
    }
  }

  if (std::filesystem::exists("/sys/fs/cgroup/cgroup.controllers", exist_ec) && !exist_ec) {
    candidates.push_back(Candidate{"/sys/fs/cgroup", false});
  }

  const std::string session_tag = "vlink-launcher-" + launcher::sanitize_identifier(session_id);

  for (const auto& cand : candidates) {
    std::error_code ec;
    std::string test_dir;
    test_dir.reserve(cand.dir.size() + 1U + session_tag.size());
    test_dir.append(cand.dir).append("/").append(session_tag);

    if (std::filesystem::create_directories(test_dir, ec) || std::filesystem::is_directory(test_dir, ec)) {
      if (::access((test_dir + "/cgroup.procs").c_str(), W_OK) == 0) {
        auto child_ctrls = read_cgroup_controllers(test_dir);

        if (child_ctrls.empty()) {
          (void)ensure_parent_subtree_controllers(cand.dir, cand.from_self_cgroup);
        }

        enable_available_controllers(test_dir);
        return test_dir;
      }

      std::filesystem::remove(test_dir, ec);
    }
  }

  return "";
}

static bool apply_cgroup_v2_limits(const std::string& cg_dir, const ResourceLimits& limits, std::string& err_field) {
  bool all_ok = true;

  auto record_fail = [&](const char* field) -> bool {
    if (err_field.empty()) {
      err_field = field;
    }

    all_ok = false;
    return !limits.strict;
  };

  if (limits.cpu_quota_percent.has_value()) {
    const uint64_t period_us = 100000ULL;
    const uint64_t quota_us = static_cast<uint64_t>(*limits.cpu_quota_percent) * 1000ULL;
    const std::string val = std::to_string(quota_us) + " " + std::to_string(period_us);

    if (!write_sysfs_file(cg_dir + "/cpu.max", val) && !record_fail("cpu_quota_percent")) {
      return false;
    }
  }

  if (limits.cpu_weight.has_value()) {
    if (!write_sysfs_file(cg_dir + "/cpu.weight", std::to_string(*limits.cpu_weight)) && !record_fail("cpu_weight")) {
      return false;
    }
  }

  if (!limits.cpu_affinity.empty()) {
    std::string cpus;

    for (size_t i = 0; i < limits.cpu_affinity.size(); ++i) {
      if (i > 0U) {
        cpus.push_back(',');
      }

      cpus.append(std::to_string(limits.cpu_affinity[i]));
    }

    if (!write_sysfs_file(cg_dir + "/cpuset.cpus", cpus) && !record_fail("cpu_affinity")) {
      return false;
    }
  }

  if (limits.memory_max_mb.has_value()) {
    uint64_t bytes = 0U;

    if ((!mb_to_bytes_checked(*limits.memory_max_mb, bytes) ||
         !write_sysfs_file(cg_dir + "/memory.max", std::to_string(bytes))) &&
        !record_fail("memory_max_mb")) {
      return false;
    }
  }

  if (limits.memory_high_mb.has_value()) {
    uint64_t bytes = 0U;

    if ((!mb_to_bytes_checked(*limits.memory_high_mb, bytes) ||
         !write_sysfs_file(cg_dir + "/memory.high", std::to_string(bytes))) &&
        !record_fail("memory_high_mb")) {
      return false;
    }
  }

  if (limits.pids_max.has_value()) {
    if (!write_sysfs_file(cg_dir + "/pids.max", std::to_string(*limits.pids_max)) && !record_fail("pids_max")) {
      return false;
    }
  }

  return all_ok;
}

static bool cgroup_is_empty(const std::string& cg_dir) {
  std::ifstream ifs(cg_dir + "/cgroup.events");
  std::string line;

  while (std::getline(ifs, line)) {
    if (Helpers::has_startwith(line, "populated ")) {
      return line == "populated 0";
    }
  }

  return true;
}
#endif

#if !defined(_WIN32) && !defined(_WIN64)
static int to_posix_signal(StopSignal sig) noexcept {
  switch (sig) {
    case StopSignal::kSigInt:
      return SIGINT;
    case StopSignal::kSigHup:
      return SIGHUP;
    case StopSignal::kSigKill:
      return SIGKILL;
    case StopSignal::kSigTerm:
    default:
      return SIGTERM;
  }
}
#endif

ResourceController::ResourceController() : impl_(std::make_unique<Impl>()) {}

ResourceController::~ResourceController() { shutdown(); }

void ResourceController::init(const std::string& session_id, const std::vector<SliceConfig>& slices) {
  std::lock_guard lock(mtx_);
  session_id_ = session_id;
  slice_limits_.clear();
  created_slices_.clear();
  cpu_samples_.clear();

  for (const auto& sc : slices) {
    slice_limits_[sc.name] = sc.limits;
  }

#if defined(__linux__)
  cgroup_root_ = detect_writable_cgroup_v2_root(session_id_);
  cgroup_writable_ = !cgroup_root_.empty();

  if (cgroup_writable_) {
    for (const auto& sc : slices) {
      const std::string slice_dir = cgroup_root_ + "/" + sanitize_identifier(sc.name) + ".slice";
      std::error_code ec;

      if (std::filesystem::create_directories(slice_dir, ec) || std::filesystem::is_directory(slice_dir, ec)) {
        created_slices_.insert(slice_dir);
        enable_available_controllers(slice_dir);
        std::string err_field;

        if (!apply_cgroup_v2_limits(slice_dir, sc.limits, err_field)) {
          VLOG_W("ResourceController: Slice '", sc.name, "' could not enforce cgroup field '", err_field, "'.");
        }
      }
    }
  }
#else
  cgroup_root_.clear();
  cgroup_writable_ = false;
#endif
}

void ResourceController::shutdown() {
  std::lock_guard lock(mtx_);

#if defined(_WIN32) || defined(_WIN64)
  for (auto& [name, handle] : impl_->job_handles) {
    (void)name;

    if (handle != nullptr) {
      ::CloseHandle(handle);
    }
  }

  impl_->job_handles.clear();
#elif defined(__linux__)
  if (cgroup_writable_ && !cgroup_root_.empty()) {
    std::error_code ec;

    for (const auto& leaf : created_cgroups_) {
      if (!leaf.empty() && cgroup_is_empty(leaf)) {
        std::filesystem::remove(leaf, ec);
      }
    }

    for (const auto& slice_dir : created_slices_) {
      std::filesystem::remove(slice_dir, ec);
    }

    for (const auto& [slice_name, _] : slice_limits_) {
      const std::string slice_dir = cgroup_root_ + "/" + sanitize_identifier(slice_name) + ".slice";
      std::filesystem::remove(slice_dir, ec);
    }

    std::filesystem::remove(cgroup_root_, ec);
  }
#endif

  cgroup_root_.clear();
  cgroup_writable_ = false;
  slice_limits_.clear();
  created_slices_.clear();
  cpu_samples_.clear();
  created_cgroups_.clear();
}

ResourceApplyResult ResourceController::apply_to_process(std::string_view unit_name, uint64_t generation, int64_t pid,
                                                         const ResourceLimits& limits, KillMode kill_mode) {
  ResourceApplyResult res;
  const bool need_group_tracking = (kill_mode == KillMode::kControlGroup || kill_mode == KillMode::kMixed);

  if (limits.empty() && !need_group_tracking) {
    return res;
  }

  ResourceLimits effective = limits;
#if defined(__linux__)
  bool cgroup_writable = false;
  std::string cgroup_root;
#endif

  {
    std::lock_guard lock(mtx_);
#if defined(__linux__)
    cgroup_writable = cgroup_writable_;
    cgroup_root = cgroup_root_;
#endif

    if (!limits.slice.empty()) {
      auto it = slice_limits_.find(limits.slice);

      if (it != slice_limits_.end()) {
        if (!effective.cpu_quota_percent.has_value()) {
          effective.cpu_quota_percent = it->second.cpu_quota_percent;
        }

        if (!effective.cpu_weight.has_value()) {
          effective.cpu_weight = it->second.cpu_weight;
        }

        if (!effective.memory_max_mb.has_value()) {
          effective.memory_max_mb = it->second.memory_max_mb;
        }

        if (!effective.memory_high_mb.has_value()) {
          effective.memory_high_mb = it->second.memory_high_mb;
        }

        if (!effective.pids_max.has_value()) {
          effective.pids_max = it->second.pids_max;
        }

        if (effective.cpu_affinity.empty()) {
          effective.cpu_affinity = it->second.cpu_affinity;
        }

        if (!effective.nice.has_value()) {
          effective.nice = it->second.nice;
        }

        if (!effective.oom_score_adj.has_value()) {
          effective.oom_score_adj = it->second.oom_score_adj;
        }

        if (!effective.rlimit_nofile.has_value()) {
          effective.rlimit_nofile = it->second.rlimit_nofile;
        }

        if (!effective.rlimit_as_mb.has_value()) {
          effective.rlimit_as_mb = it->second.rlimit_as_mb;
        }

        if (!effective.rlimit_core_mb.has_value()) {
          effective.rlimit_core_mb = it->second.rlimit_core_mb;
        }

        if (!effective.strict && it->second.strict) {
          effective.strict = true;
        }
      }
    }
  }

  if (pid <= 0) {
    if (effective.strict && !effective.empty()) {
      res.ok = false;
      res.error = "Cannot enforce strict resource limits: target process exited before limits were applied.";
    }

    return res;
  }

  auto append_warn = [&](const std::string& msg) {
    if (!res.warning.empty()) {
      res.warning.append(" ");
    }

    res.warning.append(msg);
  };

#if defined(__linux__)
  const bool want_cgroup = effective.has_cgroup_limits() || need_group_tracking;

  if (want_cgroup) {
    if (cgroup_writable) {
      std::string parent_dir = cgroup_root;

      if (!effective.slice.empty()) {
        parent_dir += "/" + sanitize_identifier(effective.slice) + ".slice";
        std::error_code ec;

        if (std::filesystem::create_directories(parent_dir, ec) || std::filesystem::is_directory(parent_dir, ec)) {
          enable_available_controllers(parent_dir);
          std::lock_guard lock(mtx_);
          created_slices_.insert(parent_dir);
        }
      }

      const std::string leaf_dir =
          parent_dir + "/" + sanitize_identifier(unit_name) + "_g" + std::to_string(generation);
      std::error_code ec;

      if (std::filesystem::create_directories(leaf_dir, ec) || std::filesystem::is_directory(leaf_dir, ec)) {
        std::string err_field;

        if (effective.has_cgroup_limits() && !apply_cgroup_v2_limits(leaf_dir, effective, err_field)) {
          if (effective.strict) {
            res.ok = false;
            res.error = "Strict cgroup limit failed for field '" + err_field + "'.";
            std::filesystem::remove(leaf_dir, ec);
            return res;
          }

          append_warn("Failed to enforce cgroup v2 field '" + err_field + "'.");
        }

        if (write_sysfs_file(leaf_dir + "/cgroup.procs", std::to_string(pid))) {
          res.cgroup_path = leaf_dir;
          std::lock_guard lock(mtx_);
          created_cgroups_.push_back(leaf_dir);
        } else if (effective.has_cgroup_limits() && effective.strict) {
          res.ok = false;
          res.error = "Strict cgroup attach failed for PID " + std::to_string(pid) + ".";
          std::filesystem::remove(leaf_dir, ec);
          return res;
        } else {
          append_warn("Could not move PID into cgroup leaf '" + leaf_dir + "'.");
          std::filesystem::remove(leaf_dir, ec);
        }
      } else if (effective.has_cgroup_limits() && effective.strict) {
        res.ok = false;
        res.error = "Strict cgroup leaf creation failed for '" + leaf_dir + "'.";
        return res;
      } else {
        append_warn("Could not create cgroup leaf '" + leaf_dir + "'.");
      }
    } else if (effective.has_cgroup_limits() && effective.strict) {
      res.ok = false;
      res.error = "Strict cgroup limits requested, but no writable cgroup v2 hierarchy is delegated.";
      return res;
    } else if (effective.has_cgroup_limits()) {
      append_warn("cgroup v2 unavailable without delegation; applying portable POSIX limits only.");
    }
  }

  const auto target_pid = static_cast<pid_t>(pid);

  auto fail_strict = [&](std::string msg) -> ResourceApplyResult {
    res.ok = false;
    res.error = std::move(msg);
    return res;
  };

  if (effective.nice.has_value() && ::setpriority(PRIO_PROCESS, static_cast<id_t>(target_pid), *effective.nice) != 0) {
    const std::string msg = "Failed to apply nice=" + std::to_string(*effective.nice) + ".";

    if (effective.strict) {
      return fail_strict(msg);
    }

    append_warn(msg);
  }

  if (!effective.cpu_affinity.empty()) {
    uint32_t max_cpu = 0U;
    std::vector<uint32_t> valid_cpus;
    valid_cpus.reserve(effective.cpu_affinity.size());
    bool had_out_of_range = false;

    for (const uint32_t cpu : effective.cpu_affinity) {
      if (cpu > kMaxSupportedCpuIndex) {
        had_out_of_range = true;
        continue;
      }

      valid_cpus.push_back(cpu);
      max_cpu = std::max(max_cpu, cpu);
    }

    if (had_out_of_range) {
      const std::string msg = "CPU index exceeds supported maximum (" + std::to_string(kMaxSupportedCpuIndex) + ").";

      if (effective.strict) {
        return fail_strict(msg);
      }

      append_warn(msg);
    }

    if (!valid_cpus.empty()) {
      const size_t num_cpus = std::max<size_t>(static_cast<size_t>(CPU_SETSIZE), static_cast<size_t>(max_cpu) + 1U);
      cpu_set_t* dyn_set = CPU_ALLOC(num_cpus);

      if (dyn_set == nullptr) {
        if (effective.strict) {
          return fail_strict("Failed to allocate cpu_set_t for cpu_affinity.");
        }

        append_warn("Failed to allocate cpu_set_t for cpu_affinity.");
      } else {
        const size_t set_bytes = CPU_ALLOC_SIZE(num_cpus);
        CPU_ZERO_S(set_bytes, dyn_set);

        for (const uint32_t cpu : valid_cpus) {
          CPU_SET_S(cpu, set_bytes, dyn_set);
        }

        const bool aff_ok = (::sched_setaffinity(target_pid, set_bytes, dyn_set) == 0);
        CPU_FREE(dyn_set);

        if (!aff_ok) {
          if (effective.strict) {
            return fail_strict("Failed to apply cpu_affinity.");
          }

          append_warn("Failed to apply cpu_affinity.");
        }
      }
    }
  }

  if (effective.oom_score_adj.has_value()) {
    const std::string oom_path = "/proc/" + std::to_string(pid) + "/oom_score_adj";

    if (!write_sysfs_file(oom_path, std::to_string(*effective.oom_score_adj))) {
      if (effective.strict) {
        return fail_strict("Failed to write oom_score_adj.");
      }

      append_warn("Failed to write oom_score_adj.");
    }
  }

#if defined(__ANDROID__) && __ANDROID_API__ >= 21
  auto apply_rlimit_field = [&](auto resource, uint64_t val, bool is_mb, const char* field) -> bool {
    uint64_t raw = val;

    if (is_mb && !mb_to_bytes_checked(val, raw)) {
      if (effective.strict) {
        res = fail_strict(std::string(field) + " exceeds maximum byte range.");
        return false;
      }

      append_warn(std::string(field) + " exceeds maximum byte range.");
      return true;
    }

    struct rlimit64 rl{};
    rl.rlim_cur = static_cast<rlim64_t>(raw);
    rl.rlim_max = static_cast<rlim64_t>(raw);

    if (::prlimit64(target_pid, resource, &rl, nullptr) != 0) {
      if (effective.strict) {
        res = fail_strict(std::string("Failed to apply ") + field + ".");
        return false;
      }

      append_warn(std::string("Failed to apply ") + field + ".");
    }

    return true;
  };

  if (effective.rlimit_nofile.has_value() &&
      !apply_rlimit_field(RLIMIT_NOFILE, *effective.rlimit_nofile, false, "rlimit_nofile")) {
    return res;
  }

  if (effective.rlimit_as_mb.has_value() &&
      !apply_rlimit_field(RLIMIT_AS, *effective.rlimit_as_mb, true, "rlimit_as_mb")) {
    return res;
  }

  if (effective.rlimit_core_mb.has_value() &&
      !apply_rlimit_field(RLIMIT_CORE, *effective.rlimit_core_mb, true, "rlimit_core_mb")) {
    return res;
  }
#elif defined(__ANDROID__)
  if (effective.rlimit_nofile.has_value() || effective.rlimit_as_mb.has_value() ||
      effective.rlimit_core_mb.has_value()) {
    if (effective.strict) {
      return fail_strict("rlimit_* requires Android API >= 21 (prlimit64).");
    }

    append_warn("rlimit_* requires Android API >= 21 (prlimit64).");
  }
#else
  constexpr auto kMaxRlim = static_cast<uint64_t>(std::numeric_limits<rlim_t>::max());
  auto apply_rlimit_field = [&](auto resource, uint64_t val, bool is_mb, const char* field) -> bool {
    uint64_t raw = val;

    if ((is_mb && !mb_to_bytes_checked(val, raw)) || raw > kMaxRlim) {
      if (effective.strict) {
        res = fail_strict(std::string(field) + " exceeds maximum rlim_t range.");
        return false;
      }

      append_warn(std::string(field) + " exceeds maximum rlim_t range.");
      return true;
    }

    struct rlimit rl{};
    rl.rlim_cur = static_cast<rlim_t>(raw);
    rl.rlim_max = static_cast<rlim_t>(raw);

    if (::prlimit(target_pid, resource, &rl, nullptr) != 0) {
      if (effective.strict) {
        res = fail_strict(std::string("Failed to apply ") + field + ".");
        return false;
      }

      append_warn(std::string("Failed to apply ") + field + ".");
    }

    return true;
  };

  if (effective.rlimit_nofile.has_value() &&
      !apply_rlimit_field(RLIMIT_NOFILE, *effective.rlimit_nofile, false, "rlimit_nofile")) {
    return res;
  }

  if (effective.rlimit_as_mb.has_value() &&
      !apply_rlimit_field(RLIMIT_AS, *effective.rlimit_as_mb, true, "rlimit_as_mb")) {
    return res;
  }

  if (effective.rlimit_core_mb.has_value() &&
      !apply_rlimit_field(RLIMIT_CORE, *effective.rlimit_core_mb, true, "rlimit_core_mb")) {
    return res;
  }
#endif
#elif defined(_WIN32) || defined(_WIN64)
  const bool has_win_unsupported = effective.cpu_weight.has_value() || effective.memory_high_mb.has_value() ||
                                   effective.oom_score_adj.has_value() || effective.rlimit_nofile.has_value() ||
                                   effective.rlimit_as_mb.has_value() || effective.rlimit_core_mb.has_value();

  if (has_win_unsupported) {
    if (effective.strict) {
      res.ok = false;
      res.error = "Strict resource limits include Linux-only fields unsupported on Windows.";
      return res;
    }

    res.warning = "Windows does not support cgroup cpu_weight/memory_high/oom_adj/rlimit; applying JobObject limits.";
  }

  HANDLE proc =
      ::OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE | PROCESS_SET_INFORMATION, FALSE, static_cast<DWORD>(pid));

  if (proc == nullptr) {
    if (effective.strict) {
      res.ok = false;
      res.error = "Failed to open Windows process handle for resource limits.";
    }

    return res;
  }

  const bool need_job = need_group_tracking || effective.memory_max_mb.has_value() || effective.pids_max.has_value() ||
                        effective.cpu_quota_percent.has_value();

  if (need_job) {
    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);

    if (job != nullptr) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};

      if (need_group_tracking) {
        jeli.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      }

      if (effective.memory_max_mb.has_value()) {
        uint64_t bytes = 0U;

        if (!mb_to_bytes_checked(*effective.memory_max_mb, bytes) ||
            bytes > static_cast<uint64_t>(std::numeric_limits<SIZE_T>::max())) {
          if (effective.strict) {
            ::CloseHandle(job);
            ::CloseHandle(proc);
            res.ok = false;
            res.error = "memory_max_mb exceeds addressable byte range.";
            return res;
          }

          append_warn("memory_max_mb exceeds addressable byte range.");
        } else {
          jeli.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
          jeli.JobMemoryLimit = static_cast<SIZE_T>(bytes);
        }
      }

      if (effective.pids_max.has_value()) {
        jeli.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        jeli.BasicLimitInformation.ActiveProcessLimit = static_cast<DWORD>(*effective.pids_max);
      }

      bool limit_ok = (jeli.BasicLimitInformation.LimitFlags == 0U) ||
                      (::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli)) != FALSE);

      if (!limit_ok) {
        if (effective.strict) {
          ::CloseHandle(job);
          ::CloseHandle(proc);
          res.ok = false;
          res.error = "Failed to configure Windows JobObject limits.";
          return res;
        }

        append_warn("Failed to configure Windows JobObject limits.");
      }

      if (effective.cpu_quota_percent.has_value()) {
        SYSTEM_INFO sys_info{};
        ::GetSystemInfo(&sys_info);
        const uint64_t ncpus = std::max<uint64_t>(1U, static_cast<uint64_t>(sys_info.dwNumberOfProcessors));
        const uint64_t rate = std::clamp<uint64_t>(
            (static_cast<uint64_t>(*effective.cpu_quota_percent) * 100ULL) / ncpus, 1ULL, 10000ULL);
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu_ctl{};
        cpu_ctl.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
        cpu_ctl.CpuRate = static_cast<DWORD>(rate);

        if (::SetInformationJobObject(job, JobObjectCpuRateControlInformation, &cpu_ctl, sizeof(cpu_ctl)) == FALSE) {
          if (effective.strict) {
            ::CloseHandle(job);
            ::CloseHandle(proc);
            res.ok = false;
            res.error = "Failed to configure Windows JobObject CPU rate control.";
            return res;
          }

          append_warn("Failed to configure Windows JobObject CPU rate control.");
        }
      }

      if (::AssignProcessToJobObject(job, proc) != FALSE) {
        const std::string job_id = "win32-job://" + sanitize_identifier(unit_name) + "_g" + std::to_string(generation);

        {
          std::lock_guard lock(mtx_);
          auto old = impl_->job_handles.find(job_id);

          if (old != impl_->job_handles.end() && old->second != nullptr) {
            ::CloseHandle(old->second);
          }

          impl_->job_handles[job_id] = job;
        }

        res.cgroup_path = job_id;
      } else {
        ::CloseHandle(job);

        if (effective.strict) {
          ::CloseHandle(proc);
          res.ok = false;
          res.error = "Failed to assign process to Windows JobObject.";
          return res;
        }

        append_warn("Failed to assign process to Windows JobObject.");
      }
    } else if (effective.strict) {
      ::CloseHandle(proc);
      res.ok = false;
      res.error = "Failed to create Windows JobObject.";
      return res;
    } else {
      append_warn("Failed to create Windows JobObject.");
    }
  }

  if (!effective.cpu_affinity.empty()) {
    DWORD_PTR mask = 0;
    bool out_of_range = false;

    for (const uint32_t cpu : effective.cpu_affinity) {
      if (cpu >= sizeof(DWORD_PTR) * 8U) {
        out_of_range = true;
        break;
      }

      mask |= (DWORD_PTR{1} << cpu);
    }

    const bool aff_ok = !out_of_range && mask != 0 && (::SetProcessAffinityMask(proc, mask) != FALSE);

    if (!aff_ok) {
      if (effective.strict) {
        ::CloseHandle(proc);
        res.ok = false;
        res.error = "Failed to apply Windows cpu_affinity.";
        return res;
      }

      append_warn("Failed to apply Windows cpu_affinity.");
    }
  }

  if (effective.nice.has_value()) {
    DWORD cls = NORMAL_PRIORITY_CLASS;

    if (*effective.nice <= -10) {
      cls = HIGH_PRIORITY_CLASS;
    } else if (*effective.nice < 0) {
      cls = ABOVE_NORMAL_PRIORITY_CLASS;
    } else if (*effective.nice >= 10) {
      cls = IDLE_PRIORITY_CLASS;
    } else if (*effective.nice > 0) {
      cls = BELOW_NORMAL_PRIORITY_CLASS;
    }

    if (::SetPriorityClass(proc, cls) == FALSE) {
      if (effective.strict) {
        ::CloseHandle(proc);
        res.ok = false;
        res.error = "Failed to apply Windows process priority class.";
        return res;
      }

      append_warn("Failed to apply Windows process priority class.");
    }
  }

  ::CloseHandle(proc);
#else
  (void)unit_name;
  (void)generation;

  if (effective.nice.has_value() && ::setpriority(PRIO_PROCESS, static_cast<id_t>(pid), *effective.nice) != 0) {
    const std::string msg = "Failed to apply nice=" + std::to_string(*effective.nice) + ".";

    if (effective.strict) {
      res.ok = false;
      res.error = msg;
      return res;
    }

    append_warn(msg);
  }

  const bool has_linux_only = effective.has_cgroup_limits() || !effective.cpu_affinity.empty() ||
                              effective.oom_score_adj.has_value() || effective.rlimit_nofile.has_value() ||
                              effective.rlimit_as_mb.has_value() || effective.rlimit_core_mb.has_value();

  if (has_linux_only) {
    if (effective.strict) {
      res.ok = false;
      res.error = "Strict cgroup/rlimit/affinity limits are not supported on this platform.";
      return res;
    }

    append_warn("Platform does not support Linux cgroup v2 / external rlimit; applying portable nice limit only.");
  }
#endif

  return res;
}

#if !defined(_WIN32) && !defined(_WIN64)
static bool signal_posix_target(int64_t pid, const std::string& cgroup_path, int signum, bool kill_group,
                                bool try_cgroup_kill) {
#if defined(__linux__)
  if (kill_group && !cgroup_path.empty()) {
    if (try_cgroup_kill && write_sysfs_file(cgroup_path + "/cgroup.kill", "1")) {
      return true;
    }

    bool sent = false;

    for (const pid_t p : read_cgroup_pids(cgroup_path)) {
      if (::kill(p, signum) == 0) {
        sent = true;
      }
    }

    if (sent) {
      return true;
    }
  }
#else
  (void)cgroup_path;
  (void)try_cgroup_kill;
#endif

  if (pid > 0) {
    const auto target = static_cast<pid_t>(pid);

    if (kill_group) {
      const pid_t pgid = ::getpgid(target);

      if (pgid == target && pgid != ::getpgid(0) && ::kill(-pgid, signum) == 0) {
        return true;
      }
    }

    return ::kill(target, signum) == 0;
  }

  return false;
}
#endif

bool ResourceController::send_stop_signal(int64_t pid, const std::string& cgroup_path, StopSignal sig, KillMode mode) {
#if !defined(_WIN32) && !defined(_WIN64)
  return signal_posix_target(pid, cgroup_path, to_posix_signal(sig), mode == KillMode::kControlGroup, false);
#else
  (void)pid;
  (void)cgroup_path;
  (void)sig;
  (void)mode;
  return false;
#endif
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
bool ResourceController::force_kill(int64_t pid, const std::string& cgroup_path, KillMode mode) {
#if defined(_WIN32) || defined(_WIN64)
  (void)pid;

  if ((mode == KillMode::kControlGroup || mode == KillMode::kMixed) && !cgroup_path.empty()) {
    std::lock_guard lock(mtx_);
    auto it = impl_->job_handles.find(cgroup_path);

    if (it != impl_->job_handles.end() && it->second != nullptr) {
      return ::TerminateJobObject(it->second, 9) != FALSE;
    }
  }

  return false;
#else
  return signal_posix_target(pid, cgroup_path, SIGKILL, mode == KillMode::kControlGroup || mode == KillMode::kMixed,
                             true);
#endif
}

bool ResourceController::reap_process_group(int64_t pgid, const std::string& cgroup_path, KillMode mode) {
  if (mode != KillMode::kControlGroup && mode != KillMode::kMixed) {
    return false;
  }

  if (!cgroup_path.empty() && force_kill(-1, cgroup_path, mode)) {
    return true;
  }

#if !defined(_WIN32) && !defined(_WIN64)
  if (pgid > 0) {
    const auto target_pgid = static_cast<pid_t>(pgid);

    if (target_pgid != ::getpgid(0)) {
      return ::kill(-target_pgid, SIGKILL) == 0;
    }
  }

  return false;
#else
  (void)pgid;
  return false;
#endif
}

bool ResourceController::send_reload_signal(int64_t pid) {
#if !defined(_WIN32) && !defined(_WIN64)
  if (pid <= 0) {
    return false;
  }

  return ::kill(static_cast<pid_t>(pid), SIGHUP) == 0;
#else
  (void)pid;
  return false;
#endif
}

void ResourceController::cleanup_unit(std::string_view unit_name, const std::string& cgroup_path) {
#if defined(__linux__)
  (void)unit_name;

  if (!cgroup_path.empty() && cgroup_is_empty(cgroup_path)) {
    std::error_code ec;

    if (std::filesystem::remove(cgroup_path, ec) || !std::filesystem::exists(cgroup_path, ec)) {
      std::lock_guard lock(mtx_);
      created_cgroups_.erase(std::remove(created_cgroups_.begin(), created_cgroups_.end(), cgroup_path),
                             created_cgroups_.end());
    }
  }
#elif defined(_WIN32) || defined(_WIN64)
  (void)unit_name;
  std::lock_guard lock(mtx_);

  if (!cgroup_path.empty()) {
    auto it = impl_->job_handles.find(cgroup_path);

    if (it != impl_->job_handles.end()) {
      if (it->second != nullptr) {
        ::CloseHandle(it->second);
      }

      impl_->job_handles.erase(it);
    }
  }
#else
  (void)unit_name;
  (void)cgroup_path;
#endif
}

ProcessMetrics ResourceController::sample_metrics(int64_t pid, const std::string& cgroup_path) {
  ProcessMetrics m;

  if (pid <= 0) {
    return m;
  }

#if defined(__linux__)
  auto parse_u64 = [](const char* text, uint64_t& out) -> bool {
    if (text == nullptr) {
      return false;
    }

    char* end = nullptr;
    errno = 0;
    const auto val = static_cast<uint64_t>(std::strtoull(text, &end, 10));

    if (end == text || errno == ERANGE) {
      return false;
    }

    out = val;
    return true;
  };

  {
    std::ifstream status_ifs("/proc/" + std::to_string(pid) + "/status");
    std::string line;

    while (std::getline(status_ifs, line)) {
      if (Helpers::has_startwith(line, "VmRSS:")) {
        uint64_t kb = 0;

        if (parse_u64(line.c_str() + 6U, kb)) {
          m.memory_rss_kb = kb;
        }
      } else if (Helpers::has_startwith(line, "Threads:")) {
        uint64_t th = 0;

        if (parse_u64(line.c_str() + 8U, th)) {
          m.thread_count = static_cast<uint32_t>(th);
        }
      }
    }
  }

  if (!cgroup_path.empty()) {
    std::string mem_str;

    if (read_first_line(cgroup_path + "/memory.current", mem_str)) {
      uint64_t bytes = 0;

      if (parse_u64(mem_str.c_str(), bytes)) {
        m.cgroup_memory_kb = bytes / 1024ULL;
      }
    }

    std::string pids_str;

    if (read_first_line(cgroup_path + "/pids.current", pids_str)) {
      uint64_t pcur = 0;

      if (parse_u64(pids_str.c_str(), pcur) && pcur > m.thread_count) {
        m.thread_count = static_cast<uint32_t>(pcur);
      }
    }
  }

  uint64_t proc_time_us = 0;
  uint64_t start_time = 0;
  bool got_cpu = false;

  if (!cgroup_path.empty()) {
    std::ifstream cpu_ifs(cgroup_path + "/cpu.stat");
    std::string key;
    uint64_t val = 0;

    while (cpu_ifs >> key >> val) {
      if (key == "usage_usec") {
        proc_time_us = val;
        got_cpu = true;
        break;
      }
    }
  }

  {
    std::string stat_line;

    if (read_first_line("/proc/" + std::to_string(pid) + "/stat", stat_line)) {
      const size_t rparen = stat_line.rfind(')');

      if (rparen != std::string::npos && (rparen + 2U) < stat_line.size()) {
        const auto fields = Helpers::split_view(std::string_view(stat_line).substr(rparen + 2U), ' ');
        uint64_t utime = 0;
        uint64_t stime = 0;

        auto parse_u64_view = [](std::string_view sv) noexcept -> uint64_t {
          uint64_t v = 0U;
          const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
          return (ec == std::errc() && ptr == sv.data() + sv.size()) ? v : 0U;
        };

        if VLIKELY (fields.size() >= 20U) {
          start_time = parse_u64_view(fields[19U]);

          if (!got_cpu) {
            utime = parse_u64_view(fields[11U]);
            stime = parse_u64_view(fields[12U]);
          }
        }

        if (!got_cpu) {
          const long clk_tck = ::sysconf(_SC_CLK_TCK);  // NOLINT(runtime/int,google-runtime-int)

          if VLIKELY (clk_tck > 0) {
            proc_time_us = ((utime + stime) * 1000000ULL) / static_cast<uint64_t>(clk_tck);
            got_cpu = true;
          }
        }
      }
    }
  }

  if (got_cpu) {
    m.cpu_usage = record_cpu_sample(pid, proc_time_us, start_time);
  } else {
    std::lock_guard lock(mtx_);
    cpu_samples_.erase(pid);
  }
#elif defined(_WIN32) || defined(_WIN64)
  (void)cgroup_path;
  HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));

  if (proc != nullptr) {
    PROCESS_MEMORY_COUNTERS pmc{};

    if (::GetProcessMemoryInfo(proc, &pmc, sizeof(pmc))) {
      m.memory_rss_kb = static_cast<uint64_t>(pmc.WorkingSetSize / 1024U);
    }

    FILETIME creation_ft{};
    FILETIME exit_ft{};
    FILETIME kernel_ft{};
    FILETIME user_ft{};

    if (::GetProcessTimes(proc, &creation_ft, &exit_ft, &kernel_ft, &user_ft) != FALSE) {
      auto ft_to_u64 = [](const FILETIME& ft) noexcept -> uint64_t {
        return (static_cast<uint64_t>(ft.dwHighDateTime) << 32U) | static_cast<uint64_t>(ft.dwLowDateTime);
      };
      const uint64_t start_time = ft_to_u64(creation_ft);
      const uint64_t proc_time_us = (ft_to_u64(kernel_ft) + ft_to_u64(user_ft)) / 10ULL;
      m.cpu_usage = record_cpu_sample(pid, proc_time_us, start_time);
    }

    ::CloseHandle(proc);

    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    if (snap != INVALID_HANDLE_VALUE) {
      PROCESSENTRY32W pe{};
      pe.dwSize = sizeof(pe);

      if (::Process32FirstW(snap, &pe) != FALSE) {
        do {
          if (pe.th32ProcessID == static_cast<DWORD>(pid)) {
            m.thread_count = static_cast<uint32_t>(pe.cntThreads);
            break;
          }
        } while (::Process32NextW(snap, &pe) != FALSE);
      }

      ::CloseHandle(snap);
    }
  } else {
    std::lock_guard lock(mtx_);
    cpu_samples_.erase(pid);
  }
#elif defined(__APPLE__)
  (void)cgroup_path;
  struct proc_taskinfo pti{};

  if (::proc_pidinfo(static_cast<int>(pid), PROC_PIDTASKINFO, 0, &pti, static_cast<int>(sizeof(pti))) ==
      static_cast<int>(sizeof(pti))) {
    m.memory_rss_kb = static_cast<uint64_t>(pti.pti_resident_size / 1024U);
    m.thread_count = static_cast<uint32_t>(std::max<int32_t>(0, pti.pti_threadnum));

    mach_timebase_info_data_t tb{};
    uint64_t proc_time_us = 0U;

    if (::mach_timebase_info(&tb) == KERN_SUCCESS && tb.denom != 0U) {
      const uint64_t total_ticks = pti.pti_total_user + pti.pti_total_system;
      proc_time_us = (total_ticks / 1000ULL) * tb.numer / tb.denom +
                     ((total_ticks % 1000ULL) * tb.numer) / (static_cast<uint64_t>(tb.denom) * 1000ULL);
    }

    uint64_t start_time = 0U;
    struct kinfo_proc kp{};
    size_t kp_len = sizeof(kp);
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid)};

    if (::sysctl(mib, 4, &kp, &kp_len, nullptr, 0) == 0 && kp_len > 0U) {
      start_time = static_cast<uint64_t>(kp.kp_proc.p_starttime.tv_sec) * 1000000ULL +
                   static_cast<uint64_t>(kp.kp_proc.p_starttime.tv_usec);
    }

    m.cpu_usage = record_cpu_sample(pid, proc_time_us, start_time);
  } else {
    std::lock_guard lock(mtx_);
    cpu_samples_.erase(pid);
  }
#elif defined(__QNX__) || defined(__QNXNTO__)
  (void)cgroup_path;
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/%d/ctl", static_cast<int>(pid));
  const int fd = ::open(path, O_RDONLY);

  if (fd >= 0) {
    procfs_info info{};
    const int rc = ::devctl(fd, DCMD_PROC_INFO, &info, sizeof(info), nullptr);
    ::close(fd);

    if (rc == EOK) {
      m.thread_count = static_cast<uint32_t>(std::max<int32_t>(0, info.num_threads));
      const uint64_t proc_time_us = (static_cast<uint64_t>(info.utime) + static_cast<uint64_t>(info.stime)) / 1000ULL;
      m.cpu_usage = record_cpu_sample(pid, proc_time_us, static_cast<uint64_t>(info.start_time));
    }
  } else {
    std::lock_guard lock(mtx_);
    cpu_samples_.erase(pid);
  }
#else
  (void)cgroup_path;
#endif

  return m;
}

double ResourceController::record_cpu_sample(int64_t pid, uint64_t proc_time_us, uint64_t start_time) {
  const uint64_t now_us = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMicro);
  double cpu_usage = -1.0;

  std::lock_guard lock(mtx_);
  auto it = cpu_samples_.find(pid);

  if (it != cpu_samples_.end() && it->second.start_time == start_time && now_us > it->second.wall_time_us &&
      proc_time_us >= it->second.proc_time_us) {
    const uint64_t d_proc = proc_time_us - it->second.proc_time_us;
    const uint64_t d_wall = now_us - it->second.wall_time_us;
    cpu_usage = (static_cast<double>(d_proc) * 100.0) / static_cast<double>(d_wall);
  }

  if (cpu_samples_.size() > 1024U && (now_us < last_prune_us_ || (now_us - last_prune_us_) >= 30000000ULL)) {
    last_prune_us_ = now_us;

    for (auto sit = cpu_samples_.begin(); sit != cpu_samples_.end();) {
      if (now_us > sit->second.wall_time_us && (now_us - sit->second.wall_time_us) > 60000000ULL) {
        sit = cpu_samples_.erase(sit);
      } else {
        ++sit;
      }
    }
  }

  cpu_samples_[pid] = CpuSample{proc_time_us, now_us, start_time};
  return cpu_usage;
}

}  // namespace launcher

}  // namespace vlink
