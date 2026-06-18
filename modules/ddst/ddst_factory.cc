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

#include "./ddst_factory.h"

#include <travodds/common/log/logger.h>

#include <charconv>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "./base/helpers.h"
#include "./base/utils.h"
#include "./ddst_qos.h"
#include "./extension/qos_profile.h"

namespace vlink {

// DdstFactory
DdstFactory::DdstFactory() {
  Bytes::init_memory_pool();

  if VUNLIKELY (DdstConf::get_thread_count() != 1) {
    VLOG_W("DdstFactory: Ddst does not support setting thread count.");
  }

  for (const auto& [name, qos] : QosProfile::get_available_qos_map()) {
    DdstConf::register_qos_internal(name, qos);
  }

  std::string dds_debug_str = Utils::get_env("VLINK_DDS_DEBUG");

  if (dds_debug_str == "1") {
    ddst::LogInitial(0xffff, 0);
  } else {
    ddst::LogInitial(ddst::LOG_WARNING | ddst::LOG_ERROR, 0);
  }

  dds_factory_ = ddst::DomainParticipantFactory::get_instance();

  std::string qos_file = Utils::get_env("VLINK_TRAVODDS_QOS_FILE");

  if (!qos_file.empty()) {
    dds_factory_->load_qos_from_xml(qos_file.c_str());
  }

  std::string default_event_qos_str = Utils::get_env("VLINK_DDS_EVENT_QOS");
  std::string default_method_qos_str = Utils::get_env("VLINK_DDS_METHOD_QOS");
  std::string default_field_qos_str = Utils::get_env("VLINK_DDS_FIELD_QOS");

  if (default_event_qos_str.empty()) {
    default_event_qos_ = QosProfile::kEvent;
  } else {
    default_event_qos_ = DdstConf::find_qos(default_event_qos_str);
  }

  if (default_method_qos_str.empty()) {
    default_method_qos_ = QosProfile::kMethod;
  } else {
    default_method_qos_ = DdstConf::find_qos(default_method_qos_str);
  }

  if (default_field_qos_str.empty()) {
    default_field_qos_ = QosProfile::kField;
  } else {
    default_field_qos_ = DdstConf::find_qos(default_field_qos_str);
  }
}

DdstFactory::~DdstFactory() { ddst::LogDestroy(); }

std::vector<std::tuple<std::string, std::string>> DdstFactory::get_discovered_topics(int32_t _domain) {
  std::vector<std::tuple<std::string, std::string>> topics;

  static auto& factory = DdstFactory::get();

  auto* part = factory.dds_factory_->lookup_participant(_domain);

  if VUNLIKELY (!part) {
    return topics;
  }

  std::vector<ddst::InstanceHandle_t> topic_handles;
  part->get_discovered_topics(topic_handles);

  topics.reserve(topic_handles.size());

  for (const auto& instance : topic_handles) {
    ddst::TopicBuiltinTopicData topic_data;
    part->get_discovered_topic_data(topic_data, instance);
    topics.emplace_back(std::forward_as_tuple(std::move(topic_data.name), std::move(topic_data.type_name)));
  }

  return topics;
}

bool DdstFactory::load_global_qos_file(const std::string& filepath) {
  static auto& factory = DdstFactory::get();

  return factory.dds_factory_->load_qos_from_xml(filepath.c_str()) == ddst::RETCODE_OK;
}

std::shared_ptr<ddst::DomainParticipant> DdstFactory::create_participant(uint8_t type, const DdstConf& conf,
                                                                         const Conf::PropertiesMap& properties) {
  static auto& factory = DdstFactory::get();

  const auto& dds_qos_ext = get_qos_ext(conf.qos_ext, "part");
  const auto& id = std::make_tuple(type, conf.domain, dds_qos_ext, properties);

  std::unique_lock lock(factory.mtx_);

  std::shared_ptr<ddst::DomainParticipant> part = get_weak_ptr(factory.part_map_, id).lock();

  if (!part) {
    lock.unlock();

    ddst::DomainParticipant* ptr = nullptr;

    if (dds_qos_ext.empty()) {
      auto dds_qos = ddst::PARTICIPANT_QOS_DEFAULT;

      set_participant_qos(dds_qos, properties);

      ptr = factory.dds_factory_->create_participant(conf.domain, dds_qos, nullptr, STATUS_MASK_ALL);
    } else {
      ptr = factory.dds_factory_->create_participant_with_profile(conf.domain, nullptr, nullptr, dds_qos_ext.c_str(),
                                                                  nullptr, STATUS_MASK_ALL);
    }

    if VUNLIKELY (!ptr) {
      VLOG_E("DdstFactory: Failed to create participant.");
      return nullptr;
    }

    part = std::shared_ptr<ddst::DomainParticipant>(ptr, [id](ddst::DomainParticipant* part) {
      {
        std::lock_guard lock(factory.mtx_);
        auto iter = factory.part_map_.find(id);

        if (iter != factory.part_map_.end() && iter->second.expired()) {
          factory.part_map_.erase(iter);
        }
      }

      factory.dds_factory_->delete_participant(part);
    });

    lock.lock();

    auto [iter, inserted] = factory.part_map_.emplace(id, part);

    if (!inserted) {
      auto inserted_part = iter->second.lock();
      if VLIKELY (inserted_part) {
        lock.unlock();
        part = std::move(inserted_part);
      } else {
        iter->second = part;
      }
    }
  }

  return part;
}

std::shared_ptr<ddst::Topic> DdstFactory::create_topic(uint8_t type, const DdstConf& conf,
                                                       ddst::DomainParticipant* part, std::string topic) {
  static auto& factory = DdstFactory::get();

  if (topic.empty()) {
    topic = conf.topic;
  }

  const auto& dds_qos_ext = get_qos_ext(conf.qos_ext, "topic");

  if VUNLIKELY (!part) {
    VLOG_E("DdstFactory: Cannot create topic without participant.");
    return nullptr;
  }

  const auto& id = std::make_tuple(type, conf.domain, topic, part);

  std::unique_lock lock(factory.mtx_);

  std::shared_ptr<ddst::Topic> dds_topic = get_weak_ptr(factory.topic_map_, id).lock();

  auto* type_support = BuiltInRawTypeSupport::get_instance();

  const auto* type_name = type_support->get_typename();

  part->registe_type(type_name, type_support);

  if (!dds_topic) {
    lock.unlock();

    ddst::Topic* ptr = nullptr;

    if (dds_qos_ext.empty()) {
      auto dds_qos = ddst::TOPIC_QOS_DEFAULT;

      ptr = part->create_topic(topic, type_name, dds_qos, nullptr, STATUS_MASK_NONE);
    } else {
      ptr = part->create_topic_with_qos_profile(topic, type_name, nullptr, nullptr, dds_qos_ext.c_str(), nullptr,
                                                STATUS_MASK_NONE);
    }

    if VUNLIKELY (!ptr) {
      VLOG_E("DdstFactory: Failed to create topic: ", topic, ".");
      return nullptr;
    }

    dds_topic = std::shared_ptr<ddst::Topic>(ptr, [id](ddst::Topic* topic) {
      {
        std::lock_guard lock(factory.mtx_);
        auto iter = factory.topic_map_.find(id);

        if (iter != factory.topic_map_.end() && iter->second.expired()) {
          factory.topic_map_.erase(iter);
        }
      }

      auto* participant = const_cast<ddst::DomainParticipant*>(topic->get_participant());
      participant->delete_topic(topic);
    });

    lock.lock();

    auto [iter, inserted] = factory.topic_map_.emplace(id, dds_topic);

    if (!inserted) {
      auto inserted_topic = iter->second.lock();
      if VLIKELY (inserted_topic) {
        lock.unlock();
        dds_topic = std::move(inserted_topic);
      } else {
        iter->second = dds_topic;
      }
    }
  }

  return dds_topic;
}

std::pair<std::shared_ptr<ddst::Topic>, std::shared_ptr<ddst::Topic>> DdstFactory::create_method_topic(
    uint8_t type, const DdstConf& conf, ddst::DomainParticipant* part) {
  const std::string& resp_topic = conf.topic + DdstConf::kRespSuffix;

  if VUNLIKELY (conf.topic.empty() || resp_topic.empty()) {
    VLOG_F("DdstFactory: Method conf topic error.");
  }

  if VUNLIKELY (conf.topic == resp_topic) {
    VLOG_F("DdstFactory: Method conf topic req and resp cannot be equal.");
  }

  return {create_topic(type, conf, part, conf.topic), create_topic(type, conf, part, resp_topic)};
}

std::shared_ptr<ddst::Publisher> DdstFactory::create_publisher(uint8_t type, const DdstConf& conf,
                                                               ddst::DomainParticipant* part) {
  static auto& factory = DdstFactory::get();

  const auto& dds_qos_ext = get_qos_ext(conf.qos_ext, "pub");
  const auto& writer_qos = get_qos_ext(conf.qos_ext, "writer");

  if VUNLIKELY (!part) {
    VLOG_E("DdstFactory: Cannot create publisher without participant.");
    return nullptr;
  }

  const auto& id = std::make_tuple(type, conf.domain, conf.qos, dds_qos_ext, writer_qos, part);

  std::unique_lock lock(factory.mtx_);
  std::shared_ptr<ddst::Publisher> publisher = get_weak_ptr(factory.publisher_map_, id).lock();

  if (!publisher) {
    lock.unlock();

    ddst::Publisher* ptr = nullptr;

    if (dds_qos_ext.empty()) {
      auto dds_qos = ddst::PUBLISHER_QOS_DEFAULT;

      ptr = part->create_publisher(dds_qos, nullptr, STATUS_MASK_NONE);
    } else {
      ptr = part->create_publisher_with_qos_profile(nullptr, nullptr, dds_qos_ext.c_str(), nullptr, STATUS_MASK_NONE);
    }

    if VUNLIKELY (!ptr) {
      VLOG_E("DdstFactory: Failed to create publisher.");
      return nullptr;
    }

    publisher = std::shared_ptr<ddst::Publisher>(ptr, [id](ddst::Publisher* publisher) {
      {
        std::lock_guard lock(factory.mtx_);
        auto iter = factory.publisher_map_.find(id);

        if (iter != factory.publisher_map_.end() && iter->second.expired()) {
          factory.publisher_map_.erase(iter);
        }
      }
      auto* participant = const_cast<ddst::DomainParticipant*>(publisher->get_participant());
      participant->delete_publisher(publisher);
    });

    lock.lock();

    auto [iter, inserted] = factory.publisher_map_.emplace(id, publisher);

    if (!inserted) {
      auto inserted_publisher = iter->second.lock();
      if VLIKELY (inserted_publisher) {
        lock.unlock();
        publisher = std::move(inserted_publisher);
      } else {
        iter->second = publisher;
      }
    }
  }

  return publisher;
}

std::shared_ptr<ddst::Subscriber> DdstFactory::create_subscriber(uint8_t type, const DdstConf& conf,
                                                                 ddst::DomainParticipant* part) {
  static auto& factory = DdstFactory::get();

  const auto& dds_qos_ext = get_qos_ext(conf.qos_ext, "sub");
  const auto& reader_qos = get_qos_ext(conf.qos_ext, "reader");

  if VUNLIKELY (!part) {
    VLOG_E("DdstFactory: Cannot create subscriber without participant.");
    return nullptr;
  }

  const auto& id = std::make_tuple(type, conf.domain, conf.qos, dds_qos_ext, reader_qos, part);

  std::unique_lock lock(factory.mtx_);

  std::shared_ptr<ddst::Subscriber> subscriber = get_weak_ptr(factory.subscriber_map_, id).lock();

  if (!subscriber) {
    lock.unlock();
    ddst::Subscriber* ptr = nullptr;
    if (dds_qos_ext.empty()) {
      auto dds_qos = ddst::SUBSCRIBER_QOS_DEFAULT;

      ptr = part->create_subscriber(dds_qos, nullptr, STATUS_MASK_NONE);
    } else {
      ptr = part->create_subscriber_with_qos_profile(nullptr, nullptr, dds_qos_ext.c_str(), nullptr, STATUS_MASK_NONE);
    }

    if VUNLIKELY (!ptr) {
      VLOG_E("DdstFactory: Failed to create subscriber.");
      return nullptr;
    }

    subscriber = std::shared_ptr<ddst::Subscriber>(ptr, [id](ddst::Subscriber* subscriber) {
      {
        std::lock_guard lock(factory.mtx_);
        auto iter = factory.subscriber_map_.find(id);

        if (iter != factory.subscriber_map_.end() && iter->second.expired()) {
          factory.subscriber_map_.erase(iter);
        }
      }
      auto* participant = const_cast<ddst::DomainParticipant*>(subscriber->get_participant());
      participant->delete_subscriber(subscriber);
    });

    lock.lock();

    auto [iter, inserted] = factory.subscriber_map_.emplace(id, subscriber);

    if (!inserted) {
      auto inserted_subscriber = iter->second.lock();
      if VLIKELY (inserted_subscriber) {
        lock.unlock();
        subscriber = std::move(inserted_subscriber);
      } else {
        iter->second = subscriber;
      }
    }
  }

  return subscriber;
}

std::shared_ptr<ddst::DataWriter> DdstFactory::create_datawriter(uint8_t type, const DdstConf& conf,
                                                                 ddst::Publisher* publisher, ddst::Topic* topic,
                                                                 ddst::DataWriterListener* listener) {
  static auto& factory = DdstFactory::get();

  const auto& dds_qos_ext = get_qos_ext(conf.qos_ext, "writer");

  if VUNLIKELY (!publisher || !topic) {
    VLOG_E("DdstFactory: Cannot create datawriter without publisher/topic.");
    return nullptr;
  }

  ddst::DataWriter* ptr = nullptr;

  if (dds_qos_ext.empty()) {
    auto dds_qos = ddst::DATAWRITER_QOS_DEFAULT;

    if (conf.qos.empty()) {
      if ((type & kPublisher) || (type & kSubscriber)) {
        convert_qos(dds_qos, factory.default_event_qos_, conf.depth);
      } else if ((type & kClient) || (type & kServer)) {
        convert_qos(dds_qos, factory.default_method_qos_, conf.depth);
      } else if ((type & kSetter) || (type & kGetter)) {
        convert_qos(dds_qos, factory.default_field_qos_, conf.depth);
      }
    } else {
      convert_qos(dds_qos, DdstConf::find_qos(conf.qos), conf.depth);
    }

    ptr = publisher->create_datawriter(topic, dds_qos, listener, STATUS_MASK_ALL);
  } else {
    ptr = publisher->create_datawriter_with_qos_profile(topic, nullptr, nullptr, dds_qos_ext.c_str(), listener,
                                                        STATUS_MASK_ALL);
  }

  if VUNLIKELY (!ptr) {
    VLOG_E("DdstFactory: Failed to create datawriter.");
    return nullptr;
  }

  return std::shared_ptr<ddst::DataWriter>(ptr, [](ddst::DataWriter* writer) {
    auto* publisher = const_cast<ddst::Publisher*>(writer->get_publisher());
    publisher->delete_datawriter(writer);
  });
}

std::shared_ptr<ddst::DataReader> DdstFactory::create_datareader(uint8_t type, const DdstConf& conf,
                                                                 ddst::Subscriber* subscriber, ddst::Topic* topic,
                                                                 ddst::DataReaderListener* listener) {
  static auto& factory = DdstFactory::get();

  const auto& dds_qos_ext = get_qos_ext(conf.qos_ext, "reader");

  if VUNLIKELY (!subscriber || !topic) {
    VLOG_E("DdstFactory: Cannot create datareader without subscriber/topic.");
    return nullptr;
  }

  ddst::DataReader* ptr = nullptr;

  if (dds_qos_ext.empty()) {
    auto dds_qos = ddst::DATAREADER_QOS_DEFAULT;

    if (conf.qos.empty()) {
      if ((type & kPublisher) || (type & kSubscriber)) {
        convert_qos(dds_qos, factory.default_event_qos_, conf.depth);
      } else if ((type & kClient) || (type & kServer)) {
        convert_qos(dds_qos, factory.default_method_qos_, conf.depth);
      } else if ((type & kSetter) || (type & kGetter)) {
        convert_qos(dds_qos, factory.default_field_qos_, conf.depth);
      }
    } else {
      convert_qos(dds_qos, DdstConf::find_qos(conf.qos), conf.depth);
    }

    ptr = subscriber->create_datareader(topic, dds_qos, listener, STATUS_MASK_ALL);
  } else {
    ptr = subscriber->create_datareader_with_qos_profile(topic, nullptr, nullptr, dds_qos_ext.c_str(), listener,
                                                         STATUS_MASK_ALL);
  }

  if VUNLIKELY (!ptr) {
    VLOG_E("DdstFactory: Failed to create datareader.");
    return nullptr;
  }

  return std::shared_ptr<ddst::DataReader>(ptr, [](ddst::DataReader* reader) {
    auto* subscriber = const_cast<ddst::Subscriber*>(reader->get_subscriber());
    subscriber->delete_datareader(reader);
  });
}

bool DdstFactory::write_data(ddst::DataWriter* writer, const Bytes& bytes, uint64_t id) {
  if VUNLIKELY (bytes.is_ptr()) {
    VLOG_E("DdstFactory: Failed to write data, does not support ptr type.");
    return false;
  }

  BuiltInRaw raw;
  raw.id = id;

  raw.data.shallow_copy(bytes);

  return writer->write(&raw, ddst::HANDLE_NIL) == ddst::RETCODE_OK;
}

bool DdstFactory::take_data(ddst::DataReader* reader, ReadMessage& msg) {
  auto ret = reader->take_next_sample(&msg.raw, msg.info);

  if (ret == ddst::RETCODE_NO_DATA) {
    return false;
  }

  if VUNLIKELY (ret != ddst::RETCODE_OK) {
    VLOG_E("DdstFactory: Failed to take data.");
    return false;
  }

  msg.id = msg.raw.id;

  msg.bytes.shallow_copy(msg.raw.data);

  msg.timestamp = msg.info.source_timestamp.ToNs();

  return true;
}

uint64_t DdstFactory::get_guid(const ddst::GUID_t& guid, uint32_t seq) {
  const auto& handle = ddst::Conversion::ToInstanceHandle(guid);

  const auto& value = handle.keyHash.value;

  uint64_t result = static_cast<uint64_t>(value[0] + value[1] + value[2] + value[3]) << 24 |
                    static_cast<uint64_t>(value[4] + value[5] + value[6] + value[7]) << 16 |
                    static_cast<uint64_t>(value[8] + value[9] + value[10] + value[11]) << 8 |
                    static_cast<uint64_t>(value[12] + value[13] + value[14] + value[15]);

  return result << 32 | seq;
}

int DdstFactory::get_default_domain_id() {
  const std::string& domain_str = Utils::get_env("VLINK_DDS_DOMAIN");
  return Helpers::to_int(domain_str, 0);
}

void DdstFactory::set_participant_qos(ddst::DomainParticipantQos& dds_qos, const Conf::PropertiesMap& properties) {
  static const std::string& ip_str = Utils::get_env("VLINK_DDS_IP");
  static const std::string& ip_multicast_str = Utils::get_env("VLINK_DDS_MULTICAST_IP");
  static const std::string& peer_str = Utils::get_env("VLINK_DDS_PEER");
  static const std::string& buf_str = Utils::get_env("VLINK_DDS_BUF");
  static const std::string& mtu_str = Utils::get_env("VLINK_DDS_MTU");

  static bool enable_udp = Helpers::to_int(Utils::get_env("VLINK_DDS_UDP"), 1) != 0;
  static bool enable_tcp = Helpers::to_int(Utils::get_env("VLINK_DDS_TCP"), 0) != 0;
  static bool enable_shm = Helpers::to_int(Utils::get_env("VLINK_DDS_SHM"), 0) != 0;

  static bool enable_less_memory = Helpers::to_int(Utils::get_env("VLINK_DDS_LESS_MEMORY"), 0) != 0;

  static bool enable_ip_filter = Helpers::to_int(Utils::get_env("VLINK_DDS_IP_FILTER"), 0) != 0;

  static std::vector<std::string> default_ip_list = Utils::get_dds_default_address(enable_ip_filter);

  [[maybe_unused]] std::string prop_ip_str = ip_str;
  [[maybe_unused]] std::string prop_ip_multicast_str = ip_multicast_str;
  [[maybe_unused]] std::string prop_peer_str = peer_str;
  [[maybe_unused]] size_t prop_buf = 0;
  [[maybe_unused]] size_t prop_mtu = 0;
  [[maybe_unused]] bool prop_enable_udp = enable_udp;
  [[maybe_unused]] bool prop_enable_tcp = enable_tcp;
  [[maybe_unused]] bool prop_enable_shm = enable_shm;
  [[maybe_unused]] bool prop_enable_less_memory = enable_less_memory;

  if (!buf_str.empty()) {
    std::from_chars(buf_str.data(), buf_str.data() + buf_str.size(), prop_buf);
  }

  if (!mtu_str.empty()) {
    std::from_chars(mtu_str.data(), mtu_str.data() + mtu_str.size(), prop_mtu);
  }

  for (const auto& [prop, value] : properties) {
    if (!Helpers::has_startwith(prop, "dds.")) {
      continue;
    }

    if (prop == "dds.ip") {
      prop_ip_str = value;
    } else if (prop == "dds.multicast.ip") {
      prop_ip_multicast_str = value;
    } else if (prop == "dds.peer") {
      prop_peer_str = value;
    } else if (prop == "dds.buf") {
      std::from_chars(value.data(), value.data() + value.size(), prop_buf);
    } else if (prop == "dds.mtu") {
      std::from_chars(value.data(), value.data() + value.size(), prop_mtu);
    } else if (prop == "dds.udp") {
      prop_enable_udp = (value == "1");
    } else if (prop == "dds.tcp") {
      prop_enable_tcp = (value == "1");
    } else if (prop == "dds.shm") {
      prop_enable_shm = (value == "1");
    } else if (prop == "dds.less_memory") {
      prop_enable_less_memory = (value == "1");
    } else {
      dds_qos.property.properties().emplace_back(prop, value);
    }
  }

  dds_qos.transport.kinds = 0;

#if !defined(__ANDROID__) && !defined(_WIN32)

  if (prop_enable_shm) {
    dds_qos.transport.addTransport(ddst::TRANSPORT_SHM_QOS);
  }
#endif

  if (prop_enable_udp) {
    dds_qos.transport.addTransport(ddst::TRANSPORT_UDPv4_QOS);
  }

  if (prop_enable_tcp) {
    dds_qos.transport.addTransport(ddst::TRANSPORT_TCPv4_QOS);
  }

  std::vector<std::string> ip_str_list;

  if (prop_ip_str.empty()) {
    ip_str_list = default_ip_list;
  } else {
    ip_str_list = Helpers::get_split_string(prop_ip_str, ';');
  }

  std::vector<std::string> dev_name_list;
  dev_name_list.reserve(ip_str_list.size());

  bool has_ipv4 = false;
  bool has_ipv6 = false;

  for (const auto& ip : ip_str_list) {
    std::string dev;

    if (ip.find(':') != std::string::npos) {
      dev = Utils::get_interface_name_by_ipv6(ip);

      if (!dev.empty()) {
        has_ipv6 = true;
      }
    } else {
      dev = Utils::get_interface_name_by_ipv4(ip);

      if (!dev.empty()) {
        has_ipv4 = true;
      }
    }

    if (!dev.empty()) {
      dev_name_list.emplace_back(std::move(dev));
    }
  }

  dds_qos.network_interface.interface_whitelist = std::move(dev_name_list);

  if (has_ipv4 && has_ipv6) {
    dds_qos.discovery.kind = ddst::DISCOVER_UDPv4_AND_UDPv6_QOS;
  } else if (has_ipv6) {
    dds_qos.discovery.kind = ddst::DISCOVER_UDPv6_QOS;
  } else {
    dds_qos.discovery.kind = ddst::DISCOVER_UDPv4_QOS;
  }
}

std::string DdstFactory::get_qos_ext(const Conf::PropertiesMap& ext, const std::string& key) {
  auto iter = ext.find(key);

  if (iter == ext.end()) {
    return "";
  }

  return iter->second;
}

}  // namespace vlink
