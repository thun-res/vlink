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

#ifndef EXAMPLES_ZEROCOPY_ZEROCOPY_CAMERA_FRAME_FRAME_CONSUMER_H_
#define EXAMPLES_ZEROCOPY_ZEROCOPY_CAMERA_FRAME_FRAME_CONSUMER_H_

#include <vlink/base/logger.h>
#include <vlink/zerocopy/camera_frame.h>

#include <cstdint>

// ---------------------------------------------------------------------------
// frame_consumer.h
//
// Inline helpers used by consumer.cc to inspect / validate / sanity-check
// CameraFrame instances received over the wire. Kept header-only so the
// example stays a single translation unit per binary.
// ---------------------------------------------------------------------------

namespace frame_consumer {

// Dump the descriptive header. `is_owner` is the giveaway: in shm:// zero-
// copy mode it will be false on the subscriber side because the buffer
// still belongs to the producer's pool slot.
inline void print_frame_info(const vlink::zerocopy::CameraFrame& frame) {
  VLOG_I("  [Frame] seq=", frame.header.seq, " ", frame.width(), "x", frame.height(),
         " format=", static_cast<int>(frame.format()), " stream=", static_cast<int>(frame.stream()),
         " size=", frame.size(), " is_owner=", frame.is_owner());
}

// Cheap structural validation -- guards against truncated transmissions
// and miswired publishers. Real consumers would add format-specific checks
// (e.g. JPEG SOI marker, NAL unit start codes).
inline bool validate_frame(const vlink::zerocopy::CameraFrame& frame) {
  if (!frame.is_valid()) {
    VLOG_W("  [Consumer] Frame is invalid");
    return false;
  }

  if (frame.width() == 0 || frame.height() == 0) {
    VLOG_W("  [Consumer] Frame has zero dimensions");
    return false;
  }

  if (frame.size() == 0) {
    VLOG_W("  [Consumer] Frame has no pixel data");
    return false;
  }

  return true;
}

// Naive additive checksum -- enough to detect missed/swapped frames during
// the demo, not a real integrity guard. Production code would use CRC32 or
// the existing AEAD path.
inline uint32_t compute_checksum(const vlink::zerocopy::CameraFrame& frame) {
  uint32_t sum = 0;
  const uint8_t* data = frame.data();
  for (size_t i = 0; i < frame.size(); ++i) {
    sum += data[i];
  }

  return sum;
}

}  // namespace frame_consumer

#endif  // EXAMPLES_ZEROCOPY_ZEROCOPY_CAMERA_FRAME_FRAME_CONSUMER_H_
