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

#include <cctype>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "./base/helpers.h"
#include "./base/utils.h"
#include "./ddsc_qos.hpp"
#include "./extension/qos_profile.h"
#include "./impl/ssl_options.h"

namespace vlink {

#if VLINK_DDSC_USE_CUSTOM_SERTYPE && defined(DDS_HAS_SHM)
static bool cyclone_xml_requests_shm(std::string_view xml) {
  std::string stripped;
  stripped.reserve(xml.size());

  for (size_t pos = 0U; pos < xml.size();) {
    if (xml.compare(pos, 4U, "<!--") == 0) {
      const size_t end = xml.find("-->", pos + 4U);

      if (end == std::string_view::npos) {
        return true;
      }

      pos = end + 3U;
      continue;
    }

    stripped.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(xml[pos++]))));
  }

  static constexpr std::string_view kShmTag = "sharedmemory";
  size_t search_pos = 0U;

  while ((search_pos = stripped.find("<sh", search_pos)) != std::string::npos) {
    size_t name_end = search_pos + 1U;

    while (name_end < stripped.size() && std::isalpha(static_cast<unsigned char>(stripped[name_end])) != 0) {
      ++name_end;
    }

    const std::string_view tag_name(stripped.data() + search_pos + 1U, name_end - (search_pos + 1U));

    if (tag_name.size() > kShmTag.size() || kShmTag.compare(0U, tag_name.size(), tag_name) != 0) {
      search_pos = name_end;
      continue;
    }

    const size_t open_end = stripped.find('>', name_end);

    if (open_end == std::string::npos) {
      return true;
    }

    size_t block_end = stripped.size();
    size_t next_pos = stripped.size();

    if (open_end > name_end && stripped[open_end - 1U] == '/') {
      block_end = open_end + 1U;
      next_pos = block_end;
    } else {
      std::string close_tag("</");
      close_tag.append(tag_name.data(), tag_name.size());
      const size_t close_pos = stripped.find(close_tag, open_end + 1U);

      if (close_pos != std::string::npos) {
        block_end = close_pos;
        next_pos = close_pos + close_tag.size();
      }
    }

    const std::string_view block(stripped.data() + search_pos, block_end - search_pos);
    const bool explicitly_disabled = block.find("<enable>false</enable>") != std::string_view::npos ||
                                     block.find("<enable>0</enable>") != std::string_view::npos ||
                                     block.find("enable=\"false\"") != std::string_view::npos ||
                                     block.find("enable='false'") != std::string_view::npos ||
                                     block.find("enable=\"0\"") != std::string_view::npos ||
                                     block.find("enable='0'") != std::string_view::npos;

    if (!explicitly_disabled) {
      return true;
    }

    search_pos = next_pos;
  }

  return false;
}

static bool cyclone_uri_requests_shm() {
  try {
    const std::string uri = Utils::get_env("VLINK_CYCLONEDDS_URI");

    if (uri.empty() || uri.find(',') != std::string::npos) {
      return true;
    }

    std::string_view trimmed(uri);

    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front())) != 0) {
      trimmed.remove_prefix(1U);
    }

    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back())) != 0) {
      trimmed.remove_suffix(1U);
    }

    if (trimmed.empty()) {
      return true;
    }

    if (trimmed.front() == '<') {
      return cyclone_xml_requests_shm(trimmed);
    }

    std::string candidate(trimmed);
    std::error_code ec;
    bool is_reg = std::filesystem::is_regular_file(candidate, ec);

    if ((ec || !is_reg) && Helpers::has_startwith(candidate, "file://")) {
      ec.clear();
      candidate = candidate.substr(7U);
      is_reg = std::filesystem::is_regular_file(candidate, ec);
    }

    if VUNLIKELY (ec || !is_reg) {
      return true;
    }

    std::ifstream ifs(candidate, std::ios::binary);

    if VUNLIKELY (!ifs.is_open()) {
      return true;
    }

    const std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return ifs.bad() || cyclone_xml_requests_shm(content);
  } catch (...) {
    return true;
  }
}
#endif

// DdscFactory
DdscFactory::DdscFactory() {
  Bytes::init_memory_pool();

  if VUNLIKELY (DdscTransport::get_thread_count() != 1) {
    VLOG_W("DdscFactory: Ddsc does not support setting thread count.");
  }

  {
    const auto& qos_profiles = QosProfile::get_available_qos_map();
    std::lock_guard lock(DdscTransport::mtx_);

    for (const auto& [name, qos] : qos_profiles) {
      DdscTransport::register_qos_internal(name, qos);
    }
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
    default_event_qos_ = DdscTransport::find_qos(default_event_qos_str);
  }

  if (default_method_qos_str.empty()) {
    default_method_qos_ = QosProfile::kMethod;
  } else {
    default_method_qos_ = DdscTransport::find_qos(default_method_qos_str);
  }

  if (default_field_qos_str.empty()) {
    default_field_qos_ = QosProfile::kField;
  } else {
    default_field_qos_ = DdscTransport::find_qos(default_field_qos_str);
  }
}

DdscFactory::~DdscFactory() = default;

std::shared_ptr<ddsc::DomainParticipant> DdscFactory::create_participant(uint8_t type, const DdscTransport& transport,
                                                                         const Transport::PropertiesMap& properties) {
  static auto& factory = DdscFactory::get();

  auto id = std::make_tuple(type, transport.domain, properties);
  std::lock_guard lifecycle_lock(factory.participant_mtx_);
  std::shared_ptr<ddsc::DomainParticipant> part;
  std::unique_ptr<ddsc::DomainParticipant> ptr;
  std::unique_lock lock(factory.mtx_);

  auto participant_iter = factory.part_map_.find(id);

  if (participant_iter != factory.part_map_.end()) {
    auto existing = participant_iter->second.lock();

    if (existing) {
      return existing;
    }

    factory.part_map_.erase(participant_iter);
  }

  std::unique_ptr<dds_qos_t, decltype(&dds_delete_qos)> dds_qos(dds_create_qos(), dds_delete_qos);

  if VUNLIKELY (!set_participant_qos(transport.domain, dds_qos.get(), properties)) {
    return nullptr;
  }

  lock.unlock();

  ptr = std::make_unique<ddsc::DomainParticipant>(transport.domain, dds_qos.get());

  if VUNLIKELY (ptr->entity <= 0) {
    decltype(factory.domain_map_)::node_type orphan_domain;
    {
      std::lock_guard cleanup_lock(factory.mtx_);
      auto domain_iter = factory.domain_map_.find(transport.domain);

      if (domain_iter != factory.domain_map_.end() && domain_iter->second.ref_count == 0) {
        orphan_domain = factory.domain_map_.extract(domain_iter);
      }
    }

    if (orphan_domain && orphan_domain.mapped().entity > 0) {
      dds_delete(orphan_domain.mapped().entity);
    }

    VLOG_E("DdscFactory: Failed to create participant.");
    return nullptr;
  }

  lock.lock();
  auto domain_iter = factory.domain_map_.find(transport.domain);
  const bool has_domain_ref = domain_iter != factory.domain_map_.end();

  auto deleter = [id, domain = transport.domain, has_domain_ref](ddsc::DomainParticipant* part) {
    std::lock_guard lifecycle_lock(factory.participant_mtx_);
    decltype(factory.domain_map_)::node_type domain_entry;

    {
      std::lock_guard lock(factory.mtx_);

      auto participant_iter = factory.part_map_.find(id);

      if (participant_iter != factory.part_map_.end() && participant_iter->second.expired()) {
        factory.part_map_.erase(participant_iter);
      }

      if (has_domain_ref) {
        auto iter = factory.domain_map_.find(domain);

        if VLIKELY (iter != factory.domain_map_.end() && iter->second.ref_count > 0) {
          --iter->second.ref_count;

          if (iter->second.ref_count == 0) {
            domain_entry = factory.domain_map_.extract(iter);
          }
        }
      }
    }

    delete part;

    if (domain_entry && domain_entry.mapped().entity > 0) {
      dds_delete(domain_entry.mapped().entity);
    }
  };

  Function<void(ddsc::DomainParticipant*), sizeof(deleter)> cleanup(std::move(deleter));

  if (has_domain_ref) {
    ++domain_iter->second.ref_count;
  }

  lock.unlock();
  part = std::shared_ptr<ddsc::DomainParticipant>(ptr.release(), std::move(cleanup));

  lock.lock();
  factory.part_map_.emplace(std::move(id), part);
  lock.unlock();

  return part;
}

std::shared_ptr<ddsc::Topic> DdscFactory::create_topic(uint8_t type, const DdscTransport& transport,
                                                       ddsc::DomainParticipant* part, std::string topic) {
  static auto& factory = DdscFactory::get();

  if (topic.empty()) {
    topic = transport.topic;
  }

  if VUNLIKELY (!part) {
    VLOG_E("DdscFactory: Cannot create topic without participant.");
    return nullptr;
  }

  auto id = std::make_tuple(type, transport.domain, topic, part);
  std::shared_ptr<ddsc::Topic> dds_topic;
  std::unique_lock lock(factory.mtx_);
  dds_topic = get_shared_ptr(factory.topic_map_, id);

  if (!dds_topic) {
    bool use_default_sertype = false;
#if VLINK_DDSC_USE_CUSTOM_SERTYPE && defined(DDS_HAS_SHM)
    auto domain_iter = factory.domain_map_.find(transport.domain);
    const bool has_domain = domain_iter != factory.domain_map_.end();

    if (has_domain) {
      use_default_sertype = domain_iter->second.shm_enabled;
    }
#endif

    lock.unlock();

#if VLINK_DDSC_USE_CUSTOM_SERTYPE && defined(DDS_HAS_SHM)
    if (!has_domain) {
      static const bool uri_requests_shm = cyclone_uri_requests_shm();
      use_default_sertype = uri_requests_shm;
    }
#endif

    auto cache_id = id;

    auto* ptr = new ddsc::Topic(part->entity, topic, "", use_default_sertype);

    if VUNLIKELY (!ptr || ptr->entity <= 0) {
      VLOG_E("DdscFactory: Failed to create topic: ", topic, ".");
      delete ptr;
      return nullptr;
    }

    dds_topic = std::shared_ptr<ddsc::Topic>(ptr, [id = std::move(cache_id)](ddsc::Topic* topic) {
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

    auto [iter, inserted] = factory.topic_map_.emplace(std::move(id), dds_topic);

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
    uint8_t type, const DdscTransport& transport, ddsc::DomainParticipant* part) {
  const std::string& resp_topic = transport.topic + DdscTransport::kRespSuffix;

  if VUNLIKELY (transport.topic.empty() || resp_topic.empty()) {
    VLOG_F("DdscFactory: Method transport topic error.");
  }

  if VUNLIKELY (transport.topic == resp_topic) {
    VLOG_F("DdscFactory: Method transport topic req and resp cannot be equal.");
  }

  return {create_topic(type, transport, part, transport.topic), create_topic(type, transport, part, resp_topic)};
}

std::shared_ptr<ddsc::Publisher> DdscFactory::create_publisher(uint8_t type, const DdscTransport& transport,
                                                               ddsc::DomainParticipant* part) {
  static auto& factory = DdscFactory::get();

  if VUNLIKELY (!part) {
    VLOG_E("DdscFactory: Cannot create publisher without participant.");
    return nullptr;
  }

  auto id = std::make_tuple(type, transport.domain, transport.qos, part);
  std::shared_ptr<ddsc::Publisher> publisher;
  std::unique_lock lock(factory.mtx_);
  publisher = get_shared_ptr(factory.publisher_map_, id);

  if (!publisher) {
    lock.unlock();

    auto cache_id = id;

    auto* ptr = new ddsc::Publisher(part->entity);

    if VUNLIKELY (!ptr || ptr->entity <= 0) {
      VLOG_E("DdscFactory: Failed to create publisher.");
      delete ptr;
      return nullptr;
    }

    publisher = std::shared_ptr<ddsc::Publisher>(ptr, [id = std::move(cache_id)](ddsc::Publisher* publisher) {
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

    auto [iter, inserted] = factory.publisher_map_.emplace(std::move(id), publisher);

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

std::shared_ptr<ddsc::Subscriber> DdscFactory::create_subscriber(uint8_t type, const DdscTransport& transport,
                                                                 ddsc::DomainParticipant* part) {
  static auto& factory = DdscFactory::get();

  if VUNLIKELY (!part) {
    VLOG_E("DdscFactory: Cannot create subscriber without participant.");
    return nullptr;
  }

  auto id = std::make_tuple(type, transport.domain, transport.qos, part);
  std::shared_ptr<ddsc::Subscriber> subscriber;
  std::unique_lock lock(factory.mtx_);
  subscriber = get_shared_ptr(factory.subscriber_map_, id);

  if (!subscriber) {
    lock.unlock();

    auto cache_id = id;

    auto* ptr = new ddsc::Subscriber(part->entity);

    if VUNLIKELY (!ptr || ptr->entity <= 0) {
      VLOG_E("DdscFactory: Failed to create subscriber.");
      delete ptr;
      return nullptr;
    }

    subscriber = std::shared_ptr<ddsc::Subscriber>(ptr, [id = std::move(cache_id)](ddsc::Subscriber* subscriber) {
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

    auto [iter, inserted] = factory.subscriber_map_.emplace(std::move(id), subscriber);

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

std::shared_ptr<ddsc::DataWriter> DdscFactory::create_datawriter(uint8_t type, const DdscTransport& transport,
                                                                 ddsc::Publisher* publisher, ddsc::Topic* topic,
                                                                 dds_listener_t* listener) {
  static auto& factory = DdscFactory::get();

  if VUNLIKELY (!publisher || !topic) {
    VLOG_E("DdscFactory: Cannot create datawriter without publisher/topic.");
    return nullptr;
  }

  dds_qos_t* dds_qos = dds_create_qos();

  if (transport.qos.empty()) {
    if ((type & kPublisher) || (type & kSubscriber)) {
      convert_qos(dds_qos, factory.default_event_qos_, transport.depth);
    } else if ((type & kClient) || (type & kServer)) {
      convert_qos(dds_qos, factory.default_method_qos_, transport.depth);
    } else if ((type & kSetter) || (type & kGetter)) {
      convert_qos(dds_qos, factory.default_field_qos_, transport.depth);
    }
  } else {
    convert_qos(dds_qos, DdscTransport::find_qos(transport.qos), transport.depth);
  }

  auto writer = std::make_shared<ddsc::DataWriter>(publisher->entity, topic->entity, dds_qos, listener);
  dds_delete_qos(dds_qos);

  if VUNLIKELY (!writer || writer->entity <= 0) {
    VLOG_E("DdscFactory: Failed to create datawriter.");
    return nullptr;
  }

  return writer;
}

std::shared_ptr<ddsc::DataReader> DdscFactory::create_datareader(uint8_t type, const DdscTransport& transport,
                                                                 ddsc::Subscriber* subscriber, ddsc::Topic* topic,
                                                                 dds_listener_t* listener) {
  static auto& factory = DdscFactory::get();

  if VUNLIKELY (!subscriber || !topic) {
    VLOG_E("DdscFactory: Cannot create datareader without subscriber/topic.");
    return nullptr;
  }

  dds_qos_t* dds_qos = dds_create_qos();

  if (transport.qos.empty()) {
    if ((type & kPublisher) || (type & kSubscriber)) {
      convert_qos(dds_qos, factory.default_event_qos_, transport.depth);
    } else if ((type & kClient) || (type & kServer)) {
      convert_qos(dds_qos, factory.default_method_qos_, transport.depth);
    } else if ((type & kSetter) || (type & kGetter)) {
      convert_qos(dds_qos, factory.default_field_qos_, transport.depth);
    }
  } else {
    convert_qos(dds_qos, DdscTransport::find_qos(transport.qos), transport.depth);
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
  static constexpr size_t kRawHeaderSize = 4U + sizeof(id) + sizeof(uint32_t);

  if VUNLIKELY (bytes.size() > std::numeric_limits<uint32_t>::max() - kRawHeaderSize) {
    return false;
  }

  vlink_dds__BuiltInRaw_ msg;

  msg.id = id;
  msg.data._buffer = const_cast<uint8_t*>(bytes.data());
  msg.data._length = bytes.size();
  msg.data._maximum = bytes.size();
  msg.data._release = false;

  auto ret = dds_write(entity, &msg);

  return ret >= 0;
}

bool DdscFactory::take_data(dds_entity_t entity, ReadMessage& msg) {
#if VLINK_DDSC_USE_CUSTOM_SERTYPE
  if VUNLIKELY (msg.current_serdata != nullptr) {
    ddsi_serdata_unref(msg.current_serdata);
    msg.current_serdata = nullptr;
  }

  if VUNLIKELY (msg.batch_count > 0U && msg.reader != entity) {
    for (uint32_t i = msg.batch_index; i < msg.batch_count; ++i) {
      if (msg.batch_serdatas[i] != nullptr) {
        ddsi_serdata_unref(msg.batch_serdatas[i]);
        msg.batch_serdatas[i] = nullptr;
      }
    }

    msg.batch_index = 0U;
    msg.batch_count = 0U;
  }

  if (msg.batch_index >= msg.batch_count) {
    msg.batch_index = 0U;
    msg.batch_count = 0U;
    msg.reader = entity;

    const auto ret = dds_takecdr(entity, msg.batch_serdatas, ReadMessage::kTakeBatchSize, msg.batch_infos,
                                 DDS_NOT_READ_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE);

    if (ret == 0 || ret == DDS_RETCODE_NO_DATA) {
      return false;
    }

    if VUNLIKELY (ret < 0) {
      VLOG_E("DdscFactory: Failed to take data.");
      return false;
    }

    msg.batch_count = static_cast<uint32_t>(ret);
  }

  const uint32_t idx = msg.batch_index++;
  msg.current_serdata = msg.batch_serdatas[idx];
  msg.batch_serdatas[idx] = nullptr;
  msg.info = msg.batch_infos[idx];

  if VLIKELY (msg.info.valid_data && msg.current_serdata != nullptr) {
    if VLIKELY (msg.current_serdata->ops == ddsc::built_in_raw_serdata_ops()) {
      const auto* sd = reinterpret_cast<const ddsc::BuiltInRawSerdata*>(msg.current_serdata);
      msg.id = sd->id;
      const uint8_t* payload = sd->payload_data();

      if (sd->payload_size > 0U && payload != nullptr) {
        msg.bytes = Bytes::shallow_copy(payload, sd->payload_size);
      } else {
        msg.bytes.clear();
      }
    } else {
      if VLIKELY (ddsi_serdata_to_sample(msg.current_serdata, &msg.fallback_sample, nullptr, nullptr)) {
        msg.id = msg.fallback_sample.id;
        msg.bytes = Bytes::shallow_copy(msg.fallback_sample.data._buffer, msg.fallback_sample.data._length);
      } else {
        msg.info.valid_data = false;
        msg.id = 0;
        msg.bytes.clear();
      }
    }
  } else {
    msg.info.valid_data = false;
    msg.id = 0;
    msg.bytes.clear();
  }

  msg.timestamp = msg.info.source_timestamp;
  msg.guid = msg.info.publication_handle;
  return true;
#elif DDS_VERSION_MAJOR >= 11
  if (msg.batch_index >= msg.batch_count || msg.reader != entity) {
    if (msg.batch_count > 0U) {
      (void)dds_return_loan(msg.reader, msg.batch_samples, static_cast<int32_t>(msg.batch_count));
      msg.batch_count = 0U;
      msg.batch_index = 0U;
      msg.sample = nullptr;
    }

    msg.reader = entity;
    std::memset(static_cast<void*>(msg.batch_samples), 0, sizeof(msg.batch_samples));

    const auto ret = dds_take_mask(entity, msg.batch_samples, msg.batch_infos, ReadMessage::kTakeBatchSize,
                                   ReadMessage::kTakeBatchSize,
                                   DDS_NOT_READ_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE);

    if (ret == 0 || ret == DDS_RETCODE_NO_DATA) {
      return false;
    }

    if VUNLIKELY (ret < 0) {
      VLOG_E("DdscFactory: Failed to take data.");
      return false;
    }

    msg.batch_count = static_cast<uint32_t>(ret);
  }

  const uint32_t idx = msg.batch_index++;
  msg.sample = msg.batch_samples[idx];
  msg.info = msg.batch_infos[idx];

  if VLIKELY (msg.info.valid_data && msg.sample != nullptr) {
    auto* sample = static_cast<vlink_dds__BuiltInRaw_*>(msg.sample);
    msg.id = sample->id;
    msg.bytes = Bytes::shallow_copy(sample->data._buffer, sample->data._length);
  } else {
    msg.id = 0;
    msg.bytes.clear();
  }

  msg.timestamp = msg.info.source_timestamp;
  msg.guid = msg.info.publication_handle;
  return true;
#else
  const auto ret = dds_take_next(entity, &msg.sample, &msg.info);

  if (ret == 0 || ret == DDS_RETCODE_NO_DATA) {
    return false;
  }

  if VUNLIKELY (ret < 0) {
    VLOG_E("DdscFactory: Failed to take data.");

    return false;
  }

  if VLIKELY (msg.info.valid_data) {
    auto* sample = static_cast<vlink_dds__BuiltInRaw_*>(msg.sample);
    msg.id = sample->id;
    msg.bytes = Bytes::shallow_copy(sample->data._buffer, sample->data._length);
  } else {
    msg.id = 0;
    msg.bytes.clear();
  }

  msg.timestamp = msg.info.source_timestamp;

  msg.guid = msg.info.publication_handle;

  return true;
#endif
}

bool DdscFactory::release_data(dds_entity_t entity, ReadMessage& msg) {
  msg.bytes.clear();

#if VLINK_DDSC_USE_CUSTOM_SERTYPE
  (void)entity;

  if (msg.current_serdata != nullptr) {
    ddsi_serdata_unref(msg.current_serdata);
    msg.current_serdata = nullptr;
  }

  return true;
#elif DDS_VERSION_MAJOR >= 11
  (void)entity;

  if (msg.batch_count > 0U && msg.batch_index >= msg.batch_count) {
    const auto ret = dds_return_loan(msg.reader, msg.batch_samples, static_cast<int32_t>(msg.batch_count));
    msg.batch_count = 0U;
    msg.batch_index = 0U;
    msg.sample = nullptr;
    return ret == DDS_RETCODE_OK;
  }

  return true;
#else
  (void)entity;
  return true;
#endif
}

uint64_t DdscFactory::get_guid(const dds_guid_t* guid, uint32_t seq) {
  uint64_t result = 14695981039346656037ULL;

  for (const auto value : guid->v) {
    result ^= static_cast<uint64_t>(value);
    result *= 1099511628211ULL;
  }

  for (size_t i = 0; i < sizeof(seq); ++i) {
    result ^= static_cast<uint64_t>((seq >> (i * 8)) & 0xFFU);
    result *= 1099511628211ULL;
  }

  return result;
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

bool DdscFactory::set_participant_qos(int32_t domain_id, dds_qos_t* dds_qos,
                                      const Transport::PropertiesMap& properties) {
  static auto& factory = DdscFactory::get();

  static const std::string& cyclone_dds_uri = process_cyclone_dds_uri();

  if (!cyclone_dds_uri.empty()) {
    return true;
  }

  static const std::string& discovery_ip_str = Utils::get_env("VLINK_DISCOVERY_IP");
  static const std::string& ip_str = Utils::get_env("VLINK_DDS_IP", discovery_ip_str);
  static const std::string& ip_multicast_str = Utils::get_env("VLINK_DDS_MULTICAST_IP");
  static const std::string& peer_str = Utils::get_env("VLINK_DDS_PEER");
  static const std::string& buf_str = Utils::get_env("VLINK_DDS_BUF");
  static const std::string& mtu_str = Utils::get_env("VLINK_DDS_MTU");

  static bool enable_udp = Helpers::to_int(Utils::get_env("VLINK_DDS_UDP"), 1) != 0;
  static bool enable_tcp = Helpers::to_int(Utils::get_env("VLINK_DDS_TCP"), 0) != 0;
  static bool enable_shm = Helpers::to_int(Utils::get_env("VLINK_DDS_SHM"), 0) != 0;
  static bool enable_noblock = Helpers::to_int(Utils::get_env("VLINK_DDS_NOBLOCK"), 0) != 0;

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
  [[maybe_unused]] bool prop_enable_noblock = enable_noblock;
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
    } else if (prop == "dds.noblock") {
      prop_enable_noblock = (value == "1");
    } else if (prop == "dds.less_memory") {
      prop_enable_less_memory = (value == "1");
    } else {
      dds_qset_prop(dds_qos, prop.c_str(), value.c_str());
    }
  }

  (void)prop_enable_noblock;
  (void)prop_enable_less_memory;

  auto ssl_cfg = SslOptions::parse_from(properties);

  const bool ssl_cfg_valid = ssl_cfg.is_valid();

#ifndef DDS_HAS_SSL
  if VUNLIKELY (ssl_cfg_valid) {
    VLOG_E("DdscFactory: ssl.* properties are set but CycloneDDS was built without DDS_HAS_SSL support.");
    return false;
  }
#endif

  auto [domain_iter, inserted] = factory.domain_map_.try_emplace(domain_id);

  if (!inserted) {
    if VUNLIKELY (ssl_cfg_valid && !domain_iter->second.ssl_enabled) {
      VLOG_E("DdscFactory: Domain ", domain_id, " is already initialized without TLS.");
      return false;
    }

    return true;
  }

  auto& domain_config = domain_iter->second;
  ddsi_config config;
  ddsi_config_init_default(&config);

  if (prop_enable_udp) {
    config.transport_selector = DDSI_TRANS_UDP;
  }

#ifdef DDS_HAS_SSL
  if (ssl_cfg_valid) {
    prop_enable_tcp = true;
    prop_enable_shm = false;
  }
#endif

  if (prop_enable_tcp) {
    config.transport_selector = DDSI_TRANS_TCP;
    config.tcp_port = 0;
    config.tcp_use_peeraddr_for_unicast = 1;
    config.compat_tcp_enable = DDSI_BOOLDEF_TRUE;
  }

#ifdef DDS_HAS_SSL
  if (ssl_cfg_valid && prop_enable_tcp) {
    config.ssl_enable = 1;
    domain_config.ssl_enabled = true;

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
  domain_config.shm_enabled = prop_enable_shm;

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
    domain_config.network_interface_list = Helpers::split_any(prop_ip_str);
  }

  if (!domain_config.network_interface_list.empty()) {
    domain_config.network_interface_elements =
        std::make_unique<ddsi_config_network_interface_listelem[]>(domain_config.network_interface_list.size());

    for (size_t i = 0; i < domain_config.network_interface_list.size(); ++i) {
      auto* interfaces = domain_config.network_interface_elements.get();

      if (i < domain_config.network_interface_list.size() - 1) {
        interfaces[i].next = &interfaces[i + 1];
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

  domain_config.peer_list = Helpers::split_any(prop_peer_str);

  if (!domain_config.peer_list.empty()) {
    domain_config.peer_elements = std::make_unique<ddsi_config_peer_listelem[]>(domain_config.peer_list.size());

    for (size_t i = 0; i < domain_config.peer_list.size(); ++i) {
      auto* peers = domain_config.peer_elements.get();

      if (i < domain_config.peer_list.size() - 1) {
        peers[i].next = &peers[i + 1];
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

    if VUNLIKELY (ssl_cfg_valid) {
      VLOG_E("DdscFactory: Failed to create TLS-enabled domain ", domain_id, ".");
      return false;
    }

    return true;
  }

  domain_config.entity = domain;
  return true;
}

}  // namespace vlink
