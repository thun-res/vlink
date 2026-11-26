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
#include <vlink/base/utils.h>
#include <vlink/external/launcher_api.h>
#include <vlink/serializer.h>
//
#include <private/bitsery_bytes.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vlink {

namespace launcher {

static constexpr uint8_t kProtocolVersion = 3;
static constexpr size_t kMaxShortStr = 256;
static constexpr size_t kMaxDetailStr = 16384;
static constexpr size_t kMaxConfigStr = 65536;
static constexpr size_t kMaxLogStr = 8192;
static constexpr size_t kMaxUnitCount = 1024;
static constexpr size_t kMaxDepCount = 256;
static constexpr size_t kMaxParamCount = 2048;
static constexpr size_t kMaxLogBatch = 256;
static constexpr size_t kMaxRemapCount = 256;

static constexpr uint32_t kHeartbeatIntervalMs = 1000;
static constexpr uint32_t kHeartbeatTimeoutMs = 5000;

enum class CommandOp : uint8_t {
  kGetStatus = 0,
  kStartUnit = 1,
  kStopUnit = 2,
  kRestartUnit = 3,
  kLoadComponent = 4,
  kUnloadComponent = 5,
  kGetParams = 6,
  kSetParam = 7,
  kShutdown = 8,
  kReloadUnit = 9,
  kResetFailed = 10,
  kIsolateGroup = 11,
  kGetUnitConfig = 12,
  kQueryLogs = 13,
  kResumeStartup = 14,
  kActivateUnit = 15,
  kDeactivateUnit = 16,
  kSetRecord = 17,
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

    s.clear();
  }
}

inline void park_leaked_object(std::shared_ptr<void> holder) {
  if (!holder) {
    return;
  }

  static auto* park_mtx = new std::mutex();
  static auto* parked = new std::vector<std::shared_ptr<void>>();
  std::lock_guard lock(*park_mtx);
  parked->push_back(std::move(holder));
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
  LoadComponentRequest load_req;
  ParamEntry param;
  LogQuery log_query;
  bool record_enable{false};
  std::string record_path;

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

[[nodiscard]] inline std::string sanitize_identifier(std::string_view name, bool allow_dot = false,
                                                     std::string_view fallback = "unit") {
  if VUNLIKELY (name.empty()) {
    return std::string(fallback);
  }

  const bool is_dot_traversal = (name == "." || name == "..");
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(name.size() + 8U);

  for (const char ch : name) {
    const auto u = static_cast<unsigned char>(ch);
    const bool is_alpha = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
    const bool is_digit = (ch >= '0' && ch <= '9');

    if (is_alpha || is_digit || ch == '-' || (allow_dot && !is_dot_traversal && ch == '.')) {
      out.push_back(ch);
    } else if (ch == '_') {
      out.append("__");
    } else {
      out.push_back('_');
      out.push_back('x');
      out.push_back(kHexDigits[(u >> 4U) & 0x0FU]);
      out.push_back(kHexDigits[u & 0x0FU]);
    }
  }

  return out;
}

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

inline std::string resolve_scheme(std::string_view scheme) {
  return scheme.empty() ? std::string(default_scheme()) : std::string(scheme);
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

template <typename NodeT>
inline void apply_control_node_properties(NodeT& node, std::string_view scheme, std::string_view bind_ip,
                                          bool native_mode) {
  const std::string_view actual_scheme = scheme.empty() ? default_scheme() : scheme;

  if (is_dds_scheme_name(actual_scheme)) {
    if (!bind_ip.empty()) {
      node.set_property("dds.ip", std::string(bind_ip));
    } else if (native_mode) {
      node.set_property("dds.ip", Utils::get_native_ip());
    }
  } else if (actual_scheme == "zenoh") {
    if (!bind_ip.empty()) {
      node.set_property("zenoh.ip", std::string(bind_ip));
    }

    if (native_mode) {
      node.set_property("zenoh.allowed_locality", "local");
    }
  }

  if (native_mode) {
    node.set_property("discovery.native", "1");
  }
}

template <typename SerializerT>
inline void serialize_str_list(SerializerT& s, std::vector<std::string>& items, size_t max_items, size_t max_len) {
  const auto list = bounded(s, items, max_items);
  s.container(list.data, list.maxSize,
              [max_len](auto& item_ser, auto& str) { item_ser(bounded(item_ser, str, max_len)); });
}

template <typename OutputT, typename ContextT>
inline void serialize_str_map(bitsery::Serializer<OutputT, ContextT>& ser,
                              std::unordered_map<std::string, std::string>& map, size_t max_items, size_t max_len) {
  if VUNLIKELY (map.size() > max_items) {
    ser.template context<bool>() = false;
    return;
  }

  auto count = static_cast<uint32_t>(map.size());
  ser.value4b(count);

  for (auto& kv : map) {
    auto& k = const_cast<std::string&>(kv.first);
    ser(bounded(ser, k, max_len));
    ser(bounded(ser, kv.second, max_len));
  }
}

template <typename InputT, typename ContextT>
inline void serialize_str_map(bitsery::Deserializer<InputT, ContextT>& ser,
                              std::unordered_map<std::string, std::string>& map, size_t max_items, size_t max_len) {
  uint32_t count = 0U;
  ser.value4b(count);

  if VUNLIKELY (ser.adapter().error() != bitsery::ReaderError::NoError) {
    return;
  }

  if VUNLIKELY (count > max_items) {
    ser.adapter().error(bitsery::ReaderError::InvalidData);
    return;
  }

  map.clear();
  map.reserve(count);

  for (uint32_t i = 0U; i < count; ++i) {
    std::string k;
    std::string v;
    ser(bounded(ser, k, max_len));
    ser(bounded(ser, v, max_len));

    if VUNLIKELY (ser.adapter().error() != bitsery::ReaderError::NoError) {
      return;
    }

    map.emplace(std::move(k), std::move(v));
  }
}

template <typename T>
inline bool encode_msg(const T& msg, Bytes& out) noexcept {
  out.clear();

  try {
    bool valid = true;
    const size_t written = bitsery::quickSerialization(valid, OutputAdapter{out}, msg);

    if VUNLIKELY (!valid) {
      out.clear();
      VLOG_E("LauncherProto: Payload exceeds field size limit.");
      return false;
    }

    (void)out.shrink_to(written);
    return true;
  } catch (const std::exception& e) {
    out.clear();
    VLOG_E("LauncherProto: Encode failed: ", e.what(), ".");
    return false;
  } catch (...) {
    out.clear();
    VLOG_E("LauncherProto: Encode failed with unknown exception.");
    return false;
  }
}

template <typename T>
inline bool decode_msg(const Bytes& in, T& msg) noexcept {
  msg = T{};

  if VUNLIKELY (in.empty() || in.data() == nullptr) {
    return false;
  }

  try {
    const auto state = bitsery::quickDeserialization<InputAdapter>(InputAdapter{in.data(), in.size()}, msg);

    if VUNLIKELY (state.first != bitsery::ReaderError::NoError || !state.second) {
      msg = T{};
      return false;
    }

    return true;
  } catch (const std::exception& e) {
    msg = T{};
    VLOG_E("LauncherProto: Decode failed: ", e.what(), ".");
    return false;
  } catch (...) {
    msg = T{};
    VLOG_E("LauncherProto: Decode failed with unknown exception.");
    return false;
  }
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
  s.ext(o, bitsery::ext::Growable{}, [](S& ser, ParamEntry& item) {
    ser(bounded(ser, item.key, kMaxShortStr));
    ser(item.type);
    ser(bounded(ser, item.value, kMaxDetailStr));
  });
}

template <typename S>
inline void serialize(S& s, UnitStatus& o) {
  s.ext(o, bitsery::ext::Growable{}, [](S& ser, UnitStatus& u) {
    ser(bounded(ser, u.name, kMaxShortStr));
    ser(bounded(ser, u.ns, kMaxShortStr));
    ser(bounded(ser, u.group, kMaxShortStr));
    ser(bounded(ser, u.task_group, kMaxShortStr));
    ser(bounded(ser, u.slice, kMaxShortStr));
    ser(bounded(ser, u.cgroup_path, kMaxShortStr));
    ser(bounded(ser, u.container, kMaxShortStr));
    ser(u.type);
    ser(u.state);
    ser(u.stage);
    ser(u.pid);
    ser(u.exit_code);
    ser(u.restart_count);
    ser(u.liveness_failures);
    ser(u.thread_count);
    ser(u.worker_index);
    ser(u.start_time_ms);
    ser(u.uptime_ms);
    ser(u.queued_offset_ms);
    ser(u.started_offset_ms);
    ser(u.ready_offset_ms);
    ser(u.startup_duration_ms);
    ser(u.generation);
    ser(u.memory_rss_kb);
    ser(u.cgroup_memory_kb);
    ser(u.cpu_usage);
    ser(u.healthy);
    ser(u.required);
    ser(bounded(ser, u.failed_reason, kMaxShortStr));
    ser(bounded(ser, u.blocker_unit, kMaxShortStr));
    ser(bounded(ser, u.detail, kMaxDetailStr));
    serialize_str_list(ser, u.dependencies, kMaxDepCount, kMaxShortStr);
    serialize_str_list(ser, u.wants, kMaxDepCount, kMaxShortStr);
    serialize_str_list(ser, u.binds_to, kMaxDepCount, kMaxShortStr);
    serialize_str_list(ser, u.conflicts, kMaxDepCount, kMaxShortStr);
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
    ser(snap.boot_duration_ms);
    ser(snap.cpu_usage);
    ser(snap.memory_usage);
    ser(bounded(ser, snap.startup_mode, kMaxShortStr));
    ser(snap.max_parallel);
    ser(bounded(ser, snap.log_dir, kMaxDetailStr));
    ser(bounded(ser, snap.config_file, kMaxDetailStr));
    const auto list = bounded(ser, snap.units, kMaxUnitCount);
    ser.container(list.data, list.maxSize);
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
inline void serialize(S& s, LogRecord& o) {
  s.ext(o, bitsery::ext::Growable{}, [](S& ser, LogRecord& rec) {
    ser(rec.seq);
    ser(rec.timestamp_ms);
    ser(bounded(ser, rec.unit_name, kMaxShortStr));
    ser(rec.level);
    ser(rec.is_stderr);
    ser(bounded(ser, rec.text, kMaxLogStr));
  });
}

template <typename S>
inline void serialize(S& s, LogQuery& o) {
  s.ext(o, bitsery::ext::Growable{}, [](S& ser, LogQuery& q) {
    ser(bounded(ser, q.unit_filter, kMaxShortStr));
    ser(bounded(ser, q.grep, kMaxShortStr));
    ser(q.after_seq);
    ser(q.min_level);
    ser(q.stderr_only);
    ser(q.limit);
  });
}

template <typename SerializerT>
inline void serialize(SerializerT& s, LoadComponentRequest& o) {
  s.ext(o, bitsery::ext::Growable{}, [](SerializerT& ser, LoadComponentRequest& req) {
    ser(bounded(ser, req.name, kMaxShortStr));
    ser(bounded(ser, req.ns, kMaxShortStr));
    ser(bounded(ser, req.library, kMaxDetailStr));
    ser(bounded(ser, req.class_name, kMaxShortStr));
    ser(bounded(ser, req.scheduler_group, kMaxShortStr));
    ser(bounded(ser, req.compute_pool, kMaxShortStr));
    ser(bounded(ser, req.container, kMaxShortStr));
    ser(req.worker_index);
    ser(req.task_priority);
    ser(req.legacy_runnable);
    ser(req.auto_start);
    const auto plist = bounded(ser, req.params, kMaxParamCount);
    ser.container(plist.data, plist.maxSize);
    serialize_str_map(ser, req.remaps, kMaxRemapCount, kMaxShortStr);
    serialize_str_map(ser, req.urls, kMaxRemapCount, kMaxShortStr);
    serialize_str_map(ser, req.inputs, kMaxRemapCount, kMaxShortStr);
    serialize_str_map(ser, req.outputs, kMaxRemapCount, kMaxShortStr);
  });
}

template <typename S>
inline void CommandRequest::serialize(S& s) {
  s.ext(*this, bitsery::ext::Growable{}, [](S& ser, CommandRequest& o) {
    ser(o.proto_version);
    ser(o.op);
    ser(bounded(ser, o.target, kMaxShortStr));
    ser(o.load_req);
    ser(o.param);
    ser(o.log_query);
    ser(o.record_enable);
    ser(bounded(ser, o.record_path, kMaxDetailStr));
  });
}

inline bool CommandRequest::operator>>(Bytes& des) const noexcept { return encode_msg(*this, des); }

inline bool CommandRequest::operator<<(const Bytes& src) noexcept { return decode_msg(src, *this); }

template <typename S>
inline void CommandResponse::serialize(S& s) {
  s.ext(*this, bitsery::ext::Growable{}, [](S& ser, CommandResponse& o) {
    ser(o.proto_version);
    ser(o.result.code);
    ser(bounded(ser, o.result.message, kMaxConfigStr));
    const auto plist = bounded(ser, o.result.params, kMaxParamCount);
    ser.container(plist.data, plist.maxSize);
    const auto llist = bounded(ser, o.result.logs, kMaxLogBatch);
    ser.container(llist.data, llist.maxSize);
    ser(o.has_snapshot);

    if (o.has_snapshot) {
      ser(o.result.snapshot);
    }
  });
}

inline bool CommandResponse::operator>>(Bytes& des) const noexcept { return encode_msg(*this, des); }

inline bool CommandResponse::operator<<(const Bytes& src) noexcept { return decode_msg(src, *this); }

inline bool operator>>(const SystemSnapshot& src, Bytes& des) noexcept { return encode_msg(src, des); }

inline bool operator<<(SystemSnapshot& des, const Bytes& src) noexcept { return decode_msg(src, des); }

inline bool operator>>(const EventRecord& src, Bytes& des) noexcept { return encode_msg(src, des); }

inline bool operator<<(EventRecord& des, const Bytes& src) noexcept { return decode_msg(src, des); }

inline bool operator>>(const LogRecord& src, Bytes& des) noexcept { return encode_msg(src, des); }

inline bool operator<<(LogRecord& des, const Bytes& src) noexcept { return decode_msg(src, des); }

}  // namespace launcher

}  // namespace vlink
