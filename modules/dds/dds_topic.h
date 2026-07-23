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

#include <string>

#include "./dds_factory.h"

namespace vlink {

class DdsCdrPubSubType final : public dds::TopicDataType {
 public:
  DdsCdrPubSubType(const std::string& type_name, dds::TypeSupport native_type);

#ifdef VLINK_SUPPORT_DDS_V3
  bool serialize(const void* data, rtps::SerializedPayload_t& payload,
                 dds::DataRepresentationId_t data_representation) override;

  bool deserialize(rtps::SerializedPayload_t& payload, void* data) override;

  uint32_t calculate_serialized_size(const void* data, dds::DataRepresentationId_t data_representation) override;

  bool compute_key(rtps::SerializedPayload_t& payload, rtps::InstanceHandle_t& handle, bool force_md5 = false) override;

  bool compute_key(const void* data, rtps::InstanceHandle_t& handle, bool force_md5 = false) override;

  void* create_data() override;

  void delete_data(void* data) override;

  void register_type_object_representation() override;
#else
  bool serialize(void* data, rtps::SerializedPayload_t* payload) override;

  bool deserialize(rtps::SerializedPayload_t* payload, void* data) override;

  std::function<uint32_t()> getSerializedSizeProvider(void* data) override;

  bool getKey(void* data, rtps::InstanceHandle_t* handle, bool force_md5 = false) override;

  void* createData() override;

  void deleteData(void* data) override;
#endif

 private:
#ifdef VLINK_SUPPORT_DDS_V3
  static bool is_compatible_data_representation(const Bytes& bytes, dds::DataRepresentationId_t data_representation);
#endif

  static bool copy_to_payload(const Bytes& bytes, rtps::SerializedPayload_t& payload);

  static bool copy_from_payload(const rtps::SerializedPayload_t& payload, Bytes& bytes);

  dds::TypeSupport native_type_;
};

}  // namespace vlink
