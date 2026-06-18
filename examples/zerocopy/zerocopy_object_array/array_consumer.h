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

#ifndef EXAMPLES_ZEROCOPY_ZEROCOPY_OBJECT_ARRAY_ARRAY_CONSUMER_H_
#define EXAMPLES_ZEROCOPY_ZEROCOPY_OBJECT_ARRAY_ARRAY_CONSUMER_H_

#include <vlink/base/logger.h>
#include <vlink/zerocopy/object_array.h>

#include <cstdint>

// ---------------------------------------------------------------------------
// array_consumer.h
//
// Inline helpers used by consumer.cc to inspect / validate / summarise
// ObjectArray instances received over the wire.  Header-only so the
// example stays a single translation unit per binary.
// ---------------------------------------------------------------------------

namespace array_consumer {

// Dump the container-level metadata.  In shm:// zero-copy mode `is_owner`
// is false on the subscriber side because the record buffer still lives in
// the producer's pool slot.
static inline void print_array_info(const vlink::zerocopy::ObjectArray& arr) {
  VLOG_I("  [Array] seq=", arr.header.seq, " source=", arr.source_id(), " count=", arr.count(),
         " pack_size=", arr.pack_size(), " capacity=", arr.capacity(), " is_owner=", arr.is_owner());
}

// Cheap structural validation.
static inline bool validate_array(const vlink::zerocopy::ObjectArray& arr) {
  if (!arr.is_valid()) {
    VLOG_W("  [Consumer] Array is invalid");
    return false;
  }

  if (arr.count() == 0) {
    VLOG_W("  [Consumer] Array has zero objects");
    return false;
  }

  if (arr.pack_size() == 0) {
    VLOG_W("  [Consumer] Array reports zero pack_size");
    return false;
  }

  return true;
}

// Walk every Object in the array and print its key fields.  `objects(i)`
// returns a typed pointer into the borrowed wire buffer -- no copy.
static inline void print_objects(const vlink::zerocopy::ObjectArray& arr) {
  for (uint32_t i = 0; i < arr.count(); ++i) {
    const vlink::zerocopy::ObjectArray::Object* obj = arr.objects(i);

    if (obj == nullptr) {
      continue;
    }

    VLOG_I("    [obj#", i, "] label=", obj->label, " class=", obj->class_id, " track=", obj->track_id, " pos=(",
           obj->position[0], ",", obj->position[1], ",", obj->position[2], ")", " vel=(", obj->velocity[0], ",",
           obj->velocity[1], ")", " yaw=", obj->yaw, " size=(", obj->size[0], "x", obj->size[1], "x", obj->size[2], ")",
           " score=", obj->score, " motion=", static_cast<int>(obj->motion_state),
           " source=", static_cast<int>(obj->source_type));
  }
}

}  // namespace array_consumer

#endif  // EXAMPLES_ZEROCOPY_ZEROCOPY_OBJECT_ARRAY_ARRAY_CONSUMER_H_
