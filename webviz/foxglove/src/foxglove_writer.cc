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

#include "./foxglove_writer.h"

#include <vlink/base/name_detector.h>
#include <vlink/base/quantize.h>
#include <vlink/zerocopy/camera_frame.h>
#include <vlink/zerocopy/point_cloud.h>
#include <vlink/zerocopy/tensor.h>

#include <CompressedImage.fbs.hpp>
#include <CompressedVideo.fbs.hpp>
#include <Grid.fbs.hpp>
#include <Log.fbs.hpp>
#include <PointCloud.fbs.hpp>
#include <RawAudio.fbs.hpp>
#include <RawImage.fbs.hpp>
#include <SceneUpdate.fbs.hpp>
#include <cmath>
#include <cstring>
#include <limits>

namespace vlink {
namespace webviz {

using Builder = flatbuffers::FlatBufferBuilder;
namespace fg = ::foxglove;

static fg::Time time_value(uint64_t nanoseconds) {
  const auto seconds = nanoseconds / 1000000000;

  if (seconds > std::numeric_limits<uint32_t>::max()) {
    return {std::numeric_limits<uint32_t>::max(), 999999999};
  }

  return {static_cast<uint32_t>(seconds), static_cast<uint32_t>(nanoseconds % 1000000000)};
}

static flatbuffers::Offset<fg::Quaternion> euler_quaternion(Builder& b, double roll, double pitch, double yaw) {
  const auto cr = std::cos(roll * 0.5);
  const auto sr = std::sin(roll * 0.5);
  const auto cp = std::cos(pitch * 0.5);
  const auto sp = std::sin(pitch * 0.5);
  const auto cy = std::cos(yaw * 0.5);
  const auto sy = std::sin(yaw * 0.5);
  return fg::CreateQuaternion(b, sr * cp * cy - cr * sp * sy, cr * sp * cy + sr * cp * sy, cr * cp * sy - sr * sp * cy,
                              cr * cp * cy + sr * sp * sy);
}

bool validate_foxglove_mapping(const MessageMapping& mapping) {
  if (!mapping.entity_path.empty() || mapping.is_static) {
    return false;
  }
  if (!mapping.converter.empty()) {
    return mapping.fields.empty() && (mapping.converter == "send_time" || mapping.converter == "passthrough" ||
                                      !native_ser(mapping.converter).empty());
  }
  const auto bfbs = foxglove_schema(mapping.target);
  if (bfbs.empty() || mapping.schema_encoding != "flatbuffer") {
    return false;
  }
  const auto* schema = reflection::GetSchema(bfbs.data());
  for (const auto& field : mapping.fields) {
    FieldPath path;
    if (!parse_field_path(field.target, path, true) || path.empty()) {
      return false;
    }
    const auto* object = schema->root_table();
    const reflection::Type* type = nullptr;
    auto base = reflection::Obj;
    for (const auto& step : path) {
      if (!step.name.empty()) {
        if (base != reflection::Obj || !object) {
          return false;
        }
        const auto* member = object->fields()->LookupByKey(step.name.c_str());
        if (!member) {
          return false;
        }
        type = member->type();
        base = type->base_type();
      }
      if (step.indexed) {
        if (!type || base != reflection::Vector) {
          return false;
        }
        base = type->element();
      }
      object = type && base == reflection::Obj ? schema->objects()->Get(type->index()) : nullptr;
    }
    if (field.time_scale &&
        (!object || !object->is_struct() ||
         (object->name()->string_view() != "foxglove.Time" && object->name()->string_view() != "foxglove.Duration"))) {
      return false;
    }
    if (field.expression && !flatbuffers::IsScalar(base) && !field.time_scale) {
      return false;
    }
    if (field.time_scale != 0U && FieldReader({}, &mapping).has_descendant(field.target)) {
      return false;
    }
  }
  return true;
}

bool write_foxglove_mapping(std::string_view schema, const FieldReader& fields, Builder& builder) {
  const auto bfbs = foxglove_schema(schema);
  if (bfbs.empty()) {
    return false;
  }
  builder.Clear();
  return write_flatbuffer_mapping(*reflection::GetSchema(bfbs.data()), fields, builder);
}

static bool write_camera(const Bytes& raw, Builder& b, std::string& schema, int64_t& timestamp) {
  using Camera = zerocopy::CameraFrame;
  Camera camera;

  if VUNLIKELY (!(camera << raw) || camera.size() == 0) {
    return false;
  }

  const auto format = camera.format();
  const auto time = time_value(camera.header.time_meas);
  timestamp = camera.header.time_meas == 0
                  ? -1
                  : static_cast<int64_t>(
                        std::min(camera.header.time_meas, static_cast<uint64_t>(std::numeric_limits<int64_t>::max())));
  const auto frame =
      b.CreateString(camera.header.frame_id, strnlen(camera.header.frame_id, sizeof(camera.header.frame_id)));
  std::string encoding(Camera::encoding_from_format(format));
  const bool video = format == Camera::kFormatH264 || format == Camera::kFormatH265 || format == Camera::kFormatAv1;

  if (video || format == Camera::kFormatJpeg || format == Camera::kFormatMjpeg || format == Camera::kFormatPng ||
      format == Camera::kFormatWebp) {
    if VUNLIKELY (video && camera.stream() == Camera::kStreamB) {
      return false;
    }

    if (format == Camera::kFormatMjpeg) {
      encoding = "jpeg";
    }

    const auto data = b.CreateVector(camera.data(), camera.size());
    const auto name = b.CreateString(encoding);

    if (video) {
      b.Finish(fg::CreateCompressedVideo(b, &time, frame, data, name));
      schema = "foxglove.CompressedVideo";
    } else {
      b.Finish(fg::CreateCompressedImage(b, &time, frame, data, name));
      schema = "foxglove.CompressedImage";
    }

    return true;
  }

  size_t pixel_size = 0;

  switch (format) {
    case Camera::kFormatNv12:
      if ((camera.width() & 1U) != 0 || (camera.height() & 1U) != 0) {
        return false;
      }
      pixel_size = 1;
      break;
    case Camera::kFormatYuyv:
    case Camera::kFormatUyvy:
      if ((camera.width() & 1U) != 0) {
        return false;
      }
      encoding = format == Camera::kFormatYuyv ? "yuv422_yuy2" : "yuv422";
      pixel_size = 2;
      break;
    case Camera::kFormatBgr888Packed:
    case Camera::kFormatRgb888Packed:
    case Camera::kFormatRgb888Planar:
    case Camera::kFormatUint8C3:
      pixel_size = 3;
      if (format == Camera::kFormatRgb888Planar) {
        encoding = "rgb8";
      }
      break;
    case Camera::kFormatMono8:
    case Camera::kFormatUint8C1:
    case Camera::kFormatBayerRggb8:
    case Camera::kFormatBayerBggr8:
    case Camera::kFormatBayerGbrg8:
    case Camera::kFormatBayerGrbg8:
      pixel_size = 1;
      break;
    case Camera::kFormatMono16:
    case Camera::kFormatUint16C1:
      pixel_size = 2;
      break;
    case Camera::kFormatRgba8888Packed:
    case Camera::kFormatBgra8888Packed:
    case Camera::kFormatFloat32C1:
      pixel_size = 4;
      break;
    default:
      return false;
  }

  const uint64_t step = static_cast<uint64_t>(camera.width()) * pixel_size;
  if (step > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  const uint64_t size = step * camera.height();
  const uint64_t expected = format == Camera::kFormatNv12 ? size + size / 2 : size;

  if VUNLIKELY (camera.width() == 0 || camera.height() == 0 || step > std::numeric_limits<uint32_t>::max() ||
                expected != camera.size()) {
    return false;
  }

  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> data;

  if (format == Camera::kFormatRgb888Planar) {
    uint8_t* output = nullptr;
    data = b.CreateUninitializedVector(camera.size(), &output);
    const auto pixels = camera.size() / 3;

    for (size_t i = 0; i < pixels; ++i) {
      output[3 * i] = camera.data()[i];
      output[3 * i + 1] = camera.data()[pixels + i];
      output[3 * i + 2] = camera.data()[2 * pixels + i];
    }
  } else {
    data = b.CreateVector(camera.data(), camera.size());
  }

  b.Finish(fg::CreateRawImage(b, &time, frame, camera.width(), camera.height(), b.CreateString(encoding),
                              static_cast<uint32_t>(step), data));
  schema = "foxglove.RawImage";
  return true;
}

static fg::NumericType point_type(uint16_t type, uint16_t size) {
  using PC = zerocopy::PointCloud;
  switch (type) {
    case PC::kBoolType:
    case PC::kUint8Type:
      return size == 1 ? fg::NumericType::UINT8 : fg::NumericType::UNKNOWN;
    case PC::kInt8Type:
      return size == 1 ? fg::NumericType::INT8 : fg::NumericType::UNKNOWN;
    case PC::kInt16Type:
      return size == 2 ? fg::NumericType::INT16 : fg::NumericType::UNKNOWN;
    case PC::kUint16Type:
      return size == 2 ? fg::NumericType::UINT16 : fg::NumericType::UNKNOWN;
    case PC::kInt32Type:
      return size == 4 ? fg::NumericType::INT32 : fg::NumericType::UNKNOWN;
    case PC::kUint32Type:
      return size == 4 ? fg::NumericType::UINT32 : fg::NumericType::UNKNOWN;
    case PC::kFloatType:
      return size == 4 ? fg::NumericType::FLOAT32 : fg::NumericType::UNKNOWN;
    case PC::kDoubleType:
      return size == 8 ? fg::NumericType::FLOAT64 : fg::NumericType::UNKNOWN;
    case PC::kUnknownType:
      switch (size) {
        case 1:
          return fg::NumericType::UINT8;
        case 2:
          return fg::NumericType::INT16;
        case 4:
          return fg::NumericType::FLOAT32;
        case 8:
          return fg::NumericType::FLOAT64;
        default:
          return fg::NumericType::UNKNOWN;
      }
    default:
      return fg::NumericType::UNKNOWN;
  }
}

static bool write_points(const Bytes& raw, Builder& b, int64_t& timestamp) {
  zerocopy::PointCloud cloud;

  if VUNLIKELY (!(cloud << raw) || cloud.size() == 0 || cloud.pack_size() == 0) {
    return false;
  }

  const auto keys = cloud.get_key_list();
  const auto extent = cloud.get_extent();
  const auto stride = cloud.pack_size() + (extent > 0 ? 6 : 0);
  size_t coordinates = 0;
  size_t offset = 0;
  std::vector<flatbuffers::Offset<fg::PackedElementField>> fields;
  fields.reserve(keys.size());

  for (size_t i = 0; i < keys.size(); ++i) {
    const auto& key = keys[i];
    auto type = point_type(key.type, key.size);

    if (key.name == "x") {
      coordinates |= 1;
    }
    if (key.name == "y") {
      coordinates |= 2;
    }
    if (key.name == "z") {
      coordinates |= 4;
    }

    if (extent > 0 && i < 3) {
      if (key.name != std::string(1, "xyz"[i]) || key.size != 2 || key.type == zerocopy::PointCloud::kInt64Type ||
          key.type == zerocopy::PointCloud::kUint64Type) {
        return false;
      }

      type = fg::NumericType::FLOAT32;
    }

    if (type == fg::NumericType::UNKNOWN) {
      return false;
    }

    fields.push_back(fg::CreatePackedElementField(b, b.CreateString(key.name), static_cast<uint32_t>(offset), type));
    offset += extent > 0 && i < 3 ? 4 : key.size;
  }

  if VUNLIKELY ((coordinates == 0 || (coordinates & (coordinates - 1)) == 0) || (extent > 0 && keys.size() < 3) ||
                offset != stride || stride > std::numeric_limits<uint32_t>::max() ||
                cloud.size() > std::numeric_limits<size_t>::max() / stride) {
    return false;
  }

  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> data;

  if (extent > 0) {
    uint8_t* output = nullptr;
    data = b.CreateUninitializedVector(cloud.size() * stride, &output);

    for (size_t i = 0; i < cloud.size(); ++i) {
      const auto* source = cloud.get_internal_data() + i * cloud.pack_size();

      for (size_t axis = 0; axis < 3; ++axis) {
        int16_t packed = 0;
        std::memcpy(&packed, source + axis * 2, sizeof(packed));
        const auto value = static_cast<float>(Quantize::decode<double>(extent, packed));
        std::memcpy(output + i * stride + axis * 4, &value, sizeof(value));
      }

      std::memcpy(output + i * stride + 12, source + 6, cloud.pack_size() - 6);
    }
  } else {
    data = b.CreateVector(cloud.get_internal_data(), cloud.size() * stride);
  }

  const auto time = time_value(cloud.header.time_meas);
  timestamp = cloud.header.time_meas == 0
                  ? -1
                  : static_cast<int64_t>(
                        std::min(cloud.header.time_meas, static_cast<uint64_t>(std::numeric_limits<int64_t>::max())));
  const auto frame =
      b.CreateString(cloud.header.frame_id, strnlen(cloud.header.frame_id, sizeof(cloud.header.frame_id)));
  b.Finish(fg::CreatePointCloud(b, &time, frame, 0, static_cast<uint32_t>(stride), b.CreateVector(fields), data));
  return true;
}

bool write_foxglove_native(const std::string& ser, const Bytes& raw, Builder& b, std::string& schema,
                           int64_t& timestamp) {
  b.Clear();
  const auto type = zerocopy::MessageParser::detect_type(ser);

  if (type == zerocopy::MessageParser::kCameraFrame) {
    return write_camera(raw, b, schema, timestamp);
  }

  if (type == zerocopy::MessageParser::kPointCloud) {
    schema = "foxglove.PointCloud";
    return write_points(raw, b, timestamp);
  }

  zerocopy::MessageParser parser;

  if VUNLIKELY (!parser.parse(type, raw)) {
    return false;
  }

  const FieldReader fields(MessageView(parser), nullptr);
  const auto nanos = fields.integer("header.time_meas");
  timestamp = nanos == 0
                  ? -1
                  : static_cast<int64_t>(std::min(nanos, static_cast<uint64_t>(std::numeric_limits<int64_t>::max())));
  const auto time = time_value(nanos);
  const auto frame = b.CreateString(fields.text("header.frame_id"));

  if (type == zerocopy::MessageParser::kOccupancyGrid) {
    const auto width = fields.integer("width");
    const auto height = fields.integer("height");
    const auto cell_size = fields.integer("cell_size");
    const auto bytes = fields.bytes("data");

    if VUNLIKELY (width == 0 || height == 0 || cell_size == 0 ||
                  width > std::numeric_limits<uint32_t>::max() / cell_size ||
                  width * cell_size > std::numeric_limits<size_t>::max() / height ||
                  bytes.size() != width * height * cell_size) {
      return false;
    }

    const auto cell_type = fields.integer("cell_type");
    auto numeric_type = fg::NumericType::FLOAT32;
    if (cell_type == zerocopy::OccupancyGrid::kCellInt8) {
      numeric_type = fg::NumericType::INT8;
    } else if (cell_type == zerocopy::OccupancyGrid::kCellUint8) {
      numeric_type = fg::NumericType::UINT8;
    } else if (cell_type == zerocopy::OccupancyGrid::kCellUint16) {
      numeric_type = fg::NumericType::UINT16;
    }
    const auto pose = fg::CreatePose(
        b, fg::CreateVector3(b, fields.number("origin_x"), fields.number("origin_y"), fields.number("origin_z")),
        euler_quaternion(b, 0, 0, fields.number("origin_yaw")));
    const std::vector<flatbuffers::Offset<fg::PackedElementField>> packed = {
        fg::CreatePackedElementField(b, b.CreateString("value"), 0, numeric_type)};
    const auto resolution = fields.number("resolution");
    b.Finish(fg::CreateGrid(b, &time, frame, pose, static_cast<uint32_t>(width),
                            fg::CreateVector2(b, resolution, resolution), static_cast<uint32_t>(width * cell_size),
                            static_cast<uint32_t>(cell_size), b.CreateVector(packed),
                            b.CreateVector(bytes.data(), bytes.size())));
    schema = "foxglove.Grid";
  } else if (type == zerocopy::MessageParser::kObjectArray) {
    std::vector<flatbuffers::Offset<fg::CubePrimitive>> cubes;
    const auto count = parser.collection_size("objects");
    cubes.reserve(count);
    static constexpr double kColors[][4] = {
        {0.5, 0.5, 0.5, 0.8}, {0.2, 0.6, 1.0, 0.8}, {0.2, 0.9, 0.2, 0.8}, {1.0, 0.8, 0.0, 0.8}, {0.8, 0.2, 0.8, 0.8}};

    for (size_t i = 0; i < count; ++i) {
      const auto get = [&](std::string_view name) {
        double value = 0;
        parser.numeric("objects", i, name, value);
        return value;
      };
      const auto size = [&](std::string_view name) {
        const auto v = get(name);
        return v > 0 ? v : 1.0;
      };
      const auto pose =
          fg::CreatePose(b, fg::CreateVector3(b, get("position[0]"), get("position[1]"), get("position[2]")),
                         euler_quaternion(b, 0, 0, get("yaw")));
      const auto state = static_cast<size_t>(get("motion_state"));
      flatbuffers::Offset<fg::Color> color;

      if (state < 5) {
        color = fg::CreateColor(b, kColors[state][0], kColors[state][1], kColors[state][2], kColors[state][3]);
      } else {
        const auto hue = std::fmod(get("class_id"), 6);
        color = fg::CreateColor(b, std::fmod(hue * 0.17, 1), std::fmod(hue * 0.29 + 0.3, 1),
                                std::fmod(hue * 0.41 + 0.6, 1), 0.8);
      }

      cubes.push_back(fg::CreateCubePrimitive(
          b, pose, fg::CreateVector3(b, size("size[0]"), size("size[1]"), size("size[2]")), color));
    }

    auto id = fields.text("source_id");

    if (id.empty()) {
      id = "object_array";
    }

    const auto entity =
        fg::CreateSceneEntity(b, &time, frame, b.CreateString(id), nullptr, false, 0, 0, b.CreateVector(cubes));
    const std::vector<flatbuffers::Offset<fg::SceneEntity>> entities = {entity};
    b.Finish(fg::CreateSceneUpdate(b, 0, b.CreateVector(entities)));
    schema = "foxglove.SceneUpdate";
  } else if (type == zerocopy::MessageParser::kAudioFrame) {
    const auto data = fields.bytes("data");
    const auto channels = fields.integer("num_channels");
    const auto samples = fields.integer("num_samples");
    const auto rate = fields.integer("sample_rate");

    if VUNLIKELY (fields.integer("format") != zerocopy::AudioFrame::kFormatPcmS16 ||
                  fields.integer("layout") != zerocopy::AudioFrame::kLayoutInterleaved || channels == 0 ||
                  samples == 0 || rate == 0 || samples > std::numeric_limits<size_t>::max() / (channels * 2) ||
                  data.size() != samples * channels * 2) {
      return false;
    }

    b.Finish(fg::CreateRawAudio(b, &time, b.CreateVector(data.data(), data.size()), b.CreateString("pcm-s16"),
                                static_cast<uint32_t>(rate), static_cast<uint32_t>(channels)));
    schema = "foxglove.RawAudio";
  } else if (type == zerocopy::MessageParser::kTensor || type == zerocopy::MessageParser::kRawData) {
    nlohmann::json metadata;
    std::string name = fields.text("name");
    std::string message;

    if (type == zerocopy::MessageParser::kTensor) {
      for (const auto* key : {"name", "model_id", "layout"}) {
        metadata[key] = fields.text(key);
      }

      for (const auto* key : {"rank", "num_elements", "element_size", "batch_size"}) {
        metadata[key] = fields.integer(key);
      }

      for (const auto* key : {"shape", "strides"}) {
        metadata[key] = nlohmann::json::array();
        const auto values = fields.view(key);

        for (size_t i = 0; i < values.size(); ++i) {
          metadata[key].push_back(field_unsigned(values.at(i).value()));
        }
      }

      metadata["dtype"] = NameDetector::get_enum(static_cast<zerocopy::Tensor::DataType>(fields.integer("dtype")));
      metadata["device"] = NameDetector::get_enum(static_cast<zerocopy::Tensor::Device>(fields.integer("device")));
      metadata["data_bytes"] = fields.bytes("data").size();
      metadata["quant_scale"] = fields.number("quant_scale");
      metadata["quant_zero_point"] = field_integer(fields.value("quant_zero_point"));
      message = metadata.dump();

      if (name.empty()) {
        name = "Tensor";
      }
    } else {
      name = "RawData";
      message = "RawData (" + std::to_string(fields.bytes("data").size()) + " bytes)";
    }

    b.Finish(fg::CreateLog(b, &time, fg::LogLevel::INFO, b.CreateString(message), b.CreateString(name)));
    schema = "foxglove.Log";
  } else {
    return false;
  }

  return true;
}

}  // namespace webviz
}  // namespace vlink
