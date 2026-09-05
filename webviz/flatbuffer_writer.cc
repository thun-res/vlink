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

#include <flatbuffers/flatbuffer_builder.h>

#include "./mapping.h"

namespace vlink {
namespace webviz {

namespace {

template <typename Visitor>
bool scalar_type(reflection::BaseType type, const Visitor& visit) {
  switch (type) {
    case reflection::Bool:
      return visit(bool{});
    case reflection::Byte:
      return visit(int8_t{});
    case reflection::UByte:
      return visit(uint8_t{});
    case reflection::Short:
      return visit(int16_t{});
    case reflection::UShort:
      return visit(uint16_t{});
    case reflection::Int:
      return visit(int32_t{});
    case reflection::UInt:
      return visit(uint32_t{});
    case reflection::Long:
      return visit(int64_t{});
    case reflection::ULong:
      return visit(uint64_t{});
    case reflection::Float:
      return visit(float{});
    case reflection::Double:
      return visit(double{});
    default:
      return false;
  }
}

class FlatbufferWriter final {
 public:
  FlatbufferWriter(const reflection::Schema& schema, flatbuffers::FlatBufferBuilder& builder)
      : schema_(schema), builder_(builder) {}

  bool table(const reflection::Object& object, const FieldReader& fields, const MessageView& source,
             const std::string& prefix, flatbuffers::uoffset_t& output) {
    if ((source.is_null() && !fields.has_descendant(prefix)) || source.is_array() ||
        !std::holds_alternative<std::monostate>(source.value())) {
      return false;
    }
    struct Field final {
      const reflection::Field* schema;
      std::string path;
      MessageView source;
      flatbuffers::uoffset_t offset{0};
    };
    std::vector<Field> entries;
    entries.reserve(object.fields()->size());
    for (const auto* field : *object.fields()) {
      if (field->deprecated()) {
        continue;
      }
      auto path = prefix.empty() ? field->name()->str() : prefix + "." + field->name()->str();
      const auto* mapped = fields.field(path);
      const auto value = mapped ? fields.view(path) : source.member(field->name()->string_view());
      if (!value.valid() && !(mapped && mapped->expression) && !fields.has_descendant(path)) {
        continue;
      }
      entries.push_back({field, std::move(path), value, 0});
      auto& entry = entries.back();
      const auto type = field->type()->base_type();
      if (mapped && mapped->time_scale && (type != reflection::Obj || !object_type(*field->type()).is_struct())) {
        return false;
      }
      if (flatbuffers::IsScalar(type) || (type == reflection::Obj && object_type(*field->type()).is_struct())) {
        continue;
      }
      if (value.is_null() && !fields.has_descendant(entry.path)) {
        continue;
      }
      if (!offset(*field->type(), fields, entry.source, entry.path, entry.offset)) {
        return false;
      }
    }
    const auto start = builder_.StartTable();
    for (const auto& entry : entries) {
      const auto& field = *entry.schema;
      const auto type = field.type()->base_type();
      if (flatbuffers::IsScalar(type)) {
        auto value = fields.field(entry.path) ? fields.value(entry.path) : entry.source.value(true);
        if (field.optional() && entry.source.is_null() && std::holds_alternative<std::monostate>(value)) {
          continue;
        }
        if (!enum_value(*field.type(), value) || !scalar_type(type, [&](auto scalar) {
              if (!field_numeric(value, scalar)) {
                return false;
              }
              if (field.optional()) {
                builder_.AddElement(field.offset(), scalar);
              } else {
                const auto fallback = std::is_floating_point_v<decltype(scalar)>
                                          ? static_cast<decltype(scalar)>(field.default_real())
                                          : static_cast<decltype(scalar)>(field.default_integer());
                builder_.AddElement(field.offset(), scalar, fallback);
              }
              return true;
            })) {
          return false;
        }
      } else if (type == reflection::Obj && object_type(*field.type()).is_struct()) {
        const auto* mapped = fields.field(entry.path);
        if (entry.source.is_null() && !fields.has_descendant(entry.path) && !(mapped && mapped->expression)) {
          continue;
        }
        const auto& structure = object_type(*field.type());
        builder_.Align(structure.minalign());
        builder_.Pad(structure.bytesize());
        const auto scope = mapped && !mapped->time_scale ? fields.child(entry.source) : fields;
        if (!write_struct(structure, scope, entry.source, entry.path, builder_.GetCurrentBufferPointer())) {
          return false;
        }
        builder_.AddStructOffset(field.offset(), builder_.GetSize());
      } else {
        builder_.AddOffset(field.offset(), flatbuffers::Offset<void>(entry.offset));
      }
    }
    output = builder_.EndTable(start);
    return true;
  }

 private:
  const reflection::Object& object_type(const reflection::Type& type) const {
    return *schema_.objects()->Get(type.index());
  }

  bool enum_value(const reflection::Type& type, FieldValue& value) const {
    const auto* name = std::get_if<std::string>(&value);
    if (type.index() < 0 || !name) {
      return true;
    }
    for (const auto* entry : *schema_.enums()->Get(type.index())->values()) {
      if (entry->name()->string_view() == *name) {
        value = entry->value();
        return true;
      }
    }
    return false;
  }

  static bool write_struct(const reflection::Object& object, const FieldReader& fields, const MessageView& source,
                           const std::string& prefix, uint8_t* output) {
    const auto* mapped = fields.field(prefix);
    const auto time_scale = mapped ? mapped->time_scale : 0;
    FieldValue seconds;
    FieldValue nanoseconds;
    if (time_scale) {
      const auto units_per_second = 1000000000 / time_scale;
      if (object.name()->string_view() == "foxglove.Time") {
        uint64_t ticks = 0;
        if (!field_numeric(fields.value(prefix), ticks)) {
          return false;
        }
        seconds = ticks / units_per_second;
        nanoseconds = (ticks % units_per_second) * time_scale;
      } else if (object.name()->string_view() == "foxglove.Duration") {
        int64_t ticks = 0;
        if (!field_numeric(fields.value(prefix), ticks)) {
          return false;
        }
        seconds = ticks / static_cast<int64_t>(units_per_second);
        nanoseconds = (ticks % static_cast<int64_t>(units_per_second)) * static_cast<int64_t>(time_scale);
      } else {
        return false;
      }
    } else if (source.is_array() || !std::holds_alternative<std::monostate>(source.value())) {
      return false;
    }
    for (const auto* field : *object.fields()) {
      const auto path = prefix + "." + field->name()->str();
      const auto child = source.member(field->name()->string_view());
      auto value = fields.field(path) ? fields.value(path) : child.value(true);
      if (time_scale) {
        value = field->name()->string_view() == "sec" ? seconds : nanoseconds;
      } else if (!child.valid() && !fields.field(path)) {
        value = flatbuffers::IsFloat(field->type()->base_type()) ? FieldValue(field->default_real())
                                                                 : FieldValue(field->default_integer());
      }
      if (!scalar_type(field->type()->base_type(), [&](auto scalar) {
            if (!field_numeric(value, scalar)) {
              return false;
            }
            flatbuffers::WriteScalar(output + field->offset(), scalar);
            return true;
          })) {
        return false;
      }
    }
    return true;
  }

  bool offset(const reflection::Type& type, const FieldReader& fields, const MessageView& source,
              const std::string& path, flatbuffers::uoffset_t& output) {
    if (type.base_type() == reflection::String) {
      const auto value = source.value();
      const auto* text = std::get_if<std::string>(&value);
      if (!text) {
        return false;
      }
      output = builder_.CreateString(*text).o;
      return true;
    }
    if (type.base_type() == reflection::Obj) {
      const auto scope = fields.field(path) ? fields.child(source) : fields;
      return table(object_type(type), scope, source, path, output);
    }
    if (type.base_type() != reflection::Vector) {
      return false;
    }
    const auto indices = fields.indices(path);
    const bool binary = type.element() == reflection::UByte && source.is_bytes();
    Bytes bytes;
    if (binary) {
      if (!source.read_bytes(bytes)) {
        return false;
      }
      if (indices.empty() && fields.field(path + "[]") == nullptr) {
        output = builder_.CreateVector(bytes.data(), bytes.size()).o;
        return true;
      }
    }
    const bool generated = !source.valid() && !indices.empty();
    auto size = source.size();
    if (binary) {
      size = bytes.size();
    } else if (generated) {
      size = indices.size();
    }
    if ((!source.is_array() && !generated && !binary) || (!indices.empty() && indices.back() >= size)) {
      return false;
    }
    if (flatbuffers::IsScalar(type.element())) {
      return scalar_type(type.element(), [&](auto scalar) {
        builder_.StartVector(size, sizeof(scalar), alignof(decltype(scalar)));
        for (size_t i = size; i > 0; --i) {
          const auto indexed = path + "[" + std::to_string(i - 1) + "]";
          const auto target = fields.field(indexed) ? indexed : path + "[]";
          const nlohmann::json byte = binary ? nlohmann::json(bytes.data()[i - 1]) : nlohmann::json{};
          const auto element = binary ? MessageView(byte) : source.at(i - 1);
          const auto scope = generated ? fields : fields.child(element, i - 1);
          auto value = fields.field(target) ? scope.value(target) : element.value(true);
          if (!enum_value(type, value) || !field_numeric(value, scalar)) {
            return false;
          }
          builder_.PushElement(scalar);
        }
        output = builder_.EndVector(size);
        return true;
      });
    }
    std::vector<flatbuffers::Offset<void>> offsets;
    offsets.reserve(size);
    for (size_t i = 0; i < size; ++i) {
      const auto indexed_path = path + "[" + std::to_string(i) + "]";
      const auto wildcard = path + "[]";
      auto target = path;
      if (fields.field(indexed_path) != nullptr || fields.has_descendant(indexed_path)) {
        target = indexed_path;
      } else if (fields.field(wildcard) != nullptr || fields.has_descendant(wildcard)) {
        target = wildcard;
      }
      auto item = source.at(i);
      auto scope = generated ? fields : fields.child(item, i);
      if (fields.field(indexed_path)) {
        item = scope.view(indexed_path);
        scope = scope.child(item);
      } else if (fields.field(wildcard)) {
        item = scope.view(wildcard);
        scope = scope.child(item);
      }
      flatbuffers::uoffset_t child = 0;
      if (type.element() == reflection::Obj) {
        if (!table(object_type(type), scope, item, target, child)) {
          return false;
        }
      } else if (type.element() == reflection::String) {
        const auto value = item.value();
        const auto* text = std::get_if<std::string>(&value);
        if (!text) {
          return false;
        }
        child = builder_.CreateString(*text).o;
      } else {
        return false;
      }
      offsets.emplace_back(child);
    }
    output = builder_.CreateVector(offsets).o;
    return true;
  }

  const reflection::Schema& schema_;
  flatbuffers::FlatBufferBuilder& builder_;
};

}  // namespace

bool write_flatbuffer_mapping(const reflection::Schema& schema, const FieldReader& fields,
                              flatbuffers::FlatBufferBuilder& builder) {
  if (!schema.root_table()) {
    return false;
  }
  flatbuffers::uoffset_t root = 0;
  if (!FlatbufferWriter(schema, builder).table(*schema.root_table(), fields, fields.view(""), {}, root)) {
    return false;
  }
  builder.Finish(flatbuffers::Offset<flatbuffers::Table>(root));
  return true;
}

}  // namespace webviz
}  // namespace vlink
