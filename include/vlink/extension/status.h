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
 * @file status.h
 * @brief DDS-aligned status event hierarchy delivered to publisher and subscriber callbacks.
 *
 * @details
 * The @c Status namespace exposes a polymorphic, shared-pointer based event model that mirrors
 * the DDS status change notification system.  When the underlying transport (DDS, intra, shm,
 * zenoh, etc.) detects an interesting condition it constructs a concrete @c Base subclass and
 * forwards it to the user-registered @c StatusCallback.  Callers use @c Base::as<T>() to safely
 * narrow the pointer to the concrete type carried by the event.
 *
 * @par Status codes
 *
 * | Value | Enumerator                     | Side    | Triggered when                                       |
 * | ----- | ------------------------------ | ------- | ---------------------------------------------------- |
 * |  0    | @c kUnknown                    | -       | transport reports an unrecognised status type        |
 * |  1    | @c kPublicationMatched         | writer  | matching subscriber appeared or disappeared          |
 * |  2    | @c kOfferedDeadlineMissed      | writer  | offered publication deadline missed                  |
 * |  3    | @c kOfferedIncompatibleQos     | writer  | discovered subscriber with incompatible QoS          |
 * |  4    | @c kLivelinessLost             | writer  | writer failed to assert liveliness within lease      |
 * |  5    | @c kSubscriptionMatched        | reader  | matching publisher appeared or disappeared           |
 * |  6    | @c kRequestedDeadlineMissed    | reader  | reader missed its requested deadline                 |
 * |  7    | @c kLivelinessChanged          | reader  | matched publisher liveliness state changed           |
 * |  8    | @c kSampleRejected             | reader  | inbound sample dropped (resource limit)              |
 * |  9    | @c kRequestedIncompatibleQos   | reader  | discovered publisher with incompatible QoS           |
 * | 10    | @c kSampleLost                 | reader  | sample lost before delivery                          |
 *
 * @par Severity matrix
 *
 * | Status                          | Severity      | Action recommended                                      |
 * | ------------------------------- | ------------- | ------------------------------------------------------- |
 * | @c kPublicationMatched          | informational | log peer count change                                   |
 * | @c kSubscriptionMatched         | informational | log peer count change                                   |
 * | @c kLivelinessChanged           | informational | track @c alive_count delta                              |
 * | @c kOfferedDeadlineMissed       | warning       | investigate slow publisher loop                         |
 * | @c kRequestedDeadlineMissed     | warning       | investigate dropped traffic                             |
 * | @c kOfferedIncompatibleQos      | warning       | reconcile QoS profile with peer                         |
 * | @c kRequestedIncompatibleQos    | warning       | reconcile QoS profile with peer                         |
 * | @c kSampleRejected              | error         | enlarge @c ResourceLimits or drop sources               |
 * | @c kSampleLost                  | error         | enlarge @c History depth or upgrade reliability         |
 * | @c kLivelinessLost              | error         | peer considers writer dead; restore heartbeats          |
 *
 * Concrete event structs and their counter / handle fields live in @c status_detail.h.
 *
 * @par Example
 * @code
 *   auto sub = vlink::Subscriber<MyMsg>::create("dds://my/topic");
 *
 *   sub->register_status_callback([](vlink::Status::BasePtr status) {
 *     switch (status->get_type()) {
 *       case vlink::Status::kSubscriptionMatched: {
 *         auto detail = status->as<vlink::Status::SubscriptionMatched>();
 *         VLOG_I("matched publishers: ", detail->current_count);
 *         break;
 *       }
 *       case vlink::Status::kSampleLost: {
 *         auto detail = status->as<vlink::Status::SampleLost>();
 *         VLOG_W("samples lost so far: ", detail->total_count);
 *         break;
 *       }
 *       default:
 *         break;
 *     }
 *   });
 * @endcode
 */

#pragma once

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>

#include "../base/exception.h"
#include "../base/macros.h"

namespace vlink {

/**
 * @namespace vlink::Status
 * @brief DDS-aligned status enumeration, base event class, and helper predicates.
 */
namespace Status {  // NOLINT(readability-identifier-naming)

/**
 * @brief Discriminator that identifies the concrete @c Base subclass carried by an event.
 *
 * @details
 * Values @c 1..4 are emitted on the writer side, values @c 5..10 on the reader side; use
 * @c is_for_writer() / @c is_for_reader() to classify a value without inspecting the
 * concrete subclass.
 */
enum Type : uint8_t {
  kUnknown = 0,  ///< Placeholder for unrecognised status events.
  // -- For writer
  kPublicationMatched = 1,      ///< Matching subscriber appeared or disappeared.
  kOfferedDeadlineMissed = 2,   ///< Writer missed its offered publication deadline.
  kOfferedIncompatibleQos = 3,  ///< Discovered subscriber with incompatible QoS.
  kLivelinessLost = 4,          ///< Writer liveliness assertion lapsed.
  // -- For reader
  kSubscriptionMatched = 5,       ///< Matching publisher appeared or disappeared.
  kRequestedDeadlineMissed = 6,   ///< Reader missed its requested deadline.
  kLivelinessChanged = 7,         ///< Liveliness of a matched publisher changed.
  kSampleRejected = 8,            ///< Inbound sample dropped by resource limits.
  kRequestedIncompatibleQos = 9,  ///< Discovered publisher with incompatible QoS.
  kSampleLost = 10,               ///< Sample lost before delivery.
};

/**
 * @brief Reports whether @p type belongs to the writer-side group.
 *
 * @param type  Status type to classify.
 * @return @c true for values @c 1..4; @c false for @c kUnknown and reader-side values.
 */
[[nodiscard]] [[maybe_unused]] static constexpr bool is_for_writer(Type type) noexcept {
  return type != kUnknown && type < kSubscriptionMatched;
}

/**
 * @brief Reports whether @p type belongs to the reader-side group.
 *
 * @param type  Status type to classify.
 * @return @c true for values @c 5..10.
 */
[[nodiscard]] [[maybe_unused]] static constexpr bool is_for_reader(Type type) noexcept {
  return type >= kSubscriptionMatched;
}

/**
 * @brief Opaque transport-defined identifier for a matched publication or subscription.
 *
 * @details
 * Treat as an opaque key; the bit pattern is transport-specific and is only meaningful when
 * compared against handles obtained from the same transport.
 */
using InstanceHandle = const void*;

/**
 * @struct Base
 * @brief Polymorphic base for every concrete status event delivered to a callback.
 *
 * @details
 * Concrete subclasses live in @c status_detail.h and carry the counter, handle, and reason
 * fields specific to each event.  Subscribers downcast through @c as<T>(), which throws
 * @c Exception::RuntimeError for @c kUnknown events.  @c std::enable_shared_from_this allows
 * implementations to safely produce a @c shared_ptr to themselves from inside member calls.
 */
struct VLINK_EXPORT Base : public std::enable_shared_from_this<Base> {
 protected:
  Base();

  virtual ~Base();

 public:
  /**
   * @brief Returns the concrete event type discriminator.
   *
   * @return One of the @c Status::Type enumerators.
   */
  [[nodiscard]] virtual Type get_type() const = 0;

  /**
   * @brief Returns the event name without numeric fields.
   *
   * @return Short string such as @c "SubscriptionMatched".  Use @c operator<< for full detail.
   */
  [[nodiscard]] virtual std::string get_string() const = 0;

  /**
   * @brief Safely narrows this event to a concrete @c Status subclass.
   *
   * @tparam T  Concrete event struct derived from @c Base; must not be @c Status::Unknown.
   * @return @c shared_ptr<T> pointing at this event.
   * @throws Exception::RuntimeError when @c get_type() is @c kUnknown.
   */
  template <typename T>
  [[nodiscard]] std::shared_ptr<T> as() const;

  /**
   * @brief Writes the human-readable description of @p status to @p ostream.
   *
   * @param ostream  Output stream.
   * @param status   Event to print.
   * @return Reference to @p ostream.
   */
  VLINK_EXPORT friend std::ostream& operator<<(std::ostream& ostream, const Base& status) noexcept;
};

/**
 * @struct Unknown
 * @brief Placeholder event emitted when the transport reports a status the runtime cannot map.
 *
 * @details
 * Callbacks normally short-circuit on @c get_type() == @c kUnknown to avoid attempting an
 * invalid downcast.
 */
struct VLINK_EXPORT Unknown final : public Base {
 public:
  /**
   * @brief Returns @c kUnknown.
   *
   * @return Always @c kUnknown.
   */
  [[nodiscard]] Type get_type() const override;

  /**
   * @brief Returns the literal "Unknown".
   *
   * @return Descriptive string.
   */
  [[nodiscard]] std::string get_string() const override;

  /**
   * @brief Writes "Unknown" to @p ostream.
   *
   * @param ostream  Output stream.
   * @param status   This @c Unknown event.
   * @return Reference to @p ostream.
   */
  VLINK_EXPORT friend std::ostream& operator<<(std::ostream& ostream, const Unknown& status) noexcept;
};

/**
 * @brief Shared-pointer alias used as the parameter type of every status callback.
 */
using BasePtr = std::shared_ptr<Status::Base>;

/**
 * @brief Writes the human-readable description of @p status to @p ostream.
 *
 * @param ostream  Output stream.
 * @param status   Shared pointer to an event.
 * @return Reference to @p ostream.
 */
VLINK_EXPORT std::ostream& operator<<(std::ostream& ostream, const BasePtr& status) noexcept;

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

template <typename T>
inline std::shared_ptr<T> Base::as() const {
  static_assert(std::is_base_of_v<Base, T> && !std::is_same_v<struct Unknown, T>,
                "Can not convert target status type.");

  if VUNLIKELY (get_type() == kUnknown) {
    throw Exception::RuntimeError("Target status is unknown");
  }

#if defined(NDEBUG) || defined(__ANDROID__)
  return std::static_pointer_cast<T>(const_cast<Base*>(this)->shared_from_this());
#else
  return std::dynamic_pointer_cast<T>(const_cast<Base*>(this)->shared_from_this());
#endif
}

}  // namespace Status

}  // namespace vlink
