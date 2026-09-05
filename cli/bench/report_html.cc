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

#include <vlink/base/format.h>
#include <vlink/base/helpers.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "./report.h"
#include "./report_helpers.h"
#include "./report_html_css.h"
#include "./report_html_i18n.h"
#include "./report_internal.h"
#include "./report_score.h"

namespace vlink::bench::report {

std::string wrap_status_brief_html(const AggregatedCase& item) {
  const std::string brief = make_case_status_brief(item);
  const char* const tokens[] = {"OK ", "WARN ", "FAIL "};
  const char* const keys[] = {"status_ok", "status_warn", "status_fail"};
  for (size_t index = 0; index < 3; ++index) {
    const std::string_view token{tokens[index]};

    // NOLINTNEXTLINE(modernize-use-starts-ends-with)
    if (brief.compare(0, token.size(), token) == 0) {
      std::string out = "<span data-i18n=\"";
      out += keys[index];
      out += "\">";
      out.append(token.data(), token.size() - 1);
      out += "</span> ";
      out += escape_html(brief.substr(token.size()));
      return out;
    }
  }

  return escape_html(brief);
}

std::string summarize_config_html(const AggregatedCase& item) {
  std::ostringstream stream;
  stream << "<div><strong data-i18n=\"insight_url_label\">url</strong>: <code>" << escape_html(item.scenario.url)
         << "</code></div>";
  if (item.transport == "shm2" && item.wire_size != 0) {
    stream << "<div><strong>slice</strong>: <code>" << escape_html(format_size_label(item.wire_size))
           << "</code></div>";
  }
  stream << "<div><strong data-i18n=\"insight_topology_label\">topology</strong>: <code>"
         << escape_html(make_topology_label(item.scenario)) << "</code></div>";
  stream << "<div><strong data-i18n=\"insight_pattern_label\">pattern</strong>: <code>"
         << escape_html(Bench::rate_pattern_to_string(item.scenario.rate_pattern)) << "</code></div>";

  if (!item.scenario.properties.empty()) {
    stream << "<div><strong data-i18n=\"config_node_label\">node</strong>: <code>"
           << escape_html(format_property_list(item.scenario.properties)) << "</code></div>";
  }

  if (!item.scenario.pub_properties.empty()) {
    stream << "<div><strong data-i18n=\"config_pub_label\">pub</strong>: <code>"
           << escape_html(format_property_list(item.scenario.pub_properties)) << "</code></div>";
  }

  if (!item.scenario.sub_properties.empty()) {
    stream << "<div><strong data-i18n=\"config_sub_label\">sub</strong>: <code>"
           << escape_html(format_property_list(item.scenario.sub_properties)) << "</code></div>";
  }

  return stream.str();
}

std::string summarize_endpoint_text(const AggregatedCase& item) {
  std::ostringstream stream;
  stream << Bench::mode_to_string(item.scenario.mode) << " | " << item.scenario.url;

  if VUNLIKELY (!item.scenario.qos_profile.empty()) {
    stream << " | qos=" << item.scenario.qos_profile;
  }

  if (!item.scenario.properties.empty()) {
    stream << " | node=" << format_property_list(item.scenario.properties);
  }

  if (!item.scenario.pub_properties.empty()) {
    stream << " | pub=" << format_property_list(item.scenario.pub_properties);
  }

  if (!item.scenario.sub_properties.empty()) {
    stream << " | sub=" << format_property_list(item.scenario.sub_properties);
  }

  return stream.str();
}

double compute_offered_msg_rate(const AggregatedCase& item) {
  if (item.scenario.rate_pattern == Bench::kMaxRatePattern || item.scenario.rate_hz <= 0) {
    return 0.0;
  }

  const double burst_messages = item.scenario.rate_pattern == Bench::kBurstRatePattern
                                    ? static_cast<double>(std::max(item.scenario.burst_messages, 1))
                                    : 1.0;
  return static_cast<double>(item.scenario.rate_hz) * burst_messages;
}

const char* get_status_class(const AggregatedCase& item) {
  if (item.all_success() && !has_delivery_loss(item)) {
    return "ok";
  }

  if (item.success_count == 0 || item.failure_count != 0) {
    return "fail";
  }

  return "warn";
}

int metric_value_precision(double value) {
  if (value >= 100.0) {
    return 0;
  }

  if (value >= 10.0) {
    return 1;
  }

  return 2;
}

std::string make_series_key(const AggregatedCase& item, SeriesRateLabelMode rate_label_mode, bool include_payload_size,
                            bool include_topology_counts) {
  std::ostringstream stream;
  stream << item.transport;
  stream << " | " << Bench::payload_to_string(item.scenario.payload);

  if (include_payload_size) {
    stream << " | size=" << format_size_label(item.scenario.payload_size);
  }

  stream << " | " << Bench::mode_to_string(item.scenario.mode);
  stream << " | "
         << (include_topology_counts ? make_topology_label(item.scenario)
                                     : Bench::topology_to_string(item.scenario.topology));

  if (!include_topology_counts && item.scenario.topology == Bench::kManyToManyTopology) {
    stream << " | publishers=" << item.scenario.publishers;
  }

  stream << " | url=" << item.scenario.url;

  if VUNLIKELY (!item.scenario.qos_profile.empty()) {
    stream << " | qos=" << item.scenario.qos_profile;
  }

  if (!item.scenario.properties.empty()) {
    stream << " | node=" << format_property_list(item.scenario.properties);
  }

  if (!item.scenario.pub_properties.empty()) {
    stream << " | pub=" << format_property_list(item.scenario.pub_properties);
  }

  if (!item.scenario.sub_properties.empty()) {
    stream << " | sub=" << format_property_list(item.scenario.sub_properties);
  }

  if (rate_label_mode == SeriesRateLabelMode::kIncludePattern && item.scenario.rate_pattern != Bench::kMaxRatePattern) {
    stream << " | pattern=" << Bench::rate_pattern_to_string(item.scenario.rate_pattern);
  } else if (rate_label_mode == SeriesRateLabelMode::kIncludeRate) {
    stream << " | " << format_rate_label(item.scenario);
  }

  return stream.str();
}

std::string make_series_label(const AggregatedCase& item, SeriesRateLabelMode rate_label_mode,
                              bool include_payload_size, bool include_topology_counts) {
  std::ostringstream stream;
  stream << item.transport;
  stream << " | " << Bench::payload_to_string(item.scenario.payload);

  if (include_payload_size) {
    stream << " | " << format_size_label(item.scenario.payload_size);
  }

  stream << " | " << Bench::mode_to_string(item.scenario.mode);
  stream << " | "
         << (include_topology_counts ? make_topology_label(item.scenario)
                                     : Bench::topology_to_string(item.scenario.topology));

  if (!include_topology_counts && item.scenario.topology == Bench::kManyToManyTopology) {
    stream << " | publishers=" << item.scenario.publishers;
  }

  stream << " | url=" << item.scenario.url;

  if (rate_label_mode == SeriesRateLabelMode::kIncludePattern && item.scenario.rate_pattern != Bench::kMaxRatePattern) {
    stream << " | pattern=" << Bench::rate_pattern_to_string(item.scenario.rate_pattern);
  } else if (rate_label_mode == SeriesRateLabelMode::kIncludeRate) {
    stream << " | " << format_rate_label(item.scenario);
  }

  return stream.str();
}

inline std::string json_escape_for_script(const std::string& input) {
  std::string output;
  output.reserve(input.size() + 8);
  for (unsigned char c : input) {
    switch (c) {
      case '"':
        output += "\\\"";
        break;
      case '\\':
        output += "\\\\";
        break;
      case '\b':
        output += "\\b";
        break;
      case '\f':
        output += "\\f";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      case '<':
        output += "\\u003c";
        break;
      case '>':
        output += "\\u003e";
        break;
      case '&':
        output += "\\u0026";
        break;
      case '/':
        output += "\\u002f";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
          output += buf;
        } else {
          output += static_cast<char>(c);
        }
    }
  }

  return output;
}

inline std::string json_number(double value) {
  if (!std::isfinite(value)) {
    return "null";
  }

  char buf[40];
  std::snprintf(buf, sizeof(buf), "%.10g", value);
  return std::string(buf);
}

inline int next_chart_id() {
  static int counter = 0;
  return ++counter;
}

struct ChartI18n final {
  const char* title_key = nullptr;
  const char* x_label_key = nullptr;
  const char* y_label_key = nullptr;
};

template <typename FilterFnT, typename XFnT, typename YFnT>
std::string build_line_chart(const std::vector<AggregatedCase>& items, std::string title, std::string x_label,
                             std::string y_label, FilterFnT&& filter_fn, XFnT&& x_fn, YFnT&& y_fn,
                             SeriesRateLabelMode rate_label_mode, bool include_payload_size = false,
                             ChartI18n i18n_keys = {}, bool include_topology_counts = true) {
  std::map<std::string, ChartSeries> series_map;
  std::vector<double> all_x;
  double max_y = 0.0;
  double min_y_positive = std::numeric_limits<double>::max();
  static const std::array<std::string, 16> kColors = {"#1f77b4", "#d62728", "#2ca02c", "#ff7f0e", "#9467bd", "#8c564b",
                                                      "#e377c2", "#17becf", "#bcbd22", "#000000", "#7f0000", "#005f5f",
                                                      "#5f005f", "#a05a00", "#003d7a", "#8b6914"};
  static const std::array<std::string, 16> kDashes = {
      "",         "10 4",    "4 4",      "2 3", "14 5 4 5", "6 4 2 4", "18 6",     "1 3",
      "12 3 3 3", "8 3 2 3", "16 4 4 4", "6 6", "10 2",     "3 3 8 3", "20 4 2 4", "5 2 2 2 2 2"};

  for (const auto& item : items) {
    if (!filter_fn(item) || !item.all_success()) {
      continue;
    }

    double x = x_fn(item);
    double y = y_fn(item);

    if (y < 0.0) {
      continue;
    }

    max_y = std::max(max_y, y);

    if (y > 0.0) {
      min_y_positive = std::min(min_y_positive, y);
    }

    all_x.emplace_back(x);

    auto key = make_series_key(item, rate_label_mode, include_payload_size, include_topology_counts);
    auto& series = series_map[key];
    series.label = make_series_label(item, rate_label_mode, include_payload_size, include_topology_counts);
    series.detail = key;

    if (item.url_order_index >= 0 && item.url_order_index < series.url_order_index) {
      series.url_order_index = item.url_order_index;
    }

    series.points[x].add(y);
  }

  if VUNLIKELY (series_map.empty()) {
    return std::string();
  }

  std::vector<ChartSeries*> series_order;
  series_order.reserve(series_map.size());
  for (auto& [key, series] : series_map) {
    (void)key;
    series_order.push_back(&series);
  }
  std::sort(series_order.begin(), series_order.end(), [](const ChartSeries* a, const ChartSeries* b) {
    return std::tuple{a->url_order_index, a->detail} < std::tuple{b->url_order_index, b->detail};
  });

  size_t series_index = 0;
  for (auto* series : series_order) {
    ++series_index;
    series->id = "S" + std::to_string(series_index);
    series->color = kColors.at((series_index - 1) % kColors.size());
    series->dash = kDashes.at(((series_index - 1) / kColors.size()) % kDashes.size());
  }

  std::sort(all_x.begin(), all_x.end());
  all_x.erase(std::unique(all_x.begin(), all_x.end()), all_x.end());
  const double min_x = all_x.front();
  const double max_x = all_x.back();

  if (max_y <= 0.0) {
    max_y = 1.0;
  }

  if (min_y_positive == std::numeric_limits<double>::max() || min_y_positive <= 0.0) {
    min_y_positive = max_y;
  }

  const bool y_use_log = min_y_positive > 0.0 && max_y > 0.0 && max_y / min_y_positive >= 50.0;
  const double y_axis_min = y_use_log ? std::floor(std::log10(min_y_positive)) : 0.0;
  const double y_axis_max = y_use_log ? std::ceil(std::log10(max_y)) : max_y;
  const double y_axis_span = std::max(y_axis_max - y_axis_min, 1e-9);

  const bool x_use_log = min_x > 0.0 && max_x > 0.0 && max_x / min_x >= 50.0;
  auto to_y_axis = [y_use_log](double v) {
    if (y_use_log) {
      return std::log10(std::max(v, 1e-9));
    }

    return v;
  };

  static constexpr double kWidth = 960.0;
  static constexpr double kHeight = 420.0;
  static constexpr double kLeft = 72.0;
  static constexpr double kRight = 20.0;
  static constexpr double kTop = 30.0;
  static constexpr double kBottom = 70.0;

  const double plot_width = kWidth - kLeft - kRight;
  const double plot_height = kHeight - kTop - kBottom;

  auto map_y = [&plot_height, &y_use_log, &to_y_axis, &y_axis_min, &y_axis_span, &max_y](double value) {
    if VUNLIKELY (!std::isfinite(value)) {
      return kTop + plot_height;
    }

    double mapped;

    if (y_use_log) {
      const double axis = to_y_axis(value);
      mapped = kTop + plot_height * (1.0 - (axis - y_axis_min) / y_axis_span);
    } else {
      mapped = kTop + plot_height * (1.0 - value / max_y);
    }

    if VUNLIKELY (!std::isfinite(mapped)) {
      return kTop + plot_height;
    }

    return std::clamp(mapped, kTop, kTop + plot_height);
  };

  const int chart_id = next_chart_id();
  std::ostringstream svg;
  svg << R"(<svg id="chart-)" << chart_id
      << R"(" class="bench-chart" viewBox="0 0 960 420" preserveAspectRatio="xMidYMid meet")"
      << R"( width="100%" height="420" xmlns="http://www.w3.org/2000/svg")"
      << R"( data-plot-left="72" data-plot-right="20" data-plot-top="30" data-plot-bottom="70">)";
  svg << R"(<defs>)";
  svg << R"(<clipPath id="chart-)" << chart_id << R"(-clip"><rect x=")" << kLeft << R"(" y=")" << kTop << R"(" width=")"
      << plot_width << R"(" height=")" << plot_height << R"("/></clipPath>)";
  svg << R"(<linearGradient id="chart-)" << chart_id << R"(-bg" x1="0" y1="0" x2="0" y2="1">)"
      << R"(<stop offset="0%" stop-color="#ffffff"/>)"
      << R"(<stop offset="100%" stop-color="#f7f9fc"/></linearGradient>)";
  svg << R"(<filter id="chart-)" << chart_id << R"(-shadow" x="-10%" y="-10%" width="120%" height="120%">)"
      << R"(<feGaussianBlur in="SourceAlpha" stdDeviation="1.2"/>)"
      << R"(<feOffset dx="0" dy="0.8" result="shadow"/>)"
      << R"(<feComponentTransfer><feFuncA type="linear" slope="0.22"/></feComponentTransfer>)"
      << R"(<feMerge><feMergeNode/><feMergeNode in="SourceGraphic"/></feMerge></filter>)";
  svg << R"(</defs>)";

  svg << R"(<rect x="0" y="0" width="960" height="420" rx="14" fill="url(#chart-)" << chart_id
      << R"RAW(-bg)" stroke="#dbe1ec" stroke-width="1"/>)RAW";
  svg << R"(<rect x=")" << (kLeft - 0.5) << R"(" y=")" << (kTop - 0.5) << R"(" width=")" << (plot_width + 1.0)
      << R"(" height=")" << (plot_height + 1.0) << R"(" fill="#ffffff" opacity="0.55"/>)";

  svg << R"(<text x="72" y="20" font-size="16" font-weight="700" fill="#0f172a" letter-spacing="0.01em")";

  if (i18n_keys.title_key) {
    svg << R"( data-i18n=")" << i18n_keys.title_key << R"(")";
  }

  svg << '>' << escape_html(title) << "</text>";

  const int y_tick_count = y_use_log ? std::max(1, static_cast<int>(std::lround(y_axis_span))) : 5;
  for (int index = 0; index <= y_tick_count; ++index) {
    const double t = static_cast<double>(index) / static_cast<double>(y_tick_count);
    const double y = kTop + plot_height * t;
    double value;

    if (y_use_log) {
      value = std::pow(10.0, y_axis_max - y_axis_span * t);
    } else {
      value = max_y * (1.0 - t);
    }

    const bool is_axis_edge = index == y_tick_count;
    svg << R"(<line x1=")" << kLeft << R"(" y1=")" << y << R"(" x2=")" << (kLeft + plot_width) << R"(" y2=")" << y
        << R"(" stroke=")" << (is_axis_edge ? "#cdd5e0" : "#e5eaf2") << R"(" stroke-width=")"
        << (is_axis_edge ? "1.0" : "0.7") << R"(" stroke-dasharray=")" << (is_axis_edge ? "0" : "2 4") << R"("/>)";

    const int precision = metric_value_precision(value);
    svg << R"(<text x=")" << (kLeft - 8) << R"(" y=")" << (y + 4)
        << R"(" font-size="11" text-anchor="end" fill="#64748b" font-weight="500">)"
        << escape_html(format_decimal(value, precision)) << "</text>";
  }

  svg << R"(<line x1=")" << kLeft << R"(" y1=")" << (kTop + plot_height) << R"(" x2=")" << (kLeft + plot_width)
      << R"(" y2=")" << (kTop + plot_height) << R"(" stroke="#475569" stroke-width="1.4"/>)";
  svg << R"(<line x1=")" << kLeft << R"(" y1=")" << kTop << R"(" x2=")" << kLeft << R"(" y2=")" << (kTop + plot_height)
      << R"(" stroke="#475569" stroke-width="1.4"/>)";

  auto to_axis = [x_use_log](double v) {
    if (x_use_log) {
      return std::log2(std::max(v, 1.0));
    }

    return v;
  };
  const double axis_min = to_axis(min_x);
  const double axis_max = to_axis(max_x);

  auto map_x = [&axis_max, &axis_min, &plot_width, &to_axis](double x_value) {
    if (axis_max <= axis_min) {
      return kLeft + plot_width / 2.0;
    }

    return kLeft + plot_width * ((to_axis(x_value) - axis_min) / (axis_max - axis_min));
  };

  const size_t tick_count = all_x.size();
  const double min_tick_gap_px = 56.0;
  const size_t max_labels =
      tick_count == 0 ? 0 : std::max<size_t>(2, static_cast<size_t>(plot_width / min_tick_gap_px));
  const size_t label_stride = tick_count == 0 ? 1 : std::max<size_t>(1, (tick_count + max_labels - 1) / max_labels);

  for (size_t tick_index = 0; tick_index < tick_count; ++tick_index) {
    const double x_value = all_x[tick_index];
    const double x = map_x(x_value);

    svg << R"(<line x1=")" << x << R"(" y1=")" << (kTop + plot_height) << R"(" x2=")" << x << R"(" y2=")"
        << (kTop + plot_height + 4) << R"(" stroke="#475569"/>)";

    const bool show_label = tick_index % label_stride == 0 || tick_index + 1 == tick_count;

    if (!show_label) {
      continue;
    }

    std::string label = format_decimal(x_value, 0);

    if (x_label.find("Payload") != std::string::npos) {
      label = format_size_label(static_cast<size_t>(std::llround(x_value)));
    }

    svg << R"(<text x=")" << x << R"(" y=")" << (kTop + plot_height + 20)
        << R"(" font-size="11" text-anchor="middle" fill="#475569">)" << escape_html(label) << "</text>";
  }

  for (const auto* series_ptr : series_order) {
    const auto& series = *series_ptr;
    std::vector<std::pair<double, double>> pts;
    pts.reserve(series.points.size());

    for (const auto& [x_value, metric] : series.points) {
      const double x = map_x(x_value);
      const double y = map_y(metric.average());
      pts.emplace_back(x, y);
    }

    if (pts.empty()) {
      continue;
    }

    std::ostringstream polyline;

    if (pts.size() == 1) {
      constexpr double kSingletonTick = 10.0;
      const auto [x, y] = pts.front();
      polyline << (x - kSingletonTick) << "," << y << ' ' << (x + kSingletonTick) << "," << y << ' ';
    } else {
      for (const auto& [x, y] : pts) {
        polyline << x << "," << y << ' ';
      }
    }

    const auto& color = series.color;

    svg << R"(<polyline class="chart-line" data-series-id=")" << escape_html(series.id) << R"(" filter="url(#chart-)"
        << chart_id << R"RAW(-shadow)" fill="none" stroke=")RAW" << color
        << R"(" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" stroke-opacity="0.95")";

    if (!series.dash.empty()) {
      svg << " stroke-dasharray=\"" << series.dash << "\"";
    }

    svg << " points=\"" << polyline.str() << "\"/>";

    for (const auto& [x_value, metric] : series.points) {
      const double x = map_x(x_value);
      const double y = map_y(metric.average());

      svg << R"(<circle class="chart-point-halo" cx=")" << x << R"(" cy=")" << y << R"(" r="5.5" fill=")" << color
          << R"(" fill-opacity="0.14" stroke="none"/>)";
      svg << R"(<circle class="chart-point" data-series-id=")" << escape_html(series.id) << R"(" cx=")" << x
          << R"(" cy=")" << y << R"(" r="3.0" fill=")" << color << R"(" stroke="#ffffff" stroke-width="1.2"/>)";
    }
  }

  svg << R"(<text x=")" << (kLeft + plot_width / 2.0) << R"(" y=")" << (kHeight - 18.0)
      << R"(" font-size="12" text-anchor="middle" fill="#334155")";

  if (i18n_keys.x_label_key) {
    svg << R"( data-i18n=")" << i18n_keys.x_label_key << R"(")";
  }

  svg << '>' << escape_html(x_label) << "</text>";
  svg << R"(<text x="18" y=")" << (kTop + plot_height / 2.0)
      << R"(" font-size="12" text-anchor="middle" fill="#334155" transform="rotate(-90 18 )"
      << (kTop + plot_height / 2.0) << R"DELIM()")DELIM";

  if (i18n_keys.y_label_key) {
    svg << R"( data-i18n=")" << i18n_keys.y_label_key << R"(")";
  }

  svg << '>' << escape_html(y_label) << "</text>";

  svg << R"(<g class="chart-crosshair" style="display:none;pointer-events:none">)"
      << R"(<line class="chart-crosshair-line" x1="0" y1=")" << kTop << R"(" x2="0" y2=")" << (kTop + plot_height)
      << R"(" stroke="#345af6" stroke-width="1" stroke-dasharray="4 3" opacity="0.75"/></g>)";
  svg << "</svg>";

  std::ostringstream data_json;
  data_json << "{\"id\":" << chart_id << R"(,"title":")" << json_escape_for_script(title) << R"(","xLabel":")"
            << json_escape_for_script(x_label) << R"(","yLabel":")" << json_escape_for_script(y_label)
            << R"(","xIsPayload":)" << (x_label.find("Payload") != std::string::npos ? "true" : "false")
            << R"(,"xScale":")" << (x_use_log ? "log2" : "linear") << "\""
            << R"(,"yScale":")" << (y_use_log ? "log10" : "linear") << "\""
            << R"(,"plot":{"left":)" << kLeft << ",\"top\":" << kTop << ",\"width\":" << plot_width
            << ",\"height\":" << plot_height << ",\"maxY\":" << json_number(max_y)
            << ",\"minY\":" << json_number(min_y_positive) << ",\"axisMinY\":" << json_number(y_axis_min)
            << ",\"axisMaxY\":" << json_number(y_axis_max) << ",\"minX\":" << json_number(min_x)
            << ",\"maxX\":" << json_number(max_x) << ",\"axisMinX\":" << json_number(axis_min)
            << ",\"axisMaxX\":" << json_number(axis_max) << "},\"series\":[";
  bool first_series = true;
  for (const auto* series_ptr : series_order) {
    const auto& series = *series_ptr;

    if (!first_series) {
      data_json << ",";
    }

    first_series = false;
    data_json << R"({"id":")" << json_escape_for_script(series.id) << R"(","color":")"
              << json_escape_for_script(series.color) << R"(","label":")" << json_escape_for_script(series.label)
              << R"(","detail":")" << json_escape_for_script(series.detail) << R"(","points":[)";
    bool first_point = true;
    for (const auto& [x_value, metric] : series.points) {
      if (!first_point) {
        data_json << ",";
      }

      first_point = false;
      data_json << "{\"x\":" << json_number(x_value) << ",\"y\":" << json_number(metric.average()) << "}";
    }

    data_json << "]}";
  }

  data_json << "]}";

  std::ostringstream body;
  body << R"(<div class="chart-card" data-chart-id=")" << chart_id << R"(">)";
  body
      << R"(<div class="chart-toolbar" role="toolbar" data-i18n-aria="aria_chart_controls" aria-label="Chart controls">)"
      << R"(<button type="button" class="chart-btn" data-chart-action="zoom-in" )"
      << R"(data-i18n-title="chart_zoom_in" data-i18n-aria="chart_zoom_in" )"
      << R"(title="Zoom in" aria-label="Zoom in">+</button>)"
      << R"(<button type="button" class="chart-btn" data-chart-action="zoom-out" )"
      << R"(data-i18n-title="chart_zoom_out" data-i18n-aria="chart_zoom_out" )"
      << R"(title="Zoom out" aria-label="Zoom out">-</button>)"
      << R"(<button type="button" class="chart-btn" data-chart-action="reset" )"
      << R"(data-i18n-title="chart_reset" data-i18n-aria="chart_reset" data-i18n="chart_reset_short" )"
      << R"(title="Reset view" aria-label="Reset view">Reset</button>)"
      << R"(<span class="chart-hint" aria-hidden="true" data-i18n="chart_hint">wheel=zoom &middot; drag=pan &middot; dblclick=reset</span>)"
      << R"(</div>)";
  body << "<div class=\"chart-scroll\">" << svg.str() << "</div>";
  body << R"(<script type="application/json" class="chart-data" data-chart-id=")" << chart_id << R"(">)"
       << data_json.str() << "</script>";
  body << R"(<div class="legend-map" role="group" data-i18n-aria="aria_series_legend" aria-label="Series legend">)";
  for (const auto* series_ptr : series_order) {
    const auto& series = *series_ptr;
    body << R"(<button type="button" class="legend-entry" data-legend-target=")" << escape_html(series.id)
         << R"(" aria-pressed="true">)"
         << R"(<svg class="legend-color" width="24" height="12" )"
         << R"(viewBox="0 0 24 12" xmlns="http://www.w3.org/2000/svg"><line x1="1" y1="6" )"
         << R"(x2="23" y2="6" stroke=")" << escape_html(series.color)
         << R"(" stroke-width="2.0" stroke-linecap="round")";

    if (!series.dash.empty()) {
      body << " stroke-dasharray=\"" << escape_html(series.dash) << "\"";
    }

    body << "/></svg><span class=\"legend-id\"><code>" << escape_html(series.id) << "</code></span><span title=\""
         << escape_html(series.detail) << "\">" << escape_html(series.label) << "</span></button>";
  }

  body << "</div>";
  body << "</div>";
  return body.str();
}

std::string build_transport_summary_table(const std::vector<AggregatedCase>& items) {
  std::ostringstream html;
  html << "<div class=\"table-scroll\"><table class=\"bench-table "
          "cardable\"><thead><tr><th data-i18n=\"th_suite\">Category</th><th data-i18n=\"th_mode\">Mode</th>"
          "<th data-i18n=\"th_transport\">Transport</th><th data-i18n=\"th_payload\">Payload</th>"
       << "<th data-i18n=\"th_config\">Config</th><th data-i18n=\"th_rate\">Rate</th>"
          "<th data-i18n=\"th_repeats\">Repeats</th>"
          "<th data-i18n=\"th_recv_mbps\">Received MB/s</th>"
          "<th data-i18n=\"th_mean_p95\">Mean P95 us</th>"
          "<th data-i18n=\"th_mean_p999\">Mean P99.9 us</th>"
          "<th data-i18n=\"th_send_block_p99\">Send-block P99 us</th>"
          "<th data-i18n=\"th_loss_pct\">Loss %</th>"
          "<th data-i18n=\"th_cpu_pct\">CPU %</th>"
          "<th data-i18n=\"th_peak_rss_mb\">Peak RSS MB</th>"
          "<th data-i18n=\"th_status\">Status</th></tr></thead><tbody>";

  for (const auto& item : items) {
    if (is_serialization_case(item)) {
      continue;
    }

    html << R"(<tr><td data-i18n-label="th_suite" data-label="Category">)"
         << escape_html(Bench::suite_to_string(item.scenario.suite))
         << R"(</td><td data-i18n-label="th_mode" data-label="Mode">)"
         << escape_html(Bench::mode_to_string(item.scenario.mode))
         << R"(</td><td data-i18n-label="th_transport" data-label="Transport">)" << escape_html(item.transport)
         << R"(</td><td data-i18n-label="th_payload" data-label="Payload">)"
         << escape_html(Bench::payload_to_string(item.scenario.payload)) << " / "
         << escape_html(format_size_label(item.scenario.payload_size))
         << R"(</td><td data-i18n-label="th_config" data-label="Config" class="config-cell">)"
         << summarize_config_html(item) << R"(</td><td data-i18n-label="th_rate" data-label="Rate"><code>)"
         << escape_html(format_rate_label(item.scenario))
         << R"(</code></td><td data-i18n-label="th_repeats" data-label="Repeats">)" << item.success_count << '/'
         << item.sample_count << R"(</td><td data-i18n-label="th_recv_mbps" data-label="Received MB/s">)"
         << format_metric_cell(item.recv_mb_per_sec)
         << R"(</td><td data-i18n-label="th_mean_p95" data-label="Mean P95 us">)"
         << format_metric_cell(item.p95_latency_us)
         << R"(</td><td data-i18n-label="th_mean_p999" data-label="Mean P99.9 us">)"
         << format_metric_cell(item.p999_latency_us)
         << R"(</td><td data-i18n-label="th_send_block_p99" data-label="Send-block P99 us">)"
         << format_metric_cell(item.p99_send_block_us)
         << R"(</td><td data-i18n-label="th_loss_pct" data-label="Loss %">)" << format_loss_ratio_cell(item)
         << R"(</td><td data-i18n-label="th_cpu_pct" data-label="CPU %">)" << format_metric_cell(item.cpu_usage)
         << R"(</td><td data-i18n-label="th_peak_rss_mb" data-label="Peak RSS MB">)"
         << format_memory_cell(item.memory_usage)
         << R"(</td><td data-i18n-label="th_status" data-label="Status" class=")" << get_status_class(item) << "\">"
         << wrap_status_brief_html(item) << "</td></tr>";
  }

  html << "</tbody></table></div>";
  return html.str();
}

std::string build_serialization_summary_table(const std::vector<AggregatedCase>& items) {
  std::ostringstream html;
  html << "<div class=\"table-scroll\"><table class=\"bench-table "
          "cardable\"><thead><tr><th data-i18n=\"th_suite\">Category</th><th data-i18n=\"th_mode\">Mode</th><th "
          "data-i18n=\"th_transport\">Transport</th><th data-i18n=\"th_payload\">Payload</th>"
       << "<th data-i18n=\"th_config\">Config</th><th data-i18n=\"th_repeats\">Repeats</th>"
          "<th data-i18n=\"th_encode_mbps\">Encode MB/s</th><th data-i18n=\"th_decode_mbps\">Decode MB/s</th>"
          "<th data-i18n=\"th_mean_encode_cpu_ms\">Encode CPU ms</th>"
          "<th data-i18n=\"th_mean_decode_cpu_ms\">Decode CPU ms</th>"
          "<th data-i18n=\"th_status\">Status</th></tr></thead><tbody>";

  for (const auto& item : items) {
    if (!is_serialization_case(item)) {
      continue;
    }

    html << R"(<tr><td data-i18n-label="th_suite" data-label="Category">)"
         << escape_html(Bench::suite_to_string(item.scenario.suite))
         << R"(</td><td data-i18n-label="th_mode" data-label="Mode">)"
         << escape_html(Bench::mode_to_string(item.scenario.mode))
         << R"(</td><td data-i18n-label="th_transport" data-label="Transport">)" << escape_html(item.transport)
         << R"(</td><td data-i18n-label="th_payload" data-label="Payload">)"
         << escape_html(Bench::payload_to_string(item.scenario.payload)) << " / "
         << escape_html(format_size_label(item.scenario.payload_size))
         << R"(</td><td data-i18n-label="th_config" data-label="Config" class="config-cell">)"
         << summarize_config_html(item) << R"(</td><td data-i18n-label="th_repeats" data-label="Repeats">)"
         << item.success_count << '/' << item.sample_count
         << R"(</td><td data-i18n-label="th_encode_mbps" data-label="Encode MB/s">)"
         << format_metric_cell(item.serialize_mb_per_sec)
         << R"(</td><td data-i18n-label="th_decode_mbps" data-label="Decode MB/s">)"
         << format_metric_cell(item.deserialize_mb_per_sec)
         << R"(</td><td data-i18n-label="th_mean_encode_cpu_ms" data-label="Encode CPU ms">)"
         << format_metric_cell(item.pub_cpu_ms)
         << R"(</td><td data-i18n-label="th_mean_decode_cpu_ms" data-label="Decode CPU ms">)"
         << format_metric_cell(item.sub_cpu_ms) << R"(</td><td data-i18n-label="th_status" data-label="Status" class=")"
         << get_status_class(item) << "\">" << wrap_status_brief_html(item) << "</td></tr>";
  }

  html << "</tbody></table></div>";
  return html.str();
}

template <typename FilterFnT, typename ScoreFnT>
const AggregatedCase* find_max_case(const std::vector<AggregatedCase>& items, FilterFnT&& filter_fn,
                                    ScoreFnT&& score_fn) {
  const AggregatedCase* best = nullptr;
  double best_score = 0.0;

  for (const auto& item : items) {
    if (!item.all_success() || has_delivery_loss(item) || !filter_fn(item)) {
      continue;
    }

    const double score = score_fn(item);

    if (best == nullptr || score > best_score) {
      best = &item;
      best_score = score;
    }
  }

  return best;
}

template <typename FilterFnT, typename ScoreFnT>
const AggregatedCase* find_min_case(const std::vector<AggregatedCase>& items, FilterFnT&& filter_fn,
                                    ScoreFnT&& score_fn) {
  const AggregatedCase* best = nullptr;
  double best_score = 0.0;

  for (const auto& item : items) {
    if (!item.all_success() || has_delivery_loss(item) || !filter_fn(item)) {
      continue;
    }

    const double score = score_fn(item);

    if (best == nullptr || score < best_score) {
      best = &item;
      best_score = score;
    }
  }

  return best;
}

std::string build_case_insight_html(const AggregatedCase& item, const char* metric_label_key,
                                    std::string_view metric_label_en, const std::string& metric_value_html) {
  std::ostringstream html;
  html << "<div><strong data-i18n=\"" << metric_label_key << "\">" << escape_html(std::string(metric_label_en))
       << "</strong>: <code>" << escape_html(item.transport) << " | "
       << escape_html(Bench::mode_to_string(item.scenario.mode)) << " | "
       << escape_html(make_topology_label(item.scenario)) << " | "
       << escape_html(Bench::payload_to_string(item.scenario.payload)) << " | "
       << escape_html(format_size_label(item.scenario.payload_size)) << "</code></div>";
  html << "<div>" << metric_value_html << " | <span data-i18n=\"insight_repeats_label\">repeats</span> "
       << item.success_count << "/" << item.sample_count << "</div>";
  html << "<div><span data-i18n=\"insight_url_label\">url</span>: <code>" << escape_html(item.scenario.url)
       << "</code></div>";
  return html.str();
}

std::string format_loss_insight_suffix(const AggregatedCase& item) {
  const double loss_percent = compute_loss_ratio_percent(item);

  if (!should_report_delivery_loss(loss_percent)) {
    return std::string();
  }

  return " | <span data-i18n=\"insight_loss_label\">loss</span> " + escape_html(format_decimal(loss_percent, 3)) + "%";
}

const char* get_score_class(double score, double max_score = 100.0) {
  if (score < 0.0) {
    return "muted";
  }

  if (score >= max_score * 0.80) {
    return "ok";
  }

  if (score >= max_score * 0.60) {
    return "warn";
  }

  return "fail";
}

std::string format_score_cell(double score, int precision = 1) {
  if (score < 0.0 || !std::isfinite(score)) {
    return "-";
  }

  return escape_html(format_decimal(score, precision));
}

double increase_score_contrast(double score) {
  const double normalized = std::clamp(score / 100.0, 0.0, 1.0);
  return clamp_score(std::pow(normalized, 1.20) * 100.0);
}

double normalize_higher_score(double value, const std::vector<double>& peers) {
  if (peers.empty()) {
    return 100.0;
  }

  if (peers.size() == 1) {
    return 100.0;
  }

  double min_value = std::numeric_limits<double>::max();
  double max_value = std::numeric_limits<double>::lowest();

  for (double peer : peers) {
    min_value = std::min(min_value, peer);
    max_value = std::max(max_value, peer);
  }

  const double min_log = std::log1p(std::max(min_value, 0.0));
  const double max_log = std::log1p(std::max(max_value, 0.0));

  if (std::abs(max_log - min_log) < 1e-9) {
    return 100.0;
  }

  return increase_score_contrast((std::log1p(std::max(value, 0.0)) - min_log) * 100.0 / (max_log - min_log));
}

double normalize_lower_score(double value, const std::vector<double>& peers) {
  if (peers.empty()) {
    return 100.0;
  }

  if (peers.size() == 1) {
    return 100.0;
  }

  double min_value = std::numeric_limits<double>::max();
  double max_value = std::numeric_limits<double>::lowest();

  for (double peer : peers) {
    min_value = std::min(min_value, peer);
    max_value = std::max(max_value, peer);
  }

  const double min_log = std::log1p(std::max(min_value, 0.0));
  const double max_log = std::log1p(std::max(max_value, 0.0));

  if (std::abs(max_log - min_log) < 1e-9) {
    return 100.0;
  }

  return increase_score_contrast((max_log - std::log1p(std::max(value, 0.0))) * 100.0 / (max_log - min_log));
}

double compute_scale_efficiency_mb_per_sec(const AggregatedCase& item) {
  double divisor = 1.0;

  switch (item.scenario.topology) {
    case Bench::kOneToManyTopology:
      divisor = static_cast<double>(std::max(item.scenario.subscribers, 1));
      break;
    case Bench::kManyToOneTopology:
      divisor = static_cast<double>(std::max(item.scenario.publishers, 1));
      break;
    case Bench::kManyToManyTopology:
      divisor = static_cast<double>(std::max(item.scenario.publishers * item.scenario.subscribers, 1));
      break;
    default:
      divisor = 1.0;
      break;
  }

  if (divisor <= 0.0) {
    divisor = 1.0;
  }

  return item.recv_mb_per_sec.average() / divisor;
}

double normalize_optional_higher_score(bool valid, double value, const std::vector<double>& peers) {
  return valid ? normalize_higher_score(value, peers) : 0.0;
}

double normalize_optional_lower_score(bool valid, double value, const std::vector<double>& peers) {
  return valid ? normalize_lower_score(value, peers) : 0.0;
}

double normalize_optional_lower_score_floored(bool valid, double value, const std::vector<double>& peers,
                                              double floor_value) {
  if (!valid) {
    return 0.0;
  }

  std::vector<double> floored_peers;
  floored_peers.reserve(peers.size());
  for (double peer : peers) {
    floored_peers.push_back(std::max(peer, floor_value));
  }

  return normalize_lower_score(std::max(value, floor_value), floored_peers);
}

double compute_repeat_success_ratio(size_t success_count, size_t sample_count) {
  if (sample_count == 0) {
    return 0.0;
  }

  return static_cast<double>(success_count) / static_cast<double>(sample_count);
}

double compute_delivery_integrity_score(double loss_percent) {
  const double loss_ratio = std::clamp(loss_percent / 100.0, 0.0, 1.0);
  return clamp_score(std::max(0.0, 1.0 - loss_ratio) * 100.0);
}

double compute_transfer_efficiency_score(double recv_mb_per_sec, double send_mb_per_sec) {
  if (send_mb_per_sec <= 0.0) {
    return 0.0;
  }

  return clamp_score(recv_mb_per_sec * 100.0 / send_mb_per_sec);
}

double compute_resource_efficiency(double throughput, double resource_cost) {
  if (throughput <= 0.0) {
    return 0.0;
  }

  if (resource_cost <= 0.0) {
    return throughput;
  }

  return throughput / resource_cost;
}

double compute_serialization_balance(double encode_mb_per_sec, double decode_mb_per_sec) {
  if (encode_mb_per_sec <= 0.0 || decode_mb_per_sec <= 0.0) {
    return 0.0;
  }

  const double max_value = std::max(encode_mb_per_sec, decode_mb_per_sec);

  if (max_value <= 0.0) {
    return 0.0;
  }

  return clamp_score(std::min(encode_mb_per_sec, decode_mb_per_sec) * 100.0 / max_value);
}

double compute_stability_gate(double coverage_ratio, double repeat_ratio) {
  const double quality = std::min(std::clamp(coverage_ratio, 0.0, 1.0), std::clamp(repeat_ratio, 0.0, 1.0));
  return 0.98 + 0.02 * quality;
}

double compute_health_score(double loss_percent, double latency_drop_percent, double coverage_ratio,
                            double repeat_ratio) {
  const double loss_score = compute_delivery_integrity_score(loss_percent);
  const double latency_drop_score = compute_delivery_integrity_score(latency_drop_percent);
  const double coverage_score = std::clamp(coverage_ratio, 0.0, 1.0) * 100.0;
  const double repeat_score = std::clamp(repeat_ratio, 0.0, 1.0) * 100.0;
  return clamp_score(loss_score * 0.45 + latency_drop_score * 0.25 + coverage_score * 0.15 + repeat_score * 0.15);
}

template <typename RowT, typename FnT>
std::vector<double> collect_peer_values(const std::vector<RowT*>& rows, FnT&& value_fn) {
  std::vector<double> peers;

  for (const auto* row : rows) {
    const auto [valid, value] = value_fn(*row);

    if (valid) {
      peers.emplace_back(value);
    }
  }

  return peers;
}

struct TransportScoreRow final {
  std::string endpoint_key;
  std::string transport;
  std::string url;
  std::string endpoint_detail;
  std::string mode;
  std::string suite;
  std::vector<std::string> urls;
  int url_order_index{std::numeric_limits<int>::max()};
  size_t case_count{0};
  size_t passing_case_count{0};
  size_t warning_case_count{0};
  size_t failing_case_count{0};
  size_t repeat_total{0};
  size_t repeat_success{0};
  uint64_t latency_drop_count{0};
  MetricSummary recv_mb_per_sec;
  MetricSummary send_mb_per_sec;
  MetricSummary recv_score;
  MetricSummary first_message_ms;
  MetricSummary discovery_ms;
  MetricSummary p95_latency_us;
  MetricSummary p99_latency_us;
  MetricSummary p999_latency_us;
  MetricSummary p9999_latency_us;
  MetricSummary max_latency_us;
  MetricSummary latency_stddev_us;
  MetricSummary latency_quality_score;
  MetricSummary p99_send_block_us;
  MetricSummary max_latency_score;
  MetricSummary p99_send_block_score;
  MetricSummary cpu_usage;
  MetricSummary memory_usage;
  MetricSummary scale_efficiency_mb_per_sec;
  double expected_sum{0.0};
  double received_sum{0.0};
  double lost_sum{0.0};
  double worst_loss_pct{0.0};
  double worst_latency_drop_pct{0.0};
  double score{-1.0};
};

struct SerializationScoreRow final {
  std::string transport;
  std::string mode;
  std::string payload;
  size_t payload_size{0};
  size_t case_count{0};
  size_t passing_case_count{0};
  size_t warning_case_count{0};
  size_t failing_case_count{0};
  size_t repeat_total{0};
  size_t repeat_success{0};
  MetricSummary serialize_msgs_per_sec;
  MetricSummary deserialize_msgs_per_sec;
  MetricSummary serialize_mb_per_sec;
  MetricSummary deserialize_mb_per_sec;
  MetricSummary pub_cpu_ms;
  MetricSummary sub_cpu_ms;
  MetricSummary memory_usage;
  double score{-1.0};
};

std::vector<TransportScoreRow> build_transport_score_rows(const std::vector<AggregatedCase>& items) {
  std::map<std::tuple<std::string, std::string, std::string>, TransportScoreRow> summary;

  for (const auto& item : items) {
    if (is_serialization_case(item)) {
      continue;
    }

    auto& row = summary[{item.endpoint_key, Bench::mode_to_string(item.scenario.mode),
                         Bench::suite_to_string(item.scenario.suite)}];
    row.endpoint_key = item.endpoint_key;
    row.transport = item.transport;
    row.url = item.scenario.url;
    row.endpoint_detail = summarize_endpoint_text(item);
    row.mode = Bench::mode_to_string(item.scenario.mode);
    row.suite = Bench::suite_to_string(item.scenario.suite);

    if (item.url_order_index >= 0 && item.url_order_index < row.url_order_index) {
      row.url_order_index = item.url_order_index;
    }

    ++row.case_count;
    row.repeat_total += item.sample_count;
    row.repeat_success += item.success_count;
    row.latency_drop_count += item.latency_samples_dropped;
    append_unique(row.urls, item.scenario.url);

    row.expected_sum += item.expected.sum;
    row.received_sum += item.received.sum;
    row.lost_sum += item.lost.sum;
    row.worst_loss_pct = std::max(row.worst_loss_pct, compute_loss_ratio_percent(item));
    row.worst_latency_drop_pct = std::max(row.worst_latency_drop_pct, compute_latency_drop_ratio_percent(item));

    const bool runtime_success = item.success_count > 0 && item.failure_count == 0;
    const bool clean_success = item.all_success() && !has_delivery_loss(item);

    if (clean_success) {
      ++row.passing_case_count;
    } else if (runtime_success) {
      ++row.warning_case_count;
    } else {
      ++row.failing_case_count;
    }

    if (runtime_success) {
      if (item.recv_mb_per_sec.count != 0) {
        const double recv = item.recv_mb_per_sec.average();
        row.recv_mb_per_sec.add(recv);
        row.recv_score.add(compute_absolute_throughput_score(recv));
      }

      if (item.send_mb_per_sec.count != 0) {
        row.send_mb_per_sec.add(item.send_mb_per_sec.average());
      }

      if (item.first_message_ms.count != 0) {
        row.first_message_ms.add(item.first_message_ms.average());
      }

      if (item.discovery_ms.count != 0) {
        row.discovery_ms.add(item.discovery_ms.average());
      }

      if (item.p95_latency_us.count != 0) {
        const double p95 = item.p95_latency_us.average();
        row.p95_latency_us.add(p95);
      }

      if (item.p99_latency_us.count != 0) {
        const double p99 = item.p99_latency_us.average();
        row.p99_latency_us.add(p99);
      }

      if (item.p999_latency_us.count != 0) {
        const double p999 = item.p999_latency_us.average();
        row.p999_latency_us.add(p999);
      }

      if (item.p9999_latency_us.count != 0) {
        const double p9999 = item.p9999_latency_us.average();
        row.p9999_latency_us.add(p9999);
      }

      if (item.max_latency_us.count != 0) {
        const double max_latency = item.max_latency_us.average();
        row.max_latency_us.add(max_latency);
        row.max_latency_score.add(compute_absolute_latency_score(max_latency, item.scenario.payload_size));
      }

      if (item.latency_stddev_us.count != 0) {
        row.latency_stddev_us.add(item.latency_stddev_us.average());
      }

      if (item.scenario.suite == Bench::kLatencySuite && score_latency_us(item) > 0.0) {
        row.latency_quality_score.add(score_latency_quality(item));
      }

      if (item.p99_send_block_us.count != 0) {
        const double p99_send_block = item.p99_send_block_us.average();
        row.p99_send_block_us.add(p99_send_block);
        row.p99_send_block_score.add(compute_absolute_send_block_score(p99_send_block, item.scenario.payload_size));
      }

      if (item.cpu_usage.count != 0) {
        row.cpu_usage.add(item.cpu_usage.average());
      }

      if (item.memory_usage.count != 0) {
        row.memory_usage.add(item.memory_usage.average());
      }

      if (item.scenario.suite == Bench::kTopologySuite && item.recv_mb_per_sec.count != 0) {
        row.scale_efficiency_mb_per_sec.add(compute_scale_efficiency_mb_per_sec(item));
      }
    }
  }

  std::vector<TransportScoreRow> rows;
  rows.reserve(summary.size());
  for (auto& [_, row] : summary) {
    rows.emplace_back(std::move(row));
  }

  std::map<std::pair<std::string, std::string>, std::vector<TransportScoreRow*>> cohorts;
  for (auto& row : rows) {
    cohorts[{row.mode, row.suite}].emplace_back(&row);
  }

  for (const auto& [_, cohort] : cohorts) {
    const auto recv_peers = collect_peer_values<TransportScoreRow>(cohort, [](const TransportScoreRow& row) {
      return std::pair{row.recv_mb_per_sec.count != 0, row.recv_mb_per_sec.average()};
    });
    const auto transfer_efficiency_peers =
        collect_peer_values<TransportScoreRow>(cohort, [](const TransportScoreRow& row) {
          const bool valid =
              row.recv_mb_per_sec.count != 0 && row.send_mb_per_sec.count != 0 && row.send_mb_per_sec.average() > 0.0;
          return std::pair{
              valid, compute_transfer_efficiency_score(row.recv_mb_per_sec.average(), row.send_mb_per_sec.average())};
        });
    const auto max_latency_peers = collect_peer_values<TransportScoreRow>(cohort, [](const TransportScoreRow& row) {
      return std::pair{row.max_latency_us.count != 0, row.max_latency_us.average()};
    });
    const auto jitter_peers = collect_peer_values<TransportScoreRow>(cohort, [](const TransportScoreRow& row) {
      return std::pair{row.latency_stddev_us.count != 0, row.latency_stddev_us.average()};
    });
    const auto latency_quality_peers = collect_peer_values<TransportScoreRow>(cohort, [](const TransportScoreRow& row) {
      return std::pair{row.latency_quality_score.count != 0, row.latency_quality_score.average()};
    });
    const auto send_block_peers = collect_peer_values<TransportScoreRow>(cohort, [](const TransportScoreRow& row) {
      return std::pair{row.p99_send_block_us.count != 0, row.p99_send_block_us.average()};
    });
    const auto scale_efficiency_peers =
        collect_peer_values<TransportScoreRow>(cohort, [](const TransportScoreRow& row) {
          return std::pair{row.scale_efficiency_mb_per_sec.count != 0, row.scale_efficiency_mb_per_sec.average()};
        });

    for (auto* row : cohort) {
      if (cohort.size() < 2) {
        row->score = -1.0;
        continue;
      }

      const size_t scored_case_count = row->passing_case_count + row->warning_case_count;

      if (scored_case_count == 0 || row->repeat_success == 0) {
        row->score = 0.0;
        continue;
      }

      const double coverage_ratio =
          row->case_count == 0 ? 0.0 : static_cast<double>(scored_case_count) / static_cast<double>(row->case_count);
      const double repeat_ratio = compute_repeat_success_ratio(row->repeat_success, row->repeat_total);
      const double main_coverage = coverage_ratio * repeat_ratio;
      const double aggregate_loss_percent = row->expected_sum <= 0.0 ? 0.0 : row->lost_sum * 100.0 / row->expected_sum;
      const double loss_percent = std::max(aggregate_loss_percent, row->worst_loss_pct);
      const double recv_value = row->recv_mb_per_sec.average();
      const double recv_score = blend_absolute_relative_score(
          row->recv_score.count != 0 ? row->recv_score.average() : compute_absolute_throughput_score(recv_value),
          normalize_optional_higher_score(row->recv_mb_per_sec.count != 0, recv_value, recv_peers));
      const double transfer_efficiency_score = normalize_optional_higher_score(
          row->recv_mb_per_sec.count != 0 && row->send_mb_per_sec.count != 0 && row->send_mb_per_sec.average() > 0.0,
          compute_transfer_efficiency_score(row->recv_mb_per_sec.average(), row->send_mb_per_sec.average()),
          transfer_efficiency_peers);
      const double max_latency_value = row->max_latency_us.average();
      const double max_latency_score = blend_absolute_relative_score(
          row->max_latency_score.count != 0 ? row->max_latency_score.average() : 0.0,
          normalize_optional_lower_score(row->max_latency_us.count != 0, max_latency_value, max_latency_peers));
      const double jitter_score = normalize_optional_lower_score(row->latency_stddev_us.count != 0,
                                                                 row->latency_stddev_us.average(), jitter_peers);
      const double latency_quality_value = row->latency_quality_score.average();
      const double latency_quality_score =
          blend_absolute_relative_score(row->latency_quality_score.count != 0 ? latency_quality_value : 0.0,
                                        normalize_optional_higher_score(row->latency_quality_score.count != 0,
                                                                        latency_quality_value, latency_quality_peers));
      const double send_block_value = row->p99_send_block_us.average();
      const double send_block_score =
          row->p99_send_block_us.count != 0
              ? blend_absolute_relative_score(
                    row->p99_send_block_score.count != 0 ? row->p99_send_block_score.average() : 0.0,
                    normalize_optional_lower_score(true, send_block_value, send_block_peers))
              : 0.0;
      const double scale_efficiency_score =
          normalize_optional_higher_score(row->scale_efficiency_mb_per_sec.count != 0,
                                          row->scale_efficiency_mb_per_sec.average(), scale_efficiency_peers);
      double latency_drop_percent = 0.0;

      if (row->latency_drop_count != 0) {
        latency_drop_percent =
            row->received_sum > 0.0 ? static_cast<double>(row->latency_drop_count) * 100.0 / row->received_sum : 100.0;
      }

      const double health_score = compute_health_score(
          loss_percent, std::max(latency_drop_percent, row->worst_latency_drop_pct), coverage_ratio, repeat_ratio);

      if (row->suite == "throughput") {
        const double primary_score = recv_score * 0.95 + transfer_efficiency_score * 0.02;
        row->score = clamp_score(primary_score * main_coverage + health_score * 0.03);
      } else if (row->suite == "latency") {
        const double primary_score =
            latency_quality_score * 0.94 + max_latency_score * 0.01 + jitter_score * 0.01 + recv_score * 0.01;
        row->score = clamp_score(primary_score * main_coverage + health_score * 0.03);
      } else if (row->suite == "backpressure") {
        const double primary_score = send_block_score * 0.80 + recv_score * 0.12 + transfer_efficiency_score * 0.05;
        row->score = clamp_score(primary_score * main_coverage + health_score * 0.03);
      } else {
        const double primary_score =
            recv_score * 0.90 + scale_efficiency_score * 0.06 + transfer_efficiency_score * 0.01;
        row->score = clamp_score(primary_score * main_coverage + health_score * 0.03);
      }
    }
  }

  std::sort(rows.begin(), rows.end(), [](const TransportScoreRow& lhs, const TransportScoreRow& rhs) {
    return std::tuple{lhs.mode, lhs.suite, lhs.url_order_index, lhs.transport, lhs.url} <
           std::tuple{rhs.mode, rhs.suite, rhs.url_order_index, rhs.transport, rhs.url};
  });
  return rows;
}

std::vector<SerializationScoreRow> build_serialization_score_rows(const std::vector<AggregatedCase>& items) {
  std::map<std::tuple<std::string, std::string, std::string, size_t>, SerializationScoreRow> summary;

  for (const auto& item : items) {
    if (!is_serialization_case(item)) {
      continue;
    }

    auto& row = summary[{item.transport, Bench::mode_to_string(item.scenario.mode),
                         Bench::payload_to_string(item.scenario.payload), item.scenario.payload_size}];
    row.transport = item.transport;
    row.mode = Bench::mode_to_string(item.scenario.mode);
    row.payload = Bench::payload_to_string(item.scenario.payload);
    row.payload_size = item.scenario.payload_size;
    ++row.case_count;
    row.repeat_total += item.sample_count;
    row.repeat_success += item.success_count;

    if (item.all_success()) {
      ++row.passing_case_count;

      if (item.serialize_msgs_per_sec.count != 0) {
        row.serialize_msgs_per_sec.add(item.serialize_msgs_per_sec.average());
      }

      if (item.deserialize_msgs_per_sec.count != 0) {
        row.deserialize_msgs_per_sec.add(item.deserialize_msgs_per_sec.average());
      }

      if (item.serialize_mb_per_sec.count != 0) {
        row.serialize_mb_per_sec.add(item.serialize_mb_per_sec.average());
      }

      if (item.deserialize_mb_per_sec.count != 0) {
        row.deserialize_mb_per_sec.add(item.deserialize_mb_per_sec.average());
      }

      if (item.pub_cpu_ms.count != 0) {
        row.pub_cpu_ms.add(item.pub_cpu_ms.average());
      }

      if (item.sub_cpu_ms.count != 0) {
        row.sub_cpu_ms.add(item.sub_cpu_ms.average());
      }

      if (item.memory_usage.count != 0) {
        row.memory_usage.add(item.memory_usage.average());
      }
    } else if (item.success_count > 0 && item.failure_count == 0) {
      ++row.warning_case_count;
    } else {
      ++row.failing_case_count;
    }
  }

  std::vector<SerializationScoreRow> rows;
  rows.reserve(summary.size());
  for (auto& [_, row] : summary) {
    rows.emplace_back(std::move(row));
  }

  std::map<std::tuple<std::string, std::string, size_t>, std::vector<SerializationScoreRow*>> cohorts;
  for (auto& row : rows) {
    cohorts[{row.mode, row.payload, row.payload_size}].emplace_back(&row);
  }

  for (const auto& [_, cohort] : cohorts) {
    const auto serialize_mb_peers =
        collect_peer_values<SerializationScoreRow>(cohort, [](const SerializationScoreRow& row) {
          return std::pair{row.serialize_mb_per_sec.count != 0, row.serialize_mb_per_sec.average()};
        });
    const auto deserialize_mb_peers =
        collect_peer_values<SerializationScoreRow>(cohort, [](const SerializationScoreRow& row) {
          return std::pair{row.deserialize_mb_per_sec.count != 0, row.deserialize_mb_per_sec.average()};
        });
    const auto serialize_msgs_peers =
        collect_peer_values<SerializationScoreRow>(cohort, [](const SerializationScoreRow& row) {
          return std::pair{row.serialize_msgs_per_sec.count != 0, row.serialize_msgs_per_sec.average()};
        });
    const auto deserialize_msgs_peers =
        collect_peer_values<SerializationScoreRow>(cohort, [](const SerializationScoreRow& row) {
          return std::pair{row.deserialize_msgs_per_sec.count != 0, row.deserialize_msgs_per_sec.average()};
        });
    const auto encode_efficiency_peers =
        collect_peer_values<SerializationScoreRow>(cohort, [](const SerializationScoreRow& row) {
          const bool valid =
              row.serialize_mb_per_sec.count != 0 && row.pub_cpu_ms.count != 0 && row.pub_cpu_ms.average() > 0.0;
          return std::pair{valid,
                           compute_resource_efficiency(row.serialize_mb_per_sec.average(), row.pub_cpu_ms.average())};
        });
    const auto decode_efficiency_peers =
        collect_peer_values<SerializationScoreRow>(cohort, [](const SerializationScoreRow& row) {
          const bool valid =
              row.deserialize_mb_per_sec.count != 0 && row.sub_cpu_ms.count != 0 && row.sub_cpu_ms.average() > 0.0;
          return std::pair{valid,
                           compute_resource_efficiency(row.deserialize_mb_per_sec.average(), row.sub_cpu_ms.average())};
        });
    const auto balance_peers = collect_peer_values<SerializationScoreRow>(cohort, [](const SerializationScoreRow& row) {
      const bool valid = row.serialize_mb_per_sec.count != 0 && row.deserialize_mb_per_sec.count != 0;
      return std::pair{valid, compute_serialization_balance(row.serialize_mb_per_sec.average(),
                                                            row.deserialize_mb_per_sec.average())};
    });
    const auto cpu_total_peers =
        collect_peer_values<SerializationScoreRow>(cohort, [](const SerializationScoreRow& row) {
          const bool valid = row.pub_cpu_ms.count != 0 || row.sub_cpu_ms.count != 0;
          return std::pair{valid, row.pub_cpu_ms.average() + row.sub_cpu_ms.average()};
        });
    const auto memory_peers = collect_peer_values<SerializationScoreRow>(cohort, [](const SerializationScoreRow& row) {
      return std::pair{row.memory_usage.count != 0, row.memory_usage.average()};
    });

    for (auto* row : cohort) {
      if (cohort.size() < 2) {
        row->score = -1.0;
        continue;
      }

      if (row->passing_case_count == 0 || row->repeat_success == 0) {
        row->score = 0.0;
        continue;
      }

      const double coverage_ratio =
          row->case_count == 0 ? 0.0
                               : static_cast<double>(row->passing_case_count) / static_cast<double>(row->case_count);
      const double repeat_ratio = compute_repeat_success_ratio(row->repeat_success, row->repeat_total);
      const double coverage_score = coverage_ratio * 100.0;
      const double repeat_score = repeat_ratio * 100.0;
      const double encode_mb_score = normalize_optional_higher_score(
          row->serialize_mb_per_sec.count != 0, row->serialize_mb_per_sec.average(), serialize_mb_peers);
      const double decode_mb_score = normalize_optional_higher_score(
          row->deserialize_mb_per_sec.count != 0, row->deserialize_mb_per_sec.average(), deserialize_mb_peers);
      const double encode_msgs_score = normalize_optional_higher_score(
          row->serialize_msgs_per_sec.count != 0, row->serialize_msgs_per_sec.average(), serialize_msgs_peers);
      const double decode_msgs_score = normalize_optional_higher_score(
          row->deserialize_msgs_per_sec.count != 0, row->deserialize_msgs_per_sec.average(), deserialize_msgs_peers);
      const double encode_efficiency_score = normalize_optional_higher_score(
          row->serialize_mb_per_sec.count != 0 && row->pub_cpu_ms.count != 0 && row->pub_cpu_ms.average() > 0.0,
          compute_resource_efficiency(row->serialize_mb_per_sec.average(), row->pub_cpu_ms.average()),
          encode_efficiency_peers);
      const double decode_efficiency_score = normalize_optional_higher_score(
          row->deserialize_mb_per_sec.count != 0 && row->sub_cpu_ms.count != 0 && row->sub_cpu_ms.average() > 0.0,
          compute_resource_efficiency(row->deserialize_mb_per_sec.average(), row->sub_cpu_ms.average()),
          decode_efficiency_peers);
      const double balance_score = normalize_optional_higher_score(
          row->serialize_mb_per_sec.count != 0 && row->deserialize_mb_per_sec.count != 0,
          compute_serialization_balance(row->serialize_mb_per_sec.average(), row->deserialize_mb_per_sec.average()),
          balance_peers);
      const double cpu_total_score =
          normalize_optional_lower_score(row->pub_cpu_ms.count != 0 || row->sub_cpu_ms.count != 0,
                                         row->pub_cpu_ms.average() + row->sub_cpu_ms.average(), cpu_total_peers);
      const double memory_score =
          normalize_optional_lower_score(row->memory_usage.count != 0, row->memory_usage.average(), memory_peers);

      const double base_score = coverage_score * 0.10 + repeat_score * 0.08 + encode_mb_score * 0.22 +
                                decode_mb_score * 0.22 + encode_msgs_score * 0.08 + decode_msgs_score * 0.08 +
                                encode_efficiency_score * 0.08 + decode_efficiency_score * 0.08 + balance_score * 0.04 +
                                cpu_total_score * 0.01 + memory_score * 0.01;

      row->score = clamp_score(base_score * compute_stability_gate(coverage_ratio, repeat_ratio));
    }
  }

  std::sort(rows.begin(), rows.end(), [](const SerializationScoreRow& lhs, const SerializationScoreRow& rhs) {
    return std::tuple{lhs.mode, lhs.payload, lhs.payload_size, -lhs.score, lhs.transport} <
           std::tuple{rhs.mode, rhs.payload, rhs.payload_size, -rhs.score, rhs.transport};
  });
  return rows;
}

std::string build_suite_score_methodology_panel() {
  std::string html;
  html.reserve(2048);
  html +=
      "<div class=\"note\"><span data-i18n=\"suite_score_methodology_note_1\">"
      "This table scores throughput, latency, backpressure, topology, and serialization separately. Compare scores "
      "inside the same category; a latency score and a throughput score are not interchangeable."
      "</span></div>";
  html +=
      "<div class=\"note\"><span data-i18n=\"suite_score_three_layers_prefix\">"
      "Each score is mainly driven by the most important metric for that category, with a small health check for "
      "failures, visible loss, latency sample drops, and repeat success.</span></div>";
  html +=
      "<div class=\"table-scroll\"><table class=\"bench-table cardable\"><thead><tr>"
      "<th data-i18n=\"th_suite\">Category</th>"
      "<th data-i18n=\"th_input_factors\">What is scored</th>"
      "<th data-i18n=\"th_gates\">How to read it</th>"
      "<th data-i18n=\"th_interpretation\">Best used for</th></tr></thead><tbody>";
  html +=
      "<tr><td data-i18n-label=\"th_suite\" data-label=\"Category\"><code>throughput</code></td>"
      "<td data-i18n-label=\"th_input_factors\" data-label=\"What is scored\" "
      "data-i18n=\"suite_score_factors_throughput\">"
      "Mainly received MB/s, with small checks for send/receive efficiency and health.</td>"
      "<td data-i18n-label=\"th_gates\" data-label=\"How to read it\" "
      "data-i18n=\"suite_score_gates_throughput\">"
      "Throughput tests mainly answer how much data was actually received. Health only highlights clear problems.</td>"
      "<td data-i18n-label=\"th_interpretation\" data-label=\"Best used for\" "
      "data-i18n=\"suite_score_interp_throughput\">"
      "Useful for choosing the configuration that carries more data under larger payloads or heavier send "
      "pressure.</td></tr>";
  html +=
      "<tr><td data-i18n-label=\"th_suite\" data-label=\"Category\"><code>latency</code></td>"
      "<td data-i18n-label=\"th_input_factors\" data-label=\"What is scored\" "
      "data-i18n=\"suite_score_factors_latency\">"
      "Mainly P95/P99 latency quality, with light tail-stability checks for P99.9/P99.99, max latency, jitter, "
      "throughput, and health.</td>"
      "<td data-i18n-label=\"th_gates\" data-label=\"How to read it\" data-i18n=\"suite_score_gates_latency\">"
      "Latency tests mainly use percentiles. The heatmap visualises P50 (typical) / P90 (slower common case) / P99 "
      "(tail). P99.9 / P99.99 stay visible as tail-stability signals, but they do not dominate the score. Lower "
      "numbers are better.</td>"
      "<td data-i18n-label=\"th_interpretation\" data-label=\"Best used for\" "
      "data-i18n=\"suite_score_interp_latency\">"
      "Useful for checking whether messages respond quickly and whether slow-message latency stays controlled. "
      "P95/P99 drive the score, while P99.9/P99.99 help explain rare stalls.</td></tr>";
  html +=
      "<tr><td data-i18n-label=\"th_suite\" data-label=\"Category\"><code>backpressure</code></td>"
      "<td data-i18n-label=\"th_input_factors\" data-label=\"What is scored\" "
      "data-i18n=\"suite_score_factors_backpressure\">"
      "Mainly publisher-side P99 send-block time, with checks for delivered throughput, send/receive efficiency, and "
      "health.</td>"
      "<td data-i18n-label=\"th_gates\" data-label=\"How to read it\" "
      "data-i18n=\"suite_score_gates_backpressure\">"
      "Backpressure tests simulate slow consumers. Lower publish() blocking time is better, but the run still needs "
      "to deliver messages reliably.</td>"
      "<td data-i18n-label=\"th_interpretation\" data-label=\"Best used for\" "
      "data-i18n=\"suite_score_interp_backpressure\">"
      "Useful for spotting transports that stall publisher threads when subscribers fall behind.</td></tr>";
  html +=
      "<tr><td data-i18n-label=\"th_suite\" data-label=\"Category\"><code>topology</code></td>"
      "<td data-i18n-label=\"th_input_factors\" data-label=\"What is scored\" "
      "data-i18n=\"suite_score_factors_topology\">"
      "Mainly total received MB/s and scaling efficiency, with small checks for send/receive efficiency and "
      "health.</td>"
      "<td data-i18n-label=\"th_gates\" data-label=\"How to read it\" data-i18n=\"suite_score_gates_topology\">"
      "Topology tests mainly check whether total throughput keeps up as publishers or subscribers increase.</td>"
      "<td data-i18n-label=\"th_interpretation\" data-label=\"Best used for\" "
      "data-i18n=\"suite_score_interp_topology\">"
      "Useful for one-to-many, many-to-one, and many-to-many deployment shapes.</td></tr>";
  html +=
      "<tr><td data-i18n-label=\"th_suite\" data-label=\"Category\"><code>serialization</code></td>"
      "<td data-i18n-label=\"th_input_factors\" data-label=\"What is scored\" "
      "data-i18n=\"suite_score_factors_serialization\">"
      "Mainly encode/decode speed, also checking coverage, repeat success, efficiency, balance, CPU, and memory.</td>"
      "<td data-i18n-label=\"th_gates\" data-label=\"How to read it\" "
      "data-i18n=\"suite_score_gates_serialization\">"
      "Serialization mainly compares encode/decode speed and checks repeat stability.</td>"
      "<td data-i18n-label=\"th_interpretation\" data-label=\"Best used for\" "
      "data-i18n=\"suite_score_interp_serialization\">"
      "Useful for judging data-format cost. It does not represent the full communication path.</td></tr>";
  html += "</tbody></table></div>";
  return html;
}

std::string build_insight_panel(const std::vector<AggregatedCase>& items) {
  std::vector<std::string> lines;
  std::vector<std::string> urls;
  std::vector<std::string> transports;
  std::vector<std::string> modes;
  std::map<std::pair<std::string, std::string>, size_t> passing_matrix;

  size_t passing_cases = 0;
  size_t unstable_cases = 0;

  for (const auto& item : items) {
    append_unique(urls, item.scenario.url);
    append_unique(transports, item.transport);
    append_unique(modes, Bench::mode_to_string(item.scenario.mode));

    if (item.all_success() && !has_delivery_loss(item)) {
      ++passing_cases;
      ++passing_matrix[{item.transport, Bench::mode_to_string(item.scenario.mode)}];
    } else {
      ++unstable_cases;
    }
  }

  if (const auto* best = find_max_case(
          items,
          [](const AggregatedCase& item) {
            return item.scenario.suite == Bench::kThroughputSuite && item.scenario.topology == Bench::kOneToOneTopology;
          },
          [](const AggregatedCase& item) { return item.recv_mb_per_sec.average(); });
      best != nullptr) {
    lines.emplace_back(build_case_insight_html(
        *best, "insight_highest_1to1_throughput", "Highest 1:1 throughput",
        "<span data-i18n=\"insight_recv_label\">received</span> " + format_metric_cell(best->recv_mb_per_sec) +
            " MB/s | <span data-i18n=\"insight_first_msg_label\">first msg</span> " +
            format_metric_cell(best->first_message_ms) + " ms" + format_loss_insight_suffix(*best)));
  }

  if (const auto* best = find_min_case(
          items,
          [](const AggregatedCase& item) {
            return item.scenario.suite == Bench::kLatencySuite && item.scenario.topology == Bench::kOneToOneTopology &&
                   item.p95_latency_us.count != 0;
          },
          [](const AggregatedCase& item) { return item.p95_latency_us.average(); });
      best != nullptr) {
    lines.emplace_back(build_case_insight_html(*best, "insight_lowest_1to1_latency", "Lowest 1:1 latency",
                                               "p95 " + format_metric_cell(best->p95_latency_us) + " us | p99 " +
                                                   format_metric_cell(best->p99_latency_us) +
                                                   " us | <span data-i18n=\"insight_recv_label\">received</span> " +
                                                   format_metric_cell(best->recv_mb_per_sec) + " MB/s"));
  }

  if (const auto* best = find_max_case(
          items,
          [](const AggregatedCase& item) {
            return item.scenario.suite == Bench::kTopologySuite && item.scenario.topology == Bench::kOneToManyTopology;
          },
          [](const AggregatedCase& item) { return item.recv_mb_per_sec.average(); });
      best != nullptr) {
    lines.emplace_back(build_case_insight_html(
        *best, "insight_highest_1n_throughput", "Highest 1:n total throughput",
        "<span data-i18n=\"insight_recv_label\">received</span> " + format_metric_cell(best->recv_mb_per_sec) +
            " MB/s | <span data-i18n=\"insight_subscribers_label\">subscribers</span> " +
            std::to_string(best->scenario.subscribers) + format_loss_insight_suffix(*best)));
  }

  if (const auto* best = find_max_case(
          items,
          [](const AggregatedCase& item) {
            return item.scenario.suite == Bench::kTopologySuite && item.scenario.topology == Bench::kManyToManyTopology;
          },
          [](const AggregatedCase& item) { return item.recv_mb_per_sec.average(); });
      best != nullptr) {
    lines.emplace_back(build_case_insight_html(
        *best, "insight_highest_nn_throughput", "Highest n:n total throughput",
        "<span data-i18n=\"insight_recv_label\">received</span> " + format_metric_cell(best->recv_mb_per_sec) +
            " MB/s | <span data-i18n=\"insight_publishers_label\">publishers</span> " +
            std::to_string(best->scenario.publishers) +
            " | <span data-i18n=\"insight_subscribers_label\">subscribers</span> " +
            std::to_string(best->scenario.subscribers)));
  }

  if (const auto* best = find_max_case(
          items, [](const AggregatedCase& item) { return item.scenario.suite == Bench::kSerializationSuite; },
          [](const AggregatedCase& item) { return item.serialize_mb_per_sec.average(); });
      best != nullptr) {
    lines.emplace_back(
        build_case_insight_html(*best, "insight_fastest_encode", "Fastest encode",
                                "<span data-i18n=\"insight_serialize_label\">serialize</span> " +
                                    format_metric_cell(best->serialize_mb_per_sec) +
                                    " MB/s | <span data-i18n=\"insight_deserialize_label\">deserialize</span> " +
                                    format_metric_cell(best->deserialize_mb_per_sec) + " MB/s"));
  }

  if (lines.empty()) {
    return std::string();
  }

  std::string html;
  html.reserve(512);
  html += R"(<div class="note"><span data-i18n="executive_coverage_sentence_prefix">This run covers</span> )" +
          std::to_string(urls.size()) + " <span data-i18n=\"executive_coverage_urls\">url(s),</span> " +
          std::to_string(transports.size()) +
          " <span data-i18n=\"executive_coverage_transports\">transport(s), and</span> " +
          std::to_string(modes.size()) + " <span data-i18n=\"executive_coverage_modes\">mode(s).</span> " +
          "<span data-i18n=\"executive_coverage_passing\">normal cases:</span> " + std::to_string(passing_cases) + "/" +
          std::to_string(items.size()) + "; " +
          "<span data-i18n=\"executive_coverage_unstable\">problem cases:</span> " + std::to_string(unstable_cases) +
          ".</div>";

  if (!passing_matrix.empty()) {
    const auto best_iter = std::max_element(passing_matrix.begin(), passing_matrix.end(),
                                            [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });

    const auto tie_count = static_cast<size_t>(
        std::count_if(passing_matrix.begin(), passing_matrix.end(),
                      [target = best_iter->second](const auto& entry) { return entry.second == target; }));

    if VLIKELY (tie_count == 1) {
      html +=
          "<div class=\"note\"><span data-i18n=\"executive_broadest_prefix\">"
          "The most normal cases come from</span> <code>" +
          escape_html(best_iter->first.first) + "</code> <span data-i18n=\"executive_broadest_in\">in</span> <code>" +
          escape_html(best_iter->first.second) +
          "</code> <span data-i18n=\"executive_broadest_mode_with\">mode with</span> " +
          std::to_string(best_iter->second) +
          " <span data-i18n=\"executive_broadest_suffix\">normal case(s).</span></div>";
    } else {
      html +=
          "<div class=\"note\"><span data-i18n=\"executive_broadest_tie_prefix\">"
          "All</span> " +
          std::to_string(tie_count) +
          " <span data-i18n=\"executive_broadest_tie_middle\">transport(s) share the widest normal coverage "
          "with</span> " +
          std::to_string(best_iter->second) +
          " <span data-i18n=\"executive_broadest_suffix\">normal case(s).</span></div>";
    }
  }

  html +=
      "<div class=\"note\" data-i18n=\"executive_highlights_note\">"
      "This section highlights the easiest results to notice in this report: highest throughput, lowest latency, "
      "topology scaling, and fastest encode/decode. Use it to find leads quickly, then confirm with the "
      "recommendation, heatmaps, and problem cases.</div>";
  for (const auto& line : lines) {
    html += "<div class=\"insight-item\">";
    html += line;
    html += "</div>";
  }

  return html;
}

std::string build_url_summary_table(const std::vector<AggregatedCase>& items) {
  struct UrlEntry final {
    std::string transport;
    std::string url;
    int url_order_index{std::numeric_limits<int>::max()};
    std::vector<std::string> modes;
    size_t passing{0};
    size_t warning{0};
    size_t failing{0};
    double best_recv_mb_per_sec{0.0};
    double best_p95_latency_us{0.0};
    bool has_best_recv{false};
    bool has_best_p95{false};
  };

  std::map<std::pair<std::string, std::string>, UrlEntry> summary;

  for (const auto& item : items) {
    auto& entry = summary[{item.transport, item.scenario.url}];

    if (entry.transport.empty()) {
      entry.transport = item.transport;
      entry.url = item.scenario.url;
    }

    if (item.url_order_index >= 0 && item.url_order_index < entry.url_order_index) {
      entry.url_order_index = item.url_order_index;
    }

    append_unique(entry.modes, Bench::mode_to_string(item.scenario.mode));

    if (item.all_success() && !has_delivery_loss(item)) {
      ++entry.passing;

      if (item.recv_mb_per_sec.count != 0) {
        const double recv_mb_per_sec = item.recv_mb_per_sec.average();
        if (!entry.has_best_recv || recv_mb_per_sec > entry.best_recv_mb_per_sec) {
          entry.best_recv_mb_per_sec = recv_mb_per_sec;
          entry.has_best_recv = true;
        }
      }

      if (item.p95_latency_us.count != 0) {
        const double p95_latency_us = item.p95_latency_us.average();
        if (!entry.has_best_p95 || p95_latency_us < entry.best_p95_latency_us) {
          entry.best_p95_latency_us = p95_latency_us;
          entry.has_best_p95 = true;
        }
      }
    } else if (item.success_count > 0 && item.failure_count == 0) {
      ++entry.warning;
    } else {
      ++entry.failing;
    }
  }

  if (summary.empty()) {
    return std::string();
  }

  std::vector<UrlEntry> ordered;
  ordered.reserve(summary.size());
  for (auto& [key, entry] : summary) {
    (void)key;
    ordered.emplace_back(std::move(entry));
  }
  std::sort(ordered.begin(), ordered.end(), [](const UrlEntry& lhs, const UrlEntry& rhs) {
    return std::tuple{lhs.url_order_index, lhs.transport, lhs.url} <
           std::tuple{rhs.url_order_index, rhs.transport, rhs.url};
  });

  std::ostringstream html;
  html << "<div class=\"table-scroll\"><table class=\"bench-table "
          "cardable\"><thead><tr><th data-i18n=\"th_transport\">Transport</th><th data-i18n=\"th_url\">URL</th><th "
          "data-i18n=\"th_modes\">Modes</th><th data-i18n=\"th_passing\">Passing</th>"
       << "<th data-i18n=\"th_warning\">Warning</th><th data-i18n=\"th_failing\">Failing</th><th "
          "data-i18n=\"th_best_recv_mbps\">Best Recv MB/s</th><th data-i18n=\"th_best_p95_us\">Best P95 "
          "us</th></tr></thead><tbody>";

  for (const auto& entry : ordered) {
    html << R"(<tr><td data-i18n-label="th_transport" data-label="Transport"><code>)" << escape_html(entry.transport)
         << R"(</code></td><td data-i18n-label="th_url" data-label="URL" class="config-cell"><code>)"
         << escape_html(entry.url) << R"(</code></td><td data-i18n-label="th_modes" data-label="Modes">)"
         << escape_html(join_strings(entry.modes, " | "))
         << R"(</td><td data-i18n-label="th_passing" data-label="Passing">)" << entry.passing
         << R"(</td><td data-i18n-label="th_warning" data-label="Warning">)" << entry.warning
         << R"(</td><td data-i18n-label="th_failing" data-label="Failing">)" << entry.failing
         << R"(</td><td data-i18n-label="th_best_recv_mbps" data-label="Best Recv MB/s">)"
         << (entry.has_best_recv ? escape_html(format_decimal(entry.best_recv_mb_per_sec)) : "-")
         << R"(</td><td data-i18n-label="th_best_p95_us" data-label="Best P95 us">)"
         << (entry.has_best_p95 ? escape_html(format_decimal(entry.best_p95_latency_us)) : "-") << "</td></tr>";
  }

  html << "</tbody></table></div>";
  return html.str();
}

std::string build_failure_panel(const std::vector<AggregatedCase>& items) {
  struct FailureEntry final {
    std::string transport;
    std::string mode;
    int url_order_index{std::numeric_limits<int>::max()};
    size_t failure_count{0};
    size_t total_count{0};
  };

  std::map<std::pair<std::string, std::string>, FailureEntry> summary;

  for (const auto& item : items) {
    auto& entry = summary[{item.transport, Bench::mode_to_string(item.scenario.mode)}];

    if (entry.transport.empty()) {
      entry.transport = item.transport;
      entry.mode = Bench::mode_to_string(item.scenario.mode);
    }

    if (item.url_order_index >= 0 && item.url_order_index < entry.url_order_index) {
      entry.url_order_index = item.url_order_index;
    }

    ++entry.total_count;

    if (!item.all_success() || has_delivery_loss(item)) {
      ++entry.failure_count;
    }
  }

  std::vector<FailureEntry> ordered;
  ordered.reserve(summary.size());
  for (auto& [key, entry] : summary) {
    (void)key;
    ordered.emplace_back(std::move(entry));
  }
  std::sort(ordered.begin(), ordered.end(), [](const FailureEntry& lhs, const FailureEntry& rhs) {
    return std::tuple{lhs.url_order_index, lhs.transport, lhs.mode} <
           std::tuple{rhs.url_order_index, rhs.transport, rhs.mode};
  });

  std::ostringstream html;
  html << "<div class=\"table-scroll\"><table class=\"bench-table "
          "cardable\"><thead><tr><th data-i18n=\"th_transport\">Transport</th><th data-i18n=\"th_mode\">Mode</th>"
          "<th data-i18n=\"th_unstable_cases\">Problem Cases</th>"
          "<th data-i18n=\"th_total_cases\">Total Cases</th></tr></thead><tbody>";
  for (const auto& entry : ordered) {
    html << R"(<tr><td data-i18n-label="th_transport" data-label="Transport"><code>)" << escape_html(entry.transport)
         << R"(</code></td><td data-i18n-label="th_mode" data-label="Mode"><code>)" << escape_html(entry.mode)
         << R"(</code></td><td data-i18n-label="th_unstable_cases" data-label="Problem Cases">)" << entry.failure_count
         << R"(</td><td data-i18n-label="th_total_cases" data-label="Total Cases">)" << entry.total_count
         << "</td></tr>";
  }

  html << "</tbody></table></div>";
  return html.str();
}

std::string build_unstable_case_table(const std::vector<AggregatedCase>& items) {
  std::ostringstream html;
  bool has_unstable = false;
  html << "<div class=\"table-scroll\"><table class=\"bench-table "
          "cardable\"><thead><tr><th data-i18n=\"th_suite\">Category</th><th data-i18n=\"th_mode\">Mode</th>"
          "<th data-i18n=\"th_transport\">Transport</th><th data-i18n=\"th_payload\">Payload</th>"
       << "<th data-i18n=\"th_config\">Config</th><th data-i18n=\"th_repeats\">Repeats</th>"
          "<th data-i18n=\"th_status\">Status</th><th data-i18n=\"th_errors\">Errors</th></tr></thead><tbody>";

  for (const auto& item : items) {
    if (item.all_success() && !has_delivery_loss(item)) {
      continue;
    }

    has_unstable = true;
    const bool failed = item.success_count == 0 || item.failure_count != 0;

    html << R"(<tr><td data-i18n-label="th_suite" data-label="Category">)"
         << escape_html(Bench::suite_to_string(item.scenario.suite))
         << R"(</td><td data-i18n-label="th_mode" data-label="Mode">)"
         << escape_html(Bench::mode_to_string(item.scenario.mode))
         << R"(</td><td data-i18n-label="th_transport" data-label="Transport">)" << escape_html(item.transport)
         << R"(</td><td data-i18n-label="th_payload" data-label="Payload">)"
         << escape_html(Bench::payload_to_string(item.scenario.payload)) << " / "
         << escape_html(format_size_label(item.scenario.payload_size))
         << R"(</td><td data-i18n-label="th_config" data-label="Config" class="config-cell">)"
         << summarize_config_html(item) << R"(</td><td data-i18n-label="th_repeats" data-label="Repeats">)"
         << item.success_count << '/' << item.sample_count
         << R"(</td><td data-i18n-label="th_status" data-label="Status" class=")" << (failed ? "fail" : "warn") << "\">"
         << wrap_status_brief_html(item);

    if (item.latency_samples_dropped != 0) {
      html << R"( | <span data-i18n="status_drop_label">drop</span> )" << item.latency_samples_dropped;
    }

    html << R"(</td><td data-i18n-label="th_errors" data-label="Errors">)"
         << escape_html(join_strings(item.errors, " | ")) << "</td></tr>";
  }

  html << "</tbody></table></div>";
  return has_unstable ? html.str() : std::string();
}

std::string build_suite_score_summary_panel(const std::vector<TransportScoreRow>& transport_rows,
                                            const std::vector<SerializationScoreRow>& serialization_rows) {
  std::map<std::pair<std::string, std::string>, const TransportScoreRow*> transport_best;
  std::map<std::string, const SerializationScoreRow*> serialization_best;

  for (const auto& row : transport_rows) {
    if (row.score < 0.0 || row.passing_case_count + row.warning_case_count == 0) {
      continue;
    }

    const auto key = std::make_pair(row.mode, row.suite);
    const auto iter = transport_best.find(key);

    if (iter == transport_best.end() || row.score > iter->second->score) {
      transport_best[key] = &row;
    }
  }

  for (const auto& row : serialization_rows) {
    if (row.score < 0.0 || row.passing_case_count == 0) {
      continue;
    }

    const auto iter = serialization_best.find(row.mode);

    if (iter == serialization_best.end() || row.score > iter->second->score) {
      serialization_best[row.mode] = &row;
    }
  }

  if (transport_best.empty() && serialization_best.empty()) {
    return std::string();
  }

  std::ostringstream html;
  html << R"(<div class="note" data-i18n="suite_score_panel_note">)"
       << "This table lists the best configuration in each test category. Different categories focus on different "
       << "signals, so compare scores inside the same category.</div>";
  html << "<div class=\"table-scroll\"><table class=\"bench-table "
          "cardable\"><thead><tr><th data-i18n=\"th_mode\">Mode</th><th data-i18n=\"th_suite\">Category</th><th "
          "data-i18n=\"th_winner\">Best Result</th><th data-i18n=\"th_score\">Score</th>"
       << "<th data-i18n=\"th_coverage\">Coverage</th><th "
          "data-i18n=\"th_conclusion\">Conclusion</th></tr></thead><tbody>";

  for (const auto& [key, row] : transport_best) {
    const double aggregate_loss_percent = row->expected_sum <= 0.0 ? 0.0 : row->lost_sum * 100.0 / row->expected_sum;
    const double loss_percent = std::max(aggregate_loss_percent, row->worst_loss_pct);
    html << R"(<tr><td data-i18n-label="th_mode" data-label="Mode"><code>)" << escape_html(row->mode)
         << R"(</code></td><td data-i18n-label="th_suite" data-label="Category"><code>)" << escape_html(row->suite)
         << R"(</code></td><td data-i18n-label="th_winner" data-label="Best Result"><code>)"
         << escape_html(row->transport) << "://</code><br><small><code>" << escape_html(row->endpoint_detail)
         << R"(</code></small></td><td data-i18n-label="th_score" data-label="Score" class=")"
         << get_score_class(row->score) << "\">" << format_score_cell(row->score)
         << R"(</td><td data-i18n-label="th_coverage" data-label="Coverage">)"
         << (row->passing_case_count + row->warning_case_count) << "/" << row->case_count
         << " <span data-i18n=\"coverage_case_suffix\">case(s),</span> "
         << escape_html(format_decimal(compute_repeat_success_ratio(row->repeat_success, row->repeat_total) * 100.0, 1))
         << "% <span data-i18n=\"coverage_repeat_success_suffix\">repeat success,</span> " << row->urls.size()
         << " <span data-i18n=\"coverage_urls_suffix\">url(s)</span></td><td data-i18n-label=\"th_conclusion\" "
            "data-label=\"Conclusion\">";

    if (row->suite == "latency") {
      html << "<span data-i18n=\"conclusion_latency_prefix\">Lowest latency:</span> P95 "
           << escape_html(format_metric_cell(row->p95_latency_us)) << " us, P99 "
           << escape_html(format_metric_cell(row->p99_latency_us)) << " us, P99.9 "
           << escape_html(format_metric_cell(row->p999_latency_us)) << " us, P99.99 "
           << escape_html(format_metric_cell(row->p9999_latency_us)) << " us";

      if (should_report_delivery_loss(loss_percent)) {
        html << ", <span data-i18n=\"conclusion_loss_label\">loss</span> "
             << escape_html(format_decimal(loss_percent, 3)) << "%";
      }

      html << ".";
    } else if (row->suite == "topology") {
      html << "<span data-i18n=\"conclusion_topology_prefix\">Best topology scaling:</span> "
           << "<span data-i18n=\"conclusion_topology_recv\">received</span> "
           << escape_html(format_metric_cell(row->recv_mb_per_sec))
           << " MB/s, <span data-i18n=\"conclusion_topology_scale\">scale efficiency</span> "
           << escape_html(format_metric_cell(row->scale_efficiency_mb_per_sec)) << " MB/s";

      if (should_report_delivery_loss(loss_percent)) {
        html << ", <span data-i18n=\"conclusion_loss_label\">loss</span> "
             << escape_html(format_decimal(loss_percent, 3)) << "%";
      }

      html << ".";
    } else if (row->suite == "backpressure") {
      html << "<span data-i18n=\"conclusion_backpressure_prefix\">Lowest publisher blocking:</span> "
           << "<span data-i18n=\"conclusion_backpressure_send_block\">send-block P99</span> "
           << escape_html(format_metric_cell(row->p99_send_block_us))
           << " us, <span data-i18n=\"conclusion_backpressure_recv\">received</span> "
           << escape_html(format_metric_cell(row->recv_mb_per_sec)) << " MB/s";

      if (should_report_delivery_loss(loss_percent)) {
        html << ", <span data-i18n=\"conclusion_loss_label\">loss</span> "
             << escape_html(format_decimal(loss_percent, 3)) << "%";
      }

      html << ".";
    } else {
      html << "<span data-i18n=\"conclusion_throughput_prefix\">Best throughput:</span> "
           << "<span data-i18n=\"conclusion_throughput_recv\">received</span> "
           << escape_html(format_metric_cell(row->recv_mb_per_sec))
           << " MB/s, <span data-i18n=\"conclusion_throughput_first_msg\">first message</span> "
           << escape_html(format_metric_cell(row->first_message_ms)) << " ms";

      if (should_report_delivery_loss(loss_percent)) {
        html << ", <span data-i18n=\"conclusion_loss_label\">loss</span> "
             << escape_html(format_decimal(loss_percent, 3)) << "%";
      }

      html << ".";
    }

    html << "</td></tr>";
  }

  for (const auto& [mode, row] : serialization_best) {
    html << R"(<tr><td data-i18n-label="th_mode" data-label="Mode"><code>)" << escape_html(mode)
         << R"(</code></td><td data-i18n-label="th_suite" data-label="Category"><code>serialization</code></td>)"
         << R"(<td data-i18n-label="th_winner" data-label="Best Result"><code>)" << escape_html(row->transport)
         << R"(</code></td><td data-i18n-label="th_score" data-label="Score" class=")" << get_score_class(row->score)
         << "\">" << format_score_cell(row->score) << R"(</td><td data-i18n-label="th_coverage" data-label="Coverage">)"
         << row->passing_case_count << "/" << row->case_count
         << " <span data-i18n=\"coverage_case_suffix\">case(s),</span> "
         << escape_html(format_decimal(compute_repeat_success_ratio(row->repeat_success, row->repeat_total) * 100.0, 1))
         << "% <span data-i18n=\"coverage_repeat_success_suffix\">repeat success,</span></td>"
         << R"(<td data-i18n-label="th_conclusion" data-label="Conclusion">)"
         << "<span data-i18n=\"conclusion_serialization_prefix\">Best codec slice for</span> <code>"
         << escape_html(row->payload) << "</code> / <code>" << escape_html(format_size_label(row->payload_size))
         << "</code>: <span data-i18n=\"conclusion_serialization_encode\">encode</span> "
         << escape_html(format_metric_cell(row->serialize_mb_per_sec))
         << " MB/s, <span data-i18n=\"conclusion_serialization_decode\">decode</span> "
         << escape_html(format_metric_cell(row->deserialize_mb_per_sec)) << " MB/s.</td></tr>";
  }

  html << "</tbody></table></div>";
  return html.str();
}

std::string build_transport_overview_panel(const std::vector<AggregatedCase>& items) {
  const auto rows = build_transport_score_rows(items);

  if (rows.empty()) {
    return std::string();
  }

  std::ostringstream html;
  html << R"(<div class="note" data-i18n="transport_rollup_note_long">)"
       << "Groups results by transport type, run mode, and test category for a quick trend view. Because a row can "
          "include multiple message sizes or rates, use latency/throughput comparisons and trend charts for exact "
          "comparisons.</div>";
  html << "<div class=\"table-scroll\"><table class=\"bench-table "
          "cardable\"><thead><tr><th data-i18n=\"th_transport\">Transport</th><th data-i18n=\"th_mode\">Mode</th>"
          "<th data-i18n=\"th_url\">URL</th><th data-i18n=\"th_suite\">Category</th><th "
          "data-i18n=\"th_score\">Score</th>"
          "<th data-i18n=\"th_coverage\">Coverage</th>"
          "<th data-i18n=\"th_mean_recv_mbps\">Mean Received MB/s</th>"
          "<th data-i18n=\"th_mean_run_p95_us\">Mean Run P95 us</th>"
          "<th data-i18n=\"th_avg_loss_pct\">Loss %</th>"
          "<th data-i18n=\"th_mean_peak_rss_mb\">Mean Peak RSS MB</th></tr></thead><tbody>";

  for (const auto& row : rows) {
    const double aggregate_loss_percent = row.expected_sum <= 0.0 ? 0.0 : row.lost_sum * 100.0 / row.expected_sum;
    const double loss_percent = std::max(aggregate_loss_percent, row.worst_loss_pct);
    html << R"(<tr><td data-i18n-label="th_transport" data-label="Transport"><code>)" << escape_html(row.transport)
         << R"(</code></td><td data-i18n-label="th_mode" data-label="Mode"><code>)" << escape_html(row.mode)
         << R"(</code></td><td data-i18n-label="th_url" data-label="URL" class="config-cell"><code>)"
         << escape_html(row.url) << R"(</code></td><td data-i18n-label="th_suite" data-label="Category"><code>)"
         << escape_html(row.suite) << R"(</code></td><td data-i18n-label="th_score" data-label="Score" class=")"
         << get_score_class(row.score) << "\">" << format_score_cell(row.score)
         << R"(</td><td data-i18n-label="th_coverage" data-label="Coverage">)"
         << (row.passing_case_count + row.warning_case_count) << "/" << row.case_count << " | "
         << escape_html(format_decimal(compute_repeat_success_ratio(row.repeat_success, row.repeat_total) * 100.0, 1))
         << "% <span data-i18n=\"coverage_repeat_short\">repeat</span></td>"
         << R"(<td data-i18n-label="th_mean_recv_mbps" data-label="Mean Received MB/s">)"
         << format_metric_cell(row.recv_mb_per_sec)
         << R"(</td><td data-i18n-label="th_mean_run_p95_us" data-label="Mean Run P95 us">)"
         << format_metric_cell(row.p95_latency_us)
         << R"(</td><td data-i18n-label="th_avg_loss_pct" data-label="Loss %">)"
         << (should_report_delivery_loss(loss_percent) ? escape_html(format_decimal(loss_percent, 3)) : "-")
         << R"(</td><td data-i18n-label="th_mean_peak_rss_mb" data-label="Mean Peak RSS MB">)"
         << format_memory_cell(row.memory_usage) << "</td></tr>";
  }

  html << "</tbody></table></div>";
  return html.str();
}

std::string build_serialization_overview_panel(const std::vector<AggregatedCase>& items) {
  const auto rows = build_serialization_score_rows(items);

  if (rows.empty()) {
    return std::string();
  }

  std::ostringstream html;
  html << R"(<div class="note" data-i18n="serialization_overview_note">)"
       << "Compares encode and decode speed across payload sizes. Use it to understand serialization cost.</div>";
  html << "<div class=\"table-scroll\"><table class=\"bench-table "
          "cardable\"><thead><tr><th data-i18n=\"th_transport\">Transport</th><th data-i18n=\"th_mode\">Mode</th>"
          "<th data-i18n=\"th_payload\">Payload</th><th data-i18n=\"th_size\">Size</th>"
          "<th data-i18n=\"th_score\">Score</th><th data-i18n=\"th_coverage\">Coverage</th>"
          "<th data-i18n=\"th_mean_encode_mbps\">Mean Encode MB/s</th>"
          "<th data-i18n=\"th_mean_decode_mbps\">Mean Decode MB/s</th>"
          "<th data-i18n=\"th_mean_encode_cpu_ms\">Mean Encode CPU ms</th>"
          "<th data-i18n=\"th_mean_decode_cpu_ms\">Mean Decode CPU ms</th>"
          "</tr></thead><tbody>";

  for (const auto& row : rows) {
    html << R"(<tr><td data-i18n-label="th_transport" data-label="Transport"><code>)" << escape_html(row.transport)
         << R"(</code></td><td data-i18n-label="th_mode" data-label="Mode"><code>)" << escape_html(row.mode)
         << R"(</code></td><td data-i18n-label="th_payload" data-label="Payload"><code>)" << escape_html(row.payload)
         << R"(</code></td><td data-i18n-label="th_size" data-label="Size"><code>)"
         << escape_html(format_size_label(row.payload_size))
         << R"(</code></td><td data-i18n-label="th_score" data-label="Score" class=")" << get_score_class(row.score)
         << "\">" << format_score_cell(row.score) << R"(</td><td data-i18n-label="th_coverage" data-label="Coverage">)"
         << row.passing_case_count << "/" << row.case_count << " | "
         << escape_html(format_decimal(compute_repeat_success_ratio(row.repeat_success, row.repeat_total) * 100.0, 1))
         << R"(% repeat</td><td data-i18n-label="th_mean_encode_mbps" data-label="Mean Encode MB/s">)"
         << format_metric_cell(row.serialize_mb_per_sec)
         << R"(</td><td data-i18n-label="th_mean_decode_mbps" data-label="Mean Decode MB/s">)"
         << format_metric_cell(row.deserialize_mb_per_sec)
         << R"(</td><td data-i18n-label="th_mean_encode_cpu_ms" data-label="Mean Encode CPU ms">)"
         << format_metric_cell(row.pub_cpu_ms)
         << R"(</td><td data-i18n-label="th_mean_decode_cpu_ms" data-label="Mean Decode CPU ms">)"
         << format_metric_cell(row.sub_cpu_ms) << "</td></tr>";
  }

  html << "</tbody></table></div>";
  return html.str();
}

std::vector<ChartPanel> build_chart_panels(const std::vector<AggregatedCase>& items) {
  std::vector<ChartPanel> panels;
  auto maybe_add = [&panels](std::string title, std::string body) {
    if (!body.empty()) {
      panels.push_back(ChartPanel{std::move(title), std::move(body)});
    }
  };

  auto mode_i18n_key = [](Bench::Mode m) -> const char* {
    switch (m) {
      case Bench::kLocalDirectMode:
        return "mode_local_direct";
      case Bench::kLocalLoopMode:
        return "mode_local_loop";
      case Bench::kProcessMode:
        return "mode_process";
    }

    return "mode_unknown";
  };

  const std::array<Bench::Mode, 3> modes = {Bench::kLocalDirectMode, Bench::kLocalLoopMode, Bench::kProcessMode};
  for (auto mode : modes) {
    const std::string mode_name = Bench::mode_to_string(mode);
    const std::string mode_suffix =
        " (<span data-i18n=\"" + std::string(mode_i18n_key(mode)) + "\">" + escape_html(mode_name) + "</span>)";

    maybe_add(std::string("<span data-i18n=\"chart_throughput\">Throughput</span>") + mode_suffix,
              build_line_chart(
                  items, "Throughput: Received MB/s vs Payload Size", "Payload Size", "Received MB/s",
                  [mode](const AggregatedCase& item) {
                    return item.scenario.suite == Bench::kThroughputSuite && item.scenario.mode == mode &&
                           item.scenario.topology == Bench::kOneToOneTopology &&
                           item.scenario.rate_pattern == Bench::kMaxRatePattern;
                  },
                  [](const AggregatedCase& item) { return static_cast<double>(item.scenario.payload_size); },
                  [](const AggregatedCase& item) { return item.recv_mb_per_sec.average(); },
                  SeriesRateLabelMode::kIncludeRate, false,
                  ChartI18n{"chart_title_throughput", "chart_axis_payload_size", "chart_axis_delivered_mbps"}));

    maybe_add(std::string("<span data-i18n=\"chart_rate_sweep\">Rate Sweep</span>") + mode_suffix,
              build_line_chart(
                  items, "Rate Sweep: Received MB/s vs Offered Msg/s", "Offered Msg/s", "Received MB/s",
                  [mode](const AggregatedCase& item) {
                    return item.scenario.suite == Bench::kThroughputSuite && item.scenario.mode == mode &&
                           item.scenario.topology == Bench::kOneToOneTopology &&
                           item.scenario.rate_pattern != Bench::kMaxRatePattern;
                  },
                  [](const AggregatedCase& item) { return compute_offered_msg_rate(item); },
                  [](const AggregatedCase& item) { return item.recv_mb_per_sec.average(); },
                  SeriesRateLabelMode::kIncludePattern, true,
                  ChartI18n{"chart_title_rate_sweep", "chart_axis_offered_msgs", "chart_axis_delivered_mbps"}));

    maybe_add(
        std::string("<span data-i18n=\"chart_latency\">Latency</span>") + mode_suffix,
        build_line_chart(
            items, "Latency: Mean Run P95 us vs Payload Size", "Payload Size", "Mean Run P95 Latency (us)",
            [mode](const AggregatedCase& item) {
              return item.scenario.suite == Bench::kLatencySuite && item.scenario.mode == mode &&
                     item.scenario.topology == Bench::kOneToOneTopology && item.p95_latency_us.count != 0;
            },
            [](const AggregatedCase& item) { return static_cast<double>(item.scenario.payload_size); },
            [](const AggregatedCase& item) { return item.p95_latency_us.average(); }, SeriesRateLabelMode::kIncludeRate,
            false, ChartI18n{"chart_title_latency", "chart_axis_payload_size", "chart_axis_p95_latency"}));

    maybe_add(std::string("<span data-i18n=\"chart_loss\">Loss</span>") + mode_suffix,
              build_line_chart(
                  items, "Loss: Loss Ratio vs Payload Size", "Payload Size", "Loss (%)",
                  [mode](const AggregatedCase& item) {
                    return item.scenario.suite == Bench::kLatencySuite && item.scenario.mode == mode &&
                           item.scenario.topology == Bench::kOneToOneTopology;
                  },
                  [](const AggregatedCase& item) { return static_cast<double>(item.scenario.payload_size); },
                  [](const AggregatedCase& item) { return compute_loss_ratio_percent(item); },
                  SeriesRateLabelMode::kIncludeRate, false,
                  ChartI18n{"chart_title_loss", "chart_axis_payload_size", "chart_axis_loss_pct"}));

    maybe_add(std::string("<span data-i18n=\"chart_send_block\">Send-block</span>") + mode_suffix,
              build_line_chart(
                  items, "Send-block: P99 publish() blocking vs Payload Size", "Payload Size", "Send-block P99 (us)",
                  [mode](const AggregatedCase& item) {
                    return item.scenario.suite == Bench::kLatencySuite && item.scenario.mode == mode &&
                           item.scenario.topology == Bench::kOneToOneTopology && item.send_block_samples.count != 0;
                  },
                  [](const AggregatedCase& item) { return static_cast<double>(item.scenario.payload_size); },
                  [](const AggregatedCase& item) { return item.p99_send_block_us.average(); },
                  SeriesRateLabelMode::kIncludeRate, false,
                  ChartI18n{"chart_title_send_block", "chart_axis_payload_size", "chart_axis_send_block_p99"}));

    maybe_add(
        std::string("<span data-i18n=\"chart_first_message\">First Message</span>") + mode_suffix,
        build_line_chart(
            items, "First Message ms vs Payload Size", "Payload Size", "First Message (ms)",
            [mode](const AggregatedCase& item) {
              return item.scenario.suite == Bench::kThroughputSuite && item.scenario.mode == mode &&
                     item.scenario.topology == Bench::kOneToOneTopology &&
                     item.scenario.rate_pattern == Bench::kMaxRatePattern;
            },
            [](const AggregatedCase& item) { return static_cast<double>(item.scenario.payload_size); },
            [](const AggregatedCase& item) { return item.first_message_ms.average(); }, SeriesRateLabelMode::kOmit,
            false, ChartI18n{"chart_title_first_message", "chart_axis_payload_size", "chart_axis_first_message"}));

    maybe_add(std::string("<span data-i18n=\"chart_resource_cpu\">Resource CPU</span>") + mode_suffix,
              build_line_chart(
                  items, "Resource: CPU% vs Payload Size", "Payload Size", "CPU (%)",
                  [mode](const AggregatedCase& item) {
                    return item.scenario.suite == Bench::kThroughputSuite && item.scenario.mode == mode &&
                           item.scenario.topology == Bench::kOneToOneTopology &&
                           item.scenario.rate_pattern == Bench::kMaxRatePattern;
                  },
                  [](const AggregatedCase& item) { return static_cast<double>(item.scenario.payload_size); },
                  [](const AggregatedCase& item) { return item.cpu_usage.average(); }, SeriesRateLabelMode::kOmit,
                  false, ChartI18n{"chart_title_resource_cpu", "chart_axis_payload_size", "chart_axis_cpu_pct"}));

    maybe_add(std::string("<span data-i18n=\"chart_resource_rss\">Resource RSS</span>") + mode_suffix,
              build_line_chart(
                  items, "Resource: Peak RSS MB vs Payload Size", "Payload Size", "Peak RSS MB",
                  [mode](const AggregatedCase& item) {
                    return item.scenario.suite == Bench::kThroughputSuite && item.scenario.mode == mode &&
                           item.scenario.topology == Bench::kOneToOneTopology &&
                           item.scenario.rate_pattern == Bench::kMaxRatePattern;
                  },
                  [](const AggregatedCase& item) { return static_cast<double>(item.scenario.payload_size); },
                  [](const AggregatedCase& item) { return item.memory_usage.average(); }, SeriesRateLabelMode::kOmit,
                  false, ChartI18n{"chart_title_resource_rss", "chart_axis_payload_size", "chart_axis_peak_rss"}));

    maybe_add(std::string("<span data-i18n=\"chart_topology_1n\">Topology 1:n</span>") + mode_suffix,
              build_line_chart(
                  items, "Topology 1:n: Received MB/s vs Subscribers", "Subscribers", "Received MB/s",
                  [mode](const AggregatedCase& item) {
                    return item.scenario.suite == Bench::kTopologySuite && item.scenario.mode == mode &&
                           item.scenario.topology == Bench::kOneToManyTopology;
                  },
                  [](const AggregatedCase& item) { return static_cast<double>(item.scenario.subscribers); },
                  [](const AggregatedCase& item) { return item.recv_mb_per_sec.average(); },
                  SeriesRateLabelMode::kIncludeRate, true,
                  ChartI18n{"chart_title_topology_1n", "chart_axis_subscribers", "chart_axis_delivered_mbps"}, false));

    maybe_add(std::string("<span data-i18n=\"chart_topology_n1\">Topology n:1</span>") + mode_suffix,
              build_line_chart(
                  items, "Topology n:1: Received MB/s vs Publishers", "Publishers", "Received MB/s",
                  [mode](const AggregatedCase& item) {
                    return item.scenario.suite == Bench::kTopologySuite && item.scenario.mode == mode &&
                           item.scenario.topology == Bench::kManyToOneTopology;
                  },
                  [](const AggregatedCase& item) { return static_cast<double>(item.scenario.publishers); },
                  [](const AggregatedCase& item) { return item.recv_mb_per_sec.average(); },
                  SeriesRateLabelMode::kIncludeRate, true,
                  ChartI18n{"chart_title_topology_n1", "chart_axis_publishers", "chart_axis_delivered_mbps"}, false));

    maybe_add(
        std::string("<span data-i18n=\"chart_topology_nn\">Topology n:n</span>") + mode_suffix,
        build_line_chart(
            items, "Topology n:n: Received MB/s vs Connection Pairs", "Connection Pairs", "Received MB/s",
            [mode](const AggregatedCase& item) {
              return item.scenario.suite == Bench::kTopologySuite && item.scenario.mode == mode &&
                     item.scenario.topology == Bench::kManyToManyTopology;
            },
            [](const AggregatedCase& item) {
              return static_cast<double>(item.scenario.publishers * item.scenario.subscribers);
            },
            [](const AggregatedCase& item) { return item.recv_mb_per_sec.average(); },
            SeriesRateLabelMode::kIncludeRate, true,
            ChartI18n{"chart_title_topology_nn", "chart_axis_endpoint_pairs", "chart_axis_delivered_mbps"}, false));
  }

  maybe_add(
      "<span data-i18n=\"chart_serialize\">Serialize</span>",
      build_line_chart(
          items, "Serialize: Encode MB/s vs Payload Size", "Payload Size", "Encode MB/s",
          [](const AggregatedCase& item) { return item.scenario.suite == Bench::kSerializationSuite; },
          [](const AggregatedCase& item) { return static_cast<double>(item.scenario.payload_size); },
          [](const AggregatedCase& item) { return item.serialize_mb_per_sec.average(); }, SeriesRateLabelMode::kOmit,
          false, ChartI18n{"chart_title_serialize", "chart_axis_payload_size", "chart_axis_encode_mbps"}));

  maybe_add(
      "<span data-i18n=\"chart_deserialize\">Deserialize</span>",
      build_line_chart(
          items, "Deserialize: Decode MB/s vs Payload Size", "Payload Size", "Decode MB/s",
          [](const AggregatedCase& item) { return item.scenario.suite == Bench::kSerializationSuite; },
          [](const AggregatedCase& item) { return static_cast<double>(item.scenario.payload_size); },
          [](const AggregatedCase& item) { return item.deserialize_mb_per_sec.average(); }, SeriesRateLabelMode::kOmit,
          false, ChartI18n{"chart_title_deserialize", "chart_axis_payload_size", "chart_axis_decode_mbps"}));

  return panels;
}

struct TransportSummary final {
  std::string endpoint_key;
  std::string transport;
  std::string url;
  std::string endpoint_detail;
  DeploymentLayer layer{DeploymentLayer::kCrossProcess};
  bool is_intra_rewritten{false};
  double avg_speed{0.0};
  double avg_capacity{0.0};
  double avg_efficiency{0.0};
  double avg_coverage{0.0};
  double avg_delivery{0.0};
  double avg_send_block{0.0};
  double avg_topology{0.0};
  double avg_sort{0.0};
  double latency_score_sum{0.0};
  double latency_fallback_sum{0.0};
  double throughput_score_sum{0.0};
  double throughput_fallback_sum{0.0};
  double cpu_efficiency_score_sum{0.0};
  double other_score_sum{0.0};
  double send_block_score_sum{0.0};
  double min_speed_score{101.0};
  double min_capacity_score{101.0};
  double expected_sum{0.0};
  double lost_sum{0.0};
  double worst_loss_pct{0.0};
  double worst_latency_drop_pct{0.0};
  size_t cases{0};
  size_t failed_cases{0};
  size_t latency_score_count{0};
  size_t latency_fallback_count{0};
  size_t throughput_score_count{0};
  size_t throughput_fallback_count{0};
  size_t cpu_efficiency_score_count{0};
  size_t other_score_count{0};
  size_t send_block_score_count{0};
  size_t solo_case_count{0};
  size_t success_case_count{0};
  std::string confidence{"high"};
};

inline int confidence_severity(const std::string& label) {
  if (label == "unknown") {
    return 4;
  }

  if (label == "noisy") {
    return 3;
  }

  if (label == "solo") {
    return 2;
  }

  if (label == "medium") {
    return 1;
  }

  return 0;
}

inline void merge_confidence_label(std::string& target, const std::string& candidate) {
  if (target.empty() || confidence_severity(candidate) > confidence_severity(target)) {
    target = candidate.empty() ? "unknown" : candidate;
  }
}

inline std::vector<TransportSummary> summarize_by_endpoint(const std::vector<AggregatedCase>& items,
                                                           const std::set<Bench::Suite>* suite_filter = nullptr) {
  std::map<std::string, TransportSummary> agg;

  for (const auto& item : items) {
    if (suite_filter != nullptr && suite_filter->count(item.scenario.suite) == 0) {
      continue;
    }

    auto& s = agg[item.endpoint_key];

    if (s.endpoint_key.empty()) {
      s.endpoint_key = item.endpoint_key;
      s.transport = item.transport;
      s.url = item.scenario.url;
      s.endpoint_detail = summarize_endpoint_text(item);
      s.layer = item.deployment_layer;
      s.is_intra_rewritten = item.is_intra_rewritten;
    }

    if (item.scenario.suite == Bench::kLatencySuite && item.speed_score >= 0.0) {
      s.latency_score_sum += std::log1p(item.speed_score);
      ++s.latency_score_count;
      if (item.speed_score < s.min_speed_score) {
        s.min_speed_score = item.speed_score;
      }
    } else if ((item.scenario.suite == Bench::kThroughputSuite || item.scenario.suite == Bench::kTopologySuite ||
                item.scenario.suite == Bench::kBackpressureSuite) &&
               item.speed_score >= 0.0) {
      s.latency_fallback_sum += std::log1p(item.speed_score);
      ++s.latency_fallback_count;
    }

    if (item.scenario.suite == Bench::kThroughputSuite && item.capacity_score >= 0.0) {
      s.throughput_score_sum += std::log1p(item.capacity_score);
      ++s.throughput_score_count;
      s.min_capacity_score = std::min(s.min_capacity_score, item.capacity_score);
    } else if ((item.scenario.suite == Bench::kTopologySuite || item.scenario.suite == Bench::kBackpressureSuite) &&
               item.capacity_score >= 0.0) {
      s.throughput_fallback_sum += std::log1p(item.capacity_score);
      ++s.throughput_fallback_count;
    }

    if ((item.scenario.suite == Bench::kLatencySuite || item.scenario.suite == Bench::kThroughputSuite ||
         item.scenario.suite == Bench::kTopologySuite || item.scenario.suite == Bench::kBackpressureSuite) &&
        item.efficiency_score >= 0.0) {
      s.cpu_efficiency_score_sum += std::log1p(item.efficiency_score);
      ++s.cpu_efficiency_score_count;
    }

    if (item.scenario.suite == Bench::kTopologySuite && item.sort_score >= 0.0) {
      s.other_score_sum += item.sort_score;
      ++s.other_score_count;
    }

    if ((item.scenario.suite == Bench::kLatencySuite || item.scenario.suite == Bench::kBackpressureSuite) &&
        item.send_block_samples.count != 0) {
      const double sb_us = item.p99_send_block_us.average();
      const double sb_score = compute_absolute_send_block_score(sb_us, item.scenario.payload_size);
      s.send_block_score_sum += std::log1p(sb_score);
      ++s.send_block_score_count;
    }

    s.expected_sum += item.expected.sum;
    s.lost_sum += item.lost.sum;
    s.worst_loss_pct = std::max(s.worst_loss_pct, compute_loss_ratio_percent(item));
    s.worst_latency_drop_pct = std::max(s.worst_latency_drop_pct, compute_latency_drop_ratio_percent(item));
    ++s.cases;

    if (item.success_count == 0 || item.failure_count != 0) {
      ++s.failed_cases;
    } else {
      ++s.success_case_count;
    }

    if (item.confidence_label == "solo") {
      ++s.solo_case_count;
    }

    if (item.sort_score < 0.0) {
      continue;
    }
  }

  std::vector<TransportSummary> out;
  out.reserve(agg.size());

  for (auto& [k, s] : agg) {
    (void)k;

    if (s.cases == 0) {
      continue;
    }

    auto geometric_score = [](double log_sum, size_t count) {
      if (count == 0) {
        return 0.0;
      }

      return std::clamp(std::expm1(log_sum / static_cast<double>(count)), 0.0, 100.0);
    };
    if (s.latency_score_count > 0) {
      s.avg_speed = geometric_score(s.latency_score_sum, s.latency_score_count);
    } else if (s.latency_fallback_count > 0) {
      s.avg_speed = geometric_score(s.latency_fallback_sum, s.latency_fallback_count);
    } else {
      s.avg_speed = 0.0;
    }

    if (s.throughput_score_count > 0) {
      s.avg_capacity = geometric_score(s.throughput_score_sum, s.throughput_score_count);
    } else if (s.throughput_fallback_count > 0) {
      s.avg_capacity = geometric_score(s.throughput_fallback_sum, s.throughput_fallback_count);
    } else {
      s.avg_capacity = 0.0;
    }

    s.avg_efficiency = geometric_score(s.cpu_efficiency_score_sum, s.cpu_efficiency_score_count);
    s.avg_coverage =
        s.cases == 0 ? 0.0
                     : std::clamp(static_cast<double>(s.cases - s.failed_cases) * 100.0 / static_cast<double>(s.cases),
                                  0.0, 100.0);

    if (s.latency_score_count > 0 && s.min_speed_score < 100.5) {
      s.avg_speed = apply_speed_gate(s.avg_speed, s.min_speed_score);
    }

    if (s.throughput_score_count > 0 && s.min_capacity_score < 100.5) {
      s.avg_capacity = apply_capacity_gate(s.avg_capacity, s.min_capacity_score);
    }

    const double loss_percent = s.expected_sum <= 0.0 ? 0.0 : s.lost_sum * 100.0 / s.expected_sum;

    double avg_delivery = 0.0;

    if (s.success_case_count != 0) {
      avg_delivery = s.expected_sum <= 0.0 ? s.avg_coverage : compute_delivery_integrity_score(loss_percent);
    }

    s.avg_delivery = avg_delivery;

    {
      const auto& cfg_conf = score_config();
      const double confidence_loss_percent = std::max(loss_percent, s.worst_loss_pct);

      if (s.success_case_count == 0) {
        s.confidence = "unknown";
      } else if (confidence_loss_percent >= cfg_conf.confidence_endpoint_noisy_loss_pct ||
                 s.worst_latency_drop_pct >= cfg_conf.confidence_endpoint_noisy_loss_pct) {
        s.confidence = "noisy";
      } else if (confidence_loss_percent >= cfg_conf.confidence_endpoint_medium_loss_pct ||
                 s.worst_latency_drop_pct >= cfg_conf.confidence_endpoint_medium_loss_pct) {
        s.confidence = "medium";
      } else if (s.solo_case_count == s.cases) {
        s.confidence = "solo";
      } else {
        s.confidence = "high";
      }
    }

    if (s.send_block_score_count > 0) {
      const double raw_send_block = geometric_score(s.send_block_score_sum, s.send_block_score_count);
      s.avg_send_block = s.latency_score_count > 0 ? std::min(raw_send_block, s.avg_speed) : raw_send_block;
    } else {
      s.avg_send_block = s.success_case_count == 0 ? 0.0 : std::clamp(s.avg_coverage, 60.0, 95.0);
    }

    double avg_topology = 0.0;

    if (s.other_score_count > 0) {
      avg_topology = clamp_score(s.other_score_sum / static_cast<double>(s.other_score_count));
    } else if (s.success_case_count != 0) {
      avg_topology = std::clamp(s.avg_coverage, 60.0, 95.0);
    }

    s.avg_topology = avg_topology;

    const auto& cfg = score_config();
    const double weighted = s.avg_speed * cfg.latency_total_weight + s.avg_capacity * cfg.throughput_total_weight +
                            s.avg_efficiency * cfg.efficiency_total_weight +
                            s.avg_delivery * cfg.delivery_total_weight +
                            s.avg_send_block * cfg.send_block_total_weight +
                            s.avg_coverage * cfg.coverage_total_weight + s.avg_topology * cfg.topology_total_weight;
    s.avg_sort = clamp_total_score(weighted * confidence_multiplier(s.confidence));
    out.emplace_back(std::move(s));
  }

  return out;
}

inline std::string format_payload_size(size_t bytes) {
  std::string out;

  if (bytes >= 1024ULL * 1024ULL) {
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    format::format_to(std::back_inserter(out), "{:g} MB", mb);
  } else if (bytes >= 1024ULL) {
    const double kb = static_cast<double>(bytes) / 1024.0;
    format::format_to(std::back_inserter(out), "{:g} KB", kb);
  } else {
    format::format_to(std::back_inserter(out), "{} B", bytes);
  }

  return out;
}

enum class HeatmapMetric : uint8_t {
  kLatency,
  kThroughput,
};

inline bool collect_heatmap_metric(const AggregatedCase& item, HeatmapMetric metric, double& value, double& score) {
  if (metric == HeatmapMetric::kLatency) {
    if (item.p99_latency_us.count == 0) {
      return false;
    }

    value = item.p99_latency_us.average();
    score = compute_absolute_latency_score(value, item.scenario.payload_size);
    return true;
  }

  if (item.recv_mb_per_sec.count == 0) {
    return false;
  }

  value = item.recv_mb_per_sec.average();
  score = compute_absolute_throughput_score(value);
  return true;
}

inline std::string format_heatmap_metric_value(HeatmapMetric metric, double value) {
  if (metric == HeatmapMetric::kLatency) {
    return format_decimal(value, value >= 100.0 ? 0 : 1) + " us";
  }

  return format_decimal(value, metric_value_precision(value)) + " MB/s";
}

inline int compute_heatmap_fill_percent(HeatmapMetric metric, int quality_score) {
  if (metric == HeatmapMetric::kLatency) {
    const int heat = std::clamp(100 - quality_score, 0, 100);
    return std::clamp(8 + static_cast<int>(std::round(static_cast<double>(heat) * 0.92)), 8, 100);
  }

  return std::clamp(8 + static_cast<int>(std::round(static_cast<double>(quality_score) * 0.92)), 8, 100);
}

inline const char* get_heatmap_color_class(HeatmapMetric metric, int quality_score) {
  if (metric == HeatmapMetric::kLatency) {
    const int heat = std::clamp(100 - quality_score, 0, 100);

    if (heat >= 70) {
      return "heat-weak";
    }

    if (heat >= 35) {
      return "heat-med";
    }

    return "heat-strong";
  }

  if (quality_score >= 85) {
    return "heat-strong";
  }

  if (quality_score >= 60) {
    return "heat-med";
  }

  return "heat-weak";
}

inline std::string build_metric_heatmap_block(const std::vector<AggregatedCase>& items, HeatmapMetric metric) {
  struct HeatmapCell final {
    double score_sum{0.0};
    double value_sum{0.0};
    double p50_sum{0.0};
    double p90_sum{0.0};
    double loss_sum{0.0};
    size_t metric_count{0};
    size_t p50_count{0};
    size_t p90_count{0};
    size_t case_count{0};
    size_t failure_count{0};
  };

  const int suite = metric == HeatmapMetric::kLatency ? Bench::kLatencySuite : Bench::kThroughputSuite;
  std::set<size_t> sizes_set;
  std::map<std::pair<std::string, size_t>, HeatmapCell> cells;
  std::map<std::string, DeploymentLayer> endpoint_layer;
  std::map<std::string, std::pair<std::string, std::string>> endpoint_label;
  std::set<std::string> endpoints_set;

  std::map<std::pair<std::string, size_t>, std::vector<double>> throughput_values;
  std::map<size_t, double> column_max_value;
  std::map<size_t, double> column_min_value;

  for (const auto& item : items) {
    if (item.endpoint_key.empty() || item.scenario.suite == Bench::kSerializationSuite) {
      continue;
    }

    endpoints_set.insert(item.endpoint_key);
    endpoint_layer[item.endpoint_key] = item.deployment_layer;
    endpoint_label[item.endpoint_key] = {item.transport, summarize_endpoint_text(item)};

    if (item.scenario.suite != suite || item.scenario.payload_size == 0) {
      continue;
    }

    sizes_set.insert(item.scenario.payload_size);

    auto key = std::make_pair(item.endpoint_key, item.scenario.payload_size);
    auto& cell = cells[key];
    ++cell.case_count;
    cell.failure_count += item.failure_count;

    double value = 0.0;
    double score = 0.0;

    if (collect_heatmap_metric(item, metric, value, score)) {
      if (metric == HeatmapMetric::kThroughput) {
        throughput_values[key].push_back(value);
        cell.value_sum += value;
        ++cell.metric_count;
      } else {
        cell.score_sum += score;
        cell.value_sum += value;
        ++cell.metric_count;

        if (item.p50_latency_us.count > 0) {
          cell.p50_sum += item.p50_latency_us.average();
          ++cell.p50_count;
        }

        if (item.p90_latency_us.count > 0) {
          cell.p90_sum += item.p90_latency_us.average();
          ++cell.p90_count;
        }
      }

      if (value > 0.0 && std::isfinite(value)) {
        const auto col_max_iter = column_max_value.find(item.scenario.payload_size);
        if (col_max_iter == column_max_value.end() || value > col_max_iter->second) {
          column_max_value[item.scenario.payload_size] = value;
        }
        const auto col_min_iter = column_min_value.find(item.scenario.payload_size);
        if (col_min_iter == column_min_value.end() || value < col_min_iter->second) {
          column_min_value[item.scenario.payload_size] = value;
        }
      }

      cell.loss_sum += compute_loss_ratio_percent(item);
    }
  }

  if (metric == HeatmapMetric::kThroughput) {
    for (auto& [key, cell] : cells) {
      if (cell.metric_count == 0) {
        continue;
      }

      const double col_max = column_max_value.count(key.second) ? column_max_value[key.second] : 0.0;
      const double col_min = column_min_value.count(key.second) ? column_min_value[key.second] : 0.0;

      if (col_max <= 0.0) {
        cell.score_sum = 0.0;
        continue;
      }

      const double log_span = std::log(std::max(col_max / std::max(col_min, 1e-9), 1.0 + 1e-6));
      double sum = 0.0;
      for (double v : throughput_values[key]) {
        double relative = 100.0;

        if (v > 0.0 && col_min > 0.0 && std::isfinite(v)) {
          const double t = std::log(std::max(v, 1e-9) / std::max(col_min, 1e-9)) / log_span;
          relative = std::clamp(t * 100.0, 0.0, 100.0);
        } else if (v <= 0.0) {
          relative = 0.0;
        }

        const double absolute = compute_absolute_throughput_score(v);
        sum += blend_absolute_relative_score(absolute, relative, 0.50);
      }

      cell.score_sum = sum;
    }
  }

  std::map<std::pair<std::string, size_t>, std::array<double, 3>> latency_rank_heat;

  if (metric == HeatmapMetric::kLatency) {
    for (size_t sz : sizes_set) {
      struct ColRow final {
        std::string endpoint;
        double p50;
        double p90;
        double p99;
      };

      std::vector<ColRow> rows;
      for (const auto& [k, c] : cells) {
        if (k.second != sz || c.metric_count == 0) {
          continue;
        }

        const auto cnt = static_cast<double>(c.metric_count);
        ColRow r{k.first, c.p50_count > 0 ? c.p50_sum / static_cast<double>(c.p50_count) : -1.0,
                 c.p90_count > 0 ? c.p90_sum / static_cast<double>(c.p90_count) : -1.0, c.value_sum / cnt};
        rows.emplace_back(std::move(r));
      }

      if (rows.empty()) {
        continue;
      }

      auto log_heat_for = [&rows](auto getter) {
        double col_min = std::numeric_limits<double>::infinity();
        double col_max = 0.0;
        for (const auto& r : rows) {
          const double v = getter(r);

          if (v > 0.0 && std::isfinite(v)) {
            col_min = std::min(col_min, v);
            col_max = std::max(col_max, v);
          }
        }

        std::map<std::string, double> out;

        if (!std::isfinite(col_min) || col_max <= 0.0) {
          for (const auto& r : rows) {
            out[r.endpoint] = -1.0;
          }
          return out;
        }

        const double log_span = std::log(std::max(col_max / std::max(col_min, 1e-9), 1.0 + 1e-6));
        for (const auto& r : rows) {
          const double v = getter(r);

          if (v <= 0.0 || !std::isfinite(v)) {
            out[r.endpoint] = -1.0;
            continue;
          }

          const double t = std::log(std::max(v, 1e-9) / std::max(col_min, 1e-9)) / log_span;
          out[r.endpoint] = std::clamp(t * 100.0, 0.0, 100.0);
        }
        return out;
      };

      const auto p50_heat = log_heat_for([](const ColRow& r) { return r.p50; });
      const auto p90_heat = log_heat_for([](const ColRow& r) { return r.p90; });
      const auto p99_heat = log_heat_for([](const ColRow& r) { return r.p99; });

      for (const auto& r : rows) {
        auto pick = [](const std::map<std::string, double>& m, const std::string& ep) {
          auto it = m.find(ep);
          return it == m.end() ? -1.0 : it->second;
        };
        latency_rank_heat[{r.endpoint, sz}] = {pick(p50_heat, r.endpoint), pick(p90_heat, r.endpoint),
                                               pick(p99_heat, r.endpoint)};
      }
    }
  }

  if (sizes_set.empty() || endpoints_set.empty()) {
    return {};
  }

  std::vector<size_t> sizes(sizes_set.begin(), sizes_set.end());
  std::vector<std::string> endpoints(endpoints_set.begin(), endpoints_set.end());
  const size_t heatmap_min_width = 160 + sizes.size() * 104;
  std::string heatmap_row_style;
  format::format_to(std::back_inserter(heatmap_row_style),
                    R"( style="grid-template-columns:160px repeat({},minmax(96px,1fr));min-width:{}px")", sizes.size(),
                    heatmap_min_width);
  std::map<std::string, double> endpoint_score_sum;
  std::map<std::string, size_t> endpoint_score_count;

  for (const auto& [key, cell] : cells) {
    if (cell.metric_count == 0) {
      continue;
    }

    endpoint_score_sum[key.first] += cell.score_sum / static_cast<double>(cell.metric_count);
    ++endpoint_score_count[key.first];
  }

  auto endpoint_avg_score = [&endpoint_score_count, &endpoint_score_sum](const std::string& endpoint) {
    const auto count_iter = endpoint_score_count.find(endpoint);

    if (count_iter == endpoint_score_count.end() || count_iter->second == 0) {
      return -1.0;
    }

    const double score = endpoint_score_sum[endpoint] / static_cast<double>(count_iter->second);
    return std::isfinite(score) ? score : -1.0;
  };

  std::sort(endpoints.begin(), endpoints.end(),
            [&endpoint_avg_score, &endpoint_layer, &endpoint_label](const std::string& a, const std::string& b) {
              const double sa = endpoint_avg_score(a);
              const double sb = endpoint_avg_score(b);

              if (sa != sb) {
                return sa > sb;
              }

              const int la = static_cast<int>(endpoint_layer[a]);
              const int lb = static_cast<int>(endpoint_layer[b]);

              if (la != lb) {
                return la < lb;
              }

              return endpoint_label[a] < endpoint_label[b];
            });

  std::string out;
  out.reserve(2048);

  if (metric == HeatmapMetric::kLatency) {
    out +=
        R"(<div class="heatmap-block"><h3 data-i18n="heatmap_title">Latency by Message Size (P50 / P90 / P99, lower is better)</h3>)";
    out +=
        R"(<p class="note" data-i18n="heatmap_note">Each cell has three bars: P50 / P90 / P99 (left to right), shorter and greener is better. Numbers on the right follow the same order, in &micro;s. Blank = no data, FAIL = case failed.</p>)";
    out +=
        R"(<div class="heatmap-legend latency-legend"><span class="heat-chip heat-strong" data-i18n="heatmap_latency_good">low latency</span><span class="heat-chip heat-med" data-i18n="heatmap_latency_mid">medium latency</span><span class="heat-chip heat-weak" data-i18n="heatmap_latency_bad">high latency</span></div>)";
  } else {
    out +=
        R"(<div class="heatmap-block"><h3 data-i18n="heatmap_throughput_title">Throughput by Message Size (received MB/s, higher is better)</h3>)";
    out +=
        R"(<p class="note" data-i18n="heatmap_throughput_note">This chart uses throughput tests only. Each row is one test target and each column is a message size. The number is mean received MB/s. Longer greener horizontal bars mean the subscriber actually received more data. Blank or failed cells have no valid result.</p>)";
  }

  out += R"(<div class="heatmap-scroll"><div class="heatmap-grid">)";
  out += R"(<div class="heatmap-row heatmap-head")";
  out += heatmap_row_style;
  out +=
      R"(><div class="heatmap-cell head-transport" data-i18n="heatmap_head_transport">test target \ message size</div>)";

  for (const auto& sz : sizes) {
    out += R"(<div class="heatmap-cell head-size">)";
    out += escape_html(format_payload_size(sz));
    out += "</div>";
  }

  out += "</div>";

  for (const auto& endpoint : endpoints) {
    const auto label = endpoint_label[endpoint];
    out += R"(<div class="heatmap-row")";
    out += heatmap_row_style;
    out += R"(><div class="heatmap-cell name"><code>)";
    out += escape_html(label.first);
    out += R"(://</code><small>)";
    out += escape_html(label.second);
    out += R"(</small></div>)";

    for (const auto& sz : sizes) {
      auto it = cells.find(std::make_pair(endpoint, sz));

      if (it == cells.end()) {
        out += R"(<div class="heatmap-cell empty">&mdash;</div>)";
        continue;
      }

      if (it->second.metric_count == 0) {
        out += R"(<div class="heatmap-cell empty heatmap-fail"><span data-i18n="status_fail">FAIL</span></div>)";
        continue;
      }

      const auto count = static_cast<double>(it->second.metric_count);
      const double score = it->second.score_sum / count;
      const double metric_value = it->second.value_sum / count;
      const double loss_percent = it->second.loss_sum / count;
      const std::string value_text = format_heatmap_metric_value(metric, metric_value);
      const int quality_pct = static_cast<int>(std::round(std::min(100.0, std::max(0.0, score))));
      const int fill_pct = compute_heatmap_fill_percent(metric, quality_pct);
      const char* color_class = get_heatmap_color_class(metric, quality_pct);
      const char* title_prefix = metric == HeatmapMetric::kLatency ? "latency score" : "throughput score";
      const std::string small_text = std::to_string(quality_pct) + "/100";
      const std::string loss_title = should_report_delivery_loss(loss_percent)
                                         ? " / loss " + escape_html(format_decimal(loss_percent, 3)) + "%"
                                         : std::string();

      if (metric == HeatmapMetric::kLatency) {
        auto fmt_us = [](double avg) { return format_decimal(avg, avg >= 100.0 ? 0 : 1); };
        const double p50_avg =
            it->second.p50_count > 0 ? it->second.p50_sum / static_cast<double>(it->second.p50_count) : 0.0;
        const double p90_avg =
            it->second.p90_count > 0 ? it->second.p90_sum / static_cast<double>(it->second.p90_count) : 0.0;
        const double p99_avg = metric_value;
        const std::string p50_text = it->second.p50_count > 0 ? fmt_us(p50_avg) : std::string("-");
        const std::string p90_text = it->second.p90_count > 0 ? fmt_us(p90_avg) : std::string("-");
        const std::string p99_text = fmt_us(p99_avg);

        const double p50_score = it->second.p50_count > 0 ? compute_absolute_latency_score(p50_avg, sz) : -1.0;
        const double p90_score = it->second.p90_count > 0 ? compute_absolute_latency_score(p90_avg, sz) : -1.0;
        const double p99_score = compute_absolute_latency_score(p99_avg, sz);

        const auto rank_it = latency_rank_heat.find({endpoint, sz});
        const std::array<double, 3> rank_heat_arr =
            rank_it != latency_rank_heat.end() ? rank_it->second : std::array<double, 3>{-1.0, -1.0, -1.0};

        auto bar_heat_pct = [](double sc, double rank_h) {
          const double abs_h = sc < 0.0 ? -1.0 : std::clamp(100.0 - sc, 0.0, 100.0);
          const double rel_h = rank_h < 0.0 ? -1.0 : std::clamp(rank_h, 0.0, 100.0);

          if (rel_h >= 0.0 && abs_h >= 0.0) {
            return static_cast<int>(std::round(0.55 * rel_h + 0.45 * abs_h));
          }

          if (rel_h >= 0.0) {
            return static_cast<int>(std::round(rel_h));
          }

          if (abs_h >= 0.0) {
            return static_cast<int>(std::round(abs_h));
          }

          return 100;
        };

        auto bar_fill = [&bar_heat_pct](double sc, double rank_h) {
          if (rank_h < 0.0 && sc < 0.0) {
            return 100;
          }

          return std::clamp(8 + bar_heat_pct(sc, rank_h) * 92 / 100, 8, 100);
        };

        format::format_to(
            std::back_inserter(out),
            R"(<div class="heatmap-cell score latency" title="{} {} / P50 {} / P90 {} / P99 {} us{} / failures {}">)"
            R"(<div class="lat-meters">)"
            R"(<span class="heatmap-latency-meter" title="P50"><span class="heatmap-latency-fill" style="--heat-pct:{};height:{}%"></span></span>)"
            R"(<span class="heatmap-latency-meter" title="P90"><span class="heatmap-latency-fill" style="--heat-pct:{};height:{}%"></span></span>)"
            R"(<span class="heatmap-latency-meter" title="P99"><span class="heatmap-latency-fill" style="--heat-pct:{};height:{}%"></span></span>)"
            R"(</div>)"
            R"(<div class="heatmap-num lat-stack">)"
            R"(<div class="lat-row"><span class="lat-tag">P50</span><strong>{}</strong><span class="lat-unit">us</span></div>)"
            R"(<div class="lat-row"><span class="lat-tag">P90</span><strong>{}</strong><span class="lat-unit">us</span></div>)"
            R"(<div class="lat-row"><span class="lat-tag">P99</span><strong>{}</strong><span class="lat-unit">us</span></div>)"
            R"(<small>{}</small></div></div>)",
            title_prefix, quality_pct, escape_html(p50_text), escape_html(p90_text), escape_html(p99_text), loss_title,
            it->second.failure_count, bar_heat_pct(p50_score, rank_heat_arr[0]), bar_fill(p50_score, rank_heat_arr[0]),
            bar_heat_pct(p90_score, rank_heat_arr[1]), bar_fill(p90_score, rank_heat_arr[1]),
            bar_heat_pct(p99_score, rank_heat_arr[2]), bar_fill(p99_score, rank_heat_arr[2]), escape_html(p50_text),
            escape_html(p90_text), escape_html(p99_text), escape_html(small_text));
      } else {
        format::format_to(std::back_inserter(out),
                          R"(<div class="heatmap-cell score throughput" title="{} {} / value {}{} / failures {}">)"
                          R"(<div class="heatmap-bar {}" style="width:{}%"></div>)"
                          R"(<span class="heatmap-num"><strong>{}</strong><small>{}</small></span></div>)",
                          title_prefix, quality_pct, escape_html(value_text), loss_title, it->second.failure_count,
                          color_class, fill_pct, escape_html(value_text), escape_html(small_text));
      }
    }

    out += "</div>";
  }

  out += "</div></div></div>";
  return out;
}

inline std::string build_score_heatmap(const std::vector<AggregatedCase>& items) {
  const auto latency_block = build_metric_heatmap_block(items, HeatmapMetric::kLatency);
  const auto throughput_block = build_metric_heatmap_block(items, HeatmapMetric::kThroughput);

  if (latency_block.empty() && throughput_block.empty()) {
    return {};
  }

  std::string out;
  out.reserve(latency_block.size() + throughput_block.size() + 96);
  out += R"(<section id="heatmap" class="panel heatmap-panel" aria-labelledby="heatmap-h">)";
  out += R"(<h2 id="heatmap-h" style="display:inline-flex"><span class="h-anchor" aria-hidden="true">04</span>)"
         R"(<span data-i18n="nav_heatmap">Latency / Throughput</span></h2>)";
  out += latency_block;
  out += throughput_block;
  out += "</section>";
  return out;
}

inline const char* deployment_layer_label(DeploymentLayer layer) {
  switch (layer) {
    case DeploymentLayer::kInProcess:
      return "in-process";
    case DeploymentLayer::kCrossProcess:
      return "cross-process";
    case DeploymentLayer::kUnknown:
      return "unknown";
  }

  return "?";
}

inline const char* deployment_layer_i18n_key(DeploymentLayer layer) {
  switch (layer) {
    case DeploymentLayer::kInProcess:
      return "layer_inprocess";
    case DeploymentLayer::kCrossProcess:
      return "layer_crossprocess";
    case DeploymentLayer::kUnknown:
      return "layer_unknown";
  }

  return "layer_unknown";
}

inline const char* medal_glyph_for_rank(int rank) {
  switch (rank) {
    case 1:
      return "&#129351;";
    case 2:
      return "&#129352;";
    case 3:
      return "&#129353;";
    default:
      return "";
  }
}

inline double transport_summary_loss_percent(const TransportSummary& summary) {
  if (summary.expected_sum <= 0.0) {
    return summary.worst_loss_pct;
  }

  const double aggregate_loss = summary.lost_sum * 100.0 / summary.expected_sum;
  return std::max(aggregate_loss, summary.worst_loss_pct);
}

inline void append_loss_pill(std::string& out, double loss_percent) {
  if (!should_report_delivery_loss(loss_percent)) {
    return;
  }

  const bool severe_loss = loss_percent > 20.0;
  format::format_to(
      std::back_inserter(out),
      R"HTML(<span class="score-pill loss{}" data-i18n-title="pill_loss_title" title="Loss ratio">&#9888; <span data-i18n="pill_loss_label">Loss</span> {}%</span>)HTML",
      severe_loss ? " loss-high" : "", escape_html(format_decimal(loss_percent, 3)));
}

inline std::string build_transport_profile_cards(const std::vector<AggregatedCase>& items) {
  const std::set<Bench::Suite> recommendation_suites{Bench::kThroughputSuite, Bench::kLatencySuite,
                                                     Bench::kTopologySuite, Bench::kBackpressureSuite};
  auto transports = summarize_by_endpoint(items, &recommendation_suites);

  if (transports.empty()) {
    return {};
  }

  std::sort(transports.begin(), transports.end(), [](const TransportSummary& a, const TransportSummary& b) {
    const double sa = std::isfinite(a.avg_sort) ? a.avg_sort : -1.0;
    const double sb = std::isfinite(b.avg_sort) ? b.avg_sort : -1.0;

    if (sa != sb) {
      return sa > sb;
    }

    if (a.layer != b.layer) {
      return static_cast<int>(a.layer) < static_cast<int>(b.layer);
    }

    return a.endpoint_detail < b.endpoint_detail;
  });

  struct TypicalNumbers final {
    double best_p95_us{-1.0};
    size_t best_p95_size{0};
    int best_p95_rate{0};
    double weakest_latency_score{101.0};
    double weakest_latency_p95_us{-1.0};
    double weakest_latency_p999_us{-1.0};
    size_t weakest_latency_size{0};
    int weakest_latency_rate{0};
    double peak_mb_s{-1.0};
    size_t peak_size{0};
    MetricSummary cpu_usage;
    double best_send_block_p99_us{-1.0};
    size_t best_send_block_size{0};
  };

  std::map<std::string, TypicalNumbers> typicals;

  for (const auto& item : items) {
    if (item.success_count == 0 || item.failure_count != 0) {
      continue;
    }

    auto& t = typicals[item.endpoint_key];

    if (item.scenario.suite == Bench::kLatencySuite) {
      const double p95 = item.p95_latency_us.average();
      if (!has_delivery_loss(item) && item.latency_samples_dropped == 0 && p95 > 0.0 &&
          (t.best_p95_us < 0.0 || p95 < t.best_p95_us)) {
        t.best_p95_us = p95;
        t.best_p95_size = item.scenario.payload_size;
        t.best_p95_rate = item.scenario.rate_hz;
      }

      if (p95 > 0.0 && item.speed_score >= 0.0 && item.speed_score < t.weakest_latency_score) {
        t.weakest_latency_score = item.speed_score;
        t.weakest_latency_p95_us = p95;
        t.weakest_latency_p999_us = item.p999_latency_us.average();
        t.weakest_latency_size = item.scenario.payload_size;
        t.weakest_latency_rate = item.scenario.rate_hz;
      }
    }

    if (item.scenario.suite == Bench::kThroughputSuite) {
      const double mb = item.recv_mb_per_sec.average();
      if (mb > t.peak_mb_s) {
        t.peak_mb_s = mb;
        t.peak_size = item.scenario.payload_size;
      }
    }

    if (item.send_block_samples.count != 0) {
      const double sb_p99 = item.p99_send_block_us.average();
      if (sb_p99 > 0.0 && (t.best_send_block_p99_us < 0.0 || sb_p99 < t.best_send_block_p99_us)) {
        t.best_send_block_p99_us = sb_p99;
        t.best_send_block_size = item.scenario.payload_size;
      }
    }

    if (item.cpu_usage.count != 0) {
      t.cpu_usage.add(item.cpu_usage.average());
    }
  }

  std::string out;
  out.reserve(4096);
  out +=
      R"(<section id="decision" class="panel profile-panel decision-panel"><h2 data-i18n="profile_title">Recommended Targets</h2>)";
  out +=
      R"(<p class="note" data-i18n="profile_note">The cards are sorted by total score for this run. Each card is one tested URL or transport configuration; open it to see the URL, run mode, best and weakest latency cases, throughput, CPU, and counted cases.</p>)";
  out +=
      R"HTML(<p class="note" data-i18n="profile_scoring_legend">Total score is out of 120. Higher is better for the current test mix. Sub-pills show: latency / throughput / resource / coverage / delivery / send-block. The "Recommended Targets" headline shows the strongest URL for this run.</p>)HTML";

  out += R"HTML(<details class="profile-formula">)HTML";
  out += R"HTML(<summary data-i18n="profile_formula_title">Total score formula (max 120)</summary>)HTML";
  out += R"HTML(<ul class="formula-list">)HTML";
  out +=
      R"HTML(<li><strong data-i18n="profile_formula_latency">Latency 30%</strong> <span data-i18n="profile_formula_latency_desc">— Subscriber-side end-to-end latency. Per case we weight P95×0.70 + P99×0.25 + P99.9×0.04 + P99.99×0.01 through a payload-aware curve, then use an equal-weight geometric mean across cases.</span></li>)HTML";
  out +=
      R"HTML(<li><strong data-i18n="profile_formula_throughput">Throughput 20%</strong> <span data-i18n="profile_formula_throughput_desc">— recv MB/s on the subscriber side, log-scaled vs 1024 MB/s reference. Higher throughput → higher score.</span></li>)HTML";
  out +=
      R"HTML(<li><strong data-i18n="profile_formula_resource">Resource 10%</strong> <span data-i18n="profile_formula_resource_desc">— throughput per unit CPU. Lower CPU at the same throughput → higher score.</span></li>)HTML";
  out +=
      R"HTML(<li><strong data-i18n="profile_formula_delivery">Delivery 10%</strong> <span data-i18n="profile_formula_delivery_desc">— Aggregated loss across all cases: avg_delivery = 100 - loss%. Loss > 5% also lightly cuts per-case latency via a logistic curve (gentle ≤20%, accelerates >20%, capped at ×0.40) to defeat survivor-bias.</span></li>)HTML";
  out +=
      R"HTML(<li><strong data-i18n="profile_formula_send_block">Send-block 10%</strong> <span data-i18n="profile_formula_send_block_desc">— wall-clock time spent inside publish() per message (P99), equal-weight across latency/backpressure cases. Penalizes APIs that block the publisher thread.</span></li>)HTML";
  out +=
      R"HTML(<li><strong data-i18n="profile_formula_coverage">Coverage 10%</strong> <span data-i18n="profile_formula_coverage_desc">— Passing cases / total cases for this URL. Standalone dimension; also serves as fallback for the topology slot when topology suite is absent.</span></li>)HTML";
  out +=
      R"HTML(<li><strong data-i18n="profile_formula_topology">Topology 10%</strong> <span data-i18n="profile_formula_topology_desc">— Topology suite roll-up (1:n / n:1 / n:n). Falls back to clamp(coverage, 60, 95) when topology suite was not run.</span></li>)HTML";
  out += R"HTML(</ul>)HTML";
  out +=
      R"HTML(<p class="formula-equation"><code data-i18n="profile_formula_equation">total = (0.36·latency + 0.24·throughput + 0.12·resource + 0.12·delivery + 0.12·send-block + 0.12·coverage + 0.12·topology) · confidence_factor</code></p>)HTML";
  out +=
      R"HTML(<p class="formula-note" data-i18n="profile_formula_confidence_note">Confidence is derived from aggregate/worst-case loss% and latency sample drops: &lt;5% high, &lt;15% medium, &ge;15% noisy; solo means no peers, unknown means all cases failed. Multipliers: high 1.00 / solo·medium 0.99 / unknown·noisy 0.95.</p>)HTML";
  out += R"HTML(</details>)HTML";

  if (!transports.empty()) {
    const auto& top = transports.front();
    out +=
        R"(<p class="recommendation-lead"><span data-i18n="profile_top_pick_prefix">Top recommendation:</span> <code>)";
    out += escape_html(top.transport);
    out += R"(://</code> <strong><span data-i18n="dec_score_combined">total</span> )";
    out += escape_html(format_decimal(top.avg_sort, 0));
    out +=
        R"(/120</strong><br><span data-i18n="profile_top_pick_reason">It is the strongest result in this run; use latency, throughput, and Transport Health below to confirm details.</span></p>)";
  }

  size_t rank = 1;
  for (const auto& ts : transports) {
    const auto& typ = typicals[ts.endpoint_key];
    const std::string speed_score_text = format_decimal(ts.avg_speed, 0);
    const std::string capacity_score_text = format_decimal(ts.avg_capacity, 0);
    const std::string efficiency_score_text = format_decimal(ts.avg_efficiency, 0);
    const std::string coverage_score_text = format_decimal(ts.avg_coverage, 0);
    const std::string delivery_score_text = format_decimal(ts.avg_delivery, 0);
    const std::string total_score_text = format_decimal(ts.avg_sort, 0);

    {
      const double score_ratio = std::clamp(ts.avg_sort / 120.0, 0.0, 1.0);
      const int hue = static_cast<int>(std::round(score_ratio * 130.0));
      const int boost = static_cast<int>(std::round(score_ratio * 100.0));
      format::format_to(std::back_inserter(out),
                        R"(<details class="profile-card" style="--score-hue:{};--score-pct:{}">)", hue, boost);
    }

    out += R"(<summary><div class="profile-head">)";

    const char* layer_key = deployment_layer_i18n_key(ts.layer);
    const std::string confidence_i18n_key = "confidence_" + ts.confidence;
    const char* medal_glyph = medal_glyph_for_rank(rank);

    if (medal_glyph[0] != '\0') {
      format::format_to(std::back_inserter(out), R"HTML(<span class="medal-glyph" aria-hidden="true">{}</span>)HTML",
                        medal_glyph);
    }

    const std::string send_block_score_text = format_decimal(ts.avg_send_block, 0);

    format::format_to(
        std::back_inserter(out),
        R"HTML(<span class="medal">#{}</span>)HTML"
        R"HTML(<code class="profile-name">{}://</code>)HTML"
        R"HTML(<span class="profile-layer" data-i18n="{}">{}</span>)HTML"
        R"HTML(<span class="score-pill speed" data-i18n-title="pill_speed_title" title="Latency sub-score (0-100). Lower tail latency scores higher.">&#9889; <span data-i18n="pill_speed_label">Latency score</span> {}</span>)HTML"
        R"HTML(<span class="score-pill capacity" data-i18n-title="pill_capacity_title" title="Throughput sub-score (0-100). Higher received MB/s scores higher.">&#128230; <span data-i18n="pill_capacity_label">Throughput score</span> {}</span>)HTML"
        R"HTML(<span class="score-pill efficiency" data-i18n-title="pill_efficiency_title" title="Resource sub-score (0-100). Higher throughput per CPU scores higher.">&#128267; <span data-i18n="pill_efficiency_label">Resource score</span> {}</span>)HTML"
        R"HTML(<span class="score-pill coverage" data-i18n-title="pill_coverage_title" title="Test coverage sub-score (0-100). Passing cases over total cases attempted.">&#127919; <span data-i18n="pill_coverage_label">Coverage score</span> {}</span>)HTML"
        R"HTML(<span class="score-pill delivery" data-i18n-title="pill_delivery_title" title="Delivery integrity sub-score (0-100). Derived from packet loss; lower loss scores higher.">&#128230; <span data-i18n="pill_delivery_label">Delivery score</span> {}</span>)HTML"
        R"HTML(<span class="score-pill send-block" data-i18n-title="pill_send_block_title" title="Send-block sub-score (0-100). Lower publish() blocking time scores higher; weight 10% of total.">&#128274; <span data-i18n="pill_send_block_label">Send-block score</span> {}</span>)HTML"
        R"HTML(<span class="score-sum" data-i18n-title="dec_score_total_title" title="Total score, max 120. Weights: 30% latency + 20% throughput + 10% resource + 10% delivery + 10% send-block + 10% coverage + 10% topology, then multiplied by confidence (high 1.00 / solo·medium 0.99 / unknown·noisy 0.95)."><span data-i18n="dec_score_combined">total</span> {}<span class="score-sum-max" aria-hidden="true"> /120</span></span>)HTML"
        R"HTML(<span class="confidence confidence-{}"><span data-i18n="dec_confidence_prefix">conf</span> <span data-i18n="{}">{}</span></span>)HTML",
        rank, escape_html(ts.transport), layer_key, deployment_layer_label(ts.layer), escape_html(speed_score_text),
        escape_html(capacity_score_text), escape_html(efficiency_score_text), escape_html(coverage_score_text),
        escape_html(delivery_score_text), escape_html(send_block_score_text), escape_html(total_score_text),
        ts.confidence, confidence_i18n_key, ts.confidence);

    append_loss_pill(out, transport_summary_loss_percent(ts));

    out += R"(</div></summary>)";
    out += R"(<div class="profile-body">)";

    out += R"(<div class="profile-facts">)";

    format::format_to(
        std::back_inserter(out),
        R"(<div class="fact-row"><span class="fact-label" data-i18n="profile_fact_url_prefix">protocol/transport</span><span class="fact-val"><code>{}://</code></span></div>)",
        escape_html(ts.transport));

    format::format_to(
        std::back_inserter(out),
        R"(<div class="fact-row"><span class="fact-label" data-i18n="th_url">URL</span><span class="fact-val"><code>{}</code></span></div>)",
        escape_html(ts.url));

    out +=
        R"(<div class="fact-row"><span class="fact-label" data-i18n="profile_fact_deployment">run mode</span><span class="fact-val" data-i18n=")";
    out += layer_key;
    out += R"(">)";
    out += deployment_layer_label(ts.layer);
    out += "</span></div>";

    out += "</div>";

    out += R"(<div class="profile-numbers">)";

    if (typ.best_p95_us > 0.0) {
      format::format_to(
          std::back_inserter(out),
          R"(<div class="num-row"><span class="num-label" data-i18n="profile_num_best_p95">best P95 latency</span>)"
          R"(<span class="num-val">{:.1f} us</span>)"
          R"(<span class="num-ctx">@ {} / {} Hz</span></div>)",
          typ.best_p95_us, format_payload_size(typ.best_p95_size), typ.best_p95_rate);
    }

    if (typ.weakest_latency_p95_us > 0.0 && typ.weakest_latency_score <= 100.0) {
      format::format_to(
          std::back_inserter(out),
          R"(<div class="num-row"><span class="num-label" data-i18n="profile_num_weakest_latency">weakest latency case</span>)"
          R"(<span class="num-val">{:.0f}/100</span>)"
          R"(<span class="num-ctx">@ {} / {} Hz, P95 {:.1f} us, P99.9 {:.1f} us</span></div>)",
          typ.weakest_latency_score, format_payload_size(typ.weakest_latency_size), typ.weakest_latency_rate,
          typ.weakest_latency_p95_us, typ.weakest_latency_p999_us);
    }

    if (typ.peak_mb_s > 0.0) {
      format::format_to(
          std::back_inserter(out),
          R"(<div class="num-row"><span class="num-label" data-i18n="profile_num_peak">peak received throughput</span>)"
          R"(<span class="num-val">{:.1f} MB/s</span>)"
          R"(<span class="num-ctx">@ {}</span></div>)",
          typ.peak_mb_s, format_payload_size(typ.peak_size));
    }

    if (typ.cpu_usage.count != 0) {
      format::format_to(std::back_inserter(out),
                        R"(<div class="num-row"><span class="num-label" data-i18n="profile_num_avg_cpu">avg CPU</span>)"
                        R"(<span class="num-val">{:.1f}%</span>)"
                        R"(<span class="num-ctx" data-i18n="profile_num_cpu_ctx">publisher + subscriber</span></div>)",
                        typ.cpu_usage.average());
    }

    if (typ.best_send_block_p99_us >= 0.0) {
      format::format_to(
          std::back_inserter(out),
          R"(<div class="num-row"><span class="num-label" data-i18n="profile_num_best_send_block">best send-block P99</span>)"
          R"(<span class="num-val">{:.2f} us</span>)"
          R"(<span class="num-ctx">@ {}</span></div>)",
          typ.best_send_block_p99_us, format_payload_size(typ.best_send_block_size));
    }

    {
      const double loss_percent = transport_summary_loss_percent(ts);
      const char* num_class = "num-val";

      if (loss_percent >= 20.0) {
        num_class = "num-val fail";
      } else if (loss_percent >= 5.0) {
        num_class = "num-val warn";
      }

      format::format_to(std::back_inserter(out),
                        R"(<div class="num-row"><span class="num-label" data-i18n="profile_num_loss">loss rate</span>)"
                        R"(<span class="{}">{}%</span>)"
                        R"(<span class="num-ctx" data-i18n="profile_num_loss_ctx">across all cases</span></div>)",
                        num_class, escape_html(format_decimal(loss_percent, 3)));
    }

    format::format_to(
        std::back_inserter(out),
        R"(<div class="num-row"><span class="num-label" data-i18n="profile_num_cases">counted runs</span>)"
        R"(<span class="num-val">{}</span></div>)",
        ts.cases);

    out += "</div></div></details>";
    ++rank;
  }

  out += "</section>";
  return out;
}

bool save_html(const Bench::Result& result, const std::string& file_path, std::string& error) {
  if (!ensure_parent_dir(file_path, error)) {
    return false;
  }

  const auto items = aggregate_cases(result);
  const auto recommendation_panel = build_transport_profile_cards(items);
  const auto heatmap_panel = build_score_heatmap(items);
  const size_t passed_cases = std::count_if(items.begin(), items.end(), [](const AggregatedCase& item) {
    return item.all_success() && !has_delivery_loss(item);
  });
  const size_t warning_cases = std::count_if(items.begin(), items.end(), [](const AggregatedCase& item) {
    return item.success_count > 0 && item.failure_count == 0 && (!item.all_success() || has_delivery_loss(item));
  });
  const size_t failed_cases = items.size() - passed_cases - warning_cases;
  const auto panels = build_chart_panels(items);
  const auto insight_panel = build_insight_panel(items);
  const auto failure_panel = build_failure_panel(items);
  const auto unstable_case_table = build_unstable_case_table(items);
  const auto url_summary_table = build_url_summary_table(items);
  const auto transport_score_rows = build_transport_score_rows(items);
  const auto serialization_score_rows = build_serialization_score_rows(items);
  const auto suite_score_summary_panel =
      build_suite_score_summary_panel(transport_score_rows, serialization_score_rows);
  const auto suite_score_methodology_panel = build_suite_score_methodology_panel();
  const auto transport_overview_panel = build_transport_overview_panel(items);
  const auto serialization_overview_panel = build_serialization_overview_panel(items);
  const auto transport_summary_table = build_transport_summary_table(items);
  const auto serialization_summary_table = build_serialization_summary_table(items);
  const bool has_transport_summary = transport_summary_table.find("<tr><td ") != std::string::npos;
  const bool has_serialization_summary = serialization_summary_table.find("<tr><td ") != std::string::npos;
  const size_t planned_cases = result.planned_case_count == 0 ? items.size() : result.planned_case_count;

  std::string html;
  html.reserve(16384);
  html += R"(<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"/>)";
  html += R"(<meta name="viewport" content="width=device-width,initial-scale=1"/>)";
  html += R"(<meta name="color-scheme" content="light only"/>)";
  html += R"(<meta name="generator" content="vlink-bench"/>)";
  html += R"(<title>vlink-bench report</title>)";
  format::format_to(std::back_inserter(html), "<style>{}</style>", kBaseCss);
  html +=
      "<script>function toggleBenchPanel(id){var elem=document.getElementById(id);if(!elem){return;}"
      "var shown=elem.style.display==='block';elem.style.display=shown?'none':'block';"
      "var btn=document.querySelector('[data-toggle-target=\"'+id+'\"]');"
      "if(btn){btn.textContent=shown?btn.getAttribute('data-show-label'):btn.getAttribute('data-hide-label');}}"
      "</script></head><body><div class=\"page\">"
      "<a href=\"#main\" class=\"skip-link\" data-i18n=\"skip_to_content\">Skip to content</a>";

  format::format_to(std::back_inserter(html),
                    "<header class=\"report-header\" role=\"banner\">"
                    "<div class=\"toolbar-cluster\">{}</div>"
                    "<div class=\"report-brand\">"
                    "<span class=\"wordmark\">VLink</span>"
                    "<span class=\"wordmark-sub\">bench</span>"
                    "</div>"
                    "<h1><span data-i18n=\"report_heading_run\">Benchmark</span> "
                    "<span class=\"grad\" data-i18n=\"report_heading_report\">report</span></h1>"
                    "<p class=\"headline-sub\" data-i18n=\"headline_subtitle\">"
                    "End-to-end transport, latency, throughput, and serialization benchmark across the requested "
                    "scenario matrix.</p>"
                    "<p class=\"report-meta\">"
                    "<span><span data-i18n=\"meta_version_label\">version</span> <code>{}</code></span>"
                    "<span class=\"meta-sep\" aria-hidden=\"true\">&bull;</span>"
                    "<span><span data-i18n=\"meta_created_label\">created</span> <code>{}</code></span>"
                    "</p>"
                    "</header>",
                    i18n::render_i18n_toggle_buttons(), escape_html(result.version), escape_html(result.created_at));

  {
    std::string strip =
        "<div class=\"summary-strip\" role=\"list\" data-i18n-aria=\"summary_strip_label\""
        " aria-label=\"At a glance\">";
    auto append_pill = [&strip](const char* cls, size_t value, const char* key, const char* fallback) {
      format::format_to(std::back_inserter(strip),
                        "<span class=\"summary-pill {}\" role=\"listitem\">"
                        "<span class=\"pill-num\">{}</span>"
                        "<span data-i18n=\"{}\">{}</span></span>",
                        cls, value, key, fallback);
    };
    append_pill("is-pass", passed_cases, "summary_pill_pass", "passing");
    append_pill("is-warn", warning_cases, "summary_pill_warn", "warning");
    append_pill("is-fail", failed_cases, "summary_pill_fail", "failing");

    if (result.skipped_case_count > 0) {
      append_pill("is-neutral", result.skipped_case_count, "summary_pill_skipped", "skipped");
    }

    append_pill("is-neutral", planned_cases, "summary_pill_planned", "planned");
    append_pill("is-neutral", result.scenarios.size(), "summary_pill_samples", "samples");
    strip += "</div>";
    const std::string marker = "</header>";
    auto pos = html.rfind(marker);

    if (pos != std::string::npos) {
      html.insert(pos, strip);
    }
  }

  (void)passed_cases;
  (void)warning_cases;
  (void)failed_cases;

  html += R"(<main id="main" tabindex="-1" role="main" data-i18n-aria="aria_main" aria-label="Report content">)";

  html += R"(<nav class="section-nav" data-i18n-aria="aria_sections" aria-label="Sections" role="navigation">)";
  {
    int nav_index = 0;
    auto add_nav = [&html, &nav_index](const char* href, const char* i18n_key, const char* fallback) {
      ++nav_index;
      const bool is_first = nav_index == 1;
      format::format_to(std::back_inserter(html),
                        "<a href=\"{}\"{}>"
                        "<span data-i18n=\"{}\">{}</span></a>",
                        href, is_first ? R"( aria-current="true" class="is-active")" : "", i18n_key, fallback);
    };

    if (!recommendation_panel.empty()) {
      add_nav("#decision", "nav_recommendation", "Recommended Targets");
    }

    add_nav("#overview", "nav_overview", "Run Overview");
    add_nav("#health", "nav_health", "Transport Health");

    if (!heatmap_panel.empty()) {
      add_nav("#heatmap", "nav_heatmap", "Latency / Throughput");
    }

    if (!suite_score_summary_panel.empty() || !insight_panel.empty()) {
      add_nav("#suite-score", "nav_suite_score", "Category Results");
    }

    if (!result.skip_messages.empty()) {
      add_nav("#planner-notes", "nav_planner_notes", "Run Notes");
    }

    if (!transport_overview_panel.empty()) {
      add_nav("#transport-rollup", "nav_transport_rollup", "Transport Summary");
    }

    if (!serialization_overview_panel.empty()) {
      add_nav("#serialization-overview", "nav_serialization", "Serialization");
    }

    if (!panels.empty()) {
      add_nav("#charts", "nav_charts", "Trend Charts");
    }

    if (has_transport_summary || has_serialization_summary) {
      add_nav("#scenario-tables", "nav_scenario_tables", "Detail Tables");
    }
  }

  html += "</nav>";

  if (!recommendation_panel.empty()) {
    html += recommendation_panel;
  }

  html +=
      "<section id=\"overview\" class=\"panel\" aria-labelledby=\"overview-h\">"
      "<h2 id=\"overview-h\"><span class=\"h-anchor\" aria-hidden=\"true\">01</span>"
      "<span data-i18n=\"overview_title\">Run Overview</span></h2>";
  format::format_to(std::back_inserter(html),
                    "<div class=\"hero\">"
                    "<div class=\"card\"><div class=\"label\" data-i18n=\"overview_samples\">Samples</div>"
                    "<div class=\"value\">{}</div></div>"
                    "<div class=\"card\"><div class=\"label\" data-i18n=\"overview_planned\">Planned Cases</div>"
                    "<div class=\"value\">{}</div></div>"
                    "<div class=\"card\"><div class=\"label\" data-i18n=\"overview_skipped\">Skipped Cases</div>"
                    "<div class=\"value\">{}</div></div>"
                    "<div class=\"card\"><div class=\"label\" data-i18n=\"overview_cases\">Cases</div>"
                    "<div class=\"value\">{}</div></div>"
                    "<div class=\"card is-pass\"><div class=\"label\" data-i18n=\"overview_passing\">Passing</div>"
                    "<div class=\"value ok\">{}</div></div>"
                    "<div class=\"card is-warn\"><div class=\"label\" data-i18n=\"overview_warning\">Warning</div>"
                    "<div class=\"value warn\">{}</div></div>"
                    "<div class=\"card is-fail\"><div class=\"label\" data-i18n=\"overview_failing\">Failing</div>"
                    "<div class=\"value fail\">{}</div></div>"
                    "</div>",
                    result.scenarios.size(), planned_cases, result.skipped_case_count, items.size(), passed_cases,
                    warning_cases, failed_cases);
  html +=
      "<p class=\"note\" data-i18n=\"overview_note\">"
      "Repeated runs of the same case are merged; averages only count successful runs. Delivery loss is shown "
      "once it reaches 5% so trace amounts of normal jitter do not distract from the main result."
      "</p>";

  html += R"(<h3 class="merged-h3" id="metrics-guide" data-i18n="metrics_title">Metric Guide</h3>)";
  html +=
      "<p class=\"note\" data-i18n=\"metrics_note\">"
      "Terminology used throughout the report. Skim it once and the recommendation, heatmaps, and category "
      "results below all become easier to interpret.</p>";
  html +=
      "<div class=\"table-scroll\"><table class=\"bench-table cardable\"><thead><tr>"
      "<th data-i18n=\"th_metric\">Metric</th><th data-i18n=\"th_meaning\">Meaning</th></tr></thead><tbody>";
  html +=
      "<tr><td data-i18n-label=\"th_metric\" data-label=\"Metric\" data-i18n=\"metric_p95_label\">P50 / P90 / P99 "
      "latency (and P99.9 / P99.99 in detail tables)</td><td data-i18n-label=\"th_meaning\" data-label=\"Meaning\" "
      "data-i18n=\"metric_p95_meaning\">Latency percentiles. The heatmap shows P50 (typical), P90 (slower common "
      "case), "
      "and P99 (tail) side by side; detail tables also include P99.9 and P99.99 to surface rare slow messages. "
      "Lower numbers are better across the board.</td></tr>";
  html +=
      "<tr><td data-i18n-label=\"th_metric\" data-label=\"Metric\" data-i18n=\"metric_mbps_label\">Received "
      "MB/s</td><td data-i18n-label=\"th_meaning\" data-label=\"Meaning\" data-i18n=\"metric_mbps_meaning\">"
      "How many megabytes per second the subscriber actually received. This is the main throughput number.</td></tr>";
  html +=
      "<tr><td data-i18n-label=\"th_metric\" data-label=\"Metric\" data-i18n=\"metric_loss_label\">Loss ratio</td>"
      "<td data-i18n-label=\"th_meaning\" data-label=\"Meaning\" data-i18n=\"metric_loss_meaning\">Messages that "
      "were expected but not received. Small loss can happen in stress tests; the report only highlights loss at "
      "5% or higher, and marks very high loss more strongly.</td></tr>";
  html +=
      "<tr><td data-i18n-label=\"th_metric\" data-label=\"Metric\" data-i18n=\"metric_confidence_label\">"
      "Confidence</td><td data-i18n-label=\"th_meaning\" data-label=\"Meaning\" "
      "data-i18n=\"metric_confidence_meaning\">A quick read on result reliability, based on run success, visible "
      "loss, and latency sample drops. Low confidence means review Transport Health and problem cases; final judgment "
      "still depends on latency and throughput.</td></tr>";
  html +=
      "<tr><td data-i18n-label=\"th_metric\" data-label=\"Metric\" data-i18n=\"metric_payload_label\">Message "
      "size / payload</td>"
      "<td data-i18n-label=\"th_meaning\" data-label=\"Meaning\" data-i18n=\"metric_payload_meaning\">The size "
      "and type of each message sent during the test. Larger messages usually stress bandwidth, buffers, and "
      "copying more."
      "</td></tr>";
  html +=
      "<tr><td data-i18n-label=\"th_metric\" data-label=\"Metric\" data-i18n=\"metric_topology_label\">Connection "
      "topology</td>"
      "<td data-i18n-label=\"th_meaning\" data-label=\"Meaning\" data-i18n=\"metric_topology_meaning\">The "
      "publisher/subscriber shape, such as 1:1, 1:n, n:1, or n:n. It shows whether a transport scales when more "
      "senders or receivers are involved.</td></tr>";
  html +=
      "<tr><td data-i18n-label=\"th_metric\" data-label=\"Metric\" data-i18n=\"metric_cpu_label\">CPU "
      "Efficiency</td><td data-i18n-label=\"th_meaning\" data-label=\"Meaning\" "
      "data-i18n=\"metric_cpu_meaning\">How much throughput is achieved for the CPU used by publisher and "
      "subscriber. It helps break ties, but latency and throughput remain the main decision signals.</td></tr>";
  html += "</tbody></table></div>";
  html += "</section>";

  {
    const bool has_failures = failed_cases > 0;
    const bool has_warnings = warning_cases > 0 || !unstable_case_table.empty();
    const char* open_attr = " open";

    html += R"(<section id="health" class="panel" aria-labelledby="health-h"><details)";
    html += open_attr;
    html +=
        "><summary><h2 id=\"health-h\" style=\"display:inline-flex\">"
        "<span class=\"h-anchor\" aria-hidden=\"true\">03</span>"
        "<span data-i18n=\"nav_health\">Transport Health</span></h2>";

    if (has_failures) {
      html += R"( <span class="health-badge health-attention" data-i18n="health_badge_failures">failures</span>)";
    } else if (has_warnings) {
      html += R"( <span class="health-badge health-warn" data-i18n="health_badge_warnings">warnings</span>)";
    } else {
      html += R"( <span class="health-badge health-ok" data-i18n="health_badge_ok">all OK</span>)";
    }

    html += "</summary>";
    html +=
        "<p class=\"note\" data-i18n=\"health_note\">"
        "Transport Health tells whether this report is trustworthy before you compare scores. It summarizes "
        "failed cases, warnings, URL coverage, and problem cases. Green means no obvious problem; yellow means "
        "review loss, skipped, or unstable results; red means inspect the problem-case error first. Small loss "
        "is still recorded; this section highlights loss, failures, or skips that may affect the conclusion.</p>";

    if (!failure_panel.empty()) {
      html += failure_panel;
    }

    if (!url_summary_table.empty()) {
      html += "<h3 data-i18n=\"health_url_coverage_h3\">Tested URL Coverage</h3>";
      html += url_summary_table;
    }

    if (!unstable_case_table.empty()) {
      html += "<h3 data-i18n=\"health_unstable_h3\">Cases to Review</h3>";
      html += unstable_case_table;
    }

    html += "</details></section>";
  }

  if (!heatmap_panel.empty()) {
    html += heatmap_panel;
  }

  if (!suite_score_summary_panel.empty() || !insight_panel.empty()) {
    html += R"(<section id="suite-score" class="panel" aria-labelledby="suite-score-h">)";

    if (!suite_score_summary_panel.empty()) {
      format::format_to(
          std::back_inserter(html),
          "<div class=\"panel-head\"><h2 id=\"suite-score-h\">"
          "<span class=\"h-anchor\" aria-hidden=\"true\">06</span>"
          "<span data-i18n=\"suite_score_title\">Category Results</span></h2>"
          "<button class=\"toggle-button\" type=\"button\" data-toggle-target=\"suite-score-methodology\" "
          "data-show-label=\"Show methodology\" data-hide-label=\"Hide methodology\" "
          "data-i18n-show=\"methodology_show\" data-i18n-hide=\"methodology_hide\" "
          "onclick=\"toggleBenchPanel('suite-score-methodology')\">Show methodology</button></div>{}"
          "<div id=\"suite-score-methodology\" class=\"toggle-panel\">{}</div>",
          suite_score_summary_panel, suite_score_methodology_panel);
    } else {
      html +=
          "<h2 id=\"suite-score-h\"><span class=\"h-anchor\" aria-hidden=\"true\">06</span>"
          "<span data-i18n=\"executive_title\">Result Highlights</span></h2>";
    }

    if (!insight_panel.empty()) {
      if (!suite_score_summary_panel.empty()) {
        html += R"(<h3 class="merged-h3" id="executive" data-i18n="executive_title">Result Highlights</h3>)";
      }

      html += insight_panel;
    }

    html += "</section>";
  }

  if (!result.skip_messages.empty()) {
    html +=
        "<section id=\"planner-notes\" class=\"panel\" aria-labelledby=\"planner-notes-h\">"
        "<h2 id=\"planner-notes-h\"><span class=\"h-anchor\" aria-hidden=\"true\">08</span>"
        "<span data-i18n=\"planner_notes_title\">Run Notes</span></h2><ul>";
    for (const auto& message : result.skip_messages) {
      format::format_to(std::back_inserter(html), "<li><code>{}</code></li>", escape_html(message));
    }

    html += "</ul></section>";
  }

  if (!transport_overview_panel.empty()) {
    format::format_to(std::back_inserter(html),
                      "<section id=\"transport-rollup\" class=\"panel\" aria-labelledby=\"transport-rollup-h\">"
                      "<h2 id=\"transport-rollup-h\"><span class=\"h-anchor\" aria-hidden=\"true\">09</span>"
                      "<span data-i18n=\"transport_rollup_title\">Transport Summary</span></h2>"
                      "<p class=\"note\" data-i18n=\"transport_rollup_note\">"
                      "Groups each URL/transport configuration into one row for a quick trend view. A row can "
                      "include multiple message sizes, rates, or topologies, so use latency/throughput comparisons "
                      "and trend charts for exact comparisons.</p>{}</section>",
                      transport_overview_panel);
  }

  if (!serialization_overview_panel.empty()) {
    format::format_to(std::back_inserter(html),
                      "<section id=\"serialization-overview\" class=\"panel\" "
                      "aria-labelledby=\"serialization-overview-h\">"
                      "<h2 id=\"serialization-overview-h\"><span class=\"h-anchor\" aria-hidden=\"true\">10</span>"
                      "<span data-i18n=\"serialization_overview_title\">Serialization Overview</span></h2>{}"
                      "</section>",
                      serialization_overview_panel);
  }

  if (!panels.empty()) {
    html +=
        "<section id=\"charts\" class=\"panel chart-panel\" aria-labelledby=\"charts-h\">"
        "<h2 id=\"charts-h\" style=\"display:inline-flex\">"
        "<span class=\"h-anchor\" aria-hidden=\"true\">11</span>"
        "<span data-i18n=\"nav_charts\">Trend Charts</span></h2>";

    for (const auto& panel : panels) {
      format::format_to(std::back_inserter(html),
                        "<details><summary><h3 style=\"display:inline-flex\">"
                        "<span>{}</span></h3></summary>{}</details>",
                        panel.title, panel.body);
    }

    html += "</section>";
  }

  if (has_transport_summary || has_serialization_summary) {
    if (has_transport_summary || has_serialization_summary) {
      html +=
          "<section id=\"scenario-tables\" class=\"panel chart-panel\" aria-labelledby=\"scenario-tables-h\">"
          "<h2 id=\"scenario-tables-h\" style=\"display:inline-flex\">"
          "<span class=\"h-anchor\" aria-hidden=\"true\">12</span>"
          "<span data-i18n=\"nav_scenario_tables\">Detail Tables</span></h2>";
    }

    if (has_transport_summary) {
      format::format_to(std::back_inserter(html),
                        "<details><summary>"
                        "<h3 style=\"display:inline-flex\">"
                        "<span data-i18n=\"scenario_transport_title\">Transport Detail Table</span></h3></summary>{}"
                        "</details>",
                        transport_summary_table);
    }

    if (has_serialization_summary) {
      format::format_to(
          std::back_inserter(html),
          "<details><summary>"
          "<h3 style=\"display:inline-flex\">"
          "<span data-i18n=\"scenario_serialization_title\">Serialization Detail Table</span></h3></summary>{}"
          "</details>",
          serialization_summary_table);
    }

    if (has_transport_summary || has_serialization_summary) {
      html += "</section>";
    }
  }

  html += "</main>";
  html +=
      "<footer class=\"bench-footer\" role=\"contentinfo\">"
      "<span><strong data-i18n=\"footer_generated\">Generated by</strong> "
      "<code>vlink-bench</code></span>"
      "<span><span data-i18n=\"footer_format\">Single-file offline HTML report</span></span>"
      "<span><span data-i18n=\"footer_doc\">Read the methodology in doc/10-cli-tools.md</span></span>"
      "</footer>";
  html += "</div>";
  html +=
      "<div class=\"chart-tooltip\" id=\"chart-tooltip\" role=\"status\" aria-live=\"polite\" aria-hidden=\"true\">"
      "<div class=\"tt-head\"></div><div class=\"tt-body\"></div></div>";

  html +=
      "<script>(function(){\n"
      "var charts=[];\n"
      "var tooltip=document.getElementById('chart-tooltip');\n"
      "var ttHead=tooltip?tooltip.querySelector('.tt-head'):null;\n"
      "var ttBody=tooltip?tooltip.querySelector('.tt-body'):null;\n"
      "var smallScreen=window.matchMedia('(max-width:520px)').matches;\n"
      "function fmt(v){if(!isFinite(v))return'-';var a=Math.abs(v);if(a>=100)return v.toFixed(0);"
      "if(a>=10)return v.toFixed(1);if(a>=1)return v.toFixed(2);return v.toFixed(3);}\n"
      "function fmtX(v,xIsPayload){if(xIsPayload){var u=['B','KB','MB','GB'];var i=0;var n=v;"
      "while(n>=1024&&i<u.length-1){n/=1024;i++;}return (i===0?n.toFixed(0):n.toFixed(n>=10?0:1))+' '+u[i];}"
      "return fmt(v);}\n"
      "function showTooltip(html,head,clientX,clientY){if(!tooltip)return;ttHead.innerHTML=head;"
      "ttBody.innerHTML=html;tooltip.classList.add('show');tooltip.setAttribute('aria-hidden','false');"
      "var w=tooltip.offsetWidth,h=tooltip.offsetHeight;var x=clientX,y=clientY-12;"
      "if(x-w/2<6)x=w/2+6;if(x+w/2>window.innerWidth-6)x=window.innerWidth-w/2-6;"
      "if(y-h<6)y=clientY+h+18;tooltip.style.left=x+'px';tooltip.style.top=y+'px';}\n"
      "function hideTooltip(){if(!tooltip)return;tooltip.classList.remove('show');"
      "tooltip.setAttribute('aria-hidden','true');}\n"
      "function escText(s){var d=document.createElement('div');d.textContent=s;return d.innerHTML;}\n"
      "function svgPoint(svg,clientX,clientY){var pt=svg.createSVGPoint();pt.x=clientX;pt.y=clientY;"
      "var ctm=svg.getScreenCTM();if(!ctm)return null;return pt.matrixTransform(ctm.inverse());}\n"
      "function applyPointScale(state){"
      "var zx=state.view.w>0?(state.base.w/state.view.w):1;"
      "var zy=state.view.h>0?(state.base.h/state.view.h):1;"
      "var z=Math.max(zx,zy);if(!isFinite(z)||z<=0)z=1;"
      "var pts=state.svg.querySelectorAll('.chart-point');"
      "for(var i=0;i<pts.length;i++){var p=pts[i];"
      "var base=p.classList.contains('highlight')?5.4:3.0;"
      "p.setAttribute('r',(base/z).toFixed(3));}"
      "var halos=state.svg.querySelectorAll('.chart-point-halo');"
      "for(var i=0;i<halos.length;i++){halos[i].setAttribute('r',(5.5/z).toFixed(3));}"
      "}\n"
      "function setViewBox(state){var v=state.view;state.svg.setAttribute('viewBox',"
      "v.x+' '+v.y+' '+v.w+' '+v.h);applyPointScale(state);}\n"
      "function clampView(state){var b=state.base;var v=state.view;var minW=b.w/20,maxW=b.w;"
      "if(v.w<minW){v.w=minW;}if(v.w>maxW){v.w=maxW;v.x=b.x;}"
      "if(v.h<b.h/20){v.h=b.h/20;}if(v.h>b.h){v.h=b.h;v.y=b.y;}"
      "if(v.x<b.x)v.x=b.x;if(v.y<b.y)v.y=b.y;"
      "if(v.x+v.w>b.x+b.w)v.x=b.x+b.w-v.w;if(v.y+v.h>b.y+b.h)v.y=b.y+b.h-v.h;}\n"
      "function zoomAt(state,clientX,clientY,factor){var p=svgPoint(state.svg,clientX,clientY);"
      "if(!p)return;var v=state.view;var nx=p.x-(p.x-v.x)*factor;var nw=v.w*factor;"
      "var ny=p.y-(p.y-v.y)*factor;var nh=v.h*factor;v.x=nx;v.y=ny;v.w=nw;v.h=nh;"
      "clampView(state);setViewBox(state);}\n"
      "function zoomCenter(state,factor){var r=state.svg.getBoundingClientRect();"
      "zoomAt(state,r.left+r.width/2,r.top+r.height/2,factor);}\n"
      "function reset(state){state.view={x:state.base.x,y:state.base.y,w:state.base.w,h:state.base.h};"
      "setViewBox(state);}\n"
      "function pickNearestPoint(state,svgX,svgY){var data=state.data;var p=data.plot;"
      "var isLogX=data.xScale==='log2';var isLogY=data.yScale==='log10';"
      "var axisMinX=(p.axisMinX!==undefined)?p.axisMinX:(isLogX?Math.log2(Math.max(p.minX,1)):p.minX);"
      "var axisMaxX=(p.axisMaxX!==undefined)?p.axisMaxX:(isLogX?Math.log2(Math.max(p.maxX,1)):p.maxX);"
      "var spanX=axisMaxX-axisMinX;"
      "var axisMinY=(p.axisMinY!==undefined)?p.axisMinY:(isLogY?Math.log10(Math.max(p.minY||1,1e-9)):0);"
      "var axisMaxY=(p.axisMaxY!==undefined)?p.axisMaxY:(isLogY?Math.log10(Math.max(p.maxY,1e-9)):p.maxY);"
      "var spanY=axisMaxY-axisMinY;"
      "function toAxisX(v){return isLogX?Math.log2(Math.max(v,1)):v;}"
      "function toAxisY(v){return isLogY?Math.log10(Math.max(v,1e-9)):v;}"
      "function mapY(v){if(isLogY){return spanY>0?p.top+(1-(toAxisY(v)-axisMinY)/spanY)*p.height:p.top+p.height/2;}"
      "return p.maxY>0?p.top+(1-v/p.maxY)*p.height:p.top+p.height/2;}"
      "var best=null;var bestDist=Infinity;var all=[];var i,j;"
      "for(i=0;i<data.series.length;i++){var s=data.series[i];if(state.hidden[s.id])continue;"
      "for(j=0;j<s.points.length;j++){var pt=s.points[j];"
      "var sx=p.left+(spanX>0?((toAxisX(pt.x)-axisMinX)/spanX)*p.width:p.width/2);"
      "var sy=mapY(pt.y);"
      "var d=(sx-svgX)*(sx-svgX)+(sy-svgY)*(sy-svgY);"
      "all.push({series:s,point:pt,sx:sx,sy:sy,dist:d});"
      "if(d<bestDist){bestDist=d;best={series:s,point:pt,sx:sx,sy:sy};}}}"
      "var pickThreshold=Math.max(p.width,p.height)*0.12;pickThreshold=pickThreshold*pickThreshold;"
      "if(!best||bestDist>pickThreshold)return null;"
      "var overlapThreshold=Math.max(p.width,p.height)*0.012;overlapThreshold=overlapThreshold*overlapThreshold;"
      "var matches=[];for(i=0;i<all.length;i++){var c=all[i];"
      "var dx=c.sx-best.sx;var dy=c.sy-best.sy;if(dx*dx+dy*dy<=overlapThreshold){matches.push(c);}}"
      "matches.sort(function(a,b){return a.dist-b.dist;});"
      "return{anchor:best,matches:matches};}\n"
      "function buildPointTooltip(state,pick){var data=state.data;var body='';var i;"
      "for(i=0;i<pick.matches.length;i++){var m=pick.matches[i];"
      "body+='<div class=\"tt-row\"><span class=\"tt-dot\" style=\"background:'+escText(m.series.color)+'\"></span>"
      "<span class=\"tt-label\">'+escText(m.series.id)+' '+escText(m.series.label)+'</span>"
      "<span class=\"tt-value\">'+escText(fmt(m.point.y))+'</span></div>';}"
      "var head=escText(data.xLabel)+': '+escText(fmtX(pick.anchor.point.x,data.xIsPayload));"
      "return{head:head,body:body};}\n"
      "function moveCrosshair(state,pick){var line=state.crosshair.querySelector('.chart-crosshair-line');"
      "if(line){line.setAttribute('x1',pick.anchor.sx);line.setAttribute('x2',pick.anchor.sx);}"
      "state.crosshair.style.display='';}\n"
      "function clearHighlight(state){state.crosshair.style.display='none';"
      "state.svg.querySelectorAll('.chart-point.highlight').forEach(function(c){c.classList.remove('highlight');});}\n"
      "function highlightPoint(state,pick){var matches=pick.matches;"
      "state.svg.querySelectorAll('.chart-point').forEach(function(c){"
      "var sid=c.getAttribute('data-series-id');"
      "var cx=parseFloat(c.getAttribute('cx'));var cy=parseFloat(c.getAttribute('cy'));"
      "var hit=false;for(var i=0;i<matches.length;i++){var m=matches[i];"
      "if(m.series.id===sid&&Math.abs(cx-m.sx)<0.6&&Math.abs(cy-m.sy)<0.6){hit=true;break;}}"
      "if(hit){c.classList.add('highlight');}else{c.classList.remove('highlight');}});"
      "applyPointScale(state);}\n"
      "function initChart(svg,dataNode){var data;try{data=JSON.parse(dataNode.textContent);}catch(e){return;}\n"
      "var vb=svg.getAttribute('viewBox').split(/\\s+/).map(parseFloat);"
      "var state={svg:svg,data:data,base:{x:vb[0],y:vb[1],w:vb[2],h:vb[3]},"
      "view:{x:vb[0],y:vb[1],w:vb[2],h:vb[3]},hidden:{},pointers:new Map(),"
      "panStart:null,crosshair:svg.querySelector('.chart-crosshair'),"
      "pinchStart:null};\n"
      "charts.push(state);\n"
      "if(!state.crosshair){state.crosshair=document.createElementNS('http://www.w3.org/2000/svg','g');}\n"
      "svg.addEventListener('wheel',function(ev){ev.preventDefault();"
      "var f=ev.deltaY<0?0.85:1.18;zoomAt(state,ev.clientX,ev.clientY,f);},{passive:false});\n"
      "svg.addEventListener('dblclick',function(){reset(state);});\n"
      "svg.addEventListener('pointerdown',function(ev){svg.setPointerCapture(ev.pointerId);"
      "state.pointers.set(ev.pointerId,{x:ev.clientX,y:ev.clientY});"
      "if(state.pointers.size===1){var p=svgPoint(svg,ev.clientX,ev.clientY);"
      "state.panStart={svgX:p?p.x:0,svgY:p?p.y:0,viewX:state.view.x,viewY:state.view.y};"
      "svg.classList.add('is-panning');}"
      "else if(state.pointers.size===2){var pts=Array.from(state.pointers.values());"
      "state.pinchStart={dist:Math.hypot(pts[0].x-pts[1].x,pts[0].y-pts[1].y),"
      "view:{x:state.view.x,y:state.view.y,w:state.view.w,h:state.view.h}};}});\n"
      "svg.addEventListener('pointermove',function(ev){if(state.pointers.has(ev.pointerId)){"
      "state.pointers.set(ev.pointerId,{x:ev.clientX,y:ev.clientY});}\n"
      "if(state.pointers.size===2&&state.pinchStart){var pts=Array.from(state.pointers.values());"
      "var d=Math.hypot(pts[0].x-pts[1].x,pts[0].y-pts[1].y);"
      "if(state.pinchStart.dist>0){var f=state.pinchStart.dist/d;var midX=(pts[0].x+pts[1].x)/2;"
      "var midY=(pts[0].y+pts[1].y)/2;state.view={x:state.pinchStart.view.x,y:state.pinchStart.view.y,"
      "w:state.pinchStart.view.w,h:state.pinchStart.view.h};setViewBox(state);"
      "zoomAt(state,midX,midY,f);}return;}\n"
      "if(state.pointers.size===1&&state.panStart){var p=svgPoint(svg,ev.clientX,ev.clientY);"
      "if(p){var dx=p.x-state.panStart.svgX;var dy=p.y-state.panStart.svgY;"
      "state.view.x=state.panStart.viewX-dx;state.view.y=state.panStart.viewY-dy;"
      "clampView(state);setViewBox(state);}return;}\n"
      "if(smallScreen){return;}\n"
      "var sp=svgPoint(svg,ev.clientX,ev.clientY);if(!sp)return;"
      "if(sp.x<state.data.plot.left||sp.x>state.data.plot.left+state.data.plot.width){"
      "clearHighlight(state);hideTooltip();return;}"
      "var pick=pickNearestPoint(state,sp.x,sp.y);"
      "if(!pick){clearHighlight(state);hideTooltip();return;}"
      "moveCrosshair(state,pick);highlightPoint(state,pick);"
      "var t=buildPointTooltip(state,pick);"
      "showTooltip(t.body,t.head,ev.clientX,ev.clientY);});\n"
      "function endPointer(ev){state.pointers.delete(ev.pointerId);"
      "if(state.pointers.size<2)state.pinchStart=null;"
      "if(state.pointers.size===0){state.panStart=null;svg.classList.remove('is-panning');}}\n"
      "svg.addEventListener('pointerup',endPointer);"
      "svg.addEventListener('pointercancel',endPointer);"
      "svg.addEventListener('pointerleave',function(ev){endPointer(ev);clearHighlight(state);hideTooltip();});\n"
      "var card=svg.closest('.chart-card');if(card){"
      "var btns=card.querySelectorAll('[data-chart-action]');btns.forEach(function(b){"
      "b.addEventListener('click',function(){var a=b.getAttribute('data-chart-action');"
      "if(a==='zoom-in'){zoomCenter(state,0.7);}else if(a==='zoom-out'){zoomCenter(state,1.4);}"
      "else if(a==='reset'){reset(state);}});});"
      "var legends=card.querySelectorAll('.legend-entry');legends.forEach(function(l){"
      "l.addEventListener('click',function(){var sid=l.getAttribute('data-legend-target');"
      "var hidden=l.getAttribute('aria-pressed')==='false'?false:true;"
      "l.setAttribute('aria-pressed',hidden?'false':'true');state.hidden[sid]=hidden;"
      "card.querySelectorAll('[data-series-id=\"'+sid+'\"]').forEach(function(el){"
      "if(hidden){el.classList.add('hidden');}else{el.classList.remove('hidden');}});});});}}\n"
      "document.querySelectorAll('script.chart-data').forEach(function(node){"
      "var id=node.getAttribute('data-chart-id');var svg=document.getElementById('chart-'+id);"
      "if(svg){initChart(svg,node);}});\n"
      "document.addEventListener('click',function(ev){if(ev.target.closest('.bench-chart'))return;"
      "if(ev.target.closest('.chart-tooltip'))return;hideTooltip();"
      "charts.forEach(function(s){clearHighlight(s);});});\n"
      "})();</script>";

  html +=
      "<script>(function(){\n"
      "var nav=document.querySelector('.section-nav');"
      "if(!nav)return;"
      "var links=Array.prototype.slice.call(nav.querySelectorAll('a[href^=\"#\"]'));"
      "if(!links.length)return;"
      "var sections=links.map(function(a){var id=a.getAttribute('href').slice(1);"
      "return{link:a,el:document.getElementById(id)};}).filter(function(x){return!!x.el;});"
      "function setActive(el){links.forEach(function(l){"
      "var on=l===el;l.classList.toggle('is-active',on);"
      "if(on){l.setAttribute('aria-current','true');}else{l.removeAttribute('aria-current');}"
      "});}\n"
      "if('IntersectionObserver' in window){"
      "var visible=new Map();"
      "var io=new IntersectionObserver(function(entries){"
      "entries.forEach(function(e){if(e.isIntersecting){visible.set(e.target,e.intersectionRatio);}"
      "else{visible.delete(e.target);}});"
      "var top=null;var topRatio=-1;"
      "visible.forEach(function(r,el){if(r>topRatio){topRatio=r;top=el;}});"
      "if(top){var s=sections.find(function(x){return x.el===top;});if(s)setActive(s.link);}"
      "},{rootMargin:'-30% 0px -55% 0px',threshold:[0,0.25,0.5,0.75,1]});"
      "sections.forEach(function(x){io.observe(x.el);});"
      "}\n"
      "links.forEach(function(a){a.addEventListener('click',function(){"
      "setTimeout(function(){setActive(a);},50);"
      "var id=a.getAttribute('href').slice(1);"
      "var el=document.getElementById(id);if(el){"
      "var d=el.querySelector('details');if(d&&!d.open){d.open=true;}}"
      "});});\n"
      "})();</script>";

  html += i18n::render_i18n_runtime_block();
  html += "</body></html>";

  return write_text_file_atomic(file_path, html, error);
}

}  // namespace vlink::bench::report
