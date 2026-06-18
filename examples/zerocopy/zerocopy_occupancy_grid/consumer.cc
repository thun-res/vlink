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

#include <atomic>
#include <chrono>
#include <thread>

#include "grid_consumer.h"

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// ---------------------------------------------------------------------------
// consumer.cc (occupancy_grid example)
//
// Standalone OccupancyGrid subscriber.  Start it first, then run producer.cc.
// Pulls grids off the loop, runs structural validation, computes a free /
// occupied / unknown histogram, and exits once 10 grids have arrived or
// after ~15s.  The atomic counter crosses thread boundaries (subscriber
// dispatch -> main poll loop).
// ---------------------------------------------------------------------------

int main() {
  VLOG_I("=== OccupancyGrid Consumer ===");
  VLOG_I("Waiting for producer (run example_occupancy_grid_producer in another terminal)");

  vlink::MessageLoop loop;
  loop.set_name("occupancy_grid_loop");
  loop.async_run();

  std::atomic<int> received{0};

  vlink::Subscriber<vlink::zerocopy::OccupancyGrid> sub("dds://example/zerocopy/occupancy_grid");
  sub.attach(&loop);
  // Listener fires on `loop`'s thread.  `grid` is a non-owning view (in
  // shm:// it points at the producer's pool slot); treat as read-only.
  sub.listen([&received](const vlink::zerocopy::OccupancyGrid& grid) {
    received++;

    if (!grid_consumer::validate_grid(grid)) {
      return;
    }

    grid_consumer::print_grid_info(grid);

    uint32_t free_cells = 0;
    uint32_t occupied_cells = 0;
    uint32_t unknown_cells = 0;
    grid_consumer::compute_histogram(grid, free_cells, occupied_cells, unknown_cells);
    VLOG_I("  histogram free=", free_cells, " occupied=", occupied_cells, " unknown=", unknown_cells);
  });

  // Bounded poll -- at most 150 * 100ms = 15s.  Early exit once we've seen
  // the expected 10 grids so CI doesn't waste time.
  for (int i = 0; i < 150; ++i) {
    std::this_thread::sleep_for(100ms);

    if (received >= 10) {
      break;
    }
  }

  VLOG_I("Total grids received: ", received.load());

  loop.quit();
  loop.wait_for_quit();

  VLOG_I("=== Consumer Complete ===");
  return 0;
}
