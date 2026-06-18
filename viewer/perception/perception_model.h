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

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace perception {

enum class RenderType : uint8_t {
  kPointCloud,
  kObjectDetection,
  kLaneLine,
  kPrediction,
  kTrafficLight,
  kStopLine,
  kTrafficSign,
  kFreespace,
  kOccupancyGrid,
  kParkingSlot,
  kEgoTrajectory,
  kHdMap,
  kCameraFrustum,
  kCovarianceEllipse,
};

enum class Encoding : uint8_t {
  kUnknown,
  kProtobuf,
  kFlatbuffers,
  kZeroCopy,
};

struct FieldMapping final {
  std::string source;
  std::string target;
  std::string expression;
  std::string default_value;
  bool has_default_value{false};
  bool default_value_is_string{false};
};

struct PolyPoint final {
  double x{0};
  double y{0};
  double z{0};
  double yaw{0};
  double speed{0};
  double timestamp{0};
};

struct BoxObject final {
  double position[3]{0, 0, 0};
  double size[3]{0, 0, 0};
  double yaw{0};
  double velocity[3]{0, 0, 0};
  double score{0};
  uint32_t class_id{0};
  uint32_t track_id{0};
  uint32_t color{0};
  std::string label;

  uint8_t color_state{0};
  float confidence{1.0F};
  int32_t countdown{-1};

  uint32_t type_id{0};
  double marker_size{0.5};

  double orientation[4]{0, 0, 0, 1};
  double fov_h{60.0};
  double fov_v{45.0};
  double near_dist{0.1};
  double far_dist{10.0};

  double covariance[4]{1, 0, 0, 1};
  float ellipse_alpha{0.3F};
};

struct Polyline final {
  std::vector<PolyPoint> points;
  uint32_t color{0};
  int type{0};
  std::string label;
  uint32_t track_id{0};
  float confidence{1.0F};
};

struct CloudPoint final {
  double x{0};
  double y{0};
  double z{0};
  double value{0};
};

struct Grid final {
  double origin_x{0};
  double origin_y{0};
  double origin_z{0};
  double resolution{0.1};
  uint32_t width{0};
  uint32_t height{0};
  std::vector<int8_t> cells;
};

struct ParkingSlot final {
  double corners[4][3]{{0}};
  uint32_t slot_id{0};
  uint32_t slot_type{0};
  uint32_t color{0};
  float confidence{1.0F};
};

struct HudField final {
  std::string slot;
  double value{0};
  std::string text;
  bool is_text{false};
};

struct Layer final {
  RenderType type{RenderType::kPointCloud};
  std::vector<BoxObject> boxes;
  std::vector<Polyline> polylines;
  std::vector<ParkingSlot> parking_slots;
  std::vector<CloudPoint> cloud;
  Grid grid;
  bool grid_valid{false};
  bool has_value_channel{false};

  [[nodiscard]] size_t primitive_count() const noexcept {
    switch (type) {
      case RenderType::kObjectDetection:
      case RenderType::kTrafficLight:
      case RenderType::kTrafficSign:
      case RenderType::kCameraFrustum:
      case RenderType::kCovarianceEllipse: {
        return boxes.size();
      }

      case RenderType::kParkingSlot: {
        return parking_slots.size();
      }

      case RenderType::kOccupancyGrid: {
        return grid_valid ? static_cast<size_t>(grid.width) * grid.height : 0;
      }

      case RenderType::kPointCloud: {
        return cloud.size();
      }

      default: {
        return polylines.size();
      }
    }
  }
};

}  // namespace perception
