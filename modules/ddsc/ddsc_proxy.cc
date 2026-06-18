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

#include "./ddsc_proxy.hpp"

#include <string>

#include "./base/logger.h"
#include "./builtin/BuiltInRaw.h"

namespace ddsc {

// DomainParticipant
DomainParticipant::DomainParticipant(int32_t domain, const dds_qos_t* qos) {
  entity = dds_create_participant(domain, qos, nullptr);

  if VUNLIKELY (entity <= 0) {
    CLOG_E("dds_create_participant: %s.", dds_strretcode(-entity));
  }
}

DomainParticipant::~DomainParticipant() {
  if VUNLIKELY (entity <= 0) {
    return;
  }

  dds_delete(entity);

  entity = 0;
}

// Topic
Topic::Topic(dds_entity_t part, const std::string& topic, const std::string& type_name) {
  (void)type_name;

  entity = dds_create_topic(part, &vlink_BuiltInRaw_desc, topic.c_str(), nullptr, nullptr);

  if VUNLIKELY (entity <= 0) {
    CLOG_E("dds_create_topic: %s.", dds_strretcode(-entity));
  }
}

Topic::~Topic() {
  if VUNLIKELY (entity <= 0) {
    return;
  }

  dds_delete(entity);

  entity = 0;
}

// Publisher
Publisher::Publisher(dds_entity_t part) {
  entity = dds_create_publisher(part, nullptr, nullptr);

  if VUNLIKELY (entity <= 0) {
    CLOG_E("dds_create_publisher: %s.", dds_strretcode(-entity));
  }
}

Publisher::~Publisher() {
  if VUNLIKELY (entity <= 0) {
    return;
  }

  dds_delete(entity);

  entity = 0;
}

// Subscriber
Subscriber::Subscriber(dds_entity_t part) {
  entity = dds_create_subscriber(part, nullptr, nullptr);

  if VUNLIKELY (entity <= 0) {
    CLOG_E("dds_create_subscriber: %s.", dds_strretcode(-entity));
  }
}

Subscriber::~Subscriber() {
  if VUNLIKELY (entity <= 0) {
    return;
  }

  dds_delete(entity);

  entity = 0;
}

// DataWriter
DataWriter::DataWriter(dds_entity_t pub, dds_entity_t topic, const dds_qos_t* qos, const dds_listener_t* listener) {
  entity = dds_create_writer(pub, topic, qos, listener);

  if VUNLIKELY (entity <= 0) {
    CLOG_E("dds_create_writer: %s.", dds_strretcode(-entity));
  }
}

DataWriter::~DataWriter() {
  if VUNLIKELY (entity <= 0) {
    return;
  }

  dds_delete(entity);

  entity = 0;
}

// DataReader
DataReader::DataReader(dds_entity_t sub, dds_entity_t topic, const dds_qos_t* qos, const dds_listener_t* listener) {
  entity = dds_create_reader(sub, topic, qos, listener);

  if VUNLIKELY (entity <= 0) {
    CLOG_E("dds_create_reader: %s.", dds_strretcode(-entity));
  }
}

DataReader::~DataReader() {
  if VUNLIKELY (entity <= 0) {
    return;
  }

  dds_delete(entity);

  entity = 0;
}

}  // namespace ddsc
