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

#include <ndds/ndds_c.h>

#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "./ddsr_proxy.hpp"
#include "./extension/qos.h"
#include "./impl/abstract_factory.h"
#include "./modules/ddsr_conf.h"

extern "C" {
#if RTI_DDS_VERSION_MAJOR >= 6
#include "builtin/7_x/BuiltInRaw.h"
#else
#include "builtin/5_x/BuiltInRaw.h"
#endif
}

namespace vlink {

// DdsrFactory
class DdsrFactory final {
 private:
  DdsrFactory();

  ~DdsrFactory();

 public:
  struct ReadMessage final {
    Bytes bytes;
    vlink_BuiltInRawSeq seq;
    DDS_SampleInfoSeq info_seq;
    DDS_SampleInfo* info{nullptr};
    uint64_t id{0};
    uint64_t guid{0};
    int64_t timestamp{0};
  };

  static std::shared_ptr<ddsr::DomainParticipant> create_participant(uint8_t type, const DdsrConf& conf,
                                                                     const Conf::PropertiesMap& properties);

  static std::shared_ptr<ddsr::Topic> create_topic(uint8_t type, const DdsrConf& conf, ddsr::DomainParticipant* part,
                                                   std::string topic = "");

  static std::pair<std::shared_ptr<ddsr::Topic>, std::shared_ptr<ddsr::Topic>> create_method_topic(
      uint8_t type, const DdsrConf& conf, ddsr::DomainParticipant* part);

  static std::shared_ptr<ddsr::Publisher> create_publisher(uint8_t type, const DdsrConf& conf,
                                                           ddsr::DomainParticipant* part);

  static std::shared_ptr<ddsr::Subscriber> create_subscriber(uint8_t type, const DdsrConf& conf,
                                                             ddsr::DomainParticipant* part);

  static std::shared_ptr<ddsr::DataWriter> create_datawriter(uint8_t type, const DdsrConf& conf,
                                                             ddsr::Publisher* publisher, ddsr::Topic* topic,
                                                             DDS_DataWriterListener* listener = nullptr);

  static std::shared_ptr<ddsr::DataReader> create_datareader(uint8_t type, const DdsrConf& conf,
                                                             ddsr::Subscriber* subscriber, ddsr::Topic* topic,
                                                             DDS_DataReaderListener* listener = nullptr);

  static bool write_data(DDS_DataWriter* writer, const Bytes& bytes, uint64_t id);

  static bool take_data(DDS_DataReader* reader, ReadMessage& msg);

  static bool release_data(DDS_DataReader* reader, ReadMessage& msg);

  static uint64_t get_guid(DDS_GUID_t* guid, uint32_t seq);

  static int get_default_domain_id();

  static DDS_DomainParticipantFactory* get_dds_factory();

 private:
  static void set_participant_qos(DDS_DomainParticipantQos& dds_qos, const Conf::PropertiesMap& properties);

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
  using TopicFilter = std::tuple<uint8_t, int32_t, std::string, ddsr::DomainParticipant*>;
  using PublisherFilter = std::tuple<uint8_t, int32_t, std::string, std::string, std::string, ddsr::DomainParticipant*>;
  using SubscriberFilter =
      std::tuple<uint8_t, int32_t, std::string, std::string, std::string, ddsr::DomainParticipant*>;

  std::map<PartFilter, std::weak_ptr<ddsr::DomainParticipant>> part_map_;
  std::map<TopicFilter, std::weak_ptr<ddsr::Topic>> topic_map_;
  std::map<PublisherFilter, std::weak_ptr<ddsr::Publisher>> publisher_map_;
  std::map<SubscriberFilter, std::weak_ptr<ddsr::Subscriber>> subscriber_map_;
  std::mutex mtx_;
  DDS_DomainParticipantFactory* dds_factory_{nullptr};

  Qos default_event_qos_;
  Qos default_method_qos_;
  Qos default_field_qos_;

  VLINK_SINGLETON_DECLARE(DdsrFactory)
};

}  // namespace vlink
