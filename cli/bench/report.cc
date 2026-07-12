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

#include "./report.h"

#include <vlink/base/elapsed_timer.h>
#include <vlink/base/format.h>
#include <vlink/base/helpers.h>
#include <vlink/base/message_loop.h>
#include <vlink/base/timer.h>
#include <vlink/base/utils.h>
#include <vlink/extension/terminal_stream.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "./report_helpers.h"
#include "./report_internal.h"

namespace vlink::bench::report {

struct TerminalColumn final {
  std::string title;
  int width{0};
  vlink::Function<std::string(const AggregatedCase&)> value;
};

struct TerminalLayout final {
  std::vector<TerminalColumn> columns;
  int summary_rows{3};
  int detail_rows{13};
  int footer_rows{1};
};

enum class TerminalSortMode : uint8_t {
  kDefault = 0,
  kRecvMbDesc = 1,
  kP95Asc = 2,
  kLossDesc = 3,
};

constexpr const char* kEllipsis = "\xE2\x80\xA6";

inline std::pair<std::string, int> truncate_to_width(std::string_view text, int max_width, bool with_ellipsis) {
  if (max_width <= 0) {
    return {std::string(), 0};
  }

  const int full_width = display_width(text);

  if (full_width <= max_width) {
    return {std::string(text), full_width};
  }

  const bool room_for_ellipsis = with_ellipsis && max_width >= 2;
  const int budget = room_for_ellipsis ? max_width - 1 : max_width;

  std::string result;
  result.reserve(text.size());
  int width = 0;

  for (size_t index = 0; index < text.size();) {
    uint32_t cp = 0;
    size_t bytes = 0;
    decode_utf8(text, index, cp, bytes);
    const int cw = codepoint_width(cp);

    if (width + cw > budget) {
      break;
    }

    result.append(text.data() + index, bytes);
    width += cw;
    index += bytes;
  }

  if (room_for_ellipsis) {
    result.append(kEllipsis);
    width += 1;
  }

  return {std::move(result), width};
}

std::string fit_text(const std::string& text, int width) {
  if (width <= 0) {
    return std::string();
  }

  auto [truncated, w] = truncate_to_width(text, width, true);

  if (w < width) {
    truncated.append(static_cast<size_t>(width - w), ' ');
  }

  return truncated;
}

std::string fit_text_no_pad(const std::string& text, int width) {
  if (width <= 0) {
    return std::string();
  }

  return truncate_to_width(text, width, true).first;
}

size_t find_wrap_pos(std::string_view text, size_t max_length) {
  const int max_cols = static_cast<int>(max_length);

  if (max_cols <= 0) {
    return 0;
  }

  int width = 0;
  size_t hard_end = 0;
  size_t soft_end = 0;

  for (size_t index = 0; index < text.size();) {
    uint32_t cp = 0;
    size_t bytes = 0;
    decode_utf8(text, index, cp, bytes);
    const int cw = codepoint_width(cp);

    if (width + cw > max_cols) {
      break;
    }

    width += cw;
    const size_t next = index + bytes;
    hard_end = next;

    if (cp == ' ' || cp == '|' || cp == ',') {
      soft_end = next;
    }

    index = next;
  }

  if (hard_end >= text.size()) {
    return text.size();
  }

  return soft_end != 0 ? soft_end : hard_end;
}

void append_wrapped_detail(std::vector<std::string>& lines, std::string_view label, const std::string& value,
                           int width) {
  const std::string prefix(label);
  const int prefix_width = display_width(prefix);
  const std::string indent(static_cast<size_t>(prefix_width), ' ');
  const int content_width = std::max(width - prefix_width, 1);
  std::string_view remaining(value);
  bool first_line = true;

  if (remaining.empty()) {
    lines.emplace_back(prefix + "-");
    return;
  }

  while (!remaining.empty()) {
    size_t piece_size = find_wrap_pos(remaining, static_cast<size_t>(content_width));
    std::string_view piece = remaining.substr(0, piece_size);

    while (!piece.empty() && piece.back() == ' ') {
      piece.remove_suffix(1);
    }

    lines.emplace_back((first_line ? prefix : indent) + std::string(piece));
    remaining.remove_prefix(piece_size);

    while (!remaining.empty() && remaining.front() == ' ') {
      remaining.remove_prefix(1);
    }

    first_line = false;
  }
}

std::vector<std::string> build_terminal_detail_lines(const AggregatedCase& item, int width) {
  std::vector<std::string> lines;
  lines.reserve(16);

  append_wrapped_detail(lines, " case: ",
                        std::string(Bench::suite_to_string(item.scenario.suite)) + " | " +
                            Bench::mode_to_string(item.scenario.mode) + " | " + make_topology_label(item.scenario) +
                            " | " + item.transport + " | " + Bench::payload_to_string(item.scenario.payload) + " | " +
                            format_size_label(item.scenario.payload_size),
                        width);

  if (is_serialization_case(item)) {
    append_wrapped_detail(lines, " rate: ",
                          format_rate_label(item.scenario) + " | repeats " + std::to_string(item.success_count) + "/" +
                              std::to_string(item.sample_count) + " | encode " +
                              format_metric_cell(item.serialize_mb_per_sec) + " MB/s | decode " +
                              format_metric_cell(item.deserialize_mb_per_sec) + " MB/s",
                          width);
    append_wrapped_detail(lines, " cpu : ",
                          "encode cpu " + format_metric_cell(item.pub_cpu_ms) + " ms | decode cpu " +
                              format_metric_cell(item.sub_cpu_ms) + " ms",
                          width);
  } else {
    append_wrapped_detail(lines, " rate: ",
                          format_rate_label(item.scenario) + " | repeats " + std::to_string(item.success_count) + "/" +
                              std::to_string(item.sample_count) + " | recv " +
                              format_metric_cell(item.recv_msgs_per_sec) + " msg/s | recv " +
                              format_metric_cell(item.recv_mb_per_sec) + " MB/s",
                          width);
    append_wrapped_detail(lines, " lat : ",
                          "p50 " + format_metric_cell(item.p50_latency_us) + " us | mean p95 " +
                              format_metric_cell(item.p95_latency_us) + " us | mean p99 " +
                              format_metric_cell(item.p99_latency_us) + " us | loss " + format_loss_detail(item),
                          width);

    if (item.p999_latency_us.count != 0 || item.p9999_latency_us.count != 0 || item.max_latency_us.count != 0 ||
        item.latency_stddev_us.count != 0) {
      append_wrapped_detail(lines, " tail: ",
                            "p99.9 " + format_metric_cell(item.p999_latency_us) + " us | p99.99 " +
                                format_metric_cell(item.p9999_latency_us) + " us | max " +
                                format_metric_cell(item.max_latency_us) + " us | stddev " +
                                format_metric_cell(item.latency_stddev_us) + " us",
                            width);
    }

    if (item.latency_samples_dropped != 0) {
      append_wrapped_detail(lines, " samp: ",
                            "latency samples dropped " + std::to_string(item.latency_samples_dropped) +
                                " after reaching internal capture limit",
                            width);
    }
  }

  append_wrapped_detail(lines, " proc: ",
                        "pub_cpu " + format_metric_cell(item.pub_cpu_ms) + " ms | sub_cpu " +
                            format_metric_cell(item.sub_cpu_ms) + " ms | cpu " + format_metric_cell(item.cpu_usage) +
                            " % | rss " + format_memory_cell(item.memory_usage) + " MB",
                        width);
  append_wrapped_detail(lines, " url : ", item.scenario.url, width);
  if (item.transport == "shm2" && item.wire_size != 0) {
    append_wrapped_detail(lines, " slice: ", format_size_label(item.wire_size), width);
  }
  append_wrapped_detail(
      lines,
      " node: ", item.scenario.properties.empty() ? std::string("-") : format_property_list(item.scenario.properties),
      width);
  append_wrapped_detail(
      lines, " pub : ",
      item.scenario.pub_properties.empty() ? std::string("-") : format_property_list(item.scenario.pub_properties),
      width);
  append_wrapped_detail(
      lines, " sub : ",
      item.scenario.sub_properties.empty() ? std::string("-") : format_property_list(item.scenario.sub_properties),
      width);
  append_wrapped_detail(lines, " stat: ", make_case_status_text(item), width);
  return lines;
}

const char* terminal_sort_mode_to_string(TerminalSortMode mode, bool serialization_only) {
  switch (mode) {
    case TerminalSortMode::kRecvMbDesc:
      return serialization_only ? "enc-mb" : "recv-mb";
    case TerminalSortMode::kP95Asc:
      return serialization_only ? "dec-mb" : "p95";
    case TerminalSortMode::kLossDesc:
      return serialization_only ? "default" : "loss";
    default:
      return "default";
  }
}

int get_stability_rank(const AggregatedCase& item) {
  if (item.all_success() && !has_delivery_loss(item)) {
    return 0;
  }

  if (item.success_count == 0 || item.failure_count != 0) {
    return 2;
  }

  return 1;
}

int get_default_detail_rows(int terminal_width) {
  if (terminal_width >= 128) {
    return 13;
  }

  if (terminal_width >= 104) {
    return 9;
  }

  return 5;
}

bool matches_suite_filter(const AggregatedCase& item, int suite_filter) {
  if (suite_filter >= 0) {
    return static_cast<int>(item.scenario.suite) == suite_filter;
  }

  return !is_serialization_case(item);
}

bool matches_mode_filter(const AggregatedCase& item, int mode_filter) {
  return mode_filter < 0 || static_cast<int>(item.scenario.mode) == mode_filter;
}

bool matches_unstable_filter(const AggregatedCase& item, bool unstable_only) {
  return !unstable_only || !item.all_success() || has_delivery_loss(item);
}

std::vector<int> build_available_suite_filters(const std::vector<AggregatedCase>& items) {
  std::vector<int> filters;
  filters.emplace_back(-1);

  for (auto suite : {Bench::kThroughputSuite, Bench::kLatencySuite, Bench::kTopologySuite, Bench::kBackpressureSuite,
                     Bench::kSerializationSuite}) {
    if (std::find_if(items.begin(), items.end(),
                     [suite](const AggregatedCase& item) { return item.scenario.suite == suite; }) != items.end()) {
      filters.emplace_back(static_cast<int>(suite));
    }
  }

  return filters;
}

std::vector<int> build_available_mode_filters(const std::vector<AggregatedCase>& items, int suite_filter) {
  std::vector<int> filters;
  filters.emplace_back(-1);

  for (auto mode : {Bench::kLocalDirectMode, Bench::kLocalLoopMode, Bench::kProcessMode}) {
    if (std::find_if(items.begin(), items.end(), [suite_filter, mode](const AggregatedCase& item) {
          return matches_suite_filter(item, suite_filter) && item.scenario.mode == mode;
        }) != items.end()) {
      filters.emplace_back(static_cast<int>(mode));
    }
  }

  return filters;
}

std::vector<std::string> build_available_transport_filters(const std::vector<AggregatedCase>& items, int suite_filter,
                                                           int mode_filter, bool unstable_only) {
  std::vector<std::string> filters;
  filters.emplace_back();

  for (const auto& item : items) {
    if (!matches_suite_filter(item, suite_filter) || !matches_mode_filter(item, mode_filter) ||
        !matches_unstable_filter(item, unstable_only)) {
      continue;
    }

    if (std::find(filters.begin(), filters.end(), item.transport) == filters.end()) {
      filters.emplace_back(item.transport);
    }
  }

  return filters;
}

template <typename T>
T get_next_filter_value(const std::vector<T>& filters, const T& current_value) {
  if (filters.empty()) {
    return current_value;
  }

  const auto it = std::find(filters.begin(), filters.end(), current_value);

  if (it == filters.end() || std::next(it) == filters.end()) {
    return filters.front();
  }

  return *std::next(it);
}

TerminalSortMode get_next_serialization_sort_mode(TerminalSortMode sort_mode) {
  if (sort_mode == TerminalSortMode::kDefault) {
    return TerminalSortMode::kRecvMbDesc;
  }

  if (sort_mode == TerminalSortMode::kRecvMbDesc) {
    return TerminalSortMode::kP95Asc;
  }

  return TerminalSortMode::kDefault;
}

TerminalSortMode get_next_transport_sort_mode(TerminalSortMode sort_mode) {
  if (sort_mode == TerminalSortMode::kDefault) {
    return TerminalSortMode::kRecvMbDesc;
  }

  if (sort_mode == TerminalSortMode::kRecvMbDesc) {
    return TerminalSortMode::kP95Asc;
  }

  if (sort_mode == TerminalSortMode::kP95Asc) {
    return TerminalSortMode::kLossDesc;
  }

  return TerminalSortMode::kDefault;
}

std::vector<const AggregatedCase*> build_terminal_view(const std::vector<AggregatedCase>& items, int suite_filter,
                                                       int mode_filter, const std::string& transport_filter,
                                                       bool failures_only, TerminalSortMode sort_mode) {
  std::vector<const AggregatedCase*> view;
  view.reserve(items.size());

  for (const auto& item : items) {
    if (!matches_suite_filter(item, suite_filter)) {
      continue;
    }

    if (!matches_mode_filter(item, mode_filter)) {
      continue;
    }

    if (!transport_filter.empty() && item.transport != transport_filter) {
      continue;
    }

    if (!matches_unstable_filter(item, failures_only)) {
      continue;
    }

    view.emplace_back(&item);
  }

  const bool serialization_only = suite_filter == static_cast<int>(Bench::kSerializationSuite);
  std::sort(view.begin(), view.end(),
            [sort_mode, serialization_only](const AggregatedCase* lhs, const AggregatedCase* rhs) {
              const int lhs_stability = get_stability_rank(*lhs);
              const int rhs_stability = get_stability_rank(*rhs);

              if (lhs_stability != rhs_stability) {
                return lhs_stability < rhs_stability;
              }

              if (serialization_only && sort_mode != TerminalSortMode::kDefault) {
                switch (sort_mode) {
                  case TerminalSortMode::kRecvMbDesc:
                    if (lhs->serialize_mb_per_sec.average() != rhs->serialize_mb_per_sec.average()) {
                      return lhs->serialize_mb_per_sec.average() > rhs->serialize_mb_per_sec.average();
                    }

                    break;
                  case TerminalSortMode::kP95Asc:
                    if (lhs->deserialize_mb_per_sec.average() != rhs->deserialize_mb_per_sec.average()) {
                      return lhs->deserialize_mb_per_sec.average() > rhs->deserialize_mb_per_sec.average();
                    }

                    break;
                  default:
                    break;
                }
              }

              if (sort_mode != TerminalSortMode::kDefault) {
                switch (sort_mode) {
                  case TerminalSortMode::kRecvMbDesc:
                    if (lhs->recv_mb_per_sec.average() != rhs->recv_mb_per_sec.average()) {
                      return lhs->recv_mb_per_sec.average() > rhs->recv_mb_per_sec.average();
                    }

                    break;
                  case TerminalSortMode::kP95Asc:
                    if (lhs->p95_latency_us.count == 0 || rhs->p95_latency_us.count == 0) {
                      return lhs->p95_latency_us.count > rhs->p95_latency_us.count;
                    }

                    if (lhs->p95_latency_us.average() != rhs->p95_latency_us.average()) {
                      return lhs->p95_latency_us.average() < rhs->p95_latency_us.average();
                    }

                    break;
                  case TerminalSortMode::kLossDesc:
                    if (compute_loss_ratio_percent(*lhs) != compute_loss_ratio_percent(*rhs)) {
                      return compute_loss_ratio_percent(*lhs) > compute_loss_ratio_percent(*rhs);
                    }

                    break;
                  default:
                    break;
                }
              }

              return std::tuple{static_cast<int>(lhs->scenario.suite),
                                static_cast<int>(lhs->scenario.mode),
                                lhs->url_order_index,
                                lhs->transport,
                                lhs->scenario.url,
                                lhs->scenario.payload_size} < std::tuple{static_cast<int>(rhs->scenario.suite),
                                                                         static_cast<int>(rhs->scenario.mode),
                                                                         rhs->url_order_index,
                                                                         rhs->transport,
                                                                         rhs->scenario.url,
                                                                         rhs->scenario.payload_size};
            });

  return view;
}

std::pair<int, int> normalize_terminal_size(std::pair<int, int> terminal_size) {
  if (terminal_size.first <= 0) {
    terminal_size.first = 96;
  }

  if (terminal_size.second <= 0) {
    terminal_size.second = 24;
  }

  return terminal_size;
}

std::string terminal_suite_filter_to_string(int suite_filter) {
  if (suite_filter < 0) {
    return "transport";
  }

  return Bench::suite_to_string(static_cast<Bench::Suite>(suite_filter));
}

std::string terminal_mode_filter_to_string(int mode_filter) {
  if (mode_filter < 0) {
    return "all";
  }

  return Bench::mode_to_string(static_cast<Bench::Mode>(mode_filter));
}

TerminalLayout build_terminal_layout(int terminal_width, int terminal_height) {
  TerminalLayout layout;

  if (terminal_width >= 159) {
    layout.columns = {
        {"Suite", 12, [](const AggregatedCase& item) { return Bench::suite_to_string(item.scenario.suite); }},
        {"Mode", 12, [](const AggregatedCase& item) { return Bench::mode_to_string(item.scenario.mode); }},
        {"Topo", 12, [](const AggregatedCase& item) { return make_topology_label(item.scenario); }},
        {"Payload", 9, [](const AggregatedCase& item) { return Bench::payload_to_string(item.scenario.payload); }},
        {"Trans", 10, [](const AggregatedCase& item) { return item.transport; }},
        {"Size", 10, [](const AggregatedCase& item) { return format_size_label(item.scenario.payload_size); }},
        {"Rate", 13, [](const AggregatedCase& item) { return format_rate_label(item.scenario); }},
        {"Repeat", 8,
         [](const AggregatedCase& item) {
           return std::to_string(item.success_count) + "/" + std::to_string(item.sample_count);
         }},
        {"RecvMB/s", 10, [](const AggregatedCase& item) { return format_metric_cell(item.recv_mb_per_sec); }},
        {"MeanP95", 10, [](const AggregatedCase& item) { return format_metric_cell(item.p95_latency_us); }},
        {"Loss%", 8, [](const AggregatedCase& item) { return format_loss_ratio_cell(item, 1); }},
        {"CPU%", 8, [](const AggregatedCase& item) { return format_metric_cell(item.cpu_usage); }},
        {"RSSMB", 8, [](const AggregatedCase& item) { return format_memory_cell(item.memory_usage); }},
        {"Status", 14, [](const AggregatedCase& item) { return make_case_status_brief(item); }},
    };
  } else if (terminal_width >= 138) {
    layout.columns = {
        {"Suite", 11, [](const AggregatedCase& item) { return Bench::suite_to_string(item.scenario.suite); }},
        {"Mode", 11, [](const AggregatedCase& item) { return Bench::mode_to_string(item.scenario.mode); }},
        {"Topo", 11, [](const AggregatedCase& item) { return make_topology_label(item.scenario); }},
        {"Payload", 8, [](const AggregatedCase& item) { return Bench::payload_to_string(item.scenario.payload); }},
        {"Trans", 9, [](const AggregatedCase& item) { return item.transport; }},
        {"Size", 9, [](const AggregatedCase& item) { return format_size_label(item.scenario.payload_size); }},
        {"Rate", 12, [](const AggregatedCase& item) { return format_rate_label(item.scenario); }},
        {"Repeat", 8,
         [](const AggregatedCase& item) {
           return std::to_string(item.success_count) + "/" + std::to_string(item.sample_count);
         }},
        {"RecvMB/s", 10, [](const AggregatedCase& item) { return format_metric_cell(item.recv_mb_per_sec); }},
        {"MeanP95", 9, [](const AggregatedCase& item) { return format_metric_cell(item.p95_latency_us); }},
        {"Loss%", 7, [](const AggregatedCase& item) { return format_loss_ratio_cell(item, 1); }},
        {"CPU%", 7, [](const AggregatedCase& item) { return format_metric_cell(item.cpu_usage); }},
        {"Status", 12, [](const AggregatedCase& item) { return make_case_status_brief(item); }},
    };
  } else if (terminal_width >= 105) {
    layout.columns = {
        {"Suite", 10, [](const AggregatedCase& item) { return Bench::suite_to_string(item.scenario.suite); }},
        {"Mode", 10, [](const AggregatedCase& item) { return Bench::mode_to_string(item.scenario.mode); }},
        {"Trans", 10, [](const AggregatedCase& item) { return item.transport; }},
        {"Payload", 8, [](const AggregatedCase& item) { return Bench::payload_to_string(item.scenario.payload); }},
        {"Size", 9, [](const AggregatedCase& item) { return format_size_label(item.scenario.payload_size); }},
        {"Rate", 11, [](const AggregatedCase& item) { return format_rate_label(item.scenario); }},
        {"RecvMB/s", 10, [](const AggregatedCase& item) { return format_metric_cell(item.recv_mb_per_sec); }},
        {"MeanP95", 9, [](const AggregatedCase& item) { return format_metric_cell(item.p95_latency_us); }},
        {"Loss%", 7, [](const AggregatedCase& item) { return format_loss_ratio_cell(item, 1); }},
        {"Status", 10, [](const AggregatedCase& item) { return make_case_status_brief(item); }},
    };
  } else {
    layout.columns = {
        {"Suite", 8, [](const AggregatedCase& item) { return Bench::suite_to_string(item.scenario.suite); }},
        {"Mode", 8, [](const AggregatedCase& item) { return Bench::mode_to_string(item.scenario.mode); }},
        {"Trans", 8, [](const AggregatedCase& item) { return item.transport; }},
        {"Size", 8, [](const AggregatedCase& item) { return format_size_label(item.scenario.payload_size); }},
        {"Rate", 8, [](const AggregatedCase& item) { return format_rate_label(item.scenario); }},
        {"RecvMB", 8, [](const AggregatedCase& item) { return format_metric_cell(item.recv_mb_per_sec); }},
        {"Status", 8, [](const AggregatedCase& item) { return make_case_status_brief(item); }},
    };
  }

  const int safe_height = std::max(terminal_height, layout.summary_rows + layout.footer_rows + 2);
  const int available_rows = std::max(safe_height - layout.summary_rows - layout.footer_rows, 2);
  const int min_table_rows = terminal_width >= 104 ? 4 : 3;
  const int default_detail_rows = get_default_detail_rows(terminal_width);
  layout.detail_rows = std::clamp(available_rows - min_table_rows, 1, default_detail_rows);
  return layout;
}

TerminalLayout build_serialization_terminal_layout(int terminal_width, int terminal_height) {
  TerminalLayout layout;

  if (terminal_width >= 127) {
    layout.columns = {
        {"Suite", 13, [](const AggregatedCase& item) { return Bench::suite_to_string(item.scenario.suite); }},
        {"Mode", 12, [](const AggregatedCase& item) { return Bench::mode_to_string(item.scenario.mode); }},
        {"Payload", 10, [](const AggregatedCase& item) { return Bench::payload_to_string(item.scenario.payload); }},
        {"Trans", 8, [](const AggregatedCase& item) { return item.transport; }},
        {"Size", 10, [](const AggregatedCase& item) { return format_size_label(item.scenario.payload_size); }},
        {"Repeat", 8,
         [](const AggregatedCase& item) {
           return std::to_string(item.success_count) + "/" + std::to_string(item.sample_count);
         }},
        {"EncMB/s", 11, [](const AggregatedCase& item) { return format_metric_cell(item.serialize_mb_per_sec); }},
        {"DecMB/s", 11, [](const AggregatedCase& item) { return format_metric_cell(item.deserialize_mb_per_sec); }},
        {"EncCPUms", 10, [](const AggregatedCase& item) { return format_metric_cell(item.pub_cpu_ms); }},
        {"DecCPUms", 10, [](const AggregatedCase& item) { return format_metric_cell(item.sub_cpu_ms); }},
        {"Status", 12, [](const AggregatedCase& item) { return make_case_status_brief(item); }},
    };
  } else if (terminal_width >= 99) {
    layout.columns = {
        {"Suite", 12, [](const AggregatedCase& item) { return Bench::suite_to_string(item.scenario.suite); }},
        {"Mode", 11, [](const AggregatedCase& item) { return Bench::mode_to_string(item.scenario.mode); }},
        {"Payload", 9, [](const AggregatedCase& item) { return Bench::payload_to_string(item.scenario.payload); }},
        {"Size", 9, [](const AggregatedCase& item) { return format_size_label(item.scenario.payload_size); }},
        {"Repeat", 8,
         [](const AggregatedCase& item) {
           return std::to_string(item.success_count) + "/" + std::to_string(item.sample_count);
         }},
        {"EncMB/s", 10, [](const AggregatedCase& item) { return format_metric_cell(item.serialize_mb_per_sec); }},
        {"DecMB/s", 10, [](const AggregatedCase& item) { return format_metric_cell(item.deserialize_mb_per_sec); }},
        {"EncCPU", 8, [](const AggregatedCase& item) { return format_metric_cell(item.pub_cpu_ms); }},
        {"Status", 12, [](const AggregatedCase& item) { return make_case_status_brief(item); }},
    };
  } else {
    layout.columns = {
        {"Suite", 10, [](const AggregatedCase& item) { return Bench::suite_to_string(item.scenario.suite); }},
        {"Payload", 8, [](const AggregatedCase& item) { return Bench::payload_to_string(item.scenario.payload); }},
        {"Size", 8, [](const AggregatedCase& item) { return format_size_label(item.scenario.payload_size); }},
        {"EncMB", 10, [](const AggregatedCase& item) { return format_metric_cell(item.serialize_mb_per_sec); }},
        {"DecMB", 10, [](const AggregatedCase& item) { return format_metric_cell(item.deserialize_mb_per_sec); }},
        {"Status", 10, [](const AggregatedCase& item) { return make_case_status_brief(item); }},
    };
  }

  const int safe_height = std::max(terminal_height, layout.summary_rows + layout.footer_rows + 2);
  const int available_rows = std::max(safe_height - layout.summary_rows - layout.footer_rows, 2);
  const int min_table_rows = terminal_width >= 99 ? 4 : 3;
  const int default_detail_rows = get_default_detail_rows(terminal_width);
  layout.detail_rows = std::clamp(available_rows - min_table_rows, 1, default_detail_rows);
  return layout;
}

constexpr const char* kStatusIconPass = "\xE2\x97\x8F ";
constexpr const char* kStatusIconWarn = "\xE2\x97\x90 ";
constexpr const char* kStatusIconFail = "\xE2\x97\x8B ";
constexpr const char* kStatusIconSelected = "\xE2\x96\xB6 ";

constexpr size_t kStatusIconBytes = 4;

inline const char* pick_status_icon(const AggregatedCase& item) {
  if (item.all_success() && !has_delivery_loss(item)) {
    return kStatusIconPass;
  }

  if (item.success_count == 0 || item.failure_count != 0) {
    return kStatusIconFail;
  }

  return kStatusIconWarn;
}

std::string build_terminal_header(const TerminalLayout& layout) {
  std::string line = " ";
  bool first = true;

  for (const auto& column : layout.columns) {
    if (first) {
      line += fit_text("  " + std::string(column.title), column.width);
      first = false;
    } else {
      line += fit_text(column.title, column.width);
    }

    line += ' ';
  }

  return line;
}

std::string build_terminal_row(const TerminalLayout& layout, const AggregatedCase& item) {
  std::string line = " ";
  bool first = true;

  for (const auto& column : layout.columns) {
    if (first) {
      const int inner_width = std::max(column.width - 2, 0);
      std::string inner = fit_text(column.value(item), inner_width);
      line += pick_status_icon(item);
      line += std::move(inner);
      first = false;
    } else {
      line += fit_text(column.value(item), column.width);
    }

    line += ' ';
  }

  return line;
}

constexpr const char* kAnsiReset = "\033[0m";
constexpr const char* kAnsiClearLine = "\033[K";

constexpr const char* kStyleHeaderBar = "\033[48;5;236;38;5;255;1m";

constexpr const char* kStyleFilterLine = "\033[38;5;250m";

constexpr const char* kStyleColumnHeader = "\033[48;5;238;38;5;252;1m";

constexpr const char* kStyleSelectedRow = "\033[48;5;238;1m";

constexpr const char* kStylePassText = "\033[38;5;78m";
constexpr const char* kStyleWarnText = "\033[38;5;179m";
constexpr const char* kStyleFailText = "\033[38;5;167m";

constexpr const char* kStyleDetailTitle = "\033[48;5;238;38;5;252;1m";

constexpr const char* kStyleDetailBody = "\033[2;38;5;250m";

constexpr const char* kStyleRule = "\033[38;5;240m";

constexpr const char* kRuleChar = "\xE2\x94\x80";

inline std::string make_horizontal_rule(int line_width) {
  std::string out;

  if (line_width <= 0) {
    return out;
  }

  out.reserve(static_cast<size_t>(line_width) * 3);

  for (int i = 0; i < line_width; ++i) {
    out += kRuleChar;
  }

  return out;
}

inline void overwrite_with_selection_chevron(std::string& row) {
  if (row.size() >= 1 + kStatusIconBytes) {
    const std::string chevron(kStatusIconSelected);

    if (chevron.size() == kStatusIconBytes) {
      std::copy(chevron.begin(), chevron.end(), row.begin() + 1);
    }
  }
}

inline bool case_matches_search(const AggregatedCase& item, const std::string& needle_lc) {
  if (needle_lc.empty()) {
    return true;
  }

  if (ascii_tolower(item.transport).find(needle_lc) != std::string::npos) {
    return true;
  }

  if (ascii_tolower(item.scenario.url).find(needle_lc) != std::string::npos) {
    return true;
  }

  if (ascii_tolower(item.scenario.qos_profile).find(needle_lc) != std::string::npos) {
    return true;
  }

  if (ascii_tolower(join_strings(item.scenario.properties, " ")).find(needle_lc) != std::string::npos) {
    return true;
  }

  if (ascii_tolower(join_strings(item.scenario.pub_properties, " ")).find(needle_lc) != std::string::npos) {
    return true;
  }

  if (ascii_tolower(join_strings(item.scenario.sub_properties, " ")).find(needle_lc) != std::string::npos) {
    return true;
  }

  return false;
}

inline void flush_overlay_frame(const std::ostringstream& output_stream) {
  constexpr int kFlushMinLine = 5;
  constexpr int kFlushMinSleep = 0;
  const auto output_str = output_stream.str();
  auto lines = Helpers::split_view(output_str, '\n');

  for (size_t index = 0; index < lines.size(); ++index) {
    VLINK_TERM_OUT << lines[index];

    if (index + 1 < lines.size()) {
      VLINK_TERM_OUT << "\n";

      if (index > 0 && index % kFlushMinLine == 0) {
        VLINK_TERM_OUT.flush();

        if constexpr (kFlushMinSleep > 0) {
          std::this_thread::sleep_for(std::chrono::microseconds(kFlushMinSleep));
        }
      }
    }
  }

  VLINK_TERM_OUT.flush();
}

void draw_terminal_help_overlay(int terminal_width, int terminal_height) {
  const int line_width = std::max(terminal_width, 1);
  const int canvas_height = std::max(terminal_height, 1);
  std::ostringstream output;
  output << "\033[H\033[J";

  std::vector<std::pair<std::string, std::string>> rows;
  rows.emplace_back("up / down", "move selection");
  rows.emplace_back("left / right", "previous / next page");
  rows.emplace_back("pgup / pgdown", "jump a full page");
  rows.emplace_back("b / space", "previous / next page (single-key)");
  rows.emplace_back("home / end", "first / last row");
  rows.emplace_back("[  ]", "scroll detail panel");
  rows.emplace_back("u", "cycle suite filter");
  rows.emplace_back("m", "cycle mode filter");
  rows.emplace_back("t", "cycle transport filter");
  rows.emplace_back("f", "toggle unstable-only filter");
  rows.emplace_back("s", "cycle sort mode (stable rows first)");
  rows.emplace_back("o", "toggle sort order ascending / descending");
  rows.emplace_back("/", "search url / qos / props / transport");
  rows.emplace_back("x", "clear active search");
  rows.emplace_back("e", "export current view to ./vlink-bench-export-<ts>.csv");
  rows.emplace_back("? / h", "toggle this help overlay");
  rows.emplace_back("esc / q", "close help / quit");

  output << kAnsiClearLine << kStyleHeaderBar
         << fit_text_no_pad(" vlink-bench  \xE2\x94\x82  help  \xE2\x94\x82  press ?  h  esc  or  q  to close",
                            line_width)
         << kAnsiReset << "\n";
  int rendered = 1;

  if (rendered < canvas_height) {
    output << kAnsiClearLine << kStyleRule << fit_text_no_pad(make_horizontal_rule(line_width), line_width)
           << kAnsiReset << "\n";
    ++rendered;
  }

  if (rendered < canvas_height) {
    output << kAnsiClearLine << kStyleColumnHeader << fit_text_no_pad(" keyboard shortcuts ", line_width) << kAnsiReset
           << "\n";
    ++rendered;
  }

  const int key_col_width = std::min(28, std::max(12, line_width / 4));

  for (const auto& row : rows) {
    if (rendered >= canvas_height - 1) {
      break;
    }

    std::string line = "  " + fit_text(row.first, key_col_width) + "  " + row.second;
    output << kAnsiClearLine << kStyleDetailBody << fit_text_no_pad(line, line_width) << kAnsiReset << "\n";
    ++rendered;
  }

  while (rendered < canvas_height - 1) {
    output << kAnsiClearLine << "\n";
    ++rendered;
  }

  if (rendered < canvas_height) {
    output << kAnsiClearLine << kStyleHeaderBar
           << fit_text_no_pad(
                  " help overlay  \xE2\x94\x82  table view paused  \xE2\x94\x82  press ?  h  esc  q  to "
                  "return",
                  line_width)
           << kAnsiReset << "\n";
    ++rendered;
  }

  flush_overlay_frame(output);
}

void draw_terminal_page(const std::vector<AggregatedCase>& items, const std::vector<const AggregatedCase*>& view,
                        size_t selected_index, size_t page_index, int rows_per_page, int terminal_width,
                        int terminal_height, size_t planned_case_count, size_t skipped_case_count, int suite_filter,
                        int mode_filter, const std::string& transport_filter, bool failures_only,
                        TerminalSortMode sort_mode, size_t detail_offset) {
  constexpr int kFlushMinLine = 5;
  constexpr int kFlushMinSleep = 0;
  std::ostringstream output;
  const bool serialization_only = suite_filter == static_cast<int>(Bench::kSerializationSuite);
  const auto layout = serialization_only ? build_serialization_terminal_layout(terminal_width, terminal_height)
                                         : build_terminal_layout(terminal_width, terminal_height);
  const size_t total_pages =
      rows_per_page > 0 ? ((view.size() + static_cast<size_t>(rows_per_page) - 1) / static_cast<size_t>(rows_per_page))
                        : 1;
  const size_t total_page_count = std::max<size_t>(total_pages, 1);
  const size_t current_page = std::min(page_index, total_page_count - 1);
  const size_t start_index = current_page * static_cast<size_t>(rows_per_page);
  const size_t end_index = std::min(start_index + static_cast<size_t>(rows_per_page), view.size());
  const size_t selected = view.empty() ? 0 : std::min(selected_index, view.size() - 1);

  const size_t passed_cases = std::count_if(view.begin(), view.end(), [](const AggregatedCase* item) {
    return item->all_success() && !has_delivery_loss(*item);
  });
  const size_t warning_cases = std::count_if(view.begin(), view.end(), [](const AggregatedCase* item) {
    return item->success_count > 0 && item->failure_count == 0 && (!item->all_success() || has_delivery_loss(*item));
  });
  const size_t failed_cases = view.size() - passed_cases - warning_cases;
  const int line_width = std::max(terminal_width, 1);
  const int min_terminal_rows = 8;

  if (terminal_height < min_terminal_rows) {
    std::vector<std::string> lines;
    lines.emplace_back(" vlink-bench terminal report ");
    lines.emplace_back(" terminal height is too small for the interactive view ");
    lines.emplace_back(" resize to at least " + std::to_string(min_terminal_rows) + " rows ");
    lines.emplace_back(" current: " + std::to_string(terminal_width) + "x" + std::to_string(terminal_height) +
                       " | press q to quit ");

    output << "\033[H\033[J";
    const int max_lines = std::max(terminal_height, 1);

    for (int index = 0; index < std::min<int>(static_cast<int>(lines.size()), max_lines); ++index) {
      if (index == 0) {
        output << kAnsiClearLine << kStyleHeaderBar << fit_text_no_pad(lines.at(static_cast<size_t>(index)), line_width)
               << kAnsiReset << "\n";
      } else {
        output << kAnsiClearLine << fit_text_no_pad(lines.at(static_cast<size_t>(index)), line_width) << "\n";
      }
    }

    const auto output_str = output.str();
    auto split_view_list = Helpers::split_view(output_str, '\n');

    for (size_t index = 0; index < split_view_list.size(); ++index) {
      VLINK_TERM_OUT << split_view_list[index];

      if (index + 1 < split_view_list.size()) {
        VLINK_TERM_OUT << "\n";

        if (index > 0 && index % kFlushMinLine == 0) {
          VLINK_TERM_OUT.flush();

          if constexpr (kFlushMinSleep > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(kFlushMinSleep));
          }
        }
      }
    }

    VLINK_TERM_OUT.flush();
    return;
  }

  output << "\033[H\033[J";

  output << kAnsiClearLine << kStyleHeaderBar
         << fit_text_no_pad(" vlink-bench  \xE2\x94\x82  terminal report  \xE2\x94\x82  planned " +
                                std::to_string(planned_case_count) + "  skipped " + std::to_string(skipped_case_count) +
                                "  all " + std::to_string(items.size()) + "  view " + std::to_string(view.size()) +
                                "  pass " + std::to_string(passed_cases) + "  warn " + std::to_string(warning_cases) +
                                "  fail " + std::to_string(failed_cases) + "  \xE2\x94\x82  " +
                                std::to_string(terminal_width) + "x" + std::to_string(terminal_height),
                            line_width)
         << kAnsiReset << "\n";

  std::string status_line = " suite " + terminal_suite_filter_to_string(suite_filter) + "  \xE2\x94\x82  mode " +
                            terminal_mode_filter_to_string(mode_filter) + "  \xE2\x94\x82  transport " +
                            (transport_filter.empty() ? std::string("all") : transport_filter) +
                            "  \xE2\x94\x82  unstable " + (failures_only ? std::string("only") : std::string("all")) +
                            "  \xE2\x94\x82  sort " + terminal_sort_mode_to_string(sort_mode, serialization_only) +
                            "  \xE2\x94\x82  ? help  / search  q quit";
  output << kAnsiClearLine << kStyleFilterLine << fit_text_no_pad(status_line, line_width) << kAnsiReset << "\n";

  output << kAnsiClearLine << kStyleColumnHeader << fit_text_no_pad(build_terminal_header(layout), line_width)
         << kAnsiReset << "\n";

  for (size_t index = start_index; index < end_index; ++index) {
    const auto& item = *view.at(index);
    auto row = fit_text_no_pad(build_terminal_row(layout, item), line_width);

    if (index == selected) {
      overwrite_with_selection_chevron(row);
      output << kAnsiClearLine << kStyleSelectedRow << row << kAnsiReset << "\n";
    } else if (item.all_success() && !has_delivery_loss(item)) {
      output << kAnsiClearLine << kStylePassText << row << kAnsiReset << "\n";
    } else if (item.success_count == 0 || item.failure_count != 0) {
      output << kAnsiClearLine << kStyleFailText << row << kAnsiReset << "\n";
    } else {
      output << kAnsiClearLine << kStyleWarnText << row << kAnsiReset << "\n";
    }
  }

  for (int blank = static_cast<int>(end_index - start_index); blank < rows_per_page; ++blank) {
    output << kAnsiClearLine << "\n";
  }

  if (!items.empty()) {
    if (!view.empty()) {
      const auto& item = *view.at(selected);
      const int detail_width = std::max(terminal_width, 1);
      const auto detail_lines = build_terminal_detail_lines(item, detail_width);
      const int detail_body_rows = std::max(layout.detail_rows - 1, 0);
      const size_t max_detail_offset = detail_lines.size() > static_cast<size_t>(detail_body_rows)
                                           ? detail_lines.size() - static_cast<size_t>(detail_body_rows)
                                           : 0;
      const size_t clamped_detail_offset = std::min(detail_offset, max_detail_offset);
      const size_t visible_end =
          std::min(clamped_detail_offset + static_cast<size_t>(detail_body_rows), detail_lines.size());
      std::ostringstream selection_title;
      selection_title << " selection ";

      if (detail_body_rows > 0 && max_detail_offset > 0) {
        selection_title << "\xE2\x94\x82 " << (clamped_detail_offset + 1) << "-" << visible_end << "/"
                        << detail_lines.size() << " ";
      }

      output << kAnsiClearLine << kStyleDetailTitle << fit_text_no_pad(selection_title.str(), line_width) << kAnsiReset
             << "\n";

      for (int index = 0; index < detail_body_rows; ++index) {
        const size_t line_index = clamped_detail_offset + static_cast<size_t>(index);

        if (line_index < detail_lines.size()) {
          output << kAnsiClearLine << kStyleDetailBody << fit_text_no_pad(detail_lines.at(line_index), detail_width)
                 << kAnsiReset << "\n";
        } else {
          output << kAnsiClearLine << "\n";
        }
      }
    } else {
      output << kAnsiClearLine << kStyleDetailTitle << fit_text_no_pad(" selection ", line_width) << kAnsiReset << "\n";

      for (int index = 0; index < std::max(layout.detail_rows - 1, 0); ++index) {
        if (index == 0) {
          output << kAnsiClearLine << kStyleDetailBody
                 << fit_text_no_pad(" no cases match the current terminal filter", std::max(terminal_width, 1))
                 << kAnsiReset << "\n";
        } else {
          output << kAnsiClearLine << "\n";
        }
      }
    }
  }

  std::ostringstream footer;
  footer << " page " << (view.empty() ? 0 : (current_page + 1)) << "/" << total_page_count << "  \xE2\x94\x82  "
         << "U " << terminal_suite_filter_to_string(suite_filter) << "  "
         << "M " << terminal_mode_filter_to_string(mode_filter) << "  "
         << "T " << (transport_filter.empty() ? std::string("all") : transport_filter) << "  "
         << "F " << (failures_only ? "on" : "off") << "  "
         << "S " << terminal_sort_mode_to_string(sort_mode, serialization_only) << "+stable";

  if (!view.empty()) {
    footer << "  \xE2\x94\x82  sel " << (selected + 1) << "/" << view.size();
  }

  if (detail_offset != 0) {
    footer << "  \xE2\x94\x82  detail +" << detail_offset;
  }

  output << kAnsiClearLine << kStyleHeaderBar << fit_text_no_pad(footer.str(), line_width) << kAnsiReset << "\n";

  const auto output_str = output.str();
  auto lines = Helpers::split_view(output_str, '\n');

  for (size_t index = 0; index < lines.size(); ++index) {
    VLINK_TERM_OUT << lines[index];

    if (index + 1 < lines.size()) {
      VLINK_TERM_OUT << "\n";

      if (index > 0 && index % kFlushMinLine == 0) {
        VLINK_TERM_OUT.flush();

        if constexpr (kFlushMinSleep > 0) {
          std::this_thread::sleep_for(std::chrono::microseconds(kFlushMinSleep));
        }
      }
    }
  }

  VLINK_TERM_OUT.flush();
}

Summary summarize(const Bench::Result& result) {
  Summary summary;
  summary.sample_count = result.scenarios.size();

  const auto items = aggregate_cases(result);
  summary.case_count = items.size();
  summary.passing_case_count = std::count_if(items.begin(), items.end(), [](const AggregatedCase& item) {
    return item.all_success() && !has_delivery_loss(item);
  });
  summary.warning_case_count = std::count_if(items.begin(), items.end(), [](const AggregatedCase& item) {
    return item.success_count > 0 && item.failure_count == 0 && (!item.all_success() || has_delivery_loss(item));
  });
  summary.failing_case_count = summary.case_count - summary.passing_case_count - summary.warning_case_count;
  return summary;
}

bool save_csv(const Bench::Result& result, const std::string& file_path, std::string& error) {
  if (!ensure_parent_dir(file_path, error)) {
    return false;
  }

  std::ostringstream stream;

  const auto items = aggregate_cases(result);
  stream
      << "suite,mode,topology,rate_pattern,payload,transport,url,qos_profile,properties,pub_properties,"
         "sub_properties,payload_size,wire_size,rate_hz,burst_messages,subscribers,publishers,warmup_ms,"
         "subscriber_sleep_us,duration_ms,drain_ms,repeat_total,repeat_success,repeat_failed,sent_avg,received_avg,"
         "expected_avg,"
         "lost_avg,loss_pct_avg,discovery_ms_avg,first_message_ms_avg,send_msgs_per_sec_avg,recv_msgs_per_sec_avg,"
         "send_mb_per_sec_avg,recv_mb_per_sec_avg,avg_latency_us_avg,p50_latency_us_avg,p95_latency_us_avg,"
         "p99_latency_us_avg,p999_latency_us_avg,p9999_latency_us_avg,max_latency_us_avg,latency_stddev_us_avg,"
         "avg_send_block_us_avg,p50_send_block_us_avg,p95_send_block_us_avg,p99_send_block_us_avg,"
         "max_send_block_us_avg,send_block_samples_avg,"
         "serialize_msgs_per_sec_avg,deserialize_msgs_per_sec_avg,serialize_mb_per_sec_avg,"
         "deserialize_mb_per_sec_avg,pub_cpu_ms_avg,sub_cpu_ms_avg,cpu_usage_pct_avg,memory_rss_mb_avg,status,errors\n";

  for (const auto& item : items) {
    stream << quote_csv(Bench::suite_to_string(item.scenario.suite)) << ','
           << quote_csv(Bench::mode_to_string(item.scenario.mode)) << ','
           << quote_csv(Bench::topology_to_string(item.scenario.topology)) << ','
           << quote_csv(Bench::rate_pattern_to_string(item.scenario.rate_pattern)) << ','
           << quote_csv(Bench::payload_to_string(item.scenario.payload)) << ',' << quote_csv(item.transport) << ','
           << quote_csv(item.scenario.url) << ','
           << quote_csv(item.scenario.qos_profile.empty() ? std::string("default") : item.scenario.qos_profile) << ','
           << quote_csv(format_property_list(item.scenario.properties)) << ','
           << quote_csv(format_property_list(item.scenario.pub_properties)) << ','
           << quote_csv(format_property_list(item.scenario.sub_properties)) << ',' << item.scenario.payload_size << ','
           << item.wire_size << ',' << item.scenario.rate_hz << ',' << item.scenario.burst_messages << ','
           << item.scenario.subscribers << ',' << item.scenario.publishers << ',' << item.scenario.warmup_ms << ','
           << item.scenario.subscriber_sleep_us << ',' << item.scenario.duration_ms << ',' << item.scenario.drain_ms
           << ',' << item.sample_count << ',' << item.success_count << ',' << item.failure_count << ','
           << format_metric_cell(item.sent) << ',' << format_metric_cell(item.received) << ','
           << format_metric_cell(item.expected) << ',' << format_metric_cell(item.lost) << ','
           << format_loss_ratio_cell(item) << ',' << format_metric_cell(item.discovery_ms) << ','
           << format_metric_cell(item.first_message_ms) << ',' << format_metric_cell(item.send_msgs_per_sec) << ','
           << format_metric_cell(item.recv_msgs_per_sec) << ',' << format_metric_cell(item.send_mb_per_sec) << ','
           << format_metric_cell(item.recv_mb_per_sec) << ',' << format_metric_cell(item.avg_latency_us) << ','
           << format_metric_cell(item.p50_latency_us) << ',' << format_metric_cell(item.p95_latency_us) << ','
           << format_metric_cell(item.p99_latency_us) << ',' << format_metric_cell(item.p999_latency_us) << ','
           << format_metric_cell(item.p9999_latency_us) << ',' << format_metric_cell(item.max_latency_us) << ','
           << format_metric_cell(item.latency_stddev_us) << ',' << format_metric_cell(item.avg_send_block_us) << ','
           << format_metric_cell(item.p50_send_block_us) << ',' << format_metric_cell(item.p95_send_block_us) << ','
           << format_metric_cell(item.p99_send_block_us) << ',' << format_metric_cell(item.max_send_block_us) << ','
           << format_metric_cell(item.send_block_samples) << ',' << format_metric_cell(item.serialize_msgs_per_sec)
           << ',' << format_metric_cell(item.deserialize_msgs_per_sec) << ','
           << format_metric_cell(item.serialize_mb_per_sec) << ',' << format_metric_cell(item.deserialize_mb_per_sec)
           << ',' << format_metric_cell(item.pub_cpu_ms) << ',' << format_metric_cell(item.sub_cpu_ms) << ','
           << format_metric_cell(item.cpu_usage) << ',' << format_memory_cell(item.memory_usage) << ','
           << quote_csv(make_case_status_text(item)) << ',' << quote_csv(join_strings(item.errors, " | ")) << '\n';
  }

  return write_text_file_atomic(file_path, stream.str(), error);
}

bool print_terminal(const Bench::Result& result, const TerminalOptions& options, std::string& error) {
  (void)error;

  const auto items = aggregate_cases(result);

  if (items.empty()) {
    std::cout << "vlink-bench: no aggregated cases to display." << std::endl;
    return true;
  }

  auto terminal_size = Utils::get_terminal_size();

  if (!options.interactive || terminal_size.first <= 0 || terminal_size.second <= 0) {
    const int line_width = terminal_size.first > 0 ? terminal_size.first : 160;
    const bool colorize = terminal_size.first > 0 && terminal_size.second > 0;
    const size_t planned_cases = result.planned_case_count == 0 ? items.size() : result.planned_case_count;
    const size_t passed_cases = std::count_if(items.begin(), items.end(), [](const AggregatedCase& item) {
      return item.all_success() && !has_delivery_loss(item);
    });
    const size_t warning_cases = std::count_if(items.begin(), items.end(), [](const AggregatedCase& item) {
      return item.success_count > 0 && item.failure_count == 0 && (!item.all_success() || has_delivery_loss(item));
    });
    const size_t failed_cases = items.size() - passed_cases - warning_cases;

    const std::string separator = make_horizontal_rule(std::max(line_width, 1));
    std::vector<const AggregatedCase*> transport_items;
    std::vector<const AggregatedCase*> serialization_items;
    transport_items.reserve(items.size());
    serialization_items.reserve(items.size());

    for (const auto& item : items) {
      if (is_serialization_case(item)) {
        serialization_items.emplace_back(&item);
      } else {
        transport_items.emplace_back(&item);
      }
    }

    const auto print_line = [&line_width, &colorize](const std::string& line, const char* color = nullptr) {
      const auto fitted = fit_text(line, line_width);

      std::string out;
      out.reserve(fitted.size() + 32);

      if (colorize && color != nullptr) {
        out.append(color);
        out.append(fitted);
        out.append("\033[0m");
      } else {
        out.append(fitted);
      }

      out.push_back('\n');
      VLINK_TERM_OUT.write_raw(out.data(), out.size());
      VLINK_TERM_OUT.flush();
    };

    print_line(" vlink-bench  \xE2\x94\x82  summary  \xE2\x94\x82  planned " + std::to_string(planned_cases) +
                   "  skipped " + std::to_string(result.skipped_case_count) + "  cases " +
                   std::to_string(items.size()) + "  pass " + std::to_string(passed_cases) + "  warn " +
                   std::to_string(warning_cases) + "  fail " + std::to_string(failed_cases) +
                   "  \xE2\x94\x82  html report has full charts",
               colorize ? kStyleHeaderBar : nullptr);

    auto print_table = [&print_line, &separator, &colorize](std::string_view title, const TerminalLayout& layout,
                                                            const std::vector<const AggregatedCase*>& table_items) {
      if (table_items.empty()) {
        return;
      }

      print_line(separator, colorize ? kStyleRule : nullptr);

      const std::string decorated_title =
          " \xE2\x94\x81\xE2\x94\x81\xE2\x94\x81 " + std::string(title) + " \xE2\x94\x81\xE2\x94\x81\xE2\x94\x81";
      print_line(decorated_title, colorize ? kStyleColumnHeader : nullptr);
      print_line(build_terminal_header(layout), colorize ? kStyleColumnHeader : nullptr);

      for (const auto* item : table_items) {
        const auto row = build_terminal_row(layout, *item);

        if (item->all_success() && !has_delivery_loss(*item)) {
          print_line(row, colorize ? kStylePassText : nullptr);
        } else if (item->success_count == 0 || item->failure_count != 0) {
          print_line(row, colorize ? kStyleFailText : nullptr);
        } else {
          print_line(row, colorize ? kStyleWarnText : nullptr);
        }
      }
    };

    print_table("transport / latency / topology / backpressure summary",
                build_terminal_layout(terminal_size.first > 0 ? terminal_size.first : 160,
                                      terminal_size.second > 0 ? terminal_size.second : 24),
                transport_items);
    print_table("serialization summary",
                build_serialization_terminal_layout(terminal_size.first > 0 ? terminal_size.first : 160,
                                                    terminal_size.second > 0 ? terminal_size.second : 24),
                serialization_items);
    print_line(separator, colorize ? kStyleRule : nullptr);
    print_line("note: aggregated p95/p99/p99.9 are mean successful-run percentiles; loss is reported separately");

    for (const auto& item : items) {
      if (item.all_success() && !has_delivery_loss(item)) {
        continue;
      }

      print_line(separator, colorize ? kStyleRule : nullptr);

      for (const auto& line : build_terminal_detail_lines(item, line_width)) {
        print_line(line, colorize ? kStyleDetailBody : nullptr);
      }
    }

    return true;
  }

  const auto initial_layout = build_terminal_layout(terminal_size.first, terminal_size.second);
  int rows_per_page = std::max(
      terminal_size.second - initial_layout.summary_rows - initial_layout.detail_rows - initial_layout.footer_rows, 1);
  size_t selected_index = 0;
  size_t page_index = 0;
  size_t detail_offset = 0;
  int suite_filter = -1;
  int mode_filter = -1;
  std::string transport_filter;
  bool failures_only = false;
  TerminalSortMode sort_mode = TerminalSortMode::kDefault;
  bool view_dirty = true;
  bool redraw = true;
  std::atomic_bool has_quit{false};

  bool show_help = false;
  std::atomic_bool help_visible{false};
  bool search_mode = false;
  std::atomic_bool search_visible{false};
  std::string search_buffer;
  std::string search_query;
  bool reverse_sort = false;
  std::string footer_flash_message;
  int footer_flash_ttl = 0;
  std::vector<const AggregatedCase*> view =
      build_terminal_view(items, suite_filter, mode_filter, transport_filter, failures_only, sort_mode);

  MessageLoop terminal_loop;
  terminal_loop.set_name("vlink-bench-terminal");

  auto recalc_page = [&terminal_size, &suite_filter, &items, &mode_filter, &transport_filter, &failures_only,
                      &sort_mode, &view, &search_query, &reverse_sort, &rows_per_page, &selected_index, &page_index,
                      &detail_offset](bool rebuild_view) {
    terminal_size = normalize_terminal_size(Utils::get_terminal_size());
    const bool serialization_only = suite_filter == static_cast<int>(Bench::kSerializationSuite);
    const auto layout = serialization_only
                            ? build_serialization_terminal_layout(terminal_size.first, terminal_size.second)
                            : build_terminal_layout(terminal_size.first, terminal_size.second);

    if (rebuild_view) {
      const auto available_mode_filters = build_available_mode_filters(items, suite_filter);

      if (std::find(available_mode_filters.begin(), available_mode_filters.end(), mode_filter) ==
          available_mode_filters.end()) {
        mode_filter = -1;
      }

      const auto available_transport_filters =
          build_available_transport_filters(items, suite_filter, mode_filter, failures_only);

      if (std::find(available_transport_filters.begin(), available_transport_filters.end(), transport_filter) ==
          available_transport_filters.end()) {
        transport_filter.clear();
      }

      view = build_terminal_view(items, suite_filter, mode_filter, transport_filter, failures_only, sort_mode);

      if (!search_query.empty()) {
        view.erase(std::remove_if(view.begin(), view.end(),
                                  [&search_query_ref = search_query](const AggregatedCase* candidate) {
                                    return !case_matches_search(*candidate, search_query_ref);
                                  }),
                   view.end());
      }

      if (reverse_sort) {
        auto bucket_begin = view.begin();

        while (bucket_begin != view.end()) {
          auto bucket_end = bucket_begin + 1;
          const int rank = get_stability_rank(**bucket_begin);

          while (bucket_end != view.end() && get_stability_rank(**bucket_end) == rank) {
            ++bucket_end;
          }

          std::reverse(bucket_begin, bucket_end);
          bucket_begin = bucket_end;
        }
      }
    }

    rows_per_page = std::max(terminal_size.second - layout.summary_rows - layout.detail_rows - layout.footer_rows, 1);

    if (view.empty()) {
      selected_index = 0;
      page_index = 0;
      return;
    }

    if (selected_index >= view.size()) {
      selected_index = view.size() - 1;
    }

    page_index = selected_index / static_cast<size_t>(rows_per_page);
    const auto detail_width = std::max(terminal_size.first, 1);
    const auto detail_lines = build_terminal_detail_lines(*view.at(selected_index), detail_width);
    const auto detail_body_rows = static_cast<size_t>(std::max(layout.detail_rows - 1, 0));
    const size_t max_detail_offset =
        detail_lines.size() > detail_body_rows ? detail_lines.size() - detail_body_rows : 0;
    detail_offset = std::min(detail_offset, max_detail_offset);
  };

  auto print_function = [&recalc_page, &view_dirty, &redraw, &show_help, &terminal_size, &items, &view, &selected_index,
                         &rows_per_page, &result, &suite_filter, &mode_filter, &transport_filter, &failures_only,
                         &sort_mode, &page_index, &detail_offset, &search_mode, &search_buffer, &footer_flash_ttl,
                         &footer_flash_message]() {
    recalc_page(view_dirty);
    view_dirty = false;
    redraw = false;

    if (show_help) {
      draw_terminal_help_overlay(terminal_size.first, terminal_size.second);
      return;
    }

    draw_terminal_page(items, view, selected_index, page_index, rows_per_page, terminal_size.first,
                       terminal_size.second, result.planned_case_count == 0 ? items.size() : result.planned_case_count,
                       result.skipped_case_count, suite_filter, mode_filter, transport_filter, failures_only, sort_mode,
                       detail_offset);

    const int line_width = std::max(terminal_size.first, 1);

    if (search_mode) {
      std::string prompt = " /search: " + search_buffer + "_  (enter to apply, esc to cancel)";
      std::ostringstream overlay;
      overlay << "\033[" << std::max(terminal_size.second, 1) << ";1H" << kAnsiClearLine << kStyleColumnHeader
              << fit_text_no_pad(prompt, line_width) << kAnsiReset;
      VLINK_TERM_OUT << overlay.str();
      VLINK_TERM_OUT.flush();
    } else if (footer_flash_ttl > 0) {
      std::ostringstream overlay;
      overlay << "\033[" << std::max(terminal_size.second, 1) << ";1H" << kAnsiClearLine << kStyleColumnHeader
              << fit_text_no_pad(" " + footer_flash_message, line_width) << kAnsiReset;
      VLINK_TERM_OUT << overlay.str();
      VLINK_TERM_OUT.flush();
      --footer_flash_ttl;
    }
  };

  auto quit_function = [&has_quit, &terminal_loop]() {
    if VUNLIKELY (has_quit.exchange(true, std::memory_order_relaxed)) {
      return;
    }

    terminal_loop.quit(true);
  };

  constexpr uint32_t kTerminalInterval = 50;

  Timer terminal_timer;
  terminal_timer.set_interval(kTerminalInterval);
  terminal_timer.set_loop_count(Timer::kInfinite);
  terminal_timer.attach(&terminal_loop);
  terminal_timer.set_callback([&quit_function, &print_function, &redraw, &terminal_size]() {
    if VUNLIKELY (Bench::stop_requested()) {
      quit_function();
      return;
    }

    const auto new_terminal_size = normalize_terminal_size(Utils::get_terminal_size());

    if (redraw || new_terminal_size != terminal_size) {
      print_function();
    }
  });
  terminal_timer.start();

  auto detect_keyboard_function = [&has_quit, &quit_function, &help_visible, &search_visible, &terminal_loop,
                                   &search_mode, &search_query, &selected_index, &detail_offset, &view_dirty, &redraw,
                                   &footer_flash_message, &footer_flash_ttl, &print_function, &search_buffer,
                                   &show_help, &suite_filter, &mode_filter, &transport_filter, &failures_only, &items,
                                   &sort_mode, &view, &rows_per_page, &terminal_size, &reverse_sort,
                                   &page_index](const std::string& key) {
    if VUNLIKELY (has_quit.load(std::memory_order_relaxed) || Bench::stop_requested()) {
      quit_function();
      return;
    }

    const bool can_quit_immediately =
        !search_visible.load(std::memory_order_relaxed) && !help_visible.load(std::memory_order_relaxed);

    if ((key == "q" || key == "esc") && can_quit_immediately) {
      quit_function();
      return;
    }

    terminal_loop.post_task([&, key]() {
      if VUNLIKELY (has_quit.load(std::memory_order_relaxed) || Bench::stop_requested()) {
        quit_function();
        return;
      }

      if (search_mode) {
        if (key == "enter") {
          search_mode = false;
          search_visible.store(false, std::memory_order_relaxed);
          search_query = ascii_tolower(search_buffer);
          selected_index = 0;
          page_index = 0;
          detail_offset = 0;
          view_dirty = true;
          redraw = true;

          if (search_query.empty()) {
            footer_flash_message = "search cleared";
          } else {
            footer_flash_message = "search \"" + search_query + "\" applied";
          }

          footer_flash_ttl = 2;
          print_function();
          return;
        }

        if (key == "esc") {
          search_mode = false;
          search_visible.store(false, std::memory_order_relaxed);
          search_buffer.clear();
          footer_flash_message = "search canceled";
          footer_flash_ttl = 2;
          redraw = true;
          print_function();
          return;
        }

        if (key.size() == 1) {
          const auto ch = static_cast<unsigned char>(key[0]);

          if (ch >= 0x20 && ch <= 0x7E) {
            search_buffer.push_back(static_cast<char>(ch));
            redraw = true;
            print_function();
            return;
          }
        }

        return;
      }

      if (show_help) {
        if (key == "?" || key == "h" || key == "esc" || key == "q") {
          show_help = false;
          help_visible.store(false, std::memory_order_relaxed);
          redraw = true;
          print_function();
        }

        return;
      }

      if (key == "q" || key == "esc") {
        quit_function();
        return;
      }

      if (key == "?" || key == "h") {
        show_help = true;
        help_visible.store(true, std::memory_order_relaxed);
        redraw = true;
        print_function();
        return;
      }

      if (key == "/") {
        search_mode = true;
        search_visible.store(true, std::memory_order_relaxed);
        search_buffer.clear();
        redraw = true;
        print_function();
        return;
      }

      bool state_changed = false;
      bool reset_detail_offset = false;

      if (key == "u") {
        suite_filter = get_next_filter_value(build_available_suite_filters(items), suite_filter);

        if (suite_filter == static_cast<int>(Bench::kSerializationSuite) && sort_mode == TerminalSortMode::kLossDesc) {
          sort_mode = TerminalSortMode::kDefault;
        }

        mode_filter = -1;
        transport_filter.clear();
        selected_index = 0;
        page_index = 0;
        view_dirty = true;
        state_changed = true;
        reset_detail_offset = true;
      } else if (key == "m") {
        mode_filter = get_next_filter_value(build_available_mode_filters(items, suite_filter), mode_filter);
        transport_filter.clear();
        selected_index = 0;
        page_index = 0;
        view_dirty = true;
        state_changed = true;
        reset_detail_offset = true;
      } else if (key == "t") {
        transport_filter = get_next_filter_value(
            build_available_transport_filters(items, suite_filter, mode_filter, failures_only), transport_filter);
        selected_index = 0;
        page_index = 0;
        view_dirty = true;
        state_changed = true;
        reset_detail_offset = true;
      } else if (key == "f") {
        failures_only = !failures_only;
        selected_index = 0;
        page_index = 0;
        view_dirty = true;
        state_changed = true;
        reset_detail_offset = true;
      } else if (key == "s") {
        if (suite_filter == static_cast<int>(Bench::kSerializationSuite)) {
          sort_mode = get_next_serialization_sort_mode(sort_mode);
        } else {
          sort_mode = get_next_transport_sort_mode(sort_mode);
        }

        selected_index = 0;
        page_index = 0;
        view_dirty = true;
        state_changed = true;
        reset_detail_offset = true;
      } else if (key == "up" && selected_index > 0) {
        --selected_index;
        state_changed = true;
        reset_detail_offset = true;
      } else if (key == "down" && selected_index + 1 < view.size()) {
        ++selected_index;
        state_changed = true;
        reset_detail_offset = true;
      } else if ((key == "left" || key == "pgup") && page_index > 0) {
        --page_index;

        if (!view.empty()) {
          selected_index = std::min(page_index * static_cast<size_t>(rows_per_page), view.size() - 1);
        }

        state_changed = true;
        reset_detail_offset = true;
      } else if ((key == "right" || key == "pgdown") &&
                 (page_index + 1) * static_cast<size_t>(rows_per_page) < view.size()) {
        ++page_index;

        if (!view.empty()) {
          selected_index = std::min(page_index * static_cast<size_t>(rows_per_page), view.size() - 1);
        }

        state_changed = true;
        reset_detail_offset = true;
      } else if (key == "home") {
        selected_index = 0;
        page_index = 0;
        state_changed = true;
        reset_detail_offset = true;
      } else if (key == "end") {
        if (!view.empty()) {
          selected_index = view.size() - 1;
          page_index = selected_index / static_cast<size_t>(rows_per_page);
          state_changed = true;
        }

        reset_detail_offset = true;
      } else if (key == "[" && !view.empty()) {
        if (detail_offset > 0) {
          --detail_offset;
          state_changed = true;
        }
      } else if (key == "]" && !view.empty()) {
        const auto detail_width = std::max(terminal_size.first, 1);
        const bool serialization_only = suite_filter == static_cast<int>(Bench::kSerializationSuite);
        const auto layout = serialization_only
                                ? build_serialization_terminal_layout(terminal_size.first, terminal_size.second)
                                : build_terminal_layout(terminal_size.first, terminal_size.second);
        const auto detail_lines = build_terminal_detail_lines(*view.at(selected_index), detail_width);
        const auto detail_body_rows = static_cast<size_t>(std::max(layout.detail_rows - 1, 0));
        const size_t max_detail_offset =
            detail_lines.size() > detail_body_rows ? detail_lines.size() - detail_body_rows : 0;

        if (detail_offset < max_detail_offset) {
          ++detail_offset;
          state_changed = true;
        }
      } else if ((key == "b" || key == " ") && !view.empty()) {
        if (key == "b" && page_index > 0) {
          --page_index;
          selected_index = std::min(page_index * static_cast<size_t>(rows_per_page), view.size() - 1);
          state_changed = true;
          reset_detail_offset = true;
        } else if (key == " " && (page_index + 1) * static_cast<size_t>(rows_per_page) < view.size()) {
          ++page_index;
          selected_index = std::min(page_index * static_cast<size_t>(rows_per_page), view.size() - 1);
          state_changed = true;
          reset_detail_offset = true;
        }
      } else if (key == "o") {
        reverse_sort = !reverse_sort;
        selected_index = 0;
        page_index = 0;
        view_dirty = true;
        state_changed = true;
        reset_detail_offset = true;
        footer_flash_message = std::string("order: ") + (reverse_sort ? "descending" : "ascending");
        footer_flash_ttl = 2;
      } else if (key == "x") {
        if (!search_query.empty()) {
          search_query.clear();
          selected_index = 0;
          page_index = 0;
          view_dirty = true;
          state_changed = true;
          reset_detail_offset = true;
          footer_flash_message = "search cleared";
          footer_flash_ttl = 2;
        }
      } else if (key == "e") {
        if (view.empty()) {
          footer_flash_message = "export: nothing to write (empty view)";
          footer_flash_ttl = 2;
          state_changed = true;
        } else {
          const uint64_t timestamp = vlink::ElapsedTimer::get_sys_timestamp(vlink::ElapsedTimer::kMilli) / 1000ULL;
          const std::string path = "./vlink-bench-export-" + std::to_string(timestamp) + ".csv";
          std::ostringstream stream;
          stream << "suite,mode,transport,url,qos_profile,properties,payload_size,rate_hz,"
                    "recv_mb_per_sec_avg,p95_latency_us_avg,loss_pct_avg,status\n";

          for (const auto* item : view) {
            stream << quote_csv(Bench::suite_to_string(item->scenario.suite)) << ','
                   << quote_csv(Bench::mode_to_string(item->scenario.mode)) << ',' << quote_csv(item->transport) << ','
                   << quote_csv(item->scenario.url) << ','
                   << quote_csv(item->scenario.qos_profile.empty() ? std::string("default")
                                                                   : item->scenario.qos_profile)
                   << ',' << quote_csv(format_property_list(item->scenario.properties)) << ','
                   << item->scenario.payload_size << ',' << item->scenario.rate_hz << ','
                   << format_metric_cell(item->recv_mb_per_sec) << ',' << format_metric_cell(item->p95_latency_us)
                   << ',' << format_loss_ratio_cell(*item) << ',' << quote_csv(make_case_status_text(*item)) << '\n';
          }

          std::string write_error;
          const bool ok = write_text_file_atomic(path, stream.str(), write_error);

          if (ok) {
            footer_flash_message = "export saved: " + path + "  (" + std::to_string(view.size()) + " rows)";
          } else {
            footer_flash_message = "export failed: " + write_error;
          }

          footer_flash_ttl = 3;
          state_changed = true;
        }
      }

      if (reset_detail_offset) {
        detail_offset = 0;
      }

      if (state_changed) {
        if (footer_flash_ttl == 0) {
          if (key == "s") {
            footer_flash_message =
                std::string("sort: ") +
                terminal_sort_mode_to_string(sort_mode, suite_filter == static_cast<int>(Bench::kSerializationSuite));
            footer_flash_ttl = 2;
          } else if (key == "u") {
            footer_flash_message = std::string("filter: suite=") + terminal_suite_filter_to_string(suite_filter);
            footer_flash_ttl = 2;
          } else if (key == "m") {
            footer_flash_message = std::string("filter: mode=") + terminal_mode_filter_to_string(mode_filter);
            footer_flash_ttl = 2;
          } else if (key == "t") {
            footer_flash_message =
                std::string("filter: transport=") + (transport_filter.empty() ? std::string("all") : transport_filter);
            footer_flash_ttl = 2;
          } else if (key == "f") {
            footer_flash_message = std::string("unstable-only: ") + (failures_only ? "on" : "off");
            footer_flash_ttl = 2;
          }
        }

        redraw = true;
        print_function();
      }
    });
  };

  Utils::start_detect_keyboard(detect_keyboard_function, 30);

  VLINK_TERM_OUT << "\033[?25l";
  VLINK_TERM_OUT.flush();

  terminal_loop.post_task([&print_function]() { print_function(); });
  terminal_loop.run();

  Utils::stop_detect_keyboard();
  VLINK_TERM_OUT << "\033[H\033[J";
  VLINK_TERM_OUT.flush();
  VLINK_TERM_OUT << "\033[0m\033[?25h" << std::endl;
  VLINK_TERM_OUT.flush();

  if VUNLIKELY (Bench::stop_requested()) {
    error = "terminated by signal";
    return false;
  }

  return true;
}

}  // namespace vlink::bench::report
