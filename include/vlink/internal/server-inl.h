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
#include "../server.h"

namespace vlink {

template <typename ReqT, typename RespT, SecurityType SecT>
inline typename Server<ReqT, RespT, SecT>::UniquePtr Server<ReqT, RespT, SecT>::create_unique(
    const std::string& url_str, InitType type) {
  return std::make_unique<Server<ReqT, RespT, SecT>>(url_str, type);
}

template <typename ReqT, typename RespT, SecurityType SecT>
inline typename Server<ReqT, RespT, SecT>::SharedPtr Server<ReqT, RespT, SecT>::create_shared(
    const std::string& url_str, InitType type) {
  return std::make_shared<Server<ReqT, RespT, SecT>>(url_str, type);
}

template <typename ReqT, typename RespT, SecurityType SecT>
template <typename ConfT, typename>
inline Server<ReqT, RespT, SecT>::Server(const ConfT& conf, InitType type) {
  static_assert(ConfT::get_allow_impl_type() & kImplType, "Conf does not support server mode.");

  if VUNLIKELY (!conf.parse(kImplType) || !conf.is_valid()) {
    VLOG_F(conf, " server configuration is invalid or could not be parsed.");
    return;
  }

  this->impl_ = conf.create_server();

  if VUNLIKELY (!this->impl_) {
    VLOG_F(conf, " server implementation not available for this transport.");
    return;
  }

  this->impl_->transport_type = conf.get_transport_type();
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
inline Server<ReqT, RespT, SecT>::Server(const std::string& url_str, InitType type)
    : Server<ReqT, RespT, SecT>(Url(url_str), type) {}

template <typename ReqT, typename RespT, SecurityType SecT>
inline bool Server<ReqT, RespT, SecT>::listen(ReqCallback&& callback) {
  static_assert(!kHasResp, "Reply not supported; use listen(ReqRespCallback&&) instead.");

  this->impl_->is_sync_type = true;

  return listen_bytes([this, callback = std::move(callback)](uint64_t, const Bytes& req_data, Bytes*) {
#ifndef VLINK_DISABLE_PROFILER
    CpuProfilerGuard profiler_guard(this->impl_->profiler.get());
#endif

    if constexpr (std::is_same_v<ReqT, Bytes>) {
      (void)this;

      callback(req_data);
    } else {
      thread_local auto req = this->template get_default_value<ReqT>();

      if VUNLIKELY (!Serializer::deserialize<kReqType>(req_data, req, this->impl_->transport_type)) {
        VLOG_T("Server deserialize failed, url: ", this->impl_->url, ".");
        return;
      }

      callback(req);
    }
  });
}

template <typename ReqT, typename RespT, SecurityType SecT>
inline bool Server<ReqT, RespT, SecT>::listen(ReqRespCallback&& callback) {
  static_assert(kHasResp, "Must have reply.");

  this->impl_->is_sync_type = true;

  return listen_bytes([this, callback = std::move(callback)](uint64_t req_id, const Bytes& req_data, Bytes* resp_data) {
#ifndef VLINK_DISABLE_PROFILER
    CpuProfilerGuard profiler_guard(this->impl_->profiler.get());
#endif

    if VUNLIKELY (!resp_data) {
      VLOG_E("Server resp_data pointer is null.");
      return;
    }

    if constexpr (std::is_same_v<ReqT, Bytes> && std::is_same_v<RespT, Bytes>) {
      (void)this;

      callback(req_data, *resp_data);

      reply_bytes<true>(req_id, *resp_data, true, resp_data);
    } else {
      thread_local auto req = this->template get_default_value<ReqT>();
      auto resp = this->template get_default_value<RespT>();

      if VUNLIKELY (!Serializer::deserialize<kReqType>(req_data, req, this->impl_->transport_type)) {
        VLOG_T("Server deserialize failed, url: ", this->impl_->url, ".");
        return;
      }

      callback(req, resp);

      if constexpr (SecT != SecurityType::kWithSecurity) {
        if (this->is_support_loan_) {
          size_t ser_size = Serializer::get_serialized_size<kRespType>(resp);

          *resp_data = this->impl_->loan(ser_size);

          if VUNLIKELY (ser_size != 0 && resp_data->empty()) {
            return;
          }
        }
      }

      if VUNLIKELY (!Serializer::serialize<kRespType>(resp, *resp_data, this->impl_->transport_type)) {
        VLOG_T("Server serialize failed, url: ", this->impl_->url, ".");

        if constexpr (SecT != SecurityType::kWithSecurity) {
          if (this->is_support_loan_) {
            this->impl_->return_loan(*resp_data);
          }
        }

        return;
      }

      reply_bytes<true>(req_id, *resp_data, true, resp_data);
    }
  });
}

template <typename ReqT, typename RespT, SecurityType SecT>
inline bool Server<ReqT, RespT, SecT>::listen_for_reply(ReqAsyncRespCallback&& callback) {
  static_assert(kHasResp, "Must have reply.");

  this->impl_->is_sync_type = false;

  return listen_bytes([this, callback = std::move(callback)](uint64_t req_id, const Bytes& req_data, Bytes*) {
#ifndef VLINK_DISABLE_PROFILER
    CpuProfilerGuard profiler_guard(this->impl_->profiler.get());
#endif

    if constexpr (std::is_same_v<ReqT, Bytes>) {
      (void)this;

      callback(req_id, req_data);
    } else {
      thread_local auto req = this->template get_default_value<ReqT>();

      if VUNLIKELY (!Serializer::deserialize<kReqType>(req_data, req, this->impl_->transport_type)) {
        VLOG_T("Server deserialize failed, url: ", this->impl_->url, ".");
        return;
      }

      callback(req_id, req);
    }
  });
}

template <typename ReqT, typename RespT, SecurityType SecT>
inline bool Server<ReqT, RespT, SecT>::reply(uint64_t req_id, const RespT& resp) {
  static_assert(kHasResp, "Reply requires a response type.");

  if VUNLIKELY (!this->impl_->is_listened) {
    VLOG_F("Server::reply() requires listen() to be called first.");
    return false;
  }

  if VUNLIKELY (this->impl_->is_sync_type) {
    VLOG_F("Server::reply() is not available in synchronous listen mode.");
    return false;
  }

  if constexpr (std::is_same_v<RespT, Bytes>) {
    return reply_bytes<false>(req_id, resp, false);
  } else {
    Bytes resp_data;

    if constexpr (SecT != SecurityType::kWithSecurity) {
      if (this->is_support_loan_) {
        size_t ser_size = Serializer::get_serialized_size<kRespType>(resp);

        resp_data = this->impl_->loan(ser_size);

        if VUNLIKELY (ser_size != 0 && resp_data.empty()) {
          return false;
        }
      }
    }

    if VUNLIKELY (!Serializer::serialize<kRespType>(resp, resp_data, this->impl_->transport_type)) {
      VLOG_T("Server serialize failed, url: ", this->impl_->url, ".");

      if constexpr (SecT != SecurityType::kWithSecurity) {
        if (this->is_support_loan_) {
          this->impl_->return_loan(resp_data);
        }
      }

      return false;
    }

    bool ret = reply_bytes<false>(req_id, resp_data, false);

    return ret;
  }
}

template <typename ReqT, typename RespT, SecurityType SecT>
inline bool Server<ReqT, RespT, SecT>::has_clients() const {
  return this->impl_->has_clients();
}

template <typename ReqT, typename RespT, SecurityType SecT>
inline bool Server<ReqT, RespT, SecT>::listen_bytes(NodeImpl::ReqRespCallback&& callback) {
  if VUNLIKELY (!this->has_inited_.load(std::memory_order_acquire)) {
    VLOG_F("Server::listen_bytes() called before init().");
    return false;
  }

  if VUNLIKELY (this->impl_->is_listened) {
    VLOG_F("Server has already been listened, url: ", this->impl_->url, ".");
    return false;
  }

  bool ret = this->impl_->listen(
      [this, callback = std::move(callback)](uint64_t req_id, const Bytes& req_data, Bytes* resp_data) {
        if constexpr (SecT == SecurityType::kWithSecurity) {
          Bytes sec_req_data;

          if VUNLIKELY (!this->impl_->security || !this->impl_->security->decrypt(req_data, sec_req_data)) {
            VLOG_T("Server decrypt failed, url: ", this->impl_->url, ".");
            return;
          }

          this->invoke_callback(callback, req_id, sec_req_data, resp_data);
        } else {
          this->impl_->try_record(ActionType::kServerRequest, req_data);

          this->invoke_callback(callback, req_id, req_data, resp_data);
        }
      });

  this->impl_->is_listened = ret;

  return ret;
}

template <typename ReqT, typename RespT, SecurityType SecT>
template <bool HasPtrT>
inline bool Server<ReqT, RespT, SecT>::reply_bytes(uint64_t req_id, const Bytes& resp_data, bool is_sync,
                                                   [[maybe_unused]] Bytes* resp_data_ptr) {
  if VUNLIKELY (!this->has_inited_.load(std::memory_order_acquire)) {
    VLOG_F("Server::reply_bytes() called before init().");
  }

  if constexpr (SecT == SecurityType::kWithSecurity) {
    Bytes sec_resp_data;

    if VUNLIKELY (!this->impl_->security || !this->impl_->security->encrypt(resp_data, sec_resp_data)) {
      VLOG_T("Server encrypt failed, url: ", this->impl_->url, ".");
      return false;
    }

    if constexpr (HasPtrT) {
      *resp_data_ptr = sec_resp_data;
    }

    return this->impl_->reply(req_id, sec_resp_data, is_sync);
  } else {
    if constexpr (HasPtrT) {
      *resp_data_ptr = resp_data;
    }

    this->impl_->try_record(ActionType::kServerResponse, resp_data);

    return this->impl_->reply(req_id, resp_data, is_sync);
  }
}

template <typename ReqT, typename RespT>
template <typename SecurityConfigT>
inline typename SecurityServer<ReqT, RespT>::UniquePtr SecurityServer<ReqT, RespT>::create_unique(
    const std::string& url_str, SecurityConfigT&& sec_cfg, InitType type) {
  static_assert(std::is_same_v<std::decay_t<SecurityConfigT>, Security::Config>,
                "SecurityConfigT must be Security::Config.");

  return std::make_unique<SecurityServer<ReqT, RespT>>(url_str, std::forward<SecurityConfigT>(sec_cfg), type);
}

template <typename ReqT, typename RespT>
template <typename SecurityConfigT>
inline typename SecurityServer<ReqT, RespT>::SharedPtr SecurityServer<ReqT, RespT>::create_shared(
    const std::string& url_str, SecurityConfigT&& sec_cfg, InitType type) {
  static_assert(std::is_same_v<std::decay_t<SecurityConfigT>, Security::Config>,
                "SecurityConfigT must be Security::Config.");

  return std::make_shared<SecurityServer<ReqT, RespT>>(url_str, std::forward<SecurityConfigT>(sec_cfg), type);
}

template <typename ReqT, typename RespT>
template <typename ConfT, typename SecurityConfigT, typename>
inline SecurityServer<ReqT, RespT>::SecurityServer(const ConfT& conf, SecurityConfigT&& sec_cfg, InitType type)
    : Server<ReqT, RespT, SecurityType::kWithSecurity>(conf, InitType::kWithoutInit) {
  static_assert(std::is_same_v<std::decay_t<SecurityConfigT>, Security::Config>,
                "SecurityConfigT must be Security::Config.");

  this->enable_security(std::forward<SecurityConfigT>(sec_cfg));

  if VLIKELY (type == InitType::kWithInit) {
    this->init();  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
  }
}

template <typename ReqT, typename RespT>
template <typename SecurityConfigT>
inline SecurityServer<ReqT, RespT>::SecurityServer(const std::string& url_str, SecurityConfigT&& sec_cfg, InitType type)
    : Server<ReqT, RespT, SecurityType::kWithSecurity>(url_str, InitType::kWithoutInit) {
  static_assert(std::is_same_v<std::decay_t<SecurityConfigT>, Security::Config>,
                "SecurityConfigT must be Security::Config.");

  this->enable_security(std::forward<SecurityConfigT>(sec_cfg));

  if VLIKELY (type == InitType::kWithInit) {
    this->init();  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
  }
}

}  // namespace vlink
