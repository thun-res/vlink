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

#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/idl.h>
#include <flatbuffers/reflection.h>
#include <vlink/base/elapsed_timer.h>
#include <vlink/base/helpers.h>
#include <vlink/base/terminal_stream.h>
#include <vlink/base/utils.h>
#include <vlink/extension/discovery_viewer.h>
#include <vlink/extension/schema_plugin_manager.h>
#include <vlink/version.h>
#include <vlink/vlink.h>
#include <vlink/zerocopy/audio_frame.h>
#include <vlink/zerocopy/camera_frame.h>
#include <vlink/zerocopy/message_parser.h>
#include <vlink/zerocopy/occupancy_grid.h>
#include <vlink/zerocopy/point_cloud.h>
#include <vlink/zerocopy/tensor.h>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>
//
#include <argparse/argparse.hpp>

#if __has_include(<unistd.h>)
#include <unistd.h>
#endif

#ifdef _WIN32
#include <Windows.h>
#undef min
#undef max
#undef GetMessage
[[maybe_unused]] static constexpr int kFlushMinSleep{0};
[[maybe_unused]] static constexpr int kFlushMinLine{5};
#elif defined(__APPLE__)
[[maybe_unused]] static constexpr int kFlushMinSleep{10};
[[maybe_unused]] static constexpr int kFlushMinLine{1};
#else
[[maybe_unused]] static constexpr int kFlushMinSleep{10};
[[maybe_unused]] static constexpr int kFlushMinLine{5};
#endif

using RawPub = vlink::Publisher<vlink::Bytes>;
using RawSub = vlink::Subscriber<vlink::Bytes>;
using RawGetter = vlink::Getter<vlink::Bytes>;

[[maybe_unused]] static constexpr size_t kMaxTaskSize{20000U};
[[maybe_unused]] static constexpr int kCounterCache{2};
[[maybe_unused]] static constexpr int kCounterWeight{2};
[[maybe_unused]] static constexpr int kCollectInterval{1000};
[[maybe_unused]] static constexpr int kTerminalInterval{50};
[[maybe_unused]] static constexpr int kMaxElapsedTime{200};

extern std::atomic_bool has_quit;
extern std::atomic_bool has_intra_bind;
extern std::atomic_bool is_paused;
extern std::atomic_bool is_changed;
extern std::atomic_bool has_printed;
extern std::atomic_bool force_update;
extern std::atomic_bool is_fbs_type;
extern std::atomic_bool is_out_of_range;
extern std::atomic_bool black_mode;
extern std::atomic<size_t> max_str_count;
extern std::atomic_bool ignore_array;
extern std::atomic_bool ignore_string;
extern std::atomic_bool ignore_default;
extern std::atomic_bool use_long_repeated;
extern std::atomic_bool print_time_string;
extern std::atomic_bool print_hex_string;
extern std::atomic_bool print_enum_string;
extern std::atomic<int> current_page;
extern std::atomic<int> total_page;
extern std::atomic<int> max_rows;
extern std::atomic<int> max_columns;
extern std::vector<std::string> filter_list;
extern std::pair<int, int> terminal_size;

bool is_text_ser_type(std::string_view ser_type);

bool load_text_for_file(const std::string& filename, std::string& content);

std::filesystem::path utf8_to_path(const std::string& utf8) noexcept;

std::string get_home_config_path(const std::string& filename);

std::string read_home_config(const std::string& filename);

bool write_home_config(const std::string& filename, const std::string& value);

bool format_json_text(const std::string& content, std::string& out);

std::pair<int, int> get_terminal_size();

void decode_terminal_utf8(std::string_view text, size_t index, uint32_t& code_point, size_t& bytes);

int terminal_codepoint_width(uint32_t code_point);

bool import_fbs_from_plugin(std::shared_ptr<flatbuffers::Parser>& parser,
                            const std::shared_ptr<vlink::SchemaPluginInterface>& schema_interface,
                            const std::string& target_ser);

void import_fbs(std::shared_ptr<flatbuffers::Parser>& parser, const std::string& target_ser,
                const std::filesystem::path& root_dir, const std::filesystem::path& sub_dir, bool& has_import,
                int depth = 0);

int start_efbs_sub(const std::string& url, const std::string& fbs_dir, const std::string& ser,
                   vlink::SchemaType schema_type, bool use_blob_encoding, bool native_mode, const std::string& filter,
                   bool use_getter);

int start_efbs_pub(const std::string& url, const std::string& fbs_dir, const std::string& fbstxt_file,
                   const std::string& fbs_json, const std::string& ser, vlink::SchemaType schema_type,
                   bool use_blob_encoding, bool native_mode, int times, int interval);
