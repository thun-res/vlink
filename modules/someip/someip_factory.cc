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

#include "./someip_factory.h"

#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

#include "./impl/server_impl.h"

namespace vlink {

// SomeipFactory
SomeipFactory::SomeipFactory() {
  Bytes::init_memory_pool();

  if VUNLIKELY (SomeipConf::get_thread_count() == 0) {
    VLOG_W("SomeipFactory: Someip does not support zero thread count.");

    SomeipConf::set_thread_count(1);
  }

  // NOLINTNEXTLINE(readability-static-accessed-through-instance)
  someip::runtime::get()->set_property("threads", std::to_string(SomeipConf::get_thread_count()));

  std::string cfg_env = Utils::get_env("VLINK_SOMEIP_CFG");

  if (!cfg_env.empty()) {
    load_global_config_file(cfg_env);
  }
}

SomeipFactory::~SomeipFactory() = default;

bool SomeipFactory::load_global_config_file(const std::string& filepath) {
  return Utils::set_env("VSOMEIP_CONFIGURATION", filepath, true);
}

// SomeipServer
SomeipServer::SomeipServer(const SomeipID& id) {
  std::tie(std::ignore, service_id_, instance_id_) = id;

  runtime_ = someip::runtime::get();

  app_ = runtime_->create_application(Utils::get_app_name());

  if VUNLIKELY (!app_->init()) {
    VLOG_E("SomeipFactory: Failed to init app.");
    return;
  }

  app_->register_message_handler(
      service_id_, instance_id_, someip::ANY_METHOD, [this](const std::shared_ptr<someip::message>& request) {
        if VUNLIKELY (request->get_message_type() != someip::message_type_e::MT_REQUEST &&
                      request->get_message_type() != someip::message_type_e::MT_REQUEST_NO_RETURN) {
          return;
        }

        auto payload = request->get_payload();
        Bytes req_data = Bytes::shallow_copy(payload->get_data(), payload->get_length());

        traverse_req_resp_callback([this, &request, &req_data](NodeImpl* impl, const auto& callback) {
          const auto* conf_ptr = impl->get_target_conf<SomeipConf>();

          if VUNLIKELY (conf_ptr->method != request->get_method() || impl->has_suspend) {
            ignore_called();
            return;
          }

          if VUNLIKELY (has_called()) {
            VLOG_F(*conf_ptr, "Two identical service requests.");
            return;
          }

          if (static_cast<ServerImpl*>(impl)->is_resp_type &&
              request->get_message_type() == someip::message_type_e::MT_REQUEST) {
            Bytes resp_data;

            callback(0, req_data, &resp_data);

            auto response = runtime_->create_response(request);
            auto payload = runtime_->create_payload(resp_data.data(), resp_data.size());

            response->set_payload(payload);

            app_->send(response);
          } else {
            callback(0, req_data, nullptr);
          }
        });
      });
}

SomeipServer::~SomeipServer() {
  app_->stop_offer_service(service_id_, instance_id_);
  app_->unregister_message_handler(service_id_, instance_id_, someip::ANY_METHOD);
  app_->clear_all_handler();
  app_->stop();

  if VLIKELY (thread_.joinable()) {
    thread_.join();
  }
}

std::any SomeipServer::get_native_handle() const { return app_.get(); }

std::shared_ptr<vsomeip_v3::application> SomeipServer::app() { return app_; }

void SomeipServer::start() {
  bool expected = false;

  if (has_started_.compare_exchange_strong(expected, true)) {
    app_->offer_service(service_id_, instance_id_);
    thread_ = std::thread([this] { app_->start(); });
  }
}

std::unordered_set<vsomeip_v3::client_t>& SomeipServer::get_clients() { return clients_; }

std::mutex& SomeipServer::get_client_mtx() { return client_mtx_; }

// SomeipClient
SomeipClient::SomeipClient(const SomeipID& id) {
  std::tie(std::ignore, service_id_, instance_id_) = id;

  runtime_ = someip::runtime::get();

  app_ = runtime_->create_application(Utils::get_app_name());

  if VUNLIKELY (!app_->init()) {
    VLOG_E("SomeipFactory: Failed to init app.");
    return;
  }

  // connected_ = app()->is_available(service_id_, instance_id_);
  app_->register_message_handler(service_id_, instance_id_, someip::ANY_EVENT,
                                 [this](const std::shared_ptr<someip::message>& message) {
                                   if (message->get_message_type() == someip::message_type_e::MT_NOTIFICATION) {
                                     auto payload = message->get_payload();
                                     Bytes msg_data = Bytes::shallow_copy(payload->get_data(), payload->get_length());

                                     traverse_msg_callback([&message, &msg_data](NodeImpl* impl, const auto& callback) {
                                       const auto* conf_ptr = impl->get_target_conf<SomeipConf>();

                                       if VUNLIKELY (conf_ptr->event != message->get_method() || impl->has_suspend) {
                                         return;
                                       }

                                       callback(msg_data);
                                     });
                                   } else if (message->get_message_type() == someip::message_type_e::MT_RESPONSE) {
                                     auto payload = message->get_payload();
                                     Bytes resp_data = Bytes::shallow_copy(payload->get_data(), payload->get_length());

                                     NodeImpl::MsgCallback cb;
                                     {
                                       std::lock_guard lock(mtx_);
                                       auto iter = resp_callbacks_.find(message->get_request());

                                       if VUNLIKELY (iter == resp_callbacks_.end()) {
                                         return;
                                       }

                                       cb = std::move(iter->second);
                                       resp_callbacks_.erase(iter);
                                     }

                                     if VLIKELY (cb) {
                                       cb(resp_data);
                                     }
                                   }
                                 });

  app_->register_availability_handler(
      service_id_, instance_id_, [this](someip::service_t service, someip::instance_t instance, bool is_available) {
        if VUNLIKELY (service != service_id_ || instance_id_ != instance) {
          return;
        }

        traverse_server_connect_callback([this, is_available](NodeImpl*, const auto& callback) {
          connected_ = is_available;

          callback(is_available);
        });
      });
}

SomeipClient::~SomeipClient() {
  app_->release_service(service_id_, instance_id_);
  app_->unregister_availability_handler(service_id_, instance_id_);
  app_->unregister_message_handler(service_id_, instance_id_, someip::ANY_METHOD);
  app_->clear_all_handler();
  app_->stop();

  if VLIKELY (thread_.joinable()) {
    thread_.join();
  }
}

std::any SomeipClient::get_native_handle() const { return app_.get(); }

std::shared_ptr<vsomeip_v3::application> SomeipClient::app() { return app_; }

void SomeipClient::start() {
  bool expected = false;

  if (has_started_.compare_exchange_strong(expected, true)) {
    app_->request_service(service_id_, instance_id_);

    thread_ = std::thread([this] { app_->start(); });
  }
}

bool SomeipClient::call(vsomeip_v3::method_t method, const Bytes& req_data, NodeImpl::MsgCallback&& callback,
                        uint64_t* seq_out) {
  auto request = someip::runtime::get()->create_request();

  request->set_service(service_id_);
  request->set_instance(instance_id_);
  request->set_method(method);
  request->set_client(app_->get_client());
  request->set_message_type(callback ? someip::message_type_e::MT_REQUEST
                                     : someip::message_type_e::MT_REQUEST_NO_RETURN);

  auto payload = someip::runtime::get()->create_payload(req_data.data(), req_data.size());
  request->set_payload(payload);

  if VLIKELY (callback) {
    std::lock_guard lock(mtx_);
    app_->send(request);

    uint64_t seq = request->get_request();

    if (seq_out) {
      *seq_out = seq;
    }

    resp_callbacks_[seq] = std::move(callback);
    return true;
  }

  app_->send(request);

  return true;
}

bool SomeipClient::is_connected() const { return connected_; }

void SomeipClient::remove_response_callback(uint64_t seq) {
  std::lock_guard lock(mtx_);
  resp_callbacks_.erase(seq);
}

}  // namespace vlink
