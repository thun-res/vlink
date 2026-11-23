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

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "./base/message_loop.h"
#include "./extension/someip_serializer.h"
#include "./impl/server_impl.h"

#ifdef VSOMEIP_DEPRECATED_UID_GID
#define VSOMEIP_SUB_HANDLE_ARG const vsomeip_sec_client_t*, const std::string&
#else
#define VSOMEIP_SUB_HANDLE_ARG someip::uid_t, someip::gid_t
#endif

namespace vlink {

// SomeipFactory
SomeipFactory::SomeipFactory() {
  Bytes::init_memory_pool();

  if VUNLIKELY (SomeipTransport::get_thread_count() == 0) {
    VLOG_W("SomeipFactory: Someip does not support zero thread count.");

    SomeipTransport::set_thread_count(1);
  }

  // NOLINTNEXTLINE(readability-static-accessed-through-instance)
  someip::runtime::get()->set_property("threads", std::to_string(SomeipTransport::get_thread_count()));

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

  if VUNLIKELY (!app_ || !app_->init()) {
    VLOG_E("SomeipFactory: Failed to init app.");
    return;
  }

  initialized_.store(true, std::memory_order_release);

  app_->register_message_handler(
      service_id_, instance_id_, someip::ANY_METHOD, [this](const std::shared_ptr<someip::message>& request) {
        if VUNLIKELY (request->get_message_type() != someip::message_type_e::MT_REQUEST &&
                      request->get_message_type() != someip::message_type_e::MT_REQUEST_NO_RETURN) {
          return;
        }

        NodeImpl* owner = nullptr;
        NodeImpl::LoopGatePtr loop_gate;
        MessageLoop* message_loop = nullptr;

        traverse_req_resp_callback([&](NodeImpl* impl, const auto& callback) {
          const auto* transport_ptr = impl->get_target_transport<SomeipTransport>();

          if VUNLIKELY (transport_ptr->method != request->get_method() || impl->has_suspend) {
            ignore_called();
            return;
          }

          if VUNLIKELY (has_called()) {
            VLOG_F(*transport_ptr, "Two identical service requests.");
            return;
          }

          message_loop = impl->get_message_loop();

          if (message_loop) {
            owner = impl;
            loop_gate = impl->get_loop_gate();
            return;
          }

          process_request(impl, request, callback);
        });

        if (message_loop) {
          const bool posted = NodeImpl::post_to_loop_gate(
              loop_gate, message_loop, [weak = weak_from_this(), request = request, owner, message_loop]() {
                auto self = weak.lock();

                if VUNLIKELY (!self) {
                  return;
                }

                bool handled = false;

                self->invoke_req_resp_callback(owner, [&](NodeImpl* target, const auto& target_callback) {
                  if VLIKELY (target->get_message_loop() == message_loop && !target->has_suspend) {
                    handled = true;
                    self->process_request(target, request, target_callback);
                  }
                });

                if VUNLIKELY (!handled && request->get_message_type() == someip::message_type_e::MT_REQUEST &&
                              self->runtime_ && self->app_ && self->is_initialized()) {
                  auto error_resp = self->runtime_->create_response(request);
                  error_resp->set_message_type(someip::message_type_e::MT_ERROR);
                  error_resp->set_return_code(someip::return_code_e::E_NOT_READY);
                  self->app_->send(error_resp);
                }
              });

          if VUNLIKELY (!posted && request->get_message_type() == someip::message_type_e::MT_REQUEST) {
            auto error_resp = runtime_->create_response(request);
            error_resp->set_message_type(someip::message_type_e::MT_ERROR);
            error_resp->set_return_code(someip::return_code_e::E_NOT_READY);
            app_->send(error_resp);
          }
        }
      });
}

void SomeipServer::process_request(NodeImpl* impl, const std::shared_ptr<someip::message>& request,
                                   const NodeImpl::ReqRespCallback& callback) {
  auto payload = request->get_payload();
  Bytes req_data = payload ? Bytes::shallow_copy(payload->get_data(), payload->get_length()) : Bytes();

  if (static_cast<ServerImpl*>(impl)->is_resp_type &&
      request->get_message_type() == someip::message_type_e::MT_REQUEST) {
    Bytes resp_data;

    callback(0, req_data, &resp_data);

    if VUNLIKELY (resp_data.size() > SomeipSerializer::kMaxPayloadSize) {
      VLOG_E(*impl->get_target_transport<SomeipTransport>(), "SOME/IP response payload exceeds the protocol limit.");
      return;
    }

    auto response = runtime_->create_response(request);

    response->set_payload(resp_data.empty()
                              ? runtime_->create_payload()
                              : runtime_->create_payload(resp_data.data(), static_cast<uint32_t>(resp_data.size())));

    app_->send(response);
  } else {
    callback(0, req_data, nullptr);
  }
}

SomeipServer::~SomeipServer() {
  if (app_ && initialized_.load(std::memory_order_acquire)) {
    app_->stop_offer_service(service_id_, instance_id_);
    app_->unregister_message_handler(service_id_, instance_id_, someip::ANY_METHOD);
    app_->clear_all_handler();

    if (has_started_.load(std::memory_order_acquire)) {
      app_->stop();
    }
  }

  if (thread_.joinable()) {
    if (thread_.get_id() == std::this_thread::get_id()) {
      thread_.detach();
    } else {
      thread_.join();
    }
  }
}

std::any SomeipServer::get_native_handle() const { return app_.get(); }

std::shared_ptr<someip::application> SomeipServer::app() { return app_; }

void SomeipServer::start() {
  if VUNLIKELY (!initialized_.load(std::memory_order_acquire)) {
    return;
  }

  bool expected = false;

  if (has_started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_relaxed)) {
    app_->offer_service(service_id_, instance_id_);
    thread_ = std::thread([app = app_, runtime = runtime_]() {
      (void)runtime;
      app->start();
    });
  }
}

std::unordered_map<someip::eventgroup_t, std::unordered_set<someip::client_t>>& SomeipServer::get_clients() {
  return clients_;
}

std::mutex& SomeipServer::get_client_mtx() { return client_mtx_; }

void SomeipServer::acquire_event(someip::event_t event, const std::set<someip::eventgroup_t>& groups,
                                 someip::event_type_e event_type, bool register_sub_handler) {
  auto current_app = app_;

  if VUNLIKELY (!current_app || !is_initialized()) {
    return;
  }

  std::lock_guard event_lock(event_mtx_);

  auto& entry = event_entries_[event];
  const bool first_event = entry.ref_count == 0U;

  if (first_event) {
    entry.event_type = event_type;
  }

  ++entry.ref_count;
  bool groups_expanded = false;

  for (auto g : groups) {
    if (++entry.group_refs[g] == 1U) {
      groups_expanded = true;
    }
  }

  if (first_event) {
    current_app->offer_event(service_id_, instance_id_, event, groups, entry.event_type);
  } else if VUNLIKELY (groups_expanded) {
    std::set<someip::eventgroup_t> active_groups;

    for (const auto& [g, count] : entry.group_refs) {
      if (count > 0U) {
        active_groups.emplace(g);
      }
    }

    current_app->stop_offer_event(service_id_, instance_id_, event);
    current_app->offer_event(service_id_, instance_id_, event, active_groups, entry.event_type);
  }

  if (!register_sub_handler) {
    return;
  }

  for (auto g : groups) {
    if (++sub_group_refs_[g] != 1U) {
      continue;
    }

    current_app->register_subscription_handler(
        service_id_, instance_id_, g,
        [weak = weak_from_this(), g](someip::client_t client_id, VSOMEIP_SUB_HANDLE_ARG, bool is_reg) {
          auto object = weak.lock();

          if VUNLIKELY (!object) {
            return false;
          }

          {
            std::lock_guard lock(object->get_client_mtx());
            auto& clients = object->get_clients()[g];

            if (is_reg) {
              clients.emplace(client_id);
            } else {
              clients.erase(client_id);
            }
          }

          object->traverse_sub_connect_callback([is_reg](NodeImpl*, const auto& callback) { callback(is_reg); });

          return is_reg;
        });
  }
}

void SomeipServer::release_event(someip::event_t event, const std::set<someip::eventgroup_t>& groups,
                                 bool unregister_sub_handler) {
  auto current_app = app_;
  const bool app_ready = current_app && is_initialized();

  std::lock_guard event_lock(event_mtx_);

  if (unregister_sub_handler) {
    for (auto g : groups) {
      auto iter = sub_group_refs_.find(g);

      if (iter == sub_group_refs_.end()) {
        continue;
      }

      if (--iter->second > 0U) {
        continue;
      }

      sub_group_refs_.erase(iter);

      if (app_ready) {
        current_app->unregister_subscription_handler(service_id_, instance_id_, g);
      }

      std::lock_guard lock(client_mtx_);
      clients_.erase(g);
    }
  }

  auto event_iter = event_entries_.find(event);

  if (event_iter != event_entries_.end()) {
    auto& entry = event_iter->second;

    for (auto g : groups) {
      auto g_iter = entry.group_refs.find(g);

      if (g_iter != entry.group_refs.end() && --g_iter->second == 0U) {
        entry.group_refs.erase(g_iter);
      }
    }

    if (--entry.ref_count == 0U) {
      event_entries_.erase(event_iter);

      if (app_ready) {
        current_app->stop_offer_event(service_id_, instance_id_, event);
      }
    }
  }
}

// SomeipClient
SomeipClient::SomeipClient(const SomeipID& id) {
  std::tie(std::ignore, service_id_, instance_id_) = id;

  runtime_ = someip::runtime::get();

  app_ = runtime_->create_application(Utils::get_app_name());

  if VUNLIKELY (!app_ || !app_->init()) {
    VLOG_E("SomeipFactory: Failed to init app.");
    return;
  }

  initialized_.store(true, std::memory_order_release);

  // connected_ = app()->is_available(service_id_, instance_id_);
  app_->register_message_handler(
      service_id_, instance_id_, someip::ANY_EVENT, [this](const std::shared_ptr<someip::message>& message) {
        if (message->get_message_type() == someip::message_type_e::MT_NOTIFICATION) {
          auto payload = message->get_payload();
          Bytes msg_data = payload ? Bytes::shallow_copy(payload->get_data(), payload->get_length()) : Bytes();
          LoopRoute first_route;
          std::vector<LoopRoute> other_routes;

          traverse_msg_callback([&](NodeImpl* impl, const auto& callback) {
            const auto* transport = impl->get_target_transport<SomeipTransport>();

            if VUNLIKELY (transport->event != message->get_method() || impl->has_suspend) {
              return;
            }

            auto* loop = impl->get_message_loop();

            if (!loop) {
              callback(msg_data);
            } else if (!first_route.loop) {
              first_route = {loop, impl->get_loop_gate()};
            } else if (loop != first_route.loop &&
                       std::find(other_routes.begin(), other_routes.end(), loop) == other_routes.end()) {
              other_routes.push_back({loop, impl->get_loop_gate()});
            }
          });

          auto post = [&](const LoopRoute& target) {
            post_to_subscriber_loop(target, [weak = weak_from_this(), message = message, loop = target.loop]() {
              auto self = weak.lock();

              if VUNLIKELY (!self) {
                return;
              }

              auto payload = message->get_payload();
              Bytes data = payload ? Bytes::shallow_copy(payload->get_data(), payload->get_length()) : Bytes();

              self->traverse_msg_callback([&](NodeImpl* impl, const auto& callback) {
                const auto* transport = impl->get_target_transport<SomeipTransport>();

                if (impl->get_message_loop() == loop && !impl->has_suspend &&
                    transport->event == message->get_method()) {
                  callback(data);
                }
              });
            });
          };

          if (first_route.loop) {
            post(first_route);
          }

          for (const auto& route : other_routes) {
            post(route);
          }
        } else if (message->get_message_type() == someip::message_type_e::MT_RESPONSE) {
          auto payload = message->get_payload();
          Bytes resp_data = payload ? Bytes::shallow_copy(payload->get_data(), payload->get_length()) : Bytes();

          ResponseCallback response;
          {
            std::lock_guard lock(mtx_);
            auto iter = resp_callbacks_.find(message->get_request());

            if VUNLIKELY (iter == resp_callbacks_.end()) {
              return;
            }

            response = std::move(iter->second);
            resp_callbacks_.erase(iter);
          }

          if (!response.dispatch) {
            response.callback(resp_data);

            return;
          }

          MessageLoop* message_loop = nullptr;
          NodeImpl::LoopGatePtr loop_gate;

          invoke_callback(response.owner, [&]() {
            message_loop = response.owner->get_message_loop();

            if (!message_loop) {
              response.callback(resp_data);
              return;
            }

            loop_gate = response.owner->get_loop_gate();
          });

          if (message_loop) {
            NodeImpl::post_to_loop_gate(
                loop_gate, message_loop,
                [weak = weak_from_this(), owner = response.owner, message_loop, callback = std::move(response.callback),
                 message = message]() {
                  auto self = weak.lock();

                  if VUNLIKELY (!self) {
                    return;
                  }

                  bool attached = false;
                  self->invoke_callback(owner, [&]() { attached = owner->get_message_loop() == message_loop; });

                  if VLIKELY (attached) {
                    auto payload = message->get_payload();

                    callback(payload ? Bytes::shallow_copy(payload->get_data(), payload->get_length()) : Bytes());
                  }
                });
          }
        } else if (message->get_message_type() == someip::message_type_e::MT_ERROR) {
          ResponseCallback response;
          {
            std::lock_guard lock(mtx_);
            auto iter = resp_callbacks_.find(message->get_request());

            if VUNLIKELY (iter == resp_callbacks_.end()) {
              return;
            }

            response = std::move(iter->second);
            resp_callbacks_.erase(iter);
          }

          if (response.error_callback) {
            response.error_callback();
          }
        }
      });

  app_->register_availability_handler(
      service_id_, instance_id_, [this](someip::service_t service, someip::instance_t instance, bool is_available) {
        if VUNLIKELY (service != service_id_ || instance_id_ != instance) {
          return;
        }

        connected_.store(is_available, std::memory_order_release);

        traverse_server_connect_callback([is_available](NodeImpl*, const auto& callback) { callback(is_available); });
      });
}

SomeipClient::~SomeipClient() {
  if (app_ && initialized_.load(std::memory_order_acquire)) {
    app_->release_service(service_id_, instance_id_);
    app_->unregister_availability_handler(service_id_, instance_id_);
    app_->unregister_message_handler(service_id_, instance_id_, someip::ANY_EVENT);
    app_->clear_all_handler();

    if (has_started_.load(std::memory_order_acquire)) {
      app_->stop();
    }
  }

  if (thread_.joinable()) {
    if (thread_.get_id() == std::this_thread::get_id()) {
      thread_.detach();
    } else {
      thread_.join();
    }
  }
}

std::any SomeipClient::get_native_handle() const { return app_.get(); }

std::shared_ptr<someip::application> SomeipClient::app() { return app_; }

void SomeipClient::start() {
  if VUNLIKELY (!initialized_.load(std::memory_order_acquire)) {
    return;
  }

  bool expected = false;

  if (has_started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_relaxed)) {
    app_->request_service(service_id_, instance_id_);

    thread_ = std::thread([app = app_, runtime = runtime_]() {
      (void)runtime;
      app->start();
    });
  }
}

bool SomeipClient::call(NodeImpl* owner, someip::method_t method, const Bytes& req_data,
                        NodeImpl::MsgCallback&& callback, uint64_t* seq_out, bool dispatch,
                        MoveFunction<void()>&& error_callback) {
  if VUNLIKELY (!initialized_.load(std::memory_order_acquire)) {
    return false;
  }

  if VUNLIKELY (req_data.size() > SomeipSerializer::kMaxPayloadSize) {
    VLOG_E("SomeipClient: Request payload exceeds the protocol limit.");
    return false;
  }

  auto request = runtime_->create_request();

  request->set_service(service_id_);
  request->set_instance(instance_id_);
  request->set_method(method);
  request->set_client(app_->get_client());
  request->set_message_type(callback ? someip::message_type_e::MT_REQUEST
                                     : someip::message_type_e::MT_REQUEST_NO_RETURN);

  auto payload = req_data.empty() ? runtime_->create_payload()
                                  : runtime_->create_payload(req_data.data(), static_cast<uint32_t>(req_data.size()));
  request->set_payload(payload);

  if VLIKELY (callback) {
    std::lock_guard lock(mtx_);
    app_->send(request);

    uint64_t seq = request->get_request();

    if (seq_out) {
      *seq_out = seq;
    }

    resp_callbacks_[seq] = ResponseCallback{owner, dispatch, std::move(callback), std::move(error_callback)};
    return true;
  }

  app_->send(request);

  return true;
}

void SomeipClient::remove_response_callback(uint64_t seq) {
  std::lock_guard lock(mtx_);
  resp_callbacks_.erase(seq);
}

void SomeipClient::cancel_calls(NodeImpl* owner) {
  std::lock_guard lock(mtx_);

  for (auto it = resp_callbacks_.begin(); it != resp_callbacks_.end();) {
    if (it->second.owner == owner) {
      it = resp_callbacks_.erase(it);
    } else {
      ++it;
    }
  }
}

bool SomeipClient::is_connected() const { return connected_.load(std::memory_order_acquire); }

void SomeipClient::acquire_event(someip::event_t event, const std::set<someip::eventgroup_t>& groups,
                                 someip::event_type_e event_type) {
  auto current_app = app_;

  if VUNLIKELY (!current_app || !is_initialized()) {
    return;
  }

  std::lock_guard event_lock(event_mtx_);

  auto& entry = event_entries_[event];
  const bool first_event = entry.ref_count == 0U;

  if (first_event) {
    entry.event_type = event_type;
  }

  ++entry.ref_count;
  bool groups_expanded = false;

  for (auto g : groups) {
    if (++entry.group_refs[g] == 1U) {
      groups_expanded = true;
    }

    ++sub_group_refs_[g];
  }

  if (first_event) {
    current_app->request_event(service_id_, instance_id_, event, groups, entry.event_type);
  } else if VUNLIKELY (groups_expanded) {
    std::set<someip::eventgroup_t> active_groups;

    for (const auto& [g, count] : entry.group_refs) {
      if (count > 0U) {
        active_groups.emplace(g);
      }
    }

    current_app->release_event(service_id_, instance_id_, event);
    current_app->request_event(service_id_, instance_id_, event, active_groups, entry.event_type);
  }
}

void SomeipClient::release_event(someip::event_t event, const std::set<someip::eventgroup_t>& groups) {
  auto current_app = app_;
  const bool app_ready = current_app && is_initialized();

  std::lock_guard event_lock(event_mtx_);

  for (auto g : groups) {
    auto iter = sub_group_refs_.find(g);

    if (iter == sub_group_refs_.end()) {
      continue;
    }

    if (--iter->second > 0U) {
      continue;
    }

    sub_group_refs_.erase(iter);

    if (app_ready) {
      current_app->unsubscribe(service_id_, instance_id_, g);
    }
  }

  auto event_iter = event_entries_.find(event);

  if (event_iter != event_entries_.end()) {
    auto& entry = event_iter->second;

    for (auto g : groups) {
      auto g_iter = entry.group_refs.find(g);

      if (g_iter != entry.group_refs.end() && --g_iter->second == 0U) {
        entry.group_refs.erase(g_iter);
      }
    }

    if (--entry.ref_count == 0U) {
      event_entries_.erase(event_iter);

      if (app_ready) {
        current_app->release_event(service_id_, instance_id_, event);
      }
    }
  }
}

}  // namespace vlink
