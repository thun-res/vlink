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

#include "./base/format.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace vlink {
namespace format {
namespace detail {

template <typename UIntT>
inline static int count_digits(UIntT n) noexcept {
  int count = 1;

  while (n >= 10) {
    n /= 10;
    ++count;
  }

  return count;
}

template <typename UIntT>
inline static void write_int_digits(char* buf, UIntT value, int num_digits) noexcept {
  char* end = buf + num_digits;

  while (value >= 10) {
    auto digit = static_cast<unsigned>(value % 10);
    *--end = static_cast<char>('0' + digit);
    value /= 10;
  }

  *--end = static_cast<char>('0' + value);
}

// StringWriter
StringWriter::StringWriter(char* buf, size_t size) noexcept : begin_(buf), ptr_(buf), end_(buf + size) {}

char* StringWriter::out() const noexcept { return ptr_; }

size_t StringWriter::written() const noexcept { return static_cast<size_t>(ptr_ - begin_); }

size_t StringWriter::total_size() const noexcept { return total_size_; }

void StringWriter::write(char c) {
  ++total_size_;

  if VLIKELY (ptr_ < end_) {
    *ptr_++ = c;
  }
}

void StringWriter::write(const char* s, size_t count) {
  total_size_ += count;

  auto avail = static_cast<size_t>(end_ - ptr_);
  size_t n = (count <= avail) ? count : avail;

  if VLIKELY (n > 0) {
    std::memcpy(ptr_, s, n);
    ptr_ += n;
  }
}

void StringWriter::write(std::string_view sv) { write(sv.data(), sv.size()); }

// NOLINTBEGIN
size_t format_uint_to(char* buf, unsigned value) noexcept {
  int num_digits = count_digits(value);
  write_int_digits(buf, value, num_digits);

  return static_cast<size_t>(num_digits);
}

size_t format_int_to(char* buf, int value) noexcept {
  if (value < 0) {
    buf[0] = '-';
    unsigned u = static_cast<unsigned>(-(value + 1)) + 1;

    return 1 + format_uint_to(buf + 1, u);
  }

  return format_uint_to(buf, static_cast<unsigned>(value));
}

size_t format_ulong_long_to(char* buf, unsigned long long value) noexcept {
  int num_digits = count_digits(value);
  write_int_digits(buf, value, num_digits);

  return static_cast<size_t>(num_digits);
}

size_t format_long_long_to(char* buf, long long value) noexcept {
  if (value < 0) {
    buf[0] = '-';
    unsigned long long u = static_cast<unsigned long long>(-(value + 1)) + 1;

    return 1 + format_ulong_long_to(buf + 1, u);
  }

  return format_ulong_long_to(buf, static_cast<unsigned long long>(value));
}

size_t format_pointer_to(char* buf, const void* ptr) noexcept {
  static constexpr const char kHexDigits[] = "0123456789abcdef";
  static_assert(sizeof(uintptr_t) <= 8, "pointer size > 64bit not supported");

  buf[0] = '0';
  buf[1] = 'x';

  char hex[16];
  int i = 16;
  auto value = reinterpret_cast<uintptr_t>(ptr);

  do {
    hex[--i] = kHexDigits[value & 0xF];
    value >>= 4;
  } while (value != 0);

  size_t n = static_cast<size_t>(16 - i);
  std::memcpy(buf + 2, hex + i, n);

  return 2 + n;
}

size_t format_float_to(char* buf, size_t buflen, float value) noexcept {
  int len = std::snprintf(buf, buflen, "%g", static_cast<double>(value));

  if VLIKELY (len > 0 && static_cast<size_t>(len) < buflen) {
    return static_cast<size_t>(len);
  }

  return 0;
}

size_t format_double_to(char* buf, size_t buflen, double value) noexcept {
  int len = std::snprintf(buf, buflen, "%g", value);

  if VLIKELY (len > 0 && static_cast<size_t>(len) < buflen) {
    return static_cast<size_t>(len);
  }

  return 0;
}

// NOLINTEND

}  // namespace detail
}  // namespace format
}  // namespace vlink
