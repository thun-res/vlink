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

#include "./zerocopy/camera_frame.h"

#include <cstdint>

namespace vlink {

namespace zerocopy {

// CameraFrame
CameraFrame::CameraFrame() noexcept {
#if defined(__arm__) || defined(__x86__) || defined(__i386__)
#ifndef __ANDROID__
#warning "[CameraFrame] No support for 32-bit architecture."
#endif
#else
  static_assert(sizeof(CameraFrame) == 80, "Sizeof must be 80 bytes.");
#endif
}

CameraFrame::~CameraFrame() noexcept {
  if (is_owner_ && data_ && size_ != 0) {
    Bytes::bytes_free(data_, size_);
  }
}

CameraFrame::CameraFrame(const CameraFrame& target) noexcept { deep_copy(target); }

CameraFrame::CameraFrame(CameraFrame&& target) noexcept { move_copy(target); }

CameraFrame& CameraFrame::operator=(const CameraFrame& target) noexcept {
  if VUNLIKELY (this == &target) {
    return *this;
  }

  deep_copy(target);

  return *this;
}

CameraFrame& CameraFrame::operator=(CameraFrame&& target) noexcept {
  if VUNLIKELY (this == &target) {
    return *this;
  }

  move_copy(target);

  return *this;
}

bool CameraFrame::operator<<(const Bytes& bytes) noexcept {
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

  std::memcpy(target_ptr, bytes.data() + kMagicNumberBeginSize + kVersionSize, sizeof(CameraFrame));

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

  data_ = const_cast<uint8_t*>(bytes.data() + kMagicNumberBeginSize + kVersionSize + sizeof(CameraFrame));
  is_owner_ = false;

  if VUNLIKELY (bytes.size() != get_serialized_size()) {
    clear();
    return false;
  }

  return true;
}

bool CameraFrame::operator>>(Bytes& bytes) const noexcept {
  static constexpr size_t kMagicNumberBeginSize = sizeof(kMagicNumberBegin);
  static constexpr size_t kVersionSize = sizeof(kWireVersion);
  static constexpr size_t kMagicNumberEndSize = sizeof(kMagicNumberEnd);

  if (bytes.empty() || bytes.size() != get_serialized_size()) {
    bytes = Bytes::create(get_serialized_size());
  }

  std::memcpy(bytes.data(), &kMagicNumberBegin, kMagicNumberBeginSize);

  std::memcpy(bytes.data() + kMagicNumberBeginSize, &kWireVersion, kVersionSize);

  // NOLINTNEXTLINE(bugprone-undefined-memory-manipulation)
  std::memcpy(bytes.data() + kMagicNumberBeginSize + kVersionSize, this, sizeof(CameraFrame));

  if VLIKELY (data_ != nullptr && size_ != 0) {
    std::memcpy(bytes.data() + kMagicNumberBeginSize + kVersionSize + sizeof(CameraFrame), data_, size_);
  }

  std::memcpy(bytes.data() + kMagicNumberBeginSize + kVersionSize + sizeof(CameraFrame) + size_, &kMagicNumberEnd,
              kMagicNumberEndSize);

  return true;
}

bool CameraFrame::check_valid(const Bytes& bytes) noexcept {
  static constexpr size_t kMagicNumberBeginSize = sizeof(kMagicNumberBegin);
  static constexpr size_t kVersionSize = sizeof(kWireVersion);
  static constexpr size_t kMagicNumberEndSize = sizeof(kMagicNumberEnd);

  if VUNLIKELY (bytes.size() < kMagicNumberBeginSize + kVersionSize + sizeof(CameraFrame) + kMagicNumberEndSize) {
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

size_t CameraFrame::get_serialized_size() const noexcept {
  static constexpr size_t kMagicNumberBeginSize = sizeof(kMagicNumberBegin);
  static constexpr size_t kVersionSize = sizeof(kWireVersion);
  static constexpr size_t kMagicNumberEndSize = sizeof(kMagicNumberEnd);

  return kMagicNumberBeginSize + kVersionSize + sizeof(CameraFrame) + size_ + kMagicNumberEndSize;
}

bool CameraFrame::is_valid() const noexcept { return data_ != nullptr && size_ != 0; }

bool CameraFrame::shallow_copy(const CameraFrame& target) noexcept {
  if VUNLIKELY (this == &target) {
    return false;
  }

  if (is_owner_ && data_ && size_ != 0) {
    Bytes::bytes_free(data_, size_);
  }

  header = target.header;

  channel_ = target.channel_;
  height_ = target.height_;
  width_ = target.width_;
  freq_ = target.freq_;
  format_ = target.format_;
  stream_ = target.stream_;
  reserved_buf_ = target.reserved_buf_;
  is_owner_ = false;
  data_ = target.data_;
  size_ = target.size_;

  return true;
}

bool CameraFrame::deep_copy(const CameraFrame& target) noexcept {
  if VLIKELY (data_ && is_owner_ && target.data_ && size_ != 0 && size_ == target.size_) {
    if VUNLIKELY (this == &target) {
      return false;
    }

    header = target.header;

    channel_ = target.channel_;
    height_ = target.height_;
    width_ = target.width_;
    freq_ = target.freq_;
    format_ = target.format_;
    stream_ = target.stream_;
    reserved_buf_ = target.reserved_buf_;

    std::memcpy(data_, target.data_, size_);

    return true;
  }

  if VUNLIKELY (!shallow_copy(target)) {
    return false;
  }

  if (data_ && size_ != 0) {
    data_ = Bytes::bytes_malloc(size_);

    std::memcpy(data_, target.data_, size_);

    is_owner_ = true;
  }

  return true;
}

bool CameraFrame::move_copy(CameraFrame& target) noexcept {
  if VUNLIKELY (!shallow_copy(target)) {
    return false;
  }

  is_owner_ = target.is_owner_;

  target.channel_ = 0;
  target.height_ = 0;
  target.width_ = 0;
  target.freq_ = 0;
  target.format_ = kFormatUnknown;
  target.stream_ = kStreamUnknown;
  target.reserved_buf_ = 0;
  target.is_owner_ = false;
  target.data_ = nullptr;
  target.size_ = 0;

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wclass-memaccess"
#if __GNUC__ >= 11
#pragma GCC diagnostic ignored "-Wstringop-overread"
#endif
#endif

  std::memset(&target.header, 0, sizeof(header));

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

  return true;
}

bool CameraFrame::create(size_t _size) noexcept {
  if VUNLIKELY (_size == 0) {
    return false;
  }

  if (is_owner_ && data_ && size_ != 0) {
    Bytes::bytes_free(data_, size_);
  }

  size_ = _size;

  data_ = Bytes::bytes_malloc(size_);

  is_owner_ = true;

  return true;
}

void CameraFrame::clear() noexcept {
  if (is_owner_ && data_ && size_ != 0) {
    Bytes::bytes_free(data_, size_);
  }

  channel_ = 0;
  height_ = 0;
  width_ = 0;
  freq_ = 0;
  format_ = kFormatUnknown;
  stream_ = kStreamUnknown;
  is_owner_ = false;
  data_ = nullptr;
  size_ = 0;

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wclass-memaccess"
#if __GNUC__ >= 11
#pragma GCC diagnostic ignored "-Wstringop-overread"
#endif
#endif

  std::memset(&header, 0, sizeof(header));

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
}

bool CameraFrame::shallow_copy(uint8_t* data, size_t size) noexcept {
  if VUNLIKELY (!data || size == 0) {
    return false;
  }

  if VUNLIKELY (data_ == data) {
    return false;
  }

  if (is_owner_ && data_ && size_ != 0) {
    Bytes::bytes_free(data_, size_);
  }

  is_owner_ = false;

  data_ = data;
  size_ = size;

  return true;
}

bool CameraFrame::deep_copy(uint8_t* data, size_t size) noexcept {
  if VUNLIKELY (!data || size == 0) {
    return false;
  }

  if (is_owner_) {
    if VUNLIKELY (!data_) {
      return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if VUNLIKELY (data_ == data) {
      return false;
    }

    if VUNLIKELY (size_ != size) {
      create(size);
    }
  } else {
    create(size);
  }

  std::memcpy(data_, data, size);

  return true;
}

bool CameraFrame::fill_data(uint8_t* data, size_t size) noexcept { return deep_copy(data, size); }

uint32_t CameraFrame::channel() const noexcept { return channel_; }

uint32_t CameraFrame::width() const noexcept { return width_; }

uint32_t CameraFrame::height() const noexcept { return height_; }

uint32_t CameraFrame::freq() const noexcept { return freq_; }

CameraFrame::Format CameraFrame::format() const noexcept { return format_; }

CameraFrame::Stream CameraFrame::stream() const noexcept { return stream_; }

const uint8_t* CameraFrame::data() const noexcept { return data_; }

size_t CameraFrame::size() const noexcept { return size_; }

bool CameraFrame::is_owner() const noexcept { return is_owner_; }

void CameraFrame::set_channel(uint32_t channel) noexcept { channel_ = channel; }

void CameraFrame::set_width(uint32_t width) noexcept { width_ = width; }

void CameraFrame::set_height(uint32_t height) noexcept { height_ = height; }

void CameraFrame::set_freq(uint32_t freq) noexcept { freq_ = freq; }

void CameraFrame::set_format(Format format) noexcept { format_ = format; }

void CameraFrame::set_stream(Stream stream) noexcept { stream_ = stream; }

}  // namespace zerocopy

}  // namespace vlink
