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

#include <exception>
#include <memory>
#include <string>
#include <utility>

#include "../base/cpu_profiler_guard.h"
#include "../base/logger.h"
#include "../client.h"
#include "../impl/url.h"
#include "../serializer.h"

namespace vlink {

template <typename ReqT, typename RespT, SecurityType SecT>
inline typename Client<ReqT, RespT, SecT>::UniquePtr Client<ReqT, RespT, SecT>::create_unique(
    const std::string& url_str, InitType type) {
  return std::make_unique<Client<ReqT, RespT, SecT>>(url_str, type);
}

template <typename ReqT, typename RespT, SecurityType SecT>
inline typename Client<ReqT, RespT, SecT>::SharedPtr Client<ReqT, RespT, SecT>::create_shared(
    const std::string& url_str, InitType type) {
  return std::make_shared<Client<ReqT, RespT, SecT>>(url_str, type);
}

template <typename ReqT, typename RespT, SecurityType SecT>
template <typename TransportT, typename>
inline Client<ReqT, RespT, SecT>::Client(const TransportT& transport, InitType type) {
  static_assert(TransportT::get_allow_impl_type() & kImplType, "Transport does not support client mode.");

  if VUNLIKELY (!transport.parse(kImplType) || !transport.is_valid()) {
    VLOG_F(transport, " client configuration is invalid or could not be parsed.");
    return;
  }

  this->impl_ = transport.create_client();

  if VUNLIKELY (!this->impl_) {
    VLOG_F(transport, " client implementation not available for this transport.");
    return;
  }

  this->impl_->transport_type = transport.get_transport_type();
  this->impl_->ser_type = Serializer::get_serialized_type<kReqType, ReqT>();

  if constexpr (kHasResp) {
    const auto resp_ser_type = Serializer::get_serialized_type<kRespType, RespT>();

    if (!this->impl_->ser_type.empty() || !resp_ser_type.empty()) {
      this->impl_->ser_type += "|" + resp_ser_type;
    }
  }

  {
    constexpr auto kReqSchemaType = Serializer::get_schema_type<kReqType, ReqT>();
    constexpr auto kRespSchemaType = Serializer::get_schema_type<kRespType, RespT>();

    if constexpr (kHasResp && kReqSchemaType != kRespSchemaType) {
      this->impl_->schema_type = SchemaType::kUnknown;
    } else {
      this->impl_->schema_type = kReqSchemaType;
    }
  }

  this->impl_->is_cdr_type = Serializer::is_cdr_type<ReqT>();
  this->impl_->is_resp_type = kHasResp;

  if constexpr (kHasResp) {
    this->impl_->is_resp_cdr_type = Serializer::is_cdr_type<RespT>();
  }

  if constexpr (std::is_same_v<TransportT, Url>) {
    this->impl_->url = transport.get_str();
  }

  if constexpr (SecT == SecurityType::kWithSecurity) {
    this->impl_->is_security_type = true;
  }

  if VLIKELY (type == InitType::kWithInit) {
    this->init();  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
  }
}

template <typename ReqT, typename RespT, SecurityType SecT>
inline Client<ReqT, RespT, SecT>::Client(const std::string& url_str, InitType type)
    : Client<ReqT, RespT, SecT>(Url(url_str), type) {}

template <typename ReqT, typename RespT, SecurityType SecT>
inline Client<ReqT, RespT, SecT>::~Client() {
  // NOLINTNEXTLINE(clang-analyzer-optin.cplusplus.VirtualCall)
  this->deinit();
}

template <typename ReqT, typename RespT, SecurityType SecT>
inline void Client<ReqT, RespT, SecT>::interrupt() {
  if constexpr (kHasResp) {
    if (!this->has_inited()) {
      std::lock_guard lock(future_mtx_);
      future_map_.clear();
    }
  }

  Node<ClientImpl, SecT>::interrupt();
}

template <typename ReqT, typename RespT, SecurityType SecT>
inline void Client<ReqT, RespT, SecT>::detect_connected(ConnectCallback&& callback) {
  this->impl_->detect_connected(std::move(callback));
}

template <typename ReqT, typename RespT, SecurityType SecT>
inline bool Client<ReqT, RespT, SecT>::wait_for_connected(std::chrono::milliseconds timeout) {
  if VUNLIKELY (timeout.count() == 0) {
    VLOG_W("Client: Timeout value is 0, using infinite wait instead.");
    timeout = Timeout::kInfinite;
  }

  return this->impl_->wait_for_connected(timeout);
}

template <typename ReqT, typename RespT, SecurityType SecT>
inline bool Client<ReqT, RespT, SecT>::is_connected() const {
  return this->impl_->is_connected();
}

template <typename ReqT, typename RespT, SecurityType SecT>
inline bool Client<ReqT, RespT, SecT>::invoke(const ReqT& req, RespT& resp, std::chrono::milliseconds timeout) {
  if VUNLIKELY (timeout.count() == 0) {
    VLOG_W("Client: Timeout value is 0, using infinite wait instead.");
    timeout = Timeout::kInfinite;
  }

#ifndef VLINK_DISABLE_PROFILER
  CpuProfilerGuard profiler_guard(this->impl_->profiler.get());
#endif

  static_assert(kHasResp, "Invoke requires a response type.");

  bool ret = false;

  if constexpr (std::is_same_v<ReqT, Bytes> && std::is_same_v<RespT, Bytes>) {
    bool received = false;
    ret = call_bytes(
        req,
        [&resp, &received](const Bytes& resp_data) {
          resp = resp_data;
          received = true;
        },
        timeout);
    ret = ret && received;
  } else {
    Bytes req_data;

    if constexpr (std::is_same_v<ReqT, Bytes>) {
      req_data =
          req.is_loaned() ? Bytes::loan_internal(req.data(), req.size()) : Bytes::shallow_copy(req.data(), req.size());
    } else if VUNLIKELY (!this->template serialize_to_transport<kReqType>(req, req_data)) {
      VLOG_T("Client serialize failed, url: ", this->impl_->url, ".");

      return false;
    }

    std::exception_ptr callback_exception;
    bool deserialize_success = false;

    ret = call_bytes(
        req_data,
        [this, &resp, &deserialize_success, &callback_exception](const Bytes& resp_data) {
          try {
            if VLIKELY (deserialize_response(resp_data, resp)) {
              deserialize_success = true;
            } else {
              VLOG_T("Client deserialize failed, url: ", this->impl_->url, ".");
            }
          } catch (...) {
            callback_exception = std::current_exception();
          }
        },
        timeout);

    if VUNLIKELY (callback_exception) {
      std::rethrow_exception(callback_exception);
    }

    ret = ret && deserialize_success;
  }

  return ret;
}

template <typename ReqT, typename RespT, SecurityType SecT>
inline std::optional<RespT> Client<ReqT, RespT, SecT>::invoke(const ReqT& req, std::chrono::milliseconds timeout) {
  if VUNLIKELY (timeout.count() == 0) {
    VLOG_W("Client: Timeout value is 0, using infinite wait instead.");
    timeout = Timeout::kInfinite;
  }

#ifndef VLINK_DISABLE_PROFILER
  CpuProfilerGuard profiler_guard(this->impl_->profiler.get());
#endif

  auto resp = this->template get_default_value<RespT>();

  if VLIKELY (invoke(req, resp, timeout)) {
    return std::make_optional<RespT>(std::move(resp));
  }

  return std::nullopt;
}

template <typename ReqT, typename RespT, SecurityType SecT>
inline bool Client<ReqT, RespT, SecT>::invoke(const ReqT& req, RespCallback&& callback) {
#ifndef VLINK_DISABLE_PROFILER
  CpuProfilerGuard profiler_guard(this->impl_->profiler.get());
#endif

  static_assert(kHasResp, "Invoke requires a response type.");

  bool ret = false;

  if constexpr (std::is_same_v<ReqT, Bytes> && std::is_same_v<RespT, Bytes>) {
    ret = call_bytes(req, [callback = std::move(callback)](const Bytes& resp_data) { callback(resp_data); });
  } else {
    Bytes req_data;

    if constexpr (std::is_same_v<ReqT, Bytes>) {
      req_data =
          req.is_loaned() ? Bytes::loan_internal(req.data(), req.size()) : Bytes::shallow_copy(req.data(), req.size());
    } else if VUNLIKELY (!this->template serialize_to_transport<kReqType>(req, req_data)) {
      VLOG_T("Client serialize failed, url: ", this->impl_->url, ".");

      return false;
    }

    ret = call_bytes(req_data, [this, callback = std::move(callback)](const Bytes& resp_data) {
      // NOLINTNEXTLINE(readability-qualified-auto)
      auto resp = this->template get_default_value<RespT>();

      if VUNLIKELY (!Serializer::deserialize<kRespType>(resp_data, resp, this->impl_->transport_type)) {
        VLOG_T("Client deserialize failed, url: ", this->impl_->url, ".");
        return;
      }

      callback(resp);
    });
  }

  return ret;
}

template <typename ReqT, typename RespT, SecurityType SecT>
inline std::future<RespT> Client<ReqT, RespT, SecT>::async_invoke(const ReqT& req) {
#ifndef VLINK_DISABLE_PROFILER
  CpuProfilerGuard profiler_guard(this->impl_->profiler.get());
#endif

  static_assert(kHasResp, "async_invoke requires a response type.");

  auto pro = std::make_shared<std::promise<RespT>>();
  auto future = pro->get_future();

  bool ret = false;
  uint64_t target_seq = 0;

  {
    std::lock_guard lock(future_mtx_);

    if VUNLIKELY (!this->has_inited()) {
      pro->set_exception(
          std::make_exception_ptr(Exception::RuntimeError("Client async_invoke error (Not initialized)")));
      return future;
    }

    target_seq = future_seq_++;
    future_map_.emplace(target_seq, std::move(pro));
  }

  auto on_response = [this, target_seq](const Bytes* resp_data) {
    if constexpr (SecT == SecurityType::kWithSecurity) {
      if VUNLIKELY (!resp_data) {
        fail_future(target_seq, "Client async_invoke error (Failed to decrypt resp)");
        return;
      }
    }

    std::shared_ptr<std::promise<RespT>> target_pro;

    {
      std::lock_guard lock(future_mtx_);
      auto it = future_map_.find(target_seq);

      if VUNLIKELY (it == future_map_.end()) {
        return;
      }

      target_pro = std::move(it->second);
      future_map_.erase(it);
    }

    if constexpr (std::is_same_v<ReqT, Bytes> && std::is_same_v<RespT, Bytes>) {
      target_pro->set_value(*resp_data);
    } else {
      try {
        auto resp = this->template get_default_value<RespT>();

        if VUNLIKELY (!deserialize_response(*resp_data, resp)) {
          VLOG_T("Client deserialize failed, url: ", this->impl_->url, ".");
          throw Exception::RuntimeError("Client async_invoke error (Failed to deserialize resp)");
        }

        target_pro->set_value(std::move(resp));
      } catch (...) {
        target_pro->set_exception(std::current_exception());
      }
    }
  };

  try {
    if constexpr (std::is_same_v<ReqT, Bytes> && std::is_same_v<RespT, Bytes>) {
      ret = call_bytes<true>(req, std::move(on_response));
    } else {
      Bytes req_data;

      if constexpr (std::is_same_v<ReqT, Bytes>) {
        req_data = req.is_loaned() ? Bytes::loan_internal(req.data(), req.size())
                                   : Bytes::shallow_copy(req.data(), req.size());
      } else if VUNLIKELY (!this->template serialize_to_transport<kReqType>(req, req_data)) {
        VLOG_T("Client serialize failed, url: ", this->impl_->url, ".");

        fail_future(target_seq, "Client async_invoke error (Failed to serialize req)");
        return future;
      }

      ret = call_bytes<true>(req_data, std::move(on_response));
    }
  } catch (...) {
    std::lock_guard lock(future_mtx_);
    future_map_.erase(target_seq);
    throw;
  }

  if VUNLIKELY (!ret) {
    fail_future(target_seq, "Client async_invoke error (Failed to call)");
  }

  return future;
}

template <typename ReqT, typename RespT, SecurityType SecT>
inline bool Client<ReqT, RespT, SecT>::send(const ReqT& req) {
#ifndef VLINK_DISABLE_PROFILER
  CpuProfilerGuard profiler_guard(this->impl_->profiler.get());
#endif

  static_assert(!kHasResp, "Send not supported; use invoke() for request-response.");

  bool ret = false;

  if constexpr (std::is_same_v<ReqT, Bytes>) {
    ret = call_bytes(req);
  } else {
    Bytes req_data;

    if VUNLIKELY (!this->template serialize_to_transport<kReqType>(req, req_data)) {
      VLOG_T("Client serialize failed, url: ", this->impl_->url, ".");

      return false;
    }

    ret = call_bytes(req_data);
  }

  return ret;
}

template <typename ReqT, typename RespT, SecurityType SecT>
inline bool Client<ReqT, RespT, SecT>::deserialize_response(const Bytes& data, RespT& response) {
  return Serializer::deserialize_owned<kRespType>(data, response, this->impl_->transport_type);
}

template <typename ReqT, typename RespT, SecurityType SecT>
inline void Client<ReqT, RespT, SecT>::fail_future(uint64_t seq, const char* error) {
  std::shared_ptr<std::promise<RespT>> pro;

  {
    std::lock_guard lock(future_mtx_);
    auto entry = future_map_.extract(seq);

    if (entry.empty()) {
      return;
    }

    pro = std::move(entry.mapped());
  }

  pro->set_exception(std::make_exception_ptr(Exception::RuntimeError(error)));
}

template <typename ReqT, typename RespT, SecurityType SecT>
template <bool ReportFailureT>
inline bool Client<ReqT, RespT, SecT>::call_bytes(
    const Bytes& req_data,
    std::conditional_t<ReportFailureT, Function<void(const Bytes*)>, NodeImpl::MsgCallback>&& callback,
    std::chrono::milliseconds timeout) {
  if constexpr (SecT == SecurityType::kWithSecurity) {
    Bytes req_sec_data;

    if VUNLIKELY (!this->impl_->security || !this->impl_->security->encrypt(req_data, req_sec_data)) {
      VLOG_T("Client encrypt failed, url: ", this->impl_->url, ".");
      return false;
    }

    if VUNLIKELY (!callback) {
      return this->impl_->call(req_sec_data, nullptr, timeout);
    }

    return this->impl_->call(
        req_sec_data,
        [this, callback = std::move(callback)](const Bytes& resp_data) {
          Bytes resp_sec_data;

          if VUNLIKELY (!this->impl_->security || !this->impl_->security->decrypt(resp_data, resp_sec_data)) {
            VLOG_T("Client decrypt failed, url: ", this->impl_->url, ".");

            if constexpr (ReportFailureT) {
              this->invoke_callback(callback, nullptr);
            }

            return;
          }

          if constexpr (ReportFailureT) {
            this->invoke_callback(callback, &resp_sec_data);
          } else {
            this->invoke_callback(callback, resp_sec_data);
          }
        },
        timeout);
  } else {
    this->impl_->record_before_send(ActionType::kClientRequest, req_data);

    if VUNLIKELY (!callback) {
      return this->impl_->call(req_data, nullptr, timeout);
    }

    return this->impl_->call(
        req_data,
        [this, callback = std::move(callback)](const Bytes& resp_data) {
          this->impl_->try_record(ActionType::kClientResponse, resp_data);

          if constexpr (ReportFailureT) {
            this->invoke_callback(callback, &resp_data);
          } else {
            this->invoke_callback(callback, resp_data);
          }
        },
        timeout);
  }
}

template <typename ReqT, typename RespT>
template <typename SecurityConfigT>
inline typename SecurityClient<ReqT, RespT>::UniquePtr SecurityClient<ReqT, RespT>::create_unique(
    const std::string& url_str, SecurityConfigT&& sec_cfg, InitType type) {
  static_assert(std::is_same_v<std::decay_t<SecurityConfigT>, Security::Config>,
                "SecurityConfigT must be Security::Config.");

  return std::make_unique<SecurityClient<ReqT, RespT>>(url_str, std::forward<SecurityConfigT>(sec_cfg), type);
}

template <typename ReqT, typename RespT>
template <typename SecurityConfigT>
inline typename SecurityClient<ReqT, RespT>::SharedPtr SecurityClient<ReqT, RespT>::create_shared(
    const std::string& url_str, SecurityConfigT&& sec_cfg, InitType type) {
  static_assert(std::is_same_v<std::decay_t<SecurityConfigT>, Security::Config>,
                "SecurityConfigT must be Security::Config.");

  return std::make_shared<SecurityClient<ReqT, RespT>>(url_str, std::forward<SecurityConfigT>(sec_cfg), type);
}

template <typename ReqT, typename RespT>
template <typename TransportT, typename SecurityConfigT, typename>
inline SecurityClient<ReqT, RespT>::SecurityClient(const TransportT& transport, SecurityConfigT&& sec_cfg,
                                                   InitType type)
    : Client<ReqT, RespT, SecurityType::kWithSecurity>(transport, InitType::kWithoutInit) {
  static_assert(std::is_same_v<std::decay_t<SecurityConfigT>, Security::Config>,
                "SecurityConfigT must be Security::Config.");

  this->enable_security(std::forward<SecurityConfigT>(sec_cfg));

  if VLIKELY (type == InitType::kWithInit) {
    this->init();  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
  }
}

template <typename ReqT, typename RespT>
template <typename SecurityConfigT>
inline SecurityClient<ReqT, RespT>::SecurityClient(const std::string& url_str, SecurityConfigT&& sec_cfg, InitType type)
    : Client<ReqT, RespT, SecurityType::kWithSecurity>(url_str, InitType::kWithoutInit) {
  static_assert(std::is_same_v<std::decay_t<SecurityConfigT>, Security::Config>,
                "SecurityConfigT must be Security::Config.");

  this->enable_security(std::forward<SecurityConfigT>(sec_cfg));

  if VLIKELY (type == InitType::kWithInit) {
    this->init();  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
  }
}

}  // namespace vlink
