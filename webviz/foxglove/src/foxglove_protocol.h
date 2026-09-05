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
#include <vlink/base/macros.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

namespace vlink {
namespace webviz {

[[maybe_unused]] static constexpr std::string_view kSubProtocol = "foxglove.websocket.v1";
[[maybe_unused]] static constexpr std::string_view kCapabilityClientPublish = "clientPublish";
[[maybe_unused]] static constexpr std::string_view kCapabilityServices = "services";
[[maybe_unused]] static constexpr std::string_view kCapabilityParameters = "parameters";
[[maybe_unused]] static constexpr std::string_view kCapabilityParametersSubscribe = "parametersSubscribe";
[[maybe_unused]] static constexpr std::string_view kCapabilityTime = "time";
[[maybe_unused]] static constexpr std::string_view kCapabilityConnectionGraph = "connectionGraph";
[[maybe_unused]] static constexpr std::string_view kCapabilityAssets = "assets";

enum class ServerBinaryOpcode : uint8_t {
  kMessageData = 0x01,
  kTime = 0x02,
  kServiceCallResponse = 0x03,
  kFetchAssetResponse = 0x04,
};

enum class ClientBinaryOpcode : uint8_t {
  kMessageData = 0x01,
  kServiceCallRequest = 0x02,
};

template <typename T>
inline void write_little_endian(uint8_t* data, T value) {
  for (size_t i = 0; i < sizeof(T); ++i) {
    data[i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

template <typename T>
inline T read_little_endian(const uint8_t* data) {
  T value = 0;
  for (size_t i = 0; i < sizeof(T); ++i) {
    value |= static_cast<T>(data[i]) << (8 * i);
  }
  return value;
}

inline std::string encode_base64(const void* data, size_t len) {
  auto bytes = Bytes::shallow_copy(static_cast<const uint8_t*>(data), len);
  return Bytes::encode_to_base64(bytes);
}

inline Bytes build_message_data(uint32_t subscription_id, uint64_t timestamp_ns, const void* payload,
                                size_t payload_len) {
  if VUNLIKELY (payload_len > std::numeric_limits<size_t>::max() - 13) {
    return {};
  }

  auto buf = Bytes::create(1 + 4 + 8 + payload_len);
  auto* ptr = buf.data();

  ptr[0] = static_cast<uint8_t>(ServerBinaryOpcode::kMessageData);
  write_little_endian(ptr + 1, subscription_id);
  write_little_endian(ptr + 5, timestamp_ns);

  if VLIKELY (payload_len > 0) {
    std::memcpy(ptr + 13, payload, payload_len);
  }

  return buf;
}

inline Bytes build_time_message(uint64_t timestamp_ns) {
  auto buf = Bytes::create(1 + 8);
  auto* ptr = buf.data();

  ptr[0] = static_cast<uint8_t>(ServerBinaryOpcode::kTime);
  write_little_endian(ptr + 1, timestamp_ns);

  return buf;
}

inline Bytes build_service_call_response(uint32_t service_id, uint32_t call_id, std::string_view encoding,
                                         const void* payload, size_t payload_len) {
  if VUNLIKELY (encoding.size() > std::numeric_limits<uint32_t>::max() ||
                payload_len > std::numeric_limits<uint32_t>::max()) {
    return {};
  }

  auto enc_len = static_cast<uint32_t>(encoding.size());
  auto buf = Bytes::create(1 + 4 + 4 + 4 + enc_len + payload_len);
  auto* ptr = buf.data();
  size_t offset = 0;

  ptr[offset++] = static_cast<uint8_t>(ServerBinaryOpcode::kServiceCallResponse);

  write_little_endian(ptr + offset, service_id);
  offset += 4;

  write_little_endian(ptr + offset, call_id);
  offset += 4;

  write_little_endian(ptr + offset, enc_len);
  offset += 4;

  std::memcpy(ptr + offset, encoding.data(), enc_len);
  offset += enc_len;

  if VLIKELY (payload_len > 0) {
    std::memcpy(ptr + offset, payload, payload_len);
  }

  return buf;
}

inline Bytes build_fetch_asset_response(uint32_t request_id, uint8_t status, std::string_view error_msg,
                                        const void* data, size_t data_len) {
  if VUNLIKELY (error_msg.size() > std::numeric_limits<uint32_t>::max() ||
                data_len > std::numeric_limits<uint32_t>::max()) {
    return {};
  }

  auto err_len = static_cast<uint32_t>(error_msg.size());
  auto buf = Bytes::create(1 + 4 + 1 + 4 + err_len + data_len);
  auto* ptr = buf.data();
  size_t offset = 0;

  ptr[offset++] = static_cast<uint8_t>(ServerBinaryOpcode::kFetchAssetResponse);

  write_little_endian(ptr + offset, request_id);
  offset += 4;

  ptr[offset++] = status;

  write_little_endian(ptr + offset, err_len);
  offset += 4;

  if VUNLIKELY (err_len > 0) {
    std::memcpy(ptr + offset, error_msg.data(), err_len);
    offset += err_len;
  }

  if VLIKELY (data_len > 0) {
    std::memcpy(ptr + offset, data, data_len);
  }

  return buf;
}

struct ClientBinaryMessage final {
  ClientBinaryOpcode opcode{ClientBinaryOpcode::kMessageData};
  uint32_t channel_or_service_id{0};
  uint32_t call_id{0};
  std::string encoding;
  const uint8_t* payload{nullptr};
  size_t payload_len{0};
};

inline bool parse_client_binary(const uint8_t* data, size_t len, ClientBinaryMessage& out) {
  if VUNLIKELY (len < 1) {
    return false;
  }

  out.opcode = static_cast<ClientBinaryOpcode>(data[0]);

  if VLIKELY (out.opcode == ClientBinaryOpcode::kMessageData) {
    if VUNLIKELY (len < 5) {
      return false;
    }

    out.channel_or_service_id = read_little_endian<uint32_t>(data + 1);
    out.payload = data + 5;
    out.payload_len = len - 5;
    return true;
  }

  if VLIKELY (out.opcode == ClientBinaryOpcode::kServiceCallRequest) {
    if VUNLIKELY (len < 13) {
      return false;
    }

    out.channel_or_service_id = read_little_endian<uint32_t>(data + 1);
    out.call_id = read_little_endian<uint32_t>(data + 5);

    uint32_t enc_len = 0;
    enc_len = read_little_endian<uint32_t>(data + 9);

    if VUNLIKELY (len < 13 || (len - 13) < enc_len) {
      return false;
    }

    out.encoding.assign(reinterpret_cast<const char*>(data + 13), enc_len);
    out.payload = data + 13 + enc_len;
    out.payload_len = len - 13 - enc_len;
    return true;
  }

  return false;
}

}  // namespace webviz
}  // namespace vlink
