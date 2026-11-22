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

#include <vlink/zerocopy/message_parser.h>
#include <vlink/zerocopy/point_cloud.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

#include "./perception_mapping.h"

namespace perception {
namespace decode {

bool decode_zerocopy_object_array(const vlink::Bytes& raw, Layer& out) {
  vlink::zerocopy::MessageParser parser;

  if VUNLIKELY (!parser.parse(vlink::zerocopy::MessageParser::Type::kObjectArray, raw)) {
    return false;
  }

  out.type = RenderType::kObjectDetection;
  out.boxes.clear();
  const size_t count = parser.collection_size("objects");
  out.boxes.reserve(count);

  for (size_t i = 0; i < count; ++i) {
    auto read = [&parser, i](std::string_view field) {
      double value = 0.0;
      parser.numeric("objects", i, field, value);
      return value;
    };

    BoxObject box;
    box.position[0] = read("position[0]");
    box.position[1] = read("position[1]");
    box.position[2] = read("position[2]");
    box.size[0] = read("size[0]");
    box.size[1] = read("size[1]");
    box.size[2] = read("size[2]");
    box.yaw = read("yaw");

    if (!std::isfinite(box.position[0]) || !std::isfinite(box.position[1]) || !std::isfinite(box.position[2]) ||
        !std::isfinite(box.size[0]) || !std::isfinite(box.size[1]) || !std::isfinite(box.size[2]) ||
        !std::isfinite(box.yaw)) {
      continue;
    }

    box.velocity[0] = read("velocity[0]");
    box.velocity[1] = read("velocity[1]");
    box.velocity[2] = read("velocity[2]");
    box.score = read("score");

    const double class_id = read("class_id");
    const double track_id = read("track_id");
    box.class_id = (std::isfinite(class_id) && class_id >= 0.0 &&
                    class_id <= static_cast<double>(std::numeric_limits<uint32_t>::max()))
                       ? static_cast<uint32_t>(class_id)
                       : 0;
    box.track_id = (std::isfinite(track_id) && track_id >= 0.0 &&
                    track_id <= static_cast<double>(std::numeric_limits<uint32_t>::max()))
                       ? static_cast<uint32_t>(track_id)
                       : 0;

    parser.text("objects", i, "label", box.label);

    out.boxes.emplace_back(std::move(box));
  }

  return true;
}

static int8_t occupancy_cell_to_int8(uint8_t cell_type, double value) {
  switch (cell_type) {
    case vlink::zerocopy::OccupancyGrid::kCellUint8:
      if (value >= 255.0) {
        return -1;
      }

      return static_cast<int8_t>(std::clamp<int64_t>(std::llround(value * 100.0 / 254.0), 0, 100));
    case vlink::zerocopy::OccupancyGrid::kCellUint16:
      if (value >= 65535.0) {
        return -1;
      }

      return static_cast<int8_t>(std::clamp<int64_t>(std::llround(value * 100.0 / 65534.0), 0, 100));
    case vlink::zerocopy::OccupancyGrid::kCellFloat32:
      return std::isfinite(value) ? static_cast<int8_t>(std::clamp(value * 100.0, -128.0, 127.0))
                                  : static_cast<int8_t>(-1);
    default:
      return std::isfinite(value) ? static_cast<int8_t>(std::clamp(value, -128.0, 127.0)) : static_cast<int8_t>(-1);
  }
}

bool decode_zerocopy_occupancy_grid(const vlink::Bytes& raw, Layer& out) {
  vlink::zerocopy::MessageParser parser;

  if VUNLIKELY (!parser.parse(vlink::zerocopy::MessageParser::Type::kOccupancyGrid, raw)) {
    return false;
  }

  out.type = RenderType::kOccupancyGrid;
  out.grid_valid = false;

  Grid& grid = out.grid;
  double width = 0.0;
  double height = 0.0;

  if (!parser.numeric("width", width) || !parser.numeric("height", height) ||
      !parser.numeric("resolution", grid.resolution) || !parser.numeric("origin_x", grid.origin_x) ||
      !parser.numeric("origin_y", grid.origin_y) || !parser.numeric("origin_z", grid.origin_z) || width < 1.0 ||
      height < 1.0 || !std::isfinite(grid.resolution) || grid.resolution <= 0.0 || !std::isfinite(grid.origin_x) ||
      !std::isfinite(grid.origin_y) || !std::isfinite(grid.origin_z)) {
    return false;
  }

  grid.width = static_cast<uint32_t>(width);
  grid.height = static_cast<uint32_t>(height);

  const size_t cell_count = parser.collection_size("cells");
  const bool valid_dimensions = grid.width <= std::numeric_limits<size_t>::max() / grid.height;

  if (!valid_dimensions || cell_count != static_cast<size_t>(grid.width) * grid.height) {
    return false;
  }

  double value = 0.0;

  if (!parser.numeric("cell_type", value)) {
    return false;
  }

  const auto cell_type = static_cast<uint8_t>(value);

  grid.cells.clear();
  grid.cells.reserve(cell_count);

  for (size_t i = 0; i < cell_count; ++i) {
    if VUNLIKELY (!parser.numeric("cells", i, "value", value)) {
      continue;
    }

    grid.cells.push_back(occupancy_cell_to_int8(cell_type, value));
  }

  out.grid_valid = grid.cells.size() == cell_count;
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
  vlink::zerocopy::PointCloud::Key intensity_key;

  for (const auto& key : key_list) {
    if (key.name == "intensity" && key.size > 0) {
      const auto iter = key_map.find(key.name);

      if (iter != key_map.end()) {
        has_intensity = true;
        intensity_offset = iter->second;
        intensity_key = key;
      }

      break;
    }
  }

  if (key_list.size() < 3) {
    return false;
  }

  auto read_key_as_double = [&](size_t point_idx, const vlink::zerocopy::PointCloud::Key& key,
                                uint16_t offset) -> double {
    if (key.type == vlink::zerocopy::PointCloud::kUnknownType) {
      if (key.size == 1) {
        return pcl.get_value<uint8_t>(point_idx, offset);
      } else if (key.size == 2) {
        return pcl.get_value<int16_t>(point_idx, offset);
      } else if (key.size == 4) {
        return pcl.get_value<float>(point_idx, offset);
      } else if (key.size == 8) {
        return pcl.get_value<double>(point_idx, offset);
      }

      return 0.0;
    }

    return pcl.get_value_for_double_float(point_idx, offset, key.type);
  };

  const bool has_compressed_xyz = (pcl.get_extent() != 0);
  const bool is_double_xyz = !has_compressed_xyz && key_list[0].type == vlink::zerocopy::PointCloud::kDoubleType &&
                             key_list[1].type == vlink::zerocopy::PointCloud::kDoubleType &&
                             key_list[2].type == vlink::zerocopy::PointCloud::kDoubleType &&
                             key_list[0].size == sizeof(double) && key_list[1].size == sizeof(double) &&
                             key_list[2].size == sizeof(double) && pcl.pack_size() >= sizeof(double) * 3;
  const bool is_float_xyz =
      has_compressed_xyz || ((key_list[0].type == vlink::zerocopy::PointCloud::kFloatType ||
                              key_list[0].type == vlink::zerocopy::PointCloud::kUnknownType) &&
                             (key_list[1].type == vlink::zerocopy::PointCloud::kFloatType ||
                              key_list[1].type == vlink::zerocopy::PointCloud::kUnknownType) &&
                             (key_list[2].type == vlink::zerocopy::PointCloud::kFloatType ||
                              key_list[2].type == vlink::zerocopy::PointCloud::kUnknownType) &&
                             key_list[0].size == sizeof(float) && key_list[1].size == sizeof(float) &&
                             key_list[2].size == sizeof(float) && pcl.pack_size() >= sizeof(float) * 3);

  uint16_t xyz_offsets[3] = {0, 0, 0};

  if (!is_double_xyz && !is_float_xyz) {
    for (size_t axis = 0; axis < 3; ++axis) {
      const auto iter = key_map.find(key_list[axis].name);

      if (iter == key_map.end() || key_list[axis].size == 0) {
        return false;
      }

      xyz_offsets[axis] = iter->second;
    }
  }

  out.has_value_channel = has_intensity;
  out.cloud.reserve(pcl.size());

  vlink::zerocopy::PointCloud::Vector3f v3f;
  vlink::zerocopy::PointCloud::Vector3d v3d;

  for (size_t i = 0; i < pcl.size(); ++i) {
    CloudPoint point;

    if (is_double_xyz) {
      if (!pcl.get_value_v3d(v3d, i)) {
        continue;
      }

      point.x = v3d.x;
      point.y = v3d.y;
      point.z = v3d.z;
    } else if (is_float_xyz) {
      if (!pcl.get_value_v3f(v3f, i)) {
        continue;
      }

      point.x = v3f.x;
      point.y = v3f.y;
      point.z = v3f.z;
    } else {
      point.x = read_key_as_double(i, key_list[0], xyz_offsets[0]);
      point.y = read_key_as_double(i, key_list[1], xyz_offsets[1]);
      point.z = read_key_as_double(i, key_list[2], xyz_offsets[2]);
    }

    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) {
      continue;
    }

    if (has_intensity) {
      const double value = read_key_as_double(i, intensity_key, intensity_offset);

      if (!std::isfinite(value)) {
        continue;
      }

      point.value = value;
    }

    out.cloud.emplace_back(point);
  }

  return !out.cloud.empty();
}

bool decode_zerocopy_mapping(const vlink::Bytes& raw, const std::string& ser, const PerceptionConfig::MappingRule& rule,
                             Layer& out) {
  return mapping::decode_zerocopy(raw, ser, rule, out);
}

bool decode_hud_zerocopy(const vlink::Bytes& raw, const std::string& ser, const PerceptionConfig::MappingRule& rule,
                         std::vector<HudField>& out) {
  return mapping::decode_hud_zerocopy(raw, ser, rule, out);
}

bool decode_zerocopy_batch(const vlink::Bytes& raw, const std::string& ser,
                           const std::vector<PerceptionConfig::MappingRule>& mappings,
                           const std::vector<PerceptionConfig::MappingRule>& hud_bindings, std::vector<Layer>& layers,
                           std::vector<std::vector<HudField>>& hud_fields) {
  return mapping::decode_zerocopy_batch(raw, ser, mappings, hud_bindings, layers, hud_fields);
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
