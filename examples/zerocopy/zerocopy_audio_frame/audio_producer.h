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

#ifndef EXAMPLES_ZEROCOPY_ZEROCOPY_AUDIO_FRAME_AUDIO_PRODUCER_H_
#define EXAMPLES_ZEROCOPY_ZEROCOPY_AUDIO_FRAME_AUDIO_PRODUCER_H_

#include <vlink/zerocopy/audio_frame.h>

#include <cmath>
#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------
// audio_producer.h
//
// Helper for the audio_frame example: builds a synthetic 48 kHz stereo PCM
// frame containing 960 samples per channel -- the canonical 20 ms frame
// size used by Opus / WebRTC / in-vehicle infotainment pipelines.  Two
// sinusoids (440 Hz on the left channel, 660 Hz on the right channel) are
// rendered into interleaved int16 samples so the resulting buffer matches
// what a real microphone capture stage would emit after format conversion.
//
// Sizing: 960 samples * 2 channels * 2 bytes = 3840 bytes per frame.
// At 50 fps (the 20 ms frame rate) that is exactly 192 KB/s -- the
// uncompressed bandwidth of CD-quality stereo.
// ---------------------------------------------------------------------------

namespace audio_producer {

struct AudioConfig {
  uint32_t sample_rate;   // Hz, e.g. 48000.
  uint16_t num_channels;  // 1 = mono, 2 = stereo.
  uint32_t num_samples;   // Samples per channel per frame.
  uint16_t bit_depth;     // PCM bit depth (16 / 24 / 32).
  float tone_left_hz;     // Tone frequency on channel 0 (Hz).
  float tone_right_hz;    // Tone frequency on channel 1 (Hz, ignored when mono).
  uint32_t freq;          // Nominal publish frequency in Hz.
  uint32_t channel;       // Microphone / sensor channel identifier.
};

// Build one AudioFrame: configure metadata, allocate the PCM buffer, then
// render two interleaved sinusoids.  Phase is anchored to `seq` so the
// signal is continuous frame-to-frame -- a real microphone would do the
// same via a free-running sample counter.
static inline vlink::zerocopy::AudioFrame create_test_frame(const AudioConfig& cfg, uint32_t seq) {
  vlink::zerocopy::AudioFrame frame;

  // Format / layout: signed 16-bit linear PCM, samples interleaved across
  // channels (L, R, L, R, ...).  This is the most common format produced by
  // ALSA / CoreAudio / WASAPI capture callbacks.
  frame.set_format(vlink::zerocopy::AudioFrame::kFormatPcmS16);
  frame.set_layout(vlink::zerocopy::AudioFrame::kLayoutInterleaved);
  frame.set_codec("PCM");
  frame.set_language("en");

  frame.set_sample_rate(cfg.sample_rate);
  frame.set_num_channels(cfg.num_channels);
  frame.set_num_samples(cfg.num_samples);
  frame.set_bit_depth(cfg.bit_depth);

  // Uncompressed -- bitrate field is documentation-only here.
  frame.set_bitrate(0);

  // Frame duration in nanoseconds (num_samples / sample_rate * 1e9).
  const uint64_t duration_ns =
      static_cast<uint64_t>(cfg.num_samples) * 1000000000ULL / static_cast<uint64_t>(cfg.sample_rate);
  frame.set_duration_ns(duration_ns);

  // Channel + freq belong to the standard transport metadata triplet
  // (channel, freq, header.seq).
  frame.set_channel(cfg.channel);
  frame.set_freq(cfg.freq);
  frame.header.seq = seq;

  // Allocate num_samples * num_channels * sizeof(int16_t) bytes.
  const size_t total_bytes =
      static_cast<size_t>(cfg.num_samples) * static_cast<size_t>(cfg.num_channels) * sizeof(int16_t);
  frame.create(total_bytes);

  // Continuous-phase sinusoid -- start sample index for this frame is
  // (seq - 1) * num_samples.  Producing identical phase as a real audio
  // pipeline keeps the consumer's frequency analysis stable.
  int16_t* samples = reinterpret_cast<int16_t*>(const_cast<uint8_t*>(frame.data()));
  const uint64_t base_sample = static_cast<uint64_t>(seq - 1) * static_cast<uint64_t>(cfg.num_samples);
  const float two_pi = 6.28318530718F;
  const float inv_sr = 1.0F / static_cast<float>(cfg.sample_rate);

  // Amplitude well below INT16_MAX to avoid any chance of clipping when the
  // consumer applies gain (= 0.5 * full-scale).
  const float amplitude = 16000.0F;

  for (uint32_t i = 0; i < cfg.num_samples; ++i) {
    const float t = static_cast<float>(base_sample + i) * inv_sr;
    const float left_v = amplitude * std::sin(two_pi * cfg.tone_left_hz * t);

    samples[i * cfg.num_channels + 0] = static_cast<int16_t>(left_v);

    if (cfg.num_channels >= 2) {
      const float right_v = amplitude * std::sin(two_pi * cfg.tone_right_hz * t);
      samples[i * cfg.num_channels + 1] = static_cast<int16_t>(right_v);
    }
  }

  return frame;
}

}  // namespace audio_producer

#endif  // EXAMPLES_ZEROCOPY_ZEROCOPY_AUDIO_FRAME_AUDIO_PRODUCER_H_
