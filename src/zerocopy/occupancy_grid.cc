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

#include "./zerocopy/occupancy_grid.h"

#include <cstdint>

namespace vlink {

namespace zerocopy {

// OccupancyGrid
OccupancyGrid::OccupancyGrid() noexcept {
#if defined(__arm__) || defined(__x86__) || defined(__i386__)
#ifndef __ANDROID__
#warning "[OccupancyGrid] No support for 32-bit architecture."
#endif
#else
  static_assert(sizeof(OccupancyGrid) == 152, "Sizeof must be 152 bytes.");
#endif
}

OccupancyGrid::~OccupancyGrid() noexcept {
  if (is_owner_ && data_ && size_ != 0) {
    Bytes::bytes_free(data_, size_);
  }
}

OccupancyGrid::OccupancyGrid(const OccupancyGrid& target) noexcept { deep_copy(target); }

OccupancyGrid::OccupancyGrid(OccupancyGrid&& target) noexcept { move_copy(target); }

OccupancyGrid& OccupancyGrid::operator=(const OccupancyGrid& target) noexcept {
  if VUNLIKELY (this == &target) {
    return *this;
  }

  deep_copy(target);

  return *this;
}

OccupancyGrid& OccupancyGrid::operator=(OccupancyGrid&& target) noexcept {
  if VUNLIKELY (this == &target) {
    return *this;
  }

  move_copy(target);

  return *this;
}

bool OccupancyGrid::operator<<(const Bytes& bytes) noexcept {
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

  std::memcpy(target_ptr, bytes.data() + kMagicNumberBeginSize + kVersionSize, sizeof(OccupancyGrid));

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

  data_ = const_cast<uint8_t*>(bytes.data() + kMagicNumberBeginSize + kVersionSize + sizeof(OccupancyGrid));
  is_owner_ = false;

  if VUNLIKELY (bytes.size() != get_serialized_size()) {
    clear();
    return false;
  }

  return true;
}

bool OccupancyGrid::operator>>(Bytes& bytes) const noexcept {
  static constexpr size_t kMagicNumberBeginSize = sizeof(kMagicNumberBegin);
  static constexpr size_t kVersionSize = sizeof(kWireVersion);
  static constexpr size_t kMagicNumberEndSize = sizeof(kMagicNumberEnd);

  if (bytes.empty() || bytes.size() != get_serialized_size()) {
    bytes = Bytes::create(get_serialized_size());
  }

  std::memcpy(bytes.data(), &kMagicNumberBegin, kMagicNumberBeginSize);

  std::memcpy(bytes.data() + kMagicNumberBeginSize, &kWireVersion, kVersionSize);

  // NOLINTNEXTLINE(bugprone-undefined-memory-manipulation)
  std::memcpy(bytes.data() + kMagicNumberBeginSize + kVersionSize, this, sizeof(OccupancyGrid));

  if VLIKELY (data_ != nullptr && size_ != 0) {
    std::memcpy(bytes.data() + kMagicNumberBeginSize + kVersionSize + sizeof(OccupancyGrid), data_, size_);
  }

  std::memcpy(bytes.data() + kMagicNumberBeginSize + kVersionSize + sizeof(OccupancyGrid) + size_, &kMagicNumberEnd,
              kMagicNumberEndSize);

  return true;
}

bool OccupancyGrid::check_valid(const Bytes& bytes) noexcept {
  static constexpr size_t kMagicNumberBeginSize = sizeof(kMagicNumberBegin);
  static constexpr size_t kVersionSize = sizeof(kWireVersion);
  static constexpr size_t kMagicNumberEndSize = sizeof(kMagicNumberEnd);

  if VUNLIKELY (bytes.size() < kMagicNumberBeginSize + kVersionSize + sizeof(OccupancyGrid) + kMagicNumberEndSize) {
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

size_t OccupancyGrid::get_serialized_size() const noexcept {
  static constexpr size_t kMagicNumberBeginSize = sizeof(kMagicNumberBegin);
  static constexpr size_t kVersionSize = sizeof(kWireVersion);
  static constexpr size_t kMagicNumberEndSize = sizeof(kMagicNumberEnd);

  return kMagicNumberBeginSize + kVersionSize + sizeof(OccupancyGrid) + size_ + kMagicNumberEndSize;
}

bool OccupancyGrid::is_valid() const noexcept { return data_ != nullptr && size_ != 0; }

bool OccupancyGrid::shallow_copy(const OccupancyGrid& target) noexcept {
  if VUNLIKELY (this == &target) {
    return false;
  }

  if (is_owner_ && data_ && size_ != 0) {
    Bytes::bytes_free(data_, size_);
  }

  header = target.header;

  update_time_ns_ = target.update_time_ns_;
  std::memcpy(map_id_, target.map_id_, sizeof(map_id_));
  channel_ = target.channel_;
  freq_ = target.freq_;
  width_ = target.width_;
  height_ = target.height_;
  valid_cell_count_ = target.valid_cell_count_;
  resolution_ = target.resolution_;
  origin_x_ = target.origin_x_;
  origin_y_ = target.origin_y_;
  origin_z_ = target.origin_z_;
  origin_yaw_ = target.origin_yaw_;
  value_min_ = target.value_min_;
  value_max_ = target.value_max_;
  default_value_ = target.default_value_;
  occupied_threshold_ = target.occupied_threshold_;
  free_threshold_ = target.free_threshold_;
  cell_type_ = target.cell_type_;
  reserved_buf_ = target.reserved_buf_;
  reserved_buf2_ = target.reserved_buf2_;
  reserved_buf3_ = target.reserved_buf3_;
  is_owner_ = false;
  data_ = target.data_;
  size_ = target.size_;

  return true;
}

bool OccupancyGrid::deep_copy(const OccupancyGrid& target) noexcept {
  if VLIKELY (data_ && is_owner_ && target.data_ && size_ != 0 && size_ == target.size_) {
    if VUNLIKELY (this == &target) {
      return false;
    }

    header = target.header;

    update_time_ns_ = target.update_time_ns_;
    std::memcpy(map_id_, target.map_id_, sizeof(map_id_));
    channel_ = target.channel_;
    freq_ = target.freq_;
    width_ = target.width_;
    height_ = target.height_;
    valid_cell_count_ = target.valid_cell_count_;
    resolution_ = target.resolution_;
    origin_x_ = target.origin_x_;
    origin_y_ = target.origin_y_;
    origin_z_ = target.origin_z_;
    origin_yaw_ = target.origin_yaw_;
    value_min_ = target.value_min_;
    value_max_ = target.value_max_;
    default_value_ = target.default_value_;
    occupied_threshold_ = target.occupied_threshold_;
    free_threshold_ = target.free_threshold_;
    cell_type_ = target.cell_type_;
    reserved_buf_ = target.reserved_buf_;
    reserved_buf2_ = target.reserved_buf2_;
    reserved_buf3_ = target.reserved_buf3_;

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

bool OccupancyGrid::move_copy(OccupancyGrid& target) noexcept {
  if VUNLIKELY (!shallow_copy(target)) {
    return false;
  }

  is_owner_ = target.is_owner_;

  target.update_time_ns_ = 0;
  std::memset(target.map_id_, 0, sizeof(target.map_id_));
  target.channel_ = 0;
  target.freq_ = 0;
  target.width_ = 0;
  target.height_ = 0;
  target.valid_cell_count_ = 0;
  target.resolution_ = 0;
  target.origin_x_ = 0;
  target.origin_y_ = 0;
  target.origin_z_ = 0;
  target.origin_yaw_ = 0;
  target.value_min_ = 0;
  target.value_max_ = 0;
  target.default_value_ = 0;
  target.occupied_threshold_ = 0;
  target.free_threshold_ = 0;
  target.cell_type_ = kCellUnknown;
  target.reserved_buf_ = 0;
  target.reserved_buf2_ = 0;
  target.reserved_buf3_ = 0;
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

bool OccupancyGrid::create(size_t _size) noexcept {
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

void OccupancyGrid::clear() noexcept {
  if (is_owner_ && data_ && size_ != 0) {
    Bytes::bytes_free(data_, size_);
  }

  update_time_ns_ = 0;
  std::memset(map_id_, 0, sizeof(map_id_));
  channel_ = 0;
  freq_ = 0;
  width_ = 0;
  height_ = 0;
  valid_cell_count_ = 0;
  resolution_ = 0;
  origin_x_ = 0;
  origin_y_ = 0;
  origin_z_ = 0;
  origin_yaw_ = 0;
  value_min_ = 0;
  value_max_ = 0;
  default_value_ = 0;
  occupied_threshold_ = 0;
  free_threshold_ = 0;
  cell_type_ = kCellUnknown;
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

bool OccupancyGrid::shallow_copy(uint8_t* data, size_t size) noexcept {
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

bool OccupancyGrid::deep_copy(uint8_t* data, size_t size) noexcept {
  if VUNLIKELY (!data || size == 0) {
    return false;
  }

  if (is_owner_) {
    if VUNLIKELY (!data_) {
      return false;
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

bool OccupancyGrid::fill_data(uint8_t* data, size_t size) noexcept { return deep_copy(data, size); }

uint64_t OccupancyGrid::update_time_ns() const noexcept { return update_time_ns_; }

std::string_view OccupancyGrid::map_id() const noexcept { return {map_id_, ::strnlen(map_id_, sizeof(map_id_))}; }

uint32_t OccupancyGrid::channel() const noexcept { return channel_; }

uint32_t OccupancyGrid::freq() const noexcept { return freq_; }

uint32_t OccupancyGrid::width() const noexcept { return width_; }

uint32_t OccupancyGrid::height() const noexcept { return height_; }

uint32_t OccupancyGrid::valid_cell_count() const noexcept { return valid_cell_count_; }

float OccupancyGrid::resolution() const noexcept { return resolution_; }

float OccupancyGrid::origin_x() const noexcept { return origin_x_; }

float OccupancyGrid::origin_y() const noexcept { return origin_y_; }

float OccupancyGrid::origin_z() const noexcept { return origin_z_; }

float OccupancyGrid::origin_yaw() const noexcept { return origin_yaw_; }

float OccupancyGrid::value_min() const noexcept { return value_min_; }

float OccupancyGrid::value_max() const noexcept { return value_max_; }

int32_t OccupancyGrid::default_value() const noexcept { return default_value_; }

float OccupancyGrid::occupied_threshold() const noexcept { return occupied_threshold_; }

float OccupancyGrid::free_threshold() const noexcept { return free_threshold_; }

OccupancyGrid::CellType OccupancyGrid::cell_type() const noexcept { return cell_type_; }

uint8_t OccupancyGrid::cell_size() const noexcept { return cell_size_of(cell_type_); }

const uint8_t* OccupancyGrid::data() const noexcept { return data_; }

size_t OccupancyGrid::size() const noexcept { return size_; }

bool OccupancyGrid::is_owner() const noexcept { return is_owner_; }

void OccupancyGrid::set_update_time_ns(uint64_t update_time_ns) noexcept { update_time_ns_ = update_time_ns; }

void OccupancyGrid::set_map_id(std::string_view map_id) noexcept {
  std::memset(map_id_, 0, sizeof(map_id_));

  size_t copy_size = map_id.size();

  if (copy_size >= sizeof(map_id_)) {
    copy_size = sizeof(map_id_) - 1;
  }

  if VLIKELY (copy_size != 0) {
    std::memcpy(map_id_, map_id.data(), copy_size);
  }
}

void OccupancyGrid::set_channel(uint32_t channel) noexcept { channel_ = channel; }

void OccupancyGrid::set_freq(uint32_t freq) noexcept { freq_ = freq; }

void OccupancyGrid::set_width(uint32_t width) noexcept { width_ = width; }

void OccupancyGrid::set_height(uint32_t height) noexcept { height_ = height; }

void OccupancyGrid::set_valid_cell_count(uint32_t valid_cell_count) noexcept { valid_cell_count_ = valid_cell_count; }

void OccupancyGrid::set_resolution(float resolution) noexcept { resolution_ = resolution; }

void OccupancyGrid::set_origin_x(float origin_x) noexcept { origin_x_ = origin_x; }

void OccupancyGrid::set_origin_y(float origin_y) noexcept { origin_y_ = origin_y; }

void OccupancyGrid::set_origin_z(float origin_z) noexcept { origin_z_ = origin_z; }

void OccupancyGrid::set_origin_yaw(float origin_yaw) noexcept { origin_yaw_ = origin_yaw; }

void OccupancyGrid::set_value_min(float value_min) noexcept { value_min_ = value_min; }

void OccupancyGrid::set_value_max(float value_max) noexcept { value_max_ = value_max; }

void OccupancyGrid::set_default_value(int32_t default_value) noexcept { default_value_ = default_value; }

void OccupancyGrid::set_occupied_threshold(float occupied_threshold) noexcept {
  occupied_threshold_ = occupied_threshold;
}

void OccupancyGrid::set_free_threshold(float free_threshold) noexcept { free_threshold_ = free_threshold; }

void OccupancyGrid::set_cell_type(CellType cell_type) noexcept { cell_type_ = cell_type; }

uint8_t OccupancyGrid::cell_size_of(CellType type) noexcept {
  uint8_t target_size = 0;

  if (type == kCellInt8 || type == kCellUint8) {
    target_size = 1;
  } else if (type == kCellUint16) {
    target_size = 2;
  } else if (type == kCellFloat32) {
    target_size = 4;
  }

  return target_size;
}

}  // namespace zerocopy

}  // namespace vlink
