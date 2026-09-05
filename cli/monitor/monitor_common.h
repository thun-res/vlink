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
#include <vlink/base/helpers.h>
#include <vlink/base/terminal_stream.h>
#include <vlink/base/utils.h>
#include <vlink/extension/discovery_viewer.h>
#include <vlink/version.h>
#include <vlink/vlink.h>

#include <argparse/argparse.hpp>
//
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if __has_include(<unistd.h>)
#include <unistd.h>
#endif

#ifndef _WIN32
#include <spawn.h>
#include <sys/wait.h>
#endif

#ifdef __APPLE__
#include <crt_externs.h>
#endif

#ifdef _WIN32
#include <Windows.h>
#undef min
#undef max
#undef GetMessage
[[maybe_unused]] static constexpr int kFlushMinSleep{0};
[[maybe_unused]] static constexpr int kFlushMinLine{5};
#elif defined(__APPLE__)
[[maybe_unused]] static constexpr int kFlushMinSleep{50};
[[maybe_unused]] static constexpr int kFlushMinLine{1};
#else
[[maybe_unused]] static constexpr int kFlushMinSleep{50};
[[maybe_unused]] static constexpr int kFlushMinLine{5};

extern char** environ;
#endif

[[maybe_unused]] static constexpr int kCounterCache{2};
[[maybe_unused]] static constexpr int kCounterWeight{2};
[[maybe_unused]] static constexpr int kCollectInterval{1000};
[[maybe_unused]] static constexpr int kTerminalInterval{50};
[[maybe_unused]] static constexpr int kMaxElapsedTime{200};
[[maybe_unused]] static constexpr int kChartHeight{30};
[[maybe_unused]] static constexpr uint64_t kSubscriberRetryDelayNs{5'000'000'000ULL};

extern std::atomic_bool has_quit;
extern std::atomic_bool has_intra_bind;
extern std::atomic_bool is_paused;
extern std::atomic_bool is_jumped;
extern std::atomic_bool has_update;
extern std::atomic<int> current_page;
extern std::atomic<int> total_pages;
extern std::atomic<size_t> max_url_size;
extern std::atomic<size_t> max_ser_size;
extern std::atomic<int> selected_line;
extern std::atomic<int> target_row;
extern std::atomic<int> row_count;
extern std::atomic<int> chart_width;
extern std::atomic<int> process_width;
extern std::atomic_bool count_mode;
extern std::atomic_bool black_mode;
extern std::atomic_bool blob_mode;
extern std::atomic_bool native_mode;
extern std::atomic_bool detail_mode;
extern std::atomic_bool observe_all_mode;
extern std::atomic_bool profiler_mode;
extern std::atomic_bool ser_mode;
extern std::atomic_bool active_mode;
extern std::atomic_bool pubsub_mode;
extern std::atomic_bool plain_mode;
extern std::atomic_bool process_mode;
extern std::atomic_bool chart_mode;
extern std::atomic_bool preset_mode;
extern std::atomic_bool filter_input_mode;
extern std::atomic_bool use_chart_dot;
extern std::atomic<int> max_rows;
extern std::atomic<int> max_columns;
extern std::atomic<double> total_profiler;
extern std::atomic<uint32_t> current_type;
extern std::atomic<uint32_t> current_schema_type;
extern std::pair<int, int> terminal_size;
extern std::vector<std::string> print_lines;
extern std::atomic<size_t> print_lines_count;
extern std::vector<vlink::DiscoveryViewer::Info> current_info_list;
extern std::mutex current_mtx;
extern std::string current_url;
extern std::string current_ser;
extern std::string proto_args;
extern std::string filter_input_text;

std::pair<int, int> get_terminal_size();

int filter_box_display_width(const std::string& line);

size_t filter_box_col_to_byte(const std::string& line, int target_col);

std::string filter_box_sgr_prefix(const std::string& line, size_t limit);

std::string filter_highlight_url(const std::string& url, const std::vector<std::string>& terms);

bool append_command_arguments(const std::string& text, std::vector<std::string>& args);

int run_decoder_process(const std::string& executable, const std::vector<std::string>& args);

int start_monitor(const std::vector<std::string>& urls, const std::string& filter, const std::string& hostname_filter,
                  const std::string& proto_dir, const std::string& fbs_dir);
