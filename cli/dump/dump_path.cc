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
  auto result = vlink::Helpers::path_to_string(path);

#ifdef _WIN32
  std::replace(result.begin(), result.end(), '\\', '/');
#endif

  return result;
}

std::string normalize_dir(std::string dir) {
  if (dir.empty()) {
    return dir;
  }

  const auto path = utf8_to_path(dir);

  if VUNLIKELY (path.empty()) {
    return dir;
  }

#ifdef _WIN32
  auto normalized = path_to_utf8(path);

  if VLIKELY (!normalized.empty()) {
    dir = normalized;
  }
#endif

  if (!dir.empty() && dir.back() == '/' && path != path.root_path()) {
    dir.pop_back();
  }

  return dir;
}

static std::string get_home_config_path(const std::string& filename) {
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
  return vlink::Helpers::trim_string(content);
}

}  // namespace vlink::dump
