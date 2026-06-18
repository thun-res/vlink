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

#include <chrono>
#include <thread>

#include "frame_producer.h"

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// ---------------------------------------------------------------------------
// producer.cc (camera_frame example)
//
// Standalone CameraFrame publisher. Pairs with consumer.cc -- run them in
// two terminals, producer second so the subscriber side is already up.
// Uses dds:// here (network path); switch the URL to shm:// for full
// zero-copy semantics. The pixel data is filled with a deterministic
// pattern (see frame_producer.h) so the consumer's checksum is predictable.
// ---------------------------------------------------------------------------

int main() {
  VLOG_I("=== CameraFrame Producer ===");

  vlink::Publisher<vlink::zerocopy::CameraFrame> pub("dds://zerocopy/camera");
  // wait_for_subscribers blocks up to 5s for at least one match. Without
  // it, the first publishes below would be discarded (no reader yet).
  pub.wait_for_subscribers(5s);

  // 320x240 NV12 @ 30Hz, image-stream type "I" (intra) on channel 0.
  // NV12 is the most common Y+UV format for ISP/camera output.
  frame_producer::FrameConfig cfg;
  cfg.width = 320;
  cfg.height = 240;
  cfg.format = vlink::zerocopy::CameraFrame::kFormatNv12;
  cfg.stream = vlink::zerocopy::CameraFrame::kStreamI;
  cfg.freq = 30;
  cfg.channel = 0;

  // 10 frames at ~30 fps (33ms). create_test_frame allocates a fresh
  // buffer per frame; on shm:// this path would use loan() instead for
  // true zero-copy. publish() takes ownership / increments refcount.
  for (uint32_t seq = 1; seq <= 10; ++seq) {
    auto frame = frame_producer::create_test_frame(cfg, seq);
    VLOG_I("Publishing frame seq=", seq, " size=", frame.size(), " bytes");
    pub.publish(frame);
    std::this_thread::sleep_for(33ms);
  }

  std::this_thread::sleep_for(500ms);
  VLOG_I("=== Producer Complete ===");
  return 0;
}
