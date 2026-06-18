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

#include <charconv>
#include <memory>
#include <string>
#include <utility>

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

  traverse_req_resp_callback([this, &msg, &msg_ref, &req_data](NodeImpl* impl, const auto& callback) {
    const auto* conf_ptr = impl->get_target_conf<FdbusConf>();

    if VUNLIKELY (static_cast<int32_t>(conf_ptr->hash_code) != msg->code() || impl->has_suspend) {
      ignore_called();
      return;
    }

    if VUNLIKELY (has_called()) {
      VLOG_F(*conf_ptr, "Two identical service requests.");
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
  quit_flag_ = true;

  prepareDestroy();

  timer_.stop();
  timer_.detach();
}

bool FdbusClient::call(uint32_t channel, const Bytes& req_data, NodeImpl::MsgCallback&& callback, int32_t timeout_ms) {
  if VUNLIKELY (!callback) {
    return send(channel, req_data.data(), req_data.size(), FDB_QOS_RELIABLE);
  }

  return invoke(
      channel,
      [channel, callback = std::move(callback)](fdbus::CBaseJob::Ptr& msg_ref, fdbus::CFdbBaseObject*) {
        auto* msg = fdbus::castToMessage<fdbus::CBaseMessage*>(msg_ref);

        if VUNLIKELY (msg->code() != static_cast<int32_t>(channel)) {
          return;
        }

        Bytes resp_data = Bytes::shallow_copy(msg->getPayloadBuffer(), msg->getPayloadSize());

        callback(resp_data);
      },
      req_data.data(), req_data.size(), nullptr, timeout_ms, FDB_QOS_RELIABLE);
}

std::any FdbusClient::get_native_handle() const { return this; }

void FdbusClient::start_timer() {
  if (!timer_.get_message_loop()) {
    return;
  }

  bool expected = false;

  if (!timer_started_.compare_exchange_strong(expected, true)) {
    return;
  }

  std::weak_ptr<FdbusClient> weak_self = weak_from_this();

  timer_.start([weak_self]() {
    auto self = weak_self.lock();

    if VUNLIKELY (!self) {
      return;
    }

    if VUNLIKELY (self->quit_flag_) {
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

  traverse_msg_callback([msg, &msg_data](NodeImpl* impl, const auto& callback) {
    const auto* conf_ptr = impl->get_target_conf<FdbusConf>();

    if VUNLIKELY (static_cast<int32_t>(conf_ptr->hash_code) != msg->code() || impl->has_suspend ||
                  conf_ptr->event != msg->topic()) {
      return;
    }

    callback(msg_data);
  });
}

}  // namespace vlink
