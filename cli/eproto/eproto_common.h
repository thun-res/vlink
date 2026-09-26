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
#include <vlink/extension/schema_plugin_manager.h>
#include <vlink/version.h>
#include <vlink/vlink.h>
#include <vlink/zerocopy/audio_frame.h>
#include <vlink/zerocopy/camera_frame.h>
#include <vlink/zerocopy/message_parser.h>
#include <vlink/zerocopy/occupancy_grid.h>
#include <vlink/zerocopy/point_cloud.h>
#include <vlink/zerocopy/tensor.h>

#if __has_include(<google/protobuf/compiler/importer.h>) && __has_include(<google/protobuf/text_format.h>)

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcpp"
#endif

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/message.h>
#include <google/protobuf/reflection.h>
#include <google/protobuf/stubs/common.h>
#include <google/protobuf/text_format.h>

#if __has_include(<google/protobuf/util/json_util.h>)
#include <google/protobuf/util/json_util.h>
#define VLINK_HAS_PROTOBUF_JSON_UTIL
#endif
#if GOOGLE_PROTOBUF_VERSION >= 3004000
#define VLINK_HAS_PROTOBUF_COMPILER
#endif

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#endif

#include <argparse/argparse.hpp>
#include <nlohmann/json.hpp>
//
#include <algorithm>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
extern std::atomic_bool is_proto_type;
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

#ifdef VLINK_HAS_PROTOBUF_COMPILER
bool load_proto_for_file(const std::string& filename, ::google::protobuf::Message* message);

bool load_proto_for_string(const std::string& content, ::google::protobuf::Message* message);

bool convert_proto_to_txt(std::string& content, ::google::protobuf::Message* message);

#ifdef VLINK_HAS_PROTOBUF_JSON_UTIL
bool load_proto_for_json_string(const std::string& content, ::google::protobuf::Message* message,
                                std::string* error = nullptr);

bool load_proto_for_json_file(const std::string& filename, ::google::protobuf::Message* message,
                              std::string* error = nullptr);

bool convert_proto_to_json(std::string& content, const ::google::protobuf::Message* message,
                           std::string* error = nullptr);
#endif

void set_proto_value_to_default(google::protobuf::Message* message);

void import_protos(google::protobuf::compiler::Importer* importer, const std::filesystem::path& root_dir,
                   const std::filesystem::path& sub_dir, bool& has_import, int depth = 0);

int start_eproto_sub(const std::string& url, const std::string& proto_dir, const std::string& ser,
                     vlink::SchemaType schema_type, bool use_blob_encoding, bool native_mode, const std::string& filter,
                     bool use_getter, bool use_json_format);

int start_eproto_pub(const std::string& url, const std::string& proto_dir, const std::string& prototxt_file,
                     const std::string& prototxt_content, const std::string& ser, vlink::SchemaType schema_type,
                     bool use_blob_encoding, bool native_mode, int times, int interval, bool use_json_format);
#endif
