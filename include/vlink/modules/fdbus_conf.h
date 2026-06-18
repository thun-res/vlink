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
 * @file fdbus_conf.h
 * @brief Transport configuration for the @c fdbus:// FDBus IPC transport.
 *
 * @details
 * @c FdbusConf binds the @c fdbus:// URL scheme to FDBus, a D-Bus-inspired IPC
 * framework optimised for same-machine, multi-process communication in embedded
 * Linux systems.  FDBus exposes both a service-oriented (broker-mediated) mode and
 * a peer-to-peer IPC mode; the chosen mode is selected through the URL fragment.
 * The transport is in-host only -- it does not cross machine boundaries.
 *
 * @par Supported Node Types
 *
 * | Publisher | Subscriber | Server | Client | Getter | Setter |
 * | :-------: | :--------: | :----: | :----: | :----: | :----: |
 * | yes       | yes        | yes    | yes    | yes    | yes    |
 *
 * @par URL Format
 * @code
 *   fdbus://<address>[?event=<name>][#<transport>]
 * @endcode
 *
 * | Component    | Description                                                                |
 * | ------------ | -------------------------------------------------------------------------- |
 * | @c address   | FDBus service/topic address (URL host concatenated with path)              |
 * | @c event     | Optional secondary event name selector (@c ?event=)                        |
 * | @c transport | URL fragment; @c "svc" (service registry, default) or @c "ipc" (direct)    |
 *
 * @par Service / Endpoint Model
 *
 * | Role        | FDBus entity         | VLink mapping                                  |
 * | ----------- | -------------------- | ---------------------------------------------- |
 * | Publisher   | CBaseServer event    | Outgoing broadcast on @c address[/event]       |
 * | Subscriber  | CBaseClient event    | Listens on @c address[/event]                  |
 * | Server      | CBaseServer method   | Handles invokes routed to @c address           |
 * | Client      | CBaseClient invoker  | Calls @c address request method                |
 * | Setter      | CBaseServer property | Publishes latest value to subscribers          |
 * | Getter      | CBaseClient watcher  | Fetches and caches the latest value            |
 *
 * @par Example
 * @code
 *   auto pub_svc = vlink::Publisher<MyMsg>::create_unique("fdbus://my_service");
 *   auto pub_ipc = vlink::Publisher<MyMsg>::create_unique("fdbus://my_service#ipc");
 *
 *   vlink::FdbusConf conf("my_service", "my_event", "svc");
 *   auto pub_direct = vlink::Publisher<MyMsg>::create_unique(conf);
 * @endcode
 *
 * @note Compiled only when @c VLINK_SUPPORT_FDBUS is defined.
 * @note @c is_valid() returns @c false when @c address is empty or @c transport is
 *       neither @c "svc" nor @c "ipc".
 */

#pragma once

#ifdef VLINK_SUPPORT_FDBUS

#include <cstdint>
#include <string>

#include "../impl/conf.h"

namespace vlink {

/**
 * @struct FdbusConf
 * @brief Concrete @c Conf describing an FDBus IPC endpoint addressed by a @c fdbus:// URL.
 *
 * @details
 * Stores the FDBus service address, an optional secondary event name, and the
 * transport mode.  The fragment of the URL selects @c "svc" (default, service
 * registry) versus @c "ipc" (direct peer-to-peer).  Equality compares only
 * @c address and @c event so two endpoints sharing a topic but using different
 * transports remain interchangeable in identity-tracking maps.
 */
struct VLINK_EXPORT FdbusConf final : public Conf {
  std::string address;           ///< FDBus service/topic address (URL host concatenated with path).
  std::string event;             ///< Optional secondary event name selector.
  std::string transport{"svc"};  ///< Transport mode; @c "svc" (service registry) or @c "ipc" (direct IPC).

  /**
   * @brief Builds an @c FdbusConf from its three logical fields.
   *
   * @param _address    Service/topic address string.
   * @param _event      Optional event name; empty by default.
   * @param _transport  Transport mode; @c "svc" or @c "ipc"; defaults to @c "svc".
   */
  explicit FdbusConf(const std::string& _address, const std::string& _event = "",
                     const std::string& _transport = "svc");

  /**
   * @brief Identity-style equality that compares @c address and @c event only.
   *
   * @details
   * @c transport is intentionally excluded so the same logical endpoint can be
   * deduplicated regardless of whether it is reached through the service registry
   * or directly via IPC.
   *
   * @param conf  Configuration to compare with.
   * @return      @c true when @c address and @c event are equal.
   */
  [[nodiscard]] bool operator==(const FdbusConf& conf) const noexcept;

  /**
   * @brief Logical negation of @c operator==.
   *
   * @param conf  Configuration to compare with.
   * @return      @c true when @c address or @c event differ.
   */
  [[nodiscard]] bool operator!=(const FdbusConf& conf) const noexcept;

  /**
   * @brief Reports this object's transport tag.
   *
   * @return @c TransportType::kFdbus.
   */
  [[nodiscard]] TransportType get_transport_type() const override;

  /**
   * @brief Cheap, non-invasive probe for the FDBus @c name_server daemon.
   *
   * @details
   * Intended for CLI diagnostics, bench preflight, and self-test skip logic.  Does
   * not create any FDBus endpoint, does not open any connection, and is safe to
   * call repeatedly from any thread.
   *
   * @return @c true when an @c name_server process is currently running on the
   *         local host, @c false otherwise.
   */
  [[nodiscard]] static bool has_name_server();

#ifndef VLINK_ENABLE_C_INTERFACE
  VLINK_DECLARE_GLOBAL_PROPERTY()
#endif
  VLINK_ALLOW_IMPL_TYPE(kServer | kClient | kPublisher | kSubscriber | kSetter | kGetter)
  VLINK_CONF_IMPL(FdbusConf)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline FdbusConf::FdbusConf(const std::string& _address, const std::string& _event, const std::string& _transport)
    : address(_address), event(_event), transport(_transport) {}

inline bool FdbusConf::operator==(const FdbusConf& conf) const noexcept {
  return address == conf.address && event == conf.event;
}

inline bool FdbusConf::operator!=(const FdbusConf& conf) const noexcept { return !(*this == conf); }

inline TransportType FdbusConf::get_transport_type() const { return TransportType::kFdbus; }

}  // namespace vlink

#endif
