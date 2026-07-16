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

#if __has_include(<flatbuffers/idl.h>)
#include <flatbuffers/base.h>
#include <flatbuffers/idl.h>
#include <flatbuffers/reflection.h>
#include <flatbuffers/reflection_generated.h>

#if FLATBUFFERS_VERSION_MAJOR >= 22
#define VLINK_HAS_FBS_COMPILER
#endif
#endif

#if __has_include(<google/protobuf/compiler/importer.h>)
#include <google/protobuf/compiler/importer.h>
#define VLINK_HAS_PROTO_COMPILER
#endif

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>
#include <vlink/base/bytes.h>
#include <vlink/base/logger.h>
#include <vlink/base/macros.h>
#include <vlink/base/plugin.h>
#include <vlink/extension/convert_plugin_interface.h>
#include <vlink/extension/schema_plugin_interface.h>
#include <vlink/extension/schema_plugin_manager.h>

#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../webviz_types.h"

namespace vlink {
namespace webviz {

using Json = nlohmann::json;

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
  struct Config final {
    std::string proto_dir;
    std::string fbs_dir;
    std::string schema_plugin_path;
    std::string convert_plugin_path;
    std::string convert_plugin_config;
    std::vector<std::string> foxglove_msgs;
  };

  explicit VlinkConvert(const Config& config);

  ~VlinkConvert();

  bool resolve_input_schema(CommandMapping& mapping);

  static bool build_route(const CommandMapping& mapping, const CommandChannel& channel, CommandRoute& route);

  bool resolve_route(const CommandChannel& channel, CommandRoute& route);

  std::vector<CommandChannel> get_publish_channels() const;

  CommandMessage encode_frontend_message(const CommandRoute& route, const Bytes& raw);

  bool decode_backend_message_to_json(const std::string& ser, SchemaType schema_type, const Bytes& raw, Bytes& payload);

 private:
  bool init_proto_resolver();

  bool init_convert_plugin();

  void load_mappings();

  bool load_mapping_file(const std::string& path);

  bool finalize_mapping(CommandMapping& mapping, std::string_view path);

  static bool ensure_protobuf_json_util(std::string_view action, std::string_view ser);

  static Json make_json_schema_leaf(std::string_view type);

  static Json make_default_json_object_schema();

  static bool is_json_schema_encoding(std::string_view schema_encoding);

  static std::string get_primary_static_url(const UrlSelector& selector);

  static Json make_proto_json_schema(const google::protobuf::Descriptor* desc,
                                     std::unordered_set<const google::protobuf::Descriptor*>& stack, int depth);

  static Json make_proto_field_json_schema(const google::protobuf::FieldDescriptor* field,
                                           std::unordered_set<const google::protobuf::Descriptor*>& stack, int depth);

  static bool build_proto_json_schema(const google::protobuf::Descriptor* desc, std::string& schema_data);

  static bool build_fbs_json_schema(const reflection::Schema* schema, std::string& schema_data);

  static bool serialize_proto_to_bytes(const google::protobuf::Message& message, Bytes& payload);

  const google::protobuf::Descriptor* find_proto_descriptor(const std::string& proto_name);

  bool resolve_proto_schema(const std::string& proto_name, std::string& schema_data);

  std::unique_ptr<google::protobuf::Message> deserialize_proto_message(const std::string& proto_name, const Bytes& raw);

  std::unique_ptr<google::protobuf::Message> create_proto_message(const std::string& proto_name);

  static int get_mapping_match_score(const CommandMapping& mapping, const CommandChannel& channel);

  const CommandMapping* find_mapping(const CommandChannel& channel, bool* ambiguous = nullptr) const;

  CommandMessage encode_direct_route(const CommandRoute& route, const Bytes& raw);

  CommandMessage encode_json_payload(const CommandRoute& route, const Bytes& raw);

#ifdef VLINK_HAS_FBS_COMPILER
  bool init_fbs_resolver();

  bool find_fbs_parser_locked(const std::string& fbs_ser);

  const reflection::Schema* resolve_fbs_schema(const std::string& fbs_ser, std::string& bfbs_storage);

  static Json make_fbs_json_schema(const reflection::Schema& schema, const reflection::Type* type,
                                   std::unordered_set<std::string>& stack, int depth);

  static Json make_fbs_object_json_schema(const reflection::Schema& schema, const reflection::Object* obj,
                                          std::unordered_set<std::string>& stack, int depth);

  static Json make_fbs_scalar_json_schema(const reflection::Schema& schema, reflection::BaseType type, int32_t index,
                                          std::unordered_set<std::string>& stack, int depth);

  static bool is_fbs_integer_type(reflection::BaseType type);
#endif

  std::vector<CommandMapping> mappings_;
  std::mutex mtx_;
  Config config_;
  const uint64_t cache_owner_id_{allocate_cache_owner_id()};

  std::shared_ptr<SchemaPluginInterface> schema_interface_;
  Plugin convert_plugin_loader_;
  std::shared_ptr<ConvertPluginInterface> convert_plugin_;
  google::protobuf::DynamicMessageFactory proto_factory_;

#ifdef VLINK_HAS_PROTO_COMPILER
  std::shared_ptr<google::protobuf::compiler::DiskSourceTree> source_tree_;
  std::shared_ptr<google::protobuf::compiler::Importer> importer_;
  std::shared_ptr<google::protobuf::DynamicMessageFactory> disk_factory_;
  std::unordered_map<std::string, const google::protobuf::Descriptor*> imported_proto_descriptors_;
#endif

#ifdef VLINK_HAS_FBS_COMPILER
  std::vector<std::unique_ptr<flatbuffers::Parser>> fbs_parser_vec_;
  std::unordered_map<std::string, size_t> fbs_parsers_;
  std::unordered_map<std::string, std::string> fbs_schema_cache_;
  std::unordered_set<std::string> fbs_not_found_;
#endif

  VLINK_DISALLOW_COPY_AND_ASSIGN(VlinkConvert)
};

}  // namespace webviz
}  // namespace vlink
