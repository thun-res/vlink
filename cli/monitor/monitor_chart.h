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

#include "./monitor_common.h"

struct SparklineHistory final {
  std::deque<double> freq_history;
  std::deque<double> rate_history;
  std::deque<double> latency_history;
  std::deque<double> loss_history;

  void add_sample(double freq, double rate, double latency, double loss) {
    freq_history.emplace_back(freq);
    rate_history.emplace_back(rate);
    latency_history.emplace_back(latency);
    loss_history.emplace_back(loss);

    size_t data_size =
        use_chart_dot ? static_cast<size_t>(chart_width.load()) * 2 : static_cast<size_t>(chart_width.load());

    while (freq_history.size() > data_size) {
      freq_history.pop_front();
    }

    while (rate_history.size() > data_size) {
      rate_history.pop_front();
    }

    while (latency_history.size() > data_size) {
      latency_history.pop_front();
    }

    while (loss_history.size() > data_size) {
      loss_history.pop_front();
    }
  }

  void clear() {
    freq_history.clear();
    rate_history.clear();
    latency_history.clear();
    loss_history.clear();
  }
};

class SparklineRenderer final {
 public:
  static const char* get_spark_char(int left_level, int right_level) {
    static constexpr uint8_t kLeftBits[] = {
        0x00, 0x40, 0x44, 0x46, 0x47,
    };

    static constexpr uint8_t kRightBits[] = {
        0x00, 0x80, 0xA0, 0xB0, 0xB8,
    };

    left_level = std::clamp(left_level, 0, 4);
    right_level = std::clamp(right_level, 0, 4);

    uint8_t bits = kLeftBits[left_level] | kRightBits[right_level];

    thread_local char buf[4];
    buf[0] = '\xe2';
    buf[1] = '\xa0' | ((bits >> 6) & 0x03);
    buf[2] = '\x80' | (bits & 0x3f);
    buf[3] = '\0';

    return buf;
  }
  static const char* get_spark_char(int level) {
    static constexpr const char* kSparkChars[] = {
        "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
        "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88",
    };

    if (level < 0) {
      level = 0;
    }

    if (level > 7) {
      level = 7;
    }

    return kSparkChars[level];
  }

  static constexpr const char* get_vline() { return "\xe2\x94\x82"; }

  static constexpr const char* get_hline() { return "\xe2\x94\x80"; }

  static constexpr const char* get_tline() { return "\xe2\x80\xbe"; }

  static std::string fill_string(const std::string& input, int width) {
    int fill_size = width - input.size();

    if (fill_size < 0) {
      return std::string(input.substr(0, width));
    } else if (fill_size == 0) {
      return input;
    }

    return input + std::string(fill_size, ' ');
  }

  static std::string repeat_str(const std::string& input, int count) {
    std::string result;

    result.reserve(input.size() * count);

    for (int i = 0; i < count; ++i) {
      result += input;
    }

    return result;
  }

  static std::vector<std::string> render_process_panel(const std::vector<vlink::DiscoveryViewer::Process>& process_list,
                                                       int panel_height) {
    std::vector<std::string> panel_lines;

    if (process_list.empty()) {
      return panel_lines;
    }

    std::string split_str = "\033[2;37m" + repeat_str(get_hline(), process_width) + "\033[0m";

    std::string more_str = fill_string("\033[5;1;34m ...... (more)\033[0m", process_width + 13);

    struct ProcessPtrCmp final {
      bool operator()(const vlink::DiscoveryViewer::Process* a, const vlink::DiscoveryViewer::Process* b) const {
        return *a < *b;
      }
    };

    std::set<vlink::DiscoveryViewer::Process*, ProcessPtrCmp> process_sort_list;

    std::map<std::tuple<uint32_t, std::string, std::string, std::string>, vlink::DiscoveryViewer::Process> process_map;

    for (const auto& process : process_list) {
      auto msg = std::make_tuple(process.pid, process.name, process.ip, process.host);

      auto [iter, inserted] = process_map.try_emplace(std::move(msg), process);

      if (!inserted) {
        iter->second.type |= process.type;
      }

      process_sort_list.emplace(&(iter->second));
    }

    for (auto* process : process_sort_list) {
      if (panel_lines.size() >= static_cast<size_t>(panel_height - 1)) {
        panel_lines.emplace_back(more_str);
        break;
      }

      std::string type_str;
      int type_visible_len = 0;

      if (process->type & vlink::kPublisher) {
        type_str += "\033[1;32m[ Publisher  ]\033[0m";
        type_visible_len += 16;
      }

      if (process->type & vlink::kSubscriber) {
        if (!type_str.empty()) {
          type_str += "  ";
        }

        type_str += "\033[1;34m[ Subscriber ]\033[0m";
        type_visible_len += 16;
      }

      if (process->type & vlink::kServer) {
        if (!type_str.empty()) {
          type_str += " ";
        }

        type_str += "\033[1;32m[ Server ]\033[0m";
        type_visible_len += 12;
      }

      if (process->type & vlink::kClient) {
        if (!type_str.empty()) {
          type_str += "  ";
        }

        type_str += "\033[1;34m[ Client ]\033[0m";
        type_visible_len += 12;
      }

      if (process->type & vlink::kSetter) {
        if (!type_str.empty()) {
          type_str += "  ";
        }

        type_str += "\033[1;32m[ Setter ]\033[0m";
        type_visible_len += 12;
      }

      if (process->type & vlink::kGetter) {
        if (!type_str.empty()) {
          type_str += "  ";
        }

        type_str += "\033[1;34m[ Getter ]\033[0m";
        type_visible_len += 12;
      }

      if (type_visible_len < process_width) {
        int total_padding = process_width - type_visible_len;
        int left_padding = total_padding / 2;
        int right_padding = total_padding - left_padding;

        std::string centered_type_str = "\033[2;37m" + repeat_str(get_hline(), left_padding) + "\033[0m " + type_str +
                                        " \033[2;37m" + repeat_str(get_hline(), right_padding) + "\033[0m";

        panel_lines.emplace_back(centered_type_str);
      } else {
        panel_lines.emplace_back(type_str);
      }

      if (panel_lines.size() >= static_cast<size_t>(panel_height - 1)) {
        panel_lines.emplace_back(more_str);
        break;
      }

      panel_lines.emplace_back(
          fill_string("\033[1;35m " + process->host + "\033[0m@" + process->ip, process_width + 11));

      if (panel_lines.size() >= static_cast<size_t>(panel_height - 1)) {
        panel_lines.emplace_back(more_str);
        break;
      }

      panel_lines.emplace_back(
          fill_string("\033[1;33m " + process->name + "\033[0m#" + std::to_string(process->pid), process_width + 11));

      if (panel_lines.size() >= static_cast<size_t>(panel_height - 1)) {
        panel_lines.emplace_back(more_str);
        break;
      }
    }

    panel_lines.emplace_back(split_str);

    return panel_lines;
  }

  static std::string format_value(double val, int width = 7, int unit_value = 1000) {
    std::string result;

    if (val >= static_cast<double>(unit_value) * unit_value * unit_value) {
      result = vlink::Helpers::double_to_string(val / unit_value / unit_value / unit_value, 1) + "G";
    } else if (val >= static_cast<double>(unit_value) * unit_value) {
      result = vlink::Helpers::double_to_string(val / unit_value / unit_value, 1) + "M";
    } else if (val >= static_cast<double>(unit_value)) {
      result = vlink::Helpers::double_to_string(val / unit_value, 1) + "K";
    } else if (val >= 100) {
      result = vlink::Helpers::double_to_string(val, 0);
    } else if (val >= 10) {
      result = vlink::Helpers::double_to_string(val, 1);
    } else {
      result = vlink::Helpers::double_to_string(val, 2);
    }

    int padding = width - static_cast<int>(result.size());

    if (padding > 0) {
      result.insert(0, padding, ' ');
    }

    return result;
  }

  static std::vector<std::string> render_chart_lines(const std::string& title, const std::deque<double>& data,
                                                     const std::string& unit, const std::string& color,
                                                     int chart_height, int unit_value) {
    std::vector<std::string> lines;

    static std::string bg_color_code = "\033[4m";

    std::string title_end =
        "(0 - " + std::to_string(use_chart_dot ? chart_width.load() * 2 : chart_width.load()) + "s)";

    std::string title_content = title;

    int title_len = title_content.size();

    int padding_total = chart_width - title_len - title_end.size();

    std::string centered_title;

    std::string current_value_str;

    double min_val = 0;
    double max_val = 0;

    if (!data.empty()) {
      min_val = *std::min_element(data.begin(), data.end());
      max_val = *std::max_element(data.begin(), data.end());
      current_value_str = ": " + format_value(data.back(), 0, unit_value) + unit;
    }

    title_content += current_value_str;
    padding_total -= current_value_str.size();

    if (padding_total > 0) {
      centered_title = bg_color_code + title_content + std::string(padding_total, ' ') + title_end + "\033[0m";
    } else {
      centered_title = bg_color_code + title_content.substr(0, chart_width) + current_value_str + title_end + "\033[0m";
    }

    lines.emplace_back(std::string(7 + 1, ' ') + centered_title);

    double range = max_val - min_val;

    if (range < 1e-9) {
      if (max_val > 0) {
        range = max_val * 0.1;
        min_val = std::max(0.0, max_val - range);
      } else {
        range = 1.0;
        min_val = 0.0;
        max_val = 1.0;
      }
    }

    int chart_height_norm = std::max(chart_height, 1);

    for (int row = chart_height - 1; row >= 0; --row) {
      thread_local std::ostringstream line;
      line.clear();
      line.str("");

      double threshold_bottom = min_val + (range * row / chart_height_norm);
      double threshold_top = min_val + (range * (row + 1) / chart_height_norm);

      if (row == chart_height - 1) {
        line << format_value(max_val, 7, unit_value) << get_vline();
      } else if (row == 0) {
        line << format_value(min_val, 7, unit_value) << get_vline();
      } else if (row == chart_height / 2 && chart_height >= 5) {
        line << format_value((min_val + max_val) / 2, 7, unit_value) << get_vline();
      } else {
        line << std::string(7, ' ') << get_vline();
      }

      line << color;

      if (use_chart_dot) {
        size_t data_size = static_cast<size_t>(chart_width.load()) * 2;

        for (size_t col = 0; col < data_size; col += 2) {
          auto get_level = [&data, &data_size, &threshold_top, &threshold_bottom, &row, &min_val](int c) -> int {
            size_t idx;

            if (data.size() < data_size) {
              int offset = data_size - static_cast<int>(data.size());

              if (c < offset) {
                return -1;
              }

              idx = c - offset;
            } else {
              idx = c;
            }

            double val = data[idx];

            if (val >= threshold_top) {
              return 4;
            } else if (val > threshold_bottom) {
              double level_ratio = (val - threshold_bottom) / (threshold_top - threshold_bottom);
              return std::clamp(static_cast<int>(std::ceil(level_ratio * 4.0)), 1, 4);
            } else if (row == 0 && val >= min_val - 1e-9) {
              return 1;
            }

            return 0;
          };

          int left_level = get_level(col);
          int right_level = (col + 1 < data_size) ? get_level(col + 1) : -1;

          if (left_level < 0 && right_level < 0) {
            line << " ";
          } else {
            line << get_spark_char(std::max(0, left_level), std::max(0, right_level));
          }
        }
      } else {
        size_t data_size = chart_width;

        for (size_t col = 0; col < data_size; ++col) {
          size_t idx;

          if (data.size() < data_size) {
            int offset = data_size - static_cast<int>(data.size());

            if (static_cast<int>(col) < offset) {
              line << " ";
              continue;
            }

            idx = col - offset;
          } else {
            idx = col;
          }

          double val = 0;

          if (!data.empty()) {
            val = data[idx];
          }

          if (val >= threshold_top) {
            line << get_spark_char(7);
          } else if (val >= threshold_bottom) {
            double level_ratio = (val - threshold_bottom) / (threshold_top - threshold_bottom);
            int spark_level = static_cast<int>(std::ceil(level_ratio * 7.0));
            spark_level = std::max(1, std::min(7, spark_level));
            line << get_spark_char(spark_level);
          } else {
            line << " ";
          }
        }
      }

      line << "\033[0m";

      lines.emplace_back(line.str());
    }

    std::string x_axis = std::string(7 + 1, ' ');

    for (int i = 0; i < chart_width; ++i) {
      x_axis.append(get_tline());
    }

    lines.emplace_back(x_axis);

    return lines;
  }

  static std::vector<std::string> render_right_panel(const SparklineHistory& history, int panel_height) {
    std::vector<std::string> panel_lines;

    int available_for_charts = panel_height;

    if VUNLIKELY (available_for_charts < 1) {
      panel_lines.emplace_back("\033[1mPanel is too small\033[0m");
      return panel_lines;
    }

    int num_charts = 4;
    int per_chart_overhead = 4;

    int chart_rows = (available_for_charts - num_charts * per_chart_overhead) / num_charts;

    if (chart_rows > kChartHeight) {
      chart_rows = kChartHeight;
    }

    if (chart_rows < 1) {
      chart_rows = 1;
    }

    if (available_for_charts < (1 + per_chart_overhead) * 4) {
      num_charts = 2;
      chart_rows = (available_for_charts - num_charts * per_chart_overhead) / num_charts;

      if (chart_rows < 1) {
        chart_rows = 1;
      }
    }

    if (available_for_charts < (1 + per_chart_overhead) * 2) {
      num_charts = 1;
      chart_rows = available_for_charts - per_chart_overhead;

      if (chart_rows < 1) {
        chart_rows = 1;
      }
    }

    auto add_chart = [&panel_lines, &chart_rows](const std::string& title, const std::deque<double>& data,
                                                 const std::string& unit, const std::string& color, int unit_value) {
      const auto& chart_lines = render_chart_lines(title, data, unit, color, chart_rows, unit_value);

      for (const auto& line : chart_lines) {
        panel_lines.emplace_back(line);
      }
    };

    panel_lines.emplace_back(7 + 1 + chart_width, ' ');

    if (num_charts >= 1) {
      add_chart("Freq", history.freq_history, "Hz", "\033[36m", 100000);

      if (num_charts > 1) {
        panel_lines.emplace_back(7 + 1 + chart_width, ' ');
      }
    }

    if (num_charts >= 2) {
      add_chart("Rate", history.rate_history, "B/s", "\033[34m", 1024);

      if (num_charts > 2) {
        panel_lines.emplace_back(7 + 1 + chart_width, ' ');
      }
    }

    if (num_charts >= 3) {
      add_chart("Loss", history.loss_history, "%", "\033[33m", 100000);

      if (num_charts > 3) {
        panel_lines.emplace_back(7 + 1 + chart_width, ' ');
      }
    }

    if (num_charts >= 4) {
      add_chart("Latency", history.latency_history, "ms", "\033[35m", 100000);
    }

    return panel_lines;
  }
};
