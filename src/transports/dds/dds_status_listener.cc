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

#include "./dds_status_listener.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include "./base/traits.h"
#include "./extension/status_detail.h"

namespace vlink {

#ifdef VLINK_SUPPORT_DDS_V3
static constexpr size_t kDdsInstanceHandleSize = rtps::RTPS_KEY_HASH_SIZE;
#else
static constexpr size_t kDdsInstanceHandleSize = 16;
#endif
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

template <Status::Type TypeT, typename T>
static auto convert_status(const T& status) {
  if constexpr (TypeT == Status::kPublicationMatched) {
    auto sts = make_status_with_handle<Status::PublicationMatched>(
        status.last_subscription_handle.value, kDdsInstanceHandleSize,
        &Status::PublicationMatched::last_subscription_handle);
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;
    sts->current_count = status.current_count;
    sts->current_count_change = status.current_count_change;

    return sts;
  } else if constexpr (TypeT == Status::kOfferedDeadlineMissed) {
    auto sts = make_status_with_handle<Status::OfferedDeadlineMissed>(
        status.last_instance_handle.value, kDdsInstanceHandleSize,
        &Status::OfferedDeadlineMissed::last_instance_handle);
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;

    return sts;
  } else if constexpr (TypeT == Status::kOfferedIncompatibleQos) {
    auto sts = std::make_shared<Status::OfferedIncompatibleQos>();
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;
    sts->last_policy_id = status.last_policy_id;

    return sts;
  } else if constexpr (TypeT == Status::kLivelinessLost) {
    auto sts = std::make_shared<Status::LivelinessLost>();
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;

    return sts;
  } else if constexpr (TypeT == Status::kSubscriptionMatched) {
    auto sts = make_status_with_handle<Status::SubscriptionMatched>(
        status.last_publication_handle.value, kDdsInstanceHandleSize,
        &Status::SubscriptionMatched::last_publication_handle);
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;
    sts->current_count = status.current_count;
    sts->current_count_change = status.current_count_change;

    return sts;
  } else if constexpr (TypeT == Status::kRequestedDeadlineMissed) {
    auto sts = make_status_with_handle<Status::RequestedDeadlineMissed>(
        status.last_instance_handle.value, kDdsInstanceHandleSize,
        &Status::RequestedDeadlineMissed::last_instance_handle);
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;

    return sts;
  } else if constexpr (TypeT == Status::kLivelinessChanged) {
    auto sts =
        make_status_with_handle<Status::LivelinessChanged>(status.last_publication_handle.value, kDdsInstanceHandleSize,
                                                           &Status::LivelinessChanged::last_publication_handle);
    sts->alive_count = status.alive_count;
    sts->not_alive_count = status.not_alive_count;
    sts->alive_count_change = status.alive_count_change;
    sts->not_alive_count_change = status.not_alive_count_change;

    return sts;
  } else if constexpr (TypeT == Status::kSampleRejected) {
    auto sts = make_status_with_handle<Status::SampleRejected>(
        status.last_instance_handle.value, kDdsInstanceHandleSize, &Status::SampleRejected::last_instance_handle);
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;
    sts->last_reason = static_cast<Status::SampleRejected::Kind>(status.last_reason);

    return sts;
  } else if constexpr (TypeT == Status::kRequestedIncompatibleQos) {
    auto sts = std::make_shared<Status::RequestedIncompatibleQos>();
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;
    sts->last_policy_id = status.last_policy_id;

    return sts;
  } else if constexpr (TypeT == Status::kSampleLost) {
    auto sts = std::make_shared<Status::SampleLost>();
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;

    return sts;
  } else {
    static_assert(Traits::ExpectFalse<T>(), "Convert status error.");

    return std::make_shared<Status::Unknown>();
  }
}

// DdsWriterListener
DdsWriterListener::DdsWriterListener(NodeImpl* impl) : impl_(impl) {}

DdsWriterListener::~DdsWriterListener() = default;

Status::BasePtr DdsWriterListener::get_status(dds::DataWriter* writer, Status::Type type) {
  switch (type) {
    case Status::kPublicationMatched: {
      dds::PublicationMatchedStatus status;
      writer->get_publication_matched_status(status);
      return convert_status<Status::kPublicationMatched>(status);
    }

    case Status::kOfferedDeadlineMissed: {
      dds::OfferedDeadlineMissedStatus status;
      writer->get_offered_deadline_missed_status(status);
      return convert_status<Status::kOfferedDeadlineMissed>(status);
    }

    case Status::kOfferedIncompatibleQos: {
      dds::OfferedIncompatibleQosStatus status;
      writer->get_offered_incompatible_qos_status(status);
      return convert_status<Status::kOfferedIncompatibleQos>(status);
    }

    case Status::kLivelinessLost: {
      dds::LivelinessLostStatus status;
      writer->get_liveliness_lost_status(status);
      return convert_status<Status::kLivelinessLost>(status);
    }

    default: {
      return std::make_shared<Status::Unknown>();
    }
  }
}

void DdsWriterListener::on_publication_matched(dds::DataWriter*, const dds::PublicationMatchedStatus& status) {
  if VLIKELY (!impl_->has_register_status()) {
    return;
  }

  impl_->call_status(convert_status<Status::kPublicationMatched>(status));
}

void DdsWriterListener::on_offered_deadline_missed(dds::DataWriter*, const dds::OfferedDeadlineMissedStatus& status) {
  if VLIKELY (!impl_->has_register_status()) {
    return;
  }

  impl_->call_status(convert_status<Status::kOfferedDeadlineMissed>(status));
}

void DdsWriterListener::on_offered_incompatible_qos(dds::DataWriter*, const dds::OfferedIncompatibleQosStatus& status) {
  if VLIKELY (!impl_->has_register_status()) {
    return;
  }

  impl_->call_status(convert_status<Status::kOfferedIncompatibleQos>(status));
}

void DdsWriterListener::on_liveliness_lost(dds::DataWriter*, const dds::LivelinessLostStatus& status) {
  if VLIKELY (!impl_->has_register_status()) {
    return;
  }

  impl_->call_status(convert_status<Status::kLivelinessLost>(status));
}

// DdsReaderListener
DdsReaderListener::DdsReaderListener(NodeImpl* impl) : impl_(impl) {}

DdsReaderListener::~DdsReaderListener() = default;

Status::BasePtr DdsReaderListener::get_status(dds::DataReader* reader, Status::Type type) {
  switch (type) {
    case Status::kSubscriptionMatched: {
      dds::SubscriptionMatchedStatus status;
      reader->get_subscription_matched_status(status);
      return convert_status<Status::kSubscriptionMatched>(status);
    }

    case Status::kRequestedDeadlineMissed: {
      dds::RequestedDeadlineMissedStatus status;
      reader->get_requested_deadline_missed_status(status);
      return convert_status<Status::kRequestedDeadlineMissed>(status);
    }

    case Status::kLivelinessChanged: {
      dds::LivelinessChangedStatus status;
      reader->get_liveliness_changed_status(status);
      return convert_status<Status::kLivelinessChanged>(status);
    }

    case Status::kSampleRejected: {
      dds::SampleRejectedStatus status;
      reader->get_sample_rejected_status(status);
      return convert_status<Status::kSampleRejected>(status);
    }

    case Status::kRequestedIncompatibleQos: {
      dds::RequestedIncompatibleQosStatus status;
      reader->get_requested_incompatible_qos_status(status);
      return convert_status<Status::kRequestedIncompatibleQos>(status);
    }

    case Status::kSampleLost: {
      dds::SampleLostStatus status;
      reader->get_sample_lost_status(status);
      return convert_status<Status::kSampleLost>(status);
    }

    default: {
      return std::make_shared<Status::Unknown>();
    }
  }
}

void DdsReaderListener::on_subscription_matched(dds::DataReader*, const dds::SubscriptionMatchedStatus& status) {
  if VLIKELY (!impl_->has_register_status()) {
    return;
  }

  impl_->call_status(convert_status<Status::kSubscriptionMatched>(status));
}

void DdsReaderListener::on_requested_deadline_missed(dds::DataReader*,
                                                     const dds::RequestedDeadlineMissedStatus& status) {
  if VLIKELY (!impl_->has_register_status()) {
    return;
  }

  impl_->call_status(convert_status<Status::kRequestedDeadlineMissed>(status));
}

void DdsReaderListener::on_liveliness_changed(dds::DataReader*, const dds::LivelinessChangedStatus& status) {
  if VLIKELY (!impl_->has_register_status()) {
    return;
  }

  impl_->call_status(convert_status<Status::kLivelinessChanged>(status));
}

void DdsReaderListener::on_sample_rejected(dds::DataReader*, const dds::SampleRejectedStatus& status) {
  if VLIKELY (!impl_->has_register_status()) {
    return;
  }

  impl_->call_status(convert_status<Status::kSampleRejected>(status));
}

void DdsReaderListener::on_requested_incompatible_qos(dds::DataReader*,
                                                      const dds::RequestedIncompatibleQosStatus& status) {
  if VLIKELY (!impl_->has_register_status()) {
    return;
  }

  impl_->call_status(convert_status<Status::kRequestedIncompatibleQos>(status));
}

void DdsReaderListener::on_sample_lost(dds::DataReader*, const dds::SampleLostStatus& status) {
  if VLIKELY (!impl_->has_register_status()) {
    return;
  }

  impl_->call_status(convert_status<Status::kSampleLost>(status));
}

}  // namespace vlink
