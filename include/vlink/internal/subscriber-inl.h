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
#include "../serializer.h"
#include "../subscriber.h"

namespace vlink {

template <typename MsgT, SecurityType SecT>
inline typename Subscriber<MsgT, SecT>::UniquePtr Subscriber<MsgT, SecT>::create_unique(const std::string& url_str,
                                                                                        InitType type) {
  return std::make_unique<Subscriber<MsgT, SecT>>(url_str, type);
}

template <typename MsgT, SecurityType SecT>
inline typename Subscriber<MsgT, SecT>::SharedPtr Subscriber<MsgT, SecT>::create_shared(const std::string& url_str,
                                                                                        InitType type) {
  return std::make_shared<Subscriber<MsgT, SecT>>(url_str, type);
}

template <typename MsgT, SecurityType SecT>
template <typename ConfT, typename>
inline Subscriber<MsgT, SecT>::Subscriber(const ConfT& conf, InitType type) {
  static_assert(ConfT::get_allow_impl_type() & kImplType, "Conf does not support subscriber mode.");

  if VUNLIKELY (!conf.parse(kImplType) || !conf.is_valid()) {
    VLOG_F(conf, " subscriber configuration is invalid or could not be parsed.");
    return;
  }

  this->impl_ = conf.create_subscriber();

  if VUNLIKELY (!this->impl_) {
    VLOG_F(conf, " subscriber implementation not available for this transport.");
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
inline Subscriber<MsgT, SecT>::Subscriber(const std::string& url_str, InitType type)
    : Subscriber<MsgT, SecT>(Url(url_str), type) {}

template <typename MsgT, SecurityType SecT>
inline bool Subscriber<MsgT, SecT>::listen(MsgCallback&& callback) {
  if constexpr (Traits::IsSharedPtr<MsgT>()) {
    if constexpr (std::is_base_of_v<IntraDataType, typename MsgT::element_type>) {
      static_assert(SecT != SecurityType::kWithSecurity, "IntraData must without security.");

      if (this->impl_->transport_type == TransportType::kIntra) {
        return listen_intra(std::move(callback));
      }
    }
  }

  return listen_bytes([this, callback = std::move(callback)](const Bytes& data) {
#ifndef VLINK_DISABLE_PROFILER
    CpuProfilerGuard profiler_guard(this->impl_->profiler.get());
#endif

    if constexpr (std::is_same_v<MsgT, Bytes>) {
      (void)this;

      callback(data);
    } else {
      thread_local auto msg = this->template get_default_value<MsgT>();

      if VUNLIKELY (!Serializer::deserialize<kMsgType>(data, msg, this->impl_->transport_type)) {
        VLOG_T("Subscriber deserialize failed, url: ", this->impl_->url, ".");
        return;
      }

      callback(msg);
    }
  });
}

template <typename MsgT, SecurityType SecT>
inline void Subscriber<MsgT, SecT>::set_manual_unloan(bool manual_unloan) {
  this->impl_->set_manual_unloan(manual_unloan);
  this->is_manual_unloan_ = manual_unloan;
}

template <typename MsgT, SecurityType SecT>
inline void Subscriber<MsgT, SecT>::set_latency_and_lost_enabled(bool enable) {
  this->impl_->set_latency_and_lost_enabled(enable);
}

template <typename MsgT, SecurityType SecT>
inline bool Subscriber<MsgT, SecT>::is_latency_and_lost_enabled() const {
  return this->impl_->is_latency_and_lost_enabled();
}

template <typename MsgT, SecurityType SecT>
inline int64_t Subscriber<MsgT, SecT>::get_latency() const {
  return this->impl_->get_latency();
}

template <typename MsgT, SecurityType SecT>
inline SampleLostInfo Subscriber<MsgT, SecT>::get_lost() const {
  return this->impl_->get_lost();
}

template <typename MsgT, SecurityType SecT>
inline void Subscriber<MsgT, SecT>::mark_as_getter() {
  if VUNLIKELY (this->has_inited_) {
    this->impl_->deinit_ext();
    this->impl_->impl_type = kGetter;
    this->impl_->init_ext();
  } else {
    this->impl_->impl_type = kGetter;
  }
}

template <typename MsgT, SecurityType SecT>
inline bool Subscriber<MsgT, SecT>::listen_bytes(NodeImpl::MsgCallback&& callback) {
  if VUNLIKELY (!this->has_inited_) {
    VLOG_F("Subscriber::listen_bytes() called before init().");
    return false;
  }

  if VUNLIKELY (this->impl_->is_listened) {
    VLOG_F("Subscriber has already been listened, url: ", this->impl_->url, ".");
    return false;
  }

  bool ret = this->impl_->listen([this, callback = std::move(callback)](const Bytes& data) {
    if constexpr (SecT == SecurityType::kWithSecurity) {
      Bytes sec_data;

      if VUNLIKELY (!this->impl_->security || !this->impl_->security->decrypt(data, sec_data)) {
        VLOG_T("Subscriber decrypt failed, url: ", this->impl_->url, ".");
        return;
      }

      this->invoke_callback(callback, sec_data);
    } else {
      this->impl_->try_record(ActionType::kSubscribe, data);

      this->invoke_callback(callback, data);
    }
  });

  this->impl_->is_listened = ret;

  return ret;
}

template <typename MsgT, SecurityType SecT>
inline bool Subscriber<MsgT, SecT>::listen_intra(MsgCallback&& callback) {
  if VUNLIKELY (!this->has_inited_) {
    VLOG_F("Subscriber::listen_intra() called before init().");
    return false;
  }

  if VUNLIKELY (this->impl_->is_listened) {
    VLOG_F("Subscriber has already been listened, url: ", this->impl_->url, ".");
    return false;
  }

  bool ret = this->impl_->listen([this, callback = std::move(callback)](const IntraData& intra_data) {
#ifndef VLINK_DISABLE_PROFILER
    CpuProfilerGuard profiler_guard(this->impl_->profiler.get());
#endif

    if constexpr (Traits::IsSharedPtr<MsgT>()) {
#if defined(NDEBUG) || defined(__ANDROID__)
      auto intra_msg = std::static_pointer_cast<typename MsgT::element_type>(intra_data);
#else
      auto intra_msg = std::dynamic_pointer_cast<typename MsgT::element_type>(intra_data);
#endif

      if VLIKELY (intra_msg) {
        MsgT typed_msg(std::move(intra_msg));
        this->invoke_callback(callback, typed_msg);
      } else {
        VLOG_T("Subscriber get intra data failed, url: ", this->impl_->url, ".");
      }
    }
  });

  this->impl_->is_listened = ret;

  return ret;
}

template <typename MsgT>
template <typename SecurityConfigT>
inline typename SecuritySubscriber<MsgT>::UniquePtr SecuritySubscriber<MsgT>::create_unique(const std::string& url_str,
                                                                                            SecurityConfigT&& sec_cfg,
                                                                                            InitType type) {
  static_assert(std::is_same_v<std::decay_t<SecurityConfigT>, Security::Config>,
                "SecurityConfigT must be Security::Config.");

  return std::make_unique<SecuritySubscriber<MsgT>>(url_str, std::forward<SecurityConfigT>(sec_cfg), type);
}

template <typename MsgT>
template <typename SecurityConfigT>
inline typename SecuritySubscriber<MsgT>::SharedPtr SecuritySubscriber<MsgT>::create_shared(const std::string& url_str,
                                                                                            SecurityConfigT&& sec_cfg,
                                                                                            InitType type) {
  static_assert(std::is_same_v<std::decay_t<SecurityConfigT>, Security::Config>,
                "SecurityConfigT must be Security::Config.");

  return std::make_shared<SecuritySubscriber<MsgT>>(url_str, std::forward<SecurityConfigT>(sec_cfg), type);
}

template <typename MsgT>
template <typename ConfT, typename SecurityConfigT, typename>
inline SecuritySubscriber<MsgT>::SecuritySubscriber(const ConfT& conf, SecurityConfigT&& sec_cfg, InitType type)
    : Subscriber<MsgT, SecurityType::kWithSecurity>(conf, InitType::kWithoutInit) {
  static_assert(std::is_same_v<std::decay_t<SecurityConfigT>, Security::Config>,
                "SecurityConfigT must be Security::Config.");

  this->enable_security(std::forward<SecurityConfigT>(sec_cfg));

  if VLIKELY (type == InitType::kWithInit) {
    this->init();  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
  }
}

template <typename MsgT>
template <typename SecurityConfigT>
inline SecuritySubscriber<MsgT>::SecuritySubscriber(const std::string& url_str, SecurityConfigT&& sec_cfg,
                                                    InitType type)
    : Subscriber<MsgT, SecurityType::kWithSecurity>(url_str, InitType::kWithoutInit) {
  static_assert(std::is_same_v<std::decay_t<SecurityConfigT>, Security::Config>,
                "SecurityConfigT must be Security::Config.");

  this->enable_security(std::forward<SecurityConfigT>(sec_cfg));

  if VLIKELY (type == InitType::kWithInit) {
    this->init();  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
  }
}

}  // namespace vlink
