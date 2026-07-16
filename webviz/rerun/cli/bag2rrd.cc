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
#include <vlink/base/logger.h>
#include <vlink/base/macros.h>
#include <vlink/base/utils.h>
#include <vlink/extension/bag_reader.h>
#include <vlink/version.h>

#include <algorithm>
#include <argparse/argparse.hpp>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <rerun.hpp>
#include <string>
#include <unordered_map>

#include "./rerun_converter.h"
#include "./webviz_app_utils.h"
#include "./webviz_time_utils.h"

int main(int argc, char* argv[]) {
  vlink::Utils::set_console_utf8_output();

  // init
  vlink::Logger::set_console_level(vlink::Logger::kOff);
  vlink::Logger::set_file_level(vlink::Logger::kOff);
  vlink::Logger::init("vlink-bag2rrd");

  // env
  vlink::Utils::unset_env("VLINK_BAG_PATH");
  // vlink::Utils::set_env("VLINK_DISCOVER_DISABLE", "1");

  // suppress rerun sdk internal logs (rust-based)
  vlink::Utils::set_env("RUST_LOG", "off");

  argparse::ArgumentParser program("vlink-bag2rrd", VLINK_VERSION, argparse::default_arguments::all);

  program.add_argument("input").help("Input VLink bag file (.vdb/.vdbx/.vcap/.vcapx)");

  program.add_argument("-o", "--output").help("Output RRD file path").required();

  program.add_argument("--proto_dir")
      .help("Directory containing VLink .proto files for dynamic parsing")
      .default_value(std::string(""));

  program.add_argument("--fbs_dir")
      .help("Directory containing VLink .fbs files for dynamic FlatBuffers parsing")
      .default_value(std::string(""));

  program.add_argument("--schema_plugin")
      .help(
          "Path to schema plugin shared library (imports protobuf/flatbuffers schemas; alternative to "
          "--proto_dir/--fbs_dir)")
      .default_value(std::string(""));

  program.add_argument("--convert_plugin")
      .help("Path to message conversion plugin shared library")
      .default_value(std::string(""));

  program.add_argument("--convert_plugin_config")
      .help("Configuration string for the conversion plugin")
      .default_value(std::string(""));

  program.add_argument("--vlink_msgs")
      .help("Path to a vlink_msgs mapping JSON file (can be specified multiple times)")
      .append()
      .default_value(std::vector<std::string>{});

  program.add_argument("--name").help("Rerun application ID").default_value(std::string("vlink-bag2rrd"));

  program.add_argument("--sequence_timeline").help("Sequence timeline name").default_value(std::string("seq"));

  program.add_argument("--time_timeline")
      .help("Relative time timeline name for bag playback")
      .default_value(std::string("vlink_time"));

  program.add_argument("--timestamp_timeline")
      .help("Message timestamp timeline name")
      .default_value(std::string("timestamp"));

  program.add_argument("--disable_time_timeline")
      .help("Disable relative bag time timeline logging")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--disable_sequence_timeline")
      .help("Disable sequence timeline logging")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--disable_timestamp_timeline")
      .help("Disable timestamp timeline logging")
      .default_value(false)
      .implicit_value(true);

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    std::cerr << program;
    return 1;
  }

  auto input_path = program.get<std::string>("input");
  auto output_path = program.get<std::string>("--output");
  std::error_code input_ec;

  if VUNLIKELY (!std::filesystem::exists(input_path, input_ec) || input_ec) {
    std::cerr << "Input file not found: " << input_path << std::endl;
    return 1;
  }

  auto proto_dir = vlink::webviz::resolve_arg_or_env(program.get<std::string>("--proto_dir"), "VLINK_PROTO_DIR");
  auto fbs_dir = vlink::webviz::resolve_arg_or_env(program.get<std::string>("--fbs_dir"), "VLINK_FBS_DIR");
  auto schema_plugin_path =
      vlink::webviz::resolve_arg_or_env(program.get<std::string>("--schema_plugin"), "VLINK_SCHEMA_PLUGIN");
  auto convert_plugin_path =
      vlink::webviz::resolve_arg_or_env(program.get<std::string>("--convert_plugin"), "VLINK_CONVERT_PLUGIN");

  auto convert_plugin_config = program.get<std::string>("--convert_plugin_config");
  auto vlink_msgs = program.get<std::vector<std::string>>("--vlink_msgs");
  auto name = program.get<std::string>("--name");
  auto sequence_timeline = program.get<std::string>("--sequence_timeline");
  auto time_timeline = program.get<std::string>("--time_timeline");
  auto timestamp_timeline = program.get<std::string>("--timestamp_timeline");
  const bool use_time_timeline = !program.get<bool>("--disable_time_timeline");
  const bool use_sequence_timeline = !program.get<bool>("--disable_sequence_timeline");
  const bool use_timestamp_timeline = !program.get<bool>("--disable_timestamp_timeline");

  if VUNLIKELY (use_time_timeline && time_timeline.empty()) {
    std::cerr << "--time_timeline must not be empty when enabled" << std::endl;
    return 1;
  }

  if VUNLIKELY (use_sequence_timeline && sequence_timeline.empty()) {
    std::cerr << "--sequence_timeline must not be empty when enabled" << std::endl;
    return 1;
  }

  if VUNLIKELY (use_timestamp_timeline && timestamp_timeline.empty()) {
    std::cerr << "--timestamp_timeline must not be empty when enabled" << std::endl;
    return 1;
  }

  if VUNLIKELY (!vlink::webviz::ensure_parent_directory(output_path)) {
    std::cerr << "Failed to create output directory for: " << output_path << std::endl;
    return 1;
  }

#ifdef _WIN32
  vlink::webviz::normalize_path(input_path);
  vlink::webviz::normalize_path(output_path);
  vlink::webviz::normalize_path(proto_dir);
  vlink::webviz::normalize_path(fbs_dir);
  vlink::webviz::normalize_path(schema_plugin_path);
  vlink::webviz::normalize_path(convert_plugin_path);
  vlink::webviz::normalize_paths(vlink_msgs);
#endif

  auto reader = vlink::BagReader::create(input_path);

  if VUNLIKELY (!reader) {
    std::cerr << "Failed to open bag file: " << input_path << std::endl;
    return 1;
  }

  const auto& info = reader->get_info();

  std::cerr << "Input: " << input_path << std::endl;
  std::cerr << "  Messages: " << info.message_count << std::endl;
  std::cerr << "  Duration: " << info.total_duration << " ms" << std::endl;
  std::cerr << "  URLs: " << info.url_metas.size() << std::endl;

  vlink::webviz::RerunConverter::Config conv_config;
  conv_config.proto_dir = proto_dir;
  conv_config.fbs_dir = fbs_dir;
  conv_config.schema_plugin_path = schema_plugin_path;
  conv_config.convert_plugin_path = convert_plugin_path;
  conv_config.convert_plugin_config = convert_plugin_config;
  conv_config.vlink_msgs = vlink_msgs;
  conv_config.timestamp_timeline = timestamp_timeline;
  conv_config.use_timestamp_timeline = use_timestamp_timeline;

  vlink::webviz::RerunConverter converter(conv_config);

  auto rec = rerun::RecordingStream(name);
  auto save_err = rec.save(output_path);

  if VUNLIKELY (save_err.is_err()) {
    std::cerr << "Failed to save RRD file '" << output_path << "': " << save_err.description << std::endl;
    return 1;
  }

  std::cerr << "Output: " << output_path << std::endl;

  std::unordered_map<std::string, std::string> url_ser_map;
  std::unordered_map<std::string, vlink::SchemaType> url_schema_map;
  std::unordered_map<std::string, int64_t> url_seq_map;

  for (const auto& meta : info.url_metas) {
    if VLIKELY (meta.valid) {
      url_ser_map[meta.url] = meta.ser_type;
      url_schema_map[meta.url] = meta.schema_type;
    }
  }

  std::atomic<uint64_t> msg_processed{0};

  reader->register_output_callback([&converter, &msg_processed, &reader, &rec, &sequence_timeline, &time_timeline,
                                    &url_seq_map, &url_schema_map, &url_ser_map, use_sequence_timeline, &info,
                                    use_time_timeline](const vlink::Frame& frame) {
    const int64_t timestamp_us = frame.timestamp;
    const std::string& url = frame.url;
    const vlink::Bytes& data = frame.data;

    std::string ser_type;
    auto schema_type = vlink::SchemaType::kUnknown;
    auto ser_iter = url_ser_map.find(url);
    auto schema_iter = url_schema_map.find(url);

    if VLIKELY (ser_iter != url_ser_map.end()) {
      ser_type = ser_iter->second;
    } else {
      ser_type = frame.ser_type;
      url_ser_map[url] = ser_type;
    }

    if VLIKELY (schema_iter != url_schema_map.end()) {
      schema_type = schema_iter->second;
    } else {
      schema_type = frame.schema_type;
      url_schema_map[url] = schema_type;
    }

    uint64_t relative_timestamp_us = 0;

    if VLIKELY (timestamp_us >= 0) {
      relative_timestamp_us = static_cast<uint64_t>(timestamp_us);
    }

    auto relative_timestamp_ns = vlink::webviz::micros_to_nanos_saturated(relative_timestamp_us);
    int64_t timestamp_ns = std::numeric_limits<int64_t>::max();

    if VLIKELY (relative_timestamp_ns <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      timestamp_ns = static_cast<int64_t>(relative_timestamp_ns);
    }

    auto seq_iter = url_seq_map.try_emplace(url, 0).first;
    auto local_sequence = seq_iter->second++;

    // Rerun keeps timeline state on the current thread until reset/disable.
    // Clear any previous message state before applying this sample's own time.
    rec.reset_time();

    if VLIKELY (use_time_timeline) {
      rec.set_time_duration_nanos(time_timeline, timestamp_ns);
    }

    if VLIKELY (use_sequence_timeline) {
      rec.set_time_sequence(sequence_timeline, local_sequence);
    }

    std::string entity_path = url;
    auto transport_pos = url.find("://");

    if VLIKELY (transport_pos != std::string::npos) {
      entity_path = url.substr(0, transport_pos) + "/" + url.substr(transport_pos + 3);
    }

    converter.convert_and_log(rec, entity_path, url, schema_type, ser_type, data);

    auto total = ++msg_processed;

    if VUNLIKELY (total % 1000 == 0 && info.message_count > 0) {
      double progress = static_cast<double>(total) / static_cast<double>(info.message_count) * 100.0;

      std::cerr << "\rProgress: " << std::fixed << std::setprecision(1) << progress << "% (" << total << "/"
                << info.message_count << " messages)" << std::flush;
    }
  });

  reader->register_finish_callback([&reader](bool) { reader->quit(); });

  vlink::Utils::register_terminate_signal([&reader](int) { reader->quit(); });

  vlink::BagReader::Config play_config;
  play_config.force_delay = 0;

  reader->play(play_config);
  reader->run();

  auto flush_err = rec.flush_blocking();

  if VUNLIKELY (flush_err.is_err()) {
    std::cerr << std::endl;
    std::cerr << "Rerun flush error: " << flush_err.description << std::endl;
    return 1;
  }

  std::cerr << std::endl;
  std::cerr << "Conversion complete:" << std::endl;
  std::cerr << "  Output: " << output_path << std::endl;
  std::cerr << "  Input messages: " << msg_processed.load() << std::endl;

  std::error_code output_ec;

  if VLIKELY (std::filesystem::exists(output_path, output_ec) && !output_ec) {
    auto output_size = std::filesystem::file_size(output_path, output_ec);

    if VLIKELY (!output_ec) {
      std::cerr << "  File size: " << output_size << " bytes" << std::endl;
    }
  }

  return 0;
}
