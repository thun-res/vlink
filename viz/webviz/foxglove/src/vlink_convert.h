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

#include <vlink/base/plugin.h>
#include <vlink/extension/convert_plugin_interface.h>

#include "../../schema_registry.h"
#include "../../webviz_types.h"

namespace vlink {
namespace webviz {

struct CommandMapping final {
  std::string topic;
  std::string encoding{"json"};
  std::string schema_name;
  std::string schema_encoding;
  std::string schema;
  UrlSelector url_selector;
  std::string ser;
  std::string payload_encoding;
  SchemaType schema_type{SchemaType::kUnknown};
};

struct CommandChannel final {
  std::string topic;
  std::string encoding;
  std::string schema_name;
  std::string schema_encoding;
  std::string schema;
};

struct CommandRoute final {
  std::string url;
  std::string ser;
  std::string payload_encoding;
  SchemaType schema_type{SchemaType::kUnknown};
  bool via_plugin{false};
  const CommandMapping* mapping{nullptr};
  ConvertPluginInterface::FrontendChannel web_channel;
};

struct CommandMessage final {
  bool success{false};
  std::string url;
  std::string ser;
  Bytes payload;
};

class VlinkConvert final {
 public:
  struct Config final : ConversionConfig {
    std::vector<std::string> foxglove_msgs;
  };

  explicit VlinkConvert(const Config& config);
  [[nodiscard]] bool resolve_input_schema(CommandMapping& mapping);
  [[nodiscard]] static bool build_route(const CommandMapping& mapping, const CommandChannel& channel,
                                        CommandRoute& route);
  [[nodiscard]] bool resolve_route(const CommandChannel& channel, CommandRoute& route);
  [[nodiscard]] std::vector<CommandChannel> get_publish_channels() const;
  [[nodiscard]] CommandMessage encode_frontend_message(const CommandRoute& route, const Bytes& raw);
  [[nodiscard]] bool decode_backend_message_to_json(const std::string& ser, SchemaType type, const Bytes& raw,
                                                    Bytes& output);

 private:
  SchemaRegistry registry_;
  std::vector<CommandMapping> mappings_;
  Plugin plugin_loader_;
  std::shared_ptr<ConvertPluginInterface> plugin_;
};

}  // namespace webviz
}  // namespace vlink
