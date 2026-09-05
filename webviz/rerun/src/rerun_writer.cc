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

#include "./rerun_writer.h"

#include <arrow/api.h>
#include <arrow/util/float16.h>
#include <vlink/base/logger.h>

#include <rerun/half.hpp>

#include "./rerun_schema.h"

namespace vlink {
namespace webviz {

namespace {

bool enum_value(std::string_view type, FieldValue& value) {
  const auto* text = std::get_if<std::string>(&value);
  uint64_t number = 0;
  const bool numeric = text == nullptr && field_numeric(value, number);
  bool enumerated = false;
  for (const auto& entry : rerun_enums()) {
    if (entry.type != type) {
      continue;
    }
    enumerated = true;
    if ((text != nullptr && entry.name == *text) || (numeric && entry.value == number)) {
      value = entry.value;
      return true;
    }
  }
  return text == nullptr && !enumerated;
}

bool valid_tensor(const arrow::Array& array) {
  const auto& tensor = static_cast<const arrow::StructArray&>(array);
  const auto& shapes = static_cast<const arrow::ListArray&>(*tensor.GetFieldByName("shape"));
  const auto& dimensions = static_cast<const arrow::UInt64Array&>(*shapes.values());
  const auto& names = static_cast<const arrow::ListArray&>(*tensor.GetFieldByName("names"));
  const auto& buffers = static_cast<const arrow::DenseUnionArray&>(*tensor.GetFieldByName("buffer"));
  for (int64_t row = 0; row < array.length(); ++row) {
    const auto rank = shapes.value_length(row);
    const auto& buffer = buffers.field(buffers.child_id(row));
    if (buffer->type_id() != arrow::Type::LIST || (!names.IsNull(row) && names.value_length(row) != rank)) {
      return false;
    }
    const auto count =
        static_cast<uint64_t>(static_cast<const arrow::ListArray&>(*buffer).value_length(buffers.value_offset(row)));
    if (rank == 0) {
      if (count > 1) {
        return false;
      }
      continue;
    }
    uint64_t product = 1;
    const auto offset = shapes.value_offset(row);
    for (int64_t i = 0; i < rank; ++i) {
      if (dimensions.Value(offset + i) == 0) {
        product = 0;
        break;
      }
    }
    for (int64_t i = 0; i < rank && product != 0; ++i) {
      const auto dimension = dimensions.Value(offset + i);
      if (dimension > count / product) {
        return false;
      }
      product *= dimension;
    }
    if (product != count) {
      return false;
    }
  }
  return true;
}

bool valid_image(const arrow::Array& format_array, const arrow::Array& buffer_array) {
  if (format_array.length() == 0 || buffer_array.length() == 0) {
    return true;
  }
  const auto& format = static_cast<const arrow::StructArray&>(format_array);
  const auto width = static_cast<const arrow::UInt32Array&>(*format.GetFieldByName("width")).Value(0);
  const auto height = static_cast<const arrow::UInt32Array&>(*format.GetFieldByName("height")).Value(0);
  const auto count = static_cast<uint64_t>(static_cast<const arrow::ListArray&>(buffer_array).value_length(0));
  const uint64_t pixels = uint64_t{width} * height;
  if (pixels > count) {
    return false;
  }
  ::rerun::encodings::ImageFormat sample;
  sample.width = 2;
  sample.height = 1;
  const auto set_enum = [&](const char* name, auto& target) {
    const auto& values = static_cast<const arrow::UInt8Array&>(*format.GetFieldByName(name));
    if (!values.IsNull(0)) {
      using Enum = typename std::decay_t<decltype(target)>::value_type;
      target = static_cast<Enum>(values.Value(0));
    }
  };
  set_enum("pixel_format", sample.pixel_format);
  set_enum("color_model", sample.color_model);
  set_enum("channel_datatype", sample.channel_datatype);
  // Two pixels preserve the SDK's chroma subsampling ratios without size_t overflow.
  const auto pair_bytes = sample.num_bytes();
  return (pixels / 2) * pair_bytes + (pixels % 2) * pair_bytes / 2 == count;
}

template <typename Builder>
bool append_number(arrow::ArrayBuilder& builder, const FieldValue& value) {
  typename Builder::value_type number{};
  return field_numeric(value, number) && static_cast<Builder&>(builder).Append(number).ok();
}

nlohmann::json binary_value(arrow::Type::type type, const uint8_t* data) {
  switch (type) {
    case arrow::Type::INT8:
      return flatbuffers::ReadScalar<int8_t>(data);
    case arrow::Type::UINT8:
      return *data;
    case arrow::Type::INT16:
      return flatbuffers::ReadScalar<int16_t>(data);
    case arrow::Type::UINT16:
      return flatbuffers::ReadScalar<uint16_t>(data);
    case arrow::Type::INT32:
      return flatbuffers::ReadScalar<int32_t>(data);
    case arrow::Type::UINT32:
      return flatbuffers::ReadScalar<uint32_t>(data);
    case arrow::Type::INT64:
      return flatbuffers::ReadScalar<int64_t>(data);
    case arrow::Type::UINT64:
      return flatbuffers::ReadScalar<uint64_t>(data);
    case arrow::Type::FLOAT:
      return flatbuffers::ReadScalar<float>(data);
    case arrow::Type::DOUBLE:
      return flatbuffers::ReadScalar<double>(data);
    case arrow::Type::HALF_FLOAT:
      return arrow::util::Float16::FromBits(flatbuffers::ReadScalar<uint16_t>(data)).ToFloat();
    default:
      return nullptr;
  }
}

template <typename Builder>
bool append_binary(arrow::ArrayBuilder& builder, const Bytes& bytes) {
  using Value = typename Builder::value_type;
  if (bytes.size() % sizeof(Value) != 0) {
    return false;
  }
  auto& values = static_cast<Builder&>(builder);
  for (size_t offset = 0; offset < bytes.size(); offset += sizeof(Value)) {
    if (!values.Append(flatbuffers::ReadScalar<Value>(bytes.data() + offset)).ok()) {
      return false;
    }
  }
  return true;
}

bool append_binary_values(arrow::ArrayBuilder& builder, const Bytes& bytes) {
  switch (builder.type()->id()) {
    case arrow::Type::UINT8:
      return static_cast<arrow::UInt8Builder&>(builder).AppendValues(bytes.data(), bytes.size()).ok();
    case arrow::Type::INT8:
      return append_binary<arrow::Int8Builder>(builder, bytes);
    case arrow::Type::INT16:
      return append_binary<arrow::Int16Builder>(builder, bytes);
    case arrow::Type::UINT16:
      return append_binary<arrow::UInt16Builder>(builder, bytes);
    case arrow::Type::INT32:
      return append_binary<arrow::Int32Builder>(builder, bytes);
    case arrow::Type::UINT32:
      return append_binary<arrow::UInt32Builder>(builder, bytes);
    case arrow::Type::INT64:
      return append_binary<arrow::Int64Builder>(builder, bytes);
    case arrow::Type::UINT64:
      return append_binary<arrow::UInt64Builder>(builder, bytes);
    case arrow::Type::FLOAT:
      return append_binary<arrow::FloatBuilder>(builder, bytes);
    case arrow::Type::DOUBLE:
      return append_binary<arrow::DoubleBuilder>(builder, bytes);
    case arrow::Type::HALF_FLOAT:
      return append_binary<arrow::HalfFloatBuilder>(builder, bytes);
    default:
      return false;
  }
}

bool append_value(arrow::ArrayBuilder& builder, const FieldReader& fields, const MessageView& source,
                  const std::string& path, std::string_view component, bool nullable) {
  const auto* mapped = fields.field(path);
  const auto type = builder.type();
  FieldValue value;
  if (mapped && mapped->expression) {
    value = fields.value(path);
  } else if ((type->id() != arrow::Type::STRUCT && type->id() != arrow::Type::LIST &&
              type->id() != arrow::Type::FIXED_SIZE_LIST && type->id() != arrow::Type::DENSE_UNION) ||
             (component == "ViewCoordinates" && source.is_bytes())) {
    value = source.value(true);
  }
  if (source.is_null() && std::holds_alternative<std::monostate>(value) && !fields.has_descendant(path)) {
    return nullable && builder.AppendNull().ok();
  }
  if (type->id() == arrow::Type::STRUCT) {
    if (source.is_array() || source.is_bytes() || !std::holds_alternative<std::monostate>(source.value())) {
      return false;
    }
    auto& structure = static_cast<arrow::StructBuilder&>(builder);
    const auto& schema = static_cast<const arrow::StructType&>(*type);
    for (int i = 0; i < schema.num_fields(); ++i) {
      const auto& field = schema.field(i);
      const auto target = path + "." + field->name();
      const auto child = fields.field(target) ? fields.view(target) : source.member(field->name());
      auto child_component = component;
      if (component == "ImageFormat") {
        if (field->name() == "pixel_format") {
          child_component = "PixelFormat";
        } else if (field->name() == "color_model") {
          child_component = "ColorModel";
        } else if (field->name() == "channel_datatype") {
          child_component = "ChannelDatatype";
        }
      }
      if (!append_value(*structure.field_builder(i), fields, child, target, child_component, field->nullable())) {
        return false;
      }
    }
    return structure.Append().ok();
  }
  if (type->id() == arrow::Type::LIST || type->id() == arrow::Type::FIXED_SIZE_LIST) {
    auto* values = type->id() == arrow::Type::LIST ? static_cast<arrow::ListBuilder&>(builder).value_builder()
                                                   : static_cast<arrow::FixedSizeListBuilder&>(builder).value_builder();
    const auto& schema = static_cast<const arrow::BaseListType&>(*type);
    if (component == "ViewCoordinates" && std::holds_alternative<std::string>(value)) {
      const auto& text = std::get<std::string>(value);
      if (text.size() != 3 || !static_cast<arrow::FixedSizeListBuilder&>(builder).Append().ok()) {
        return false;
      }
      for (const char direction : text) {
        const auto entry = std::find_if(rerun_enums().begin(), rerun_enums().end(), [&](const RerunEnum& value) {
          return value.type == "ViewDir" && value.name.front() == direction;
        });
        if (entry == rerun_enums().end() ||
            !static_cast<arrow::UInt8Builder*>(values)->Append(static_cast<uint8_t>(entry->value)).ok()) {
          return false;
        }
      }
      return true;
    }
    const auto indices = fields.indices(path);
    const bool generated = !source.is_array() && !source.is_bytes() && !indices.empty();
    size_t count = generated ? indices.size() : source.size();
    Bytes bytes;
    const bool binary = source.is_bytes();
    if (binary) {
      const auto width = values->type()->byte_width();
      if (width <= 0 || !source.read_bytes(bytes) || bytes.size() % width != 0) {
        return false;
      }
      count = bytes.size() / width;
    }
    if ((!source.is_array() && !generated && !binary) || (!indices.empty() && indices.back() >= count)) {
      return false;
    }
    if (type->id() == arrow::Type::FIXED_SIZE_LIST &&
        count != static_cast<size_t>(static_cast<const arrow::FixedSizeListType&>(*type).list_size())) {
      return false;
    }
    if (!(type->id() == arrow::Type::LIST ? static_cast<arrow::ListBuilder&>(builder).Append()
                                          : static_cast<arrow::FixedSizeListBuilder&>(builder).Append())
             .ok()) {
      return false;
    }
    if (binary && indices.empty() && fields.field(path + "[]") == nullptr) {
      return append_binary_values(*values, bytes);
    }
    for (size_t i = 0; i < count; ++i) {
      const auto indexed = path + "[" + std::to_string(i) + "]";
      const auto wildcard = path + "[]";
      auto target = path;
      if (fields.field(indexed) != nullptr || fields.has_descendant(indexed)) {
        target = indexed;
      } else if (fields.field(wildcard) != nullptr || fields.has_descendant(wildcard)) {
        target = wildcard;
      }
      if (binary && target == path) {
        const auto width = values->type()->byte_width();
        const auto element = Bytes::shallow_copy(bytes.data() + i * width, width);
        if (!append_binary_values(*values, element)) {
          return false;
        }
        continue;
      }
      const auto number = binary ? binary_value(values->type()->id(), bytes.data() + i * values->type()->byte_width())
                                 : nlohmann::json{};
      auto child = binary ? MessageView(number) : source.at(i);
      auto scope = generated ? fields : fields.child(child, i);
      if (fields.field(indexed)) {
        child = scope.view(indexed);
      } else if (fields.field(wildcard)) {
        child = scope.view(wildcard);
      }
      if (!append_value(*values, scope, child, target, component == "ViewCoordinates" ? "ViewDir" : component,
                        schema.value_field()->nullable())) {
        return false;
      }
    }
    return true;
  }
  if (type->id() == arrow::Type::DENSE_UNION) {
    auto& target = static_cast<arrow::DenseUnionBuilder&>(builder);
    const auto& schema = static_cast<const arrow::DenseUnionType&>(*type);
    int selected = -1;
    MessageView value;
    std::string selected_path;
    for (int i = 0; i < schema.num_fields(); ++i) {
      const auto field_path = path + "." + schema.field(i)->name();
      const auto child = fields.field(field_path) ? fields.view(field_path) : source.member(schema.field(i)->name());
      if (child.valid() || fields.has_descendant(field_path)) {
        if (selected >= 0) {
          return false;
        }
        selected = i;
        value = child;
        selected_path = field_path;
      }
    }
    return selected >= 0 && target.Append(schema.type_codes()[selected]).ok() &&
           append_value(*target.child_builder(selected), fields, value, selected_path, component,
                        schema.field(selected)->nullable());
  }
  if (type->id() == arrow::Type::STRING) {
    const auto* text = std::get_if<std::string>(&value);
    return text != nullptr && static_cast<arrow::StringBuilder&>(builder).Append(*text).ok();
  }
  if (component == "Color" && source.is_array()) {
    if (source.size() != 3 && source.size() != 4) {
      return false;
    }
    uint32_t color = 0;
    for (size_t i = 0; i < 4; ++i) {
      uint8_t channel = 255;
      if (i < source.size() && !field_numeric(source.at(i).value(true), channel)) {
        return false;
      }
      color = (color << 8) | channel;
    }
    value = uint64_t{color};
  }
  if (!enum_value(component, value)) {
    return false;
  }
  switch (type->id()) {
    case arrow::Type::BOOL:
      return append_number<arrow::BooleanBuilder>(builder, value);
    case arrow::Type::INT8:
      return append_number<arrow::Int8Builder>(builder, value);
    case arrow::Type::UINT8:
      return append_number<arrow::UInt8Builder>(builder, value);
    case arrow::Type::INT16:
      return append_number<arrow::Int16Builder>(builder, value);
    case arrow::Type::UINT16:
      return append_number<arrow::UInt16Builder>(builder, value);
    case arrow::Type::INT32:
      return append_number<arrow::Int32Builder>(builder, value);
    case arrow::Type::UINT32:
      return append_number<arrow::UInt32Builder>(builder, value);
    case arrow::Type::INT64:
      return append_number<arrow::Int64Builder>(builder, value);
    case arrow::Type::UINT64:
      return append_number<arrow::UInt64Builder>(builder, value);
    case arrow::Type::FLOAT:
      return append_number<arrow::FloatBuilder>(builder, value);
    case arrow::Type::DOUBLE:
      return append_number<arrow::DoubleBuilder>(builder, value);
    case arrow::Type::HALF_FLOAT: {
      float number = 0;
      return field_numeric(value, number) &&
             static_cast<arrow::HalfFloatBuilder&>(builder).Append(::rerun::half::from_float(number).f16).ok();
    }
    case arrow::Type::NA:
      return false;
    default:
      return false;
  }
}

}  // namespace

bool validate_rerun_mapping(const MessageMapping& mapping) {
  if (!mapping.converter.empty()) {
    return mapping.fields.empty() && !mapping.is_static &&
           (mapping.converter == "send_time" || !native_ser(mapping.converter).empty());
  }
  const auto first = std::find_if(rerun_fields().begin(), rerun_fields().end(),
                                  [&](const RerunField& field) { return field.archetype == mapping.target; });
  if (first == rerun_fields().end()) {
    return false;
  }
  for (const auto& field : mapping.fields) {
    FieldPath path;
    if (field.time_scale || !parse_field_path(field.target, path, true) || path.empty()) {
      return false;
    }
    const auto found = std::find_if(first, rerun_fields().end(), [&](const RerunField& item) {
      return item.archetype == mapping.target && item.name == path.front().name;
    });
    if (found == rerun_fields().end()) {
      return false;
    }
    auto type = found->type();
    bool batch = found->batch;
    for (size_t i = 0; i < path.size(); ++i) {
      const auto& step = path[i];
      if (i > 0 && !step.name.empty()) {
        if (batch || (type->id() != arrow::Type::STRUCT && type->id() != arrow::Type::DENSE_UNION)) {
          return false;
        }
        const auto member = std::find_if(type->fields().begin(), type->fields().end(),
                                         [&](const auto& item) { return item->name() == step.name; });
        if (member == type->fields().end()) {
          return false;
        }
        type = (*member)->type();
      }
      if (step.indexed) {
        if (batch) {
          batch = false;
        } else if (type->id() == arrow::Type::LIST || type->id() == arrow::Type::FIXED_SIZE_LIST) {
          if (type->id() == arrow::Type::FIXED_SIZE_LIST &&
              step.index >= static_cast<size_t>(static_cast<const arrow::FixedSizeListType&>(*type).list_size())) {
            return false;
          }
          type = static_cast<const arrow::BaseListType&>(*type).value_type();
        } else {
          return false;
        }
      }
    }
    if (field.expression && (type->id() == arrow::Type::STRUCT || type->id() == arrow::Type::LIST ||
                             type->id() == arrow::Type::FIXED_SIZE_LIST || type->id() == arrow::Type::DENSE_UNION ||
                             type->id() == arrow::Type::STRING || type->id() == arrow::Type::NA)) {
      return false;
    }
  }
  return true;
}

bool write_rerun(::rerun::RecordingStream& recording, const std::string& path, std::string_view archetype,
                 const FieldReader& fields, bool is_static) {
  std::vector<::rerun::ComponentBatch> batches;
  std::shared_ptr<arrow::Array> image_format;
  std::shared_ptr<arrow::Array> image_buffer;
  for (const auto& field : rerun_fields()) {
    if (field.archetype != archetype) {
      continue;
    }
    const auto source = fields.view(field.name);
    const auto* mapped = fields.field(field.name);
    if (!source.valid() && !(mapped && mapped->expression) && !fields.has_descendant(field.name)) {
      continue;
    }
    std::unique_ptr<arrow::ArrayBuilder> builder;
    if (!arrow::MakeBuilder(arrow::default_memory_pool(), field.type(), &builder).ok()) {
      return false;
    }
    const auto target = std::string(field.name);
    bool success = true;
    if (!source.is_null() || (mapped && mapped->expression) || fields.has_descendant(field.name)) {
      if (field.batch) {
        const auto indices = fields.indices(target);
        const bool generated = !source.valid() && !indices.empty();
        if (source.is_array() || generated) {
          const auto count = generated ? indices.size() : source.size();
          success = indices.empty() || indices.back() < count;
          for (size_t i = 0; i < count && success; ++i) {
            const auto indexed = target + "[" + std::to_string(i) + "]";
            const auto wildcard = target + "[]";
            auto item_path = target;
            if (fields.field(indexed) != nullptr || fields.has_descendant(indexed)) {
              item_path = indexed;
            } else if (fields.field(wildcard) != nullptr || fields.has_descendant(wildcard)) {
              item_path = wildcard;
            }
            auto value = source.at(i);
            auto scope = generated ? fields : fields.child(value, i);
            if (fields.field(indexed)) {
              value = scope.view(indexed);
            } else if (fields.field(wildcard)) {
              value = scope.view(wildcard);
            }
            success = append_value(*builder, scope, value, item_path, field.component, archetype == "StateChange");
          }
        } else {
          success = append_value(*builder, fields, source, target, field.component, false);
        }
      } else {
        const auto scope = mapped && !mapped->expression ? fields.child(source) : fields;
        success = append_value(*builder, scope, source, target, field.component, false);
      }
    }
    std::shared_ptr<arrow::Array> array;
    if (!success || !builder->Finish(&array).ok() || !array->ValidateFull().ok() ||
        (field.component == "TensorData" && !valid_tensor(*array))) {
      MLOG_W("Invalid Rerun field: archetype={} field={}", archetype, field.name);
      return false;
    }
    if (field.component == "ImageFormat") {
      image_format = array;
    } else if (field.component == "ImageBuffer") {
      image_buffer = array;
    }
    auto batch = ::rerun::ComponentBatch::from_arrow_array(std::move(array), *field.descriptor);
    if (!batch.is_ok()) {
      return false;
    }
    batches.push_back(std::move(batch.value));
  }
  if (image_format && image_buffer && !valid_image(*image_format, *image_buffer)) {
    MLOG_W("Rerun image format does not match its buffer: archetype={}", archetype);
    return false;
  }
  const bool properties = archetype == "RecordingInfo";
  return !batches.empty() && recording
                                 .try_log_serialized_batches(properties ? "__properties/" : path,
                                                             properties || is_static, std::move(batches))
                                 .is_ok();
}

}  // namespace webviz
}  // namespace vlink
