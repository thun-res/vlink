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

#include "foxglove_converter.h"

#include <vlink/base/functional.h>
#include <vlink/base/helpers.h>

#include <CameraCalibration.fbs.hpp>
#include <CameraCalibration_bfbs.fbs.hpp>
#include <CircleAnnotation.fbs.hpp>
#include <Color.fbs.hpp>
#include <CompressedImage.fbs.hpp>
#include <CompressedImage_bfbs.fbs.hpp>
#include <CompressedVideo.fbs.hpp>
#include <CompressedVideo_bfbs.fbs.hpp>
#include <CubePrimitive.fbs.hpp>
#include <FrameTransform.fbs.hpp>
#include <FrameTransform_bfbs.fbs.hpp>
#include <FrameTransforms.fbs.hpp>
#include <FrameTransforms_bfbs.fbs.hpp>
#include <GeoJSON.fbs.hpp>
#include <GeoJSON_bfbs.fbs.hpp>
#include <Grid.fbs.hpp>
#include <Grid_bfbs.fbs.hpp>
#include <ImageAnnotations.fbs.hpp>
#include <ImageAnnotations_bfbs.fbs.hpp>
#include <JointState.fbs.hpp>
#include <JointStates.fbs.hpp>
#include <JointStates_bfbs.fbs.hpp>
#include <LaserScan.fbs.hpp>
#include <LaserScan_bfbs.fbs.hpp>
#include <LocationFix.fbs.hpp>
#include <LocationFix_bfbs.fbs.hpp>
#include <LocationFixes.fbs.hpp>
#include <LocationFixes_bfbs.fbs.hpp>
#include <Log.fbs.hpp>
#include <Log_bfbs.fbs.hpp>
#include <PackedElementField.fbs.hpp>
#include <Point2.fbs.hpp>
#include <Point3.fbs.hpp>
#include <Point3InFrame.fbs.hpp>
#include <Point3InFrame_bfbs.fbs.hpp>
#include <PointCloud.fbs.hpp>
#include <PointCloud_bfbs.fbs.hpp>
#include <PointsAnnotation.fbs.hpp>
#include <Pose.fbs.hpp>
#include <PoseInFrame.fbs.hpp>
#include <PoseInFrame_bfbs.fbs.hpp>
#include <PosesInFrame.fbs.hpp>
#include <PosesInFrame_bfbs.fbs.hpp>
#include <Quaternion.fbs.hpp>
#include <RawAudio.fbs.hpp>
#include <RawAudio_bfbs.fbs.hpp>
#include <RawImage.fbs.hpp>
#include <RawImage_bfbs.fbs.hpp>
#include <SceneEntity.fbs.hpp>
#include <SceneUpdate.fbs.hpp>
#include <SceneUpdate_bfbs.fbs.hpp>
#include <TextAnnotation.fbs.hpp>
#include <Vector2.fbs.hpp>
#include <Vector3.fbs.hpp>
#include <VoxelGrid.fbs.hpp>
#include <VoxelGrid_bfbs.fbs.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

[[maybe_unused]] static bool is_flatbuffers_schema_encoding(std::string_view encoding) {
  std::string normalized{encoding};

  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

  return normalized == "flatbuffers" || normalized == "flatbuffer" || normalized == "fbs" || normalized == "bfbs";
}

constexpr std::string_view kFoxgloveFlatbufferEncoding = "flatbuffer";

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
flatbuffers::Offset<flatbuffers::Vector<uint8_t>> FoxgloveConverter::create_proto_repeated_byte_vector(
    flatbuffers::FlatBufferBuilder& builder, const google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field, const google::protobuf::Reflection& ref) {
  if VUNLIKELY (!field.is_repeated()) {
    return 0;
  }

  int count = ref.FieldSize(msg, &field);

  if VUNLIKELY (count <= 0) {
    return 0;
  }

  thread_local Bytes scratch;

  if VUNLIKELY (!scratch.resize(static_cast<size_t>(count))) {
    return 0;
  }

  auto* dst = scratch.data();

  if VUNLIKELY (!dst) {
    return 0;
  }

  for (int i = 0; i < count; ++i) {
    switch (field.cpp_type()) {
      case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
        dst[i] = static_cast<uint8_t>(ref.GetRepeatedUInt32(msg, &field, i));
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
        dst[i] = static_cast<uint8_t>(ref.GetRepeatedInt32(msg, &field, i));
        break;
      default:
        return 0;
    }
  }

  return builder.CreateVector(dst, static_cast<size_t>(count));
}
#ifdef VLINK_HAS_FBS_COMPILER
template <typename Resolver>
bool resolve_thread_local_fbs_schema(const std::string& ser, const void* owner, Resolver&& resolver,
                                     const reflection::Schema*& out_schema) {
  struct ThreadLocalFbsSchemaCache final {
    const void* owner{nullptr};
    std::string ser;
    std::string schema_data;
    const reflection::Schema* schema{nullptr};
  };

  thread_local ThreadLocalFbsSchemaCache cache;

  if VUNLIKELY (cache.schema == nullptr || cache.owner != owner || cache.ser != ser) {
    cache.schema_data.clear();

    if VUNLIKELY (!resolver(ser, cache.schema_data)) {
      cache.owner = nullptr;
      cache.ser.clear();
      cache.schema = nullptr;
      return false;
    }

    cache.owner = owner;
    cache.ser = ser;
    cache.schema = get_verified_fbs_schema(ser, cache.schema_data);
  }

  out_schema = cache.schema;
  return out_schema != nullptr;
}
#endif

FoxgloveConverter::FoxgloveConverter(const Config& config) : config_(config) {
  Bytes::init_memory_pool();
  init_proto_resolver();
  init_convert_plugin();

#ifdef VLINK_HAS_FBS_COMPILER
  init_fbs_resolver();
#endif

  load_mappings();
}

FoxgloveConverter::~FoxgloveConverter() = default;

bool FoxgloveConverter::init_proto_resolver() {
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
      MLOG_W("Proto directory does not exist: {}", config_.proto_dir);
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
      } else {
        MLOG_W("No .proto files found in: {}", config_.proto_dir);
      }
    }
  }
#endif

  return has_resolver;
}

bool FoxgloveConverter::init_convert_plugin() {
  return load_convert_plugin(config_.convert_plugin_path, config_.convert_plugin_config, convert_plugin_loader_,
                             convert_plugin_);
}

const google::protobuf::Descriptor* FoxgloveConverter::find_proto_descriptor(const std::string& proto_name) {
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

std::unique_ptr<google::protobuf::Message> FoxgloveConverter::deserialize_proto_message(const std::string& ser,
                                                                                        const Bytes& raw) {
  const auto* desc = find_proto_descriptor(ser);

  if VUNLIKELY (!desc) {
    MLOG_W("Descriptor not found: {}", ser);
    return nullptr;
  }

  const google::protobuf::Message* prototype = nullptr;

#ifdef VLINK_HAS_PROTO_COMPILER

  if VLIKELY (disk_factory_ && imported_proto_descriptors_.find(ser) != imported_proto_descriptors_.end()) {
    prototype = disk_factory_->GetPrototype(desc);
  }
#endif

  if VUNLIKELY (!prototype && schema_interface_) {
    prototype = proto_factory_.GetPrototype(desc);
  }

  if VUNLIKELY (!prototype) {
    MLOG_W("Failed to get prototype for: {}", ser);
    return nullptr;
  }

  std::unique_ptr<google::protobuf::Message> msg(prototype->New());

  if VUNLIKELY (raw.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    MLOG_W("Protobuf too large: {} bytes", raw.size());
    return nullptr;
  }

  if VUNLIKELY (!msg->ParseFromArray(raw.data(), static_cast<int>(raw.size()))) {
    MLOG_W("Failed to parse protobuf message: {}", ser);
    return nullptr;
  }

  return msg;
}

::foxglove::Time FoxgloveConverter::make_timestamp_from_us(uint64_t us) {
  auto sec = static_cast<uint32_t>(us / 1000000ULL);
  auto nsec = static_cast<uint32_t>((us % 1000000ULL) * 1000);
  return {sec, nsec};
}

::foxglove::Time FoxgloveConverter::make_timestamp_from_ns(uint64_t ns) {
  auto sec = static_cast<uint32_t>(ns / 1000000000ULL);
  auto nsec = static_cast<uint32_t>(ns % 1000000000ULL);
  return {sec, nsec};
}

#ifdef VLINK_HAS_FBS_COMPILER

bool FoxgloveConverter::init_fbs_resolver() {
  bool has_resolver = schema_interface_ != nullptr;

  if VUNLIKELY (config_.fbs_dir.empty()) {
    return has_resolver;
  }

  auto fbs_path = std::filesystem::path(config_.fbs_dir);
  std::error_code ec;

  if VUNLIKELY (!std::filesystem::exists(fbs_path, ec) || ec) {
    MLOG_W("FBS directory does not exist: {}", config_.fbs_dir);
    return has_resolver;
  }

  std::vector<std::filesystem::path> fbs_files;
  scan_fbs_files(fbs_path, fbs_files);

  if VUNLIKELY (fbs_files.empty()) {
    MLOG_W("No .fbs files found in: {}", config_.fbs_dir);
    return has_resolver;
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
        MLOG_W("Failed to parse FBS: {}: {}", Helpers::path_to_string(fbs_file), parser->error_);
        continue;
      }
    } else {
      const char* full_dirs[] = {root_dir_str.c_str(), sub_dir_str.c_str(), nullptr};

      if VUNLIKELY (!parser->Parse(schema_file.c_str(), full_dirs)) {
        MLOG_W("Failed to parse FBS: {}: {}", Helpers::path_to_string(fbs_file), parser->error_);
        continue;
      }
    }

    std::vector<std::string> type_names;

    for (auto* def : parser->structs_.vec) {
      if VUNLIKELY (!def || def->generated) {
        continue;
      }

      auto type_name = def->name;

      if VLIKELY (fbs_parsers_.find(type_name) == fbs_parsers_.end()) {
        type_names.emplace_back(type_name);
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

  return has_resolver || !fbs_parsers_.empty();
}

bool FoxgloveConverter::find_fbs_parser_locked(const std::string& fbs_ser) {
  if VLIKELY (fbs_parsers_.find(fbs_ser) != fbs_parsers_.end()) {
    return true;
  }

  if VUNLIKELY (fbs_not_found_.find(fbs_ser) != fbs_not_found_.end()) {
    return false;
  }

  if VLIKELY (schema_interface_) {
    auto schema = schema_interface_->search_schema(fbs_ser, SchemaType::kFlatbuffers);
    if VLIKELY (schema.schema_type == SchemaType::kFlatbuffers && !schema.data.empty()) {
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

bool FoxgloveConverter::resolve_custom_fbs_schema(const std::string& fbs_ser, std::string& schema_data) {
  std::lock_guard lock(mtx_);

  auto cache_key = "custom_fbs:" + fbs_ser;
  auto cache_iter = schema_cache_.find(cache_key);

  if VLIKELY (cache_iter != schema_cache_.end()) {
    schema_data = cache_iter->second;
    return true;
  }

  if VUNLIKELY (!schema_interface_) {
    return false;
  }

  auto schema = schema_interface_->search_schema(fbs_ser, SchemaType::kFlatbuffers);

  if VUNLIKELY (schema.schema_type != SchemaType::kFlatbuffers || schema.data.empty()) {
    return false;
  }

  schema_data.assign(reinterpret_cast<const char*>(schema.data.data()), schema.data.size());
  schema_cache_[cache_key] = schema_data;
  return true;
}

double FoxgloveConverter::get_fbs_double(const flatbuffers::Table& table, const reflection::Object& obj,
                                         const std::string& field_name, const FieldMapping& mapping) {
  const auto* field = find_fbs_field(obj, field_name);

  if VLIKELY (field) {
    switch (field->type()->base_type()) {
      case reflection::Float:
      case reflection::Double:
        return flatbuffers::GetAnyFieldF(table, *field);
      case reflection::Byte:
      case reflection::Short:
      case reflection::Int:
      case reflection::Long:
      case reflection::UByte:
      case reflection::UShort:
      case reflection::UInt:
      case reflection::ULong:
      case reflection::Bool:
        return static_cast<double>(flatbuffers::GetAnyFieldI(table, *field));
      default:
        break;
    }
  }

  double default_value = 0.0;

  if VLIKELY (try_parse_numeric_default(mapping, default_value)) {
    return default_value;
  }

  return 0.0;
}

std::string FoxgloveConverter::get_fbs_string(const flatbuffers::Table& table, const reflection::Object& obj,
                                              const std::string& field_name, const FieldMapping& mapping,
                                              const reflection::Schema* schema) {
  if VUNLIKELY (!mapping.expression.empty() && schema != nullptr) {
    return format_expression_string(evaluate_expression_with_fbs(mapping.expression, table, obj, *schema));
  }

  const auto* field = find_fbs_field(obj, field_name);

  if VLIKELY (field && field->type()->base_type() == reflection::String) {
    return flatbuffers::GetAnyFieldS(table, *field, nullptr);
  }

  return mapping.has_default_value ? mapping.default_value : std::string{};
}

// NOLINTNEXTLINE(google-readability-function-size)
FoxgloveMessage FoxgloveConverter::convert_fbs_mapping(const FoxgloveMapping& mapping, const std::string& ser,
                                                       const Bytes& raw) {
  const reflection::Schema* schema = nullptr;

  if VUNLIKELY (!resolve_thread_local_fbs_schema(
                    ser, this,
                    [this](const std::string& type_name, std::string& schema_data) {
                      return resolve_custom_fbs_schema(type_name, schema_data);
                    },
                    schema)) {
    FoxgloveMessage result;
    return result;
  }

  if VUNLIKELY (!schema || !schema->root_table()) {
    FoxgloveMessage result;
    return result;
  }

  if VUNLIKELY (!verify_fbs_payload(*schema, raw, ser, "Foxglove mapping")) {
    FoxgloveMessage result;
    return result;
  }

  const auto* root_table = flatbuffers::GetAnyRoot(raw.data());

  if VUNLIKELY (!root_table) {
    FoxgloveMessage result;
    return result;
  }

  const auto& obj = *schema->root_table();

  int64_t fbs_timestamp_ns = -1;

  if VLIKELY (!mapping.timestamp_field.empty()) {
    fbs_timestamp_ns =
        extract_fbs_timestamp_ns(*root_table, obj, *schema, mapping.timestamp_field, mapping.timestamp_unit);
  }

  auto fbs_get_double = [schema](const flatbuffers::Table& tbl, const reflection::Object& o, const std::string& src,
                                 const std::string& expr = {}) -> double {
    if VUNLIKELY (!expr.empty()) {
      return evaluate_expression_with_fbs(expr, tbl, o, *schema);
    }

    if VUNLIKELY (has_nested_field_path(src)) {
      return safe_nested_fbs_double(tbl, o, *schema, src);
    }

    FieldMapping empty_fm;
    return get_fbs_double(tbl, o, src, empty_fm);
  };

  auto fbs_get_string = [schema](const flatbuffers::Table& tbl, const reflection::Object& o, const std::string& src,
                                 const std::string& def) -> std::string {
    if VUNLIKELY (has_nested_field_path(src)) {
      bool found = false;
      auto val = resolve_nested_fbs_string(tbl, o, *schema, src, &found);
      return found ? val : def;
    }

    FieldMapping fm;
    fm.default_value = def;
    fm.has_default_value = true;
    fm.default_value_is_string = true;
    return get_fbs_string(tbl, o, src, fm, schema);
  };

  if (mapping.schema == "foxglove.LocationFix") {
    FoxgloveMessage result;
    thread_local flatbuffers::FlatBufferBuilder builder(4096);
    builder.Clear();

    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    uint64_t timestamp_us = 0;
    uint64_t timestamp_ns = 0;
    std::string frame_id;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "timestamp") {
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "frame_id") {
        frame_id = fbs_get_string(*root_table, obj, fm.source, fm.default_value);
      } else if (fm.target == "latitude") {
        latitude = fbs_get_double(*root_table, obj, fm.source, fm.expression);
      } else if (fm.target == "longitude") {
        longitude = fbs_get_double(*root_table, obj, fm.source, fm.expression);
      } else if (fm.target == "altitude") {
        altitude = fbs_get_double(*root_table, obj, fm.source, fm.expression);
      }
    }

    auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
    auto fid = builder.CreateString(frame_id);
    auto loc = ::foxglove::CreateLocationFix(builder, &ts, fid, latitude, longitude, altitude);
    builder.Finish(loc);

    result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
    result.success = true;
    result.schema_name = "foxglove.LocationFix";
    result.encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.timestamp_ns = fbs_timestamp_ns;
    return result;
  }

  if (mapping.schema == "foxglove.PoseInFrame") {
    FoxgloveMessage result;
    thread_local flatbuffers::FlatBufferBuilder builder(4096);
    builder.Clear();

    uint64_t timestamp_us = 0;
    uint64_t timestamp_ns = 0;
    std::string frame_id;
    double qx = 0.0;
    double qy = 0.0;
    double qz = 0.0;
    double qw = 1.0;
    double position_x = 0.0;
    double position_y = 0.0;
    double position_z = 0.0;
    bool has_euler = false;
    double euler_roll = 0.0;
    double euler_pitch = 0.0;
    double euler_yaw = 0.0;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "timestamp") {
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "frame_id") {
        frame_id = fbs_get_string(*root_table, obj, fm.source, fm.default_value);
      } else if (fm.target == "pose" || fm.target == "pose_euler") {
        const flatbuffers::Table* target_tbl = nullptr;
        const reflection::Object* target_obj_ptr = nullptr;
        std::string target_field_name;

        if (resolve_fbs_parent_field_path(*root_table, obj, *schema, fm.source, target_tbl, target_obj_ptr,
                                          target_field_name)) {
          const auto* target_field = find_fbs_field(*target_obj_ptr, target_field_name);

          if (target_field && target_field->type()->base_type() == reflection::Obj) {
            const auto* sub_table = flatbuffers::GetFieldT(*target_tbl, *target_field);

            if (sub_table && schema->objects()) {
              const auto* sub_obj = schema->objects()->Get(static_cast<uint32_t>(target_field->type()->index()));

              if (sub_obj) {
                FieldMapping empty_fm;

                if (fm.target == "pose") {
                  qx = get_fbs_double(*sub_table, *sub_obj, "x", empty_fm);
                  qy = get_fbs_double(*sub_table, *sub_obj, "y", empty_fm);
                  qz = get_fbs_double(*sub_table, *sub_obj, "z", empty_fm);
                  qw = get_fbs_double(*sub_table, *sub_obj, "w", empty_fm);
                } else {
                  euler_roll = get_fbs_double(*sub_table, *sub_obj, "x", empty_fm);
                  euler_pitch = get_fbs_double(*sub_table, *sub_obj, "y", empty_fm);
                  euler_yaw = get_fbs_double(*sub_table, *sub_obj, "z", empty_fm);
                  has_euler = true;
                }
              }
            }
          }
        }
      } else if (fm.target == "position_x") {
        position_x = fbs_get_double(*root_table, obj, fm.source, fm.expression);
      } else if (fm.target == "position_y") {
        position_y = fbs_get_double(*root_table, obj, fm.source, fm.expression);
      } else if (fm.target == "position_z") {
        position_z = fbs_get_double(*root_table, obj, fm.source, fm.expression);
      }
    }

    if (has_euler) {
      double cr = std::cos(euler_roll * 0.5);
      double sr = std::sin(euler_roll * 0.5);
      double cp = std::cos(euler_pitch * 0.5);
      double sp = std::sin(euler_pitch * 0.5);
      double cy = std::cos(euler_yaw * 0.5);
      double sy = std::sin(euler_yaw * 0.5);

      qw = cr * cp * cy + sr * sp * sy;
      qx = sr * cp * cy - cr * sp * sy;
      qy = cr * sp * cy + sr * cp * sy;
      qz = cr * cp * sy - sr * sp * cy;
    }

    if VUNLIKELY (qx == 0.0 && qy == 0.0 && qz == 0.0 && qw == 0.0) {
      qw = 1.0;
    }

    auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
    auto fid = builder.CreateString(frame_id);

    auto orientation = ::foxglove::CreateQuaternion(builder, qx, qy, qz, qw);
    auto position = ::foxglove::CreateVector3(builder, position_x, position_y, position_z);
    auto pose = ::foxglove::CreatePose(builder, position, orientation);
    auto pif = ::foxglove::CreatePoseInFrame(builder, &ts, fid, pose);
    builder.Finish(pif);

    result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
    result.success = true;
    result.schema_name = "foxglove.PoseInFrame";
    result.encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.timestamp_ns = fbs_timestamp_ns;
    return result;
  }

  if (mapping.schema == "foxglove.SceneUpdate") {
    FoxgloveMessage result;
    thread_local flatbuffers::FlatBufferBuilder builder(16 * 1024);
    builder.Clear();

    uint64_t timestamp_us = 0;
    uint64_t timestamp_ns = 0;
    std::string frame_id = "base_link";

    std::string entity_sub_items;
    std::string entity_x_src;
    std::string entity_y_src;
    std::string entity_z_src;
    std::string entity_w_src;
    std::string entity_l_src;
    std::string entity_h_src;
    std::string entity_heading_src;
    std::string entity_x_expr;
    std::string entity_y_expr;
    std::string entity_z_expr;
    std::string entity_w_expr;
    std::string entity_l_expr;
    std::string entity_h_expr;
    std::string entity_heading_expr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "timestamp") {
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "frame_id") {
        frame_id = fbs_get_string(*root_table, obj, fm.source, fm.default_value);
      } else if (fm.target == "entity_sub_items") {
        entity_sub_items = fm.source;
      } else if (fm.target == "entity_x") {
        entity_x_src = fm.source;
        entity_x_expr = fm.expression;
      } else if (fm.target == "entity_y") {
        entity_y_src = fm.source;
        entity_y_expr = fm.expression;
      } else if (fm.target == "entity_z") {
        entity_z_src = fm.source;
        entity_z_expr = fm.expression;
      } else if (fm.target == "entity_width") {
        entity_w_src = fm.source;
        entity_w_expr = fm.expression;
      } else if (fm.target == "entity_length") {
        entity_l_src = fm.source;
        entity_l_expr = fm.expression;
      } else if (fm.target == "entity_height") {
        entity_h_src = fm.source;
        entity_h_expr = fm.expression;
      } else if (fm.target == "entity_heading") {
        entity_heading_src = fm.source;
        entity_heading_expr = fm.expression;
      }
    }

    bool has_entity_fields = !entity_x_src.empty() || !entity_y_src.empty() || !entity_z_src.empty();

    auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
    std::vector<flatbuffers::Offset<::foxglove::SceneEntity>> entity_offsets;

    auto build_fbs_cube = [&entity_offsets, &fbs_get_double, &frame_id, &has_entity_fields, &schema,
                           &entity_heading_expr, &entity_heading_src, &entity_h_expr, &entity_h_src, &entity_l_expr,
                           &entity_l_src, &entity_w_expr, &entity_w_src, &entity_x_expr, &entity_x_src, &entity_y_expr,
                           &entity_y_src, &entity_z_expr, &entity_z_src,
                           &ts](const flatbuffers::Table& item_tbl, const reflection::Object& item_obj, int idx,
                                const std::string& parent_id) {
      double px = 0.0;
      double py = 0.0;
      double pz = 0.0;
      double sx = 1.0;
      double sy = 1.0;
      double sz = 1.0;
      double heading = 0.0;

      FieldMapping empty_fm;

      if (has_entity_fields) {
        if (!entity_x_src.empty()) {
          px = fbs_get_double(item_tbl, item_obj, entity_x_src, entity_x_expr);
        }

        if (!entity_y_src.empty()) {
          py = fbs_get_double(item_tbl, item_obj, entity_y_src, entity_y_expr);
        }

        if (!entity_z_src.empty()) {
          pz = fbs_get_double(item_tbl, item_obj, entity_z_src, entity_z_expr);
        }

        if (!entity_w_src.empty()) {
          auto v = fbs_get_double(item_tbl, item_obj, entity_w_src, entity_w_expr);

          if (v != 0.0) {
            sx = v;
          }
        }

        if (!entity_l_src.empty()) {
          auto v = fbs_get_double(item_tbl, item_obj, entity_l_src, entity_l_expr);

          if (v != 0.0) {
            sy = v;
          }
        }

        if (!entity_h_src.empty()) {
          auto v = fbs_get_double(item_tbl, item_obj, entity_h_src, entity_h_expr);

          if (v != 0.0) {
            sz = v;
          }
        }

        if (!entity_heading_src.empty()) {
          heading = fbs_get_double(item_tbl, item_obj, entity_heading_src, entity_heading_expr);
        }
      } else {
        px = get_fbs_double(item_tbl, item_obj, "x", empty_fm);
        py = get_fbs_double(item_tbl, item_obj, "y", empty_fm);
        pz = get_fbs_double(item_tbl, item_obj, "z", empty_fm);

        if (px == 0.0 && py == 0.0 && pz == 0.0) {
          px = get_fbs_double(item_tbl, item_obj, "cx", empty_fm);
          py = get_fbs_double(item_tbl, item_obj, "cy", empty_fm);
          pz = get_fbs_double(item_tbl, item_obj, "cz", empty_fm);
        }

        if (px == 0.0 && py == 0.0 && pz == 0.0) {
          const auto* pos_field = find_fbs_field(item_obj, "position");

          if (pos_field && pos_field->type()->base_type() == reflection::Obj) {
            const auto* pos_tbl = flatbuffers::GetFieldT(item_tbl, *pos_field);

            if (pos_tbl && schema->objects()) {
              const auto* pos_obj = schema->objects()->Get(static_cast<uint32_t>(pos_field->type()->index()));

              if (pos_obj) {
                px = get_fbs_double(*pos_tbl, *pos_obj, "x", empty_fm);
                py = get_fbs_double(*pos_tbl, *pos_obj, "y", empty_fm);
                pz = get_fbs_double(*pos_tbl, *pos_obj, "z", empty_fm);
              }
            }
          }
        }

        auto w_val = get_fbs_double(item_tbl, item_obj, "width", empty_fm);
        auto l_val = get_fbs_double(item_tbl, item_obj, "length", empty_fm);
        auto h_val = get_fbs_double(item_tbl, item_obj, "height", empty_fm);

        if (w_val != 0.0) {
          sx = w_val;
        }

        if (l_val != 0.0) {
          sy = l_val;
        }

        if (h_val != 0.0) {
          sz = h_val;
        }

        heading = get_fbs_double(item_tbl, item_obj, "heading_angle", empty_fm);

        if (heading == 0.0) {
          heading = get_fbs_double(item_tbl, item_obj, "yaw", empty_fm);
        }
      }

      double qz_val = std::sin(heading * 0.5);
      double qw_val = std::cos(heading * 0.5);

      auto entity_fid = builder.CreateString(frame_id);
      auto entity_id = builder.CreateString(parent_id + "_" + std::to_string(idx));

      auto pos = ::foxglove::CreateVector3(builder, px, py, pz);
      auto orient = ::foxglove::CreateQuaternion(builder, 0.0, 0.0, qz_val, qw_val);
      auto pose = ::foxglove::CreatePose(builder, pos, orient);
      auto size = ::foxglove::CreateVector3(builder, sx, sy, sz);
      auto color = ::foxglove::CreateColor(builder, 0.2, 0.8, 0.2, 0.8);

      auto cube = ::foxglove::CreateCubePrimitive(builder, pose, size, color);
      std::vector<flatbuffers::Offset<::foxglove::CubePrimitive>> cubes_data = {cube};
      auto cubes_vec = builder.CreateVector(cubes_data);

      auto entity = ::foxglove::CreateSceneEntity(builder, &ts, entity_fid, entity_id, nullptr, false, 0, 0, cubes_vec);
      entity_offsets.emplace_back(entity);
    };

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target != "entities") {
        continue;
      }

      const flatbuffers::Table* entities_parent = nullptr;
      const reflection::Object* entities_parent_obj = nullptr;
      std::string entities_field_name;

      if VUNLIKELY (!resolve_fbs_parent_field_path(*root_table, obj, *schema, fm.source, entities_parent,
                                                   entities_parent_obj, entities_field_name)) {
        continue;
      }

      if VUNLIKELY (!entities_parent || !entities_parent_obj) {
        continue;
      }

      const auto* vec_field = find_fbs_field(*entities_parent_obj, entities_field_name);

      if VUNLIKELY (!vec_field || vec_field->type()->base_type() != reflection::Vector) {
        continue;
      }

      if (vec_field->type()->element() != reflection::Obj) {
        continue;
      }

      const auto* vec = flatbuffers::GetFieldV<flatbuffers::Offset<flatbuffers::Table>>(*entities_parent, *vec_field);

      if VUNLIKELY (!vec) {
        continue;
      }

      auto sub_obj_idx = vec_field->type()->index();

      if VUNLIKELY (sub_obj_idx < 0 || !schema->objects()) {
        continue;
      }

      const auto* sub_obj = schema->objects()->Get(static_cast<uint32_t>(sub_obj_idx));

      if VUNLIKELY (!sub_obj) {
        continue;
      }

      for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
        const auto* item = vec->Get(i);

        if VUNLIKELY (!item) {
          continue;
        }

        if (!entity_sub_items.empty()) {
          const auto* sub_field = find_fbs_field(*sub_obj, entity_sub_items);

          if (sub_field && sub_field->type()->base_type() == reflection::Vector) {
            const auto* sub_vec = flatbuffers::GetFieldV<flatbuffers::Offset<flatbuffers::Table>>(*item, *sub_field);

            if (sub_vec && schema->objects()) {
              const auto* sub_sub_obj = schema->objects()->Get(static_cast<uint32_t>(sub_field->type()->index()));

              if (sub_sub_obj) {
                for (flatbuffers::uoffset_t j = 0; j < sub_vec->size(); ++j) {
                  const auto* sub_item = sub_vec->Get(j);

                  if (sub_item) {
                    build_fbs_cube(*sub_item, *sub_sub_obj, static_cast<int>(j), std::to_string(i));
                  }
                }
              }
            }
          }
        } else {
          build_fbs_cube(*item, *sub_obj, static_cast<int>(i), "e");
        }
      }
    }

    auto entities_vec = builder.CreateVector(entity_offsets);
    auto scene = ::foxglove::CreateSceneUpdate(builder, 0, entities_vec);
    builder.Finish(scene);

    result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
    result.success = true;
    result.schema_name = "foxglove.SceneUpdate";
    result.encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.timestamp_ns = fbs_timestamp_ns;
    return result;
  }

  if (mapping.schema == "foxglove.FrameTransform") {
    FoxgloveMessage result;
    thread_local flatbuffers::FlatBufferBuilder builder(4096);
    builder.Clear();

    uint64_t timestamp_us = 0;
    uint64_t timestamp_ns = 0;
    std::string parent_frame_id;
    std::string child_frame_id;
    double tx = 0.0;
    double ty = 0.0;
    double tz = 0.0;
    double qx = 0.0;
    double qy = 0.0;
    double qz = 0.0;
    double qw = 1.0;
    bool has_euler = false;
    double euler_roll = 0.0;
    double euler_pitch = 0.0;
    double euler_yaw = 0.0;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "timestamp") {
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "parent_frame_id") {
        parent_frame_id = fbs_get_string(*root_table, obj, fm.source, fm.default_value);
      } else if (fm.target == "child_frame_id") {
        child_frame_id = fbs_get_string(*root_table, obj, fm.source, fm.default_value);
      } else if (fm.target == "translation_x") {
        tx = fbs_get_double(*root_table, obj, fm.source, fm.expression);
      } else if (fm.target == "translation_y") {
        ty = fbs_get_double(*root_table, obj, fm.source, fm.expression);
      } else if (fm.target == "translation_z") {
        tz = fbs_get_double(*root_table, obj, fm.source, fm.expression);
      } else if (fm.target == "rotation_x") {
        qx = fbs_get_double(*root_table, obj, fm.source, fm.expression);
      } else if (fm.target == "rotation_y") {
        qy = fbs_get_double(*root_table, obj, fm.source, fm.expression);
      } else if (fm.target == "rotation_z") {
        qz = fbs_get_double(*root_table, obj, fm.source, fm.expression);
      } else if (fm.target == "rotation_w") {
        qw = fbs_get_double(*root_table, obj, fm.source, fm.expression);
      } else if (fm.target == "euler_roll") {
        euler_roll = fbs_get_double(*root_table, obj, fm.source, fm.expression);
        has_euler = true;
      } else if (fm.target == "euler_pitch") {
        euler_pitch = fbs_get_double(*root_table, obj, fm.source, fm.expression);
        has_euler = true;
      } else if (fm.target == "euler_yaw") {
        euler_yaw = fbs_get_double(*root_table, obj, fm.source, fm.expression);
        has_euler = true;
      }
    }

    if (has_euler) {
      double cr = std::cos(euler_roll * 0.5);
      double sr = std::sin(euler_roll * 0.5);
      double cp = std::cos(euler_pitch * 0.5);
      double sp = std::sin(euler_pitch * 0.5);
      double cy = std::cos(euler_yaw * 0.5);
      double sy = std::sin(euler_yaw * 0.5);

      qw = cr * cp * cy + sr * sp * sy;
      qx = sr * cp * cy - cr * sp * sy;
      qy = cr * sp * cy + sr * cp * sy;
      qz = cr * cp * sy - sr * sp * cy;
    }

    auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
    auto pfid = builder.CreateString(parent_frame_id);
    auto cfid = builder.CreateString(child_frame_id);
    auto translation = ::foxglove::CreateVector3(builder, tx, ty, tz);
    auto rotation = ::foxglove::CreateQuaternion(builder, qx, qy, qz, qw);
    auto ft = ::foxglove::CreateFrameTransform(builder, &ts, pfid, cfid, translation, rotation);
    builder.Finish(ft);

    result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
    result.success = true;
    result.schema_name = "foxglove.FrameTransform";
    result.encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.timestamp_ns = fbs_timestamp_ns;
    return result;
  }

  if (mapping.schema == "foxglove.Log") {
    FoxgloveMessage result;
    thread_local flatbuffers::FlatBufferBuilder builder(4096);
    builder.Clear();

    uint64_t timestamp_us = 0;
    uint64_t timestamp_ns = 0;
    std::string level_str;
    std::string message;
    std::string name;
    std::string file;
    uint32_t line = 0;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "timestamp") {
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "level") {
        level_str = fbs_get_string(*root_table, obj, fm.source, fm.default_value);
      } else if (fm.target == "message") {
        message = fbs_get_string(*root_table, obj, fm.source, fm.default_value);
      } else if (fm.target == "name") {
        name = fbs_get_string(*root_table, obj, fm.source, fm.default_value);
      } else if (fm.target == "file") {
        file = fbs_get_string(*root_table, obj, fm.source, fm.default_value);
      } else if (fm.target == "line") {
        line = checked_unsigned_cast<uint32_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      }
    }

    auto level_enum = ::foxglove::LogLevel::UNKNOWN;

    if (level_str == "debug") {
      level_enum = ::foxglove::LogLevel::DEBUG;
    } else if (level_str == "info") {
      level_enum = ::foxglove::LogLevel::INFO;
    } else if (level_str == "warning") {
      level_enum = ::foxglove::LogLevel::WARNING;
    } else if (level_str == "error") {
      level_enum = ::foxglove::LogLevel::ERROR;
    } else if (level_str == "fatal") {
      level_enum = ::foxglove::LogLevel::FATAL;
    }

    auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
    auto msg_str = builder.CreateString(message);
    auto name_str = builder.CreateString(name);
    auto file_str = builder.CreateString(file);
    auto log = ::foxglove::CreateLog(builder, &ts, level_enum, msg_str, name_str, file_str, line);
    builder.Finish(log);

    result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
    result.success = true;
    result.schema_name = "foxglove.Log";
    result.encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.timestamp_ns = fbs_timestamp_ns;
    return result;
  }

  if (mapping.schema == "foxglove.LaserScan") {
    FoxgloveMessage result;
    thread_local flatbuffers::FlatBufferBuilder builder(64 * 1024);
    builder.Clear();

    uint64_t timestamp_us = 0;
    uint64_t timestamp_ns = 0;
    std::string frame_id;
    double start_angle = 0.0;
    double end_angle = 0.0;
    std::string ranges_src;
    std::string intensities_src;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "timestamp") {
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "frame_id") {
        frame_id = fbs_get_string(*root_table, obj, fm.source, fm.default_value);
      } else if (fm.target == "start_angle") {
        start_angle = fbs_get_double(*root_table, obj, fm.source, fm.expression);
      } else if (fm.target == "end_angle") {
        end_angle = fbs_get_double(*root_table, obj, fm.source, fm.expression);
      } else if (fm.target == "ranges") {
        ranges_src = fm.source;
      } else if (fm.target == "intensities") {
        intensities_src = fm.source;
      }
    }

    auto read_fbs_double_vector = [&obj, root_table](const std::string& src) -> std::vector<double> {
      std::vector<double> out;

      if (src.empty()) {
        return out;
      }

      const auto* field = find_fbs_field(obj, src);

      if VUNLIKELY (!field || field->type()->base_type() != reflection::Vector) {
        return out;
      }

      auto elem_type = field->type()->element();

      if (elem_type == reflection::Float || elem_type == reflection::Double) {
        if (elem_type == reflection::Double) {
          const auto* vec = flatbuffers::GetFieldV<double>(*root_table, *field);

          if (vec) {
            out.reserve(vec->size());

            for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
              out.emplace_back(vec->Get(i));
            }
          }
        } else {
          const auto* vec = flatbuffers::GetFieldV<float>(*root_table, *field);

          if (vec) {
            out.reserve(vec->size());

            for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
              out.emplace_back(static_cast<double>(vec->Get(i)));
            }
          }
        }
      } else if (elem_type == reflection::Long) {
        const auto* vec = flatbuffers::GetFieldV<int64_t>(*root_table, *field);

        if (vec) {
          out.reserve(vec->size());

          for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
            out.emplace_back(static_cast<double>(vec->Get(i)));
          }
        }
      } else if (elem_type == reflection::ULong) {
        const auto* vec = flatbuffers::GetFieldV<uint64_t>(*root_table, *field);

        if (vec) {
          out.reserve(vec->size());

          for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
            out.emplace_back(static_cast<double>(vec->Get(i)));
          }
        }
      } else if (elem_type == reflection::Int) {
        const auto* vec = flatbuffers::GetFieldV<int32_t>(*root_table, *field);

        if (vec) {
          out.reserve(vec->size());

          for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
            out.emplace_back(static_cast<double>(vec->Get(i)));
          }
        }
      } else if (elem_type == reflection::UInt) {
        const auto* vec = flatbuffers::GetFieldV<uint32_t>(*root_table, *field);

        if (vec) {
          out.reserve(vec->size());

          for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
            out.emplace_back(static_cast<double>(vec->Get(i)));
          }
        }
      } else if (elem_type == reflection::Short) {
        const auto* vec = flatbuffers::GetFieldV<int16_t>(*root_table, *field);

        if (vec) {
          out.reserve(vec->size());

          for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
            out.emplace_back(static_cast<double>(vec->Get(i)));
          }
        }
      } else if (elem_type == reflection::UShort) {
        const auto* vec = flatbuffers::GetFieldV<uint16_t>(*root_table, *field);

        if (vec) {
          out.reserve(vec->size());

          for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
            out.emplace_back(static_cast<double>(vec->Get(i)));
          }
        }
      } else if (elem_type == reflection::Byte) {
        const auto* vec = flatbuffers::GetFieldV<int8_t>(*root_table, *field);

        if (vec) {
          out.reserve(vec->size());

          for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
            out.emplace_back(static_cast<double>(vec->Get(i)));
          }
        }
      } else if (elem_type == reflection::UByte) {
        const auto* vec = flatbuffers::GetFieldV<uint8_t>(*root_table, *field);

        if (vec) {
          out.reserve(vec->size());

          for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
            out.emplace_back(static_cast<double>(vec->Get(i)));
          }
        }
      }

      return out;
    };

    auto ranges_data = read_fbs_double_vector(ranges_src);
    auto intensities_data = read_fbs_double_vector(intensities_src);

    auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
    auto fid = builder.CreateString(frame_id);
    auto ranges_vec = ranges_data.empty() ? 0 : builder.CreateVector(ranges_data);
    auto intensities_vec = intensities_data.empty() ? 0 : builder.CreateVector(intensities_data);
    auto scan = ::foxglove::CreateLaserScan(builder, &ts, fid, 0, start_angle, end_angle, ranges_vec, intensities_vec);
    builder.Finish(scan);

    result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
    result.success = true;
    result.schema_name = "foxglove.LaserScan";
    result.encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.timestamp_ns = fbs_timestamp_ns;
    return result;
  }

  if (mapping.schema == "foxglove.RawImage") {
    FoxgloveMessage result;
    thread_local flatbuffers::FlatBufferBuilder builder(256 * 1024);
    builder.Clear();

    uint64_t timestamp_us = 0;
    uint64_t timestamp_ns = 0;
    std::string frame_id;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string encoding;
    uint32_t step = 0;
    std::string data_src;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "timestamp") {
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "frame_id") {
        frame_id = fbs_get_string(*root_table, obj, fm.source, fm.default_value);
      } else if (fm.target == "width") {
        width = checked_unsigned_cast<uint32_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "height") {
        height = checked_unsigned_cast<uint32_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "encoding") {
        encoding = fbs_get_string(*root_table, obj, fm.source, fm.default_value);
      } else if (fm.target == "step") {
        step = checked_unsigned_cast<uint32_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "data") {
        data_src = fm.source;
      }
    }

    flatbuffers::Offset<flatbuffers::Vector<uint8_t>> data_vec = 0;

    if (!data_src.empty()) {
      const auto* field = find_fbs_field(obj, data_src);

      if (field && field->type()->base_type() == reflection::Vector) {
        const auto* vec = flatbuffers::GetFieldV<uint8_t>(*root_table, *field);

        // NOLINTNEXTLINE(readability-container-size-empty, clang-analyzer-core.StackAddressEscape)
        if VLIKELY (vec && vec->size() != 0U) {
          data_vec = builder.CreateVector(vec->data(), vec->size());
        }
      }
    }

    auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
    auto fid = builder.CreateString(frame_id);
    auto enc = builder.CreateString(encoding);
    auto img = ::foxglove::CreateRawImage(builder, &ts, fid, width, height, enc, step, data_vec);
    builder.Finish(img);

    result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
    result.success = true;
    result.schema_name = "foxglove.RawImage";
    result.encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.timestamp_ns = fbs_timestamp_ns;
    return result;
  }

  if (mapping.schema == "foxglove.GeoJSON") {
    FoxgloveMessage result;
    thread_local flatbuffers::FlatBufferBuilder builder(4096);
    builder.Clear();

    std::string geojson;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "geojson") {
        geojson = fbs_get_string(*root_table, obj, fm.source, fm.default_value);
      }
    }

    auto geojson_str = builder.CreateString(geojson);
    auto geo = ::foxglove::CreateGeoJSON(builder, geojson_str);
    builder.Finish(geo);

    result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
    result.success = true;
    result.schema_name = "foxglove.GeoJSON";
    result.encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.timestamp_ns = fbs_timestamp_ns;
    return result;
  }

  if (mapping.schema == "foxglove.PosesInFrame") {
    FoxgloveMessage result;
    thread_local flatbuffers::FlatBufferBuilder builder(16 * 1024);
    builder.Clear();

    uint64_t timestamp_us = 0;
    uint64_t timestamp_ns = 0;
    std::string frame_id;
    std::string poses_src;
    std::string pose_px_src;
    std::string pose_py_src;
    std::string pose_pz_src;
    std::string pose_qx_src;
    std::string pose_qy_src;
    std::string pose_qz_src;
    std::string pose_qw_src;
    std::string pose_px_expr;
    std::string pose_py_expr;
    std::string pose_pz_expr;
    std::string pose_qx_expr;
    std::string pose_qy_expr;
    std::string pose_qz_expr;
    std::string pose_qw_expr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "timestamp") {
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "frame_id") {
        frame_id = fbs_get_string(*root_table, obj, fm.source, fm.default_value);
      } else if (fm.target == "poses") {
        poses_src = fm.source;
      } else if (fm.target == "pose_position_x") {
        pose_px_src = fm.source;
        pose_px_expr = fm.expression;
      } else if (fm.target == "pose_position_y") {
        pose_py_src = fm.source;
        pose_py_expr = fm.expression;
      } else if (fm.target == "pose_position_z") {
        pose_pz_src = fm.source;
        pose_pz_expr = fm.expression;
      } else if (fm.target == "pose_orientation_x") {
        pose_qx_src = fm.source;
        pose_qx_expr = fm.expression;
      } else if (fm.target == "pose_orientation_y") {
        pose_qy_src = fm.source;
        pose_qy_expr = fm.expression;
      } else if (fm.target == "pose_orientation_z") {
        pose_qz_src = fm.source;
        pose_qz_expr = fm.expression;
      } else if (fm.target == "pose_orientation_w") {
        pose_qw_src = fm.source;
        pose_qw_expr = fm.expression;
      }
    }

    auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
    std::vector<flatbuffers::Offset<::foxglove::Pose>> pose_offsets;

    if (!poses_src.empty()) {
      const flatbuffers::Table* vec_parent = nullptr;
      const reflection::Object* vec_parent_obj = nullptr;
      std::string vec_field_name;

      if VUNLIKELY (!resolve_fbs_parent_field_path(*root_table, obj, *schema, poses_src, vec_parent, vec_parent_obj,
                                                   vec_field_name)) {
        vec_parent = nullptr;
      }

      if (vec_parent && vec_parent_obj) {
        const auto* vec_field = find_fbs_field(*vec_parent_obj, vec_field_name);

        if (vec_field && vec_field->type()->base_type() == reflection::Vector &&
            vec_field->type()->element() == reflection::Obj) {
          const auto* vec = flatbuffers::GetFieldV<flatbuffers::Offset<flatbuffers::Table>>(*vec_parent, *vec_field);

          if (vec && schema->objects()) {
            const auto* sub_obj = schema->objects()->Get(static_cast<uint32_t>(vec_field->type()->index()));

            if (sub_obj) {
              for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
                const auto* item = vec->Get(i);

                if VUNLIKELY (!item) {
                  continue;
                }

                double px = 0.0;
                double py = 0.0;
                double pz = 0.0;
                double qx = 0.0;
                double qy = 0.0;
                double qz = 0.0;
                double qw = 1.0;

                if (!pose_px_src.empty()) {
                  px = fbs_get_double(*item, *sub_obj, pose_px_src, pose_px_expr);
                }

                if (!pose_py_src.empty()) {
                  py = fbs_get_double(*item, *sub_obj, pose_py_src, pose_py_expr);
                }

                if (!pose_pz_src.empty()) {
                  pz = fbs_get_double(*item, *sub_obj, pose_pz_src, pose_pz_expr);
                }

                if (!pose_qx_src.empty()) {
                  qx = fbs_get_double(*item, *sub_obj, pose_qx_src, pose_qx_expr);
                }

                if (!pose_qy_src.empty()) {
                  qy = fbs_get_double(*item, *sub_obj, pose_qy_src, pose_qy_expr);
                }

                if (!pose_qz_src.empty()) {
                  qz = fbs_get_double(*item, *sub_obj, pose_qz_src, pose_qz_expr);
                }

                if (!pose_qw_src.empty()) {
                  qw = fbs_get_double(*item, *sub_obj, pose_qw_src, pose_qw_expr);
                }

                auto position = ::foxglove::CreateVector3(builder, px, py, pz);
                auto orientation = ::foxglove::CreateQuaternion(builder, qx, qy, qz, qw);
                pose_offsets.emplace_back(::foxglove::CreatePose(builder, position, orientation));
              }
            }
          }
        }
      }
    }

    auto fid = builder.CreateString(frame_id);
    auto poses_vec = builder.CreateVector(pose_offsets);
    auto pif = ::foxglove::CreatePosesInFrame(builder, &ts, fid, poses_vec);
    builder.Finish(pif);

    result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
    result.success = true;
    result.schema_name = "foxglove.PosesInFrame";
    result.encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.timestamp_ns = fbs_timestamp_ns;
    return result;
  }

  if (mapping.schema == "foxglove.FrameTransforms") {
    FoxgloveMessage result;
    thread_local flatbuffers::FlatBufferBuilder builder(16 * 1024);
    builder.Clear();

    std::string transforms_src;
    std::string ft_ts_src;
    std::string ft_ts_ns_src;
    std::string ft_parent_src;
    std::string ft_child_src;
    std::string ft_tx_src;
    std::string ft_ty_src;
    std::string ft_tz_src;
    std::string ft_qx_src;
    std::string ft_qy_src;
    std::string ft_qz_src;
    std::string ft_qw_src;
    std::string ft_ts_expr;
    std::string ft_ts_ns_expr;
    std::string ft_tx_expr;
    std::string ft_ty_expr;
    std::string ft_tz_expr;
    std::string ft_qx_expr;
    std::string ft_qy_expr;
    std::string ft_qz_expr;
    std::string ft_qw_expr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "transforms") {
        transforms_src = fm.source;
      } else if (fm.target == "transform_timestamp") {
        ft_ts_src = fm.source;
        ft_ts_expr = fm.expression;
      } else if (fm.target == "transform_timestamp_ns") {
        ft_ts_ns_src = fm.source;
        ft_ts_ns_expr = fm.expression;
      } else if (fm.target == "transform_parent_frame_id") {
        ft_parent_src = fm.source;
      } else if (fm.target == "transform_child_frame_id") {
        ft_child_src = fm.source;
      } else if (fm.target == "transform_translation_x") {
        ft_tx_src = fm.source;
        ft_tx_expr = fm.expression;
      } else if (fm.target == "transform_translation_y") {
        ft_ty_src = fm.source;
        ft_ty_expr = fm.expression;
      } else if (fm.target == "transform_translation_z") {
        ft_tz_src = fm.source;
        ft_tz_expr = fm.expression;
      } else if (fm.target == "transform_rotation_x") {
        ft_qx_src = fm.source;
        ft_qx_expr = fm.expression;
      } else if (fm.target == "transform_rotation_y") {
        ft_qy_src = fm.source;
        ft_qy_expr = fm.expression;
      } else if (fm.target == "transform_rotation_z") {
        ft_qz_src = fm.source;
        ft_qz_expr = fm.expression;
      } else if (fm.target == "transform_rotation_w") {
        ft_qw_src = fm.source;
        ft_qw_expr = fm.expression;
      }
    }

    std::vector<flatbuffers::Offset<::foxglove::FrameTransform>> transform_offsets;

    if (!transforms_src.empty()) {
      const auto* vec_field = find_fbs_field(obj, transforms_src);

      if (vec_field && vec_field->type()->base_type() == reflection::Vector &&
          vec_field->type()->element() == reflection::Obj) {
        const auto* vec = flatbuffers::GetFieldV<flatbuffers::Offset<flatbuffers::Table>>(*root_table, *vec_field);

        if (vec && schema->objects()) {
          const auto* sub_obj = schema->objects()->Get(static_cast<uint32_t>(vec_field->type()->index()));

          if (sub_obj) {
            for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
              const auto* item = vec->Get(i);

              if VUNLIKELY (!item) {
                continue;
              }

              FieldMapping empty_fm;

              uint64_t ft_ts_us = 0;
              uint64_t ft_ts_ns = 0;

              if (!ft_ts_src.empty()) {
                ft_ts_us = checked_unsigned_cast<uint64_t>(fbs_get_double(*item, *sub_obj, ft_ts_src, ft_ts_expr));
              }

              if (!ft_ts_ns_src.empty()) {
                ft_ts_ns =
                    checked_unsigned_cast<uint64_t>(fbs_get_double(*item, *sub_obj, ft_ts_ns_src, ft_ts_ns_expr));
              }

              std::string parent_fid;

              if (!ft_parent_src.empty()) {
                parent_fid = get_fbs_string(*item, *sub_obj, ft_parent_src, empty_fm);
              }

              std::string child_fid;

              if (!ft_child_src.empty()) {
                child_fid = get_fbs_string(*item, *sub_obj, ft_child_src, empty_fm);
              }

              double itx = 0.0;
              double ity = 0.0;
              double itz = 0.0;

              if (!ft_tx_src.empty()) {
                itx = fbs_get_double(*item, *sub_obj, ft_tx_src, ft_tx_expr);
              }

              if (!ft_ty_src.empty()) {
                ity = fbs_get_double(*item, *sub_obj, ft_ty_src, ft_ty_expr);
              }

              if (!ft_tz_src.empty()) {
                itz = fbs_get_double(*item, *sub_obj, ft_tz_src, ft_tz_expr);
              }

              double iqx = 0.0;
              double iqy = 0.0;
              double iqz = 0.0;
              double iqw = 1.0;

              if (!ft_qx_src.empty()) {
                iqx = fbs_get_double(*item, *sub_obj, ft_qx_src, ft_qx_expr);
              }

              if (!ft_qy_src.empty()) {
                iqy = fbs_get_double(*item, *sub_obj, ft_qy_src, ft_qy_expr);
              }

              if (!ft_qz_src.empty()) {
                iqz = fbs_get_double(*item, *sub_obj, ft_qz_src, ft_qz_expr);
              }

              if (!ft_qw_src.empty()) {
                iqw = fbs_get_double(*item, *sub_obj, ft_qw_src, ft_qw_expr);
              }

              auto item_ts = (ft_ts_ns > 0) ? make_timestamp_from_ns(ft_ts_ns) : make_timestamp_from_us(ft_ts_us);
              auto pfid = builder.CreateString(parent_fid);
              auto cfid = builder.CreateString(child_fid);
              auto translation = ::foxglove::CreateVector3(builder, itx, ity, itz);
              auto rotation = ::foxglove::CreateQuaternion(builder, iqx, iqy, iqz, iqw);
              transform_offsets.emplace_back(
                  ::foxglove::CreateFrameTransform(builder, &item_ts, pfid, cfid, translation, rotation));
            }
          }
        }
      }
    }

    auto transforms_vec = builder.CreateVector(transform_offsets);
    auto fts = ::foxglove::CreateFrameTransforms(builder, transforms_vec);
    builder.Finish(fts);

    result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
    result.success = true;
    result.schema_name = "foxglove.FrameTransforms";
    result.encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.timestamp_ns = fbs_timestamp_ns;
    return result;
  }

  if (mapping.schema == "foxglove.LocationFixes") {
    FoxgloveMessage result;
    thread_local flatbuffers::FlatBufferBuilder builder(16 * 1024);
    builder.Clear();

    std::string fixes_src;
    std::string fix_ts_src;
    std::string fix_ts_ns_src;
    std::string fix_frame_id_src;
    std::string fix_lat_src;
    std::string fix_lon_src;
    std::string fix_alt_src;
    std::string fix_ts_expr;
    std::string fix_ts_ns_expr;
    std::string fix_lat_expr;
    std::string fix_lon_expr;
    std::string fix_alt_expr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "fixes") {
        fixes_src = fm.source;
      } else if (fm.target == "fix_timestamp") {
        fix_ts_src = fm.source;
        fix_ts_expr = fm.expression;
      } else if (fm.target == "fix_timestamp_ns") {
        fix_ts_ns_src = fm.source;
        fix_ts_ns_expr = fm.expression;
      } else if (fm.target == "fix_frame_id") {
        fix_frame_id_src = fm.source;
      } else if (fm.target == "fix_latitude") {
        fix_lat_src = fm.source;
        fix_lat_expr = fm.expression;
      } else if (fm.target == "fix_longitude") {
        fix_lon_src = fm.source;
        fix_lon_expr = fm.expression;
      } else if (fm.target == "fix_altitude") {
        fix_alt_src = fm.source;
        fix_alt_expr = fm.expression;
      }
    }

    std::vector<flatbuffers::Offset<::foxglove::LocationFix>> fix_offsets;

    if (!fixes_src.empty()) {
      const auto* vec_field = find_fbs_field(obj, fixes_src);

      if (vec_field && vec_field->type()->base_type() == reflection::Vector &&
          vec_field->type()->element() == reflection::Obj) {
        const auto* vec = flatbuffers::GetFieldV<flatbuffers::Offset<flatbuffers::Table>>(*root_table, *vec_field);

        if (vec && schema->objects()) {
          const auto* sub_obj = schema->objects()->Get(static_cast<uint32_t>(vec_field->type()->index()));

          if (sub_obj) {
            for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
              const auto* item = vec->Get(i);

              if VUNLIKELY (!item) {
                continue;
              }

              FieldMapping empty_fm;

              uint64_t item_ts_us = 0;
              uint64_t item_ts_ns = 0;

              if (!fix_ts_src.empty()) {
                item_ts_us = checked_unsigned_cast<uint64_t>(fbs_get_double(*item, *sub_obj, fix_ts_src, fix_ts_expr));
              }

              if (!fix_ts_ns_src.empty()) {
                item_ts_ns =
                    checked_unsigned_cast<uint64_t>(fbs_get_double(*item, *sub_obj, fix_ts_ns_src, fix_ts_ns_expr));
              }

              std::string item_frame_id;

              if (!fix_frame_id_src.empty()) {
                item_frame_id = get_fbs_string(*item, *sub_obj, fix_frame_id_src, empty_fm);
              }

              double lat = 0.0;
              double lon = 0.0;
              double alt = 0.0;

              if (!fix_lat_src.empty()) {
                lat = fbs_get_double(*item, *sub_obj, fix_lat_src, fix_lat_expr);
              }

              if (!fix_lon_src.empty()) {
                lon = fbs_get_double(*item, *sub_obj, fix_lon_src, fix_lon_expr);
              }

              if (!fix_alt_src.empty()) {
                alt = fbs_get_double(*item, *sub_obj, fix_alt_src, fix_alt_expr);
              }

              auto item_ts = (item_ts_ns > 0) ? make_timestamp_from_ns(item_ts_ns) : make_timestamp_from_us(item_ts_us);
              auto fid = builder.CreateString(item_frame_id);
              fix_offsets.emplace_back(::foxglove::CreateLocationFix(builder, &item_ts, fid, lat, lon, alt));
            }
          }
        }
      }
    }

    auto fixes_vec = builder.CreateVector(fix_offsets);
    auto lfs = ::foxglove::CreateLocationFixes(builder, fixes_vec);
    builder.Finish(lfs);

    result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
    result.success = true;
    result.schema_name = "foxglove.LocationFixes";
    result.encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.timestamp_ns = fbs_timestamp_ns;
    return result;
  }

  if (mapping.schema == "foxglove.CameraCalibration") {
    FoxgloveMessage result;
    thread_local flatbuffers::FlatBufferBuilder builder(4096);
    builder.Clear();

    uint64_t timestamp_us = 0;
    uint64_t timestamp_ns = 0;
    std::string frame_id;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string distortion_model;
    std::string d_src;
    std::string k_src;
    std::string r_src;
    std::string p_src;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "timestamp") {
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "frame_id") {
        frame_id = fbs_get_string(*root_table, obj, fm.source, fm.default_value);
      } else if (fm.target == "width") {
        width = checked_unsigned_cast<uint32_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "height") {
        height = checked_unsigned_cast<uint32_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "distortion_model") {
        distortion_model = fbs_get_string(*root_table, obj, fm.source, fm.default_value);
      } else if (fm.target == "d") {
        d_src = fm.source;
      } else if (fm.target == "k") {
        k_src = fm.source;
      } else if (fm.target == "r") {
        r_src = fm.source;
      } else if (fm.target == "p") {
        p_src = fm.source;
      }
    }

    auto read_fbs_double_vector = [&obj, root_table](const std::string& src) -> std::vector<double> {
      std::vector<double> out;

      if (src.empty()) {
        return out;
      }

      const auto* field = find_fbs_field(obj, src);

      if VUNLIKELY (!field || field->type()->base_type() != reflection::Vector) {
        return out;
      }

      auto elem_type = field->type()->element();

      if (elem_type == reflection::Double) {
        const auto* vec = flatbuffers::GetFieldV<double>(*root_table, *field);

        if (vec) {
          out.reserve(vec->size());

          for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
            out.emplace_back(vec->Get(i));
          }
        }
      } else if (elem_type == reflection::Float) {
        const auto* vec = flatbuffers::GetFieldV<float>(*root_table, *field);

        if (vec) {
          out.reserve(vec->size());

          for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
            out.emplace_back(static_cast<double>(vec->Get(i)));
          }
        }
      }

      return out;
    };

    auto d_data = read_fbs_double_vector(d_src);
    auto k_data = read_fbs_double_vector(k_src);
    auto r_data = read_fbs_double_vector(r_src);
    auto p_data = read_fbs_double_vector(p_src);

    auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
    auto fid = builder.CreateString(frame_id);
    auto dm = builder.CreateString(distortion_model);
    auto d_vec = d_data.empty() ? 0 : builder.CreateVector(d_data);
    auto k_vec = k_data.empty() ? 0 : builder.CreateVector(k_data);
    auto r_vec = r_data.empty() ? 0 : builder.CreateVector(r_data);
    auto p_vec = p_data.empty() ? 0 : builder.CreateVector(p_data);
    auto cal = ::foxglove::CreateCameraCalibration(builder, &ts, fid, width, height, dm, d_vec, k_vec, r_vec, p_vec);
    builder.Finish(cal);

    result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
    result.success = true;
    result.schema_name = "foxglove.CameraCalibration";
    result.encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.timestamp_ns = fbs_timestamp_ns;
    return result;
  }

  if (mapping.schema == "foxglove.CompressedVideo") {
    FoxgloveMessage result;
    thread_local flatbuffers::FlatBufferBuilder builder(256 * 1024);
    builder.Clear();

    uint64_t timestamp_us = 0;
    uint64_t timestamp_ns = 0;
    std::string frame_id;
    std::string format;
    std::string data_src;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "timestamp") {
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "frame_id") {
        frame_id = fbs_get_string(*root_table, obj, fm.source, fm.default_value);
      } else if (fm.target == "format") {
        format = fbs_get_string(*root_table, obj, fm.source, fm.default_value);
      } else if (fm.target == "data") {
        data_src = fm.source;
      }
    }

    flatbuffers::Offset<flatbuffers::Vector<uint8_t>> data_vec = 0;

    if (!data_src.empty()) {
      const auto* field = find_fbs_field(obj, data_src);

      if (field && field->type()->base_type() == reflection::Vector) {
        const auto* vec = flatbuffers::GetFieldV<uint8_t>(*root_table, *field);

        // NOLINTNEXTLINE(readability-container-size-empty, clang-analyzer-core.StackAddressEscape)
        if VLIKELY (vec && vec->size() != 0U) {
          data_vec = builder.CreateVector(vec->data(), vec->size());
        }
      }
    }

    auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
    auto fid = builder.CreateString(frame_id);
    auto fmt = builder.CreateString(format);
    auto cv = ::foxglove::CreateCompressedVideo(builder, &ts, fid, data_vec, fmt);
    builder.Finish(cv);

    result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
    result.success = true;
    result.schema_name = "foxglove.CompressedVideo";
    result.encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.timestamp_ns = fbs_timestamp_ns;
    return result;
  }

  if (mapping.schema == "foxglove.Grid") {
    FoxgloveMessage result;
    thread_local flatbuffers::FlatBufferBuilder builder(64 * 1024);
    builder.Clear();

    uint64_t timestamp_us = 0;
    uint64_t timestamp_ns = 0;
    std::string frame_id;
    uint32_t column_count = 0;
    double cell_size_x = 1.0;
    double cell_size_y = 1.0;
    uint32_t row_stride = 0;
    uint32_t cell_stride = 0;
    std::string fields_src;
    std::string data_src;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "timestamp") {
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "frame_id") {
        frame_id = fbs_get_string(*root_table, obj, fm.source, fm.default_value);
      } else if (fm.target == "column_count") {
        column_count = checked_unsigned_cast<uint32_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "cell_size_x") {
        cell_size_x = fbs_get_double(*root_table, obj, fm.source, fm.expression);
      } else if (fm.target == "cell_size_y") {
        cell_size_y = fbs_get_double(*root_table, obj, fm.source, fm.expression);
      } else if (fm.target == "row_stride") {
        row_stride = checked_unsigned_cast<uint32_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "cell_stride") {
        cell_stride = checked_unsigned_cast<uint32_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "fields") {
        fields_src = fm.source;
      } else if (fm.target == "data") {
        data_src = fm.source;
      }
    }

    std::vector<flatbuffers::Offset<::foxglove::PackedElementField>> field_offsets;

    if (!fields_src.empty()) {
      const auto* vec_field = find_fbs_field(obj, fields_src);

      if (vec_field && vec_field->type()->base_type() == reflection::Vector &&
          vec_field->type()->element() == reflection::Obj) {
        const auto* vec = flatbuffers::GetFieldV<flatbuffers::Offset<flatbuffers::Table>>(*root_table, *vec_field);

        if (vec && schema->objects()) {
          const auto* sub_obj = schema->objects()->Get(static_cast<uint32_t>(vec_field->type()->index()));

          if (sub_obj) {
            for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
              const auto* item = vec->Get(i);

              if VUNLIKELY (!item) {
                continue;
              }

              FieldMapping empty_fm;
              auto fname = get_fbs_string(*item, *sub_obj, "name", empty_fm);
              auto foffset = checked_unsigned_cast<uint32_t>(get_fbs_double(*item, *sub_obj, "offset", empty_fm));
              auto ftype_val = checked_unsigned_cast<uint8_t>(get_fbs_double(*item, *sub_obj, "type", empty_fm));
              auto ftype = static_cast<::foxglove::NumericType>(ftype_val);
              auto fname_off = builder.CreateString(fname);
              field_offsets.emplace_back(::foxglove::CreatePackedElementField(builder, fname_off, foffset, ftype));
            }
          }
        }
      }
    }

    flatbuffers::Offset<flatbuffers::Vector<uint8_t>> data_vec = 0;

    if (!data_src.empty()) {
      const auto* field = find_fbs_field(obj, data_src);

      if (field && field->type()->base_type() == reflection::Vector) {
        const auto* vec = flatbuffers::GetFieldV<uint8_t>(*root_table, *field);

        // NOLINTNEXTLINE(readability-container-size-empty, clang-analyzer-core.StackAddressEscape)
        if VLIKELY (vec && vec->size() != 0U) {
          data_vec = builder.CreateVector(vec->data(), vec->size());
        }
      }
    }

    auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
    auto fid = builder.CreateString(frame_id);
    auto pose = ::foxglove::CreatePose(builder, ::foxglove::CreateVector3(builder, 0.0, 0.0, 0.0),
                                       ::foxglove::CreateQuaternion(builder, 0.0, 0.0, 0.0, 1.0));
    auto cell_sz = ::foxglove::CreateVector2(builder, cell_size_x, cell_size_y);
    auto fields_vec = builder.CreateVector(field_offsets);
    auto grid = ::foxglove::CreateGrid(builder, &ts, fid, pose, column_count, cell_sz, row_stride, cell_stride,
                                       fields_vec, data_vec);
    builder.Finish(grid);

    result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
    result.success = true;
    result.schema_name = "foxglove.Grid";
    result.encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.timestamp_ns = fbs_timestamp_ns;
    return result;
  }

  if (mapping.schema == "foxglove.ImageAnnotations") {
    FoxgloveMessage result;
    thread_local flatbuffers::FlatBufferBuilder builder(16 * 1024);
    builder.Clear();

    uint64_t timestamp_us = 0;
    uint64_t timestamp_ns = 0;
    std::string circles_src;
    std::string points_src;
    std::string texts_src;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "timestamp") {
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "circles") {
        circles_src = fm.source;
      } else if (fm.target == "points") {
        points_src = fm.source;
      } else if (fm.target == "texts") {
        texts_src = fm.source;
      }
    }

    auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);

    std::vector<flatbuffers::Offset<::foxglove::CircleAnnotation>> circle_offsets;

    if (!circles_src.empty()) {
      const auto* vec_field = find_fbs_field(obj, circles_src);

      if (vec_field && vec_field->type()->base_type() == reflection::Vector &&
          vec_field->type()->element() == reflection::Obj) {
        const auto* vec = flatbuffers::GetFieldV<flatbuffers::Offset<flatbuffers::Table>>(*root_table, *vec_field);

        if (vec && schema->objects()) {
          const auto* sub_obj = schema->objects()->Get(static_cast<uint32_t>(vec_field->type()->index()));

          if (sub_obj) {
            for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
              const auto* item = vec->Get(i);

              if VUNLIKELY (!item) {
                continue;
              }

              FieldMapping empty_fm;
              double cx = get_fbs_double(*item, *sub_obj, "x", empty_fm);
              double cy = get_fbs_double(*item, *sub_obj, "y", empty_fm);
              double diameter = get_fbs_double(*item, *sub_obj, "diameter", empty_fm);
              double thickness = get_fbs_double(*item, *sub_obj, "thickness", empty_fm);

              auto pos = ::foxglove::CreatePoint2(builder, cx, cy);
              auto fill = ::foxglove::CreateColor(builder, 1.0, 0.0, 0.0, 0.5);
              auto outline = ::foxglove::CreateColor(builder, 1.0, 0.0, 0.0, 1.0);
              circle_offsets.emplace_back(
                  ::foxglove::CreateCircleAnnotation(builder, &ts, pos, diameter, thickness, fill, outline));
            }
          }
        }
      }
    }

    std::vector<flatbuffers::Offset<::foxglove::PointsAnnotation>> points_offsets;

    if (!points_src.empty()) {
      const auto* vec_field = find_fbs_field(obj, points_src);

      if (vec_field && vec_field->type()->base_type() == reflection::Vector &&
          vec_field->type()->element() == reflection::Obj) {
        const auto* vec = flatbuffers::GetFieldV<flatbuffers::Offset<flatbuffers::Table>>(*root_table, *vec_field);

        if (vec && schema->objects()) {
          const auto* sub_obj = schema->objects()->Get(static_cast<uint32_t>(vec_field->type()->index()));

          if (sub_obj) {
            for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
              const auto* item = vec->Get(i);

              if VUNLIKELY (!item) {
                continue;
              }

              FieldMapping empty_fm;
              double px = get_fbs_double(*item, *sub_obj, "x", empty_fm);
              double py = get_fbs_double(*item, *sub_obj, "y", empty_fm);

              auto pt = ::foxglove::CreatePoint2(builder, px, py);
              std::vector<flatbuffers::Offset<::foxglove::Point2>> pts_vec = {pt};
              auto pts = builder.CreateVector(pts_vec);
              auto outline = ::foxglove::CreateColor(builder, 0.0, 1.0, 0.0, 1.0);
              points_offsets.emplace_back(::foxglove::CreatePointsAnnotation(
                  builder, &ts, ::foxglove::PointsAnnotationType::POINTS, pts, outline));
            }
          }
        }
      }
    }

    std::vector<flatbuffers::Offset<::foxglove::TextAnnotation>> text_offsets;

    if (!texts_src.empty()) {
      const auto* vec_field = find_fbs_field(obj, texts_src);

      if (vec_field && vec_field->type()->base_type() == reflection::Vector &&
          vec_field->type()->element() == reflection::Obj) {
        const auto* vec = flatbuffers::GetFieldV<flatbuffers::Offset<flatbuffers::Table>>(*root_table, *vec_field);

        if (vec && schema->objects()) {
          const auto* sub_obj = schema->objects()->Get(static_cast<uint32_t>(vec_field->type()->index()));

          if (sub_obj) {
            for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
              const auto* item = vec->Get(i);

              if VUNLIKELY (!item) {
                continue;
              }

              FieldMapping empty_fm;
              double tx = get_fbs_double(*item, *sub_obj, "x", empty_fm);
              double ty = get_fbs_double(*item, *sub_obj, "y", empty_fm);
              auto text_str = get_fbs_string(*item, *sub_obj, "text", empty_fm);
              double font_size = get_fbs_double(*item, *sub_obj, "font_size", empty_fm);

              if (font_size <= 0.0) {
                font_size = 12.0;
              }

              auto pos = ::foxglove::CreatePoint2(builder, tx, ty);
              auto text_off = builder.CreateString(text_str);
              auto text_color = ::foxglove::CreateColor(builder, 1.0, 1.0, 1.0, 1.0);
              text_offsets.emplace_back(
                  ::foxglove::CreateTextAnnotation(builder, &ts, pos, text_off, font_size, text_color));
            }
          }
        }
      }
    }

    auto circles_vec = builder.CreateVector(circle_offsets);
    auto points_vec = builder.CreateVector(points_offsets);
    auto texts_vec = builder.CreateVector(text_offsets);
    auto ann = ::foxglove::CreateImageAnnotations(builder, circles_vec, points_vec, texts_vec, 0, &ts);
    builder.Finish(ann);

    result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
    result.success = true;
    result.schema_name = "foxglove.ImageAnnotations";
    result.encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.timestamp_ns = fbs_timestamp_ns;
    return result;
  }

  if (mapping.schema == "foxglove.JointStates") {
    FoxgloveMessage result;
    thread_local flatbuffers::FlatBufferBuilder builder(16 * 1024);
    builder.Clear();

    uint64_t timestamp_us = 0;
    uint64_t timestamp_ns = 0;
    std::string joints_src;
    std::string joint_name_src;
    std::string joint_position_src;
    std::string joint_velocity_src;
    std::string joint_effort_src;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "timestamp") {
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "joints") {
        joints_src = fm.source;
      } else if (fm.target == "joint_name") {
        joint_name_src = fm.source;
      } else if (fm.target == "joint_position") {
        joint_position_src = fm.source;
      } else if (fm.target == "joint_velocity") {
        joint_velocity_src = fm.source;
      } else if (fm.target == "joint_effort") {
        joint_effort_src = fm.source;
      }
    }

    std::vector<flatbuffers::Offset<::foxglove::JointState>> joint_offsets;

    if (!joints_src.empty()) {
      const auto* vec_field = find_fbs_field(obj, joints_src);

      if (vec_field && vec_field->type()->base_type() == reflection::Vector &&
          vec_field->type()->element() == reflection::Obj) {
        const auto* vec = flatbuffers::GetFieldV<flatbuffers::Offset<flatbuffers::Table>>(*root_table, *vec_field);

        if (vec && schema->objects()) {
          const auto* sub_obj = schema->objects()->Get(static_cast<uint32_t>(vec_field->type()->index()));

          if (sub_obj) {
            for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
              const auto* item = vec->Get(i);

              if VUNLIKELY (!item) {
                continue;
              }

              FieldMapping empty_fm;
              auto name_field = joint_name_src.empty() ? "name" : joint_name_src;
              auto pos_field = joint_position_src.empty() ? "position" : joint_position_src;
              auto vel_field = joint_velocity_src.empty() ? "velocity" : joint_velocity_src;
              auto eff_field = joint_effort_src.empty() ? "effort" : joint_effort_src;

              auto jname = get_fbs_string(*item, *sub_obj, name_field, empty_fm);
              double jpos = get_fbs_double(*item, *sub_obj, pos_field, empty_fm);
              double jvel = get_fbs_double(*item, *sub_obj, vel_field, empty_fm);
              double jeff = get_fbs_double(*item, *sub_obj, eff_field, empty_fm);

              auto name_off = builder.CreateString(jname);
              joint_offsets.emplace_back(
                  ::foxglove::CreateJointState(builder, name_off, jpos, jvel, ::flatbuffers::nullopt, jeff));
            }
          }
        }
      }
    }

    auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
    auto joints_vec = builder.CreateVector(joint_offsets);
    auto js = ::foxglove::CreateJointStates(builder, &ts, joints_vec);
    builder.Finish(js);

    result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
    result.success = true;
    result.schema_name = "foxglove.JointStates";
    result.encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.timestamp_ns = fbs_timestamp_ns;
    return result;
  }

  if (mapping.schema == "foxglove.Point3InFrame") {
    FoxgloveMessage result;
    thread_local flatbuffers::FlatBufferBuilder builder(4096);
    builder.Clear();

    uint64_t timestamp_us = 0;
    uint64_t timestamp_ns = 0;
    std::string frame_id;
    double position_x = 0.0;
    double position_y = 0.0;
    double position_z = 0.0;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "timestamp") {
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "frame_id") {
        frame_id = fbs_get_string(*root_table, obj, fm.source, fm.default_value);
      } else if (fm.target == "position_x") {
        position_x = fbs_get_double(*root_table, obj, fm.source, fm.expression);
      } else if (fm.target == "position_y") {
        position_y = fbs_get_double(*root_table, obj, fm.source, fm.expression);
      } else if (fm.target == "position_z") {
        position_z = fbs_get_double(*root_table, obj, fm.source, fm.expression);
      }
    }

    auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
    auto fid = builder.CreateString(frame_id);
    auto point = ::foxglove::CreatePoint3(builder, position_x, position_y, position_z);
    auto p3f = ::foxglove::CreatePoint3InFrame(builder, &ts, fid, point);
    builder.Finish(p3f);

    result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
    result.success = true;
    result.schema_name = "foxglove.Point3InFrame";
    result.encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.timestamp_ns = fbs_timestamp_ns;
    return result;
  }

  if (mapping.schema == "foxglove.RawAudio") {
    FoxgloveMessage result;
    thread_local flatbuffers::FlatBufferBuilder builder(64 * 1024);
    builder.Clear();

    uint64_t timestamp_us = 0;
    uint64_t timestamp_ns = 0;
    uint32_t sample_rate = 0;
    uint32_t number_of_channels = 0;
    std::string format;
    std::string data_src;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "timestamp") {
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "sample_rate") {
        sample_rate = checked_unsigned_cast<uint32_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "number_of_channels") {
        number_of_channels =
            checked_unsigned_cast<uint32_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "format") {
        format = fbs_get_string(*root_table, obj, fm.source, fm.default_value);
      } else if (fm.target == "data") {
        data_src = fm.source;
      }
    }

    flatbuffers::Offset<flatbuffers::Vector<uint8_t>> data_vec = 0;

    if (!data_src.empty()) {
      const auto* field = find_fbs_field(obj, data_src);

      if (field && field->type()->base_type() == reflection::Vector) {
        const auto* vec = flatbuffers::GetFieldV<uint8_t>(*root_table, *field);

        // NOLINTNEXTLINE(readability-container-size-empty, clang-analyzer-core.StackAddressEscape)
        if VLIKELY (vec && vec->size() != 0U) {
          data_vec = builder.CreateVector(vec->data(), vec->size());
        }
      }
    }

    auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
    auto fmt = builder.CreateString(format);
    auto ra = ::foxglove::CreateRawAudio(builder, &ts, data_vec, fmt, sample_rate, number_of_channels);
    builder.Finish(ra);

    result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
    result.success = true;
    result.schema_name = "foxglove.RawAudio";
    result.encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.timestamp_ns = fbs_timestamp_ns;
    return result;
  }

  if (mapping.schema == "foxglove.VoxelGrid") {
    FoxgloveMessage result;
    thread_local flatbuffers::FlatBufferBuilder builder(64 * 1024);
    builder.Clear();

    uint64_t timestamp_us = 0;
    uint64_t timestamp_ns = 0;
    std::string frame_id;
    double voxel_size_x = 1.0;
    double voxel_size_y = 1.0;
    double voxel_size_z = 1.0;
    uint32_t row_count = 0;
    uint32_t column_count = 0;
    uint32_t slice_stride = 0;
    uint32_t row_stride = 0;
    uint32_t cell_stride = 0;
    std::string fields_src;
    std::string data_src;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "timestamp") {
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "frame_id") {
        frame_id = fbs_get_string(*root_table, obj, fm.source, fm.default_value);
      } else if (fm.target == "voxel_size_x") {
        voxel_size_x = fbs_get_double(*root_table, obj, fm.source, fm.expression);
      } else if (fm.target == "voxel_size_y") {
        voxel_size_y = fbs_get_double(*root_table, obj, fm.source, fm.expression);
      } else if (fm.target == "voxel_size_z") {
        voxel_size_z = fbs_get_double(*root_table, obj, fm.source, fm.expression);
      } else if (fm.target == "row_count") {
        row_count = checked_unsigned_cast<uint32_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "column_count") {
        column_count = checked_unsigned_cast<uint32_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "slice_stride") {
        slice_stride = checked_unsigned_cast<uint32_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "row_stride") {
        row_stride = checked_unsigned_cast<uint32_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "cell_stride") {
        cell_stride = checked_unsigned_cast<uint32_t>(fbs_get_double(*root_table, obj, fm.source, fm.expression));
      } else if (fm.target == "fields") {
        fields_src = fm.source;
      } else if (fm.target == "data") {
        data_src = fm.source;
      }
    }

    std::vector<flatbuffers::Offset<::foxglove::PackedElementField>> field_offsets;

    if (!fields_src.empty()) {
      const auto* vec_field = find_fbs_field(obj, fields_src);

      if (vec_field && vec_field->type()->base_type() == reflection::Vector &&
          vec_field->type()->element() == reflection::Obj) {
        const auto* vec = flatbuffers::GetFieldV<flatbuffers::Offset<flatbuffers::Table>>(*root_table, *vec_field);

        if (vec && schema->objects()) {
          const auto* sub_obj = schema->objects()->Get(static_cast<uint32_t>(vec_field->type()->index()));

          if (sub_obj) {
            for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
              const auto* item = vec->Get(i);

              if VUNLIKELY (!item) {
                continue;
              }

              FieldMapping empty_fm;
              auto fname = get_fbs_string(*item, *sub_obj, "name", empty_fm);
              auto foffset = checked_unsigned_cast<uint32_t>(get_fbs_double(*item, *sub_obj, "offset", empty_fm));
              auto ftype_val = checked_unsigned_cast<uint8_t>(get_fbs_double(*item, *sub_obj, "type", empty_fm));
              auto ftype = static_cast<::foxglove::NumericType>(ftype_val);
              auto fname_off = builder.CreateString(fname);
              field_offsets.emplace_back(::foxglove::CreatePackedElementField(builder, fname_off, foffset, ftype));
            }
          }
        }
      }
    }

    flatbuffers::Offset<flatbuffers::Vector<uint8_t>> data_vec = 0;

    if (!data_src.empty()) {
      const auto* field = find_fbs_field(obj, data_src);

      if (field && field->type()->base_type() == reflection::Vector) {
        const auto* vec = flatbuffers::GetFieldV<uint8_t>(*root_table, *field);

        // NOLINTNEXTLINE(readability-container-size-empty, clang-analyzer-core.StackAddressEscape)
        if VLIKELY (vec && vec->size() != 0U) {
          data_vec = builder.CreateVector(vec->data(), vec->size());
        }
      }
    }

    auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
    auto fid = builder.CreateString(frame_id);
    auto pose = ::foxglove::CreatePose(builder, ::foxglove::CreateVector3(builder, 0.0, 0.0, 0.0),
                                       ::foxglove::CreateQuaternion(builder, 0.0, 0.0, 0.0, 1.0));
    auto cell_sz = ::foxglove::CreateVector3(builder, voxel_size_x, voxel_size_y, voxel_size_z);
    auto fields_vec = builder.CreateVector(field_offsets);
    auto vg = ::foxglove::CreateVoxelGrid(builder, &ts, fid, pose, row_count, column_count, cell_sz, slice_stride,
                                          row_stride, cell_stride, fields_vec, data_vec);
    builder.Finish(vg);

    result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
    result.success = true;
    result.schema_name = "foxglove.VoxelGrid";
    result.encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
    result.timestamp_ns = fbs_timestamp_ns;
    return result;
  }

  MLOG_W("FBS mapping: unsupported target schema: {}", mapping.schema);
  FoxgloveMessage fbs_empty;
  return fbs_empty;
}

#endif

void FoxgloveConverter::load_mappings() {
  mappings_.clear();
  mapping_index_.clear();

  for (const auto& file : config_.vlink_msgs) {
    if VUNLIKELY (!load_mapping_file(file)) {
      MLOG_W("Failed to load mapping: {}", file);
    }
  }

  for (const auto& m : mappings_) {
    mapping_index_[m.ser].push_back(&m);
  }
}

bool FoxgloveConverter::load_mapping_file(const std::string& path) {
  std::vector<FoxgloveMapping> loaded_mappings;

  auto ok = load_json_entries(
      path, "Mapping file not found", "Failed to parse mapping", [&loaded_mappings, &path](const Json& obj) -> bool {
        try {
          if VUNLIKELY (!obj.is_object()) {
            return false;
          }

          FoxgloveMapping mapping;
          mapping.ser = obj.value("ser", std::string());

          if VUNLIKELY (!parse_url_selector(obj, path, "mapping", mapping.url_selector)) {
            return false;
          }

          mapping.schema = obj.value("schema", std::string());
          mapping.encoding = obj.value("encoding", std::string(kFoxgloveFlatbufferEncoding));
          mapping.schema_encoding = obj.value("schema_encoding", std::string(kFoxgloveFlatbufferEncoding));
          mapping.converter = obj.value("converter", std::string());
          mapping.timestamp_field = obj.value("timestamp_field", std::string());

          if VUNLIKELY (!parse_timestamp_unit(obj, "timestamp_unit", path, "mapping", mapping.timestamp_unit)) {
            return false;
          }

          if (obj.contains("topic")) {
            MLOG_W(
                "vlink_msgs mapping in {} ignores topic; Foxglove channel topic always follows the runtime VLink URL",
                path);
          }

          if VUNLIKELY (!parse_field_mappings(obj, path, "mapping", mapping.field_mappings)) {
            return false;
          }

          if VUNLIKELY (mapping.ser.empty()) {
            MLOG_W("Invalid mapping in {}: missing ser", path);
            return false;
          }

          if (mapping.converter == "passthrough") {
            if (mapping.schema.empty()) {
              mapping.schema = mapping.ser;
            }

            if VUNLIKELY (!obj.contains("schema_encoding")) {
              mapping.schema_encoding = mapping.encoding;
            }

            if VUNLIKELY (mapping.encoding != "protobuf" && !is_flatbuffers_schema_encoding(mapping.encoding)) {
              MLOG_W("Invalid passthrough mapping in {}: encoding must be protobuf or flatbuffer", path);
              return false;
            }

            if VUNLIKELY (mapping.schema_encoding != mapping.encoding) {
              MLOG_W("Invalid passthrough mapping in {}: schema_encoding must equal encoding", path);
              return false;
            }

            if VUNLIKELY (!mapping.field_mappings.empty()) {
              MLOG_W("Invalid passthrough mapping in {}: field_mappings are not used", path);
              return false;
            }
          }

          if VUNLIKELY (mapping.schema.empty() && mapping.converter.empty()) {
            MLOG_W("Invalid mapping in {}: missing schema or converter", path);
            return false;
          }

          if VUNLIKELY (mapping.converter == "send_time" && mapping.timestamp_field.empty()) {
            MLOG_W("Invalid mapping in {}: converter 'send_time' requires timestamp_field", path);
            return false;
          }

          loaded_mappings.emplace_back(std::move(mapping));
          return true;
        } catch (const std::exception& e) {
          MLOG_W("Invalid mapping entry in {}: {}", path, e.what());
          return false;
        }
      });

  if VLIKELY (ok) {
    mappings_.insert(mappings_.end(), std::make_move_iterator(loaded_mappings.begin()),
                     std::make_move_iterator(loaded_mappings.end()));
  }

  return ok;
}

const FoxgloveMapping* FoxgloveConverter::find_mapping(std::string_view url, const std::string& ser,
                                                       bool* ambiguous) const {
  struct MappingCache final {
    const FoxgloveConverter* owner{nullptr};
    std::string url;
    std::string ser;
    const FoxgloveMapping* mapping{nullptr};
    bool ambiguous{false};
  };

  thread_local MappingCache cache;

  if (cache.owner == this && cache.url == url && cache.ser == ser) {
    if (ambiguous) {
      *ambiguous = cache.ambiguous;
    }

    return cache.ambiguous ? nullptr : cache.mapping;
  }

  auto mapping_iter = mapping_index_.find(ser);

  if VUNLIKELY (mapping_iter == mapping_index_.end()) {
    cache.owner = this;
    cache.url.assign(url.data(), url.size());
    cache.ser = ser;
    cache.mapping = nullptr;
    cache.ambiguous = false;

    if (ambiguous) {
      *ambiguous = false;
    }

    return nullptr;
  }

  const FoxgloveMapping* best = nullptr;
  int best_score = -1;
  bool has_ambiguity = false;

  for (const auto* mapping : mapping_iter->second) {
    const auto score = score_url_selector(url, mapping->url_selector);

    if VUNLIKELY (score < 0) {
      continue;
    }

    if VLIKELY (score > best_score) {
      best = mapping;
      best_score = score;
      has_ambiguity = false;
      continue;
    }

    if VUNLIKELY (score == best_score) {
      has_ambiguity = true;
    }
  }

  if (ambiguous) {
    *ambiguous = has_ambiguity;
  }

  cache.owner = this;
  cache.url.assign(url.data(), url.size());
  cache.ser = ser;
  cache.mapping = best;
  cache.ambiguous = has_ambiguity;

  if VUNLIKELY (has_ambiguity) {
    MLOG_W("Ambiguous foxglove mapping: url={} ser={}", url, ser);
    return nullptr;
  }

  return best;
}

bool FoxgloveConverter::resolve_proto_schema(const std::string& proto_name, std::string& schema_data) {
  std::lock_guard lock(mtx_);

  auto cache_key = "vlink:" + proto_name;
  auto cache_iter = schema_cache_.find(cache_key);

  if VLIKELY (cache_iter != schema_cache_.end()) {
    schema_data = cache_iter->second;
    return true;
  }

  if VLIKELY (schema_interface_) {
    auto schema_record = schema_interface_->search_schema(proto_name, SchemaType::kProtobuf);
    if VLIKELY (schema_record.schema_type == SchemaType::kProtobuf && !schema_record.data.empty()) {
      schema_data.assign(reinterpret_cast<const char*>(schema_record.data.data()), schema_record.data.size());
      schema_cache_[cache_key] = schema_data;
      return true;
    }
  }

#ifdef VLINK_HAS_PROTO_COMPILER
  {
    auto iter = imported_proto_descriptors_.find(proto_name);

    if VLIKELY (iter != imported_proto_descriptors_.end()) {
      const auto* desc = iter->second;

      if VLIKELY (desc && desc->file()) {
        std::vector<const google::protobuf::FileDescriptor*> ordered;
#if GOOGLE_PROTOBUF_VERSION >= 6030000
        std::unordered_set<std::string_view> seen;
#else
        std::unordered_set<std::string> seen;
#endif

        MoveFunction<void(const google::protobuf::FileDescriptor*)> dfs;

        dfs = [&ordered, &seen, &dfs](const google::protobuf::FileDescriptor* fd) {
#if GOOGLE_PROTOBUF_VERSION >= 6030000
          std::string_view name = fd->name();
#else
          const std::string& name = fd->name();
#endif

          if (seen.count(name)) {
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

        schema_cache_[cache_key] = schema_data;
        return true;
      }  // NOLINT(modernize-loop-convert)
    }
  }
#endif

  return false;
}

bool FoxgloveConverter::resolve_fbs_schema(const std::string& schema_name, std::string& schema_data) {
  std::lock_guard lock(mtx_);

  auto cache_key = "fbs:" + schema_name;
  auto cache_iter = schema_cache_.find(cache_key);

  if VLIKELY (cache_iter != schema_cache_.end()) {
    schema_data = cache_iter->second;
    return true;
  }

  const uint8_t* bfbs_data = nullptr;
  size_t bfbs_size = 0;

  // clang-format off
  // NOLINTBEGIN
  static const std::unordered_map<std::string, std::pair<const uint8_t*, size_t>> kSchemaRegistry = {
    {"foxglove.CameraCalibration",  {::foxglove::CameraCalibrationBinarySchema::data(),  ::foxglove::CameraCalibrationBinarySchema::size()}},
    {"foxglove.CompressedImage",    {::foxglove::CompressedImageBinarySchema::data(),     ::foxglove::CompressedImageBinarySchema::size()}},
    {"foxglove.CompressedVideo",    {::foxglove::CompressedVideoBinarySchema::data(),     ::foxglove::CompressedVideoBinarySchema::size()}},
    {"foxglove.FrameTransform",     {::foxglove::FrameTransformBinarySchema::data(),      ::foxglove::FrameTransformBinarySchema::size()}},
    {"foxglove.FrameTransforms",    {::foxglove::FrameTransformsBinarySchema::data(),     ::foxglove::FrameTransformsBinarySchema::size()}},
    {"foxglove.GeoJSON",            {::foxglove::GeoJSONBinarySchema::data(),             ::foxglove::GeoJSONBinarySchema::size()}},
    {"foxglove.Grid",               {::foxglove::GridBinarySchema::data(),                ::foxglove::GridBinarySchema::size()}},
    {"foxglove.ImageAnnotations",   {::foxglove::ImageAnnotationsBinarySchema::data(),    ::foxglove::ImageAnnotationsBinarySchema::size()}},
    {"foxglove.JointStates",        {::foxglove::JointStatesBinarySchema::data(),         ::foxglove::JointStatesBinarySchema::size()}},
    {"foxglove.LaserScan",          {::foxglove::LaserScanBinarySchema::data(),           ::foxglove::LaserScanBinarySchema::size()}},
    {"foxglove.LocationFix",        {::foxglove::LocationFixBinarySchema::data(),         ::foxglove::LocationFixBinarySchema::size()}},
    {"foxglove.LocationFixes",      {::foxglove::LocationFixesBinarySchema::data(),       ::foxglove::LocationFixesBinarySchema::size()}},
    {"foxglove.Log",                {::foxglove::LogBinarySchema::data(),                 ::foxglove::LogBinarySchema::size()}},
    {"foxglove.Point3InFrame",      {::foxglove::Point3InFrameBinarySchema::data(),       ::foxglove::Point3InFrameBinarySchema::size()}},
    {"foxglove.PointCloud",         {::foxglove::PointCloudBinarySchema::data(),          ::foxglove::PointCloudBinarySchema::size()}},
    {"foxglove.PoseInFrame",        {::foxglove::PoseInFrameBinarySchema::data(),         ::foxglove::PoseInFrameBinarySchema::size()}},
    {"foxglove.PosesInFrame",       {::foxglove::PosesInFrameBinarySchema::data(),        ::foxglove::PosesInFrameBinarySchema::size()}},
    {"foxglove.RawAudio",           {::foxglove::RawAudioBinarySchema::data(),            ::foxglove::RawAudioBinarySchema::size()}},
    {"foxglove.RawImage",           {::foxglove::RawImageBinarySchema::data(),            ::foxglove::RawImageBinarySchema::size()}},
    {"foxglove.SceneUpdate",        {::foxglove::SceneUpdateBinarySchema::data(),         ::foxglove::SceneUpdateBinarySchema::size()}},
    {"foxglove.VoxelGrid",          {::foxglove::VoxelGridBinarySchema::data(),           ::foxglove::VoxelGridBinarySchema::size()}},
  };
  // NOLINTEND
  // clang-format on

  auto reg_iter = kSchemaRegistry.find(schema_name);

  if VLIKELY (reg_iter != kSchemaRegistry.end()) {
    bfbs_data = reg_iter->second.first;
    bfbs_size = reg_iter->second.second;
  }

  if VUNLIKELY (!bfbs_data || bfbs_size == 0) {
    MLOG_W("No embedded schema for: {}", schema_name);
    return false;
  }

  schema_data.assign(reinterpret_cast<const char*>(bfbs_data), bfbs_size);
  schema_cache_[cache_key] = schema_data;
  return true;
}

FoxgloveMessage FoxgloveConverter::convert_proto_mapping(const FoxgloveMapping& mapping, const std::string& ser,
                                                         const Bytes& raw) {
  auto msg = deserialize_proto_message(ser, raw);

  if VUNLIKELY (!msg) {
    FoxgloveMessage result;
    return result;
  }

  auto extract_ts = [&mapping, &msg](FoxgloveMessage& result) {
    if VLIKELY (!mapping.timestamp_field.empty() && result.success) {
      result.timestamp_ns = extract_proto_timestamp_ns(*msg, mapping.timestamp_field, mapping.timestamp_unit);
    }

    return result;
  };

  using ProtoConvertFn = FoxgloveMessage (*)(const FoxgloveMapping&, const google::protobuf::Message&);
  static const std::unordered_map<std::string, ProtoConvertFn> kProtoDispatch = {
      {"foxglove.LocationFix", convert_location_fix},
      {"foxglove.PoseInFrame", convert_pose_in_frame},
      {"foxglove.SceneUpdate", convert_scene_update},
      {"foxglove.FrameTransform", convert_frame_transform},
      {"foxglove.Log", convert_log},
      {"foxglove.LaserScan", convert_laser_scan},
      {"foxglove.RawImage", convert_raw_image},
      {"foxglove.GeoJSON", convert_geo_json},
      {"foxglove.PosesInFrame", convert_poses_in_frame},
      {"foxglove.FrameTransforms", convert_frame_transforms},
      {"foxglove.LocationFixes", convert_location_fixes},
      {"foxglove.CameraCalibration", convert_camera_calibration},
      {"foxglove.CompressedVideo", convert_compressed_video},
      {"foxglove.Grid", convert_grid},
      {"foxglove.ImageAnnotations", convert_image_annotations},
      {"foxglove.JointStates", convert_joint_states},
      {"foxglove.Point3InFrame", convert_point3_in_frame},
      {"foxglove.RawAudio", convert_raw_audio},
      {"foxglove.VoxelGrid", convert_voxel_grid},
  };

  auto dispatch_iter = kProtoDispatch.find(mapping.schema);

  if (dispatch_iter != kProtoDispatch.end()) {
    auto result = dispatch_iter->second(mapping, *msg);
    return extract_ts(result);
  }

  MLOG_W("Unsupported target schema: {}", mapping.schema);
  FoxgloveMessage result;
  return result;
}

FoxgloveMessage FoxgloveConverter::convert_location_fix(const FoxgloveMapping& mapping,
                                                        const google::protobuf::Message& msg) {
  FoxgloveMessage result;
  thread_local flatbuffers::FlatBufferBuilder builder(4096);
  builder.Clear();

  double latitude = 0.0;
  double longitude = 0.0;
  double altitude = 0.0;
  uint64_t timestamp_us = 0;
  uint64_t timestamp_ns = 0;
  std::string frame_id;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "timestamp") {
      timestamp_us = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "timestamp_ns") {
      timestamp_ns = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "frame_id") {
      frame_id = get_proto_string(msg, fm.source, fm);
    } else if (fm.target == "latitude") {
      latitude = get_proto_double(msg, fm.source, fm);
    } else if (fm.target == "longitude") {
      longitude = get_proto_double(msg, fm.source, fm);
    } else if (fm.target == "altitude") {
      altitude = get_proto_double(msg, fm.source, fm);
    }
  }

  auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
  auto fid = builder.CreateString(frame_id);
  auto loc = ::foxglove::CreateLocationFix(builder, &ts, fid, latitude, longitude, altitude);
  builder.Finish(loc);

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.schema_name = "foxglove.LocationFix";
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

FoxgloveMessage FoxgloveConverter::convert_pose_in_frame(const FoxgloveMapping& mapping,
                                                         const google::protobuf::Message& msg) {
  FoxgloveMessage result;
  thread_local flatbuffers::FlatBufferBuilder builder(4096);
  builder.Clear();

  uint64_t timestamp_us = 0;
  uint64_t timestamp_ns = 0;
  std::string frame_id;
  double position_x = 0.0;
  double position_y = 0.0;
  double position_z = 0.0;

  const google::protobuf::Message* orientation_msg = nullptr;
  const google::protobuf::Message* euler_msg = nullptr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "timestamp") {
      timestamp_us = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "timestamp_ns") {
      timestamp_ns = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "frame_id") {
      frame_id = get_proto_string(msg, fm.source, fm);
    } else if (fm.target == "pose" || fm.target == "pose_euler") {
      const google::protobuf::Message* target_msg = nullptr;

      const google::protobuf::Message* target_parent = nullptr;
      std::string target_field_name;

      if (resolve_proto_parent_field_path(msg, fm.source, target_parent, target_field_name)) {
        const auto* desc = target_parent->GetDescriptor();
        const auto* ref = target_parent->GetReflection();
        const auto* field = find_proto_field_cached(*desc, target_field_name);

        if (field && field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE &&
            (!field->has_presence() || ref->HasField(*target_parent, field))) {
          target_msg = &ref->GetMessage(*target_parent, field);
        }
      }

      if (target_msg) {
        if (fm.target == "pose") {
          orientation_msg = target_msg;
        } else {
          euler_msg = target_msg;
        }
      }
    } else if (fm.target == "position_x") {
      position_x = get_proto_double(msg, fm.source, fm);
    } else if (fm.target == "position_y") {
      position_y = get_proto_double(msg, fm.source, fm);
    } else if (fm.target == "position_z") {
      position_z = get_proto_double(msg, fm.source, fm);
    }
  }

  auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
  auto fid = builder.CreateString(frame_id);

  flatbuffers::Offset<::foxglove::Pose> pose_offset = 0;

  if (orientation_msg) {
    auto get_field = [&orientation_msg](const char* name) -> double {
      const auto* f = find_proto_field_cached(*orientation_msg->GetDescriptor(), name);

      if VUNLIKELY (!f || !is_proto_numeric_type(f->cpp_type())) {
        return 0.0;
      }

      return get_proto_numeric_value(*orientation_msg, f);
    };

    double qx = get_field("x");
    double qy = get_field("y");
    double qz = get_field("z");
    double qw = get_field("w");

    if VUNLIKELY (qx == 0.0 && qy == 0.0 && qz == 0.0 && qw == 0.0) {
      qw = 1.0;
    }

    auto orientation = ::foxglove::CreateQuaternion(builder, qx, qy, qz, qw);
    auto position = ::foxglove::CreateVector3(builder, position_x, position_y, position_z);
    pose_offset = ::foxglove::CreatePose(builder, position, orientation);
  } else if (euler_msg) {
    auto get_euler = [&euler_msg](const char* name) -> double {
      const auto* f = find_proto_field_cached(*euler_msg->GetDescriptor(), name);

      if VUNLIKELY (!f || !is_proto_numeric_type(f->cpp_type())) {
        return 0.0;
      }

      return get_proto_numeric_value(*euler_msg, f);
    };

    double roll = get_euler("x");
    double pitch = get_euler("y");
    double yaw = get_euler("z");

    double cr = std::cos(roll * 0.5);
    double sr = std::sin(roll * 0.5);
    double cp = std::cos(pitch * 0.5);
    double sp = std::sin(pitch * 0.5);
    double cy = std::cos(yaw * 0.5);
    double sy = std::sin(yaw * 0.5);

    double qw = cr * cp * cy + sr * sp * sy;
    double qx = sr * cp * cy - cr * sp * sy;
    double qy = cr * sp * cy + sr * cp * sy;
    double qz = cr * cp * sy - sr * sp * cy;

    auto orientation = ::foxglove::CreateQuaternion(builder, qx, qy, qz, qw);
    auto position = ::foxglove::CreateVector3(builder, position_x, position_y, position_z);
    pose_offset = ::foxglove::CreatePose(builder, position, orientation);
  } else if (position_x != 0.0 || position_y != 0.0 || position_z != 0.0) {
    auto orientation = ::foxglove::CreateQuaternion(builder, 0.0, 0.0, 0.0, 1.0);
    auto position = ::foxglove::CreateVector3(builder, position_x, position_y, position_z);
    pose_offset = ::foxglove::CreatePose(builder, position, orientation);
  }

  auto pif = ::foxglove::CreatePoseInFrame(builder, &ts, fid, pose_offset);
  builder.Finish(pif);

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.schema_name = "foxglove.PoseInFrame";
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

FoxgloveMessage FoxgloveConverter::convert_scene_update(const FoxgloveMapping& mapping,
                                                        const google::protobuf::Message& msg) {
  FoxgloveMessage result;
  thread_local flatbuffers::FlatBufferBuilder builder(16 * 1024);
  builder.Clear();

  uint64_t timestamp_us = 0;
  uint64_t timestamp_ns = 0;
  std::string frame_id = "base_link";

  std::string entity_sub_items;
  std::string entity_x_src;
  std::string entity_y_src;
  std::string entity_z_src;
  std::string entity_w_src;
  std::string entity_l_src;
  std::string entity_h_src;
  std::string entity_heading_src;
  std::string entity_x_expr;
  std::string entity_y_expr;
  std::string entity_z_expr;
  std::string entity_w_expr;
  std::string entity_l_expr;
  std::string entity_h_expr;
  std::string entity_heading_expr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "timestamp") {
      timestamp_us = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "timestamp_ns") {
      timestamp_ns = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "frame_id") {
      frame_id = get_proto_string(msg, fm.source, fm);
    } else if (fm.target == "entity_sub_items") {
      entity_sub_items = fm.source;
    } else if (fm.target == "entity_x") {
      entity_x_src = fm.source;
      entity_x_expr = fm.expression;
    } else if (fm.target == "entity_y") {
      entity_y_src = fm.source;
      entity_y_expr = fm.expression;
    } else if (fm.target == "entity_z") {
      entity_z_src = fm.source;
      entity_z_expr = fm.expression;
    } else if (fm.target == "entity_width") {
      entity_w_src = fm.source;
      entity_w_expr = fm.expression;
    } else if (fm.target == "entity_length") {
      entity_l_src = fm.source;
      entity_l_expr = fm.expression;
    } else if (fm.target == "entity_height") {
      entity_h_src = fm.source;
      entity_h_expr = fm.expression;
    } else if (fm.target == "entity_heading") {
      entity_heading_src = fm.source;
      entity_heading_expr = fm.expression;
    }
  }

  bool has_entity_fields = !entity_x_src.empty() || !entity_y_src.empty() || !entity_z_src.empty();

  auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);

  std::vector<flatbuffers::Offset<::foxglove::SceneEntity>> entity_offsets;

  auto try_get_field = [](const google::protobuf::Message& m, const std::string& name,
                          const std::string& expr) -> double {
    if (!expr.empty()) {
      return evaluate_expression_with_msg(expr, m);
    }

    if (has_nested_field_path(name)) {
      return safe_nested_double(m, name);
    }

    const auto* d = m.GetDescriptor();
    const auto* f = find_proto_field_cached(*d, name);

    if VUNLIKELY (!f) {
      return 0.0;
    }

    return get_proto_numeric_value(m, f);
  };

  auto build_cube = [&entity_h_expr, &entity_h_src, &entity_heading_expr, &entity_heading_src, &entity_l_expr,
                     &entity_l_src, &entity_offsets, &entity_w_expr, &entity_w_src, &entity_x_expr, &entity_x_src,
                     &entity_y_expr, &entity_y_src, &entity_z_expr, &entity_z_src, &frame_id, &has_entity_fields,
                     &try_get_field, &ts](const google::protobuf::Message& sub, int idx, const std::string& parent_id) {
    double px = 0.0;
    double py = 0.0;
    double pz = 0.0;
    double sx = 1.0;
    double sy = 1.0;
    double sz = 1.0;
    double heading = 0.0;

    if (has_entity_fields) {
      if (!entity_x_src.empty()) {
        px = try_get_field(sub, entity_x_src, entity_x_expr);
      }

      if (!entity_y_src.empty()) {
        py = try_get_field(sub, entity_y_src, entity_y_expr);
      }

      if (!entity_z_src.empty()) {
        pz = try_get_field(sub, entity_z_src, entity_z_expr);
      }

      if (!entity_w_src.empty()) {
        auto v = try_get_field(sub, entity_w_src, entity_w_expr);

        if (v != 0.0) {
          sx = v;
        }
      }

      if (!entity_l_src.empty()) {
        auto v = try_get_field(sub, entity_l_src, entity_l_expr);

        if (v != 0.0) {
          sy = v;
        }
      }

      if (!entity_h_src.empty()) {
        auto v = try_get_field(sub, entity_h_src, entity_h_expr);

        if (v != 0.0) {
          sz = v;
        }
      }

      if (!entity_heading_src.empty()) {
        heading = try_get_field(sub, entity_heading_src, entity_heading_expr);
      }
    } else {
      const auto* sub_desc = sub.GetDescriptor();
      const auto* sub_ref = sub.GetReflection();

      auto direct_get = [&sub](const char* name) -> double {
        const auto* f = find_proto_field_cached(*sub.GetDescriptor(), name);

        if VUNLIKELY (!f || !is_proto_numeric_type(f->cpp_type())) {
          return 0.0;
        }

        return get_proto_numeric_value(sub, f);
      };

      px = direct_get("x");
      py = direct_get("y");
      pz = direct_get("z");

      if (px == 0.0 && py == 0.0 && pz == 0.0) {
        px = direct_get("cx");
        py = direct_get("cy");
        pz = direct_get("cz");
      }

      if (px == 0.0 && py == 0.0 && pz == 0.0) {
        const auto* pos_field = find_proto_field_cached(*sub_desc, "position");

        if (pos_field && pos_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
          const auto& pos_msg = sub_ref->GetMessage(sub, pos_field);
          auto pos_get = [&pos_msg](const char* name) -> double {
            const auto* f = find_proto_field_cached(*pos_msg.GetDescriptor(), name);

            if VUNLIKELY (!f || !is_proto_numeric_type(f->cpp_type())) {
              return 0.0;
            }

            return get_proto_numeric_value(pos_msg, f);
          };

          px = pos_get("x");
          py = pos_get("y");
          pz = pos_get("z");
        }
      }

      auto w_val = direct_get("width");
      auto l_val = direct_get("length");
      auto h_val = direct_get("height");

      if (w_val != 0.0) {
        sx = w_val;
      }

      if (l_val != 0.0) {
        sy = l_val;
      }

      if (h_val != 0.0) {
        sz = h_val;
      }

      heading = direct_get("heading_angle");

      if (heading == 0.0) {
        heading = direct_get("yaw");
      }
    }

    double qx = 0.0;
    double qy = 0.0;
    double qz = std::sin(heading * 0.5);
    double qw = std::cos(heading * 0.5);

    auto entity_fid = builder.CreateString(frame_id);
    auto entity_id = builder.CreateString(parent_id + "_" + std::to_string(idx));

    auto position = ::foxglove::CreateVector3(builder, px, py, pz);
    auto orientation = ::foxglove::CreateQuaternion(builder, qx, qy, qz, qw);
    auto pose = ::foxglove::CreatePose(builder, position, orientation);
    auto size = ::foxglove::CreateVector3(builder, sx, sy, sz);
    auto color_offset = ::foxglove::CreateColor(builder, 0.2, 0.8, 0.2, 0.8);

    auto cube = ::foxglove::CreateCubePrimitive(builder, pose, size, color_offset);
    std::vector<flatbuffers::Offset<::foxglove::CubePrimitive>> cubes_vec_data = {cube};
    auto cubes_vec = builder.CreateVector(cubes_vec_data);

    auto entity = ::foxglove::CreateSceneEntity(builder, &ts, entity_fid, entity_id, nullptr, false, 0, 0, cubes_vec);
    entity_offsets.emplace_back(entity);
  };

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target != "entities") {
      continue;
    }

    const google::protobuf::Message* entities_parent = nullptr;
    std::string entities_field_name;

    if VUNLIKELY (!resolve_proto_parent_field_path(msg, fm.source, entities_parent, entities_field_name)) {
      continue;
    }

    if VUNLIKELY (!entities_parent) {
      continue;
    }

    const auto* desc = entities_parent->GetDescriptor();
    const auto* ref = entities_parent->GetReflection();
    const auto* field = find_proto_field_cached(*desc, entities_field_name);

    if VUNLIKELY (!field || !field->is_repeated()) {
      continue;
    }

    int count = ref->FieldSize(*entities_parent, field);

    for (int i = 0; i < count; ++i) {
      const auto& item = ref->GetRepeatedMessage(*entities_parent, field, i);

      if (!entity_sub_items.empty()) {
        const auto* item_desc = item.GetDescriptor();
        const auto* item_ref = item.GetReflection();
        const auto* sub_field = find_proto_field_cached(*item_desc, entity_sub_items);

        if (sub_field && sub_field->is_repeated()) {
          int sub_count = item_ref->FieldSize(item, sub_field);

          for (int j = 0; j < sub_count; ++j) {
            const auto& sub_item = item_ref->GetRepeatedMessage(item, sub_field, j);
            build_cube(sub_item, j, std::to_string(i));
          }
        }
      } else {
        build_cube(item, i, "e");
      }
    }
  }

  auto entities_vec = builder.CreateVector(entity_offsets);
  auto scene = ::foxglove::CreateSceneUpdate(builder, 0, entities_vec);
  builder.Finish(scene);

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.schema_name = "foxglove.SceneUpdate";
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

FoxgloveMessage FoxgloveConverter::convert_frame_transform(const FoxgloveMapping& mapping,
                                                           const google::protobuf::Message& msg) {
  FoxgloveMessage result;
  thread_local flatbuffers::FlatBufferBuilder builder(4096);
  builder.Clear();

  uint64_t timestamp_us = 0;
  uint64_t timestamp_ns = 0;
  std::string parent_frame_id;
  std::string child_frame_id;
  double tx = 0.0;
  double ty = 0.0;
  double tz = 0.0;
  double qx = 0.0;
  double qy = 0.0;
  double qz = 0.0;
  double qw = 1.0;
  bool has_euler = false;
  double euler_roll = 0.0;
  double euler_pitch = 0.0;
  double euler_yaw = 0.0;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "timestamp") {
      timestamp_us = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "timestamp_ns") {
      timestamp_ns = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "parent_frame_id") {
      parent_frame_id = get_proto_string(msg, fm.source, fm);
    } else if (fm.target == "child_frame_id") {
      child_frame_id = get_proto_string(msg, fm.source, fm);
    } else if (fm.target == "translation_x") {
      tx = get_proto_double(msg, fm.source, fm);
    } else if (fm.target == "translation_y") {
      ty = get_proto_double(msg, fm.source, fm);
    } else if (fm.target == "translation_z") {
      tz = get_proto_double(msg, fm.source, fm);
    } else if (fm.target == "rotation_x") {
      qx = get_proto_double(msg, fm.source, fm);
    } else if (fm.target == "rotation_y") {
      qy = get_proto_double(msg, fm.source, fm);
    } else if (fm.target == "rotation_z") {
      qz = get_proto_double(msg, fm.source, fm);
    } else if (fm.target == "rotation_w") {
      qw = get_proto_double(msg, fm.source, fm);
    } else if (fm.target == "euler_roll") {
      euler_roll = get_proto_double(msg, fm.source, fm);
      has_euler = true;
    } else if (fm.target == "euler_pitch") {
      euler_pitch = get_proto_double(msg, fm.source, fm);
      has_euler = true;
    } else if (fm.target == "euler_yaw") {
      euler_yaw = get_proto_double(msg, fm.source, fm);
      has_euler = true;
    }
  }

  if (has_euler) {
    double cr = std::cos(euler_roll * 0.5);
    double sr = std::sin(euler_roll * 0.5);
    double cp = std::cos(euler_pitch * 0.5);
    double sp = std::sin(euler_pitch * 0.5);
    double cy = std::cos(euler_yaw * 0.5);
    double sy = std::sin(euler_yaw * 0.5);

    qw = cr * cp * cy + sr * sp * sy;
    qx = sr * cp * cy - cr * sp * sy;
    qy = cr * sp * cy + sr * cp * sy;
    qz = cr * cp * sy - sr * sp * cy;
  }

  auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
  auto pfid = builder.CreateString(parent_frame_id);
  auto cfid = builder.CreateString(child_frame_id);
  auto translation = ::foxglove::CreateVector3(builder, tx, ty, tz);
  auto rotation = ::foxglove::CreateQuaternion(builder, qx, qy, qz, qw);
  auto ft = ::foxglove::CreateFrameTransform(builder, &ts, pfid, cfid, translation, rotation);
  builder.Finish(ft);

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.schema_name = "foxglove.FrameTransform";
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

FoxgloveMessage FoxgloveConverter::convert_log(const FoxgloveMapping& mapping, const google::protobuf::Message& msg) {
  FoxgloveMessage result;
  thread_local flatbuffers::FlatBufferBuilder builder(4096);
  builder.Clear();

  uint64_t timestamp_us = 0;
  uint64_t timestamp_ns = 0;
  std::string level_str;
  std::string message;
  std::string name;
  std::string file;
  uint32_t line = 0;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "timestamp") {
      timestamp_us = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "timestamp_ns") {
      timestamp_ns = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "level") {
      level_str = get_proto_string(msg, fm.source, fm);
    } else if (fm.target == "message") {
      message = get_proto_string(msg, fm.source, fm);
    } else if (fm.target == "name") {
      name = get_proto_string(msg, fm.source, fm);
    } else if (fm.target == "file") {
      file = get_proto_string(msg, fm.source, fm);
    } else if (fm.target == "line") {
      line = checked_unsigned_cast<uint32_t>(get_proto_double(msg, fm.source, fm));
    }
  }

  auto level_enum = ::foxglove::LogLevel::UNKNOWN;

  if (level_str == "debug") {
    level_enum = ::foxglove::LogLevel::DEBUG;
  } else if (level_str == "info") {
    level_enum = ::foxglove::LogLevel::INFO;
  } else if (level_str == "warning") {
    level_enum = ::foxglove::LogLevel::WARNING;
  } else if (level_str == "error") {
    level_enum = ::foxglove::LogLevel::ERROR;
  } else if (level_str == "fatal") {
    level_enum = ::foxglove::LogLevel::FATAL;
  }

  auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
  auto msg_str = builder.CreateString(message);
  auto name_str = builder.CreateString(name);
  auto file_str = builder.CreateString(file);
  auto log = ::foxglove::CreateLog(builder, &ts, level_enum, msg_str, name_str, file_str, line);
  builder.Finish(log);

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.schema_name = "foxglove.Log";
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

FoxgloveMessage FoxgloveConverter::convert_laser_scan(const FoxgloveMapping& mapping,
                                                      const google::protobuf::Message& msg) {
  FoxgloveMessage result;
  thread_local flatbuffers::FlatBufferBuilder builder(64 * 1024);
  builder.Clear();

  uint64_t timestamp_us = 0;
  uint64_t timestamp_ns = 0;
  std::string frame_id;
  double start_angle = 0.0;
  double end_angle = 0.0;
  std::string ranges_src;
  std::string intensities_src;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "timestamp") {
      timestamp_us = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "timestamp_ns") {
      timestamp_ns = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "frame_id") {
      frame_id = get_proto_string(msg, fm.source, fm);
    } else if (fm.target == "start_angle") {
      start_angle = get_proto_double(msg, fm.source, fm);
    } else if (fm.target == "end_angle") {
      end_angle = get_proto_double(msg, fm.source, fm);
    } else if (fm.target == "ranges") {
      ranges_src = fm.source;
    } else if (fm.target == "intensities") {
      intensities_src = fm.source;
    }
  }

  auto read_proto_double_array = [&msg](const std::string& field_name) -> std::vector<double> {
    std::vector<double> out;

    if (field_name.empty()) {
      return out;
    }

    const auto* desc = msg.GetDescriptor();
    const auto* ref = msg.GetReflection();
    const auto* field = find_proto_field_cached(*desc, field_name);

    if VUNLIKELY (!field || !field->is_repeated()) {
      return out;
    }

    int count = ref->FieldSize(msg, field);
    out.reserve(static_cast<size_t>(count));

    for (int i = 0; i < count; ++i) {
      switch (field->cpp_type()) {
        case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
          out.emplace_back(ref->GetRepeatedDouble(msg, field, i));
          break;
        case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
          out.emplace_back(static_cast<double>(ref->GetRepeatedFloat(msg, field, i)));
          break;
        case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
          out.emplace_back(static_cast<double>(ref->GetRepeatedInt32(msg, field, i)));
          break;
        case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
          out.emplace_back(static_cast<double>(ref->GetRepeatedInt64(msg, field, i)));
          break;
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
          out.emplace_back(static_cast<double>(ref->GetRepeatedUInt32(msg, field, i)));
          break;
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
          out.emplace_back(static_cast<double>(ref->GetRepeatedUInt64(msg, field, i)));
          break;
        default:
          break;
      }
    }

    return out;
  };

  auto ranges_data = read_proto_double_array(ranges_src);
  auto intensities_data = read_proto_double_array(intensities_src);

  auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
  auto fid = builder.CreateString(frame_id);
  auto ranges_vec = ranges_data.empty() ? 0 : builder.CreateVector(ranges_data);
  auto intensities_vec = intensities_data.empty() ? 0 : builder.CreateVector(intensities_data);
  auto scan = ::foxglove::CreateLaserScan(builder, &ts, fid, 0, start_angle, end_angle, ranges_vec, intensities_vec);
  builder.Finish(scan);

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.schema_name = "foxglove.LaserScan";
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

FoxgloveMessage FoxgloveConverter::convert_raw_image(const FoxgloveMapping& mapping,
                                                     const google::protobuf::Message& msg) {
  FoxgloveMessage result;
  thread_local flatbuffers::FlatBufferBuilder builder(256 * 1024);
  builder.Clear();

  uint64_t timestamp_us = 0;
  uint64_t timestamp_ns = 0;
  std::string frame_id;
  uint32_t width = 0;
  uint32_t height = 0;
  std::string encoding;
  uint32_t step = 0;
  std::string data_src;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "timestamp") {
      timestamp_us = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "timestamp_ns") {
      timestamp_ns = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "frame_id") {
      frame_id = get_proto_string(msg, fm.source, fm);
    } else if (fm.target == "width") {
      width = checked_unsigned_cast<uint32_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "height") {
      height = checked_unsigned_cast<uint32_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "encoding") {
      encoding = get_proto_string(msg, fm.source, fm);
    } else if (fm.target == "step") {
      step = checked_unsigned_cast<uint32_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "data") {
      data_src = fm.source;
    }
  }

  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> data_vec = 0;

  if (!data_src.empty()) {
    const auto* desc = msg.GetDescriptor();
    const auto* ref = msg.GetReflection();
    const auto* field = find_proto_field_cached(*desc, data_src);

    if (field) {
      if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING) {
        std::string scratch;
        const auto& raw_bytes = ref->GetStringReference(msg, field, &scratch);

        if VLIKELY (!raw_bytes.empty()) {
          data_vec = builder.CreateVector(reinterpret_cast<const uint8_t*>(raw_bytes.data()), raw_bytes.size());
        }
      } else if (field->is_repeated()) {
        data_vec = create_proto_repeated_byte_vector(builder, msg, *field, *ref);
      }
    }
  }

  auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
  auto fid = builder.CreateString(frame_id);
  auto enc = builder.CreateString(encoding);
  auto img = ::foxglove::CreateRawImage(builder, &ts, fid, width, height, enc, step, data_vec);
  builder.Finish(img);

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.schema_name = "foxglove.RawImage";
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

FoxgloveMessage FoxgloveConverter::convert_geo_json(const FoxgloveMapping& mapping,
                                                    const google::protobuf::Message& msg) {
  FoxgloveMessage result;
  thread_local flatbuffers::FlatBufferBuilder builder(4096);
  builder.Clear();

  std::string geojson;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "geojson") {
      geojson = get_proto_string(msg, fm.source, fm);
    }
  }

  auto geojson_str = builder.CreateString(geojson);
  auto geo = ::foxglove::CreateGeoJSON(builder, geojson_str);
  builder.Finish(geo);

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.schema_name = "foxglove.GeoJSON";
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

FoxgloveMessage FoxgloveConverter::convert_poses_in_frame(const FoxgloveMapping& mapping,
                                                          const google::protobuf::Message& msg) {
  FoxgloveMessage result;
  thread_local flatbuffers::FlatBufferBuilder builder(16 * 1024);
  builder.Clear();

  uint64_t timestamp_us = 0;
  uint64_t timestamp_ns = 0;
  std::string frame_id;
  std::string poses_src;
  std::string pose_px_src;
  std::string pose_py_src;
  std::string pose_pz_src;
  std::string pose_qx_src;
  std::string pose_qy_src;
  std::string pose_qz_src;
  std::string pose_qw_src;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "timestamp") {
      timestamp_us = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "timestamp_ns") {
      timestamp_ns = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "frame_id") {
      frame_id = get_proto_string(msg, fm.source, fm);
    } else if (fm.target == "poses") {
      poses_src = fm.source;
    } else if (fm.target == "pose_position_x") {
      pose_px_src = fm.source;
    } else if (fm.target == "pose_position_y") {
      pose_py_src = fm.source;
    } else if (fm.target == "pose_position_z") {
      pose_pz_src = fm.source;
    } else if (fm.target == "pose_orientation_x") {
      pose_qx_src = fm.source;
    } else if (fm.target == "pose_orientation_y") {
      pose_qy_src = fm.source;
    } else if (fm.target == "pose_orientation_z") {
      pose_qz_src = fm.source;
    } else if (fm.target == "pose_orientation_w") {
      pose_qw_src = fm.source;
    }
  }

  auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
  std::vector<flatbuffers::Offset<::foxglove::Pose>> pose_offsets;

  if (!poses_src.empty()) {
    const google::protobuf::Message* poses_parent = nullptr;
    std::string poses_field_name;

    if VUNLIKELY (!resolve_proto_parent_field_path(msg, poses_src, poses_parent, poses_field_name)) {
      poses_parent = nullptr;
    }

    if (poses_parent) {
      const auto* desc = poses_parent->GetDescriptor();
      const auto* ref = poses_parent->GetReflection();
      const auto* field = find_proto_field_cached(*desc, poses_field_name);

      if (field && field->is_repeated()) {
        int count = ref->FieldSize(*poses_parent, field);

        for (int i = 0; i < count; ++i) {
          const auto& item = ref->GetRepeatedMessage(*poses_parent, field, i);

          auto direct_get = [&item](const std::string& name) -> double {
            if (name.empty()) {
              return 0.0;
            }

            if (has_nested_field_path(name)) {
              return safe_nested_double(item, name);
            }

            const auto* f = find_proto_field_cached(*item.GetDescriptor(), name);

            if VUNLIKELY (!f || !is_proto_numeric_type(f->cpp_type())) {
              return 0.0;
            }

            return get_proto_numeric_value(item, f);
          };

          double px = direct_get(pose_px_src);
          double py = direct_get(pose_py_src);
          double pz = direct_get(pose_pz_src);
          double qx = direct_get(pose_qx_src);
          double qy = direct_get(pose_qy_src);
          double qz = direct_get(pose_qz_src);
          double qw = pose_qw_src.empty() ? 1.0 : direct_get(pose_qw_src);

          auto position = ::foxglove::CreateVector3(builder, px, py, pz);
          auto orientation = ::foxglove::CreateQuaternion(builder, qx, qy, qz, qw);
          pose_offsets.emplace_back(::foxglove::CreatePose(builder, position, orientation));
        }
      }
    }
  }

  auto fid = builder.CreateString(frame_id);
  auto poses_vec = builder.CreateVector(pose_offsets);
  auto pif = ::foxglove::CreatePosesInFrame(builder, &ts, fid, poses_vec);
  builder.Finish(pif);

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.schema_name = "foxglove.PosesInFrame";
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

FoxgloveMessage FoxgloveConverter::convert_frame_transforms(const FoxgloveMapping& mapping,
                                                            const google::protobuf::Message& msg) {
  FoxgloveMessage result;
  thread_local flatbuffers::FlatBufferBuilder builder(16 * 1024);
  builder.Clear();

  std::string transforms_src;
  std::string ft_ts_src;
  std::string ft_ts_ns_src;
  std::string ft_parent_src;
  std::string ft_child_src;
  std::string ft_tx_src;
  std::string ft_ty_src;
  std::string ft_tz_src;
  std::string ft_qx_src;
  std::string ft_qy_src;
  std::string ft_qz_src;
  std::string ft_qw_src;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "transforms") {
      transforms_src = fm.source;
    } else if (fm.target == "transform_timestamp") {
      ft_ts_src = fm.source;
    } else if (fm.target == "transform_timestamp_ns") {
      ft_ts_ns_src = fm.source;
    } else if (fm.target == "transform_parent_frame_id") {
      ft_parent_src = fm.source;
    } else if (fm.target == "transform_child_frame_id") {
      ft_child_src = fm.source;
    } else if (fm.target == "transform_translation_x") {
      ft_tx_src = fm.source;
    } else if (fm.target == "transform_translation_y") {
      ft_ty_src = fm.source;
    } else if (fm.target == "transform_translation_z") {
      ft_tz_src = fm.source;
    } else if (fm.target == "transform_rotation_x") {
      ft_qx_src = fm.source;
    } else if (fm.target == "transform_rotation_y") {
      ft_qy_src = fm.source;
    } else if (fm.target == "transform_rotation_z") {
      ft_qz_src = fm.source;
    } else if (fm.target == "transform_rotation_w") {
      ft_qw_src = fm.source;
    }
  }

  std::vector<flatbuffers::Offset<::foxglove::FrameTransform>> transform_offsets;

  if (!transforms_src.empty()) {
    const auto* desc = msg.GetDescriptor();
    const auto* ref = msg.GetReflection();
    const auto* field = find_proto_field_cached(*desc, transforms_src);

    if (field && field->is_repeated()) {
      int count = ref->FieldSize(msg, field);

      for (int i = 0; i < count; ++i) {
        const auto& item = ref->GetRepeatedMessage(msg, field, i);

        auto direct_get = [&item](const std::string& name) -> double {
          if (name.empty()) {
            return 0.0;
          }

          if (has_nested_field_path(name)) {
            return safe_nested_double(item, name);
          }

          const auto* f = find_proto_field_cached(*item.GetDescriptor(), name);

          if VUNLIKELY (!f || !is_proto_numeric_type(f->cpp_type())) {
            return 0.0;
          }

          return get_proto_numeric_value(item, f);
        };

        auto direct_get_str = [&item](const std::string& name) -> std::string {
          if (name.empty()) {
            return {};
          }

          if (has_nested_field_path(name)) {
            return resolve_nested_string(item, name);
          }

          const auto* f = find_proto_field_cached(*item.GetDescriptor(), name);

          if VUNLIKELY (!f || f->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_STRING) {
            return {};
          }

          return item.GetReflection()->GetString(item, f);
        };

        auto item_ts_us = checked_unsigned_cast<uint64_t>(direct_get(ft_ts_src));
        auto item_ts_ns = checked_unsigned_cast<uint64_t>(direct_get(ft_ts_ns_src));
        std::string parent_fid = direct_get_str(ft_parent_src);
        std::string child_fid = direct_get_str(ft_child_src);
        double itx = direct_get(ft_tx_src);
        double ity = direct_get(ft_ty_src);
        double itz = direct_get(ft_tz_src);
        double iqx = direct_get(ft_qx_src);
        double iqy = direct_get(ft_qy_src);
        double iqz = direct_get(ft_qz_src);
        double iqw = ft_qw_src.empty() ? 1.0 : direct_get(ft_qw_src);

        auto item_ts = (item_ts_ns > 0) ? make_timestamp_from_ns(item_ts_ns) : make_timestamp_from_us(item_ts_us);
        auto pfid = builder.CreateString(parent_fid);
        auto cfid = builder.CreateString(child_fid);
        auto translation = ::foxglove::CreateVector3(builder, itx, ity, itz);
        auto rotation = ::foxglove::CreateQuaternion(builder, iqx, iqy, iqz, iqw);
        transform_offsets.emplace_back(
            ::foxglove::CreateFrameTransform(builder, &item_ts, pfid, cfid, translation, rotation));
      }
    }
  }

  auto transforms_vec = builder.CreateVector(transform_offsets);
  auto fts = ::foxglove::CreateFrameTransforms(builder, transforms_vec);
  builder.Finish(fts);

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.schema_name = "foxglove.FrameTransforms";
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

FoxgloveMessage FoxgloveConverter::convert_location_fixes(const FoxgloveMapping& mapping,
                                                          const google::protobuf::Message& msg) {
  FoxgloveMessage result;
  thread_local flatbuffers::FlatBufferBuilder builder(16 * 1024);
  builder.Clear();

  std::string fixes_src;
  std::string fix_ts_src;
  std::string fix_ts_ns_src;
  std::string fix_frame_id_src;
  std::string fix_lat_src;
  std::string fix_lon_src;
  std::string fix_alt_src;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "fixes") {
      fixes_src = fm.source;
    } else if (fm.target == "fix_timestamp") {
      fix_ts_src = fm.source;
    } else if (fm.target == "fix_timestamp_ns") {
      fix_ts_ns_src = fm.source;
    } else if (fm.target == "fix_frame_id") {
      fix_frame_id_src = fm.source;
    } else if (fm.target == "fix_latitude") {
      fix_lat_src = fm.source;
    } else if (fm.target == "fix_longitude") {
      fix_lon_src = fm.source;
    } else if (fm.target == "fix_altitude") {
      fix_alt_src = fm.source;
    }
  }

  std::vector<flatbuffers::Offset<::foxglove::LocationFix>> fix_offsets;

  if (!fixes_src.empty()) {
    const auto* desc = msg.GetDescriptor();
    const auto* ref = msg.GetReflection();
    const auto* field = find_proto_field_cached(*desc, fixes_src);

    if (field && field->is_repeated()) {
      int count = ref->FieldSize(msg, field);

      for (int i = 0; i < count; ++i) {
        const auto& item = ref->GetRepeatedMessage(msg, field, i);

        auto direct_get = [&item](const std::string& name) -> double {
          if (name.empty()) {
            return 0.0;
          }

          if (has_nested_field_path(name)) {
            return safe_nested_double(item, name);
          }

          const auto* f = find_proto_field_cached(*item.GetDescriptor(), name);

          if VUNLIKELY (!f || !is_proto_numeric_type(f->cpp_type())) {
            return 0.0;
          }

          return get_proto_numeric_value(item, f);
        };

        auto direct_get_str = [&item](const std::string& name) -> std::string {
          if (name.empty()) {
            return {};
          }

          if (has_nested_field_path(name)) {
            return resolve_nested_string(item, name);
          }

          const auto* f = find_proto_field_cached(*item.GetDescriptor(), name);

          if VUNLIKELY (!f || f->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_STRING) {
            return {};
          }

          return item.GetReflection()->GetString(item, f);
        };

        auto item_ts_us = checked_unsigned_cast<uint64_t>(direct_get(fix_ts_src));
        auto item_ts_ns = checked_unsigned_cast<uint64_t>(direct_get(fix_ts_ns_src));
        std::string item_frame_id = direct_get_str(fix_frame_id_src);
        double lat = direct_get(fix_lat_src);
        double lon = direct_get(fix_lon_src);
        double alt = direct_get(fix_alt_src);

        auto item_ts = (item_ts_ns > 0) ? make_timestamp_from_ns(item_ts_ns) : make_timestamp_from_us(item_ts_us);
        auto fid = builder.CreateString(item_frame_id);
        fix_offsets.emplace_back(::foxglove::CreateLocationFix(builder, &item_ts, fid, lat, lon, alt));
      }
    }
  }

  auto fixes_vec = builder.CreateVector(fix_offsets);
  auto lfs = ::foxglove::CreateLocationFixes(builder, fixes_vec);
  builder.Finish(lfs);

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.schema_name = "foxglove.LocationFixes";
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

FoxgloveMessage FoxgloveConverter::convert_camera_calibration(const FoxgloveMapping& mapping,
                                                              const google::protobuf::Message& msg) {
  FoxgloveMessage result;
  thread_local flatbuffers::FlatBufferBuilder builder(4096);
  builder.Clear();

  uint64_t timestamp_us = 0;
  uint64_t timestamp_ns = 0;
  std::string frame_id;
  uint32_t width = 0;
  uint32_t height = 0;
  std::string distortion_model;
  std::string d_src;
  std::string k_src;
  std::string r_src;
  std::string p_src;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "timestamp") {
      timestamp_us = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "timestamp_ns") {
      timestamp_ns = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "frame_id") {
      frame_id = get_proto_string(msg, fm.source, fm);
    } else if (fm.target == "width") {
      width = checked_unsigned_cast<uint32_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "height") {
      height = checked_unsigned_cast<uint32_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "distortion_model") {
      distortion_model = get_proto_string(msg, fm.source, fm);
    } else if (fm.target == "d") {
      d_src = fm.source;
    } else if (fm.target == "k") {
      k_src = fm.source;
    } else if (fm.target == "r") {
      r_src = fm.source;
    } else if (fm.target == "p") {
      p_src = fm.source;
    }
  }

  auto read_proto_double_array = [&msg](const std::string& field_name) -> std::vector<double> {
    std::vector<double> out;

    if (field_name.empty()) {
      return out;
    }

    const auto* desc = msg.GetDescriptor();
    const auto* ref = msg.GetReflection();
    const auto* field = find_proto_field_cached(*desc, field_name);

    if VUNLIKELY (!field || !field->is_repeated()) {
      return out;
    }

    int count = ref->FieldSize(msg, field);
    out.reserve(static_cast<size_t>(count));

    for (int i = 0; i < count; ++i) {
      switch (field->cpp_type()) {
        case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
          out.emplace_back(ref->GetRepeatedDouble(msg, field, i));
          break;
        case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
          out.emplace_back(static_cast<double>(ref->GetRepeatedFloat(msg, field, i)));
          break;
        case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
          out.emplace_back(static_cast<double>(ref->GetRepeatedInt32(msg, field, i)));
          break;
        case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
          out.emplace_back(static_cast<double>(ref->GetRepeatedInt64(msg, field, i)));
          break;
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
          out.emplace_back(static_cast<double>(ref->GetRepeatedUInt32(msg, field, i)));
          break;
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
          out.emplace_back(static_cast<double>(ref->GetRepeatedUInt64(msg, field, i)));
          break;
        default:
          break;
      }
    }

    return out;
  };

  auto d_data = read_proto_double_array(d_src);
  auto k_data = read_proto_double_array(k_src);
  auto r_data = read_proto_double_array(r_src);
  auto p_data = read_proto_double_array(p_src);

  auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
  auto fid = builder.CreateString(frame_id);
  auto dm = builder.CreateString(distortion_model);
  auto d_vec = d_data.empty() ? 0 : builder.CreateVector(d_data);
  auto k_vec = k_data.empty() ? 0 : builder.CreateVector(k_data);
  auto r_vec = r_data.empty() ? 0 : builder.CreateVector(r_data);
  auto p_vec = p_data.empty() ? 0 : builder.CreateVector(p_data);
  auto cal = ::foxglove::CreateCameraCalibration(builder, &ts, fid, width, height, dm, d_vec, k_vec, r_vec, p_vec);
  builder.Finish(cal);

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.schema_name = "foxglove.CameraCalibration";
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

FoxgloveMessage FoxgloveConverter::convert_compressed_video(const FoxgloveMapping& mapping,
                                                            const google::protobuf::Message& msg) {
  FoxgloveMessage result;
  thread_local flatbuffers::FlatBufferBuilder builder(256 * 1024);
  builder.Clear();

  uint64_t timestamp_us = 0;
  uint64_t timestamp_ns = 0;
  std::string frame_id;
  std::string format;
  std::string data_src;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "timestamp") {
      timestamp_us = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "timestamp_ns") {
      timestamp_ns = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "frame_id") {
      frame_id = get_proto_string(msg, fm.source, fm);
    } else if (fm.target == "format") {
      format = get_proto_string(msg, fm.source, fm);
    } else if (fm.target == "data") {
      data_src = fm.source;
    }
  }

  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> data_vec = 0;

  if (!data_src.empty()) {
    const auto* desc = msg.GetDescriptor();
    const auto* ref = msg.GetReflection();
    const auto* field = find_proto_field_cached(*desc, data_src);

    if (field) {
      if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING) {
        std::string scratch;
        const auto& raw_bytes = ref->GetStringReference(msg, field, &scratch);

        if VLIKELY (!raw_bytes.empty()) {
          data_vec = builder.CreateVector(reinterpret_cast<const uint8_t*>(raw_bytes.data()), raw_bytes.size());
        }
      } else if (field->is_repeated()) {
        data_vec = create_proto_repeated_byte_vector(builder, msg, *field, *ref);
      }
    }
  }

  auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
  auto fid = builder.CreateString(frame_id);
  auto fmt = builder.CreateString(format);
  auto cv = ::foxglove::CreateCompressedVideo(builder, &ts, fid, data_vec, fmt);
  builder.Finish(cv);

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.schema_name = "foxglove.CompressedVideo";
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

FoxgloveMessage FoxgloveConverter::convert_grid(const FoxgloveMapping& mapping, const google::protobuf::Message& msg) {
  FoxgloveMessage result;
  thread_local flatbuffers::FlatBufferBuilder builder(64 * 1024);
  builder.Clear();

  uint64_t timestamp_us = 0;
  uint64_t timestamp_ns = 0;
  std::string frame_id;
  uint32_t column_count = 0;
  double cell_size_x = 1.0;
  double cell_size_y = 1.0;
  uint32_t row_stride = 0;
  uint32_t cell_stride = 0;
  std::string fields_src;
  std::string data_src;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "timestamp") {
      timestamp_us = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "timestamp_ns") {
      timestamp_ns = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "frame_id") {
      frame_id = get_proto_string(msg, fm.source, fm);
    } else if (fm.target == "column_count") {
      column_count = checked_unsigned_cast<uint32_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "cell_size_x") {
      cell_size_x = get_proto_double(msg, fm.source, fm);
    } else if (fm.target == "cell_size_y") {
      cell_size_y = get_proto_double(msg, fm.source, fm);
    } else if (fm.target == "row_stride") {
      row_stride = checked_unsigned_cast<uint32_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "cell_stride") {
      cell_stride = checked_unsigned_cast<uint32_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "fields") {
      fields_src = fm.source;
    } else if (fm.target == "data") {
      data_src = fm.source;
    }
  }

  std::vector<flatbuffers::Offset<::foxglove::PackedElementField>> field_offsets;

  if (!fields_src.empty()) {
    const auto* desc = msg.GetDescriptor();
    const auto* ref = msg.GetReflection();
    const auto* field = find_proto_field_cached(*desc, fields_src);

    if (field && field->is_repeated()) {
      int count = ref->FieldSize(msg, field);

      for (int i = 0; i < count; ++i) {
        const auto& item = ref->GetRepeatedMessage(msg, field, i);
        const auto* item_desc = item.GetDescriptor();
        const auto* item_ref = item.GetReflection();

        std::string fname;
        uint32_t foffset = 0;
        uint8_t ftype_val = 0;

        const auto* name_f = find_proto_field_cached(*item_desc, "name");

        if (name_f && name_f->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING) {
          fname = item_ref->GetString(item, name_f);
        }

        const auto* offset_f = find_proto_field_cached(*item_desc, "offset");

        if (offset_f && is_proto_numeric_type(offset_f->cpp_type())) {
          foffset = checked_unsigned_cast<uint32_t>(get_proto_numeric_value(item, offset_f));
        }

        const auto* type_f = find_proto_field_cached(*item_desc, "type");

        if (type_f) {
          if (type_f->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_ENUM) {
            ftype_val = checked_unsigned_cast<uint8_t>(static_cast<double>(item_ref->GetEnumValue(item, type_f)));
          } else if (is_proto_numeric_type(type_f->cpp_type())) {
            ftype_val = checked_unsigned_cast<uint8_t>(get_proto_numeric_value(item, type_f));
          }
        }

        auto fname_off = builder.CreateString(fname);
        field_offsets.emplace_back(::foxglove::CreatePackedElementField(
            builder, fname_off, foffset, static_cast<::foxglove::NumericType>(ftype_val)));
      }
    }
  }

  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> data_vec = 0;

  if (!data_src.empty()) {
    const auto* desc = msg.GetDescriptor();
    const auto* ref = msg.GetReflection();
    const auto* field = find_proto_field_cached(*desc, data_src);

    if (field) {
      if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING) {
        std::string scratch;
        const auto& raw_bytes = ref->GetStringReference(msg, field, &scratch);

        if VLIKELY (!raw_bytes.empty()) {
          data_vec = builder.CreateVector(reinterpret_cast<const uint8_t*>(raw_bytes.data()), raw_bytes.size());
        }
      } else if (field->is_repeated()) {
        data_vec = create_proto_repeated_byte_vector(builder, msg, *field, *ref);
      }
    }
  }

  auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
  auto fid = builder.CreateString(frame_id);
  auto pose = ::foxglove::CreatePose(builder, ::foxglove::CreateVector3(builder, 0.0, 0.0, 0.0),
                                     ::foxglove::CreateQuaternion(builder, 0.0, 0.0, 0.0, 1.0));
  auto cell_sz = ::foxglove::CreateVector2(builder, cell_size_x, cell_size_y);
  auto fields_vec = builder.CreateVector(field_offsets);
  auto grid = ::foxglove::CreateGrid(builder, &ts, fid, pose, column_count, cell_sz, row_stride, cell_stride,
                                     fields_vec, data_vec);
  builder.Finish(grid);

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.schema_name = "foxglove.Grid";
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

FoxgloveMessage FoxgloveConverter::convert_image_annotations(const FoxgloveMapping& mapping,
                                                             const google::protobuf::Message& msg) {
  FoxgloveMessage result;
  thread_local flatbuffers::FlatBufferBuilder builder(16 * 1024);
  builder.Clear();

  uint64_t timestamp_us = 0;
  uint64_t timestamp_ns = 0;
  std::string circles_src;
  std::string points_src;
  std::string texts_src;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "timestamp") {
      timestamp_us = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "timestamp_ns") {
      timestamp_ns = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "circles") {
      circles_src = fm.source;
    } else if (fm.target == "points") {
      points_src = fm.source;
    } else if (fm.target == "texts") {
      texts_src = fm.source;
    }
  }

  auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);

  auto read_repeated_msgs = [&msg](const std::string& field_name) -> std::vector<const google::protobuf::Message*> {
    std::vector<const google::protobuf::Message*> out;

    if (field_name.empty()) {
      return out;
    }

    const auto* desc = msg.GetDescriptor();
    const auto* ref = msg.GetReflection();
    const auto* field = find_proto_field_cached(*desc, field_name);

    if VUNLIKELY (!field || !field->is_repeated()) {
      return out;
    }

    int count = ref->FieldSize(msg, field);

    for (int i = 0; i < count; ++i) {
      out.emplace_back(&ref->GetRepeatedMessage(msg, field, i));
    }

    return out;
  };

  auto get_sub_double = [](const google::protobuf::Message& m, const char* name) -> double {
    const auto* f = find_proto_field_cached(*m.GetDescriptor(), name);

    if VUNLIKELY (!f || !is_proto_numeric_type(f->cpp_type())) {
      return 0.0;
    }

    return get_proto_numeric_value(m, f);
  };

  auto get_sub_string = [](const google::protobuf::Message& m, const char* name) -> std::string {
    const auto* f = find_proto_field_cached(*m.GetDescriptor(), name);

    if VUNLIKELY (!f || f->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_STRING) {
      return {};
    }

    return m.GetReflection()->GetString(m, f);
  };

  std::vector<flatbuffers::Offset<::foxglove::CircleAnnotation>> circle_offsets;

  for (const auto* item : read_repeated_msgs(circles_src)) {
    double cx = get_sub_double(*item, "x");
    double cy = get_sub_double(*item, "y");
    double diameter = get_sub_double(*item, "diameter");
    double thickness = get_sub_double(*item, "thickness");

    auto pos = ::foxglove::CreatePoint2(builder, cx, cy);
    auto fill = ::foxglove::CreateColor(builder, 1.0, 0.0, 0.0, 0.5);
    auto outline = ::foxglove::CreateColor(builder, 1.0, 0.0, 0.0, 1.0);
    circle_offsets.emplace_back(
        ::foxglove::CreateCircleAnnotation(builder, &ts, pos, diameter, thickness, fill, outline));
  }

  std::vector<flatbuffers::Offset<::foxglove::PointsAnnotation>> points_offsets;

  for (const auto* item : read_repeated_msgs(points_src)) {
    double px = get_sub_double(*item, "x");
    double py = get_sub_double(*item, "y");

    auto pt = ::foxglove::CreatePoint2(builder, px, py);
    std::vector<flatbuffers::Offset<::foxglove::Point2>> pts_vec = {pt};
    auto pts = builder.CreateVector(pts_vec);
    auto outline = ::foxglove::CreateColor(builder, 0.0, 1.0, 0.0, 1.0);
    points_offsets.emplace_back(
        ::foxglove::CreatePointsAnnotation(builder, &ts, ::foxglove::PointsAnnotationType::POINTS, pts, outline));
  }

  std::vector<flatbuffers::Offset<::foxglove::TextAnnotation>> text_offsets;

  for (const auto* item : read_repeated_msgs(texts_src)) {
    double tx = get_sub_double(*item, "x");
    double ty = get_sub_double(*item, "y");
    auto text_str = get_sub_string(*item, "text");
    double font_size = get_sub_double(*item, "font_size");

    if (font_size <= 0.0) {
      font_size = 12.0;
    }

    auto pos = ::foxglove::CreatePoint2(builder, tx, ty);
    auto text_off = builder.CreateString(text_str);
    auto text_color = ::foxglove::CreateColor(builder, 1.0, 1.0, 1.0, 1.0);
    text_offsets.emplace_back(::foxglove::CreateTextAnnotation(builder, &ts, pos, text_off, font_size, text_color));
  }

  auto circles_vec = builder.CreateVector(circle_offsets);
  auto points_vec = builder.CreateVector(points_offsets);
  auto texts_vec = builder.CreateVector(text_offsets);
  auto ann = ::foxglove::CreateImageAnnotations(builder, circles_vec, points_vec, texts_vec, 0, &ts);
  builder.Finish(ann);

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.schema_name = "foxglove.ImageAnnotations";
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

FoxgloveMessage FoxgloveConverter::convert_joint_states(const FoxgloveMapping& mapping,
                                                        const google::protobuf::Message& msg) {
  FoxgloveMessage result;
  thread_local flatbuffers::FlatBufferBuilder builder(16 * 1024);
  builder.Clear();

  uint64_t timestamp_us = 0;
  uint64_t timestamp_ns = 0;
  std::string joints_src;
  std::string joint_name_src;
  std::string joint_position_src;
  std::string joint_velocity_src;
  std::string joint_effort_src;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "timestamp") {
      timestamp_us = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "timestamp_ns") {
      timestamp_ns = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "joints") {
      joints_src = fm.source;
    } else if (fm.target == "joint_name") {
      joint_name_src = fm.source;
    } else if (fm.target == "joint_position") {
      joint_position_src = fm.source;
    } else if (fm.target == "joint_velocity") {
      joint_velocity_src = fm.source;
    } else if (fm.target == "joint_effort") {
      joint_effort_src = fm.source;
    }
  }

  std::vector<flatbuffers::Offset<::foxglove::JointState>> joint_offsets;

  if (!joints_src.empty()) {
    const auto* desc = msg.GetDescriptor();
    const auto* ref = msg.GetReflection();
    const auto* field = find_proto_field_cached(*desc, joints_src);

    if (field && field->is_repeated()) {
      int count = ref->FieldSize(msg, field);

      for (int i = 0; i < count; ++i) {
        const auto& item = ref->GetRepeatedMessage(msg, field, i);

        auto direct_get = [&item](const std::string& name) -> double {
          if (name.empty()) {
            return 0.0;
          }

          if (has_nested_field_path(name)) {
            return safe_nested_double(item, name);
          }

          const auto* f = find_proto_field_cached(*item.GetDescriptor(), name);

          if VUNLIKELY (!f || !is_proto_numeric_type(f->cpp_type())) {
            return 0.0;
          }

          return get_proto_numeric_value(item, f);
        };

        auto direct_get_str = [&item](const std::string& name) -> std::string {
          if (name.empty()) {
            return {};
          }

          if (has_nested_field_path(name)) {
            bool found = false;
            auto value = resolve_nested_string(item, name, &found);
            return found ? value : std::string{};
          }

          const auto* f = find_proto_field_cached(*item.GetDescriptor(), name);

          if VUNLIKELY (!f || f->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_STRING) {
            return {};
          }

          return item.GetReflection()->GetString(item, f);
        };

        auto name_field = joint_name_src.empty() ? "name" : joint_name_src;
        auto pos_field = joint_position_src.empty() ? "position" : joint_position_src;
        auto vel_field = joint_velocity_src.empty() ? "velocity" : joint_velocity_src;
        auto eff_field = joint_effort_src.empty() ? "effort" : joint_effort_src;

        auto jname = direct_get_str(name_field);
        double jpos = direct_get(pos_field);
        double jvel = direct_get(vel_field);
        double jeff = direct_get(eff_field);

        auto name_off = builder.CreateString(jname);
        joint_offsets.emplace_back(
            ::foxglove::CreateJointState(builder, name_off, jpos, jvel, ::flatbuffers::nullopt, jeff));
      }
    }
  }

  auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
  auto joints_vec = builder.CreateVector(joint_offsets);
  auto js = ::foxglove::CreateJointStates(builder, &ts, joints_vec);
  builder.Finish(js);

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.schema_name = "foxglove.JointStates";
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

FoxgloveMessage FoxgloveConverter::convert_point3_in_frame(const FoxgloveMapping& mapping,
                                                           const google::protobuf::Message& msg) {
  FoxgloveMessage result;
  thread_local flatbuffers::FlatBufferBuilder builder(4096);
  builder.Clear();

  uint64_t timestamp_us = 0;
  uint64_t timestamp_ns = 0;
  std::string frame_id;
  double position_x = 0.0;
  double position_y = 0.0;
  double position_z = 0.0;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "timestamp") {
      timestamp_us = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "timestamp_ns") {
      timestamp_ns = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "frame_id") {
      frame_id = get_proto_string(msg, fm.source, fm);
    } else if (fm.target == "position_x") {
      position_x = get_proto_double(msg, fm.source, fm);
    } else if (fm.target == "position_y") {
      position_y = get_proto_double(msg, fm.source, fm);
    } else if (fm.target == "position_z") {
      position_z = get_proto_double(msg, fm.source, fm);
    }
  }

  auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
  auto fid = builder.CreateString(frame_id);
  auto point = ::foxglove::CreatePoint3(builder, position_x, position_y, position_z);
  auto p3f = ::foxglove::CreatePoint3InFrame(builder, &ts, fid, point);
  builder.Finish(p3f);

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.schema_name = "foxglove.Point3InFrame";
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

FoxgloveMessage FoxgloveConverter::convert_raw_audio(const FoxgloveMapping& mapping,
                                                     const google::protobuf::Message& msg) {
  FoxgloveMessage result;
  thread_local flatbuffers::FlatBufferBuilder builder(64 * 1024);
  builder.Clear();

  uint64_t timestamp_us = 0;
  uint64_t timestamp_ns = 0;
  uint32_t sample_rate = 0;
  uint32_t number_of_channels = 0;
  std::string format;
  std::string data_src;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "timestamp") {
      timestamp_us = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "timestamp_ns") {
      timestamp_ns = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "sample_rate") {
      sample_rate = checked_unsigned_cast<uint32_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "number_of_channels") {
      number_of_channels = checked_unsigned_cast<uint32_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "format") {
      format = get_proto_string(msg, fm.source, fm);
    } else if (fm.target == "data") {
      data_src = fm.source;
    }
  }

  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> data_vec = 0;

  if (!data_src.empty()) {
    const auto* desc = msg.GetDescriptor();
    const auto* ref = msg.GetReflection();
    const auto* field = find_proto_field_cached(*desc, data_src);

    if (field) {
      if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING) {
        std::string scratch;
        const auto& raw_bytes = ref->GetStringReference(msg, field, &scratch);

        if VLIKELY (!raw_bytes.empty()) {
          data_vec = builder.CreateVector(reinterpret_cast<const uint8_t*>(raw_bytes.data()), raw_bytes.size());
        }
      } else if (field->is_repeated()) {
        data_vec = create_proto_repeated_byte_vector(builder, msg, *field, *ref);
      }
    }
  }

  auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
  auto fmt = builder.CreateString(format);
  auto ra = ::foxglove::CreateRawAudio(builder, &ts, data_vec, fmt, sample_rate, number_of_channels);
  builder.Finish(ra);

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.schema_name = "foxglove.RawAudio";
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

FoxgloveMessage FoxgloveConverter::convert_voxel_grid(const FoxgloveMapping& mapping,
                                                      const google::protobuf::Message& msg) {
  FoxgloveMessage result;
  thread_local flatbuffers::FlatBufferBuilder builder(64 * 1024);
  builder.Clear();

  uint64_t timestamp_us = 0;
  uint64_t timestamp_ns = 0;
  std::string frame_id;
  double voxel_size_x = 1.0;
  double voxel_size_y = 1.0;
  double voxel_size_z = 1.0;
  uint32_t row_count = 0;
  uint32_t column_count = 0;
  uint32_t slice_stride = 0;
  uint32_t row_stride = 0;
  uint32_t cell_stride = 0;
  std::string fields_src;
  std::string data_src;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "timestamp") {
      timestamp_us = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "timestamp_ns") {
      timestamp_ns = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "frame_id") {
      frame_id = get_proto_string(msg, fm.source, fm);
    } else if (fm.target == "voxel_size_x") {
      voxel_size_x = get_proto_double(msg, fm.source, fm);
    } else if (fm.target == "voxel_size_y") {
      voxel_size_y = get_proto_double(msg, fm.source, fm);
    } else if (fm.target == "voxel_size_z") {
      voxel_size_z = get_proto_double(msg, fm.source, fm);
    } else if (fm.target == "row_count") {
      row_count = checked_unsigned_cast<uint32_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "column_count") {
      column_count = checked_unsigned_cast<uint32_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "slice_stride") {
      slice_stride = checked_unsigned_cast<uint32_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "row_stride") {
      row_stride = checked_unsigned_cast<uint32_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "cell_stride") {
      cell_stride = checked_unsigned_cast<uint32_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "fields") {
      fields_src = fm.source;
    } else if (fm.target == "data") {
      data_src = fm.source;
    }
  }

  std::vector<flatbuffers::Offset<::foxglove::PackedElementField>> field_offsets;

  if (!fields_src.empty()) {
    const auto* desc = msg.GetDescriptor();
    const auto* ref = msg.GetReflection();
    const auto* field = find_proto_field_cached(*desc, fields_src);

    if (field && field->is_repeated()) {
      int count = ref->FieldSize(msg, field);

      for (int i = 0; i < count; ++i) {
        const auto& item = ref->GetRepeatedMessage(msg, field, i);
        const auto* item_desc = item.GetDescriptor();
        const auto* item_ref = item.GetReflection();

        std::string fname;
        uint32_t foffset = 0;
        uint8_t ftype_val = 0;

        const auto* name_f = find_proto_field_cached(*item_desc, "name");

        if (name_f && name_f->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING) {
          fname = item_ref->GetString(item, name_f);
        }

        const auto* offset_f = find_proto_field_cached(*item_desc, "offset");

        if (offset_f && is_proto_numeric_type(offset_f->cpp_type())) {
          foffset = checked_unsigned_cast<uint32_t>(get_proto_numeric_value(item, offset_f));
        }

        const auto* type_f = find_proto_field_cached(*item_desc, "type");

        if (type_f) {
          if (type_f->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_ENUM) {
            ftype_val = checked_unsigned_cast<uint8_t>(static_cast<double>(item_ref->GetEnumValue(item, type_f)));
          } else if (is_proto_numeric_type(type_f->cpp_type())) {
            ftype_val = checked_unsigned_cast<uint8_t>(get_proto_numeric_value(item, type_f));
          }
        }

        auto fname_off = builder.CreateString(fname);
        field_offsets.emplace_back(::foxglove::CreatePackedElementField(
            builder, fname_off, foffset, static_cast<::foxglove::NumericType>(ftype_val)));
      }
    }
  }

  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> data_vec = 0;

  if (!data_src.empty()) {
    const auto* desc = msg.GetDescriptor();
    const auto* ref = msg.GetReflection();
    const auto* field = find_proto_field_cached(*desc, data_src);

    if (field) {
      if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING) {
        std::string scratch;
        const auto& raw_bytes = ref->GetStringReference(msg, field, &scratch);

        if VLIKELY (!raw_bytes.empty()) {
          data_vec = builder.CreateVector(reinterpret_cast<const uint8_t*>(raw_bytes.data()), raw_bytes.size());
        }
      } else if (field->is_repeated()) {
        data_vec = create_proto_repeated_byte_vector(builder, msg, *field, *ref);
      }
    }
  }

  auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
  auto fid = builder.CreateString(frame_id);
  auto pose = ::foxglove::CreatePose(builder, ::foxglove::CreateVector3(builder, 0.0, 0.0, 0.0),
                                     ::foxglove::CreateQuaternion(builder, 0.0, 0.0, 0.0, 1.0));
  auto cell_sz = ::foxglove::CreateVector3(builder, voxel_size_x, voxel_size_y, voxel_size_z);
  auto fields_vec = builder.CreateVector(field_offsets);
  auto vg = ::foxglove::CreateVoxelGrid(builder, &ts, fid, pose, row_count, column_count, cell_sz, slice_stride,
                                        row_stride, cell_stride, fields_vec, data_vec);
  builder.Finish(vg);

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.schema_name = "foxglove.VoxelGrid";
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

FoxgloveMessage FoxgloveConverter::convert(std::string_view url, SchemaType schema_type, const std::string& ser,
                                           const Bytes& raw) {
  FoxgloveMessage result;

  if (schema_type == SchemaType::kZeroCopy) {
    if (Helpers::has_startwith(ser, "vlink::zerocopy::CameraFrame")) {
      return camera_frame_fbs(raw);
    }

    if (Helpers::has_startwith(ser, "vlink::zerocopy::PointCloud")) {
      return point_cloud_fbs(raw);
    }

    if (Helpers::has_startwith(ser, "vlink::zerocopy::RawData")) {
      return raw_data_to_log(raw);
    }

    if (Helpers::has_startwith(ser, "vlink::zerocopy::OccupancyGrid")) {
      return occupancy_grid_fbs(raw);
    }

    if (Helpers::has_startwith(ser, "vlink::zerocopy::Tensor")) {
      return tensor_fbs(raw);
    }

    if (Helpers::has_startwith(ser, "vlink::zerocopy::ObjectArray")) {
      return object_array_fbs(raw);
    }

    if (Helpers::has_startwith(ser, "vlink::zerocopy::AudioFrame")) {
      return audio_frame_fbs(raw);
    }
  }

  bool ambiguous = false;
  const auto* mapping = find_mapping(url, ser, &ambiguous);

  if VUNLIKELY (ambiguous) {
    return result;
  }

  if VLIKELY (mapping) {
    if VUNLIKELY (mapping->converter == "passthrough") {
      result = FoxgloveMessage();
      result.payload = Bytes::shallow_copy(raw.data(), raw.size());
      result.success = true;
      result.schema_name = mapping->schema.empty() ? ser : mapping->schema;
      result.encoding = mapping->encoding;
      result.schema_encoding = mapping->schema_encoding.empty() ? mapping->encoding : mapping->schema_encoding;

      if (!mapping->timestamp_field.empty()) {
        if (schema_type == SchemaType::kProtobuf && mapping->encoding == "protobuf") {
          auto msg = deserialize_proto_message(ser, raw);

          if VLIKELY (msg) {
            result.timestamp_ns = extract_proto_timestamp_ns(*msg, mapping->timestamp_field, mapping->timestamp_unit);
          }
        }
#ifdef VLINK_HAS_FBS_COMPILER
        else if (schema_type == SchemaType::kFlatbuffers && is_flatbuffers_schema_encoding(mapping->encoding)) {
          const reflection::Schema* schema = nullptr;

          if (resolve_thread_local_fbs_schema(
                  ser, this,
                  [this](const std::string& type_name, std::string& schema_data) {
                    return resolve_custom_fbs_schema(type_name, schema_data);
                  },
                  schema) &&
              schema != nullptr && schema->root_table() != nullptr &&
              verify_fbs_payload(*schema, raw, ser, "Foxglove passthrough timestamp")) {
            const auto* root_table = flatbuffers::GetAnyRoot(raw.data());

            if (root_table) {
              result.timestamp_ns = extract_fbs_timestamp_ns(*root_table, *schema->root_table(), *schema,
                                                             mapping->timestamp_field, mapping->timestamp_unit);
            }
          }
        }
#endif
      }

      return result;
    }

    if VUNLIKELY (!mapping->converter.empty()) {
      if (mapping->converter == "camera_frame") {
        return camera_frame_fbs(raw);
      }

      if (mapping->converter == "point_cloud") {
        return point_cloud_fbs(raw);
      }

      if (mapping->converter == "send_time") {
        result = FoxgloveMessage();
        result.is_send_time = true;
        result.success = true;
        result.payload = Bytes::shallow_copy(raw.data(), raw.size());

        if (!mapping->timestamp_field.empty()) {
          if (schema_type == SchemaType::kProtobuf && mapping->encoding == "protobuf") {
            auto msg = deserialize_proto_message(ser, raw);

            if VLIKELY (msg) {
              result.timestamp_ns = extract_proto_timestamp_ns(*msg, mapping->timestamp_field, mapping->timestamp_unit);
            }
          }
#ifdef VLINK_HAS_FBS_COMPILER
          else if (schema_type == SchemaType::kFlatbuffers && is_flatbuffers_schema_encoding(mapping->encoding)) {
            const reflection::Schema* schema = nullptr;

            if (resolve_thread_local_fbs_schema(
                    ser, this,
                    [this](const std::string& type_name, std::string& schema_data) {
                      return resolve_custom_fbs_schema(type_name, schema_data);
                    },
                    schema) &&
                schema != nullptr && schema->root_table() != nullptr &&
                verify_fbs_payload(*schema, raw, ser, "Foxglove send_time timestamp")) {
              const auto* root_table = flatbuffers::GetAnyRoot(raw.data());

              if (root_table) {
                result.timestamp_ns = extract_fbs_timestamp_ns(*root_table, *schema->root_table(), *schema,
                                                               mapping->timestamp_field, mapping->timestamp_unit);
              }
            }
          }
#endif
        }

        return result;
      }
    }

    if (mapping->encoding == "protobuf") {
      if VUNLIKELY (schema_type != SchemaType::kProtobuf) {
        return {};
      }

      auto proto_result = convert_proto_mapping(*mapping, ser, raw);

      if VLIKELY (proto_result.success) {
        return proto_result;
      }

      return {};
    } else if (is_flatbuffers_schema_encoding(mapping->encoding)) {
      if VUNLIKELY (schema_type != SchemaType::kFlatbuffers) {
        return {};
      }

#ifdef VLINK_HAS_FBS_COMPILER
      auto fbs_result = convert_fbs_mapping(*mapping, ser, raw);

      if VLIKELY (fbs_result.success) {
        return fbs_result;
      }
#endif

      return {};
    }

    return {};
  }

  if VUNLIKELY (convert_plugin_ && convert_plugin_->can_convert(ser, ConvertPluginInterface::Target::kFoxglove)) {
    ConvertPluginInterface::SchemaInfo schema_info;

    bool has_schema = convert_plugin_->get_schema(ser, ConvertPluginInterface::Target::kFoxglove, schema_info);

    if (has_schema && schema_info.type_name == "SendTime") {
      FoxgloveMessage plugin_result;
      plugin_result.is_send_time = true;
      plugin_result.success = true;
      plugin_result.timestamp_ns = convert_plugin_->get_timestamp(ser, raw, ConvertPluginInterface::Target::kFoxglove);
      return plugin_result;
    }

    Bytes payload;

    if VUNLIKELY (!convert_plugin_->convert(ser, raw, ConvertPluginInterface::Target::kFoxglove, payload)) {
      MLOG_W("Convert plugin convert() failed for: {}", ser);
      return {};
    }

    FoxgloveMessage plugin_result;
    plugin_result.success = true;
    plugin_result.payload = std::move(payload);
    plugin_result.timestamp_ns = convert_plugin_->get_timestamp(ser, raw, ConvertPluginInterface::Target::kFoxglove);

    if VUNLIKELY (!has_schema) {
      MLOG_W("Convert plugin get_schema() failed for: {}", ser);
    } else {
      plugin_result.schema_name = std::move(schema_info.type_name);
      plugin_result.encoding = std::move(schema_info.encoding);
      plugin_result.schema_encoding = std::move(schema_info.schema_encoding);
      plugin_result.schema_data = std::move(schema_info.schema_data);
    }

    return plugin_result;
  }

  if (schema_type == SchemaType::kRaw) {
    if VUNLIKELY (is_text_ser(ser)) {
      return string_to_log(raw);
    }
  } else if (schema_type == SchemaType::kProtobuf) {
    if VLIKELY (find_proto_descriptor(ser)) {
      result = FoxgloveMessage();
      result.payload = Bytes::shallow_copy(raw.data(), raw.size());
      result.success = true;
      result.schema_name = ser;
      result.encoding = "protobuf";
      result.schema_encoding = "protobuf";
      return result;
    }
  }
#ifdef VLINK_HAS_FBS_COMPILER
  else if (schema_type == SchemaType::kFlatbuffers) {
    std::lock_guard lock(mtx_);

    if VLIKELY (fbs_parsers_.find(ser) != fbs_parsers_.end()) {
      result = FoxgloveMessage();
      result.payload = Bytes::shallow_copy(raw.data(), raw.size());
      result.success = true;
      result.schema_name = ser;
      result.encoding = std::string(kFoxgloveFlatbufferEncoding);
      result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
      return result;
    }
  }
#endif

  return result;
}

bool FoxgloveConverter::get_schema_info(std::string_view url, SchemaType schema_type, const std::string& ser,
                                        std::string& schema_name, std::string& encoding, std::string& schema_encoding,
                                        std::string& schema_data, bool* is_send_time) {
  if (is_send_time) {
    *is_send_time = false;
  }

  if (schema_type == SchemaType::kZeroCopy) {
    if (Helpers::has_startwith(ser, "vlink::zerocopy::CameraFrame")) {
      schema_name = "foxglove.RawImage";
      encoding = std::string(kFoxgloveFlatbufferEncoding);
      schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
      return resolve_fbs_schema(schema_name, schema_data);
    }

    if (Helpers::has_startwith(ser, "vlink::zerocopy::PointCloud")) {
      schema_name = "foxglove.PointCloud";
      encoding = std::string(kFoxgloveFlatbufferEncoding);
      schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
      return resolve_fbs_schema(schema_name, schema_data);
    }

    if (Helpers::has_startwith(ser, "vlink::zerocopy::RawData")) {
      schema_name = "foxglove.Log";
      encoding = std::string(kFoxgloveFlatbufferEncoding);
      schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
      return resolve_fbs_schema(schema_name, schema_data);
    }

    if (Helpers::has_startwith(ser, "vlink::zerocopy::OccupancyGrid")) {
      schema_name = "foxglove.Grid";
      encoding = std::string(kFoxgloveFlatbufferEncoding);
      schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
      return resolve_fbs_schema(schema_name, schema_data);
    }

    if (Helpers::has_startwith(ser, "vlink::zerocopy::Tensor")) {
      schema_name = "foxglove.Log";
      encoding = std::string(kFoxgloveFlatbufferEncoding);
      schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
      return resolve_fbs_schema(schema_name, schema_data);
    }

    if (Helpers::has_startwith(ser, "vlink::zerocopy::ObjectArray")) {
      schema_name = "foxglove.SceneUpdate";
      encoding = std::string(kFoxgloveFlatbufferEncoding);
      schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
      return resolve_fbs_schema(schema_name, schema_data);
    }

    if (Helpers::has_startwith(ser, "vlink::zerocopy::AudioFrame")) {
      schema_name = "foxglove.RawAudio";
      encoding = std::string(kFoxgloveFlatbufferEncoding);
      schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
      return resolve_fbs_schema(schema_name, schema_data);
    }
  }

  bool ambiguous = false;
  const auto* mapping = find_mapping(url, ser, &ambiguous);

  if VUNLIKELY (ambiguous) {
    return false;
  }

  if (mapping && mapping->converter == "send_time") {
    if (is_send_time) {
      *is_send_time = true;
    }

    if (!mapping->schema.empty()) {
      schema_name = mapping->schema;
      schema_encoding = mapping->schema_encoding.empty() ? mapping->encoding : mapping->schema_encoding;
      encoding = schema_encoding;
      return resolve_schema_by_name(schema_name, schema_encoding, schema_data);
    }

    if (schema_type == SchemaType::kProtobuf && mapping->encoding == "protobuf") {
      if (resolve_proto_schema(ser, schema_data)) {
        schema_name = ser;
        encoding = "protobuf";
        schema_encoding = "protobuf";
        return true;
      }
    }

#ifdef VLINK_HAS_FBS_COMPILER

    if (schema_type == SchemaType::kFlatbuffers && is_flatbuffers_schema_encoding(mapping->encoding)) {
      if (resolve_custom_fbs_schema(ser, schema_data)) {
        schema_name = ser;
        encoding = std::string(kFoxgloveFlatbufferEncoding);
        schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
        return true;
      }
    }
#endif

    schema_name = "vlink.SendTime";
    encoding = "send_time";
    schema_encoding.clear();
    schema_data.clear();
    return true;
  }

  if VLIKELY (mapping) {
    schema_name = mapping->schema.empty() ? ser : mapping->schema;
    schema_encoding =
        mapping->schema_encoding.empty() ? std::string(kFoxgloveFlatbufferEncoding) : mapping->schema_encoding;
    encoding = (mapping->converter == "passthrough") ? mapping->encoding : schema_encoding;
    if (is_flatbuffers_schema_encoding(encoding)) {
      encoding = std::string(kFoxgloveFlatbufferEncoding);
    }
    if (is_flatbuffers_schema_encoding(schema_encoding)) {
      schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
    }
    return resolve_schema_by_name(schema_name, schema_encoding, schema_data);
  }

  if (convert_plugin_ && convert_plugin_->can_convert(ser, ConvertPluginInterface::Target::kFoxglove)) {
    ConvertPluginInterface::SchemaInfo schema_info;

    if (convert_plugin_->get_schema(ser, ConvertPluginInterface::Target::kFoxglove, schema_info)) {
      schema_name = std::move(schema_info.type_name);
      encoding = std::move(schema_info.encoding);
      schema_encoding = std::move(schema_info.schema_encoding);
      schema_data = std::move(schema_info.schema_data);

      if (schema_name == "SendTime") {
        encoding = "send_time";

        if (is_send_time) {
          *is_send_time = true;
        }
      }

      return true;
    }

    MLOG_W("Convert plugin matched '{}' but did not return schema info", ser);
    return false;
  }

  if (schema_type == SchemaType::kRaw) {
    if VUNLIKELY (is_text_ser(ser)) {
      schema_name = "foxglove.Log";
      encoding = std::string(kFoxgloveFlatbufferEncoding);
      schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
      return resolve_fbs_schema(schema_name, schema_data);
    }
  } else if (schema_type == SchemaType::kProtobuf) {
    if (resolve_proto_schema(ser, schema_data)) {
      schema_name = ser;
      encoding = "protobuf";
      schema_encoding = "protobuf";
      return true;
    }
  }
#ifdef VLINK_HAS_FBS_COMPILER
  else if (schema_type == SchemaType::kFlatbuffers) {
    if (resolve_custom_fbs_schema(ser, schema_data)) {
      schema_name = ser;
      encoding = std::string(kFoxgloveFlatbufferEncoding);
      schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
      return true;
    }
  }
#endif

  return false;
}

bool FoxgloveConverter::resolve_schema_by_name(const std::string& schema_name, const std::string& schema_enc,
                                               std::string& schema_data) {
  if (is_flatbuffers_schema_encoding(schema_enc)) {
    if (resolve_fbs_schema(schema_name, schema_data)) {
      return true;
    }

#ifdef VLINK_HAS_FBS_COMPILER
    return resolve_custom_fbs_schema(schema_name, schema_data);
#else
    return false;
#endif
  }

  if (schema_enc == "protobuf") {
    return resolve_proto_schema(schema_name, schema_data);
  }

  return false;
}

bool FoxgloveConverter::has_send_time_mapping() const {
  return std::any_of(mappings_.begin(), mappings_.end(),
                     [](const FoxgloveMapping& mapping) { return mapping.converter == "send_time"; });
}

FoxgloveMessage FoxgloveConverter::raw_data_to_log(const Bytes& raw) {
  FoxgloveMessage result;

  zerocopy::RawData rd;

  if VUNLIKELY (!(rd << raw)) {
    MLOG_W("Failed to deserialize RawData");
    return result;
  }

  auto data_size = rd.size();
  std::string message = "RawData (" + std::to_string(data_size) + " bytes)";

  thread_local flatbuffers::FlatBufferBuilder builder(4096);
  builder.Clear();

  auto ts = make_timestamp_from_ns(rd.header.time_meas);
  auto msg_str = builder.CreateString(message);
  auto name_str = builder.CreateString("RawData");
  auto file_str = builder.CreateString("");

  auto log = ::foxglove::CreateLog(builder, &ts, ::foxglove::LogLevel::INFO, msg_str, name_str, file_str, 0);
  builder.Finish(log);

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.schema_name = "foxglove.Log";
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

FoxgloveMessage FoxgloveConverter::string_to_log(const Bytes& raw) {
  FoxgloveMessage result;

  if VUNLIKELY (raw.empty()) {
    return result;
  }

  std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());

  thread_local flatbuffers::FlatBufferBuilder builder(4096);
  builder.Clear();

  ::foxglove::Time ts{0, 0};
  auto msg_str = builder.CreateString(text);
  auto name_str = builder.CreateString("");
  auto file_str = builder.CreateString("");

  auto log = ::foxglove::CreateLog(builder, &ts, ::foxglove::LogLevel::INFO, msg_str, name_str, file_str, 0);
  builder.Finish(log);

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.schema_name = "foxglove.Log";
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

static bool mul_size(size_t a, size_t b, size_t& out) {
  if VUNLIKELY (a != 0 && b > std::numeric_limits<size_t>::max() / a) {
    return false;
  }

  out = a * b;
  return true;
}

static bool set_raw_info(std::string_view encoding, size_t row_bytes, size_t data_size, std::string& out_encoding,
                         uint32_t& step, size_t& expected) {
  if VUNLIKELY (row_bytes > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
    return false;
  }

  out_encoding = std::string(encoding);
  step = static_cast<uint32_t>(row_bytes);
  expected = data_size;

  return true;
}

static bool camera_frame_raw_info(const zerocopy::CameraFrame& frame, std::string& encoding, uint32_t& step,
                                  size_t& expected, bool& rgb_planar) {
  const uint32_t width = frame.width();
  const uint32_t height = frame.height();

  if VUNLIKELY (width == 0 || height == 0) {
    return false;
  }

  size_t pixels = 0;

  if VUNLIKELY (!mul_size(static_cast<size_t>(width), static_cast<size_t>(height), pixels)) {
    return false;
  }

  auto set_bpp = [&](std::string_view name, size_t bytes_per_pixel) {
    size_t row_bytes = 0;
    size_t data_size = 0;
    return mul_size(static_cast<size_t>(width), bytes_per_pixel, row_bytes) &&
           mul_size(pixels, bytes_per_pixel, data_size) &&
           set_raw_info(name, row_bytes, data_size, encoding, step, expected);
  };

  auto set_yuv420 = [&](std::string_view name) {
    if VUNLIKELY ((width % 2U) != 0 || (height % 2U) != 0 ||
                  pixels > std::numeric_limits<size_t>::max() - pixels / 2U) {
      return false;
    }

    return set_raw_info(name, width, pixels + pixels / 2U, encoding, step, expected);
  };

  rgb_planar = false;

  switch (frame.format()) {
    case zerocopy::CameraFrame::kFormatNv12:
      return set_yuv420("nv12");
    case zerocopy::CameraFrame::kFormatYuyv:
      return (width % 2U) == 0 && set_bpp("yuv422_yuy2", 2U);
    case zerocopy::CameraFrame::kFormatUyvy:
      return (width % 2U) == 0 && set_bpp("yuv422", 2U);
    case zerocopy::CameraFrame::kFormatBgr888Packed:
      return set_bpp("bgr8", 3U);
    case zerocopy::CameraFrame::kFormatRgb888Packed:
      return set_bpp("rgb8", 3U);
    case zerocopy::CameraFrame::kFormatRgb888Planar:
      rgb_planar = true;
      return set_bpp("rgb8", 3U);
    case zerocopy::CameraFrame::kFormatMono8:
      return set_bpp("mono8", 1U);
    case zerocopy::CameraFrame::kFormatMono16:
      return set_bpp("mono16", 2U);
    case zerocopy::CameraFrame::kFormatRgba8888Packed:
      return set_bpp("rgba8", 4U);
    case zerocopy::CameraFrame::kFormatBgra8888Packed:
      return set_bpp("bgra8", 4U);
    case zerocopy::CameraFrame::kFormatUint8C1:
      return set_bpp("8UC1", 1U);
    case zerocopy::CameraFrame::kFormatUint8C3:
      return set_bpp("8UC3", 3U);
    case zerocopy::CameraFrame::kFormatUint16C1:
      return set_bpp("16UC1", 2U);
    case zerocopy::CameraFrame::kFormatFloat32C1:
      return set_bpp("32FC1", 4U);
    case zerocopy::CameraFrame::kFormatBayerRggb8:
    case zerocopy::CameraFrame::kFormatBayerBggr8:
    case zerocopy::CameraFrame::kFormatBayerGbrg8:
    case zerocopy::CameraFrame::kFormatBayerGrbg8:
      return set_bpp(zerocopy::CameraFrame::encoding_from_format(frame.format()), 1U);
    default:
      return false;
  }
}

FoxgloveMessage FoxgloveConverter::camera_frame_fbs(const Bytes& raw) {
  FoxgloveMessage result;

  zerocopy::CameraFrame frame;

  if VUNLIKELY (!(frame << raw)) {
    MLOG_W("Failed to deserialize CameraFrame");
    return result;
  }

  auto fmt = frame.format();

  std::string fmt_str(zerocopy::CameraFrame::encoding_from_format(fmt));
  if VUNLIKELY (fmt == zerocopy::CameraFrame::kFormatUnknown || fmt_str == "unknown") {
    MLOG_W("CameraFrame format is unknown, skipping");
    return result;
  }

  thread_local flatbuffers::FlatBufferBuilder builder(256 * 1024);
  builder.Clear();

  auto ts = make_timestamp_from_ns(frame.header.time_meas);
  auto frame_id =
      builder.CreateString(frame.header.frame_id, ::strnlen(frame.header.frame_id, sizeof(frame.header.frame_id)));

  if (fmt == zerocopy::CameraFrame::kFormatH266) {
    MLOG_W("Foxglove CompressedVideo does not support H.266");
    return result;
  }

  if (fmt == zerocopy::CameraFrame::kFormatH264 || fmt == zerocopy::CameraFrame::kFormatH265 ||
      fmt == zerocopy::CameraFrame::kFormatAv1) {
    auto data_vec = builder.CreateVector(frame.data(), frame.size());
    auto format = builder.CreateString(fmt_str);
    auto msg = ::foxglove::CreateCompressedVideo(builder, &ts, frame_id, data_vec, format);
    builder.Finish(msg);
    result.schema_name = "foxglove.CompressedVideo";
  } else if (fmt == zerocopy::CameraFrame::kFormatJpeg || fmt == zerocopy::CameraFrame::kFormatPng ||
             fmt == zerocopy::CameraFrame::kFormatMjpeg || fmt == zerocopy::CameraFrame::kFormatWebp) {
    if (fmt == zerocopy::CameraFrame::kFormatMjpeg) {
      fmt_str = "jpeg";
    }

    auto data_vec = builder.CreateVector(frame.data(), frame.size());
    auto format = builder.CreateString(fmt_str);
    auto msg = ::foxglove::CreateCompressedImage(builder, &ts, frame_id, data_vec, format);
    builder.Finish(msg);
    result.schema_name = "foxglove.CompressedImage";
  } else {
    uint32_t step = 0;
    size_t expected = 0;
    bool rgb_planar = false;

    if VUNLIKELY (!camera_frame_raw_info(frame, fmt_str, step, expected, rgb_planar) || frame.size() < expected) {
      MLOG_W("CameraFrame raw format is invalid or unsupported, format={} width={} height={} size={}", fmt_str,
             frame.width(), frame.height(), frame.size());
      return result;
    }

    std::vector<uint8_t> rgb_data;
    flatbuffers::Offset<flatbuffers::Vector<uint8_t>> data_vec;

    if (rgb_planar) {
      size_t pixels = 0;

      if VUNLIKELY (!mul_size(static_cast<size_t>(frame.width()), static_cast<size_t>(frame.height()), pixels)) {
        return result;
      }

      rgb_data.resize(expected);

      const auto* r = frame.data();
      const auto* g = r + pixels;
      const auto* b = g + pixels;

      for (size_t i = 0; i < pixels; ++i) {
        rgb_data[i * 3U + 0U] = r[i];
        rgb_data[i * 3U + 1U] = g[i];
        rgb_data[i * 3U + 2U] = b[i];
      }

      data_vec = builder.CreateVector(rgb_data);
    } else {
      data_vec = builder.CreateVector(frame.data(), expected);
    }

    auto encoding = builder.CreateString(fmt_str);
    auto msg =
        ::foxglove::CreateRawImage(builder, &ts, frame_id, frame.width(), frame.height(), encoding, step, data_vec);
    builder.Finish(msg);
    result.schema_name = "foxglove.RawImage";
  }

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

FoxgloveMessage FoxgloveConverter::point_cloud_fbs(const Bytes& raw) {
  FoxgloveMessage result;

  zerocopy::PointCloud pc;

  if VUNLIKELY (!(pc << raw)) {
    MLOG_W("Failed to deserialize PointCloud (raw={})", raw.size());
    return result;
  }

  if VUNLIKELY (pc.size() == 0 || pc.pack_size() == 0) {
    MLOG_W("PointCloud is empty: size={} pack_size={}", pc.size(), pc.pack_size());
    return result;
  }

  auto data_size = pc.size() * pc.pack_size();
  bool has_compressed_xyz = (pc.get_extent() != 0);

  thread_local flatbuffers::FlatBufferBuilder builder(1024 * 1024);
  builder.Clear();

  auto frame_id = builder.CreateString(pc.header.frame_id, ::strnlen(pc.header.frame_id, sizeof(pc.header.frame_id)));

  zerocopy::PointCloud::KeyList key_list;
  auto key_map = pc.get_key_map(&key_list);

  if VUNLIKELY (key_map.empty()) {
    MLOG_W("PointCloud key map is empty");
    return result;
  }

  if VUNLIKELY (has_compressed_xyz && key_list.size() < 3) {
    MLOG_W("Compressed PointCloud has fewer than 3 fields");
    return result;
  }

  std::vector<flatbuffers::Offset<::foxglove::PackedElementField>> field_offsets;
  struct PointCloudFieldCopy {
    uint16_t src_offset{0};
    uint32_t dst_offset{0};
    uint8_t src_size{0};
  };
  std::vector<PointCloudFieldCopy> field_copies;
  if (has_compressed_xyz) {
    field_copies.reserve(key_list.size());
  }
  uint32_t field_offset = 0;
  uint16_t src_field_offset = 0;

  for (size_t key_index = 0; key_index < key_list.size(); ++key_index) {
    const auto& key = key_list[key_index];
    auto name = builder.CreateString(key.name);

    auto num_type = ::foxglove::NumericType::UNKNOWN;
    uint8_t dst_size = key.size;

    switch (key.type) {
      case zerocopy::PointCloud::kBoolType:
      case zerocopy::PointCloud::kUint8Type:
        num_type = ::foxglove::NumericType::UINT8;
        break;
      case zerocopy::PointCloud::kInt8Type:
        num_type = ::foxglove::NumericType::INT8;
        break;
      case zerocopy::PointCloud::kUint16Type:
        num_type = ::foxglove::NumericType::UINT16;
        break;
      case zerocopy::PointCloud::kInt16Type:
        num_type = ::foxglove::NumericType::INT16;
        break;
      case zerocopy::PointCloud::kUint32Type:
        num_type = ::foxglove::NumericType::UINT32;
        break;
      case zerocopy::PointCloud::kInt32Type:
        num_type = ::foxglove::NumericType::INT32;
        break;
      case zerocopy::PointCloud::kFloatType:
        num_type = ::foxglove::NumericType::FLOAT32;
        break;
      case zerocopy::PointCloud::kDoubleType:
        num_type = ::foxglove::NumericType::FLOAT64;
        break;
      default:
        switch (key.size) {
          case 1:
            num_type = ::foxglove::NumericType::UINT8;
            break;
          case 2:
            num_type = ::foxglove::NumericType::UINT16;
            break;
          case 4:
            num_type = ::foxglove::NumericType::FLOAT32;
            break;
          case 8:
            num_type = ::foxglove::NumericType::FLOAT64;
            break;
          default:
            MLOG_W("PointCloud field '{}': type_num not set, inferred type={} from size={}", key.name,
                   static_cast<uint8_t>(num_type), key.size);
            break;
        }
        break;
    }

    if (has_compressed_xyz && key_index < 3) {
      num_type = ::foxglove::NumericType::FLOAT32;
      dst_size = sizeof(float);
    }

    field_offsets.emplace_back(::foxglove::CreatePackedElementField(builder, name, field_offset, num_type));
    if (has_compressed_xyz) {
      field_copies.push_back({src_field_offset, field_offset, key.size});
      src_field_offset += key.size;
    }
    field_offset += dst_size;
  }

  uint32_t point_stride = has_compressed_xyz ? field_offset : static_cast<uint32_t>(pc.pack_size());
  auto fields_vec = builder.CreateVector(field_offsets);

  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> data_vec;

  if VLIKELY (pc.get_internal_data() && data_size > 0) {
    if (has_compressed_xyz) {
      if VUNLIKELY (point_stride != 0 && pc.size() > std::numeric_limits<size_t>::max() / point_stride) {
        MLOG_W("Compressed PointCloud output size overflow: size={} stride={}", pc.size(), point_stride);
        return result;
      }

      uint8_t* out_data = nullptr;
      data_vec = builder.CreateUninitializedVector<uint8_t>(pc.size() * point_stride, &out_data);

      for (size_t i = 0; i < pc.size(); ++i) {
        const auto* src = pc.get_internal_data() + (i * pc.pack_size());
        auto* dst = out_data + (i * point_stride);

        float x = 0.0F;
        float y = 0.0F;
        float z = 0.0F;
        pc.get_value_v3f(x, y, z, i);

        std::memcpy(dst + field_copies[0].dst_offset, &x, sizeof(float));
        std::memcpy(dst + field_copies[1].dst_offset, &y, sizeof(float));
        std::memcpy(dst + field_copies[2].dst_offset, &z, sizeof(float));

        for (size_t j = 3; j < field_copies.size(); ++j) {
          const auto& field = field_copies[j];
          std::memcpy(dst + field.dst_offset, src + field.src_offset, field.src_size);
        }
      }
    } else {
      data_vec = builder.CreateVector(pc.get_internal_data(), data_size);
    }
  } else {
    data_vec = builder.CreateVector(static_cast<const uint8_t*>(nullptr), 0);
  }

  auto ts = make_timestamp_from_ns(pc.header.time_meas);

  auto msg = ::foxglove::CreatePointCloud(builder, &ts, frame_id, 0, point_stride, fields_vec, data_vec);
  builder.Finish(msg);

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.schema_name = "foxglove.PointCloud";
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

FoxgloveMessage FoxgloveConverter::occupancy_grid_fbs(const Bytes& raw) {
  FoxgloveMessage result;

  zerocopy::OccupancyGrid og;

  if VUNLIKELY (!(og << raw)) {
    MLOG_W("Failed to deserialize OccupancyGrid (raw={})", raw.size());
    return result;
  }

  auto cell_sz = og.cell_size();
  auto width = og.width();
  auto height = og.height();

  if VUNLIKELY (cell_sz == 0 || width == 0 || height == 0) {
    MLOG_W("OccupancyGrid invalid: width={} height={} cell_size={}", width, height, cell_sz);
    return result;
  }

  if VUNLIKELY (static_cast<size_t>(width) > std::numeric_limits<size_t>::max() / height) {
    MLOG_W("OccupancyGrid cell count overflow: width={} height={}", width, height);
    return result;
  }

  const size_t cell_count = static_cast<size_t>(width) * static_cast<size_t>(height);

  if VUNLIKELY (cell_count > std::numeric_limits<size_t>::max() / cell_sz ||
                width > std::numeric_limits<uint32_t>::max() / cell_sz) {
    MLOG_W("OccupancyGrid byte size overflow: width={} height={} cell_size={}", width, height, cell_sz);
    return result;
  }

  const size_t data_size = cell_count * static_cast<size_t>(cell_sz);

  if VUNLIKELY (data_size > og.size()) {
    MLOG_W("OccupancyGrid payload too small: need={} have={}", data_size, og.size());
    return result;
  }

  thread_local flatbuffers::FlatBufferBuilder builder(256 * 1024);
  builder.Clear();

  auto ts = make_timestamp_from_ns(og.header.time_meas);
  auto frame_id = builder.CreateString(og.header.frame_id, ::strnlen(og.header.frame_id, sizeof(og.header.frame_id)));

  auto num_type = ::foxglove::NumericType::UNKNOWN;
  std::string field_name = "value";

  switch (og.cell_type()) {
    case zerocopy::OccupancyGrid::kCellInt8:
      num_type = ::foxglove::NumericType::INT8;
      break;
    case zerocopy::OccupancyGrid::kCellUint8:
      num_type = ::foxglove::NumericType::UINT8;
      break;
    case zerocopy::OccupancyGrid::kCellUint16:
      num_type = ::foxglove::NumericType::UINT16;
      break;
    case zerocopy::OccupancyGrid::kCellFloat32:
      num_type = ::foxglove::NumericType::FLOAT32;
      break;
    default:
      switch (cell_sz) {
        case 1:
          num_type = ::foxglove::NumericType::UINT8;
          break;
        case 2:
          num_type = ::foxglove::NumericType::UINT16;
          break;
        case 4:
          num_type = ::foxglove::NumericType::FLOAT32;
          break;
        default:
          MLOG_W("OccupancyGrid: unknown cell_type, inferred FLOAT32 from cell_size={}", cell_sz);
          num_type = ::foxglove::NumericType::FLOAT32;
          break;
      }
      break;
  }

  auto field_name_offset = builder.CreateString(field_name);
  std::vector<flatbuffers::Offset<::foxglove::PackedElementField>> field_offsets;
  field_offsets.emplace_back(::foxglove::CreatePackedElementField(builder, field_name_offset, 0, num_type));
  auto fields_vec = builder.CreateVector(field_offsets);

  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> data_vec;

  if VLIKELY (og.data() && data_size > 0) {
    data_vec = builder.CreateVector(og.data(), data_size);
  } else {
    data_vec = builder.CreateVector(static_cast<const uint8_t*>(nullptr), 0);
  }

  double half_yaw = static_cast<double>(og.origin_yaw()) * 0.5;
  double qz = std::sin(half_yaw);
  double qw = std::cos(half_yaw);

  auto position = ::foxglove::CreateVector3(builder, static_cast<double>(og.origin_x()),
                                            static_cast<double>(og.origin_y()), static_cast<double>(og.origin_z()));
  auto orientation = ::foxglove::CreateQuaternion(builder, 0.0, 0.0, qz, qw);
  auto pose = ::foxglove::CreatePose(builder, position, orientation);
  auto cell_size_vec =
      ::foxglove::CreateVector2(builder, static_cast<double>(og.resolution()), static_cast<double>(og.resolution()));

  uint32_t cell_stride = cell_sz;
  uint32_t row_stride = width * static_cast<uint32_t>(cell_sz);

  auto grid = ::foxglove::CreateGrid(builder, &ts, frame_id, pose, width, cell_size_vec, row_stride, cell_stride,
                                     fields_vec, data_vec);
  builder.Finish(grid);

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.schema_name = "foxglove.Grid";
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

FoxgloveMessage FoxgloveConverter::tensor_fbs(const Bytes& raw) {
  FoxgloveMessage result;

  zerocopy::Tensor tensor;

  if VUNLIKELY (!(tensor << raw)) {
    MLOG_W("Failed to deserialize Tensor (raw={})", raw.size());
    return result;
  }

  std::string dtype_str(::vlink::NameDetector::get_enum(tensor.dtype()));
  std::string device_str(::vlink::NameDetector::get_enum(tensor.device()));

  Json shape_arr = Json::array();
  for (uint8_t i = 0; i < tensor.rank(); ++i) {
    shape_arr.push_back(tensor.shape_at(i));
  }

  Json strides_arr = Json::array();
  for (uint8_t i = 0; i < tensor.rank(); ++i) {
    strides_arr.push_back(tensor.stride_at(i));
  }

  Json info;
  info["name"] = std::string(tensor.name());
  info["model_id"] = std::string(tensor.model_id());
  info["layout"] = std::string(tensor.layout());
  info["dtype"] = dtype_str;
  info["device"] = device_str;
  info["rank"] = tensor.rank();
  info["shape"] = std::move(shape_arr);
  info["strides"] = std::move(strides_arr);
  info["num_elements"] = tensor.num_elements();
  info["element_size"] = tensor.element_size();
  info["data_bytes"] = tensor.size();
  info["batch_size"] = tensor.batch_size();
  info["quant_scale"] = tensor.quant_scale();
  info["quant_zero_point"] = tensor.quant_zero_point();

  std::string message = info.dump();

  thread_local flatbuffers::FlatBufferBuilder builder(8192);
  builder.Clear();

  auto ts = make_timestamp_from_ns(tensor.header.time_meas);
  auto msg_str = builder.CreateString(message);

  auto tensor_name = tensor.name();
  std::string name_label = tensor_name.empty() ? std::string("Tensor") : std::string(tensor_name);
  auto name_str = builder.CreateString(name_label);
  auto file_str = builder.CreateString("");

  auto log = ::foxglove::CreateLog(builder, &ts, ::foxglove::LogLevel::INFO, msg_str, name_str, file_str, 0);
  builder.Finish(log);

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.schema_name = "foxglove.Log";
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

FoxgloveMessage FoxgloveConverter::object_array_fbs(const Bytes& raw) {
  FoxgloveMessage result;

  zerocopy::ObjectArray arr;

  if VUNLIKELY (!(arr << raw)) {
    MLOG_W("Failed to deserialize ObjectArray (raw={})", raw.size());
    return result;
  }

  auto count = arr.count();

  if VUNLIKELY (count != 0 && (!arr.data() || arr.pack_size() != sizeof(zerocopy::ObjectArray::Object))) {
    MLOG_W("ObjectArray invalid: count={} pack_size={}", count, arr.pack_size());
    return result;
  }

  thread_local flatbuffers::FlatBufferBuilder builder(64 * 1024);
  builder.Clear();

  auto ts = make_timestamp_from_ns(arr.header.time_meas);
  auto entity_fid =
      builder.CreateString(arr.header.frame_id, ::strnlen(arr.header.frame_id, sizeof(arr.header.frame_id)));

  std::string source_id_str(arr.source_id());

  if (source_id_str.empty()) {
    source_id_str = "object_array";
  }

  auto entity_id = builder.CreateString(source_id_str);

  std::vector<flatbuffers::Offset<::foxglove::CubePrimitive>> cubes_vec_data;
  cubes_vec_data.reserve(count);

  static constexpr double kPalette[][4] = {
      {0.5, 0.5, 0.5, 0.8}, {0.2, 0.6, 1.0, 0.8}, {0.2, 0.9, 0.2, 0.8}, {1.0, 0.8, 0.0, 0.8}, {0.8, 0.2, 0.8, 0.8},
  };

  for (uint32_t i = 0; i < count; ++i) {
    const auto* obj = arr.objects(i);

    if VUNLIKELY (!obj) {
      continue;
    }

    double half_yaw = static_cast<double>(obj->yaw) * 0.5;
    double qz = std::sin(half_yaw);
    double qw = std::cos(half_yaw);

    auto position =
        ::foxglove::CreateVector3(builder, static_cast<double>(obj->position[0]), static_cast<double>(obj->position[1]),
                                  static_cast<double>(obj->position[2]));
    auto orientation = ::foxglove::CreateQuaternion(builder, 0.0, 0.0, qz, qw);
    auto pose = ::foxglove::CreatePose(builder, position, orientation);

    auto sx = static_cast<double>(obj->size[0]);
    auto sy = static_cast<double>(obj->size[1]);
    auto sz = static_cast<double>(obj->size[2]);

    if (sx <= 0.0) {
      sx = 1.0;
    }

    if (sy <= 0.0) {
      sy = 1.0;
    }

    if (sz <= 0.0) {
      sz = 1.0;
    }

    auto size = ::foxglove::CreateVector3(builder, sx, sy, sz);

    auto palette_idx = static_cast<size_t>(obj->motion_state);

    if (palette_idx >= (sizeof(kPalette) / sizeof(kPalette[0]))) {
      auto hue = static_cast<double>(obj->class_id % 6U);
      auto color = ::foxglove::CreateColor(builder, std::fmod(hue * 0.17, 1.0), std::fmod(hue * 0.29 + 0.3, 1.0),
                                           std::fmod(hue * 0.41 + 0.6, 1.0), 0.8);
      cubes_vec_data.emplace_back(::foxglove::CreateCubePrimitive(builder, pose, size, color));
      continue;
    }

    auto color = ::foxglove::CreateColor(builder, kPalette[palette_idx][0], kPalette[palette_idx][1],
                                         kPalette[palette_idx][2], kPalette[palette_idx][3]);
    cubes_vec_data.emplace_back(::foxglove::CreateCubePrimitive(builder, pose, size, color));
  }

  auto cubes_vec = builder.CreateVector(cubes_vec_data);

  auto entity = ::foxglove::CreateSceneEntity(builder, &ts, entity_fid, entity_id, nullptr, false, 0, 0, cubes_vec);

  std::vector<flatbuffers::Offset<::foxglove::SceneEntity>> entity_offsets = {entity};
  auto entities_vec = builder.CreateVector(entity_offsets);

  auto scene = ::foxglove::CreateSceneUpdate(builder, 0, entities_vec);
  builder.Finish(scene);

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.schema_name = "foxglove.SceneUpdate";
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

FoxgloveMessage FoxgloveConverter::audio_frame_fbs(const Bytes& raw) {
  FoxgloveMessage result;

  zerocopy::AudioFrame frame;

  if VUNLIKELY (!(frame << raw)) {
    MLOG_W("Failed to deserialize AudioFrame (raw={})", raw.size());
    return result;
  }

  if VUNLIKELY (frame.format() != zerocopy::AudioFrame::kFormatPcmS16 ||
                frame.layout() != zerocopy::AudioFrame::kLayoutInterleaved) {
    MLOG_W("Foxglove RawAudio supports only interleaved pcm-s16 AudioFrame messages");
    return result;
  }

  const uint32_t sample_rate = frame.sample_rate();
  const uint32_t num_samples = frame.num_samples();
  const uint16_t num_channels = frame.num_channels();

  if VUNLIKELY (!frame.data() || frame.size() == 0 || sample_rate == 0 || num_samples == 0 || num_channels == 0) {
    MLOG_W("AudioFrame invalid for RawAudio: size={} sample_rate={} samples={} channels={}", frame.size(), sample_rate,
           num_samples, num_channels);
    return result;
  }

  const size_t channel_sample_size = static_cast<size_t>(num_channels) * sizeof(int16_t);

  if VUNLIKELY (channel_sample_size == 0 || num_samples > std::numeric_limits<size_t>::max() / channel_sample_size) {
    MLOG_W("AudioFrame RawAudio size overflow: samples={} channels={}", num_samples, num_channels);
    return result;
  }

  const size_t expected_size = static_cast<size_t>(num_samples) * channel_sample_size;

  if VUNLIKELY (frame.size() != expected_size) {
    MLOG_W("AudioFrame RawAudio size mismatch: actual={} expected={}", frame.size(), expected_size);
    return result;
  }

  thread_local flatbuffers::FlatBufferBuilder builder(64 * 1024);
  builder.Clear();

  auto ts = make_timestamp_from_ns(frame.header.time_meas);
  auto fmt = builder.CreateString("pcm-s16");

  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> data_vec;

  if VLIKELY (frame.data() && frame.size() > 0) {
    data_vec = builder.CreateVector(frame.data(), frame.size());
  } else {
    data_vec = builder.CreateVector(static_cast<const uint8_t*>(nullptr), 0);
  }

  auto ra = ::foxglove::CreateRawAudio(builder, &ts, data_vec, fmt, sample_rate, static_cast<uint32_t>(num_channels));
  builder.Finish(ra);

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.schema_name = "foxglove.RawAudio";
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

}  // namespace webviz
}  // namespace vlink
