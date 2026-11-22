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
#include "./dds_factory.h"
#include "./extension/qos_profile.h"

namespace vlink {

[[maybe_unused]] static Duration_t get_dds_duration(int32_t ms) {
  if (ms < 0) {
#ifdef VLINK_SUPPORT_DDS_V3
    return dds::c_TimeInfinite;
#else
    return eprosima::fastrtps::c_TimeInfinite;
#endif
  }

  return Duration_t(ms / 1000, (ms % 1000) * 1000000);
}

template <typename T>
[[maybe_unused]] static void convert_qos(T& dds_qos, const Qos& qos, int32_t depth = 0) {
  if VUNLIKELY (!qos.valid) {
    return;
  }

  // reliability

  if (qos.reliability.kind == Qos::Reliability::kBestEffort) {
    dds_qos.reliability().kind = dds::BEST_EFFORT_RELIABILITY_QOS;
  } else {
    dds_qos.reliability().kind = dds::RELIABLE_RELIABILITY_QOS;
  }

  dds_qos.reliability().max_blocking_time = get_dds_duration(qos.reliability.block_time);

  if constexpr (std::is_same_v<T, dds::DataWriterQos>) {
#ifdef VLINK_SUPPORT_DDS_V3
    dds_qos.reliable_writer_qos().times.heartbeat_period = get_dds_duration(qos.reliability.heartbeat_time);
#else
    dds_qos.reliable_writer_qos().times.heartbeatPeriod = get_dds_duration(qos.reliability.heartbeat_time);
#endif
  }

  // history

  if (qos.history.kind == Qos::History::kKeepLast) {
    dds_qos.history().kind = dds::KEEP_LAST_HISTORY_QOS;
  } else {
    dds_qos.history().kind = dds::KEEP_ALL_HISTORY_QOS;
  }

  if (depth == 0) {
    dds_qos.history().depth = qos.history.depth;
  } else {
    dds_qos.history().depth = depth;
  }

  // durability

  if (qos.durability.kind == Qos::Durability::kVolatile) {
    dds_qos.durability().kind = dds::VOLATILE_DURABILITY_QOS;
  } else if (qos.durability.kind == Qos::Durability::kTransientLocal) {
    dds_qos.durability().kind = dds::TRANSIENT_LOCAL_DURABILITY_QOS;
  } else if (qos.durability.kind == Qos::Durability::kTransient) {
    dds_qos.durability().kind = dds::TRANSIENT_DURABILITY_QOS;
  } else {
    dds_qos.durability().kind = dds::PERSISTENT_DURABILITY_QOS;
  }

  // publish_mode
  if constexpr (std::is_same_v<T, dds::DataWriterQos>) {
    if (qos.publish_mode.kind == Qos::PublishMode::kSync) {
      dds_qos.publish_mode().kind = dds::SYNCHRONOUS_PUBLISH_MODE;
    } else {
      dds_qos.publish_mode().kind = dds::ASYNCHRONOUS_PUBLISH_MODE;
    }
  }

  // liveliness

  if (qos.liveliness.kind == Qos::Liveliness::kAutomatic) {
    dds_qos.liveliness().kind = dds::AUTOMATIC_LIVELINESS_QOS;
  } else if (qos.liveliness.kind == Qos::Liveliness::kManualParticipant) {
    dds_qos.liveliness().kind = dds::MANUAL_BY_PARTICIPANT_LIVELINESS_QOS;
  } else {
    dds_qos.liveliness().kind = dds::MANUAL_BY_TOPIC_LIVELINESS_QOS;
  }

  dds_qos.liveliness().lease_duration = get_dds_duration(qos.liveliness.duration);

  if (qos.liveliness.duration > 0) {
    dds_qos.liveliness().announcement_period = get_dds_duration(qos.liveliness.duration / 10);
  }

  // destination_order

  if (qos.destination_order.kind == Qos::DestinationOrder::kReceptionTimestamp) {
    dds_qos.destination_order().kind = dds::BY_RECEPTION_TIMESTAMP_DESTINATIONORDER_QOS;
  } else {
    dds_qos.destination_order().kind = dds::BY_SOURCE_TIMESTAMP_DESTINATIONORDER_QOS;
  }

  // ownership

  if (qos.ownership.kind == Qos::Ownership::kShared) {
    dds_qos.ownership().kind = dds::SHARED_OWNERSHIP_QOS;
  } else {
    dds_qos.ownership().kind = dds::EXCLUSIVE_OWNERSHIP_QOS;
  }

  // deadline
  dds_qos.deadline().period = get_dds_duration(qos.deadline.period);

  // lifespan
  dds_qos.lifespan().duration = get_dds_duration(qos.lifespan.duration);

  // latency_budget
  dds_qos.latency_budget().duration = get_dds_duration(qos.latency_budget.duration);

  // resource_limits

  if (qos.resource_limits.max_samples > 0 && qos.resource_limits.max_instances > 0 &&
      qos.resource_limits.max_samples_per_instance > 0) {
    dds_qos.resource_limits().max_samples = qos.resource_limits.max_samples;
    dds_qos.resource_limits().max_instances = qos.resource_limits.max_instances;
    dds_qos.resource_limits().max_samples_per_instance = qos.resource_limits.max_samples_per_instance;
  }
}

}  // namespace vlink
