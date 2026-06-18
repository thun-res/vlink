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

#pragma once

#include <dds/dds.h>
#include <dds/ddsi/ddsi_config.h>

#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "./builtin/BuiltInRaw.h"
#include "./ddsc_proxy.hpp"
#include "./extension/qos.h"
#include "./impl/abstract_factory.h"
#include "./modules/ddsc_conf.h"

namespace vlink {

// DdscFactory
class DdscFactory final {
 private:
  DdscFactory();

  ~DdscFactory();

 public:
  struct ReadMessage final {
    Bytes bytes;
    dds_sample_info_t info;
    void* sample{nullptr};
    uint64_t id{0};
    uint64_t guid{0};
    int64_t timestamp{0};
  };

  static std::shared_ptr<ddsc::DomainParticipant> create_participant(uint8_t type, const DdscConf& conf,
                                                                     const Conf::PropertiesMap& properties);

  static std::shared_ptr<ddsc::Topic> create_topic(uint8_t type, const DdscConf& conf, ddsc::DomainParticipant* part,
                                                   std::string topic = "");

  static std::pair<std::shared_ptr<ddsc::Topic>, std::shared_ptr<ddsc::Topic>> create_method_topic(
      uint8_t type, const DdscConf& conf, ddsc::DomainParticipant* part);

  static std::shared_ptr<ddsc::Publisher> create_publisher(uint8_t type, const DdscConf& conf,
                                                           ddsc::DomainParticipant* part);

  static std::shared_ptr<ddsc::Subscriber> create_subscriber(uint8_t type, const DdscConf& conf,
                                                             ddsc::DomainParticipant* part);

  static std::shared_ptr<ddsc::DataWriter> create_datawriter(uint8_t type, const DdscConf& conf,
                                                             ddsc::Publisher* publisher, ddsc::Topic* topic,
                                                             dds_listener_t* listener = nullptr);

  static std::shared_ptr<ddsc::DataReader> create_datareader(uint8_t type, const DdscConf& conf,
                                                             ddsc::Subscriber* subscriber, ddsc::Topic* topic,
                                                             dds_listener_t* listener = nullptr);

  static bool write_data(dds_entity_t entity, const Bytes& bytes, uint64_t id);

  static bool take_data(dds_entity_t entity, ReadMessage& msg);

  static bool release_data(dds_entity_t entity, ReadMessage& msg);

  static uint64_t get_guid(const dds_guid_t* guid, uint32_t seq);

  static int get_default_domain_id();

  static std::string process_cyclone_dds_uri();

 private:
  static void set_participant_qos(int32_t domain_id, dds_qos_t* dds_qos, const Conf::PropertiesMap& properties);

  template <typename MapT, typename KeyT, typename ValueT = typename MapT::mapped_type>
  static auto get_weak_ptr(MapT& map, const KeyT& key) -> ValueT {
    auto iter = map.find(key);

    if (iter == map.end()) {
      return ValueT();
    }

    return iter->second;
  }

  using PartFilter = std::tuple<uint8_t, int32_t, Conf::PropertiesMap>;
  using TopicFilter = std::tuple<uint8_t, int32_t, std::string, ddsc::DomainParticipant*>;
  using PublisherFilter = std::tuple<uint8_t, int32_t, std::string, ddsc::DomainParticipant*>;
  using SubscriberFilter = std::tuple<uint8_t, int32_t, std::string, ddsc::DomainParticipant*>;

  struct DomainEntry final {
    dds_entity_t entity{0};
    size_t ref_count{0};
    std::string ssl_keystore;
    std::string ssl_key_pass;
    std::string ssl_ciphers;
    std::vector<std::string> network_interface_list;
    std::unique_ptr<ddsi_config_network_interface_listelem[]> network_interface_elements;
    std::vector<std::string> peer_list;
    std::unique_ptr<ddsi_config_peer_listelem[]> peer_elements;
  };

  std::map<int32_t, DomainEntry> domain_map_;
  std::map<PartFilter, std::weak_ptr<ddsc::DomainParticipant>> part_map_;
  std::map<TopicFilter, std::weak_ptr<ddsc::Topic>> topic_map_;
  std::map<PublisherFilter, std::weak_ptr<ddsc::Publisher>> publisher_map_;
  std::map<SubscriberFilter, std::weak_ptr<ddsc::Subscriber>> subscriber_map_;
  std::mutex mtx_;

  Qos default_event_qos_;
  Qos default_method_qos_;
  Qos default_field_qos_;

  VLINK_SINGLETON_DECLARE(DdscFactory)
};

}  // namespace vlink
