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

#include "./vlink_convert.h"

#include <vlink/base/logger.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "../../webviz_loader_utils.h"

namespace vlink {
namespace webviz {

using Json = nlohmann::json;

static std::string primary_url(const UrlSelector& selector) {
  return is_static_url_selector(selector) && !selector.whitelist_exact.empty() ? selector.whitelist_exact.front() : "";
}

VlinkConvert::VlinkConvert(const Config& config) : registry_(config) {
  load_convert_plugin(config.convert_plugin_path, config.convert_plugin_config, plugin_loader_, plugin_);
  for (const auto& path : config.foxglove_msgs) {
    std::vector<CommandMapping> loaded;
    const auto parse = [&](const Json& entry) {
      try {
        for (const auto* key : {"topic", "vlink_encoding", "payload_encoding", "schema_base64", "converter",
                                "schema_type", "field_mappings"}) {
          if (entry.contains(key)) {
            MLOG_E("Unsupported foxglove_msgs key {} in {}", key, path);
            return false;
          }
        }
        CommandMapping mapping;
        mapping.ser = entry.at("ser").get<std::string>();
        mapping.payload_encoding = entry.value("encoding", "");
        mapping.schema_name = entry.value("schema_name", "");
        mapping.schema_encoding = entry.value("schema_encoding", "");
        if (!parse_url_selector(entry, path, "foxglove_msgs", mapping.url_selector)) {
          return false;
        }
        mapping.topic = primary_url(mapping.url_selector);
        if (entry.contains("schema")) {
          mapping.schema = entry["schema"].is_string() ? entry["schema"].get<std::string>() : entry["schema"].dump();
        } else if (entry.contains("schema_path")) {
          auto schema_path = std::filesystem::path(entry.at("schema_path").get<std::string>());
          if (schema_path.is_relative()) {
            schema_path = std::filesystem::path(path).parent_path() / schema_path;
          }
          std::ifstream stream(schema_path);
          if (!stream) {
            return false;
          }
          mapping.schema.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
          if (mapping.schema.empty()) {
            return false;
          }
        }
        if (!resolve_input_schema(mapping)) {
          return false;
        }
        loaded.push_back(std::move(mapping));
        return true;
      } catch (const Json::exception& error) {
        MLOG_E("Invalid foxglove_msgs {}: {}", path, error.what());
        return false;
      }
    };
    if (load_json_entries(path, "Cannot read foxglove_msgs", "Invalid foxglove_msgs", parse)) {
      for (auto& mapping : loaded) {
        mappings_.push_back(std::move(mapping));
      }
    }
  }
}

bool VlinkConvert::resolve_input_schema(CommandMapping& mapping) {
  mapping.encoding = "json";
  if (mapping.ser.empty()) {
    return false;
  }
  if (mapping.payload_encoding.empty()) {
    if (mapping.schema_type == SchemaType::kFlatbuffers) {
      mapping.payload_encoding = "flatbuffers";
    } else if (is_json_ser(mapping.ser)) {
      mapping.payload_encoding = "json";
    } else if (is_text_ser(mapping.ser)) {
      mapping.payload_encoding = "text";
    } else {
      mapping.payload_encoding = "protobuf";
    }
  }
  if (is_flatbuffers_encoding(mapping.payload_encoding)) {
    mapping.payload_encoding = "flatbuffers";
  }
  const auto type = SchemaData::convert_encoding(mapping.payload_encoding);
  if (type == SchemaType::kUnknown || type == SchemaType::kZeroCopy ||
      (mapping.schema_type != SchemaType::kUnknown && mapping.schema_type != type) ||
      !is_target_encoding_compatible(mapping.ser, mapping.payload_encoding)) {
    return false;
  }
  mapping.schema_type = type;
  if (!mapping.schema_encoding.empty() && mapping.schema_encoding != "jsonschema" &&
      mapping.schema_encoding != "json") {
    return false;
  }
  if (mapping.schema.empty()) {
    if (type == SchemaType::kProtobuf || type == SchemaType::kFlatbuffers) {
      const auto* source = registry_.find(mapping.ser, type);
      if (!source) {
        return false;
      }
      mapping.schema = source_json_schema(*source);
    } else {
      mapping.schema = Json{{"type", mapping.payload_encoding == "text" ? "string" : "object"}}.dump();
    }
  }
  const auto schema = Json::parse(mapping.schema, nullptr, false);
  if (!schema.is_object()) {
    return false;
  }
  mapping.schema = schema.dump();
  if (mapping.schema_name.empty()) {
    if (!is_json_ser(mapping.ser) && !is_text_ser(mapping.ser)) {
      mapping.schema_name = mapping.ser;
    } else {
      mapping.schema_name = "webviz.publish.";
      bool separator = true;
      const auto url = mapping.topic.empty() ? primary_url(mapping.url_selector) : mapping.topic;
      for (const unsigned char c : url) {
        if (std::isalnum(c) || c == '_') {
          mapping.schema_name.push_back(static_cast<char>(std::tolower(c)));
          separator = false;
        } else if (!separator) {
          mapping.schema_name.push_back('.');
          separator = true;
        }
      }
      while (!mapping.schema_name.empty() && mapping.schema_name.back() == '.') {
        mapping.schema_name.pop_back();
      }
      if (mapping.schema_name == "webviz.publish") {
        mapping.schema_name += ".channel";
      }
    }
  }
  if (mapping.schema_encoding.empty()) {
    mapping.schema_encoding = "jsonschema";
  }
  return true;
}

bool VlinkConvert::build_route(const CommandMapping& mapping, const CommandChannel& channel, CommandRoute& route) {
  route = CommandRoute{};
  route.url = channel.topic.empty() ? primary_url(mapping.url_selector) : channel.topic;
  route.ser = mapping.ser;
  route.schema_type = mapping.schema_type;
  route.payload_encoding = mapping.payload_encoding;
  route.mapping = &mapping;
  route.web_channel.topic = route.url;
  route.web_channel.encoding = mapping.encoding;
  route.web_channel.schema_name = mapping.schema_name;
  route.web_channel.schema_encoding = mapping.schema_encoding;
  route.web_channel.schema = mapping.schema;
  return !route.url.empty() && !route.ser.empty();
}

bool VlinkConvert::resolve_route(const CommandChannel& channel, CommandRoute& route) {
  const CommandMapping* selected = nullptr;
  int best = -1;
  bool ambiguous = false;
  for (const auto& mapping : mappings_) {
    const int score = score_url_selector(channel.topic, mapping.url_selector);
    if (score < 0 || (!channel.encoding.empty() && channel.encoding != mapping.encoding) ||
        (!channel.schema_name.empty() && channel.schema_name != mapping.schema_name) ||
        (!channel.schema_encoding.empty() && channel.schema_encoding != mapping.schema_encoding)) {
      continue;
    }
    if (score > best) {
      best = score;
      selected = &mapping;
      ambiguous = false;
    } else if (score == best) {
      ambiguous = true;
    }
  }
  if (ambiguous) {
    MLOG_W("Ambiguous Foxglove publish route: {}", channel.topic);
    return false;
  }
  if (selected) {
    return build_route(*selected, channel, route);
  }
  route = CommandRoute{};
  route.web_channel.topic = channel.topic;
  route.web_channel.encoding = channel.encoding;
  route.web_channel.schema_name = channel.schema_name;
  route.web_channel.schema_encoding = channel.schema_encoding;
  route.web_channel.schema = channel.schema;
  if (!plugin_ || !plugin_->can_publish(route.web_channel, ConvertPluginInterface::Target::kFoxglove)) {
    return false;
  }
  ConvertPluginInterface::PublishInfo target;
  if (!plugin_->get_publish(route.web_channel, ConvertPluginInterface::Target::kFoxglove, target) ||
      target.url.empty() || target.ser_type.empty() || !SchemaData::is_valid_type(target.schema_type)) {
    return false;
  }
  route.url = std::move(target.url);
  route.ser = std::move(target.ser_type);
  route.schema_type = target.schema_type;
  route.payload_encoding = SchemaData::convert_type(target.schema_type);
  route.via_plugin = true;
  return true;
}

std::vector<CommandChannel> VlinkConvert::get_publish_channels() const {
  std::unordered_map<std::string, CommandChannel> channels;
  std::unordered_set<std::string> ambiguous;
  for (const auto& mapping : mappings_) {
    if (!is_static_url_selector(mapping.url_selector)) {
      continue;
    }
    for (const auto& url : mapping.url_selector.whitelist_exact) {
      if (ambiguous.count(url)) {
        continue;
      }
      const auto found = channels.find(url);
      if (found != channels.end()) {
        const auto& channel = found->second;
        if (channel.encoding != mapping.encoding || channel.schema_name != mapping.schema_name ||
            channel.schema_encoding != mapping.schema_encoding || channel.schema != mapping.schema) {
          channels.erase(found);
          ambiguous.insert(url);
        }
      } else {
        channels.emplace(
            url, CommandChannel{url, mapping.encoding, mapping.schema_name, mapping.schema_encoding, mapping.schema});
      }
    }
  }
  std::vector<CommandChannel> result;
  result.reserve(channels.size());
  for (auto& entry : channels) {
    result.push_back(std::move(entry.second));
  }
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) { return left.topic < right.topic; });
  return result;
}

CommandMessage VlinkConvert::encode_frontend_message(const CommandRoute& route, const Bytes& raw) {
  CommandMessage result;
  result.url = route.url;
  result.ser = route.ser;
  if (route.via_plugin) {
    result.success = plugin_ && plugin_->convert_publish(route.web_channel, raw,
                                                         ConvertPluginInterface::Target::kFoxglove, result.payload);
  } else if (!route.mapping) {
    return result;
  } else if (route.payload_encoding == "json") {
    result.payload = Bytes::shallow_copy(raw.data(), raw.size());
    result.success = true;
  } else if (route.payload_encoding == "text") {
    const auto json = Json::parse(raw.data(), raw.data() + raw.size(), nullptr, false);
    if (json.is_discarded()) {
      return result;
    }
    const auto text = json.is_string() ? json.get<std::string>() : json.dump();
    result.payload = Bytes::create(text.size());
    std::memcpy(result.payload.data(), text.data(), text.size());
    result.success = true;
  } else {
    result.success = registry_.encode_json(route.ser, route.schema_type, raw, result.payload);
  }
  return result;
}

bool VlinkConvert::decode_backend_message_to_json(const std::string& ser, SchemaType type, const Bytes& raw,
                                                  Bytes& output) {
  if (type == SchemaType::kRaw && is_json_ser(ser)) {
    output = Bytes::shallow_copy(raw.data(), raw.size());
    return true;
  }
  if (type == SchemaType::kRaw && is_text_ser(ser)) {
    const auto text = Json(std::string(reinterpret_cast<const char*>(raw.data()), raw.size())).dump();
    output = Bytes::create(text.size());
    std::memcpy(output.data(), text.data(), text.size());
    return true;
  }
  return registry_.decode_json(ser, type, raw, output);
}

}  // namespace webviz
}  // namespace vlink
