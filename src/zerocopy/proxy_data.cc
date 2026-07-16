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

#include "./zerocopy/proxy_data.h"

#include <cstdint>
#include <limits>
#include <string_view>

#include "./zerocopy/header.h"

namespace vlink {

namespace zerocopy {

// ProxyData
ProxyData::ProxyData() noexcept {
#if defined(__arm__) || defined(__x86__) || defined(__i386__)
#ifndef __ANDROID__
#warning "[ProxyData] No support for 32-bit architecture."
#endif
#else
  static_assert(sizeof(ProxyData) == 80, "Sizeof must be 80 bytes.");
#endif
}

ProxyData::~ProxyData() noexcept {
  if (is_owner_ && data_ && size_ != 0) {
    Bytes::bytes_free(data_, size_);
  }
}

ProxyData::ProxyData(const ProxyData& target) noexcept { deep_copy(target); }

ProxyData::ProxyData(ProxyData&& target) noexcept { move_copy(target); }

ProxyData& ProxyData::operator=(const ProxyData& target) noexcept {
  if VUNLIKELY (this == &target) {
    return *this;
  }

  deep_copy(target);

  return *this;
}

ProxyData& ProxyData::operator=(ProxyData&& target) noexcept {
  if VUNLIKELY (this == &target) {
    return *this;
  }

  move_copy(target);

  return *this;
}

bool ProxyData::operator<<(const Bytes& bytes) noexcept {
  static constexpr size_t kMagicNumberBeginSize = sizeof(kMagicNumberBegin);
  static constexpr size_t kVersionSize = sizeof(kWireVersion);
  // static constexpr size_t kMagicNumberEndSize = sizeof(kMagicNumberEnd);

  if VUNLIKELY (bytes.empty()) {
    return false;
  }

  if VUNLIKELY (!check_valid(bytes)) {
    return false;
  }

  uint32_t wire_version = 0;
  std::memcpy(&wire_version, bytes.data() + kMagicNumberBeginSize, kVersionSize);

  if VUNLIKELY (version_major(wire_version) != version_major(kWireVersion)) {
    return false;
  }

  if (is_owner_ && data_ && size_ != 0) {
    Bytes::bytes_free(data_, size_);
  }

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wclass-memaccess"
#if __GNUC__ >= 11
#pragma GCC diagnostic ignored "-Wstringop-overread"
#endif
#endif

  auto* target_ptr = reinterpret_cast<uint8_t*>(this);

  std::memcpy(target_ptr, bytes.data() + kMagicNumberBeginSize + kVersionSize, sizeof(ProxyData));

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

  data_ = const_cast<uint8_t*>(bytes.data() + kMagicNumberBeginSize + kVersionSize + sizeof(ProxyData));
  is_owner_ = false;

  if VUNLIKELY (bytes.size() != get_serialized_size()) {
    clear();
    return false;
  }

  if VUNLIKELY (static_cast<uint64_t>(data_pos_) + data_size_ != url_pos_) {
    clear();
    return false;
  }

  if VUNLIKELY (static_cast<uint64_t>(url_pos_) + url_size_ != ser_pos_) {
    clear();
    return false;
  }

  if VUNLIKELY (static_cast<uint64_t>(ser_pos_) + ser_size_ != hostname_pos_) {
    clear();
    return false;
  }

  if VUNLIKELY (static_cast<uint64_t>(hostname_pos_) + hostname_size_ != size_) {
    clear();
    return false;
  }

  return true;
}

bool ProxyData::operator>>(Bytes& bytes) const noexcept {
  static constexpr size_t kMagicNumberBeginSize = sizeof(kMagicNumberBegin);
  static constexpr size_t kVersionSize = sizeof(kWireVersion);
  static constexpr size_t kMagicNumberEndSize = sizeof(kMagicNumberEnd);

  if (bytes.empty() || bytes.size() != get_serialized_size()) {
    bytes = Bytes::create(get_serialized_size());

    if VUNLIKELY (bytes.empty()) {
      return false;
    }
  }

  std::memcpy(bytes.data(), &kMagicNumberBegin, kMagicNumberBeginSize);

  std::memcpy(bytes.data() + kMagicNumberBeginSize, &kWireVersion, kVersionSize);

  // NOLINTNEXTLINE(bugprone-undefined-memory-manipulation)
  std::memcpy(bytes.data() + kMagicNumberBeginSize + kVersionSize, this, sizeof(ProxyData));

  const auto data_offset = reinterpret_cast<const uint8_t*>(&data_) - reinterpret_cast<const uint8_t*>(this);
  std::memset(bytes.data() + kMagicNumberBeginSize + kVersionSize + data_offset, 0, sizeof(data_));

  if VLIKELY (data_ != nullptr && size_ != 0) {
    std::memcpy(bytes.data() + kMagicNumberBeginSize + kVersionSize + sizeof(ProxyData), data_, size_);
  }

  std::memcpy(bytes.data() + kMagicNumberBeginSize + kVersionSize + sizeof(ProxyData) + size_, &kMagicNumberEnd,
              kMagicNumberEndSize);

  return true;
}

uint32_t ProxyData::control_id() const noexcept { return control_id_; }

uint32_t ProxyData::mode() const noexcept { return mode_; }

int64_t ProxyData::timestamp() const noexcept { return timestamp_; }

int64_t ProxyData::seq() const noexcept { return seq_; }

uint32_t ProxyData::schema() const noexcept { return schema_; }

Bytes ProxyData::raw() const noexcept {
  if VUNLIKELY (!data_ || size_ == 0 || data_size_ == 0) {
    return Bytes();
  }

  return Bytes::shallow_copy(data_ + data_pos_, data_size_);
}

std::string_view ProxyData::url() const noexcept {
  if VUNLIKELY (!data_ || size_ == 0 || url_size_ == 0) {
    return std::string_view();
  }

  return std::string_view(reinterpret_cast<char*>(data_) + url_pos_, url_size_);
}

std::string_view ProxyData::ser() const noexcept {
  if VUNLIKELY (!data_ || size_ == 0 || ser_size_ == 0) {
    return std::string_view();
  }

  return std::string_view(reinterpret_cast<char*>(data_) + ser_pos_, ser_size_);
}

std::string_view ProxyData::hostname() const noexcept {
  if VUNLIKELY (!data_ || size_ == 0 || hostname_size_ == 0) {
    return std::string_view();
  }

  return std::string_view(reinterpret_cast<char*>(data_) + hostname_pos_, hostname_size_);
}

void ProxyData::set_control_id(uint32_t control_id) noexcept { control_id_ = control_id; }

void ProxyData::set_mode(uint32_t mode) noexcept { mode_ = mode; }

void ProxyData::set_timestamp(int64_t timestamp) noexcept { timestamp_ = timestamp; }

void ProxyData::set_seq(int64_t seq) noexcept { seq_ = seq; }

void ProxyData::set_schema(uint32_t schema) noexcept { schema_ = schema; }

bool ProxyData::check_valid(const Bytes& bytes) noexcept {
  static constexpr size_t kMagicNumberBeginSize = sizeof(kMagicNumberBegin);
  static constexpr size_t kVersionSize = sizeof(kWireVersion);
  static constexpr size_t kMagicNumberEndSize = sizeof(kMagicNumberEnd);

  if VUNLIKELY (bytes.size() < kMagicNumberBeginSize + kVersionSize + sizeof(ProxyData) + kMagicNumberEndSize) {
    return false;
  }

  uint32_t check_magic = 0;

  std::memcpy(&check_magic, bytes.begin(), kMagicNumberBeginSize);

  if VUNLIKELY (check_magic != kMagicNumberBegin) {
    return false;
  }

  uint32_t wire_version = 0;
  std::memcpy(&wire_version, bytes.data() + kMagicNumberBeginSize, kVersionSize);

  if VUNLIKELY (version_major(wire_version) != version_major(kWireVersion)) {
    return false;
  }

  std::memcpy(&check_magic, bytes.end() - kMagicNumberEndSize, kMagicNumberEndSize);

  if VUNLIKELY (check_magic != kMagicNumberEnd) {
    return false;
  }

  return true;
}

size_t ProxyData::get_serialized_size() const noexcept {
  static constexpr size_t kMagicNumberBeginSize = sizeof(kMagicNumberBegin);
  static constexpr size_t kVersionSize = sizeof(kWireVersion);
  static constexpr size_t kMagicNumberEndSize = sizeof(kMagicNumberEnd);

  return kMagicNumberBeginSize + kVersionSize + sizeof(ProxyData) + size_ + kMagicNumberEndSize;
}

bool ProxyData::is_valid() const noexcept {
  if VUNLIKELY (static_cast<uint64_t>(data_pos_) + data_size_ != url_pos_) {
    return false;
  }

  if VUNLIKELY (static_cast<uint64_t>(url_pos_) + url_size_ != ser_pos_) {
    return false;
  }

  if VUNLIKELY (static_cast<uint64_t>(ser_pos_) + ser_size_ != hostname_pos_) {
    return false;
  }

  if VUNLIKELY (static_cast<uint64_t>(hostname_pos_) + hostname_size_ != size_) {
    return false;
  }

  return data_ != nullptr && size_ != 0;
}

bool ProxyData::shallow_copy(const ProxyData& target) noexcept {
  if VUNLIKELY (this == &target) {
    return false;
  }

  if (is_owner_ && data_ && size_ != 0) {
    const auto current = reinterpret_cast<uintptr_t>(data_);
    const auto source = reinterpret_cast<uintptr_t>(target.data_);

    if VUNLIKELY (source >= current && source - current < size_) {
      return false;
    }

    Bytes::bytes_free(data_, size_);
  }

  control_id_ = target.control_id_;
  mode_ = target.mode_;
  timestamp_ = target.timestamp_;
  seq_ = target.seq_;
  schema_ = target.schema_;

  data_pos_ = target.data_pos_;
  data_size_ = target.data_size_;

  url_pos_ = target.url_pos_;
  url_size_ = target.url_size_;

  ser_pos_ = target.ser_pos_;
  ser_size_ = target.ser_size_;

  hostname_pos_ = target.hostname_pos_;
  hostname_size_ = target.hostname_size_;

  reserved_buf_ = target.reserved_buf_;
  reserved_buf2_ = target.reserved_buf2_;

  size_ = target.size_;
  data_ = target.data_;

  is_owner_ = false;

  return true;
}

bool ProxyData::deep_copy(const ProxyData& target) noexcept {
  if VLIKELY (data_ && is_owner_ && target.data_ && size_ != 0 && size_ == target.size_) {
    const auto current = reinterpret_cast<uintptr_t>(data_);
    const auto source = reinterpret_cast<uintptr_t>(target.data_);

    if VUNLIKELY (source >= current && source - current < size_) {
      return false;
    }

    control_id_ = target.control_id_;
    mode_ = target.mode_;
    timestamp_ = target.timestamp_;
    seq_ = target.seq_;
    schema_ = target.schema_;

    data_pos_ = target.data_pos_;
    data_size_ = target.data_size_;

    url_pos_ = target.url_pos_;
    url_size_ = target.url_size_;

    ser_pos_ = target.ser_pos_;
    ser_size_ = target.ser_size_;

    hostname_pos_ = target.hostname_pos_;
    hostname_size_ = target.hostname_size_;

    reserved_buf_ = target.reserved_buf_;
    reserved_buf2_ = target.reserved_buf2_;

    std::memcpy(data_, target.data_, size_);

    return true;
  }

  if VUNLIKELY (!shallow_copy(target)) {
    return false;
  }

  if (data_ && size_ != 0) {
    auto* target_data = data_;
    data_ = Bytes::bytes_malloc(size_);

    if VUNLIKELY (!data_) {
      clear();
      return false;
    }

    std::memcpy(data_, target_data, size_);
    is_owner_ = true;
  }

  return true;
}

bool ProxyData::move_copy(ProxyData& target) noexcept {
  if VUNLIKELY (!shallow_copy(target)) {
    return false;
  }

  is_owner_ = target.is_owner_;

  target.control_id_ = 0;
  target.mode_ = 0;
  target.timestamp_ = 0;
  target.seq_ = 0;
  target.schema_ = 0;

  target.data_pos_ = 0;
  target.data_size_ = 0;

  target.url_pos_ = 0;
  target.url_size_ = 0;

  target.ser_pos_ = 0;
  target.ser_size_ = 0;

  target.hostname_pos_ = 0;
  target.hostname_size_ = 0;

  target.reserved_buf_ = 0;
  target.reserved_buf2_ = 0;

  target.size_ = 0;
  target.data_ = nullptr;
  target.is_owner_ = false;

  return true;
}

void ProxyData::create(const Bytes& raw, std::string_view url, std::string_view ser, uint32_t schema,
                       std::string_view hostname) noexcept {
  static constexpr uint64_t kMax = std::numeric_limits<uint32_t>::max();

  if VUNLIKELY (raw.size() > kMax || url.size() > kMax || ser.size() > kMax || hostname.size() > kMax) {
    clear();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return;   // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  const uint64_t total = static_cast<uint64_t>(raw.size()) + url.size() + ser.size() + hostname.size();

  if VUNLIKELY (total > kMax) {
    clear();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return;   // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  bool aliases_owned = false;

  if (is_owner_ && data_ && size_ != 0) {
    const auto current = reinterpret_cast<uintptr_t>(data_);
    const auto raw_source = reinterpret_cast<uintptr_t>(raw.data());
    const auto url_source = reinterpret_cast<uintptr_t>(url.data());
    const auto ser_source = reinterpret_cast<uintptr_t>(ser.data());
    const auto hostname_source = reinterpret_cast<uintptr_t>(hostname.data());

    aliases_owned = (!raw.empty() && raw_source >= current && raw_source - current < size_) ||
                    (!url.empty() && url_source >= current && url_source - current < size_) ||
                    (!ser.empty() && ser_source >= current && ser_source - current < size_) ||
                    (!hostname.empty() && hostname_source >= current && hostname_source - current < size_);

    if VLIKELY (!aliases_owned) {
      Bytes::bytes_free(data_, size_);
      data_ = nullptr;
      size_ = 0;
      is_owner_ = false;
    }
  }

  const auto data_size = static_cast<uint32_t>(raw.size());
  const uint32_t url_pos = data_size;
  const auto url_size = static_cast<uint32_t>(url.size());
  const uint32_t ser_pos = url_pos + url_size;
  const auto ser_size = static_cast<uint32_t>(ser.size());
  const uint32_t hostname_pos = ser_pos + ser_size;
  const auto hostname_size = static_cast<uint32_t>(hostname.size());

  uint8_t* new_data = nullptr;
  if VLIKELY (total != 0) {
    new_data = Bytes::bytes_malloc(total);

    if VUNLIKELY (!new_data) {
      clear();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      return;   // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if VLIKELY (data_size != 0) {
      std::memcpy(new_data, raw.data(), data_size);
    }

    if (url_size != 0) {
      std::memcpy(new_data + url_pos, url.data(), url_size);
    }

    if (ser_size != 0) {
      std::memcpy(new_data + ser_pos, ser.data(), ser_size);
    }

    if (hostname_size != 0) {
      std::memcpy(new_data + hostname_pos, hostname.data(), hostname_size);
    }
  }

  if VUNLIKELY (aliases_owned) {
    Bytes::bytes_free(data_, size_);
  }

  schema_ = schema;

  data_pos_ = 0;
  data_size_ = data_size;

  url_pos_ = url_pos;
  url_size_ = url_size;

  ser_pos_ = ser_pos;
  ser_size_ = ser_size;

  hostname_pos_ = hostname_pos;
  hostname_size_ = hostname_size;

  size_ = total;
  data_ = new_data;
  is_owner_ = new_data != nullptr;
}

void ProxyData::clear() noexcept {
  if (is_owner_ && data_ && size_ != 0) {
    Bytes::bytes_free(data_, size_);
  }

  control_id_ = 0;
  mode_ = 0;
  timestamp_ = 0;
  seq_ = 0;
  schema_ = 0;

  data_pos_ = 0;
  data_size_ = 0;

  url_pos_ = 0;
  url_size_ = 0;

  ser_pos_ = 0;
  ser_size_ = 0;

  hostname_pos_ = 0;
  hostname_size_ = 0;

  reserved_buf_ = 0;
  reserved_buf2_ = 0;

  size_ = 0;
  data_ = nullptr;
  is_owner_ = false;
}

size_t ProxyData::size() const noexcept { return size_; }

bool ProxyData::is_owner() const noexcept { return is_owner_; }

uint8_t& ProxyData::get_reserved() noexcept { return reserved_buf_; }

uint16_t& ProxyData::get_reserved2() noexcept { return reserved_buf2_; }

}  // namespace zerocopy

}  // namespace vlink
