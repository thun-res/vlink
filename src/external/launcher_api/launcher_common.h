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

#include <vlink/base/bytes.h>
#include <vlink/base/logger.h>
#include <vlink/external/launcher_api.h>
//
#include <private/bitsery_bytes.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace vlink::launcher {

static constexpr uint8_t kProtocolVersion = 4;
static constexpr size_t kMaxShortStr = 256;
static constexpr size_t kMaxDetailStr = 4096;
static constexpr size_t kMaxUnitCount = 512;
static constexpr size_t kMaxDepCount = 128;
static constexpr size_t kMaxParamCount = 1024;

static constexpr uint32_t kHeartbeatIntervalMs = 1000;
static constexpr uint32_t kHeartbeatTimeoutMs = 5000;

enum class CommandOp : uint8_t {
  kGetStatus = 0,
  kStartUnit = 1,
  kStopUnit = 2,
  kRestartUnit = 3,
  kReloadUnit = 4,
  kGetParams = 5,
  kSetParam = 6,
  kShutdown = 7,
};

static_assert(sizeof(UnitType) == 1, "wire format: UnitType must be 1 byte");
static_assert(sizeof(UnitState) == 1, "wire format: UnitState must be 1 byte");
static_assert(sizeof(ErrorCode) == 1, "wire format: ErrorCode must be 1 byte");
static_assert(sizeof(ParamType) == 1, "wire format: ParamType must be 1 byte");
static_assert(sizeof(CommandOp) == 1, "wire format: CommandOp must be 1 byte");

using bitsery_bytes::bounded;
using bitsery_bytes::InputAdapter;
using bitsery_bytes::OutputAdapter;

inline void wipe_secret_string(std::string& s) noexcept {
  if (!s.empty()) {
    volatile char* p = s.data();

    for (size_t i = 0U; i < s.size(); ++i) {
      p[i] = '\0';
    }

    std::atomic_signal_fence(std::memory_order_seq_cst);
    s.clear();
    s.shrink_to_fit();
  }
}

struct HeartbeatMsg final {
  uint8_t proto_version{kProtocolVersion};
  std::string session_id;
  uint64_t timestamp_ms{0};
  uint64_t uptime_ms{0};
  uint32_t unit_count{0};
  uint32_t ready_count{0};
  uint32_t failed_count{0};

  template <typename S>
  void serialize(S& s);

  bool operator>>(Bytes& des) const noexcept;
  bool operator<<(const Bytes& src) noexcept;
};

struct CommandRequest final {
  uint8_t proto_version{kProtocolVersion};
  CommandOp op{CommandOp::kGetStatus};
  std::string target;
  ParamEntry param;

  template <typename S>
  void serialize(S& s);

  bool operator>>(Bytes& des) const noexcept;
  bool operator<<(const Bytes& src) noexcept;
};

struct CommandResponse final {
  uint8_t proto_version{kProtocolVersion};
  CommandResult result;
  bool has_snapshot{false};

  template <typename S>
  void serialize(S& s);

  bool operator>>(Bytes& des) const noexcept;
  bool operator<<(const Bytes& src) noexcept;
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline std::string_view default_scheme() noexcept {
#if defined(VLINK_SUPPORT_DDS)
  return "dds";
#elif defined(VLINK_SUPPORT_DDSC)
  return "ddsc";
#elif defined(VLINK_SUPPORT_DDSR)
  return "ddsr";
#elif defined(VLINK_SUPPORT_ZENOH)
  return "zenoh";
#else
  return "intra";
#endif
}

inline bool is_dds_scheme_name(std::string_view scheme) noexcept {
  return scheme == "dds" || scheme == "ddsc" || scheme == "ddsr" || scheme == "ddsf";
}

inline std::string build_channel_url(std::string_view scheme, std::string_view ns_prefix, std::string_view channel,
                                     std::string_view qos, uint32_t domain_id) {
  const std::string_view actual_scheme = scheme.empty() ? default_scheme() : scheme;

  while (!ns_prefix.empty() && ns_prefix.front() == '/') {
    ns_prefix.remove_prefix(1U);
  }

  while (!ns_prefix.empty() && ns_prefix.back() == '/') {
    ns_prefix.remove_suffix(1U);
  }

  const std::string_view actual_ns = ns_prefix.empty() ? std::string_view("launcher") : ns_prefix;

  std::string url;
  url.reserve(120);
  url.append(actual_scheme).append("://").append(actual_ns).append("/").append(channel);

  const bool is_dds_like = is_dds_scheme_name(actual_scheme);
  bool has_query = false;

  auto append_param = [&](std::string_view key, std::string_view value) {
    if (value.empty()) {
      return;
    }

    url.push_back(has_query ? '&' : '?');
    has_query = true;
    url.append(key).append("=").append(value);
  };

  if (is_dds_like) {
    if (!qos.empty()) {
      append_param("qos", qos);
    }

    append_param("domain", std::to_string(domain_id));
  } else if (actual_scheme == "intra" || actual_scheme == "shm" || actual_scheme == "shm2") {
    if (domain_id != 0U) {
      append_param("domain", std::to_string(domain_id));
    }
  } else {
    if (!qos.empty()) {
      append_param("qos", qos);
    }

    if (domain_id != 0U) {
      append_param("domain", std::to_string(domain_id));
    }
  }

  return url;
}

template <typename T>
inline bool encode_msg(const T& obj, Bytes& des) noexcept {
  try {
    des.clear();
    bool ok = true;
    bitsery::Serializer<OutputAdapter, bool> ser(ok, OutputAdapter{des});
    ser(obj);
    ser.adapter().flush();
    const size_t written = ser.adapter().writtenBytesCount();

    if VUNLIKELY (!ok) {
      des.clear();
      return false;
    }

    (void)des.shrink_to(written);
    return true;
  } catch (const std::exception& e) {
    VLOG_E("Launcher encode_msg exception: ", e.what());
    des.clear();
    return false;
  } catch (...) {
    des.clear();
    return false;
  }
}

template <typename T>
inline bool decode_msg(const Bytes& src, T& obj) noexcept {
  obj = T{};

  if VUNLIKELY (src.empty() || src.data() == nullptr) {
    return false;
  }

  try {
    const auto state = bitsery::quickDeserialization<InputAdapter>(InputAdapter{src.data(), src.size()}, obj);

    if VUNLIKELY (state.first != bitsery::ReaderError::NoError || !state.second) {
      obj = T{};
      return false;
    }

    return true;
  } catch (const std::exception& e) {
    VLOG_E("Launcher decode_msg exception: ", e.what());
    obj = T{};
    return false;
  } catch (...) {
    obj = T{};
    return false;
  }
}

template <typename S>
inline void serialize_str_list(S& s, std::vector<std::string>& items, size_t max_items, size_t max_len) {
  const auto list = bounded(s, items, max_items);
  s.container(list.data, list.maxSize,
              [max_len](auto& item_ser, auto& str) { item_ser(bounded(item_ser, str, max_len)); });
}

template <typename S>
inline void HeartbeatMsg::serialize(S& s) {
  s.ext(*this, bitsery::ext::Growable{}, [](S& ser, HeartbeatMsg& o) {
    ser(o.proto_version);
    ser(bounded(ser, o.session_id, kMaxShortStr));
    ser(o.timestamp_ms);
    ser(o.uptime_ms);
    ser(o.unit_count);
    ser(o.ready_count);
    ser(o.failed_count);
  });
}

inline bool HeartbeatMsg::operator>>(Bytes& des) const noexcept { return encode_msg(*this, des); }
inline bool HeartbeatMsg::operator<<(const Bytes& src) noexcept { return decode_msg(src, *this); }

template <typename S>
inline void serialize(S& s, ParamEntry& o) {
  s.ext(o, bitsery::ext::Growable{}, [](S& ser, ParamEntry& p) {
    ser(bounded(ser, p.key, kMaxShortStr));
    ser(p.type);
    ser(bounded(ser, p.value, kMaxDetailStr));
  });
}

template <typename S>
inline void serialize(S& s, UnitStatus& o) {
  s.ext(o, bitsery::ext::Growable{}, [](S& ser, UnitStatus& u) {
    ser(bounded(ser, u.name, kMaxShortStr));
    ser(bounded(ser, u.ns, kMaxShortStr));
    ser(bounded(ser, u.group, kMaxShortStr));
    ser(u.type);
    ser(u.state);
    ser(u.pid);
    ser(u.exit_code);
    ser(u.restart_count);
    ser(u.generation);
    ser(u.start_time_ms);
    ser(u.uptime_ms);
    ser(u.healthy);
    ser(u.required);
    ser(bounded(ser, u.failed_reason, kMaxShortStr));
    ser(bounded(ser, u.detail, kMaxDetailStr));
    serialize_str_list(ser, u.dependencies, kMaxDepCount, kMaxShortStr);
  });
}

template <typename S>
inline void serialize(S& s, SystemSnapshot& o) {
  s.ext(o, bitsery::ext::Growable{}, [](S& ser, SystemSnapshot& snap) {
    ser(bounded(ser, snap.session_id, kMaxShortStr));
    ser(bounded(ser, snap.version, kMaxShortStr));
    ser(bounded(ser, snap.host, kMaxShortStr));
    ser(snap.server_pid);
    ser(snap.timestamp_ms);
    ser(snap.uptime_ms);
    ser(bounded(ser, snap.config_file, kMaxDetailStr));
    const auto ulist = bounded(ser, snap.units, kMaxUnitCount);
    ser.container(ulist.data, ulist.maxSize);
  });
}

template <typename S>
inline void serialize(S& s, EventRecord& o) {
  s.ext(o, bitsery::ext::Growable{}, [](S& ser, EventRecord& ev) {
    ser(ev.timestamp_ms);
    ser(bounded(ser, ev.unit_name, kMaxShortStr));
    ser(ev.unit_type);
    ser(ev.old_state);
    ser(ev.new_state);
    ser(bounded(ser, ev.event_name, kMaxShortStr));
    ser(bounded(ser, ev.message, kMaxDetailStr));
  });
}

template <typename S>
inline void CommandRequest::serialize(S& s) {
  s.ext(*this, bitsery::ext::Growable{}, [](S& ser, CommandRequest& o) {
    ser(o.proto_version);
    ser(o.op);
    ser(bounded(ser, o.target, kMaxShortStr));
    ser(o.param);
  });
}

inline bool valid_unit_type(UnitType v) noexcept {
  return static_cast<uint8_t>(v) <= static_cast<uint8_t>(UnitType::kComponent);
}
inline bool valid_unit_state(UnitState v) noexcept {
  return static_cast<uint8_t>(v) <= static_cast<uint8_t>(UnitState::kFailed);
}
inline bool valid_error_code(ErrorCode v) noexcept {
  return static_cast<uint8_t>(v) <= static_cast<uint8_t>(ErrorCode::kInternalError);
}
inline bool valid_param_type(ParamType v) noexcept {
  return static_cast<uint8_t>(v) <= static_cast<uint8_t>(ParamType::kStringList);
}
inline bool valid_command_op(CommandOp v) noexcept {
  return static_cast<uint8_t>(v) <= static_cast<uint8_t>(CommandOp::kShutdown);
}

inline bool validate_decoded(const HeartbeatMsg&) noexcept { return true; }

inline bool validate_decoded(const ParamEntry& p) noexcept { return valid_param_type(p.type); }

inline bool validate_decoded(const UnitStatus& u) noexcept {
  return valid_unit_type(u.type) && valid_unit_state(u.state);
}

inline bool validate_decoded(const SystemSnapshot& snap) noexcept {
  for (const auto& u : snap.units) {
    if VUNLIKELY (!validate_decoded(u)) {
      return false;
    }
  }

  return true;
}

inline bool validate_decoded(const EventRecord& ev) noexcept {
  return valid_unit_type(ev.unit_type) && valid_unit_state(ev.old_state) && valid_unit_state(ev.new_state);
}

inline bool validate_decoded(const CommandRequest& req) noexcept {
  return valid_command_op(req.op) && validate_decoded(req.param);
}

inline bool validate_decoded(const CommandResponse& resp) noexcept {
  if VUNLIKELY (!valid_error_code(resp.result.code)) {
    return false;
  }

  for (const auto& p : resp.result.params) {
    if VUNLIKELY (!validate_decoded(p)) {
      return false;
    }
  }

  if (resp.result.snapshot.has_value() && !validate_decoded(*resp.result.snapshot)) {
    return false;
  }

  return true;
}

inline bool CommandRequest::operator>>(Bytes& des) const noexcept { return encode_msg(*this, des); }
inline bool CommandRequest::operator<<(const Bytes& src) noexcept {
  if (!decode_msg(src, *this)) {
    return false;
  }

  if VUNLIKELY (!validate_decoded(*this)) {
    *this = CommandRequest{};
    return false;
  }

  return true;
}

template <typename S>
inline void CommandResponse::serialize(S& s) {
  s.ext(*this, bitsery::ext::Growable{}, [](S& ser, CommandResponse& o) {
    ser(o.proto_version);
    ser(o.result.code);
    ser(bounded(ser, o.result.message, kMaxDetailStr));
    const auto plist = bounded(ser, o.result.params, kMaxParamCount);
    ser.container(plist.data, plist.maxSize);

    if constexpr (std::is_same_v<S, bitsery::Serializer<OutputAdapter, bool>>) {
      bool has_snap = o.result.snapshot.has_value();
      ser(has_snap);

      if (has_snap) {
        ser(*o.result.snapshot);
      }
    } else {
      ser(o.has_snapshot);

      if (o.has_snapshot) {
        o.result.snapshot.emplace();
        ser(*o.result.snapshot);
      } else {
        o.result.snapshot.reset();
      }
    }
  });
}

inline bool CommandResponse::operator>>(Bytes& des) const noexcept { return encode_msg(*this, des); }
inline bool CommandResponse::operator<<(const Bytes& src) noexcept {
  if (!decode_msg(src, *this)) {
    return false;
  }

  if VUNLIKELY (!validate_decoded(*this)) {
    *this = CommandResponse{};
    return false;
  }

  return true;
}

inline bool operator>>(const SystemSnapshot& src, Bytes& des) noexcept { return encode_msg(src, des); }
inline bool operator<<(SystemSnapshot& des, const Bytes& src) noexcept {
  if (!decode_msg(src, des)) {
    return false;
  }

  if VUNLIKELY (!validate_decoded(des)) {
    des = SystemSnapshot{};
    return false;
  }

  return true;
}
inline bool operator>>(const EventRecord& src, Bytes& des) noexcept { return encode_msg(src, des); }
inline bool operator<<(EventRecord& des, const Bytes& src) noexcept {
  if (!decode_msg(src, des)) {
    return false;
  }

  if VUNLIKELY (!validate_decoded(des)) {
    des = EventRecord{};
    return false;
  }

  return true;
}

}  // namespace vlink::launcher
