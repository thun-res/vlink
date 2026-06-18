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
#include <vlink/zerocopy/audio_frame.h>

#include <chrono>
#include <thread>

#include "audio_producer.h"

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// ---------------------------------------------------------------------------
// producer.cc (audio_frame example)
//
// Standalone AudioFrame publisher.  Pairs with consumer.cc -- run them in
// two terminals, producer second so the subscriber side is already up.
// Uses dds:// here (network path); switch the URL to shm:// for full
// zero-copy semantics.  Each frame carries 20 ms of 48 kHz stereo S16 PCM
// containing two sinusoids (440 Hz left, 660 Hz right) -- continuous in
// phase across frames so the consumer-side peak measurement is stable.
// ---------------------------------------------------------------------------

int main() {
  VLOG_I("=== AudioFrame Producer ===");

  vlink::Publisher<vlink::zerocopy::AudioFrame> pub("dds://example/zerocopy/audio_frame");
  // wait_for_subscribers blocks up to 5s for at least one match.  Without
  // it, the first publishes below would be discarded (no reader yet).
  pub.wait_for_subscribers(5s);

  // 48 kHz / stereo / 960 samples = 20 ms frame.  This is the canonical
  // Opus / WebRTC frame size and a common in-vehicle infotainment unit.
  audio_producer::AudioConfig cfg;
  cfg.sample_rate = 48000;
  cfg.num_channels = 2;
  cfg.num_samples = 960;
  cfg.bit_depth = 16;
  cfg.tone_left_hz = 440.0F;
  cfg.tone_right_hz = 660.0F;
  cfg.freq = 50;
  cfg.channel = 0;

  // 10 frames at the natural 20 ms cadence (50 fps).  create_test_frame
  // allocates a fresh buffer per frame; on shm:// this path would use loan()
  // instead for true zero-copy.  publish() takes ownership of / increments
  // the refcount on the produced frame.
  for (uint32_t seq = 1; seq <= 10; ++seq) {
    auto frame = audio_producer::create_test_frame(cfg, seq);
    VLOG_I("Publishing audio seq=", seq, " size=", frame.size(), " bytes duration_ns=", frame.duration_ns());
    pub.publish(frame);
    std::this_thread::sleep_for(20ms);
  }

  std::this_thread::sleep_for(500ms);
  VLOG_I("=== Producer Complete ===");
  return 0;
}
