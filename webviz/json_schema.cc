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

#include "./schema_registry.h"

namespace vlink {
namespace webviz {

using Json = nlohmann::json;

static Json proto_object_schema(const google::protobuf::Descriptor& message, Json& definitions);

static Json proto_field_schema(const google::protobuf::FieldDescriptor& field, Json& definitions) {
  using Field = google::protobuf::FieldDescriptor;
  Json schema;
  switch (field.cpp_type()) {
    case Field::CPPTYPE_MESSAGE:
      schema = proto_object_schema(*field.message_type(), definitions);
      break;
    case Field::CPPTYPE_INT64:
    case Field::CPPTYPE_UINT64:
      schema = {
          {"oneOf", Json::array({Json{{"type", "string"}, {"pattern", "^-?[0-9]+$"}}, Json{{"type", "integer"}}})}};
      break;
    case Field::CPPTYPE_INT32:
    case Field::CPPTYPE_UINT32:
      schema = {{"type", "integer"}};
      break;
    case Field::CPPTYPE_FLOAT:
    case Field::CPPTYPE_DOUBLE:
      schema = {{"oneOf", {{{"type", "number"}}, {{"enum", {"NaN", "Infinity", "-Infinity"}}}}}};
      break;
    case Field::CPPTYPE_BOOL:
      schema = {{"type", "boolean"}};
      break;
    case Field::CPPTYPE_STRING:
      schema = {{"type", "string"}};
      if (field.type() == Field::TYPE_BYTES) {
        schema["contentEncoding"] = "base64";
      }
      break;
    case Field::CPPTYPE_ENUM: {
      Json values = Json::array();
      for (int i = 0; i < field.enum_type()->value_count(); ++i) {
        values.push_back(std::string(field.enum_type()->value(i)->name()));
      }
      schema = {{"oneOf", Json::array({Json{{"enum", std::move(values)}}, Json{{"type", "integer"}}})}};
      break;
    }
  }
  if (field.is_map()) {
    return {{"type", "object"},
            {"additionalProperties", proto_field_schema(*field.message_type()->map_value(), definitions)}};
  }
  return field.is_repeated() ? Json{{"type", "array"}, {"items", std::move(schema)}} : schema;
}

static Json proto_object_schema(const google::protobuf::Descriptor& message, Json& definitions) {
  const std::string name(message.full_name());
  if (name == "google.protobuf.Timestamp" || name == "google.protobuf.Duration" ||
      name == "google.protobuf.FieldMask") {
    return {{"type", "string"}};
  }
  if (name == "google.protobuf.Struct") {
    return {{"type", "object"}};
  }
  if (name == "google.protobuf.Value" || name == "google.protobuf.Any") {
    return Json::object();
  }
  if (name == "google.protobuf.ListValue") {
    return {{"type", "array"}, {"items", Json::object()}};
  }
  if (name.rfind("google.protobuf.", 0) == 0 && name.size() > 5 && name.substr(name.size() - 5) == "Value" &&
      message.field_count() == 1 && message.field(0)->name() == "value") {
    return proto_field_schema(*message.field(0), definitions);
  }
  if (!definitions.contains(name)) {
    definitions[name] = Json::object();
    Json properties = Json::object();
    for (int i = 0; i < message.field_count(); ++i) {
      const auto& field = *message.field(i);
      properties[std::string(field.json_name())] = proto_field_schema(field, definitions);
    }
    definitions[name] = {{"type", "object"}, {"properties", std::move(properties)}};
  }
  return {{"$ref", "#/$defs/" + name}};
}

static Json fbs_scalar_schema(const reflection::Schema& source, reflection::BaseType type, int32_t index,
                              Json& definitions);

static Json fbs_field_schema(const reflection::Schema& source, const reflection::Type& type, Json& definitions) {
  if (type.base_type() == reflection::Vector || type.base_type() == reflection::Array) {
    Json result = {{"type", "array"}, {"items", fbs_scalar_schema(source, type.element(), type.index(), definitions)}};
    if (type.base_type() == reflection::Array) {
      result["minItems"] = type.fixed_length();
      result["maxItems"] = type.fixed_length();
    }
    return result;
  }
  return fbs_scalar_schema(source, type.base_type(), type.index(), definitions);
}

static Json fbs_scalar_schema(const reflection::Schema& source, reflection::BaseType type, int32_t index,
                              Json& definitions) {
  if (type == reflection::Obj && index >= 0) {
    const auto* object = source.objects()->Get(static_cast<flatbuffers::uoffset_t>(index));
    const auto name = object->name()->str();
    if (!definitions.contains(name)) {
      definitions[name] = Json::object();
      Json properties = Json::object();
      for (const auto* field : *object->fields()) {
        properties[field->name()->str()] = fbs_field_schema(source, *field->type(), definitions);
      }
      definitions[name] = {{"type", "object"}, {"properties", std::move(properties)}};
    }
    return {{"$ref", "#/$defs/" + name}};
  }
  if (type == reflection::String) {
    return {{"type", "string"}};
  }
  if (type == reflection::Bool) {
    return {{"type", "boolean"}};
  }
  if (type == reflection::Float || type == reflection::Double) {
    return {{"type", "number"}};
  }
  if (flatbuffers::IsInteger(type)) {
    if (index >= 0 && source.enums() && static_cast<size_t>(index) < source.enums()->size()) {
      Json values = Json::array();
      for (const auto* value : *source.enums()->Get(static_cast<flatbuffers::uoffset_t>(index))->values()) {
        values.push_back(value->name()->str());
      }
      return {{"oneOf", Json::array({Json{{"enum", std::move(values)}}, Json{{"type", "integer"}}})}};
    }
    return {{"type", "integer"}};
  }
  return Json::object();
}

std::string source_json_schema(const SourceSchema& source) {
  Json definitions = Json::object();
  Json root;
  if (source.prototype) {
    root = proto_object_schema(*source.prototype->GetDescriptor(), definitions);
  } else if (source.flatbuffer && source.flatbuffer->root_table()) {
    int32_t index = -1;
    for (flatbuffers::uoffset_t i = 0; i < source.flatbuffer->objects()->size(); ++i) {
      if (source.flatbuffer->objects()->Get(i) == source.flatbuffer->root_table()) {
        index = static_cast<int32_t>(i);
        break;
      }
    }
    if (index < 0) {
      return {};
    }
    root = fbs_scalar_schema(*source.flatbuffer, reflection::Obj, index, definitions);
  } else {
    return {};
  }
  if (!definitions.empty()) {
    root["$defs"] = std::move(definitions);
  }
  root["$schema"] = "https://json-schema.org/draft/2020-12/schema";
  return root.dump();
}

}  // namespace webviz
}  // namespace vlink
