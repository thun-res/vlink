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

#include <vlink/base/cancellation.h>
#include <vlink/base/condition_variable.h>
#include <vlink/base/functional.h>
#include <vlink/base/macros.h>
#include <vlink/base/message_loop.h>
#include <vlink/base/plugin.h>
#include <vlink/extension/runnable_plugin_interface.h>
#include <vlink/external/launcher/module.h>
//
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "./launcher_config.h"
#include "./scheduler_manager.h"

namespace vlink {

namespace launcher {

enum class ComponentStopResult : uint8_t {
  kNotFound = 0,
  kStoppedClean = 1,
  kDeferredDrain = 2,
  kTimedOutQuarantined = 3,
};

class ComponentHost final {
 public:
  using ReadyCallback = Function<void(const std::string& name, uint64_t generation)>;
  using HealthCallback =
      Function<void(const std::string& name, uint64_t generation, bool healthy, const std::string& detail)>;
  using WatchdogCallback = Function<void(const std::string& name, uint64_t generation)>;

  explicit ComponentHost(SchedulerManager& scheduler);

  ~ComponentHost();

  void set_plugin_dirs(const std::vector<std::string>& dirs);

  void set_callbacks(ReadyCallback&& on_ready, HealthCallback&& on_health, WatchdogCallback&& on_watchdog = nullptr);

  [[nodiscard]] bool start_component(const ComponentSpec& spec, uint64_t generation, std::string* error_out,
                                     bool initial_active = true, uint32_t timeout_ms = 5000U);

  ComponentStopResult stop_component(const std::string& name, uint32_t timeout_ms = 5000U,
                                     uint64_t expected_generation = 0U);

  void stop_all(uint32_t timeout_ms = 5000U);

  [[nodiscard]] bool activate_component(const std::string& name, std::string* error_out, uint32_t timeout_ms = 5000U);

  [[nodiscard]] bool deactivate_component(const std::string& name, std::string* error_out, uint32_t timeout_ms = 5000U);

  [[nodiscard]] bool reload_component(const std::string& name, std::string* error_out, uint32_t timeout_ms = 5000U);

  [[nodiscard]] bool has_component(const std::string& name) const;

  [[nodiscard]] double get_cpu_usage(const std::string& name) const;

  [[nodiscard]] uint64_t get_last_watchdog_ms(const std::string& name) const;

  [[nodiscard]] bool get_params(const std::string& name, LauncherParamMap& out) const;

  [[nodiscard]] bool set_param(const std::string& name, std::string_view key, const LauncherParamValue& value,
                               std::string* error_out, uint32_t timeout_ms = 5000U);

  [[nodiscard]] bool set_component_record(const std::string& name, const std::string& path, bool force_publish,
                                          std::string* error_out, uint32_t timeout_ms = 5000U);

 private:
  class ComponentContextImpl;

  struct HostLifeState final {
    std::shared_mutex mtx;
    bool alive{true};
  };

  struct LoadedInstance final {
    ComponentSpec spec;
    uint64_t generation{0};
    std::shared_ptr<LauncherModuleInterface> module;
    LauncherComponent* component{nullptr};
    std::shared_ptr<RunablePluginInterface> legacy_plugin;
    std::shared_ptr<ComponentContextImpl> ctx;
    CancellationSource cancel_source;
    MessageLoop* assigned_loop{nullptr};
    ThreadPool* compute_pool{nullptr};
    std::mutex lifecycle_mtx;
    std::atomic_bool started{false};
    std::shared_ptr<std::atomic_bool> active{std::make_shared<std::atomic_bool>(false)};
  };

  std::shared_ptr<LoadedInstance> find_instance(const std::string& name) const;

  template <typename OpFnT>
  bool dispatch_component_op(const std::string& name, uint32_t timeout_ms, const char* op_label,
                             const char* not_found_msg, std::string* error_out, OpFnT&& op_fn);

  std::deque<std::string> build_search_paths() const;

  std::shared_ptr<LauncherModuleInterface> load_or_get_module(const std::string& lib_name);

  void schedule_dedicated_release(const std::string& name, MessageLoop* assigned_loop, uint32_t timeout_ms);

  bool try_post_deferred_cleanup(const Function<void()>& fn);

  std::shared_ptr<HostLifeState> life_state_;

  SchedulerManager& scheduler_;
  std::mutex module_mtx_;
  Plugin plugin_loader_;
  std::unordered_map<std::string, std::shared_ptr<LauncherModuleInterface>> module_cache_;

  mutable std::mutex mtx_;
  ConditionVariable transition_cv_;
  std::unordered_set<std::string> transitioning_;
  std::vector<std::string> plugin_dirs_;
  std::unordered_map<std::string, std::shared_ptr<LoadedInstance>> instances_;
  ReadyCallback on_ready_;
  HealthCallback on_health_;
  WatchdogCallback on_watchdog_;

  std::mutex cleanup_loop_mtx_;
  std::unique_ptr<MessageLoop> cleanup_loop_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(ComponentHost)
};

}  // namespace launcher

}  // namespace vlink
