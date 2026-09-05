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

#include <vlink/base/quantize.h>
#include <vlink/zerocopy/audio_frame.h>
#include <vlink/zerocopy/camera_frame.h>
#include <vlink/zerocopy/occupancy_grid.h>
#include <vlink/zerocopy/point_cloud.h>
#include <vlink/zerocopy/tensor.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

#include "./rerun_writer.h"

namespace vlink {
namespace webviz {

namespace rr = ::rerun;
namespace re = ::rerun::encodings;

static bool product(size_t left, size_t right, size_t& result) {
  if (right && left > SIZE_MAX / right) {
    return false;
  }
  result = left * right;
  return true;
}

template <typename T>
static re::TensorBuffer tensor_buffer(const Bytes& data) {
  const auto count = data.size() / sizeof(T);
  if (reinterpret_cast<uintptr_t>(data.data()) % alignof(T) == 0) {
    return re::TensorBuffer(rr::Collection<T>::borrow(static_cast<const void*>(data.data()), count));
  }
  std::vector<T> aligned(count);
  std::memcpy(aligned.data(), data.data(), data.size());
  return re::TensorBuffer(rr::Collection<T>::take_ownership(std::move(aligned)));
}

static bool tensor(rr::RecordingStream& rec, const std::string& path, const Bytes& data,
                   const std::vector<uint64_t>& shape, const std::vector<std::string>& names, uint64_t type) {
  using Tensor = zerocopy::Tensor;
  const size_t widths[] = {0, 1, 1, 1, 2, 2, 4, 4, 8, 8, 2, 2, 4, 8};
  if (type >= std::size(widths) || !widths[type] || shape.empty()) {
    return false;
  }
  size_t expected = widths[type];
  for (const auto dimension : shape) {
    if (!dimension || dimension > SIZE_MAX || !product(expected, static_cast<size_t>(dimension), expected)) {
      return false;
    }
  }
  if (expected != data.size()) {
    return false;
  }
  re::TensorBuffer buffer;
  switch (type) {
    case Tensor::kBool: {
      const bool normalize =
          std::any_of(data.data(), data.data() + data.size(), [](uint8_t value) { return value > 1; });
      if (normalize) {
        std::vector<uint8_t> normalized(data.size());
        for (size_t i = 0; i < data.size(); ++i) {
          normalized[i] = data.data()[i] != 0 ? 1U : 0U;
        }
        buffer = re::TensorBuffer(rr::Collection<uint8_t>::take_ownership(std::move(normalized)));
      } else {
        buffer = tensor_buffer<uint8_t>(data);
      }
      break;
    }
    case Tensor::kInt8:
      buffer = tensor_buffer<int8_t>(data);
      break;
    case Tensor::kUint8:
      buffer = tensor_buffer<uint8_t>(data);
      break;
    case Tensor::kInt16:
      buffer = tensor_buffer<int16_t>(data);
      break;
    case Tensor::kUint16:
      buffer = tensor_buffer<uint16_t>(data);
      break;
    case Tensor::kInt32:
      buffer = tensor_buffer<int32_t>(data);
      break;
    case Tensor::kUint32:
      buffer = tensor_buffer<uint32_t>(data);
      break;
    case Tensor::kInt64:
      buffer = tensor_buffer<int64_t>(data);
      break;
    case Tensor::kUint64:
      buffer = tensor_buffer<uint64_t>(data);
      break;
    case Tensor::kFloat16:
      buffer = tensor_buffer<rr::half>(data);
      break;
    case Tensor::kFloat32:
      buffer = tensor_buffer<float>(data);
      break;
    case Tensor::kFloat64:
      buffer = tensor_buffer<double>(data);
      break;
    case Tensor::kBfloat16: {
      std::vector<float> values(data.size() / 2);
      for (size_t i = 0; i < values.size(); ++i) {
        uint16_t bits;
        std::memcpy(&bits, data.data() + i * 2, 2);
        const uint32_t expanded = static_cast<uint32_t>(bits) << 16;
        std::memcpy(&values[i], &expanded, 4);
      }
      buffer = re::TensorBuffer(rr::Collection<float>::take_ownership(std::move(values)));
      break;
    }
    default:
      return false;
  }
  auto value = rr::Tensor(shape, std::move(buffer));
  if (!names.empty()) {
    value = std::move(value).with_dim_names(names);
  }
  return rec.try_log(path, value).is_ok();
}

static bool camera(rr::RecordingStream& rec, const std::string& path, const Bytes& raw, std::string_view timeline) {
  using Camera = zerocopy::CameraFrame;
  Camera frame;
  if (!(frame << raw) || frame.size() == 0) {
    return false;
  }
  if (!timeline.empty() && frame.header.time_meas > 0) {
    rec.set_time_timestamp_nanos_since_epoch(
        timeline, static_cast<int64_t>(std::min<uint64_t>(frame.header.time_meas, INT64_MAX)));
  }
  const auto format = frame.format();
  auto data = Bytes::shallow_copy(frame.data(), frame.size());
  auto blob = rr::Collection<uint8_t>::borrow(data.data(), data.size());
  if (format == Camera::kFormatH264 || format == Camera::kFormatH265 || format == Camera::kFormatAv1) {
    if (frame.stream() == Camera::kStreamB) {
      return false;
    }
    auto codec = rr::components::VideoCodec::AV1;
    if (format == Camera::kFormatH264) {
      codec = rr::components::VideoCodec::H264;
    } else if (format == Camera::kFormatH265) {
      codec = rr::components::VideoCodec::H265;
    }
    return rec.try_log(path, rr::VideoStream(codec).with_sample(rr::components::VideoSample(blob))).is_ok();
  }
  if (format == Camera::kFormatJpeg || format == Camera::kFormatMjpeg || format == Camera::kFormatPng ||
      format == Camera::kFormatWebp) {
    const char* media = "image/jpeg";
    if (format == Camera::kFormatPng) {
      media = "image/png";
    } else if (format == Camera::kFormatWebp) {
      media = "image/webp";
    }
    return rec.try_log(path, rr::EncodedImage::from_bytes(blob, rr::MediaType(media))).is_ok();
  }
  const auto width = frame.width();
  const auto height = frame.height();
  size_t pixels;
  if (!width || !height || !product(width, height, pixels)) {
    return false;
  }
  const rr::WidthHeight resolution{width, height};
  const auto image = [&](re::ColorModel model, re::ChannelDatatype type = re::ChannelDatatype::U8) {
    const auto expected = re::ImageFormat(resolution, model, type).num_bytes();
    return expected == data.size() &&
           rec.try_log(path,
                       rr::Image(rr::Collection<uint8_t>::borrow(data.data(), data.size()), resolution, model, type))
               .is_ok();
  };
  if (format >= Camera::kFormatUint8C1 && format <= Camera::kFormatFloat64C4) {
    const uint8_t group = (format - Camera::kFormatUint8C1) / 4;
    const uint8_t channels = (format - Camera::kFormatUint8C1) % 4 + 1;
    const re::ChannelDatatype types[] = {re::ChannelDatatype::U8,  re::ChannelDatatype::I8,  re::ChannelDatatype::U16,
                                         re::ChannelDatatype::I16, re::ChannelDatatype::I32, re::ChannelDatatype::F32,
                                         re::ChannelDatatype::F64};
    const uint8_t tensor_types[] = {3, 2, 5, 4, 6, 12, 13};
    if (channels == 1) {
      return image(re::ColorModel::L, types[group]);
    }
    return tensor(rec, path, data, {height, width, channels}, {"height", "width", "channel"}, tensor_types[group]);
  }
  if (format == Camera::kFormatRgb888Packed) {
    return image(re::ColorModel::RGB);
  }
  if (format == Camera::kFormatBgr888Packed) {
    return image(re::ColorModel::BGR);
  }
  if (format == Camera::kFormatRgba8888Packed) {
    return image(re::ColorModel::RGBA);
  }
  if (format == Camera::kFormatBgra8888Packed) {
    return image(re::ColorModel::BGRA);
  }
  if (format == Camera::kFormatMono8 || (format >= Camera::kFormatBayerRggb8 && format <= Camera::kFormatBayerGrbg8)) {
    return image(re::ColorModel::L);
  }
  if (format == Camera::kFormatMono16 ||
      (format >= Camera::kFormatBayerRggb16 && format <= Camera::kFormatBayerGrbg16)) {
    return image(re::ColorModel::L, re::ChannelDatatype::U16);
  }
  size_t expected;
  if (format == Camera::kFormatRgb888Planar) {
    if (!product(pixels, 3, expected) || expected != data.size()) {
      return false;
    }
    auto packed = Bytes::create(expected);
    for (size_t i = 0; i < pixels; ++i) {
      for (size_t c = 0; c < 3; ++c) {
        packed.data()[i * 3 + c] = data.data()[c * pixels + i];
      }
    }
    data = std::move(packed);
    return image(re::ColorModel::RGB);
  }
  if (format == Camera::kFormatNv21) {
    if ((width % 2) || (height % 2) || !product(pixels, 3, expected) || expected / 2 != data.size()) {
      return false;
    }
    auto nv12 = Bytes::create(data.size());
    std::memcpy(nv12.data(), data.data(), pixels);
    for (size_t i = pixels; i < data.size(); i += 2) {
      nv12.data()[i] = data.data()[i + 1];
      nv12.data()[i + 1] = data.data()[i];
    }
    return rec
        .try_log(path, rr::Image(rr::Collection<uint8_t>::borrow(nv12.data(), nv12.size()), resolution,
                                 re::PixelFormat::NV12))
        .is_ok();
  }
  if (format == Camera::kFormatYvyu || format == Camera::kFormatUyvy || format == Camera::kFormatVyuy) {
    if ((width % 2) || !product(pixels, 2, expected) || expected != data.size()) {
      return false;
    }
    auto yuy2 = Bytes::create(data.size());
    const size_t y = format == Camera::kFormatYvyu ? 0 : 1;
    size_t u = 3;
    if (format == Camera::kFormatUyvy) {
      u = 0;
    } else if (format == Camera::kFormatVyuy) {
      u = 2;
    }
    const size_t v = u ^ 2U;
    for (size_t i = 0; i < data.size(); i += 4) {
      yuy2.data()[i] = data.data()[i + y];
      yuy2.data()[i + 1] = data.data()[i + u];
      yuy2.data()[i + 2] = data.data()[i + y + 2];
      yuy2.data()[i + 3] = data.data()[i + v];
    }
    return rec
        .try_log(path, rr::Image(rr::Collection<uint8_t>::borrow(yuy2.data(), yuy2.size()), resolution,
                                 re::PixelFormat::YUY2))
        .is_ok();
  }
  if (format == Camera::kFormatYuv444) {
    if (!product(pixels, 3, expected) || expected != data.size()) {
      return false;
    }
    auto rgb = Bytes::create(expected);
    const auto channel = [](double value) { return static_cast<uint8_t>(std::clamp(std::round(value), 0.0, 255.0)); };
    for (size_t i = 0; i < pixels; ++i) {
      const double y = data.data()[i];
      const double u = data.data()[pixels + i] - 128.0;
      const double v = data.data()[2 * pixels + i] - 128.0;
      rgb.data()[3 * i] = channel(y + 1.402 * v);
      rgb.data()[3 * i + 1] = channel(y - 0.344136 * u - 0.714136 * v);
      rgb.data()[3 * i + 2] = channel(y + 1.772 * u);
    }
    data = std::move(rgb);
    return image(re::ColorModel::RGB);
  }
  re::PixelFormat pixel_format;
  switch (format) {
    case Camera::kFormatYuv420:
      pixel_format = re::PixelFormat::Y_U_V12_FullRange;
      break;
    case Camera::kFormatNv12:
      pixel_format = re::PixelFormat::NV12;
      break;
    case Camera::kFormatYuv422:
      pixel_format = re::PixelFormat::Y_U_V16_FullRange;
      break;
    case Camera::kFormatYuyv:
      pixel_format = re::PixelFormat::YUY2;
      break;
    default:
      return false;
  }
  const bool subsampled = format == Camera::kFormatYuv420 || format == Camera::kFormatNv12;
  if ((width % 2) || (subsampled && (height % 2))) {
    return false;
  }
  const size_t multiplier = subsampled ? 3 : 2;
  if (!product(pixels, multiplier, expected) || expected / (subsampled ? 2 : 1) != data.size()) {
    return false;
  }
  return rec.try_log(path, rr::Image(blob, resolution, pixel_format)).is_ok();
}

static bool points(rr::RecordingStream& rec, const std::string& path, const zerocopy::MessageParser& parser) {
  const auto fields = parser.element_fields("data");
  const auto find = [&](std::string_view name) -> const zerocopy::MessageParser::Field* {
    for (const auto& field : fields) {
      if (field.name == name) {
        return &field;
      }
    }
    return nullptr;
  };
  const auto* x = find("x");
  const auto* y = find("y");
  const auto* z = find("z");
  if (!x || !y || !z) {
    return false;
  }
  const auto* r = find("r");
  const auto* g = find("g");
  const auto* b = find("b");
  const auto* a = find("a");
  const bool rgb = r != nullptr && g != nullptr && b != nullptr;
  const auto* intensity = rgb ? nullptr : find("intensity");
  const auto* radius = find("radius");
  const auto* class_id = find("class_id");
  const auto* label = find("label");
  const size_t count = parser.collection_size("data");
  double minimum = std::numeric_limits<double>::max();
  double maximum = std::numeric_limits<double>::lowest();
  if (intensity) {
    for (size_t i = 0; i < count; ++i) {
      double value;
      if (!parser.numeric("data", i, *intensity, value) || !std::isfinite(value)) {
        return false;
      }
      minimum = std::min(minimum, value);
      maximum = std::max(maximum, value);
    }
    if (maximum <= minimum) {
      minimum = 0;
      maximum = 1;
    }
  }
  std::vector<rr::Position3D> positions;
  std::vector<rr::Color> colors;
  std::vector<rr::Radius> radii;
  std::vector<rr::components::ClassId> classes;
  std::vector<rr::Text> labels;
  positions.reserve(count);
  if (rgb || intensity) {
    colors.reserve(count);
  }
  if (radius) {
    radii.reserve(count);
  }
  if (class_id) {
    classes.reserve(count);
  }
  if (label) {
    labels.reserve(count);
  }
  for (size_t i = 0; i < count; ++i) {
    bool valid = true;
    const auto value = [&](const zerocopy::MessageParser::Field* field, double fallback = 0) {
      double number = fallback;
      if (field && (!parser.numeric("data", i, *field, number) || !std::isfinite(number))) {
        valid = false;
      }
      return number;
    };
    const double px = value(x);
    const double py = value(y);
    const double pz = value(z);
    constexpr double kLimit = std::numeric_limits<float>::max();
    if (!valid || std::abs(px) > kLimit || std::abs(py) > kLimit || std::abs(pz) > kLimit) {
      return false;
    }
    positions.emplace_back(static_cast<float>(px), static_cast<float>(py), static_cast<float>(pz));
    const auto channel = [](double number, double limit) {
      return number >= 0 && number <= limit ? static_cast<uint16_t>(number) : uint16_t{0};
    };
    if (rgb) {
      const auto red = channel(value(r), 255);
      const auto green = channel(value(g), 255);
      const auto blue = channel(value(b), 255);
      colors.emplace_back(static_cast<uint8_t>(red), static_cast<uint8_t>(green), static_cast<uint8_t>(blue),
                          static_cast<uint8_t>(channel(value(a, 255), 255)));
    } else if (intensity) {
      const double scale = std::isfinite(maximum - minimum) ? 1 : std::max(std::abs(minimum), std::abs(maximum));
      const double normalized =
          std::clamp((value(intensity) / scale - minimum / scale) / (maximum / scale - minimum / scale), 0.0, 1.0);
      const double scaled = normalized * 4;
      const uint8_t red = static_cast<uint8_t>(255 * std::clamp(scaled - 2, 0.0, 1.0));
      const uint8_t green = static_cast<uint8_t>(255 * std::clamp(std::min(scaled, 4 - scaled), 0.0, 1.0));
      const uint8_t blue = static_cast<uint8_t>(255 * std::clamp(2 - scaled, 0.0, 1.0));
      colors.emplace_back(red, green, blue);
    }
    if (radius) {
      const auto number = value(radius);
      if (std::abs(number) > kLimit) {
        return false;
      }
      radii.emplace_back(static_cast<float>(number));
    }
    if (class_id) {
      classes.emplace_back(channel(value(class_id), UINT16_MAX));
    }
    if (label) {
      zerocopy::MessageParser::Value value;
      if (!parser.value("data", i, *label, value)) {
        return false;
      }
      if (const auto* integer = std::get_if<int64_t>(&value)) {
        labels.emplace_back(std::to_string(*integer));
      } else if (const auto* integer = std::get_if<uint64_t>(&value)) {
        labels.emplace_back(std::to_string(*integer));
      } else if (const auto* number = std::get_if<double>(&value)) {
        if (!std::isfinite(*number) || *number < -0x1p63 || *number >= 0x1p63) {
          return false;
        }
        labels.emplace_back(std::to_string(static_cast<int64_t>(*number)));
      } else {
        return false;
      }
    }
    if (!valid) {
      return false;
    }
  }
  return rec
      .try_log(path, rr::Points3D(std::move(positions))
                         .with_colors(std::move(colors))
                         .with_radii(std::move(radii))
                         .with_class_ids(std::move(classes))
                         .with_labels(std::move(labels))
                         .with_show_labels(false))
      .is_ok();
}

bool write_rerun_native(rr::RecordingStream& rec, const std::string& path, const std::string& ser, const Bytes& raw,
                        std::string_view timeline) {
  using Parser = zerocopy::MessageParser;
  const auto type = Parser::detect_type(ser);
  if (type == Parser::kCameraFrame) {
    return camera(rec, path, raw, timeline);
  }
  Parser parser;
  if (!parser.parse(ser, raw)) {
    return false;
  }
  const FieldReader f(MessageView(parser), nullptr);
  if (!timeline.empty() && f.integer("header.time_meas") > 0) {
    rec.set_time_timestamp_nanos_since_epoch(
        timeline, static_cast<int64_t>(std::min<uint64_t>(f.integer("header.time_meas"), INT64_MAX)));
  }
  if (type == Parser::kPointCloud) {
    return points(rec, path, parser);
  }
  if (type == Parser::kRawData) {
    auto data = f.bytes("data");
    return !data.empty() &&
           rec.try_log(path, rr::Asset3D::from_file_contents(rr::Collection<uint8_t>::borrow(data.data(), data.size()),
                                                             std::nullopt))
               .is_ok();
  }
  if (type == Parser::kTensor) {
    const auto source = f.view("shape");
    const auto strides = f.view("strides");
    std::vector<uint64_t> shape(source.size());
    std::vector<std::string> names(source.size());
    const auto layout = f.text("layout");
    size_t elements = 1;
    for (size_t i = source.size(); i-- > 0;) {
      shape[i] = field_unsigned(source.at(i).value());
      if (field_unsigned(strides.at(i).value()) != elements || shape[i] == 0U || shape[i] > SIZE_MAX ||
          !product(elements, static_cast<size_t>(shape[i]), elements)) {
        return false;
      }
      names[i] = i < layout.size() ? layout.substr(i, 1) : "dim" + std::to_string(i);
    }
    if (elements != f.integer("num_elements")) {
      return false;
    }
    const auto data = f.bytes("data");
    return tensor(rec, path, data, shape, names, f.integer("dtype"));
  }
  if (type == Parser::kAudioFrame) {
    using Audio = zerocopy::AudioFrame;
    uint64_t dtype;
    switch (f.integer("format")) {
      case Audio::kFormatPcmU8:
        dtype = zerocopy::Tensor::kUint8;
        break;
      case Audio::kFormatPcmS16:
        dtype = zerocopy::Tensor::kInt16;
        break;
      case Audio::kFormatPcmS32:
        dtype = zerocopy::Tensor::kInt32;
        break;
      case Audio::kFormatPcmF32:
        dtype = zerocopy::Tensor::kFloat32;
        break;
      default:
        return false;
    }
    const auto data = f.bytes("data");
    const auto channels = f.integer("num_channels");
    const auto samples = f.integer("num_samples");
    if (channels == 0 || samples > SIZE_MAX / channels || parser.collection_size("data") != channels * samples) {
      return false;
    }
    if (f.integer("layout") == Audio::kLayoutInterleaved) {
      return tensor(rec, path, data, {samples, channels}, {"sample", "channel"}, dtype);
    }
    if (f.integer("layout") == Audio::kLayoutPlanar) {
      return tensor(rec, path, data, {channels, samples}, {"channel", "sample"}, dtype);
    }
    return false;
  }
  if (type == Parser::kOccupancyGrid) {
    using Grid = zerocopy::OccupancyGrid;
    const auto width = f.integer("width");
    const auto height = f.integer("height");
    size_t count;
    size_t expected;
    if (!width || !height || width > UINT32_MAX || height > UINT32_MAX || !product(width, height, count) ||
        !product(count, f.integer("cell_size"), expected)) {
      return false;
    }
    auto data = f.bytes("data");
    if (data.size() != expected) {
      return false;
    }
    const auto cell_type = f.integer("cell_type");
    if (cell_type != Grid::kCellUint8) {
      auto gray = Bytes::create(count);
      double minimum = f.number("value_min");
      double maximum = f.number("value_max");
      if (!std::isfinite(minimum) || !std::isfinite(maximum) || maximum <= minimum) {
        minimum = 0;
        maximum = 1;
      }
      for (size_t i = 0; i < count; ++i) {
        double value;
        if (!parser.numeric("data", i, "value", value) || !std::isfinite(value)) {
          return false;
        }
        if (cell_type == Grid::kCellInt8) {
          gray.data()[i] = value < 0 ? 127 : 255 - std::min(static_cast<int>(value), 100) * 255 / 100;
        } else if (cell_type == Grid::kCellUint16) {
          gray.data()[i] = static_cast<uint16_t>(value) >> 8;
        } else if (cell_type == Grid::kCellFloat32) {
          gray.data()[i] = static_cast<uint8_t>(255 * std::clamp((value - minimum) / (maximum - minimum), 0.0, 1.0));
        } else {
          return false;
        }
      }
      data = std::move(gray);
    }
    return rec
        .try_log(path, rr::Image(rr::Collection<uint8_t>::borrow(data.data(), data.size()),
                                 {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}, re::ColorModel::L))
        .is_ok();
  }
  if (type == Parser::kObjectArray) {
    const auto objects = f.view("objects");
    std::vector<rr::Position3D> centers;
    std::vector<rr::HalfSize3D> sizes;
    std::vector<rr::components::RotationQuat> rotations;
    std::vector<rr::Color> colors;
    std::vector<rr::components::ClassId> classes;
    std::vector<rr::Text> labels;
    centers.reserve(objects.size());
    sizes.reserve(objects.size());
    rotations.reserve(objects.size());
    colors.reserve(objects.size());
    classes.reserve(objects.size());
    labels.reserve(objects.size());
    const rr::Color palette[] = {rr::Color(160, 160, 160), rr::Color(80, 200, 120), rr::Color(255, 96, 96),
                                 rr::Color(255, 196, 64), rr::Color(96, 160, 220)};
    for (size_t i = 0; i < objects.size(); ++i) {
      const FieldReader item(objects.at(i), nullptr);
      centers.emplace_back(static_cast<float>(item.number("position_x")), static_cast<float>(item.number("position_y")),
                           static_cast<float>(item.number("position_z")));
      sizes.emplace_back(static_cast<float>(item.number("size_x") / 2), static_cast<float>(item.number("size_y") / 2),
                         static_cast<float>(item.number("size_z") / 2));
      const auto half_yaw = static_cast<float>(item.number("yaw") / 2);
      rotations.emplace_back(re::Quaternion::from_xyzw(0, 0, std::sin(half_yaw), std::cos(half_yaw)));
      const auto motion = item.integer("motion_state");
      colors.push_back(palette[motion < std::size(palette) ? motion : 0]);
      classes.emplace_back(static_cast<uint16_t>(std::min<uint64_t>(item.integer("class_id"), UINT16_MAX)));
      auto label = item.text("label");
      if (item.integer("track_id")) {
        label += "#" + std::to_string(item.integer("track_id"));
      }
      labels.emplace_back(std::move(label));
    }
    return rec
        .try_log(path, rr::Boxes3D::from_centers_and_half_sizes(std::move(centers), std::move(sizes))
                           .with_quaternions(std::move(rotations))
                           .with_colors(std::move(colors))
                           .with_class_ids(std::move(classes))
                           .with_labels(std::move(labels))
                           .with_show_labels(false))
        .is_ok();
  }
  return false;
}

}  // namespace webviz
}  // namespace vlink
