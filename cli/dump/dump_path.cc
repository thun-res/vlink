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

#include "./dump_path.h"

#include <vlink/base/helpers.h>
#include <vlink/base/utils.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

namespace vlink::dump {

std::filesystem::path utf8_to_path(const std::string& utf8) noexcept {
  try {
    if (utf8.empty()) {
      return {};
    }

#ifdef _WIN32
    return std::filesystem::path(vlink::Helpers::string_to_wstring(utf8));
#else
    return std::filesystem::path(utf8);
#endif
  } catch (const std::filesystem::filesystem_error&) {
    return {};
  } catch (const std::exception&) {
    return {};
  }
}

std::string path_to_utf8(const std::filesystem::path& path) noexcept {
  try {
#ifdef _WIN32
    auto result = vlink::Helpers::path_to_string(path);
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
#else
    return path.string();
#endif
  } catch (const std::filesystem::filesystem_error&) {
    return {};
  } catch (const std::exception&) {
    return {};
  }
}

std::string normalize_dir(std::string dir) {
#ifdef _WIN32

  try {
    dir = vlink::Helpers::path_to_string(std::filesystem::path(dir));
  } catch (std::filesystem::filesystem_error&) {
  }

  std::replace(dir.begin(), dir.end(), '\\', '/');
#endif

  if (!dir.empty() && dir.back() == '/') {
    dir.pop_back();
  }

  return dir;
}

std::string get_home_config_path(const std::string& filename) {
  std::string home = vlink::Utils::get_env("HOME");

  if (home.empty()) {
    home = vlink::Utils::get_env("USERPROFILE");
  }

  if VUNLIKELY (home.empty()) {
    return {};
  }

  auto home_path = utf8_to_path(home);

  if VUNLIKELY (home_path.empty()) {
    return {};
  }

  try {
    auto joined = home_path / filename;
    return path_to_utf8(joined);
  } catch (const std::filesystem::filesystem_error&) {
    return {};
  } catch (const std::exception&) {
    return {};
  }
}

std::string read_home_config(const std::string& filename) {
  auto config_path_utf8 = get_home_config_path(filename);

  if VUNLIKELY (config_path_utf8.empty()) {
    return {};
  }

  auto fs_path = utf8_to_path(config_path_utf8);

  if VUNLIKELY (fs_path.empty()) {
    return {};
  }

  std::error_code ec;

  if (!std::filesystem::is_regular_file(fs_path, ec) || ec) {
    return {};
  }

  std::ifstream input(fs_path, std::ios::binary);

  if VUNLIKELY (!input.is_open()) {
    return {};
  }

  std::string content;
  content.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());

  auto back_pos = content.find_last_not_of(" \t\r\n");

  if (back_pos != std::string::npos) {
    content.erase(back_pos + 1);
  } else {
    content.clear();
  }

  auto front_pos = content.find_first_not_of(" \t\r\n");

  if (front_pos != std::string::npos) {
    content.erase(0, front_pos);
  }

  return content;
}

}  // namespace vlink::dump
