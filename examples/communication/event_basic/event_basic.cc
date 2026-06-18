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

#include <atomic>
#include <chrono>

#include "sensor_types.h"

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// event_basic: Event model over DDS with a periodic Timer and signal handling.
//
// Demonstrates:
//   - vlink::Publisher / vlink::Subscriber on the "dds://" backend
//     (cross-process, network-capable).
//   - vlink::MessageLoop driven *synchronously* via run() -- main() itself
//     becomes the loop thread (no extra worker needed).
//   - vlink::Timer scheduled on the loop for periodic publication.
//   - vlink::Utils::register_terminate_signal for clean Ctrl+C shutdown.
//
// Typical scenarios: long-running sensor producer, daemon-style publishers.
int main() {
  static constexpr char kUrl[] = "dds://sensor/temperature";
  static constexpr int kMaxPublish = 10;

  vlink::MessageLoop loop;
  loop.set_name("main_loop");

  // Install SIGINT/SIGTERM handler. The lambda is invoked from signal context
  // but only calls loop.quit(), which is signal-safe (thread-safe atomic flag).
  vlink::Utils::register_terminate_signal([&loop](int) { loop.quit(); });

  vlink::Subscriber<SensorData> sub(kUrl);
  // Bind subscriber dispatch to the loop BEFORE listen(); see hello_pubsub
  // for the "attach-before-listen" rationale.
  sub.attach(&loop);

  // Atomic counter shared with the loop-thread callback below.
  std::atomic<int> received{0};
  // Fires on the loop thread once per delivered sample.
  sub.listen([&received](const SensorData& data) {
    VLOG_I("[sub] id=", data.id, " value=", data.value, " ts=", data.timestamp);
    received.fetch_add(1);
  });

  vlink::Publisher<SensorData> pub(kUrl);
  // Block until at least one subscriber is matched -- avoids losing the first
  // few samples on transports that drop pre-discovery traffic.
  pub.wait_for_subscribers();

  // Periodic timer attached to the loop: callback fires on the loop thread
  // every 500ms indefinitely (kInfinite). Calling loop.quit() from the
  // callback when the budget is reached cleanly stops both the timer and
  // the run() call below.
  std::atomic<int> published{0};
  vlink::Timer timer(&loop, 500, vlink::Timer::kInfinite, [&pub, &published, &loop]() {
    int seq = published.fetch_add(1) + 1;

    if (seq > kMaxPublish) {
      loop.quit();
      return;
    }

    SensorData data{};
    data.id = seq;
    data.value = 20.0F + static_cast<float>(seq) * 0.5F;
    data.timestamp = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());

    pub.publish(data);
    VLOG_I("[pub] #", seq, " value=", data.value);
  });
  timer.start();

  // Synchronous run(): turns *this* thread into the loop thread. Blocks until
  // either the timer callback or the signal handler calls loop.quit().
  // (Contrast hello_pubsub which uses async_run() + manual main work.)
  loop.run();

  VLOG_I("published=", published.load(), " received=", received.load());

  return 0;
}
