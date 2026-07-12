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
    box.velocity[0] = read("velocity[0]");
    box.velocity[1] = read("velocity[1]");
    box.velocity[2] = read("velocity[2]");
    box.score = read("score");
    box.class_id = static_cast<uint32_t>(read("class_id"));
    box.track_id = static_cast<uint32_t>(read("track_id"));

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

  Grid& grid = out.grid;
  double value = 0.0;
  parser.numeric("width", value);
  grid.width = static_cast<uint32_t>(value);
  parser.numeric("height", value);
  grid.height = static_cast<uint32_t>(value);
  parser.numeric("resolution", grid.resolution);
  parser.numeric("origin_x", grid.origin_x);
  parser.numeric("origin_y", grid.origin_y);
  parser.numeric("origin_z", grid.origin_z);

  const size_t cell_count = parser.collection_size("cells");
  parser.numeric("cell_type", value);
  const auto cell_type = static_cast<uint8_t>(value);

  grid.cells.clear();
  grid.cells.reserve(cell_count);

  for (size_t i = 0; i < cell_count; ++i) {
    if VUNLIKELY (!parser.numeric("cells", i, "value", value)) {
      continue;
    }

    grid.cells.push_back(occupancy_cell_to_int8(cell_type, value));
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
