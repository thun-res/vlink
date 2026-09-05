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

#include "./monitor_chart.h"
#include "./monitor_common.h"

class CustomDiscoveryViewer : public vlink::DiscoveryViewer {
 public:
  using DiscoveryViewer::DiscoveryViewer;

  bool post_keyboard_task(vlink::MessageLoop::Callback&& callback) {
    is_posting_keyboard_task_ = true;
    const bool ret = post_untracked_task(std::move(callback), vlink::TaskOverflowPolicy::kUseDispatcherStrategy,
                                         vlink::TaskDropPolicy::kProtected);
    is_posting_keyboard_task_ = false;

    return ret;
  }

 protected:
  uint32_t get_max_elapsed_time() const override { return is_posting_keyboard_task_ ? 0U : kMaxElapsedTime; }

 private:
  inline static thread_local bool is_posting_keyboard_task_{false};
};

// NOLINTNEXTLINE(google-readability-function-size)
int start_monitor(const std::vector<std::string>& urls, const std::string& filter, const std::string& hostname_filter,
                  const std::string& proto_dir, const std::string& fbs_dir) {
  using RawSub = vlink::Subscriber<vlink::Bytes>;

  const std::string native_ip = native_mode ? vlink::Utils::get_env("VLINK_DDS_NATIVE_IP", "127.0.0.1") : std::string();

  std::shared_ptr<CustomDiscoveryViewer> discovery_viewer;

  try {
    vlink::DiscoveryViewer::FilterType filter_type = vlink::DiscoveryViewer::kFilterAvailable;

    if (native_mode) {
      filter_type = vlink::DiscoveryViewer::kFilterNative;
    }

    discovery_viewer = std::make_shared<CustomDiscoveryViewer>(filter_type);
  } catch (vlink::Exception::RuntimeError& e) {
    std::cerr << e.what() << std::endl;
    has_quit = true;
    return -1;
  }

  if (plain_mode) {
    VLINK_TERM_OUT << "Information Collecting in Plain Text Mode, Please Wait..." << std::endl;
  } else {
    VLINK_TERM_OUT << "Information Collecting, Please Wait..." << std::endl;
  }

  VLINK_TERM_OUT.flush();

  std::unordered_map<std::string, std::shared_ptr<RawSub>> sub_ptr_map;
  std::unordered_map<std::string, std::atomic<int64_t>> sub_seq_map;
  std::unordered_map<std::string, std::atomic<size_t>> sub_size_map;
  std::unordered_map<std::string, std::atomic<double>> sub_lost_map;
  std::unordered_map<std::string, std::atomic<int64_t>> sub_lat_map;
  std::unordered_map<std::string, vlink::ElapsedTimer> sub_elapsed_map;
  std::unordered_map<std::string, std::deque<int64_t>> sub_seq_buffer_map;
  std::unordered_map<std::string, std::deque<size_t>> sub_size_buffer_map;
  std::unordered_map<std::string, std::deque<double>> sub_lost_buffer_map;
  std::unordered_map<std::string, std::deque<int64_t>> sub_lat_buffer_map;
  std::unordered_map<std::string, vlink::SampleLostInfo> sub_last_sample_map;
  std::unordered_map<std::string, SparklineHistory> sparkline_history_map;
  std::unordered_map<std::string, uint64_t> sub_retry_after_map;

  vlink::ElapsedTimer key_elapsed_timer;

  std::vector<std::string> filter_list = vlink::Helpers::split_any(filter);
  const std::vector<std::string> hostname_filter_list = vlink::Helpers::split_any(hostname_filter);

  filter_input_text = filter;

  std::unordered_set<std::string> target_urls_set(urls.begin(), urls.end());

  int active_cnt = 0;
  double total_rate = 0;

  std::mutex print_mtx;

  auto print_function = [&print_mtx, &active_cnt, &total_rate, &sparkline_history_map, &filter_list](bool by_auto) {
    std::lock_guard lock(print_mtx);

    if VUNLIKELY (!has_update) {
      return;
    }

    if (plain_mode) {
      static int print_count = 0;

      VLINK_TERM_OUT << "***** Update " << ++print_count << " at "
                     << vlink::Helpers::format_date(vlink::ElapsedTimer::get_sys_timestamp(vlink::ElapsedTimer::kNano))
                     << " *****\n";

      std::string title = std::string("[TYPE]");

      if (count_mode) {
        title += std::string(8, ' ');
      } else {
        title += std::string(4, ' ');
      }

      title += std::string("[URL]") + std::string(max_url_size - 5 + 3, ' ');

      if (ser_mode) {
        title += ("[SER]" + std::string(max_ser_size - 5 + 3, ' '));
      }

      if (detail_mode) {
        title += std::string("[FREQ]") + std::string(6, ' ') + std::string("[RATE]") + std::string(6, ' ') +
                 std::string("[LOSS]") + std::string(3, ' ') + std::string("[LATENCY]") + std::string(3, ' ');
      }

      if (profiler_mode) {
        title += ("[PROFILER]" + std::string(4, ' '));
      }

      if (detail_mode) {
        title.pop_back();
        title.pop_back();
      }

      VLINK_TERM_OUT << title << "\n";

      for (const auto& line : print_lines) {
        std::string plain_line = line;
        size_t pos = 0;

        while ((pos = plain_line.find("\033[", pos)) != std::string::npos) {
          size_t end = plain_line.find('m', pos);

          if (end != std::string::npos) {
            plain_line.erase(pos, end - pos + 1);
          } else {
            break;
          }
        }

        VLINK_TERM_OUT << plain_line << "\n";
      }

      VLINK_TERM_OUT << "Total Count: " << print_lines.size();

      if (detail_mode) {
        VLINK_TERM_OUT << " | Active: " << active_cnt
                       << " | Total Rate: " << vlink::Helpers::format_rate_size(total_rate);
      }

      VLINK_TERM_OUT << "\n" << std::flush;

      return;
    }

    if VUNLIKELY (is_jumped) {
      return;
    }

    static bool paused_draw_finished = false;

    if (!is_paused) {
      paused_draw_finished = false;
    }

    if (paused_draw_finished && by_auto) {
      return;
    }

    row_count = print_lines.size();

    target_row = max_rows.load();

    terminal_size = get_terminal_size();

    if VUNLIKELY (terminal_size.first <= 0 || terminal_size.second <= 0) {
      return;
    }

    static bool first_draw = false;

    if (!first_draw) {
      VLINK_TERM_OUT << "\033[H\033[J";
      VLINK_TERM_OUT.flush();
      first_draw = true;
    }

    auto [terminal_width, terminal_height] = terminal_size;

    int chart_panel_width = chart_mode && detail_mode ? (chart_width + 8) : 0;
    int process_panel_width = process_mode ? process_width.load() : 0;
    int total_panel_width = chart_panel_width + process_panel_width;

    bool show_process_panel = process_mode && (terminal_width >= 80);

    if (show_process_panel && terminal_width < (40 + process_panel_width)) {
      show_process_panel = false;
    }

    bool show_chart_panel = chart_mode && detail_mode && (terminal_width >= 80);

    if (show_chart_panel && terminal_width < (40 + chart_panel_width)) {
      show_chart_panel = false;
    }

    if (show_process_panel && show_chart_panel && terminal_width < (40 + total_panel_width)) {
      show_chart_panel = false;
    }

    bool show_panel = show_process_panel || show_chart_panel;

    if (target_row <= 0) {
      if (terminal_height <= 0) {
        terminal_height = 25;
      } else if (terminal_height > 100) {
        terminal_height = 100;
      }

      target_row = terminal_height - 3;

      if (target_row < 3) {
        target_row = 3;
      }
    } else {
      terminal_height = target_row;
    }

    total_pages = (print_lines.size() + target_row - 1) / target_row;

    if (current_page >= total_pages) {
      current_page = total_pages - 1;
    }

    if (current_page < 0) {
      current_page = 0;
    }

    int start_index = current_page * target_row;
    int end_index = std::min(start_index + target_row, static_cast<int>(print_lines.size()));

    std::vector<std::string> process_panel_lines;
    std::vector<std::string> chart_panel_lines;

    VLINK_TERM_OUT << "\033[H\033[K";

    if (is_paused) {
      VLINK_TERM_OUT << "\033[33m"
                     << "Information Collected by vlink-monitor (Paused):"
                     << "\033[0m" << std::endl;
    } else {
      VLINK_TERM_OUT << "Information Collected by vlink-monitor:" << std::endl;
    }

    std::string title = std::string("\033[44;37;1m") + std::string("[TYPE]");

    if (count_mode) {
      title += std::string(8, ' ');
    } else {
      title += std::string(4, ' ');
    }

    title += std::string("[URL]") + std::string(max_url_size - 5 + 3, ' ');

    if (ser_mode) {
      title += ("[SER]" + std::string(max_ser_size - 5 + 3, ' '));
    }

    if (detail_mode) {
      title += std::string("[FREQ]") + std::string(6, ' ') + std::string("[RATE]") + std::string(6, ' ') +
               std::string("[LOSS]") + std::string(3, ' ') + std::string("[LATENCY]") + std::string(3, ' ');
    }

    if (profiler_mode) {
      title += ("[PROFILER]" + std::string(4, ' '));
    }

    if (detail_mode) {
      title.pop_back();
      title.pop_back();
    }

    int title_real_size = title.size() - 10;

    if (title_real_size < 1) {
      title_real_size = 1;
    }

    if (show_chart_panel && terminal_width < (title_real_size + process_panel_width + chart_panel_width + 3)) {
      show_chart_panel = false;
      chart_panel_width = 0;
    }

    if (show_process_panel && terminal_width < (title_real_size + process_panel_width + chart_panel_width + 3)) {
      show_process_panel = false;
      process_panel_width = 0;
    }

    if (show_panel) {
      if (show_process_panel && show_chart_panel) {
        title += std::string(7 + 1, ' ');
        title += "[PROCESS]";
        title += std::string(process_width - 16, ' ');
        title += std::string(7 + 2, ' ');
        title += "[CHART]";
        title += std::string(chart_width - 7, ' ');
      } else if (show_process_panel) {
        title += std::string(7 + 1, ' ');
        title += "[PROCESS]";
        title += std::string(process_width - 16, ' ');
      } else if (show_chart_panel) {
        title += std::string(7 + 2, ' ');
        title += "[CHART]";
        title += std::string(chart_width - 7, ' ');
      }
    }

    title += "\033[0m";

    std::string vline = "\033[44;37m \033[0m";

    std::string real_title = std::string(title_real_size, ' ');

    VLINK_TERM_OUT << "\033[K";

    if (title_real_size < terminal_width) {
      VLINK_TERM_OUT << title << std::endl;
    } else {
      VLINK_TERM_OUT << title.substr(0, terminal_width + 10) << "\033[0m" << std::endl;
    }

    VLINK_TERM_OUT.flush();

    const int selected_line_snapshot = selected_line;

    if (show_process_panel) {
      if (selected_line_snapshot >= 0 && static_cast<size_t>(selected_line_snapshot) < current_info_list.size()) {
        const auto& selected_info = current_info_list[selected_line_snapshot];
        process_panel_lines = SparklineRenderer::render_process_panel(selected_info.process_list, target_row);
      } else {
        process_panel_lines = SparklineRenderer::render_process_panel({}, target_row);
      }
    }

    if (show_chart_panel) {
      if (selected_line_snapshot >= 0 && static_cast<size_t>(selected_line_snapshot) < current_info_list.size()) {
        const auto& selected_url = current_info_list[selected_line_snapshot].url;
        const auto& history = sparkline_history_map[selected_url];
        chart_panel_lines = SparklineRenderer::render_right_panel(history, target_row);
      } else {
        chart_panel_lines = SparklineRenderer::render_right_panel(SparklineHistory(), target_row);
      }
    }

    std::string current_str;

    size_t estimated_size = static_cast<size_t>(end_index - start_index) * terminal_width;

    current_str.reserve(estimated_size);

    for (int i = start_index; i < end_index; ++i) {
      std::string line_str = print_lines[i];

      int panel_row = i - start_index;

      if (title_real_size < terminal_width) {
        if (i == selected_line) {
          vlink::Helpers::replace_string(line_str, "\033[37m", "\033[30m");
          current_str += "\033[47;30;1m" + line_str + "\033[0m";
        } else {
          current_str += line_str;
        }

        if (show_process_panel) {
          current_str.append(vline);

          std::string process_part;

          if (static_cast<size_t>(panel_row) < process_panel_lines.size()) {
            process_part = process_panel_lines[panel_row];
          } else {
            process_part = std::string(process_panel_width, ' ');
          }

          current_str.append(process_part);
        }

        if (show_chart_panel) {
          current_str.append(vline);

          std::string chart_part;

          if (static_cast<size_t>(panel_row) < chart_panel_lines.size()) {
            chart_part = chart_panel_lines[panel_row];
          } else {
            chart_part = std::string(chart_panel_width, ' ');
          }

          current_str.append(chart_part);
        }
      } else {
        size_t cut = filter_box_col_to_byte(line_str, terminal_width);

        if (i == selected_line) {
          vlink::Helpers::replace_string(line_str, "\033[37m", "\033[30m");
          current_str += "\033[47;30;1m" + line_str.substr(0, cut) + "\033[0m";
        } else {
          current_str += line_str.substr(0, cut) + "\033[0m";
        }
      }

      current_str.append("\n");
    }

    for (int i = 0; i < target_row - (end_index - start_index); ++i) {
      int panel_row = (end_index - start_index) + i;

      if (title_real_size < terminal_width) {
        current_str.append(real_title);

        if (show_process_panel) {
          current_str.append(vline);

          std::string process_part;

          if (static_cast<size_t>(panel_row) < process_panel_lines.size()) {
            process_part = process_panel_lines[panel_row];
          } else {
            process_part = std::string(process_panel_width, ' ');
          }

          current_str.append(process_part);
        }

        if (show_chart_panel) {
          current_str.append(vline);

          std::string chart_part;

          if (static_cast<size_t>(panel_row) < chart_panel_lines.size()) {
            chart_part = chart_panel_lines[panel_row];
          } else {
            chart_part = std::string(chart_panel_width, ' ');
          }

          current_str.append(chart_part);
        }
      } else {
        current_str.append(std::string(terminal_width, ' '));
      }

      current_str.append("\n");
    }

    std::string last_line_str;

    last_line_str = std::string("\033[44;37;1m") + std::string("<") +
                    (total_pages == 0 ? std::string("0") : std::to_string(current_page + 1)) + std::string("/") +
                    std::to_string(total_pages) + std::string(">") + std::string("\033[0m [ ");

    if (count_mode) {
      last_line_str += "\033[4mT\033[0m ";
    } else {
      last_line_str += "\033[0mT\033[0m ";
    }

    if (detail_mode) {
      last_line_str += "\033[4mL\033[0m ";
    } else {
      last_line_str += "\033[0mL\033[0m ";
    }

    if (observe_all_mode) {
      last_line_str += "\033[4mO\033[0m ";
    } else {
      last_line_str += "\033[0mO\033[0m ";
    }

    if (profiler_mode) {
      last_line_str += "\033[4mE\033[0m ";
    } else {
      last_line_str += "\033[0mE\033[0m ";
    }

    if (ser_mode) {
      last_line_str += "\033[4mS\033[0m ";
    } else {
      last_line_str += "\033[0mS\033[0m ";
    }

    if (active_mode) {
      last_line_str += "\033[4mA\033[0m ";
    } else {
      last_line_str += "\033[0mA\033[0m ";
    }

    if (pubsub_mode) {
      last_line_str += "\033[4mY\033[0m ";
    } else {
      last_line_str += "\033[0mY\033[0m ";
    }

    if (process_mode) {
      last_line_str += "\033[4mP\033[0m ";
    } else {
      last_line_str += "\033[0mP\033[0m ";
    }

    if (chart_mode) {
      last_line_str += "\033[4mC\033[0m ";
    } else {
      last_line_str += "\033[0mC\033[0m ";
    }

    if (!filter_list.empty()) {
      last_line_str += "\033[4mI\033[0m ";
    } else {
      last_line_str += "\033[0mI\033[0m ";
    }

    last_line_str += std::string("] | Total: ") + std::to_string(print_lines.size());

    if (detail_mode) {
      last_line_str += std::string(" | Active: ") + std::to_string(active_cnt);
      last_line_str += std::string(" | Rate: ") + vlink::Helpers::format_rate_size(total_rate);
    }

    if (profiler_mode) {
      if (total_profiler < 0) {
        last_line_str += std::string(" | Profiler: ") + "N/A";
      } else {
        last_line_str += std::string(" | Profiler: ") + vlink::Helpers::double_to_string(total_profiler, 2) + "%";
      }
    }

    if VLIKELY (last_line_str.size() <= static_cast<size_t>(terminal_width + 84)) {
      current_str += last_line_str;
    } else {
      current_str += last_line_str.substr(0, terminal_width + 84);
    }

    current_str += std::string(1, ' ');

    if (filter_input_mode) {
      static constexpr int kFilterBoxMaxWidth = 60;
      static constexpr int kFilterBoxMinWidth = 24;
      static constexpr int kFilterBoxHeight = 5;
      static constexpr const char* kFilterBoxColor = "\033[44;1;37m";
      static constexpr const char* kFilterBoxReset = "\033[0m";

      int content_rows = target_row.load();

      bool box_drawable = terminal_width >= 12 && content_rows >= kFilterBoxHeight;

      int center_width = title_real_size;

      if (center_width > terminal_width) {
        center_width = terminal_width;
      }

      int box_width = std::clamp(center_width - 4, kFilterBoxMinWidth, kFilterBoxMaxWidth);

      if (box_width > terminal_width - 2) {
        box_width = terminal_width - 2;
      }

      int inner_width = box_width - 2;

      if (inner_width < 6) {
        inner_width = 6;
      }

      int box_col = (center_width - box_width) / 2 + 1;

      if (box_col < 1) {
        box_col = 1;
      }

      int box_row = 3 + (content_rows - kFilterBoxHeight) / 2;

      if (box_row > content_rows - kFilterBoxHeight + 3) {
        box_row = content_rows - kFilterBoxHeight + 3;
      }

      if (box_row < 3) {
        box_row = 3;
      }

      std::string title_label = "[ Filter URLs ]";
      int title_cols = static_cast<int>(title_label.size());
      int title_room = inner_width - 2 - title_cols;

      if (title_room < 0) {
        title_label = title_label.substr(0, inner_width - 2);
        title_cols = static_cast<int>(title_label.size());
        title_room = inner_width - 2 - title_cols;
      }

      std::string top_line = "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x80" + title_label;

      for (int i = 0; i < title_room; ++i) {
        top_line += "\xe2\x94\x80";
      }

      top_line += "\xe2\x94\x90";

      std::string mid_line = "\xe2\x94\x9c";
      std::string bottom_line = "\xe2\x94\x94";

      for (int i = 0; i < inner_width; ++i) {
        mid_line += "\xe2\x94\x80";
        bottom_line += "\xe2\x94\x80";
      }

      mid_line += "\xe2\x94\xa4";
      bottom_line += "\xe2\x94\x98";

      int avail = inner_width - 4;

      if (avail < 0) {
        avail = 0;
      }

      std::string display_text = filter_input_text;

      while (filter_box_display_width(display_text) > avail) {
        size_t adv = 1;

        while (adv < display_text.size() && (static_cast<unsigned char>(display_text[adv]) & 0xC0) == 0x80) {
          ++adv;
        }

        display_text.erase(0, adv);
      }

      int field_pad = inner_width - 4 - filter_box_display_width(display_text);

      if (field_pad < 0) {
        field_pad = 0;
      }

      std::string input_line = "\xe2\x94\x82 > \033[93m" + display_text + "\033[7m \033[27m\033[37m" +
                               std::string(field_pad, ' ') + "\xe2\x94\x82";

      std::string hint_text = "Space: multi-term   Enter/Esc: close";

      if (static_cast<int>(hint_text.size()) > inner_width - 1) {
        hint_text = hint_text.substr(0, inner_width - 1);
      }

      int hint_pad = inner_width - 1 - static_cast<int>(hint_text.size());

      if (hint_pad < 0) {
        hint_pad = 0;
      }

      std::string hint_line =
          "\xe2\x94\x82 \033[90m" + hint_text + "\033[37m" + std::string(hint_pad, ' ') + "\xe2\x94\x82";

      if (box_drawable) {
        std::string box_lines[kFilterBoxHeight] = {top_line, input_line, mid_line, hint_line, bottom_line};
        std::vector<std::string> doc_lines = vlink::Helpers::split(current_str, '\n');

        for (int k = 0; k < kFilterBoxHeight; ++k) {
          int idx = (box_row - 3) + k;

          if (idx >= 0 && idx < static_cast<int>(doc_lines.size())) {
            const std::string& src = doc_lines[idx];

            size_t left_end = filter_box_col_to_byte(src, box_col - 1);
            size_t right_start = filter_box_col_to_byte(src, box_col - 1 + box_width);

            std::string left = src.substr(0, left_end);
            std::string right = right_start < src.size() ? src.substr(right_start) : std::string();

            int pad_cols = (box_col - 1) - filter_box_display_width(left);

            if (pad_cols < 0) {
              pad_cols = 0;
            }

            std::string out;
            out.reserve(left.size() + box_lines[k].size() + right.size() + pad_cols + 32);
            out += left;
            out += kFilterBoxReset;
            out.append(pad_cols, ' ');
            out += kFilterBoxColor;
            out += box_lines[k];
            out += kFilterBoxReset;
            out += filter_box_sgr_prefix(src, right_start);
            out += right;
            out += kFilterBoxReset;

            doc_lines[idx] = std::move(out);
          }
        }

        current_str.clear();

        for (size_t k = 0; k < doc_lines.size(); ++k) {
          current_str += doc_lines[k];

          if (k < doc_lines.size() - 1) {
            current_str += "\n";
          }
        }
      }
    }

    auto print_split_view_list = vlink::Helpers::split_view(current_str, '\n');

    for (size_t i = 0; i < print_split_view_list.size(); ++i) {
      VLINK_TERM_OUT << "\033[K";
      VLINK_TERM_OUT << print_split_view_list[i];

      if (i < print_split_view_list.size() - 1) {
        VLINK_TERM_OUT << "\n";

        if (i > 0 && i % kFlushMinLine == 0) {
          VLINK_TERM_OUT.flush();
          if constexpr (kFlushMinSleep > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(kFlushMinSleep));
          }
        }
      }
    }

    VLINK_TERM_OUT.flush();

    if (is_paused) {
      paused_draw_finished = true;
    }
  };

  auto update_terminal_function = [&print_function](bool to_update = false) {
    const auto& size = get_terminal_size();

    if VUNLIKELY (size.first <= 0 || size.second <= 0) {
      return;
    }

    if (terminal_size != size) {
      terminal_size = size;

      if (to_update && !chart_mode && !process_mode) {
        print_function(false);
      }
    }
  };

  auto update_meta_function = [&print_function]() {
    if (target_row > 0) {
      int total_items = static_cast<int>(print_lines.size());
      int start_index = current_page * target_row;
      int end_index = std::min(start_index + target_row, total_items);

      if (selected_line >= 0) {
        if (selected_line >= total_items) {
          selected_line = -1;
        } else if (selected_line < start_index || selected_line >= end_index) {
          current_page = selected_line / target_row;
          print_function(false);
        }
      }

      const int selected_line_snapshot = selected_line;

      if (selected_line_snapshot >= 0 && static_cast<size_t>(selected_line_snapshot) < current_info_list.size()) {
        const auto& current_info = current_info_list.at(selected_line_snapshot);
        std::lock_guard lock(current_mtx);
        current_type = current_info.type;
        current_schema_type = static_cast<uint32_t>(current_info.schema_type);
        current_url = current_info.url;
        current_ser = current_info.ser_type;
      } else {
        std::lock_guard lock(current_mtx);
        current_type = 0;
        current_schema_type = 0;
        current_url.clear();
        current_ser.clear();
      }

    } else {
      std::lock_guard lock(current_mtx);
      current_type = 0;
      current_schema_type = 0;
      current_url.clear();
      current_ser.clear();
    }
  };

  auto clear_function = [&sub_ptr_map, &sub_seq_map, &sub_size_map, &sub_lost_map, &sub_lat_map, &sub_elapsed_map,
                         &sub_seq_buffer_map, &sub_size_buffer_map, &sub_lost_buffer_map, &sub_lat_buffer_map,
                         &sub_last_sample_map, &sparkline_history_map, &sub_retry_after_map]() {
    sub_ptr_map.clear();
    sub_seq_map.clear();
    sub_size_map.clear();
    sub_lost_map.clear();
    sub_lat_map.clear();
    sub_elapsed_map.clear();
    sub_seq_buffer_map.clear();
    sub_size_buffer_map.clear();
    sub_lost_buffer_map.clear();
    sub_lat_buffer_map.clear();
    sub_last_sample_map.clear();
    sparkline_history_map.clear();
    sub_retry_after_map.clear();
  };

  auto update_function = [&target_urls_set, &filter_list, &hostname_filter_list, &discovery_viewer, &native_ip,
                          &sub_ptr_map, &sub_seq_map, &sub_size_map, &sub_lost_map, &sub_lat_map, &sub_elapsed_map,
                          &sub_seq_buffer_map, &sub_size_buffer_map, &sub_lost_buffer_map, &sub_lat_buffer_map,
                          &sub_last_sample_map, &sparkline_history_map, &sub_retry_after_map, &clear_function,
                          &active_cnt, &total_rate, &key_elapsed_timer](bool collect_sample = false) {
    total_profiler = -1;
    active_cnt = 0;
    total_rate = 0;

    const int selected_line_snapshot = selected_line;
    std::string selected_url;

    if (selected_line_snapshot >= 0 && static_cast<size_t>(selected_line_snapshot) < current_info_list.size()) {
      selected_url = current_info_list[selected_line_snapshot].url;
    }

    if (!is_paused) {
      current_info_list.clear();
      print_lines.clear();
    }

    has_update = true;

    const auto& info_list = discovery_viewer->get_info_list();

    current_info_list.reserve(info_list.size());
    print_lines.reserve(info_list.size());

    {
      std::unordered_set<std::string> current_urls;

      current_urls.reserve(info_list.size());

      for (const auto& info : info_list) {
        current_urls.emplace(info.url);
      }

      for (auto iter = sub_seq_map.begin(); iter != sub_seq_map.end();) {
        if VUNLIKELY (current_urls.count(iter->first) == 0) {
          const std::string url = iter->first;
          sub_ptr_map.erase(url);
          sub_size_map.erase(url);
          sub_lost_map.erase(url);
          sub_lat_map.erase(url);
          sub_elapsed_map.erase(url);
          sub_seq_buffer_map.erase(url);
          sub_size_buffer_map.erase(url);
          sub_lost_buffer_map.erase(url);
          sub_lat_buffer_map.erase(url);
          sub_last_sample_map.erase(url);
          sub_retry_after_map.erase(url);
          sparkline_history_map.erase(url);

          iter = sub_seq_map.erase(iter);
        } else {
          ++iter;
        }
      }
    }

    max_url_size = 16;
    max_ser_size = 16;

    for (const auto& info : info_list) {
      max_url_size = std::max(info.url.size(), max_url_size.load());
      max_ser_size = std::max(info.ser_type.size(), max_ser_size.load());
    }

    if (!detail_mode) {
      clear_function();
    }

    int space_cnt = 0;

    for (const auto& info : info_list) {
      if (!target_urls_set.empty()) {
        bool found = target_urls_set.count(info.url) != 0;
        bool condition = black_mode ? found : !found;

        if (condition) {
          continue;
        }
      }

      if (!filter_list.empty()) {
        bool skip = black_mode ? false : true;

        std::string left_str = info.url;
        std::transform(left_str.begin(), left_str.end(), left_str.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const auto& f : filter_list) {
          if (f.empty()) {
            continue;
          }

          std::string right_str = f;
          std::transform(right_str.begin(), right_str.end(), right_str.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

          if (left_str.find(right_str) != std::string::npos) {
            skip = black_mode ? true : false;
            break;
          }
        }

        if (skip) {
          continue;
        }
      }

      if (!hostname_filter_list.empty()) {
        bool found = false;

        for (const auto& process : info.process_list) {
          for (const auto& hostname : hostname_filter_list) {
            if (std::search(process.host.begin(), process.host.end(), hostname.begin(), hostname.end(),
                            [](unsigned char left, unsigned char right) {
                              return std::tolower(left) == std::tolower(right);
                            }) != process.host.end()) {
              found = true;
              break;
            }
          }

          if (found) {
            break;
          }
        }

        if (!found) {
          continue;
        }
      }

      if (pubsub_mode) {
        if (!(info.type & vlink::kPublisher) && !(info.type & vlink::kSubscriber)) {
          continue;
        }
      }

      thread_local std::ostringstream line;
      line.clear();
      line.str("");

      auto process_profiler_func = [&info]() {
        bool has_left_value = false;
        bool has_right_value = false;
        double left_value = 0;
        double right_value = 0;

        for (const auto& process : info.process_list) {
          if ((process.type & vlink::kPublisher) || (process.type & vlink::kSetter) ||
              (process.type & vlink::kClient)) {
            has_left_value = true;
            left_value += process.profiler;
          } else {
            has_right_value = true;
            right_value += process.profiler;
          }
        }

        std::string left_str;
        std::string right_str;

        if (left_value <= 0 && left_value > -1) {
          left_str = "0.00";
        } else if (left_value < 10) {
          left_str = vlink::Helpers::double_to_string(left_value, 2);
        } else if (left_value < 100) {
          left_str = vlink::Helpers::double_to_string(left_value, 1);
        } else if (left_value < 1000) {
          left_str = vlink::Helpers::double_to_string(left_value, 0) + " ";
        } else {
          left_str = "999+";
        }

        if (right_value <= 0 && right_value > -1) {
          right_str = "0.00";
        } else if (right_value < 10) {
          right_str = vlink::Helpers::double_to_string(right_value, 2);
        } else if (right_value < 100) {
          right_str = vlink::Helpers::double_to_string(right_value, 1);
        } else if (right_value < 1000) {
          right_str = vlink::Helpers::double_to_string(right_value, 0) + " ";
        } else {
          right_str = "999+";
        }

        std::string profiler_str;

        if (left_value >= 0 && has_left_value) {
          profiler_str += left_str + "%|";

          if (total_profiler == -1) {
            total_profiler = 0;
          }

          total_profiler = total_profiler + left_value;
        } else {
          profiler_str += "-----|";
        }

        if (right_value >= 0 && has_right_value) {
          profiler_str += right_str + "%";

          if (total_profiler == -1) {
            total_profiler = 0;
          }

          total_profiler = total_profiler + right_value;
        } else {
          profiler_str += "-----";
        }

        line << profiler_str;
      };

      if (!detail_mode) {
        line << "\033[37m";

        if (count_mode) {
          line << vlink::DiscoveryViewer::convert_type_to_view(info.type, info.process_list);
        } else {
          line << vlink::DiscoveryViewer::convert_type_to_view(info.type);
        }

        line << std::string(3, ' ');

        if (!filter_list.empty()) {
          line << filter_highlight_url(info.url, filter_list);
        } else {
          line << info.url;
        }

        space_cnt = max_url_size - info.url.size() + 3;

        if VUNLIKELY (space_cnt < 3) {
          space_cnt = 3;
        }

        line << std::string(space_cnt, ' ');

        if (ser_mode) {
          line << info.ser_type;

          space_cnt = max_ser_size - info.ser_type.size() + 3;

          if VUNLIKELY (space_cnt < 3) {
            space_cnt = 3;
          }

          line << std::string(space_cnt, ' ');
        }

        if (profiler_mode) {
          process_profiler_func();
          line << std::string(1, ' ');
        }

        line << "\033[0m";

        if (!is_paused) {
          current_info_list.emplace_back(info);
          print_lines.emplace_back(line.str());
        }

        continue;
      }

      std::atomic<int64_t>& seq = sub_seq_map[info.url];
      std::atomic<size_t>& size = sub_size_map[info.url];
      std::atomic<double>& lost = sub_lost_map[info.url];
      std::atomic<int64_t>& lat = sub_lat_map[info.url];
      vlink::ElapsedTimer& elapsed = sub_elapsed_map[info.url];
      std::deque<int64_t>& seq_buffer = sub_seq_buffer_map[info.url];
      std::deque<size_t>& size_buffer = sub_size_buffer_map[info.url];
      std::deque<double>& lost_buffer = sub_lost_buffer_map[info.url];
      std::deque<int64_t>& lat_buffer = sub_lat_buffer_map[info.url];

      SparklineHistory& spark_history = sparkline_history_map[info.url];

      space_cnt = max_url_size - info.url.size() + 3;

      if VUNLIKELY (space_cnt < 3) {
        space_cnt = 3;
      }

      if ((!(info.type & vlink::kPublisher) && !(info.type & vlink::kSetter)) ||
          (!has_intra_bind && vlink::Url::is_intra_type(info.url)) ||
          (!observe_all_mode && (selected_url != info.url || (!is_paused && key_elapsed_timer.get() < 250)))) {
        sub_ptr_map.erase(info.url);
        sub_seq_map.erase(info.url);
        sub_size_map.erase(info.url);
        sub_lost_map.erase(info.url);
        sub_lat_map.erase(info.url);
        sub_elapsed_map.erase(info.url);
        sub_seq_buffer_map.erase(info.url);
        sub_size_buffer_map.erase(info.url);
        sub_lost_buffer_map.erase(info.url);
        sub_lat_buffer_map.erase(info.url);
        sparkline_history_map.erase(info.url);
        sub_last_sample_map.erase(info.url);
        sub_retry_after_map.erase(info.url);

        if (observe_all_mode && active_mode) {
          continue;
        }

        line << "\033[37m";

        if (count_mode) {
          line << vlink::DiscoveryViewer::convert_type_to_view(info.type, info.process_list);
        } else {
          line << vlink::DiscoveryViewer::convert_type_to_view(info.type);
        }

        line << std::string(3, ' ');

        if (!filter_list.empty()) {
          line << filter_highlight_url(info.url, filter_list);
        } else {
          line << info.url;
        }

        if (ser_mode) {
          line << std::string(space_cnt, ' ');
          line << info.ser_type;

          space_cnt = max_ser_size - info.ser_type.size() + 3;

          if VUNLIKELY (space_cnt < 3) {
            space_cnt = 3;
          }
        }

        line << std::string(space_cnt, ' ');

        line << "---";
        line << std::string(9, ' ');
        line << "---";
        line << std::string(9, ' ');
        line << "---";
        line << std::string(6, ' ');
        line << "---";
        line << std::string(7, ' ');

        if (profiler_mode) {
          line << std::string(2, ' ');
          process_profiler_func();
          line << std::string(1, ' ');
        }

        line << "\033[0m";

        if (!is_paused) {
          current_info_list.emplace_back(info);
          print_lines.emplace_back(line.str());
        }

        continue;
      }

      if VUNLIKELY (!elapsed.is_active()) {
        elapsed.start();
      }

      if (auto retry_iter = sub_retry_after_map.find(info.url); retry_iter != sub_retry_after_map.end()) {
        if (vlink::ElapsedTimer::get_cpu_timestamp(vlink::ElapsedTimer::kNano) < retry_iter->second) {
          continue;
        }

        sub_retry_after_map.erase(retry_iter);
      }

      auto ptr_iter = sub_ptr_map.find(info.url);

      if VLIKELY (ptr_iter != sub_ptr_map.end()) {
        auto* sub_ptr = ptr_iter->second.get();

        const auto sub_current_schema_type = sub_ptr ? sub_ptr->get_schema_type() : vlink::SchemaType::kUnknown;
        const auto sub_expected_schema_type =
            info.schema_type == vlink::SchemaType::kUnknown ? sub_current_schema_type : info.schema_type;

        if VUNLIKELY (sub_ptr && (sub_ptr->get_ser_type() != info.ser_type ||
                                  sub_current_schema_type != sub_expected_schema_type)) {
          sub_ptr_map.erase(ptr_iter);
          ptr_iter = sub_ptr_map.end();
        }
      }

      if VUNLIKELY (ptr_iter == sub_ptr_map.end()) {
        std::shared_ptr<RawSub> sub;

        try {
          sub = std::make_shared<RawSub>(info.url, vlink::InitType::kWithoutInit);

          sub->set_safety_quit(true);
          sub->set_latency_and_lost_enabled(true);

          if (native_mode) {
            sub->set_property("dds.ip", native_ip);
          }

          sub->set_discovery_enabled(false);
          sub->set_ser_type(info.ser_type, info.schema_type);

          sub->init();

          sub->listen([sub_ptr = sub.get(), &discovery_viewer, &seq, &size, &lat, &elapsed](const vlink::Bytes& bytes) {
            if VUNLIKELY (has_quit || discovery_viewer->is_ready_to_quit()) {
              return;
            }

            if VUNLIKELY (is_jumped) {
              return;
            }

            ++seq;
            size += bytes.size();
            lat += sub_ptr->get_latency();
            elapsed.restart();
          });

          sub_ptr_map.emplace(info.url, std::move(sub));
        } catch (const std::runtime_error&) {
          sub_retry_after_map[info.url] =
              vlink::ElapsedTimer::get_cpu_timestamp(vlink::ElapsedTimer::kNano) + kSubscriberRetryDelayNs;
          seq = 0;
          size = 0;
          lost = 0;
          lat = 0;
          seq_buffer.clear();
          size_buffer.clear();
          lost_buffer.clear();
          lat_buffer.clear();
          spark_history.clear();

          elapsed.stop();

          continue;
        }
      } else if (collect_sample) {
        auto& last_sample = sub_last_sample_map[info.url];

        const auto& sample_info = ptr_iter->second->get_lost();

        int64_t total_sample = sample_info.total - last_sample.total;
        int64_t lost_sample = sample_info.lost - last_sample.lost;

        if (total_sample > 0 && lost_sample > 0) {
          lost = static_cast<double>(lost_sample) / total_sample;
        } else {
          lost = 0;
        }

        last_sample = sample_info;
      }

      int64_t sample_seq = seq_buffer.empty() ? 0 : seq_buffer.back();
      size_t sample_size = 0;
      int64_t sample_latency = 0;

      if (collect_sample) {
        sample_seq = seq.exchange(0, std::memory_order_relaxed);
        sample_size = size.exchange(0, std::memory_order_relaxed);
        sample_latency = lat.exchange(0, std::memory_order_relaxed);
      }

      if (sample_seq > 0 && seq_buffer.size() >= kCounterCache && size_buffer.size() >= kCounterCache) {
        line << "\033[32m";  // green
        ++active_cnt;
      } else {
        if (elapsed.get() > kCollectInterval * kCounterCache) {
          sample_seq = 0;
          sample_size = 0;
          lost = 0;
          sample_latency = 0;
          seq_buffer.clear();
          size_buffer.clear();
          lost_buffer.clear();
          lat_buffer.clear();

          if (active_mode) {
            continue;
          }

          line << "\033[31m";  // red
        } else {
          line << "\033[33m";  // yellow
        }
      }

      if (count_mode) {
        line << vlink::DiscoveryViewer::convert_type_to_view(info.type, info.process_list);
      } else {
        line << vlink::DiscoveryViewer::convert_type_to_view(info.type);
      }

      line << std::string(3, ' ');

      if (!filter_list.empty()) {
        line << filter_highlight_url(info.url, filter_list);
      } else {
        line << info.url;
      }

      if (ser_mode) {
        line << std::string(space_cnt, ' ');
        line << info.ser_type;

        space_cnt = max_ser_size - info.ser_type.size() + 3;

        if VUNLIKELY (space_cnt < 3) {
          space_cnt = 3;
        }
      }

      line << std::string(space_cnt, ' ');

      {
        double freq = 0;
        double rate = 0;
        double loss = 0;
        double latency = 0;
        int weight = 1;
        int total_weight = 0;

        if (collect_sample) {
          seq_buffer.emplace_back(sample_seq);
          while (seq_buffer.size() > kCounterCache) {
            seq_buffer.pop_front();
          }

          size_buffer.emplace_back(sample_size);
          while (size_buffer.size() > kCounterCache) {
            size_buffer.pop_front();
          }

          lost_buffer.emplace_back(lost);
          while (lost_buffer.size() > kCounterCache) {
            lost_buffer.pop_front();
          }

          if (sample_seq <= 0) {
            lat_buffer.emplace_back(sample_latency);
          } else {
            lat_buffer.emplace_back(static_cast<double>(sample_latency) / sample_seq);
          }

          while (lat_buffer.size() > kCounterCache) {
            lat_buffer.pop_front();
          }
        }

        if VLIKELY (seq_buffer.size() == size_buffer.size()) {
          for (size_t i = 0; i < seq_buffer.size(); ++i) {
            freq += seq_buffer[i] * weight;
            rate += size_buffer[i] * weight;
            loss += lost_buffer[i] * weight;
            latency += lat_buffer[i] * weight;
            total_weight += weight;
            weight *= kCounterWeight;
          }
        }

        if VLIKELY (total_weight > 0) {
          freq = freq / total_weight;
          rate = rate / total_weight;
          loss = loss / total_weight;
          latency = latency / total_weight;
        } else {
          freq = 0;
          rate = 0;
          loss = 0;
          latency = 0;
        }

        if VUNLIKELY (loss > 1) {
          loss = 0;
        }

        double latency_ms = latency / 1000'000;

        if (latency_ms < 0 || latency_ms > 5000) {
          latency_ms = 0;
        }

        if (collect_sample) {
          spark_history.add_sample(freq, rate, latency_ms, loss * 100);
        }

        std::string seq_str = vlink::Helpers::double_to_string(freq) + "Hz";

        line << seq_str;

        space_cnt = 12 - seq_str.size();

        if VUNLIKELY (space_cnt < 1) {
          space_cnt = 1;
        }

        line << std::string(space_cnt, ' ');

        total_rate += rate;

        std::string rate_str = vlink::Helpers::format_rate_size(rate);

        line << rate_str;

        space_cnt = 12 - rate_str.size();

        if VUNLIKELY (space_cnt < 1) {
          space_cnt = 1;
        }

        line << std::string(space_cnt, ' ');

        std::string loss_str = vlink::Helpers::double_to_string(loss * 100) + "%";

        line << loss_str;

        space_cnt = 9 - loss_str.size();

        if VUNLIKELY (space_cnt < 1) {
          space_cnt = 1;
        }

        line << std::string(space_cnt, ' ');

        std::string latency_str;

        if (sample_seq == 0) {
          latency_str = "---";
        } else if (latency > 5000'000'000 || latency < -500'000) {
          latency_str = "N/A";
        } else if (latency < 0) {
          latency_str = "0.00ms";
        } else {
          latency_str = vlink::Helpers::double_to_string(latency / 1000'000, 2) + "ms";
        }

        line << latency_str;

        if (profiler_mode) {
          space_cnt = 12 - latency_str.size();

          if VUNLIKELY (space_cnt < 1) {
            space_cnt = 1;
          }

          line << std::string(space_cnt, ' ');

          process_profiler_func();
          line << std::string(1, ' ');
        } else {
          space_cnt = 10 - latency_str.size();

          if VUNLIKELY (space_cnt < 1) {
            space_cnt = 1;
          }

          line << std::string(space_cnt, ' ');
        }

        line << "\033[0m";
      }

      if (!is_paused) {
        current_info_list.emplace_back(info);
        print_lines.emplace_back(line.str());
      }
    }

    if (!is_paused && !selected_url.empty()) {
      auto selected_info = std::find_if(current_info_list.begin(), current_info_list.end(),
                                        [&selected_url](const auto& info) { return info.url == selected_url; });

      if (selected_info == current_info_list.end()) {
        selected_line = -1;
      } else {
        selected_line = static_cast<int>(std::distance(current_info_list.begin(), selected_info));

        if (target_row > 0) {
          current_page = selected_line / target_row;
        }
      }
    }

    print_lines_count.store(print_lines.size(), std::memory_order_release);
  };

  vlink::Timer update_timer;
  update_timer.set_interval(kCollectInterval);
  update_timer.set_loop_count(vlink::Timer::kInfinite);
  update_timer.attach(discovery_viewer.get());
  update_timer.set_callback([&update_function, &print_function, &update_meta_function]() {
    update_function(true);
    print_function(true);
    update_meta_function();
  });
  update_timer.start();

  vlink::Timer terminal_timer;
  terminal_timer.set_interval(kTerminalInterval);
  terminal_timer.set_loop_count(vlink::Timer::kInfinite);
  terminal_timer.attach(discovery_viewer.get());
  terminal_timer.set_callback([&update_terminal_function]() { update_terminal_function(true); });
  terminal_timer.start();

  auto sub_command_function = [&proto_dir, &fbs_dir](std::string& executable,
                                                     std::vector<std::string>& command_args) -> bool {
    uint32_t selected_type = 0;
    vlink::SchemaType selected_schema_type = vlink::SchemaType::kUnknown;
    std::string selected_url;
    std::string selected_ser;

    {
      std::lock_guard lock(current_mtx);
      selected_type = current_type.load(std::memory_order_relaxed);
      selected_schema_type = static_cast<vlink::SchemaType>(current_schema_type.load(std::memory_order_relaxed));
      selected_url = current_url;
      selected_ser = current_ser;
    }

    if VUNLIKELY (selected_url.empty() || selected_url.front() == '-' ||
                  (!selected_ser.empty() && selected_ser.front() == '-')) {
      std::cerr << "Unable to use invalid discovery metadata." << std::endl;
      return false;
    }

    if VUNLIKELY (selected_type & vlink::kServer || selected_type & vlink::kClient) {
      std::cerr << "Unable to parse Server/Client data." << std::endl;
      return false;
    } else if VUNLIKELY (!has_intra_bind && vlink::Url::is_intra_type(selected_url)) {
      std::cerr << "Unable to parse intra url." << std::endl;
      return false;
    }

    bool should_use_getter = ((selected_type & vlink::kGetter) != 0) ||
                             (((selected_type & vlink::kSetter) != 0) && ((selected_type & vlink::kPublisher) == 0));

    auto command_schema_type = selected_schema_type;

    if (blob_mode && command_schema_type == vlink::SchemaType::kUnknown) {
      command_schema_type = vlink::SchemaData::infer_ser_type(selected_ser);
    }

    if (command_schema_type == vlink::SchemaType::kUnknown && !blob_mode) {
      std::cerr << "Unable to determine schema_type for url: " << selected_url
                << ". Wait for discovery metadata and retry." << std::endl;
      return false;
    }

    if (selected_ser.empty() && !blob_mode) {
      std::cerr << "Unable to determine ser_type for url: " << selected_url
                << ". Wait for discovery metadata and retry." << std::endl;
      return false;
    }

    std::string schema_label;

    if (!blob_mode) {
      schema_label = vlink::SchemaData::convert_type(command_schema_type);

      if (schema_label.empty()) {
        std::cerr << "Unable to determine schema_type for url: " << selected_url << "." << std::endl;
        return false;
      }
    } else if (command_schema_type == vlink::SchemaType::kUnknown) {
      command_schema_type = vlink::SchemaType::kRaw;
    }

    if (command_schema_type == vlink::SchemaType::kProtobuf || command_schema_type == vlink::SchemaType::kZeroCopy ||
        command_schema_type == vlink::SchemaType::kRaw) {
#ifdef _WIN32
      executable = vlink::Utils::get_app_dir() + "/vlink-eproto.exe";
#else
      executable = vlink::Utils::get_app_dir() + "/vlink-eproto";
#endif

      command_args = {executable, "sub", selected_url};

      if (!selected_ser.empty()) {
        command_args.emplace_back("-s");
        command_args.emplace_back(selected_ser);
      }

      if (!proto_dir.empty()) {
        command_args.emplace_back("-d");
        command_args.emplace_back(proto_dir);
      }
    } else if (command_schema_type == vlink::SchemaType::kFlatbuffers) {
#ifdef _WIN32
      executable = vlink::Utils::get_app_dir() + "/vlink-efbs.exe";
#else
      executable = vlink::Utils::get_app_dir() + "/vlink-efbs";
#endif

      command_args = {executable, "sub", selected_url};

      if (!selected_ser.empty()) {
        command_args.emplace_back("-s");
        command_args.emplace_back(selected_ser);
      }

      if (!fbs_dir.empty()) {
        command_args.emplace_back("-d");
        command_args.emplace_back(fbs_dir);
      }
    } else {
      std::cerr << "Unable to build decoder command for url: " << selected_url << "." << std::endl;
      return false;
    }

    if (should_use_getter) {
      command_args.emplace_back("-g");
    }

    command_args.emplace_back("-x");

    if (blob_mode) {
      command_args.emplace_back("blob");
    } else {
      command_args.emplace_back(schema_label);
    }

    command_args.emplace_back("-e");
    command_args.emplace_back("-y");

    if (native_mode) {
      command_args.emplace_back("-n");
    }

    if (max_columns > 0) {
      command_args.emplace_back("--columns");
      command_args.emplace_back(std::to_string(max_columns));
    }

    if (max_rows > 0) {
      command_args.emplace_back("--rows");
      command_args.emplace_back(std::to_string(max_rows));
    }

    if (!proto_args.empty() && !append_command_arguments(proto_args, command_args)) {
      std::cerr << "Unable to parse proto_args: unmatched quote." << std::endl;
      return false;
    }

    return true;
  };

  auto quit_function = [&discovery_viewer](int) {
    if VUNLIKELY (has_quit) {
      return;
    }

    has_quit = true;

    if VLIKELY (discovery_viewer) {
      discovery_viewer->quit(true);
    }
  };

  auto apply_filter_input_function = [&filter_list, &update_function, &print_function, &clear_function]() {
    filter_list = vlink::Helpers::split_any(filter_input_text);
    selected_line = -1;
    current_page = 0;

    clear_function();
    update_function();
    print_function(false);
  };

  auto detect_keyboard_function = [&discovery_viewer, &update_meta_function, &quit_function, &print_function,
                                   &update_timer, &terminal_timer, &update_function, &clear_function,
                                   &key_elapsed_timer, &sub_command_function,
                                   &apply_filter_input_function](const std::string& key) {
    if (plain_mode) {
      if (key == "esc" || key == "q") {
        quit_function(0);

        return;
      }

      return;
    }

    key_elapsed_timer.restart();

    if (!discovery_viewer) {
      return;
    }

    if (filter_input_mode) {
      if (key == "enter" || key == "esc") {
        filter_input_mode = false;

        discovery_viewer->post_keyboard_task([&print_function]() { print_function(false); });
      } else if (key == "backspace") {
        discovery_viewer->post_keyboard_task([&apply_filter_input_function]() {
          while (!filter_input_text.empty() && (static_cast<unsigned char>(filter_input_text.back()) & 0xC0) == 0x80) {
            filter_input_text.pop_back();
          }

          if (!filter_input_text.empty()) {
            filter_input_text.pop_back();
          }

          apply_filter_input_function();
        });
      } else if (key.size() == 1) {
        discovery_viewer->post_keyboard_task([key, &apply_filter_input_function]() {
          filter_input_text += key;

          apply_filter_input_function();
        });
      }

      return;
    }

    if (is_jumped) {
      return;
    }

    if (key == "esc" || key == "q") {
      quit_function(0);
    } else if (key == " ") {
      discovery_viewer->post_keyboard_task([&print_function]() {
        is_paused = !is_paused;
        print_function(false);
      });
    } else if (key == "left") {
      discovery_viewer->post_keyboard_task([&update_meta_function, &print_function]() {
        if (current_page >= 1) {
          --current_page;
          selected_line = -1;
        }

        print_function(false);
        update_meta_function();
      });
    } else if (key == "right") {
      discovery_viewer->post_keyboard_task([&update_meta_function, &print_function]() {
        if (current_page < total_pages - 1) {
          ++current_page;
          selected_line = -1;
        }

        print_function(false);
        update_meta_function();
      });
    } else if (key == "up") {
      discovery_viewer->post_keyboard_task([&update_meta_function, &print_function]() {
        if (selected_line < 0) {
          selected_line = ((current_page + 1) * target_row) - 1;

          if (selected_line < 0) {
            selected_line = 0;
          } else if (selected_line > row_count - 1) {
            selected_line = row_count - 1;
          }

          print_function(false);
        } else if (selected_line == current_page * target_row && current_page > 0) {
          --current_page;
          int start_index = current_page * target_row;
          int end_index =
              std::min(start_index + target_row, static_cast<int>(print_lines_count.load(std::memory_order_acquire)));
          selected_line = end_index - 1;

          print_function(false);
        } else {
          selected_line = std::max(selected_line - 1, 0);
          print_function(false);
          update_meta_function();
        }
      });
    } else if (key == "down") {
      discovery_viewer->post_keyboard_task([&update_meta_function, &print_function]() {
        int start_index = current_page * target_row;
        int end_index =
            std::min(start_index + target_row, static_cast<int>(print_lines_count.load(std::memory_order_acquire)));

        if (selected_line < 0) {
          selected_line = current_page * target_row;

          print_function(false);
        } else if (selected_line == end_index - 1 && current_page < total_pages - 1) {
          ++current_page;
          start_index = current_page * target_row;
          selected_line = start_index;

          print_function(false);
        } else {
          selected_line =
              std::min(selected_line + 1, static_cast<int>(print_lines_count.load(std::memory_order_acquire)) - 1);
          print_function(false);
          update_meta_function();
        }
      });
    } else if (key == "enter") {
      is_jumped = true;

      if (!discovery_viewer->post_keyboard_task([&discovery_viewer, &update_timer, &terminal_timer, &print_function,
                                                 &update_function, &update_meta_function, &clear_function,
                                                 &sub_command_function]() {
            if (selected_line < 0 ||
                selected_line >= static_cast<int>(print_lines_count.load(std::memory_order_acquire))) {
              is_jumped = false;
              return;
            }

            // update_function();

            update_meta_function();

            update_timer.stop();
            terminal_timer.stop();
            vlink::Utils::stop_detect_keyboard();

            clear_function();

            VLINK_TERM_OUT << "\033[H\033[J";
            VLINK_TERM_OUT.flush();

            int ret = 0;
            std::string executable;
            std::vector<std::string> command_args;

            if VLIKELY (sub_command_function(executable, command_args)) {
              ret = run_decoder_process(executable, command_args);
            } else {
              ret = -1;
            }

            VLINK_TERM_OUT << "\033[?25l";
            VLINK_TERM_OUT.flush();

            if (ret == 0) {
              is_jumped = false;
              update_function();
              print_function(false);
            } else {
              vlink::Timer::call_once(discovery_viewer.get(), 2000, [&print_function, &update_function]() {
                is_jumped = false;
                update_function();
                print_function(false);
              });
            }

            update_timer.start();
            terminal_timer.start();
            vlink::Utils::start_detect_keyboard();
          })) {
        is_jumped = false;
      }
    } else if (key == "z") {
      discovery_viewer->post_keyboard_task([&print_function]() {
        if (selected_line >= 0) {
          selected_line = -1;
          print_function(false);
        }
      });
    } else if (key == "t") {
      discovery_viewer->post_keyboard_task([&print_function, &update_function]() {
        count_mode = !count_mode;
        update_function();
        print_function(false);
      });
    } else if (key == "l") {
      discovery_viewer->post_keyboard_task([&print_function, &update_function]() {
        detail_mode = !detail_mode;
        update_function();
        print_function(false);
      });
    } else if (key == "o") {
      discovery_viewer->post_keyboard_task([&print_function, &update_function]() {
        observe_all_mode = !observe_all_mode;
        update_function();
        print_function(false);
      });
    } else if (key == "e") {
      discovery_viewer->post_keyboard_task([&print_function, &update_function]() {
        profiler_mode = !profiler_mode;
        update_function();
        print_function(false);
      });
    } else if (key == "s") {
      discovery_viewer->post_keyboard_task([&print_function, &update_function]() {
        ser_mode = !ser_mode;
        update_function();
        print_function(false);
      });
    } else if (key == "a") {
      discovery_viewer->post_keyboard_task([&print_function, &update_function]() {
        active_mode = !active_mode;
        update_function();
        print_function(false);
      });
    } else if (key == "y") {
      discovery_viewer->post_keyboard_task([&print_function, &update_function]() {
        pubsub_mode = !pubsub_mode;
        update_function();
        print_function(false);
      });
    } else if (key == "p") {
      discovery_viewer->post_keyboard_task([&print_function, &update_function]() {
        process_mode = !process_mode;
        update_function();
        print_function(false);
      });
    } else if (key == "c") {
      discovery_viewer->post_keyboard_task([&print_function, &update_function]() {
        chart_mode = !chart_mode;
        update_function();
        print_function(false);
      });
    } else if (key == "i") {
      filter_input_mode = true;

      discovery_viewer->post_keyboard_task([&print_function]() { print_function(false); });
    }
  };

  vlink::Utils::register_terminate_signal(quit_function);

  vlink::Utils::start_detect_keyboard(detect_keyboard_function);

  discovery_viewer->run();

  has_quit = true;

  vlink::Utils::stop_detect_keyboard();

  if (!plain_mode) {
    VLINK_TERM_OUT << "\033[H\033[J";
    VLINK_TERM_OUT.flush();
  }

  clear_function();

  discovery_viewer.reset();

  return 0;
}
