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

#include <vlink/base/condition_variable.h>
#include <vlink/base/helpers.h>
#include <vlink/base/logger.h>
#include <vlink/base/macros.h>
#include <vlink/base/multi_loop.h>
#include <vlink/base/timer.h>
#include <vlink/base/utils.h>
#include <vlink/external/launcher_component.h>
#include <vlink/external/launcher_notify.h>
//
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vlink {

static_assert(std::atomic_bool::is_always_lock_free, "signal flags must be lock-free");

static std::atomic_bool g_standalone_stop_requested{false};
static std::atomic_bool g_standalone_reload_requested{false};

#if defined(_WIN32) || defined(_WIN64)
static BOOL WINAPI standalone_console_ctrl_handler(DWORD ctrl_type) noexcept {
  switch (ctrl_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
      g_standalone_stop_requested.store(true, std::memory_order_release);
      return TRUE;
    default:
      return FALSE;
  }
}

class SignalHandlerGuard final {
 public:
  SignalHandlerGuard() noexcept {
    installed_ = (::SetConsoleCtrlHandler(standalone_console_ctrl_handler, TRUE) != FALSE);
  }
  ~SignalHandlerGuard() noexcept {
    if (installed_) {
      (void)::SetConsoleCtrlHandler(standalone_console_ctrl_handler, FALSE);
    }
  }

  SignalHandlerGuard(const SignalHandlerGuard&) = delete;
  SignalHandlerGuard& operator=(const SignalHandlerGuard&) = delete;

 private:
  bool installed_{false};
};
#else
static void standalone_signal_handler(int sig) noexcept {
  if (sig == SIGHUP) {
    g_standalone_reload_requested.store(true, std::memory_order_release);
    return;
  }

  g_standalone_stop_requested.store(true, std::memory_order_release);
}

class SignalHandlerGuard final {
 public:
  SignalHandlerGuard() noexcept {
    struct sigaction sa{};
    sa.sa_handler = standalone_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    saved_int_ = (::sigaction(SIGINT, &sa, &prev_int_) == 0);
    saved_term_ = (::sigaction(SIGTERM, &sa, &prev_term_) == 0);
    saved_hup_ = (::sigaction(SIGHUP, &sa, &prev_hup_) == 0);
  }

  ~SignalHandlerGuard() noexcept {
    if (saved_int_) {
      (void)::sigaction(SIGINT, &prev_int_, nullptr);
    }

    if (saved_term_) {
      (void)::sigaction(SIGTERM, &prev_term_, nullptr);
    }

    if (saved_hup_) {
      (void)::sigaction(SIGHUP, &prev_hup_, nullptr);
    }
  }

  SignalHandlerGuard(const SignalHandlerGuard&) = delete;
  SignalHandlerGuard& operator=(const SignalHandlerGuard&) = delete;

 private:
  struct sigaction prev_int_{};
  struct sigaction prev_term_{};
  struct sigaction prev_hup_{};
  bool saved_int_{false};
  bool saved_term_{false};
  bool saved_hup_{false};
};
#endif

static bool parse_int64_str(std::string_view text, int64_t& out) noexcept {
  if VUNLIKELY (text.empty()) {
    return false;
  }

  int64_t val = 0;
  const auto res = std::from_chars(text.data(), text.data() + text.size(), val, 10);

  if (res.ec != std::errc() || res.ptr != text.data() + text.size()) {
    return false;
  }

  out = val;
  return true;
}

static bool parse_double_str(const std::string& text, double& out) noexcept {
  if VUNLIKELY (text.empty()) {
    return false;
  }

  if (std::isspace(static_cast<unsigned char>(text.front())) != 0 ||
      std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    return false;
  }

  size_t num_start = 0U;

  if (text[0] == '+' || text[0] == '-') {
    num_start = 1U;
  }

  if (num_start + 1U < text.size() && text[num_start] == '0' &&
      (text[num_start + 1U] == 'x' || text[num_start + 1U] == 'X')) {
    return false;
  }

  char* end = nullptr;
  const double val = std::strtod(text.c_str(), &end);

  if (end != text.c_str() + text.size() || !std::isfinite(val)) {
    return false;
  }

  out = val;
  return true;
}

static LauncherParamValue parse_cli_param_value(const std::string& raw) {
  if (raw == "true" || raw == "TRUE") {
    return true;
  }

  if (raw == "false" || raw == "FALSE") {
    return false;
  }

  int64_t iv = 0;

  if (parse_int64_str(raw, iv)) {
    return iv;
  }

  double dv = 0.0;

  if (parse_double_str(raw, dv)) {
    return dv;
  }

  return raw;
}

static bool json_fits_int64(const nlohmann::json& val) noexcept {
  if (val.is_number_unsigned()) {
    return val.get<uint64_t>() <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  }

  return val.is_number_integer();
}

static void load_params_from_json(const std::string& raw_json, LauncherParamMap& out) {
  if (raw_json.empty()) {
    return;
  }

  try {
    const auto doc = nlohmann::json::parse(raw_json);

    if (!doc.is_object()) {
      return;
    }

    for (auto it = doc.begin(); it != doc.end(); ++it) {
      const auto& val = it.value();

      if (val.is_boolean()) {
        out[it.key()] = val.get<bool>();
      } else if (json_fits_int64(val)) {
        out[it.key()] = val.get<int64_t>();
      } else if (val.is_number()) {
        const double dv = val.get<double>();

        if (std::isfinite(dv)) {
          out[it.key()] = dv;
        }
      } else if (val.is_string()) {
        out[it.key()] = val.get<std::string>();
      } else if (val.is_array()) {
        if (val.empty()) {
          out[it.key()] = std::vector<std::string>{};
        } else {
          bool all_bool = true;
          bool all_int = true;
          bool all_num = true;
          bool all_str = true;

          for (const auto& elem : val) {
            if (!elem.is_boolean()) {
              all_bool = false;
            }

            if (!json_fits_int64(elem)) {
              all_int = false;
            }

            if (!elem.is_number() || !std::isfinite(elem.get<double>())) {
              all_num = false;
            }

            if (!elem.is_string()) {
              all_str = false;
            }
          }

          if (all_bool) {
            std::vector<bool> arr;
            arr.reserve(val.size());

            for (const auto& elem : val) {
              arr.push_back(elem.get<bool>());
            }

            out[it.key()] = std::move(arr);
          } else if (all_int) {
            std::vector<int64_t> arr;
            arr.reserve(val.size());

            for (const auto& elem : val) {
              arr.push_back(elem.get<int64_t>());
            }

            out[it.key()] = std::move(arr);
          } else if (all_num) {
            std::vector<double> arr;
            arr.reserve(val.size());

            for (const auto& elem : val) {
              arr.push_back(elem.get<double>());
            }

            out[it.key()] = std::move(arr);
          } else if (all_str) {
            std::vector<std::string> arr;
            arr.reserve(val.size());

            for (const auto& elem : val) {
              arr.push_back(elem.get<std::string>());
            }

            out[it.key()] = std::move(arr);
          }
        }
      }
    }
  } catch (const std::exception& e) {
    VLOG_W("LauncherComponent: Failed to parse VLINK_LAUNCHER_PARAMS_JSON: ", e.what());
  }
}

static void load_remaps_from_json(const std::string& raw_json, std::unordered_map<std::string, std::string>& out) {
  if (raw_json.empty()) {
    return;
  }

  try {
    const auto doc = nlohmann::json::parse(raw_json);

    if (!doc.is_object()) {
      return;
    }

    for (auto it = doc.begin(); it != doc.end(); ++it) {
      if (it.value().is_string()) {
        out[it.key()] = it.value().get<std::string>();
      }
    }
  } catch (const std::exception& e) {
    VLOG_W("LauncherComponent: Failed to parse VLINK_LAUNCHER_REMAPS_JSON: ", e.what());
  }
}

static bool post_and_wait(MessageLoop& loop, MoveFunction<void()>&& fn) {
  struct WaitState final {
    std::mutex mtx;
    ConditionVariable cv;
    bool finished{false};
    bool executed{false};
  };

  struct FinishGuard final {
    explicit FinishGuard(std::shared_ptr<WaitState> s) : state(std::move(s)) {}
    FinishGuard(const FinishGuard&) = delete;
    FinishGuard& operator=(const FinishGuard&) = delete;
    ~FinishGuard() {
      if (state) {
        {
          std::lock_guard lock(state->mtx);
          state->finished = true;
        }

        state->cv.notify_all();
      }
    }
    std::shared_ptr<WaitState> state;
  };

  auto state = std::make_shared<WaitState>();
  auto guard = std::make_shared<FinishGuard>(state);

  auto task_cb = [state, guard = std::move(guard), task = std::move(fn)]() mutable {
    task();
    {
      std::lock_guard lock(state->mtx);
      state->executed = true;
      state->finished = true;
    }

    state->cv.notify_all();
    guard->state.reset();
  };

  PostTaskOptions opts;
  opts.drop_policy = TaskDropPolicy::kProtected;
  const TaskHandle handle =
      (loop.get_type() == MessageLoop::kPriorityType)
          ? loop.post_task_with_priority_handle(std::move(task_cb), MessageLoop::kHighestPriority, opts)
          : loop.post_task_handle(std::move(task_cb), opts);
  const auto st = handle.state();

  if VUNLIKELY (st == TaskExecutionState::kInvalid || st == TaskExecutionState::kRejected) {
    return false;
  }

  std::unique_lock lock(state->mtx);
  state->cv.wait(lock, [&state]() { return state->finished; });
  return state->executed;
}

struct LauncherContext::Impl final {
  explicit Impl(Hooks&& h)
      : name(std::move(h.name)),
        ns(std::move(h.ns)),
        loop(h.loop),
        params(std::move(h.params)),
        remaps(std::move(h.remaps)),
        watchdog_ms(h.watchdog_ms),
        worker_threads(h.worker_threads == 0U ? 1U : h.worker_threads),
        standalone(h.standalone),
        on_ready(std::move(h.on_ready)),
        on_health(std::move(h.on_health)),
        on_watchdog(std::move(h.on_watchdog)),
        on_request_stop(std::move(h.on_request_stop)) {}

  std::string name;
  std::string ns;
  MessageLoop* loop{nullptr};
  mutable std::mutex param_mtx;
  LauncherParamMap params;
  std::unordered_map<std::string, std::string> remaps;
  uint32_t watchdog_ms{0};
  uint32_t worker_threads{1};
  bool standalone{false};
  std::atomic_bool stopping{false};
  CancellationSource cancel_source;
  MoveFunction<void(std::string_view)> on_ready;
  MoveFunction<void(bool, std::string_view)> on_health;
  MoveFunction<void()> on_watchdog;
  MoveFunction<void()> on_request_stop;
};

LauncherContext::LauncherContext(Hooks&& hooks) : impl_(std::make_unique<Impl>(std::move(hooks))) {}

LauncherContext::~LauncherContext() = default;

const std::string& LauncherContext::name() const noexcept { return impl_->name; }

const std::string& LauncherContext::ns() const noexcept { return impl_->ns; }

MessageLoop& LauncherContext::loop() const noexcept { return *impl_->loop; }

bool LauncherContext::is_stopping() const noexcept { return impl_->stopping.load(std::memory_order_acquire); }

bool LauncherContext::is_standalone() const noexcept { return impl_->standalone; }

uint32_t LauncherContext::watchdog_interval_ms() const noexcept { return impl_->watchdog_ms; }

MessageLoop::Type LauncherContext::queue_type() const noexcept {
  return (impl_->loop != nullptr) ? impl_->loop->get_type() : MessageLoop::kNormalType;
}

MessageLoop::Strategy LauncherContext::queue_strategy() const noexcept {
  return (impl_->loop != nullptr) ? impl_->loop->get_strategy() : MessageLoop::kOptimizationStrategy;
}

uint32_t LauncherContext::worker_threads() const noexcept { return impl_->worker_threads; }

CancellationToken LauncherContext::cancellation_token() const { return impl_->cancel_source.token(); }

bool LauncherContext::has_param(const std::string& key) const {
  std::lock_guard lock(impl_->param_mtx);
  return impl_->params.count(key) != 0U;
}

std::optional<LauncherParamValue> LauncherContext::get_param(const std::string& key) const {
  std::lock_guard lock(impl_->param_mtx);
  const auto it = impl_->params.find(key);

  if (it == impl_->params.end()) {
    return std::nullopt;
  }

  return it->second;
}

bool LauncherContext::get_param_bool(const std::string& key, bool default_val) const {
  std::lock_guard lock(impl_->param_mtx);
  auto it = impl_->params.find(key);

  if (it == impl_->params.end()) {
    return default_val;
  }

  const auto* b = std::get_if<bool>(&it->second);

  if (b != nullptr) {
    return *b;
  }

  const auto* i = std::get_if<int64_t>(&it->second);

  if (i != nullptr) {
    return *i != 0;
  }

  const auto* s = std::get_if<std::string>(&it->second);

  if (s != nullptr) {
    if (*s == "true" || *s == "TRUE" || *s == "1" || *s == "yes") {
      return true;
    }

    if (*s == "false" || *s == "FALSE" || *s == "0" || *s == "no") {
      return false;
    }
  }

  return default_val;
}

int64_t LauncherContext::get_param_int(const std::string& key, int64_t default_val) const {
  std::lock_guard lock(impl_->param_mtx);
  auto it = impl_->params.find(key);

  if (it == impl_->params.end()) {
    return default_val;
  }

  const auto* i = std::get_if<int64_t>(&it->second);

  if (i != nullptr) {
    return *i;
  }

  const auto* b = std::get_if<bool>(&it->second);

  if (b != nullptr) {
    return *b ? 1 : 0;
  }

  const auto* d = std::get_if<double>(&it->second);

  if (d != nullptr && std::isfinite(*d) && *d >= -9223372036854775808.0 && *d < 9223372036854775808.0) {
    return static_cast<int64_t>(*d);
  }

  const auto* s = std::get_if<std::string>(&it->second);

  if (s != nullptr) {
    int64_t parsed = 0;

    if (parse_int64_str(*s, parsed)) {
      return parsed;
    }
  }

  return default_val;
}

double LauncherContext::get_param_double(const std::string& key, double default_val) const {
  std::lock_guard lock(impl_->param_mtx);
  auto it = impl_->params.find(key);

  if (it == impl_->params.end()) {
    return default_val;
  }

  const auto* d = std::get_if<double>(&it->second);

  if (d != nullptr) {
    return *d;
  }

  const auto* i = std::get_if<int64_t>(&it->second);

  if (i != nullptr) {
    return static_cast<double>(*i);
  }

  const auto* b = std::get_if<bool>(&it->second);

  if (b != nullptr) {
    return *b ? 1.0 : 0.0;
  }

  const auto* s = std::get_if<std::string>(&it->second);

  if (s != nullptr) {
    double parsed = 0.0;

    if (parse_double_str(*s, parsed)) {
      return parsed;
    }
  }

  return default_val;
}

std::string LauncherContext::get_param_string(const std::string& key, const std::string& default_val) const {
  std::lock_guard lock(impl_->param_mtx);
  auto it = impl_->params.find(key);

  if (it == impl_->params.end()) {
    return default_val;
  }

  const auto* s = std::get_if<std::string>(&it->second);

  if (s != nullptr) {
    return *s;
  }

  const auto* b = std::get_if<bool>(&it->second);

  if (b != nullptr) {
    return *b ? "true" : "false";
  }

  const auto* i = std::get_if<int64_t>(&it->second);

  if (i != nullptr) {
    return std::to_string(*i);
  }

  const auto* d = std::get_if<double>(&it->second);

  if (d != nullptr) {
    return std::to_string(*d);
  }

  return default_val;
}

LauncherParamMap LauncherContext::params() const {
  std::lock_guard lock(impl_->param_mtx);
  return impl_->params;
}

const std::unordered_map<std::string, std::string>& LauncherContext::remaps() const noexcept { return impl_->remaps; }

std::string LauncherContext::resolve_url(std::string_view url, std::string_view inproc_scheme) const {
  if VUNLIKELY (url.empty()) {
    return {};
  }

  std::string key(url);
  const auto it = impl_->remaps.find(key);

  if (it != impl_->remaps.end()) {
    return it->second;
  }

  if (!impl_->standalone && !inproc_scheme.empty()) {
    while (inproc_scheme.size() >= 3U && inproc_scheme.substr(inproc_scheme.size() - 3U) == "://") {
      inproc_scheme.remove_suffix(3U);
    }

    const size_t sep = url.find("://");

    if (!inproc_scheme.empty() && sep != std::string_view::npos && sep > 0U) {
      if (url.substr(0U, sep) == inproc_scheme) {
        return key;
      }

      std::string out;
      out.reserve(inproc_scheme.size() + url.size() - sep);
      out.append(inproc_scheme);
      out.append(url.substr(sep));
      return out;
    }
  }

  return key;
}

void LauncherContext::mark_ready(std::string_view detail) {
  if (impl_->on_ready) {
    impl_->on_ready(detail);
  }
}

void LauncherContext::report_health(bool healthy, std::string_view detail) {
  if (impl_->on_health) {
    impl_->on_health(healthy, detail);
  }
}

void LauncherContext::watchdog_ping() {
  if (impl_->on_watchdog) {
    impl_->on_watchdog();
  }
}

void LauncherContext::request_stop() {
  impl_->stopping.store(true, std::memory_order_release);
  (void)impl_->cancel_source.request_cancel();

  if (impl_->on_request_stop) {
    impl_->on_request_stop();
  }
}

void LauncherContext::set_stopping(bool stopping) noexcept {
  impl_->stopping.store(stopping, std::memory_order_release);

  if (stopping) {
    (void)impl_->cancel_source.request_cancel();
  }
}

void LauncherContext::update_param_internal(const std::string& key, LauncherParamValue value) {
  std::lock_guard lock(impl_->param_mtx);
  impl_->params[key] = std::move(value);
}

bool LauncherComponent::on_reload(LauncherContext& ctx) {
  (void)ctx;
  return true;
}

bool LauncherComponent::on_param_changed(LauncherContext& ctx, const std::string& key,
                                         const LauncherParamValue& value) {
  (void)ctx;
  (void)key;
  (void)value;
  return true;
}

bool LauncherComponent::on_drain_stop(uint32_t timeout_ms) noexcept {
  (void)timeout_ms;
  return true;
}

struct LauncherTimerComponent::Impl final {
  explicit Impl(uint32_t def_period, bool auto_wd, bool auto_rdy) noexcept
      : default_period_ms(def_period == 0U ? 1U : def_period),
        active_period_ms(def_period == 0U ? 1U : def_period),
        auto_watchdog(auto_wd),
        auto_ready(auto_rdy) {}

  uint32_t default_period_ms{1000U};
  std::atomic<uint32_t> active_period_ms{1000U};
  bool auto_watchdog{true};
  bool auto_ready{true};
  std::mutex timer_mtx;
  std::unique_ptr<Timer> timer;
};

LauncherTimerComponent::LauncherTimerComponent(uint32_t default_period_ms, bool auto_watchdog, bool auto_ready)
    : impl_(std::make_unique<Impl>(default_period_ms, auto_watchdog, auto_ready)) {}

LauncherTimerComponent::~LauncherTimerComponent() = default;

uint32_t LauncherTimerComponent::period_ms() const noexcept {
  return impl_->active_period_ms.load(std::memory_order_acquire);
}

bool LauncherTimerComponent::on_timer_init(LauncherContext& ctx) {
  (void)ctx;
  return true;
}

void LauncherTimerComponent::on_timer_stop(LauncherContext& ctx) { (void)ctx; }

bool LauncherTimerComponent::on_start(LauncherContext& ctx) {
  const int64_t configured = ctx.get_param_int("period_ms", static_cast<int64_t>(impl_->default_period_ms));
  const uint32_t period = (configured >= 1 && configured <= static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
                              ? static_cast<uint32_t>(configured)
                              : impl_->default_period_ms;
  impl_->active_period_ms.store(period, std::memory_order_release);

  if (!on_timer_init(ctx)) {
    return false;
  }

  auto timer = std::make_unique<Timer>(period, Timer::kInfinite, [this, &ctx]() {
    if (ctx.is_stopping()) {
      return;
    }

    if (impl_->auto_watchdog) {
      ctx.watchdog_ping();
    }

    on_tick(ctx);
  });

  if VUNLIKELY (!timer->attach(&ctx.loop())) {
    timer.reset();
    on_timer_stop(ctx);
    return false;
  }

  timer->start();

  {
    std::lock_guard lock(impl_->timer_mtx);
    impl_->timer = std::move(timer);
  }

  if (impl_->auto_ready) {
    ctx.mark_ready("timer ready (" + std::to_string(period) + "ms)");
  }

  return true;
}

void LauncherTimerComponent::on_stop(LauncherContext& ctx) {
  std::unique_ptr<Timer> old_timer;

  {
    std::lock_guard lock(impl_->timer_mtx);
    old_timer = std::move(impl_->timer);
  }

  if (old_timer) {
    old_timer->stop();
    old_timer.reset();
  }

  on_timer_stop(ctx);
}

bool LauncherTimerComponent::on_param_changed(LauncherContext& ctx, const std::string& key,
                                              const LauncherParamValue& value) {
  (void)ctx;

  if (key == "period_ms") {
    const auto* iv = std::get_if<int64_t>(&value);

    if (iv == nullptr || *iv < 1 || *iv > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
      return false;
    }

    const auto next_period = static_cast<uint32_t>(*iv);
    impl_->active_period_ms.store(next_period, std::memory_order_release);

    std::lock_guard lock(impl_->timer_mtx);

    if (impl_->timer) {
      impl_->timer->set_interval(next_period);
    }

    return true;
  }

  return true;
}

int run_launcher_component(int argc, char** argv, LauncherComponent& component, std::string_view default_name) {
  const bool managed = LauncherNotify::is_managed();
  std::string unit_name = Utils::get_env("VLINK_LAUNCHER_UNIT_NAME");

  if (unit_name.empty()) {
    unit_name = default_name.empty() ? std::string("component") : std::string(default_name);
  }

  std::string unit_ns = Utils::get_env("VLINK_LAUNCHER_UNIT_NS");
  LauncherParamMap params;
  const std::string params_file = Utils::get_env("VLINK_LAUNCHER_PARAMS_FILE");

  if (!params_file.empty()) {
#if defined(_WIN32) || defined(_WIN64)
    std::ifstream ifs(std::filesystem::path(Helpers::string_to_wstring(params_file)), std::ios::binary);
#else
    std::ifstream ifs(params_file, std::ios::binary);
#endif
    if (ifs) {
      std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
      load_params_from_json(content, params);
    } else {
      VLOG_W("LauncherComponent: Failed to open VLINK_LAUNCHER_PARAMS_FILE '", params_file, "'.");
    }
  }

  load_params_from_json(Utils::get_env("VLINK_LAUNCHER_PARAMS_JSON"), params);

  std::unordered_map<std::string, std::string> remaps;
  load_remaps_from_json(Utils::get_env("VLINK_LAUNCHER_REMAPS_JSON"), remaps);

  std::string sched_str = Utils::get_env("VLINK_LAUNCHER_SCHEDULER");
  std::string queue_str = Utils::get_env("VLINK_LAUNCHER_QUEUE");
  std::string strat_str = Utils::get_env("VLINK_LAUNCHER_QUEUE_STRATEGY");
  uint32_t worker_threads = 1U;
  {
    int64_t env_threads = 0;

    if (parse_int64_str(Utils::get_env("VLINK_LAUNCHER_THREADS"), env_threads) && env_threads >= 1 &&
        env_threads <= 64) {
      worker_threads = static_cast<uint32_t>(env_threads);
    }
  }

  if (argv != nullptr) {
    for (int i = 1; i < argc; ++i) {
      if (argv[i] == nullptr) {
        continue;
      }

      const std::string_view arg(argv[i]);

      if ((arg == "--name" || arg == "-n") && i + 1 < argc && argv[i + 1] != nullptr) {
        unit_name = argv[++i];
      } else if (arg.rfind("--name=", 0U) == 0U) {
        unit_name = std::string(arg.substr(7U));
      } else if (arg == "--ns" && i + 1 < argc && argv[i + 1] != nullptr) {
        unit_ns = argv[++i];
      } else if (arg.rfind("--ns=", 0U) == 0U) {
        unit_ns = std::string(arg.substr(5U));
      } else if ((arg == "--param" || arg == "-p") && i + 1 < argc && argv[i + 1] != nullptr) {
        const std::string kv(argv[++i]);
        const size_t eq = kv.find('=');

        if (eq != std::string::npos && eq > 0U) {
          params[kv.substr(0U, eq)] = parse_cli_param_value(kv.substr(eq + 1U));
        }
      } else if (arg.rfind("--param=", 0U) == 0U) {
        const std::string kv(arg.substr(8U));
        const size_t eq = kv.find('=');

        if (eq != std::string::npos && eq > 0U) {
          params[kv.substr(0U, eq)] = parse_cli_param_value(kv.substr(eq + 1U));
        }
      } else if ((arg == "--remap" || arg == "-r") && i + 1 < argc && argv[i + 1] != nullptr) {
        const std::string kv(argv[++i]);
        const size_t eq = kv.find('=');

        if (eq != std::string::npos && eq > 0U) {
          remaps[kv.substr(0U, eq)] = kv.substr(eq + 1U);
        }
      } else if (arg.rfind("--remap=", 0U) == 0U) {
        const std::string kv(arg.substr(8U));
        const size_t eq = kv.find('=');

        if (eq != std::string::npos && eq > 0U) {
          remaps[kv.substr(0U, eq)] = kv.substr(eq + 1U);
        }
      } else if (arg == "--scheduler" && i + 1 < argc && argv[i + 1] != nullptr) {
        sched_str = argv[++i];
      } else if (arg.rfind("--scheduler=", 0U) == 0U) {
        sched_str = std::string(arg.substr(12U));
      } else if (arg == "--threads" && i + 1 < argc && argv[i + 1] != nullptr) {
        int64_t tv = 0;

        if (parse_int64_str(argv[++i], tv) && tv >= 1 && tv <= 64) {
          worker_threads = static_cast<uint32_t>(tv);
        }
      } else if (arg.rfind("--threads=", 0U) == 0U) {
        int64_t tv = 0;

        if (parse_int64_str(arg.substr(10U), tv) && tv >= 1 && tv <= 64) {
          worker_threads = static_cast<uint32_t>(tv);
        }
      } else if (arg == "--queue" && i + 1 < argc && argv[i + 1] != nullptr) {
        queue_str = argv[++i];
      } else if (arg.rfind("--queue=", 0U) == 0U) {
        queue_str = std::string(arg.substr(8U));
      } else if (arg == "--queue-strategy" && i + 1 < argc && argv[i + 1] != nullptr) {
        strat_str = argv[++i];
      } else if (arg.rfind("--queue-strategy=", 0U) == 0U) {
        strat_str = std::string(arg.substr(17U));
      }
    }
  }

  auto lower_str = [](std::string s) {
    for (char& ch : s) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }

    return s;
  };
  sched_str = lower_str(std::move(sched_str));
  queue_str = lower_str(std::move(queue_str));
  strat_str = lower_str(std::move(strat_str));

  if (!queue_str.empty() && queue_str != "normal" && queue_str != "lockfree" && queue_str != "priority") {
    VLOG_W("LauncherComponent '", unit_name, "': Unknown queue '", queue_str, "', falling back to normal.");
  }

  MessageLoop::Type qtype = MessageLoop::kNormalType;

  if (queue_str == "lockfree") {
    qtype = MessageLoop::kLockfreeType;
  } else if (queue_str == "priority") {
    qtype = MessageLoop::kPriorityType;
  }

  if (!strat_str.empty() && strat_str != "optimization" && strat_str != "pop" && strat_str != "block") {
    VLOG_W("LauncherComponent '", unit_name, "': Unknown queue_strategy '", strat_str,
           "', falling back to optimization.");
  }

  MessageLoop::Strategy qstrat = MessageLoop::kOptimizationStrategy;

  if (strat_str == "pop") {
    qstrat = MessageLoop::kPopStrategy;
  } else if (strat_str == "block") {
    qstrat = MessageLoop::kBlockStrategy;
  }

  if (!sched_str.empty() && sched_str != "shared" && sched_str != "dedicated" && sched_str != "multi") {
    VLOG_W("LauncherComponent '", unit_name, "': Unknown scheduler '", sched_str, "', falling back to dedicated.");
  }

  const bool use_multi = (sched_str == "multi");

  if (use_multi && worker_threads < 2U) {
    worker_threads = 2U;
  } else if (!use_multi) {
    worker_threads = 1U;
  }

  g_standalone_stop_requested.store(false, std::memory_order_release);
  g_standalone_reload_requested.store(false, std::memory_order_release);
  SignalHandlerGuard signal_guard;

  std::unique_ptr<MessageLoop> loop_ptr;

  if (use_multi) {
    loop_ptr = std::make_unique<MultiLoop>(worker_threads, qtype);
  } else {
    loop_ptr = std::make_unique<MessageLoop>(qtype);
  }

  loop_ptr->set_strategy(qstrat);
  loop_ptr->set_name(unit_name.substr(0U, 15U));
  MessageLoop& loop = *loop_ptr;

  std::mutex wait_mtx;
  ConditionVariable wait_cv;
  std::atomic_bool stop_flag{false};

  LauncherContext::Hooks hooks;
  hooks.name = unit_name;
  hooks.ns = std::move(unit_ns);
  hooks.loop = &loop;
  hooks.params = std::move(params);
  hooks.remaps = std::move(remaps);
  hooks.watchdog_ms = managed ? LauncherNotify::watchdog_interval_ms() : 0U;
  hooks.worker_threads = worker_threads;
  hooks.standalone = true;
  hooks.on_ready = [managed](std::string_view detail) {
    if (managed) {
      (void)LauncherNotify::notify_ready(detail);
    }
  };
  hooks.on_health = [managed](bool healthy, std::string_view detail) {
    if (managed) {
      (void)LauncherNotify::notify_health(healthy, detail);
    }
  };
  hooks.on_watchdog = [managed]() {
    if (managed) {
      (void)LauncherNotify::notify_watchdog();
    }
  };
  hooks.on_request_stop = [&]() {
    {
      std::lock_guard lock(wait_mtx);
      stop_flag.store(true, std::memory_order_release);
    }

    wait_cv.notify_all();
  };

  LauncherContext ctx(std::move(hooks));

  if VUNLIKELY (!loop.async_run()) {
    VLOG_E("LauncherComponent '", unit_name, "': Failed to start MessageLoop.");
    return 1;
  }

  bool start_ok = false;
  const bool start_dispatched = post_and_wait(loop, [&]() {
    try {
      start_ok = component.on_start(ctx);
    } catch (const std::exception& e) {
      VLOG_E("LauncherComponent '", unit_name, "' on_start exception: ", e.what());
      start_ok = false;
    } catch (...) {
      VLOG_E("LauncherComponent '", unit_name, "' on_start unknown exception.");
      start_ok = false;
    }
  });

  if VUNLIKELY (!start_dispatched || !start_ok) {
    loop.quit(true);
    loop.wait_for_quit();
    return 1;
  }

  while (!stop_flag.load(std::memory_order_acquire) && !g_standalone_stop_requested.load(std::memory_order_acquire)) {
    if (g_standalone_reload_requested.exchange(false, std::memory_order_acq_rel)) {
      if (managed) {
        (void)LauncherNotify::notify_reloading("reloading");
      }

      (void)post_and_wait(loop, [&]() {
        try {
          (void)component.on_reload(ctx);
        } catch (const std::exception& e) {
          VLOG_E("LauncherComponent '", unit_name, "' on_reload exception: ", e.what());
        } catch (...) {
          VLOG_E("LauncherComponent '", unit_name, "' on_reload unknown exception.");
        }
      });
    }

    std::unique_lock lock(wait_mtx);
    (void)wait_cv.wait_for(lock, std::chrono::milliseconds(50), [&]() {
      return stop_flag.load(std::memory_order_acquire) || g_standalone_stop_requested.load(std::memory_order_acquire) ||
             g_standalone_reload_requested.load(std::memory_order_acquire);
    });
  }

  ctx.set_stopping(true);

  if (managed) {
    (void)LauncherNotify::notify_stopping("stopping");
  }

  (void)post_and_wait(loop, [&]() {
    try {
      component.on_stop(ctx);
    } catch (const std::exception& e) {
      VLOG_E("LauncherComponent '", unit_name, "' on_stop exception: ", e.what());
    } catch (...) {
      VLOG_E("LauncherComponent '", unit_name, "' on_stop unknown exception.");
    }
  });

  if (!component.on_drain_stop(3000U)) {
    loop.quit(false);
    loop.wait_for_quit();
    (void)component.on_drain_stop(1000U);
  } else {
    loop.quit(false);
    loop.wait_for_quit();
  }

  return 0;
}

int run_launcher_component_from_library(int argc, char** argv, std::string_view library, std::string_view class_name,
                                        std::string_view default_name) {
  if VUNLIKELY (library.empty()) {
    VLOG_E("LauncherComponent: Empty library name.");
    return 1;
  }

  Plugin plugin;
  const std::string lib_str(library);
  auto module = plugin.load<LauncherModuleInterface>(lib_str, LauncherModuleInterface::kVersionMajor,
                                                     LauncherModuleInterface::kVersionMinor);

  if (!module) {
    const std::string dirs_env = Utils::get_env("VLINK_LAUNCHER_PLUGIN_DIRS");
#if defined(_WIN32) || defined(_WIN64)
    constexpr char kPathSep = ';';
#else
    constexpr char kPathSep = ':';
#endif
    size_t pos = 0U;

    while (!module && pos <= dirs_env.size()) {
      const size_t next = dirs_env.find(kPathSep, pos);
      const std::string dir = (next == std::string::npos) ? dirs_env.substr(pos) : dirs_env.substr(pos, next - pos);
      pos = (next == std::string::npos) ? (dirs_env.size() + 1U) : (next + 1U);

      if (dir.empty()) {
        continue;
      }

      const std::filesystem::path base(dir);
      const std::vector<std::string> names = {
          lib_str,
#if defined(_WIN32) || defined(_WIN64)
          lib_str + ".dll",
#elif defined(__APPLE__)
          "lib" + lib_str + ".dylib",
#else
          "lib" + lib_str + ".so",
#endif
      };

      for (const auto& fname : names) {
        const std::filesystem::path cand = base / fname;
        std::error_code ec;

        if (std::filesystem::exists(cand, ec)) {
          module = plugin.load<LauncherModuleInterface>(Helpers::path_to_string(cand),
                                                        LauncherModuleInterface::kVersionMajor,
                                                        LauncherModuleInterface::kVersionMinor);

          if (module) {
            break;
          }
        }
      }
    }
  }

  if VUNLIKELY (!module) {
    VLOG_E("LauncherComponent: Failed to load library '", library, "'.");
    return 1;
  }

  struct CompDeleter final {
    LauncherModuleInterface* mod{nullptr};
    void operator()(LauncherComponent* ptr) const noexcept {
      if (mod != nullptr && ptr != nullptr) {
        mod->destroy_component(ptr);
      }
    }
  };

  std::unique_ptr<LauncherComponent, CompDeleter> comp(module->create_component(class_name), CompDeleter{module.get()});

  if VUNLIKELY (!comp) {
    VLOG_E("LauncherComponent: Failed to instantiate component '", class_name, "' from '", library, "'.");
    return 1;
  }

  std::string fallback_name(default_name);

  if (fallback_name.empty()) {
    if (!class_name.empty()) {
      fallback_name.assign(class_name);
    } else {
      const auto exported = module->list_components();

      if (!exported.empty()) {
        fallback_name = exported.front();
      }
    }
  }

  return run_launcher_component(argc, argv, *comp, fallback_name);
}

}  // namespace vlink
