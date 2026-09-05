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

#include "./efbs_common.h"

[[maybe_unused]] std::pair<int, int> get_terminal_size() {
  auto size = vlink::Utils::get_terminal_size();

  if (max_columns > 0) {
    size.first = max_columns;
  }

  if (max_rows > 0) {
    size.second = max_rows;
  }

  if (size.first < 0) {
    size.first = 1000;
  }

  if (size.second < 0) {
    size.second = 25;
  }

  return size;
}

[[maybe_unused]] void decode_terminal_utf8(std::string_view text, size_t index, uint32_t& code_point, size_t& bytes) {
  const auto lead = static_cast<unsigned char>(text[index]);

  if (lead < 0x80) {
    code_point = lead;
    bytes = 1;
    return;
  }

  if ((lead & 0xE0) == 0xC0 && index + 1 < text.size() &&
      (static_cast<unsigned char>(text[index + 1]) & 0xC0) == 0x80) {
    code_point = (static_cast<uint32_t>(lead & 0x1F) << 6) | (static_cast<unsigned char>(text[index + 1]) & 0x3F);
    bytes = 2;
    return;
  }

  if ((lead & 0xF0) == 0xE0 && index + 2 < text.size() &&
      (static_cast<unsigned char>(text[index + 1]) & 0xC0) == 0x80 &&
      (static_cast<unsigned char>(text[index + 2]) & 0xC0) == 0x80) {
    code_point = (static_cast<uint32_t>(lead & 0x0F) << 12) |
                 (static_cast<uint32_t>(static_cast<unsigned char>(text[index + 1]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(text[index + 2]) & 0x3F);
    bytes = 3;
    return;
  }

  if ((lead & 0xF8) == 0xF0 && index + 3 < text.size() &&
      (static_cast<unsigned char>(text[index + 1]) & 0xC0) == 0x80 &&
      (static_cast<unsigned char>(text[index + 2]) & 0xC0) == 0x80 &&
      (static_cast<unsigned char>(text[index + 3]) & 0xC0) == 0x80) {
    code_point = (static_cast<uint32_t>(lead & 0x07) << 18) |
                 (static_cast<uint32_t>(static_cast<unsigned char>(text[index + 1]) & 0x3F) << 12) |
                 (static_cast<uint32_t>(static_cast<unsigned char>(text[index + 2]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(text[index + 3]) & 0x3F);
    bytes = 4;
    return;
  }

  code_point = lead;
  bytes = 1;
}

[[maybe_unused]] int terminal_codepoint_width(uint32_t code_point) {
  if (code_point < 0x20 || code_point == 0x7F) {
    return 0;
  }

  if ((code_point >= 0x0300 && code_point <= 0x036F) || (code_point >= 0x200B && code_point <= 0x200D) ||
      code_point == 0xFEFF) {
    return 0;
  }

  if ((code_point >= 0x1100 && code_point <= 0x115F) || (code_point >= 0x2E80 && code_point <= 0x303E) ||
      (code_point >= 0x3041 && code_point <= 0x33FF) || (code_point >= 0x3400 && code_point <= 0x4DBF) ||
      (code_point >= 0x4E00 && code_point <= 0x9FFF) || (code_point >= 0xA000 && code_point <= 0xA4CF) ||
      (code_point >= 0xAC00 && code_point <= 0xD7A3) || (code_point >= 0xF900 && code_point <= 0xFAFF) ||
      (code_point >= 0xFE30 && code_point <= 0xFE4F) || (code_point >= 0xFF00 && code_point <= 0xFF60) ||
      (code_point >= 0xFFE0 && code_point <= 0xFFE6) || (code_point >= 0x20000 && code_point <= 0x2FFFD) ||
      (code_point >= 0x30000 && code_point <= 0x3FFFD)) {
    return 2;
  }

  return 1;
}
