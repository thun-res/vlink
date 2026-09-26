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

#include <vlink/base/elapsed_timer.h>
#include <vlink/base/functional.h>
#include <vlink/base/plugin.h>
#include <vlink/extension/bag_plugin_interface.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

enum TimeMethod : uint8_t {
  kUseUnknown = 0,
  kUseRelTime = 1,
  kUseLocalTime = 2,
  kUseUtcTime = 4,
};

[[maybe_unused]] static constexpr int kCollectInterval{1000};
[[maybe_unused]] static constexpr int64_t kDayMilliseconds{24 * 60 * 60 * 1000};

extern std::atomic_bool has_intra_bind;
extern std::atomic_bool data_has_changed;
extern std::atomic_bool is_paused;
extern std::atomic_bool is_broken;
extern std::atomic_bool is_play_mode;
extern std::atomic<uint64_t> pause_total_time;
extern std::atomic<double> play_rate;
extern std::atomic_bool is_split_mode;
extern std::atomic<int> split_count;
extern std::atomic_bool pause_to_next_flag;
extern std::atomic<int> compress_level;
extern std::atomic<int64_t> max_task_depth;
extern std::atomic<double> max_memory_size;
extern std::atomic<size_t> total_size;
extern std::atomic_bool skip_blank;
extern std::atomic<uint8_t> time_method;
extern std::atomic<int> play_loop_index;
extern std::atomic<int> play_loop_times;
extern std::atomic_bool has_quit;
extern bool quiet_flag;
extern bool detail_flag;
extern vlink::ElapsedTimer main_elapsed_timer;
extern vlink::ElapsedTimer pause_elapsed_timer;
extern vlink::Function<int64_t()> time_callback;
extern vlink::Function<int64_t()> split_index_callback;

void start_print(int64_t start_time, int64_t total_time, int64_t date_time, bool restart);
void stop_print();
void update_print();
void reset_print();
void print_progress(double progress);

bool clone_paths_overlap(const std::filesystem::path& source, const std::filesystem::path& target,
                         bool split_name_by_time);

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

  bag->bind_bag_interface(plugin_interface);

  return 0;
}
