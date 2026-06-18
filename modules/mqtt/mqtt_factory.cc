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

#include "./mqtt_factory.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include "./base/elapsed_timer.h"
#include "./base/helpers.h"
#include "./base/timer.h"
#include "./base/utils.h"
#include "./impl/server_impl.h"
#include "./impl/ssl_options.h"

namespace vlink {

static void encode_u64be(uint8_t* buf, uint64_t val) {
  buf[0] = static_cast<uint8_t>(val >> 56);
  buf[1] = static_cast<uint8_t>(val >> 48);
  buf[2] = static_cast<uint8_t>(val >> 40);
  buf[3] = static_cast<uint8_t>(val >> 32);
  buf[4] = static_cast<uint8_t>(val >> 24);
  buf[5] = static_cast<uint8_t>(val >> 16);
  buf[6] = static_cast<uint8_t>(val >> 8);
  buf[7] = static_cast<uint8_t>(val >> 0);
}

static uint64_t decode_u64be(const uint8_t* buf) {
  return (static_cast<uint64_t>(buf[0]) << 56) | (static_cast<uint64_t>(buf[1]) << 48) |
         (static_cast<uint64_t>(buf[2]) << 40) | (static_cast<uint64_t>(buf[3]) << 32) |
         (static_cast<uint64_t>(buf[4]) << 24) | (static_cast<uint64_t>(buf[5]) << 16) |
         (static_cast<uint64_t>(buf[6]) << 8) | (static_cast<uint64_t>(buf[7]) << 0);
}

// MqttFactory
MqttFactory::MqttFactory() {
  Bytes::init_memory_pool();

  if VUNLIKELY (MqttConf::get_thread_count() != 1) {
    VLOG_W("MqttConf: Mqtt does not support setting thread count.");
  }

  init();
}

MqttFactory::~MqttFactory() { deinit(); }

void MqttFactory::init() {
  bool expected = false;

  if VUNLIKELY (!has_inited_.compare_exchange_strong(expected, true)) {
    return;
  }

  message_loop_.async_run();
}

void MqttFactory::deinit() {
  bool expected = true;

  if VUNLIKELY (!has_inited_.compare_exchange_strong(expected, false)) {
    return;
  }

  const bool in_loop = message_loop_.is_in_same_thread();

  if (in_loop) {
    cleanup();
  } else if VLIKELY (message_loop_.post_task([this]() { cleanup(); })) {
    message_loop_.wait_for_idle();
  } else {
    cleanup();
  }

  message_loop_.quit();

  if VLIKELY (!in_loop) {
    message_loop_.wait_for_quit();
  }
}

void MqttFactory::cleanup() {
  {
    std::lock_guard lock(sub_mtx_);
    subscription_callbacks_.clear();
  }

  {
    std::lock_guard lock(connection_mtx_);
    connection_callbacks_.clear();
  }

  std::lock_guard lock(client_mtx_);

  for (auto& [session_id, client] : client_map_) {
    auto ctx_iter = client_context_map_.find(session_id);

    if (ctx_iter != client_context_map_.end() && ctx_iter->second) {
      ctx_iter->second->factory.store(nullptr, std::memory_order_release);
    }

    if (MQTTClient_isConnected(client)) {
      MQTTClient_disconnect(client, 1000);
    }

    MQTTClient_destroy(&client);
  }

  client_map_.clear();
  client_context_map_.clear();
  client_connected_state_.clear();
}

void MqttFactory::encode_header(const MqttHeader& header, uint8_t* buf) {
  encode_u64be(buf + 0, header.guid);
  encode_u64be(buf + 8, header.timestamp);
  encode_u64be(buf + 16, header.channel);
  encode_u64be(buf + 24, header.seq);
}

bool MqttFactory::decode_header(MqttHeader& header, const uint8_t* data, size_t size) {
  if VUNLIKELY (size < kMqttHeaderSize) {
    return false;
  }

  header.guid = decode_u64be(data + 0);
  header.timestamp = decode_u64be(data + 8);
  header.channel = decode_u64be(data + 16);
  header.seq = decode_u64be(data + 24);

  return true;
}

int MqttFactory::get_default_domain_id() {
  const std::string& domain_str = Utils::get_env("VLINK_MQTT_DOMAIN");
  return Helpers::to_int(domain_str, 0);
}

int MqttFactory::get_default_qos() {
  const std::string& qos_str = Utils::get_env("VLINK_MQTT_QOS");
  return Helpers::to_int(qos_str, 1);
}

bool MqttFactory::connect_client_locked(const MqttSessionID& session_id, MQTTClient& client) {
  const auto& [domain, fragment, properties] = session_id;

  if (client && MQTTClient_isConnected(client)) {
    return true;
  }

  static std::string env_broker = Utils::get_env("VLINK_MQTT_BROKER", "tcp://localhost:1883");
  static std::string env_client_prefix = Utils::get_env("VLINK_MQTT_CLIENT_ID", "vlink_mqtt");
  static int env_keepalive = Helpers::to_int(Utils::get_env("VLINK_MQTT_KEEPALIVE", "60"), 60);

  std::string broker = env_broker;

  if (!fragment.empty()) {
    broker = fragment;
  }

  for (const auto& [prop, value] : properties) {
    if (prop == "mqtt.broker") {
      broker = value;
    }
  }

  auto ssl_cfg = SslOptions::parse_from(properties);

  bool ssl_cfg_valid = ssl_cfg.is_valid();

  if (ssl_cfg_valid) {
    if (Helpers::has_startwith(broker, "tcp://")) {
      broker = "ssl://" + broker.substr(6);
    }
  }

  MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
  conn_opts.keepAliveInterval = env_keepalive;
  conn_opts.cleansession = 1;

  MQTTClient_SSLOptions ssl_opts = MQTTClient_SSLOptions_initializer;

  if (ssl_cfg_valid) {
    if (!ssl_cfg.ca_file.empty()) {
      ssl_opts.trustStore = ssl_cfg.ca_file.c_str();
    }

    if (!ssl_cfg.cert_file.empty()) {
      ssl_opts.keyStore = ssl_cfg.cert_file.c_str();
    }

    if (!ssl_cfg.key_file.empty()) {
      ssl_opts.privateKey = ssl_cfg.key_file.c_str();
    }

    if (!ssl_cfg.key_password.empty()) {
      ssl_opts.privateKeyPassword = ssl_cfg.key_password.c_str();
    }

    if (!ssl_cfg.ciphers.empty()) {
      ssl_opts.enabledCipherSuites = ssl_cfg.ciphers.c_str();
    }

    ssl_opts.enableServerCertAuth = ssl_cfg.verify_peer ? 1 : 0;

    conn_opts.ssl = &ssl_opts;
  }

  if (!client) {
    auto [ctx_iter, inserted] = client_context_map_.try_emplace(session_id);

    if (inserted || !ctx_iter->second) {
      ctx_iter->second = std::make_unique<ClientContext>();
    }

    ctx_iter->second->factory.store(this, std::memory_order_release);
    ctx_iter->second->session_id = session_id;

    std::string client_id =
        env_client_prefix + "_" + std::to_string(domain) + "_" + std::to_string(reinterpret_cast<uintptr_t>(&client));

    int rc = MQTTClient_create(&client, broker.c_str(), client_id.c_str(), MQTTCLIENT_PERSISTENCE_NONE, nullptr);

    if VUNLIKELY (rc != MQTTCLIENT_SUCCESS) {
      VLOG_E("MqttFactory: Failed to invoke [MQTTClient_create], rc=", rc, ".");
      return false;
    }

    rc = MQTTClient_setCallbacks(client, ctx_iter->second.get(), on_connection_lost, on_message_arrived, on_delivered);

    if VUNLIKELY (rc != MQTTCLIENT_SUCCESS) {
      VLOG_E("MqttFactory: Failed to invoke [MQTTClient_setCallbacks], rc=", rc, ".");
      MQTTClient_destroy(&client);
      return false;
    }
  }

  ClientContext* ctx = nullptr;

  if (auto ctx_it = client_context_map_.find(session_id); ctx_it != client_context_map_.end()) {
    ctx = ctx_it->second.get();
  }

  int rc = MQTTClient_connect(client, &conn_opts);

  if VUNLIKELY (rc != MQTTCLIENT_SUCCESS) {
    if (!ctx || ctx->last_failed_rc != rc) {
      VLOG_E("MqttFactory: Failed to invoke [MQTTClient_connect], broker=", broker, " rc=", rc, ".");

      if (ctx) {
        ctx->last_failed_rc = rc;
      }
    }

    return false;
  }

  if (ctx) {
    ctx->last_failed_rc = 0;
  }

  return true;
}

void MqttFactory::set_connection_state(const MqttSessionID& session_id, bool connected) {
  bool changed = false;

  {
    std::lock_guard lock(client_mtx_);
    bool& state = client_connected_state_[session_id];

    if (state != connected) {
      state = connected;
      changed = true;
    }
  }

  if (changed) {
    notify_connection_change(session_id, connected);
  }
}

void MqttFactory::notify_connection_change(const MqttSessionID& session_id, bool connected) {
  std::vector<Function<void(bool)>> callbacks;

  {
    std::lock_guard lock(connection_mtx_);

    for (const auto& callback_entry : connection_callbacks_) {
      if (callback_entry.second.first == session_id && callback_entry.second.second) {
        callbacks.emplace_back(callback_entry.second.second);
      }
    }
  }

  for (const auto& callback : callbacks) {
    callback(connected);
  }
}

void MqttFactory::subscribe_existing_topics(const MqttSessionID& session_id) {
  std::vector<std::pair<std::string, int>> subscriptions;

  {
    std::lock_guard lock(sub_mtx_);

    for (const auto& [subscription_id, owner_map] : subscription_callbacks_) {
      const auto& [target_session_id, topic] = subscription_id;

      if (target_session_id != session_id || owner_map.empty()) {
        continue;
      }

      int qos = 0;

      for (const auto& owner_entry : owner_map) {
        qos = std::max(qos, owner_entry.second.qos);
      }

      subscriptions.emplace_back(topic, qos);
    }
  }

  if (subscriptions.empty()) {
    return;
  }

  std::lock_guard lock(client_mtx_);

  auto iter = client_map_.find(session_id);

  if (iter == client_map_.end() || MQTTClient_isConnected(iter->second) == 0) {
    return;
  }

  for (const auto& [topic, qos] : subscriptions) {
    int rc = MQTTClient_subscribe(iter->second, topic.c_str(), qos);

    if VUNLIKELY (rc != MQTTCLIENT_SUCCESS) {
      VLOG_E("MqttFactory: Failed to subscribe to topic '", topic, "', rc=", rc, ".");
    }
  }
}

void MqttFactory::schedule_reconnect(uint32_t delay_ms) {
  if (!has_inited_.load(std::memory_order_acquire)) {
    return;
  }

  bool expected = false;

  if (!reconnect_scheduled_.compare_exchange_strong(expected, true)) {
    return;
  }

  auto reconnect_task = [this]() {
    reconnect_scheduled_.store(false, std::memory_order_release);

    if (reconnect_all()) {
      schedule_reconnect(1000);
    }
  };

  bool posted = false;

  if (delay_ms == 0) {
    posted = message_loop_.post_task(std::move(reconnect_task));
  } else {
    posted = Timer::call_once(&message_loop_, delay_ms, std::move(reconnect_task));
  }

  if VUNLIKELY (!posted) {
    reconnect_scheduled_.store(false, std::memory_order_release);
  }
}

MQTTClient& MqttFactory::get_client(int32_t domain, const std::string& fragment,
                                    const Conf::PropertiesMap& properties) {
  MqttSessionID session_id = MqttSessionID{domain, fragment, properties};

  MQTTClient* client_ptr = nullptr;
  bool was_connected = false;
  bool is_connected = false;

  {
    std::lock_guard lock(client_mtx_);

    auto [iter, inserted] = client_map_.emplace(session_id, MQTTClient{nullptr});
    client_connected_state_.try_emplace(session_id, false);
    client_ptr = &iter->second;
    was_connected = (iter->second != nullptr) && (MQTTClient_isConnected(iter->second) == 1);
    is_connected = connect_client_locked(session_id, iter->second);
  }

  if (is_connected) {
    set_connection_state(session_id, true);

    if (!was_connected) {
      subscribe_existing_topics(session_id);
    }
  } else {
    set_connection_state(session_id, false);
    schedule_reconnect(1000);
  }

  return *client_ptr;
}

bool MqttFactory::is_client_connected(int32_t domain, const std::string& fragment,
                                      const Conf::PropertiesMap& properties) {
  MqttSessionID session_id = MqttSessionID{domain, fragment, properties};

  std::lock_guard lock(client_mtx_);

  auto iter = client_map_.find(session_id);

  if (iter == client_map_.end()) {
    return false;
  }

  return MQTTClient_isConnected(iter->second) != 0;
}

MessageLoop& MqttFactory::get_message_loop() { return message_loop_; }

void MqttFactory::subscribe_topic(void* owner, int32_t domain, const std::string& fragment,
                                  const Conf::PropertiesMap& properties, const std::string& topic, int qos,
                                  Function<void(const std::string&, const uint8_t*, size_t)>&& callback) {
  MqttSessionID session_id = MqttSessionID{domain, fragment, properties};
  MqttSubscriptionID subscription_id = MqttSubscriptionID{session_id, topic};
  int target_qos = qos;
  bool should_subscribe = false;

  MQTTClient& client = get_client(domain, fragment, properties);

  {
    std::lock_guard lock(sub_mtx_);

    auto& owner_map = subscription_callbacks_[subscription_id];
    int old_target_qos = 0;

    for (const auto& owner_entry : owner_map) {
      old_target_qos = std::max(old_target_qos, owner_entry.second.qos);
    }

    bool had_subscription = !owner_map.empty();
    owner_map[owner] = Subscription{qos, std::move(callback)};

    for (const auto& owner_entry : owner_map) {
      target_qos = std::max(target_qos, owner_entry.second.qos);
    }

    should_subscribe = !had_subscription || target_qos != old_target_qos;
  }

  if (!should_subscribe) {
    return;
  }

  std::lock_guard lock(client_mtx_);

  if (MQTTClient_isConnected(client)) {
    int rc = MQTTClient_subscribe(client, topic.c_str(), target_qos);

    if VUNLIKELY (rc != MQTTCLIENT_SUCCESS) {
      VLOG_E("MqttFactory: Failed to subscribe to topic '", topic, "', rc=", rc, ".");
    }
  }
}

void MqttFactory::unsubscribe_topic(void* owner, int32_t domain, const std::string& fragment,
                                    const Conf::PropertiesMap& properties, const std::string& topic) {
  MqttSessionID session_id = MqttSessionID{domain, fragment, properties};
  MqttSubscriptionID subscription_id = MqttSubscriptionID{session_id, topic};
  bool should_unsubscribe = false;
  bool should_resubscribe = false;
  int old_target_qos = 0;
  int target_qos = 0;

  {
    std::lock_guard lock(sub_mtx_);

    auto iter = subscription_callbacks_.find(subscription_id);

    if (iter == subscription_callbacks_.end()) {
      return;
    }

    for (const auto& owner_entry : iter->second) {
      old_target_qos = std::max(old_target_qos, owner_entry.second.qos);
    }

    if (iter->second.erase(owner) == 0) {
      return;
    }

    if (iter->second.empty()) {
      subscription_callbacks_.erase(iter);
      should_unsubscribe = true;
    } else {
      for (const auto& owner_entry : iter->second) {
        target_qos = std::max(target_qos, owner_entry.second.qos);
      }

      should_resubscribe = target_qos != old_target_qos;
    }
  }

  std::lock_guard lock(client_mtx_);

  auto iter = client_map_.find(session_id);

  if (iter == client_map_.end() || (MQTTClient_isConnected(iter->second) == 0)) {
    return;
  }

  if (should_unsubscribe) {
    MQTTClient_unsubscribe(iter->second, topic.c_str());
  } else if (should_resubscribe) {
    int rc = MQTTClient_subscribe(iter->second, topic.c_str(), target_qos);

    if VUNLIKELY (rc != MQTTCLIENT_SUCCESS) {
      VLOG_E("MqttFactory: Failed to resubscribe to topic '", topic, "', rc=", rc, ".");
    }
  }
}

bool MqttFactory::publish_topic(int32_t domain, const std::string& fragment, const Conf::PropertiesMap& properties,
                                const std::string& topic, const uint8_t* payload, size_t payload_size, int qos) {
  if VUNLIKELY (payload_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
    VLOG_E("MqttFactory: Payload is too large for MQTTClient_publish, size=", payload_size, ".");
    return false;
  }

  MQTTClient& client = get_client(domain, fragment, properties);

  std::lock_guard lock(client_mtx_);

  if VUNLIKELY (!MQTTClient_isConnected(client)) {
    return false;
  }

  int rc = MQTTClient_publish(client, topic.c_str(), static_cast<int>(payload_size), const_cast<uint8_t*>(payload), qos,
                              0, nullptr);

  return rc == MQTTCLIENT_SUCCESS;
}

void MqttFactory::register_connection_callback(void* owner, int32_t domain, const std::string& fragment,
                                               const Conf::PropertiesMap& properties, Function<void(bool)>&& callback) {
  std::lock_guard lock(connection_mtx_);
  connection_callbacks_[owner] = {MqttSessionID{domain, fragment, properties}, std::move(callback)};
}

void MqttFactory::unregister_connection_callback(void* owner) {
  std::lock_guard lock(connection_mtx_);
  connection_callbacks_.erase(owner);
}

int MqttFactory::on_message_arrived(void* context, char* topic_name, int topic_len, MQTTClient_message* message) {
  auto* client_context = static_cast<ClientContext*>(context);

  auto* factory = client_context ? client_context->factory.load(std::memory_order_acquire) : nullptr;

  if VUNLIKELY (!factory) {
    MQTTClient_freeMessage(&message);
    MQTTClient_free(topic_name);
    return 1;
  }

  MqttSessionID session_id = client_context->session_id;

  std::string topic =
      (topic_len > 0) ? std::string(topic_name, static_cast<size_t>(topic_len)) : std::string(topic_name);

  const auto* payload = static_cast<const uint8_t*>(message->payload);
  auto payload_len = static_cast<size_t>(message->payloadlen);

  Bytes data_copy = Bytes::deep_copy(payload, payload_len);

  MQTTClient_freeMessage(&message);
  MQTTClient_free(topic_name);

  factory->message_loop_.post_task(
      [factory, session_id = std::move(session_id), topic = std::move(topic), data = std::move(data_copy)]() {
        std::vector<Function<void(const std::string&, const uint8_t*, size_t)>> callbacks;

        {
          std::lock_guard lock(factory->sub_mtx_);

          auto iter = factory->subscription_callbacks_.find(MqttSubscriptionID{session_id, topic});

          if (iter != factory->subscription_callbacks_.end()) {
            for (const auto& owner_entry : iter->second) {
              if (owner_entry.second.callback) {
                callbacks.emplace_back(owner_entry.second.callback);
              }
            }
          }
        }

        for (const auto& callback : callbacks) {
          callback(topic, data.data(), data.size());
        }
      });

  return 1;
}

// NOLINTNEXTLINE(readability-non-const-parameter)
void MqttFactory::on_connection_lost(void* context, char* cause) {
  VLOG_W("MqttFactory: Connection lost, cause: ", (cause ? cause : "unknown"), ".");

  auto* client_context = static_cast<ClientContext*>(context);

  auto* factory = client_context ? client_context->factory.load(std::memory_order_acquire) : nullptr;

  if VUNLIKELY (!factory) {
    return;
  }

  factory->set_connection_state(client_context->session_id, false);
  factory->schedule_reconnect();
}

void MqttFactory::on_delivered(void* context, MQTTClient_deliveryToken dt) {
  (void)context;
  (void)dt;
}

bool MqttFactory::reconnect_all() {
  std::map<MqttSessionID, std::vector<std::pair<std::string, int>>> subscriptions;

  {
    std::lock_guard lock(sub_mtx_);

    for (const auto& [subscription_id, owner_map] : subscription_callbacks_) {
      const auto& [session_id, topic] = subscription_id;

      if (owner_map.empty()) {
        continue;
      }

      int qos = 0;

      for (const auto& owner_entry : owner_map) {
        qos = std::max(qos, owner_entry.second.qos);
      }

      subscriptions[session_id].emplace_back(topic, qos);
    }
  }

  std::vector<std::pair<MqttSessionID, bool>> state_changes;
  bool has_disconnected = false;

  {
    std::lock_guard lock(client_mtx_);

    for (auto& [session_id, client] : client_map_) {
      bool was_connected = (client != nullptr) && (MQTTClient_isConnected(client) == 1);
      bool connected = connect_client_locked(session_id, client);
      bool& state = client_connected_state_[session_id];

      if (state != connected) {
        state = connected;
        state_changes.emplace_back(session_id, connected);
      }

      if (!connected) {
        has_disconnected = true;
        continue;
      }

      if (was_connected) {
        continue;
      }

      auto sub_iter = subscriptions.find(session_id);

      if (sub_iter == subscriptions.end()) {
        continue;
      }

      for (const auto& [topic, qos] : sub_iter->second) {
        int rc = MQTTClient_subscribe(client, topic.c_str(), qos);

        if VUNLIKELY (rc != MQTTCLIENT_SUCCESS) {
          VLOG_E("MqttFactory: Failed to subscribe to topic '", topic, "', rc=", rc, ".");
        }
      }
    }
  }

  for (const auto& [session_id, connected] : state_changes) {
    notify_connection_change(session_id, connected);
  }

  return has_disconnected;
}

// MqttPublisher
MqttPublisher::MqttPublisher(const MqttID& id) {
  const auto& [impl_type, address, domain, qos, fragment, properties] = id;

  topic_ = address;

  if (domain != 0) {
    topic_ += ("_" + std::to_string(domain));
  }

  guid_ = std::hash<std::string>{}(topic_) ^ (reinterpret_cast<uint64_t>(this) << 32);

  domain_ = domain;
  qos_ = qos;
  fragment_ = fragment;
  properties_ = properties;

  MqttFactory::get().get_client(domain_, fragment_, properties_);
}

MqttPublisher::~MqttPublisher() { MqttFactory::get().unregister_connection_callback(this); }

std::any MqttPublisher::get_native_handle() const { return this; }

bool MqttPublisher::has_subscribers() const {
  return MqttFactory::get().is_client_connected(domain_, fragment_, properties_);
}

void MqttPublisher::enable_connection_notifications() {
  auto weak_self = weak_from_this();

  MqttFactory::get().register_connection_callback(this, domain_, fragment_, properties_, [weak_self](bool connected) {
    auto self = weak_self.lock();

    if VUNLIKELY (!self) {
      return;
    }

    self->traverse_sub_connect_callback([connected](NodeImpl*, const auto& callback) { callback(connected); });
  });
}

bool MqttPublisher::publish(uint64_t channel, const Bytes& bytes) {
  auto& factory = MqttFactory::get();

  if VUNLIKELY (bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max()) - kMqttHeaderSize) {
    VLOG_E("MqttFactory: Publisher payload is too large, size=", bytes.size(), ".");
    return false;
  }

  MqttHeader header{guid_, ElapsedTimer::get_sys_timestamp(ElapsedTimer::kNano, false), channel, ++seq_};

  std::vector<uint8_t> payload(kMqttHeaderSize + bytes.size());

  MqttFactory::encode_header(header, payload.data());

  if (!bytes.empty()) {
    std::memcpy(payload.data() + kMqttHeaderSize, bytes.data(), bytes.size());
  }

  return factory.publish_topic(domain_, fragment_, properties_, topic_, payload.data(), payload.size(), qos_);
}

// MqttSubscriber
MqttSubscriber::MqttSubscriber(const MqttID& id) {
  const auto& [impl_type, address, domain, qos, fragment, properties] = id;

  topic_ = address;

  if (domain != 0) {
    topic_ += ("_" + std::to_string(domain));
  }

  guid_ = std::hash<std::string>{}(topic_) ^ (reinterpret_cast<uint64_t>(this) << 32);

  domain_ = domain;
  qos_ = qos;
  fragment_ = fragment;
  properties_ = properties;

  MqttFactory::get().get_client(domain_, fragment_, properties_);
}

MqttSubscriber::~MqttSubscriber() { unsubscribe(); }

std::any MqttSubscriber::get_native_handle() const { return this; }

bool MqttSubscriber::suspend() {
  is_suspend_ = true;
  return true;
}

bool MqttSubscriber::resume() {
  is_suspend_ = false;
  return true;
}

bool MqttSubscriber::is_suspend() const { return is_suspend_; }

void MqttSubscriber::subscribe() {
  bool expected = false;

  if VUNLIKELY (!has_subscribe_.compare_exchange_strong(expected, true)) {
    return;
  }

  auto weak_self = weak_from_this();

  MqttFactory::get().subscribe_topic(
      this, domain_, fragment_, properties_, topic_, qos_,
      [weak_self](const std::string& /*topic*/, const uint8_t* data, size_t size) {
        auto self = weak_self.lock();

        if VUNLIKELY (!self) {
          return;
        }

        if VUNLIKELY (self->is_suspend_) {
          return;
        }

        MqttHeader header;

        if VUNLIKELY (!MqttFactory::decode_header(header, data, size)) {
          VLOG_E("MqttFactory: Failed to decode subscriber header.");
          return;
        }

        Bytes msg_bytes =
            (size > kMqttHeaderSize) ? Bytes::shallow_copy(data + kMqttHeaderSize, size - kMqttHeaderSize) : Bytes();

        self->process_message(header.channel, header.seq, header.guid, header.timestamp, msg_bytes);
      });
}

void MqttSubscriber::unsubscribe() {
  bool expected = true;

  if VUNLIKELY (!has_subscribe_.compare_exchange_strong(expected, false)) {
    return;
  }

  MqttFactory::get().unsubscribe_topic(this, domain_, fragment_, properties_, topic_);
}

void MqttSubscriber::set_latency_and_lost_enabled(bool enable) {
  is_latency_and_lost_enabled_.store(enable, std::memory_order_release);
}

bool MqttSubscriber::is_latency_and_lost_enabled() const {
  return is_latency_and_lost_enabled_.load(std::memory_order_acquire);
}

int64_t MqttSubscriber::get_latency() const {
  if (!is_latency_and_lost_enabled_.load(std::memory_order_acquire)) {
    return 0;
  }

  return last_latency_.load(std::memory_order_relaxed);
}

const CalculateSample& MqttSubscriber::get_calculate_sample() const { return calc_sample_; }

void MqttSubscriber::process_message(uint64_t channel, uint64_t seq, uint64_t guid, uint64_t timestamp,
                                     const Bytes& bytes) {
  if VUNLIKELY (is_latency_and_lost_enabled_.load(std::memory_order_acquire)) {
    calc_sample_.update(seq, guid);
    last_latency_.store(ElapsedTimer::get_sys_timestamp(ElapsedTimer::kNano, false) - timestamp,
                        std::memory_order_relaxed);
  }

  auto* impl = get_first_impl();

  if VUNLIKELY (!impl) {
    return;
  }

  auto* message_loop = impl->get_message_loop();

  if (message_loop) {
    auto weak_self = weak_from_this();

    message_loop->post_task([weak_self, channel, bytes]() {
      auto self = weak_self.lock();

      if VUNLIKELY (!self) {
        return;
      }

      auto* impl = self->get_first_impl();

      if VUNLIKELY (!impl || !impl->get_message_loop()) {
        return;
      }

      self->traverse_msg_callback([channel, &bytes](NodeImpl* impl, const auto& callback) {
        const auto* conf_ptr = impl->get_target_conf<MqttConf>();

        if (static_cast<uint64_t>(conf_ptr->hash_code) != channel) {
          return;
        }

        callback(bytes);
      });
    });
  } else {
    traverse_msg_callback([channel, &bytes](NodeImpl* impl, const auto& callback) {
      const auto* conf_ptr = impl->get_target_conf<MqttConf>();

      if (static_cast<uint64_t>(conf_ptr->hash_code) != channel) {
        return;
      }

      callback(bytes);
    });
  }
}

// MqttServer
MqttServer::MqttServer(const MqttID& id) {
  const auto& [impl_type, address, domain, qos, fragment, properties] = id;

  topic_ = address;

  if (domain != 0) {
    topic_ += ("_" + std::to_string(domain));
  }

  guid_ = std::hash<std::string>{}(topic_) ^ (reinterpret_cast<uint64_t>(this) << 32);

  resp_topic_ = topic_ + MqttConf::kRespSuffix;
  domain_ = domain;
  qos_ = qos;
  fragment_ = fragment;
  properties_ = properties;

  MqttFactory::get().get_client(domain_, fragment_, properties_);
}

void MqttServer::start_listening() {
  bool expected = false;

  if VUNLIKELY (!has_listening_.compare_exchange_strong(expected, true)) {
    return;
  }

  auto weak_self = weak_from_this();

  MqttFactory::get().subscribe_topic(
      this, domain_, fragment_, properties_, topic_, qos_,
      [weak_self](const std::string& /*topic*/, const uint8_t* data, size_t size) {
        auto self = weak_self.lock();

        if VUNLIKELY (!self) {
          return;
        }

        if VUNLIKELY (self->is_suspend_) {
          return;
        }

        MqttHeader header;

        if VUNLIKELY (!MqttFactory::decode_header(header, data, size)) {
          VLOG_E("MqttFactory: Failed to decode server header.");
          return;
        }

        Bytes req_bytes =
            (size > kMqttHeaderSize) ? Bytes::shallow_copy(data + kMqttHeaderSize, size - kMqttHeaderSize) : Bytes();

        self->process_message(header.channel, header.seq, req_bytes);
      });
}

MqttServer::~MqttServer() { MqttFactory::get().unsubscribe_topic(this, domain_, fragment_, properties_, topic_); }

std::any MqttServer::get_native_handle() const { return this; }

bool MqttServer::suspend() {
  is_suspend_ = true;
  return true;
}

bool MqttServer::resume() {
  is_suspend_ = false;
  return true;
}

bool MqttServer::is_suspend() const { return is_suspend_; }

bool MqttServer::reply(uint64_t channel, uint64_t req_id, const Bytes& resp_data) {
  auto& factory = MqttFactory::get();

  if VUNLIKELY (resp_data.size() > static_cast<size_t>(std::numeric_limits<int>::max()) - kMqttHeaderSize) {
    VLOG_E("MqttFactory: Server response payload is too large, size=", resp_data.size(), ".");
    return false;
  }

  MqttHeader header{guid_, 0, channel, req_id};

  std::vector<uint8_t> payload(kMqttHeaderSize + resp_data.size());

  MqttFactory::encode_header(header, payload.data());

  if (!resp_data.empty()) {
    std::memcpy(payload.data() + kMqttHeaderSize, resp_data.data(), resp_data.size());
  }

  return factory.publish_topic(domain_, fragment_, properties_, resp_topic_, payload.data(), payload.size(), qos_);
}

void MqttServer::process_message(uint64_t channel, uint64_t seq, const Bytes& req_bytes) {
  auto* impl = get_first_impl();

  if VUNLIKELY (!impl) {
    return;
  }

  auto* message_loop = impl->get_message_loop();

  if (message_loop) {
    auto weak_self = weak_from_this();

    message_loop->post_task([weak_self, channel, seq, req_bytes]() {
      auto self = weak_self.lock();

      if VUNLIKELY (!self) {
        return;
      }

      auto* impl = self->get_first_impl();

      if VUNLIKELY (!impl || !impl->get_message_loop()) {
        return;
      }

      bool is_deferred = false;

      self->traverse_req_resp_callback(
          [self, channel, seq, &req_bytes, &is_deferred](NodeImpl* impl, const auto& callback) {
            const auto* conf_ptr = impl->get_target_conf<MqttConf>();

            if (static_cast<uint64_t>(conf_ptr->hash_code) != channel) {
              self->ignore_called();
              return;
            }

            if VUNLIKELY (self->has_called()) {
              VLOG_F(*conf_ptr, "Two identical service requests.");
              return;
            }

            if (static_cast<ServerImpl*>(impl)->is_resp_type) {
              Bytes resp_bytes;

              callback(seq, req_bytes, &resp_bytes);
              is_deferred = !static_cast<ServerImpl*>(impl)->is_sync_type;
            } else {
              callback(seq, req_bytes, nullptr);
              is_deferred = false;
            }
          });
    });
  } else {
    bool is_deferred = false;

    traverse_req_resp_callback([this, channel, seq, &req_bytes, &is_deferred](NodeImpl* impl, const auto& callback) {
      const auto* conf_ptr = impl->get_target_conf<MqttConf>();

      if (static_cast<uint64_t>(conf_ptr->hash_code) != channel) {
        ignore_called();
        return;
      }

      if VUNLIKELY (has_called()) {
        VLOG_F(*conf_ptr, "Two identical service requests.");
        return;
      }

      std::lock_guard lock(mtx_);

      if (static_cast<ServerImpl*>(impl)->is_resp_type) {
        Bytes resp_bytes;

        callback(seq, req_bytes, &resp_bytes);
        is_deferred = !static_cast<ServerImpl*>(impl)->is_sync_type;
      } else {
        callback(seq, req_bytes, nullptr);
        is_deferred = false;
      }
    });
  }
}

void MqttServer::stop_listening() {
  bool expected = true;

  if VUNLIKELY (!has_listening_.compare_exchange_strong(expected, false)) {
    return;
  }

  MqttFactory::get().unsubscribe_topic(this, domain_, fragment_, properties_, topic_);
}

// MqttClient
MqttClient::MqttClient(const MqttID& id) {
  const auto& [impl_type, address, domain, qos, fragment, properties] = id;

  topic_ = address;

  if (domain != 0) {
    topic_ += ("_" + std::to_string(domain));
  }

  guid_ = std::hash<std::string>{}(topic_) ^ (reinterpret_cast<uint64_t>(this) << 32);

  resp_topic_ = topic_ + MqttConf::kRespSuffix;
  domain_ = domain;
  qos_ = qos;
  fragment_ = fragment;
  properties_ = properties;

  MqttFactory::get().get_client(domain_, fragment_, properties_);

  has_connected_.store(MqttFactory::get().is_client_connected(domain_, fragment_, properties_),
                       std::memory_order_release);
}

void MqttClient::start_listening() {
  bool expected = false;

  if VUNLIKELY (!has_listening_.compare_exchange_strong(expected, true)) {
    return;
  }

  auto weak_self = weak_from_this();

  MqttFactory::get().subscribe_topic(this, domain_, fragment_, properties_, resp_topic_, qos_,
                                     [weak_self](const std::string& /*topic*/, const uint8_t* data, size_t size) {
                                       auto self = weak_self.lock();

                                       if VUNLIKELY (!self) {
                                         return;
                                       }

                                       MqttHeader header;

                                       if VUNLIKELY (!MqttFactory::decode_header(header, data, size)) {
                                         VLOG_E("MqttFactory: Failed to decode client response header.");
                                         return;
                                       }

                                       Bytes resp_bytes =
                                           Bytes::deep_copy(data + kMqttHeaderSize, size - kMqttHeaderSize);

                                       Function<void(uint64_t, const Bytes&)> callback;
                                       NodeImpl* owner = nullptr;

                                       {
                                         std::lock_guard lock(self->mtx_);

                                         auto iter = self->callbacks_.find(header.seq);

                                         if (iter != self->callbacks_.end()) {
                                           owner = iter->second.owner;
                                           callback = std::move(iter->second.callback);
                                           self->callbacks_.erase(iter);
                                         }
                                       }

                                       if (callback && self->is_contains_impl(owner)) {
                                         callback(header.channel, resp_bytes);
                                       }
                                     });
}

MqttClient::~MqttClient() {
  MqttFactory::get().unsubscribe_topic(this, domain_, fragment_, properties_, resp_topic_);
  MqttFactory::get().unregister_connection_callback(this);
}

std::any MqttClient::get_native_handle() const { return this; }

bool MqttClient::is_connected() const {
  return MqttFactory::get().is_client_connected(domain_, fragment_, properties_);
}

void MqttClient::stop_listening() {
  bool expected = true;

  if VUNLIKELY (!has_listening_.compare_exchange_strong(expected, false)) {
    return;
  }

  MqttFactory::get().unsubscribe_topic(this, domain_, fragment_, properties_, resp_topic_);
}

void MqttClient::enable_connection_notifications() {
  auto weak_self = weak_from_this();

  MqttFactory::get().register_connection_callback(this, domain_, fragment_, properties_, [weak_self](bool connected) {
    auto self = weak_self.lock();

    if VUNLIKELY (!self) {
      return;
    }

    self->has_connected_.store(connected, std::memory_order_release);
    self->traverse_server_connect_callback([connected](NodeImpl*, const auto& callback) { callback(connected); });
  });
}

bool MqttClient::call(NodeImpl* owner, uint64_t channel, const Bytes& req_data, NodeImpl::MsgCallback&& callback,
                      int timeout_ms) {
  auto& factory = MqttFactory::get();

  if VUNLIKELY (req_data.size() > static_cast<size_t>(std::numeric_limits<int>::max()) - kMqttHeaderSize) {
    VLOG_E("MqttFactory: Client request payload is too large, size=", req_data.size(), ".");
    return false;
  }

  uint64_t seq_guid = Helpers::hash_combine(guid_, ++seq_);

  MqttHeader header{guid_, 0, channel, seq_guid};

  std::vector<uint8_t> payload(kMqttHeaderSize + req_data.size());

  MqttFactory::encode_header(header, payload.data());

  if (!req_data.empty()) {
    std::memcpy(payload.data() + kMqttHeaderSize, req_data.data(), req_data.size());
  }

  const bool has_callback = static_cast<bool>(callback);

  if VLIKELY (has_callback) {
    std::lock_guard lock(mtx_);

    callbacks_[seq_guid] =
        ResponseCallback{owner, [callback = std::move(callback), channel](uint64_t target_channel, const Bytes& bytes) {
                           if (channel != target_channel) {
                             return;
                           }

                           callback(bytes);
                         }};
  }

  bool published = factory.publish_topic(domain_, fragment_, properties_, topic_, payload.data(), payload.size(), qos_);

  if VUNLIKELY (!published) {
    if VLIKELY (has_callback) {
      std::lock_guard lock(mtx_);
      callbacks_.erase(seq_guid);
    }

    return false;
  }

  if (has_callback && timeout_ms > 0) {
    auto weak_self = weak_from_this();
    auto& message_loop = MqttFactory::get().get_message_loop();

    bool posted = Timer::call_once(&message_loop, static_cast<uint32_t>(timeout_ms), [weak_self, seq_guid]() {
      auto self = weak_self.lock();

      if VUNLIKELY (!self) {
        return;
      }

      std::lock_guard lock(self->mtx_);
      self->callbacks_.erase(seq_guid);
    });

    if VUNLIKELY (!posted) {
      std::lock_guard lock(mtx_);
      callbacks_.erase(seq_guid);
      VLOG_W("MqttFactory: Failed to schedule MQTT call timeout cleanup.");
    }
  }

  return true;
}

void MqttClient::cancel_calls(NodeImpl* owner) {
  std::lock_guard lock(mtx_);

  for (auto iter = callbacks_.begin(); iter != callbacks_.end();) {
    if (iter->second.owner == owner) {
      iter = callbacks_.erase(iter);
    } else {
      ++iter;
    }
  }
}

}  // namespace vlink
