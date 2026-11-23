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
#include <string_view>
#include <utility>

#include "./base/elapsed_timer.h"
#include "./base/helpers.h"
#include "./base/timer.h"
#include "./base/utils.h"
#include "./base/uuid.h"
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

static bool post_connection_task(MessageLoop& loop, MessageLoop::Callback&& callback) {
  PostTaskOptions options;
  options.drop_policy = TaskDropPolicy::kProtected;

  return loop.post_task_handle(std::move(callback), options).state() != TaskExecutionState::kRejected;
}

static bool is_mqtt_wildcard(std::string_view topic) {
  return topic.find('+') != std::string_view::npos || topic.find('#') != std::string_view::npos;
}

static bool mqtt_topic_matches(std::string_view filter, std::string_view topic) {
  if (filter == topic) {
    return true;
  }

  if (!is_mqtt_wildcard(filter)) {
    return false;
  }

  if (!topic.empty() && topic[0] == '$' && (filter.empty() || filter[0] != '$')) {
    return false;
  }

  size_t f_pos = 0;
  size_t t_pos = 0;

  while (f_pos <= filter.size() && t_pos <= topic.size()) {
    size_t f_end = filter.find('/', f_pos);
    std::string_view f_level =
        (f_end == std::string_view::npos) ? filter.substr(f_pos) : filter.substr(f_pos, f_end - f_pos);

    if (f_level == "#") {
      return f_end == std::string_view::npos;
    }

    size_t t_end = topic.find('/', t_pos);
    std::string_view t_level =
        (t_end == std::string_view::npos) ? topic.substr(t_pos) : topic.substr(t_pos, t_end - t_pos);

    if (f_level != "+" && f_level != t_level) {
      return false;
    }

    const bool f_done = (f_end == std::string_view::npos);
    const bool t_done = (t_end == std::string_view::npos);

    if (f_done && t_done) {
      return true;
    }

    if (f_done != t_done) {
      if (t_done && !f_done && filter.substr(f_end + 1) == "#") {
        return true;
      }

      return false;
    }

    f_pos = f_end + 1;
    t_pos = t_end + 1;
  }

  return false;
}

static bool publish_mqtt_payload(MqttFactory& factory, int32_t domain, const std::string& fragment,
                                 const Transport::PropertiesMap& properties, const std::string& topic, int qos,
                                 const MqttHeader& header, const Bytes& bytes) {
  auto payload = Bytes::create(kMqttHeaderSize + bytes.size());

  if VUNLIKELY (payload.empty()) {
    return false;
  }

  MqttFactory::encode_header(header, payload.data());

  if (!bytes.empty()) {
    std::memcpy(payload.data() + kMqttHeaderSize, bytes.data(), bytes.size());
  }

  return factory.publish_topic(domain, fragment, properties, topic, payload.data(), payload.size(), qos);
}

// MqttFactory
MqttFactory::MqttFactory() {
  Bytes::init_memory_pool();

  if VUNLIKELY (MqttTransport::get_thread_count() != 1) {
    VLOG_W("MqttTransport: Mqtt does not support setting thread count.");
  }

  init();
}

MqttFactory::~MqttFactory() {
#ifdef _WIN32
  if (Utils::is_terminating()) {
    return;
  }
#endif

  deinit();
}

void MqttFactory::init() {
  bool expected = false;

  if VUNLIKELY (!has_inited_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                     std::memory_order_relaxed)) {
    return;
  }

  message_loop_.async_run();
  reconnect_loop_.async_run();
}

void MqttFactory::deinit() {
  bool expected = true;

  if VUNLIKELY (!has_inited_.compare_exchange_strong(expected, false, std::memory_order_acq_rel,
                                                     std::memory_order_relaxed)) {
    return;
  }

  const bool in_reconnect_loop = reconnect_loop_.is_in_same_thread();

  reconnect_loop_.quit();

  if VLIKELY (!in_reconnect_loop) {
    reconnect_loop_.wait_for_quit();
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
    wildcard_sub_count_ = 0;
  }

  {
    std::lock_guard lock(connection_mtx_);
    connection_callbacks_.clear();
  }

  std::map<MqttSessionID, SessionPtr> sessions;
  {
    std::lock_guard lock(client_mtx_);
    sessions.swap(client_map_);
  }

  for (auto& [session_id, session] : sessions) {
    (void)session_id;

    if (!session) {
      continue;
    }

    std::lock_guard session_lock(session->mtx);

    if (session->context) {
      session->context->factory.store(nullptr, std::memory_order_release);
    }

    if (session->client) {
      if (MQTTClient_isConnected(session->client)) {
        MQTTClient_disconnect(session->client, 1000);
      }

      MQTTClient_destroy(&session->client);
    }

    session->connected = false;
    session->dirty_topics.clear();
  }
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

MqttFactory::SessionPtr MqttFactory::get_or_create_session(const MqttSessionID& session_id) {
  std::lock_guard lock(client_mtx_);

  auto [iter, inserted] = client_map_.try_emplace(session_id);

  if (inserted || !iter->second) {
    iter->second = std::make_shared<SessionEntry>();
  }

  return iter->second;
}

MqttFactory::SessionPtr MqttFactory::find_session(const MqttSessionID& session_id) {
  std::lock_guard lock(client_mtx_);

  auto iter = client_map_.find(session_id);

  if (iter == client_map_.end()) {
    return nullptr;
  }

  return iter->second;
}

MQTTClient* MqttFactory::get_client(int32_t domain, const std::string& fragment,
                                    const Transport::PropertiesMap& properties) {
  MqttSessionID session_id = MqttSessionID{domain, fragment, properties};
  auto session = get_or_create_session(session_id);

  int rc = MQTTCLIENT_SUCCESS;
  {
    std::lock_guard session_lock(session->mtx);
    rc = connect_session_locked(session_id, *session);
  }

  if VUNLIKELY (rc == MQTTCLIENT_SSL_NOT_SUPPORTED) {
    std::lock_guard lock(client_mtx_);
    auto iter = client_map_.find(session_id);

    if (iter != client_map_.end() && iter->second == session) {
      client_map_.erase(iter);
    }

    return nullptr;
  }

  if (rc != MQTTCLIENT_SUCCESS) {
    schedule_reconnect(1000);
  }

  return &session->client;
}

bool MqttFactory::is_client_connected(int32_t domain, const std::string& fragment,
                                      const Transport::PropertiesMap& properties) {
  auto session = find_session(MqttSessionID{domain, fragment, properties});

  if (!session) {
    return false;
  }

  std::lock_guard session_lock(session->mtx);

  return session->client != nullptr && MQTTClient_isConnected(session->client) != 0;
}

MessageLoop& MqttFactory::get_message_loop() { return message_loop_; }

void MqttFactory::subscribe_topic(void* owner, int32_t domain, const std::string& fragment,
                                  const Transport::PropertiesMap& properties, const std::string& topic, int qos,
                                  Function<void(const std::string&, const uint8_t*, size_t)>&& callback) {
  MqttSessionID session_id = MqttSessionID{domain, fragment, properties};
  auto session = get_or_create_session(session_id);

  int connect_rc = MQTTCLIENT_SUCCESS;
  {
    std::lock_guard session_lock(session->mtx);
    connect_rc = connect_session_locked(session_id, *session);
  }

  if VUNLIKELY (connect_rc == MQTTCLIENT_SSL_NOT_SUPPORTED) {
    std::lock_guard lock(client_mtx_);
    auto iter = client_map_.find(session_id);

    if (iter != client_map_.end() && iter->second == session) {
      client_map_.erase(iter);
    }

    return;
  }

  bool need_reconnect = connect_rc != MQTTCLIENT_SUCCESS;
  MqttSubscriptionID subscription_id = MqttSubscriptionID{std::move(session_id), topic};
  int target_qos = qos;
  bool should_subscribe = false;

  {
    std::lock_guard session_lock(session->mtx);

    {
      std::lock_guard lock(sub_mtx_);

      auto& owner_map = subscription_callbacks_[std::move(subscription_id)];
      int old_target_qos = 0;

      for (const auto& owner_entry : owner_map) {
        old_target_qos = std::max(old_target_qos, owner_entry.second.qos);
      }

      bool had_subscription = !owner_map.empty();

      if (!had_subscription && is_mqtt_wildcard(topic)) {
        ++wildcard_sub_count_;
      }

      owner_map[owner] = Subscription{qos, std::move(callback)};

      for (const auto& owner_entry : owner_map) {
        target_qos = std::max(target_qos, owner_entry.second.qos);
      }

      should_subscribe = !had_subscription || target_qos != old_target_qos;
    }

    if (should_subscribe) {
      if (session->client && MQTTClient_isConnected(session->client)) {
        int rc = MQTTClient_subscribe(session->client, topic.c_str(), target_qos);

        if VUNLIKELY (rc != MQTTCLIENT_SUCCESS) {
          VLOG_E("MqttFactory: Failed to subscribe to topic '", topic, "', rc=", rc, ".");
          session->dirty_topics.emplace(topic);
          need_reconnect = true;
        } else {
          session->dirty_topics.erase(topic);
        }
      } else {
        session->dirty_topics.emplace(topic);
        need_reconnect = true;
      }
    }
  }

  if (need_reconnect) {
    schedule_reconnect(1000);
  }
}

void MqttFactory::unsubscribe_topic(void* owner, int32_t domain, const std::string& fragment,
                                    const Transport::PropertiesMap& properties, const std::string& topic) {
  MqttSessionID session_id = MqttSessionID{domain, fragment, properties};
  auto session = find_session(session_id);
  MqttSubscriptionID subscription_id = MqttSubscriptionID{std::move(session_id), topic};
  bool should_unsubscribe = false;
  bool should_resubscribe = false;
  int old_target_qos = 0;
  int target_qos = 0;
  bool need_reconnect = false;

  auto update_subscriptions = [&]() -> bool {
    std::lock_guard lock(sub_mtx_);

    auto iter = subscription_callbacks_.find(subscription_id);

    if (iter == subscription_callbacks_.end()) {
      return false;
    }

    for (const auto& owner_entry : iter->second) {
      old_target_qos = std::max(old_target_qos, owner_entry.second.qos);
    }

    if (iter->second.erase(owner) == 0) {
      return false;
    }

    if (iter->second.empty()) {
      if (is_mqtt_wildcard(topic) && wildcard_sub_count_ > 0) {
        --wildcard_sub_count_;
      }

      subscription_callbacks_.erase(iter);
      should_unsubscribe = true;
    } else {
      for (const auto& owner_entry : iter->second) {
        target_qos = std::max(target_qos, owner_entry.second.qos);
      }

      should_resubscribe = target_qos != old_target_qos;
    }

    return true;
  };

  if (!session) {
    update_subscriptions();
    return;
  }

  {
    std::lock_guard session_lock(session->mtx);

    if (!update_subscriptions()) {
      return;
    }

    if (should_unsubscribe) {
      session->dirty_topics.erase(topic);
    }

    if (!session->client || MQTTClient_isConnected(session->client) == 0) {
      return;
    }

    if (should_unsubscribe) {
      MQTTClient_unsubscribe(session->client, topic.c_str());
    } else if (should_resubscribe) {
      int rc = MQTTClient_subscribe(session->client, topic.c_str(), target_qos);

      if VUNLIKELY (rc != MQTTCLIENT_SUCCESS) {
        VLOG_E("MqttFactory: Failed to resubscribe to topic '", topic, "', rc=", rc, ".");
        session->dirty_topics.emplace(topic);
        need_reconnect = true;
      } else {
        session->dirty_topics.erase(topic);
      }
    }
  }

  if (need_reconnect) {
    schedule_reconnect(1000);
  }
}

bool MqttFactory::publish_topic(int32_t domain, const std::string& fragment, const Transport::PropertiesMap& properties,
                                const std::string& topic, const uint8_t* payload, size_t payload_size, int qos) {
  if VUNLIKELY (payload_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
    VLOG_E("MqttFactory: Payload is too large for MQTTClient_publish, size=", payload_size, ".");
    return false;
  }

  auto session = find_session(MqttSessionID{domain, fragment, properties});

  if VUNLIKELY (!session) {
    return false;
  }

  std::lock_guard session_lock(session->mtx);

  if VUNLIKELY (!session->client || !MQTTClient_isConnected(session->client)) {
    return false;
  }

  int rc = MQTTClient_publish(session->client, topic.c_str(), static_cast<int>(payload_size),
                              const_cast<uint8_t*>(payload), qos, 0, nullptr);

  return rc == MQTTCLIENT_SUCCESS;
}

void MqttFactory::register_connection_callback(void* owner, int32_t domain, const std::string& fragment,
                                               const Transport::PropertiesMap& properties,
                                               Function<void(bool)>&& callback) {
  std::lock_guard lock(connection_mtx_);
  connection_callbacks_[owner] = {MqttSessionID{domain, fragment, properties}, std::move(callback)};
}

void MqttFactory::unregister_connection_callback(void* owner) {
  std::lock_guard lock(connection_mtx_);
  connection_callbacks_.erase(owner);
}

int MqttFactory::connect_session_locked(const MqttSessionID& session_id, SessionEntry& session, bool retry_dirty) {
  const auto& [domain, fragment, properties] = session_id;

  if (session.client && MQTTClient_isConnected(session.client)) {
    if (!retry_dirty || session.dirty_topics.empty()) {
      return MQTTCLIENT_SUCCESS;
    }

    return restore_subscriptions_locked(session_id, session, true) ? MQTTCLIENT_SUCCESS : MQTTCLIENT_FAILURE;
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

  const bool ssl_cfg_valid = ssl_cfg.is_valid();

  if (ssl_cfg_valid) {
    if (Helpers::has_startwith(broker, "tcp://")) {
      broker = "ssl://" + broker.substr(6);
    } else if (Helpers::has_startwith(broker, "ws://")) {
      broker = "wss://" + broker.substr(5);
    }
  }

  const bool use_ssl = ssl_cfg_valid || Helpers::has_startwith(broker, "ssl://") ||
                       Helpers::has_startwith(broker, "tls://") || Helpers::has_startwith(broker, "mqtts://") ||
                       Helpers::has_startwith(broker, "wss://");

#ifndef VLINK_MQTT_HAS_SSL
  if VUNLIKELY (use_ssl) {
    VLOG_E("MqttFactory: TLS/SSL requested but linked without paho-mqtt3cs SSL support.");
    return MQTTCLIENT_SSL_NOT_SUPPORTED;
  }
#endif

  set_connection_state_locked(session_id, session, false);

  if VUNLIKELY (use_ssl && !ssl_cfg.server_name.empty()) {
    VLOG_W("MqttFactory: ssl.server_name (SNI) is not supported by Paho MQTT C.");
  }

  MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
  conn_opts.keepAliveInterval = env_keepalive >= 0 ? env_keepalive : 60;
  conn_opts.connectTimeout = 5;
  conn_opts.cleansession = 1;

  MQTTClient_SSLOptions ssl_opts = MQTTClient_SSLOptions_initializer;

  if (use_ssl) {
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

  if (!session.client) {
    if (!session.context) {
      session.context = std::make_unique<ClientContext>();
    }

    session.context->factory.store(this, std::memory_order_release);
    session.context->session_id = session_id;

    std::string client_id = env_client_prefix + "_" + std::to_string(domain) + "_" +
                            Uuid::generate_random().to_compact_string().substr(0, 12);

    int rc =
        MQTTClient_create(&session.client, broker.c_str(), client_id.c_str(), MQTTCLIENT_PERSISTENCE_NONE, nullptr);

    if VUNLIKELY (rc != MQTTCLIENT_SUCCESS) {
      VLOG_E("MqttFactory: Failed to invoke [MQTTClient_create], rc=", rc, ".");
      return MQTTCLIENT_FAILURE;
    }

    rc = MQTTClient_setCallbacks(session.client, session.context.get(), on_connection_lost, on_message_arrived,
                                 on_delivered);

    if VUNLIKELY (rc != MQTTCLIENT_SUCCESS) {
      VLOG_E("MqttFactory: Failed to invoke [MQTTClient_setCallbacks], rc=", rc, ".");
      MQTTClient_destroy(&session.client);
      return MQTTCLIENT_FAILURE;
    }
  }

  ClientContext* ctx = session.context.get();

  int rc = MQTTClient_connect(session.client, &conn_opts);

  if VUNLIKELY (rc != MQTTCLIENT_SUCCESS) {
    if (!ctx || ctx->last_failed_rc != rc) {
      VLOG_E("MqttFactory: Failed to invoke [MQTTClient_connect], broker=", broker, " rc=", rc, ".");

      if (ctx) {
        ctx->last_failed_rc = rc;
      }
    }

    return MQTTCLIENT_FAILURE;
  }

  if (ctx) {
    ctx->last_failed_rc = 0;
  }

  set_connection_state_locked(session_id, session, true);

  return restore_subscriptions_locked(session_id, session, false) ? MQTTCLIENT_SUCCESS : MQTTCLIENT_FAILURE;
}

bool MqttFactory::restore_subscriptions_locked(const MqttSessionID& session_id, SessionEntry& session,
                                               bool only_dirty) {
  std::vector<std::pair<std::string, int>> subscriptions;

  {
    std::lock_guard lock(sub_mtx_);
    auto iter = subscription_callbacks_.lower_bound({session_id, {}});

    for (; iter != subscription_callbacks_.end() && std::get<0>(iter->first) == session_id; ++iter) {
      if (iter->second.empty()) {
        continue;
      }

      const auto& topic = std::get<1>(iter->first);

      if (only_dirty && session.dirty_topics.count(topic) == 0U) {
        continue;
      }

      int qos = 0;

      for (const auto& owner_entry : iter->second) {
        qos = std::max(qos, owner_entry.second.qos);
      }

      subscriptions.emplace_back(topic, qos);
    }
  }

  std::unordered_set<std::string> failed_topics;

  for (const auto& [topic, qos] : subscriptions) {
    int rc = MQTTClient_subscribe(session.client, topic.c_str(), qos);

    if VUNLIKELY (rc != MQTTCLIENT_SUCCESS) {
      VLOG_E("MqttFactory: Failed to subscribe to topic '", topic, "', rc=", rc, ".");
      failed_topics.emplace(topic);
    }
  }

  session.dirty_topics = std::move(failed_topics);
  return session.dirty_topics.empty();
}

void MqttFactory::set_connection_state_locked(const MqttSessionID& session_id, SessionEntry& session, bool connected) {
  if (session.connected == connected) {
    return;
  }

  session.connected = connected;
  post_connection_task(message_loop_,
                       [this, session_id, connected]() { notify_connection_change(session_id, connected); });
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

void MqttFactory::schedule_reconnect(uint32_t delay_ms) {
  if (!has_inited_.load(std::memory_order_acquire)) {
    return;
  }

  bool expected = false;

  if (!reconnect_scheduled_.compare_exchange_strong(expected, true, std::memory_order_acquire,
                                                    std::memory_order_relaxed)) {
    return;
  }

  auto reconnect_task = [this]() {
    reconnect_scheduled_.store(false, std::memory_order_release);

    if (has_inited_.load(std::memory_order_acquire) && reconnect_all()) {
      schedule_reconnect(1000);
    }
  };

  bool posted = false;

  if (delay_ms == 0) {
    posted = post_connection_task(reconnect_loop_, std::move(reconnect_task));
  } else {
    posted = Timer::call_once(&reconnect_loop_, delay_ms, std::move(reconnect_task));
  }

  if VUNLIKELY (!posted) {
    reconnect_scheduled_.store(false, std::memory_order_release);
  }
}

int MqttFactory::on_message_arrived(void* context, char* topic_name, int topic_len, MQTTClient_message* message) {
  auto* client_context = static_cast<ClientContext*>(context);

  auto* factory = client_context ? client_context->factory.load(std::memory_order_acquire) : nullptr;

  if VUNLIKELY (!factory) {
    MQTTClient_freeMessage(&message);
    MQTTClient_free(topic_name);
    return 1;
  }

  MqttSubscriptionID subscription_id = MqttSubscriptionID{
      client_context->session_id,
      (topic_len > 0) ? std::string(topic_name, static_cast<size_t>(topic_len)) : std::string(topic_name)};

  const auto* payload = static_cast<const uint8_t*>(message->payload);
  auto payload_len = static_cast<size_t>(message->payloadlen);

  Bytes data_copy = Bytes::deep_copy(payload, payload_len);

  MQTTClient_freeMessage(&message);
  MQTTClient_free(topic_name);

  factory->message_loop_.post_task([factory, subscription_id = std::move(subscription_id),
                                    data = std::move(data_copy)]() {
    std::vector<Function<void(const std::string&, const uint8_t*, size_t)>> callbacks;

    {
      std::lock_guard lock(factory->sub_mtx_);

      const auto& target_session = std::get<0>(subscription_id);
      const auto& target_topic = std::get<1>(subscription_id);

      auto iter = factory->subscription_callbacks_.find(subscription_id);

      if (iter != factory->subscription_callbacks_.end()) {
        for (const auto& owner_entry : iter->second) {
          if (owner_entry.second.callback) {
            callbacks.emplace_back(owner_entry.second.callback);
          }
        }
      }

      if VUNLIKELY (factory->wildcard_sub_count_ > 0) {
        auto scan_iter = factory->subscription_callbacks_.lower_bound({target_session, {}});

        for (; scan_iter != factory->subscription_callbacks_.end() && std::get<0>(scan_iter->first) == target_session;
             ++scan_iter) {
          const auto& filter_topic = std::get<1>(scan_iter->first);

          if (filter_topic == target_topic || !is_mqtt_wildcard(filter_topic)) {
            continue;
          }

          if (mqtt_topic_matches(filter_topic, target_topic)) {
            for (const auto& owner_entry : scan_iter->second) {
              if (owner_entry.second.callback) {
                callbacks.emplace_back(owner_entry.second.callback);
              }
            }
          }
        }
      }
    }

    for (const auto& callback : callbacks) {
      callback(std::get<1>(subscription_id), data.data(), data.size());
    }
  });

  return 1;
}

// NOLINTNEXTLINE(readability-non-const-parameter)
void MqttFactory::on_connection_lost(void* context, char* cause) {
  auto* client_context = static_cast<ClientContext*>(context);

  auto* factory = client_context ? client_context->factory.load(std::memory_order_acquire) : nullptr;

  if VUNLIKELY (!factory) {
    return;
  }

  post_connection_task(factory->message_loop_, [factory, session_id = client_context->session_id,
                                                reason = std::string(cause ? cause : "unknown")]() {
    VLOG_W("MqttFactory: Connection lost, cause: ", reason, ".");

    auto session = factory->find_session(session_id);

    if (!session) {
      return;
    }

    {
      std::lock_guard session_lock(session->mtx);

      if (session->client && MQTTClient_isConnected(session->client)) {
        return;
      }

      factory->set_connection_state_locked(session_id, *session, false);
    }

    factory->schedule_reconnect();
  });
}

void MqttFactory::on_delivered(void* context, MQTTClient_deliveryToken dt) {
  (void)context;
  (void)dt;
}

bool MqttFactory::reconnect_all() {
  bool has_disconnected = false;
  std::vector<std::pair<MqttSessionID, SessionPtr>> sessions;

  {
    std::lock_guard client_lock(client_mtx_);
    sessions.reserve(client_map_.size());

    for (const auto& [session_id, session] : client_map_) {
      if (session) {
        sessions.emplace_back(session_id, session);
      }
    }
  }

  for (auto& [session_id, session] : sessions) {
    if (!has_inited_.load(std::memory_order_acquire)) {
      break;
    }

    std::lock_guard session_lock(session->mtx);

    if (connect_session_locked(session_id, *session, true) != MQTTCLIENT_SUCCESS) {
      has_disconnected = true;
    }
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

  guid_ = decode_u64be(Uuid::generate_random().bytes().data() + sizeof(guid_));

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
  MqttFactory::get().register_connection_callback(
      this, domain_, fragment_, properties_, [weak_self = weak_from_this()](bool connected) {
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

  MqttHeader header{guid_, ElapsedTimer::get_sys_timestamp(ElapsedTimer::kNano, false), channel,
                    seq_.fetch_add(1, std::memory_order_relaxed) + 1};

  return publish_mqtt_payload(factory, domain_, fragment_, properties_, topic_, qos_, header, bytes);
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

void MqttSubscriber::subscribe() {
  bool expected = false;

  if VUNLIKELY (!has_subscribe_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                        std::memory_order_relaxed)) {
    return;
  }

  MqttFactory::get().subscribe_topic(
      this, domain_, fragment_, properties_, topic_, qos_,
      [weak_self = weak_from_this()](const std::string& topic, const uint8_t* data, size_t size) {
        (void)topic;

        auto self = weak_self.lock();

        if VUNLIKELY (!self) {
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

  if VUNLIKELY (!has_subscribe_.compare_exchange_strong(expected, false, std::memory_order_acq_rel,
                                                        std::memory_order_relaxed)) {
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

  LoopRoute first_route;
  std::vector<LoopRoute> other_routes;

  traverse_msg_callback([&](NodeImpl* impl, const auto& callback) {
    const auto* transport = impl->get_target_transport<MqttTransport>();

    if VUNLIKELY (static_cast<uint64_t>(transport->hash_code) != channel || impl->has_suspend) {
      return;
    }

    auto* loop = impl->get_message_loop();

    if (!loop) {
      callback(bytes);
    } else if (!first_route.loop) {
      first_route = {loop, impl->get_loop_gate()};
    } else if (loop != first_route.loop &&
               std::find(other_routes.begin(), other_routes.end(), loop) == other_routes.end()) {
      other_routes.push_back({loop, impl->get_loop_gate()});
    }
  });

  if (!first_route.loop) {
    return;
  }

  if (other_routes.empty()) {
    post_to_subscriber_loop(first_route, [weak = weak_from_this(), loop = first_route.loop, channel, bytes = bytes]() {
      auto self = weak.lock();

      if VUNLIKELY (!self) {
        return;
      }

      self->deliver_message(loop, channel, bytes);
    });

    return;
  }

  auto data = std::make_shared<Bytes>(bytes);

  auto post = [&](const LoopRoute& route) {
    post_to_subscriber_loop(route, [weak = weak_from_this(), loop = route.loop, channel, data]() {
      auto self = weak.lock();

      if VUNLIKELY (!self) {
        return;
      }

      self->deliver_message(loop, channel, *data);
    });
  };

  post(first_route);

  for (const auto& route : other_routes) {
    post(route);
  }
}

void MqttSubscriber::deliver_message(MessageLoop* loop, uint64_t channel, const Bytes& bytes) {
  traverse_msg_callback([&](NodeImpl* impl, const auto& callback) {
    const auto* transport = impl->get_target_transport<MqttTransport>();

    if (impl->get_message_loop() == loop && !impl->has_suspend &&
        static_cast<uint64_t>(transport->hash_code) == channel) {
      callback(bytes);
    }
  });
}

// MqttServer
MqttServer::MqttServer(const MqttID& id) {
  const auto& [impl_type, address, domain, qos, fragment, properties] = id;

  topic_ = address;

  if (domain != 0) {
    topic_ += ("_" + std::to_string(domain));
  }

  guid_ = std::hash<std::string>{}(topic_) ^ (reinterpret_cast<uint64_t>(this) << 32);

  resp_topic_ = topic_ + MqttTransport::kRespSuffix;
  domain_ = domain;
  qos_ = qos;
  fragment_ = fragment;
  properties_ = properties;

  MqttFactory::get().get_client(domain_, fragment_, properties_);
}

MqttServer::~MqttServer() { MqttFactory::get().unsubscribe_topic(this, domain_, fragment_, properties_, topic_); }

std::any MqttServer::get_native_handle() const { return this; }

void MqttServer::start_listening() {
  bool expected = false;

  if VUNLIKELY (!has_listening_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                        std::memory_order_relaxed)) {
    return;
  }

  MqttFactory::get().subscribe_topic(
      this, domain_, fragment_, properties_, topic_, qos_,
      [weak_self = weak_from_this()](const std::string& topic, const uint8_t* data, size_t size) {
        (void)topic;

        auto self = weak_self.lock();

        if VUNLIKELY (!self) {
          return;
        }

        if VUNLIKELY (self->is_suspend_.load(std::memory_order_relaxed)) {
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

bool MqttServer::suspend() {
  is_suspend_.store(true, std::memory_order_relaxed);
  return true;
}

bool MqttServer::resume() {
  is_suspend_.store(false, std::memory_order_relaxed);
  return true;
}

bool MqttServer::is_suspend() const { return is_suspend_.load(std::memory_order_relaxed); }

bool MqttServer::reply(uint64_t channel, uint64_t req_id, const Bytes& resp_data) {
  auto& factory = MqttFactory::get();

  if VUNLIKELY (resp_data.size() > static_cast<size_t>(std::numeric_limits<int>::max()) - kMqttHeaderSize) {
    VLOG_E("MqttFactory: Server response payload is too large, size=", resp_data.size(), ".");
    return false;
  }

  MqttHeader header{guid_, 0, channel, req_id};

  return publish_mqtt_payload(factory, domain_, fragment_, properties_, resp_topic_, qos_, header, resp_data);
}

void MqttServer::process_message(uint64_t channel, uint64_t seq, const Bytes& req_bytes) {
  NodeImpl* owner = nullptr;
  MessageLoop* message_loop = nullptr;
  NodeImpl::LoopGatePtr loop_gate;

  traverse_req_resp_callback([&](NodeImpl* impl, const auto& callback) {
    const auto* transport = impl->get_target_transport<MqttTransport>();

    if VUNLIKELY (static_cast<uint64_t>(transport->hash_code) != channel) {
      ignore_called();
      return;
    }

    if VUNLIKELY (has_called()) {
      VLOG_F(*transport, "Two identical service requests.");
      return;
    }

    owner = impl;
    message_loop = impl->get_message_loop();

    if (message_loop) {
      loop_gate = impl->get_loop_gate();
    } else {
      Bytes resp_bytes;

      callback(seq, req_bytes, static_cast<ServerImpl*>(impl)->is_resp_type ? &resp_bytes : nullptr);
    }
  });

  if (message_loop) {
    NodeImpl::post_to_loop_gate(
        loop_gate, message_loop, [weak = weak_from_this(), owner, message_loop, seq, req_bytes = req_bytes]() {
          auto self = weak.lock();

          if VUNLIKELY (!self) {
            return;
          }

          self->invoke_req_resp_callback(owner, [&](NodeImpl* impl, const auto& callback) {
            if VLIKELY (impl->get_message_loop() == message_loop) {
              Bytes resp_bytes;

              callback(seq, req_bytes, static_cast<ServerImpl*>(impl)->is_resp_type ? &resp_bytes : nullptr);
            }
          });
        });
  }
}

void MqttServer::stop_listening() {
  bool expected = true;

  if VUNLIKELY (!has_listening_.compare_exchange_strong(expected, false, std::memory_order_acq_rel,
                                                        std::memory_order_relaxed)) {
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

  guid_ = decode_u64be(Uuid::generate_random().bytes().data() + sizeof(guid_));

  resp_topic_ = topic_ + MqttTransport::kRespSuffix;
  domain_ = domain;
  qos_ = qos;
  fragment_ = fragment;
  properties_ = properties;

  MqttFactory::get().get_client(domain_, fragment_, properties_);

  has_connected_.store(MqttFactory::get().is_client_connected(domain_, fragment_, properties_),
                       std::memory_order_release);
}

MqttClient::~MqttClient() {
  MqttFactory::get().unsubscribe_topic(this, domain_, fragment_, properties_, resp_topic_);
  MqttFactory::get().unregister_connection_callback(this);
}

std::any MqttClient::get_native_handle() const { return this; }

void MqttClient::start_listening() {
  bool expected = false;

  if VUNLIKELY (!has_listening_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                        std::memory_order_relaxed)) {
    return;
  }

  MqttFactory::get().subscribe_topic(
      this, domain_, fragment_, properties_, resp_topic_, qos_,
      [weak_self = weak_from_this()](const std::string& topic, const uint8_t* data, size_t size) {
        (void)topic;

        auto self = weak_self.lock();

        if VUNLIKELY (!self) {
          return;
        }

        MqttHeader header;

        if VUNLIKELY (!MqttFactory::decode_header(header, data, size)) {
          VLOG_E("MqttFactory: Failed to decode client response header.");
          return;
        }

        Bytes resp_bytes = Bytes::shallow_copy(data + kMqttHeaderSize, size - kMqttHeaderSize);

        ResponseCallback response;

        {
          std::lock_guard lock(self->mtx_);

          auto iter = self->callbacks_.find(header.seq);

          if (iter != self->callbacks_.end()) {
            response = std::move(iter->second);
            self->callbacks_.erase(iter);
          }
        }

        if VUNLIKELY (!response.callback) {
          return;
        }

        if (!response.dispatch) {
          response.callback(header.channel, resp_bytes);

          return;
        }

        MessageLoop* loop = nullptr;
        NodeImpl::LoopGatePtr loop_gate;

        self->invoke_callback(response.owner, [&] {
          loop = response.owner->get_message_loop();

          if (loop) {
            loop_gate = response.owner->get_loop_gate();
          } else {
            response.callback(header.channel, resp_bytes);
          }
        });

        if (loop) {
          NodeImpl::post_to_loop_gate(loop_gate, loop,
                                      [weak_self, owner = response.owner, loop, callback = std::move(response.callback),
                                       channel = header.channel, resp_bytes]() {
                                        auto self = weak_self.lock();

                                        if VUNLIKELY (!self) {
                                          return;
                                        }

                                        bool attached = false;
                                        self->invoke_callback(owner,
                                                              [&]() { attached = owner->get_message_loop() == loop; });

                                        if VLIKELY (attached) {
                                          callback(channel, resp_bytes);
                                        }
                                      });
        }
      });
}

void MqttClient::stop_listening() {
  bool expected = true;

  if VUNLIKELY (!has_listening_.compare_exchange_strong(expected, false, std::memory_order_acq_rel,
                                                        std::memory_order_relaxed)) {
    return;
  }

  MqttFactory::get().unsubscribe_topic(this, domain_, fragment_, properties_, resp_topic_);
}

void MqttClient::enable_connection_notifications() {
  MqttFactory::get().register_connection_callback(
      this, domain_, fragment_, properties_, [weak_self = weak_from_this()](bool connected) {
        auto self = weak_self.lock();

        if VUNLIKELY (!self) {
          return;
        }

        self->has_connected_.store(connected, std::memory_order_release);
        std::vector<std::pair<LoopRoute, MoveFunction<void()>>> pending;
        self->traverse_server_connect_callback([&](NodeImpl* owner, const auto& callback) {
          auto* loop = owner->get_message_loop();

          if (!loop) {
            callback(connected);
            return;
          }

          pending.emplace_back(LoopRoute{loop, owner->get_loop_gate()}, [weak_self, owner, loop, callback, connected] {
            auto object = weak_self.lock();

            if (object) {
              bool attached = false;
              object->invoke_callback(owner, [&] { attached = owner->get_message_loop() == loop; });

              if (attached) {
                callback(connected);
              }
            }
          });
        });

        for (auto& [route, task] : pending) {
          NodeImpl::post_to_loop_gate(route.gate, route.loop, std::move(task));
        }
      });
}

bool MqttClient::is_connected() const {
  return MqttFactory::get().is_client_connected(domain_, fragment_, properties_);
}

bool MqttClient::call(NodeImpl* owner, uint64_t channel, const Bytes& req_data, NodeImpl::MsgCallback&& callback,
                      uint64_t* seq_out, bool dispatch) {
  auto& factory = MqttFactory::get();

  if VUNLIKELY (req_data.size() > static_cast<size_t>(std::numeric_limits<int>::max()) - kMqttHeaderSize) {
    VLOG_E("MqttFactory: Client request payload is too large, size=", req_data.size(), ".");
    return false;
  }

  uint64_t seq_guid = Helpers::hash_combine(guid_, seq_.fetch_add(1, std::memory_order_relaxed) + 1);

  MqttHeader header{guid_, 0, channel, seq_guid};

  const bool has_callback = static_cast<bool>(callback);

  if VLIKELY (has_callback) {
    std::lock_guard lock(mtx_);

    if (seq_out) {
      *seq_out = seq_guid;
    }

    callbacks_[seq_guid] = ResponseCallback{
        owner, dispatch, [callback = std::move(callback), channel](uint64_t target_channel, const Bytes& bytes) {
          if (channel != target_channel) {
            return;
          }

          callback(bytes);
        }};
  }

  bool published = publish_mqtt_payload(factory, domain_, fragment_, properties_, topic_, qos_, header, req_data);

  if VUNLIKELY (!published) {
    if VLIKELY (has_callback) {
      std::lock_guard lock(mtx_);
      callbacks_.erase(seq_guid);
    }

    return false;
  }

  return true;
}

void MqttClient::remove_response_callback(uint64_t seq) {
  std::lock_guard lock(mtx_);
  callbacks_.erase(seq);
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
