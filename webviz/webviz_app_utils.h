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

#pragma once

#include <vlink/base/helpers.h>
#include <vlink/base/macros.h>
#include <vlink/base/utils.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "webviz_types.h"

namespace vlink {
namespace webviz {

inline std::string resolve_relative_path(const std::filesystem::path& config_dir, const std::string& path) {
  if VUNLIKELY (path.empty()) {
    return {};
  }

  if VUNLIKELY (std::filesystem::path(path).is_absolute()) {
    return path;
  }

  return Helpers::path_to_string(config_dir / std::filesystem::path(path));
}

inline bool append_config_paths(const nlohmann::json& root, const char* key, const std::filesystem::path& config_dir,
                                std::vector<std::string>& out) {
  if VUNLIKELY (!root.contains(key) || !root[key].is_array()) {
    return !root.contains(key);
  }

  for (const auto& item : root[key]) {
    if VUNLIKELY (!item.is_string()) {
      return false;
    }

    auto path = item.get<std::string>();

    if VLIKELY (!path.empty()) {
      path = resolve_relative_path(config_dir, path);
    }

    out.emplace_back(std::move(path));
  }

  return true;
}

inline std::string normalize_token(std::string_view value) {
  std::string normalized(value);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return normalized;
}

inline void append_filter_patterns(std::vector<std::string>& out, std::string_view value) {
  if VUNLIKELY (value.empty()) {
    return;
  }

  for (const auto item : Helpers::split_any_view(value)) {
    auto normalized = normalize_token(item);

    if VUNLIKELY (normalized.empty()) {
      continue;
    }

    out.emplace_back(std::move(normalized));
  }
}

inline void normalize_filter_patterns(std::vector<std::string>& filters) {
  std::unordered_set<std::string> seen;
  std::vector<std::string> normalized;
  normalized.reserve(filters.size());

  for (const auto& item : filters) {
    std::vector<std::string> split_tokens;
    append_filter_patterns(split_tokens, item);

    for (auto& token : split_tokens) {
      if VLIKELY (seen.insert(token).second) {
        normalized.emplace_back(std::move(token));
      }
    }
  }

  filters = std::move(normalized);
}

inline void normalize_exact_filters(std::vector<std::string>& filters) {
  std::unordered_set<std::string> seen;
  std::vector<std::string> normalized;
  normalized.reserve(filters.size());

  for (auto& item : filters) {
    if VUNLIKELY (item.empty()) {
      continue;
    }

    if VLIKELY (seen.insert(item).second) {
      normalized.emplace_back(std::move(item));
    }
  }

  filters = std::move(normalized);
}

inline bool append_json_filter_value(const nlohmann::json& root, const char* key, std::vector<std::string>& exact_out,
                                     std::vector<std::string>& pattern_out) {
  if VUNLIKELY (!root.contains(key)) {
    return true;
  }

  const auto& value = root[key];

  if VLIKELY (value.is_string()) {
    append_filter_patterns(pattern_out, value.get<std::string>());
    return true;
  }

  if VUNLIKELY (!value.is_array()) {
    return false;
  }

  for (const auto& item : value) {
    if VUNLIKELY (!item.is_string()) {
      return false;
    }

    exact_out.emplace_back(item.get<std::string>());
  }

  return true;
}

inline bool match_filter_patterns(std::string_view value, const std::vector<std::string>& filters) {
  if VUNLIKELY (filters.empty()) {
    return false;
  }

  struct NormalizeCache final {
    std::string value;
    std::string normalized;
  };

  thread_local NormalizeCache cache;
  const std::string* normalized = nullptr;

  if VLIKELY (cache.value == value) {
    normalized = &cache.normalized;
  } else {
    cache.value.assign(value.data(), value.size());
    cache.normalized = normalize_token(value);
    normalized = &cache.normalized;
  }

  for (const auto& filter : filters) {
    if VUNLIKELY (filter.empty()) {
      continue;
    }

    if VLIKELY (normalized->find(filter) != std::string::npos) {
      return true;
    }
  }

  return false;
}

inline bool match_exact_filters(std::string_view value, const std::vector<std::string>& filters) {
  if VUNLIKELY (filters.empty()) {
    return false;
  }

  for (const auto& filter : filters) {
    if VUNLIKELY (filter.empty()) {
      continue;
    }

    if VLIKELY (value == filter) {
      return true;
    }
  }

  return false;
}

inline bool matches_any_filter(std::string_view value, const std::vector<std::string>& exact_filters,
                               const std::vector<std::string>& pattern_filters) {
  return match_exact_filters(value, exact_filters) || match_filter_patterns(value, pattern_filters);
}

inline int score_filter_group(std::string_view value, const std::vector<std::string>& exact_filters,
                              const std::vector<std::string>& pattern_filters) {
  if VLIKELY (match_exact_filters(value, exact_filters)) {
    return 3;
  }

  if VLIKELY (match_filter_patterns(value, pattern_filters)) {
    return 2;
  }

  if VUNLIKELY (exact_filters.empty() && pattern_filters.empty()) {
    return 1;
  }

  return -1;
}

inline bool is_allowed_by_filters(std::string_view value, const std::vector<std::string>& whitelist_exact,
                                  const std::vector<std::string>& whitelist_patterns,
                                  const std::vector<std::string>& blacklist_exact,
                                  const std::vector<std::string>& blacklist_patterns) {
  if VUNLIKELY (whitelist_exact.empty() && whitelist_patterns.empty() && blacklist_exact.empty() &&
                blacklist_patterns.empty()) {
    return true;
  }

  if VLIKELY ((!whitelist_exact.empty() || !whitelist_patterns.empty()) &&
              !matches_any_filter(value, whitelist_exact, whitelist_patterns)) {
    return false;
  }

  return !matches_any_filter(value, blacklist_exact, blacklist_patterns);
}

template <typename Owner>
inline bool is_allowed_by_filters_cached(const Owner* owner, std::string_view value,
                                         const std::vector<std::string>& whitelist_exact,
                                         const std::vector<std::string>& whitelist_patterns,
                                         const std::vector<std::string>& blacklist_exact,
                                         const std::vector<std::string>& blacklist_patterns) {
  struct FilterCacheEntry final {
    const Owner* owner{nullptr};
    size_t hash{0};
    std::string value;
    bool allowed{false};
  };

  thread_local std::array<FilterCacheEntry, 8> cache_entries;
  const auto hash = std::hash<std::string_view>{}(value);
  auto& cache_entry = cache_entries[hash % cache_entries.size()];

  if VLIKELY (cache_entry.owner == owner && cache_entry.hash == hash && cache_entry.value == value) {
    return cache_entry.allowed;
  }

  cache_entry.owner = owner;
  cache_entry.hash = hash;
  cache_entry.value.assign(value.data(), value.size());
  cache_entry.allowed = is_allowed_by_filters(cache_entry.value, whitelist_exact, whitelist_patterns, blacklist_exact,
                                              blacklist_patterns);
  return cache_entry.allowed;
}

inline void normalize_url_selector(UrlSelector& selector) {
  normalize_exact_filters(selector.whitelist_exact);
  normalize_filter_patterns(selector.whitelist_patterns);
  normalize_exact_filters(selector.blacklist_exact);
  normalize_filter_patterns(selector.blacklist_patterns);
}

inline bool is_static_url_selector(const UrlSelector& selector) {
  return selector.configured && selector.whitelist_patterns.empty() && selector.blacklist_exact.empty() &&
         selector.blacklist_patterns.empty() && !selector.whitelist_exact.empty();
}

inline bool matches_url_selector(std::string_view url, const UrlSelector& selector) {
  if VUNLIKELY (!selector.configured) {
    return true;
  }

  return is_allowed_by_filters(url, selector.whitelist_exact, selector.whitelist_patterns, selector.blacklist_exact,
                               selector.blacklist_patterns);
}

inline int score_url_selector(std::string_view url, const UrlSelector& selector) {
  if VUNLIKELY (!selector.configured) {
    return 0;
  }

  const auto whitelist_score = score_filter_group(url, selector.whitelist_exact, selector.whitelist_patterns);

  if VUNLIKELY (whitelist_score < 0) {
    return -1;
  }

  if VUNLIKELY (matches_any_filter(url, selector.blacklist_exact, selector.blacklist_patterns)) {
    return -1;
  }

  return whitelist_score;
}

inline std::string resolve_arg_or_env(const std::string& value, const char* env_name) {
  if VLIKELY (!value.empty()) {
    return value;
  }

  return Utils::get_env(env_name);
}

inline bool validate_port(int port, std::string_view option_name, std::string& error) {
  if VUNLIKELY (port <= 0 || port > 65535) {
    error = std::string(option_name) + " must be in [1, 65535]";
    return false;
  }

  return true;
}

inline bool ensure_parent_directory(const std::string& file_path) {
  auto parent_path = std::filesystem::path(file_path).parent_path();

  if VUNLIKELY (parent_path.empty()) {
    return true;
  }

  std::error_code ec;
  std::filesystem::create_directories(parent_path, ec);
  return !ec;
}

#ifdef _WIN32
inline void normalize_path(std::string& path) {
  if VUNLIKELY (path.empty()) {
    return;
  }

  path = Helpers::path_to_string(std::filesystem::path(path));
  std::replace(path.begin(), path.end(), '\\', '/');

  if VUNLIKELY (!path.empty() && path.back() == '/') {
    path.pop_back();
  }
}

inline void normalize_paths(std::vector<std::string>& paths) {
  for (auto& path : paths) {
    normalize_path(path);
  }
}
#endif

}  // namespace webviz
}  // namespace vlink
