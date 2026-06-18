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
#include "../base/memory_resource.h"
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
template <typename ConfT, typename>
inline Client<ReqT, RespT, SecT>::Client(const ConfT& conf, InitType type) {
  static_assert(ConfT::get_allow_impl_type() & kImplType, "Conf does not support client mode.");

  if VUNLIKELY (!conf.parse(kImplType) || !conf.is_valid()) {
    VLOG_F(conf, " client configuration is invalid or could not be parsed.");
    return;
  }

  this->impl_ = conf.create_client();

  if VUNLIKELY (!this->impl_) {
    VLOG_F(conf, " client implementation not available for this transport.");
    return;
  }

  this->impl_->transport_type = conf.get_transport_type();
  this->impl_->ser_type = Serializer::get_serialized_type<kReqType, ReqT>();

  if constexpr (kHasResp) {
    const auto resp_ser_type = Serializer::get_serialized_type<kRespType, RespT>();

    if (!this->impl_->ser_type.empty() || !resp_ser_type.empty()) {
      this->impl_->ser_type += ";" + resp_ser_type;
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

template <typename ReqT, typename RespT, SecurityType SecT>
inline Client<ReqT, RespT, SecT>::Client(const std::string& url_str, InitType type)
    : Client<ReqT, RespT, SecT>(Url(url_str), type) {}

template <typename ReqT, typename RespT, SecurityType SecT>
inline Client<ReqT, RespT, SecT>::~Client() {
  {
    std::lock_guard lock(future_mtx_);
    future_map_.clear();
  }

  // NOLINTNEXTLINE(clang-analyzer-optin.cplusplus.VirtualCall)
  this->deinit();
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
    ret = call_bytes(req, [&resp](const Bytes& resp_data) { resp = resp_data; }, timeout);
  } else {
    Bytes req_data;

    if constexpr (SecT != SecurityType::kWithSecurity) {
      if (this->is_support_loan_) {
        size_t ser_size = Serializer::get_serialized_size<kReqType>(req);

        req_data = this->impl_->loan(ser_size);

        if VUNLIKELY (ser_size != 0 && req_data.empty()) {
          return false;
        }
      }
    }

    if VUNLIKELY (!Serializer::serialize<kReqType>(req, req_data, this->impl_->transport_type)) {
      VLOG_T("Client serialize failed, url: ", this->impl_->url, ".");

      if constexpr (SecT != SecurityType::kWithSecurity) {
        if (this->is_support_loan_) {
          this->impl_->return_loan(req_data);
        }
      }

      return false;
    }

    bool deserialize_success = false;

    ret = call_bytes(
        req_data,
        [this, &resp, &deserialize_success](const Bytes& resp_data) {
          if VLIKELY (Serializer::deserialize<kRespType>(resp_data, resp, this->impl_->transport_type)) {
            deserialize_success = true;
          } else {
            VLOG_T("Client deserialize failed, url: ", this->impl_->url, ".");
          }
        },
        timeout);

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

  thread_local auto resp = this->template get_default_value<RespT>();

  if VLIKELY (invoke(req, resp, timeout)) {
    return std::make_optional<RespT>(resp);
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

    if constexpr (SecT != SecurityType::kWithSecurity) {
      if (this->is_support_loan_) {
        size_t ser_size = Serializer::get_serialized_size<kReqType>(req);

        req_data = this->impl_->loan(ser_size);

        if VUNLIKELY (ser_size != 0 && req_data.empty()) {
          return false;
        }
      }
    }

    if VUNLIKELY (!Serializer::serialize<kReqType>(req, req_data, this->impl_->transport_type)) {
      VLOG_T("Client serialize failed, url: ", this->impl_->url, ".");

      if constexpr (SecT != SecurityType::kWithSecurity) {
        if (this->is_support_loan_) {
          this->impl_->return_loan(req_data);
        }
      }

      return false;
    }

    ret = call_bytes(req_data, [this, callback = std::move(callback)](const Bytes& resp_data) {
      thread_local auto resp = this->template get_default_value<RespT>();

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

  auto pro = MemoryResource::make_shared<std::promise<RespT>>();
  auto future = pro->get_future();

  bool ret = false;
  int64_t target_seq = 0;

  {
    std::lock_guard lock(future_mtx_);
    target_seq = future_seq_++;
    future_map_.emplace(target_seq, pro);
  }

  auto cleanup_on_error = [this, target_seq, pro](const std::string& error_str) {
    std::lock_guard lock(future_mtx_);
    future_map_.erase(target_seq);

    try {
      throw Exception::RuntimeError(error_str);
    } catch (std::exception&) {
      pro->set_exception(std::current_exception());
    }
  };

  if constexpr (std::is_same_v<ReqT, Bytes> && std::is_same_v<RespT, Bytes>) {
    ret = call_bytes(req, [this, target_seq](const Bytes& resp_data) {
      std::lock_guard lock(future_mtx_);
      auto it = future_map_.find(target_seq);

      if VLIKELY (it != future_map_.end()) {
        it->second->set_value(resp_data);
        future_map_.erase(it);
      }
    });
  } else {
    Bytes req_data;

    if constexpr (SecT != SecurityType::kWithSecurity) {
      if (this->is_support_loan_) {
        size_t ser_size = Serializer::get_serialized_size<kReqType>(req);
        req_data = this->impl_->loan(ser_size);

        if VUNLIKELY (ser_size != 0 && req_data.empty()) {
          cleanup_on_error("Client async_invoke error (Failed to Loan)");
          return future;
        }
      }
    }

    if VUNLIKELY (!Serializer::serialize<kReqType>(req, req_data, this->impl_->transport_type)) {
      VLOG_T("Client serialize failed, url: ", this->impl_->url, ".");

      if constexpr (SecT != SecurityType::kWithSecurity) {
        if (this->is_support_loan_) {
          this->impl_->return_loan(req_data);
        }
      }

      cleanup_on_error("Client async_invoke error (Failed to serialize req)");
      return future;
    }

    ret = call_bytes(req_data, [this, target_seq](const Bytes& resp_data) {
      bool convert_success = false;

      thread_local auto resp = this->template get_default_value<RespT>();

      if VLIKELY (Serializer::deserialize<kRespType>(resp_data, resp, this->impl_->transport_type)) {
        convert_success = true;
      } else {
        VLOG_T("Client deserialize failed, url: ", this->impl_->url, ".");
      }

      std::lock_guard lock(future_mtx_);

      auto it = future_map_.find(target_seq);

      if VLIKELY (it != future_map_.end()) {
        if VLIKELY (convert_success) {
          it->second->set_value(resp);
        } else {
          try {
            throw Exception::RuntimeError("Client async_invoke error (Failed to deserialize resp)");
          } catch (std::exception&) {
            it->second->set_exception(std::current_exception());
          }
        }

        future_map_.erase(it);
      }
    });
  }

  if VUNLIKELY (!ret) {
    cleanup_on_error("Client async_invoke error (Failed to call)");
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

    if constexpr (SecT != SecurityType::kWithSecurity) {
      if (this->is_support_loan_) {
        size_t ser_size = Serializer::get_serialized_size<kReqType>(req);

        req_data = this->impl_->loan(ser_size);

        if VUNLIKELY (ser_size != 0 && req_data.empty()) {
          return false;
        }
      }
    }

    if VUNLIKELY (!Serializer::serialize<kReqType>(req, req_data, this->impl_->transport_type)) {
      VLOG_T("Client serialize failed, url: ", this->impl_->url, ".");

      if constexpr (SecT != SecurityType::kWithSecurity) {
        if (this->is_support_loan_) {
          this->impl_->return_loan(req_data);
        }
      }

      return false;
    }

    ret = call_bytes(req_data);
  }

  return ret;
}

template <typename ReqT, typename RespT, SecurityType SecT>
inline bool Client<ReqT, RespT, SecT>::call_bytes(const Bytes& req_data, NodeImpl::MsgCallback&& callback,
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
            return;
          }

          this->invoke_callback(callback, resp_sec_data);
        },
        timeout);
  } else {
    this->impl_->try_record(ActionType::kClientRequest, req_data);

    if VUNLIKELY (!callback) {
      return this->impl_->call(req_data, nullptr, timeout);
    }

    return this->impl_->call(
        req_data,
        [this, callback = std::move(callback)](const Bytes& resp_data) {
          this->impl_->try_record(ActionType::kClientResponse, resp_data);

          this->invoke_callback(callback, resp_data);
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
template <typename ConfT, typename SecurityConfigT, typename>
inline SecurityClient<ReqT, RespT>::SecurityClient(const ConfT& conf, SecurityConfigT&& sec_cfg, InitType type)
    : Client<ReqT, RespT, SecurityType::kWithSecurity>(conf, InitType::kWithoutInit) {
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
