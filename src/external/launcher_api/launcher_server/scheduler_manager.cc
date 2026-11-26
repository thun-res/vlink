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

#include <vlink/base/condition_variable.h>
#include <vlink/base/logger.h>
#include <vlink/base/utils.h>
//
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__linux__) && !defined(__ANDROID__)
#include <pthread.h>
#include <sched.h>
#elif defined(__ANDROID__)
#include <sched.h>
#elif defined(__QNX__) || defined(__QNXNTO__)
#include <sched.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>
#elif !defined(_WIN32) && !defined(_WIN64)
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

class ConfiguredMessageLoop final : public MessageLoop {
 public:
  ConfiguredMessageLoop(Type type, uint32_t max_elapsed_ms, std::string group_name)
      : MessageLoop(type), max_elapsed_ms_(max_elapsed_ms), group_name_(std::move(group_name)) {}

  [[nodiscard]] uint32_t get_max_elapsed_time() const override { return max_elapsed_ms_; }

 protected:
  void on_task_timeout(Callback&& callback, uint32_t elapsed_time) override {
    (void)callback;
    const uint64_t count = timeout_count_.fetch_add(1U, std::memory_order_relaxed) + 1U;
    VLOG_W_EVERY_MS(5000, "SchedulerManager: Group '", group_name_,
                    "' dropped overdue queued task (elapsed=", elapsed_time, "ms, budget=", max_elapsed_ms_,
                    "ms, total_timeouts=", count, ").");
  }

 private:
  uint32_t max_elapsed_ms_{0};
  std::string group_name_;
  std::atomic<uint64_t> timeout_count_{0};
};

class ConfiguredMultiLoop final : public MultiLoop {
 public:
  ConfiguredMultiLoop(size_t thread_num, Type type, uint32_t max_elapsed_ms, std::string group_name)
      : MultiLoop(thread_num, type), max_elapsed_ms_(max_elapsed_ms), group_name_(std::move(group_name)) {}

  [[nodiscard]] uint32_t get_max_elapsed_time() const override { return max_elapsed_ms_; }

 protected:
  void on_task_timeout(Callback&& callback, uint32_t elapsed_time) override {
    (void)callback;
    const uint64_t count = timeout_count_.fetch_add(1U, std::memory_order_relaxed) + 1U;
    VLOG_W_EVERY_MS(5000, "SchedulerManager: Group '", group_name_,
                    "' dropped overdue queued task (elapsed=", elapsed_time, "ms, budget=", max_elapsed_ms_,
                    "ms, total_timeouts=", count, ").");
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
      group->thread_pool = std::make_unique<ThreadPool>(workers, to_thread_pool_type(cfg.queue_type));
      group->thread_pool->set_name(clamp_thread_name("vlp_" + cfg.name));
      group->thread_pool->set_strategy(to_thread_pool_strategy(cfg.strategy));

      if (has_thread_tuning) {
        struct TuneBarrier final {
          std::mutex mtx;
          ConditionVariable cv;
          std::unordered_set<std::thread::id> tuned_threads;
          bool release{false};
        };

        auto barrier = std::make_shared<TuneBarrier>();
        const int native_policy = to_native_sched_policy(cfg.sched_policy);
        const int priority = cfg.thread_priority;
        const auto cpus = cfg.cpu_affinity;
        std::vector<TaskHandle> handles;
        handles.reserve(workers);

        for (size_t i = 0U; i < workers; ++i) {
          handles.push_back(group->thread_pool->post_task_handle(
              [barrier, workers, cpus, i, native_policy, priority, name = cfg.name]() {
                const auto tid = std::this_thread::get_id();
                bool first_on_thread = false;

                {
                  std::lock_guard lk(barrier->mtx);
                  first_on_thread = barrier->tuned_threads.insert(tid).second;
                }

                if (first_on_thread) {
                  if (!cpus.empty() && !apply_current_thread_affinity(cpus, i, false)) {
                    VLOG_W("SchedulerManager: Failed to set thread_pool CPU affinity for group '", name, "'.");
                  }

                  if ((native_policy >= 0 || priority != 0) && !Utils::set_thread_priority(priority, native_policy)) {
                    VLOG_W("SchedulerManager: Failed to set thread_pool priority for group '", name, "'.");
                  }
                }

                std::unique_lock lk(barrier->mtx);

                if (barrier->tuned_threads.size() >= workers) {
                  barrier->release = true;
                  barrier->cv.notify_all();
                } else {
                  (void)barrier->cv.wait_for(lk, std::chrono::milliseconds(200), [&] { return barrier->release; });
                }
              }));
        }

        for (const auto& h : handles) {
          (void)h.wait(300);
        }
      }

      auto anchor_loop = create_configured_loop(cfg, "vl_" + cfg.name + "_0", 0U);
      anchor_loop->async_run();
      group->loops.push_back(std::move(anchor_loop));
    } else if (cfg.mode == SchedulerMode::kMulti && !has_thread_tuning) {
      group->multi_loop = std::make_unique<ConfiguredMultiLoop>(std::max<size_t>(1U, cfg.threads), cfg.queue_type,
                                                                cfg.max_elapsed_ms, cfg.name);
      group->multi_loop->set_name(clamp_thread_name("vl_" + cfg.name));
      group->multi_loop->set_strategy(cfg.strategy);
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

void SchedulerManager::shutdown() {
  std::unordered_map<std::string, std::unique_ptr<MessageLoop>> dedicated;
  std::unordered_map<std::string, std::unique_ptr<GroupRuntime>> groups;

  {
    std::lock_guard lock(mtx_);

    if (!running_) {
      return;
    }

    running_ = false;
    dedicated.swap(dedicated_loops_);
    groups.swap(groups_);
  }

  for (auto& entry : dedicated) {
    auto& loop = entry.second;

    if (loop && loop->is_running()) {
      loop->quit(false);
      loop->wait_for_quit();
    }
  }

  for (auto& entry : groups) {
    auto& group = entry.second;

    if (!group) {
      continue;
    }

    if (group->multi_loop && group->multi_loop->is_running()) {
      group->multi_loop->quit(false);
      group->multi_loop->wait_for_quit();
    }

    for (auto& loop : group->loops) {
      if (loop && loop->is_running()) {
        loop->quit(false);
        loop->wait_for_quit();
      }
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
      return dit->second.get();
    }

    const size_t slot = (worker_index >= 0) ? static_cast<size_t>(worker_index) : dedicated_loops_.size();
    auto loop = create_configured_loop(group.config, "vl_" + unit_key, slot);
    loop->async_run();
    auto* raw = loop.get();
    dedicated_loops_.emplace(unit_key, std::move(loop));
    return raw;
  }

  if (group.multi_loop) {
    return group.multi_loop.get();
  }

  if (!group.loops.empty()) {
    size_t idx = 0U;

    if (worker_index >= 0) {
      idx = static_cast<size_t>(worker_index) % group.loops.size();
    } else {
      idx = group.next_index.fetch_add(1U, std::memory_order_relaxed) % group.loops.size();
    }

    return group.loops[idx].get();
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

std::unique_ptr<MessageLoop> SchedulerManager::detach_dedicated_loop(std::string_view unit_name) {
  std::lock_guard lock(mtx_);
  auto it = dedicated_loops_.find(std::string(unit_name));

  if (it == dedicated_loops_.end()) {
    return nullptr;
  }

  auto loop = std::move(it->second);
  dedicated_loops_.erase(it);
  return loop;
}

bool SchedulerManager::has_group(std::string_view group_name) const {
  std::lock_guard lock(mtx_);
  return groups_.count(std::string(group_name)) != 0U;
}

size_t SchedulerManager::group_count() const {
  std::lock_guard lock(mtx_);
  return groups_.size();
}

std::unique_ptr<MessageLoop> SchedulerManager::create_configured_loop(const SchedulerConfig& cfg,
                                                                      const std::string& thread_name,
                                                                      size_t worker_idx) {
  auto loop = std::make_unique<ConfiguredMessageLoop>(cfg.queue_type, cfg.max_elapsed_ms, cfg.name);
  loop->set_name(clamp_thread_name(thread_name));
  loop->set_strategy(cfg.strategy);

  const bool pin_single = (cfg.mode == SchedulerMode::kChoreography);
  const auto cpus = cfg.cpu_affinity;
  const int native_policy = to_native_sched_policy(cfg.sched_policy);
  const int priority = cfg.thread_priority;

  if (!cpus.empty() || native_policy >= 0 || priority != 0) {
    loop->register_begin_handler([cpus, worker_idx, pin_single, native_policy, priority, name = cfg.name]() {
      if (!cpus.empty() && !apply_current_thread_affinity(cpus, worker_idx, pin_single)) {
        VLOG_W("SchedulerManager: Failed to set CPU affinity for group '", name, "'.");
      }

      if ((native_policy >= 0 || priority != 0) && !Utils::set_thread_priority(priority, native_policy)) {
        VLOG_W("SchedulerManager: Failed to set thread priority for group '", name, "'.");
      }
    });
  }

  return loop;
}

}  // namespace launcher

}  // namespace vlink
