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
// File: embedded_components.cc
//
// Example 3 -- Self-contained plugin library for the embedded C++ orchestrator.
//
// Exports five collaborating components across @core, @driving, @parking and
// dynamic hot-plug roles (see embedded_orchestrator.toml):
//   VehicleBridge   -- @core chassis bridge kept online across mode switches
//   OdomFeed        -- @driving periodic odometry publisher
//   CruisePlanner   -- @driving active/standby (kInactive) cruise planner with
//                      ThreadPool compute offload, live param & hot reload
//   ParkingPlanner  -- @parking on-demand planner (autostart = false) started
//                      atomically via isolate_group("@parking")
//   TelemetryTap    -- Dynamically loaded observer verifying runtime URL remap
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

// Section: core chassis bridge kept running across @driving -> @parking switch.
class VehicleBridgeComponent final : public vlink::LauncherComponent {
 public:
  bool on_configure(vlink::LauncherContext& ctx) override {
    VLOG_I("VehicleBridge configured: in=", ctx.get_input_url("cmd", "intra://embedded/vehicle/cmd"));
    return true;
  }

  bool on_start(vlink::LauncherContext& ctx) override {
    ctx_ = &ctx;
    sub_ = ctx.create_subscriber<std::string>(
        "cmd",
        [this](const std::string& cmd) {
          const uint64_t count = applied_.fetch_add(1U, std::memory_order_relaxed) + 1U;

          if (count % 4U == 1U) {
            VLOG_I("VehicleBridge applied #", count, ": ", cmd);

            if VLIKELY (ctx_ != nullptr) {
              ctx_->report_health(true, "applied=" + std::to_string(count));
            }
          }
        },
        "intra://embedded/vehicle/cmd");
    return sub_.valid();
  }

  bool on_activate(vlink::LauncherContext& ctx) override {
    ctx.mark_ready();
    return true;
  }

  void on_stop() override {
    sub_.reset();
    ctx_ = nullptr;
  }

 private:
  vlink::LauncherContext* ctx_{nullptr};
  vlink::LauncherSubscriber<std::string> sub_;
  std::atomic<uint64_t> applied_{0};
};

// Section: periodic odometry publisher active in @driving mode.
class OdomFeedComponent final : public vlink::LauncherTimerComponent {
 public:
  OdomFeedComponent() : vlink::LauncherTimerComponent(120U) {}

  bool on_timer_configure(vlink::LauncherContext& ctx) override {
    VLOG_I("OdomFeed configured: out=", ctx.get_output_url("odom", "intra://embedded/vehicle/odom"),
           ", period_ms=", interval_ms());
    return true;
  }

  bool on_timer_start(vlink::LauncherContext& ctx) override {
    pub_ = ctx.create_publisher<std::string>("odom", "intra://embedded/vehicle/odom");
    return pub_.valid();
  }

  bool on_tick(vlink::LauncherContext& ctx) override {
    (void)ctx;

    if VLIKELY (pub_.valid()) {
      const uint64_t seq = seq_.fetch_add(1U, std::memory_order_relaxed) + 1U;
      (void)pub_.publish("odom#" + std::to_string(seq) + ":v=" + std::to_string(12.0 + static_cast<double>(seq % 4U)));
    }

    return true;
  }

  void on_timer_stop() override { pub_.reset(); }

 private:
  vlink::LauncherPublisher<std::string> pub_;
  std::atomic<uint64_t> seq_{0};
};

// Section: cruise planner supporting active/standby failover, live params and hot reload.
class CruisePlannerComponent final : public vlink::LauncherComponent {
 public:
  bool on_configure(vlink::LauncherContext& ctx) override {
    role_ = ctx.get_param_string("role", "primary");
    target_speed_mps_.store(ctx.get_param_double("target_speed_mps", 15.0), std::memory_order_relaxed);
    VLOG_I("CruisePlanner configured: role=", role_,
           ", target_speed=", target_speed_mps_.load(std::memory_order_relaxed),
           ", in=", ctx.get_input_url("odom", "intra://embedded/vehicle/odom"),
           ", out=", ctx.get_output_url("cmd", "intra://embedded/vehicle/cmd"));
    return true;
  }

  bool on_start(vlink::LauncherContext& ctx) override {
    ctx_ = &ctx;
    pub_ = ctx.create_publisher<std::string>("cmd", "intra://embedded/vehicle/cmd");
    sub_ = ctx.create_subscriber<std::string>(
        "odom",
        [this](const std::string& odom) {
          auto pub = pub_;

          if VUNLIKELY (ctx_ == nullptr || !pub) {
            return;
          }

          const double speed = target_speed_mps_.load(std::memory_order_relaxed);
          (void)ctx_->post_compute_task([this, pub = std::move(pub), odom, speed]() {
            const uint64_t seq = plans_.fetch_add(1U, std::memory_order_relaxed) + 1U;
            (void)pub.publish("cruise[" + role_ + "#" + std::to_string(seq) + ":v=" + std::to_string(speed) + "]<-" +
                              odom);
          });
        },
        "intra://embedded/vehicle/odom");
    return pub_.valid() && sub_.valid();
  }

  bool on_activate(vlink::LauncherContext& ctx) override {
    ctx.mark_ready();
    VLOG_I("CruisePlanner activated: role=", role_);
    return true;
  }

  void on_deactivate() override { VLOG_I("CruisePlanner deactivated (standby): role=", role_); }

  bool on_reload(vlink::LauncherContext& ctx) override {
    const double cur_speed = target_speed_mps_.load(std::memory_order_relaxed);
    const double next_speed = ctx.get_param_double("target_speed_mps", cur_speed);
    target_speed_mps_.store(next_speed, std::memory_order_relaxed);
    VLOG_I("CruisePlanner reloaded: role=", role_, ", target_speed=", next_speed);
    return true;
  }

  void on_stop() override {
    sub_.reset();
    pub_.reset();
    ctx_ = nullptr;
  }

  bool on_param_changed(std::string_view key, const vlink::LauncherParamValue& value) override {
    if (key == "target_speed_mps") {
      const auto* v = std::get_if<double>(&value);

      if VUNLIKELY (v == nullptr || *v <= 0.0 || *v > 40.0) {
        return false;
      }

      target_speed_mps_.store(*v, std::memory_order_relaxed);
      return true;
    }

    return true;
  }

 private:
  vlink::LauncherContext* ctx_{nullptr};
  std::string role_;
  std::atomic<double> target_speed_mps_{15.0};
  vlink::LauncherSubscriber<std::string> sub_;
  vlink::LauncherPublisher<std::string> pub_;
  std::atomic<uint64_t> plans_{0};
};

// Section: on-demand @parking planner activated via isolate_group("@parking").
class ParkingPlannerComponent final : public vlink::LauncherTimerComponent {
 public:
  ParkingPlannerComponent() : vlink::LauncherTimerComponent(100U) {}

  bool on_timer_configure(vlink::LauncherContext& ctx) override {
    slot_id_ = ctx.get_param_string("slot_id", "B2-08");
    VLOG_I("ParkingPlanner configured: slot=", slot_id_,
           ", out=", ctx.get_output_url("cmd", "intra://embedded/vehicle/cmd"));
    return true;
  }

  bool on_timer_start(vlink::LauncherContext& ctx) override {
    pub_ = ctx.create_publisher<std::string>("cmd", "intra://embedded/vehicle/cmd");
    return pub_.valid();
  }

  bool on_tick(vlink::LauncherContext& ctx) override {
    (void)ctx;

    if VLIKELY (pub_.valid()) {
      const uint64_t step = step_.fetch_add(1U, std::memory_order_relaxed) + 1U;
      (void)pub_.publish("park[slot=" + slot_id_ + ":step=" + std::to_string(step) + ":v=0.8]");
    }

    return true;
  }

  void on_timer_stop() override { pub_.reset(); }

 private:
  std::string slot_id_;
  vlink::LauncherPublisher<std::string> pub_;
  std::atomic<uint64_t> step_{0};
};

// Section: dynamically hot-plugged observer wired via LoadComponentRequest.inputs.
class TelemetryTapComponent final : public vlink::LauncherTimerComponent {
 public:
  TelemetryTapComponent() : vlink::LauncherTimerComponent(150U) {}

  bool on_timer_configure(vlink::LauncherContext& ctx) override {
    VLOG_I("TelemetryTap configured: tap_url=", ctx.get_input_url("cmd", "intra://embedded/tap/default"));
    return true;
  }

  bool on_timer_start(vlink::LauncherContext& ctx) override {
    sub_ = ctx.create_subscriber<std::string>(
        "cmd",
        [this](const std::string& msg) {
          observed_.fetch_add(1U, std::memory_order_relaxed);
          VLOG_D("TelemetryTap captured: ", msg);
        },
        "intra://embedded/tap/default");
    return sub_.valid();
  }

  bool on_tick(vlink::LauncherContext& ctx) override {
    VLOG_I("TelemetryTap summary: captured=", observed_.load(std::memory_order_relaxed), ", cpu=", ctx.get_cpu_usage(),
           "%");
    return true;
  }

  void on_timer_stop() override { sub_.reset(); }

 private:
  vlink::LauncherSubscriber<std::string> sub_;
  std::atomic<uint64_t> observed_{0};
};

VLINK_LAUNCHER_MODULE_BEGIN(1, 0)
VLINK_LAUNCHER_EXPORT_COMPONENT("VehicleBridge", VehicleBridgeComponent)
VLINK_LAUNCHER_EXPORT_COMPONENT("OdomFeed", OdomFeedComponent)
VLINK_LAUNCHER_EXPORT_COMPONENT("CruisePlanner", CruisePlannerComponent)
VLINK_LAUNCHER_EXPORT_COMPONENT("ParkingPlanner", ParkingPlannerComponent)
VLINK_LAUNCHER_EXPORT_COMPONENT("TelemetryTap", TelemetryTapComponent)
VLINK_LAUNCHER_MODULE_END()
