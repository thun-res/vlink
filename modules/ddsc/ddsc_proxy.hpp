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

#include <dds/dds.h>

#include <string>
#include <utility>

namespace ddsc {

// DomainParticipant
struct DomainParticipant {
  explicit DomainParticipant(int32_t domain, const dds_qos_t* qos = nullptr);

  ~DomainParticipant();

  dds_entity_t entity{0};
};

// Topic
struct Topic {
  explicit Topic(dds_entity_t part, const std::string& topic, const std::string& type_name = "");

  ~Topic();

  dds_entity_t entity{0};
};

// Publisher
struct Publisher {
  explicit Publisher(dds_entity_t part);

  ~Publisher();

  dds_entity_t entity{0};
};

// Subscriber
struct Subscriber {
  explicit Subscriber(dds_entity_t part);

  ~Subscriber();

  dds_entity_t entity{0};
};

// DataWriter
struct DataWriter {
  explicit DataWriter(dds_entity_t pub, dds_entity_t topic, const dds_qos_t* qos, const dds_listener_t* listener);

  ~DataWriter();

  dds_entity_t entity{0};
};

// DataReader
struct DataReader {
  explicit DataReader(dds_entity_t sub, dds_entity_t topic, const dds_qos_t* qos, const dds_listener_t* listener);

  ~DataReader();

  dds_entity_t entity{0};
};

}  // namespace ddsc
