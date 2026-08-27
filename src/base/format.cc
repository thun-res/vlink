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
#include <cstring>

#include "./base/helpers.h"

namespace vlink {
namespace format {
namespace detail {

// NOLINTBEGIN
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
  return Helpers::format_floating_to(buf, buflen, value);
}

size_t format_double_to(char* buf, size_t buflen, double value) noexcept {
  return Helpers::format_floating_to(buf, buflen, value);
}

size_t format_long_double_to(char* buf, size_t buflen, long double value) noexcept {
  return Helpers::format_floating_to(buf, buflen, value);
}

size_t format_double_spec_to(char* buf, size_t buflen, double value, char type, int precision, bool alt) noexcept {
  return Helpers::format_floating_spec_to(buf, buflen, value, type, precision, alt);
}

size_t format_long_double_spec_to(char* buf, size_t buflen, long double value, char type, int precision,
                                  bool alt) noexcept {
  return Helpers::format_floating_spec_to(buf, buflen, value, type, precision, alt);
}

const char* parse_spec_ref(const char* p, const char* end, int& ref) noexcept {
  ++p;
  int index = kSpecRefAuto;

  if (p != end && *p >= '0' && *p <= '9') {
    index = 0;

    while (p != end && *p >= '0' && *p <= '9') {
      if (index < 100000000) {
        index = index * 10 + (*p - '0');
      }

      ++p;
    }
  }

  if (p != end && *p == '}') {
    ref = index;
    ++p;
  }

  return p;
}

const char* parse_format_spec(const char* p, const char* end, FormatSpec& spec) noexcept {
  if (p != end && p + 1 != end && (p[1] == '<' || p[1] == '>' || p[1] == '^') && *p != '{' && *p != '}') {
    spec.fill = *p;
    spec.align = p[1] == '<' ? Align::kLeft : (p[1] == '>' ? Align::kRight : Align::kCenter);
    p += 2;
  } else if (p != end && (*p == '<' || *p == '>' || *p == '^')) {
    spec.align = *p == '<' ? Align::kLeft : (*p == '>' ? Align::kRight : Align::kCenter);
    ++p;
  }

  if (p != end && (*p == '+' || *p == '-' || *p == ' ')) {
    spec.sign = *p == '+' ? Sign::kPlus : (*p == ' ' ? Sign::kSpace : Sign::kMinus);
    ++p;
  }

  if (p != end && *p == '#') {
    spec.alt = true;
    ++p;
  }

  if (p != end && *p == '0') {
    spec.zero = true;
    ++p;
  }

  if (p != end && *p == '{') {
    p = parse_spec_ref(p, end, spec.width_ref);
  } else {
    while (p != end && *p >= '0' && *p <= '9') {
      if (spec.width < 100000000) {
        spec.width = spec.width * 10 + (*p - '0');
      }

      ++p;
    }
  }

  if (p != end && *p == '.') {
    ++p;
    spec.precision = 0;

    if (p != end && *p == '{') {
      p = parse_spec_ref(p, end, spec.precision_ref);
    } else {
      while (p != end && *p >= '0' && *p <= '9') {
        if (spec.precision < 100000000) {
          spec.precision = spec.precision * 10 + (*p - '0');
        }

        ++p;
      }
    }
  }

  if (p != end) {
    switch (*p) {
      case 'b':
      case 'B':
      case 'c':
      case 'd':
      case 'o':
      case 'x':
      case 'X':
      case 'a':
      case 'A':
      case 'e':
      case 'E':
      case 'f':
      case 'F':
      case 'g':
      case 'G':
      case 's':
      case 'p':
      case '?':
        spec.type = *p;
        ++p;
        break;
      default:
        break;
    }
  }

  return p;
}

// NOLINTEND

}  // namespace detail
}  // namespace format
}  // namespace vlink
