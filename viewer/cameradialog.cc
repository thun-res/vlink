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

// NOLINTBEGIN

#include "./cameradialog.h"

#include <vlink/base/helpers.h>
#include <vlink/external/proxy_api.h>
#include <vlink/zerocopy/camera_frame.h>

#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QHideEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QSettings>
#include <QShowEvent>
#include <QStandardPaths>
#include <QTimeZone>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "./mainwindow.h"
#include "./perceptiondialog.h"
#include "./point3ddialog.h"
#include "./settingsdialog.h"
#include "./ui_cameradialog.h"
#include "./ui_mainwindow.h"

#ifdef _WIN32
#ifdef GetMessage
#undef GetMessage
#endif
#endif

#ifdef _WIN32
#pragma warning(push)
#pragma warning(disable : 4127)
#endif

#include <Eigen/Dense>

#ifdef _WIN32
#pragma warning(pop)
#endif

#define USE_USER_CONDITION 1

[[maybe_unused]] static uint32_t get_point3d_color(double value, double min_value, double max_value, bool inversion) {
  double normalized = (value - min_value) / (max_value - min_value);

  normalized = std::clamp(normalized, 0.0, 1.0);

  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;

  if (inversion) {
    if (normalized < 0.25) {
      double t = normalized / 0.25;
      r = 255;
      g = static_cast<uint8_t>(t * 165);
      b = 0;
    } else if (normalized < 0.5) {
      double t = (normalized - 0.25) / 0.25;
      r = 255;
      g = 165 + static_cast<uint8_t>(t * (255 - 165));
      b = 0;
    } else if (normalized < 0.75) {
      double t = (normalized - 0.5) / 0.25;
      r = static_cast<uint8_t>((1.0 - t) * 255);
      g = 255;
      b = 0;
    } else {
      double t = (normalized - 0.75) / 0.25;
      r = 0;
      g = 255;
      b = static_cast<uint8_t>(t * 255);
    }
  } else {
    if (normalized < 0.25) {
      double t = normalized / 0.25;
      r = 0;
      g = static_cast<uint8_t>(t * 255);
      b = 255;
    } else if (normalized < 0.5) {
      double t = (normalized - 0.25) / 0.25;
      r = 0;
      g = 255;
      b = 255 - static_cast<uint8_t>(t * 255);
    } else if (normalized < 0.75) {
      double t = (normalized - 0.5) / 0.25;
      r = static_cast<uint8_t>(t * 255);
      g = 255;
      b = 0;
    } else {
      double t = (normalized - 0.75) / 0.25;
      r = 255;
      g = 255 - static_cast<uint8_t>(t * 255);
      b = 0;
    }
  }

  return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

static QSize get_label_pixmap_size(const QLabel* label) {
  if (!label) {
    return QSize();
  }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  return label->pixmap().size();
#elif QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
  return label->pixmap(Qt::ReturnByValue).size();
#else
  const QPixmap* pixmap = label->pixmap();
  return pixmap ? pixmap->size() : QSize();
#endif
}

static bool has_label_pixmap(const QLabel* label) {
  if (!label) {
    return false;
  }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  return !label->pixmap().isNull();
#elif QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
  return !label->pixmap(Qt::ReturnByValue).isNull();
#else
  return label->pixmap() != nullptr;
#endif
}

static bool needs_raw_size(FFmpegDecoder::InType type) {
  switch (type) {
    case FFmpegDecoder::InType::kYUV420:
    case FFmpegDecoder::InType::kYUV422:
    case FFmpegDecoder::InType::kYUV444:
    case FFmpegDecoder::InType::kNV12:
    case FFmpegDecoder::InType::kYUYV:
    case FFmpegDecoder::InType::kYVYU:
    case FFmpegDecoder::InType::kUYVY:
    case FFmpegDecoder::InType::kNV21:
    case FFmpegDecoder::InType::kBGR888:
    case FFmpegDecoder::InType::kRGB888:
      return true;
    default:
      return false;
  }
}

static bool to_qbytes(const vlink::Bytes& bytes, QByteArray& output) {
  if (!bytes.data() || bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return false;
  }

  output = QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()));
  return true;
}

static bool get_camera_frame_image_size(const vlink::zerocopy::CameraFrame& frame, int& width, int& height) {
  if (frame.width() == 0 || frame.height() == 0 ||
      frame.width() > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
      frame.height() > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
    return false;
  }

  width = static_cast<int>(frame.width());
  height = static_cast<int>(frame.height());

  return true;
}

static bool get_ffmpeg_raw_payload_size(FFmpegDecoder::InType type, int width, int height, size_t& expected) {
  expected = 0;

  if (!needs_raw_size(type)) {
    return true;
  }

  if VUNLIKELY (width <= 0 || height <= 0) {
    return false;
  }

  const size_t width_size = static_cast<size_t>(width);
  const size_t height_size = static_cast<size_t>(height);

  if VUNLIKELY (width_size > std::numeric_limits<size_t>::max() / height_size) {
    return false;
  }

  const size_t pixels = width_size * height_size;

  size_t bytes_per_pixel = 0;

  switch (type) {
    case FFmpegDecoder::InType::kYUV420:
    case FFmpegDecoder::InType::kNV12:
    case FFmpegDecoder::InType::kNV21:
      if VUNLIKELY ((width % 2) != 0 || (height % 2) != 0) {
        return false;
      }

      if VUNLIKELY (pixels > std::numeric_limits<size_t>::max() - pixels / 2U) {
        return false;
      }

      expected = pixels + pixels / 2U;
      break;
    case FFmpegDecoder::InType::kYUV422:
    case FFmpegDecoder::InType::kYUYV:
    case FFmpegDecoder::InType::kYVYU:
    case FFmpegDecoder::InType::kUYVY:
      if VUNLIKELY ((width % 2) != 0) {
        return false;
      }

      bytes_per_pixel = 2U;
      break;
    case FFmpegDecoder::InType::kYUV444:
    case FFmpegDecoder::InType::kBGR888:
    case FFmpegDecoder::InType::kRGB888:
      bytes_per_pixel = 3U;
      break;
    default:
      return true;
  }

  if (bytes_per_pixel != 0U) {
    if VUNLIKELY (pixels > std::numeric_limits<size_t>::max() / bytes_per_pixel) {
      return false;
    }

    expected = pixels * bytes_per_pixel;
  }

  return true;
}

static bool has_camera_frame_payload(const vlink::zerocopy::CameraFrame& frame, size_t bytes_per_pixel) {
  int width = 0;
  int height = 0;

  if VUNLIKELY (!get_camera_frame_image_size(frame, width, height) || bytes_per_pixel == 0) {
    return false;
  }

  const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);

  if VUNLIKELY (pixels / static_cast<size_t>(width) != static_cast<size_t>(height)) {
    return false;
  }

  if VUNLIKELY (pixels > std::numeric_limits<size_t>::max() / bytes_per_pixel) {
    return false;
  }

  const size_t expected = pixels * bytes_per_pixel;

  return frame.data() && frame.size() >= expected;
}

static bool has_ffmpeg_payload(const vlink::Bytes& bytes, FFmpegDecoder::InType type, int width, int height) {
  if VUNLIKELY (!bytes.data() || bytes.size() == 0) {
    return false;
  }

  size_t expected = 0;

  if VUNLIKELY (!get_ffmpeg_raw_payload_size(type, width, height, expected)) {
    return false;
  }

  return bytes.size() >= expected;
}

static bool has_ffmpeg_payload(const vlink::zerocopy::CameraFrame& frame, FFmpegDecoder::InType type) {
  int width = 0;
  int height = 0;

  if (needs_raw_size(type) && !get_camera_frame_image_size(frame, width, height)) {
    return false;
  }

  const vlink::Bytes bytes = vlink::Bytes::shallow_copy(frame.data(), frame.size());

  return has_ffmpeg_payload(bytes, type, width, height);
}

static bool make_qimage_copy(const uint8_t* data, int width, int height, size_t bytes_per_line, QImage::Format format,
                             QImage& image) {
  if VUNLIKELY (!data || width <= 0 || height <= 0 || bytes_per_line == 0 ||
                bytes_per_line > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return false;
  }

  QImage view(data, width, height, static_cast<int>(bytes_per_line), format);

  if VUNLIKELY (view.isNull()) {
    return false;
  }

  image = view.copy();
  return !image.isNull();
}

static uint8_t clamp_u8(int value) { return static_cast<uint8_t>(std::clamp(value, 0, 255)); }

static void yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v, uint8_t* rgb) {
  const int c = static_cast<int>(y) - 16;
  const int d = static_cast<int>(u) - 128;
  const int e = static_cast<int>(v) - 128;

  rgb[0] = clamp_u8((298 * c + 409 * e + 128) >> 8);
  rgb[1] = clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8);
  rgb[2] = clamp_u8((298 * c + 516 * d + 128) >> 8);
}

static bool make_packed_yuv422_qimage(const vlink::zerocopy::CameraFrame& frame, QImage& image) {
  int width = 0;
  int height = 0;

  if VUNLIKELY (!get_camera_frame_image_size(frame, width, height) || (width % 2) != 0 ||
                !has_camera_frame_payload(frame, 2)) {
    return false;
  }

  image = QImage(width, height, QImage::Format_RGB888);

  if VUNLIKELY (image.isNull()) {
    return false;
  }

  const auto fmt = frame.format();
  const uint8_t* src = frame.data();

  for (int y = 0; y < height; ++y) {
    auto* dst = image.scanLine(y);
    const uint8_t* row = src + static_cast<size_t>(y) * static_cast<size_t>(width) * 2U;

    for (int x = 0; x < width; x += 2) {
      const uint8_t* p = row + static_cast<size_t>(x) * 2U;
      uint8_t y0 = 0;
      uint8_t y1 = 0;
      uint8_t u = 0;
      uint8_t v = 0;

      if (fmt == vlink::zerocopy::CameraFrame::kFormatYuyv) {
        y0 = p[0];
        u = p[1];
        y1 = p[2];
        v = p[3];
      } else if (fmt == vlink::zerocopy::CameraFrame::kFormatYvyu) {
        y0 = p[0];
        v = p[1];
        y1 = p[2];
        u = p[3];
      } else if (fmt == vlink::zerocopy::CameraFrame::kFormatUyvy) {
        u = p[0];
        y0 = p[1];
        v = p[2];
        y1 = p[3];
      } else {
        v = p[0];
        y0 = p[1];
        u = p[2];
        y1 = p[3];
      }

      yuv_to_rgb(y0, u, v, dst + static_cast<size_t>(x) * 3U);
      if (x + 1 < width) {
        yuv_to_rgb(y1, u, v, dst + static_cast<size_t>(x + 1) * 3U);
      }
    }
  }

  return true;
}

template <typename T>
static bool make_normalized_gray_qimage(const vlink::zerocopy::CameraFrame& frame, QImage& image, int channels = 1) {
  if VUNLIKELY (channels <= 0 || !has_camera_frame_payload(frame, sizeof(T) * static_cast<size_t>(channels))) {
    return false;
  }

  int width = 0;
  int height = 0;
  get_camera_frame_image_size(frame, width, height);

  const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
  const size_t pixel_stride = static_cast<size_t>(channels) * sizeof(T);
  const auto* data = frame.data();

  double min_value = std::numeric_limits<double>::infinity();
  double max_value = -std::numeric_limits<double>::infinity();

  for (size_t i = 0; i < pixels; ++i) {
    T sample{};

    std::memcpy(&sample, data + i * pixel_stride, sizeof(T));

    const double value = static_cast<double>(sample);

    if VUNLIKELY (!std::isfinite(value)) {
      continue;
    }

    min_value = std::min(min_value, value);
    max_value = std::max(max_value, value);
  }

  if VUNLIKELY (!std::isfinite(min_value) || !std::isfinite(max_value)) {
    return false;
  }

  image = QImage(width, height, QImage::Format_Grayscale8);

  if VUNLIKELY (image.isNull()) {
    return false;
  }

  const double range = max_value - min_value;

  for (int y = 0; y < height; ++y) {
    auto* dst = image.scanLine(y);

    for (int x = 0; x < width; ++x) {
      T sample{};
      const size_t index = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);

      std::memcpy(&sample, data + index * pixel_stride, sizeof(T));

      const double value = static_cast<double>(sample);

      if VUNLIKELY (!std::isfinite(value)) {
        dst[x] = 0;
      } else if (range <= 0.0) {
        dst[x] = 128;
      } else {
        dst[x] = clamp_u8(static_cast<int>((value - min_value) * 255.0 / range));
      }
    }
  }

  return true;
}

static bool make_rgb_planar_qimage(const vlink::zerocopy::CameraFrame& frame, QImage& image) {
  if VUNLIKELY (!has_camera_frame_payload(frame, 3)) {
    return false;
  }

  int width = 0;
  int height = 0;
  get_camera_frame_image_size(frame, width, height);

  const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
  const auto* r = frame.data();
  const auto* g = r + pixels;
  const auto* b = g + pixels;

  image = QImage(width, height, QImage::Format_RGB888);

  if VUNLIKELY (image.isNull()) {
    return false;
  }

  for (int y = 0; y < height; ++y) {
    auto* dst = image.scanLine(y);
    for (int x = 0; x < width; ++x) {
      const size_t index = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
      dst[static_cast<size_t>(x) * 3U + 0U] = r[index];
      dst[static_cast<size_t>(x) * 3U + 1U] = g[index];
      dst[static_cast<size_t>(x) * 3U + 2U] = b[index];
    }
  }

  return true;
}

static bool make_bgr888_qimage(const vlink::zerocopy::CameraFrame& frame, QImage& image) {
  if VUNLIKELY (!has_camera_frame_payload(frame, 3)) {
    return false;
  }

  int width = 0;
  int height = 0;
  get_camera_frame_image_size(frame, width, height);

  image = QImage(width, height, QImage::Format_RGB888);

  if VUNLIKELY (image.isNull()) {
    return false;
  }

  const auto* src = frame.data();
  for (int y = 0; y < height; ++y) {
    auto* dst = image.scanLine(y);
    for (int x = 0; x < width; ++x) {
      const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 3U;
      dst[static_cast<size_t>(x) * 3U + 0U] = src[index + 2U];
      dst[static_cast<size_t>(x) * 3U + 1U] = src[index + 1U];
      dst[static_cast<size_t>(x) * 3U + 2U] = src[index + 0U];
    }
  }

  return true;
}

static bool make_bgra8888_qimage(const vlink::zerocopy::CameraFrame& frame, QImage& image) {
  if VUNLIKELY (!has_camera_frame_payload(frame, 4)) {
    return false;
  }

  int width = 0;
  int height = 0;
  get_camera_frame_image_size(frame, width, height);

  image = QImage(width, height, QImage::Format_RGBA8888);

  if VUNLIKELY (image.isNull()) {
    return false;
  }

  const auto* src = frame.data();
  for (int y = 0; y < height; ++y) {
    auto* dst = image.scanLine(y);
    for (int x = 0; x < width; ++x) {
      const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4U;
      dst[static_cast<size_t>(x) * 4U + 0U] = src[index + 2U];
      dst[static_cast<size_t>(x) * 4U + 1U] = src[index + 1U];
      dst[static_cast<size_t>(x) * 4U + 2U] = src[index + 0U];
      dst[static_cast<size_t>(x) * 4U + 3U] = src[index + 3U];
    }
  }

  return true;
}

static int bayer_color(const vlink::zerocopy::CameraFrame::Format format, int x, int y) {
  const bool odd_x = (x & 1) != 0;
  const bool odd_y = (y & 1) != 0;

  switch (format) {
    case vlink::zerocopy::CameraFrame::kFormatBayerRggb8:
    case vlink::zerocopy::CameraFrame::kFormatBayerRggb16:
      return !odd_y ? (!odd_x ? 0 : 1) : (!odd_x ? 1 : 2);
    case vlink::zerocopy::CameraFrame::kFormatBayerBggr8:
    case vlink::zerocopy::CameraFrame::kFormatBayerBggr16:
      return !odd_y ? (!odd_x ? 2 : 1) : (!odd_x ? 1 : 0);
    case vlink::zerocopy::CameraFrame::kFormatBayerGbrg8:
    case vlink::zerocopy::CameraFrame::kFormatBayerGbrg16:
      return !odd_y ? (!odd_x ? 1 : 2) : (!odd_x ? 0 : 1);
    default:
      return !odd_y ? (!odd_x ? 1 : 0) : (!odd_x ? 2 : 1);
  }
}

static uint8_t bayer_sample(const uint8_t* data, size_t index, bool is_16bit) {
  if (!is_16bit) {
    return data[index];
  }

  uint16_t value = 0;
  std::memcpy(&value, data + index * sizeof(uint16_t), sizeof(uint16_t));
  return static_cast<uint8_t>(value >> 8U);
}

static bool make_bayer_qimage(const vlink::zerocopy::CameraFrame& frame, QImage& image) {
  const bool is_16bit = frame.format() == vlink::zerocopy::CameraFrame::kFormatBayerRggb16 ||
                        frame.format() == vlink::zerocopy::CameraFrame::kFormatBayerBggr16 ||
                        frame.format() == vlink::zerocopy::CameraFrame::kFormatBayerGbrg16 ||
                        frame.format() == vlink::zerocopy::CameraFrame::kFormatBayerGrbg16;

  if VUNLIKELY (!has_camera_frame_payload(frame, is_16bit ? sizeof(uint16_t) : sizeof(uint8_t))) {
    return false;
  }

  int width = 0;
  int height = 0;
  get_camera_frame_image_size(frame, width, height);

  image = QImage(width, height, QImage::Format_RGB888);

  if VUNLIKELY (image.isNull()) {
    return false;
  }

  const auto* data = frame.data();
  for (int y = 0; y < height; ++y) {
    auto* dst = image.scanLine(y);
    for (int x = 0; x < width; ++x) {
      int sum[3] = {0, 0, 0};
      int count[3] = {0, 0, 0};

      for (int dy = -1; dy <= 1; ++dy) {
        const int yy = y + dy;
        if (yy < 0 || yy >= height) {
          continue;
        }

        for (int dx = -1; dx <= 1; ++dx) {
          const int xx = x + dx;
          if (xx < 0 || xx >= width) {
            continue;
          }

          const int color = bayer_color(frame.format(), xx, yy);
          const size_t index = static_cast<size_t>(yy) * static_cast<size_t>(width) + static_cast<size_t>(xx);
          sum[color] += bayer_sample(data, index, is_16bit);
          ++count[color];
        }
      }

      const size_t out = static_cast<size_t>(x) * 3U;
      dst[out + 0U] = count[0] > 0 ? static_cast<uint8_t>(sum[0] / count[0]) : 0;
      dst[out + 1U] = count[1] > 0 ? static_cast<uint8_t>(sum[1] / count[1]) : 0;
      dst[out + 2U] = count[2] > 0 ? static_cast<uint8_t>(sum[2] / count[2]) : 0;
    }
  }

  return true;
}

static bool make_camera_frame_qimage(const vlink::zerocopy::CameraFrame& frame, QImage& image) {
  int width = 0;
  int height = 0;

  if VUNLIKELY (!get_camera_frame_image_size(frame, width, height)) {
    return false;
  }

  switch (frame.format()) {
    case vlink::zerocopy::CameraFrame::kFormatRgb888Packed:
      return has_camera_frame_payload(frame, 3) &&
             make_qimage_copy(frame.data(), width, height, static_cast<size_t>(width) * 3U, QImage::Format_RGB888,
                              image);
    case vlink::zerocopy::CameraFrame::kFormatBgr888Packed:
      return make_bgr888_qimage(frame, image);
    case vlink::zerocopy::CameraFrame::kFormatRgb888Planar:
      return make_rgb_planar_qimage(frame, image);
    case vlink::zerocopy::CameraFrame::kFormatRgba8888Packed:
      return has_camera_frame_payload(frame, 4) &&
             make_qimage_copy(frame.data(), width, height, static_cast<size_t>(width) * 4U, QImage::Format_RGBA8888,
                              image);
    case vlink::zerocopy::CameraFrame::kFormatBgra8888Packed:
      return make_bgra8888_qimage(frame, image);
    case vlink::zerocopy::CameraFrame::kFormatMono8:
    case vlink::zerocopy::CameraFrame::kFormatUint8C1:
      return has_camera_frame_payload(frame, 1) &&
             make_qimage_copy(frame.data(), width, height, static_cast<size_t>(width), QImage::Format_Grayscale8,
                              image);
    case vlink::zerocopy::CameraFrame::kFormatUint8C2:
      return make_normalized_gray_qimage<uint8_t>(frame, image, 2);
    case vlink::zerocopy::CameraFrame::kFormatUint8C3:
      return make_normalized_gray_qimage<uint8_t>(frame, image, 3);
    case vlink::zerocopy::CameraFrame::kFormatUint8C4:
      return make_normalized_gray_qimage<uint8_t>(frame, image, 4);
    case vlink::zerocopy::CameraFrame::kFormatMono16:
    case vlink::zerocopy::CameraFrame::kFormatUint16C1:
      return make_normalized_gray_qimage<uint16_t>(frame, image);
    case vlink::zerocopy::CameraFrame::kFormatUint16C2:
      return make_normalized_gray_qimage<uint16_t>(frame, image, 2);
    case vlink::zerocopy::CameraFrame::kFormatUint16C3:
      return make_normalized_gray_qimage<uint16_t>(frame, image, 3);
    case vlink::zerocopy::CameraFrame::kFormatUint16C4:
      return make_normalized_gray_qimage<uint16_t>(frame, image, 4);
    case vlink::zerocopy::CameraFrame::kFormatInt8C1:
      return make_normalized_gray_qimage<int8_t>(frame, image);
    case vlink::zerocopy::CameraFrame::kFormatInt8C2:
      return make_normalized_gray_qimage<int8_t>(frame, image, 2);
    case vlink::zerocopy::CameraFrame::kFormatInt8C3:
      return make_normalized_gray_qimage<int8_t>(frame, image, 3);
    case vlink::zerocopy::CameraFrame::kFormatInt8C4:
      return make_normalized_gray_qimage<int8_t>(frame, image, 4);
    case vlink::zerocopy::CameraFrame::kFormatInt16C1:
      return make_normalized_gray_qimage<int16_t>(frame, image);
    case vlink::zerocopy::CameraFrame::kFormatInt16C2:
      return make_normalized_gray_qimage<int16_t>(frame, image, 2);
    case vlink::zerocopy::CameraFrame::kFormatInt16C3:
      return make_normalized_gray_qimage<int16_t>(frame, image, 3);
    case vlink::zerocopy::CameraFrame::kFormatInt16C4:
      return make_normalized_gray_qimage<int16_t>(frame, image, 4);
    case vlink::zerocopy::CameraFrame::kFormatInt32C1:
      return make_normalized_gray_qimage<int32_t>(frame, image);
    case vlink::zerocopy::CameraFrame::kFormatInt32C2:
      return make_normalized_gray_qimage<int32_t>(frame, image, 2);
    case vlink::zerocopy::CameraFrame::kFormatInt32C3:
      return make_normalized_gray_qimage<int32_t>(frame, image, 3);
    case vlink::zerocopy::CameraFrame::kFormatInt32C4:
      return make_normalized_gray_qimage<int32_t>(frame, image, 4);
    case vlink::zerocopy::CameraFrame::kFormatFloat32C1:
      return make_normalized_gray_qimage<float>(frame, image);
    case vlink::zerocopy::CameraFrame::kFormatFloat32C2:
      return make_normalized_gray_qimage<float>(frame, image, 2);
    case vlink::zerocopy::CameraFrame::kFormatFloat32C3:
      return make_normalized_gray_qimage<float>(frame, image, 3);
    case vlink::zerocopy::CameraFrame::kFormatFloat32C4:
      return make_normalized_gray_qimage<float>(frame, image, 4);
    case vlink::zerocopy::CameraFrame::kFormatFloat64C1:
      return make_normalized_gray_qimage<double>(frame, image);
    case vlink::zerocopy::CameraFrame::kFormatFloat64C2:
      return make_normalized_gray_qimage<double>(frame, image, 2);
    case vlink::zerocopy::CameraFrame::kFormatFloat64C3:
      return make_normalized_gray_qimage<double>(frame, image, 3);
    case vlink::zerocopy::CameraFrame::kFormatFloat64C4:
      return make_normalized_gray_qimage<double>(frame, image, 4);
    case vlink::zerocopy::CameraFrame::kFormatYuyv:
    case vlink::zerocopy::CameraFrame::kFormatYvyu:
    case vlink::zerocopy::CameraFrame::kFormatUyvy:
    case vlink::zerocopy::CameraFrame::kFormatVyuy:
      return make_packed_yuv422_qimage(frame, image);
    case vlink::zerocopy::CameraFrame::kFormatBayerRggb8:
    case vlink::zerocopy::CameraFrame::kFormatBayerBggr8:
    case vlink::zerocopy::CameraFrame::kFormatBayerGbrg8:
    case vlink::zerocopy::CameraFrame::kFormatBayerGrbg8:
    case vlink::zerocopy::CameraFrame::kFormatBayerRggb16:
    case vlink::zerocopy::CameraFrame::kFormatBayerBggr16:
    case vlink::zerocopy::CameraFrame::kFormatBayerGbrg16:
    case vlink::zerocopy::CameraFrame::kFormatBayerGrbg16:
      return make_bayer_qimage(frame, image);
    default:
      return false;
  }
}

static bool make_encoded_camera_frame_qimage(const vlink::zerocopy::CameraFrame& frame, QImage& image) {
  if VUNLIKELY (!frame.data() || frame.size() == 0 ||
                frame.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return false;
  }

  const char* format = nullptr;
  switch (frame.format()) {
    case vlink::zerocopy::CameraFrame::kFormatJpeg:
    case vlink::zerocopy::CameraFrame::kFormatMjpeg:
      format = "JPG";
      break;
    case vlink::zerocopy::CameraFrame::kFormatPng:
      format = "PNG";
      break;
    case vlink::zerocopy::CameraFrame::kFormatWebp:
      format = "WEBP";
      break;
    default:
      return false;
  }

  image = QImage::fromData(frame.data(), static_cast<int>(frame.size()), format);
  if (image.isNull()) {
    image = QImage::fromData(frame.data(), static_cast<int>(frame.size()));
  }

  return !image.isNull();
}

static FFmpegDecoder::InType camera_frame_decoder_type(vlink::zerocopy::CameraFrame::Format format) {
  switch (format) {
    case vlink::zerocopy::CameraFrame::kFormatJpeg:
    case vlink::zerocopy::CameraFrame::kFormatMjpeg:
      return FFmpegDecoder::InType::kJPG;
    case vlink::zerocopy::CameraFrame::kFormatH264:
      return FFmpegDecoder::InType::kH264;
    case vlink::zerocopy::CameraFrame::kFormatH265:
      return FFmpegDecoder::InType::kH265;
    case vlink::zerocopy::CameraFrame::kFormatAv1:
      return FFmpegDecoder::InType::kAV1;
    case vlink::zerocopy::CameraFrame::kFormatH266:
      return FFmpegDecoder::InType::kH266;
    case vlink::zerocopy::CameraFrame::kFormatPng:
      return FFmpegDecoder::InType::kPNG;
    case vlink::zerocopy::CameraFrame::kFormatWebp:
      return FFmpegDecoder::InType::kWEBP;
    case vlink::zerocopy::CameraFrame::kFormatYuv420:
      return FFmpegDecoder::InType::kYUV420;
    case vlink::zerocopy::CameraFrame::kFormatYuv422:
      return FFmpegDecoder::InType::kYUV422;
    case vlink::zerocopy::CameraFrame::kFormatYuv444:
      return FFmpegDecoder::InType::kYUV444;
    case vlink::zerocopy::CameraFrame::kFormatNv12:
      return FFmpegDecoder::InType::kNV12;
    case vlink::zerocopy::CameraFrame::kFormatNv21:
      return FFmpegDecoder::InType::kNV21;
    default:
      return FFmpegDecoder::InType::kUnknown;
  }
}

static bool resolve_flatbuffers_field_path(const FlatbuffersObjectView& root_view, const reflection::Schema& schema,
                                           const std::string& path, FlatbuffersObjectView& parent_view,
                                           const reflection::Field*& field_out) {
  parent_view = {};
  field_out = nullptr;

  if (!root_view.valid() || path.empty()) {
    return false;
  }

  auto current_view = root_view;
  size_t begin = 0;

  while (begin < path.size()) {
    const auto pos = path.find('.', begin);
    const auto token = path.substr(begin, pos == std::string::npos ? std::string::npos : pos - begin);
    const auto* field = find_field(*current_view.object, token);

    if (!field) {
      return false;
    }

    if (pos == std::string::npos) {
      parent_view = current_view;
      field_out = field;
      return true;
    }

    FlatbuffersObjectView child_view;

    if (!get_child_view(current_view, *field, schema, child_view)) {
      return false;
    }

    current_view = child_view;
    begin = pos + 1;
  }

  return false;
}

class CameraLabel : public QLabel {
 public:
  using PathCallback = vlink::MoveFunction<void(const std::string& path, bool whole_label)>;
  using SizeCallback = vlink::MoveFunction<void(int w, int h)>;

  explicit CameraLabel(const QString& title, CameraDialog* camera_dialog, QWidget* parent = nullptr)
      : QLabel(parent), title_(title), camera_dialog_(camera_dialog) {
    setStyleSheet("background-color: rgb(25, 25, 25); color: rgb(225, 225, 225);");
    setAlignment(Qt::AlignCenter);

    QFont font = this->font();
    font.setBold(true);
    font.setPixelSize(30);

    setFont(font);
  }

  ~CameraLabel() {}

  void set_show_info(bool show_info) { show_info_ = show_info; }

  void set_camera_size(QSize size) { size_ = size; }

  void set_timestamp(uint64_t timestamp) { timestamp_ = timestamp; }

  void set_error(bool error) { has_error_ = error; }

  void set_update_points(bool update_points) {
    update_points_ = update_points;
    update();
  };

  void update_info(float fps, float time) {
    fps_ = fps;
    time_ = time;
    update();
  }

  std::string get_title() const { return title_.toStdString(); }

  void register_path_callback(PathCallback&& path_callback) { path_callback_ = std::move(path_callback); }

  void register_size_callback(SizeCallback&& size_callback) { size_callback_ = std::move(size_callback); }

 protected:
  void paintEvent(QPaintEvent* event) override {
    QLabel::paintEvent(event);

    QPainter painter(this);

    QFont font = painter.font();

    if (camera_dialog_->proj_params_.is_valid) {
      if (update_points_) {
        if (points_pixmap_.isNull() || points_pixmap_.width() != width() || points_pixmap_.height() != height()) {
          points_pixmap_ = QPixmap(width(), height());
        }

        points_pixmap_.fill(Qt::transparent);

        QPainter points_painter(&points_pixmap_);

        font.setPixelSize(12);
        points_painter.setFont(font);

        QFontMetrics top_metrics(font);
        QRect proj_rect = top_metrics.boundingRect(0, 0, 0, 0, Qt::AlignLeft | Qt::AlignVCenter, "Projection");

        QRect bottom_rect(width() - proj_rect.width() - 10, 0, proj_rect.width() + 10, proj_rect.height() + 10);
        points_painter.setBrush(QBrush(QColor(255, 255, 0, 100)));
        points_painter.setPen(QColor(0, 0, 0, 0));
        points_painter.drawRect(bottom_rect);

        points_painter.setPen(QColor(255, 255, 255, 200));
        points_painter.drawText(width() - proj_rect.width() - 5, 5, proj_rect.width(), proj_rect.height(),
                                Qt::AlignLeft | Qt::AlignVCenter, "Projection");

        const QSize pixmap_size = get_label_pixmap_size(this);
        if (pixmap_size.width() > 0 && pixmap_size.height() > 0) {
          double aspect_ratio_pixmap = static_cast<double>(pixmap_size.width()) / pixmap_size.height();
          double aspect_ratio_label = static_cast<double>(width()) / height();

          QRect pixmap_rect;

          if (aspect_ratio_pixmap > aspect_ratio_label) {
            int new_width = width();
            int new_height = new_width / aspect_ratio_pixmap;
            pixmap_rect.setRect(0, 0 + (height() - new_height) / 2, new_width, new_height);
          } else {
            int new_height = height();
            int new_width = new_height * aspect_ratio_pixmap;
            pixmap_rect.setRect(0 + (width() - new_width) / 2, 0, new_width, new_height);
          }

          for (const auto& p : std::as_const(camera_dialog_->projection_points_)) {
            if (p.z() < 0) {
              points_painter.setPen(QPen(QBrush(0xFF55FFFF), camera_dialog_->proj_params_.point_size));
            } else {
              points_painter.setPen(
                  QPen(QBrush(get_point3d_color(p.z(), 0, 255 * camera_dialog_->proj_params_.color_percent,
                                                camera_dialog_->proj_params_.inversion)),
                       camera_dialog_->proj_params_.point_size));
            }

            float px = pixmap_rect.width() / camera_dialog_->proj_params_.img_width * p.x() + pixmap_rect.x();

            float py = pixmap_rect.height() / camera_dialog_->proj_params_.img_height * p.y() + pixmap_rect.y();

            points_painter.drawPoint(px, py);
          }
        }

        points_painter.end();

        update_points_ = false;
      }

      if (!points_pixmap_.isNull()) {
        painter.drawPixmap(0, 0, points_pixmap_);
      }
    }

    if (show_info_) {
      font.setPixelSize(12);
      painter.setFont(font);

      QFontMetrics top_metrics(font);
      QRect text_rect = top_metrics.boundingRect(0, 0, 0, 0, Qt::AlignLeft | Qt::AlignVCenter, title_);

      QRect top_rect(0, 0, text_rect.width() + 10, text_rect.height() + 10);
      painter.setBrush(QBrush(QColor(0, 0, 0, 100)));
      painter.setPen(QColor(0, 0, 0, 0));
      painter.drawRect(top_rect);

      painter.setPen(QColor(255, 255, 255, 200));
      painter.drawText(5, 5, text_rect.width(), text_rect.height(), Qt::AlignLeft | Qt::AlignVCenter, title_);

      if (!has_error_) {
        font.setPixelSize(10);
        painter.setFont(font);

        QString decoding_time_str = time_ >= 0 ? (QString::number(time_, 'f', 2) + "ms") : "---";
        QString bottom_text = "SIZE: " + QString::number(size_.width()) + "x" + QString::number(size_.height()) + "  " +
                              "FPS: " + QString::number(fps_, 'f', 2) + "  " + "ADT: " + decoding_time_str;

        if (timestamp_ > 0) {
          bottom_text += "  TS: ";
          bottom_text += QString::fromStdString(vlink::ProxyAPI::get_format_sys_time(timestamp_));
        }

        QFontMetrics bottom_metrics(font);
        QRect bottom_text_rect = bottom_metrics.boundingRect(0, 0, 0, 0, Qt::AlignLeft | Qt::AlignVCenter, bottom_text);

        QRect bottom_rect(0, height() - bottom_text_rect.height() - 10, bottom_text_rect.width() + 10,
                          bottom_text_rect.height() + 10);
        painter.setBrush(QBrush(QColor(0, 0, 0, 100)));
        painter.setPen(QColor(0, 0, 0, 0));
        painter.drawRect(bottom_rect);

        painter.setPen(QColor(50, 255, 50, 200));
        painter.drawText(5, height() - bottom_text_rect.height() - 5, bottom_text_rect.width(),
                         bottom_text_rect.height(), Qt::AlignLeft | Qt::AlignVCenter, bottom_text);
      }
    }
  }

  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() == Qt::RightButton) {
      right_pressed_ = true;
    } else {
      right_pressed_ = false;
    }

    QLabel::mousePressEvent(event);
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    if (event->button() == Qt::RightButton && right_pressed_) {
      QMenu menu(this);

      QAction* save_only_action = menu.addAction(tr("Save only image"));
      QAction* save_whole_action = menu.addAction(tr("Save whole label"));

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
      QAction* selected_action = menu.exec(event->globalPosition().toPoint());
#else
      QAction* selected_action = menu.exec(event->globalPos());
#endif

      if (selected_action == save_only_action || selected_action == save_whole_action) {
        QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                           QSettings::IniFormat);

        settings.beginGroup("CameraDialog");

        QFileDialog dialog(MainWindow::get_instance(), tr("Save image file"),
                           settings.value("image_dir", qApp->applicationDirPath()).toString(),
                           "Image files (*.jpg *.png)");

        settings.endGroup();

#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
        dialog.setOption(QFileDialog::DontUseNativeDialog, true);
#endif

        dialog.setFileMode(QFileDialog::AnyFile);
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setDefaultSuffix("jpg");

        if (dialog.exec() == QDialog::Accepted) {
          QString file_path = dialog.selectedFiles().constFirst();
          if (!file_path.isEmpty()) {
            settings.beginGroup("CameraDialog");
            settings.setValue("image_dir", QFileInfo(file_path).dir().path());
            settings.endGroup();
            settings.sync();

            if (path_callback_) {
              path_callback_(file_path.toStdString(), (selected_action == save_whole_action));
            }
          }
        }
      }
    }

    right_pressed_ = false;

    QLabel::mouseReleaseEvent(event);
  }

  void resizeEvent(QResizeEvent* event) override {
    update_points_ = true;

    if (size_callback_) {
      size_callback_(event->size().width(), event->size().height());
    }

    QLabel::resizeEvent(event);
  }

 private:
  QString title_;
  CameraDialog* camera_dialog_{nullptr};
  bool show_info_{true};
  bool has_error_{false};
  QSize size_{0, 0};
  float fps_{0};
  float time_{0};
  uint64_t timestamp_{0};
  bool right_pressed_{false};
  bool update_points_{false};
  PathCallback path_callback_;
  SizeCallback size_callback_;
  QPixmap points_pixmap_;
};

CameraDialog::CameraDialog(QWidget* parent) : QDialog(parent), ui(new Ui::CameraDialog) {
  window_ = MainWindow::get_instance();

  if (parent) {
    setWindowFlags(Qt::Window | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint | Qt::WindowStaysOnTopHint);
  } else {
    setWindowFlags(Qt::Window | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);
  }

  ui->setupUi(this);

  ui->stackedWidget->setCurrentIndex(0);

  {
    QFont font = ui->textEdit->font();
    font.setFamily("Noto Mono");
    ui->textEdit->setFont(font);
  }

  ui->pushButton_point3d->setEnabled(false);

  ui->label_pause->setVisible(false);

  camera_layout_ = new QGridLayout(ui->widget);

  msg_list_.emplace_back(nullptr, nullptr);

  const auto& selected_items = window_->ui->treeWidget_url->selectedItems();

  if (selected_items.count() > 8) {
    ui->comboBox_quality->setCurrentIndex(3);
    display_quality_ = 0.25;
  } else if (selected_items.count() > 4) {
    ui->comboBox_quality->setCurrentIndex(2);
    display_quality_ = 0.5;
  } else if (selected_items.count() > 1) {
    ui->comboBox_quality->setCurrentIndex(1);
    display_quality_ = 0.75;
  } else {
    ui->comboBox_quality->setCurrentIndex(0);
    display_quality_ = 1.0;
  }

  multi_mode_ = selected_items.count() > 1;

  if (multi_mode_) {
    ui->groupBox_info->hide();
  }

  ui->checkBox_cache->setEnabled(FFmpegDecoder::is_valid());
  ui->checkBox_hard->setEnabled(FFmpegDecoder::is_valid());
  ui->label_quality->setEnabled(true);
  ui->comboBox_quality->setEnabled(true);

  fbs_field_list_.emplace_back();

  google::protobuf::Descriptor* target_desc = nullptr;

  {
    std::lock_guard lock(window_->data_mutex_);

    select_urls_.clear();

    if (selected_items.count() == 1) {
      QString url = selected_items.at(0)->text(1);
      QString ser = selected_items.at(0)->data(1, Qt::UserRole).toString();

      select_urls_.emplace(url.toStdString());

      if (current_url_.empty()) {
        current_url_ = url.toStdString();
      }

      const auto url_str = url.toStdString();
      const auto ser_str = ser.toStdString();
      const auto schema_iter = window_->schema_type_map_.find(url_str);
      const auto schema_type =
          schema_iter != window_->schema_type_map_.end() ? schema_iter->second : vlink::SchemaType::kUnknown;

      if (schema_type == vlink::SchemaType::kProtobuf && !target_desc && window_->des_pool_) {
        target_desc = const_cast<google::protobuf::Descriptor*>(window_->des_pool_->FindMessageTypeByName(ser_str));
      } else if (schema_type == vlink::SchemaType::kFlatbuffers && !target_fbs_context_) {
        target_fbs_context_ = window_->flatbuffers_runtime_.find_context(ser_str);
      }

      ui->pushButton_point3d->setEnabled(false);
    } else {
      for (const auto& item : selected_items) {
        QString url = item->text(1);
        QString ser = item->data(1, Qt::UserRole).toString();

#if USE_USER_CONDITION
        QString lower_url = url.toLower();
        QString lower_ser = ser.toLower();

        if (lower_url.contains("calib")) {
          continue;
        }

        if (!lower_ser.contains("cam")) {
          continue;
        }
#endif

        select_urls_.emplace(url.toStdString());

        if (current_url_.empty()) {
          current_url_ = url.toStdString();
        }

        const auto url_str = url.toStdString();
        const auto ser_str = ser.toStdString();
        const auto schema_iter = window_->schema_type_map_.find(url_str);
        const auto schema_type =
            schema_iter != window_->schema_type_map_.end() ? schema_iter->second : vlink::SchemaType::kUnknown;

        if (schema_type == vlink::SchemaType::kProtobuf && !target_desc && window_->des_pool_) {
          target_desc = const_cast<google::protobuf::Descriptor*>(window_->des_pool_->FindMessageTypeByName(ser_str));
        } else if (schema_type == vlink::SchemaType::kFlatbuffers && !target_fbs_context_) {
          target_fbs_context_ = window_->flatbuffers_runtime_.find_context(ser_str);
        }
      }

      ui->pushButton_point3d->setEnabled(false);
    }

    auto data_callback = [this](const vlink::ProxyAPI::Data& proxy_data) {
      if (select_urls_.count(proxy_data.url) == 0) {
        std::lock_guard lock(data_mutex_);
        if (data_callback_) {
          data_callback_(proxy_data);
        }

        return;
      }

      if (pause_flag_) {
        return;
      }

      QElapsedTimer timer;
      timer.start();

      const auto schema_type = proxy_data.schema;

      if (schema_type == vlink::SchemaType::kZeroCopy &&
          proxy_data.ser == vlink::Serializer::get_serialized_type<vlink::zerocopy::CameraFrame>()) {
        QMetaObject::invokeMethod(this, "update_ui_for_zero_copy_types", Qt::QueuedConnection,
                                  Q_ARG(QVariant, QVariant::fromValue<vlink::ProxyAPI::Data>(proxy_data)),
                                  Q_ARG(QElapsedTimer, timer));
      } else if (schema_type == vlink::SchemaType::kProtobuf && target_msg_) {
        QMetaObject::invokeMethod(this, "update_ui_for_proto", Qt::QueuedConnection,
                                  Q_ARG(QVariant, QVariant::fromValue<vlink::ProxyAPI::Data>(proxy_data)),
                                  Q_ARG(QElapsedTimer, timer));
      } else if (schema_type == vlink::SchemaType::kFlatbuffers && target_fbs_context_) {
        QMetaObject::invokeMethod(this, "update_ui_for_flatbuffers", Qt::QueuedConnection,
                                  Q_ARG(QVariant, QVariant::fromValue<vlink::ProxyAPI::Data>(proxy_data)),
                                  Q_ARG(QElapsedTimer, timer));
      } else {
        QMetaObject::invokeMethod(this, "update_ui_for_unknown_types", Qt::QueuedConnection,
                                  Q_ARG(QVariant, QVariant::fromValue<vlink::ProxyAPI::Data>(proxy_data)),
                                  Q_ARG(QElapsedTimer, timer));
      }
    };

    if (parent) {
      if (auto* parent_dialog = qobject_cast<Point3DDialog*>(parent)) {
        std::lock_guard plock(parent_dialog->data_mutex_);
        parent_dialog->data_callback_ = std::move(data_callback);
      } else if (auto* perception_dialog = qobject_cast<PerceptionDialog*>(parent)) {
        std::lock_guard plock(perception_dialog->data_mutex_);
        perception_dialog->data_callback_ = std::move(data_callback);
      }
    } else {
      window_->data_callback_ = std::move(data_callback);
    }
  }

  int num_cols = std::ceil(std::sqrt(select_urls_.size()));
  int index = 0;

  for (const auto& url : select_urls_) {
    int row = index / num_cols;
    int col = index % num_cols;

    CameraLabel* label = new CameraLabel(QString::fromStdString(url), this, ui->widget);
    label->setText(tr("No Image"));
    camera_layout_->addWidget(label, row, col);

    Detail detail;
    detail.channel = index;
    detail.label = label;
    detail.frame_count = 0;
    detail.last_frame_count = 0;
    detail.total_rate = 0;
    detail.state = kNoImage;

    Detail& t_detail = camera_detail_map_.emplace(label->get_title(), std::move(detail)).first->second;
    channel_map_.emplace(index, label->get_title());

    label->register_path_callback([&t_detail](const std::string& path, bool whole_label) {
      QFileInfo file_info(QString::fromStdString(path));

      if (whole_label) {
        if (file_info.suffix().toLower() == "jpg") {
          t_detail.label->grab().save(file_info.filePath(), "jpg", 100);
        } else if (file_info.suffix().toLower() == "png") {
          t_detail.label->grab().save(file_info.filePath(), "png", 100);
        }
      } else {
        if (file_info.suffix().toLower() == "jpg") {
          t_detail.img.save(file_info.filePath(), "jpg", 100);
        } else if (file_info.suffix().toLower() == "png") {
          t_detail.img.save(file_info.filePath(), "png", 100);
        }
      }
    });

    label->register_size_callback([label, &t_detail](int w, int h) {
      if (t_detail.state == kLoadSucceed && !t_detail.img.isNull()) {
        QPixmap pixmap;

        if (!pixmap.convertFromImage(t_detail.img)) {
          return;
        }

        pixmap =
            pixmap.scaled(w, h, Qt::AspectRatioMode::KeepAspectRatio, Qt::TransformationMode::SmoothTransformation);

        label->setPixmap(pixmap);
      }
    });

    ++index;
  }

  if (select_urls_.size() == 1 && !parent) {
    ui->pushButton_projection->setEnabled(true);
  } else {
    ui->pushButton_projection->setEnabled(false);
  }

  ui->widget->setLayout(camera_layout_);

  if (target_desc) {
    ui->label_proto->setEnabled(true);
    ui->comboBox_proto->setEnabled(true);
    ui->label_offset->setEnabled(false);
    ui->spinBox_offset->setEnabled(false);

    target_msg_ = window_->factory_->GetPrototype(target_desc)->New();

    if (target_msg_) {
      for (int i = 0; i < target_msg_->GetDescriptor()->field_count(); ++i) {
        const auto* field = target_msg_->GetDescriptor()->field(i);

        if (!field->is_repeated() && field->type() == google::protobuf::FieldDescriptor::TYPE_BYTES) {
#if GOOGLE_PROTOBUF_VERSION >= 6030000
          ui->comboBox_proto->addItem(field->name().data());
#else
          ui->comboBox_proto->addItem(field->name().c_str());
#endif
          msg_list_.emplace_back(nullptr, field);
        } else if (!field->is_repeated() && field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
          const auto* sub_desc = field->message_type();
          for (int j = 0; j < sub_desc->field_count(); ++j) {
            const auto* sub_field = sub_desc->field(j);

            if (!sub_field->is_repeated() && sub_field->type() == google::protobuf::FieldDescriptor::TYPE_BYTES) {
#if GOOGLE_PROTOBUF_VERSION >= 6030000
              ui->comboBox_proto->addItem(sub_field->name().data());
#else
              ui->comboBox_proto->addItem(sub_field->name().c_str());
#endif
              msg_list_.emplace_back(field, sub_field);
            }
          }
        }
      }
    }

    if (ui->comboBox_proto->count() > 1) {
      ui->comboBox_proto->setCurrentIndex(1);
      ui->groupBox_config->setEnabled(true);
      ui->groupBox_info->setEnabled(true);
    } else {
      ui->groupBox_config->setEnabled(false);
      ui->groupBox_info->setEnabled(false);
    }
  } else if (target_fbs_context_ && target_fbs_context_->valid()) {
    ui->label_proto->setEnabled(true);
    ui->comboBox_proto->setEnabled(true);
    ui->label_offset->setEnabled(false);
    ui->spinBox_offset->setEnabled(false);

    const auto* root_object = target_fbs_context_->root_object;
    const auto* schema = target_fbs_context_->schema;

    if (root_object && root_object->fields() && schema) {
      for (uint32_t i = 0; i < root_object->fields()->size(); ++i) {
        const auto* field = root_object->fields()->Get(i);

        if (!field) {
          continue;
        }

        if (is_bytes_field(*field)) {
          const auto field_name = field->name()->str();
          ui->comboBox_proto->addItem(QString::fromStdString(field_name));
          fbs_field_list_.emplace_back(field_name);
          continue;
        }

        if (field->type()->base_type() != reflection::Obj || !schema->objects()) {
          continue;
        }

        const auto* child_object = schema->objects()->Get(static_cast<uint32_t>(field->type()->index()));
        if (!child_object || !child_object->fields()) {
          continue;
        }

        for (uint32_t j = 0; j < child_object->fields()->size(); ++j) {
          const auto* child_field = child_object->fields()->Get(j);

          if (!child_field || !is_bytes_field(*child_field)) {
            continue;
          }

          const auto path = field->name()->str() + "." + child_field->name()->str();
          ui->comboBox_proto->addItem(QString::fromStdString(path));
          fbs_field_list_.emplace_back(path);
        }
      }
    }

    if (ui->comboBox_proto->count() > 1) {
      ui->comboBox_proto->setCurrentIndex(1);
      ui->groupBox_config->setEnabled(true);
      ui->groupBox_info->setEnabled(true);
    } else {
      ui->groupBox_config->setEnabled(false);
      ui->groupBox_info->setEnabled(false);
    }
  } else {
    ui->label_proto->setEnabled(false);
    ui->comboBox_proto->setEnabled(false);
    ui->label_offset->setEnabled(true);
    ui->spinBox_offset->setEnabled(true);

    ui->groupBox_config->setEnabled(true);
    ui->groupBox_info->setEnabled(true);
  }

  {
    QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                       QSettings::IniFormat);

    const bool use_ext_settings = this->parent() != nullptr;
    QVariant camera_type_value;
    QVariant camera_type_name_value;
    QVariant yuv_width_value;
    QVariant yuv_height_value;
    QByteArray geometry;

    settings.beginGroup(use_ext_settings ? "CameraDialog_ext" : "CameraDialog");
    camera_type_value = settings.value("camera_type");
    camera_type_name_value = settings.value("camera_type_name");
    yuv_width_value = settings.value("yuv_width");
    yuv_height_value = settings.value("yuv_height");
    geometry = settings.value("geometry").toByteArray();
    settings.endGroup();

    if (use_ext_settings) {
      settings.beginGroup("CameraDialog");
      if (!camera_type_value.isValid()) {
        camera_type_value = settings.value("camera_type");
      }
      if (!camera_type_name_value.isValid()) {
        camera_type_name_value = settings.value("camera_type_name");
      }
      if (!yuv_width_value.isValid()) {
        yuv_width_value = settings.value("yuv_width");
      }
      if (!yuv_height_value.isValid()) {
        yuv_height_value = settings.value("yuv_height");
      }
      settings.endGroup();
    }

    bool camera_type_applied = false;

    if (camera_type_name_value.isValid()) {
      const int camera_type = ui->comboBox_type->findText(camera_type_name_value.toString());

      if (camera_type >= 0) {
        ui->comboBox_type->setCurrentIndex(camera_type);
        camera_type_applied = true;
      }
    }

    if (!camera_type_applied && camera_type_value.isValid()) {
      const int camera_type = camera_type_value.toInt() + 1;

      if (camera_type >= 0 && camera_type < ui->comboBox_type->count()) {
        ui->comboBox_type->setCurrentIndex(camera_type);
      }
    }

    if (yuv_width_value.isValid() && yuv_width_value.toInt() > 0) {
      ui->spinBox_yuv_width->setValue(yuv_width_value.toInt());
    }

    if (yuv_height_value.isValid() && yuv_height_value.toInt() > 0) {
      ui->spinBox_yuv_height->setValue(yuv_height_value.toInt());
    }

    if (!geometry.isEmpty()) {
      restoreGeometry(geometry);
    }
  }

  timer_ = new QTimer(this);
  timer_->setTimerType(Qt::PreciseTimer);
  timer_->setInterval(1000);

  connect(timer_, &QTimer::timeout, this, [this]() {
    for (auto& [url, detail] : camera_detail_map_) {
      if (!detail.label) {
        return;
      }

      if (has_label_pixmap(detail.label)) {
        if (detail.frame_count != 0) {
          float real_frame_count = (detail.last_frame_count * 1 + detail.frame_count * 2) / 3.0f;
          detail.last_frame_count = detail.frame_count;
          if (!multi_mode_) {
            ui->label_frame2->setText(QString::number(real_frame_count, 'f', 2));
          }

          if (FFmpegDecoder::is_valid() && detail.decoder) {
            detail.label->update_info(real_frame_count, detail.decoder->get_average_decode_cost());
          } else {
            detail.label->update_info(real_frame_count, -1);
          }

          QString total_rate_str;
          if (detail.total_rate < 1024) {
            total_rate_str = QString::number(detail.total_rate) + "B/S";
          } else if (detail.total_rate < 1024 * 1024) {
            total_rate_str = QString::number(detail.total_rate / 1024.0F, 'f', 2) + "KB/S";
          } else {
            total_rate_str = QString::number(detail.total_rate / 1024 / 1024.0F, 'f', 2) + "MB/S";
          }
          if (!multi_mode_) {
            ui->label_transfer2->setText(total_rate_str);
          }

        } else {
          if (detail.state != kNoImage) {
            if (!multi_mode_) {
              ui->label_transfer2->setText("---");
              ui->label_frame2->setText("---");
              ui->label_size2->setText("---");
            }

            // ui->label_img->setPixmap(QPixmap());
            // ui->label_img->setText(tr("No Image"));
            detail.state = kNoImage;
          }

          if (FFmpegDecoder::is_valid() && detail.decoder) {
            detail.label->update_info(0, detail.decoder->get_average_decode_cost());
          } else {
            detail.label->update_info(0, -1);
          }
        }
      }

      detail.frame_count = 0;
      detail.total_rate = 0;
    }
  });

  connect(ui->comboBox_proto, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
          [this](int index) {
            if (index == 0) {
              ui->groupBox_config->setEnabled(false);
              ui->groupBox_info->setEnabled(false);
              ui->groupBox_yuv->setEnabled(false);

              for (auto& [url, detail] : camera_detail_map_) {
                detail.decoder.reset();
                ++detail.decoder_seq;
                detail.decoder_type = FFmpegDecoder::InType::kUnknown;
                detail.decoder_width = 0;
                detail.decoder_height = 0;
              }
            } else {
              ui->groupBox_config->setEnabled(true);
              ui->groupBox_info->setEnabled(true);
              ui->groupBox_yuv->setEnabled(has_yuv_format());

              for (auto& [url, detail] : camera_detail_map_) {
                create_decoder(url, get_decoder_type());
              }
            }
          });

  connect(ui->comboBox_type, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
          [this](int index) {
            QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                               QSettings::IniFormat);

            if (this->parent()) {
              settings.beginGroup("CameraDialog_ext");
            } else {
              settings.beginGroup("CameraDialog");
            }

            settings.setValue("camera_type", index - 1);
            settings.setValue("camera_type_name", ui->comboBox_type->itemText(index));
            settings.endGroup();
            settings.sync();

            ui->groupBox_yuv->setEnabled(has_yuv_format());

            for (auto& [url, detail] : camera_detail_map_) {
              create_decoder(url, get_decoder_type());
            }
          });

  connect(ui->comboBox_quality, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
          [this](int index) {
            switch (index) {
              case 0:
                display_quality_ = 1.0;
                break;
              case 1:
                display_quality_ = 0.75;
                break;
              case 2:
                display_quality_ = 0.5;
                break;
              case 3:
                display_quality_ = 0.25;
                break;
              default:
                break;
            }

            (void)index;
            ui->groupBox_yuv->setEnabled(has_yuv_format());

            for (auto& [url, detail] : camera_detail_map_) {
              create_decoder(url, get_decoder_type());
            }
          });

  connect(ui->comboBox_elapsed, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
          [this](int index) {
            (void)index;

            for (auto& [url, detail] : camera_detail_map_) {
              create_decoder(url, get_decoder_type());
            }
          });

  timer_->start();

  ui->groupBox_yuv->setEnabled(has_yuv_format());

  for (auto& [url, detail] : camera_detail_map_) {
    create_decoder(url, get_decoder_type());
  }

  ui->label_transfer2->setText("---");
  ui->label_frame2->setText("---");
  ui->label_size2->setText("---");

  ui->pushButton_close->setFocusPolicy(Qt::NoFocus);
  setFocus();
}

CameraDialog::~CameraDialog() {
  quit_flag_ = true;

  {
    QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                       QSettings::IniFormat);

    if (this->parent()) {
      settings.beginGroup("CameraDialog_ext");
    } else {
      settings.beginGroup("CameraDialog");
    }

    settings.setValue("geometry", saveGeometry());
    settings.setValue("yuv_width", ui->spinBox_yuv_width->value());
    settings.setValue("yuv_height", ui->spinBox_yuv_height->value());

    settings.endGroup();

    settings.sync();
  }

  if (auto* parent_dialog = qobject_cast<Point3DDialog*>(parent())) {
    std::lock_guard lock(parent_dialog->data_mutex_);
    parent_dialog->data_callback_ = nullptr;
  } else if (auto* perception_dialog = qobject_cast<PerceptionDialog*>(parent())) {
    std::lock_guard lock(perception_dialog->data_mutex_);
    perception_dialog->data_callback_ = nullptr;
  } else if (!parent() && window_) {
    std::lock_guard lock(window_->data_mutex_);
    window_->data_callback_ = nullptr;
  }

  {
    std::lock_guard lock(data_mutex_);

    data_callback_ = nullptr;
  }

  for (auto& [url, detail] : camera_detail_map_) {
    detail.decoder.reset();
    ++detail.decoder_seq;
  }

  if (point3d_dialog_) {
    point3d_dialog_->close();
    point3d_dialog_->deleteLater();
    point3d_dialog_ = nullptr;
  }

  delete ui;

  if (target_msg_) {
    delete target_msg_;
  }

  camera_detail_map_.clear();
}

void CameraDialog::showEvent(QShowEvent* event) {
  if (timer_ && !timer_->isActive()) {
    timer_->start();
  }

  QDialog::showEvent(event);
}

void CameraDialog::hideEvent(QHideEvent* event) { QDialog::hideEvent(event); }

void CameraDialog::closeEvent(QCloseEvent* event) {
  timer_->stop();
  QDialog::closeEvent(event);
}

void CameraDialog::resizeEvent(QResizeEvent* event) { QDialog::resizeEvent(event); }

void CameraDialog::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Space) {
    on_pushButton_pause_clicked();
  }

  QDialog::keyPressEvent(event);
}

void CameraDialog::keyReleaseEvent(QKeyEvent* event) { QDialog::keyReleaseEvent(event); }

void CameraDialog::update_ui_for_proto(const QVariant& variant, const QElapsedTimer& timer) {
  if (!target_msg_) {
    return;
  }

  if (pause_flag_) {
    return;
  }

  if (timer.elapsed() > 1000) {
    return;
  }

  const auto& proxy_data = variant.value<vlink::ProxyAPI::Data>();
  auto decoder_type = get_decoder_type();

  auto& detail = camera_detail_map_[proxy_data.url];

  if (!detail.label) {
    return;
  }

  auto& [outer_field, field] = msg_list_.at(ui->comboBox_proto->currentIndex());

  if (!field) {
    if (detail.state != kNoImage) {
      if (!multi_mode_) {
        ui->label_transfer2->setText("---");
        ui->label_frame2->setText("---");
        ui->label_size2->setText("---");
      }

      detail.label->set_error(true);
      detail.label->setPixmap(QPixmap());
      detail.label->setText(tr("No Image"));
      detail.state = kNoImage;
    }
    return;
  }

  if (!target_msg_->ParseFromArray(proxy_data.raw.data(), proxy_data.raw.size())) {
    if (detail.state != kParseFailed) {
      if (!multi_mode_) {
        ui->label_transfer2->setText("---");
        ui->label_frame2->setText("---");
        ui->label_size2->setText("---");
      }

      detail.label->set_error(true);
      detail.label->setPixmap(QPixmap());
      detail.label->setText(tr("Parse failed"));
      detail.state = kParseFailed;
    }
    return;
  }

  ui->spinBox_offset->setEnabled(false);
  ui->label_offset->setEnabled(false);

  const google::protobuf::Message* msg = target_msg_;

  if (outer_field) {
    msg = &target_msg_->GetReflection()->GetMessage(*target_msg_, outer_field);
  }

  const std::string& raw_str = msg->GetReflection()->GetString(*msg, field);

  const auto& raw_data = vlink::Bytes::shallow_copy(reinterpret_cast<const uint8_t*>(raw_str.c_str()), raw_str.size());

  if (raw_data.empty()) {
    if (detail.state != kParseFailed) {
      if (!multi_mode_) {
        ui->label_transfer2->setText("---");
        ui->label_frame2->setText("---");
        ui->label_size2->setText("---");
      }

      detail.label->set_error(true);
      detail.label->setPixmap(QPixmap());
      detail.label->setText(tr("Parse failed"));
      detail.state = kParseFailed;
    }
    return;
  }

  process_raw(proxy_data.url, detail, raw_data, decoder_type);
}

void CameraDialog::update_ui_for_flatbuffers(const QVariant& variant, const QElapsedTimer& timer) {
  if (!target_fbs_context_ || !target_fbs_context_->valid()) {
    return;
  }

  if (pause_flag_) {
    return;
  }

  if (timer.elapsed() > 1000) {
    return;
  }

  const auto& proxy_data = variant.value<vlink::ProxyAPI::Data>();
  auto decoder_type = get_decoder_type();

  auto& detail = camera_detail_map_[proxy_data.url];

  if (!detail.label) {
    return;
  }

  if (ui->comboBox_proto->currentIndex() < 0 ||
      static_cast<size_t>(ui->comboBox_proto->currentIndex()) >= fbs_field_list_.size()) {
    return;
  }

  const auto& field_path = fbs_field_list_.at(ui->comboBox_proto->currentIndex());

  if (field_path.empty()) {
    return;
  }

  FlatbuffersObjectView root_view;

  if (!make_root_view(*target_fbs_context_, proxy_data.raw, root_view)) {
    if (detail.state != kParseFailed) {
      if (!multi_mode_) {
        ui->label_transfer2->setText("---");
        ui->label_frame2->setText("---");
        ui->label_size2->setText("---");
      }

      detail.label->set_error(true);
      detail.label->setPixmap(QPixmap());
      detail.label->setText(tr("Parse failed"));
      detail.state = kParseFailed;
    }
    return;
  }

  FlatbuffersObjectView parent_view;
  const reflection::Field* field = nullptr;

  if (!resolve_flatbuffers_field_path(root_view, *target_fbs_context_->schema, field_path, parent_view, field) ||
      !field) {
    if (detail.state != kParseFailed) {
      if (!multi_mode_) {
        ui->label_transfer2->setText("---");
        ui->label_frame2->setText("---");
        ui->label_size2->setText("---");
      }

      detail.label->set_error(true);
      detail.label->setPixmap(QPixmap());
      detail.label->setText(tr("Parse failed"));
      detail.state = kParseFailed;
    }
    return;
  }

  vlink::Bytes raw_data;

  if (!get_bytes(parent_view, *field, raw_data) || raw_data.empty()) {
    if (detail.state != kParseFailed) {
      if (!multi_mode_) {
        ui->label_transfer2->setText("---");
        ui->label_frame2->setText("---");
        ui->label_size2->setText("---");
      }

      detail.label->set_error(true);
      detail.label->setPixmap(QPixmap());
      detail.label->setText(tr("Parse failed"));
      detail.state = kParseFailed;
    }
    return;
  }

  ui->spinBox_offset->setEnabled(false);
  ui->label_offset->setEnabled(false);

  process_raw(proxy_data.url, detail, raw_data, decoder_type);
}

void CameraDialog::update_ui_for_zero_copy_types(const QVariant& variant, const QElapsedTimer& timer) {
  if (pause_flag_) {
    return;
  }

  if (timer.elapsed() > 1000) {
    return;
  }

  const auto& proxy_data = variant.value<vlink::ProxyAPI::Data>();

  auto& detail = camera_detail_map_[proxy_data.url];

  if (!detail.label) {
    return;
  }

  vlink::zerocopy::CameraFrame camera_frame;
  camera_frame << proxy_data.raw;

  const auto decoder_type = get_decoder_type();

  if (decoder_type == FFmpegDecoder::InType::kUnknown || !camera_frame.data() || camera_frame.size() == 0) {
    process_frame(proxy_data.url, detail, camera_frame);
    return;
  }

  int camera_width = 0;
  int camera_height = 0;
  const bool has_valid_camera_size = get_camera_frame_image_size(camera_frame, camera_width, camera_height);

  if (camera_detail_map_.size() == 1 && has_valid_camera_size) {
    ui->spinBox_yuv_width->setValue(camera_width);
    ui->spinBox_yuv_height->setValue(camera_height);
  }

  ui->spinBox_offset->setEnabled(false);
  ui->label_offset->setEnabled(false);

  detail.label->set_timestamp(camera_frame.header.time_meas / 1000);

  int width = has_valid_camera_size ? camera_width : 0;
  int height = has_valid_camera_size ? camera_height : 0;

  if ((width <= 0 || height <= 0) && ui->groupBox_yuv->isEnabled()) {
    width = ui->spinBox_yuv_width->value();
    height = ui->spinBox_yuv_height->value();
  }

  if (!detail.decoder || detail.decoder_type != decoder_type || detail.decoder_width != width ||
      detail.decoder_height != height) {
    create_decoder(proxy_data.url, decoder_type, width, height);
  }

  vlink::Bytes raw_data = vlink::Bytes::shallow_copy(camera_frame.data(), camera_frame.size());

  process_raw(proxy_data.url, detail, raw_data, decoder_type);
}

void CameraDialog::update_ui_for_unknown_types(const QVariant& variant, const QElapsedTimer& timer) {
  if (pause_flag_) {
    return;
  }

  if (timer.elapsed() > 3000) {
    return;
  }

  const auto& proxy_data = variant.value<vlink::ProxyAPI::Data>();
  auto decoder_type = get_decoder_type();

  auto& detail = camera_detail_map_[proxy_data.url];

  if (!detail.label) {
    return;
  }

  ui->spinBox_offset->setEnabled(true);
  ui->label_offset->setEnabled(true);

  if (static_cast<size_t>(ui->spinBox_offset->value()) >= proxy_data.raw.size()) {
    return;
  }

  const auto& raw_data = vlink::Bytes::shallow_copy(proxy_data.raw.data() + ui->spinBox_offset->value(),
                                                    proxy_data.raw.size() - ui->spinBox_offset->value());

  if (raw_data.empty()) {
    if (detail.state != kParseFailed) {
      if (!multi_mode_) {
        ui->label_transfer2->setText("---");
        ui->label_frame2->setText("---");
        ui->label_size2->setText("---");
      }

      detail.label->set_error(true);
      detail.label->setPixmap(QPixmap());
      detail.label->setText(tr("Parse failed"));
      detail.state = kParseFailed;
    }
    return;
  }

  process_raw(proxy_data.url, detail, raw_data, decoder_type);
}

void CameraDialog::on_checkBox_display_clicked(bool checked) {
  for (auto& [url, detail] : camera_detail_map_) {
    if (detail.label) {
      detail.label->set_show_info(checked);
      detail.label->update();
    }
  }
}

void CameraDialog::on_pushButton_close_clicked() { this->close(); }

void CameraDialog::on_pushButton_yuv_clicked() {
  auto decoder_type = get_decoder_type();

  if ((target_msg_ || target_fbs_context_) && ui->comboBox_proto->currentIndex() == 0) {
    for (auto& [url, detail] : camera_detail_map_) {
      detail.decoder.reset();
      ++detail.decoder_seq;
      detail.decoder_type = FFmpegDecoder::InType::kUnknown;
      detail.decoder_width = 0;
      detail.decoder_height = 0;
    }
  } else {
    for (auto& [url, detail] : camera_detail_map_) {
      create_decoder(url, decoder_type);
    }
  }
}

void CameraDialog::on_checkBox_cache_clicked(bool checked) {
  (void)checked;

  for (auto& [url, detail] : camera_detail_map_) {
    create_decoder(url, get_decoder_type());
  }
}

void CameraDialog::on_checkBox_hard_toggled(bool checked) {
  (void)checked;

  for (auto& [url, detail] : camera_detail_map_) {
    create_decoder(url, get_decoder_type());
  }
}

void CameraDialog::on_pushButton_point3d_clicked() {
  if (point3d_dialog_) {
    point3d_dialog_->show();
  } else {
    point3d_dialog_ = new Point3DDialog(this, false);
    point3d_dialog_->show();
  }
}

void CameraDialog::on_pushButton_projection_clicked() {
  if (!projection_dialog_) {
    projection_dialog_ = new ProjectionDialog(this);
  }

  proj_params_ = projection_dialog_->process();

  if (!proj_params_.is_valid) {
    return;
  }

  {
    Eigen::Matrix4f extrinsic_matrix = Eigen::Matrix4f::Identity();

    if (proj_params_.ext_rvec.norm() < 1e-6) {
      extrinsic_matrix.block<3, 3>(0, 0) = Eigen::Matrix3f::Identity();
    } else {
      extrinsic_matrix.block<3, 3>(0, 0) =
          Eigen::AngleAxisf(proj_params_.ext_rvec.norm(), proj_params_.ext_rvec.normalized()).toRotationMatrix();
    }

    extrinsic_matrix.block<3, 1>(0, 3) = proj_params_.ext_tvec;

    projection_matrix_ = proj_params_.in_mat * extrinsic_matrix.block<3, 4>(0, 0);
  }

  if (!point3d_dialog_) {
    point3d_dialog_ = new Point3DDialog(this, true);
    connect(point3d_dialog_, &Point3DDialog::point3d_map_changed, this, [this]() { process_projection(); });
  }

  process_projection();
}

void CameraDialog::on_pushButton_pause_clicked() {
  pause_flag_ = !pause_flag_.load();

  if (pause_flag_) {
    ui->label_pause->setVisible(true);
    ui->pushButton_pause->setText(tr("Resume"));
    ui->pushButton_pause->setIcon(QIcon(":/resource/resume.png"));
  } else {
    ui->label_pause->setVisible(false);
    ui->pushButton_pause->setText(tr("Pause"));
    ui->pushButton_pause->setIcon(QIcon(":/resource/pause.png"));
  }
}

void CameraDialog::process_image(const QString& url, int width, int height, int bytes_per_line,
                                 const QByteArray& img_data, int decoder_seq) {
  auto it = camera_detail_map_.find(url.toStdString());
  if (it == camera_detail_map_.end() || it->second.decoder_seq != decoder_seq) {
    return;
  }

  if (img_data.isEmpty()) {
    process_qimage(url, QImage(), true);
    return;
  }

  QImage image;

  const size_t expected = width > 0 && height > 0 && bytes_per_line > 0
                              ? static_cast<size_t>(bytes_per_line) * static_cast<size_t>(height)
                              : 0U;
  if (expected > 0U && expected <= static_cast<size_t>(img_data.size())) {
    image =
        QImage(reinterpret_cast<const uint8_t*>(img_data.data()), width, height, bytes_per_line, QImage::Format_RGB888)
            .copy();
  }

  process_qimage(url, image, true);
}

void CameraDialog::process_error(const QString& url, int decoder_seq) {
  auto it = camera_detail_map_.find(url.toStdString());
  if (it == camera_detail_map_.end() || it->second.decoder_seq != decoder_seq || !it->second.label) {
    return;
  }

  auto& detail = it->second;
  detail.label->set_error(true);
  detail.label->setPixmap(QPixmap());
  detail.label->setText(tr("Load failed"));
  detail.img = QImage();
  detail.state = kLoadFailed;
}

void CameraDialog::process_qimage(const QString& url, const QImage& image, bool scaled_by_decoder) {
  if (quit_flag_) {
    return;
  }

  auto& detail = camera_detail_map_[url.toStdString()];

  if (!detail.label) {
    return;
  }

  QImage display_image = image;
  if (!scaled_by_decoder && display_quality_ > 0.0 && display_quality_ < 1.0 && !image.isNull()) {
    const int width = std::max(1, static_cast<int>(std::lround(image.width() * display_quality_)));
    const int height = std::max(1, static_cast<int>(std::lround(image.height() * display_quality_)));
    display_image =
        image.scaled(width, height, Qt::AspectRatioMode::IgnoreAspectRatio, Qt::TransformationMode::FastTransformation);
  }

  QPixmap pixmap;
  bool ok = !display_image.isNull() && pixmap.convertFromImage(display_image);

  if (!ok) {
    if (detail.state != kLoadFailed) {
      if (!multi_mode_) {
        ui->label_transfer2->setText("---");
        ui->label_frame2->setText("---");
        ui->label_size2->setText("---");
      }

      detail.label->set_error(true);
      detail.label->setPixmap(QPixmap());
      detail.label->setText(tr("Load failed"));
      detail.img = QImage();
      detail.state = kLoadFailed;
    }
    return;
  }

  detail.img = image;

  if (scaled_by_decoder) {
    if (!multi_mode_) {
      ui->label_size2->setText(QString::number(pixmap.width() / display_quality_) + "x" +
                               QString::number(pixmap.height() / display_quality_));
    }

    detail.label->set_camera_size(QSize(pixmap.width() / display_quality_, pixmap.height() / display_quality_));
  } else {
    if (!multi_mode_) {
      ui->label_size2->setText(QString::number(image.width()) + "x" + QString::number(image.height()));
    }

    detail.label->set_camera_size(image.size());
  }

  if (detail.state != kLoadSucceed) {
    detail.label->setText("");
  }

  pixmap = pixmap.scaled(detail.label->size(), Qt::AspectRatioMode::KeepAspectRatio,
                         Qt::TransformationMode::SmoothTransformation);

  if (quit_flag_) {
    return;
  }

  detail.label->set_error(false);

  detail.label->setPixmap(pixmap);

  detail.label->update();

  detail.state = kLoadSucceed;

  detail.frame_count += 1;
}

void CameraDialog::process_frame(const std::string& url, Detail& detail, const vlink::zerocopy::CameraFrame& frame) {
  auto clear_decoder = [&detail]() {
    const bool had_decoder = detail.decoder || detail.decoder_type != FFmpegDecoder::InType::kUnknown ||
                             detail.decoder_width != 0 || detail.decoder_height != 0;

    detail.decoder.reset();
    detail.decoder_type = FFmpegDecoder::InType::kUnknown;
    detail.decoder_width = 0;
    detail.decoder_height = 0;
    detail.img = QImage();

    if (had_decoder) {
      ++detail.decoder_seq;
    }
  };

  if VUNLIKELY (!frame.data() || frame.size() == 0) {
    clear_decoder();

    if (detail.state != kParseFailed) {
      if (!multi_mode_) {
        ui->label_transfer2->setText("---");
        ui->label_frame2->setText("---");
        ui->label_size2->setText("---");
      }

      detail.label->set_error(true);
      detail.label->setPixmap(QPixmap());
      detail.label->setText(tr("Parse failed"));
      detail.label->set_timestamp(0);
      detail.state = kParseFailed;
    }
    return;
  }

  detail.label->set_timestamp(frame.header.time_meas / 1000);

  int camera_width = 0;
  int camera_height = 0;
  const bool has_valid_camera_size = get_camera_frame_image_size(frame, camera_width, camera_height);

  if (camera_detail_map_.size() == 1 && has_valid_camera_size) {
    ui->spinBox_yuv_width->setValue(camera_width);
    ui->spinBox_yuv_height->setValue(camera_height);
  }

  ui->spinBox_offset->setEnabled(false);
  ui->label_offset->setEnabled(false);

  vlink::Bytes raw_data = vlink::Bytes::shallow_copy(frame.data(), frame.size());

  const auto decoder_type = camera_frame_decoder_type(frame.format());
  const bool ffmpeg_can_decode = FFmpegDecoder::is_valid() && decoder_type != FFmpegDecoder::InType::kUnknown;

  if (!ffmpeg_can_decode) {
    QImage image;

    if (make_camera_frame_qimage(frame, image) || make_encoded_camera_frame_qimage(frame, image)) {
      clear_decoder();

      process_qimage(QString::fromStdString(url), image, false);
      detail.total_rate += raw_data.size();
      return;
    }
  }

  if (decoder_type == FFmpegDecoder::InType::kUnknown) {
    clear_decoder();

    if (detail.state != kNoSupport) {
      if (!multi_mode_) {
        ui->label_transfer2->setText("---");
        ui->label_frame2->setText("---");
        ui->label_size2->setText("---");
      }

      detail.label->set_error(true);
      detail.label->setPixmap(QPixmap());
      detail.label->setText(tr("Not support"));
      detail.label->set_timestamp(0);
      detail.state = kNoSupport;
    }
    detail.total_rate += raw_data.size();
    return;
  }

  if (FFmpegDecoder::is_valid()) {
    const int width = has_valid_camera_size ? camera_width : 0;
    const int height = has_valid_camera_size ? camera_height : 0;

    if ((needs_raw_size(decoder_type) && !has_valid_camera_size) || !has_ffmpeg_payload(frame, decoder_type)) {
      clear_decoder();

      if (detail.state != kParseFailed) {
        if (!multi_mode_) {
          ui->label_transfer2->setText("---");
          ui->label_frame2->setText("---");
          ui->label_size2->setText("---");
        }

        detail.label->set_error(true);
        detail.label->setPixmap(QPixmap());
        detail.label->setText(tr("Parse failed"));
        detail.label->set_timestamp(0);
        detail.state = kParseFailed;
      }
      detail.total_rate += raw_data.size();
      return;
    }

    if (!detail.decoder || detail.decoder_type != decoder_type || detail.decoder_width != width ||
        detail.decoder_height != height) {
      create_decoder(url, decoder_type, width, height);
    }

    if (detail.decoder) {
      detail.decoder->post_data(detail.channel, 0, raw_data);
    }
  } else {
    clear_decoder();

    if (detail.state != kNoSupport) {
      if (!multi_mode_) {
        ui->label_transfer2->setText("---");
        ui->label_frame2->setText("---");
        ui->label_size2->setText("---");
      }

      detail.label->set_error(true);
      detail.label->setPixmap(QPixmap());
      detail.label->setText(tr("Not support"));
      detail.label->set_timestamp(0);
      detail.state = kNoSupport;
    }
  }

  detail.total_rate += raw_data.size();
}

void CameraDialog::process_raw(const std::string& url, Detail& detail, const vlink::Bytes& raw_data,
                               FFmpegDecoder::InType decoder_type) {
  auto set_parse_failed = [&detail, this]() {
    if (detail.state == kParseFailed) {
      return;
    }

    if (!multi_mode_) {
      ui->label_transfer2->setText("---");
      ui->label_frame2->setText("---");
      ui->label_size2->setText("---");
    }

    detail.label->set_error(true);
    detail.label->setPixmap(QPixmap());
    detail.label->setText(tr("Parse failed"));
    detail.state = kParseFailed;
  };

  if (decoder_type == FFmpegDecoder::InType::kUnknown) {
    vlink::zerocopy::CameraFrame frame;

    if (frame << raw_data) {
      process_frame(url, detail, frame);
      return;
    }

    QImage image;

    if (raw_data.data() && raw_data.size() <= static_cast<size_t>(std::numeric_limits<int>::max())) {
      image = QImage::fromData(static_cast<const uchar*>(raw_data.data()), static_cast<int>(raw_data.size()));
    }

    process_qimage(QString::fromStdString(url), image, false);
    detail.total_rate += raw_data.size();
    return;
  }

  if (FFmpegDecoder::is_valid() && detail.decoder) {
    const int width = detail.decoder_width > 0 ? detail.decoder_width : ui->spinBox_yuv_width->value();
    const int height = detail.decoder_height > 0 ? detail.decoder_height : ui->spinBox_yuv_height->value();

    if (!has_ffmpeg_payload(raw_data, decoder_type, width, height)) {
      set_parse_failed();
      detail.total_rate += raw_data.size();
      return;
    }

    detail.decoder->post_data(detail.channel, 0, raw_data);
    detail.total_rate += raw_data.size();
    return;
  }

  QImage image;

  if (raw_data.data() && raw_data.size() <= static_cast<size_t>(std::numeric_limits<int>::max())) {
    const auto* data = static_cast<const uchar*>(raw_data.data());
    const int size = static_cast<int>(raw_data.size());

    switch (decoder_type) {
      case FFmpegDecoder::InType::kJPG:
        image = QImage::fromData(data, size, "JPG");
        break;
      case FFmpegDecoder::InType::kPNG:
        image = QImage::fromData(data, size, "PNG");
        break;
      case FFmpegDecoder::InType::kWEBP:
        image = QImage::fromData(data, size, "WEBP");
        break;
      default:
        break;
    }
  }

  if (!image.isNull()) {
    process_qimage(QString::fromStdString(url), image, false);
    detail.total_rate += raw_data.size();
    return;
  }

  set_parse_failed();
  detail.total_rate += raw_data.size();
}

void CameraDialog::create_decoder(const std::string& url, FFmpegDecoder::InType type, int width, int height) {
  auto& detail = camera_detail_map_[url];

  if (type == FFmpegDecoder::InType::kUnknown) {
    detail.decoder.reset();
    ++detail.decoder_seq;
    detail.decoder_type = FFmpegDecoder::InType::kUnknown;
    detail.decoder_width = 0;
    detail.decoder_height = 0;
    return;
  }

  if ((width <= 0 || height <= 0) && ui->groupBox_yuv->isEnabled()) {
    width = ui->spinBox_yuv_width->value();
    height = ui->spinBox_yuv_height->value();
  }

  if (needs_raw_size(type) && (width <= 0 || height <= 0)) {
    detail.decoder.reset();
    ++detail.decoder_seq;
    detail.decoder_type = FFmpegDecoder::InType::kUnknown;
    detail.decoder_width = 0;
    detail.decoder_height = 0;
    if (detail.label && detail.state != kParseFailed) {
      if (!multi_mode_) {
        ui->label_transfer2->setText("---");
        ui->label_frame2->setText("---");
        ui->label_size2->setText("---");
      }

      detail.label->set_error(true);
      detail.label->setPixmap(QPixmap());
      detail.label->setText(tr("Parse failed"));
      detail.state = kParseFailed;
    }
    return;
  }

  if (!FFmpegDecoder::is_valid()) {
    detail.decoder.reset();
    ++detail.decoder_seq;
    detail.decoder_type = FFmpegDecoder::InType::kUnknown;
    detail.decoder_width = 0;
    detail.decoder_height = 0;
    return;
  }

  int max_elapsed_time = 0;
  switch (ui->comboBox_elapsed->currentIndex()) {
    case 0:
      max_elapsed_time = 50;
      break;
    case 1:
      max_elapsed_time = 100;
      break;
    case 2:
      max_elapsed_time = 200;
      break;
    case 3:
      max_elapsed_time = 400;
      break;
    case 4:
      max_elapsed_time = 800;
      break;
    default:
      max_elapsed_time = 0;
      break;
  }

  FFmpegDecoder::Config config;
  config.in_type = type;
  config.out_type = FFmpegDecoder::OutType::kRGB888;
  config.width = width;
  config.height = height;
  config.scale = display_quality_;
  config.cache_frame = ui->checkBox_cache->isChecked();
  config.use_hard_codec = ui->checkBox_hard->isChecked();
  config.max_elapsed_time = max_elapsed_time;
  config.max_codec_time = 0;

  ++detail.decoder_seq;
  const int decoder_seq = detail.decoder_seq;
  const QString decoder_url = QString::fromStdString(url);

  detail.decoder.emplace(config);
  detail.decoder_type = type;
  detail.decoder_width = width;
  detail.decoder_height = height;

  detail.decoder->register_handler([this, decoder_url, decoder_seq](int channel, int seq, int width, int height,
                                                                    int bytes_per_line, const vlink::Bytes& img_data) {
    (void)channel;
    (void)seq;
    QByteArray bytes;
    if (!to_qbytes(img_data, bytes)) {
      QMetaObject::invokeMethod(this, "process_error", Qt::QueuedConnection, Q_ARG(QString, decoder_url),
                                Q_ARG(int, decoder_seq));
      return;
    }

    QMetaObject::invokeMethod(this, "process_image", Qt::QueuedConnection, Q_ARG(QString, decoder_url),
                              Q_ARG(int, width), Q_ARG(int, height), Q_ARG(int, bytes_per_line),
                              Q_ARG(QByteArray, bytes), Q_ARG(int, decoder_seq));
  });

  detail.decoder->register_error_handler([this, decoder_url, decoder_seq](int channel, int seq) {
    (void)seq;
    (void)channel;
    QMetaObject::invokeMethod(this, "process_error", Qt::QueuedConnection, Q_ARG(QString, decoder_url),
                              Q_ARG(int, decoder_seq));
  });
}

FFmpegDecoder::InType CameraDialog::get_decoder_type() const {
  switch (ui->comboBox_type->currentIndex()) {
    case 0:
      return FFmpegDecoder::InType::kUnknown;
    case 1:
      return FFmpegDecoder::InType::kJPG;
    case 2:
      return FFmpegDecoder::InType::kPNG;
    case 3:
      return FFmpegDecoder::InType::kWEBP;
    case 4:
      return FFmpegDecoder::InType::kH264;
    case 5:
      return FFmpegDecoder::InType::kH265;
    case 6:
      return FFmpegDecoder::InType::kH266;
    case 7:
      return FFmpegDecoder::InType::kMPEG4;
    case 8:
      return FFmpegDecoder::InType::kAV1;
    case 9:
      return FFmpegDecoder::InType::kYUV420;
    case 10:
      return FFmpegDecoder::InType::kYUV422;
    case 11:
      return FFmpegDecoder::InType::kYUV444;
    case 12:
      return FFmpegDecoder::InType::kNV12;
    case 13:
      return FFmpegDecoder::InType::kNV21;
    case 14:
      return FFmpegDecoder::InType::kYUYV;
    case 15:
      return FFmpegDecoder::InType::kYVYU;
    case 16:
      return FFmpegDecoder::InType::kUYVY;
    case 17:
      return FFmpegDecoder::InType::kBGR888;
    case 18:
      return FFmpegDecoder::InType::kRGB888;
    default:
      return FFmpegDecoder::InType::kUnknown;
  }
}

bool CameraDialog::has_yuv_format() const { return get_decoder_type() >= FFmpegDecoder::InType::kYUV420; }

int CameraDialog::get_number_for_msg(const google::protobuf::Message* msg,
                                     const google::protobuf::FieldDescriptor* field) {
  switch (field->cpp_type()) {
    case google::protobuf::FieldDescriptor::CPPTYPE_INT32: {
      return msg->GetReflection()->GetInt32(*msg, field);
    } break;
    case google::protobuf::FieldDescriptor::CPPTYPE_INT64: {
      return msg->GetReflection()->GetInt64(*msg, field);
    } break;
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT32: {
      return msg->GetReflection()->GetUInt32(*msg, field);
    } break;
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT64: {
      return msg->GetReflection()->GetUInt64(*msg, field);
    }
    default:
      return 0;
  }
}

void CameraDialog::process_projection() {
  if (!proj_params_.is_valid) {
    return;
  }

  if (pause_flag_) {
    return;
  }

  projection_points_.clear();

  float k1 = proj_params_.distortion_mat[0];
  float k2 = proj_params_.distortion_mat[1];
  float p1 = proj_params_.distortion_mat[2];
  float p2 = proj_params_.distortion_mat[3];
  float k3 = proj_params_.distortion_mat.size() > 4 ? proj_params_.distortion_mat[4] : 0;

  float proj_fx = proj_params_.in_mat(0, 0);
  float proj_fy = proj_params_.in_mat(1, 1);
  float proj_cx = proj_params_.in_mat(0, 2);
  float proj_cy = proj_params_.in_mat(1, 2);

  for (const auto& [url, list] : point3d_dialog_->get_point3d_map()) {
    for (const auto& [x, y, z, index, c1, c2, intensity, value_list] : list) {
      if (std::isnan(x) || std::isnan(y) || std::isnan(z)) {
        continue;
      }

      Eigen::Vector4f point_3d_homogeneous(x, y, z, 1.0f);

      Eigen::Vector3f point_2d_homogeneous = projection_matrix_ * point_3d_homogeneous;

      if (point_2d_homogeneous.z() <= 0) {
        continue;
      }

      float px = point_2d_homogeneous.x() / point_2d_homogeneous.z();
      float py = point_2d_homogeneous.y() / point_2d_homogeneous.z();

      if (proj_params_.enable_distortion_mat) {
        float cx = (px - proj_cx) / proj_fx;
        float cy = (py - proj_cy) / proj_fy;

        float r2 = cx * cx + cy * cy;
        float radial_distortion = 1 + k1 * r2 + k2 * r2 * r2 + k3 * r2 * r2 * r2;

        float cx_distorted = cx * radial_distortion;
        float cy_distorted = cy * radial_distortion;

        cx_distorted += 2 * p1 * cx * cy + p2 * (r2 + 2 * cx * cx);
        cy_distorted += p1 * (r2 + 2 * cy * cy) + 2 * p2 * cx * cy;

        px = proj_fx * cx_distorted + proj_cx;
        py = proj_fy * cy_distorted + proj_cy;
      }

      if (px < 0 || px >= proj_params_.img_width || py < 0 || py >= proj_params_.img_height) {
        continue;
      }

      projection_points_.append(QVector3D(px, py, intensity));
    }
  }

  if (!current_url_.empty()) {
    auto& detail = camera_detail_map_[current_url_];

    if (detail.label) {
      detail.label->set_update_points(true);
    }
  }
}

// NOLINTEND
