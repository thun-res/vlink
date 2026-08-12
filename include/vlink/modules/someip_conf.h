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
 * @file someip_conf.h
 * @brief Transport configuration for the @c someip:// AUTOSAR SOME/IP transport.
 *
 * @details
 * @c SomeipConf binds the @c someip:// URL scheme to OpenSOMEIP, an open-source
 * implementation of the AUTOSAR SOME/IP (Scalable service-Oriented MiddlewarE
 * over IP) protocol.  SOME/IP is the standard service-oriented
 * communication protocol used in modern automotive Ethernet backbones; it carries
 * RPC method calls, event broadcasts and notifiable field updates between ECUs.
 * Unlike string-named transports, SOME/IP identifies endpoints purely through
 * 16-bit numeric identifiers.
 *
 * @par Supported Node Types
 *
 * | Publisher | Subscriber | Server | Client | Getter | Setter |
 * | :-------: | :--------: | :----: | :----: | :----: | :----: |
 * | yes       | yes        | yes    | yes    | yes    | yes    |
 *
 * @par SOME/IP Identifier Model
 *
 * | Field         | Applies to         | Description                                    |
 * | ------------- | ------------------ | ---------------------------------------------- |
 * | @c service    | all node types     | 16-bit SOME/IP Service ID                      |
 * | @c instance   | all node types     | 16-bit Service Instance ID                     |
 * | @c method     | Server / Client    | 16-bit Method ID for RPC interfaces            |
 * | @c groups     | Pub/Sub + Field    | Set of 16-bit Event Group IDs                  |
 * | @c event      | Pub/Sub + Field    | 16-bit Event ID within the selected groups     |
 * | @c field      | Setter / Getter    | @c true marks the endpoint as a SOME/IP field  |
 *
 * @par URL Format
 * @code
 *   // RPC (Server / Client):
 *   someip://<service>/<instance>?method=<method_id>
 *
 *   // Event (Publisher / Subscriber):
 *   someip://<service>/<instance>?groups=<g1|g2|...>&event=<event_id>
 *
 *   // Field (Setter / Getter):
 *   someip://<service>/<instance>?groups=<g1|g2|...>&event=<event_id>&field=1
 * @endcode
 *
 * Numeric values are parsed by @c Helpers::to_int(), so decimal, @c 0x-prefixed
 * hexadecimal, and leading-zero octal forms are all accepted.  @c service and
 * @c instance must be non-zero.
 *
 * @par Example
 * @code
 *   // RPC server: service 0x1234, instance 0x5678, method 0x0001
 *   auto server = vlink::Server<MyReq, MyResp>::create_unique("someip://4660/22136?method=1");
 *
 *   // Event publisher: service 0x1234, instance 0x5678, group 0x0001, event 0x0010
 *   auto pub = vlink::Publisher<MyMsg>::create_unique("someip://4660/22136?groups=1&event=16");
 *
 *   // Direct construction:
 *   vlink::SomeipConf conf(0x1234, 0x5678, {0x0001}, 0x0010);
 *   auto pub2 = vlink::Publisher<MyMsg>::create_unique(conf);
 * @endcode
 *
 * @par VLink SOME/IP Configuration
 * A VLink SOME/IP JSON configuration file can be loaded process-wide before any
 * endpoint is created:
 * @code
 *   vlink::SomeipConf::load_global_config_file("/etc/vlink/opensomeip.json");
 * @endcode
 * Node-local @c someip.* properties set before explicit initialisation override
 * file, environment and global-property values.
 *
 * @note Compiled only when @c VLINK_SUPPORT_SOMEIP is defined.
 * @note @c service and @c instance must both be non-zero; otherwise @c is_valid()
 *       returns @c false.
 * @note For @c kPublisher / @c kSubscriber / @c kSetter / @c kGetter, both
 *       @c groups and @c event must be set.
 * @note For @c kSetter / @c kGetter, @c field must be @c true.
 * @note Method ID @c 0xFFFE is reserved by the VLink peer-control extension.
 */

#pragma once

#ifdef VLINK_SUPPORT_SOMEIP

#include <cstdint>
#include <set>
#include <string>

#include "../impl/conf.h"

namespace vlink {

/**
 * @struct SomeipConf
 * @brief Concrete @c Conf describing a SOME/IP endpoint addressed by a @c someip:// URL.
 *
 * @details
 * Two constructor overloads cover the two SOME/IP communication patterns: one for
 * method-based RPC and one for event-group-based publish/subscribe and field
 * synchronisation.  The @c field flag distinguishes events from fields when the
 * group-based constructor is used.
 */
struct VLINK_EXPORT SomeipConf final : public Conf {
  using Groups = std::set<uint16_t>;  ///< Type alias for the set of SOME/IP Event Group IDs.

  uint16_t service{0};   ///< SOME/IP Service ID; must be non-zero for a valid configuration.
  uint16_t instance{0};  ///< SOME/IP Service Instance ID; must be non-zero.
  uint16_t method{0};    ///< SOME/IP Method ID; used by @c kServer / @c kClient endpoints only.
  Groups groups;         ///< Set of SOME/IP Event Group IDs; required for event and field endpoints.
  uint16_t event{0};     ///< SOME/IP Event ID within the selected groups; required for event / field endpoints.
  bool field{false};     ///< @c true when this endpoint is a SOME/IP field (@c kSetter / @c kGetter).

  /**
   * @brief Builds a @c SomeipConf for an RPC endpoint (Server or Client).
   *
   * @param _service   SOME/IP Service ID.
   * @param _instance  SOME/IP Service Instance ID.
   * @param _method    SOME/IP Method ID identifying the RPC interface.
   */
  explicit SomeipConf(uint16_t _service, uint16_t _instance, uint16_t _method);

  /**
   * @brief Builds a @c SomeipConf for an event-group endpoint (Pub/Sub or Setter/Getter).
   *
   * @param _service   SOME/IP Service ID.
   * @param _instance  SOME/IP Service Instance ID.
   * @param _groups    Set of SOME/IP Event Group IDs to subscribe to.
   * @param _event     SOME/IP Event ID within the chosen groups.
   * @param _field     @c true for field-style (Setter/Getter), @c false for event-style (Pub/Sub).
   */
  explicit SomeipConf(uint16_t _service, uint16_t _instance, const Groups& _groups, uint16_t _event,
                      bool _field = false);

  /**
   * @brief Component-wise equality on all configuration fields.
   *
   * @param conf  Configuration to compare with.
   * @return      @c true when @c service, @c instance, @c method, @c groups, @c event and @c field all match.
   */
  [[nodiscard]] bool operator==(const SomeipConf& conf) const noexcept;

  /**
   * @brief Logical negation of @c operator==.
   *
   * @param conf  Configuration to compare with.
   * @return      @c true when any field differs from @p conf.
   */
  [[nodiscard]] bool operator!=(const SomeipConf& conf) const noexcept;

  /**
   * @brief Reports this object's transport tag.
   *
   * @return @c TransportType::kSomeip.
   */
  [[nodiscard]] TransportType get_transport_type() const override;

  /**
   * @brief Loads a VLink SOME/IP JSON configuration file as the process-wide default.
   *
   * @details
   * Delegates to @c SomeipFactory::load_global_config_file().  The loader accepts
   * an @c opensomeip object containing nested @c someip.* settings and the common
   * unicast, service-discovery and services fields used by existing SOME/IP JSON
   * deployments.  Call before creating any @c someip:// endpoint.
   *
   * @param filepath  Path to the JSON configuration file.
   * @return          @c true when the file was parsed and installed, @c false otherwise.
   */
  static bool load_global_config_file(const std::string& filepath);

#ifndef VLINK_ENABLE_C_INTERFACE
  VLINK_DECLARE_GLOBAL_PROPERTY()
#endif
  VLINK_ALLOW_IMPL_TYPE(kServer | kClient | kPublisher | kSubscriber | kSetter | kGetter)
  VLINK_CONF_IMPL(SomeipConf)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline SomeipConf::SomeipConf(uint16_t _service, uint16_t _instance, uint16_t _method)
    : service(_service), instance(_instance), method(_method) {}

inline SomeipConf::SomeipConf(uint16_t _service, uint16_t _instance, const Groups& _groups, uint16_t _event,
                              bool _field)
    : service(_service), instance(_instance), groups(_groups), event(_event), field(_field) {}

inline bool SomeipConf::operator==(const SomeipConf& conf) const noexcept {
  return service == conf.service && instance == conf.instance && method == conf.method && groups == conf.groups &&
         event == conf.event && field == conf.field;
}

inline bool SomeipConf::operator!=(const SomeipConf& conf) const noexcept { return !(*this == conf); }

inline TransportType SomeipConf::get_transport_type() const { return TransportType::kSomeip; }

}  // namespace vlink

#endif
