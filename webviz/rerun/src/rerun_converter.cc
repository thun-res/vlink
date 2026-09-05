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

#include "./rerun_converter.h"

#include <vlink/base/logger.h>

#include "../../webviz_loader_utils.h"
#include "./rerun_writer.h"

namespace vlink {
namespace webviz {

RerunConverter::RerunConverter(const Config& config)
    : registry_(config),
      mappings_(config.vlink_msgs, "archetype"),
      timeline_(config.use_timestamp_timeline ? config.timestamp_timeline : "") {
  mappings_.validate(validate_rerun_mapping);
  load_convert_plugin(config.convert_plugin_path, config.convert_plugin_config, plugin_loader_, plugin_);
}

RerunRoute RerunConverter::resolve(std::string_view url, SchemaType type, const std::string& ser) {
  RerunRoute route;
  route.ser = ser;
  route.type = type;
  route.schema = registry_.find(ser, type);
  bool ambiguous = false;
  route.mappings = mappings_.select(url, ser, &ambiguous);
  route.valid = !ambiguous && mappings_.valid();
  if (!route.valid) {
    return route;
  }
  route.plugin = plugin_ && plugin_->can_convert(ser, ConvertPluginInterface::Target::kRerun);
  return route;
}

bool RerunConverter::convert_and_log(::rerun::RecordingStream& rec, const std::string& path, const RerunRoute& route,
                                     const Bytes& raw, int64_t fallback_timestamp_ns) {
  if (!route.valid) {
    return false;
  }
  const auto set_timestamp = [&](int64_t timestamp) {
    if (timestamp < 0) {
      timestamp = fallback_timestamp_ns;
    }
    if (timestamp >= 0 && !timeline_.empty()) {
      rec.set_time_timestamp_nanos_since_epoch(timeline_, timestamp);
    }
  };
  const auto native_type = zerocopy::MessageParser::detect_type(route.ser);
  if (route.mappings.empty() && route.type == SchemaType::kZeroCopy &&
      native_type != zerocopy::MessageParser::kUnknown && native_type != zerocopy::MessageParser::kProxyData) {
    set_timestamp(-1);
    return write_rerun_native(rec, path, route.ser, raw, timeline_);
  }
  if (!route.mappings.empty()) {
    DecodedMessage source;
    bool needs_fields = false;
    size_t outputs = 0;
    for (const auto* mapping : route.mappings) {
      needs_fields |= mapping->converter.empty() || mapping->converter == "send_time" || !mapping->timestamp.empty();
      outputs += mapping->converter != "send_time" ? 1U : 0U;
    }
    const bool decoded = !needs_fields || source.decode(route.schema, route.type, route.ser, raw);
    bool success = decoded;
    for (const auto* mapping : route.mappings) {
      if (mapping->converter == "send_time") {
        const auto timestamp = FieldReader(source.view(), mapping).timestamp();
        if (timestamp >= 0) {
          rec.set_time_duration_nanos("vlink_time", timestamp);
        } else {
          success = false;
        }
      }
    }
    for (const auto* mapping : route.mappings) {
      if (mapping->converter == "send_time") {
        continue;
      }
      auto output_path = path;
      if (!mapping->entity_path.empty()) {
        output_path = mapping->entity_path;
      } else if (outputs > 1) {
        output_path += "/" + (mapping->target.empty() ? mapping->converter : mapping->target);
      }
      const auto timestamp = FieldReader(source.view(), mapping).timestamp();
      set_timestamp(timestamp);
      const auto native = native_ser(mapping->converter);
      bool written = false;
      if (!native.empty()) {
        written = write_rerun_native(rec, output_path, native, raw, timestamp < 0 ? timeline_ : "");
      } else if (mapping->converter.empty()) {
        const bool matches = (mapping->encoding == "protobuf" && route.type == SchemaType::kProtobuf) ||
                             (mapping->encoding == "flatbuffer" && route.type == SchemaType::kFlatbuffers) ||
                             (mapping->encoding == "zerocopy" && route.type == SchemaType::kZeroCopy) ||
                             (mapping->encoding == "json" && route.ser == "json");
        written =
            decoded && matches &&
            write_rerun(rec, output_path, mapping->target, FieldReader(source.view(), mapping), mapping->is_static);
      }
      success = written && success;
    }
    return success;
  }
  if (route.plugin) {
    ConvertPluginInterface::SchemaInfo schema;
    if (!plugin_->get_schema(route.ser, ConvertPluginInterface::Target::kRerun, schema)) {
      return false;
    }
    if (schema.type_name == "SendTime") {
      return true;
    }
    Bytes payload;
    if (!plugin_->convert(route.ser, raw, ConvertPluginInterface::Target::kRerun, payload)) {
      return false;
    }
    if (!plugin_->get_schema(route.ser, ConvertPluginInterface::Target::kRerun, schema) || schema.encoding != "json") {
      return false;
    }
    const auto json = nlohmann::json::parse(payload.data(), payload.data() + payload.size(), nullptr, false);
    if (!json.is_object()) {
      return false;
    }
    set_timestamp(plugin_->get_timestamp(route.ser, raw, ConvertPluginInterface::Target::kRerun));
    return write_rerun(rec, path, schema.type_name, FieldReader(MessageView(json), nullptr));
  }
  set_timestamp(-1);
  if (is_text_ser(route.ser)) {
    return rec.try_log(path, ::rerun::TextLog(std::string(reinterpret_cast<const char*>(raw.data()), raw.size())))
        .is_ok();
  }
  if (route.type == SchemaType::kProtobuf && route.schema) {
    DecodedMessage source;
    if (!source.decode(route.schema, route.type, route.ser, raw)) {
      return false;
    }
    return rec.try_log(path, ::rerun::TextLog(source.text())).is_ok();
  }
  return rec.try_log(path, ::rerun::TextLog("[" + route.ser + "] raw " + std::to_string(raw.size()) + " bytes"))
      .is_ok();
}

}  // namespace webviz
}  // namespace vlink
