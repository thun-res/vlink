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
#include "../setter.h"

namespace vlink {

template <typename ValueT, SecurityType SecT>
inline typename Setter<ValueT, SecT>::UniquePtr Setter<ValueT, SecT>::create_unique(const std::string& url_str,
                                                                                    InitType type) {
  return std::make_unique<Setter<ValueT, SecT>>(url_str, type);
}

template <typename ValueT, SecurityType SecT>
inline typename Setter<ValueT, SecT>::SharedPtr Setter<ValueT, SecT>::create_shared(const std::string& url_str,
                                                                                    InitType type) {
  return std::make_shared<Setter<ValueT, SecT>>(url_str, type);
}

template <typename ValueT, SecurityType SecT>
template <typename ConfT, typename>
inline Setter<ValueT, SecT>::Setter(const ConfT& conf, InitType type) {
  static_assert(ConfT::get_allow_impl_type() & kImplType, "Conf does not support setter mode.");

  if VUNLIKELY (!conf.parse(kImplType) || !conf.is_valid()) {
    VLOG_F(conf, " setter configuration is invalid or could not be parsed.");
    return;
  }

  this->impl_ = conf.create_setter();

  if VUNLIKELY (!this->impl_) {
    VLOG_F(conf, " setter implementation not available for this transport.");
    return;
  }

  this->impl_->transport_type = conf.get_transport_type();
  this->impl_->ser_type = Serializer::get_serialized_type<kValueType, ValueT>();
  this->impl_->schema_type = Serializer::get_schema_type<kValueType, ValueT>();
  this->impl_->is_cdr_type = Serializer::is_cdr_type<ValueT>();

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

template <typename ValueT, SecurityType SecT>
inline Setter<ValueT, SecT>::Setter(const std::string& url_str, InitType type)
    : Setter<ValueT, SecT>(Url(url_str), type) {}

template <typename ValueT, SecurityType SecT>
inline Setter<ValueT, SecT>::~Setter() {
  // NOLINTNEXTLINE(clang-analyzer-optin.cplusplus.VirtualCall)
  this->deinit();
}

template <typename ValueT, SecurityType SecT>
inline bool Setter<ValueT, SecT>::init() {
  if VUNLIKELY ((!Node<SetterImpl, SecT>::init())) {
    return false;
  }

  this->impl_->sync([this]() {
    std::optional<ValueT> snapshot;
    {
      std::lock_guard lock(mtx_);
      snapshot = value_;
    }

    if VLIKELY (snapshot.has_value()) {
      write(snapshot.value());
    }
  });

  return true;
}

template <typename ValueT, SecurityType SecT>
inline bool Setter<ValueT, SecT>::deinit() {
  return Node<SetterImpl, SecT>::deinit();
}

template <typename ValueT, SecurityType SecT>
inline void Setter<ValueT, SecT>::set(const ValueT& value) {
#ifndef VLINK_DISABLE_PROFILER
  CpuProfilerGuard profiler_guard(this->impl_->profiler.get());
#endif

  {
    std::lock_guard lock(mtx_);
    value_.emplace(value);
  }

  write(value);
}

template <typename ValueT, SecurityType SecT>
inline void Setter<ValueT, SecT>::mark_as_publisher() {
  if VUNLIKELY (this->has_inited_) {
    this->impl_->deinit_ext();
    this->impl_->impl_type = kPublisher;
    this->impl_->init_ext();
  } else {
    this->impl_->impl_type = kPublisher;
  }
}

template <typename ValueT, SecurityType SecT>
inline void Setter<ValueT, SecT>::write(const ValueT& value) {
  if constexpr (std::is_same_v<ValueT, Bytes>) {
    write_bytes(value);
  } else {
    Bytes msg_data;

    if constexpr (SecT != SecurityType::kWithSecurity) {
      if (this->is_support_loan_) {
        size_t ser_size = Serializer::get_serialized_size<kValueType>(value);

        msg_data = this->impl_->loan(ser_size);

        if VUNLIKELY (ser_size != 0 && msg_data.empty()) {
          return;
        }
      }
    }

    if VUNLIKELY (!Serializer::serialize<kValueType>(value, msg_data, this->impl_->transport_type)) {
      VLOG_T("Setter serialize failed, url: ", this->impl_->url, ".");

      if constexpr (SecT != SecurityType::kWithSecurity) {
        if (this->is_support_loan_) {
          this->impl_->return_loan(msg_data);
        }
      }

      return;
    }

    write_bytes(msg_data);
  }
}

template <typename ValueT, SecurityType SecT>
inline void Setter<ValueT, SecT>::write_bytes(const Bytes& data) {
  if constexpr (SecT == SecurityType::kWithSecurity) {
    Bytes sec_data;

    if VUNLIKELY (!this->impl_->security || !this->impl_->security->encrypt(data, sec_data)) {
      VLOG_T("Setter encrypt failed, url: ", this->impl_->url, ".");
      return;
    }

    this->impl_->write(sec_data);
  } else {
    this->impl_->try_record(ActionType::kSet, data);

    this->impl_->write(data);
  }
}

template <typename ValueT>
template <typename SecurityConfigT>
inline typename SecuritySetter<ValueT>::UniquePtr SecuritySetter<ValueT>::create_unique(const std::string& url_str,
                                                                                        SecurityConfigT&& sec_cfg,
                                                                                        InitType type) {
  static_assert(std::is_same_v<std::decay_t<SecurityConfigT>, Security::Config>,
                "SecurityConfigT must be Security::Config.");

  return std::make_unique<SecuritySetter<ValueT>>(url_str, std::forward<SecurityConfigT>(sec_cfg), type);
}

template <typename ValueT>
template <typename SecurityConfigT>
inline typename SecuritySetter<ValueT>::SharedPtr SecuritySetter<ValueT>::create_shared(const std::string& url_str,
                                                                                        SecurityConfigT&& sec_cfg,
                                                                                        InitType type) {
  static_assert(std::is_same_v<std::decay_t<SecurityConfigT>, Security::Config>,
                "SecurityConfigT must be Security::Config.");

  return std::make_shared<SecuritySetter<ValueT>>(url_str, std::forward<SecurityConfigT>(sec_cfg), type);
}

template <typename ValueT>
template <typename ConfT, typename SecurityConfigT, typename>
inline SecuritySetter<ValueT>::SecuritySetter(const ConfT& conf, SecurityConfigT&& sec_cfg, InitType type)
    : Setter<ValueT, SecurityType::kWithSecurity>(conf, InitType::kWithoutInit) {
  static_assert(std::is_same_v<std::decay_t<SecurityConfigT>, Security::Config>,
                "SecurityConfigT must be Security::Config.");

  this->enable_security(std::forward<SecurityConfigT>(sec_cfg));

  if VLIKELY (type == InitType::kWithInit) {
    this->init();  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
  }
}

template <typename ValueT>
template <typename SecurityConfigT>
inline SecuritySetter<ValueT>::SecuritySetter(const std::string& url_str, SecurityConfigT&& sec_cfg, InitType type)
    : Setter<ValueT, SecurityType::kWithSecurity>(url_str, InitType::kWithoutInit) {
  static_assert(std::is_same_v<std::decay_t<SecurityConfigT>, Security::Config>,
                "SecurityConfigT must be Security::Config.");

  this->enable_security(std::forward<SecurityConfigT>(sec_cfg));

  if VLIKELY (type == InitType::kWithInit) {
    this->init();  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
  }
}

}  // namespace vlink
