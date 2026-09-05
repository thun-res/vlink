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
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "./bag_commands.h"
#include "./bag_common.h"

// NOLINTNEXTLINE(google-readability-function-size)
int bag_play(const std::string& path, const std::vector<std::string>& urls, const std::string& filter, bool black_mode,
             bool native_mode, bool auto_pause, const std::vector<int>& actions, int64_t begin_time, int64_t end_time,
             uint8_t input_time_method, bool has_clock_begin_time, bool has_clock_end_time, int times, double rate,
             const std::string& plugin_name) {
  using RawPub = vlink::Publisher<vlink::Bytes>;

  const std::string native_ip = native_mode ? vlink::Utils::get_env("VLINK_DDS_NATIVE_IP", "127.0.0.1") : std::string();

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

  if (input_time_method == kUseLocalTime || input_time_method == kUseUtcTime) {
    int64_t clock_start = date_time;

    if (input_time_method == kUseLocalTime) {
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
        pub->set_property("dds.ip", native_ip);
      }

      pub->set_ser_type(ser, meta.schema_type);

      try {
        pub->init();
      } catch (const vlink::Exception::RuntimeError& e) {
        std::cerr << e.what() << std::endl;
        has_quit = true;
        return -1;
      }

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
          pub->set_property("dds.ip", native_ip);
        }

        pub->set_ser_type(ser, meta.schema_type);

        try {
          pub->init();
        } catch (const vlink::Exception::RuntimeError& e) {
          std::cerr << e.what() << std::endl;
          has_quit = true;
          return -1;
        }

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

  const int64_t total_time = player->get_info().total_duration;
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
            target_date_time = (date_time + player_ptr->get_info().timezone * 60 * 1000) % kDayMilliseconds;

            if (target_date_time < 0) {
              target_date_time += kDayMilliseconds;
            }
          } else if (time_method == kUseUtcTime) {
            target_date_time = date_time;
          }

          if (!quiet_flag && !detail_flag) {
            const bool restart = last_status == vlink::BagReader::kStopped && !player_ptr->is_ready_to_quit();

            if (skip_blank && begin_time == 0) {
              start_print(player_ptr->get_info().blank_duration, total_time, target_date_time, restart);
            } else {
              start_print(begin_time, total_time, target_date_time, restart);
            }
          }
        }

        last_status = status;

        update_print();
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

    // if (!quiet_flag && !detail_flag) {
    //   stop_print();
    // }
  };

  vlink::Utils::register_terminate_signal(quit_function, true);

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

  player->clear_bag_interface();

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
