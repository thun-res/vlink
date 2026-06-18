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

#include "./extension/status.h"

#include <string>

#include "./extension/status_detail.h"

namespace vlink {

namespace Status {

template <typename T>
static void write_if_type(std::ostream& ostream, const Base& status) noexcept {
#if defined(NDEBUG) || defined(__ANDROID__)
  const auto* target = static_cast<const T*>(&status);
#else
  const auto* target = dynamic_cast<const T*>(&status);
#endif

  if VLIKELY (target) {
    ostream << *target;
    return;
  }

  ostream << "Unknown";
}

// Base
Base::Base() = default;

Base::~Base() = default;

std::ostream& operator<<(std::ostream& ostream, const Base& status) noexcept {
  try {
    switch (status.get_type()) {
      case kUnknown:
        ostream << "Unknown";
        break;
      case kPublicationMatched:
        write_if_type<PublicationMatched>(ostream, status);
        break;
      case kOfferedDeadlineMissed:
        write_if_type<OfferedDeadlineMissed>(ostream, status);
        break;
      case kOfferedIncompatibleQos:
        write_if_type<OfferedIncompatibleQos>(ostream, status);
        break;
      case kLivelinessLost:
        write_if_type<LivelinessLost>(ostream, status);
        break;
      case kSubscriptionMatched:
        write_if_type<SubscriptionMatched>(ostream, status);
        break;
      case kRequestedDeadlineMissed:
        write_if_type<RequestedDeadlineMissed>(ostream, status);
        break;
      case kLivelinessChanged:
        write_if_type<LivelinessChanged>(ostream, status);
        break;
      case kSampleRejected:
        write_if_type<SampleRejected>(ostream, status);
        break;
      case kRequestedIncompatibleQos:
        write_if_type<RequestedIncompatibleQos>(ostream, status);
        break;
      case kSampleLost:
        write_if_type<SampleLost>(ostream, status);
        break;
      default:
        ostream << "Unknown";
        break;
    }
  } catch (std::exception&) {
    ostream << "Unknown";
  }

  return ostream;
}

// Unknown
Type Unknown::get_type() const { return kUnknown; }

std::string Unknown::get_string() const { return "Unknown"; }

std::ostream& operator<<(std::ostream& ostream, const Unknown& status) noexcept {
  ostream << status.get_string();

  return ostream;
}

// BasePtr
std::ostream& operator<<(std::ostream& ostream, const BasePtr& status) noexcept {
  if VUNLIKELY (!status) {
    ostream << "null";
    return ostream;
  }

  ostream << *status;

  return ostream;
}

}  // namespace Status

}  // namespace vlink
