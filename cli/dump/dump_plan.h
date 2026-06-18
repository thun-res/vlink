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

#include <vlink/extension/bag_reader.h>
#include <vlink/impl/types.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace vlink::dump {

struct SliceOptions final {
  double window_seconds{0};
  double cache_size{4.0};
  double event_pre{5.0};
  double event_post{3.0};
  double event_state_max_age{0.5};
  double event_min_interval{0.0};
  double dropout_threshold{1.0};
  std::vector<std::string> urls;
  std::vector<int> actions{6};
  std::vector<std::string> ignore_compress;
  int64_t begin_time{0};
  int64_t end_time{0};
  std::string bag_file;
  std::string target_url;
  std::string out_dir;
  std::string suffix;
  std::string manifest_name{"manifest.json"};
  std::string scan_output_name{"events.json"};
  std::string proto_dir;
  std::string fbs_dir;
  std::string schema_config_path;
  std::string filter_expr;
  std::string tag_name;
  std::string url_filter;
  std::string segments_file;
  std::string event_expr;
  int compress_level{3};
  int sample_step{1};
  bool compress{false};
  bool force{false};
  bool no_manifest{false};
  bool export_csv{false};
  bool black_mode{false};
  bool wal_mode{false};
  bool scan_only{false};
  bool dry_run{false};
  bool quality_check{false};
  bool quality_only{false};
  bool begin_time_set{false};
  bool end_time_set{false};
};

struct SegmentDef final {
  std::string name;
  int64_t begin_ms{0};
  int64_t end_ms{0};
};

struct UrlSelection final {
  std::unordered_set<std::string> urls;
  bool all{false};
};

struct EventVarState final {
  double value{0.0};
  int64_t timestamp_ms{0};
};

std::string trim_copy(std::string value);

std::string to_lower_copy(std::string value);

uint32_t stable_hash_32(std::string_view value);

std::string hex8(uint32_t value);

std::string sanitize_file_component(const std::string& raw, const std::string& fallback);

bool is_supported_bag_suffix(std::string suffix);

std::string sanitize_suffix(const std::string& raw_suffix);

bool validate_duration_seconds(std::string_view option, double seconds, bool allow_zero);

bool validate_cache_size_mb(std::string_view option, double cache_size);

inline int64_t seconds_to_milliseconds(double seconds) { return static_cast<int64_t>(seconds * 1000.0); }

inline int64_t cache_size_to_bytes(double cache_size_mb) {
  return static_cast<int64_t>(cache_size_mb * 1024.0 * 1024.0);
}

inline bool action_selected(const std::vector<int>& actions, vlink::ActionType action_type) {
  return std::find(actions.begin(), actions.end(), static_cast<int>(action_type)) != actions.end();
}

std::string csv_name_for_slice_file(std::string file_name);

bool validate_common_slice_options(const SliceOptions& opt);

bool build_url_selection(const vlink::BagReader::Info& info, const std::vector<std::string>& urls,
                         const std::string& url_filter, bool black_mode, const std::string& target_url,
                         UrlSelection& selection, bool fail_on_empty = true);

bool normalize_segment_plan(std::vector<SegmentDef>& segments, int64_t effective_begin, int64_t effective_end,
                            const std::string& source_name);

bool preflight_output_files(const std::filesystem::path& out_dir, const std::vector<std::string>& file_names,
                            bool force);

}  // namespace vlink::dump
