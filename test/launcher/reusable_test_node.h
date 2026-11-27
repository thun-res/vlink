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

// NOLINTBEGIN

#include <vlink/base/timer.h>
#include <vlink/base/utils.h>
#include <vlink/external/launcher_component.h>

#include <fstream>
#include <memory>
#include <string>

class ReusableTestNode final : public vlink::LauncherComponent {
 public:
  bool on_start(vlink::LauncherContext& ctx) override {
    if (ctx.get_param_bool("fail_start", false)) {
      return false;
    }

    marker_file_ = ctx.get_param_string("marker_file", "");
    order_file_ = ctx.get_param_string("order_file", "");
    const int64_t rate = ctx.get_param_int("rate_hz", 10);
    const std::string label = ctx.get_param_string("label", "default");
    const bool hang_watchdog = ctx.get_param_bool("hang_watchdog", false);
    const bool never_ready = ctx.get_param_bool("never_ready", false);
    const int64_t self_stop_ms = ctx.get_param_int("self_stop_ms", 0);

    append_to(marker_file_, "START:" + ctx.name() + ":ns=" + ctx.ns() +
                                ":standalone=" + (ctx.is_standalone() ? "1" : "0") + ":rate=" + std::to_string(rate) +
                                ":label=" + label + ":wd=" + std::to_string(ctx.watchdog_interval_ms()) +
                                ":stopping=" + (ctx.is_stopping() ? "1" : "0") + ":pid=" + vlink::Utils::get_pid_str() +
                                ":qtype=" + std::to_string(static_cast<int>(ctx.queue_type())) +
                                ":qstrat=" + std::to_string(static_cast<int>(ctx.queue_strategy())) +
                                ":threads=" + std::to_string(ctx.worker_threads()));
    append_to(order_file_, "START:" + ctx.name());

    const std::string probe_url = ctx.get_param_string("probe_url", "");

    if (!probe_url.empty()) {
      append_to(marker_file_, "URL:" + ctx.resolve_url(probe_url));
    }

    const std::string large_blob = ctx.get_param_string("large_blob", "");

    if (!large_blob.empty()) {
      append_to(marker_file_, "LARGE_LEN:" + std::to_string(large_blob.size()));
    }

    if (ctx.get_param_bool("verify_all_params", false)) {
      bool ok = true;
      ok = ok && ctx.has_param("p_bool") && ctx.get_param_bool("p_bool", false);
      ok = ok && ctx.has_param("p_int") && ctx.get_param_int("p_int", 0) == -42;
      ok = ok && ctx.has_param("p_double") &&
           (ctx.get_param_double("p_double", 0.0) > 3.1 && ctx.get_param_double("p_double", 0.0) < 3.2);
      ok = ok && ctx.has_param("p_str") && ctx.get_param_string("p_str", "") == "hello";
      ok = ok && !ctx.has_param("p_missing") && !ctx.get_param("p_missing").has_value();
      const auto bl = ctx.get_param("p_bool_list");
      const auto il = ctx.get_param("p_int_list");
      const auto dl = ctx.get_param("p_double_list");
      const auto sl = ctx.get_param("p_str_list");
      ok = ok && bl.has_value() && std::holds_alternative<std::vector<bool>>(*bl) &&
           std::get<std::vector<bool>>(*bl) == std::vector<bool>{true, false};
      ok = ok && il.has_value() && std::holds_alternative<std::vector<int64_t>>(*il) &&
           std::get<std::vector<int64_t>>(*il) == std::vector<int64_t>{1, -2, 3};
      ok = ok && dl.has_value() && std::holds_alternative<std::vector<double>>(*dl) &&
           std::get<std::vector<double>>(*dl).size() == 2U;
      ok = ok && sl.has_value() && std::holds_alternative<std::vector<std::string>>(*sl) &&
           std::get<std::vector<std::string>>(*sl) == std::vector<std::string>{"x", "y"};
      ok = ok && ctx.params().size() >= 8U;
      append_to(marker_file_, ok ? "ALL_PARAMS_OK" : "ALL_PARAMS_FAIL");
    }

    if (!hang_watchdog) {
      wd_timer_ =
          std::make_unique<vlink::Timer>(&ctx.loop(), 150, vlink::Timer::kInfinite, [&ctx]() { ctx.watchdog_ping(); });
      wd_timer_->start();
    }

    if (self_stop_ms > 0) {
      self_stop_timer_ = std::make_unique<vlink::Timer>(&ctx.loop(), static_cast<uint64_t>(self_stop_ms), 1,
                                                        [&ctx]() { ctx.request_stop(); });
      self_stop_timer_->start();
    }

    if (!never_ready) {
      ctx.mark_ready("reusable ready:" + label);
    }

    return true;
  }

  void on_stop(vlink::LauncherContext& ctx) override {
    if (self_stop_timer_) {
      self_stop_timer_->stop();
      self_stop_timer_.reset();
    }

    if (wd_timer_) {
      wd_timer_->stop();
      wd_timer_.reset();
    }

    append_to(marker_file_, std::string("STOPPED:stopping=") + (ctx.is_stopping() ? "1" : "0"));
    append_to(order_file_, "STOP:" + ctx.name());
  }

  bool on_reload(vlink::LauncherContext& ctx) override {
    append_to(marker_file_, "RELOADED");
    ctx.report_health(true, "reloaded");
    return true;
  }

  bool on_param_changed(vlink::LauncherContext& ctx, const std::string& key,
                        const vlink::LauncherParamValue& value) override {
    (void)ctx;
    (void)value;

    if (key == "reject_me") {
      return false;
    }

    append_to(marker_file_, "PARAM:" + key);
    return true;
  }

 private:
  static void append_to(const std::string& path, const std::string& line) {
    if (path.empty()) {
      return;
    }

    std::ofstream ofs(path, std::ios::app);
    ofs << line << "\n";
  }

  std::string marker_file_;
  std::string order_file_;
  std::unique_ptr<vlink::Timer> wd_timer_;
  std::unique_ptr<vlink::Timer> self_stop_timer_;
};

class MinimalTestNode final : public vlink::LauncherComponent {
 public:
  bool on_start(vlink::LauncherContext& ctx) override {
    ctx.mark_ready("minimal ready");
    return true;
  }

  void on_stop(vlink::LauncherContext& ctx) override { (void)ctx; }
};

class TimerTestNode final : public vlink::LauncherTimerComponent {
 public:
  TimerTestNode() : vlink::LauncherTimerComponent(40U, true, true) {}

 protected:
  bool on_timer_init(vlink::LauncherContext& ctx) override {
    marker_file_ = ctx.get_param_string("marker_file", "");
    ticks_.store(0U, std::memory_order_release);
    return true;
  }

  void on_tick(vlink::LauncherContext& ctx) override {
    const uint32_t n = ticks_.fetch_add(1U, std::memory_order_acq_rel) + 1U;

    if (!marker_file_.empty()) {
      std::ofstream ofs(marker_file_, std::ios::app);
      ofs << "TICK:" << n << ":period=" << period_ms() << ":standalone=" << (ctx.is_standalone() ? "1" : "0") << "\n";
    }
  }

  void on_timer_stop(vlink::LauncherContext& ctx) override {
    (void)ctx;

    if (!marker_file_.empty()) {
      std::ofstream ofs(marker_file_, std::ios::app);
      ofs << "TIMER_STOP\n";
    }
  }

 private:
  std::string marker_file_;
  std::atomic<uint32_t> ticks_{0};
};

#ifdef VLINK_ENABLE_COROUTINE
class CoroTestNode final : public vlink::LauncherCoroutineComponent {
 public:
  CoroTestNode() : vlink::LauncherCoroutineComponent(true, 3000U) {}

 protected:
  bool on_coro_init(vlink::LauncherContext& ctx) override {
    marker_file_ = ctx.get_param_string("marker_file", "");
    return true;
  }

  vlink::Co::Task<void> run(vlink::LauncherContext& ctx) override {
    auto token = ctx.cancellation_token();
    uint32_t step = 0U;

    while (!token.is_cancellation_requested() && !ctx.is_stopping()) {
      ++step;
      ctx.watchdog_ping();

      if (!marker_file_.empty()) {
        std::ofstream ofs(marker_file_, std::ios::app);
        ofs << "CORO_STEP:" << step << ":standalone=" << (ctx.is_standalone() ? "1" : "0") << "\n";
      }

      co_await vlink::Co::delay_ms(ctx.loop(), 30U);
    }

    if (!marker_file_.empty()) {
      std::ofstream ofs(marker_file_, std::ios::app);
      ofs << "CORO_DONE:" << step << "\n";
    }
  }

 private:
  std::string marker_file_;
};
#endif

// NOLINTEND
