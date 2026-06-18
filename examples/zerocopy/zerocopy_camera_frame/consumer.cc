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
#include <vlink/zerocopy/camera_frame.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "frame_consumer.h"

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// ---------------------------------------------------------------------------
// consumer.cc (camera_frame example)
//
// Standalone CameraFrame subscriber. Start it first, then run producer.cc.
// Pulls frames off the loop, runs structural validation + checksum, and
// exits once 10 frames have arrived or after ~15s. The atomic counter
// crosses thread boundaries (subscriber dispatch -> main poll loop).
// ---------------------------------------------------------------------------

int main() {
  VLOG_I("=== CameraFrame Consumer ===");
  VLOG_I("Waiting for producer (run example_camera_producer in another terminal)");

  vlink::MessageLoop loop;
  loop.set_name("camera_loop");
  loop.async_run();

  std::atomic<int> received{0};

  vlink::Subscriber<vlink::zerocopy::CameraFrame> sub("dds://zerocopy/camera");
  sub.attach(&loop);
  // Listener fires on `loop`'s thread. `frame` is a non-owning view (in
  // shm:// it points at the producer's pool slot); treat as read-only.
  sub.listen([&received](const vlink::zerocopy::CameraFrame& frame) {
    received++;

    if (!frame_consumer::validate_frame(frame)) {
      return;
    }

    frame_consumer::print_frame_info(frame);
    VLOG_I("  checksum=", frame_consumer::compute_checksum(frame));
  });

  // Bounded poll -- at most 150 * 100ms = 15s. Early exit once we've seen
  // the expected 10 frames so CI doesn't waste time.
  for (int i = 0; i < 150; ++i) {
    std::this_thread::sleep_for(100ms);

    if (received >= 10) {
      break;
    }
  }

  VLOG_I("Total frames received: ", received.load());

  loop.quit();
  loop.wait_for_quit();

  VLOG_I("=== Consumer Complete ===");
  return 0;
}
