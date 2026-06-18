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

#include <vlink/base/logger.h>
#include <vlink/vlink.h>
#include <vlink/zerocopy/occupancy_grid.h>

#include <chrono>
#include <thread>

#include "grid_producer.h"

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// ---------------------------------------------------------------------------
// producer.cc (occupancy_grid example)
//
// Standalone OccupancyGrid publisher.  Pairs with consumer.cc -- run them in
// two terminals, producer second so the subscriber side is already up.
// Uses dds:// here (network path); switch the URL to shm:// for full
// zero-copy semantics.  The cell data is painted with a deterministic
// circular obstacle pattern (see grid_producer.h) so the consumer-side
// histogram is predictable.
// ---------------------------------------------------------------------------

int main() {
  VLOG_I("=== OccupancyGrid Producer ===");

  vlink::Publisher<vlink::zerocopy::OccupancyGrid> pub("dds://example/zerocopy/occupancy_grid");
  // wait_for_subscribers blocks up to 5s for at least one match.  Without
  // it, the first publishes below would be discarded (no reader yet).
  pub.wait_for_subscribers(5s);

  // 200x200 cell grid at 5cm resolution -> 10m x 10m world footprint
  // anchored at (-5, -5).  This matches the typical ROS local-costmap setup
  // around an ego vehicle.
  grid_producer::GridConfig cfg;
  cfg.width = 200;
  cfg.height = 200;
  cfg.resolution = 0.05F;
  cfg.origin_x = -5.0F;
  cfg.origin_y = -5.0F;
  cfg.obstacle_ratio = 0.25F;
  cfg.freq = 10;
  cfg.channel = 0;

  // 10 grids at ~10 Hz (100ms).  create_test_grid allocates a fresh cell
  // buffer per grid; on shm:// this path would use loan() instead for true
  // zero-copy.  publish() takes ownership of / increments the refcount on
  // the produced grid.
  for (uint32_t seq = 1; seq <= 10; ++seq) {
    auto grid = grid_producer::create_test_grid(cfg, seq);
    VLOG_I("Publishing grid seq=", seq, " size=", grid.size(), " bytes valid_cells=", grid.valid_cell_count());
    pub.publish(grid);
    std::this_thread::sleep_for(100ms);
  }

  std::this_thread::sleep_for(500ms);
  VLOG_I("=== Producer Complete ===");
  return 0;
}
