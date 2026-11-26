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
#include <vlink/base/cpu_profiler.h>
#include <vlink/base/functional.h>
#include <vlink/base/macros.h>
#include <vlink/base/plugin.h>
#include <vlink/extension/runnable_plugin_interface.h>
#include <vlink/external/launcher_component.h>
//
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "./launcher_config.h"
#include "./scheduler_manager.h"

namespace vlink {

namespace launcher {

class ComponentHost final {
 public:
  using ReadyCallback = Function<void(const std::string& name)>;
  using HealthCallback = Function<void(const std::string& name, bool healthy, const std::string& detail)>;
  using WatchdogCallback = Function<void(const std::string& name)>;

  explicit ComponentHost(SchedulerManager& scheduler);

  ~ComponentHost();

  void set_plugin_dirs(const std::vector<std::string>& dirs);

  void set_callbacks(ReadyCallback&& on_ready, HealthCallback&& on_health, WatchdogCallback&& on_watchdog = nullptr);

  [[nodiscard]] bool start_component(const ComponentSpec& spec, std::string* error_out, bool initial_active = true);

  bool stop_component(const std::string& name);

  void stop_all();

  [[nodiscard]] bool activate_component(const std::string& name, std::string* error_out);

  [[nodiscard]] bool deactivate_component(const std::string& name, std::string* error_out);

  [[nodiscard]] bool reload_component(const std::string& name, std::string* error_out);

  [[nodiscard]] bool has_component(const std::string& name) const;

  [[nodiscard]] double get_cpu_usage(const std::string& name) const;

  [[nodiscard]] uint64_t get_last_watchdog_ms(const std::string& name) const;

  [[nodiscard]] bool get_params(const std::string& name, LauncherParamMap& out) const;

  [[nodiscard]] bool set_param(const std::string& name, std::string_view key, const LauncherParamValue& value,
                               std::string* error_out);

  [[nodiscard]] bool set_component_record(const std::string& name, const std::string& path, bool force_publish,
                                          std::string* error_out);

 private:
  class ComponentContextImpl;

  struct HostLifeState final {
    std::shared_mutex mtx;
    bool alive{true};
  };

  struct LoadedInstance final {
    ComponentSpec spec;
    std::shared_ptr<LauncherModuleInterface> module;
    LauncherComponent* component{nullptr};
    std::shared_ptr<RunablePluginInterface> legacy_plugin;
    std::shared_ptr<ComponentContextImpl> ctx;
    CancellationSource cancel_source;
    MessageLoop* assigned_loop{nullptr};
    ThreadPool* compute_pool{nullptr};
    std::mutex lifecycle_mtx;
    std::atomic_bool started{false};
    std::shared_ptr<std::atomic_bool> active{MemoryResource::make_shared<std::atomic_bool>(false)};
  };

  std::deque<std::string> build_search_paths() const;

  std::shared_ptr<LauncherModuleInterface> load_or_get_module(const std::string& lib_name);

  void schedule_dedicated_release(const std::string& name, MessageLoop* assigned_loop);

  void join_reaper_threads();

  struct ReaperEntry final {
    std::shared_ptr<std::atomic_bool> done;
    std::thread thread;
  };

  std::shared_ptr<HostLifeState> life_state_;

  SchedulerManager& scheduler_;
  Plugin plugin_loader_;

  mutable std::mutex mtx_;
  ConditionVariable transition_cv_;
  std::unordered_set<std::string> transitioning_;
  std::mutex reaper_mtx_;
  std::vector<ReaperEntry> reaper_threads_;
  std::vector<std::string> plugin_dirs_;
  std::unordered_map<std::string, std::shared_ptr<LauncherModuleInterface>> module_cache_;
  std::unordered_map<std::string, std::shared_ptr<LoadedInstance>> instances_;
  ReadyCallback on_ready_;
  HealthCallback on_health_;
  WatchdogCallback on_watchdog_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(ComponentHost)
};

}  // namespace launcher

}  // namespace vlink
