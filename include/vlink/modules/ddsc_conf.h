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
 * @file ddsc_conf.h
 * @brief Transport configuration for the @c ddsc:// CycloneDDS transport.
 *
 * @details
 * @c DdscConf binds the @c ddsc:// URL scheme to Eclipse CycloneDDS, the lightweight,
 * production-grade open-source DDS implementation from the Eclipse Foundation.  The
 * backend is fully cross-machine: writers and readers participate in a standards-
 * compliant RTPS Domain and discover each other automatically over UDP multicast
 * (or via a configured discovery server).  Use it whenever a DDS deployment is
 * required and a small footprint is preferred over the broader Fast-DDS feature set.
 *
 * @par Supported Node Types
 *
 * | Publisher | Subscriber | Server | Client | Getter | Setter |
 * | :-------: | :--------: | :----: | :----: | :----: | :----: |
 * | yes       | yes        | yes    | yes    | yes    | yes    |
 *
 * @par URL Format
 * @code
 *   ddsc://<topic>[?domain=<N>&depth=<N>&qos=<profile>]
 * @endcode
 *
 * | Component | Description                                                                  |
 * | --------- | ---------------------------------------------------------------------------- |
 * | @c topic  | CycloneDDS topic name, assembled from the URL host plus path                 |
 * | @c domain | DDS Domain ID (@c ?domain=); falls back to the @c VLINK_DDS_DOMAIN env var   |
 * | @c depth  | Optional history-depth override; @c 0 keeps the depth of the selected QoS    |
 * | @c qos    | Named QoS profile previously registered via @c register_qos()                |
 *
 * @par Backend-Specific Options
 *
 * | Option                | Purpose                                            | Default   |
 * | --------------------- | -------------------------------------------------- | --------- |
 * | Domain ID             | Isolates discovery traffic between deployments     | 0         |
 * | History depth         | Per-instance KeepLast retention                    | from QoS  |
 * | Response topic suffix | Auto-derived RPC reply topic name                  | ___resp   |
 *
 * @par Example
 * @code
 *   vlink::Qos qos;
 *   qos.reliability.kind = vlink::Qos::Reliability::kReliable;
 *   vlink::DdscConf::register_qos("reliable_profile", qos);
 *
 *   auto pub = vlink::Publisher<MyMsg>::create_unique("ddsc://sensors/lidar?domain=7&qos=reliable_profile");
 *   auto sub = vlink::Subscriber<MyMsg>::create_unique("ddsc://sensors/lidar?domain=7&qos=reliable_profile");
 * @endcode
 *
 * @note Compiled only when @c VLINK_SUPPORT_DDSC is defined at build time.
 * @note Unlike @c DdsConf, no external XML profile loading or @c register_topic() helper is exposed.
 */

#pragma once

#ifdef VLINK_SUPPORT_DDSC

#include <cstdint>
#include <functional>
#include <map>
#include <shared_mutex>
#include <string>

#include "../extension/qos.h"
#include "../impl/conf.h"

namespace vlink {

/**
 * @struct DdscConf
 * @brief Concrete @c Conf describing a CycloneDDS endpoint addressed by a @c ddsc:// URL.
 *
 * @details
 * Carries the four parameters that fully identify a CycloneDDS DataReader or
 * DataWriter: the topic name, the Domain ID, an optional history-depth override,
 * and an optional named QoS profile.  Instances may be created either directly or
 * via @c Url URL parsing inside the @c DdscFactory.
 */
struct VLINK_EXPORT DdscConf final : public Conf {
  std::string topic;  ///< CycloneDDS topic name (URL host concatenated with the path).
  int32_t domain{0};  ///< DDS Domain ID participated in by readers and writers (non-negative).
  int32_t depth{0};   ///< Optional history-depth override; @c 0 keeps the QoS-selected depth.
  std::string qos;    ///< Key of a named QoS profile registered through @c register_qos().

  /**
   * @brief Builds a @c DdscConf from its four logical fields.
   *
   * @param _topic   CycloneDDS topic name.
   * @param _domain  Domain ID; defaults to @c 0.
   * @param _depth   History-depth override; defaults to @c 0 (use QoS depth).
   * @param _qos     Named QoS profile key; empty by default.
   */
  explicit DdscConf(const std::string& _topic, int32_t _domain = 0, int32_t _depth = 0, const std::string& _qos = "");

  /**
   * @brief Component-wise equality on @c topic, @c domain, @c depth and @c qos.
   *
   * @param conf  Configuration to compare with.
   * @return      @c true when every field of @c *this matches @p conf.
   */
  [[nodiscard]] bool operator==(const DdscConf& conf) const noexcept;

  /**
   * @brief Logical negation of @c operator==.
   *
   * @param conf  Configuration to compare with.
   * @return      @c true when any field differs from @p conf.
   */
  [[nodiscard]] bool operator!=(const DdscConf& conf) const noexcept;

  /**
   * @brief Reports this object's transport tag.
   *
   * @return @c TransportType::kDdsc.
   */
  [[nodiscard]] TransportType get_transport_type() const override;

  /**
   * @brief Registers a named QoS profile that nodes may select through @c ?qos= in the URL.
   *
   * @details
   * Profile names share a global namespace.  Names that collide with DDS-reserved
   * tokens (@c part, @c topic, @c pub, @c sub, @c writer, @c reader, @c depth) or
   * with an already registered profile abort with a fatal log entry.
   *
   * @param name  Unique profile key; must not collide with any reserved token.
   * @param qos   The @c Qos value to associate with @p name.
   */
  static void register_qos(const std::string& name, const Qos& qos);

 private:
  static void register_qos_internal(const std::string& name, const Qos& qos);

  static const Qos& find_qos(const std::string& name);

  friend class DdscFactory;
  static std::map<std::string, Qos> qos_map_;
  static std::shared_mutex mtx_;
  static constexpr const char* kRespSuffix{"___resp"};
#ifndef VLINK_ENABLE_C_INTERFACE
  VLINK_DECLARE_GLOBAL_PROPERTY()
#endif
  VLINK_ALLOW_IMPL_TYPE(kServer | kClient | kPublisher | kSubscriber | kSetter | kGetter)
  VLINK_CONF_IMPL(DdscConf)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline DdscConf::DdscConf(const std::string& _topic, int32_t _domain, int32_t _depth, const std::string& _qos)
    : topic(_topic), domain(_domain), depth(_depth), qos(_qos) {}

inline bool DdscConf::operator==(const DdscConf& conf) const noexcept {
  return topic == conf.topic && domain == conf.domain && depth == conf.depth && qos == conf.qos;
}

inline bool DdscConf::operator!=(const DdscConf& conf) const noexcept { return !(*this == conf); }

inline TransportType DdscConf::get_transport_type() const { return TransportType::kDdsc; }

}  // namespace vlink

#endif
