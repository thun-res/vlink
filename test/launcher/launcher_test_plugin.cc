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

// NOLINTBEGIN

#include <vlink/external/launcher_component.h>
#include <vlink/publisher.h>
#include <vlink/subscriber.h>
//
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

class TestTalkerComponent final : public vlink::LauncherTimerComponent {
 public:
  TestTalkerComponent() : vlink::LauncherTimerComponent(40U) {}

  bool on_timer_configure(vlink::LauncherContext& ctx) override {
    topic_ = ctx.get_output_url("chatter", ctx.get_param_string("topic", "intra://launcher/test/chatter"));
    payload_prefix_ = ctx.get_param_string("prefix", "ping");
    return true;
  }

  bool on_timer_start(vlink::LauncherContext& ctx) override {
    pub_ = std::make_unique<vlink::Publisher<std::string>>(topic_);
    (void)ctx.post_compute_task([this] { compute_runs_.fetch_add(1, std::memory_order_relaxed); });
    return true;
  }

  bool on_tick(vlink::LauncherContext& ctx) override {
    (void)ctx;

    if (pub_) {
      const uint64_t seq = seq_.fetch_add(1U, std::memory_order_relaxed) + 1U;
      (void)pub_->publish(payload_prefix_ + "#" + std::to_string(seq));
    }

    return true;
  }

  bool on_reload(vlink::LauncherContext& ctx) override {
    reload_count_.fetch_add(1, std::memory_order_relaxed);
    payload_prefix_ = ctx.get_param_string("prefix", payload_prefix_);
    return true;
  }

  void on_timer_stop() override { pub_.reset(); }

  bool on_timer_param_changed(std::string_view key, const vlink::LauncherParamValue& value) override {
    if (key == "prefix") {
      const auto* s = std::get_if<std::string>(&value);

      if (s == nullptr) {
        return false;
      }

      payload_prefix_ = *s;
      return true;
    }

    if (key == "reject_me") {
      return false;
    }

    return true;
  }

 private:
  std::string topic_;
  std::string payload_prefix_;
  std::unique_ptr<vlink::Publisher<std::string>> pub_;
  std::atomic<uint64_t> seq_{0};
  std::atomic<int> compute_runs_{0};
  std::atomic<int> reload_count_{0};
};

class TestListenerComponent final : public vlink::LauncherComponent {
 public:
  bool on_configure(vlink::LauncherContext& ctx) override {
    topic_ = ctx.get_input_url("chatter", ctx.get_param_string("topic", "intra://launcher/test/chatter"));
    tags_ = ctx.get_param_string_list("tags");
    return true;
  }

  bool on_start(vlink::LauncherContext& ctx) override {
    sub_ = std::make_unique<vlink::Subscriber<std::string>>(topic_);
    sub_->listen([this](const std::string&) { recv_count_.fetch_add(1U, std::memory_order_relaxed); });
    ctx.mark_ready();
    return true;
  }

  void on_stop() override { sub_.reset(); }

 private:
  std::string topic_;
  std::vector<std::string> tags_;
  std::unique_ptr<vlink::Subscriber<std::string>> sub_;
  std::atomic<uint64_t> recv_count_{0};
};

class TestFailOnStartComponent final : public vlink::LauncherComponent {
 public:
  bool on_start(vlink::LauncherContext& ctx) override {
    (void)ctx;
    return false;
  }

  void on_stop() override {}

  void on_cleanup() override {
    const int calls = cleanup_calls_.fetch_add(1, std::memory_order_acq_rel) + 1;

    if (calls > 1) {
      std::abort();
    }
  }

 private:
  std::atomic<int> cleanup_calls_{0};
};

class TestManagedPortComponent final : public vlink::LauncherComponent {
 public:
  bool on_start(vlink::LauncherContext& ctx) override {
    ctx_ = &ctx;
    pub_ = ctx.create_publisher<std::string>("out", "intra://launcher/ut/managed/out");
    sub_ = ctx.create_subscriber<std::string>(
        "in",
        [this](const std::string& msg) {
          recv_count_.fetch_add(1U, std::memory_order_relaxed);

          if (pub_.valid()) {
            (void)pub_.publish("echo:" + msg);
          }
        },
        "intra://launcher/ut/managed/in");
    return pub_.valid() && sub_.valid();
  }

  bool on_activate(vlink::LauncherContext& ctx) override {
    if (pub_.valid()) {
      (void)pub_.publish("activated");
    }

    ctx.mark_ready();
    return true;
  }

  void on_stop() override {
    sub_.reset();
    pub_.reset();
    ctx_ = nullptr;
  }

  bool on_param_changed(std::string_view key, const vlink::LauncherParamValue& value) override {
    (void)value;

    if (key == "probe_throw_task" && ctx_ != nullptr) {
      auto handle = ctx_->post_compute_task([]() { throw std::runtime_error("intentional task failure"); });
      (void)handle.wait();
      return handle.state() == vlink::TaskExecutionState::kFailed;
    }

    if (key == "probe_param_helpers" && ctx_ != nullptr) {
      const bool b1 = ctx_->get_param_bool("p_bool", false);
      const bool b2 = ctx_->get_param_bool("p_bool_str", false);
      const bool b3 = ctx_->get_param_bool("p_bool_false_str", true);
      const int64_t i1 = ctx_->get_param_int("p_int", 0);
      const int64_t i2 = ctx_->get_param_int("p_int_str", 0);
      const double d1 = ctx_->get_param_double("p_double", 0.0);
      const double d2 = ctx_->get_param_double("p_int", 0.0);
      const double d3 = ctx_->get_param_double("p_double_str", 0.0);
      const auto bl = ctx_->get_param_bool_list("p_bool_list");
      const auto il = ctx_->get_param_int_list("p_int_list");
      const auto dl = ctx_->get_param_double_list("p_double_list");
      const auto dl_from_i = ctx_->get_param_double_list("p_int_list");
      const auto sl = ctx_->get_param_string_list("p_str_list");
      vlink::Schedule::Config sc;
      sc.delay_ms = 5U;
      auto st = ctx_->exec_task(sc, []() { return true; });
      (void)st;
      return b1 && b2 && !b3 && i1 == 42 && i2 == 99 && d1 > 1.4 && d2 > 41.0 && d3 > 2.4 && bl.size() == 2U &&
             il.size() == 2U && dl.size() == 2U && dl_from_i.size() == 2U && sl.size() == 2U;
    }

    if (key == "probe_publish" && pub_.valid()) {
      return pub_.publish("manual_probe");
    }

    if (key == "probe_force_publish" && pub_.valid()) {
      const auto* s = std::get_if<std::string>(&value);
      return pub_.publish(s != nullptr ? *s : std::string("forced_probe"), true);
    }

    if (key == "expect_recv_count") {
      const auto* expected = std::get_if<int64_t>(&value);
      return expected != nullptr && recv_count_.load(std::memory_order_acquire) == static_cast<uint64_t>(*expected);
    }

    return true;
  }

 private:
  vlink::LauncherContext* ctx_{nullptr};
  vlink::LauncherPublisher<std::string> pub_;
  vlink::LauncherSubscriber<std::string> sub_;
  std::atomic<uint64_t> recv_count_{0};
};

#ifdef VLINK_ENABLE_COROUTINE
class TestCoroComponent final : public vlink::LauncherCoroutineComponent {
 public:
  vlink::Co::Task<void> on_coro_run(vlink::LauncherContext& ctx) override {
    ctx.mark_ready();
    (void)ctx.co_spawn([](vlink::LauncherContext& c) -> vlink::Co::Task<void> { co_await c.co_await_stop(); }(ctx));

    while (co_await ctx.co_sleep(30U)) {
      ctx.watchdog_ping();
    }
  }
};
#endif

VLINK_LAUNCHER_MODULE_BEGIN(1, 0)
VLINK_LAUNCHER_EXPORT_COMPONENT("TestTalker", TestTalkerComponent)
VLINK_LAUNCHER_EXPORT_COMPONENT("TestListener", TestListenerComponent)
VLINK_LAUNCHER_EXPORT_COMPONENT("TestFailOnStart", TestFailOnStartComponent)
VLINK_LAUNCHER_EXPORT_COMPONENT("TestManagedPort", TestManagedPortComponent)
#ifdef VLINK_ENABLE_COROUTINE
VLINK_LAUNCHER_EXPORT_COMPONENT("TestCoro", TestCoroComponent)
#endif
VLINK_LAUNCHER_MODULE_END()

// NOLINTEND
