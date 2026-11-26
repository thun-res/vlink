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

#include <vlink/external/launcher_api.h>
//
#include <vlink/base/elapsed_timer.h>
#include <vlink/base/helpers.h>
#include <vlink/base/logger.h>
#include <vlink/base/timer.h>
#include <vlink/client.h>
#include <vlink/extension/security.h>
#include <vlink/getter.h>
#include <vlink/subscriber.h>
//
#include <nlohmann/json.hpp>
//
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <locale>
#include <memory>
#include <mutex>
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

static constexpr size_t kLauncherApiMaxTaskCount = 4096U;
static constexpr uint32_t kLauncherApiMaxElapsedMs = 5000U;

namespace launcher {

bool CommandResult::ok() const noexcept { return code == ErrorCode::kOk; }

const char* unit_type_to_string(UnitType type) noexcept {
  switch (type) {
    case UnitType::kProcess:
      return "process";
    case UnitType::kComponent:
      return "component";
    case UnitType::kContainer:
      return "container";
    case UnitType::kTask:
      return "task";
    case UnitType::kProxy:
      return "proxy";
    default:
      return "unknown";
  }
}

const char* unit_state_to_string(UnitState state) noexcept {
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
    case UnitState::kRestarting:
      return "restarting";
    case UnitState::kFailed:
      return "failed";
    case UnitState::kSkipped:
      return "skipped";
    case UnitState::kInactive:
      return "inactive";
    default:
      return "unknown";
  }
}

const char* error_code_to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kOk:
      return "ok";
    case ErrorCode::kInvalidArgument:
      return "invalid_argument";
    case ErrorCode::kNotFound:
      return "not_found";
    case ErrorCode::kAlreadyExists:
      return "already_exists";
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
    default:
      return "unknown";
  }
}

ParamEntry encode_param_entry(std::string_view key, const LauncherParamValue& value) {
  ParamEntry entry;
  entry.key = std::string(key);

  std::visit(
      [&entry](const auto& v) {
        using ValT = std::decay_t<decltype(v)>;

        if constexpr (std::is_same_v<ValT, bool>) {
          entry.type = ParamType::kBool;
          entry.value = v ? "true" : "false";
        } else if constexpr (std::is_same_v<ValT, int64_t>) {
          entry.type = ParamType::kInt;
          entry.value = std::to_string(v);
        } else if constexpr (std::is_same_v<ValT, double>) {
          entry.type = ParamType::kDouble;
          char buf[64];
          const size_t n = Helpers::format_floating_spec_to(buf, sizeof(buf) - 1U, v, 'g', 17, false);
          entry.value.assign(buf, n);
        } else if constexpr (std::is_same_v<ValT, std::string>) {
          entry.type = ParamType::kString;
          entry.value = v;
        } else if constexpr (std::is_same_v<ValT, std::vector<bool>>) {
          entry.type = ParamType::kBoolList;
          nlohmann::json arr = nlohmann::json::array();

          for (const bool item : v) {
            arr.push_back(item);
          }

          entry.value = arr.dump();
        } else if constexpr (std::is_same_v<ValT, std::vector<int64_t>>) {
          entry.type = ParamType::kIntList;
          entry.value = nlohmann::json(v).dump();
        } else if constexpr (std::is_same_v<ValT, std::vector<double>>) {
          entry.type = ParamType::kDoubleList;
          entry.value = nlohmann::json(v).dump();
        } else if constexpr (std::is_same_v<ValT, std::vector<std::string>>) {
          entry.type = ParamType::kStringList;
          entry.value = nlohmann::json(v).dump();
        }
      },
      value);

  return entry;
}

bool decode_param_entry(const ParamEntry& entry, LauncherParamValue& out) {
  switch (entry.type) {
    case ParamType::kBool: {
      if (entry.value == "true" || entry.value == "1") {
        out = true;
        return true;
      }

      if (entry.value == "false" || entry.value == "0") {
        out = false;
        return true;
      }

      return false;
    }
    case ParamType::kInt: {
      if (entry.value.empty() || std::isspace(static_cast<unsigned char>(entry.value.front())) != 0) {
        return false;
      }

      errno = 0;
      char* end = nullptr;
      const auto parsed = static_cast<int64_t>(std::strtoll(entry.value.c_str(), &end, 10));

      if (errno != 0 || end != entry.value.c_str() + entry.value.size()) {
        return false;
      }

      out = parsed;
      return true;
    }
    case ParamType::kDouble: {
      if (entry.value.empty() || std::isspace(static_cast<unsigned char>(entry.value.front())) != 0 ||
          std::isspace(static_cast<unsigned char>(entry.value.back())) != 0) {
        return false;
      }

      std::istringstream iss(entry.value);
      iss.imbue(std::locale::classic());
      double parsed = 0.0;
      iss >> parsed;

      if (iss.fail() || !iss.eof() || !std::isfinite(parsed)) {
        return false;
      }

      out = parsed;
      return true;
    }
    case ParamType::kString: {
      out = entry.value;
      return true;
    }
    case ParamType::kBoolList:
    case ParamType::kIntList:
    case ParamType::kDoubleList:
    case ParamType::kStringList: {
      auto parsed = nlohmann::json::parse(entry.value, nullptr, false);

      if (parsed.is_discarded() || !parsed.is_array()) {
        return false;
      }

      auto decode_list = [&parsed, &out](auto&& extract) -> bool {
        using ElemT = std::decay_t<decltype(extract(parsed[0]))>;
        using ValueT = typename ElemT::value_type;
        std::vector<ValueT> list;
        list.reserve(parsed.size());

        for (const auto& item : parsed) {
          auto elem = extract(item);

          if (!elem.has_value()) {
            return false;
          }

          list.push_back(std::move(*elem));
        }

        out = std::move(list);
        return true;
      };

      if (entry.type == ParamType::kBoolList) {
        return decode_list([](const nlohmann::json& item) -> std::optional<bool> {
          return item.is_boolean() ? std::optional<bool>(item.get<bool>()) : std::nullopt;
        });
      }

      if (entry.type == ParamType::kIntList) {
        return decode_list([](const nlohmann::json& item) -> std::optional<int64_t> {
          if (!item.is_number_integer() ||
              (item.is_number_unsigned() &&
               item.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))) {
            return std::nullopt;
          }

          return item.get<int64_t>();
        });
      }

      if (entry.type == ParamType::kDoubleList) {
        return decode_list([](const nlohmann::json& item) -> std::optional<double> {
          if (!item.is_number()) {
            return std::nullopt;
          }

          const double v = item.get<double>();
          return std::isfinite(v) ? std::optional<double>(v) : std::nullopt;
        });
      }

      return decode_list([](const nlohmann::json& item) -> std::optional<std::string> {
        return item.is_string() ? std::optional<std::string>(item.get<std::string>()) : std::nullopt;
      });
    }
    default:
      return false;
  }
}

}  // namespace launcher

static launcher::CommandResult make_error_result(launcher::ErrorCode code, std::string message) {
  launcher::CommandResult res;
  res.code = code;
  res.message = std::move(message);
  return res;
}

template <typename NodeT, typename... ArgsT>
static std::unique_ptr<NodeT> create_control_node(std::string_view scheme, std::string_view bind_ip, bool native_mode,
                                                  ArgsT&&... args) {
  auto node = std::make_unique<NodeT>(std::forward<ArgsT>(args)..., InitType::kWithoutInit);
  node->set_discovery_enabled(false);
  node->set_safety_quit(true);
  launcher::apply_control_node_properties(*node, scheme, bind_ip, native_mode);

  if VUNLIKELY (!node->init()) {
    return nullptr;
  }

  return node;
}

#ifdef VLINK_ENABLE_SECURITY
static Security::Config make_channel_sec_cfg(const std::string& key, std::string_view aad) {
  Security::Config cfg;
  cfg.key = key;
  cfg.advanced.aad_context = std::string(aad);
  return cfg;
}
#endif

struct LauncherAPI::Impl final {
  using PlainClient = Client<launcher::CommandRequest, launcher::CommandResponse>;
  using PlainGetter = Getter<launcher::SystemSnapshot>;
  using PlainEventSub = Subscriber<launcher::EventRecord>;
  using PlainLogSub = Subscriber<launcher::LogRecord>;
  using PlainHbSub = Subscriber<launcher::HeartbeatMsg>;

#ifdef VLINK_ENABLE_SECURITY
  using SecClient = SecurityClient<launcher::CommandRequest, launcher::CommandResponse>;
  using SecGetter = SecurityGetter<launcher::SystemSnapshot>;
  using SecEventSub = SecuritySubscriber<launcher::EventRecord>;
  using SecLogSub = SecuritySubscriber<launcher::LogRecord>;
  using SecHbSub = SecuritySubscriber<launcher::HeartbeatMsg>;
#endif

  explicit Impl(LauncherAPI* owner_ptr) : owner(owner_ptr) {}

  LauncherAPI* owner{nullptr};
  Config config;
  bool use_security{false};
  bool initialized{false};

  std::atomic_bool connected{false};
  std::atomic_bool hb_timed_out{false};
  std::atomic_bool closing{false};
  std::atomic_bool proto_warned{false};
  std::atomic<uint64_t> last_hb_ms{0};

  mutable std::mutex session_mtx;
  std::string server_session_id;

  mutable std::mutex cb_mtx;
  std::shared_ptr<ConnectCallback> connect_cb;
  std::shared_ptr<SnapshotCallback> snapshot_cb;
  std::shared_ptr<EventCallback> event_cb;
  std::shared_ptr<LogCallback> log_cb;

  mutable std::mutex snap_mtx;
  std::optional<launcher::SystemSnapshot> cached_snapshot;

  std::unique_ptr<Timer> hb_timer;

  std::unique_ptr<PlainClient> client;
  std::unique_ptr<PlainGetter> state_getter;
  std::unique_ptr<PlainEventSub> event_sub;
  std::unique_ptr<PlainLogSub> log_sub;
  std::unique_ptr<PlainHbSub> hb_sub;

#ifdef VLINK_ENABLE_SECURITY
  std::unique_ptr<SecClient> sec_client;
  std::unique_ptr<SecGetter> sec_state_getter;
  std::unique_ptr<SecEventSub> sec_event_sub;
  std::unique_ptr<SecLogSub> sec_log_sub;
  std::unique_ptr<SecHbSub> sec_hb_sub;
#endif

  template <typename CbT, typename ArgT>
  void dispatch_cb(std::shared_ptr<CbT> Impl::* slot, ArgT&& arg);

  template <typename CbT>
  bool swap_cb(std::shared_ptr<CbT> Impl::* slot, CbT&& callback);

  void dispatch_connect(bool is_conn);

  void dispatch_snapshot(const launcher::SystemSnapshot& snap);

  void dispatch_event(const launcher::EventRecord& ev);

  void dispatch_log(const launcher::LogRecord& rec);

  void release_transports();

  launcher::CommandResult invoke_rpc(const launcher::CommandRequest& req, uint32_t timeout_ms);

  launcher::CommandResult invoke_target_op(launcher::CommandOp op, const std::string& target, uint32_t timeout_ms);
};

template <typename CbT, typename ArgT>
void LauncherAPI::Impl::dispatch_cb(std::shared_ptr<CbT> Impl::* slot, ArgT&& arg) {
  if VUNLIKELY (closing.load(std::memory_order_acquire)) {
    return;
  }

  {
    std::lock_guard lock(cb_mtx);
    const auto& cur = this->*slot;

    if (!cur || !(*cur)) {
      return;
    }
  }

  auto task = [this, slot, payload = std::forward<ArgT>(arg)]() {
    if VUNLIKELY (closing.load(std::memory_order_acquire)) {
      return;
    }

    std::shared_ptr<CbT> cb;

    {
      std::lock_guard lock(cb_mtx);
      cb = this->*slot;
    }

    if (cb && *cb) {
      (*cb)(payload);
    }
  };

  if (owner->is_running()) {
    (void)owner->post_task(std::move(task));
  } else {
    task();
  }
}

template <typename CbT>
bool LauncherAPI::Impl::swap_cb(std::shared_ptr<CbT> Impl::* slot, CbT&& callback) {
  const bool has_cb = static_cast<bool>(callback);
  auto new_cb = has_cb ? std::make_shared<CbT>(std::forward<CbT>(callback)) : nullptr;
  std::shared_ptr<CbT> old_cb;

  {
    std::lock_guard lock(cb_mtx);
    old_cb = std::move(this->*slot);
    this->*slot = std::move(new_cb);
  }

  old_cb = nullptr;
  return has_cb;
}

void LauncherAPI::Impl::dispatch_connect(bool is_conn) { dispatch_cb(&Impl::connect_cb, is_conn); }

void LauncherAPI::Impl::dispatch_snapshot(const launcher::SystemSnapshot& snap) {
  if VUNLIKELY (closing.load(std::memory_order_acquire)) {
    return;
  }

  {
    std::lock_guard lock(snap_mtx);
    cached_snapshot = snap;
  }

  dispatch_cb(&Impl::snapshot_cb, snap);
}

void LauncherAPI::Impl::dispatch_event(const launcher::EventRecord& ev) { dispatch_cb(&Impl::event_cb, ev); }

void LauncherAPI::Impl::dispatch_log(const launcher::LogRecord& rec) { dispatch_cb(&Impl::log_cb, rec); }

void LauncherAPI::Impl::release_transports() {
  if (client) {
    client->interrupt();
  }

#ifdef VLINK_ENABLE_SECURITY
  if (sec_client) {
    sec_client->interrupt();
  }
#endif

  hb_sub.reset();
  event_sub.reset();
  log_sub.reset();
  state_getter.reset();
  client.reset();

#ifdef VLINK_ENABLE_SECURITY
  sec_hb_sub.reset();
  sec_event_sub.reset();
  sec_log_sub.reset();
  sec_state_getter.reset();
  sec_client.reset();
#endif
}

LauncherAPI::LauncherAPI(const Config& config) : impl_(std::make_unique<Impl>(this)) {
  impl_->config = config;
  impl_->use_security = !config.security_key.empty();
  set_name("LauncherAPI");

  const std::string actual_scheme = launcher::resolve_scheme(config.scheme);

  if (impl_->use_security && actual_scheme == "intra") {
    VLOG_E("LauncherAPI: Security key is not supported with intra:// transport.");
    impl_->initialized = false;
    return;
  }

  const std::string ctrl_url =
      launcher::build_channel_url(actual_scheme, config.namespace_prefix, "ctrl", "method", config.domain_id);
  const std::string state_url =
      launcher::build_channel_url(actual_scheme, config.namespace_prefix, "state", "field", config.domain_id);
  const std::string event_url =
      launcher::build_channel_url(actual_scheme, config.namespace_prefix, "events", "event", config.domain_id);
  const std::string log_url =
      launcher::build_channel_url(actual_scheme, config.namespace_prefix, "logs", "log", config.domain_id);
  const std::string hb_url =
      launcher::build_channel_url(actual_scheme, config.namespace_prefix, "hb", "clock", config.domain_id);

  auto on_hb = [this](const launcher::HeartbeatMsg& hb) {
    if VUNLIKELY (hb.proto_version != launcher::kProtocolVersion) {
      if (!impl_->proto_warned.exchange(true, std::memory_order_acq_rel)) {
        VLOG_W("LauncherAPI: Heartbeat protocol version mismatch (server=", hb.proto_version,
               ", local=", launcher::kProtocolVersion, ").");
      }

      return;
    }

    bool session_changed = false;
    bool newly_connected = false;

    {
      std::lock_guard lock(impl_->session_mtx);
      impl_->last_hb_ms.store(ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli), std::memory_order_release);
      impl_->hb_timed_out.store(false, std::memory_order_release);

      if (!hb.session_id.empty()) {
        if (!impl_->server_session_id.empty() && impl_->server_session_id != hb.session_id) {
          session_changed = true;
        }

        impl_->server_session_id = hb.session_id;
      }

      newly_connected = !impl_->connected.exchange(true, std::memory_order_acq_rel);
    }

    if (session_changed) {
      {
        std::lock_guard lock(impl_->snap_mtx);
        impl_->cached_snapshot.reset();
      }

      VLOG_I("LauncherAPI: Detected LauncherServer session change -> ", hb.session_id, ".");
    }

    if (newly_connected || session_changed) {
      impl_->dispatch_connect(true);
    }
  };

  auto on_snap = [this](const launcher::SystemSnapshot& snap) { impl_->dispatch_snapshot(snap); };
  auto on_event = [this](const launcher::EventRecord& ev) { impl_->dispatch_event(ev); };
  auto on_log = [this](const launcher::LogRecord& rec) { impl_->dispatch_log(rec); };

  if (impl_->use_security) {
#ifdef VLINK_ENABLE_SECURITY
    impl_->sec_client =
        create_control_node<Impl::SecClient>(actual_scheme, config.bind_ip, config.native_mode, ctrl_url,
                                             make_channel_sec_cfg(config.security_key, "launcher.ctrl"));
    impl_->sec_state_getter =
        create_control_node<Impl::SecGetter>(actual_scheme, config.bind_ip, config.native_mode, state_url,
                                             make_channel_sec_cfg(config.security_key, "launcher.state"));
    impl_->sec_hb_sub = create_control_node<Impl::SecHbSub>(actual_scheme, config.bind_ip, config.native_mode, hb_url,
                                                            make_channel_sec_cfg(config.security_key, "launcher.hb"));

    if (config.subscribe_events) {
      impl_->sec_event_sub =
          create_control_node<Impl::SecEventSub>(actual_scheme, config.bind_ip, config.native_mode, event_url,
                                                 make_channel_sec_cfg(config.security_key, "launcher.events"));
    }

    if (config.subscribe_logs) {
      impl_->sec_log_sub =
          create_control_node<Impl::SecLogSub>(actual_scheme, config.bind_ip, config.native_mode, log_url,
                                               make_channel_sec_cfg(config.security_key, "launcher.logs"));
    }

    const bool nodes_ok = impl_->sec_client && impl_->sec_state_getter && impl_->sec_hb_sub &&
                          (!config.subscribe_events || impl_->sec_event_sub) &&
                          (!config.subscribe_logs || impl_->sec_log_sub);

    if VUNLIKELY (!nodes_ok) {
      impl_->release_transports();
      impl_->initialized = false;
      return;
    }

    impl_->sec_state_getter->listen(std::move(on_snap));
    impl_->sec_hb_sub->listen(std::move(on_hb));

    if (impl_->sec_event_sub) {
      impl_->sec_event_sub->listen(std::move(on_event));
    }

    if (impl_->sec_log_sub) {
      impl_->sec_log_sub->listen(std::move(on_log));
    }

    impl_->initialized = true;
#else
    VLOG_E("LauncherAPI: Security key specified, but VLink was built without ENABLE_SECURITY.");
    impl_->initialized = false;
#endif
  } else {
    impl_->client = create_control_node<Impl::PlainClient>(actual_scheme, config.bind_ip, config.native_mode, ctrl_url);
    impl_->state_getter =
        create_control_node<Impl::PlainGetter>(actual_scheme, config.bind_ip, config.native_mode, state_url);
    impl_->hb_sub = create_control_node<Impl::PlainHbSub>(actual_scheme, config.bind_ip, config.native_mode, hb_url);

    if (config.subscribe_events) {
      impl_->event_sub =
          create_control_node<Impl::PlainEventSub>(actual_scheme, config.bind_ip, config.native_mode, event_url);
    }

    if (config.subscribe_logs) {
      impl_->log_sub =
          create_control_node<Impl::PlainLogSub>(actual_scheme, config.bind_ip, config.native_mode, log_url);
    }

    const bool nodes_ok = impl_->client && impl_->state_getter && impl_->hb_sub &&
                          (!config.subscribe_events || impl_->event_sub) && (!config.subscribe_logs || impl_->log_sub);

    if VUNLIKELY (!nodes_ok) {
      impl_->release_transports();
      impl_->initialized = false;
      return;
    }

    impl_->state_getter->listen(std::move(on_snap));
    impl_->hb_sub->listen(std::move(on_hb));

    if (impl_->event_sub) {
      impl_->event_sub->listen(std::move(on_event));
    }

    if (impl_->log_sub) {
      impl_->log_sub->listen(std::move(on_log));
    }

    impl_->initialized = true;
  }
}

LauncherAPI::~LauncherAPI() {
  impl_->closing.store(true, std::memory_order_release);
  impl_->release_transports();

  if (is_running()) {
    quit(true);
    wait_for_quit();
  }

  launcher::wipe_secret_string(impl_->config.security_key);
}

const LauncherAPI::Config& LauncherAPI::get_current_config() const noexcept { return impl_->config; }

void LauncherAPI::register_connect_callback(ConnectCallback&& callback) {
  if (impl_->swap_cb(&Impl::connect_cb, std::move(callback)) && is_connected()) {
    impl_->dispatch_connect(true);
  }
}

void LauncherAPI::register_snapshot_callback(SnapshotCallback&& callback) {
  if (!impl_->swap_cb(&Impl::snapshot_cb, std::move(callback)) || impl_->closing.load(std::memory_order_acquire)) {
    return;
  }

  auto deliver_latest = [this]() {
    if VUNLIKELY (impl_->closing.load(std::memory_order_acquire)) {
      return;
    }

    std::shared_ptr<SnapshotCallback> cb;

    {
      std::lock_guard lock(impl_->cb_mtx);
      cb = impl_->snapshot_cb;
    }

    if (!cb || !(*cb)) {
      return;
    }

    std::optional<launcher::SystemSnapshot> latest;

    {
      std::lock_guard lock(impl_->snap_mtx);
      latest = impl_->cached_snapshot;
    }

    if (latest.has_value()) {
      (*cb)(*latest);
    }
  };

  if (is_running()) {
    (void)post_task(std::move(deliver_latest));
  } else {
    deliver_latest();
  }
}

void LauncherAPI::register_event_callback(EventCallback&& callback) {
  (void)impl_->swap_cb(&Impl::event_cb, std::move(callback));
}

void LauncherAPI::register_log_callback(LogCallback&& callback) {
  (void)impl_->swap_cb(&Impl::log_cb, std::move(callback));
}

bool LauncherAPI::is_connected() const noexcept {
  if VUNLIKELY (!impl_->initialized) {
    return false;
  }

  if (impl_->hb_timed_out.load(std::memory_order_acquire)) {
    return false;
  }

  if (impl_->connected.load(std::memory_order_acquire)) {
    return true;
  }

  if (impl_->use_security) {
#ifdef VLINK_ENABLE_SECURITY
    return impl_->sec_client && impl_->sec_client->is_connected();
#else
    return false;
#endif
  }

  return impl_->client && impl_->client->is_connected();
}

bool LauncherAPI::wait_for_connected(uint32_t timeout_ms) {
  if VUNLIKELY (!impl_->initialized) {
    return false;
  }

  const uint32_t actual_ms = (timeout_ms == 0U) ? impl_->config.default_timeout_ms : timeout_ms;
  const auto dur = std::chrono::milliseconds(actual_ms);

  if (impl_->use_security) {
#ifdef VLINK_ENABLE_SECURITY
    return impl_->sec_client && impl_->sec_client->wait_for_connected(dur);
#else
    return false;
#endif
  }

  return impl_->client && impl_->client->wait_for_connected(dur);
}

std::optional<launcher::SystemSnapshot> LauncherAPI::get_cached_snapshot() const {
  {
    std::lock_guard lock(impl_->snap_mtx);

    if (impl_->cached_snapshot.has_value()) {
      return impl_->cached_snapshot;
    }
  }

  std::string expected_session;

  {
    std::lock_guard lock(impl_->session_mtx);
    expected_session = impl_->server_session_id;
  }

  std::optional<launcher::SystemSnapshot> snap;

  if (impl_->use_security) {
#ifdef VLINK_ENABLE_SECURITY
    if (impl_->sec_state_getter) {
      snap = impl_->sec_state_getter->get();
    }
#endif
  } else if (impl_->state_getter) {
    snap = impl_->state_getter->get();
  }

  if (snap.has_value() && !expected_session.empty() && snap->session_id != expected_session) {
    return std::nullopt;
  }

  return snap;
}

launcher::CommandResult LauncherAPI::Impl::invoke_rpc(const launcher::CommandRequest& req, uint32_t timeout_ms) {
  if VUNLIKELY (!initialized) {
    return make_error_result(launcher::ErrorCode::kUnsupported, "LauncherAPI is not initialized.");
  }

  const uint32_t actual_ms = (timeout_ms == 0U) ? config.default_timeout_ms : timeout_ms;
  const auto dur = std::chrono::milliseconds(actual_ms);
  launcher::CommandResponse resp;
  bool invoked = false;

  if (use_security) {
#ifdef VLINK_ENABLE_SECURITY
    if (sec_client) {
      if (!sec_client->is_connected() && !sec_client->wait_for_connected(dur)) {
        return make_error_result(launcher::ErrorCode::kDisconnected, "Failed to connect to LauncherServer.");
      }

      invoked = sec_client->invoke(req, resp, dur);
    }
#else
    return make_error_result(launcher::ErrorCode::kUnsupported, "Security support disabled at compile time.");
#endif
  } else if (client) {
    if (!client->is_connected() && !client->wait_for_connected(dur)) {
      return make_error_result(launcher::ErrorCode::kDisconnected, "Failed to connect to LauncherServer.");
    }

    invoked = client->invoke(req, resp, dur);
  }

  if VUNLIKELY (!invoked) {
    return make_error_result(launcher::ErrorCode::kTimeout, "Launcher control RPC timed out.");
  }

  if VUNLIKELY (resp.proto_version != launcher::kProtocolVersion) {
    return make_error_result(launcher::ErrorCode::kVersionMismatch, "Launcher protocol version mismatch.");
  }

  return std::move(resp.result);
}

launcher::CommandResult LauncherAPI::get_status(uint32_t timeout_ms) {
  launcher::CommandRequest req;
  req.op = launcher::CommandOp::kGetStatus;
  auto res = impl_->invoke_rpc(req, timeout_ms);

  if (res.ok()) {
    std::lock_guard lock(impl_->snap_mtx);
    impl_->cached_snapshot = res.snapshot;
  }

  return res;
}

launcher::CommandResult LauncherAPI::Impl::invoke_target_op(launcher::CommandOp op, const std::string& target,
                                                            uint32_t timeout_ms) {
  launcher::CommandRequest req;
  req.op = op;
  req.target = target;
  return invoke_rpc(req, timeout_ms);
}

launcher::CommandResult LauncherAPI::start_unit(const std::string& target, uint32_t timeout_ms) {
  return impl_->invoke_target_op(launcher::CommandOp::kStartUnit, target, timeout_ms);
}

launcher::CommandResult LauncherAPI::stop_unit(const std::string& target, uint32_t timeout_ms) {
  return impl_->invoke_target_op(launcher::CommandOp::kStopUnit, target, timeout_ms);
}

launcher::CommandResult LauncherAPI::restart_unit(const std::string& target, uint32_t timeout_ms) {
  return impl_->invoke_target_op(launcher::CommandOp::kRestartUnit, target, timeout_ms);
}

launcher::CommandResult LauncherAPI::reload_unit(const std::string& target, uint32_t timeout_ms) {
  return impl_->invoke_target_op(launcher::CommandOp::kReloadUnit, target, timeout_ms);
}

launcher::CommandResult LauncherAPI::activate_unit(const std::string& target, uint32_t timeout_ms) {
  return impl_->invoke_target_op(launcher::CommandOp::kActivateUnit, target, timeout_ms);
}

launcher::CommandResult LauncherAPI::deactivate_unit(const std::string& target, uint32_t timeout_ms) {
  return impl_->invoke_target_op(launcher::CommandOp::kDeactivateUnit, target, timeout_ms);
}

launcher::CommandResult LauncherAPI::reset_failed(const std::string& target, uint32_t timeout_ms) {
  return impl_->invoke_target_op(launcher::CommandOp::kResetFailed, target, timeout_ms);
}

launcher::CommandResult LauncherAPI::isolate_group(const std::string& group_name, uint32_t timeout_ms) {
  return impl_->invoke_target_op(launcher::CommandOp::kIsolateGroup, group_name, timeout_ms);
}

launcher::CommandResult LauncherAPI::get_unit_config(const std::string& unit_name, uint32_t timeout_ms) {
  return impl_->invoke_target_op(launcher::CommandOp::kGetUnitConfig, unit_name, timeout_ms);
}

launcher::CommandResult LauncherAPI::query_logs(const launcher::LogQuery& query, uint32_t timeout_ms) {
  launcher::CommandRequest req;
  req.op = launcher::CommandOp::kQueryLogs;
  req.log_query = query;
  return impl_->invoke_rpc(req, timeout_ms);
}

launcher::CommandResult LauncherAPI::load_component(const launcher::LoadComponentRequest& request,
                                                    uint32_t timeout_ms) {
  launcher::CommandRequest req;
  req.op = launcher::CommandOp::kLoadComponent;
  req.load_req = request;
  return impl_->invoke_rpc(req, timeout_ms);
}

launcher::CommandResult LauncherAPI::unload_component(const std::string& unit_name, uint32_t timeout_ms) {
  return impl_->invoke_target_op(launcher::CommandOp::kUnloadComponent, unit_name, timeout_ms);
}

launcher::CommandResult LauncherAPI::get_params(const std::string& unit_name, uint32_t timeout_ms) {
  return impl_->invoke_target_op(launcher::CommandOp::kGetParams, unit_name, timeout_ms);
}

launcher::CommandResult LauncherAPI::set_param(const std::string& unit_name, const launcher::ParamEntry& entry,
                                               uint32_t timeout_ms) {
  launcher::CommandRequest req;
  req.op = launcher::CommandOp::kSetParam;
  req.target = unit_name;
  req.param = entry;
  return impl_->invoke_rpc(req, timeout_ms);
}

launcher::CommandResult LauncherAPI::set_record(const std::string& target, bool enable, const std::string& path,
                                                uint32_t timeout_ms) {
  launcher::CommandRequest req;
  req.op = launcher::CommandOp::kSetRecord;
  req.target = target;
  req.record_enable = enable;
  req.record_path = path;
  return impl_->invoke_rpc(req, timeout_ms);
}

launcher::CommandResult LauncherAPI::resume_startup(uint32_t timeout_ms) {
  launcher::CommandRequest req;
  req.op = launcher::CommandOp::kResumeStartup;
  return impl_->invoke_rpc(req, timeout_ms);
}

launcher::CommandResult LauncherAPI::shutdown_server(uint32_t timeout_ms) {
  launcher::CommandRequest req;
  req.op = launcher::CommandOp::kShutdown;
  return impl_->invoke_rpc(req, timeout_ms);
}

size_t LauncherAPI::get_max_task_count() const { return kLauncherApiMaxTaskCount; }

uint32_t LauncherAPI::get_max_elapsed_time() const { return kLauncherApiMaxElapsedMs; }

void LauncherAPI::on_begin() {
  MessageLoop::on_begin();

  impl_->hb_timer = std::make_unique<Timer>(this, launcher::kHeartbeatIntervalMs, Timer::kInfinite, [this] {
    if VUNLIKELY (impl_->closing.load(std::memory_order_acquire)) {
      return;
    }

    const uint64_t last = impl_->last_hb_ms.load(std::memory_order_acquire);

    if (last == 0U) {
      return;
    }

    const uint64_t now = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);

    if (now > last && (now - last) > launcher::kHeartbeatTimeoutMs) {
      bool timed_out = false;

      {
        std::lock_guard lock(impl_->session_mtx);
        const uint64_t latest = impl_->last_hb_ms.load(std::memory_order_acquire);

        if (latest == last && impl_->connected.exchange(false, std::memory_order_acq_rel)) {
          impl_->hb_timed_out.store(true, std::memory_order_release);
          timed_out = true;
        }
      }

      if (timed_out) {
        std::shared_ptr<ConnectCallback> cb;

        {
          std::lock_guard lock(impl_->cb_mtx);
          cb = impl_->connect_cb;
        }

        if (cb && *cb) {
          (*cb)(false);
        }
      }
    }
  });
  impl_->hb_timer->start();
}

void LauncherAPI::on_end() {
  if (impl_->hb_timer) {
    impl_->hb_timer->stop();
    impl_->hb_timer.reset();
  }

  MessageLoop::on_end();
}

}  // namespace vlink
