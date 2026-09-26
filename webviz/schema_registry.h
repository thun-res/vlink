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

#include <flatbuffers/idl.h>
#include <google/protobuf/dynamic_message.h>
#include <vlink/extension/schema_plugin_interface.h>

#if __has_include(<google/protobuf/compiler/importer.h>)
#include <google/protobuf/compiler/importer.h>
#define VLINK_HAS_PROTO_COMPILER
#endif

#include <memory>
#include <mutex>
#include <unordered_map>

#include "./message_view.h"

namespace vlink {
namespace webviz {

struct ConversionConfig {
  std::string proto_dir;
  std::string fbs_dir;
  std::string schema_plugin_path;
  std::string convert_plugin_path;
  std::string convert_plugin_config;
  std::vector<std::string> vlink_msgs;
};

struct SourceSchema final {
  std::string name;
  std::string data;
  const google::protobuf::Message* prototype{nullptr};
  const reflection::Schema* flatbuffer{nullptr};
};

[[nodiscard]] std::string source_json_schema(const SourceSchema& source);

class SchemaRegistry final {
 public:
  explicit SchemaRegistry(const ConversionConfig& config);
  [[nodiscard]] const SourceSchema* find(const std::string& name, SchemaType type);
  [[nodiscard]] bool decode_json(const std::string& name, SchemaType type, const Bytes& raw, Bytes& output);
  [[nodiscard]] bool encode_json(const std::string& name, SchemaType type, const Bytes& json, Bytes& output);

 private:
  void add_proto(const google::protobuf::Descriptor& fallback);
  void load_flatbuffers(const std::string& directory);

  std::shared_ptr<SchemaPluginInterface> plugin_;
#ifdef VLINK_HAS_PROTO_COMPILER
  google::protobuf::compiler::DiskSourceTree source_tree_;
  std::unique_ptr<google::protobuf::compiler::Importer> importer_;
#endif
  google::protobuf::DynamicMessageFactory factory_;
  std::unordered_map<std::string, std::unique_ptr<SourceSchema>> proto_schemas_;
  std::unordered_map<std::string, std::unique_ptr<SourceSchema>> fbs_schemas_;
  std::mutex mutex_;
};

class DecodedMessage final {
 public:
  DecodedMessage() = default;
  DecodedMessage(const DecodedMessage&) = delete;
  DecodedMessage& operator=(const DecodedMessage&) = delete;
  DecodedMessage(DecodedMessage&&) = delete;
  DecodedMessage& operator=(DecodedMessage&&) = delete;

  [[nodiscard]] bool decode(const SourceSchema* schema, SchemaType type, const std::string& ser, const Bytes& raw);
  [[nodiscard]] MessageView view() const;
  [[nodiscard]] std::string text() const;

 private:
  std::unique_ptr<google::protobuf::Message> proto_;
  nlohmann::json json_;
  zerocopy::MessageParser zero_;
  MessageView view_;
};

}  // namespace webviz
}  // namespace vlink
