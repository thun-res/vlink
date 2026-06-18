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

#include "sensor_types.h"

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// event_advanced: production-grade Event-model features.
//
// Demonstrates:
//   - detect_subscribers: async notification when subscriber set changes.
//   - publish(msg, force=true): force-send even when no subscriber matched.
//   - Fan-out to multiple subscribers on the same URL.
//   - set_latency_and_lost_enabled: per-sample latency + loss accounting.
//   - SampleLostInfo: cumulative total/lost sample counters.
//
// Typical scenarios: telemetry pipelines that need observability, fault
// detection, or proof-of-life publishing.
int main() {
  static constexpr char kUrl[] = "dds://advanced/sensor";

  vlink::MessageLoop loop;
  loop.set_name("main_loop");
  loop.async_run();

  // detect_subscribers: registers an async observer of subscriber presence.
  // The callback fires on the loop thread whenever the matched-subscriber set
  // transitions empty<->non-empty. This is asynchronous discovery -- the call
  // returns immediately, before the first notification is delivered.
  vlink::Publisher<SensorReading> pub(kUrl);
  pub.attach(&loop);
  pub.detect_subscribers([](bool has) { VLOG_I("[pub] subscribers present: ", has); });
  VLOG_I("[pub] has_subscribers (before): ", pub.has_subscribers());

  // force=true publishes even when no subscriber matched: the sample is
  // serialized and pushed onto the wire regardless. Useful for diagnostic
  // beacons or to keep recordings populated when consumers are absent.
  SensorReading forced{0, -1.0};
  VLOG_I("[pub] normal publish (no subs): ", pub.publish(forced));
  VLOG_I("[pub] forced publish (no subs): ", pub.publish(forced, true));

  // Fan-out: multiple subscribers on the same URL each receive every sample.
  // Atomic counters because each listen() callback runs on the loop thread
  // and main() reads them after wait_for_idle().
  std::atomic<int> count1{0};
  std::atomic<int> count2{0};
  std::atomic<int> count3{0};

  vlink::Subscriber<SensorReading> sub1(kUrl);
  sub1.attach(&loop);
  sub1.listen([&count1](const SensorReading& msg) {
    VLOG_I("[sub1] id=", msg.sensor_id, " value=", msg.value);
    count1.fetch_add(1);
  });

  vlink::Subscriber<SensorReading> sub2(kUrl);
  sub2.attach(&loop);
  sub2.listen([&count2](const SensorReading&) { count2.fetch_add(1); });

  // set_latency_and_lost_enabled(true) MUST be called BEFORE listen(): it
  // allocates the per-sample timing buffers and reconfigures the dispatch
  // path. Calling it after listen() is a no-op (or worse, undefined).
  vlink::Subscriber<SensorReading> sub3(kUrl);
  sub3.attach(&loop);
  sub3.set_latency_and_lost_enabled(true);
  // Callback runs on the loop thread; get_latency() reads the most-recent
  // measurement for this delivery in microseconds.
  sub3.listen([&sub3, &count3](const SensorReading& msg) {
    count3.fetch_add(1);
    VLOG_I("[sub3] id=", msg.sensor_id, " latency=", sub3.get_latency(), "us");
  });

  // Bounded handshake: poll for at least one subscriber, time out at 2s.
  VLOG_I("[pub] wait_for_subscribers: ", pub.wait_for_subscribers(2000ms));

  for (int i = 1; i <= 5; ++i) {
    pub.publish({i, 10.0 + i * 0.1});
    std::this_thread::sleep_for(100ms);
  }

  loop.wait_for_idle(2000);

  // Cumulative loss accounting: total=samples the framework expected to see,
  // lost=samples that never reached this subscriber (queue overflow, transport
  // drop, etc.). Only valid because set_latency_and_lost_enabled(true).
  vlink::SampleLostInfo lost = sub3.get_lost();
  VLOG_I("[sub3] total=", lost.total, " lost=", lost.lost);
  VLOG_I("sub1=", count1.load(), " sub2=", count2.load(), " sub3=", count3.load());

  loop.quit();
  loop.wait_for_quit();

  return 0;
}
