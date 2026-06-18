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

#ifndef EXAMPLES_ZEROCOPY_ZEROCOPY_OBJECT_ARRAY_ARRAY_PRODUCER_H_
#define EXAMPLES_ZEROCOPY_ZEROCOPY_OBJECT_ARRAY_ARRAY_PRODUCER_H_

#include <vlink/zerocopy/object_array.h>

#include <cstdint>
#include <cstring>
#include <string_view>

// ---------------------------------------------------------------------------
// array_producer.h
//
// Helper for the object_array example: builds a synthetic 3-D detection
// array containing a small grab-bag of objects (car / pedestrian / cyclist)
// at varying positions and velocities.  The container is pre-sized to 16
// slots; only 4 are populated each frame so the example also exercises the
// "logical count < capacity" path.  All object positions drift slightly
// with `seq` so consumers can see the kinematic state change frame-to-frame.
// ---------------------------------------------------------------------------

namespace array_producer {

struct ArrayConfig {
  uint32_t capacity;  // Maximum number of Object slots to pre-allocate.
  uint32_t freq;      // Nominal publish frequency in Hz.
  uint32_t channel;   // Sensor / producer channel identifier.
};

// Populate a single Object record.  The Object struct is a POD with direct
// field access -- no setters -- so we write into the public fields directly.
// `label` is a 32-byte char[]; we strncpy and force NUL-terminate even though
// the field is already zero-initialised, for defence in depth.
static inline void fill_object(vlink::zerocopy::ObjectArray::Object& obj, std::string_view label, uint32_t class_id,
                               uint32_t track_id, float px, float py, float vx, float vy, float yaw, float length,
                               float width, float height, float score, vlink::zerocopy::ObjectArray::MotionState motion,
                               vlink::zerocopy::ObjectArray::SourceType source) {
  // Bounded copy into the fixed-size label buffer.  sizeof(obj.label) is 32;
  // strncpy fills the remainder with NULs which is fine for char[].
  const size_t label_max = sizeof(obj.label) - 1;
  const size_t copy_len = label.size() < label_max ? label.size() : label_max;
  std::memcpy(obj.label, label.data(), copy_len);
  obj.label[copy_len] = '\0';

  // 3-D position in metres (world frame).
  obj.position[0] = px;
  obj.position[1] = py;
  obj.position[2] = 0.0F;

  obj.yaw = yaw;
  obj.yaw_rate = 0.0F;

  // Bounding-box dimensions: length, width, height in metres.
  obj.size[0] = length;
  obj.size[1] = width;
  obj.size[2] = height;

  // Linear velocity in metres per second.
  obj.velocity[0] = vx;
  obj.velocity[1] = vy;
  obj.velocity[2] = 0.0F;

  obj.acceleration[0] = 0.0F;
  obj.acceleration[1] = 0.0F;
  obj.acceleration[2] = 0.0F;

  obj.score = score;
  obj.existence_probability = score;

  // Diagonal-only position covariance (xx, yy, zz on indices 0, 3, 5).
  obj.position_covariance[0] = 0.25F;
  obj.position_covariance[3] = 0.25F;
  obj.position_covariance[5] = 0.10F;

  obj.class_id = class_id;
  obj.track_id = track_id;
  obj.age = 1;
  obj.num_observations = 1;
  obj.motion_state = motion;
  obj.source_type = source;
  obj.subtype_id = 0;
}

// Build one ObjectArray: configure metadata, allocate the record buffer,
// then push a handful of objects whose pose drifts with `seq`.
static inline vlink::zerocopy::ObjectArray create_test_array(const ArrayConfig& cfg, uint32_t seq) {
  vlink::zerocopy::ObjectArray arr;

  // Producer module identifier baked into the header.  Useful when several
  // perception pipelines publish on the same topic.
  arr.set_source_id("fusion_v1");
  arr.set_channel(cfg.channel);
  arr.set_freq(cfg.freq);
  arr.header.seq = seq;

  // Pre-allocate `capacity` Object slots.  push_value() will append from
  // count_ = 0 and bump count_ on each call.
  arr.create(cfg.capacity);

  // Sequence-driven position drift -- 0.5 m per frame along +x.
  const float drift = static_cast<float>(seq) * 0.5F;

  // Object 0: a car ~12 m ahead, cruising at 8.5 m/s.
  vlink::zerocopy::ObjectArray::Object obj0;
  fill_object(obj0, "car", /*class_id=*/1, /*track_id=*/101, /*px=*/12.0F + drift, /*py=*/0.3F,
              /*vx=*/8.5F, /*vy=*/0.0F, /*yaw=*/0.05F, /*length=*/4.5F, /*width=*/1.8F, /*height=*/1.6F,
              /*score=*/0.92F, vlink::zerocopy::ObjectArray::kMotionMoving,
              vlink::zerocopy::ObjectArray::kSourceFusion);
  arr.push_value(obj0);

  // Object 1: a pedestrian ~5 m to the side, walking laterally.
  vlink::zerocopy::ObjectArray::Object obj1;
  fill_object(obj1, "pedestrian", /*class_id=*/2, /*track_id=*/102, /*px=*/3.5F, /*py=*/-2.0F + drift * 0.1F,
              /*vx=*/0.0F, /*vy=*/1.2F, /*yaw=*/1.5708F, /*length=*/0.6F, /*width=*/0.6F, /*height=*/1.7F,
              /*score=*/0.85F, vlink::zerocopy::ObjectArray::kMotionMoving,
              vlink::zerocopy::ObjectArray::kSourceCamera);
  arr.push_value(obj1);

  // Object 2: a cyclist ~7 m ahead and to the right.
  vlink::zerocopy::ObjectArray::Object obj2;
  fill_object(obj2, "cyclist", /*class_id=*/3, /*track_id=*/103, /*px=*/7.0F + drift * 0.4F, /*py=*/1.5F,
              /*vx=*/4.0F, /*vy=*/0.0F, /*yaw=*/0.1F, /*length=*/1.8F, /*width=*/0.7F, /*height=*/1.7F,
              /*score=*/0.78F, vlink::zerocopy::ObjectArray::kMotionMoving, vlink::zerocopy::ObjectArray::kSourceLidar);
  arr.push_value(obj2);

  // Object 3: a parked car off to the right -- stationary motion state.
  vlink::zerocopy::ObjectArray::Object obj3;
  fill_object(obj3, "car", /*class_id=*/1, /*track_id=*/104, /*px=*/15.0F, /*py=*/-3.5F, /*vx=*/0.0F,
              /*vy=*/0.0F, /*yaw=*/0.0F, /*length=*/4.6F, /*width=*/1.9F, /*height=*/1.5F, /*score=*/0.88F,
              vlink::zerocopy::ObjectArray::kMotionParked, vlink::zerocopy::ObjectArray::kSourceRadar);
  arr.push_value(obj3);

  return arr;
}

}  // namespace array_producer

#endif  // EXAMPLES_ZEROCOPY_ZEROCOPY_OBJECT_ARRAY_ARRAY_PRODUCER_H_
