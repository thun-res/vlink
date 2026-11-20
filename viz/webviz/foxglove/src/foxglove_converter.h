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

#include <flatbuffers/detached_buffer.h>
#include <vlink/base/plugin.h>
#include <vlink/extension/convert_plugin_interface.h>

#include "../../mapping.h"
#include "../../schema_registry.h"

namespace vlink {
namespace webviz {

struct FoxgloveSchema {
  bool is_send_time{false};
  std::string schema_name;
  std::string encoding;
  std::string schema_encoding;
  std::string schema_data;
};

struct FoxgloveOutput final {
  FoxgloveSchema schema;
  const MessageMapping* mapping{nullptr};
  bool plugin{false};
  bool schema_from_payload{false};
};

struct FoxgloveRoute final {
  bool valid{true};
  SchemaType type{SchemaType::kUnknown};
  std::string ser;
  const SourceSchema* source{nullptr};
  std::vector<FoxgloveOutput> outputs;
};

struct FoxgloveMessage final : FoxgloveSchema {
  bool success{false};
  size_t output{0};
  flatbuffers::DetachedBuffer buffer;
  Bytes payload;
  int64_t timestamp_ns{-1};
};

class FoxgloveConverter final {
 public:
  using Config = ConversionConfig;

  explicit FoxgloveConverter(const Config& config);
  [[nodiscard]] bool valid() const { return mappings_.valid(); }
  [[nodiscard]] FoxgloveRoute resolve(std::string_view url, SchemaType type, const std::string& ser);
  [[nodiscard]] std::vector<FoxgloveMessage> convert(const FoxgloveRoute& route, const Bytes& raw);
  [[nodiscard]] bool resolve_schema_by_name(const std::string& name, const std::string& encoding, std::string& data);
  [[nodiscard]] bool has_send_time_mapping() const;

 private:
  [[nodiscard]] bool describe(SchemaType type, const std::string& ser, FoxgloveOutput& output);
  SchemaRegistry registry_;
  MappingSet mappings_;
  Plugin plugin_loader_;
  std::shared_ptr<ConvertPluginInterface> plugin_;
};

}  // namespace webviz
}  // namespace vlink
