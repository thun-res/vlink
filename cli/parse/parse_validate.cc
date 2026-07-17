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

#include "./parse_validate.h"

#include <vlink/base/helpers.h>

#include <array>
#include <iostream>
#include <vector>

namespace vlink::parse {

const char* parse_type_to_string(ParseType type) {
  switch (type) {
    case ParseType::kConsole:
      return "console";
    case ParseType::kCsv:
      return "csv";
    case ParseType::kJson:
      return "json";
    case ParseType::kBin:
      return "bin";
    case ParseType::kJpg:
      return "jpg";
    case ParseType::kH264:
      return "h264";
    case ParseType::kH265:
      return "h265";
    case ParseType::kRaw:
      return "raw";
    case ParseType::kPcd:
      return "pcd";
    case ParseType::kSlice:
      return "slice";
    case ParseType::kScan:
      return "scan";
  }

  return "unknown";
}

bool is_parse_export_type(ParseType type) {
  switch (type) {
    case ParseType::kConsole:
    case ParseType::kCsv:
    case ParseType::kJson:
    case ParseType::kBin:
    case ParseType::kJpg:
    case ParseType::kH264:
    case ParseType::kH265:
    case ParseType::kRaw:
    case ParseType::kPcd:
      return true;
    case ParseType::kSlice:
    case ParseType::kScan:
      return false;
  }

  return false;
}

bool option_used(const argparse::ArgumentParser& program, std::string_view short_option, std::string_view long_option) {
  return program.is_used(short_option) || (!long_option.empty() && program.is_used(long_option));
}

struct ModeRule final {
  std::string_view option;
  std::string_view valid_modes_hint;
  bool valid_in_parse_export;
  bool valid_in_slice;
  bool valid_in_scan;
};

static constexpr std::array<ModeRule, 34> kModeRules{{
    {"-m", "parse/export modes", true, false, false},
    {"-n", "parse/export modes", true, false, false},
    {"--hz", "parse/export modes", true, false, false},
    {"-x", "parse/export modes", true, false, false},
    {"-l", "parse/export modes or -t slice", true, true, false},
    {"-w", "-t slice", false, true, false},
    {"--segments", "-t slice", false, true, false},
    {"--suffix", "-t slice", false, true, false},
    {"--compress", "-t slice", false, true, false},
    {"--no_manifest", "-t slice", false, true, false},
    {"--manifest", "-t slice", false, true, false},
    {"--filter", "-t slice", false, true, false},
    {"--export_csv", "-t slice", false, true, false},
    {"--tag", "-t slice", false, true, false},
    {"--wal_mode", "-t slice", false, true, false},
    {"--cache_size", "-t slice", false, true, false},
    {"--compress_level", "-t slice", false, true, false},
    {"--ignore_compress", "-t slice", false, true, false},
    {"--sample_step", "-t slice", false, true, false},
    {"--dry_run", "-t slice", false, true, false},
    {"--scan_output", "-t scan", false, false, true},
    {"--quality_check", "-t scan", false, false, true},
    {"--dropout_threshold", "-t scan", false, false, true},
    {"--event", "-t slice or -t scan", false, true, true},
    {"--pre", "-t slice or -t scan", false, true, true},
    {"--post", "-t slice or -t scan", false, true, true},
    {"--event_state_max_age", "-t slice or -t scan", false, true, true},
    {"--event_min_interval", "-t slice or -t scan", false, true, true},
    {"--force", "-t slice or -t scan", false, true, true},
    {"--schema_config", "-t slice or -t scan", false, true, true},
    {"--urls", "-t slice or -t scan", false, true, true},
    {"--url_filter", "-t slice or -t scan", false, true, true},
    {"--black", "-t slice or -t scan", false, true, true},
    {"--actions", "-t slice or -t scan", false, true, true},
}};

static bool mode_allows_rule(const ModeRule& rule, ParseType type) {
  if (is_parse_export_type(type)) {
    return rule.valid_in_parse_export;
  }

  if (type == ParseType::kSlice) {
    return rule.valid_in_slice;
  }

  if (type == ParseType::kScan) {
    return rule.valid_in_scan;
  }

  return false;
}

static bool check_table(const argparse::ArgumentParser& program, ParseType type) {
  for (const auto& rule : kModeRules) {
    if (!program.is_used(rule.option)) {
      continue;
    }

    if (mode_allows_rule(rule, type)) {
      continue;
    }

    std::cerr << "Option " << rule.option << " is only valid with " << rule.valid_modes_hint << " (current: -t "
              << parse_type_to_string(type) << ")." << std::endl;
    return false;
  }

  return true;
}

static bool check_url_arguments(const argparse::ArgumentParser& program, ParseType type) {
  if (type != ParseType::kSlice && type != ParseType::kScan) {
    return true;
  }

  if (program.is_used("--url_filter") &&
      vlink::Helpers::trim_string_view(program.get<std::string>("--url_filter")).empty()) {
    std::cerr << "Option --url_filter requires at least one non-space keyword." << std::endl;
    return false;
  }

  if (program.is_used("--urls") && program.get<std::vector<std::string>>("--urls").empty()) {
    std::cerr << "Option --urls requires at least one URL." << std::endl;
    return false;
  }

  return true;
}

static bool check_event_dependencies(const argparse::ArgumentParser& program, ParseType type) {
  if (type != ParseType::kSlice && type != ParseType::kScan) {
    return true;
  }

  if (program.is_used("--event") && vlink::Helpers::trim_string_view(program.get<std::string>("--event")).empty()) {
    std::cerr << "Option --event requires a non-space expression." << std::endl;
    return false;
  }

  if (program.is_used("--event") && !option_used(program, "-c", "--condition")) {
    std::cerr << "Option --event requires -c/--condition to bind expression variables." << std::endl;
    return false;
  }

  if (!program.is_used("--event")) {
    for (const auto* option : {"--pre", "--post", "--event_state_max_age", "--event_min_interval"}) {
      if (program.is_used(option)) {
        std::cerr << "Option " << option << " is only valid with --event (current: -t " << parse_type_to_string(type)
                  << ")." << std::endl;
        return false;
      }
    }
  }

  return true;
}

static bool check_parse_export_only(const argparse::ArgumentParser& program, ParseType type, bool has_bag_input,
                                    bool url_argument_used, const std::string& target_url) {
  if (is_parse_export_type(type) && (!url_argument_used || target_url == "*")) {
    std::cerr << "A target URL is required for -t " << parse_type_to_string(type)
              << ". Use '*' only with slice/scan when selecting all topics." << std::endl;
    return false;
  }

  if (program.is_used("--native") && !is_parse_export_type(type)) {
    std::cerr << "Option --native is only valid for live parse/export modes." << std::endl;
    return false;
  }

  if (program.is_used("--native") && has_bag_input) {
    std::cerr << "Option --native is only valid without -f/--bag_file." << std::endl;
    return false;
  }

  if (program.is_used("--plugin") && !has_bag_input) {
    std::cerr << "Option --plugin requires -f/--bag_file." << std::endl;
    return false;
  }

  if (!has_bag_input && option_used(program, "-b", "--begin_time")) {
    std::cerr << "Option -b/--begin_time is only valid with bag input." << std::endl;
    return false;
  }

  if (!has_bag_input && option_used(program, "-e", "--end_time")) {
    std::cerr << "Option -e/--end_time is only valid with bag input." << std::endl;
    return false;
  }

  if ((type == ParseType::kSlice || type == ParseType::kScan) && url_argument_used && target_url != "*" &&
      (program.is_used("--urls") || program.is_used("--url_filter"))) {
    std::cerr << "Use either positional url '" << target_url
              << "' or --urls/--url_filter, not both. This avoids silently changing the selected topics." << std::endl;
    return false;
  }

  return true;
}

static bool check_quality_constraints(const argparse::ArgumentParser& program, ParseType type) {
  if (program.is_used("--dropout_threshold") && !program.is_used("--quality_check")) {
    std::cerr << "Option --dropout_threshold requires --quality_check." << std::endl;
    return false;
  }

  if (type == ParseType::kScan && !program.is_used("--event") && !program.is_used("--quality_check")) {
    std::cerr << "Scan mode requires --event or --quality_check." << std::endl;
    return false;
  }

  if (type == ParseType::kScan && program.is_used("--quality_check") && !program.is_used("--event") &&
      option_used(program, "-c", "--condition")) {
    std::cerr << "Option -c/--condition is only valid with --event in scan mode." << std::endl;
    return false;
  }

  return true;
}

static bool check_slice_segment_sources(const argparse::ArgumentParser& program, ParseType type) {
  if (type != ParseType::kSlice) {
    return true;
  }

  if (program.is_used("--filter") && vlink::Helpers::trim_string_view(program.get<std::string>("--filter")).empty()) {
    std::cerr << "Option --filter requires a non-space expression." << std::endl;
    return false;
  }

  int segment_plan_count = 0;
  segment_plan_count += option_used(program, "-w", "--window") ? 1 : 0;
  segment_plan_count += program.is_used("--segments") ? 1 : 0;
  segment_plan_count += program.is_used("--event") ? 1 : 0;

  if (segment_plan_count > 1) {
    std::cerr << "Slice mode accepts only one segment source: --window, --segments, or --event." << std::endl;
    return false;
  }

  if (program.is_used("--export_csv") && !option_used(program, "-c", "--condition")) {
    std::cerr << "Option --export_csv requires -c/--condition." << std::endl;
    return false;
  }

  return true;
}

bool validate_mode_options(const argparse::ArgumentParser& program, ParseType type, bool has_bag_input,
                           bool url_argument_used, const std::string& target_url) {
  if (!check_parse_export_only(program, type, has_bag_input, url_argument_used, target_url)) {
    return false;
  }

  if (!check_table(program, type)) {
    return false;
  }

  if (!check_url_arguments(program, type)) {
    return false;
  }

  if (!check_event_dependencies(program, type)) {
    return false;
  }

  if (!check_quality_constraints(program, type)) {
    return false;
  }

  if (!check_slice_segment_sources(program, type)) {
    return false;
  }

  return true;
}

}  // namespace vlink::parse
