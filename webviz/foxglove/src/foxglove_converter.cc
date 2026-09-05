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

#include "./foxglove_converter.h"

#include <vlink/base/functional.h>
#include <vlink/base/helpers.h>
#include <vlink/zerocopy/message_parser.h>

#include <CameraCalibration.fbs.hpp>
#include <CameraCalibration_bfbs.fbs.hpp>
#include <CircleAnnotation.fbs.hpp>
#include <Color.fbs.hpp>
#include <CompressedAudio_bfbs.fbs.hpp>
#include <CompressedImage.fbs.hpp>
#include <CompressedImage_bfbs.fbs.hpp>
#include <CompressedPointCloud_bfbs.fbs.hpp>
#include <CompressedVideo.fbs.hpp>
#include <CompressedVideo_bfbs.fbs.hpp>
#include <CubePrimitive.fbs.hpp>
#include <Event_bfbs.fbs.hpp>
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
#include <Odometry_bfbs.fbs.hpp>
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
#include "../../zerocopy_message.h"

#ifdef _WIN32
#undef min
#undef max
#undef GetMessage
#undef ERROR
#endif

namespace vlink {
namespace webviz {

[[maybe_unused]] static bool is_flatbuffers_schema_encoding(std::string_view encoding) {
  return is_flatbuffers_encoding(normalize_token(encoding));
}

constexpr std::string_view kFoxgloveFlatbufferEncoding = "flatbuffer";

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

static bool camera_frame_raw_info(zerocopy::CameraFrame::Format format, uint32_t width, uint32_t height,
                                  std::string& encoding, uint32_t& step, size_t& expected, bool& rgb_planar) {
  if VUNLIKELY (width == 0 || height == 0) {
    return false;
  }

  size_t pixels = 0;

  if VUNLIKELY (!mul_size(static_cast<size_t>(width), static_cast<size_t>(height), pixels)) {
    return false;
  }

  auto set_bpp = [&width, &pixels, &encoding, &step, &expected](std::string_view name, size_t bytes_per_pixel) {
    size_t row_bytes = 0;
    size_t data_size = 0;

    return mul_size(static_cast<size_t>(width), bytes_per_pixel, row_bytes) &&
           mul_size(pixels, bytes_per_pixel, data_size) &&
           set_raw_info(name, row_bytes, data_size, encoding, step, expected);
  };

  auto set_yuv420 = [&width, &height, &pixels, &encoding, &step, &expected](std::string_view name) {
    if VUNLIKELY ((width % 2U) != 0 || (height % 2U) != 0 ||
                  pixels > std::numeric_limits<size_t>::max() - pixels / 2U) {
      return false;
    }

    return set_raw_info(name, width, pixels + pixels / 2U, encoding, step, expected);
  };

  rgb_planar = false;

  switch (format) {
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
      return set_bpp(zerocopy::CameraFrame::encoding_from_format(format), 1U);
    default:
      return false;
  }
}

static bool read_parser_uint64(const zerocopy::MessageParser& parser, std::string_view path, uint64_t& out) {
  zerocopy::MessageParser::Value value;

  if VUNLIKELY (!parser.value(path, value)) {
    return false;
  }

  const auto* integer = std::get_if<uint64_t>(&value);

  if VUNLIKELY (integer == nullptr) {
    return false;
  }

  out = *integer;
  return true;
}

static bool read_parser_bytes(const zerocopy::MessageParser& parser, std::string_view path, Bytes& out) {
  zerocopy::MessageParser::Value value;

  if VUNLIKELY (!parser.value(path, value)) {
    return false;
  }

  const auto* bytes = std::get_if<Bytes>(&value);

  if VUNLIKELY (bytes == nullptr) {
    return false;
  }

  out = Bytes::shallow_copy(bytes->data(), bytes->size());
  return true;
}

template <typename T>
static void write_unaligned(uint8_t* target, T value) {
  std::memcpy(target, &value, sizeof(value));
}

static bool write_point_field(const PointCloudView& view, size_t index, const PointCloudFieldView& field,
                              ::foxglove::NumericType type, uint8_t* target) {
  zerocopy::MessageParser::Value value;

  if VUNLIKELY (!view.value(index, field, value)) {
    return false;
  }

  if (const auto* integer = std::get_if<int64_t>(&value)) {
    switch (type) {
      case ::foxglove::NumericType::INT8:
        write_unaligned(target, static_cast<int8_t>(*integer));
        return true;
      case ::foxglove::NumericType::INT16:
        write_unaligned(target, static_cast<int16_t>(*integer));
        return true;
      case ::foxglove::NumericType::INT32:
        write_unaligned(target, static_cast<int32_t>(*integer));
        return true;
      default:
        return false;
    }
  }

  if (const auto* integer = std::get_if<uint64_t>(&value)) {
    switch (type) {
      case ::foxglove::NumericType::UINT8:
        write_unaligned(target, static_cast<uint8_t>(*integer));
        return true;
      case ::foxglove::NumericType::UINT16:
        write_unaligned(target, static_cast<uint16_t>(*integer));
        return true;
      case ::foxglove::NumericType::UINT32:
        write_unaligned(target, static_cast<uint32_t>(*integer));
        return true;
      default:
        return false;
    }
  }

  const auto* number = std::get_if<double>(&value);

  if VUNLIKELY (number == nullptr) {
    return false;
  }

  if (type == ::foxglove::NumericType::FLOAT32) {
    write_unaligned(target, static_cast<float>(*number));
    return true;
  }

  if (type == ::foxglove::NumericType::FLOAT64) {
    write_unaligned(target, *number);
    return true;
  }

  return false;
}

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

FoxgloveMessage FoxgloveConverter::convert(std::string_view url, SchemaType schema_type, const std::string& ser,
                                           const Bytes& raw) {
  activate_cache_owner(cache_owner_id_);
  FoxgloveMessage result;
  bool ambiguous = false;
  const auto* mapping = find_mapping(url, ser, &ambiguous);
  const auto zerocopy_type = zerocopy::MessageParser::detect_type(ser);

  if VUNLIKELY (ambiguous) {
    return result;
  }

  if (schema_type == SchemaType::kZeroCopy && mapping != nullptr && mapping->encoding == "zerocopy" &&
      mapping->converter.empty() && !mapping->field_mappings.empty()) {
    std::vector<std::string> mapping_sources;
    mapping_sources.reserve(mapping->field_mappings.size() * 2 + 1);
    mapping_sources.push_back(mapping->timestamp_field);

    for (const auto& field_mapping : mapping->field_mappings) {
      mapping_sources.push_back(field_mapping.source);
      mapping_sources.push_back(field_mapping.expression);
    }

    auto message = make_zerocopy_message(ser, raw, mapping_sources);

    if VUNLIKELY (!message) {
      MLOG_W("Failed to adapt zerocopy message for Foxglove mapping: {}", ser);
      return result;
    }

    return convert_proto_mapping(*mapping, *message);
  }

  if (schema_type == SchemaType::kZeroCopy) {
    if (zerocopy_type == zerocopy::MessageParser::Type::kCameraFrame) {
      return camera_frame_fbs(raw);
    }

    if (zerocopy_type == zerocopy::MessageParser::Type::kPointCloud) {
      return point_cloud_fbs(raw);
    }

    if (zerocopy_type == zerocopy::MessageParser::Type::kRawData) {
      return raw_data_to_log(raw);
    }

    if (zerocopy_type == zerocopy::MessageParser::Type::kOccupancyGrid) {
      return occupancy_grid_fbs(raw);
    }

    if (zerocopy_type == zerocopy::MessageParser::Type::kTensor) {
      return tensor_fbs(raw);
    }

    if (zerocopy_type == zerocopy::MessageParser::Type::kObjectArray) {
      return object_array_fbs(raw);
    }

    if (zerocopy_type == zerocopy::MessageParser::Type::kAudioFrame) {
      return audio_frame_fbs(raw);
    }
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
                  ser, cache_owner_id_,
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
                    ser, cache_owner_id_,
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

      if (is_flatbuffers_schema_encoding(plugin_result.encoding)) {
        plugin_result.encoding = std::string(kFoxgloveFlatbufferEncoding);
      }

      if (is_flatbuffers_schema_encoding(plugin_result.schema_encoding)) {
        plugin_result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
      }
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

    if VLIKELY (find_fbs_parser_locked(ser)) {
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

  bool ambiguous = false;
  const auto* mapping = find_mapping(url, ser, &ambiguous);
  const auto zerocopy_type = zerocopy::MessageParser::detect_type(ser);

  if VUNLIKELY (ambiguous) {
    return false;
  }

  if (schema_type == SchemaType::kZeroCopy && mapping != nullptr && mapping->encoding == "zerocopy" &&
      mapping->converter.empty() && !mapping->field_mappings.empty()) {
    schema_name = mapping->schema;
    schema_encoding =
        mapping->schema_encoding.empty() ? std::string(kFoxgloveFlatbufferEncoding) : mapping->schema_encoding;
    encoding = schema_encoding;
    return resolve_schema_by_name(schema_name, schema_encoding, schema_data);
  }

  if (schema_type == SchemaType::kZeroCopy) {
    if (zerocopy_type == zerocopy::MessageParser::Type::kCameraFrame) {
      schema_name = "foxglove.RawImage";
      encoding = std::string(kFoxgloveFlatbufferEncoding);
      schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
      return resolve_fbs_schema(schema_name, schema_data);
    }

    if (zerocopy_type == zerocopy::MessageParser::Type::kPointCloud) {
      schema_name = "foxglove.PointCloud";
      encoding = std::string(kFoxgloveFlatbufferEncoding);
      schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
      return resolve_fbs_schema(schema_name, schema_data);
    }

    if (zerocopy_type == zerocopy::MessageParser::Type::kRawData) {
      schema_name = "foxglove.Log";
      encoding = std::string(kFoxgloveFlatbufferEncoding);
      schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
      return resolve_fbs_schema(schema_name, schema_data);
    }

    if (zerocopy_type == zerocopy::MessageParser::Type::kOccupancyGrid) {
      schema_name = "foxglove.Grid";
      encoding = std::string(kFoxgloveFlatbufferEncoding);
      schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
      return resolve_fbs_schema(schema_name, schema_data);
    }

    if (zerocopy_type == zerocopy::MessageParser::Type::kTensor) {
      schema_name = "foxglove.Log";
      encoding = std::string(kFoxgloveFlatbufferEncoding);
      schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
      return resolve_fbs_schema(schema_name, schema_data);
    }

    if (zerocopy_type == zerocopy::MessageParser::Type::kObjectArray) {
      schema_name = "foxglove.SceneUpdate";
      encoding = std::string(kFoxgloveFlatbufferEncoding);
      schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
      return resolve_fbs_schema(schema_name, schema_data);
    }

    if (zerocopy_type == zerocopy::MessageParser::Type::kAudioFrame) {
      schema_name = "foxglove.RawAudio";
      encoding = std::string(kFoxgloveFlatbufferEncoding);
      schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
      return resolve_fbs_schema(schema_name, schema_data);
    }
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

      if (is_flatbuffers_schema_encoding(encoding)) {
        encoding = std::string(kFoxgloveFlatbufferEncoding);
      }

      if (is_flatbuffers_schema_encoding(schema_encoding)) {
        schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
      }

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

          if (is_flatbuffers_schema_encoding(mapping.encoding)) {
            mapping.encoding = std::string(kFoxgloveFlatbufferEncoding);
          }

          if (is_flatbuffers_schema_encoding(mapping.schema_encoding)) {
            mapping.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);
          }

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

          if VUNLIKELY (mapping.encoding != "protobuf" && mapping.encoding != "zerocopy" &&
                        !is_flatbuffers_schema_encoding(mapping.encoding)) {
            MLOG_W("Invalid mapping in {}: unsupported source encoding {}", path, mapping.encoding);
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

FoxgloveMessage FoxgloveConverter::camera_frame_fbs(const Bytes& raw) {
  FoxgloveMessage result;

  zerocopy::CameraFrame frame;

  if VUNLIKELY (!(frame << raw)) {
    MLOG_W("Failed to deserialize CameraFrame");
    return result;
  }

  const auto fmt = frame.format();
  const auto width = frame.width();
  const auto height = frame.height();
  const auto time_meas = frame.header.time_meas;
  const std::string_view frame_id_value(frame.header.frame_id,
                                        strnlen(frame.header.frame_id, sizeof(frame.header.frame_id)));
  const auto* data = frame.data();
  const size_t data_size = frame.size();

  if VUNLIKELY (data == nullptr || data_size == 0) {
    MLOG_W("CameraFrame payload is empty");
    return result;
  }

  std::string fmt_str(zerocopy::CameraFrame::encoding_from_format(fmt));

  if VUNLIKELY (fmt == zerocopy::CameraFrame::kFormatUnknown || fmt_str == "unknown") {
    MLOG_W("CameraFrame format is unknown, skipping");
    return result;
  }

  thread_local flatbuffers::FlatBufferBuilder builder(256 * 1024);
  builder.Clear();

  auto ts = make_timestamp_from_ns(time_meas);
  auto frame_id = builder.CreateString(frame_id_value.data(), frame_id_value.size());

  if VUNLIKELY (fmt == zerocopy::CameraFrame::kFormatH266) {
    MLOG_W("Foxglove CompressedVideo does not support H.266");
    return result;
  }

  if (fmt == zerocopy::CameraFrame::kFormatH264 || fmt == zerocopy::CameraFrame::kFormatH265 ||
      fmt == zerocopy::CameraFrame::kFormatAv1) {
    if VUNLIKELY (frame.stream() == zerocopy::CameraFrame::kStreamB) {
      MLOG_W("Foxglove CompressedVideo does not support B-frames");
      return result;
    }

    auto data_vec = builder.CreateVector(data, data_size);
    auto format = builder.CreateString(fmt_str);
    auto msg = ::foxglove::CreateCompressedVideo(builder, &ts, frame_id, data_vec, format);
    builder.Finish(msg);
    result.schema_name = "foxglove.CompressedVideo";
  } else if (fmt == zerocopy::CameraFrame::kFormatJpeg || fmt == zerocopy::CameraFrame::kFormatPng ||
             fmt == zerocopy::CameraFrame::kFormatMjpeg || fmt == zerocopy::CameraFrame::kFormatWebp) {
    if (fmt == zerocopy::CameraFrame::kFormatMjpeg) {
      fmt_str = "jpeg";
    }

    auto data_vec = builder.CreateVector(data, data_size);
    auto format = builder.CreateString(fmt_str);
    auto msg = ::foxglove::CreateCompressedImage(builder, &ts, frame_id, data_vec, format);
    builder.Finish(msg);
    result.schema_name = "foxglove.CompressedImage";
  } else {
    uint32_t step = 0;
    size_t expected = 0;
    bool rgb_planar = false;

    if VUNLIKELY (!camera_frame_raw_info(fmt, width, height, fmt_str, step, expected, rgb_planar) ||
                  data_size != expected) {
      MLOG_W("CameraFrame raw format is invalid or unsupported, format={} width={} height={} size={}", fmt_str, width,
             height, data_size);
      return result;
    }

    std::vector<uint8_t> rgb_data;
    flatbuffers::Offset<flatbuffers::Vector<uint8_t>> data_vec;

    if (rgb_planar) {
      size_t pixels = 0;

      if VUNLIKELY (!mul_size(static_cast<size_t>(width), static_cast<size_t>(height), pixels)) {
        return result;
      }

      rgb_data.resize(expected);

      const auto* r = data;
      const auto* g = r + pixels;
      const auto* b = g + pixels;

      for (size_t i = 0; i < pixels; ++i) {
        rgb_data[i * 3U + 0U] = r[i];
        rgb_data[i * 3U + 1U] = g[i];
        rgb_data[i * 3U + 2U] = b[i];
      }

      data_vec = builder.CreateVector(rgb_data);
    } else {
      data_vec = builder.CreateVector(data, expected);
    }

    auto encoding = builder.CreateString(fmt_str);
    auto msg = ::foxglove::CreateRawImage(builder, &ts, frame_id, width, height, encoding, step, data_vec);
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

  zerocopy::PointCloud point_cloud;

  if VUNLIKELY (!(point_cloud << raw)) {
    MLOG_W("Failed to deserialize PointCloud (raw={})", raw.size());
    return result;
  }
  const PointCloudView view(point_cloud);
  if VUNLIKELY (!view.valid()) {
    MLOG_W("PointCloud layout is invalid");
    return result;
  }

  const size_t point_count = point_cloud.size();
  const size_t pack_size = point_cloud.pack_size();
  const uint64_t extent = point_cloud.get_extent();
  const uint64_t time_meas = point_cloud.header.time_meas;
  const std::string_view frame_id_value(point_cloud.header.frame_id,
                                        strnlen(point_cloud.header.frame_id, sizeof(point_cloud.header.frame_id)));

  if VUNLIKELY (point_count == 0 || pack_size == 0) {
    MLOG_W("PointCloud is empty: size={} pack_size={}", point_count, pack_size);
    return result;
  }

  if VUNLIKELY (point_count > std::numeric_limits<size_t>::max() / pack_size) {
    MLOG_W("PointCloud payload size overflow: size={} pack_size={}", point_count, pack_size);
    return result;
  }

  const auto data_size = static_cast<size_t>(point_count * pack_size);
  const bool has_compressed_xyz = extent != 0;
  const auto& fields = view.fields();

  if VUNLIKELY (fields.empty()) {
    MLOG_W("PointCloud key map is empty");
    return result;
  }

  const bool leading_xyz =
      fields.size() >= 3 && fields[0].field.name == "x" && fields[1].field.name == "y" && fields[2].field.name == "z";
  bool has_x = leading_xyz;
  bool has_y = leading_xyz;
  bool has_z = leading_xyz;

  if VUNLIKELY (!leading_xyz) {
    for (const auto& field : fields) {
      has_x = has_x || field.field.name == "x";
      has_y = has_y || field.field.name == "y";
      has_z = has_z || field.field.name == "z";
    }
  }

  if VUNLIKELY (static_cast<unsigned>(has_x) + static_cast<unsigned>(has_y) + static_cast<unsigned>(has_z) < 2U) {
    MLOG_W("Foxglove PointCloud requires at least two coordinate fields from x, y, and z");
    return result;
  }

  if VUNLIKELY (has_compressed_xyz && !leading_xyz) {
    MLOG_W("Compressed PointCloud requires x, y, and z as its first three fields");
    return result;
  }

  thread_local flatbuffers::FlatBufferBuilder builder(1024 * 1024);
  builder.Clear();

  auto frame_id = builder.CreateString(frame_id_value.data(), frame_id_value.size());
  std::vector<flatbuffers::Offset<::foxglove::PackedElementField>> field_offsets;
  struct PointCloudFieldCopy {
    const PointCloudFieldView* source{nullptr};
    uint32_t dst_offset{0};
    ::foxglove::NumericType type{::foxglove::NumericType::UNKNOWN};
  };
  std::vector<PointCloudFieldCopy> field_copies;

  if (has_compressed_xyz) {
    field_copies.reserve(fields.size());
  }

  uint32_t field_offset = 0;

  for (size_t field_index = 0; field_index < fields.size(); ++field_index) {
    const auto& field = fields[field_index];
    auto name = builder.CreateString(field.field.name);

    auto num_type = ::foxglove::NumericType::UNKNOWN;
    uint16_t dst_size = field.field.storage_size;

    switch (field.field.native_type) {
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
      case zerocopy::PointCloud::kInt64Type:
      case zerocopy::PointCloud::kUint64Type:
        MLOG_W("Foxglove PointCloud does not support 64-bit integer field '{}'", field.field.name);
        return result;
      case zerocopy::PointCloud::kFloatType:
        num_type = ::foxglove::NumericType::FLOAT32;
        break;
      case zerocopy::PointCloud::kDoubleType:
        num_type = ::foxglove::NumericType::FLOAT64;
        break;
      default:
        switch (field.field.storage_size) {
          case 1:
            num_type = ::foxglove::NumericType::UINT8;
            break;
          case 2:
            num_type = ::foxglove::NumericType::INT16;
            break;
          case 4:
            num_type = ::foxglove::NumericType::FLOAT32;
            break;
          case 8:
            num_type = ::foxglove::NumericType::FLOAT64;
            break;
          default:
            MLOG_W("PointCloud field '{}': type_num not set, inferred type={} from size={}", field.field.name,
                   static_cast<uint8_t>(num_type), field.field.storage_size);
            break;
        }
        break;
    }

    if (has_compressed_xyz && field_index < 3) {
      num_type = ::foxglove::NumericType::FLOAT32;
      dst_size = sizeof(float);
    }

    field_offsets.emplace_back(::foxglove::CreatePackedElementField(builder, name, field_offset, num_type));

    if (has_compressed_xyz) {
      field_copies.push_back({&field, field_offset, num_type});
    }

    field_offset += dst_size;
  }

  uint32_t point_stride = has_compressed_xyz ? field_offset : static_cast<uint32_t>(pack_size);
  auto fields_vec = builder.CreateVector(field_offsets);

  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> data_vec;

  if VLIKELY (!view.data().empty()) {
    if (has_compressed_xyz) {
      if VUNLIKELY (point_stride != 0 && point_count > std::numeric_limits<size_t>::max() / point_stride) {
        MLOG_W("Compressed PointCloud output size overflow: size={} stride={}", point_count, point_stride);
        return result;
      }

      uint8_t* out_data = nullptr;
      data_vec = builder.CreateUninitializedVector<uint8_t>(point_count * point_stride, &out_data);

      for (size_t i = 0; i < point_count; ++i) {
        auto* dst = out_data + (i * point_stride);

        for (const auto& field : field_copies) {
          if VUNLIKELY (!write_point_field(view, i, *field.source, field.type, dst + field.dst_offset)) {
            MLOG_W("PointCloud field '{}' cannot be converted for Foxglove", field.source->field.name);
            return result;
          }
        }
      }
    } else {
      data_vec = builder.CreateVector(view.data().data(), data_size);
    }
  } else {
    data_vec = builder.CreateVector(static_cast<const uint8_t*>(nullptr), 0);
  }

  auto ts = make_timestamp_from_ns(time_meas);

  auto msg = ::foxglove::CreatePointCloud(builder, &ts, frame_id, 0, point_stride, fields_vec, data_vec);
  builder.Finish(msg);

  result.payload = Bytes::shallow_copy(builder.GetBufferPointer(), builder.GetSize());
  result.success = true;
  result.schema_name = "foxglove.PointCloud";
  result.encoding = std::string(kFoxgloveFlatbufferEncoding);
  result.schema_encoding = std::string(kFoxgloveFlatbufferEncoding);

  return result;
}

FoxgloveMessage FoxgloveConverter::raw_data_to_log(const Bytes& raw) {
  FoxgloveMessage result;

  zerocopy::MessageParser parser;

  if VUNLIKELY (!parser.parse(zerocopy::MessageParser::Type::kRawData, raw)) {
    MLOG_W("Failed to deserialize RawData");
    return result;
  }

  zerocopy::MessageParser::Value size_value;
  zerocopy::MessageParser::Value time_value;

  if VUNLIKELY (!parser.value("size", size_value) || !parser.value("header.time_meas", time_value)) {
    MLOG_W("RawData metadata is incomplete");
    return result;
  }

  const auto data_size = std::get<uint64_t>(size_value);
  std::string message = "RawData (" + std::to_string(data_size) + " bytes)";

  thread_local flatbuffers::FlatBufferBuilder builder(4096);
  builder.Clear();

  auto ts = make_timestamp_from_ns(std::get<uint64_t>(time_value));
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

FoxgloveMessage FoxgloveConverter::occupancy_grid_fbs(const Bytes& raw) {
  FoxgloveMessage result;

  zerocopy::MessageParser parser;

  if VUNLIKELY (!parser.parse(zerocopy::MessageParser::Type::kOccupancyGrid, raw)) {
    MLOG_W("Failed to deserialize OccupancyGrid (raw={})", raw.size());
    return result;
  }

  uint64_t cell_size = 0;
  uint64_t width_value = 0;
  uint64_t height_value = 0;
  uint64_t cell_type = 0;
  uint64_t time_meas = 0;
  double origin_x = 0.0;
  double origin_y = 0.0;
  double origin_z = 0.0;
  double origin_yaw = 0.0;
  double resolution = 0.0;
  std::string frame_id_value;
  Bytes data;

  if VUNLIKELY (!read_parser_uint64(parser, "cell_size", cell_size) ||
                !read_parser_uint64(parser, "width", width_value) ||
                !read_parser_uint64(parser, "height", height_value) ||
                !read_parser_uint64(parser, "cell_type", cell_type) ||
                !read_parser_uint64(parser, "header.time_meas", time_meas) || !parser.numeric("origin_x", origin_x) ||
                !parser.numeric("origin_y", origin_y) || !parser.numeric("origin_z", origin_z) ||
                !parser.numeric("origin_yaw", origin_yaw) || !parser.numeric("resolution", resolution) ||
                !parser.text("header.frame_id", frame_id_value) || !read_parser_bytes(parser, "data", data)) {
    MLOG_W("OccupancyGrid metadata is incomplete");
    return result;
  }

  const auto cell_sz = static_cast<uint8_t>(cell_size);
  const auto width = static_cast<uint32_t>(width_value);
  const auto height = static_cast<uint32_t>(height_value);

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

  if VUNLIKELY (data_size != data.size()) {
    MLOG_W("OccupancyGrid payload size mismatch: expected={} actual={}", data_size, data.size());
    return result;
  }

  thread_local flatbuffers::FlatBufferBuilder builder(256 * 1024);
  builder.Clear();

  auto ts = make_timestamp_from_ns(time_meas);
  auto frame_id = builder.CreateString(frame_id_value);

  auto num_type = ::foxglove::NumericType::UNKNOWN;
  std::string field_name = "value";

  switch (static_cast<zerocopy::OccupancyGrid::CellType>(cell_type)) {
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

  if VLIKELY (!data.empty()) {
    data_vec = builder.CreateVector(data.data(), data_size);
  } else {
    data_vec = builder.CreateVector(static_cast<const uint8_t*>(nullptr), 0);
  }

  double half_yaw = origin_yaw * 0.5;
  double qz = std::sin(half_yaw);
  double qw = std::cos(half_yaw);

  auto position = ::foxglove::CreateVector3(builder, origin_x, origin_y, origin_z);
  auto orientation = ::foxglove::CreateQuaternion(builder, 0.0, 0.0, qz, qw);
  auto pose = ::foxglove::CreatePose(builder, position, orientation);
  auto cell_size_vec = ::foxglove::CreateVector2(builder, resolution, resolution);

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

  zerocopy::MessageParser parser;

  if VUNLIKELY (!parser.parse(zerocopy::MessageParser::Type::kTensor, raw)) {
    MLOG_W("Failed to deserialize Tensor (raw={})", raw.size());
    return result;
  }

  uint64_t dtype = 0;
  uint64_t device = 0;
  uint64_t num_elements = 0;
  uint64_t element_size = 0;
  uint64_t batch_size = 0;
  uint64_t time_meas = 0;
  double quant_scale = 0.0;
  double quant_zero_point = 0.0;
  std::string name;
  std::string model_id;
  std::string layout;
  Bytes data;

  if VUNLIKELY (!read_parser_uint64(parser, "dtype", dtype) || !read_parser_uint64(parser, "device", device) ||
                !read_parser_uint64(parser, "num_elements", num_elements) ||
                !read_parser_uint64(parser, "element_size", element_size) ||
                !read_parser_uint64(parser, "batch_size", batch_size) ||
                !read_parser_uint64(parser, "header.time_meas", time_meas) ||
                !parser.numeric("quant_scale", quant_scale) || !parser.numeric("quant_zero_point", quant_zero_point) ||
                !parser.text("name", name) || !parser.text("model_id", model_id) || !parser.text("layout", layout) ||
                !read_parser_bytes(parser, "data", data)) {
    MLOG_W("Tensor metadata is incomplete");
    return result;
  }

  std::string dtype_str(
      ::vlink::NameDetector::get_enum(static_cast<zerocopy::Tensor::DataType>(static_cast<uint8_t>(dtype))));
  std::string device_str(
      ::vlink::NameDetector::get_enum(static_cast<zerocopy::Tensor::Device>(static_cast<uint8_t>(device))));

  Json shape_arr = Json::array();
  const size_t rank = parser.collection_size("shape");

  for (size_t i = 0; i < rank; ++i) {
    double dimension = 0.0;

    if VUNLIKELY (!parser.numeric("shape", i, "value", dimension)) {
      return result;
    }

    shape_arr.push_back(static_cast<uint64_t>(dimension));
  }

  Json strides_arr = Json::array();

  for (size_t i = 0; i < rank; ++i) {
    double stride = 0.0;

    if VUNLIKELY (!parser.numeric("strides", i, "value", stride)) {
      return result;
    }

    strides_arr.push_back(static_cast<uint64_t>(stride));
  }

  Json info;
  info["name"] = name;
  info["model_id"] = model_id;
  info["layout"] = layout;
  info["dtype"] = dtype_str;
  info["device"] = device_str;
  info["rank"] = rank;
  info["shape"] = std::move(shape_arr);
  info["strides"] = std::move(strides_arr);
  info["num_elements"] = num_elements;
  info["element_size"] = element_size;
  info["data_bytes"] = data.size();
  info["batch_size"] = batch_size;
  info["quant_scale"] = quant_scale;
  info["quant_zero_point"] = static_cast<int64_t>(quant_zero_point);

  std::string message = info.dump();

  thread_local flatbuffers::FlatBufferBuilder builder(8192);
  builder.Clear();

  auto ts = make_timestamp_from_ns(time_meas);
  auto msg_str = builder.CreateString(message);

  std::string name_label = name.empty() ? std::string("Tensor") : name;
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

  zerocopy::MessageParser parser;

  if VUNLIKELY (!parser.parse(zerocopy::MessageParser::Type::kObjectArray, raw)) {
    MLOG_W("Failed to deserialize ObjectArray (raw={})", raw.size());
    return result;
  }

  const size_t count = parser.collection_size("objects");

  thread_local flatbuffers::FlatBufferBuilder builder(64 * 1024);
  builder.Clear();

  uint64_t time_meas = 0;
  std::string frame_id;
  std::string source_id_str;

  if VUNLIKELY (!read_parser_uint64(parser, "header.time_meas", time_meas) ||
                !parser.text("header.frame_id", frame_id) || !parser.text("source_id", source_id_str)) {
    MLOG_W("ObjectArray metadata is incomplete");
    return result;
  }

  auto ts = make_timestamp_from_ns(time_meas);
  auto entity_fid = builder.CreateString(frame_id);

  if (source_id_str.empty()) {
    source_id_str = "object_array";
  }

  auto entity_id = builder.CreateString(source_id_str);

  std::vector<flatbuffers::Offset<::foxglove::CubePrimitive>> cubes_vec_data;
  cubes_vec_data.reserve(count);

  static constexpr double kPalette[][4] = {
      {0.5, 0.5, 0.5, 0.8}, {0.2, 0.6, 1.0, 0.8}, {0.2, 0.9, 0.2, 0.8}, {1.0, 0.8, 0.0, 0.8}, {0.8, 0.2, 0.8, 0.8},
  };

  for (size_t i = 0; i < count; ++i) {
    double yaw = 0.0;
    double position_x = 0.0;
    double position_y = 0.0;
    double position_z = 0.0;
    double size_x = 0.0;
    double size_y = 0.0;
    double size_z = 0.0;
    double motion_state = 0.0;
    double class_id = 0.0;

    if VUNLIKELY (!parser.numeric("objects", i, "yaw", yaw) ||
                  !parser.numeric("objects", i, "position[0]", position_x) ||
                  !parser.numeric("objects", i, "position[1]", position_y) ||
                  !parser.numeric("objects", i, "position[2]", position_z) ||
                  !parser.numeric("objects", i, "size[0]", size_x) ||
                  !parser.numeric("objects", i, "size[1]", size_y) ||
                  !parser.numeric("objects", i, "size[2]", size_z) ||
                  !parser.numeric("objects", i, "motion_state", motion_state) ||
                  !parser.numeric("objects", i, "class_id", class_id)) {
      continue;
    }

    double half_yaw = yaw * 0.5;
    double qz = std::sin(half_yaw);
    double qw = std::cos(half_yaw);

    auto position = ::foxglove::CreateVector3(builder, position_x, position_y, position_z);
    auto orientation = ::foxglove::CreateQuaternion(builder, 0.0, 0.0, qz, qw);
    auto pose = ::foxglove::CreatePose(builder, position, orientation);

    auto sx = size_x;
    auto sy = size_y;
    auto sz = size_z;

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

    auto palette_idx = static_cast<size_t>(motion_state);

    if (palette_idx >= (sizeof(kPalette) / sizeof(kPalette[0]))) {
      auto hue = std::fmod(class_id, 6.0);
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

  zerocopy::MessageParser parser;

  if VUNLIKELY (!parser.parse(zerocopy::MessageParser::Type::kAudioFrame, raw)) {
    MLOG_W("Failed to deserialize AudioFrame (raw={})", raw.size());
    return result;
  }

  uint64_t format = 0;
  uint64_t layout = 0;
  uint64_t sample_rate_value = 0;
  uint64_t num_samples_value = 0;
  uint64_t num_channels_value = 0;
  uint64_t time_meas = 0;
  Bytes data;

  if VUNLIKELY (!read_parser_uint64(parser, "format", format) || !read_parser_uint64(parser, "layout", layout) ||
                !read_parser_uint64(parser, "sample_rate", sample_rate_value) ||
                !read_parser_uint64(parser, "num_samples", num_samples_value) ||
                !read_parser_uint64(parser, "num_channels", num_channels_value) ||
                !read_parser_uint64(parser, "header.time_meas", time_meas) ||
                !read_parser_bytes(parser, "data", data)) {
    MLOG_W("AudioFrame metadata is incomplete");
    return result;
  }

  if VUNLIKELY (format != zerocopy::AudioFrame::kFormatPcmS16 || layout != zerocopy::AudioFrame::kLayoutInterleaved) {
    MLOG_W("Foxglove RawAudio supports only interleaved pcm-s16 AudioFrame messages");
    return result;
  }

  const auto sample_rate = static_cast<uint32_t>(sample_rate_value);
  const auto num_samples = static_cast<uint32_t>(num_samples_value);
  const auto num_channels = static_cast<uint16_t>(num_channels_value);

  if VUNLIKELY (data.empty() || sample_rate == 0 || num_samples == 0 || num_channels == 0) {
    MLOG_W("AudioFrame invalid for RawAudio: size={} sample_rate={} samples={} channels={}", data.size(), sample_rate,
           num_samples, num_channels);
    return result;
  }

  const size_t channel_sample_size = static_cast<size_t>(num_channels) * sizeof(int16_t);

  if VUNLIKELY (channel_sample_size == 0 || num_samples > std::numeric_limits<size_t>::max() / channel_sample_size) {
    MLOG_W("AudioFrame RawAudio size overflow: samples={} channels={}", num_samples, num_channels);
    return result;
  }

  const size_t expected_size = static_cast<size_t>(num_samples) * channel_sample_size;

  if VUNLIKELY (data.size() != expected_size) {
    MLOG_W("AudioFrame RawAudio size mismatch: actual={} expected={}", data.size(), expected_size);
    return result;
  }

  thread_local flatbuffers::FlatBufferBuilder builder(64 * 1024);
  builder.Clear();

  auto ts = make_timestamp_from_ns(time_meas);
  auto fmt = builder.CreateString("pcm-s16");

  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> data_vec;

  if VLIKELY (!data.empty()) {
    data_vec = builder.CreateVector(data.data(), data.size());
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

FoxgloveMessage FoxgloveConverter::convert_proto_mapping(const FoxgloveMapping& mapping, const std::string& ser,
                                                         const Bytes& raw) {
  auto msg = deserialize_proto_message(ser, raw);

  if VUNLIKELY (!msg) {
    FoxgloveMessage result;
    return result;
  }

  return convert_proto_mapping(mapping, *msg);
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
FoxgloveMessage FoxgloveConverter::convert_proto_mapping(const FoxgloveMapping& mapping,
                                                         const google::protobuf::Message& message) {
  const auto* msg = &message;

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
  const FieldMapping* entity_x_fm = nullptr;
  const FieldMapping* entity_y_fm = nullptr;
  const FieldMapping* entity_z_fm = nullptr;
  const FieldMapping* entity_w_fm = nullptr;
  const FieldMapping* entity_l_fm = nullptr;
  const FieldMapping* entity_h_fm = nullptr;
  const FieldMapping* entity_heading_fm = nullptr;

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
      entity_x_fm = &fm;
    } else if (fm.target == "entity_y") {
      entity_y_fm = &fm;
    } else if (fm.target == "entity_z") {
      entity_z_fm = &fm;
    } else if (fm.target == "entity_width") {
      entity_w_fm = &fm;
    } else if (fm.target == "entity_length") {
      entity_l_fm = &fm;
    } else if (fm.target == "entity_height") {
      entity_h_fm = &fm;
    } else if (fm.target == "entity_heading") {
      entity_heading_fm = &fm;
    }
  }

  bool has_entity_fields = entity_x_fm != nullptr || entity_y_fm != nullptr || entity_z_fm != nullptr ||
                           entity_w_fm != nullptr || entity_l_fm != nullptr || entity_h_fm != nullptr ||
                           entity_heading_fm != nullptr;

  auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);

  std::vector<flatbuffers::Offset<::foxglove::SceneEntity>> entity_offsets;

  auto build_cube = [&entity_h_fm, &entity_heading_fm, &entity_l_fm, &entity_offsets, &entity_w_fm, &entity_x_fm,
                     &entity_y_fm, &entity_z_fm, &frame_id, &has_entity_fields,
                     &ts](const google::protobuf::Message& sub, int idx, const std::string& parent_id) {
    double px = 0.0;
    double py = 0.0;
    double pz = 0.0;
    double sx = 1.0;
    double sy = 1.0;
    double sz = 1.0;
    double heading = 0.0;

    if (has_entity_fields) {
      if (entity_x_fm) {
        px = get_proto_double(sub, entity_x_fm->source, *entity_x_fm);
      }

      if (entity_y_fm) {
        py = get_proto_double(sub, entity_y_fm->source, *entity_y_fm);
      }

      if (entity_z_fm) {
        pz = get_proto_double(sub, entity_z_fm->source, *entity_z_fm);
      }

      if (entity_w_fm) {
        auto v = get_proto_double(sub, entity_w_fm->source, *entity_w_fm);

        if (v != 0.0) {
          sx = v;
        }
      }

      if (entity_l_fm) {
        auto v = get_proto_double(sub, entity_l_fm->source, *entity_l_fm);

        if (v != 0.0) {
          sy = v;
        }
      }

      if (entity_h_fm) {
        auto v = get_proto_double(sub, entity_h_fm->source, *entity_h_fm);

        if (v != 0.0) {
          sz = v;
        }
      }

      if (entity_heading_fm) {
        heading = get_proto_double(sub, entity_heading_fm->source, *entity_heading_fm);
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

    if VUNLIKELY (!field || !field->is_repeated() ||
                  field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      continue;
    }

    int count = ref->FieldSize(*entities_parent, field);

    for (int i = 0; i < count; ++i) {
      const auto& item = ref->GetRepeatedMessage(*entities_parent, field, i);

      if (!entity_sub_items.empty()) {
        const auto* item_desc = item.GetDescriptor();
        const auto* item_ref = item.GetReflection();
        const auto* sub_field = find_proto_field_cached(*item_desc, entity_sub_items);

        if (sub_field && sub_field->is_repeated() &&
            sub_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
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
  const FieldMapping* pose_px_fm = nullptr;
  const FieldMapping* pose_py_fm = nullptr;
  const FieldMapping* pose_pz_fm = nullptr;
  const FieldMapping* pose_qx_fm = nullptr;
  const FieldMapping* pose_qy_fm = nullptr;
  const FieldMapping* pose_qz_fm = nullptr;
  const FieldMapping* pose_qw_fm = nullptr;

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
      pose_px_fm = &fm;
    } else if (fm.target == "pose_position_y") {
      pose_py_fm = &fm;
    } else if (fm.target == "pose_position_z") {
      pose_pz_fm = &fm;
    } else if (fm.target == "pose_orientation_x") {
      pose_qx_fm = &fm;
    } else if (fm.target == "pose_orientation_y") {
      pose_qy_fm = &fm;
    } else if (fm.target == "pose_orientation_z") {
      pose_qz_fm = &fm;
    } else if (fm.target == "pose_orientation_w") {
      pose_qw_fm = &fm;
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

      if (field && field->is_repeated() && field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
        int count = ref->FieldSize(*poses_parent, field);

        for (int i = 0; i < count; ++i) {
          const auto& item = ref->GetRepeatedMessage(*poses_parent, field, i);

          double px = pose_px_fm ? get_proto_double(item, pose_px_fm->source, *pose_px_fm) : 0.0;
          double py = pose_py_fm ? get_proto_double(item, pose_py_fm->source, *pose_py_fm) : 0.0;
          double pz = pose_pz_fm ? get_proto_double(item, pose_pz_fm->source, *pose_pz_fm) : 0.0;
          double qx = pose_qx_fm ? get_proto_double(item, pose_qx_fm->source, *pose_qx_fm) : 0.0;
          double qy = pose_qy_fm ? get_proto_double(item, pose_qy_fm->source, *pose_qy_fm) : 0.0;
          double qz = pose_qz_fm ? get_proto_double(item, pose_qz_fm->source, *pose_qz_fm) : 0.0;
          double qw = pose_qw_fm ? get_proto_double(item, pose_qw_fm->source, *pose_qw_fm) : 1.0;

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
  const FieldMapping* ft_ts_fm = nullptr;
  const FieldMapping* ft_ts_ns_fm = nullptr;
  const FieldMapping* ft_parent_fm = nullptr;
  const FieldMapping* ft_child_fm = nullptr;
  const FieldMapping* ft_tx_fm = nullptr;
  const FieldMapping* ft_ty_fm = nullptr;
  const FieldMapping* ft_tz_fm = nullptr;
  const FieldMapping* ft_qx_fm = nullptr;
  const FieldMapping* ft_qy_fm = nullptr;
  const FieldMapping* ft_qz_fm = nullptr;
  const FieldMapping* ft_qw_fm = nullptr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "transforms") {
      transforms_src = fm.source;
    } else if (fm.target == "transform_timestamp") {
      ft_ts_fm = &fm;
    } else if (fm.target == "transform_timestamp_ns") {
      ft_ts_ns_fm = &fm;
    } else if (fm.target == "transform_parent_frame_id") {
      ft_parent_fm = &fm;
    } else if (fm.target == "transform_child_frame_id") {
      ft_child_fm = &fm;
    } else if (fm.target == "transform_translation_x") {
      ft_tx_fm = &fm;
    } else if (fm.target == "transform_translation_y") {
      ft_ty_fm = &fm;
    } else if (fm.target == "transform_translation_z") {
      ft_tz_fm = &fm;
    } else if (fm.target == "transform_rotation_x") {
      ft_qx_fm = &fm;
    } else if (fm.target == "transform_rotation_y") {
      ft_qy_fm = &fm;
    } else if (fm.target == "transform_rotation_z") {
      ft_qz_fm = &fm;
    } else if (fm.target == "transform_rotation_w") {
      ft_qw_fm = &fm;
    }
  }

  std::vector<flatbuffers::Offset<::foxglove::FrameTransform>> transform_offsets;

  if (!transforms_src.empty()) {
    const auto* desc = msg.GetDescriptor();
    const auto* ref = msg.GetReflection();
    const auto* field = find_proto_field_cached(*desc, transforms_src);

    if (field && field->is_repeated() && field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      int count = ref->FieldSize(msg, field);

      for (int i = 0; i < count; ++i) {
        const auto& item = ref->GetRepeatedMessage(msg, field, i);

        auto item_ts_us =
            ft_ts_fm ? checked_unsigned_cast<uint64_t>(get_proto_double(item, ft_ts_fm->source, *ft_ts_fm)) : 0;
        auto item_ts_ns =
            ft_ts_ns_fm ? checked_unsigned_cast<uint64_t>(get_proto_double(item, ft_ts_ns_fm->source, *ft_ts_ns_fm))
                        : 0;
        std::string parent_fid =
            ft_parent_fm ? get_proto_string(item, ft_parent_fm->source, *ft_parent_fm) : std::string{};
        std::string child_fid = ft_child_fm ? get_proto_string(item, ft_child_fm->source, *ft_child_fm) : std::string{};
        double itx = ft_tx_fm ? get_proto_double(item, ft_tx_fm->source, *ft_tx_fm) : 0.0;
        double ity = ft_ty_fm ? get_proto_double(item, ft_ty_fm->source, *ft_ty_fm) : 0.0;
        double itz = ft_tz_fm ? get_proto_double(item, ft_tz_fm->source, *ft_tz_fm) : 0.0;
        double iqx = ft_qx_fm ? get_proto_double(item, ft_qx_fm->source, *ft_qx_fm) : 0.0;
        double iqy = ft_qy_fm ? get_proto_double(item, ft_qy_fm->source, *ft_qy_fm) : 0.0;
        double iqz = ft_qz_fm ? get_proto_double(item, ft_qz_fm->source, *ft_qz_fm) : 0.0;
        double iqw = ft_qw_fm ? get_proto_double(item, ft_qw_fm->source, *ft_qw_fm) : 1.0;

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
  const FieldMapping* fix_ts_fm = nullptr;
  const FieldMapping* fix_ts_ns_fm = nullptr;
  const FieldMapping* fix_frame_id_fm = nullptr;
  const FieldMapping* fix_lat_fm = nullptr;
  const FieldMapping* fix_lon_fm = nullptr;
  const FieldMapping* fix_alt_fm = nullptr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "fixes") {
      fixes_src = fm.source;
    } else if (fm.target == "fix_timestamp") {
      fix_ts_fm = &fm;
    } else if (fm.target == "fix_timestamp_ns") {
      fix_ts_ns_fm = &fm;
    } else if (fm.target == "fix_frame_id") {
      fix_frame_id_fm = &fm;
    } else if (fm.target == "fix_latitude") {
      fix_lat_fm = &fm;
    } else if (fm.target == "fix_longitude") {
      fix_lon_fm = &fm;
    } else if (fm.target == "fix_altitude") {
      fix_alt_fm = &fm;
    }
  }

  std::vector<flatbuffers::Offset<::foxglove::LocationFix>> fix_offsets;

  if (!fixes_src.empty()) {
    const auto* desc = msg.GetDescriptor();
    const auto* ref = msg.GetReflection();
    const auto* field = find_proto_field_cached(*desc, fixes_src);

    if (field && field->is_repeated() && field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      int count = ref->FieldSize(msg, field);

      for (int i = 0; i < count; ++i) {
        const auto& item = ref->GetRepeatedMessage(msg, field, i);

        auto item_ts_us =
            fix_ts_fm ? checked_unsigned_cast<uint64_t>(get_proto_double(item, fix_ts_fm->source, *fix_ts_fm)) : 0;
        auto item_ts_ns =
            fix_ts_ns_fm ? checked_unsigned_cast<uint64_t>(get_proto_double(item, fix_ts_ns_fm->source, *fix_ts_ns_fm))
                         : 0;
        std::string item_frame_id =
            fix_frame_id_fm ? get_proto_string(item, fix_frame_id_fm->source, *fix_frame_id_fm) : std::string{};
        double lat = fix_lat_fm ? get_proto_double(item, fix_lat_fm->source, *fix_lat_fm) : 0.0;
        double lon = fix_lon_fm ? get_proto_double(item, fix_lon_fm->source, *fix_lon_fm) : 0.0;
        double alt = fix_alt_fm ? get_proto_double(item, fix_alt_fm->source, *fix_alt_fm) : 0.0;

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

    if (field && field->is_repeated() && field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
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

    if VUNLIKELY (!field || !field->is_repeated() ||
                  field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
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
  const FieldMapping* joint_name_fm = nullptr;
  const FieldMapping* joint_position_fm = nullptr;
  const FieldMapping* joint_velocity_fm = nullptr;
  const FieldMapping* joint_effort_fm = nullptr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "timestamp") {
      timestamp_us = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "timestamp_ns") {
      timestamp_ns = checked_unsigned_cast<uint64_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "joints") {
      joints_src = fm.source;
    } else if (fm.target == "joint_name") {
      joint_name_fm = &fm;
    } else if (fm.target == "joint_position") {
      joint_position_fm = &fm;
    } else if (fm.target == "joint_velocity") {
      joint_velocity_fm = &fm;
    } else if (fm.target == "joint_effort") {
      joint_effort_fm = &fm;
    }
  }

  std::vector<flatbuffers::Offset<::foxglove::JointState>> joint_offsets;

  if (!joints_src.empty()) {
    const auto* desc = msg.GetDescriptor();
    const auto* ref = msg.GetReflection();
    const auto* field = find_proto_field_cached(*desc, joints_src);

    if (field && field->is_repeated() && field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      int count = ref->FieldSize(msg, field);

      for (int i = 0; i < count; ++i) {
        const auto& item = ref->GetRepeatedMessage(msg, field, i);

        FieldMapping empty_fm;
        auto jname = joint_name_fm ? get_proto_string(item, joint_name_fm->source, *joint_name_fm)
                                   : get_proto_string(item, "name", empty_fm);
        double jpos = joint_position_fm ? get_proto_double(item, joint_position_fm->source, *joint_position_fm)
                                        : get_proto_double(item, "position", empty_fm);
        double jvel = joint_velocity_fm ? get_proto_double(item, joint_velocity_fm->source, *joint_velocity_fm)
                                        : get_proto_double(item, "velocity", empty_fm);
        double jeff = joint_effort_fm ? get_proto_double(item, joint_effort_fm->source, *joint_effort_fm)
                                      : get_proto_double(item, "effort", empty_fm);

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

    if (field && field->is_repeated() && field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
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
    {"foxglove.CompressedAudio",    {::foxglove::CompressedAudioBinarySchema::data(),     ::foxglove::CompressedAudioBinarySchema::size()}},
    {"foxglove.CompressedImage",    {::foxglove::CompressedImageBinarySchema::data(),     ::foxglove::CompressedImageBinarySchema::size()}},
    {"foxglove.CompressedPointCloud", {::foxglove::CompressedPointCloudBinarySchema::data(), ::foxglove::CompressedPointCloudBinarySchema::size()}},
    {"foxglove.CompressedVideo",    {::foxglove::CompressedVideoBinarySchema::data(),     ::foxglove::CompressedVideoBinarySchema::size()}},
    {"foxglove.Event",              {::foxglove::EventBinarySchema::data(),               ::foxglove::EventBinarySchema::size()}},
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
    {"foxglove.Odometry",           {::foxglove::OdometryBinarySchema::data(),            ::foxglove::OdometryBinarySchema::size()}},
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

#ifdef VLINK_HAS_FBS_COMPILER
bool FoxgloveConverter::resolve_custom_fbs_schema(const std::string& fbs_ser, std::string& schema_data) {
  std::lock_guard lock(mtx_);

  auto cache_key = "custom_fbs:" + fbs_ser;
  auto cache_iter = schema_cache_.find(cache_key);

  if VLIKELY (cache_iter != schema_cache_.end()) {
    schema_data = cache_iter->second;
    return true;
  }

  if VLIKELY (schema_interface_) {
    auto schema = schema_interface_->search_schema(fbs_ser, SchemaType::kFlatbuffers);

    if VLIKELY (schema.schema_type == SchemaType::kFlatbuffers && !schema.data.empty()) {
      schema_data.assign(reinterpret_cast<const char*>(schema.data.data()), schema.data.size());
      schema_cache_[cache_key] = schema_data;
      return true;
    }
  }

  if VUNLIKELY (!find_fbs_parser_locked(fbs_ser)) {
    return false;
  }

  auto parser_iter = fbs_parsers_.find(fbs_ser);

  if VUNLIKELY (parser_iter == fbs_parsers_.end() || parser_iter->second >= fbs_parser_vec_.size() ||
                !fbs_parser_vec_[parser_iter->second]) {
    return false;
  }

  auto& parser = *fbs_parser_vec_[parser_iter->second];

  if VUNLIKELY (!parser.SetRootType(fbs_ser.c_str())) {
    return false;
  }

  parser.Serialize();
  const auto* buf_ptr = parser.builder_.GetBufferPointer();
  auto buf_size = parser.builder_.GetSize();

  if VUNLIKELY (!buf_ptr || buf_size == 0) {
    MLOG_W("Failed to serialize BFBS for: {}", fbs_ser);
    return false;
  }

  schema_data.assign(reinterpret_cast<const char*>(buf_ptr), buf_size);
  schema_cache_[cache_key] = schema_data;
  return true;
}

#endif

const FoxgloveMapping* FoxgloveConverter::find_mapping(std::string_view url, const std::string& ser,
                                                       bool* ambiguous) const {
  struct MappingCache final {
    uint64_t owner_id{0};
    std::string url;
    std::string ser;
    const FoxgloveMapping* mapping{nullptr};
    bool ambiguous{false};
  };

  thread_local MappingCache cache;

  if (cache.owner_id == cache_owner_id_ && cache.url == url && cache.ser == ser) {
    if (ambiguous) {
      *ambiguous = cache.ambiguous;
    }

    return cache.ambiguous ? nullptr : cache.mapping;
  }

  auto mapping_iter = mapping_index_.find(ser);

  if VUNLIKELY (mapping_iter == mapping_index_.end()) {
    cache.owner_id = cache_owner_id_;
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

  cache.owner_id = cache_owner_id_;
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

// NOLINTNEXTLINE(google-readability-function-size)
FoxgloveMessage FoxgloveConverter::convert_fbs_mapping(const FoxgloveMapping& mapping, const std::string& ser,
                                                       const Bytes& raw) {
  const reflection::Schema* schema = nullptr;

  if VUNLIKELY (!resolve_thread_local_fbs_schema(
                    ser, cache_owner_id_,
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

  auto fbs_get_mapped_double = [schema](const flatbuffers::Table& tbl, const reflection::Object& o,
                                        const FieldMapping& fm) -> double {
    if VUNLIKELY (!fm.expression.empty()) {
      return evaluate_expression_with_fbs(fm.expression, tbl, o, *schema);
    }

    if VUNLIKELY (has_nested_field_path(fm.source)) {
      const auto value = resolve_nested_fbs_double(tbl, o, *schema, fm.source, true);

      if VLIKELY (!std::isnan(value)) {
        return value;
      }
    }

    return get_fbs_double(tbl, o, fm.source, fm);
  };

  auto fbs_get_mapped_string = [schema](const flatbuffers::Table& tbl, const reflection::Object& o,
                                        const FieldMapping& fm) -> std::string {
    if VUNLIKELY (!fm.expression.empty()) {
      return format_expression_string(evaluate_expression_with_fbs(fm.expression, tbl, o, *schema));
    }

    if VUNLIKELY (has_nested_field_path(fm.source)) {
      bool found = false;
      auto value = resolve_nested_fbs_string(tbl, o, *schema, fm.source, &found, true);

      if VLIKELY (found) {
        return value;
      }
    }

    return get_fbs_string(tbl, o, fm.source, fm, schema);
  };

  auto fbs_get_object_mapped_double = [schema](const FbsObjectView& view, const FieldMapping& fm) -> double {
    if VUNLIKELY (!fm.expression.empty()) {
      return evaluate_expression_with_fbs(fm.expression, view, *schema);
    }

    if VUNLIKELY (has_nested_field_path(fm.source)) {
      const auto value = resolve_nested_fbs_double(view, *schema, fm.source, true);

      if VLIKELY (!std::isnan(value)) {
        return value;
      }
    }

    return get_fbs_double(view, fm.source, fm);
  };

  auto fbs_get_object_mapped_string = [schema](const FbsObjectView& view, const FieldMapping& fm) -> std::string {
    if VUNLIKELY (!fm.expression.empty()) {
      return format_expression_string(evaluate_expression_with_fbs(fm.expression, view, *schema));
    }

    if VUNLIKELY (has_nested_field_path(fm.source)) {
      bool found = false;
      auto value = resolve_nested_fbs_string(view, *schema, fm.source, &found, true);

      if VLIKELY (found) {
        return value;
      }
    }

    return get_fbs_string(view, fm.source, fm, schema);
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
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "frame_id") {
        frame_id = fbs_get_mapped_string(*root_table, obj, fm);
      } else if (fm.target == "latitude") {
        latitude = fbs_get_mapped_double(*root_table, obj, fm);
      } else if (fm.target == "longitude") {
        longitude = fbs_get_mapped_double(*root_table, obj, fm);
      } else if (fm.target == "altitude") {
        altitude = fbs_get_mapped_double(*root_table, obj, fm);
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
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "frame_id") {
        frame_id = fbs_get_mapped_string(*root_table, obj, fm);
      } else if (fm.target == "pose" || fm.target == "pose_euler") {
        FbsObjectView target_parent;
        std::string target_field_name;

        if (resolve_fbs_parent_field_path(*root_table, obj, *schema, fm.source, target_parent, target_field_name)) {
          const auto* target_field = find_fbs_field(*target_parent.object, target_field_name);

          if (target_field && target_field->type()->base_type() == reflection::Obj) {
            const auto* sub_obj = find_fbs_object(*schema, target_field->type()->index());
            FbsObjectView child;

            if (sub_obj && resolve_fbs_object_field(target_parent, *target_field, *sub_obj, child)) {
              FieldMapping empty_fm;

              if (fm.target == "pose") {
                qx = get_fbs_double(child, "x", empty_fm);
                qy = get_fbs_double(child, "y", empty_fm);
                qz = get_fbs_double(child, "z", empty_fm);
                qw = get_fbs_double(child, "w", empty_fm);
              } else {
                euler_roll = get_fbs_double(child, "x", empty_fm);
                euler_pitch = get_fbs_double(child, "y", empty_fm);
                euler_yaw = get_fbs_double(child, "z", empty_fm);
                has_euler = true;
              }
            }
          }
        }
      } else if (fm.target == "position_x") {
        position_x = fbs_get_mapped_double(*root_table, obj, fm);
      } else if (fm.target == "position_y") {
        position_y = fbs_get_mapped_double(*root_table, obj, fm);
      } else if (fm.target == "position_z") {
        position_z = fbs_get_mapped_double(*root_table, obj, fm);
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
    const FieldMapping* entity_x_fm = nullptr;
    const FieldMapping* entity_y_fm = nullptr;
    const FieldMapping* entity_z_fm = nullptr;
    const FieldMapping* entity_w_fm = nullptr;
    const FieldMapping* entity_l_fm = nullptr;
    const FieldMapping* entity_h_fm = nullptr;
    const FieldMapping* entity_heading_fm = nullptr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "timestamp") {
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "frame_id") {
        frame_id = fbs_get_mapped_string(*root_table, obj, fm);
      } else if (fm.target == "entity_sub_items") {
        entity_sub_items = fm.source;
      } else if (fm.target == "entity_x") {
        entity_x_fm = &fm;
      } else if (fm.target == "entity_y") {
        entity_y_fm = &fm;
      } else if (fm.target == "entity_z") {
        entity_z_fm = &fm;
      } else if (fm.target == "entity_width") {
        entity_w_fm = &fm;
      } else if (fm.target == "entity_length") {
        entity_l_fm = &fm;
      } else if (fm.target == "entity_height") {
        entity_h_fm = &fm;
      } else if (fm.target == "entity_heading") {
        entity_heading_fm = &fm;
      }
    }

    bool has_entity_fields = entity_x_fm != nullptr || entity_y_fm != nullptr || entity_z_fm != nullptr ||
                             entity_w_fm != nullptr || entity_l_fm != nullptr || entity_h_fm != nullptr ||
                             entity_heading_fm != nullptr;

    auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
    std::vector<flatbuffers::Offset<::foxglove::SceneEntity>> entity_offsets;

    auto build_fbs_cube = [&entity_h_fm, &entity_heading_fm, &entity_l_fm, &entity_offsets, &entity_w_fm, &entity_x_fm,
                           &entity_y_fm, &entity_z_fm, &fbs_get_object_mapped_double, &frame_id, &has_entity_fields,
                           &schema, &ts](const FbsObjectView& item, int idx, const std::string& parent_id) {
      double px = 0.0;
      double py = 0.0;
      double pz = 0.0;
      double sx = 1.0;
      double sy = 1.0;
      double sz = 1.0;
      double heading = 0.0;

      FieldMapping empty_fm;

      if (has_entity_fields) {
        if (entity_x_fm) {
          px = fbs_get_object_mapped_double(item, *entity_x_fm);
        }

        if (entity_y_fm) {
          py = fbs_get_object_mapped_double(item, *entity_y_fm);
        }

        if (entity_z_fm) {
          pz = fbs_get_object_mapped_double(item, *entity_z_fm);
        }

        if (entity_w_fm) {
          auto v = fbs_get_object_mapped_double(item, *entity_w_fm);

          if (v != 0.0) {
            sx = v;
          }
        }

        if (entity_l_fm) {
          auto v = fbs_get_object_mapped_double(item, *entity_l_fm);

          if (v != 0.0) {
            sy = v;
          }
        }

        if (entity_h_fm) {
          auto v = fbs_get_object_mapped_double(item, *entity_h_fm);

          if (v != 0.0) {
            sz = v;
          }
        }

        if (entity_heading_fm) {
          heading = fbs_get_object_mapped_double(item, *entity_heading_fm);
        }
      } else {
        px = get_fbs_double(item, "x", empty_fm);
        py = get_fbs_double(item, "y", empty_fm);
        pz = get_fbs_double(item, "z", empty_fm);

        if (px == 0.0 && py == 0.0 && pz == 0.0) {
          px = get_fbs_double(item, "cx", empty_fm);
          py = get_fbs_double(item, "cy", empty_fm);
          pz = get_fbs_double(item, "cz", empty_fm);
        }

        if (px == 0.0 && py == 0.0 && pz == 0.0) {
          const auto* pos_field = find_fbs_field(*item.object, "position");

          if (pos_field && pos_field->type()->base_type() == reflection::Obj) {
            const auto* pos_obj = find_fbs_object(*schema, pos_field->type()->index());
            FbsObjectView position;

            if (pos_obj && resolve_fbs_object_field(item, *pos_field, *pos_obj, position)) {
              px = get_fbs_double(position, "x", empty_fm);
              py = get_fbs_double(position, "y", empty_fm);
              pz = get_fbs_double(position, "z", empty_fm);
            }
          }
        }

        auto w_val = get_fbs_double(item, "width", empty_fm);
        auto l_val = get_fbs_double(item, "length", empty_fm);
        auto h_val = get_fbs_double(item, "height", empty_fm);

        if (w_val != 0.0) {
          sx = w_val;
        }

        if (l_val != 0.0) {
          sy = l_val;
        }

        if (h_val != 0.0) {
          sz = h_val;
        }

        heading = get_fbs_double(item, "heading_angle", empty_fm);

        if (heading == 0.0) {
          heading = get_fbs_double(item, "yaw", empty_fm);
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

      FbsObjectView entities_parent;
      std::string entities_field_name;

      if VUNLIKELY (!resolve_fbs_parent_field_path(*root_table, obj, *schema, fm.source, entities_parent,
                                                   entities_field_name)) {
        continue;
      }

      const auto* vec_field = find_fbs_field(*entities_parent.object, entities_field_name);

      if VUNLIKELY (!vec_field || vec_field->type()->base_type() != reflection::Vector) {
        continue;
      }

      if (vec_field->type()->element() != reflection::Obj) {
        continue;
      }

      const auto* vec = get_fbs_vector(entities_parent, *vec_field);

      if VUNLIKELY (!vec) {
        continue;
      }

      const auto* sub_obj = find_fbs_object(*schema, vec_field->type()->index());

      if VUNLIKELY (!sub_obj) {
        continue;
      }

      for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
        FbsObjectView item;

        if VUNLIKELY (!resolve_fbs_vector_object_unchecked(*vec, i, *sub_obj, item)) {
          continue;
        }

        if (!entity_sub_items.empty()) {
          const auto* sub_field = find_fbs_field(*sub_obj, entity_sub_items);

          if (sub_field && sub_field->type()->base_type() == reflection::Vector &&
              sub_field->type()->element() == reflection::Obj && sub_field->type()->index() >= 0) {
            const auto* sub_vec = get_fbs_vector(item, *sub_field);

            if (sub_vec) {
              const auto* sub_sub_obj = find_fbs_object(*schema, sub_field->type()->index());

              if (sub_sub_obj) {
                for (flatbuffers::uoffset_t j = 0; j < sub_vec->size(); ++j) {
                  FbsObjectView sub_item;

                  if (resolve_fbs_vector_object_unchecked(*sub_vec, j, *sub_sub_obj, sub_item)) {
                    build_fbs_cube(sub_item, static_cast<int>(j), std::to_string(i));
                  }
                }
              }
            }
          }
        } else {
          build_fbs_cube(item, static_cast<int>(i), "e");
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
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "parent_frame_id") {
        parent_frame_id = fbs_get_mapped_string(*root_table, obj, fm);
      } else if (fm.target == "child_frame_id") {
        child_frame_id = fbs_get_mapped_string(*root_table, obj, fm);
      } else if (fm.target == "translation_x") {
        tx = fbs_get_mapped_double(*root_table, obj, fm);
      } else if (fm.target == "translation_y") {
        ty = fbs_get_mapped_double(*root_table, obj, fm);
      } else if (fm.target == "translation_z") {
        tz = fbs_get_mapped_double(*root_table, obj, fm);
      } else if (fm.target == "rotation_x") {
        qx = fbs_get_mapped_double(*root_table, obj, fm);
      } else if (fm.target == "rotation_y") {
        qy = fbs_get_mapped_double(*root_table, obj, fm);
      } else if (fm.target == "rotation_z") {
        qz = fbs_get_mapped_double(*root_table, obj, fm);
      } else if (fm.target == "rotation_w") {
        qw = fbs_get_mapped_double(*root_table, obj, fm);
      } else if (fm.target == "euler_roll") {
        euler_roll = fbs_get_mapped_double(*root_table, obj, fm);
        has_euler = true;
      } else if (fm.target == "euler_pitch") {
        euler_pitch = fbs_get_mapped_double(*root_table, obj, fm);
        has_euler = true;
      } else if (fm.target == "euler_yaw") {
        euler_yaw = fbs_get_mapped_double(*root_table, obj, fm);
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
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "level") {
        level_str = fbs_get_mapped_string(*root_table, obj, fm);
      } else if (fm.target == "message") {
        message = fbs_get_mapped_string(*root_table, obj, fm);
      } else if (fm.target == "name") {
        name = fbs_get_mapped_string(*root_table, obj, fm);
      } else if (fm.target == "file") {
        file = fbs_get_mapped_string(*root_table, obj, fm);
      } else if (fm.target == "line") {
        line = checked_unsigned_cast<uint32_t>(fbs_get_mapped_double(*root_table, obj, fm));
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
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "frame_id") {
        frame_id = fbs_get_mapped_string(*root_table, obj, fm);
      } else if (fm.target == "start_angle") {
        start_angle = fbs_get_mapped_double(*root_table, obj, fm);
      } else if (fm.target == "end_angle") {
        end_angle = fbs_get_mapped_double(*root_table, obj, fm);
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
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "frame_id") {
        frame_id = fbs_get_mapped_string(*root_table, obj, fm);
      } else if (fm.target == "width") {
        width = checked_unsigned_cast<uint32_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "height") {
        height = checked_unsigned_cast<uint32_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "encoding") {
        encoding = fbs_get_mapped_string(*root_table, obj, fm);
      } else if (fm.target == "step") {
        step = checked_unsigned_cast<uint32_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "data") {
        data_src = fm.source;
      }
    }

    flatbuffers::Offset<flatbuffers::Vector<uint8_t>> data_vec = 0;

    if (!data_src.empty()) {
      const auto* field = find_fbs_field(obj, data_src);

      if (field && is_fbs_byte_vector(*field)) {
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
        geojson = fbs_get_mapped_string(*root_table, obj, fm);
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
    const FieldMapping* pose_px_fm = nullptr;
    const FieldMapping* pose_py_fm = nullptr;
    const FieldMapping* pose_pz_fm = nullptr;
    const FieldMapping* pose_qx_fm = nullptr;
    const FieldMapping* pose_qy_fm = nullptr;
    const FieldMapping* pose_qz_fm = nullptr;
    const FieldMapping* pose_qw_fm = nullptr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "timestamp") {
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "frame_id") {
        frame_id = fbs_get_mapped_string(*root_table, obj, fm);
      } else if (fm.target == "poses") {
        poses_src = fm.source;
      } else if (fm.target == "pose_position_x") {
        pose_px_fm = &fm;
      } else if (fm.target == "pose_position_y") {
        pose_py_fm = &fm;
      } else if (fm.target == "pose_position_z") {
        pose_pz_fm = &fm;
      } else if (fm.target == "pose_orientation_x") {
        pose_qx_fm = &fm;
      } else if (fm.target == "pose_orientation_y") {
        pose_qy_fm = &fm;
      } else if (fm.target == "pose_orientation_z") {
        pose_qz_fm = &fm;
      } else if (fm.target == "pose_orientation_w") {
        pose_qw_fm = &fm;
      }
    }

    auto ts = (timestamp_ns > 0) ? make_timestamp_from_ns(timestamp_ns) : make_timestamp_from_us(timestamp_us);
    std::vector<flatbuffers::Offset<::foxglove::Pose>> pose_offsets;

    if (!poses_src.empty()) {
      FbsObjectView vec_parent;
      std::string vec_field_name;

      if (resolve_fbs_parent_field_path(*root_table, obj, *schema, poses_src, vec_parent, vec_field_name)) {
        const auto* vec_field = find_fbs_field(*vec_parent.object, vec_field_name);

        if (vec_field && vec_field->type()->base_type() == reflection::Vector &&
            vec_field->type()->element() == reflection::Obj) {
          const auto* vec = get_fbs_vector(vec_parent, *vec_field);
          const auto* sub_obj = find_fbs_object(*schema, vec_field->type()->index());

          if (vec && sub_obj) {
            for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
              FbsObjectView item;

              if VUNLIKELY (!resolve_fbs_vector_object_unchecked(*vec, i, *sub_obj, item)) {
                continue;
              }

              double px = 0.0;
              double py = 0.0;
              double pz = 0.0;
              double qx = 0.0;
              double qy = 0.0;
              double qz = 0.0;
              double qw = 1.0;

              if (pose_px_fm) {
                px = fbs_get_object_mapped_double(item, *pose_px_fm);
              }

              if (pose_py_fm) {
                py = fbs_get_object_mapped_double(item, *pose_py_fm);
              }

              if (pose_pz_fm) {
                pz = fbs_get_object_mapped_double(item, *pose_pz_fm);
              }

              if (pose_qx_fm) {
                qx = fbs_get_object_mapped_double(item, *pose_qx_fm);
              }

              if (pose_qy_fm) {
                qy = fbs_get_object_mapped_double(item, *pose_qy_fm);
              }

              if (pose_qz_fm) {
                qz = fbs_get_object_mapped_double(item, *pose_qz_fm);
              }

              if (pose_qw_fm) {
                qw = fbs_get_object_mapped_double(item, *pose_qw_fm);
              }

              auto position = ::foxglove::CreateVector3(builder, px, py, pz);
              auto orientation = ::foxglove::CreateQuaternion(builder, qx, qy, qz, qw);
              pose_offsets.emplace_back(::foxglove::CreatePose(builder, position, orientation));
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
    const FieldMapping* ft_ts_fm = nullptr;
    const FieldMapping* ft_ts_ns_fm = nullptr;
    const FieldMapping* ft_parent_fm = nullptr;
    const FieldMapping* ft_child_fm = nullptr;
    const FieldMapping* ft_tx_fm = nullptr;
    const FieldMapping* ft_ty_fm = nullptr;
    const FieldMapping* ft_tz_fm = nullptr;
    const FieldMapping* ft_qx_fm = nullptr;
    const FieldMapping* ft_qy_fm = nullptr;
    const FieldMapping* ft_qz_fm = nullptr;
    const FieldMapping* ft_qw_fm = nullptr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "transforms") {
        transforms_src = fm.source;
      } else if (fm.target == "transform_timestamp") {
        ft_ts_fm = &fm;
      } else if (fm.target == "transform_timestamp_ns") {
        ft_ts_ns_fm = &fm;
      } else if (fm.target == "transform_parent_frame_id") {
        ft_parent_fm = &fm;
      } else if (fm.target == "transform_child_frame_id") {
        ft_child_fm = &fm;
      } else if (fm.target == "transform_translation_x") {
        ft_tx_fm = &fm;
      } else if (fm.target == "transform_translation_y") {
        ft_ty_fm = &fm;
      } else if (fm.target == "transform_translation_z") {
        ft_tz_fm = &fm;
      } else if (fm.target == "transform_rotation_x") {
        ft_qx_fm = &fm;
      } else if (fm.target == "transform_rotation_y") {
        ft_qy_fm = &fm;
      } else if (fm.target == "transform_rotation_z") {
        ft_qz_fm = &fm;
      } else if (fm.target == "transform_rotation_w") {
        ft_qw_fm = &fm;
      }
    }

    std::vector<flatbuffers::Offset<::foxglove::FrameTransform>> transform_offsets;

    if (!transforms_src.empty()) {
      const auto* vec_field = find_fbs_field(obj, transforms_src);

      if (vec_field && vec_field->type()->base_type() == reflection::Vector &&
          vec_field->type()->element() == reflection::Obj) {
        for_each_fbs_vector_object(
            FbsObjectView{&obj, root_table, nullptr}, *vec_field, *schema,
            [&](const FbsObjectView& item, flatbuffers::uoffset_t) {
              uint64_t ft_ts_us = 0;
              uint64_t ft_ts_ns = 0;

              if (ft_ts_fm) {
                ft_ts_us = checked_unsigned_cast<uint64_t>(fbs_get_object_mapped_double(item, *ft_ts_fm));
              }

              if (ft_ts_ns_fm) {
                ft_ts_ns = checked_unsigned_cast<uint64_t>(fbs_get_object_mapped_double(item, *ft_ts_ns_fm));
              }

              std::string parent_fid;

              if (ft_parent_fm) {
                parent_fid = fbs_get_object_mapped_string(item, *ft_parent_fm);
              }

              std::string child_fid;

              if (ft_child_fm) {
                child_fid = fbs_get_object_mapped_string(item, *ft_child_fm);
              }

              double itx = 0.0;
              double ity = 0.0;
              double itz = 0.0;

              if (ft_tx_fm) {
                itx = fbs_get_object_mapped_double(item, *ft_tx_fm);
              }

              if (ft_ty_fm) {
                ity = fbs_get_object_mapped_double(item, *ft_ty_fm);
              }

              if (ft_tz_fm) {
                itz = fbs_get_object_mapped_double(item, *ft_tz_fm);
              }

              double iqx = 0.0;
              double iqy = 0.0;
              double iqz = 0.0;
              double iqw = 1.0;

              if (ft_qx_fm) {
                iqx = fbs_get_object_mapped_double(item, *ft_qx_fm);
              }

              if (ft_qy_fm) {
                iqy = fbs_get_object_mapped_double(item, *ft_qy_fm);
              }

              if (ft_qz_fm) {
                iqz = fbs_get_object_mapped_double(item, *ft_qz_fm);
              }

              if (ft_qw_fm) {
                iqw = fbs_get_object_mapped_double(item, *ft_qw_fm);
              }

              auto item_ts = (ft_ts_ns > 0) ? make_timestamp_from_ns(ft_ts_ns) : make_timestamp_from_us(ft_ts_us);
              auto pfid = builder.CreateString(parent_fid);
              auto cfid = builder.CreateString(child_fid);
              auto translation = ::foxglove::CreateVector3(builder, itx, ity, itz);
              auto rotation = ::foxglove::CreateQuaternion(builder, iqx, iqy, iqz, iqw);
              transform_offsets.emplace_back(
                  ::foxglove::CreateFrameTransform(builder, &item_ts, pfid, cfid, translation, rotation));
            });
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
    const FieldMapping* fix_ts_fm = nullptr;
    const FieldMapping* fix_ts_ns_fm = nullptr;
    const FieldMapping* fix_frame_id_fm = nullptr;
    const FieldMapping* fix_lat_fm = nullptr;
    const FieldMapping* fix_lon_fm = nullptr;
    const FieldMapping* fix_alt_fm = nullptr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "fixes") {
        fixes_src = fm.source;
      } else if (fm.target == "fix_timestamp") {
        fix_ts_fm = &fm;
      } else if (fm.target == "fix_timestamp_ns") {
        fix_ts_ns_fm = &fm;
      } else if (fm.target == "fix_frame_id") {
        fix_frame_id_fm = &fm;
      } else if (fm.target == "fix_latitude") {
        fix_lat_fm = &fm;
      } else if (fm.target == "fix_longitude") {
        fix_lon_fm = &fm;
      } else if (fm.target == "fix_altitude") {
        fix_alt_fm = &fm;
      }
    }

    std::vector<flatbuffers::Offset<::foxglove::LocationFix>> fix_offsets;

    if (!fixes_src.empty()) {
      const auto* vec_field = find_fbs_field(obj, fixes_src);

      if (vec_field && vec_field->type()->base_type() == reflection::Vector &&
          vec_field->type()->element() == reflection::Obj) {
        for_each_fbs_vector_object(
            FbsObjectView{&obj, root_table, nullptr}, *vec_field, *schema,
            [&](const FbsObjectView& item, flatbuffers::uoffset_t) {
              uint64_t item_ts_us = 0;
              uint64_t item_ts_ns = 0;

              if (fix_ts_fm) {
                item_ts_us = checked_unsigned_cast<uint64_t>(fbs_get_object_mapped_double(item, *fix_ts_fm));
              }

              if (fix_ts_ns_fm) {
                item_ts_ns = checked_unsigned_cast<uint64_t>(fbs_get_object_mapped_double(item, *fix_ts_ns_fm));
              }

              std::string item_frame_id;

              if (fix_frame_id_fm) {
                item_frame_id = fbs_get_object_mapped_string(item, *fix_frame_id_fm);
              }

              double lat = 0.0;
              double lon = 0.0;
              double alt = 0.0;

              if (fix_lat_fm) {
                lat = fbs_get_object_mapped_double(item, *fix_lat_fm);
              }

              if (fix_lon_fm) {
                lon = fbs_get_object_mapped_double(item, *fix_lon_fm);
              }

              if (fix_alt_fm) {
                alt = fbs_get_object_mapped_double(item, *fix_alt_fm);
              }

              auto item_ts = (item_ts_ns > 0) ? make_timestamp_from_ns(item_ts_ns) : make_timestamp_from_us(item_ts_us);
              auto fid = builder.CreateString(item_frame_id);
              fix_offsets.emplace_back(::foxglove::CreateLocationFix(builder, &item_ts, fid, lat, lon, alt));
            });
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
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "frame_id") {
        frame_id = fbs_get_mapped_string(*root_table, obj, fm);
      } else if (fm.target == "width") {
        width = checked_unsigned_cast<uint32_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "height") {
        height = checked_unsigned_cast<uint32_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "distortion_model") {
        distortion_model = fbs_get_mapped_string(*root_table, obj, fm);
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
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "frame_id") {
        frame_id = fbs_get_mapped_string(*root_table, obj, fm);
      } else if (fm.target == "format") {
        format = fbs_get_mapped_string(*root_table, obj, fm);
      } else if (fm.target == "data") {
        data_src = fm.source;
      }
    }

    flatbuffers::Offset<flatbuffers::Vector<uint8_t>> data_vec = 0;

    if (!data_src.empty()) {
      const auto* field = find_fbs_field(obj, data_src);

      if (field && is_fbs_byte_vector(*field)) {
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
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "frame_id") {
        frame_id = fbs_get_mapped_string(*root_table, obj, fm);
      } else if (fm.target == "column_count") {
        column_count = checked_unsigned_cast<uint32_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "cell_size_x") {
        cell_size_x = fbs_get_mapped_double(*root_table, obj, fm);
      } else if (fm.target == "cell_size_y") {
        cell_size_y = fbs_get_mapped_double(*root_table, obj, fm);
      } else if (fm.target == "row_stride") {
        row_stride = checked_unsigned_cast<uint32_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "cell_stride") {
        cell_stride = checked_unsigned_cast<uint32_t>(fbs_get_mapped_double(*root_table, obj, fm));
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
        for_each_fbs_vector_object(
            FbsObjectView{&obj, root_table, nullptr}, *vec_field, *schema,
            [&](const FbsObjectView& item, flatbuffers::uoffset_t) {
              FieldMapping empty_fm;
              auto fname = get_fbs_string(item, "name", empty_fm);
              auto foffset = checked_unsigned_cast<uint32_t>(get_fbs_double(item, "offset", empty_fm));
              auto ftype_val = checked_unsigned_cast<uint8_t>(get_fbs_double(item, "type", empty_fm));
              auto ftype = static_cast<::foxglove::NumericType>(ftype_val);
              auto fname_off = builder.CreateString(fname);
              field_offsets.emplace_back(::foxglove::CreatePackedElementField(builder, fname_off, foffset, ftype));
            });
      }
    }

    flatbuffers::Offset<flatbuffers::Vector<uint8_t>> data_vec = 0;

    if (!data_src.empty()) {
      const auto* field = find_fbs_field(obj, data_src);

      if (field && is_fbs_byte_vector(*field)) {
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
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
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
        for_each_fbs_vector_object(FbsObjectView{&obj, root_table, nullptr}, *vec_field, *schema,
                                   [&](const FbsObjectView& item, flatbuffers::uoffset_t) {
                                     FieldMapping empty_fm;
                                     double cx = get_fbs_double(item, "x", empty_fm);
                                     double cy = get_fbs_double(item, "y", empty_fm);
                                     double diameter = get_fbs_double(item, "diameter", empty_fm);
                                     double thickness = get_fbs_double(item, "thickness", empty_fm);

                                     auto pos = ::foxglove::CreatePoint2(builder, cx, cy);
                                     auto fill = ::foxglove::CreateColor(builder, 1.0, 0.0, 0.0, 0.5);
                                     auto outline = ::foxglove::CreateColor(builder, 1.0, 0.0, 0.0, 1.0);
                                     circle_offsets.emplace_back(::foxglove::CreateCircleAnnotation(
                                         builder, &ts, pos, diameter, thickness, fill, outline));
                                   });
      }
    }

    std::vector<flatbuffers::Offset<::foxglove::PointsAnnotation>> points_offsets;

    if (!points_src.empty()) {
      const auto* vec_field = find_fbs_field(obj, points_src);

      if (vec_field && vec_field->type()->base_type() == reflection::Vector &&
          vec_field->type()->element() == reflection::Obj) {
        for_each_fbs_vector_object(FbsObjectView{&obj, root_table, nullptr}, *vec_field, *schema,
                                   [&](const FbsObjectView& item, flatbuffers::uoffset_t) {
                                     FieldMapping empty_fm;
                                     double px = get_fbs_double(item, "x", empty_fm);
                                     double py = get_fbs_double(item, "y", empty_fm);

                                     auto pt = ::foxglove::CreatePoint2(builder, px, py);
                                     std::vector<flatbuffers::Offset<::foxglove::Point2>> pts_vec = {pt};
                                     auto pts = builder.CreateVector(pts_vec);
                                     auto outline = ::foxglove::CreateColor(builder, 0.0, 1.0, 0.0, 1.0);
                                     points_offsets.emplace_back(::foxglove::CreatePointsAnnotation(
                                         builder, &ts, ::foxglove::PointsAnnotationType::POINTS, pts, outline));
                                   });
      }
    }

    std::vector<flatbuffers::Offset<::foxglove::TextAnnotation>> text_offsets;

    if (!texts_src.empty()) {
      const auto* vec_field = find_fbs_field(obj, texts_src);

      if (vec_field && vec_field->type()->base_type() == reflection::Vector &&
          vec_field->type()->element() == reflection::Obj) {
        for_each_fbs_vector_object(FbsObjectView{&obj, root_table, nullptr}, *vec_field, *schema,
                                   [&](const FbsObjectView& item, flatbuffers::uoffset_t) {
                                     FieldMapping empty_fm;
                                     double tx = get_fbs_double(item, "x", empty_fm);
                                     double ty = get_fbs_double(item, "y", empty_fm);
                                     auto text_str = get_fbs_string(item, "text", empty_fm);
                                     double font_size = get_fbs_double(item, "font_size", empty_fm);

                                     if (font_size <= 0.0) {
                                       font_size = 12.0;
                                     }

                                     auto pos = ::foxglove::CreatePoint2(builder, tx, ty);
                                     auto text_off = builder.CreateString(text_str);
                                     auto text_color = ::foxglove::CreateColor(builder, 1.0, 1.0, 1.0, 1.0);
                                     text_offsets.emplace_back(::foxglove::CreateTextAnnotation(
                                         builder, &ts, pos, text_off, font_size, text_color));
                                   });
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
    const FieldMapping* joint_name_fm = nullptr;
    const FieldMapping* joint_position_fm = nullptr;
    const FieldMapping* joint_velocity_fm = nullptr;
    const FieldMapping* joint_effort_fm = nullptr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "timestamp") {
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "joints") {
        joints_src = fm.source;
      } else if (fm.target == "joint_name") {
        joint_name_fm = &fm;
      } else if (fm.target == "joint_position") {
        joint_position_fm = &fm;
      } else if (fm.target == "joint_velocity") {
        joint_velocity_fm = &fm;
      } else if (fm.target == "joint_effort") {
        joint_effort_fm = &fm;
      }
    }

    std::vector<flatbuffers::Offset<::foxglove::JointState>> joint_offsets;

    if (!joints_src.empty()) {
      const auto* vec_field = find_fbs_field(obj, joints_src);

      if (vec_field && vec_field->type()->base_type() == reflection::Vector &&
          vec_field->type()->element() == reflection::Obj) {
        for_each_fbs_vector_object(
            FbsObjectView{&obj, root_table, nullptr}, *vec_field, *schema,
            [&](const FbsObjectView& item, flatbuffers::uoffset_t) {
              FieldMapping empty_fm;
              auto jname = joint_name_fm ? fbs_get_object_mapped_string(item, *joint_name_fm)
                                         : get_fbs_string(item, "name", empty_fm);
              double jpos = joint_position_fm ? fbs_get_object_mapped_double(item, *joint_position_fm)
                                              : get_fbs_double(item, "position", empty_fm);
              double jvel = joint_velocity_fm ? fbs_get_object_mapped_double(item, *joint_velocity_fm)
                                              : get_fbs_double(item, "velocity", empty_fm);
              double jeff = joint_effort_fm ? fbs_get_object_mapped_double(item, *joint_effort_fm)
                                            : get_fbs_double(item, "effort", empty_fm);

              auto name_off = builder.CreateString(jname);
              joint_offsets.emplace_back(
                  ::foxglove::CreateJointState(builder, name_off, jpos, jvel, ::flatbuffers::nullopt, jeff));
            });
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
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "frame_id") {
        frame_id = fbs_get_mapped_string(*root_table, obj, fm);
      } else if (fm.target == "position_x") {
        position_x = fbs_get_mapped_double(*root_table, obj, fm);
      } else if (fm.target == "position_y") {
        position_y = fbs_get_mapped_double(*root_table, obj, fm);
      } else if (fm.target == "position_z") {
        position_z = fbs_get_mapped_double(*root_table, obj, fm);
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
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "sample_rate") {
        sample_rate = checked_unsigned_cast<uint32_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "number_of_channels") {
        number_of_channels = checked_unsigned_cast<uint32_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "format") {
        format = fbs_get_mapped_string(*root_table, obj, fm);
      } else if (fm.target == "data") {
        data_src = fm.source;
      }
    }

    flatbuffers::Offset<flatbuffers::Vector<uint8_t>> data_vec = 0;

    if (!data_src.empty()) {
      const auto* field = find_fbs_field(obj, data_src);

      if (field && is_fbs_byte_vector(*field)) {
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
        timestamp_us = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "timestamp_ns") {
        timestamp_ns = checked_unsigned_cast<uint64_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "frame_id") {
        frame_id = fbs_get_mapped_string(*root_table, obj, fm);
      } else if (fm.target == "voxel_size_x") {
        voxel_size_x = fbs_get_mapped_double(*root_table, obj, fm);
      } else if (fm.target == "voxel_size_y") {
        voxel_size_y = fbs_get_mapped_double(*root_table, obj, fm);
      } else if (fm.target == "voxel_size_z") {
        voxel_size_z = fbs_get_mapped_double(*root_table, obj, fm);
      } else if (fm.target == "row_count") {
        row_count = checked_unsigned_cast<uint32_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "column_count") {
        column_count = checked_unsigned_cast<uint32_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "slice_stride") {
        slice_stride = checked_unsigned_cast<uint32_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "row_stride") {
        row_stride = checked_unsigned_cast<uint32_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "cell_stride") {
        cell_stride = checked_unsigned_cast<uint32_t>(fbs_get_mapped_double(*root_table, obj, fm));
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
        for_each_fbs_vector_object(
            FbsObjectView{&obj, root_table, nullptr}, *vec_field, *schema,
            [&](const FbsObjectView& item, flatbuffers::uoffset_t) {
              FieldMapping empty_fm;
              auto fname = get_fbs_string(item, "name", empty_fm);
              auto foffset = checked_unsigned_cast<uint32_t>(get_fbs_double(item, "offset", empty_fm));
              auto ftype_val = checked_unsigned_cast<uint8_t>(get_fbs_double(item, "type", empty_fm));
              auto ftype = static_cast<::foxglove::NumericType>(ftype_val);
              auto fname_off = builder.CreateString(fname);
              field_offsets.emplace_back(::foxglove::CreatePackedElementField(builder, fname_off, foffset, ftype));
            });
      }
    }

    flatbuffers::Offset<flatbuffers::Vector<uint8_t>> data_vec = 0;

    if (!data_src.empty()) {
      const auto* field = find_fbs_field(obj, data_src);

      if (field && is_fbs_byte_vector(*field)) {
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

double FoxgloveConverter::get_fbs_double(const flatbuffers::Table& table, const reflection::Object& obj,
                                         const std::string& field_name, const FieldMapping& mapping) {
  return get_fbs_double(FbsObjectView{&obj, &table, nullptr}, field_name, mapping);
}

double FoxgloveConverter::get_fbs_double(const FbsObjectView& view, const std::string& field_name,
                                         const FieldMapping& mapping) {
  const auto* field = view && view.object ? find_fbs_field(*view.object, field_name) : nullptr;

  if VLIKELY (field && is_fbs_numeric_type(field->type()->base_type()) && is_fbs_field_present(view, *field)) {
    return get_fbs_field_as_double(view, *field);
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
  return get_fbs_string(FbsObjectView{&obj, &table, nullptr}, field_name, mapping, schema);
}

std::string FoxgloveConverter::get_fbs_string(const FbsObjectView& view, const std::string& field_name,
                                              const FieldMapping& mapping, const reflection::Schema* schema) {
  if VUNLIKELY (!mapping.expression.empty() && schema != nullptr && view && view.object) {
    return format_expression_string(evaluate_expression_with_fbs(mapping.expression, view, *schema));
  }

  const auto* field = view && view.object ? find_fbs_field(*view.object, field_name) : nullptr;

  if VLIKELY (field && field->type()->base_type() == reflection::String && is_fbs_field_present(view, *field)) {
    return get_fbs_field_as_string(view, *field, schema);
  }

  return mapping.has_default_value ? mapping.default_value : std::string{};
}

#endif

}  // namespace webviz
}  // namespace vlink
