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
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "./foxglove_converter.h"
#include "./private/mcap_import.h"
#include "./webviz_app_utils.h"
#include "./webviz_time_utils.h"

int main(int argc, char* argv[]) {
  vlink::Utils::set_console_utf8_output();

  // init
  vlink::Logger::set_console_level(vlink::Logger::kOff);
  vlink::Logger::set_file_level(vlink::Logger::kOff);
  vlink::Logger::init("vlink-bag2mcap");

  // env
  vlink::Utils::unset_env("VLINK_BAG_PATH");
  // vlink::Utils::set_env("VLINK_DISCOVER_DISABLE", "1");

  argparse::ArgumentParser program("vlink-bag2mcap", VLINK_VERSION, argparse::default_arguments::all);

  program.add_argument("input").help("Input VLink bag file (.vdb/.vdbx/.vcap/.vcapx)");

  program.add_argument("-o", "--output").help("Output MCAP file path").required();

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

  program.add_argument("--compression")
      .help("MCAP compression algorithm (none/lz4/zstd)")
      .default_value(std::string("zstd"));

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
  auto compression_str = program.get<std::string>("--compression");

  if VUNLIKELY (compression_str != "none" && compression_str != "lz4" && compression_str != "zstd") {
    std::cerr << "Invalid --compression value: " << compression_str << " (expected none/lz4/zstd)" << std::endl;
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
  const auto recording_start_ns = info.start_timestamp > 0 && static_cast<uint64_t>(info.start_timestamp) <=
                                                                  std::numeric_limits<uint64_t>::max() / 1000000ULL
                                      ? static_cast<uint64_t>(info.start_timestamp) * 1000000ULL
                                      : 0ULL;

  std::cerr << "Input: " << input_path << std::endl;
  std::cerr << "  Messages: " << info.message_count << std::endl;
  std::cerr << "  Duration: " << info.total_duration << " ms" << std::endl;
  std::cerr << "  URLs: " << info.url_metas.size() << std::endl;

  vlink::webviz::FoxgloveConverter::Config conv_config;
  conv_config.proto_dir = proto_dir;
  conv_config.fbs_dir = fbs_dir;
  conv_config.schema_plugin_path = schema_plugin_path;
  conv_config.convert_plugin_path = convert_plugin_path;
  conv_config.convert_plugin_config = convert_plugin_config;
  conv_config.vlink_msgs = vlink_msgs;

  vlink::webviz::FoxgloveConverter converter(conv_config);

  mcap::Compression compression = mcap::Compression::Zstd;

  if VUNLIKELY (compression_str == "none") {
    compression = mcap::Compression::None;
  } else if VUNLIKELY (compression_str == "lz4") {
    compression = mcap::Compression::Lz4;
  }

  mcap::McapWriterOptions options("vlink-bag2mcap");
  options.compression = compression;

  mcap::McapWriter mcap_writer;
  auto status = mcap_writer.open(output_path, options);

  if VUNLIKELY (!status.ok()) {
    std::cerr << "Failed to open MCAP output: " << status.message << std::endl;
    return 1;
  }

  struct UrlChannelState final {
    mcap::ChannelId id{0};
    bool allow_raw_fallback{false};
    std::string signature;
  };

  std::unordered_map<std::string, UrlChannelState> channel_map;
  std::unordered_map<std::string, mcap::ChannelId> channel_signature_map;
  std::unordered_map<std::string, mcap::SchemaId> schema_map;
  std::unordered_map<std::string, std::string> url_ser_map;
  std::unordered_map<std::string, vlink::SchemaType> url_schema_map;
  std::unordered_set<std::string> internal_time_urls;
  std::unordered_set<std::string> invalid_payload_urls;

  auto ensure_schema_id = [&mcap_writer, &schema_map](const std::string& schema_name,
                                                      const std::string& schema_encoding,
                                                      const std::string& schema_data) -> mcap::SchemaId {
    std::string schema_key;
    schema_key.reserve(schema_name.size() + schema_encoding.size() + schema_data.size() + 2);
    schema_key.append(schema_name);
    schema_key.push_back('|');
    schema_key.append(schema_encoding);
    schema_key.push_back('|');
    schema_key.append(schema_data);

    auto schema_iter = schema_map.find(schema_key);

    if VLIKELY (schema_iter != schema_map.end()) {
      return schema_iter->second;
    }

    mcap::Schema schema;
    schema.id = 0;
    schema.name = schema_name;
    schema.encoding = schema_encoding;
    schema.data.assign(reinterpret_cast<const std::byte*>(schema_data.data()),
                       reinterpret_cast<const std::byte*>(schema_data.data() + schema_data.size()));
    mcap_writer.addSchema(schema);
    schema_map[schema_key] = schema.id;
    return schema.id;
  };

  auto ensure_url_channel = [&channel_map, &channel_signature_map, &mcap_writer](
                                const std::string& url, const std::string& channel_encoding, mcap::SchemaId schema_id,
                                bool allow_raw_fallback) -> mcap::ChannelId {
    auto channel_signature = url + "|" + channel_encoding + "|" + std::to_string(schema_id);
    auto channel_iter = channel_signature_map.find(channel_signature);
    mcap::ChannelId channel_id = 0;

    if VLIKELY (channel_iter != channel_signature_map.end()) {
      channel_id = channel_iter->second;
    } else {
      mcap::Channel channel(url, channel_encoding, schema_id);
      mcap_writer.addChannel(channel);
      channel_id = channel.id;
      channel_signature_map[channel_signature] = channel_id;
    }

    channel_map[url] = UrlChannelState{channel_id, allow_raw_fallback, std::move(channel_signature)};
    return channel_id;
  };

  for (const auto& meta : info.url_metas) {
    if VUNLIKELY (!meta.valid) {
      continue;
    }

    url_ser_map[meta.url] = meta.ser_type;
    url_schema_map[meta.url] = meta.schema_type;

    std::string schema_name;
    std::string encoding;
    std::string schema_encoding;
    std::string schema_data;

    if VLIKELY (converter.get_schema_info(meta.url, meta.schema_type, meta.ser_type, schema_name, encoding,
                                          schema_encoding, schema_data)) {
      if VUNLIKELY (encoding == "send_time") {
        internal_time_urls.insert(meta.url);
        continue;
      }

      auto schema_id = ensure_schema_id(schema_name, schema_encoding, schema_data);
      ensure_url_channel(meta.url, encoding, schema_id, false);

      std::cerr << "  Registered: " << meta.url << " -> " << schema_name << std::endl;
    }
  }

  std::atomic<uint64_t> msg_converted{0};
  std::atomic<uint64_t> msg_failed{0};
  std::atomic<uint64_t> msg_skipped{0};
  std::atomic<uint32_t> seq_counter{0};

  reader->register_output_callback([&channel_map, &converter, &ensure_schema_id, &ensure_url_channel, &info,
                                    &internal_time_urls, &invalid_payload_urls, &mcap_writer, &msg_converted,
                                    &msg_failed, &msg_skipped, reader, &seq_counter, &recording_start_ns,
                                    &url_schema_map, &url_ser_map](const vlink::Frame& frame) {
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

    auto channel_iter = channel_map.find(url);

    if VUNLIKELY (channel_iter == channel_map.end()) {
      std::string schema_name;
      std::string encoding;
      std::string schema_encoding;
      std::string schema_data;

      if VLIKELY (converter.get_schema_info(url, schema_type, ser_type, schema_name, encoding, schema_encoding,
                                            schema_data)) {
        if VUNLIKELY (encoding == "send_time") {
          internal_time_urls.insert(url);
          ++msg_skipped;
          return;
        }

        auto schema_id = ensure_schema_id(schema_name, schema_encoding, schema_data);
        ensure_url_channel(url, encoding, schema_id, false);
      } else {
        ensure_url_channel(url, "raw", 0, true);
      }

      channel_iter = channel_map.find(url);
    }

    auto result = converter.convert(url, schema_type, ser_type, data);

    if VUNLIKELY (internal_time_urls.count(url) != 0) {
      ++msg_skipped;
      return;
    }

    uint64_t timestamp_ns = 0;

    if VLIKELY (timestamp_us >= 0) {
      auto relative_timestamp_ns = vlink::webviz::micros_to_nanos_saturated(static_cast<uint64_t>(timestamp_us));
      timestamp_ns = vlink::webviz::add_nanos_saturated(recording_start_ns, relative_timestamp_ns);
    }

    if VLIKELY (result.success && result.timestamp_ns >= 0) {
      timestamp_ns = static_cast<uint64_t>(result.timestamp_ns);
    }

    if VLIKELY (result.success && !result.schema_name.empty()) {
      std::string schema_data = result.schema_data;

      if VUNLIKELY (schema_data.empty() &&
                    !converter.resolve_schema_by_name(result.schema_name, result.schema_encoding, schema_data)) {
        if VLIKELY (invalid_payload_urls.emplace(url).second) {
          MLOG_W("Skip message for {}: failed to resolve schema {} ({})", url, result.schema_name,
                 result.schema_encoding);
        }

        ++msg_failed;
        return;
      }

      auto schema_id = ensure_schema_id(result.schema_name, result.schema_encoding, schema_data);
      auto current_signature = url + "|" + result.encoding + "|" + std::to_string(schema_id);

      if VUNLIKELY (channel_iter->second.signature != current_signature) {
        ensure_url_channel(url, result.encoding, schema_id, false);
        channel_iter = channel_map.find(url);
      }
    }

    if VUNLIKELY (result.success && result.schema_name.empty()) {
      if VLIKELY (invalid_payload_urls.emplace(url).second) {
        MLOG_W("Skip message for {}: converter returned transformed payload without schema metadata", url);
      }

      ++msg_failed;
      return;
    }

    if VUNLIKELY (!result.success) {
      if VUNLIKELY (!channel_iter->second.allow_raw_fallback) {
        if VLIKELY (invalid_payload_urls.emplace(url).second) {
          MLOG_W("Skip message for {}: channel is registered with converted schema, but payload conversion failed",
                 url);
        }

        ++msg_failed;
        return;
      }

      mcap::Message msg;
      msg.channelId = channel_iter->second.id;
      msg.sequence = ++seq_counter;
      msg.logTime = timestamp_ns;
      msg.publishTime = timestamp_ns;
      msg.dataSize = data.size();
      msg.data = reinterpret_cast<const std::byte*>(data.data());
      auto raw_status = mcap_writer.write(msg);

      if VUNLIKELY (!raw_status.ok()) {
        MLOG_W("Failed to write raw message for {}: {}", url, raw_status.message);
        ++msg_failed;
        return;
      }

      ++msg_skipped;
      return;
    }

    mcap::Message msg;
    msg.channelId = channel_iter->second.id;
    msg.sequence = ++seq_counter;
    msg.logTime = timestamp_ns;
    msg.publishTime = timestamp_ns;
    msg.dataSize = result.payload.size();
    msg.data = reinterpret_cast<const std::byte*>(result.payload.data());

    auto write_status = mcap_writer.write(msg);

    if VUNLIKELY (!write_status.ok()) {
      MLOG_W("Failed to write message for {}: {}", url, write_status.message);
      ++msg_failed;
      return;
    }

    ++msg_converted;

    auto total = msg_converted.load() + msg_failed.load() + msg_skipped.load();

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

  mcap_writer.close();
  const auto failed = msg_failed.load();

  std::cerr << std::endl;
  std::cerr << (failed == 0 ? "Conversion complete:" : "Conversion finished with errors:") << std::endl;
  std::cerr << "  Output: " << output_path << std::endl;
  std::cerr << "  Converted: " << msg_converted.load() << std::endl;
  std::cerr << "  Raw fallback/skipped: " << msg_skipped.load() << std::endl;
  std::cerr << "  Failed: " << failed << std::endl;

  std::error_code output_ec;

  if VLIKELY (std::filesystem::exists(output_path, output_ec) && !output_ec) {
    auto output_size = std::filesystem::file_size(output_path, output_ec);

    if VLIKELY (!output_ec) {
      std::cerr << "  File size: " << output_size << " bytes" << std::endl;
    }
  }

  return failed == 0 ? 0 : 1;
}
