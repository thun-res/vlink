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

#include <vlink/base/helpers.h>
#include <vlink/base/utils.h>
#include <vlink/extension/bag_reader.h>
#include <vlink/extension/bag_writer.h>
#include <vlink/vlink.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>
#include <vector>

#include "./bag_commands.h"
#include "./bag_common.h"

// NOLINTNEXTLINE(google-readability-function-size)
int bag_clone(const std::string& source_path, const std::string& target_path, const std::vector<std::string>& urls,
              const std::string& tag_name, const std::string& filter, bool black_mode, const std::vector<int>& actions,
              int64_t begin_time, int64_t end_time, bool has_clock_begin_time, bool has_clock_end_time, bool compress,
              bool split_name_by_time, double split_by_size, int64_t split_by_time, bool force, bool wal_mode,
              double cache_size, const std::vector<std::string>& ignore_compress, const std::string& plugin_name) {
  is_play_mode = true;
  play_rate = 1.0;

  try {
#ifdef _WIN32
    auto filesys_source_path = std::filesystem::path(vlink::Helpers::string_to_wstring(source_path));
    auto filesys_target_path = std::filesystem::path(vlink::Helpers::string_to_wstring(target_path));
#else
    auto filesys_source_path = std::filesystem::path(source_path);
    auto filesys_target_path = std::filesystem::path(target_path);
#endif

    if VUNLIKELY (!std::filesystem::exists(filesys_source_path)) {
      std::cerr << "The target file not exists." << std::endl;
      has_quit = true;
      return -1;
    }

    if VUNLIKELY (clone_paths_overlap(filesys_source_path, filesys_target_path, split_name_by_time)) {
      std::cerr << "The clone output overlaps a source bag file. Use another output path or directory; "
                   "for timestamped splits, use another directory or disable --split_name_by_time."
                << std::endl;
      has_quit = true;
      return -1;
    }
  } catch (const nlohmann::json::exception& e) {
    std::cerr << "Cannot parse source split manifest: " << e.what() << std::endl;
    has_quit = true;
    return -1;
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

  if (time_method == kUseLocalTime || time_method == kUseUtcTime) {
    int64_t clock_start = date_time;

    if (time_method == kUseLocalTime) {
      clock_start = (clock_start + static_cast<int64_t>(player->get_info().timezone) * 60 * 1000) % kDayMilliseconds;

      if (clock_start < 0) {
        clock_start += kDayMilliseconds;
      }
    }

    if (has_clock_begin_time) {
      begin_time -= clock_start;

      if (begin_time < 0) {
        begin_time += kDayMilliseconds;
      }
    }

    if (has_clock_end_time) {
      end_time -= clock_start;

      if (end_time < 0) {
        end_time += kDayMilliseconds;
      }
    }
  }

  if VUNLIKELY (has_clock_end_time && end_time == 0) {
    std::cerr << "Clock end time equals the recording start and cannot represent a bounded range." << std::endl;
    has_quit = true;
    return -1;
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

  auto quit_function = [player](int) {
    if VUNLIKELY (has_quit.exchange(true, std::memory_order_acq_rel)) {
      return;
    }

    is_broken = true;

    if VLIKELY (player) {
      player->stop();
      player->quit(true);
    }
  };

  vlink::Utils::register_terminate_signal(quit_function, true);

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

  player->clear_bag_interface();

  recorder->clear_bag_interface();

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
