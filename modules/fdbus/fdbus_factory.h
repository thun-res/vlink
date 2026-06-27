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

#include <fdbus/fdbus.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "./base/message_loop.h"
#include "./impl/abstract_factory.h"
#include "./modules/fdbus_conf.h"

namespace vlink {

namespace fdbus = ipc::fdbus;

using FdbusID = std::tuple<uint8_t, std::string, std::string>;

// FdbusFactory
class FdbusFactory final : public AbstractFactory<FdbusID> {
 private:
  FdbusFactory();

  ~FdbusFactory() override;

 public:
  static void set_worker(fdbus::CBaseEndpoint* end_point);

  static MessageLoop& get_message_loop();

 private:
  std::vector<std::shared_ptr<ipc::fdbus::CBaseWorker>> workers_;
  size_t worker_nums_{0};
  std::mutex mtx_;
  MessageLoop message_loop_;

  VLINK_SINGLETON_DECLARE(FdbusFactory)
};

// FdbusServer
class FdbusServer final : public AbstractObject<FdbusID>,
                          public fdbus::CBaseServer,
                          public std::enable_shared_from_this<FdbusServer> {
 public:
  explicit FdbusServer(const FdbusID& id);

  ~FdbusServer() override;

  std::any get_native_handle() const override;

 private:
  void onSubscribe(fdbus::CBaseJob::Ptr& msg_ref) override;

  void onOffline(const fdbus::CFdbOnlineInfo& info) override;

  void onInvoke(fdbus::CBaseJob::Ptr& msg_ref) override;
};

// FdbusClient
class FdbusClient final : public AbstractObject<FdbusID>,
                          public fdbus::CBaseClient,
                          public std::enable_shared_from_this<FdbusClient> {
 public:
  explicit FdbusClient(const FdbusID& id);

  ~FdbusClient() override;

  std::any get_native_handle() const override;

  void start_timer();

  bool call(uint32_t channel, const Bytes& req_data, NodeImpl::MsgCallback&& callback = nullptr,
            int32_t timeout_ms = 0);

 private:
  void onOnline(const fdbus::CFdbOnlineInfo& info) override;

  void onOffline(const fdbus::CFdbOnlineInfo& info) override;

  void onBroadcast(fdbus::CBaseJob::Ptr& msg_ref) override;

  std::atomic_bool quit_flag_{false};
  std::atomic_bool timer_started_{false};
  std::string url_;
  Timer timer_;
};

}  // namespace vlink
