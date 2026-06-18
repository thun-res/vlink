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

#include "dump_extract.h"

#include <vlink/base/helpers.h>

#include <charconv>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef VLINK_HAS_PROTOBUF_COMPILER

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcpp"
#endif

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/reflection.h>
#include <google/protobuf/text_format.h>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

static constexpr int kMaxSchemaImportDepth = 100;
static constexpr size_t kMaxSchemaDirEntries = 1000;

void import_protos(google::protobuf::compiler::Importer* importer, const std::filesystem::path& root_dir,
                   const std::filesystem::path& sub_dir, bool& has_import, int depth) {
  if VUNLIKELY (depth >= kMaxSchemaImportDepth) {
    return;
  }

  std::vector<std::filesystem::directory_entry> file_list;

  try {
    for (const auto& entry : std::filesystem::directory_iterator(sub_dir)) {
      file_list.emplace_back(entry);
    }
  } catch (std::filesystem::filesystem_error&) {
    return;
  }

  if VUNLIKELY (file_list.empty() || file_list.size() > kMaxSchemaDirEntries) {
    return;
  }

  for (const auto& file : file_list) {
    try {
      if (file.is_regular_file() && file.path().extension() == ".proto") {
#ifdef _WIN32
        auto relative_path = vlink::Helpers::path_to_string(std::filesystem::relative(file.path(), root_dir));
        std::replace(relative_path.begin(), relative_path.end(), '\\', '/');
#else
        auto relative_path = std::filesystem::relative(file.path(), root_dir).string();
#endif

        if VLIKELY (importer->Import(relative_path)) {
          has_import = true;
        }
      } else if (file.is_directory()) {
        import_protos(importer, root_dir, file.path(), has_import, depth + 1);
      }
    } catch (std::filesystem::filesystem_error&) {
      continue;
    }
  }
}

bool extract_proto_value(const google::protobuf::Message& message, const std::vector<std::string>& path_parts,
                         size_t depth, VariantType& result) {
  if VUNLIKELY (depth >= path_parts.size()) {
    return false;
  }

  const auto* descriptor = message.GetDescriptor();
  const auto* reflection = message.GetReflection();

  std::string field_name = path_parts[depth];
  int array_index = -1;

  auto bracket_pos = field_name.find('[');

  if VUNLIKELY (bracket_pos != std::string::npos) {
    auto close_pos = field_name.find(']', bracket_pos);

    if VLIKELY (close_pos != std::string::npos && close_pos > bracket_pos) {
      std::from_chars(field_name.data() + bracket_pos + 1, field_name.data() + close_pos, array_index);
      field_name = field_name.substr(0, bracket_pos);
    }
  }

  const auto* field = descriptor->FindFieldByName(field_name);

  if VUNLIKELY (!field) {
    return false;
  }

  bool is_leaf = (depth == path_parts.size() - 1);

  auto extract_scalar = [&field, &result](auto get_fn) -> bool {
    if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      return false;
    }

    result = get_fn(field->cpp_type());
    return true;
  };

  if (field->is_repeated()) {
    if VUNLIKELY (array_index < 0 || array_index >= reflection->FieldSize(message, field)) {
      return false;
    }

    if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      const auto& sub_msg = reflection->GetRepeatedMessage(message, field, array_index);

      if (is_leaf) {
        std::string text;
        bool ret = google::protobuf::TextFormat::PrintToString(sub_msg, &text);

        if VLIKELY (ret) {
          result = std::move(text);
        } else {
          result = "";
        }

        return true;
      }

      return extract_proto_value(sub_msg, path_parts, depth + 1, result);
    }

    if (!is_leaf) {
      return false;
    }

    return extract_scalar([&](int cpp_type) -> VariantType {
      using google::protobuf::FieldDescriptor;

      switch (cpp_type) {
        case FieldDescriptor::CPPTYPE_INT32:
          return static_cast<int64_t>(reflection->GetRepeatedInt32(message, field, array_index));
        case FieldDescriptor::CPPTYPE_INT64:
          return reflection->GetRepeatedInt64(message, field, array_index);
        case FieldDescriptor::CPPTYPE_UINT32:
          return static_cast<int64_t>(reflection->GetRepeatedUInt32(message, field, array_index));
        case FieldDescriptor::CPPTYPE_UINT64:
          return reflection->GetRepeatedUInt64(message, field, array_index);
        case FieldDescriptor::CPPTYPE_DOUBLE:
          return reflection->GetRepeatedDouble(message, field, array_index);
        case FieldDescriptor::CPPTYPE_FLOAT:
          return static_cast<double>(reflection->GetRepeatedFloat(message, field, array_index));
        case FieldDescriptor::CPPTYPE_BOOL:
          return static_cast<int64_t>(reflection->GetRepeatedBool(message, field, array_index));
        case FieldDescriptor::CPPTYPE_ENUM:
          return static_cast<int64_t>(reflection->GetRepeatedEnumValue(message, field, array_index));
        case FieldDescriptor::CPPTYPE_STRING:
          if (field->type() == FieldDescriptor::TYPE_BYTES) {
            return vlink::Bytes::from_string(reflection->GetRepeatedString(message, field, array_index));
          }

          return reflection->GetRepeatedString(message, field, array_index);
        default:
          return int64_t{0};
      }
    });
  }

  if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
    const auto& sub_msg = reflection->GetMessage(message, field);

    if (is_leaf) {
      std::string text;
      bool ret = google::protobuf::TextFormat::PrintToString(sub_msg, &text);

      if VLIKELY (ret) {
        result = std::move(text);
      } else {
        result = "";
      }

      return true;
    }

    return extract_proto_value(sub_msg, path_parts, depth + 1, result);
  }

  if (!is_leaf) {
    return false;
  }

  return extract_scalar([&](int cpp_type) -> VariantType {
    using google::protobuf::FieldDescriptor;

    switch (cpp_type) {
      case FieldDescriptor::CPPTYPE_INT32:
        return static_cast<int64_t>(reflection->GetInt32(message, field));
      case FieldDescriptor::CPPTYPE_INT64:
        return reflection->GetInt64(message, field);
      case FieldDescriptor::CPPTYPE_UINT32:
        return static_cast<int64_t>(reflection->GetUInt32(message, field));
      case FieldDescriptor::CPPTYPE_UINT64:
        return reflection->GetUInt64(message, field);
      case FieldDescriptor::CPPTYPE_DOUBLE:
        return reflection->GetDouble(message, field);
      case FieldDescriptor::CPPTYPE_FLOAT:
        return static_cast<double>(reflection->GetFloat(message, field));
      case FieldDescriptor::CPPTYPE_BOOL:
        return static_cast<int64_t>(reflection->GetBool(message, field));
      case FieldDescriptor::CPPTYPE_ENUM:
        return static_cast<int64_t>(reflection->GetEnumValue(message, field));
      case FieldDescriptor::CPPTYPE_STRING:
        if (field->type() == FieldDescriptor::TYPE_BYTES) {
          return vlink::Bytes::from_string(reflection->GetString(message, field));
        }

        return reflection->GetString(message, field);
      default:
        return int64_t{0};
    }
  });
}

#endif

bool match_zerocopy_type(const std::string& ser, std::string_view type_name) {
  if (ser == type_name) {
    return true;
  }

  if (ser.size() > type_name.size()) {
    auto pos = ser.size() - type_name.size();
    char before = ser[pos - 1];

    if ((before == '.' || before == ':' || before == '/') && ser.compare(pos, type_name.size(), type_name) == 0) {
      return true;
    }
  }

  return false;
}

std::string format_zerocopy_header(const vlink::zerocopy::Header& header) {
  std::string s;
  s += "header {\n";
  s += "  frame_id: " + std::string(header.frame_id_view()) + "\n";
  s += "  seq: " + std::to_string(header.seq) + "\n";
  s += "  time_meas: " + vlink::Helpers::format_date(header.time_meas) + "\n";
  s += "  time_pub: " + vlink::Helpers::format_date(header.time_pub) + "\n";
  s += "}\n";
  return s;
}

std::string format_raw_data(const vlink::zerocopy::RawData& raw_data) {
  std::string s = format_zerocopy_header(raw_data.header);
  s += "size: " + std::to_string(raw_data.size()) + "\n";
  s += "data: {...}\n";
  return s;
}

std::string format_camera_frame(const vlink::zerocopy::CameraFrame& frame) {
  std::string s = format_zerocopy_header(frame.header);
  s += "channel: " + std::to_string(frame.channel()) + "\n";
  s += "height: " + std::to_string(frame.height()) + "\n";
  s += "width: " + std::to_string(frame.width()) + "\n";
  s += "freq: " + std::to_string(frame.freq()) + "\n";
  s += "format: " + std::string(vlink::NameDetector::get_enum(frame.format())) + "\n";
  s += "stream: " + std::string(vlink::NameDetector::get_enum(frame.stream())) + "\n";
  s += "size: " + std::to_string(frame.size()) + "\n";
  s += "data: {...}\n";
  return s;
}

std::string format_point_cloud(const vlink::zerocopy::PointCloud& pc) {
  std::string s = format_zerocopy_header(pc.header);
  s += "protocol {\n";
  s += "  size_list: " + pc.get_protocol_size_str() + "\n";
  s += "  name_list: " + pc.get_protocol_name_str() + "\n";
  s += "  type_list: " + pc.get_protocol_type_str() + "\n";
  s += "}\n";
  s += "size: " + std::to_string(pc.size()) + "\n";
  s += "pack_size: " + std::to_string(pc.pack_size()) + "\n";
  s += "extent: " + std::to_string(pc.get_extent()) + "\n";
  s += "downsample: " + std::to_string(pc.get_downsample()) + "\n";
  s += "vertical: " + std::to_string(pc.get_vertical()) + "\n";
  return s;
}

std::string format_occupancy_grid(const vlink::zerocopy::OccupancyGrid& og) {
  std::string s = format_zerocopy_header(og.header);
  s += "map_id: " + std::string(og.map_id()) + "\n";
  s += "channel: " + std::to_string(og.channel()) + "\n";
  s += "freq: " + std::to_string(og.freq()) + "\n";
  s += "width: " + std::to_string(og.width()) + "\n";
  s += "height: " + std::to_string(og.height()) + "\n";
  s += "resolution: " + std::to_string(og.resolution()) + "\n";
  s += "origin_x: " + std::to_string(og.origin_x()) + "\n";
  s += "origin_y: " + std::to_string(og.origin_y()) + "\n";
  s += "origin_z: " + std::to_string(og.origin_z()) + "\n";
  s += "origin_yaw: " + std::to_string(og.origin_yaw()) + "\n";
  s += "cell_type: " + std::string(vlink::NameDetector::get_enum(og.cell_type())) + "\n";
  s += "default_value: " + std::to_string(og.default_value()) + "\n";
  s += "value_min: " + std::to_string(og.value_min()) + "\n";
  s += "value_max: " + std::to_string(og.value_max()) + "\n";
  s += "occupied_threshold: " + std::to_string(og.occupied_threshold()) + "\n";
  s += "free_threshold: " + std::to_string(og.free_threshold()) + "\n";
  s += "valid_cell_count: " + std::to_string(og.valid_cell_count()) + "\n";
  s += "update_time_ns: " + std::to_string(og.update_time_ns()) + "\n";
  s += "size: " + std::to_string(og.size()) + "\n";
  s += "data: {...}\n";
  return s;
}

std::string format_tensor(const vlink::zerocopy::Tensor& tensor) {
  std::string s = format_zerocopy_header(tensor.header);
  s += "name: " + std::string(tensor.name()) + "\n";
  s += "model_id: " + std::string(tensor.model_id()) + "\n";
  s += "layout: " + std::string(tensor.layout()) + "\n";
  s += "dtype: " + std::string(vlink::NameDetector::get_enum(tensor.dtype())) + "\n";
  s += "device: " + std::string(vlink::NameDetector::get_enum(tensor.device())) + "\n";
  s += "rank: " + std::to_string(static_cast<uint32_t>(tensor.rank())) + "\n";
  s += "num_elements: " + std::to_string(tensor.num_elements()) + "\n";
  s += "element_size: " + std::to_string(static_cast<uint32_t>(tensor.element_size())) + "\n";
  s += "batch_size: " + std::to_string(tensor.batch_size()) + "\n";

  std::string shape_str = "[";

  for (uint8_t i = 0; i < tensor.rank(); ++i) {
    if (i > 0) {
      shape_str += ", ";
    }

    shape_str += std::to_string(tensor.shape_at(i));
  }

  shape_str += "]";
  s += "shape: " + shape_str + "\n";
  s += "quant_scale: " + std::to_string(tensor.quant_scale()) + "\n";
  s += "quant_zero_point: " + std::to_string(tensor.quant_zero_point()) + "\n";
  s += "channel: " + std::to_string(tensor.channel()) + "\n";
  s += "freq: " + std::to_string(tensor.freq()) + "\n";
  s += "update_time_ns: " + std::to_string(tensor.update_time_ns()) + "\n";
  s += "size: " + std::to_string(tensor.size()) + "\n";
  s += "data: {...}\n";
  return s;
}

std::string format_object_array(const vlink::zerocopy::ObjectArray& arr) {
  std::string s = format_zerocopy_header(arr.header);
  s += "source_id: " + std::string(arr.source_id()) + "\n";
  s += "channel: " + std::to_string(arr.channel()) + "\n";
  s += "freq: " + std::to_string(arr.freq()) + "\n";
  s += "count: " + std::to_string(arr.count()) + "\n";
  s += "pack_size: " + std::to_string(arr.pack_size()) + "\n";
  s += "update_time_ns: " + std::to_string(arr.update_time_ns()) + "\n";
  s += "data: {...}\n";
  return s;
}

std::string format_audio_frame(const vlink::zerocopy::AudioFrame& frame) {
  std::string s = format_zerocopy_header(frame.header);
  s += "channel: " + std::to_string(frame.channel()) + "\n";
  s += "freq: " + std::to_string(frame.freq()) + "\n";
  s += "sample_rate: " + std::to_string(frame.sample_rate()) + "\n";
  s += "num_samples: " + std::to_string(frame.num_samples()) + "\n";
  s += "num_channels: " + std::to_string(static_cast<uint32_t>(frame.num_channels())) + "\n";
  s += "bit_depth: " + std::to_string(static_cast<uint32_t>(frame.bit_depth())) + "\n";
  s += "bitrate: " + std::to_string(frame.bitrate()) + "\n";
  s += "format: " + std::string(vlink::NameDetector::get_enum(frame.format())) + "\n";
  s += "layout: " + std::string(vlink::NameDetector::get_enum(frame.layout())) + "\n";
  s += "codec: " + std::string(frame.codec()) + "\n";
  s += "language: " + std::string(frame.language()) + "\n";
  s += "duration_ns: " + std::to_string(frame.duration_ns()) + "\n";
  s += "update_time_ns: " + std::to_string(frame.update_time_ns()) + "\n";
  s += "size: " + std::to_string(frame.size()) + "\n";
  s += "data: {...}\n";
  return s;
}

bool extract_zerocopy_header_value(const vlink::zerocopy::Header& header, const std::string& field,
                                   VariantType& result) {
  if (field == "header.seq") {
    result = static_cast<int64_t>(header.seq);
    return true;
  }

  if (field == "header.time_meas") {
    result = static_cast<int64_t>(header.time_meas);
    return true;
  }

  if (field == "header.time_pub") {
    result = static_cast<int64_t>(header.time_pub);
    return true;
  }

  if (field == "header.frame_id") {
    result = std::string(header.frame_id_view());
    return true;
  }

  return false;
}

bool extract_zerocopy_value(const std::string& ser, const vlink::Bytes& bytes, const std::string& field,
                            VariantType& result) {
  if (match_zerocopy_type(ser, "RawData")) {
    vlink::zerocopy::RawData raw_data;

    if VUNLIKELY (!vlink::Serializer::convert(bytes, raw_data)) {
      return false;
    }

    if (extract_zerocopy_header_value(raw_data.header, field, result)) {
      return true;
    }

    if (field == "size") {
      result = static_cast<int64_t>(raw_data.size());
      return true;
    }

    if (field == "data") {
      result = std::string("<binary:" + std::to_string(raw_data.size()) + ">");
      return true;
    }

    return false;
  }

  if (match_zerocopy_type(ser, "CameraFrame")) {
    vlink::zerocopy::CameraFrame frame;

    if VUNLIKELY (!vlink::Serializer::convert(bytes, frame)) {
      return false;
    }

    if (extract_zerocopy_header_value(frame.header, field, result)) {
      return true;
    }

    if (field == "width") {
      result = static_cast<int64_t>(frame.width());
      return true;
    }

    if (field == "height") {
      result = static_cast<int64_t>(frame.height());
      return true;
    }

    if (field == "channel") {
      result = static_cast<int64_t>(frame.channel());
      return true;
    }

    if (field == "freq") {
      result = static_cast<int64_t>(frame.freq());
      return true;
    }

    if (field == "format") {
      result = static_cast<int64_t>(frame.format());
      return true;
    }

    if (field == "stream") {
      result = static_cast<int64_t>(frame.stream());
      return true;
    }

    if (field == "size") {
      result = static_cast<int64_t>(frame.size());
      return true;
    }

    if (field == "data") {
      result = std::string("<binary:" + std::to_string(frame.size()) + ">");
      return true;
    }

    return false;
  }

  if (match_zerocopy_type(ser, "PointCloud")) {
    vlink::zerocopy::PointCloud pc;

    if VUNLIKELY (!vlink::Serializer::convert(bytes, pc)) {
      return false;
    }

    if (extract_zerocopy_header_value(pc.header, field, result)) {
      return true;
    }

    if (field == "size") {
      result = static_cast<int64_t>(pc.size());
      return true;
    }

    if (field == "pack_size") {
      result = static_cast<int64_t>(pc.pack_size());
      return true;
    }

    if (field == "extent") {
      result = static_cast<int64_t>(pc.get_extent());
      return true;
    }

    if (field == "downsample") {
      result = static_cast<int64_t>(pc.get_downsample());
      return true;
    }

    if (field == "vertical") {
      result = static_cast<int64_t>(pc.get_vertical());
      return true;
    }

    if (vlink::Helpers::has_startwith(field, "data")) {
      auto pos_left = field.find('[');
      auto pos_right = field.find(']');
      int array_pos = -1;

      if (pos_left != std::string::npos && pos_right != std::string::npos && pos_right > pos_left) {
        std::from_chars(field.data() + pos_left + 1, field.data() + pos_right, array_pos);
      }

      if (array_pos < 0 || static_cast<size_t>(array_pos) >= pc.size()) {
        return false;
      }

      std::string value_name = (pos_right + 2 < field.size()) ? field.substr(pos_right + 2) : "";

      if (value_name.empty()) {
        return false;
      }

      vlink::zerocopy::PointCloud::KeyList key_list;
      auto key_map = pc.get_key_map(&key_list);

      const bool has_compressed_xyz = (pc.get_extent() != 0);

      for (size_t key_index = 0; key_index < key_list.size(); ++key_index) {
        const auto& key = key_list[key_index];

        if (key.name != value_name) {
          continue;
        }

        if (has_compressed_xyz && key_index < 3) {
          vlink::zerocopy::PointCloud::Vector3f v3f;
          pc.get_value_v3f(v3f, array_pos);

          if (key_index == 0) {
            result = static_cast<double>(v3f.x);
          } else if (key_index == 1) {
            result = static_cast<double>(v3f.y);
          } else {
            result = static_cast<double>(v3f.z);
          }

          return true;
        }

        if (key.type != vlink::zerocopy::PointCloud::kUnknownType) {
          switch (key.type) {
            case vlink::zerocopy::PointCloud::kFloatType:
              result = static_cast<double>(pc.get_value<float>(array_pos, key_map, key.name));
              return true;
            case vlink::zerocopy::PointCloud::kDoubleType:
              result = pc.get_value<double>(array_pos, key_map, key.name);
              return true;
            case vlink::zerocopy::PointCloud::kInt8Type:
              result = static_cast<int64_t>(pc.get_value<int8_t>(array_pos, key_map, key.name));
              return true;
            case vlink::zerocopy::PointCloud::kUint8Type:
            case vlink::zerocopy::PointCloud::kBoolType:
              result = static_cast<int64_t>(pc.get_value<uint8_t>(array_pos, key_map, key.name));
              return true;
            case vlink::zerocopy::PointCloud::kInt16Type:
              result = static_cast<int64_t>(pc.get_value<int16_t>(array_pos, key_map, key.name));
              return true;
            case vlink::zerocopy::PointCloud::kUint16Type:
              result = static_cast<int64_t>(pc.get_value<uint16_t>(array_pos, key_map, key.name));
              return true;
            case vlink::zerocopy::PointCloud::kInt32Type:
              result = static_cast<int64_t>(pc.get_value<int32_t>(array_pos, key_map, key.name));
              return true;
            case vlink::zerocopy::PointCloud::kUint32Type:
              result = static_cast<int64_t>(pc.get_value<uint32_t>(array_pos, key_map, key.name));
              return true;
            case vlink::zerocopy::PointCloud::kInt64Type:
              result = pc.get_value<int64_t>(array_pos, key_map, key.name);
              return true;
            case vlink::zerocopy::PointCloud::kUint64Type:
              result = pc.get_value<uint64_t>(array_pos, key_map, key.name);
              return true;
            default:
              return false;
          }
        } else {
          if (key.size == 1) {
            result = static_cast<int64_t>(pc.get_value<uint8_t>(array_pos, key_map, key.name));
          } else if (key.size == 2) {
            result = static_cast<int64_t>(pc.get_value<int16_t>(array_pos, key_map, key.name));
          } else if (key.size == 4) {
            result = static_cast<double>(pc.get_value<float>(array_pos, key_map, key.name));
          } else if (key.size == 8) {
            result = pc.get_value<double>(array_pos, key_map, key.name);
          } else {
            return false;
          }

          return true;
        }
      }
    }

    return false;
  }

  if (match_zerocopy_type(ser, "OccupancyGrid")) {
    vlink::zerocopy::OccupancyGrid og;

    if VUNLIKELY (!vlink::Serializer::convert(bytes, og)) {
      return false;
    }

    if (extract_zerocopy_header_value(og.header, field, result)) {
      return true;
    }

    if (field == "map_id") {
      result = std::string(og.map_id());
      return true;
    }

    if (field == "channel") {
      result = static_cast<int64_t>(og.channel());
      return true;
    }

    if (field == "freq") {
      result = static_cast<int64_t>(og.freq());
      return true;
    }

    if (field == "width") {
      result = static_cast<int64_t>(og.width());
      return true;
    }

    if (field == "height") {
      result = static_cast<int64_t>(og.height());
      return true;
    }

    if (field == "resolution") {
      result = static_cast<double>(og.resolution());
      return true;
    }

    if (field == "origin_x") {
      result = static_cast<double>(og.origin_x());
      return true;
    }

    if (field == "origin_y") {
      result = static_cast<double>(og.origin_y());
      return true;
    }

    if (field == "origin_z") {
      result = static_cast<double>(og.origin_z());
      return true;
    }

    if (field == "origin_yaw") {
      result = static_cast<double>(og.origin_yaw());
      return true;
    }

    if (field == "cell_type") {
      result = static_cast<int64_t>(og.cell_type());
      return true;
    }

    if (field == "default_value") {
      result = static_cast<int64_t>(og.default_value());
      return true;
    }

    if (field == "value_min") {
      result = static_cast<double>(og.value_min());
      return true;
    }

    if (field == "value_max") {
      result = static_cast<double>(og.value_max());
      return true;
    }

    if (field == "occupied_threshold") {
      result = static_cast<double>(og.occupied_threshold());
      return true;
    }

    if (field == "free_threshold") {
      result = static_cast<double>(og.free_threshold());
      return true;
    }

    if (field == "valid_cell_count") {
      result = static_cast<int64_t>(og.valid_cell_count());
      return true;
    }

    if (field == "update_time_ns") {
      result = static_cast<int64_t>(og.update_time_ns());
      return true;
    }

    if (field == "size") {
      result = static_cast<int64_t>(og.size());
      return true;
    }

    if (field == "data") {
      result = std::string("<binary:" + std::to_string(og.size()) + ">");
      return true;
    }

    return false;
  }

  if (match_zerocopy_type(ser, "Tensor")) {
    vlink::zerocopy::Tensor tensor;

    if VUNLIKELY (!vlink::Serializer::convert(bytes, tensor)) {
      return false;
    }

    if (extract_zerocopy_header_value(tensor.header, field, result)) {
      return true;
    }

    if (field == "name") {
      result = std::string(tensor.name());
      return true;
    }

    if (field == "model_id") {
      result = std::string(tensor.model_id());
      return true;
    }

    if (field == "layout") {
      result = std::string(tensor.layout());
      return true;
    }

    if (field == "dtype") {
      result = static_cast<int64_t>(tensor.dtype());
      return true;
    }

    if (field == "device") {
      result = static_cast<int64_t>(tensor.device());
      return true;
    }

    if (field == "rank") {
      result = static_cast<int64_t>(tensor.rank());
      return true;
    }

    if (field == "num_elements") {
      result = static_cast<int64_t>(tensor.num_elements());
      return true;
    }

    if (field == "element_size") {
      result = static_cast<int64_t>(tensor.element_size());
      return true;
    }

    if (field == "batch_size") {
      result = static_cast<int64_t>(tensor.batch_size());
      return true;
    }

    if (field == "quant_scale") {
      result = static_cast<double>(tensor.quant_scale());
      return true;
    }

    if (field == "quant_zero_point") {
      result = static_cast<int64_t>(tensor.quant_zero_point());
      return true;
    }

    if (field == "channel") {
      result = static_cast<int64_t>(tensor.channel());
      return true;
    }

    if (field == "freq") {
      result = static_cast<int64_t>(tensor.freq());
      return true;
    }

    if (field == "update_time_ns") {
      result = static_cast<int64_t>(tensor.update_time_ns());
      return true;
    }

    if (field == "size") {
      result = static_cast<int64_t>(tensor.size());
      return true;
    }

    if (field == "data") {
      result = std::string("<binary:" + std::to_string(tensor.size()) + ">");
      return true;
    }

    if (field == "shape") {
      std::string shape_str = "[";

      for (uint8_t i = 0; i < tensor.rank(); ++i) {
        if (i > 0) {
          shape_str += ", ";
        }

        shape_str += std::to_string(tensor.shape_at(i));
      }

      shape_str += "]";
      result = shape_str;
      return true;
    }

    if (vlink::Helpers::has_startwith(field, "shape[")) {
      auto pos_left = field.find('[');
      auto pos_right = field.find(']');
      int dim_pos = -1;

      if (pos_left != std::string::npos && pos_right != std::string::npos && pos_right > pos_left) {
        std::from_chars(field.data() + pos_left + 1, field.data() + pos_right, dim_pos);
      }

      if (dim_pos < 0 || dim_pos >= tensor.rank()) {
        return false;
      }

      result = static_cast<int64_t>(tensor.shape_at(static_cast<uint8_t>(dim_pos)));
      return true;
    }

    return false;
  }

  if (match_zerocopy_type(ser, "ObjectArray")) {
    vlink::zerocopy::ObjectArray arr;

    if VUNLIKELY (!vlink::Serializer::convert(bytes, arr)) {
      return false;
    }

    if (extract_zerocopy_header_value(arr.header, field, result)) {
      return true;
    }

    if (field == "source_id") {
      result = std::string(arr.source_id());
      return true;
    }

    if (field == "channel") {
      result = static_cast<int64_t>(arr.channel());
      return true;
    }

    if (field == "freq") {
      result = static_cast<int64_t>(arr.freq());
      return true;
    }

    if (field == "count") {
      result = static_cast<int64_t>(arr.count());
      return true;
    }

    if (field == "pack_size") {
      result = static_cast<int64_t>(arr.pack_size());
      return true;
    }

    if (field == "update_time_ns") {
      result = static_cast<int64_t>(arr.update_time_ns());
      return true;
    }

    if (field == "data") {
      result = std::string("<objects:" + std::to_string(arr.count()) + ">");
      return true;
    }

    return false;
  }

  if (match_zerocopy_type(ser, "AudioFrame")) {
    vlink::zerocopy::AudioFrame frame;

    if VUNLIKELY (!vlink::Serializer::convert(bytes, frame)) {
      return false;
    }

    if (extract_zerocopy_header_value(frame.header, field, result)) {
      return true;
    }

    if (field == "channel") {
      result = static_cast<int64_t>(frame.channel());
      return true;
    }

    if (field == "freq") {
      result = static_cast<int64_t>(frame.freq());
      return true;
    }

    if (field == "sample_rate") {
      result = static_cast<int64_t>(frame.sample_rate());
      return true;
    }

    if (field == "num_samples") {
      result = static_cast<int64_t>(frame.num_samples());
      return true;
    }

    if (field == "num_channels") {
      result = static_cast<int64_t>(frame.num_channels());
      return true;
    }

    if (field == "bit_depth") {
      result = static_cast<int64_t>(frame.bit_depth());
      return true;
    }

    if (field == "bitrate") {
      result = static_cast<int64_t>(frame.bitrate());
      return true;
    }

    if (field == "format") {
      result = static_cast<int64_t>(frame.format());
      return true;
    }

    if (field == "layout") {
      result = static_cast<int64_t>(frame.layout());
      return true;
    }

    if (field == "codec") {
      result = std::string(frame.codec());
      return true;
    }

    if (field == "language") {
      result = std::string(frame.language());
      return true;
    }

    if (field == "duration_ns") {
      result = static_cast<int64_t>(frame.duration_ns());
      return true;
    }

    if (field == "update_time_ns") {
      result = static_cast<int64_t>(frame.update_time_ns());
      return true;
    }

    if (field == "size") {
      result = static_cast<int64_t>(frame.size());
      return true;
    }

    if (field == "data") {
      result = std::string("<binary:" + std::to_string(frame.size()) + ">");
      return true;
    }

    return false;
  }

  return false;
}

std::string format_zerocopy_message(const std::string& ser, const vlink::Bytes& bytes) {
  if (match_zerocopy_type(ser, "RawData")) {
    vlink::zerocopy::RawData raw_data;

    if (vlink::Serializer::convert(bytes, raw_data)) {
      return format_raw_data(raw_data);
    }
  } else if (match_zerocopy_type(ser, "CameraFrame")) {
    vlink::zerocopy::CameraFrame frame;

    if (vlink::Serializer::convert(bytes, frame)) {
      return format_camera_frame(frame);
    }
  } else if (match_zerocopy_type(ser, "PointCloud")) {
    vlink::zerocopy::PointCloud pc;

    if (vlink::Serializer::convert(bytes, pc)) {
      return format_point_cloud(pc);
    }
  } else if (match_zerocopy_type(ser, "OccupancyGrid")) {
    vlink::zerocopy::OccupancyGrid og;

    if (vlink::Serializer::convert(bytes, og)) {
      return format_occupancy_grid(og);
    }
  } else if (match_zerocopy_type(ser, "Tensor")) {
    vlink::zerocopy::Tensor tensor;

    if (vlink::Serializer::convert(bytes, tensor)) {
      return format_tensor(tensor);
    }
  } else if (match_zerocopy_type(ser, "ObjectArray")) {
    vlink::zerocopy::ObjectArray arr;

    if (vlink::Serializer::convert(bytes, arr)) {
      return format_object_array(arr);
    }
  } else if (match_zerocopy_type(ser, "AudioFrame")) {
    vlink::zerocopy::AudioFrame frame;

    if (vlink::Serializer::convert(bytes, frame)) {
      return format_audio_frame(frame);
    }
  }

  return "<unsupported zerocopy type>";
}

vlink::Bytes extract_zerocopy_binary(const std::string& ser, const vlink::Bytes& bytes, const std::string& field) {
  if (field == "data") {
    if (match_zerocopy_type(ser, "RawData")) {
      vlink::zerocopy::RawData raw_data;

      if (vlink::Serializer::convert(bytes, raw_data)) {
        return vlink::Bytes::shallow_copy(const_cast<uint8_t*>(raw_data.data()), raw_data.size());
      }
    } else if (match_zerocopy_type(ser, "CameraFrame")) {
      vlink::zerocopy::CameraFrame frame;

      if (vlink::Serializer::convert(bytes, frame)) {
        return vlink::Bytes::shallow_copy(const_cast<uint8_t*>(frame.data()), frame.size());
      }
    } else if (match_zerocopy_type(ser, "PointCloud")) {
      vlink::zerocopy::PointCloud pc;

      if (vlink::Serializer::convert(bytes, pc)) {
        // NOLINTNEXTLINE(readability-redundant-casting)
        auto payload_size = static_cast<size_t>(pc.size()) * static_cast<size_t>(pc.pack_size());
        return vlink::Bytes::shallow_copy(const_cast<uint8_t*>(pc.get_internal_data()), payload_size);
      }
    } else if (match_zerocopy_type(ser, "OccupancyGrid")) {
      vlink::zerocopy::OccupancyGrid og;

      if (vlink::Serializer::convert(bytes, og)) {
        return vlink::Bytes::shallow_copy(const_cast<uint8_t*>(og.data()), og.size());
      }
    } else if (match_zerocopy_type(ser, "Tensor")) {
      vlink::zerocopy::Tensor tensor;

      if (vlink::Serializer::convert(bytes, tensor)) {
        return vlink::Bytes::shallow_copy(const_cast<uint8_t*>(tensor.data()), tensor.size());
      }
    } else if (match_zerocopy_type(ser, "ObjectArray")) {
      vlink::zerocopy::ObjectArray arr;

      if (vlink::Serializer::convert(bytes, arr)) {
        // NOLINTNEXTLINE(readability-redundant-casting)
        auto payload_size = static_cast<size_t>(arr.count()) * static_cast<size_t>(arr.pack_size());
        return vlink::Bytes::shallow_copy(const_cast<uint8_t*>(arr.data()), payload_size);
      }
    } else if (match_zerocopy_type(ser, "AudioFrame")) {
      vlink::zerocopy::AudioFrame frame;

      if (vlink::Serializer::convert(bytes, frame)) {
        return vlink::Bytes::shallow_copy(const_cast<uint8_t*>(frame.data()), frame.size());
      }
    }
  }

  return {};
}

bool write_pcd_file(const std::string& file_path, const vlink::zerocopy::PointCloud& pc) {
  if VUNLIKELY (!pc.is_valid()) {
    return false;
  }

  vlink::zerocopy::PointCloud::KeyList key_list;
  (void)pc.get_key_map(&key_list);

  if VUNLIKELY (key_list.empty()) {
    return false;
  }

  const bool compressed = (pc.get_extent() != 0);

  if VUNLIKELY (compressed && key_list.size() < 3) {
    return false;
  }

  std::ofstream file(file_path, std::ios::binary);

  if VUNLIKELY (!file.is_open()) {
    return false;
  }

  std::string fields_str;
  std::string size_str;
  std::string type_str;
  std::string count_str;

  for (size_t i = 0; i < key_list.size(); ++i) {
    const auto& key = key_list[i];

    if (i > 0) {
      fields_str += " ";
      size_str += " ";
      type_str += " ";
      count_str += " ";
    }

    const bool xyz_compressed = (compressed && i < 3);

    fields_str += key.name;
    size_str += std::to_string(xyz_compressed ? static_cast<uint8_t>(sizeof(float)) : key.size);
    count_str += "1";

    if (xyz_compressed) {
      type_str += "F";
      continue;
    }

    switch (key.type) {
      case vlink::zerocopy::PointCloud::kFloatType:
      case vlink::zerocopy::PointCloud::kDoubleType:
        type_str += "F";
        break;
      case vlink::zerocopy::PointCloud::kInt8Type:
      case vlink::zerocopy::PointCloud::kInt16Type:
      case vlink::zerocopy::PointCloud::kInt32Type:
      case vlink::zerocopy::PointCloud::kInt64Type:
        type_str += "I";
        break;
      case vlink::zerocopy::PointCloud::kUint8Type:
      case vlink::zerocopy::PointCloud::kUint16Type:
      case vlink::zerocopy::PointCloud::kUint32Type:
      case vlink::zerocopy::PointCloud::kUint64Type:
      case vlink::zerocopy::PointCloud::kBoolType:
        type_str += "U";
        break;
      default:
        if (key.size == 4 || key.size == 8) {
          type_str += "F";
        } else {
          type_str += "U";
        }

        break;
    }
  }

  file << "# .PCD v0.7 - Point Cloud Data file format\n";
  file << "VERSION 0.7\n";
  file << "FIELDS " << fields_str << "\n";
  file << "SIZE " << size_str << "\n";
  file << "TYPE " << type_str << "\n";
  file << "COUNT " << count_str << "\n";
  file << "WIDTH " << pc.size() << "\n";
  file << "HEIGHT 1\n";
  file << "VIEWPOINT 0 0 0 1 0 0 0\n";
  file << "POINTS " << pc.size() << "\n";
  file << "DATA binary\n";

  const auto* data = pc.get_internal_data();

  if (data) {
    if (compressed) {
      const auto pack_size = pc.pack_size();

      std::vector<uint16_t> src_offsets(key_list.size(), 0);
      uint16_t src_offset = 0;

      for (size_t i = 0; i < key_list.size(); ++i) {
        src_offsets[i] = src_offset;
        src_offset += key_list[i].size;
      }

      for (size_t i = 0; i < pc.size(); ++i) {
        const auto* src = data + (i * pack_size);

        float x = 0.0F;
        float y = 0.0F;
        float z = 0.0F;
        pc.get_value_v3f(x, y, z, i);

        file.write(reinterpret_cast<const char*>(&x), static_cast<std::streamsize>(sizeof(float)));
        file.write(reinterpret_cast<const char*>(&y), static_cast<std::streamsize>(sizeof(float)));
        file.write(reinterpret_cast<const char*>(&z), static_cast<std::streamsize>(sizeof(float)));

        for (size_t j = 3; j < key_list.size(); ++j) {
          file.write(reinterpret_cast<const char*>(src + src_offsets[j]),
                     static_cast<std::streamsize>(key_list[j].size));
        }
      }
    } else {
      file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(pc.size() * pc.pack_size()));
    }
  }

  file.close();
  return file.good();
}
