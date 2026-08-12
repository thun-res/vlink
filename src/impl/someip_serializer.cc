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

#include "./impl/someip_serializer.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace vlink {

namespace SomeipSerializer {  // NOLINT(readability-identifier-naming)

static constexpr uint8_t kUtf8Bom[] = {0xEF, 0xBB, 0xBF};
static constexpr size_t kHeaderSize = 16U;

static bool is_valid_utf8(const uint8_t* data, size_t size) noexcept {
  size_t position = 0;

  while (position < size) {
    const uint8_t first = data[position];

    if VLIKELY (first <= 0x7FU) {
      ++position;
      continue;
    }

    size_t continuation_count = 0;
    uint8_t second_min = 0x80U;
    uint8_t second_max = 0xBFU;

    if (first >= 0xC2U && first <= 0xDFU) {
      continuation_count = 1U;
    } else if (first >= 0xE0U && first <= 0xEFU) {
      continuation_count = 2U;
      second_min = first == 0xE0U ? 0xA0U : 0x80U;
      second_max = first == 0xEDU ? 0x9FU : 0xBFU;
    } else if (first >= 0xF0U && first <= 0xF4U) {
      continuation_count = 3U;
      second_min = first == 0xF0U ? 0x90U : 0x80U;
      second_max = first == 0xF4U ? 0x8FU : 0xBFU;
    } else {
      return false;
    }

    if VUNLIKELY (continuation_count > size - position - 1U) {
      return false;
    }

    const uint8_t second = data[position + 1U];

    if VUNLIKELY (second < second_min || second > second_max) {
      return false;
    }

    for (size_t index = 2U; index <= continuation_count; ++index) {
      const uint8_t continuation = data[position + index];

      if VUNLIKELY (continuation < 0x80U || continuation > 0xBFU) {
        return false;
      }
    }

    position += continuation_count + 1U;
  }

  return true;
}

Writer::Writer(uint8_t* data, size_t capacity, size_t alignment, Endian endian) noexcept
    : data_(data), capacity_(std::min(capacity, kMaxPayloadSize)), alignment_(alignment), endian_(endian) {}

size_t Writer::position() const noexcept { return position_; }

bool Writer::append(const uint8_t* data, size_t size) noexcept {
  if VUNLIKELY (position_ > capacity_ || size > capacity_ - position_) {
    return false;
  }

  if (data_ && size > 0U) {
    std::memcpy(data_ + position_, data, size);
  }

  position_ += size;

  return true;
}

bool Writer::append_unsigned(uint64_t value, size_t width) noexcept {
  if VUNLIKELY (width == 0U || width > sizeof(value) || position_ > capacity_ || width > capacity_ - position_) {
    return false;
  }

  for (size_t index = 0; index < width; ++index) {
    const size_t shift = (endian_ == Endian::kBig ? width - index - 1U : index) * 8U;

    if (data_) {
      data_[position_ + index] = static_cast<uint8_t>((value >> shift) & 0xFFU);
    }
  }

  position_ += width;

  return true;
}

bool Writer::begin_length_delimited(size_t& length_position, size_t& data_position, size_t width) noexcept {
  if VUNLIKELY (width != 1U && width != 2U && width != 4U) {
    return false;
  }

  length_position = position_;

  if VUNLIKELY (position_ > capacity_ || width > capacity_ - position_) {
    return false;
  }

  position_ += width;
  data_position = position_;

  return true;
}

bool Writer::end_length_delimited(size_t length_position, size_t data_position, size_t width) noexcept {
  if VUNLIKELY ((width != 1U && width != 2U && width != 4U) || position_ < data_position) {
    return false;
  }

  const size_t byte_length = position_ - data_position;
  const uint64_t max_length = (uint64_t{1U} << (width * 8U)) - 1U;

  if VUNLIKELY (byte_length > max_length || length_position > capacity_ || width > capacity_ - length_position) {
    return false;
  }

  if (data_) {
    for (size_t index = 0U; index < width; ++index) {
      const size_t shift = (width - index - 1U) * 8U;
      data_[length_position + index] = static_cast<uint8_t>((byte_length >> shift) & 0xFFU);
    }
  }

  return true;
}

bool Writer::align() noexcept {
  if VLIKELY (alignment_ == 1U) {
    return true;
  }

  const size_t mask = alignment_ - 1U;
  const size_t remainder = ((position_ & mask) + (kHeaderSize & mask)) & mask;
  const size_t padding = (alignment_ - remainder) & mask;

  if VUNLIKELY (position_ > capacity_ || padding > capacity_ - position_) {
    return false;
  }

  if (data_ && padding > 0U) {
    std::memset(data_ + position_, 0, padding);
  }

  position_ += padding;

  return true;
}

Reader::Reader(const uint8_t* data, size_t size, size_t alignment, Endian endian) noexcept
    : data_(data), size_(size), alignment_(alignment), endian_(endian) {}

size_t Reader::position() const noexcept { return position_; }

size_t Reader::size() const noexcept { return size_; }

const uint8_t* Reader::current_data() const noexcept { return position_ < size_ ? data_ + position_ : nullptr; }

bool Reader::read(uint8_t* data, size_t size, size_t end) noexcept {
  if VUNLIKELY (end > size_ || position_ > end || size > end - position_) {
    return false;
  }

  if (size > 0U) {
    std::memcpy(data, data_ + position_, size);
  }

  position_ += size;

  return true;
}

bool Reader::skip(size_t size, size_t end) noexcept {
  if VUNLIKELY (end > size_ || position_ > end || size > end - position_) {
    return false;
  }

  position_ += size;

  return true;
}

bool Reader::read_unsigned(uint64_t& value, size_t width, size_t end) noexcept {
  if VUNLIKELY (width == 0U || width > sizeof(value) || end > size_ || position_ > end || width > end - position_) {
    return false;
  }

  value = 0;

  for (size_t index = 0; index < width; ++index) {
    const size_t shift = (endian_ == Endian::kBig ? width - index - 1U : index) * 8U;
    value |= static_cast<uint64_t>(data_[position_ + index]) << shift;
  }

  position_ += width;

  return true;
}

bool Reader::begin_length_delimited(size_t end, size_t& value_end, size_t width) noexcept {
  if VUNLIKELY ((width != 1U && width != 2U && width != 4U) || end > size_ || position_ > end ||
                width > end - position_) {
    return false;
  }

  uint32_t byte_length = 0U;

  for (size_t index = 0U; index < width; ++index) {
    byte_length = (byte_length << 8U) | data_[position_ + index];
  }

  position_ += width;

  if VUNLIKELY (byte_length > end - position_) {
    return false;
  }

  value_end = position_ + static_cast<size_t>(byte_length);

  return true;
}

bool Reader::align(size_t end) noexcept {
  if VLIKELY (alignment_ == 1U) {
    return true;
  }

  const size_t mask = alignment_ - 1U;
  const size_t remainder = ((position_ & mask) + (kHeaderSize & mask)) & mask;
  const size_t padding = (alignment_ - remainder) & mask;

  return skip(padding, end);
}

bool write_value(Writer& writer, bool value) noexcept {
  return writer.append_unsigned(value ? 1U : 0U, sizeof(uint8_t));
}

bool write_value(Writer& writer, uint8_t value) noexcept { return writer.append_unsigned(value, sizeof(value)); }

bool write_value(Writer& writer, uint16_t value) noexcept { return writer.append_unsigned(value, sizeof(value)); }

bool write_value(Writer& writer, uint32_t value) noexcept { return writer.append_unsigned(value, sizeof(value)); }

bool write_value(Writer& writer, uint64_t value) noexcept { return writer.append_unsigned(value, sizeof(value)); }

bool write_value(Writer& writer, int8_t value) noexcept {
  return writer.append_unsigned(static_cast<uint8_t>(value), sizeof(value));
}

bool write_value(Writer& writer, int16_t value) noexcept {
  return writer.append_unsigned(static_cast<uint16_t>(value), sizeof(value));
}

bool write_value(Writer& writer, int32_t value) noexcept {
  return writer.append_unsigned(static_cast<uint32_t>(value), sizeof(value));
}

bool write_value(Writer& writer, int64_t value) noexcept {
  return writer.append_unsigned(static_cast<uint64_t>(value), sizeof(value));
}

bool write_value(Writer& writer, float value) noexcept {
  static_assert(sizeof(float) == sizeof(uint32_t), "SOME/IP float must use IEEE 754 binary32 storage.");
  static_assert(std::numeric_limits<float>::is_iec559, "SOME/IP float requires IEEE 754 semantics.");

  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));

  return writer.append_unsigned(bits, sizeof(bits));
}

bool write_value(Writer& writer, double value) noexcept {
  static_assert(sizeof(double) == sizeof(uint64_t), "SOME/IP double must use IEEE 754 binary64 storage.");
  static_assert(std::numeric_limits<double>::is_iec559, "SOME/IP double requires IEEE 754 semantics.");

  uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));

  return writer.append_unsigned(bits, sizeof(bits));
}

bool write_value(Writer& writer, const std::string& value, size_t len) noexcept {
  const auto* data = reinterpret_cast<const uint8_t*>(value.data());
  if VUNLIKELY (!is_valid_utf8(data, value.size())) {
    return false;
  }

  size_t length_position = 0;
  size_t data_position = 0;

  if VUNLIKELY (!writer.begin_length_delimited(length_position, data_position, len)) {
    return false;
  }

  const uint8_t terminator = 0;

  return writer.append(kUtf8Bom, sizeof(kUtf8Bom)) && writer.append(data, value.size()) &&
         writer.append(&terminator, sizeof(terminator)) &&
         writer.end_length_delimited(length_position, data_position, len);
}

bool write_value(Writer& writer, const Bytes& value, size_t len) noexcept {
  size_t length_position = 0;
  size_t data_position = 0;

  if VUNLIKELY (!writer.begin_length_delimited(length_position, data_position, len)) {
    return false;
  }

  return writer.append(value.data(), value.size()) && writer.end_length_delimited(length_position, data_position, len);
}

bool read_value(Reader& reader, bool& value, size_t end) noexcept {
  uint64_t raw = 0;

  if VUNLIKELY (!reader.read_unsigned(raw, sizeof(uint8_t), end)) {
    return false;
  }

  value = (raw & 0x01U) != 0U;

  return true;
}

bool read_value(Reader& reader, uint8_t& value, size_t end) noexcept {
  uint64_t raw = 0;

  if VUNLIKELY (!reader.read_unsigned(raw, sizeof(value), end)) {
    return false;
  }

  value = static_cast<uint8_t>(raw);

  return true;
}

bool read_value(Reader& reader, uint16_t& value, size_t end) noexcept {
  uint64_t raw = 0;

  if VUNLIKELY (!reader.read_unsigned(raw, sizeof(value), end)) {
    return false;
  }

  value = static_cast<uint16_t>(raw);

  return true;
}

bool read_value(Reader& reader, uint32_t& value, size_t end) noexcept {
  uint64_t raw = 0;

  if VUNLIKELY (!reader.read_unsigned(raw, sizeof(value), end)) {
    return false;
  }

  value = static_cast<uint32_t>(raw);

  return true;
}

bool read_value(Reader& reader, uint64_t& value, size_t end) noexcept {
  return reader.read_unsigned(value, sizeof(value), end);
}

bool read_value(Reader& reader, int8_t& value, size_t end) noexcept {
  uint8_t raw = 0;

  if VUNLIKELY (!read_value(reader, raw, end)) {
    return false;
  }

  std::memcpy(&value, &raw, sizeof(value));

  return true;
}

bool read_value(Reader& reader, int16_t& value, size_t end) noexcept {
  uint16_t raw = 0;

  if VUNLIKELY (!read_value(reader, raw, end)) {
    return false;
  }

  std::memcpy(&value, &raw, sizeof(value));

  return true;
}

bool read_value(Reader& reader, int32_t& value, size_t end) noexcept {
  uint32_t raw = 0;

  if VUNLIKELY (!read_value(reader, raw, end)) {
    return false;
  }

  std::memcpy(&value, &raw, sizeof(value));

  return true;
}

bool read_value(Reader& reader, int64_t& value, size_t end) noexcept {
  uint64_t raw = 0;

  if VUNLIKELY (!read_value(reader, raw, end)) {
    return false;
  }

  std::memcpy(&value, &raw, sizeof(value));

  return true;
}

bool read_value(Reader& reader, float& value, size_t end) noexcept {
  uint32_t bits = 0;

  if VUNLIKELY (!read_value(reader, bits, end)) {
    return false;
  }

  std::memcpy(&value, &bits, sizeof(value));

  return true;
}

bool read_value(Reader& reader, double& value, size_t end) noexcept {
  uint64_t bits = 0;

  if VUNLIKELY (!read_value(reader, bits, end)) {
    return false;
  }

  std::memcpy(&value, &bits, sizeof(value));

  return true;
}

bool read_value(Reader& reader, std::string& value, size_t end, size_t len) {
  size_t value_end = 0;

  if VUNLIKELY (!reader.begin_length_delimited(end, value_end, len)) {
    return false;
  }

  const size_t length = value_end - reader.position();
  if VUNLIKELY (length < sizeof(kUtf8Bom) + 1U) {
    return false;
  }

  const auto* data = reader.current_data();
  const size_t value_size = length - sizeof(kUtf8Bom) - 1U;

  if VUNLIKELY (!data || std::memcmp(data, kUtf8Bom, sizeof(kUtf8Bom)) != 0 || data[length - 1U] != 0U ||
                !is_valid_utf8(data + sizeof(kUtf8Bom), value_size)) {
    return false;
  }

  value.assign(reinterpret_cast<const char*>(data + sizeof(kUtf8Bom)), value_size);

  return reader.skip(length, value_end);
}

bool read_value(Reader& reader, Bytes& value, size_t end, size_t len) noexcept {
  size_t value_end = 0;

  if VUNLIKELY (!reader.begin_length_delimited(end, value_end, len)) {
    return false;
  }

  const size_t length = value_end - reader.position();

  if (length == 0U) {
    value.clear();

    return true;
  }

  if (value.is_owner() && value.offset() == 0U && length <= value.capacity()) {
    if VUNLIKELY (!value.resize(length)) {
      return false;
    }

    return reader.read(value.data(), length, value_end);
  }

  Bytes target = Bytes::create(length);

  if VUNLIKELY (length > 0U && target.empty()) {
    return false;
  }

  if VUNLIKELY (!reader.read(target.data(), length, value_end)) {
    return false;
  }

  value = std::move(target);

  return true;
}

}  // namespace SomeipSerializer

}  // namespace vlink
