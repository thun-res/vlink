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
#include <vlink/zerocopy/tensor.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "tensor_consumer.h"

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// ---------------------------------------------------------------------------
// consumer.cc (tensor example)
//
// Standalone Tensor subscriber.  Start it first, then run producer.cc.
// Pulls tensors off the loop, runs structural validation + per-tensor
// min/max/mean statistics, and exits once 10 tensors have arrived or after
// ~15s.  The atomic counter crosses thread boundaries (subscriber dispatch
// -> main poll loop).
// ---------------------------------------------------------------------------

int main() {
  VLOG_I("=== Tensor Consumer ===");
  VLOG_I("Waiting for producer (run example_tensor_producer in another terminal)");

  vlink::MessageLoop loop;
  loop.set_name("tensor_loop");
  loop.async_run();

  std::atomic<int> received{0};

  vlink::Subscriber<vlink::zerocopy::Tensor> sub("dds://example/zerocopy/tensor");
  sub.attach(&loop);
  // Listener fires on `loop`'s thread.  `tensor` is a non-owning view (in
  // shm:// it points at the producer's pool slot); treat as read-only.
  sub.listen([&received](const vlink::zerocopy::Tensor& tensor) {
    received++;

    if (!tensor_consumer::validate_tensor(tensor)) {
      return;
    }

    tensor_consumer::print_tensor_info(tensor);

    float min_v = 0.0F;
    float max_v = 0.0F;
    float mean_v = 0.0F;
    tensor_consumer::compute_stats(tensor, min_v, max_v, mean_v);
    VLOG_I("  stats min=", min_v, " max=", max_v, " mean=", mean_v);
  });

  // Bounded poll -- at most 150 * 100ms = 15s.  Early exit once we've seen
  // the expected 10 tensors so CI doesn't waste time.
  for (int i = 0; i < 150; ++i) {
    std::this_thread::sleep_for(100ms);

    if (received >= 10) {
      break;
    }
  }

  VLOG_I("Total tensors received: ", received.load());

  loop.quit();
  loop.wait_for_quit();

  VLOG_I("=== Consumer Complete ===");
  return 0;
}
