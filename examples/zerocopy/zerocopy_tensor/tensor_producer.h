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

#ifndef EXAMPLES_ZEROCOPY_ZEROCOPY_TENSOR_TENSOR_PRODUCER_H_
#define EXAMPLES_ZEROCOPY_ZEROCOPY_TENSOR_TENSOR_PRODUCER_H_

#include <vlink/zerocopy/tensor.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// tensor_producer.h
//
// Helper for the tensor example: builds a synthetic NCHW float32 tensor of
// shape {1, 3, 224, 224} -- the canonical ImageNet image feed for ResNet /
// ViT / YOLO -family models -- and fills it with a smooth sin/cos pattern.
// The choice of shape, dtype, layout label and quant params mirrors what a
// real inference producer (camera frontend -> preprocessing) would emit so
// the example doubles as a copy-paste skeleton for production code.
//
// Sizing: 1 * 3 * 224 * 224 * sizeof(float) = 602112 bytes ~ 588 KiB per
// tensor.  Large enough to feel the cost of an extra memcpy, but still
// trivial to enqueue at 30 Hz over dds://.
// ---------------------------------------------------------------------------

namespace tensor_producer {

struct TensorConfig {
  uint32_t batch;     // N -- batch size (typically 1 for live inference).
  uint32_t channels;  // C -- feature channels (3 for RGB / BGR).
  uint32_t height;    // H -- spatial height.
  uint32_t width;     // W -- spatial width.
  uint32_t freq;      // Nominal publish frequency in Hz.
  uint32_t channel;   // Sensor / producer / output-port channel identifier.
};

// Build one Tensor: configure metadata, allocate the data buffer, then
// paint a smooth analytic pattern that depends on `seq` so consumers can
// fingerprint individual frames without storing reference tensors.
static inline vlink::zerocopy::Tensor create_test_tensor(const TensorConfig& cfg, uint32_t seq) {
  vlink::zerocopy::Tensor tensor;

  // Descriptive metadata.  `name` and `model_id` survive across the wire so
  // multi-model topologies can route on these fields rather than topic name.
  tensor.set_name("image");
  tensor.set_model_id("demo_backbone_v1");
  tensor.set_layout("NCHW");

  // dtype must be set BEFORE create() so element_size_ is cached when
  // num_elements * element_size is later validated by the wire path.
  tensor.set_dtype(vlink::zerocopy::Tensor::kFloat32);
  tensor.set_device(vlink::zerocopy::Tensor::kDeviceCpu);

  // Shape + strides + num_elements are computed in a single call.  Note that
  // set_shape() expects a contiguous shape array; the C-style array decays
  // into the required pointer.
  const uint32_t shape[4] = {cfg.batch, cfg.channels, cfg.height, cfg.width};
  tensor.set_shape(shape, 4);
  tensor.set_batch_size(cfg.batch);

  // Quantisation knobs left at neutral values -- this tensor is fp32 so the
  // scale / zero-point fields are documentation-only.
  tensor.set_quant_scale(0.0F);
  tensor.set_quant_zero_point(0);

  // Channel + freq belong to the standard transport metadata triplet
  // (channel, freq, header.seq).
  tensor.set_channel(cfg.channel);
  tensor.set_freq(cfg.freq);
  tensor.header.seq = seq;

  // Allocate num_elements * sizeof(float) bytes.  num_elements was cached by
  // set_shape().
  const size_t total_bytes = static_cast<size_t>(tensor.num_elements()) * sizeof(float);
  tensor.create(total_bytes);

  // Fill with a deterministic sin/cos lattice -- per-channel phase offset
  // distinguishes R/G/B planes; sequence number shifts the phase frame-to-
  // frame so the same pixel changes value over time.
  float* values = reinterpret_cast<float*>(const_cast<uint8_t*>(tensor.data()));
  const float phase = static_cast<float>(seq) * 0.1F;

  for (uint32_t b = 0; b < cfg.batch; ++b) {
    for (uint32_t c = 0; c < cfg.channels; ++c) {
      const float channel_phase = phase + static_cast<float>(c) * 1.0472F;  // ~60deg

      for (uint32_t h = 0; h < cfg.height; ++h) {
        for (uint32_t w = 0; w < cfg.width; ++w) {
          const float fx = static_cast<float>(w) / static_cast<float>(cfg.width);
          const float fy = static_cast<float>(h) / static_cast<float>(cfg.height);

          // Bounded in [-1, 1]; mirrors a normalised image tensor after the
          // typical mean/std subtraction step.
          const float v = std::sin(fx * 6.2832F + channel_phase) * std::cos(fy * 6.2832F);

          // Linear NCHW index: ((b*C + c)*H + h)*W + w.
          const size_t idx = (((static_cast<size_t>(b) * cfg.channels + c) * cfg.height + h) * cfg.width + w);
          values[idx] = v;
        }
      }
    }
  }

  return tensor;
}

}  // namespace tensor_producer

#endif  // EXAMPLES_ZEROCOPY_ZEROCOPY_TENSOR_TENSOR_PRODUCER_H_
