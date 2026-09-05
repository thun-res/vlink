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
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "./bag_commands.h"
#include "./bag_common.h"

// NOLINTNEXTLINE(google-readability-function-size)
int bag_merge(const std::vector<std::string>& source_paths, const std::string& target_path, const std::string& tag_name,
              bool compress, bool force) {
  vlink::Utils::register_terminate_signal([](int) { has_quit = true; });

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

    for (const auto& path : source_paths) {
#ifdef _WIN32
      const auto source = std::filesystem::path(vlink::Helpers::string_to_wstring(path));
#else
      const auto source = std::filesystem::path(path);
#endif
      if VUNLIKELY (clone_paths_overlap(source, target, false)) {
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
      start_timestamp = std::min(start_timestamp, info.start_timestamp);

      for (const auto& meta : info.url_metas) {
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

    if VUNLIKELY (!force && std::filesystem::exists(target)) {
      std::cerr << "The target file already exists. Use --force to overwrite it." << std::endl;
      return -1;
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
    config.tag_name = tag_name;
    config.compress = compress ? vlink::BagWriter::kCompressAuto : vlink::BagWriter::kCompressNone;
    config.sync_mode = true;
    config.optimize_on_exit = true;
    auto writer = vlink::BagWriter::create(target_path, config);
    if VUNLIKELY (!writer) {
      return -1;
    }

    for (const auto& [key, schema] : schemas) {
      if VUNLIKELY (!writer->push_schema(schema)) {
        std::cerr << "Failed to write schema: " << schema.name << std::endl;
        return -1;
      }
    }

    int64_t count = 0;
    bool write_failed = false;
    while (!pending.empty() && !has_quit) {
      const size_t index = pending.top();
      pending.pop();

      // The cursor owns the payload until its next read; synchronous push consumes it here.
      if VUNLIKELY (writer->push(frames[index]) < 0) {
        write_failed = true;
        break;
      }
      ++count;

      if (read_frame(index)) {
        pending.push(index);
      } else if (read_failed) {
        break;
      }
    }

    writer->close();
    if VUNLIKELY (read_failed || write_failed || writer->fail() || has_quit) {
      std::cerr << "Merge did not complete; the output may be partial." << std::endl;
      return -1;
    }

    if (!quiet_flag) {
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
