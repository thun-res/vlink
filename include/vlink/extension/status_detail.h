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
 * @file status_detail.h
 * @brief Concrete @c Status event structs carrying DDS-style counters, handles, and reason codes.
 *
 * @details
 * Each struct here implements one entry in @c Status::Type and exposes the precise fields the
 * matching DDS event provides.  Counter fields cover lifetime totals and per-notification deltas
 * so user code can decide whether to react.  Handles point at the most recently affected peer
 * or instance and follow the opaque @c InstanceHandle protocol described in @c status.h.
 *
 * @par Detail fields by event
 *
 * | Event                       | Counter fields                                | Handle / reason field           |
 * | --------------------------- | --------------------------------------------- | ------------------------------- |
 * | @c PublicationMatched       | total / current count + deltas                | @c last_subscription_handle     |
 * | @c OfferedDeadlineMissed    | @c total_count, @c total_count_change         | @c last_instance_handle         |
 * | @c OfferedIncompatibleQos   | @c total_count, @c total_count_change         | @c last_policy_id               |
 * | @c LivelinessLost           | @c total_count, @c total_count_change         | -                               |
 * | @c SubscriptionMatched      | total / current count + deltas                | @c last_publication_handle      |
 * | @c RequestedDeadlineMissed  | @c total_count, @c total_count_change         | @c last_instance_handle         |
 * | @c LivelinessChanged        | alive / not-alive count + deltas              | @c last_publication_handle      |
 * | @c SampleRejected           | @c total_count, @c total_count_change         | @c last_reason + handle         |
 * | @c RequestedIncompatibleQos | @c total_count, @c total_count_change         | @c last_policy_id               |
 * | @c SampleLost               | @c total_count, @c total_count_change         | -                               |
 *
 * @par Example
 * @code
 *   sub->register_status_callback([](vlink::Status::BasePtr status) {
 *     if (status->get_type() == vlink::Status::kSampleRejected) {
 *       auto detail = status->as<vlink::Status::SampleRejected>();
 *       if (detail->last_reason == vlink::Status::SampleRejected::kRejectedBySamplesLimit) {
 *         VLOG_W("rejected: queue full, total=", detail->total_count);
 *       }
 *     }
 *   });
 * @endcode
 */

#pragma once

#include <cstdint>
#include <string>

#include "./status.h"

namespace vlink {

namespace Status {

/* ================== For writer ================== */

/**
 * @struct PublicationMatched
 * @brief Writer-side event raised when a matching subscriber appears or disappears.
 *
 * @details
 * Provides cumulative and current counts of matched subscribers together with the most recent
 * subscription handle.  Negative @c current_count_change indicates a peer was removed.
 */
struct VLINK_EXPORT PublicationMatched final : public Base {
 public:
  /**
   * @brief Returns @c kPublicationMatched.
   *
   * @return Status type discriminator.
   */
  [[nodiscard]] Type get_type() const override;

  /**
   * @brief Returns the literal @c "PublicationMatched".
   *
   * @return Event name string.
   */
  [[nodiscard]] std::string get_string() const override;

  int32_t total_count{0};                            ///< Cumulative subscribers ever matched.
  int32_t total_count_change{0};                     ///< Delta in @c total_count since the last notification.
  int32_t current_count{0};                          ///< Subscribers currently matched.
  int32_t current_count_change{0};                   ///< Delta in @c current_count since the last notification.
  InstanceHandle last_subscription_handle{nullptr};  ///< Opaque handle of the subscriber that triggered this event.

  /**
   * @brief Streams the counter fields to @p ostream.
   *
   * @param ostream  Output stream.
   * @param status   Event to print.
   * @return Reference to @p ostream.
   */
  VLINK_EXPORT friend std::ostream& operator<<(std::ostream& ostream, const PublicationMatched& status) noexcept;
};

/**
 * @struct OfferedDeadlineMissed
 * @brief Writer-side event raised when the writer fails to publish within its offered deadline.
 *
 * @details
 * Fires once per missed instance; @c last_instance_handle identifies the most recent offender.
 */
struct VLINK_EXPORT OfferedDeadlineMissed final : public Base {
 public:
  /**
   * @brief Returns @c kOfferedDeadlineMissed.
   *
   * @return Status type discriminator.
   */
  [[nodiscard]] Type get_type() const override;

  /**
   * @brief Returns the literal @c "OfferedDeadlineMissed".
   *
   * @return Event name string.
   */
  [[nodiscard]] std::string get_string() const override;

  int32_t total_count{0};                        ///< Cumulative deadline misses across all instances.
  int32_t total_count_change{0};                 ///< Delta in @c total_count since the last notification.
  InstanceHandle last_instance_handle{nullptr};  ///< Opaque handle of the instance that missed most recently.

  /**
   * @brief Streams the counter fields to @p ostream.
   *
   * @param ostream  Output stream.
   * @param status   Event to print.
   * @return Reference to @p ostream.
   */
  VLINK_EXPORT friend std::ostream& operator<<(std::ostream& ostream, const OfferedDeadlineMissed& status) noexcept;
};

/**
 * @struct OfferedIncompatibleQos
 * @brief Writer-side event raised when a subscriber is rejected for incompatible QoS.
 */
struct VLINK_EXPORT OfferedIncompatibleQos final : public Base {
 public:
  /**
   * @brief Returns @c kOfferedIncompatibleQos.
   *
   * @return Status type discriminator.
   */
  [[nodiscard]] Type get_type() const override;

  /**
   * @brief Returns the literal @c "OfferedIncompatibleQos".
   *
   * @return Event name string.
   */
  [[nodiscard]] std::string get_string() const override;

  int32_t total_count{0};         ///< Cumulative incompatible subscribers detected.
  int32_t total_count_change{0};  ///< Delta in @c total_count since the last notification.
  int32_t last_policy_id{0};      ///< DDS QoS policy identifier that caused the rejection.

  /**
   * @brief Streams the counter fields to @p ostream.
   *
   * @param ostream  Output stream.
   * @param status   Event to print.
   * @return Reference to @p ostream.
   */
  VLINK_EXPORT friend std::ostream& operator<<(std::ostream& ostream, const OfferedIncompatibleQos& status) noexcept;
};

/**
 * @struct LivelinessLost
 * @brief Writer-side event raised when the liveliness lease lapses without assertion.
 *
 * @details
 * Peers will consider the writer dead until liveliness is re-asserted.
 */
struct VLINK_EXPORT LivelinessLost final : public Base {
 public:
  /**
   * @brief Returns @c kLivelinessLost.
   *
   * @return Status type discriminator.
   */
  [[nodiscard]] Type get_type() const override;

  /**
   * @brief Returns the literal @c "LivelinessLost".
   *
   * @return Event name string.
   */
  [[nodiscard]] std::string get_string() const override;

  int32_t total_count{0};         ///< Cumulative liveliness-lost events emitted.
  int32_t total_count_change{0};  ///< Delta in @c total_count since the last notification.

  /**
   * @brief Streams the counter fields to @p ostream.
   *
   * @param ostream  Output stream.
   * @param status   Event to print.
   * @return Reference to @p ostream.
   */
  VLINK_EXPORT friend std::ostream& operator<<(std::ostream& ostream, const LivelinessLost& status) noexcept;
};

/* ================== For reader ================== */

/**
 * @struct SubscriptionMatched
 * @brief Reader-side event raised when a matching publisher appears or disappears.
 */
struct VLINK_EXPORT SubscriptionMatched final : public Base {
 public:
  /**
   * @brief Returns @c kSubscriptionMatched.
   *
   * @return Status type discriminator.
   */
  [[nodiscard]] Type get_type() const override;

  /**
   * @brief Returns the literal @c "SubscriptionMatched".
   *
   * @return Event name string.
   */
  [[nodiscard]] std::string get_string() const override;

  int32_t total_count{0};                           ///< Cumulative publishers ever matched.
  int32_t total_count_change{0};                    ///< Delta in @c total_count since the last notification.
  int32_t current_count{0};                         ///< Publishers currently matched.
  int32_t current_count_change{0};                  ///< Delta in @c current_count since the last notification.
  InstanceHandle last_publication_handle{nullptr};  ///< Opaque handle of the publisher that triggered this event.

  /**
   * @brief Streams the counter fields to @p ostream.
   *
   * @param ostream  Output stream.
   * @param status   Event to print.
   * @return Reference to @p ostream.
   */
  VLINK_EXPORT friend std::ostream& operator<<(std::ostream& ostream, const SubscriptionMatched& status) noexcept;
};

/**
 * @struct RequestedDeadlineMissed
 * @brief Reader-side event raised when the reader does not receive data within its requested deadline.
 */
struct VLINK_EXPORT RequestedDeadlineMissed final : public Base {
 public:
  /**
   * @brief Returns @c kRequestedDeadlineMissed.
   *
   * @return Status type discriminator.
   */
  [[nodiscard]] Type get_type() const override;

  /**
   * @brief Returns the literal @c "RequestedDeadlineMissed".
   *
   * @return Event name string.
   */
  [[nodiscard]] std::string get_string() const override;

  int32_t total_count{0};                        ///< Cumulative deadline misses across all instances.
  int32_t total_count_change{0};                 ///< Delta in @c total_count since the last notification.
  InstanceHandle last_instance_handle{nullptr};  ///< Opaque handle of the instance that missed most recently.

  /**
   * @brief Streams the counter fields to @p ostream.
   *
   * @param ostream  Output stream.
   * @param status   Event to print.
   * @return Reference to @p ostream.
   */
  VLINK_EXPORT friend std::ostream& operator<<(std::ostream& ostream, const RequestedDeadlineMissed& status) noexcept;
};

/**
 * @struct LivelinessChanged
 * @brief Reader-side event raised when the liveliness of a matched publisher changes.
 *
 * @details
 * The @c alive_count and @c not_alive_count fields hold the current totals; the matching
 * @c *_change fields hold the delta since the last notification.
 */
struct VLINK_EXPORT LivelinessChanged final : public Base {
 public:
  /**
   * @brief Returns @c kLivelinessChanged.
   *
   * @return Status type discriminator.
   */
  [[nodiscard]] Type get_type() const override;

  /**
   * @brief Returns the literal @c "LivelinessChanged".
   *
   * @return Event name string.
   */
  [[nodiscard]] std::string get_string() const override;

  int32_t alive_count{0};                           ///< Matched publishers currently considered alive.
  int32_t not_alive_count{0};                       ///< Matched publishers currently considered not alive.
  int32_t alive_count_change{0};                    ///< Delta in @c alive_count since the last notification.
  int32_t not_alive_count_change{0};                ///< Delta in @c not_alive_count since the last notification.
  InstanceHandle last_publication_handle{nullptr};  ///< Opaque handle of the publisher whose state changed.

  /**
   * @brief Streams the counter fields to @p ostream.
   *
   * @param ostream  Output stream.
   * @param status   Event to print.
   * @return Reference to @p ostream.
   */
  VLINK_EXPORT friend std::ostream& operator<<(std::ostream& ostream, const LivelinessChanged& status) noexcept;
};

/**
 * @struct SampleRejected
 * @brief Reader-side event raised when an inbound sample is dropped due to a resource limit.
 */
struct VLINK_EXPORT SampleRejected final : public Base {
 public:
  /**
   * @brief Reason codes describing which resource ceiling rejected the sample.
   *
   * | Enumerator                              | Meaning                                   |
   * | --------------------------------------- | ----------------------------------------- |
   * | @c kNotRejected                         | placeholder; sample was not rejected      |
   * | @c kRejectedByInstancesLimit            | @c max_instances exhausted                |
   * | @c kRejectedBySamplesLimit              | @c max_samples exhausted                  |
   * | @c kRejectedBySamplesPerInstanceLimit   | @c max_samples_per_instance exhausted     |
   */
  enum Kind : uint8_t {
    kNotRejected = 0,                       ///< Placeholder reason; no rejection occurred.
    kRejectedByInstancesLimit = 1,          ///< Reader exhausted its instance budget.
    kRejectedBySamplesLimit = 2,            ///< Reader exhausted its total-sample budget.
    kRejectedBySamplesPerInstanceLimit = 3  ///< Reader exhausted its per-instance budget.
  };

  /**
   * @brief Returns @c kSampleRejected.
   *
   * @return Status type discriminator.
   */
  [[nodiscard]] Type get_type() const override;

  /**
   * @brief Returns the literal @c "SampleRejected".
   *
   * @return Event name string.
   */
  [[nodiscard]] std::string get_string() const override;

  int32_t total_count{0};                        ///< Cumulative samples rejected.
  int32_t total_count_change{0};                 ///< Delta in @c total_count since the last notification.
  Kind last_reason{kNotRejected};                ///< Reason code for the most recent rejection.
  InstanceHandle last_instance_handle{nullptr};  ///< Opaque handle of the rejected instance.

  /**
   * @brief Streams the counter and reason fields to @p ostream.
   *
   * @param ostream  Output stream.
   * @param status   Event to print.
   * @return Reference to @p ostream.
   */
  VLINK_EXPORT friend std::ostream& operator<<(std::ostream& ostream, const SampleRejected& status) noexcept;
};

/**
 * @struct RequestedIncompatibleQos
 * @brief Reader-side event raised when a publisher is rejected for incompatible QoS.
 */
struct VLINK_EXPORT RequestedIncompatibleQos final : public Base {
 public:
  /**
   * @brief Returns @c kRequestedIncompatibleQos.
   *
   * @return Status type discriminator.
   */
  [[nodiscard]] Type get_type() const override;

  /**
   * @brief Returns the literal @c "RequestedIncompatibleQos".
   *
   * @return Event name string.
   */
  [[nodiscard]] std::string get_string() const override;

  int32_t total_count{0};         ///< Cumulative incompatible publishers detected.
  int32_t total_count_change{0};  ///< Delta in @c total_count since the last notification.
  int32_t last_policy_id{0};      ///< DDS QoS policy identifier that caused the rejection.

  /**
   * @brief Streams the counter fields to @p ostream.
   *
   * @param ostream  Output stream.
   * @param status   Event to print.
   * @return Reference to @p ostream.
   */
  VLINK_EXPORT friend std::ostream& operator<<(std::ostream& ostream, const RequestedIncompatibleQos& status) noexcept;
};

/**
 * @struct SampleLost
 * @brief Reader-side event raised when a sample is lost between writer and reader.
 *
 * @details
 * Typically caused by writer rate exceeding the reader's @c History depth.
 */
struct VLINK_EXPORT SampleLost final : public Base {
 public:
  /**
   * @brief Returns @c kSampleLost.
   *
   * @return Status type discriminator.
   */
  [[nodiscard]] Type get_type() const override;

  /**
   * @brief Returns the literal @c "SampleLost".
   *
   * @return Event name string.
   */
  [[nodiscard]] std::string get_string() const override;

  int32_t total_count{0};         ///< Cumulative samples lost.
  int32_t total_count_change{0};  ///< Delta in @c total_count since the last notification.

  /**
   * @brief Streams the counter fields to @p ostream.
   *
   * @param ostream  Output stream.
   * @param status   Event to print.
   * @return Reference to @p ostream.
   */
  VLINK_EXPORT friend std::ostream& operator<<(std::ostream& ostream, const SampleLost& status) noexcept;
};

}  // namespace Status

}  // namespace vlink
