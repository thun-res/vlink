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

#include <string>
#include <utility>

extern "C" {
#if RTI_DDS_VERSION_MAJOR >= 6
#include "builtin/7_x/BuiltInRawSupport.h"
#else
#include "builtin/5_x/BuiltInRawSupport.h"
#endif
}

namespace ddsr {

// DomainParticipant
struct DomainParticipant {
  explicit DomainParticipant(int32_t domain, const DDS_DomainParticipantQos* qos, const std::string& qos_profile = "");

  ~DomainParticipant();

  DDS_DomainParticipant* entity{nullptr};
};

// Topic
struct Topic {
  explicit Topic(DDS_DomainParticipant* _part, const std::string& topic, const std::string& type_name,
                 const DDS_TopicQos* qos, const std::string& qos_profile = "");

  ~Topic();

  DDS_DomainParticipant* part{nullptr};
  DDS_Topic* entity{nullptr};
};

// Publisher
struct Publisher {
  explicit Publisher(DDS_DomainParticipant* _part, struct DDS_PublisherQos* qos, const std::string& qos_profile = "");

  ~Publisher();

  DDS_DomainParticipant* part{nullptr};
  DDS_Publisher* entity{nullptr};
};

// Subscriber
struct Subscriber {
  explicit Subscriber(DDS_DomainParticipant* _part, struct DDS_SubscriberQos* qos, const std::string& qos_profile = "");

  ~Subscriber();

  DDS_DomainParticipant* part{nullptr};
  DDS_Subscriber* entity{nullptr};
};

// DataWriter
struct DataWriter {
  explicit DataWriter(DDS_Publisher* _pub, DDS_Topic* topic, const DDS_DataWriterListener* listener,
                      const DDS_DataWriterQos* qos, const std::string& qos_profile = "");

  ~DataWriter();

  DDS_Publisher* pub{nullptr};
  DDS_DataWriter* entity{nullptr};
};

// DataReader
struct DataReader {
  explicit DataReader(DDS_Subscriber* _sub, DDS_Topic* topic, const DDS_DataReaderListener* listener,
                      const DDS_DataReaderQos* qos, const std::string& qos_profile = "");

  ~DataReader();

  DDS_Subscriber* sub{nullptr};
  DDS_DataReader* entity{nullptr};
};

}  // namespace ddsr
