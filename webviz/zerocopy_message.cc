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

#include "./zerocopy_message.h"

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <vlink/base/quantize.h>
#include <vlink/zerocopy/message_parser.h>
#include <vlink/zerocopy/point_cloud.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "./webviz_common.h"

namespace vlink {

namespace webviz {

using Descriptor = google::protobuf::Descriptor;
using FieldDescriptor = google::protobuf::FieldDescriptor;
using FieldProto = google::protobuf::FieldDescriptorProto;
using Message = google::protobuf::Message;
using MessageProto = google::protobuf::DescriptorProto;
using Field = zerocopy::MessageParser::Field;
using ValueType = zerocopy::MessageParser::ValueType;

template <typename T>
static T read_point_value(const uint8_t* data) noexcept {
  T value{};
  std::memcpy(&value, data, sizeof(value));
  return value;
}

PointCloudView::PointCloudView(const zerocopy::MessageParser& parser) {
  if (parser.type() != zerocopy::MessageParser::Type::kPointCloud) return;

  zerocopy::MessageParser::Value data_value;
  zerocopy::MessageParser::Value size_value;
  zerocopy::MessageParser::Value pack_size_value;
  double extent = 0.0;
  if (!parser.value("size", size_value) || !parser.value("pack_size", pack_size_value) ||
      !parser.numeric("extent", extent) || !parser.value("data", data_value))
    return;

  const auto* bytes = std::get_if<Bytes>(&data_value);
  const auto* size_ptr = std::get_if<uint64_t>(&size_value);
  const auto* pack_size_ptr = std::get_if<uint64_t>(&pack_size_value);
  if (size_ptr == nullptr || pack_size_ptr == nullptr || extent < 0.0 ||
      extent > static_cast<double>(std::numeric_limits<uint16_t>::max()))
    return;
  const uint64_t size = *size_ptr;
  const uint64_t pack_size = *pack_size_ptr;
  extent_ = static_cast<uint16_t>(extent);
  if (bytes == nullptr || pack_size == 0 || size > std::numeric_limits<size_t>::max() / pack_size ||
      size * pack_size > bytes->size())
    return;

  size_ = static_cast<size_t>(size);
  pack_size_ = static_cast<size_t>(pack_size);
  data_ = Bytes::shallow_copy(bytes->data(), bytes->size());
  uint16_t offset = 0;
  const auto source_fields = parser.element_fields("points");
  fields_.reserve(source_fields.size());
  for (size_t index = 0; index < source_fields.size(); ++index) {
    const auto& field = source_fields[index];
    if (field.storage_size == 0 || static_cast<size_t>(offset) + field.storage_size > pack_size_) {
      fields_.clear();
      return;
    }
    fields_.push_back({field, offset, index});
    offset = static_cast<uint16_t>(offset + field.storage_size);
  }
  valid_ = !fields_.empty();
}

PointCloudView::PointCloudView(const zerocopy::PointCloud& point_cloud) {
  size_ = point_cloud.size();
  pack_size_ = point_cloud.pack_size();
  extent_ = point_cloud.get_extent();
  if (pack_size_ == 0 || size_ > std::numeric_limits<size_t>::max() / pack_size_) return;
  data_ = Bytes::shallow_copy(point_cloud.get_internal_data(), size_ * pack_size_);

  zerocopy::PointCloud::KeyList keys;
  const auto key_map = point_cloud.get_key_map(&keys);
  (void)key_map;
  fields_.reserve(keys.size());
  uint16_t offset = 0;
  using PC = zerocopy::PointCloud;
  for (size_t index = 0; index < keys.size(); ++index) {
    const auto& key = keys[index];
    if (key.size == 0 || static_cast<size_t>(offset) + key.size > pack_size_) {
      fields_.clear();
      return;
    }
    auto value_type = ValueType::kDouble;
    if (key.type == PC::kInt8Type || key.type == PC::kInt16Type || key.type == PC::kInt32Type ||
        key.type == PC::kInt64Type)
      value_type = ValueType::kInt64;
    else if (key.type == PC::kBoolType || key.type == PC::kUint8Type || key.type == PC::kUint16Type ||
             key.type == PC::kUint32Type || key.type == PC::kUint64Type)
      value_type = ValueType::kUInt64;
    zerocopy::MessageParser::Field field{key.name, value_type, key.type, key.size};
    field.is_bool = key.type == PC::kBoolType;
    fields_.push_back({std::move(field), offset, index});
    offset = static_cast<uint16_t>(offset + key.size);
  }
  valid_ = !fields_.empty() && (size_ == 0 || data_.data() != nullptr);
}

bool PointCloudView::valid() const noexcept { return valid_; }
size_t PointCloudView::size() const noexcept { return size_; }
size_t PointCloudView::pack_size() const noexcept { return pack_size_; }
const Bytes& PointCloudView::data() const noexcept { return data_; }
const std::vector<PointCloudFieldView>& PointCloudView::fields() const noexcept { return fields_; }

const PointCloudFieldView* PointCloudView::find(std::string_view name) const noexcept {
  const auto iter =
      std::find_if(fields_.begin(), fields_.end(), [name](const auto& item) { return item.field.name == name; });
  return iter == fields_.end() ? nullptr : &*iter;
}

bool PointCloudView::value(size_t point, const PointCloudFieldView& item,
                           zerocopy::MessageParser::Value& out) const noexcept {
  if (!valid_ || point >= size_ || static_cast<size_t>(item.offset) + item.field.storage_size > pack_size_)
    return false;
  const uint8_t* source = data_.data() + point * pack_size_ + item.offset;
  if (extent_ != 0 && item.index < 3) {
    out = Quantize::decode<double>(extent_, read_point_value<int16_t>(source));
    return true;
  }

  using PC = zerocopy::PointCloud;
  switch (item.field.native_type) {
    case PC::kBoolType:
      out = static_cast<uint64_t>(read_point_value<uint8_t>(source) != 0);
      return true;
    case PC::kInt8Type:
      out = static_cast<int64_t>(read_point_value<int8_t>(source));
      return true;
    case PC::kUint8Type:
      out = static_cast<uint64_t>(read_point_value<uint8_t>(source));
      return true;
    case PC::kInt16Type:
      out = static_cast<int64_t>(read_point_value<int16_t>(source));
      return true;
    case PC::kUint16Type:
      out = static_cast<uint64_t>(read_point_value<uint16_t>(source));
      return true;
    case PC::kInt32Type:
      out = static_cast<int64_t>(read_point_value<int32_t>(source));
      return true;
    case PC::kUint32Type:
      out = static_cast<uint64_t>(read_point_value<uint32_t>(source));
      return true;
    case PC::kInt64Type:
      out = read_point_value<int64_t>(source);
      return true;
    case PC::kUint64Type:
      out = read_point_value<uint64_t>(source);
      return true;
    case PC::kFloatType:
      out = static_cast<double>(read_point_value<float>(source));
      return true;
    case PC::kDoubleType:
      out = read_point_value<double>(source);
      return true;
    case PC::kUnknownType:
      break;
    default:
      return false;
  }
  switch (item.field.storage_size) {
    case sizeof(uint8_t):
      out = static_cast<uint64_t>(read_point_value<uint8_t>(source));
      return true;
    case sizeof(int16_t):
      out = static_cast<int64_t>(read_point_value<int16_t>(source));
      return true;
    case sizeof(float):
      out = static_cast<double>(read_point_value<float>(source));
      return true;
    case sizeof(double):
      out = read_point_value<double>(source);
      return true;
    default:
      return false;
  }
}

bool PointCloudView::numeric(size_t point, const PointCloudFieldView& field, double& out) const noexcept {
  zerocopy::MessageParser::Value value;
  if (!this->value(point, field, value)) return false;
  if (const auto* number = std::get_if<double>(&value))
    out = *number;
  else if (const auto* integer = std::get_if<int64_t>(&value))
    out = static_cast<double>(*integer);
  else if (const auto* integer = std::get_if<uint64_t>(&value))
    out = static_cast<double>(*integer);
  else
    return false;
  return true;
}

struct DynamicStore final {
  google::protobuf::DescriptorPool pool;
  google::protobuf::DynamicMessageFactory factory{&pool};
  std::unordered_map<std::string, const Descriptor*> descriptors;
};

static DynamicStore& dynamic_store() {
  static DynamicStore store;
  return store;
}

static std::mutex& dynamic_store_mutex() {
  static std::mutex mutex;
  return mutex;
}

static void add_field(MessageProto& message, std::string name, int number, FieldProto::Type type,
                      FieldProto::Label label = FieldProto::LABEL_OPTIONAL, std::string type_name = {}) {
  auto* field = message.add_field();
  field->set_name(std::move(name));
  field->set_number(number);
  field->set_type(type);
  field->set_label(label);

  if (!type_name.empty()) {
    field->set_type_name(std::move(type_name));
  }
}

static void add_header(MessageProto& root, std::string_view package) {
  auto* header = root.add_nested_type();
  header->set_name("Header");
  add_field(*header, "frame_id", 1, FieldProto::TYPE_STRING);
  add_field(*header, "seq", 2, FieldProto::TYPE_UINT32);
  add_field(*header, "reserved", 3, FieldProto::TYPE_UINT32);
  add_field(*header, "time_meas", 4, FieldProto::TYPE_UINT64);
  add_field(*header, "time_pub", 5, FieldProto::TYPE_UINT64);
  add_field(root, "header", 1, FieldProto::TYPE_MESSAGE, FieldProto::LABEL_OPTIONAL,
            "." + std::string(package) + ".Root.Header");
}

static FieldProto::Type protobuf_type(ValueType type) {
  switch (type) {
    case ValueType::kInt64:
      return FieldProto::TYPE_INT64;
    case ValueType::kUInt64:
      return FieldProto::TYPE_UINT64;
    case ValueType::kString:
      return FieldProto::TYPE_STRING;
    case ValueType::kBytes:
      return FieldProto::TYPE_BYTES;
    case ValueType::kDouble:
    default:
      return FieldProto::TYPE_DOUBLE;
  }
}

static std::string sanitize_identifier(std::string value) {
  for (char& ch : value) {
    if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') {
      ch = '_';
    }
  }

  if (value.empty() || std::isdigit(static_cast<unsigned char>(value.front()))) {
    value.insert(value.begin(), '_');
  }

  return value;
}

static std::string unique_field_name(const std::string& raw, std::unordered_set<std::string>& used) {
  const std::string base = sanitize_identifier(raw);
  std::string name = base;

  for (size_t suffix = 2; used.count(name) != 0; ++suffix) {
    name = base + "_" + std::to_string(suffix);
  }

  used.insert(name);
  return name;
}

static const FieldDescriptor* field(const Message& message, std::string_view name) {
  return message.GetDescriptor()->FindFieldByName(std::string(name));
}

static void set_uint(Message& message, std::string_view name, uint64_t value) {
  const auto* target = field(message, name);

  if VUNLIKELY (target == nullptr) {
    return;
  }

  const auto* reflection = message.GetReflection();

  if (target->type() == FieldDescriptor::TYPE_UINT32) {
    reflection->SetUInt32(&message, target, static_cast<uint32_t>(value));
  } else {
    reflection->SetUInt64(&message, target, value);
  }
}

static void set_int(Message& message, std::string_view name, int64_t value) {
  const auto* target = field(message, name);

  if VUNLIKELY (target == nullptr) {
    return;
  }

  message.GetReflection()->SetInt64(&message, target, value);
}

static void set_double(Message& message, std::string_view name, double value) {
  const auto* target = field(message, name);

  if VUNLIKELY (target == nullptr) {
    return;
  }

  message.GetReflection()->SetDouble(&message, target, value);
}

static void set_bool(Message& message, std::string_view name, bool value) {
  const auto* target = field(message, name);

  if VUNLIKELY (target == nullptr) {
    return;
  }

  message.GetReflection()->SetBool(&message, target, value);
}

static void set_string(Message& message, std::string_view name, std::string_view value) {
  const auto* target = field(message, name);

  if VUNLIKELY (target == nullptr) {
    return;
  }

  message.GetReflection()->SetString(&message, target, std::string(value));
}

static void set_bytes(Message& message, std::string_view name, const uint8_t* data, size_t size) {
  if VUNLIKELY (data == nullptr && size != 0) {
    return;
  }

  if (size == 0) {
    set_string(message, name, {});
    return;
  }

  set_string(message, name, std::string_view(reinterpret_cast<const char*>(data), size));
}

static void fill_header(Message& root, const zerocopy::MessageParser& parser) {
  const auto* target = field(root, "header");

  if VUNLIKELY (target == nullptr) {
    return;
  }

  auto* header = root.GetReflection()->MutableMessage(&root, target);
  zerocopy::MessageParser::Value value;

  if VLIKELY (parser.value("header.frame_id", value)) {
    set_string(*header, "frame_id", std::get<std::string>(value));
  }

  if VLIKELY (parser.value("header.seq", value)) {
    set_uint(*header, "seq", std::get<uint64_t>(value));
  }

  if VLIKELY (parser.value("header.reserved", value)) {
    set_uint(*header, "reserved", std::get<uint64_t>(value));
  }

  if VLIKELY (parser.value("header.time_meas", value)) {
    set_uint(*header, "time_meas", std::get<uint64_t>(value));
  }

  if VLIKELY (parser.value("header.time_pub", value)) {
    set_uint(*header, "time_pub", std::get<uint64_t>(value));
  }
}

static std::string vector_field_name(std::string_view name) {
  const auto bracket = name.find('[');

  if (bracket == std::string_view::npos) {
    return std::string(name);
  }

  return std::string(name.substr(0, bracket));
}

static void add_point_fields(MessageProto& point, const std::vector<Field>& fields) {
  int number = 1;
  std::unordered_set<std::string> used_names;

  for (const auto& source : fields) {
    add_field(point, unique_field_name(source.name, used_names), number++, protobuf_type(source.type));
  }
}

static void add_object_fields(MessageProto& object, const std::vector<Field>& fields) {
  int number = 1;
  std::unordered_set<std::string> vectors;

  for (const auto& source : fields) {
    if (source.name.find('[') != std::string::npos) {
      auto field_name = vector_field_name(source.name);

      if (vectors.insert(field_name).second) {
        add_field(object, std::move(field_name), number++, protobuf_type(source.type), FieldProto::LABEL_REPEATED);
      }

      continue;
    }

    add_field(object, source.name, number++, protobuf_type(source.type));
  }
}

static const Descriptor* build_descriptor(const std::string& key, const std::string& type,
                                          const std::vector<Field>& root_fields,
                                          const std::vector<Field>* element_fields) {
  auto& store = dynamic_store();
  const std::lock_guard lock(dynamic_store_mutex());
  auto iter = store.descriptors.find(key);

  if VLIKELY (iter != store.descriptors.end()) {
    return iter->second;
  }

  const std::string package = "vlink.webviz.zerocopy.p" + std::to_string(store.descriptors.size());
  google::protobuf::FileDescriptorProto file_proto;
  file_proto.set_name(package + ".proto");
  file_proto.set_package(package);
  file_proto.set_syntax("proto3");
  auto* root = file_proto.add_message_type();
  root->set_name("Root");

  int number = 1;

  if (type != "ProxyData") {
    add_header(*root, package);
    number = 2;
  }

  std::string payload_name;

  for (const auto& source : root_fields) {
    if (Helpers::has_startwith(source.name, "header.")) {
      continue;
    }

    if (source.type == ValueType::kBytes) {
      payload_name = source.name;
      continue;
    }

    add_field(*root, source.name, number++, protobuf_type(source.type));
  }

  const std::vector<Field> no_fields;
  const auto& elements = element_fields != nullptr ? *element_fields : no_fields;
  const auto scalar_type = elements.empty() ? FieldProto::TYPE_DOUBLE : protobuf_type(elements.front().type);

  if (type == "PointCloud") {
    auto* point = root->add_nested_type();
    point->set_name("Point");
    add_point_fields(*point, elements);
    add_field(*root, payload_name, number++, FieldProto::TYPE_MESSAGE, FieldProto::LABEL_REPEATED,
              "." + package + ".Root.Point");
  } else if (type == "ObjectArray") {
    auto* object = root->add_nested_type();
    object->set_name("Object");
    add_object_fields(*object, elements);
    add_field(*root, payload_name, number++, FieldProto::TYPE_MESSAGE, FieldProto::LABEL_REPEATED,
              "." + package + ".Root.Object");
  } else if (type == "OccupancyGrid") {
    add_field(*root, payload_name, number++, scalar_type, FieldProto::LABEL_REPEATED);
  } else if (type == "Tensor") {
    add_field(*root, "shape", number++, FieldProto::TYPE_UINT32, FieldProto::LABEL_REPEATED);
    add_field(*root, "strides", number++, FieldProto::TYPE_UINT32, FieldProto::LABEL_REPEATED);
    add_field(*root, payload_name, number++, scalar_type, FieldProto::LABEL_REPEATED);
    add_field(*root, "raw", number++, FieldProto::TYPE_BYTES);
  } else if (type == "RawData" || type == "CameraFrame" || type == "ProxyData" || type == "AudioFrame") {
    add_field(*root, payload_name, number++, FieldProto::TYPE_BYTES);
  } else {
    return nullptr;
  }

  const auto* file = store.pool.BuildFile(file_proto);

  if VUNLIKELY (file == nullptr) {
    return nullptr;
  }

  const auto* descriptor = file->FindMessageTypeByName("Root");
  store.descriptors.emplace(key, descriptor);

  static constexpr size_t kDescriptorCacheWarnLimit = 256;

  if VUNLIKELY (store.descriptors.size() == kDescriptorCacheWarnLimit) {
    MLOG_W("zerocopy dynamic descriptor cache reached {} entries; unstable per-message schemas grow it without bound",
           kDescriptorCacheWarnLimit);
  }

  return descriptor;
}

static std::unique_ptr<Message> create_message(const Descriptor* descriptor) {
  if VUNLIKELY (descriptor == nullptr) {
    return nullptr;
  }

  const std::lock_guard lock(dynamic_store_mutex());
  const auto* prototype = dynamic_store().factory.GetPrototype(descriptor);
  return prototype == nullptr ? nullptr : std::unique_ptr<Message>(prototype->New());
}

static double numeric_value(const zerocopy::MessageParser::Value& value) {
  return std::visit(
      [](const auto& source) -> double {
        using Value = std::decay_t<decltype(source)>;

        if constexpr (std::is_integral_v<Value>) {
          return expression_integer_to_double(source);
        } else if constexpr (std::is_floating_point_v<Value>) {
          return static_cast<double>(source);
        }

        return 0.0;
      },
      value);
}

static void set_value(Message& message, std::string_view name, const zerocopy::MessageParser::Value& value) {
  const auto* target = field(message, name);

  if VUNLIKELY (target == nullptr) {
    return;
  }

  if (const auto* source = std::get_if<std::string>(&value)) {
    set_string(message, name, *source);
    return;
  }

  if (const auto* source = std::get_if<Bytes>(&value)) {
    set_bytes(message, name, source->data(), source->size());
    return;
  }

  if (const auto* source = std::get_if<uint64_t>(&value)) {
    if (target->type() == FieldDescriptor::TYPE_DOUBLE) {
      set_double(message, name, expression_integer_to_double(*source));
    } else if (target->type() == FieldDescriptor::TYPE_INT64) {
      set_int(message, name, static_cast<int64_t>(*source));
    } else if (target->type() == FieldDescriptor::TYPE_BOOL) {
      set_bool(message, name, *source != 0);
    } else {
      set_uint(message, name, *source);
    }

    return;
  }

  if (const auto* source = std::get_if<int64_t>(&value)) {
    if (target->type() == FieldDescriptor::TYPE_DOUBLE) {
      set_double(message, name, expression_integer_to_double(*source));
    } else if (target->type() == FieldDescriptor::TYPE_BOOL) {
      set_bool(message, name, *source != 0);
    } else if (target->type() == FieldDescriptor::TYPE_INT64) {
      set_int(message, name, *source);
    } else {
      set_uint(message, name, static_cast<uint64_t>(*source));
    }

    return;
  }

  const auto source = std::get<double>(value);

  switch (target->type()) {
    case FieldDescriptor::TYPE_BOOL:
      set_bool(message, name, source != 0.0);
      break;
    case FieldDescriptor::TYPE_INT64:
      set_int(message, name, static_cast<int64_t>(source));
      break;
    case FieldDescriptor::TYPE_DOUBLE:
      set_double(message, name, source);
      break;
    default:
      set_uint(message, name, static_cast<uint64_t>(source));
      break;
  }
}

static void add_repeated_value(Message& message, std::string_view name, const zerocopy::MessageParser::Value& value) {
  const auto* target = field(message, name);

  if VUNLIKELY (target == nullptr) {
    return;
  }

  const auto* reflection = message.GetReflection();

  if (target->type() == FieldDescriptor::TYPE_UINT32) {
    uint32_t source = 0;

    if (const auto* exact = std::get_if<uint64_t>(&value)) {
      source = static_cast<uint32_t>(*exact);
    } else if (const auto* exact = std::get_if<int64_t>(&value)) {
      source = static_cast<uint32_t>(*exact);
    } else {
      source = static_cast<uint32_t>(std::get<double>(value));
    }

    reflection->AddUInt32(&message, target, source);
    return;
  }

  if (target->type() == FieldDescriptor::TYPE_INT64) {
    const auto source =
        std::holds_alternative<int64_t>(value) ? std::get<int64_t>(value) : static_cast<int64_t>(numeric_value(value));
    reflection->AddInt64(&message, target, source);
    return;
  }

  if (target->type() == FieldDescriptor::TYPE_UINT64) {
    const auto source = std::holds_alternative<uint64_t>(value) ? std::get<uint64_t>(value)
                                                                : static_cast<uint64_t>(numeric_value(value));
    reflection->AddUInt64(&message, target, source);
    return;
  }

  reflection->AddDouble(&message, target, numeric_value(value));
}

static void fill_root_fields(Message& message, const zerocopy::MessageParser& parser) {
  zerocopy::MessageParser::Value value;

  for (const auto& source : parser.fields()) {
    if (Helpers::has_startwith(source.name, "header.") || source.name == "data") {
      continue;
    }

    if VLIKELY (parser.value(source.name, value)) {
      set_value(message, source.name, value);
    }
  }
}

static constexpr size_t kFullCollection = static_cast<size_t>(-1);

static size_t referenced_collection_limit(const std::vector<std::string>& sources) {
  static constexpr std::string_view kCollection = "data";
  size_t limit = 0;

  for (const auto& source : sources) {
    size_t pos = source.find(kCollection);

    while (pos != std::string::npos) {
      const size_t after = pos + kCollection.size();
      const bool boundary_before =
          pos == 0 || (std::isalnum(static_cast<unsigned char>(source[pos - 1])) == 0 && source[pos - 1] != '_');

      if (boundary_before) {
        if (after < source.size() && source[after] == '[') {
          size_t index = 0;
          size_t cursor = after + 1;
          bool parsed = false;

          while (cursor < source.size() && std::isdigit(static_cast<unsigned char>(source[cursor])) != 0) {
            index = index * 10 + static_cast<size_t>(source[cursor] - '0');
            parsed = true;
            ++cursor;
          }

          if (parsed && cursor < source.size() && source[cursor] == ']') {
            limit = std::max(limit, index + 1);
          }
        } else if (after >= source.size() ||
                   (std::isalnum(static_cast<unsigned char>(source[after])) == 0 && source[after] != '_')) {
          return kFullCollection;
        }
      }

      pos = source.find(kCollection, after);
    }
  }

  return limit;
}

static void fill_point_data(Message& message, const zerocopy::MessageParser& parser,
                            const std::vector<zerocopy::MessageParser::Field>& fields, size_t limit) {
  const PointCloudView view(parser);
  if (!view.valid()) return;
  const auto* data = field(message, "data");
  const size_t count = std::min(view.size(), limit);
  std::vector<std::pair<std::string, const PointCloudFieldView*>> copies;
  copies.reserve(fields.size());
  std::unordered_set<std::string> used_names;
  for (const auto& source : fields) {
    const auto* source_field = view.find(source.name);
    if (source_field != nullptr) copies.emplace_back(unique_field_name(source.name, used_names), source_field);
  }

  for (size_t index = 0; index < count; ++index) {
    auto* point = message.GetReflection()->AddMessage(&message, data);

    for (const auto& [field_name, source] : copies) {
      zerocopy::MessageParser::Value value;

      if VLIKELY (view.value(index, *source, value)) {
        set_value(*point, field_name, value);
      }
    }
  }
}

static void fill_object_data(Message& message, const zerocopy::MessageParser& parser) {
  const auto* data = field(message, "data");
  const auto fields = parser.element_fields("data");

  for (size_t index = 0; index < parser.collection_size("data"); ++index) {
    auto* object = message.GetReflection()->AddMessage(&message, data);

    for (const auto& source : fields) {
      zerocopy::MessageParser::Value value;

      if VUNLIKELY (!parser.value("data", index, source.name, value)) {
        continue;
      }

      if (source.name.find('[') != std::string::npos) {
        add_repeated_value(*object, vector_field_name(source.name), value);
      } else if (source.name == "reserved_buf") {
        set_value(*object, "reserved", value);
      } else {
        set_value(*object, source.name, value);
      }
    }
  }
}

static void fill_scalar_collection(Message& message, const zerocopy::MessageParser& parser, std::string_view collection,
                                   size_t limit) {
  const size_t count = std::min(parser.collection_size(collection), limit);

  for (size_t index = 0; index < count; ++index) {
    zerocopy::MessageParser::Value value;

    if VLIKELY (parser.value(collection, index, "value", value)) {
      add_repeated_value(message, collection, value);
    }
  }
}

std::unique_ptr<google::protobuf::Message> make_zerocopy_message(const std::string& ser, const Bytes& raw,
                                                                 const std::vector<std::string>& sources) {
  zerocopy::MessageParser parser;

  if VUNLIKELY (!parser.parse(ser, raw)) {
    return nullptr;
  }

  return make_zerocopy_message(parser, sources);
}

std::unique_ptr<google::protobuf::Message> make_zerocopy_message(const zerocopy::MessageParser& parser,
                                                                 const std::vector<std::string>& sources) {
  if VUNLIKELY (!parser.valid()) {
    return nullptr;
  }

  const std::string type(zerocopy::MessageParser::type_name(parser.type()));
  const auto root_fields = parser.fields();
  const auto element_fields = parser.element_fields("data");
  std::string descriptor_key = type;

  if (!element_fields.empty()) {
    for (const auto& field : element_fields) {
      descriptor_key.append(1, ':')
          .append(std::to_string(field.name.size()))
          .append(1, ':')
          .append(field.name)
          .append(1, ':')
          .append(std::to_string(static_cast<uint8_t>(field.type)));
    }
  }

  const auto* collection_fields = element_fields.empty() ? nullptr : &element_fields;
  auto message = create_message(build_descriptor(descriptor_key, type, root_fields, collection_fields));

  if VUNLIKELY (!message) {
    return nullptr;
  }

  if (parser.type() != zerocopy::MessageParser::Type::kProxyData) {
    fill_header(*message, parser);
  }

  fill_root_fields(*message, parser);

  const size_t data_limit = referenced_collection_limit(sources);

  switch (parser.type()) {
    case zerocopy::MessageParser::Type::kObjectArray:
      fill_object_data(*message, parser);
      break;
    case zerocopy::MessageParser::Type::kPointCloud:
      fill_point_data(*message, parser, element_fields, data_limit);
      break;
    case zerocopy::MessageParser::Type::kOccupancyGrid:
      fill_scalar_collection(*message, parser, "data", data_limit);
      break;
    case zerocopy::MessageParser::Type::kTensor:
      fill_scalar_collection(*message, parser, "shape", kFullCollection);
      fill_scalar_collection(*message, parser, "strides", kFullCollection);
      fill_scalar_collection(*message, parser, "data", data_limit);
      break;
    default:
      break;
  }

  return message;
}

}  // namespace webviz

}  // namespace vlink
