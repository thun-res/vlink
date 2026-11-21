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

#include "./fdbus_factory.h"

#include <algorithm>
#include <charconv>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "./base/utils.h"
#include "./impl/publisher_impl.h"
#include "./impl/server_impl.h"

namespace vlink {

// FdbusFactory
FdbusFactory::FdbusFactory() {
  Bytes::init_memory_pool();

  if VUNLIKELY (FdbusConf::get_thread_count() == 0) {
    VLOG_W("FdbusFactory: Fdbus does not support zero thread count.");

    FdbusConf::set_thread_count(1);
  }

  FDB_CONTEXT->start();

  workers_.reserve(FdbusConf::get_thread_count());

  for (size_t i = 0; i < FdbusConf::get_thread_count(); ++i) {
    auto worker = std::make_shared<fdbus::CBaseWorker>();
    worker->start();
    workers_.emplace_back(std::move(worker));
  }

  message_loop_.set_name("FDBUS-FACTORY");
  message_loop_.async_run();
}

FdbusFactory::~FdbusFactory() {
#ifdef _WIN32
  if (Utils::is_terminating()) {
    (void)::new (std::nothrow) auto(std::move(workers_));
    return;
  }
#endif

  message_loop_.quit();
  message_loop_.wait_for_quit();

  for (auto& worker : workers_) {
    worker->exit();
    worker->join();
  }
}

void FdbusFactory::set_worker(fdbus::CBaseEndpoint* end_point) {
  static auto& factory = FdbusFactory::get();

  std::lock_guard lock(factory.mtx_);

  size_t worker_index = factory.worker_nums_ % factory.workers_.size();
  ++factory.worker_nums_;
  auto* worker = factory.workers_[worker_index].get();

  end_point->worker(worker);
}

MessageLoop& FdbusFactory::get_message_loop() {
  static auto& factory = FdbusFactory::get();

  return factory.message_loop_;
}

// FdbusServer
FdbusServer::FdbusServer(const FdbusID& id) {
  const auto& [impl_type, transport, address] = id;

  FdbusFactory::set_worker(this);
  subscriber_timer_.attach(worker(), false);

  enableTCP(true);
  enableUDP(true);
  enableAysncRead(true);
  enableAysncWrite(true);
  enableBlockingMode(true);
  // enableReconnect(false);
  enableTCPSecure(false);
  keepAlive(1000, 3);

  const std::string& url = transport + "://" + address;
  FdbSocketId_t bind_id = bind(url.c_str());

  if VUNLIKELY (transport == "ipc" && bind_id == FDB_INVALID_ID) {
    VLOG_E("FdbusFactory: Server bind failed.");
  }
}

FdbusServer::~FdbusServer() {
  subscriber_timer_.attach(nullptr);
  prepareDestroy();
}

std::any FdbusServer::get_native_handle() const { return this; }

bool FdbusServer::has_clients() const { return has_clients_.load(std::memory_order_acquire); }

bool FdbusServer::has_subscribers(const FdbusConf& conf) {
  fdbus::tSubscribedSessionSets sessions;
  return context()->sendSyncEndeavor([&](fdbus::CBaseJob::Ptr&) {
    getSubscribeTable(static_cast<int32_t>(conf.hash_code), conf.event.c_str(), sessions);
  }) && !sessions.empty();
}

bool FdbusServer::monitor_subscribers(const FdbusConf& conf) {
  worker()->sendAsync([weak = weak_from_this()](fdbus::CBaseJob::Ptr&) {
    if (auto self = weak.lock()) {
      self->subscriber_timer_.enable();
    }
  });

  return has_subscribers(conf);
}

void FdbusServer::refresh_subscribers(fdbus::CMethodLoopTimer<FdbusServer>* timer) {
  auto self = weak_from_this().lock();

  if VUNLIKELY (!self) {
    return;
  }

  if (sub_connect_map_is_empty()) {
    if (timer) {
      timer->disable();
    }

    return;
  }

  if (!has_clients()) {
    bool has_publishers = false;

    traverse_sub_connect_callback([&](NodeImpl* impl, const auto& callback) {
      auto* publisher = dynamic_cast<PublisherImpl*>(impl);

      if (!publisher) {
        return;
      }

      has_publishers = true;

      if (publisher->has_subscribers()) {
        callback(false);
      }
    });

    if (timer && !has_publishers) {
      timer->disable();
    }

    return;
  }

  fdbus::tFdbSubscribeMsgTbl subscriptions;

  if VUNLIKELY (!context()->sendSyncEndeavor([&](fdbus::CBaseJob::Ptr&) { getSubscribeTable(subscriptions); })) {
    return;
  }

  bool has_publishers = false;
  traverse_sub_connect_callback([&](NodeImpl* impl, const auto& callback) {
    auto* publisher = dynamic_cast<PublisherImpl*>(impl);

    if (!publisher) {
      return;
    }

    has_publishers = true;
    const auto* conf = impl->get_target_conf<FdbusConf>();
    const auto matches = [&](int32_t code) {
      const auto iter = subscriptions.find(code);
      return iter != subscriptions.end() && (iter->second.count(conf->event) != 0 || iter->second.count("") != 0);
    };
    const auto code = static_cast<int32_t>(conf->hash_code);
    const bool present =
        matches(code) || matches(fdbMakeGroup(code)) || matches(fdbMakeGroupEvent(FDB_EVENT_ALL_GROUPS));

    if (present != publisher->has_subscribers()) {
      callback(present);
    }
  });

  if (timer && !has_publishers) {
    timer->disable();
  }
}

void FdbusServer::onOnline(const fdbus::CFdbOnlineInfo& info) {
  has_clients_.store(true, std::memory_order_release);
  fdbus::CBaseServer::onOnline(info);
}

void FdbusServer::onSubscribe(fdbus::CBaseJob::Ptr& msg_ref) {
  using namespace fdbus;  // NOLINT(build/namespaces, google-build-using-namespace)

  auto* msg = castToMessage<CFdbMessage*>(msg_ref);
  const CFdbMsgSubscribeItem* sub_item = nullptr;

  FDB_BEGIN_FOREACH_SIGNAL(msg, sub_item) {
    traverse_sub_connect_callback([this, sub_item](NodeImpl* impl, const auto& callback) {
      const auto* conf_ptr = impl->get_target_conf<FdbusConf>();
      const auto code = static_cast<int32_t>(conf_ptr->hash_code);
      const auto subscribed = sub_item->msg_code();

      if ((subscribed != code && subscribed != fdbMakeGroup(code) &&
           subscribed != fdbMakeGroupEvent(FDB_EVENT_ALL_GROUPS)) ||
          (sub_item->has_topic() && !sub_item->topic().empty() && conf_ptr->event != sub_item->topic())) {
        return;
      }

      callback(has_clients());
    });
  }
  FDB_END_FOREACH_SIGNAL()
}

void FdbusServer::onOffline(const fdbus::CFdbOnlineInfo& info) {
  const bool connected = !info.mFirstOrLast;
  has_clients_.store(connected, std::memory_order_release);

  fdbus::CBaseServer::onOffline(info);

  if (connected) {
    refresh_subscribers(nullptr);
  } else {
    traverse_sub_connect_callback([](NodeImpl*, const auto& callback) { callback(false); });
  }
}

void FdbusServer::onInvoke(fdbus::CBaseJob::Ptr& msg_ref) {
  auto* msg = fdbus::castToMessage<fdbus::CBaseMessage*>(msg_ref);
  Bytes req_data = Bytes::shallow_copy(msg->getPayloadBuffer(), msg->getPayloadSize());
  NodeImpl* owner = nullptr;
  NodeImpl::LoopGatePtr loop_gate;
  MessageLoop* message_loop = nullptr;

  traverse_req_resp_callback([&](NodeImpl* impl, const auto& callback) {
    const auto* conf_ptr = impl->get_target_conf<FdbusConf>();

    if VUNLIKELY (static_cast<int32_t>(conf_ptr->hash_code) != msg->code() || impl->has_suspend) {
      ignore_called();
      return;
    }

    if VUNLIKELY (has_called()) {
      VLOG_F(*conf_ptr, "Two identical service requests.");
      return;
    }

    message_loop = impl->get_message_loop();

    if (message_loop) {
      owner = impl;
      loop_gate = impl->get_loop_gate();
      return;
    }

    if (static_cast<ServerImpl*>(impl)->is_resp_type && msg->needReply()) {
      Bytes resp_data;

      callback(0, req_data, &resp_data);

      fdbus::CBaseMessage::reply(msg_ref, resp_data.data(), resp_data.size());
    } else {
      callback(0, req_data, nullptr);
    }
  });

  if (message_loop) {
    const bool posted = NodeImpl::post_to_loop_gate(
        loop_gate, message_loop, [weak = weak_from_this(), owner, message_loop, request_ref = msg_ref]() mutable {
          auto self = weak.lock();

          auto reply_unavailable = [&request_ref]() {
            auto* request = fdbus::castToMessage<fdbus::CBaseMessage*>(request_ref);

            if (request && request->needReply()) {
              fdbus::CBaseMessage::status(request_ref, fdbus::FDB_ST_NON_EXIST, "Service unavailable.");
            }
          };

          if VUNLIKELY (!self) {
            reply_unavailable();
            return;
          }

          bool handled = false;

          self->invoke_req_resp_callback(owner, [&](NodeImpl* target, const auto& target_callback) {
            if VUNLIKELY (target->get_message_loop() != message_loop ||
                          target->has_suspend.load(std::memory_order_relaxed)) {
              return;
            }

            handled = true;

            auto* request = fdbus::castToMessage<fdbus::CBaseMessage*>(request_ref);
            Bytes data = Bytes::shallow_copy(request->getPayloadBuffer(), request->getPayloadSize());

            if (static_cast<ServerImpl*>(target)->is_resp_type && request->needReply()) {
              Bytes response;

              target_callback(0, data, &response);
              fdbus::CBaseMessage::reply(request_ref, response.data(), response.size());
            } else {
              target_callback(0, data, nullptr);
            }
          });

          if VUNLIKELY (!handled) {
            reply_unavailable();
          }
        });

    if VUNLIKELY (!posted && msg->needReply()) {
      fdbus::CBaseMessage::status(msg_ref, fdbus::FDB_ST_NON_EXIST, "Service unavailable.");
    }
  }
}

// FdbusClient
FdbusClient::FdbusClient(const FdbusID& id) {
  const auto& [impl_type, transport, address] = id;

  FdbusFactory::set_worker(this);

  enableTCP(true);
  enableUDP(true);
  enableAysncRead(true);
  enableAysncWrite(true);
  enableBlockingMode(true);
  enableReconnect(false);
  enableTCPSecure(false);
  keepAlive(1000, 3);

  url_ = transport + "://" + address;
  connect(url_.c_str());

  if (transport == "ipc") {
    timer_.set_interval(10);
    timer_.set_loop_count(Timer::kInfinite);
    timer_.attach(&FdbusFactory::get_message_loop());
  }
}

FdbusClient::~FdbusClient() {
  quit_flag_.store(true, std::memory_order_release);

  prepareDestroy();

  timer_.stop();
  timer_.detach();
}

std::any FdbusClient::get_native_handle() const { return this; }

bool FdbusClient::is_connected() const { return connected_.load(std::memory_order_acquire); }

void FdbusClient::remove_subscriber(NodeImpl* owner) {
  invoke_callback(owner, [&] {
    const auto* conf = owner->get_target_conf<FdbusConf>();
    bool subscribed = false;
    bool shared = false;
    traverse_server_connect_callback([&](NodeImpl* impl, const auto&) {
      if (impl == owner) {
        subscribed = true;
      } else if (impl->init_impl_type != kClient && impl->get_target_conf<FdbusConf>()->event == conf->event) {
        shared = true;
      }
    });

    remove_impl(owner);

    if (subscribed && !shared && is_connected()) {
      fdbus::CFdbMsgSubscribeList subscriptions;
      subscriptions.addNotifyItem(static_cast<int32_t>(conf->hash_code), conf->event.c_str());
      unsubscribe(subscriptions, FDB_QOS_RELIABLE);
    }
  });
}

void FdbusClient::start_timer() {
  if (!timer_.get_message_loop()) {
    return;
  }

  bool expected = false;

  if (!timer_started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_relaxed)) {
    return;
  }

  timer_.start([weak_self = weak_from_this()]() {
    auto self = weak_self.lock();

    if VUNLIKELY (!self) {
      return;
    }

    if VUNLIKELY (self->quit_flag_.load(std::memory_order_acquire)) {
      return;
    }

    if (!self->is_connected()) {
      self->timer_.set_interval(50);
      self->connect(self->url_.c_str());
    } else {
      self->timer_.set_interval(100);
    }
  });
}

bool FdbusClient::call(NodeImpl* owner, uint32_t channel, const Bytes& req_data, NodeImpl::MsgCallback&& callback,
                       int32_t timeout_ms, bool dispatch, Function<void()>&& error_callback) {
  if VUNLIKELY (!callback) {
    return send(channel, req_data.data(), req_data.size(), FDB_QOS_RELIABLE);
  }

  if (!dispatch) {
    return invoke(
        channel,
        [channel, callback = std::move(callback), error_callback = std::move(error_callback)](
            fdbus::CBaseJob::Ptr& msg_ref, fdbus::CFdbBaseObject*) mutable {
          auto* msg = fdbus::castToMessage<fdbus::CBaseMessage*>(msg_ref);

          if VUNLIKELY (msg->isStatus() || msg->code() != static_cast<int32_t>(channel)) {
            if (error_callback) {
              error_callback();
            }

            return;
          }

          callback(Bytes::shallow_copy(msg->getPayloadBuffer(), msg->getPayloadSize()));
        },
        req_data.data(), req_data.size(), nullptr, timeout_ms, FDB_QOS_RELIABLE);
  }

  return invoke(
      channel,
      [weak_self = weak_from_this(), owner, channel, callback = std::move(callback)](fdbus::CBaseJob::Ptr& msg_ref,
                                                                                     fdbus::CFdbBaseObject*) mutable {
        auto* msg = fdbus::castToMessage<fdbus::CBaseMessage*>(msg_ref);

        if VUNLIKELY (msg->isStatus() || msg->code() != static_cast<int32_t>(channel)) {
          return;
        }

        auto self = weak_self.lock();

        if VUNLIKELY (!self) {
          return;
        }

        MessageLoop* message_loop = nullptr;
        NodeImpl::LoopGatePtr loop_gate;

        self->invoke_callback(owner, [&] {
          message_loop = owner->get_message_loop();

          if (!message_loop) {
            callback(Bytes::shallow_copy(msg->getPayloadBuffer(), msg->getPayloadSize()));
            return;
          }

          loop_gate = owner->get_loop_gate();
        });

        if (message_loop) {
          NodeImpl::post_to_loop_gate(
              loop_gate, message_loop,
              [weak_self = std::move(weak_self), owner, callback = std::move(callback), msg_ref,
               message_loop]() mutable {
                auto self = weak_self.lock();
                bool attached = false;

                if (self) {
                  self->invoke_callback(owner, [&] { attached = owner->get_message_loop() == message_loop; });
                }

                if VLIKELY (attached) {
                  auto* response = fdbus::castToMessage<fdbus::CBaseMessage*>(msg_ref);

                  callback(Bytes::shallow_copy(response->getPayloadBuffer(), response->getPayloadSize()));
                }
              });
        }
      },
      req_data.data(), req_data.size(), nullptr, timeout_ms, FDB_QOS_RELIABLE);
}

void FdbusClient::onOnline(const fdbus::CFdbOnlineInfo& info) {
  (void)info;

  connected_.store(true, std::memory_order_release);
  traverse_server_connect_callback([](NodeImpl*, const auto& callback) { callback(true); });
}

void FdbusClient::onOffline(const fdbus::CFdbOnlineInfo& info) {
  const bool connected = !info.mFirstOrLast;
  connected_.store(connected, std::memory_order_release);

  traverse_server_connect_callback([connected](NodeImpl*, const auto& callback) { callback(connected); });
}

void FdbusClient::onBroadcast(fdbus::CBaseJob::Ptr& msg_ref) {
  auto* msg = fdbus::castToMessage<fdbus::CBaseMessage*>(msg_ref);
  Bytes msg_data = Bytes::shallow_copy(msg->getPayloadBuffer(), msg->getPayloadSize());
  LoopRoute first_route;
  std::vector<LoopRoute> other_routes;

  traverse_msg_callback([&](NodeImpl* impl, const auto& callback) {
    const auto* conf_ptr = impl->get_target_conf<FdbusConf>();

    if VUNLIKELY (static_cast<int32_t>(conf_ptr->hash_code) != msg->code() || impl->has_suspend ||
                  conf_ptr->event != msg->topic()) {
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
    post_to_subscriber_loop(target, [weak = weak_from_this(), message_ref = msg_ref, loop = target.loop]() mutable {
      auto self = weak.lock();

      if VUNLIKELY (!self) {
        return;
      }

      auto* message = fdbus::castToMessage<fdbus::CBaseMessage*>(message_ref);
      Bytes data = Bytes::shallow_copy(message->getPayloadBuffer(), message->getPayloadSize());

      self->traverse_msg_callback([&](NodeImpl* impl, const auto& callback) {
        const auto* conf = impl->get_target_conf<FdbusConf>();

        if (impl->get_message_loop() == loop && !impl->has_suspend &&
            static_cast<int32_t>(conf->hash_code) == message->code() && conf->event == message->topic()) {
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
}

}  // namespace vlink
