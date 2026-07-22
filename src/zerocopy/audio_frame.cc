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

#include "./zerocopy/audio_frame.h"

#include <cstdint>

namespace vlink {

namespace zerocopy {

// AudioFrame
AudioFrame::AudioFrame() noexcept {
#if defined(__arm__) || defined(__x86__) || defined(__i386__)
#ifndef __ANDROID__
#warning "[AudioFrame] No support for 32-bit architecture."
#endif
#else
  static_assert(sizeof(AudioFrame) == 128, "Sizeof must be 128 bytes.");
#endif
}

AudioFrame::~AudioFrame() noexcept {
  if (is_owner_ && data_ && size_ != 0) {
    Bytes::bytes_free(data_, size_);
  }
}

AudioFrame::AudioFrame(const AudioFrame& target) noexcept { deep_copy(target); }

AudioFrame::AudioFrame(AudioFrame&& target) noexcept { move_copy(target); }

AudioFrame& AudioFrame::operator=(const AudioFrame& target) noexcept {
  if VUNLIKELY (this == &target) {
    return *this;
  }

  deep_copy(target);

  return *this;
}

AudioFrame& AudioFrame::operator=(AudioFrame&& target) noexcept {
  if VUNLIKELY (this == &target) {
    return *this;
  }

  move_copy(target);

  return *this;
}

bool AudioFrame::operator<<(const Bytes& bytes) noexcept {
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

  std::memcpy(target_ptr, bytes.data() + kMagicNumberBeginSize + kVersionSize, sizeof(AudioFrame));

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

  data_ = const_cast<uint8_t*>(bytes.data() + kMagicNumberBeginSize + kVersionSize + sizeof(AudioFrame));
  is_owner_ = false;

  if VUNLIKELY (bytes.size() != get_serialized_size()) {
    clear();
    return false;
  }

  return true;
}

bool AudioFrame::operator>>(Bytes& bytes) const noexcept {
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
  std::memcpy(bytes.data() + kMagicNumberBeginSize + kVersionSize, this, sizeof(AudioFrame));

  const auto data_offset = reinterpret_cast<const uint8_t*>(&data_) - reinterpret_cast<const uint8_t*>(this);
  const size_t data_pointer_size = sizeof(data_);
  std::memset(bytes.data() + kMagicNumberBeginSize + kVersionSize + data_offset, 0, data_pointer_size);

  if VLIKELY (data_ != nullptr && size_ != 0) {
    std::memcpy(bytes.data() + kMagicNumberBeginSize + kVersionSize + sizeof(AudioFrame), data_, size_);
  }

  std::memcpy(bytes.data() + kMagicNumberBeginSize + kVersionSize + sizeof(AudioFrame) + size_, &kMagicNumberEnd,
              kMagicNumberEndSize);

  return true;
}

bool AudioFrame::check_valid(const Bytes& bytes) noexcept {
  static constexpr size_t kMagicNumberBeginSize = sizeof(kMagicNumberBegin);
  static constexpr size_t kVersionSize = sizeof(kWireVersion);
  static constexpr size_t kMagicNumberEndSize = sizeof(kMagicNumberEnd);

  if VUNLIKELY (bytes.size() < kMagicNumberBeginSize + kVersionSize + sizeof(AudioFrame) + kMagicNumberEndSize) {
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

size_t AudioFrame::get_serialized_size() const noexcept {
  static constexpr size_t kMagicNumberBeginSize = sizeof(kMagicNumberBegin);
  static constexpr size_t kVersionSize = sizeof(kWireVersion);
  static constexpr size_t kMagicNumberEndSize = sizeof(kMagicNumberEnd);

  return kMagicNumberBeginSize + kVersionSize + sizeof(AudioFrame) + size_ + kMagicNumberEndSize;
}

bool AudioFrame::is_valid() const noexcept { return data_ != nullptr && size_ != 0; }

bool AudioFrame::shallow_copy(const AudioFrame& target) noexcept {
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
  duration_ns_ = target.duration_ns_;
  std::memcpy(codec_, target.codec_, sizeof(codec_));
  std::memcpy(language_, target.language_, sizeof(language_));
  channel_ = target.channel_;
  freq_ = target.freq_;
  sample_rate_ = target.sample_rate_;
  num_samples_ = target.num_samples_;
  bitrate_ = target.bitrate_;
  num_channels_ = target.num_channels_;
  bit_depth_ = target.bit_depth_;
  format_ = target.format_;
  layout_ = target.layout_;
  reserved_buf_ = target.reserved_buf_;
  reserved_buf2_ = target.reserved_buf2_;
  is_owner_ = false;
  data_ = target.data_;
  size_ = target.size_;

  return true;
}

bool AudioFrame::deep_copy(const AudioFrame& target) noexcept {
  if VLIKELY (data_ && is_owner_ && target.data_ && size_ != 0 && size_ == target.size_) {
    const auto current = reinterpret_cast<uintptr_t>(data_);
    const auto source = reinterpret_cast<uintptr_t>(target.data_);

    if VUNLIKELY (source >= current && source - current < size_) {
      return false;
    }

    header = target.header;

    update_time_ns_ = target.update_time_ns_;
    duration_ns_ = target.duration_ns_;
    std::memcpy(codec_, target.codec_, sizeof(codec_));
    std::memcpy(language_, target.language_, sizeof(language_));
    channel_ = target.channel_;
    freq_ = target.freq_;
    sample_rate_ = target.sample_rate_;
    num_samples_ = target.num_samples_;
    bitrate_ = target.bitrate_;
    num_channels_ = target.num_channels_;
    bit_depth_ = target.bit_depth_;
    format_ = target.format_;
    layout_ = target.layout_;
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
      size_ = 0;
      return false;
    }

    std::memcpy(data_, target_data, size_);
    is_owner_ = true;
  }

  return true;
}

bool AudioFrame::move_copy(AudioFrame& target) noexcept {
  if VUNLIKELY (!shallow_copy(target)) {
    return false;
  }

  is_owner_ = target.is_owner_;

  target.update_time_ns_ = 0;
  target.duration_ns_ = 0;
  std::memset(target.codec_, 0, sizeof(target.codec_));
  std::memset(target.language_, 0, sizeof(target.language_));
  target.channel_ = 0;
  target.freq_ = 0;
  target.sample_rate_ = 0;
  target.num_samples_ = 0;
  target.bitrate_ = 0;
  target.num_channels_ = 0;
  target.bit_depth_ = 0;
  target.format_ = kFormatUnknown;
  target.layout_ = kLayoutUnknown;
  target.reserved_buf_ = 0;
  target.reserved_buf2_ = 0;
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

bool AudioFrame::create(size_t _size) noexcept {
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

void AudioFrame::clear() noexcept {
  if (is_owner_ && data_ && size_ != 0) {
    Bytes::bytes_free(data_, size_);
  }

  update_time_ns_ = 0;
  duration_ns_ = 0;
  std::memset(codec_, 0, sizeof(codec_));
  std::memset(language_, 0, sizeof(language_));
  channel_ = 0;
  freq_ = 0;
  sample_rate_ = 0;
  num_samples_ = 0;
  bitrate_ = 0;
  num_channels_ = 0;
  bit_depth_ = 0;
  format_ = kFormatUnknown;
  layout_ = kLayoutUnknown;
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

bool AudioFrame::shallow_copy(uint8_t* data, size_t size) noexcept {
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

bool AudioFrame::deep_copy(uint8_t* data, size_t size) noexcept {
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

bool AudioFrame::fill_data(uint8_t* data, size_t size) noexcept { return deep_copy(data, size); }

uint64_t AudioFrame::update_time_ns() const noexcept { return update_time_ns_; }

uint64_t AudioFrame::duration_ns() const noexcept { return duration_ns_; }

std::string_view AudioFrame::codec() const noexcept { return {codec_, ::strnlen(codec_, sizeof(codec_))}; }

std::string_view AudioFrame::language() const noexcept { return {language_, ::strnlen(language_, sizeof(language_))}; }

uint32_t AudioFrame::channel() const noexcept { return channel_; }

uint32_t AudioFrame::freq() const noexcept { return freq_; }

uint32_t AudioFrame::sample_rate() const noexcept { return sample_rate_; }

uint32_t AudioFrame::num_samples() const noexcept { return num_samples_; }

uint32_t AudioFrame::bitrate() const noexcept { return bitrate_; }

uint16_t AudioFrame::num_channels() const noexcept { return num_channels_; }

uint16_t AudioFrame::bit_depth() const noexcept { return bit_depth_; }

AudioFrame::Format AudioFrame::format() const noexcept { return format_; }

AudioFrame::Layout AudioFrame::layout() const noexcept { return layout_; }

const uint8_t* AudioFrame::data() const noexcept { return data_; }

size_t AudioFrame::size() const noexcept { return size_; }

bool AudioFrame::is_owner() const noexcept { return is_owner_; }

void AudioFrame::set_update_time_ns(uint64_t update_time_ns) noexcept { update_time_ns_ = update_time_ns; }

void AudioFrame::set_duration_ns(uint64_t duration_ns) noexcept { duration_ns_ = duration_ns; }

void AudioFrame::set_codec(std::string_view codec) noexcept {
  std::memset(codec_, 0, sizeof(codec_));

  size_t copy_size = codec.size();

  if (copy_size >= sizeof(codec_)) {
    copy_size = sizeof(codec_) - 1;
  }

  if VLIKELY (copy_size != 0) {
    std::memcpy(codec_, codec.data(), copy_size);
  }
}

void AudioFrame::set_language(std::string_view language) noexcept {
  std::memset(language_, 0, sizeof(language_));

  size_t copy_size = language.size();

  if (copy_size >= sizeof(language_)) {
    copy_size = sizeof(language_) - 1;
  }

  if VLIKELY (copy_size != 0) {
    std::memcpy(language_, language.data(), copy_size);
  }
}

void AudioFrame::set_channel(uint32_t channel) noexcept { channel_ = channel; }

void AudioFrame::set_freq(uint32_t freq) noexcept { freq_ = freq; }

void AudioFrame::set_sample_rate(uint32_t sample_rate) noexcept { sample_rate_ = sample_rate; }

void AudioFrame::set_num_samples(uint32_t num_samples) noexcept { num_samples_ = num_samples; }

void AudioFrame::set_bitrate(uint32_t bitrate) noexcept { bitrate_ = bitrate; }

void AudioFrame::set_num_channels(uint16_t num_channels) noexcept { num_channels_ = num_channels; }

void AudioFrame::set_bit_depth(uint16_t bit_depth) noexcept { bit_depth_ = bit_depth; }

void AudioFrame::set_format(Format format) noexcept { format_ = format; }

void AudioFrame::set_layout(Layout layout) noexcept { layout_ = layout; }

}  // namespace zerocopy

}  // namespace vlink
