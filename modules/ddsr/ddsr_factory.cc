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

#include "./ddsr_factory.hpp"

#include <charconv>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "./base/helpers.h"
#include "./base/utils.h"
#include "./ddsr_qos.hpp"
#include "./extension/qos_profile.h"
#include "./impl/ssl_options.h"

namespace vlink {

// DdsrFactory
DdsrFactory::DdsrFactory() {
  Bytes::init_memory_pool();

  if VUNLIKELY (DdsrConf::get_thread_count() != 1) {
    VLOG_W("DdsrFactory: Ddsr does not support setting thread count.");
  }

  dds_factory_ = DDS_DomainParticipantFactory_get_instance();

  for (const auto& [name, qos] : QosProfile::get_available_qos_map()) {
    DdsrConf::register_qos_internal(name, qos);
  }

  std::string dds_debug_str = Utils::get_env("VLINK_DDS_DEBUG");

  NDDS_Config_Logger* dds_logger = NDDS_Config_Logger_get_instance();

  if (dds_debug_str == "1") {
    NDDS_Config_Logger_set_verbosity(dds_logger, NDDS_CONFIG_LOG_VERBOSITY_STATUS_ALL);
  } else {
    NDDS_Config_Logger_set_verbosity(dds_logger, NDDS_CONFIG_LOG_VERBOSITY_ERROR);
  }

  std::string default_event_qos_str = Utils::get_env("VLINK_DDS_EVENT_QOS");
  std::string default_method_qos_str = Utils::get_env("VLINK_DDS_METHOD_QOS");
  std::string default_field_qos_str = Utils::get_env("VLINK_DDS_FIELD_QOS");

  if (default_event_qos_str.empty()) {
    default_event_qos_ = QosProfile::kEvent;
  } else {
    default_event_qos_ = DdsrConf::find_qos(default_event_qos_str);
  }

  if (default_method_qos_str.empty()) {
    default_method_qos_ = QosProfile::kMethod;
  } else {
    default_method_qos_ = DdsrConf::find_qos(default_method_qos_str);
  }

  if (default_field_qos_str.empty()) {
    default_field_qos_ = QosProfile::kField;
  } else {
    default_field_qos_ = DdsrConf::find_qos(default_field_qos_str);
  }
}

DdsrFactory::~DdsrFactory() {
  DDS_DomainParticipantFactory_finalize_instance();

  dds_factory_ = nullptr;
}

std::shared_ptr<ddsr::DomainParticipant> DdsrFactory::create_participant(uint8_t type, const DdsrConf& conf,
                                                                         const Conf::PropertiesMap& properties) {
  static auto& factory = DdsrFactory::get();

  const auto& dds_qos_ext = get_qos_ext(conf.qos_ext, "part");
  const auto& id = std::make_tuple(type, conf.domain, dds_qos_ext, properties);

  std::lock_guard lifecycle_lock(factory.participant_mtx_);
  std::unique_lock lock(factory.mtx_);
  std::shared_ptr<ddsr::DomainParticipant> part = get_weak_ptr(factory.part_map_, id).lock();

  if (!part) {
    lock.unlock();

    DDS_DomainParticipantQos dds_qos;
    DDS_DomainParticipantQos_initialize(&dds_qos);

    if (dds_qos_ext.empty()) {
      DDS_DomainParticipantQos_get_defaultI(&dds_qos);
    } else {
      auto ret = DDS_DomainParticipantFactory_get_participant_qos_from_profile(
          factory.dds_factory_, &dds_qos, DDS_BUILTIN_QOS_LIB, dds_qos_ext.c_str());

      if VUNLIKELY (ret != DDS_RETCODE_OK) {
        VLOG_E("DdsrFactory: Failed to load participant QoS profile [", dds_qos_ext, "].");
        DDS_DomainParticipantQos_finalize(&dds_qos);
        return nullptr;
      }
    }

    set_participant_qos(dds_qos, properties);

    auto* ptr = new ddsr::DomainParticipant(conf.domain, &dds_qos);

    DDS_DomainParticipantQos_finalize(&dds_qos);

    if VUNLIKELY (!ptr || !ptr->entity) {
      VLOG_E("DdsrFactory: Failed to create participant.");
      delete ptr;
      return nullptr;
    }

    part = std::shared_ptr<ddsr::DomainParticipant>(ptr, [id](ddsr::DomainParticipant* part) {
      std::lock_guard lifecycle_lock(factory.participant_mtx_);

      {
        std::lock_guard lock(factory.mtx_);
        auto iter = factory.part_map_.find(id);

        if (iter != factory.part_map_.end() && iter->second.expired()) {
          factory.part_map_.erase(iter);
        }
      }
      delete part;
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

std::shared_ptr<ddsr::Topic> DdsrFactory::create_topic(uint8_t type, const DdsrConf& conf,
                                                       ddsr::DomainParticipant* part, std::string topic) {
  static auto& factory = DdsrFactory::get();

  const auto& dds_qos_ext = get_qos_ext(conf.qos_ext, "topic");

  if (topic.empty()) {
    topic = conf.topic;
  }

  if VUNLIKELY (!part || !part->entity) {
    VLOG_E("DdsrFactory: Cannot create topic without participant.");
    return nullptr;
  }

  const auto& id = std::make_tuple(type, conf.domain, topic, part);

  std::unique_lock lock(factory.mtx_);
  std::shared_ptr<ddsr::Topic> dds_topic = get_weak_ptr(factory.topic_map_, id).lock();

  if (!dds_topic) {
    lock.unlock();

    auto* ptr = new ddsr::Topic(part->entity, topic, vlink_BuiltInRawTypeSupport_get_type_name(), nullptr, dds_qos_ext);

    if VUNLIKELY (!ptr || !ptr->entity) {
      VLOG_E("DdsrFactory: Failed to create topic: ", topic, ".");
      delete ptr;
      return nullptr;
    }

    dds_topic = std::shared_ptr<ddsr::Topic>(ptr, [id](ddsr::Topic* topic) {
      {
        std::lock_guard lock(factory.mtx_);
        auto iter = factory.topic_map_.find(id);

        if (iter != factory.topic_map_.end() && iter->second.expired()) {
          factory.topic_map_.erase(iter);
        }
      }

      delete topic;
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

std::pair<std::shared_ptr<ddsr::Topic>, std::shared_ptr<ddsr::Topic> > DdsrFactory::create_method_topic(
    uint8_t type, const DdsrConf& conf, ddsr::DomainParticipant* part) {
  const std::string& resp_topic = conf.topic + DdsrConf::kRespSuffix;

  if VUNLIKELY (conf.topic.empty() || resp_topic.empty()) {
    VLOG_F("DdsrFactory: Method conf topic error.");
  }

  if VUNLIKELY (conf.topic == resp_topic) {
    VLOG_F("DdsrFactory: Method conf topic req and resp cannot be equal.");
  }

  return {create_topic(type, conf, part, conf.topic), create_topic(type, conf, part, resp_topic)};
}

std::shared_ptr<ddsr::Publisher> DdsrFactory::create_publisher(uint8_t type, const DdsrConf& conf,
                                                               ddsr::DomainParticipant* part) {
  static auto& factory = DdsrFactory::get();

  const auto& dds_qos_ext = get_qos_ext(conf.qos_ext, "pub");
  const auto& writer_qos = get_qos_ext(conf.qos_ext, "writer");

  if VUNLIKELY (!part || !part->entity) {
    VLOG_E("DdsrFactory: Cannot create publisher without participant.");
    return nullptr;
  }

  const auto& id = std::make_tuple(type, conf.domain, conf.qos, dds_qos_ext, writer_qos, part);

  std::unique_lock lock(factory.mtx_);
  std::shared_ptr<ddsr::Publisher> publisher = get_weak_ptr(factory.publisher_map_, id).lock();

  if (!publisher) {
    lock.unlock();

    auto* ptr = new ddsr::Publisher(part->entity, nullptr, dds_qos_ext);

    if VUNLIKELY (!ptr || !ptr->entity) {
      VLOG_E("DdsrFactory: Failed to create publisher.");
      delete ptr;
      return nullptr;
    }

    publisher = std::shared_ptr<ddsr::Publisher>(ptr, [id](ddsr::Publisher* publisher) {
      {
        std::lock_guard lock(factory.mtx_);
        auto iter = factory.publisher_map_.find(id);

        if (iter != factory.publisher_map_.end() && iter->second.expired()) {
          factory.publisher_map_.erase(iter);
        }
      }

      delete publisher;
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

std::shared_ptr<ddsr::Subscriber> DdsrFactory::create_subscriber(uint8_t type, const DdsrConf& conf,
                                                                 ddsr::DomainParticipant* part) {
  static auto& factory = DdsrFactory::get();

  const auto& dds_qos_ext = get_qos_ext(conf.qos_ext, "sub");
  const auto& reader_qos = get_qos_ext(conf.qos_ext, "reader");

  if VUNLIKELY (!part || !part->entity) {
    VLOG_E("DdsrFactory: Cannot create subscriber without participant.");
    return nullptr;
  }

  const auto& id = std::make_tuple(type, conf.domain, conf.qos, dds_qos_ext, reader_qos, part);

  std::unique_lock lock(factory.mtx_);
  std::shared_ptr<ddsr::Subscriber> subscriber = get_weak_ptr(factory.subscriber_map_, id).lock();

  if (!subscriber) {
    lock.unlock();

    auto* ptr = new ddsr::Subscriber(part->entity, nullptr, dds_qos_ext);

    if VUNLIKELY (!ptr || !ptr->entity) {
      VLOG_E("DdsrFactory: Failed to create subscriber.");
      delete ptr;
      return nullptr;
    }

    subscriber = std::shared_ptr<ddsr::Subscriber>(ptr, [id](ddsr::Subscriber* subscriber) {
      {
        std::lock_guard lock(factory.mtx_);
        auto iter = factory.subscriber_map_.find(id);

        if (iter != factory.subscriber_map_.end() && iter->second.expired()) {
          factory.subscriber_map_.erase(iter);
        }
      }

      delete subscriber;
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

std::shared_ptr<ddsr::DataWriter> DdsrFactory::create_datawriter(uint8_t type, const DdsrConf& conf,
                                                                 ddsr::Publisher* publisher, ddsr::Topic* topic,
                                                                 DDS_DataWriterListener* listener) {
  static auto& factory = DdsrFactory::get();

  const auto& dds_qos_ext = get_qos_ext(conf.qos_ext, "writer");

  if VUNLIKELY (!publisher || !publisher->entity || !topic || !topic->entity) {
    VLOG_E("DdsrFactory: Cannot create datawriter without publisher/topic.");
    return nullptr;
  }

  std::shared_ptr<ddsr::DataWriter> ptr;

  if (dds_qos_ext.empty()) {
    DDS_DataWriterQos dds_qos;
    DDS_DataWriterQos_initialize(&dds_qos);
    DDS_DataWriterQos_get_defaultI(&dds_qos);

    if (conf.qos.empty()) {
      if ((type & kPublisher) || (type & kSubscriber)) {
        convert_qos(dds_qos, factory.default_event_qos_, conf.depth);
      } else if ((type & kClient) || (type & kServer)) {
        convert_qos(dds_qos, factory.default_method_qos_, conf.depth);
      } else if ((type & kSetter) || (type & kGetter)) {
        convert_qos(dds_qos, factory.default_field_qos_, conf.depth);
      }
    } else {
      convert_qos(dds_qos, DdsrConf::find_qos(conf.qos), conf.depth);
    }

    ptr = std::make_shared<ddsr::DataWriter>(publisher->entity, topic->entity, listener, &dds_qos);

    DDS_DataWriterQos_finalize(&dds_qos);
  } else {
    ptr = std::make_shared<ddsr::DataWriter>(publisher->entity, topic->entity, listener, nullptr, dds_qos_ext);
  }

  if VUNLIKELY (!ptr || !ptr->entity) {
    VLOG_E("DdsrFactory: Failed to create datawriter.");
    return nullptr;
  }

  return ptr;
}

std::shared_ptr<ddsr::DataReader> DdsrFactory::create_datareader(uint8_t type, const DdsrConf& conf,
                                                                 ddsr::Subscriber* subscriber, ddsr::Topic* topic,
                                                                 DDS_DataReaderListener* listener) {
  static auto& factory = DdsrFactory::get();

  const auto& dds_qos_ext = get_qos_ext(conf.qos_ext, "reader");

  if VUNLIKELY (!subscriber || !subscriber->entity || !topic || !topic->entity) {
    VLOG_E("DdsrFactory: Cannot create datareader without subscriber/topic.");
    return nullptr;
  }

  std::shared_ptr<ddsr::DataReader> ptr;

  if (dds_qos_ext.empty()) {
    DDS_DataReaderQos dds_qos;
    DDS_DataReaderQos_initialize(&dds_qos);
    DDS_DataReaderQos_get_defaultI(&dds_qos);

    if (conf.qos.empty()) {
      if ((type & kPublisher) || (type & kSubscriber)) {
        convert_qos(dds_qos, factory.default_event_qos_, conf.depth);
      } else if ((type & kClient) || (type & kServer)) {
        convert_qos(dds_qos, factory.default_method_qos_, conf.depth);
      } else if ((type & kSetter) || (type & kGetter)) {
        convert_qos(dds_qos, factory.default_field_qos_, conf.depth);
      }
    } else {
      convert_qos(dds_qos, DdsrConf::find_qos(conf.qos), conf.depth);
    }

    ptr = std::make_shared<ddsr::DataReader>(subscriber->entity, topic->entity, listener, &dds_qos);

    DDS_DataReaderQos_finalize(&dds_qos);
  } else {
    ptr = std::make_shared<ddsr::DataReader>(subscriber->entity, topic->entity, listener, nullptr, dds_qos_ext);
  }

  if VUNLIKELY (!ptr || !ptr->entity) {
    VLOG_E("DdsrFactory: Failed to create datareader.");
    return nullptr;
  }

  return ptr;
}

bool DdsrFactory::write_data(DDS_DataWriter* writer, const Bytes& bytes, uint64_t id) {
  auto* arrow = vlink_BuiltInRawDataWriter_narrow(writer);

  vlink_BuiltInRaw msg;

  msg.id = id;
  msg.data = DDS_SEQUENCE_INITIALIZER;
  msg.data._contiguous_buffer = const_cast<uint8_t*>(bytes.data());
  msg.data._length = bytes.size();
  msg.data._maximum = bytes.size();
  msg.data._owned = DDS_BOOLEAN_FALSE;
  msg.data._elementAllocParams = {DDS_BOOLEAN_FALSE, DDS_BOOLEAN_FALSE, DDS_BOOLEAN_FALSE};
  msg.data._elementDeallocParams = {DDS_BOOLEAN_FALSE, DDS_BOOLEAN_FALSE};

  auto ret = vlink_BuiltInRawDataWriter_write(arrow, &msg, &DDS_HANDLE_NIL);

  return ret == DDS_RETCODE_OK;
}

bool DdsrFactory::take_data(DDS_DataReader* reader, ReadMessage& msg) {
  auto* arrow = vlink_BuiltInRawDataReader_narrow(reader);

  auto ret = vlink_BuiltInRawDataReader_take(arrow, &msg.seq, &msg.info_seq, 1, DDS_ANY_SAMPLE_STATE,
                                             DDS_ANY_VIEW_STATE, DDS_ANY_INSTANCE_STATE);

  if (ret == DDS_RETCODE_NO_DATA) {
    return false;
  }

  if VUNLIKELY (ret != DDS_RETCODE_OK) {
    VLOG_E("DdsrFactory: Failed to take data.");
    return false;
  }

  msg.info = DDS_SampleInfoSeq_get_reference(&msg.info_seq, 0);

  if VUNLIKELY (!msg.info) {
    release_data(reader, msg);
    return false;
  }

  auto* sample = vlink_BuiltInRawSeq_get_reference(&msg.seq, 0);

  msg.id = sample->id;

  msg.bytes = Bytes::shallow_copy(sample->data._contiguous_buffer, sample->data._length);

  msg.timestamp =
      static_cast<int64_t>(msg.info->source_timestamp.sec) * 1000000000LL + msg.info->source_timestamp.nanosec;

  return true;
}

bool DdsrFactory::release_data(DDS_DataReader* reader, ReadMessage& msg) {
  auto* arrow = vlink_BuiltInRawDataReader_narrow(reader);

  auto ret = vlink_BuiltInRawDataReader_return_loan(arrow, &msg.seq, &msg.info_seq);

  return ret == DDS_RETCODE_OK;
}

uint64_t DdsrFactory::get_guid(DDS_GUID_t* guid, uint32_t seq) {
  uint64_t result = 14695981039346656037ULL;

  for (const auto value : guid->value) {
    result ^= static_cast<uint64_t>(value);
    result *= 1099511628211ULL;
  }

  for (size_t i = 0; i < sizeof(seq); ++i) {
    result ^= static_cast<uint64_t>((seq >> (i * 8)) & 0xFFU);
    result *= 1099511628211ULL;
  }

  return result;
}

int DdsrFactory::get_default_domain_id() {
  const std::string& domain_str = Utils::get_env("VLINK_DDS_DOMAIN");
  return Helpers::to_int(domain_str, 0);
}

DDS_DomainParticipantFactory* DdsrFactory::get_dds_factory() {
  static auto& factory = DdsrFactory::get();

  return factory.dds_factory_;
}

void DdsrFactory::set_participant_qos(DDS_DomainParticipantQos& dds_qos, const Conf::PropertiesMap& properties) {
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

  std::string prop_ip_str = ip_str;
  std::string prop_ip_multicast_str = ip_multicast_str;
  std::string prop_peer_str = peer_str;
  size_t prop_buf = 0;
  size_t prop_mtu = 0;
  bool prop_enable_udp = enable_udp;
  bool prop_enable_tcp = enable_tcp;
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
      DDS_PropertyQosPolicyHelper_add_property(&dds_qos.property, prop.c_str(), value.c_str(), DDS_BOOLEAN_FALSE);
    }
  }

  (void)prop_enable_less_memory;

  DDS_TransportBuiltinKindMask transport_mask = DDS_TRANSPORTBUILTIN_MASK_NONE;

  if (prop_enable_udp) {
    transport_mask |= DDS_TRANSPORTBUILTIN_UDPv4;
  }

#if !defined(__ANDROID__) && !defined(_WIN32)

  if (prop_enable_shm) {
    transport_mask |= DDS_TRANSPORTBUILTIN_SHMEM;
  }
#endif

  dds_qos.transport_builtin.mask = transport_mask;

  std::vector<std::string> ip_str_list;

  if (prop_ip_str.empty()) {
    ip_str_list = default_ip_list;
  } else {
    ip_str_list = Helpers::split_any(prop_ip_str);
  }

  std::string joined_ips;

  if (!ip_str_list.empty()) {
    for (size_t i = 0; i < ip_str_list.size(); ++i) {
      if (i > 0) {
        joined_ips.push_back(',');
      }

      joined_ips.append(ip_str_list[i]);
    }

    DDS_PropertyQosPolicyHelper_assert_property(&dds_qos.property,
                                                "dds.transport.UDPv4.builtin.parent.allow_interfaces",
                                                joined_ips.c_str(), DDS_BOOLEAN_FALSE);
  }

  if (prop_buf > 0) {
    const std::string buf_value = std::to_string(prop_buf);

    DDS_PropertyQosPolicyHelper_assert_property(
        &dds_qos.property, "dds.transport.UDPv4.builtin.send_socket_buffer_size", buf_value.c_str(), DDS_BOOLEAN_FALSE);
    DDS_PropertyQosPolicyHelper_assert_property(
        &dds_qos.property, "dds.transport.UDPv4.builtin.recv_socket_buffer_size", buf_value.c_str(), DDS_BOOLEAN_FALSE);
  }

  if (prop_mtu > 0) {
    const std::string mtu_value = std::to_string(prop_mtu);

    DDS_PropertyQosPolicyHelper_assert_property(
        &dds_qos.property, "dds.transport.UDPv4.builtin.parent.message_size_max", mtu_value.c_str(), DDS_BOOLEAN_FALSE);
  }

  if (!prop_peer_str.empty()) {
    const auto peer_list = Helpers::split_any(prop_peer_str);

    if (!peer_list.empty()) {
      const auto peer_count = static_cast<DDS_Long>(peer_list.size());

      DDS_StringSeq_set_maximum(&dds_qos.discovery.initial_peers, peer_count);
      DDS_StringSeq_set_length(&dds_qos.discovery.initial_peers, peer_count);

      for (size_t i = 0; i < peer_list.size(); ++i) {
        char** ref = DDS_StringSeq_get_reference(&dds_qos.discovery.initial_peers, static_cast<DDS_Long>(i));

        if (ref != nullptr) {
          DDS_String_replace(ref, peer_list[i].c_str());
        }
      }
    }
  }

  if (!prop_ip_multicast_str.empty()) {
    const auto multicast_list = Helpers::split_any(prop_ip_multicast_str);

    if (!multicast_list.empty()) {
      const auto multicast_count = static_cast<DDS_Long>(multicast_list.size());

      DDS_StringSeq_set_maximum(&dds_qos.discovery.multicast_receive_addresses, multicast_count);
      DDS_StringSeq_set_length(&dds_qos.discovery.multicast_receive_addresses, multicast_count);

      for (size_t i = 0; i < multicast_list.size(); ++i) {
        char** ref =
            DDS_StringSeq_get_reference(&dds_qos.discovery.multicast_receive_addresses, static_cast<DDS_Long>(i));

        if (ref != nullptr) {
          DDS_String_replace(ref, multicast_list[i].c_str());
        }
      }
    }
  }

  auto ssl_cfg = SslOptions::parse_from(properties);

  bool ssl_cfg_valid = ssl_cfg.is_valid();

  if (ssl_cfg_valid && !prop_enable_tcp) {
    prop_enable_tcp = true;
  }

  if (prop_enable_tcp) {
    static constexpr const char* kTcpAlias = "dds.transport.tcp.tcp1";

    DDS_PropertyQosPolicyHelper_assert_property(&dds_qos.property, "dds.transport.load_plugins", kTcpAlias,
                                                DDS_BOOLEAN_FALSE);
    DDS_PropertyQosPolicyHelper_assert_property(&dds_qos.property, "dds.transport.tcp.tcp1.library", "nddstransporttcp",
                                                DDS_BOOLEAN_FALSE);
    DDS_PropertyQosPolicyHelper_assert_property(&dds_qos.property, "dds.transport.tcp.tcp1.create_function",
                                                "NDDS_Transport_TCPv4_create", DDS_BOOLEAN_FALSE);

    if (!joined_ips.empty()) {
      DDS_PropertyQosPolicyHelper_assert_property(&dds_qos.property, "dds.transport.tcp.tcp1.parent.allow_interfaces",
                                                  joined_ips.c_str(), DDS_BOOLEAN_FALSE);
    }

    if (prop_buf > 0) {
      const std::string buf_value = std::to_string(prop_buf);

      DDS_PropertyQosPolicyHelper_assert_property(&dds_qos.property, "dds.transport.tcp.tcp1.send_socket_buffer_size",
                                                  buf_value.c_str(), DDS_BOOLEAN_FALSE);
      DDS_PropertyQosPolicyHelper_assert_property(&dds_qos.property, "dds.transport.tcp.tcp1.recv_socket_buffer_size",
                                                  buf_value.c_str(), DDS_BOOLEAN_FALSE);
    }

    if (prop_mtu > 0) {
      const std::string mtu_value = std::to_string(prop_mtu);

      DDS_PropertyQosPolicyHelper_assert_property(&dds_qos.property, "dds.transport.tcp.tcp1.parent.message_size_max",
                                                  mtu_value.c_str(), DDS_BOOLEAN_FALSE);
    }

    if (ssl_cfg_valid) {
      DDS_PropertyQosPolicyHelper_assert_property(&dds_qos.property, "dds.transport.tcp.tcp1.parent.classid",
                                                  "NDDS_TRANSPORT_CLASSID_TLSV4_LAN", DDS_BOOLEAN_FALSE);

      if (!ssl_cfg.ca_file.empty()) {
        DDS_PropertyQosPolicyHelper_assert_property(&dds_qos.property, "dds.transport.tcp.tcp1.tls.verify.ca_file",
                                                    ssl_cfg.ca_file.c_str(), DDS_BOOLEAN_FALSE);
      }

      if (!ssl_cfg.cert_file.empty()) {
        DDS_PropertyQosPolicyHelper_assert_property(&dds_qos.property,
                                                    "dds.transport.tcp.tcp1.tls.identity.certificate_chain_file",
                                                    ssl_cfg.cert_file.c_str(), DDS_BOOLEAN_FALSE);
      }

      if (!ssl_cfg.key_file.empty()) {
        DDS_PropertyQosPolicyHelper_assert_property(&dds_qos.property,
                                                    "dds.transport.tcp.tcp1.tls.identity.private_key_file",
                                                    ssl_cfg.key_file.c_str(), DDS_BOOLEAN_FALSE);
      }

      if (!ssl_cfg.key_password.empty()) {
        DDS_PropertyQosPolicyHelper_assert_property(&dds_qos.property,
                                                    "dds.transport.tcp.tcp1.tls.identity.private_key_password",
                                                    ssl_cfg.key_password.c_str(), DDS_BOOLEAN_FALSE);
      }

      if (!ssl_cfg.ciphers.empty()) {
        DDS_PropertyQosPolicyHelper_assert_property(&dds_qos.property, "dds.transport.tcp.tcp1.tls.cipher.cipher_list",
                                                    ssl_cfg.ciphers.c_str(), DDS_BOOLEAN_FALSE);
      }

      if (!ssl_cfg.verify_peer) {
        DDS_PropertyQosPolicyHelper_assert_property(&dds_qos.property, "dds.transport.tcp.tcp1.tls.verify.verify_depth",
                                                    "0", DDS_BOOLEAN_FALSE);
      }
    }
  }
}

std::string DdsrFactory::get_qos_ext(const Conf::PropertiesMap& ext, const std::string& key) {
  auto iter = ext.find(key);

  if (iter == ext.end()) {
    return "";
  }

  return iter->second;
}

}  // namespace vlink
