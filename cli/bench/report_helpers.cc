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

#include "./report_helpers.h"

#include <vlink/base/helpers.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "./bench.h"
#include "./report_score.h"

namespace vlink::bench::report {

static constexpr double kReportLossDisplayThresholdPercent = 5.0;

void decode_utf8(std::string_view text, size_t index, uint32_t& code_point, size_t& bytes) {
  const auto lead = static_cast<unsigned char>(text[index]);

  if (lead < 0x80) {
    code_point = lead;
    bytes = 1;
    return;
  }

  if ((lead & 0xE0) == 0xC0) {
    if (index + 1 < text.size() && (static_cast<unsigned char>(text[index + 1]) & 0xC0) == 0x80) {
      code_point = (static_cast<uint32_t>(lead & 0x1F) << 6) | (static_cast<unsigned char>(text[index + 1]) & 0x3F);
      bytes = 2;
      return;
    }
  } else if ((lead & 0xF0) == 0xE0) {
    if (index + 2 < text.size() && (static_cast<unsigned char>(text[index + 1]) & 0xC0) == 0x80 &&
        (static_cast<unsigned char>(text[index + 2]) & 0xC0) == 0x80) {
      code_point = (static_cast<uint32_t>(lead & 0x0F) << 12) |
                   (static_cast<uint32_t>(static_cast<unsigned char>(text[index + 1]) & 0x3F) << 6) |
                   (static_cast<unsigned char>(text[index + 2]) & 0x3F);
      bytes = 3;
      return;
    }
  } else if ((lead & 0xF8) == 0xF0) {
    if (index + 3 < text.size() && (static_cast<unsigned char>(text[index + 1]) & 0xC0) == 0x80 &&
        (static_cast<unsigned char>(text[index + 2]) & 0xC0) == 0x80 &&
        (static_cast<unsigned char>(text[index + 3]) & 0xC0) == 0x80) {
      code_point = (static_cast<uint32_t>(lead & 0x07) << 18) |
                   (static_cast<uint32_t>(static_cast<unsigned char>(text[index + 1]) & 0x3F) << 12) |
                   (static_cast<uint32_t>(static_cast<unsigned char>(text[index + 2]) & 0x3F) << 6) |
                   (static_cast<unsigned char>(text[index + 3]) & 0x3F);
      bytes = 4;
      return;
    }
  }

  code_point = lead;
  bytes = 1;
}

int codepoint_width(uint32_t cp) {
  if (cp < 0x20 || cp == 0x7F) {
    return 0;
  }

  if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x200B && cp <= 0x200D) || cp == 0xFEFF) {
    return 0;
  }

  if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E) || (cp >= 0x3041 && cp <= 0x33FF) ||
      (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) ||
      (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) ||
      (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x20000 && cp <= 0x2FFFD) ||
      (cp >= 0x30000 && cp <= 0x3FFFD)) {
    return 2;
  }

  return 1;
}

bool ensure_parent_dir(const std::string& file_path, std::string& error) {
  auto parent = std::filesystem::path(file_path).parent_path();

  if (parent.empty()) {
    return true;
  }

  std::error_code ec;
  std::filesystem::create_directories(parent, ec);

  if (ec) {
    error = ec.message();
    return false;
  }

  return true;
}

bool write_text_file_atomic(const std::string& file_path, std::string_view content, std::string& error) {
  const std::filesystem::path target_path(file_path);
  std::filesystem::path tmp_path = target_path;
  tmp_path += ".tmp";

  {
    std::ofstream stream(tmp_path, std::ios::out | std::ios::trunc | std::ios::binary);

    if (!stream.is_open()) {
      error.assign("open output file failed: ").append(tmp_path.string());
      return false;
    }

    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    stream.flush();

    if (!stream.good()) {
      std::error_code ec;
      std::filesystem::remove(tmp_path, ec);
      error.assign("write output file failed: ").append(tmp_path.string());
      return false;
    }
  }

  std::error_code ec;
  std::filesystem::rename(tmp_path, target_path, ec);

  if (ec) {
    std::error_code remove_ec;
    std::filesystem::remove(tmp_path, remove_ec);
    error.assign("rename output file failed: ").append(ec.message());
    return false;
  }

  return true;
}

std::string get_transport_from_url(const std::string& url) {
  auto pos = url.find("://");

  if (pos == std::string::npos) {
    return "unknown";
  }

  return url.substr(0, pos);
}

std::string format_size_label(size_t value) {
  std::ostringstream stream;

  if (value >= 1024ULL * 1024ULL) {
    stream << std::fixed << std::setprecision(value % (1024ULL * 1024ULL) == 0 ? 0 : 1)
           << static_cast<double>(value) / (1024.0 * 1024.0) << " MB";
  } else if (value >= 1024ULL) {
    stream << std::fixed << std::setprecision(value % 1024ULL == 0 ? 0 : 1) << static_cast<double>(value) / 1024.0
           << " KB";
  } else {
    stream << value << " B";
  }

  return stream.str();
}

std::string format_decimal(double value, int precision) { return Helpers::double_to_string(value, precision); }

std::string format_metric_cell(const MetricSummary& metric, int precision) {
  if (metric.count == 0) {
    return "-";
  }

  const double value = metric.average();

  if (!std::isfinite(value)) {
    return "-";
  }

  return format_decimal(value, precision);
}

std::string format_memory_cell(const MetricSummary& metric) {
  if (metric.count == 0) {
    return "-";
  }

  const double memory_mb = metric.average();

  if (!std::isfinite(memory_mb)) {
    return "-";
  }

  return format_decimal(memory_mb, memory_mb >= 100.0 ? 0 : 1);
}

double compute_loss_ratio_percent(const AggregatedCase& item) {
  if (item.worst_run_loss_pct >= 0.0) {
    return item.worst_run_loss_pct;
  }

  if (item.expected.sum <= 0.0) {
    return 0.0;
  }

  return item.lost.sum * 100.0 / item.expected.sum;
}

bool should_report_delivery_loss(double loss_percent) {
  return std::isfinite(loss_percent) && loss_percent >= kReportLossDisplayThresholdPercent;
}

bool has_delivery_loss(const AggregatedCase& item) {
  return should_report_delivery_loss(compute_loss_ratio_percent(item));
}

bool has_runtime_success(const AggregatedCase& item) { return item.success_count > 0 && item.failure_count == 0; }

double compute_latency_drop_ratio_percent(const AggregatedCase& item) {
  if (item.worst_run_latency_drop_pct >= 0.0) {
    return item.worst_run_latency_drop_pct;
  }

  const auto dropped = static_cast<double>(item.latency_samples_dropped);
  const double delivered = item.received.sum;

  if (dropped <= 0.0) {
    return 0.0;
  }

  if (delivered <= 0.0) {
    return 100.0;
  }

  return dropped * 100.0 / delivered;
}

std::string format_loss_ratio_cell(const AggregatedCase& item, int precision) {
  if (item.expected.sum <= 0.0) {
    return "-";
  }

  return format_decimal(compute_loss_ratio_percent(item), precision);
}

std::string format_loss_detail(const AggregatedCase& item) {
  if (item.lost.count == 0 || item.expected.sum <= 0.0) {
    return format_metric_cell(item.lost, 0);
  }

  return format_metric_cell(item.lost, 0) + " / " + format_metric_cell(item.expected, 0) + " (" +
         format_loss_ratio_cell(item) + "%)";
}

std::string join_strings(const std::vector<std::string>& values, std::string_view separator) {
  if (values.empty()) {
    return std::string();
  }

  std::ostringstream stream;

  for (size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      stream << separator;
    }

    stream << values.at(index);
  }

  return stream.str();
}

std::string format_property_list(const std::vector<std::string>& values) {
  if (values.empty()) {
    return std::string();
  }

  std::ostringstream stream;
  stream << '[';

  for (size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      stream << ',';
    }

    stream << std::quoted(values[index]);
  }

  stream << ']';
  return stream.str();
}

std::string quote_csv(const std::string& input) {
  bool needs_quotes = false;
  std::string escaped;
  escaped.reserve(input.size());

  for (char c : input) {
    if (c == '"' || c == ',' || c == '\n' || c == '\r') {
      needs_quotes = true;
    }

    if (c == '"') {
      escaped += "\"\"";
    } else {
      escaped.push_back(c);
    }
  }

  if (!needs_quotes) {
    return escaped;
  }

  return '"' + escaped + '"';
}

std::string escape_html(const std::string& input) {
  std::string output;
  output.reserve(input.size());

  for (char c : input) {
    switch (c) {
      case '&':
        output += "&amp;";
        break;
      case '<':
        output += "&lt;";
        break;
      case '>':
        output += "&gt;";
        break;
      case '"':
        output += "&quot;";
        break;
      case '\'':
        output += "&#39;";
        break;
      default:
        output.push_back(c);
        break;
    }
  }

  return output;
}

int display_width(std::string_view text) {
  int width = 0;

  for (size_t index = 0; index < text.size();) {
    uint32_t cp = 0;
    size_t bytes = 0;
    decode_utf8(text, index, cp, bytes);
    width += codepoint_width(cp);
    index += bytes;
  }

  return width;
}

std::string ascii_tolower(std::string text) {
  for (auto& ch : text) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }

  return text;
}

std::string strip_ansi_escape_codes(const std::string& input) {
  std::string output;
  output.reserve(input.size());

  for (size_t index = 0; index < input.size();) {
    if (input[index] == '\033' && index + 1 < input.size() && input[index + 1] == '[') {
      index += 2;

      while (index < input.size()) {
        const char c = input[index++];

        if (c >= '@' && c <= '~') {
          break;
        }
      }

      continue;
    }

    char c = input[index++];

    if (c == '\r' || c == '\n' || c == '\t') {
      c = ' ';
    }

    if (static_cast<unsigned char>(c) < 0x20 && c != ' ') {
      continue;
    }

    output.push_back(c);
  }

  std::string normalized;
  normalized.reserve(output.size());
  bool prev_space = false;

  for (char c : output) {
    if (c == ' ') {
      if (!prev_space) {
        normalized.push_back(c);
      }

      prev_space = true;
    } else {
      normalized.push_back(c);
      prev_space = false;
    }
  }

  while (!normalized.empty() && normalized.front() == ' ') {
    normalized.erase(normalized.begin());
  }

  while (!normalized.empty() && normalized.back() == ' ') {
    normalized.pop_back();
  }

  return normalized;
}

double score_latency_us(const AggregatedCase& item) {
  if (item.scenario.suite == Bench::kLatencySuite) {
    const auto& cfg = score_config();
    double weighted_sum = 0.0;
    double weight_sum = 0.0;

    auto add_latency = [&weighted_sum, &weight_sum](const MetricSummary& metric, double weight) {
      if (metric.count == 0) {
        return;
      }

      weighted_sum += metric.average() * weight;
      weight_sum += weight;
    };

    add_latency(item.p95_latency_us, cfg.latency_p95_weight);
    add_latency(item.p99_latency_us, cfg.latency_p99_weight);
    add_latency(item.p999_latency_us, cfg.latency_p999_weight);
    add_latency(item.p9999_latency_us, cfg.latency_p9999_weight);

    if (weight_sum > 0.0) {
      return weighted_sum / weight_sum;
    }
  }

  return item.avg_latency_us.average();
}

double score_latency_quality(const AggregatedCase& item) {
  const auto& cfg = score_config();
  double weighted_sum = 0.0;
  double weight_sum = 0.0;

  auto add_latency_score = [&weighted_sum, &weight_sum, &item](const MetricSummary& metric, double weight) {
    if (metric.count == 0) {
      return;
    }

    weighted_sum += compute_absolute_latency_score(metric.average(), item.scenario.payload_size) * weight;
    weight_sum += weight;
  };

  add_latency_score(item.p95_latency_us, cfg.latency_p95_weight);
  add_latency_score(item.p99_latency_us, cfg.latency_p99_weight);
  add_latency_score(item.p999_latency_us, cfg.latency_p999_weight);
  add_latency_score(item.p9999_latency_us, cfg.latency_p9999_weight);

  if (weight_sum <= 0.0) {
    return compute_absolute_latency_score(score_latency_us(item), item.scenario.payload_size);
  }

  return std::clamp(weighted_sum / weight_sum, 0.0, 100.0);
}

std::string format_rate_label(const Bench::Scenario& scenario) {
  std::string label;

  if (scenario.rate_pattern == Bench::kMaxRatePattern) {
    label = "max";
  } else if (scenario.rate_pattern == Bench::kBurstRatePattern) {
    label = std::to_string(scenario.burst_messages) + "x@" + std::to_string(scenario.rate_hz) + "Hz";
  } else if (scenario.rate_hz <= 0) {
    label = "fixed";
  } else {
    label = std::to_string(scenario.rate_hz) + "Hz";
  }

  if (scenario.subscriber_sleep_us > 0) {
    label += " sub-sleep=" + std::to_string(scenario.subscriber_sleep_us) + "us";
  }

  return label;
}

std::string make_topology_label(const Bench::Scenario& scenario) {
  std::ostringstream stream;
  stream << Bench::topology_to_string(scenario.topology) << " [" << scenario.publishers << 'p' << '/'
         << scenario.subscribers << 's' << ']';
  return stream.str();
}

bool is_serialization_case(const AggregatedCase& item) { return item.scenario.suite == Bench::kSerializationSuite; }

std::string make_case_status_text(const AggregatedCase& item) {
  std::ostringstream stream;

  if (item.all_success() && !has_delivery_loss(item)) {
    stream << "OK (" << item.success_count << '/' << item.sample_count << ')';
  } else if (item.failure_count == 0 && item.success_count > 0) {
    stream << "WARN (" << item.success_count << '/' << item.sample_count << ')';
  } else if (item.success_count == 0) {
    stream << "FAIL (0/" << item.sample_count << ')';
  } else {
    stream << "PART (" << item.success_count << '/' << item.sample_count << ')';
  }

  if (item.latency_samples_dropped != 0) {
    stream << " | latency drop " << item.latency_samples_dropped;
  }

  if (has_delivery_loss(item)) {
    stream << " | loss " << format_loss_ratio_cell(item, 3) << '%';
  }

  if (!item.errors.empty()) {
    stream << ": " << join_strings(item.errors, " | ");
  }

  return stream.str();
}

std::string make_case_status_brief(const AggregatedCase& item) {
  if (item.all_success() && !has_delivery_loss(item)) {
    return "OK " + std::to_string(item.success_count) + "/" + std::to_string(item.sample_count);
  }

  if (item.failure_count == 0 && item.success_count > 0) {
    std::string status = "WARN " + std::to_string(item.success_count) + "/" + std::to_string(item.sample_count);

    if (item.latency_samples_dropped != 0) {
      status += " d" + std::to_string(item.latency_samples_dropped);
    }

    if (has_delivery_loss(item)) {
      status += " loss";
    }

    return status;
  }

  if (item.success_count == 0) {
    return "FAIL 0/" + std::to_string(item.sample_count);
  }

  return "PART " + std::to_string(item.success_count) + "/" + std::to_string(item.sample_count);
}

}  // namespace vlink::bench::report
