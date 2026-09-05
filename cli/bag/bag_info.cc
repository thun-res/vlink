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
#include <vlink/vlink.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>

#include "./bag_commands.h"
#include "./bag_common.h"

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
