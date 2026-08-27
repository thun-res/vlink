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
//     in direct mode, where callbacks run inline on the publishing thread.
//   - The wait_for_subscribers handshake before the first publish.
//
// Typical scenarios: in-process telemetry fan-out, sensor stub for unit tests,
// fastest possible smoke test of the Event API.
int main() {
  static constexpr char kUrl[] = "intra://hello/pubsub#direct";
  static constexpr int kMessageCount = 5;

  // The #direct fragment selects inline dispatch, so each publish completes its
  // subscriber callbacks before returning.
  vlink::Subscriber<SensorReading> sub(kUrl);

  int received = 0;
  sub.listen([&received](const SensorReading& msg) {
    VLOG_I("[sub] seq=", msg.sequence, " temp=", msg.temperature);
    received++;
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

  VLOG_I("published=", kMessageCount, " received=", received);

  return 0;
}
