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

// Ping side of a latency benchmark.
//
// Demonstrates a half-duplex round-trip timing harness using two raw-Bytes
// pub/sub topics (ping and pong). The transport backend is URL-driven via
// ping_pong_common.h so the same code measures DDS, shared memory, fdbus, qnx
// or SOME/IP. Payload size is configurable (CLI arg). Typical engineering
// scenario: characterising end-to-end latency of a candidate transport for a
// given message size before committing it to a production data flow.

#include <vlink/base/logger.h>
#include <vlink/base/message_loop.h>
#include <vlink/base/utils.h>
#include <vlink/vlink.h>

#include <charconv>
#include <chrono>
#include <string>

#include "../ping_pong_common.h"

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

static constexpr uint32_t kTestInterval = 1000U;

int main(int argc, char* argv[]) {
  uint64_t test_size = 1024;

  if (argc == 2) {
    std::string str(argv[1]);
    auto [p, error] = std::from_chars(str.data(), str.data() + str.size(), test_size);

    if (error != std::errc()) {
      VLOG_W("Invalid args.");
      return 1;
    }
  } else if (argc != 1) {
    VLOG_I("Usage: sample_ping [payload_size]");
    return 1;
  }

  if (test_size == 0) {
    VLOG_W("Invalid args.");
    return 1;
  }

  // MessageLoop hosts the periodic re-send timer. Ctrl+C -> quit() unwinds
  // pub/sub destructors cleanly instead of killing the process mid-publish.
  vlink::MessageLoop message_loop;
  vlink::Utils::register_terminate_signal([&message_loop](int) { message_loop.quit(); });

  // Outbound: raw Bytes payload (no serialization overhead).
  vlink::Publisher<vlink::Bytes> pub(Common::get_ping_url());
  // Inbound: echoed Bytes from the pong process.
  vlink::Subscriber<vlink::Bytes> sub(Common::get_pong_url());

  // start_stamp is written from the timer thread (publisher side) and read from
  // the subscriber's callback thread; an atomic time_point gives us a
  // lock-free, tear-free hand-off across those two threads. steady_clock is
  // chosen because it is monotonic -- system_clock can jump backwards on NTP
  // sync and would produce spurious negative deltas.
  std::atomic<std::chrono::steady_clock::time_point> start_stamp = std::chrono::steady_clock::now();

  // Subscriber callback: fires on the transport worker thread when pong echoes
  // back. The half-RTT (one-way delay) approximation requires the network and
  // host to be roughly symmetric.
  sub.listen([&start_stamp](const vlink::Bytes&) {
    auto duration =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_stamp.load());
    // Round-trip / 2 = one-way (ms).
    double delay = duration.count() / 2000.0;
    CLOG_D("Delay(ms) = %.3lf.", delay);
  });

  // Pre-allocate the payload once. Bytes::create reserves the exact size up
  // front so per-iteration allocation does not pollute the latency measurement
  // with allocator noise.
  vlink::Bytes data = vlink::Bytes::create(test_size);

  auto send_func = [&pub, &data, &start_stamp]() {
    start_stamp = std::chrono::steady_clock::now();
    pub.publish(data);
  };

  // Kick off the first round immediately so we don't wait one interval.
  send_func();

  // Periodic re-send every kTestInterval ms; the timer fires on the MessageLoop
  // thread. Set to kInfinite -- the loop is stopped only by the signal handler.
  vlink::Timer timer;
  timer.attach(&message_loop);
  timer.set_interval(kTestInterval);
  timer.set_loop_count(vlink::Timer::kInfinite);
  timer.start(send_func);

  message_loop.run();
  return 0;
}
