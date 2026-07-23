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

#include "./dds_topic.h"

#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace vlink {

DdsCdrPubSubType::DdsCdrPubSubType(const std::string& type_name, dds::TypeSupport native_type)
    : native_type_(std::move(native_type)) {
#ifdef VLINK_SUPPORT_DDS_V3
  set_name(type_name);
  max_serialized_type_size =
      native_type_ && native_type_->max_serialized_type_size >= 4U ? native_type_->max_serialized_type_size : 4U;
  is_compute_key_provided = native_type_ && native_type_->is_compute_key_provided;
#else
  setName(type_name.c_str());
  m_typeSize = native_type_ && native_type_->m_typeSize >= 4U ? native_type_->m_typeSize : 4U;
  m_isGetKeyDefined = native_type_ && native_type_->m_isGetKeyDefined;
#endif
}

#ifdef VLINK_SUPPORT_DDS_V3
bool DdsCdrPubSubType::is_compatible_data_representation(const Bytes& bytes,
                                                         dds::DataRepresentationId_t data_representation) {
  if VUNLIKELY (!bytes.data() || bytes.size() < 4U) {
    return false;
  }

  const auto encapsulation = static_cast<uint16_t>((static_cast<uint16_t>(bytes.data()[0]) << 8U) | bytes.data()[1]);

  if (data_representation == dds::XCDR_DATA_REPRESENTATION) {
    return encapsulation <= 3U;
  }
  if (data_representation == dds::XCDR2_DATA_REPRESENTATION) {
    return encapsulation >= 6U && encapsulation <= 11U;
  }

  return false;
}

bool DdsCdrPubSubType::serialize(const void* const data, rtps::SerializedPayload_t& payload,
                                 dds::DataRepresentationId_t data_representation) {
  const auto& bytes = *static_cast<const Bytes*>(data);

  return is_compatible_data_representation(bytes, data_representation) && copy_to_payload(bytes, payload);
}

bool DdsCdrPubSubType::deserialize(rtps::SerializedPayload_t& payload, void* data) {
  return copy_from_payload(payload, *static_cast<Bytes*>(data));
}

uint32_t DdsCdrPubSubType::calculate_serialized_size(const void* const data,
                                                     dds::DataRepresentationId_t data_representation) {
  const auto& bytes = *static_cast<const Bytes*>(data);

  if VUNLIKELY (!is_compatible_data_representation(bytes, data_representation)) {
    return 0U;
  }

  const auto size = bytes.size();

  return size <= std::numeric_limits<uint32_t>::max() ? static_cast<uint32_t>(size) : 0U;
}

bool DdsCdrPubSubType::compute_key(rtps::SerializedPayload_t& payload, rtps::InstanceHandle_t& handle, bool force_md5) {
  return native_type_ && native_type_->compute_key(payload, handle, force_md5);
}

bool DdsCdrPubSubType::compute_key(const void* const data, rtps::InstanceHandle_t& handle, bool force_md5) {
  rtps::SerializedPayload_t payload;
  return copy_to_payload(*static_cast<const Bytes*>(data), payload) && compute_key(payload, handle, force_md5);
}

void* DdsCdrPubSubType::create_data() { return new Bytes; }

void DdsCdrPubSubType::delete_data(void* data) { delete static_cast<Bytes*>(data); }

void DdsCdrPubSubType::register_type_object_representation() {
  if (native_type_) {
    native_type_->register_type_object_representation();
    type_identifiers_ = native_type_->type_identifiers();
  }
}
#else
bool DdsCdrPubSubType::serialize(void* data, rtps::SerializedPayload_t* payload) {
  return payload != nullptr && copy_to_payload(*static_cast<const Bytes*>(data), *payload);
}

bool DdsCdrPubSubType::deserialize(rtps::SerializedPayload_t* payload, void* data) {
  return payload != nullptr && copy_from_payload(*payload, *static_cast<Bytes*>(data));
}

std::function<uint32_t()> DdsCdrPubSubType::getSerializedSizeProvider(void* data) {
  return [data] {
    const auto size = static_cast<Bytes*>(data)->size();
    return size <= std::numeric_limits<uint32_t>::max() ? static_cast<uint32_t>(size) : 0U;
  };
}

bool DdsCdrPubSubType::getKey(void* data, rtps::InstanceHandle_t* handle, bool force_md5) {
  if VUNLIKELY (!native_type_ || !handle) {
    return false;
  }

  rtps::SerializedPayload_t payload;

  if VUNLIKELY (!copy_to_payload(*static_cast<Bytes*>(data), payload)) {
    return false;
  }

  void* sample = native_type_.create_data();
  const bool result = sample != nullptr && native_type_->deserialize(&payload, sample) &&
                      native_type_->getKey(sample, handle, force_md5);

  if VUNLIKELY (sample) {
    native_type_.delete_data(sample);
  }

  return result;
}

void* DdsCdrPubSubType::createData() { return new Bytes; }

void DdsCdrPubSubType::deleteData(void* data) { delete static_cast<Bytes*>(data); }
#endif

bool DdsCdrPubSubType::copy_to_payload(const Bytes& bytes, rtps::SerializedPayload_t& payload) {
  if VUNLIKELY (!bytes.data() || bytes.size() < 4U || bytes.size() > std::numeric_limits<uint32_t>::max()) {
    return false;
  }

  const auto size = static_cast<uint32_t>(bytes.size());

  if VUNLIKELY (payload.max_size < size) {
    try {
      payload.reserve(size);
    } catch (const std::bad_alloc&) {
      return false;
    }
  }
  if VUNLIKELY (!payload.data || payload.max_size < size) {
    return false;
  }

  std::memcpy(payload.data, bytes.data(), size);
  payload.length = size;
  payload.encapsulation = static_cast<uint16_t>((static_cast<uint16_t>(bytes.data()[0]) << 8U) | bytes.data()[1]);

  return true;
}

bool DdsCdrPubSubType::copy_from_payload(const rtps::SerializedPayload_t& payload, Bytes& bytes) {
  if VUNLIKELY (!payload.data || payload.length < 4U) {
    return false;
  }

  bytes = Bytes::shallow_copy(payload.data, payload.length);

  return true;
}

}  // namespace vlink
