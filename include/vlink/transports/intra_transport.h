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
 * @file intra_transport.h
 * @brief Transport configuration for the @c intra:// in-process / zero-copy transport.
 *
 * @details
 * @c IntraTransport binds the @c intra:// URL scheme to VLink's in-process message bus.
 * Publishers and subscribers that share the same OS process exchange messages
 * directly through an in-memory queue without crossing kernel or network
 * boundaries.  When the payload is an @c IntraDataType (a shared-pointer wrapper
 * around the message), serialisation is bypassed entirely and the pointer is
 * handed off zero-copy; all other types take the normal @c Serializer path.
 *
 * @par Zero-Copy Hand-off Path
 * @code
 *   Publisher                        MessageBus                        Subscriber
 *   ---------                        ----------                        ----------
 *   IntraDataType<T>  ----enqueue->  ring buffer (shared_ptr only) ->  IntraDataType<T>
 *   pod / proto / etc --serialize->  Bytes ------------------------->  deserialize -> T
 * @endcode
 *
 * @par Supported Node Types
 *
 * | Publisher | Subscriber | Server | Client | Getter | Setter |
 * | :-------: | :--------: | :----: | :----: | :----: | :----: |
 * | yes       | yes        | yes    | yes    | yes    | yes    |
 *
 * @par URL Format
 * @code
 *   intra://<address>[?event=<name>&pipeline=<id>][#<type>]
 * @endcode
 *
 * | Component   | Description                                                              |
 * | ----------- | ------------------------------------------------------------------------ |
 * | @c address  | In-process topic name (URL host concatenated with path); not empty       |
 * | @c event    | Optional secondary event filter (@c ?event=)                             |
 * | @c pipeline | Queue-mode pipeline ID (@c ?pipeline=); @c 0 selects the default pipe    |
 * | @c type     | URL fragment; @c "queue" (buffered) or @c "direct" (inline dispatch)     |
 *
 * @par Delivery Modes
 *
 * | Mode       | Wakeup latency | Threading                       | Use case                  |
 * | ---------- | -------------- | ------------------------------- | ------------------------- |
 * | @c queue   | Bounded        | Subscriber loop drains the bus  | Default; decoupled timing |
 * | @c direct  | Synchronous    | Caller thread invokes callback  | Hot loops, low latency    |
 *
 * @par Example
 * @code
 *   auto pub  = vlink::Publisher<MyMsg>::create_unique("intra://sensors/imu");
 *   auto sub  = vlink::Subscriber<MyMsg>::create_unique("intra://sensors/imu");
 *   auto fast = vlink::Publisher<MyMsg>::create_unique("intra://control/cmd#direct");
 *   auto pipe = vlink::Publisher<MyMsg>::create_unique("intra://svc?event=ev&pipeline=4");
 * @endcode
 *
 * @note Compiled only when @c VLINK_SUPPORT_INTRA is defined.
 * @note @c address must not be empty; @c is_valid() additionally requires @c type
 *       to be either @c "queue" or @c "direct".
 */

#pragma once

#ifdef VLINK_SUPPORT_INTRA

#include <cstdint>
#include <string>

#include "../impl/transport.h"

namespace vlink {

/**
 * @struct IntraTransport
 * @brief Concrete @c Transport describing an in-process endpoint addressed by an @c intra:// URL.
 *
 * @details
 * Fields map one-to-one onto the URL components: @c address from host plus path,
 * @c event from @c ?event=, @c pipeline from @c ?pipeline=, and @c type from the
 * URL fragment (@c "queue" or @c "direct").
 */
struct VLINK_EXPORT IntraTransport final : public Transport {
  std::string address;        ///< In-process topic address (URL host concatenated with path); must not be empty.
  std::string event;          ///< Optional secondary event filter string.
  int32_t pipeline{0};        ///< Queue-mode pipeline identifier; @c 0 selects the default pipe.
  std::string type{"queue"};  ///< Delivery mode; @c "queue" (buffered) or @c "direct" (inline dispatch).

  /**
   * @brief Builds an @c IntraTransport from its four logical fields.
   *
   * @param _address   Topic address string; must not be empty.
   * @param _event     Optional secondary event filter; empty by default.
   * @param _pipeline  Queue-mode pipeline ID; defaults to @c 0.
   * @param _type      Delivery mode; @c "queue" or @c "direct"; defaults to @c "queue".
   */
  explicit IntraTransport(const std::string& _address, const std::string& _event = "", int _pipeline = 0,
                          const std::string& _type = "queue");

  /**
   * @brief Component-wise equality on all configuration fields.
   *
   * @param transport  Configuration to compare with.
   * @return           @c true when @c address, @c event, @c pipeline and @c type all match.
   */
  [[nodiscard]] bool operator==(const IntraTransport& transport) const noexcept;

  /**
   * @brief Logical negation of @c operator==.
   *
   * @param transport  Configuration to compare with.
   * @return           @c true when any field differs from @p transport.
   */
  [[nodiscard]] bool operator!=(const IntraTransport& transport) const noexcept;

  /**
   * @brief Reports this object's transport tag.
   *
   * @return @c TransportType::kIntra.
   */
  [[nodiscard]] TransportType get_transport_type() const override;

#ifndef VLINK_ENABLE_C_INTERFACE
  VLINK_DECLARE_GLOBAL_PROPERTY()
#endif
  VLINK_ALLOW_IMPL_TYPE(kServer | kClient | kPublisher | kSubscriber | kSetter | kGetter)
  VLINK_TRANSPORT_IMPL(IntraTransport)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline IntraTransport::IntraTransport(const std::string& _address, const std::string& _event, int _pipeline,
                                      const std::string& _type)
    : address(_address), event(_event), pipeline(_pipeline), type(_type) {}

inline bool IntraTransport::operator==(const IntraTransport& transport) const noexcept {
  return address == transport.address && event == transport.event && pipeline == transport.pipeline &&
         type == transport.type;
}

inline bool IntraTransport::operator!=(const IntraTransport& transport) const noexcept { return !(*this == transport); }

inline TransportType IntraTransport::get_transport_type() const { return TransportType::kIntra; }

}  // namespace vlink

#endif
