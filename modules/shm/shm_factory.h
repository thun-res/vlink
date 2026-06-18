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

#if __has_include(<unistd.h>)
#include <unistd.h>
#endif

#ifdef _WIN32
#define _WINSOCKAPI_  // NOLINT(bugprone-reserved-identifier, readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Winsock2.h>
#include <ws2tcpip.h>
#endif

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

// iox
#include <iceoryx_hoofs/cxx/optional.hpp>
#include <iceoryx_hoofs/cxx/scoped_static.hpp>
#include <iceoryx_posh/mepoo/mepoo_config.hpp>
#include <iceoryx_posh/popo/listener.hpp>
#include <iceoryx_posh/popo/untyped_client.hpp>
#include <iceoryx_posh/popo/untyped_publisher.hpp>
#include <iceoryx_posh/popo/untyped_server.hpp>
#include <iceoryx_posh/popo/untyped_subscriber.hpp>
#include <iceoryx_posh/roudi/iceoryx_roudi_components.hpp>
#include <iceoryx_posh/roudi/introspection_types.hpp>
#include <iceoryx_posh/roudi/roudi_config_toml_file_provider.hpp>
#include <iceoryx_posh/runtime/service_discovery.hpp>
#include <iceoryx_versions.hpp>

#include "./base/message_loop.h"
#include "./base/sys_semaphore.h"
#include "./base/utils.h"
#include "./impl/abstract_factory.h"
#include "./impl/calculate_sample.h"
#include "./modules/shm_conf.h"

#define SHM_USE_CUSTOM_SEQ 1

namespace vlink {

namespace shm = iox;

using ShmID = std::tuple<uint8_t, std::string, int32_t, int32_t, int32_t, int32_t>;

[[maybe_unused]] static constexpr int kDefaultReqDepth = 50;
[[maybe_unused]] static constexpr int kDefaultRespDepth = 10;
[[maybe_unused]] static constexpr int kDefaultSubDepth = 5;

// ShmFactory
class ShmFactory final : public AbstractFactory<ShmID> {
 public:
  using DetectCallback = Function<void()>;
  using DiscoveryCallback = Function<void(shm::runtime::ServiceDiscovery*)>;
  using ShmId = std::tuple<shm::capro::IdString_t, shm::capro::IdString_t, shm::capro::IdString_t>;

 private:
  ShmFactory();

  ~ShmFactory() override;

 public:
  static constexpr size_t get_loaned_offset() {
#if SHM_USE_CUSTOM_SEQ
    return sizeof(uint64_t) + sizeof(uint64_t);
#else
    return sizeof(uint64_t);
#endif
  }

  static constexpr size_t get_loaned_alignment() { return sizeof(uint64_t); }

  static void write_header(uint8_t* loaned_ptr, uint64_t channel, uint64_t seq) {
#if SHM_USE_CUSTOM_SEQ
    std::memcpy(loaned_ptr, &channel, sizeof(uint64_t));
    std::memcpy(loaned_ptr + sizeof(uint64_t), &seq, sizeof(uint64_t));
#else
    (void)seq;
    std::memcpy(loaned_ptr, &channel, sizeof(uint64_t));
#endif
  }

  static void write_data(uint8_t* loaned_ptr, uint64_t channel, uint64_t seq, const Bytes& data) {
#if SHM_USE_CUSTOM_SEQ
    std::memcpy(loaned_ptr, &channel, sizeof(uint64_t));
    std::memcpy(loaned_ptr + sizeof(uint64_t), &seq, sizeof(uint64_t));
    std::memcpy(loaned_ptr + sizeof(uint64_t) + sizeof(uint64_t), data.data(), data.size());
#else
    (void)seq;
    std::memcpy(loaned_ptr, &channel, sizeof(uint64_t));
    std::memcpy(loaned_ptr + sizeof(uint64_t), data.data(), data.size());
#endif
  }

  static void read_data(const uint8_t* loaned_ptr, uint64_t payload_size, uint64_t& channel, uint64_t& seq,
                        Bytes& data) {
#if SHM_USE_CUSTOM_SEQ

    if VUNLIKELY (payload_size < sizeof(uint64_t) + sizeof(uint64_t)) {
      return;
    }

    std::memcpy(&channel, loaned_ptr, sizeof(uint64_t));
    std::memcpy(&seq, loaned_ptr + sizeof(uint64_t), sizeof(uint64_t));
    data = Bytes::shallow_copy(loaned_ptr + sizeof(uint64_t) + sizeof(uint64_t),
                               payload_size - sizeof(uint64_t) - sizeof(uint64_t));
#else
    (void)seq;

    if VUNLIKELY (payload_size < sizeof(uint64_t)) {
      return;
    }

    std::memcpy(&channel, loaned_ptr, sizeof(uint64_t));
    data = Bytes::shallow_copy(loaned_ptr + sizeof(uint64_t), payload_size - sizeof(uint64_t));
#endif
  }

  static bool has_roudi_inited();

  static bool has_runtime_inited();

  static bool has_roudi_running();

  static bool auto_init_roudi(bool same_process_from_roudi = false);

  static shm::capro::ServiceDescription get_description(const std::string& service, const std::string& instance,
                                                        const std::string& event);

  static void init_log_level(bool wait_roudi);

  static void init_roudi(const std::string& config_path = "", int memory_strategy = 0, bool monitoring_enable = true);

  static void init_runtime(std::string name = "", bool same_process_from_roudi = false);

  static void deinit_runtime();

  static void deinit_roudi();

  shm::popo::Listener* get_listener(int32_t domain = 0);

  void try_to_destroy_listener(int32_t domain = 0, shm::popo::Listener* listener = nullptr);

  void add_detect_callback(void* node, DetectCallback&& callback);

  void remove_detect_callback(void* node);

  void start_detect_node_count();

  uint64_t get_publisher_count(const shm::capro::ServiceDescription& description);

  uint64_t get_subscriber_count(const shm::capro::ServiceDescription& description);

  int get_sub_depth() const;

 private:
  std::unordered_map<int32_t, std::shared_ptr<shm::popo::Listener>> listener_map_;
  MessageLoop message_loop_{MessageLoop::kNormalType};
  Timer detect_timer_;
  std::unordered_map<void*, DetectCallback> detect_map_;
  std::shared_mutex detect_mtx_;
  std::mutex listener_mtx_;
  std::unordered_map<AbstractNode*, DiscoveryCallback> discovery_map_;
  int sub_depth_{kDefaultSubDepth};

  std::optional<shm::popo::Subscriber<shm::roudi::PortIntrospectionFieldTopic>> port_sub_;
  iox::roudi::PortIntrospectionFieldTopic topic_list_;
  std::shared_mutex topic_mtx_;

  VLINK_SINGLETON_DECLARE(ShmFactory)
};

// ShmServer
class ShmServer final : public AbstractObject<ShmID>, public std::enable_shared_from_this<ShmServer> {
 public:
  explicit ShmServer(const ShmID& id);

  ~ShmServer() override;

  std::any get_native_handle() const override;

  bool suspend();

  bool resume();

  bool is_suspend() const;

  void process_message();

  void start();

  void stop();

  bool has_clients() const;

  Bytes loan(uint64_t channel, int64_t size);

  bool release(const Bytes& bytes);

  bool reply(uint64_t channel, const Bytes& resp_data);

 private:
  static void on_request_received(shm::popo::UntypedServer* server, ShmServer* target);

  alignas(64) std::atomic<uint64_t> seq_{0};
  std::atomic_bool is_suspend_{false};

  int32_t domain_{0};
  shm::popo::Listener* listener_{nullptr};
  std::optional<shm::popo::UntypedServer> server_;
  const iox::popo::RequestHeader* last_req_header_{nullptr};
  std::mutex mtx_;
};

// ShmClient
class ShmClient final : public AbstractObject<ShmID>, public std::enable_shared_from_this<ShmClient> {
 public:
  explicit ShmClient(const ShmID& id);

  ~ShmClient() override;

  std::any get_native_handle() const override;

  void process_message();

  bool is_connected() const;

  void enable_detect_timer();

  void disable_detect_timer();

  Bytes loan(uint64_t channel, int64_t size);

  bool release(const Bytes& bytes);

  bool call(uint64_t channel, const Bytes& req_data, NodeImpl::MsgCallback&& callback = nullptr,
            uint64_t* seq_out = nullptr);

  void remove_response_callback(uint64_t seq);

 private:
  void detect_server();

  void discovery_server(bool connect);

  static void on_response_received(shm::popo::UntypedClient*, ShmClient* target);

  std::atomic_bool has_detect_timer_{false};
  std::atomic_bool last_connected_{false};
  std::atomic_bool quit_flag_{false};
  alignas(64) std::atomic<uint64_t> seq_{0};

  int32_t domain_{0};
  shm::popo::Listener* listener_{nullptr};
  std::optional<shm::popo::UntypedClient> client_;
  std::mutex mtx_;
  std::unordered_map<uint64_t, Function<void(uint64_t, const Bytes&)>> callbacks_;
};

// ShmPublisher
class ShmPublisher final : public AbstractObject<ShmID>, public std::enable_shared_from_this<ShmPublisher> {
 public:
  explicit ShmPublisher(const ShmID& id);

  ~ShmPublisher() override;

  std::any get_native_handle() const override;

  bool has_subscribers() const;

  Bytes loan(uint64_t channel, int64_t size);

  bool release(const Bytes& bytes);

  bool publish(uint64_t channel, const Bytes& bytes);

  void enable_detect_timer();

  void disable_detect_timer();

 private:
  void detect_subscribers();

  void discovery_subscribers(bool has_subscribers);

  std::atomic_bool has_detect_timer_{false};
  std::atomic_bool last_has_subscribers_{false};
  std::atomic_bool quit_flag_{false};
  alignas(64) std::atomic<uint64_t> seq_{0};

  int32_t domain_{0};
  int32_t wait_{0};
  std::optional<shm::popo::UntypedPublisher> pub_;
  std::optional<SysSemaphore> sem_;
};

// ShmSubscriber
class ShmSubscriber final : public AbstractObject<ShmID>, public std::enable_shared_from_this<ShmSubscriber> {
 public:
  explicit ShmSubscriber(const ShmID& id);

  ~ShmSubscriber() override;

  std::any get_native_handle() const override;

  bool suspend();

  bool resume();

  bool is_suspend() const;

  void process_message();

  void subscribe();

  void unsubscribe();

  void set_manual_unloan(bool manual_unloan);

  bool release(const Bytes& bytes);

  void set_latency_and_lost_enabled(bool enable);

  bool is_latency_and_lost_enabled() const;

  const CalculateSample& get_calculate_sample() const;

 private:
  static void on_msg_received(shm::popo::UntypedSubscriber* sub, ShmSubscriber* target);

  std::atomic_bool is_suspend_{false};

  int32_t domain_{0};
  int32_t wait_{0};

  shm::popo::Listener* listener_{nullptr};

  std::optional<shm::popo::UntypedSubscriber> sub_;

  std::optional<SysSemaphore> sem_;

  std::atomic_bool is_latency_and_lost_enabled_{false};

  CalculateSample calc_sample_;

  std::atomic_bool manual_unloan_{false};
};

}  // namespace vlink
