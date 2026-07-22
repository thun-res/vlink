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

#include "./zerocopy/tensor.h"

#include <cstdint>
#include <limits>

namespace vlink {

namespace zerocopy {

// Tensor
Tensor::Tensor() noexcept {
#if defined(__arm__) || defined(__x86__) || defined(__i386__)
#ifndef __ANDROID__
#warning "[Tensor] No support for 32-bit architecture."
#endif
#else
  static_assert(sizeof(Tensor) == 248, "Sizeof must be 248 bytes.");
#endif
}

Tensor::~Tensor() noexcept {
  if (is_owner_ && data_ && size_ != 0) {
    Bytes::bytes_free(data_, size_);
  }
}

Tensor::Tensor(const Tensor& target) noexcept { deep_copy(target); }

Tensor::Tensor(Tensor&& target) noexcept { move_copy(target); }

Tensor& Tensor::operator=(const Tensor& target) noexcept {
  if VUNLIKELY (this == &target) {
    return *this;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  deep_copy(target);

  return *this;
}

Tensor& Tensor::operator=(Tensor&& target) noexcept {
  if VUNLIKELY (this == &target) {
    return *this;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  move_copy(target);

  return *this;
}

bool Tensor::operator<<(const Bytes& bytes) noexcept {
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

  std::memcpy(target_ptr, bytes.data() + kMagicNumberBeginSize + kVersionSize, sizeof(Tensor));

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

  data_ = const_cast<uint8_t*>(bytes.data() + kMagicNumberBeginSize + kVersionSize + sizeof(Tensor));
  is_owner_ = false;

  if VUNLIKELY (bytes.size() != get_serialized_size()) {
    clear();
    return false;
  }

  if VUNLIKELY (rank_ > kMaxRank) {
    rank_ = kMaxRank;
  }

  element_size_ = element_size_of(dtype_);

  return true;
}

bool Tensor::operator>>(Bytes& bytes) const noexcept {
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
  std::memcpy(bytes.data() + kMagicNumberBeginSize + kVersionSize, this, sizeof(Tensor));

  const auto data_offset = reinterpret_cast<const uint8_t*>(&data_) - reinterpret_cast<const uint8_t*>(this);
  const size_t data_pointer_size = sizeof(data_);
  std::memset(bytes.data() + kMagicNumberBeginSize + kVersionSize + data_offset, 0, data_pointer_size);

  if VLIKELY (data_ != nullptr && size_ != 0) {
    std::memcpy(bytes.data() + kMagicNumberBeginSize + kVersionSize + sizeof(Tensor), data_, size_);
  }

  std::memcpy(bytes.data() + kMagicNumberBeginSize + kVersionSize + sizeof(Tensor) + size_, &kMagicNumberEnd,
              kMagicNumberEndSize);

  return true;
}

bool Tensor::check_valid(const Bytes& bytes) noexcept {
  static constexpr size_t kMagicNumberBeginSize = sizeof(kMagicNumberBegin);
  static constexpr size_t kVersionSize = sizeof(kWireVersion);
  static constexpr size_t kMagicNumberEndSize = sizeof(kMagicNumberEnd);

  if VUNLIKELY (bytes.size() < kMagicNumberBeginSize + kVersionSize + sizeof(Tensor) + kMagicNumberEndSize) {
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

size_t Tensor::get_serialized_size() const noexcept {
  static constexpr size_t kMagicNumberBeginSize = sizeof(kMagicNumberBegin);
  static constexpr size_t kVersionSize = sizeof(kWireVersion);
  static constexpr size_t kMagicNumberEndSize = sizeof(kMagicNumberEnd);

  return kMagicNumberBeginSize + kVersionSize + sizeof(Tensor) + size_ + kMagicNumberEndSize;
}

bool Tensor::is_valid() const noexcept { return data_ != nullptr && size_ != 0; }

bool Tensor::shallow_copy(const Tensor& target) noexcept {
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

  header = target.header;

  update_time_ns_ = target.update_time_ns_;
  num_elements_ = target.num_elements_;
  std::memcpy(name_, target.name_, sizeof(name_));
  std::memcpy(model_id_, target.model_id_, sizeof(model_id_));
  std::memcpy(layout_, target.layout_, sizeof(layout_));
  std::memcpy(shape_, target.shape_, sizeof(shape_));
  std::memcpy(strides_, target.strides_, sizeof(strides_));
  channel_ = target.channel_;
  freq_ = target.freq_;
  batch_size_ = target.batch_size_;
  quant_scale_ = target.quant_scale_;
  quant_zero_point_ = target.quant_zero_point_;
  dtype_ = target.dtype_;
  rank_ = target.rank_;
  device_ = target.device_;
  element_size_ = target.element_size_;
  reserved_buf_ = target.reserved_buf_;
  reserved_buf2_ = target.reserved_buf2_;
  reserved_buf3_ = target.reserved_buf3_;
  is_owner_ = false;
  data_ = target.data_;
  size_ = target.size_;

  return true;
}

bool Tensor::deep_copy(const Tensor& target) noexcept {
  if VLIKELY (data_ && is_owner_ && target.data_ && size_ != 0 && size_ == target.size_) {
    const auto current = reinterpret_cast<uintptr_t>(data_);
    const auto source = reinterpret_cast<uintptr_t>(target.data_);

    if VUNLIKELY (source >= current && source - current < size_) {
      return false;
    }

    header = target.header;

    update_time_ns_ = target.update_time_ns_;
    num_elements_ = target.num_elements_;
    std::memcpy(name_, target.name_, sizeof(name_));
    std::memcpy(model_id_, target.model_id_, sizeof(model_id_));
    std::memcpy(layout_, target.layout_, sizeof(layout_));
    std::memcpy(shape_, target.shape_, sizeof(shape_));
    std::memcpy(strides_, target.strides_, sizeof(strides_));
    channel_ = target.channel_;
    freq_ = target.freq_;
    batch_size_ = target.batch_size_;
    quant_scale_ = target.quant_scale_;
    quant_zero_point_ = target.quant_zero_point_;
    dtype_ = target.dtype_;
    rank_ = target.rank_;
    device_ = target.device_;
    element_size_ = target.element_size_;
    reserved_buf_ = target.reserved_buf_;
    reserved_buf2_ = target.reserved_buf2_;
    reserved_buf3_ = target.reserved_buf3_;

    std::memcpy(data_, target.data_, size_);

    return true;
  }

  if VUNLIKELY (!shallow_copy(target)) {
    return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if (data_ && size_ != 0) {
    auto* target_data = data_;
    data_ = Bytes::bytes_malloc(size_);

    if VUNLIKELY (!data_) {
      size_ = 0;
      return false;
    }

    std::memcpy(data_, target_data, size_);
    is_owner_ = true;
  }

  return true;
}

bool Tensor::move_copy(Tensor& target) noexcept {
  if VUNLIKELY (!shallow_copy(target)) {
    return false;
  }

  is_owner_ = target.is_owner_;

  target.update_time_ns_ = 0;
  target.num_elements_ = 0;
  std::memset(target.name_, 0, sizeof(target.name_));
  std::memset(target.model_id_, 0, sizeof(target.model_id_));
  std::memset(target.layout_, 0, sizeof(target.layout_));
  std::memset(target.shape_, 0, sizeof(target.shape_));
  std::memset(target.strides_, 0, sizeof(target.strides_));
  target.channel_ = 0;
  target.freq_ = 0;
  target.batch_size_ = 0;
  target.quant_scale_ = 0;
  target.quant_zero_point_ = 0;
  target.dtype_ = kDataUnknown;
  target.rank_ = 0;
  target.device_ = kDeviceCpu;
  target.element_size_ = 0;
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

bool Tensor::create(size_t _size) noexcept {
  if VUNLIKELY (_size == 0) {
    return false;
  }

  if (is_owner_ && data_ && size_ != 0) {
    Bytes::bytes_free(data_, size_);
  }

  data_ = Bytes::bytes_malloc(_size);

  if VUNLIKELY (!data_) {
    size_ = 0;
    is_owner_ = false;
    return false;
  }

  size_ = _size;
  is_owner_ = true;

  return true;
}

void Tensor::clear() noexcept {
  if (is_owner_ && data_ && size_ != 0) {
    Bytes::bytes_free(data_, size_);
  }

  update_time_ns_ = 0;
  num_elements_ = 0;
  std::memset(name_, 0, sizeof(name_));
  std::memset(model_id_, 0, sizeof(model_id_));
  std::memset(layout_, 0, sizeof(layout_));
  std::memset(shape_, 0, sizeof(shape_));
  std::memset(strides_, 0, sizeof(strides_));
  channel_ = 0;
  freq_ = 0;
  batch_size_ = 0;
  quant_scale_ = 0;
  quant_zero_point_ = 0;
  dtype_ = kDataUnknown;
  rank_ = 0;
  device_ = kDeviceCpu;
  element_size_ = 0;
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

bool Tensor::shallow_copy(uint8_t* data, size_t size) noexcept {
  if VUNLIKELY (!data || size == 0) {
    return false;
  }

  if VUNLIKELY (data_ == data) {
    return false;
  }

  if (is_owner_ && data_ && size_ != 0) {
    const auto current = reinterpret_cast<uintptr_t>(data_);
    const auto source = reinterpret_cast<uintptr_t>(data);

    if VUNLIKELY (source >= current && source - current < size_) {
      return false;
    }

    Bytes::bytes_free(data_, size_);
  }

  is_owner_ = false;

  data_ = data;
  size_ = size;

  return true;
}

bool Tensor::deep_copy(uint8_t* data, size_t size) noexcept {
  if VUNLIKELY (!data || size == 0) {
    return false;
  }

  if (is_owner_) {
    if VUNLIKELY (!data_) {
      return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    const auto current = reinterpret_cast<uintptr_t>(data_);
    const auto source = reinterpret_cast<uintptr_t>(data);

    if VUNLIKELY (source >= current && source - current < size_) {
      return false;
    }

    if VUNLIKELY (size_ != size && !create(size)) {
      return false;
    }
  } else if VUNLIKELY (!create(size)) {
    return false;
  }

  std::memcpy(data_, data, size);

  return true;
}

bool Tensor::fill_data(uint8_t* data, size_t size) noexcept { return deep_copy(data, size); }

uint64_t Tensor::update_time_ns() const noexcept { return update_time_ns_; }

uint64_t Tensor::num_elements() const noexcept { return num_elements_; }

std::string_view Tensor::name() const noexcept { return {name_, ::strnlen(name_, sizeof(name_))}; }

std::string_view Tensor::model_id() const noexcept { return {model_id_, ::strnlen(model_id_, sizeof(model_id_))}; }

std::string_view Tensor::layout() const noexcept { return {layout_, ::strnlen(layout_, sizeof(layout_))}; }

const uint32_t* Tensor::shape() const noexcept { return shape_; }

uint32_t Tensor::shape_at(uint8_t dim) const noexcept {
  if VUNLIKELY (dim >= kMaxRank) {
    return 0;
  }

  return shape_[dim];
}

const uint32_t* Tensor::strides() const noexcept { return strides_; }

uint32_t Tensor::stride_at(uint8_t dim) const noexcept {
  if VUNLIKELY (dim >= kMaxRank) {
    return 0;
  }

  return strides_[dim];
}

uint32_t Tensor::channel() const noexcept { return channel_; }

uint32_t Tensor::freq() const noexcept { return freq_; }

uint32_t Tensor::batch_size() const noexcept { return batch_size_; }

float Tensor::quant_scale() const noexcept { return quant_scale_; }

int32_t Tensor::quant_zero_point() const noexcept { return quant_zero_point_; }

Tensor::DataType Tensor::dtype() const noexcept { return dtype_; }

uint8_t Tensor::rank() const noexcept { return rank_; }

Tensor::Device Tensor::device() const noexcept { return device_; }

uint8_t Tensor::element_size() const noexcept { return element_size_; }

const uint8_t* Tensor::data() const noexcept { return data_; }

size_t Tensor::size() const noexcept { return size_; }

bool Tensor::is_owner() const noexcept { return is_owner_; }

void Tensor::set_update_time_ns(uint64_t update_time_ns) noexcept { update_time_ns_ = update_time_ns; }

void Tensor::set_name(std::string_view name) noexcept {
  std::memset(name_, 0, sizeof(name_));

  size_t copy_size = name.size();

  if (copy_size >= sizeof(name_)) {
    copy_size = sizeof(name_) - 1;
  }

  if VLIKELY (copy_size != 0) {
    std::memcpy(name_, name.data(), copy_size);
  }
}

void Tensor::set_model_id(std::string_view model_id) noexcept {
  std::memset(model_id_, 0, sizeof(model_id_));

  size_t copy_size = model_id.size();

  if (copy_size >= sizeof(model_id_)) {
    copy_size = sizeof(model_id_) - 1;
  }

  if VLIKELY (copy_size != 0) {
    std::memcpy(model_id_, model_id.data(), copy_size);
  }
}

void Tensor::set_layout(std::string_view layout) noexcept {
  std::memset(layout_, 0, sizeof(layout_));

  size_t copy_size = layout.size();

  if (copy_size >= sizeof(layout_)) {
    copy_size = sizeof(layout_) - 1;
  }

  if VLIKELY (copy_size != 0) {
    std::memcpy(layout_, layout.data(), copy_size);
  }
}

void Tensor::set_shape(const uint32_t* shape, uint8_t rank) noexcept {
  std::memset(shape_, 0, sizeof(shape_));
  std::memset(strides_, 0, sizeof(strides_));

  if VUNLIKELY (rank == 0 || !shape) {
    rank_ = 0;
    num_elements_ = 0;
    batch_size_ = 0;
    return;
  }

  if (rank > kMaxRank) {
    rank = kMaxRank;
  }

  uint64_t total = 1;
  bool has_zero = false;
  bool total_overflow = false;

  for (uint8_t i = 0; i < rank; ++i) {
    shape_[i] = shape[i];

    if (shape[i] == 0) {
      has_zero = true;
      total = 0;
    } else if (!has_zero && !total_overflow) {
      if VUNLIKELY (total > std::numeric_limits<uint64_t>::max() / shape[i]) {
        total_overflow = true;
      } else {
        total *= shape[i];
      }
    }
  }

  if VUNLIKELY (total_overflow && !has_zero) {
    std::memset(shape_, 0, sizeof(shape_));
    std::memset(strides_, 0, sizeof(strides_));
    rank_ = 0;
    num_elements_ = 0;
    batch_size_ = 0;
    return;
  }

  uint64_t running = 1;

  for (uint8_t i = rank; i > 0; --i) {
    if VUNLIKELY (running > std::numeric_limits<uint32_t>::max()) {
      std::memset(shape_, 0, sizeof(shape_));
      std::memset(strides_, 0, sizeof(strides_));
      rank_ = 0;
      num_elements_ = 0;
      batch_size_ = 0;
      return;
    }

    strides_[i - 1] = static_cast<uint32_t>(running);

    if (shape_[i - 1] == 0) {
      running = 0;
    } else if (i > 1) {
      running *= shape_[i - 1];
    }
  }

  rank_ = rank;
  num_elements_ = total;
  batch_size_ = shape_[0];
}

void Tensor::set_shape_at(uint8_t dim, uint32_t value) noexcept {
  if VUNLIKELY (dim >= kMaxRank) {
    return;
  }

  shape_[dim] = value;
}

void Tensor::set_stride_at(uint8_t dim, uint32_t value) noexcept {
  if VUNLIKELY (dim >= kMaxRank) {
    return;
  }

  strides_[dim] = value;
}

void Tensor::set_channel(uint32_t channel) noexcept { channel_ = channel; }

void Tensor::set_freq(uint32_t freq) noexcept { freq_ = freq; }

void Tensor::set_batch_size(uint32_t batch_size) noexcept { batch_size_ = batch_size; }

void Tensor::set_quant_scale(float quant_scale) noexcept { quant_scale_ = quant_scale; }

void Tensor::set_quant_zero_point(int32_t quant_zero_point) noexcept { quant_zero_point_ = quant_zero_point; }

void Tensor::set_dtype(DataType dtype) noexcept {
  dtype_ = dtype;
  element_size_ = element_size_of(dtype);
}

void Tensor::set_device(Device device) noexcept { device_ = device; }

uint8_t Tensor::element_size_of(DataType dtype) noexcept {
  uint8_t target_size = 0;

  if (dtype == kBool || dtype == kInt8 || dtype == kUint8) {
    target_size = 1;
  } else if (dtype == kInt16 || dtype == kUint16 || dtype == kFloat16 || dtype == kBfloat16) {
    target_size = 2;
  } else if (dtype == kInt32 || dtype == kUint32 || dtype == kFloat32) {
    target_size = 4;
  } else if (dtype == kInt64 || dtype == kUint64 || dtype == kFloat64) {
    target_size = 8;
  }

  return target_size;
}

}  // namespace zerocopy

}  // namespace vlink
