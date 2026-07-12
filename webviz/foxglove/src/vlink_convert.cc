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

#if defined(__has_include)
#if __has_include(<google/protobuf/util/json_util.h>)
#include <google/protobuf/util/json_util.h>
#define VLINK_HAS_PROTOBUF_JSON_UTIL
#endif
#endif
#include <vlink/base/functional.h>
#include <vlink/base/helpers.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <utility>

#include "../../webviz_common.h"
#include "../../webviz_loader_utils.h"

#ifdef _WIN32
#undef min
#undef max
#undef GetMessage
#undef ERROR
#endif

namespace vlink {
namespace webviz {

bool VlinkConvert::ensure_protobuf_json_util(std::string_view action, std::string_view ser) {
#ifdef VLINK_HAS_PROTOBUF_JSON_UTIL
  return true;
#endif

  MLOG_W("Protobuf JSON support is unavailable for {} [{}]: protobuf json_util.h is not available", action, ser);
  return false;
}

Json VlinkConvert::make_json_schema_leaf(std::string_view type) {
  Json leaf = Json::object();
  leaf["type"] = type;
  return leaf;
}

Json VlinkConvert::make_default_json_object_schema() {
  Json schema = Json::object();
  schema["type"] = "object";
  schema["properties"] = Json::object();
  return schema;
}

bool VlinkConvert::is_json_schema_encoding(std::string_view schema_encoding) {
  return schema_encoding.empty() || schema_encoding == "jsonschema" || schema_encoding == "json";
}

std::string VlinkConvert::get_primary_static_url(const UrlSelector& selector) {
  if VLIKELY (is_static_url_selector(selector) && !selector.whitelist_exact.empty()) {
    return selector.whitelist_exact.front();
  }

  return {};
}

Json VlinkConvert::make_proto_field_json_schema(const google::protobuf::FieldDescriptor* field,
                                                std::unordered_set<const google::protobuf::Descriptor*>& stack,
                                                int depth) {
  if VUNLIKELY (!field) {
    return Json::object();
  }

  if VUNLIKELY (field->is_map()) {
    Json schema = Json::object();
    schema["type"] = "object";

    const auto* entry_desc = field->message_type();
    const auto* value_field = entry_desc ? entry_desc->FindFieldByName("value") : nullptr;
    schema["additionalProperties"] =
        value_field ? make_proto_field_json_schema(value_field, stack, depth + 1) : Json::object();
    return schema;
  }

  Json base_schema = Json::object();

  switch (field->cpp_type()) {
    case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
    case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
      base_schema = make_json_schema_leaf("number");
      break;
    case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
    case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
      base_schema = make_json_schema_leaf("integer");
      break;
    case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
      base_schema = make_json_schema_leaf("boolean");
      break;
    case google::protobuf::FieldDescriptor::CPPTYPE_STRING:
      base_schema = make_json_schema_leaf("string");

      if VUNLIKELY (field->type() == google::protobuf::FieldDescriptor::TYPE_BYTES) {
        base_schema["contentEncoding"] = "base64";
      }

      break;
    case google::protobuf::FieldDescriptor::CPPTYPE_ENUM: {
      base_schema = make_json_schema_leaf("string");
      Json values = Json::array();

      if (const auto* enum_desc = field->enum_type()) {
        for (int i = 0; i < enum_desc->value_count(); ++i) {
          if (const auto* value = enum_desc->value(i)) {
            values.emplace_back(value->name());
          }
        }
      }

      if VLIKELY (!values.empty()) {
        base_schema["enum"] = std::move(values);
      }

      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE:
      base_schema = make_proto_json_schema(field->message_type(), stack, depth + 1);
      break;
  }

  if VLIKELY (!field->is_repeated()) {
    return base_schema;
  }

  Json array_schema = Json::object();
  array_schema["type"] = "array";
  array_schema["items"] = std::move(base_schema);
  return array_schema;
}

Json VlinkConvert::make_proto_json_schema(const google::protobuf::Descriptor* desc,
                                          std::unordered_set<const google::protobuf::Descriptor*>& stack, int depth) {
  Json schema = make_default_json_object_schema();

  if VUNLIKELY (!desc || depth > 8 || stack.count(desc) > 0U) {
    return schema;
  }

  stack.insert(desc);
  Json required = Json::array();

  for (int i = 0; i < desc->field_count(); ++i) {
    const auto* field = desc->field(i);

    if VUNLIKELY (!field) {
      continue;
    }

    const auto& field_name = field->json_name();
    schema["properties"][field_name] = make_proto_field_json_schema(field, stack, depth + 1);

    if VUNLIKELY (field->is_required()) {
      required.emplace_back(field_name);
    }
  }

  stack.erase(desc);

  if VLIKELY (!required.empty()) {
    schema["required"] = std::move(required);
  }

  return schema;
}

bool VlinkConvert::build_proto_json_schema(const google::protobuf::Descriptor* desc, std::string& schema_data) {
  if VUNLIKELY (!desc) {
    return false;
  }

  std::unordered_set<const google::protobuf::Descriptor*> stack;
  schema_data = make_proto_json_schema(desc, stack, 0).dump();
  return true;
}

#ifdef VLINK_HAS_FBS_COMPILER
bool VlinkConvert::is_fbs_integer_type(reflection::BaseType type) {
  switch (type) {
    case reflection::Byte:
    case reflection::UByte:
    case reflection::Short:
    case reflection::UShort:
    case reflection::Int:
    case reflection::UInt:
    case reflection::Long:
    case reflection::ULong:
    case reflection::UType:
      return true;
    default:
      return false;
  }
}

Json VlinkConvert::make_fbs_object_json_schema(const reflection::Schema& schema, const reflection::Object* obj,
                                               std::unordered_set<std::string>& stack, int depth) {
  Json result = make_default_json_object_schema();

  if VUNLIKELY (!obj || !obj->name() || depth > 8) {
    return result;
  }

  const auto object_name = obj->name()->str();

  if VUNLIKELY (stack.count(object_name) > 0U) {
    return result;
  }

  stack.insert(object_name);
  Json required = Json::array();

  if VLIKELY (obj->fields()) {
    for (unsigned i = 0; i < obj->fields()->size(); ++i) {
      const auto* field = obj->fields()->Get(i);

      if VUNLIKELY (!field || !field->name()) {
        continue;
      }

      result["properties"][field->name()->str()] = make_fbs_json_schema(schema, field->type(), stack, depth + 1);

      if VUNLIKELY (field->required()) {
        required.emplace_back(field->name()->str());
      }
    }
  }

  stack.erase(object_name);

  if VLIKELY (!required.empty()) {
    result["required"] = std::move(required);
  }

  return result;
}

Json VlinkConvert::make_fbs_scalar_json_schema(const reflection::Schema& schema, reflection::BaseType type,
                                               int32_t index, std::unordered_set<std::string>& stack, int depth) {
  if VUNLIKELY (type == reflection::Bool) {
    return make_json_schema_leaf("boolean");
  }

  if VLIKELY (type == reflection::Float || type == reflection::Double) {
    return make_json_schema_leaf("number");
  }

  if VLIKELY (type == reflection::String) {
    return make_json_schema_leaf("string");
  }

  if VLIKELY (is_fbs_integer_type(type)) {
    const auto* enum_def = (schema.enums() && index >= 0 && index < static_cast<int32_t>(schema.enums()->size()))
                               ? schema.enums()->Get(static_cast<uint32_t>(index))
                               : nullptr;
    if VLIKELY (enum_def && !enum_def->is_union()) {
      Json enum_schema = make_json_schema_leaf("string");
      Json values = Json::array();

      if VLIKELY (enum_def->values()) {
        for (unsigned i = 0; i < enum_def->values()->size(); ++i) {
          if (const auto* value = enum_def->values()->Get(i); value && value->name()) {
            values.emplace_back(value->name()->str());
          }
        }
      }

      if VLIKELY (!values.empty()) {
        enum_schema["enum"] = std::move(values);
      }

      return enum_schema;
    }

    return make_json_schema_leaf("integer");
  }

  if VLIKELY (type == reflection::Obj) {
    if VUNLIKELY (!schema.objects() || index < 0 || index >= static_cast<int32_t>(schema.objects()->size())) {
      return make_default_json_object_schema();
    }

    return make_fbs_object_json_schema(schema, schema.objects()->Get(static_cast<uint32_t>(index)), stack, depth + 1);
  }

  return make_default_json_object_schema();
}

Json VlinkConvert::make_fbs_json_schema(const reflection::Schema& schema, const reflection::Type* type,
                                        std::unordered_set<std::string>& stack, int depth) {
  if VUNLIKELY (!type) {
    return Json::object();
  }

  auto base_type = type->base_type();

  if VLIKELY (base_type == reflection::Vector || base_type == reflection::Vector64 || base_type == reflection::Array) {
    Json result = Json::object();
    result["type"] = "array";
    result["items"] = make_fbs_scalar_json_schema(schema, type->element(), type->index(), stack, depth + 1);
    return result;
  }

  return make_fbs_scalar_json_schema(schema, base_type, type->index(), stack, depth + 1);
}

bool VlinkConvert::build_fbs_json_schema(const reflection::Schema* schema, std::string& schema_data) {
  if VUNLIKELY (!schema || !schema->root_table()) {
    return false;
  }

  std::unordered_set<std::string> stack;
  schema_data = make_fbs_object_json_schema(*schema, schema->root_table(), stack, 0).dump();
  return true;
}
#endif

bool VlinkConvert::serialize_proto_to_bytes(const google::protobuf::Message& message, Bytes& payload) {
  auto byte_size = message.ByteSizeLong();

  if VUNLIKELY (byte_size > std::numeric_limits<int>::max()) {
    return false;
  }

  if VUNLIKELY (byte_size == 0) {
    payload.clear();
    return true;
  }

  auto bytes = Bytes::create(byte_size);

  if VUNLIKELY (bytes.empty()) {
    return false;
  }

  if VUNLIKELY (!message.SerializeToArray(bytes.data(), static_cast<int>(byte_size))) {
    return false;
  }

  payload = std::move(bytes);
  return true;
}

VlinkConvert::VlinkConvert(const Config& config) : config_(config) {
  Bytes::init_memory_pool();
  init_proto_resolver();
  init_convert_plugin();

#ifdef VLINK_HAS_FBS_COMPILER
  init_fbs_resolver();
#endif

  load_mappings();
}

VlinkConvert::~VlinkConvert() = default;

bool VlinkConvert::init_proto_resolver() {
  bool has_resolver = false;

  auto& mgr = SchemaPluginManager::get(config_.schema_plugin_path);

  if VLIKELY (mgr.is_valid()) {
    schema_interface_ = mgr.get_interface();
    has_resolver = true;
  }

#ifdef VLINK_HAS_PROTO_COMPILER

  if VLIKELY (!config_.proto_dir.empty()) {
    auto proto_path = std::filesystem::path(config_.proto_dir);
    std::error_code ec;

    if VUNLIKELY (!std::filesystem::exists(proto_path, ec) || ec) {
      MLOG_W("Client proto directory does not exist: {}", config_.proto_dir);
    } else {
      source_tree_ = std::make_shared<google::protobuf::compiler::DiskSourceTree>();
      source_tree_->MapPath("", Helpers::path_to_string(proto_path));

      importer_ = std::make_shared<google::protobuf::compiler::Importer>(source_tree_.get(), nullptr);

      bool has_import = false;
      imported_proto_descriptors_.clear();
      import_protos(importer_.get(), proto_path, proto_path, has_import, 0, &imported_proto_descriptors_);

      if VLIKELY (has_import) {
        disk_factory_ = std::make_shared<google::protobuf::DynamicMessageFactory>();
        has_resolver = true;
      }
    }
  }
#endif

  return has_resolver;
}

bool VlinkConvert::init_convert_plugin() {
  return load_convert_plugin(config_.convert_plugin_path, config_.convert_plugin_config, convert_plugin_loader_,
                             convert_plugin_);
}

void VlinkConvert::load_mappings() {
  mappings_.clear();

  for (const auto& file : config_.foxglove_msgs) {
    if VUNLIKELY (!load_mapping_file(file)) {
      MLOG_W("Failed to load foxglove_msgs mapping: {}", file);
    }
  }
}

bool VlinkConvert::load_mapping_file(const std::string& path) {
  std::vector<CommandMapping> loaded_mappings;
  const auto base_dir = std::filesystem::path(path).parent_path();

  auto ok = load_json_entries(
      path, "foxglove_msgs mapping file not found", "Failed to parse foxglove_msgs mapping",
      [this, &loaded_mappings, &path, &base_dir](const Json& obj) -> bool {
        try {
          if VUNLIKELY (!obj.is_object()) {
            return false;
          }

          CommandMapping mapping;
          const bool has_explicit_schema = obj.contains("schema") || obj.contains("schema_path") ||
                                           obj.contains("schema_base64") || obj.contains("schema_name") ||
                                           obj.contains("schema_encoding");

          if VUNLIKELY (obj.contains("topic")) {
            MLOG_W("Invalid foxglove_msgs mapping in {}: topic is no longer supported, use url only", path);
            return false;
          }

          if VUNLIKELY (obj.contains("vlink_encoding") || obj.contains("payload_encoding")) {
            MLOG_W("Invalid foxglove_msgs mapping in {}: use encoding only, vlink_encoding/payload_encoding removed",
                   path);
            return false;
          }

          mapping.schema_name = obj.value("schema_name", std::string());
          mapping.schema_encoding = obj.value("schema_encoding", std::string());

          if VLIKELY (obj.contains("schema")) {
            if VLIKELY (obj["schema"].is_string()) {
              mapping.schema = obj["schema"].get<std::string>();
            } else {
              mapping.schema = obj["schema"].dump();
            }
          } else if VUNLIKELY (obj.contains("schema_base64")) {
            MLOG_W("Invalid foxglove_msgs mapping in {}: schema_base64 is not supported for frontend JSON schema",
                   path);
            return false;
          } else if VUNLIKELY (obj.contains("schema_path")) {
            auto schema_path = std::filesystem::path(obj["schema_path"].get<std::string>());

            if VUNLIKELY (!schema_path.is_absolute()) {
              schema_path = base_dir / schema_path;
            }

            std::ifstream ifs(schema_path);

            if VLIKELY (ifs.is_open()) {
              mapping.schema = std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            }

            if VUNLIKELY (mapping.schema.empty()) {
              MLOG_W("Invalid foxglove_msgs mapping in {}: failed to read schema_path {}", path,
                     Helpers::path_to_string(schema_path));
              return false;
            }
          }

          if VUNLIKELY (!parse_url_selector(obj, path, "foxglove_msgs mapping", mapping.url_selector)) {
            return false;
          }

          mapping.ser = obj.value("ser", std::string());

          if VUNLIKELY (obj.contains("converter")) {
            MLOG_W("Invalid foxglove_msgs mapping in {}: converter is no longer supported", path);
            return false;
          }

          if VUNLIKELY (obj.contains("schema_type")) {
            MLOG_W("Invalid foxglove_msgs mapping in {}: use encoding only, schema_type removed", path);
            return false;
          }

          mapping.payload_encoding = obj.value("encoding", std::string());

          if VUNLIKELY (mapping.payload_encoding.empty()) {
            if VLIKELY (is_json_ser(mapping.ser)) {
              mapping.payload_encoding = "json";
            } else if VLIKELY (is_text_ser(mapping.ser)) {
              mapping.payload_encoding = "text";
            } else {
              mapping.payload_encoding = "protobuf";
            }
          }

          mapping.encoding = "json";

          if VUNLIKELY (mapping.ser.empty()) {
            MLOG_W("Invalid foxglove_msgs mapping in {}: missing ser", path);
            return false;
          }

          mapping.topic = get_primary_static_url(mapping.url_selector);

          if VUNLIKELY (obj.contains("field_mappings")) {
            MLOG_W(
                "Invalid foxglove_msgs mapping in {}: frontend downlink no longer supports field_mappings; "
                "use direct schema routing or convert plugin",
                path);
            return false;
          }

          if VUNLIKELY (mapping.payload_encoding.empty()) {
            MLOG_W("Invalid foxglove_msgs mapping in {}: missing encoding", path);
            return false;
          }

          if VUNLIKELY (mapping.payload_encoding != "json" && mapping.payload_encoding != "text" &&
                        mapping.payload_encoding != "protobuf" && !is_flatbuffers_encoding(mapping.payload_encoding)) {
            MLOG_W("Invalid foxglove_msgs mapping in {}: unsupported target encoding {}", path,
                   mapping.payload_encoding);
            return false;
          }

          mapping.schema_type = SchemaData::convert_encoding(mapping.payload_encoding);

          if VUNLIKELY (mapping.schema_type == SchemaType::kUnknown || mapping.schema_type == SchemaType::kZeroCopy) {
            MLOG_W("Invalid foxglove_msgs mapping in {}: unsupported target encoding {}", path,
                   mapping.payload_encoding);
            return false;
          }

          if VUNLIKELY (mapping.schema_type == SchemaType::kFlatbuffers) {
            mapping.payload_encoding = "flatbuffers";
          }

          if VUNLIKELY (!is_target_encoding_compatible(mapping.ser, mapping.payload_encoding)) {
            MLOG_W(
                "Invalid foxglove_msgs mapping in {}: ser={} is incompatible with target encoding {}; frontend "
                "payloads are always JSON, and encoding selects the backend target encoding",
                path, mapping.ser, mapping.payload_encoding);
            return false;
          }

          if VUNLIKELY (!finalize_mapping(mapping, path)) {
            return false;
          }

          if VUNLIKELY (has_explicit_schema && mapping.schema.empty()) {
            MLOG_W("Invalid foxglove_msgs mapping in {}: explicit frontend schema config resolved to empty schema",
                   path);
            return false;
          }

          loaded_mappings.emplace_back(std::move(mapping));
          return true;
        } catch (const std::exception& e) {
          MLOG_W("Invalid foxglove_msgs entry in {}: {}", path, e.what());
          return false;
        }
      });

  if VLIKELY (ok) {
    mappings_.insert(mappings_.end(), std::make_move_iterator(loaded_mappings.begin()),
                     std::make_move_iterator(loaded_mappings.end()));
  }

  return ok;
}

bool VlinkConvert::finalize_mapping(CommandMapping& mapping, std::string_view path) {
  mapping.encoding = "json";

  if VUNLIKELY (!resolve_input_schema(mapping)) {
    MLOG_W("Invalid foxglove_msgs mapping in {}: failed to resolve input schema for topic {}", path,
           mapping.topic.empty() ? "<dynamic>" : mapping.topic);
    return false;
  }

  return true;
}

bool VlinkConvert::resolve_input_schema(CommandMapping& mapping) {
  mapping.encoding = "json";

  if VUNLIKELY (mapping.payload_encoding.empty()) {
    if VLIKELY (mapping.schema_type == SchemaType::kProtobuf) {
      mapping.payload_encoding = "protobuf";
    } else if VLIKELY (mapping.schema_type == SchemaType::kFlatbuffers) {
      mapping.payload_encoding = "flatbuffers";
    } else if VLIKELY (mapping.schema_type == SchemaType::kRaw) {
      if VLIKELY (is_json_ser(mapping.ser)) {
        mapping.payload_encoding = "json";
      } else if VLIKELY (is_text_ser(mapping.ser)) {
        mapping.payload_encoding = "text";
      }
    }
  }

  if VUNLIKELY (mapping.schema_type == SchemaType::kUnknown) {
    mapping.schema_type = SchemaData::convert_encoding(mapping.payload_encoding);
  }

  if VUNLIKELY (mapping.schema_type == SchemaType::kUnknown || mapping.schema_type == SchemaType::kZeroCopy) {
    return false;
  }

  if VLIKELY (mapping.schema_type == SchemaType::kProtobuf) {
    if VUNLIKELY (mapping.payload_encoding != "protobuf") {
      MLOG_W("Foxglove frontend target encoding mismatch: ser={} encoding={}", mapping.ser, mapping.payload_encoding);
      return false;
    }
  } else if VLIKELY (mapping.schema_type == SchemaType::kFlatbuffers) {
    if VUNLIKELY (!is_flatbuffers_encoding(mapping.payload_encoding)) {
      MLOG_W("Foxglove frontend target encoding mismatch: ser={} encoding={}", mapping.ser, mapping.payload_encoding);
      return false;
    }

    mapping.payload_encoding = "flatbuffers";
  } else if VUNLIKELY (mapping.payload_encoding != "json" && mapping.payload_encoding != "text") {
    MLOG_W("Foxglove frontend target encoding mismatch: ser={} encoding={}", mapping.ser, mapping.payload_encoding);
    return false;
  }

  if VUNLIKELY (!is_target_encoding_compatible(mapping.ser, mapping.payload_encoding)) {
    MLOG_W("Foxglove frontend target encoding mismatch: ser={} encoding={}", mapping.ser, mapping.payload_encoding);
    return false;
  }

  if VUNLIKELY (!is_json_schema_encoding(mapping.schema_encoding)) {
    MLOG_W("Foxglove frontend publish schema must use jsonschema/json: topic={} schema_encoding={}",
           mapping.topic.empty() ? "<dynamic>" : mapping.topic, mapping.schema_encoding);
    return false;
  }

  if VUNLIKELY (mapping.payload_encoding == "protobuf" && !ensure_protobuf_json_util("route validation", mapping.ser)) {
    return false;
  }

  if VUNLIKELY (mapping.schema.empty()) {
    if VLIKELY (mapping.payload_encoding == "protobuf" && !mapping.ser.empty()) {
      if VUNLIKELY (!build_proto_json_schema(find_proto_descriptor(mapping.ser), mapping.schema)) {
        return false;
      }
#ifdef VLINK_HAS_FBS_COMPILER
    } else if VLIKELY (mapping.payload_encoding == "flatbuffers" && !mapping.ser.empty()) {
      std::string schema_storage;
      const auto* schema = resolve_fbs_schema(mapping.ser, schema_storage);

      if VUNLIKELY (!build_fbs_json_schema(schema, mapping.schema)) {
        return false;
      }
#endif
    } else if VLIKELY (mapping.payload_encoding == "text") {
      mapping.schema = make_json_schema_leaf("string").dump();
    } else {
      mapping.schema = make_default_json_object_schema().dump();
    }
  } else if VUNLIKELY (mapping.schema_encoding.empty()) {
    mapping.schema_encoding = "jsonschema";
  }

  if VUNLIKELY (mapping.schema_name.empty()) {
    auto schema_topic = !mapping.topic.empty() ? mapping.topic : get_primary_static_url(mapping.url_selector);

    if VLIKELY (!mapping.ser.empty() && !is_json_ser(mapping.ser) && !is_text_ser(mapping.ser)) {
      mapping.schema_name = mapping.ser;
    } else {
      std::string schema_name = "webviz.publish.";
      bool last_was_sep = true;

      for (unsigned char ch : schema_topic) {
        if VLIKELY (std::isalnum(ch) != 0U || ch == '_') {
          schema_name.push_back(static_cast<char>(std::tolower(ch)));
          last_was_sep = false;
        } else if VLIKELY (!last_was_sep) {
          schema_name.push_back('.');
          last_was_sep = true;
        }
      }

      while (!schema_name.empty() && schema_name.back() == '.') {
        schema_name.pop_back();
      }

      if VUNLIKELY (schema_name == "webviz.publish") {
        schema_name += ".channel";
      }

      mapping.schema_name = std::move(schema_name);
    }
  }

  if VUNLIKELY (mapping.schema_encoding.empty()) {
    mapping.schema_encoding = "jsonschema";
  }

  if VUNLIKELY (!mapping.schema.empty()) {
    try {
      auto parsed_schema = Json::parse(mapping.schema);

      if VUNLIKELY (!parsed_schema.is_object()) {
        return false;
      }

      mapping.schema = parsed_schema.dump();
    } catch (const std::exception& e) {
      MLOG_W("Invalid foxglove frontend JSON schema for topic {}: {}",
             mapping.topic.empty() ? "<dynamic>" : mapping.topic, e.what());
      return false;
    }
  }

  return !mapping.schema.empty();
}

const google::protobuf::Descriptor* VlinkConvert::find_proto_descriptor(const std::string& proto_name) {
  if VLIKELY (schema_interface_) {
    auto* desc_ptr = schema_interface_->search_protobuf_descriptor(proto_name);

    if VLIKELY (desc_ptr) {
      return reinterpret_cast<const google::protobuf::Descriptor*>(desc_ptr);
    }
  }

#ifdef VLINK_HAS_PROTO_COMPILER
  auto iter = imported_proto_descriptors_.find(proto_name);

  if VLIKELY (iter != imported_proto_descriptors_.end()) {
    return iter->second;
  }
#endif

  return nullptr;
}

bool VlinkConvert::resolve_proto_schema(const std::string& proto_name, std::string& schema_data) {
  if VLIKELY (schema_interface_) {
    auto schema = schema_interface_->search_schema(proto_name, SchemaType::kProtobuf);
    if VLIKELY (schema.schema_type == SchemaType::kProtobuf && !schema.data.empty()) {
      schema_data.assign(reinterpret_cast<const char*>(schema.data.data()), schema.data.size());
      return true;
    }
  }

  const auto* desc = find_proto_descriptor(proto_name);

  if VUNLIKELY (!desc) {
    return false;
  }

  std::vector<const google::protobuf::FileDescriptor*> ordered;
#if GOOGLE_PROTOBUF_VERSION >= 6030000
  std::unordered_set<std::string_view> seen;
#else
  std::unordered_set<std::string> seen;
#endif

  MoveFunction<void(const google::protobuf::FileDescriptor*)> dfs =
      [&dfs, &ordered, &seen](const google::protobuf::FileDescriptor* fd) {
        if VUNLIKELY (!fd) {
          return;
        }

#if GOOGLE_PROTOBUF_VERSION >= 6030000
        std::string_view name = fd->name();
#else
        const std::string& name = fd->name();
#endif

        if VUNLIKELY (seen.count(name) > 0U) {
          return;
        }

        seen.insert(name);

        for (int i = 0; i < fd->dependency_count(); ++i) {
          dfs(fd->dependency(i));
        }

        ordered.emplace_back(fd);
      };

  dfs(desc->file());

  google::protobuf::FileDescriptorSet fd_set;

  for (const auto* fd : ordered) {
    fd->CopyTo(fd_set.add_file());
  }

  schema_data.resize(fd_set.ByteSizeLong());

  if VUNLIKELY (!fd_set.SerializeToArray(schema_data.data(), static_cast<int>(schema_data.size()))) {
    schema_data.clear();
    return false;
  }

  return true;
}

std::unique_ptr<google::protobuf::Message> VlinkConvert::create_proto_message(const std::string& proto_name) {
  const auto* desc = find_proto_descriptor(proto_name);

  if VUNLIKELY (!desc) {
    return nullptr;
  }

  const google::protobuf::Message* prototype = nullptr;

#ifdef VLINK_HAS_PROTO_COMPILER

  if VLIKELY (disk_factory_ && imported_proto_descriptors_.find(proto_name) != imported_proto_descriptors_.end()) {
    prototype = disk_factory_->GetPrototype(desc);
  }
#endif

  if VUNLIKELY (!prototype && schema_interface_) {
    prototype = proto_factory_.GetPrototype(desc);
  }

  if VUNLIKELY (!prototype) {
    return nullptr;
  }

  return std::unique_ptr<google::protobuf::Message>(prototype->New());
}

std::unique_ptr<google::protobuf::Message> VlinkConvert::deserialize_proto_message(const std::string& proto_name,
                                                                                   const Bytes& raw) {
  auto message = create_proto_message(proto_name);

  if VUNLIKELY (!message) {
    return nullptr;
  }

  if VUNLIKELY (raw.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return nullptr;
  }

  if VUNLIKELY (!message->ParseFromArray(raw.data(), static_cast<int>(raw.size()))) {
    return nullptr;
  }

  return message;
}

int VlinkConvert::get_mapping_match_score(const CommandMapping& mapping, const CommandChannel& channel) {
  const auto url_score = score_url_selector(channel.topic, mapping.url_selector);

  if VUNLIKELY (url_score < 0) {
    return -1;
  }

  if VUNLIKELY (!mapping.encoding.empty() && !channel.encoding.empty() && mapping.encoding != channel.encoding) {
    return -1;
  }

  if VUNLIKELY (!mapping.schema_name.empty() && !channel.schema_name.empty() &&
                mapping.schema_name != channel.schema_name) {
    return -1;
  }

  if VUNLIKELY (!mapping.schema_encoding.empty() && !channel.schema_encoding.empty() &&
                mapping.schema_encoding != channel.schema_encoding) {
    return -1;
  }

  int score = 0;

  score += url_score * 2;

  if VLIKELY (!mapping.encoding.empty() && !channel.encoding.empty()) {
    score += 2;
  }

  if VLIKELY (!mapping.schema_name.empty() && !channel.schema_name.empty()) {
    score += 2;
  }

  if VLIKELY (!mapping.schema_encoding.empty() && !channel.schema_encoding.empty()) {
    ++score;
  }

  return score;
}

const CommandMapping* VlinkConvert::find_mapping(const CommandChannel& channel, bool* ambiguous) const {
  struct MappingCache final {
    const VlinkConvert* owner{nullptr};
    std::string topic;
    std::string encoding;
    std::string schema_name;
    std::string schema_encoding;
    const CommandMapping* mapping{nullptr};
    bool ambiguous{false};
  };

  thread_local MappingCache cache;

  if VLIKELY (cache.owner == this && cache.topic == channel.topic && cache.encoding == channel.encoding &&
              cache.schema_name == channel.schema_name && cache.schema_encoding == channel.schema_encoding) {
    if VLIKELY (ambiguous) {
      *ambiguous = cache.ambiguous;
    }

    return cache.ambiguous ? nullptr : cache.mapping;
  }

  const CommandMapping* best = nullptr;
  int best_score = -1;
  bool has_ambiguity = false;

  for (const auto& mapping : mappings_) {
    const auto score = get_mapping_match_score(mapping, channel);

    if VUNLIKELY (score < 0) {
      continue;
    }

    if VLIKELY (score > best_score) {
      best = &mapping;
      best_score = score;
      has_ambiguity = false;
      continue;
    }

    if VUNLIKELY (score == best_score) {
      has_ambiguity = true;
    }
  }

  if VLIKELY (ambiguous) {
    *ambiguous = has_ambiguity;
  }

  cache.owner = this;
  cache.topic = channel.topic;
  cache.encoding = channel.encoding;
  cache.schema_name = channel.schema_name;
  cache.schema_encoding = channel.schema_encoding;
  cache.mapping = best;
  cache.ambiguous = has_ambiguity;

  if VUNLIKELY (has_ambiguity) {
    MLOG_W("Ambiguous foxglove publish route: topic={} encoding={} schema_name={} schema_encoding={}", channel.topic,
           channel.encoding, channel.schema_name, channel.schema_encoding);
    return nullptr;
  }

  return best;
}

bool VlinkConvert::build_route(const CommandMapping& mapping, const CommandChannel& channel, CommandRoute& route) {
  route = CommandRoute{};
  route.mapping = &mapping;
  route.url = !channel.topic.empty() ? channel.topic : get_primary_static_url(mapping.url_selector);
  route.ser = mapping.ser;
  route.payload_encoding = mapping.payload_encoding;
  route.schema_type = mapping.schema_type;
  route.web_channel.topic = channel.topic.empty() ? route.url : channel.topic;
  route.web_channel.encoding = channel.encoding.empty() ? mapping.encoding : channel.encoding;
  route.web_channel.schema_name = channel.schema_name.empty() ? mapping.schema_name : channel.schema_name;
  route.web_channel.schema_encoding =
      channel.schema_encoding.empty() ? mapping.schema_encoding : channel.schema_encoding;
  route.web_channel.schema = channel.schema.empty() ? mapping.schema : channel.schema;
  return !route.url.empty() && !route.ser.empty();
}

bool VlinkConvert::resolve_route(const CommandChannel& channel, CommandRoute& route) {
  route = CommandRoute{};
  route.web_channel.topic = channel.topic;
  route.web_channel.encoding = channel.encoding;
  route.web_channel.schema_name = channel.schema_name;
  route.web_channel.schema_encoding = channel.schema_encoding;
  route.web_channel.schema = channel.schema;

  bool ambiguous = false;
  const auto* mapping = find_mapping(channel, &ambiguous);

  if VLIKELY (mapping != nullptr) {
    return build_route(*mapping, channel, route);
  }

  if VUNLIKELY (ambiguous) {
    return false;
  }

  if VUNLIKELY (convert_plugin_ &&
                convert_plugin_->can_publish(route.web_channel, ConvertPluginInterface::Target::kFoxglove)) {
    ConvertPluginInterface::PublishInfo publish_info;

    if VLIKELY (convert_plugin_->get_publish(route.web_channel, ConvertPluginInterface::Target::kFoxglove,
                                             publish_info) &&
                !publish_info.url.empty() && !publish_info.ser_type.empty()) {
      route.url = std::move(publish_info.url);
      route.ser = std::move(publish_info.ser_type);
      route.schema_type =
          SchemaData::is_valid_type(publish_info.schema_type) ? publish_info.schema_type : SchemaType::kUnknown;

      if VUNLIKELY (route.schema_type == SchemaType::kUnknown) {
        MLOG_W("Convert plugin publish route is missing schema_type for backend target: {}", channel.topic);
        return false;
      }

      route.payload_encoding = std::string(SchemaData::convert_type(route.schema_type));
      route.via_plugin = true;
      return true;
    }

    MLOG_W("Convert plugin matched a foxglove publish channel but did not return publish info: {}", channel.topic);
    return false;
  }

  return false;
}

std::vector<CommandChannel> VlinkConvert::get_publish_channels() const {
  std::unordered_map<std::string, CommandChannel> by_topic;
  std::unordered_set<std::string> blocked_topics;

  for (const auto& mapping : mappings_) {
    if VUNLIKELY (mapping.encoding.empty()) {
      continue;
    }

    if VUNLIKELY (mapping.schema.empty() || mapping.schema_name.empty() || mapping.schema_encoding.empty()) {
      continue;
    }

    if VUNLIKELY (!is_static_url_selector(mapping.url_selector)) {
      continue;
    }

    for (const auto& url : mapping.url_selector.whitelist_exact) {
      if VUNLIKELY (url.empty()) {
        continue;
      }

      CommandChannel channel;
      channel.topic = url;
      channel.encoding = mapping.encoding;
      channel.schema_name = mapping.schema_name;
      channel.schema_encoding = mapping.schema_encoding;
      channel.schema = mapping.schema;

      if VUNLIKELY (blocked_topics.count(channel.topic) > 0U) {
        continue;
      }

      auto topic_iter = by_topic.find(channel.topic);

      if VLIKELY (topic_iter == by_topic.end()) {
        by_topic.emplace(channel.topic, std::move(channel));
        continue;
      }

      const auto& existing = topic_iter->second;

      if VUNLIKELY (existing.encoding != channel.encoding || existing.schema_name != channel.schema_name ||
                    existing.schema_encoding != channel.schema_encoding || existing.schema != channel.schema) {
        MLOG_W("Skipping foxglove publish schema advertise for ambiguous topic: {}", channel.topic);
        blocked_topics.emplace(channel.topic);
        by_topic.erase(topic_iter);
      }
    }
  }

  std::vector<CommandChannel> publish_channels;
  publish_channels.reserve(by_topic.size());

  for (auto& topic_channel : by_topic) {
    publish_channels.emplace_back(std::move(topic_channel.second));
  }

  std::sort(publish_channels.begin(), publish_channels.end(),
            [](const CommandChannel& lhs, const CommandChannel& rhs) { return lhs.topic < rhs.topic; });

  return publish_channels;
}

CommandMessage VlinkConvert::encode_frontend_message(const CommandRoute& route, const Bytes& raw) {
  CommandMessage result;
  result.url = route.url;
  result.ser = route.ser;

  if VUNLIKELY (route.via_plugin) {
    if VLIKELY (convert_plugin_ &&
                convert_plugin_->convert_publish(route.web_channel, raw, ConvertPluginInterface::Target::kFoxglove,
                                                 result.payload)) {
      result.success = true;
      return result;
    }

    return result;
  }

  return encode_direct_route(route, raw);
}

bool VlinkConvert::decode_backend_message_to_json(const std::string& ser, SchemaType schema_type, const Bytes& raw,
                                                  Bytes& payload) {
  payload.clear();

  if VUNLIKELY (schema_type == SchemaType::kRaw && ser == "json") {
    payload.shallow_copy(raw);
    return true;
  }

  if VUNLIKELY (schema_type == SchemaType::kRaw && (ser == "text" || ser == "std::string" || ser == "string")) {
    std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());
    auto normalized = Json(text).dump();
    payload = Bytes::deep_copy(reinterpret_cast<const uint8_t*>(normalized.data()), normalized.size());
    return true;
  }

  if VLIKELY (schema_type == SchemaType::kProtobuf && find_proto_descriptor(ser)) {
    auto message = deserialize_proto_message(ser, raw);

    if VUNLIKELY (!message) {
      MLOG_W("Failed to decode backend protobuf payload for {}", ser);
      return false;
    }

#ifdef VLINK_HAS_PROTOBUF_JSON_UTIL
    std::string json_text;
    google::protobuf::util::JsonPrintOptions options;
    const auto status = google::protobuf::util::MessageToJsonString(*message, &json_text, options);

    if VUNLIKELY (!status.ok()) {
      MLOG_W("Failed to encode protobuf {} as JSON: {}", ser, status.ToString());
      return false;
    }

    payload = Bytes::deep_copy(reinterpret_cast<const uint8_t*>(json_text.data()), json_text.size());
    return true;
#else
    return ensure_protobuf_json_util("response conversion", ser);
#endif
  }

#ifdef VLINK_HAS_FBS_COMPILER

  if VLIKELY (schema_type == SchemaType::kFlatbuffers) {
    std::lock_guard lock(mtx_);

    if VUNLIKELY (!find_fbs_parser_locked(ser)) {
      MLOG_W("No FlatBuffers parser available for backend payload: {}", ser);
      return false;
    }

    auto parser_iter = fbs_parsers_.find(ser);

    if VUNLIKELY (parser_iter == fbs_parsers_.end() || parser_iter->second >= fbs_parser_vec_.size()) {
      MLOG_W("FlatBuffers parser lookup failed for backend payload: {}", ser);
      return false;
    }

    auto& parser = *fbs_parser_vec_[parser_iter->second];
    parser.SetRootType(ser.c_str());
    parser.opts.strict_json = true;

    std::string json_text;
    const auto* err = flatbuffers::GenText(parser, raw.data(), &json_text);

    if VUNLIKELY (err != nullptr) {
      MLOG_W("Failed to encode FlatBuffers {} as JSON: {}", ser, err);
      return false;
    }

    try {
      auto parsed = Json::parse(json_text);
      auto normalized = parsed.dump();
      payload = Bytes::deep_copy(reinterpret_cast<const uint8_t*>(normalized.data()), normalized.size());
      return true;
    } catch (const std::exception& e) {
      MLOG_W("Failed to normalize FlatBuffers JSON payload for {}: {}", ser, e.what());
      return false;
    }
  }
#endif

  MLOG_W("Unsupported backend response serialization for Foxglove JSON conversion: {} (schema_type={})", ser,
         static_cast<int>(schema_type));
  return false;
}

CommandMessage VlinkConvert::encode_direct_route(const CommandRoute& route, const Bytes& raw) {
  if VUNLIKELY (!route.mapping) {
    return {};
  }

  return encode_json_payload(route, raw);
}

CommandMessage VlinkConvert::encode_json_payload(const CommandRoute& route, const Bytes& raw) {
  CommandMessage result;
  result.url = route.url;
  result.ser = route.ser;

  if VUNLIKELY (!route.mapping) {
    return result;
  }

  std::string raw_text(reinterpret_cast<const char*>(raw.data()), raw.size());

  if VLIKELY (route.mapping->payload_encoding == "json") {
    result.payload.shallow_copy(raw);
    result.success = true;
    return result;
  }

  if VUNLIKELY (route.mapping->payload_encoding == "text") {
    try {
      auto input = Json::parse(raw_text);
      std::string text_payload;

      if VLIKELY (input.is_string()) {
        text_payload = input.get<std::string>();
      } else {
        text_payload = input.dump();
      }

      result.payload = Bytes::deep_copy(reinterpret_cast<const uint8_t*>(text_payload.data()), text_payload.size());
      result.success = true;
    } catch (const std::exception& e) {
      MLOG_W("Failed to parse client JSON text payload for {}: {}", route.url, e.what());
    }

    return result;
  }

  if VUNLIKELY (route.mapping->payload_encoding == "protobuf") {
    auto message = create_proto_message(route.ser);

    if VUNLIKELY (!message) {
      MLOG_W("No protobuf descriptor for target type: {}", route.ser);
      return result;
    }

#ifdef VLINK_HAS_PROTOBUF_JSON_UTIL
    google::protobuf::util::JsonParseOptions options;
    const auto status = google::protobuf::util::JsonStringToMessage(raw_text, message.get(), options);

    if VUNLIKELY (!status.ok()) {
      MLOG_W("Failed to parse client JSON into protobuf {}: {}", route.ser, status.ToString());
      return result;
    }

    if VUNLIKELY (!serialize_proto_to_bytes(*message, result.payload)) {
      MLOG_W("Failed to serialize protobuf command: {}", route.ser);
      return result;
    }

    result.success = true;
    return result;
#else
    ensure_protobuf_json_util("request conversion", route.ser);
    return result;
#endif
  }

#ifdef VLINK_HAS_FBS_COMPILER

  if VUNLIKELY (route.mapping->payload_encoding == "flatbuffers") {
    std::lock_guard lock(mtx_);

    if VUNLIKELY (!find_fbs_parser_locked(route.ser)) {
      return result;
    }

    auto parser_iter = fbs_parsers_.find(route.ser);

    if VUNLIKELY (parser_iter == fbs_parsers_.end() || parser_iter->second >= fbs_parser_vec_.size()) {
      return result;
    }

    auto& parser = *fbs_parser_vec_[parser_iter->second];
    parser.builder_.Clear();
    parser.SetRootType(route.ser.c_str());

    if VUNLIKELY (!parser.Parse(raw_text.c_str())) {
      MLOG_W("Failed to parse client JSON into FlatBuffers {}: {}", route.ser, parser.error_);
      return result;
    }

    result.payload = Bytes::deep_copy(parser.builder_.GetBufferPointer(), parser.builder_.GetSize());
    result.success = !result.payload.empty();
    return result;
  }
#endif

  MLOG_W("Unsupported frontend JSON route: target_encoding={} url={}", route.mapping->payload_encoding, route.url);

  return result;
}

#ifdef VLINK_HAS_FBS_COMPILER
bool VlinkConvert::init_fbs_resolver() {
  if VUNLIKELY (config_.fbs_dir.empty()) {
    return false;
  }

  auto fbs_path = std::filesystem::path(config_.fbs_dir);
  std::error_code ec;

  if VUNLIKELY (!std::filesystem::exists(fbs_path, ec) || ec) {
    MLOG_W("Client FBS directory does not exist: {}", config_.fbs_dir);
    return false;
  }

  std::vector<std::filesystem::path> fbs_files;
  scan_fbs_files(fbs_path, fbs_files);

  if VUNLIKELY (fbs_files.empty()) {
    return false;
  }

  std::string root_dir_str = Helpers::path_to_string(fbs_path);
  const char* include_dirs[] = {root_dir_str.c_str(), nullptr};
  fbs_parser_vec_.reserve(fbs_parser_vec_.size() + fbs_files.size());

  for (const auto& fbs_file : fbs_files) {
    std::string schema_file;

    if VUNLIKELY (!flatbuffers::LoadFile(Helpers::path_to_string(fbs_file).c_str(), false, &schema_file)) {
      continue;
    }

    auto parser = std::make_unique<flatbuffers::Parser>();
    std::string sub_dir_str = Helpers::path_to_string(fbs_file.parent_path());

    if VLIKELY (sub_dir_str == root_dir_str) {
      if VUNLIKELY (!parser->Parse(schema_file.c_str(), include_dirs)) {
        continue;
      }
    } else {
      const char* full_dirs[] = {root_dir_str.c_str(), sub_dir_str.c_str(), nullptr};

      if VUNLIKELY (!parser->Parse(schema_file.c_str(), full_dirs)) {
        continue;
      }
    }

    std::vector<std::string> type_names;

    for (auto* def : parser->structs_.vec) {
      if VUNLIKELY (!def || def->generated) {
        continue;
      }

      if VLIKELY (fbs_parsers_.find(def->name) == fbs_parsers_.end()) {
        type_names.emplace_back(def->name);
      }
    }

    if VUNLIKELY (type_names.empty()) {
      continue;
    }

    const size_t parser_index = fbs_parser_vec_.size();
    fbs_parser_vec_.emplace_back(std::move(parser));

    for (const auto& type_name : type_names) {
      fbs_parsers_[type_name] = parser_index;
    }
  }

  return !fbs_parsers_.empty();
}

bool VlinkConvert::find_fbs_parser_locked(const std::string& fbs_ser) {
  if VLIKELY (fbs_parsers_.find(fbs_ser) != fbs_parsers_.end()) {
    return true;
  }

  if VUNLIKELY (fbs_not_found_.count(fbs_ser) > 0) {
    return false;
  }

  if VLIKELY (schema_interface_) {
    auto schema = schema_interface_->search_schema(fbs_ser, SchemaType::kFlatbuffers);
    if VLIKELY (!schema.data.empty() && schema.schema_type == SchemaType::kFlatbuffers) {
      auto parser = std::make_unique<flatbuffers::Parser>();

      if VLIKELY (parser->Deserialize(reinterpret_cast<const uint8_t*>(schema.data.data()), schema.data.size()) &&
                  parser->SetRootType(fbs_ser.c_str())) {
        const size_t parser_index = fbs_parser_vec_.size();
        fbs_parser_vec_.emplace_back(std::move(parser));
        fbs_parsers_[fbs_ser] = parser_index;
        return true;
      }
    }
  }

  if VUNLIKELY (config_.fbs_dir.empty()) {
    fbs_not_found_.insert(fbs_ser);
    return false;
  }

  auto fbs_path = std::filesystem::path(config_.fbs_dir);
  std::error_code ec;

  if VUNLIKELY (!std::filesystem::exists(fbs_path, ec) || ec) {
    fbs_not_found_.insert(fbs_ser);
    return false;
  }

  std::vector<std::filesystem::path> fbs_files;
  scan_fbs_files(fbs_path, fbs_files);

  std::string root_dir_str = Helpers::path_to_string(fbs_path);
  const char* include_dirs[] = {root_dir_str.c_str(), nullptr};

  for (const auto& fbs_file : fbs_files) {
    std::string schema_file;

    if VUNLIKELY (!flatbuffers::LoadFile(Helpers::path_to_string(fbs_file).c_str(), false, &schema_file)) {
      continue;
    }

    auto parser = std::make_unique<flatbuffers::Parser>();
    std::string sub_dir_str = Helpers::path_to_string(fbs_file.parent_path());

    if VLIKELY (sub_dir_str == root_dir_str) {
      if VUNLIKELY (!parser->Parse(schema_file.c_str(), include_dirs)) {
        continue;
      }
    } else {
      const char* full_dirs[] = {root_dir_str.c_str(), sub_dir_str.c_str(), nullptr};

      if VUNLIKELY (!parser->Parse(schema_file.c_str(), full_dirs)) {
        continue;
      }
    }

    if VLIKELY (parser->LookupStruct(fbs_ser)) {
      parser->SetRootType(fbs_ser.c_str());
      const size_t parser_index = fbs_parser_vec_.size();
      fbs_parser_vec_.emplace_back(std::move(parser));
      fbs_parsers_[fbs_ser] = parser_index;
      return true;
    }
  }

  fbs_not_found_.insert(fbs_ser);
  return false;
}

const reflection::Schema* VlinkConvert::resolve_fbs_schema(const std::string& fbs_ser, std::string& bfbs_storage) {
  std::lock_guard lock(mtx_);
  auto cache_iter = fbs_schema_cache_.find(fbs_ser);

  if VLIKELY (cache_iter != fbs_schema_cache_.end()) {
    bfbs_storage = cache_iter->second;
    return reflection::GetSchema(reinterpret_cast<const uint8_t*>(bfbs_storage.data()));
  }

  if VUNLIKELY (!find_fbs_parser_locked(fbs_ser)) {
    return nullptr;
  }

  auto parser_iter = fbs_parsers_.find(fbs_ser);

  if VUNLIKELY (parser_iter == fbs_parsers_.end() || parser_iter->second >= fbs_parser_vec_.size()) {
    return nullptr;
  }

  auto& parser = *fbs_parser_vec_[parser_iter->second];
  parser.SetRootType(fbs_ser.c_str());
  parser.Serialize();

  auto* bfbs_data = parser.builder_.GetBufferPointer();
  auto bfbs_size = parser.builder_.GetSize();

  if VUNLIKELY (!bfbs_data || bfbs_size == 0) {
    return nullptr;
  }

  auto& cached = fbs_schema_cache_[fbs_ser];
  cached.assign(reinterpret_cast<const char*>(bfbs_data), bfbs_size);
  bfbs_storage = cached;
  return reflection::GetSchema(reinterpret_cast<const uint8_t*>(bfbs_storage.data()));
}

#endif

}  // namespace webviz
}  // namespace vlink
