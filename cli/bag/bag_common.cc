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

#include "./bag_common.h"

#include <vlink/base/condition_variable.h>
#include <vlink/base/helpers.h>
#include <vlink/base/utils.h>
#include <vlink/vlink.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

std::atomic_bool has_intra_bind{false};
std::atomic_bool data_has_changed{false};
std::atomic_bool quit_flag{false};
std::atomic_bool is_paused{false};
std::atomic_bool is_broken{false};
std::atomic_bool is_play_mode{false};
std::atomic<uint64_t> pause_total_time{0};
std::atomic_bool update_flag{false};
std::atomic<double> play_rate{1.0};
std::atomic_bool is_split_mode{false};
std::atomic<int> split_count{0};
std::atomic_bool pause_to_next_flag{false};
std::atomic<int> compress_level{0};
std::atomic<int64_t> max_task_depth{0};
std::atomic<double> max_memory_size{0};
std::atomic<size_t> total_size{0};
std::atomic<double> total_real_size{0};
std::atomic_bool skip_blank{false};
std::atomic<uint8_t> time_method{kUseUnknown};

[[maybe_unused]] std::atomic<int> play_loop_index{-1};
[[maybe_unused]] std::atomic<int> play_loop_times{1};

[[maybe_unused]] std::atomic_bool has_quit{false};
[[maybe_unused]] bool quiet_flag{false};
[[maybe_unused]] bool detail_flag{false};

vlink::ConditionVariable quit_cv;
std::mutex print_mtx;
std::thread print_thread;
bool print_stopping{false};
vlink::ElapsedTimer main_elapsed_timer{vlink::ElapsedTimer::kMicro};
vlink::ElapsedTimer pause_elapsed_timer{vlink::ElapsedTimer::kMicro};
vlink::Function<int64_t()> time_callback;
vlink::Function<int64_t()> split_index_callback;

vlink::ElapsedTimer total_size_timer;

[[maybe_unused]] void start_print(int64_t start_time, int64_t total_time, int64_t date_time, bool restart) {
  static bool has_start = false;

  std::unique_lock lock(print_mtx);

  if (print_stopping || print_thread.joinable()) {
    return;
  }

  quit_flag = false;

  if (!has_start) {
    has_start = true;
    main_elapsed_timer.start();
  } else if (restart) {
    main_elapsed_timer.restart();
    pause_elapsed_timer.stop();
    pause_total_time = 0;
  }

  print_thread = std::thread([start_time, total_time, date_time]() {
    int64_t print_time = 0;
    int split_index = 0;

    total_real_size = 0;
    total_size = 0;
    total_size_timer.restart();

    double percent = 0;

    while (!quit_flag) {
      std::unique_lock lock(print_mtx);
      quit_cv.wait_for(lock, std::chrono::milliseconds(50),
                       []() -> bool { return quit_flag || is_paused || update_flag; });

      if VUNLIKELY (quit_flag) {
        break;
      }

      std::cout << "\033[2K\r";

      if VUNLIKELY (is_paused) {
        std::cout << "\033[33m";
      } else {
        if (data_has_changed) {
          data_has_changed = false;
          std::cout << "\033[32m";
        } else {
          std::cout << "\033[31m";
        }
      }

      if (time_callback) {
        print_time = time_callback();
      } else {
        print_time = start_time + (main_elapsed_timer.get() - pause_total_time) * play_rate / 1000;
      }

      if (split_index_callback) {
        split_index = split_index_callback();
      }

      std::cout << vlink::Helpers::format_milliseconds(print_time + date_time, false);

      if (is_play_mode) {
        std::cout << "/";
        std::cout << vlink::Helpers::format_milliseconds(total_time + date_time, false);
      }

      switch (time_method) {
        case kUseUnknown:
          break;
        case kUseRelTime:
          break;
        case kUseLocalTime:
          std::cout << " LOC";
          break;
        case kUseUtcTime:
          std::cout << " UTC";
          break;
        default:
          break;
      }

      std::cout << " | ";
      // NOLINTNEXTLINE(readability-redundant-parentheses)
      std::cout << std::fixed << std::setprecision(2) << (print_time) / 1000.0F << "s";
      std::cout << " | ";

      std::cout << vlink::Helpers::format_rate_size(total_real_size);

      std::cout << " ";

      if (is_play_mode) {
        if (play_loop_times != 1) {
          std::cout << "| ";
          std::cout << std::to_string(play_loop_index + 1) + "-" +
                           (play_loop_times <= 0 ? "~" : std::to_string(play_loop_times));
          std::cout << " ";
        }

        if (split_count > 0) {
          std::cout << "| ";
          std::cout << std::to_string(split_index + 1) + "/" + std::to_string(split_count);
          std::cout << " ";
        }
      } else {
        if (is_split_mode) {
          std::cout << "| ";
          std::cout << std::to_string(split_index + 1) + "/~";
          std::cout << " ";
        }
      }

      const int64_t duration = std::max(total_time, static_cast<int64_t>(1));
      percent = (static_cast<double>(print_time) / static_cast<double>(duration)) * 100.0;

      if VUNLIKELY (percent < 0) {
        percent = 0;
      }

      if VUNLIKELY (percent > 100) {
        percent = 100;
      }

      if VUNLIKELY (is_paused) {
        total_real_size = 0;
        total_size = 0;

        std::cout << "\033[43;37;1m";
        std::cout << " || ";

        if (is_play_mode) {
          std::cout << std::fixed << std::setprecision(1) << percent << "% ";
          std::cout.unsetf(std::ios::fixed);
        }

        std::cout << "\033[0m:";
        std::cout.flush();

        quit_cv.wait(lock, []() -> bool { return quit_flag || update_flag || !is_paused; });

        total_size_timer.restart();
      } else {
        if (is_play_mode) {
          std::cout << "\033[44;37;1m";
          std::cout << " >> ";
          std::cout << std::fixed << std::setprecision(1) << percent << "% ";
          std::cout.unsetf(std::ios::fixed);
          std::cout << "\033[0m:";
          std::cout.flush();
        } else {
          std::cout << "\033[44;37;1m";
          std::cout << " << ";
          std::cout << "\033[0m:";
          std::cout.flush();
        }
      }

      update_flag = false;

      if (total_size_timer.get() >= 1000) {
        total_real_size = total_size.load();
        total_size = 0;
        total_size_timer.restart();
      }
    }
  });
}

[[maybe_unused]] void stop_print() {
  std::thread thread_to_join;

  {
    std::unique_lock lock(print_mtx);

    if (print_stopping) {
      quit_cv.wait(lock, []() { return !print_stopping; });
      return;
    }

    quit_flag = true;
    is_paused = false;
    print_stopping = true;
    thread_to_join = std::move(print_thread);
  }

  quit_cv.notify_all();

  if VLIKELY (thread_to_join.joinable()) {
    thread_to_join.join();
  }

  {
    std::unique_lock lock(print_mtx);
    print_stopping = false;
  }

  quit_cv.notify_all();
}

[[maybe_unused]] void update_print() {
  {
    std::unique_lock lock(print_mtx);
    update_flag = true;
  }

  quit_cv.notify_all();
}

[[maybe_unused]] void reset_print() { quit_cv.notify_all(); }

[[maybe_unused]] void print_progress(double progress) {
  static constexpr int kProgressTotalCount = 50;

  progress = std::max(0.0, std::min(1.0, progress));

  int filled = static_cast<int>(std::lround(progress * kProgressTotalCount));

  if (filled > kProgressTotalCount) {
    filled = kProgressTotalCount;
  }

  int empty = kProgressTotalCount - filled;

  std::string bar;
  bar.reserve(kProgressTotalCount);

  bar.append(filled, '#');
  bar.append(empty, '-');

  std::cout << "\033[2K\r";

  std::cout << "Progress: ["
            << "\033[32m" << std::string(filled, '#') << "\033[0m"  // 绿色
            << "\033[90m" << std::string(empty, '-') << "\033[0m"   // 灰色
            << "] ";

  std::cout << vlink::Helpers::double_to_string(progress * 100, 2) << " % ";

  std::cout.flush();
}

static std::vector<std::filesystem::path> clone_bag_files(const std::filesystem::path& path, bool cleanup) {
  std::vector<std::filesystem::path> files{path};
  std::string suffix = path.extension().string();
  std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return std::tolower(c); });

  if ((suffix != ".vdbx" && suffix != ".vcapx") || !std::filesystem::exists(path)) {
    return files;
  }

  try {
    nlohmann::json manifest;
    std::ifstream file(path);
    file >> manifest;

    for (const auto& item : manifest["VLinkFiles"]) {
      if (cleanup && !item.is_string()) {
        continue;
      }

      const auto name = item.get<std::string>();
#ifdef _WIN32
      const auto member = std::filesystem::path(vlink::Helpers::string_to_wstring(name));
#else
      const auto member = std::filesystem::path(name);
#endif

      if (cleanup && (member.empty() || member == "." || member == ".." || member != member.filename())) {
        continue;
      }

      files.emplace_back(path.parent_path() / member);
    }
  } catch (const nlohmann::json::exception& e) {
    if (!cleanup) {
      throw;
    }

    std::cerr << "Cannot parse existing split manifest: " << e.what() << std::endl;
  }

  return files;
}

bool clone_paths_overlap(const std::filesystem::path& source, const std::filesystem::path& target,
                         bool split_name_by_time) {
  auto source_files = clone_bag_files(source, false);
  const auto target_files = clone_bag_files(target, true);

  std::string source_suffix = source.extension().string();
  std::transform(source_suffix.begin(), source_suffix.end(), source_suffix.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  if (source_suffix == ".vdb" || source_suffix == ".vdbx") {
    const size_t source_count = source_files.size();
    for (size_t i = source_suffix == ".vdbx" ? 1 : 0; i < source_count; ++i) {
      const auto database_path = std::filesystem::weakly_canonical(source_files[i]);
      for (const auto* suffix : {"-wal", "-shm"}) {
        auto sidecar = database_path;
        sidecar += suffix;
        if (std::filesystem::exists(sidecar)) {
          source_files.emplace_back(std::move(sidecar));
        }
      }
    }
  }

  auto overlaps_source = [&source_files](const std::filesystem::path& candidate) {
    for (const auto& input : source_files) {
      if (std::filesystem::weakly_canonical(input) == std::filesystem::weakly_canonical(candidate) ||
          (std::filesystem::exists(input) && std::filesystem::exists(candidate) &&
           std::filesystem::equivalent(input, candidate))) {
        return true;
      }
    }

    return false;
  };

  for (const auto& output : target_files) {
    if (overlaps_source(output)) {
      return true;
    }
  }

  std::string suffix = target.extension().string();
  std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return std::tolower(c); });

  if (suffix != ".vdbx" && suffix != ".vcapx") {
    return false;
  }

  suffix.pop_back();
  const std::string target_stem = vlink::Helpers::path_to_string(target.stem());
  const auto output_dir = std::filesystem::absolute(target).parent_path();

  if (!std::filesystem::exists(output_dir)) {
    return false;
  }

  for (const auto& entry : std::filesystem::directory_iterator(output_dir)) {
    std::string extension = entry.path().extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (extension != suffix) {
      continue;
    }

    const std::string name = vlink::Helpers::path_to_string(entry.path().stem());
    std::string output_name;
    bool matches = false;

    if (split_name_by_time) {
      static constexpr std::string_view kTimePattern = "0000-00-00_00-00-00-000";
      matches = name.size() == kTimePattern.size();

      for (size_t i = 0; matches && i < name.size(); ++i) {
        matches = kTimePattern[i] == '0' ? name[i] >= '0' && name[i] <= '9' : name[i] == kTimePattern[i];
      }

      output_name = name + suffix;
    } else {
      const auto dot = name.find_last_of('.');

      if (dot != std::string::npos && dot + 1 < name.size()) {
        const auto begin = name.begin() + static_cast<std::string::difference_type>(dot + 1);
        matches = *begin != '0' && std::all_of(begin, name.end(), [](char c) { return c >= '0' && c <= '9'; });
        output_name = target_stem;
        output_name.append(name, dot);
        output_name += suffix;
      }
    }

#ifdef _WIN32
    const auto output_file = std::filesystem::path(vlink::Helpers::string_to_wstring(output_name));
#else
    const auto output_file = std::filesystem::path(output_name);
#endif

    if (matches && overlaps_source(output_dir / output_file)) {
      return true;
    }
  }

  return false;
}
