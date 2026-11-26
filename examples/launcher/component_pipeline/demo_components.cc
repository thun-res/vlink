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
// File: demo_components.cc
//
// Example 1 -- In-process zero-copy perception -> fusion -> control pipeline.
//
// Demonstrates four collaborating components loaded into a single host process
// by vlink-launcher (see demo_launch.toml):
//   DemoSensor     -- LauncherTimerComponent publishing scan frames
//   DemoFusion     -- LauncherComponent offloading work to lock-free ThreadPool
//   DemoController -- Active / standby (kInactive) controller supporting hot
//                     activate/deactivate failover without teardown
//   DemoMonitor    -- Periodic subscriber reporting pipeline stats and CPU
//
// All VLink transport URLs are declared in demo_launch.toml ([urls],
// [component.inputs], [component.outputs]) and resolved via LauncherContext.
// =============================================================================

#include <vlink/base/logger.h>
#include <vlink/base/macros.h>
#include <vlink/external/launcher_component.h>
//
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

// Section: periodic sensor simulator driven by LauncherTimerComponent.
class DemoSensorComponent final : public vlink::LauncherTimerComponent {
 public:
  DemoSensorComponent() : vlink::LauncherTimerComponent(200U) {}

  bool on_timer_configure(vlink::LauncherContext& ctx) override {
    sensor_id_ = ctx.get_param_string("sensor_id", "lidar_0");
    VLOG_I("DemoSensor configured: id=", sensor_id_, ", out=", ctx.get_output_url("scan", "intra://demo/sensor/scan"),
           ", period_ms=", interval_ms());
    return true;
  }

  bool on_timer_start(vlink::LauncherContext& ctx) override {
    pub_ = ctx.create_publisher<std::string>("scan", "intra://demo/sensor/scan");
    return pub_.valid();
  }

  bool on_tick(vlink::LauncherContext& ctx) override {
    (void)ctx;

    if VLIKELY (pub_.valid()) {
      const uint64_t seq = seq_.fetch_add(1U, std::memory_order_relaxed) + 1U;
      (void)pub_.publish(sensor_id_ + ":range=" + std::to_string(2.5 + static_cast<double>(seq % 5U)) +
                         ":seq=" + std::to_string(seq));
    }

    return true;
  }

  bool on_reload(vlink::LauncherContext& ctx) override {
    sensor_id_ = ctx.get_param_string("sensor_id", sensor_id_);
    VLOG_I("DemoSensor reloaded: id=", sensor_id_);
    return true;
  }

  void on_timer_stop() override { pub_.reset(); }

  bool on_timer_param_changed(std::string_view key, const vlink::LauncherParamValue& value) override {
    if (key == "sensor_id") {
      const auto* s = std::get_if<std::string>(&value);

      if VUNLIKELY (s == nullptr) {
        return false;
      }

      sensor_id_ = *s;
      return true;
    }

    return true;
  }

 private:
  std::string sensor_id_;
  vlink::LauncherPublisher<std::string> pub_;
  std::atomic<uint64_t> seq_{0};
};

// Section: perception fusion offloading compute onto the component ThreadPool.
class DemoFusionComponent final : public vlink::LauncherComponent {
 public:
  bool on_configure(vlink::LauncherContext& ctx) override {
    threshold_m_ = ctx.get_param_double("threshold_m", 4.0);
    VLOG_I("DemoFusion configured: in=", ctx.get_input_url("scan", "intra://demo/sensor/scan"),
           ", out=", ctx.get_output_url("obstacles", "intra://demo/perception/obstacles"),
           ", threshold_m=", threshold_m_);
    return true;
  }

  bool on_start(vlink::LauncherContext& ctx) override {
    ctx_ = &ctx;
    pub_ = ctx.create_publisher<std::string>("obstacles", "intra://demo/perception/obstacles");
    sub_ = ctx.create_subscriber<std::string>(
        "scan",
        [this](const std::string& frame) {
          auto pub = pub_;

          if VUNLIKELY (ctx_ == nullptr || !pub) {
            return;
          }

          (void)ctx_->post_compute_task([this, pub = std::move(pub), frame]() {
            const uint64_t cnt = processed_.fetch_add(1U, std::memory_order_relaxed) + 1U;
            (void)pub.publish("obstacles[" + std::to_string(cnt) + "]<-" + frame);
          });
        },
        "intra://demo/sensor/scan");
    return pub_.valid() && sub_.valid();
  }

  bool on_activate(vlink::LauncherContext& ctx) override {
    ctx.mark_ready();
    return true;
  }

  void on_stop() override {
    sub_.reset();
    pub_.reset();
    ctx_ = nullptr;
  }

  bool on_param_changed(std::string_view key, const vlink::LauncherParamValue& value) override {
    if (key == "threshold_m") {
      const auto* d = std::get_if<double>(&value);

      if VUNLIKELY (d == nullptr || *d <= 0.0) {
        return false;
      }

      threshold_m_ = *d;
      return true;
    }

    return true;
  }

 private:
  vlink::LauncherContext* ctx_{nullptr};
  double threshold_m_{4.0};
  vlink::LauncherSubscriber<std::string> sub_;
  vlink::LauncherPublisher<std::string> pub_;
  std::atomic<uint64_t> processed_{0};
};

// Section: lifecycle-aware motion controller supporting active/standby switch.
class DemoControllerComponent final : public vlink::LauncherComponent {
 public:
  bool on_configure(vlink::LauncherContext& ctx) override {
    mode_tag_ = ctx.get_param_string("mode", "primary");
    VLOG_I("DemoController configured: mode=", mode_tag_,
           ", in=", ctx.get_input_url("obstacles", "intra://demo/perception/obstacles"),
           ", out=", ctx.get_output_url("cmd_vel", "intra://demo/control/cmd_vel"));
    return true;
  }

  bool on_start(vlink::LauncherContext& ctx) override {
    pub_ = ctx.create_publisher<std::string>("cmd_vel", "intra://demo/control/cmd_vel");
    sub_ = ctx.create_subscriber<std::string>(
        "obstacles",
        [this](const std::string& obs) {
          if VUNLIKELY (!pub_) {
            return;
          }

          const uint64_t seq = cmds_.fetch_add(1U, std::memory_order_relaxed) + 1U;
          (void)pub_.publish("cmd[" + mode_tag_ + "#" + std::to_string(seq) + "](" + obs + ")");
        },
        "intra://demo/perception/obstacles");
    return pub_.valid() && sub_.valid();
  }

  bool on_activate(vlink::LauncherContext& ctx) override {
    ctx.mark_ready();
    VLOG_I("DemoController activated: mode=", mode_tag_);
    return true;
  }

  void on_deactivate() override { VLOG_I("DemoController deactivated (standby): mode=", mode_tag_); }

  bool on_reload(vlink::LauncherContext& ctx) override {
    mode_tag_ = ctx.get_param_string("mode", mode_tag_);
    VLOG_I("DemoController reloaded: mode=", mode_tag_);
    return true;
  }

  void on_stop() override {
    sub_.reset();
    pub_.reset();
  }

 private:
  std::string mode_tag_;
  vlink::LauncherSubscriber<std::string> sub_;
  vlink::LauncherPublisher<std::string> pub_;
  std::atomic<uint64_t> cmds_{0};
};

// Section: periodic diagnostics sink observing command output and CPU load.
class DemoMonitorComponent final : public vlink::LauncherTimerComponent {
 public:
  DemoMonitorComponent() : vlink::LauncherTimerComponent(1000U) {}

  bool on_timer_start(vlink::LauncherContext& ctx) override {
    sub_ = ctx.create_subscriber<std::string>(
        "cmd_vel",
        [this](const std::string& msg) {
          recv_count_.fetch_add(1U, std::memory_order_relaxed);
          VLOG_D("DemoMonitor observed: ", msg);
        },
        "intra://demo/control/cmd_vel");
    return sub_.valid();
  }

  bool on_tick(vlink::LauncherContext& ctx) override {
    VLOG_I("DemoMonitor stats: received=", recv_count_.load(std::memory_order_relaxed), ", cpu=", ctx.get_cpu_usage(),
           "%");
    return true;
  }

  void on_timer_stop() override { sub_.reset(); }

 private:
  vlink::LauncherSubscriber<std::string> sub_;
  std::atomic<uint64_t> recv_count_{0};
};

#ifdef VLINK_ENABLE_COROUTINE
// Section: optional C++20 coroutine heartbeat monitor.
class DemoCoroMonitor final : public vlink::LauncherCoroutineComponent {
 public:
  vlink::Co::Task<void> on_coro_run(vlink::LauncherContext& ctx) override {
    ctx.mark_ready();

    while (co_await ctx.co_sleep(1000U)) {
      ctx.watchdog_ping();
      VLOG_D("DemoCoroMonitor heartbeat (cpu=", ctx.get_cpu_usage(), "%)");
    }
  }
};
#endif

VLINK_LAUNCHER_MODULE_BEGIN(1, 0)
VLINK_LAUNCHER_EXPORT_COMPONENT("DemoSensor", DemoSensorComponent)
VLINK_LAUNCHER_EXPORT_COMPONENT("DemoFusion", DemoFusionComponent)
VLINK_LAUNCHER_EXPORT_COMPONENT("DemoController", DemoControllerComponent)
VLINK_LAUNCHER_EXPORT_COMPONENT("DemoMonitor", DemoMonitorComponent)
#ifdef VLINK_ENABLE_COROUTINE
VLINK_LAUNCHER_EXPORT_COMPONENT("DemoCoroMonitor", DemoCoroMonitor)
#endif
VLINK_LAUNCHER_MODULE_END()
