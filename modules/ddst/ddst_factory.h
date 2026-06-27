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

#include <travodds/dcps/domain/domainparticipant.h>
#include <travodds/dcps/domain/domainparticipantfactory.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "./builtin/BuiltInRawTypeSupport.h"
#include "./impl/abstract_factory.h"
#include "./modules/ddst_conf.h"

namespace vlink {

namespace ddst = TRAVODDS;

// DdstFactory
class DdstFactory final {
 private:
  DdstFactory();

  ~DdstFactory();

 public:
  struct ReadMessage final {
    Bytes bytes;
    ddst::SampleInfo info;
    BuiltInRaw raw;
    uint64_t id{0};
    uint64_t guid{0};
    int64_t timestamp{0};
  };

  static std::vector<std::tuple<std::string, std::string>> get_discovered_topics(int32_t _domain);

  static bool load_global_qos_file(const std::string& filepath);

  static std::shared_ptr<ddst::DomainParticipant> create_participant(uint8_t type, const DdstConf& conf,
                                                                     const Conf::PropertiesMap& properties);

  static std::shared_ptr<ddst::Topic> create_topic(uint8_t type, const DdstConf& conf, ddst::DomainParticipant* part,
                                                   std::string topic = "");

  static std::pair<std::shared_ptr<ddst::Topic>, std::shared_ptr<ddst::Topic>> create_method_topic(
      uint8_t type, const DdstConf& conf, ddst::DomainParticipant* part);

  static std::shared_ptr<ddst::Publisher> create_publisher(uint8_t type, const DdstConf& conf,
                                                           ddst::DomainParticipant* part);

  static std::shared_ptr<ddst::Subscriber> create_subscriber(uint8_t type, const DdstConf& conf,
                                                             ddst::DomainParticipant* part);

  static std::shared_ptr<ddst::DataWriter> create_datawriter(uint8_t type, const DdstConf& conf,
                                                             ddst::Publisher* publisher, ddst::Topic* topic,
                                                             ddst::DataWriterListener* listener);

  static std::shared_ptr<ddst::DataReader> create_datareader(uint8_t type, const DdstConf& conf,
                                                             ddst::Subscriber* subscriber, ddst::Topic* topic,
                                                             ddst::DataReaderListener* listener);

  static bool write_data(ddst::DataWriter* writer, const Bytes& bytes, uint64_t id);

  static bool take_data(ddst::DataReader* reader, ReadMessage& msg);

  static uint64_t get_guid(const ddst::GUID_t& guid, uint32_t seq);

  static int get_default_domain_id();

 private:
  static void set_participant_qos(ddst::DomainParticipantQos& dds_qos, const Conf::PropertiesMap& properties);

  static std::string get_qos_ext(const Conf::PropertiesMap& ext, const std::string& key);

  template <typename MapT, typename KeyT, typename ValueT = typename MapT::mapped_type>
  static auto get_weak_ptr(MapT& map, const KeyT& key) -> ValueT {
    auto iter = map.find(key);

    if (iter == map.end()) {
      return ValueT();
    }

    return iter->second;
  }

  using PartFilter = std::tuple<uint8_t, int32_t, std::string, Conf::PropertiesMap>;
  using TopicFilter = std::tuple<uint8_t, int32_t, std::string, ddst::DomainParticipant*>;
  using PublisherFilter = std::tuple<uint8_t, int32_t, std::string, std::string, std::string, ddst::DomainParticipant*>;
  using SubscriberFilter =
      std::tuple<uint8_t, int32_t, std::string, std::string, std::string, ddst::DomainParticipant*>;

  ddst::DomainParticipantFactory* dds_factory_{nullptr};
  std::map<PartFilter, std::weak_ptr<ddst::DomainParticipant>> part_map_;
  std::map<TopicFilter, std::weak_ptr<ddst::Topic>> topic_map_;
  std::map<PublisherFilter, std::weak_ptr<ddst::Publisher>> publisher_map_;
  std::map<SubscriberFilter, std::weak_ptr<ddst::Subscriber>> subscriber_map_;
  std::mutex mtx_;

  Qos default_event_qos_;
  Qos default_method_qos_;
  Qos default_field_qos_;

  VLINK_SINGLETON_DECLARE(DdstFactory)
};

}  // namespace vlink
