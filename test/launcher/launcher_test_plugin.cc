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
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

class TestHangingWatchdogComponent final : public vlink::LauncherComponent {
 public:
  bool on_configure(vlink::LauncherContext& ctx) override {
    gate_file_ = ctx.get_param_string("gate_file", "");
    hang_once_file_ = ctx.get_param_string("hang_once_file", "");
    max_block_ms_ = static_cast<int>(ctx.get_param_int("max_block_ms", 8000));
    return true;
  }

  bool on_start(vlink::LauncherContext& ctx) override {
    ctx_ = &ctx;
    ctx.watchdog_ping();
    ctx.mark_ready();

    bool should_hang = true;

    if (!hang_once_file_.empty()) {
      std::error_code ec;

      if (std::filesystem::exists(hang_once_file_, ec)) {
        should_hang = false;
      } else {
        std::ofstream ofs(hang_once_file_, std::ios::out | std::ios::trunc);
        ofs << "hung\n";
      }
    }

    if (should_hang) {
      timer_ = ctx.create_timer(
          30U,
          [this]() {
            if (triggered_.exchange(true, std::memory_order_acq_rel)) {
              return;
            }

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_block_ms_);

            while (std::chrono::steady_clock::now() < deadline) {
              if (!gate_file_.empty()) {
                std::error_code ec;

                if (std::filesystem::exists(gate_file_, ec)) {
                  break;
                }
              }

              std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
          },
          1);
      timer_->start();
    } else {
      timer_ = ctx.create_timer(
          50U,
          [this]() {
            if (ctx_ != nullptr) {
              ctx_->watchdog_ping();
            }
          },
          -1);
      timer_->start();
    }

    return true;
  }

  void on_stop() override {
    if (timer_) {
      timer_->detach();
      timer_.reset();
    }

    ctx_ = nullptr;
  }

 private:
  vlink::LauncherContext* ctx_{nullptr};
  std::unique_ptr<vlink::Timer> timer_;
  std::string gate_file_;
  std::string hang_once_file_;
  int max_block_ms_{8000};
  std::atomic<bool> triggered_{false};
};

class TestSlowReadyComponent final : public vlink::LauncherComponent {
 public:
  bool on_configure(vlink::LauncherContext& ctx) override {
    ready_file_ = ctx.get_param_string("ready_file", "");
    return true;
  }

  bool on_start(vlink::LauncherContext& ctx) override {
    ctx_ = &ctx;
    timer_ = ctx.create_timer(
        25U,
        [this]() {
          if (marked_.load(std::memory_order_acquire) || ctx_ == nullptr) {
            return;
          }

          std::error_code ec;

          if (ready_file_.empty() || std::filesystem::exists(ready_file_, ec)) {
            if (!marked_.exchange(true, std::memory_order_acq_rel)) {
              ctx_->mark_ready();
            }
          }
        },
        -1);
    timer_->start();
    return true;
  }

  void on_stop() override {
    if (timer_) {
      timer_->detach();
      timer_.reset();
    }

    ctx_ = nullptr;
  }

 private:
  vlink::LauncherContext* ctx_{nullptr};
  std::unique_ptr<vlink::Timer> timer_;
  std::string ready_file_;
  std::atomic<bool> marked_{false};
};

class TestCpuIsolationComponent final : public vlink::LauncherComponent {
 public:
  bool on_configure(vlink::LauncherContext& ctx) override {
    busy_ = ctx.get_param_bool("busy", false);
    return true;
  }

  bool on_start(vlink::LauncherContext& ctx) override {
    ctx.mark_ready();
    timer_ = ctx.create_timer(
        10U,
        [this]() {
          if (busy_) {
            volatile double acc = 1.0;
            const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(25);

            while (std::chrono::steady_clock::now() < end) {
              acc = std::sin(acc + 0.001) + 1.0;
            }

            sink_.store(acc, std::memory_order_relaxed);
          } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
          }
        },
        -1);
    timer_->start();
    return true;
  }

  void on_stop() override {
    if (timer_) {
      timer_->detach();
      timer_.reset();
    }
  }

 private:
  bool busy_{false};
  std::unique_ptr<vlink::Timer> timer_;
  std::atomic<double> sink_{0.0};
};

class TestTimeoutAndLoanComponent final : public vlink::LauncherComponent {
 public:
  bool on_start(vlink::LauncherContext& ctx) override {
    ctx_ = &ctx;
    pub_ = ctx.create_publisher<vlink::Bytes>("out", "intra://launcher/ut/loan/out");
    ctx.mark_ready();
    return pub_.valid();
  }

  void on_stop() override {
    pub_.reset();
    ctx_ = nullptr;
  }

  bool on_param_changed(std::string_view key, const vlink::LauncherParamValue& value) override {
    (void)value;

    if (key == "probe_loan") {
      const bool sup = pub_.is_support_loan();
      (void)sup;
      auto b = pub_.loan(32);

      if (!b.empty()) {
        pub_.return_loan(b);
      }

      return true;
    }

    if (key == "probe_loan_inactive") {
      auto b = pub_.loan(32);
      return b.empty();
    }

    if (key == "probe_timeout_exec" && ctx_ != nullptr) {
      timeout_exec_count_.store(0, std::memory_order_release);
      (void)ctx_->post_task([]() { std::this_thread::sleep_for(std::chrono::milliseconds(30)); });
      (void)ctx_->post_task([this]() { timeout_exec_count_.fetch_add(1, std::memory_order_release); });
      return true;
    }

    if (key == "expect_timeout_exec") {
      return timeout_exec_count_.load(std::memory_order_acquire) == 1;
    }

    return true;
  }

 private:
  vlink::LauncherContext* ctx_{nullptr};
  vlink::LauncherPublisher<vlink::Bytes> pub_;
  std::atomic<int> timeout_exec_count_{0};
};

class TestRollbackComponent final : public vlink::LauncherComponent {
 public:
  bool on_configure(vlink::LauncherContext& ctx) override {
    ctx_ = &ctx;
    marker_file_ = ctx.get_param_string("marker_file", "");
    activate_delay_ms_.store(static_cast<int>(ctx.get_param_int("activate_delay_ms", 0)), std::memory_order_release);
    deactivate_delay_ms_.store(static_cast<int>(ctx.get_param_int("deactivate_delay_ms", 0)),
                               std::memory_order_release);
    fail_activate_.store(ctx.get_param_bool("fail_activate", false), std::memory_order_release);
    throw_activate_.store(ctx.get_param_bool("throw_activate", false), std::memory_order_release);
    throw_deactivate_.store(ctx.get_param_bool("throw_deactivate", false), std::memory_order_release);
    reject_param_.store(ctx.get_param_bool("reject_param", false), std::memory_order_release);
    reject_reload_.store(ctx.get_param_bool("reject_reload", false), std::memory_order_release);
    drain_sleep_ms_.store(static_cast<int>(ctx.get_param_int("drain_sleep_ms", 0)), std::memory_order_release);

    if (ctx.get_param_bool("test_ports", false)) {
      auto bad_pub = ctx.create_publisher<vlink::Bytes>("missing_port", "");
      auto bad_sub = ctx.create_subscriber<vlink::Bytes>("missing_port", [](const vlink::Bytes&) {}, "");

      if (bad_pub.valid() || bad_sub.valid()) {
        return false;
      }

      bool bad_scheme_threw = false;

      try {
        (void)ctx.create_publisher<vlink::Bytes>("missing_port", "badscheme://invalid");
      } catch (const std::exception&) {
        bad_scheme_threw = true;
      }

      if (!bad_scheme_threw) {
        return false;
      }

      pub_ = ctx.create_publisher<vlink::Bytes>("out", "intra://launcher/ut/rollback_port");
      sub_ = ctx.create_subscriber<vlink::Bytes>(
          "in",
          [this](const vlink::Bytes& msg) {
            recv_bytes_.fetch_add(msg.size(), std::memory_order_relaxed);
            append_marker("RECV:" + std::string(reinterpret_cast<const char*>(msg.data()), msg.size()));
          },
          "intra://launcher/ut/rollback_port", false);

      if (!pub_.valid() || !sub_.valid()) {
        return false;
      }

      vlink::Bytes inactive_buf = vlink::Bytes::deep_copy(reinterpret_cast<const uint8_t*>("should_suppress"), 15U);

      if (pub_.publish(std::move(inactive_buf))) {
        return false;
      }
    }

    return true;
  }

  bool on_start(vlink::LauncherContext& ctx) override {
    const int dms = drain_sleep_ms_.load(std::memory_order_acquire);

    if (dms > 0) {
      (void)ctx.post_compute_task([this, dms] {
        std::this_thread::sleep_for(std::chrono::milliseconds(dms));
        append_marker("DRAIN_TASK_DONE");
      });
    }

    return true;
  }

  bool on_activate(vlink::LauncherContext& ctx) override {
    append_marker("ACT");
    const int dms = activate_delay_ms_.load(std::memory_order_acquire);

    if (dms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(dms));
    }

    if (throw_activate_.load(std::memory_order_acquire)) {
      throw std::runtime_error("intentional on_activate throw");
    }

    if (fail_activate_.load(std::memory_order_acquire)) {
      return false;
    }

    if (pub_.valid()) {
      vlink::Bytes payload = vlink::Bytes::deep_copy(reinterpret_cast<const uint8_t*>("zero_copy_ok"), 12U);
      const bool ok = pub_.publish(std::move(payload));

      if (!ok || !payload.empty()) {
        return false;
      }
    }

    ctx.mark_ready();
    return true;
  }

  void on_deactivate() override {
    append_marker("DEACT");
    const int dms = deactivate_delay_ms_.load(std::memory_order_acquire);

    if (dms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(dms));
    }

    if (throw_deactivate_.load(std::memory_order_acquire)) {
      throw std::runtime_error("intentional on_deactivate throw");
    }
  }

  void on_stop() override {
    sub_.reset();
    pub_.reset();
  }

  void on_cleanup() override { append_marker("CLEANUP"); }

  bool on_reload(vlink::LauncherContext&) override { return !reject_reload_.load(std::memory_order_acquire); }

  bool on_param_changed(std::string_view key, const vlink::LauncherParamValue& val) override {
    if (reject_param_.load(std::memory_order_acquire) && key != "reject_param") {
      return false;
    }

    if (key == "activate_delay_ms" && std::holds_alternative<int64_t>(val)) {
      activate_delay_ms_.store(static_cast<int>(std::get<int64_t>(val)), std::memory_order_release);
    } else if (key == "deactivate_delay_ms" && std::holds_alternative<int64_t>(val)) {
      deactivate_delay_ms_.store(static_cast<int>(std::get<int64_t>(val)), std::memory_order_release);
    } else if (key == "fail_activate" && std::holds_alternative<bool>(val)) {
      fail_activate_.store(std::get<bool>(val), std::memory_order_release);
    } else if (key == "throw_activate" && std::holds_alternative<bool>(val)) {
      throw_activate_.store(std::get<bool>(val), std::memory_order_release);
    } else if (key == "throw_deactivate" && std::holds_alternative<bool>(val)) {
      throw_deactivate_.store(std::get<bool>(val), std::memory_order_release);
    } else if (key == "reject_param" && std::holds_alternative<bool>(val)) {
      reject_param_.store(std::get<bool>(val), std::memory_order_release);
    } else if (key == "reject_reload" && std::holds_alternative<bool>(val)) {
      reject_reload_.store(std::get<bool>(val), std::memory_order_release);
    }

    return true;
  }

 private:
  void append_marker(const std::string& line) {
    if (marker_file_.empty()) {
      return;
    }

    std::lock_guard<std::mutex> lock(marker_mtx_);
    std::ofstream ofs(marker_file_, std::ios::out | std::ios::app);
    ofs << line << "\n";
  }

  vlink::LauncherContext* ctx_{nullptr};
  std::string marker_file_;
  std::mutex marker_mtx_;
  vlink::LauncherPublisher<vlink::Bytes> pub_;
  vlink::LauncherSubscriber<vlink::Bytes> sub_;
  std::atomic<size_t> recv_bytes_{0U};
  std::atomic<int> activate_delay_ms_{0};
  std::atomic<int> deactivate_delay_ms_{0};
  std::atomic<bool> fail_activate_{false};
  std::atomic<bool> throw_activate_{false};
  std::atomic<bool> throw_deactivate_{false};
  std::atomic<bool> reject_param_{false};
  std::atomic<bool> reject_reload_{false};
  std::atomic<int> drain_sleep_ms_{0};
};

class TestTimerEdgeComponent final : public vlink::LauncherTimerComponent {
 public:
  TestTimerEdgeComponent() : vlink::LauncherTimerComponent(40U) {}

  bool on_timer_configure(vlink::LauncherContext& ctx) override {
    fail_tick_.store(ctx.get_param_bool("fail_tick", false), std::memory_order_release);
    return true;
  }

  bool on_tick(vlink::LauncherContext&) override {
    ticks_.fetch_add(1, std::memory_order_relaxed);
    return !fail_tick_.load(std::memory_order_acquire);
  }

  bool on_timer_param_changed(std::string_view key, const vlink::LauncherParamValue& val) override {
    if (key == "fail_tick" && std::holds_alternative<bool>(val)) {
      fail_tick_.store(std::get<bool>(val), std::memory_order_release);
    }

    return true;
  }

 private:
  std::atomic<bool> fail_tick_{false};
  std::atomic<int> ticks_{0};
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

class TestCoroEdgeComponent final : public vlink::LauncherCoroutineComponent {
 public:
  bool on_coro_start(vlink::LauncherContext& ctx) override {
    marker_file_ = ctx.get_param_string("marker_file", "");
    throw_run_ = ctx.get_param_bool("throw_run", false);
    return !ctx.get_param_bool("fail_start", false);
  }

  vlink::Co::Task<void> on_coro_run(vlink::LauncherContext& ctx) override {
    ctx.mark_ready();
    int val = 0;
    co_await ctx.co_exec(vlink::Schedule::Config{}, [&val]() { val = 42; });

    if (val == 42) {
      append_marker("EXEC_42");
    }

    if (throw_run_) {
      throw std::runtime_error("intentional coro throw");
    }

    const bool slept = co_await ctx.co_sleep(5000U);

    if (!slept) {
      append_marker("SLEEP_CANCELLED");
    }
  }

  void on_coro_stop() override { append_marker("CORO_STOPPED"); }

 private:
  void append_marker(const std::string& line) {
    if (marker_file_.empty()) {
      return;
    }

    std::ofstream ofs(marker_file_, std::ios::out | std::ios::app);
    ofs << line << "\n";
  }

  std::string marker_file_;
  bool throw_run_{false};
};
#endif

VLINK_LAUNCHER_MODULE_BEGIN(1, 0)
VLINK_LAUNCHER_EXPORT_COMPONENT("TestTalker", TestTalkerComponent)
VLINK_LAUNCHER_EXPORT_COMPONENT("TestListener", TestListenerComponent)
VLINK_LAUNCHER_EXPORT_COMPONENT("TestFailOnStart", TestFailOnStartComponent)
VLINK_LAUNCHER_EXPORT_COMPONENT("TestManagedPort", TestManagedPortComponent)
VLINK_LAUNCHER_EXPORT_COMPONENT("TestHangingWatchdog", TestHangingWatchdogComponent)
VLINK_LAUNCHER_EXPORT_COMPONENT("TestSlowReady", TestSlowReadyComponent)
VLINK_LAUNCHER_EXPORT_COMPONENT("TestCpuIsolation", TestCpuIsolationComponent)
VLINK_LAUNCHER_EXPORT_COMPONENT("TestTimeoutAndLoan", TestTimeoutAndLoanComponent)
VLINK_LAUNCHER_EXPORT_COMPONENT("TestRollback", TestRollbackComponent)
VLINK_LAUNCHER_EXPORT_COMPONENT("TestTimerEdge", TestTimerEdgeComponent)
#ifdef VLINK_ENABLE_COROUTINE
VLINK_LAUNCHER_EXPORT_COMPONENT("TestCoro", TestCoroComponent)
VLINK_LAUNCHER_EXPORT_COMPONENT("TestCoroEdge", TestCoroEdgeComponent)
#endif
VLINK_LAUNCHER_MODULE_END()

// NOLINTEND
