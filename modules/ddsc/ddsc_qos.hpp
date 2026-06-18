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

#pragma once

#include "./base/helpers.h"
#include "./ddsc_factory.hpp"
#include "./extension/qos_profile.h"

namespace vlink {

[[maybe_unused]] static dds_duration_t get_dds_duration(int32_t ms) {
  if (ms < 0) {
    return DDS_INFINITY;
  }

  return DDS_MSECS(ms);
}

[[maybe_unused]] static void convert_qos(dds_qos_t* dds_qos, const Qos& qos, int32_t depth = 0) {
  if VUNLIKELY (!qos.valid) {
    return;
  }

  // reliability

  if (qos.reliability.kind == Qos::Reliability::kBestEffort) {
    dds_qset_reliability(dds_qos, DDS_RELIABILITY_BEST_EFFORT, get_dds_duration(qos.reliability.block_time));
  } else {
    dds_qset_reliability(dds_qos, DDS_RELIABILITY_RELIABLE, get_dds_duration(qos.reliability.block_time));
  }

  // history

  if (depth == 0) {
    depth = qos.history.depth;
  }

  if (qos.history.kind == Qos::History::kKeepLast) {
    dds_qset_history(dds_qos, DDS_HISTORY_KEEP_LAST, depth);
  } else {
    dds_qset_history(dds_qos, DDS_HISTORY_KEEP_ALL, depth);
  }

  // durability

  if (qos.durability.kind == Qos::Durability::kVolatile) {
    dds_qset_durability(dds_qos, DDS_DURABILITY_VOLATILE);
  } else if (qos.durability.kind == Qos::Durability::kTransientLocal) {
    dds_qset_durability(dds_qos, DDS_DURABILITY_TRANSIENT_LOCAL);
  } else if (qos.durability.kind == Qos::Durability::kTransient) {
    dds_qset_durability(dds_qos, DDS_DURABILITY_TRANSIENT);
  } else {
    dds_qset_durability(dds_qos, DDS_DURABILITY_PERSISTENT);
  }

  // publish_mode

  // liveliness

  if (qos.liveliness.kind == Qos::Liveliness::kAutomatic) {
    dds_qset_liveliness(dds_qos, DDS_LIVELINESS_AUTOMATIC, get_dds_duration(qos.liveliness.duration));
  } else if (qos.liveliness.kind == Qos::Liveliness::kManualParticipant) {
    dds_qset_liveliness(dds_qos, DDS_LIVELINESS_MANUAL_BY_PARTICIPANT, get_dds_duration(qos.liveliness.duration));
  } else {
    dds_qset_liveliness(dds_qos, DDS_LIVELINESS_MANUAL_BY_TOPIC, get_dds_duration(qos.liveliness.duration));
  }

  // destination_order

  if (qos.destination_order.kind == Qos::DestinationOrder::kReceptionTimestamp) {
    dds_qset_destination_order(dds_qos, DDS_DESTINATIONORDER_BY_RECEPTION_TIMESTAMP);
  } else {
    dds_qset_destination_order(dds_qos, DDS_DESTINATIONORDER_BY_SOURCE_TIMESTAMP);
  }

  // ownership

  if (qos.ownership.kind == Qos::Ownership::kShared) {
    dds_qset_ownership(dds_qos, DDS_OWNERSHIP_SHARED);
  } else {
    dds_qset_ownership(dds_qos, DDS_OWNERSHIP_EXCLUSIVE);
  }

  // deadline
  dds_qset_deadline(dds_qos, get_dds_duration(qos.deadline.period));

  // lifespan
  dds_qset_lifespan(dds_qos, get_dds_duration(qos.lifespan.duration));

  // latency_budget
  dds_qset_latency_budget(dds_qos, get_dds_duration(qos.latency_budget.duration));

  // resource_limits

  if (qos.resource_limits.max_samples > 0 && qos.resource_limits.max_instances > 0 &&
      qos.resource_limits.max_samples_per_instance > 0) {
    dds_qset_resource_limits(dds_qos, qos.resource_limits.max_samples, qos.resource_limits.max_instances,
                             qos.resource_limits.max_samples_per_instance);
  }
}

}  // namespace vlink
