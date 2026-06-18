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

#ifndef EXAMPLES_ZEROCOPY_ZEROCOPY_OCCUPANCY_GRID_GRID_CONSUMER_H_
#define EXAMPLES_ZEROCOPY_ZEROCOPY_OCCUPANCY_GRID_GRID_CONSUMER_H_

#include <vlink/base/logger.h>
#include <vlink/zerocopy/occupancy_grid.h>

#include <cstdint>

// ---------------------------------------------------------------------------
// grid_consumer.h
//
// Inline helpers used by consumer.cc to inspect / validate / summarise
// OccupancyGrid instances received over the wire.  Header-only so the
// example stays a single translation unit per binary.
// ---------------------------------------------------------------------------

namespace grid_consumer {

// Dump the descriptive metadata.  In shm:// zero-copy mode `is_owner` is
// false on the subscriber side because the cell buffer still lives in the
// producer's pool slot.
static inline void print_grid_info(const vlink::zerocopy::OccupancyGrid& grid) {
  VLOG_I("  [Grid] seq=", grid.header.seq, " id=", grid.map_id(), " ", grid.width(), "x", grid.height(),
         " resolution=", grid.resolution(), "m cell_type=", static_cast<int>(grid.cell_type()), " size=", grid.size(),
         " is_owner=", grid.is_owner());
}

// Cheap structural validation.  Real consumers would also verify that the
// payload size matches width * height * cell_size() exactly and that the
// default_value falls inside [value_min, value_max].
static inline bool validate_grid(const vlink::zerocopy::OccupancyGrid& grid) {
  if (!grid.is_valid()) {
    VLOG_W("  [Consumer] Grid is invalid");
    return false;
  }

  if (grid.width() == 0 || grid.height() == 0) {
    VLOG_W("  [Consumer] Grid has zero dimensions");
    return false;
  }

  if (grid.size() == 0) {
    VLOG_W("  [Consumer] Grid has no cell payload");
    return false;
  }

  return true;
}

// Compute a 3-bucket histogram of cell values (free / occupied / unknown).
// Operates on the raw cell buffer borrowed from the wire -- no additional
// copy.  Returns the histogram via output parameters so the caller can log
// in one statement.
static inline void compute_histogram(const vlink::zerocopy::OccupancyGrid& grid, uint32_t& free_cells,
                                     uint32_t& occupied_cells, uint32_t& unknown_cells) {
  free_cells = 0;
  occupied_cells = 0;
  unknown_cells = 0;

  // For non-int8 cell layouts we punt: the caller should switch on cell_type
  // and walk a typed pointer.  This demo deliberately publishes int8 only.
  if (grid.cell_type() != vlink::zerocopy::OccupancyGrid::kCellInt8) {
    return;
  }

  const int8_t* cells = reinterpret_cast<const int8_t*>(grid.data());
  for (size_t i = 0; i < grid.size(); ++i) {
    const int8_t value = cells[i];

    if (value < 0) {
      ++unknown_cells;
    }

    if (value == 0) {
      ++free_cells;
    }

    if (value > 0) {
      ++occupied_cells;
    }
  }
}

}  // namespace grid_consumer

#endif  // EXAMPLES_ZEROCOPY_ZEROCOPY_OCCUPANCY_GRID_GRID_CONSUMER_H_
