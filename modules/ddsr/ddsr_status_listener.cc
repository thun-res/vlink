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

#include "./ddsr_status_listener.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include "./base/traits.h"
#include "./extension/status_detail.h"

namespace vlink {

static constexpr size_t kStatusInstanceHandleStorageSize = 32;

template <typename StatusT>
static std::shared_ptr<StatusT> make_status_with_handle(const void* handle, size_t size,
                                                        Status::InstanceHandle StatusT::* member) {
  auto storage = std::make_shared<std::array<uint8_t, kStatusInstanceHandleStorageSize>>();
  storage->fill(0);

  auto sts = std::shared_ptr<StatusT>(new StatusT(), [storage](StatusT* ptr) { delete ptr; });

  if VLIKELY (handle && size <= storage->size()) {
    std::memcpy(storage->data(), handle, size);
    sts.get()->*member = storage->data();
  }

  return sts;
}

template <typename T>
static auto convert_status(const T& status) {
  if constexpr (std::is_same_v<T, DDS_PublicationMatchedStatus>) {
    auto sts = make_status_with_handle<Status::PublicationMatched>(
        status.last_subscription_handle.keyHash.value, sizeof(status.last_subscription_handle.keyHash.value),
        &Status::PublicationMatched::last_subscription_handle);
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;
    sts->current_count = status.current_count;
    sts->current_count_change = status.current_count_change;

    return sts;
  } else if constexpr (std::is_same_v<T, DDS_OfferedDeadlineMissedStatus>) {
    auto sts = make_status_with_handle<Status::OfferedDeadlineMissed>(
        status.last_instance_handle.keyHash.value, sizeof(status.last_instance_handle.keyHash.value),
        &Status::OfferedDeadlineMissed::last_instance_handle);
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;

    return sts;
  } else if constexpr (std::is_same_v<T, DDS_OfferedIncompatibleQosStatus>) {
    auto sts = std::make_shared<Status::OfferedIncompatibleQos>();
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;
    sts->last_policy_id = status.last_policy_id;

    return sts;
  } else if constexpr (std::is_same_v<T, DDS_LivelinessLostStatus>) {
    auto sts = std::make_shared<Status::LivelinessLost>();
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;

    return sts;
  } else if constexpr (std::is_same_v<T, DDS_SubscriptionMatchedStatus>) {
    auto sts = make_status_with_handle<Status::SubscriptionMatched>(
        status.last_publication_handle.keyHash.value, sizeof(status.last_publication_handle.keyHash.value),
        &Status::SubscriptionMatched::last_publication_handle);
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;
    sts->current_count = status.current_count;
    sts->current_count_change = status.current_count_change;

    return sts;
  } else if constexpr (std::is_same_v<T, DDS_RequestedDeadlineMissedStatus>) {
    auto sts = make_status_with_handle<Status::RequestedDeadlineMissed>(
        status.last_instance_handle.keyHash.value, sizeof(status.last_instance_handle.keyHash.value),
        &Status::RequestedDeadlineMissed::last_instance_handle);
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;

    return sts;
  } else if constexpr (std::is_same_v<T, DDS_LivelinessChangedStatus>) {
    auto sts = make_status_with_handle<Status::LivelinessChanged>(status.last_publication_handle.keyHash.value,
                                                                  sizeof(status.last_publication_handle.keyHash.value),
                                                                  &Status::LivelinessChanged::last_publication_handle);
    sts->alive_count = status.alive_count;
    sts->not_alive_count = status.not_alive_count;
    sts->alive_count_change = status.alive_count_change;
    sts->not_alive_count_change = status.not_alive_count_change;

    return sts;
  } else if constexpr (std::is_same_v<T, DDS_SampleRejectedStatus>) {
    auto sts = make_status_with_handle<Status::SampleRejected>(status.last_instance_handle.keyHash.value,
                                                               sizeof(status.last_instance_handle.keyHash.value),
                                                               &Status::SampleRejected::last_instance_handle);
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;
    sts->last_reason = static_cast<Status::SampleRejected::Kind>(status.last_reason);

    return sts;
  } else if constexpr (std::is_same_v<T, DDS_RequestedIncompatibleQosStatus>) {
    auto sts = std::make_shared<Status::RequestedIncompatibleQos>();
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;
    sts->last_policy_id = status.last_policy_id;

    return sts;
  } else if constexpr (std::is_same_v<T, DDS_SampleLostStatus>) {
    auto sts = std::make_shared<Status::SampleLost>();
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;

    return sts;
  } else {
    static_assert(Traits::ExpectFalse<T>(), "Convert status error.");

    return std::make_shared<Status::Unknown>();
  }
}

// For writer
static void g_on_publication_matched(void* listener_data, DDS_DataWriter*, const DDS_PublicationMatchedStatus* status) {
  auto* instance = static_cast<DdsrWriterListener*>(listener_data);

  instance->on_publication_matched(instance->get_impl(), *status);

  if VLIKELY (!instance->get_impl()->has_register_status()) {
    return;
  }

  instance->get_impl()->call_status(convert_status(*status));
}

static void g_on_offered_deadline_missed(void* listener_data, DDS_DataWriter*,
                                         const DDS_OfferedDeadlineMissedStatus* status) {
  auto* instance = static_cast<DdsrWriterListener*>(listener_data);

  if VLIKELY (!instance->get_impl()->has_register_status()) {
    return;
  }

  instance->get_impl()->call_status(convert_status(*status));
}

static void g_on_offered_incompatible_qos(void* listener_data, DDS_DataWriter*,
                                          const DDS_OfferedIncompatibleQosStatus* status) {
  auto* instance = static_cast<DdsrWriterListener*>(listener_data);

  if VLIKELY (!instance->get_impl()->has_register_status()) {
    return;
  }

  instance->get_impl()->call_status(convert_status(*status));
}

static void g_on_liveliness_lost(void* listener_data, DDS_DataWriter*, const DDS_LivelinessLostStatus* status) {
  auto* instance = static_cast<DdsrWriterListener*>(listener_data);

  if VLIKELY (!instance->get_impl()->has_register_status()) {
    return;
  }

  instance->get_impl()->call_status(convert_status(*status));
}

// For reader
static void g_on_subscription_matched(void* listener_data, DDS_DataReader*,
                                      const DDS_SubscriptionMatchedStatus* status) {
  auto* instance = static_cast<DdsrReaderListener*>(listener_data);

  instance->on_subscription_matched(instance->get_impl(), *status);

  if VLIKELY (!instance->get_impl()->has_register_status()) {
    return;
  }

  instance->get_impl()->call_status(convert_status(*status));
}

static void g_on_requested_deadline_missed(void* listener_data, DDS_DataReader*,
                                           const DDS_RequestedDeadlineMissedStatus* status) {
  auto* instance = static_cast<DdsrReaderListener*>(listener_data);

  if VLIKELY (!instance->get_impl()->has_register_status()) {
    return;
  }

  instance->get_impl()->call_status(convert_status(*status));
}

static void g_on_liveliness_changed(void* listener_data, DDS_DataReader*, const DDS_LivelinessChangedStatus* status) {
  auto* instance = static_cast<DdsrReaderListener*>(listener_data);

  if VLIKELY (!instance->get_impl()->has_register_status()) {
    return;
  }

  instance->get_impl()->call_status(convert_status(*status));
}

static void g_on_sample_rejected(void* listener_data, DDS_DataReader*, const DDS_SampleRejectedStatus* status) {
  auto* instance = static_cast<DdsrReaderListener*>(listener_data);

  if VLIKELY (!instance->get_impl()->has_register_status()) {
    return;
  }

  instance->get_impl()->call_status(convert_status(*status));
}

static void g_on_requested_incompatible_qos(void* listener_data, DDS_DataReader*,
                                            const DDS_RequestedIncompatibleQosStatus* status) {
  auto* instance = static_cast<DdsrReaderListener*>(listener_data);

  if VLIKELY (!instance->get_impl()->has_register_status()) {
    return;
  }

  instance->get_impl()->call_status(convert_status(*status));
}

static void g_on_sample_lost(void* listener_data, DDS_DataReader*, const DDS_SampleLostStatus* status) {
  auto* instance = static_cast<DdsrReaderListener*>(listener_data);

  if VLIKELY (!instance->get_impl()->has_register_status()) {
    return;
  }

  instance->get_impl()->call_status(convert_status(*status));
}

static void g_on_data_available(void* listener_data, DDS_DataReader* reader) {
  auto* instance = static_cast<DdsrReaderListener*>(listener_data);

  instance->on_data_available(instance->get_impl(), reader);
}

// DdsrWriterListener
DdsrWriterListener::DdsrWriterListener(NodeImpl* impl) : impl_(impl) {
  listener_.as_listener.listener_data = this;

  listener_.on_publication_matched = g_on_publication_matched;
  listener_.on_offered_deadline_missed = g_on_offered_deadline_missed;
  listener_.on_offered_incompatible_qos = g_on_offered_incompatible_qos;
  listener_.on_liveliness_lost = g_on_liveliness_lost;
}

DdsrWriterListener::~DdsrWriterListener() {
  listener_.as_listener.listener_data = nullptr;

  listener_.on_publication_matched = nullptr;
  listener_.on_offered_deadline_missed = nullptr;
  listener_.on_offered_incompatible_qos = nullptr;
  listener_.on_liveliness_lost = nullptr;
}

Status::BasePtr DdsrWriterListener::get_status(DDS_DataWriter* writer, Status::Type type) {
  switch (type) {
    case Status::kPublicationMatched: {
      DDS_PublicationMatchedStatus status;
      DDS_DataWriter_get_publication_matched_status(writer, &status);
      return convert_status(status);
    }

    case Status::kOfferedDeadlineMissed: {
      DDS_OfferedDeadlineMissedStatus status;
      DDS_DataWriter_get_offered_deadline_missed_status(writer, &status);
      return convert_status(status);
    }

    case Status::kOfferedIncompatibleQos: {
      DDS_OfferedIncompatibleQosStatus status;
      DDS_DataWriter_get_offered_incompatible_qos_status(writer, &status);
      return convert_status(status);
    }

    case Status::kLivelinessLost: {
      DDS_LivelinessLostStatus status;
      DDS_DataWriter_get_liveliness_lost_status(writer, &status);
      return convert_status(status);
    }

    default: {
      return std::make_shared<Status::Unknown>();
    }
  }
}

void DdsrWriterListener::on_publication_matched(NodeImpl* impl, const DDS_PublicationMatchedStatus& status) {
  (void)impl;
  (void)status;
}

// DdsrReaderListener
DdsrReaderListener::DdsrReaderListener(NodeImpl* impl) : impl_(impl) {
  listener_.as_listener.listener_data = this;

  listener_.on_subscription_matched = g_on_subscription_matched;
  listener_.on_requested_deadline_missed = g_on_requested_deadline_missed;
  listener_.on_liveliness_changed = g_on_liveliness_changed;
  listener_.on_sample_rejected = g_on_sample_rejected;
  listener_.on_requested_incompatible_qos = g_on_requested_incompatible_qos;
  listener_.on_sample_lost = g_on_sample_lost;
  listener_.on_data_available = g_on_data_available;
}

DdsrReaderListener::~DdsrReaderListener() {
  listener_.as_listener.listener_data = nullptr;

  listener_.on_subscription_matched = nullptr;
  listener_.on_requested_deadline_missed = nullptr;
  listener_.on_liveliness_changed = nullptr;
  listener_.on_sample_rejected = nullptr;
  listener_.on_requested_incompatible_qos = nullptr;
  listener_.on_sample_lost = nullptr;
  listener_.on_data_available = nullptr;
}

Status::BasePtr DdsrReaderListener::get_status(DDS_DataReader* reader, Status::Type type) {
  switch (type) {
    case Status::kSubscriptionMatched: {
      DDS_SubscriptionMatchedStatus status;
      DDS_DataReader_get_subscription_matched_status(reader, &status);
      return convert_status(status);
    }

    case Status::kRequestedDeadlineMissed: {
      DDS_RequestedDeadlineMissedStatus status;
      DDS_DataReader_get_requested_deadline_missed_status(reader, &status);
      return convert_status(status);
    }

    case Status::kLivelinessChanged: {
      DDS_LivelinessChangedStatus status;
      DDS_DataReader_get_liveliness_changed_status(reader, &status);
      return convert_status(status);
    }

    case Status::kSampleRejected: {
      DDS_SampleRejectedStatus status;
      DDS_DataReader_get_sample_rejected_status(reader, &status);
      return convert_status(status);
    }

    case Status::kRequestedIncompatibleQos: {
      DDS_RequestedIncompatibleQosStatus status;
      DDS_DataReader_get_requested_incompatible_qos_status(reader, &status);
      return convert_status(status);
    }

    case Status::kSampleLost: {
      DDS_SampleLostStatus status;
      DDS_DataReader_get_sample_lost_status(reader, &status);
      return convert_status(status);
    }

    default: {
      return std::make_shared<Status::Unknown>();
    }
  }
}

void DdsrReaderListener::on_subscription_matched(NodeImpl* impl, const DDS_SubscriptionMatchedStatus& status) {
  (void)impl;
  (void)status;
}

void DdsrReaderListener::on_data_available(NodeImpl* impl, DDS_DataReader* reader) {
  (void)impl;
  (void)reader;
}

}  // namespace vlink
