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

#include "./zerocopy/object_array.h"

#include <cstdint>
#include <limits>

namespace vlink {

namespace zerocopy {

// Object
ObjectArray::Object::Object() noexcept {
#if defined(__arm__) || defined(__x86__) || defined(__i386__)
#ifndef __ANDROID__
#warning "[ObjectArray::Object] No support for 32-bit architecture."
#endif
#else
  static_assert(sizeof(ObjectArray::Object) == 144, "Sizeof must be 144 bytes.");
#endif
}

// ObjectArray
ObjectArray::ObjectArray() noexcept {
#if defined(__arm__) || defined(__x86__) || defined(__i386__)
#ifndef __ANDROID__
#warning "[ObjectArray] No support for 32-bit architecture."
#endif
#else
  static_assert(sizeof(ObjectArray) == 112, "Sizeof must be 112 bytes.");
#endif
}

ObjectArray::~ObjectArray() noexcept {
  if (is_owner_ && data_ && capacity_ != 0) {
    Bytes::bytes_free(data_, capacity_);
  }
}

ObjectArray::ObjectArray(const ObjectArray& target) noexcept { deep_copy(target); }

ObjectArray::ObjectArray(ObjectArray&& target) noexcept { move_copy(target); }

ObjectArray& ObjectArray::operator=(const ObjectArray& target) noexcept {
  if VUNLIKELY (this == &target) {
    return *this;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  deep_copy(target);

  return *this;
}

ObjectArray& ObjectArray::operator=(ObjectArray&& target) noexcept {
  if VUNLIKELY (this == &target) {
    return *this;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  move_copy(target);

  return *this;
}

bool ObjectArray::operator<<(const Bytes& bytes) noexcept {
  static constexpr size_t kMagicNumberBeginSize = sizeof(kMagicNumberBegin);
  static constexpr size_t kVersionSize = sizeof(kWireVersion);
  static constexpr size_t kMagicNumberEndSize = sizeof(kMagicNumberEnd);
  static constexpr size_t kSerializedOverhead =
      kMagicNumberBeginSize + kVersionSize + sizeof(ObjectArray) + kMagicNumberEndSize;

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

  std::memcpy(target_ptr, bytes.data() + kMagicNumberBeginSize + kVersionSize, sizeof(ObjectArray));

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

  data_ = const_cast<uint8_t*>(bytes.data() + kMagicNumberBeginSize + kVersionSize + sizeof(ObjectArray));

  capacity_ = 0;

  is_owner_ = false;

  const auto payload_size = bytes.size() - kSerializedOverhead;

  if VUNLIKELY (count_ != 0 &&
                (pack_size_ != sizeof(Object) || static_cast<size_t>(count_) > payload_size / sizeof(Object))) {
    clear();
    return false;
  }

  if VUNLIKELY (bytes.size() != get_serialized_size()) {
    clear();
    return false;
  }

  return true;
}

bool ObjectArray::operator>>(Bytes& bytes) const noexcept {
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
  std::memcpy(bytes.data() + kMagicNumberBeginSize + kVersionSize, this, sizeof(ObjectArray));

  const auto data_offset = reinterpret_cast<const uint8_t*>(&data_) - reinterpret_cast<const uint8_t*>(this);
  const size_t data_pointer_size = sizeof(data_);
  std::memset(bytes.data() + kMagicNumberBeginSize + kVersionSize + data_offset, 0, data_pointer_size);

  if VLIKELY (data_ != nullptr && count_ != 0 && pack_size_ != 0) {
    std::memcpy(bytes.data() + kMagicNumberBeginSize + kVersionSize + sizeof(ObjectArray), data_,
                static_cast<size_t>(count_) * pack_size_);
  }

  std::memcpy(bytes.data() + kMagicNumberBeginSize + kVersionSize + sizeof(ObjectArray) +
                  (static_cast<size_t>(count_) * pack_size_),
              &kMagicNumberEnd, kMagicNumberEndSize);

  return true;
}

bool ObjectArray::check_valid(const Bytes& bytes) noexcept {
  static constexpr size_t kMagicNumberBeginSize = sizeof(kMagicNumberBegin);
  static constexpr size_t kVersionSize = sizeof(kWireVersion);
  static constexpr size_t kMagicNumberEndSize = sizeof(kMagicNumberEnd);

  if VUNLIKELY (bytes.size() < kMagicNumberBeginSize + kVersionSize + sizeof(ObjectArray) + kMagicNumberEndSize) {
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

size_t ObjectArray::get_serialized_size() const noexcept {
  static constexpr size_t kMagicNumberBeginSize = sizeof(kMagicNumberBegin);
  static constexpr size_t kVersionSize = sizeof(kWireVersion);
  static constexpr size_t kMagicNumberEndSize = sizeof(kMagicNumberEnd);

  return kMagicNumberBeginSize + kVersionSize + sizeof(ObjectArray) + (static_cast<size_t>(count_) * pack_size_) +
         kMagicNumberEndSize;
}

bool ObjectArray::is_valid() const noexcept { return data_ != nullptr && count_ != 0 && pack_size_ != 0; }

bool ObjectArray::shallow_copy(const ObjectArray& target) noexcept {
  if VUNLIKELY (this == &target) {
    return false;
  }

  if (is_owner_ && data_ && capacity_ != 0) {
    const auto current = reinterpret_cast<uintptr_t>(data_);
    const auto source = reinterpret_cast<uintptr_t>(target.data_);

    if VUNLIKELY (source >= current && source - current < capacity_) {
      return false;
    }

    Bytes::bytes_free(data_, capacity_);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  header = target.header;

  update_time_ns_ = target.update_time_ns_;
  std::memcpy(source_id_, target.source_id_, sizeof(source_id_));
  channel_ = target.channel_;
  freq_ = target.freq_;
  count_ = target.count_;
  pack_size_ = target.pack_size_;
  reserved_buf_ = target.reserved_buf_;
  reserved_buf2_ = target.reserved_buf2_;
  reserved_buf3_ = target.reserved_buf3_;
  reserved_buf4_ = target.reserved_buf4_;
  reserved_buf5_ = target.reserved_buf5_;
  is_owner_ = false;
  capacity_ = 0;
  data_ = target.data_;

  return true;
}

bool ObjectArray::deep_copy(const ObjectArray& target) noexcept {
  const size_t target_size = static_cast<size_t>(target.count_) * target.pack_size_;

  if VLIKELY (data_ && is_owner_ && target.data_ && capacity_ != 0 && capacity_ == target_size) {
    const auto current = reinterpret_cast<uintptr_t>(data_);
    const auto source = reinterpret_cast<uintptr_t>(target.data_);

    if VUNLIKELY (source >= current && source - current < capacity_) {
      return false;
    }

    header = target.header;

    update_time_ns_ = target.update_time_ns_;
    std::memcpy(source_id_, target.source_id_, sizeof(source_id_));
    channel_ = target.channel_;
    freq_ = target.freq_;
    count_ = target.count_;
    pack_size_ = target.pack_size_;
    reserved_buf_ = target.reserved_buf_;
    reserved_buf2_ = target.reserved_buf2_;
    reserved_buf3_ = target.reserved_buf3_;
    reserved_buf4_ = target.reserved_buf4_;
    reserved_buf5_ = target.reserved_buf5_;

    std::memcpy(data_, target.data_, target_size);

    return true;
  }

  if VUNLIKELY (!shallow_copy(target)) {
    return false;
  }

  if (data_ && target_size != 0) {
    auto* target_data = data_;
    capacity_ = target_size;
    data_ = Bytes::bytes_malloc(capacity_);

    if VUNLIKELY (!data_) {
      count_ = 0;
      capacity_ = 0;
      return false;
    }

    std::memcpy(data_, target_data, target_size);
    is_owner_ = true;
  }

  return true;
}

bool ObjectArray::move_copy(ObjectArray& target) noexcept {
  if VUNLIKELY (!shallow_copy(target)) {
    return false;
  }

  is_owner_ = target.is_owner_;
  capacity_ = target.capacity_;

  target.update_time_ns_ = 0;
  std::memset(target.source_id_, 0, sizeof(target.source_id_));
  target.channel_ = 0;
  target.freq_ = 0;
  target.count_ = 0;
  target.pack_size_ = 0;
  target.reserved_buf_ = 0;
  target.reserved_buf2_ = 0;
  target.reserved_buf3_ = 0;
  target.reserved_buf4_ = 0;
  target.reserved_buf5_ = 0;
  target.is_owner_ = false;
  target.data_ = nullptr;
  target.capacity_ = 0;

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

bool ObjectArray::create(size_t count) noexcept {
  if VUNLIKELY (count == 0) {
    return false;
  }

  if VUNLIKELY (count > std::numeric_limits<size_t>::max() / sizeof(Object)) {
    return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if (is_owner_ && data_ && capacity_ != 0) {
    Bytes::bytes_free(data_, capacity_);
  }

  pack_size_ = sizeof(Object);
  capacity_ = count * pack_size_;
  count_ = 0;
  data_ = Bytes::bytes_malloc(capacity_);

  if VUNLIKELY (!data_) {
    capacity_ = 0;
    is_owner_ = false;
    return false;
  }

  is_owner_ = true;

  return true;
}

void ObjectArray::clear() noexcept {
  if (is_owner_ && data_ && capacity_ != 0) {
    Bytes::bytes_free(data_, capacity_);
  }

  update_time_ns_ = 0;
  std::memset(source_id_, 0, sizeof(source_id_));
  channel_ = 0;
  freq_ = 0;
  count_ = 0;
  pack_size_ = 0;
  is_owner_ = false;
  data_ = nullptr;
  capacity_ = 0;

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

bool ObjectArray::push_value(const Object& object) noexcept {
  if VUNLIKELY (!is_owner_ || !data_ || pack_size_ == 0 || capacity_ == 0) {
    return false;
  }

  if VUNLIKELY ((static_cast<size_t>(count_) + 1) * pack_size_ > capacity_) {
    return false;
  }

  if VUNLIKELY (pack_size_ != sizeof(Object)) {
    return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  std::memcpy(data_ + (static_cast<size_t>(count_) * pack_size_), &object, sizeof(Object));

  ++count_;

  return true;
}

bool ObjectArray::set_value(uint32_t index, const Object& object) noexcept {
  if VUNLIKELY (!is_owner_ || !data_ || pack_size_ == 0 || capacity_ == 0) {
    return false;
  }

  if VUNLIKELY (index >= count_) {
    return false;
  }

  if VUNLIKELY (pack_size_ != sizeof(Object)) {
    return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  std::memcpy(data_ + (static_cast<size_t>(index) * pack_size_), &object, sizeof(Object));

  return true;
}

bool ObjectArray::get_value(uint32_t index, Object& object) const noexcept {
  if VUNLIKELY (!data_ || pack_size_ == 0) {
    object = Object{};
    return false;
  }

  if VUNLIKELY (index >= count_) {
    object = Object{};
    return false;
  }

  if VUNLIKELY (pack_size_ != sizeof(Object)) {
    object = Object{};  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;       // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  std::memcpy(&object, data_ + (static_cast<size_t>(index) * pack_size_), sizeof(Object));

  return true;
}

ObjectArray::Object ObjectArray::get_value(uint32_t index) const noexcept {
  Object object;

  get_value(index, object);

  return object;
}

bool ObjectArray::resize(uint32_t count) noexcept {
  if VUNLIKELY (!is_owner_ || !data_ || pack_size_ == 0 || capacity_ == 0) {
    return false;
  }

  if VUNLIKELY (static_cast<size_t>(count) * pack_size_ > capacity_) {
    return false;
  }

  count_ = count;

  return true;
}

uint64_t ObjectArray::update_time_ns() const noexcept { return update_time_ns_; }

std::string_view ObjectArray::source_id() const noexcept {
  return {source_id_, ::strnlen(source_id_, sizeof(source_id_))};
}

uint32_t ObjectArray::channel() const noexcept { return channel_; }

uint32_t ObjectArray::freq() const noexcept { return freq_; }

uint32_t ObjectArray::count() const noexcept { return count_; }

uint32_t ObjectArray::pack_size() const noexcept { return pack_size_; }

const ObjectArray::Object* ObjectArray::objects(uint32_t index) const noexcept {
  if VUNLIKELY (!data_ || pack_size_ != sizeof(Object) || index >= count_) {
    return nullptr;
  }

  return reinterpret_cast<const Object*>(data_ + (static_cast<size_t>(index) * pack_size_));
}

const uint8_t* ObjectArray::data() const noexcept { return data_; }

size_t ObjectArray::capacity() const noexcept { return capacity_; }

bool ObjectArray::is_owner() const noexcept { return is_owner_; }

void ObjectArray::set_update_time_ns(uint64_t update_time_ns) noexcept { update_time_ns_ = update_time_ns; }

void ObjectArray::set_source_id(std::string_view source_id) noexcept {
  std::memset(source_id_, 0, sizeof(source_id_));

  size_t copy_size = source_id.size();

  if (copy_size >= sizeof(source_id_)) {
    copy_size = sizeof(source_id_) - 1;
  }

  if VLIKELY (copy_size != 0) {
    std::memcpy(source_id_, source_id.data(), copy_size);
  }
}

void ObjectArray::set_channel(uint32_t channel) noexcept { channel_ = channel; }

void ObjectArray::set_freq(uint32_t freq) noexcept { freq_ = freq; }

}  // namespace zerocopy

}  // namespace vlink
