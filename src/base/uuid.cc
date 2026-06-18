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

#include "./base/uuid.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <ostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace vlink {

static constexpr char kHexDigits[] = "0123456789abcdef";

static bool is_hex_char(char ch) noexcept {
  return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

static uint8_t hex_to_nibble(char ch) noexcept {
  if (ch >= '0' && ch <= '9') {
    return static_cast<uint8_t>(ch - '0');
  }

  if (ch >= 'a' && ch <= 'f') {
    return static_cast<uint8_t>(ch - 'a' + 10);
  }

  if (ch >= 'A' && ch <= 'F') {
    return static_cast<uint8_t>(ch - 'A' + 10);
  }

  return 0U;
}

static std::mt19937::result_type fallback_seed_value() noexcept {
  const auto count = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  const auto unsigned_count = static_cast<uint64_t>(count);
  const auto low = static_cast<std::mt19937::result_type>(unsigned_count & 0xFFFFFFFFULL);
  const auto high = static_cast<std::mt19937::result_type>((unsigned_count >> 32U) & 0xFFFFFFFFULL);

  return static_cast<std::mt19937::result_type>(low ^ high);
}

static std::mt19937 make_seeded_engine() noexcept {
  try {
    std::random_device rd;
    std::array<uint32_t, 8> seed_data{};
    std::generate(seed_data.begin(), seed_data.end(), std::ref(rd));
    std::seed_seq seq(seed_data.begin(), seed_data.end());

    return std::mt19937(seq);
  } catch (...) {
    return std::mt19937(fallback_seed_value());
  }
}

static std::mt19937& thread_engine() noexcept {
  thread_local std::mt19937 engine = make_seeded_engine();

  return engine;
}

static void fill_random_bytes(std::mt19937& engine, uint8_t* buffer, size_t count) noexcept {
  if (count == 0U || buffer == nullptr) {
    return;
  }

  std::uniform_int_distribution<uint32_t> distribution;

  size_t i = 0U;

  while (count - i >= 4U) {
    const uint32_t value = distribution(engine);
    buffer[i] = static_cast<uint8_t>(value & 0xFFU);
    buffer[i + 1U] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
    buffer[i + 2U] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
    buffer[i + 3U] = static_cast<uint8_t>((value >> 24U) & 0xFFU);
    i += 4U;
  }

  if (i < count) {
    uint32_t value = distribution(engine);

    while (i < count) {
      buffer[i] = static_cast<uint8_t>(value & 0xFFU);
      value >>= 8U;
      ++i;
    }
  }
}

static void encode_hex(const uint8_t* bytes, size_t count, std::string& out) {
  if (count == 0U || bytes == nullptr) {
    return;
  }

  for (size_t i = 0U; i < count; ++i) {
    const uint8_t byte_value = bytes[i];
    out.push_back(kHexDigits[(byte_value >> 4U) & 0x0FU]);
    out.push_back(kHexDigits[byte_value & 0x0FU]);
  }
}

static std::string_view trim_braces(std::string_view str) noexcept {
  if (str.size() >= 2U && str.front() == '{' && str.back() == '}') {
    return str.substr(1U, str.size() - 2U);
  }

  return str;
}

static bool parse_into(std::string_view str, std::array<uint8_t, Uuid::kByteSize>* out) noexcept {
  if (str.size() < 2U) {
    return false;
  }

  bool first_digit = true;
  size_t index = 0U;
  std::array<uint8_t, Uuid::kByteSize> data{};

  for (char ch : str) {
    if (ch == '-') {
      continue;
    }

    if (index >= Uuid::kByteSize || !is_hex_char(ch)) {
      return false;
    }

    if (first_digit) {
      data[index] = static_cast<uint8_t>(hex_to_nibble(ch) << 4U);
      first_digit = false;
    } else {
      data[index] = static_cast<uint8_t>(data[index] | hex_to_nibble(ch));
      ++index;
      first_digit = true;
    }
  }

  if (index != Uuid::kByteSize || !first_digit) {
    return false;
  }

  if (out != nullptr) {
    *out = data;
  }

  return true;
}

// Uuid
std::string Uuid::to_string() const noexcept {
  try {
    std::string out;
    out.reserve(kStringSize);

    for (size_t i = 0U; i < kByteSize; ++i) {
      if (i == 4U || i == 6U || i == 8U || i == 10U) {
        out.push_back('-');
      }

      const uint8_t byte_value = data_[i];
      out.push_back(kHexDigits[(byte_value >> 4U) & 0x0FU]);
      out.push_back(kHexDigits[byte_value & 0x0FU]);
    }

    return out;
  } catch (...) {
    return {};
  }
}

std::string Uuid::to_compact_string() const noexcept {
  try {
    std::string out;
    out.reserve(kByteSize * 2U);
    encode_hex(data_.data(), kByteSize, out);

    return out;
  } catch (...) {
    return {};
  }
}

std::optional<Uuid> Uuid::from_string(std::string_view str) noexcept {
  if (str.size() >= 2U && str.front() == '{' && str.back() != '}') {
    return std::nullopt;
  }

  std::array<uint8_t, kByteSize> data{};

  if (!parse_into(trim_braces(str), &data)) {
    return std::nullopt;
  }

  return Uuid{data};
}

bool Uuid::is_valid(std::string_view str) noexcept { return from_string(str).has_value(); }

std::optional<Uuid> Uuid::from_string(const char* str) noexcept {
  if (str == nullptr) {
    return std::nullopt;
  }

  return from_string(std::string_view(str));
}

bool Uuid::is_valid(const char* str) noexcept { return from_string(str).has_value(); }

Uuid Uuid::generate_random() noexcept { return generate_random(thread_engine()); }

Uuid Uuid::generate_random(std::mt19937& engine) noexcept {
  std::array<uint8_t, kByteSize> data{};
  fill_random_bytes(engine, data.data(), kByteSize);

  data[6] = static_cast<uint8_t>((data[6] & 0x0FU) | 0x40U);
  data[8] = static_cast<uint8_t>((data[8] & 0x3FU) | 0x80U);

  return Uuid{data};
}

std::vector<Uuid::value_type> Uuid::random_bytes(size_t count) noexcept {
  try {
    std::vector<value_type> buffer(count);

    if (count > 0U) {
      fill_random_bytes(thread_engine(), buffer.data(), count);
    }

    return buffer;
  } catch (...) {
    return {};
  }
}

std::string Uuid::random_hex(size_t byte_count) noexcept {
  try {
    std::string out;

    if (byte_count == 0U) {
      return out;
    }

    if (byte_count > out.max_size() / 2U) {
      return {};
    }

    out.reserve(byte_count * 2U);

    std::vector<value_type> buffer(byte_count);
    fill_random_bytes(thread_engine(), buffer.data(), byte_count);
    encode_hex(buffer.data(), byte_count, out);

    return out;
  } catch (...) {
    return {};
  }
}

std::ostream& operator<<(std::ostream& ostream, const Uuid& id) {
  ostream << id.to_string();

  return ostream;
}

}  // namespace vlink
