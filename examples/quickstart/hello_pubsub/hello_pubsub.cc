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
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// Plain trivially-copyable POD: dispatched through vlink's "Standard" serializer
// (raw memcpy), no schema registration required.
struct SensorReading {
  int sequence;
  float temperature;
};

// hello_pubsub: minimal Event-model (pub/sub) walkthrough.
//
// Demonstrates:
//   - vlink::Publisher<T> / vlink::Subscriber<T> on the "intra://" backend
//     (intra-process, lockless single-binary delivery -- the cheapest scheme).
//   - vlink::MessageLoop driven asynchronously (async_run) so that subscriber
//     callbacks run on a worker thread while main() continues publishing.
//   - The "attach before listen" rule and wait_for_subscribers handshake.
//
// Typical scenarios: in-process telemetry fan-out, sensor stub for unit tests,
// fastest possible smoke test of the Event API.
int main() {
  static constexpr char kUrl[] = "intra://hello/pubsub";
  static constexpr int kMessageCount = 5;

  // A MessageLoop is the thread context that delivers subscriber callbacks.
  // async_run() spawns a dedicated worker thread and returns immediately, so
  // main() can keep publishing while the loop drains its queue in parallel.
  // (Use run() instead if you want main() itself to *be* the loop thread.)
  vlink::MessageLoop loop;
  loop.async_run();

  // Construct the subscriber, then attach to a loop BEFORE listen().
  // attach() binds delivery context; calling listen() before attach() would
  // either fall back to an implicit thread or fail to activate.
  vlink::Subscriber<SensorReading> sub(kUrl);
  sub.attach(&loop);

  // Shared counter between the loop thread (writer) and main (reader at end).
  // Atomic is required because the callback runs on the loop worker thread.
  std::atomic<int> received{0};
  // Lambda invoked on the loop thread once per delivered sample.
  sub.listen([&received](const SensorReading& msg) {
    VLOG_I("[sub] seq=", msg.sequence, " temp=", msg.temperature);
    received.fetch_add(1);
  });

  vlink::Publisher<SensorReading> pub(kUrl);
  // Block until at least one matched subscriber is discovered. Without this
  // handshake the very first publish() may race ahead of subscriber setup and
  // be silently dropped on lossy transports.
  pub.wait_for_subscribers();

  for (int i = 1; i <= kMessageCount; ++i) {
    SensorReading msg{i, 22.5F + static_cast<float>(i) * 0.3F};
    pub.publish(msg);
    VLOG_I("[pub] seq=", msg.sequence, " temp=", msg.temperature);
    std::this_thread::sleep_for(50ms);
  }

  // Drain in-flight callbacks: wait until the loop queue has been idle for the
  // given budget (ms). Required because publish() returns immediately while
  // listen() callbacks are still queued on the loop thread.
  loop.wait_for_idle(500);
  VLOG_I("published=", kMessageCount, " received=", received.load());

  // Orderly shutdown: quit() signals the loop to stop accepting work, then
  // wait_for_quit() joins the worker thread before destructors run.
  loop.quit();
  loop.wait_for_quit();

  return 0;
}
