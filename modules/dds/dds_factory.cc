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

#include "./dds_factory.h"

#include <charconv>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "./base/helpers.h"
#include "./base/utils.h"
#include "./dds_qos.h"
#include "./extension/qos_profile.h"
#include "./impl/ssl_options.h"

namespace vlink {

// DdsFactory
DdsFactory::DdsFactory() {
  Bytes::init_memory_pool();

  if VUNLIKELY (DdsConf::get_thread_count() != 1) {
    VLOG_W("DdsFactory: Dds does not support setting thread count.");
  }

  for (const auto& [name, qos] : QosProfile::get_available_qos_map()) {
    DdsConf::register_qos_internal(name, qos);
  }

  std::string dds_debug_str = Utils::get_env("VLINK_DDS_DEBUG");

  if (dds_debug_str == "1") {
    dds::Log::SetVerbosity(dds::Log::Kind::Info);
  } else {
    dds::Log::SetVerbosity(dds::Log::Kind::Error);
  }

  dds_factory_ = dds::DomainParticipantFactory::get_instance();

  std::string qos_file = Utils::get_env("VLINK_FASTDDS_QOS_FILE");

  if (!qos_file.empty()) {
    dds_factory_->load_XML_profiles_file(qos_file);
  }

  std::string default_event_qos_str = Utils::get_env("VLINK_DDS_EVENT_QOS");
  std::string default_method_qos_str = Utils::get_env("VLINK_DDS_METHOD_QOS");
  std::string default_field_qos_str = Utils::get_env("VLINK_DDS_FIELD_QOS");

  if (default_event_qos_str.empty()) {
    default_event_qos_ = QosProfile::kEvent;
  } else {
    default_event_qos_ = DdsConf::find_qos(default_event_qos_str);
  }

  if (default_method_qos_str.empty()) {
    default_method_qos_ = QosProfile::kMethod;
  } else {
    default_method_qos_ = DdsConf::find_qos(default_method_qos_str);
  }

  if (default_field_qos_str.empty()) {
    default_field_qos_ = QosProfile::kField;
  } else {
    default_field_qos_ = DdsConf::find_qos(default_field_qos_str);
  }
}

DdsFactory::~DdsFactory() = default;

std::vector<std::tuple<std::string, std::string>> DdsFactory::get_discovered_topics(int32_t _domain) {
  std::vector<std::tuple<std::string, std::string>> topics;

  static auto& factory = DdsFactory::get();

  auto* part = factory.dds_factory_->lookup_participant(_domain);

  if VUNLIKELY (!part) {
    return topics;
  }

  std::vector<dds::InstanceHandle_t> topic_handles;
  part->get_discovered_topics(topic_handles);

  topics.reserve(topic_handles.size());

  for (const auto& instance : topic_handles) {
    dds::builtin::TopicBuiltinTopicData topic_data;
    part->get_discovered_topic_data(topic_data, instance);
    topics.emplace_back(std::forward_as_tuple(std::move(topic_data.name), std::move(topic_data.type_name)));
  }

  return topics;
}

bool DdsFactory::load_global_qos_file(const std::string& filepath) {
  static auto& factory = DdsFactory::get();

#ifdef VLINK_SUPPORT_DDS_V3
  return factory.dds_factory_->load_XML_profiles_file(filepath) == dds::RETCODE_OK;
#else
  return factory.dds_factory_->load_XML_profiles_file(filepath) == ReturnCode_t::RETCODE_OK;
#endif
}

std::shared_ptr<dds::DomainParticipant> DdsFactory::create_participant(uint8_t type, const DdsConf& conf,
                                                                       const Conf::PropertiesMap& properties) {
  static auto& factory = DdsFactory::get();

  const auto& dds_qos_ext = get_qos_ext(conf.qos_ext, "part");
  const auto& id = std::make_tuple(type, conf.domain, dds_qos_ext, properties);

  std::unique_lock lock(factory.mtx_);

  std::shared_ptr<dds::DomainParticipant> part = get_weak_ptr(factory.part_map_, id).lock();

  if (!part) {
    lock.unlock();

    dds::DomainParticipant* ptr = nullptr;

    if (dds_qos_ext.empty()) {
      auto dds_qos = dds::PARTICIPANT_QOS_DEFAULT;

      set_participant_qos(dds_qos, properties);

      ptr = factory.dds_factory_->create_participant(conf.domain, dds_qos, nullptr, dds::StatusMask::all());
    } else {
      ptr = factory.dds_factory_->create_participant_with_profile(conf.domain, dds_qos_ext, nullptr,
                                                                  dds::StatusMask::all());
    }

    if VUNLIKELY (!ptr) {
      VLOG_E("DdsFactory: Failed to create participant.");
      return nullptr;
    }

    part = std::shared_ptr<dds::DomainParticipant>(ptr, [id](dds::DomainParticipant* part) {
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

std::shared_ptr<dds::Topic> DdsFactory::create_topic(uint8_t type, const DdsConf& conf, dds::DomainParticipant* part,
                                                     bool is_cdr_type, std::string topic) {
  static auto& factory = DdsFactory::get();

  dds::TypeSupport type_support;

  if (topic.empty()) {
    topic = conf.topic;
  }

  Function<void*()> type_support_callback = DdsConf::find_type_support(topic);

  if (is_cdr_type) {
    if VUNLIKELY (!type_support_callback) {
      VLOG_F("DdsFactory: Topic ", topic, " has no registered typesupport.");
    }
  } else {
    if VUNLIKELY (type_support_callback) {
      VLOG_F("DdsFactory: Topic ", topic, " does not support BuiltIn::Raw.");
    }

    if VUNLIKELY (!factory.raw_typesupport_) {
      factory.raw_typesupport_.reset(new BuiltInRawPubSubType);  // NOLINT(modernize-make-shared)
    }
  }

  const auto& dds_qos_ext = get_qos_ext(conf.qos_ext, "topic");

  if VUNLIKELY (!part) {
    VLOG_E("DdsFactory: Cannot create topic without participant.");
    return nullptr;
  }

  const auto& id = std::make_tuple(type, conf.domain, topic, part);

  std::unique_lock lock(factory.mtx_);
  std::shared_ptr<dds::Topic> dds_topic = get_weak_ptr(factory.topic_map_, id).lock();

  if (!dds_topic) {
    lock.unlock();

    if (is_cdr_type) {
      type_support.reset(static_cast<dds::TopicDataType*>(type_support_callback()));
    } else {
      type_support = factory.raw_typesupport_;
    }

    if VLIKELY (type_support) {
      part->register_type(type_support);
    } else {
      VLOG_F("DdsFactory: Topic ", topic, " registration failed.");
    }

    dds::Topic* ptr = nullptr;

    if (dds_qos_ext.empty()) {
      auto dds_qos = dds::TOPIC_QOS_DEFAULT;

      ptr = part->create_topic(topic, type_support.get_type_name(), dds_qos);
    } else {
      ptr = part->create_topic_with_profile(topic, type_support.get_type_name(), dds_qos_ext);
    }

    if VUNLIKELY (!ptr) {
      VLOG_E("DdsFactory: Failed to create topic: ", topic, ".");
      return nullptr;
    }

    dds_topic = std::shared_ptr<dds::Topic>(ptr, [id](dds::Topic* topic) {
      {
        std::lock_guard lock(factory.mtx_);
        auto iter = factory.topic_map_.find(id);

        if (iter != factory.topic_map_.end() && iter->second.expired()) {
          factory.topic_map_.erase(iter);
        }
      }

      auto* participant = const_cast<dds::DomainParticipant*>(topic->get_participant());
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
  } else {
    type_support = part->find_type(dds_topic->get_type_name());

    if (!type_support) {
      if (is_cdr_type) {
        type_support.reset(static_cast<dds::TopicDataType*>(type_support_callback()));
      } else {
        type_support = factory.raw_typesupport_;
      }
    }

    if VLIKELY (type_support) {
      part->register_type(type_support);
    } else {
      VLOG_F("DdsFactory: Topic ", topic, " registration failed.");
    }
  }

  return dds_topic;
}

std::pair<std::shared_ptr<dds::Topic>, std::shared_ptr<dds::Topic>> DdsFactory::create_method_topic(
    uint8_t type, const DdsConf& conf, dds::DomainParticipant* part, bool is_cdr_type) {
  const std::string& resp_topic = conf.topic + DdsConf::kRespSuffix;

  if VUNLIKELY (conf.topic.empty() || resp_topic.empty()) {
    VLOG_F("DdsFactory: Method conf topic error.");
  }

  if VUNLIKELY (conf.topic == resp_topic) {
    VLOG_F("DdsFactory: Method conf topic req and resp cannot be equal.");
  }

  return {create_topic(type, conf, part, is_cdr_type, conf.topic),
          create_topic(type, conf, part, is_cdr_type, resp_topic)};
}

std::shared_ptr<dds::Publisher> DdsFactory::create_publisher(uint8_t type, const DdsConf& conf,
                                                             dds::DomainParticipant* part) {
  static auto& factory = DdsFactory::get();

  const auto& dds_qos_ext = get_qos_ext(conf.qos_ext, "pub");
  const auto& writer_qos = get_qos_ext(conf.qos_ext, "writer");

  if VUNLIKELY (!part) {
    VLOG_E("DdsFactory: Cannot create publisher without participant.");
    return nullptr;
  }

  const auto& id = std::make_tuple(type, conf.domain, conf.qos, dds_qos_ext, writer_qos, part);

  std::unique_lock lock(factory.mtx_);
  std::shared_ptr<dds::Publisher> publisher = get_weak_ptr(factory.publisher_map_, id).lock();

  if (!publisher) {
    lock.unlock();

    dds::Publisher* ptr = nullptr;

    if (dds_qos_ext.empty()) {
      auto dds_qos = dds::PUBLISHER_QOS_DEFAULT;

      ptr = part->create_publisher(dds_qos, nullptr);
    } else {
      ptr = part->create_publisher_with_profile(dds_qos_ext, nullptr);
    }

    if VUNLIKELY (!ptr) {
      VLOG_E("DdsFactory: Failed to create publisher.");
      return nullptr;
    }

    publisher = std::shared_ptr<dds::Publisher>(ptr, [id](dds::Publisher* publisher) {
      {
        std::lock_guard lock(factory.mtx_);
        auto iter = factory.publisher_map_.find(id);

        if (iter != factory.publisher_map_.end() && iter->second.expired()) {
          factory.publisher_map_.erase(iter);
        }
      }
      auto* participant = const_cast<dds::DomainParticipant*>(publisher->get_participant());
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

std::shared_ptr<dds::Subscriber> DdsFactory::create_subscriber(uint8_t type, const DdsConf& conf,
                                                               dds::DomainParticipant* part) {
  static auto& factory = DdsFactory::get();

  const auto& dds_qos_ext = get_qos_ext(conf.qos_ext, "sub");
  const auto& reader_qos = get_qos_ext(conf.qos_ext, "reader");

  if VUNLIKELY (!part) {
    VLOG_E("DdsFactory: Cannot create subscriber without participant.");
    return nullptr;
  }

  const auto& id = std::make_tuple(type, conf.domain, conf.qos, dds_qos_ext, reader_qos, part);

  std::unique_lock lock(factory.mtx_);

  std::shared_ptr<dds::Subscriber> subscriber = get_weak_ptr(factory.subscriber_map_, id).lock();

  if (!subscriber) {
    lock.unlock();
    dds::Subscriber* ptr = nullptr;
    if (dds_qos_ext.empty()) {
      auto dds_qos = dds::SUBSCRIBER_QOS_DEFAULT;

      ptr = part->create_subscriber(dds_qos, nullptr);
    } else {
      ptr = part->create_subscriber_with_profile(dds_qos_ext, nullptr);
    }

    if VUNLIKELY (!ptr) {
      VLOG_E("DdsFactory: Failed to create subscriber.");
      return nullptr;
    }

    subscriber = std::shared_ptr<dds::Subscriber>(ptr, [id](dds::Subscriber* subscriber) {
      {
        std::lock_guard lock(factory.mtx_);
        auto iter = factory.subscriber_map_.find(id);

        if (iter != factory.subscriber_map_.end() && iter->second.expired()) {
          factory.subscriber_map_.erase(iter);
        }
      }
      auto* participant = const_cast<dds::DomainParticipant*>(subscriber->get_participant());
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

std::shared_ptr<dds::DataWriter> DdsFactory::create_datawriter(uint8_t type, const DdsConf& conf,
                                                               dds::Publisher* publisher, dds::Topic* topic,
                                                               dds::DataWriterListener* listener) {
  static auto& factory = DdsFactory::get();

  const auto& dds_qos_ext = get_qos_ext(conf.qos_ext, "writer");

  if VUNLIKELY (!publisher || !topic) {
    VLOG_E("DdsFactory: Cannot create datawriter without publisher/topic.");
    return nullptr;
  }

  dds::DataWriter* ptr = nullptr;

  if (dds_qos_ext.empty()) {
    auto dds_qos = dds::DATAWRITER_QOS_DEFAULT;

    if (conf.qos.empty()) {
      if ((type & kPublisher) || (type & kSubscriber)) {
        convert_qos(dds_qos, factory.default_event_qos_, conf.depth);
      } else if ((type & kClient) || (type & kServer)) {
        convert_qos(dds_qos, factory.default_method_qos_, conf.depth);
      } else if ((type & kSetter) || (type & kGetter)) {
        convert_qos(dds_qos, factory.default_field_qos_, conf.depth);
      }
    } else {
      convert_qos(dds_qos, DdsConf::find_qos(conf.qos), conf.depth);
    }

    ptr = publisher->create_datawriter(topic, dds_qos, listener);
  } else {
    ptr = publisher->create_datawriter_with_profile(topic, dds_qos_ext, listener);
  }

  if VUNLIKELY (!ptr) {
    VLOG_E("DdsFactory: Failed to create datawriter.");
    return nullptr;
  }

  return std::shared_ptr<dds::DataWriter>(ptr, [](dds::DataWriter* writer) {
    auto* publisher = const_cast<dds::Publisher*>(writer->get_publisher());
    publisher->delete_datawriter(writer);
  });
}

std::shared_ptr<dds::DataReader> DdsFactory::create_datareader(uint8_t type, const DdsConf& conf,
                                                               dds::Subscriber* subscriber, dds::Topic* topic,
                                                               dds::DataReaderListener* listener) {
  static auto& factory = DdsFactory::get();

  const auto& dds_qos_ext = get_qos_ext(conf.qos_ext, "reader");

  if VUNLIKELY (!subscriber || !topic) {
    VLOG_E("DdsFactory: Cannot create datareader without subscriber/topic.");
    return nullptr;
  }

  dds::DataReader* ptr = nullptr;

  if (dds_qos_ext.empty()) {
    auto dds_qos = dds::DATAREADER_QOS_DEFAULT;

    if (conf.qos.empty()) {
      if ((type & kPublisher) || (type & kSubscriber)) {
        convert_qos(dds_qos, factory.default_event_qos_, conf.depth);
      } else if ((type & kClient) || (type & kServer)) {
        convert_qos(dds_qos, factory.default_method_qos_, conf.depth);
      } else if ((type & kSetter) || (type & kGetter)) {
        convert_qos(dds_qos, factory.default_field_qos_, conf.depth);
      }
    } else {
      convert_qos(dds_qos, DdsConf::find_qos(conf.qos), conf.depth);
    }

    ptr = subscriber->create_datareader(topic, dds_qos, listener);
  } else {
    ptr = subscriber->create_datareader_with_profile(topic, dds_qos_ext, listener);
  }

  if VUNLIKELY (!ptr) {
    VLOG_E("DdsFactory: Failed to create datareader.");
    return nullptr;
  }

  return std::shared_ptr<dds::DataReader>(ptr, [](dds::DataReader* reader) {
    auto* subscriber = const_cast<dds::Subscriber*>(reader->get_subscriber());
    subscriber->delete_datareader(reader);
  });
}

bool DdsFactory::write_data(dds::DataWriter* writer, const Bytes& bytes, uint64_t id) {
  if VUNLIKELY (bytes.is_ptr()) {
    VLOG_E("DdsFactory: write_data() type mismatch, expected raw bytes but received ptr type.");
    return false;
  }

  BuiltInRaw raw;
  raw.id() = id;

  raw.data().shallow_copy(bytes);

#ifdef VLINK_SUPPORT_DDS_V3
  return writer->write(&raw) == dds::RETCODE_OK;
#else
  return writer->write(&raw);
#endif
}

bool DdsFactory::write_cdr_data(dds::DataWriter* writer, const Bytes& bytes, rtps::WriteParams* params) {
  if VUNLIKELY (!bytes.is_ptr()) {
    VLOG_E("DdsFactory: write_cdr_data() type mismatch, expected ptr type but received raw bytes.");
    return false;
  }

#ifdef VLINK_SUPPORT_DDS_V3

  if (params) {
    return writer->write(bytes.to_ptr(), *params) == dds::RETCODE_OK;
  }

  return writer->write(bytes.to_ptr()) == dds::RETCODE_OK;
#else

  if (params) {
    return writer->write(bytes.to_ptr(), *params);
  }

  return writer->write(bytes.to_ptr());
#endif
}

bool DdsFactory::take_data(dds::DataReader* reader, ReadMessage& msg) {
  auto ret = reader->take_next_sample(&msg.raw, &msg.info);

#ifdef VLINK_SUPPORT_DDS_V3

  if (ret == dds::RETCODE_NO_DATA) {
    return false;
  }

  if VUNLIKELY (ret != dds::RETCODE_OK) {
    VLOG_E("DdsFactory: Failed to take data.");
    return false;
  }
#else

  if (ret == ReturnCode_t::RETCODE_NO_DATA) {
    return false;
  }

  if VUNLIKELY (ret != ReturnCode_t::RETCODE_OK) {
    VLOG_E("DdsFactory: Failed to take data.");
    return false;
  }
#endif

  msg.id = msg.raw.id();

  msg.bytes.shallow_copy(msg.raw.data());

  msg.timestamp = msg.info.source_timestamp.to_ns();

  // std::memcpy(&msg.guid, msg.info.publication_handle.value + 8, sizeof(uint64_t));

  return true;
}

bool DdsFactory::take_cdr_data(dds::DataReader* reader, ReadCdrMessage& msg) {
  auto ret = reader->take_next_sample(msg.sample, &msg.info);

#ifdef VLINK_SUPPORT_DDS_V3

  if (ret == dds::RETCODE_NO_DATA) {
    return false;
  }

  if VUNLIKELY (ret != dds::RETCODE_OK) {
    VLOG_E("DdsFactory: Failed to take data.");
    return false;
  }
#else

  if (ret == ReturnCode_t::RETCODE_NO_DATA) {
    return false;
  }

  if VUNLIKELY (ret != ReturnCode_t::RETCODE_OK) {
    VLOG_E("DdsFactory: Failed to take data.");
    return false;
  }
#endif

  msg.bytes = Bytes::shallow_copy_ptr(msg.sample);

  msg.timestamp = msg.info.source_timestamp.to_ns();

  return true;
}

uint64_t DdsFactory::get_guid(const rtps::GUID_t& guid, uint32_t seq) {
  const auto& handle = static_cast<const rtps::InstanceHandle_t&>(guid);

  uint64_t result = static_cast<uint64_t>(handle.value[0] + handle.value[1] + handle.value[2] + handle.value[3]) << 24 |
                    static_cast<uint64_t>(handle.value[4] + handle.value[5] + handle.value[6] + handle.value[7]) << 16 |
                    static_cast<uint64_t>(handle.value[8] + handle.value[9] + handle.value[10] + handle.value[11])
                        << 8 |
                    static_cast<uint64_t>(handle.value[12] + handle.value[13] + handle.value[14] + handle.value[15]);

  return result << 32 | seq;
}

int DdsFactory::get_default_domain_id() {
  const std::string& domain_str = Utils::get_env("VLINK_DDS_DOMAIN");
  return Helpers::to_int(domain_str, 0);
}

void DdsFactory::set_participant_qos(dds::DomainParticipantQos& dds_qos, const Conf::PropertiesMap& properties) {
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
      dds_qos.properties().properties().emplace_back(prop, value);
    }
  }

  dds_qos.transport().use_builtin_transports = false;
  dds_qos.wire_protocol().ignore_non_matching_locators = true;
  dds_qos.wire_protocol().builtin.avoid_builtin_multicast = true;

  //   dds_qos.wire_protocol().port.domainIDGain = 250;
  //   dds_qos.wire_protocol().port.participantIDGain = 2;
  //   dds_qos.wire_protocol().port.portBase = 7400;
  //   dds_qos.wire_protocol().port.offsetd0 = 0;
  //   dds_qos.wire_protocol().port.offsetd1 = 10;
  //   dds_qos.wire_protocol().port.offsetd2 = 1;
  //   dds_qos.wire_protocol().port.offsetd3 = 11;
  // #ifdef VLINK_SUPPORT_DDS_V3
  //   dds_qos.wire_protocol().builtin.discovery_config.discoveryProtocol = rtps::DiscoveryProtocol::SIMPLE;
  // #else
  //   dds_qos.wire_protocol().builtin.discovery_config.discoveryProtocol = rtps::DiscoveryProtocol_t::SIMPLE;
  // #endif
  //   dds_qos.wire_protocol().builtin.discovery_config.use_SIMPLE_EndpointDiscoveryProtocol = true;
  //   dds_qos.wire_protocol().builtin.discovery_config.m_simpleEDP.use_PublicationReaderANDSubscriptionWriter = true;
  //   dds_qos.wire_protocol().builtin.discovery_config.m_simpleEDP.use_PublicationWriterANDSubscriptionReader = true;
  //   dds_qos.wire_protocol().builtin.discovery_config.leaseDuration = get_dds_duration(20000);
  //   dds_qos.wire_protocol().builtin.discovery_config.leaseDuration_announcementperiod = get_dds_duration(3000);
  //   dds_qos.wire_protocol().builtin.discovery_config.initial_announcements.count = 5;
  //   dds_qos.wire_protocol().builtin.discovery_config.initial_announcements.period = get_dds_duration(100);
  //   dds_qos.wire_protocol().builtin.discovery_config.ignoreParticipantFlags =
  //       static_cast<rtps::ParticipantFilteringFlags>(rtps::ParticipantFilteringFlags::FILTER_SAME_PROCESS);

  if (prop_enable_less_memory) {
    dds_qos.wire_protocol().builtin.readerHistoryMemoryPolicy = rtps::DYNAMIC_RESERVE_MEMORY_MODE;
    dds_qos.wire_protocol().builtin.writerHistoryMemoryPolicy = rtps::DYNAMIC_RESERVE_MEMORY_MODE;
    // dds_qos.allocation().participants = {2, 10, 1};
    // dds_qos.allocation().readers = {2, 20, 1};
    // dds_qos.allocation().writers = {2, 20, 1};
    // dds_qos.wire_protocol().builtin.readerPayloadSize = 512;
    // dds_qos.wire_protocol().builtin.writerPayloadSize = 512;
    // dds_qos.allocation().locators.max_unicast_locators = 4;
    // dds_qos.allocation().locators.max_multicast_locators = 1;
    // dds_qos.allocation().data_limits.max_user_data = 256;
    // dds_qos.allocation().data_limits.max_properties = 512;
    // dds_qos.allocation().data_limits.max_partitions = 256;
  }

#if !defined(__ANDROID__) && !defined(_WIN32)

  if (prop_enable_shm) {
    auto shm_descriptor = std::make_shared<rtps2::SharedMemTransportDescriptor>();
    dds_qos.transport().user_transports.emplace_back(std::move(shm_descriptor));
  }
#endif

  {
    rtps::Locator_t pdp_locator;
    pdp_locator.kind = LOCATOR_KIND_UDPv4;
    rtps::IPLocator::setIPv4(pdp_locator, "239.255.0.1");
    dds_qos.wire_protocol().builtin.metatrafficMulticastLocatorList.push_back(std::move(pdp_locator));
  }

  std::vector<std::string> ip_str_list;

  if (prop_ip_str.empty()) {
    ip_str_list = default_ip_list;
  } else {
    ip_str_list = Helpers::get_split_string(prop_ip_str, ';');
  }

  rtps::LocatorList_t ip_locators = get_locators(ip_str_list);

  if (!ip_locators.empty()) {
    if (!prop_enable_shm) {
      dds_qos.wire_protocol().default_unicast_locator_list.push_back(ip_locators);
    }

    dds_qos.wire_protocol().builtin.metatrafficUnicastLocatorList.push_back(ip_locators);
  }

  if (!prop_ip_multicast_str.empty()) {
    ip_str_list = Helpers::get_split_string(prop_ip_multicast_str, ';');
    rtps::LocatorList_t multicast_ip_locators = get_locators(ip_str_list);

    if (!multicast_ip_locators.empty()) {
      dds_qos.wire_protocol().default_multicast_locator_list.push_back(std::move(multicast_ip_locators));
    }
  }

  if (!prop_peer_str.empty()) {
    auto peer_str_list = Helpers::get_split_string(prop_peer_str, ';');
    rtps::LocatorList_t peer_locators = get_locators(peer_str_list);

    if (!peer_locators.empty()) {
      if (dds_qos.wire_protocol().builtin.metatrafficUnicastLocatorList.empty()) {
        dds_qos.wire_protocol().builtin.metatrafficUnicastLocatorList.push_back(rtps::LocatorList_t());
      }

      dds_qos.wire_protocol().builtin.initialPeersList.push_back(std::move(peer_locators));
    }
  }

  if (prop_enable_udp) {
    auto udp_descriptor = std::make_shared<rtps2::UDPv4TransportDescriptor>();

    if (ip_str_list.size() == 1 || !prop_ip_str.empty()) {
#ifdef VLINK_SUPPORT_DDS_V3
      udp_descriptor->interface_allowlist.reserve(ip_str_list.size());

      for (const auto& ip : ip_str_list) {
        // rtps::NetmaskFilterKind::OFF
        udp_descriptor->interface_allowlist.emplace_back(ip);
      }
#else
      udp_descriptor->interfaceWhiteList = ip_str_list;
#endif
    }

    if (prop_buf > 0) {
      udp_descriptor->sendBufferSize = static_cast<uint32_t>(prop_buf);
      udp_descriptor->receiveBufferSize = static_cast<uint32_t>(prop_buf);
    }

    if (prop_mtu > 0) {
      udp_descriptor->maxMessageSize = static_cast<uint32_t>(prop_mtu);
    }

    dds_qos.transport().user_transports.emplace_back(std::move(udp_descriptor));
  }

  auto ssl_cfg = SslOptions::parse_from(properties);

  bool ssl_cfg_valid = ssl_cfg.is_valid();

  if (ssl_cfg_valid && !prop_enable_tcp) {
    prop_enable_tcp = true;
  }

  if (prop_enable_tcp) {
    auto tcp_descriptor = std::make_shared<rtps2::TCPv4TransportDescriptor>();

    if (prop_buf > 0) {
      tcp_descriptor->sendBufferSize = static_cast<uint32_t>(prop_buf);
      tcp_descriptor->receiveBufferSize = static_cast<uint32_t>(prop_buf);
    }

    tcp_descriptor->keep_alive_frequency_ms = 1000;
    tcp_descriptor->keep_alive_timeout_ms = 3000;

    if (ssl_cfg_valid) {
      tcp_descriptor->apply_security = true;

      if (!ssl_cfg.ca_file.empty()) {
        tcp_descriptor->tls_config.verify_file = ssl_cfg.ca_file;
      }

      if (!ssl_cfg.cert_file.empty()) {
        tcp_descriptor->tls_config.cert_chain_file = ssl_cfg.cert_file;
      }

      if (!ssl_cfg.key_file.empty()) {
        tcp_descriptor->tls_config.private_key_file = ssl_cfg.key_file;
      }

      if (!ssl_cfg.key_password.empty()) {
        tcp_descriptor->tls_config.password = ssl_cfg.key_password;
      }

      if (!ssl_cfg.server_name.empty()) {
        tcp_descriptor->tls_config.server_name = ssl_cfg.server_name;
      }

      if (ssl_cfg.verify_peer) {
        tcp_descriptor->tls_config.add_verify_mode(
            rtps2::TCPTransportDescriptor::TLSConfig::TLSVerifyMode::VERIFY_PEER);
      } else {
        tcp_descriptor->tls_config.add_verify_mode(
            rtps2::TCPTransportDescriptor::TLSConfig::TLSVerifyMode::VERIFY_NONE);
      }

      tcp_descriptor->tls_config.add_option(rtps2::TCPTransportDescriptor::TLSConfig::TLSOptions::DEFAULT_WORKAROUNDS);
      tcp_descriptor->tls_config.add_option(rtps2::TCPTransportDescriptor::TLSConfig::TLSOptions::NO_SSLV2);
      tcp_descriptor->tls_config.add_option(rtps2::TCPTransportDescriptor::TLSConfig::TLSOptions::NO_SSLV3);
    }

    tcp_descriptor->add_listener_port(0);
    rtps::Locator_t tcp_locator;
    tcp_locator.kind = LOCATOR_KIND_TCPv4;
    rtps::IPLocator::setIPv4(tcp_locator, "0.0.0.0");
    rtps::IPLocator::setPhysicalPort(tcp_locator, 0);
    rtps::IPLocator::setLogicalPort(tcp_locator, 0);
    dds_qos.wire_protocol().builtin.metatrafficUnicastLocatorList.push_back(tcp_locator);
    dds_qos.wire_protocol().default_unicast_locator_list.push_back(tcp_locator);
    rtps::Locator_t pdp_locator;
    pdp_locator.kind = LOCATOR_KIND_UDPv4;
    rtps::IPLocator::setIPv4(pdp_locator, "239.255.0.1");
    dds_qos.wire_protocol().builtin.metatrafficMulticastLocatorList.push_back(std::move(pdp_locator));
    dds_qos.transport().user_transports.emplace_back(std::move(tcp_descriptor));
  }
}

rtps::LocatorList_t DdsFactory::get_locators(const std::vector<std::string>& list) {
  rtps::LocatorList_t locator_list;

  if (list.empty()) {
    return locator_list;
  }

  for (const auto& ip : list) {
    if (ip.find(':') != std::string::npos) {
      rtps2::Locator locator;
      locator.kind = LOCATOR_KIND_UDPv6;
      locator.port = 0;

      if VLIKELY (rtps::IPLocator::setIPv6(locator, ip)) {
        locator_list.push_back(std::move(locator));
      }
    } else {
      rtps2::Locator locator;
      locator.kind = LOCATOR_KIND_UDPv4;
      locator.port = 0;

      if VLIKELY (rtps::IPLocator::setIPv4(locator, ip)) {
        locator_list.push_back(std::move(locator));
      }
    }
  }

  return locator_list;
}

std::string DdsFactory::get_qos_ext(const Conf::PropertiesMap& ext, const std::string& key) {
  auto iter = ext.find(key);

  if (iter == ext.end()) {
    return "";
  }

  return iter->second;
}

}  // namespace vlink
