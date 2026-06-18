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

#ifndef EXAMPLES_ZEROCOPY_ZEROCOPY_OCCUPANCY_GRID_GRID_PRODUCER_H_
#define EXAMPLES_ZEROCOPY_ZEROCOPY_OCCUPANCY_GRID_GRID_PRODUCER_H_

#include <vlink/zerocopy/occupancy_grid.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// grid_producer.h
//
// Helper for the occupancy_grid example: builds a synthetic 2-D occupancy
// grid filled with a circular obstacle pattern (a 0/100/-1 ROS-style map).
// The mapping mirrors the ROS nav_msgs/OccupancyGrid convention exactly so
// the produced grids can be replayed against rviz / Foxglove without
// post-processing:
//   *   0   -> free cell (probability < free_threshold).
//   * 100   -> occupied cell (probability > occupied_threshold).
//   *  -1   -> unknown cell (default_value).
// Cell storage uses int8 (kCellInt8) so each cell consumes exactly one byte
// and the wire payload size matches width * height.
// ---------------------------------------------------------------------------

namespace grid_producer {

struct GridConfig {
  uint32_t width;        // Grid width in cells (columns).
  uint32_t height;       // Grid height in cells (rows).
  float resolution;      // Metres per cell.
  float origin_x;        // World-frame X of the bottom-left corner.
  float origin_y;        // World-frame Y of the bottom-left corner.
  float obstacle_ratio;  // Radius of the centre obstacle relative to width.
  uint32_t freq;         // Nominal publish frequency in Hz.
  uint32_t channel;      // Sensor / producer channel identifier.
};

// Build one OccupancyGrid: configure metadata, allocate the int8 cell buffer,
// then paint a circular obstacle ring around the centre.  The radius shifts
// slightly with `seq` so successive frames are not bit-identical and the
// consumer-side checksum varies frame-to-frame (useful for spot-checking
// pipeline integrity without uploading large fixtures).
static inline vlink::zerocopy::OccupancyGrid create_test_grid(const GridConfig& cfg, uint32_t seq) {
  vlink::zerocopy::OccupancyGrid grid;

  // Static map identifier baked into the header.  Multiple maps (local /
  // global / lane-level) commonly share the same topic; map_id lets consumers
  // disambiguate without inspecting cells.
  grid.set_map_id("demo_local_map");
  grid.set_channel(cfg.channel);
  grid.set_freq(cfg.freq);

  // Map dimensions and world-to-grid transform.  We anchor (0, 0) of the grid
  // at the bottom-left corner exactly as REP-105 specifies.
  grid.set_width(cfg.width);
  grid.set_height(cfg.height);
  grid.set_resolution(cfg.resolution);
  grid.set_origin_x(cfg.origin_x);
  grid.set_origin_y(cfg.origin_y);
  grid.set_origin_z(0.0F);
  grid.set_origin_yaw(0.0F);

  // ROS-style int8 occupancy: -1 unknown, 0 free, 100 occupied.
  grid.set_cell_type(vlink::zerocopy::OccupancyGrid::kCellInt8);
  grid.set_default_value(-1);
  grid.set_value_min(-1.0F);
  grid.set_value_max(100.0F);
  grid.set_occupied_threshold(0.65F);
  grid.set_free_threshold(0.20F);

  // Track sequence number on the standard Header so the consumer can detect
  // drops or reordering.
  grid.header.seq = seq;

  // Allocate width * height bytes of cell storage (1 byte per kCellInt8).
  const size_t total_cells = static_cast<size_t>(cfg.width) * static_cast<size_t>(cfg.height);
  grid.create(total_cells);

  // Paint the synthetic pattern.  The buffer is mutable through the owned
  // pointer; we drop const to write cells directly without an extra copy.
  int8_t* cells = reinterpret_cast<int8_t*>(const_cast<uint8_t*>(grid.data()));

  // Circular obstacle ring centred on the map.  Radius oscillates with seq
  // so frames remain visually distinguishable.
  const float cx = static_cast<float>(cfg.width) * 0.5F;
  const float cy = static_cast<float>(cfg.height) * 0.5F;
  const float base_radius = static_cast<float>(cfg.width) * cfg.obstacle_ratio;
  const float radius = base_radius + static_cast<float>(seq % 5);
  const float ring_thickness = 2.5F;

  // Track how many cells end up as non-default; passed via valid_cell_count
  // as a sparsity hint for consumers (e.g. costmap fusion).
  uint32_t valid_count = 0;

  for (uint32_t row = 0; row < cfg.height; ++row) {
    for (uint32_t col = 0; col < cfg.width; ++col) {
      const float dx = static_cast<float>(col) - cx;
      const float dy = static_cast<float>(row) - cy;
      const float dist = std::sqrt(dx * dx + dy * dy);

      int8_t value = -1;  // Default: unknown.

      if (dist < radius - ring_thickness) {
        // Inside the ring -> free space.
        value = 0;
        ++valid_count;
      }

      if (dist >= radius - ring_thickness && dist <= radius + ring_thickness) {
        // On the ring -> occupied obstacle.
        value = 100;
        ++valid_count;
      }

      cells[row * cfg.width + col] = value;
    }
  }

  grid.set_valid_cell_count(valid_count);

  return grid;
}

}  // namespace grid_producer

#endif  // EXAMPLES_ZEROCOPY_ZEROCOPY_OCCUPANCY_GRID_GRID_PRODUCER_H_
