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

#include <iox2/iceoryx2.h>

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

#include "./base/message_loop.h"
#include "./base/sys_semaphore.h"
#include "./base/utils.h"
#include "./impl/abstract_factory.h"
#include "./impl/calculate_sample.h"
#include "./modules/shm2_conf.h"

#define SHM2_USE_CUSTOM_SEQ 1

namespace vlink {

using ShmID2 = std::tuple<uint8_t, std::string, int32_t, int32_t, int32_t, int32_t, int64_t>;

[[maybe_unused]] static constexpr int kDefaultReqDepth2 = 50;
[[maybe_unused]] static constexpr int kDefaultRespDepth2 = 10;
[[maybe_unused]] static constexpr int kDefaultSubDepth2 = 5;

class Shm2Factory final : public AbstractFactory<ShmID2> {
 public:
  using DetectCallback = Function<void()>;
  using PollCallback = Function<void()>;

  struct PollEntry final {
    PollCallback callback;
    iox2_waitset_guard_h guard{nullptr};
  };

 private:
  Shm2Factory();

  ~Shm2Factory() override;

 public:
  static constexpr size_t get_loaned_offset() {
#if SHM2_USE_CUSTOM_SEQ
    return sizeof(uint64_t) + sizeof(uint64_t);
#else
    return sizeof(uint64_t);
#endif
  }

  static constexpr size_t get_loaned_alignment() { return sizeof(uint64_t); }

  static void write_header(uint8_t* loaned_ptr, uint64_t channel, uint64_t seq) {
#if SHM2_USE_CUSTOM_SEQ
    std::memcpy(loaned_ptr, &channel, sizeof(uint64_t));
    std::memcpy(loaned_ptr + sizeof(uint64_t), &seq, sizeof(uint64_t));
#else
    (void)seq;
    std::memcpy(loaned_ptr, &channel, sizeof(uint64_t));
#endif
  }

  static void write_data(uint8_t* loaned_ptr, uint64_t channel, uint64_t seq, const Bytes& data) {
#if SHM2_USE_CUSTOM_SEQ
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
#if SHM2_USE_CUSTOM_SEQ

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

  static std::string make_service_name(const std::string& address, const std::string& suffix, int32_t domain);

  iox2_node_h_ref get_node() const;

  iox2_waitset_h_ref get_waitset() const { return &waitset_; }

  void add_detect_event_callback(iox2_port_factory_pub_sub_h handle, DetectCallback&& callback);

  void remove_detect_event_callback(iox2_port_factory_pub_sub_h handle);

  void add_detect_method_callback(iox2_port_factory_request_response_h handle, DetectCallback&& callback);

  void remove_detect_method_callback(iox2_port_factory_request_response_h handle);

  int get_default_depth() const;

  void register_poll(void* handle, PollCallback&& callback, iox2_waitset_guard_h guard = nullptr);

  void unregister_poll(void* handle);

  void poll_thread_func();

 private:
  static iox2_callback_progression_e on_process(iox2_waitset_attachment_id_h attachment_id, void* context);

  std::atomic_bool poll_quit_{false};

  iox2_config_t config_storage_{};
  iox2_config_h config_;

  iox2_node_h node_{nullptr};
  iox2_waitset_h waitset_{nullptr};

  iox2_port_factory_event_t wakeup_event_pf_storage_{};
  iox2_port_factory_event_h wakeup_event_pf_handle_{nullptr};
  iox2_notifier_t wakeup_notifier_storage_{};
  iox2_notifier_h wakeup_notifier_{nullptr};
  iox2_listener_t wakeup_listener_storage_{};
  iox2_listener_h wakeup_listener_{nullptr};
  iox2_waitset_guard_t wakeup_guard_storage_{};
  iox2_waitset_guard_h wakeup_guard_{nullptr};

  MessageLoop message_loop_{MessageLoop::kNormalType};
  Timer detect_timer_;
  std::unordered_map<iox2_port_factory_pub_sub_h, DetectCallback> detect_event_map_;
  std::unordered_map<iox2_port_factory_request_response_h, DetectCallback> detect_method_map_;
  std::shared_mutex detect_mtx_;
  int default_depth_{kDefaultSubDepth2};

  std::thread poll_thread_;

  std::unordered_map<void*, PollEntry> poll_map_;
  std::shared_mutex sub_list_mtx_;

  VLINK_SINGLETON_DECLARE(Shm2Factory)
};

class Shm2Server final : public AbstractObject<ShmID2>, public std::enable_shared_from_this<Shm2Server> {
 public:
  explicit Shm2Server(const ShmID2& id);

  ~Shm2Server() override;

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
  static void poll_requests(Shm2Server* target);

  alignas(64) std::atomic<uint64_t> seq_{0};
  std::atomic_bool is_suspend_{false};
  std::atomic_bool is_offering_{false};

  iox2_port_factory_request_response_t pf_storage_{};
  iox2_port_factory_request_response_h pf_handle_{nullptr};
  std::string service_name_;
  int32_t domain_{0};
  int64_t payload_size_{0};

  iox2_server_t server_storage_{};
  iox2_server_h server_{nullptr};

  iox2_active_request_t active_req_storage_{};
  iox2_active_request_h active_req_{nullptr};

  iox2_port_factory_event_t event_pf_storage_{};
  iox2_port_factory_event_h event_pf_handle_{nullptr};
  iox2_notifier_t notifier_storage_{};
  iox2_notifier_h notifier_{nullptr};
  iox2_waitset_guard_t guard_storage_{};
  iox2_waitset_guard_h guard_{nullptr};

  std::mutex mtx_;

  struct ServerLoanEntry {
    std::unique_ptr<iox2_response_mut_t> storage;
    iox2_response_mut_h handle{nullptr};
  };

  std::mutex loan_mtx_;
  std::unordered_map<const uint8_t*, ServerLoanEntry> loan_map_;
};

class Shm2Client final : public AbstractObject<ShmID2>, public std::enable_shared_from_this<Shm2Client> {
 public:
  explicit Shm2Client(const ShmID2& id);

  ~Shm2Client() override;

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

  std::atomic_bool has_detect_timer_{false};
  std::atomic_bool last_connected_{false};
  std::atomic_bool quit_flag_{false};
  alignas(64) std::atomic<uint64_t> seq_{0};

  iox2_port_factory_request_response_t pf_storage_{};
  iox2_port_factory_request_response_h pf_handle_{nullptr};
  std::string service_name_;
  int32_t domain_{0};
  int64_t payload_size_{0};

  iox2_client_t client_storage_{};
  iox2_client_h client_{nullptr};

  iox2_port_factory_event_t event_pf_storage_{};
  iox2_port_factory_event_h event_pf_handle_{nullptr};
  iox2_listener_t listener_storage_{};
  iox2_listener_h listener_{nullptr};
  iox2_waitset_guard_t guard_storage_{};
  iox2_waitset_guard_h guard_{nullptr};

  std::mutex mtx_;
  std::unordered_map<uint64_t, Function<void(uint64_t, const Bytes&)>> callbacks_;

  std::unordered_map<uint64_t, iox2_pending_response_t> pending_storage_map_;
  std::unordered_map<uint64_t, iox2_pending_response_h> pending_map_;

  Function<void()> req_notifier_fn_;

  struct ClientLoanEntry {
    std::unique_ptr<iox2_request_mut_t> storage;
    iox2_request_mut_h handle{nullptr};
  };

  std::mutex loan_mtx_;
  std::unordered_map<const uint8_t*, ClientLoanEntry> loan_map_;
};

class Shm2Publisher final : public AbstractObject<ShmID2>, public std::enable_shared_from_this<Shm2Publisher> {
 public:
  explicit Shm2Publisher(const ShmID2& id);

  ~Shm2Publisher() override;

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

  alignas(64) std::atomic<uint64_t> seq_{0};
  std::atomic_bool has_detect_timer_{false};
  std::atomic_bool last_has_subscribers_{false};
  std::atomic_bool quit_flag_{false};

  iox2_port_factory_pub_sub_t pf_storage_{};
  iox2_port_factory_pub_sub_h pf_handle_{nullptr};
  std::string service_name_;
  int32_t domain_{0};
  int32_t wait_{0};
  int64_t payload_size_{0};
  int32_t history_{0};
  int32_t depth_{0};

  iox2_publisher_t publisher_storage_{};
  iox2_publisher_h publisher_{nullptr};

  iox2_port_factory_event_t event_pf_storage_{};
  iox2_port_factory_event_h event_pf_handle_{nullptr};
  iox2_notifier_t notifier_storage_{};
  iox2_notifier_h notifier_{nullptr};

  uint32_t notify_every_{1};
  std::atomic<uint32_t> notify_counter_{0};

  std::optional<SysSemaphore> sem_;

  struct PublisherLoanEntry {
    std::unique_ptr<iox2_sample_mut_t> storage;
    iox2_sample_mut_h handle{nullptr};
  };

  std::mutex loan_mtx_;
  std::unordered_map<const uint8_t*, PublisherLoanEntry> loan_map_;
};

class Shm2Subscriber final : public AbstractObject<ShmID2>, public std::enable_shared_from_this<Shm2Subscriber> {
 public:
  explicit Shm2Subscriber(const ShmID2& id);

  ~Shm2Subscriber() override;

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
  std::atomic_bool is_suspend_{false};
  std::atomic_bool is_subscribed_{false};

  iox2_port_factory_pub_sub_t pf_storage_{};
  iox2_port_factory_pub_sub_h pf_handle_{nullptr};
  iox2_port_factory_subscriber_builder_h sub_builder_{nullptr};
  std::string service_name_;
  int32_t domain_{0};
  int32_t wait_{0};
  int64_t payload_size_{0};
  int32_t history_{0};
  int32_t depth_{0};

  iox2_subscriber_t subscriber_storage_{};
  iox2_subscriber_h subscriber_{nullptr};
  iox2_waitset_guard_t guard_storage_{};
  iox2_waitset_guard_h guard_{nullptr};

  iox2_port_factory_event_t event_pf_storage_{};
  iox2_port_factory_event_h event_pf_handle_{nullptr};
  iox2_listener_t listener_storage_{};
  iox2_listener_h listener_{nullptr};

  std::optional<SysSemaphore> sem_;

  std::atomic_bool is_latency_and_lost_enabled_{false};
  CalculateSample calc_sample_;
  std::atomic_bool manual_unloan_{false};

  struct SubscriberLoanEntry {
    std::unique_ptr<iox2_sample_t> storage;
    iox2_sample_h handle{nullptr};
  };

  std::mutex loan_mtx_;
  std::unordered_map<const uint8_t*, SubscriberLoanEntry> loan_map_;
};

}  // namespace vlink
