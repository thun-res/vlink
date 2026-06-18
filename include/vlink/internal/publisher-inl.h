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

#include <memory>
#include <string>
#include <utility>

#include "../base/cpu_profiler_guard.h"
#include "../base/logger.h"
#include "../impl/url.h"
#include "../publisher.h"
#include "../serializer.h"

namespace vlink {

template <typename MsgT, SecurityType SecT>
inline typename Publisher<MsgT, SecT>::UniquePtr Publisher<MsgT, SecT>::create_unique(const std::string& url_str,
                                                                                      InitType type) {
  return std::make_unique<Publisher<MsgT, SecT>>(url_str, type);
}

template <typename MsgT, SecurityType SecT>
inline typename Publisher<MsgT, SecT>::SharedPtr Publisher<MsgT, SecT>::create_shared(const std::string& url_str,
                                                                                      InitType type) {
  return std::make_shared<Publisher<MsgT, SecT>>(url_str, type);
}

template <typename MsgT, SecurityType SecT>
template <typename ConfT, typename>
inline Publisher<MsgT, SecT>::Publisher(const ConfT& conf, InitType type) {
  static_assert(ConfT::get_allow_impl_type() & kImplType, "Conf does not support publisher mode.");

  if VUNLIKELY (!conf.parse(kImplType) || !conf.is_valid()) {
    VLOG_F(conf, " publisher configuration is invalid or could not be parsed.");
    return;
  }

  this->impl_ = conf.create_publisher();

  if VUNLIKELY (!this->impl_) {
    VLOG_F(conf, " publisher implementation not available for this transport.");
    return;
  }

  this->impl_->transport_type = conf.get_transport_type();
  this->impl_->ser_type = Serializer::get_serialized_type<kMsgType, MsgT>();
  this->impl_->schema_type = Serializer::get_schema_type<kMsgType, MsgT>();
  this->impl_->is_cdr_type = Serializer::is_cdr_type<MsgT>();

  if constexpr (std::is_same_v<ConfT, Url>) {
    this->impl_->url = conf.get_str();
  }

  if constexpr (SecT == SecurityType::kWithSecurity) {
    this->impl_->is_security_type = true;
  }

  if VLIKELY (type == InitType::kWithInit) {
    this->init();  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
  }
}

template <typename MsgT, SecurityType SecT>
inline Publisher<MsgT, SecT>::Publisher(const std::string& url_str, InitType type)
    : Publisher<MsgT, SecT>(Url(url_str), type) {}

template <typename MsgT, SecurityType SecT>
inline void Publisher<MsgT, SecT>::detect_subscribers(ConnectCallback&& callback) {
  return this->impl_->detect_subscribers(std::move(callback));
}

template <typename MsgT, SecurityType SecT>
inline bool Publisher<MsgT, SecT>::wait_for_subscribers(std::chrono::milliseconds timeout) {
  if VUNLIKELY (timeout.count() == 0) {
    VLOG_W("Publisher: Timeout value is 0, using infinite wait instead.");
    timeout = Timeout::kInfinite;
  }

  return this->impl_->wait_for_subscribers(timeout);
}

template <typename MsgT, SecurityType SecT>
inline bool Publisher<MsgT, SecT>::has_subscribers() const {
  return this->impl_->has_subscribers();
}

template <typename MsgT, SecurityType SecT>
inline bool Publisher<MsgT, SecT>::publish(const MsgT& msg, bool force) {
#ifndef VLINK_DISABLE_PROFILER
  CpuProfilerGuard profiler_guard(this->impl_->profiler.get());
#endif

  if (!force) {
    if (!this->impl_->has_subscribers()) {
      return false;
    }
  }

  if constexpr (Traits::IsSharedPtr<MsgT>()) {
    if constexpr (std::is_base_of_v<IntraDataType, typename MsgT::element_type>) {
      static_assert(SecT != SecurityType::kWithSecurity, "IntraData must without security.");

      if (this->impl_->transport_type == TransportType::kIntra) {
        return write_intra(msg);
      }
    }
  }

  if constexpr (std::is_same_v<MsgT, Bytes>) {
    return write_bytes(msg);
  } else {
    Bytes msg_data;

    if constexpr (SecT != SecurityType::kWithSecurity) {
      if (this->is_support_loan_) {
        size_t ser_size = Serializer::get_serialized_size<kMsgType>(msg);

        msg_data = this->impl_->loan(ser_size);

        if VUNLIKELY (ser_size != 0 && msg_data.empty()) {
          return false;
        }
      }
    }

    if VUNLIKELY (!Serializer::serialize<kMsgType>(msg, msg_data, this->impl_->transport_type)) {
      VLOG_T("Publisher serialize failed, url: ", this->impl_->url, ".");

      if constexpr (SecT != SecurityType::kWithSecurity) {
        if (this->is_support_loan_) {
          this->impl_->return_loan(msg_data);
        }
      }

      return false;
    }

    bool ret = write_bytes(msg_data);

    return ret;
  }
}

template <typename MsgT, SecurityType SecT>
bool Publisher<MsgT, SecT>::publish_fbb(const void* fbb, bool force) {
  const auto* fbb_ptr = static_cast<const flatbuffers::FlatBufferBuilder*>(fbb);

#ifndef VLINK_DISABLE_PROFILER
  CpuProfilerGuard profiler_guard(this->impl_->profiler.get());
#endif

  if (!force) {
    if (!this->impl_->has_subscribers()) {
      return false;
    }
  }

  return write_bytes(Bytes::shallow_copy(fbb_ptr->GetBufferPointer(), fbb_ptr->GetSize()));
}

template <typename MsgT, SecurityType SecT>
inline void Publisher<MsgT, SecT>::mark_as_setter() {
  if VUNLIKELY (this->has_inited_) {
    this->impl_->deinit_ext();
    this->impl_->impl_type = kSetter;
    this->impl_->init_ext();
  } else {
    this->impl_->impl_type = kSetter;
  }
}

template <typename MsgT, SecurityType SecT>
inline bool Publisher<MsgT, SecT>::write_bytes(const Bytes& data) {
  if constexpr (SecT == SecurityType::kWithSecurity) {
    Bytes sec_data;

    if VUNLIKELY (!this->impl_->security || !this->impl_->security->encrypt(data, sec_data)) {
      VLOG_T("Publisher encrypt failed, url: ", this->impl_->url, ".");
      return false;
    }

    return this->impl_->write(sec_data);
  } else {
    this->impl_->try_record(ActionType::kPublish, data);

    return this->impl_->write(data);
  }
}

template <typename MsgT, SecurityType SecT>
inline bool Publisher<MsgT, SecT>::write_intra(const IntraData& intra_data) {
  return this->impl_->write(intra_data);
}

template <typename MsgT>
template <typename SecurityConfigT>
inline typename SecurityPublisher<MsgT>::UniquePtr SecurityPublisher<MsgT>::create_unique(const std::string& url_str,
                                                                                          SecurityConfigT&& sec_cfg,
                                                                                          InitType type) {
  static_assert(std::is_same_v<std::decay_t<SecurityConfigT>, Security::Config>,
                "SecurityConfigT must be Security::Config.");

  return std::make_unique<SecurityPublisher<MsgT>>(url_str, std::forward<SecurityConfigT>(sec_cfg), type);
}

template <typename MsgT>
template <typename SecurityConfigT>
inline typename SecurityPublisher<MsgT>::SharedPtr SecurityPublisher<MsgT>::create_shared(const std::string& url_str,
                                                                                          SecurityConfigT&& sec_cfg,
                                                                                          InitType type) {
  static_assert(std::is_same_v<std::decay_t<SecurityConfigT>, Security::Config>,
                "SecurityConfigT must be Security::Config.");

  return std::make_shared<SecurityPublisher<MsgT>>(url_str, std::forward<SecurityConfigT>(sec_cfg), type);
}

template <typename MsgT>
template <typename ConfT, typename SecurityConfigT, typename>
inline SecurityPublisher<MsgT>::SecurityPublisher(const ConfT& conf, SecurityConfigT&& sec_cfg, InitType type)
    : Publisher<MsgT, SecurityType::kWithSecurity>(conf, InitType::kWithoutInit) {
  static_assert(std::is_same_v<std::decay_t<SecurityConfigT>, Security::Config>,
                "SecurityConfigT must be Security::Config.");

  this->enable_security(std::forward<SecurityConfigT>(sec_cfg));

  if VLIKELY (type == InitType::kWithInit) {
    this->init();  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
  }
}

template <typename MsgT>
template <typename SecurityConfigT>
inline SecurityPublisher<MsgT>::SecurityPublisher(const std::string& url_str, SecurityConfigT&& sec_cfg, InitType type)
    : Publisher<MsgT, SecurityType::kWithSecurity>(url_str, InitType::kWithoutInit) {
  static_assert(std::is_same_v<std::decay_t<SecurityConfigT>, Security::Config>,
                "SecurityConfigT must be Security::Config.");

  this->enable_security(std::forward<SecurityConfigT>(sec_cfg));

  if VLIKELY (type == InitType::kWithInit) {
    this->init();  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
  }
}

}  // namespace vlink
