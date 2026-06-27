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

#include "./ddst_status_listener.h"

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

template <Status::Type TypeT, typename T>
static auto convert_status(const T& status) {
  if constexpr (TypeT == Status::kPublicationMatched) {
    auto sts = make_status_with_handle<Status::PublicationMatched>(
        status.last_subscription_handle.keyHash.value, sizeof(status.last_subscription_handle.keyHash.value),
        &Status::PublicationMatched::last_subscription_handle);
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;
    sts->current_count = status.current_count;
    sts->current_count_change = status.current_count_change;

    return sts;
  } else if constexpr (TypeT == Status::kOfferedDeadlineMissed) {
    auto sts = make_status_with_handle<Status::OfferedDeadlineMissed>(
        status.last_instance_handle.keyHash.value, sizeof(status.last_instance_handle.keyHash.value),
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
        status.last_publication_handle.keyHash.value, sizeof(status.last_publication_handle.keyHash.value),
        &Status::SubscriptionMatched::last_publication_handle);
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;
    sts->current_count = status.current_count;
    sts->current_count_change = status.current_count_change;

    return sts;
  } else if constexpr (TypeT == Status::kRequestedDeadlineMissed) {
    auto sts = make_status_with_handle<Status::RequestedDeadlineMissed>(
        status.last_instance_handle.keyHash.value, sizeof(status.last_instance_handle.keyHash.value),
        &Status::RequestedDeadlineMissed::last_instance_handle);
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;

    return sts;
  } else if constexpr (TypeT == Status::kLivelinessChanged) {
    auto sts = make_status_with_handle<Status::LivelinessChanged>(status.last_publication_handle.keyHash.value,
                                                                  sizeof(status.last_publication_handle.keyHash.value),
                                                                  &Status::LivelinessChanged::last_publication_handle);
    sts->alive_count = status.alive_count;
    sts->not_alive_count = status.not_alive_count;
    sts->alive_count_change = status.alive_count_change;
    sts->not_alive_count_change = status.not_alive_count_change;

    return sts;
  } else if constexpr (TypeT == Status::kSampleRejected) {
    auto sts = make_status_with_handle<Status::SampleRejected>(status.last_instance_handle.keyHash.value,
                                                               sizeof(status.last_instance_handle.keyHash.value),
                                                               &Status::SampleRejected::last_instance_handle);
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

// DdstWriterListener
DdstWriterListener::DdstWriterListener(NodeImpl* impl) : impl_(impl) {}

DdstWriterListener::~DdstWriterListener() = default;

Status::BasePtr DdstWriterListener::get_status(ddst::DataWriter* writer, Status::Type type) {
  switch (type) {
    case Status::kPublicationMatched: {
      ddst::PublicationMatchedStatus status;
      writer->get_publication_matched_status(status);
      return convert_status<Status::kPublicationMatched>(status);
    }

    case Status::kOfferedDeadlineMissed: {
      ddst::OfferedDeadlineMissedStatus status;
      writer->get_offered_deadline_missed_status(status);
      return convert_status<Status::kOfferedDeadlineMissed>(status);
    }

    case Status::kOfferedIncompatibleQos: {
      ddst::OfferedIncompatibleQosStatus status;
      writer->get_offered_incompatible_qos_status(status);
      return convert_status<Status::kOfferedIncompatibleQos>(status);
    }

    case Status::kLivelinessLost: {
      ddst::LivelinessLostStatus status;
      writer->get_liveliness_lost_status(status);
      return convert_status<Status::kLivelinessLost>(status);
    }

    default: {
      return std::make_shared<Status::Unknown>();
    }
  }
}

void DdstWriterListener::on_publication_matched(ddst::DataWriter*, const ddst::PublicationMatchedStatus& status) {
  if VLIKELY (!impl_->has_register_status()) {
    return;
  }

  impl_->call_status(convert_status<Status::kPublicationMatched>(status));
}

void DdstWriterListener::on_offered_deadline_missed(ddst::DataWriter*,
                                                    const ddst::OfferedDeadlineMissedStatus& status) {
  if VLIKELY (!impl_->has_register_status()) {
    return;
  }

  impl_->call_status(convert_status<Status::kOfferedDeadlineMissed>(status));
}

void DdstWriterListener::on_offered_incompatible_qos(ddst::DataWriter*,
                                                     const ddst::OfferedIncompatibleQosStatus& status) {
  if VLIKELY (!impl_->has_register_status()) {
    return;
  }

  impl_->call_status(convert_status<Status::kOfferedIncompatibleQos>(status));
}

void DdstWriterListener::on_liveliness_lost(ddst::DataWriter*, const ddst::LivelinessLostStatus& status) {
  if VLIKELY (!impl_->has_register_status()) {
    return;
  }

  impl_->call_status(convert_status<Status::kLivelinessLost>(status));
}

// DdstReaderListener
DdstReaderListener::DdstReaderListener(NodeImpl* impl) : impl_(impl) {}

DdstReaderListener::~DdstReaderListener() = default;

Status::BasePtr DdstReaderListener::get_status(ddst::DataReader* reader, Status::Type type) {
  switch (type) {
    case Status::kSubscriptionMatched: {
      ddst::SubscriptionMatchedStatus status;
      reader->get_subscription_matched_status(status);
      return convert_status<Status::kSubscriptionMatched>(status);
    }

    case Status::kRequestedDeadlineMissed: {
      ddst::RequestedDeadlineMissedStatus status;
      reader->get_requested_deadline_missed_status(status);
      return convert_status<Status::kRequestedDeadlineMissed>(status);
    }

    case Status::kLivelinessChanged: {
      ddst::LivelinessChangedStatus status;
      reader->get_liveliness_changed_status(status);
      return convert_status<Status::kLivelinessChanged>(status);
    }

    case Status::kSampleRejected: {
      ddst::SampleRejectedStatus status;
      reader->get_sample_rejected_status(status);
      return convert_status<Status::kSampleRejected>(status);
    }

    case Status::kRequestedIncompatibleQos: {
      ddst::RequestedIncompatibleQosStatus status;
      reader->get_requested_incompatible_qos_status(status);
      return convert_status<Status::kRequestedIncompatibleQos>(status);
    }

    case Status::kSampleLost: {
      ddst::SampleLostStatus status;
      reader->get_sample_lost_status(status);
      return convert_status<Status::kSampleLost>(status);
    }

    default: {
      return std::make_shared<Status::Unknown>();
    }
  }
}

void DdstReaderListener::on_subscription_matched(ddst::DataReader*, const ddst::SubscriptionMatchedStatus& status) {
  if VLIKELY (!impl_->has_register_status()) {
    return;
  }

  impl_->call_status(convert_status<Status::kSubscriptionMatched>(status));
}

void DdstReaderListener::on_requested_deadline_missed(ddst::DataReader*,
                                                      const ddst::RequestedDeadlineMissedStatus& status) {
  if VLIKELY (!impl_->has_register_status()) {
    return;
  }

  impl_->call_status(convert_status<Status::kRequestedDeadlineMissed>(status));
}

void DdstReaderListener::on_liveliness_changed(ddst::DataReader*, const ddst::LivelinessChangedStatus& status) {
  if VLIKELY (!impl_->has_register_status()) {
    return;
  }

  impl_->call_status(convert_status<Status::kLivelinessChanged>(status));
}

void DdstReaderListener::on_sample_rejected(ddst::DataReader*, const ddst::SampleRejectedStatus& status) {
  if VLIKELY (!impl_->has_register_status()) {
    return;
  }

  impl_->call_status(convert_status<Status::kSampleRejected>(status));
}

void DdstReaderListener::on_requested_incompatible_qos(ddst::DataReader*,
                                                       const ddst::RequestedIncompatibleQosStatus& status) {
  if VLIKELY (!impl_->has_register_status()) {
    return;
  }

  impl_->call_status(convert_status<Status::kRequestedIncompatibleQos>(status));
}

void DdstReaderListener::on_sample_lost(ddst::DataReader*, const ddst::SampleLostStatus& status) {
  if VLIKELY (!impl_->has_register_status()) {
    return;
  }

  impl_->call_status(convert_status<Status::kSampleLost>(status));
}

}  // namespace vlink
