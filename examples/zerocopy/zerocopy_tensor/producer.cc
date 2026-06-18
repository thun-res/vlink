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

#include <chrono>
#include <thread>

#include "tensor_producer.h"

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// ---------------------------------------------------------------------------
// producer.cc (tensor example)
//
// Standalone Tensor publisher.  Pairs with consumer.cc -- run them in two
// terminals, producer second so the subscriber side is already up.  Uses
// dds:// here (network path); switch the URL to shm:// for full zero-copy
// semantics.  Each tensor is a 1x3x224x224 NCHW float32 image filled with a
// smooth sin/cos lattice so the consumer-side statistics (min / max / mean)
// are predictable.
// ---------------------------------------------------------------------------

int main() {
  VLOG_I("=== Tensor Producer ===");

  vlink::Publisher<vlink::zerocopy::Tensor> pub("dds://example/zerocopy/tensor");
  // wait_for_subscribers blocks up to 5s for at least one match.  Without
  // it, the first publishes below would be discarded (no reader yet).
  pub.wait_for_subscribers(5s);

  // 1x3x224x224 NCHW @ 30Hz -- the canonical ImageNet input feed.
  tensor_producer::TensorConfig cfg;
  cfg.batch = 1;
  cfg.channels = 3;
  cfg.height = 224;
  cfg.width = 224;
  cfg.freq = 30;
  cfg.channel = 0;

  // 10 tensors at ~30 fps (33ms).  create_test_tensor allocates a fresh
  // buffer per tensor; on shm:// this path would use loan() instead for
  // true zero-copy.  publish() takes ownership / increments the refcount.
  for (uint32_t seq = 1; seq <= 10; ++seq) {
    auto tensor = tensor_producer::create_test_tensor(cfg, seq);
    VLOG_I("Publishing tensor seq=", seq, " size=", tensor.size(), " bytes num_elements=", tensor.num_elements());
    pub.publish(tensor);
    std::this_thread::sleep_for(33ms);
  }

  std::this_thread::sleep_for(500ms);
  VLOG_I("=== Producer Complete ===");
  return 0;
}
