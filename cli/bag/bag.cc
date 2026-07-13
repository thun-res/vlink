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

#include <vlink/base/condition_variable.h>
#include <vlink/base/elapsed_timer.h>
#include <vlink/base/helpers.h>
#include <vlink/base/plugin.h>
#include <vlink/base/utils.h>
#include <vlink/extension/bag_plugin_interface.h>
#include <vlink/extension/bag_reader.h>
#include <vlink/extension/bag_writer.h>
#include <vlink/extension/discovery_viewer.h>
#include <vlink/version.h>
#include <vlink/vlink.h>

#include <argparse/argparse.hpp>
//
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

enum TimeMethod : uint8_t {
  kUseUnknown = 0,
  kUseRelTime = 1,
  kUseLocalTime = 2,
  kUseUtcTime = 4,
};

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
std::atomic<uint64_t> skip_time{0};
std::atomic<uint8_t> time_method{kUseUnknown};

[[maybe_unused]] std::atomic<int> play_loop_index{-1};
[[maybe_unused]] std::atomic<int> play_loop_times{1};

[[maybe_unused]] static std::atomic_bool has_quit{false};
[[maybe_unused]] static constexpr int kCollectInterval{1000};
[[maybe_unused]] static bool quiet_flag{false};
[[maybe_unused]] static bool detail_flag{false};

vlink::ConditionVariable quit_cv;
std::mutex print_mtx;
std::thread print_thread;
vlink::ElapsedTimer main_elapsed_timer{vlink::ElapsedTimer::kMicro};
vlink::ElapsedTimer pause_elapsed_timer{vlink::ElapsedTimer::kMicro};
vlink::Function<int64_t()> time_callback;
vlink::Function<int64_t()> split_index_callback;

vlink::ElapsedTimer total_size_timer;

[[maybe_unused]] static double convert_time_to_seconds(const std::string& time_str) {
  int64_t hours = 0;
  int64_t minutes = 0;
  int64_t seconds = 0;
  int64_t milliseconds = 0;

  char delimiter1 = {0};
  char delimiter2 = {0};
  char delimiter3 = {0};

  thread_local std::stringstream ss;
  ss.clear();
  ss.str(time_str);

  if (ss >> hours >> delimiter1 >> minutes >> delimiter2 >> seconds) {
    if (delimiter1 != ':' || delimiter2 != ':') {
      return -1;
    }

    if (ss >> delimiter3 >> milliseconds) {
      if (delimiter3 != ':') {
        return -1;
      }
    } else {
      milliseconds = 0;
    }

    if (hours < 0 || minutes < 0 || minutes >= 60 || seconds < 0 || seconds >= 60 || milliseconds < 0 ||
        milliseconds >= 1000) {
      return -1;
    }

    return (hours * 3600) + (minutes * 60) + seconds + (milliseconds / 1000.0);
  }

  return -1;
}

[[maybe_unused]] static void start_print(int64_t start_time, int64_t total_time, int64_t date_time, bool restart) {
  quit_flag = false;

  static bool has_start = false;

  {
    std::unique_lock lock(print_mtx);

    if (!has_start) {
      has_start = true;
      main_elapsed_timer.start();
    } else {
      if (restart) {
        main_elapsed_timer.restart();
        pause_elapsed_timer.stop();
        pause_total_time = 0;
      }
    }
  }

  if VUNLIKELY (print_thread.joinable()) {
    return;
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

      if (skip_blank) {
        percent = (static_cast<double>(print_time - skip_time) / std::max(total_time, static_cast<int64_t>(1))) * 100.0;
      } else {
        percent = (static_cast<double>(print_time) / std::max(total_time, static_cast<int64_t>(1))) * 100.0;
      }

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

[[maybe_unused]] static void stop_print() {
  std::unique_lock lock(print_mtx);

  if (!quit_flag) {
    quit_flag = true;
    is_paused = false;

    lock.unlock();

    quit_cv.notify_all();

    if VLIKELY (print_thread.joinable()) {
      print_thread.join();
    }
  }
}

[[maybe_unused]] static void update_print() {
  {
    std::unique_lock lock(print_mtx);
    update_flag = true;
  }

  quit_cv.notify_all();
}

[[maybe_unused]] static void reset_print() { quit_cv.notify_all(); }

[[maybe_unused]] static void print_progress(double progress) {
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

int bag_info(const std::string& path) {
  is_play_mode = true;

  try {
#ifdef _WIN32
    auto filesys_path = std::filesystem::path(vlink::Helpers::string_to_wstring(path));
#else
    auto filesys_path = std::filesystem::path(path);
#endif

    if VUNLIKELY (!std::filesystem::exists(filesys_path)) {
      std::cerr << "The target file not exists." << std::endl;
      has_quit = true;
      return -1;
    }
  } catch (std::filesystem::filesystem_error& e) {
    std::cerr << e.what() << std::endl;
    has_quit = true;
    return -1;
  }

  std::shared_ptr<vlink::BagReader> player;

  try {
    player = vlink::BagReader::create(path, true);
  } catch (vlink::Exception::RuntimeError&) {
    has_quit = true;
    return -1;
  }

  is_split_mode = player->is_split_mode();

  if (detail_flag) {
    std::cout << "Data Lists:\n" << std::endl;
    player->register_output_callback([](const vlink::Frame& frame) {
      const int64_t timestamp = frame.timestamp;
      const std::string& url = frame.url;

      std::cout << "\033[2K\r";
      std::cout << std::fixed << std::setprecision(6) << timestamp / 1000000.0 << "s " << url << std::endl;
    });

    auto quit_function = [&player](int) {
      if VUNLIKELY (has_quit) {
        return;
      }

      has_quit = true;

      if VLIKELY (player) {
        player->stop();
        player->quit(true);
      }

      is_broken = true;
    };

    vlink::Utils::register_terminate_signal(quit_function);

    vlink::BagReader::Config config;
    config.begin_time = 0;
    config.end_time = 0;
    config.times = 1;
    config.rate = 1.0;
    config.skip_blank = true;
    config.force_delay = 1;
    config.auto_pause = false;
    config.auto_quit = true;

    player->play(config);

    vlink::Utils::start_detect_keyboard([&quit_function, &player](const std::string& key) {
      if (key == "q" || key == "esc") {
        quit_function(0);
      } else if (key == " ") {
        if (is_paused) {
          if VLIKELY (pause_elapsed_timer.is_active()) {
            pause_total_time += pause_elapsed_timer.get();
            pause_elapsed_timer.stop();
          }
          is_paused = false;
          player->resume();
        } else {
          pause_elapsed_timer.start();
          is_paused = true;
          player->pause();
        }
        reset_print();
      }
    });

    player->run();

    has_quit = true;

    vlink::Utils::stop_detect_keyboard();
    std::cout << std::endl;
    std::cout.flush();
  } else {
    std::cout << "File Name:     " << player->get_info().file_name << std::endl;
    std::cout << "File Size:     " << vlink::Helpers::format_file_size(player->get_info().file_size);

    if (player->get_info().total_raw_size > 0) {
      std::cout << " (Raw: " << vlink::Helpers::format_file_size(player->get_info().total_raw_size) + ")";
    }

    std::cout << std::endl;

    std::cout << "Tag Name:      " << player->get_info().tag_name << std::endl;
    std::cout << "Version:       " << player->get_info().version << std::endl;
    std::cout << "Storage Type:  " << player->get_info().storage_type << std::endl;
    std::cout << "Compression:   " << player->get_info().compression_type;

    if (!player->get_info().compression_type.empty() && player->get_info().compression_type != "None" &&
        player->get_info().total_raw_size > 0) {
      auto file_size = player->get_info().file_size;

      if (file_size > player->get_info().total_raw_size) {
        file_size = player->get_info().total_raw_size;
      }

      std::cout << " (Ratio: "
                << vlink::Helpers::double_to_string(100.0 * file_size / player->get_info().total_raw_size, 0) + "%)";
    }

    std::cout << std::endl;

    std::cout << "Process Name:  " << player->get_info().process_name << std::endl;

    std::cout << "Meta Flags:    ";

    std::string flags_str;

    if (player->get_info().has_completed) {
      flags_str.append("completed | ");
    }

    if (player->get_info().has_idx_elapsed) {
      flags_str.append("idx_elapsed | ");
    }

    if (player->get_info().has_idx_url) {
      flags_str.append("idx_url | ");
    }

    if (player->get_info().has_schema) {
      flags_str.append("schema | ");
    }

    if (flags_str.size() >= 3) {
      flags_str.pop_back();
      flags_str.pop_back();
      flags_str.pop_back();
    }

    std::cout << flags_str;
    std::cout << std::endl;

    std::cout << "Date Time:     " << player->get_info().date_time;

    if (player->get_info().timezone == 0) {
      std::cout << " (UTC)";
    } else {
      const int64_t timezone = player->get_info().timezone;
      const int64_t timezone_magnitude = timezone < 0 ? -timezone : timezone;

      if (player->get_info().timezone > 0) {
        std::cout << " (Timezone: +";
      } else {
        std::cout << " (Timezone: -";
      }

      std::cout << std::setw(2) << std::setfill('0') << timezone_magnitude / 60;
      std::cout << ":";
      std::cout << std::setw(2) << std::setfill('0') << timezone_magnitude % 60;
      std::cout << std::setfill(' ');
      std::cout << ":00)";
    }

    std::cout << std::endl;

    std::cout << "Duration:      " << vlink::Helpers::format_milliseconds(player->get_info().blank_duration, true);
    std::cout << " ~ ";
    std::cout << vlink::Helpers::format_milliseconds(player->get_info().total_duration, true);
    std::cout << std::endl;

    std::cout << "Message Count: " << player->get_info().message_count << std::endl;

    if (player->get_info().split_count > 0) {
      std::cout << "Split Count:   " << std::to_string(player->get_info().split_count);

      if (player->get_info().split_by_time > 0) {
        std::cout << " (";
        std::cout << "By time: ";
        std::cout << vlink::Helpers::double_to_string(player->get_info().split_by_time / 1000.0, 2);
        std::cout << "s)";
      } else if (player->get_info().split_by_size > 0) {
        std::cout << " (";
        std::cout << "By size: ";
        std::cout << vlink::Helpers::double_to_string(player->get_info().split_by_size / 1024.0 / 1024.0 / 1024.0, 2);
        std::cout << "GB)";
      }

      std::cout << std::endl;
    } else {
      std::cout << "Split Count:   "
                << "---" << std::endl;
    }

    size_t max_url_type_size = 6;
    size_t max_count_type_size = 7;
    size_t max_size_type_size = 7;
    size_t max_freq_type_size = 7;
    size_t max_loss_type_size = 6;
    size_t max_url_size = 10;
    size_t max_ser_type_size = 10;

    for (const auto& meta : player->get_info().url_metas) {
      max_url_type_size = std::max(max_url_type_size, meta.url_type.size());
      max_count_type_size = std::max(max_count_type_size, std::to_string(meta.count).size());
      max_size_type_size = std::max(max_size_type_size, vlink::Helpers::format_file_size(meta.size).size());

      std::string freq_str;

      if (meta.freq >= 1000000) {
        freq_str = "999999.99Hz";
      } else {
        freq_str = vlink::Helpers::double_to_string(meta.freq, 2) + "Hz";
      }

      max_freq_type_size = std::max(max_freq_type_size, freq_str.size());

      if (meta.loss > 0 && meta.loss < 0.0001) {
        max_loss_type_size = std::max(max_loss_type_size, std::string("00.0000%").size());
      } else {
        max_loss_type_size = std::max(max_loss_type_size, std::string("00.00%").size());
      }

      max_url_size = std::max(max_url_size, meta.url.size());
      max_ser_type_size = std::max(max_ser_type_size, meta.ser_type.size());
    }

    (void)max_ser_type_size;

    std::cout << "Meta List:";
    std::cout << std::string("     ");
    std::cout << "[Type]";
    std::cout << std::string(max_url_type_size - 4, ' ');
    std::cout << "[Count]";
    std::cout << std::string(max_count_type_size - 5, ' ');
    std::cout << "[Size]";
    std::cout << std::string(max_size_type_size - 4, ' ');
    std::cout << "[Freq]";
    std::cout << std::string(max_freq_type_size - 4, ' ');
    std::cout << "[Loss]";
    std::cout << std::string(max_loss_type_size - 4, ' ');
    std::cout << "[Url]";
    std::cout << std::string(max_url_size - 3, ' ');
    std::cout << "[Ser]";

    std::cout << std::endl;

    std::string loss_str;
    for (const auto& meta : player->get_info().url_metas) {
      std::cout << std::string("               ");

      std::cout << meta.url_type;
      std::cout << std::string(
          std::max(static_cast<int>(max_url_type_size) - static_cast<int>(meta.url_type.size()) + 2, 2), ' ');

      if (meta.count == 0) {
        std::cout << "Unknown";
        std::cout << std::string(std::max(static_cast<int>(max_count_type_size) - 7 + 2, 2), ' ');
      } else {
        std::cout << meta.count;
        std::cout << std::string(
            std::max(static_cast<int>(max_count_type_size) - static_cast<int>(std::to_string(meta.count).size()) + 2,
                     2),
            ' ');
      }

      if (meta.size == 0) {
        std::cout << "Unknown";
        std::cout << std::string(std::max(static_cast<int>(max_size_type_size) - 7 + 2, 2), ' ');
      } else {
        auto size_str = vlink::Helpers::format_file_size(meta.size);
        std::cout << size_str;
        std::cout << std::string(
            std::max(static_cast<int>(max_size_type_size) - static_cast<int>(size_str.size()) + 2, 2), ' ');
      }

      if (meta.freq == 0) {
        std::cout << "Unknown";
        std::cout << std::string(std::max(static_cast<int>(max_freq_type_size) - 7 + 2, 2), ' ');
      } else {
        std::string freq_str;

        if (meta.freq >= 1000000) {
          freq_str = "999999.99Hz";
        } else {
          freq_str = vlink::Helpers::double_to_string(meta.freq, 2) + "Hz";
        }

        std::cout << freq_str;
        std::cout << std::string(
            std::max(static_cast<int>(max_freq_type_size) - static_cast<int>(freq_str.size()) + 2, 2), ' ');
      }

      if (max_loss_type_size > 6) {
        loss_str = vlink::Helpers::double_to_string(meta.loss * 100, 4) + "%";
      } else {
        loss_str = vlink::Helpers::double_to_string(meta.loss * 100, 2) + "%";
      }

      std::cout << loss_str;
      std::cout << std::string(
          std::max(static_cast<int>(max_loss_type_size) - static_cast<int>(loss_str.size()) + 2, 2), ' ');

      std::cout << meta.url;
      std::cout << std::string(std::max(static_cast<int>(max_url_size) - static_cast<int>(meta.url.size()) + 2, 2),
                               ' ');

      std::cout << meta.ser_type;

      std::cout << std::endl;
    }
  }

  has_quit = true;

  player.reset();

  return 0;
}

template <typename BagPtrT>
static int load_and_bind_bag_plugin(vlink::Plugin& plugin, const std::string& plugin_name, const BagPtrT& bag) {
  if (plugin_name.empty()) {
    return 0;
  }

  auto plugin_interface = plugin.load<vlink::BagPluginInterface>(plugin_name, 2, 0);

  if VUNLIKELY (!plugin_interface) {
    std::cerr << "Failed to load plugin (" << plugin_name << ")." << std::endl;
    return -1;
  }

  bag->bind_plugin_interface(plugin_interface);

  return 0;
}

// NOLINTNEXTLINE(google-readability-function-size)
int bag_record(const std::string& path, const std::vector<std::string>& urls, const std::string& tag_name,
               const std::string& filter, bool black_mode, bool native_mode, double duration, double wait_time,
               bool compress, bool force, int64_t max_row_count, double max_bytes_size, bool enable_limit,
               bool split_name_by_time, double split_by_size, int64_t split_by_time, bool deft, double max_packet_size,
               bool wal_mode, double cache_size, bool sync_mode, const std::vector<std::string>& ignore_compress,
               const std::string& plugin_name) {
  using RawSub = vlink::Subscriber<vlink::Bytes>;

  is_play_mode = false;

  std::atomic<int> status = 0;

#ifdef _WIN32
  auto filesys_path = std::filesystem::path(vlink::Helpers::string_to_wstring(path));
#else
  auto filesys_path = std::filesystem::path(path);
#endif

  try {
    if VUNLIKELY (!force && std::filesystem::exists(filesys_path)) {
      vlink::Utils::register_terminate_signal(
          [](int) {
            has_quit = true;
            std::exit(1);
          },
          true);

      std::cout << "The target file already exists, force overwriting? (Y/N):" << std::endl;

      std::string input;
      std::cin >> input;

      if (input != "y" && input != "Y" && input != "yes" && input != "Yes" && input != "YES") {
        std::cout << "Exit." << std::endl;
        has_quit = true;
        return 0;
      }
    }
  } catch (std::filesystem::filesystem_error& e) {
    std::cerr << e.what() << std::endl;
    has_quit = true;
    return -1;
  }

  std::vector<std::string> filter_list = vlink::Helpers::split_any(filter);

  vlink::Plugin plugin;

  std::shared_ptr<vlink::DiscoveryViewer> discovery_viewer;

  std::shared_ptr<vlink::BagWriter> recorder;

  std::unordered_map<std::string, std::shared_ptr<RawSub>> sub_map;

  std::mutex subs_mtx;

  size_t real_max_packet_size = max_packet_size * 1024L * 1024L;

  size_t real_max_memory_size = max_memory_size * 1024L * 1024L * 1024L;

  auto quit_function = [&discovery_viewer, &recorder, wait_time, &status](int) {
    if VUNLIKELY (has_quit.exchange(true)) {
      return;
    }

    if VLIKELY (discovery_viewer) {
      discovery_viewer->quit(true);
    }

    if VLIKELY (recorder) {
      recorder->clear_plugin_interface();

      if (!quiet_flag && !detail_flag) {
        stop_print();
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(5));

      std::cout << "\033[2K\rPlease wait for record(Up to " << std::fixed << std::setprecision(1) << wait_time;
      std::cout << "s)...";
      std::cout.flush();

      if VUNLIKELY (!recorder->wait_for_idle(wait_time * 1000)) {
        std::cerr << "BagWriter force to quit." << std::endl;
        recorder->quit(true);
        status = 1;
      } else {
        recorder->quit();
      }

      std::cout << "\033[2K\r";
      std::cout.flush();
    }

    is_broken = true;
  };

  vlink::Utils::register_terminate_signal(quit_function, true);

  try {
    vlink::DiscoveryViewer::FilterType filter_type = vlink::DiscoveryViewer::kFilterAvailable;

    if (native_mode) {
      filter_type = vlink::DiscoveryViewer::kFilterNative;
    }

    discovery_viewer = std::make_shared<vlink::DiscoveryViewer>(filter_type);
  } catch (vlink::Exception::RuntimeError&) {
    has_quit = true;
    return -1;
  }

  discovery_viewer->async_run();

  if (!deft) {
    if (!quiet_flag) {
      std::cout << "Information Collecting, Please Wait...";
      std::cout.flush();
    }

    discovery_viewer->wait_for_quit(kCollectInterval);

    if (!quiet_flag) {
      std::cout << "\033[2K\r";
      std::cout.flush();
    }
  }

  vlink::BagWriter::Config config;
  config.tag_name = tag_name;
  config.cache_size = 1024LL * 1024LL * cache_size;
  config.wal_mode = wal_mode;
  config.compress = compress ? vlink::BagWriter::kCompressAuto : vlink::BagWriter::kCompressNone;
  config.max_row_count = max_row_count;
  config.max_bytes_size = 1024LL * 1024LL * 1024LL * max_bytes_size;
  config.enable_limit = enable_limit;
  config.split_name_by_time = split_name_by_time;
  config.split_by_size = 1024LL * 1024LL * 1024LL * split_by_size;
  config.split_by_time = split_by_time;
  config.begin_time = 0;
  config.compress_level = compress_level.load();
  config.max_task_depth = max_task_depth;
  config.max_memory_size = real_max_memory_size;
  config.sync_mode = sync_mode;
  config.optimize_on_exit = true;

  if (!ignore_compress.empty()) {
    config.ignore_compress_urls.insert(ignore_compress.begin(), ignore_compress.end());
  }

  std::unordered_set<std::string> target_urls_set(urls.begin(), urls.end());

  try {
    recorder = vlink::BagWriter::create(path, config);
  } catch (vlink::Exception::RuntimeError&) {
    has_quit = true;
    return -1;
  }

  if VUNLIKELY (load_and_bind_bag_plugin(plugin, plugin_name, recorder) != 0) {
    has_quit = true;
    return -1;
  }

  is_split_mode = recorder->is_split_mode();

  recorder->register_split_callback(
      [recorder_ptr = recorder.get(), &sub_map, &subs_mtx](int split_index, const std::string& split_filename) {
        (void)split_filename;

        if (split_index == 0) {
          return;
        }

        std::lock_guard lock(subs_mtx);

        double loss = 0;
        for (const auto& [url, sub] : sub_map) {
          const auto& sample_lost_info = sub->get_lost();

          if (sample_lost_info.total > 0 && sample_lost_info.lost > 0) {
            loss = static_cast<double>(sample_lost_info.lost) / sample_lost_info.total;
          } else {
            loss = 0;
          }

          recorder_ptr->set_url_loss(url, loss);
        }
      },
      true);

  auto update_urls_function = [&target_urls_set, &filter_list, &recorder, &sub_map, &subs_mtx, &status, black_mode,
                               native_mode,
                               real_max_packet_size](const std::vector<vlink::DiscoveryViewer::Info>& info_list) {
    {
      std::unordered_set<std::string> current_urls;

      current_urls.reserve(info_list.size());

      for (const auto& info : info_list) {
        current_urls.emplace(info.url);
      }

      std::lock_guard lock(subs_mtx);

      for (auto iter = sub_map.begin(); iter != sub_map.end();) {
        if VUNLIKELY (current_urls.count(iter->first) == 0) {
          iter = sub_map.erase(iter);
        } else {
          ++iter;
        }
      }
    }

    for (const auto& info : info_list) {
      if VUNLIKELY ((!(info.type & vlink::kPublisher) && !(info.type & vlink::kSetter)) ||
                    (!has_intra_bind && vlink::Url::is_intra_type(info.url))) {
        continue;
      }

      auto sub_iter = sub_map.find(info.url);

      if (sub_iter != sub_map.end()) {
        auto* target_sub = sub_iter->second.get();

        if VUNLIKELY (!target_sub) {
          continue;
        }

        const auto current_schema_type = target_sub->get_schema_type();
        const auto expected_schema_type =
            info.schema_type == vlink::SchemaType::kUnknown ? current_schema_type : info.schema_type;

        if VUNLIKELY (target_sub->get_ser_type() != info.ser_type || current_schema_type != expected_schema_type) {
          target_sub->set_ser_type(info.ser_type, info.schema_type);
        }

        continue;
      }

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

      std::shared_ptr<RawSub> sub;

      try {
        sub = std::make_shared<RawSub>(info.url, vlink::InitType::kWithoutInit);

        if (info.type & vlink::kGetter) {
          sub->mark_as_getter();
        }

        sub->set_latency_and_lost_enabled(true);

        if (native_mode) {
          sub->set_property("dds.ip", "127.0.0.1");
        }

        sub->set_ser_type(info.ser_type, info.schema_type);
        sub->init();
      } catch (vlink::Exception::RuntimeError&) {
        continue;
      }

      std::weak_ptr<RawSub> weak_sub = sub;
      sub->listen([real_max_packet_size, weak_sub, url = info.url, &recorder, &status](const vlink::Bytes& data) {
        if VUNLIKELY (has_quit || recorder->is_ready_to_quit()) {
          return;
        }

        if VUNLIKELY (is_paused) {
          return;
        }

        if VUNLIKELY (data.size() > real_max_packet_size) {  // LIMIT SIZE
          return;
        }

        int64_t timestamp = main_elapsed_timer.get() - pause_total_time;

        total_size += data.size();

        auto sub = weak_sub.lock();

        if VUNLIKELY (!sub) {
          return;
        }

        vlink::Frame frame;
        frame.timestamp = timestamp;
        frame.url = url;
        frame.ser_type = sub->get_ser_type();
        frame.schema_type = sub->get_schema_type();
        frame.action_type = vlink::ActionType::kSubscribe;
        frame.data = vlink::Bytes::shallow_copy(data.data(), data.size());
        if VUNLIKELY (recorder->push(frame) < 0) {
          status = 1;
          has_quit = true;
          is_broken = true;
          recorder->quit(true);
          return;
        }

        if (!quiet_flag) {
          if (detail_flag) {
            std::cout << "\033[2K\r";
            std::cout << std::fixed << std::setprecision(6) << timestamp / 1000000.0 << "s " << url << std::endl;
          } else {
            data_has_changed = true;
          }
        }
      });

      std::lock_guard lock(subs_mtx);
      sub_map.emplace(info.url, std::move(sub));
    }
  };

  vlink::Timer duration_timer;

  if (duration > 0) {
    duration_timer.set_interval(duration * 1000);
    duration_timer.set_loop_count(1);
    duration_timer.attach(discovery_viewer.get());
  }

  recorder->register_begin_handler([recorder_ptr = recorder.get(), &duration_timer, &quit_function, duration]() {
    if VUNLIKELY (has_quit) {
      recorder_ptr->quit(true);
      return;
    }

    if (duration > 0) {
      duration_timer.start([&quit_function] { quit_function(0); });
    }

    if (!quiet_flag && !detail_flag) {
      start_print(0, 0, 0, true);
    }
  });

  recorder->register_end_handler([]() {
    if (!quiet_flag && !detail_flag) {
      stop_print();
    }
  });

  split_index_callback = [recorder_ptr = recorder.get()]() -> int64_t {
    if VUNLIKELY (has_quit) {
      return 0;
    }

    return recorder_ptr->get_split_index();
  };

  if VLIKELY (!discovery_viewer->is_ready_to_quit()) {
    if (!quiet_flag) {
      vlink::Utils::start_detect_keyboard([&quit_function](const std::string& key) {
        if (key == "q" || key == "esc") {
          quit_function(0);
        } else if (key == " ") {
          if (is_paused) {
            if VLIKELY (pause_elapsed_timer.is_active()) {
              pause_total_time += pause_elapsed_timer.get();
              pause_elapsed_timer.stop();
            }
            is_paused = false;
          } else {
            pause_elapsed_timer.start();
            is_paused = true;
          }
          reset_print();
        }
      });
    }

    main_elapsed_timer.start();
    discovery_viewer->post_task(
        [&discovery_viewer, &update_urls_function]() { update_urls_function(discovery_viewer->get_info_list()); });
    discovery_viewer->register_callback(update_urls_function);

    recorder->run();

    while (has_quit && !is_broken) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    recorder->clear_plugin_interface();

    if (!quiet_flag) {
      vlink::Utils::stop_detect_keyboard();
      std::cout << std::endl;
      std::cout.flush();
    }
  }

  discovery_viewer->quit(true);
  discovery_viewer->wait_for_quit();

  {
    std::lock_guard lock(subs_mtx);

    double loss = 0;
    for (const auto& [url, sub] : sub_map) {
      const auto& sample_lost_info = sub->get_lost();

      if (sample_lost_info.total > 0 && sample_lost_info.lost > 0) {
        loss = static_cast<double>(sample_lost_info.lost) / sample_lost_info.total;
      } else {
        loss = 0;
      }

      recorder->set_url_loss(url, loss);
    }

    sub_map.clear();
  }

  stop_print();

  if (has_quit.exchange(true)) {
    while (!is_broken) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  duration_timer.stop();
  duration_timer.detach();
  discovery_viewer.reset();

  recorder->close();

  if VUNLIKELY (recorder->fail()) {
    status = 1;
  }

  recorder.reset();

  return status.load();
}

// NOLINTNEXTLINE(google-readability-function-size)
int bag_play(const std::string& path, const std::vector<std::string>& urls, const std::string& filter, bool black_mode,
             bool native_mode, bool auto_pause, const std::vector<int>& actions, int64_t begin_time, int64_t end_time,
             int times, double rate, const std::string& plugin_name) {
  using RawPub = vlink::Publisher<vlink::Bytes>;

  is_play_mode = true;

  play_rate = rate;

  play_loop_times = times;

#ifdef _WIN32
  auto filesys_path = std::filesystem::path(vlink::Helpers::string_to_wstring(path));
#else
  auto filesys_path = std::filesystem::path(path);
#endif

  try {
    if VUNLIKELY (!std::filesystem::exists(filesys_path)) {
      std::cerr << "The target file not exists." << std::endl;
      has_quit = true;
      return -1;
    }
  } catch (std::filesystem::filesystem_error& e) {
    std::cerr << e.what() << std::endl;
    has_quit = true;
    return -1;
  }

  vlink::Plugin plugin;

  std::shared_ptr<vlink::BagReader> player;

  std::unordered_map<std::string, std::shared_ptr<RawPub>> pub_map;

  std::vector<std::string> filter_list = vlink::Helpers::split_any(filter);

  std::unordered_set<std::string> filter_urls;

  try {
    player = vlink::BagReader::create(path, true);
  } catch (vlink::Exception::RuntimeError&) {
    has_quit = true;
    return -1;
  }

  if VUNLIKELY (begin_time < 0 || end_time < 0) {
    std::cerr << "Invalid time (input error)." << std::endl;
    has_quit = true;
    return -1;
  }

  int64_t date_time = player->get_info().start_timestamp % (24ULL * 60 * 60 * 1000);

  if VUNLIKELY (date_time < 0) {
    std::cerr << "Invalid datatime." << std::endl;
    has_quit = true;
    return -1;
  }

  if (time_method == kUseLocalTime) {
    if (begin_time > 0) {
      begin_time -= (date_time + player->get_info().timezone * 60 * 1000);
    }

    if (end_time > 0) {
      end_time -= (date_time + player->get_info().timezone * 60 * 1000);
    }

    if (begin_time < 0) {
      begin_time += 86400000;
    }

    if (end_time < 0) {
      end_time += 86400000;
    }

    if VUNLIKELY (begin_time < 0 || end_time < 0) {
      std::cerr << "Invalid systime." << std::endl;
      has_quit = true;
      return -1;
    }
  } else if (time_method == kUseUtcTime) {
    if (begin_time > 0) {
      begin_time -= date_time;
    }

    if (end_time > 0) {
      end_time -= date_time;
    }

    if (begin_time < 0) {
      begin_time += 86400000;
    }

    if (end_time < 0) {
      end_time += 86400000;
    }

    if VUNLIKELY (begin_time < 0 || end_time < 0) {
      std::cerr << "Invalid systime." << std::endl;
      has_quit = true;
      return -1;
    }
  }

  if VUNLIKELY (begin_time > 0 && end_time > 0 && begin_time > end_time) {
    std::cerr << "Invalid time." << std::endl;
    has_quit = true;
    return -1;
  }

  if VUNLIKELY (begin_time > player->get_info().total_duration || end_time > player->get_info().total_duration) {
    std::cerr << "Invalid time (duration error)." << std::endl;
    has_quit = true;
    return -1;
  }

  skip_time = player->get_info().blank_duration;

  is_split_mode = player->is_split_mode();

  if VUNLIKELY (load_and_bind_bag_plugin(plugin, plugin_name, player) != 0) {
    has_quit = true;
    return -1;
  }

  for (const auto& meta : player->get_info().url_metas) {
    if (meta.url_type == "Method") {
      continue;
    }

    const std::string& url = meta.url;
    const std::string& ser = meta.ser_type;

    if (!filter_list.empty()) {
      bool skip = black_mode ? false : true;

      std::string left_str = url;
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

    if (urls.empty()) {
      std::shared_ptr<RawPub> pub;

      try {
        pub = std::make_shared<RawPub>(url, vlink::InitType::kWithoutInit);
      } catch (vlink::Exception::RuntimeError&) {
        continue;
      }

      if (meta.url_type == "Field") {
        pub->mark_as_setter();
      }

      if (native_mode) {
        pub->set_property("dds.ip", "127.0.0.1");
      }

      pub->set_ser_type(ser, meta.schema_type);
      pub->init();
      pub_map.emplace(url, std::move(pub));
      filter_urls.emplace(url);
    } else {
      std::shared_ptr<RawPub> pub;

      auto iter = std::find(urls.begin(), urls.end(), url);
      bool condition = black_mode ? iter == urls.end() : iter != urls.end();
      if VLIKELY (condition) {
        try {
          pub = std::make_shared<RawPub>(url, vlink::InitType::kWithoutInit);
        } catch (vlink::Exception::RuntimeError&) {
          continue;
        }

        if (meta.url_type == "Field") {
          pub->mark_as_setter();
        }

        if (native_mode) {
          pub->set_property("dds.ip", "127.0.0.1");
        }

        pub->set_ser_type(ser, meta.schema_type);
        pub->init();
        pub_map.emplace(url, std::move(pub));
        filter_urls.emplace(url);
      }
    }
  }

  if VUNLIKELY (pub_map.empty()) {
    std::cerr << "Can't find any urls to play." << std::endl;
    has_quit = true;
    return -1;
  }

  vlink::BagReader::Status last_status = vlink::BagReader::kStopped;

  auto total_time = player->get_info().total_duration;

  time_callback = [player_ptr = player.get()]() -> int64_t {
    if VUNLIKELY (has_quit) {
      return 0;
    }

    return player_ptr->get_timestamp();
  };

  split_index_callback = [player_ptr = player.get()]() -> int64_t {
    if VUNLIKELY (has_quit) {
      return 0;
    }

    return player_ptr->get_split_index();
  };

  player->register_output_callback([player_ptr = player.get(), actions, &pub_map](const vlink::Frame& frame) {
    const int64_t timestamp = frame.timestamp;
    const std::string& url = frame.url;
    const vlink::ActionType action_type = frame.action_type;
    const vlink::Bytes& data = frame.data;

    if (action_type != vlink::ActionType::kUnknownAction) {
      auto piter = std::find(actions.begin(), actions.end(), static_cast<int>(action_type));

      if (piter == actions.end()) {
        if (pause_to_next_flag) {
          player_ptr->pause_to_next();
        }

        return;
      }
    }

    auto iter = pub_map.find(url);

    if (iter == pub_map.end()) {
      if (pause_to_next_flag) {
        player_ptr->pause_to_next();
      }

      return;
    }

    total_size += data.size();

    iter->second->publish(data);

    pause_to_next_flag = false;

    if (!quiet_flag) {
      if (detail_flag) {
        std::cout << "\033[2K\r";
        std::cout << std::fixed << std::setprecision(6) << timestamp / 1000000.0 << "s " << url << std::endl;
      } else {
        data_has_changed = true;
      }
    }
  });

  player->register_status_callback(
      [player_ptr = player.get(), begin_time, total_time, date_time, &last_status](vlink::BagReader::Status status) {
        split_count = player_ptr->get_info().split_count;

        if (last_status == vlink::BagReader::kStopped) {
          ++play_loop_index;
        }

        if (status == vlink::BagReader::kStopped) {
          pause_to_next_flag = false;
          is_paused = false;

          if (!quiet_flag && !detail_flag) {
            stop_print();
          }
        } else {
          is_paused = (status == vlink::BagReader::kPaused);

          int64_t target_date_time = 0;

          if (time_method == kUseLocalTime) {
            target_date_time = date_time + player_ptr->get_info().timezone * 60 * 1000;
          } else if (time_method == kUseUtcTime) {
            target_date_time = date_time;

            if (target_date_time > 24 * 60 * 60 * 1000) {
              target_date_time -= 24 * 60 * 60 * 1000;
            }
          }

          if (!quiet_flag && !detail_flag) {
            if (skip_blank && begin_time == 0) {
              start_print(player_ptr->get_info().blank_duration, total_time, target_date_time,
                          (last_status == vlink::BagReader::kStopped && !player_ptr->is_ready_to_quit()));
            } else {
              start_print(begin_time, total_time, target_date_time,
                          (last_status == vlink::BagReader::kStopped && !player_ptr->is_ready_to_quit()));
            }
          }
        }

        last_status = status;

        update_print();
      });

  auto quit_function = [&player](int) {
    if VUNLIKELY (has_quit) {
      return;
    }

    has_quit = true;

    if VLIKELY (player) {
      player->clear_plugin_interface();
      player->stop();
      player->quit(true);
    }

    // if (!quiet_flag && !detail_flag) {
    //   stop_print();
    // }

    is_broken = true;
  };

  vlink::Utils::register_terminate_signal(quit_function);

  if (!quiet_flag) {
    vlink::Utils::start_detect_keyboard([&quit_function, &player, rate, times](const std::string& key) {
      if (key == "q" || key == "esc") {
        quit_function(0);
      } else if (key == " ") {
        if (is_paused) {
          if VLIKELY (pause_elapsed_timer.is_active()) {
            pause_total_time += pause_elapsed_timer.get();
            pause_elapsed_timer.stop();
          }

          is_paused = false;
          player->resume();
        } else {
          pause_elapsed_timer.start();
          is_paused = true;
          player->pause();
        }

        reset_print();
      } else if (key == "left") {
        player->jump(player->get_timestamp() - 1000, rate, times, false);
      } else if (key == "right") {
        player->jump(player->get_timestamp() + 1000, rate, times, false);
      } else if (key == "up") {
        player->jump(player->get_timestamp() - 5000, rate, times, false);
      } else if (key == "down") {
        player->jump(player->get_timestamp() + 5000, rate, times, false);
      } else if (key == "p") {
        if (player->get_status() == vlink::BagReader::kPaused) {
          pause_to_next_flag = true;
          player->pause_to_next();
        }
      }
    });

    std::cout << "Please Wait...";
    std::cout.flush();
  }

  vlink::BagReader::Config config;
  config.begin_time = begin_time;
  config.end_time = end_time;
  config.times = times;
  config.rate = rate;
  config.skip_blank = skip_blank;
  config.force_delay = -1;
  config.auto_pause = auto_pause;
  config.auto_quit = true;

  if (!filter_list.empty()) {
    config.filter_urls = filter_urls;
  }

  player->play(config);

  player->run();

  player->clear_plugin_interface();

  has_quit = true;

  stop_print();

  if (!quiet_flag) {
    vlink::Utils::stop_detect_keyboard();
    std::cout << std::endl;
    std::cout.flush();
  }

  plugin.clear();

  pub_map.clear();
  player.reset();

  if (!quiet_flag) {
    std::cout << "\033[2K\r";
    std::cout.flush();
  }

  return 0;
}

// NOLINTNEXTLINE(google-readability-function-size)
int bag_clone(const std::string& source_path, const std::string& target_path, const std::vector<std::string>& urls,
              const std::string& tag_name, const std::string& filter, bool black_mode, const std::vector<int>& actions,
              int64_t begin_time, int64_t end_time, bool compress, bool split_name_by_time, double split_by_size,
              int64_t split_by_time, bool force, bool wal_mode, double cache_size,
              const std::vector<std::string>& ignore_compress, const std::string& plugin_name) {
  is_play_mode = true;
  play_rate = 1.0;

  try {
#ifdef _WIN32
    auto filesys_source_path = std::filesystem::path(vlink::Helpers::string_to_wstring(source_path));
#else
    auto filesys_source_path = std::filesystem::path(source_path);
#endif

    if VUNLIKELY (!std::filesystem::exists(filesys_source_path)) {
      std::cerr << "The target file not exists." << std::endl;
      has_quit = true;
      return -1;
    }

    if VUNLIKELY (std::filesystem::path(source_path) == std::filesystem::path(target_path)) {
      std::cerr << "The source file is same as target file." << std::endl;
      has_quit = true;
      return -1;
    }
  } catch (std::filesystem::filesystem_error& e) {
    std::cerr << e.what() << std::endl;
    has_quit = true;
    return -1;
  }

  try {
#ifdef _WIN32
    auto filesys_target_path = std::filesystem::path(vlink::Helpers::string_to_wstring(target_path));
#else
    auto filesys_target_path = std::filesystem::path(target_path);
#endif

    if VUNLIKELY (!force && std::filesystem::exists(filesys_target_path)) {
      vlink::Utils::register_terminate_signal(
          [](int) {
            has_quit = true;
            std::exit(1);
          },
          true);

      std::cout << "The target file already exists, force overwriting? (Y/N):" << std::endl;

      std::string input;
      std::cin >> input;

      if (input != "y" && input != "Y" && input != "yes" && input != "Yes" && input != "YES") {
        std::cout << "Exit." << std::endl;
        has_quit = true;
        return 0;
      }
    }
  } catch (std::filesystem::filesystem_error& e) {
    std::cerr << e.what() << std::endl;
    has_quit = true;
    return -1;
  }

  vlink::Plugin plugin;

  std::shared_ptr<vlink::BagReader> player;

  std::shared_ptr<vlink::BagWriter> recorder;

  std::atomic_bool clone_write_failed{false};

  std::vector<std::string> filter_list = vlink::Helpers::split_any(filter);

  std::unordered_set<std::string> filter_urls;

  std::unordered_set<std::string> final_urls_set;

  if (!quiet_flag) {
    std::cout << "Please Wait...";
    std::cout.flush();
  }

  try {
    player = vlink::BagReader::create(source_path, true, true);
  } catch (vlink::Exception::RuntimeError&) {
    has_quit = true;
    return -1;
  }

  if VUNLIKELY (begin_time < 0 || end_time < 0) {
    std::cerr << "Invalid time (input error)." << std::endl;
    has_quit = true;
    return -1;
  }

  int64_t date_time = player->get_info().start_timestamp % (24ULL * 60 * 60 * 1000);

  if VUNLIKELY (date_time < 0) {
    std::cerr << "Invalid datatime." << std::endl;
    has_quit = true;
    return -1;
  }

  if (time_method == kUseLocalTime) {
    if (begin_time > 0) {
      begin_time -= (date_time + player->get_info().timezone * 60 * 1000);
    }

    if (end_time > 0) {
      end_time -= (date_time + player->get_info().timezone * 60 * 1000);
    }

    if (begin_time < 0) {
      begin_time += 86400000;
    }

    if (end_time < 0) {
      end_time += 86400000;
    }

    if VUNLIKELY (begin_time < 0 || end_time < 0) {
      std::cerr << "Invalid systime." << std::endl;
      has_quit = true;
      return -1;
    }
  } else if (time_method == kUseUtcTime) {
    if (begin_time > 0) {
      begin_time -= date_time;
    }

    if (end_time > 0) {
      end_time -= date_time;
    }

    if (begin_time < 0) {
      begin_time += 86400000;
    }

    if (end_time < 0) {
      end_time += 86400000;
    }

    if VUNLIKELY (begin_time < 0 || end_time < 0) {
      std::cerr << "Invalid systime." << std::endl;
      has_quit = true;
      return -1;
    }
  }

  if VUNLIKELY (begin_time > 0 && end_time > 0 && begin_time > end_time) {
    std::cerr << "Invalid time." << std::endl;
    has_quit = true;
    return -1;
  }

  if VUNLIKELY (begin_time > player->get_info().total_duration || end_time > player->get_info().total_duration) {
    std::cerr << "Invalid time (duration error)." << std::endl;
    has_quit = true;
    return -1;
  }

  vlink::BagWriter::Config record_config;
  record_config.tag_name = tag_name;
  record_config.cache_size = 1024LL * 1024LL * cache_size;
  record_config.wal_mode = wal_mode;
  record_config.split_name_by_time = split_name_by_time;
  record_config.split_by_size = 1024LL * 1024LL * 1024LL * split_by_size;
  record_config.split_by_time = split_by_time;
  record_config.begin_time = std::max(begin_time, player->get_info().blank_duration);
  record_config.compress = compress ? vlink::BagWriter::kCompressAuto : vlink::BagWriter::kCompressNone;
  record_config.compress_level = compress_level.load();
  // record_config.max_task_depth = max_task_depth;
  record_config.start_timestamp = player->get_info().start_timestamp;
  record_config.sync_mode = true;
  record_config.optimize_on_exit = true;

  if (!ignore_compress.empty()) {
    record_config.ignore_compress_urls.insert(ignore_compress.begin(), ignore_compress.end());
  }

  try {
    recorder = vlink::BagWriter::create(target_path, record_config);
  } catch (vlink::Exception::RuntimeError&) {
    has_quit = true;
    return -1;
  }

  if VUNLIKELY (load_and_bind_bag_plugin(plugin, plugin_name, recorder) != 0) {
    has_quit = true;
    return -1;
  }

  is_split_mode = player->is_split_mode();

  for (const auto& meta : player->get_info().url_metas) {
    if (meta.url_type == "Method") {
      continue;
    }

    const std::string& url = meta.url;

    if (!filter_list.empty()) {
      bool skip = black_mode ? false : true;

      std::string left_str = url;
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

    if (urls.empty()) {
      final_urls_set.emplace(url);
      filter_urls.emplace(url);
    } else {
      auto iter = std::find(urls.begin(), urls.end(), url);
      bool condition = black_mode ? iter == urls.end() : iter != urls.end();
      if VLIKELY (condition) {
        final_urls_set.emplace(url);
        filter_urls.emplace(url);
      }
    }
  }

  bool clone_all = false;

  if (filter_list.empty() && urls.empty() && !black_mode) {
    clone_all = true;
  }

  for (const auto& url_meta : player->get_info().url_metas) {
    if (url_meta.url_type == "Method") {
      continue;
    }

    recorder->set_url_loss(url_meta.url, url_meta.loss);
  }

  auto quit_function = [&player](int) {
    if VUNLIKELY (has_quit) {
      return;
    }

    has_quit = true;

    if VLIKELY (player) {
      player->clear_plugin_interface();
      player->stop();
      player->quit(true);
    }

    is_broken = true;
  };

  vlink::Utils::register_terminate_signal(quit_function);

  player->register_begin_handler([player_ptr = player.get(), recorder_ptr = recorder.get(), &clone_write_failed]() {
    auto schema_list = player_ptr->detect_schema();

    for (const auto& schema_data : schema_list) {
      if VUNLIKELY (!recorder_ptr->push_schema(schema_data)) {
        std::cerr << "cli/bag: push_schema failed for ser=[" << schema_data.name << "] schema_type=["
                  << static_cast<int>(schema_data.schema_type) << "]; abort clone." << std::endl;
        has_quit = true;
        is_broken = true;
        clone_write_failed = true;
        recorder_ptr->quit(true);
        player_ptr->quit(true);
        break;
      }
    }
  });

  player->register_output_callback([player_ptr = player.get(), recorder_ptr = recorder.get(), actions, clone_all,
                                    &final_urls_set, &clone_write_failed](const vlink::Frame& frame) {
    const int64_t timestamp = frame.timestamp;
    const std::string& url = frame.url;
    const vlink::ActionType action_type = frame.action_type;
    const vlink::Bytes& data = frame.data;

    if VUNLIKELY (has_quit || recorder_ptr->is_ready_to_quit()) {
      return;
    }

    if (clone_all || final_urls_set.count(url) != 0) {
      vlink::ActionType output_action = action_type;

      if (action_type != vlink::ActionType::kUnknownAction) {
        auto piter = std::find(actions.begin(), actions.end(), static_cast<int>(action_type));

        if (piter == actions.end()) {
          return;
        }
      } else {
        output_action = vlink::ActionType::kSubscribe;
      }

      vlink::Frame push_frame;
      push_frame.timestamp = timestamp;
      push_frame.url = url;
      push_frame.ser_type = frame.ser_type;
      push_frame.schema_type = frame.schema_type;
      push_frame.action_type = output_action;
      push_frame.data = vlink::Bytes::shallow_copy(data.data(), data.size());

      if VUNLIKELY (recorder_ptr->push(push_frame) < 0) {
        clone_write_failed = true;
        has_quit = true;
        is_broken = true;
        recorder_ptr->quit(true);
        player_ptr->quit(true);
        return;
      }

      if (!quiet_flag) {
        if (detail_flag) {
          std::cout << "\033[2K\r";
          std::cout << std::fixed << std::setprecision(6) << timestamp / 1000000.0 << "s " << url << std::endl;
        }
      }
    }
  });

  recorder->async_run();

  vlink::BagReader::Config config;
  config.begin_time = begin_time;
  config.end_time = end_time;
  config.times = 1;
  config.rate = 1.0;
  config.skip_blank = false;
  config.force_delay = 0;
  config.auto_pause = false;
  config.auto_quit = true;

  if (!filter_list.empty()) {
    config.filter_urls = filter_urls;
  }

  player->play(config);

  int64_t rel_begin_time = begin_time > 0 ? begin_time : player->get_info().blank_duration;
  int64_t rel_end_time = end_time > 0 ? end_time : player->get_info().total_duration;

  auto update_progress_function = [rel_begin_time, rel_end_time, &player]() {
    if (player->get_status() == vlink::BagReader::kPlaying) {
      if (!quiet_flag) {
        int64_t time_diff = player->get_real_timestamp() - rel_begin_time;

        if (time_diff < 0) {
          time_diff = 0;
        }

        print_progress(static_cast<double>(time_diff) /
                       std::max(rel_end_time - rel_begin_time, static_cast<int64_t>(1)));
      }
    }
  };

  vlink::Timer progress_timer;
  progress_timer.set_interval(50);
  progress_timer.set_loop_count(vlink::Timer::kInfinite);
  progress_timer.attach(recorder.get());

  if (!quiet_flag && !detail_flag) {
    progress_timer.start(update_progress_function);
    recorder->post_task(update_progress_function);
  }

  player->run();

  player->clear_plugin_interface();

  recorder->clear_plugin_interface();

  progress_timer.stop();
  progress_timer.detach();

  if VUNLIKELY (!recorder->wait_for_idle(10000U)) {
    clone_write_failed = true;
    recorder->quit(true);
  } else {
    recorder->quit();
  }

  recorder->wait_for_quit();
  recorder->close();

  if VUNLIKELY (recorder->fail()) {
    clone_write_failed = true;
  }

  has_quit = true;

  player.reset();
  recorder.reset();

  if (!quiet_flag) {
    if (is_broken) {
      std::cout << std::endl;
      std::cout << "Break." << std::endl;
    } else {
      print_progress(100);
      std::cout << std::endl;
      std::cout << "Done." << std::endl;
    }
  }

  return clone_write_failed ? -1 : 0;
}

int bag_check(const std::string& path) {
  is_play_mode = true;

  try {
#ifdef _WIN32
    auto filesys_path = std::filesystem::path(vlink::Helpers::string_to_wstring(path));
#else
    auto filesys_path = std::filesystem::path(path);
#endif

    if VUNLIKELY (!std::filesystem::exists(filesys_path)) {
      std::cerr << "The target file not exists." << std::endl;
      has_quit = true;
      return -1;
    }
  } catch (std::filesystem::filesystem_error& e) {
    std::cerr << e.what() << std::endl;
    has_quit = true;
    return -1;
  }

  std::shared_ptr<vlink::BagReader> player;

  try {
    player = vlink::BagReader::create(path, true);
  } catch (vlink::Exception::RuntimeError&) {
    has_quit = true;
    return -1;
  }

  is_split_mode = player->is_split_mode();

  auto quit_function = [&player](int) {
    if VUNLIKELY (has_quit) {
      return;
    }

    has_quit = true;

    if VLIKELY (player) {
      player->stop();
      player->quit(true);
    }

    is_broken = true;
  };

  vlink::Utils::register_terminate_signal(quit_function);

  if (!quiet_flag) {
    std::cout << "Please Wait...";
    std::cout.flush();
  }

  player->register_idle_handler([player_ptr = player.get()]() { player_ptr->quit(); });

  auto fret = player->check();

  player->run();

  has_quit = true;

  player.reset();

  if (!fret.valid()) {
    if (!quiet_flag) {
      std::cout << "\033[2K\r";

      if (is_broken) {
        std::cout << "Break." << std::endl;
      } else {
        std::cout << "Error." << std::endl;
      }
    }

    return -1;
  }

  if (!fret.get()) {
    if (!quiet_flag) {
      std::cout << "\033[2K\r";

      if (is_broken) {
        std::cout << "Break." << std::endl;
      } else {
        std::cout << "Error." << std::endl;
      }
    }

    return -1;
  }

  if (!quiet_flag) {
    std::cout << "\033[2K\r";

    if (is_broken) {
      std::cout << "Break." << std::endl;
    } else {
      std::cout << "Done." << std::endl;
    }
  }

  return 0;
}

int bag_reindex(const std::string& path) {
  is_play_mode = true;

  try {
#ifdef _WIN32
    auto filesys_path = std::filesystem::path(vlink::Helpers::string_to_wstring(path));
#else
    auto filesys_path = std::filesystem::path(path);
#endif

    if VUNLIKELY (!std::filesystem::exists(filesys_path)) {
      std::cerr << "The target file not exists." << std::endl;
      has_quit = true;
      return -1;
    }
  } catch (std::filesystem::filesystem_error& e) {
    std::cerr << e.what() << std::endl;
    has_quit = true;
    return -1;
  }

  std::shared_ptr<vlink::BagReader> player;

  try {
    player = vlink::BagReader::create(path, false);
  } catch (vlink::Exception::RuntimeError&) {
    has_quit = true;
    return -1;
  }

  is_split_mode = player->is_split_mode();

  auto quit_function = [&player](int) {
    if VUNLIKELY (has_quit) {
      return;
    }

    has_quit = true;

    if VLIKELY (player) {
      player->stop();
      player->quit(true);
    }

    is_broken = true;
  };

  vlink::Utils::register_terminate_signal(quit_function);

  if (!quiet_flag) {
    std::cout << "Please Wait...";
    std::cout.flush();
  }

  player->register_idle_handler([player_ptr = player.get()]() { player_ptr->quit(); });

  auto fret = player->reindex();

  player->run();

  has_quit = true;

  player.reset();

  if (!fret.valid()) {
    if (!quiet_flag) {
      std::cout << "\033[2K\r";

      if (is_broken) {
        std::cout << "Break." << std::endl;
      } else {
        std::cout << "Error." << std::endl;
      }
    }

    return -1;
  }

  if (!fret.get()) {
    if (!quiet_flag) {
      std::cout << "\033[2K\r";

      if (is_broken) {
        std::cout << "Break." << std::endl;
      } else {
        std::cout << "Error." << std::endl;
      }
    }

    return -1;
  }

  if (!quiet_flag) {
    std::cout << "\033[2K\r";

    if (is_broken) {
      std::cout << "Break." << std::endl;
    } else {
      std::cout << "Done." << std::endl;
    }
  }

  return 0;
}

int bag_fix(const std::string& path, bool rebuild_mode) {
  is_play_mode = true;

  try {
#ifdef _WIN32
    auto filesys_path = std::filesystem::path(vlink::Helpers::string_to_wstring(path));
#else
    auto filesys_path = std::filesystem::path(path);
#endif

    if VUNLIKELY (!std::filesystem::exists(filesys_path)) {
      std::cerr << "The target file not exists." << std::endl;
      has_quit = true;
      return -1;
    }
  } catch (std::filesystem::filesystem_error& e) {
    std::cerr << e.what() << std::endl;
    has_quit = true;
    return -1;
  }

  std::shared_ptr<vlink::BagReader> player;

  try {
    player = vlink::BagReader::create(path, false, true);
  } catch (vlink::Exception::RuntimeError&) {
    has_quit = true;
    return -1;
  }

  is_split_mode = player->is_split_mode();

  auto quit_function = [&player](int) {
    if VUNLIKELY (has_quit) {
      return;
    }

    has_quit = true;

    if VLIKELY (player) {
      player->stop();
      player->quit(true);
    }

    is_broken = true;
  };

  vlink::Utils::register_terminate_signal(quit_function);

  if (!quiet_flag) {
    std::cout << "Please Wait...";
    std::cout.flush();
  }

  player->register_idle_handler([player_ptr = player.get()]() { player_ptr->quit(); });

  auto fret = player->fix(rebuild_mode);

  player->run();

  has_quit = true;

  player.reset();

  if (!fret.valid()) {
    if (!quiet_flag) {
      std::cout << "\033[2K\r";

      if (is_broken) {
        std::cout << "Break." << std::endl;
      } else {
        std::cout << "Error." << std::endl;
      }
    }

    return -1;
  }

  if (!fret.get()) {
    if (!quiet_flag) {
      std::cout << "\033[2K\r";

      if (is_broken) {
        std::cout << "Break." << std::endl;
      } else {
        std::cout << "Error." << std::endl;
      }
    }

    return -1;
  }

  if (!quiet_flag) {
    std::cout << "\033[2K\r";

    if (is_broken) {
      std::cout << "Break." << std::endl;
    } else {
      std::cout << "Done." << std::endl;
    }
  }

  return 0;
}

int bag_tag(const std::string& path, const std::string& tag_name) {
  is_play_mode = true;

  try {
#ifdef _WIN32
    auto filesys_path = std::filesystem::path(vlink::Helpers::string_to_wstring(path));
#else
    auto filesys_path = std::filesystem::path(path);
#endif

    if VUNLIKELY (!std::filesystem::exists(filesys_path)) {
      std::cerr << "The target file not exists." << std::endl;
      has_quit = true;
      return -1;
    }
  } catch (std::filesystem::filesystem_error& e) {
    std::cerr << e.what() << std::endl;
    has_quit = true;
    return -1;
  }

  if VUNLIKELY (tag_name.empty()) {
    std::cerr << "Tag name can not be empty." << std::endl;
    has_quit = true;
    return -1;
  }

  std::shared_ptr<vlink::BagReader> player;

  try {
    player = vlink::BagReader::create(path, false);
  } catch (vlink::Exception::RuntimeError&) {
    has_quit = true;
    return -1;
  }

  is_split_mode = player->is_split_mode();

  auto quit_function = [&player](int) {
    if VUNLIKELY (has_quit) {
      return;
    }

    has_quit = true;

    if VLIKELY (player) {
      player->stop();
      player->quit(true);
    }

    is_broken = true;
  };

  vlink::Utils::register_terminate_signal(quit_function);

  player->register_idle_handler([player_ptr = player.get()]() { player_ptr->quit(); });

  player->tag(tag_name);

  player->run();

  has_quit = true;

  player.reset();

  return 0;
}

// NOLINTNEXTLINE(google-readability-function-size)
int main(int argc, char* argv[]) {
  vlink::Utils::set_console_utf8_output();

  // init
  // vlink::Logger::set_console_level(vlink::Logger::kOff);
  vlink::Logger::set_file_level(vlink::Logger::kOff);
  vlink::Logger::init("vlink-bag");

  // env
  vlink::Utils::unset_env("VLINK_BAG_PATH");
  // vlink::Utils::set_env("VLINK_DISCOVER_DISABLE", "1");

  // intra_bind
  std::string intra_bind = vlink::Utils::get_env("VLINK_INTRA_BIND");

  if (!intra_bind.empty()) {
    has_intra_bind = true;
  }

  // arg parser
  argparse::ArgumentParser program("vlink-bag", VLINK_VERSION, argparse::default_arguments::all);

  program.add_description("Note: You may need to add multicast/broadcast [" +
                          vlink::DiscoveryViewer::get_listen_address() + "]");

  // info command
  argparse::ArgumentParser info_command("info", VLINK_VERSION, argparse::default_arguments::help);
  info_command.add_argument("path").help("Database path").required();
  info_command.add_argument("-l", "--detail").help("Detail mode").default_value(false).implicit_value(true);

  info_command.add_description("Print infomation");

  std::string info_example_str = "Example:\n  vlink-bag info /tmp/bag.vdb";
  info_example_str += "\n  ";
  info_example_str += "vlink-bag info /tmp/bag.vdbx";
  info_example_str += "\n  ";
  info_example_str += "vlink-bag info /tmp/bag.vcap";
  info_example_str += "\n  ";
  info_example_str += "vlink-bag info /tmp/bag.vcapx";
  info_command.add_epilog(info_example_str);

  // record command
  argparse::ArgumentParser record_command("record", VLINK_VERSION, argparse::default_arguments::help);
  record_command.add_argument("path").help("Database path").required();
  record_command.add_argument("-u", "--urls")
      .help("Bind urls, empty is all")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  record_command.add_argument("-t", "--tag").help("Set tag name").default_value(std::string());
  record_command.add_argument("-i", "--filter")
      .help("URL keyword filter, comma-separated or quoted space-separated")
      .default_value(std::string());
  record_command.add_argument("-k", "--black").help("Blacklist mode").default_value(false).implicit_value(true);
  record_command.add_argument("-n", "--native").help("Native mode").default_value(false).implicit_value(true);
  record_command.add_argument("-d", "--duration")
      .help("Duration(s), duration <= 0 means invalid")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(0));
  record_command.add_argument("-w", "--wait")
      .help("Max wait for quit time(s)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(30));
  record_command.add_argument("-p", "--compress").help("Compress data").default_value(false).implicit_value(true);
  record_command.add_argument("-f", "--force").help("Overwriting").default_value(false).implicit_value(true);
  record_command.add_argument("-q", "--quiet").help("Quiet mode").default_value(false).implicit_value(true);
  record_command.add_argument("-l", "--detail").help("Detail mode").default_value(false).implicit_value(true);
  record_command.add_argument("-o", "--split_name_by_time")
      .help("Split name by time")
      .default_value(false)
      .implicit_value(true);
  record_command.add_argument("-z", "--split_by_size")
      .help("Split size(GB)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(vlink::BagWriter::Config().split_by_size / 1024.0 / 1024.0 / 1024.0));
  record_command.add_argument("-y", "--split_by_time")
      .help("Split time(s)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(vlink::BagWriter::Config().split_by_time));
  record_command.add_argument("-g", "--deft")
      .help("No collect serialization infomation")
      .default_value(false)
      .implicit_value(true);
  record_command.add_argument("-x", "--max_packet_size")
      .help("Max packet size(MB)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(4.0));
  record_command.add_argument("-j", "--wal_mode").help("Enable wal mode").default_value(false).implicit_value(true);
  record_command.add_argument("-c", "--cache_size")
      .help("Cache size(MB)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(vlink::BagWriter::Config().cache_size / 1024.0 / 1024.0));
  record_command.add_argument("-s", "--sync_mode")
      .help("Synchronous write mode")
      .default_value(false)
      .implicit_value(true);
  record_command.add_argument("--max_task_depth")
      .help("Max pending tasks in the queue")
      .scan<'d', int64_t>()
      .default_value(vlink::BagWriter::Config().max_task_depth);
  record_command.add_argument("--max_memory_size")
      .help("Max memory size in the queue(GB)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(vlink::BagWriter::Config().max_memory_size / 1024.0 / 1024.0 / 1024.0));
  record_command.add_argument("--max_row_count")
      .help("Max row count")
      .scan<'d', int64_t>()
      .default_value(vlink::BagWriter::Config().max_row_count);
  record_command.add_argument("--max_bytes_size")
      .help("Max bytes size(GB)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(vlink::BagWriter::Config().max_bytes_size / 1024.0 / 1024.0 / 1024.0));
  record_command.add_argument("--enable_limit").help("Enable limit").default_value(false).implicit_value(true);
  record_command.add_argument("--compress_level")
      .help("Compress level (range: 1 ~ 5, 0 means default)")
      .scan<'d', int>()
      .default_value(static_cast<int>(vlink::BagWriter::Config().compress_level));
  record_command.add_argument("--ignore_compress")
      .help("Ignore compress urls")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);

  record_command.add_argument("--plugin").help("Plugin name").default_value(std::string());

  record_command.add_description("Record data");

  std::string record_example_str = "Example:\n  vlink-bag record /tmp/bag.vdb";
  record_example_str += "\n  ";
  record_example_str += "vlink-bag record /tmp/bag.vdbx";
  record_example_str += "\n  ";
  record_example_str += "vlink-bag record /tmp/bag.vcap";
  record_example_str += "\n  ";
  record_example_str += "vlink-bag record /tmp/bag.vcapx";
  record_command.add_epilog(record_example_str);

  // play command
  argparse::ArgumentParser play_command("play", VLINK_VERSION, argparse::default_arguments::help);
  play_command.add_argument("path").help("Database path").required();
  play_command.add_argument("-u", "--urls")
      .help("Bind urls, empty is all")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  play_command.add_argument("-i", "--filter")
      .help("URL keyword filter, comma-separated or quoted space-separated")
      .default_value(std::string());
  play_command.add_argument("-k", "--black").help("Blacklist mode").default_value(false).implicit_value(true);
  play_command.add_argument("-n", "--native").help("Native mode").default_value(false).implicit_value(true);
  play_command.add_argument("-s", "--actions")
      .help(
          "1: C/Req, 2: C/Resp, "
          "3: S/Req, 4: S/Resp, "
          "5: Pub, 6: Sub, "
          "7: Set, 8: Get")
      .scan<'d', int>()
      .default_value(std::vector<int>{6})
      .nargs(argparse::nargs_pattern::any);
  play_command.add_argument("-b", "--begin_time")
      .help("Begin time(s)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(0));
  play_command.add_argument("-e", "--end_time")
      .help("End time(s)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(0));
  play_command.add_argument("-t", "--times")
      .help("Play times, times <= 0 means infinite")
      .scan<'d', int>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<int>(1));
  play_command.add_argument("-r", "--rate")
      .help("Play rate[0.01 ~ 100]")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(1.0));
  play_command.add_argument("-q", "--quiet").help("Quiet mode").default_value(false).implicit_value(true);
  play_command.add_argument("-l", "--detail").help("Detail mode").default_value(false).implicit_value(true);
  play_command.add_argument("-m", "--skip_blank").help("Skip black").default_value(false).implicit_value(true);
  play_command.add_argument("-j", "--auto_pause").help("Auto pause").default_value(false).implicit_value(true);

  play_command.add_argument("--local_time").help("Show local time").default_value(false).implicit_value(true);
  play_command.add_argument("--utc_time").help("Show utc time").default_value(false).implicit_value(true);
  play_command.add_argument("--rel_begin_time")
      .help("Relative Begin time(format: '00:00:00' or 00:00:00:000)")
      .default_value(std::string());
  play_command.add_argument("--rel_end_time")
      .help("Relative End time(format: '00:00:00' or 00:00:00:000)")
      .default_value(std::string());
  play_command.add_argument("--local_begin_time")
      .help("Local Begin time(format: '00:00:00' or 00:00:00:000)")
      .default_value(std::string());
  play_command.add_argument("--local_end_time")
      .help("Local End time(format: '00:00:00' or 00:00:00:000)")
      .default_value(std::string());
  play_command.add_argument("--utc_begin_time")
      .help("UTC Begin time(format: '00:00:00' or 00:00:00:000)")
      .default_value(std::string());
  play_command.add_argument("--utc_end_time")
      .help("UTC End time(format: '00:00:00' or 00:00:00:000)")
      .default_value(std::string());

  play_command.add_argument("--plugin").help("Plugin name").default_value(std::string());

  play_command.add_description("Play data");

  std::string play_example_str = "Example:\n  vlink-bag play /tmp/bag.vdb";
  play_example_str += "\n  ";
  play_example_str += "vlink-bag play /tmp/bag.vdbx";
  play_example_str += "\n  ";
  play_example_str += "vlink-bag play /tmp/bag.vcap";
  play_example_str += "\n  ";
  play_example_str += "vlink-bag play /tmp/bag.vcapx";
  play_command.add_epilog(play_example_str);

  // clone command
  argparse::ArgumentParser clone_command("clone", VLINK_VERSION, argparse::default_arguments::help);
  clone_command.add_argument("source_path").help("Source database path").required();
  clone_command.add_argument("target_path").help("Target database path").required();
  clone_command.add_argument("-u", "--urls")
      .help("Bind urls, empty is all")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  clone_command.add_argument("-t", "--tag").help("Set tag name").default_value(std::string());
  clone_command.add_argument("-i", "--filter")
      .help("URL keyword filter, comma-separated or quoted space-separated")
      .default_value(std::string());
  clone_command.add_argument("-k", "--black").help("Blacklist mode").default_value(false).implicit_value(true);
  clone_command.add_argument("-s", "--actions")
      .help(
          "1: C/Req, 2: C/Resp, "
          "3: S/Req, 4: S/Resp, "
          "5: Pub, 6: Sub, "
          "7: Set, 8: Get")
      .scan<'d', int>()
      .default_value(std::vector<int>{6})
      .nargs(argparse::nargs_pattern::any);
  clone_command.add_argument("-b", "--begin_time")
      .help("Begin time(s)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(0));
  clone_command.add_argument("-e", "--end_time")
      .help("End time(s)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(0));
  clone_command.add_argument("-q", "--quiet").help("Quiet mode").default_value(false).implicit_value(true);
  clone_command.add_argument("-l", "--detail").help("Detail mode").default_value(false).implicit_value(true);
  clone_command.add_argument("-p", "--compress").help("Compress data").default_value(false).implicit_value(true);
  clone_command.add_argument("-o", "--split_name_by_time")
      .help("Split name by time")
      .default_value(false)
      .implicit_value(true);
  clone_command.add_argument("-z", "--split_by_size")
      .help("Split size(GB)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(vlink::BagWriter::Config().split_by_size / 1024.0 / 1024.0 / 1024.0));
  clone_command.add_argument("-y", "--split_by_time")
      .help("Split time(s)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(vlink::BagWriter::Config().split_by_time));
  clone_command.add_argument("-f", "--force").help("Overwriting").default_value(false).implicit_value(true);
  clone_command.add_argument("-j", "--wal_mode").help("Enable wal mode").default_value(false).implicit_value(true);
  clone_command.add_argument("-c", "--cache_size")
      .help("Cache size(MB)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(vlink::BagWriter::Config().cache_size / 1024.0 / 1024.0));

  clone_command.add_argument("--rel_begin_time")
      .help("Relative Begin time(format: '00:00:00' or 00:00:00:000)")
      .default_value(std::string());
  clone_command.add_argument("--rel_end_time")
      .help("Relative End time(format: '00:00:00' or 00:00:00:000)")
      .default_value(std::string());
  clone_command.add_argument("--local_begin_time")
      .help("Local Begin time(format: '00:00:00' or 00:00:00:000)")
      .default_value(std::string());
  clone_command.add_argument("--local_end_time")
      .help("Local End time(format: '00:00:00' or 00:00:00:000)")
      .default_value(std::string());
  clone_command.add_argument("--utc_begin_time")
      .help("UTC Begin time(format: '00:00:00' or 00:00:00:000)")
      .default_value(std::string());
  clone_command.add_argument("--utc_end_time")
      .help("UTC End time(format: '00:00:00' or 00:00:00:000)")
      .default_value(std::string());
  clone_command.add_argument("--compress_level")
      .help("Compress level (range: 1 ~ 5, 0 means default)")
      .scan<'d', int>()
      .default_value(static_cast<int>(vlink::BagWriter::Config().compress_level));
  clone_command.add_argument("--ignore_compress")
      .help("Ignore compress urls")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  clone_command.add_argument("--import_schema")
      .help("Try import embedded schema data")
      .default_value(false)
      .implicit_value(true);

  clone_command.add_argument("--plugin").help("Plugin name").default_value(std::string());

  clone_command.add_description("Clone data");

  std::string clone_example_str = "Example:\n  vlink-bag clone /tmp/old_bag.vdb /tmp/new_bag.vdb";
  clone_example_str += "\n  ";
  clone_example_str += "vlink-bag clone /tmp/old_bag.vdbx /tmp/new_bag.vdbx";
  clone_example_str += "\n  ";
  clone_example_str += "vlink-bag clone /tmp/old_bag.vcap /tmp/new_bag.vcap";
  clone_example_str += "\n  ";
  clone_example_str += "vlink-bag clone /tmp/old_bag.vcapx /tmp/new_bag.vdbx";
  clone_example_str += "\n  ";
  clone_example_str += "vlink-bag clone /tmp/old_bag.vdb /tmp/new_bag.vcap";
  clone_example_str += "\n  ";
  clone_example_str += "vlink-bag clone /tmp/old_bag.vcap /tmp/new_bag.vdb";
  clone_command.add_epilog(clone_example_str);

  // check command
  argparse::ArgumentParser check_command("check", VLINK_VERSION, argparse::default_arguments::help);
  check_command.add_argument("path").help("Database path").required();
  check_command.add_description("Check data");

  std::string check_example_str = "Example:\n  vlink-bag check /tmp/bag.vdb";
  check_example_str += "\n  ";
  check_example_str += "vlink-bag check /tmp/bag.vdbx";
  check_example_str += "\n  ";
  check_example_str += "vlink-bag check /tmp/bag.vcap";
  check_example_str += "\n  ";
  check_example_str += "vlink-bag check /tmp/bag.vcapx";
  check_command.add_epilog(check_example_str);

  // reindex command
  argparse::ArgumentParser reindex_command("reindex", VLINK_VERSION, argparse::default_arguments::help);
  reindex_command.add_argument("path").help("Database path").required();
  reindex_command.add_description("Rebuild index");

  std::string reindex_example_str = "Example:\n  vlink-bag reindex /tmp/bag.vdb";
  reindex_example_str += "\n  ";
  reindex_example_str += "vlink-bag reindex /tmp/bag.vdbx";
  reindex_example_str += "\n  ";
  reindex_example_str += "vlink-bag reindex /tmp/bag.vcap";
  reindex_example_str += "\n  ";
  reindex_example_str += "vlink-bag reindex /tmp/bag.vcapx";
  reindex_command.add_epilog(reindex_example_str);

  // fix command
  argparse::ArgumentParser fix_command("fix", VLINK_VERSION, argparse::default_arguments::help);
  fix_command.add_argument("path").help("Database path").required();
  fix_command.add_argument("-y", "--rebuild").help("Rebuild mode").default_value(false).implicit_value(true);
  fix_command.add_description("Fix data");

  std::string fix_example_str = "Example:\n  vlink-bag fix /tmp/bag.vdb";
  fix_example_str += "\n  ";
  fix_example_str += "vlink-bag fix /tmp/bag.vdbx";
  fix_example_str += "\n  ";
  fix_example_str += "vlink-bag fix /tmp/bag.vcap";
  fix_example_str += "\n  ";
  fix_example_str += "vlink-bag fix /tmp/bag.vcapx";
  fix_command.add_epilog(fix_example_str);

  // tag command
  argparse::ArgumentParser tag_command("tag", VLINK_VERSION, argparse::default_arguments::help);
  tag_command.add_argument("path").help("Database path").required();
  tag_command.add_argument("tag").help("Tag name").required();
  tag_command.add_description("Set tag");

  std::string tag_example_str = "Example:\n  vlink-bag tag /tmp/bag.vdb 'tag_name'";
  tag_example_str += "\n  ";
  tag_example_str += "vlink-bag tag /tmp/bag.vdbx 'tag_name'";
  tag_example_str += "\n  ";
  tag_example_str += "vlink-bag tag /tmp/bag.vcap 'tag_name'";
  tag_example_str += "\n  ";
  tag_example_str += "vlink-bag tag /tmp/bag.vcapx 'tag_name'";
  tag_command.add_epilog(tag_example_str);

  program.add_subparser(info_command);
  program.add_subparser(record_command);
  program.add_subparser(play_command);
  program.add_subparser(clone_command);
  program.add_subparser(check_command);
  program.add_subparser(reindex_command);
  program.add_subparser(fix_command);
  program.add_subparser(tag_command);

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;

    if (program.is_subcommand_used("info")) {
      std::cerr << info_command << std::endl;
    } else if (program.is_subcommand_used("record")) {
      std::cerr << record_command << std::endl;
    } else if (program.is_subcommand_used("play")) {
      std::cerr << play_command << std::endl;
    } else if (program.is_subcommand_used("clone")) {
      std::cerr << clone_command << std::endl;
    } else if (program.is_subcommand_used("check")) {
      std::cerr << check_command << std::endl;
    } else if (program.is_subcommand_used("reindex")) {
      std::cerr << reindex_command << std::endl;
    } else if (program.is_subcommand_used("fix")) {
      std::cerr << fix_command << std::endl;
    } else if (program.is_subcommand_used("tag")) {
      std::cerr << tag_command << std::endl;
    }

    return 1;
  }

  auto check_bag_path = [](const std::string& path, const char* label) {
    std::string suffix_check = path;

    std::transform(suffix_check.begin(), suffix_check.end(), suffix_check.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (vlink::Helpers::has_endwith(suffix_check, ".vdb") || vlink::Helpers::has_endwith(suffix_check, ".vdbx") ||
        vlink::Helpers::has_endwith(suffix_check, ".vcap") || vlink::Helpers::has_endwith(suffix_check, ".vcapx")) {
      return true;
    }

    std::cerr << "Warning: Invalid " << label << " suffix: " << path << ". Expected .vdb, .vdbx, .vcap or .vcapx."
              << std::endl;
    return false;
  };

  if (program.is_subcommand_used("info")) {
    auto path = info_command.get<std::string>("path");

#ifdef _WIN32
    try {
      path = vlink::Helpers::path_to_string(std::filesystem::path(path));
    } catch (std::filesystem::filesystem_error&) {
    }
#endif

    detail_flag = info_command.is_used("-l");

    if VUNLIKELY (!check_bag_path(path, "input path")) {
      return -1;
    }

    return bag_info(path);
  } else if (program.is_subcommand_used("record")) {
    auto path = record_command.get<std::string>("path");

#ifdef _WIN32
    try {
      path = vlink::Helpers::path_to_string(std::filesystem::path(path));
    } catch (std::filesystem::filesystem_error&) {
    }
#endif

    const auto& urls = record_command.get<std::vector<std::string>>("-u");
    auto tag_name = record_command.get<std::string>("-t");
    const auto& filter = record_command.get<std::string>("-i");

#ifdef _WIN32
    tag_name = vlink::Helpers::string_local_to_utf8(tag_name);
#endif

    auto black_mode = record_command.is_used("-k");
    auto native_mode = record_command.is_used("-n");
    auto duration = record_command.get<double>("-d");
    auto wait_time = record_command.get<double>("-w");
    auto compress = record_command.is_used("-p");
    auto force = record_command.is_used("-f");

    auto max_row_count = record_command.get<int64_t>("--max_row_count");
    auto max_bytes_size = record_command.get<double>("--max_bytes_size");
    auto enable_limit = record_command.is_used("--enable_limit");

    auto split_name_by_time = record_command.is_used("-o");
    auto split_by_size = record_command.get<double>("-z");
    auto split_by_time = record_command.get<double>("-y");

    auto deft = record_command.is_used("-g");
    auto max_packet_size = record_command.get<double>("-x");
    auto wal_mode = record_command.is_used("-j");
    auto cache_size = record_command.get<double>("-c");
    auto sync_mode = record_command.is_used("-s");

    max_task_depth = record_command.get<int64_t>("--max_task_depth");
    const auto max_memory_size_gb = record_command.get<double>("--max_memory_size");
    max_memory_size = max_memory_size_gb;

    quiet_flag = record_command.is_used("-q");
    detail_flag = record_command.is_used("-l");

    if VUNLIKELY (!check_bag_path(path, "output path")) {
      return -1;
    }

    if VUNLIKELY (sync_mode && record_command.is_used("--max_task_depth")) {
      std::cerr << "Sync mode and task depth cannot be set at the same time" << std::endl;
      return -1;
    }

    if VUNLIKELY (sync_mode && record_command.is_used("--max_memory_size")) {
      std::cerr << "Sync mode and memory size cannot be set at the same time" << std::endl;
      return -1;
    }

    if VUNLIKELY (urls.empty() && deft) {
      std::cerr << "The deft must be turned off in the bind all urls mode" << std::endl;
      return -1;
    }

    if VUNLIKELY (wait_time < 0) {
      std::cerr << "Invalid wait_time [-w]" << std::endl;
      return -1;
    }

    static constexpr auto kMaxPacketSizeMb = std::numeric_limits<size_t>::max() / (1024ULL * 1024ULL);

    static constexpr uint64_t kMaxMemoryBytes = static_cast<uint64_t>(std::numeric_limits<size_t>::max()) <
                                                        static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
                                                    ? static_cast<uint64_t>(std::numeric_limits<size_t>::max())
                                                    : static_cast<uint64_t>(std::numeric_limits<int64_t>::max());

    static constexpr auto kMaxMemorySizeGb = kMaxMemoryBytes / (1024ULL * 1024ULL * 1024ULL);

    static constexpr auto kMaxCacheSizeMb = std::numeric_limits<int64_t>::max() / (1024LL * 1024LL);

    if VUNLIKELY (!std::isfinite(max_packet_size) || max_packet_size <= 0 || max_packet_size > kMaxPacketSizeMb) {
      std::cerr << "Invalid max_packet_size [-x]" << std::endl;
      return -1;
    }

    if VUNLIKELY (!std::isfinite(max_memory_size_gb) || max_memory_size_gb <= 0 ||
                  max_memory_size_gb > kMaxMemorySizeGb) {
      std::cerr << "Invalid max_memory_size [--max_memory_size]" << std::endl;
      return -1;
    }

    if VUNLIKELY (!std::isfinite(cache_size) || cache_size < 0 || cache_size > kMaxCacheSizeMb) {
      std::cerr << "Invalid cache_size [-c]" << std::endl;
      return -1;
    }

    compress_level = record_command.get<int>("--compress_level");

    auto ignore_compress = record_command.get<std::vector<std::string>>("--ignore_compress");

    if VUNLIKELY (!compress &&
                  (record_command.is_used("--compress_level") || record_command.is_used("--ignore_compress"))) {
      std::cerr << "Must set compress [-p]" << std::endl;
      return -1;
    }

    auto record_plugin_name = record_command.get<std::string>("--plugin");

    return bag_record(path, urls, tag_name, filter, black_mode, native_mode, duration, wait_time, compress, force,
                      max_row_count, max_bytes_size, enable_limit, split_name_by_time, split_by_size,
                      split_by_time * 1000, deft, max_packet_size, wal_mode, cache_size, sync_mode, ignore_compress,
                      record_plugin_name);
  } else if (program.is_subcommand_used("play")) {
    auto path = play_command.get<std::string>("path");

#ifdef _WIN32
    try {
      path = vlink::Helpers::path_to_string(std::filesystem::path(path));
    } catch (std::filesystem::filesystem_error&) {
    }
#endif

    const auto& urls = play_command.get<std::vector<std::string>>("-u");
    const auto& filter = play_command.get<std::string>("-i");

    if VUNLIKELY (!check_bag_path(path, "input path")) {
      return -1;
    }

    auto black_mode = play_command.is_used("-k");
    auto native_mode = play_command.is_used("-n");

    quiet_flag = play_command.is_used("-q");
    detail_flag = play_command.is_used("-l");

    skip_blank = play_command.is_used("-m");

    auto auto_pause = play_command.is_used("-j");

    auto actions = play_command.get<std::vector<int>>("-s");
    auto begin_time = play_command.get<double>("-b");
    auto end_time = play_command.get<double>("-e");
    auto times = play_command.get<int>("-t");
    auto rate = play_command.get<double>("-r");

    auto show_local_time = play_command.is_used("--local_time");
    auto show_utc_time = play_command.is_used("--utc_time");

    auto rel_begin_time = play_command.get<std::string>("--rel_begin_time");
    auto rel_end_time = play_command.get<std::string>("--rel_end_time");

    auto local_begin_time = play_command.get<std::string>("--local_begin_time");
    auto local_end_time = play_command.get<std::string>("--local_end_time");

    auto utc_begin_time = play_command.get<std::string>("--utc_begin_time");
    auto utc_end_time = play_command.get<std::string>("--utc_end_time");

    auto plugin_name = play_command.get<std::string>("--plugin");

    time_method = kUseUnknown;

    if (play_command.is_used("--rel_begin_time") || play_command.is_used("--rel_end_time")) {
      time_method |= kUseRelTime;
    }

    if (play_command.is_used("--local_begin_time") || play_command.is_used("--local_end_time")) {
      time_method |= kUseLocalTime;
    }

    if (play_command.is_used("--utc_begin_time") || play_command.is_used("--utc_end_time")) {
      time_method |= kUseUtcTime;
    }

    if VUNLIKELY (time_method != kUseUnknown && time_method != kUseRelTime && time_method != kUseLocalTime &&
                  time_method != kUseUtcTime) {
      std::cerr << "You cannot use diff time formats at the same time" << std::endl;
      return -1;
    }

    switch (time_method) {
      case kUseUnknown:
        break;
      case kUseRelTime:
        if (!rel_begin_time.empty()) {
          begin_time = convert_time_to_seconds(rel_begin_time);
        }

        if (!rel_end_time.empty()) {
          end_time = convert_time_to_seconds(rel_end_time);
        }

        if VUNLIKELY (std::abs(begin_time) > 0.001 && std::abs(end_time) > 0.001 && begin_time >= end_time) {
          std::cerr << "Invalid begin_time and end_time [-b] [-e]" << std::endl;
          return -1;
        }

        break;
      case kUseLocalTime:
        if (!local_begin_time.empty()) {
          begin_time = convert_time_to_seconds(local_begin_time);
        }

        if (!local_end_time.empty()) {
          end_time = convert_time_to_seconds(local_end_time);
        }

        break;
      case kUseUtcTime:
        if (!utc_begin_time.empty()) {
          begin_time = convert_time_to_seconds(utc_begin_time);
        }

        if (!utc_end_time.empty()) {
          end_time = convert_time_to_seconds(utc_end_time);
        }

        break;
      default:
        break;
    }

    if VUNLIKELY (show_local_time && time_method != kUseUnknown && time_method != kUseLocalTime) {
      std::cerr << "You cannot use diff time formats at the same time" << std::endl;
      return -1;
    }

    if VUNLIKELY (show_utc_time && time_method != kUseUnknown && time_method != kUseUtcTime) {
      std::cerr << "You cannot use diff time formats at the same time" << std::endl;
      return -1;
    }

    if VUNLIKELY (show_local_time && show_utc_time) {
      std::cerr << "You cannot use diff time formats at the same time" << std::endl;
      return -1;
    }

    if (show_local_time) {
      time_method = kUseLocalTime;
    } else if (show_utc_time) {
      time_method = kUseUtcTime;
    }

    for (auto a : actions) {
      if VUNLIKELY (a < 1 || a > 8) {
        std::cerr << "Invalid actions [-s]" << std::endl;
        return -1;
      }
    }

    if VUNLIKELY (rate < 0.009999 || rate > 100.000001) {
      std::cerr << "Invalid rate [-r]" << std::endl;
      return -1;
    }

    return bag_play(path, urls, filter, black_mode, native_mode, auto_pause, actions, begin_time * 1000,
                    end_time * 1000, times, rate, plugin_name);
  } else if (program.is_subcommand_used("clone")) {
    auto source_path = clone_command.get<std::string>("source_path");

    auto target_path = clone_command.get<std::string>("target_path");

#ifdef _WIN32
    try {
      source_path = vlink::Helpers::path_to_string(std::filesystem::path(source_path));
      target_path = vlink::Helpers::path_to_string(std::filesystem::path(target_path));
    } catch (std::filesystem::filesystem_error&) {
    }
#endif

    const auto& urls = clone_command.get<std::vector<std::string>>("-u");
    auto tag_name = clone_command.get<std::string>("-t");
    const auto& filter = clone_command.get<std::string>("-i");

    if VUNLIKELY (!check_bag_path(source_path, "input path")) {
      return -1;
    }

    if VUNLIKELY (!check_bag_path(target_path, "output path")) {
      return -1;
    }

#ifdef _WIN32
    tag_name = vlink::Helpers::string_local_to_utf8(tag_name);
#endif

    auto black_mode = clone_command.is_used("-k");
    auto actions = clone_command.get<std::vector<int>>("-s");
    auto begin_time = clone_command.get<double>("-b");
    auto end_time = clone_command.get<double>("-e");

    quiet_flag = clone_command.is_used("-q");
    detail_flag = clone_command.is_used("-l");

    auto compress = clone_command.is_used("-p");
    auto split_name_by_time = clone_command.is_used("-o");
    auto split_by_size = clone_command.get<double>("-z");
    auto split_by_time = clone_command.get<double>("-y");

    auto force = clone_command.is_used("-f");
    auto wal_mode = clone_command.is_used("-j");
    auto cache_size = clone_command.get<double>("-c");

    auto rel_begin_time = clone_command.get<std::string>("--rel_begin_time");
    auto rel_end_time = clone_command.get<std::string>("--rel_end_time");

    auto local_begin_time = clone_command.get<std::string>("--local_begin_time");
    auto local_end_time = clone_command.get<std::string>("--local_end_time");

    auto utc_begin_time = clone_command.get<std::string>("--utc_begin_time");
    auto utc_end_time = clone_command.get<std::string>("--utc_end_time");

    time_method = kUseUnknown;

    if (clone_command.is_used("--rel_begin_time") || clone_command.is_used("--rel_end_time")) {
      time_method |= kUseRelTime;
    }

    if (clone_command.is_used("--local_begin_time") || clone_command.is_used("--local_end_time")) {
      time_method |= kUseLocalTime;
    }

    if (clone_command.is_used("--utc_begin_time") || clone_command.is_used("--utc_end_time")) {
      time_method |= kUseUtcTime;
    }

    if VUNLIKELY (time_method != kUseUnknown && time_method != kUseRelTime && time_method != kUseLocalTime &&
                  time_method != kUseUtcTime) {
      std::cerr << "You cannot use diff time formats at the same time" << std::endl;
      return -1;
    }

    switch (time_method) {
      case kUseUnknown:
        break;
      case kUseRelTime:
        if (!rel_begin_time.empty()) {
          begin_time = convert_time_to_seconds(rel_begin_time);
        }

        if (!rel_end_time.empty()) {
          end_time = convert_time_to_seconds(rel_end_time);
        }

        if VUNLIKELY (std::abs(begin_time) > 0.001 && std::abs(end_time) > 0.001 && begin_time >= end_time) {
          std::cerr << "Invalid begin_time and end_time [-b] [-e]" << std::endl;
          return -1;
        }

        break;
      case kUseLocalTime:
        if (!local_begin_time.empty()) {
          begin_time = convert_time_to_seconds(local_begin_time);
        }

        if (!local_end_time.empty()) {
          end_time = convert_time_to_seconds(local_end_time);
        }

        break;
      case kUseUtcTime:
        if (!utc_begin_time.empty()) {
          begin_time = convert_time_to_seconds(utc_begin_time);
        }

        if (!utc_end_time.empty()) {
          end_time = convert_time_to_seconds(utc_end_time);
        }

        break;
      default:
        break;
    }

    for (auto a : actions) {
      if VUNLIKELY (a < 1 || a > 8) {
        std::cerr << "Invalid actions [-s]" << std::endl;
        return -1;
      }
    }

    static constexpr auto kMaxCacheSizeMb = std::numeric_limits<int64_t>::max() / (1024LL * 1024LL);

    if VUNLIKELY (!std::isfinite(cache_size) || cache_size < 0 || cache_size > kMaxCacheSizeMb) {
      std::cerr << "Invalid cache_size [-c]" << std::endl;
      return -1;
    }

    compress_level = clone_command.get<int>("--compress_level");

    auto ignore_compress = clone_command.get<std::vector<std::string>>("--ignore_compress");

    if VUNLIKELY (!compress &&
                  (clone_command.is_used("--compress_level") || clone_command.is_used("--ignore_compress"))) {
      std::cerr << "Must set compress [-p]" << std::endl;
      return -1;
    }

    if (!clone_command.is_used("--import_schema")) {
      vlink::Utils::unset_env("VLINK_SCHEMA_PLUGIN");
    }

    auto clone_plugin_name = clone_command.get<std::string>("--plugin");

    return bag_clone(source_path, target_path, urls, tag_name, filter, black_mode, actions, begin_time * 1000,
                     end_time * 1000, compress, split_name_by_time, split_by_size, split_by_time * 1000, force,
                     wal_mode, cache_size, ignore_compress, clone_plugin_name);
  } else if (program.is_subcommand_used("check")) {
    auto path = check_command.get<std::string>("path");

#ifdef _WIN32
    try {
      path = vlink::Helpers::path_to_string(std::filesystem::path(path));
    } catch (std::filesystem::filesystem_error&) {
    }
#endif

    if VUNLIKELY (!check_bag_path(path, "input path")) {
      return -1;
    }

    return bag_check(path);
  } else if (program.is_subcommand_used("reindex")) {
    auto path = reindex_command.get<std::string>("path");

#ifdef _WIN32
    try {
      path = vlink::Helpers::path_to_string(std::filesystem::path(path));
    } catch (std::filesystem::filesystem_error&) {
    }
#endif

    if VUNLIKELY (!check_bag_path(path, "input path")) {
      return -1;
    }

    return bag_reindex(path);
  } else if (program.is_subcommand_used("fix")) {
    auto path = fix_command.get<std::string>("path");

#ifdef _WIN32
    try {
      path = vlink::Helpers::path_to_string(std::filesystem::path(path));
    } catch (std::filesystem::filesystem_error&) {
    }
#endif

    auto rebuild_mode = fix_command.is_used("-y");

    if VUNLIKELY (!check_bag_path(path, "input path")) {
      return -1;
    }

    return bag_fix(path, rebuild_mode);
  } else if (program.is_subcommand_used("tag")) {
    auto path = tag_command.get<std::string>("path");

#ifdef _WIN32
    try {
      path = vlink::Helpers::path_to_string(std::filesystem::path(path));
    } catch (std::filesystem::filesystem_error&) {
    }
#endif

    auto tag_name = tag_command.get<std::string>("tag");

    if VUNLIKELY (!check_bag_path(path, "input path")) {
      return -1;
    }

#ifdef _WIN32
    tag_name = vlink::Helpers::string_local_to_utf8(tag_name);
#endif

    return bag_tag(path, tag_name);
  }

  std::cerr << program << std::endl;

  return 1;
}
