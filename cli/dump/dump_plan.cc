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

#include "dump_plan.h"

#include <vlink/base/helpers.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>
#include <limits>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace vlink::dump {

std::string trim_copy(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.erase(value.begin());
  }

  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.pop_back();
  }

  return value;
}

std::string to_lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
  return value;
}

uint32_t stable_hash_32(std::string_view value) {
  uint32_t hash = 2166136261U;

  for (unsigned char ch : value) {
    hash ^= ch;
    hash *= 16777619U;
  }

  return hash;
}

std::string hex8(uint32_t value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(8, '0');

  for (int i = 7; i >= 0; --i) {
    out[static_cast<size_t>(i)] = kHex[value & 0x0FU];
    value >>= 4U;
  }

  return out;
}

std::string sanitize_file_component(const std::string& raw, const std::string& fallback) {
  auto trimmed = trim_copy(raw);
  std::string result;
  result.reserve(trimmed.size());

  bool last_was_underscore = false;
  bool last_was_dot = false;

  for (unsigned char ch : trimmed) {
    if (std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.') {
      if (ch == '.' && last_was_dot) {
        continue;
      }

      result.push_back(static_cast<char>(ch));
      last_was_underscore = false;
      last_was_dot = ch == '.';
    } else {
      if (!last_was_underscore) {
        result.push_back('_');
        last_was_underscore = true;
      }

      last_was_dot = false;
    }
  }

  result = trim_copy(result);

  while (!result.empty() && (result.front() == '.' || result.front() == '_')) {
    result.erase(result.begin());
  }

  while (!result.empty() && (result.back() == '.' || result.back() == '_')) {
    result.pop_back();
  }

  if (result.empty() || result == "." || result == "..") {
    return fallback;
  }

  static constexpr size_t kMaxFileComponent = 120;

  if (result.size() > kMaxFileComponent) {
    auto suffix = "_" + hex8(stable_hash_32(trimmed));
    result.resize(kMaxFileComponent - suffix.size());

    while (!result.empty() && (result.back() == '.' || result.back() == '_' || result.back() == '-')) {
      result.pop_back();
    }

    if (result.empty()) {
      return fallback;
    }

    result += suffix;
  }

  return result;
}

bool is_supported_bag_suffix(std::string suffix) {
  suffix = to_lower_copy(std::move(suffix));
  return suffix == ".vdb" || suffix == ".vdbx" || suffix == ".vcap" || suffix == ".vcapx";
}

std::string sanitize_suffix(const std::string& raw_suffix) {
  auto suffix = sanitize_file_component(raw_suffix, "");

  while (!suffix.empty() && suffix.front() == '_') {
    suffix.erase(suffix.begin());
  }

  if (suffix.empty() || suffix == ".") {
    return {};
  }

  if (suffix.front() != '.') {
    suffix.insert(suffix.begin(), '.');
  }

  suffix = to_lower_copy(std::move(suffix));

  if (!is_supported_bag_suffix(suffix)) {
    return {};
  }

  return suffix;
}

bool validate_duration_seconds(std::string_view option, double seconds, bool allow_zero) {
  if (!std::isfinite(seconds) || seconds < 0) {
    std::cerr << option << " must be non-negative.\n";
    return false;
  }

  if (!allow_zero && seconds <= 0) {
    std::cerr << option << " must be greater than 0.\n";
    return false;
  }

  if (seconds > 0 && seconds * 1000.0 < 1.0) {
    std::cerr << option << " must be at least 0.001 seconds; dump time precision is 1 ms.\n";
    return false;
  }

  static constexpr auto kMaxDurationSeconds = static_cast<double>(std::numeric_limits<int64_t>::max()) / 1000.0;

  if (seconds >= kMaxDurationSeconds) {
    std::cerr << option << " is too large.\n";
    return false;
  }

  return true;
}

bool validate_cache_size_mb(std::string_view option, double cache_size) {
  static constexpr double kMaxCacheSizeMb = 4096.0;

  if (!std::isfinite(cache_size) || cache_size <= 0) {
    std::cerr << option << " must be greater than 0.\n";
    return false;
  }

  if (cache_size > kMaxCacheSizeMb) {
    std::cerr << option << " must not exceed " << kMaxCacheSizeMb << " MB.\n";
    return false;
  }

  return true;
}

std::string csv_name_for_slice_file(std::string file_name) {
  auto dot_pos = file_name.rfind('.');

  if (dot_pos != std::string::npos) {
    file_name = file_name.substr(0, dot_pos);
  }

  file_name += ".csv";
  return file_name;
}

bool validate_common_slice_options(const SliceOptions& opt) {
  if (!validate_duration_seconds("--window", opt.window_seconds, true)) {
    return false;
  }

  if (!validate_duration_seconds("--pre", opt.event_pre, true) ||
      !validate_duration_seconds("--post", opt.event_post, true)) {
    return false;
  }

  if (!validate_duration_seconds("--event_state_max_age", opt.event_state_max_age, true)) {
    return false;
  }

  if (!validate_duration_seconds("--event_min_interval", opt.event_min_interval, true)) {
    return false;
  }

  if (!validate_duration_seconds("--dropout_threshold", opt.dropout_threshold, false)) {
    return false;
  }

  if (!validate_cache_size_mb("--cache_size", opt.cache_size)) {
    return false;
  }

  if (opt.compress_level < 0 || opt.compress_level > 5) {
    std::cerr << "--compress_level must be in range [0, 5].\n";
    return false;
  }

  if (opt.sample_step < 1) {
    std::cerr << "--sample_step must be greater than 0.\n";
    return false;
  }

  if (opt.actions.empty()) {
    std::cerr << "--actions must contain at least one action type.\n";
    return false;
  }

  for (auto action : opt.actions) {
    if (action < static_cast<int>(vlink::ActionType::kUnknownAction) ||
        action > static_cast<int>(vlink::ActionType::kGet)) {
      std::cerr << "Invalid action type: " << action << '\n';
      return false;
    }
  }

  if (!opt.suffix.empty() && sanitize_suffix(opt.suffix).empty()) {
    std::cerr << "--suffix must be one of .vdb, .vdbx, .vcap, or .vcapx.\n";
    return false;
  }

  return true;
}

bool build_url_selection(const vlink::BagReader::Info& info, const std::vector<std::string>& urls,
                         const std::string& url_filter, bool black_mode, const std::string& target_url,
                         UrlSelection& selection, bool fail_on_empty) {
  selection.urls.clear();
  selection.all = false;

  auto split_keywords = [](const std::string& raw) {
    std::vector<std::string> keywords;

    for (auto& item : vlink::Helpers::split_any(raw)) {
      keywords.emplace_back(to_lower_copy(std::move(item)));
    }

    return keywords;
  };

  auto url_matches = [](const std::string& url, const std::vector<std::string>& keywords) {
    auto lower_url = to_lower_copy(url);

    return std::any_of(keywords.begin(), keywords.end(),
                       [&](const std::string& keyword) { return lower_url.find(keyword) != std::string::npos; });
  };

  auto trimmed_filter = trim_copy(url_filter);
  auto trimmed_target = trim_copy(target_url);

  if (!trimmed_filter.empty()) {
    auto keywords = split_keywords(trimmed_filter);

    for (const auto& meta : info.url_metas) {
      if (meta.url_type == "Method") {
        continue;
      }

      bool match = url_matches(meta.url, keywords);
      bool keep = black_mode ? !match : match;

      if (keep) {
        selection.urls.emplace(meta.url);
      }
    }
  } else if (!urls.empty()) {
    std::vector<std::string> clean_urls;
    clean_urls.reserve(urls.size());
    std::unordered_set<std::string> matched_urls;

    for (const auto& raw_url : urls) {
      auto clean_url = trim_copy(raw_url);

      if (!clean_url.empty()) {
        clean_urls.emplace_back(std::move(clean_url));
      }
    }

    if (clean_urls.empty()) {
      if (fail_on_empty) {
        std::cerr << "Option --urls requires at least one non-empty URL.\n";
        return false;
      }

      return true;
    }

    for (const auto& meta : info.url_metas) {
      if (meta.url_type == "Method") {
        continue;
      }

      auto iter = std::find(clean_urls.begin(), clean_urls.end(), meta.url);
      bool keep = black_mode ? iter == clean_urls.end() : iter != clean_urls.end();

      if (iter != clean_urls.end()) {
        matched_urls.emplace(meta.url);
      }

      if (keep) {
        selection.urls.emplace(meta.url);
      }
    }

    if (fail_on_empty) {
      for (const auto& clean_url : clean_urls) {
        if (matched_urls.count(clean_url) == 0) {
          std::cerr << "Selected URL not found in bag metadata: " << clean_url << '\n';
          return false;
        }
      }
    }
  } else if (!trimmed_target.empty() && trimmed_target != "*") {
    for (const auto& meta : info.url_metas) {
      if (meta.url == trimmed_target && meta.url_type != "Method") {
        selection.urls.emplace(meta.url);
        break;
      }
    }
  } else {
    selection.all = true;
  }

  if (fail_on_empty && !selection.all && selection.urls.empty()) {
    std::cerr << "No URL matched the selected target/filter.\n";
    return false;
  }

  return true;
}

bool normalize_segment_plan(std::vector<SegmentDef>& segments, int64_t effective_begin, int64_t effective_end,
                            const std::string& source_name) {
  std::vector<SegmentDef> normalized;
  normalized.reserve(segments.size());

  for (const auto& segment : segments) {
    auto seg = segment;
    auto original_begin_ms = seg.begin_ms;
    auto original_end_ms = seg.end_ms;
    seg.name = trim_copy(seg.name);

    if (seg.end_ms <= seg.begin_ms) {
      std::cerr << "Invalid segment time range";

      if (!seg.name.empty()) {
        std::cerr << " for " << seg.name;
      }

      std::cerr << " in " << source_name << ": [" << seg.begin_ms << ", " << seg.end_ms << ")\n";
      return false;
    }

    seg.begin_ms = std::max(seg.begin_ms, effective_begin);
    seg.end_ms = std::min(seg.end_ms, effective_end);

    if (seg.end_ms <= seg.begin_ms) {
      std::cerr << "Segment is outside requested time range";

      if (!seg.name.empty()) {
        std::cerr << " for " << seg.name;
      }

      std::cerr << " in " << source_name << ": requested [" << original_begin_ms << ", " << original_end_ms
                << "), effective [" << effective_begin << ", " << effective_end << ")\n";
      return false;
    }

    normalized.emplace_back(std::move(seg));
  }

  if (normalized.empty()) {
    std::cerr << "No valid segments in requested time range";

    if (!source_name.empty()) {
      std::cerr << ": " << source_name;
    }

    std::cerr << '\n';
    return false;
  }

  std::sort(normalized.begin(), normalized.end(), [](const SegmentDef& a, const SegmentDef& b) {
    if (a.begin_ms != b.begin_ms) {
      return a.begin_ms < b.begin_ms;
    }

    if (a.end_ms != b.end_ms) {
      return a.end_ms < b.end_ms;
    }

    return a.name < b.name;
  });

  for (size_t i = 1; i < normalized.size(); ++i) {
    if (normalized[i].begin_ms < normalized[i - 1].end_ms) {
      std::cerr << "Overlapping segments are not supported in single-pass slice mode: " << normalized[i - 1].name
                << " [" << normalized[i - 1].begin_ms << ", " << normalized[i - 1].end_ms << ") overlaps "
                << normalized[i].name << " [" << normalized[i].begin_ms << ", " << normalized[i].end_ms << ")\n";
      return false;
    }
  }

  std::unordered_map<std::string, int> suffix_counters;
  std::unordered_set<std::string> used_names;

  for (size_t i = 0; i < normalized.size(); ++i) {
    auto base_name = sanitize_file_component(normalized[i].name, "seg_" + std::to_string(i));
    auto candidate = base_name;
    auto& counter = suffix_counters[base_name];

    while (used_names.count(candidate) != 0) {
      ++counter;
      candidate = base_name + "_" + std::to_string(counter + 1);
    }

    used_names.emplace(candidate);
    normalized[i].name = std::move(candidate);
  }

  segments = std::move(normalized);
  return true;
}

bool preflight_output_files(const std::filesystem::path& out_dir, const std::vector<std::string>& file_names,
                            bool force) {
  std::unordered_set<std::string> planned_paths;

  for (const auto& file_name : file_names) {
    auto file_path = std::filesystem::path(file_name);

    if (file_name.empty() || file_path.empty() || file_path.is_absolute() || file_path.has_parent_path()) {
      std::cerr << "Unsafe output file name: " << file_name << '\n';
      return false;
    }

    auto path = out_dir / file_name;
    auto normalized = path.lexically_normal().string();

    if (planned_paths.count(normalized) != 0) {
      std::cerr << "Duplicate output path: " << normalized << '\n';
      return false;
    }

    planned_paths.emplace(normalized);

    std::error_code status_error;
    auto status = std::filesystem::symlink_status(path, status_error);

    if (status_error) {
      if (status_error.default_error_condition() == std::errc::no_such_file_or_directory) {
        continue;
      }

      std::cerr << "Failed to inspect output path: " << path.string() << " (" << status_error.message() << ")\n";
      return false;
    }

    if (std::filesystem::exists(status)) {
      if (std::filesystem::is_symlink(status)) {
        std::cerr << "Refusing to overwrite symlink: " << path.string() << '\n';
        return false;
      }

      if (std::filesystem::is_directory(status)) {
        std::cerr << "Output path is a directory: " << path.string() << '\n';
        return false;
      }

      if (!std::filesystem::is_regular_file(status)) {
        std::cerr << "Output path is not a regular file: " << path.string() << '\n';
        return false;
      }

      if (!force) {
        std::cerr << "File already exists: " << path.string() << " (use --force to overwrite)\n";
        return false;
      }
    }
  }

  return true;
}

}  // namespace vlink::dump
