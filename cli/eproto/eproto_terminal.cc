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

#include "./eproto_common.h"

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
      (code_point >= 0xFE00 && code_point <= 0xFE0F) || code_point == 0xFEFF) {
    return 0;
  }

  if ((code_point >= 0x1100 && code_point <= 0x115F) || (code_point >= 0x231A && code_point <= 0x231B) ||
      (code_point >= 0x23E9 && code_point <= 0x23F3) || (code_point >= 0x23F8 && code_point <= 0x23FA) ||
      (code_point >= 0x25FD && code_point <= 0x25FE) || (code_point >= 0x2614 && code_point <= 0x2615) ||
      (code_point >= 0x2648 && code_point <= 0x2653) || code_point == 0x267F || code_point == 0x2693 ||
      code_point == 0x26A1 || (code_point >= 0x26AA && code_point <= 0x26AB) ||
      (code_point >= 0x26BD && code_point <= 0x26BE) || (code_point >= 0x26C4 && code_point <= 0x26C5) ||
      code_point == 0x26CE || code_point == 0x26D4 || code_point == 0x26EA ||
      (code_point >= 0x26F2 && code_point <= 0x26F3) || code_point == 0x26F5 || code_point == 0x26FA ||
      code_point == 0x26FD || code_point == 0x2705 || (code_point >= 0x270A && code_point <= 0x270B) ||
      code_point == 0x2728 || code_point == 0x274C || code_point == 0x274E ||
      (code_point >= 0x2753 && code_point <= 0x2755) || code_point == 0x2757 ||
      (code_point >= 0x2795 && code_point <= 0x2797) || code_point == 0x27B0 || code_point == 0x27BF ||
      code_point == 0x2B50 || code_point == 0x2B55 || (code_point >= 0x2E80 && code_point <= 0x303E) ||
      (code_point >= 0x3041 && code_point <= 0x33FF) || (code_point >= 0x3400 && code_point <= 0x4DBF) ||
      (code_point >= 0x4E00 && code_point <= 0x9FFF) || (code_point >= 0xA000 && code_point <= 0xA4CF) ||
      (code_point >= 0xAC00 && code_point <= 0xD7A3) || (code_point >= 0xF900 && code_point <= 0xFAFF) ||
      (code_point >= 0xFE30 && code_point <= 0xFE4F) || (code_point >= 0xFF00 && code_point <= 0xFF60) ||
      (code_point >= 0xFFE0 && code_point <= 0xFFE6) || (code_point >= 0x1F300 && code_point <= 0x1FAFF) ||
      (code_point >= 0x20000 && code_point <= 0x2FFFD) || (code_point >= 0x30000 && code_point <= 0x3FFFD)) {
    return 2;
  }

  return 1;
}

[[maybe_unused]] std::string_view truncate_terminal_text(std::string_view text, int max_width) {
  if (max_width <= 0) {
    return {};
  }

  int width = 0;
  size_t offset = 0;

  while (offset < text.size()) {
    uint32_t code_point = 0;
    size_t bytes = 0;
    decode_terminal_utf8(text, offset, code_point, bytes);
    const int cw = terminal_codepoint_width(code_point);

    if (width + cw > max_width) {
      break;
    }

    width += cw;
    offset += bytes;
  }

  return text.substr(0, offset);
}
