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

#include <MQTTClient.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "./base/message_loop.h"
#include "./impl/abstract_factory.h"
#include "./impl/calculate_sample.h"
#include "./modules/mqtt_conf.h"

namespace vlink {

using MqttID = std::tuple<uint8_t, std::string, int32_t, int32_t, std::string, Conf::PropertiesMap>;
using MqttSessionID = std::tuple<int32_t, std::string, Conf::PropertiesMap>;
using MqttSubscriptionID = std::tuple<MqttSessionID, std::string>;

struct alignas(16) MqttHeader final {
  uint64_t guid{0};
  uint64_t timestamp{0};
  uint64_t channel{0};
  uint64_t seq{0};
};

static constexpr size_t kMqttHeaderSize = sizeof(MqttHeader);

// MqttFactory
class MqttFactory final : public AbstractFactory<MqttID> {
 private:
  MqttFactory();

  ~MqttFactory() override;

  void init();

  void deinit();

  void cleanup();

 public:
  static void encode_header(const MqttHeader& header, uint8_t* buf);

  static bool decode_header(MqttHeader& header, const uint8_t* data, size_t size);

  static int get_default_domain_id();

  static int get_default_qos();

  MQTTClient& get_client(int32_t domain, const std::string& fragment, const Conf::PropertiesMap& properties);

  bool is_client_connected(int32_t domain, const std::string& fragment, const Conf::PropertiesMap& properties);

  MessageLoop& get_message_loop();

  void subscribe_topic(void* owner, int32_t domain, const std::string& fragment, const Conf::PropertiesMap& properties,
                       const std::string& topic, int qos,
                       Function<void(const std::string&, const uint8_t*, size_t)>&& callback);

  void unsubscribe_topic(void* owner, int32_t domain, const std::string& fragment,
                         const Conf::PropertiesMap& properties, const std::string& topic);

  bool publish_topic(int32_t domain, const std::string& fragment, const Conf::PropertiesMap& properties,
                     const std::string& topic, const uint8_t* payload, size_t payload_size, int qos);

  void register_connection_callback(void* owner, int32_t domain, const std::string& fragment,
                                    const Conf::PropertiesMap& properties, Function<void(bool)>&& callback);

  void unregister_connection_callback(void* owner);

 private:
  struct ClientContext final {
    std::atomic<MqttFactory*> factory{nullptr};
    MqttSessionID session_id;
    int last_failed_rc{0};
  };

  struct Subscription final {
    int qos{1};
    Function<void(const std::string&, const uint8_t*, size_t)> callback;
  };

  bool connect_client_locked(const MqttSessionID& session_id, MQTTClient& client);

  void set_connection_state(const MqttSessionID& session_id, bool connected);

  void notify_connection_change(const MqttSessionID& session_id, bool connected);

  void subscribe_existing_topics(const MqttSessionID& session_id);

  void schedule_reconnect(uint32_t delay_ms = 0);

  static int on_message_arrived(void* context, char* topic_name, int topic_len, MQTTClient_message* message);

  static void on_connection_lost(void* context, char* cause);

  static void on_delivered(void* context, MQTTClient_deliveryToken dt);

  bool reconnect_all();

  std::atomic_bool has_inited_{false};

  MessageLoop message_loop_{MessageLoop::kNormalType};
  std::map<MqttSessionID, MQTTClient> client_map_;
  std::map<MqttSessionID, std::unique_ptr<ClientContext>> client_context_map_;
  std::map<MqttSessionID, bool> client_connected_state_;
  std::mutex client_mtx_;

  std::map<MqttSubscriptionID, std::map<void*, Subscription>> subscription_callbacks_;
  std::mutex sub_mtx_;

  std::unordered_map<void*, std::pair<MqttSessionID, Function<void(bool)>>> connection_callbacks_;
  std::mutex connection_mtx_;
  std::atomic_bool reconnect_scheduled_{false};

  VLINK_SINGLETON_DECLARE(MqttFactory)
};

// MqttPublisher
class MqttPublisher final : public AbstractObject<MqttID>, public std::enable_shared_from_this<MqttPublisher> {
 public:
  explicit MqttPublisher(const MqttID& id);

  ~MqttPublisher() override;

  std::any get_native_handle() const override;

  bool has_subscribers() const;

  void enable_connection_notifications();

  bool publish(uint64_t channel, const Bytes& bytes);

 private:
  alignas(64) std::atomic<uint64_t> seq_{0};

  uint64_t guid_{0};
  std::string topic_;
  int32_t domain_{0};
  int32_t qos_{1};
  std::string fragment_;
  Conf::PropertiesMap properties_;
};

// MqttSubscriber
class MqttSubscriber final : public AbstractObject<MqttID>, public std::enable_shared_from_this<MqttSubscriber> {
 public:
  explicit MqttSubscriber(const MqttID& id);

  ~MqttSubscriber() override;

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

  void process_message(uint64_t channel, uint64_t seq, uint64_t guid, uint64_t timestamp, const Bytes& bytes);

 private:
  std::atomic<int64_t> last_latency_{0};
  std::atomic_bool is_suspend_{false};
  std::atomic_bool has_subscribe_{false};

  uint64_t guid_{0};
  std::string topic_;
  int32_t domain_{0};
  int32_t qos_{1};
  std::string fragment_;
  Conf::PropertiesMap properties_;
  CalculateSample calc_sample_;
  std::atomic_bool is_latency_and_lost_enabled_{false};
};

// MqttServer
class MqttServer final : public AbstractObject<MqttID>, public std::enable_shared_from_this<MqttServer> {
 public:
  explicit MqttServer(const MqttID& id);

  ~MqttServer() override;

  std::any get_native_handle() const override;

  void start_listening();

  bool suspend();

  bool resume();

  bool is_suspend() const;

  bool reply(uint64_t channel, uint64_t req_id, const Bytes& resp_data);

  void process_message(uint64_t channel, uint64_t seq, const Bytes& req_bytes);

  void stop_listening();

 private:
  std::atomic_bool is_suspend_{false};
  std::atomic_bool has_listening_{false};

  uint64_t guid_{0};
  std::string topic_;
  std::string resp_topic_;
  int32_t domain_{0};
  int32_t qos_{1};
  std::string fragment_;
  Conf::PropertiesMap properties_;
  std::mutex mtx_;
};

// MqttClient
class MqttClient final : public AbstractObject<MqttID>, public std::enable_shared_from_this<MqttClient> {
 public:
  explicit MqttClient(const MqttID& id);

  ~MqttClient() override;

  std::any get_native_handle() const override;

  void start_listening();

  void stop_listening();

  void enable_connection_notifications();

  bool is_connected() const;

  bool call(NodeImpl* owner, uint64_t channel, const Bytes& req_data, NodeImpl::MsgCallback&& callback = nullptr,
            int timeout_ms = 0);

  void cancel_calls(NodeImpl* owner);

 private:
  struct ResponseCallback final {
    NodeImpl* owner{nullptr};
    Function<void(uint64_t, const Bytes&)> callback;
  };

  std::atomic_bool has_connected_{false};
  std::atomic_bool has_listening_{false};
  alignas(64) std::atomic<uint64_t> seq_{0};

  uint64_t guid_{0};
  std::string topic_;
  std::string resp_topic_;
  int32_t domain_{0};
  int32_t qos_{1};
  std::string fragment_;
  Conf::PropertiesMap properties_;
  std::mutex mtx_;
  std::unordered_map<uint64_t, ResponseCallback> callbacks_;
};

}  // namespace vlink
