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

#include "./extension/someip_serializer.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>
#include <utility>

namespace vlink {

namespace SomeipSerializer {  // NOLINT(readability-identifier-naming)

static constexpr uint8_t kUtf8Bom[] = {0xEF, 0xBB, 0xBF};
static constexpr uint8_t kUtf16BomBig[] = {0xFE, 0xFF};
static constexpr uint8_t kUtf16BomLittle[] = {0xFF, 0xFE};
static constexpr size_t kUtf16UnitSize = 2U;
static constexpr size_t kHeaderSize = 16U;

// Writer
Writer::Writer(uint8_t* data, size_t capacity, size_t alignment, Endian endian) noexcept
    : data_(data), capacity_(std::min(capacity, kMaxPayloadSize)), alignment_(alignment), endian_(endian) {}

size_t Writer::position() const noexcept { return position_; }

bool Writer::is_size_only() const noexcept { return data_ == nullptr; }

Writer Writer::size_only() const noexcept {
  Writer result = *this;
  result.data_ = nullptr;

  return result;
}

bool Writer::append(const uint8_t* data, size_t size) noexcept {
  if VUNLIKELY (size > capacity_ - position_) {
    return false;
  }

  if (data_ && size > 0U) {
    std::memcpy(data_ + position_, data, size);
  }

  position_ += size;

  return true;
}

bool Writer::append_unsigned(uint64_t value, size_t width) noexcept { return append_unsigned(value, width, endian_); }

bool Writer::append_unsigned(uint64_t value, size_t width, Endian endian) noexcept {
  if VUNLIKELY (width == 0U || width > sizeof(value) || width > capacity_ - position_) {
    return false;
  }

  for (size_t index = 0; index < width; ++index) {
    const size_t shift = (endian == Endian::kBig ? width - index - 1U : index) * 8U;

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

  if VUNLIKELY (width > capacity_ - position_) {
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

  if VUNLIKELY (padding > capacity_ - position_) {
    return false;
  }

  if (data_ && padding > 0U) {
    std::memset(data_ + position_, 0, padding);
  }

  position_ += padding;

  return true;
}

// Reader
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
  return read_unsigned(value, width, endian_, end);
}

bool Reader::read_unsigned(uint64_t& value, size_t width, Endian endian, size_t end) noexcept {
  if VUNLIKELY (width == 0U || width > sizeof(value) || end > size_ || position_ > end || width > end - position_) {
    return false;
  }

  value = 0;

  for (size_t index = 0; index < width; ++index) {
    const size_t shift = (endian == Endian::kBig ? width - index - 1U : index) * 8U;
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

// detail
namespace detail {

static bool is_valid_utf8(const uint8_t* data, size_t size, size_t maximum) noexcept {
  size_t position = 0;
  size_t count = 0;

  while (position < size) {
    const uint8_t first = data[position];

    if VLIKELY (first <= 0x7FU) {
      ++position;

      if (maximum != std::numeric_limits<size_t>::max()) {
        ++count;
        if VUNLIKELY (count > maximum) {
          return false;
        }
      }

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

    if (maximum != std::numeric_limits<size_t>::max()) {
      ++count;
      if VUNLIKELY (count > maximum) {
        return false;
      }
    }
  }

  return true;
}

static bool is_valid_utf16(const char16_t* data, size_t size, size_t maximum) noexcept {
  size_t position = 0;
  size_t count = 0;

  while (position < size) {
    const auto unit = static_cast<uint16_t>(data[position]);

    if VLIKELY (unit < 0xD800U || unit > 0xDFFFU) {
      ++position;

      if (maximum != std::numeric_limits<size_t>::max()) {
        ++count;
        if VUNLIKELY (count > maximum) {
          return false;
        }
      }

      continue;
    }

    if VUNLIKELY (unit > 0xDBFFU || position + 1U == size) {
      return false;
    }

    const auto low = static_cast<uint16_t>(data[position + 1U]);

    if VUNLIKELY (low < 0xDC00U || low > 0xDFFFU) {
      return false;
    }

    position += 2U;

    if (maximum != std::numeric_limits<size_t>::max()) {
      ++count;
      if VUNLIKELY (count > maximum) {
        return false;
      }
    }
  }

  return true;
}

static bool write_zeroes(Writer& writer, size_t size) noexcept {
  if (writer.is_size_only()) {
    return writer.append(nullptr, size);
  }

  static constexpr uint8_t kZeroes[256] = {};

  while (size > sizeof(kZeroes)) {
    if VUNLIKELY (!writer.append(kZeroes, sizeof(kZeroes))) {
      return false;
    }

    size -= sizeof(kZeroes);
  }

  return writer.append(kZeroes, size);
}

static bool write_utf8_body(Writer& writer, const std::string& value, size_t maximum, bool strip_leading_bom) noexcept {
  const auto* data = reinterpret_cast<const uint8_t*>(value.data());
  const size_t offset =
      strip_leading_bom && value.size() >= sizeof(kUtf8Bom) && std::memcmp(data, kUtf8Bom, sizeof(kUtf8Bom)) == 0
          ? sizeof(kUtf8Bom)
          : 0U;

  if VUNLIKELY (!is_valid_utf8(data + offset, value.size() - offset, maximum)) {
    return false;
  }

  const uint8_t terminator = 0U;

  return writer.append(kUtf8Bom, sizeof(kUtf8Bom)) && writer.append(data + offset, value.size() - offset) &&
         writer.append(&terminator, sizeof(terminator));
}

static bool write_utf16_body(Writer& writer, const std::u16string& value, Endian encoding, size_t maximum,
                             bool strip_leading_bom) noexcept {
  const size_t offset = strip_leading_bom && !value.empty() && value.front() == u'\uFEFF' ? 1U : 0U;

  if VUNLIKELY (!is_valid_utf16(value.data() + offset, value.size() - offset, maximum)) {
    return false;
  }

  if (writer.is_size_only()) {
    return writer.append(nullptr, kUtf16UnitSize) && writer.append(nullptr, (value.size() - offset) * kUtf16UnitSize) &&
           writer.append(nullptr, kUtf16UnitSize);
  }

  const uint8_t* bom = encoding == Endian::kBig ? kUtf16BomBig : kUtf16BomLittle;

  if VUNLIKELY (!writer.append(bom, kUtf16UnitSize)) {
    return false;
  }

  uint8_t chunk[256];
  size_t filled = 0U;

  for (size_t index = offset; index < value.size(); ++index) {
    const auto bits = static_cast<uint16_t>(value[index]);

    chunk[filled] = static_cast<uint8_t>(encoding == Endian::kBig ? bits >> 8U : bits & 0xFFU);
    chunk[filled + 1U] = static_cast<uint8_t>(encoding == Endian::kBig ? bits & 0xFFU : bits >> 8U);
    filled += kUtf16UnitSize;

    if (filled == sizeof(chunk)) {
      if VUNLIKELY (!writer.append(chunk, filled)) {
        return false;
      }

      filled = 0U;
    }
  }

  chunk[filled] = 0U;
  chunk[filled + 1U] = 0U;

  return writer.append(chunk, filled + kUtf16UnitSize);
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

bool write_value(Writer& writer, const std::string& value, size_t len, size_t maximum) noexcept {
  size_t length_position = 0;
  size_t data_position = 0;

  return writer.begin_length_delimited(length_position, data_position, len) &&
         write_value_body(writer, value, maximum) && writer.end_length_delimited(length_position, data_position, len);
}

bool write_value(Writer& writer, const Bytes& value, size_t len, size_t maximum) noexcept {
  size_t length_position = 0;
  size_t data_position = 0;

  return writer.begin_length_delimited(length_position, data_position, len) &&
         write_value_body(writer, value, maximum) && writer.end_length_delimited(length_position, data_position, len);
}

bool write_value(Writer& writer, const std::u16string& value, size_t len, Endian encoding, size_t maximum) noexcept {
  size_t length_position = 0;
  size_t data_position = 0;

  return writer.begin_length_delimited(length_position, data_position, len) &&
         write_value_body(writer, value, encoding, maximum) &&
         writer.end_length_delimited(length_position, data_position, len);
}

bool write_fixed_value(Writer& writer, const std::string& value, size_t size, size_t len, size_t maximum) noexcept {
  if VUNLIKELY (size < sizeof(kUtf8Bom) + 1U || value.size() > size - sizeof(kUtf8Bom) - 1U) {
    return false;
  }

  size_t length_position = 0U;
  size_t data_position = 0U;

  if VUNLIKELY (len > 0U && !writer.begin_length_delimited(length_position, data_position, len)) {
    return false;
  }

  if VUNLIKELY (!write_utf8_body(writer, value, maximum, false) ||
                !write_zeroes(writer, size - sizeof(kUtf8Bom) - value.size() - 1U)) {
    return false;
  }

  return len == 0U || writer.end_length_delimited(length_position, data_position, len);
}

bool write_fixed_value(Writer& writer, const std::u16string& value, size_t size, size_t len, Endian encoding,
                       size_t maximum) noexcept {
  if VUNLIKELY (size < kUtf16UnitSize * 2U || size % kUtf16UnitSize != 0U ||
                value.size() > size / kUtf16UnitSize - 2U) {
    return false;
  }

  size_t length_position = 0U;
  size_t data_position = 0U;

  if VUNLIKELY (len > 0U && !writer.begin_length_delimited(length_position, data_position, len)) {
    return false;
  }

  const size_t encoded_size = (value.size() + 2U) * kUtf16UnitSize;

  if VUNLIKELY (!write_utf16_body(writer, value, encoding, maximum, false) ||
                !write_zeroes(writer, size - encoded_size)) {
    return false;
  }

  return len == 0U || writer.end_length_delimited(length_position, data_position, len);
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

bool read_value(Reader& reader, std::string& value, size_t end, size_t len, size_t maximum) noexcept {
  size_t value_end = 0;

  return reader.begin_length_delimited(end, value_end, len) && read_value_body(reader, value, value_end, maximum);
}

bool read_value(Reader& reader, Bytes& value, size_t end, size_t len, size_t maximum) noexcept {
  size_t value_end = 0;

  return reader.begin_length_delimited(end, value_end, len) && read_value_body(reader, value, value_end, maximum);
}

bool read_value(Reader& reader, std::u16string& value, size_t end, size_t len, Endian encoding,
                size_t maximum) noexcept {
  size_t value_end = 0;

  return reader.begin_length_delimited(end, value_end, len) &&
         read_value_body(reader, value, value_end, encoding, maximum);
}

bool read_fixed_value(Reader& reader, std::string& value, size_t end, size_t size, size_t len,
                      size_t maximum) noexcept {
  size_t value_end = 0U;

  if (len == 0U) {
    if VUNLIKELY (reader.position() > end || size > end - reader.position()) {
      return false;
    }

    value_end = reader.position() + size;
  } else if VUNLIKELY (!reader.begin_length_delimited(end, value_end, len) || value_end - reader.position() > size) {
    return false;
  }

  if VUNLIKELY (!read_value_body(reader, value, value_end)) {
    return false;
  }

  while (!value.empty() && value.back() == '\0') {
    value.pop_back();
  }

  return maximum == std::numeric_limits<size_t>::max() ||
         is_valid_utf8(reinterpret_cast<const uint8_t*>(value.data()), value.size(), maximum);
}

bool read_fixed_value(Reader& reader, std::u16string& value, size_t end, size_t size, size_t len, Endian encoding,
                      size_t maximum) noexcept {
  size_t value_end = 0U;

  if (len == 0U) {
    if VUNLIKELY (reader.position() > end || size > end - reader.position()) {
      return false;
    }

    value_end = reader.position() + size;
  } else if VUNLIKELY (!reader.begin_length_delimited(end, value_end, len) || value_end - reader.position() > size) {
    return false;
  }

  if VUNLIKELY (!read_value_body(reader, value, value_end, encoding)) {
    return false;
  }

  while (!value.empty() && value.back() == u'\0') {
    value.pop_back();
  }

  return maximum == std::numeric_limits<size_t>::max() || is_valid_utf16(value.data(), value.size(), maximum);
}

bool read_value_body(Reader& reader, std::string& value, size_t end, size_t maximum) noexcept {
  const size_t length = end - reader.position();

  if VUNLIKELY (length < sizeof(kUtf8Bom) + 1U) {
    return false;
  }

  const auto* data = reader.current_data();
  const size_t value_size = length - sizeof(kUtf8Bom) - 1U;

  if VUNLIKELY (!data || std::memcmp(data, kUtf8Bom, sizeof(kUtf8Bom)) != 0 || data[length - 1U] != 0U ||
                !is_valid_utf8(data + sizeof(kUtf8Bom), value_size, maximum)) {
    return false;
  }

  value.assign(reinterpret_cast<const char*>(data + sizeof(kUtf8Bom)), value_size);

  return reader.skip(length, end);
}

bool read_value_body(Reader& reader, std::u16string& value, size_t end, Endian encoding, size_t maximum) noexcept {
  const size_t length = end - reader.position();
  const size_t encoded_length = length & ~size_t{1U};

  if VUNLIKELY (encoded_length < kUtf16UnitSize * 2U) {
    return false;
  }

  const auto* data = reader.current_data();
  const uint8_t* bom = encoding == Endian::kBig ? kUtf16BomBig : kUtf16BomLittle;

  if VUNLIKELY (!data || std::memcmp(data, bom, kUtf16UnitSize) != 0 || data[encoded_length - 2U] != 0U ||
                data[encoded_length - 1U] != 0U) {
    return false;
  }

  const uint8_t* units = data + kUtf16UnitSize;
  const size_t unit_count = encoded_length / kUtf16UnitSize - 2U;
  size_t code_point_count = 0U;

  for (size_t index = 0U; index < unit_count; ++index) {
    const uint8_t first = units[index * kUtf16UnitSize];
    const uint8_t second = units[index * kUtf16UnitSize + 1U];
    const auto high = static_cast<uint16_t>(encoding == Endian::kBig ? first : second);
    const auto low = static_cast<uint16_t>(encoding == Endian::kBig ? second : first);
    const auto unit = static_cast<uint16_t>(static_cast<uint16_t>(high << 8U) | low);

    if (unit >= 0xD800U && unit <= 0xDFFFU) {
      if VUNLIKELY (unit > 0xDBFFU || index + 1U == unit_count) {
        return false;
      }

      const uint8_t next_first = units[(index + 1U) * kUtf16UnitSize];
      const uint8_t next_second = units[(index + 1U) * kUtf16UnitSize + 1U];
      const auto next_high = static_cast<uint16_t>(encoding == Endian::kBig ? next_first : next_second);
      const auto next_low = static_cast<uint16_t>(encoding == Endian::kBig ? next_second : next_first);
      const auto next = static_cast<uint16_t>(static_cast<uint16_t>(next_high << 8U) | next_low);

      if VUNLIKELY (next < 0xDC00U || next > 0xDFFFU) {
        return false;
      }

      ++index;
    }

    if (maximum != std::numeric_limits<size_t>::max()) {
      ++code_point_count;
      if VUNLIKELY (code_point_count > maximum) {
        return false;
      }
    }
  }

  value.resize(unit_count);

  for (size_t index = 0U; index < unit_count; ++index) {
    const uint8_t first = units[index * kUtf16UnitSize];
    const uint8_t second = units[index * kUtf16UnitSize + 1U];
    const auto high = static_cast<uint16_t>(encoding == Endian::kBig ? first : second);
    const auto low = static_cast<uint16_t>(encoding == Endian::kBig ? second : first);

    value[index] = static_cast<char16_t>(static_cast<uint16_t>(high << 8U) | low);
  }

  return reader.skip(length, end);
}

bool read_value_body(Reader& reader, Bytes& value, size_t end, size_t maximum) noexcept {
  const size_t length = end - reader.position();
  const size_t kept = length < maximum ? length : maximum;

  if (kept == 0U) {
    value.clear();

    return reader.skip(length, end);
  }

  if (value.is_owner() && value.offset() == 0U && kept <= value.capacity()) {
    if VUNLIKELY (!value.resize(kept) || !reader.read(value.data(), kept, end)) {
      return false;
    }

    return reader.skip(length - kept, end);
  }

  Bytes target = Bytes::create(kept);

  if VUNLIKELY (target.empty()) {
    return false;
  }

  if VUNLIKELY (!reader.read(target.data(), kept, end)) {
    return false;
  }

  value = std::move(target);

  return reader.skip(length - kept, end);
}

bool read_tag(Reader& reader, uint8_t& wire_type, uint16_t& data_id, size_t end) noexcept {
  uint64_t raw = 0;

  if VUNLIKELY (!reader.read_unsigned(raw, sizeof(uint16_t), Endian::kBig, end) || (raw & 0x8000U) != 0U) {
    return false;
  }

  wire_type = static_cast<uint8_t>((raw >> 12U) & 0x07U);
  data_id = static_cast<uint16_t>(raw & 0x0FFFU);

  return true;
}

bool read_union_frame(Reader& reader, size_t len, size_t type_width, size_t end, uint64_t& selector,
                      size_t& data_end) noexcept {
  if VUNLIKELY ((len != 1U && len != 2U && len != 4U) || (type_width != 1U && type_width != 2U && type_width != 4U)) {
    return false;
  }

  uint64_t byte_length = 0;

  if VUNLIKELY (!reader.read_unsigned(byte_length, len, Endian::kBig, end) ||
                !reader.read_unsigned(selector, type_width, Endian::kBig, end) ||
                byte_length > end - reader.position()) {
    return false;
  }

  data_end = reader.position() + static_cast<size_t>(byte_length);

  return true;
}

bool write_value_body(Writer& writer, const std::string& value, size_t maximum) noexcept {
  return write_utf8_body(writer, value, maximum, true);
}

bool write_value_body(Writer& writer, const std::u16string& value, Endian encoding, size_t maximum) noexcept {
  return write_utf16_body(writer, value, encoding, maximum, true);
}

bool write_value_body(Writer& writer, const Bytes& value, size_t maximum) noexcept {
  return value.size() <= maximum && writer.append(value.data(), value.size());
}

bool write_tag(Writer& writer, uint8_t wire_type, uint16_t data_id) noexcept {
  const auto tag = static_cast<uint16_t>((static_cast<uint16_t>(wire_type) << 12U) | data_id);

  return writer.append_unsigned(tag, sizeof(uint16_t), Endian::kBig);
}

bool write_union_frame(Writer& writer, uint64_t selector, size_t len, size_t type_width, size_t& length_position,
                       size_t& content_position) noexcept {
  if VUNLIKELY (type_width != 1U && type_width != 2U && type_width != 4U) {
    return false;
  }

  if VUNLIKELY (selector > (uint64_t{1U} << (type_width * 8U)) - 1U) {
    return false;
  }

  size_t data_position = 0;

  if VUNLIKELY (!writer.begin_length_delimited(length_position, data_position, len) ||
                !writer.append_unsigned(selector, type_width, Endian::kBig)) {
    return false;
  }

  content_position = writer.position();

  return true;
}

bool begin_tlv_body(Reader& reader, uint8_t wire_type, size_t configured_len, size_t end, size_t& body_end) noexcept {
  size_t width = 0;

  if (wire_type == 4U) {
    width = configured_len;
  } else if (wire_type >= 5U && wire_type <= 7U) {
    width = size_t{1U} << (wire_type - 5U);
  } else {
    return false;
  }

  return reader.begin_length_delimited(end, body_end, width);
}

bool begin_tlv_complex(Writer& writer, uint16_t data_id, size_t len, bool dynamic, size_t& length_position,
                       size_t& data_position) noexcept {
  uint8_t wire_type = 7U;

  if (!dynamic) {
    wire_type = 4U;
  } else if (len == 1U) {
    wire_type = 5U;
  } else if (len == 2U) {
    wire_type = 6U;
  }

  return write_tag(writer, wire_type, data_id) && writer.begin_length_delimited(length_position, data_position, len);
}

bool skip_tlv_value(Reader& reader, uint8_t wire_type, size_t configured_len, size_t end) noexcept {
  switch (wire_type) {
    case 0U:
      return reader.skip(sizeof(uint8_t), end);
    case 1U:
      return reader.skip(sizeof(uint16_t), end);
    case 2U:
      return reader.skip(sizeof(uint32_t), end);
    case 3U:
      return reader.skip(sizeof(uint64_t), end);
    case 4U: {
      size_t value_end = 0U;

      return reader.begin_length_delimited(end, value_end, configured_len) &&
             reader.skip(value_end - reader.position(), value_end);
    }
    case 5U:
    case 6U:
    case 7U: {
      const size_t width = size_t{1U} << (wire_type - 5U);

      size_t value_end = 0;

      return reader.begin_length_delimited(end, value_end, width) &&
             reader.skip(value_end - reader.position(), value_end);
    }
    default:
      return false;
  }
}

void report_check_available_failure() noexcept {
  std::cerr << "SOME/IP serialization rejected by check_available().\n";
}

bool prepare_destination(Bytes& des, size_t size, uint8_t offset) noexcept {
  if (des.is_owner() && des.offset() == offset && size <= des.capacity()) {
    if VUNLIKELY (!des.resize(size)) {
      return false;
    }
  } else {
    des = Bytes::create(size, offset);
  }

  return size == 0U || des.data() != nullptr;
}

}  // namespace detail

}  // namespace SomeipSerializer

}  // namespace vlink
