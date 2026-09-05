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

#include "./parse_extract.h"

#include <vlink/base/helpers.h>

#include <charconv>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

#include "./parse_path.h"

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
      if VUNLIKELY (file_list.size() >= kMaxSchemaDirEntries) {
        return;
      }

      file_list.emplace_back(entry);
    }
  } catch (std::filesystem::filesystem_error&) {
    return;
  }

  if VUNLIKELY (file_list.empty()) {
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

  if (bracket_pos != std::string::npos) {
    auto close_pos = field_name.find(']', bracket_pos);

    if VUNLIKELY (close_pos == std::string::npos || close_pos != field_name.size() - 1 ||
                  close_pos == bracket_pos + 1) {
      return false;
    }

    const auto* index_begin = field_name.data() + bracket_pos + 1;
    const auto* index_end = field_name.data() + close_pos;
    const auto [ptr, ec] = std::from_chars(index_begin, index_end, array_index);

    if VUNLIKELY (ec != std::errc() || ptr != index_end || array_index < 0) {
      return false;
    }

    field_name.resize(bracket_pos);
  }

  const auto* field = descriptor->FindFieldByName(field_name);

  if VUNLIKELY (!field) {
    return false;
  }

  if VUNLIKELY (array_index >= 0 && !field->is_repeated()) {
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

    return extract_scalar([&message, &reflection, &field, &array_index](int cpp_type) -> VariantType {
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

  return extract_scalar([&reflection, &message, &field](int cpp_type) -> VariantType {
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

bool extract_zerocopy_value(const std::string& ser, const vlink::Bytes& bytes, const std::string& field,
                            VariantType& result) {
  vlink::zerocopy::MessageParser message_parser;

  if VUNLIKELY (!message_parser.parse(ser, bytes)) {
    return false;
  }

  return extract_zerocopy_value(message_parser, field, result);
}

bool extract_zerocopy_value(const vlink::zerocopy::MessageParser& parser, const std::string& field,
                            VariantType& result) {
  vlink::zerocopy::MessageParser::Value value;

  if VUNLIKELY (!parser.value(field, value)) {
    return false;
  }

  std::visit(
      [&result](const auto& item) {
        using Item = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Item, vlink::Bytes>) {
          result = std::string("<binary:") + std::to_string(item.size()) + ">";
        } else {
          result = item;
        }
      },
      value);
  return true;
}

std::string format_zerocopy_message(const std::string& ser, const vlink::Bytes& bytes) {
  vlink::zerocopy::MessageParser message_parser;
  const auto zerocopy_type = vlink::zerocopy::MessageParser::detect_type(ser);

  if VUNLIKELY (zerocopy_type == vlink::zerocopy::MessageParser::Type::kUnknown) {
    return "<unsupported zerocopy type>";
  }

  if VUNLIKELY (!message_parser.parse(zerocopy_type, bytes)) {
    return "<malformed zerocopy payload>";
  }

  vlink::zerocopy::MessageFormatOptions format_options;
  format_options.date = true;
  format_options.enum_name = true;
  format_options.expand_arrays = false;

  return vlink::zerocopy::format_message(message_parser, format_options);
}

vlink::Bytes extract_zerocopy_binary(const vlink::zerocopy::MessageParser& parser, const std::string& field) {
  vlink::zerocopy::MessageParser::Value value;

  if VUNLIKELY (!parser.value(field, value)) {
    return {};
  }

  auto* binary = std::get_if<vlink::Bytes>(&value);

  if VUNLIKELY (binary == nullptr) {
    return {};
  }

  return std::move(*binary);
}

template <typename T>
static void write_pcd_scalar(std::ofstream& file, T value) {
  file.write(reinterpret_cast<const char*>(&value), static_cast<std::streamsize>(sizeof(value)));
}

bool write_pcd_file(const std::string& file_path, const vlink::zerocopy::PointCloud& point_cloud) {
  if VUNLIKELY (!point_cloud.is_valid()) {
    return false;
  }

  vlink::zerocopy::PointCloud::KeyList key_list;
  (void)point_cloud.get_key_map(&key_list);

  if VUNLIKELY (key_list.empty()) {
    return false;
  }

  const bool compressed = point_cloud.get_extent() != 0;

  if VUNLIKELY (compressed && key_list.size() < 3) {
    return false;
  }

  std::ofstream file(vlink::parse::utf8_to_path(file_path), std::ios::binary);

  if VUNLIKELY (!file.is_open()) {
    return false;
  }

  std::string fields_str;
  std::string size_str;
  std::string type_str;
  std::string count_str;

  for (size_t i = 0; i < key_list.size(); ++i) {
    const auto& field = key_list[i];

    if VUNLIKELY (field.size == 0) {
      return false;
    }

    if (i > 0) {
      fields_str += " ";
      size_str += " ";
      type_str += " ";
      count_str += " ";
    }

    const bool compressed_coordinate = compressed && i < 3;
    fields_str += field.name;
    size_str += std::to_string(compressed_coordinate ? sizeof(float) : field.size);
    count_str += "1";

    if (compressed_coordinate || field.type == vlink::zerocopy::PointCloud::kFloatType ||
        field.type == vlink::zerocopy::PointCloud::kDoubleType ||
        (field.type == vlink::zerocopy::PointCloud::kUnknownType && field.size >= sizeof(float))) {
      type_str += "F";
    } else if (field.type == vlink::zerocopy::PointCloud::kInt8Type ||
               field.type == vlink::zerocopy::PointCloud::kInt16Type ||
               field.type == vlink::zerocopy::PointCloud::kInt32Type ||
               field.type == vlink::zerocopy::PointCloud::kInt64Type ||
               (field.type == vlink::zerocopy::PointCloud::kUnknownType && field.size == sizeof(int16_t))) {
      type_str += "I";
    } else {
      type_str += "U";
    }
  }

  file << "# .PCD v0.7 - Point Cloud Data file format\n";
  file << "VERSION 0.7\n";
  file << "FIELDS " << fields_str << "\n";
  file << "SIZE " << size_str << "\n";
  file << "TYPE " << type_str << "\n";
  file << "COUNT " << count_str << "\n";
  file << "WIDTH " << point_cloud.size() << "\n";
  file << "HEIGHT 1\n";
  file << "VIEWPOINT 0 0 0 1 0 0 0\n";
  file << "POINTS " << point_cloud.size() << "\n";
  file << "DATA binary\n";

  const auto* data = point_cloud.get_internal_data();

  if VUNLIKELY (data == nullptr) {
    return false;
  }

  if (!compressed) {
    file.write(reinterpret_cast<const char*>(data),
               static_cast<std::streamsize>(point_cloud.size() * point_cloud.pack_size()));
  } else {
    const auto pack_size = point_cloud.pack_size();
    std::vector<uint16_t> offsets(key_list.size(), 0);
    uint16_t offset = 0;

    for (size_t i = 0; i < key_list.size(); ++i) {
      offsets[i] = offset;
      offset += key_list[i].size;
    }

    for (size_t point = 0; point < point_cloud.size(); ++point) {
      const auto* source = data + point * pack_size;
      float x = 0.0F;
      float y = 0.0F;
      float z = 0.0F;

      if VUNLIKELY (!point_cloud.get_value_v3f(x, y, z, point)) {
        return false;
      }

      write_pcd_scalar(file, x);
      write_pcd_scalar(file, y);
      write_pcd_scalar(file, z);

      for (size_t field = 3; field < key_list.size(); ++field) {
        file.write(reinterpret_cast<const char*>(source + offsets[field]),
                   static_cast<std::streamsize>(key_list[field].size));
      }
    }
  }

  file.close();
  return file.good();
}
