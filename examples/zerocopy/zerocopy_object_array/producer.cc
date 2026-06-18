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
#include <vlink/zerocopy/object_array.h>

#include <chrono>
#include <thread>

#include "array_producer.h"

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// ---------------------------------------------------------------------------
// producer.cc (object_array example)
//
// Standalone ObjectArray publisher.  Pairs with consumer.cc -- run them in
// two terminals, producer second so the subscriber side is already up.
// Uses dds:// here (network path); switch the URL to shm:// for full
// zero-copy semantics.  Each array carries 4 synthetic detections (one car,
// one pedestrian, one cyclist, one parked car) whose pose drifts with the
// sequence number.
// ---------------------------------------------------------------------------

int main() {
  VLOG_I("=== ObjectArray Producer ===");

  vlink::Publisher<vlink::zerocopy::ObjectArray> pub("dds://example/zerocopy/object_array");
  // wait_for_subscribers blocks up to 5s for at least one match.  Without
  // it, the first publishes below would be discarded (no reader yet).
  pub.wait_for_subscribers(5s);

  // 16-slot capacity @ 20 Hz -- typical perception output rate.  Only 4
  // slots are filled each frame so the array also exercises the "logical
  // count < capacity" path.
  array_producer::ArrayConfig cfg;
  cfg.capacity = 16;
  cfg.freq = 20;
  cfg.channel = 0;

  // 10 arrays at ~20 Hz (50ms).  create_test_array allocates a fresh slot
  // buffer per array; on shm:// this path would use loan() instead for
  // true zero-copy.  publish() takes ownership / increments the refcount.
  for (uint32_t seq = 1; seq <= 10; ++seq) {
    auto arr = array_producer::create_test_array(cfg, seq);
    VLOG_I("Publishing array seq=", seq, " count=", arr.count(), " capacity=", arr.capacity());
    pub.publish(arr);
    std::this_thread::sleep_for(50ms);
  }

  std::this_thread::sleep_for(500ms);
  VLOG_I("=== Producer Complete ===");
  return 0;
}
