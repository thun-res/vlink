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

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace vlink {

namespace zerocopy {

struct CameraFrameFormatEncoding {
  std::string_view name;
  CameraFrame::Format format;
};

static std::string_view normalize_camera_encoding(std::string_view encoding, char (&buffer)[64]) noexcept {
  size_t n = 0;
  for (char c : encoding) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      continue;
    }

    if VUNLIKELY (n + 1 >= sizeof(buffer)) {
      return {};
    }

    buffer[n++] = static_cast<char>(std::tolower(static_cast<unsigned char>(c == '-' ? '_' : c)));
  }

  buffer[n] = '\0';
  return std::string_view(buffer, n);
}

static constexpr CameraFrameFormatEncoding kCameraFrameEncodingAliases[] = {
    {"yuv420", CameraFrame::kFormatYuv420},
    {"i420", CameraFrame::kFormatYuv420},
    {"yuv422", CameraFrame::kFormatYuv422},
    {"yuv444", CameraFrame::kFormatYuv444},
    {"nv12", CameraFrame::kFormatNv12},
    {"nv21", CameraFrame::kFormatNv21},
    {"yuyv", CameraFrame::kFormatYuyv},
    {"yuyv422", CameraFrame::kFormatYuyv},
    {"yuy2", CameraFrame::kFormatYuyv},
    {"yvyu", CameraFrame::kFormatYvyu},
    {"uyvy", CameraFrame::kFormatUyvy},
    {"vyuy", CameraFrame::kFormatVyuy},
    {"bgr8", CameraFrame::kFormatBgr888Packed},
    {"rgb8", CameraFrame::kFormatRgb888Packed},
    {"rgb8_planar", CameraFrame::kFormatRgb888Planar},
    {"mono8", CameraFrame::kFormatMono8},
    {"mono16", CameraFrame::kFormatMono16},
    {"rgba8", CameraFrame::kFormatRgba8888Packed},
    {"bgra8", CameraFrame::kFormatBgra8888Packed},
    {"rgba8888", CameraFrame::kFormatRgba8888Packed},
    {"bgra8888", CameraFrame::kFormatBgra8888Packed},
    {"8uc1", CameraFrame::kFormatUint8C1},
    {"8uc2", CameraFrame::kFormatUint8C2},
    {"8uc3", CameraFrame::kFormatUint8C3},
    {"8uc4", CameraFrame::kFormatUint8C4},
    {"8sc1", CameraFrame::kFormatInt8C1},
    {"8sc2", CameraFrame::kFormatInt8C2},
    {"8sc3", CameraFrame::kFormatInt8C3},
    {"8sc4", CameraFrame::kFormatInt8C4},
    {"16uc1", CameraFrame::kFormatUint16C1},
    {"16uc2", CameraFrame::kFormatUint16C2},
    {"16uc3", CameraFrame::kFormatUint16C3},
    {"16uc4", CameraFrame::kFormatUint16C4},
    {"16sc1", CameraFrame::kFormatInt16C1},
    {"16sc2", CameraFrame::kFormatInt16C2},
    {"16sc3", CameraFrame::kFormatInt16C3},
    {"16sc4", CameraFrame::kFormatInt16C4},
    {"32sc1", CameraFrame::kFormatInt32C1},
    {"32sc2", CameraFrame::kFormatInt32C2},
    {"32sc3", CameraFrame::kFormatInt32C3},
    {"32sc4", CameraFrame::kFormatInt32C4},
    {"32fc1", CameraFrame::kFormatFloat32C1},
    {"32fc2", CameraFrame::kFormatFloat32C2},
    {"32fc3", CameraFrame::kFormatFloat32C3},
    {"32fc4", CameraFrame::kFormatFloat32C4},
    {"64fc1", CameraFrame::kFormatFloat64C1},
    {"64fc2", CameraFrame::kFormatFloat64C2},
    {"64fc3", CameraFrame::kFormatFloat64C3},
    {"64fc4", CameraFrame::kFormatFloat64C4},
    {"bayer_rggb8", CameraFrame::kFormatBayerRggb8},
    {"bayer_bggr8", CameraFrame::kFormatBayerBggr8},
    {"bayer_gbrg8", CameraFrame::kFormatBayerGbrg8},
    {"bayer_grbg8", CameraFrame::kFormatBayerGrbg8},
    {"bayer_rggb16", CameraFrame::kFormatBayerRggb16},
    {"bayer_bggr16", CameraFrame::kFormatBayerBggr16},
    {"bayer_gbrg16", CameraFrame::kFormatBayerGbrg16},
    {"bayer_grbg16", CameraFrame::kFormatBayerGrbg16},
    {"jpeg", CameraFrame::kFormatJpeg},
    {"jpg", CameraFrame::kFormatJpeg},
    {"png", CameraFrame::kFormatPng},
    {"mjpeg", CameraFrame::kFormatMjpeg},
    {"motion_jpeg", CameraFrame::kFormatMjpeg},
    {"h264", CameraFrame::kFormatH264},
    {"avc", CameraFrame::kFormatH264},
    {"h265", CameraFrame::kFormatH265},
    {"hevc", CameraFrame::kFormatH265},
    {"h266", CameraFrame::kFormatH266},
    {"vvc", CameraFrame::kFormatH266},
    {"av1", CameraFrame::kFormatAv1},
    {"webp", CameraFrame::kFormatWebp},
};

static constexpr CameraFrameFormatEncoding kCameraFrameCanonicalEncodings[] = {
    {"unknown", CameraFrame::kFormatUnknown},
    {"yuv420", CameraFrame::kFormatYuv420},
    {"yuv422", CameraFrame::kFormatYuv422},
    {"yuv444", CameraFrame::kFormatYuv444},
    {"nv12", CameraFrame::kFormatNv12},
    {"nv21", CameraFrame::kFormatNv21},
    {"yuyv", CameraFrame::kFormatYuyv},
    {"yvyu", CameraFrame::kFormatYvyu},
    {"uyvy", CameraFrame::kFormatUyvy},
    {"vyuy", CameraFrame::kFormatVyuy},
    {"bgr8", CameraFrame::kFormatBgr888Packed},
    {"rgb8", CameraFrame::kFormatRgb888Packed},
    {"rgb8_planar", CameraFrame::kFormatRgb888Planar},
    {"mono8", CameraFrame::kFormatMono8},
    {"mono16", CameraFrame::kFormatMono16},
    {"rgba8", CameraFrame::kFormatRgba8888Packed},
    {"bgra8", CameraFrame::kFormatBgra8888Packed},
    {"8UC1", CameraFrame::kFormatUint8C1},
    {"8UC2", CameraFrame::kFormatUint8C2},
    {"8UC3", CameraFrame::kFormatUint8C3},
    {"8UC4", CameraFrame::kFormatUint8C4},
    {"8SC1", CameraFrame::kFormatInt8C1},
    {"8SC2", CameraFrame::kFormatInt8C2},
    {"8SC3", CameraFrame::kFormatInt8C3},
    {"8SC4", CameraFrame::kFormatInt8C4},
    {"16UC1", CameraFrame::kFormatUint16C1},
    {"16UC2", CameraFrame::kFormatUint16C2},
    {"16UC3", CameraFrame::kFormatUint16C3},
    {"16UC4", CameraFrame::kFormatUint16C4},
    {"16SC1", CameraFrame::kFormatInt16C1},
    {"16SC2", CameraFrame::kFormatInt16C2},
    {"16SC3", CameraFrame::kFormatInt16C3},
    {"16SC4", CameraFrame::kFormatInt16C4},
    {"32SC1", CameraFrame::kFormatInt32C1},
    {"32SC2", CameraFrame::kFormatInt32C2},
    {"32SC3", CameraFrame::kFormatInt32C3},
    {"32SC4", CameraFrame::kFormatInt32C4},
    {"32FC1", CameraFrame::kFormatFloat32C1},
    {"32FC2", CameraFrame::kFormatFloat32C2},
    {"32FC3", CameraFrame::kFormatFloat32C3},
    {"32FC4", CameraFrame::kFormatFloat32C4},
    {"64FC1", CameraFrame::kFormatFloat64C1},
    {"64FC2", CameraFrame::kFormatFloat64C2},
    {"64FC3", CameraFrame::kFormatFloat64C3},
    {"64FC4", CameraFrame::kFormatFloat64C4},
    {"bayer_rggb8", CameraFrame::kFormatBayerRggb8},
    {"bayer_bggr8", CameraFrame::kFormatBayerBggr8},
    {"bayer_gbrg8", CameraFrame::kFormatBayerGbrg8},
    {"bayer_grbg8", CameraFrame::kFormatBayerGrbg8},
    {"bayer_rggb16", CameraFrame::kFormatBayerRggb16},
    {"bayer_bggr16", CameraFrame::kFormatBayerBggr16},
    {"bayer_gbrg16", CameraFrame::kFormatBayerGbrg16},
    {"bayer_grbg16", CameraFrame::kFormatBayerGrbg16},
    {"jpeg", CameraFrame::kFormatJpeg},
    {"h264", CameraFrame::kFormatH264},
    {"h265", CameraFrame::kFormatH265},
    {"png", CameraFrame::kFormatPng},
    {"mjpeg", CameraFrame::kFormatMjpeg},
    {"h266", CameraFrame::kFormatH266},
    {"av1", CameraFrame::kFormatAv1},
    {"webp", CameraFrame::kFormatWebp},
};

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

CameraFrame::Format CameraFrame::format_from_encoding(std::string_view encoding) noexcept {
  char normalized[64];
  const auto key = normalize_camera_encoding(encoding, normalized);
  if (key.empty()) {
    return kFormatUnknown;
  }

  for (const auto& item : kCameraFrameEncodingAliases) {
    if (key == item.name) {
      return item.format;
    }
  }

  return kFormatUnknown;
}

std::string_view CameraFrame::encoding_from_format(Format format) noexcept {
  for (const auto& item : kCameraFrameCanonicalEncodings) {
    if (format == item.format) {
      return item.name;
    }
  }

  return "unknown";
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
