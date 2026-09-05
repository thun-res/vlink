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

#include "./monitor_common.h"

[[maybe_unused]] std::atomic_bool has_quit{false};

std::atomic_bool has_intra_bind{false};
std::atomic_bool is_paused{false};
std::atomic_bool is_jumped{false};
std::atomic_bool has_update{false};
std::atomic<int> current_page{0};
std::atomic<int> total_pages{1};
std::atomic<size_t> max_url_size{10};
std::atomic<size_t> max_ser_size{10};
std::atomic<int> selected_line{-1};
std::atomic<int> target_row{0};
std::atomic<int> row_count{0};
std::atomic<int> chart_width{30};
std::atomic<int> process_width{40};
std::atomic_bool count_mode{false};
std::atomic_bool black_mode{false};
std::atomic_bool blob_mode{false};
std::atomic_bool native_mode{false};
std::atomic_bool detail_mode{false};
std::atomic_bool observe_all_mode{false};
std::atomic_bool profiler_mode{false};
std::atomic_bool ser_mode{false};
std::atomic_bool active_mode{false};
std::atomic_bool pubsub_mode{false};
std::atomic_bool plain_mode{false};
std::atomic_bool process_mode{false};
std::atomic_bool chart_mode{false};
std::atomic_bool preset_mode{false};
std::atomic_bool filter_input_mode{false};
std::atomic_bool use_chart_dot{false};
std::atomic<int> max_rows{0};
std::atomic<int> max_columns{0};
std::atomic<double> total_profiler{-1};
std::atomic<uint32_t> current_type;
std::atomic<uint32_t> current_schema_type{0};

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
std::pair<int, int> terminal_size{0, 0};
std::vector<std::string> print_lines;
std::atomic<size_t> print_lines_count{0};
std::vector<vlink::DiscoveryViewer::Info> current_info_list;

std::mutex current_mtx;         // NOLINT(runtime/string)
std::string current_url;        // NOLINT(runtime/string)
std::string current_ser;        // NOLINT(runtime/string)
std::string proto_args;         // NOLINT(runtime/string)
std::string filter_input_text;  // NOLINT(runtime/string)
