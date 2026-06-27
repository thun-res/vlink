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

#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "./base/elapsed_timer.h"
#include "./base/helpers.h"
#include "./base/logger.h"
#include "./base/multi_loop.h"
#include "./base/utils.h"
#include "./impl/abstract_factory.h"
#include "./modules/qnx_conf.h"

namespace vlink {

using QnxID = std::tuple<uint8_t, std::string>;

// QnxLoop
class QnxLoop final : public MultiLoop {
 public:
  using MultiLoop::MultiLoop;

 private:
  size_t get_max_task_count() const override;
};

// QnxFactory
class QnxFactory final : public AbstractFactory<QnxID> {
 private:
  QnxFactory();

  ~QnxFactory() override;

 public:
  static std::string get_back_address(const std::string& address, int coid);

  static QnxLoop& get_message_loop();

  static int32_t get_pid();

 private:
  std::optional<QnxLoop> message_loop_;

  VLINK_SINGLETON_DECLARE(QnxFactory)
};

// QnxServer
class QnxServer final : public AbstractObject<QnxID>, public std::enable_shared_from_this<QnxServer> {
 public:
  explicit QnxServer(const QnxID& id);

  ~QnxServer() override;

  bool publish(uint32_t channel, const Bytes& msg_data);

  std::any get_native_handle() const override;

  int get_session_count();

 private:
  void process_message();

  std::atomic_bool quit_flag_{false};
  std::atomic_bool is_busy_{false};
  std::atomic<name_attach_t*> fd_{nullptr};

  std::string address_;
  uint32_t resp_token{0};
  std::unordered_map<int, int> clients_;
  std::unordered_map<uint32_t, std::set<int>> channels_;
  std::unordered_map<int, int> scoid_to_id_;
  std::unordered_map<uint32_t, Bytes> resp_cache_map_;
  std::thread thread_;
  std::recursive_mutex mtx_;
};

// QnxClient
class QnxClient final : public AbstractObject<QnxID>, public std::enable_shared_from_this<QnxClient> {
 public:
  explicit QnxClient(const QnxID& id);

  ~QnxClient() override;

  std::any get_native_handle() const override;

  bool call(uint32_t channel, const Bytes& req_data, NodeImpl::MsgCallback&& callback = nullptr);

  bool subscribe(uint32_t channel);

  bool is_connected() const;

  bool listen();

  void start_timer();

 private:
  void try_connect();

  void try_detect();

  void disconnect();

  std::atomic_bool quit_flag_{false};
  std::atomic_bool is_busy_{false};
  std::atomic_bool timer_started_{false};
  std::atomic<name_attach_t*> back_fd_{nullptr};
  std::atomic<int> coid_{-1};
  std::string address_;
  Timer timer_;
  std::thread thread_;
};

}  // namespace vlink
