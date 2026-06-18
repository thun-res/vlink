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
 * @file mqtt_conf.h
 * @brief Transport configuration for the @c mqtt:// MQTT broker bridge.
 *
 * @details
 * @c MqttConf binds the @c mqtt:// URL scheme to the Eclipse Paho MQTT C client.
 * MQTT is a lightweight publish/subscribe protocol designed for constrained
 * devices, intermittent links, and low-bandwidth wide-area networks; it always
 * routes through a broker (no peer-to-peer mode).  Use this transport to bridge
 * VLink topics to a cloud / fleet MQTT broker, or to interoperate with existing
 * MQTT-based telemetry pipelines.
 *
 * @par Supported Node Types
 *
 * | Publisher | Subscriber | Server | Client | Getter | Setter |
 * | :-------: | :--------: | :----: | :----: | :----: | :----: |
 * | yes       | yes        | yes    | yes    | yes    | yes    |
 *
 * @par URL Format
 * @code
 *   mqtt://<address>[?event=<name>&domain=<N>&qos=<0|1|2>][#<broker_uri>]
 * @endcode
 *
 * | Component  | Description                                                                |
 * | ---------- | -------------------------------------------------------------------------- |
 * | @c address | MQTT topic path (URL host concatenated with path)                          |
 * | @c event   | Optional secondary event filter (@c ?event=)                               |
 * | @c domain  | Domain / namespace identifier (@c ?domain=); factory default applied       |
 * | @c qos     | MQTT QoS level @c 0, @c 1 or @c 2 (@c ?qos=); factory default applied      |
 * | @c fragment| Optional broker URI override carried in the URL fragment                   |
 *
 * @par Broker Connection
 *
 * | Property         | Source                       | Description                          |
 * | ---------------- | ---------------------------- | ------------------------------------ |
 * | Broker URI       | @c VLINK_MQTT_BROKER env var | @c tcp:// / @c ssl:// / @c ws:// URI |
 * | Domain ID        | @c VLINK_MQTT_DOMAIN env var | Default @c ?domain= when absent      |
 * | QoS level        | @c VLINK_MQTT_QOS env var    | Default @c 0, @c 1 or @c 2           |
 * | Keep-alive (s)   | @c VLINK_MQTT_KEEPALIVE env  | MQTT keep-alive interval             |
 * | Client ID prefix | @c VLINK_MQTT_CLIENT_ID env  | Prefix used for generated client IDs |
 *
 * @par TLS Configuration
 *
 * | Property              | Description                                                |
 * | --------------------- | ---------------------------------------------------------- |
 * | @c ssl:// URI scheme  | Selects TLS transport instead of plain TCP                 |
 * | CA certificate file   | Configured through @c set_property("mqtt.ca_file", path)   |
 * | Client certificate    | Configured through @c set_property("mqtt.cert_file", path) |
 * | Client private key    | Configured through @c set_property("mqtt.key_file", path)  |
 * | Username / password   | Set via @c mqtt.username and @c mqtt.password properties   |
 *
 * @par Example
 * @code
 *   // Defaults inherit broker URI and QoS from environment variables:
 *   auto pub = vlink::Publisher<MyMsg>::create_unique("mqtt://telemetry/state?qos=1");
 *
 *   // Override broker URI via URL fragment:
 *   auto sub = vlink::Subscriber<MyMsg>::create_unique("mqtt://telemetry/state#tcp://10.0.0.5:1883");
 * @endcode
 *
 * @note Compiled only when @c VLINK_SUPPORT_MQTT is defined.
 * @note URL parsing uses @c MqttFactory::get_default_domain_id() and
 *       @c MqttFactory::get_default_qos() when @c domain or @c qos are omitted;
 *       direct construction defaults remain @c domain=0 and @c qos=1.
 * @note @c is_valid() returns @c false when @c address is empty, @c domain is
 *       negative, or @c qos is outside @c [0, 2].
 */

#pragma once

#ifdef VLINK_SUPPORT_MQTT

#include <cstdint>
#include <map>
#include <shared_mutex>
#include <string>

#include "../impl/conf.h"

namespace vlink {

/**
 * @struct MqttConf
 * @brief Concrete @c Conf describing an MQTT endpoint addressed by an @c mqtt:// URL.
 *
 * @details
 * Stores the MQTT topic path, optional secondary event filter, domain identifier,
 * MQTT QoS level, and an optional broker URI override carried in the URL fragment.
 */
struct VLINK_EXPORT MqttConf final : public Conf {
  std::string address;   ///< MQTT topic path (URL host concatenated with path).
  std::string event;     ///< Optional secondary event filter string.
  int32_t domain{0};     ///< Domain / namespace identifier (non-negative).
  int32_t qos{1};        ///< MQTT QoS level; @c 0, @c 1 or @c 2.
  std::string fragment;  ///< Optional broker URI override carried in the URL fragment.

  /**
   * @brief Builds an @c MqttConf from its five logical fields.
   *
   * @param _address   MQTT topic path.
   * @param _event     Optional event filter; empty by default.
   * @param _domain    Domain identifier; defaults to @c 0.
   * @param _qos       MQTT QoS level; defaults to @c 1.
   * @param _fragment  Optional broker URI override; empty by default.
   */
  explicit MqttConf(const std::string& _address, const std::string& _event = "", int32_t _domain = 0, int32_t _qos = 1,
                    const std::string& _fragment = "");

  /**
   * @brief Component-wise equality on all configuration fields.
   *
   * @param conf  Configuration to compare with.
   * @return      @c true when every field of @c *this matches @p conf.
   */
  [[nodiscard]] bool operator==(const MqttConf& conf) const noexcept;

  /**
   * @brief Logical negation of @c operator==.
   *
   * @param conf  Configuration to compare with.
   * @return      @c true when any field differs from @p conf.
   */
  [[nodiscard]] bool operator!=(const MqttConf& conf) const noexcept;

  /**
   * @brief Reports this object's transport tag.
   *
   * @return @c TransportType::kMqtt.
   */
  [[nodiscard]] TransportType get_transport_type() const override;

  static constexpr const char* kRespSuffix{"___resp"};  ///< Suffix appended to RPC reply topic names.

#ifndef VLINK_ENABLE_C_INTERFACE
  VLINK_DECLARE_GLOBAL_PROPERTY()
#endif
  VLINK_ALLOW_IMPL_TYPE(kServer | kClient | kPublisher | kSubscriber | kSetter | kGetter)
  VLINK_CONF_IMPL(MqttConf)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline MqttConf::MqttConf(const std::string& _address, const std::string& _event, int32_t _domain, int32_t _qos,
                          const std::string& _fragment)
    : address(_address), event(_event), domain(_domain), qos(_qos), fragment(_fragment) {}

inline bool MqttConf::operator==(const MqttConf& conf) const noexcept {
  return address == conf.address && event == conf.event && domain == conf.domain && qos == conf.qos &&
         fragment == conf.fragment;
}

inline bool MqttConf::operator!=(const MqttConf& conf) const noexcept { return !(*this == conf); }

inline TransportType MqttConf::get_transport_type() const { return TransportType::kMqtt; }

}  // namespace vlink

#endif
