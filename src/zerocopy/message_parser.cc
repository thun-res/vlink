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

#include <vlink/base/helpers.h>
#include <vlink/base/name_detector.h>
#include <vlink/serializer.h>
#include <vlink/zerocopy/message_parser.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

namespace vlink {

namespace zerocopy {

template <typename T>
static T read_unaligned(const uint8_t* data) noexcept {
  T value{};
  std::memcpy(&value, data, sizeof(value));

  return value;
}

static float bits_to_float(uint32_t bits) noexcept {
  float value = 0.0F;
  std::memcpy(&value, &bits, sizeof(value));

  return value;
}

static bool parse_index(std::string_view path, std::string_view prefix, size_t& index,
                        std::string_view& remainder) noexcept {
  if VUNLIKELY (!Helpers::has_startwith(path, prefix) || path.size() <= prefix.size() || path[prefix.size()] != '[') {
    return false;
  }

  const size_t close = path.find(']', prefix.size() + 1);

  if VUNLIKELY (close == std::string_view::npos) {
    return false;
  }

  const char* first = path.data() + prefix.size() + 1;
  const char* last = path.data() + close;
  const auto result = std::from_chars(first, last, index);

  if VUNLIKELY (result.ec != std::errc() || result.ptr != last) {
    return false;
  }

  remainder = path.substr(close + 1);

  if (remainder.empty()) {
    return true;
  }

  if VUNLIKELY (!Helpers::has_startwith(remainder, ".")) {
    return false;
  }

  remainder.remove_prefix(1);

  return true;
}

template <typename T>
static bool store_number(T value, MessageParser::Value& out) {
  if constexpr (std::is_floating_point_v<T>) {
    out = static_cast<double>(value);
  } else if constexpr (std::is_signed_v<T>) {
    out = static_cast<int64_t>(value);
  } else {
    out = static_cast<uint64_t>(value);
  }

  return true;
}

template <typename T>
static bool read_number(std::string_view path, std::string_view field, T value, MessageParser::Value& out) {
  if (path != field) {
    return false;
  }

  return store_number(value, out);
}

static bool read_string(std::string_view path, std::string_view field, std::string_view value,
                        MessageParser::Value& out) {
  if (path != field) {
    return false;
  }

  out = std::string(value);

  return true;
}

static double half_to_double(uint16_t value) noexcept {
  const uint32_t sign = static_cast<uint32_t>(value & 0x8000U) << 16U;

  uint32_t exponent = (value >> 10U) & 0x1FU;
  uint32_t mantissa = value & 0x03FFU;
  uint32_t bits = 0;

  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      exponent = 113U;

      while ((mantissa & 0x0400U) == 0) {
        mantissa <<= 1U;
        --exponent;
      }

      bits = sign | (exponent << 23U) | ((mantissa & 0x03FFU) << 13U);
    }
  } else if (exponent == 0x1FU) {
    bits = sign | 0x7F800000U | (mantissa << 13U);
  } else {
    bits = sign | ((exponent + 112U) << 23U) | (mantissa << 13U);
  }

  return static_cast<double>(bits_to_float(bits));
}

static bool header_value(const Header& header, std::string_view path, MessageParser::Value& out) {
  if (path == "header.frame_id") {
    out = std::string(header.frame_id_view());
    return true;
  }

  if (read_number(path, "header.seq", header.seq, out)) {
    return true;
  }

  if (read_number(path, "header.reserved", header.reserved, out)) {
    return true;
  }

  if (read_number(path, "header.time_meas", header.time_meas, out)) {
    return true;
  }

  if (read_number(path, "header.time_pub", header.time_pub, out)) {
    return true;
  }

  return false;
}

static bool convert_numeric(const MessageParser::Value& value, double& out, bool* precision_loss) noexcept {
  bool loss = false;

  if (const auto* signed_number = std::get_if<int64_t>(&value)) {
    constexpr int64_t kMaxExactInteger = INT64_C(1) << std::numeric_limits<double>::digits;
    loss = *signed_number > kMaxExactInteger || *signed_number < -kMaxExactInteger;
    out = static_cast<double>(*signed_number);
  } else if (const auto* unsigned_number = std::get_if<uint64_t>(&value)) {
    constexpr uint64_t kMaxExactInteger = UINT64_C(1) << std::numeric_limits<double>::digits;
    loss = *unsigned_number > kMaxExactInteger;
    out = static_cast<double>(*unsigned_number);
  } else if (const auto* double_number = std::get_if<double>(&value)) {
    out = *double_number;
  } else {
    return false;
  }

  if (precision_loss != nullptr) {
    *precision_loss = loss;
  }

  return true;
}

static std::string_view normalize_collection(std::string_view collection) noexcept {
  if (collection == "objects" || collection == "points" || collection == "cells" || collection == "elements") {
    return "data";
  }

  return collection;
}

static MessageParser::ValueType point_value_type(uint8_t type, uint8_t size) noexcept {
  switch (type) {
    case PointCloud::kInt8Type:
    case PointCloud::kInt16Type:
    case PointCloud::kInt32Type:
    case PointCloud::kInt64Type:
      return MessageParser::ValueType::kInt64;
    case PointCloud::kBoolType:
    case PointCloud::kUint8Type:
    case PointCloud::kUint16Type:
    case PointCloud::kUint32Type:
    case PointCloud::kUint64Type:
      return MessageParser::ValueType::kUInt64;
    case PointCloud::kUnknownType:
      if (size == sizeof(uint8_t)) {
        return MessageParser::ValueType::kUInt64;
      }

      if (size == sizeof(int16_t)) {
        return MessageParser::ValueType::kInt64;
      }

      return MessageParser::ValueType::kDouble;
    default:
      return MessageParser::ValueType::kDouble;
  }
}

static size_t hash_point_field(std::string_view name) noexcept {
  size_t hash = sizeof(size_t) == sizeof(uint64_t) ? static_cast<size_t>(UINT64_C(14695981039346656037))
                                                   : static_cast<size_t>(UINT32_C(2166136261));
  const size_t prime = sizeof(size_t) == sizeof(uint64_t) ? static_cast<size_t>(UINT64_C(1099511628211))
                                                          : static_cast<size_t>(UINT32_C(16777619));
  for (const char character : name) {
    hash ^= static_cast<uint8_t>(character);
    hash *= prime;
  }

  return hash;
}

static MessageParser::ValueType tensor_value_type(uint8_t type) noexcept {
  switch (type) {
    case Tensor::kInt8:
    case Tensor::kInt16:
    case Tensor::kInt32:
    case Tensor::kInt64:
      return MessageParser::ValueType::kInt64;
    case Tensor::kBool:
    case Tensor::kUint8:
    case Tensor::kUint16:
    case Tensor::kUint32:
    case Tensor::kUint64:
      return MessageParser::ValueType::kUInt64;
    default:
      return MessageParser::ValueType::kDouble;
  }
}

static size_t audio_element_size(uint8_t format) noexcept {
  switch (format) {
    case AudioFrame::kFormatPcmU8:
      return sizeof(uint8_t);
    case AudioFrame::kFormatPcmS16:
      return sizeof(int16_t);
    case AudioFrame::kFormatPcmS24:
      return 3;
    case AudioFrame::kFormatPcmS32:
      return sizeof(int32_t);
    case AudioFrame::kFormatPcmF32:
      return sizeof(float);
    default:
      return 0;
  }
}

MessageParser::Type MessageParser::detect_type(std::string_view serialized_type) noexcept {
  static constexpr std::pair<std::string_view, Type> kTypes[] = {
      {"RawData", Type::kRawData},         {"CameraFrame", Type::kCameraFrame},     {"PointCloud", Type::kPointCloud},
      {"ProxyData", Type::kProxyData},     {"OccupancyGrid", Type::kOccupancyGrid}, {"Tensor", Type::kTensor},
      {"ObjectArray", Type::kObjectArray}, {"AudioFrame", Type::kAudioFrame},
  };

  for (const auto& [name, type] : kTypes) {
    if (serialized_type == name) {
      return type;
    }

    if (serialized_type.size() <= name.size()) {
      continue;
    }

    const size_t offset = serialized_type.size() - name.size();
    const char separator = serialized_type[offset - 1];

    if ((separator == '.' || separator == ':' || separator == '/') && serialized_type.substr(offset) == name) {
      return type;
    }
  }

  return Type::kUnknown;
}

std::string_view MessageParser::type_name(Type type) noexcept {
  switch (type) {
    case Type::kRawData:
      return "RawData";
    case Type::kCameraFrame:
      return "CameraFrame";
    case Type::kPointCloud:
      return "PointCloud";
    case Type::kProxyData:
      return "ProxyData";
    case Type::kOccupancyGrid:
      return "OccupancyGrid";
    case Type::kTensor:
      return "Tensor";
    case Type::kObjectArray:
      return "ObjectArray";
    case Type::kAudioFrame:
      return "AudioFrame";
    default:
      return {};
  }
}

bool MessageParser::parse(std::string_view serialized_type, const Bytes& bytes) {
  return parse(detect_type(serialized_type), bytes);
}

bool MessageParser::parse(Type type, const Bytes& bytes) {
  clear();

  bool parsed = false;

  switch (type) {
    case Type::kRawData:
      parsed = Serializer::convert(bytes, message_.emplace<RawData>());
      break;

    case Type::kCameraFrame:
      parsed = Serializer::convert(bytes, message_.emplace<CameraFrame>());
      break;

    case Type::kPointCloud:
      parsed = Serializer::convert(bytes, message_.emplace<PointCloud>());
      break;

    case Type::kProxyData:
      parsed = Serializer::convert(bytes, message_.emplace<ProxyData>());
      break;

    case Type::kOccupancyGrid:
      parsed = Serializer::convert(bytes, message_.emplace<OccupancyGrid>());
      break;

    case Type::kTensor:
      parsed = Serializer::convert(bytes, message_.emplace<Tensor>());
      break;

    case Type::kObjectArray:
      parsed = Serializer::convert(bytes, message_.emplace<ObjectArray>());
      break;

    case Type::kAudioFrame:
      parsed = Serializer::convert(bytes, message_.emplace<AudioFrame>());
      break;
    default:
      return false;
  }

  if VUNLIKELY (!parsed) {
    clear();
    return false;
  }

  if (type == Type::kPointCloud) {
    const auto& message = std::get<PointCloud>(message_);
    PointCloud::KeyList keys;

    [[maybe_unused]] const auto key_map = message.get_key_map(&keys);
    point_fields_.reserve(keys.size());

    size_t byte_offset = 0;

    for (size_t element_index = 0; element_index < keys.size(); ++element_index) {
      const auto& key = keys[element_index];
      Field field{key.name, point_value_type(key.type, key.size), key.type, key.size};
      field.is_bool = key.type == PointCloud::kBoolType;
      field.byte_offset = byte_offset;
      field.element_index = element_index;
      point_fields_.push_back(std::move(field));
      byte_offset += key.size;
    }

    size_t bucket_count = 1;

    while (bucket_count < point_fields_.size() * 2) {
      bucket_count <<= 1U;
    }

    constexpr auto kNoField = static_cast<size_t>(-1);

    point_field_buckets_.assign(bucket_count, kNoField);
    point_field_next_.assign(point_fields_.size(), kNoField);

    for (size_t i = point_fields_.size(); i-- > 0;) {
      const size_t bucket = hash_point_field(point_fields_[i].name) & (bucket_count - 1);
      point_field_next_[i] = point_field_buckets_[bucket];
      point_field_buckets_[bucket] = i;
    }
  }

  type_ = type;

  switch (type_) {
    case Type::kRawData:
      reserved_[0] = std::get<RawData>(message_).get_reserved();
      break;
    case Type::kCameraFrame:
      reserved_[0] = std::get<CameraFrame>(message_).get_reserved();
      break;
    case Type::kPointCloud:
      reserved_[0] = std::get<PointCloud>(message_).get_reserved();
      reserved_[1] = std::get<PointCloud>(message_).get_reserved2();
      reserved_[2] = std::get<PointCloud>(message_).get_reserved3();
      break;
    case Type::kProxyData:
      reserved_[0] = std::get<ProxyData>(message_).get_reserved();
      reserved_[1] = std::get<ProxyData>(message_).get_reserved2();
      break;
    case Type::kOccupancyGrid:
      reserved_[0] = std::get<OccupancyGrid>(message_).get_reserved();
      reserved_[1] = std::get<OccupancyGrid>(message_).get_reserved2();
      reserved_[2] = std::get<OccupancyGrid>(message_).get_reserved3();
      break;
    case Type::kTensor:
      reserved_[0] = std::get<Tensor>(message_).get_reserved();
      reserved_[1] = std::get<Tensor>(message_).get_reserved2();
      reserved_[2] = std::get<Tensor>(message_).get_reserved3();
      break;
    case Type::kObjectArray:
      reserved_[0] = std::get<ObjectArray>(message_).get_reserved();
      reserved_[1] = std::get<ObjectArray>(message_).get_reserved2();
      reserved_[2] = std::get<ObjectArray>(message_).get_reserved3();
      reserved_[3] = std::get<ObjectArray>(message_).get_reserved4();
      reserved_[4] = std::get<ObjectArray>(message_).get_reserved5();
      break;
    case Type::kAudioFrame:
      reserved_[0] = std::get<AudioFrame>(message_).get_reserved();
      reserved_[1] = std::get<AudioFrame>(message_).get_reserved2();
      break;
    default:
      break;
  }

  return true;
}

void MessageParser::clear() noexcept {
  type_ = Type::kUnknown;
  message_.emplace<std::monostate>();
  reserved_.fill(0);
  point_fields_.clear();
  point_field_buckets_.clear();
  point_field_next_.clear();
}

bool MessageParser::value(std::string_view path, Value& out) const {
  if (path.find('[') == std::string_view::npos) {
    return root_value(path, out);
  }

  size_t index = 0;
  std::string_view field;
  for (const std::string_view collection : {"data", "objects", "points", "cells", "elements", "shape", "strides"}) {
    if (parse_index(path, collection, index, field)) {
      return element_value(normalize_collection(collection), index, field, out);
    }
  }

  return root_value(path, out);
}

bool MessageParser::value(std::string_view collection, size_t index, std::string_view field, Value& out) const {
  return element_value(normalize_collection(collection), index, field, out);
}

bool MessageParser::value(std::string_view collection, size_t index, const Field& field, Value& out) const {
  collection = normalize_collection(collection);

  if (type_ != Type::kPointCloud || collection != "data" || index >= collection_size(collection) ||
      field.element_index >= point_fields_.size()) {
    return false;
  }

  const auto& cached = point_fields_[field.element_index];
  if (cached.name != field.name || cached.type != field.type || cached.native_type != field.native_type ||
      cached.storage_size != field.storage_size || cached.byte_offset != field.byte_offset) {
    return false;
  }

  return point_value(index, cached, out);
}

bool MessageParser::numeric(std::string_view path, double& out, bool* precision_loss) const {
  Value parsed;
  return value(path, parsed) && convert_numeric(parsed, out, precision_loss);
}

bool MessageParser::numeric(std::string_view collection, size_t index, std::string_view field, double& out,
                            bool* precision_loss) const {
  Value parsed;
  return value(collection, index, field, parsed) && convert_numeric(parsed, out, precision_loss);
}

bool MessageParser::numeric(std::string_view collection, size_t index, const Field& field, double& out,
                            bool* precision_loss) const {
  Value parsed;
  return value(collection, index, field, parsed) && convert_numeric(parsed, out, precision_loss);
}

bool MessageParser::text(std::string_view path, std::string& out) const {
  Value parsed;

  if VUNLIKELY (!value(path, parsed)) {
    return false;
  }

  auto* string = std::get_if<std::string>(&parsed);

  if VUNLIKELY (string == nullptr) {
    return false;
  }

  out = std::move(*string);

  return true;
}

bool MessageParser::text(std::string_view collection, size_t index, std::string_view field, std::string& out) const {
  Value parsed;

  if VUNLIKELY (!value(collection, index, field, parsed)) {
    return false;
  }

  auto* string = std::get_if<std::string>(&parsed);

  if VUNLIKELY (string == nullptr) {
    return false;
  }

  out = std::move(*string);

  return true;
}

bool MessageParser::root_value(std::string_view path, Value& out) const {
  if (Helpers::has_startwith(path, "header.")) {
    const Header* header = nullptr;
    switch (type_) {
      case Type::kRawData:
        header = &get<RawData>()->header;
        break;

      case Type::kCameraFrame:
        header = &get<CameraFrame>()->header;
        break;

      case Type::kPointCloud:
        header = &get<PointCloud>()->header;
        break;

      case Type::kOccupancyGrid:
        header = &get<OccupancyGrid>()->header;
        break;

      case Type::kTensor:
        header = &get<Tensor>()->header;
        break;

      case Type::kObjectArray:
        header = &get<ObjectArray>()->header;
        break;

      case Type::kAudioFrame:
        header = &get<AudioFrame>()->header;
        break;
      default:
        break;
    }

    return header != nullptr && header_value(*header, path, out);
  }

  switch (type_) {
    case Type::kRawData: {
      const auto& message = *get<RawData>();

      if (read_number(path, "size", message.size(), out)) {
        return true;
      }

      if (read_number(path, "reserved", reserved_[0], out)) {
        return true;
      }

      if (path == "data") {
        out = Bytes::shallow_copy(message.data(), message.size());
        return true;
      }

      break;
    }

    case Type::kCameraFrame: {
      const auto& message = *get<CameraFrame>();

      if (read_number(path, "channel", message.channel(), out)) {
        return true;
      }

      if (read_number(path, "width", message.width(), out)) {
        return true;
      }

      if (read_number(path, "height", message.height(), out)) {
        return true;
      }

      if (read_number(path, "freq", message.freq(), out)) {
        return true;
      }

      if (read_number(path, "format", message.format(), out)) {
        return true;
      }

      if (read_number(path, "stream", message.stream(), out)) {
        return true;
      }

      if (read_number(path, "size", message.size(), out)) {
        return true;
      }

      if (read_number(path, "reserved", reserved_[0], out)) {
        return true;
      }

      if (path == "data") {
        out = Bytes::shallow_copy(message.data(), message.size());
        return true;
      }

      break;
    }

    case Type::kPointCloud: {
      const auto& message = *get<PointCloud>();

      if (read_number(path, "size", message.size(), out)) {
        return true;
      }

      if (read_number(path, "pack_size", message.pack_size(), out)) {
        return true;
      }

      if (read_number(path, "extent", message.get_extent(), out)) {
        return true;
      }

      if (read_number(path, "downsample", message.get_downsample(), out)) {
        return true;
      }

      if (read_number(path, "vertical", message.get_vertical(), out)) {
        return true;
      }

      if (read_number(path, "reserved_size", message.get_reserved_size(), out)) {
        return true;
      }

      if (read_number(path, "get_reserved_size", message.get_reserved_size(), out)) {
        return true;
      }

      if (read_number(path, "reserved", reserved_[0], out)) {
        return true;
      }

      if (read_number(path, "reserved2", reserved_[1], out)) {
        return true;
      }

      if (read_number(path, "reserved3", reserved_[2], out)) {
        return true;
      }

      if (path == "data") {
        out = Bytes::shallow_copy(message.get_internal_data(), message.size() * message.pack_size());
        return true;
      }

      break;
    }

    case Type::kProxyData: {
      const auto& message = *get<ProxyData>();

      if (read_number(path, "control_id", message.control_id(), out)) {
        return true;
      }

      if (read_number(path, "mode", message.mode(), out)) {
        return true;
      }

      if (read_number(path, "timestamp", message.timestamp(), out)) {
        return true;
      }

      if (read_number(path, "seq", message.seq(), out)) {
        return true;
      }

      if (read_number(path, "schema", message.schema(), out)) {
        return true;
      }

      if (read_number(path, "size", message.raw().size(), out)) {
        return true;
      }

      if (read_number(path, "raw.size", message.raw().size(), out)) {
        return true;
      }

      if (read_number(path, "reserved", reserved_[0], out)) {
        return true;
      }

      if (read_number(path, "reserved2", reserved_[1], out)) {
        return true;
      }

      if (read_string(path, "url", message.url(), out)) {
        return true;
      }

      if (read_string(path, "ser", message.ser(), out)) {
        return true;
      }

      if (read_string(path, "hostname", message.hostname(), out)) {
        return true;
      }

      if (path == "raw" || path == "data") {
        out = message.raw();
        return true;
      }

      break;
    }

    case Type::kOccupancyGrid: {
      const auto& message = *get<OccupancyGrid>();

      if (read_string(path, "map_id", message.map_id(), out)) {
        return true;
      }

      if (read_number(path, "channel", message.channel(), out)) {
        return true;
      }

      if (read_number(path, "freq", message.freq(), out)) {
        return true;
      }

      if (read_number(path, "width", message.width(), out)) {
        return true;
      }

      if (read_number(path, "height", message.height(), out)) {
        return true;
      }

      if (read_number(path, "resolution", message.resolution(), out)) {
        return true;
      }

      if (read_number(path, "origin_x", message.origin_x(), out)) {
        return true;
      }

      if (read_number(path, "origin_y", message.origin_y(), out)) {
        return true;
      }

      if (read_number(path, "origin_z", message.origin_z(), out)) {
        return true;
      }

      if (read_number(path, "origin_yaw", message.origin_yaw(), out)) {
        return true;
      }

      if (read_number(path, "cell_type", message.cell_type(), out)) {
        return true;
      }

      if (read_number(path, "cell_size", message.cell_size(), out)) {
        return true;
      }

      if (read_number(path, "default_value", message.default_value(), out)) {
        return true;
      }

      if (read_number(path, "value_min", message.value_min(), out)) {
        return true;
      }

      if (read_number(path, "value_max", message.value_max(), out)) {
        return true;
      }

      if (read_number(path, "occupied_threshold", message.occupied_threshold(), out)) {
        return true;
      }

      if (read_number(path, "free_threshold", message.free_threshold(), out)) {
        return true;
      }

      if (read_number(path, "valid_cell_count", message.valid_cell_count(), out)) {
        return true;
      }

      if (read_number(path, "update_time_ns", message.update_time_ns(), out)) {
        return true;
      }

      if (read_number(path, "size", message.size(), out)) {
        return true;
      }

      if (read_number(path, "reserved", reserved_[0], out)) {
        return true;
      }

      if (read_number(path, "reserved2", reserved_[1], out)) {
        return true;
      }

      if (read_number(path, "reserved3", reserved_[2], out)) {
        return true;
      }

      if (path == "data") {
        out = Bytes::shallow_copy(message.data(), message.size());
        return true;
      }

      break;
    }

    case Type::kTensor: {
      const auto& message = *get<Tensor>();

      if (read_string(path, "name", message.name(), out)) {
        return true;
      }

      if (read_string(path, "model_id", message.model_id(), out)) {
        return true;
      }

      if (read_string(path, "layout", message.layout(), out)) {
        return true;
      }

      if (read_number(path, "dtype", message.dtype(), out)) {
        return true;
      }

      if (read_number(path, "device", message.device(), out)) {
        return true;
      }

      if (read_number(path, "rank", message.rank(), out)) {
        return true;
      }

      if (read_number(path, "num_elements", message.num_elements(), out)) {
        return true;
      }

      if (read_number(path, "element_size", message.element_size(), out)) {
        return true;
      }

      if (read_number(path, "batch_size", message.batch_size(), out)) {
        return true;
      }

      if (read_number(path, "quant_scale", message.quant_scale(), out)) {
        return true;
      }

      if (read_number(path, "quant_zero_point", message.quant_zero_point(), out)) {
        return true;
      }

      if (read_number(path, "channel", message.channel(), out)) {
        return true;
      }

      if (read_number(path, "freq", message.freq(), out)) {
        return true;
      }

      if (read_number(path, "update_time_ns", message.update_time_ns(), out)) {
        return true;
      }

      if (read_number(path, "size", message.size(), out)) {
        return true;
      }

      if (read_number(path, "reserved", reserved_[0], out)) {
        return true;
      }

      if (read_number(path, "reserved2", reserved_[1], out)) {
        return true;
      }

      if (read_number(path, "reserved3", reserved_[2], out)) {
        return true;
      }

      if (path == "data") {
        out = Bytes::shallow_copy(message.data(), message.size());
        return true;
      }

      break;
    }

    case Type::kObjectArray: {
      const auto& message = *get<ObjectArray>();

      if (read_string(path, "source_id", message.source_id(), out)) {
        return true;
      }

      if (read_number(path, "channel", message.channel(), out)) {
        return true;
      }

      if (read_number(path, "freq", message.freq(), out)) {
        return true;
      }

      if (read_number(path, "count", message.count(), out)) {
        return true;
      }

      if (read_number(path, "pack_size", message.pack_size(), out)) {
        return true;
      }

      if (read_number(path, "size", static_cast<uint64_t>(message.count()) * message.pack_size(), out)) {
        return true;
      }

      if (read_number(path, "update_time_ns", message.update_time_ns(), out)) {
        return true;
      }

      if (read_number(path, "reserved", reserved_[0], out)) {
        return true;
      }

      if (read_number(path, "reserved2", reserved_[1], out)) {
        return true;
      }

      if (read_number(path, "reserved3", reserved_[2], out)) {
        return true;
      }

      if (read_number(path, "reserved4", reserved_[3], out)) {
        return true;
      }

      if (read_number(path, "reserved5", reserved_[4], out)) {
        return true;
      }

      if (path == "data") {
        out = Bytes::shallow_copy(message.data(), static_cast<size_t>(message.count()) * message.pack_size());
        return true;
      }

      break;
    }

    case Type::kAudioFrame: {
      const auto& message = *get<AudioFrame>();

      if (read_string(path, "codec", message.codec(), out)) {
        return true;
      }

      if (read_string(path, "language", message.language(), out)) {
        return true;
      }

      if (read_number(path, "channel", message.channel(), out)) {
        return true;
      }

      if (read_number(path, "freq", message.freq(), out)) {
        return true;
      }

      if (read_number(path, "sample_rate", message.sample_rate(), out)) {
        return true;
      }

      if (read_number(path, "num_samples", message.num_samples(), out)) {
        return true;
      }

      if (read_number(path, "num_channels", message.num_channels(), out)) {
        return true;
      }

      if (read_number(path, "bit_depth", message.bit_depth(), out)) {
        return true;
      }

      if (read_number(path, "bitrate", message.bitrate(), out)) {
        return true;
      }

      if (read_number(path, "format", message.format(), out)) {
        return true;
      }

      if (read_number(path, "layout", message.layout(), out)) {
        return true;
      }

      if (read_number(path, "duration_ns", message.duration_ns(), out)) {
        return true;
      }

      if (read_number(path, "update_time_ns", message.update_time_ns(), out)) {
        return true;
      }

      if (read_number(path, "size", message.size(), out)) {
        return true;
      }

      if (read_number(path, "reserved", reserved_[0], out)) {
        return true;
      }

      if (read_number(path, "reserved2", reserved_[1], out)) {
        return true;
      }

      if (path == "data") {
        out = Bytes::shallow_copy(message.data(), message.size());
        return true;
      }

      break;
    }

    default:
      break;
  }

  return false;
}

bool MessageParser::element_value(std::string_view collection, size_t index, std::string_view field, Value& out) const {
  if VUNLIKELY (index >= collection_size(collection)) {
    return false;
  }

  if (type_ == Type::kTensor) {
    const auto& message = *get<Tensor>();

    if (collection == "shape") {
      if VUNLIKELY (!field.empty() && field != "value") {
        return false;
      }

      return store_number(message.shape_at(static_cast<uint8_t>(index)), out);
    }

    if (collection == "strides") {
      if VUNLIKELY (!field.empty() && field != "value") {
        return false;
      }

      return store_number(message.stride_at(static_cast<uint8_t>(index)), out);
    }

    if (collection != "data" || (!field.empty() && field != "value")) {
      return false;
    }

    const uint8_t* data = message.data() + index * message.element_size();
    switch (message.dtype()) {
      case Tensor::kBool:
        return store_number(read_unaligned<uint8_t>(data) != 0, out);
      case Tensor::kInt8:
        return store_number(read_unaligned<int8_t>(data), out);
      case Tensor::kUint8:
        return store_number(read_unaligned<uint8_t>(data), out);
      case Tensor::kInt16:
        return store_number(read_unaligned<int16_t>(data), out);
      case Tensor::kUint16:
        return store_number(read_unaligned<uint16_t>(data), out);
      case Tensor::kInt32:
        return store_number(read_unaligned<int32_t>(data), out);
      case Tensor::kUint32:
        return store_number(read_unaligned<uint32_t>(data), out);
      case Tensor::kInt64:
        return store_number(read_unaligned<int64_t>(data), out);
      case Tensor::kUint64:
        return store_number(read_unaligned<uint64_t>(data), out);
      case Tensor::kFloat16:
        out = half_to_double(read_unaligned<uint16_t>(data));
        return true;
      case Tensor::kBfloat16: {
        const uint32_t bits = static_cast<uint32_t>(read_unaligned<uint16_t>(data)) << 16U;
        out = static_cast<double>(bits_to_float(bits));
        return true;
      }

      case Tensor::kFloat32:
        return store_number(read_unaligned<float>(data), out);
      case Tensor::kFloat64:
        return store_number(read_unaligned<double>(data), out);
      default:
        return false;
    }
  }

  if (type_ == Type::kOccupancyGrid && collection == "data") {
    const auto& message = *get<OccupancyGrid>();

    if VUNLIKELY (!field.empty() && field != "value") {
      return false;
    }

    const uint8_t* data = message.data() + index * message.cell_size();

    switch (message.cell_type()) {
      case OccupancyGrid::kCellInt8:
        return store_number(read_unaligned<int8_t>(data), out);
      case OccupancyGrid::kCellUint8:
        return store_number(read_unaligned<uint8_t>(data), out);
      case OccupancyGrid::kCellUint16:
        return store_number(read_unaligned<uint16_t>(data), out);
      case OccupancyGrid::kCellFloat32:
        return store_number(read_unaligned<float>(data), out);
      default:
        return false;
    }
  }

  if (type_ == Type::kPointCloud && collection == "data") {
    if VUNLIKELY (point_field_buckets_.empty()) {
      return false;
    }

    constexpr auto kNoField = static_cast<size_t>(-1);
    size_t field_index = point_field_buckets_[hash_point_field(field) & (point_field_buckets_.size() - 1)];

    while (field_index != kNoField && point_fields_[field_index].name != field) {
      field_index = point_field_next_[field_index];
    }

    if VUNLIKELY (field_index == kNoField) {
      return false;
    }

    return point_value(index, point_fields_[field_index], out);
  }

  if (type_ == Type::kAudioFrame && collection == "data") {
    const auto& message = *get<AudioFrame>();

    if VUNLIKELY (!field.empty() && field != "value") {
      return false;
    }

    const size_t element_size = audio_element_size(message.format());
    const uint8_t* data = message.data() + index * element_size;

    switch (message.format()) {
      case AudioFrame::kFormatPcmU8:
        return store_number(read_unaligned<uint8_t>(data), out);
      case AudioFrame::kFormatPcmS16:
        return store_number(read_unaligned<int16_t>(data), out);
      case AudioFrame::kFormatPcmS24: {
        const int32_t magnitude = static_cast<int32_t>(read_unaligned<uint8_t>(data)) |
                                  (static_cast<int32_t>(read_unaligned<uint8_t>(data + 1)) << 8U) |
                                  (static_cast<int32_t>(read_unaligned<uint8_t>(data + 2)) << 16U);
        return store_number((magnitude ^ 0x800000) - 0x800000, out);
      }

      case AudioFrame::kFormatPcmS32:
        return store_number(read_unaligned<int32_t>(data), out);
      case AudioFrame::kFormatPcmF32:
        return store_number(read_unaligned<float>(data), out);
      default:
        return false;
    }
  }

  if (type_ != Type::kObjectArray || collection != "data") {
    return false;
  }

  const auto* object = get<ObjectArray>()->objects(static_cast<uint32_t>(index));

  if VUNLIKELY (object == nullptr) {
    return false;
  }

  if (field == "label") {
    out = std::string(object->label, strnlen(object->label, sizeof(object->label)));
    return true;
  }

  if (read_number(field, "yaw", object->yaw, out)) {
    return true;
  }

  if (read_number(field, "yaw_rate", object->yaw_rate, out)) {
    return true;
  }

  if (read_number(field, "score", object->score, out)) {
    return true;
  }

  if (read_number(field, "existence_probability", object->existence_probability, out)) {
    return true;
  }

  if (read_number(field, "class_id", object->class_id, out)) {
    return true;
  }

  if (read_number(field, "track_id", object->track_id, out)) {
    return true;
  }

  if (read_number(field, "age", object->age, out)) {
    return true;
  }

  if (read_number(field, "num_observations", object->num_observations, out)) {
    return true;
  }

  if (read_number(field, "motion_state", object->motion_state, out)) {
    return true;
  }

  if (read_number(field, "source_type", object->source_type, out)) {
    return true;
  }

  if (read_number(field, "subtype_id", object->subtype_id, out)) {
    return true;
  }

  if (read_number(field, "reserved_buf", object->reserved_buf, out)) {
    return true;
  }

  if (field == "reserved") {
    return store_number(object->reserved_buf, out);
  }

  static constexpr std::pair<std::string_view, uint8_t> kVectorFields[] = {
      {"position", 3}, {"size", 3}, {"velocity", 3}, {"acceleration", 3}, {"position_covariance", 6}};

  for (const auto& [name, count] : kVectorFields) {
    size_t component = 0;
    std::string_view tail;

    if (!parse_index(field, name, component, tail) || !tail.empty() || component >= count) {
      continue;
    }

    if (name == "position") {
      return store_number(object->position[component], out);
    }

    if (name == "size") {
      return store_number(object->size[component], out);
    }

    if (name == "velocity") {
      return store_number(object->velocity[component], out);
    }

    if (name == "acceleration") {
      return store_number(object->acceleration[component], out);
    }

    return store_number(object->position_covariance[component], out);
  }

  static constexpr std::pair<std::string_view, std::pair<std::string_view, size_t>> kAliases[] = {
      {"position_x", {"position", 0}},
      {"position_y", {"position", 1}},
      {"position_z", {"position", 2}},
      {"size_x", {"size", 0}},
      {"size_y", {"size", 1}},
      {"size_z", {"size", 2}},
      {"velocity_x", {"velocity", 0}},
      {"velocity_y", {"velocity", 1}},
      {"velocity_z", {"velocity", 2}},
      {"acceleration_x", {"acceleration", 0}},
      {"acceleration_y", {"acceleration", 1}},
      {"acceleration_z", {"acceleration", 2}}};

  for (const auto& [alias, target] : kAliases) {
    if (field != alias) {
      continue;
    }

    if (target.first == "position") {
      return store_number(object->position[target.second], out);
    }

    if (target.first == "size") {
      return store_number(object->size[target.second], out);
    }

    if (target.first == "velocity") {
      return store_number(object->velocity[target.second], out);
    }

    return store_number(object->acceleration[target.second], out);
  }

  return false;
}

bool MessageParser::point_value(size_t index, const Field& field, Value& out) const {
  const auto& message = *get<PointCloud>();

  if (message.get_extent() != 0 && field.element_index < 3) {
    return store_number(message.get_value<double>(index, field.byte_offset), out);
  }

  switch (field.native_type) {
    case PointCloud::kBoolType:
      return store_number(message.get_value<bool>(index, field.byte_offset), out);
    case PointCloud::kInt8Type:
      return store_number(message.get_value<int8_t>(index, field.byte_offset), out);
    case PointCloud::kUint8Type:
      return store_number(message.get_value<uint8_t>(index, field.byte_offset), out);
    case PointCloud::kInt16Type:
      return store_number(message.get_value<int16_t>(index, field.byte_offset), out);
    case PointCloud::kUint16Type:
      return store_number(message.get_value<uint16_t>(index, field.byte_offset), out);
    case PointCloud::kInt32Type:
      return store_number(message.get_value<int32_t>(index, field.byte_offset), out);
    case PointCloud::kUint32Type:
      return store_number(message.get_value<uint32_t>(index, field.byte_offset), out);
    case PointCloud::kInt64Type:
      return store_number(message.get_value<int64_t>(index, field.byte_offset), out);
    case PointCloud::kUint64Type:
      return store_number(message.get_value<uint64_t>(index, field.byte_offset), out);
    case PointCloud::kFloatType:
      return store_number(message.get_value<float>(index, field.byte_offset), out);
    case PointCloud::kDoubleType:
      return store_number(message.get_value<double>(index, field.byte_offset), out);
    case PointCloud::kUnknownType:
      break;
    default:
      return false;
  }

  switch (field.storage_size) {
    case sizeof(uint8_t):
      return store_number(message.get_value<uint8_t>(index, field.byte_offset), out);
    case sizeof(int16_t):
      return store_number(message.get_value<int16_t>(index, field.byte_offset), out);
    case sizeof(float):
      return store_number(message.get_value<float>(index, field.byte_offset), out);
    case sizeof(double):
      return store_number(message.get_value<double>(index, field.byte_offset), out);
    default:
      return false;
  }
}

size_t MessageParser::collection_size(std::string_view collection) const noexcept {
  collection = normalize_collection(collection);

  if (type_ == Type::kPointCloud && collection == "data") {
    return get<PointCloud>()->size();
  }

  if (type_ == Type::kObjectArray && collection == "data") {
    return get<ObjectArray>()->count();
  }

  if (type_ == Type::kOccupancyGrid && collection == "data") {
    const auto& message = *get<OccupancyGrid>();

    if VUNLIKELY (message.cell_size() == 0) {
      return 0;
    }

    const size_t payload_count = message.size() / message.cell_size();
    const size_t declared_count = static_cast<size_t>(message.width()) * message.height();
    return std::min(payload_count, declared_count);
  }

  if (type_ == Type::kTensor) {
    const auto& message = *get<Tensor>();

    if (collection == "shape" || collection == "strides") {
      return message.rank();
    }

    if (collection == "data" && message.element_size() != 0) {
      return std::min<size_t>(message.num_elements(), message.size() / message.element_size());
    }
  }

  if (type_ == Type::kAudioFrame && collection == "data") {
    const auto& message = *get<AudioFrame>();
    const size_t element_size = audio_element_size(message.format());

    if VUNLIKELY (element_size == 0 || message.bit_depth() != element_size * 8U) {
      return 0;
    }

    return message.size() / element_size;
  }

  return 0;
}

std::vector<MessageParser::Field> MessageParser::fields() const {
  std::vector<Field> result;

  if VUNLIKELY (!valid()) {
    return result;
  }

  const auto add_text = [&result](std::string_view name) { result.push_back({std::string(name), ValueType::kString}); };

  const auto add_number = [&result](std::string_view name, ValueType type) {
    result.push_back({std::string(name), type});
  };

  const auto add_bytes = [&result](std::string_view name) { result.push_back({std::string(name), ValueType::kBytes}); };

  const auto add_time = [&result](std::string_view name) {
    Field field{std::string(name), ValueType::kUInt64};
    field.is_time = true;
    result.push_back(std::move(field));
  };

  const auto add_bool = [&result](std::string_view name) {
    Field field{std::string(name), ValueType::kUInt64};
    field.is_bool = true;
    result.push_back(std::move(field));
  };

  const auto add_enum = [&result](std::string_view name, EnumKind kind) {
    Field field{std::string(name), ValueType::kUInt64};
    field.enum_kind = kind;
    result.push_back(std::move(field));
  };

  const auto add_reserved = [&result](std::string_view name) {
    Field field{std::string(name), ValueType::kUInt64};
    field.is_reserved = true;
    result.push_back(std::move(field));
  };

  if (type_ != Type::kProxyData) {
    add_text("header.frame_id");
    add_number("header.seq", ValueType::kUInt64);
    add_reserved("header.reserved");
    add_time("header.time_meas");
    add_time("header.time_pub");
  }

  switch (type_) {
    case Type::kRawData:
      add_number("size", ValueType::kUInt64);
      add_reserved("reserved");
      add_bytes("data");
      break;

    case Type::kCameraFrame:
      add_number("channel", ValueType::kUInt64);
      add_number("height", ValueType::kUInt64);
      add_number("width", ValueType::kUInt64);
      add_number("freq", ValueType::kUInt64);
      add_enum("format", EnumKind::kEnumCameraFormat);
      add_enum("stream", EnumKind::kEnumCameraStream);
      add_number("size", ValueType::kUInt64);
      add_reserved("reserved");
      add_bytes("data");
      break;

    case Type::kPointCloud:
      add_number("size", ValueType::kUInt64);
      add_number("pack_size", ValueType::kUInt64);
      add_number("extent", ValueType::kUInt64);
      add_number("downsample", ValueType::kUInt64);
      add_bool("vertical");
      add_reserved("reserved_size");
      add_reserved("reserved");
      add_reserved("reserved2");
      add_reserved("reserved3");
      add_bytes("data");
      break;

    case Type::kProxyData:
      add_number("control_id", ValueType::kUInt64);
      add_number("mode", ValueType::kUInt64);
      add_number("timestamp", ValueType::kInt64);
      add_number("seq", ValueType::kInt64);
      add_number("schema", ValueType::kUInt64);
      add_text("url");
      add_text("ser");
      add_text("hostname");
      add_number("size", ValueType::kUInt64);
      add_reserved("reserved");
      add_reserved("reserved2");
      add_bytes("raw");
      break;

    case Type::kOccupancyGrid:
      add_text("map_id");
      add_number("width", ValueType::kUInt64);
      add_number("height", ValueType::kUInt64);
      add_number("channel", ValueType::kUInt64);
      add_number("freq", ValueType::kUInt64);
      add_number("valid_cell_count", ValueType::kUInt64);
      add_number("default_value", ValueType::kInt64);
      add_number("resolution", ValueType::kDouble);
      add_number("origin_x", ValueType::kDouble);
      add_number("origin_y", ValueType::kDouble);
      add_number("origin_z", ValueType::kDouble);
      add_number("origin_yaw", ValueType::kDouble);
      add_number("value_min", ValueType::kDouble);
      add_number("value_max", ValueType::kDouble);
      add_number("occupied_threshold", ValueType::kDouble);
      add_number("free_threshold", ValueType::kDouble);
      add_enum("cell_type", EnumKind::kEnumGridCellType);
      add_number("cell_size", ValueType::kUInt64);
      add_time("update_time_ns");
      add_number("size", ValueType::kUInt64);
      add_reserved("reserved");
      add_reserved("reserved2");
      add_reserved("reserved3");
      add_bytes("data");
      break;

    case Type::kTensor:
      add_text("name");
      add_text("model_id");
      add_text("layout");
      add_enum("dtype", EnumKind::kEnumTensorDataType);
      add_enum("device", EnumKind::kEnumTensorDevice);
      add_number("rank", ValueType::kUInt64);
      add_number("num_elements", ValueType::kUInt64);
      add_number("element_size", ValueType::kUInt64);
      add_number("batch_size", ValueType::kUInt64);
      add_number("channel", ValueType::kUInt64);
      add_number("freq", ValueType::kUInt64);
      add_number("quant_zero_point", ValueType::kInt64);
      add_number("quant_scale", ValueType::kDouble);
      add_time("update_time_ns");
      add_number("size", ValueType::kUInt64);
      add_reserved("reserved");
      add_reserved("reserved2");
      add_reserved("reserved3");
      add_bytes("data");
      break;

    case Type::kObjectArray:
      add_text("source_id");
      add_number("channel", ValueType::kUInt64);
      add_number("freq", ValueType::kUInt64);
      add_number("count", ValueType::kUInt64);
      add_number("pack_size", ValueType::kUInt64);
      add_time("update_time_ns");
      add_number("size", ValueType::kUInt64);
      add_reserved("reserved");
      add_reserved("reserved2");
      add_reserved("reserved3");
      add_reserved("reserved4");
      add_reserved("reserved5");
      add_bytes("data");
      break;

    case Type::kAudioFrame:
      add_text("codec");
      add_text("language");
      add_number("channel", ValueType::kUInt64);
      add_number("freq", ValueType::kUInt64);
      add_number("sample_rate", ValueType::kUInt64);
      add_number("num_samples", ValueType::kUInt64);
      add_number("num_channels", ValueType::kUInt64);
      add_number("bit_depth", ValueType::kUInt64);
      add_number("bitrate", ValueType::kUInt64);
      add_enum("format", EnumKind::kEnumAudioFormat);
      add_enum("layout", EnumKind::kEnumAudioLayout);
      add_number("duration_ns", ValueType::kUInt64);
      add_time("update_time_ns");
      add_number("size", ValueType::kUInt64);
      add_reserved("reserved");
      add_reserved("reserved2");
      add_bytes("data");
      break;
    default:
      break;
  }

  return result;
}

std::vector<MessageParser::Field> MessageParser::element_fields(std::string_view collection) const {
  collection = normalize_collection(collection);

  if (type_ == Type::kPointCloud && collection == "data") {
    return point_fields_;
  }

  if (type_ == Type::kTensor && collection == "data") {
    const auto& message = *get<Tensor>();
    return {{"value", tensor_value_type(message.dtype()), message.dtype(), message.element_size()}};
  }

  if (type_ == Type::kTensor && (collection == "shape" || collection == "strides")) {
    return {{"value", ValueType::kUInt64}};
  }

  if (type_ == Type::kOccupancyGrid && collection == "data") {
    const auto& message = *get<OccupancyGrid>();

    switch (message.cell_type()) {
      case OccupancyGrid::kCellInt8:
        return {{"value", ValueType::kInt64, message.cell_type(), message.cell_size()}};
      case OccupancyGrid::kCellUint8:
      case OccupancyGrid::kCellUint16:
        return {{"value", ValueType::kUInt64, message.cell_type(), message.cell_size()}};
      case OccupancyGrid::kCellFloat32:
        return {{"value", ValueType::kDouble, message.cell_type(), message.cell_size()}};
      default:
        return {};
    }
  }

  if (type_ == Type::kAudioFrame && collection == "data") {
    const auto& message = *get<AudioFrame>();

    switch (message.format()) {
      case AudioFrame::kFormatPcmU8:
        return {{"value", ValueType::kUInt64, message.format(), static_cast<uint16_t>(sizeof(uint8_t))}};
      case AudioFrame::kFormatPcmS16:
        return {{"value", ValueType::kInt64, message.format(), static_cast<uint16_t>(sizeof(int16_t))}};
      case AudioFrame::kFormatPcmS24:
        return {{"value", ValueType::kInt64, message.format(), 3}};
      case AudioFrame::kFormatPcmS32:
        return {{"value", ValueType::kInt64, message.format(), static_cast<uint16_t>(sizeof(int32_t))}};
      case AudioFrame::kFormatPcmF32:
        return {{"value", ValueType::kDouble, message.format(), static_cast<uint16_t>(sizeof(float))}};
      default:
        return {};
    }
  }

  if (type_ == Type::kObjectArray && collection == "data") {
    return {{"label", ValueType::kString},
            {"position[0]", ValueType::kDouble},
            {"position[1]", ValueType::kDouble},
            {"position[2]", ValueType::kDouble},
            {"yaw", ValueType::kDouble},
            {"size[0]", ValueType::kDouble},
            {"size[1]", ValueType::kDouble},
            {"size[2]", ValueType::kDouble},
            {"yaw_rate", ValueType::kDouble},
            {"velocity[0]", ValueType::kDouble},
            {"velocity[1]", ValueType::kDouble},
            {"velocity[2]", ValueType::kDouble},
            {"score", ValueType::kDouble},
            {"acceleration[0]", ValueType::kDouble},
            {"acceleration[1]", ValueType::kDouble},
            {"acceleration[2]", ValueType::kDouble},
            {"existence_probability", ValueType::kDouble},
            {"position_covariance[0]", ValueType::kDouble},
            {"position_covariance[1]", ValueType::kDouble},
            {"position_covariance[2]", ValueType::kDouble},
            {"position_covariance[3]", ValueType::kDouble},
            {"position_covariance[4]", ValueType::kDouble},
            {"position_covariance[5]", ValueType::kDouble},
            {"class_id", ValueType::kUInt64},
            {"track_id", ValueType::kUInt64},
            {"age", ValueType::kUInt64},
            {"num_observations", ValueType::kUInt64},
            {"motion_state", ValueType::kUInt64},
            {"source_type", ValueType::kUInt64},
            {"subtype_id", ValueType::kUInt64},
            {"reserved", ValueType::kUInt64}};
  }

  return {};
}

static std::string format_integer(const MessageParser::Value& value, bool hex) {
  if (const auto* number = std::get_if<int64_t>(&value)) {
    return hex ? Helpers::format_hex_number(*number) : std::to_string(*number);
  }

  if (const auto* number = std::get_if<uint64_t>(&value)) {
    return hex ? Helpers::format_hex_number(*number) : std::to_string(*number);
  }

  if (const auto* number = std::get_if<double>(&value)) {
    return std::to_string(*number);
  }

  return {};
}

static std::string_view enum_label(MessageParser::EnumKind kind, uint64_t value) {
  switch (kind) {
    case MessageParser::EnumKind::kEnumCameraFormat:
      return NameDetector::get_enum(static_cast<CameraFrame::Format>(value));
    case MessageParser::EnumKind::kEnumCameraStream:
      return NameDetector::get_enum(static_cast<CameraFrame::Stream>(value));
    case MessageParser::EnumKind::kEnumGridCellType:
      return NameDetector::get_enum(static_cast<OccupancyGrid::CellType>(value));
    case MessageParser::EnumKind::kEnumTensorDataType:
      return NameDetector::get_enum(static_cast<Tensor::DataType>(value));
    case MessageParser::EnumKind::kEnumTensorDevice:
      return NameDetector::get_enum(static_cast<Tensor::Device>(value));
    case MessageParser::EnumKind::kEnumAudioFormat:
      return NameDetector::get_enum(static_cast<AudioFrame::Format>(value));
    case MessageParser::EnumKind::kEnumAudioLayout:
      return NameDetector::get_enum(static_cast<AudioFrame::Layout>(value));
    default:
      return {};
  }
}

static std::string format_scalar(const MessageParser& parser, const MessageParser::Field& field,
                                 const MessageFormatOptions& options) {
  MessageParser::Value value;

  if VUNLIKELY (!parser.value(field.name, value)) {
    return {};
  }

  if (const auto* text = std::get_if<std::string>(&value)) {
    return *text;
  }

  if (field.is_bool) {
    const auto* number = std::get_if<uint64_t>(&value);
    return number != nullptr && *number != 0 ? "true" : "false";
  }

  if (field.enum_kind != MessageParser::EnumKind::kEnumNone) {
    const auto* number = std::get_if<uint64_t>(&value);
    const uint64_t raw = number != nullptr ? *number : 0;
    return options.enum_name ? std::string(enum_label(field.enum_kind, raw)) : std::to_string(raw);
  }

  if (field.is_time) {
    const auto* number = std::get_if<uint64_t>(&value);
    const uint64_t raw = number != nullptr ? *number : 0;

    if (options.date) {
      return Helpers::format_date(static_cast<int64_t>(raw));
    }

    return options.hex ? Helpers::format_hex_number(raw) : std::to_string(raw);
  }

  return format_integer(value, options.hex);
}

static std::string format_pointcloud_protocol(const MessageParser& parser) {
  static constexpr std::array<std::string_view, 12> kTypeNames = {
      "unknown", "bool", "int8", "uint8", "int16", "uint16", "int32", "uint32", "int64", "uint64", "float", "double"};

  const auto fields = parser.element_fields("data");

  uint64_t extent = 0;
  MessageParser::Value extent_value;

  if (parser.value("extent", extent_value)) {
    if (const auto* number = std::get_if<uint64_t>(&extent_value)) {
      extent = *number;
    }
  }

  std::string size_list;
  std::string name_list;
  std::string type_list;

  for (size_t i = 0; i < fields.size(); ++i) {
    if (i != 0) {
      size_list += ",";
      name_list += ",";
      type_list += ",";
    }

    const auto& field = fields[i];
    auto native_type = field.native_type;

    if (extent != 0 && i < 3 && native_type == PointCloud::kInt16Type && field.storage_size == sizeof(int16_t)) {
      native_type = PointCloud::kFloatType;
    }

    size_list += std::to_string(field.storage_size);
    name_list += field.name;
    type_list += native_type < kTypeNames.size() ? std::string(kTypeNames[native_type]) : "unknown";
  }

  std::string result = "protocol {\n";
  result += "  size_list: " + size_list + "\n";
  result += "  name_list: " + name_list + "\n";
  result += "  type_list: " + type_list + "\n";
  result += "}\n";

  return result;
}

static std::string format_tensor_shape(const MessageParser& parser) {
  std::string shape = "[";
  const size_t count = parser.collection_size("shape");

  for (size_t d = 0; d < count; ++d) {
    if (d != 0) {
      shape += ", ";
    }

    MessageParser::Value value;

    if (parser.value("shape", d, std::string_view{}, value)) {
      if (const auto* number = std::get_if<uint64_t>(&value)) {
        shape += std::to_string(*number);
      }
    }
  }

  shape += "]";

  return "shape: " + shape + "\n";
}

static std::string format_pointcloud_points(const MessageParser& parser, const MessageFormatOptions& options,
                                            bool* truncated) {
  const auto fields = parser.element_fields("data");
  const size_t total = parser.collection_size("data");
  const size_t count = std::min(total, options.max_elements);

  if (truncated != nullptr && total > options.max_elements) {
    *truncated = true;
  }

  std::string result;

  for (size_t i = 0; i < count; ++i) {
    result += "data[" + std::to_string(i) + "] {\n";

    for (const auto& field : fields) {
      MessageParser::Value value;
      std::string text;

      if (parser.value("data", i, field, value)) {
        if (field.is_bool) {
          const auto* number = std::get_if<uint64_t>(&value);
          text = number != nullptr && *number != 0 ? "true" : "false";
        } else {
          text = format_integer(value, options.hex);
        }
      }

      result += "  " + field.name + ": " + text + "\n";
    }

    result += "}\n";
  }

  return result;
}

std::string format_message(const MessageParser& parser, const MessageFormatOptions& options, bool* truncated) {
  if (truncated != nullptr) {
    *truncated = false;
  }

  std::string result;

  if VUNLIKELY (!parser.valid()) {
    return result;
  }

  const auto fields = parser.fields();
  const bool is_point_cloud = parser.type() == MessageParser::Type::kPointCloud;
  const bool is_tensor = parser.type() == MessageParser::Type::kTensor;

  std::string header;

  size_t index = 0;

  for (; index < fields.size(); ++index) {
    const auto& field = fields[index];

    if (!Helpers::has_startwith(field.name, "header.")) {
      break;
    }

    if (field.is_reserved) {
      continue;
    }

    header += "  " + field.name.substr(std::string_view("header.").size()) + ": " +
              format_scalar(parser, field, options) + "\n";
  }

  if (!header.empty()) {
    result += "header {\n";
    result += header;
    result += "}\n";
  }

  if (is_point_cloud) {
    result += format_pointcloud_protocol(parser);
  }

  for (; index < fields.size(); ++index) {
    const auto& field = fields[index];

    if (field.is_reserved) {
      continue;
    }

    if (field.type == MessageParser::ValueType::kBytes) {
      if (is_tensor) {
        result += format_tensor_shape(parser);
      }

      if (is_point_cloud) {
        if (options.expand_arrays) {
          result += format_pointcloud_points(parser, options, truncated);
        }
      } else {
        result += field.name + ": {...}\n";
      }

      continue;
    }

    result += field.name + ": " + format_scalar(parser, field, options) + "\n";
  }

  return result;
}

}  // namespace zerocopy

}  // namespace vlink
