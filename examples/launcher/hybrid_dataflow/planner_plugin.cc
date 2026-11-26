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

// =============================================================================
// File: planner_plugin.cc
//
// Example 2 -- Containerized plugin components for hybrid multi-process stack.
//
// Loaded inside an isolated [[container]] process (see hybrid_dataflow.toml):
//   HybridPlanner    -- Subscribes to front/rear external sensor processes,
//                       runs planning on ThreadPool, publishes cmd_vel to
//                       the external actuator process.
//   SafetySupervisor -- Periodically polls the external actuator's RPC status
//                       endpoint and reports health to LauncherServer.
// =============================================================================

#include <vlink/base/logger.h>
#include <vlink/base/macros.h>
#include <vlink/client.h>
#include <vlink/external/launcher_component.h>
//
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

// Section: multi-input planner component running inside planning_host container.
class HybridPlannerComponent final : public vlink::LauncherComponent {
 public:
  bool on_configure(vlink::LauncherContext& ctx) override {
    max_speed_.store(ctx.get_param_double("max_speed_mps", 1.5), std::memory_order_relaxed);
    VLOG_I("HybridPlanner configured: front=", ctx.get_input_url("scan_front", "dds://hybrid/sensor/front/scan"),
           ", rear=", ctx.get_input_url("scan_rear", "dds://hybrid/sensor/rear/scan"),
           ", out=", ctx.get_output_url("cmd_vel", "dds://hybrid/control/cmd_vel"),
           ", max_speed=", max_speed_.load(std::memory_order_relaxed));
    return true;
  }

  bool on_start(vlink::LauncherContext& ctx) override {
    ctx_ = &ctx;
    pub_ = ctx.create_publisher<std::string>("cmd_vel", "dds://hybrid/control/cmd_vel");

    auto on_scan = [this](const std::string& scan) {
      auto pub = pub_;

      if VUNLIKELY (ctx_ == nullptr || !pub) {
        return;
      }

      (void)ctx_->post_compute_task([this, pub = std::move(pub), scan]() {
        const uint64_t plan_id = plans_.fetch_add(1U, std::memory_order_relaxed) + 1U;
        const double speed = max_speed_.load(std::memory_order_relaxed);
        (void)pub.publish("plan#" + std::to_string(plan_id) + ":v=" + std::to_string(speed) + ":src=" + scan);
      });
    };

    sub_front_ = ctx.create_subscriber<std::string>("scan_front", on_scan, "dds://hybrid/sensor/front/scan");
    sub_rear_ = ctx.create_subscriber<std::string>("scan_rear", on_scan, "dds://hybrid/sensor/rear/scan");
    return pub_.valid() && sub_front_.valid() && sub_rear_.valid();
  }

  bool on_activate(vlink::LauncherContext& ctx) override {
    ctx.mark_ready();
    return true;
  }

  void on_stop() override {
    sub_front_.reset();
    sub_rear_.reset();
    pub_.reset();
    ctx_ = nullptr;
  }

  bool on_param_changed(std::string_view key, const vlink::LauncherParamValue& value) override {
    if (key == "max_speed_mps") {
      const auto* d = std::get_if<double>(&value);

      if VUNLIKELY (d == nullptr || *d < 0.0) {
        return false;
      }

      max_speed_.store(*d, std::memory_order_relaxed);
      return true;
    }

    return true;
  }

 private:
  vlink::LauncherContext* ctx_{nullptr};
  std::atomic<double> max_speed_{1.5};
  vlink::LauncherSubscriber<std::string> sub_front_;
  vlink::LauncherSubscriber<std::string> sub_rear_;
  vlink::LauncherPublisher<std::string> pub_;
  std::atomic<uint64_t> plans_{0};
};

// Section: periodic RPC health supervisor polling the external actuator node.
class SafetySupervisorComponent final : public vlink::LauncherTimerComponent {
 public:
  SafetySupervisorComponent() : vlink::LauncherTimerComponent(1000U) {}

  bool on_timer_configure(vlink::LauncherContext& ctx) override {
    rpc_url_ = ctx.get_url("status_rpc", "dds://hybrid/actuator/status");
    VLOG_I("SafetySupervisor configured: rpc=", rpc_url_);
    return true;
  }

  bool on_timer_start(vlink::LauncherContext& ctx) override {
    (void)ctx;
    client_ = std::make_unique<vlink::Client<std::string, std::string>>(rpc_url_);
    return true;
  }

  bool on_tick(vlink::LauncherContext& ctx) override {
    if VLIKELY (client_ != nullptr) {
      std::string resp;

      if VLIKELY (client_->invoke("ping", resp, std::chrono::milliseconds(300))) {
        ctx.report_health(true, resp);
        VLOG_I("SafetySupervisor actuator health: ", resp);
      } else {
        ctx.report_health(false, "actuator status rpc timeout");
      }
    }

    return true;
  }

  void on_timer_stop() override { client_.reset(); }

 private:
  std::string rpc_url_;
  std::unique_ptr<vlink::Client<std::string, std::string>> client_;
};

VLINK_LAUNCHER_MODULE_BEGIN(1, 0)
VLINK_LAUNCHER_EXPORT_COMPONENT("HybridPlanner", HybridPlannerComponent)
VLINK_LAUNCHER_EXPORT_COMPONENT("SafetySupervisor", SafetySupervisorComponent)
VLINK_LAUNCHER_MODULE_END()
