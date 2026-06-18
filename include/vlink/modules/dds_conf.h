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
 * @file dds_conf.h
 * @brief Transport configuration for the @c dds:// eProsima Fast-DDS / Fast-RTPS transport.
 *
 * @details
 * @c DdsConf is the default cross-machine transport in VLink and binds the @c dds://
 * URL scheme to the eProsima Fast-DDS implementation of the OMG DDS specification.
 * It supports the full RTPS wire protocol over UDP/TCP and SHM, scales from a single
 * LAN segment to wide-area deployments, and interoperates with any other compliant
 * DDS vendor on the same Domain.  Pub/sub, RPC (request/response over DDS topics)
 * and field-style state synchronisation are all routed through Fast-DDS DataReaders
 * and DataWriters under the hood.
 *
 * @par Supported Node Types
 *
 * | Publisher | Subscriber | Server | Client | Getter | Setter |
 * | :-------: | :--------: | :----: | :----: | :----: | :----: |
 * | yes       | yes        | yes    | yes    | yes    | yes    |
 *
 * @par URL Format
 * @code
 *   dds://<topic>[?domain=<N>&depth=<N>&qos=<profile>]
 *   dds://<topic>[?domain=<N>&part=<v>&topic=<v>&pub=<v>&sub=<v>&writer=<v>&reader=<v>]
 * @endcode
 *
 * | Component  | Description                                                                  |
 * | ---------- | ---------------------------------------------------------------------------- |
 * | @c topic   | DDS topic name; URL host concatenated with the URL path                      |
 * | @c domain  | DDS Domain ID (@c ?domain=); defaults from the @c VLINK_DDS_DOMAIN env var   |
 * | @c depth   | Optional history-depth override; @c 0 keeps the QoS-selected depth           |
 * | @c qos     | Named QoS profile registered with @c register_qos() (@c ?qos=)               |
 * | @c qos_ext | Remaining query keys after @c domain, @c depth, @c qos have been removed     |
 *
 * @par Environment Variables
 *
 * | Variable           | Description                                                  | Default |
 * | ------------------ | ------------------------------------------------------------ | ------- |
 * | VLINK_DDS_DOMAIN   | Default DDS Domain ID when @c ?domain= is not present in URL | 0       |
 *
 * @par QoS Registration
 * Named profiles must be registered before any endpoint that references them is created.
 * A typical reliable + transient-local profile for late-joining subscribers looks like:
 * @code
 *   vlink::Qos late_joiner;
 *   late_joiner.reliability.kind = vlink::Qos::Reliability::kReliable;
 *   late_joiner.durability.kind  = vlink::Qos::Durability::kTransientLocal;
 *   late_joiner.history.kind     = vlink::Qos::History::kKeepLast;
 *   late_joiner.history.depth    = 16;
 *   vlink::DdsConf::register_qos("late_joiner", late_joiner);
 * @endcode
 *
 * @par Type-Support Registration
 * For CDR-serialised messages the Fast-DDS @c TopicDataType factory must be wired
 * to the topic name before any endpoint is opened on that topic.  An optional
 * response type can be registered at the same time for RPC topics; the helper
 * automatically appends the @c "___resp" suffix.
 * @code
 *   vlink::DdsConf::register_topic<MyMsgPubSubType>("my_topic");
 *   vlink::DdsConf::register_topic<MyReqPubSubType, MyRespPubSubType>("my_rpc");
 *   vlink::DdsConf::register_url<MyMsgPubSubType>("dds://my_topic?domain=1");
 * @endcode
 *
 * @par Example
 * @code
 *   vlink::DdsConf::load_global_qos_file("/etc/vlink/dds_profile.xml");
 *
 *   vlink::Qos qos;
 *   qos.reliability.kind = vlink::Qos::Reliability::kReliable;
 *   qos.durability.kind  = vlink::Qos::Durability::kTransientLocal;
 *   vlink::DdsConf::register_qos("reliable_tl", qos);
 *
 *   auto pub = vlink::Publisher<MyMsg>::create_unique("dds://state?domain=42&qos=reliable_tl");
 *   auto sub = vlink::Subscriber<MyMsg>::create_unique("dds://state?domain=42&qos=reliable_tl");
 * @endcode
 *
 * @note Compiled only when @c VLINK_SUPPORT_DDS is defined.
 * @note @c qos and @c qos_ext are mutually exclusive; setting both forces @c is_valid() to @c false.
 * @note RPC reply topics are derived by appending @c "___resp" to the topic name.
 */

#pragma once

#ifdef VLINK_SUPPORT_DDS

#include <cstdint>
#include <map>
#include <shared_mutex>
#include <string>
#include <tuple>
#include <vector>

#include "../base/functional.h"
#include "../extension/qos.h"
#include "../impl/conf.h"

namespace eprosima::fastdds::dds {
class TopicDataType;
}

namespace vlink {

/**
 * @struct DdsConf
 * @brief Concrete @c Conf describing a Fast-DDS endpoint addressed by a @c dds:// URL.
 *
 * @details
 * Captures the topic, Domain ID, history-depth override, and either a named QoS
 * profile or a per-entity QoS property map.  Both constructors share the same
 * @c topic and @c domain fields; the second constructor populates @c qos_ext
 * instead of @c qos.
 */
struct VLINK_EXPORT DdsConf final : public Conf {
  std::string topic;      ///< Fast-DDS topic name (URL host concatenated with path).
  int32_t domain{0};      ///< DDS Domain ID joined by the underlying DomainParticipant.
  int32_t depth{0};       ///< History-depth override; @c 0 keeps the QoS-selected depth.
  std::string qos;        ///< Named QoS profile key registered via @c register_qos().
  PropertiesMap qos_ext;  ///< Per-entity property map; populated from query keys outside @c domain / @c depth / @c qos.

  /**
   * @brief Builds a @c DdsConf from topic, Domain, depth, and an optional named QoS profile.
   *
   * @param _topic   Fast-DDS topic name.
   * @param _domain  Domain ID; defaults to @c 0.
   * @param _depth   History-depth override; defaults to @c 0 (use QoS depth).
   * @param _qos     Named QoS profile key; empty by default.
   */
  explicit DdsConf(const std::string& _topic, int32_t _domain = 0, int32_t _depth = 0, const std::string& _qos = "");

  /**
   * @brief Builds a @c DdsConf from topic, Domain, and an explicit per-entity QoS map.
   *
   * @details
   * Use this overload when QoS must be assembled at runtime instead of being
   * registered as a named profile.  Mutually exclusive with the @c qos field;
   * @c is_valid() returns @c false if both are non-empty on the same instance.
   *
   * @param _topic    Fast-DDS topic name.
   * @param _domain   DDS Domain ID.
   * @param _qos_ext  Property map carrying per-entity QoS overrides.
   */
  explicit DdsConf(const std::string& _topic, int32_t _domain, const PropertiesMap& _qos_ext);

  /**
   * @brief Component-wise equality on all configuration fields.
   *
   * @param conf  Configuration to compare with.
   * @return      @c true when @c topic, @c domain, @c depth, @c qos and @c qos_ext all match.
   */
  [[nodiscard]] bool operator==(const DdsConf& conf) const noexcept;

  /**
   * @brief Logical negation of @c operator==.
   *
   * @param conf  Configuration to compare with.
   * @return      @c true when any field differs from @p conf.
   */
  [[nodiscard]] bool operator!=(const DdsConf& conf) const noexcept;

  /**
   * @brief Reports this object's transport tag.
   *
   * @return @c TransportType::kDds.
   */
  [[nodiscard]] TransportType get_transport_type() const override;

  /**
   * @brief Returns the topics currently discovered on the given DDS Domain.
   *
   * @details
   * Each entry is a @c (topic_name, type_name) pair captured from the
   * @c DdsFactory discovery cache.  The result is a point-in-time snapshot and may
   * be empty when discovery has not yet completed.
   *
   * @param _domain  DDS Domain ID to query.
   * @return         Vector of @c (topic_name, type_name) tuples; may be empty.
   */
  [[nodiscard]] static std::vector<std::tuple<std::string, std::string>> get_discovered_topics(int32_t _domain);

  /**
   * @brief Loads a Fast-DDS XML QoS profile file as the process-wide default.
   *
   * @details
   * Must be invoked before any @c dds:// participant is created; profile names
   * declared in the file become available to all Fast-DDS endpoints in the process.
   *
   * @param filepath  Absolute or relative path to a Fast-DDS XML profile file.
   * @return          @c true when the file was loaded successfully, @c false otherwise.
   */
  static bool load_global_qos_file(const std::string& filepath);

  /**
   * @brief Registers a Fast-DDS @c TopicDataType factory for a topic name.
   *
   * @details
   * Required for any CDR-serialised (non-Protobuf) message type.  Call once per
   * topic before any endpoint is opened on it.  When @c TypeSupportRespT is not
   * @c void, the response type is also registered under the topic name with a
   * trailing @c "___resp" suffix.
   *
   * @tparam TypeSupportT      Fast-DDS @c TopicDataType subclass for the request/message type.
   * @tparam TypeSupportRespT  Fast-DDS @c TopicDataType subclass for the response type; @c void to skip.
   * @param name               DDS topic name the factory is bound to.
   */
  template <typename TypeSupportT, typename TypeSupportRespT = void>
  static void register_topic(const std::string& name);

  /**
   * @brief Convenience wrapper that derives the topic name from a @c dds:// URL.
   *
   * @details
   * Parses @p name with @c UrlParser, extracts the topic part, and forwards to
   * @c register_topic<TypeSupportT, TypeSupportRespT>().
   *
   * @tparam TypeSupportT      Fast-DDS @c TopicDataType subclass for the request type.
   * @tparam TypeSupportRespT  Fast-DDS @c TopicDataType subclass for the response type; @c void to skip.
   * @param name               Full URL string, for example @c "dds://my_topic?domain=1".
   */
  template <typename TypeSupportT, typename TypeSupportRespT = void>
  static void register_url(const std::string& name);

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
  template <typename TypeSupportT>
  static void register_topic_internal(const std::string& name);

  static void register_qos_internal(const std::string& name, const Qos& qos);

  static Function<void*()> find_type_support(const std::string& name);

  static const Qos& find_qos(const std::string& name);

  static std::string get_topic_for_url(const std::string& url);

  friend class DdsFactory;
  static std::map<std::string, Function<void*()>> type_support_map_;
  static std::map<std::string, Qos> qos_map_;
  static std::shared_mutex mtx_;
  static constexpr const char* kRespSuffix{"___resp"};
#ifndef VLINK_ENABLE_C_INTERFACE
  VLINK_DECLARE_GLOBAL_PROPERTY()
#endif
  VLINK_ALLOW_IMPL_TYPE(kServer | kClient | kPublisher | kSubscriber | kSetter | kGetter)
  VLINK_CONF_IMPL(DdsConf)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline DdsConf::DdsConf(const std::string& _topic, int32_t _domain, int32_t _depth, const std::string& _qos)
    : topic(_topic), domain(_domain), depth(_depth), qos(_qos) {}

inline DdsConf::DdsConf(const std::string& _topic, int32_t _domain, const PropertiesMap& _qos_ext)
    : topic(_topic), domain(_domain), qos_ext(_qos_ext) {}

inline bool DdsConf::operator==(const DdsConf& conf) const noexcept {
  return topic == conf.topic && domain == conf.domain && depth == conf.depth && qos == conf.qos &&
         qos_ext == conf.qos_ext;
}

inline bool DdsConf::operator!=(const DdsConf& conf) const noexcept { return !(*this == conf); }

inline TransportType DdsConf::get_transport_type() const { return TransportType::kDds; }

template <typename TypeSupportT, typename TypeSupportRespT>
inline void DdsConf::register_topic(const std::string& name) {
  std::lock_guard lock(mtx_);
  register_topic_internal<TypeSupportT>(name);
  if constexpr (!std::is_same_v<TypeSupportRespT, void>) {
    register_topic_internal<TypeSupportRespT>(name + kRespSuffix);
  }
}

template <typename TypeSupportT, typename TypeSupportRespT>
inline void DdsConf::register_url(const std::string& name) {
  register_topic<TypeSupportT, TypeSupportRespT>(get_topic_for_url(name));
}

template <typename TypeSupportT>
inline void DdsConf::register_topic_internal(const std::string& name) {
  static_assert(std::is_base_of_v<eprosima::fastdds::dds::TopicDataType, TypeSupportT>, "Must be dds type.");

  type_support_map_[name] = [] { return new TypeSupportT(); };
}

}  // namespace vlink

#endif
