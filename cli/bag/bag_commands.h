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

#pragma once

#include <cstdint>
#include <string>
#include <vector>

int bag_info(const std::string& path);

int bag_record(const std::string& path, const std::vector<std::string>& urls, const std::string& tag_name,
               const std::string& filter, bool black_mode, bool native_mode, double duration, double wait_time,
               bool compress, bool force, int64_t max_row_count, double max_bytes_size, bool enable_limit,
               bool split_name_by_time, double split_by_size, int64_t split_by_time, int64_t max_split_count, bool deft,
               double max_packet_size, bool wal_mode, double cache_size, bool sync_mode,
               const std::vector<std::string>& ignore_compress, const std::string& plugin_name);

int bag_play(const std::string& path, const std::vector<std::string>& urls, const std::string& filter, bool black_mode,
             bool native_mode, bool auto_pause, const std::vector<int>& actions, int64_t begin_time, int64_t end_time,
             uint8_t input_time_method, bool has_clock_begin_time, bool has_clock_end_time, int times, double rate,
             const std::string& plugin_name);

int bag_clone(const std::string& source_path, const std::string& target_path, const std::vector<std::string>& urls,
              const std::string& tag_name, const std::string& filter, bool black_mode, const std::vector<int>& actions,
              int64_t begin_time, int64_t end_time, bool has_clock_begin_time, bool has_clock_end_time, bool compress,
              bool split_name_by_time, double split_by_size, int64_t split_by_time, bool force, bool wal_mode,
              double cache_size, const std::vector<std::string>& ignore_compress, const std::string& plugin_name);

int bag_merge(const std::vector<std::string>& source_paths, const std::string& target_path, const std::string& tag_name,
              bool compress, bool force);

int bag_check(const std::string& path);

int bag_reindex(const std::string& path);

int bag_fix(const std::string& path, bool rebuild_mode);

int bag_tag(const std::string& path, const std::string& tag_name);
