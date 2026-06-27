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

#include "./extension/status_detail.h"

#include <string>

namespace vlink {

namespace Status {

// --- For writer

// PublicationMatched
Type PublicationMatched::get_type() const { return kPublicationMatched; }

std::string PublicationMatched::get_string() const { return "PublicationMatched"; }

std::ostream& operator<<(std::ostream& ostream, const PublicationMatched& status) noexcept {
  ostream << status.get_string() << ":";
  ostream << "[total_count]" << status.total_count;
  ostream << "[total_count_change]" << status.total_count_change;
  ostream << "[current_count]" << status.current_count;
  ostream << "[current_count_change]" << status.current_count_change;
  ostream << "[last_subscription_handle]" << status.last_subscription_handle;

  return ostream;
}

// OfferedDeadlineMissed
Type OfferedDeadlineMissed::get_type() const { return kOfferedDeadlineMissed; }

std::string OfferedDeadlineMissed::get_string() const { return "OfferedDeadlineMissed"; }

std::ostream& operator<<(std::ostream& ostream, const OfferedDeadlineMissed& status) noexcept {
  ostream << status.get_string() << ":";
  ostream << "[total_count]" << status.total_count;
  ostream << "[total_count_change]" << status.total_count_change;
  ostream << "[last_instance_handle]" << status.last_instance_handle;

  return ostream;
}

// OfferedIncompatibleQos
Type OfferedIncompatibleQos::get_type() const { return kOfferedIncompatibleQos; }

std::string OfferedIncompatibleQos::get_string() const { return "OfferedIncompatibleQos"; }

std::ostream& operator<<(std::ostream& ostream, const OfferedIncompatibleQos& status) noexcept {
  ostream << status.get_string() << ":";
  ostream << "[total_count]" << status.total_count;
  ostream << "[total_count_change]" << status.total_count_change;
  ostream << "[last_policy_id]" << status.last_policy_id;

  return ostream;
}

// LivelinessLost
Type LivelinessLost::get_type() const { return kLivelinessLost; }

std::string LivelinessLost::get_string() const { return "LivelinessLost"; }

std::ostream& operator<<(std::ostream& ostream, const LivelinessLost& status) noexcept {
  ostream << status.get_string() << ":";
  ostream << "[total_count]" << status.total_count;
  ostream << "[total_count_change]" << status.total_count_change;

  return ostream;
}

// --- For reader

// SubscriptionMatched
Type SubscriptionMatched::get_type() const { return kSubscriptionMatched; }

std::string SubscriptionMatched::get_string() const { return "SubscriptionMatched"; }

std::ostream& operator<<(std::ostream& ostream, const SubscriptionMatched& status) noexcept {
  ostream << status.get_string() << ":";
  ostream << "[total_count]" << status.total_count;
  ostream << "[total_count_change]" << status.total_count_change;
  ostream << "[current_count]" << status.current_count;
  ostream << "[current_count_change]" << status.current_count_change;
  ostream << "[last_publication_handle]" << status.last_publication_handle;

  return ostream;
}

// RequestedDeadlineMissed
Type RequestedDeadlineMissed::get_type() const { return kRequestedDeadlineMissed; }

std::string RequestedDeadlineMissed::get_string() const { return "RequestedDeadlineMissed"; }

std::ostream& operator<<(std::ostream& ostream, const RequestedDeadlineMissed& status) noexcept {
  ostream << status.get_string() << ":";
  ostream << "[total_count]" << status.total_count;
  ostream << "[total_count_change]" << status.total_count_change;
  ostream << "[last_instance_handle]" << status.last_instance_handle;

  return ostream;
}

// LivelinessChanged
Type LivelinessChanged::get_type() const { return kLivelinessChanged; }

std::string LivelinessChanged::get_string() const { return "LivelinessChanged"; }

std::ostream& operator<<(std::ostream& ostream, const LivelinessChanged& status) noexcept {
  ostream << status.get_string() << ":";
  ostream << "[alive_count]" << status.alive_count;
  ostream << "[not_alive_count]" << status.not_alive_count;
  ostream << "[alive_count_change]" << status.alive_count_change;
  ostream << "[not_alive_count_change]" << status.not_alive_count_change;
  ostream << "[last_publication_handle]" << status.last_publication_handle;

  return ostream;
}

// SampleRejected
Type SampleRejected::get_type() const { return kSampleRejected; }

std::string SampleRejected::get_string() const { return "SampleRejected"; }

std::ostream& operator<<(std::ostream& ostream, const SampleRejected& status) noexcept {
  ostream << status.get_string() << ":";
  ostream << "[total_count]" << status.total_count;
  ostream << "[total_count_change]" << status.total_count_change;
  ostream << "[last_reason]" << +status.last_reason;
  ostream << "[last_instance_handle]" << status.last_instance_handle;

  return ostream;
}

// RequestedIncompatibleQos
Type RequestedIncompatibleQos::get_type() const { return kRequestedIncompatibleQos; }

std::string RequestedIncompatibleQos::get_string() const { return "RequestedIncompatibleQos"; }

std::ostream& operator<<(std::ostream& ostream, const RequestedIncompatibleQos& status) noexcept {
  ostream << status.get_string() << ":";
  ostream << "[total_count]" << status.total_count;
  ostream << "[total_count_change]" << status.total_count_change;
  ostream << "[last_policy_id]" << status.last_policy_id;

  return ostream;
}

// SampleLost
Type SampleLost::get_type() const { return kSampleLost; }

std::string SampleLost::get_string() const { return "SampleLost"; }

std::ostream& operator<<(std::ostream& ostream, const SampleLost& status) noexcept {
  ostream << status.get_string() << ":";
  ostream << "[total_count]" << status.total_count;
  ostream << "[total_count_change]" << status.total_count_change;

  return ostream;
}

//

}  // namespace Status

}  // namespace vlink
