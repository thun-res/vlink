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
#include <vlink/base/message_loop.h>
#include <vlink/base/plugin.h>
#include <vlink/external/launcher_component.h>

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

class ComponentHost final {
 public:
  using ReadyCallback = MoveFunction<void(const std::string& name, uint64_t gen, const std::string& detail)>;
  using HealthCallback =
      MoveFunction<void(const std::string& name, uint64_t gen, bool healthy, const std::string& detail)>;
  using StopRequestCallback = MoveFunction<void(const std::string& name, uint64_t gen)>;

  ComponentHost() = default;
  ~ComponentHost();

  ComponentHost(const ComponentHost&) = delete;
  ComponentHost& operator=(const ComponentHost&) = delete;

  void set_plugin_dirs(const std::vector<std::string>& dirs);
  void set_callbacks(ReadyCallback on_ready, HealthCallback on_health, StopRequestCallback on_stop_req);

  bool start_component(const ComponentSpec& spec, uint64_t generation, std::string* error_out = nullptr);

  bool stop_component(const std::string& name, uint32_t timeout_ms = 5000,
                      std::optional<uint64_t> expected_gen = std::nullopt);

  bool reload_component(const std::string& name, std::string* error_out = nullptr);

  bool set_param(const std::string& name, const std::string& key, const LauncherParamValue& value,
                 std::string* error_out = nullptr);

  [[nodiscard]] std::optional<LauncherParamMap> get_params(const std::string& name) const;

  [[nodiscard]] uint64_t get_last_watchdog_ms(const std::string& name) const noexcept;

  void stop_all();

 private:
  struct Callbacks final {
    ReadyCallback on_ready;
    HealthCallback on_health;
    StopRequestCallback on_stop_req;
  };

  struct SharedCallbacks final {
    std::mutex mtx;
    std::shared_ptr<Callbacks> callbacks;
  };

  struct Entry final {
    std::string name;
    uint64_t generation{0};
    bool started{false};
    std::mutex op_mtx;
    std::atomic_bool stopped{false};
    std::atomic<uint32_t> in_flight_ops{0};
    std::shared_ptr<LauncherModuleInterface> module;
    LauncherComponent* instance{nullptr};
    std::unique_ptr<LauncherContext> context;
    std::unique_ptr<MessageLoop> dedicated_loop;
    MessageLoop* active_loop{nullptr};
    std::atomic<uint64_t> last_watchdog_ms{0};
  };

  void cleanup_entry(const std::shared_ptr<Entry>& entry, bool invoke_on_stop, uint32_t timeout_ms,
                     bool start_completed = true);
  std::shared_ptr<LauncherModuleInterface> load_module_locked(const std::string& library, std::string* error_out);
  MessageLoop* ensure_shared_loop_locked(MessageLoop::Type qtype, MessageLoop::Strategy qstrat);

  mutable std::mutex mtx_;
  std::shared_ptr<SharedCallbacks> shared_cbs_{std::make_shared<SharedCallbacks>()};
  Plugin plugin_loader_;
  std::vector<std::string> plugin_dirs_;
  std::unordered_map<std::string, std::shared_ptr<LauncherModuleInterface>> modules_;
  std::unordered_map<std::string, std::shared_ptr<Entry>> entries_;
  std::vector<std::shared_ptr<Entry>> orphans_;
  std::unordered_map<uint16_t, std::unique_ptr<MessageLoop>> shared_loops_;
};

}  // namespace vlink::launcher
