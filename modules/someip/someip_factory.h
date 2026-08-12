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

#include <e2e/e2e_config.h>
#include <e2e/e2e_header.h>
#include <e2e/e2e_protection.h>
#include <sd/sd_client.h>
#include <sd/sd_server.h>
#include <someip/message.h>
#include <transport/tcp_transport.h>
#include <transport/transport.h>
#include <transport/udp_transport.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "./base/condition_variable.h"
#include "./impl/abstract_factory.h"
#include "./modules/someip_conf.h"

namespace vlink {

using SomeipID = std::tuple<uint8_t, uint16_t, uint16_t, Conf::PropertiesMap>;

struct SomeipSettings final {
  bool use_tcp{false};
  std::string local_ip{"127.0.0.1"};
  std::string remote_ip{"127.0.0.1"};
  uint16_t local_port{0};
  uint16_t remote_port{0};
  uint16_t client_id{0};
  uint8_t interface_version{0};
  size_t max_payload_size{1400 - someip::Message::get_header_size()};
  someip::transport::UdpTransportConfig udp;
  someip::transport::TcpTransportConfig tcp;
  bool sd_enabled{false};
  someip::sd::SdConfig sd;
  bool e2e_enabled{false};
  someip::e2e::E2EConfig e2e;
  std::chrono::milliseconds probe_interval{250};
  std::chrono::milliseconds connection_timeout{1000};
};

// SomeipFactory
class SomeipFactory final : public AbstractFactory<SomeipID> {
 private:
  SomeipFactory();

  ~SomeipFactory() override;

 public:
  static bool load_global_config_file(const std::string& filepath);

  Conf::PropertiesMap resolve_properties(const SomeipConf& conf, const Conf::PropertiesMap& node_properties) const;

  SomeipSettings make_settings(uint16_t service, uint16_t instance, bool server,
                               const Conf::PropertiesMap& properties) const;

 private:
  struct ServiceConfig final {
    std::string transport;
    uint16_t unreliable_port{0};
    uint16_t reliable_port{0};
  };

  bool load_config_file(const std::string& filepath);

  mutable std::mutex config_mtx_;
  Conf::PropertiesMap config_properties_;
  std::map<std::pair<uint16_t, uint16_t>, ServiceConfig> service_configs_;

  VLINK_SINGLETON_DECLARE(SomeipFactory)
};

// SomeipServer
class SomeipServer final : public AbstractObject<SomeipID>,
                           public someip::transport::ITransportListener,
                           public std::enable_shared_from_this<SomeipServer> {
 public:
  explicit SomeipServer(const SomeipID& id);

  ~SomeipServer() override;

  std::any get_native_handle() const override;

  void start();

  void offer_event(uint16_t event, const SomeipConf::Groups& groups, bool field);

  void stop_offer_event(uint16_t event, const SomeipConf::Groups& groups, bool field);

  bool publish(uint16_t event, const Bytes& msg_data, bool field);

  [[nodiscard]] bool has_subscribers(const SomeipConf::Groups& groups) const;

 private:
  struct EventInfo final {
    std::map<uint16_t, size_t> group_ref_counts;
    size_t field_references{0};
    someip::platform::ByteBuffer cached_payload;
    bool has_cached_payload{false};
    size_t references{0};
  };

  bool send_message(someip::Message& message, const someip::transport::Endpoint& endpoint);

  void handle_control(const someip::Message& message, const someip::transport::Endpoint& sender);

  void update_subscriber(const someip::transport::Endpoint& endpoint, const SomeipConf::Groups& groups,
                         bool subscribed);

  void notify_subscriber_change();

  void on_message_received(someip::MessagePtr message, const someip::transport::Endpoint& sender) override;

  void on_connection_lost(const someip::transport::Endpoint& endpoint) override;

  void on_connection_established(const someip::transport::Endpoint& endpoint) override;

  void on_error(someip::Result error) override;

  std::atomic_bool has_started_{false};
  std::atomic<uint16_t> next_session_id_{1};
  std::atomic<uint64_t> receive_thread_id_{0};
  std::atomic<uint64_t> connection_thread_id_{0};
  uint16_t service_id_{0};
  uint16_t instance_id_{0};
  SomeipSettings settings_;
  std::unique_ptr<someip::transport::ITransport> transport_;
  std::unique_ptr<someip::sd::SdServer> sd_server_;
  someip::e2e::E2EProtection e2e_;
  std::mutex send_mtx_;
  std::map<uint16_t, EventInfo> events_;
  std::unordered_map<uint16_t, std::unordered_set<someip::transport::Endpoint, someip::transport::Endpoint::Hash>>
      subscribers_;
  mutable std::mutex mtx_;
};

// SomeipClient
class SomeipClient final : public AbstractObject<SomeipID>,
                           public someip::transport::ITransportListener,
                           public std::enable_shared_from_this<SomeipClient> {
 public:
  explicit SomeipClient(const SomeipID& id);

  ~SomeipClient() override;

  std::any get_native_handle() const override;

  void start();

  bool subscribe(const SomeipConf::Groups& groups);

  void unsubscribe(const SomeipConf::Groups& groups);

  bool call(uint16_t method, const Bytes& req_data, NodeImpl::MsgCallback&& callback = nullptr,
            uint64_t* seq_out = nullptr);

  void remove_response_callback(uint64_t seq);

  [[nodiscard]] bool is_connected() const;

  [[nodiscard]] bool is_receive_thread() const;

 private:
  bool send_message(someip::Message& message);

  bool send_control(uint8_t operation, const SomeipConf::Groups& groups);

  void update_remote_endpoint(const someip::sd::ServiceInstance& instance);

  void update_connected(bool connected);

  void monitor_connection();

  void on_message_received(someip::MessagePtr message, const someip::transport::Endpoint& sender) override;

  void on_connection_lost(const someip::transport::Endpoint& endpoint) override;

  void on_connection_established(const someip::transport::Endpoint& endpoint) override;

  void on_error(someip::Result error) override;

  std::atomic_bool has_started_{false};
  std::atomic_bool connected_{false};
  std::atomic_bool monitor_running_{false};
  std::atomic<uint16_t> next_session_id_{1};
  std::atomic<uint64_t> receive_thread_id_{0};
  std::atomic<uint64_t> connection_thread_id_{0};
  uint16_t service_id_{0};
  uint16_t instance_id_{0};
  SomeipSettings settings_;
  someip::transport::Endpoint remote_endpoint_;
  std::unique_ptr<someip::transport::ITransport> transport_;
  std::unique_ptr<someip::sd::SdClient> sd_client_;
  someip::e2e::E2EProtection e2e_;
  std::mutex send_mtx_;
  std::map<uint16_t, size_t> group_ref_counts_;
  std::unordered_map<uint64_t, std::pair<uint16_t, NodeImpl::MsgCallback>> resp_callbacks_;
  std::chrono::steady_clock::time_point last_message_time_{std::chrono::steady_clock::now()};
  std::thread monitor_thread_;
  ConditionVariable monitor_cv_;
  std::mutex monitor_mtx_;
  mutable std::mutex mtx_;
};

}  // namespace vlink
