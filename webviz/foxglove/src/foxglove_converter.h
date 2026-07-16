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

#pragma once

#if __has_include(<flatbuffers/idl.h>)
#include <flatbuffers/base.h>
#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/idl.h>
#include <flatbuffers/reflection.h>
#include <flatbuffers/reflection_generated.h>

#if FLATBUFFERS_VERSION_MAJOR >= 22
#define VLINK_HAS_FBS_COMPILER
#endif

#endif

#if __has_include(<google/protobuf/compiler/importer.h>)
#include <google/protobuf/compiler/importer.h>
#define VLINK_HAS_PROTO_COMPILER
#endif

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>
#include <vlink/base/bytes.h>
#include <vlink/base/helpers.h>
#include <vlink/base/logger.h>
#include <vlink/base/macros.h>
#include <vlink/base/plugin.h>
#include <vlink/extension/convert_plugin_interface.h>
#include <vlink/extension/schema_plugin_interface.h>
#include <vlink/extension/schema_plugin_manager.h>
#include <vlink/zerocopy/audio_frame.h>
#include <vlink/zerocopy/camera_frame.h>
#include <vlink/zerocopy/object_array.h>
#include <vlink/zerocopy/occupancy_grid.h>
#include <vlink/zerocopy/point_cloud.h>
#include <vlink/zerocopy/raw_data.h>
#include <vlink/zerocopy/tensor.h>

#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../webviz_types.h"

namespace foxglove {
struct Time;
}

namespace vlink {
namespace webviz {

using Json = nlohmann::json;

#ifdef VLINK_HAS_FBS_COMPILER
struct FbsObjectView;
#endif

struct FoxgloveMapping final {
  std::string ser;
  UrlSelector url_selector;
  std::string schema;
  std::string encoding;
  std::string schema_encoding;
  std::string converter;
  std::string timestamp_field;
  std::string timestamp_unit;
  std::vector<FieldMapping> field_mappings;
};

struct FoxgloveMessage final {
  bool success{false};
  bool is_send_time{false};
  std::string schema_name;
  std::string encoding;
  std::string schema_encoding;
  std::string schema_data;
  Bytes payload;
  int64_t timestamp_ns{-1};
};

class FoxgloveConverter final {
 public:
  struct Config final {
    std::string proto_dir;
    std::string fbs_dir;
    std::string schema_plugin_path;
    std::string convert_plugin_path;
    std::string convert_plugin_config;
    std::vector<std::string> vlink_msgs;
  };

  explicit FoxgloveConverter(const Config& config);

  ~FoxgloveConverter();

  FoxgloveMessage convert(std::string_view url, SchemaType schema_type, const std::string& ser, const Bytes& raw);

  bool get_schema_info(std::string_view url, SchemaType schema_type, const std::string& ser, std::string& schema_name,
                       std::string& encoding, std::string& schema_encoding, std::string& schema_data,
                       bool* is_send_time = nullptr);

  bool resolve_schema_by_name(const std::string& schema_name, const std::string& schema_encoding,
                              std::string& schema_data);

  bool has_send_time_mapping() const;

 private:
  bool init_proto_resolver();

  bool init_convert_plugin();

  void load_mappings();

  bool load_mapping_file(const std::string& path);

  const google::protobuf::Descriptor* find_proto_descriptor(const std::string& proto_name);

  std::unique_ptr<google::protobuf::Message> deserialize_proto_message(const std::string& ser, const Bytes& raw);

  static FoxgloveMessage camera_frame_fbs(const Bytes& raw);

  static FoxgloveMessage point_cloud_fbs(const Bytes& raw);

  static FoxgloveMessage raw_data_to_log(const Bytes& raw);

  static FoxgloveMessage occupancy_grid_fbs(const Bytes& raw);

  static FoxgloveMessage tensor_fbs(const Bytes& raw);

  static FoxgloveMessage object_array_fbs(const Bytes& raw);

  static FoxgloveMessage audio_frame_fbs(const Bytes& raw);

  static FoxgloveMessage string_to_log(const Bytes& raw);

  static flatbuffers::Offset<flatbuffers::Vector<uint8_t>> create_proto_repeated_byte_vector(
      flatbuffers::FlatBufferBuilder& builder, const google::protobuf::Message& msg,
      const google::protobuf::FieldDescriptor& field, const google::protobuf::Reflection& ref);

  static ::foxglove::Time make_timestamp_from_us(uint64_t us);

  static ::foxglove::Time make_timestamp_from_ns(uint64_t ns);

  FoxgloveMessage convert_proto_mapping(const FoxgloveMapping& mapping, const std::string& ser, const Bytes& raw);

  FoxgloveMessage convert_proto_mapping(const FoxgloveMapping& mapping, const google::protobuf::Message& message);

  static FoxgloveMessage convert_location_fix(const FoxgloveMapping& mapping, const google::protobuf::Message& msg);

  static FoxgloveMessage convert_pose_in_frame(const FoxgloveMapping& mapping, const google::protobuf::Message& msg);

  static FoxgloveMessage convert_scene_update(const FoxgloveMapping& mapping, const google::protobuf::Message& msg);

  static FoxgloveMessage convert_frame_transform(const FoxgloveMapping& mapping, const google::protobuf::Message& msg);

  static FoxgloveMessage convert_log(const FoxgloveMapping& mapping, const google::protobuf::Message& msg);

  static FoxgloveMessage convert_laser_scan(const FoxgloveMapping& mapping, const google::protobuf::Message& msg);

  static FoxgloveMessage convert_raw_image(const FoxgloveMapping& mapping, const google::protobuf::Message& msg);

  static FoxgloveMessage convert_geo_json(const FoxgloveMapping& mapping, const google::protobuf::Message& msg);

  static FoxgloveMessage convert_poses_in_frame(const FoxgloveMapping& mapping, const google::protobuf::Message& msg);

  static FoxgloveMessage convert_frame_transforms(const FoxgloveMapping& mapping, const google::protobuf::Message& msg);

  static FoxgloveMessage convert_location_fixes(const FoxgloveMapping& mapping, const google::protobuf::Message& msg);

  static FoxgloveMessage convert_camera_calibration(const FoxgloveMapping& mapping,
                                                    const google::protobuf::Message& msg);

  static FoxgloveMessage convert_compressed_video(const FoxgloveMapping& mapping, const google::protobuf::Message& msg);

  static FoxgloveMessage convert_grid(const FoxgloveMapping& mapping, const google::protobuf::Message& msg);

  static FoxgloveMessage convert_image_annotations(const FoxgloveMapping& mapping,
                                                   const google::protobuf::Message& msg);

  static FoxgloveMessage convert_joint_states(const FoxgloveMapping& mapping, const google::protobuf::Message& msg);

  static FoxgloveMessage convert_point3_in_frame(const FoxgloveMapping& mapping, const google::protobuf::Message& msg);

  static FoxgloveMessage convert_raw_audio(const FoxgloveMapping& mapping, const google::protobuf::Message& msg);

  static FoxgloveMessage convert_voxel_grid(const FoxgloveMapping& mapping, const google::protobuf::Message& msg);

  bool resolve_fbs_schema(const std::string& schema_name, std::string& schema_data);

  bool resolve_proto_schema(const std::string& proto_name, std::string& schema_data);

  bool resolve_custom_fbs_schema(const std::string& fbs_ser, std::string& schema_data);

  const FoxgloveMapping* find_mapping(std::string_view url, const std::string& ser, bool* ambiguous = nullptr) const;

#ifdef VLINK_HAS_FBS_COMPILER
  bool init_fbs_resolver();

  bool find_fbs_parser_locked(const std::string& fbs_ser);

  FoxgloveMessage convert_fbs_mapping(const FoxgloveMapping& mapping, const std::string& ser, const Bytes& raw);

  static double get_fbs_double(const flatbuffers::Table& table, const reflection::Object& obj,
                               const std::string& field_name, const FieldMapping& mapping);

  static double get_fbs_double(const FbsObjectView& view, const std::string& field_name, const FieldMapping& mapping);

  static std::string get_fbs_string(const flatbuffers::Table& table, const reflection::Object& obj,
                                    const std::string& field_name, const FieldMapping& mapping,
                                    const reflection::Schema* schema = nullptr);

  static std::string get_fbs_string(const FbsObjectView& view, const std::string& field_name,
                                    const FieldMapping& mapping, const reflection::Schema* schema = nullptr);
#endif

  std::unordered_map<std::string, std::string> schema_cache_;
  std::vector<FoxgloveMapping> mappings_;
  std::unordered_map<std::string, std::vector<const FoxgloveMapping*>> mapping_index_;
  std::mutex mtx_;
  Config config_;
  const uint64_t cache_owner_id_{allocate_cache_owner_id()};

  std::shared_ptr<SchemaPluginInterface> schema_interface_;
  Plugin convert_plugin_loader_;
  std::shared_ptr<ConvertPluginInterface> convert_plugin_;
  google::protobuf::DynamicMessageFactory proto_factory_;

#ifdef VLINK_HAS_PROTO_COMPILER
  std::shared_ptr<google::protobuf::compiler::DiskSourceTree> source_tree_;
  std::shared_ptr<google::protobuf::compiler::Importer> importer_;
  std::shared_ptr<google::protobuf::DynamicMessageFactory> disk_factory_;
  std::unordered_map<std::string, const google::protobuf::Descriptor*> imported_proto_descriptors_;
#endif

#ifdef VLINK_HAS_FBS_COMPILER
  std::vector<std::unique_ptr<flatbuffers::Parser>> fbs_parser_vec_;
  std::unordered_map<std::string, size_t> fbs_parsers_;
  std::unordered_set<std::string> fbs_not_found_;
#endif

  VLINK_DISALLOW_COPY_AND_ASSIGN(FoxgloveConverter)
};

}  // namespace webviz
}  // namespace vlink
