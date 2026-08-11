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

/**
 * @file qos_profile.h
 * @brief Curated catalogue of pre-built @c Qos instances for common VLink workloads.
 *
 * @details
 * Picking the right @c Qos for a topic involves balancing reliability, history depth,
 * durability, publish mode and priority.  This file declares sixteen ready-to-use
 * @c constexpr profiles inside @c vlink::QosProfile, each tuned for a specific class of
 * traffic encountered in autonomy and embedded stacks.  Every profile is constructed
 * with @c valid set to @c true and can be passed straight to any VLink endpoint or
 * registered with a transport (@c DdsConf::register_qos) before being referenced from
 * a URL.
 *
 * Profile catalogue (use-case oriented):
 *
 * | Profile        | Reliability      | History        | Durability      | PublishMode | Priority   |
 * | -------------- | ---------------- | -------------- | --------------- | ----------- | ---------- |
 * | @c kEvent      | Reliable         | KeepLast(5)    | Volatile        | Sync        | RealTime   |
 * | @c kMethod     | Reliable         | KeepAll        | Volatile        | Sync        | High       |
 * | @c kField      | Reliable         | KeepLast(1)    | TransientLocal  | Sync        | High       |
 * | @c kSensor     | BestEffort       | KeepLast(10)   | Volatile        | ASync       | Normal*    |
 * | @c kParameter  | Reliable         | KeepLast(500)  | TransientLocal  | Sync        | Normal     |
 * | @c kService    | Reliable         | KeepLast(10)   | TransientLocal  | Sync        | Normal     |
 * | @c kClock      | BestEffort       | KeepLast(1)    | Volatile        | Sync        | Low*       |
 * | @c kStatic     | Reliable         | KeepAll        | TransientLocal  | Sync        | Normal     |
 * | @c kLight      | Reliable         | KeepLast(1)    | Volatile        | ASync       | High       |
 * | @c kPoor       | BestEffort       | KeepLast(5)    | Volatile        | ASync       | Background |
 * | @c kBetter     | BestEffort       | KeepLast(50)   | Volatile        | Sync        | RealTime   |
 * | @c kBest       | Reliable         | KeepLast(200)  | Volatile        | Sync        | RealTime   |
 * | @c kLarge      | Reliable (HB500) | KeepLast(500)  | Volatile        | Sync        | Low        |
 * | @c kAlarm      | Reliable         | KeepAll        | TransientLocal  | Sync        | RealTime*  |
 * | @c kCommand    | Reliable         | KeepLast(1)    | Volatile        | Sync        | RealTime   |
 * | @c kLog        | Reliable         | KeepLast(100)  | Volatile        | ASync       | Background |
 *
 * Profiles marked @c * (@c kSensor, @c kClock, @c kAlarm) are dispatched as express (the
 * @c is_express flag is set).  Use-case mapping:
 * @c kEvent = discrete control events, @c kMethod = RPC, @c kField = latest-value state sync,
 * @c kSensor = high-rate sensors, @c kParameter = slow config, @c kService = discovery,
 * @c kClock = time sync, @c kStatic = maps / calibration, @c kLight = small frequent traffic,
 * @c kPoor = low-priority telemetry, @c kBetter = best-effort throughput,
 * @c kBest = reliable throughput, @c kLarge = large payload (heartbeat 500 ms),
 * @c kAlarm = safety-critical alarms, @c kCommand = actuator commands, @c kLog = log streams.
 *
 * @par Looking profiles up by name
 * @code
 * const auto& qos_map = vlink::QosProfile::get_available_qos_map();
 *
 * if (auto it = qos_map.find("sensor"); it != qos_map.end()) {
 *   vlink::DdsConf::register_qos("my_sensor_qos", it->second);
 * }
 * @endcode
 *
 * @par Using a profile by URL
 * @code
 * // Built-in names are recognised directly in URLs:
 * auto pub = vlink::Publisher<MyMsg>::create_unique("dds://sensor_data?qos=sensor");
 *
 * // Or apply a profile programmatically:
 * vlink::Qos qos = vlink::QosProfile::kField;
 * qos.history.depth = 5;        // tweak the depth before use
 * vlink::DdsConf::register_qos("my_field_qos", qos);
 * @endcode
 */

#pragma once

#include <string>
#include <unordered_map>

#include "../base/macros.h"
#include "./qos.h"

namespace vlink {

/**
 * @namespace vlink::QosProfile
 * @brief Pre-built @c Qos constants for common autonomy and embedded workloads.
 *
 * @details
 * Every constant in this namespace carries @c valid = @c true and is safe to pass
 * directly to any VLink endpoint or to register with a transport.  Pick the closest
 * profile for your traffic and customise a copy if you need finer control.  All built-in
 * profiles default-construct Deadline and Lifespan, leaving the Deadline period at @c -1
 * (no constraint) and the Lifespan duration at @c -1 (infinite).  Configure a finite
 * Lifespan only when the hosts share a suitable clock-synchronisation contract.
 *
 * @see Qos, get_available_qos_map()
 */
namespace QosProfile {  // NOLINT(readability-identifier-naming)

/**
 * @brief Reliable, KeepLast(5), Volatile, Sync, RealTime priority.
 *
 * @details Designed for discrete control events where delivery must be guaranteed and
 * a small backlog of late arrivals is acceptable.  A 1000 ms automatic liveliness lease
 * surfaces a dead writer quickly.  Lifespan is infinite (-1), and no Deadline is imposed
 * because control events are aperiodic.
 */
[[maybe_unused]] static inline constexpr Qos kEvent{
    "event",
    true,
    Qos::Reliability{Qos::Reliability::kReliable},
    Qos::History{Qos::History::kKeepLast, 5},
    Qos::Durability{Qos::Durability::kVolatile},
    Qos::PublishMode{Qos::PublishMode::kSync},
    Qos::Liveliness{Qos::Liveliness::kAutomatic, 1000},
    Qos::DestinationOrder{},
    Qos::Ownership{},
    Qos::Deadline{},
    Qos::Lifespan{},
    Qos::LatencyBudget{},
    Qos::ResourceLimits{},
    Qos::Additions{Qos::Additions::kPriorityRealTime, false},
};

/**
 * @brief Reliable, KeepAll, Volatile, Sync, High priority.
 *
 * @details Designed for RPC-style request/response flows.  KeepAll ensures no request
 * is dropped even under sustained load.  A 2000 ms liveliness lease detects a stalled
 * peer.  Lifespan is infinite (-1), and no Deadline is imposed because request arrival
 * is demand-driven, not periodic.
 */
[[maybe_unused]] static inline constexpr Qos kMethod{
    "method",
    true,
    Qos::Reliability{Qos::Reliability::kReliable},
    Qos::History{Qos::History::kKeepAll, 1},
    Qos::Durability{Qos::Durability::kVolatile},
    Qos::PublishMode{Qos::PublishMode::kSync},
    Qos::Liveliness{Qos::Liveliness::kAutomatic, 2000},
    Qos::DestinationOrder{},
    Qos::Ownership{},
    Qos::Deadline{},
    Qos::Lifespan{},
    Qos::LatencyBudget{},
    Qos::ResourceLimits{},
    Qos::Additions{Qos::Additions::kPriorityHigh, false},
};

/**
 * @brief Reliable, KeepLast(1), TransientLocal, Sync, High priority.
 *
 * @details Designed for Field model traffic where only the latest value matters but
 * late-joining subscribers must still receive that latest value on demand.  A 2000 ms
 * liveliness lease guards the writer; Lifespan is infinite (-1) so the latest state never
 * expires, and no Deadline is imposed because state updates are change-driven.
 */
[[maybe_unused]] static inline constexpr Qos kField{
    "field",
    true,
    Qos::Reliability{Qos::Reliability::kReliable},
    Qos::History{Qos::History::kKeepLast, 1},
    Qos::Durability{Qos::Durability::kTransientLocal},
    Qos::PublishMode{Qos::PublishMode::kSync},
    Qos::Liveliness{Qos::Liveliness::kAutomatic, 2000},
    Qos::DestinationOrder{},
    Qos::Ownership{},
    Qos::Deadline{},
    Qos::Lifespan{},
    Qos::LatencyBudget{},
    Qos::ResourceLimits{},
    Qos::Additions{Qos::Additions::kPriorityHigh, false},
};

/**
 * @brief BestEffort, KeepLast(10), Volatile, ASync, Normal priority, express delivery.
 *
 * @details Designed for high-rate sensor streams (LiDAR, camera, IMU) where throughput
 * dominates and a few dropped samples are preferable to back-pressure.  A tight 500 ms
 * liveliness lease detects loss of writer liveliness.  Deadline is unconstrained (-1),
 * and Lifespan is infinite (-1).
 */
[[maybe_unused]] static inline constexpr Qos kSensor{
    "sensor",
    true,
    Qos::Reliability{Qos::Reliability::kBestEffort},
    Qos::History{Qos::History::kKeepLast, 10},
    Qos::Durability{Qos::Durability::kVolatile},
    Qos::PublishMode{Qos::PublishMode::kASync},
    Qos::Liveliness{Qos::Liveliness::kAutomatic, 500},
    Qos::DestinationOrder{},
    Qos::Ownership{},
    Qos::Deadline{},
    Qos::Lifespan{},
    Qos::LatencyBudget{},
    Qos::ResourceLimits{},
    Qos::Additions{Qos::Additions::kPriorityNormal, true},
};

/**
 * @brief Reliable, KeepLast(500), TransientLocal, Sync, Normal priority.
 *
 * @details Designed for configuration parameters that change rarely but must be delivered
 * reliably.  TransientLocal lets late-joining subscribers catch up on the retained history,
 * and the KeepLast depth of 500 stays within the default @c max_samples_per_instance limit.
 * A relaxed 5000 ms liveliness lease suits slow traffic; Lifespan is infinite (-1) so the
 * configured values never expire, and no Deadline is imposed because changes are sporadic.
 */
[[maybe_unused]] static inline constexpr Qos kParameter{
    "parameter",
    true,
    Qos::Reliability{Qos::Reliability::kReliable},
    Qos::History{Qos::History::kKeepLast, 500},
    Qos::Durability{Qos::Durability::kTransientLocal},
    Qos::PublishMode{Qos::PublishMode::kSync},
    Qos::Liveliness{Qos::Liveliness::kAutomatic, 5000},
    Qos::DestinationOrder{},
    Qos::Ownership{},
    Qos::Deadline{},
    Qos::Lifespan{},
    Qos::LatencyBudget{},
    Qos::ResourceLimits{},
    Qos::Additions{Qos::Additions::kPriorityNormal, false},
};

/**
 * @brief Reliable, KeepLast(10), TransientLocal, Sync, Normal priority.
 *
 * @details Designed for service registration and discovery messages where late joiners
 * must see the current set of advertised services.  A 3000 ms liveliness lease lets a
 * vanished provider be reaped; Lifespan is infinite (-1) so an advertisement persists
 * until withdrawn, and no Deadline is imposed because registrations are event-driven.
 */
[[maybe_unused]] static inline constexpr Qos kService{
    "service",
    true,
    Qos::Reliability{Qos::Reliability::kReliable},
    Qos::History{Qos::History::kKeepLast, 10},
    Qos::Durability{Qos::Durability::kTransientLocal},
    Qos::PublishMode{Qos::PublishMode::kSync},
    Qos::Liveliness{Qos::Liveliness::kAutomatic, 3000},
    Qos::DestinationOrder{},
    Qos::Ownership{},
    Qos::Deadline{},
    Qos::Lifespan{},
    Qos::LatencyBudget{},
    Qos::ResourceLimits{},
    Qos::Additions{Qos::Additions::kPriorityNormal, false},
};

/**
 * @brief BestEffort, KeepLast(1), Volatile, Sync, Low priority, express delivery.
 *
 * @details Designed for periodic time synchronisation broadcasts where only the most
 * recent tick has value and an occasional skipped tick is harmless.  Synchronous express
 * dispatch keeps tick jitter low, and a 1000 ms liveliness lease detects loss of writer
 * liveliness.  Deadline is unconstrained (-1), and Lifespan is infinite (-1) so host clock
 * skew cannot discard synchronisation ticks prematurely.
 */
[[maybe_unused]] static inline constexpr Qos kClock{
    "clock",
    true,
    Qos::Reliability{Qos::Reliability::kBestEffort},
    Qos::History{Qos::History::kKeepLast, 1},
    Qos::Durability{Qos::Durability::kVolatile},
    Qos::PublishMode{Qos::PublishMode::kSync},
    Qos::Liveliness{Qos::Liveliness::kAutomatic, 1000},
    Qos::DestinationOrder{},
    Qos::Ownership{},
    Qos::Deadline{},
    Qos::Lifespan{},
    Qos::LatencyBudget{},
    Qos::ResourceLimits{},
    Qos::Additions{Qos::Additions::kPriorityLow, true},
};

/**
 * @brief Reliable, KeepAll, TransientLocal, Sync, Normal priority.
 *
 * @details Designed for largely static datasets (HD maps, calibration tables) that any
 * late-joining subscriber should receive in full (retained up to the configured
 * @c ResourceLimits).  A long 10000 ms liveliness lease
 * tolerates a writer that publishes once and stays quiet; Lifespan is infinite (-1) so the
 * dataset never expires, and no Deadline is imposed because the data is essentially static.
 */
[[maybe_unused]] static inline constexpr Qos kStatic{
    "static",
    true,
    Qos::Reliability{Qos::Reliability::kReliable},
    Qos::History{Qos::History::kKeepAll, 1},
    Qos::Durability{Qos::Durability::kTransientLocal},
    Qos::PublishMode{Qos::PublishMode::kSync},
    Qos::Liveliness{Qos::Liveliness::kAutomatic, 10000},
    Qos::DestinationOrder{},
    Qos::Ownership{},
    Qos::Deadline{},
    Qos::Lifespan{},
    Qos::LatencyBudget{},
    Qos::ResourceLimits{},
    Qos::Additions{Qos::Additions::kPriorityNormal, false},
};

/**
 * @brief Reliable, KeepLast(1), Volatile, ASync, High priority.
 *
 * @details Designed for small frequent messages where only the latest value matters
 * and asynchronous delivery keeps CPU overhead low.  A 1000 ms liveliness lease guards
 * the writer.  Lifespan is infinite (-1), and no Deadline is imposed so the profile stays
 * usable for both periodic and bursty small-message traffic.
 */
[[maybe_unused]] static inline constexpr Qos kLight{
    "light",
    true,
    Qos::Reliability{Qos::Reliability::kReliable},
    Qos::History{Qos::History::kKeepLast, 1},
    Qos::Durability{Qos::Durability::kVolatile},
    Qos::PublishMode{Qos::PublishMode::kASync},
    Qos::Liveliness{Qos::Liveliness::kAutomatic, 1000},
    Qos::DestinationOrder{},
    Qos::Ownership{},
    Qos::Deadline{},
    Qos::Lifespan{},
    Qos::LatencyBudget{},
    Qos::ResourceLimits{},
    Qos::Additions{Qos::Additions::kPriorityHigh, false},
};

/**
 * @brief BestEffort, KeepLast(5), Volatile, ASync, Background priority.
 *
 * @details Designed for low-priority telemetry and diagnostics where any sample loss
 * is acceptable and the goal is to minimise CPU and bandwidth impact.  A relaxed 5000 ms
 * liveliness lease keeps overhead low.  Lifespan is infinite (-1), and no Deadline is
 * imposed because telemetry cadence is not contractual.
 */
[[maybe_unused]] static inline constexpr Qos kPoor{
    "poor",
    true,
    Qos::Reliability{Qos::Reliability::kBestEffort},
    Qos::History{Qos::History::kKeepLast, 5},
    Qos::Durability{Qos::Durability::kVolatile},
    Qos::PublishMode{Qos::PublishMode::kASync},
    Qos::Liveliness{Qos::Liveliness::kAutomatic, 5000},
    Qos::DestinationOrder{},
    Qos::Ownership{},
    Qos::Deadline{},
    Qos::Lifespan{},
    Qos::LatencyBudget{},
    Qos::ResourceLimits{},
    Qos::Additions{Qos::Additions::kPriorityBackground, false},
};

/**
 * @brief BestEffort, KeepLast(50), Volatile, Sync, RealTime priority.
 *
 * @details Designed for high-throughput best-effort streams that benefit from a deeper
 * buffer and real-time dispatch priority.  A 1000 ms liveliness lease guards the writer
 * and Lifespan is infinite (-1); no Deadline is imposed so the profile suits variable-rate
 * throughput.
 */
[[maybe_unused]] static inline constexpr Qos kBetter{
    "better",
    true,
    Qos::Reliability{Qos::Reliability::kBestEffort},
    Qos::History{Qos::History::kKeepLast, 50},
    Qos::Durability{Qos::Durability::kVolatile},
    Qos::PublishMode{Qos::PublishMode::kSync},
    Qos::Liveliness{Qos::Liveliness::kAutomatic, 1000},
    Qos::DestinationOrder{},
    Qos::Ownership{},
    Qos::Deadline{},
    Qos::Lifespan{},
    Qos::LatencyBudget{},
    Qos::ResourceLimits{},
    Qos::Additions{Qos::Additions::kPriorityRealTime, false},
};

/**
 * @brief Reliable, KeepLast(200), Volatile, Sync, RealTime priority.
 *
 * @details Designed for high-throughput reliable streams that require predictable
 * latency, pairing reliability with synchronous publishing and a deep buffer.  A 1000 ms
 * liveliness lease guards the writer and Lifespan is infinite (-1); no Deadline is imposed
 * so the profile suits variable-rate throughput.
 */
[[maybe_unused]] static inline constexpr Qos kBest{
    "best",
    true,
    Qos::Reliability{Qos::Reliability::kReliable},
    Qos::History{Qos::History::kKeepLast, 200},
    Qos::Durability{Qos::Durability::kVolatile},
    Qos::PublishMode{Qos::PublishMode::kSync},
    Qos::Liveliness{Qos::Liveliness::kAutomatic, 1000},
    Qos::DestinationOrder{},
    Qos::Ownership{},
    Qos::Deadline{},
    Qos::Lifespan{},
    Qos::LatencyBudget{},
    Qos::ResourceLimits{},
    Qos::Additions{Qos::Additions::kPriorityRealTime, false},
};

/**
 * @brief Reliable, KeepLast(500), Volatile, Sync, Low priority with a shorter 500 ms heartbeat.
 *
 * @details Designed for large payload transfers (maps, point clouds, images) where a
 * large buffer and a tighter 500 ms heartbeat (vs the 3000 ms default) trigger faster
 * NACK-driven recovery of lost fragments over slower transport pipelines.
 * A 3000 ms liveliness lease suits the slower cadence and Lifespan is infinite (-1); no
 * Deadline is imposed because large transfers are not periodic.
 */
[[maybe_unused]] static inline constexpr Qos kLarge{
    "large",
    true,
    Qos::Reliability{Qos::Reliability::kReliable, 100, 500},
    Qos::History{Qos::History::kKeepLast, 500},
    Qos::Durability{Qos::Durability::kVolatile},
    Qos::PublishMode{Qos::PublishMode::kSync},
    Qos::Liveliness{Qos::Liveliness::kAutomatic, 3000},
    Qos::DestinationOrder{},
    Qos::Ownership{},
    Qos::Deadline{},
    Qos::Lifespan{},
    Qos::LatencyBudget{},
    Qos::ResourceLimits{},
    Qos::Additions{Qos::Additions::kPriorityLow, false},
};

/**
 * @brief Reliable, KeepAll, TransientLocal, Sync, RealTime priority, express delivery.
 *
 * @details Designed for safety-critical alarms and fault events that must be delivered
 * reliably and made available to late-joining subscribers.  KeepAll plus TransientLocal
 * latch the outstanding alarm set (retained up to the configured @c ResourceLimits — raise
 * @c max_samples_per_instance for very bursty alarm sources), express + RealTime minimise
 * dispatch latency, a tight 500 ms liveliness lease detects a dead annunciator, and Lifespan
 * is infinite (-1) so an alarm stays valid until explicitly cleared; no Deadline is imposed
 * because alarms are aperiodic.
 */
[[maybe_unused]] static inline constexpr Qos kAlarm{
    "alarm",
    true,
    Qos::Reliability{Qos::Reliability::kReliable},
    Qos::History{Qos::History::kKeepAll, 1},
    Qos::Durability{Qos::Durability::kTransientLocal},
    Qos::PublishMode{Qos::PublishMode::kSync},
    Qos::Liveliness{Qos::Liveliness::kAutomatic, 500},
    Qos::DestinationOrder{},
    Qos::Ownership{},
    Qos::Deadline{},
    Qos::Lifespan{},
    Qos::LatencyBudget{},
    Qos::ResourceLimits{},
    Qos::Additions{Qos::Additions::kPriorityRealTime, true},
};

/**
 * @brief Reliable, KeepLast(1), Volatile, Sync, RealTime priority.
 *
 * @details Designed for actuator and vehicle-control commands where only the most recent
 * command is valid and delivery must be guaranteed with minimal latency.  KeepLast(1) keeps
 * just the latest order and a tight 500 ms liveliness lease detects a dead commander.
 * Lifespan is infinite (-1), and no Deadline is imposed because commands are issued on
 * demand rather than on a fixed cadence.
 */
[[maybe_unused]] static inline constexpr Qos kCommand{
    "command",
    true,
    Qos::Reliability{Qos::Reliability::kReliable},
    Qos::History{Qos::History::kKeepLast, 1},
    Qos::Durability{Qos::Durability::kVolatile},
    Qos::PublishMode{Qos::PublishMode::kSync},
    Qos::Liveliness{Qos::Liveliness::kAutomatic, 500},
    Qos::DestinationOrder{},
    Qos::Ownership{},
    Qos::Deadline{},
    Qos::Lifespan{},
    Qos::LatencyBudget{},
    Qos::ResourceLimits{},
    Qos::Additions{Qos::Additions::kPriorityRealTime, false},
};

/**
 * @brief Reliable, KeepLast(100), Volatile, ASync, Background priority.
 *
 * @details Designed for log and event streams that should be delivered reliably without
 * stealing cycles from the data plane.  KeepLast(100) buffers a short burst, asynchronous
 * dispatch at Background priority keeps logging off the hot path, a relaxed 5000 ms
 * liveliness lease suits the low cadence, and Lifespan is infinite (-1); no Deadline is
 * imposed because log volume is bursty.
 */
[[maybe_unused]] static inline constexpr Qos kLog{
    "log",
    true,
    Qos::Reliability{Qos::Reliability::kReliable},
    Qos::History{Qos::History::kKeepLast, 100},
    Qos::Durability{Qos::Durability::kVolatile},
    Qos::PublishMode{Qos::PublishMode::kASync},
    Qos::Liveliness{Qos::Liveliness::kAutomatic, 5000},
    Qos::DestinationOrder{},
    Qos::Ownership{},
    Qos::Deadline{},
    Qos::Lifespan{},
    Qos::LatencyBudget{},
    Qos::ResourceLimits{},
    Qos::Additions{Qos::Additions::kPriorityBackground, false},
};

/**
 * @brief Returns the name-to-@c Qos lookup table containing every profile in this namespace.
 *
 * @details
 * The map is keyed by profile name (e.g. @c "sensor", @c "event") and is safe to read
 * concurrently from any thread once initialised.
 *
 * @return Constant reference to the global @c unordered_map<string, Qos>.
 */
[[nodiscard]] VLINK_EXPORT const std::unordered_map<std::string, Qos>& get_available_qos_map() noexcept;

}  // namespace QosProfile

}  // namespace vlink
