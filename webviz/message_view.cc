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

#include "./message_view.h"

#include <vlink/base/logger.h>

#include <charconv>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace vlink {
namespace webviz {

bool parse_field_path(std::string_view text, FieldPath& path, bool allow_wildcard) {
  path.clear();

  while (!text.empty()) {
    FieldStep step;
    const auto end = text.find_first_of(".[");
    step.name.assign(text.substr(0, end));

    if (step.name.empty() && text.front() != '[') {
      return false;
    }

    text.remove_prefix(end == std::string_view::npos ? text.size() : end);

    if (!text.empty() && text.front() == '[') {
      const auto close = text.find(']');

      if (close == std::string_view::npos) {
        return false;
      }
      if (allow_wildcard && close > 2 && text[1] == '0') {
        return false;
      }

      if (close != 1 || !allow_wildcard) {
        const auto result = std::from_chars(text.data() + 1, text.data() + close, step.index);
        if (result.ec != std::errc() || result.ptr != text.data() + close) {
          return false;
        }
      }

      step.indexed = true;
      text.remove_prefix(close + 1);
    }

    path.push_back(std::move(step));

    if (!text.empty()) {
      if (text.front() == '[') {
        continue;
      }
      if (text.front() != '.' || text.size() == 1 || (allow_wildcard && text[1] == '[')) {
        return false;
      }

      text.remove_prefix(1);
    }
  }

  return true;
}

double field_number(const FieldValue& value, double fallback) {
  return std::visit(
      [fallback](const auto& item) -> double {
        using T = std::decay_t<decltype(item)>;

        if constexpr (std::is_arithmetic_v<T>) {
          return static_cast<double>(item);
        } else if constexpr (std::is_same_v<T, std::string>) {
          double result = 0;
          std::istringstream stream(item);
          return stream >> result ? result : fallback;
        } else {
          return fallback;
        }
      },
      value);
}

int64_t field_integer(const FieldValue& value, int64_t fallback) {
  if (const auto* item = std::get_if<int64_t>(&value)) {
    return *item;
  }

  if (const auto* item = std::get_if<uint64_t>(&value)) {
    return *item <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ? static_cast<int64_t>(*item) : fallback;
  }

  const auto number = field_number(value, static_cast<double>(fallback));
  return std::isfinite(number) && number >= -0x1p63 && number < 0x1p63 ? static_cast<int64_t>(number) : fallback;
}

uint64_t field_unsigned(const FieldValue& value, uint64_t fallback) {
  if (const auto* item = std::get_if<uint64_t>(&value)) {
    return *item;
  }

  if (const auto* item = std::get_if<int64_t>(&value)) {
    return *item >= 0 ? static_cast<uint64_t>(*item) : fallback;
  }

  const auto number = field_number(value, static_cast<double>(fallback));
  return std::isfinite(number) && number >= 0 && number < 0x1p64 ? static_cast<uint64_t>(number) : fallback;
}

std::string field_text(const FieldValue& value) {
  return std::visit(
      [](const auto& item) -> std::string {
        using T = std::decay_t<decltype(item)>;

        if constexpr (std::is_same_v<T, std::string>) {
          return item;
        } else if constexpr (std::is_same_v<T, bool>) {
          return item ? "true" : "false";
        } else if constexpr (std::is_integral_v<T>) {
          return std::to_string(item);
        } else if constexpr (std::is_same_v<T, double>) {
          if (!std::isfinite(item)) {
            return {};
          }

          std::ostringstream stream;
          stream << std::setprecision(15) << item;
          return stream.str();
        } else {
          return {};
        }
      },
      value);
}

MessageView::MessageView(const google::protobuf::Message& message) : kind_(kProto), proto_(&message) {}

MessageView::MessageView(const nlohmann::json& message) : kind_(kJson), json_(&message) {}

MessageView::MessageView(const zerocopy::MessageParser& message) : kind_(kZeroCopy), zero_(&message) {}

MessageView::MessageView(const uint8_t* data, const reflection::Schema& schema)
    : kind_(kFlatbuffer),
      schema_(&schema),
      object_(schema.root_table()),
      data_(reinterpret_cast<const uint8_t*>(flatbuffers::GetAnyRoot(data))),
      base_(reflection::Obj) {}

std::string MessageView::text() const {
  if (kind_ == kProto) {
    const auto* message = proto_object();
    return message ? message->ShortDebugString() : std::string{};
  }
  return kind_ == kJson ? json_->dump() : std::string{};
}

bool MessageView::valid() const {
  if (kind_ == kProto && proto_field_ && !proto_field_->is_repeated()) {
    return !proto_field_->has_presence() || proto_->GetReflection()->HasField(*proto_, proto_field_);
  }

  if (kind_ == kZeroCopy) {
    if (zero_path_.empty()) {
      return zero_->valid();
    }
    zerocopy::MessageParser::Value value;
    if (zero_->value(zero_path_, value) || zero_->collection_size(zero_path_) > 0 ||
        !zero_->element_fields(zero_path_).empty()) {
      return true;
    }
    const auto bracket = zero_path_.find('[');
    const auto end = zero_path_.find(']');
    if (bracket != std::string::npos && end != std::string::npos) {
      size_t index = 0;
      const auto parsed = std::from_chars(zero_path_.data() + bracket + 1, zero_path_.data() + end, index);
      const std::string_view collection(zero_path_.data(), bracket);
      if (parsed.ec != std::errc() || index >= zero_->collection_size(collection)) {
        return false;
      }
      if (end + 1 == zero_path_.size()) {
        return true;
      }
      const auto suffix = zero_path_.substr(end + 2);
      for (const auto& field : zero_->element_fields(collection)) {
        if (field.name.size() > suffix.size() && field.name.compare(0, suffix.size(), suffix) == 0 &&
            (field.name[suffix.size()] == '.' || field.name[suffix.size()] == '[')) {
          return true;
        }
      }
    }
    for (const auto& field : zero_->fields()) {
      if (field.name.size() > zero_path_.size() && field.name.compare(0, zero_path_.size(), zero_path_) == 0 &&
          (field.name[zero_path_.size()] == '.' || field.name[zero_path_.size()] == '[')) {
        return true;
      }
    }
    return false;
  }
  return kind_ != kEmpty && (kind_ != kFlatbuffer || data_ != nullptr);
}

const google::protobuf::Message* MessageView::proto_object() const {
  if (!proto_field_) {
    return proto_;
  }

  if (proto_field_->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
    return nullptr;
  }

  const auto* reflection = proto_->GetReflection();

  if (proto_field_->is_repeated()) {
    return proto_index_ < 0 ? nullptr : &reflection->GetRepeatedMessage(*proto_, proto_field_, proto_index_);
  }

  return reflection->HasField(*proto_, proto_field_) ? &reflection->GetMessage(*proto_, proto_field_) : nullptr;
}

MessageView MessageView::member(std::string_view name) const {
  MessageView result;

  switch (kind_) {
    case kProto: {
      const auto* message = proto_object();

      if (!message) {
        return result;
      }

      const auto* field = message->GetDescriptor()->FindFieldByName(std::string(name));

      if (!field) {
        return result;
      }

      result.kind_ = kProto;
      result.proto_ = message;
      result.proto_field_ = field;
      return result;
    }
    case kJson: {
      if (json_->is_object()) {
        const auto iter = json_->find(name);

        if (iter != json_->end()) {
          return MessageView(*iter);
        }
      }

      return result;
    }
    case kZeroCopy:
      result.kind_ = kZeroCopy;
      result.zero_ = zero_;
      result.zero_path_ = zero_path_;

      if (!result.zero_path_.empty()) {
        result.zero_path_ += '.';
      }

      result.zero_path_.append(name);
      return result;
    case kFlatbuffer: {
      if (!data_ || base_ != reflection::Obj || !object_) {
        return result;
      }

      const auto* field = object_->fields()->LookupByKey(std::string(name).c_str());

      if (!field) {
        return result;
      }

      result.kind_ = kFlatbuffer;
      result.schema_ = schema_;
      result.fbs_field_ = field;
      const auto* type = field->type();
      result.base_ = type->base_type();
      result.element_ = type->element();
      result.fixed_size_ = type->fixed_length();

      if (type->index() >= 0 && (result.base_ == reflection::Obj || result.element_ == reflection::Obj)) {
        result.object_ = schema_->objects()->Get(static_cast<flatbuffers::uoffset_t>(type->index()));
      }

      result.data_ = object_->is_struct()
                         ? data_ + field->offset()
                         : reinterpret_cast<const flatbuffers::Table*>(data_)->GetAddressOf(field->offset());

      if (result.data_ && (result.base_ == reflection::String || result.base_ == reflection::Vector ||
                           (result.base_ == reflection::Obj && !result.object_->is_struct()))) {
        result.data_ += flatbuffers::ReadScalar<flatbuffers::uoffset_t>(result.data_);
      }

      return result;
    }
    case kEmpty:
      return result;
  }

  return result;
}

bool MessageView::is_bytes() const {
  switch (kind_) {
    case kProto:
      return proto_field_ != nullptr && (!proto_field_->is_repeated() || proto_index_ >= 0) &&
             proto_field_->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING;
    case kJson:
      return json_->is_string() || (json_->is_object() && json_->contains("base64"));
    case kFlatbuffer:
      return base_ == reflection::String ||
             (base_ == reflection::Vector && (element_ == reflection::Byte || element_ == reflection::UByte));
    case kZeroCopy:
      return std::holds_alternative<Bytes>(value());
    case kEmpty:
      return false;
  }
  return false;
}

bool MessageView::is_array() const {
  switch (kind_) {
    case kProto:
      return proto_field_ != nullptr && proto_field_->is_repeated() && proto_index_ < 0;
    case kJson:
      return json_->is_array();
    case kFlatbuffer:
      return base_ == reflection::Vector || base_ == reflection::Array;
    case kZeroCopy:
      return !zero_->element_fields(zero_path_).empty();
    case kEmpty:
      return false;
  }
  return false;
}

size_t MessageView::size() const {
  switch (kind_) {
    case kProto:
      return proto_field_ && proto_field_->is_repeated() && proto_index_ < 0
                 ? static_cast<size_t>(proto_->GetReflection()->FieldSize(*proto_, proto_field_))
                 : 0;
    case kJson:
      return json_->is_array() ? json_->size() : 0;
    case kZeroCopy:
      return zero_->collection_size(zero_path_);
    case kFlatbuffer:
      if (!data_) {
        return 0;
      }

      if (base_ == reflection::Array) {
        return fixed_size_;
      }

      return base_ == reflection::Vector ? flatbuffers::ReadScalar<flatbuffers::uoffset_t>(data_) : 0;
    case kEmpty:
      return 0;
  }

  return 0;
}

MessageView MessageView::at(size_t index) const {
  if VUNLIKELY (kind_ != kZeroCopy && index >= size()) {
    return {};
  }

  MessageView result;

  switch (kind_) {
    case kProto:
      result.kind_ = kProto;
      result.proto_ = proto_;
      result.proto_field_ = proto_field_;
      result.proto_index_ = static_cast<int>(index);
      break;
    case kJson:
      return MessageView((*json_)[index]);
    case kZeroCopy:
      result.kind_ = kZeroCopy;
      result.zero_ = zero_;
      result.zero_path_ = zero_path_ + "[" + std::to_string(index) + "]";
      break;
    case kFlatbuffer: {
      result.kind_ = kFlatbuffer;
      result.schema_ = schema_;
      result.object_ = object_;
      result.base_ = element_;
      const auto stride = element_ == reflection::Obj && object_->is_struct() ? static_cast<size_t>(object_->bytesize())
                                                                              : flatbuffers::GetTypeSize(element_);
      result.data_ = data_ + (base_ == reflection::Vector ? sizeof(flatbuffers::uoffset_t) : 0) + index * stride;

      if (element_ == reflection::String || (element_ == reflection::Obj && !object_->is_struct())) {
        result.data_ += flatbuffers::ReadScalar<flatbuffers::uoffset_t>(result.data_);
      }

      break;
    }
    case kEmpty:
      break;
  }

  return result;
}

MessageView MessageView::find(const FieldPath& path) const {
  MessageView result = *this;

  for (const auto& step : path) {
    if (!step.name.empty()) {
      result = result.member(step.name);
    }

    if (step.indexed) {
      result = result.at(step.index);
    }

    if (kind_ != kZeroCopy && !result.valid()) {
      break;
    }
  }

  return result;
}

MessageView MessageView::find(std::string_view path) const {
  FieldPath steps;
  return parse_field_path(path, steps) ? find(steps) : MessageView{};
}

FieldValue MessageView::fbs_value() const {
  switch (base_) {
    case reflection::Bool:
      return flatbuffers::ReadScalar<uint8_t>(data_) != 0;
    case reflection::Byte:
      return int64_t{flatbuffers::ReadScalar<int8_t>(data_)};
    case reflection::UByte:
      return uint64_t{flatbuffers::ReadScalar<uint8_t>(data_)};
    case reflection::Short:
      return int64_t{flatbuffers::ReadScalar<int16_t>(data_)};
    case reflection::UShort:
      return uint64_t{flatbuffers::ReadScalar<uint16_t>(data_)};
    case reflection::Int:
      return int64_t{flatbuffers::ReadScalar<int32_t>(data_)};
    case reflection::UInt:
      return uint64_t{flatbuffers::ReadScalar<uint32_t>(data_)};
    case reflection::Long:
      return flatbuffers::ReadScalar<int64_t>(data_);
    case reflection::ULong:
      return flatbuffers::ReadScalar<uint64_t>(data_);
    case reflection::Float:
      return double{flatbuffers::ReadScalar<float>(data_)};
    case reflection::Double:
      return flatbuffers::ReadScalar<double>(data_);
    case reflection::String:
      return reinterpret_cast<const flatbuffers::String*>(data_)->str();
    default:
      return {};
  }
}

FieldValue MessageView::value(bool schema_default) const {
  if (kind_ == kFlatbuffer) {
    if (data_) {
      return fbs_value();
    }

    if (schema_default && fbs_field_ && !fbs_field_->optional() && flatbuffers::IsScalar(base_)) {
      return flatbuffers::IsFloat(base_) ? FieldValue(fbs_field_->default_real())
                                         : FieldValue(fbs_field_->default_integer());
    }

    return {};
  }

  if (kind_ == kZeroCopy) {
    zerocopy::MessageParser::Value item;

    if (!zero_->value(zero_path_, item)) {
      return {};
    }

    return std::visit([](auto&& entry) -> FieldValue { return std::forward<decltype(entry)>(entry); }, std::move(item));
  }

  if (kind_ == kJson) {
    if (json_->is_number_unsigned()) {
      return json_->get<uint64_t>();
    }
    if (json_->is_number_integer()) {
      return json_->get<int64_t>();
    }
    if (json_->is_number_float()) {
      return json_->get<double>();
    }
    if (json_->is_boolean()) {
      return json_->get<bool>();
    }
    if (json_->is_string()) {
      return json_->get<std::string>();
    }
    return {};
  }

  if (kind_ != kProto || !proto_field_ || (proto_field_->is_repeated() && proto_index_ < 0)) {
    return {};
  }

  const auto* r = proto_->GetReflection();
  const auto* f = proto_field_;
  const auto& m = *proto_;
  const int i = proto_index_;
  const bool repeated = f->is_repeated();

  if (!repeated && f->has_presence() && !r->HasField(m, f) && !schema_default) {
    return {};
  }

  using F = google::protobuf::FieldDescriptor;

  switch (f->cpp_type()) {
    case F::CPPTYPE_INT32:
      return int64_t{repeated ? r->GetRepeatedInt32(m, f, i) : r->GetInt32(m, f)};
    case F::CPPTYPE_INT64:
      return repeated ? r->GetRepeatedInt64(m, f, i) : r->GetInt64(m, f);
    case F::CPPTYPE_UINT32:
      return uint64_t{repeated ? r->GetRepeatedUInt32(m, f, i) : r->GetUInt32(m, f)};
    case F::CPPTYPE_UINT64:
      return repeated ? r->GetRepeatedUInt64(m, f, i) : r->GetUInt64(m, f);
    case F::CPPTYPE_DOUBLE:
      return repeated ? r->GetRepeatedDouble(m, f, i) : r->GetDouble(m, f);
    case F::CPPTYPE_FLOAT:
      return double{repeated ? r->GetRepeatedFloat(m, f, i) : r->GetFloat(m, f)};
    case F::CPPTYPE_BOOL:
      return repeated ? r->GetRepeatedBool(m, f, i) : r->GetBool(m, f);
    case F::CPPTYPE_ENUM:
      return int64_t{repeated ? r->GetRepeatedEnumValue(m, f, i) : r->GetEnumValue(m, f)};
    case F::CPPTYPE_STRING:
      return repeated ? r->GetRepeatedString(m, f, i) : r->GetString(m, f);
    case F::CPPTYPE_MESSAGE:
      return {};
  }

  return {};
}

bool MessageView::read_bytes(Bytes& output) const {
  if (kind_ == kJson && json_->is_object() && json_->contains("base64")) {
    const auto& value = json_->at("base64");
    if (!value.is_string()) {
      return false;
    }
    const auto& text = value.get_ref<const std::string&>();
    output = Bytes::decode_from_base64(text);
    return text.empty() || !output.empty();
  }
  if (!is_bytes()) {
    return false;
  }
  output = bytes();
  return true;
}

Bytes MessageView::bytes() const {
  if (kind_ == kZeroCopy) {
    auto item = value();

    if (auto* data = std::get_if<Bytes>(&item)) {
      return std::move(*data);
    }

    return {};
  }

  if (kind_ == kFlatbuffer && data_) {
    if (base_ == reflection::Vector && (element_ == reflection::UByte || element_ == reflection::Byte)) {
      return Bytes::shallow_copy(data_ + sizeof(flatbuffers::uoffset_t), size());
    }

    if (base_ == reflection::String) {
      const auto* text = reinterpret_cast<const flatbuffers::String*>(data_);
      return Bytes::shallow_copy(text->Data(), text->size());
    }
  }

  if (kind_ == kProto && proto_field_ && (!proto_field_->is_repeated() || proto_index_ >= 0) &&
      proto_field_->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING) {
    std::string scratch;
    const auto* reflection = proto_->GetReflection();
    const auto& text = proto_field_->is_repeated()
                           ? reflection->GetRepeatedStringReference(*proto_, proto_field_, proto_index_, &scratch)
                           : reflection->GetStringReference(*proto_, proto_field_, &scratch);

    if (&text != &scratch) {
      return Bytes::shallow_copy(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    }

    auto result = Bytes::create(text.size());
    std::memcpy(result.data(), text.data(), text.size());
    return result;
  }

  if (kind_ == kJson && json_->is_string()) {
    const auto& text = json_->get_ref<const std::string&>();
    return Bytes::shallow_copy(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  }

  auto result = Bytes::create(size());

  for (size_t i = 0; i < size(); ++i) {
    const auto value = at(i).value();

    if (kind_ == kJson && ((!std::holds_alternative<int64_t>(value) && !std::holds_alternative<uint64_t>(value)) ||
                           field_integer(value, -1) < 0 || field_unsigned(value) > 255)) {
      return {};
    }

    result.data()[i] = std::holds_alternative<int64_t>(value) ? static_cast<uint8_t>(std::get<int64_t>(value))
                                                              : static_cast<uint8_t>(field_unsigned(value));
  }

  return result;
}

}  // namespace webviz
}  // namespace vlink
