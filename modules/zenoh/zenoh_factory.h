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

#ifdef VLINK_ENABLE_ZENOH_PICO
#include <zenoh-pico.h>
#else
#include <zenoh.h>
#endif

#include <atomic>
#include <cstddef>
#include <deque>
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
#include "./extension/qos.h"
#include "./impl/abstract_factory.h"
#include "./impl/calculate_sample.h"
#include "./modules/zenoh_conf.h"

namespace vlink {

#if !defined(VLINK_ENABLE_ZENOH_PICO) && defined(Z_FEATURE_SHARED_MEMORY) && defined(Z_FEATURE_UNSTABLE_API)
#define VLINK_ZENOH_SHM_AVAILABLE 1
#else
#define VLINK_ZENOH_SHM_AVAILABLE 0
#endif

using SessionID = std::tuple<int32_t, int32_t, std::string, Conf::PropertiesMap>;
using ZenohID = std::tuple<uint8_t, std::string, int32_t, int32_t, std::string, std::string, Conf::PropertiesMap>;
using ZenohSessionPtr = std::shared_ptr<z_owned_session_t>;
static constexpr size_t kZenohDefaultShmLoanThreshold{8 * 1024};

struct alignas(16) ZenohHeader final {
  uint64_t guid{0};
  uint64_t timestamp{0};
  uint64_t channel{0};
  uint64_t seq{0};
};

#if VLINK_ZENOH_SHM_AVAILABLE
class ZenohShmSupport final {
 public:
  ZenohShmSupport();

  ~ZenohShmSupport();

  void configure(bool enabled, bool blocking, size_t loan_threshold);

  [[nodiscard]] bool is_support_loan(const ZenohSessionPtr& session);

  Bytes loan(const ZenohSessionPtr& session, int64_t size);

  bool release(const Bytes& bytes);

  bool build_payload(z_owned_bytes_t* payload, const Bytes& bytes);

 private:
  bool init_locked(const ZenohSessionPtr& session);

  void clear_locked();

  mutable std::mutex mtx_;
  z_owned_shared_shm_provider_t provider_;
  std::vector<std::pair<const uint8_t*, z_owned_shm_mut_t>> loan_map_;
  bool enabled_{false};
  bool blocking_{false};
  size_t loan_threshold_{kZenohDefaultShmLoanThreshold};
  bool ready_{false};
};
#endif

// ZenohFactory
class ZenohFactory final : public AbstractFactory<ZenohID> {
 private:
  ZenohFactory();

  ~ZenohFactory() override;

  void init();

  void deinit();

  void cleanup();

 public:
  static bool write_header(const ZenohHeader& header, z_owned_bytes_t* attachment);

  static bool read_header(ZenohHeader& header, const z_loaned_bytes_t* attachment);

  static int get_default_domain_id();

  static z_priority_t convert_priority(Qos::Additions::Priority priority);

#if defined(Z_FEATURE_UNSTABLE_API)
  static z_reliability_t convert_reliability(Qos::Reliability::Kind reliability);
#endif

  static uint64_t ntp64_to_ns(uint64_t ntp64);

  ZenohSessionPtr get_session(int32_t domain, int32_t depth, const std::string& fragment,
                              const Conf::PropertiesMap& properties);

  const Qos& find_qos(uint8_t impl_type, const std::string& name);

  MessageLoop& get_message_loop();

 private:
  std::atomic_bool has_inited_{false};
  std::atomic_bool has_config_{false};

  MessageLoop message_loop_{MessageLoop::kNormalType};
  z_owned_config_t global_config_;
  std::map<SessionID, ZenohSessionPtr> session_map_;
  std::mutex session_mtx_;
  Qos default_event_qos_;
  Qos default_method_qos_;
  Qos default_field_qos_;

  VLINK_SINGLETON_DECLARE(ZenohFactory)
};

// ZenohServer
class ZenohServer final : public AbstractObject<ZenohID>, public std::enable_shared_from_this<ZenohServer> {
 public:
  explicit ZenohServer(const ZenohID& id);

  ~ZenohServer() override;

  std::any get_native_handle() const override;

  bool suspend();

  bool resume();

  bool is_suspend() const;

  bool is_support_loan() const;

  Bytes loan(uint64_t channel, int64_t size);

  bool release(const Bytes& bytes);

  bool reply(uint64_t channel, uint64_t req_id, const Bytes& resp_data);

  void process_message(uint64_t channel, uint64_t seq, MessageLoop* message_loop, Bytes&& req_bytes);

  static void on_data_callback(z_loaned_query_t* query, void* context);

 private:
  bool build_payload(z_owned_bytes_t* payload, const Bytes& bytes);

  void drop_query(uint64_t req_id);

  std::atomic_bool is_suspend_{false};

  uint64_t guid_{0};
  std::string topic_;
  std::string liveliness_key_;
  ZenohSessionPtr session_;
  z_owned_liveliness_token_t liveliness_token_;
  z_owned_queryable_t server_;
  z_view_keyexpr_t keyexpr_;
  z_view_keyexpr_t liveliness_keyexpr_;
  z_queryable_options_t options_;
  z_congestion_control_t congestion_control_{Z_CONGESTION_CONTROL_BLOCK};
  z_priority_t priority_{Z_PRIORITY_DATA};
  bool is_express_{false};
  std::unordered_map<uint64_t, z_owned_query_t> query_map_;
  std::mutex query_mtx_;
  std::mutex mtx_;
#if VLINK_ZENOH_SHM_AVAILABLE
  mutable ZenohShmSupport shm_;
#endif
};

// ZenohClient
class ZenohClient final : public AbstractObject<ZenohID>, public std::enable_shared_from_this<ZenohClient> {
 public:
  explicit ZenohClient(const ZenohID& id);

  ~ZenohClient() override;

  std::any get_native_handle() const override;

  bool is_connected() const;

  bool is_support_loan() const;

  Bytes loan(uint64_t channel, int64_t size);

  bool release(const Bytes& bytes);

  bool call(NodeImpl* owner, uint64_t channel, const Bytes& req_data, NodeImpl::MsgCallback&& callback = nullptr,
            int timeout_ms = 0);

  void cancel_calls(NodeImpl* owner);

  void check_online();

  static void on_data_callback(z_loaned_reply_t* reply, void* context);

  static void on_reply_drop(void* context);

  static void on_liveliness_change(z_loaned_sample_t* sample, void* context);

 private:
  bool build_payload(z_owned_bytes_t* payload, const Bytes& bytes);

  std::atomic_bool has_connected_{false};
  alignas(64) std::atomic<uint64_t> seq_{0};

  uint64_t guid_{0};
  std::string topic_;
  std::string liveliness_key_;
  z_owned_subscriber_t liveliness_sub_;
  ZenohSessionPtr session_;
  z_view_keyexpr_t keyexpr_;
  z_view_keyexpr_t liveliness_keyexpr_;
  z_query_target_t target_{Z_QUERY_TARGET_DEFAULT};
  z_congestion_control_t congestion_control_{Z_CONGESTION_CONTROL_BLOCK};
  z_priority_t priority_{Z_PRIORITY_DATA};
  bool is_express_{false};
  std::mutex mtx_;

  struct ResponseCallback final {
    NodeImpl* owner{nullptr};
    Function<void(uint64_t, const Bytes&)> callback;
  };

  std::unordered_map<uint64_t, ResponseCallback> callbacks_;
#if VLINK_ZENOH_SHM_AVAILABLE
  mutable ZenohShmSupport shm_;
#endif
};

// ZenohPublisher
class ZenohPublisher final : public AbstractObject<ZenohID>, public std::enable_shared_from_this<ZenohPublisher> {
 public:
  explicit ZenohPublisher(const ZenohID& id);

  ~ZenohPublisher() override;

  std::any get_native_handle() const override;

  void check_matching();

  bool has_subscribers() const;

  bool is_support_loan() const;

  Bytes loan(uint64_t channel, int64_t size);

  bool release(const Bytes& bytes);

  bool publish(uint64_t channel, const Bytes& bytes);

  static void on_matching_status(const z_matching_status_t* status, void* context);

 private:
  bool build_payload(z_owned_bytes_t* payload, const Bytes& bytes);

  std::atomic_bool has_subscribers_{false};
  std::atomic_bool quit_flag_{false};
  alignas(64) std::atomic<uint64_t> seq_{0};

  uint64_t guid_{0};
  std::string topic_;
  ZenohSessionPtr session_;
  z_owned_publisher_t pub_;
  z_view_keyexpr_t keyexpr_;
  z_publisher_options_t options_;
  z_owned_matching_listener_t matching_listener_;
#if VLINK_ZENOH_SHM_AVAILABLE
  mutable ZenohShmSupport shm_;
#endif
};

// ZenohSubscriber
class ZenohSubscriber final : public AbstractObject<ZenohID>, public std::enable_shared_from_this<ZenohSubscriber> {
 public:
  explicit ZenohSubscriber(const ZenohID& id);

  ~ZenohSubscriber() override;

  std::any get_native_handle() const override;

  bool suspend();

  bool resume();

  bool is_suspend() const;

  void subscribe();

  void unsubscribe();

  void set_latency_and_lost_enabled(bool enable);

  bool is_latency_and_lost_enabled() const;

  int64_t get_latency() const;

  const CalculateSample& get_calculate_sample() const;

  void process_message(uint64_t channel, uint64_t seq, uint64_t guid, uint64_t timestamp, MessageLoop* message_loop,
                       Bytes&& bytes);

  static void on_data_callback(z_loaned_sample_t* sample, void* context);

 private:
  std::atomic<int64_t> last_latency_{0};
  std::atomic_bool is_suspend_{false};
  std::atomic_bool has_subscribe_{false};

  uint64_t guid_{0};
  std::string topic_;
  ZenohSessionPtr session_;
  z_owned_subscriber_t sub_;
  z_view_keyexpr_t keyexpr_;
  z_subscriber_options_t options_;
  CalculateSample calc_sample_;
  std::atomic_bool is_latency_and_lost_enabled_{false};
};

}  // namespace vlink
