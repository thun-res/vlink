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
#include "../getter.h"
#include "../impl/url.h"
#include "../serializer.h"

namespace vlink {

template <typename ValueT, SecurityType SecT>
inline typename Getter<ValueT, SecT>::UniquePtr Getter<ValueT, SecT>::create_unique(const std::string& url_str,
                                                                                    InitType type) {
  return std::make_unique<Getter<ValueT, SecT>>(url_str, type);
}

template <typename ValueT, SecurityType SecT>
inline typename Getter<ValueT, SecT>::SharedPtr Getter<ValueT, SecT>::create_shared(const std::string& url_str,
                                                                                    InitType type) {
  return std::make_shared<Getter<ValueT, SecT>>(url_str, type);
}

template <typename ValueT, SecurityType SecT>
template <typename ConfT, typename>
inline Getter<ValueT, SecT>::Getter(const ConfT& conf, InitType type) {
  static_assert(ConfT::get_allow_impl_type() & kImplType, "Conf does not support getter mode.");

  if VUNLIKELY (!conf.parse(kImplType) || !conf.is_valid()) {
    VLOG_F(conf, " getter configuration is invalid or could not be parsed.");
    return;
  }

  this->impl_ = conf.create_getter();

  if VUNLIKELY (!this->impl_) {
    VLOG_F(conf, " getter implementation not available for this transport.");
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
inline Getter<ValueT, SecT>::Getter(const std::string& url_str, InitType type)
    : Getter<ValueT, SecT>(Url(url_str), type) {}

template <typename ValueT, SecurityType SecT>
inline Getter<ValueT, SecT>::~Getter() {
  // NOLINTNEXTLINE(clang-analyzer-optin.cplusplus.VirtualCall)
  this->deinit();
}

template <typename ValueT, SecurityType SecT>
inline Getter<ValueT, SecT>::PendingDelivery::PendingDelivery(PendingDelivery*& head,
                                                              std::unique_lock<std::mutex>& lock, const Bytes& data)
    : head(head), lock(lock), data(data), previous(head) {
  head = this;
}

template <typename ValueT, SecurityType SecT>
inline Getter<ValueT, SecT>::PendingDelivery::~PendingDelivery() {
  if (!lock.owns_lock()) {
    lock.lock();
  }

  auto** link = &head;

  while (*link && *link != this) {
    link = &(*link)->previous;
  }

  if (*link) {
    *link = committed ? nullptr : previous;
  }
}

template <typename ValueT, SecurityType SecT>
inline std::optional<ValueT> Getter<ValueT, SecT>::get() const {
  std::lock_guard lock(mtx_);

  if constexpr (kInlineValue) {
    return value_;
  } else {
    return value_ ? *value_ : std::nullopt;
  }
}

template <typename ValueT, SecurityType SecT>
inline bool Getter<ValueT, SecT>::wait_for_value(std::chrono::milliseconds timeout) {
  if VUNLIKELY (timeout.count() == 0) {
    VLOG_W("Getter: Timeout value is 0, using infinite wait instead.");
    timeout = Timeout::kInfinite;
  }

  const auto generation = this->impl_->get_interrupt_generation();
  std::unique_lock lock(mtx_);

  auto interrupted = [&]() { return this->impl_->get_interrupt_generation() != generation; };

  if VUNLIKELY (interrupted()) {
    return false;
  }

  this->impl_->reset_interrupted();

  if (value_) {
    return true;
  }

  has_value_notification_ = false;

  auto predicate = [&]() { return has_value_notification_ || interrupted(); };

  if (timeout.count() < 0) {
    cv_.wait(lock, std::move(predicate));
    return !interrupted();
  } else {
    return cv_.wait_for(lock, timeout, std::move(predicate)) && !interrupted();
  }
}

template <typename ValueT, SecurityType SecT>
inline bool Getter<ValueT, SecT>::listen(MsgCallback&& callback) {
  std::lock_guard lock(mtx_);

  if VUNLIKELY (this->impl_->is_listened) {
    VLOG_F("Getter has already been listened.");
    return false;
  }

  callback_ = std::move(callback);

  this->impl_->is_listened = true;

  return true;
}

template <typename ValueT, SecurityType SecT>
inline void Getter<ValueT, SecT>::set_change_reporting(bool enable) {
  std::lock_guard lock(mtx_);

  if (change_reporting_ != enable) {
    last_cache_.reset();
    value_is_basis_ = false;
    pending_delivery_ = nullptr;
    ++change_generation_;
  }

  change_reporting_ = enable;
}

template <typename ValueT, SecurityType SecT>
inline void Getter<ValueT, SecT>::set_latency_and_lost_enabled(bool enable) {
  this->impl_->set_latency_and_lost_enabled(enable);
}

template <typename ValueT, SecurityType SecT>
inline bool Getter<ValueT, SecT>::is_latency_and_lost_enabled() const {
  return this->impl_->is_latency_and_lost_enabled();
}

template <typename ValueT, SecurityType SecT>
inline int64_t Getter<ValueT, SecT>::get_latency() const {
  return this->impl_->get_latency();
}

template <typename ValueT, SecurityType SecT>
inline SampleLostInfo Getter<ValueT, SecT>::get_lost() const {
  return this->impl_->get_lost();
}

template <typename ValueT, SecurityType SecT>
inline bool Getter<ValueT, SecT>::get_change_reporting() const {
  std::lock_guard lock(mtx_);
  return change_reporting_;
}

template <typename ValueT, SecurityType SecT>
inline bool Getter<ValueT, SecT>::init() {
  if VUNLIKELY ((!Node<GetterImpl, SecT>::init())) {
    return false;
  }

  const bool listened = listen_bytes([this](const Bytes& data) {
#ifndef VLINK_DISABLE_PROFILER
    CpuProfilerGuard profiler_guard(this->impl_->profiler.get());
#endif

    const auto is_duplicate = [this, &data]() {
      if (pending_delivery_) {
        return pending_delivery_->data == data;
      }

      if constexpr (std::is_same_v<ValueT, Bytes>) {
        if (value_is_basis_) {
          return value_ == data;
        }
      }

      return last_cache_ == data;
    };

    std::unique_lock lock(mtx_);
    const bool change_reporting = change_reporting_;
    const auto generation = change_generation_;
    const bool has_callback = static_cast<bool>(callback_);

    if (change_reporting && is_duplicate()) {
      return;
    }

    auto deliver = [this, &data, &is_duplicate, change_reporting, generation, has_callback](
                       auto& value, std::unique_lock<std::mutex> delivery_lock) {
      std::optional<Bytes> snapshot;
      const auto copy_snapshot = [&delivery_lock, &snapshot, &data]() {
        if (delivery_lock.owns_lock()) {
          delivery_lock.unlock();
        }

        snapshot.emplace(data);
        return snapshot->real_size() == data.real_size();
      };

      const auto wire_size = data.real_size();
      std::optional<PendingDelivery> pending;

      if (has_callback && (std::is_same_v<ValueT, Bytes> || change_reporting)) {
        if VUNLIKELY (!copy_snapshot()) {
          return;
        }

        if (change_reporting) {
          delivery_lock.lock();

          if (generation == change_generation_) {
            if (is_duplicate()) {
              delivery_lock.unlock();
              return;
            }

            pending.emplace(pending_delivery_, delivery_lock, *snapshot);
          }
        }
      }

      if VLIKELY (has_callback) {
        if (delivery_lock.owns_lock()) {
          delivery_lock.unlock();
        }

        if constexpr (std::is_same_v<ValueT, Bytes>) {
          callback_(*snapshot);
        } else {
          callback_(value);
        }
      }

      std::unique_ptr<std::optional<ValueT>> prepared_value;

      if constexpr (!kInlineValue) {
        prepared_value = std::make_unique<std::optional<ValueT>>(std::in_place, std::move(value));
      }

      bool recheck = !delivery_lock.owns_lock();

      if (recheck) {
        delivery_lock.lock();
      }

      if (!snapshot) {
        const auto can_reuse = [&data](const std::optional<Bytes>& buffer) {
          return buffer && data.offset() == 0U && buffer->real_size() == data.size();
        };

        bool needs_snapshot;

        if constexpr (std::is_same_v<ValueT, Bytes>) {
          needs_snapshot = !can_reuse(value_);
        } else {
          needs_snapshot = change_reporting && generation == change_generation_ && !can_reuse(last_cache_);
        }

        if (needs_snapshot) {
          if VUNLIKELY (!copy_snapshot()) {
            if constexpr (std::is_same_v<ValueT, Bytes>) {
              return;
            }
          }

          delivery_lock.lock();
          recheck = true;
        }
      }

      const bool current_basis = change_reporting && generation == change_generation_;

      if (recheck && !has_callback && current_basis && is_duplicate()) {
        delivery_lock.unlock();
        return;
      }

      const auto update_bytes = [&data, &snapshot](std::optional<Bytes>& cache) {
        if (!snapshot) {
          cache = data;
        } else if (cache && cache->capacity() + cache->offset() > Bytes::stack_size()) {
          cache.swap(snapshot);
        } else {
          cache.emplace(std::move(*snapshot));
        }
      };

      if constexpr (std::is_same_v<ValueT, Bytes>) {
        if VUNLIKELY (value_is_basis_ && !current_basis) {
          last_cache_.emplace(std::move(*value_));
          value_is_basis_ = false;
        }

        update_bytes(value_);

        if (current_basis) {
          last_cache_.reset();
          value_is_basis_ = true;
        }
      } else {
        if constexpr (kInlineValue) {
          value_.emplace(std::move(value));
        } else {
          value_.swap(prepared_value);
        }

        if (current_basis) {
          update_bytes(last_cache_);

          if VUNLIKELY (last_cache_->real_size() != wire_size) {
            last_cache_.reset();
          }
        }
      }

      if (pending.has_value()) {
        pending->committed = true;
        pending.reset();
      }

      has_value_notification_ = true;
      delivery_lock.unlock();
      cv_.notify_all();
    };

    if constexpr (std::is_same_v<ValueT, Bytes>) {
      deliver(data, std::move(lock));
    } else {
      lock.unlock();
      auto value = this->template get_default_value<ValueT>();

      if VUNLIKELY (!Serializer::deserialize_owned<kValueType>(data, value, this->impl_->transport_type)) {
        VLOG_T("Getter deserialize failed, url: ", this->impl_->url, ".");
        return;
      }

      deliver(value, std::move(lock));
    }
  });

  if VUNLIKELY (!listened) {
    Node<GetterImpl, SecT>::deinit();
    return false;
  }

  return true;
}

template <typename ValueT, SecurityType SecT>
inline void Getter<ValueT, SecT>::interrupt() {
  {
    std::lock_guard lock(mtx_);
    Node<GetterImpl, SecT>::interrupt();
  }

  cv_.notify_all();
}

template <typename ValueT, SecurityType SecT>
inline void Getter<ValueT, SecT>::mark_as_subscriber() {
  if VUNLIKELY (this->has_inited_.load(std::memory_order_acquire)) {
    this->impl_->deinit_ext();
    this->impl_->impl_type = kSubscriber;
    this->impl_->init_ext();
  } else {
    this->impl_->impl_type = kSubscriber;
  }
}

template <typename ValueT, SecurityType SecT>
inline bool Getter<ValueT, SecT>::listen_bytes(NodeImpl::MsgCallback&& callback) {
  if (!this->has_inited_.load(std::memory_order_acquire)) {
    return false;
  }

  return this->impl_->listen([this, callback = std::move(callback)](const Bytes& data) {
    if constexpr (SecT == SecurityType::kWithSecurity) {
      Bytes sec_data;

      if VUNLIKELY (!this->impl_->security || !this->impl_->security->decrypt(data, sec_data)) {
        VLOG_T("Getter decrypt failed, url: ", this->impl_->url, ".");
        return;
      }

      this->invoke_callback(callback, sec_data);
    } else {
      this->impl_->try_record(ActionType::kGet, data);

      this->invoke_callback(callback, data);
    }
  });
}

template <typename ValueT>
template <typename SecurityConfigT>
inline typename SecurityGetter<ValueT>::UniquePtr SecurityGetter<ValueT>::create_unique(const std::string& url_str,
                                                                                        SecurityConfigT&& sec_cfg,
                                                                                        InitType type) {
  static_assert(std::is_same_v<std::decay_t<SecurityConfigT>, Security::Config>,
                "SecurityConfigT must be Security::Config.");

  return std::make_unique<SecurityGetter<ValueT>>(url_str, std::forward<SecurityConfigT>(sec_cfg), type);
}

template <typename ValueT>
template <typename SecurityConfigT>
inline typename SecurityGetter<ValueT>::SharedPtr SecurityGetter<ValueT>::create_shared(const std::string& url_str,
                                                                                        SecurityConfigT&& sec_cfg,
                                                                                        InitType type) {
  static_assert(std::is_same_v<std::decay_t<SecurityConfigT>, Security::Config>,
                "SecurityConfigT must be Security::Config.");

  return std::make_shared<SecurityGetter<ValueT>>(url_str, std::forward<SecurityConfigT>(sec_cfg), type);
}

template <typename ValueT>
template <typename ConfT, typename SecurityConfigT, typename>
inline SecurityGetter<ValueT>::SecurityGetter(const ConfT& conf, SecurityConfigT&& sec_cfg, InitType type)
    : Getter<ValueT, SecurityType::kWithSecurity>(conf, InitType::kWithoutInit) {
  static_assert(std::is_same_v<std::decay_t<SecurityConfigT>, Security::Config>,
                "SecurityConfigT must be Security::Config.");

  this->enable_security(std::forward<SecurityConfigT>(sec_cfg));

  if VLIKELY (type == InitType::kWithInit) {
    this->init();  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
  }
}

template <typename ValueT>
template <typename SecurityConfigT>
inline SecurityGetter<ValueT>::SecurityGetter(const std::string& url_str, SecurityConfigT&& sec_cfg, InitType type)
    : Getter<ValueT, SecurityType::kWithSecurity>(url_str, InitType::kWithoutInit) {
  static_assert(std::is_same_v<std::decay_t<SecurityConfigT>, Security::Config>,
                "SecurityConfigT must be Security::Config.");

  this->enable_security(std::forward<SecurityConfigT>(sec_cfg));

  if VLIKELY (type == InitType::kWithInit) {
    this->init();  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
  }
}

}  // namespace vlink
