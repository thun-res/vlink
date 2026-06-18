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

#include "./extension/url_remap.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "./base/helpers.h"
#include "./base/logger.h"

// json
#include <nlohmann/json.hpp>

namespace vlink {

// UrlRemap
UrlRemap::UrlRemap() noexcept = default;

UrlRemap::~UrlRemap() noexcept = default;

bool UrlRemap::load(const std::string& file_path) noexcept {
  if VUNLIKELY (is_valid_) {
    return false;
  }

  remap_list_.clear();
  cache_map_.clear();

  std::filesystem::path target_path;

  try {
#ifdef _WIN32
    target_path = std::filesystem::path(Helpers::string_to_wstring(file_path));
#else
    target_path = std::filesystem::path(file_path);
#endif

    if VUNLIKELY (!std::filesystem::exists(target_path)) {
      error_string_ = "UrlRemap file does not exist";

      is_valid_ = false;
      return false;
    }
  } catch (std::filesystem::filesystem_error&) {
    error_string_ = "UrlRemap file does not exist";

    is_valid_ = false;
    return false;
  }

  try {
    nlohmann::ordered_json root_json;

    {
      std::ifstream file(target_path);

      file >> root_json;

      file.close();
    }

    if VUNLIKELY (root_json.empty() || !root_json.is_object()) {
      error_string_ = "Wrong format";

      VLOG_W("UrlRemap: Parse error: ", error_string_, ".");

      is_valid_ = false;
      return false;
    }

    for (const auto& [key, value] : root_json.items()) {
      remap_list_.emplace_back(key, value);
    }
  } catch (nlohmann::json::exception& e) {
    error_string_ = e.what();

    VLOG_W("UrlRemap: Parse error: ", error_string_, ".");

    remap_list_.clear();
    cache_map_.clear();

    is_valid_ = false;
    return false;
  }

  error_string_.clear();

  is_valid_ = true;

  return true;
}

bool UrlRemap::unload() noexcept {
  if VUNLIKELY (!is_valid_) {
    return false;
  }

  remap_list_.clear();
  cache_map_.clear();

  error_string_.clear();

  is_valid_ = false;

  return true;
}

bool UrlRemap::reload(const std::string& file_path) noexcept {
  unload();

  return load(file_path);
}

const std::string& UrlRemap::convert(const std::string& url) noexcept {
  if VUNLIKELY (!is_valid_) {
    // NOLINTNEXTLINE(bugprone-return-const-ref-from-parameter)
    return url;
  }

  auto iter = cache_map_.find(url);

  if VLIKELY (iter != cache_map_.end()) {
    return iter->second;
  }

  for (const auto& [key, value] : remap_list_) {
    if (url != value && url.find(key) != std::string::npos) {
      if (is_enable_log_) {
        VLOG_I("UrlRemap: ", url, " -> ", value, ".");
      }

      cache_map_.try_emplace(url, value);
      return value;
    }
  }

  cache_map_.try_emplace(url, url);

  // NOLINTNEXTLINE(bugprone-return-const-ref-from-parameter)
  return url;
}

}  // namespace vlink
