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

#include "./monitor_common.h"

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

[[maybe_unused]] int filter_box_display_width(const std::string& line) {
  int col = 0;
  size_t i = 0;

  while (i < line.size()) {
    if (line[i] == '\033') {
      ++i;

      if (i < line.size() && line[i] == '[') {
        ++i;

        while (i < line.size() && !(line[i] >= '@' && line[i] <= '~')) {
          ++i;
        }

        if (i < line.size()) {
          ++i;
        }
      }

      continue;
    }

    ++i;

    while (i < line.size() && (static_cast<unsigned char>(line[i]) & 0xC0) == 0x80) {
      ++i;
    }

    ++col;
  }

  return col;
}

[[maybe_unused]] size_t filter_box_col_to_byte(const std::string& line, int target_col) {
  int col = 0;
  size_t i = 0;

  while (i < line.size() && col < target_col) {
    if (line[i] == '\033') {
      ++i;

      if (i < line.size() && line[i] == '[') {
        ++i;

        while (i < line.size() && !(line[i] >= '@' && line[i] <= '~')) {
          ++i;
        }

        if (i < line.size()) {
          ++i;
        }
      }

      continue;
    }

    ++i;

    while (i < line.size() && (static_cast<unsigned char>(line[i]) & 0xC0) == 0x80) {
      ++i;
    }

    ++col;
  }

  return i;
}

[[maybe_unused]] std::string filter_box_sgr_prefix(const std::string& line, size_t limit) {
  std::string codes;
  size_t i = 0;

  while (i < line.size() && i < limit) {
    if (line[i] != '\033') {
      ++i;

      continue;
    }

    size_t start = i;
    ++i;

    if (i < line.size() && line[i] == '[') {
      ++i;

      while (i < line.size() && !(line[i] >= '@' && line[i] <= '~')) {
        ++i;
      }

      if (i < line.size()) {
        ++i;
      }
    }

    codes += line.substr(start, i - start);
  }

  return codes;
}

[[maybe_unused]] std::string filter_highlight_url(const std::string& url, const std::vector<std::string>& terms) {
  std::string lower = url;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  std::vector<bool> mark(url.size(), false);
  bool any = false;

  for (const auto& term : terms) {
    if (term.empty()) {
      continue;
    }

    std::string needle = term;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    size_t pos = lower.find(needle);

    while (pos != std::string::npos) {
      for (size_t i = pos; i < pos + needle.size(); ++i) {
        mark[i] = true;
      }

      any = true;
      pos = lower.find(needle, pos + 1);
    }
  }

  if (!any) {
    return url;
  }

  std::string out;
  out.reserve(url.size() + 16);

  bool active = false;

  for (size_t i = 0; i < url.size(); ++i) {
    if (mark[i] && !active) {
      out += "\033[1;4m";
      active = true;
    } else if (!mark[i] && active) {
      out += "\033[22;24m";
      active = false;
    }

    out += url[i];
  }

  if (active) {
    out += "\033[22;24m";
  }

  return out;
}
