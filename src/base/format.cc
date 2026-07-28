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

// NOLINTEND

}  // namespace detail
}  // namespace format
}  // namespace vlink
