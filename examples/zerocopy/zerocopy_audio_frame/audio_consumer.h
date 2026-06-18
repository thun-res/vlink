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

#ifndef EXAMPLES_ZEROCOPY_ZEROCOPY_AUDIO_FRAME_AUDIO_CONSUMER_H_
#define EXAMPLES_ZEROCOPY_ZEROCOPY_AUDIO_FRAME_AUDIO_CONSUMER_H_

#include <vlink/base/logger.h>
#include <vlink/zerocopy/audio_frame.h>

#include <cstdint>
#include <cstdlib>

// ---------------------------------------------------------------------------
// audio_consumer.h
//
// Inline helpers used by consumer.cc to inspect / validate / measure
// AudioFrame instances received over the wire.  Header-only so the example
// stays a single translation unit per binary.
// ---------------------------------------------------------------------------

namespace audio_consumer {

// Dump the descriptive metadata.  In shm:// zero-copy mode `is_owner` is
// false on the subscriber side because the PCM buffer still lives in the
// producer's pool slot.
static inline void print_frame_info(const vlink::zerocopy::AudioFrame& frame) {
  VLOG_I("  [Audio] seq=", frame.header.seq, " codec=", frame.codec(), " lang=", frame.language(),
         " sr=", frame.sample_rate(), " ch=", frame.num_channels(), " bits=", frame.bit_depth(),
         " samples=", frame.num_samples(), " duration_ns=", frame.duration_ns(), " size=", frame.size(),
         " is_owner=", frame.is_owner());
}

// Cheap structural validation -- guards against truncated transmissions
// and mis-configured publishers.
static inline bool validate_frame(const vlink::zerocopy::AudioFrame& frame) {
  if (!frame.is_valid()) {
    VLOG_W("  [Consumer] Frame is invalid");
    return false;
  }

  if (frame.sample_rate() == 0) {
    VLOG_W("  [Consumer] Frame has zero sample rate");
    return false;
  }

  if (frame.num_samples() == 0) {
    VLOG_W("  [Consumer] Frame has no samples");
    return false;
  }

  if (frame.size() == 0) {
    VLOG_W("  [Consumer] Frame has no PCM payload");
    return false;
  }

  return true;
}

// Compute the peak absolute sample value (proxy for loudness) for a S16 PCM
// frame.  Operates on the raw buffer borrowed from the wire -- no extra
// copy.  Returns 0 for non-S16 formats; a real consumer would dispatch on
// frame.format() and walk through the matching typed pointer.
static inline int32_t compute_peak_s16(const vlink::zerocopy::AudioFrame& frame) {
  if (frame.format() != vlink::zerocopy::AudioFrame::kFormatPcmS16) {
    return 0;
  }

  const int16_t* samples = reinterpret_cast<const int16_t*>(frame.data());
  const size_t sample_count = frame.size() / sizeof(int16_t);

  int32_t peak = 0;

  for (size_t i = 0; i < sample_count; ++i) {
    const int32_t mag = std::abs(static_cast<int32_t>(samples[i]));

    if (mag > peak) {
      peak = mag;
    }
  }

  return peak;
}

}  // namespace audio_consumer

#endif  // EXAMPLES_ZEROCOPY_ZEROCOPY_AUDIO_FRAME_AUDIO_CONSUMER_H_
