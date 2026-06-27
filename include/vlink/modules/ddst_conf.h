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
 * @file ddst_conf.h
 * @brief Transport configuration for the @c ddst:// TravoDDS transport.
 *
 * @details
 * @c DdstConf binds the @c ddst:// URL scheme to TravoDDS, a domestic open-source
 * DDS implementation hosted at https://gitee.com/agiros/travodds.  The API surface
 * mirrors @c DdsConf so callers may swap between Fast-DDS, RTI Connext, and TravoDDS
 * by changing only the URL scheme; choose @c ddst:// when locality of supply chain
 * or specific TravoDDS extensions are required.
 *
 * @par Supported Node Types
 *
 * | Publisher | Subscriber | Server | Client | Getter | Setter |
 * | :-------: | :--------: | :----: | :----: | :----: | :----: |
 * | yes       | yes        | yes    | yes    | yes    | yes    |
 *
 * @par URL Format
 * @code
 *   ddst://<topic>[?domain=<N>&depth=<N>&qos=<profile>]
 *   ddst://<topic>[?domain=<N>&part=<v>&topic=<v>&pub=<v>&sub=<v>&writer=<v>&reader=<v>]
 * @endcode
 *
 * | Component  | Description                                                                  |
 * | ---------- | ---------------------------------------------------------------------------- |
 * | @c topic   | TravoDDS topic name (URL host concatenated with path)                        |
 * | @c domain  | DDS Domain ID (@c ?domain=); defaults from the @c VLINK_DDS_DOMAIN env var   |
 * | @c depth   | Optional history-depth override; @c 0 keeps the QoS-selected depth           |
 * | @c qos     | Named QoS profile registered via @c register_qos() (@c ?qos=)                |
 * | @c qos_ext | Remaining query keys after @c domain, @c depth and @c qos are removed        |
 *
 * @par Backend-Specific Options
 *
 * | Option                   | Purpose                                            | Default |
 * | ------------------------ | -------------------------------------------------- | ------- |
 * | XML QoS profile file     | Loaded via @c load_global_qos_file()               | none    |
 * | Discovered topics query  | Snapshot from @c get_discovered_topics()           | n/a     |
 * | RPC reply suffix         | Auto-derived response topic name                   | ___resp |
 *
 * @par Example
 * @code
 *   vlink::DdstConf::load_global_qos_file("/etc/vlink/ddst_profile.xml");
 *
 *   vlink::Qos qos;
 *   qos.reliability.kind = vlink::Qos::Reliability::kReliable;
 *   vlink::DdstConf::register_qos("reliable", qos);
 *
 *   auto pub = vlink::Publisher<MyMsg>::create_unique("ddst://telemetry/imu?domain=1&qos=reliable");
 * @endcode
 *
 * @note Compiled only when @c VLINK_SUPPORT_DDST is defined.
 * @note @c qos and @c qos_ext are mutually exclusive; setting both forces @c is_valid() to @c false.
 * @note RPC reply topics are derived by appending @c "___resp" to the topic name.
 */

#pragma once

#ifdef VLINK_SUPPORT_DDST

#include <cstdint>
#include <map>
#include <shared_mutex>
#include <string>
#include <tuple>
#include <vector>

#include "../base/functional.h"
#include "../extension/qos.h"
#include "../impl/conf.h"

namespace vlink {

/**
 * @struct DdstConf
 * @brief Concrete @c Conf describing a TravoDDS endpoint addressed by a @c ddst:// URL.
 *
 * @details
 * Holds the topic name, Domain ID, optional history-depth override, and either a
 * named QoS profile key or a per-entity property map for fine-grained tuning.
 */
struct VLINK_EXPORT DdstConf final : public Conf {
  std::string topic;      ///< TravoDDS topic name (URL host concatenated with path).
  int32_t domain{0};      ///< DDS Domain ID joined by the underlying DomainParticipant.
  int32_t depth{0};       ///< Optional history-depth override; @c 0 keeps the QoS-selected depth.
  std::string qos;        ///< Named QoS profile key registered via @c register_qos().
  PropertiesMap qos_ext;  ///< Per-entity property map; populated from query keys outside @c domain / @c depth / @c qos.

  /**
   * @brief Builds a @c DdstConf from topic, Domain, depth, and optional named QoS profile.
   *
   * @param _topic   TravoDDS topic name.
   * @param _domain  Domain ID; defaults to @c 0.
   * @param _depth   History-depth override; defaults to @c 0 (use QoS depth).
   * @param _qos     Named QoS profile key; empty by default.
   */
  explicit DdstConf(const std::string& _topic, int32_t _domain = 0, int32_t _depth = 0, const std::string& _qos = "");

  /**
   * @brief Builds a @c DdstConf from topic, Domain, and a per-entity QoS property map.
   *
   * @param _topic    TravoDDS topic name.
   * @param _domain   Domain ID.
   * @param _qos_ext  Property map carrying per-entity QoS overrides.
   */
  explicit DdstConf(const std::string& _topic, int32_t _domain, const PropertiesMap& _qos_ext);

  /**
   * @brief Component-wise equality on all configuration fields.
   *
   * @param conf  Configuration to compare with.
   * @return      @c true when @c topic, @c domain, @c depth, @c qos and @c qos_ext all match.
   */
  [[nodiscard]] bool operator==(const DdstConf& conf) const noexcept;

  /**
   * @brief Logical negation of @c operator==.
   *
   * @param conf  Configuration to compare with.
   * @return      @c true when any field differs from @p conf.
   */
  [[nodiscard]] bool operator!=(const DdstConf& conf) const noexcept;

  /**
   * @brief Reports this object's transport tag.
   *
   * @return @c TransportType::kDdst.
   */
  [[nodiscard]] TransportType get_transport_type() const override;

  /**
   * @brief Returns the topics currently discovered on the given DDS Domain.
   *
   * @details
   * Each entry is a @c (topic_name, type_name) pair captured from the
   * @c DdstFactory discovery cache.  The snapshot may be empty before discovery
   * settles.
   *
   * @param _domain  DDS Domain ID to query.
   * @return         Vector of @c (topic_name, type_name) tuples; may be empty.
   */
  [[nodiscard]] static std::vector<std::tuple<std::string, std::string>> get_discovered_topics(int32_t _domain);

  /**
   * @brief Loads a TravoDDS XML QoS profile file as the process-wide default.
   *
   * @details
   * Must be called before any @c ddst:// participant is created.  Profile names
   * declared in the file become available to all TravoDDS endpoints in the process.
   *
   * @param filepath  Path to the XML QoS profile file.
   * @return          @c true when the file was loaded successfully, @c false otherwise.
   */
  static bool load_global_qos_file(const std::string& filepath);

  /**
   * @brief Registers a named QoS profile that endpoints may reference via @c ?qos=.
   *
   * @details
   * Profile names share a global namespace.  Collisions with reserved tokens
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

  friend class DdstFactory;
  static std::map<std::string, Function<void*()>> type_support_map_;
  static std::map<std::string, Qos> qos_map_;
  static std::shared_mutex mtx_;
  static constexpr const char* kRespSuffix{"___resp"};
#ifndef VLINK_ENABLE_C_INTERFACE
  VLINK_DECLARE_GLOBAL_PROPERTY()
#endif
  VLINK_ALLOW_IMPL_TYPE(kServer | kClient | kPublisher | kSubscriber | kSetter | kGetter)
  VLINK_CONF_IMPL(DdstConf)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline DdstConf::DdstConf(const std::string& _topic, int32_t _domain, int32_t _depth, const std::string& _qos)
    : topic(_topic), domain(_domain), depth(_depth), qos(_qos) {}

inline DdstConf::DdstConf(const std::string& _topic, int32_t _domain, const PropertiesMap& _qos_ext)
    : topic(_topic), domain(_domain), qos_ext(_qos_ext) {}

inline bool DdstConf::operator==(const DdstConf& conf) const noexcept {
  return topic == conf.topic && domain == conf.domain && depth == conf.depth && qos == conf.qos &&
         qos_ext == conf.qos_ext;
}

inline bool DdstConf::operator!=(const DdstConf& conf) const noexcept { return !(*this == conf); }

inline TransportType DdstConf::get_transport_type() const { return TransportType::kDdst; }

}  // namespace vlink

#endif
