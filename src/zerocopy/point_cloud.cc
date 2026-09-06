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

#include "./zerocopy/point_cloud.h"

#include <tsl/robin_set.h>

#include <array>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace vlink {

namespace zerocopy {

static void pc_pack_to_vertical(uint8_t* dst, const uint8_t* src, size_t count, uint16_t pack, const uint16_t* offsets,
                                const uint8_t* sizes, size_t field_count) noexcept {
  size_t out_pos = 0;

  for (size_t f = 0; f < field_count; ++f) {
    uint16_t field_offset = offsets[f];
    uint8_t field_size = sizes[f];

    for (size_t p = 0; p < count; ++p) {
      std::memcpy(dst + out_pos, src + (p * pack) + field_offset, field_size);

      out_pos += field_size;
    }
  }
}

static void pc_unpack_from_vertical(uint8_t* dst, const uint8_t* src, size_t count, uint16_t pack,
                                    const uint16_t* offsets, const uint8_t* sizes, size_t field_count) noexcept {
  size_t in_pos = 0;

  for (size_t f = 0; f < field_count; ++f) {
    uint16_t field_offset = offsets[f];
    uint8_t field_size = sizes[f];

    for (size_t p = 0; p < count; ++p) {
      std::memcpy(dst + (p * pack) + field_offset, src + in_pos, field_size);

      in_pos += field_size;
    }
  }
}

static int32_t pc_floor_div(int32_t value, int32_t divisor) noexcept {
  int32_t q = value / divisor;
  int32_t r = value % divisor;

  if (r != 0 && r < 0) {
    --q;
  }

  return q;
}

static uint64_t pc_voxel_key(int32_t fx, int32_t fy, int32_t fz) noexcept {
  static constexpr uint64_t kBias = static_cast<uint64_t>(1) << 20;
  static constexpr uint64_t kMask = (static_cast<uint64_t>(1) << 21) - 1;

  uint64_t kx = (static_cast<uint64_t>(static_cast<int64_t>(fx) + static_cast<int64_t>(kBias))) & kMask;
  uint64_t ky = (static_cast<uint64_t>(static_cast<int64_t>(fy) + static_cast<int64_t>(kBias))) & kMask;
  uint64_t kz = (static_cast<uint64_t>(static_cast<int64_t>(fz) + static_cast<int64_t>(kBias))) & kMask;

  return (kx << 42) | (ky << 21) | kz;
}

struct PcVoxelKeyHash {
  size_t operator()(uint64_t key) const noexcept {
    key ^= key >> 33;
    key *= 0xFF51AFD7ED558CCDULL;
    key ^= key >> 33;
    key *= 0xC4CEB9FE1A85EC53ULL;
    key ^= key >> 33;

    return static_cast<size_t>(key);
  }
};

static constexpr size_t kVoxelLutSize = 65536;

// PointCloud::Vector3f
PointCloud::Vector3f::Vector3f() noexcept {
#if defined(__arm__) || defined(__x86__) || defined(__i386__)
#ifndef __ANDROID__
#warning "[PointCloud::Vector3f] No support for 32-bit architecture."
#endif
#else
  static_assert(sizeof(Vector3f) == 12, "Sizeof must be 12 bytes.");
#endif
}

PointCloud::Vector3f::Vector3f(float _x, float _y, float _z) noexcept : x(_x), y(_y), z(_z) {}

std::ostream& operator<<(std::ostream& ostream, const PointCloud::Vector3f& v3f) noexcept {
  ostream << "(" << v3f.x << ", " << v3f.y << ", " << v3f.z << ")";

  return ostream;
}

// PointCloud::Vector3d
PointCloud::Vector3d::Vector3d() noexcept {
#if defined(__arm__) || defined(__x86__) || defined(__i386__)
#ifndef __ANDROID__
#warning "[PointCloud::Vector3d] No support for 32-bit architecture."
#endif
#else
  static_assert(sizeof(Vector3d) == 24, "Sizeof must be 24 bytes.");
#endif
}

PointCloud::Vector3d::Vector3d(double _x, double _y, double _z) noexcept : x(_x), y(_y), z(_z) {}

std::ostream& operator<<(std::ostream& ostream, const PointCloud::Vector3d& v3d) noexcept {
  ostream << "(" << v3d.x << ", " << v3d.y << ", " << v3d.z << ")";

  return ostream;
}

// PointCloud
PointCloud::PointCloud() noexcept {
#if defined(__arm__) || defined(__x86__) || defined(__i386__)
#ifndef __ANDROID__
#warning "[PointCloud] No support for 32-bit architecture."
#endif
#else
  static_assert(sizeof(PointCloud) == 256, "Sizeof must be 256 bytes.");
#endif
}

PointCloud::~PointCloud() noexcept {
  if (is_owner_ && data_ && capacity_ != 0) {
    Bytes::bytes_free(data_, capacity_);
  }
}

PointCloud::PointCloud(const PointCloud& target) noexcept { deep_copy(target); }

PointCloud::PointCloud(PointCloud&& target) noexcept { move_copy(target); }

PointCloud& PointCloud::operator=(const PointCloud& target) noexcept {
  if VUNLIKELY (this == &target) {
    return *this;
  }

  deep_copy(target);

  return *this;
}

PointCloud& PointCloud::operator=(PointCloud&& target) noexcept {
  if VUNLIKELY (this == &target) {
    return *this;
  }

  move_copy(target);

  return *this;
}

bool PointCloud::operator<<(const Bytes& bytes) noexcept {
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
    return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if (is_owner_ && data_ && capacity_ != 0) {
    Bytes::bytes_free(data_, capacity_);
  }

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wclass-memaccess"
#if __GNUC__ >= 11
#pragma GCC diagnostic ignored "-Wstringop-overread"
#endif
#endif

  auto* target_ptr = reinterpret_cast<uint8_t*>(this);

  std::memcpy(target_ptr, bytes.data() + kMagicNumberBeginSize + kVersionSize, sizeof(PointCloud));

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

  is_owner_ = false;
  index_ = 0;
  data_ = const_cast<uint8_t*>(bytes.data() + kMagicNumberBeginSize + kVersionSize + sizeof(PointCloud));
  capacity_ = 0;

  uint8_t vertical_raw = 0;
  std::memcpy(&vertical_raw, &vertical_, sizeof(vertical_raw));

  if VUNLIKELY (vertical_raw > 1) {
    clear(true);

    return false;
  }

  vertical_ = (vertical_raw != 0);

  if (extent_ == 0) {
    downsample_ = 0;
  }

  static constexpr size_t kSerializedOverhead =
      kMagicNumberBeginSize + kVersionSize + sizeof(PointCloud) + sizeof(kMagicNumberEnd);

  if VUNLIKELY (pack_size_ != 0 && size_ > (std::numeric_limits<size_t>::max() - kSerializedOverhead) / pack_size_) {
    clear(true);

    return false;
  }

  if VUNLIKELY (bytes.size() != get_serialized_size()) {
    clear(true);

    return false;
  }

  std::array<uint16_t, 16> field_offsets{};
  std::array<uint8_t, 16> field_sizes{};
  size_t field_count = 0;

  if VLIKELY (pack_size_ != 0) {
    uint16_t field_offset = 0;
    bool leading_zero = true;

    for (int i = 15; i >= 0; --i) {
      uint8_t field_size = (protocol_.size_num >> (i * 4)) & 0xF;

      if (leading_zero && field_size == 0) {
        continue;
      }

      leading_zero = false;
      field_offsets[field_count] = field_offset;
      field_sizes[field_count] = field_size;
      ++field_count;
      field_offset += field_size;
    }

    if VUNLIKELY (field_offset != pack_size_) {
      clear(true);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

      return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
  }

  if (vertical_ && size_ != 0 && pack_size_ != 0) {
    const uint8_t* payload = data_;

    capacity_ = size_ * pack_size_;
    data_ = Bytes::bytes_malloc(capacity_);

    if VUNLIKELY (!data_) {
      clear(true);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

      return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    is_owner_ = true;
    index_ = capacity_;

    pc_unpack_from_vertical(data_, payload, size_, pack_size_, field_offsets.data(), field_sizes.data(), field_count);
  }

  return true;
}

bool PointCloud::operator>>(Bytes& bytes) const noexcept {
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
  std::memcpy(bytes.data() + kMagicNumberBeginSize + kVersionSize, this, sizeof(PointCloud));

  const auto data_offset = reinterpret_cast<const uint8_t*>(&data_) - reinterpret_cast<const uint8_t*>(this);
  const size_t data_pointer_size = sizeof(data_);
  std::memset(bytes.data() + kMagicNumberBeginSize + kVersionSize + data_offset, 0, data_pointer_size);

  if VLIKELY (data_ != nullptr && size_ != 0 && pack_size_ != 0) {
    uint8_t* payload = bytes.data() + kMagicNumberBeginSize + kVersionSize + sizeof(PointCloud);

    if (vertical_) {
      std::array<uint16_t, 16> field_offsets{};
      std::array<uint8_t, 16> field_sizes{};
      size_t field_count = 0;
      uint16_t field_offset = 0;
      bool leading_zero = true;

      for (int i = 15; i >= 0; --i) {
        uint8_t field_size = (protocol_.size_num >> (i * 4)) & 0xF;

        if (leading_zero && field_size == 0) {
          continue;
        }

        leading_zero = false;
        field_offsets[field_count] = field_offset;
        field_sizes[field_count] = field_size;
        ++field_count;

        field_offset += field_size;
      }

      pc_pack_to_vertical(payload, data_, size_, pack_size_, field_offsets.data(), field_sizes.data(), field_count);
    } else {
      std::memcpy(payload, data_, size_ * pack_size_);
    }
  }

  std::memcpy(bytes.data() + kMagicNumberBeginSize + kVersionSize + sizeof(PointCloud) + (size_ * pack_size_),
              &kMagicNumberEnd, kMagicNumberEndSize);

  return true;
}

bool PointCloud::check_valid(const Bytes& bytes) noexcept {
  static constexpr size_t kMagicNumberBeginSize = sizeof(kMagicNumberBegin);
  static constexpr size_t kVersionSize = sizeof(kWireVersion);
  static constexpr size_t kMagicNumberEndSize = sizeof(kMagicNumberEnd);

  if VUNLIKELY (bytes.size() < kMagicNumberBeginSize + kVersionSize + sizeof(PointCloud) + kMagicNumberEndSize) {
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

bool PointCloud::is_valid() const noexcept { return data_ != nullptr && size_ != 0 && pack_size_ != 0; }

bool PointCloud::shallow_copy(const PointCloud& target) noexcept {
  if VUNLIKELY (this == &target) {
    return false;
  }

  if (is_owner_ && data_ && capacity_ != 0) {
    const auto current = reinterpret_cast<uintptr_t>(data_);
    const auto source = reinterpret_cast<uintptr_t>(target.data_);

    if VUNLIKELY (source >= current && source - current < capacity_) {
      return false;
    }

    Bytes::bytes_free(data_, capacity_);
  }

  header = target.header;
  capacity_ = 0;
  size_ = target.size_;
  reserved_buf_ = target.reserved_buf_;
  reserved_buf2_ = target.reserved_buf2_;
  reserved_buf3_ = target.reserved_buf3_;
  downsample_ = target.downsample_;
  extent_ = target.extent_;
  vertical_ = target.vertical_;
  pack_size_ = target.pack_size_;
  is_owner_ = false;
  index_ = target.index_;
  data_ = target.data_;

  protocol_.size_num = target.protocol_.size_num;
  protocol_.type_num = target.protocol_.type_num;
  std::memcpy(protocol_.names, target.protocol_.names, sizeof(protocol_.names));

  return true;
}

bool PointCloud::deep_copy(const PointCloud& target) noexcept {
  if VUNLIKELY (target.pack_size_ != 0 && target.size_ > std::numeric_limits<size_t>::max() / target.pack_size_) {
    return false;
  }

  const size_t target_size = target.size_ * target.pack_size_;

  if VLIKELY (data_ && is_owner_ && target.data_ && capacity_ != 0 && capacity_ == target_size) {
    const auto current = reinterpret_cast<uintptr_t>(data_);
    const auto source = reinterpret_cast<uintptr_t>(target.data_);

    if VUNLIKELY (source >= current && source - current < capacity_) {
      return false;
    }

    header = target.header;
    size_ = target.size_;
    reserved_buf_ = target.reserved_buf_;
    reserved_buf2_ = target.reserved_buf2_;
    reserved_buf3_ = target.reserved_buf3_;
    downsample_ = target.downsample_;
    extent_ = target.extent_;
    vertical_ = target.vertical_;
    pack_size_ = target.pack_size_;
    index_ = target.index_;

    protocol_.size_num = target.protocol_.size_num;
    protocol_.type_num = target.protocol_.type_num;
    std::memcpy(protocol_.names, target.protocol_.names, sizeof(protocol_.names));

    std::memcpy(data_, target.data_, capacity_);

    return true;
  }

  if VUNLIKELY (!shallow_copy(target)) {
    return false;
  }

  if VLIKELY (data_ != nullptr && target_size != 0) {
    auto* target_data = data_;
    capacity_ = target_size;
    data_ = Bytes::bytes_malloc(capacity_);

    if VUNLIKELY (!data_) {
      capacity_ = 0;
      size_ = 0;
      index_ = 0;
      return false;
    }

    std::memcpy(data_, target_data, capacity_);
    is_owner_ = true;
  } else {
    data_ = nullptr;
  }

  return true;
}

bool PointCloud::move_copy(PointCloud& target) noexcept {
  if VUNLIKELY (!shallow_copy(target)) {
    return false;
  }

  is_owner_ = target.is_owner_;
  capacity_ = target.capacity_;

  target.capacity_ = 0;
  target.size_ = 0;
  target.reserved_buf_ = 0;
  target.reserved_buf2_ = 0;
  target.reserved_buf3_ = 0;
  target.downsample_ = 0;
  target.extent_ = 0;
  target.vertical_ = false;
  target.pack_size_ = 0;
  target.is_owner_ = false;
  target.index_ = 0;
  target.data_ = nullptr;

  target.protocol_.size_num = 0;
  target.protocol_.type_num = 0;
  std::memset(&target.protocol_.names, 0, sizeof(protocol_.names));

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

size_t PointCloud::get_serialized_size() const noexcept {
  static constexpr size_t kMagicNumberBeginSize = sizeof(kMagicNumberBegin);
  static constexpr size_t kVersionSize = sizeof(kWireVersion);
  static constexpr size_t kMagicNumberEndSize = sizeof(kMagicNumberEnd);

  return kMagicNumberBeginSize + kVersionSize + sizeof(PointCloud) + (size_ * pack_size_) + kMagicNumberEndSize;
}

PointCloud::KeyMap PointCloud::get_key_map(KeyList* key_list) const noexcept {
  KeyMap map;

  auto target_key_list = protocol_.get_key_list();

  uint16_t index = 0;

  for (const auto& key : target_key_list) {
    map.try_emplace(key.name, index);

    index += key.size;
  }

  if (key_list) {
    if (extent_ != 0) {
      for (size_t i = 0; i < target_key_list.size() && i < 3; ++i) {
        if (target_key_list[i].type == kInt16Type && target_key_list[i].size == sizeof(int16_t)) {
          target_key_list[i].type = kFloatType;
        }
      }
    }

    *key_list = std::move(target_key_list);
  }

  return map;
}

PointCloud::KeyList PointCloud::get_key_list() const noexcept {
  auto key_list = protocol_.get_key_list();

  if (extent_ != 0) {
    for (size_t i = 0; i < key_list.size() && i < 3; ++i) {
      if (key_list[i].type == kInt16Type && key_list[i].size == sizeof(int16_t)) {
        key_list[i].type = kFloatType;
      }
    }
  }

  return key_list;
}

size_t PointCloud::size() const noexcept { return size_; }

size_t PointCloud::pack_size() const noexcept { return pack_size_; }

bool PointCloud::is_owner() const noexcept { return is_owner_; }

uint16_t PointCloud::get_extent() const noexcept { return extent_; }

bool PointCloud::get_vertical() const noexcept { return vertical_; }

void PointCloud::set_vertical(bool vertical) noexcept { vertical_ = vertical; }

uint8_t PointCloud::get_downsample() const noexcept { return downsample_; }

bool PointCloud::downsample(uint8_t level) noexcept {
  if (level == 0) {
    downsample_ = 0;

    return true;
  }

  if VUNLIKELY (extent_ == 0) {
    return false;
  }

  if VUNLIKELY (!is_owner_ || !data_ || pack_size_ < (sizeof(int16_t) * 3)) {
    return false;
  }

  downsample_ = level;

  if VUNLIKELY (size_ == 0) {
    return true;
  }

  auto v_q = static_cast<int32_t>((static_cast<uint32_t>(level) * 128U + 127U) / 255U);

  const int32_t cell_lo = pc_floor_div(-32768, v_q);
  const int32_t cell_hi = pc_floor_div(32767, v_q);
  const int32_t cells_span = cell_hi - cell_lo;
  const uint64_t cells_per_axis = static_cast<uint64_t>(cells_span) + 1;
  const uint64_t cell_count = cells_per_axis * cells_per_axis * cells_per_axis;
  const size_t reserve_n = (cell_count < static_cast<uint64_t>(size_)) ? static_cast<size_t>(cell_count)
                                                                       : size_;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

  const uint16_t ps = pack_size_;

  tsl::robin_set<uint64_t, PcVoxelKeyHash> seen;
  seen.reserve(reserve_n);

  std::vector<int16_t> cell_lut;
  const int16_t* lut = nullptr;

  if (size_ >= kVoxelLutSize) {
    cell_lut.resize(kVoxelLutSize);

    for (size_t idx = 0; idx < kVoxelLutSize; ++idx) {
      const int32_t q = static_cast<int32_t>(idx) - 32768;
      cell_lut[idx] = static_cast<int16_t>(pc_floor_div(q, v_q));
    }

    lut = cell_lut.data();
  }

  size_t w = 0;

  for (size_t r = 0; r < size_; ++r) {
    const uint8_t* src = data_ + (r * ps);

    int16_t qx = 0;
    int16_t qy = 0;
    int16_t qz = 0;

    std::memcpy(&qx, src, sizeof(int16_t));
    std::memcpy(&qy, src + sizeof(int16_t), sizeof(int16_t));
    std::memcpy(&qz, src + (sizeof(int16_t) * 2), sizeof(int16_t));

    int32_t fx = 0;
    int32_t fy = 0;
    int32_t fz = 0;

    if (lut != nullptr) {
      fx = lut[static_cast<int32_t>(qx) + 32768];
      fy = lut[static_cast<int32_t>(qy) + 32768];
      fz = lut[static_cast<int32_t>(qz) + 32768];
    } else {
      fx = pc_floor_div(qx, v_q);
      fy = pc_floor_div(qy, v_q);
      fz = pc_floor_div(qz, v_q);
    }

    if (seen.insert(pc_voxel_key(fx, fy, fz)).second) {
      if (w != r) {
        std::memcpy(data_ + (w * ps), src, ps);
      }

      ++w;
    }
  }

  size_ = w;
  index_ = w * ps;

  return true;
}

uint64_t PointCloud::get_protocol_size_num() const noexcept { return protocol_.size_num; }

uint64_t PointCloud::get_protocol_type_num() const noexcept { return protocol_.type_num; }

std::string PointCloud::get_protocol_size_str() const noexcept { return protocol_.get_size_for_print(); }

std::string PointCloud::get_protocol_name_str() const noexcept {
  return std::string(protocol_.names, ::strnlen(protocol_.names, sizeof(protocol_.names)));
}

std::string PointCloud::get_protocol_type_str() const noexcept {
  if (extent_ == 0) {
    return protocol_.get_type_for_print();
  }

  auto key_list = protocol_.get_key_list();

  std::string print_str;

  for (size_t i = 0; i < key_list.size(); ++i) {
    const auto& key = key_list[i];
    uint8_t type = key.type;

    if (i < 3 && key.type == kInt16Type && key.size == sizeof(int16_t)) {
      type = kFloatType;
    }

    if (!print_str.empty()) {
      print_str += ",";
    }

    switch (type) {
      case kBoolType:
        print_str += "bool";
        break;
      case kInt8Type:
        print_str += "int8";
        break;
      case kUint8Type:
        print_str += "uint8";
        break;
      case kInt16Type:
        print_str += "int16";
        break;
      case kUint16Type:
        print_str += "uint16";
        break;
      case kInt32Type:
        print_str += "int32";
        break;
      case kUint32Type:
        print_str += "uint32";
        break;
      case kInt64Type:
        print_str += "int64";
        break;
      case kUint64Type:
        print_str += "uint64";
        break;
      case kFloatType:
        print_str += "float";
        break;
      case kDoubleType:
        print_str += "double";
        break;
      default:
        break;
    }
  }

  return print_str;
}

const uint8_t* PointCloud::get_internal_data() const noexcept { return data_; }

size_t PointCloud::get_reserved_size() const noexcept {
  if (pack_size_ == 0) {
    return 0;
  }

  return capacity_ / pack_size_;
}

bool PointCloud::get_value_v3f(float& x, float& y, float& z, size_t loop_index) const noexcept {
  if (extent_ != 0) {
    if VUNLIKELY (!data_ || loop_index >= size_ || pack_size_ < sizeof(int16_t) * 3) {
      return false;
    }

    const uint8_t* base = data_ + (loop_index * pack_size_);

    int16_t qx = 0;
    int16_t qy = 0;
    int16_t qz = 0;

    std::memcpy(&qx, base, sizeof(int16_t));
    std::memcpy(&qy, base + sizeof(int16_t), sizeof(int16_t));
    std::memcpy(&qz, base + (sizeof(int16_t) * 2), sizeof(int16_t));

    x = Quantize::decode<float>(extent_, qx);
    y = Quantize::decode<float>(extent_, qy);
    z = Quantize::decode<float>(extent_, qz);

    return true;
  }

  if VUNLIKELY (!data_ || loop_index >= size_ || pack_size_ < sizeof(float) * 3) {
    return false;
  }

  std::memcpy(&x, data_ + (loop_index * pack_size_), sizeof(float));
  std::memcpy(&y, data_ + (loop_index * pack_size_) + sizeof(float), sizeof(float));
  std::memcpy(&z, data_ + (loop_index * pack_size_) + sizeof(float) + sizeof(float), sizeof(float));

  return true;
}

bool PointCloud::get_value_v3f(Vector3f& v3f, size_t loop_index) const noexcept {
  if (extent_ != 0) {
    return get_value_v3f(v3f.x, v3f.y, v3f.z, loop_index);
  }

  if VUNLIKELY (!data_ || loop_index >= size_ || pack_size_ < sizeof(float) * 3) {
    return false;
  }

  std::memcpy(&v3f, data_ + (loop_index * pack_size_), sizeof(float) * 3);

  return true;
}

PointCloud::Vector3f PointCloud::get_value_v3f(size_t loop_index) const noexcept {
  PointCloud::Vector3f v3f;

  get_value_v3f(v3f, loop_index);

  return v3f;
}

bool PointCloud::get_value_v3d(double& x, double& y, double& z, size_t loop_index) const noexcept {
  if (extent_ != 0) {
    if VUNLIKELY (!data_ || loop_index >= size_ || pack_size_ < sizeof(int16_t) * 3) {
      return false;
    }

    const uint8_t* base = data_ + (loop_index * pack_size_);

    int16_t qx = 0;
    int16_t qy = 0;
    int16_t qz = 0;

    std::memcpy(&qx, base, sizeof(int16_t));
    std::memcpy(&qy, base + sizeof(int16_t), sizeof(int16_t));
    std::memcpy(&qz, base + (sizeof(int16_t) * 2), sizeof(int16_t));

    x = Quantize::decode<double>(extent_, qx);
    y = Quantize::decode<double>(extent_, qy);
    z = Quantize::decode<double>(extent_, qz);

    return true;
  }

  if VUNLIKELY (!data_ || loop_index >= size_ || pack_size_ < sizeof(double) * 3) {
    return false;
  }

  std::memcpy(&x, data_ + (loop_index * pack_size_), sizeof(double));
  std::memcpy(&y, data_ + (loop_index * pack_size_) + sizeof(double), sizeof(double));
  std::memcpy(&z, data_ + (loop_index * pack_size_) + sizeof(double) + sizeof(double), sizeof(double));

  return true;
}

bool PointCloud::get_value_v3d(Vector3d& v3d, size_t loop_index) const noexcept {
  if (extent_ != 0) {
    return get_value_v3d(v3d.x, v3d.y, v3d.z, loop_index);
  }

  if VUNLIKELY (!data_ || loop_index >= size_ || pack_size_ < sizeof(double) * 3) {
    return false;
  }

  std::memcpy(&v3d, data_ + (loop_index * pack_size_), sizeof(double) * 3);

  return true;
}

PointCloud::Vector3d PointCloud::get_value_v3d(size_t loop_index) const noexcept {
  PointCloud::Vector3d v3d;

  get_value_v3d(v3d, loop_index);

  return v3d;
}

double PointCloud::get_value_for_double_float(size_t loop_index, uint16_t offset, uint8_t type) const noexcept {
  switch (type) {
    case kBoolType:
      return get_value<uint8_t>(loop_index, offset);
    case kInt8Type:
      return get_value<int8_t>(loop_index, offset);
    case kUint8Type:
      return get_value<uint8_t>(loop_index, offset);
    case kInt16Type:
      return get_value<int16_t>(loop_index, offset);
    case kUint16Type:
      return get_value<uint16_t>(loop_index, offset);
    case kInt32Type:
      return get_value<int32_t>(loop_index, offset);
    case kUint32Type:
      return get_value<uint32_t>(loop_index, offset);
    case kInt64Type:
      return get_value<int64_t>(loop_index, offset);
    case kUint64Type:
      return get_value<uint64_t>(loop_index, offset);
    case kFloatType:
      return get_value<float>(loop_index, offset);
    case kDoubleType:
      return get_value<double>(loop_index, offset);
    default:
      return 0;
  }
}

double PointCloud::get_value_for_double_float(size_t loop_index, KeyMap& key_map, std::string_view key,
                                              uint8_t type) const noexcept {
  switch (type) {
    case kBoolType:
      return get_value<uint8_t>(loop_index, key_map, key);
    case kInt8Type:
      return get_value<int8_t>(loop_index, key_map, key);
    case kUint8Type:
      return get_value<uint8_t>(loop_index, key_map, key);
    case kInt16Type:
      return get_value<int16_t>(loop_index, key_map, key);
    case kUint16Type:
      return get_value<uint16_t>(loop_index, key_map, key);
    case kInt32Type:
      return get_value<int32_t>(loop_index, key_map, key);
    case kUint32Type:
      return get_value<uint32_t>(loop_index, key_map, key);
    case kInt64Type:
      return get_value<int64_t>(loop_index, key_map, key);
    case kUint64Type:
      return get_value<uint64_t>(loop_index, key_map, key);
    case kFloatType:
      return get_value<float>(loop_index, key_map, key);
    case kDoubleType:
      return get_value<double>(loop_index, key_map, key);
    default:
      return 0;
  }
}

std::string PointCloud::get_value_for_print(size_t loop_index, uint16_t offset, uint8_t type) const noexcept {
  switch (type) {
    case kBoolType:
      return get_value<bool>(loop_index, offset) ? "true" : "false";
    case kInt8Type:
      return std::to_string(get_value<int8_t>(loop_index, offset));
    case kUint8Type:
      return std::to_string(get_value<uint8_t>(loop_index, offset));
    case kInt16Type:
      return std::to_string(get_value<int16_t>(loop_index, offset));
    case kUint16Type:
      return std::to_string(get_value<uint16_t>(loop_index, offset));
    case kInt32Type:
      return std::to_string(get_value<int32_t>(loop_index, offset));
    case kUint32Type:
      return std::to_string(get_value<uint32_t>(loop_index, offset));
    case kInt64Type:
      return std::to_string(get_value<int64_t>(loop_index, offset));
    case kUint64Type:
      return std::to_string(get_value<uint64_t>(loop_index, offset));
    case kFloatType:
      return std::to_string(get_value<float>(loop_index, offset));
    case kDoubleType:
      return std::to_string(get_value<double>(loop_index, offset));
    default:
      return std::string();
  }
}

std::string PointCloud::get_value_for_print(size_t loop_index, KeyMap& key_map, std::string_view key,
                                            uint8_t type) const noexcept {
  switch (type) {
    case kBoolType:
      return get_value<bool>(loop_index, key_map, key) ? "true" : "false";
    case kInt8Type:
      return std::to_string(get_value<int8_t>(loop_index, key_map, key));
    case kUint8Type:
      return std::to_string(get_value<uint8_t>(loop_index, key_map, key));
    case kInt16Type:
      return std::to_string(get_value<int16_t>(loop_index, key_map, key));
    case kUint16Type:
      return std::to_string(get_value<uint16_t>(loop_index, key_map, key));
    case kInt32Type:
      return std::to_string(get_value<int32_t>(loop_index, key_map, key));
    case kUint32Type:
      return std::to_string(get_value<uint32_t>(loop_index, key_map, key));
    case kInt64Type:
      return std::to_string(get_value<int64_t>(loop_index, key_map, key));
    case kUint64Type:
      return std::to_string(get_value<uint64_t>(loop_index, key_map, key));
    case kFloatType:
      return std::to_string(get_value<float>(loop_index, key_map, key));
    case kDoubleType:
      return std::to_string(get_value<double>(loop_index, key_map, key));
    default:
      return std::string();
  }
}

bool PointCloud::create(size_t size, uint64_t size_num, uint64_t type_num, std::string_view key_str, uint16_t extent,
                        bool vertical) noexcept {
  if VUNLIKELY (!Protocol::check_valid(size_num, key_str)) {
    return false;
  }

  Protocol new_protocol{};
  new_protocol.size_num = size_num;
  std::memset(new_protocol.names, 0, sizeof(new_protocol.names));
  std::memcpy(new_protocol.names, key_str.data(), key_str.size());
  new_protocol.type_num = type_num;

  if (extent != 0) {
    PointCloud protocol_probe;
    protocol_probe.protocol_ = new_protocol;

    if VUNLIKELY (!protocol_probe.compress_protocol_xyz()) {
      return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    new_protocol = protocol_probe.protocol_;
  }

  const size_t new_pack_size = new_protocol.get_pack_size();

  if VUNLIKELY (new_pack_size != 0 && size > std::numeric_limits<size_t>::max() / new_pack_size) {
    return false;
  }

  if (is_owner_ && data_ && capacity_ != 0) {
    Bytes::bytes_free(data_, capacity_);
  }

  data_ = nullptr;
  is_owner_ = false;
  capacity_ = 0;
  size_ = 0;
  index_ = 0;

  protocol_ = new_protocol;
  extent_ = extent;
  vertical_ = vertical;
  downsample_ = 0;

  pack_size_ = new_pack_size;
  capacity_ = size * pack_size_;

  if VLIKELY (capacity_ != 0) {
    data_ = Bytes::bytes_malloc(capacity_);

    if VUNLIKELY (!data_) {
      capacity_ = 0;
      return false;
    }

    is_owner_ = true;
  }

  return true;
}

void PointCloud::clear(bool force) noexcept {
  if (force) {
    if (is_owner_ && data_ && capacity_ != 0) {
      Bytes::bytes_free(data_, capacity_);
    }

    pack_size_ = 0;
    capacity_ = 0;
    data_ = nullptr;
    reserved_buf_ = 0;
    reserved_buf2_ = 0;
    reserved_buf3_ = 0;
    extent_ = 0;
    vertical_ = false;
    is_owner_ = false;

    protocol_.size_num = 0;
    std::memset(protocol_.names, 0, sizeof(protocol_.names));
    protocol_.type_num = 0;

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

  size_ = 0;
  index_ = 0;
  downsample_ = 0;
}

// PointCloud::Protocol
bool PointCloud::Protocol::check_valid(uint64_t _size_num, std::string_view _names) noexcept {
  if VUNLIKELY (_size_num == 0 || _names.empty() || _names.size() > sizeof(names)) {
    return false;
  }

  uint16_t num_count = 0;
  uint16_t key_count = 0;

  do {
    num_count++;
    _size_num >>= 4;
  } while (_size_num != 0);

  for (auto c : _names) {
    if (c == ',') {
      ++key_count;
    }
  }

  if (!_names.empty()) {
    ++key_count;
  }

  if VUNLIKELY (key_count != num_count || key_count < 3 || key_count > 16) {
    return false;
  }

  return true;
}

std::string PointCloud::Protocol::get_names(const std::vector<std::string>& keys) noexcept {
  std::string key_str;

  for (const auto& key : keys) {
    if (!key_str.empty()) {
      key_str += ',';
    }

    key_str += key;
  }

  if VUNLIKELY (key_str.size() > sizeof(names)) {
    return std::string();
  }

  return key_str;
}

PointCloud::KeyList PointCloud::Protocol::get_key_list() const noexcept {
  KeyList key_list;
  key_list.reserve(16);

  uint64_t temp_size_num = size_num;
  uint64_t temp_type_num = type_num;

  std::istringstream iss(std::string(names, ::strnlen(names, sizeof(names))));
  std::string token;

  bool leading_zero = true;

  for (int i = 15; i >= 0; --i) {
    uint8_t size_nibble = (temp_size_num >> (i * 4)) & 0xF;
    uint8_t type_nibble = (temp_type_num >> (i * 4)) & 0xF;

    if (leading_zero && size_nibble == 0) {
      continue;
    }

    leading_zero = false;

    if VUNLIKELY (!std::getline(iss, token, ',')) {
      break;
    }

    Key key;
    key.size = size_nibble;
    key.type = type_nibble;
    key.name = token;

    key_list.emplace_back(std::move(key));
  }

  return key_list;
}

std::string PointCloud::Protocol::get_size_for_print() const noexcept {
  bool leading_zero = true;

  uint64_t temp_size_num = size_num;

  std::string print_str;

  for (int i = 15; i >= 0; --i) {
    uint8_t size_nibble = (temp_size_num >> (i * 4)) & 0xF;

    if (leading_zero && size_nibble == 0) {
      continue;
    }

    leading_zero = false;

    if (!print_str.empty()) {
      print_str += ",";
    }

    print_str += std::to_string(size_nibble);
  }

  return print_str;
}

std::string PointCloud::Protocol::get_type_for_print() const noexcept {
  bool leading_zero = true;

  uint64_t temp_type_num = type_num;

  std::string print_str;

  for (int i = 15; i >= 0; --i) {
    uint8_t type_nibble = (temp_type_num >> (i * 4)) & 0xF;

    if (leading_zero && type_nibble == 0) {
      continue;
    }

    leading_zero = false;

    if (!print_str.empty()) {
      print_str += ",";
    }

    switch (type_nibble) {
      case kBoolType:
        print_str += "bool";
        break;
      case kInt8Type:
        print_str += "int8";
        break;
      case kUint8Type:
        print_str += "uint8";
        break;
      case kInt16Type:
        print_str += "int16";
        break;
      case kUint16Type:
        print_str += "uint16";
        break;
      case kInt32Type:
        print_str += "int32";
        break;
      case kUint32Type:
        print_str += "uint32";
        break;
      case kInt64Type:
        print_str += "int64";
        break;
      case kUint64Type:
        print_str += "uint64";
        break;
      case kFloatType:
        print_str += "float";
        break;
      case kDoubleType:
        print_str += "double";
        break;
      default:
        break;
    }
  }

  return print_str;
}

bool PointCloud::compress_protocol_xyz() noexcept {
  uint16_t num_fields = 0;
  uint64_t temp_size_num = protocol_.size_num;

  do {
    ++num_fields;
    temp_size_num >>= 4;
  } while (temp_size_num != 0);

  if VUNLIKELY (num_fields < 3) {
    return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  for (uint16_t i = 0; i < 3; ++i) {
    uint64_t shift = static_cast<uint64_t>(num_fields - 1 - i) * 4;
    uint8_t cur_type = (protocol_.type_num >> shift) & 0xF;

    if VUNLIKELY (cur_type != kFloatType && cur_type != kDoubleType && cur_type != kInt16Type) {
      return false;
    }
  }

  for (uint16_t i = 0; i < 3; ++i) {
    uint64_t shift = static_cast<uint64_t>(num_fields - 1 - i) * 4;

    protocol_.size_num &= ~(static_cast<uint64_t>(0xF) << shift);
    protocol_.size_num |= (static_cast<uint64_t>(sizeof(int16_t)) << shift);
    protocol_.type_num &= ~(static_cast<uint64_t>(0xF) << shift);
    protocol_.type_num |= (static_cast<uint64_t>(kInt16Type) << shift);
  }

  return true;
}

}  // namespace zerocopy

}  // namespace vlink
