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
 * @file qnx_conf.h
 * @brief Transport configuration for the @c qnx:// QNX native channel transport.
 *
 * @details
 * @c QnxConf binds the @c qnx:// URL scheme to the QNX Neutrino native IPC
 * primitives (channels and connections).  Messaging is performed directly through
 * @c ChannelCreate / @c ConnectAttach / @c MsgSend / @c MsgReceive without any
 * intermediate broker; this yields deterministic, low-jitter latency suitable
 * for hard real-time workloads.  The transport is available only when the binary
 * is compiled for a QNX target.
 *
 * @par Supported Node Types
 *
 * | Publisher | Subscriber | Server | Client | Getter | Setter |
 * | :-------: | :--------: | :----: | :----: | :----: | :----: |
 * | yes       | yes        | yes    | yes    | yes    | yes    |
 *
 * @par URL Format
 * @code
 *   qnx://<address>[?event=<name>]
 * @endcode
 *
 * | Component  | Description                                                            |
 * | ---------- | ---------------------------------------------------------------------- |
 * | @c address | QNX channel/topic name (URL host concatenated with path); not empty    |
 * | @c event   | Optional secondary event filter (@c ?event=)                           |
 *
 * @par Channel / Connection Topology
 * @code
 *   Publisher / Server                            Subscriber / Client
 *   ------------------                            ------------------------
 *   ChannelCreate(<address>)  <---- MsgSend ----  ConnectAttach(<address>)
 *           |                                              |
 *           v                                              v
 *      MsgReceive() loop                                MsgSend() / pulse
 * @endcode
 *
 * @par Example
 * @code
 *   auto pub = vlink::Publisher<MyMsg>::create_unique("qnx://control/cmd");
 *   auto sub = vlink::Subscriber<MyMsg>::create_unique("qnx://control/cmd");
 * @endcode
 *
 * @note Compiled only when @c VLINK_SUPPORT_QNX is defined.
 * @note QNX-only transport; not usable on Linux, Windows, or macOS hosts.
 * @note @c is_valid() returns @c false when @c address is empty.
 */

#pragma once

#ifdef VLINK_SUPPORT_QNX

#include <cstdint>
#include <string>

#include "../impl/conf.h"

namespace vlink {

/**
 * @struct QnxConf
 * @brief Concrete @c Conf describing a QNX-native channel endpoint addressed by a @c qnx:// URL.
 *
 * @details
 * Carries only the channel/topic name and an optional secondary event filter.
 * Other QNX-specific settings (priority inheritance, pulse code, etc.) are
 * inferred from the @c Qos object attached at endpoint construction time.
 */
struct VLINK_EXPORT QnxConf final : public Conf {
  std::string address;  ///< QNX channel/topic address (URL host concatenated with path); must not be empty.
  std::string event;    ///< Optional secondary event filter string.

  /**
   * @brief Builds a @c QnxConf from its two logical fields.
   *
   * @param _address  Channel/topic address string; must not be empty.
   * @param _event    Optional secondary event filter; empty by default.
   */
  explicit QnxConf(const std::string& _address, const std::string& _event = "");

  /**
   * @brief Component-wise equality on @c address and @c event.
   *
   * @param conf  Configuration to compare with.
   * @return      @c true when both fields match.
   */
  [[nodiscard]] bool operator==(const QnxConf& conf) const noexcept;

  /**
   * @brief Logical negation of @c operator==.
   *
   * @param conf  Configuration to compare with.
   * @return      @c true when either field differs from @p conf.
   */
  [[nodiscard]] bool operator!=(const QnxConf& conf) const noexcept;

  /**
   * @brief Reports this object's transport tag.
   *
   * @return @c TransportType::kQnx.
   */
  [[nodiscard]] TransportType get_transport_type() const override;

#ifndef VLINK_ENABLE_C_INTERFACE
  VLINK_DECLARE_GLOBAL_PROPERTY()
#endif
  VLINK_ALLOW_IMPL_TYPE(kServer | kClient | kPublisher | kSubscriber | kSetter | kGetter)
  VLINK_CONF_IMPL(QnxConf)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline QnxConf::QnxConf(const std::string& _address, const std::string& _event) : address(_address), event(_event) {}

inline bool QnxConf::operator==(const QnxConf& conf) const noexcept {
  return address == conf.address && event == conf.event;
}

inline bool QnxConf::operator!=(const QnxConf& conf) const noexcept { return !(*this == conf); }

inline TransportType QnxConf::get_transport_type() const { return TransportType::kQnx; }

}  // namespace vlink

#endif
