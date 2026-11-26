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

#include "./scheduler_manager.h"

#include <vlink/base/deadline_timer.h>
#include <vlink/base/logger.h>
#include <vlink/base/utils.h>
//
#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../launcher_common.h"

#if defined(__linux__) && !defined(__ANDROID__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#elif defined(__ANDROID__)
#include <sched.h>
#include <sys/resource.h>
#elif defined(__QNX__) || defined(__QNXNTO__)
#include <pthread.h>
#include <sched.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>
#elif !defined(_WIN32) && !defined(_WIN64)
#include <pthread.h>
#include <sched.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vlink {

namespace launcher {

static constexpr size_t kMaxThreadNameLen = 15U;
#if defined(__linux__)
static constexpr uint32_t kMaxSupportedCpuIndex = 65535U;
#endif

static std::string clamp_thread_name(std::string_view raw) {
  if (raw.size() <= kMaxThreadNameLen) {
    return std::string(raw);
  }

  return std::string(raw.substr(0, kMaxThreadNameLen));
}

static bool apply_current_thread_affinity(const std::vector<uint32_t>& cpus, size_t worker_idx, bool pin_single_core) {
  if (cpus.empty()) {
    return true;
  }

#if defined(__linux__)
  uint32_t max_cpu = 0U;

  if (pin_single_core) {
    max_cpu = cpus[worker_idx % cpus.size()];
  } else {
    for (const uint32_t cpu : cpus) {
      max_cpu = std::max(max_cpu, cpu);
    }
  }

  if (max_cpu > kMaxSupportedCpuIndex) {
    return false;
  }

  const size_t num_cpus = std::max<size_t>(static_cast<size_t>(CPU_SETSIZE), static_cast<size_t>(max_cpu) + 1U);
  cpu_set_t* dyn_set = CPU_ALLOC(num_cpus);

  if (dyn_set == nullptr) {
    return false;
  }

  const size_t set_bytes = CPU_ALLOC_SIZE(num_cpus);
  CPU_ZERO_S(set_bytes, dyn_set);

  if (pin_single_core) {
    CPU_SET_S(cpus[worker_idx % cpus.size()], set_bytes, dyn_set);
  } else {
    for (const uint32_t cpu : cpus) {
      CPU_SET_S(cpu, set_bytes, dyn_set);
    }
  }

#if defined(__ANDROID__)
  const bool applied = (::sched_setaffinity(0, set_bytes, dyn_set) == 0);
#else
  const bool applied = (::pthread_setaffinity_np(::pthread_self(), set_bytes, dyn_set) == 0);
#endif
  CPU_FREE(dyn_set);
  return applied;
#elif defined(_WIN32) || defined(_WIN64)
  DWORD_PTR mask = 0;

  if (pin_single_core) {
    const uint32_t cpu = cpus[worker_idx % cpus.size()];

    if (cpu >= sizeof(DWORD_PTR) * 8U) {
      return false;
    }

    mask = (DWORD_PTR{1} << cpu);
  } else {
    for (const uint32_t cpu : cpus) {
      if (cpu >= sizeof(DWORD_PTR) * 8U) {
        return false;
      }

      mask |= (DWORD_PTR{1} << cpu);
    }
  }

  return mask != 0 && ::SetThreadAffinityMask(::GetCurrentThread(), mask) != 0;
#elif defined(__QNX__) || defined(__QNXNTO__)
  const size_t ncpus = static_cast<size_t>(std::max<int>(1, _syspage_ptr->num_cpu));
  const size_t rsize = RMSK_SIZE(ncpus);
  std::vector<uint32_t> buf(1U + rsize * 2U, 0U);
  buf[0] = static_cast<uint32_t>(rsize);
  uint32_t* runmask = &buf[1];
  uint32_t* inherit = &buf[1U + rsize];

  if (pin_single_core) {
    const uint32_t cpu = cpus[worker_idx % cpus.size()];

    if (cpu >= ncpus) {
      return false;
    }

    RMSK_SET(cpu, runmask);
    RMSK_SET(cpu, inherit);
  } else {
    for (const uint32_t cpu : cpus) {
      if (cpu >= ncpus) {
        return false;
      }

      RMSK_SET(cpu, runmask);
      RMSK_SET(cpu, inherit);
    }
  }

  return ::ThreadCtl(_NTO_TCTL_RUNMASK_GET_AND_SET_INHERIT, buf.data()) != -1;
#else
  uint32_t mask = 0U;

  if (pin_single_core) {
    const uint32_t cpu = cpus[worker_idx % cpus.size()];

    if (cpu >= 32U) {
      return false;
    }

    mask = (uint32_t{1U} << cpu);
  } else {
    for (const uint32_t cpu : cpus) {
      if (cpu >= 32U) {
        return false;
      }

      mask |= (uint32_t{1U} << cpu);
    }
  }

  return mask != 0U && Utils::set_thread_stick(mask);
#endif
}

static int to_native_sched_policy(SchedPolicy policy) noexcept {
#if !defined(_WIN32) && !defined(_WIN64)
  switch (policy) {
    case SchedPolicy::kOther:
      return SCHED_OTHER;
    case SchedPolicy::kFifo:
      return SCHED_FIFO;
    case SchedPolicy::kRr:
      return SCHED_RR;
    default:
      return -1;
  }
#else
  (void)policy;
  return -1;
#endif
}

static bool apply_current_thread_priority(SchedPolicy policy, int priority) {
  const int native_policy = to_native_sched_policy(policy);

  if (native_policy < 0 && priority == 0) {
    return true;
  }

#if defined(__linux__)
  if (native_policy < 0) {
    return ::setpriority(PRIO_PROCESS, 0, std::clamp(priority, -20, 19)) == 0;
  }

  if (native_policy == SCHED_OTHER && priority != 0) {
    const bool sched_ok = Utils::set_thread_priority(0, SCHED_OTHER);
    const bool nice_ok = (::setpriority(PRIO_PROCESS, 0, std::clamp(priority, -20, 19)) == 0);
    return sched_ok && nice_ok;
  }

  return Utils::set_thread_priority(priority, native_policy);
#elif !defined(_WIN32) && !defined(_WIN64)
  int eff_policy = native_policy;

  if (eff_policy < 0) {
    struct sched_param cur_param{};

    if (::pthread_getschedparam(::pthread_self(), &eff_policy, &cur_param) != 0) {
      return false;
    }
  }

  return Utils::set_thread_priority(priority, eff_policy);
#else
  return Utils::set_thread_priority(priority, native_policy);
#endif
}

static void apply_worker_tuning(const std::vector<uint32_t>& cpus, SchedPolicy policy, int priority, size_t worker_idx,
                                bool pin_single, const std::string& group_name) {
  if (!cpus.empty() && !apply_current_thread_affinity(cpus, worker_idx, pin_single)) {
    VLOG_W("SchedulerManager: Failed to set CPU affinity for group '", group_name, "'.");
  }

  if ((policy != SchedPolicy::kDefault || priority != 0) && !apply_current_thread_priority(policy, priority)) {
    VLOG_W("SchedulerManager: Failed to set thread priority for group '", group_name, "'.");
  }
}

static ThreadPool::Type to_thread_pool_type(MessageLoop::Type type) noexcept {
  return (type == MessageLoop::kLockfreeType) ? ThreadPool::kLockfreeType : ThreadPool::kNormalType;
}

static ThreadPool::Strategy to_thread_pool_strategy(MessageLoop::Strategy strategy) noexcept {
  switch (strategy) {
    case MessageLoop::kPopStrategy:
      return ThreadPool::kPopStrategy;
    case MessageLoop::kBlockStrategy:
      return ThreadPool::kBlockStrategy;
    case MessageLoop::kOptimizationStrategy:
    default:
      return ThreadPool::kOptimizationStrategy;
  }
}

template <typename LoopT>
class ConfiguredLoop final : public LoopT {
 public:
  template <typename... ArgsT>
  ConfiguredLoop(uint32_t max_elapsed_ms, std::string group_name, ArgsT&&... args)
      : LoopT(std::forward<ArgsT>(args)...), max_elapsed_ms_(max_elapsed_ms), group_name_(std::move(group_name)) {}

  [[nodiscard]] uint32_t get_max_elapsed_time() const override { return max_elapsed_ms_; }

 protected:
  void on_task_timeout(typename LoopT::Callback&& callback, uint32_t elapsed_time) override {
    const uint64_t count = timeout_count_.fetch_add(1U, std::memory_order_relaxed) + 1U;
    VLOG_W_EVERY_MS(5000, "SchedulerManager: Group '", group_name_,
                    "' queued task exceeded max_elapsed_ms (elapsed=", elapsed_time, "ms, budget=", max_elapsed_ms_,
                    "ms, total_overdue=", count, "); executing anyway.");

    if VLIKELY (callback) {
      callback();
    }
  }

 private:
  uint32_t max_elapsed_ms_{0};
  std::string group_name_;
  std::atomic<uint64_t> timeout_count_{0};
};

SchedulerManager::SchedulerManager() = default;

SchedulerManager::~SchedulerManager() { shutdown(); }

bool SchedulerManager::init(const std::vector<SchedulerConfig>& configs) {
  std::lock_guard lock(mtx_);

  if (running_) {
    return true;
  }

  SchedulerConfig default_cfg;
  default_cfg.name = "default";
  default_cfg.mode = SchedulerMode::kShared;
  default_cfg.queue_type = MessageLoop::kPriorityType;
  default_cfg.strategy = MessageLoop::kOptimizationStrategy;
  default_cfg.threads = 1;

  auto create_group = [&](const SchedulerConfig& cfg) {
    if (groups_.count(cfg.name) != 0U) {
      VLOG_W("SchedulerManager: Ignoring duplicate scheduler group '", cfg.name, "'.");
      return;
    }

    auto group = std::make_unique<GroupRuntime>();
    group->config = cfg;

    const bool has_thread_tuning =
        (!cfg.cpu_affinity.empty() || cfg.sched_policy != SchedPolicy::kDefault || cfg.thread_priority != 0);

    if (cfg.mode == SchedulerMode::kDedicated) {
      groups_[cfg.name] = std::move(group);
      return;
    }

    if (cfg.mode == SchedulerMode::kThreadPool) {
      const size_t workers = std::max<size_t>(1U, cfg.threads);

      if (has_thread_tuning) {
        auto cpus = cfg.cpu_affinity;
        const SchedPolicy policy = cfg.sched_policy;
        const int priority = cfg.thread_priority;
        const std::string gname = cfg.name;
        group->thread_pool = std::make_unique<ThreadPool>(
            workers, to_thread_pool_type(cfg.queue_type), [cpus, policy, priority, gname](size_t worker_idx) {
              apply_worker_tuning(cpus, policy, priority, worker_idx, false, gname);
            });
      } else {
        group->thread_pool = std::make_unique<ThreadPool>(workers, to_thread_pool_type(cfg.queue_type));
      }

      group->thread_pool->set_name(clamp_thread_name("vlp_" + cfg.name));
      group->thread_pool->set_strategy(to_thread_pool_strategy(cfg.strategy));

      auto anchor_loop = create_configured_loop(cfg, "vl_" + cfg.name + "_0", 0U);
      anchor_loop->async_run();
      group->loops.push_back(std::move(anchor_loop));
    } else if (cfg.mode == SchedulerMode::kMulti) {
      group->multi_loop = std::make_unique<ConfiguredLoop<MultiLoop>>(
          cfg.max_elapsed_ms, cfg.name, std::max<size_t>(1U, cfg.threads), cfg.queue_type);
      group->multi_loop->set_name(clamp_thread_name("vl_" + cfg.name));
      group->multi_loop->set_strategy(cfg.strategy);

      if (has_thread_tuning) {
        auto cpus = cfg.cpu_affinity;
        const SchedPolicy policy = cfg.sched_policy;
        const int priority = cfg.thread_priority;
        const std::string gname = cfg.name;
        group->multi_loop->register_worker_begin_handler([cpus, policy, priority, gname](size_t worker_idx) {
          apply_worker_tuning(cpus, policy, priority, worker_idx, false, gname);
        });
      }

      group->multi_loop->async_run();
    } else {
      const size_t count = std::max<size_t>(1U, cfg.threads);
      group->loops.reserve(count);

      for (size_t i = 0; i < count; ++i) {
        const std::string tname = "vl_" + cfg.name + "_" + std::to_string(i);
        auto loop = create_configured_loop(cfg, tname, i);
        loop->async_run();
        group->loops.push_back(std::move(loop));
      }
    }

    groups_[cfg.name] = std::move(group);
  };

  for (const auto& cfg : configs) {
    create_group(cfg);
  }

  if (groups_.count("default") == 0U) {
    create_group(default_cfg);
  }

  running_ = true;
  return true;
}

void SchedulerManager::shutdown(uint32_t timeout_ms) {
  std::unordered_map<std::string, std::unique_ptr<MessageLoop>> dedicated;
  std::unordered_map<std::string, std::unique_ptr<GroupRuntime>> groups;
  std::unordered_set<MessageLoop*> poisoned;

  {
    std::lock_guard lock(mtx_);

    if (!running_) {
      return;
    }

    running_ = false;
    dedicated.swap(dedicated_loops_);
    groups.swap(groups_);
    poisoned.swap(poisoned_loops_);
  }

  DeadlineTimer deadline(static_cast<int64_t>(timeout_ms == 0U ? 5000U : timeout_ms), ElapsedTimer::kMilli);

  for (auto& entry : dedicated) {
    if (entry.second && entry.second->is_running()) {
      entry.second->quit(false);
    }
  }

  for (auto& entry : groups) {
    auto& group = entry.second;

    if (!group) {
      continue;
    }

    if (group->multi_loop && group->multi_loop->is_running()) {
      group->multi_loop->quit(false);
    }

    for (auto& loop : group->loops) {
      if (loop && loop->is_running()) {
        loop->quit(false);
      }
    }
  }

  auto wait_loop_bounded = [&](auto& loop, const std::string& label) {
    if (!loop || !loop->is_running()) {
      return;
    }

    const bool is_poisoned = poisoned.count(loop.get()) != 0U;
    const int rem_ms = static_cast<int>(
        std::min<int64_t>(deadline.remaining_time(), static_cast<int64_t>(std::numeric_limits<int>::max())));
    const int wait_ms = is_poisoned ? std::max(50, rem_ms) : std::max(250, rem_ms);

    if (!loop->wait_for_quit(wait_ms)) {
      VLOG_E("SchedulerManager: Loop '", label, "' did not exit within ", wait_ms,
             "ms during shutdown; quarantining wedged loop.");
      park_leaked_object(std::shared_ptr<MessageLoop>(loop.release()));
    }
  };

  for (auto& entry : dedicated) {
    wait_loop_bounded(entry.second, entry.first);
  }

  for (auto& entry : groups) {
    auto& group = entry.second;

    if (!group) {
      continue;
    }

    wait_loop_bounded(group->multi_loop, entry.first);

    for (size_t i = 0U; i < group->loops.size(); ++i) {
      wait_loop_bounded(group->loops[i], entry.first + "[" + std::to_string(i) + "]");
    }

    if (group->thread_pool) {
      group->thread_pool->shutdown();
    }
  }
}

MessageLoop* SchedulerManager::acquire_loop(std::string_view group_name, std::string_view unit_name,
                                            int32_t worker_index) {
  std::lock_guard lock(mtx_);

  if VUNLIKELY (!running_) {
    return nullptr;
  }

  const std::string key = group_name.empty() ? "default" : std::string(group_name);
  auto it = groups_.find(key);

  if (it == groups_.end()) {
    return nullptr;
  }

  auto& group = *it->second;

  if (group.config.mode == SchedulerMode::kDedicated) {
    const std::string unit_key(unit_name);
    auto dit = dedicated_loops_.find(unit_key);

    if (dit != dedicated_loops_.end()) {
      if (poisoned_loops_.count(dit->second.get()) != 0U) {
        return nullptr;
      }

      return dit->second.get();
    }

    const size_t slot = (worker_index >= 0) ? static_cast<size_t>(worker_index)
                                            : group.next_index.fetch_add(1U, std::memory_order_relaxed);
    auto loop = create_configured_loop(group.config, "vl_" + unit_key, slot);
    loop->async_run();
    auto* raw = loop.get();
    dedicated_loops_.emplace(unit_key, std::move(loop));
    return raw;
  }

  if (group.multi_loop) {
    if (poisoned_loops_.count(group.multi_loop.get()) != 0U) {
      return nullptr;
    }

    return group.multi_loop.get();
  }

  if (!group.loops.empty()) {
    if (worker_index >= 0) {
      const size_t idx = static_cast<size_t>(worker_index) % group.loops.size();
      MessageLoop* chosen = group.loops[idx].get();

      if (poisoned_loops_.count(chosen) != 0U) {
        return nullptr;
      }

      return chosen;
    }

    const size_t count = group.loops.size();
    const size_t start_idx = group.next_index.fetch_add(1U, std::memory_order_relaxed) % count;

    for (size_t offset = 0U; offset < count; ++offset) {
      MessageLoop* candidate = group.loops[(start_idx + offset) % count].get();

      if (candidate != nullptr && poisoned_loops_.count(candidate) == 0U) {
        return candidate;
      }
    }
  }

  return nullptr;
}

ThreadPool* SchedulerManager::acquire_thread_pool(std::string_view group_name) const {
  std::lock_guard lock(mtx_);

  if VUNLIKELY (!running_ || group_name.empty()) {
    return nullptr;
  }

  auto it = groups_.find(std::string(group_name));

  if (it == groups_.end() || !it->second) {
    return nullptr;
  }

  return it->second->thread_pool.get();
}

std::unique_ptr<MessageLoop> SchedulerManager::detach_dedicated_loop(std::string_view unit_name,
                                                                     MessageLoop* expected_loop) {
  std::lock_guard lock(mtx_);
  auto it = dedicated_loops_.find(std::string(unit_name));

  if (it == dedicated_loops_.end()) {
    return nullptr;
  }

  if (expected_loop != nullptr && it->second.get() != expected_loop) {
    return nullptr;
  }

  auto loop = std::move(it->second);
  dedicated_loops_.erase(it);
  poisoned_loops_.erase(loop.get());
  return loop;
}

bool SchedulerManager::is_dedicated_group(std::string_view group_name) const {
  std::lock_guard lock(mtx_);
  const std::string key = group_name.empty() ? "default" : std::string(group_name);
  auto it = groups_.find(key);
  return it != groups_.end() && it->second && it->second->config.mode == SchedulerMode::kDedicated;
}

void SchedulerManager::mark_loop_poisoned(MessageLoop* loop) {
  if (loop == nullptr) {
    return;
  }

  std::lock_guard lock(mtx_);
  poisoned_loops_.insert(loop);
}

bool SchedulerManager::has_group(std::string_view group_name) const {
  std::lock_guard lock(mtx_);
  return groups_.count(std::string(group_name)) != 0U;
}

std::unique_ptr<MessageLoop> SchedulerManager::create_configured_loop(const SchedulerConfig& cfg,
                                                                      const std::string& thread_name,
                                                                      size_t worker_idx) {
  auto loop = std::make_unique<ConfiguredLoop<MessageLoop>>(cfg.max_elapsed_ms, cfg.name, cfg.queue_type);
  loop->set_name(clamp_thread_name(thread_name));
  loop->set_strategy(cfg.strategy);

  const bool pin_single = (cfg.mode == SchedulerMode::kChoreography);
  const auto cpus = cfg.cpu_affinity;
  const SchedPolicy policy = cfg.sched_policy;
  const int priority = cfg.thread_priority;

  if (!cpus.empty() || policy != SchedPolicy::kDefault || priority != 0) {
    loop->register_begin_handler([cpus, worker_idx, pin_single, policy, priority, name = cfg.name]() {
      apply_worker_tuning(cpus, policy, priority, worker_idx, pin_single, name);
    });
  }

  return loop;
}

}  // namespace launcher

}  // namespace vlink
