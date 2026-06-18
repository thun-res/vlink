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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "./report_internal.h"

namespace vlink::bench::report {

std::string get_transport_from_url(const std::string& url);
std::string format_size_label(size_t value);
std::string format_decimal(double value, int precision = 2);
std::string format_metric_cell(const MetricSummary& metric, int precision = 2);
std::string format_memory_cell(const MetricSummary& metric);
std::string format_loss_ratio_cell(const AggregatedCase& item, int precision = 2);
std::string format_loss_detail(const AggregatedCase& item);
std::string format_rate_label(const Bench::Scenario& scenario);
std::string make_topology_label(const Bench::Scenario& scenario);
std::string make_case_status_brief(const AggregatedCase& item);
std::string make_case_status_text(const AggregatedCase& item);
std::string join_strings(const std::vector<std::string>& values, std::string_view separator);
std::string strip_ansi_escape_codes(const std::string& input);
std::string escape_html(const std::string& input);
std::string quote_csv(const std::string& input);

double compute_loss_ratio_percent(const AggregatedCase& item);
double compute_latency_drop_ratio_percent(const AggregatedCase& item);
bool should_report_delivery_loss(double loss_percent);
bool has_delivery_loss(const AggregatedCase& item);
bool has_runtime_success(const AggregatedCase& item);
bool is_serialization_case(const AggregatedCase& item);

double score_latency_us(const AggregatedCase& item);
double score_latency_quality(const AggregatedCase& item);

bool ensure_parent_dir(const std::string& file_path, std::string& error);
bool write_text_file_atomic(const std::string& file_path, std::string_view content, std::string& error);

int display_width(std::string_view text);
std::string ascii_tolower(std::string text);
void decode_utf8(std::string_view text, size_t index, uint32_t& code_point, size_t& bytes);
int codepoint_width(uint32_t cp);

template <typename ContainerT>
void append_unique(ContainerT& container, const std::string& value) {
  if (value.empty()) {
    return;
  }

  if (std::find(container.begin(), container.end(), value) == container.end()) {
    container.emplace_back(value);
  }
}

}  // namespace vlink::bench::report
