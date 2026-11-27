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
#include <vlink/base/elapsed_timer.h>
#include <vlink/base/logger.h>
#include <vlink/base/macros.h>
#include <vlink/base/timer.h>
#include <vlink/client.h>
#include <vlink/extension/security.h>
#include <vlink/external/launcher_api.h>
#include <vlink/getter.h>
#include <vlink/subscriber.h>
//
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "../launcher_common.h"

namespace vlink {

namespace launcher {

std::string_view unit_type_name(UnitType type) noexcept {
  switch (type) {
    case UnitType::kProcess:
      return "process";
    case UnitType::kComponent:
      return "component";
  }

  return "unknown";
}

std::string_view unit_state_name(UnitState state) noexcept {
  switch (state) {
    case UnitState::kPending:
      return "pending";
    case UnitState::kStarting:
      return "starting";
    case UnitState::kRunning:
      return "running";
    case UnitState::kReady:
      return "ready";
    case UnitState::kStopping:
      return "stopping";
    case UnitState::kStopped:
      return "stopped";
    case UnitState::kFailed:
      return "failed";
  }

  return "unknown";
}

std::string_view error_code_name(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kOk:
      return "ok";
    case ErrorCode::kInvalidArgument:
      return "invalid_argument";
    case ErrorCode::kNotFound:
      return "not_found";
    case ErrorCode::kAlreadyRunning:
      return "already_running";
    case ErrorCode::kNotRunning:
      return "not_running";
    case ErrorCode::kDependencyFailed:
      return "dependency_failed";
    case ErrorCode::kUnsupported:
      return "unsupported";
    case ErrorCode::kTimeout:
      return "timeout";
    case ErrorCode::kDisconnected:
      return "disconnected";
    case ErrorCode::kVersionMismatch:
      return "version_mismatch";
    case ErrorCode::kUnauthorized:
      return "unauthorized";
    case ErrorCode::kInternalError:
      return "internal_error";
  }

  return "unknown";
}

static std::string format_double_lossless(double v) {
  std::ostringstream oss;
  oss.imbue(std::locale::classic());
  oss << std::setprecision(std::numeric_limits<double>::max_digits10) << v;
  return oss.str();
}

static bool parse_double_strict(const std::string& text, double& out) noexcept {
  if (text.empty()) {
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

static bool json_fits_int64_api(const nlohmann::json& val) noexcept {
  if (val.is_number_unsigned()) {
    return val.get<uint64_t>() <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  }

  return val.is_number_integer();
}

ParamEntry param_value_to_entry(const std::string& key, const LauncherParamValue& value) {
  ParamEntry e;
  e.key = key;

  std::visit(
      [&e](const auto& v) {
        using T = std::decay_t<decltype(v)>;

        if constexpr (std::is_same_v<T, bool>) {
          e.type = ParamType::kBool;
          e.value = v ? "true" : "false";
        } else if constexpr (std::is_same_v<T, int64_t>) {
          e.type = ParamType::kInt;
          e.value = std::to_string(v);
        } else if constexpr (std::is_same_v<T, double>) {
          e.type = ParamType::kDouble;
          e.value = format_double_lossless(v);
        } else if constexpr (std::is_same_v<T, std::string>) {
          e.type = ParamType::kString;
          e.value = v;
        } else if constexpr (std::is_same_v<T, std::vector<bool>>) {
          e.type = ParamType::kBoolList;
          nlohmann::json arr = nlohmann::json::array();

          for (const bool item : v) {
            arr.push_back(item);
          }

          e.value = arr.dump();
        } else if constexpr (std::is_same_v<T, std::vector<int64_t>>) {
          e.type = ParamType::kIntList;
          e.value = nlohmann::json(v).dump();
        } else if constexpr (std::is_same_v<T, std::vector<double>>) {
          e.type = ParamType::kDoubleList;
          e.value = nlohmann::json(v).dump();
        } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
          e.type = ParamType::kStringList;
          e.value = nlohmann::json(v).dump();
        }
      },
      value);

  return e;
}

bool entry_to_param_value(const ParamEntry& entry, LauncherParamValue& out) {
  try {
    switch (entry.type) {
      case ParamType::kBool:
        if (entry.value == "true" || entry.value == "1") {
          out = true;
          return true;
        }

        if (entry.value == "false" || entry.value == "0") {
          out = false;
          return true;
        }

        return false;

      case ParamType::kInt: {
        int64_t val = 0;
        const auto res = std::from_chars(entry.value.data(), entry.value.data() + entry.value.size(), val, 10);

        if (res.ec != std::errc() || res.ptr != entry.value.data() + entry.value.size()) {
          return false;
        }

        out = val;
        return true;
      }

      case ParamType::kDouble: {
        double val = 0.0;

        if (!parse_double_strict(entry.value, val)) {
          return false;
        }

        out = val;
        return true;
      }

      case ParamType::kString:
        out = entry.value;
        return true;

      case ParamType::kBoolList: {
        const auto arr = nlohmann::json::parse(entry.value);

        if (!arr.is_array()) {
          return false;
        }

        std::vector<bool> list;
        list.reserve(arr.size());

        for (const auto& item : arr) {
          if (!item.is_boolean()) {
            return false;
          }

          list.push_back(item.get<bool>());
        }

        out = std::move(list);
        return true;
      }

      case ParamType::kIntList: {
        const auto arr = nlohmann::json::parse(entry.value);

        if (!arr.is_array()) {
          return false;
        }

        std::vector<int64_t> list;
        list.reserve(arr.size());

        for (const auto& item : arr) {
          if (!json_fits_int64_api(item)) {
            return false;
          }

          list.push_back(item.get<int64_t>());
        }

        out = std::move(list);
        return true;
      }

      case ParamType::kDoubleList: {
        const auto arr = nlohmann::json::parse(entry.value);

        if (!arr.is_array()) {
          return false;
        }

        std::vector<double> list;
        list.reserve(arr.size());

        for (const auto& item : arr) {
          if (!item.is_number()) {
            return false;
          }

          const double v = item.get<double>();

          if (!std::isfinite(v)) {
            return false;
          }

          list.push_back(v);
        }

        out = std::move(list);
        return true;
      }

      case ParamType::kStringList: {
        const auto arr = nlohmann::json::parse(entry.value);

        if (!arr.is_array()) {
          return false;
        }

        std::vector<std::string> list;
        list.reserve(arr.size());

        for (const auto& item : arr) {
          if (!item.is_string()) {
            return false;
          }

          list.push_back(item.get<std::string>());
        }

        out = std::move(list);
        return true;
      }
    }
  } catch (...) {
    return false;
  }

  return false;
}

}  // namespace launcher

struct LauncherAPI::Impl final {
  using PlainClient = Client<launcher::CommandRequest, launcher::CommandResponse>;
  using PlainGetter = Getter<launcher::SystemSnapshot>;
  using PlainEventSub = Subscriber<launcher::EventRecord>;
  using PlainHbSub = Subscriber<launcher::HeartbeatMsg>;

#ifdef VLINK_ENABLE_SECURITY
  using SecClient = SecurityClient<launcher::CommandRequest, launcher::CommandResponse>;
  using SecGetter = SecurityGetter<launcher::SystemSnapshot>;
  using SecEventSub = SecuritySubscriber<launcher::EventRecord>;
  using SecHbSub = SecuritySubscriber<launcher::HeartbeatMsg>;
#endif

  explicit Impl(LauncherAPI* owner_ptr, const Config& cfg) : owner(owner_ptr), config(cfg) {}

  ~Impl() {
    shutdown_channels();
    launcher::wipe_secret_string(config.security_key);
  }

  void dispatch_connect(bool is_conn) {
    std::shared_ptr<ConnectCallback> cb;

    {
      std::lock_guard lock(cb_mtx);
      cb = connect_cb;
    }

    if (cb && *cb) {
      (*cb)(is_conn);
    }
  }

  void dispatch_snapshot(const launcher::SystemSnapshot& snap) {
    std::shared_ptr<SnapshotCallback> cb;

    {
      std::lock_guard lock(cb_mtx);
      cb = snapshot_cb;
    }

    if (cb && *cb) {
      (*cb)(snap);
    }
  }

  void dispatch_event(const launcher::EventRecord& ev) {
    std::shared_ptr<EventCallback> cb;

    {
      std::lock_guard lock(cb_mtx);
      cb = event_cb;
    }

    if (cb && *cb) {
      (*cb)(ev);
    }
  }

  bool init_channels() {
    std::lock_guard lock(chan_mtx);

    if (stopped) {
      return false;
    }

    if (channels_ready) {
      return true;
    }

    const std::string scheme = config.scheme.empty() ? std::string(launcher::default_scheme()) : config.scheme;
    const std::string cmd_url =
        launcher::build_channel_url(scheme, config.namespace_prefix, "ctrl", "method", config.domain_id);
    const std::string state_url =
        launcher::build_channel_url(scheme, config.namespace_prefix, "state", "field", config.domain_id);
    const std::string event_url =
        launcher::build_channel_url(scheme, config.namespace_prefix, "events", "event", config.domain_id);
    const std::string hb_url =
        launcher::build_channel_url(scheme, config.namespace_prefix, "hb", "clock", config.domain_id);

    auto on_state = [this](const launcher::SystemSnapshot& snap) {
      {
        std::lock_guard slock(state_mtx);
        cached_snapshot = snap;
      }

      dispatch_snapshot(snap);
    };

    auto on_event = [this](const launcher::EventRecord& ev) { dispatch_event(ev); };

    auto on_hb = [this](const launcher::HeartbeatMsg& hb) {
      if VUNLIKELY (hb.proto_version != launcher::kProtocolVersion) {
        return;
      }

      const uint64_t now_ms = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);
      bool notify_connect = false;

      {
        std::lock_guard slock(state_mtx);
        last_hb_steady_ms = now_ms;

        if (!connected_flag || active_session != hb.session_id) {
          if (active_session != hb.session_id) {
            cached_snapshot.reset();
          }

          active_session = hb.session_id;
          connected_flag = true;
          connected.store(true, std::memory_order_release);
          notify_connect = true;
        }
      }

      if (notify_connect) {
        conn_cv.notify_all();
        dispatch_connect(true);
      }
    };

#ifdef VLINK_ENABLE_SECURITY
    use_security = !config.security_key.empty() && scheme != "intra";

    if (use_security) {
      auto make_sec_cfg = [this](const char* aad) {
        Security::Config sc;
        sc.key = config.security_key;
        sc.advanced.aad_context = aad;
        return sc;
      };

      auto new_client =
          std::make_shared<SecClient>(cmd_url, make_sec_cfg("vlink.launcher.ctrl"), InitType::kWithoutInit);
      auto new_getter =
          std::make_shared<SecGetter>(state_url, make_sec_cfg("vlink.launcher.state"), InitType::kWithoutInit);
      auto new_event_sub =
          std::make_shared<SecEventSub>(event_url, make_sec_cfg("vlink.launcher.events"), InitType::kWithoutInit);
      auto new_hb_sub = std::make_shared<SecHbSub>(hb_url, make_sec_cfg("vlink.launcher.hb"), InitType::kWithoutInit);

      if (!new_client->init() || !new_getter->init() || !new_event_sub->init() || !new_hb_sub->init()) {
        return false;
      }

      (void)new_getter->attach(owner);
      (void)new_event_sub->attach(owner);
      (void)new_hb_sub->attach(owner);

      new_getter->listen(std::move(on_state));
      new_event_sub->listen(std::move(on_event));
      new_hb_sub->listen(std::move(on_hb));

      sec_client = std::move(new_client);
      sec_getter = std::move(new_getter);
      sec_event_sub = std::move(new_event_sub);
      sec_hb_sub = std::move(new_hb_sub);
      channels_ready = true;
      return true;
    }
#else
    if (!config.security_key.empty() && scheme != "intra") {
      VLOG_E("LauncherAPI: security_key configured but VLink was built without VLINK_ENABLE_SECURITY.");
      return false;
    }
#endif

    auto new_client = std::make_shared<PlainClient>(cmd_url, InitType::kWithoutInit);
    auto new_getter = std::make_shared<PlainGetter>(state_url, InitType::kWithoutInit);
    auto new_event_sub = std::make_shared<PlainEventSub>(event_url, InitType::kWithoutInit);
    auto new_hb_sub = std::make_shared<PlainHbSub>(hb_url, InitType::kWithoutInit);

    if (!new_client->init() || !new_getter->init() || !new_event_sub->init() || !new_hb_sub->init()) {
      return false;
    }

    (void)new_getter->attach(owner);
    (void)new_event_sub->attach(owner);
    (void)new_hb_sub->attach(owner);

    new_getter->listen(std::move(on_state));
    new_event_sub->listen(std::move(on_event));
    new_hb_sub->listen(std::move(on_hb));

    client = std::move(new_client);
    getter = std::move(new_getter);
    event_sub = std::move(new_event_sub);
    hb_sub = std::move(new_hb_sub);
    channels_ready = true;
    return true;
  }

  void shutdown_channels() {
    std::unique_ptr<Timer> old_timer;
    std::shared_ptr<PlainHbSub> old_hb_sub;
    std::shared_ptr<PlainEventSub> old_event_sub;
    std::shared_ptr<PlainGetter> old_getter;
    std::shared_ptr<PlainClient> old_client;
#ifdef VLINK_ENABLE_SECURITY
    std::shared_ptr<SecHbSub> old_sec_hb_sub;
    std::shared_ptr<SecEventSub> old_sec_event_sub;
    std::shared_ptr<SecGetter> old_sec_getter;
    std::shared_ptr<SecClient> old_sec_client;
#endif

    {
      std::lock_guard lock(chan_mtx);
      stopped = true;
      channels_ready = false;
      old_timer.swap(hb_timer);
      old_hb_sub.swap(hb_sub);
      old_event_sub.swap(event_sub);
      old_getter.swap(getter);
      old_client.swap(client);
#ifdef VLINK_ENABLE_SECURITY
      old_sec_hb_sub.swap(sec_hb_sub);
      old_sec_event_sub.swap(sec_event_sub);
      old_sec_getter.swap(sec_getter);
      old_sec_client.swap(sec_client);
#endif
    }

    if (old_timer) {
      old_timer->stop();
      old_timer.reset();
    }

    if (old_client) {
      old_client->interrupt();
    }

#ifdef VLINK_ENABLE_SECURITY
    if (old_sec_client) {
      old_sec_client->interrupt();
    }
#endif

    old_hb_sub.reset();
    old_event_sub.reset();
    old_getter.reset();
    old_client.reset();
#ifdef VLINK_ENABLE_SECURITY
    old_sec_hb_sub.reset();
    old_sec_event_sub.reset();
    old_sec_getter.reset();
    old_sec_client.reset();
#endif

    {
      std::lock_guard slock(state_mtx);
      closed_flag = true;
      connected_flag = false;
      connected.store(false, std::memory_order_release);
      active_session.clear();
      cached_snapshot.reset();
    }

    conn_cv.notify_all();
  }

  void check_heartbeat_timeout() {
    const uint64_t now_ms = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);
    bool lost = false;

    {
      std::lock_guard slock(state_mtx);

      if (connected_flag && now_ms > last_hb_steady_ms &&
          (now_ms - last_hb_steady_ms) > launcher::kHeartbeatTimeoutMs) {
        connected_flag = false;
        connected.store(false, std::memory_order_release);
        active_session.clear();
        cached_snapshot.reset();
        lost = true;
      }
    }

    if (lost) {
      dispatch_connect(false);
    }
  }

  launcher::CommandResult invoke(const launcher::CommandRequest& req, uint32_t timeout_ms) {
    if (!init_channels()) {
      return {launcher::ErrorCode::kDisconnected, "control client is closed or failed to init", std::nullopt, {}};
    }

    uint32_t eff_timeout = (timeout_ms == 0U) ? config.default_timeout_ms : timeout_ms;

    if (eff_timeout == 0U) {
      eff_timeout = 3000U;
    }

    const uint64_t deadline = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli) + eff_timeout;
    launcher::CommandResponse resp;
    bool invoked = false;

    bool sec_mode = false;
    std::shared_ptr<PlainClient> plain_cli;
#ifdef VLINK_ENABLE_SECURITY
    std::shared_ptr<SecClient> sec_cli;
#endif

    {
      std::lock_guard lock(chan_mtx);
      sec_mode = use_security;
      plain_cli = client;
#ifdef VLINK_ENABLE_SECURITY
      sec_cli = sec_client;
#endif
    }

    auto remaining_ms = [&]() -> uint64_t {
      const uint64_t now = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);
      return (deadline > now) ? (deadline - now) : 0ULL;
    };

#ifdef VLINK_ENABLE_SECURITY
    if (sec_mode) {
      if VUNLIKELY (!sec_cli) {
        return {launcher::ErrorCode::kDisconnected, "control client not initialized", std::nullopt, {}};
      }

      if (!sec_cli->is_connected()) {
        const uint64_t rem = remaining_ms();

        if (rem == 0ULL || !sec_cli->wait_for_connected(std::chrono::milliseconds(rem))) {
          return {launcher::ErrorCode::kDisconnected, "failed to connect to LauncherServer", std::nullopt, {}};
        }
      }

      const uint64_t rem = remaining_ms();

      if (rem == 0ULL) {
        return {launcher::ErrorCode::kTimeout, "RPC deadline expired before invoke", std::nullopt, {}};
      }

      invoked = sec_cli->invoke(req, resp, std::chrono::milliseconds(rem));
    } else
#endif
    {
      (void)sec_mode;

      if VUNLIKELY (!plain_cli) {
        return {launcher::ErrorCode::kDisconnected, "control client not initialized", std::nullopt, {}};
      }

      if (!plain_cli->is_connected()) {
        const uint64_t rem = remaining_ms();

        if (rem == 0ULL || !plain_cli->wait_for_connected(std::chrono::milliseconds(rem))) {
          return {launcher::ErrorCode::kDisconnected, "failed to connect to LauncherServer", std::nullopt, {}};
        }
      }

      const uint64_t rem = remaining_ms();

      if (rem == 0ULL) {
        return {launcher::ErrorCode::kTimeout, "RPC deadline expired before invoke", std::nullopt, {}};
      }

      invoked = plain_cli->invoke(req, resp, std::chrono::milliseconds(rem));
    }

    if (!invoked) {
      return {launcher::ErrorCode::kTimeout, "RPC invoke failed or timed out", std::nullopt, {}};
    }

    if VUNLIKELY (resp.proto_version != launcher::kProtocolVersion) {
      return {launcher::ErrorCode::kVersionMismatch, "launcher protocol version mismatch", std::nullopt, {}};
    }

    if (resp.has_snapshot && resp.result.snapshot.has_value()) {
      std::lock_guard slock(state_mtx);
      cached_snapshot = resp.result.snapshot;
    }

    return std::move(resp.result);
  }

  LauncherAPI* owner{nullptr};
  Config config;
  mutable std::mutex chan_mtx;
  mutable std::mutex state_mtx;
  std::mutex cb_mtx;
  ConditionVariable conn_cv;

  bool stopped{false};
  bool channels_ready{false};
  bool use_security{false};
  bool connected_flag{false};
  bool closed_flag{false};
  std::atomic_bool connected{false};
  uint64_t last_hb_steady_ms{0};
  std::string active_session;
  std::optional<launcher::SystemSnapshot> cached_snapshot;

  std::shared_ptr<ConnectCallback> connect_cb;
  std::shared_ptr<SnapshotCallback> snapshot_cb;
  std::shared_ptr<EventCallback> event_cb;

  std::unique_ptr<Timer> hb_timer;
  std::shared_ptr<PlainClient> client;
  std::shared_ptr<PlainGetter> getter;
  std::shared_ptr<PlainEventSub> event_sub;
  std::shared_ptr<PlainHbSub> hb_sub;

#ifdef VLINK_ENABLE_SECURITY
  std::shared_ptr<SecClient> sec_client;
  std::shared_ptr<SecGetter> sec_getter;
  std::shared_ptr<SecEventSub> sec_event_sub;
  std::shared_ptr<SecHbSub> sec_hb_sub;
#endif
};

LauncherAPI::LauncherAPI(const Config& config) : impl_(std::make_unique<Impl>(this, config)) {}

LauncherAPI::LauncherAPI() : LauncherAPI(Config{}) {}

LauncherAPI::~LauncherAPI() {
  if (MessageLoop::is_running()) {
    quit(true);
    wait_for_quit();
  }

  impl_->shutdown_channels();
}

void LauncherAPI::register_connect_callback(ConnectCallback&& callback) {
  auto next = callback ? std::make_shared<ConnectCallback>(std::move(callback)) : nullptr;
  std::shared_ptr<ConnectCallback> old;

  {
    std::lock_guard lock(impl_->cb_mtx);
    old = std::move(impl_->connect_cb);
    impl_->connect_cb = std::move(next);
  }
}

void LauncherAPI::register_snapshot_callback(SnapshotCallback&& callback) {
  auto next = callback ? std::make_shared<SnapshotCallback>(std::move(callback)) : nullptr;
  std::shared_ptr<SnapshotCallback> old;

  {
    std::lock_guard lock(impl_->cb_mtx);
    old = std::move(impl_->snapshot_cb);
    impl_->snapshot_cb = std::move(next);
  }
}

void LauncherAPI::register_event_callback(EventCallback&& callback) {
  auto next = callback ? std::make_shared<EventCallback>(std::move(callback)) : nullptr;
  std::shared_ptr<EventCallback> old;

  {
    std::lock_guard lock(impl_->cb_mtx);
    old = std::move(impl_->event_cb);
    impl_->event_cb = std::move(next);
  }
}

bool LauncherAPI::is_connected() const noexcept { return impl_->connected.load(std::memory_order_acquire); }

bool LauncherAPI::wait_for_connected(uint32_t timeout_ms) {
  if (is_in_same_thread()) {
    return is_connected();
  }

  if (!MessageLoop::is_running() || !impl_->init_channels()) {
    return false;
  }

  std::unique_lock lock(impl_->state_mtx);
  (void)impl_->conn_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                [this]() { return impl_->connected_flag || impl_->closed_flag; });
  return impl_->connected_flag;
}

std::string LauncherAPI::session_id() const {
  std::lock_guard lock(impl_->state_mtx);
  return impl_->active_session;
}

std::optional<launcher::SystemSnapshot> LauncherAPI::latest_snapshot() const {
  std::lock_guard lock(impl_->state_mtx);
  return impl_->cached_snapshot;
}

launcher::CommandResult LauncherAPI::get_status(uint32_t timeout_ms) {
  launcher::CommandRequest req;
  req.op = launcher::CommandOp::kGetStatus;
  return impl_->invoke(req, timeout_ms);
}

launcher::CommandResult LauncherAPI::start_unit(const std::string& target, uint32_t timeout_ms) {
  launcher::CommandRequest req;
  req.op = launcher::CommandOp::kStartUnit;
  req.target = target;
  return impl_->invoke(req, timeout_ms);
}

launcher::CommandResult LauncherAPI::stop_unit(const std::string& target, uint32_t timeout_ms) {
  launcher::CommandRequest req;
  req.op = launcher::CommandOp::kStopUnit;
  req.target = target;
  return impl_->invoke(req, timeout_ms);
}

launcher::CommandResult LauncherAPI::restart_unit(const std::string& target, uint32_t timeout_ms) {
  launcher::CommandRequest req;
  req.op = launcher::CommandOp::kRestartUnit;
  req.target = target;
  return impl_->invoke(req, timeout_ms);
}

launcher::CommandResult LauncherAPI::reload_unit(const std::string& target, uint32_t timeout_ms) {
  launcher::CommandRequest req;
  req.op = launcher::CommandOp::kReloadUnit;
  req.target = target;
  return impl_->invoke(req, timeout_ms);
}

launcher::CommandResult LauncherAPI::get_params(const std::string& unit_name, uint32_t timeout_ms) {
  launcher::CommandRequest req;
  req.op = launcher::CommandOp::kGetParams;
  req.target = unit_name;
  return impl_->invoke(req, timeout_ms);
}

launcher::CommandResult LauncherAPI::set_param(const std::string& unit_name, const launcher::ParamEntry& entry,
                                               uint32_t timeout_ms) {
  launcher::CommandRequest req;
  req.op = launcher::CommandOp::kSetParam;
  req.target = unit_name;
  req.param = entry;
  return impl_->invoke(req, timeout_ms);
}

launcher::CommandResult LauncherAPI::shutdown_server(uint32_t timeout_ms) {
  launcher::CommandRequest req;
  req.op = launcher::CommandOp::kShutdown;
  return impl_->invoke(req, timeout_ms);
}

void LauncherAPI::on_begin() {
  {
    std::lock_guard lock(impl_->chan_mtx);
    impl_->stopped = false;
  }

  {
    std::lock_guard slock(impl_->state_mtx);
    impl_->closed_flag = false;
  }

  if (!impl_->init_channels()) {
    return;
  }

  std::lock_guard lock(impl_->chan_mtx);

  if (!impl_->stopped) {
    impl_->hb_timer = std::make_unique<Timer>(this, launcher::kHeartbeatIntervalMs, Timer::kInfinite,
                                              [this]() { impl_->check_heartbeat_timeout(); });
    impl_->hb_timer->start();
  }
}

void LauncherAPI::on_end() { impl_->shutdown_channels(); }

}  // namespace vlink
