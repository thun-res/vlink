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
#include <vlink/zerocopy/message_parser.h>
#include <vlink/zerocopy/object_array.h>
#include <vlink/zerocopy/occupancy_grid.h>
#include <vlink/zerocopy/point_cloud.h>
#include <vlink/zerocopy/raw_data.h>
#include <vlink/zerocopy/tensor.h>

#include <mutex>
#include <nlohmann/json.hpp>
#include <rerun.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../webviz_types.h"

namespace vlink {
namespace webviz {

using Json = nlohmann::json;

#ifdef VLINK_HAS_FBS_COMPILER
struct FbsObjectView;
#endif

struct RerunMap final {
  std::string ser;
  UrlSelector url_selector;
  std::string archetype;
  std::string encoding;
  std::string converter;
  std::string timestamp_field;
  std::string timestamp_unit;
  std::vector<FieldMapping> field_mappings;
};

class RerunConverter final {
 public:
  struct Config final {
    std::string proto_dir;
    std::string fbs_dir;
    std::string schema_plugin_path;
    std::string convert_plugin_path;
    std::string convert_plugin_config;
    std::vector<std::string> vlink_msgs;
    std::string timestamp_timeline{"timestamp"};
    bool use_timestamp_timeline{true};
  };

  explicit RerunConverter(const Config& config);

  ~RerunConverter();

  void convert_and_log(::rerun::RecordingStream& rec, const std::string& entity_path, std::string_view url,
                       SchemaType schema_type, const std::string& ser, const Bytes& raw,
                       int64_t fallback_timestamp_ns = -1);

 private:
  void apply_message_timestamp(::rerun::RecordingStream& rec, int64_t timestamp_ns) const;

  bool init_proto_resolver();

  bool init_convert_plugin();

  void load_mappings();

  bool load_mapping_file(const std::string& path);

  const google::protobuf::Descriptor* find_proto_descriptor(const std::string& proto_name);

  std::unique_ptr<google::protobuf::Message> deserialize_proto_message(const std::string& ser, const Bytes& raw);

  const std::vector<const RerunMap*>& find_all_mappings(std::string_view url, const std::string& ser) const;

  static bool log_camera_frame(::rerun::RecordingStream& rec, const std::string& entity_path,
                               const zerocopy::CameraFrame& frame);

  static bool log_camera_frame(::rerun::RecordingStream& rec, const std::string& entity_path, const Bytes& raw);

  static bool log_point_cloud(::rerun::RecordingStream& rec, const std::string& entity_path,
                              const zerocopy::PointCloud& point_cloud);

  static bool log_point_cloud(::rerun::RecordingStream& rec, const std::string& entity_path, const Bytes& raw);

  static bool log_raw_data(::rerun::RecordingStream& rec, const std::string& entity_path,
                           const zerocopy::MessageParser& parser);

  static bool log_raw_data(::rerun::RecordingStream& rec, const std::string& entity_path, const Bytes& raw);

  static bool log_occupancy_grid(::rerun::RecordingStream& rec, const std::string& entity_path,
                                 const zerocopy::MessageParser& parser);

  static bool log_occupancy_grid(::rerun::RecordingStream& rec, const std::string& entity_path, const Bytes& raw);

  static bool log_tensor(::rerun::RecordingStream& rec, const std::string& entity_path,
                         const zerocopy::MessageParser& parser);

  static bool log_tensor(::rerun::RecordingStream& rec, const std::string& entity_path, const Bytes& raw);

  static bool log_object_array(::rerun::RecordingStream& rec, const std::string& entity_path,
                               const zerocopy::MessageParser& parser);

  static bool log_object_array(::rerun::RecordingStream& rec, const std::string& entity_path, const Bytes& raw);

  static bool log_audio_frame(::rerun::RecordingStream& rec, const std::string& entity_path,
                              const zerocopy::MessageParser& parser);

  static bool log_audio_frame(::rerun::RecordingStream& rec, const std::string& entity_path, const Bytes& raw);

  static bool log_proto_with_mapping(::rerun::RecordingStream& rec, const std::string& entity_path,
                                     const RerunMap& mapping, const google::protobuf::Message& msg);

  static bool log_geo_points(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                             const google::protobuf::Message& msg);

  static bool log_transform3d(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                              const google::protobuf::Message& msg);

  static bool log_boxes3d(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                          const google::protobuf::Message& msg);

  static bool log_points3d(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                           const google::protobuf::Message& msg);

  static bool log_encoded_image(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                                const google::protobuf::Message& msg);

  static bool log_image(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                        const google::protobuf::Message& msg);

  static bool log_text_log(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                           const google::protobuf::Message& msg);

  static bool log_pinhole(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                          const google::protobuf::Message& msg);

  static bool log_depth_image(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                              const google::protobuf::Message& msg);

  static bool log_scalars(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                          const google::protobuf::Message& msg);

  static bool log_line_strips3d(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                                const google::protobuf::Message& msg);

  static bool log_line_strips2d(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                                const google::protobuf::Message& msg);

  static bool log_boxes2d(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                          const google::protobuf::Message& msg);

  static bool log_arrows3d(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                           const google::protobuf::Message& msg);

  static bool log_points2d(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                           const google::protobuf::Message& msg);

  static bool log_segmentation_image(::rerun::RecordingStream& rec, const std::string& entity_path,
                                     const RerunMap& mapping, const google::protobuf::Message& msg);

  static bool log_series_line(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                              const google::protobuf::Message& msg);

  static bool log_series_point(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                               const google::protobuf::Message& msg);

  static bool log_arrows2d(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                           const google::protobuf::Message& msg);

  static bool log_mesh3d(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                         const google::protobuf::Message& msg);

  static bool log_cylinders3d(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                              const google::protobuf::Message& msg);

  static bool log_ellipsoids3d(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                               const google::protobuf::Message& msg);

  static bool log_geo_line_strings(::rerun::RecordingStream& rec, const std::string& entity_path,
                                   const RerunMap& mapping, const google::protobuf::Message& msg);

  static bool log_bar_chart(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                            const google::protobuf::Message& msg);

  static bool log_annotation_context(::rerun::RecordingStream& rec, const std::string& entity_path,
                                     const RerunMap& mapping, const google::protobuf::Message& msg);

  static bool log_capsules3d(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                             const google::protobuf::Message& msg);

  static bool log_encoded_depth_image(::rerun::RecordingStream& rec, const std::string& entity_path,
                                      const RerunMap& mapping, const google::protobuf::Message& msg);

  static bool log_asset3d(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                          const google::protobuf::Message& msg);

  static bool log_graph_nodes(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                              const google::protobuf::Message& msg);

  static bool log_graph_edges(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                              const google::protobuf::Message& msg);

  static bool log_view_coordinates(::rerun::RecordingStream& rec, const std::string& entity_path,
                                   const RerunMap& mapping, const google::protobuf::Message& msg);

  static bool log_instance_poses3d(::rerun::RecordingStream& rec, const std::string& entity_path,
                                   const RerunMap& mapping, const google::protobuf::Message& msg);

  static bool log_asset_video(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                              const google::protobuf::Message& msg);

  static bool log_video_frame_reference(::rerun::RecordingStream& rec, const std::string& entity_path,
                                        const RerunMap& mapping, const google::protobuf::Message& msg);

  static bool log_tensor(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                         const google::protobuf::Message& msg);

  static bool log_plugin_json(::rerun::RecordingStream& rec, const std::string& entity_path,
                              const std::string& archetype, const Json& json_data);

  static int64_t clamp_header_timestamp_ns(uint64_t timestamp_ns);

  static Bytes decode_plugin_binary(const Json& j, std::string_view base64_key = "data_base64",
                                    std::string_view array_key = "data");

  static std::string normalize_plugin_text(std::string text);

  static std::string infer_media_type(const Json& j);

  static bool resolve_plugin_image_size(const Json& j, uint32_t& width, uint32_t& height);

  static void log_view_coordinates_value(::rerun::RecordingStream& rec, const std::string& entity_path,
                                         std::string system);

#ifdef VLINK_HAS_FBS_COMPILER
  bool init_fbs_resolver();

  bool resolve_fbs_schema(const std::string& fbs_ser, std::string& schema_data);

  bool log_fbs_with_mapping(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                            const std::string& ser, const Bytes& raw);

  static double get_fbs_double(const flatbuffers::Table& table, const reflection::Object& obj,
                               const std::string& field_name);

  static double get_fbs_double(const FbsObjectView& view, const std::string& field_name);

  static double get_fbs_double(const flatbuffers::Table& table, const reflection::Object& obj,
                               const std::string& field_name, const FieldMapping& mapping);

  static double get_fbs_double(const FbsObjectView& view, const std::string& field_name, const FieldMapping& mapping);

  static std::string get_fbs_string(const flatbuffers::Table& table, const reflection::Object& obj,
                                    const std::string& field_name, const reflection::Schema* schema = nullptr);

  static std::string get_fbs_string(const FbsObjectView& view, const std::string& field_name,
                                    const reflection::Schema* schema = nullptr);

  static std::string get_fbs_string(const flatbuffers::Table& table, const reflection::Object& obj,
                                    const std::string& field_name, const FieldMapping& mapping,
                                    const reflection::Schema* schema = nullptr);

  static std::string get_fbs_string(const FbsObjectView& view, const std::string& field_name,
                                    const FieldMapping& mapping, const reflection::Schema* schema = nullptr);
#endif

  std::vector<RerunMap> mappings_;
  std::unordered_map<std::string, std::vector<const RerunMap*>> mapping_multi_index_;
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
  std::unordered_map<std::string, std::string> fbs_schema_cache_;
#endif

  std::unordered_set<std::string> warned_types_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(RerunConverter)
};

}  // namespace webviz
}  // namespace vlink
