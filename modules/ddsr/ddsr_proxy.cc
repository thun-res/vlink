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

#include "./ddsr_proxy.hpp"

#include <string>

#include "./base/logger.h"
#include "./ddsr_factory.hpp"

extern "C" {
#if RTI_DDS_VERSION_MAJOR >= 6
#include "builtin/7_x/BuiltInRaw.h"
#else
#include "builtin/5_x/BuiltInRaw.h"
#endif
}

namespace ddsr {

// DomainParticipant
DomainParticipant::DomainParticipant(int32_t domain, const DDS_DomainParticipantQos* qos,
                                     const std::string& qos_profile) {
  const auto* target_qos = qos ? qos : &DDS_PARTICIPANT_QOS_DEFAULT;

  DDS_StatusMask mask = DDS_STATUS_MASK_NONE;

  if (qos_profile.empty()) {
    entity = DDS_DomainParticipantFactory_create_participant(vlink::DdsrFactory::get_dds_factory(), domain, target_qos,
                                                             nullptr, mask);
  } else {
    entity = DDS_DomainParticipantFactory_create_participant_with_profile(
        vlink::DdsrFactory::get_dds_factory(), domain, DDS_BUILTIN_QOS_LIB, qos_profile.c_str(), nullptr, mask);
  }

  if VUNLIKELY (!entity) {
    VLOG_E("DDS_DomainParticipantFactory_create_participant failed.");
  } else {
    vlink_BuiltInRawTypeSupport_register_type(entity, vlink_BuiltInRawTypeSupport_get_type_name());
  }
}

DomainParticipant::~DomainParticipant() {
  if VUNLIKELY (!entity) {
    return;
  }

  auto ret = DDS_DomainParticipantFactory_delete_participant(vlink::DdsrFactory::get_dds_factory(), entity);

  entity = nullptr;

  if VUNLIKELY (ret != DDS_RETCODE_OK) {
    VLOG_E("DDS_DomainParticipantFactory_delete_participant failed.");
  }
}

// Topic
Topic::Topic(DDS_DomainParticipant* _part, const std::string& topic, const std::string& type_name,
             const DDS_TopicQos* qos, const std::string& qos_profile) {
  const auto* target_qos = qos ? qos : &DDS_TOPIC_QOS_DEFAULT;

  part = _part;

  DDS_StatusMask mask = DDS_STATUS_MASK_NONE;

  if (qos_profile.empty()) {
    entity = DDS_DomainParticipant_create_topic(part, topic.c_str(), type_name.c_str(), target_qos, nullptr, mask);
  } else {
    entity = DDS_DomainParticipant_create_topic_with_profile(part, topic.c_str(), type_name.c_str(),
                                                             DDS_BUILTIN_QOS_LIB, qos_profile.c_str(), nullptr, mask);
  }

  if VUNLIKELY (!entity) {
    VLOG_E("DDS_DomainParticipant_create_topic failed.");
  }
}

Topic::~Topic() {
  if VUNLIKELY (!entity || !part) {
    return;
  }

  auto ret = DDS_DomainParticipant_delete_topic(part, entity);

  part = nullptr;
  entity = nullptr;

  if VUNLIKELY (ret != DDS_RETCODE_OK) {
    VLOG_E("DDS_DomainParticipant_delete_topic failed.");
  }
}

// Publisher
Publisher::Publisher(DDS_DomainParticipant* _part, struct DDS_PublisherQos* qos, const std::string& qos_profile) {
  const auto* target_qos = qos ? qos : &DDS_PUBLISHER_QOS_DEFAULT;

  part = _part;

  DDS_StatusMask mask = DDS_STATUS_MASK_NONE;

  if (qos_profile.empty()) {
    entity = DDS_DomainParticipant_create_publisher(part, target_qos, nullptr, mask);
  } else {
    entity = DDS_DomainParticipant_create_publisher_with_profile(part, DDS_BUILTIN_QOS_LIB, qos_profile.c_str(),
                                                                 nullptr, mask);
  }

  if VUNLIKELY (!entity) {
    VLOG_E("DDS_DomainParticipant_create_publisher failed.");
  }
}

Publisher::~Publisher() {
  if VUNLIKELY (!entity || !part) {
    return;
  }

  auto ret = DDS_DomainParticipant_delete_publisher(part, entity);

  part = nullptr;
  entity = nullptr;

  if VUNLIKELY (ret != DDS_RETCODE_OK) {
    VLOG_E("DDS_DomainParticipant_delete_publisher failed.");
  }
}

// Subscriber
Subscriber::Subscriber(DDS_DomainParticipant* _part, struct DDS_SubscriberQos* qos, const std::string& qos_profile) {
  const auto* target_qos = qos ? qos : &DDS_SUBSCRIBER_QOS_DEFAULT;

  part = _part;

  DDS_StatusMask mask = DDS_STATUS_MASK_NONE;

  if (qos_profile.empty()) {
    entity = DDS_DomainParticipant_create_subscriber(part, target_qos, nullptr, mask);
  } else {
    entity = DDS_DomainParticipant_create_subscriber_with_profile(part, DDS_BUILTIN_QOS_LIB, qos_profile.c_str(),
                                                                  nullptr, mask);
  }

  if VUNLIKELY (!entity) {
    VLOG_E("DDS_DomainParticipant_create_subscriber failed.");
  }
}

Subscriber::~Subscriber() {
  if VUNLIKELY (!entity || !part) {
    return;
  }

  auto ret = DDS_DomainParticipant_delete_subscriber(part, entity);

  part = nullptr;
  entity = nullptr;

  if VUNLIKELY (ret != DDS_RETCODE_OK) {
    VLOG_E("DDS_DomainParticipant_delete_subscriber failed.");
  }
}

// DataWriter
DataWriter::DataWriter(DDS_Publisher* _pub, DDS_Topic* topic, const DDS_DataWriterListener* listener,
                       const DDS_DataWriterQos* qos, const std::string& qos_profile) {
  const auto* target_qos = qos ? qos : &DDS_DATAWRITER_QOS_DEFAULT;

  pub = _pub;

  DDS_StatusMask mask = DDS_PUBLICATION_MATCHED_STATUS | DDS_OFFERED_DEADLINE_MISSED_STATUS |
                        DDS_OFFERED_INCOMPATIBLE_QOS_STATUS | DDS_LIVELINESS_LOST_STATUS;

  if (qos_profile.empty()) {
    entity = DDS_Publisher_create_datawriter(pub, topic, target_qos, listener, mask);
  } else {
    entity = DDS_Publisher_create_datawriter_with_profile(pub, topic, DDS_BUILTIN_QOS_LIB, qos_profile.c_str(),
                                                          listener, mask);
  }

  if VUNLIKELY (!entity) {
    VLOG_E("DDS_Publisher_create_datawriter failed.");
  }
}

DataWriter::~DataWriter() {
  if VUNLIKELY (!entity || !pub) {
    return;
  }

  auto ret = DDS_Publisher_delete_datawriter(pub, entity);

  pub = nullptr;
  entity = nullptr;

  if VUNLIKELY (ret != DDS_RETCODE_OK) {
    VLOG_E("DDS_Publisher_delete_datawriter failed.");
  }
}

// DataReader
DataReader::DataReader(DDS_Subscriber* _sub, DDS_Topic* topic, const DDS_DataReaderListener* listener,
                       const DDS_DataReaderQos* qos, const std::string& qos_profile) {
  const auto* target_qos = qos ? qos : &DDS_DATAREADER_QOS_DEFAULT;

  sub = _sub;

  DDS_StatusMask mask = DDS_SUBSCRIPTION_MATCHED_STATUS | DDS_REQUESTED_DEADLINE_MISSED_STATUS |
                        DDS_LIVELINESS_CHANGED_STATUS | DDS_SAMPLE_REJECTED_STATUS |
                        DDS_REQUESTED_INCOMPATIBLE_QOS_STATUS | DDS_SAMPLE_LOST_STATUS | DDS_DATA_AVAILABLE_STATUS;

  if (qos_profile.empty()) {
    entity = DDS_Subscriber_create_datareader(sub, topic->_as_TopicDescription, target_qos, listener, mask);  // BUG?
  } else {
    entity = DDS_Subscriber_create_datareader_with_profile(sub, topic->_as_TopicDescription, DDS_BUILTIN_QOS_LIB,
                                                           qos_profile.c_str(), listener, mask);  // BUG?
  }

  if VUNLIKELY (!entity) {
    VLOG_E("DDS_Subscriber_create_datareader failed.");
  }
}

DataReader::~DataReader() {
  if VUNLIKELY (!entity || !sub) {
    return;
  }

  auto ret = DDS_Subscriber_delete_datareader(sub, entity);

  sub = nullptr;
  entity = nullptr;

  if VUNLIKELY (ret != DDS_RETCODE_OK) {
    VLOG_E("DDS_Subscriber_delete_datareader failed.");
  }
}

}  // namespace ddsr
