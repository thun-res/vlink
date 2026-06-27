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
 * @file zenoh_conf.h
 * @brief Transport configuration for the @c zenoh:// Eclipse Zenoh transport.
 *
 * @details
 * @c ZenohConf binds the @c zenoh:// URL scheme to Eclipse Zenoh, a unified data
 * protocol that combines publish/subscribe, queryable storage, and computed query
 * patterns under a single key-expression namespace.  Zenoh scales from constrained
 * micro-controllers to cloud-side routers and is well suited to multi-site fleet
 * and edge deployments.  Optional shared-memory acceleration lowers latency for
 * large payloads exchanged between processes on the same host.
 *
 * @par Supported Node Types
 *
 * | Publisher | Subscriber | Server | Client | Getter | Setter |
 * | :-------: | :--------: | :----: | :----: | :----: | :----: |
 * | yes       | yes        | yes    | yes    | yes    | yes    |
 *
 * @par Routing Modes
 *
 * | Mode      | Topology                                          | Typical use case               |
 * | --------- | ------------------------------------------------- | ------------------------------ |
 * | @c peer   | Fully meshed P2P between participants             | LAN with no router             |
 * | @c client | Connects to a router as a leaf node               | Fleet edge connecting to cloud |
 * | @c router | Forwards and stores data for connected clients    | Gateway / aggregation point    |
 *
 * @par URL Format
 * @code
 *   zenoh://<address>[?event=<name>&domain=<N>&qos=<profile>&depth=<N>&shm=<bool>
 *                    &shm_mode=<lazy|init>&shm_size=<N>&shm_threshold=<N>
 *                    &shm_loan_threshold=<N>&shm_blocking=<bool>][#<fragment>]
 * @endcode
 *
 * | Component             | Description                                                            |
 * | --------------------- | ---------------------------------------------------------------------- |
 * | @c address            | Zenoh key expression (URL host concatenated with path)                 |
 * | @c event              | Optional secondary event filter (@c ?event=)                           |
 * | @c domain             | Zenoh session/domain identifier (@c ?domain=); factory default applied |
 * | @c qos                | Named QoS profile registered via @c register_qos()                     |
 * | @c depth              | TX queue override; @c 0 uses the QoS-selected history depth            |
 * | @c shm                | Enable Zenoh shared-memory acceleration (boolean)                      |
 * | @c shm_mode           | Pool init strategy; @c lazy or @c init                                 |
 * | @c shm_size           | SHM pool size; accepts bytes, K, M, or G suffixes                      |
 * | @c shm_threshold      | Minimum payload size to switch the SHM path on                         |
 * | @c shm_loan_threshold | Minimum size for VLink SHM loan buffers                                |
 * | @c shm_blocking       | Whether @c loan() blocks when the pool is exhausted                    |
 * | @c fragment           | Optional transport hint or session-config fragment                     |
 *
 * @par QoS Registration
 * @code
 *   vlink::Qos qos;
 *   qos.reliability.kind = vlink::Qos::Reliability::kReliable;
 *   vlink::ZenohConf::register_qos("reliable", qos);
 *
 *   auto pub = vlink::Publisher<MyMsg>::create_unique("zenoh://vehicle/speed?qos=reliable");
 * @endcode
 *
 * @note Compiled only when @c VLINK_SUPPORT_ZENOH is defined.
 * @note @c is_valid() returns @c false when @c address is empty or @c domain is negative.
 */

#pragma once

#ifdef VLINK_SUPPORT_ZENOH

#include <cstdint>
#include <functional>
#include <map>
#include <shared_mutex>
#include <string>

#include "../extension/qos.h"
#include "../impl/conf.h"

namespace vlink {

/**
 * @struct ZenohConf
 * @brief Concrete @c Conf describing a Zenoh endpoint addressed by a @c zenoh:// URL.
 *
 * @details
 * Stores the Zenoh key expression, an optional secondary event filter, the session
 * domain identifier, an optional named QoS profile and depth override, plus the
 * tunable SHM-acceleration knobs exposed through URL query keys.
 */
struct VLINK_EXPORT ZenohConf final : public Conf {
  std::string address;             ///< Zenoh key expression (URL host concatenated with path).
  std::string event;               ///< Optional secondary event filter string.
  int32_t domain{0};               ///< Zenoh session / domain identifier (non-negative).
  int32_t depth{0};                ///< TX queue override; @c 0 uses the QoS-selected history depth.
  std::string qos;                 ///< Named QoS profile key registered via @c register_qos().
  std::string fragment;            ///< Optional transport hint or session-config fragment.
  std::string shm;                 ///< Optional SHM acceleration enable (string boolean).
  std::string shm_mode;            ///< Optional SHM pool init strategy; @c lazy or @c init.
  std::string shm_size;            ///< Optional SHM pool size; accepts bytes, K, M, or G suffixes.
  std::string shm_threshold;       ///< Optional minimum payload size before SHM path engages.
  std::string shm_loan_threshold;  ///< Optional minimum size for VLink SHM loan buffers.
  std::string shm_blocking;        ///< Optional blocking behaviour for @c loan() when the pool is full.

  /**
   * @brief Builds a @c ZenohConf from the URL's primary fields.
   *
   * @param _address   Zenoh key expression.
   * @param _event     Optional secondary event filter; empty by default.
   * @param _domain    Domain identifier; defaults to @c 0.
   * @param _qos       Named QoS profile key; empty by default.
   * @param _fragment  Optional transport-hint fragment; empty by default.
   */
  explicit ZenohConf(const std::string& _address, const std::string& _event = "", int32_t _domain = 0,
                     const std::string& _qos = "", const std::string& _fragment = "");

  /**
   * @brief Component-wise equality on all configuration fields, including SHM tunables.
   *
   * @param conf  Configuration to compare with.
   * @return      @c true when every field of @c *this matches @p conf.
   */
  [[nodiscard]] bool operator==(const ZenohConf& conf) const noexcept;

  /**
   * @brief Logical negation of @c operator==.
   *
   * @param conf  Configuration to compare with.
   * @return      @c true when any field differs from @p conf.
   */
  [[nodiscard]] bool operator!=(const ZenohConf& conf) const noexcept;

  /**
   * @brief Reports this object's transport tag.
   *
   * @return @c TransportType::kZenoh.
   */
  [[nodiscard]] TransportType get_transport_type() const override;

  /**
   * @brief Copies non-empty Zenoh SHM tunables into a property map.
   *
   * @details
   * Lets URL-supplied @c shm* query keys and explicit @c set_property("zenoh.*", ...)
   * calls share the same factory property path without enlarging the factory key set.
   *
   * @param properties  Destination property map; entries are added for each non-empty SHM field.
   */
  void append_properties(PropertiesMap& properties) const;

  /**
   * @brief Registers a named QoS profile that endpoints may reference via @c ?qos=.
   *
   * @details
   * Profile names share a global namespace.  Collisions with reserved tokens
   * (@c part, @c topic, @c pub, @c sub, @c writer, @c reader) or with an
   * already registered profile abort with a fatal log entry.
   *
   * @param name  Unique profile key; must not collide with any reserved token.
   * @param qos   @c Qos value associated with the key.
   */
  static void register_qos(const std::string& name, const Qos& qos);

 private:
  static void register_qos_internal(const std::string& name, const Qos& qos);

  static const Qos& find_qos(const std::string& name);

  friend class ZenohFactory;
  static std::map<std::string, Qos> qos_map_;
  static std::shared_mutex mtx_;
  static constexpr const char* kRespSuffix{"___resp"};
#ifndef VLINK_ENABLE_C_INTERFACE
  VLINK_DECLARE_GLOBAL_PROPERTY()
#endif
  VLINK_ALLOW_IMPL_TYPE(kServer | kClient | kPublisher | kSubscriber | kSetter | kGetter)
  VLINK_CONF_IMPL(ZenohConf)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline ZenohConf::ZenohConf(const std::string& _address, const std::string& _event, int32_t _domain,
                            const std::string& _qos, const std::string& _fragment)
    : address(_address), event(_event), domain(_domain), qos(_qos), fragment(_fragment) {}

inline bool ZenohConf::operator==(const ZenohConf& conf) const noexcept {
  return address == conf.address && event == conf.event && domain == conf.domain && depth == conf.depth &&
         qos == conf.qos && fragment == conf.fragment && shm == conf.shm && shm_mode == conf.shm_mode &&
         shm_size == conf.shm_size && shm_threshold == conf.shm_threshold &&
         shm_loan_threshold == conf.shm_loan_threshold && shm_blocking == conf.shm_blocking;
}

inline bool ZenohConf::operator!=(const ZenohConf& conf) const noexcept { return !(*this == conf); }

inline TransportType ZenohConf::get_transport_type() const { return TransportType::kZenoh; }

inline void ZenohConf::append_properties(PropertiesMap& properties) const {
  if (!shm.empty()) {
    properties["zenoh.shm"] = shm;
  }

  if (!shm_mode.empty()) {
    properties["zenoh.shm_mode"] = shm_mode;
  }

  if (!shm_size.empty()) {
    properties["zenoh.shm_size"] = shm_size;
  }

  if (!shm_threshold.empty()) {
    properties["zenoh.shm_threshold"] = shm_threshold;
  }

  if (!shm_loan_threshold.empty()) {
    properties["zenoh.shm_loan_threshold"] = shm_loan_threshold;
  }

  if (!shm_blocking.empty()) {
    properties["zenoh.shm_blocking"] = shm_blocking;
  }
}

}  // namespace vlink

#endif
