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

#ifdef VLINK_SUPPORT_DDS_V3
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/DataWriterListener.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/rtps/common/WriteParams.hpp>
#include <fastdds/rtps/transport/TCPv4TransportDescriptor.hpp>
#include <fastdds/rtps/transport/TCPv6TransportDescriptor.hpp>
#include <fastdds/rtps/transport/UDPv4TransportDescriptor.hpp>
#include <fastdds/rtps/transport/UDPv6TransportDescriptor.hpp>
#include <fastdds/rtps/transport/shared_mem/SharedMemTransportDescriptor.hpp>

#include "./builtin/3_x/BuiltInRawPubSubTypes.hpp"
#else
#include <fastdds/rtps/transport/TCPv4TransportDescriptor.h>
#include <fastdds/rtps/transport/TCPv6TransportDescriptor.h>
#include <fastdds/rtps/transport/UDPv4TransportDescriptor.h>
#include <fastdds/rtps/transport/UDPv6TransportDescriptor.h>
#include <fastdds/rtps/transport/shared_mem/SharedMemTransportDescriptor.h>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/DataWriterListener.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>

#include "./builtin/2_x/BuiltInRawPubSubTypes.hpp"
#endif

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "./extension/qos.h"
#include "./impl/abstract_factory.h"
#include "./modules/dds_conf.h"

namespace vlink {

#ifdef VLINK_SUPPORT_DDS_V3
namespace dds = eprosima::fastdds::dds;
namespace rtps = eprosima::fastdds::rtps;
namespace rtps2 = eprosima::fastdds::rtps;
#else
namespace dds = eprosima::fastdds::dds;
namespace rtps = eprosima::fastrtps::rtps;
namespace rtps2 = eprosima::fastdds::rtps;
#endif

// DdsFactory
class DdsFactory final {
 private:
  DdsFactory();

  ~DdsFactory();

 public:
  struct ReadCdrMessage final {
    Bytes bytes;
    dds::SampleInfo info;
    uint64_t id{0};
    void* sample{nullptr};
    int64_t timestamp{0};
  };

  struct ReadMessage final {
    Bytes bytes;
    dds::SampleInfo info;
    BuiltInRaw raw;
    uint64_t id{0};
    uint64_t guid{0};
    int64_t timestamp{0};
  };

  static std::vector<std::tuple<std::string, std::string>> get_discovered_topics(int32_t _domain);

  static bool load_global_qos_file(const std::string& filepath);

  static std::shared_ptr<dds::DomainParticipant> create_participant(uint8_t type, const DdsConf& conf,
                                                                    const Conf::PropertiesMap& properties);

  static std::shared_ptr<dds::Topic> create_topic(uint8_t type, const DdsConf& conf, dds::DomainParticipant* part,
                                                  bool is_cdr_type, std::string topic = "");

  static std::pair<std::shared_ptr<dds::Topic>, std::shared_ptr<dds::Topic>> create_method_topic(
      uint8_t type, const DdsConf& conf, dds::DomainParticipant* part, bool is_cdr_type);

  static std::shared_ptr<dds::Publisher> create_publisher(uint8_t type, const DdsConf& conf,
                                                          dds::DomainParticipant* part);

  static std::shared_ptr<dds::Subscriber> create_subscriber(uint8_t type, const DdsConf& conf,
                                                            dds::DomainParticipant* part);

  static std::shared_ptr<dds::DataWriter> create_datawriter(uint8_t type, const DdsConf& conf,
                                                            dds::Publisher* publisher, dds::Topic* topic,
                                                            dds::DataWriterListener* listener);

  static std::shared_ptr<dds::DataReader> create_datareader(uint8_t type, const DdsConf& conf,
                                                            dds::Subscriber* subscriber, dds::Topic* topic,
                                                            dds::DataReaderListener* listener);

  static bool write_data(dds::DataWriter* writer, const Bytes& bytes, uint64_t id);

  static bool write_cdr_data(dds::DataWriter* writer, const Bytes& bytes, rtps::WriteParams* params = nullptr);

  static bool take_data(dds::DataReader* reader, ReadMessage& msg);

  static bool take_cdr_data(dds::DataReader* reader, ReadCdrMessage& msg);

  static uint64_t get_guid(const rtps::GUID_t& guid, uint32_t seq);

  static int get_default_domain_id();

 private:
  static void set_participant_qos(dds::DomainParticipantQos& dds_qos, const Conf::PropertiesMap& properties);

  static rtps::LocatorList_t get_locators(const std::vector<std::string>& list);

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
  using TopicFilter = std::tuple<uint8_t, int32_t, std::string, dds::DomainParticipant*>;
  using PublisherFilter = std::tuple<uint8_t, int32_t, std::string, std::string, std::string, dds::DomainParticipant*>;
  using SubscriberFilter = std::tuple<uint8_t, int32_t, std::string, std::string, std::string, dds::DomainParticipant*>;

  dds::DomainParticipantFactory* dds_factory_{nullptr};
  std::map<PartFilter, std::weak_ptr<dds::DomainParticipant>> part_map_;
  std::map<TopicFilter, std::weak_ptr<dds::Topic>> topic_map_;
  std::map<PublisherFilter, std::weak_ptr<dds::Publisher>> publisher_map_;
  std::map<SubscriberFilter, std::weak_ptr<dds::Subscriber>> subscriber_map_;
  dds::TypeSupport raw_typesupport_;
  std::mutex mtx_;

  Qos default_event_qos_;
  Qos default_method_qos_;
  Qos default_field_qos_;

  VLINK_SINGLETON_DECLARE(DdsFactory)
};

}  // namespace vlink
