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
#include <vlink/external/proxy_api.h>
//
#include <bitsery/adapter/buffer.h>
#include <bitsery/bitsery.h>
#include <bitsery/brief_syntax.h>
#include <bitsery/brief_syntax/string.h>
#include <bitsery/brief_syntax/vector.h>
#include <bitsery/ext/growable.h>
#include <bitsery/traits/vector.h>
//
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define VLINK_PROXY_ENABLE_FILTER 1
#define VLINK_PROXY_ENABLE_HANDSHAKE 1

namespace bitsery {

namespace traits {

template <>
struct ContainerTraits<vlink::Bytes> {
  using TValue = uint8_t;
  static constexpr bool isResizable = true;   // NOLINT(readability-identifier-naming)
  static constexpr bool isContiguous = true;  // NOLINT(readability-identifier-naming)

  static size_t size(const vlink::Bytes& c) noexcept { return c.size(); }
};

template <>
struct BufferAdapterTraits<vlink::Bytes> {
  using TIterator = uint8_t*;
  using TConstIterator = const uint8_t*;
  using TValue = uint8_t;

  // NOLINTNEXTLINE(readability-identifier-naming)
  static void increaseBufferSize(vlink::Bytes& c, size_t currSize, size_t minSize) {
    const size_t cap = c.capacity();
    size_t target;

    if (cap == 0) {
      target = std::max<size_t>(minSize, vlink::Bytes::stack_size());
    } else {
      target = cap + (cap >> 1) + 64;
      target -= target % 64;

      if (target < minSize) {
        target = (minSize + 63) & ~size_t{63};
      }
    }

    if VUNLIKELY (!c.is_owner()) {
      auto new_buf = vlink::Bytes::create(target);

      if VUNLIKELY (!new_buf.is_owner()) {
        throw std::bad_alloc{};
      }

      if (currSize > 0 && c.data() != nullptr) {
        std::memcpy(new_buf.data(), c.data(), currSize);
      }

      c = std::move(new_buf);
    } else if VUNLIKELY (!c.resize(target)) {
      throw std::bad_alloc{};
    }
  }
};

}  // namespace traits

}  // namespace bitsery

namespace vlink {

namespace proxy {

[[maybe_unused]] static constexpr std::string_view kSocketBufStr = "8388608";
[[maybe_unused]] static constexpr std::string_view kSocketMtuStr = "65500";

[[maybe_unused]] static constexpr std::string_view kDataUrlCtx = "://proxy/proxy_data/v3?qos=better&domain=";
[[maybe_unused]] static constexpr std::string_view kViewerDataUrlCtx = "://proxy/viewer_data/v3?qos=better&domain=";

[[maybe_unused]] static constexpr std::string_view kDataReliableUrlCtx =
    "://proxy/proxy_data/reliable/v3?qos=large&domain=";
[[maybe_unused]] static constexpr std::string_view kViewerDataReliableUrlCtx =
    "://proxy/viewer_data/reliable/v3?qos=large&domain=";

[[maybe_unused]] static constexpr std::string_view kTimeUrlCtx = "://proxy/time/v3?qos=clock&domain=";
[[maybe_unused]] static constexpr std::string_view kInfoListUrlCtx = "://proxy/info_list/v3?qos=poor&domain=";
[[maybe_unused]] static constexpr std::string_view kControlUrlCtx = "://proxy/control/v3?qos=best&domain=";
[[maybe_unused]] static constexpr std::string_view kHandshakeUrlCtx = "://proxy/handshake/v3?qos=best&domain=";

[[maybe_unused]] static constexpr std::string_view kDataShmUrlCtx = "://proxy/proxy_data/v3?domain=";
[[maybe_unused]] static constexpr std::string_view kViewerDataShmUrlCtx = "://proxy/viewer_data/v3?domain=";

[[maybe_unused]] static constexpr uint32_t kHandshakeWaitMs = 800;
[[maybe_unused]] static constexpr uint32_t kHandshakeInvokeMs = 800;

[[maybe_unused]] static constexpr size_t kMaxStringSize = 256;
[[maybe_unused]] static constexpr size_t kMaxTokenSize = 1024;
[[maybe_unused]] static constexpr size_t kMaxUrlSize = 1024;
[[maybe_unused]] static constexpr size_t kMaxFilterSize = 4096;
[[maybe_unused]] static constexpr size_t kMaxTopicListSize = 4096;
[[maybe_unused]] static constexpr size_t kMaxProcessListSize = 256;

enum HandshakeResult : uint8_t {
  kHandshakeOk = 0,
  kHandshakeVersionMismatch = 1,
  kHandshakeInternalError = 2,
};

[[nodiscard]] inline std::string make_url(std::string_view scheme, std::string_view ctx, std::string_view domain) {
  std::string out;

  out.reserve(scheme.size() + ctx.size() + domain.size());

  out.append(scheme).append(ctx).append(domain);

  return out;
}

using OutputAdapter = bitsery::OutputBufferAdapter<Bytes>;
using InputAdapter = bitsery::InputBufferAdapter<const uint8_t*>;

template <typename T>
inline bool encode_to_bytes(const T& src, Bytes& des) noexcept {
  des.clear();

  try {
    const size_t written = bitsery::quickSerialization<OutputAdapter>(des, src);

    (void)des.shrink_to(written);

    return true;
  } catch (const std::exception& e) {
    des.clear();
    VLOG_T("ProxyCommon: Bitsery serialize failed: ", e.what(), ".");
    return false;
  }
}

template <typename T>
inline bool decode_from_bytes(const Bytes& src, T& des) noexcept {
  if VUNLIKELY (src.empty()) {
    des = T{};
    VLOG_T("ProxyCommon: Empty payload, returning default-constructed value.");
    return true;
  }

  try {
    const auto state = bitsery::quickDeserialization<InputAdapter>(InputAdapter{src.data(), src.size()}, des);

    if VUNLIKELY (state.first != bitsery::ReaderError::NoError || !state.second) {
      des = T{};
      VLOG_T("ProxyCommon: Bitsery deserialize failed: error=", static_cast<int>(state.first),
             ", completed=", state.second, ".");
      return false;
    }

    return true;
  } catch (const std::exception& e) {
    des = T{};
    VLOG_T("ProxyCommon: Bitsery deserialize failed: ", e.what(), ".");
    return false;
  }
}

}  // namespace proxy

static_assert(sizeof(ProxyAPI::Mode) == 1, "wire format: ProxyAPI::Mode must remain 1 byte");
static_assert(sizeof(ProxyAPI::Status) == 1, "wire format: ProxyAPI::Status must remain 1 byte");
static_assert(sizeof(SchemaType) == 1, "wire format: SchemaType must remain 1 byte");
static_assert(sizeof(ImplType) == 1, "wire format: ImplType must remain 1 byte");
static_assert(sizeof(proxy::HandshakeResult) == 1, "wire format: HandshakeResult must remain 1 byte");

template <typename S>
inline void serialize(S& s, ProxyAPI::Process& msg) {
  s(msg.type, bitsery::maxSize(msg.host, proxy::kMaxStringSize), msg.pid,
    bitsery::maxSize(msg.name, proxy::kMaxStringSize), bitsery::maxSize(msg.ip, proxy::kMaxStringSize));
}

template <typename S>
inline void serialize(S& s, ProxyAPI::UrlMeta& msg) {
  s(bitsery::maxSize(msg.url, proxy::kMaxUrlSize), bitsery::maxSize(msg.ser, proxy::kMaxUrlSize), msg.schema, msg.type);
}

template <typename S>
inline void serialize(S& s, ProxyAPI::Info& msg) {
  s(msg.type, bitsery::maxSize(msg.url, proxy::kMaxUrlSize), bitsery::maxSize(msg.ser, proxy::kMaxUrlSize), msg.schema,
    msg.status, msg.freq, msg.rate, msg.loss, msg.latency,
    bitsery::maxSize(msg.process_list, proxy::kMaxProcessListSize));
}

template <typename S>
inline void serialize(S& s, ProxyAPI::Control& msg) {
  s(msg.mode, bitsery::maxSize(msg.url_meta_list, proxy::kMaxTopicListSize), msg.filter_by_process,
    bitsery::maxSize(msg.filter_str, proxy::kMaxFilterSize), msg.filter_type);
}

namespace proxy {

struct ControlPacket final {
  uint32_t control_id{0};
  std::string token;
  ProxyAPI::Control body;

  template <typename S>
  void serialize(S& s) {
    s.ext(*this, bitsery::ext::Growable{}, [](S& sub, ControlPacket& obj) {
      sub(obj.control_id, bitsery::maxSize(obj.token, kMaxTokenSize), obj.body);
    });
  }

  bool operator>>(Bytes& des) const noexcept { return encode_to_bytes(*this, des); }
  bool operator<<(const Bytes& src) noexcept { return decode_from_bytes(src, *this); }
};

struct InfoListPacket final {
  uint32_t control_id{0};
  std::string hostname;
  std::vector<ProxyAPI::Info> info_list;

  template <typename S>
  void serialize(S& s) {
    s.ext(*this, bitsery::ext::Growable{}, [](S& sub, InfoListPacket& obj) {
      sub(obj.control_id, bitsery::maxSize(obj.hostname, kMaxStringSize),
          bitsery::maxSize(obj.info_list, kMaxTopicListSize));
    });
  }

  bool operator>>(Bytes& des) const noexcept { return encode_to_bytes(*this, des); }
  bool operator<<(const Bytes& src) noexcept { return decode_from_bytes(src, *this); }
};

struct TimePacket final {
  uint32_t control_id{0};
  ProxyAPI::Mode mode{ProxyAPI::kOffline};
  uint64_t sys_time{0};
  uint64_t boot_time{0};
  bool reliable_mode{false};
  bool tcp_mode{false};
  bool direct_mode{false};
  std::string version;
  std::string hostname;
  std::string machine_id;
  double cpu_usage{0.0};
  double memory_usage{0.0};
  std::string token;

  template <typename S>
  void serialize(S& s) {
    s.ext(*this, bitsery::ext::Growable{}, [](S& sub, TimePacket& obj) {
      sub(obj.control_id, obj.mode, obj.sys_time, obj.boot_time, obj.reliable_mode, obj.tcp_mode, obj.direct_mode,
          bitsery::maxSize(obj.version, kMaxStringSize), bitsery::maxSize(obj.hostname, kMaxStringSize),
          bitsery::maxSize(obj.machine_id, kMaxStringSize), obj.cpu_usage, obj.memory_usage,
          bitsery::maxSize(obj.token, kMaxTokenSize));
    });
  }

  bool operator>>(Bytes& des) const noexcept { return encode_to_bytes(*this, des); }
  bool operator<<(const Bytes& src) noexcept { return decode_from_bytes(src, *this); }
};

struct HandshakeReqPacket final {
  uint32_t control_id{0};
  std::string version;
  std::string hostname;
  std::string role;

  template <typename S>
  void serialize(S& s) {
    s.ext(*this, bitsery::ext::Growable{}, [](S& sub, HandshakeReqPacket& obj) {
      sub(obj.control_id, bitsery::maxSize(obj.version, kMaxStringSize), bitsery::maxSize(obj.hostname, kMaxStringSize),
          bitsery::maxSize(obj.role, kMaxStringSize));
    });
  }

  bool operator>>(Bytes& des) const noexcept { return encode_to_bytes(*this, des); }
  bool operator<<(const Bytes& src) noexcept { return decode_from_bytes(src, *this); }
};

struct HandshakeRespPacket final {
  HandshakeResult result{kHandshakeOk};
  std::string token;
  std::string version;
  std::string hostname;
  std::string machine_id;

  template <typename S>
  void serialize(S& s) {
    s.ext(*this, bitsery::ext::Growable{}, [](S& sub, HandshakeRespPacket& obj) {
      sub(obj.result, bitsery::maxSize(obj.token, kMaxTokenSize), bitsery::maxSize(obj.version, kMaxStringSize),
          bitsery::maxSize(obj.hostname, kMaxStringSize), bitsery::maxSize(obj.machine_id, kMaxStringSize));
    });
  }

  bool operator>>(Bytes& des) const noexcept { return encode_to_bytes(*this, des); }
  bool operator<<(const Bytes& src) noexcept { return decode_from_bytes(src, *this); }
};

}  // namespace proxy

}  // namespace vlink
