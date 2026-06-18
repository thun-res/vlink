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

#include "./perception_decode.h"

#include <vlink/zerocopy/object_array.h>
#include <vlink/zerocopy/occupancy_grid.h>
#include <vlink/zerocopy/point_cloud.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "./perception_mapping.h"

namespace perception {
namespace decode {

bool decode_zerocopy_object_array(const vlink::Bytes& raw, Layer& out) {
  vlink::zerocopy::ObjectArray arr;

  if (!(arr << raw)) {
    return false;
  }

  out.type = RenderType::kObjectDetection;
  out.boxes.clear();
  out.boxes.reserve(arr.count());

  for (uint32_t i = 0; i < arr.count(); ++i) {
    const auto obj = arr.get_value(i);

    BoxObject box;
    box.position[0] = obj.position[0];
    box.position[1] = obj.position[1];
    box.position[2] = obj.position[2];
    box.size[0] = obj.size[0];
    box.size[1] = obj.size[1];
    box.size[2] = obj.size[2];
    box.yaw = obj.yaw;
    box.velocity[0] = obj.velocity[0];
    box.velocity[1] = obj.velocity[1];
    box.velocity[2] = obj.velocity[2];
    box.score = obj.score;
    box.class_id = obj.class_id;
    box.track_id = obj.track_id;

    if (obj.label[0] != '\0') {
      box.label.assign(obj.label, ::strnlen(obj.label, sizeof(obj.label)));
    }

    out.boxes.emplace_back(std::move(box));
  }

  return true;
}

bool decode_zerocopy_occupancy_grid(const vlink::Bytes& raw, Layer& out) {
  vlink::zerocopy::OccupancyGrid occupancy_grid;

  if (!(occupancy_grid << raw) || !occupancy_grid.is_valid()) {
    return false;
  }

  out.type = RenderType::kOccupancyGrid;

  Grid& grid = out.grid;
  grid.width = occupancy_grid.width();
  grid.height = occupancy_grid.height();
  grid.resolution = occupancy_grid.resolution();
  grid.origin_x = occupancy_grid.origin_x();
  grid.origin_y = occupancy_grid.origin_y();
  grid.origin_z = occupancy_grid.origin_z();

  const auto cell_size = std::max<uint8_t>(occupancy_grid.cell_size(), 1);
  const auto expected_cells = static_cast<size_t>(grid.width) * static_cast<size_t>(grid.height);
  const auto available_cells = occupancy_grid.size() / cell_size;
  const auto cell_count = expected_cells > 0 ? std::min(expected_cells, available_cells) : 0;
  const auto* data = occupancy_grid.data();

  grid.cells.clear();
  grid.cells.reserve(cell_count);

  for (size_t i = 0; i < cell_count; ++i) {
    if (occupancy_grid.cell_type() == vlink::zerocopy::OccupancyGrid::kCellFloat32 && cell_size >= sizeof(float)) {
      float value = 0;
      std::memcpy(&value, data + i * cell_size, sizeof(float));
      grid.cells.push_back(std::isfinite(value) ? static_cast<int8_t>(std::clamp(value * 100.0f, -128.0f, 127.0f))
                                                : static_cast<int8_t>(-1));
    } else if (occupancy_grid.cell_type() == vlink::zerocopy::OccupancyGrid::kCellUint16 &&
               cell_size >= sizeof(uint16_t)) {
      uint16_t value = 0;
      std::memcpy(&value, data + i * cell_size, sizeof(uint16_t));
      grid.cells.push_back(static_cast<int8_t>(std::min<uint16_t>(value, 127)));
    } else {
      grid.cells.push_back(static_cast<int8_t>(data[i * cell_size]));
    }
  }

  out.grid_valid = !grid.cells.empty();
  return out.grid_valid;
}

bool decode_zerocopy_point_cloud(const vlink::Bytes& raw, Layer& out) {
  vlink::zerocopy::PointCloud pcl;

  if (!(pcl << raw) || !pcl.is_valid()) {
    return false;
  }

  out.type = RenderType::kPointCloud;
  out.cloud.clear();

  vlink::zerocopy::PointCloud::KeyList key_list;
  auto key_map = pcl.get_key_map(&key_list);

  bool has_intensity = false;
  uint16_t intensity_offset = 0;

  for (const auto& key : key_list) {
    if (key.name == "intensity" && key.type == vlink::zerocopy::PointCloud::kFloatType) {
      has_intensity = true;
      intensity_offset = key_map["intensity"];
      break;
    }
  }

  out.has_value_channel = has_intensity;
  out.cloud.reserve(pcl.size());

  vlink::zerocopy::PointCloud::Vector3f v3f;

  for (size_t i = 0; i < pcl.size(); ++i) {
    if (!pcl.get_value_v3f(v3f, i)) {
      continue;
    }

    if (std::isnan(v3f.x) || std::isnan(v3f.y) || std::isnan(v3f.z)) {
      continue;
    }

    CloudPoint point;
    point.x = v3f.x;
    point.y = v3f.y;
    point.z = v3f.z;

    if (has_intensity) {
      float value = 0;
      pcl.get_value<float>(value, i, intensity_offset);
      point.value = value;
    }

    out.cloud.emplace_back(point);
  }

  return !out.cloud.empty();
}

void decode_proto(const google::protobuf::Message& root, const PerceptionConfig::MappingRule& rule, Layer& out) {
  mapping::decode_proto(root, rule, out);
}

void decode_fbs(const flatbuffers::Table& root, const reflection::Schema& schema, const reflection::Object& root_obj,
                const PerceptionConfig::MappingRule& rule, Layer& out) {
  mapping::decode_fbs(root, schema, root_obj, rule, out);
}

void decode_hud_proto(const google::protobuf::Message& root, const PerceptionConfig::MappingRule& rule,
                      std::vector<HudField>& out) {
  mapping::decode_hud_proto(root, rule, out);
}

void decode_hud_fbs(const flatbuffers::Table& root, const reflection::Schema& schema,
                    const reflection::Object& root_obj, const PerceptionConfig::MappingRule& rule,
                    std::vector<HudField>& out) {
  mapping::decode_hud_fbs(root, schema, root_obj, rule, out);
}

}  // namespace decode
}  // namespace perception

// NOLINTEND
