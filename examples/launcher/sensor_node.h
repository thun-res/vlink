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

#include <vlink/base/logger.h>
#include <vlink/base/timer.h>
#include <vlink/external/launcher_component.h>

#include <memory>
#include <string>

class SensorNode final : public vlink::LauncherComponent {
 public:
  bool on_start(vlink::LauncherContext& ctx) override {
    const int64_t rate_hz = ctx.get_param_int("rate_hz", 5);
    url_ = ctx.resolve_url(ctx.get_param_string("url", "dds://sensor/lidar/event/0"));
    const uint32_t period_ms = (rate_hz > 0) ? static_cast<uint32_t>(1000 / rate_hz) : 200U;

    VLOG_I("[", ctx.name(), "] Starting SensorNode (standalone=", ctx.is_standalone() ? 1 : 0, ", url=", url_,
           ", rate=", rate_hz, "Hz)");

    start_timer(ctx, period_ms);
    ctx.mark_ready("sensor streaming on " + url_);
    return true;
  }

  void on_stop(vlink::LauncherContext& ctx) override {
    if (timer_) {
      timer_->stop();
      timer_.reset();
    }

    VLOG_I("[", ctx.name(), "] SensorNode stopped.");
  }

  bool on_reload(vlink::LauncherContext& ctx) override {
    VLOG_I("[", ctx.name(), "] SensorNode reloaded.");
    ctx.report_health(true, "reloaded");
    return true;
  }

  bool on_param_changed(vlink::LauncherContext& ctx, const std::string& key,
                        const vlink::LauncherParamValue& value) override {
    if (key == "rate_hz") {
      const auto* rate = std::get_if<int64_t>(&value);

      if (rate == nullptr || *rate <= 0 || *rate > 1000) {
        return false;
      }

      start_timer(ctx, static_cast<uint32_t>(1000 / *rate));
      VLOG_I("[", ctx.name(), "] Updated rate_hz=", *rate);
    }

    return true;
  }

 private:
  void start_timer(vlink::LauncherContext& ctx, uint32_t period_ms) {
    if (timer_) {
      timer_->stop();
      timer_.reset();
    }

    timer_ = std::make_unique<vlink::Timer>(&ctx.loop(), period_ms, vlink::Timer::kInfinite, [this, &ctx]() {
      ctx.watchdog_ping();
      VLOG_D("[", ctx.name(), "] tick -> ", url_);
    });
    timer_->start();
  }

  std::string url_;
  std::unique_ptr<vlink::Timer> timer_;
};
