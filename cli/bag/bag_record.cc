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
#include <vlink/extension/bag_writer.h>
#include <vlink/extension/discovery_viewer.h>
#include <vlink/vlink.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "./bag_commands.h"
#include "./bag_common.h"

// NOLINTNEXTLINE(google-readability-function-size)
int bag_record(const std::string& path, const std::vector<std::string>& urls, const std::string& tag_name,
               const std::string& filter, bool black_mode, bool native_mode, double duration, double wait_time,
               bool compress, bool force, int64_t max_row_count, double max_bytes_size, bool enable_limit,
               bool split_name_by_time, double split_by_size, int64_t split_by_time, int64_t max_split_count, bool deft,
               double max_packet_size, bool wal_mode, double cache_size, bool sync_mode,
               const std::vector<std::string>& ignore_compress, const std::string& plugin_name) {
  using RawSub = vlink::Subscriber<vlink::Bytes>;

  const std::string native_ip = native_mode ? vlink::Utils::get_env("VLINK_DDS_NATIVE_IP", "127.0.0.1") : std::string();

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
      recorder->clear_bag_interface();

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
  config.max_split_count = max_split_count;
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

  auto update_urls_function = [&target_urls_set, &filter_list, &recorder, &sub_map, &subs_mtx, &status, &native_ip,
                               black_mode, native_mode,
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

      {
        std::lock_guard lock(subs_mtx);
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
            sub_map.erase(sub_iter);
          } else {
            continue;
          }
        }
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
          sub->set_property("dds.ip", native_ip);
        }

        sub->set_ser_type(info.ser_type, info.schema_type);
        sub->init();
      } catch (const std::runtime_error&) {
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

    recorder->clear_bag_interface();

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
