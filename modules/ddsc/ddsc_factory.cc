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

#include "./ddsc_factory.hpp"

#include <dds/ddsi/ddsi_config.h>

#include <charconv>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "./base/helpers.h"
#include "./base/utils.h"
#include "./ddsc_qos.hpp"
#include "./extension/qos_profile.h"
#include "./impl/ssl_options.h"

namespace vlink {

// DdscFactory
DdscFactory::DdscFactory() {
  Bytes::init_memory_pool();

  if VUNLIKELY (DdscConf::get_thread_count() != 1) {
    VLOG_W("DdscFactory: Ddsc does not support setting thread count.");
  }

  for (const auto& [name, qos] : QosProfile::get_available_qos_map()) {
    DdscConf::register_qos_internal(name, qos);
  }

  if (process_cyclone_dds_uri().empty()) {
    std::string dds_debug_str = Utils::get_env("VLINK_DDS_DEBUG");

    if (dds_debug_str == "1") {
      dds_set_log_mask(DDS_LC_ALL);
    } else {
      dds_set_log_mask(DDS_LC_FATAL);
    }
  }

  std::string default_event_qos_str = Utils::get_env("VLINK_DDS_EVENT_QOS");
  std::string default_method_qos_str = Utils::get_env("VLINK_DDS_METHOD_QOS");
  std::string default_field_qos_str = Utils::get_env("VLINK_DDS_FIELD_QOS");

  if (default_event_qos_str.empty()) {
    default_event_qos_ = QosProfile::kEvent;
  } else {
    default_event_qos_ = DdscConf::find_qos(default_event_qos_str);
  }

  if (default_method_qos_str.empty()) {
    default_method_qos_ = QosProfile::kMethod;
  } else {
    default_method_qos_ = DdscConf::find_qos(default_method_qos_str);
  }

  if (default_field_qos_str.empty()) {
    default_field_qos_ = QosProfile::kField;
  } else {
    default_field_qos_ = DdscConf::find_qos(default_field_qos_str);
  }
}

DdscFactory::~DdscFactory() = default;

std::shared_ptr<ddsc::DomainParticipant> DdscFactory::create_participant(uint8_t type, const DdscConf& conf,
                                                                         const Conf::PropertiesMap& properties) {
  static auto& factory = DdscFactory::get();

  const auto& id = std::make_tuple(type, conf.domain, properties);
  std::unique_lock lock(factory.mtx_);
  std::shared_ptr<ddsc::DomainParticipant> part = get_weak_ptr(factory.part_map_, id).lock();

  if (!part) {
    factory.part_map_.erase(id);

    dds_qos_t* dds_qos = dds_create_qos();

    set_participant_qos(conf.domain, dds_qos, properties);

    bool has_domain_ref = false;
    auto domain_iter = factory.domain_map_.find(conf.domain);

    if (domain_iter != factory.domain_map_.end()) {
      ++domain_iter->second.ref_count;
      has_domain_ref = true;
    }

    auto* ptr = new ddsc::DomainParticipant(conf.domain, dds_qos);

    if VUNLIKELY (!ptr || ptr->entity <= 0) {
      VLOG_E("DdscFactory: Failed to create participant.");
      delete ptr;
      dds_delete_qos(dds_qos);

      if (has_domain_ref) {
        auto iter = factory.domain_map_.find(conf.domain);
        if VLIKELY (iter != factory.domain_map_.end() && iter->second.ref_count > 0) {
          --iter->second.ref_count;
        }
      }

      return nullptr;
    }

    part = std::shared_ptr<ddsc::DomainParticipant>(
        ptr, [id, domain = conf.domain, has_domain_ref](ddsc::DomainParticipant* part) {
          {
            std::lock_guard lock(factory.mtx_);

            if (auto iter = factory.part_map_.find(id); iter != factory.part_map_.end() && iter->second.expired()) {
              factory.part_map_.erase(iter);
            }

            delete part;

            if (!has_domain_ref) {
              return;
            }

            auto iter = factory.domain_map_.find(domain);

            if VLIKELY (iter != factory.domain_map_.end() && iter->second.ref_count > 0) {
              --iter->second.ref_count;

              if (iter->second.ref_count == 0) {
                dds_delete(iter->second.entity);
                factory.domain_map_.erase(iter);
              }
            }
          }
        });

    factory.part_map_.emplace(id, part);

    dds_delete_qos(dds_qos);
  }

  return part;
}

std::shared_ptr<ddsc::Topic> DdscFactory::create_topic(uint8_t type, const DdscConf& conf,
                                                       ddsc::DomainParticipant* part, std::string topic) {
  static auto& factory = DdscFactory::get();

  if (topic.empty()) {
    topic = conf.topic;
  }

  if VUNLIKELY (!part) {
    VLOG_E("DdscFactory: Cannot create topic without participant.");
    return nullptr;
  }

  const auto& id = std::make_tuple(type, conf.domain, topic, part);
  std::unique_lock lock(factory.mtx_);
  std::shared_ptr<ddsc::Topic> dds_topic = get_weak_ptr(factory.topic_map_, id).lock();

  if (!dds_topic) {
    lock.unlock();
    auto* ptr = new ddsc::Topic(part->entity, topic);

    if VUNLIKELY (!ptr || ptr->entity <= 0) {
      VLOG_E("DdscFactory: Failed to create topic: ", topic, ".");
      delete ptr;
      return nullptr;
    }

    dds_topic = std::shared_ptr<ddsc::Topic>(ptr, [id](ddsc::Topic* topic) {
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

std::pair<std::shared_ptr<ddsc::Topic>, std::shared_ptr<ddsc::Topic> > DdscFactory::create_method_topic(
    uint8_t type, const DdscConf& conf, ddsc::DomainParticipant* part) {
  const std::string& resp_topic = conf.topic + DdscConf::kRespSuffix;

  if VUNLIKELY (conf.topic.empty() || resp_topic.empty()) {
    VLOG_F("DdscFactory: Method conf topic error.");
  }

  if VUNLIKELY (conf.topic == resp_topic) {
    VLOG_F("DdscFactory: Method conf topic req and resp cannot be equal.");
  }

  return {create_topic(type, conf, part, conf.topic), create_topic(type, conf, part, resp_topic)};
}

std::shared_ptr<ddsc::Publisher> DdscFactory::create_publisher(uint8_t type, const DdscConf& conf,
                                                               ddsc::DomainParticipant* part) {
  static auto& factory = DdscFactory::get();

  if VUNLIKELY (!part) {
    VLOG_E("DdscFactory: Cannot create publisher without participant.");
    return nullptr;
  }

  const auto& id = std::make_tuple(type, conf.domain, conf.qos, part);
  std::unique_lock lock(factory.mtx_);
  std::shared_ptr<ddsc::Publisher> publisher = get_weak_ptr(factory.publisher_map_, id).lock();

  if (!publisher) {
    lock.unlock();

    auto* ptr = new ddsc::Publisher(part->entity);

    if VUNLIKELY (!ptr || ptr->entity <= 0) {
      VLOG_E("DdscFactory: Failed to create publisher.");
      delete ptr;
      return nullptr;
    }

    publisher = std::shared_ptr<ddsc::Publisher>(ptr, [id](ddsc::Publisher* publisher) {
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

std::shared_ptr<ddsc::Subscriber> DdscFactory::create_subscriber(uint8_t type, const DdscConf& conf,
                                                                 ddsc::DomainParticipant* part) {
  static auto& factory = DdscFactory::get();

  if VUNLIKELY (!part) {
    VLOG_E("DdscFactory: Cannot create subscriber without participant.");
    return nullptr;
  }

  const auto& id = std::make_tuple(type, conf.domain, conf.qos, part);
  std::unique_lock lock(factory.mtx_);
  std::shared_ptr<ddsc::Subscriber> subscriber = get_weak_ptr(factory.subscriber_map_, id).lock();

  if (!subscriber) {
    lock.unlock();

    auto* ptr = new ddsc::Subscriber(part->entity);

    if VUNLIKELY (!ptr || ptr->entity <= 0) {
      VLOG_E("DdscFactory: Failed to create subscriber.");
      delete ptr;
      return nullptr;
    }

    subscriber = std::shared_ptr<ddsc::Subscriber>(ptr, [id](ddsc::Subscriber* subscriber) {
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

std::shared_ptr<ddsc::DataWriter> DdscFactory::create_datawriter(uint8_t type, const DdscConf& conf,
                                                                 ddsc::Publisher* publisher, ddsc::Topic* topic,
                                                                 dds_listener_t* listener) {
  static auto& factory = DdscFactory::get();

  if VUNLIKELY (!publisher || !topic) {
    VLOG_E("DdscFactory: Cannot create datawriter without publisher/topic.");
    return nullptr;
  }

  dds_qos_t* dds_qos = dds_create_qos();

  if (conf.qos.empty()) {
    if ((type & kPublisher) || (type & kSubscriber)) {
      convert_qos(dds_qos, factory.default_event_qos_, conf.depth);
    } else if ((type & kClient) || (type & kServer)) {
      convert_qos(dds_qos, factory.default_method_qos_, conf.depth);
    } else if ((type & kSetter) || (type & kGetter)) {
      convert_qos(dds_qos, factory.default_field_qos_, conf.depth);
    }
  } else {
    convert_qos(dds_qos, DdscConf::find_qos(conf.qos), conf.depth);
  }

  auto writer = std::make_shared<ddsc::DataWriter>(publisher->entity, topic->entity, dds_qos, listener);
  dds_delete_qos(dds_qos);

  if VUNLIKELY (!writer || writer->entity <= 0) {
    VLOG_E("DdscFactory: Failed to create datawriter.");
    return nullptr;
  }

  return writer;
}

std::shared_ptr<ddsc::DataReader> DdscFactory::create_datareader(uint8_t type, const DdscConf& conf,
                                                                 ddsc::Subscriber* subscriber, ddsc::Topic* topic,
                                                                 dds_listener_t* listener) {
  static auto& factory = DdscFactory::get();

  if VUNLIKELY (!subscriber || !topic) {
    VLOG_E("DdscFactory: Cannot create datareader without subscriber/topic.");
    return nullptr;
  }

  dds_qos_t* dds_qos = dds_create_qos();

  if (conf.qos.empty()) {
    if ((type & kPublisher) || (type & kSubscriber)) {
      convert_qos(dds_qos, factory.default_event_qos_, conf.depth);
    } else if ((type & kClient) || (type & kServer)) {
      convert_qos(dds_qos, factory.default_method_qos_, conf.depth);
    } else if ((type & kSetter) || (type & kGetter)) {
      convert_qos(dds_qos, factory.default_field_qos_, conf.depth);
    }
  } else {
    convert_qos(dds_qos, DdscConf::find_qos(conf.qos), conf.depth);
  }

  auto reader = std::make_shared<ddsc::DataReader>(subscriber->entity, topic->entity, dds_qos, listener);
  dds_delete_qos(dds_qos);

  if VUNLIKELY (!reader || reader->entity <= 0) {
    VLOG_E("DdscFactory: Failed to create datareader.");
    return nullptr;
  }

  return reader;
}

bool DdscFactory::write_data(dds_entity_t entity, const Bytes& bytes, uint64_t id) {
  vlink_BuiltInRaw msg;

  msg.id = id;
  msg.data._buffer = const_cast<uint8_t*>(bytes.data());
  msg.data._length = bytes.size();
  msg.data._maximum = bytes.size();
  msg.data._release = false;

  auto ret = dds_write(entity, &msg);

  return ret >= 0;
}

bool DdscFactory::take_data(dds_entity_t entity, ReadMessage& msg) {
  auto ret = dds_take_next(entity, &msg.sample, &msg.info);

  if (ret == 0) {
    return false;
  }

  if (ret == DDS_RETCODE_NO_DATA) {
    return false;
  }

  if VUNLIKELY (ret < 0) {
    VLOG_E("DdscFactory: Failed to take data.");

    return false;
  }

  auto* sample = static_cast<vlink_BuiltInRaw*>(msg.sample);

  msg.id = sample->id;

  msg.bytes = Bytes::shallow_copy(sample->data._buffer, sample->data._length);

  msg.timestamp = msg.info.source_timestamp;

  msg.guid = msg.info.publication_handle;

  return true;
}

bool DdscFactory::release_data(dds_entity_t entity, ReadMessage& msg) {
  auto ret = dds_return_loan(entity, &msg.sample, 1);

  msg.sample = nullptr;

  return ret == DDS_RETCODE_OK;
}

uint64_t DdscFactory::get_guid(const dds_guid_t* guid, uint32_t seq) {
  uint64_t result = 0;

  std::memcpy(&result, guid->v, sizeof(uint64_t));

  return result << 32 | seq;
}

int DdscFactory::get_default_domain_id() {
  const std::string& domain_str = Utils::get_env("VLINK_DDS_DOMAIN");
  return Helpers::to_int(domain_str, 0);
}

std::string DdscFactory::process_cyclone_dds_uri() {
  std::string cyclone_dds_uri = Utils::get_env("VLINK_CYCLONEDDS_URI");

  if (!cyclone_dds_uri.empty()) {
    Utils::set_env("CYCLONEDDS_URI", cyclone_dds_uri);
  }

  return cyclone_dds_uri;
}

void DdscFactory::set_participant_qos(int32_t domain_id, dds_qos_t* dds_qos, const Conf::PropertiesMap& properties) {
  static auto& factory = DdscFactory::get();

  static const std::string& cyclone_dds_uri = process_cyclone_dds_uri();

  if (!cyclone_dds_uri.empty()) {
    return;
  }

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
      dds_qset_prop(dds_qos, prop.c_str(), value.c_str());
    }
  }

  (void)prop_enable_less_memory;

  if (factory.domain_map_.find(domain_id) != factory.domain_map_.end()) {
    return;
  }

  ddsi_config config;
  ddsi_config_init_default(&config);

  auto [domain_iter, inserted] = factory.domain_map_.try_emplace(domain_id);

  if VUNLIKELY (!inserted) {
    return;
  }

  auto& domain_config = domain_iter->second;

  if (prop_enable_udp) {
    config.transport_selector = DDSI_TRANS_UDP;
  }

  auto ssl_cfg = SslOptions::parse_from(properties);

  bool ssl_cfg_valid = ssl_cfg.is_valid();

  if (ssl_cfg_valid && !prop_enable_tcp) {
    prop_enable_tcp = true;
  }

  if (prop_enable_tcp) {
    config.transport_selector = DDSI_TRANS_TCP;
    config.tcp_port = 0;
    config.tcp_use_peeraddr_for_unicast = 1;
    config.compat_tcp_enable = DDSI_BOOLDEF_TRUE;
  }

#ifdef DDS_HAS_SSL

  if (ssl_cfg_valid && prop_enable_tcp) {
    config.ssl_enable = 1;

    if (!ssl_cfg.cert_file.empty()) {
      domain_config.ssl_keystore = ssl_cfg.cert_file;
    } else if (!ssl_cfg.key_file.empty()) {
      domain_config.ssl_keystore = ssl_cfg.key_file;
    } else if (!ssl_cfg.ca_file.empty()) {
      domain_config.ssl_keystore = ssl_cfg.ca_file;
    }

    if (!domain_config.ssl_keystore.empty()) {
      config.ssl_keystore = const_cast<char*>(domain_config.ssl_keystore.c_str());
    }

    if (!ssl_cfg.key_password.empty()) {
      domain_config.ssl_key_pass = ssl_cfg.key_password;
      config.ssl_key_pass = const_cast<char*>(domain_config.ssl_key_pass.c_str());
    }

    int provided =
        (ssl_cfg.cert_file.empty() ? 0 : 1) + (ssl_cfg.key_file.empty() ? 0 : 1) + (ssl_cfg.ca_file.empty() ? 0 : 1);

    if VUNLIKELY (provided > 1) {
      VLOG_W(
          "DdscFactory: CycloneDDS only supports a single ssl_keystore (PEM/PKCS#12 with private key, certificate "
          "and CA chain combined); ssl.cert/ssl.key/ssl.ca cannot be specified separately. Picked one and ignored "
          "the others.");
    }

    config.ssl_verify = ssl_cfg.verify_peer ? 1 : 0;
    config.ssl_self_signed = ssl_cfg.verify_peer ? 0 : 1;

    if (!ssl_cfg.ciphers.empty()) {
      domain_config.ssl_ciphers = ssl_cfg.ciphers;
      config.ssl_ciphers = const_cast<char*>(domain_config.ssl_ciphers.c_str());
    }
  }
#else

  if (ssl_cfg_valid) {
    VLOG_W("DdscFactory: ssl.* properties are set but CycloneDDS was built without DDS_HAS_SSL support.");
  }
#endif

  if (prop_buf > 0) {
    config.socket_sndbuf_size.min.isdefault = 1;
    config.socket_sndbuf_size.max.isdefault = 0;
    config.socket_sndbuf_size.max.value = static_cast<uint32_t>(prop_buf);

    config.socket_rcvbuf_size.min.isdefault = 1;
    config.socket_rcvbuf_size.max.isdefault = 0;
    config.socket_rcvbuf_size.max.value = static_cast<uint32_t>(prop_buf);
  }

  if (prop_mtu > 0) {
    if (prop_mtu < config.fragment_size) {
      config.fragment_size = static_cast<uint32_t>(prop_mtu);
    }

    if (prop_mtu < config.max_rexmit_msg_size) {
      config.max_rexmit_msg_size = static_cast<uint32_t>(prop_mtu);
    }

    config.max_msg_size = static_cast<uint32_t>(prop_mtu);
  }

#ifdef DDS_HAS_SHM

  if (prop_enable_shm) {
    config.enable_shm = 1;
  } else {
    config.enable_shm = 0;
  }
#endif

  if (prop_ip_multicast_str.empty()) {
    config.allowMulticast = DDSI_AMC_SPDP;
  } else {
    config.allowMulticast = DDSI_AMC_TRUE;
  }

  if (prop_ip_str.empty()) {
    domain_config.network_interface_list = default_ip_list;
  } else {
    domain_config.network_interface_list = Helpers::get_split_string(prop_ip_str, ';');
  }

  if (!domain_config.network_interface_list.empty()) {
    domain_config.network_interface_elements =
        std::make_unique<ddsi_config_network_interface_listelem[]>(domain_config.network_interface_list.size());

    for (size_t i = 0; i < domain_config.network_interface_list.size(); ++i) {
      auto* interfaces = domain_config.network_interface_elements.get();

      if (i < domain_config.network_interface_list.size() - 1) {
        interfaces[i].next = &(interfaces[i + 1]);
      } else {
        interfaces[i].next = nullptr;
      }

      interfaces[i].cfg.automatic = 0;
      interfaces[i].cfg.name = nullptr;
      interfaces[i].cfg.address = const_cast<char*>(domain_config.network_interface_list.at(i).c_str());
      interfaces[i].cfg.prefer_multicast = 0;
      interfaces[i].cfg.presence_required = 1;
      interfaces[i].cfg.priority.isdefault = 1;
      interfaces[i].cfg.multicast = DDSI_BOOLDEF_TRUE;
    }

    config.network_interfaces = domain_config.network_interface_elements.get();
  }

  domain_config.peer_list = Helpers::get_split_string(prop_peer_str, ';');

  if (!domain_config.peer_list.empty()) {
    domain_config.peer_elements = std::make_unique<ddsi_config_peer_listelem[]>(domain_config.peer_list.size());

    for (size_t i = 0; i < domain_config.peer_list.size(); ++i) {
      auto* peers = domain_config.peer_elements.get();

      if (i < domain_config.peer_list.size() - 1) {
        peers[i].next = &(peers[i + 1]);
      } else {
        peers[i].next = nullptr;
      }

      peers[i].peer = const_cast<char*>(domain_config.peer_list.at(i).c_str());
    }

    config.peers = domain_config.peer_elements.get();
  }

  auto domain = dds_create_domain_with_rawconfig(domain_id, &config);

  if VUNLIKELY (domain <= 0) {
    factory.domain_map_.erase(domain_iter);
    return;
  }

  domain_config.entity = domain;
}

}  // namespace vlink
