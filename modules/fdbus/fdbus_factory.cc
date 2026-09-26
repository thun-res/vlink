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

#include "./base/utils.h"
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

FdbusServer::~FdbusServer() { prepareDestroy(); }

std::any FdbusServer::get_native_handle() const { return this; }

void FdbusServer::onSubscribe(fdbus::CBaseJob::Ptr& msg_ref) {
  using namespace fdbus;  // NOLINT(build/namespaces, google-build-using-namespace)

  auto* msg = castToMessage<CFdbMessage*>(msg_ref);
  const CFdbMsgSubscribeItem* sub_item = nullptr;

  FDB_BEGIN_FOREACH_SIGNAL(msg, sub_item) {
    if VUNLIKELY (!sub_item->has_topic()) {
      continue;
    }

    traverse_sub_connect_callback([this, sub_item](NodeImpl* impl, const auto& callback) {
      const auto* conf_ptr = impl->get_target_conf<FdbusConf>();

      if (static_cast<int32_t>(conf_ptr->hash_code) != sub_item->msg_code() || conf_ptr->event != sub_item->topic()) {
        return;
      }

      callback(getSessionCount() > 0);
    });
  }
  FDB_END_FOREACH_SIGNAL()
}

void FdbusServer::onOffline(const fdbus::CFdbOnlineInfo& info) {
  (void)info;

  traverse_sub_connect_callback([this](NodeImpl*, const auto& callback) { callback(getSessionCount() > 0); });
}

void FdbusServer::onInvoke(fdbus::CBaseJob::Ptr& msg_ref) {
  auto* msg = fdbus::castToMessage<fdbus::CBaseMessage*>(msg_ref);
  Bytes req_data = Bytes::shallow_copy(msg->getPayloadBuffer(), msg->getPayloadSize());
  NodeImpl* owner = nullptr;
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

    owner = impl;
    message_loop = impl->get_message_loop();

    if (message_loop) {
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
    message_loop->post_task([weak = weak_from_this(), owner, message_loop, request_ref = msg_ref]() mutable {
      auto self = weak.lock();

      if VUNLIKELY (!self) {
        return;
      }

      self->invoke_req_resp_callback(owner, [&](NodeImpl* impl, const auto& callback) {
        if (impl->get_message_loop() != message_loop || impl->has_suspend) {
          return;
        }

        auto* request = fdbus::castToMessage<fdbus::CBaseMessage*>(request_ref);
        Bytes data = Bytes::shallow_copy(request->getPayloadBuffer(), request->getPayloadSize());

        if (static_cast<ServerImpl*>(impl)->is_resp_type && request->needReply()) {
          Bytes response;

          callback(0, data, &response);
          fdbus::CBaseMessage::reply(request_ref, response.data(), response.size());
        } else {
          callback(0, data, nullptr);
        }
      });
    });
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

void FdbusClient::start_timer() {
  if (!timer_.get_message_loop()) {
    return;
  }

  bool expected = false;

  if (!timer_started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_relaxed)) {
    return;
  }

  std::weak_ptr<FdbusClient> weak_self = weak_from_this();

  timer_.start([weak_self]() {
    auto self = weak_self.lock();

    if VUNLIKELY (!self) {
      return;
    }

    if VUNLIKELY (self->quit_flag_.load(std::memory_order_acquire)) {
      return;
    }

    if (!self->connected(FDB_SEC_NO_CHECK)) {
      self->timer_.set_interval(50);
      self->connect(self->url_.c_str());
    } else {
      self->timer_.set_interval(100);
    }
  });
}

bool FdbusClient::call(NodeImpl* owner, uint32_t channel, const Bytes& req_data, NodeImpl::MsgCallback&& callback,
                       int32_t timeout_ms, bool dispatch) {
  if VUNLIKELY (!callback) {
    return send(channel, req_data.data(), req_data.size(), FDB_QOS_RELIABLE);
  }

  if (!dispatch) {
    return invoke(
        channel,
        [channel, callback = std::move(callback)](fdbus::CBaseJob::Ptr& msg_ref, fdbus::CFdbBaseObject*) {
          auto* msg = fdbus::castToMessage<fdbus::CBaseMessage*>(msg_ref);

          if VUNLIKELY (msg->isStatus() || msg->code() != static_cast<int32_t>(channel)) {
            return;
          }

          callback(Bytes::shallow_copy(msg->getPayloadBuffer(), msg->getPayloadSize()));
        },
        req_data.data(), req_data.size(), nullptr, timeout_ms, FDB_QOS_RELIABLE);
  }

  auto weak_self = weak_from_this();

  return invoke(
      channel,
      [weak_self, owner, channel, callback = std::move(callback)](fdbus::CBaseJob::Ptr& msg_ref,
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

        self->invoke_callback(owner, [&] {
          message_loop = owner->get_message_loop();

          if (!message_loop) {
            callback(Bytes::shallow_copy(msg->getPayloadBuffer(), msg->getPayloadSize()));
          }
        });

        if (message_loop) {
          message_loop->post_task([weak_self, owner, callback = std::move(callback), msg_ref, message_loop]() mutable {
            auto self = weak_self.lock();
            bool attached = false;

            if (self) {
              self->invoke_callback(owner, [&] { attached = owner->get_message_loop() == message_loop; });
            }

            if (attached) {
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

  traverse_server_connect_callback([](NodeImpl*, const auto& callback) { callback(true); });

  traverse_msg_callback([this](NodeImpl* impl, const auto&) {
    const auto* conf_ptr = impl->get_target_conf<FdbusConf>();

    fdbus::CFdbMsgSubscribeList subscribe_list;

    subscribe_list.addNotifyItem(static_cast<int32_t>(conf_ptr->hash_code), conf_ptr->event.data());

    subscribe(subscribe_list);
  });
}

void FdbusClient::onOffline(const fdbus::CFdbOnlineInfo& info) {
  (void)info;

  traverse_server_connect_callback([](NodeImpl*, const auto& callback) { callback(false); });
}

void FdbusClient::onBroadcast(fdbus::CBaseJob::Ptr& msg_ref) {
  auto* msg = fdbus::castToMessage<fdbus::CBaseMessage*>(msg_ref);
  Bytes msg_data = Bytes::shallow_copy(msg->getPayloadBuffer(), msg->getPayloadSize());
  MessageLoop* first_loop = nullptr;
  std::vector<MessageLoop*> other_loops;

  traverse_msg_callback([&](NodeImpl* impl, const auto& callback) {
    const auto* conf_ptr = impl->get_target_conf<FdbusConf>();

    if VUNLIKELY (static_cast<int32_t>(conf_ptr->hash_code) != msg->code() || impl->has_suspend ||
                  conf_ptr->event != msg->topic()) {
      return;
    }

    auto* loop = impl->get_message_loop();

    if (!loop) {
      callback(msg_data);
    } else if (!first_loop) {
      first_loop = loop;
    } else if (loop != first_loop && std::find(other_loops.begin(), other_loops.end(), loop) == other_loops.end()) {
      other_loops.emplace_back(loop);
    }
  });

  auto post = [&](MessageLoop* loop) {
    loop->post_task([weak = weak_from_this(), message_ref = msg_ref, loop]() mutable {
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

  if (first_loop) {
    post(first_loop);
  }

  for (auto* loop : other_loops) {
    post(loop);
  }
}

}  // namespace vlink
