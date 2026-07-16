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

#include "./rerun_converter.h"

#include <vlink/base/helpers.h>
#include <vlink/zerocopy/message_parser.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <mutex>
#include <unordered_set>

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

void RerunConverter::apply_message_timestamp(::rerun::RecordingStream& rec, int64_t timestamp_ns) const {
  if VUNLIKELY (timestamp_ns < 0) {
    return;
  }

  if VUNLIKELY (!config_.use_timestamp_timeline || config_.timestamp_timeline.empty()) {
    return;
  }

  rec.set_time_timestamp_nanos_since_epoch(config_.timestamp_timeline, timestamp_ns);
}

int64_t RerunConverter::clamp_header_timestamp_ns(uint64_t timestamp_ns) {
  if VUNLIKELY (timestamp_ns == 0) {
    return -1;
  }

  if VUNLIKELY (timestamp_ns > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return std::numeric_limits<int64_t>::max();
  }

  return static_cast<int64_t>(timestamp_ns);
}

static uint64_t zerocopy_timestamp(const zerocopy::MessageParser& parser) {
  zerocopy::MessageParser::Value value;

  if VUNLIKELY (!parser.value("header.time_meas", value)) {
    return 0;
  }

  const auto* timestamp = std::get_if<uint64_t>(&value);
  return timestamp == nullptr ? 0 : *timestamp;
}

static bool read_parser_size(const zerocopy::MessageParser& parser, std::string_view path, size_t& out) {
  zerocopy::MessageParser::Value value;

  if VUNLIKELY (!parser.value(path, value)) {
    return false;
  }

  const auto* size = std::get_if<uint64_t>(&value);

  if VUNLIKELY (size == nullptr || *size > std::numeric_limits<size_t>::max()) {
    return false;
  }

  out = static_cast<size_t>(*size);
  return true;
}

static bool image_size(size_t pixels, size_t bytes_per_pixel, size_t data_size, size_t& out) {
  if VUNLIKELY (!mul_size(pixels, bytes_per_pixel, out) || out != data_size) {
    return false;
  }

  return true;
}

static bool copy_channel(const uint8_t* data, size_t data_size, size_t pixels, size_t step, size_t offset,
                         std::vector<uint8_t>& gray) {
  size_t expected = 0;

  if VUNLIKELY (step == 0 || offset >= step || !image_size(pixels, step, data_size, expected)) {
    return false;
  }

  gray.resize(pixels);

  for (size_t i = 0; i < pixels; ++i) {
    gray[i] = data[i * step + offset];
  }

  return true;
}

static bool log_raw_image(::rerun::RecordingStream& rec, const std::string& entity_path, const uint8_t* data,
                          size_t data_size, uint32_t width, uint32_t height,
                          ::rerun::datatypes::ColorModel color_model) {
  auto pixel_data = ::rerun::Collection<uint8_t>::borrow(data, data_size);
  auto image = ::rerun::archetypes::Image(std::move(pixel_data), {width, height}, color_model);
  rec.log(entity_path, image);

  return true;
}

static bool log_raw_image(::rerun::RecordingStream& rec, const std::string& entity_path, std::vector<uint8_t>&& data,
                          uint32_t width, uint32_t height, ::rerun::datatypes::ColorModel color_model) {
  auto pixel_data = ::rerun::Collection<uint8_t>::borrow(data.data(), data.size());
  auto image = ::rerun::archetypes::Image(std::move(pixel_data), {width, height}, color_model);
  rec.log(entity_path, image);

  return true;
}

static bool log_pixel_image(::rerun::RecordingStream& rec, const std::string& entity_path, const uint8_t* data,
                            size_t data_size, uint32_t width, uint32_t height,
                            ::rerun::datatypes::PixelFormat pixel_format) {
  size_t pixels = 0;
  size_t expected_size = 0;

  if VUNLIKELY (!mul_size(static_cast<size_t>(width), static_cast<size_t>(height), pixels)) {
    return false;
  }

  switch (pixel_format) {
    case ::rerun::datatypes::PixelFormat::Y_U_V24_FullRange:
    case ::rerun::datatypes::PixelFormat::Y_U_V24_LimitedRange:
      if VUNLIKELY (!mul_size(pixels, 3U, expected_size)) {
        return false;
      }
      break;
    case ::rerun::datatypes::PixelFormat::Y_U_V16_FullRange:
    case ::rerun::datatypes::PixelFormat::Y_U_V16_LimitedRange:
    case ::rerun::datatypes::PixelFormat::YUY2:
      if VUNLIKELY (!mul_size(pixels, 2U, expected_size)) {
        return false;
      }
      break;
    case ::rerun::datatypes::PixelFormat::Y_U_V12_FullRange:
    case ::rerun::datatypes::PixelFormat::Y_U_V12_LimitedRange:
    case ::rerun::datatypes::PixelFormat::NV12:
      if VUNLIKELY (!mul_size(pixels, 3U, expected_size) || (expected_size % 2U) != 0) {
        return false;
      }
      expected_size /= 2U;
      break;
    case ::rerun::datatypes::PixelFormat::Y8_LimitedRange:
    case ::rerun::datatypes::PixelFormat::Y8_FullRange:
      expected_size = pixels;
      break;
    default:
      return false;
  }

  const auto format = ::rerun::datatypes::ImageFormat({width, height}, pixel_format);
  const auto sdk_size = format.num_bytes();

  if VUNLIKELY (expected_size != data_size || sdk_size != expected_size) {
    return false;
  }

  auto pixel_data = ::rerun::Collection<uint8_t>::borrow(data, expected_size);
  auto image = ::rerun::archetypes::Image(std::move(pixel_data), {width, height}, pixel_format);
  rec.log(entity_path, image);

  return true;
}

static bool log_typed_image(::rerun::RecordingStream& rec, const std::string& entity_path, const uint8_t* data,
                            size_t data_size, uint32_t width, uint32_t height,
                            ::rerun::datatypes::ColorModel color_model, ::rerun::datatypes::ChannelDatatype datatype) {
  size_t channels = 0;

  switch (color_model) {
    case ::rerun::datatypes::ColorModel::L:
      channels = 1;
      break;
    case ::rerun::datatypes::ColorModel::BGR:
    case ::rerun::datatypes::ColorModel::RGB:
      channels = 3;
      break;
    case ::rerun::datatypes::ColorModel::BGRA:
    case ::rerun::datatypes::ColorModel::RGBA:
      channels = 4;
      break;
    default:
      return false;
  }

  size_t element_size = 0;

  switch (datatype) {
    case ::rerun::datatypes::ChannelDatatype::U8:
    case ::rerun::datatypes::ChannelDatatype::I8:
      element_size = 1;
      break;
    case ::rerun::datatypes::ChannelDatatype::U16:
    case ::rerun::datatypes::ChannelDatatype::I16:
    case ::rerun::datatypes::ChannelDatatype::F16:
      element_size = 2;
      break;
    case ::rerun::datatypes::ChannelDatatype::U32:
    case ::rerun::datatypes::ChannelDatatype::I32:
    case ::rerun::datatypes::ChannelDatatype::F32:
      element_size = 4;
      break;
    case ::rerun::datatypes::ChannelDatatype::U64:
    case ::rerun::datatypes::ChannelDatatype::I64:
    case ::rerun::datatypes::ChannelDatatype::F64:
      element_size = 8;
      break;
    default:
      return false;
  }

  size_t pixels = 0;
  size_t elements = 0;
  size_t expected_size = 0;

  if VUNLIKELY (!mul_size(static_cast<size_t>(width), static_cast<size_t>(height), pixels) ||
                !mul_size(pixels, channels, elements) || !mul_size(elements, element_size, expected_size)) {
    return false;
  }

  const auto format = ::rerun::datatypes::ImageFormat({width, height}, color_model, datatype);
  const auto sdk_size = format.num_bytes();

  if VUNLIKELY (expected_size != data_size || sdk_size != expected_size) {
    return false;
  }

  auto pixel_data = ::rerun::Collection<uint8_t>::borrow(data, expected_size);
  auto image = ::rerun::archetypes::Image(std::move(pixel_data), {width, height}, color_model, datatype);
  rec.log(entity_path, image);

  return true;
}

static ::rerun::archetypes::Pinhole make_pinhole(float fx, float fy, float cx, float cy, float width, float height) {
  return ::rerun::archetypes::Pinhole(
             ::rerun::components::PinholeProjection(std::array<float, 9>{fx, 0.0F, 0.0F, 0.0F, fy, 0.0F, cx, cy, 1.0F}))
      .with_resolution(width, height);
}

static bool parse_depth_datatype(std::string_view name, ::rerun::datatypes::ChannelDatatype& datatype,
                                 size_t& element_size) {
  using ChannelDatatype = ::rerun::datatypes::ChannelDatatype;

  if (name.empty() || name == "U8") {
    datatype = ChannelDatatype::U8;
    element_size = sizeof(uint8_t);
  } else if (name == "I8") {
    datatype = ChannelDatatype::I8;
    element_size = sizeof(int8_t);
  } else if (name == "U16") {
    datatype = ChannelDatatype::U16;
    element_size = sizeof(uint16_t);
  } else if (name == "I16") {
    datatype = ChannelDatatype::I16;
    element_size = sizeof(int16_t);
  } else if (name == "U32") {
    datatype = ChannelDatatype::U32;
    element_size = sizeof(uint32_t);
  } else if (name == "I32") {
    datatype = ChannelDatatype::I32;
    element_size = sizeof(int32_t);
  } else if (name == "U64") {
    datatype = ChannelDatatype::U64;
    element_size = sizeof(uint64_t);
  } else if (name == "I64") {
    datatype = ChannelDatatype::I64;
    element_size = sizeof(int64_t);
  } else if (name == "F16") {
    datatype = ChannelDatatype::F16;
    element_size = sizeof(uint16_t);
  } else if (name == "F32") {
    datatype = ChannelDatatype::F32;
    element_size = sizeof(float);
  } else if (name == "F64") {
    datatype = ChannelDatatype::F64;
    element_size = sizeof(double);
  } else {
    return false;
  }

  return true;
}

static bool log_depth_image_bytes(::rerun::RecordingStream& rec, const std::string& entity_path, const uint8_t* data,
                                  size_t data_size, uint32_t width, uint32_t height, std::string_view datatype_name,
                                  float meter) {
  ::rerun::datatypes::ChannelDatatype datatype;
  size_t element_size = 0;
  size_t pixel_count = 0;
  size_t expected_size = 0;

  if VUNLIKELY (data == nullptr || width == 0 || height == 0 ||
                !parse_depth_datatype(datatype_name, datatype, element_size) ||
                !mul_size(static_cast<size_t>(width), static_cast<size_t>(height), pixel_count) ||
                !mul_size(pixel_count, element_size, expected_size) || data_size != expected_size) {
    return false;
  }

  auto pixel_data = ::rerun::Collection<uint8_t>::borrow(data, data_size);
  auto depth_image = ::rerun::archetypes::DepthImage(std::move(pixel_data), {width, height}, datatype);

  if (meter > 0.0F && std::isfinite(meter)) {
    depth_image = std::move(depth_image).with_meter(::rerun::components::DepthMeter(meter));
  }

  rec.log(entity_path, depth_image);
  return true;
}

template <typename T>
static ::rerun::datatypes::TensorBuffer make_tensor_buffer(const uint8_t* data, size_t elements) {
  if VLIKELY (reinterpret_cast<uintptr_t>(data) % alignof(T) == 0U) {
    return ::rerun::datatypes::TensorBuffer(::rerun::Collection<T>::borrow(static_cast<const void*>(data), elements));
  }

  std::vector<T> values(elements);
  std::memcpy(values.data(), data, elements * sizeof(T));
  return ::rerun::datatypes::TensorBuffer(::rerun::Collection<T>::take_ownership(std::move(values)));
}

static bool log_camera_frame_tensor(::rerun::RecordingStream& rec, const std::string& entity_path,
                                    zerocopy::CameraFrame::Format format, const uint8_t* data, size_t data_size,
                                    uint32_t width, uint32_t height, size_t pixels) {
  enum class ElementType : uint8_t { kU8, kI8, kU16, kI16, kI32, kF32, kF64 };

  size_t channels = 0;
  size_t element_size = 0;
  ElementType element_type = ElementType::kU8;
  ::rerun::datatypes::TensorBuffer buffer;

  switch (format) {
    case zerocopy::CameraFrame::kFormatUint8C1:
    case zerocopy::CameraFrame::kFormatUint8C2:
    case zerocopy::CameraFrame::kFormatUint8C3:
    case zerocopy::CameraFrame::kFormatUint8C4:
      channels = static_cast<size_t>(format) - static_cast<size_t>(zerocopy::CameraFrame::kFormatUint8C1) + 1U;
      element_size = sizeof(uint8_t);
      break;
    case zerocopy::CameraFrame::kFormatInt8C1:
    case zerocopy::CameraFrame::kFormatInt8C2:
    case zerocopy::CameraFrame::kFormatInt8C3:
    case zerocopy::CameraFrame::kFormatInt8C4:
      channels = static_cast<size_t>(format) - static_cast<size_t>(zerocopy::CameraFrame::kFormatInt8C1) + 1U;
      element_size = sizeof(int8_t);
      element_type = ElementType::kI8;
      break;
    case zerocopy::CameraFrame::kFormatUint16C1:
    case zerocopy::CameraFrame::kFormatUint16C2:
    case zerocopy::CameraFrame::kFormatUint16C3:
    case zerocopy::CameraFrame::kFormatUint16C4:
      channels = static_cast<size_t>(format) - static_cast<size_t>(zerocopy::CameraFrame::kFormatUint16C1) + 1U;
      element_size = sizeof(uint16_t);
      element_type = ElementType::kU16;
      break;
    case zerocopy::CameraFrame::kFormatInt16C1:
    case zerocopy::CameraFrame::kFormatInt16C2:
    case zerocopy::CameraFrame::kFormatInt16C3:
    case zerocopy::CameraFrame::kFormatInt16C4:
      channels = static_cast<size_t>(format) - static_cast<size_t>(zerocopy::CameraFrame::kFormatInt16C1) + 1U;
      element_size = sizeof(int16_t);
      element_type = ElementType::kI16;
      break;
    case zerocopy::CameraFrame::kFormatInt32C1:
    case zerocopy::CameraFrame::kFormatInt32C2:
    case zerocopy::CameraFrame::kFormatInt32C3:
    case zerocopy::CameraFrame::kFormatInt32C4:
      channels = static_cast<size_t>(format) - static_cast<size_t>(zerocopy::CameraFrame::kFormatInt32C1) + 1U;
      element_size = sizeof(int32_t);
      element_type = ElementType::kI32;
      break;
    case zerocopy::CameraFrame::kFormatFloat32C1:
    case zerocopy::CameraFrame::kFormatFloat32C2:
    case zerocopy::CameraFrame::kFormatFloat32C3:
    case zerocopy::CameraFrame::kFormatFloat32C4:
      channels = static_cast<size_t>(format) - static_cast<size_t>(zerocopy::CameraFrame::kFormatFloat32C1) + 1U;
      element_size = sizeof(float);
      element_type = ElementType::kF32;
      break;
    case zerocopy::CameraFrame::kFormatFloat64C1:
    case zerocopy::CameraFrame::kFormatFloat64C2:
    case zerocopy::CameraFrame::kFormatFloat64C3:
    case zerocopy::CameraFrame::kFormatFloat64C4:
      channels = static_cast<size_t>(format) - static_cast<size_t>(zerocopy::CameraFrame::kFormatFloat64C1) + 1U;
      element_size = sizeof(double);
      element_type = ElementType::kF64;
      break;
    case zerocopy::CameraFrame::kFormatBayerRggb16:
    case zerocopy::CameraFrame::kFormatBayerBggr16:
    case zerocopy::CameraFrame::kFormatBayerGbrg16:
    case zerocopy::CameraFrame::kFormatBayerGrbg16:
      channels = 1U;
      element_size = sizeof(uint16_t);
      element_type = ElementType::kU16;
      break;
    default:
      return false;
  }

  if VUNLIKELY (channels == 0 || pixels > std::numeric_limits<size_t>::max() / channels) {
    return false;
  }

  const size_t elements = pixels * channels;

  if VUNLIKELY (elements > std::numeric_limits<size_t>::max() / element_size) {
    return false;
  }

  const size_t bytes = elements * element_size;

  if VUNLIKELY (bytes != data_size) {
    return false;
  }

  switch (element_type) {
    case ElementType::kU8:
      buffer = ::rerun::datatypes::TensorBuffer(::rerun::Collection<uint8_t>::borrow(data, elements));
      break;
    case ElementType::kI8:
      buffer = make_tensor_buffer<int8_t>(data, elements);
      break;
    case ElementType::kU16:
      buffer = make_tensor_buffer<uint16_t>(data, elements);
      break;
    case ElementType::kI16:
      buffer = make_tensor_buffer<int16_t>(data, elements);
      break;
    case ElementType::kI32:
      buffer = make_tensor_buffer<int32_t>(data, elements);
      break;
    case ElementType::kF32:
      buffer = make_tensor_buffer<float>(data, elements);
      break;
    case ElementType::kF64:
      buffer = make_tensor_buffer<double>(data, elements);
      break;
  }

  std::vector<uint64_t> shape;
  shape.reserve(channels > 1U ? 3U : 2U);
  shape.emplace_back(height);
  shape.emplace_back(width);

  if (channels > 1U) {
    shape.emplace_back(channels);
  }

  auto tensor = ::rerun::archetypes::Tensor(std::move(shape), std::move(buffer));

  if (channels > 1U) {
    tensor = std::move(tensor).with_dim_names({"height", "width", "channel"});
  } else {
    tensor = std::move(tensor).with_dim_names({"height", "width"});
  }

  rec.log(entity_path, tensor);

  return true;
}

Bytes RerunConverter::decode_plugin_binary(const Json& j, std::string_view base64_key, std::string_view array_key) {
  if (j.contains(base64_key) && j[base64_key].is_string()) {
    return Bytes::decode_from_base64(j[base64_key].get<std::string>());
  }

  if (j.contains(array_key) && j[array_key].is_array()) {
    const auto& data = j[array_key];
    auto bytes = Bytes::create(data.size());

    for (size_t i = 0; i < data.size(); ++i) {
      if (!data[i].is_number_integer() && !data[i].is_number_unsigned()) {
        return {};
      }

      if (data[i].is_number_unsigned()) {
        auto value = data[i].get<uint64_t>();

        if VUNLIKELY (value > std::numeric_limits<uint8_t>::max()) {
          return {};
        }

        bytes[i] = static_cast<uint8_t>(value);
      } else {
        auto value = data[i].get<int64_t>();

        if VUNLIKELY (value < 0 || value > std::numeric_limits<uint8_t>::max()) {
          return {};
        }

        bytes[i] = static_cast<uint8_t>(value);
      }
    }

    return bytes;
  }

  return {};
}

std::string RerunConverter::normalize_plugin_text(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return text;
}

std::string RerunConverter::infer_media_type(const Json& j) {
  auto media_type = j.value("media_type", std::string{});

  if (!media_type.empty()) {
    return media_type;
  }

  auto format = normalize_plugin_text(j.value("format", std::string{}));

  if (format == "jpeg" || format == "jpg") {
    return "image/jpeg";
  }

  if (format == "png") {
    return "image/png";
  }

  if (format == "webp") {
    return "image/webp";
  }

  if (format == "bmp") {
    return "image/bmp";
  }

  return {};
}

bool RerunConverter::resolve_plugin_image_size(const Json& j, uint32_t& width, uint32_t& height) {
  width = j.value("width", static_cast<uint32_t>(0));
  height = j.value("height", static_cast<uint32_t>(0));

  if ((width == 0 || height == 0) && j.contains("resolution") && j["resolution"].is_array() &&
      j["resolution"].size() >= 2) {
    width = j["resolution"][0].get<uint32_t>();
    height = j["resolution"][1].get<uint32_t>();
  }

  return width > 0 && height > 0;
}

void RerunConverter::log_view_coordinates_value(::rerun::RecordingStream& rec, const std::string& entity_path,
                                                std::string system) {
  std::transform(system.begin(), system.end(), system.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });

  if (system == "RUB" || system == "RIGHT_HAND_Y_UP") {
    rec.log_static(entity_path, ::rerun::archetypes::ViewCoordinates::RIGHT_HAND_Y_UP);
  } else if (system == "RDF" || system == "RIGHT_HAND_Z_UP") {
    rec.log_static(entity_path, system == "RDF" ? ::rerun::archetypes::ViewCoordinates::RDF
                                                : ::rerun::archetypes::ViewCoordinates::RIGHT_HAND_Z_UP);
  } else if (system == "RIGHT_HAND_Z_DOWN") {
    rec.log_static(entity_path, ::rerun::archetypes::ViewCoordinates::RIGHT_HAND_Z_DOWN);
  } else if (system == "LEFT_HAND_Y_UP") {
    rec.log_static(entity_path, ::rerun::archetypes::ViewCoordinates::LEFT_HAND_Y_UP);
  } else if (system == "LEFT_HAND_Z_UP") {
    rec.log_static(entity_path, ::rerun::archetypes::ViewCoordinates::LEFT_HAND_Z_UP);
  } else if (system == "FLU") {
    rec.log_static(entity_path, ::rerun::archetypes::ViewCoordinates::FLU);
  } else if (system == "FRD") {
    rec.log_static(entity_path, ::rerun::archetypes::ViewCoordinates::FRD);
  } else {
    rec.log_static(entity_path, ::rerun::archetypes::ViewCoordinates::RIGHT_HAND_Z_UP);
    MLOG_W("ViewCoordinates: unknown system '{}', defaulting to RIGHT_HAND_Z_UP", system);
  }
}

#ifdef VLINK_HAS_FBS_COMPILER  // NOLINT(readability-redundant-preprocessor)

double RerunConverter::get_fbs_double(const flatbuffers::Table& table, const reflection::Object& obj,
                                      const std::string& field_name) {
  return get_fbs_double(FbsObjectView{&obj, &table, nullptr}, field_name);
}

double RerunConverter::get_fbs_double(const FbsObjectView& view, const std::string& field_name) {
  const auto* field = view && view.object ? find_fbs_field(*view.object, field_name) : nullptr;

  if VLIKELY (field && is_fbs_numeric_type(field->type()->base_type())) {
    return get_fbs_field_as_double(view, *field);
  }

  return 0.0;
}

double RerunConverter::get_fbs_double(const flatbuffers::Table& table, const reflection::Object& obj,
                                      const std::string& field_name, const FieldMapping& mapping) {
  return get_fbs_double(FbsObjectView{&obj, &table, nullptr}, field_name, mapping);
}

double RerunConverter::get_fbs_double(const FbsObjectView& view, const std::string& field_name,
                                      const FieldMapping& mapping) {
  const auto* field = view && view.object ? find_fbs_field(*view.object, field_name) : nullptr;

  if VLIKELY (field && is_fbs_numeric_type(field->type()->base_type()) && is_fbs_field_present(view, *field)) {
    return get_fbs_field_as_double(view, *field);
  }

  double default_value = 0.0;
  return try_parse_numeric_default(mapping, default_value) ? default_value : 0.0;
}

std::string RerunConverter::get_fbs_string(const flatbuffers::Table& table, const reflection::Object& obj,
                                           const std::string& field_name, const reflection::Schema* schema) {
  return get_fbs_string(FbsObjectView{&obj, &table, nullptr}, field_name, schema);
}

std::string RerunConverter::get_fbs_string(const FbsObjectView& view, const std::string& field_name,
                                           const reflection::Schema* schema) {
  const auto* field = view && view.object ? find_fbs_field(*view.object, field_name) : nullptr;

  if VLIKELY (field && field->type()->base_type() == reflection::String) {
    return get_fbs_field_as_string(view, *field, schema);
  }

  return {};
}

std::string RerunConverter::get_fbs_string(const flatbuffers::Table& table, const reflection::Object& obj,
                                           const std::string& field_name, const FieldMapping& mapping,
                                           const reflection::Schema* schema) {
  return get_fbs_string(FbsObjectView{&obj, &table, nullptr}, field_name, mapping, schema);
}

std::string RerunConverter::get_fbs_string(const FbsObjectView& view, const std::string& field_name,
                                           const FieldMapping& mapping, const reflection::Schema* schema) {
  const auto* field = view && view.object ? find_fbs_field(*view.object, field_name) : nullptr;

  if VLIKELY (field && field->type()->base_type() == reflection::String && is_fbs_field_present(view, *field)) {
    return get_fbs_field_as_string(view, *field, schema);
  }

  return mapping.has_default_value ? mapping.default_value : std::string{};
}

#endif

RerunConverter::RerunConverter(const Config& config) : config_(config) {
  Bytes::init_memory_pool();
  init_proto_resolver();
  init_convert_plugin();

#ifdef VLINK_HAS_FBS_COMPILER  // NOLINT(readability-redundant-preprocessor)
  init_fbs_resolver();
#endif

  load_mappings();
}

RerunConverter::~RerunConverter() = default;

bool RerunConverter::init_proto_resolver() {
  bool has_resolver = false;

  auto& mgr = SchemaPluginManager::get(config_.schema_plugin_path);

  if (mgr.is_valid()) {
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

  // if VUNLIKELY (!has_resolver) {
  //   MLOG_W("No proto resolver available (no plugin, no proto_dir)");
  // }

  return has_resolver;
}

bool RerunConverter::init_convert_plugin() {
  return load_convert_plugin(config_.convert_plugin_path, config_.convert_plugin_config, convert_plugin_loader_,
                             convert_plugin_);
}

#ifdef VLINK_HAS_FBS_COMPILER  // NOLINT(readability-redundant-preprocessor)
bool RerunConverter::init_fbs_resolver() {
  bool has_resolver = schema_interface_ != nullptr;

  if (config_.fbs_dir.empty()) {
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

    if (sub_dir_str == root_dir_str) {
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

      if (fbs_parsers_.find(type_name) == fbs_parsers_.end()) {
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

bool RerunConverter::resolve_fbs_schema(const std::string& fbs_ser, std::string& schema_data) {
  std::lock_guard lock(mtx_);
  auto cache_iter = fbs_schema_cache_.find(fbs_ser);

  if VLIKELY (cache_iter != fbs_schema_cache_.end()) {
    schema_data = cache_iter->second;
    return true;
  }

  auto parser_iter = fbs_parsers_.find(fbs_ser);

  if (parser_iter == fbs_parsers_.end()) {
    if (fbs_not_found_.find(fbs_ser) != fbs_not_found_.end()) {
      return false;
    }

    if (schema_interface_) {
      auto schema = schema_interface_->search_schema(fbs_ser, SchemaType::kFlatbuffers);
      if (!schema.data.empty() && schema.schema_type == SchemaType::kFlatbuffers) {
        auto& cached = fbs_schema_cache_[fbs_ser];
        cached.assign(reinterpret_cast<const char*>(schema.data.data()), schema.data.size());
        schema_data = cached;
        return true;
      }
    }

    if (config_.fbs_dir.empty()) {
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

    bool found = false;

    for (const auto& fbs_file : fbs_files) {
      std::string schema_file;

      if VUNLIKELY (!flatbuffers::LoadFile(Helpers::path_to_string(fbs_file).c_str(), false, &schema_file)) {
        continue;
      }

      auto parser = std::make_unique<flatbuffers::Parser>();
      std::string sub_dir_str = Helpers::path_to_string(fbs_file.parent_path());

      if (sub_dir_str == root_dir_str) {
        if VUNLIKELY (!parser->Parse(schema_file.c_str(), include_dirs)) {
          continue;
        }
      } else {
        const char* full_dirs[] = {root_dir_str.c_str(), sub_dir_str.c_str(), nullptr};

        if VUNLIKELY (!parser->Parse(schema_file.c_str(), full_dirs)) {
          continue;
        }
      }

      if (parser->LookupStruct(fbs_ser)) {
        parser->SetRootType(fbs_ser.c_str());
        const size_t parser_index = fbs_parser_vec_.size();
        fbs_parser_vec_.emplace_back(std::move(parser));
        fbs_parsers_[fbs_ser] = parser_index;
        found = true;
        break;
      }
    }

    if VLIKELY (!found) {
      fbs_not_found_.insert(fbs_ser);
      return false;
    }

    parser_iter = fbs_parsers_.find(fbs_ser);
  }

  if VUNLIKELY (parser_iter->second >= fbs_parser_vec_.size()) {
    return false;
  }

  auto& parser = *fbs_parser_vec_[parser_iter->second];
  parser.SetRootType(fbs_ser.c_str());
  parser.Serialize();

  auto* buf_ptr = parser.builder_.GetBufferPointer();
  auto buf_size = parser.builder_.GetSize();

  if VUNLIKELY (!buf_ptr || buf_size == 0) {
    MLOG_W("Failed to serialize BFBS for: {}", fbs_ser);
    return false;
  }

  auto& cached = fbs_schema_cache_[fbs_ser];
  cached.assign(reinterpret_cast<const char*>(buf_ptr), buf_size);
  schema_data = cached;
  return true;
}
#endif

const google::protobuf::Descriptor* RerunConverter::find_proto_descriptor(const std::string& proto_name) {
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

std::unique_ptr<google::protobuf::Message> RerunConverter::deserialize_proto_message(const std::string& ser,
                                                                                     const Bytes& raw) {
  const auto* desc = find_proto_descriptor(ser);

  if VUNLIKELY (!desc) {
    MLOG_W("Descriptor not found: {}", ser);
    return nullptr;
  }

  const google::protobuf::Message* prototype = nullptr;

#ifdef VLINK_HAS_PROTO_COMPILER

  if (disk_factory_ && imported_proto_descriptors_.find(ser) != imported_proto_descriptors_.end()) {
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

void RerunConverter::load_mappings() {
  mappings_.clear();
  mapping_multi_index_.clear();

  for (const auto& file : config_.vlink_msgs) {
    if VUNLIKELY (!load_mapping_file(file)) {
      MLOG_W("Failed to load mapping: {}", file);
    }
  }

  for (const auto& m : mappings_) {
    mapping_multi_index_[m.ser].emplace_back(&m);
  }
}

bool RerunConverter::load_mapping_file(const std::string& path) {
  std::vector<RerunMap> loaded_mappings;

  auto ok = load_json_entries(
      path, "Mapping file not found", "Failed to parse mapping", [&loaded_mappings, &path](const Json& obj) -> bool {
        try {
          if VUNLIKELY (!obj.is_object()) {
            return false;
          }

          RerunMap mapping;
          mapping.ser = obj.value("ser", std::string());

          if VUNLIKELY (!parse_url_selector(obj, path, "mapping", mapping.url_selector)) {
            return false;
          }

          mapping.archetype = obj.value("archetype", std::string());
          mapping.encoding = obj.value("encoding", std::string("protobuf"));
          mapping.converter = obj.value("converter", std::string());
          mapping.timestamp_field = obj.value("timestamp_field", std::string());

          if VUNLIKELY (!parse_timestamp_unit(obj, "timestamp_unit", path, "mapping", mapping.timestamp_unit)) {
            return false;
          }

          if (obj.contains("topic")) {
            MLOG_W("vlink_msgs mapping in {} ignores topic; Rerun entity path always follows the runtime VLink URL",
                   path);
          }

          if VUNLIKELY (!parse_field_mappings(obj, path, "mapping", mapping.field_mappings)) {
            return false;
          }

          if VUNLIKELY (mapping.ser.empty()) {
            MLOG_W("Invalid mapping in {}: missing ser", path);
            return false;
          }

          if VUNLIKELY (mapping.encoding != "protobuf" && mapping.encoding != "flatbuffers" &&
                        mapping.encoding != "zerocopy") {
            MLOG_W("Invalid mapping in {}: unsupported source encoding {}", path, mapping.encoding);
            return false;
          }

          if VUNLIKELY (mapping.archetype.empty() && mapping.converter.empty()) {
            MLOG_W("Invalid mapping in {}: missing archetype or converter", path);
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

const std::vector<const RerunMap*>& RerunConverter::find_all_mappings(std::string_view url,
                                                                      const std::string& ser) const {
  struct MappingCache final {
    uint64_t owner_id{0};
    std::string url;
    std::string ser;
    std::vector<const RerunMap*> matches;
  };

  thread_local MappingCache cache;

  if (cache.owner_id == cache_owner_id_ && cache.url == url && cache.ser == ser) {
    return cache.matches;
  }

  cache.owner_id = cache_owner_id_;
  cache.url.assign(url.data(), url.size());
  cache.ser = ser;
  cache.matches.clear();

  auto mapping_iter = mapping_multi_index_.find(ser);

  if VLIKELY (mapping_iter != mapping_multi_index_.end()) {
    struct BestMatch final {
      int score{-1};
      const RerunMap* mapping{nullptr};
      bool ambiguous{false};
    };

    std::unordered_map<std::string, BestMatch> best_matches;

    for (const auto* mapping : mapping_iter->second) {
      auto score = score_url_selector(url, mapping->url_selector);

      if VUNLIKELY (score < 0) {
        continue;
      }

      const auto& key = mapping->converter.empty() ? mapping->archetype : mapping->converter;
      auto& match = best_matches[key];

      if VLIKELY (score > match.score) {
        match.score = score;
        match.mapping = mapping;
        match.ambiguous = false;
        continue;
      }

      if VUNLIKELY (score == match.score) {
        match.ambiguous = true;
      }
    }

    cache.matches.reserve(best_matches.size());

    for (const auto* mapping : mapping_iter->second) {
      const auto& key = mapping->converter.empty() ? mapping->archetype : mapping->converter;
      auto best_iter = best_matches.find(key);

      if VUNLIKELY (best_iter == best_matches.end()) {
        continue;
      }

      if VUNLIKELY (best_iter->second.ambiguous) {
        MLOG_W("Ambiguous rerun mapping: url={} ser={} target={}", url, ser, key);
        best_matches.erase(best_iter);
        continue;
      }

      if VLIKELY (best_iter->second.mapping == mapping) {
        cache.matches.emplace_back(mapping);
        best_matches.erase(best_iter);
      }
    }
  }

  return cache.matches;
}

void RerunConverter::convert_and_log(::rerun::RecordingStream& rec, const std::string& entity_path,
                                     std::string_view url, SchemaType schema_type, const std::string& ser,
                                     const Bytes& raw, int64_t fallback_timestamp_ns) {
  activate_cache_owner(cache_owner_id_);
  const auto zerocopy_type = zerocopy::MessageParser::detect_type(ser);
  zerocopy::MessageParser zerocopy_parser;

  auto apply_primary_timestamp = [this, &rec, fallback_timestamp_ns](int64_t message_timestamp_ns) {
    if VLIKELY (message_timestamp_ns >= 0) {
      apply_message_timestamp(rec, message_timestamp_ns);
      return;
    }

    if VLIKELY (fallback_timestamp_ns >= 0) {
      apply_message_timestamp(rec, fallback_timestamp_ns);
    }
  };

  const std::vector<const RerunMap*>* zerocopy_mappings = nullptr;
  bool needs_zerocopy_mapping = false;

  if (schema_type == SchemaType::kZeroCopy) {
    zerocopy_mappings = &find_all_mappings(url, ser);
    needs_zerocopy_mapping =
        std::any_of(zerocopy_mappings->begin(), zerocopy_mappings->end(), [](const RerunMap* mapping) {
          return mapping != nullptr && mapping->encoding == "zerocopy" && mapping->converter.empty() &&
                 !mapping->field_mappings.empty();
        });
  }

  if (!needs_zerocopy_mapping && schema_type == SchemaType::kZeroCopy &&
      zerocopy_type == zerocopy::MessageParser::Type::kCameraFrame) {
    zerocopy::CameraFrame frame;

    if VUNLIKELY (!(frame << raw)) {
      MLOG_W("Failed to deserialize CameraFrame");
      return;
    }

    apply_primary_timestamp(clamp_header_timestamp_ns(frame.header.time_meas));
    if VUNLIKELY (!log_camera_frame(rec, entity_path, frame)) {
      MLOG_W("log_camera_frame failed: path={} raw={}", entity_path, raw.size());
    }
    return;
  }

  if (!needs_zerocopy_mapping && schema_type == SchemaType::kZeroCopy &&
      zerocopy_type == zerocopy::MessageParser::Type::kPointCloud) {
    zerocopy::PointCloud point_cloud;

    if VUNLIKELY (!(point_cloud << raw)) {
      MLOG_W("Failed to deserialize PointCloud (raw={})", raw.size());
      return;
    }

    apply_primary_timestamp(clamp_header_timestamp_ns(point_cloud.header.time_meas));
    if VUNLIKELY (!log_point_cloud(rec, entity_path, point_cloud)) {
      MLOG_W("log_point_cloud failed: path={} raw={}", entity_path, raw.size());
    }
    return;
  }

  if (schema_type == SchemaType::kZeroCopy && zerocopy_type != zerocopy::MessageParser::Type::kUnknown) {
    if VUNLIKELY (!zerocopy_parser.parse(zerocopy_type, raw)) {
      MLOG_W("Failed to deserialize {}", zerocopy::MessageParser::type_name(zerocopy_type));
      return;
    }
  }

  if (schema_type == SchemaType::kZeroCopy) {
    const auto& mappings = *zerocopy_mappings;

    if (needs_zerocopy_mapping) {
      std::vector<std::string> mapping_sources;

      for (const auto* mapping : mappings) {
        if (mapping == nullptr) {
          continue;
        }

        mapping_sources.push_back(mapping->timestamp_field);

        for (const auto& field_mapping : mapping->field_mappings) {
          mapping_sources.push_back(field_mapping.source);
          mapping_sources.push_back(field_mapping.expression);
        }
      }

      auto message = make_zerocopy_message(zerocopy_parser, mapping_sources);

      if VUNLIKELY (!message) {
        MLOG_W("Failed to adapt zerocopy message for Rerun mapping: {}", ser);
        return;
      }

      int64_t mapped_timestamp_ns = -1;

      for (const auto* mapping : mappings) {
        if (mapping == nullptr || mapping->timestamp_field.empty()) {
          continue;
        }

        mapped_timestamp_ns = extract_proto_timestamp_ns(*message, mapping->timestamp_field, mapping->timestamp_unit);

        if (mapped_timestamp_ns >= 0) {
          break;
        }
      }

      apply_primary_timestamp(mapped_timestamp_ns);
      bool logged = false;

      for (const auto* mapping : mappings) {
        if (mapping == nullptr || mapping->encoding != "zerocopy" || !mapping->converter.empty() ||
            mapping->field_mappings.empty()) {
          continue;
        }

        const auto path = mappings.size() > 1 ? entity_path + "/" + mapping->archetype : entity_path;

        if (log_proto_with_mapping(rec, path, *mapping, *message)) {
          logged = true;
        }
      }

      if VLIKELY (logged) {
        return;
      }
    }
  }

  if (schema_type == SchemaType::kZeroCopy && zerocopy_type != zerocopy::MessageParser::Type::kUnknown &&
      zerocopy_type != zerocopy::MessageParser::Type::kProxyData) {
    const auto message_timestamp_ns = clamp_header_timestamp_ns(zerocopy_timestamp(zerocopy_parser));

    apply_primary_timestamp(message_timestamp_ns);

    switch (zerocopy_type) {
      case zerocopy::MessageParser::Type::kCameraFrame:
        if VUNLIKELY (!log_camera_frame(rec, entity_path, raw)) {
          MLOG_W("log_camera_frame failed: path={} raw={}", entity_path, raw.size());
        }

        break;

      case zerocopy::MessageParser::Type::kPointCloud:
        if VUNLIKELY (!log_point_cloud(rec, entity_path, raw)) {
          MLOG_W("log_point_cloud failed: path={} raw={}", entity_path, raw.size());
        }

        break;

      case zerocopy::MessageParser::Type::kRawData:
        log_raw_data(rec, entity_path, zerocopy_parser);
        break;

      case zerocopy::MessageParser::Type::kOccupancyGrid:
        if VUNLIKELY (!log_occupancy_grid(rec, entity_path, zerocopy_parser)) {
          MLOG_W("log_occupancy_grid failed: path={} raw={}", entity_path, raw.size());
        }

        break;

      case zerocopy::MessageParser::Type::kTensor:
        if VUNLIKELY (!log_tensor(rec, entity_path, zerocopy_parser)) {
          MLOG_W("log_tensor failed: path={} raw={}", entity_path, raw.size());
        }

        break;

      case zerocopy::MessageParser::Type::kObjectArray:
        if VUNLIKELY (!log_object_array(rec, entity_path, zerocopy_parser)) {
          MLOG_W("log_object_array failed: path={} raw={}", entity_path, raw.size());
        }

        break;

      case zerocopy::MessageParser::Type::kAudioFrame:
        if VUNLIKELY (!log_audio_frame(rec, entity_path, zerocopy_parser)) {
          MLOG_W("log_audio_frame failed: path={} raw={}", entity_path, raw.size());
        }

        break;
      default:
        break;
    }

    return;
  }

  const auto& all_mappings = find_all_mappings(url, ser);

  if VLIKELY (!all_mappings.empty()) {
    bool handled_send_time = false;
    int64_t send_time_ns = -1;
    size_t archetype_mapping_count = 0;

    for (const auto* mapping : all_mappings) {
      if VLIKELY (mapping->converter.empty()) {
        ++archetype_mapping_count;
        continue;
      }

      if (mapping->converter == "camera_frame") {
        apply_primary_timestamp(-1);
        log_camera_frame(rec, entity_path, raw);
        return;
      }

      if (mapping->converter == "point_cloud") {
        apply_primary_timestamp(-1);
        log_point_cloud(rec, entity_path, raw);
        return;
      }

      if (mapping->converter == "raw_data") {
        apply_primary_timestamp(-1);
        log_raw_data(rec, entity_path, raw);
        return;
      }

      if (mapping->converter == "occupancy_grid") {
        apply_primary_timestamp(-1);
        log_occupancy_grid(rec, entity_path, raw);
        return;
      }

      if (mapping->converter == "tensor") {
        apply_primary_timestamp(-1);
        log_tensor(rec, entity_path, raw);
        return;
      }

      if (mapping->converter == "object_array") {
        apply_primary_timestamp(-1);
        log_object_array(rec, entity_path, raw);
        return;
      }

      if (mapping->converter == "audio_frame") {
        apply_primary_timestamp(-1);
        log_audio_frame(rec, entity_path, raw);
        return;
      }

      if (mapping->converter == "send_time" && !mapping->timestamp_field.empty()) {
        handled_send_time = true;

        if (schema_type == SchemaType::kProtobuf && mapping->encoding == "protobuf") {
          auto msg = deserialize_proto_message(ser, raw);

          if VLIKELY (msg) {
            auto ts = extract_proto_timestamp_ns(*msg, mapping->timestamp_field, mapping->timestamp_unit);

            if (ts >= 0) {
              send_time_ns = ts;
            }
          }
        }
#ifdef VLINK_HAS_FBS_COMPILER  // NOLINT(readability-redundant-preprocessor)
        else if (schema_type == SchemaType::kFlatbuffers && mapping->encoding == "flatbuffers") {
          const reflection::Schema* schema = nullptr;

          if (resolve_thread_local_fbs_schema(
                  ser, cache_owner_id_,
                  [this](const std::string& type_name, std::string& schema_data) {
                    return resolve_fbs_schema(type_name, schema_data);
                  },
                  schema) &&
              schema != nullptr && schema->root_table() != nullptr &&
              verify_fbs_payload(*schema, raw, ser, "Rerun send_time timestamp")) {
            const auto* root_table = flatbuffers::GetAnyRoot(raw.data());

            if (root_table) {
              auto ts = extract_fbs_timestamp_ns(*root_table, *schema->root_table(), *schema, mapping->timestamp_field,
                                                 mapping->timestamp_unit);

              if (ts >= 0) {
                send_time_ns = ts;
              }
            }
          }
        }
#endif

        continue;
      }
    }

    if VUNLIKELY (handled_send_time && archetype_mapping_count == 0) {
      std::lock_guard lock(mtx_);

      if (warned_types_.insert("standalone_send_time:" + ser).second) {
        MLOG_W("Rerun send_time mapping for '{}' has no archetype mapping; timeline values only apply to logged data",
               ser);
      }

      return;
    }

    if VUNLIKELY (handled_send_time && send_time_ns >= 0) {
      rec.set_time_duration_nanos("vlink_time", send_time_ns);
    }

    int64_t message_timestamp_ns = -1;

    bool need_proto = std::any_of(all_mappings.begin(), all_mappings.end(), [](const RerunMap* mapping) {
      return mapping->converter.empty() && mapping->encoding == "protobuf";
    });

    std::unique_ptr<google::protobuf::Message> msg;

    if VLIKELY (need_proto && schema_type == SchemaType::kProtobuf && find_proto_descriptor(ser)) {
      msg = deserialize_proto_message(ser, raw);
    }

    for (const auto* mapping : all_mappings) {
      if (!mapping->converter.empty() || mapping->timestamp_field.empty()) {
        continue;
      }

      if (schema_type == SchemaType::kProtobuf && mapping->encoding == "protobuf" && msg) {
        auto ts = extract_proto_timestamp_ns(*msg, mapping->timestamp_field, mapping->timestamp_unit);

        if VLIKELY (ts >= 0) {
          message_timestamp_ns = ts;
          break;
        }

        continue;
      }

#ifdef VLINK_HAS_FBS_COMPILER  // NOLINT(readability-redundant-preprocessor)

      if (schema_type == SchemaType::kFlatbuffers && mapping->encoding == "flatbuffers") {
        const reflection::Schema* schema = nullptr;

        if (resolve_thread_local_fbs_schema(
                ser, cache_owner_id_,
                [this](const std::string& type_name, std::string& schema_data) {
                  return resolve_fbs_schema(type_name, schema_data);
                },
                schema) &&
            schema != nullptr && schema->root_table() != nullptr &&
            verify_fbs_payload(*schema, raw, ser, "Rerun message timestamp")) {
          const auto* root_table = flatbuffers::GetAnyRoot(raw.data());

          if (root_table) {
            auto ts = extract_fbs_timestamp_ns(*root_table, *schema->root_table(), *schema, mapping->timestamp_field,
                                               mapping->timestamp_unit);

            if VLIKELY (ts >= 0) {
              message_timestamp_ns = ts;
            }
          }
        }

        if VLIKELY (message_timestamp_ns >= 0) {
          break;
        }
      }
#endif
    }

    apply_primary_timestamp(message_timestamp_ns);

    bool any_logged = false;

    for (const auto* mapping : all_mappings) {
      if VUNLIKELY (!mapping->converter.empty()) {
        continue;
      }

      auto log_path = archetype_mapping_count > 1 ? entity_path + "/" + mapping->archetype : entity_path;

      if (schema_type == SchemaType::kProtobuf && mapping->encoding == "protobuf") {
        if VLIKELY (msg && log_proto_with_mapping(rec, log_path, *mapping, *msg)) {
          any_logged = true;
          continue;
        }
      } else if (schema_type == SchemaType::kFlatbuffers && mapping->encoding == "flatbuffers") {
#ifdef VLINK_HAS_FBS_COMPILER  // NOLINT(readability-redundant-preprocessor)

        if (log_fbs_with_mapping(rec, log_path, *mapping, ser, raw)) {
          any_logged = true;
          continue;
        }
#endif
      }
    }

    if VLIKELY (any_logged) {
      return;
    }

    if VLIKELY (msg) {
      rec.log(entity_path, ::rerun::TextLog(msg->ShortDebugString()));
      return;
    }
  }

  if VUNLIKELY (convert_plugin_ && convert_plugin_->can_convert(ser, ConvertPluginInterface::Target::kRerun)) {
    auto plugin_ts = convert_plugin_->get_timestamp(ser, raw, ConvertPluginInterface::Target::kRerun);

    ConvertPluginInterface::SchemaInfo schema_info;

    bool has_schema = convert_plugin_->get_schema(ser, ConvertPluginInterface::Target::kRerun, schema_info);

    if VUNLIKELY (has_schema && schema_info.type_name == "SendTime") {
      std::lock_guard lock(mtx_);

      if (warned_types_.insert("plugin_send_time:" + ser).second) {
        MLOG_W("Rerun plugin SendTime for '{}' has no logged archetype; standalone timeline updates are unsupported",
               ser);
      }

      return;
    }

    Bytes payload;

    if VUNLIKELY (!convert_plugin_->convert(ser, raw, ConvertPluginInterface::Target::kRerun, payload)) {
      MLOG_W("Convert plugin convert() failed for: {}", ser);
      return;
    }

    if VUNLIKELY (!has_schema) {
      MLOG_W("Convert plugin get_schema() failed for: {}", ser);
      return;
    }

    if VUNLIKELY (schema_info.encoding != "json") {
      MLOG_W("Convert plugin unsupported encoding '{}' for: {}", schema_info.encoding, ser);
      return;
    }

    if VUNLIKELY (payload.empty()) {
      return;
    }

    try {
      auto json_data = Json::parse(reinterpret_cast<const char*>(payload.data()),
                                   reinterpret_cast<const char*>(payload.data()) + payload.size());

      apply_primary_timestamp(plugin_ts);

      if VUNLIKELY (!log_plugin_json(rec, entity_path, schema_info.type_name, json_data)) {
        MLOG_W("Convert plugin log_plugin_json failed for archetype '{}' type '{}'", schema_info.type_name, ser);
      }
    } catch (const std::exception& e) {
      MLOG_W("Convert plugin JSON parse error for {}: {}", ser, e.what());
    }

    return;
  }

  if VUNLIKELY (is_text_ser(ser)) {
    apply_primary_timestamp(-1);
    std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());
    rec.log(entity_path, ::rerun::TextLog(text));
    return;
  }

  if (schema_type == SchemaType::kProtobuf) {
    auto unmapped_msg = deserialize_proto_message(ser, raw);

    if VLIKELY (unmapped_msg) {
      apply_primary_timestamp(-1);
      rec.log(entity_path, ::rerun::TextLog(unmapped_msg->ShortDebugString()));
      return;
    }
  }

  {
    std::lock_guard lock(mtx_);

    if (warned_types_.find(ser) == warned_types_.end()) {
      warned_types_.insert(ser);
      MLOG_W("Unknown ser type: {} (size={}, no mapping)", ser, raw.size());
    }
  }

  apply_primary_timestamp(-1);
  rec.log(entity_path, ::rerun::TextLog("[" + ser + "] raw " + std::to_string(raw.size()) + " bytes"));
}

bool RerunConverter::log_camera_frame(::rerun::RecordingStream& rec, const std::string& entity_path,
                                      const zerocopy::CameraFrame& frame) {
  if VUNLIKELY (frame.data() == nullptr || frame.size() == 0) {
    return false;
  }
  const auto fmt = frame.format();

  std::string media_type;

  switch (fmt) {
    case zerocopy::CameraFrame::kFormatJpeg:
      media_type = "image/jpeg";
      break;
    case zerocopy::CameraFrame::kFormatPng:
      media_type = "image/png";
      break;
    case zerocopy::CameraFrame::kFormatMjpeg:
      media_type = "image/jpeg";
      break;
    case zerocopy::CameraFrame::kFormatWebp:
      media_type = "image/webp";
      break;
    default:
      break;
  }

  const auto* data_ptr = frame.data();
  const size_t data_size = frame.size();

  if VUNLIKELY (fmt == zerocopy::CameraFrame::kFormatH266) {
    MLOG_W("Rerun VideoStream does not support H.266 CameraFrame samples");
    return false;
  }

  if (fmt == zerocopy::CameraFrame::kFormatH264 || fmt == zerocopy::CameraFrame::kFormatH265 ||
      fmt == zerocopy::CameraFrame::kFormatAv1) {
    if VUNLIKELY (frame.stream() == zerocopy::CameraFrame::kStreamB) {
      MLOG_W("Rerun VideoStream does not support B-frames");
      return false;
    }

    auto codec = ::rerun::components::VideoCodec::H264;

    if (fmt == zerocopy::CameraFrame::kFormatH265) {
      codec = ::rerun::components::VideoCodec::H265;
    } else if (fmt == zerocopy::CameraFrame::kFormatAv1) {
      codec = ::rerun::components::VideoCodec::AV1;
    }

    auto blob = ::rerun::Collection<uint8_t>::borrow(data_ptr, data_size);
    rec.log(entity_path,
            ::rerun::archetypes::VideoStream(codec).with_sample(::rerun::components::VideoSample(std::move(blob))));
    return true;
  }

  if (fmt == zerocopy::CameraFrame::kFormatJpeg || fmt == zerocopy::CameraFrame::kFormatPng ||
      fmt == zerocopy::CameraFrame::kFormatMjpeg || fmt == zerocopy::CameraFrame::kFormatWebp) {
    auto blob = ::rerun::Collection<uint8_t>::borrow(data_ptr, data_size);

    rec.log(entity_path,
            ::rerun::archetypes::EncodedImage::from_bytes(blob, ::rerun::components::MediaType(media_type)));
    return true;
  }

  const auto width = frame.width();
  const auto height = frame.height();

  if VUNLIKELY (width == 0 || height == 0) {
    MLOG_W("CameraFrame raw pixel format but width={} height={}, skipping", width, height);
    return false;
  }

  size_t pixels = 0;

  if VUNLIKELY (!mul_size(static_cast<size_t>(width), static_cast<size_t>(height), pixels)) {
    return false;
  }

  size_t bytes = 0;

  switch (fmt) {
    case zerocopy::CameraFrame::kFormatRgb888Packed:
      if VUNLIKELY (!image_size(pixels, 3U, data_size, bytes)) {
        return false;
      }

      return log_raw_image(rec, entity_path, data_ptr, bytes, width, height, ::rerun::datatypes::ColorModel::RGB);

    case zerocopy::CameraFrame::kFormatBgr888Packed:
      if VUNLIKELY (!image_size(pixels, 3U, data_size, bytes)) {
        return false;
      }

      return log_raw_image(rec, entity_path, data_ptr, bytes, width, height, ::rerun::datatypes::ColorModel::BGR);

    case zerocopy::CameraFrame::kFormatRgba8888Packed:
      if VUNLIKELY (!image_size(pixels, 4U, data_size, bytes)) {
        return false;
      }

      return log_raw_image(rec, entity_path, data_ptr, bytes, width, height, ::rerun::datatypes::ColorModel::RGBA);

    case zerocopy::CameraFrame::kFormatBgra8888Packed:
      if VUNLIKELY (!image_size(pixels, 4U, data_size, bytes)) {
        return false;
      }

      return log_raw_image(rec, entity_path, data_ptr, bytes, width, height, ::rerun::datatypes::ColorModel::BGRA);

    case zerocopy::CameraFrame::kFormatRgb888Planar: {
      if VUNLIKELY (!image_size(pixels, 3U, data_size, bytes)) {
        return false;
      }

      std::vector<uint8_t> interleaved(bytes);
      const auto* r_plane = data_ptr;
      const auto* g_plane = data_ptr + pixels;
      const auto* b_plane = data_ptr + pixels * 2U;

      for (size_t i = 0; i < pixels; ++i) {
        interleaved[i * 3U + 0U] = r_plane[i];
        interleaved[i * 3U + 1U] = g_plane[i];
        interleaved[i * 3U + 2U] = b_plane[i];
      }

      return log_raw_image(rec, entity_path, std::move(interleaved), width, height,
                           ::rerun::datatypes::ColorModel::RGB);
    }

    case zerocopy::CameraFrame::kFormatYuv420:
      if VUNLIKELY ((width % 2U) != 0 || (height % 2U) != 0) {
        return false;
      }

      return log_pixel_image(rec, entity_path, data_ptr, data_size, width, height,
                             ::rerun::datatypes::PixelFormat::Y_U_V12_FullRange);

    case zerocopy::CameraFrame::kFormatNv12:
      if VUNLIKELY ((width % 2U) != 0 || (height % 2U) != 0) {
        return false;
      }

      return log_pixel_image(rec, entity_path, data_ptr, data_size, width, height,
                             ::rerun::datatypes::PixelFormat::NV12);

    case zerocopy::CameraFrame::kFormatNv21:
      if VUNLIKELY ((width % 2U) != 0 || (height % 2U) != 0 ||
                    pixels > std::numeric_limits<size_t>::max() - pixels / 2U || pixels + pixels / 2U != data_size) {
        return false;
      }

      return log_raw_image(rec, entity_path, data_ptr, pixels, width, height, ::rerun::datatypes::ColorModel::L);

    case zerocopy::CameraFrame::kFormatYuv422:
      if VUNLIKELY ((width % 2U) != 0) {
        return false;
      }

      return log_pixel_image(rec, entity_path, data_ptr, data_size, width, height,
                             ::rerun::datatypes::PixelFormat::Y_U_V16_FullRange);

    case zerocopy::CameraFrame::kFormatYuv444:
      if VUNLIKELY (!image_size(pixels, 3U, data_size, bytes)) {
        return false;
      }

      return log_raw_image(rec, entity_path, data_ptr, pixels, width, height, ::rerun::datatypes::ColorModel::L);

    case zerocopy::CameraFrame::kFormatYuyv:
      if VUNLIKELY ((width % 2U) != 0) {
        return false;
      }

      return log_pixel_image(rec, entity_path, data_ptr, data_size, width, height,
                             ::rerun::datatypes::PixelFormat::YUY2);

    case zerocopy::CameraFrame::kFormatYvyu: {
      if VUNLIKELY ((width % 2U) != 0) {
        return false;
      }

      std::vector<uint8_t> gray;

      if VUNLIKELY (!copy_channel(data_ptr, data_size, pixels, 2U, 0U, gray)) {
        return false;
      }

      return log_raw_image(rec, entity_path, std::move(gray), width, height, ::rerun::datatypes::ColorModel::L);
    }

    case zerocopy::CameraFrame::kFormatUyvy:
    case zerocopy::CameraFrame::kFormatVyuy: {
      if VUNLIKELY ((width % 2U) != 0) {
        return false;
      }

      std::vector<uint8_t> gray;

      if VUNLIKELY (!copy_channel(data_ptr, data_size, pixels, 2U, 1U, gray)) {
        return false;
      }

      return log_raw_image(rec, entity_path, std::move(gray), width, height, ::rerun::datatypes::ColorModel::L);
    }

    case zerocopy::CameraFrame::kFormatMono8:
    case zerocopy::CameraFrame::kFormatBayerRggb8:
    case zerocopy::CameraFrame::kFormatBayerBggr8:
    case zerocopy::CameraFrame::kFormatBayerGbrg8:
    case zerocopy::CameraFrame::kFormatBayerGrbg8:
      if VUNLIKELY (pixels != data_size) {
        return false;
      }

      return log_raw_image(rec, entity_path, data_ptr, pixels, width, height, ::rerun::datatypes::ColorModel::L);

    case zerocopy::CameraFrame::kFormatUint8C1:
      return log_typed_image(rec, entity_path, data_ptr, data_size, width, height, ::rerun::datatypes::ColorModel::L,
                             ::rerun::datatypes::ChannelDatatype::U8);

    case zerocopy::CameraFrame::kFormatInt8C1:
      return log_typed_image(rec, entity_path, data_ptr, data_size, width, height, ::rerun::datatypes::ColorModel::L,
                             ::rerun::datatypes::ChannelDatatype::I8);

    case zerocopy::CameraFrame::kFormatUint8C2:
    case zerocopy::CameraFrame::kFormatUint8C3:
    case zerocopy::CameraFrame::kFormatUint8C4:
    case zerocopy::CameraFrame::kFormatInt8C2:
    case zerocopy::CameraFrame::kFormatInt8C3:
    case zerocopy::CameraFrame::kFormatInt8C4:
      return log_camera_frame_tensor(rec, entity_path, fmt, data_ptr, data_size, width, height, pixels);

    case zerocopy::CameraFrame::kFormatMono16:
      return log_typed_image(rec, entity_path, data_ptr, data_size, width, height, ::rerun::datatypes::ColorModel::L,
                             ::rerun::datatypes::ChannelDatatype::U16);

    case zerocopy::CameraFrame::kFormatUint16C1:
      return log_typed_image(rec, entity_path, data_ptr, data_size, width, height, ::rerun::datatypes::ColorModel::L,
                             ::rerun::datatypes::ChannelDatatype::U16);

    case zerocopy::CameraFrame::kFormatInt16C1:
      return log_typed_image(rec, entity_path, data_ptr, data_size, width, height, ::rerun::datatypes::ColorModel::L,
                             ::rerun::datatypes::ChannelDatatype::I16);

    case zerocopy::CameraFrame::kFormatInt32C1:
      return log_typed_image(rec, entity_path, data_ptr, data_size, width, height, ::rerun::datatypes::ColorModel::L,
                             ::rerun::datatypes::ChannelDatatype::I32);

    case zerocopy::CameraFrame::kFormatFloat32C1:
      return log_typed_image(rec, entity_path, data_ptr, data_size, width, height, ::rerun::datatypes::ColorModel::L,
                             ::rerun::datatypes::ChannelDatatype::F32);

    case zerocopy::CameraFrame::kFormatFloat64C1:
      return log_typed_image(rec, entity_path, data_ptr, data_size, width, height, ::rerun::datatypes::ColorModel::L,
                             ::rerun::datatypes::ChannelDatatype::F64);

    case zerocopy::CameraFrame::kFormatBayerRggb16:
    case zerocopy::CameraFrame::kFormatBayerBggr16:
    case zerocopy::CameraFrame::kFormatBayerGbrg16:
    case zerocopy::CameraFrame::kFormatBayerGrbg16:
      return log_typed_image(rec, entity_path, data_ptr, data_size, width, height, ::rerun::datatypes::ColorModel::L,
                             ::rerun::datatypes::ChannelDatatype::U16);

    case zerocopy::CameraFrame::kFormatUint16C2:
    case zerocopy::CameraFrame::kFormatUint16C3:
    case zerocopy::CameraFrame::kFormatUint16C4:
    case zerocopy::CameraFrame::kFormatInt16C2:
    case zerocopy::CameraFrame::kFormatInt16C3:
    case zerocopy::CameraFrame::kFormatInt16C4:
    case zerocopy::CameraFrame::kFormatInt32C2:
    case zerocopy::CameraFrame::kFormatInt32C3:
    case zerocopy::CameraFrame::kFormatInt32C4:
    case zerocopy::CameraFrame::kFormatFloat32C2:
    case zerocopy::CameraFrame::kFormatFloat32C3:
    case zerocopy::CameraFrame::kFormatFloat32C4:
    case zerocopy::CameraFrame::kFormatFloat64C2:
    case zerocopy::CameraFrame::kFormatFloat64C3:
    case zerocopy::CameraFrame::kFormatFloat64C4:
      return log_camera_frame_tensor(rec, entity_path, fmt, data_ptr, data_size, width, height, pixels);

    default:
      return false;
  }
}

bool RerunConverter::log_camera_frame(::rerun::RecordingStream& rec, const std::string& entity_path, const Bytes& raw) {
  zerocopy::CameraFrame frame;

  if VUNLIKELY (!(frame << raw)) {
    MLOG_W("Failed to deserialize CameraFrame");
    return false;
  }

  return log_camera_frame(rec, entity_path, frame);
}

bool RerunConverter::log_point_cloud(::rerun::RecordingStream& rec, const std::string& entity_path,
                                     const zerocopy::PointCloud& point_cloud) {
  const PointCloudView view(point_cloud);
  if VUNLIKELY (!view.valid()) return false;
  const size_t point_count = view.size();

  if VUNLIKELY (point_count == 0) {
    return false;
  }

  const auto& fields = view.fields();

  if VUNLIKELY (fields.empty()) {
    return false;
  }

  const auto find_field = [&view](std::string_view name) -> const PointCloudFieldView* { return view.find(name); };
  const auto valid_field = [&find_field](std::string_view name) -> const PointCloudFieldView* {
    const auto* iterator = find_field(name);
    if (iterator == nullptr) {
      return nullptr;
    }

    if VUNLIKELY (iterator->field.storage_size == 0) {
      return nullptr;
    }

    if (iterator->field.native_type == zerocopy::PointCloud::kUnknownType &&
        iterator->field.storage_size != sizeof(uint8_t) && iterator->field.storage_size != sizeof(int16_t) &&
        iterator->field.storage_size != sizeof(float) && iterator->field.storage_size != sizeof(double)) {
      return nullptr;
    }

    return iterator;
  };

  const auto* x_field = valid_field("x");
  const auto* y_field = valid_field("y");
  const auto* z_field = valid_field("z");
  const bool has_xyz = x_field != nullptr && y_field != nullptr && z_field != nullptr;

  if VUNLIKELY (!has_xyz) {
    MLOG_W("PointCloud missing x/y/z fields");
    return false;
  }

  const auto* r_field = valid_field("r");
  const auto* g_field = valid_field("g");
  const auto* b_field = valid_field("b");
  const bool has_rgb = r_field != nullptr && g_field != nullptr && b_field != nullptr;
  const auto* a_field = has_rgb ? valid_field("a") : nullptr;
  const auto* intensity_field = !has_rgb ? valid_field("intensity") : nullptr;
  const auto* radius_field = valid_field("radius");
  const auto* class_id_field = valid_field("class_id");
  const auto* label_field = valid_field("label");
  const bool has_alpha = a_field != nullptr;
  const bool has_intensity = intensity_field != nullptr;
  const bool has_radius = radius_field != nullptr;
  const bool has_class_id = class_id_field != nullptr;
  const bool has_label = label_field != nullptr;

  double intensity_min = std::numeric_limits<double>::max();
  double intensity_max = std::numeric_limits<double>::lowest();
  double intensity_scale = 1.0;
  bool scaled_intensity = false;

  if (has_intensity) {
    for (size_t i = 0; i < point_count; ++i) {
      double value = 0.0;

      if VUNLIKELY (!view.numeric(i, *intensity_field, value) || !std::isfinite(value)) {
        return false;
      }

      intensity_min = std::min(intensity_min, value);
      intensity_max = std::max(intensity_max, value);
    }

    const double intensity_range = intensity_max - intensity_min;

    if (intensity_max <= intensity_min) {
      intensity_min = 0.0;
      intensity_max = 1.0;
    } else if VUNLIKELY (!std::isfinite(intensity_range) || intensity_range < std::numeric_limits<double>::min()) {
      intensity_scale = std::max(std::abs(intensity_min), std::abs(intensity_max));
      intensity_min /= intensity_scale;
      intensity_max /= intensity_scale;
      scaled_intensity = true;
    }
  }

  std::vector<::rerun::Position3D> positions(point_count);
  std::vector<::rerun::Color> colors;
  std::vector<::rerun::Radius> radii;
  std::vector<::rerun::components::ClassId> class_ids;
  std::vector<::rerun::components::Text> labels;

  if (has_rgb || has_intensity) {
    colors.resize(point_count);
  }

  if (has_radius) {
    radii.resize(point_count);
  }

  if (has_class_id) {
    class_ids.resize(point_count);
  }

  // NOLINTNEXTLINE(readability-redundant-parentheses)
  double inv_range = (has_intensity) ? 1.0 / (intensity_max - intensity_min) : 0.0;

  for (size_t i = 0; i < point_count; ++i) {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    if VUNLIKELY (!view.numeric(i, *x_field, x) || !view.numeric(i, *y_field, y) || !view.numeric(i, *z_field, z)) {
      return false;
    }

    constexpr auto kFloatMax = static_cast<double>(std::numeric_limits<float>::max());

    if VUNLIKELY (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || std::abs(x) > kFloatMax ||
                  std::abs(y) > kFloatMax || std::abs(z) > kFloatMax) {
      return false;
    }

    positions[i] = ::rerun::Position3D(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));

    if (has_rgb) {
      double red = 0.0;
      double green = 0.0;
      double blue = 0.0;

      if VUNLIKELY (!view.numeric(i, *r_field, red) || !view.numeric(i, *g_field, green) ||
                    !view.numeric(i, *b_field, blue)) {
        return false;
      }

      const auto r = checked_unsigned_cast<uint8_t>(red);
      const auto g = checked_unsigned_cast<uint8_t>(green);
      const auto b = checked_unsigned_cast<uint8_t>(blue);

      if (has_alpha) {
        double alpha = 0.0;

        if VUNLIKELY (!view.numeric(i, *a_field, alpha)) {
          return false;
        }

        const auto a = checked_unsigned_cast<uint8_t>(alpha);
        colors[i] = ::rerun::Color(r, g, b, a);
      } else {
        colors[i] = ::rerun::Color(r, g, b);
      }
    } else if (has_intensity) {
      double value = 0.0;

      if VUNLIKELY (!view.numeric(i, *intensity_field, value) || !std::isfinite(value)) {
        return false;
      }

      if VUNLIKELY (scaled_intensity) {
        value /= intensity_scale;
      }

      auto val = std::clamp((value - intensity_min) * inv_range, 0.0, 1.0);

      uint8_t r = 0;
      uint8_t g = 0;
      uint8_t b = 0;

      if (val < 0.25) {
        auto t = val * 4.0;
        g = static_cast<uint8_t>(255.0 * t);
        b = 255;
      } else if (val < 0.5) {
        auto t = (val - 0.25) * 4.0;
        g = 255;
        b = static_cast<uint8_t>(255.0 * (1.0 - t));
      } else if (val < 0.75) {
        auto t = (val - 0.5) * 4.0;
        r = static_cast<uint8_t>(255.0 * t);
        g = 255;
      } else {
        auto t = (val - 0.75) * 4.0;
        r = 255;
        g = static_cast<uint8_t>(255.0 * (1.0 - t));
      }

      colors[i] = ::rerun::Color(r, g, b);
    }

    if (has_radius) {
      double radius = 0.0;

      if VUNLIKELY (!view.numeric(i, *radius_field, radius) || !std::isfinite(radius) || std::abs(radius) > kFloatMax) {
        return false;
      }

      radii[i] = ::rerun::Radius(static_cast<float>(radius));
    }

    if (has_class_id) {
      double class_id = 0.0;

      if VUNLIKELY (!view.numeric(i, *class_id_field, class_id)) {
        return false;
      }

      auto cid = checked_unsigned_cast<uint16_t>(class_id);
      class_ids[i] = ::rerun::components::ClassId(cid);
    }
  }

  if (has_label) {
    labels.reserve(point_count);

    for (size_t i = 0; i < point_count; ++i) {
      zerocopy::MessageParser::Value label;

      if VUNLIKELY (!view.value(i, *label_field, label)) {
        return false;
      }

      if (const auto* value = std::get_if<int64_t>(&label)) {
        labels.emplace_back(std::to_string(*value));
      } else if (const auto* value = std::get_if<uint64_t>(&label)) {
        labels.emplace_back(std::to_string(*value));
      } else if (const auto* value = std::get_if<double>(&label)) {
        constexpr double kInt64Lower = -0x1p63;
        constexpr double kInt64Upper = 0x1p63;

        if VUNLIKELY (!std::isfinite(*value) || *value < kInt64Lower || *value >= kInt64Upper) {
          return false;
        }

        labels.emplace_back(std::to_string(static_cast<int64_t>(*value)));
      } else {
        return false;
      }
    }
  }

  auto points = ::rerun::archetypes::Points3D(std::move(positions));

  if (!colors.empty()) {
    points = std::move(points).with_colors(std::move(colors));
  }

  if (!radii.empty()) {
    points = std::move(points).with_radii(std::move(radii));
  }

  if (!class_ids.empty()) {
    points = std::move(points).with_class_ids(std::move(class_ids));
  }

  if (!labels.empty()) {
    points = std::move(points).with_labels(std::move(labels));
    points = std::move(points).with_show_labels(::rerun::components::ShowLabels(false));
  }

  rec.log(entity_path, points);

  return true;
}

bool RerunConverter::log_point_cloud(::rerun::RecordingStream& rec, const std::string& entity_path, const Bytes& raw) {
  zerocopy::PointCloud point_cloud;

  if VUNLIKELY (!(point_cloud << raw)) {
    MLOG_W("Failed to deserialize PointCloud (raw={})", raw.size());
    return false;
  }

  return log_point_cloud(rec, entity_path, point_cloud);
}

bool RerunConverter::log_raw_data(::rerun::RecordingStream& rec, const std::string& entity_path,
                                  const zerocopy::MessageParser& parser) {
  zerocopy::MessageParser::Value value;

  if VUNLIKELY (!parser.value("data", value)) {
    return false;
  }

  const auto* data = std::get_if<Bytes>(&value);

  if VUNLIKELY (data == nullptr || data->empty()) {
    return false;
  }

  rec.log(entity_path, ::rerun::archetypes::Asset3D(::rerun::components::Blob(
                           ::rerun::Collection<uint8_t>::borrow(data->data(), data->size()))));

  return true;
}

bool RerunConverter::log_raw_data(::rerun::RecordingStream& rec, const std::string& entity_path, const Bytes& raw) {
  zerocopy::MessageParser parser;

  if VUNLIKELY (!parser.parse(zerocopy::MessageParser::Type::kRawData, raw)) {
    MLOG_W("Failed to deserialize RawData");
    return false;
  }

  return log_raw_data(rec, entity_path, parser);
}

bool RerunConverter::log_occupancy_grid(::rerun::RecordingStream& rec, const std::string& entity_path,
                                        const zerocopy::MessageParser& parser) {
  double width = 0;
  double height = 0;
  double cell_type_value = 0;
  size_t cell_size = 0;
  size_t payload_size = 0;

  if VUNLIKELY (!parser.numeric("width", width) || !parser.numeric("height", height) ||
                !parser.numeric("cell_type", cell_type_value) || !read_parser_size(parser, "cell_size", cell_size) ||
                !read_parser_size(parser, "size", payload_size)) {
    MLOG_W("OccupancyGrid metadata is incomplete");
    return false;
  }

  const auto w = static_cast<uint32_t>(width);
  const auto h = static_cast<uint32_t>(height);

  if VUNLIKELY (w == 0 || h == 0) {
    MLOG_W("OccupancyGrid invalid: width={} height={}", w, h);
    return false;
  }

  size_t cell_count = 0;
  size_t expected_size = 0;

  if VUNLIKELY (cell_size == 0 || !mul_size(static_cast<size_t>(w), static_cast<size_t>(h), cell_count) ||
                !mul_size(cell_count, cell_size, expected_size) || payload_size != expected_size) {
    MLOG_W("OccupancyGrid payload size mismatch: width={} height={} cell_size={} expected={} actual={}", w, h,
           cell_size, expected_size, payload_size);
    return false;
  }

  const auto cell_type = static_cast<zerocopy::OccupancyGrid::CellType>(cell_type_value);
  std::vector<uint8_t> gray(cell_count, 0);

  switch (cell_type) {
    case zerocopy::OccupancyGrid::kCellInt8: {
      for (size_t i = 0; i < cell_count; ++i) {
        double value = 0;

        if VUNLIKELY (!parser.numeric("data", i, "value", value)) {
          return false;
        }

        if (value < 0) {
          gray[i] = 127;
        } else {
          const auto clamped = std::min<int>(static_cast<int>(value), 100);
          gray[i] = static_cast<uint8_t>(255 - clamped * 255 / 100);
        }
      }

      break;
    }

    case zerocopy::OccupancyGrid::kCellUint8: {
      for (size_t i = 0; i < cell_count; ++i) {
        double value = 0;

        if VUNLIKELY (!parser.numeric("data", i, "value", value)) {
          return false;
        }

        gray[i] = static_cast<uint8_t>(value);
      }

      break;
    }

    case zerocopy::OccupancyGrid::kCellUint16: {
      for (size_t i = 0; i < cell_count; ++i) {
        double value = 0;

        if VUNLIKELY (!parser.numeric("data", i, "value", value)) {
          return false;
        }

        gray[i] = static_cast<uint8_t>(static_cast<uint16_t>(value) >> 8U);
      }

      break;
    }

    case zerocopy::OccupancyGrid::kCellFloat32: {
      double v_min = 0;
      double v_max = 0;
      parser.numeric("value_min", v_min);
      parser.numeric("value_max", v_max);

      if (!std::isfinite(v_min) || !std::isfinite(v_max) || !(v_max > v_min)) {
        v_min = 0.0;
        v_max = 1.0;
      }

      const double inv_range = 1.0 / (v_max - v_min);

      for (size_t i = 0; i < cell_count; ++i) {
        double value = 0;

        if VUNLIKELY (!parser.numeric("data", i, "value", value)) {
          return false;
        }

        if VUNLIKELY (!std::isfinite(value)) {
          return false;
        }

        const double normalized = std::clamp((value - v_min) * inv_range, 0.0, 1.0);
        gray[i] = static_cast<uint8_t>(normalized * 255.0);
      }

      break;
    }
    default: {
      MLOG_W("OccupancyGrid unsupported cell_type={}", static_cast<int>(cell_type));
      return false;
    }
  }

  auto pixel_data = ::rerun::Collection<uint8_t>::borrow(gray.data(), gray.size());
  auto image = ::rerun::archetypes::Image(std::move(pixel_data), {w, h}, ::rerun::datatypes::ColorModel::L);
  rec.log(entity_path, image);

  return true;
}

bool RerunConverter::log_occupancy_grid(::rerun::RecordingStream& rec, const std::string& entity_path,
                                        const Bytes& raw) {
  zerocopy::MessageParser parser;

  if VUNLIKELY (!parser.parse(zerocopy::MessageParser::Type::kOccupancyGrid, raw)) {
    MLOG_W("Failed to deserialize OccupancyGrid");
    return false;
  }

  return log_occupancy_grid(rec, entity_path, parser);
}

template <typename T>
static bool read_parser_collection(const zerocopy::MessageParser& parser, size_t count, std::vector<T>& output) {
  output.resize(count);

  for (size_t index = 0; index < count; ++index) {
    zerocopy::MessageParser::Value value;

    if VUNLIKELY (!parser.value("data", index, "value", value)) {
      return false;
    }

    if (const auto* integer = std::get_if<int64_t>(&value)) {
      output[index] = static_cast<T>(*integer);
      continue;
    }

    if (const auto* integer = std::get_if<uint64_t>(&value)) {
      output[index] = static_cast<T>(*integer);
      continue;
    }

    if (const auto* number = std::get_if<double>(&value)) {
      output[index] = static_cast<T>(*number);
      continue;
    }

    return false;
  }

  return true;
}

bool RerunConverter::log_tensor(::rerun::RecordingStream& rec, const std::string& entity_path,
                                const zerocopy::MessageParser& parser) {
  double rank_value = 0;

  if VUNLIKELY (!parser.numeric("rank", rank_value)) {
    MLOG_W("Tensor metadata is incomplete");
    return false;
  }

  const auto rank = static_cast<uint8_t>(rank_value);

  if VUNLIKELY (rank == 0) {
    MLOG_W("Tensor invalid: rank={}", static_cast<int>(rank));
    return false;
  }

  std::vector<uint64_t> shape;
  shape.reserve(rank);
  size_t expected_elements = 1;

  for (uint8_t i = 0; i < rank; ++i) {
    double dimension = 0;

    if VUNLIKELY (!parser.numeric("shape", i, "value", dimension)) {
      MLOG_W("Tensor shape is incomplete: dim={} rank={}", i, static_cast<int>(rank));
      return false;
    }

    const auto dim = static_cast<uint32_t>(dimension);

    if VUNLIKELY (dim == 0 || expected_elements > std::numeric_limits<size_t>::max() / dim) {
      MLOG_W("Tensor shape invalid: dim={} rank={}", dim, static_cast<int>(rank));
      return false;
    }

    expected_elements *= dim;
    shape.emplace_back(dim);
  }

  ::rerun::datatypes::TensorBuffer buffer;
  double dtype_value = 0;

  if VUNLIKELY (!parser.numeric("dtype", dtype_value)) {
    return false;
  }

  const auto dtype = static_cast<zerocopy::Tensor::DataType>(dtype_value);
  const size_t element_size = zerocopy::Tensor::element_size_of(dtype);

  if VUNLIKELY (element_size == 0) {
    MLOG_W("Tensor dtype unsupported for rerun: {}", static_cast<int>(dtype));
    return false;
  }

  if VUNLIKELY (expected_elements > std::numeric_limits<size_t>::max() / element_size) {
    MLOG_W("Tensor byte size overflow");
    return false;
  }

  size_t expected_stride = 1;

  for (size_t i = rank; i > 0; --i) {
    double stride_value = 0.0;

    if VUNLIKELY (!parser.numeric("strides", i - 1U, "value", stride_value) ||
                  stride_value != static_cast<double>(expected_stride)) {
      MLOG_W("Rerun Tensor requires contiguous row-major strides");
      return false;
    }

    expected_stride *= static_cast<size_t>(shape[i - 1U]);
  }

  const size_t expected_bytes = expected_elements * element_size;
  zerocopy::MessageParser::Value declared_elements_value;
  zerocopy::MessageParser::Value declared_size_value;

  if VUNLIKELY (!parser.value("num_elements", declared_elements_value) || !parser.value("size", declared_size_value)) {
    return false;
  }

  const auto* declared_elements = std::get_if<uint64_t>(&declared_elements_value);
  const auto* declared_size = std::get_if<uint64_t>(&declared_size_value);

  if VUNLIKELY (declared_elements == nullptr || declared_size == nullptr || *declared_elements != expected_elements ||
                *declared_size != expected_bytes) {
    MLOG_W("Tensor payload metadata does not match shape: elements={} bytes={}", expected_elements, expected_bytes);
    return false;
  }

  if VUNLIKELY (parser.collection_size("data") != expected_elements) {
    MLOG_W("Tensor element count mismatch: actual={} expected={}", parser.collection_size("data"), expected_elements);
    return false;
  }

  switch (dtype) {
    case zerocopy::Tensor::kBool:
    case zerocopy::Tensor::kUint8: {
      std::vector<uint8_t> values;

      if VUNLIKELY (!read_parser_collection(parser, expected_elements, values)) {
        return false;
      }

      buffer = ::rerun::datatypes::TensorBuffer(::rerun::Collection<uint8_t>::take_ownership(std::move(values)));
      break;
    }

    case zerocopy::Tensor::kInt8: {
      std::vector<int8_t> values;

      if VUNLIKELY (!read_parser_collection(parser, expected_elements, values)) {
        return false;
      }

      buffer = ::rerun::datatypes::TensorBuffer(::rerun::Collection<int8_t>::take_ownership(std::move(values)));
      break;
    }

    case zerocopy::Tensor::kInt16: {
      std::vector<int16_t> values;

      if VUNLIKELY (!read_parser_collection(parser, expected_elements, values)) {
        return false;
      }

      buffer = ::rerun::datatypes::TensorBuffer(::rerun::Collection<int16_t>::take_ownership(std::move(values)));
      break;
    }

    case zerocopy::Tensor::kUint16: {
      std::vector<uint16_t> values;

      if VUNLIKELY (!read_parser_collection(parser, expected_elements, values)) {
        return false;
      }

      buffer = ::rerun::datatypes::TensorBuffer(::rerun::Collection<uint16_t>::take_ownership(std::move(values)));
      break;
    }

    case zerocopy::Tensor::kInt32: {
      std::vector<int32_t> values;

      if VUNLIKELY (!read_parser_collection(parser, expected_elements, values)) {
        return false;
      }

      buffer = ::rerun::datatypes::TensorBuffer(::rerun::Collection<int32_t>::take_ownership(std::move(values)));
      break;
    }

    case zerocopy::Tensor::kUint32: {
      std::vector<uint32_t> values;

      if VUNLIKELY (!read_parser_collection(parser, expected_elements, values)) {
        return false;
      }

      buffer = ::rerun::datatypes::TensorBuffer(::rerun::Collection<uint32_t>::take_ownership(std::move(values)));
      break;
    }

    case zerocopy::Tensor::kFloat16:
    case zerocopy::Tensor::kBfloat16:
    case zerocopy::Tensor::kFloat32: {
      std::vector<float> values;

      if VUNLIKELY (!read_parser_collection(parser, expected_elements, values)) {
        return false;
      }

      buffer = ::rerun::datatypes::TensorBuffer(::rerun::Collection<float>::take_ownership(std::move(values)));
      break;
    }

    case zerocopy::Tensor::kInt64: {
      std::vector<int64_t> values;

      if VUNLIKELY (!read_parser_collection(parser, expected_elements, values)) {
        return false;
      }

      buffer = ::rerun::datatypes::TensorBuffer(::rerun::Collection<int64_t>::take_ownership(std::move(values)));
      break;
    }

    case zerocopy::Tensor::kUint64: {
      std::vector<uint64_t> values;

      if VUNLIKELY (!read_parser_collection(parser, expected_elements, values)) {
        return false;
      }

      buffer = ::rerun::datatypes::TensorBuffer(::rerun::Collection<uint64_t>::take_ownership(std::move(values)));
      break;
    }

    case zerocopy::Tensor::kFloat64: {
      std::vector<double> values;

      if VUNLIKELY (!read_parser_collection(parser, expected_elements, values)) {
        return false;
      }

      buffer = ::rerun::datatypes::TensorBuffer(::rerun::Collection<double>::take_ownership(std::move(values)));
      break;
    }
    default: {
      return false;
    }
  }

  auto archetype = ::rerun::archetypes::Tensor(std::move(shape), std::move(buffer));

  std::string layout;
  parser.text("layout", layout);

  if (!layout.empty()) {
    std::vector<std::string> dim_names;
    dim_names.reserve(rank);

    for (uint8_t i = 0; i < rank; ++i) {
      if (i < layout.size()) {
        dim_names.emplace_back(1, layout[i]);
      } else {
        dim_names.emplace_back("dim" + std::to_string(static_cast<int>(i)));
      }
    }

    archetype = std::move(archetype).with_dim_names(std::move(dim_names));
  }

  rec.log(entity_path, archetype);

  return true;
}

bool RerunConverter::log_tensor(::rerun::RecordingStream& rec, const std::string& entity_path, const Bytes& raw) {
  zerocopy::MessageParser parser;

  if VUNLIKELY (!parser.parse(zerocopy::MessageParser::Type::kTensor, raw)) {
    MLOG_W("Failed to deserialize Tensor");
    return false;
  }

  return log_tensor(rec, entity_path, parser);
}

bool RerunConverter::log_object_array(::rerun::RecordingStream& rec, const std::string& entity_path,
                                      const zerocopy::MessageParser& parser) {
  const size_t count = parser.collection_size("objects");

  if VUNLIKELY (count == 0) {
    return false;
  }

  std::vector<::rerun::Position3D> centers;
  std::vector<::rerun::HalfSize3D> half_sizes;
  std::vector<::rerun::components::RotationQuat> rotations;
  std::vector<::rerun::Color> colors;
  std::vector<::rerun::components::ClassId> class_ids;
  std::vector<::rerun::components::Text> labels;

  centers.reserve(count);
  half_sizes.reserve(count);
  rotations.reserve(count);
  colors.reserve(count);
  class_ids.reserve(count);
  labels.reserve(count);

  static const ::rerun::Color kMotionColors[] = {
      ::rerun::Color(160, 160, 160), ::rerun::Color(80, 200, 120), ::rerun::Color(255, 96, 96),
      ::rerun::Color(255, 196, 64),  ::rerun::Color(96, 160, 220),
  };

  static constexpr size_t kMotionColorCount = sizeof(kMotionColors) / sizeof(kMotionColors[0]);

  for (size_t i = 0; i < count; ++i) {
    double position_x = 0;
    double position_y = 0;
    double position_z = 0;
    double size_x = 0;
    double size_y = 0;
    double size_z = 0;
    double yaw = 0;
    zerocopy::MessageParser::Value motion_state_value;
    zerocopy::MessageParser::Value class_id_value;
    zerocopy::MessageParser::Value track_id_value;
    std::string label;

    if VUNLIKELY (!parser.numeric("objects", i, "position_x", position_x) ||
                  !parser.numeric("objects", i, "position_y", position_y) ||
                  !parser.numeric("objects", i, "position_z", position_z) ||
                  !parser.numeric("objects", i, "size_x", size_x) || !parser.numeric("objects", i, "size_y", size_y) ||
                  !parser.numeric("objects", i, "size_z", size_z) || !parser.numeric("objects", i, "yaw", yaw) ||
                  !parser.value("objects", i, "motion_state", motion_state_value) ||
                  !parser.value("objects", i, "class_id", class_id_value) ||
                  !parser.value("objects", i, "track_id", track_id_value) ||
                  !parser.text("objects", i, "label", label)) {
      continue;
    }

    const auto* motion_state = std::get_if<uint64_t>(&motion_state_value);
    const auto* class_id = std::get_if<uint64_t>(&class_id_value);
    const auto* track_id = std::get_if<uint64_t>(&track_id_value);

    if VUNLIKELY (motion_state == nullptr || class_id == nullptr || track_id == nullptr) {
      continue;
    }

    centers.emplace_back(static_cast<float>(position_x), static_cast<float>(position_y),
                         static_cast<float>(position_z));
    half_sizes.emplace_back(static_cast<float>(size_x * 0.5), static_cast<float>(size_y * 0.5),
                            static_cast<float>(size_z * 0.5));

    const auto half_yaw = static_cast<float>(yaw * 0.5);
    auto sin_half = std::sin(half_yaw);
    auto cos_half = std::cos(half_yaw);
    rotations.emplace_back(::rerun::datatypes::Quaternion::from_xyzw(0.0F, 0.0F, sin_half, cos_half));

    const auto motion_idx = static_cast<size_t>(*motion_state);

    if (motion_idx < kMotionColorCount) {
      colors.emplace_back(kMotionColors[motion_idx]);
    } else {
      colors.emplace_back(kMotionColors[0]);
    }

    class_ids.emplace_back(static_cast<uint16_t>(std::min<uint64_t>(*class_id, 0xFFFFU)));

    if (*track_id != 0) {
      label.append("#").append(std::to_string(*track_id));
    }

    labels.emplace_back(label);
  }

  if VUNLIKELY (centers.empty()) {
    return false;
  }

  auto boxes = ::rerun::archetypes::Boxes3D::from_centers_and_half_sizes(centers, half_sizes);

  if (!rotations.empty()) {
    boxes = std::move(boxes).with_quaternions(std::move(rotations));
  }

  if (!colors.empty()) {
    boxes = std::move(boxes).with_colors(std::move(colors));
  }

  if (!class_ids.empty()) {
    boxes = std::move(boxes).with_class_ids(std::move(class_ids));
  }

  if (!labels.empty()) {
    boxes = std::move(boxes).with_labels(std::move(labels));
    boxes = std::move(boxes).with_show_labels(::rerun::components::ShowLabels(false));
  }

  rec.log(entity_path, boxes);

  return true;
}

bool RerunConverter::log_object_array(::rerun::RecordingStream& rec, const std::string& entity_path, const Bytes& raw) {
  zerocopy::MessageParser parser;

  if VUNLIKELY (!parser.parse(zerocopy::MessageParser::Type::kObjectArray, raw)) {
    MLOG_W("Failed to deserialize ObjectArray");
    return false;
  }

  return log_object_array(rec, entity_path, parser);
}

bool RerunConverter::log_audio_frame(::rerun::RecordingStream& rec, const std::string& entity_path,
                                     const zerocopy::MessageParser& parser) {
  double num_channels_value = 0;
  double num_samples_value = 0;
  double layout_value = 0;
  double format_value = 0;
  size_t payload_size = 0;

  if VUNLIKELY (!parser.numeric("num_channels", num_channels_value) ||
                !parser.numeric("num_samples", num_samples_value) || !parser.numeric("layout", layout_value) ||
                !parser.numeric("format", format_value) || !read_parser_size(parser, "size", payload_size)) {
    return false;
  }

  const auto num_channels = static_cast<uint16_t>(num_channels_value);
  const auto num_samples = static_cast<uint32_t>(num_samples_value);
  const auto layout = static_cast<zerocopy::AudioFrame::Layout>(layout_value);

  if VUNLIKELY (num_channels == 0 || num_samples == 0) {
    return false;
  }

  if VUNLIKELY (layout != zerocopy::AudioFrame::kLayoutInterleaved && layout != zerocopy::AudioFrame::kLayoutPlanar) {
    MLOG_W("AudioFrame layout unsupported for rerun tensor: {}", static_cast<int>(layout));
    return false;
  }

  ::rerun::datatypes::TensorBuffer buffer;
  const auto format = static_cast<zerocopy::AudioFrame::Format>(format_value);
  size_t element_size = 0;

  switch (format) {
    case zerocopy::AudioFrame::kFormatPcmU8:
      element_size = sizeof(uint8_t);
      break;
    case zerocopy::AudioFrame::kFormatPcmS16:
      element_size = sizeof(int16_t);
      break;
    case zerocopy::AudioFrame::kFormatPcmS32:
      element_size = sizeof(int32_t);
      break;
    case zerocopy::AudioFrame::kFormatPcmF32:
      element_size = sizeof(float);
      break;
    default:
      MLOG_W("AudioFrame format unsupported for rerun tensor: {}", static_cast<int>(format));
      return false;
  }

  const size_t channel_sample_size = static_cast<size_t>(num_channels) * element_size;
  size_t expected_size = 0;

  if VUNLIKELY (channel_sample_size == 0 ||
                !mul_size(static_cast<size_t>(num_samples), channel_sample_size, expected_size)) {
    MLOG_W("AudioFrame byte size overflow: samples={} channels={}", num_samples, num_channels);
    return false;
  }

  const size_t expected_elements = static_cast<size_t>(num_samples) * num_channels;

  if VUNLIKELY (payload_size != expected_size || parser.collection_size("data") != expected_elements) {
    MLOG_W("AudioFrame payload size mismatch: actual={} expected={}", payload_size, expected_size);
    return false;
  }

  switch (format) {
    case zerocopy::AudioFrame::kFormatPcmU8: {
      std::vector<uint8_t> values;

      if VUNLIKELY (!read_parser_collection(parser, expected_elements, values)) {
        return false;
      }

      buffer = ::rerun::datatypes::TensorBuffer(::rerun::Collection<uint8_t>::take_ownership(std::move(values)));
      break;
    }

    case zerocopy::AudioFrame::kFormatPcmS16: {
      std::vector<int16_t> values;

      if VUNLIKELY (!read_parser_collection(parser, expected_elements, values)) {
        return false;
      }

      buffer = ::rerun::datatypes::TensorBuffer(::rerun::Collection<int16_t>::take_ownership(std::move(values)));
      break;
    }

    case zerocopy::AudioFrame::kFormatPcmS32: {
      std::vector<int32_t> values;

      if VUNLIKELY (!read_parser_collection(parser, expected_elements, values)) {
        return false;
      }

      buffer = ::rerun::datatypes::TensorBuffer(::rerun::Collection<int32_t>::take_ownership(std::move(values)));
      break;
    }

    case zerocopy::AudioFrame::kFormatPcmF32: {
      std::vector<float> values;

      if VUNLIKELY (!read_parser_collection(parser, expected_elements, values)) {
        return false;
      }

      buffer = ::rerun::datatypes::TensorBuffer(::rerun::Collection<float>::take_ownership(std::move(values)));
      break;
    }
    default:
      return false;
  }

  std::vector<uint64_t> shape;
  shape.reserve(2);

  if (layout == zerocopy::AudioFrame::kLayoutPlanar) {
    shape.emplace_back(static_cast<uint64_t>(num_channels));
    shape.emplace_back(static_cast<uint64_t>(num_samples));
  } else {
    shape.emplace_back(static_cast<uint64_t>(num_samples));
    shape.emplace_back(static_cast<uint64_t>(num_channels));
  }

  auto archetype = ::rerun::archetypes::Tensor(std::move(shape), std::move(buffer));

  std::vector<std::string> dim_names;
  dim_names.reserve(2);

  if (layout == zerocopy::AudioFrame::kLayoutPlanar) {
    dim_names.emplace_back("channel");
    dim_names.emplace_back("sample");
  } else {
    dim_names.emplace_back("sample");
    dim_names.emplace_back("channel");
  }

  archetype = std::move(archetype).with_dim_names(std::move(dim_names));

  rec.log(entity_path, archetype);

  return true;
}

bool RerunConverter::log_audio_frame(::rerun::RecordingStream& rec, const std::string& entity_path, const Bytes& raw) {
  zerocopy::MessageParser parser;

  if VUNLIKELY (!parser.parse(zerocopy::MessageParser::Type::kAudioFrame, raw)) {
    MLOG_W("Failed to deserialize AudioFrame");
    return false;
  }

  return log_audio_frame(rec, entity_path, parser);
}

bool RerunConverter::log_proto_with_mapping(::rerun::RecordingStream& rec, const std::string& entity_path,
                                            const RerunMap& mapping, const google::protobuf::Message& msg) {
  using RerunLogFn =
      bool (*)(::rerun::RecordingStream&, const std::string&, const RerunMap&, const google::protobuf::Message&);
  static const std::unordered_map<std::string, RerunLogFn> kRerunDispatch = {
      {"GeoPoints", log_geo_points},
      {"Transform3D", log_transform3d},
      {"Boxes3D", log_boxes3d},
      {"Points3D", log_points3d},
      {"EncodedImage", log_encoded_image},
      {"Image", log_image},
      {"TextLog", log_text_log},
      {"Pinhole", log_pinhole},
      {"DepthImage", log_depth_image},
      {"Scalars", log_scalars},
      {"LineStrips3D", log_line_strips3d},
      {"LineStrips2D", log_line_strips2d},
      {"Boxes2D", log_boxes2d},
      {"Arrows3D", log_arrows3d},
      {"Points2D", log_points2d},
      {"SegmentationImage", log_segmentation_image},
      {"SeriesLine", log_series_line},
      {"SeriesLines", log_series_line},
      {"SeriesPoint", log_series_point},
      {"SeriesPoints", log_series_point},
      {"Arrows2D", log_arrows2d},
      {"Mesh3D", log_mesh3d},
      {"Cylinders3D", log_cylinders3d},
      {"Ellipsoids3D", log_ellipsoids3d},
      {"GeoLineStrings", log_geo_line_strings},
      {"BarChart", log_bar_chart},
      {"AnnotationContext", log_annotation_context},
      {"Capsules3D", log_capsules3d},
      {"EncodedDepthImage", log_encoded_depth_image},
      {"Asset3D", log_asset3d},
      {"GraphNodes", log_graph_nodes},
      {"GraphEdges", log_graph_edges},
      {"ViewCoordinates", log_view_coordinates},
      {"InstancePoses3D", log_instance_poses3d},
      {"AssetVideo", log_asset_video},
      {"VideoFrameReference", log_video_frame_reference},
      {"Tensor", log_tensor},
  };

  const auto& archetype = mapping.archetype;
  auto dispatch_iter = kRerunDispatch.find(archetype);

  if (dispatch_iter != kRerunDispatch.end()) {
    return dispatch_iter->second(rec, entity_path, mapping, msg);
  }

  thread_local std::unordered_set<std::string> warned_archtypes;

  if (warned_archtypes.insert(archetype).second) {
    MLOG_W("Unknown rerun archetype '{}', falling back to TextLog", archetype);
  }

  rec.log(entity_path, ::rerun::TextLog(msg.ShortDebugString()));
  return true;
}

bool RerunConverter::log_geo_points(::rerun::RecordingStream& rec, const std::string& entity_path,
                                    const RerunMap& mapping, const google::protobuf::Message& msg) {
  double latitude = 0.0;
  double longitude = 0.0;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "latitude") {
      latitude = get_proto_double(msg, fm.source, fm);
    } else if (fm.target == "longitude") {
      longitude = get_proto_double(msg, fm.source, fm);
    }
  }

  rec.log(entity_path, ::rerun::archetypes::GeoPoints({{latitude, longitude}}));
  return true;
}

bool RerunConverter::log_transform3d(::rerun::RecordingStream& rec, const std::string& entity_path,
                                     const RerunMap& mapping, const google::protobuf::Message& msg) {
  double position_x = 0.0;
  double position_y = 0.0;
  double position_z = 0.0;

  const google::protobuf::Message* orientation_msg = nullptr;
  const google::protobuf::Message* euler_msg = nullptr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "pose" || fm.target == "pose_euler") {
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

  auto translation = ::rerun::components::Translation3D(static_cast<float>(position_x), static_cast<float>(position_y),
                                                        static_cast<float>(position_z));

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

    if (qx == 0.0 && qy == 0.0 && qz == 0.0 && qw == 0.0) {
      qw = 1.0;
    }

    auto quaternion = ::rerun::Quaternion::from_xyzw(static_cast<float>(qx), static_cast<float>(qy),
                                                     static_cast<float>(qz), static_cast<float>(qw));

    rec.log(entity_path, ::rerun::archetypes::Transform3D().with_translation(translation).with_quaternion(quaternion));
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

    auto quaternion = ::rerun::Quaternion::from_xyzw(static_cast<float>(qx), static_cast<float>(qy),
                                                     static_cast<float>(qz), static_cast<float>(qw));

    rec.log(entity_path, ::rerun::archetypes::Transform3D().with_translation(translation).with_quaternion(quaternion));
  } else {
    rec.log(entity_path, ::rerun::archetypes::Transform3D().with_translation(translation));
  }

  return true;
}

bool RerunConverter::log_boxes3d(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                                 const google::protobuf::Message& msg) {
  std::string entity_sub_items;
  const FieldMapping* entity_x_mapping = nullptr;
  const FieldMapping* entity_y_mapping = nullptr;
  const FieldMapping* entity_z_mapping = nullptr;
  const FieldMapping* entity_w_mapping = nullptr;
  const FieldMapping* entity_l_mapping = nullptr;
  const FieldMapping* entity_h_mapping = nullptr;
  const FieldMapping* entity_heading_mapping = nullptr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "entity_sub_items") {
      entity_sub_items = fm.source;
    } else if (fm.target == "entity_x") {
      entity_x_mapping = &fm;
    } else if (fm.target == "entity_y") {
      entity_y_mapping = &fm;
    } else if (fm.target == "entity_z") {
      entity_z_mapping = &fm;
    } else if (fm.target == "entity_width") {
      entity_w_mapping = &fm;
    } else if (fm.target == "entity_length") {
      entity_l_mapping = &fm;
    } else if (fm.target == "entity_height") {
      entity_h_mapping = &fm;
    } else if (fm.target == "entity_heading") {
      entity_heading_mapping = &fm;
    }
  }

  bool has_entity_fields = entity_x_mapping != nullptr || entity_y_mapping != nullptr || entity_z_mapping != nullptr ||
                           entity_w_mapping != nullptr || entity_l_mapping != nullptr || entity_h_mapping != nullptr ||
                           entity_heading_mapping != nullptr;

  std::vector<::rerun::Position3D> centers;
  std::vector<::rerun::HalfSize3D> half_sizes;
  std::vector<::rerun::components::RotationQuat> rotations;
  std::vector<::rerun::Color> box_colors;

  auto process_item = [&box_colors, &centers, &entity_h_mapping, &entity_heading_mapping, &entity_l_mapping,
                       &entity_w_mapping, &entity_x_mapping, &entity_y_mapping, &entity_z_mapping, &half_sizes,
                       &has_entity_fields, &rotations](const google::protobuf::Message& item) {
    double px = 0.0;
    double py = 0.0;
    double pz = 0.0;
    double sx = 1.0;
    double sy = 1.0;
    double sz = 1.0;
    double heading = 0.0;

    if (has_entity_fields) {
      if (entity_x_mapping) {
        px = get_proto_double(item, entity_x_mapping->source, *entity_x_mapping);
      }

      if (entity_y_mapping) {
        py = get_proto_double(item, entity_y_mapping->source, *entity_y_mapping);
      }

      if (entity_z_mapping) {
        pz = get_proto_double(item, entity_z_mapping->source, *entity_z_mapping);
      }

      if (entity_w_mapping) {
        auto v = get_proto_double(item, entity_w_mapping->source, *entity_w_mapping);

        if (v != 0.0) {
          sx = v;
        }
      }

      if (entity_l_mapping) {
        auto v = get_proto_double(item, entity_l_mapping->source, *entity_l_mapping);

        if (v != 0.0) {
          sy = v;
        }
      }

      if (entity_h_mapping) {
        auto v = get_proto_double(item, entity_h_mapping->source, *entity_h_mapping);

        if (v != 0.0) {
          sz = v;
        }
      }

      if (entity_heading_mapping) {
        heading = get_proto_double(item, entity_heading_mapping->source, *entity_heading_mapping);
      }
    } else {
      FieldMapping fm;
      px = get_proto_double(item, "x", fm);
      py = get_proto_double(item, "y", fm);
      pz = get_proto_double(item, "z", fm);

      if (px == 0.0 && py == 0.0 && pz == 0.0) {
        px = get_proto_double(item, "cx", fm);
        py = get_proto_double(item, "cy", fm);
        pz = get_proto_double(item, "cz", fm);
      }

      if (px == 0.0 && py == 0.0 && pz == 0.0) {
        px = safe_nested_double(item, "position.x");
        py = safe_nested_double(item, "position.y");
        pz = safe_nested_double(item, "position.z");
      }

      auto w_val = get_proto_double(item, "width", fm);
      auto l_val = get_proto_double(item, "length", fm);
      auto h_val = get_proto_double(item, "height", fm);

      if (w_val != 0.0) {
        sx = w_val;
      }

      if (l_val != 0.0) {
        sy = l_val;
      }

      if (h_val != 0.0) {
        sz = h_val;
      }

      heading = get_proto_double(item, "heading_angle", fm);

      if (heading == 0.0) {
        heading = get_proto_double(item, "yaw", fm);
      }
    }

    centers.emplace_back(static_cast<float>(px), static_cast<float>(py), static_cast<float>(pz));
    half_sizes.emplace_back(static_cast<float>(sx * 0.5), static_cast<float>(sy * 0.5), static_cast<float>(sz * 0.5));

    if (heading != 0.0) {
      auto qz_val = static_cast<float>(std::sin(heading * 0.5));
      auto qw_val = static_cast<float>(std::cos(heading * 0.5));
      rotations.emplace_back(::rerun::datatypes::Quaternion::from_xyzw(0.0F, 0.0F, qz_val, qw_val));
    } else {
      rotations.emplace_back(::rerun::datatypes::Quaternion::from_xyzw(0.0F, 0.0F, 0.0F, 1.0F));
    }

    box_colors.emplace_back(51, 204, 51, 204);
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
    const auto* vec_field = find_proto_field_cached(*desc, entities_field_name);

    if VUNLIKELY (!vec_field || !vec_field->is_repeated()) {
      continue;
    }

    int count = ref->FieldSize(*entities_parent, vec_field);

    for (int i = 0; i < count; ++i) {
      if (vec_field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
        continue;
      }

      const auto& item = ref->GetRepeatedMessage(*entities_parent, vec_field, i);

      if (!entity_sub_items.empty()) {
        const auto* sub_desc = item.GetDescriptor();
        const auto* sub_ref = item.GetReflection();
        const auto* sub_field = find_proto_field_cached(*sub_desc, entity_sub_items);

        if (sub_field && sub_field->is_repeated() &&
            sub_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
          int sub_count = sub_ref->FieldSize(item, sub_field);

          for (int j = 0; j < sub_count; ++j) {
            process_item(sub_ref->GetRepeatedMessage(item, sub_field, j));
          }
        }
      } else {
        process_item(item);
      }
    }
  }

  if (!centers.empty()) {
    auto boxes = ::rerun::archetypes::Boxes3D::from_centers_and_half_sizes(centers, half_sizes);

    if (!rotations.empty()) {
      boxes = std::move(boxes).with_quaternions(std::move(rotations));
    }

    if (!box_colors.empty()) {
      boxes = std::move(boxes).with_colors(std::move(box_colors));
    }

    rec.log(entity_path, boxes);
  }

  return true;
}

bool RerunConverter::log_points3d(::rerun::RecordingStream& rec, const std::string& entity_path,
                                  const RerunMap& mapping, const google::protobuf::Message& msg) {
  std::string ranges_src;
  const FieldMapping* entity_x_mapping = nullptr;
  const FieldMapping* entity_y_mapping = nullptr;
  const FieldMapping* entity_z_mapping = nullptr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "ranges") {
      ranges_src = fm.source;
    } else if (fm.target == "entity_x" || fm.target == "point_x") {
      entity_x_mapping = &fm;
    } else if (fm.target == "entity_y" || fm.target == "point_y") {
      entity_y_mapping = &fm;
    } else if (fm.target == "entity_z" || fm.target == "point_z") {
      entity_z_mapping = &fm;
    }
  }

  const auto* desc = msg.GetDescriptor();
  const auto* ref = msg.GetReflection();
  auto ranges_field_name = ranges_src.empty() ? std::string("ranges") : ranges_src;
  const auto* ranges_field = find_proto_field_cached(*desc, ranges_field_name);

  if (ranges_field && ranges_field->is_repeated()) {
    double angle_min = 0.0;
    double angle_max = 0.0;
    double angle_increment = 0.0;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "angle_min") {
        angle_min = get_proto_double(msg, fm.source, fm);
      } else if (fm.target == "angle_max") {
        angle_max = get_proto_double(msg, fm.source, fm);
      } else if (fm.target == "angle_increment") {
        angle_increment = get_proto_double(msg, fm.source, fm);
      }
    }

    int range_count = ref->FieldSize(msg, ranges_field);
    std::vector<::rerun::Position3D> positions;
    positions.reserve(range_count);

    for (int i = 0; i < range_count; ++i) {
      double range = 0.0;

      if (ranges_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_FLOAT) {
        range = static_cast<double>(ref->GetRepeatedFloat(msg, ranges_field, i));
      } else if (ranges_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE) {
        range = ref->GetRepeatedDouble(msg, ranges_field, i);
      }

      if (std::isinf(range) || std::isnan(range) || range <= 0.0) {
        continue;
      }

      double angle = angle_min + i * angle_increment;

      if (angle_increment == 0.0 && range_count > 1) {
        angle = angle_min + i * (angle_max - angle_min) / (range_count - 1);
      }

      auto x = static_cast<float>(range * std::cos(angle));
      auto y = static_cast<float>(range * std::sin(angle));
      positions.emplace_back(x, y, 0.0F);
    }

    if (!positions.empty()) {
      rec.log(entity_path, ::rerun::archetypes::Points3D(std::move(positions)));
    }

    return true;
  }

  std::vector<::rerun::Position3D> positions;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target != "entities" && fm.target != "points") {
      continue;
    }

    const google::protobuf::Message* parent = nullptr;
    std::string field_name;

    if VUNLIKELY (!resolve_proto_parent_field_path(msg, fm.source, parent, field_name)) {
      continue;
    }

    if VUNLIKELY (!parent) {
      continue;
    }

    const auto* p_desc = parent->GetDescriptor();
    const auto* p_ref = parent->GetReflection();
    const auto* vec_field = find_proto_field_cached(*p_desc, field_name);

    if VUNLIKELY (!vec_field || !vec_field->is_repeated()) {
      continue;
    }

    int count = p_ref->FieldSize(*parent, vec_field);

    for (int i = 0; i < count; ++i) {
      if (vec_field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
        continue;
      }

      const auto& item = p_ref->GetRepeatedMessage(*parent, vec_field, i);
      FieldMapping empty_fm;

      auto resolve_field = [&empty_fm, &item](const FieldMapping* field_mapping, std::string_view fallback) -> double {
        if (!field_mapping) {
          return get_proto_double(item, fallback, empty_fm);
        }

        return get_proto_double(item, field_mapping->source, *field_mapping);
      };

      double px = resolve_field(entity_x_mapping, "x");
      double py = resolve_field(entity_y_mapping, "y");
      double pz = resolve_field(entity_z_mapping, "z");

      positions.emplace_back(static_cast<float>(px), static_cast<float>(py), static_cast<float>(pz));
    }
  }

  if (!positions.empty()) {
    rec.log(entity_path, ::rerun::archetypes::Points3D(std::move(positions)));
  }

  return true;
}

bool RerunConverter::log_encoded_image(::rerun::RecordingStream& rec, const std::string& entity_path,
                                       const RerunMap& mapping, const google::protobuf::Message& msg) {
  std::string format;
  Bytes data;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "format") {
      format = get_proto_string(msg, fm.source, fm);
    } else if (fm.target == "data") {
      data = get_proto_bytes(msg, fm.source);
    }
  }

  if (data.empty()) {
    data = get_proto_bytes(msg, "data");

    if (data.empty()) {
      data = get_proto_bytes(msg, "image_data");
    }
  }

  if VUNLIKELY (data.empty()) {
    return false;
  }

  std::string media_type;

  if (format == "jpeg" || format == "jpg") {
    media_type = "image/jpeg";
  } else if (format == "png") {
    media_type = "image/png";
  }

  auto blob = ::rerun::Collection<uint8_t>::borrow(data.data(), data.size());

  if (media_type.empty()) {
    rec.log(entity_path, ::rerun::archetypes::EncodedImage::from_bytes(blob));
  } else {
    rec.log(entity_path,
            ::rerun::archetypes::EncodedImage::from_bytes(blob, ::rerun::components::MediaType(media_type)));
  }

  return true;
}

bool RerunConverter::log_image(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                               const google::protobuf::Message& msg) {
  uint32_t width = 0;
  uint32_t height = 0;
  Bytes data;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "width") {
      width = checked_unsigned_cast<uint32_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "height") {
      height = checked_unsigned_cast<uint32_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "data") {
      data = get_proto_bytes(msg, fm.source);
    }
  }

  if VUNLIKELY (data.empty() || width == 0 || height == 0) {
    return false;
  }

  size_t pixel_total = 0;

  if VUNLIKELY (!mul_size(static_cast<size_t>(width), static_cast<size_t>(height), pixel_total)) {
    return false;
  }

  if (data.size() == pixel_total) {
    return log_typed_image(rec, entity_path, data.data(), data.size(), width, height, ::rerun::datatypes::ColorModel::L,
                           ::rerun::datatypes::ChannelDatatype::U8);
  }

  size_t expected_size = 0;

  if (mul_size(pixel_total, 3U, expected_size) && data.size() == expected_size) {
    return log_typed_image(rec, entity_path, data.data(), data.size(), width, height,
                           ::rerun::datatypes::ColorModel::RGB, ::rerun::datatypes::ChannelDatatype::U8);
  }

  if (mul_size(pixel_total, 4U, expected_size) && data.size() == expected_size) {
    return log_typed_image(rec, entity_path, data.data(), data.size(), width, height,
                           ::rerun::datatypes::ColorModel::RGBA, ::rerun::datatypes::ChannelDatatype::U8);
  }

  return false;
}

bool RerunConverter::log_text_log(::rerun::RecordingStream& rec, const std::string& entity_path,
                                  const RerunMap& mapping, const google::protobuf::Message& msg) {
  std::string message;
  std::string level;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "message") {
      message = get_proto_string(msg, fm.source, fm);
    } else if (fm.target == "level" || fm.target == "severity") {
      level = get_proto_string(msg, fm.source, fm);
    }
  }

  if (message.empty()) {
    message = msg.ShortDebugString();
  }

  auto text_log = ::rerun::TextLog(message);

  if (!level.empty()) {
    text_log = std::move(text_log).with_level(::rerun::components::TextLogLevel(level));
  }

  rec.log(entity_path, text_log);

  return true;
}

bool RerunConverter::log_pinhole(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                                 const google::protobuf::Message& msg) {
  double fx = 0.0;
  double fy = 0.0;
  double cx = 0.0;
  double cy = 0.0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool has_cx = false;
  bool has_cy = false;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "fx") {
      fx = get_proto_double(msg, fm.source, fm);
    } else if (fm.target == "fy") {
      fy = get_proto_double(msg, fm.source, fm);
    } else if (fm.target == "cx") {
      cx = get_proto_double(msg, fm.source, fm);
      has_cx = true;
    } else if (fm.target == "cy") {
      cy = get_proto_double(msg, fm.source, fm);
      has_cy = true;
    } else if (fm.target == "width" || fm.target == "image_width") {
      width = checked_unsigned_cast<uint32_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "height" || fm.target == "image_height") {
      height = checked_unsigned_cast<uint32_t>(get_proto_double(msg, fm.source, fm));
    }
  }

  const auto resolution_width = static_cast<float>(width > 0 ? width : cx * 2.0);
  const auto resolution_height = static_cast<float>(height > 0 ? height : cy * 2.0);
  const auto principal_x = has_cx ? static_cast<float>(cx) : resolution_width * 0.5F;
  const auto principal_y = has_cy ? static_cast<float>(cy) : resolution_height * 0.5F;

  if VUNLIKELY (!std::isfinite(fx) || !std::isfinite(fy) || !std::isfinite(principal_x) ||
                !std::isfinite(principal_y) || !std::isfinite(resolution_width) || !std::isfinite(resolution_height) ||
                fx <= 0.0 || fy <= 0.0 || resolution_width <= 0.0F || resolution_height <= 0.0F) {
    return false;
  }

  auto pinhole = make_pinhole(static_cast<float>(fx), static_cast<float>(fy), principal_x, principal_y,
                              resolution_width, resolution_height);

  rec.log_static(entity_path, pinhole);

  return true;
}

bool RerunConverter::log_depth_image(::rerun::RecordingStream& rec, const std::string& entity_path,
                                     const RerunMap& mapping, const google::protobuf::Message& msg) {
  uint32_t width = 0;
  uint32_t height = 0;
  std::string data_src;
  std::string datatype;
  float meter = 0.0F;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "column_count" || fm.target == "width") {
      width = checked_unsigned_cast<uint32_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "row_count" || fm.target == "height") {
      height = checked_unsigned_cast<uint32_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "data") {
      data_src = fm.source;
    } else if (fm.target == "datatype") {
      datatype = get_proto_string(msg, fm.source, fm);
    } else if (fm.target == "meter") {
      meter = static_cast<float>(get_proto_double(msg, fm.source, fm));
    }
  }

  const auto* desc = msg.GetDescriptor();
  const auto* ref = msg.GetReflection();
  auto data_field_name = data_src.empty() ? std::string("data") : data_src;
  const auto* data_field = find_proto_field_cached(*desc, data_field_name);

  if (!data_field || width == 0 || height == 0) {
    return false;
  }

  if (data_field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_STRING) {
    return false;
  }

  std::string scratch;
  const auto& data = ref->GetStringReference(msg, data_field, &scratch);
  return log_depth_image_bytes(rec, entity_path, reinterpret_cast<const uint8_t*>(data.data()), data.size(), width,
                               height, datatype, meter);
}

bool RerunConverter::log_scalars(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                                 const google::protobuf::Message& msg) {
  double value = 0.0;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "value") {
      value = get_proto_double(msg, fm.source, fm);
      break;
    }
  }

  rec.log(entity_path, ::rerun::archetypes::Scalars(value));
  return true;
}

bool RerunConverter::log_line_strips3d(::rerun::RecordingStream& rec, const std::string& entity_path,
                                       const RerunMap& mapping, const google::protobuf::Message& msg) {
  std::string entities_src;
  const FieldMapping* x_mapping = nullptr;
  const FieldMapping* y_mapping = nullptr;
  const FieldMapping* z_mapping = nullptr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "entities" || fm.target == "strips" || fm.target == "points") {
      entities_src = fm.source;
    } else if (fm.target == "point_x") {
      x_mapping = &fm;
    } else if (fm.target == "point_y") {
      y_mapping = &fm;
    } else if (fm.target == "point_z") {
      z_mapping = &fm;
    }
  }

  const auto* desc = msg.GetDescriptor();
  const auto* ref = msg.GetReflection();

  std::string field_name = entities_src.empty() ? "points" : entities_src;
  const auto* vec_field = find_proto_field_cached(*desc, field_name);

  if (vec_field && vec_field->is_repeated() &&
      vec_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
    int count = ref->FieldSize(msg, vec_field);
    std::vector<::rerun::Position3D> strip;
    strip.reserve(count);
    FieldMapping empty_fm;

    for (int i = 0; i < count; ++i) {
      const auto& item = ref->GetRepeatedMessage(msg, vec_field, i);
      double px =
          x_mapping ? get_proto_double(item, x_mapping->source, *x_mapping) : get_proto_double(item, "x", empty_fm);
      double py =
          y_mapping ? get_proto_double(item, y_mapping->source, *y_mapping) : get_proto_double(item, "y", empty_fm);
      double pz =
          z_mapping ? get_proto_double(item, z_mapping->source, *z_mapping) : get_proto_double(item, "z", empty_fm);
      strip.emplace_back(static_cast<float>(px), static_cast<float>(py), static_cast<float>(pz));
    }

    if (!strip.empty()) {
      rec.log(entity_path, ::rerun::archetypes::LineStrips3D({std::move(strip)}));
    }

    return true;
  }

  return false;
}

bool RerunConverter::log_line_strips2d(::rerun::RecordingStream& rec, const std::string& entity_path,
                                       const RerunMap& mapping, const google::protobuf::Message& msg) {
  std::string entities_src;
  const FieldMapping* x_mapping = nullptr;
  const FieldMapping* y_mapping = nullptr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "entities" || fm.target == "strips" || fm.target == "points") {
      entities_src = fm.source;
    } else if (fm.target == "point_x") {
      x_mapping = &fm;
    } else if (fm.target == "point_y") {
      y_mapping = &fm;
    }
  }

  const auto* desc = msg.GetDescriptor();
  const auto* ref = msg.GetReflection();

  std::string field_name = entities_src.empty() ? "points" : entities_src;
  const auto* vec_field = find_proto_field_cached(*desc, field_name);

  if (vec_field && vec_field->is_repeated() &&
      vec_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
    int count = ref->FieldSize(msg, vec_field);
    std::vector<::rerun::Position2D> strip;
    strip.reserve(count);
    FieldMapping empty_fm;

    for (int i = 0; i < count; ++i) {
      const auto& item = ref->GetRepeatedMessage(msg, vec_field, i);
      double px =
          x_mapping ? get_proto_double(item, x_mapping->source, *x_mapping) : get_proto_double(item, "x", empty_fm);
      double py =
          y_mapping ? get_proto_double(item, y_mapping->source, *y_mapping) : get_proto_double(item, "y", empty_fm);
      strip.emplace_back(static_cast<float>(px), static_cast<float>(py));
    }

    if (!strip.empty()) {
      rec.log(entity_path, ::rerun::archetypes::LineStrips2D({std::move(strip)}));
    }

    return true;
  }

  return false;
}

bool RerunConverter::log_boxes2d(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                                 const google::protobuf::Message& msg) {
  std::string entities_src;
  const FieldMapping* x_mapping = nullptr;
  const FieldMapping* y_mapping = nullptr;
  const FieldMapping* w_mapping = nullptr;
  const FieldMapping* h_mapping = nullptr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "entities") {
      entities_src = fm.source;
    } else if (fm.target == "center_x") {
      x_mapping = &fm;
    } else if (fm.target == "center_y") {
      y_mapping = &fm;
    } else if (fm.target == "width" || fm.target == "size_x") {
      w_mapping = &fm;
    } else if (fm.target == "height" || fm.target == "size_y") {
      h_mapping = &fm;
    }
  }

  const auto* desc = msg.GetDescriptor();
  const auto* ref = msg.GetReflection();

  std::string field_name = entities_src.empty() ? "boxes" : entities_src;
  const auto* vec_field = find_proto_field_cached(*desc, field_name);

  if VUNLIKELY (!vec_field || !vec_field->is_repeated()) {
    return false;
  }

  int count = ref->FieldSize(msg, vec_field);
  std::vector<::rerun::datatypes::Vec2D> centers;
  std::vector<::rerun::datatypes::Vec2D> half_sizes;
  centers.reserve(count);
  half_sizes.reserve(count);

  for (int i = 0; i < count; ++i) {
    if (vec_field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      continue;
    }

    const auto& item = ref->GetRepeatedMessage(msg, vec_field, i);
    FieldMapping empty_fm;

    double cx = x_mapping ? get_proto_double(item, x_mapping->source, *x_mapping)
                          : get_proto_double(item, "center_x", empty_fm);
    double cy = y_mapping ? get_proto_double(item, y_mapping->source, *y_mapping)
                          : get_proto_double(item, "center_y", empty_fm);
    double w =
        w_mapping ? get_proto_double(item, w_mapping->source, *w_mapping) : get_proto_double(item, "width", empty_fm);
    double h =
        h_mapping ? get_proto_double(item, h_mapping->source, *h_mapping) : get_proto_double(item, "height", empty_fm);

    centers.emplace_back(static_cast<float>(cx), static_cast<float>(cy));
    half_sizes.emplace_back(static_cast<float>(w * 0.5), static_cast<float>(h * 0.5));
  }

  if (!centers.empty()) {
    rec.log(entity_path, ::rerun::archetypes::Boxes2D::from_centers_and_half_sizes(centers, half_sizes));
  }

  return true;
}

bool RerunConverter::log_arrows3d(::rerun::RecordingStream& rec, const std::string& entity_path,
                                  const RerunMap& mapping, const google::protobuf::Message& msg) {
  std::string entities_src;
  const FieldMapping* origin_x_mapping = nullptr;
  const FieldMapping* origin_y_mapping = nullptr;
  const FieldMapping* origin_z_mapping = nullptr;
  const FieldMapping* vec_x_mapping = nullptr;
  const FieldMapping* vec_y_mapping = nullptr;
  const FieldMapping* vec_z_mapping = nullptr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "entities") {
      entities_src = fm.source;
    } else if (fm.target == "origin_x") {
      origin_x_mapping = &fm;
    } else if (fm.target == "origin_y") {
      origin_y_mapping = &fm;
    } else if (fm.target == "origin_z") {
      origin_z_mapping = &fm;
    } else if (fm.target == "vector_x") {
      vec_x_mapping = &fm;
    } else if (fm.target == "vector_y") {
      vec_y_mapping = &fm;
    } else if (fm.target == "vector_z") {
      vec_z_mapping = &fm;
    }
  }

  const auto* desc = msg.GetDescriptor();
  const auto* ref = msg.GetReflection();

  std::string field_name = entities_src.empty() ? "arrows" : entities_src;
  const auto* vec_field = find_proto_field_cached(*desc, field_name);

  if VUNLIKELY (!vec_field || !vec_field->is_repeated()) {
    FieldMapping empty_fm;
    double ox = origin_x_mapping ? get_proto_double(msg, origin_x_mapping->source, *origin_x_mapping)
                                 : get_proto_double(msg, "origin_x", empty_fm);
    double oy = origin_y_mapping ? get_proto_double(msg, origin_y_mapping->source, *origin_y_mapping)
                                 : get_proto_double(msg, "origin_y", empty_fm);
    double oz = origin_z_mapping ? get_proto_double(msg, origin_z_mapping->source, *origin_z_mapping)
                                 : get_proto_double(msg, "origin_z", empty_fm);
    double vx = vec_x_mapping ? get_proto_double(msg, vec_x_mapping->source, *vec_x_mapping)
                              : get_proto_double(msg, "vector_x", empty_fm);
    double vy = vec_y_mapping ? get_proto_double(msg, vec_y_mapping->source, *vec_y_mapping)
                              : get_proto_double(msg, "vector_y", empty_fm);
    double vz = vec_z_mapping ? get_proto_double(msg, vec_z_mapping->source, *vec_z_mapping)
                              : get_proto_double(msg, "vector_z", empty_fm);

    auto arrows = ::rerun::archetypes::Arrows3D::from_vectors(
        {::rerun::Vector3D(static_cast<float>(vx), static_cast<float>(vy), static_cast<float>(vz))});
    arrows = std::move(arrows).with_origins(
        {::rerun::Position3D(static_cast<float>(ox), static_cast<float>(oy), static_cast<float>(oz))});
    rec.log(entity_path, arrows);
    return true;
  }

  int count = ref->FieldSize(msg, vec_field);
  std::vector<::rerun::Vector3D> vectors;
  std::vector<::rerun::Position3D> origins;
  vectors.reserve(count);
  origins.reserve(count);

  for (int i = 0; i < count; ++i) {
    if (vec_field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      continue;
    }

    const auto& item = ref->GetRepeatedMessage(msg, vec_field, i);
    FieldMapping empty_fm;
    double ox = origin_x_mapping ? get_proto_double(item, origin_x_mapping->source, *origin_x_mapping)
                                 : get_proto_double(item, "origin_x", empty_fm);
    double oy = origin_y_mapping ? get_proto_double(item, origin_y_mapping->source, *origin_y_mapping)
                                 : get_proto_double(item, "origin_y", empty_fm);
    double oz = origin_z_mapping ? get_proto_double(item, origin_z_mapping->source, *origin_z_mapping)
                                 : get_proto_double(item, "origin_z", empty_fm);
    double vx = vec_x_mapping ? get_proto_double(item, vec_x_mapping->source, *vec_x_mapping)
                              : get_proto_double(item, "vector_x", empty_fm);
    double vy = vec_y_mapping ? get_proto_double(item, vec_y_mapping->source, *vec_y_mapping)
                              : get_proto_double(item, "vector_y", empty_fm);
    double vz = vec_z_mapping ? get_proto_double(item, vec_z_mapping->source, *vec_z_mapping)
                              : get_proto_double(item, "vector_z", empty_fm);

    origins.emplace_back(static_cast<float>(ox), static_cast<float>(oy), static_cast<float>(oz));
    vectors.emplace_back(static_cast<float>(vx), static_cast<float>(vy), static_cast<float>(vz));
  }

  if (!vectors.empty()) {
    auto arrows = ::rerun::archetypes::Arrows3D::from_vectors(std::move(vectors));
    arrows = std::move(arrows).with_origins(std::move(origins));
    rec.log(entity_path, arrows);
  }

  return true;
}

bool RerunConverter::log_points2d(::rerun::RecordingStream& rec, const std::string& entity_path,
                                  const RerunMap& mapping, const google::protobuf::Message& msg) {
  std::string entities_src;
  const FieldMapping* x_mapping = nullptr;
  const FieldMapping* y_mapping = nullptr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "entities" || fm.target == "points") {
      entities_src = fm.source;
    } else if (fm.target == "point_x") {
      x_mapping = &fm;
    } else if (fm.target == "point_y") {
      y_mapping = &fm;
    }
  }

  const auto* desc = msg.GetDescriptor();
  const auto* ref = msg.GetReflection();

  std::string field_name = entities_src.empty() ? "points" : entities_src;
  const auto* vec_field = find_proto_field_cached(*desc, field_name);

  if VUNLIKELY (!vec_field || !vec_field->is_repeated()) {
    return false;
  }

  int count = ref->FieldSize(msg, vec_field);
  std::vector<::rerun::Position2D> positions;
  positions.reserve(count);

  for (int i = 0; i < count; ++i) {
    if (vec_field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      continue;
    }

    const auto& item = ref->GetRepeatedMessage(msg, vec_field, i);
    FieldMapping empty_fm;
    double px =
        x_mapping ? get_proto_double(item, x_mapping->source, *x_mapping) : get_proto_double(item, "x", empty_fm);
    double py =
        y_mapping ? get_proto_double(item, y_mapping->source, *y_mapping) : get_proto_double(item, "y", empty_fm);
    positions.emplace_back(static_cast<float>(px), static_cast<float>(py));
  }

  if (!positions.empty()) {
    rec.log(entity_path, ::rerun::archetypes::Points2D(std::move(positions)));
  }

  return true;
}

bool RerunConverter::log_segmentation_image(::rerun::RecordingStream& rec, const std::string& entity_path,
                                            const RerunMap& mapping, const google::protobuf::Message& msg) {
  uint32_t width = 0;
  uint32_t height = 0;
  Bytes data;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "width") {
      width = checked_unsigned_cast<uint32_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "height") {
      height = checked_unsigned_cast<uint32_t>(get_proto_double(msg, fm.source, fm));
    } else if (fm.target == "data") {
      data = get_proto_bytes(msg, fm.source);
    }
  }

  if VUNLIKELY (data.empty() || width == 0 || height == 0) {
    return false;
  }

  size_t pixel_total = 0;

  if VUNLIKELY (!mul_size(static_cast<size_t>(width), static_cast<size_t>(height), pixel_total) ||
                data.size() != pixel_total) {
    return false;
  }

  auto pixel_data = ::rerun::Collection<uint8_t>::borrow(data.data(), data.size());
  rec.log(entity_path, ::rerun::archetypes::SegmentationImage(pixel_data, {width, height}));

  return true;
}

bool RerunConverter::log_series_line(::rerun::RecordingStream& rec, const std::string& entity_path,
                                     const RerunMap& mapping, const google::protobuf::Message& msg) {
  double value = 0.0;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "value") {
      value = get_proto_double(msg, fm.source, fm);
      break;
    }
  }

  rec.log(entity_path, ::rerun::archetypes::Scalars(value));
  rec.log(entity_path, ::rerun::archetypes::SeriesLines());

  return true;
}

bool RerunConverter::log_series_point(::rerun::RecordingStream& rec, const std::string& entity_path,
                                      const RerunMap& mapping, const google::protobuf::Message& msg) {
  double value = 0.0;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "value") {
      value = get_proto_double(msg, fm.source, fm);
      break;
    }
  }

  rec.log(entity_path, ::rerun::archetypes::Scalars(value));
  rec.log(entity_path, ::rerun::archetypes::SeriesPoints());

  return true;
}

bool RerunConverter::log_arrows2d(::rerun::RecordingStream& rec, const std::string& entity_path,
                                  const RerunMap& mapping, const google::protobuf::Message& msg) {
  std::string entities_src;
  const FieldMapping* origin_x_mapping = nullptr;
  const FieldMapping* origin_y_mapping = nullptr;
  const FieldMapping* vec_x_mapping = nullptr;
  const FieldMapping* vec_y_mapping = nullptr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "entities") {
      entities_src = fm.source;
    } else if (fm.target == "origin_x") {
      origin_x_mapping = &fm;
    } else if (fm.target == "origin_y") {
      origin_y_mapping = &fm;
    } else if (fm.target == "vector_x") {
      vec_x_mapping = &fm;
    } else if (fm.target == "vector_y") {
      vec_y_mapping = &fm;
    }
  }

  const auto* desc = msg.GetDescriptor();
  const auto* ref = msg.GetReflection();

  std::string field_name = entities_src.empty() ? "arrows" : entities_src;
  const auto* vec_field = find_proto_field_cached(*desc, field_name);

  if VUNLIKELY (!vec_field || !vec_field->is_repeated()) {
    FieldMapping empty_fm;
    double ox = origin_x_mapping ? get_proto_double(msg, origin_x_mapping->source, *origin_x_mapping)
                                 : get_proto_double(msg, "origin_x", empty_fm);
    double oy = origin_y_mapping ? get_proto_double(msg, origin_y_mapping->source, *origin_y_mapping)
                                 : get_proto_double(msg, "origin_y", empty_fm);
    double vx = vec_x_mapping ? get_proto_double(msg, vec_x_mapping->source, *vec_x_mapping)
                              : get_proto_double(msg, "vector_x", empty_fm);
    double vy = vec_y_mapping ? get_proto_double(msg, vec_y_mapping->source, *vec_y_mapping)
                              : get_proto_double(msg, "vector_y", empty_fm);

    auto arrows = ::rerun::archetypes::Arrows2D::from_vectors(
        {::rerun::components::Vector2D(static_cast<float>(vx), static_cast<float>(vy))});
    arrows = std::move(arrows).with_origins({::rerun::Position2D(static_cast<float>(ox), static_cast<float>(oy))});
    rec.log(entity_path, arrows);
    return true;
  }

  int count = ref->FieldSize(msg, vec_field);
  std::vector<::rerun::components::Vector2D> vectors;
  std::vector<::rerun::Position2D> origins;
  vectors.reserve(count);
  origins.reserve(count);

  for (int i = 0; i < count; ++i) {
    if (vec_field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      continue;
    }

    const auto& item = ref->GetRepeatedMessage(msg, vec_field, i);
    FieldMapping empty_fm;
    double ox = origin_x_mapping ? get_proto_double(item, origin_x_mapping->source, *origin_x_mapping)
                                 : get_proto_double(item, "origin_x", empty_fm);
    double oy = origin_y_mapping ? get_proto_double(item, origin_y_mapping->source, *origin_y_mapping)
                                 : get_proto_double(item, "origin_y", empty_fm);
    double vx = vec_x_mapping ? get_proto_double(item, vec_x_mapping->source, *vec_x_mapping)
                              : get_proto_double(item, "vector_x", empty_fm);
    double vy = vec_y_mapping ? get_proto_double(item, vec_y_mapping->source, *vec_y_mapping)
                              : get_proto_double(item, "vector_y", empty_fm);

    origins.emplace_back(static_cast<float>(ox), static_cast<float>(oy));
    vectors.emplace_back(static_cast<float>(vx), static_cast<float>(vy));
  }

  if (!vectors.empty()) {
    auto arrows = ::rerun::archetypes::Arrows2D::from_vectors(std::move(vectors));
    arrows = std::move(arrows).with_origins(std::move(origins));
    rec.log(entity_path, arrows);
  }

  return true;
}

bool RerunConverter::log_mesh3d(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                                const google::protobuf::Message& msg) {
  std::string vertices_src;
  std::string indices_src;
  std::string colors_src;
  std::string normals_src;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "vertices" || fm.target == "vertex_positions") {
      vertices_src = fm.source;
    } else if (fm.target == "indices" || fm.target == "triangle_indices") {
      indices_src = fm.source;
    } else if (fm.target == "vertex_colors" || fm.target == "colors") {
      colors_src = fm.source;
    } else if (fm.target == "vertex_normals" || fm.target == "normals") {
      normals_src = fm.source;
    }
  }

  const auto* desc = msg.GetDescriptor();
  const auto* ref = msg.GetReflection();

  std::vector<::rerun::Position3D> positions;
  std::vector<::rerun::components::TriangleIndices> triangle_indices;
  std::vector<::rerun::Color> vertex_colors;
  std::vector<::rerun::components::Vector3D> vertex_normals;

  std::string vert_field_name = vertices_src.empty() ? "vertices" : vertices_src;
  const auto* vert_field = find_proto_field_cached(*desc, vert_field_name);

  if (vert_field && vert_field->is_repeated()) {
    int count = ref->FieldSize(msg, vert_field);

    if (vert_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      positions.reserve(count);
      FieldMapping empty_fm;

      for (int i = 0; i < count; ++i) {
        const auto& item = ref->GetRepeatedMessage(msg, vert_field, i);
        double vx = get_proto_double(item, "x", empty_fm);
        double vy = get_proto_double(item, "y", empty_fm);
        double vz = get_proto_double(item, "z", empty_fm);
        positions.emplace_back(static_cast<float>(vx), static_cast<float>(vy), static_cast<float>(vz));
      }
    } else if (vert_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_FLOAT ||
               vert_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE) {
      positions.reserve(count / 3);

      for (int i = 0; i + 2 < count; i += 3) {
        float vx = 0.0F;
        float vy = 0.0F;
        float vz = 0.0F;

        if (vert_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_FLOAT) {
          vx = ref->GetRepeatedFloat(msg, vert_field, i);
          vy = ref->GetRepeatedFloat(msg, vert_field, i + 1);
          vz = ref->GetRepeatedFloat(msg, vert_field, i + 2);
        } else {
          vx = static_cast<float>(ref->GetRepeatedDouble(msg, vert_field, i));
          vy = static_cast<float>(ref->GetRepeatedDouble(msg, vert_field, i + 1));
          vz = static_cast<float>(ref->GetRepeatedDouble(msg, vert_field, i + 2));
        }

        positions.emplace_back(vx, vy, vz);
      }
    }
  }

  if VUNLIKELY (positions.empty()) {
    return false;
  }

  std::string idx_field_name = indices_src.empty() ? "indices" : indices_src;
  const auto* idx_field = find_proto_field_cached(*desc, idx_field_name);

  if (idx_field && idx_field->is_repeated()) {
    int count = ref->FieldSize(msg, idx_field);

    if (idx_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      triangle_indices.reserve(count);
      FieldMapping empty_fm;

      for (int i = 0; i < count; ++i) {
        const auto& item = ref->GetRepeatedMessage(msg, idx_field, i);
        auto i0 = checked_unsigned_cast<uint32_t>(get_proto_double(item, "v0", empty_fm));
        auto i1 = checked_unsigned_cast<uint32_t>(get_proto_double(item, "v1", empty_fm));
        auto i2 = checked_unsigned_cast<uint32_t>(get_proto_double(item, "v2", empty_fm));
        triangle_indices.emplace_back(::rerun::datatypes::UVec3D{i0, i1, i2});
      }
    } else {
      triangle_indices.reserve(count / 3);

      auto idx_cpp_type = idx_field->cpp_type();

      for (int i = 0; i + 2 < count; i += 3) {
        uint32_t i0 = 0;
        uint32_t i1 = 0;
        uint32_t i2 = 0;

        if (idx_cpp_type == google::protobuf::FieldDescriptor::CPPTYPE_UINT32) {
          i0 = ref->GetRepeatedUInt32(msg, idx_field, i);
          i1 = ref->GetRepeatedUInt32(msg, idx_field, i + 1);
          i2 = ref->GetRepeatedUInt32(msg, idx_field, i + 2);
        } else if (idx_cpp_type == google::protobuf::FieldDescriptor::CPPTYPE_INT32) {
          i0 = static_cast<uint32_t>(ref->GetRepeatedInt32(msg, idx_field, i));
          i1 = static_cast<uint32_t>(ref->GetRepeatedInt32(msg, idx_field, i + 1));
          i2 = static_cast<uint32_t>(ref->GetRepeatedInt32(msg, idx_field, i + 2));
        } else if (idx_cpp_type == google::protobuf::FieldDescriptor::CPPTYPE_UINT64) {
          i0 = static_cast<uint32_t>(ref->GetRepeatedUInt64(msg, idx_field, i));
          i1 = static_cast<uint32_t>(ref->GetRepeatedUInt64(msg, idx_field, i + 1));
          i2 = static_cast<uint32_t>(ref->GetRepeatedUInt64(msg, idx_field, i + 2));
        } else if (idx_cpp_type == google::protobuf::FieldDescriptor::CPPTYPE_INT64) {
          i0 = static_cast<uint32_t>(ref->GetRepeatedInt64(msg, idx_field, i));
          i1 = static_cast<uint32_t>(ref->GetRepeatedInt64(msg, idx_field, i + 1));
          i2 = static_cast<uint32_t>(ref->GetRepeatedInt64(msg, idx_field, i + 2));
        } else {
          continue;
        }

        triangle_indices.emplace_back(::rerun::datatypes::UVec3D{i0, i1, i2});
      }
    }
  }

  std::string col_field_name = colors_src.empty() ? "vertex_colors" : colors_src;
  const auto* col_field = find_proto_field_cached(*desc, col_field_name);

  if (col_field && col_field->is_repeated()) {
    int count = ref->FieldSize(msg, col_field);

    if (col_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      vertex_colors.reserve(count);
      FieldMapping empty_fm;
      const auto* color_msg_type = col_field->message_type();
      const auto* a_field_desc = color_msg_type ? color_msg_type->FindFieldByName("a") : nullptr;

      for (int i = 0; i < count; ++i) {
        const auto& item = ref->GetRepeatedMessage(msg, col_field, i);
        auto r = checked_unsigned_cast<uint8_t>(get_proto_double(item, "r", empty_fm));
        auto g = checked_unsigned_cast<uint8_t>(get_proto_double(item, "g", empty_fm));
        auto b = checked_unsigned_cast<uint8_t>(get_proto_double(item, "b", empty_fm));
        auto a = a_field_desc ? checked_unsigned_cast<uint8_t>(get_proto_double(item, "a", empty_fm))
                              : static_cast<uint8_t>(255);
        vertex_colors.emplace_back(r, g, b, a);
      }
    } else {
      vertex_colors.reserve(count);

      for (int i = 0; i < count; ++i) {
        uint32_t rgba = 0;

        if (col_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_UINT32) {
          rgba = ref->GetRepeatedUInt32(msg, col_field, i);
        } else if (col_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_INT32) {
          rgba = static_cast<uint32_t>(ref->GetRepeatedInt32(msg, col_field, i));
        }

        vertex_colors.emplace_back(rgba);
      }
    }
  }

  std::string norm_field_name = normals_src.empty() ? "vertex_normals" : normals_src;
  const auto* norm_field = find_proto_field_cached(*desc, norm_field_name);

  if (norm_field && norm_field->is_repeated()) {
    int count = ref->FieldSize(msg, norm_field);

    if (norm_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      vertex_normals.reserve(count);
      FieldMapping empty_fm;

      for (int i = 0; i < count; ++i) {
        const auto& item = ref->GetRepeatedMessage(msg, norm_field, i);
        double nx = get_proto_double(item, "x", empty_fm);
        double ny = get_proto_double(item, "y", empty_fm);
        double nz = get_proto_double(item, "z", empty_fm);
        vertex_normals.emplace_back(static_cast<float>(nx), static_cast<float>(ny), static_cast<float>(nz));
      }
    }
  }

  auto mesh = ::rerun::archetypes::Mesh3D(std::move(positions));

  if (!triangle_indices.empty()) {
    mesh = std::move(mesh).with_triangle_indices(std::move(triangle_indices));
  }

  if (!vertex_colors.empty()) {
    mesh = std::move(mesh).with_vertex_colors(std::move(vertex_colors));
  }

  if (!vertex_normals.empty()) {
    mesh = std::move(mesh).with_vertex_normals(std::move(vertex_normals));
  }

  rec.log(entity_path, mesh);
  return true;
}

bool RerunConverter::log_cylinders3d(::rerun::RecordingStream& rec, const std::string& entity_path,
                                     const RerunMap& mapping, const google::protobuf::Message& msg) {
  std::string entities_src;
  const FieldMapping* length_mapping = nullptr;
  const FieldMapping* radius_mapping = nullptr;
  const FieldMapping* center_x_mapping = nullptr;
  const FieldMapping* center_y_mapping = nullptr;
  const FieldMapping* center_z_mapping = nullptr;
  const FieldMapping* color_r_mapping = nullptr;
  const FieldMapping* color_g_mapping = nullptr;
  const FieldMapping* color_b_mapping = nullptr;
  const FieldMapping* color_a_mapping = nullptr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "entities") {
      entities_src = fm.source;
    } else if (fm.target == "length") {
      length_mapping = &fm;
    } else if (fm.target == "radius") {
      radius_mapping = &fm;
    } else if (fm.target == "center_x") {
      center_x_mapping = &fm;
    } else if (fm.target == "center_y") {
      center_y_mapping = &fm;
    } else if (fm.target == "center_z") {
      center_z_mapping = &fm;
    } else if (fm.target == "color_r") {
      color_r_mapping = &fm;
    } else if (fm.target == "color_g") {
      color_g_mapping = &fm;
    } else if (fm.target == "color_b") {
      color_b_mapping = &fm;
    } else if (fm.target == "color_a") {
      color_a_mapping = &fm;
    }
  }

  FieldMapping empty_fm;
  auto get_value = [&empty_fm](const google::protobuf::Message& value, const FieldMapping* field_mapping,
                               std::string_view fallback) {
    return field_mapping ? get_proto_double(value, field_mapping->source, *field_mapping)
                         : get_proto_double(value, fallback, empty_fm);
  };

  const auto* desc = msg.GetDescriptor();
  const auto* ref = msg.GetReflection();

  std::string field_name = entities_src.empty() ? "cylinders" : entities_src;
  const auto* vec_field = find_proto_field_cached(*desc, field_name);

  if VUNLIKELY (!vec_field || !vec_field->is_repeated()) {
    auto length = static_cast<float>(get_value(msg, length_mapping, "length"));
    auto radius = static_cast<float>(get_value(msg, radius_mapping, "radius"));
    auto cx = static_cast<float>(get_value(msg, center_x_mapping, "center_x"));
    auto cy = static_cast<float>(get_value(msg, center_y_mapping, "center_y"));
    auto cz = static_cast<float>(get_value(msg, center_z_mapping, "center_z"));

    auto cylinders = ::rerun::archetypes::Cylinders3D::from_lengths_and_radii({length}, {radius});
    cylinders = std::move(cylinders).with_centers({::rerun::datatypes::Vec3D{cx, cy, cz}});

    if (color_r_mapping) {
      auto r = checked_unsigned_cast<uint8_t>(get_value(msg, color_r_mapping, {}));
      auto g = checked_unsigned_cast<uint8_t>(get_value(msg, color_g_mapping, {}));
      auto b = checked_unsigned_cast<uint8_t>(get_value(msg, color_b_mapping, {}));
      auto a = checked_unsigned_cast<uint8_t>(color_a_mapping ? get_value(msg, color_a_mapping, {}) : 255.0);
      cylinders = std::move(cylinders).with_colors({::rerun::Color(r, g, b, a)});
    }

    rec.log(entity_path, cylinders);
    return true;
  }

  int count = ref->FieldSize(msg, vec_field);
  std::vector<float> lengths;
  std::vector<float> radii;
  std::vector<::rerun::datatypes::Vec3D> centers;
  std::vector<::rerun::Color> colors;
  lengths.reserve(count);
  radii.reserve(count);
  centers.reserve(count);

  for (int i = 0; i < count; ++i) {
    if (vec_field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      continue;
    }

    const auto& item = ref->GetRepeatedMessage(msg, vec_field, i);
    lengths.emplace_back(static_cast<float>(get_value(item, length_mapping, "length")));
    radii.emplace_back(static_cast<float>(get_value(item, radius_mapping, "radius")));
    auto cx = static_cast<float>(get_value(item, center_x_mapping, "center_x"));
    auto cy = static_cast<float>(get_value(item, center_y_mapping, "center_y"));
    auto cz = static_cast<float>(get_value(item, center_z_mapping, "center_z"));
    centers.emplace_back(cx, cy, cz);

    if (color_r_mapping) {
      auto r = checked_unsigned_cast<uint8_t>(get_value(item, color_r_mapping, {}));
      auto g = checked_unsigned_cast<uint8_t>(get_value(item, color_g_mapping, {}));
      auto b = checked_unsigned_cast<uint8_t>(get_value(item, color_b_mapping, {}));
      auto a = checked_unsigned_cast<uint8_t>(color_a_mapping ? get_value(item, color_a_mapping, {}) : 255.0);
      colors.emplace_back(r, g, b, a);
    }
  }

  if (!lengths.empty()) {
    auto cylinders = ::rerun::archetypes::Cylinders3D::from_lengths_and_radii(std::move(lengths), std::move(radii));
    cylinders = std::move(cylinders).with_centers(std::move(centers));

    if (!colors.empty()) {
      cylinders = std::move(cylinders).with_colors(std::move(colors));
    }

    rec.log(entity_path, cylinders);
  }

  return true;
}

bool RerunConverter::log_ellipsoids3d(::rerun::RecordingStream& rec, const std::string& entity_path,
                                      const RerunMap& mapping, const google::protobuf::Message& msg) {
  std::string entities_src;
  const FieldMapping* half_size_x_mapping = nullptr;
  const FieldMapping* half_size_y_mapping = nullptr;
  const FieldMapping* half_size_z_mapping = nullptr;
  const FieldMapping* center_x_mapping = nullptr;
  const FieldMapping* center_y_mapping = nullptr;
  const FieldMapping* center_z_mapping = nullptr;
  const FieldMapping* color_r_mapping = nullptr;
  const FieldMapping* color_g_mapping = nullptr;
  const FieldMapping* color_b_mapping = nullptr;
  const FieldMapping* color_a_mapping = nullptr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "entities") {
      entities_src = fm.source;
    } else if (fm.target == "half_size_x") {
      half_size_x_mapping = &fm;
    } else if (fm.target == "half_size_y") {
      half_size_y_mapping = &fm;
    } else if (fm.target == "half_size_z") {
      half_size_z_mapping = &fm;
    } else if (fm.target == "center_x") {
      center_x_mapping = &fm;
    } else if (fm.target == "center_y") {
      center_y_mapping = &fm;
    } else if (fm.target == "center_z") {
      center_z_mapping = &fm;
    } else if (fm.target == "color_r") {
      color_r_mapping = &fm;
    } else if (fm.target == "color_g") {
      color_g_mapping = &fm;
    } else if (fm.target == "color_b") {
      color_b_mapping = &fm;
    } else if (fm.target == "color_a") {
      color_a_mapping = &fm;
    }
  }

  FieldMapping empty_fm;
  auto get_value = [&empty_fm](const google::protobuf::Message& value, const FieldMapping* field_mapping,
                               std::string_view fallback) {
    return field_mapping ? get_proto_double(value, field_mapping->source, *field_mapping)
                         : get_proto_double(value, fallback, empty_fm);
  };

  const auto* desc = msg.GetDescriptor();
  const auto* ref = msg.GetReflection();

  std::string field_name = entities_src.empty() ? "ellipsoids" : entities_src;
  const auto* vec_field = find_proto_field_cached(*desc, field_name);

  if VUNLIKELY (!vec_field || !vec_field->is_repeated()) {
    auto hx = static_cast<float>(get_value(msg, half_size_x_mapping, "half_size_x"));
    auto hy = static_cast<float>(get_value(msg, half_size_y_mapping, "half_size_y"));
    auto hz = static_cast<float>(get_value(msg, half_size_z_mapping, "half_size_z"));
    auto cx = static_cast<float>(get_value(msg, center_x_mapping, "center_x"));
    auto cy = static_cast<float>(get_value(msg, center_y_mapping, "center_y"));
    auto cz = static_cast<float>(get_value(msg, center_z_mapping, "center_z"));

    auto ellipsoids = ::rerun::archetypes::Ellipsoids3D::from_centers_and_half_sizes(
        {::rerun::datatypes::Vec3D{cx, cy, cz}}, {::rerun::HalfSize3D(hx, hy, hz)});

    if (color_r_mapping) {
      auto r = checked_unsigned_cast<uint8_t>(get_value(msg, color_r_mapping, {}));
      auto g = checked_unsigned_cast<uint8_t>(get_value(msg, color_g_mapping, {}));
      auto b = checked_unsigned_cast<uint8_t>(get_value(msg, color_b_mapping, {}));
      auto a = checked_unsigned_cast<uint8_t>(color_a_mapping ? get_value(msg, color_a_mapping, {}) : 255.0);
      ellipsoids = std::move(ellipsoids).with_colors({::rerun::Color(r, g, b, a)});
    }

    rec.log(entity_path, ellipsoids);
    return true;
  }

  int count = ref->FieldSize(msg, vec_field);
  std::vector<::rerun::HalfSize3D> half_sizes;
  std::vector<::rerun::datatypes::Vec3D> centers;
  std::vector<::rerun::Color> colors;
  half_sizes.reserve(count);
  centers.reserve(count);

  for (int i = 0; i < count; ++i) {
    if (vec_field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      continue;
    }

    const auto& item = ref->GetRepeatedMessage(msg, vec_field, i);
    auto hx = static_cast<float>(get_value(item, half_size_x_mapping, "half_size_x"));
    auto hy = static_cast<float>(get_value(item, half_size_y_mapping, "half_size_y"));
    auto hz = static_cast<float>(get_value(item, half_size_z_mapping, "half_size_z"));
    auto cx = static_cast<float>(get_value(item, center_x_mapping, "center_x"));
    auto cy = static_cast<float>(get_value(item, center_y_mapping, "center_y"));
    auto cz = static_cast<float>(get_value(item, center_z_mapping, "center_z"));

    half_sizes.emplace_back(hx, hy, hz);
    centers.emplace_back(cx, cy, cz);

    if (color_r_mapping) {
      auto r = checked_unsigned_cast<uint8_t>(get_value(item, color_r_mapping, {}));
      auto g = checked_unsigned_cast<uint8_t>(get_value(item, color_g_mapping, {}));
      auto b = checked_unsigned_cast<uint8_t>(get_value(item, color_b_mapping, {}));
      auto a = checked_unsigned_cast<uint8_t>(color_a_mapping ? get_value(item, color_a_mapping, {}) : 255.0);
      colors.emplace_back(r, g, b, a);
    }
  }

  if (!half_sizes.empty()) {
    auto ellipsoids =
        ::rerun::archetypes::Ellipsoids3D::from_centers_and_half_sizes(std::move(centers), std::move(half_sizes));

    if (!colors.empty()) {
      ellipsoids = std::move(ellipsoids).with_colors(std::move(colors));
    }

    rec.log(entity_path, ellipsoids);
  }

  return true;
}

bool RerunConverter::log_geo_line_strings(::rerun::RecordingStream& rec, const std::string& entity_path,
                                          const RerunMap& mapping, const google::protobuf::Message& msg) {
  std::string entities_src;
  const FieldMapping* lat_mapping = nullptr;
  const FieldMapping* lon_mapping = nullptr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "entities" || fm.target == "line_strings" || fm.target == "points") {
      entities_src = fm.source;
    } else if (fm.target == "latitude" || fm.target == "lat") {
      lat_mapping = &fm;
    } else if (fm.target == "longitude" || fm.target == "lon") {
      lon_mapping = &fm;
    }
  }

  const auto* desc = msg.GetDescriptor();
  const auto* ref = msg.GetReflection();

  std::string field_name = entities_src.empty() ? "points" : entities_src;
  const auto* vec_field = find_proto_field_cached(*desc, field_name);

  if (vec_field && vec_field->is_repeated() &&
      vec_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
    int count = ref->FieldSize(msg, vec_field);
    std::vector<::rerun::datatypes::DVec2D> lat_lons;
    lat_lons.reserve(count);
    FieldMapping empty_fm;

    for (int i = 0; i < count; ++i) {
      const auto& item = ref->GetRepeatedMessage(msg, vec_field, i);
      double lat = lat_mapping ? get_proto_double(item, lat_mapping->source, *lat_mapping)
                               : get_proto_double(item, "latitude", empty_fm);
      double lon = lon_mapping ? get_proto_double(item, lon_mapping->source, *lon_mapping)
                               : get_proto_double(item, "longitude", empty_fm);
      lat_lons.emplace_back(lat, lon);
    }

    if (!lat_lons.empty()) {
      auto line_string = ::rerun::components::GeoLineString::from_lat_lon(std::move(lat_lons));
      rec.log(entity_path, ::rerun::archetypes::GeoLineStrings(std::move(line_string)));
    }

    return true;
  }

  return false;
}

bool RerunConverter::log_bar_chart(::rerun::RecordingStream& rec, const std::string& entity_path,
                                   const RerunMap& mapping, const google::protobuf::Message& msg) {
  std::string values_src;
  const FieldMapping* values_mapping = nullptr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "values") {
      values_src = fm.source;
      values_mapping = &fm;
      break;
    }
  }

  const auto* desc = msg.GetDescriptor();
  const auto* ref = msg.GetReflection();

  std::string field_name = values_src.empty() ? "values" : values_src;
  const auto* values_field = find_proto_field_cached(*desc, field_name);

  if VUNLIKELY (!values_field) {
    if (!values_mapping) {
      return false;
    }

    double value = get_proto_double(msg, values_mapping->source, *values_mapping);
    rec.log(entity_path, ::rerun::archetypes::BarChart::f64({value}));
    return true;
  }

  if VUNLIKELY (!values_field->is_repeated()) {
    FieldMapping empty_fm;
    double value = values_mapping ? get_proto_double(msg, values_mapping->source, *values_mapping)
                                  : get_proto_double(msg, field_name, empty_fm);
    rec.log(entity_path, ::rerun::archetypes::BarChart::f64({value}));
    return true;
  }

  int count = ref->FieldSize(msg, values_field);
  std::vector<double> values;
  values.reserve(count);

  for (int i = 0; i < count; ++i) {
    if (values_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_FLOAT) {
      values.emplace_back(static_cast<double>(ref->GetRepeatedFloat(msg, values_field, i)));
    } else if (values_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE) {
      values.emplace_back(ref->GetRepeatedDouble(msg, values_field, i));
    } else if (values_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_INT32) {
      values.emplace_back(static_cast<double>(ref->GetRepeatedInt32(msg, values_field, i)));
    } else if (values_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_INT64) {
      values.emplace_back(static_cast<double>(ref->GetRepeatedInt64(msg, values_field, i)));
    } else if (values_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_UINT32) {
      values.emplace_back(static_cast<double>(ref->GetRepeatedUInt32(msg, values_field, i)));
    } else if (values_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_UINT64) {
      values.emplace_back(static_cast<double>(ref->GetRepeatedUInt64(msg, values_field, i)));
    }
  }

  if (!values.empty()) {
    rec.log(entity_path, ::rerun::archetypes::BarChart::f64(std::move(values)));
  }

  return true;
}

bool RerunConverter::log_annotation_context(::rerun::RecordingStream& rec, const std::string& entity_path,
                                            const RerunMap& mapping, const google::protobuf::Message& msg) {
  std::string entities_src;
  const FieldMapping* class_id_mapping = nullptr;
  const FieldMapping* label_mapping = nullptr;
  const FieldMapping* color_r_mapping = nullptr;
  const FieldMapping* color_g_mapping = nullptr;
  const FieldMapping* color_b_mapping = nullptr;
  const FieldMapping* color_a_mapping = nullptr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "entities" || fm.target == "annotations" || fm.target == "class_descriptions") {
      entities_src = fm.source;
    } else if (fm.target == "class_id") {
      class_id_mapping = &fm;
    } else if (fm.target == "label") {
      label_mapping = &fm;
    } else if (fm.target == "color_r") {
      color_r_mapping = &fm;
    } else if (fm.target == "color_g") {
      color_g_mapping = &fm;
    } else if (fm.target == "color_b") {
      color_b_mapping = &fm;
    } else if (fm.target == "color_a") {
      color_a_mapping = &fm;
    }
  }

  const auto* desc = msg.GetDescriptor();
  const auto* ref = msg.GetReflection();

  std::string field_name = entities_src.empty() ? "annotations" : entities_src;
  const auto* vec_field = find_proto_field_cached(*desc, field_name);

  if VUNLIKELY (!vec_field || !vec_field->is_repeated()) {
    return false;
  }

  int count = ref->FieldSize(msg, vec_field);
  std::vector<::rerun::datatypes::AnnotationInfo> annotations;
  annotations.reserve(count);

  for (int i = 0; i < count; ++i) {
    if (vec_field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      continue;
    }

    const auto& item = ref->GetRepeatedMessage(msg, vec_field, i);
    FieldMapping empty_fm;

    auto class_id = checked_unsigned_cast<uint16_t>(
        class_id_mapping ? get_proto_double(item, class_id_mapping->source, *class_id_mapping)
                         : get_proto_double(item, "class_id", empty_fm));

    auto label = label_mapping ? get_proto_string(item, label_mapping->source, *label_mapping)
                               : get_proto_string(item, "label", empty_fm);

    if (color_r_mapping) {
      auto r = checked_unsigned_cast<uint8_t>(get_proto_double(item, color_r_mapping->source, *color_r_mapping));
      auto g = checked_unsigned_cast<uint8_t>(
          color_g_mapping ? get_proto_double(item, color_g_mapping->source, *color_g_mapping) : 0.0);
      auto b = checked_unsigned_cast<uint8_t>(
          color_b_mapping ? get_proto_double(item, color_b_mapping->source, *color_b_mapping) : 0.0);
      auto a = checked_unsigned_cast<uint8_t>(
          color_a_mapping ? get_proto_double(item, color_a_mapping->source, *color_a_mapping) : 255.0);

      if (label.empty()) {
        annotations.emplace_back(class_id, ::rerun::datatypes::Rgba32(r, g, b, a));
      } else {
        annotations.emplace_back(class_id, label, ::rerun::datatypes::Rgba32(r, g, b, a));
      }
    } else {
      if (label.empty()) {
        annotations.emplace_back(class_id);
      } else {
        annotations.emplace_back(class_id, label);
      }
    }
  }

  if (!annotations.empty()) {
    std::vector<::rerun::datatypes::ClassDescriptionMapElem> class_map;
    class_map.reserve(annotations.size());

    for (auto& ann : annotations) {
      class_map.emplace_back(::rerun::datatypes::ClassDescription(std::move(ann)));
    }

    rec.log_static(entity_path, ::rerun::archetypes::AnnotationContext(
                                    ::rerun::components::AnnotationContext(std::move(class_map))));
  }

  return true;
}

bool RerunConverter::log_capsules3d(::rerun::RecordingStream& rec, const std::string& entity_path,
                                    const RerunMap& mapping, const google::protobuf::Message& msg) {
  std::string entities_src;
  const FieldMapping* length_mapping = nullptr;
  const FieldMapping* radius_mapping = nullptr;
  const FieldMapping* center_x_mapping = nullptr;
  const FieldMapping* center_y_mapping = nullptr;
  const FieldMapping* center_z_mapping = nullptr;
  const FieldMapping* color_r_mapping = nullptr;
  const FieldMapping* color_g_mapping = nullptr;
  const FieldMapping* color_b_mapping = nullptr;
  const FieldMapping* color_a_mapping = nullptr;
  const FieldMapping* qx_mapping = nullptr;
  const FieldMapping* qy_mapping = nullptr;
  const FieldMapping* qz_mapping = nullptr;
  const FieldMapping* qw_mapping = nullptr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "entities") {
      entities_src = fm.source;
    } else if (fm.target == "length") {
      length_mapping = &fm;
    } else if (fm.target == "radius") {
      radius_mapping = &fm;
    } else if (fm.target == "center_x") {
      center_x_mapping = &fm;
    } else if (fm.target == "center_y") {
      center_y_mapping = &fm;
    } else if (fm.target == "center_z") {
      center_z_mapping = &fm;
    } else if (fm.target == "color_r") {
      color_r_mapping = &fm;
    } else if (fm.target == "color_g") {
      color_g_mapping = &fm;
    } else if (fm.target == "color_b") {
      color_b_mapping = &fm;
    } else if (fm.target == "color_a") {
      color_a_mapping = &fm;
    } else if (fm.target == "qx") {
      qx_mapping = &fm;
    } else if (fm.target == "qy") {
      qy_mapping = &fm;
    } else if (fm.target == "qz") {
      qz_mapping = &fm;
    } else if (fm.target == "qw") {
      qw_mapping = &fm;
    }
  }

  FieldMapping empty_fm;
  auto get_value = [&empty_fm](const google::protobuf::Message& value, const FieldMapping* field_mapping,
                               std::string_view fallback) {
    return field_mapping ? get_proto_double(value, field_mapping->source, *field_mapping)
                         : get_proto_double(value, fallback, empty_fm);
  };

  const auto* desc = msg.GetDescriptor();
  const auto* ref = msg.GetReflection();

  std::string field_name = entities_src.empty() ? "capsules" : entities_src;
  const auto* vec_field = find_proto_field_cached(*desc, field_name);

  if VUNLIKELY (!vec_field || !vec_field->is_repeated()) {
    auto length = static_cast<float>(get_value(msg, length_mapping, "length"));
    auto radius = static_cast<float>(get_value(msg, radius_mapping, "radius"));

    auto capsules = ::rerun::archetypes::Capsules3D::from_lengths_and_radii({length}, {radius});

    auto cx = static_cast<float>(get_value(msg, center_x_mapping, "center_x"));
    auto cy = static_cast<float>(get_value(msg, center_y_mapping, "center_y"));
    auto cz = static_cast<float>(get_value(msg, center_z_mapping, "center_z"));
    capsules = std::move(capsules).with_translations({::rerun::components::Translation3D(cx, cy, cz)});

    if (qx_mapping) {
      auto qx = static_cast<float>(get_value(msg, qx_mapping, {}));
      auto qy = static_cast<float>(get_value(msg, qy_mapping, {}));
      auto qz = static_cast<float>(get_value(msg, qz_mapping, {}));
      auto qw = static_cast<float>(get_value(msg, qw_mapping, {}));
      capsules = std::move(capsules).with_quaternions({::rerun::datatypes::Quaternion::from_xyzw(qx, qy, qz, qw)});
    }

    if (color_r_mapping) {
      auto r = checked_unsigned_cast<uint8_t>(get_value(msg, color_r_mapping, {}));
      auto g = checked_unsigned_cast<uint8_t>(get_value(msg, color_g_mapping, {}));
      auto b = checked_unsigned_cast<uint8_t>(get_value(msg, color_b_mapping, {}));
      auto a = checked_unsigned_cast<uint8_t>(color_a_mapping ? get_value(msg, color_a_mapping, {}) : 255.0);
      capsules = std::move(capsules).with_colors({::rerun::Color(r, g, b, a)});
    }

    rec.log(entity_path, capsules);
    return true;
  }

  int count = ref->FieldSize(msg, vec_field);
  std::vector<float> lengths;
  std::vector<float> radii;
  std::vector<::rerun::components::Translation3D> translations;
  std::vector<::rerun::components::RotationQuat> quaternions;
  std::vector<::rerun::Color> colors;
  lengths.reserve(count);
  radii.reserve(count);
  translations.reserve(count);

  for (int i = 0; i < count; ++i) {
    if (vec_field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      continue;
    }

    const auto& item = ref->GetRepeatedMessage(msg, vec_field, i);
    lengths.emplace_back(static_cast<float>(get_value(item, length_mapping, "length")));
    radii.emplace_back(static_cast<float>(get_value(item, radius_mapping, "radius")));
    auto cx = static_cast<float>(get_value(item, center_x_mapping, "center_x"));
    auto cy = static_cast<float>(get_value(item, center_y_mapping, "center_y"));
    auto cz = static_cast<float>(get_value(item, center_z_mapping, "center_z"));
    translations.emplace_back(cx, cy, cz);

    if (qx_mapping) {
      auto qx = static_cast<float>(get_value(item, qx_mapping, {}));
      auto qy = static_cast<float>(get_value(item, qy_mapping, {}));
      auto qz = static_cast<float>(get_value(item, qz_mapping, {}));
      auto qw = static_cast<float>(get_value(item, qw_mapping, {}));
      quaternions.emplace_back(::rerun::datatypes::Quaternion::from_xyzw(qx, qy, qz, qw));
    }

    if (color_r_mapping) {
      auto r = checked_unsigned_cast<uint8_t>(get_value(item, color_r_mapping, {}));
      auto g = checked_unsigned_cast<uint8_t>(get_value(item, color_g_mapping, {}));
      auto b = checked_unsigned_cast<uint8_t>(get_value(item, color_b_mapping, {}));
      auto a = checked_unsigned_cast<uint8_t>(color_a_mapping ? get_value(item, color_a_mapping, {}) : 255.0);
      colors.emplace_back(r, g, b, a);
    }
  }

  if (!lengths.empty()) {
    auto capsules = ::rerun::archetypes::Capsules3D::from_lengths_and_radii(std::move(lengths), std::move(radii));
    capsules = std::move(capsules).with_translations(std::move(translations));

    if (!quaternions.empty()) {
      capsules = std::move(capsules).with_quaternions(std::move(quaternions));
    }

    if (!colors.empty()) {
      capsules = std::move(capsules).with_colors(std::move(colors));
    }

    rec.log(entity_path, capsules);
  }

  return true;
}

bool RerunConverter::log_encoded_depth_image(::rerun::RecordingStream& rec, const std::string& entity_path,
                                             const RerunMap& mapping, const google::protobuf::Message& msg) {
  std::string data_src;
  const FieldMapping* media_type_mapping = nullptr;
  const FieldMapping* meter_mapping = nullptr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "data") {
      data_src = fm.source;
    } else if (fm.target == "media_type") {
      media_type_mapping = &fm;
    } else if (fm.target == "meter") {
      meter_mapping = &fm;
    }
  }

  auto raw_data = get_proto_bytes(msg, data_src.empty() ? "data" : data_src);

  if VUNLIKELY (raw_data.empty()) {
    MLOG_W("EncodedDepthImage: no data field found");
    return false;
  }

  auto blob = ::rerun::Collection<uint8_t>::borrow(raw_data.data(), raw_data.size());
  auto depth_image = ::rerun::archetypes::EncodedDepthImage(::rerun::components::Blob(std::move(blob)));

  FieldMapping empty_fm;
  auto media_type_val = media_type_mapping ? get_proto_string(msg, media_type_mapping->source, *media_type_mapping)
                                           : get_proto_string(msg, "media_type", empty_fm);

  if (!media_type_val.empty()) {
    depth_image = std::move(depth_image).with_media_type(::rerun::components::MediaType(media_type_val));
  }

  if (meter_mapping) {
    auto meter_val = static_cast<float>(get_proto_double(msg, meter_mapping->source, *meter_mapping));

    if (meter_val > 0.0F) {
      depth_image = std::move(depth_image).with_meter(::rerun::components::DepthMeter(meter_val));
    }
  } else {
    auto meter_val = static_cast<float>(get_proto_double(msg, "meter", empty_fm));

    if (meter_val > 0.0F) {
      depth_image = std::move(depth_image).with_meter(::rerun::components::DepthMeter(meter_val));
    }
  }

  rec.log(entity_path, depth_image);
  return true;
}

bool RerunConverter::log_asset3d(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                                 const google::protobuf::Message& msg) {
  std::string data_src;
  const FieldMapping* media_type_mapping = nullptr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "data") {
      data_src = fm.source;
    } else if (fm.target == "media_type") {
      media_type_mapping = &fm;
    }
  }

  auto raw_data = get_proto_bytes(msg, data_src.empty() ? "data" : data_src);

  if VUNLIKELY (raw_data.empty()) {
    MLOG_W("Asset3D: no data field found");
    return false;
  }

  FieldMapping empty_fm;
  auto media_type_val = media_type_mapping ? get_proto_string(msg, media_type_mapping->source, *media_type_mapping)
                                           : get_proto_string(msg, "media_type", empty_fm);

  auto blob = ::rerun::Collection<uint8_t>::borrow(raw_data.data(), raw_data.size());

  auto asset = ::rerun::archetypes::Asset3D::from_file_contents(
      std::move(blob), media_type_val.empty() ? std::optional<::rerun::components::MediaType>(std::nullopt)
                                              : std::optional<::rerun::components::MediaType>(media_type_val));

  rec.log(entity_path, asset);
  return true;
}

bool RerunConverter::log_graph_nodes(::rerun::RecordingStream& rec, const std::string& entity_path,
                                     const RerunMap& mapping, const google::protobuf::Message& msg) {
  std::string entities_src;
  const FieldMapping* node_id_mapping = nullptr;
  const FieldMapping* pos_x_mapping = nullptr;
  const FieldMapping* pos_y_mapping = nullptr;
  const FieldMapping* label_mapping = nullptr;
  const FieldMapping* color_r_mapping = nullptr;
  const FieldMapping* color_g_mapping = nullptr;
  const FieldMapping* color_b_mapping = nullptr;
  const FieldMapping* color_a_mapping = nullptr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "entities" || fm.target == "nodes") {
      entities_src = fm.source;
    } else if (fm.target == "node_id") {
      node_id_mapping = &fm;
    } else if (fm.target == "position_x") {
      pos_x_mapping = &fm;
    } else if (fm.target == "position_y") {
      pos_y_mapping = &fm;
    } else if (fm.target == "label") {
      label_mapping = &fm;
    } else if (fm.target == "color_r") {
      color_r_mapping = &fm;
    } else if (fm.target == "color_g") {
      color_g_mapping = &fm;
    } else if (fm.target == "color_b") {
      color_b_mapping = &fm;
    } else if (fm.target == "color_a") {
      color_a_mapping = &fm;
    }
  }

  FieldMapping empty_fm;
  auto get_value = [&empty_fm](const google::protobuf::Message& value, const FieldMapping* field_mapping,
                               std::string_view fallback) {
    return field_mapping ? get_proto_double(value, field_mapping->source, *field_mapping)
                         : get_proto_double(value, fallback, empty_fm);
  };

  const auto* desc = msg.GetDescriptor();
  const auto* ref = msg.GetReflection();

  std::string field_name = entities_src.empty() ? "nodes" : entities_src;
  const auto* vec_field = find_proto_field_cached(*desc, field_name);

  if VUNLIKELY (!vec_field || !vec_field->is_repeated()) {
    auto node_id = node_id_mapping ? get_proto_string(msg, node_id_mapping->source, *node_id_mapping)
                                   : get_proto_string(msg, "node_id", empty_fm);

    if (node_id.empty()) {
      return false;
    }

    auto nodes = ::rerun::archetypes::GraphNodes({::rerun::components::GraphNode(node_id)});

    if (pos_x_mapping) {
      auto px = static_cast<float>(get_value(msg, pos_x_mapping, {}));
      auto py = static_cast<float>(get_value(msg, pos_y_mapping, {}));
      nodes = std::move(nodes).with_positions({::rerun::components::Position2D(px, py)});
    }

    if (label_mapping) {
      auto label = get_proto_string(msg, label_mapping->source, *label_mapping);

      if (!label.empty()) {
        nodes = std::move(nodes).with_labels({::rerun::components::Text(label)});
      }
    }

    if (color_r_mapping) {
      auto r = checked_unsigned_cast<uint8_t>(get_value(msg, color_r_mapping, {}));
      auto g = checked_unsigned_cast<uint8_t>(get_value(msg, color_g_mapping, {}));
      auto b = checked_unsigned_cast<uint8_t>(get_value(msg, color_b_mapping, {}));
      auto a = checked_unsigned_cast<uint8_t>(color_a_mapping ? get_value(msg, color_a_mapping, {}) : 255.0);
      nodes = std::move(nodes).with_colors({::rerun::Color(r, g, b, a)});
    }

    rec.log(entity_path, nodes);
    return true;
  }

  int count = ref->FieldSize(msg, vec_field);
  std::vector<::rerun::components::GraphNode> node_ids;
  std::vector<::rerun::components::Position2D> positions;
  std::vector<::rerun::components::Text> labels;
  std::vector<::rerun::Color> colors;
  node_ids.reserve(count);
  positions.reserve(count);
  labels.reserve(count);
  colors.reserve(count);

  for (int i = 0; i < count; ++i) {
    if (vec_field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      continue;
    }

    const auto& item = ref->GetRepeatedMessage(msg, vec_field, i);
    auto node_id = node_id_mapping ? get_proto_string(item, node_id_mapping->source, *node_id_mapping)
                                   : get_proto_string(item, "node_id", empty_fm);

    if VUNLIKELY (node_id.empty()) {
      continue;
    }

    node_ids.emplace_back(node_id);

    if (pos_x_mapping) {
      auto px = static_cast<float>(get_value(item, pos_x_mapping, {}));
      auto py = static_cast<float>(get_value(item, pos_y_mapping, {}));
      positions.emplace_back(px, py);
    }

    if (label_mapping) {
      auto label = get_proto_string(item, label_mapping->source, *label_mapping);
      labels.emplace_back(label);
    }

    if (color_r_mapping) {
      auto r = checked_unsigned_cast<uint8_t>(get_value(item, color_r_mapping, {}));
      auto g = checked_unsigned_cast<uint8_t>(get_value(item, color_g_mapping, {}));
      auto b = checked_unsigned_cast<uint8_t>(get_value(item, color_b_mapping, {}));
      auto a = checked_unsigned_cast<uint8_t>(color_a_mapping ? get_value(item, color_a_mapping, {}) : 255.0);
      colors.emplace_back(r, g, b, a);
    }
  }

  if (!node_ids.empty()) {
    auto nodes = ::rerun::archetypes::GraphNodes(std::move(node_ids));

    if (!positions.empty()) {
      nodes = std::move(nodes).with_positions(std::move(positions));
    }

    if (!labels.empty()) {
      nodes = std::move(nodes).with_labels(std::move(labels));
    }

    if (!colors.empty()) {
      nodes = std::move(nodes).with_colors(std::move(colors));
    }

    rec.log(entity_path, nodes);
  }

  return true;
}

bool RerunConverter::log_graph_edges(::rerun::RecordingStream& rec, const std::string& entity_path,
                                     const RerunMap& mapping, const google::protobuf::Message& msg) {
  std::string entities_src;
  const FieldMapping* source_mapping = nullptr;
  const FieldMapping* target_mapping = nullptr;
  const FieldMapping* graph_type_mapping = nullptr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "entities" || fm.target == "edges") {
      entities_src = fm.source;
    } else if (fm.target == "source") {
      source_mapping = &fm;
    } else if (fm.target == "target") {
      target_mapping = &fm;
    } else if (fm.target == "graph_type") {
      graph_type_mapping = &fm;
    }
  }

  const auto* desc = msg.GetDescriptor();
  const auto* ref = msg.GetReflection();

  std::string field_name = entities_src.empty() ? "edges" : entities_src;
  const auto* vec_field = find_proto_field_cached(*desc, field_name);

  if VUNLIKELY (!vec_field || !vec_field->is_repeated()) {
    FieldMapping empty_fm;
    auto src = source_mapping ? get_proto_string(msg, source_mapping->source, *source_mapping)
                              : get_proto_string(msg, "source", empty_fm);
    auto tgt = target_mapping ? get_proto_string(msg, target_mapping->source, *target_mapping)
                              : get_proto_string(msg, "target", empty_fm);

    if (src.empty() || tgt.empty()) {
      return false;
    }

    auto edges = ::rerun::archetypes::GraphEdges({::rerun::components::GraphEdge(src, tgt)});

    auto type_str = graph_type_mapping ? get_proto_string(msg, graph_type_mapping->source, *graph_type_mapping)
                                       : get_proto_string(msg, "graph_type", empty_fm);

    if (type_str == "directed" || type_str == "Directed") {
      edges = std::move(edges).with_graph_type(::rerun::components::GraphType::Directed);
    }

    rec.log(entity_path, edges);
    return true;
  }

  int count = ref->FieldSize(msg, vec_field);
  std::vector<::rerun::components::GraphEdge> edge_list;
  edge_list.reserve(count);

  for (int i = 0; i < count; ++i) {
    if (vec_field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      continue;
    }

    const auto& item = ref->GetRepeatedMessage(msg, vec_field, i);
    FieldMapping empty_fm;
    auto src = source_mapping ? get_proto_string(item, source_mapping->source, *source_mapping)
                              : get_proto_string(item, "source", empty_fm);
    auto tgt = target_mapping ? get_proto_string(item, target_mapping->source, *target_mapping)
                              : get_proto_string(item, "target", empty_fm);

    if VUNLIKELY (src.empty() || tgt.empty()) {
      continue;
    }

    edge_list.emplace_back(src, tgt);
  }

  if (!edge_list.empty()) {
    auto edges = ::rerun::archetypes::GraphEdges(std::move(edge_list));

    FieldMapping empty_fm;
    auto type_str = graph_type_mapping ? get_proto_string(msg, graph_type_mapping->source, *graph_type_mapping)
                                       : get_proto_string(msg, "graph_type", empty_fm);

    if (type_str == "directed" || type_str == "Directed") {
      edges = std::move(edges).with_graph_type(::rerun::components::GraphType::Directed);
    }

    rec.log(entity_path, edges);
  }

  return true;
}

bool RerunConverter::log_view_coordinates(::rerun::RecordingStream& rec, const std::string& entity_path,
                                          const RerunMap& mapping, const google::protobuf::Message& msg) {
  const FieldMapping* system_mapping = nullptr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "system" || fm.target == "coordinates") {
      system_mapping = &fm;
    }
  }

  FieldMapping empty_fm;
  auto system_str = system_mapping ? get_proto_string(msg, system_mapping->source, *system_mapping)
                                   : get_proto_string(msg, "system", empty_fm);

  if (system_str.empty()) {
    system_str = get_proto_string(msg, "coordinates", empty_fm);
  }

  log_view_coordinates_value(rec, entity_path, system_str);

  return true;
}

bool RerunConverter::log_instance_poses3d(::rerun::RecordingStream& rec, const std::string& entity_path,
                                          const RerunMap& mapping, const google::protobuf::Message& msg) {
  std::string entities_src;
  const FieldMapping* tx_mapping = nullptr;
  const FieldMapping* ty_mapping = nullptr;
  const FieldMapping* tz_mapping = nullptr;
  const FieldMapping* qx_mapping = nullptr;
  const FieldMapping* qy_mapping = nullptr;
  const FieldMapping* qz_mapping = nullptr;
  const FieldMapping* qw_mapping = nullptr;
  const FieldMapping* sx_mapping = nullptr;
  const FieldMapping* sy_mapping = nullptr;
  const FieldMapping* sz_mapping = nullptr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "entities" || fm.target == "poses") {
      entities_src = fm.source;
    } else if (fm.target == "translation_x") {
      tx_mapping = &fm;
    } else if (fm.target == "translation_y") {
      ty_mapping = &fm;
    } else if (fm.target == "translation_z") {
      tz_mapping = &fm;
    } else if (fm.target == "qx") {
      qx_mapping = &fm;
    } else if (fm.target == "qy") {
      qy_mapping = &fm;
    } else if (fm.target == "qz") {
      qz_mapping = &fm;
    } else if (fm.target == "qw") {
      qw_mapping = &fm;
    } else if (fm.target == "scale_x") {
      sx_mapping = &fm;
    } else if (fm.target == "scale_y") {
      sy_mapping = &fm;
    } else if (fm.target == "scale_z") {
      sz_mapping = &fm;
    }
  }

  FieldMapping empty_fm;
  auto get_value = [&empty_fm](const google::protobuf::Message& value, const FieldMapping* field_mapping,
                               std::string_view fallback) {
    return field_mapping ? get_proto_double(value, field_mapping->source, *field_mapping)
                         : get_proto_double(value, fallback, empty_fm);
  };

  const auto* desc = msg.GetDescriptor();
  const auto* ref = msg.GetReflection();

  std::string field_name = entities_src.empty() ? "poses" : entities_src;
  const auto* vec_field = find_proto_field_cached(*desc, field_name);

  if VUNLIKELY (!vec_field || !vec_field->is_repeated()) {
    auto poses = ::rerun::archetypes::InstancePoses3D();

    auto tx = static_cast<float>(get_value(msg, tx_mapping, "translation_x"));
    auto ty = static_cast<float>(get_value(msg, ty_mapping, "translation_y"));
    auto tz = static_cast<float>(get_value(msg, tz_mapping, "translation_z"));
    poses = std::move(poses).with_translations({::rerun::components::Translation3D(tx, ty, tz)});

    if (qx_mapping) {
      auto qx = static_cast<float>(get_value(msg, qx_mapping, {}));
      auto qy = static_cast<float>(get_value(msg, qy_mapping, {}));
      auto qz = static_cast<float>(get_value(msg, qz_mapping, {}));
      auto qw = static_cast<float>(get_value(msg, qw_mapping, {}));
      poses = std::move(poses).with_quaternions({::rerun::datatypes::Quaternion::from_xyzw(qx, qy, qz, qw)});
    }

    if (sx_mapping) {
      auto sx = static_cast<float>(get_value(msg, sx_mapping, {}));
      auto sy = static_cast<float>(get_value(msg, sy_mapping, {}));
      auto sz = static_cast<float>(get_value(msg, sz_mapping, {}));
      poses = std::move(poses).with_scales({::rerun::components::Scale3D(sx, sy, sz)});
    }

    rec.log(entity_path, poses);
    return true;
  }

  int count = ref->FieldSize(msg, vec_field);
  std::vector<::rerun::components::Translation3D> translations;
  std::vector<::rerun::components::RotationQuat> quaternions;
  std::vector<::rerun::components::Scale3D> scales;
  translations.reserve(count);

  for (int i = 0; i < count; ++i) {
    if (vec_field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      continue;
    }

    const auto& item = ref->GetRepeatedMessage(msg, vec_field, i);
    auto tx = static_cast<float>(get_value(item, tx_mapping, "translation_x"));
    auto ty = static_cast<float>(get_value(item, ty_mapping, "translation_y"));
    auto tz = static_cast<float>(get_value(item, tz_mapping, "translation_z"));
    translations.emplace_back(tx, ty, tz);

    if (qx_mapping) {
      auto qx = static_cast<float>(get_value(item, qx_mapping, {}));
      auto qy = static_cast<float>(get_value(item, qy_mapping, {}));
      auto qz = static_cast<float>(get_value(item, qz_mapping, {}));
      auto qw = static_cast<float>(get_value(item, qw_mapping, {}));
      quaternions.emplace_back(::rerun::datatypes::Quaternion::from_xyzw(qx, qy, qz, qw));
    }

    if (sx_mapping) {
      auto sx = static_cast<float>(get_value(item, sx_mapping, {}));
      auto sy = static_cast<float>(get_value(item, sy_mapping, {}));
      auto sz = static_cast<float>(get_value(item, sz_mapping, {}));
      scales.emplace_back(sx, sy, sz);
    }
  }

  if (!translations.empty()) {
    auto poses = ::rerun::archetypes::InstancePoses3D();
    poses = std::move(poses).with_translations(std::move(translations));

    if (!quaternions.empty()) {
      poses = std::move(poses).with_quaternions(std::move(quaternions));
    }

    if (!scales.empty()) {
      poses = std::move(poses).with_scales(std::move(scales));
    }

    rec.log(entity_path, poses);
  }

  return true;
}

bool RerunConverter::log_asset_video(::rerun::RecordingStream& rec, const std::string& entity_path,
                                     const RerunMap& mapping, const google::protobuf::Message& msg) {
  std::string data_src;
  const FieldMapping* media_type_mapping = nullptr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "data") {
      data_src = fm.source;
    } else if (fm.target == "media_type") {
      media_type_mapping = &fm;
    }
  }

  auto raw_data = get_proto_bytes(msg, data_src.empty() ? "data" : data_src);

  if VUNLIKELY (raw_data.empty()) {
    MLOG_W("AssetVideo: no data field found");
    return false;
  }

  FieldMapping empty_fm;
  auto media_type_val = media_type_mapping ? get_proto_string(msg, media_type_mapping->source, *media_type_mapping)
                                           : get_proto_string(msg, "media_type", empty_fm);

  auto blob = ::rerun::Collection<uint8_t>::borrow(raw_data.data(), raw_data.size());
  auto video = ::rerun::archetypes::AssetVideo::from_bytes(
      std::move(blob), media_type_val.empty() ? std::optional<::rerun::components::MediaType>(std::nullopt)
                                              : std::optional<::rerun::components::MediaType>(media_type_val));

  rec.log(entity_path, video);
  return true;
}

bool RerunConverter::log_video_frame_reference(::rerun::RecordingStream& rec, const std::string& entity_path,
                                               const RerunMap& mapping, const google::protobuf::Message& msg) {
  const FieldMapping* timestamp_ns_mapping = nullptr;
  const FieldMapping* video_reference_mapping = nullptr;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "timestamp_ns") {
      timestamp_ns_mapping = &fm;
    } else if (fm.target == "video_reference") {
      video_reference_mapping = &fm;
    }
  }

  FieldMapping empty_fm;
  auto ts_ns = static_cast<int64_t>(timestamp_ns_mapping
                                        ? get_proto_double(msg, timestamp_ns_mapping->source, *timestamp_ns_mapping)
                                        : get_proto_double(msg, "timestamp_ns", empty_fm));

  auto frame_ref =
      ::rerun::archetypes::VideoFrameReference(::rerun::components::VideoTimestamp(std::chrono::nanoseconds(ts_ns)));

  if (video_reference_mapping) {
    auto ref_path = get_proto_string(msg, video_reference_mapping->source, *video_reference_mapping);

    if (!ref_path.empty()) {
      frame_ref = std::move(frame_ref).with_video_reference(ref_path);
    }
  }

  rec.log(entity_path, frame_ref);
  return true;
}

bool RerunConverter::log_tensor(::rerun::RecordingStream& rec, const std::string& entity_path, const RerunMap& mapping,
                                const google::protobuf::Message& msg) {
  std::string shape_src;
  std::string data_src;
  std::string dim_names_src;

  for (const auto& fm : mapping.field_mappings) {
    if (fm.target == "shape") {
      shape_src = fm.source;
    } else if (fm.target == "data") {
      data_src = fm.source;
    } else if (fm.target == "dim_names") {
      dim_names_src = fm.source;
    }
  }

  const auto* desc = msg.GetDescriptor();
  const auto* ref = msg.GetReflection();

  std::string shape_field_name = shape_src.empty() ? "shape" : shape_src;
  const auto* shape_field = find_proto_field_cached(*desc, shape_field_name);

  std::vector<uint64_t> shape;

  if (shape_field && shape_field->is_repeated()) {
    int count = ref->FieldSize(msg, shape_field);

    for (int i = 0; i < count; ++i) {
      if (shape_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_UINT64) {
        shape.emplace_back(ref->GetRepeatedUInt64(msg, shape_field, i));
      } else if (shape_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_INT64) {
        shape.emplace_back(static_cast<uint64_t>(ref->GetRepeatedInt64(msg, shape_field, i)));
      } else if (shape_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_UINT32) {
        shape.emplace_back(static_cast<uint64_t>(ref->GetRepeatedUInt32(msg, shape_field, i)));
      } else if (shape_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_INT32) {
        shape.emplace_back(static_cast<uint64_t>(ref->GetRepeatedInt32(msg, shape_field, i)));
      }
    }
  }

  if VUNLIKELY (shape.empty()) {
    MLOG_W("Tensor: no shape field found");
    return false;
  }

  auto raw_data = get_proto_bytes(msg, data_src.empty() ? "data" : data_src);

  if VUNLIKELY (raw_data.empty()) {
    MLOG_W("Tensor: no data field found");
    return false;
  }

  auto data_collection = ::rerun::Collection<uint8_t>::borrow(raw_data.data(), raw_data.size());
  auto tensor =
      ::rerun::archetypes::Tensor(std::move(shape), ::rerun::datatypes::TensorBuffer(std::move(data_collection)));

  std::string dim_names_field = dim_names_src.empty() ? "dim_names" : dim_names_src;
  const auto* dn_field = find_proto_field_cached(*desc, dim_names_field);

  if (dn_field && dn_field->is_repeated() &&
      dn_field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING) {
    int count = ref->FieldSize(msg, dn_field);
    std::vector<std::string> names;
    names.reserve(count);

    for (int i = 0; i < count; ++i) {
      names.emplace_back(ref->GetRepeatedString(msg, dn_field, i));
    }

    tensor = std::move(tensor).with_dim_names(std::move(names));
  }

  rec.log(entity_path, tensor);
  return true;
}

// NOLINTNEXTLINE(google-readability-function-size)
bool RerunConverter::log_plugin_json(::rerun::RecordingStream& rec, const std::string& entity_path,
                                     const std::string& archetype, const Json& j) {
  if (archetype == "Points3D") {
    if (!j.contains("positions") || !j["positions"].is_array()) {
      return false;
    }

    std::vector<::rerun::Position3D> positions;

    for (const auto& p : j["positions"]) {
      if (p.is_array() && p.size() >= 3) {
        positions.emplace_back(p[0].get<float>(), p[1].get<float>(), p[2].get<float>());
      }
    }

    auto points = ::rerun::archetypes::Points3D(std::move(positions));

    if (j.contains("colors") && j["colors"].is_array()) {
      std::vector<::rerun::Color> colors;

      for (const auto& c : j["colors"]) {
        if (c.is_array() && c.size() >= 3) {
          colors.emplace_back(c[0].get<uint8_t>(), c[1].get<uint8_t>(), c[2].get<uint8_t>(),
                              static_cast<uint8_t>(c.size() >= 4 ? c[3].get<uint8_t>() : 255));
        }
      }

      points = std::move(points).with_colors(std::move(colors));
    }

    if (j.contains("radii") && j["radii"].is_array()) {
      std::vector<::rerun::Radius> radii;

      for (const auto& r : j["radii"]) {
        radii.emplace_back(r.get<float>());
      }

      points = std::move(points).with_radii(std::move(radii));
    }

    rec.log(entity_path, points);
    return true;
  }

  if (archetype == "GeoPoints") {
    if (!j.contains("lat_deg") || !j["lat_deg"].is_array()) {
      return false;
    }

    std::vector<::rerun::components::LatLon> lat_lons;
    const auto& lats = j["lat_deg"];
    const auto& lons = j.value("lon_deg", Json::array());

    for (size_t i = 0; i < lats.size(); ++i) {
      auto lat = lats[i].get<double>();
      auto lon = (i < lons.size()) ? lons[i].get<double>() : 0.0;
      lat_lons.emplace_back(lat, lon);
    }

    rec.log(entity_path, ::rerun::archetypes::GeoPoints(std::move(lat_lons)));
    return true;
  }

  if (archetype == "EncodedImage") {
    auto data = decode_plugin_binary(j);

    if (data.empty()) {
      return false;
    }

    auto media_type = infer_media_type(j);
    auto blob = ::rerun::Collection<uint8_t>::borrow(data.data(), data.size());

    if (media_type.empty()) {
      rec.log(entity_path, ::rerun::archetypes::EncodedImage::from_bytes(blob));
    } else {
      rec.log(entity_path,
              ::rerun::archetypes::EncodedImage::from_bytes(blob, ::rerun::components::MediaType(media_type)));
    }

    return true;
  }

  if (archetype == "TextLog") {
    auto text = j.value("text", std::string(""));
    auto level = j.value("level", std::string(""));

    auto text_log = ::rerun::TextLog(text);

    if (!level.empty()) {
      text_log = std::move(text_log).with_level(::rerun::components::TextLogLevel(level));
    }

    rec.log(entity_path, text_log);
    return true;
  }

  if (archetype == "Scalars") {
    if (j.contains("value")) {
      rec.log(entity_path, ::rerun::archetypes::Scalars(j["value"].get<double>()));
      return true;
    }

    return false;
  }

  if (archetype == "Transform3D") {
    auto translation = ::rerun::Vec3D{0.0F, 0.0F, 0.0F};

    if (j.contains("translation") && j["translation"].is_array() && j["translation"].size() >= 3) {
      translation = ::rerun::Vec3D{j["translation"][0].get<float>(), j["translation"][1].get<float>(),
                                   j["translation"][2].get<float>()};
    }

    auto quaternion = ::rerun::Quaternion::from_xyzw(0.0F, 0.0F, 0.0F, 1.0F);

    if (j.contains("rotation_quat") && j["rotation_quat"].is_array() && j["rotation_quat"].size() >= 4) {
      quaternion =
          ::rerun::Quaternion::from_xyzw(j["rotation_quat"][0].get<float>(), j["rotation_quat"][1].get<float>(),
                                         j["rotation_quat"][2].get<float>(), j["rotation_quat"][3].get<float>());
    }

    rec.log(entity_path, ::rerun::archetypes::Transform3D().with_translation(translation).with_quaternion(quaternion));
    return true;
  }

  if (archetype == "Boxes3D") {
    if (!j.contains("half_sizes") || !j["half_sizes"].is_array()) {
      return false;
    }

    std::vector<::rerun::HalfSize3D> half_sizes;

    for (const auto& hs : j["half_sizes"]) {
      if (hs.is_array() && hs.size() >= 3) {
        half_sizes.emplace_back(hs[0].get<float>(), hs[1].get<float>(), hs[2].get<float>());
      }
    }

    auto boxes = ::rerun::archetypes::Boxes3D::from_half_sizes(std::move(half_sizes));

    if (j.contains("centers") && j["centers"].is_array()) {
      std::vector<::rerun::Position3D> centers;

      for (const auto& c : j["centers"]) {
        if (c.is_array() && c.size() >= 3) {
          centers.emplace_back(c[0].get<float>(), c[1].get<float>(), c[2].get<float>());
        }
      }

      boxes = std::move(boxes).with_centers(std::move(centers));
    }

    if (j.contains("quaternions") && j["quaternions"].is_array()) {
      std::vector<::rerun::components::RotationQuat> quats;

      for (const auto& q : j["quaternions"]) {
        if (q.is_array() && q.size() >= 4) {
          quats.emplace_back(::rerun::datatypes::Quaternion::from_xyzw(q[0].get<float>(), q[1].get<float>(),
                                                                       q[2].get<float>(), q[3].get<float>()));
        }
      }

      boxes = std::move(boxes).with_quaternions(std::move(quats));
    }

    if (j.contains("colors") && j["colors"].is_array()) {
      std::vector<::rerun::Color> colors;

      for (const auto& c : j["colors"]) {
        if (c.is_array() && c.size() >= 3) {
          colors.emplace_back(c[0].get<uint8_t>(), c[1].get<uint8_t>(), c[2].get<uint8_t>(),
                              static_cast<uint8_t>(c.size() >= 4 ? c[3].get<uint8_t>() : 255));
        }
      }

      boxes = std::move(boxes).with_colors(std::move(colors));
    }

    if (j.contains("labels") && j["labels"].is_array()) {
      std::vector<::rerun::Text> labels;

      for (const auto& l : j["labels"]) {
        labels.emplace_back(l.get<std::string>());
      }

      boxes = std::move(boxes).with_labels(std::move(labels));
    }

    rec.log(entity_path, boxes);
    return true;
  }

  if (archetype == "Pinhole") {
    if (!j.contains("image_from_camera") || !j["image_from_camera"].is_array()) {
      return false;
    }

    const auto& mat = j["image_from_camera"];

    if (mat.size() >= 3 && mat[0].is_array() && mat[0].size() >= 3 && mat[1].is_array() && mat[1].size() >= 3) {
      auto fx = mat[0][0].get<float>();
      auto fy = mat[1][1].get<float>();
      auto cx = mat[0][2].get<float>();
      auto cy = mat[1][2].get<float>();

      auto width = cx * 2.0F;
      auto height = cy * 2.0F;
      const auto resolution = j.find("resolution");

      if (resolution != j.end() && resolution->is_array() && resolution->size() >= 2) {
        width = (*resolution)[0].get<float>();
        height = (*resolution)[1].get<float>();
      }

      if VUNLIKELY (!std::isfinite(fx) || !std::isfinite(fy) || !std::isfinite(cx) || !std::isfinite(cy) ||
                    !std::isfinite(width) || !std::isfinite(height) || fx <= 0.0F || fy <= 0.0F || width <= 0.0F ||
                    height <= 0.0F) {
        return false;
      }

      auto pinhole = make_pinhole(fx, fy, cx, cy, width, height);

      rec.log(entity_path, pinhole);
      return true;
    }

    return false;
  }

  if (archetype == "DepthImage") {
    uint32_t width = 0;
    uint32_t height = 0;

    if (!resolve_plugin_image_size(j, width, height)) {
      return false;
    }

    auto data = decode_plugin_binary(j);

    if (data.empty()) {
      return false;
    }

    std::string_view datatype;
    const auto datatype_iter = j.find("datatype");

    if (datatype_iter != j.end()) {
      if VUNLIKELY (!datatype_iter->is_string()) {
        return false;
      }

      datatype = datatype_iter->get_ref<const std::string&>();
    }

    const auto meter = j.value("meter", 0.0F);
    return log_depth_image_bytes(rec, entity_path, data.data(), data.size(), width, height, datatype, meter);
  }

  if (archetype == "Image") {
    uint32_t width = 0;
    uint32_t height = 0;

    if (!resolve_plugin_image_size(j, width, height)) {
      return false;
    }

    auto data = decode_plugin_binary(j);

    if (data.empty()) {
      return false;
    }

    size_t pixel_count = 0;

    if VUNLIKELY (!mul_size(static_cast<size_t>(width), static_cast<size_t>(height), pixel_count)) {
      return false;
    }

    auto color_model = normalize_plugin_text(j.value("color_model", std::string{}));

    if (color_model.empty()) {
      color_model = normalize_plugin_text(j.value("encoding", std::string{}));
    }

    auto rerun_color_model = ::rerun::datatypes::ColorModel::L;

    if (color_model == "rgba" || color_model == "rgba8") {
      rerun_color_model = ::rerun::datatypes::ColorModel::RGBA;
    } else if (color_model == "bgra" || color_model == "bgra8") {
      rerun_color_model = ::rerun::datatypes::ColorModel::BGRA;
    } else if (color_model == "rgb" || color_model == "rgb8") {
      rerun_color_model = ::rerun::datatypes::ColorModel::RGB;
    } else if (color_model == "bgr" || color_model == "bgr8") {
      rerun_color_model = ::rerun::datatypes::ColorModel::BGR;
    } else if (!(color_model == "l" || color_model == "gray" || color_model == "grey" || color_model == "mono8" ||
                 color_model == "y8")) {
      size_t expected_size = 0;

      if (mul_size(pixel_count, 4U, expected_size) && data.size() == expected_size) {
        rerun_color_model = ::rerun::datatypes::ColorModel::RGBA;
      } else if (mul_size(pixel_count, 3U, expected_size) && data.size() == expected_size) {
        rerun_color_model = ::rerun::datatypes::ColorModel::RGB;
      }
    }

    return log_typed_image(rec, entity_path, data.data(), data.size(), width, height, rerun_color_model,
                           ::rerun::datatypes::ChannelDatatype::U8);
  }

  if (archetype == "LineStrips3D") {
    if (!j.contains("strips") || !j["strips"].is_array()) {
      return false;
    }

    std::vector<std::vector<::rerun::Position3D>> strips;

    for (const auto& strip : j["strips"]) {
      if (!strip.is_array()) {
        continue;
      }

      std::vector<::rerun::Position3D> line;

      for (const auto& p : strip) {
        if (p.is_array() && p.size() >= 3) {
          line.emplace_back(p[0].get<float>(), p[1].get<float>(), p[2].get<float>());
        }
      }

      if (!line.empty()) {
        strips.emplace_back(std::move(line));
      }
    }

    if (!strips.empty()) {
      rec.log(entity_path, ::rerun::archetypes::LineStrips3D(std::move(strips)));
    }

    return true;
  }

  if (archetype == "LineStrips2D") {
    if (!j.contains("strips") || !j["strips"].is_array()) {
      return false;
    }

    std::vector<std::vector<::rerun::Position2D>> strips;

    for (const auto& strip : j["strips"]) {
      if (!strip.is_array()) {
        continue;
      }

      std::vector<::rerun::Position2D> line;

      for (const auto& p : strip) {
        if (p.is_array() && p.size() >= 2) {
          line.emplace_back(p[0].get<float>(), p[1].get<float>());
        }
      }

      if (!line.empty()) {
        strips.emplace_back(std::move(line));
      }
    }

    if (!strips.empty()) {
      rec.log(entity_path, ::rerun::archetypes::LineStrips2D(std::move(strips)));
    }

    return true;
  }

  if (archetype == "Boxes2D") {
    if (!j.contains("half_sizes") || !j["half_sizes"].is_array()) {
      return false;
    }

    std::vector<::rerun::datatypes::Vec2D> half_sizes;

    for (const auto& hs : j["half_sizes"]) {
      if (hs.is_array() && hs.size() >= 2) {
        half_sizes.emplace_back(hs[0].get<float>(), hs[1].get<float>());
      }
    }

    auto boxes = ::rerun::archetypes::Boxes2D::from_half_sizes(std::move(half_sizes));

    if (j.contains("centers") && j["centers"].is_array()) {
      std::vector<::rerun::datatypes::Vec2D> centers;

      for (const auto& c : j["centers"]) {
        if (c.is_array() && c.size() >= 2) {
          centers.emplace_back(c[0].get<float>(), c[1].get<float>());
        }
      }

      boxes = std::move(boxes).with_centers(std::move(centers));
    }

    if (j.contains("labels") && j["labels"].is_array()) {
      std::vector<::rerun::Text> labels;

      for (const auto& l : j["labels"]) {
        labels.emplace_back(l.get<std::string>());
      }

      boxes = std::move(boxes).with_labels(std::move(labels));
    }

    rec.log(entity_path, boxes);
    return true;
  }

  if (archetype == "Arrows3D") {
    if (!j.contains("vectors") || !j["vectors"].is_array()) {
      return false;
    }

    std::vector<::rerun::Vector3D> vectors;

    for (const auto& v : j["vectors"]) {
      if (v.is_array() && v.size() >= 3) {
        vectors.emplace_back(v[0].get<float>(), v[1].get<float>(), v[2].get<float>());
      }
    }

    auto arrows = ::rerun::archetypes::Arrows3D::from_vectors(std::move(vectors));

    if (j.contains("origins") && j["origins"].is_array()) {
      std::vector<::rerun::Position3D> origins;

      for (const auto& o : j["origins"]) {
        if (o.is_array() && o.size() >= 3) {
          origins.emplace_back(o[0].get<float>(), o[1].get<float>(), o[2].get<float>());
        }
      }

      arrows = std::move(arrows).with_origins(std::move(origins));
    }

    if (j.contains("colors") && j["colors"].is_array()) {
      std::vector<::rerun::Color> colors;

      for (const auto& c : j["colors"]) {
        if (c.is_array() && c.size() >= 3) {
          colors.emplace_back(c[0].get<uint8_t>(), c[1].get<uint8_t>(), c[2].get<uint8_t>(),
                              static_cast<uint8_t>(c.size() >= 4 ? c[3].get<uint8_t>() : 255));
        }
      }

      arrows = std::move(arrows).with_colors(std::move(colors));
    }

    rec.log(entity_path, arrows);
    return true;
  }

  if (archetype == "Points2D") {
    if (!j.contains("positions") || !j["positions"].is_array()) {
      return false;
    }

    std::vector<::rerun::Position2D> positions;

    for (const auto& p : j["positions"]) {
      if (p.is_array() && p.size() >= 2) {
        positions.emplace_back(p[0].get<float>(), p[1].get<float>());
      }
    }

    auto points = ::rerun::archetypes::Points2D(std::move(positions));

    if (j.contains("colors") && j["colors"].is_array()) {
      std::vector<::rerun::Color> colors;

      for (const auto& c : j["colors"]) {
        if (c.is_array() && c.size() >= 3) {
          colors.emplace_back(c[0].get<uint8_t>(), c[1].get<uint8_t>(), c[2].get<uint8_t>(),
                              static_cast<uint8_t>(c.size() >= 4 ? c[3].get<uint8_t>() : 255));
        }
      }

      points = std::move(points).with_colors(std::move(colors));
    }

    rec.log(entity_path, points);
    return true;
  }

  if (archetype == "SegmentationImage") {
    uint32_t width = 0;
    uint32_t height = 0;

    if (!resolve_plugin_image_size(j, width, height)) {
      return false;
    }

    auto data = decode_plugin_binary(j);

    if (data.empty()) {
      return false;
    }

    size_t pixel_count = 0;

    if VUNLIKELY (!mul_size(static_cast<size_t>(width), static_cast<size_t>(height), pixel_count) ||
                  data.size() != pixel_count) {
      return false;
    }

    auto pixel_data = ::rerun::Collection<uint8_t>::borrow(data.data(), data.size());
    rec.log(entity_path, ::rerun::archetypes::SegmentationImage(pixel_data, {width, height}));
    return true;
  }

  if (archetype == "SeriesLine" || archetype == "SeriesLines") {
    if (j.contains("value")) {
      rec.log(entity_path, ::rerun::archetypes::Scalars(j["value"].get<double>()));
      rec.log(entity_path, ::rerun::archetypes::SeriesLines());
      return true;
    }

    return false;
  }

  if (archetype == "SeriesPoint" || archetype == "SeriesPoints") {
    if (j.contains("value")) {
      rec.log(entity_path, ::rerun::archetypes::Scalars(j["value"].get<double>()));
      rec.log(entity_path, ::rerun::archetypes::SeriesPoints());
      return true;
    }

    return false;
  }

  if (archetype == "Clear") {
    bool recursive = j.value("recursive", true);
    rec.log(entity_path, ::rerun::archetypes::Clear(recursive));
    return true;
  }

  if (archetype == "TextDocument") {
    auto text = j.value("text", std::string(""));
    rec.log(entity_path, ::rerun::archetypes::TextDocument(text));
    return true;
  }

  if (archetype == "AnnotationContext") {
    if (!j.contains("annotations") || !j["annotations"].is_array()) {
      return false;
    }

    std::vector<::rerun::datatypes::AnnotationInfo> annotations;

    for (const auto& ann : j["annotations"]) {
      auto class_id = ann.value("class_id", static_cast<uint16_t>(0));
      auto label = ann.value("label", std::string(""));

      if (ann.contains("color") && ann["color"].is_array() && ann["color"].size() >= 3) {
        auto r = ann["color"][0].get<uint8_t>();
        auto g = ann["color"][1].get<uint8_t>();
        auto b = ann["color"][2].get<uint8_t>();
        auto a = static_cast<uint8_t>(ann["color"].size() >= 4 ? ann["color"][3].get<uint8_t>() : 255);

        if (label.empty()) {
          annotations.emplace_back(class_id, ::rerun::datatypes::Rgba32(r, g, b, a));
        } else {
          annotations.emplace_back(class_id, label, ::rerun::datatypes::Rgba32(r, g, b, a));
        }
      } else {
        if (label.empty()) {
          annotations.emplace_back(class_id);
        } else {
          annotations.emplace_back(class_id, label);
        }
      }
    }

    if (!annotations.empty()) {
      std::vector<::rerun::datatypes::ClassDescriptionMapElem> class_map;
      class_map.reserve(annotations.size());

      for (auto& ann : annotations) {
        class_map.emplace_back(::rerun::datatypes::ClassDescription(std::move(ann)));
      }

      rec.log_static(entity_path, ::rerun::archetypes::AnnotationContext(
                                      ::rerun::components::AnnotationContext(std::move(class_map))));
    }

    return true;
  }

  if (archetype == "Arrows2D") {
    if (!j.contains("vectors") || !j["vectors"].is_array()) {
      return false;
    }

    std::vector<::rerun::components::Vector2D> vectors;

    for (const auto& v : j["vectors"]) {
      if (v.is_array() && v.size() >= 2) {
        vectors.emplace_back(v[0].get<float>(), v[1].get<float>());
      }
    }

    auto arrows = ::rerun::archetypes::Arrows2D::from_vectors(std::move(vectors));

    if (j.contains("origins") && j["origins"].is_array()) {
      std::vector<::rerun::Position2D> origins;

      for (const auto& o : j["origins"]) {
        if (o.is_array() && o.size() >= 2) {
          origins.emplace_back(o[0].get<float>(), o[1].get<float>());
        }
      }

      arrows = std::move(arrows).with_origins(std::move(origins));
    }

    if (j.contains("colors") && j["colors"].is_array()) {
      std::vector<::rerun::Color> colors;

      for (const auto& c : j["colors"]) {
        if (c.is_array() && c.size() >= 3) {
          colors.emplace_back(c[0].get<uint8_t>(), c[1].get<uint8_t>(), c[2].get<uint8_t>(),
                              static_cast<uint8_t>(c.size() >= 4 ? c[3].get<uint8_t>() : 255));
        }
      }

      arrows = std::move(arrows).with_colors(std::move(colors));
    }

    rec.log(entity_path, arrows);
    return true;
  }

  if (archetype == "Mesh3D") {
    if (!j.contains("vertex_positions") || !j["vertex_positions"].is_array()) {
      return false;
    }

    std::vector<::rerun::Position3D> positions;

    for (const auto& p : j["vertex_positions"]) {
      if (p.is_array() && p.size() >= 3) {
        positions.emplace_back(p[0].get<float>(), p[1].get<float>(), p[2].get<float>());
      }
    }

    auto mesh = ::rerun::archetypes::Mesh3D(std::move(positions));

    if (j.contains("triangle_indices") && j["triangle_indices"].is_array()) {
      std::vector<::rerun::components::TriangleIndices> indices;

      for (const auto& tri : j["triangle_indices"]) {
        if (tri.is_array() && tri.size() >= 3) {
          indices.emplace_back(
              ::rerun::datatypes::UVec3D{tri[0].get<uint32_t>(), tri[1].get<uint32_t>(), tri[2].get<uint32_t>()});
        }
      }

      mesh = std::move(mesh).with_triangle_indices(std::move(indices));
    }

    if (j.contains("vertex_colors") && j["vertex_colors"].is_array()) {
      std::vector<::rerun::Color> colors;

      for (const auto& c : j["vertex_colors"]) {
        if (c.is_array() && c.size() >= 3) {
          colors.emplace_back(c[0].get<uint8_t>(), c[1].get<uint8_t>(), c[2].get<uint8_t>(),
                              static_cast<uint8_t>(c.size() >= 4 ? c[3].get<uint8_t>() : 255));
        }
      }

      mesh = std::move(mesh).with_vertex_colors(std::move(colors));
    }

    if (j.contains("vertex_normals") && j["vertex_normals"].is_array()) {
      std::vector<::rerun::components::Vector3D> normals;

      for (const auto& n : j["vertex_normals"]) {
        if (n.is_array() && n.size() >= 3) {
          normals.emplace_back(n[0].get<float>(), n[1].get<float>(), n[2].get<float>());
        }
      }

      mesh = std::move(mesh).with_vertex_normals(std::move(normals));
    }

    rec.log(entity_path, mesh);
    return true;
  }

  if (archetype == "Cylinders3D") {
    if (!j.contains("lengths") || !j["lengths"].is_array() || !j.contains("radii") || !j["radii"].is_array()) {
      return false;
    }

    std::vector<float> lengths;
    std::vector<float> radii;

    for (const auto& l : j["lengths"]) {
      lengths.emplace_back(l.get<float>());
    }

    for (const auto& r : j["radii"]) {
      radii.emplace_back(r.get<float>());
    }

    auto cylinders = ::rerun::archetypes::Cylinders3D::from_lengths_and_radii(std::move(lengths), std::move(radii));

    if (j.contains("centers") && j["centers"].is_array()) {
      std::vector<::rerun::datatypes::Vec3D> centers;

      for (const auto& c : j["centers"]) {
        if (c.is_array() && c.size() >= 3) {
          centers.emplace_back(c[0].get<float>(), c[1].get<float>(), c[2].get<float>());
        }
      }

      cylinders = std::move(cylinders).with_centers(std::move(centers));
    }

    if (j.contains("colors") && j["colors"].is_array()) {
      std::vector<::rerun::Color> colors;

      for (const auto& c : j["colors"]) {
        if (c.is_array() && c.size() >= 3) {
          colors.emplace_back(c[0].get<uint8_t>(), c[1].get<uint8_t>(), c[2].get<uint8_t>(),
                              static_cast<uint8_t>(c.size() >= 4 ? c[3].get<uint8_t>() : 255));
        }
      }

      cylinders = std::move(cylinders).with_colors(std::move(colors));
    }

    rec.log(entity_path, cylinders);
    return true;
  }

  if (archetype == "Ellipsoids3D") {
    if (!j.contains("half_sizes") || !j["half_sizes"].is_array()) {
      return false;
    }

    std::vector<::rerun::HalfSize3D> half_sizes;

    for (const auto& hs : j["half_sizes"]) {
      if (hs.is_array() && hs.size() >= 3) {
        half_sizes.emplace_back(hs[0].get<float>(), hs[1].get<float>(), hs[2].get<float>());
      }
    }

    auto ellipsoids = ::rerun::archetypes::Ellipsoids3D::from_half_sizes(std::move(half_sizes));

    if (j.contains("centers") && j["centers"].is_array()) {
      std::vector<::rerun::datatypes::Vec3D> centers;

      for (const auto& c : j["centers"]) {
        if (c.is_array() && c.size() >= 3) {
          centers.emplace_back(c[0].get<float>(), c[1].get<float>(), c[2].get<float>());
        }
      }

      ellipsoids = std::move(ellipsoids).with_centers(std::move(centers));
    }

    if (j.contains("colors") && j["colors"].is_array()) {
      std::vector<::rerun::Color> colors;

      for (const auto& c : j["colors"]) {
        if (c.is_array() && c.size() >= 3) {
          colors.emplace_back(c[0].get<uint8_t>(), c[1].get<uint8_t>(), c[2].get<uint8_t>(),
                              static_cast<uint8_t>(c.size() >= 4 ? c[3].get<uint8_t>() : 255));
        }
      }

      ellipsoids = std::move(ellipsoids).with_colors(std::move(colors));
    }

    rec.log(entity_path, ellipsoids);
    return true;
  }

  if (archetype == "GeoLineStrings") {
    if (!j.contains("line_strings") || !j["line_strings"].is_array()) {
      return false;
    }

    std::vector<::rerun::components::GeoLineString> line_strings;

    for (const auto& ls : j["line_strings"]) {
      if (!ls.is_array()) {
        continue;
      }

      std::vector<::rerun::datatypes::DVec2D> lat_lons;

      for (const auto& p : ls) {
        if (p.is_array() && p.size() >= 2) {
          lat_lons.emplace_back(p[0].get<double>(), p[1].get<double>());
        }
      }

      if (!lat_lons.empty()) {
        line_strings.emplace_back(::rerun::components::GeoLineString::from_lat_lon(std::move(lat_lons)));
      }
    }

    if (!line_strings.empty()) {
      auto geo = ::rerun::archetypes::GeoLineStrings(std::move(line_strings));

      if (j.contains("colors") && j["colors"].is_array()) {
        std::vector<::rerun::Color> colors;

        for (const auto& c : j["colors"]) {
          if (c.is_array() && c.size() >= 3) {
            colors.emplace_back(c[0].get<uint8_t>(), c[1].get<uint8_t>(), c[2].get<uint8_t>(),
                                static_cast<uint8_t>(c.size() >= 4 ? c[3].get<uint8_t>() : 255));
          }
        }

        geo = std::move(geo).with_colors(std::move(colors));
      }

      rec.log(entity_path, geo);
    }

    return true;
  }

  if (archetype == "BarChart") {
    if (!j.contains("values") || !j["values"].is_array()) {
      return false;
    }

    std::vector<double> values;

    for (const auto& v : j["values"]) {
      values.emplace_back(v.get<double>());
    }

    if (!values.empty()) {
      rec.log(entity_path, ::rerun::archetypes::BarChart::f64(std::move(values)));
    }

    return true;
  }

  if (archetype == "Capsules3D") {
    if (!j.contains("lengths") || !j["lengths"].is_array() || !j.contains("radii") || !j["radii"].is_array()) {
      return false;
    }

    std::vector<float> lengths;
    std::vector<float> radii;

    for (const auto& l : j["lengths"]) {
      lengths.emplace_back(l.get<float>());
    }

    for (const auto& r : j["radii"]) {
      radii.emplace_back(r.get<float>());
    }

    auto capsules = ::rerun::archetypes::Capsules3D::from_lengths_and_radii(std::move(lengths), std::move(radii));

    if (j.contains("translations") && j["translations"].is_array()) {
      std::vector<::rerun::components::Translation3D> translations;

      for (const auto& t : j["translations"]) {
        if (t.is_array() && t.size() >= 3) {
          translations.emplace_back(t[0].get<float>(), t[1].get<float>(), t[2].get<float>());
        }
      }

      capsules = std::move(capsules).with_translations(std::move(translations));
    }

    if (j.contains("quaternions") && j["quaternions"].is_array()) {
      std::vector<::rerun::components::RotationQuat> quats;

      for (const auto& q : j["quaternions"]) {
        if (q.is_array() && q.size() >= 4) {
          quats.emplace_back(::rerun::datatypes::Quaternion::from_xyzw(q[0].get<float>(), q[1].get<float>(),
                                                                       q[2].get<float>(), q[3].get<float>()));
        }
      }

      capsules = std::move(capsules).with_quaternions(std::move(quats));
    }

    if (j.contains("colors") && j["colors"].is_array()) {
      std::vector<::rerun::Color> colors;

      for (const auto& c : j["colors"]) {
        if (c.is_array() && c.size() >= 3) {
          colors.emplace_back(c[0].get<uint8_t>(), c[1].get<uint8_t>(), c[2].get<uint8_t>(),
                              static_cast<uint8_t>(c.size() >= 4 ? c[3].get<uint8_t>() : 255));
        }
      }

      capsules = std::move(capsules).with_colors(std::move(colors));
    }

    rec.log(entity_path, capsules);
    return true;
  }

  if (archetype == "EncodedDepthImage") {
    auto data = decode_plugin_binary(j);

    if (data.empty()) {
      return false;
    }

    auto blob = ::rerun::Collection<uint8_t>::borrow(data.data(), data.size());
    auto depth_image = ::rerun::archetypes::EncodedDepthImage(::rerun::components::Blob(std::move(blob)));
    auto media_type = infer_media_type(j);

    if (!media_type.empty()) {
      depth_image = std::move(depth_image).with_media_type(::rerun::components::MediaType(media_type));
    }

    auto meter = static_cast<float>(j.value("meter", 0.0));

    if (meter > 0.0F) {
      depth_image = std::move(depth_image).with_meter(::rerun::components::DepthMeter(meter));
    }

    rec.log(entity_path, depth_image);
    return true;
  }

  if (archetype == "Asset3D") {
    auto data = decode_plugin_binary(j);

    if (data.empty()) {
      return false;
    }

    auto media_type = infer_media_type(j);
    auto blob = ::rerun::Collection<uint8_t>::borrow(data.data(), data.size());
    auto asset = ::rerun::archetypes::Asset3D::from_file_contents(
        std::move(blob), media_type.empty() ? std::optional<::rerun::components::MediaType>(std::nullopt)
                                            : std::optional<::rerun::components::MediaType>(media_type));

    rec.log(entity_path, asset);
    return true;
  }

  if (archetype == "GraphNodes") {
    if (!j.contains("node_ids") || !j["node_ids"].is_array()) {
      return false;
    }

    std::vector<::rerun::components::GraphNode> node_ids;
    node_ids.reserve(j["node_ids"].size());

    for (const auto& id : j["node_ids"]) {
      node_ids.emplace_back(id.get<std::string>());
    }

    auto nodes = ::rerun::archetypes::GraphNodes(std::move(node_ids));

    if (j.contains("positions") && j["positions"].is_array()) {
      std::vector<::rerun::components::Position2D> positions;
      positions.reserve(j["positions"].size());

      for (const auto& p : j["positions"]) {
        if (p.is_array() && p.size() >= 2) {
          positions.emplace_back(p[0].get<float>(), p[1].get<float>());
        }
      }

      nodes = std::move(nodes).with_positions(std::move(positions));
    }

    if (j.contains("labels") && j["labels"].is_array()) {
      std::vector<::rerun::components::Text> labels;
      labels.reserve(j["labels"].size());

      for (const auto& l : j["labels"]) {
        labels.emplace_back(l.get<std::string>());
      }

      nodes = std::move(nodes).with_labels(std::move(labels));
    }

    if (j.contains("colors") && j["colors"].is_array()) {
      std::vector<::rerun::Color> colors;
      colors.reserve(j["colors"].size());

      for (const auto& c : j["colors"]) {
        if (c.is_array() && c.size() >= 3) {
          colors.emplace_back(c[0].get<uint8_t>(), c[1].get<uint8_t>(), c[2].get<uint8_t>(),
                              static_cast<uint8_t>(c.size() >= 4 ? c[3].get<uint8_t>() : 255));
        }
      }

      nodes = std::move(nodes).with_colors(std::move(colors));
    }

    rec.log(entity_path, nodes);
    return true;
  }

  if (archetype == "GraphEdges") {
    if (!j.contains("edges") || !j["edges"].is_array()) {
      return false;
    }

    std::vector<::rerun::components::GraphEdge> edge_list;

    for (const auto& e : j["edges"]) {
      if (e.is_array() && e.size() >= 2) {
        edge_list.emplace_back(e[0].get<std::string>(), e[1].get<std::string>());
      }
    }

    if (edge_list.empty()) {
      return false;
    }

    auto edges = ::rerun::archetypes::GraphEdges(std::move(edge_list));

    auto graph_type = j.value("graph_type", std::string(""));

    if (graph_type == "directed" || graph_type == "Directed") {
      edges = std::move(edges).with_graph_type(::rerun::components::GraphType::Directed);
    }

    rec.log(entity_path, edges);
    return true;
  }

  if (archetype == "ViewCoordinates") {
    auto system = j.value("system", std::string("RIGHT_HAND_Z_UP"));
    log_view_coordinates_value(rec, entity_path, system);
    return true;
  }

  if (archetype == "InstancePoses3D") {
    auto poses = ::rerun::archetypes::InstancePoses3D();

    if (j.contains("translations") && j["translations"].is_array()) {
      std::vector<::rerun::components::Translation3D> translations;

      for (const auto& t : j["translations"]) {
        if (t.is_array() && t.size() >= 3) {
          translations.emplace_back(t[0].get<float>(), t[1].get<float>(), t[2].get<float>());
        }
      }

      poses = std::move(poses).with_translations(std::move(translations));
    }

    if (j.contains("quaternions") && j["quaternions"].is_array()) {
      std::vector<::rerun::components::RotationQuat> quats;

      for (const auto& q : j["quaternions"]) {
        if (q.is_array() && q.size() >= 4) {
          quats.emplace_back(::rerun::datatypes::Quaternion::from_xyzw(q[0].get<float>(), q[1].get<float>(),
                                                                       q[2].get<float>(), q[3].get<float>()));
        }
      }

      poses = std::move(poses).with_quaternions(std::move(quats));
    }

    if (j.contains("scales") && j["scales"].is_array()) {
      std::vector<::rerun::components::Scale3D> scales;

      for (const auto& s : j["scales"]) {
        if (s.is_array() && s.size() >= 3) {
          scales.emplace_back(s[0].get<float>(), s[1].get<float>(), s[2].get<float>());
        }
      }

      poses = std::move(poses).with_scales(std::move(scales));
    }

    rec.log(entity_path, poses);
    return true;
  }

  if (archetype == "AssetVideo") {
    auto data = decode_plugin_binary(j);

    if (data.empty()) {
      return false;
    }

    auto media_type = infer_media_type(j);
    auto blob = ::rerun::Collection<uint8_t>::borrow(data.data(), data.size());
    auto video = ::rerun::archetypes::AssetVideo::from_bytes(
        std::move(blob), media_type.empty() ? std::optional<::rerun::components::MediaType>(std::nullopt)
                                            : std::optional<::rerun::components::MediaType>(media_type));

    rec.log(entity_path, video);
    return true;
  }

  if (archetype == "VideoFrameReference") {
    auto ts_ns = j.value("timestamp_ns", static_cast<int64_t>(0));
    auto frame_ref =
        ::rerun::archetypes::VideoFrameReference(::rerun::components::VideoTimestamp(std::chrono::nanoseconds(ts_ns)));

    if (j.contains("video_reference") && j["video_reference"].is_string()) {
      frame_ref = std::move(frame_ref).with_video_reference(j["video_reference"].get<std::string>());
    }

    rec.log(entity_path, frame_ref);
    return true;
  }

  if (archetype == "Tensor") {
    if (!j.contains("shape") || !j["shape"].is_array()) {
      return false;
    }

    std::vector<uint64_t> shape;
    shape.reserve(j["shape"].size());

    for (const auto& dim : j["shape"]) {
      if (!dim.is_number_integer() && !dim.is_number_unsigned()) {
        return false;
      }

      shape.emplace_back(dim.get<uint64_t>());
    }

    auto data = decode_plugin_binary(j);

    if (shape.empty() || data.empty()) {
      return false;
    }

    auto data_collection = ::rerun::Collection<uint8_t>::borrow(data.data(), data.size());
    auto tensor =
        ::rerun::archetypes::Tensor(std::move(shape), ::rerun::datatypes::TensorBuffer(std::move(data_collection)));

    if (j.contains("dim_names") && j["dim_names"].is_array()) {
      std::vector<std::string> dim_names;
      dim_names.reserve(j["dim_names"].size());

      for (const auto& name : j["dim_names"]) {
        if (!name.is_string()) {
          return false;
        }

        dim_names.emplace_back(name.get<std::string>());
      }

      tensor = std::move(tensor).with_dim_names(std::move(dim_names));
    }

    rec.log(entity_path, tensor);
    return true;
  }

  MLOG_W("Unknown Rerun archetype from plugin: {}", archetype);
  return false;
}

#ifdef VLINK_HAS_FBS_COMPILER  // NOLINT(readability-redundant-preprocessor)

// NOLINTNEXTLINE(google-readability-function-size)
bool RerunConverter::log_fbs_with_mapping(::rerun::RecordingStream& rec, const std::string& entity_path,
                                          const RerunMap& mapping, const std::string& ser, const Bytes& raw) {
  const reflection::Schema* schema = nullptr;

  if VUNLIKELY (!resolve_thread_local_fbs_schema(
                    ser, cache_owner_id_,
                    [this](const std::string& type_name, std::string& schema_data) {
                      return resolve_fbs_schema(type_name, schema_data);
                    },
                    schema)) {
    return false;
  }

  if VUNLIKELY (!schema || !schema->root_table()) {
    return false;
  }

  if VUNLIKELY (!verify_fbs_payload(*schema, raw, ser, "Rerun mapping")) {
    return false;
  }

  const auto* root_table = flatbuffers::GetAnyRoot(raw.data());

  if VUNLIKELY (!root_table) {
    return false;
  }

  const auto& obj = *schema->root_table();

  auto fbs_get_double = [schema](const flatbuffers::Table& tbl, const reflection::Object& o, const std::string& src,
                                 const std::string& expr = {}) -> double {
    if (!expr.empty()) {
      return evaluate_expression_with_fbs(expr, tbl, o, *schema);
    }

    if (has_nested_field_path(src)) {
      return safe_nested_fbs_double(tbl, o, *schema, src);
    }

    return get_fbs_double(tbl, o, src);
  };

  auto fbs_get_string = [schema](const flatbuffers::Table& tbl, const reflection::Object& o,
                                 const std::string& src) -> std::string {
    if (has_nested_field_path(src)) {
      bool found = false;
      auto val = resolve_nested_fbs_string(tbl, o, *schema, src, &found);

      if VLIKELY (found) {
        return val;
      }

      return {};
    }

    return get_fbs_string(tbl, o, src);
  };

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

  const auto& archetype = mapping.archetype;

  if (archetype == "GeoPoints") {
    double latitude = 0.0;
    double longitude = 0.0;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "latitude") {
        latitude = fbs_get_mapped_double(*root_table, obj, fm);
      } else if (fm.target == "longitude") {
        longitude = fbs_get_mapped_double(*root_table, obj, fm);
      }
    }

    rec.log(entity_path, ::rerun::archetypes::GeoPoints({{latitude, longitude}}));
    return true;
  }

  if (archetype == "Transform3D") {
    double px = 0.0;
    double py = 0.0;
    double pz = 0.0;
    double qx = 0.0;
    double qy = 0.0;
    double qz = 0.0;
    double qw = 1.0;
    bool has_euler = false;
    double euler_roll = 0.0;
    double euler_pitch = 0.0;
    double euler_yaw = 0.0;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "pose" || fm.target == "pose_euler") {
        FbsObjectView target_parent;
        std::string target_field_name;

        if (resolve_fbs_parent_field_path(*root_table, obj, *schema, fm.source, target_parent, target_field_name)) {
          const auto* target_field = find_fbs_field(*target_parent.object, target_field_name);

          if (target_field && target_field->type()->base_type() == reflection::Obj) {
            const auto* sub_obj = find_fbs_object(*schema, target_field->type()->index());
            FbsObjectView child;

            if (sub_obj && resolve_fbs_object_field(target_parent, *target_field, *sub_obj, child)) {
              if (fm.target == "pose") {
                qx = get_fbs_double(child, "x");
                qy = get_fbs_double(child, "y");
                qz = get_fbs_double(child, "z");
                qw = get_fbs_double(child, "w");
              } else {
                euler_roll = get_fbs_double(child, "x");
                euler_pitch = get_fbs_double(child, "y");
                euler_yaw = get_fbs_double(child, "z");
                has_euler = true;
              }
            }
          }
        }
      } else if (fm.target == "position_x") {
        px = fbs_get_mapped_double(*root_table, obj, fm);
      } else if (fm.target == "position_y") {
        py = fbs_get_mapped_double(*root_table, obj, fm);
      } else if (fm.target == "position_z") {
        pz = fbs_get_mapped_double(*root_table, obj, fm);
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

    auto translation =
        ::rerun::components::Translation3D(static_cast<float>(px), static_cast<float>(py), static_cast<float>(pz));

    auto quaternion = ::rerun::Quaternion::from_xyzw(static_cast<float>(qx), static_cast<float>(qy),
                                                     static_cast<float>(qz), static_cast<float>(qw));

    rec.log(entity_path, ::rerun::archetypes::Transform3D().with_translation(translation).with_quaternion(quaternion));
    return true;
  }

  if (archetype == "Boxes3D") {
    std::string entity_sub_items;
    const FieldMapping* entity_x_mapping = nullptr;
    const FieldMapping* entity_y_mapping = nullptr;
    const FieldMapping* entity_z_mapping = nullptr;
    const FieldMapping* entity_w_mapping = nullptr;
    const FieldMapping* entity_l_mapping = nullptr;
    const FieldMapping* entity_h_mapping = nullptr;
    const FieldMapping* entity_heading_mapping = nullptr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "entity_sub_items") {
        entity_sub_items = fm.source;
      } else if (fm.target == "entity_x") {
        entity_x_mapping = &fm;
      } else if (fm.target == "entity_y") {
        entity_y_mapping = &fm;
      } else if (fm.target == "entity_z") {
        entity_z_mapping = &fm;
      } else if (fm.target == "entity_width") {
        entity_w_mapping = &fm;
      } else if (fm.target == "entity_length") {
        entity_l_mapping = &fm;
      } else if (fm.target == "entity_height") {
        entity_h_mapping = &fm;
      } else if (fm.target == "entity_heading") {
        entity_heading_mapping = &fm;
      }
    }

    bool has_entity_fields = entity_x_mapping != nullptr || entity_y_mapping != nullptr ||
                             entity_z_mapping != nullptr || entity_w_mapping != nullptr ||
                             entity_l_mapping != nullptr || entity_h_mapping != nullptr ||
                             entity_heading_mapping != nullptr;

    std::vector<::rerun::Position3D> centers;
    std::vector<::rerun::HalfSize3D> half_sizes;
    std::vector<::rerun::components::RotationQuat> rotations;
    std::vector<::rerun::Color> box_colors;

    auto build_fbs_box = [&box_colors, &centers, &entity_h_mapping, &entity_heading_mapping, &entity_l_mapping,
                          &entity_w_mapping, &entity_x_mapping, &entity_y_mapping, &entity_z_mapping,
                          &fbs_get_object_mapped_double, &half_sizes, &has_entity_fields, &rotations,
                          &schema](const FbsObjectView& item) {
      double bpx = 0.0;
      double bpy = 0.0;
      double bpz = 0.0;
      double bsx = 1.0;
      double bsy = 1.0;
      double bsz = 1.0;
      double heading = 0.0;

      if (has_entity_fields) {
        if (entity_x_mapping) {
          bpx = fbs_get_object_mapped_double(item, *entity_x_mapping);
        }

        if (entity_y_mapping) {
          bpy = fbs_get_object_mapped_double(item, *entity_y_mapping);
        }

        if (entity_z_mapping) {
          bpz = fbs_get_object_mapped_double(item, *entity_z_mapping);
        }

        if (entity_w_mapping) {
          auto v = fbs_get_object_mapped_double(item, *entity_w_mapping);

          if (v != 0.0) {
            bsx = v;
          }
        }

        if (entity_l_mapping) {
          auto v = fbs_get_object_mapped_double(item, *entity_l_mapping);

          if (v != 0.0) {
            bsy = v;
          }
        }

        if (entity_h_mapping) {
          auto v = fbs_get_object_mapped_double(item, *entity_h_mapping);

          if (v != 0.0) {
            bsz = v;
          }
        }

        if (entity_heading_mapping) {
          heading = fbs_get_object_mapped_double(item, *entity_heading_mapping);
        }
      } else {
        bpx = get_fbs_double(item, "x");
        bpy = get_fbs_double(item, "y");
        bpz = get_fbs_double(item, "z");

        if (bpx == 0.0 && bpy == 0.0 && bpz == 0.0) {
          bpx = get_fbs_double(item, "cx");
          bpy = get_fbs_double(item, "cy");
          bpz = get_fbs_double(item, "cz");
        }

        if (bpx == 0.0 && bpy == 0.0 && bpz == 0.0) {
          const auto* pos_field = find_fbs_field(*item.object, "position");

          if (pos_field && pos_field->type()->base_type() == reflection::Obj) {
            const auto* pos_obj = find_fbs_object(*schema, pos_field->type()->index());
            FbsObjectView position;

            if (pos_obj && resolve_fbs_object_field(item, *pos_field, *pos_obj, position)) {
              bpx = get_fbs_double(position, "x");
              bpy = get_fbs_double(position, "y");
              bpz = get_fbs_double(position, "z");
            }
          }
        }

        auto w_val = get_fbs_double(item, "width");
        auto l_val = get_fbs_double(item, "length");
        auto h_val = get_fbs_double(item, "height");

        if (w_val != 0.0) {
          bsx = w_val;
        }

        if (l_val != 0.0) {
          bsy = l_val;
        }

        if (h_val != 0.0) {
          bsz = h_val;
        }

        heading = get_fbs_double(item, "heading_angle");

        if (heading == 0.0) {
          heading = get_fbs_double(item, "yaw");
        }
      }

      centers.emplace_back(static_cast<float>(bpx), static_cast<float>(bpy), static_cast<float>(bpz));
      half_sizes.emplace_back(static_cast<float>(bsx * 0.5), static_cast<float>(bsy * 0.5),
                              static_cast<float>(bsz * 0.5));

      if (heading != 0.0) {
        auto qz_val = static_cast<float>(std::sin(heading * 0.5));
        auto qw_val = static_cast<float>(std::cos(heading * 0.5));
        rotations.emplace_back(::rerun::datatypes::Quaternion::from_xyzw(0.0F, 0.0F, qz_val, qw_val));
      } else {
        rotations.emplace_back(::rerun::datatypes::Quaternion::from_xyzw(0.0F, 0.0F, 0.0F, 1.0F));
      }

      box_colors.emplace_back(51, 204, 51, 204);
    };

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target != "entities") {
        continue;
      }

      FbsObjectView entities_parent;
      std::string entities_field_name;

      if (!resolve_fbs_parent_field_path(*root_table, obj, *schema, fm.source, entities_parent, entities_field_name)) {
        continue;
      }

      const auto* vec_field = find_fbs_field(*entities_parent.object, entities_field_name);

      if (!vec_field || vec_field->type()->base_type() != reflection::Vector ||
          vec_field->type()->element() != reflection::Obj) {
        continue;
      }

      const auto* vec = get_fbs_vector(entities_parent, *vec_field);

      if (!vec) {
        continue;
      }

      const auto* sub_obj = find_fbs_object(*schema, vec_field->type()->index());

      if (!sub_obj) {
        continue;
      }

      for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
        FbsObjectView item;

        if (!resolve_fbs_vector_object_unchecked(*vec, i, *sub_obj, item)) {
          continue;
        }

        if (!entity_sub_items.empty()) {
          const auto* sub_field = find_fbs_field(*sub_obj, entity_sub_items);

          if (sub_field && sub_field->type()->base_type() == reflection::Vector &&
              sub_field->type()->element() == reflection::Obj) {
            const auto* sub_vec = get_fbs_vector(item, *sub_field);

            if (sub_vec) {
              const auto* sub_sub_obj = find_fbs_object(*schema, sub_field->type()->index());

              if (sub_sub_obj) {
                for (flatbuffers::uoffset_t j = 0; j < sub_vec->size(); ++j) {
                  FbsObjectView sub_item;

                  if (resolve_fbs_vector_object_unchecked(*sub_vec, j, *sub_sub_obj, sub_item)) {
                    build_fbs_box(sub_item);
                  }
                }
              }
            }
          }
        } else {
          build_fbs_box(item);
        }
      }
    }

    if (!centers.empty()) {
      auto boxes = ::rerun::archetypes::Boxes3D::from_centers_and_half_sizes(centers, half_sizes);

      if (!rotations.empty()) {
        boxes = std::move(boxes).with_quaternions(std::move(rotations));
      }

      if (!box_colors.empty()) {
        boxes = std::move(boxes).with_colors(std::move(box_colors));
      }

      rec.log(entity_path, boxes);
    }

    return true;
  }

  if (archetype == "TextLog") {
    std::string message;
    const FieldMapping* level_mapping = nullptr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "message") {
        message = fbs_get_mapped_string(*root_table, obj, fm);
      } else if (fm.target == "level" || fm.target == "severity") {
        level_mapping = &fm;
      }
    }

    if (message.empty()) {
      message = fbs_get_string(*root_table, obj, "message");

      if (message.empty()) {
        message = fbs_get_string(*root_table, obj, "msg");
      }
    }

    if (!message.empty()) {
      auto level_str = level_mapping ? fbs_get_mapped_string(*root_table, obj, *level_mapping)
                                     : fbs_get_string(*root_table, obj, "level");
      auto text_log = ::rerun::TextLog(message);

      if (!level_str.empty()) {
        text_log = std::move(text_log).with_level(::rerun::components::TextLogLevel(level_str));
      }

      rec.log(entity_path, text_log);
      return true;
    }
  }

  if (archetype == "EncodedImage") {
    std::string data_field_name = "data";
    const FieldMapping* format_mapping = nullptr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "data") {
        data_field_name = fm.source;
      } else if (fm.target == "format") {
        format_mapping = &fm;
      }
    }

    const auto* data_fld = find_fbs_field(obj, data_field_name);

    if (data_fld && is_fbs_byte_vector(*data_fld)) {
      const auto* vec = flatbuffers::GetFieldV<uint8_t>(*root_table, *data_fld);

      // NOLINTNEXTLINE(readability-container-size-empty)
      if VLIKELY (vec != nullptr && vec->size() != 0) {
        auto blob = ::rerun::Collection<uint8_t>::borrow(vec->data(), vec->size());

        auto format_str = format_mapping ? fbs_get_mapped_string(*root_table, obj, *format_mapping)
                                         : fbs_get_string(*root_table, obj, "format");
        std::string media_type;

        if (format_str == "jpeg" || format_str == "jpg") {
          media_type = "image/jpeg";
        } else if (format_str == "png") {
          media_type = "image/png";
        }

        if (media_type.empty()) {
          rec.log(entity_path, ::rerun::archetypes::EncodedImage::from_bytes(blob));
        } else {
          rec.log(entity_path,
                  ::rerun::archetypes::EncodedImage::from_bytes(blob, ::rerun::components::MediaType(media_type)));
        }

        return true;
      }
    }
  }

  if (archetype == "Image") {
    uint32_t width = 0;
    uint32_t height = 0;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "width") {
        width = checked_unsigned_cast<uint32_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "height") {
        height = checked_unsigned_cast<uint32_t>(fbs_get_mapped_double(*root_table, obj, fm));
      }
    }

    if (width == 0 || height == 0) {
      return false;
    }

    std::string data_field_name = "data";

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "data") {
        data_field_name = fm.source;
        break;
      }
    }

    const auto* data_fld = find_fbs_field(obj, data_field_name);

    if (!data_fld || !is_fbs_byte_vector(*data_fld)) {
      return false;
    }

    const auto* vec = flatbuffers::GetFieldV<uint8_t>(*root_table, *data_fld);

    // NOLINTNEXTLINE(readability-container-size-empty)
    if VUNLIKELY (vec == nullptr || vec->size() == 0) {
      return false;
    }

    size_t pixel_total = 0;

    if VUNLIKELY (!mul_size(static_cast<size_t>(width), static_cast<size_t>(height), pixel_total)) {
      return false;
    }

    if (vec->size() == pixel_total) {
      return log_typed_image(rec, entity_path, vec->data(), vec->size(), width, height,
                             ::rerun::datatypes::ColorModel::L, ::rerun::datatypes::ChannelDatatype::U8);
    }

    size_t expected_size = 0;

    if (mul_size(pixel_total, 3U, expected_size) && vec->size() == expected_size) {
      return log_typed_image(rec, entity_path, vec->data(), vec->size(), width, height,
                             ::rerun::datatypes::ColorModel::RGB, ::rerun::datatypes::ChannelDatatype::U8);
    }

    if (mul_size(pixel_total, 4U, expected_size) && vec->size() == expected_size) {
      return log_typed_image(rec, entity_path, vec->data(), vec->size(), width, height,
                             ::rerun::datatypes::ColorModel::RGBA, ::rerun::datatypes::ChannelDatatype::U8);
    }

    return false;
  }

  if (archetype == "Pinhole") {
    double fx = 0.0;
    double fy = 0.0;
    double cx = 0.0;
    double cy = 0.0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool has_cx = false;
    bool has_cy = false;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "fx") {
        fx = fbs_get_mapped_double(*root_table, obj, fm);
      } else if (fm.target == "fy") {
        fy = fbs_get_mapped_double(*root_table, obj, fm);
      } else if (fm.target == "cx") {
        cx = fbs_get_mapped_double(*root_table, obj, fm);
        has_cx = true;
      } else if (fm.target == "cy") {
        cy = fbs_get_mapped_double(*root_table, obj, fm);
        has_cy = true;
      } else if (fm.target == "width" || fm.target == "image_width") {
        width = checked_unsigned_cast<uint32_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "height" || fm.target == "image_height") {
        height = checked_unsigned_cast<uint32_t>(fbs_get_mapped_double(*root_table, obj, fm));
      }
    }

    const auto resolution_width = static_cast<float>(width > 0 ? width : cx * 2.0);
    const auto resolution_height = static_cast<float>(height > 0 ? height : cy * 2.0);
    const auto principal_x = has_cx ? static_cast<float>(cx) : resolution_width * 0.5F;
    const auto principal_y = has_cy ? static_cast<float>(cy) : resolution_height * 0.5F;

    if VUNLIKELY (!std::isfinite(fx) || !std::isfinite(fy) || !std::isfinite(principal_x) ||
                  !std::isfinite(principal_y) || !std::isfinite(resolution_width) ||
                  !std::isfinite(resolution_height) || fx <= 0.0 || fy <= 0.0 || resolution_width <= 0.0F ||
                  resolution_height <= 0.0F) {
      return false;
    }

    auto pinhole = make_pinhole(static_cast<float>(fx), static_cast<float>(fy), principal_x, principal_y,
                                resolution_width, resolution_height);

    rec.log_static(entity_path, pinhole);
    return true;
  }

  if (archetype == "DepthImage") {
    uint32_t width = 0;
    uint32_t height = 0;
    std::string data_field_name = "data";
    std::string datatype;
    float meter = 0.0F;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "column_count" || fm.target == "width") {
        width = checked_unsigned_cast<uint32_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "row_count" || fm.target == "height") {
        height = checked_unsigned_cast<uint32_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "data") {
        data_field_name = fm.source;
      } else if (fm.target == "datatype") {
        datatype = fbs_get_mapped_string(*root_table, obj, fm);
      } else if (fm.target == "meter") {
        meter = static_cast<float>(fbs_get_mapped_double(*root_table, obj, fm));
      }
    }

    if (width == 0 || height == 0) {
      return false;
    }

    const auto* data_fld = find_fbs_field(obj, data_field_name);

    if (!data_fld || !is_fbs_byte_vector(*data_fld)) {
      return false;
    }

    const auto* vec = flatbuffers::GetFieldV<uint8_t>(*root_table, *data_fld);

    // NOLINTNEXTLINE(readability-container-size-empty)
    if VUNLIKELY (vec == nullptr || vec->size() == 0) {
      return false;
    }

    return log_depth_image_bytes(rec, entity_path, vec->data(), vec->size(), width, height, datatype, meter);
  }

  if (archetype == "SegmentationImage") {
    uint32_t width = 0;
    uint32_t height = 0;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "width") {
        width = checked_unsigned_cast<uint32_t>(fbs_get_mapped_double(*root_table, obj, fm));
      } else if (fm.target == "height") {
        height = checked_unsigned_cast<uint32_t>(fbs_get_mapped_double(*root_table, obj, fm));
      }
    }

    if (width == 0 || height == 0) {
      return false;
    }

    std::string data_field_name = "data";

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "data") {
        data_field_name = fm.source;
        break;
      }
    }

    const auto* data_fld = find_fbs_field(obj, data_field_name);

    if (!data_fld || !is_fbs_byte_vector(*data_fld)) {
      return false;
    }

    const auto* vec = flatbuffers::GetFieldV<uint8_t>(*root_table, *data_fld);

    // NOLINTNEXTLINE(readability-container-size-empty)
    if VUNLIKELY (vec == nullptr || vec->size() == 0) {
      return false;
    }

    size_t pixel_total = 0;

    if VUNLIKELY (!mul_size(static_cast<size_t>(width), static_cast<size_t>(height), pixel_total) ||
                  vec->size() != pixel_total) {
      return false;
    }

    auto pixel_data = ::rerun::Collection<uint8_t>::borrow(vec->data(), vec->size());
    rec.log(entity_path, ::rerun::archetypes::SegmentationImage(pixel_data, {width, height}));
    return true;
  }

  if (archetype == "Scalars") {
    double value = 0.0;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "value") {
        value = fbs_get_mapped_double(*root_table, obj, fm);
        break;
      }
    }

    rec.log(entity_path, ::rerun::archetypes::Scalars(value));
    return true;
  }

  if (archetype == "SeriesLine" || archetype == "SeriesLines") {
    double value = 0.0;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "value") {
        value = fbs_get_mapped_double(*root_table, obj, fm);
        break;
      }
    }

    rec.log(entity_path, ::rerun::archetypes::Scalars(value));
    rec.log(entity_path, ::rerun::archetypes::SeriesLines());
    return true;
  }

  if (archetype == "SeriesPoint" || archetype == "SeriesPoints") {
    double value = 0.0;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "value") {
        value = fbs_get_mapped_double(*root_table, obj, fm);
        break;
      }
    }

    rec.log(entity_path, ::rerun::archetypes::Scalars(value));
    rec.log(entity_path, ::rerun::archetypes::SeriesPoints());
    return true;
  }

  if (archetype == "LineStrips3D") {
    std::string entities_src;
    const FieldMapping* x_mapping = nullptr;
    const FieldMapping* y_mapping = nullptr;
    const FieldMapping* z_mapping = nullptr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "entities" || fm.target == "strips" || fm.target == "points") {
        entities_src = fm.source;
      } else if (fm.target == "point_x") {
        x_mapping = &fm;
      } else if (fm.target == "point_y") {
        y_mapping = &fm;
      } else if (fm.target == "point_z") {
        z_mapping = &fm;
      }
    }

    std::string field_name = entities_src.empty() ? "points" : entities_src;
    const auto* vec_field = find_fbs_field(obj, field_name);

    if (vec_field && vec_field->type()->base_type() == reflection::Vector &&
        vec_field->type()->element() == reflection::Obj) {
      FbsObjectView root{&obj, root_table, nullptr};
      const auto* vec = get_fbs_vector(root, *vec_field);

      if (vec) {
        std::vector<::rerun::Position3D> strip;
        strip.reserve(vec->size());
        for_each_fbs_vector_object(root, *vec_field, *schema, [&](const FbsObjectView& item, flatbuffers::uoffset_t) {
          auto px = static_cast<float>(x_mapping ? fbs_get_object_mapped_double(item, *x_mapping)
                                                 : get_fbs_double(item, "x"));
          auto py = static_cast<float>(y_mapping ? fbs_get_object_mapped_double(item, *y_mapping)
                                                 : get_fbs_double(item, "y"));
          auto pz = static_cast<float>(z_mapping ? fbs_get_object_mapped_double(item, *z_mapping)
                                                 : get_fbs_double(item, "z"));
          strip.emplace_back(px, py, pz);
        });

        if (!strip.empty()) {
          rec.log(entity_path, ::rerun::archetypes::LineStrips3D({std::move(strip)}));
        }

        return true;
      }
    }

    return false;
  }

  if (archetype == "LineStrips2D") {
    std::string entities_src;
    const FieldMapping* x_mapping = nullptr;
    const FieldMapping* y_mapping = nullptr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "entities" || fm.target == "strips" || fm.target == "points") {
        entities_src = fm.source;
      } else if (fm.target == "point_x") {
        x_mapping = &fm;
      } else if (fm.target == "point_y") {
        y_mapping = &fm;
      }
    }

    std::string field_name = entities_src.empty() ? "points" : entities_src;
    const auto* vec_field = find_fbs_field(obj, field_name);

    if (vec_field && vec_field->type()->base_type() == reflection::Vector &&
        vec_field->type()->element() == reflection::Obj) {
      FbsObjectView root{&obj, root_table, nullptr};
      const auto* vec = get_fbs_vector(root, *vec_field);

      if (vec) {
        std::vector<::rerun::Position2D> strip;
        strip.reserve(vec->size());
        for_each_fbs_vector_object(root, *vec_field, *schema, [&](const FbsObjectView& item, flatbuffers::uoffset_t) {
          auto px = static_cast<float>(x_mapping ? fbs_get_object_mapped_double(item, *x_mapping)
                                                 : get_fbs_double(item, "x"));
          auto py = static_cast<float>(y_mapping ? fbs_get_object_mapped_double(item, *y_mapping)
                                                 : get_fbs_double(item, "y"));
          strip.emplace_back(px, py);
        });

        if (!strip.empty()) {
          rec.log(entity_path, ::rerun::archetypes::LineStrips2D({std::move(strip)}));
        }

        return true;
      }
    }

    return false;
  }

  if (archetype == "Boxes2D") {
    std::string entities_src;
    const FieldMapping* x_mapping = nullptr;
    const FieldMapping* y_mapping = nullptr;
    const FieldMapping* w_mapping = nullptr;
    const FieldMapping* h_mapping = nullptr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "entities") {
        entities_src = fm.source;
      } else if (fm.target == "center_x") {
        x_mapping = &fm;
      } else if (fm.target == "center_y") {
        y_mapping = &fm;
      } else if (fm.target == "width" || fm.target == "size_x") {
        w_mapping = &fm;
      } else if (fm.target == "height" || fm.target == "size_y") {
        h_mapping = &fm;
      }
    }

    std::string field_name = entities_src.empty() ? "boxes" : entities_src;
    const auto* vec_field = find_fbs_field(obj, field_name);

    if (!vec_field || vec_field->type()->base_type() != reflection::Vector) {
      return false;
    }

    if (vec_field->type()->element() != reflection::Obj) {
      return false;
    }

    FbsObjectView root{&obj, root_table, nullptr};
    const auto* vec = get_fbs_vector(root, *vec_field);

    if (!vec) {
      return false;
    }

    std::vector<::rerun::datatypes::Vec2D> centers;
    std::vector<::rerun::datatypes::Vec2D> half_sizes;
    centers.reserve(vec->size());
    half_sizes.reserve(vec->size());

    for_each_fbs_vector_object(root, *vec_field, *schema, [&](const FbsObjectView& item, flatbuffers::uoffset_t) {
      double cx = x_mapping ? fbs_get_object_mapped_double(item, *x_mapping) : get_fbs_double(item, "center_x");
      double cy = y_mapping ? fbs_get_object_mapped_double(item, *y_mapping) : get_fbs_double(item, "center_y");
      double w = w_mapping ? fbs_get_object_mapped_double(item, *w_mapping) : get_fbs_double(item, "width");
      double h = h_mapping ? fbs_get_object_mapped_double(item, *h_mapping) : get_fbs_double(item, "height");

      centers.emplace_back(static_cast<float>(cx), static_cast<float>(cy));
      half_sizes.emplace_back(static_cast<float>(w * 0.5), static_cast<float>(h * 0.5));
    });

    if (!centers.empty()) {
      rec.log(entity_path, ::rerun::archetypes::Boxes2D::from_centers_and_half_sizes(centers, half_sizes));
    }

    return true;
  }

  if (archetype == "Arrows2D") {
    std::string entities_src;
    const FieldMapping* origin_x_mapping = nullptr;
    const FieldMapping* origin_y_mapping = nullptr;
    const FieldMapping* vec_x_mapping = nullptr;
    const FieldMapping* vec_y_mapping = nullptr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "entities") {
        entities_src = fm.source;
      } else if (fm.target == "origin_x") {
        origin_x_mapping = &fm;
      } else if (fm.target == "origin_y") {
        origin_y_mapping = &fm;
      } else if (fm.target == "vector_x") {
        vec_x_mapping = &fm;
      } else if (fm.target == "vector_y") {
        vec_y_mapping = &fm;
      }
    }

    auto ox = static_cast<float>(origin_x_mapping ? fbs_get_mapped_double(*root_table, obj, *origin_x_mapping)
                                                  : fbs_get_double(*root_table, obj, "origin_x"));
    auto oy = static_cast<float>(origin_y_mapping ? fbs_get_mapped_double(*root_table, obj, *origin_y_mapping)
                                                  : fbs_get_double(*root_table, obj, "origin_y"));
    auto vx = static_cast<float>(vec_x_mapping ? fbs_get_mapped_double(*root_table, obj, *vec_x_mapping)
                                               : fbs_get_double(*root_table, obj, "vector_x"));
    auto vy = static_cast<float>(vec_y_mapping ? fbs_get_mapped_double(*root_table, obj, *vec_y_mapping)
                                               : fbs_get_double(*root_table, obj, "vector_y"));

    auto arrows = ::rerun::archetypes::Arrows2D::from_vectors({::rerun::components::Vector2D(vx, vy)});
    arrows = std::move(arrows).with_origins({::rerun::Position2D(ox, oy)});
    rec.log(entity_path, arrows);
    return true;
  }

  if (archetype == "Arrows3D") {
    const FieldMapping* origin_x_mapping = nullptr;
    const FieldMapping* origin_y_mapping = nullptr;
    const FieldMapping* origin_z_mapping = nullptr;
    const FieldMapping* vec_x_mapping = nullptr;
    const FieldMapping* vec_y_mapping = nullptr;
    const FieldMapping* vec_z_mapping = nullptr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "origin_x") {
        origin_x_mapping = &fm;
      } else if (fm.target == "origin_y") {
        origin_y_mapping = &fm;
      } else if (fm.target == "origin_z") {
        origin_z_mapping = &fm;
      } else if (fm.target == "vector_x") {
        vec_x_mapping = &fm;
      } else if (fm.target == "vector_y") {
        vec_y_mapping = &fm;
      } else if (fm.target == "vector_z") {
        vec_z_mapping = &fm;
      }
    }

    auto ox = static_cast<float>(origin_x_mapping ? fbs_get_mapped_double(*root_table, obj, *origin_x_mapping)
                                                  : fbs_get_double(*root_table, obj, "origin_x"));
    auto oy = static_cast<float>(origin_y_mapping ? fbs_get_mapped_double(*root_table, obj, *origin_y_mapping)
                                                  : fbs_get_double(*root_table, obj, "origin_y"));
    auto oz = static_cast<float>(origin_z_mapping ? fbs_get_mapped_double(*root_table, obj, *origin_z_mapping)
                                                  : fbs_get_double(*root_table, obj, "origin_z"));
    auto vx = static_cast<float>(vec_x_mapping ? fbs_get_mapped_double(*root_table, obj, *vec_x_mapping)
                                               : fbs_get_double(*root_table, obj, "vector_x"));
    auto vy = static_cast<float>(vec_y_mapping ? fbs_get_mapped_double(*root_table, obj, *vec_y_mapping)
                                               : fbs_get_double(*root_table, obj, "vector_y"));
    auto vz = static_cast<float>(vec_z_mapping ? fbs_get_mapped_double(*root_table, obj, *vec_z_mapping)
                                               : fbs_get_double(*root_table, obj, "vector_z"));

    auto arrows = ::rerun::archetypes::Arrows3D::from_vectors({::rerun::Vector3D(vx, vy, vz)});
    arrows = std::move(arrows).with_origins({::rerun::Position3D(ox, oy, oz)});
    rec.log(entity_path, arrows);
    return true;
  }

  if (archetype == "Cylinders3D") {
    const FieldMapping* length_mapping = nullptr;
    const FieldMapping* radius_mapping = nullptr;
    const FieldMapping* cx_mapping = nullptr;
    const FieldMapping* cy_mapping = nullptr;
    const FieldMapping* cz_mapping = nullptr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "length") {
        length_mapping = &fm;
      } else if (fm.target == "radius") {
        radius_mapping = &fm;
      } else if (fm.target == "center_x") {
        cx_mapping = &fm;
      } else if (fm.target == "center_y") {
        cy_mapping = &fm;
      } else if (fm.target == "center_z") {
        cz_mapping = &fm;
      }
    }

    auto length = static_cast<float>(length_mapping ? fbs_get_mapped_double(*root_table, obj, *length_mapping)
                                                    : fbs_get_double(*root_table, obj, "length"));
    auto radius = static_cast<float>(radius_mapping ? fbs_get_mapped_double(*root_table, obj, *radius_mapping)
                                                    : fbs_get_double(*root_table, obj, "radius"));
    auto cx = static_cast<float>(cx_mapping ? fbs_get_mapped_double(*root_table, obj, *cx_mapping)
                                            : fbs_get_double(*root_table, obj, "center_x"));
    auto cy = static_cast<float>(cy_mapping ? fbs_get_mapped_double(*root_table, obj, *cy_mapping)
                                            : fbs_get_double(*root_table, obj, "center_y"));
    auto cz = static_cast<float>(cz_mapping ? fbs_get_mapped_double(*root_table, obj, *cz_mapping)
                                            : fbs_get_double(*root_table, obj, "center_z"));

    auto cylinders = ::rerun::archetypes::Cylinders3D::from_lengths_and_radii({length}, {radius});
    cylinders = std::move(cylinders).with_centers({::rerun::datatypes::Vec3D{cx, cy, cz}});
    rec.log(entity_path, cylinders);
    return true;
  }

  if (archetype == "Ellipsoids3D") {
    const FieldMapping* hx_mapping = nullptr;
    const FieldMapping* hy_mapping = nullptr;
    const FieldMapping* hz_mapping = nullptr;
    const FieldMapping* cx_mapping = nullptr;
    const FieldMapping* cy_mapping = nullptr;
    const FieldMapping* cz_mapping = nullptr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "half_size_x") {
        hx_mapping = &fm;
      } else if (fm.target == "half_size_y") {
        hy_mapping = &fm;
      } else if (fm.target == "half_size_z") {
        hz_mapping = &fm;
      } else if (fm.target == "center_x") {
        cx_mapping = &fm;
      } else if (fm.target == "center_y") {
        cy_mapping = &fm;
      } else if (fm.target == "center_z") {
        cz_mapping = &fm;
      }
    }

    auto hx = static_cast<float>(hx_mapping ? fbs_get_mapped_double(*root_table, obj, *hx_mapping)
                                            : fbs_get_double(*root_table, obj, "half_size_x"));
    auto hy = static_cast<float>(hy_mapping ? fbs_get_mapped_double(*root_table, obj, *hy_mapping)
                                            : fbs_get_double(*root_table, obj, "half_size_y"));
    auto hz = static_cast<float>(hz_mapping ? fbs_get_mapped_double(*root_table, obj, *hz_mapping)
                                            : fbs_get_double(*root_table, obj, "half_size_z"));
    auto cx = static_cast<float>(cx_mapping ? fbs_get_mapped_double(*root_table, obj, *cx_mapping)
                                            : fbs_get_double(*root_table, obj, "center_x"));
    auto cy = static_cast<float>(cy_mapping ? fbs_get_mapped_double(*root_table, obj, *cy_mapping)
                                            : fbs_get_double(*root_table, obj, "center_y"));
    auto cz = static_cast<float>(cz_mapping ? fbs_get_mapped_double(*root_table, obj, *cz_mapping)
                                            : fbs_get_double(*root_table, obj, "center_z"));

    auto ellipsoids = ::rerun::archetypes::Ellipsoids3D::from_centers_and_half_sizes(
        {::rerun::datatypes::Vec3D{cx, cy, cz}}, {::rerun::HalfSize3D(hx, hy, hz)});
    rec.log(entity_path, ellipsoids);
    return true;
  }

  if (archetype == "BarChart") {
    std::string values_src;
    const FieldMapping* values_mapping = nullptr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "values") {
        values_src = fm.source;
        values_mapping = &fm;
        break;
      }
    }

    std::string field_name = values_src.empty() ? "values" : values_src;
    const auto* val_field = find_fbs_field(obj, field_name);

    if (val_field && val_field->type()->base_type() == reflection::Vector) {
      std::vector<double> values;

      switch (val_field->type()->element()) {
        case reflection::Float: {
          const auto* vec = flatbuffers::GetFieldV<float>(*root_table, *val_field);

          if (vec) {
            values.reserve(vec->size());

            for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
              values.emplace_back(static_cast<double>(vec->Get(i)));
            }
          }

          break;
        }

        case reflection::Double: {
          const auto* vec = flatbuffers::GetFieldV<double>(*root_table, *val_field);

          if (vec) {
            values.reserve(vec->size());

            for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
              values.emplace_back(vec->Get(i));
            }
          }

          break;
        }

        case reflection::Int: {
          const auto* vec = flatbuffers::GetFieldV<int32_t>(*root_table, *val_field);

          if (vec) {
            values.reserve(vec->size());

            for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
              values.emplace_back(static_cast<double>(vec->Get(i)));
            }
          }

          break;
        }

        default:
          break;
      }

      if (!values.empty()) {
        rec.log(entity_path, ::rerun::archetypes::BarChart::f64(std::move(values)));
        return true;
      }
    }

    double value = values_mapping ? fbs_get_mapped_double(*root_table, obj, *values_mapping)
                                  : fbs_get_double(*root_table, obj, field_name);
    rec.log(entity_path, ::rerun::archetypes::BarChart::f64({value}));
    return true;
  }

  if (archetype == "Points2D") {
    std::string entities_src;
    const FieldMapping* x_mapping = nullptr;
    const FieldMapping* y_mapping = nullptr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "entities" || fm.target == "points") {
        entities_src = fm.source;
      } else if (fm.target == "point_x") {
        x_mapping = &fm;
      } else if (fm.target == "point_y") {
        y_mapping = &fm;
      }
    }

    std::string field_name = entities_src.empty() ? "points" : entities_src;
    const auto* vec_field = find_fbs_field(obj, field_name);

    if (vec_field && vec_field->type()->base_type() == reflection::Vector &&
        vec_field->type()->element() == reflection::Obj) {
      FbsObjectView root{&obj, root_table, nullptr};
      const auto* vec = get_fbs_vector(root, *vec_field);

      if (vec) {
        std::vector<::rerun::Position2D> positions;
        positions.reserve(vec->size());
        for_each_fbs_vector_object(root, *vec_field, *schema, [&](const FbsObjectView& item, flatbuffers::uoffset_t) {
          auto px = static_cast<float>(x_mapping ? fbs_get_object_mapped_double(item, *x_mapping)
                                                 : get_fbs_double(item, "x"));
          auto py = static_cast<float>(y_mapping ? fbs_get_object_mapped_double(item, *y_mapping)
                                                 : get_fbs_double(item, "y"));
          positions.emplace_back(px, py);
        });

        if (!positions.empty()) {
          rec.log(entity_path, ::rerun::archetypes::Points2D(std::move(positions)));
        }

        return true;
      }
    }

    auto px = static_cast<float>(x_mapping ? fbs_get_mapped_double(*root_table, obj, *x_mapping)
                                           : fbs_get_double(*root_table, obj, "x"));
    auto py = static_cast<float>(y_mapping ? fbs_get_mapped_double(*root_table, obj, *y_mapping)
                                           : fbs_get_double(*root_table, obj, "y"));
    rec.log(entity_path, ::rerun::archetypes::Points2D({::rerun::Position2D(px, py)}));
    return true;
  }

  if (archetype == "Points3D") {
    std::string entities_src;
    const FieldMapping* x_mapping = nullptr;
    const FieldMapping* y_mapping = nullptr;
    const FieldMapping* z_mapping = nullptr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "entities" || fm.target == "points") {
        entities_src = fm.source;
      } else if (fm.target == "point_x") {
        x_mapping = &fm;
      } else if (fm.target == "point_y") {
        y_mapping = &fm;
      } else if (fm.target == "point_z") {
        z_mapping = &fm;
      }
    }

    std::string field_name = entities_src.empty() ? "points" : entities_src;
    const auto* vec_field = find_fbs_field(obj, field_name);

    if (vec_field && vec_field->type()->base_type() == reflection::Vector &&
        vec_field->type()->element() == reflection::Obj) {
      FbsObjectView root{&obj, root_table, nullptr};
      const auto* vec = get_fbs_vector(root, *vec_field);

      if (vec) {
        std::vector<::rerun::Position3D> positions;
        positions.reserve(vec->size());
        for_each_fbs_vector_object(root, *vec_field, *schema, [&](const FbsObjectView& item, flatbuffers::uoffset_t) {
          auto px = static_cast<float>(x_mapping ? fbs_get_object_mapped_double(item, *x_mapping)
                                                 : get_fbs_double(item, "x"));
          auto py = static_cast<float>(y_mapping ? fbs_get_object_mapped_double(item, *y_mapping)
                                                 : get_fbs_double(item, "y"));
          auto pz = static_cast<float>(z_mapping ? fbs_get_object_mapped_double(item, *z_mapping)
                                                 : get_fbs_double(item, "z"));
          positions.emplace_back(px, py, pz);
        });

        if (!positions.empty()) {
          rec.log(entity_path, ::rerun::archetypes::Points3D(std::move(positions)));
        }

        return true;
      }
    }

    auto px = static_cast<float>(x_mapping ? fbs_get_mapped_double(*root_table, obj, *x_mapping)
                                           : fbs_get_double(*root_table, obj, "x"));
    auto py = static_cast<float>(y_mapping ? fbs_get_mapped_double(*root_table, obj, *y_mapping)
                                           : fbs_get_double(*root_table, obj, "y"));
    auto pz = static_cast<float>(z_mapping ? fbs_get_mapped_double(*root_table, obj, *z_mapping)
                                           : fbs_get_double(*root_table, obj, "z"));
    rec.log(entity_path, ::rerun::archetypes::Points3D({::rerun::Position3D(px, py, pz)}));
    return true;
  }

  if (archetype == "AnnotationContext" || archetype == "GeoLineStrings" || archetype == "Mesh3D") {
    return false;
  }

  if (archetype == "Capsules3D") {
    const FieldMapping* length_mapping = nullptr;
    const FieldMapping* radius_mapping = nullptr;
    const FieldMapping* cx_mapping = nullptr;
    const FieldMapping* cy_mapping = nullptr;
    const FieldMapping* cz_mapping = nullptr;
    const FieldMapping* qx_mapping = nullptr;
    const FieldMapping* qy_mapping = nullptr;
    const FieldMapping* qz_mapping = nullptr;
    const FieldMapping* qw_mapping = nullptr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "length") {
        length_mapping = &fm;
      } else if (fm.target == "radius") {
        radius_mapping = &fm;
      } else if (fm.target == "center_x") {
        cx_mapping = &fm;
      } else if (fm.target == "center_y") {
        cy_mapping = &fm;
      } else if (fm.target == "center_z") {
        cz_mapping = &fm;
      } else if (fm.target == "qx" || fm.target == "rotation.x") {
        qx_mapping = &fm;
      } else if (fm.target == "qy" || fm.target == "rotation.y") {
        qy_mapping = &fm;
      } else if (fm.target == "qz" || fm.target == "rotation.z") {
        qz_mapping = &fm;
      } else if (fm.target == "qw" || fm.target == "rotation.w") {
        qw_mapping = &fm;
      }
    }

    auto length = static_cast<float>(length_mapping ? fbs_get_mapped_double(*root_table, obj, *length_mapping)
                                                    : fbs_get_double(*root_table, obj, "length"));
    auto radius = static_cast<float>(radius_mapping ? fbs_get_mapped_double(*root_table, obj, *radius_mapping)
                                                    : fbs_get_double(*root_table, obj, "radius"));
    auto cx = static_cast<float>(cx_mapping ? fbs_get_mapped_double(*root_table, obj, *cx_mapping)
                                            : fbs_get_double(*root_table, obj, "center_x"));
    auto cy = static_cast<float>(cy_mapping ? fbs_get_mapped_double(*root_table, obj, *cy_mapping)
                                            : fbs_get_double(*root_table, obj, "center_y"));
    auto cz = static_cast<float>(cz_mapping ? fbs_get_mapped_double(*root_table, obj, *cz_mapping)
                                            : fbs_get_double(*root_table, obj, "center_z"));

    auto capsules = ::rerun::archetypes::Capsules3D::from_lengths_and_radii({length}, {radius});
    capsules = std::move(capsules).with_translations({::rerun::components::Translation3D(cx, cy, cz)});

    auto qx = static_cast<float>(qx_mapping ? fbs_get_mapped_double(*root_table, obj, *qx_mapping)
                                            : fbs_get_double(*root_table, obj, "qx"));
    auto qy = static_cast<float>(qy_mapping ? fbs_get_mapped_double(*root_table, obj, *qy_mapping)
                                            : fbs_get_double(*root_table, obj, "qy"));
    auto qz = static_cast<float>(qz_mapping ? fbs_get_mapped_double(*root_table, obj, *qz_mapping)
                                            : fbs_get_double(*root_table, obj, "qz"));
    auto qw = static_cast<float>(qw_mapping ? fbs_get_mapped_double(*root_table, obj, *qw_mapping)
                                            : fbs_get_double(*root_table, obj, "qw"));

    if (qx != 0.0F || qy != 0.0F || qz != 0.0F || qw != 0.0F) {
      capsules = std::move(capsules).with_quaternions({::rerun::datatypes::Quaternion::from_xyzw(qx, qy, qz, qw)});
    }

    rec.log(entity_path, capsules);
    return true;
  }

  if (archetype == "EncodedDepthImage") {
    std::string data_field_name = "data";
    const FieldMapping* media_type_mapping = nullptr;
    const FieldMapping* meter_mapping = nullptr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "data") {
        data_field_name = fm.source;
      } else if (fm.target == "media_type") {
        media_type_mapping = &fm;
      } else if (fm.target == "meter") {
        meter_mapping = &fm;
      }
    }

    const auto* data_fld = find_fbs_field(obj, data_field_name);

    if (data_fld && is_fbs_byte_vector(*data_fld)) {
      const auto* vec = flatbuffers::GetFieldV<uint8_t>(*root_table, *data_fld);

      // NOLINTNEXTLINE(readability-container-size-empty)
      if VLIKELY (vec != nullptr && vec->size() != 0) {
        auto blob = ::rerun::Collection<uint8_t>::borrow(vec->data(), vec->size());
        auto depth_image = ::rerun::archetypes::EncodedDepthImage(::rerun::components::Blob(std::move(blob)));

        auto media_type_str = media_type_mapping ? fbs_get_mapped_string(*root_table, obj, *media_type_mapping)
                                                 : fbs_get_string(*root_table, obj, "media_type");

        if (!media_type_str.empty()) {
          depth_image = std::move(depth_image).with_media_type(::rerun::components::MediaType(media_type_str));
        }

        auto meter_val = static_cast<float>(meter_mapping ? fbs_get_mapped_double(*root_table, obj, *meter_mapping)
                                                          : fbs_get_double(*root_table, obj, "meter"));

        if (meter_val > 0.0F) {
          depth_image = std::move(depth_image).with_meter(::rerun::components::DepthMeter(meter_val));
        }

        rec.log(entity_path, depth_image);
        return true;
      }
    }
  }

  if (archetype == "Asset3D") {
    std::string data_field_name = "data";
    const FieldMapping* media_type_mapping = nullptr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "data") {
        data_field_name = fm.source;
      } else if (fm.target == "media_type") {
        media_type_mapping = &fm;
      }
    }

    const auto* data_fld = find_fbs_field(obj, data_field_name);

    if (data_fld && is_fbs_byte_vector(*data_fld)) {
      const auto* vec = flatbuffers::GetFieldV<uint8_t>(*root_table, *data_fld);

      // NOLINTNEXTLINE(readability-container-size-empty)
      if VLIKELY (vec != nullptr && vec->size() != 0) {
        auto blob = ::rerun::Collection<uint8_t>::borrow(vec->data(), vec->size());
        auto media_type_str = media_type_mapping ? fbs_get_mapped_string(*root_table, obj, *media_type_mapping)
                                                 : fbs_get_string(*root_table, obj, "media_type");

        auto asset = ::rerun::archetypes::Asset3D::from_file_contents(
            std::move(blob), media_type_str.empty() ? std::optional<::rerun::components::MediaType>(std::nullopt)
                                                    : std::optional<::rerun::components::MediaType>(media_type_str));

        rec.log(entity_path, asset);
        return true;
      }
    }
  }

  if (archetype == "GraphNodes" || archetype == "GraphEdges") {
    return false;
  }

  if (archetype == "ViewCoordinates") {
    const FieldMapping* system_mapping = nullptr;
    const FieldMapping* coordinates_mapping = nullptr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "system") {
        system_mapping = &fm;
      } else if (fm.target == "coordinates") {
        coordinates_mapping = &fm;
      }
    }

    auto system_str = system_mapping ? fbs_get_mapped_string(*root_table, obj, *system_mapping)
                                     : fbs_get_string(*root_table, obj, "system");

    if (system_str.empty()) {
      system_str = coordinates_mapping ? fbs_get_mapped_string(*root_table, obj, *coordinates_mapping)
                                       : fbs_get_string(*root_table, obj, "coordinates");
    }

    log_view_coordinates_value(rec, entity_path, system_str);
    return true;
  }

  if (archetype == "InstancePoses3D") {
    const FieldMapping* tx_mapping = nullptr;
    const FieldMapping* ty_mapping = nullptr;
    const FieldMapping* tz_mapping = nullptr;
    const FieldMapping* qx_mapping = nullptr;
    const FieldMapping* qy_mapping = nullptr;
    const FieldMapping* qz_mapping = nullptr;
    const FieldMapping* qw_mapping = nullptr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "translation_x" || fm.target == "translation.x") {
        tx_mapping = &fm;
      } else if (fm.target == "translation_y" || fm.target == "translation.y") {
        ty_mapping = &fm;
      } else if (fm.target == "translation_z" || fm.target == "translation.z") {
        tz_mapping = &fm;
      } else if (fm.target == "qx" || fm.target == "rotation.x") {
        qx_mapping = &fm;
      } else if (fm.target == "qy" || fm.target == "rotation.y") {
        qy_mapping = &fm;
      } else if (fm.target == "qz" || fm.target == "rotation.z") {
        qz_mapping = &fm;
      } else if (fm.target == "qw" || fm.target == "rotation.w") {
        qw_mapping = &fm;
      }
    }

    auto tx = static_cast<float>(tx_mapping ? fbs_get_mapped_double(*root_table, obj, *tx_mapping)
                                            : fbs_get_double(*root_table, obj, "translation_x"));
    auto ty = static_cast<float>(ty_mapping ? fbs_get_mapped_double(*root_table, obj, *ty_mapping)
                                            : fbs_get_double(*root_table, obj, "translation_y"));
    auto tz = static_cast<float>(tz_mapping ? fbs_get_mapped_double(*root_table, obj, *tz_mapping)
                                            : fbs_get_double(*root_table, obj, "translation_z"));

    auto poses = ::rerun::archetypes::InstancePoses3D();
    poses = std::move(poses).with_translations({::rerun::components::Translation3D(tx, ty, tz)});

    auto qx = static_cast<float>(qx_mapping ? fbs_get_mapped_double(*root_table, obj, *qx_mapping)
                                            : fbs_get_double(*root_table, obj, "qx"));
    auto qy = static_cast<float>(qy_mapping ? fbs_get_mapped_double(*root_table, obj, *qy_mapping)
                                            : fbs_get_double(*root_table, obj, "qy"));
    auto qz = static_cast<float>(qz_mapping ? fbs_get_mapped_double(*root_table, obj, *qz_mapping)
                                            : fbs_get_double(*root_table, obj, "qz"));
    auto qw = static_cast<float>(qw_mapping ? fbs_get_mapped_double(*root_table, obj, *qw_mapping)
                                            : fbs_get_double(*root_table, obj, "qw"));

    if (qx != 0.0F || qy != 0.0F || qz != 0.0F || qw != 0.0F) {
      poses = std::move(poses).with_quaternions({::rerun::datatypes::Quaternion::from_xyzw(qx, qy, qz, qw)});
    }

    rec.log(entity_path, poses);
    return true;
  }

  if (archetype == "AssetVideo") {
    std::string data_field_name = "data";
    const FieldMapping* media_type_mapping = nullptr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "data") {
        data_field_name = fm.source;
      } else if (fm.target == "media_type") {
        media_type_mapping = &fm;
      }
    }

    const auto* data_fld = find_fbs_field(obj, data_field_name);

    if (data_fld && is_fbs_byte_vector(*data_fld)) {
      const auto* vec = flatbuffers::GetFieldV<uint8_t>(*root_table, *data_fld);

      // NOLINTNEXTLINE(readability-container-size-empty)
      if VLIKELY (vec != nullptr && vec->size() != 0) {
        auto blob = ::rerun::Collection<uint8_t>::borrow(vec->data(), vec->size());
        auto media_type_str = media_type_mapping ? fbs_get_mapped_string(*root_table, obj, *media_type_mapping)
                                                 : fbs_get_string(*root_table, obj, "media_type");

        auto video = ::rerun::archetypes::AssetVideo::from_bytes(
            std::move(blob), media_type_str.empty() ? std::optional<::rerun::components::MediaType>(std::nullopt)
                                                    : std::optional<::rerun::components::MediaType>(media_type_str));

        rec.log(entity_path, video);
        return true;
      }
    }
  }

  if (archetype == "VideoFrameReference") {
    const FieldMapping* timestamp_mapping = nullptr;
    const FieldMapping* video_reference_mapping = nullptr;

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "timestamp_ns") {
        timestamp_mapping = &fm;
      } else if (fm.target == "video_reference") {
        video_reference_mapping = &fm;
      }
    }

    auto ts_ns = static_cast<int64_t>(timestamp_mapping ? fbs_get_mapped_double(*root_table, obj, *timestamp_mapping)
                                                        : fbs_get_double(*root_table, obj, "timestamp_ns"));
    auto frame_ref =
        ::rerun::archetypes::VideoFrameReference(::rerun::components::VideoTimestamp(std::chrono::nanoseconds(ts_ns)));

    auto ref_path = video_reference_mapping ? fbs_get_mapped_string(*root_table, obj, *video_reference_mapping)
                                            : fbs_get_string(*root_table, obj, "video_reference");

    if (!ref_path.empty()) {
      frame_ref = std::move(frame_ref).with_video_reference(ref_path);
    }

    rec.log(entity_path, frame_ref);
    return true;
  }

  if (archetype == "Tensor") {
    std::string shape_field_name = "shape";
    std::string data_field_name = "data";
    std::string dim_names_field_name = "dim_names";

    for (const auto& fm : mapping.field_mappings) {
      if (fm.target == "shape") {
        shape_field_name = fm.source;
      } else if (fm.target == "data") {
        data_field_name = fm.source;
      } else if (fm.target == "dim_names") {
        dim_names_field_name = fm.source;
      }
    }

    const auto* shape_fld = find_fbs_field(obj, shape_field_name);
    const auto* data_fld = find_fbs_field(obj, data_field_name);

    if (!shape_fld || !data_fld || shape_fld->type()->base_type() != reflection::Vector ||
        !is_fbs_byte_vector(*data_fld)) {
      return false;
    }

    std::vector<uint64_t> shape;

    switch (shape_fld->type()->element()) {
      case reflection::UByte: {
        const auto* vec = flatbuffers::GetFieldV<uint8_t>(*root_table, *shape_fld);

        if (vec) {
          shape.reserve(vec->size());

          for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
            shape.emplace_back(vec->Get(i));
          }
        }

        break;
      }

      case reflection::Byte: {
        const auto* vec = flatbuffers::GetFieldV<int8_t>(*root_table, *shape_fld);

        if (vec) {
          shape.reserve(vec->size());

          for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
            shape.emplace_back(static_cast<uint64_t>(vec->Get(i)));
          }
        }

        break;
      }

      case reflection::UShort: {
        const auto* vec = flatbuffers::GetFieldV<uint16_t>(*root_table, *shape_fld);

        if (vec) {
          shape.reserve(vec->size());

          for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
            shape.emplace_back(vec->Get(i));
          }
        }

        break;
      }

      case reflection::Short: {
        const auto* vec = flatbuffers::GetFieldV<int16_t>(*root_table, *shape_fld);

        if (vec) {
          shape.reserve(vec->size());

          for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
            shape.emplace_back(static_cast<uint64_t>(vec->Get(i)));
          }
        }

        break;
      }

      case reflection::UInt: {
        const auto* vec = flatbuffers::GetFieldV<uint32_t>(*root_table, *shape_fld);

        if (vec) {
          shape.reserve(vec->size());

          for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
            shape.emplace_back(vec->Get(i));
          }
        }

        break;
      }

      case reflection::Int: {
        const auto* vec = flatbuffers::GetFieldV<int32_t>(*root_table, *shape_fld);

        if (vec) {
          shape.reserve(vec->size());

          for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
            shape.emplace_back(static_cast<uint64_t>(vec->Get(i)));
          }
        }

        break;
      }

      case reflection::ULong: {
        const auto* vec = flatbuffers::GetFieldV<uint64_t>(*root_table, *shape_fld);

        if (vec) {
          shape.reserve(vec->size());

          for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
            shape.emplace_back(vec->Get(i));
          }
        }

        break;
      }

      case reflection::Long: {
        const auto* vec = flatbuffers::GetFieldV<int64_t>(*root_table, *shape_fld);

        if (vec) {
          shape.reserve(vec->size());

          for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
            shape.emplace_back(static_cast<uint64_t>(vec->Get(i)));
          }
        }

        break;
      }

      default:
        return false;
    }

    const auto* data_vec = flatbuffers::GetFieldV<uint8_t>(*root_table, *data_fld);

    // NOLINTNEXTLINE(readability-container-size-empty)
    if (data_vec == nullptr || data_vec->size() == 0 || shape.empty()) {
      return false;
    }

    auto data_collection = ::rerun::Collection<uint8_t>::borrow(data_vec->data(), data_vec->size());
    auto tensor =
        ::rerun::archetypes::Tensor(std::move(shape), ::rerun::datatypes::TensorBuffer(std::move(data_collection)));

    const auto* dim_names_fld = find_fbs_field(obj, dim_names_field_name);

    if (dim_names_fld && dim_names_fld->type()->base_type() == reflection::Vector &&
        dim_names_fld->type()->element() == reflection::String) {
      const auto* names_vec =
          flatbuffers::GetFieldV<flatbuffers::Offset<flatbuffers::String>>(*root_table, *dim_names_fld);

      if (names_vec) {
        std::vector<std::string> dim_names;
        dim_names.reserve(names_vec->size());

        for (flatbuffers::uoffset_t i = 0; i < names_vec->size(); ++i) {
          const auto* name = names_vec->Get(i);

          if (name) {
            dim_names.emplace_back(name->str());
          }
        }

        if (!dim_names.empty()) {
          tensor = std::move(tensor).with_dim_names(std::move(dim_names));
        }
      }
    }

    rec.log(entity_path, tensor);
    return true;
  }

  rec.log(entity_path, ::rerun::TextLog("[FBS] " + ser + " (no specific converter)"));
  return true;
}

#endif

}  // namespace webviz
}  // namespace vlink
