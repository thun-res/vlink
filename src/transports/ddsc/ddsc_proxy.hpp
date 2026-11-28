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
#include <dds/version.h>

#if DDS_VERSION_MAJOR == 0 && DDS_VERSION_MINOR == 10
#include <dds/ddsi/ddsi_serdata.h>
#include <dds/ddsi/ddsi_sertype.h>
#define VLINK_DDSC_USE_CUSTOM_SERTYPE 1
#else
#define VLINK_DDSC_USE_CUSTOM_SERTYPE 0
#endif

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "./base/macros.h"

namespace ddsc {

#if VLINK_DDSC_USE_CUSTOM_SERTYPE
static constexpr uint32_t kRawWireHeaderSize = 16U;

struct BuiltInRawSerdata final {
  struct ddsi_serdata c;
  uint8_t* wire_buf{nullptr};
  size_t wire_cap{0U};
  uint64_t id{0U};
  uint32_t wire_size{0U};
  uint32_t payload_size{0U};

  [[nodiscard]] const uint8_t* payload_data() const noexcept {
    return wire_buf != nullptr ? (wire_buf + kRawWireHeaderSize) : nullptr;
  }
};

const struct ddsi_serdata_ops* built_in_raw_serdata_ops() noexcept;
#endif

// DomainParticipant
struct DomainParticipant {
  explicit DomainParticipant(int32_t domain, const dds_qos_t* qos = nullptr);

  ~DomainParticipant();

  dds_entity_t entity{0};

  VLINK_DISALLOW_COPY_AND_ASSIGN(DomainParticipant)
};

// Topic
struct Topic {
  explicit Topic(dds_entity_t part, const std::string& topic, const std::string& type_name = "",
                 bool use_default_sertype = false);

  ~Topic();

  dds_entity_t entity{0};

  VLINK_DISALLOW_COPY_AND_ASSIGN(Topic)
};

// Publisher
struct Publisher {
  explicit Publisher(dds_entity_t part);

  ~Publisher();

  dds_entity_t entity{0};

  VLINK_DISALLOW_COPY_AND_ASSIGN(Publisher)
};

// Subscriber
struct Subscriber {
  explicit Subscriber(dds_entity_t part);

  ~Subscriber();

  dds_entity_t entity{0};

  VLINK_DISALLOW_COPY_AND_ASSIGN(Subscriber)
};

// DataWriter
struct DataWriter {
  explicit DataWriter(dds_entity_t pub, dds_entity_t topic, const dds_qos_t* qos, const dds_listener_t* listener);

  ~DataWriter();

  dds_entity_t entity{0};

  VLINK_DISALLOW_COPY_AND_ASSIGN(DataWriter)
};

// DataReader
struct DataReader {
  explicit DataReader(dds_entity_t sub, dds_entity_t topic, const dds_qos_t* qos, const dds_listener_t* listener);

  ~DataReader();

  dds_entity_t entity{0};

  VLINK_DISALLOW_COPY_AND_ASSIGN(DataReader)
};

}  // namespace ddsc
