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

#include <atomic>
#include <chrono>
#include <thread>

#include "array_consumer.h"

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// ---------------------------------------------------------------------------
// consumer.cc (object_array example)
//
// Standalone ObjectArray subscriber.  Start it first, then run producer.cc.
// Pulls arrays off the loop, runs structural validation and prints every
// Object record, and exits once 10 arrays have arrived or after ~15s.  The
// atomic counter crosses thread boundaries (subscriber dispatch -> main
// poll loop).
// ---------------------------------------------------------------------------

int main() {
  VLOG_I("=== ObjectArray Consumer ===");
  VLOG_I("Waiting for producer (run example_object_array_producer in another terminal)");

  vlink::MessageLoop loop;
  loop.set_name("object_array_loop");
  loop.async_run();

  std::atomic<int> received{0};

  vlink::Subscriber<vlink::zerocopy::ObjectArray> sub("dds://example/zerocopy/object_array");
  sub.attach(&loop);
  // Listener fires on `loop`'s thread.  `arr` is a non-owning view (in
  // shm:// it points at the producer's pool slot); treat as read-only.
  sub.listen([&received](const vlink::zerocopy::ObjectArray& arr) {
    received++;

    if (!array_consumer::validate_array(arr)) {
      return;
    }

    array_consumer::print_array_info(arr);
    array_consumer::print_objects(arr);
  });

  // Bounded poll -- at most 150 * 100ms = 15s.  Early exit once we've seen
  // the expected 10 arrays so CI doesn't waste time.
  for (int i = 0; i < 150; ++i) {
    std::this_thread::sleep_for(100ms);

    if (received >= 10) {
      break;
    }
  }

  VLOG_I("Total arrays received: ", received.load());

  loop.quit();
  loop.wait_for_quit();

  VLOG_I("=== Consumer Complete ===");
  return 0;
}
