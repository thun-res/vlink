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

#include "./base/bytes.h"

#include <lzav/lzav.h>

#include <array>
#include <charconv>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "./base/logger.h"
#include "./base/memory_pool.h"

#define VLINK_BYTES_MEM_RESET 0

namespace vlink {

static constexpr uint32_t kCrc32Polynomial = 0xEDB88320U;

static constexpr uint64_t kCrc64Polynomial = 0x42F0E1EBA9EA3693ULL;

static constexpr const char kBase64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

static constexpr const char* kHexTable[256] = {
    "00 ", "01 ", "02 ", "03 ", "04 ", "05 ", "06 ", "07 ", "08 ", "09 ", "0A ", "0B ", "0C ", "0D ", "0E ", "0F ",
    "10 ", "11 ", "12 ", "13 ", "14 ", "15 ", "16 ", "17 ", "18 ", "19 ", "1A ", "1B ", "1C ", "1D ", "1E ", "1F ",
    "20 ", "21 ", "22 ", "23 ", "24 ", "25 ", "26 ", "27 ", "28 ", "29 ", "2A ", "2B ", "2C ", "2D ", "2E ", "2F ",
    "30 ", "31 ", "32 ", "33 ", "34 ", "35 ", "36 ", "37 ", "38 ", "39 ", "3A ", "3B ", "3C ", "3D ", "3E ", "3F ",
    "40 ", "41 ", "42 ", "43 ", "44 ", "45 ", "46 ", "47 ", "48 ", "49 ", "4A ", "4B ", "4C ", "4D ", "4E ", "4F ",
    "50 ", "51 ", "52 ", "53 ", "54 ", "55 ", "56 ", "57 ", "58 ", "59 ", "5A ", "5B ", "5C ", "5D ", "5E ", "5F ",
    "60 ", "61 ", "62 ", "63 ", "64 ", "65 ", "66 ", "67 ", "68 ", "69 ", "6A ", "6B ", "6C ", "6D ", "6E ", "6F ",
    "70 ", "71 ", "72 ", "73 ", "74 ", "75 ", "76 ", "77 ", "78 ", "79 ", "7A ", "7B ", "7C ", "7D ", "7E ", "7F ",
    "80 ", "81 ", "82 ", "83 ", "84 ", "85 ", "86 ", "87 ", "88 ", "89 ", "8A ", "8B ", "8C ", "8D ", "8E ", "8F ",
    "90 ", "91 ", "92 ", "93 ", "94 ", "95 ", "96 ", "97 ", "98 ", "99 ", "9A ", "9B ", "9C ", "9D ", "9E ", "9F ",
    "A0 ", "A1 ", "A2 ", "A3 ", "A4 ", "A5 ", "A6 ", "A7 ", "A8 ", "A9 ", "AA ", "AB ", "AC ", "AD ", "AE ", "AF ",
    "B0 ", "B1 ", "B2 ", "B3 ", "B4 ", "B5 ", "B6 ", "B7 ", "B8 ", "B9 ", "BA ", "BB ", "BC ", "BD ", "BE ", "BF ",
    "C0 ", "C1 ", "C2 ", "C3 ", "C4 ", "C5 ", "C6 ", "C7 ", "C8 ", "C9 ", "CA ", "CB ", "CC ", "CD ", "CE ", "CF ",
    "D0 ", "D1 ", "D2 ", "D3 ", "D4 ", "D5 ", "D6 ", "D7 ", "D8 ", "D9 ", "DA ", "DB ", "DC ", "DD ", "DE ", "DF ",
    "E0 ", "E1 ", "E2 ", "E3 ", "E4 ", "E5 ", "E6 ", "E7 ", "E8 ", "E9 ", "EA ", "EB ", "EC ", "ED ", "EE ", "EF ",
    "F0 ", "F1 ", "F2 ", "F3 ", "F4 ", "F5 ", "F6 ", "F7 ", "F8 ", "F9 ", "FA ", "FB ", "FC ", "FD ", "FE ", "FF "};

static constexpr uint8_t kCompressHeaderMagic[4] = {
    0x17,
    0x49,
    0xB2,
    0x6F,
};

static constexpr uint8_t kCompressFooterMagic[4] = {
    0xA7,
    0x05,
    0xED,
    0x71,
};

static constexpr size_t kMaxCompressCacheSize = 1024UL * 1024UL;

static constexpr auto check_magic(const uint8_t* data, const uint8_t* magic, size_t len) noexcept {
  for (size_t i = 0; i < len; ++i) {
    if (data[i] != magic[i]) {
      return false;
    }
  }

  return true;
}

static auto& bytes_compress_cache() noexcept {
  thread_local std::vector<uint8_t> compress_cache(kMaxCompressCacheSize);

  return compress_cache;
}

// Bytes
uint8_t* Bytes::bytes_malloc(size_t size) noexcept {
  return static_cast<uint8_t*>(MemoryPool::global_instance().allocate(size));
}

void Bytes::bytes_free(uint8_t* ptr, size_t size) noexcept { MemoryPool::global_instance().deallocate(ptr, size); }

void Bytes::init_memory_pool() noexcept { (void)MemoryPool::global_instance(); }

void Bytes::release_memory_pool() noexcept { MemoryPool::global_instance().clear(); }

Bytes Bytes::create(size_t size, uint8_t offset) noexcept {
#if defined(__arm__) || defined(__x86__) || defined(__i386__)
#ifndef __ANDROID__
#warning "Bytes No support for 32-bit architecture."
#endif
#else
  static_assert(sizeof(Bytes) == 128, "Sizeof must be 128 bytes.");
#endif

  return Bytes(kCreate, nullptr, size, offset, false);
}

Bytes Bytes::shallow_copy(uint8_t* data, size_t size) noexcept {
  return Bytes(kShallowCopy, size == 0 ? nullptr : data, size, 0, false);
}

Bytes Bytes::shallow_copy(const uint8_t* data, size_t size) noexcept {
  return Bytes(kShallowCopy, size == 0 ? nullptr : const_cast<uint8_t*>(data), size, 0, false);
}

Bytes Bytes::shallow_copy_ptr(void* data) noexcept {
  return Bytes(kShallowCopy, static_cast<uint8_t*>(data), 0, 0, false);
}

Bytes Bytes::deep_copy(uint8_t* data, size_t size, uint8_t offset) noexcept {
  return Bytes(kDeepCopy, size == 0 ? nullptr : data, size, offset, false);
}

Bytes Bytes::deep_copy(const uint8_t* data, size_t size, uint8_t offset) noexcept {
  return Bytes(kDeepCopy, size == 0 ? nullptr : const_cast<uint8_t*>(data), size, offset, false);
}

Bytes Bytes::loan_internal(uint8_t* data, size_t size) noexcept {
  return Bytes(kShallowCopy, size == 0 ? nullptr : data, size, 0, true);
}

Bytes Bytes::loan_internal(const uint8_t* data, size_t size) noexcept {
  return Bytes(kShallowCopy, size == 0 ? nullptr : const_cast<uint8_t*>(data), size, 0, true);
}

Bytes Bytes::from_string(const std::string& str, uint8_t offset) noexcept {
  if (str.empty()) {
    return Bytes(kDeepCopy, nullptr, 0, offset, false);
  }

  return Bytes(kDeepCopy, reinterpret_cast<uint8_t*>(const_cast<char*>(str.data())), str.size(), offset, false);
}

Bytes Bytes::from_user_input(const std::string& str, bool* ok) noexcept {
  if (str.empty()) {
    if (ok) {
      *ok = false;
    }

    return Bytes();
  }

  std::vector<uint8_t> vec;
  vec.reserve((str.size() + 1) / 2);

  thread_local std::istringstream ss;
  ss.clear();
  ss.str(str);

  std::string byte_str;

  auto parse_byte = [&vec, ok](std::string_view token) -> bool {
    if (token.empty()) {
      if (ok) {
        *ok = false;
      }
      return false;
    }

    uint32_t parsed_value = 0;
    auto [p, error] = std::from_chars(token.data(), token.data() + token.size(), parsed_value, 16);

    if VUNLIKELY (error != std::errc() || p != token.data() + token.size() || parsed_value > 0xFF) {
      if (ok) {
        *ok = false;
      }
      return false;
    }

    vec.emplace_back(static_cast<uint8_t>(parsed_value));
    return true;
  };

  while (ss >> std::skipws >> byte_str) {
    std::string_view token(byte_str);

    if (token.size() >= 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X')) {
      token.remove_prefix(2);
    }

    if VUNLIKELY (token.empty() || (token.size() > 2 && (token.size() % 2) != 0)) {
      if (ok) {
        *ok = false;
      }

      return Bytes();
    }

    if (token.size() <= 2) {
      if VUNLIKELY (!parse_byte(token)) {
        return Bytes();
      }
      continue;
    }

    for (size_t index = 0; index < token.size(); index += 2) {
      if VUNLIKELY (!parse_byte(token.substr(index, 2))) {
        return Bytes();
      }
    }
  }

  if (ok) {
    *ok = true;
  }

  return Bytes(vec);
}

std::string Bytes::convert_to_hex_str(const uint8_t* value, size_t size) noexcept {
  std::string str;

  if (!value || size == 0) {
    str = "{}";
    return str;
  }

  static constexpr uint8_t kCount = 3;

  str.resize(size * kCount);

  for (size_t i = 0; i < size; ++i) {
    std::memcpy(&str[(i * kCount)], kHexTable[value[i]], kCount);
  }

  str.pop_back();

  return str;
}

Bytes Bytes::reverse_order(const Bytes& target) noexcept {
  if (target.empty()) {
    return Bytes();
  }

  size_t size = target.size();

  Bytes result = Bytes::create(size);

  if VUNLIKELY (result.empty()) {
    return result;
  }

  for (size_t i = 0; i < size; ++i) {
    result[i] = target[size - 1 - i];
  }

  return result;
}

std::string Bytes::encode_to_base64(const Bytes& target) noexcept {
  if (target.empty()) {
    return std::string();
  }

  std::string encoded;

  int val = 0;
  int valb = -6;

  for (auto c : target) {
    val = (val << 8) + c;
    valb += 8;

    while (valb >= 0) {
      encoded.push_back(kBase64Table[(val >> valb) & 0x3F]);
      valb -= 6;
    }
  }

  if (valb > -6) {
    encoded.push_back(kBase64Table[((val << 8) >> (valb + 8)) & 0x3F]);
  }

  size_t padding = (4 - (encoded.size() % 4)) % 4;
  encoded.append(padding, '=');

  return encoded;
}

Bytes Bytes::decode_from_base64(const std::string& target) noexcept {
  if (target.size() % 4 != 0) {
    return Bytes();
  }

  size_t padding = 0;
  size_t first_padding = target.find('=');

  if (first_padding != std::string::npos) {
    padding = target.size() - first_padding;

    if VUNLIKELY (padding > 2) {
      return Bytes();
    }

    for (size_t i = first_padding; i < target.size(); ++i) {
      if VUNLIKELY (target[i] != '=') {
        return Bytes();
      }
    }
  }

  std::vector<uint8_t> buffer;
  buffer.reserve((target.size() * 3) / 4);

  static const auto kTable = [] {
    std::array<int, 256> tbl{};
    tbl.fill(-1);

    for (size_t i = 0; i < sizeof(kBase64Table) - 1; ++i) {
      tbl[static_cast<unsigned char>(kBase64Table[i])] = i;
    }

    return tbl;
  }();

  uint32_t val = 0;
  int valb = -8;

  const size_t decode_size = first_padding == std::string::npos ? target.size() : first_padding;

  for (size_t i = 0; i < decode_size; ++i) {
    auto c = target[i];

    int idx = kTable[static_cast<unsigned char>(c)];

    if VUNLIKELY (idx == -1) {
      return Bytes();
    }

    val = (val << 6) + idx;
    valb += 6;

    if (valb >= 0) {
      buffer.emplace_back(static_cast<uint8_t>((val >> valb) & 0xFF));
      valb -= 8;
    }
  }

  return Bytes(buffer);
}

uint32_t Bytes::get_crc_32(const Bytes& target) noexcept {
  uint32_t crc = 0xFFFFFFFFU;

  static constexpr auto kCrc32Table = []() {
    std::array<uint32_t, 256> table{};

    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t crc = i;

      for (uint32_t bit = 0; bit < 8; ++bit) {
        crc = (crc & 1U) ? (kCrc32Polynomial ^ (crc >> 1U)) : (crc >> 1U);
      }

      table[i] = crc;
    }

    return table;
  }();

  for (auto u : target) {
    crc = kCrc32Table[(crc ^ u) & 0xFFU] ^ (crc >> 8U);
  }

  return crc ^ 0xFFFFFFFFU;
}

uint64_t Bytes::get_crc_64(const Bytes& target) noexcept {
  uint64_t crc = 0x0000000000000000ULL;

  static constexpr auto kCrc64Table = []() {
    std::array<uint64_t, 256> table{};

    for (uint64_t i = 0; i < 256; ++i) {
      uint64_t crc = i << 56U;

      for (uint64_t bit = 0; bit < 8; ++bit) {
        crc = (crc & 0x8000000000000000ULL) ? (kCrc64Polynomial ^ (crc << 1U)) : (crc << 1U);
      }

      table[i] = crc;
    }

    return table;
  }();

  for (auto u : target) {
    crc = kCrc64Table[((crc >> 56U) ^ u) & 0xFFU] ^ (crc << 8U);
  }

  return crc ^ 0x0000000000000000ULL;
}

Bytes::Bytes() noexcept = default;

Bytes::Bytes(const Bytes& target) noexcept {
  if VUNLIKELY (this == &target) {
    return;
  }

  if (target.offset_ > 0 && target.data_) {
    process_type(kCreate, nullptr, target.size_, target.offset_, false);

    if VLIKELY (data_) {
      std::memcpy(data_, target.data_, target.size_ + target.offset_);
    }
  } else {
    process_type(kDeepCopy, target.data_, target.size_, target.offset_, false);
  }
}

Bytes::Bytes(Bytes&& target) noexcept {
  if VUNLIKELY (this == &target) {
    return;
  }

  process_type(kMove, target.data_, target.size_, target.offset_, target.is_loaned_, &target);
}

Bytes::Bytes(const std::initializer_list<uint8_t>& list) noexcept {
  process_type(kCreate, nullptr, list.size(), 0, false);

  if VLIKELY (data_) {
    size_t index = 0;

    for (const auto& value : list) {
      data_[index] = value;
      ++index;
    }
  }
}

Bytes::Bytes(const std::vector<uint8_t>& data) noexcept {
  process_type(kDeepCopy, const_cast<uint8_t*>(data.data()), data.size(), 0, false);
}

Bytes::~Bytes() noexcept {
  if (data_ && data_ != stack_data_ && is_owner_ && capacity_ + offset_ > kStackSize) {
    bytes_free(data_, capacity_ + offset_);
  }
}

Bytes& Bytes::operator=(const Bytes& target) noexcept {
  if VUNLIKELY (this == &target) {
    return *this;
  }

  if (target.offset_ > 0 && target.data_) {
    Bytes tmp;
    tmp.process_type(kCreate, nullptr, target.size_, target.offset_, false);

    if VLIKELY (tmp.data_) {
      std::memcpy(tmp.data_, target.data_, target.size_ + target.offset_);
    }

    *this = std::move(tmp);
  } else {
    process_type(kDeepCopy, target.data_, target.size_, target.offset_, false);
  }

  return *this;
}

Bytes& Bytes::operator=(Bytes&& target) noexcept {
  if VUNLIKELY (this == &target) {
    return *this;
  }

  process_type(kMove, target.data_, target.size_, target.offset_, target.is_loaned_, &target);

  return *this;
}

Bytes& Bytes::operator=(const std::vector<uint8_t>& data) noexcept {
  process_type(kDeepCopy, const_cast<uint8_t*>(data.data()), data.size(), 0, false);

  return *this;
}

bool Bytes::operator==(const Bytes& target) const noexcept {
  if VUNLIKELY (this == &target) {
    return true;
  }

  if (size_ != target.size_) {
    return false;
  }

  const uint8_t* this_data = data_ ? (data_ + offset_) : nullptr;
  const uint8_t* target_data = target.data_ ? (target.data_ + target.offset_) : nullptr;

  if (this_data == target_data) {
    return true;
  }

  if (!this_data || !target_data) {
    return size_ == 0;
  }

  return std::memcmp(this_data, target_data, size_) == 0;
}

bool Bytes::operator!=(const Bytes& target) const noexcept { return !(*this == target); }

bool Bytes::operator==(const std::vector<uint8_t>& data) const noexcept {
  if (size_ != data.size()) {
    return false;
  }

  const uint8_t* this_data = data_ ? (data_ + offset_) : nullptr;

  if (!this_data) {
    return data.empty();
  }

  if (this_data == data.data()) {
    return true;
  }

  return std::memcmp(this_data, data.data(), size_) == 0;
}

bool Bytes::operator!=(const std::vector<uint8_t>& data) const noexcept { return !(*this == data); }

std::vector<uint8_t> Bytes::to_raw_data() const noexcept {
  if VUNLIKELY (empty() || is_ptr() || !data_) {
    return std::vector<uint8_t>();
  }

  return std::vector<uint8_t>(data_ + offset_, data_ + offset_ + size_);
}

std::string Bytes::to_string() const noexcept {
  if VUNLIKELY (empty() || is_ptr() || !data_) {
    return std::string();
  }

  return std::string(reinterpret_cast<const char*>(data_ + offset_), size_);
}

std::string_view Bytes::to_string_view() const noexcept {
  if VUNLIKELY (empty() || is_ptr() || !data_) {
    return std::string_view();
  }

  return std::string_view(reinterpret_cast<const char*>(data_ + offset_), size_);
}

bool Bytes::is_compress_data(const uint8_t* data, size_t size) noexcept {
  if VUNLIKELY (!data) {
    return false;
  }

  if (size < 12 + 1) {
    return false;
  }

  if (!check_magic(data, kCompressHeaderMagic, sizeof(kCompressHeaderMagic))) {
    return false;
  }

  if (!check_magic(data + size - sizeof(kCompressFooterMagic), kCompressFooterMagic, sizeof(kCompressFooterMagic))) {
    return false;
  }

  return true;
}

Bytes Bytes::compress_data(const uint8_t* data, size_t size, bool high_ratio) noexcept {
  Bytes target_bytes;

  if VUNLIKELY (!data || size == 0) {
    return target_bytes;
  }

  if VUNLIKELY (size > kMaxCompressCacheSize) {
    CLOG_E("Bytes: Input size too large: %zu.", size);

    return target_bytes;
  }

  size_t max_compressed_size = high_ratio ? lzav::lzav_compress_bound_hi(size) : lzav::lzav_compress_bound(size);

  target_bytes = Bytes::create(sizeof(kCompressHeaderMagic) + 4 + max_compressed_size + sizeof(kCompressFooterMagic));

  if VUNLIKELY (target_bytes.empty()) {
    return target_bytes;
  }

  std::memcpy(target_bytes.data(), kCompressHeaderMagic, sizeof(kCompressHeaderMagic));

  target_bytes[4] = static_cast<uint8_t>((size >> 24) & 0xFF);
  target_bytes[5] = static_cast<uint8_t>((size >> 16) & 0xFF);
  target_bytes[6] = static_cast<uint8_t>((size >> 8) & 0xFF);
  target_bytes[7] = static_cast<uint8_t>((size) & 0xFF);  // NOLINT(readability-redundant-parentheses)

  int result = 0;

  if (high_ratio) {
    result = lzav::lzav_compress_hi(data, target_bytes.data() + sizeof(kCompressHeaderMagic) + 4,
                                    static_cast<int>(size), static_cast<int>(max_compressed_size));
  } else {
    if (size < lzav::LZAV_MR5_THR) {
      result = lzav::lzav_compress_mref5(data, target_bytes.data() + sizeof(kCompressHeaderMagic) + 4,
                                         static_cast<int>(size), static_cast<int>(max_compressed_size),
                                         bytes_compress_cache().data(), bytes_compress_cache().size());
    } else {
      result = lzav::lzav_compress_mref6(data, target_bytes.data() + sizeof(kCompressHeaderMagic) + 4,
                                         static_cast<int>(size), static_cast<int>(max_compressed_size),
                                         bytes_compress_cache().data(), bytes_compress_cache().size());
    }
  }

  if VUNLIKELY (result <= 0) {
    target_bytes.clear();
    return target_bytes;
  }

  size_t final_size = sizeof(kCompressHeaderMagic) + 4 + static_cast<size_t>(result) + sizeof(kCompressFooterMagic);

  if VUNLIKELY (!target_bytes.resize(final_size)) {
    target_bytes.clear();
    return target_bytes;
  }

  std::memcpy(&target_bytes[sizeof(kCompressHeaderMagic) + 4 + result], kCompressFooterMagic,
              sizeof(kCompressFooterMagic));

  return target_bytes;
}

Bytes Bytes::uncompress_data(const uint8_t* data, size_t size, bool check_valid) noexcept {
  Bytes target_bytes;

  constexpr size_t kMinCompressedSize = sizeof(kCompressHeaderMagic) + 4 + sizeof(kCompressFooterMagic);

  if VUNLIKELY (!data || size < kMinCompressedSize) {
    return target_bytes;
  }

  if (check_valid) {
    if VUNLIKELY (!is_compress_data(data, size)) {
      return target_bytes;
    }
  }

  uint32_t target_size = (static_cast<uint32_t>(data[4]) << 24) | (static_cast<uint32_t>(data[5]) << 16) |
                         (static_cast<uint32_t>(data[6]) << 8) | (static_cast<uint32_t>(data[7]));

  constexpr uint32_t kMaxUncompressedSize = 256 * 1024 * 1024;  // 256MB

  if VUNLIKELY (target_size == 0 || target_size > kMaxUncompressedSize) {
    CLOG_E("Bytes: Invalid uncompressed size: %u.", target_size);

    return target_bytes;
  }

  target_bytes = Bytes::create(target_size);

  if VUNLIKELY (target_bytes.empty()) {
    return target_bytes;
  }

  size_t compressed_size = size - sizeof(kCompressHeaderMagic) - 4 - sizeof(kCompressFooterMagic);

  int result = lzav::lzav_decompress(data + sizeof(kCompressHeaderMagic) + 4, target_bytes.data(),
                                     static_cast<int>(compressed_size), static_cast<int>(target_size));

  if VUNLIKELY (result != static_cast<int>(target_size)) {
    target_bytes.clear();
  }

  return target_bytes;
}

void Bytes::clear() noexcept {
  if (data_ && data_ != stack_data_ && is_owner_ && capacity_ + offset_ > kStackSize) {
    bytes_free(data_, capacity_ + offset_);
  }

#if VLINK_BYTES_MEM_RESET
  std::memset(stack_data_, 0, kStackSize);
#endif

  data_ = nullptr;
  size_ = 0;
  capacity_ = 0;
  offset_ = 0;
  is_owner_ = false;
  is_loaned_ = false;
}

bool Bytes::shrink_to(size_t size) noexcept {
  if VUNLIKELY (!is_owner_) {
    VLOG_E("Bytes: Cannot shrink_to on non-owned Bytes.");
    return false;
  }

  if VUNLIKELY (size > size_) {
    VLOG_E("Bytes: Cannot shrink_to a size larger than current size.");
    return false;
  }

  if VUNLIKELY (size > capacity_) {
    VLOG_E("Bytes: Cannot shrink_to a size larger than capacity.");
    return false;
  }

  size_ = size;

  return true;
}

bool Bytes::reserve(size_t new_capacity) noexcept {
  if VUNLIKELY (!is_owner_) {
    VLOG_E("Bytes: Cannot reserve on non-owned Bytes.");
    return false;
  }

  if (new_capacity <= capacity_) {
    return true;
  }

  if VUNLIKELY (new_capacity > std::numeric_limits<size_t>::max() - offset_) {
    VLOG_E("Bytes: Cannot reserve due to size overflow.");
    return false;
  }

  size_t total_new_size = new_capacity + offset_;
  size_t total_old_size = capacity_ + offset_;

  uint8_t* new_data = nullptr;

  if (total_new_size > kStackSize) {
    new_data = bytes_malloc(total_new_size);

    if VUNLIKELY (!new_data) {
      VLOG_E("Bytes: Failed to reserve memory.");
      return false;
    }

#if VLINK_BYTES_MEM_RESET
    std::memset(new_data, 0, total_new_size);
#endif
  } else {
    new_data = stack_data_;
  }

  if (new_data != data_ && data_ && (size_ + offset_) > 0) {
    std::memcpy(new_data, data_, size_ + offset_);
  }

  if (data_ && data_ != stack_data_ && total_old_size > kStackSize) {
    bytes_free(data_, total_old_size);
  }

  data_ = new_data;
  capacity_ = new_capacity;

  return true;
}

bool Bytes::resize(size_t size) noexcept {
  if VUNLIKELY (!is_owner_) {
    VLOG_E("Bytes: Cannot resize on non-owned Bytes.");
    return false;
  }

  if (size <= capacity_) {
#if VLINK_BYTES_MEM_RESET

    if (size > size_ && data_) {
      size_t available_space = capacity_;
      size_t new_end_offset = size;

      if (new_end_offset <= available_space) {
        std::memset(data_ + offset_ + size_, 0, size - size_);
      } else {
        VLOG_E("Bytes: Resize would exceed allocated memory.");
        return false;
      }
    }
#endif
    size_ = size;

    return true;
  }

  if VUNLIKELY (!reserve(size)) {
    return false;
  }

  size_ = size;

  return true;
}

Bytes& Bytes::shallow_copy(const Bytes& bytes) noexcept {
  process_type(kShallowCopy, bytes.data_, bytes.size_, bytes.offset_, false);

  return *this;
}

Bytes& Bytes::deep_copy(const Bytes& bytes) noexcept {
  if (bytes.offset_ > 0 && bytes.data_) {
    Bytes tmp = Bytes::deep_copy(bytes.data_, bytes.size_ + bytes.offset_);
    process_type(kCreate, nullptr, bytes.size_, bytes.offset_, false);

    if VLIKELY (data_ && tmp.data_) {
      std::memcpy(data_, tmp.data_, bytes.size_ + bytes.offset_);
    }
  } else {
    process_type(kDeepCopy, const_cast<uint8_t*>(bytes.data()), bytes.size_, 0, false);
  }

  return *this;
}

Bytes& Bytes::deep_copy_self() noexcept {
  if VUNLIKELY (is_owner_) {
    return *this;
  }

  if VUNLIKELY (!data_ || size_ == 0) {
    clear();
    return *this;
  }

  if (offset_ > 0) {
    uint8_t* old_data = data_;
    size_t total_size = size_ + offset_;

    process_type(kCreate, nullptr, size_, offset_, false);

    if VLIKELY (data_) {
      std::memcpy(data_, old_data, total_size);
    }
  } else {
    process_type(kDeepCopy, data_ + offset_, size_, 0, false);
  }

  return *this;
}

Bytes::Bytes(Type type, uint8_t* data, size_t size, uint8_t offset, bool loaned) noexcept {
  process_type(type, data, size, offset, loaned);
}

void Bytes::process_type(Type type, uint8_t* data, size_t size, uint8_t offset, bool loaned, Bytes* tmp) noexcept {
  switch (type) {
    case kCreate: {
      // VLOG_W("kCreate");

      if VUNLIKELY (size > std::numeric_limits<size_t>::max() - offset) {
        VLOG_E("Bytes: Cannot create due to size overflow.");
        clear();
        return;
      }

      size_t total_size = size + offset;
      uint8_t* new_data = nullptr;

      if (total_size > kStackSize) {
        new_data = bytes_malloc(total_size);

        if VUNLIKELY (!new_data) {
          VLOG_E("Bytes: Failed to allocate memory.");
          clear();
          return;
        }
#if VLINK_BYTES_MEM_RESET
        std::memset(new_data, 0, total_size);
#endif
      } else if (total_size != 0) {
        new_data = stack_data_;
      }

      if (is_owner_ && data_) {
        if (data_ != stack_data_ && capacity_ + offset_ > kStackSize) {
          bytes_free(data_, capacity_ + offset_);
          data_ = nullptr;
        }
      }

      data_ = new_data;

      size_ = size;
      capacity_ = size;
      offset_ = offset;
      is_owner_ = true;
      is_loaned_ = false;

      break;
    }

    case kShallowCopy: {
      // VLOG_W("kShallowCopy");

      if (is_owner_ && data_) {
        if VUNLIKELY (data_ == data) {
          VLOG_E("Bytes: Cannot shallow copy self.");
          return;
        }

        if (data_ != stack_data_ && capacity_ + offset_ > kStackSize) {
          bytes_free(data_, capacity_ + offset_);
        }
      }

      data_ = data;
      size_ = size;
      capacity_ = 0;
      offset_ = offset;
      is_owner_ = false;
      is_loaned_ = loaned;

      break;
    }

    case kDeepCopy: {
      // VLOG_W("kDeepCopy");

      if VUNLIKELY (size > std::numeric_limits<size_t>::max() - offset) {
        VLOG_E("Bytes: Cannot deep copy due to size overflow.");
        clear();
        return;
      }

      size_t total_size = size + offset;

      uint8_t* deferred_free_buffer = nullptr;
      size_t deferred_free_size = 0;

      if (is_owner_ && data_) {
        if VUNLIKELY (data_ == data) {
          VLOG_E("Bytes: Cannot deep copy self.");
          return;
        }

        bool can_reuse =
            (data_ != stack_data_ && capacity_ + offset_ > kStackSize && capacity_ + offset_ == total_size);

        if (can_reuse && data) {
          uint8_t* dst_start = data_;
          uint8_t* dst_end = data_ + capacity_ + offset_;
          const uint8_t* src_start = data;
          const uint8_t* src_end = data + size;

          if (src_start < dst_end && dst_start < src_end) {
            can_reuse = false;
          }
        }

        if (data_ != stack_data_ && capacity_ + offset_ > kStackSize && !can_reuse) {
          deferred_free_buffer = data_;
          deferred_free_size = capacity_ + offset_;
          data_ = nullptr;
          capacity_ = 0;
          offset_ = 0;
        }
      }

      if VLIKELY (total_size != 0) {
        if (total_size > kStackSize) {
          if (capacity_ + offset_ != total_size || data_ == stack_data_) {
            data_ = bytes_malloc(total_size);

            if VUNLIKELY (!data_) {
              VLOG_E("Bytes: Failed to allocate memory.");

              if (deferred_free_buffer) {
                bytes_free(deferred_free_buffer, deferred_free_size);
              }

              clear();

              return;
            }

#if VLINK_BYTES_MEM_RESET
            std::memset(data_, 0, total_size);
#endif
          }
        } else {
          data_ = stack_data_;
        }

        if VLIKELY (data && data_) {
          if VLIKELY (size > 0) {
            std::memcpy(data_ + offset, data, size);
          }
        }

        size_ = size;
        capacity_ = size;
        offset_ = offset;
        is_owner_ = true;
        is_loaned_ = false;
      } else {
        data_ = data;
        size_ = 0;
        capacity_ = 0;
        offset_ = 0;
        is_owner_ = false;
        is_loaned_ = false;
      }

      if (deferred_free_buffer) {
        bytes_free(deferred_free_buffer, deferred_free_size);
      }

      break;
    }

    case kMove: {
      // VLOG_W("kMove");

      if VUNLIKELY (!tmp) {
        VLOG_E("Bytes: Reference tmp is empty.");
        break;
      }

      if (is_owner_ && data_) {
        if VUNLIKELY (data_ == data) {
          VLOG_E("Bytes: Cannot move self.");
          return;
        }

        if (data_ != stack_data_ && capacity_ + offset_ > kStackSize) {
          bytes_free(data_, capacity_ + offset_);
        }
      }

      if (tmp->capacity_ + tmp->offset_ != 0 && tmp->capacity_ + tmp->offset_ <= kStackSize && tmp->is_owner_) {
        size_t copy_size = tmp->size_ + tmp->offset_;

        if VLIKELY (copy_size > 0 && copy_size <= kStackSize) {
          std::memcpy(stack_data_, tmp->stack_data_, copy_size);
        }

        data_ = stack_data_;
      } else {
        data_ = tmp->data_;
      }

      size_ = tmp->size_;
      capacity_ = tmp->capacity_;
      offset_ = tmp->offset_;
      is_owner_ = tmp->is_owner_;
      is_loaned_ = tmp->is_loaned_;

      tmp->data_ = nullptr;
      tmp->size_ = 0;
      tmp->capacity_ = 0;
      tmp->offset_ = 0;
      tmp->is_owner_ = false;
      tmp->is_loaned_ = false;

#if VLINK_BYTES_MEM_RESET
      std::memset(tmp->stack_data_, 0, kStackSize);
#endif

      break;
    }

    default:
      break;
  }
}

std::ostream& operator<<(std::ostream& ostream, const Bytes& target) noexcept {
  if (target.empty()) {
    ostream << "{}";
    return ostream;
  }

  if (target.is_ptr()) {
    ostream << "(ptr)" << static_cast<void*>(target.data_);
    return ostream;
  }

  if (target.offset_ > 0) {
    ostream << "(offset=" << +target.offset_ << ") ";
  }

  ostream << Bytes::convert_to_hex_str(target.data_ + target.offset_, target.size());

  return ostream;
}

}  // namespace vlink
