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

#include "./ddsc_status_listener.hpp"

#include <memory>

#include "./base/traits.h"
#include "./extension/status_detail.h"

namespace vlink {

template <typename T>
static auto convert_status(const T& status) {
  if constexpr (std::is_same_v<T, dds_publication_matched_status_t>) {
    auto sts = std::make_shared<Status::PublicationMatched>();
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;
    sts->current_count = status.current_count;
    sts->current_count_change = status.current_count_change;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    sts->last_subscription_handle = reinterpret_cast<Status::InstanceHandle>(status.last_subscription_handle);

    return sts;
  } else if constexpr (std::is_same_v<T, dds_offered_deadline_missed_status_t>) {
    auto sts = std::make_shared<Status::OfferedDeadlineMissed>();
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    sts->last_instance_handle = reinterpret_cast<Status::InstanceHandle>(status.last_instance_handle);

    return sts;
  } else if constexpr (std::is_same_v<T, dds_offered_incompatible_qos_status_t>) {
    auto sts = std::make_shared<Status::OfferedIncompatibleQos>();
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;
    sts->last_policy_id = status.last_policy_id;

    return sts;
  } else if constexpr (std::is_same_v<T, dds_liveliness_lost_status_t>) {
    auto sts = std::make_shared<Status::LivelinessLost>();
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;

    return sts;
  } else if constexpr (std::is_same_v<T, dds_subscription_matched_status_t>) {
    auto sts = std::make_shared<Status::SubscriptionMatched>();
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;
    sts->current_count = status.current_count;
    sts->current_count_change = status.current_count_change;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    sts->last_publication_handle = reinterpret_cast<Status::InstanceHandle>(status.last_publication_handle);

    return sts;
  } else if constexpr (std::is_same_v<T, dds_requested_deadline_missed_status_t>) {
    auto sts = std::make_shared<Status::RequestedDeadlineMissed>();
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    sts->last_instance_handle = reinterpret_cast<Status::InstanceHandle>(status.last_instance_handle);

    return sts;
  } else if constexpr (std::is_same_v<T, dds_liveliness_changed_status_t>) {
    auto sts = std::make_shared<Status::LivelinessChanged>();
    sts->alive_count = status.alive_count;
    sts->not_alive_count = status.not_alive_count;
    sts->alive_count_change = status.alive_count_change;
    sts->not_alive_count_change = status.not_alive_count_change;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    sts->last_publication_handle = reinterpret_cast<Status::InstanceHandle>(status.last_publication_handle);

    return sts;
  } else if constexpr (std::is_same_v<T, dds_sample_rejected_status_t>) {
    auto sts = std::make_shared<Status::SampleRejected>();
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;
    sts->last_reason = static_cast<Status::SampleRejected::Kind>(status.last_reason);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    sts->last_instance_handle = reinterpret_cast<Status::InstanceHandle>(status.last_instance_handle);

    return sts;
  } else if constexpr (std::is_same_v<T, dds_requested_incompatible_qos_status_t>) {
    auto sts = std::make_shared<Status::RequestedIncompatibleQos>();
    sts->total_count = status.total_count;
    sts->total_count_change = status.total_count_change;
    sts->last_policy_id = status.last_policy_id;

    return sts;
  } else if constexpr (std::is_same_v<T, dds_sample_lost_status_t>) {
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
static void g_on_publication_matched(dds_entity_t, const dds_publication_matched_status_t status, void* arg) {
  auto* instance = static_cast<DdscWriterListener*>(arg);

  instance->on_publication_matched(instance->get_impl(), status);

  if VLIKELY (!instance->get_impl()->has_register_status()) {
    return;
  }

  instance->get_impl()->call_status(convert_status(status));
}

static void g_on_offered_deadline_missed(dds_entity_t, const dds_offered_deadline_missed_status_t status, void* arg) {
  auto* instance = static_cast<DdscWriterListener*>(arg);

  if VLIKELY (!instance->get_impl()->has_register_status()) {
    return;
  }

  instance->get_impl()->call_status(convert_status(status));
}

static void g_on_offered_incompatible_qos(dds_entity_t, const dds_offered_incompatible_qos_status_t status, void* arg) {
  auto* instance = static_cast<DdscWriterListener*>(arg);

  if VLIKELY (!instance->get_impl()->has_register_status()) {
    return;
  }

  instance->get_impl()->call_status(convert_status(status));
}

static void g_on_liveliness_lost(dds_entity_t, const dds_liveliness_lost_status_t status, void* arg) {
  auto* instance = static_cast<DdscWriterListener*>(arg);

  if VLIKELY (!instance->get_impl()->has_register_status()) {
    return;
  }

  instance->get_impl()->call_status(convert_status(status));
}

// For reader
static void g_on_subscription_matched(dds_entity_t, const dds_subscription_matched_status_t status, void* arg) {
  auto* instance = static_cast<DdscReaderListener*>(arg);

  instance->on_subscription_matched(instance->get_impl(), status);

  if VLIKELY (!instance->get_impl()->has_register_status()) {
    return;
  }

  instance->get_impl()->call_status(convert_status(status));
}

static void g_on_requested_deadline_missed(dds_entity_t, const dds_requested_deadline_missed_status_t status,
                                           void* arg) {
  auto* instance = static_cast<DdscReaderListener*>(arg);

  if VLIKELY (!instance->get_impl()->has_register_status()) {
    return;
  }

  instance->get_impl()->call_status(convert_status(status));
}

static void g_on_liveliness_changed(dds_entity_t, const dds_liveliness_changed_status_t status, void* arg) {
  auto* instance = static_cast<DdscReaderListener*>(arg);

  if VLIKELY (!instance->get_impl()->has_register_status()) {
    return;
  }

  instance->get_impl()->call_status(convert_status(status));
}

static void g_on_sample_rejected(dds_entity_t, const dds_sample_rejected_status_t status, void* arg) {
  auto* instance = static_cast<DdscReaderListener*>(arg);

  if VLIKELY (!instance->get_impl()->has_register_status()) {
    return;
  }

  instance->get_impl()->call_status(convert_status(status));
}

static void g_on_requested_incompatible_qos(dds_entity_t, const dds_requested_incompatible_qos_status_t status,
                                            void* arg) {
  auto* instance = static_cast<DdscReaderListener*>(arg);

  if VLIKELY (!instance->get_impl()->has_register_status()) {
    return;
  }

  instance->get_impl()->call_status(convert_status(status));
}

static void g_on_sample_lost(dds_entity_t, const dds_sample_lost_status_t status, void* arg) {
  auto* instance = static_cast<DdscReaderListener*>(arg);

  if VLIKELY (!instance->get_impl()->has_register_status()) {
    return;
  }

  instance->get_impl()->call_status(convert_status(status));
}

static void g_on_data_available(dds_entity_t reader, void* arg) {
  auto* instance = static_cast<DdscReaderListener*>(arg);

  instance->on_data_available(instance->get_impl(), reader);
}

// DdscWriterListener
DdscWriterListener::DdscWriterListener(NodeImpl* impl) : impl_(impl) {
  listener_ = dds_create_listener(this);

  dds_lset_publication_matched(listener_, g_on_publication_matched);
  dds_lset_offered_deadline_missed(listener_, g_on_offered_deadline_missed);
  dds_lset_offered_incompatible_qos(listener_, g_on_offered_incompatible_qos);
  dds_lset_liveliness_lost(listener_, g_on_liveliness_lost);
}

DdscWriterListener::~DdscWriterListener() { dds_delete_listener(listener_); }

Status::BasePtr DdscWriterListener::get_status(dds_entity_t writer, Status::Type type) {
  switch (type) {
    case Status::kPublicationMatched: {
      dds_publication_matched_status_t status;
      dds_get_publication_matched_status(writer, &status);
      return convert_status(status);
    }

    case Status::kOfferedDeadlineMissed: {
      dds_offered_deadline_missed_status_t status;
      dds_get_offered_deadline_missed_status(writer, &status);
      return convert_status(status);
    }

    case Status::kOfferedIncompatibleQos: {
      dds_offered_incompatible_qos_status_t status;
      dds_get_offered_incompatible_qos_status(writer, &status);
      return convert_status(status);
    }

    case Status::kLivelinessLost: {
      dds_liveliness_lost_status_t status;
      dds_get_liveliness_lost_status(writer, &status);
      return convert_status(status);
    }

    default: {
      return std::make_shared<Status::Unknown>();
    }
  }
}

void DdscWriterListener::on_publication_matched(NodeImpl* impl, const dds_publication_matched_status_t& status) {
  (void)impl;
  (void)status;
}

// DdscReaderListener
DdscReaderListener::DdscReaderListener(NodeImpl* impl) : impl_(impl) {
  listener_ = dds_create_listener(this);

  dds_lset_subscription_matched(listener_, g_on_subscription_matched);
  dds_lset_requested_deadline_missed(listener_, g_on_requested_deadline_missed);
  dds_lset_liveliness_changed(listener_, g_on_liveliness_changed);
  dds_lset_sample_rejected(listener_, g_on_sample_rejected);
  dds_lset_requested_incompatible_qos(listener_, g_on_requested_incompatible_qos);
  dds_lset_sample_lost(listener_, g_on_sample_lost);
  dds_lset_data_available(listener_, g_on_data_available);
}

DdscReaderListener::~DdscReaderListener() { dds_delete_listener(listener_); }

Status::BasePtr DdscReaderListener::get_status(dds_entity_t reader, Status::Type type) {
  switch (type) {
    case Status::kSubscriptionMatched: {
      dds_subscription_matched_status_t status;
      dds_get_subscription_matched_status(reader, &status);
      return convert_status(status);
    }

    case Status::kRequestedDeadlineMissed: {
      dds_requested_deadline_missed_status_t status;
      dds_get_requested_deadline_missed_status(reader, &status);
      return convert_status(status);
    }

    case Status::kLivelinessChanged: {
      dds_liveliness_changed_status_t status;
      dds_get_liveliness_changed_status(reader, &status);
      return convert_status(status);
    }

    case Status::kSampleRejected: {
      dds_sample_rejected_status_t status;
      dds_get_sample_rejected_status(reader, &status);
      return convert_status(status);
    }

    case Status::kRequestedIncompatibleQos: {
      dds_requested_incompatible_qos_status_t status;
      dds_get_requested_incompatible_qos_status(reader, &status);
      return convert_status(status);
    }

    case Status::kSampleLost: {
      dds_sample_lost_status_t status;
      dds_get_sample_lost_status(reader, &status);
      return convert_status(status);
    }

    default: {
      return std::make_shared<Status::Unknown>();
    }
  }
}

void DdscReaderListener::on_subscription_matched(NodeImpl* impl, const dds_subscription_matched_status_t& status) {
  (void)impl;
  (void)status;
}

void DdscReaderListener::on_data_available(NodeImpl* impl, dds_entity_t reader) {
  (void)impl;
  (void)reader;
}

}  // namespace vlink
