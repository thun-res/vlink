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

#include "./bench.h"

#include <vlink/base/elapsed_timer.h>
#include <vlink/base/format.h>
#include <vlink/base/process.h>
#include <vlink/base/utils.h>
#include <vlink/extension/qos_profile.h>
#include <vlink/extension/terminal_stream.h>
#include <vlink/serializer.h>
#include <vlink/version.h>
#include <vlink/vlink.h>
#include <vlink/zerocopy/raw_data.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <numeric>
#include <sstream>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#include <psapi.h>
#undef min
#undef max
#undef GetMessage
#else
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#elif defined(_POSIX_VERSION)
#include <sys/resource.h>
#endif
#endif

namespace vlink::bench {

using json = nlohmann::json;

constexpr size_t kMaxLatencySamples{1000000};
constexpr int kDefaultProcessReadyTimeoutMs{30000};
constexpr int kDefaultProcessStartTimeoutMs{15000};
constexpr int kDefaultProcessMeasureBufferMs{10000};
constexpr int kDefaultProcessCleanupTimeoutMs{3000};

int get_env_int_or_default(const char* name, int default_value) noexcept {
  const auto raw = Utils::get_env(name);

  if VLIKELY (raw.empty()) {
    return default_value;
  }

  try {
    size_t pos = 0;
    const auto parsed = std::stoi(raw, &pos);

    if VUNLIKELY (pos != raw.size() || parsed < 0) {
      return default_value;
    }

    return parsed;
  } catch (std::exception&) {
    return default_value;
  }
}

const int kProcessReadyTimeoutMs =
    get_env_int_or_default("VLINK_BENCH_READY_TIMEOUT_MS", kDefaultProcessReadyTimeoutMs);
const int kProcessStartTimeoutMs =
    get_env_int_or_default("VLINK_BENCH_START_TIMEOUT_MS", kDefaultProcessStartTimeoutMs);
const int kProcessMeasureBufferMs =
    get_env_int_or_default("VLINK_BENCH_MEASURE_BUFFER_MS", kDefaultProcessMeasureBufferMs);
const int kProcessGraceTimeoutMs =
    get_env_int_or_default("VLINK_BENCH_CLEANUP_TIMEOUT_MS", kDefaultProcessCleanupTimeoutMs);
std::atomic_bool stop_requested_flag{false};

std::string format_size_label(size_t value);
std::string format_decimal(double value, int precision);

inline void run_decode_utf8(std::string_view text, size_t index, uint32_t& code_point, size_t& bytes) {
  const auto lead = static_cast<unsigned char>(text[index]);

  if (lead < 0x80) {
    code_point = lead;
    bytes = 1;
    return;
  }

  if ((lead & 0xE0) == 0xC0 && index + 1 < text.size() &&
      (static_cast<unsigned char>(text[index + 1]) & 0xC0) == 0x80) {
    code_point = (static_cast<uint32_t>(lead & 0x1F) << 6) | (static_cast<unsigned char>(text[index + 1]) & 0x3F);
    bytes = 2;
    return;
  }

  if ((lead & 0xF0) == 0xE0 && index + 2 < text.size() &&
      (static_cast<unsigned char>(text[index + 1]) & 0xC0) == 0x80 &&
      (static_cast<unsigned char>(text[index + 2]) & 0xC0) == 0x80) {
    code_point = (static_cast<uint32_t>(lead & 0x0F) << 12) |
                 (static_cast<uint32_t>(static_cast<unsigned char>(text[index + 1]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(text[index + 2]) & 0x3F);
    bytes = 3;
    return;
  }

  if ((lead & 0xF8) == 0xF0 && index + 3 < text.size() &&
      (static_cast<unsigned char>(text[index + 1]) & 0xC0) == 0x80 &&
      (static_cast<unsigned char>(text[index + 2]) & 0xC0) == 0x80 &&
      (static_cast<unsigned char>(text[index + 3]) & 0xC0) == 0x80) {
    code_point = (static_cast<uint32_t>(lead & 0x07) << 18) |
                 (static_cast<uint32_t>(static_cast<unsigned char>(text[index + 1]) & 0x3F) << 12) |
                 (static_cast<uint32_t>(static_cast<unsigned char>(text[index + 2]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(text[index + 3]) & 0x3F);
    bytes = 4;
    return;
  }

  code_point = lead;
  bytes = 1;
}

inline int run_codepoint_width(uint32_t cp) {
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

inline int run_display_width(std::string_view text) {
  int width = 0;

  for (size_t index = 0; index < text.size();) {
    uint32_t cp = 0;
    size_t bytes = 0;
    run_decode_utf8(text, index, cp, bytes);
    width += run_codepoint_width(cp);
    index += bytes;
  }

  return width;
}

int get_run_output_width() {
  const auto terminal_size = Utils::get_terminal_size();

  if (terminal_size.first > 0) {
    return terminal_size.first;
  }

  return 120;
}

bool use_run_output_color() {
  const auto terminal_size = Utils::get_terminal_size();
  return terminal_size.first > 0 && terminal_size.second > 0;
}

std::string run_truncate_to_width(std::string_view text, int max_width) {
  if (max_width <= 0) {
    return std::string();
  }

  if (run_display_width(text) <= max_width) {
    return std::string(text);
  }

  const int budget = std::max(0, max_width - 1);
  std::string out;
  int width = 0;

  for (size_t index = 0; index < text.size();) {
    uint32_t cp = 0;
    size_t bytes = 0;
    run_decode_utf8(text, index, cp, bytes);
    const int cw = run_codepoint_width(cp);

    if (width + cw > budget) {
      break;
    }

    out.append(text.data() + index, bytes);
    width += cw;
    index += bytes;
  }

  out += "\xE2\x80\xA6";
  return out;
}

std::string fit_run_text(std::string_view text, int width) {
  if (width <= 0) {
    return std::string();
  }

  const int actual = run_display_width(text);

  if (actual <= width) {
    return std::string(text) + std::string(static_cast<size_t>(width - actual), ' ');
  }

  return run_truncate_to_width(text, width);
}

std::string fit_run_text_no_pad(std::string_view text, int width) {
  if (width <= 0) {
    return std::string();
  }

  const int actual = run_display_width(text);

  if (actual <= width) {
    return std::string(text);
  }

  return run_truncate_to_width(text, width);
}

std::string fit_cell_left(std::string_view text, int width) { return fit_run_text(text, width); }

std::string fit_cell_right(std::string_view text, int width) {
  if (width <= 0) {
    return std::string();
  }

  const int actual = run_display_width(text);

  if (actual >= width) {
    return run_truncate_to_width(text, width);
  }

  return std::string(static_cast<size_t>(width - actual), ' ') + std::string(text);
}

constexpr const char* kRunAnsiReset = "\033[0m";
constexpr const char* kRunStyleBanner = "\033[48;5;236;38;5;255;1m";
constexpr const char* kRunStyleRule = "\033[38;5;240m";
constexpr const char* kRunStyleTitle = "\033[1;38;5;252m";
constexpr const char* kRunStyleDim = "\033[2;38;5;245m";
constexpr const char* kRunStylePhaseWarmup = "\033[38;5;245m";
constexpr const char* kRunStylePhaseMeasure = "\033[38;5;78m";
constexpr const char* kRunStylePhaseDrain = "\033[38;5;179m";
constexpr const char* kRunStylePhaseWrap = "\033[2;38;5;245m";
constexpr const char* kRunStyleBarEmpty = "\033[38;5;238m";
constexpr const char* kRunStyleOk = "\033[38;5;78m";
constexpr const char* kRunStyleFail = "\033[38;5;167m";

void print_run_line(std::string_view text, const char* color = nullptr) {
  const bool is_tty = use_run_output_color();
  const std::string line = is_tty ? fit_run_text(text, get_run_output_width()) : std::string(text);

  std::string out;
  out.reserve(line.size() + 32);

  if (is_tty && color != nullptr) {
    out.append(color);
    out.append(line);
    out.append(kRunAnsiReset);
  } else {
    out.append(line);
  }

  out.push_back('\n');
  VLINK_TERM_OUT.write_raw(out.data(), out.size());
  VLINK_TERM_OUT.flush();
}

constexpr int kRunColSuite = 10;
constexpr int kRunColMode = 7;
constexpr int kRunColTopo = 4;
constexpr int kRunColPayload = 8;
constexpr int kRunColSize = 7;

std::string_view short_suite_label(Bench::Suite suite) noexcept {
  switch (suite) {
    case Bench::kThroughputSuite:
      return "throughput";
    case Bench::kLatencySuite:
      return "latency";
    case Bench::kTopologySuite:
      return "topology";
    case Bench::kSerializationSuite:
      return "serialize";
    case Bench::kBackpressureSuite:
      return "backpressure";
    default:
      return "unknown";
  }
}

std::string_view short_mode_label(Bench::Mode mode) noexcept {
  switch (mode) {
    case Bench::kLocalDirectMode:
      return "direct";
    case Bench::kLocalLoopMode:
      return "loop";
    case Bench::kProcessMode:
      return "process";
    default:
      return "unknown";
  }
}

constexpr const char* kRunColSep = " \xE2\x94\x82 ";
constexpr const char* kRunIconPending = "\xE2\x96\xB8 ";
constexpr const char* kRunIconPass = "\xE2\x97\x8F ";
constexpr const char* kRunIconFail = "\xE2\x97\x8B ";

std::string make_run_index_tag(size_t index, size_t total) {
  const auto total_digits = std::to_string(total).size();
  std::string inner;
  inner.reserve(total_digits * 2 + 3);
  const auto idx_str = std::to_string(index);
  inner.append(total_digits - std::min(total_digits, idx_str.size()), ' ');
  inner.append(idx_str);
  inner.push_back('/');
  inner.append(std::to_string(total));
  return "[ " + inner + " ] ";
}

std::string make_run_case_title(size_t index, size_t total, const Bench::Scenario& scenario) {
  const auto tag = make_run_index_tag(index, total);
  const int index_col_w = run_display_width(tag);
  const int icon_w = 2;
  const int fixed_cells = kRunColSuite + kRunColMode + kRunColTopo + kRunColPayload + kRunColSize;
  const int sep_w = run_display_width(kRunColSep);
  const int fixed_total = index_col_w + icon_w + fixed_cells + sep_w * 5;
  const int terminal_w = get_run_output_width();
  const int url_budget = std::max(terminal_w - fixed_total - 1, 16);

  std::string url_cell = scenario.url;

  std::string line;
  line.reserve(256);
  line.append(tag);
  line.append(kRunIconPending);
  line.append(fit_cell_left(short_suite_label(scenario.suite), kRunColSuite));
  line.append(kRunColSep);
  line.append(fit_cell_left(short_mode_label(scenario.mode), kRunColMode));
  line.append(kRunColSep);
  line.append(fit_cell_left(Bench::topology_to_string(scenario.topology), kRunColTopo));
  line.append(kRunColSep);
  line.append(fit_cell_left(Bench::payload_to_string(scenario.payload), kRunColPayload));
  line.append(kRunColSep);
  line.append(fit_cell_right(format_size_label(scenario.payload_size), kRunColSize));
  line.append(kRunColSep);
  line.append(fit_run_text_no_pad(url_cell, url_budget));
  return line;
}

constexpr int kRunSetupEstimateMs = 250;
constexpr int kRunTeardownEstimateMs = 150;
constexpr int kRunCooldownMs = 200;

std::string format_duration_short(int64_t ms) {
  if (ms < 0) {
    ms = 0;
  }

  const int hours = static_cast<int>(ms / (1000LL * 60 * 60));
  const int mins = static_cast<int>((ms % (1000LL * 60 * 60)) / (1000 * 60));
  const int secs = static_cast<int>((ms % (1000 * 60)) / 1000);
  const int millis = static_cast<int>(ms % 1000);
  char buf[32];

  if (hours > 0) {
    std::snprintf(buf, sizeof(buf), "%dh%02dm%02ds", hours, mins, secs);
  } else if (mins > 0) {
    std::snprintf(buf, sizeof(buf), "%dm%02ds", mins, secs);
  } else if (secs > 0) {
    std::snprintf(buf, sizeof(buf), "%ds", secs);
  } else {
    std::snprintf(buf, sizeof(buf), "%dms", millis);
  }

  return buf;
}

int64_t plan_case_estimate_ms(const Bench::Scenario& scenario) {
  int64_t total = kRunSetupEstimateMs + kRunTeardownEstimateMs;
  total += std::max(0, scenario.warmup_ms);
  total += std::max(0, scenario.duration_ms);
  total += std::max(0, scenario.drain_ms);
  return total;
}

int64_t plan_remaining_ms(const std::vector<Bench::Scenario>& scenarios, size_t from_index) {
  int64_t total = 0;
  for (size_t i = from_index; i < scenarios.size(); ++i) {
    total += plan_case_estimate_ms(scenarios[i]);

    if (i + 1 < scenarios.size()) {
      total += kRunCooldownMs;
    }
  }

  return total;
}

std::string make_run_eta_line(size_t done, size_t total, int64_t elapsed_ms, int64_t plan_left_ms, bool include_avg) {
  std::string line;
  line.reserve(96);
  line.append("  \xE2\x8F\xB1  ");

  if (include_avg && done > 0) {
    const double avg_sec = static_cast<double>(elapsed_ms) / static_cast<double>(done) / 1000.0;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "~%.1fs/case \xC2\xB7 ", avg_sec);
    line.append(buf);
  }

  line.append(std::to_string(done));
  line.push_back('/');
  line.append(std::to_string(total));
  line.append(" done \xC2\xB7 elapsed ");
  line.append(format_duration_short(elapsed_ms));

  if (done < total) {
    int64_t eta_ms = plan_left_ms;

    if (done > 0) {
      const auto actual_left = static_cast<int64_t>(static_cast<double>(elapsed_ms) *
                                                    static_cast<double>(total - done) / static_cast<double>(done));
      eta_ms = (plan_left_ms + actual_left) / 2;
    }

    line.append(" \xC2\xB7 eta ");
    line.append(format_duration_short(eta_ms));
  } else {
    line.append(" \xC2\xB7 finished");
  }

  return line;
}

std::string make_run_progress_line(int elapsed_ms, int warmup_ms, int duration_ms, int drain_ms, int width) {
  const int setup_ms = kRunSetupEstimateMs;
  const int teardown_ms = kRunTeardownEstimateMs;
  const int total_ms = std::max(1, setup_ms + warmup_ms + duration_ms + drain_ms + teardown_ms);
  const int end_setup = setup_ms;
  const int end_warmup = end_setup + warmup_ms;
  const int end_measure = end_warmup + duration_ms;
  const int end_drain = end_measure + drain_ms;
  const double ratio = std::min(1.0, std::max(0.0, static_cast<double>(elapsed_ms) / total_ms));

  std::string_view phase_label;
  const char* phase_style = nullptr;

  if (elapsed_ms < end_setup) {
    phase_label = "setup";
    phase_style = kRunStylePhaseWarmup;
  } else if (elapsed_ms < end_warmup) {
    phase_label = "warmup";
    phase_style = kRunStylePhaseWarmup;
  } else if (elapsed_ms < end_measure) {
    phase_label = "measure";
    phase_style = kRunStylePhaseMeasure;
  } else if (elapsed_ms < end_drain) {
    phase_label = "drain";
    phase_style = kRunStylePhaseDrain;
  } else {
    phase_label = "wrap-up";
    phase_style = kRunStylePhaseWrap;
  }

  const int indent_w = 2;
  const int icon_w = 2;
  const int phase_col_w = 7;
  const int gap_w = 2;
  const int elapsed_col_w = 14;
  const int overhead = indent_w + icon_w + phase_col_w + gap_w + gap_w + elapsed_col_w;
  int bar_w = std::max(8, std::min(36, width - overhead - 2));

  if (width - overhead - 2 < 8) {
    bar_w = std::max(4, width - overhead - 2);
  }

  const int filled = static_cast<int>(std::lround(ratio * bar_w));
  const int remain = std::max(0, bar_w - filled);

  auto pixel_at = [bar_w, total_ms](int ms) {
    return std::clamp(static_cast<int>(std::lround(static_cast<double>(ms) / total_ms * bar_w)), 0, bar_w);
  };
  const int px_setup_end = pixel_at(end_setup);
  const int px_warmup_end = pixel_at(end_warmup);
  const int px_measure_end = pixel_at(end_measure);
  const int px_drain_end = pixel_at(end_drain);

  auto build_segment = [filled](int from_px, int to_px) {
    const int lo = std::min(from_px, filled);
    const int hi = std::min(to_px, filled);
    std::string seg;

    if (hi > lo) {
      seg.reserve(static_cast<size_t>(hi - lo) * 3);
      for (int i = lo; i < hi; ++i) seg.append("\xE2\x96\x88");
    }

    return seg;
  };

  std::string empty_bar;
  empty_bar.reserve(static_cast<size_t>(remain) * 3);
  for (int i = 0; i < remain; ++i) empty_bar.append("\xE2\x96\x91");

  char elapsed_buf[32];
  std::snprintf(elapsed_buf, sizeof(elapsed_buf), "%.1f / %.1f s", static_cast<double>(elapsed_ms) / 1000.0,
                static_cast<double>(total_ms) / 1000.0);

  const bool color = use_run_output_color();
  std::string line;
  line.reserve(384);
  line.append("  ");

  if (color) {
    line.append(phase_style);
    line.append(kRunIconPending);
    line.append(fit_cell_left(phase_label, phase_col_w));
    line.append(kRunAnsiReset);
    line.append("  ");
    auto append_phase_seg = [&line](const std::string& seg, const char* style) {
      if (seg.empty()) {
        return;
      }

      line.append(style);
      line.append(seg);
      line.append(kRunAnsiReset);
    };
    append_phase_seg(build_segment(0, px_setup_end), kRunStylePhaseWarmup);
    append_phase_seg(build_segment(px_setup_end, px_warmup_end), kRunStylePhaseWarmup);
    append_phase_seg(build_segment(px_warmup_end, px_measure_end), kRunStylePhaseMeasure);
    append_phase_seg(build_segment(px_measure_end, px_drain_end), kRunStylePhaseDrain);
    append_phase_seg(build_segment(px_drain_end, bar_w), kRunStylePhaseWrap);
    line.append(kRunStyleBarEmpty);
    line.append(empty_bar);
    line.append(kRunAnsiReset);
    line.append("  ");
    line.append(kRunStyleDim);
    line.append(fit_cell_right(elapsed_buf, elapsed_col_w));
    line.append(kRunAnsiReset);
  } else {
    std::string filled_bar;
    filled_bar.reserve(static_cast<size_t>(filled) * 3);
    for (int i = 0; i < filled; ++i) filled_bar.append("\xE2\x96\x88");
    line.append(kRunIconPending);
    line.append(fit_cell_left(phase_label, phase_col_w));
    line.append("  ");
    line.append(filled_bar);
    line.append(empty_bar);
    line.append("  ");
    line.append(fit_cell_right(elapsed_buf, elapsed_col_w));
  }

  return line;
}

std::string make_run_result_line(const Bench::ScenarioResult& scenario_result, double elapsed_ms) {
  const bool success = scenario_result.success;
  const char* icon = kRunIconPass;
  const char* status_word = "OK";
  const char* icon_style = kRunStyleOk;

  if (!success) {
    icon = kRunIconFail;
    status_word = "FAIL";
    icon_style = kRunStyleFail;
  }

  char elapsed_buf[32];
  std::snprintf(elapsed_buf, sizeof(elapsed_buf), "%.2f s", elapsed_ms / 1000.0);

  const bool color = use_run_output_color();
  std::string line;
  line.reserve(256);
  line.append("  ");

  if (color) {
    line.append(icon_style);
    line.append(icon);
    line.append(fit_cell_left(status_word, 4));
    line.append(kRunAnsiReset);
  } else {
    line.append(icon);
    line.append(fit_cell_left(status_word, 4));
  }

  line.append("  ");

  if (color) {
    line.append(kRunStyleDim);
  }

  line.append(fit_cell_right(elapsed_buf, 10));

  if (color) {
    line.append(kRunAnsiReset);
  }

  auto append_metric = [&line, &color](std::string_view label, std::string_view value, int value_w,
                                       std::string_view unit) {
    line.append(kRunColSep);

    if (color) {
      line.append(kRunStyleDim);
    }

    line.append(label);
    line.push_back(' ');

    if (color) {
      line.append(kRunAnsiReset);
    }

    line.append(fit_cell_right(value, value_w));
    line.push_back(' ');

    if (color) {
      line.append(kRunStyleDim);
    }

    line.append(unit);

    if (color) {
      line.append(kRunAnsiReset);
    }
  };

  if (success) {
    if (scenario_result.scenario.suite == Bench::kSerializationSuite) {
      append_metric("enc", format_decimal(scenario_result.serialize_mb_per_sec, 2), 8, "MB/s");
      append_metric("dec", format_decimal(scenario_result.deserialize_mb_per_sec, 2), 8, "MB/s");
      append_metric("rss", format_decimal(scenario_result.memory_usage, 1), 6, "MB");
    } else if (scenario_result.scenario.suite == Bench::kLatencySuite) {
      append_metric("recv", format_decimal(scenario_result.recv_mb_per_sec, 2), 8, "MB/s");
      append_metric("p95", format_decimal(scenario_result.p95_latency_us, 2), 8, "us");
      append_metric("loss", std::to_string(scenario_result.lost), 6, "");
    } else {
      append_metric("recv", format_decimal(scenario_result.recv_mb_per_sec, 2), 8, "MB/s");
      append_metric("msg", format_decimal(scenario_result.recv_msgs_per_sec, 2), 8, "/s");
      append_metric("loss", std::to_string(scenario_result.lost), 6, "");
    }
  } else if (!scenario_result.error.empty()) {
    line.append(kRunColSep);

    if (color) {
      line.append(kRunStyleFail);
    }

    line.append("error: ");

    if (color) {
      line.append(kRunAnsiReset);
    }

    line.append(scenario_result.error);
  }

  return line;
}

class ProgressTicker final {
 public:
  ProgressTicker(bool enabled, int warmup_ms, int duration_ms, int drain_ms);
  ~ProgressTicker();
  ProgressTicker(const ProgressTicker&) = delete;
  ProgressTicker& operator=(const ProgressTicker&) = delete;
  void stop() noexcept;

 private:
  bool enabled_;
  int warmup_ms_;
  int duration_ms_;
  int drain_ms_;
  uint64_t start_ns_;
  std::unique_ptr<MessageLoop> loop_;
  std::unique_ptr<Timer> timer_;
  std::atomic_bool stopped_{false};
};

ProgressTicker::ProgressTicker(bool enabled, int warmup_ms, int duration_ms, int drain_ms)
    : enabled_(enabled),
      warmup_ms_(std::max(0, warmup_ms)),
      duration_ms_(std::max(0, duration_ms)),
      drain_ms_(std::max(0, drain_ms)),
      start_ns_(ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano)) {
  if VUNLIKELY (!enabled_) {
    return;
  }

  std::string buf;
  buf.reserve(256);
  buf.append("\r\033[2K");
  buf.append(make_run_progress_line(0, warmup_ms_, duration_ms_, drain_ms_, get_run_output_width()));
  VLINK_TERM_OUT.write_raw(buf.data(), buf.size());
  VLINK_TERM_OUT.flush();

  loop_ = std::make_unique<MessageLoop>();
  loop_->async_run();
  timer_ = std::make_unique<Timer>(loop_.get(), 100U, Timer::kInfinite, [this]() {
    const auto elapsed_ms =
        static_cast<int>((ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano) - start_ns_) / 1000000ULL);
    std::string tick;
    tick.reserve(256);
    tick.append("\r\033[2K");
    tick.append(make_run_progress_line(elapsed_ms, warmup_ms_, duration_ms_, drain_ms_, get_run_output_width()));
    VLINK_TERM_OUT.write_raw(tick.data(), tick.size());
    VLINK_TERM_OUT.flush();
  });
  timer_->start();
}

ProgressTicker::~ProgressTicker() { stop(); }

void ProgressTicker::stop() noexcept {
  if VUNLIKELY (!enabled_) {
    return;
  }

  if (stopped_.exchange(true)) {
    return;
  }

  if (timer_) {
    timer_->stop();
  }

  if (loop_) {
    loop_->quit();
    loop_->wait_for_quit();
  }

  static constexpr std::string_view kClearLine{"\r\033[2K"};
  VLINK_TERM_OUT.write_raw(kClearLine.data(), kClearLine.size());
  VLINK_TERM_OUT.flush();
}

int split_publisher_workload(int total, int publishers, int publisher_index) noexcept;

bool check_stop_requested(std::string& error) {
  if (!Bench::stop_requested()) {
    return false;
  }

  if (error.empty()) {
    error = "terminated by signal";
  }

  return true;
}

const char* process_error_to_string(Process::Error error) {
  switch (error) {
    case Process::kNoError:
      return "no error";
    case Process::kUnknownError:
      return "unknown process error";
    case Process::kStartError:
      return "process start error";
    case Process::kCrashedError:
      return "process crashed";
    case Process::kTimedOutError:
      return "process timeout";
    case Process::kWriteError:
      return "process write error";
    case Process::kReadError:
      return "process read error";
    case Process::kBufferOverflowError:
      return "process output buffer overflow";
  }

  return "process error";
}

const char* process_state_to_string(Process::State state) {
  switch (state) {
    case Process::kNotRunningState:
      return "not-running";
    case Process::kStartingState:
      return "starting";
    case Process::kRunningState:
      return "running";
  }

  return "unknown";
}

std::string format_process_failure(Process& process, const std::string& fallback) {
  std::string stderr_text;
  process.read_all_error(stderr_text);

  std::string error = fallback;

  if (!process.is_running()) {
    const int code = process.get_exit_code();
    const auto process_error = process.get_error();

    if (code < 0) {
      error += " (";
      error += process_error_to_string(process_error);
      error += ", state ";
      error += process_state_to_string(process.get_state());
      error += ", exit code unavailable";
      error += ")";
    } else if (process.get_exit_status() == Process::kCrashExitStatus) {
      error += " (worker crashed, exit code " + std::to_string(code) + ")";
    } else if (code != 0) {
      error += " (worker exit code " + std::to_string(code) + ")";
    }
  }

  if (!stderr_text.empty()) {
    error += ": " + stderr_text;
  }

  return error;
}

template <typename DurationT>
bool sleep_for_or_stop(DurationT duration, std::string& error) {
  const uint64_t deadline_ns =
      ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano) +
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());

  while (true) {
    const uint64_t now_ns = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano);

    if (now_ns >= deadline_ns) {
      break;
    }

    if VUNLIKELY (check_stop_requested(error)) {
      return false;
    }

    const uint64_t remaining_ns = deadline_ns - now_ns;
    const uint64_t sleep_ms = std::min<uint64_t>(remaining_ns / 1000000ULL, 50ULL);

    if (sleep_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }
  }

  return !check_stop_requested(error);
}

bool sleep_until_or_stop(uint64_t deadline_ns, std::string& error) {
  while (true) {
    const uint64_t now_ns = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano);

    if (now_ns >= deadline_ns) {
      break;
    }

    if VUNLIKELY (check_stop_requested(error)) {
      return false;
    }

    const uint64_t remaining_ns = deadline_ns - now_ns;
    const uint64_t sleep_ms = std::min<uint64_t>(remaining_ns / 1000000ULL, 50ULL);

    if (sleep_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }
  }

  return !check_stop_requested(error);
}

void sleep_until_or_stop_unchecked(uint64_t deadline_ns) {
  while (!Bench::stop_requested()) {
    const uint64_t now_ns = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano);

    if (now_ns >= deadline_ns) {
      break;
    }

    const uint64_t remaining_ns = deadline_ns - now_ns;
    const uint64_t sleep_ms = std::min<uint64_t>(remaining_ns / 1000000ULL, 50ULL);

    if (sleep_ms == 0) {
      break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  }
}

struct BenchFrameHeader final {
  uint64_t seq{0};
  uint64_t send_ns{0};
  uint32_t payload_size{0};
  uint32_t publisher_index{0};
};

static_assert(std::is_standard_layout_v<BenchFrameHeader>, "BenchFrameHeader must be standard-layout.");
static_assert(std::is_trivially_copyable_v<BenchFrameHeader>, "BenchFrameHeader must be trivially-copyable.");

inline uint64_t steady_time_ns() noexcept { return ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano); }

bool wait_for_worker_start(uint64_t& start_ns, std::string& error) {
  std::string start_signal;

  if (!std::getline(std::cin, start_signal)) {
    error = "worker start signal failed";
    return false;
  }

  start_signal = Helpers::trim_string(start_signal);

  constexpr std::string_view kPrefix = "GO ";

  if (!Helpers::has_startwith(start_signal, kPrefix)) {
    error = "worker start signal failed";
    return false;
  }

  try {
    size_t pos = 0;
    const auto parsed = std::stoull(start_signal.substr(kPrefix.size()), &pos);

    if (pos != start_signal.size() - kPrefix.size()) {
      error = "worker start signal failed";
      return false;
    }

    start_ns = static_cast<uint64_t>(parsed);
  } catch (std::exception&) {
    error = "worker start signal failed";
    return false;
  }

  return true;
}

std::string get_now_string() {
  std::time_t now = std::time(nullptr);
  std::tm tm_now{};
#ifdef _WIN32
  ::localtime_s(&tm_now, &now);
#else
  ::localtime_r(&now, &tm_now);
#endif
  std::ostringstream stream;
  stream << std::put_time(&tm_now, "%Y-%m-%d %H:%M:%S");
  return stream.str();
}

std::string get_platform_name() {
#ifdef _WIN32
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#elif defined(__linux__)
  return "linux";
#else
  return "unknown";
#endif
}

std::string to_lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(::tolower(c)); });
  return value;
}

std::string get_transport_from_url(const std::string& url) {
  auto pos = url.find("://");

  if (pos == std::string::npos) {
    return "unknown";
  }

  return url.substr(0, pos);
}

const std::string& make_runtime_url(const Bench::Scenario& scenario) { return scenario.url; }

bool parse_property_entry(const std::string& entry, std::string& key, std::string& value) {
  auto pos = entry.find('=');

  if (pos == std::string::npos || pos == 0 || pos + 1 >= entry.size()) {
    return false;
  }

  key = entry.substr(0, pos);
  value = entry.substr(pos + 1);
  return !key.empty();
}

std::vector<std::string> qos_to_property_entries(const Qos& qos) {
  std::vector<std::string> properties;
  properties.reserve(18);
  properties.emplace_back("qos.reliability.kind=" + std::to_string(qos.reliability.kind));
  properties.emplace_back("qos.reliability.block_time=" + std::to_string(qos.reliability.block_time));
  properties.emplace_back("qos.reliability.heartbeat_time=" + std::to_string(qos.reliability.heartbeat_time));
  properties.emplace_back("qos.history.kind=" + std::to_string(qos.history.kind));
  properties.emplace_back("qos.history.depth=" + std::to_string(qos.history.depth));
  properties.emplace_back("qos.durability.kind=" + std::to_string(qos.durability.kind));
  properties.emplace_back("qos.publish_mode.kind=" + std::to_string(qos.publish_mode.kind));
  properties.emplace_back("qos.liveliness.kind=" + std::to_string(qos.liveliness.kind));
  properties.emplace_back("qos.liveliness.duration=" + std::to_string(qos.liveliness.duration));
  properties.emplace_back("qos.destination_order.kind=" + std::to_string(qos.destination_order.kind));
  properties.emplace_back("qos.ownership.kind=" + std::to_string(qos.ownership.kind));
  properties.emplace_back("qos.deadline.period=" + std::to_string(qos.deadline.period));
  properties.emplace_back("qos.lifespan.duration=" + std::to_string(qos.lifespan.duration));
  properties.emplace_back("qos.latency_budget.duration=" + std::to_string(qos.latency_budget.duration));
  properties.emplace_back("qos.resource_limits.max_samples=" + std::to_string(qos.resource_limits.max_samples));
  properties.emplace_back("qos.resource_limits.max_instances=" + std::to_string(qos.resource_limits.max_instances));
  properties.emplace_back("qos.resource_limits.max_samples_per_instance=" +
                          std::to_string(qos.resource_limits.max_samples_per_instance));
  properties.emplace_back("qos.additions.priority=" + std::to_string(qos.additions.priority));
  properties.emplace_back(std::string("qos.additions.is_express=") + (qos.additions.is_express ? "true" : "false"));
  return properties;
}

bool append_profile_properties(const std::string& qos_profile, std::vector<std::string>& properties,
                               std::string& error) {
  if (qos_profile.empty() || qos_profile == "default") {
    return true;
  }

  const auto& qos_map = QosProfile::get_available_qos_map();
  auto iter = qos_map.find(qos_profile);

  if (iter == qos_map.end()) {
    error = "unknown qos profile: " + qos_profile;
    return false;
  }

  auto profile_properties = qos_to_property_entries(iter->second);
  properties.insert(properties.end(), profile_properties.begin(), profile_properties.end());
  return true;
}

template <typename NodeT>
bool init_node_with_properties(NodeT& node, const std::string& qos_profile, const std::vector<std::string>& properties,
                               const std::vector<std::string>& side_properties, std::string& error) {
  std::vector<std::string> merged_properties = properties;

  if (!append_profile_properties(qos_profile, merged_properties, error)) {
    return false;
  }

  merged_properties.insert(merged_properties.end(), side_properties.begin(), side_properties.end());

  for (const auto& entry : merged_properties) {
    std::string key;
    std::string value;

    if (!parse_property_entry(entry, key, value)) {
      error = "invalid property: " + entry;
      return false;
    }

    node.set_property(key, value);
  }

  if (!node.init()) {
    error = "node init failed";
    return false;
  }

  return true;
}

size_t normalized_payload_size(size_t payload_size) noexcept {
  return std::max(payload_size, sizeof(BenchFrameHeader));
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

std::string format_decimal(double value, int precision = 2) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(precision) << value;
  return stream.str();
}

struct LatencyStats final {
  double avg{0.0};
  double p50{0.0};
  double p90{0.0};
  double p95{0.0};
  double p99{0.0};
  double p999{0.0};
  double p9999{0.0};
  double max{0.0};
  double stddev{0.0};
};

double get_percentile(const std::vector<double>& values, double percentile) {
  if (values.empty()) {
    return 0.0;
  }

  double pos = (percentile / 100.0) * static_cast<double>(values.size() - 1);
  auto lower = static_cast<size_t>(std::floor(pos));
  auto upper = static_cast<size_t>(std::ceil(pos));

  if (lower == upper) {
    return values.at(lower);
  }

  double ratio = pos - static_cast<double>(lower);
  return values.at(lower) + (values.at(upper) - values.at(lower)) * ratio;
}

LatencyStats calculate_latency_stats(std::vector<double> values) {
  LatencyStats stats;

  if (values.empty()) {
    return stats;
  }

  std::sort(values.begin(), values.end());
  stats.max = values.back();
  stats.avg = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
  stats.p50 = get_percentile(values, 50.0);
  stats.p90 = get_percentile(values, 90.0);
  stats.p95 = get_percentile(values, 95.0);
  stats.p99 = get_percentile(values, 99.0);
  stats.p999 = get_percentile(values, 99.9);
  stats.p9999 = get_percentile(values, 99.99);

  double variance = 0.0;

  for (const auto value : values) {
    double diff = value - stats.avg;
    variance += diff * diff;
  }

  stats.stddev = std::sqrt(variance / static_cast<double>(values.size()));
  return stats;
}

template <typename MsgT>
struct BenchCodec;

template <>
struct BenchCodec<Bytes> final {
  static Bytes create_template(size_t payload_size) {
    size_t total_size = normalized_payload_size(payload_size);
    Bytes bytes = Bytes::create(total_size);
    std::fill(bytes.data() + sizeof(BenchFrameHeader), bytes.data() + total_size, static_cast<uint8_t>(0xA5));
    return bytes;
  }

  static void stamp(Bytes& bytes, size_t payload_size, uint64_t seq, uint32_t publisher_index, uint64_t send_ns) {
    BenchFrameHeader header;
    header.seq = seq;
    header.send_ns = send_ns;
    header.payload_size = static_cast<uint32_t>(payload_size);
    header.publisher_index = publisher_index;
    std::memcpy(bytes.data(), &header, sizeof(header));
  }

  static Bytes make(size_t payload_size, uint64_t seq, uint32_t publisher_index, uint64_t send_ns) {
    Bytes bytes = create_template(payload_size);
    stamp(bytes, payload_size, seq, publisher_index, send_ns);
    return bytes;
  }

  static bool extract(const Bytes& message, uint64_t& send_ns) {
    if (message.size() < sizeof(BenchFrameHeader)) {
      return false;
    }

    BenchFrameHeader header;
    std::memcpy(&header, message.data(), sizeof(header));
    send_ns = header.send_ns;
    return true;
  }

  static size_t wire_size(size_t payload_size) { return make(payload_size, 1, 0, steady_time_ns()).size(); }
};

template <>
struct BenchCodec<std::string> final {
  static std::string create_template(size_t payload_size) {
    size_t total_size = normalized_payload_size(payload_size);
    std::string message(total_size, 'A');
    return message;
  }

  static void stamp(std::string& message, size_t payload_size, uint64_t seq, uint32_t publisher_index,
                    uint64_t send_ns) {
    BenchFrameHeader header;
    header.seq = seq;
    header.send_ns = send_ns;
    header.payload_size = static_cast<uint32_t>(payload_size);
    header.publisher_index = publisher_index;
    std::memcpy(message.data(), &header, sizeof(header));
  }

  static std::string make(size_t payload_size, uint64_t seq, uint32_t publisher_index, uint64_t send_ns) {
    std::string message = create_template(payload_size);
    stamp(message, payload_size, seq, publisher_index, send_ns);
    return message;
  }

  static bool extract(const std::string& message, uint64_t& send_ns) {
    if (message.size() < sizeof(BenchFrameHeader)) {
      return false;
    }

    BenchFrameHeader header;
    std::memcpy(&header, message.data(), sizeof(header));
    send_ns = header.send_ns;
    return true;
  }

  static size_t wire_size(size_t payload_size) { return make(payload_size, 1, 0, steady_time_ns()).size(); }
};

template <>
struct BenchCodec<zerocopy::RawData> final {
  static zerocopy::RawData create_template(size_t payload_size) {
    zerocopy::RawData message;
    size_t total_size = normalized_payload_size(payload_size);
    message.create(total_size);
    std::fill(const_cast<uint8_t*>(message.data()) + sizeof(BenchFrameHeader),
              const_cast<uint8_t*>(message.data()) + total_size, static_cast<uint8_t>(0xA5));
    return message;
  }

  static void stamp(zerocopy::RawData& message, size_t payload_size, uint64_t seq, uint32_t publisher_index,
                    uint64_t send_ns) {
    message.header.seq = static_cast<uint32_t>(seq);
    message.header.time_pub = send_ns;
    BenchFrameHeader header;
    header.seq = seq;
    header.send_ns = send_ns;
    header.payload_size = static_cast<uint32_t>(payload_size);
    header.publisher_index = publisher_index;
    std::memcpy(const_cast<uint8_t*>(message.data()), &header, sizeof(header));
  }

  static zerocopy::RawData make(size_t payload_size, uint64_t seq, uint32_t publisher_index, uint64_t send_ns) {
    zerocopy::RawData message = create_template(payload_size);
    stamp(message, payload_size, seq, publisher_index, send_ns);
    return message;
  }

  static bool extract(const zerocopy::RawData& message, uint64_t& send_ns) {
    if (message.size() < sizeof(BenchFrameHeader)) {
      return false;
    }

    send_ns = message.header.time_pub;
    return send_ns != 0;
  }

  static size_t wire_size(size_t payload_size) {
    zerocopy::RawData message = make(payload_size, 1, 0, steady_time_ns());
    return Serializer::get_serialized_size(message);
  }
};

struct Collector final {
  uint64_t start_ns{0};
  std::atomic<uint64_t> measure_begin_ns{0};
  std::atomic<uint64_t> measure_end_ns{0};
  size_t wire_size{0};
  bool enable_latency{false};

  std::atomic<uint64_t> first_receive_ns{0};
  std::atomic<uint64_t> received{0};
  std::atomic<uint64_t> measured_received{0};
  std::atomic<uint64_t> measured_bytes{0};
  std::atomic<uint64_t> latency_samples_dropped{0};

  std::mutex latency_mtx;
  std::vector<double> latencies_us;

  void on_message(uint64_t send_ns) {
    uint64_t now_ns = steady_time_ns();
    const uint64_t begin_ns = this->measure_begin_ns.load(std::memory_order_relaxed);
    const uint64_t end_ns = this->measure_end_ns.load(std::memory_order_relaxed);
    received.fetch_add(1, std::memory_order_relaxed);

    uint64_t expected = 0;
    first_receive_ns.compare_exchange_strong(expected, now_ns, std::memory_order_relaxed);

    if (now_ns < begin_ns || now_ns > end_ns || send_ns == 0) {
      return;
    }

    measured_received.fetch_add(1, std::memory_order_relaxed);
    measured_bytes.fetch_add(static_cast<uint64_t>(wire_size), std::memory_order_relaxed);

    if (!enable_latency) {
      return;
    }

    std::lock_guard lock(latency_mtx);

    if (latencies_us.size() < kMaxLatencySamples) {
      latencies_us.push_back(static_cast<double>(now_ns - send_ns) / 1000.0);
    } else {
      latency_samples_dropped.fetch_add(1, std::memory_order_relaxed);
    }
  }
};

class ResourceSampler final {
 public:
  ResourceSampler() = default;

  ~ResourceSampler() {
    running_.store(false, std::memory_order_release);

    if (worker_.joinable()) {
      worker_.join();
    }
  }

  ResourceSampler(const ResourceSampler&) = delete;
  ResourceSampler& operator=(const ResourceSampler&) = delete;
  ResourceSampler(ResourceSampler&&) = delete;
  ResourceSampler& operator=(ResourceSampler&&) = delete;

  void start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }

    start_wall_ns_ = steady_time_ns();
    start_cpu_us_ = get_process_cpu_time_us();
    peak_memory_mb_ = get_process_rss_mb();

    worker_ = std::thread([this]() {
      while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        sample_memory_peak();
      }
    });
  }

  void stop(double& cpu_usage, double& memory_usage) {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
      cpu_usage = 0.0;
      memory_usage = 0.0;
      return;
    }

    if (worker_.joinable()) {
      worker_.join();
    }

    sample_memory_peak();

    const uint64_t end_wall_ns = steady_time_ns();
    const uint64_t end_cpu_us = get_process_cpu_time_us();
    const auto wall_us = static_cast<double>(std::max<uint64_t>(end_wall_ns - start_wall_ns_, 1)) / 1000.0;
    const auto cpu_us = static_cast<double>(end_cpu_us >= start_cpu_us_ ? (end_cpu_us - start_cpu_us_) : 0);

    std::lock_guard lock(mtx_);
    cpu_usage = wall_us > 0.0 ? (cpu_us * 100.0 / wall_us) : 0.0;
    memory_usage = peak_memory_mb_;
  }

 private:
  static uint64_t get_process_cpu_time_us() noexcept {
#ifdef _WIN32
    FILETIME creation_time;
    FILETIME exit_time;
    FILETIME kernel_time;
    FILETIME user_time;

    if VUNLIKELY (!::GetProcessTimes(::GetCurrentProcess(), &creation_time, &exit_time, &kernel_time, &user_time)) {
      return 0;
    }

    ULARGE_INTEGER kernel{};
    ULARGE_INTEGER user{};
    kernel.LowPart = kernel_time.dwLowDateTime;
    kernel.HighPart = kernel_time.dwHighDateTime;
    user.LowPart = user_time.dwLowDateTime;
    user.HighPart = user_time.dwHighDateTime;
    return (kernel.QuadPart + user.QuadPart) / 10ULL;
#elif defined(_POSIX_VERSION)
    struct rusage usage{};

    if VUNLIKELY (::getrusage(RUSAGE_SELF, &usage) != 0) {
      return 0;
    }

    const uint64_t user_time = (static_cast<uint64_t>(usage.ru_utime.tv_sec) * 1000'000ULL) + usage.ru_utime.tv_usec;
    const uint64_t system_time = (static_cast<uint64_t>(usage.ru_stime.tv_sec) * 1000'000ULL) + usage.ru_stime.tv_usec;
    return user_time + system_time;
#else
    return 0;
#endif
  }

  static double get_process_rss_mb() noexcept {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};

    if VUNLIKELY (!::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                          sizeof(counters))) {
      return 0.0;
    }

    return static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
#elif defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;

    if VUNLIKELY (::task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
                  KERN_SUCCESS) {
      return 0.0;
    }

    return static_cast<double>(info.resident_size) / (1024.0 * 1024.0);
#elif defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    int64_t total_pages = 0;
    int64_t rss_pages = 0;

    if (statm.is_open() && (statm >> total_pages >> rss_pages) && rss_pages > 0) {
      const auto page_size = static_cast<int64_t>(::sysconf(_SC_PAGESIZE));

      if (page_size > 0) {
        return static_cast<double>(rss_pages) * static_cast<double>(page_size) / (1024.0 * 1024.0);
      }
    }

    return 0.0;
#else
    return 0.0;
#endif
  }

  void sample_memory_peak() {
    const double rss_mb = get_process_rss_mb();
    std::lock_guard lock(mtx_);
    peak_memory_mb_ = std::max(peak_memory_mb_, rss_mb);
  }

  std::atomic_bool running_{false};
  std::thread worker_;
  std::mutex mtx_;
  double peak_memory_mb_{0.0};
  uint64_t start_wall_ns_{0};
  uint64_t start_cpu_us_{0};
};

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

#ifndef _WIN32
  std::filesystem::permissions(parent,
                               std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                                   std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
                                   std::filesystem::perms::others_exec,
                               std::filesystem::perm_options::replace, ec);
#endif

  return true;
}

void scenario_to_json(json& obj, const Bench::Scenario& scenario) {
  obj["suite"] = Bench::suite_to_string(scenario.suite);
  obj["mode"] = Bench::mode_to_string(scenario.mode);
  obj["topology"] = Bench::topology_to_string(scenario.topology);
  obj["rate_pattern"] = Bench::rate_pattern_to_string(scenario.rate_pattern);
  obj["payload"] = Bench::payload_to_string(scenario.payload);
  obj["url"] = scenario.url;
  obj["qos_profile"] = scenario.qos_profile;
  obj["properties"] = scenario.properties;
  obj["pub_properties"] = scenario.pub_properties;
  obj["sub_properties"] = scenario.sub_properties;
  obj["payload_size"] = scenario.payload_size;
  obj["rate_hz"] = scenario.rate_hz;
  obj["burst_messages"] = scenario.burst_messages;
  obj["subscribers"] = scenario.subscribers;
  obj["publishers"] = scenario.publishers;
  obj["warmup_ms"] = scenario.warmup_ms;
  obj["duration_ms"] = scenario.duration_ms;
  obj["drain_ms"] = scenario.drain_ms;
  obj["repeat_index"] = scenario.repeat_index;
  obj["subscriber_sleep_us"] = scenario.subscriber_sleep_us;
}

bool scenario_from_json(const json& obj, Bench::Scenario& scenario, std::string& error) {
  if (!obj.contains("suite") || !obj.contains("mode") || !obj.contains("payload")) {
    error = "scenario json is incomplete";
    return false;
  }

  std::string suite_str = obj.value("suite", std::string());
  std::string mode_str = obj.value("mode", std::string());
  std::string topology_str = obj.value("topology", std::string("1:1"));
  std::string rate_pattern_str = obj.value("rate_pattern", std::string("max"));
  std::string payload_str = obj.value("payload", std::string());

  if (!Bench::parse_suite(suite_str, scenario.suite) || !Bench::parse_mode(mode_str, scenario.mode) ||
      !Bench::parse_topology(topology_str, scenario.topology) ||
      !Bench::parse_rate_pattern(rate_pattern_str, scenario.rate_pattern) ||
      !Bench::parse_payload(payload_str, scenario.payload)) {
    error = "scenario json contains invalid enum value";
    return false;
  }

  scenario.url = obj.value("url", std::string());
  scenario.qos_profile = obj.value("qos_profile", std::string());
  scenario.properties = obj.value("properties", std::vector<std::string>());
  scenario.pub_properties = obj.value("pub_properties", std::vector<std::string>());
  scenario.sub_properties = obj.value("sub_properties", std::vector<std::string>());
  scenario.payload_size = obj.value("payload_size", size_t{0});
  scenario.rate_hz = obj.value("rate_hz", 0);
  scenario.burst_messages = obj.value("burst_messages", 1);
  scenario.subscribers = obj.value("subscribers", 1);
  scenario.publishers = obj.value("publishers", 1);
  scenario.warmup_ms = obj.value("warmup_ms", 1000);
  scenario.duration_ms = obj.value("duration_ms", 3000);
  scenario.drain_ms = obj.value("drain_ms", 300);
  scenario.repeat_index = obj.value("repeat_index", 0);
  scenario.subscriber_sleep_us = obj.value("subscriber_sleep_us", 0);
  return true;
}

void result_to_json(json& obj, const Bench::ScenarioResult& result) {
  json scenario_json;
  scenario_to_json(scenario_json, result.scenario);
  obj["scenario"] = std::move(scenario_json);
  obj["transport"] = result.transport;
  obj["wire_size"] = result.wire_size;
  obj["success"] = result.success;
  obj["error"] = result.error;
  obj["sent"] = result.sent;
  obj["received"] = result.received;
  obj["expected"] = result.expected;
  obj["lost"] = result.lost;
  obj["discovery_ms"] = result.discovery_ms;
  obj["first_message_ms"] = result.first_message_ms;
  obj["send_msgs_per_sec"] = result.send_msgs_per_sec;
  obj["recv_msgs_per_sec"] = result.recv_msgs_per_sec;
  obj["send_mb_per_sec"] = result.send_mb_per_sec;
  obj["recv_mb_per_sec"] = result.recv_mb_per_sec;
  obj["avg_latency_us"] = result.avg_latency_us;
  obj["p50_latency_us"] = result.p50_latency_us;
  obj["p90_latency_us"] = result.p90_latency_us;
  obj["p95_latency_us"] = result.p95_latency_us;
  obj["p99_latency_us"] = result.p99_latency_us;
  obj["p999_latency_us"] = result.p999_latency_us;
  obj["p9999_latency_us"] = result.p9999_latency_us;
  obj["max_latency_us"] = result.max_latency_us;
  obj["latency_stddev_us"] = result.latency_stddev_us;
  obj["latency_samples_dropped"] = result.latency_samples_dropped;
  obj["avg_send_block_us"] = result.avg_send_block_us;
  obj["p50_send_block_us"] = result.p50_send_block_us;
  obj["p95_send_block_us"] = result.p95_send_block_us;
  obj["p99_send_block_us"] = result.p99_send_block_us;
  obj["max_send_block_us"] = result.max_send_block_us;
  obj["send_block_samples"] = result.send_block_samples;
  obj["serialize_msgs_per_sec"] = result.serialize_msgs_per_sec;
  obj["deserialize_msgs_per_sec"] = result.deserialize_msgs_per_sec;
  obj["serialize_mb_per_sec"] = result.serialize_mb_per_sec;
  obj["deserialize_mb_per_sec"] = result.deserialize_mb_per_sec;
  obj["pub_cpu_ms"] = result.pub_cpu_ms;
  obj["sub_cpu_ms"] = result.sub_cpu_ms;
  obj["cpu_usage"] = result.cpu_usage;
  obj["memory_usage"] = result.memory_usage;
}

bool result_from_json(const json& obj, Bench::ScenarioResult& result, std::string& error) {
  if (!obj.contains("scenario")) {
    error = "result json is incomplete";
    return false;
  }

  if (!scenario_from_json(obj.at("scenario"), result.scenario, error)) {
    return false;
  }

  result.transport = obj.value("transport", std::string());
  result.wire_size = obj.value("wire_size", size_t{0});
  result.success = obj.value("success", false);
  result.error = obj.value("error", std::string());
  result.sent = obj.value("sent", uint64_t{0});
  result.received = obj.value("received", uint64_t{0});
  result.expected = obj.value("expected", uint64_t{0});
  result.lost = obj.value("lost", uint64_t{0});
  result.discovery_ms = obj.value("discovery_ms", 0.0);
  result.first_message_ms = obj.value("first_message_ms", 0.0);
  result.send_msgs_per_sec = obj.value("send_msgs_per_sec", 0.0);
  result.recv_msgs_per_sec = obj.value("recv_msgs_per_sec", 0.0);
  result.send_mb_per_sec = obj.value("send_mb_per_sec", 0.0);
  result.recv_mb_per_sec = obj.value("recv_mb_per_sec", 0.0);
  result.avg_latency_us = obj.value("avg_latency_us", 0.0);
  result.p50_latency_us = obj.value("p50_latency_us", 0.0);
  result.p90_latency_us = obj.value("p90_latency_us", 0.0);
  result.p95_latency_us = obj.value("p95_latency_us", 0.0);
  result.p99_latency_us = obj.value("p99_latency_us", 0.0);
  result.p999_latency_us = obj.value("p999_latency_us", 0.0);
  result.p9999_latency_us = obj.value("p9999_latency_us", 0.0);
  result.max_latency_us = obj.value("max_latency_us", 0.0);
  result.latency_stddev_us = obj.value("latency_stddev_us", 0.0);
  result.latency_samples_dropped = obj.value("latency_samples_dropped", uint64_t{0});
  result.avg_send_block_us = obj.value("avg_send_block_us", 0.0);
  result.p50_send_block_us = obj.value("p50_send_block_us", 0.0);
  result.p95_send_block_us = obj.value("p95_send_block_us", 0.0);
  result.p99_send_block_us = obj.value("p99_send_block_us", 0.0);
  result.max_send_block_us = obj.value("max_send_block_us", 0.0);
  result.send_block_samples = obj.value("send_block_samples", uint64_t{0});
  result.serialize_msgs_per_sec = obj.value("serialize_msgs_per_sec", 0.0);
  result.deserialize_msgs_per_sec = obj.value("deserialize_msgs_per_sec", 0.0);
  result.serialize_mb_per_sec = obj.value("serialize_mb_per_sec", 0.0);
  result.deserialize_mb_per_sec = obj.value("deserialize_mb_per_sec", 0.0);
  result.pub_cpu_ms = obj.value("pub_cpu_ms", 0.0);
  result.sub_cpu_ms = obj.value("sub_cpu_ms", 0.0);
  result.cpu_usage = obj.value("cpu_usage", 0.0);
  result.memory_usage = obj.value("memory_usage", 0.0);
  return true;
}

template <typename MsgT>
bool run_serialization_case(const Bench::Scenario& scenario, Bench::ScenarioResult& result, std::string& error) {
  (void)error;

  result.transport = "cpu";
  result.wire_size = BenchCodec<MsgT>::wire_size(scenario.payload_size);

  MsgT sample = BenchCodec<MsgT>::make(scenario.payload_size, 1, 0, steady_time_ns());
  int duration_ms = std::max(scenario.duration_ms, 100);

  {
    Bytes wire;
    uint64_t count = 0;
    ElapsedTimer cpu_timer(ElapsedTimer::kCpuActiveTime, ElapsedTimer::kMicro);
    cpu_timer.start();
    uint64_t begin_ns = steady_time_ns();
    uint64_t deadline_ns = begin_ns + static_cast<uint64_t>(duration_ms) * 1000000ULL;

    while (steady_time_ns() < deadline_ns) {
      Serializer::serialize(sample, wire);
      ++count;
    }

    uint64_t elapsed_ns = std::max<uint64_t>(steady_time_ns() - begin_ns, 1);
    result.serialize_msgs_per_sec = static_cast<double>(count) * 1000000000.0 / static_cast<double>(elapsed_ns);
    result.serialize_mb_per_sec = static_cast<double>(count) * static_cast<double>(result.wire_size) /
                                  (1024.0 * 1024.0) * 1000000000.0 / static_cast<double>(elapsed_ns);
    result.pub_cpu_ms = static_cast<double>(cpu_timer.get()) / 1000.0;
  }

  {
    Bytes wire;
    Serializer::serialize(sample, wire);
    MsgT out;
    uint64_t count = 0;
    ElapsedTimer cpu_timer(ElapsedTimer::kCpuActiveTime, ElapsedTimer::kMicro);
    cpu_timer.start();
    uint64_t begin_ns = steady_time_ns();
    uint64_t deadline_ns = begin_ns + static_cast<uint64_t>(duration_ms) * 1000000ULL;

    while (steady_time_ns() < deadline_ns) {
      Serializer::deserialize(wire, out);
      ++count;
    }

    uint64_t elapsed_ns = std::max<uint64_t>(steady_time_ns() - begin_ns, 1);
    result.deserialize_msgs_per_sec = static_cast<double>(count) * 1000000000.0 / static_cast<double>(elapsed_ns);
    result.deserialize_mb_per_sec = static_cast<double>(count) * static_cast<double>(result.wire_size) /
                                    (1024.0 * 1024.0) * 1000000000.0 / static_cast<double>(elapsed_ns);
    result.sub_cpu_ms = static_cast<double>(cpu_timer.get()) / 1000.0;
  }

  result.success = true;
  return true;
}

template <typename MsgT>
bool run_local_pubsub_case(const Bench::Scenario& scenario, Bench::ScenarioResult& result, std::string& error) {
  const auto runtime_url = make_runtime_url(scenario);
  Collector collector;
  collector.start_ns = steady_time_ns();
  collector.measure_begin_ns.store(std::numeric_limits<uint64_t>::max(), std::memory_order_relaxed);
  collector.measure_end_ns.store(std::numeric_limits<uint64_t>::max(), std::memory_order_relaxed);
  collector.wire_size = BenchCodec<MsgT>::wire_size(scenario.payload_size);
  collector.enable_latency = scenario.suite == Bench::kLatencySuite;

  std::vector<std::unique_ptr<MessageLoop>> subscriber_loops;

  if (scenario.mode == Bench::kLocalLoopMode) {
    subscriber_loops.reserve(static_cast<size_t>(scenario.subscribers));

    for (int i = 0; i < scenario.subscribers; ++i) {
      auto loop = std::make_unique<MessageLoop>();
      loop->async_run();
      subscriber_loops.emplace_back(std::move(loop));
    }
  }

  std::vector<std::unique_ptr<Subscriber<MsgT>>> subscribers;
  subscribers.reserve(static_cast<size_t>(scenario.subscribers));

  for (int i = 0; i < scenario.subscribers; ++i) {
    auto sub = std::make_unique<Subscriber<MsgT>>(runtime_url, InitType::kWithoutInit);

    if (scenario.mode == Bench::kLocalLoopMode) {
      sub->attach(subscriber_loops.at(static_cast<size_t>(i)).get());
    }

    sub->set_latency_and_lost_enabled(true);

    if (!init_node_with_properties(*sub, scenario.qos_profile, scenario.properties, scenario.sub_properties, error)) {
      for (auto& worker_loop : subscriber_loops) {
        worker_loop->quit();
        worker_loop->wait_for_quit();
      }

      return false;
    }

    const int sub_sleep_us = scenario.subscriber_sleep_us;
    bool ok = sub->listen([&collector, sub_sleep_us](const MsgT& message) {
      try {
        uint64_t send_ns = 0;

        if (BenchCodec<MsgT>::extract(message, send_ns)) {
          collector.on_message(send_ns);
        }

        if (sub_sleep_us > 0) {
          std::this_thread::sleep_for(std::chrono::microseconds(sub_sleep_us));
        }
      } catch (std::exception&) {
      }
    });

    if VUNLIKELY (!ok) {
      error = "subscriber listen failed";

      for (auto& worker_loop : subscriber_loops) {
        worker_loop->quit();
        worker_loop->wait_for_quit();
      }

      return false;
    }

    subscribers.emplace_back(std::move(sub));
  }

  std::vector<std::unique_ptr<Publisher<MsgT>>> publishers;
  publishers.reserve(static_cast<size_t>(scenario.publishers));

  for (int i = 0; i < scenario.publishers; ++i) {
    auto pub = std::make_unique<Publisher<MsgT>>(runtime_url, InitType::kWithoutInit);

    if (!init_node_with_properties(*pub, scenario.qos_profile, scenario.properties, scenario.pub_properties, error)) {
      for (auto& worker_loop : subscriber_loops) {
        worker_loop->quit();
        worker_loop->wait_for_quit();
      }

      return false;
    }

    publishers.emplace_back(std::move(pub));
  }

  double discovery_ms = 0.0;

  for (const auto& publisher : publishers) {
    const uint64_t discovery_begin_ns = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano);

    if (!publisher->wait_for_subscribers(std::chrono::milliseconds(std::max(scenario.warmup_ms + 3000, 10000)))) {
      error = "wait_for_subscribers failed";

      for (auto& worker_loop : subscriber_loops) {
        worker_loop->quit();
        worker_loop->wait_for_quit();
      }

      return false;
    }

    discovery_ms = std::max(
        discovery_ms,
        static_cast<double>(ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano) - discovery_begin_ns) / 1000000.0);
  }

  result.discovery_ms = discovery_ms;

  ResourceSampler resource_sampler;
  bool sampler_started = false;
  std::atomic<uint64_t> measured_sent{0};
  std::atomic<uint64_t> measured_bytes{0};
  std::atomic_bool start_flag{false};
  std::vector<double> publisher_cpu_ms(static_cast<size_t>(scenario.publishers), 0.0);
  std::vector<std::vector<double>> publisher_send_block_us(static_cast<size_t>(scenario.publishers));
  std::vector<std::thread> publisher_threads;
  publisher_threads.reserve(static_cast<size_t>(scenario.publishers));
  const uint64_t publish_begin_ns = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano);
  const uint64_t publish_end_ns =
      publish_begin_ns + static_cast<uint64_t>(scenario.warmup_ms + scenario.duration_ms) * 1000000ULL;
  const uint64_t baseline_time_ns = publish_begin_ns + static_cast<uint64_t>(scenario.warmup_ms) * 1000000ULL;
  std::atomic<uint64_t> publish_measure_begin_ns{std::numeric_limits<uint64_t>::max()};
  std::atomic<uint64_t> publish_measure_end_ns{0};

  for (int i = 0; i < scenario.publishers; ++i) {
    publisher_threads.emplace_back([&, index = i]() {
      try {
        MsgT message_template = BenchCodec<MsgT>::create_template(scenario.payload_size);
        ElapsedTimer pub_cpu(ElapsedTimer::kCpuActiveTime, ElapsedTimer::kMicro);

        while (!start_flag.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }

        pub_cpu.start();
        uint64_t seq = 1;
        int burst_count = std::max(scenario.burst_messages, 1);

        if (scenario.rate_pattern == Bench::kBurstRatePattern) {
          burst_count = split_publisher_workload(std::max(scenario.burst_messages, 1), scenario.publishers, index);
        }

        int rate_hz = scenario.rate_hz;

        if (scenario.rate_pattern == Bench::kFixedRatePattern && scenario.rate_hz > 0) {
          rate_hz = split_publisher_workload(scenario.rate_hz, scenario.publishers, index);
        }

        if ((scenario.rate_pattern == Bench::kFixedRatePattern && rate_hz <= 0) ||
            (scenario.rate_pattern == Bench::kBurstRatePattern && burst_count <= 0)) {
          sleep_until_or_stop_unchecked(publish_end_ns);
          publisher_cpu_ms.at(static_cast<size_t>(index)) = static_cast<double>(pub_cpu.get()) / 1000.0;
          return;
        }

        uint64_t next_time_ns = publish_begin_ns;

        while (!Bench::stop_requested() && ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano) < publish_end_ns) {
          const uint64_t now_ns = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano);

          if (scenario.rate_pattern != Bench::kMaxRatePattern && rate_hz > 0 && now_ns < next_time_ns) {
            sleep_until_or_stop_unchecked(next_time_ns);
            continue;
          }

          int send_count = scenario.rate_pattern == Bench::kBurstRatePattern ? burst_count : 1;

          for (int burst_index = 0; burst_index < send_count; ++burst_index) {
            uint64_t send_ns = steady_time_ns();

            if (send_ns > publish_end_ns) {
              break;
            }

            MsgT message = message_template;
            BenchCodec<MsgT>::stamp(message, scenario.payload_size, seq++, static_cast<uint32_t>(index), send_ns);

            const uint64_t before_publish_ns = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano);
            const bool published = publishers.at(static_cast<size_t>(index))->publish(message);
            const uint64_t after_publish_ns = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano);
            const bool in_measure_window = send_ns >= publish_measure_begin_ns.load(std::memory_order_relaxed) &&
                                           send_ns <= publish_measure_end_ns.load(std::memory_order_relaxed);

            if (published && in_measure_window) {
              measured_sent.fetch_add(1, std::memory_order_relaxed);
              measured_bytes.fetch_add(static_cast<uint64_t>(collector.wire_size), std::memory_order_relaxed);
            }

            if (in_measure_window) {
              publisher_send_block_us.at(static_cast<size_t>(index))
                  .push_back(static_cast<double>(after_publish_ns - before_publish_ns) / 1000.0);
            }
          }

          if (scenario.rate_pattern != Bench::kMaxRatePattern && rate_hz > 0) {
            next_time_ns += static_cast<uint64_t>(1000000000LL / std::max(rate_hz, 1));
          }
        }

        publisher_cpu_ms.at(static_cast<size_t>(index)) = static_cast<double>(pub_cpu.get()) / 1000.0;
      } catch (std::exception&) {
      }
    });
  }

  std::vector<SampleLostInfo> baseline_losts(subscribers.size());

  if (scenario.warmup_ms <= 0) {
    for (size_t index = 0; index < subscribers.size(); ++index) {
      baseline_losts.at(index) = subscribers.at(index)->get_lost();
    }

    const uint64_t measure_begin_ns = steady_time_ns();
    collector.measure_begin_ns.store(measure_begin_ns, std::memory_order_relaxed);
    collector.measure_end_ns.store(
        measure_begin_ns + static_cast<uint64_t>(std::max(scenario.duration_ms, 1)) * 1000000ULL,
        std::memory_order_relaxed);
    publish_measure_begin_ns.store(measure_begin_ns, std::memory_order_relaxed);
    publish_measure_end_ns.store(collector.measure_end_ns.load(std::memory_order_relaxed), std::memory_order_relaxed);
    resource_sampler.start();
    sampler_started = true;
  }

  start_flag.store(true, std::memory_order_release);

  if (scenario.warmup_ms > 0) {
    sleep_until_or_stop_unchecked(baseline_time_ns);

    for (size_t index = 0; index < subscribers.size(); ++index) {
      baseline_losts.at(index) = subscribers.at(index)->get_lost();
    }

    const uint64_t measure_begin_ns = steady_time_ns();
    collector.measure_begin_ns.store(measure_begin_ns, std::memory_order_relaxed);
    collector.measure_end_ns.store(
        measure_begin_ns + static_cast<uint64_t>(std::max(scenario.duration_ms, 1)) * 1000000ULL,
        std::memory_order_relaxed);
    publish_measure_begin_ns.store(measure_begin_ns, std::memory_order_relaxed);
    publish_measure_end_ns.store(collector.measure_end_ns.load(std::memory_order_relaxed), std::memory_order_relaxed);
    resource_sampler.start();
    sampler_started = true;
  }

  for (auto& thread : publisher_threads) {
    thread.join();
  }

  if (sampler_started) {
    resource_sampler.stop(result.cpu_usage, result.memory_usage);
  }

  sleep_until_or_stop_unchecked(ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano) +
                                static_cast<uint64_t>(std::max(scenario.drain_ms, 0)) * 1000000ULL);

  for (auto& worker_loop : subscriber_loops) {
    worker_loop->wait_for_idle(std::max(scenario.drain_ms, 0));
  }

  result.transport = get_transport_from_url(scenario.url);
  result.wire_size = collector.wire_size;
  result.sent = measured_sent.load(std::memory_order_relaxed);
  result.received = collector.measured_received.load(std::memory_order_relaxed);
  result.expected = 0;
  result.lost = 0;
  result.pub_cpu_ms = std::accumulate(publisher_cpu_ms.begin(), publisher_cpu_ms.end(), 0.0);

  for (size_t index = 0; index < subscribers.size(); ++index) {
    const auto& sub = subscribers.at(index);
    auto lost = sub->get_lost();
    const auto& baseline = baseline_losts.at(index);
    result.expected += lost.total >= baseline.total ? static_cast<uint64_t>(lost.total - baseline.total) : 0ULL;
    result.lost += lost.lost >= baseline.lost ? static_cast<uint64_t>(lost.lost - baseline.lost) : 0ULL;
  }

  if (auto first_ns = collector.first_receive_ns.load(std::memory_order_relaxed); first_ns != 0) {
    result.first_message_ms = static_cast<double>(first_ns - collector.start_ns) / 1000000.0;
  }

  double duration_sec = static_cast<double>(std::max(scenario.duration_ms, 1)) / 1000.0;
  result.send_msgs_per_sec = static_cast<double>(result.sent) / duration_sec;
  result.recv_msgs_per_sec = static_cast<double>(result.received) / duration_sec;
  result.send_mb_per_sec =
      static_cast<double>(measured_bytes.load(std::memory_order_relaxed)) / (1024.0 * 1024.0) / duration_sec;
  result.recv_mb_per_sec =
      static_cast<double>(collector.measured_bytes.load(std::memory_order_relaxed)) / (1024.0 * 1024.0) / duration_sec;

  {
    std::lock_guard lock(collector.latency_mtx);
    auto stats = calculate_latency_stats(collector.latencies_us);
    result.avg_latency_us = stats.avg;
    result.p50_latency_us = stats.p50;
    result.p90_latency_us = stats.p90;
    result.p95_latency_us = stats.p95;
    result.p99_latency_us = stats.p99;
    result.p999_latency_us = stats.p999;
    result.p9999_latency_us = stats.p9999;
    result.max_latency_us = stats.max;
    result.latency_stddev_us = stats.stddev;
    result.latency_samples_dropped = collector.latency_samples_dropped.load(std::memory_order_relaxed);
  }

  {
    std::vector<double> all_send_block_us;
    size_t total_count = 0;
    for (const auto& v : publisher_send_block_us) {
      total_count += v.size();
    }

    all_send_block_us.reserve(total_count);
    for (auto& v : publisher_send_block_us) {
      all_send_block_us.insert(all_send_block_us.end(), v.begin(), v.end());
    }

    auto sb_stats = calculate_latency_stats(all_send_block_us);
    result.avg_send_block_us = sb_stats.avg;
    result.p50_send_block_us = sb_stats.p50;
    result.p95_send_block_us = sb_stats.p95;
    result.p99_send_block_us = sb_stats.p99;
    result.max_send_block_us = sb_stats.max;
    result.send_block_samples = static_cast<uint64_t>(all_send_block_us.size());
  }

  result.success = true;

  for (auto& worker_loop : subscriber_loops) {
    worker_loop->quit();
    worker_loop->wait_for_quit();
  }

  return true;
}

template <typename MsgT>
bool run_pub_worker_impl(const Bench::WorkerOptions& options, Bench::ScenarioResult& result,
                         std::vector<double>& send_block_samples_out, std::string& error) {
  Publisher<MsgT> publisher(options.url, InitType::kWithoutInit);

  if (!init_node_with_properties(publisher, options.qos_profile, options.properties, options.pub_properties, error)) {
    return false;
  }

  result.transport = get_transport_from_url(options.url);
  result.wire_size = BenchCodec<MsgT>::wire_size(options.payload_size);

  const uint64_t discovery_begin_ns = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano);

  if (!publisher.wait_for_subscribers(std::chrono::milliseconds(std::max(options.warmup_ms + 3000, 10000)))) {
    error = "wait_for_subscribers failed";
    return false;
  }

  result.discovery_ms =
      static_cast<double>(ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano) - discovery_begin_ns) / 1000000.0;

  uint64_t shared_start_ns = 0;

  if (options.wait_start && !wait_for_worker_start(shared_start_ns, error)) {
    return false;
  }

  ResourceSampler resource_sampler;
  bool sampler_started = false;

  uint64_t measured_sent = 0;
  uint64_t measured_bytes = 0;
  ElapsedTimer pub_cpu(ElapsedTimer::kCpuActiveTime, ElapsedTimer::kMicro);
  pub_cpu.start();
  MsgT message_template = BenchCodec<MsgT>::create_template(options.payload_size);

  const uint64_t publish_begin_ns =
      options.wait_start ? shared_start_ns : ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano);
  const uint64_t publish_end_ns =
      publish_begin_ns + static_cast<uint64_t>(options.warmup_ms + options.duration_ms) * 1000000ULL;
  const uint64_t measure_begin_ns = publish_begin_ns + static_cast<uint64_t>(options.warmup_ms) * 1000000ULL;
  const uint64_t measure_end_ns =
      measure_begin_ns + static_cast<uint64_t>(std::max(options.duration_ms, 1)) * 1000000ULL;

  if (options.wait_start && !sleep_until_or_stop(publish_begin_ns, error)) {
    return false;
  }

  if (options.warmup_ms <= 0) {
    resource_sampler.start();
    sampler_started = true;
  }

  uint64_t next_time_ns = publish_begin_ns;
  uint64_t seq = 1;
  std::vector<double> send_block_us;

  while (!Bench::stop_requested() && ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano) < publish_end_ns) {
    const uint64_t now_ns = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano);

    if (!sampler_started && now_ns >= measure_begin_ns) {
      resource_sampler.start();
      sampler_started = true;
    }

    if (options.rate_pattern != Bench::kMaxRatePattern && options.rate_hz > 0 && now_ns < next_time_ns) {
      if (!sleep_until_or_stop(next_time_ns, error)) {
        return false;
      }

      continue;
    }

    int burst_count = options.rate_pattern == Bench::kBurstRatePattern ? std::max(options.burst_messages, 1) : 1;

    for (int burst_index = 0; burst_index < burst_count; ++burst_index) {
      uint64_t send_ns = steady_time_ns();

      if (send_ns > publish_end_ns) {
        break;
      }

      MsgT message = message_template;
      BenchCodec<MsgT>::stamp(message, options.payload_size, seq, static_cast<uint32_t>(options.publisher_index),
                              send_ns);

      const uint64_t before_publish_ns = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano);
      const bool published = publisher.publish(message);
      const uint64_t after_publish_ns = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano);
      const bool in_measure_window = send_ns >= measure_begin_ns && send_ns <= measure_end_ns;

      if (published && in_measure_window) {
        ++measured_sent;
        measured_bytes += result.wire_size;
      }

      if (in_measure_window) {
        send_block_us.push_back(static_cast<double>(after_publish_ns - before_publish_ns) / 1000.0);
      }

      ++seq;
    }

    if (options.rate_pattern != Bench::kMaxRatePattern && options.rate_hz > 0) {
      next_time_ns += static_cast<uint64_t>(1000000000LL / std::max(options.rate_hz, 1));
    }
  }

  if VUNLIKELY (check_stop_requested(error)) {
    return false;
  }

  if (sampler_started) {
    resource_sampler.stop(result.cpu_usage, result.memory_usage);
  }

  result.sent = measured_sent;
  result.pub_cpu_ms = static_cast<double>(pub_cpu.get()) / 1000.0;
  double duration_sec = static_cast<double>(std::max(options.duration_ms, 1)) / 1000.0;
  result.send_msgs_per_sec = static_cast<double>(measured_sent) / duration_sec;
  result.send_mb_per_sec = static_cast<double>(measured_bytes) / (1024.0 * 1024.0) / duration_sec;

  {
    auto sb_stats = calculate_latency_stats(send_block_us);
    result.avg_send_block_us = sb_stats.avg;
    result.p50_send_block_us = sb_stats.p50;
    result.p95_send_block_us = sb_stats.p95;
    result.p99_send_block_us = sb_stats.p99;
    result.max_send_block_us = sb_stats.max;
    result.send_block_samples = static_cast<uint64_t>(send_block_us.size());
    send_block_samples_out = std::move(send_block_us);
  }

  result.success = true;
  return true;
}

template <typename MsgT>
bool run_sub_worker_impl(const Bench::WorkerOptions& options, Bench::ScenarioResult& result,
                         std::vector<double>& latency_samples, std::string& error) {
  Collector collector;
  collector.start_ns = 0;
  collector.measure_begin_ns.store(std::numeric_limits<uint64_t>::max(), std::memory_order_relaxed);
  collector.measure_end_ns.store(std::numeric_limits<uint64_t>::max(), std::memory_order_relaxed);
  collector.wire_size = BenchCodec<MsgT>::wire_size(options.payload_size);
  collector.enable_latency = options.enable_latency;

  Subscriber<MsgT> subscriber(options.url, InitType::kWithoutInit);
  subscriber.set_latency_and_lost_enabled(true);

  if (!init_node_with_properties(subscriber, options.qos_profile, options.properties, options.sub_properties, error)) {
    return false;
  }

  const int sub_sleep_us = options.subscriber_sleep_us;

  if (!subscriber.listen([&collector, sub_sleep_us](const MsgT& message) {
        uint64_t send_ns = 0;

        if (BenchCodec<MsgT>::extract(message, send_ns)) {
          collector.on_message(send_ns);
        }

        if (sub_sleep_us > 0) {
          std::this_thread::sleep_for(std::chrono::microseconds(sub_sleep_us));
        }
      })) {
    error = "subscriber listen failed";
    return false;
  }

  SampleLostInfo baseline_lost{};
  std::cout << "READY" << std::endl;
  std::cout.flush();
  std::fflush(stdout);

  uint64_t shared_start_ns = 0;

  if (options.wait_start && !wait_for_worker_start(shared_start_ns, error)) {
    return false;
  }

  const uint64_t subscribe_begin_ns =
      options.wait_start ? shared_start_ns : ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano);
  const uint64_t measure_begin_ns = subscribe_begin_ns + static_cast<uint64_t>(options.warmup_ms) * 1000000ULL;
  const uint64_t measure_end_ns =
      measure_begin_ns + static_cast<uint64_t>(std::max(options.duration_ms, 1)) * 1000000ULL;

  collector.start_ns = subscribe_begin_ns;

  if (options.warmup_ms <= 0) {
    baseline_lost = subscriber.get_lost();
    collector.measure_begin_ns.store(measure_begin_ns, std::memory_order_relaxed);
    collector.measure_end_ns.store(measure_end_ns, std::memory_order_relaxed);
  }

  ResourceSampler resource_sampler;
  bool sampler_started = false;
  ElapsedTimer sub_cpu(ElapsedTimer::kCpuActiveTime, ElapsedTimer::kMicro);
  sub_cpu.start();

  if (options.wait_start) {
    if (!sleep_until_or_stop(subscribe_begin_ns, error)) {
      return false;
    }
  }

  if (options.warmup_ms <= 0) {
    resource_sampler.start();
    sampler_started = true;
  }

  if (options.warmup_ms > 0) {
    if (!sleep_until_or_stop(measure_begin_ns, error)) {
      return false;
    }

    baseline_lost = subscriber.get_lost();
    collector.measure_begin_ns.store(measure_begin_ns, std::memory_order_relaxed);
    collector.measure_end_ns.store(measure_end_ns, std::memory_order_relaxed);
    resource_sampler.start();
    sampler_started = true;
  }

  if (!sleep_for_or_stop(std::chrono::milliseconds(std::max(options.duration_ms, 1)), error)) {
    return false;
  }

  if (sampler_started) {
    resource_sampler.stop(result.cpu_usage, result.memory_usage);
  }

  if (!sleep_for_or_stop(std::chrono::milliseconds(std::max(options.drain_ms, 0)), error)) {
    return false;
  }

  result.transport = get_transport_from_url(options.url);
  result.wire_size = collector.wire_size;
  result.received = collector.measured_received.load(std::memory_order_relaxed);
  result.sub_cpu_ms = static_cast<double>(sub_cpu.get()) / 1000.0;

  auto lost = subscriber.get_lost();
  result.expected = lost.total >= baseline_lost.total ? static_cast<uint64_t>(lost.total - baseline_lost.total) : 0ULL;
  result.lost = lost.lost >= baseline_lost.lost ? static_cast<uint64_t>(lost.lost - baseline_lost.lost) : 0ULL;

  if (auto first_ns = collector.first_receive_ns.load(std::memory_order_relaxed); first_ns != 0) {
    result.first_message_ms = static_cast<double>(first_ns - collector.start_ns) / 1000000.0;
  }

  double duration_sec = static_cast<double>(std::max(options.duration_ms, 1)) / 1000.0;
  result.recv_msgs_per_sec = static_cast<double>(result.received) / duration_sec;
  result.recv_mb_per_sec =
      static_cast<double>(collector.measured_bytes.load(std::memory_order_relaxed)) / (1024.0 * 1024.0) / duration_sec;

  {
    std::lock_guard lock(collector.latency_mtx);
    latency_samples = collector.latencies_us;
    auto stats = calculate_latency_stats(collector.latencies_us);
    result.avg_latency_us = stats.avg;
    result.p50_latency_us = stats.p50;
    result.p90_latency_us = stats.p90;
    result.p95_latency_us = stats.p95;
    result.p99_latency_us = stats.p99;
    result.p999_latency_us = stats.p999;
    result.p9999_latency_us = stats.p9999;
    result.max_latency_us = stats.max;
    result.latency_stddev_us = stats.stddev;
    result.latency_samples_dropped = collector.latency_samples_dropped.load(std::memory_order_relaxed);
  }

  result.success = true;
  return true;
}

bool save_worker_result(const Bench::ScenarioResult& result, const std::vector<double>& latency_samples,
                        const std::vector<double>& send_block_samples, const std::string& file_path,
                        std::string& error) {
  if (!ensure_parent_dir(file_path, error)) {
    return false;
  }

  std::ofstream stream(file_path, std::ios::out | std::ios::trunc);

  if (!stream.is_open()) {
    error = "open result file failed";
    return false;
  }

  json obj;
  result_to_json(obj, result);

  if (!latency_samples.empty()) {
    obj["latency_samples_us"] = latency_samples;
  }

  if (!send_block_samples.empty()) {
    obj["send_block_samples_us"] = send_block_samples;
  }

  stream << obj.dump(2);
  return true;
}

bool load_worker_result(const std::string& file_path, Bench::ScenarioResult& result,
                        std::vector<double>& latency_samples, std::vector<double>& send_block_samples,
                        std::string& error) {
  std::ifstream stream(file_path);

  if (!stream.is_open()) {
    error = "open result file failed";
    return false;
  }

  json obj;

  try {
    stream >> obj;
  } catch (const json::exception& e) {
    error = e.what();
    return false;
  }

  latency_samples.clear();
  send_block_samples.clear();

  if (!result_from_json(obj, result, error)) {
    return false;
  }

  if (obj.contains("send_block_samples_us")) {
    try {
      send_block_samples = obj.at("send_block_samples_us").get<std::vector<double>>();
    } catch (const json::exception& e) {
      error = e.what();
      return false;
    }
  }

  if (obj.contains("latency_samples_us")) {
    try {
      latency_samples = obj.at("latency_samples_us").get<std::vector<double>>();
    } catch (const json::exception& e) {
      error = e.what();
      return false;
    }
  }

  return true;
}

void append_worker_result_error(const std::string& file_path, std::string& error) {
  if (file_path.empty()) {
    return;
  }

  std::error_code exists_ec;

  if (!std::filesystem::exists(file_path, exists_ec) || exists_ec) {
    return;
  }

  Bench::ScenarioResult worker_result;
  std::vector<double> latency_samples;
  std::vector<double> send_block_samples;
  std::string load_error;

  if (!load_worker_result(file_path, worker_result, latency_samples, send_block_samples, load_error) ||
      worker_result.error.empty()) {
    return;
  }

  const std::string worker_error = worker_result.error;

  if (worker_error.empty() || error.find(worker_error) != std::string::npos) {
    return;
  }

  if (!error.empty()) {
    error += ": ";
  }

  error += worker_error;
}

bool wait_process_ready(Process& process, uint64_t deadline_ns, std::string& error) {
  while (ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano) < deadline_ns) {
    if VUNLIKELY (check_stop_requested(error)) {
      return false;
    }

    std::string line;

    while (process.read_line_stdout(line)) {
      if (Helpers::trim_string(line) == "READY") {
        return true;
      }
    }

    if (!process.is_running()) {
      error = format_process_failure(process, "process exited before ready");
      return false;
    }

    const uint64_t now_ns = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano);
    const int64_t remaining_ms = now_ns >= deadline_ns ? 0 : static_cast<int64_t>((deadline_ns - now_ns) / 1000000ULL);

    if (remaining_ms <= 0) {
      break;
    }

    process.wait_for_ready_read(static_cast<int>(std::min<int64_t>(remaining_ms, 100)));
  }

  std::string line;

  while (process.read_line_stdout(line)) {
    if (Helpers::trim_string(line) == "READY") {
      return true;
    }
  }

  if (!process.is_running()) {
    error = format_process_failure(process, "process exited before ready");
    return false;
  }

  error = format_process_failure(process, "wait process ready timeout");
  return false;
}

bool verify_process_exit(Process& process, std::string& error) {
  if (process.get_exit_status() == Process::kCrashExitStatus) {
    std::string stderr_text;
    process.read_all_error(stderr_text);
    error = "worker process crashed (exit code " + std::to_string(process.get_exit_code()) + ")";

    if (!stderr_text.empty()) {
      error += ": " + stderr_text;
    }

    return false;
  }

  const int code = process.get_exit_code();

  if (code != 0) {
    std::string stderr_text;
    process.read_all_error(stderr_text);
    error = "worker process exited with code " + std::to_string(code);

    if (!stderr_text.empty()) {
      error += ": " + stderr_text;
    }

    return false;
  }

  return true;
}

void graceful_shutdown_process(Process& process) {
  constexpr int kGracefulShutdownMs = 1500;

  if VUNLIKELY (!process.is_running()) {
    return;
  }

  process.terminate();

  if (process.wait_for_finished(kGracefulShutdownMs)) {
    return;
  }

  if VUNLIKELY (process.is_running()) {
    process.kill();
    process.wait_for_finished(kProcessGraceTimeoutMs);
  }
}

bool wait_process_finished(Process& process, uint64_t deadline_ns, std::string& error) {
  while (ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano) < deadline_ns) {
    if VUNLIKELY (check_stop_requested(error)) {
      graceful_shutdown_process(process);
      return false;
    }

    const uint64_t now_ns = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano);
    const int64_t remaining_ms = now_ns >= deadline_ns ? 0 : static_cast<int64_t>((deadline_ns - now_ns) / 1000000ULL);

    if (remaining_ms <= 0) {
      break;
    }

    if (process.wait_for_finished(static_cast<int>(std::min<int64_t>(remaining_ms, 100)))) {
      return verify_process_exit(process, error);
    }
  }

  if (process.wait_for_finished(0)) {
    return verify_process_exit(process, error);
  }

  graceful_shutdown_process(process);
  std::string stderr_text;
  process.read_all_error(stderr_text);
  error = stderr_text.empty() ? "process wait_for_finished timeout" : stderr_text;
  return false;
}

void stop_processes(std::vector<std::unique_ptr<Process>>& processes) {
  for (auto& process : processes) {
    if VLIKELY (process && process->is_running()) {
      process->terminate();
    }
  }

  constexpr int kGracefulShutdownMs = 1500;

  for (auto& process : processes) {
    if VLIKELY (process && process->is_running()) {
      process->wait_for_finished(kGracefulShutdownMs);
    }
  }

  for (auto& process : processes) {
    if VUNLIKELY (process && process->is_running()) {
      process->kill();
      process->wait_for_finished(kProcessGraceTimeoutMs);
    }
  }
}

void append_property_args(std::vector<std::string>& args, const char* key, const std::vector<std::string>& properties) {
  for (const auto& property : properties) {
    args.emplace_back(key);
    args.emplace_back(property);
  }
}

int split_publisher_workload(int total, int publishers, int publisher_index) noexcept {
  if (total <= 0) {
    return 0;
  }

  const int safe_publishers = std::max(publishers, 1);

  if (safe_publishers == 1) {
    return total;
  }

  const int base = total / safe_publishers;
  const int remain = total % safe_publishers;
  return base + (publisher_index < remain ? 1 : 0);
}

void append_worker_args(std::vector<std::string>& args, const Bench::Scenario& scenario, bool is_pub,
                        const std::string& result_path, int publisher_index) {
  int worker_rate_hz = scenario.rate_hz;
  int worker_burst_messages = std::max(scenario.burst_messages, 1);

  if (is_pub && scenario.publishers > 1) {
    if (scenario.rate_pattern == Bench::kFixedRatePattern && scenario.rate_hz > 0) {
      worker_rate_hz = split_publisher_workload(scenario.rate_hz, scenario.publishers, publisher_index);
    }

    if (scenario.rate_pattern == Bench::kBurstRatePattern) {
      worker_burst_messages =
          split_publisher_workload(std::max(scenario.burst_messages, 1), scenario.publishers, publisher_index);
    }
  }

  args.emplace_back(is_pub ? "pub" : "sub");
  args.emplace_back(scenario.url);
  args.emplace_back("-k");
  args.emplace_back(Bench::payload_to_string(scenario.payload));

  if (!scenario.qos_profile.empty()) {
    args.emplace_back("-q");
    args.emplace_back(scenario.qos_profile);
  }

  args.emplace_back("--pattern");
  args.emplace_back(Bench::rate_pattern_to_string(scenario.rate_pattern));
  args.emplace_back("--size");
  args.emplace_back(std::to_string(scenario.payload_size));
  args.emplace_back("-r");
  args.emplace_back(std::to_string(worker_rate_hz));
  args.emplace_back("--burst");
  args.emplace_back(std::to_string(worker_burst_messages));
  args.emplace_back("--warmup");
  args.emplace_back(std::to_string(scenario.warmup_ms));
  args.emplace_back("--duration");
  args.emplace_back(std::to_string(scenario.duration_ms));
  args.emplace_back("--drain");
  args.emplace_back(std::to_string(scenario.drain_ms));
  args.emplace_back("-o");
  args.emplace_back(result_path);
  append_property_args(args, "--property", scenario.properties);
  append_property_args(args, "--pub-property", scenario.pub_properties);
  append_property_args(args, "--sub-property", scenario.sub_properties);

  if (is_pub) {
    args.emplace_back("--publisher-id");
    args.emplace_back(std::to_string(publisher_index));
  } else if (scenario.suite == Bench::kLatencySuite) {
    args.emplace_back("--latency");
  }

  if (!is_pub && scenario.subscriber_sleep_us > 0) {
    args.emplace_back("--subscriber-sleep-us");
    args.emplace_back(std::to_string(scenario.subscriber_sleep_us));
  }

  args.emplace_back("--wait-start");
}

bool start_worker_process(const Bench::RunOptions& options, const std::vector<std::string>& args,
                          const std::string& worker_label, std::unique_ptr<Process>& process, std::string& error) {
  constexpr int kMaxStartAttempts = 3;
  std::string last_error;

  for (int attempt = 1; attempt <= kMaxStartAttempts; ++attempt) {
    auto candidate = std::make_unique<Process>();
    candidate->set_process_mode(Process::kSeparateMode);
    candidate->set_inherit_environment(true);
    candidate->start(options.executable_path, args);

    if VLIKELY (candidate->wait_for_started(kProcessStartTimeoutMs)) {
      process = std::move(candidate);
      return true;
    }

    last_error = format_process_failure(*candidate, worker_label + " worker start failed");

    if (candidate->get_exit_code() >= 0) {
      error = last_error;
      return false;
    }

    if (attempt < kMaxStartAttempts) {
      if VUNLIKELY (!sleep_for_or_stop(std::chrono::milliseconds(100 * attempt), error)) {
        return false;
      }
    }
  }

  error = last_error;
  return false;
}

bool run_process_pubsub_case(const Bench::RunOptions& options, const Bench::Scenario& scenario,
                             Bench::ScenarioResult& result, std::string& error) {
  if VUNLIKELY (check_stop_requested(error)) {
    return false;
  }

  const auto runtime_url = make_runtime_url(scenario);

  if (get_transport_from_url(runtime_url) == "intra") {
    error = "intra transport only supports local mode";
    return false;
  }

  std::error_code ec;
  std::filesystem::path tmp_root = std::filesystem::temp_directory_path(ec);

  if (ec) {
    error = "temp_directory_path failed: " + ec.message();
    return false;
  }

  std::filesystem::path temp_dir = tmp_root / ("vlink-bench-" + std::to_string(steady_time_ns()));
  std::filesystem::create_directories(temp_dir, ec);

  if (ec) {
    error = ec.message();
    return false;
  }

#ifndef _WIN32
  std::filesystem::permissions(temp_dir,
                               std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                                   std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
                                   std::filesystem::perms::others_exec,
                               std::filesystem::perm_options::replace, ec);
#endif

  auto cleanup = [&temp_dir]() {
    std::error_code remove_ec;
    std::filesystem::remove_all(temp_dir, remove_ec);
  };

  std::vector<std::unique_ptr<Process>> sub_processes;
  std::vector<std::unique_ptr<Process>> pub_processes;
  std::vector<std::string> sub_result_paths;
  std::vector<std::string> pub_result_paths;
  sub_processes.reserve(static_cast<size_t>(scenario.subscribers));
  pub_processes.reserve(static_cast<size_t>(scenario.publishers));
  sub_result_paths.reserve(static_cast<size_t>(scenario.subscribers));
  pub_result_paths.reserve(static_cast<size_t>(scenario.publishers));

  const std::string transport_scheme = get_transport_from_url(runtime_url);
  const bool transport_needs_stagger = transport_scheme == "dds" || transport_scheme == "ddsc" ||
                                       transport_scheme == "ddsr" || transport_scheme == "ddst" ||
                                       transport_scheme == "someip" || transport_scheme == "zenoh" ||
                                       transport_scheme == "mqtt";
  const int sub_spawn_stagger_ms = transport_needs_stagger && scenario.subscribers > 4 ? 120 : 0;

  for (int index = 0; index < scenario.subscribers; ++index) {
    if VUNLIKELY (check_stop_requested(error)) {
      stop_processes(sub_processes);
      cleanup();
      return false;
    }

    if (index > 0 && sub_spawn_stagger_ms > 0) {
      if VUNLIKELY (!sleep_for_or_stop(std::chrono::milliseconds(sub_spawn_stagger_ms), error)) {
        stop_processes(sub_processes);
        cleanup();
        return false;
      }
    }

    std::vector<std::string> args;
    sub_result_paths.emplace_back((temp_dir / ("sub-" + std::to_string(index) + ".json")).string());
    append_worker_args(args, scenario, false, sub_result_paths.back(), index);

    std::unique_ptr<Process> process;

    if VUNLIKELY (!start_worker_process(options, args, "sub", process, error)) {
      append_worker_result_error(sub_result_paths.back(), error);
      stop_processes(sub_processes);
      cleanup();
      return false;
    }

    sub_processes.emplace_back(std::move(process));
  }

  for (size_t index = 0; index < sub_processes.size(); ++index) {
    auto& process = sub_processes[index];
    const uint64_t ready_deadline_ns = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano) +
                                       static_cast<uint64_t>(kProcessReadyTimeoutMs) * 1000000ULL;

    if VUNLIKELY (!wait_process_ready(*process, ready_deadline_ns, error)) {
      append_worker_result_error(sub_result_paths[index], error);
      stop_processes(sub_processes);
      cleanup();
      return false;
    }
  }

  for (int index = 0; index < scenario.publishers; ++index) {
    if VUNLIKELY (check_stop_requested(error)) {
      stop_processes(pub_processes);
      stop_processes(sub_processes);
      cleanup();
      return false;
    }

    std::vector<std::string> args;
    pub_result_paths.emplace_back((temp_dir / ("pub-" + std::to_string(index) + ".json")).string());
    append_worker_args(args, scenario, true, pub_result_paths.back(), index);

    std::unique_ptr<Process> process;

    if VUNLIKELY (!start_worker_process(options, args, "pub", process, error)) {
      append_worker_result_error(pub_result_paths.back(), error);
      stop_processes(pub_processes);
      stop_processes(sub_processes);
      cleanup();
      return false;
    }

    pub_processes.emplace_back(std::move(process));
  }

  const uint64_t shared_start_ns = steady_time_ns() + 300000000ULL;
  const std::string start_signal = "GO " + std::to_string(shared_start_ns) + "\n";

  for (size_t index = 0; index < sub_processes.size(); ++index) {
    auto& process = sub_processes[index];

    if (process->write(start_signal) != start_signal.size()) {
      error = format_process_failure(*process, "sub worker start signal failed");
      append_worker_result_error(sub_result_paths[index], error);
      stop_processes(pub_processes);
      stop_processes(sub_processes);
      cleanup();
      return false;
    }
  }

  for (size_t index = 0; index < pub_processes.size(); ++index) {
    auto& process = pub_processes[index];

    if (process->write(start_signal) != start_signal.size()) {
      error = format_process_failure(*process, "pub worker start signal failed");
      append_worker_result_error(pub_result_paths[index], error);
      stop_processes(pub_processes);
      stop_processes(sub_processes);
      cleanup();
      return false;
    }
  }

  const uint64_t finish_deadline_ns =
      ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano) +
      static_cast<uint64_t>(scenario.warmup_ms + scenario.duration_ms + scenario.drain_ms + kProcessMeasureBufferMs) *
          1000000ULL;

  for (size_t index = 0; index < pub_processes.size(); ++index) {
    auto& process = pub_processes[index];

    if (!wait_process_finished(*process, finish_deadline_ns, error)) {
      append_worker_result_error(pub_result_paths[index], error);
      stop_processes(pub_processes);
      stop_processes(sub_processes);
      cleanup();
      return false;
    }
  }

  for (size_t index = 0; index < sub_processes.size(); ++index) {
    auto& process = sub_processes[index];

    if (!wait_process_finished(*process, finish_deadline_ns, error)) {
      append_worker_result_error(sub_result_paths[index], error);
      stop_processes(sub_processes);
      cleanup();
      return false;
    }
  }

  std::vector<Bench::ScenarioResult> pub_results;
  std::vector<Bench::ScenarioResult> sub_results;
  std::vector<double> all_latency_samples;
  std::vector<double> all_send_block_samples;
  pub_results.reserve(pub_result_paths.size());
  sub_results.reserve(sub_result_paths.size());

  for (const auto& path : pub_result_paths) {
    Bench::ScenarioResult worker_result;
    std::vector<double> latency_samples;
    std::vector<double> send_block_samples;

    if (!load_worker_result(path, worker_result, latency_samples, send_block_samples, error)) {
      cleanup();
      return false;
    }

    if (!send_block_samples.empty()) {
      all_send_block_samples.insert(all_send_block_samples.end(), send_block_samples.begin(), send_block_samples.end());
    }

    pub_results.emplace_back(std::move(worker_result));
  }

  for (const auto& path : sub_result_paths) {
    Bench::ScenarioResult worker_result;
    std::vector<double> latency_samples;
    std::vector<double> send_block_samples;

    if (!load_worker_result(path, worker_result, latency_samples, send_block_samples, error)) {
      cleanup();
      return false;
    }

    if (!latency_samples.empty()) {
      all_latency_samples.insert(all_latency_samples.end(), latency_samples.begin(), latency_samples.end());
    }

    sub_results.emplace_back(std::move(worker_result));
  }

  result.transport = get_transport_from_url(scenario.url);
  result.wire_size = 0;
  result.discovery_ms = 0.0;
  result.first_message_ms = 0.0;
  result.success = true;

  auto append_error = [&result](const std::string& worker_error) {
    if (worker_error.empty()) {
      return;
    }

    if (!result.error.empty()) {
      result.error += " | ";
    }

    result.error += worker_error;
  };

  for (const auto& worker : pub_results) {
    result.wire_size = result.wire_size == 0 ? worker.wire_size : result.wire_size;
    result.sent += worker.sent;
    result.discovery_ms = std::max(result.discovery_ms, worker.discovery_ms);
    result.send_msgs_per_sec += worker.send_msgs_per_sec;
    result.send_mb_per_sec += worker.send_mb_per_sec;
    result.pub_cpu_ms += worker.pub_cpu_ms;
    result.cpu_usage += worker.cpu_usage;
    result.memory_usage += worker.memory_usage;
    result.success = result.success && worker.success;
    append_error(worker.error);
  }

  for (const auto& worker : sub_results) {
    result.wire_size = result.wire_size == 0 ? worker.wire_size : result.wire_size;
    result.received += worker.received;
    result.expected += worker.expected;
    result.lost += worker.lost;

    if (worker.first_message_ms > 0.0) {
      result.first_message_ms = result.first_message_ms <= 0.0
                                    ? worker.first_message_ms
                                    : std::min(result.first_message_ms, worker.first_message_ms);
    }

    result.recv_msgs_per_sec += worker.recv_msgs_per_sec;
    result.recv_mb_per_sec += worker.recv_mb_per_sec;
    result.sub_cpu_ms += worker.sub_cpu_ms;
    result.cpu_usage += worker.cpu_usage;
    result.memory_usage += worker.memory_usage;
    result.latency_samples_dropped += worker.latency_samples_dropped;
    result.success = result.success && worker.success;
    append_error(worker.error);
  }

  if (!all_latency_samples.empty()) {
    auto stats = calculate_latency_stats(std::move(all_latency_samples));
    result.avg_latency_us = stats.avg;
    result.p50_latency_us = stats.p50;
    result.p90_latency_us = stats.p90;
    result.p95_latency_us = stats.p95;
    result.p99_latency_us = stats.p99;
    result.p999_latency_us = stats.p999;
    result.p9999_latency_us = stats.p9999;
    result.max_latency_us = stats.max;
    result.latency_stddev_us = stats.stddev;
  } else if (!sub_results.empty()) {
    result.avg_latency_us = 0.0;
    result.p50_latency_us = 0.0;
    result.p90_latency_us = 0.0;
    result.p95_latency_us = 0.0;
    result.p99_latency_us = 0.0;
    result.p999_latency_us = 0.0;
    result.p9999_latency_us = 0.0;
    result.max_latency_us = 0.0;
    result.latency_stddev_us = 0.0;
  }

  if (!all_send_block_samples.empty()) {
    const size_t sample_count = all_send_block_samples.size();
    auto stats = calculate_latency_stats(std::move(all_send_block_samples));
    result.avg_send_block_us = stats.avg;
    result.p50_send_block_us = stats.p50;
    result.p95_send_block_us = stats.p95;
    result.p99_send_block_us = stats.p99;
    result.max_send_block_us = stats.max;
    result.send_block_samples = static_cast<uint64_t>(sample_count);
  }

  cleanup();

  if (!result.success && result.error.empty()) {
    result.error = "process benchmark failed";
  }

  return result.success;
}

bool run_scenario(const Bench::RunOptions& options, const Bench::Scenario& scenario, Bench::ScenarioResult& result,
                  std::string& error) {
  result.scenario = scenario;
  result.transport = get_transport_from_url(scenario.url);

  if (scenario.suite == Bench::kSerializationSuite) {
    switch (scenario.payload) {
      case Bench::kBytesPayload:
        return run_serialization_case<Bytes>(scenario, result, error);
      case Bench::kStringPayload:
        return run_serialization_case<std::string>(scenario, result, error);
      case Bench::kRawDataPayload:
        return run_serialization_case<zerocopy::RawData>(scenario, result, error);
      default:
        error = "invalid payload kind";
        return false;
    }
  }

  if (scenario.mode == Bench::kProcessMode) {
    return run_process_pubsub_case(options, scenario, result, error);
  }

  switch (scenario.payload) {
    case Bench::kBytesPayload:
      return run_local_pubsub_case<Bytes>(scenario, result, error);
    case Bench::kStringPayload:
      return run_local_pubsub_case<std::string>(scenario, result, error);
    case Bench::kRawDataPayload:
      return run_local_pubsub_case<zerocopy::RawData>(scenario, result, error);
    default:
      error = "invalid payload kind";
      return false;
  }
}

std::vector<Bench::Scenario> expand_scenarios(const Bench::RunOptions& options) {
  std::vector<Bench::Scenario> scenarios;
  const auto& urls = options.urls;
  const auto& payload_sizes = options.payload_sizes;
  const auto& latency_sizes = options.latency_sizes;
  const auto& topology_sizes = options.topology_sizes.empty() ? options.latency_sizes : options.topology_sizes;
  const auto& latency_rates = options.latency_rates;
  const auto& fanout_subscribers = options.fanout_subscribers;
  const auto& publisher_counts = options.publisher_counts;
  const auto& burst_messages = options.burst_messages;
  std::vector<std::string> qos_profiles =
      options.qos_profiles.empty() ? std::vector<std::string>{std::string()} : options.qos_profiles;
  std::vector<Bench::Mode> modes =
      options.modes.empty() ? std::vector<Bench::Mode>{Bench::kLocalDirectMode, Bench::kLocalLoopMode} : options.modes;
  std::vector<Bench::Topology> topologies =
      options.topologies.empty() ? std::vector<Bench::Topology>{Bench::kOneToOneTopology, Bench::kOneToManyTopology,
                                                                Bench::kManyToOneTopology, Bench::kManyToManyTopology}
                                 : options.topologies;
  std::vector<Bench::RatePattern> rate_patterns =
      options.rate_patterns.empty()
          ? std::vector<Bench::RatePattern>{Bench::kMaxRatePattern, Bench::kFixedRatePattern, Bench::kBurstRatePattern}
          : options.rate_patterns;
  const bool enable_max_rate =
      std::find(rate_patterns.begin(), rate_patterns.end(), Bench::kMaxRatePattern) != rate_patterns.end();
  const bool enable_fixed_rate =
      std::find(rate_patterns.begin(), rate_patterns.end(), Bench::kFixedRatePattern) != rate_patterns.end();
  const bool enable_burst_rate =
      std::find(rate_patterns.begin(), rate_patterns.end(), Bench::kBurstRatePattern) != rate_patterns.end();

  auto append_unique_value = [](auto& values, const auto& value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
      values.emplace_back(value);
    }
  };

  auto pick_focus_sizes = [&append_unique_value](const std::vector<size_t>& source,
                                                 std::initializer_list<size_t> thresholds, bool include_last) {
    std::vector<size_t> focus;

    if (source.empty()) {
      return focus;
    }

    append_unique_value(focus, source.front());

    for (auto threshold : thresholds) {
      auto it = std::find_if(source.begin(), source.end(), [threshold](size_t value) { return value >= threshold; });

      if (it != source.end()) {
        append_unique_value(focus, *it);
      }
    }

    if (include_last) {
      append_unique_value(focus, source.back());
    }

    return focus;
  };

  auto pick_tail_values = [](const auto& source, size_t keep) {
    using ValueT = typename std::decay_t<decltype(source)>::value_type;
    std::vector<ValueT> values;

    if (source.empty() || keep == 0) {
      return values;
    }

    const size_t begin = source.size() > keep ? source.size() - keep : 0;
    values.insert(values.end(), source.begin() + static_cast<ptrdiff_t>(begin), source.end());
    return values;
  };

  const auto throughput_sweep_sizes =
      options.payload_sizes_explicit ? payload_sizes : pick_focus_sizes(payload_sizes, {64, 1024, 65536}, true);
  const auto throughput_burst_sizes =
      options.payload_sizes_explicit ? payload_sizes : pick_focus_sizes(payload_sizes, {64, 1024}, false);
  const auto latency_pressure_sizes =
      options.latency_sizes_explicit ? latency_sizes : pick_focus_sizes(latency_sizes, {64, 1024, 16384}, true);
  const auto latency_burst_sizes =
      options.latency_sizes_explicit ? latency_sizes : pick_focus_sizes(latency_sizes, {64, 1024}, false);
  const auto topology_scale_sizes =
      options.topology_sizes_explicit ? topology_sizes : pick_focus_sizes(topology_sizes, {1024}, true);
  const auto topology_pressure_sizes =
      options.topology_sizes_explicit ? topology_sizes : pick_focus_sizes(topology_sizes, {1024}, false);
  const auto burst_rates = options.latency_rates_explicit
                               ? latency_rates
                               : pick_tail_values(latency_rates, std::min<size_t>(latency_rates.size(), 2));
  const auto topology_pressure_rates =
      options.latency_rates_explicit ? latency_rates : pick_tail_values(latency_rates, 1);
  const int latency_floor_rate = latency_rates.empty() ? 0 : latency_rates.front();
  const auto& effective_burst_messages = options.burst_messages_explicit ? options.burst_messages : burst_messages;
  const std::vector<Bench::PayloadKind> transport_payloads =
      !options.payloads.empty()
          ? options.payloads
          : std::vector<Bench::PayloadKind>{Bench::kBytesPayload, Bench::kStringPayload, Bench::kRawDataPayload};
  const std::vector<Bench::PayloadKind> topology_payloads =
      !options.payloads.empty() ? options.payloads
                                : std::vector<Bench::PayloadKind>{Bench::kBytesPayload, Bench::kRawDataPayload};
  const std::vector<Bench::PayloadKind> serialization_payloads =
      options.payloads_explicit
          ? options.payloads
          : std::vector<Bench::PayloadKind>{Bench::kBytesPayload, Bench::kStringPayload, Bench::kRawDataPayload};

  auto make_base = [&options](Bench::Scenario& scenario, Bench::Suite suite, Bench::Mode mode,
                              Bench::PayloadKind payload, const std::string& url, const std::string& qos_profile,
                              int repeat) {
    scenario.suite = suite;
    scenario.mode = mode;
    scenario.payload = payload;
    scenario.url = url;
    scenario.qos_profile = qos_profile;
    scenario.properties = options.properties;
    scenario.pub_properties = options.pub_properties;
    scenario.sub_properties = options.sub_properties;
    scenario.warmup_ms = options.warmup_ms;
    scenario.duration_ms = options.duration_ms;
    scenario.drain_ms = options.drain_ms;
    scenario.repeat_index = repeat;
  };

  auto append_throughput_case = [&scenarios, &make_base](
                                    Bench::Suite suite, Bench::Mode mode, Bench::PayloadKind payload,
                                    const std::string& url, const std::string& qos_profile, size_t payload_size,
                                    Bench::RatePattern rate_pattern, int rate_hz, int burst_count, int repeat) {
    Bench::Scenario scenario;
    make_base(scenario, suite, mode, payload, url, qos_profile, repeat);
    scenario.topology = Bench::kOneToOneTopology;
    scenario.rate_pattern = rate_pattern;
    scenario.payload_size = payload_size;
    scenario.rate_hz = rate_hz;
    scenario.burst_messages = burst_count;
    scenarios.emplace_back(std::move(scenario));
  };

  auto append_topology_case = [&scenarios, &make_base](Bench::Suite suite, Bench::Mode mode, Bench::PayloadKind payload,
                                                       const std::string& url, const std::string& qos_profile,
                                                       size_t payload_size, Bench::Topology topology,
                                                       Bench::RatePattern rate_pattern, int rate_hz, int burst_count,
                                                       int publishers, int subscribers, int repeat) {
    Bench::Scenario scenario;
    make_base(scenario, suite, mode, payload, url, qos_profile, repeat);
    scenario.topology = topology;
    scenario.rate_pattern = rate_pattern;
    scenario.payload_size = payload_size;
    scenario.rate_hz = rate_hz;
    scenario.burst_messages = burst_count;
    scenario.publishers = publishers;
    scenario.subscribers = subscribers;
    scenarios.emplace_back(std::move(scenario));
  };

  const std::vector<int> backpressure_sleeps =
      options.backpressure_sleep_us.empty() ? std::vector<int>{100, 1000, 10000} : options.backpressure_sleep_us;
  const size_t backpressure_payload_size =
      payload_sizes.empty() ? 4096 : payload_sizes[std::min<size_t>(1, payload_sizes.size() - 1)];

  for (auto suite : options.suites) {
    if (suite == Bench::kSerializationSuite) {
      for (auto payload : serialization_payloads) {
        for (auto payload_size : payload_sizes) {
          for (int repeat = 0; repeat < options.repeat_count; ++repeat) {
            Bench::Scenario scenario;
            scenario.suite = suite;
            scenario.mode = Bench::kLocalDirectMode;
            scenario.topology = Bench::kOneToOneTopology;
            scenario.rate_pattern = Bench::kMaxRatePattern;
            scenario.payload = payload;
            scenario.url = "serialize://bench";
            scenario.payload_size = payload_size;
            scenario.burst_messages = 1;
            scenario.duration_ms = options.serialization_duration_ms;
            scenario.repeat_index = repeat;
            scenarios.emplace_back(std::move(scenario));
          }
        }
      }

      continue;
    }

    const auto& suite_payloads = suite == Bench::kTopologySuite ? topology_payloads : transport_payloads;

    for (auto mode : modes) {
      for (const auto& url : urls) {
        for (const auto& qos_profile : qos_profiles) {
          for (auto payload : suite_payloads) {
            if (suite == Bench::kThroughputSuite) {
              if (enable_max_rate) {
                for (auto payload_size : payload_sizes) {
                  for (int repeat = 0; repeat < options.repeat_count; ++repeat) {
                    append_throughput_case(suite, mode, payload, url, qos_profile, payload_size, Bench::kMaxRatePattern,
                                           0, 1, repeat);
                  }
                }
              }

              if (enable_fixed_rate && !latency_rates.empty()) {
                for (auto payload_size : throughput_sweep_sizes) {
                  for (auto rate_hz : latency_rates) {
                    for (int repeat = 0; repeat < options.repeat_count; ++repeat) {
                      append_throughput_case(suite, mode, payload, url, qos_profile, payload_size,
                                             Bench::kFixedRatePattern, rate_hz, 1, repeat);
                    }
                  }
                }
              }

              if (enable_burst_rate) {
                for (auto payload_size : throughput_burst_sizes) {
                  for (auto rate_hz : burst_rates) {
                    for (auto burst_count : effective_burst_messages) {
                      for (int repeat = 0; repeat < options.repeat_count; ++repeat) {
                        append_throughput_case(suite, mode, payload, url, qos_profile, payload_size,
                                               Bench::kBurstRatePattern, rate_hz, burst_count, repeat);
                      }
                    }
                  }
                }
              }
            } else if (suite == Bench::kLatencySuite) {
              for (auto payload_size : latency_sizes) {
                for (int repeat = 0; repeat < options.repeat_count; ++repeat) {
                  append_topology_case(suite, mode, payload, url, qos_profile, payload_size, Bench::kOneToOneTopology,
                                       Bench::kFixedRatePattern, latency_floor_rate, 1, 1, 1, repeat);
                }
              }

              for (auto payload_size : latency_pressure_sizes) {
                for (auto rate_hz : latency_rates) {
                  if (rate_hz == latency_floor_rate) {
                    continue;
                  }

                  for (int repeat = 0; repeat < options.repeat_count; ++repeat) {
                    append_topology_case(suite, mode, payload, url, qos_profile, payload_size, Bench::kOneToOneTopology,
                                         Bench::kFixedRatePattern, rate_hz, 1, 1, 1, repeat);
                  }
                }
              }

              if (enable_burst_rate) {
                for (auto payload_size : latency_burst_sizes) {
                  for (auto rate_hz : burst_rates) {
                    for (auto burst_count : effective_burst_messages) {
                      for (int repeat = 0; repeat < options.repeat_count; ++repeat) {
                        append_topology_case(suite, mode, payload, url, qos_profile, payload_size,
                                             Bench::kOneToOneTopology, Bench::kBurstRatePattern, rate_hz, burst_count,
                                             1, 1, repeat);
                      }
                    }
                  }
                }
              }
            } else if (suite == Bench::kBackpressureSuite) {
              for (auto sleep_us : backpressure_sleeps) {
                if (sleep_us <= 0) {
                  continue;
                }

                for (int repeat = 0; repeat < options.repeat_count; ++repeat) {
                  Bench::Scenario scenario;
                  make_base(scenario, suite, mode, payload, url, qos_profile, repeat);
                  scenario.topology = Bench::kOneToOneTopology;
                  scenario.rate_pattern = Bench::kMaxRatePattern;
                  scenario.payload_size = backpressure_payload_size;
                  scenario.rate_hz = 0;
                  scenario.burst_messages = 1;
                  scenario.subscriber_sleep_us = sleep_us;
                  scenarios.emplace_back(std::move(scenario));
                }
              }
            } else if (suite == Bench::kTopologySuite) {
              for (auto topology : topologies) {
                if (topology == Bench::kOneToOneTopology) {
                  continue;
                }

                if (enable_max_rate) {
                  for (auto payload_size : topology_scale_sizes) {
                    if (topology == Bench::kOneToManyTopology) {
                      for (auto subscribers : fanout_subscribers) {
                        for (int repeat = 0; repeat < options.repeat_count; ++repeat) {
                          append_topology_case(suite, mode, payload, url, qos_profile, payload_size, topology,
                                               Bench::kMaxRatePattern, 0, 1, 1, subscribers, repeat);
                        }
                      }

                      continue;
                    }

                    if (topology == Bench::kManyToOneTopology) {
                      for (auto publishers : publisher_counts) {
                        for (int repeat = 0; repeat < options.repeat_count; ++repeat) {
                          append_topology_case(suite, mode, payload, url, qos_profile, payload_size, topology,
                                               Bench::kMaxRatePattern, 0, 1, publishers, 1, repeat);
                        }
                      }

                      continue;
                    }

                    for (auto publishers : publisher_counts) {
                      for (auto subscribers : fanout_subscribers) {
                        for (int repeat = 0; repeat < options.repeat_count; ++repeat) {
                          append_topology_case(suite, mode, payload, url, qos_profile, payload_size, topology,
                                               Bench::kMaxRatePattern, 0, 1, publishers, subscribers, repeat);
                        }
                      }
                    }
                  }
                }

                if (enable_fixed_rate) {
                  for (auto payload_size : topology_pressure_sizes) {
                    for (auto rate_hz : topology_pressure_rates) {
                      if (topology == Bench::kOneToManyTopology) {
                        for (auto subscribers : fanout_subscribers) {
                          for (int repeat = 0; repeat < options.repeat_count; ++repeat) {
                            append_topology_case(suite, mode, payload, url, qos_profile, payload_size, topology,
                                                 Bench::kFixedRatePattern, rate_hz, 1, 1, subscribers, repeat);
                          }
                        }

                        continue;
                      }

                      if (topology == Bench::kManyToOneTopology) {
                        for (auto publishers : publisher_counts) {
                          for (int repeat = 0; repeat < options.repeat_count; ++repeat) {
                            append_topology_case(suite, mode, payload, url, qos_profile, payload_size, topology,
                                                 Bench::kFixedRatePattern, rate_hz, 1, publishers, 1, repeat);
                          }
                        }

                        continue;
                      }

                      for (auto publishers : publisher_counts) {
                        for (auto subscribers : fanout_subscribers) {
                          for (int repeat = 0; repeat < options.repeat_count; ++repeat) {
                            append_topology_case(suite, mode, payload, url, qos_profile, payload_size, topology,
                                                 Bench::kFixedRatePattern, rate_hz, 1, publishers, subscribers, repeat);
                          }
                        }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  for (auto& scenario : scenarios) {
    if (scenario.payload_size == 0) {
      continue;
    }

    const int kib = static_cast<int>(scenario.payload_size / 1024);
    const int auto_drain = options.drain_ms + kib / 4;
    const int capped = std::min(auto_drain, 1500);
    scenario.drain_ms = std::max(scenario.drain_ms, capped);
  }

  return scenarios;
}

const char* Bench::suite_to_string(Suite suite) noexcept {
  switch (suite) {
    case kThroughputSuite:
      return "throughput";
    case kLatencySuite:
      return "latency";
    case kTopologySuite:
      return "topology";
    case kSerializationSuite:
      return "serialization";
    case kBackpressureSuite:
      return "backpressure";
    default:
      return "unknown";
  }
}

const char* Bench::mode_to_string(Mode mode) noexcept {
  switch (mode) {
    case kLocalDirectMode:
      return "local-direct";
    case kLocalLoopMode:
      return "local-loop";
    case kProcessMode:
      return "process";
    default:
      return "unknown";
  }
}

const char* Bench::topology_to_string(Topology topology) noexcept {
  switch (topology) {
    case kOneToOneTopology:
      return "1:1";
    case kOneToManyTopology:
      return "1:n";
    case kManyToOneTopology:
      return "n:1";
    case kManyToManyTopology:
      return "n:n";
    default:
      return "unknown";
  }
}

const char* Bench::rate_pattern_to_string(RatePattern pattern) noexcept {
  switch (pattern) {
    case kMaxRatePattern:
      return "max";
    case kFixedRatePattern:
      return "fixed";
    case kBurstRatePattern:
      return "burst";
    default:
      return "unknown";
  }
}

const char* Bench::payload_to_string(PayloadKind payload) noexcept {
  switch (payload) {
    case kBytesPayload:
      return "bytes";
    case kStringPayload:
      return "string";
    case kRawDataPayload:
      return "rawdata";
    default:
      return "unknown";
  }
}

bool Bench::parse_suite(const std::string& value, Suite& suite) noexcept {
  auto lower = to_lower_copy(value);

  if (lower == "throughput") {
    suite = kThroughputSuite;
    return true;
  }

  if (lower == "latency") {
    suite = kLatencySuite;
    return true;
  }

  if (lower == "fanout" || lower == "topology" || lower == "scale") {
    suite = kTopologySuite;
    return true;
  }

  if (lower == "serialization") {
    suite = kSerializationSuite;
    return true;
  }

  if (lower == "backpressure" || lower == "bp") {
    suite = kBackpressureSuite;
    return true;
  }

  return false;
}

bool Bench::parse_mode(const std::string& value, Mode& mode) noexcept {
  auto lower = to_lower_copy(value);

  if (lower == "local" || lower == "local-loop" || lower == "loop") {
    mode = kLocalLoopMode;
    return true;
  }

  if (lower == "local-direct" || lower == "direct") {
    mode = kLocalDirectMode;
    return true;
  }

  if (lower == "process") {
    mode = kProcessMode;
    return true;
  }

  return false;
}

bool Bench::parse_topology(const std::string& value, Topology& topology) noexcept {
  auto lower = to_lower_copy(value);

  if (lower == "1:1" || lower == "1x1" || lower == "one-to-one") {
    topology = kOneToOneTopology;
    return true;
  }

  if (lower == "1:n" || lower == "1xn" || lower == "fanout" || lower == "one-to-many") {
    topology = kOneToManyTopology;
    return true;
  }

  if (lower == "n:1" || lower == "nx1" || lower == "many-to-one") {
    topology = kManyToOneTopology;
    return true;
  }

  if (lower == "n:n" || lower == "nxn" || lower == "many-to-many") {
    topology = kManyToManyTopology;
    return true;
  }

  return false;
}

bool Bench::parse_rate_pattern(const std::string& value, RatePattern& pattern) noexcept {
  auto lower = to_lower_copy(value);

  if (lower == "max" || lower == "unlimited") {
    pattern = kMaxRatePattern;
    return true;
  }

  if (lower == "fixed" || lower == "rate") {
    pattern = kFixedRatePattern;
    return true;
  }

  if (lower == "burst") {
    pattern = kBurstRatePattern;
    return true;
  }

  return false;
}

bool Bench::parse_payload(const std::string& value, PayloadKind& payload) noexcept {
  auto lower = to_lower_copy(value);

  if (lower == "bytes" || lower == "raw") {
    payload = kBytesPayload;
    return true;
  }

  if (lower == "string" || lower == "text") {
    payload = kStringPayload;
    return true;
  }

  if (lower == "rawdata" || lower == "zerocopy") {
    payload = kRawDataPayload;
    return true;
  }

  return false;
}

void Bench::request_stop() noexcept { stop_requested_flag.store(true, std::memory_order_relaxed); }

void Bench::reset_stop() noexcept { stop_requested_flag.store(false, std::memory_order_relaxed); }

bool Bench::stop_requested() noexcept { return stop_requested_flag.load(std::memory_order_relaxed); }

bool Bench::run(const RunOptions& options, Result& result, std::string& error) {
  if VUNLIKELY (check_stop_requested(error)) {
    return false;
  }

  if (options.suites.empty()) {
    error = "benchmark suites are empty";
    return false;
  }

  if (options.payloads.empty()) {
    error = "benchmark payloads are empty";
    return false;
  }

  result.version = VLINK_VERSION;
  result.created_at = get_now_string();
  result.host_name = Utils::get_host_name();
  result.platform = get_platform_name();
  result.planned_case_count = 0;
  result.skipped_case_count = 0;
  result.skip_messages.clear();
  result.scenarios.clear();

  auto scenarios = expand_scenarios(options);
  result.planned_case_count = scenarios.size();
  std::vector<Scenario> runnable_scenarios;
  runnable_scenarios.reserve(scenarios.size());
  size_t skipped_count = 0;

  const bool has_local_mode = std::any_of(options.modes.begin(), options.modes.end(),
                                          [](Mode m) { return m == kLocalDirectMode || m == kLocalLoopMode; });

  bool intra_rewritten_logged = false;
  bool intra_skipped_logged = false;
  for (const auto& scenario : scenarios) {
    const bool is_intra = get_transport_from_url(scenario.url) == "intra";

    if (is_intra && scenario.mode == kProcessMode) {
      if (has_local_mode) {
        ++skipped_count;

        if (!intra_skipped_logged) {
          result.skip_messages.emplace_back(
              "intra:// process-mode scenarios skipped (local modes are enabled; intra cannot run across processes)");
          intra_skipped_logged = true;
        }

        continue;
      }

      Scenario rewritten = scenario;
      rewritten.mode = kLocalDirectMode;
      runnable_scenarios.emplace_back(std::move(rewritten));

      if (!intra_rewritten_logged) {
        result.skip_messages.emplace_back(
            "intra:// process-mode scenarios rewritten to local-direct (intra is in-process only)");
        intra_rewritten_logged = true;
      }

      continue;
    }

    runnable_scenarios.emplace_back(scenario);
  }

  result.skipped_case_count = skipped_count;

  if (runnable_scenarios.empty()) {
    error = result.skip_messages.empty() ? "no runnable scenarios after validation"
                                         : "no runnable scenarios after validation: " + result.skip_messages.front();
    return false;
  }

  const bool color = use_run_output_color();
  print_run_line(" vlink-bench  \xE2\x94\x82  run  \xE2\x94\x82  planned " + std::to_string(result.planned_case_count) +
                     "   runnable " + std::to_string(runnable_scenarios.size()) + "   skipped " +
                     std::to_string(skipped_count) +
                     (color ? std::string("   \xE2\x94\x82  q / esc stops current run") : std::string()),
                 color ? kRunStyleBanner : nullptr);

  {
    const int rule_width = get_run_output_width();
    std::string rule;
    rule.reserve(static_cast<size_t>(rule_width) * 3);

    for (int i = 0; i < rule_width; ++i) rule.append("\xE2\x94\x80");
    print_run_line(rule, color ? kRunStyleRule : nullptr);
  }

  result.scenarios.reserve(runnable_scenarios.size());

  {
    const int64_t initial_plan_ms = plan_remaining_ms(runnable_scenarios, 0);
    print_run_line(make_run_eta_line(0, runnable_scenarios.size(), 0, initial_plan_ms, false),
                   color ? kRunStyleDim : nullptr);
  }

  ElapsedTimer total_elapsed(ElapsedTimer::kMilli);
  total_elapsed.start();
  size_t index = 0;

  for (const auto& scenario : runnable_scenarios) {
    if VUNLIKELY (check_stop_requested(error)) {
      return false;
    }

    ++index;

    print_run_line(make_run_case_title(index, runnable_scenarios.size(), scenario), color ? kRunStyleTitle : nullptr);

    ScenarioResult scenario_result;
    std::string scenario_error;
    ElapsedTimer scenario_elapsed(ElapsedTimer::kMilli);
    scenario_elapsed.start();

    {
      ProgressTicker ticker(color, scenario.warmup_ms, scenario.duration_ms, scenario.drain_ms);
      bool ok = run_scenario(options, scenario, scenario_result, scenario_error);

      if VUNLIKELY (!ok) {
        scenario_result.scenario = scenario;
        scenario_result.transport = get_transport_from_url(scenario.url);
        scenario_result.error = scenario_error;
        scenario_result.success = false;
      }
    }

    const auto elapsed_ms = scenario_elapsed.get();

    print_run_line(make_run_result_line(scenario_result, elapsed_ms), nullptr);

    result.scenarios.emplace_back(std::move(scenario_result));

    const auto wall_elapsed_ms = total_elapsed.get();
    const int64_t plan_left_ms = index < runnable_scenarios.size() ? plan_remaining_ms(runnable_scenarios, index) : 0;
    print_run_line(make_run_eta_line(index, runnable_scenarios.size(), wall_elapsed_ms, plan_left_ms, true),
                   color ? kRunStyleDim : nullptr);

    if (index < runnable_scenarios.size()) {
      std::string cooldown_error;
      if (!sleep_for_or_stop(std::chrono::milliseconds(200), cooldown_error)) {
        if (!cooldown_error.empty()) {
          error = std::move(cooldown_error);
          return false;
        }
      }
    }
  }

  return true;
}

bool Bench::save_json(const Result& result, const std::string& file_path, std::string& error) {
  if (!ensure_parent_dir(file_path, error)) {
    return false;
  }

  json root;
  root["version"] = result.version;
  root["created_at"] = result.created_at;
  root["host_name"] = result.host_name;
  root["platform"] = result.platform;
  root["command_line"] = result.command_line;
  root["planned_case_count"] = result.planned_case_count;
  root["skipped_case_count"] = result.skipped_case_count;
  root["skip_messages"] = result.skip_messages;
  root["scenarios"] = json::array();

  for (const auto& scenario : result.scenarios) {
    json item;
    result_to_json(item, scenario);
    root["scenarios"].push_back(std::move(item));
  }

  const std::filesystem::path target_path(file_path);
  std::filesystem::path tmp_path = target_path;
  tmp_path += ".tmp";

  {
    std::ofstream stream(tmp_path, std::ios::out | std::ios::trunc | std::ios::binary);

    if (!stream.is_open()) {
      error = "open json file failed: " + tmp_path.string();
      return false;
    }

    stream << root.dump(2);
    stream.flush();

    if (!stream.good()) {
      std::error_code ec;
      std::filesystem::remove(tmp_path, ec);
      error = "write json file failed";
      return false;
    }
  }

  std::error_code ec;
  std::filesystem::rename(tmp_path, target_path, ec);

  if (ec) {
    std::error_code remove_ec;
    std::filesystem::remove(tmp_path, remove_ec);
    error = "rename json file failed: " + ec.message();
    return false;
  }

  return true;
}

bool Bench::load_json(const std::string& file_path, Result& result, std::string& error) {
  std::ifstream stream(file_path);

  if (!stream.is_open()) {
    error = "open json file failed";
    return false;
  }

  json root;

  try {
    stream >> root;
  } catch (const json::exception& e) {
    error = e.what();
    return false;
  }

  result.version = root.value("version", std::string());
  result.created_at = root.value("created_at", std::string());
  result.host_name = root.value("host_name", std::string());
  result.platform = root.value("platform", std::string());
  result.command_line = root.value("command_line", std::string());
  result.planned_case_count = root.value("planned_case_count", size_t{0});
  result.skipped_case_count = root.value("skipped_case_count", size_t{0});
  result.skip_messages = root.value("skip_messages", std::vector<std::string>());
  result.scenarios.clear();

  if (!root.contains("scenarios") || !root["scenarios"].is_array()) {
    error = "invalid json scenarios";
    return false;
  }

  for (const auto& item : root["scenarios"]) {
    ScenarioResult scenario;

    if (!result_from_json(item, scenario, error)) {
      return false;
    }

    result.scenarios.emplace_back(std::move(scenario));
  }

  return true;
}

int Bench::run_pub_worker(const WorkerOptions& options) {
  ScenarioResult result;
  std::vector<double> latency_samples;
  std::vector<double> send_block_samples;
  result.scenario.mode = kProcessMode;
  result.scenario.payload = options.payload;
  result.scenario.url = options.url;
  result.scenario.payload_size = options.payload_size;
  result.scenario.rate_hz = options.rate_hz;
  result.scenario.warmup_ms = options.warmup_ms;
  result.scenario.duration_ms = options.duration_ms;
  result.scenario.drain_ms = options.drain_ms;
  result.scenario.subscriber_sleep_us = options.subscriber_sleep_us;

  std::string error;
  bool ok = false;

  switch (options.payload) {
    case kBytesPayload:
      ok = run_pub_worker_impl<Bytes>(options, result, send_block_samples, error);
      break;
    case kStringPayload:
      ok = run_pub_worker_impl<std::string>(options, result, send_block_samples, error);
      break;
    case kRawDataPayload:
      ok = run_pub_worker_impl<zerocopy::RawData>(options, result, send_block_samples, error);
      break;
    default:
      error = "invalid payload kind";
      break;
  }

  if VUNLIKELY (!ok) {
    result.success = false;
    result.error = error;
  }

  if (!options.result_file.empty()) {
    std::string save_error;

    if (!save_worker_result(result, latency_samples, send_block_samples, options.result_file, save_error)) {
      std::cerr << save_error << std::endl;
      return 1;
    }
  }

  return ok ? 0 : 1;
}

int Bench::run_sub_worker(const WorkerOptions& options) {
  ScenarioResult result;
  std::vector<double> latency_samples;
  result.scenario.mode = kProcessMode;
  result.scenario.payload = options.payload;
  result.scenario.url = options.url;
  result.scenario.payload_size = options.payload_size;
  result.scenario.rate_hz = options.rate_hz;
  result.scenario.warmup_ms = options.warmup_ms;
  result.scenario.duration_ms = options.duration_ms;
  result.scenario.drain_ms = options.drain_ms;
  result.scenario.subscriber_sleep_us = options.subscriber_sleep_us;

  std::string error;
  bool ok = false;

  switch (options.payload) {
    case kBytesPayload:
      ok = run_sub_worker_impl<Bytes>(options, result, latency_samples, error);
      break;
    case kStringPayload:
      ok = run_sub_worker_impl<std::string>(options, result, latency_samples, error);
      break;
    case kRawDataPayload:
      ok = run_sub_worker_impl<zerocopy::RawData>(options, result, latency_samples, error);
      break;
    default:
      error = "invalid payload kind";
      break;
  }

  if VUNLIKELY (!ok) {
    result.success = false;
    result.error = error;
  }

  if (!options.result_file.empty()) {
    std::string save_error;
    std::vector<double> empty_send_block;

    if (!save_worker_result(result, latency_samples, empty_send_block, options.result_file, save_error)) {
      std::cerr << save_error << std::endl;
      return 1;
    }
  }

  return ok ? 0 : 1;
}

}  // namespace vlink::bench
