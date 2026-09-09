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
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "./bag_commands.h"
#include "./bag_common.h"

// NOLINTNEXTLINE(google-readability-function-size)
int bag_merge(const std::vector<std::string>& source_paths, const std::string& target_path,
              const std::vector<std::string>& urls, const std::string& tag_name, const std::string& filter,
              bool black_mode, const std::vector<int>& actions, int64_t begin_time, int64_t end_time,
              bool has_clock_begin_time, bool has_clock_end_time, bool compress, bool split_name_by_time,
              double split_by_size, int64_t split_by_time, bool force, bool wal_mode, double cache_size,
              const std::vector<std::string>& ignore_compress, const std::string& plugin_name) {
  vlink::Plugin plugin;

  auto quit_function = [](int) { has_quit = true; };

  vlink::Utils::register_terminate_signal(quit_function);

  try {
#ifdef _WIN32
    const auto target = std::filesystem::path(vlink::Helpers::string_to_wstring(target_path));
#else
    const auto target = std::filesystem::path(target_path);
#endif
    std::vector<std::shared_ptr<vlink::BagReader>> readers;
    readers.reserve(source_paths.size());
    std::unordered_map<std::string, std::pair<std::string, vlink::SchemaType>> url_types;
    std::map<std::pair<vlink::SchemaType, std::string>, vlink::SchemaData> schemas;
    int64_t start_timestamp = std::numeric_limits<int64_t>::max();
    int32_t timezone = 0;
    double total_count = 0;
    std::unordered_set<std::string> final_urls_set;
    auto filter_list = vlink::Helpers::split_any(filter);

    for (auto& keyword : filter_list) {
      std::transform(keyword.begin(), keyword.end(), keyword.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }

    auto select_url = [&](const std::string& url) {
      if (!urls.empty()) {
        const bool found = std::find(urls.begin(), urls.end(), url) != urls.end();

        if (found == black_mode) {
          return false;
        }
      }

      if (!filter_list.empty()) {
        std::string lower_url = url;
        std::transform(lower_url.begin(), lower_url.end(), lower_url.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool found = std::any_of(filter_list.begin(), filter_list.end(), [&](const std::string& keyword) {
          return !keyword.empty() && lower_url.find(keyword) != std::string::npos;
        });

        if (found == black_mode) {
          return false;
        }
      }

      return true;
    };

    for (const auto& path : source_paths) {
#ifdef _WIN32
      const auto source = std::filesystem::path(vlink::Helpers::string_to_wstring(path));
#else
      const auto source = std::filesystem::path(path);
#endif
      if VUNLIKELY (clone_paths_overlap(source, target, split_name_by_time)) {
        std::cerr << "The merge output overlaps a source bag file: " << path << std::endl;
        return -1;
      }

      auto reader = vlink::BagReader::create(path);

      if VUNLIKELY (!reader) {
        return -1;
      }

      const auto& info = reader->get_info();

      if VUNLIKELY (info.start_timestamp <= 0 || info.start_timestamp > std::numeric_limits<int64_t>::max() / 1000) {
        std::cerr << "Invalid recording start timestamp: " << path << std::endl;
        return -1;
      }

      if (info.start_timestamp < start_timestamp) {
        start_timestamp = info.start_timestamp;
        timezone = info.timezone;
      }

      total_count += info.message_count;

      for (const auto& meta : info.url_metas) {
        if (!select_url(meta.url)) {
          continue;
        }

        final_urls_set.emplace(meta.url);

        const auto [iter, inserted] = url_types.try_emplace(meta.url, meta.ser_type, meta.schema_type);

        if VUNLIKELY (!inserted && (iter->second.first != meta.ser_type || iter->second.second != meta.schema_type)) {
          std::cerr << "Conflicting serialization types for URL: " << meta.url << std::endl;
          return -1;
        }
      }

      for (auto& schema : reader->detect_schema()) {
        auto key = std::make_pair(schema.schema_type, schema.name);
        const auto iter = schemas.find(key);

        if (iter != schemas.end()) {
          if VUNLIKELY (iter->second.encoding != schema.encoding || iter->second.data != schema.data) {
            std::cerr << "Conflicting embedded schema: " << schema.name << std::endl;
            return -1;
          }
        } else {
          schemas.emplace(std::move(key), std::move(schema));
        }
      }

      readers.emplace_back(std::move(reader));

      if VUNLIKELY (has_quit) {
        return -1;
      }
    }

    int64_t total_duration = 0;
    int64_t blank_duration = std::numeric_limits<int64_t>::max();

    for (const auto& reader : readers) {
      const auto& info = reader->get_info();
      const int64_t offset = info.start_timestamp - start_timestamp;

      if VUNLIKELY (info.total_duration < 0 ||
                    info.total_duration > std::numeric_limits<int64_t>::max() / 1000 - offset) {
        std::cerr << "Invalid recording duration." << std::endl;
        return -1;
      }

      total_duration = std::max(total_duration, offset + info.total_duration);
      blank_duration = std::min(blank_duration, offset + info.blank_duration);
    }

    if (time_method == kUseLocalTime || time_method == kUseUtcTime) {
      int64_t clock_start = start_timestamp % kDayMilliseconds;

      if (time_method == kUseLocalTime) {
        clock_start = (clock_start + static_cast<int64_t>(timezone) * 60 * 1000) % kDayMilliseconds;

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
      return -1;
    }

    if VUNLIKELY (begin_time < 0 || end_time < 0 || (end_time > 0 && begin_time > end_time) ||
                  begin_time > total_duration || end_time > total_duration) {
      std::cerr << "Invalid time (merged duration error)." << std::endl;
      return -1;
    }

    if VUNLIKELY (!force && std::filesystem::exists(target)) {
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

      vlink::Utils::register_terminate_signal(quit_function);
    }

    std::vector<vlink::Frame> frames(readers.size());
    std::vector<int64_t> previous_timestamps(readers.size(), -1);
    int64_t max_timestamp = std::numeric_limits<int64_t>::max();
    std::string suffix = target.extension().string();
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return std::tolower(c); });
    if (suffix == ".vcap" || suffix == ".vcapx") {
      if VUNLIKELY (start_timestamp > std::numeric_limits<int64_t>::max() / 1000000) {
        std::cerr << "Recording start timestamp exceeds the VCAP range." << std::endl;
        return -1;
      }
      max_timestamp = (std::numeric_limits<int64_t>::max() - start_timestamp * 1000000) / 1000;
    }

    auto later = [&frames](size_t left, size_t right) {
      if (frames[left].timestamp != frames[right].timestamp) {
        return frames[left].timestamp > frames[right].timestamp;
      }
      return left > right;
    };
    std::priority_queue<size_t, std::vector<size_t>, decltype(later)> pending(later);
    bool read_failed = false;

    auto read_frame = [&](size_t index) {
      auto& reader = readers[index];
      auto& frame = frames[index];
      if (!reader->read_next(frame)) {
        if VUNLIKELY (reader->fail()) {
          std::cerr << "Failed to read bag: " << source_paths[index] << std::endl;
          read_failed = true;
        }
        return false;
      }

      const int64_t offset = (reader->get_info().start_timestamp - start_timestamp) * 1000;
      if VUNLIKELY (frame.timestamp < 0 || frame.timestamp < previous_timestamps[index] || offset > max_timestamp ||
                    frame.timestamp > max_timestamp - offset) {
        std::cerr << "Invalid or decreasing frame timestamp in bag: " << source_paths[index] << std::endl;
        read_failed = true;
        return false;
      }
      previous_timestamps[index] = frame.timestamp;
      frame.timestamp += offset;
      return true;
    };

    for (size_t i = 0; i < readers.size(); ++i) {
      if (read_frame(i)) {
        pending.push(i);
      }
    }

    if VUNLIKELY (read_failed || has_quit) {
      return -1;
    }

    vlink::BagWriter::Config config;
    config.start_timestamp = start_timestamp;
    config.cache_size = 1024LL * 1024LL * cache_size;
    config.wal_mode = wal_mode;
    config.split_name_by_time = split_name_by_time;
    config.split_by_size = 1024LL * 1024LL * 1024LL * split_by_size;
    config.split_by_time = split_by_time;
    config.begin_time = std::max(begin_time, blank_duration);
    config.compress_level = compress_level.load();
    config.ignore_compress_urls.insert(ignore_compress.begin(), ignore_compress.end());
    config.tag_name = tag_name;
    config.compress = compress ? vlink::BagWriter::kCompressAuto : vlink::BagWriter::kCompressNone;
    config.sync_mode = true;
    config.optimize_on_exit = true;

    auto writer = vlink::BagWriter::create(target_path, config);

    if VUNLIKELY (!writer) {
      return -1;
    }

    if VUNLIKELY (load_and_bind_bag_plugin(plugin, plugin_name, writer) != 0) {
      return -1;
    }

    for (const auto& [key, schema] : schemas) {
      if VUNLIKELY (!writer->push_schema(schema)) {
        std::cerr << "Failed to write schema: " << schema.name << std::endl;
        return -1;
      }
    }

    int64_t count = 0;
    int64_t processed_count = 0;
    bool write_failed = false;
    vlink::ElapsedTimer progress_timer;
    progress_timer.start();

    if (!quiet_flag && !detail_flag) {
      print_progress(0.0);
    }

    while (!pending.empty() && !has_quit) {
      const size_t index = pending.top();
      pending.pop();

      ++processed_count;

      if (!quiet_flag && !detail_flag && progress_timer.get() >= 50) {
        print_progress(total_count > 0 ? std::min(processed_count / total_count, 0.9999) : 0.0);
        progress_timer.restart();
      }

      const auto& frame = frames[index];
      const bool selected =
          final_urls_set.count(frame.url) != 0 && frame.timestamp >= begin_time * 1000 &&
          (end_time == 0 || frame.timestamp <= end_time * 1000) &&
          (frame.action_type == vlink::ActionType::kUnknownAction ||
           std::find(actions.begin(), actions.end(), static_cast<int>(frame.action_type)) != actions.end());

      if (selected) {
        if (frames[index].action_type == vlink::ActionType::kUnknownAction) {
          frames[index].action_type = vlink::ActionType::kSubscribe;
        }

        if VUNLIKELY (writer->push(frames[index]) < 0) {
          write_failed = true;
          break;
        }

        ++count;

        if (!quiet_flag && detail_flag) {
          std::cout << std::fixed << std::setprecision(6) << frame.timestamp / 1000000.0 << "s " << frame.url
                    << std::endl;
        }
      }

      if (read_frame(index)) {
        pending.push(index);
      } else if (read_failed) {
        break;
      }
    }

    writer->clear_bag_interface();
    writer->close();

    if VUNLIKELY (read_failed || write_failed || writer->fail() || has_quit) {
      if (!quiet_flag && !detail_flag) {
        std::cout << std::endl;
      }

      std::cerr << "Merge did not complete; the output may be partial." << std::endl;
      return -1;
    }

    if (!quiet_flag) {
      if (!detail_flag) {
        print_progress(1.0);
        std::cout << std::endl;
      }

      std::cout << "Merged " << count << " frames from " << readers.size() << " bags." << std::endl;
    }

    has_quit = true;
    return 0;
  } catch (const std::filesystem::filesystem_error& e) {
    std::cerr << e.what() << std::endl;
  } catch (const nlohmann::json::exception& e) {
    std::cerr << e.what() << std::endl;
  } catch (const vlink::Exception::RuntimeError& e) {
    std::cerr << e.what() << std::endl;
  }

  has_quit = true;
  return -1;
}
