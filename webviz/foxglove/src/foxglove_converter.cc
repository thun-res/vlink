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

#include "./foxglove_converter.h"

#include <vlink/base/logger.h>

#include <Log.fbs.hpp>

#include "../../webviz_loader_utils.h"
#include "./foxglove_writer.h"

namespace vlink {
namespace webviz {

static std::string wire_encoding(std::string_view encoding) {
  return encoding == "flatbuffers" || encoding == "flatbuffer" || encoding == "fbs" || encoding == "bfbs"
             ? "flatbuffer"
             : std::string(encoding);
}

static std::string native_schema(const std::string& ser) {
  using Parser = zerocopy::MessageParser;
  switch (Parser::detect_type(ser)) {
    case Parser::kCameraFrame:
      return "foxglove.RawImage";
    case Parser::kPointCloud:
      return "foxglove.PointCloud";
    case Parser::kOccupancyGrid:
      return "foxglove.Grid";
    case Parser::kObjectArray:
      return "foxglove.SceneUpdate";
    case Parser::kAudioFrame:
      return "foxglove.RawAudio";
    case Parser::kTensor:
    case Parser::kRawData:
      return "foxglove.Log";
    default:
      return {};
  }
}

FoxgloveConverter::FoxgloveConverter(const Config& config) : registry_(config), mappings_(config.vlink_msgs, "schema") {
  mappings_.validate(validate_foxglove_mapping);
  load_convert_plugin(config.convert_plugin_path, config.convert_plugin_config, plugin_loader_, plugin_);
}

bool FoxgloveConverter::describe(SchemaType type, const std::string& ser, FoxgloveOutput& output) {
  auto& result = output.schema;
  const auto* mapping = output.mapping;
  result.encoding = "flatbuffer";
  result.schema_encoding = "flatbuffer";
  if (mapping && mapping->converter == "send_time") {
    result.is_send_time = true;
    const auto* source = registry_.find(ser, type);
    if (source) {
      result.schema_name = source->name;
      result.encoding = result.schema_encoding = type == SchemaType::kProtobuf ? "protobuf" : "flatbuffer";
      result.schema_data = source->data;
    } else {
      result.schema_name = "SendTime";
      result.encoding = "send_time";
      result.schema_encoding.clear();
    }
    return true;
  }
  if (mapping) {
    result.schema_name = native_schema(native_ser(mapping->converter));
    output.schema_from_payload = result.schema_name == "foxglove.RawImage";
    if (result.schema_name.empty()) {
      result.schema_name = mapping->target.empty() ? ser : mapping->target;
      result.schema_encoding = wire_encoding(mapping->schema_encoding);
      result.encoding = mapping->converter == "passthrough" ? wire_encoding(mapping->encoding) : result.schema_encoding;
    }
  } else if (type == SchemaType::kZeroCopy) {
    result.schema_name = native_schema(ser);
    output.schema_from_payload = result.schema_name == "foxglove.RawImage";
  }
  if (!mapping && result.schema_name.empty() && plugin_ &&
      plugin_->can_convert(ser, ConvertPluginInterface::Target::kFoxglove)) {
    ConvertPluginInterface::SchemaInfo schema;
    if (!plugin_->get_schema(ser, ConvertPluginInterface::Target::kFoxglove, schema)) {
      return false;
    }
    output.plugin = true;
    result.schema_name = std::move(schema.type_name);
    result.encoding = wire_encoding(schema.encoding);
    result.schema_encoding = wire_encoding(schema.schema_encoding);
    result.schema_data = std::move(schema.schema_data);
    result.is_send_time = result.schema_name == "SendTime";
    if (result.is_send_time) {
      result.encoding = "send_time";
    }
    return !result.schema_name.empty();
  }
  if (!mapping && result.schema_name.empty()) {
    if (type == SchemaType::kRaw && is_text_ser(ser)) {
      result.schema_name = "foxglove.Log";
    } else if (type == SchemaType::kProtobuf || type == SchemaType::kFlatbuffers) {
      result.schema_name = ser;
      result.encoding = result.schema_encoding = type == SchemaType::kProtobuf ? "protobuf" : "flatbuffer";
    }
  }
  return !result.schema_name.empty() &&
         resolve_schema_by_name(result.schema_name, result.schema_encoding, result.schema_data);
}

FoxgloveRoute FoxgloveConverter::resolve(std::string_view url, SchemaType type, const std::string& ser) {
  FoxgloveRoute route;
  route.type = type;
  route.ser = ser;
  route.source = registry_.find(ser, type);
  bool ambiguous = false;
  auto mappings = mappings_.select(url, ser, &ambiguous);
  if (ambiguous || !mappings_.valid()) {
    route.valid = false;
    return route;
  }
  if (mappings.empty()) {
    mappings.push_back(nullptr);
  }
  for (const auto* mapping : mappings) {
    FoxgloveOutput output;
    output.mapping = mapping;
    if (describe(type, ser, output)) {
      route.outputs.push_back(std::move(output));
    } else if (mapping || (plugin_ && plugin_->can_convert(ser, ConvertPluginInterface::Target::kFoxglove))) {
      route.valid = false;
    }
  }
  return route;
}

std::vector<FoxgloveMessage> FoxgloveConverter::convert(const FoxgloveRoute& route, const Bytes& raw) {
  if (!route.valid) {
    return {};
  }
  DecodedMessage source;
  bool decoded = false;
  bool attempted_decode = false;
  const auto decode = [&] {
    if (!attempted_decode) {
      attempted_decode = true;
      decoded = source.decode(route.source, route.type, route.ser, raw);
    }
    return decoded;
  };
  std::vector<FoxgloveMessage> results;
  results.reserve(route.outputs.size());
  for (size_t i = 0; i < route.outputs.size(); ++i) {
    const auto& output = route.outputs[i];
    const auto* mapping = output.mapping;
    FoxgloveMessage result;
    result.output = i;
    result.is_send_time = output.schema.is_send_time;
    result.schema_name = output.schema.schema_name;
    result.encoding = output.schema.encoding;
    result.schema_encoding = output.schema.schema_encoding;
    flatbuffers::FlatBufferBuilder builder;
    bool written = false;
    const auto native = mapping ? native_ser(mapping->converter) : std::string{};
    if (mapping && (mapping->converter == "send_time" || mapping->converter == "passthrough")) {
      result.success = true;
      result.payload = Bytes::shallow_copy(raw.data(), raw.size());
      if (!mapping->timestamp.empty() && decode()) {
        result.timestamp_ns = FieldReader(source.view(), mapping).timestamp();
      }
    } else if (mapping && mapping->converter.empty()) {
      const auto& encoding = mapping->encoding;
      const bool matches = (encoding == "protobuf" && route.type == SchemaType::kProtobuf) ||
                           (encoding == "flatbuffer" && route.type == SchemaType::kFlatbuffers) ||
                           (encoding == "zerocopy" && route.type == SchemaType::kZeroCopy) ||
                           (encoding == "json" && route.ser == "json");
      if (matches && decode()) {
        const FieldReader fields(source.view(), mapping);
        result.timestamp_ns = fields.timestamp();
        written = write_foxglove_mapping(result.schema_name, fields, builder);
      }
      result.success = written;
    } else if (!native.empty() || (route.type == SchemaType::kZeroCopy && !native_schema(route.ser).empty())) {
      int64_t native_timestamp = -1;
      written = write_foxglove_native(native.empty() ? route.ser : native, raw, builder, result.schema_name,
                                      native_timestamp);
      result.success = written;
      if (mapping && !mapping->timestamp.empty() && decode()) {
        result.timestamp_ns = FieldReader(source.view(), mapping).timestamp();
      }
    } else if (output.plugin) {
      ConvertPluginInterface::SchemaInfo schema;
      result.success = plugin_->get_schema(route.ser, ConvertPluginInterface::Target::kFoxglove, schema);
      if (result.success && schema.type_name != "SendTime") {
        result.success = plugin_->convert(route.ser, raw, ConvertPluginInterface::Target::kFoxglove, result.payload) &&
                         plugin_->get_schema(route.ser, ConvertPluginInterface::Target::kFoxglove, schema);
      }
      if (result.success) {
        result.timestamp_ns = plugin_->get_timestamp(route.ser, raw, ConvertPluginInterface::Target::kFoxglove);
        result.schema_name = std::move(schema.type_name);
        result.encoding = wire_encoding(schema.encoding);
        result.schema_encoding = wire_encoding(schema.schema_encoding);
        result.schema_data = std::move(schema.schema_data);
        result.is_send_time = result.schema_name == "SendTime";
        if (result.is_send_time) {
          result.encoding = "send_time";
        }
      }
    } else if (route.type == SchemaType::kRaw && is_text_ser(route.ser)) {
      const ::foxglove::Time time(0, 0);
      builder.Finish(
          ::foxglove::CreateLog(builder, &time, ::foxglove::LogLevel::INFO,
                                builder.CreateString(reinterpret_cast<const char*>(raw.data()), raw.size())));
      result.success = written = true;
    } else {
      result.success = true;
      result.payload = Bytes::shallow_copy(raw.data(), raw.size());
    }
    if (written) {
      result.buffer = builder.Release();
      result.payload = Bytes::shallow_copy(result.buffer.data(), result.buffer.size());
    }
    results.push_back(std::move(result));
  }
  return results;
}

bool FoxgloveConverter::resolve_schema_by_name(const std::string& name, const std::string& encoding,
                                               std::string& data) {
  const auto wire = wire_encoding(encoding);
  if (wire == "flatbuffer") {
    const auto embedded = foxglove_schema(name);
    if (!embedded.empty()) {
      data.assign(embedded);
      return true;
    }
  }
  auto type = SchemaType::kUnknown;
  if (wire == "protobuf") {
    type = SchemaType::kProtobuf;
  } else if (wire == "flatbuffer") {
    type = SchemaType::kFlatbuffers;
  }
  const auto* schema = registry_.find(name, type);
  if (!schema) {
    return false;
  }
  data = schema->data;
  return true;
}

bool FoxgloveConverter::has_send_time_mapping() const { return mappings_.has_converter("send_time"); }

}  // namespace webviz
}  // namespace vlink
