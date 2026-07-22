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

#include <vlink/base/elapsed_timer.h>
#include <vlink/base/helpers.h>
#include <vlink/base/terminal_stream.h>
#include <vlink/base/utils.h>
#include <vlink/extension/discovery_viewer.h>
#include <vlink/version.h>
#include <vlink/vlink.h>

#include <argparse/argparse.hpp>
//
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if __has_include(<unistd.h>)
#include <unistd.h>
#endif

#ifndef _WIN32
#include <spawn.h>
#include <sys/wait.h>
#endif

#ifdef __APPLE__
#include <crt_externs.h>
#endif

#ifdef _WIN32
#include <Windows.h>
#undef min
#undef max
#undef GetMessage
[[maybe_unused]] static constexpr int kFlushMinSleep{0};
[[maybe_unused]] static constexpr int kFlushMinLine{5};
#elif defined(__APPLE__)
[[maybe_unused]] static constexpr int kFlushMinSleep{50};
[[maybe_unused]] static constexpr int kFlushMinLine{1};
#else
[[maybe_unused]] static constexpr int kFlushMinSleep{50};
[[maybe_unused]] static constexpr int kFlushMinLine{5};

extern char** environ;
#endif

[[maybe_unused]] static constexpr int kCounterCache{2};
[[maybe_unused]] static constexpr int kCounterWeight{2};
[[maybe_unused]] static constexpr int kCollectInterval{1000};
[[maybe_unused]] static constexpr int kTerminalInterval{50};
[[maybe_unused]] static constexpr int kMaxElapsedTime{200};
[[maybe_unused]] static constexpr int kChartHeight{30};
[[maybe_unused]] static constexpr uint64_t kSubscriberRetryDelayNs{5'000'000'000ULL};

static bool append_command_arguments(const std::string& text, std::vector<std::string>& args) {
  std::string current;
  char quote = '\0';
  bool has_token = false;

  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];

    if (quote != '\0') {
      if (c == quote) {
        quote = '\0';
      } else if (c == '\\' && quote == '"' && i + 1 < text.size() && text[i + 1] == '"') {
        current.push_back(text[++i]);
      } else {
        current.push_back(c);
      }

      has_token = true;
      continue;
    }

    if (c == '\'' || c == '"') {
      quote = c;
      has_token = true;
    } else if (std::isspace(static_cast<unsigned char>(c))) {
      if (has_token) {
        args.emplace_back(std::move(current));
        current.clear();
        has_token = false;
      }
    } else if (c == '\\' && i + 1 < text.size() &&
               (std::isspace(static_cast<unsigned char>(text[i + 1])) || text[i + 1] == '\'' || text[i + 1] == '"')) {
      current.push_back(text[++i]);
      has_token = true;
    } else {
      current.push_back(c);
      has_token = true;
    }
  }

  if (quote != '\0') {
    return false;
  }

  if (has_token) {
    args.emplace_back(std::move(current));
  }

  return true;
}

static int run_decoder_process(const std::string& executable, const std::vector<std::string>& args) {
#ifdef _WIN32
  std::wstring command_line;

  auto append_quoted_argument = [&command_line](const std::wstring& argument) {
    if (!command_line.empty()) {
      command_line.push_back(L' ');
    }

    command_line.push_back(L'"');
    size_t backslash_count = 0;

    for (const wchar_t c : argument) {
      if (c == L'\\') {
        ++backslash_count;
      } else if (c == L'"') {
        command_line.append(backslash_count * 2 + 1, L'\\');
        command_line.push_back(c);
        backslash_count = 0;
      } else {
        command_line.append(backslash_count, L'\\');
        command_line.push_back(c);
        backslash_count = 0;
      }
    }

    command_line.append(backslash_count * 2, L'\\');
    command_line.push_back(L'"');
  };

  const auto wide_executable = vlink::Helpers::string_to_wstring(executable);

  for (const auto& arg : args) {
    append_quoted_argument(vlink::Helpers::string_to_wstring(arg));
  }

  STARTUPINFOW startup_info{};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESTDHANDLES;
  const HANDLE standard_handles[] = {::GetStdHandle(STD_INPUT_HANDLE), ::GetStdHandle(STD_OUTPUT_HANDLE),
                                     ::GetStdHandle(STD_ERROR_HANDLE)};
  HANDLE inherited_handles[3]{};

  for (size_t i = 0; i < 3; ++i) {
    if (standard_handles[i] == nullptr || standard_handles[i] == INVALID_HANDLE_VALUE ||
        !::DuplicateHandle(::GetCurrentProcess(), standard_handles[i], ::GetCurrentProcess(), &inherited_handles[i], 0,
                           TRUE, DUPLICATE_SAME_ACCESS)) {
      for (size_t j = 0; j < i; ++j) {
        ::CloseHandle(inherited_handles[j]);
      }

      return -1;
    }
  }

  startup_info.hStdInput = inherited_handles[0];
  startup_info.hStdOutput = inherited_handles[1];
  startup_info.hStdError = inherited_handles[2];
  PROCESS_INFORMATION process_info{};

  const bool created = ::CreateProcessW(wide_executable.c_str(), command_line.data(), nullptr, nullptr, TRUE, 0,
                                        nullptr, nullptr, &startup_info, &process_info);

  for (const HANDLE handle : inherited_handles) {
    ::CloseHandle(handle);
  }

  if (!created) {
    return -1;
  }

  ::CloseHandle(process_info.hThread);

  const DWORD wait_result = ::WaitForSingleObject(process_info.hProcess, INFINITE);
  DWORD exit_code = 0;
  const bool exited = wait_result == WAIT_OBJECT_0 && ::GetExitCodeProcess(process_info.hProcess, &exit_code);
  ::CloseHandle(process_info.hProcess);

  return exited ? static_cast<int>(exit_code) : -1;
#else
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);

  for (const auto& arg : args) {
    argv.emplace_back(const_cast<char*>(arg.c_str()));
  }

  argv.emplace_back(nullptr);

  struct sigaction ignore_action{};
  struct sigaction old_int_action{};
  struct sigaction old_quit_action{};
  ignore_action.sa_handler = SIG_IGN;
  sigemptyset(&ignore_action.sa_mask);

  if (sigaction(SIGINT, &ignore_action, &old_int_action) != 0) {
    return -1;
  }

  if (sigaction(SIGQUIT, &ignore_action, &old_quit_action) != 0) {
    sigaction(SIGINT, &old_int_action, nullptr);
    return -1;
  }

  posix_spawnattr_t attr;

  if (posix_spawnattr_init(&attr) != 0) {
    sigaction(SIGINT, &old_int_action, nullptr);
    sigaction(SIGQUIT, &old_quit_action, nullptr);
    return -1;
  }

  sigset_t child_default_signals;
  sigemptyset(&child_default_signals);
  sigaddset(&child_default_signals, SIGINT);
  sigaddset(&child_default_signals, SIGQUIT);

  if (posix_spawnattr_setsigdefault(&attr, &child_default_signals) != 0 ||
      posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF) != 0) {
    posix_spawnattr_destroy(&attr);
    sigaction(SIGINT, &old_int_action, nullptr);
    sigaction(SIGQUIT, &old_quit_action, nullptr);
    return -1;
  }

  pid_t pid = -1;
#ifdef __APPLE__
  char** environment = *_NSGetEnviron();
#else
  char** environment = environ;
#endif
  const int spawn_error = posix_spawn(&pid, executable.c_str(), nullptr, &attr, argv.data(), environment);
  posix_spawnattr_destroy(&attr);

  if (spawn_error != 0) {
    sigaction(SIGINT, &old_int_action, nullptr);
    sigaction(SIGQUIT, &old_quit_action, nullptr);
    return -1;
  }

  int status = 0;

  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      sigaction(SIGINT, &old_int_action, nullptr);
      sigaction(SIGQUIT, &old_quit_action, nullptr);
      return -1;
    }
  }

  sigaction(SIGINT, &old_int_action, nullptr);
  sigaction(SIGQUIT, &old_quit_action, nullptr);

  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }

  return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
#endif
}

[[maybe_unused]] static std::atomic_bool has_quit{false};

std::atomic_bool has_intra_bind{false};
std::atomic_bool is_paused{false};
std::atomic_bool is_jumped{false};
std::atomic_bool has_update{false};
std::atomic<int> current_page{0};
std::atomic<int> total_pages{1};
std::atomic<size_t> max_url_size{10};
std::atomic<size_t> max_ser_size{10};
std::atomic<int> selected_line{-1};
std::atomic<int> target_row{0};
std::atomic<int> row_count{0};
std::atomic<int> chart_width{30};
std::atomic<int> process_width{40};
std::atomic_bool count_mode{false};
std::atomic_bool black_mode{false};
std::atomic_bool blob_mode{false};
std::atomic_bool native_mode{false};
std::atomic_bool detail_mode{false};
std::atomic_bool observe_all_mode{false};
std::atomic_bool profiler_mode{false};
std::atomic_bool ser_mode{false};
std::atomic_bool active_mode{false};
std::atomic_bool pubsub_mode{false};
std::atomic_bool plain_mode{false};
std::atomic_bool process_mode{false};
std::atomic_bool chart_mode{false};
std::atomic_bool preset_mode{false};
std::atomic_bool filter_input_mode{false};
std::atomic_bool use_chart_dot{false};
std::atomic<int> max_rows{0};
std::atomic<int> max_columns{0};
std::atomic<double> total_profiler{-1};
std::atomic<uint32_t> current_type;
std::atomic<uint32_t> current_schema_type{0};

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
std::pair<int, int> terminal_size{0, 0};
std::vector<std::string> print_lines;
std::atomic<size_t> print_lines_count{0};
std::vector<vlink::DiscoveryViewer::Info> current_info_list;

std::mutex current_mtx;         // NOLINT(runtime/string)
std::string current_url;        // NOLINT(runtime/string)
std::string current_ser;        // NOLINT(runtime/string)
std::string proto_args;         // NOLINT(runtime/string)
std::string filter_input_text;  // NOLINT(runtime/string)

[[maybe_unused]] static std::pair<int, int> get_terminal_size() {
  auto size = vlink::Utils::get_terminal_size();

  if (max_columns > 0) {
    size.first = max_columns;
  }

  if (max_rows > 0) {
    size.second = max_rows;
  }

  if (size.first < 0) {
    size.first = 1000;
  }

  if (size.second < 0) {
    size.second = 25;
  }

  return size;
}

[[maybe_unused]] static int filter_box_display_width(const std::string& line) {
  int col = 0;
  size_t i = 0;

  while (i < line.size()) {
    if (line[i] == '\033') {
      ++i;

      if (i < line.size() && line[i] == '[') {
        ++i;

        while (i < line.size() && !(line[i] >= '@' && line[i] <= '~')) {
          ++i;
        }

        if (i < line.size()) {
          ++i;
        }
      }

      continue;
    }

    ++i;

    while (i < line.size() && (static_cast<unsigned char>(line[i]) & 0xC0) == 0x80) {
      ++i;
    }

    ++col;
  }

  return col;
}

[[maybe_unused]] static size_t filter_box_col_to_byte(const std::string& line, int target_col) {
  int col = 0;
  size_t i = 0;

  while (i < line.size() && col < target_col) {
    if (line[i] == '\033') {
      ++i;

      if (i < line.size() && line[i] == '[') {
        ++i;

        while (i < line.size() && !(line[i] >= '@' && line[i] <= '~')) {
          ++i;
        }

        if (i < line.size()) {
          ++i;
        }
      }

      continue;
    }

    ++i;

    while (i < line.size() && (static_cast<unsigned char>(line[i]) & 0xC0) == 0x80) {
      ++i;
    }

    ++col;
  }

  return i;
}

[[maybe_unused]] static std::string filter_box_sgr_prefix(const std::string& line, size_t limit) {
  std::string codes;
  size_t i = 0;

  while (i < line.size() && i < limit) {
    if (line[i] != '\033') {
      ++i;

      continue;
    }

    size_t start = i;
    ++i;

    if (i < line.size() && line[i] == '[') {
      ++i;

      while (i < line.size() && !(line[i] >= '@' && line[i] <= '~')) {
        ++i;
      }

      if (i < line.size()) {
        ++i;
      }
    }

    codes += line.substr(start, i - start);
  }

  return codes;
}

[[maybe_unused]] static std::string filter_highlight_url(const std::string& url,
                                                         const std::vector<std::string>& terms) {
  std::string lower = url;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  std::vector<bool> mark(url.size(), false);
  bool any = false;

  for (const auto& term : terms) {
    if (term.empty()) {
      continue;
    }

    std::string needle = term;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    size_t pos = lower.find(needle);

    while (pos != std::string::npos) {
      for (size_t i = pos; i < pos + needle.size(); ++i) {
        mark[i] = true;
      }

      any = true;
      pos = lower.find(needle, pos + 1);
    }
  }

  if (!any) {
    return url;
  }

  std::string out;
  out.reserve(url.size() + 16);

  bool active = false;

  for (size_t i = 0; i < url.size(); ++i) {
    if (mark[i] && !active) {
      out += "\033[1;4m";
      active = true;
    } else if (!mark[i] && active) {
      out += "\033[22;24m";
      active = false;
    }

    out += url[i];
  }

  if (active) {
    out += "\033[22;24m";
  }

  return out;
}

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

class CustomDiscoveryViewer : public vlink::DiscoveryViewer {
 public:
  using DiscoveryViewer::DiscoveryViewer;

 protected:
  uint32_t get_max_elapsed_time() const override { return kMaxElapsedTime; }
};

// NOLINTNEXTLINE(google-readability-function-size)
int start_monitor(const std::vector<std::string>& urls, const std::string& filter, const std::string& proto_dir,
                  const std::string& fbs_dir) {
  using RawSub = vlink::Subscriber<vlink::Bytes>;

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

  auto update_function = [&target_urls_set, &filter_list, &discovery_viewer, &sub_ptr_map, &sub_seq_map, &sub_size_map,
                          &sub_lost_map, &sub_lat_map, &sub_elapsed_map, &sub_seq_buffer_map, &sub_size_buffer_map,
                          &sub_lost_buffer_map, &sub_lat_buffer_map, &sub_last_sample_map, &sparkline_history_map,
                          &sub_retry_after_map, &clear_function, &active_cnt, &total_rate, &key_elapsed_timer]() {
    total_profiler = -1;
    active_cnt = 0;
    total_rate = 0;

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
        std::transform(left_str.begin(), left_str.end(), left_str.begin(), [](char& c) { return std::tolower(c); });
        for (const auto& f : filter_list) {
          if (f.empty()) {
            continue;
          }

          std::string right_str = f;
          std::transform(right_str.begin(), right_str.end(), right_str.begin(),
                         [](char& c) { return std::tolower(c); });

          if (left_str.find(right_str) != std::string::npos) {
            skip = black_mode ? true : false;
            break;
          }
        }

        if (skip) {
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
          (!observe_all_mode &&
           (selected_line != static_cast<int>(print_lines.size()) || key_elapsed_timer.get() < 250))) {
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

        if (sub_ptr &&
            (sub_ptr->get_ser_type() != info.ser_type || sub_current_schema_type != sub_expected_schema_type)) {
          sub_ptr->set_ser_type(info.ser_type, info.schema_type);
        }
      }

      if VUNLIKELY (ptr_iter == sub_ptr_map.end()) {
        std::shared_ptr<RawSub> sub;

        try {
          sub = std::make_shared<RawSub>(info.url, vlink::InitType::kWithoutInit);

          sub->set_safety_quit(true);
          sub->set_latency_and_lost_enabled(true);

          if (native_mode) {
            sub->set_property("dds.ip", "127.0.0.1");
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
        } catch (vlink::Exception::RuntimeError&) {
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
      } else {
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

      if (seq > 0 && seq_buffer.size() >= kCounterCache && size_buffer.size() >= kCounterCache) {
        line << "\033[32m";  // green
        ++active_cnt;
      } else {
        if (elapsed.get() > kCollectInterval * kCounterCache) {
          seq = 0;
          size = 0;
          lost = 0;
          lat = 0;
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

        seq_buffer.emplace_back(seq);
        while (seq_buffer.size() > kCounterCache) {
          seq_buffer.pop_front();
        }

        size_buffer.emplace_back(size);
        while (size_buffer.size() > kCounterCache) {
          size_buffer.pop_front();
        }

        lost_buffer.emplace_back(lost);
        while (lost_buffer.size() > kCounterCache) {
          lost_buffer.pop_front();
        }

        if (seq <= 0) {
          lat_buffer.emplace_back(lat);
        } else {
          lat_buffer.emplace_back(static_cast<double>(lat) / seq);
        }

        while (lat_buffer.size() > kCounterCache) {
          lat_buffer.pop_front();
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

        spark_history.add_sample(freq, rate, latency_ms, loss * 100);

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

        if (seq == 0) {
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

      seq = 0;
      size = 0;
      lat = 0;
    }

    print_lines_count.store(print_lines.size(), std::memory_order_release);
  };

  vlink::Timer update_timer;
  update_timer.set_interval(kCollectInterval);
  update_timer.set_loop_count(vlink::Timer::kInfinite);
  update_timer.attach(discovery_viewer.get());
  update_timer.set_callback([&update_function, &print_function, &update_meta_function]() {
    update_function();
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

  auto apply_filter_input_function = [&filter_list, &update_function, &print_function]() {
    filter_list = vlink::Helpers::split_any(filter_input_text);
    selected_line = -1;
    current_page = 0;

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

        discovery_viewer->post_task([&print_function]() { print_function(false); });
      } else if (key == "backspace") {
        discovery_viewer->post_task([&apply_filter_input_function]() {
          while (!filter_input_text.empty() && (static_cast<unsigned char>(filter_input_text.back()) & 0xC0) == 0x80) {
            filter_input_text.pop_back();
          }

          if (!filter_input_text.empty()) {
            filter_input_text.pop_back();
          }

          apply_filter_input_function();
        });
      } else if (key.size() == 1) {
        discovery_viewer->post_task([key, &apply_filter_input_function]() {
          filter_input_text += key;

          apply_filter_input_function();
        });
      }

      return;
    }

    if (key == "esc" || key == "q") {
      if (is_jumped) {
        return;
      }

      quit_function(0);
    } else if (key == " ") {
      if (is_paused) {
        is_paused = false;
      } else {
        is_paused = true;
      }

      discovery_viewer->post_task([&print_function]() { print_function(false); });
    } else if (key == "left") {
      if (current_page >= 1) {
        --current_page;
        selected_line = -1;
      }

      discovery_viewer->post_task([&update_meta_function, &print_function]() {
        print_function(false);
        update_meta_function();
      });
    } else if (key == "right") {
      if (current_page < total_pages - 1) {
        ++current_page;
        selected_line = -1;
      }

      discovery_viewer->post_task([&update_meta_function, &print_function]() {
        print_function(false);
        update_meta_function();
      });
    } else if (key == "up") {
      if (selected_line < 0) {
        selected_line = ((current_page + 1) * target_row) - 1;

        if (selected_line < 0) {
          selected_line = 0;
        } else if (selected_line > row_count - 1) {
          selected_line = row_count - 1;
        }

        discovery_viewer->post_task([&print_function]() { print_function(false); });
      } else if (selected_line == current_page * target_row && current_page > 0) {
        --current_page;
        int start_index = current_page * target_row;
        int end_index =
            std::min(start_index + target_row, static_cast<int>(print_lines_count.load(std::memory_order_acquire)));
        selected_line = end_index - 1;

        discovery_viewer->post_task([&print_function]() { print_function(false); });
      } else {
        selected_line = std::max(selected_line - 1, 0);

        discovery_viewer->post_task([&update_meta_function, &print_function]() {
          print_function(false);
          update_meta_function();
        });
      }
    } else if (key == "down") {
      int start_index = current_page * target_row;
      int end_index =
          std::min(start_index + target_row, static_cast<int>(print_lines_count.load(std::memory_order_acquire)));

      if (selected_line < 0) {
        selected_line = current_page * target_row;

        discovery_viewer->post_task([&print_function]() { print_function(false); });
      } else if (selected_line == end_index - 1 && current_page < total_pages - 1) {
        ++current_page;
        start_index = current_page * target_row;
        selected_line = start_index;

        discovery_viewer->post_task([&print_function]() { print_function(false); });
      } else {
        selected_line =
            std::min(selected_line + 1, static_cast<int>(print_lines_count.load(std::memory_order_acquire)) - 1);

        discovery_viewer->post_task([&update_meta_function, &print_function]() {
          print_function(false);
          update_meta_function();
        });
      }
    } else if (key == "enter") {
      if (selected_line >= 0 && selected_line < static_cast<int>(print_lines_count.load(std::memory_order_acquire))) {
        discovery_viewer->post_task([&discovery_viewer, &update_timer, &terminal_timer, &print_function,
                                     &update_function, &update_meta_function, &clear_function,
                                     &sub_command_function]() {
          // update_function();

          update_meta_function();

          is_jumped = true;
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
        });
      }
    } else if (key == "z") {
      if (selected_line >= 0) {
        selected_line = -1;
        discovery_viewer->post_task([&print_function]() { print_function(false); });
      }
    } else if (key == "t") {
      count_mode = !count_mode;
      discovery_viewer->post_task([&print_function, &update_function]() {
        update_function();
        print_function(false);
      });
    } else if (key == "l") {
      detail_mode = !detail_mode;
      discovery_viewer->post_task([&print_function, &update_function]() {
        update_function();
        print_function(false);
      });
    } else if (key == "o") {
      observe_all_mode = !observe_all_mode;
      discovery_viewer->post_task([&print_function, &update_function]() {
        update_function();
        print_function(false);
      });
    } else if (key == "e") {
      profiler_mode = !profiler_mode;
      discovery_viewer->post_task([&print_function, &update_function]() {
        update_function();
        print_function(false);
      });
    } else if (key == "s") {
      ser_mode = !ser_mode;
      discovery_viewer->post_task([&print_function, &update_function]() {
        update_function();
        print_function(false);
      });
    } else if (key == "a") {
      active_mode = !active_mode;
      discovery_viewer->post_task([&print_function, &update_function]() {
        update_function();
        print_function(false);
      });
    } else if (key == "y") {
      pubsub_mode = !pubsub_mode;
      discovery_viewer->post_task([&print_function, &update_function]() {
        update_function();
        print_function(false);
      });
    } else if (key == "p") {
      process_mode = !process_mode;
      discovery_viewer->post_task([&print_function, &update_function]() {
        update_function();
        print_function(false);
      });
    } else if (key == "c") {
      chart_mode = !chart_mode;
      discovery_viewer->post_task([&print_function, &update_function]() {
        update_function();
        print_function(false);
      });
    } else if (key == "i") {
      filter_input_mode = true;

      discovery_viewer->post_task([&print_function]() { print_function(false); });
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

int main(int argc, char* argv[]) {
  std::ios::sync_with_stdio(false);
  vlink::Utils::set_console_utf8_output();

  VLINK_TERM_OUT.init();

  // init
  vlink::Logger::set_console_level(vlink::Logger::kOff);
  vlink::Logger::set_file_level(vlink::Logger::kOff);
  vlink::Logger::init("vlink-monitor");

  // env
  vlink::Utils::unset_env("VLINK_BAG_PATH");

  // intra_bind
  std::string intra_bind = vlink::Utils::get_env("VLINK_INTRA_BIND");

  if (!intra_bind.empty()) {
    has_intra_bind = true;
  }

  // arg parser
  argparse::ArgumentParser program("vlink-monitor", VLINK_VERSION, argparse::default_arguments::all);

  program.add_description("Note: You may need to add multicast/broadcast [" +
                          vlink::DiscoveryViewer::get_listen_address() + "]");

  program.add_argument("-u", "--urls")
      .help("Bind urls")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  program.add_argument("-i", "--filter")
      .help("URL keyword filter, comma-separated or quoted space-separated")
      .default_value(std::string());
  program.add_argument("-b", "--blob")
      .help("Force blob output for Enter jump")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("-k", "--black").help("Blacklist mode").default_value(false).implicit_value(true);
  program.add_argument("-n", "--native").help("Native mode").default_value(false).implicit_value(true);
  program.add_argument("-t", "--node_count").help("Node count mode").default_value(false).implicit_value(true);
  program.add_argument("-l", "--detail").help("Detail mode (Hot key)").default_value(false).implicit_value(true);
  program.add_argument("-o", "--observe_all")
      .help("Observe all mode (Hot key)")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("-e", "--profiler").help("Show profiler (Hot key)").default_value(false).implicit_value(true);
  program.add_argument("-s", "--ser").help("Show serialize type (Hot key)").default_value(false).implicit_value(true);
  program.add_argument("-a", "--active").help("Only show active (Hot key)").default_value(false).implicit_value(true);
  program.add_argument("-y", "--pubsub").help("Only show pub/sub (Hot key)").default_value(false).implicit_value(true);
  program.add_argument("-p", "--process")
      .help("Show process panel (Hot key)")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("-c", "--chart").help("Show chart panel (Hot key)").default_value(false).implicit_value(true);
  program.add_argument("-x", "--preset")
      .help("Preset mode(Same as enabling '-l -o -p -c')")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("-g", "--proto_args").help("Append eproto/efbs args").default_value(std::string());

  program.add_argument("-d", "--proto_dir").help("Proto dir").default_value(std::string());

  program.add_argument("-f", "--fbs_dir").help("Flatbuffers dir").default_value(std::string());

  program.add_argument("--plain")
      .help("Plain text output mode (for redirection)")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--dot").help("Use chart dot to paint").default_value(false).implicit_value(true);

  program.add_argument("--rows")
      .help("Maximum rows(0 means automatic)")
      .scan<'d', int>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<int>(0));
  program.add_argument("--columns")
      .help("Maximum columns(0 means automatic)")
      .scan<'d', int>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<int>(0));

  program.add_argument("--chart_width")
      .help("Chart width")
      .scan<'d', int>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<int>(30));

  program.add_argument("--process_width")
      .help("Process width")
      .scan<'d', int>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<int>(40));

  program.add_epilog("Example:\n  vlink-monitor -lo");

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    std::cerr << program << std::endl;
    return 1;
  }

  const auto& urls = program.get<std::vector<std::string>>("-u");
  const auto& filter = program.get<std::string>("-i");

  black_mode = program.is_used("-k");
  blob_mode = program.is_used("-b");
  native_mode = program.is_used("-n");
  count_mode = program.is_used("-t");
  detail_mode = program.is_used("-l");
  observe_all_mode = program.is_used("-o");
  profiler_mode = program.is_used("-e");
  active_mode = program.is_used("-a");
  ser_mode = program.is_used("-s");
  pubsub_mode = program.is_used("-y");
  process_mode = program.is_used("-p");
  chart_mode = program.is_used("-c");

  preset_mode = program.is_used("-x");

  plain_mode = program.is_used("--plain");

  use_chart_dot = program.is_used("--dot");

  max_rows = program.get<int>("--rows");

  max_columns = program.get<int>("--columns");

  chart_width = program.get<int>("--chart_width");

  process_width = program.get<int>("--process_width");

  proto_args = program.get<std::string>("-g");

  auto proto_dir = program.get<std::string>("-d");

  auto fbs_dir = program.get<std::string>("-f");

  if (proto_dir.empty()) {
    proto_dir = vlink::Utils::get_env("VLINK_PROTO_DIR");
  }

  if (fbs_dir.empty()) {
    fbs_dir = vlink::Utils::get_env("VLINK_FBS_DIR");
  }

  if (preset_mode) {
    detail_mode = true;
    observe_all_mode = true;
    process_mode = true;
    chart_mode = true;
  }

  if VUNLIKELY (chart_width < 10 || chart_width > 100) {
    std::cerr << "Invalid [chart_width], range 10 - 100." << std::endl;
    return -1;
  }

  if VUNLIKELY (process_width < 20 || process_width > 100) {
    std::cerr << "Invalid [process_width], range 20 - 100." << std::endl;
    return -1;
  }

#ifdef _WIN32

  if (program.is_used("-d")) {
    try {
      proto_dir = vlink::Helpers::path_to_string(std::filesystem::path(proto_dir));
    } catch (std::filesystem::filesystem_error&) {
    }

    std::replace(proto_dir.begin(), proto_dir.end(), '\\', '/');
  }

  if (program.is_used("-f")) {
    try {
      fbs_dir = vlink::Helpers::path_to_string(std::filesystem::path(fbs_dir));
    } catch (std::filesystem::filesystem_error&) {
    }

    std::replace(fbs_dir.begin(), fbs_dir.end(), '\\', '/');
  }
#endif

  if VUNLIKELY (!detail_mode && observe_all_mode) {
    std::cerr << "Observe all mode[-o] only use for Detail mode[-l]." << std::endl;
    return -1;
  }

  if VUNLIKELY (!detail_mode && active_mode) {
    std::cerr << "Active mode[-a] only use for Detail mode[-l]." << std::endl;
    return -1;
  }

  VLINK_TERM_OUT << "\033[?25l";
  VLINK_TERM_OUT.flush();

  int ret = start_monitor(urls, filter, proto_dir, fbs_dir);

  VLINK_TERM_OUT << "\033[?25h";
  VLINK_TERM_OUT.flush();

  return ret;
}
