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

#ifndef EXAMPLES_ZEROCOPY_ZEROCOPY_TENSOR_TENSOR_CONSUMER_H_
#define EXAMPLES_ZEROCOPY_ZEROCOPY_TENSOR_TENSOR_CONSUMER_H_

#include <vlink/base/logger.h>
#include <vlink/zerocopy/tensor.h>

#include <cstdint>

// ---------------------------------------------------------------------------
// tensor_consumer.h
//
// Inline helpers used by consumer.cc to inspect / validate / statistically
// summarise Tensor instances received over the wire.  Header-only so the
// example stays a single translation unit per binary.
// ---------------------------------------------------------------------------

namespace tensor_consumer {

// Dump the descriptive metadata.  In shm:// zero-copy mode `is_owner` is
// false on the subscriber side because the data buffer still lives in the
// producer's pool slot.
static inline void print_tensor_info(const vlink::zerocopy::Tensor& tensor) {
  VLOG_I("  [Tensor] seq=", tensor.header.seq, " name=", tensor.name(), " layout=", tensor.layout(),
         " rank=", static_cast<int>(tensor.rank()), " num_elements=", tensor.num_elements(),
         " dtype=", static_cast<int>(tensor.dtype()), " size=", tensor.size(), " is_owner=", tensor.is_owner());
}

// Cheap structural validation -- guards against corrupted shape / dtype.
static inline bool validate_tensor(const vlink::zerocopy::Tensor& tensor) {
  if (!tensor.is_valid()) {
    VLOG_W("  [Consumer] Tensor is invalid");
    return false;
  }

  if (tensor.rank() == 0) {
    VLOG_W("  [Consumer] Tensor has zero rank");
    return false;
  }

  if (tensor.num_elements() == 0) {
    VLOG_W("  [Consumer] Tensor has zero elements");
    return false;
  }

  return true;
}

// Compute min / max / mean over a float32 tensor in a single pass.  Operates
// on the raw buffer borrowed from the wire -- no additional copy.  Returns
// the statistics via output parameters so the caller can log in one line.
static inline void compute_stats(const vlink::zerocopy::Tensor& tensor, float& min_v, float& max_v, float& mean_v) {
  min_v = 0.0F;
  max_v = 0.0F;
  mean_v = 0.0F;

  // Only float32 is supported here.  A production consumer would dispatch on
  // tensor.dtype() and walk the buffer through the corresponding typed view.
  if (tensor.dtype() != vlink::zerocopy::Tensor::kFloat32) {
    return;
  }

  const float* values = reinterpret_cast<const float*>(tensor.data());
  const uint64_t n = tensor.num_elements();

  if (n == 0) {
    return;
  }

  float acc = 0.0F;
  float lo = values[0];
  float hi = values[0];

  for (uint64_t i = 0; i < n; ++i) {
    const float v = values[i];
    acc += v;

    if (v < lo) {
      lo = v;
    }

    if (v > hi) {
      hi = v;
    }
  }

  min_v = lo;
  max_v = hi;
  mean_v = acc / static_cast<float>(n);
}

}  // namespace tensor_consumer

#endif  // EXAMPLES_ZEROCOPY_ZEROCOPY_TENSOR_TENSOR_CONSUMER_H_
