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

  if VUNLIKELY (std::filesystem::equivalent(input_path, output_path, input_ec)) {
    std::cerr << "Input and output refer to the same file: " << output_path << std::endl;
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

  if VUNLIKELY (!converter.valid()) {
    std::cerr << "Invalid Foxglove mapping configuration" << std::endl;
    return 1;
  }

  mcap::Compression compression = mcap::Compression::Zstd;

  if VUNLIKELY (compression_str == "none") {
    compression = mcap::Compression::None;
  } else if VUNLIKELY (compression_str == "lz4") {
    std::cerr << "LZ4 is unavailable in this VLink build; writing uncompressed MCAP" << std::endl;
    compression = mcap::Compression::None;
  }

  mcap::McapWriterOptions options("vlink-bag2mcap");
  options.compression = compression;

  mcap::McapWriter mcap_writer;
  auto status = mcap_writer.open(output_path, options);

  if VUNLIKELY (!status.ok()) {
    std::cerr << "Failed to open MCAP output: " << status.message << std::endl;
    return 1;
  }

  struct Output final {
    mcap::ChannelId channel{0};
    std::string name;
    std::string encoding;
    std::string schema_encoding;
    std::string plugin_schema;
  };
  struct Stream final {
    vlink::webviz::FoxgloveRoute route;
    std::vector<Output> outputs;
    mcap::ChannelId raw_channel{0};
  };
  std::unordered_map<std::string, Stream> streams;
  std::unordered_map<std::string, mcap::SchemaId> schemas;
  std::unordered_map<std::string, mcap::ChannelId> channels;
  const auto add_stream = [&](const std::string& url, vlink::SchemaType type, const std::string& ser) -> Stream& {
    Stream stream;
    stream.route = converter.resolve(url, type, ser);
    stream.outputs.resize(stream.route.outputs.size());
    return streams.insert_or_assign(url, std::move(stream)).first->second;
  };
  for (const auto& meta : info.url_metas) {
    if (meta.valid) {
      add_stream(meta.url, meta.schema_type, meta.ser_type);
    }
  }
  const auto ensure_channel = [&](const std::string& url, const std::string& encoding, const std::string& name,
                                  const std::string& schema_encoding, const std::string& schema_data) {
    mcap::SchemaId schema_id = 0;
    if (!name.empty()) {
      const auto key = name + "|" + schema_encoding + "|" + schema_data;
      const auto found = schemas.find(key);
      if (found == schemas.end()) {
        mcap::Schema schema;
        schema.name = name;
        schema.encoding = schema_encoding;
        schema.data.assign(reinterpret_cast<const std::byte*>(schema_data.data()),
                           reinterpret_cast<const std::byte*>(schema_data.data() + schema_data.size()));
        mcap_writer.addSchema(schema);
        schema_id = schemas.emplace(key, schema.id).first->second;
      } else {
        schema_id = found->second;
      }
    }
    const auto key = url + "|" + encoding + "|" + std::to_string(schema_id);
    const auto found = channels.find(key);
    if (found != channels.end()) {
      return found->second;
    }
    mcap::Channel channel(url, encoding, schema_id);
    mcap_writer.addChannel(channel);
    return channels.emplace(key, channel.id).first->second;
  };
  std::atomic<uint64_t> msg_converted{0};
  std::atomic<uint64_t> msg_failed{0};
  std::atomic<uint64_t> msg_skipped{0};
  uint32_t sequence = 0;
  const auto write_message = [&](mcap::ChannelId channel, uint64_t timestamp, const vlink::Bytes& payload) {
    mcap::Message message;
    message.channelId = channel;
    message.sequence = ++sequence;
    message.logTime = message.publishTime = timestamp;
    message.dataSize = payload.size();
    message.data = reinterpret_cast<const std::byte*>(payload.data());
    const auto status = mcap_writer.write(message);
    if (!status.ok()) {
      MLOG_E("Failed to write MCAP message: {}", status.message);
    }
    return status.ok();
  };
  reader->register_output_callback([&](const vlink::Frame& frame) {
    const auto found = streams.find(frame.url);
    auto& stream = found == streams.end() ? add_stream(frame.url, frame.schema_type, frame.ser_type) : found->second;
    uint64_t timestamp = 0;
    if (frame.timestamp >= 0) {
      timestamp = vlink::webviz::add_nanos_saturated(
          recording_start_ns, vlink::webviz::micros_to_nanos_saturated(static_cast<uint64_t>(frame.timestamp)));
    }
    if (!stream.route.valid) {
      ++msg_failed;
      return;
    }
    if (stream.route.outputs.empty()) {
      if (stream.raw_channel == 0) {
        stream.raw_channel = ensure_channel(frame.url, "raw", {}, {}, {});
      }
      if (write_message(stream.raw_channel, timestamp, frame.data)) {
        ++msg_skipped;
      } else {
        ++msg_failed;
      }
      return;
    }
    auto results = converter.convert(stream.route, frame.data);
    bool failed = false;
    bool written = false;
    for (const auto& result : results) {
      if (!result.success) {
        failed = true;
        continue;
      }
      if (result.encoding == "send_time") {
        continue;
      }
      auto& output = stream.outputs[result.output];
      const bool plugin = stream.route.outputs[result.output].plugin;
      if (output.channel == 0 || output.name != result.schema_name || output.encoding != result.encoding ||
          output.schema_encoding != result.schema_encoding || (plugin && output.plugin_schema != result.schema_data)) {
        const auto& advertised = stream.route.outputs[result.output].schema;
        std::string schema = result.schema_data;
        const bool original =
            advertised.schema_name == result.schema_name && advertised.schema_encoding == result.schema_encoding;
        if (!plugin && !original &&
            !converter.resolve_schema_by_name(result.schema_name, result.schema_encoding, schema)) {
          failed = true;
          continue;
        }
        output.channel = ensure_channel(frame.url, result.encoding, result.schema_name, result.schema_encoding,
                                        !plugin && original ? advertised.schema_data : schema);
        output.name = result.schema_name;
        output.encoding = result.encoding;
        output.schema_encoding = result.schema_encoding;
        if (plugin) {
          output.plugin_schema = std::move(schema);
        }
      }
      const auto sample_time = result.timestamp_ns < 0 ? timestamp : static_cast<uint64_t>(result.timestamp_ns);
      if (write_message(output.channel, sample_time, result.payload)) {
        written = true;
      } else {
        failed = true;
      }
    }
    if (failed) {
      ++msg_failed;
      MLOG_W("Failed to convert message: {} ({})", frame.url, stream.route.ser);
    } else if (written) {
      ++msg_converted;
    } else {
      ++msg_skipped;
    }
    const auto total = msg_converted.load() + msg_failed.load() + msg_skipped.load();
    if (total % 1000 == 0 && info.message_count > 0) {
      std::cerr << "\rProgress: " << total << "/" << info.message_count << " messages" << std::flush;
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
