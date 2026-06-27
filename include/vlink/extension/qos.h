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
 * @file qos.h
 * @brief Quality of Service (QoS) policy aggregate for VLink publishers and subscribers.
 *
 * @details
 * @c Qos is a plain-old-data struct that bundles all DDS-compatible Quality of Service
 * parameters into a single aggregate.  It is passed to publishers, subscribers, clients,
 * servers, getters, and setters to control transport behaviour.
 *
 * The struct mirrors DDS QoS concepts and maps cleanly onto FastDDS, CycloneDDS, and
 * RTI DDS backends.  For non-DDS backends (shm, zenoh, intra, etc.), each backend
 * interprets the relevant subset of fields.
 *
 * Pre-built profiles are available in the @c QosProfile namespace (see @c qos_profile.h).
 *
 * | Sub-policy          | Key parameter                       | Default                            |
 * | ------------------- | ----------------------------------- | ---------------------------------- |
 * | @c Reliability      | @c kind, block_time, heartbeat_time | Reliable, 100 ms block, 3000 ms hb |
 * | @c History          | @c kind, depth                      | KeepLast, depth=1                  |
 * | @c Durability       | @c kind                             | Volatile                           |
 * | @c PublishMode      | @c kind                             | Sync                               |
 * | @c Liveliness       | @c kind, duration                   | Automatic, infinite                |
 * | @c DestinationOrder | @c kind                             | ReceptionTimestamp                 |
 * | @c Ownership        | @c kind                             | Shared                             |
 * | @c Deadline         | @c period                           | -1 (no deadline)                   |
 * | @c Lifespan         | @c duration                         | -1 (infinite)                      |
 * | @c LatencyBudget    | @c duration                         | 0 (best-effort)                    |
 * | @c ResourceLimits   | max_samples / instances             | 6000 / 10 / 500                    |
 * | @c Additions        | @c priority, @c is_express          | Normal, not express                |
 *
 * @par Example
 * @code
 * // Custom QoS: best-effort, keep last 10, volatile
 * vlink::Qos qos;
 * qos.reliability.kind  = vlink::Qos::Reliability::kBestEffort;
 * qos.history.kind      = vlink::Qos::History::kKeepLast;
 * qos.history.depth     = 10;
 * qos.durability.kind   = vlink::Qos::Durability::kVolatile;
 *
 * vlink::DdsConf::register_qos("my_qos", qos);
 * auto pub = vlink::Publisher<MyMsg>::create_unique("dds://my_topic?qos=my_qos");
 * @endcode
 */

#pragma once

#include <cstdint>

namespace vlink {

/**
 * @struct Qos
 * @brief Aggregate Quality of Service policy for a VLink communication endpoint.
 *
 * @details
 * All sub-policies have sensible defaults.  Only the fields relevant to the active
 * transport backend are used; unsupported fields are silently ignored.
 *
 * The @c name field (max 19 chars) identifies the profile for display purposes.
 * The @c valid flag must be @c true for the Qos to be applied; @c QosProfile
 * constants already set @c valid = @c true.
 */
struct Qos final {
  /**
   * @struct Reliability
   * @brief Controls whether message delivery is guaranteed.
   *
   * | Kind         | Behaviour                                              |
   * | ------------ | ------------------------------------------------------ |
   * | kBestEffort  | No retransmission; messages may be lost                |
   * | kReliable    | Retransmit until ACK or @c block_time expires          |
   */
  struct Reliability final {
    enum Kind : uint8_t {
      kBestEffort = 0,  ///< Fire-and-forget; no retransmission.
      kReliable = 1,    ///< Retransmit until acknowledged.
    };

    Kind kind{kReliable};          ///< Delivery guarantee kind.
    int32_t block_time{100};       ///< Max time (ms) a reliable write may block waiting for space.
    int32_t heartbeat_time{3000};  ///< Heartbeat interval (ms) for reliable delivery.
  };

  /**
   * @struct History
   * @brief Controls how many samples are kept for late-joining subscribers.
   *
   * | Kind       | Behaviour                                                |
   * | ---------- | -------------------------------------------------------- |
   * | kKeepLast  | Keep the @c depth most recent samples per instance       |
   * | kKeepAll   | Keep all samples (subject to ResourceLimits)             |
   */
  struct History final {
    enum Kind : uint8_t {
      kKeepLast = 0,  ///< Keep only the @c depth most recent samples.
      kKeepAll = 1,   ///< Keep all unread samples.
    };

    Kind kind{kKeepLast};  ///< History retention kind.
    int32_t depth{1};      ///< Number of samples to keep per instance (KeepLast only).
  };

  /**
   * @struct Durability
   * @brief Controls how samples persist after they are published.
   *
   * | Kind            | Behaviour                                               |
   * | --------------- | ------------------------------------------------------- |
   * | kVolatile       | No persistence; late joiners see only new samples       |
   * | kTransientLocal | Samples cached in the DataWriter; late joiners catch up |
   * | kTransient      | Samples persist in an external service                  |
   * | kPersistent     | Samples persist to stable storage                       |
   */
  struct Durability final {
    enum Kind : uint8_t {
      kVolatile = 0,        ///< No persistence beyond the DataWriter lifetime.
      kTransientLocal = 1,  ///< DataWriter caches samples for late-joining readers.
      kTransient = 2,       ///< Durability service stores samples.
      kPersistent = 3,      ///< Samples written to stable storage.
    };

    Kind kind{kVolatile};  ///< Durability kind.
  };

  /**
   * @struct PublishMode
   * @brief Controls whether the DataWriter sends synchronously or asynchronously.
   *
   * | Kind   | Behaviour                                                     |
   * | ------ | ------------------------------------------------------------- |
   * | kSync  | Write completes before returning to the caller                |
   * | kASync | Write is queued and sent by a background thread               |
   */
  struct PublishMode final {
    enum Kind : uint8_t {
      kSync = 0,   ///< Synchronous publish.
      kASync = 1,  ///< Asynchronous publish via background thread.
    };

    Kind kind{kSync};  ///< Publish mode.
  };

  /**
   * @struct Liveliness
   * @brief Controls how the liveness of a DataWriter is asserted and detected.
   *
   * | Kind                | Behaviour                                         |
   * | ------------------- | ------------------------------------------------- |
   * | kAutomatic          | Middleware asserts liveliness automatically       |
   * | kManualParticipant  | Application must assert at participant level      |
   * | kManualTopic        | Application must assert at topic level            |
   */
  struct Liveliness final {
    enum Kind : uint8_t {
      kAutomatic = 0,          ///< Automatic liveliness assertion.
      kManualParticipant = 1,  ///< Manual assertion at DomainParticipant level.
      kManualTopic = 2,        ///< Manual assertion at DataWriter level.
    };

    Kind kind{kAutomatic};  ///< Liveliness assertion kind.
    int32_t duration{-1};   ///< Lease duration in ms.  -1 = infinite.
  };

  /**
   * @struct DestinationOrder
   * @brief Controls the ordering of received samples.
   *
   * | Kind                 | Behaviour                                       |
   * | -------------------- | ----------------------------------------------- |
   * | kReceptionTimestamp  | Order by time the reader received the sample    |
   * | kSourceTimestamp     | Order by time the writer sent the sample        |
   */
  struct DestinationOrder final {
    enum Kind : uint8_t {
      kReceptionTimestamp = 0,  ///< Order by reception time.
      kSourceTimestamp = 1,     ///< Order by source write time.
    };

    Kind kind{kReceptionTimestamp};  ///< Sample ordering criterion.
  };

  /**
   * @struct Ownership
   * @brief Controls whether multiple writers can write to the same instance.
   *
   * | Kind       | Behaviour                                                 |
   * | ---------- | --------------------------------------------------------- |
   * | kShared    | Multiple writers may update the same instance             |
   * | kExclusive | Only the writer with the highest strength may update      |
   */
  struct Ownership final {
    enum Kind : uint8_t {
      kShared = 0,     ///< Multiple writers share ownership.
      kExclusive = 1,  ///< Only the highest-strength writer has ownership.
    };

    Kind kind{kShared};  ///< Ownership kind.
  };

  /**
   * @struct Deadline
   * @brief Specifies the maximum period between successive data publications.
   *
   * @details
   * A @c period of -1 means no deadline constraint.  When the deadline is
   * missed, the middleware fires a deadline-missed status event.
   */
  struct Deadline final {
    int32_t period{-1};  ///< Max interval between writes (ms).  -1 = no constraint.
  };

  /**
   * @struct Lifespan
   * @brief Specifies the maximum age of a sample before it is discarded.
   *
   * @details
   * A @c duration of -1 means samples never expire.
   */
  struct Lifespan final {
    int32_t duration{-1};  ///< Sample maximum age (ms).  -1 = infinite.
  };

  /**
   * @struct LatencyBudget
   * @brief Provides a hint about the maximum acceptable end-to-end latency.
   *
   * @details
   * A @c duration of 0 requests the lowest possible latency.  The middleware
   * may use this hint to schedule delivery.
   */
  struct LatencyBudget final {
    int32_t duration{0};  ///< Acceptable delivery latency (ms).  0 = best possible.
  };

  /**
   * @struct ResourceLimits
   * @brief Limits on the number of samples, instances, and samples per instance.
   *
   * @details
   * These limits apply to the DataWriter and DataReader internal queues.
   * Setting them too low may cause samples to be rejected under load.
   */
  struct ResourceLimits final {
    int32_t max_samples{6000};              ///< Maximum total samples across all instances.
    int32_t max_instances{10};              ///< Maximum number of instances.
    int32_t max_samples_per_instance{500};  ///< Maximum samples per instance.
  };

  /**
   * @struct Additions
   * @brief VLink-specific extensions beyond standard DDS QoS.
   *
   * @details
   * @c priority controls the task dispatch priority for priority-type message loops.
   * @c is_express hints that messages should bypass the normal queue and be sent immediately.
   */
  struct Additions final {
    /**
     * @brief Dispatch priority values for priority-aware loops.
     *
     * | Priority           | Value | Use case                      |
     * | ------------------ | ----- | ----------------------------- |
     * | kPriorityRealTime  |   1   | Hard real-time control        |
     * | kPriorityHigh      |   2   | High-priority events          |
     * | kPriorityNormal    |   4   | Standard application messages |
     * | kPriorityLow       |   6   | Background telemetry          |
     * | kPriorityBackground|   7   | Best-effort background tasks  |
     */
    enum Priority : uint8_t {
      kPriorityRealTime = 1,    ///< Highest priority; hard real-time.
      kPriorityHigh = 2,        ///< High-priority processing.
      kPriorityNormal = 4,      ///< Default application priority.
      kPriorityLow = 6,         ///< Low-priority background work.
      kPriorityBackground = 7,  ///< Lowest priority; background tasks.
    };

    Priority priority{kPriorityNormal};  ///< Task dispatch priority.
    bool is_express{false};              ///< If true, bypass normal queuing for immediate delivery.
  };

  char name[20] = {0};                 ///< Profile name (max 19 chars + NUL).  For display only.
  bool valid{false};                   ///< Must be true for the Qos to be applied by the transport.
  Reliability reliability;             ///< Delivery guarantee policy.
  History history;                     ///< Sample retention policy.
  Durability durability;               ///< Sample persistence policy.
  PublishMode publish_mode;            ///< Synchronous or asynchronous publishing.
  Liveliness liveliness;               ///< Liveliness assertion policy.
  DestinationOrder destination_order;  ///< Sample ordering policy.
  Ownership ownership;                 ///< Writer ownership policy.
  Deadline deadline;                   ///< Maximum period between publications.
  Lifespan lifespan;                   ///< Maximum sample age before discard.
  LatencyBudget latency_budget;        ///< Acceptable end-to-end latency hint.
  ResourceLimits resource_limits;      ///< Internal queue size limits.
  Additions additions;                 ///< VLink-specific extensions.
};

}  // namespace vlink
