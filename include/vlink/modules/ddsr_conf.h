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
 * @file ddsr_conf.h
 * @brief Transport configuration for the @c ddsr:// RTI Connext DDS transport.
 *
 * @details
 * @c DdsrConf binds the @c ddsr:// URL scheme to RTI Connext DDS, the commercial
 * DDS stack widely used in safety-critical avionics, automotive ADAS, and industrial
 * automation deployments where deterministic real-time behaviour and certified
 * tooling are required.  The API surface mirrors @c DdsConf so an application can
 * switch between Fast-DDS and Connext by changing only the URL scheme.
 *
 * @par Supported Node Types
 *
 * | Publisher | Subscriber | Server | Client | Getter | Setter |
 * | :-------: | :--------: | :----: | :----: | :----: | :----: |
 * | yes       | yes        | yes    | yes    | yes    | yes    |
 *
 * @par URL Format
 * @code
 *   ddsr://<topic>[?domain=<N>&depth=<N>&qos=<profile>]
 *   ddsr://<topic>[?domain=<N>&part=<v>&topic=<v>&pub=<v>&sub=<v>&writer=<v>&reader=<v>]
 * @endcode
 *
 * | Component  | Description                                                                  |
 * | ---------- | ---------------------------------------------------------------------------- |
 * | @c topic   | RTI Connext topic name (URL host concatenated with path)                     |
 * | @c domain  | DDS Domain ID (@c ?domain=); defaults from the @c VLINK_DDS_DOMAIN env var   |
 * | @c depth   | Optional history-depth override; @c 0 keeps the QoS-selected depth           |
 * | @c qos     | Named QoS profile registered via @c register_qos() (@c ?qos=)                |
 * | @c qos_ext | Remaining query keys after @c domain, @c depth and @c qos are removed        |
 *
 * @par Backend-Specific Options
 *
 * | Option                 | Purpose                                                | Default |
 * | ---------------------- | ------------------------------------------------------ | ------- |
 * | XML QoS Library/Profile| Per-entity overrides via @c qos_ext keys               | none    |
 * | RPC reply suffix       | Auto-derived response topic name                       | ___resp |
 * | Domain ID              | DomainParticipant Domain joined by readers and writers | 0       |
 *
 * @par Example
 * @code
 *   vlink::Qos qos;
 *   qos.reliability.kind = vlink::Qos::Reliability::kReliable;
 *   vlink::DdsrConf::register_qos("rtps_reliable", qos);
 *
 *   auto pub = vlink::Publisher<MyMsg>::create_unique("ddsr://control/cmd?domain=3&qos=rtps_reliable");
 *   auto sub = vlink::Subscriber<MyMsg>::create_unique("ddsr://control/cmd?domain=3&qos=rtps_reliable");
 * @endcode
 *
 * @note Compiled only when @c VLINK_SUPPORT_DDSR is defined.
 * @note @c qos and @c qos_ext are mutually exclusive on the same instance.
 * @note RPC reply topics are derived by appending @c "___resp" to the topic name.
 */

#pragma once

#ifdef VLINK_SUPPORT_DDSR

#include <cstdint>
#include <functional>
#include <map>
#include <shared_mutex>
#include <string>

#include "../extension/qos.h"
#include "../impl/conf.h"

namespace vlink {

/**
 * @struct DdsrConf
 * @brief Concrete @c Conf describing an RTI Connext DDS endpoint addressed by a @c ddsr:// URL.
 *
 * @details
 * Holds the topic name, Domain ID, optional history-depth override, and either a
 * named QoS profile or a per-entity property map.
 */
struct VLINK_EXPORT DdsrConf final : public Conf {
  std::string topic;      ///< RTI Connext topic name (URL host concatenated with path).
  int32_t domain{0};      ///< DDS Domain ID joined by the underlying DomainParticipant.
  int32_t depth{0};       ///< Optional history-depth override; @c 0 keeps the QoS-selected depth.
  std::string qos;        ///< Named QoS profile key registered via @c register_qos().
  PropertiesMap qos_ext;  ///< Per-entity property map; populated from query keys outside @c domain / @c depth / @c qos.

  /**
   * @brief Builds a @c DdsrConf from topic, Domain, depth, and optional named QoS profile.
   *
   * @param _topic   RTI Connext topic name.
   * @param _domain  Domain ID; defaults to @c 0.
   * @param _depth   History-depth override; defaults to @c 0 (use QoS depth).
   * @param _qos     Named QoS profile key; empty by default.
   */
  explicit DdsrConf(const std::string& _topic, int32_t _domain = 0, int32_t _depth = 0, const std::string& _qos = "");

  /**
   * @brief Builds a @c DdsrConf from topic, Domain, and an explicit per-entity QoS map.
   *
   * @param _topic    RTI Connext topic name.
   * @param _domain   Domain ID.
   * @param _qos_ext  Property map carrying per-entity QoS overrides.
   */
  explicit DdsrConf(const std::string& _topic, int32_t _domain, const PropertiesMap& _qos_ext);

  /**
   * @brief Component-wise equality on all configuration fields.
   *
   * @param conf  Configuration to compare with.
   * @return      @c true when @c topic, @c domain, @c depth, @c qos and @c qos_ext all match.
   */
  [[nodiscard]] bool operator==(const DdsrConf& conf) const noexcept;

  /**
   * @brief Logical negation of @c operator==.
   *
   * @param conf  Configuration to compare with.
   * @return      @c true when any field differs from @p conf.
   */
  [[nodiscard]] bool operator!=(const DdsrConf& conf) const noexcept;

  /**
   * @brief Reports this object's transport tag.
   *
   * @return @c TransportType::kDdsr.
   */
  [[nodiscard]] TransportType get_transport_type() const override;

  /**
   * @brief Registers a named QoS profile that endpoints may reference via @c ?qos=.
   *
   * @details
   * Profile names share a global namespace.  Collisions with DDS-reserved tokens
   * (@c part, @c topic, @c pub, @c sub, @c writer, @c reader, @c depth) or with an
   * already registered profile abort with a fatal log entry.
   *
   * @param name  Unique profile key; must not collide with any reserved token.
   * @param qos   @c Qos value associated with the key.
   */
  static void register_qos(const std::string& name, const Qos& qos);

 private:
  static void register_qos_internal(const std::string& name, const Qos& qos);

  static const Qos& find_qos(const std::string& name);

  friend class DdsrFactory;
  static std::map<std::string, Qos> qos_map_;
  static std::shared_mutex mtx_;
  static constexpr const char* kRespSuffix{"___resp"};
#ifndef VLINK_ENABLE_C_INTERFACE
  VLINK_DECLARE_GLOBAL_PROPERTY()
#endif
  VLINK_ALLOW_IMPL_TYPE(kServer | kClient | kPublisher | kSubscriber | kSetter | kGetter)
  VLINK_CONF_IMPL(DdsrConf)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline DdsrConf::DdsrConf(const std::string& _topic, int32_t _domain, int32_t _depth, const std::string& _qos)
    : topic(_topic), domain(_domain), depth(_depth), qos(_qos) {}

inline DdsrConf::DdsrConf(const std::string& _topic, int32_t _domain, const PropertiesMap& _qos_ext)
    : topic(_topic), domain(_domain), qos_ext(_qos_ext) {}

inline bool DdsrConf::operator==(const DdsrConf& conf) const noexcept {
  return topic == conf.topic && domain == conf.domain && depth == conf.depth && qos == conf.qos &&
         qos_ext == conf.qos_ext;
}

inline bool DdsrConf::operator!=(const DdsrConf& conf) const noexcept { return !(*this == conf); }

inline TransportType DdsrConf::get_transport_type() const { return TransportType::kDdsr; }

}  // namespace vlink

#endif
