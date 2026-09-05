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

#include "./foxglove_server.h"

//
#include <vlink/base/elapsed_timer.h>
#include <vlink/base/utils.h>
#include <vlink/version.h>

//
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

//
#include "../../webviz_loader_utils.h"
#include "./foxglove_protocol.h"
#include "./webviz_app_utils.h"
#include "./webviz_bridge_utils.h"
#include "./webviz_time_utils.h"

namespace vlink {
namespace webviz {

static constexpr size_t kMaxTaskDepth = 10000U;
static constexpr size_t kMaxClientSendBufferSize = 64U * 1024U * 1024U;
static constexpr size_t kMaxClientSendQueueSize = 4096U;

static void close_slow_client(const ConnectionPtr& conn, const websocketpp::lib::error_code& ec) {
  if VLIKELY (!conn || ec != websocketpp::error::make_error_code(websocketpp::error::send_queue_full)) {
    return;
  }

  websocketpp::lib::error_code close_ec;
  conn->close(websocketpp::close::status::policy_violation, "outgoing send buffer limit exceeded", close_ec);
}

FoxgloveServer::FoxgloveServer(const Config& config)
    : MessageLoop(MessageLoop::kNormalType),
      config_(config),
      session_id_(std::to_string(ElapsedTimer::get_sys_timestamp(ElapsedTimer::kNano))) {
  set_name("FoxgloveServer");

  bridge_ = ProxyBridge::create(config.proxy_config, this);

  FoxgloveConverter::Config conv_config;
  conv_config.proto_dir = config.proto_dir;
  conv_config.fbs_dir = config.fbs_dir;
  conv_config.schema_plugin_path = config.schema_plugin_path;
  conv_config.convert_plugin_path = config.convert_plugin_path;
  conv_config.convert_plugin_config = config.convert_plugin_config;
  conv_config.vlink_msgs = config.vlink_msgs;
  foxglove_converter_ = std::make_unique<FoxgloveConverter>(conv_config);

  VlinkConvert::Config vlink_config;
  vlink_config.proto_dir = config.proto_dir;
  vlink_config.fbs_dir = config.fbs_dir;
  vlink_config.schema_plugin_path = config.schema_plugin_path;
  vlink_config.convert_plugin_path = config.convert_plugin_path;
  vlink_config.convert_plugin_config = config.convert_plugin_config;
  vlink_config.foxglove_msgs = config.foxglove_msgs;
  vlink_convert_ = std::make_unique<VlinkConvert>(vlink_config);

  FoxgloveRpc::Config rpc_config;
  rpc_config.rpc_msgs = config.rpc_msgs;
  rpc_config.transport = config.proxy_config.transport;

  if VLIKELY (!config.rpc_msgs.empty()) {
    rpc_ = std::make_unique<FoxgloveRpc>(rpc_config, vlink_convert_.get(), this);
  }

  if VLIKELY (!config_.parameters.url.empty() || !config_.parameters.values.empty()) {
    config_.parameters.transport = config_.proxy_config.transport;
    parameters_ = std::make_unique<FoxgloveParameters>(config_.parameters);
  }

  if VLIKELY (config_.capabilities.publish && vlink_convert_) {
    install_publish_channels();
  }
}

FoxgloveServer::~FoxgloveServer() {
  stop();
  {
    std::lock_guard lifecycle_lock(lifecycle_mtx_);
  }
  wait_for_quit();
  rpc_.reset();
}

bool FoxgloveServer::start() {
  std::unique_lock lifecycle_lock(lifecycle_mtx_);
  if VUNLIKELY (!foxglove_converter_->valid()) {
    MLOG_E("Invalid Foxglove mapping configuration");
    return false;
  }

  if VUNLIKELY (running_.exchange(true)) {
    return true;
  }

  if VUNLIKELY (!async_run()) {
    running_.store(false);
    return false;
  }

  if (config_.proxy_config.interface_mode == ProxyBridge::kProxyApi) {
    set_global_status("proxy-bridge-disconnected", 1,
                      "Waiting for VLink proxy " VLINK_VERSION " on DDS domain " +
                          std::to_string(config_.proxy_config.transport.domain_id));
  }

  if VUNLIKELY (!init_bridge() || (parameters_ && !parameters_->start()) || !init_websocket()) {
    lifecycle_lock.unlock();
    stop();
    return false;
  }

  lifecycle_lock.unlock();
  MLOG_I("Foxglove server started on {}:{}", config_.address, config_.port);
  log_connect_hint();
  ws_server_->run();
  MLOG_I("Foxglove server stopped");
  return true;
}

void FoxgloveServer::stop() {
  if VUNLIKELY (!running_.exchange(false)) {
    return;
  }
  std::lock_guard lifecycle_lock(lifecycle_mtx_);

  reset_bridge_wall_time_state(last_sys_time_ns_, bridge_time_elapsed_);
  reset_bridge_session_time_anchor(session_start_sys_time_ns_);
  {
    std::lock_guard lock(bridge_control_mtx_);
    bridge_control_signature_.clear();
  }

  if VLIKELY (bridge_) {
    bridge_->stop();
  }

  if VLIKELY (parameters_) {
    parameters_->stop();
  }

  if VLIKELY (ws_server_) {
    websocketpp::lib::error_code ec;
    ws_server_->stop_listening(ec);

    std::vector<ConnectionHdl> client_hdls;

    {
      std::unique_lock lock(clients_mtx_);

      for (auto& [ptr, client] : clients_) {
        client_hdls.emplace_back(client.hdl);
      }
    }

    for (const auto& hdl : client_hdls) {
      ws_server_->close(hdl, websocketpp::close::status::going_away, "server shutdown", ec);

      if VUNLIKELY (ec) {
        ec.clear();
      }
    }

    ws_server_->stop();
  }

  {
    std::lock_guard lifecycle_lock(channel_lifecycle_mtx_);
    std::scoped_lock state_lock(clients_mtx_, channels_mtx_, sub_counts_mtx_);

    clients_.clear();
    publish_channels_.clear();
    url_sub_counts_.clear();
    channel_subscribers_.clear();

    for (auto channel_iter = channels_.begin(); channel_iter != channels_.end();) {
      if VUNLIKELY (channel_iter->second.is_control_only) {
        ++channel_iter;
        continue;
      }

      if VLIKELY (!channel_iter->second.url.empty()) {
        streams_.erase(channel_iter->second.url);
      }

      channel_iter = channels_.erase(channel_iter);
    }
  }

  {
    std::unique_lock lock(info_mtx_);
    last_info_map_.clear();
    prev_connection_graph_ = Json{};
  }

  {
    std::unique_lock lock(status_mtx_);
    global_statuses_.clear();
  }

  quit(false);

  if VLIKELY (!is_in_same_thread()) {
    wait_for_quit();
  }
}

size_t FoxgloveServer::get_max_task_count() const { return kMaxTaskDepth; }

bool FoxgloveServer::init_websocket() {
  ws_server_ = std::make_unique<WsServer>();

  ws_server_->set_access_channels(websocketpp::log::alevel::none);
  ws_server_->set_error_channels(websocketpp::log::elevel::none);

  ws_server_->init_asio();
  ws_server_->set_reuse_addr(true);

  ws_server_->set_open_handler([this](ConnectionHdl hdl) { on_ws_open(hdl); });

  ws_server_->set_close_handler([this](ConnectionHdl hdl) { on_ws_close(hdl); });

  ws_server_->set_message_handler([this](ConnectionHdl hdl, MessagePtr msg) { on_ws_message(hdl, msg); });

  ws_server_->set_ping_handler([this](ConnectionHdl hdl, const std::string& payload) -> bool {
    auto conn = ws_server_->get_con_from_hdl(hdl);
    websocketpp::lib::error_code ec;
    conn->pong(payload, ec);
    close_slow_client(conn, ec);
    return false;
  });

  ws_server_->set_validate_handler([this](ConnectionHdl hdl) -> bool {
    auto conn = ws_server_->get_con_from_hdl(hdl);
    const auto& subprotocols = conn->get_requested_subprotocols();

    for (const auto& sp : subprotocols) {
      if VLIKELY (sp == kSubProtocol) {
        conn->select_subprotocol(std::string(kSubProtocol));
        return true;
      }
    }

    MLOG_W("Rejecting client without required subprotocol {}", kSubProtocol);
    return false;
  });

  websocketpp::lib::error_code ec;
  ws_server_->listen(config_.address, std::to_string(config_.port), ec);

  if VUNLIKELY (ec) {
    MLOG_E("WebSocket listen failed on {}:{}: {}", config_.address, config_.port, ec.message());
    ws_server_.reset();  // NOLINT(readability-ambiguous-smartptr-reset-call)
    return false;
  }

  ws_server_->start_accept();
  return true;
}

bool FoxgloveServer::init_bridge() {
  if VUNLIKELY (!bridge_) {
    MLOG_E("Proxy bridge is not initialized");
    return false;
  }

  bridge_->register_connect_callback([this](bool connected) {
    if VUNLIKELY (!running_.load()) {
      return;
    }

    on_bridge_connected(connected);
  });

  bridge_->register_info_callback([this](const std::vector<ProxyAPI::Info>& info_list) {
    if VUNLIKELY (!running_.load()) {
      return;
    }

    on_bridge_info(info_list);
  });

  bridge_->register_data_callback([this](const ProxyAPI::Data& data) {
    if VUNLIKELY (!running_.load()) {
      return;
    }

    on_bridge_data(data);
  });

  bridge_->register_time_callback([this](uint64_t sys_time, uint64_t boot_time) {
    if VUNLIKELY (!running_.load()) {
      return;
    }

    on_bridge_time(sys_time, boot_time);
  });

  bridge_->register_error_callback([this](ProxyAPI::Error error) {
    if VUNLIKELY (!running_.load()) {
      return;
    }

    log_proxy_bridge_error(*bridge_, error);

    if VLIKELY (error == ProxyAPI::kNoError) {
      clear_global_status("proxy-bridge-error");
      return;
    }

    auto message = proxy_bridge_error_message(*bridge_, error);

    if VLIKELY (!message.empty()) {
      set_global_status("proxy-bridge-error", 2, message);
    }
  });

  if VUNLIKELY (!bridge_->start()) {
    MLOG_E("Failed to start proxy bridge in {} mode", ProxyBridge::to_string(config_.proxy_config.interface_mode));
    return false;
  }

  return true;
}

bool FoxgloveServer::get_json_u32(const Json& value, uint32_t& out) {
  if VUNLIKELY (!value.is_number_unsigned() && !value.is_number_integer()) {
    return false;
  }

  if VLIKELY (value.is_number_unsigned()) {
    auto raw = value.get<uint64_t>();

    if VUNLIKELY (raw > std::numeric_limits<uint32_t>::max()) {
      return false;
    }

    out = static_cast<uint32_t>(raw);
    return true;
  }

  auto raw = value.get<int64_t>();

  if VUNLIKELY (raw < 0 || raw > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
    return false;
  }

  out = static_cast<uint32_t>(raw);
  return true;
}

Json FoxgloveServer::make_advertise_channel_json(const ChannelInfo& channel_info) {
  Json channel;
  channel["id"] = channel_info.id;
  channel["topic"] = channel_info.topic;
  channel["encoding"] = channel_info.encoding;
  channel["schemaName"] = channel_info.schema_name;
  channel["schemaEncoding"] = channel_info.schema_encoding;
  channel["schema"] = channel_info.schema_base64.empty() ? channel_info.schema : channel_info.schema_base64;
  return channel;
}

void FoxgloveServer::update_channel_schema_payload(ChannelInfo& channel_info) {
  if VLIKELY (channel_info.schema_encoding == "protobuf" || is_flatbuffers_encoding(channel_info.schema_encoding)) {
    channel_info.schema_base64 = encode_base64(channel_info.schema.data(), channel_info.schema.size());
  } else {
    channel_info.schema_base64.clear();
  }
}

bool FoxgloveServer::is_binary_schema_encoding(std::string_view schema_encoding) {
  return schema_encoding == "protobuf" || is_flatbuffers_encoding(schema_encoding);
}

bool FoxgloveServer::schemas_match(std::string_view provided_schema, std::string_view expected_schema,
                                   std::string_view schema_encoding) {
  if VUNLIKELY (is_binary_schema_encoding(schema_encoding)) {
    return provided_schema == encode_base64(expected_schema.data(), expected_schema.size());
  }

  if VUNLIKELY (schema_encoding == "jsonschema" || schema_encoding == "json") {
    try {
      return Json::parse(provided_schema) == Json::parse(expected_schema);
    } catch (const std::exception&) {
    }
  }

  return provided_schema == expected_schema;
}

bool FoxgloveServer::parse_parameter_names(const Json& msg, std::vector<std::string>& out, std::string& error) {
  out.clear();

  if VUNLIKELY (!msg.contains("parameterNames") || !msg["parameterNames"].is_array()) {
    error = "parameterNames must be an array";
    return false;
  }

  std::unordered_set<std::string> seen;

  for (const auto& item : msg["parameterNames"]) {
    if VUNLIKELY (!item.is_string()) {
      error = "parameterNames entries must be strings";
      return false;
    }

    auto name = item.get<std::string>();

    if VUNLIKELY (name.empty()) {
      error = "parameterNames entries must not be empty";
      return false;
    }

    if VLIKELY (seen.insert(name).second) {
      out.emplace_back(std::move(name));
    }
  }

  return true;
}

Json FoxgloveServer::build_sorted_connection_entries(std::unordered_map<std::string, std::vector<std::string>>& groups,
                                                     const char* value_key) {
  std::vector<std::pair<std::string, std::vector<std::string>>> ordered_entries;
  ordered_entries.reserve(groups.size());

  for (auto& [name, values] : groups) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    ordered_entries.emplace_back(name, std::move(values));
  }

  std::sort(ordered_entries.begin(), ordered_entries.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

  Json entries = Json::array();

  for (auto& [name, values] : ordered_entries) {
    Json entry;
    entry["name"] = std::move(name);
    entry[value_key] = std::move(values);
    entries.emplace_back(std::move(entry));
  }

  return entries;
}

void FoxgloveServer::on_ws_open(ConnectionHdl hdl) {
  std::lock_guard lifecycle_lock(channel_lifecycle_mtx_);
  if (!running_.load()) {
    return;
  }
  auto conn = ws_server_->get_con_from_hdl(hdl);
  conn->set_max_send_queue_size(kMaxClientSendQueueSize);
  conn->set_max_send_buffer_size(kMaxClientSendBufferSize);
  auto* raw_ptr = conn.get();

  {
    std::unique_lock lock(clients_mtx_);
    ClientInfo client;
    client.id = next_client_id_.fetch_add(1);
    client.hdl = hdl;
    client.conn = conn;
    client.name = conn->get_remote_endpoint();
    clients_[raw_ptr] = std::move(client);
  }

  send_server_info(hdl);
  send_active_statuses(hdl);
  send_advertise(hdl);
  send_advertise_rpcs(hdl);
}

void FoxgloveServer::on_ws_close(ConnectionHdl hdl) {
  auto conn = ws_server_->get_con_from_hdl(hdl);
  auto* raw_ptr = conn.get();

  std::string client_name;
  uint64_t client_id = 0;
  bool need_update = false;

  {
    std::unique_lock lock(clients_mtx_);
    auto client_iter = clients_.find(raw_ptr);

    if VLIKELY (client_iter != clients_.end()) {
      client_id = client_iter->second.id;
      client_name = client_iter->second.name;

      if VLIKELY (!client_iter->second.subscription_map.empty()) {
        std::scoped_lock state_lock(channels_mtx_, sub_counts_mtx_);

        for (const auto& [sub_id, ch_id] : client_iter->second.subscription_map) {
          auto channel_iter = channels_.find(ch_id);

          if VUNLIKELY (channel_iter == channels_.end()) {
            continue;
          }

          if VUNLIKELY (channel_iter->second.is_control_only) {
            auto subscriber_iter = channel_subscribers_.find(ch_id);

            if VLIKELY (subscriber_iter != channel_subscribers_.end()) {
              auto& subs = subscriber_iter->second;
              subs.erase(std::remove_if(subs.begin(), subs.end(),
                                        [raw_ptr](const ChannelSubscriber& s) { return s.client_ptr == raw_ptr; }),
                         subs.end());

              if VUNLIKELY (subs.empty()) {
                channel_subscribers_.erase(subscriber_iter);
              }
            }

            continue;
          }

          auto sub_count_iter = url_sub_counts_.find(channel_iter->second.url);

          if VUNLIKELY (sub_count_iter == url_sub_counts_.end()) {
            continue;
          }

          if VLIKELY (sub_count_iter->second <= 1) {
            url_sub_counts_.erase(sub_count_iter);
            need_update = true;
          } else {
            --sub_count_iter->second;
          }

          auto subscriber_iter = channel_subscribers_.find(ch_id);

          if VLIKELY (subscriber_iter != channel_subscribers_.end()) {
            auto& subs = subscriber_iter->second;
            subs.erase(std::remove_if(subs.begin(), subs.end(),
                                      [raw_ptr](const ChannelSubscriber& s) { return s.client_ptr == raw_ptr; }),
                       subs.end());

            if VLIKELY (subs.empty()) {
              channel_subscribers_.erase(subscriber_iter);
            }
          }
        }
      }

      clients_.erase(client_iter);
    }
  }

  {
    std::unique_lock ch_lock(channels_mtx_);
    auto channel_iter = publish_channels_.find(raw_ptr);

    if VUNLIKELY (channel_iter != publish_channels_.end()) {
      if VLIKELY (!channel_iter->second.empty()) {
        need_update = true;
      }

      publish_channels_.erase(channel_iter);
    }
  }

  if VLIKELY (rpc_ && client_id != 0) {
    rpc_->cancel_client(client_id);
  }

  if VUNLIKELY (need_update) {
    update_bridge_control();
  }
}

void FoxgloveServer::on_ws_message(ConnectionHdl hdl, MessagePtr msg) {
  if VLIKELY (msg->get_opcode() == websocketpp::frame::opcode::text) {
    handle_json_message(hdl, msg->get_payload());
  } else if VLIKELY (msg->get_opcode() == websocketpp::frame::opcode::binary) {
    handle_binary_message(hdl, msg->get_payload());
  }
}

void FoxgloveServer::handle_json_message(ConnectionHdl hdl, const std::string& payload) {
  Json msg;

  try {
    msg = Json::parse(payload);
  } catch (const std::exception& e) {
    MLOG_W("Invalid JSON message: {}", e.what());
    return;
  }

  if VUNLIKELY (!msg.is_object()) {
    MLOG_W("Invalid JSON message: root must be an object");
    return;
  }

  std::string op;

  try {
    op = msg.value("op", std::string());

    if VLIKELY (op == "subscribe") {
      handle_subscribe(hdl, msg);
    } else if VLIKELY (op == "unsubscribe") {
      handle_unsubscribe(hdl, msg);
    } else if VUNLIKELY (op == "advertise") {
      handle_publish_advertise(hdl, msg);
    } else if VUNLIKELY (op == "unadvertise") {
      handle_publish_unadvertise(hdl, msg);
    } else if VUNLIKELY (op == "subscribeConnectionGraph") {
      handle_subscribe_connection_graph(hdl);
    } else if VUNLIKELY (op == "unsubscribeConnectionGraph") {
      handle_unsubscribe_connection_graph(hdl);
    } else if VUNLIKELY (op == "getParameters") {
      handle_get_parameters(hdl, msg);
    } else if VUNLIKELY (op == "setParameters") {
      handle_set_parameters(hdl, msg);
    } else if VUNLIKELY (op == "subscribeParameterUpdates") {
      handle_subscribe_parameter_updates(hdl, msg);
    } else if VUNLIKELY (op == "unsubscribeParameterUpdates") {
      handle_unsubscribe_parameter_updates(hdl, msg);
    } else if VUNLIKELY (op == "fetchAsset") {
      handle_fetch_asset(hdl, msg);
    } else {
      MLOG_W("Unknown op: {}", op);
    }
  } catch (const std::exception& e) {
    MLOG_W("Invalid JSON message for op '{}': {}", op, e.what());
  }
}

void FoxgloveServer::handle_binary_message(ConnectionHdl hdl, const std::string& payload) {
  if VUNLIKELY (payload.empty()) {
    return;
  }

  auto opcode = static_cast<ClientBinaryOpcode>(static_cast<uint8_t>(payload[0]));

  if VLIKELY (opcode == ClientBinaryOpcode::kMessageData) {
    handle_publish_message(hdl, payload);
  } else if VLIKELY (opcode == ClientBinaryOpcode::kServiceCallRequest) {
    handle_rpc_call_request(hdl, payload);
  }
}

void FoxgloveServer::handle_subscribe(ConnectionHdl hdl, const Json& msg) {
  if VUNLIKELY (!msg.contains("subscriptions") || !msg["subscriptions"].is_array()) {
    return;
  }

  bool need_update = false;
  std::vector<std::pair<int, std::string>> pending_statuses;

  {
    std::scoped_lock state_lock(clients_mtx_, channels_mtx_, sub_counts_mtx_);
    void* raw_ptr = nullptr;
    auto* client = find_client_unlocked(hdl, &raw_ptr);

    if VUNLIKELY (!client) {
      return;
    }

    std::unordered_set<uint32_t> subscribed_channels;
    subscribed_channels.reserve(client->subscription_map.size() + msg["subscriptions"].size());

    for (const auto& subscription : client->subscription_map) {
      subscribed_channels.emplace(subscription.second);
    }

    for (const auto& sub : msg["subscriptions"]) {
      if VUNLIKELY (!sub.is_object()) {
        continue;
      }

      if VUNLIKELY (!sub.contains("id") || !sub.contains("channelId")) {
        continue;
      }

      uint32_t sub_id = 0;
      uint32_t channel_id = 0;

      if VUNLIKELY (!get_json_u32(sub["id"], sub_id) || !get_json_u32(sub["channelId"], channel_id)) {
        continue;
      }

      auto old_sub_iter = client->subscription_map.find(sub_id);

      if VUNLIKELY (old_sub_iter != client->subscription_map.end()) {
        MLOG_W("Ignoring reuse of active subscription id: {}", sub_id);
        pending_statuses.emplace_back(
            2, "Subscription ID was already used: " + std::to_string(sub_id) + "; ignoring subscription");
        continue;
      }

      if VUNLIKELY (subscribed_channels.count(channel_id) != 0U) {
        MLOG_W("Ignoring duplicate subscription to channel id: {}", channel_id);
        pending_statuses.emplace_back(
            1, "Client is already subscribed to channel: " + std::to_string(channel_id) + "; ignoring subscription");
        continue;
      }

      auto channel_iter = channels_.find(channel_id);

      if VUNLIKELY (channel_iter == channels_.end() || channel_iter->second.schema_name.empty() ||
                    channel_iter->second.is_time_only) {
        MLOG_W("Subscribe to unknown channel_id: {}", channel_id);
        continue;
      }

      if VUNLIKELY (channel_iter->second.is_control_only) {
        MLOG_W("Ignoring subscribe to publish-only control channel: {}", channel_iter->second.topic);
        continue;
      }

      client->subscription_map[sub_id] = channel_id;
      subscribed_channels.emplace(channel_id);
      channel_subscribers_[channel_id].push_back({raw_ptr, sub_id});

      auto& count = url_sub_counts_[channel_iter->second.url];

      if VLIKELY (count == 0) {
        need_update = true;
      }

      ++count;
    }
  }

  if VLIKELY (need_update) {
    update_bridge_control();
  }

  for (const auto& status : pending_statuses) {
    send_status(hdl, status.first, status.second);
  }
}

void FoxgloveServer::handle_unsubscribe(ConnectionHdl hdl, const Json& msg) {
  if VUNLIKELY (!msg.contains("subscriptionIds") || !msg["subscriptionIds"].is_array()) {
    return;
  }

  bool need_update = false;

  {
    std::scoped_lock state_lock(clients_mtx_, channels_mtx_, sub_counts_mtx_);
    void* raw_ptr = nullptr;
    auto* client = find_client_unlocked(hdl, &raw_ptr);

    if VUNLIKELY (!client) {
      return;
    }

    for (const auto& id : msg["subscriptionIds"]) {
      uint32_t sub_id = 0;

      if VUNLIKELY (!get_json_u32(id, sub_id)) {
        continue;
      }

      auto sub_iter = client->subscription_map.find(sub_id);

      if VUNLIKELY (sub_iter == client->subscription_map.end()) {
        continue;
      }

      auto ch_id = sub_iter->second;
      auto channel_iter = channels_.find(ch_id);

      if VLIKELY (channel_iter != channels_.end()) {
        if VLIKELY (!channel_iter->second.is_control_only) {
          auto sub_count_iter = url_sub_counts_.find(channel_iter->second.url);

          if VLIKELY (sub_count_iter != url_sub_counts_.end()) {
            if VLIKELY (sub_count_iter->second <= 1) {
              url_sub_counts_.erase(sub_count_iter);
              need_update = true;
            } else {
              --sub_count_iter->second;
            }
          }
        }
      }

      auto subscriber_iter = channel_subscribers_.find(ch_id);

      if VLIKELY (subscriber_iter != channel_subscribers_.end()) {
        auto& subs = subscriber_iter->second;
        subs.erase(std::remove_if(subs.begin(), subs.end(),
                                  [raw_ptr, sub_id](const ChannelSubscriber& s) {
                                    return s.client_ptr == raw_ptr && s.subscription_id == sub_id;
                                  }),
                   subs.end());

        if VLIKELY (subs.empty()) {
          channel_subscribers_.erase(subscriber_iter);
        }
      }

      client->subscription_map.erase(sub_iter);
    }
  }

  if VLIKELY (need_update) {
    update_bridge_control();
  }
}

void FoxgloveServer::handle_publish_advertise(ConnectionHdl hdl, const Json& msg) {
  if VUNLIKELY (!config_.capabilities.publish || !bridge_ || !bridge_->can_inject()) {
    MLOG_W("Client publish not enabled, ignoring advertise from client");
    return;
  }

  bool need_update = false;
  std::vector<std::pair<int, std::string>> pending_statuses;
  auto channels = msg.value("channels", Json::array());

  if VUNLIKELY (!channels.is_array()) {
    return;
  }

  {
    std::scoped_lock state_lock(clients_mtx_, channels_mtx_);
    void* raw_ptr = nullptr;

    if VUNLIKELY (!find_client_unlocked(hdl, &raw_ptr) || !raw_ptr) {
      return;
    }

    for (const auto& ch : channels) {
      if VUNLIKELY (!ch.is_object()) {
        continue;
      }

      if VUNLIKELY (!ch.contains("topic") || !ch["topic"].is_string()) {
        continue;
      }

      PublishChannel publish_channel;
      publish_channel.topic = ch["topic"].get<std::string>();

      if VLIKELY (ch.contains("encoding") && ch["encoding"].is_string()) {
        publish_channel.encoding = ch["encoding"].get<std::string>();
      }

      if VLIKELY (ch.contains("schemaName") && ch["schemaName"].is_string()) {
        publish_channel.schema_name = ch["schemaName"].get<std::string>();
      }

      if VLIKELY (ch.contains("schemaEncoding") && ch["schemaEncoding"].is_string()) {
        publish_channel.schema_encoding = ch["schemaEncoding"].get<std::string>();
      }

      if VLIKELY (ch.contains("schema")) {
        if VLIKELY (ch["schema"].is_string()) {
          publish_channel.schema = ch["schema"].get<std::string>();
        } else {
          publish_channel.schema = ch["schema"].dump();
        }
      }

      if VUNLIKELY (publish_channel.topic.empty()) {
        continue;
      }

      if VUNLIKELY (!ch.contains("id")) {
        MLOG_W("Frontend publish channel advertised without valid numeric id for topic: {}", publish_channel.topic);
        continue;
      }

      uint32_t id = 0;

      if VUNLIKELY (!get_json_u32(ch["id"], id)) {
        MLOG_W("Frontend publish channel advertised without valid numeric id for topic: {}", publish_channel.topic);
        continue;
      }

      CommandChannel route_channel;
      route_channel.topic = publish_channel.topic;
      route_channel.encoding = publish_channel.encoding;
      route_channel.schema_name = publish_channel.schema_name;
      route_channel.schema_encoding = publish_channel.schema_encoding;
      route_channel.schema = publish_channel.schema;

      CommandRoute route;

      if VLIKELY (vlink_convert_ && vlink_convert_->resolve_route(route_channel, route)) {
        if VUNLIKELY (publish_channel.encoding.empty()) {
          publish_channel.encoding = route.web_channel.encoding;
        }

        if VUNLIKELY (publish_channel.schema_name.empty()) {
          publish_channel.schema_name = route.web_channel.schema_name;
        }

        if VUNLIKELY (publish_channel.schema_encoding.empty()) {
          publish_channel.schema_encoding = route.web_channel.schema_encoding;
        }

        if VUNLIKELY (publish_channel.schema.empty()) {
          publish_channel.schema = route.web_channel.schema;
        }

        publish_channel.schema_type = SchemaData::resolve_type(
            SchemaData::is_valid_type(route.schema_type) ? route.schema_type : SchemaType::kUnknown, route.ser);
        publish_channel.has_route = true;
        publish_channel.route = std::move(route);

        std::string route_error;

        if VLIKELY (validate_publish_route_unlocked(raw_ptr, id, publish_channel.topic, publish_channel.schema_name,
                                                    publish_channel.schema_encoding, publish_channel.schema,
                                                    publish_channel.route, route_error)) {
          need_update = true;
        } else {
          publish_channel.has_route = false;
          publish_channel.route = CommandRoute{};

          if VUNLIKELY (!route_error.empty()) {
            pending_statuses.emplace_back(2, route_error);
          }
        }
      } else {
        pending_statuses.emplace_back(1, "Client publish channel did not match any route: " + publish_channel.topic);
      }

      auto existing_iter = publish_channels_[raw_ptr].find(id);

      if VUNLIKELY (existing_iter != publish_channels_[raw_ptr].end() && existing_iter->second.has_route) {
        need_update = true;
      }

      publish_channels_[raw_ptr][id] = std::move(publish_channel);
    }
  }

  for (const auto& status : pending_statuses) {
    send_status(hdl, status.first, status.second);
  }

  if VLIKELY (need_update) {
    update_bridge_control();
  }
}

void FoxgloveServer::handle_publish_unadvertise(ConnectionHdl hdl, const Json& msg) {
  auto channel_ids = msg.value("channelIds", Json::array());
  bool need_update = false;

  if VUNLIKELY (!channel_ids.is_array()) {
    return;
  }

  {
    std::scoped_lock state_lock(clients_mtx_, channels_mtx_);
    void* raw_ptr = nullptr;

    if VUNLIKELY (!find_client_unlocked(hdl, &raw_ptr) || !raw_ptr) {
      return;
    }

    auto channel_map_iter = publish_channels_.find(raw_ptr);

    if VUNLIKELY (channel_map_iter == publish_channels_.end()) {
      return;
    }

    auto& channel_map = channel_map_iter->second;

    for (const auto& id_json : channel_ids) {
      uint32_t id = 0;

      if VUNLIKELY (!get_json_u32(id_json, id)) {
        continue;
      }

      auto channel_iter = channel_map.find(id);

      if VLIKELY (channel_iter != channel_map.end()) {
        if VLIKELY (channel_iter->second.has_route) {
          need_update = true;
        }

        channel_map.erase(channel_iter);
      }
    }

    if VUNLIKELY (channel_map.empty()) {
      publish_channels_.erase(channel_map_iter);
    }
  }

  if VLIKELY (need_update) {
    update_bridge_control();
  }
}

void FoxgloveServer::handle_publish_message(ConnectionHdl hdl, const std::string& payload) {
  if VUNLIKELY (!config_.capabilities.publish || !bridge_ || !bridge_->can_inject()) {
    return;
  }

  ClientBinaryMessage bin_msg;

  if VUNLIKELY (!parse_client_binary(reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), bin_msg)) {
    return;
  }

  CommandRoute route;
  auto route_schema_type = SchemaType::kUnknown;
  std::string status_message;
  bool lookup_failed = false;

  {
    std::scoped_lock state_lock(clients_mtx_, channels_mtx_);
    void* raw_ptr = nullptr;

    if VUNLIKELY (!find_client_unlocked(hdl, &raw_ptr) || !raw_ptr) {
      return;
    }

    auto channel_map_iter = publish_channels_.find(raw_ptr);

    if VLIKELY (channel_map_iter != publish_channels_.end()) {
      auto publish_iter = channel_map_iter->second.find(bin_msg.channel_or_service_id);

      if VLIKELY (publish_iter != channel_map_iter->second.end()) {
        if VUNLIKELY (!publish_iter->second.has_route) {
          MLOG_W("Client channel {} has no publish route: {}", bin_msg.channel_or_service_id,
                 publish_iter->second.topic);
          status_message = "Client channel has no active publish route: " + publish_iter->second.topic;
          lookup_failed = true;
        } else {
          route = publish_iter->second.route;
          route_schema_type = SchemaData::is_valid_type(route.schema_type) ? route.schema_type : SchemaType::kUnknown;

          if VUNLIKELY (route_schema_type == SchemaType::kUnknown) {
            route_schema_type = publish_iter->second.schema_type;
          }
        }
      }
    }

    if VUNLIKELY (!lookup_failed && (route.url.empty() || route.ser.empty())) {
      MLOG_W("Client message for unknown channel: {}", bin_msg.channel_or_service_id);
      status_message = "Client message references an unknown publish channel";
      lookup_failed = true;
    }
  }

  if VUNLIKELY (!lookup_failed && route_schema_type == SchemaType::kUnknown) {
    status_message = "Client channel has no active publish route";
    lookup_failed = true;
  }

  if VUNLIKELY (lookup_failed && !status_message.empty()) {
    send_status(hdl, 2, status_message);
    return;
  }

  auto raw_msg = Bytes::shallow_copy(bin_msg.payload, bin_msg.payload_len);

  auto converted = vlink_convert_ ? vlink_convert_->encode_frontend_message(route, raw_msg) : CommandMessage{};

  if VUNLIKELY (!converted.success) {
    if VLIKELY (running_.load()) {
      MLOG_W("Failed to convert client message for topic: {}", route.url);
    }

    send_status(hdl, 2, "Failed to convert client message for topic: " + route.url);
    return;
  }

  ProxyAPI::Data data;
  data.url = converted.url;
  data.ser = converted.ser;
  data.schema = route_schema_type;
  data.raw = std::move(converted.payload);

  if VUNLIKELY (!bridge_ || !bridge_->send_data(data)) {
    if VLIKELY (running_.load()) {
      MLOG_W("Failed to dispatch client message via proxy bridge for topic: {}", data.url);
    }

    send_status(hdl, 2, "Failed to dispatch client message via proxy bridge for topic: " + data.url);
  }
}

void FoxgloveServer::handle_rpc_call_request(ConnectionHdl hdl, const std::string& payload) {
  ClientBinaryMessage bin_msg;

  if VUNLIKELY (!parse_client_binary(reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), bin_msg)) {
    return;
  }

  if VUNLIKELY (!config_.capabilities.rpcs) {
    Json failure;
    failure["op"] = "serviceCallFailure";
    failure["serviceId"] = bin_msg.channel_or_service_id;
    failure["callId"] = bin_msg.call_id;
    failure["message"] = "RPC capability is disabled";
    send_json(hdl, failure);
    return;
  }

  if VUNLIKELY (!rpc_) {
    Json failure;
    failure["op"] = "serviceCallFailure";
    failure["serviceId"] = bin_msg.channel_or_service_id;
    failure["callId"] = bin_msg.call_id;
    failure["message"] = "RPC bridge is not initialized";
    send_json(hdl, failure);
    return;
  }

  if VUNLIKELY (!rpc_->is_rpc_allowed(bin_msg.channel_or_service_id,
                                      [this](std::string_view url) { return is_url_allowed(url); })) {
    Json failure;
    failure["op"] = "serviceCallFailure";
    failure["serviceId"] = bin_msg.channel_or_service_id;
    failure["callId"] = bin_msg.call_id;
    failure["message"] = "RPC service is blocked by filter or unknown";
    send_json(hdl, failure);
    return;
  }

  uint64_t client_key = 0;
  bool client_found = false;

  {
    std::shared_lock lock(clients_mtx_);
    auto* client = find_client_unlocked(hdl);

    if VLIKELY (client) {
      client_key = client->id;
      client_found = true;
    }
  }

  if VUNLIKELY (!client_found) {
    Json failure;
    failure["op"] = "serviceCallFailure";
    failure["serviceId"] = bin_msg.channel_or_service_id;
    failure["callId"] = bin_msg.call_id;
    failure["message"] = "Client session is not registered";
    send_json(hdl, failure);
    return;
  }

  auto request = Bytes::shallow_copy(bin_msg.payload, bin_msg.payload_len);

  rpc_->call_rpc(
      client_key, bin_msg.channel_or_service_id, bin_msg.call_id, bin_msg.encoding, request,
      [this, hdl](uint32_t rpc_id, uint32_t call_id, const std::string& response_encoding,
                  const Bytes& response_payload) {
        auto resp = build_service_call_response(rpc_id, call_id, response_encoding, response_payload.data(),
                                                response_payload.size());
        send_binary(hdl, resp);
      },
      [this, hdl](uint32_t rpc_id, uint32_t call_id, const std::string& message) {
        Json failure;
        failure["op"] = "serviceCallFailure";
        failure["serviceId"] = rpc_id;
        failure["callId"] = call_id;
        failure["message"] = message;
        send_json(hdl, failure);
      });
}

void FoxgloveServer::handle_subscribe_connection_graph(ConnectionHdl hdl) {
  if VUNLIKELY (!config_.capabilities.connection_graph) {
    return;
  }

  {
    std::unique_lock lock(clients_mtx_);
    auto* client = find_client_unlocked(hdl);

    if VLIKELY (client) {
      client->subscribed_connection_graph = true;
    }
  }

  Json msg;

  {
    std::shared_lock lock(info_mtx_);
    msg = build_connection_graph();
  }

  send_json(hdl, msg);
}

void FoxgloveServer::handle_unsubscribe_connection_graph(ConnectionHdl hdl) {
  if VUNLIKELY (!config_.capabilities.connection_graph) {
    return;
  }

  std::unique_lock lock(clients_mtx_);
  auto* client = find_client_unlocked(hdl);

  if VLIKELY (client) {
    client->subscribed_connection_graph = false;
  }
}

void FoxgloveServer::handle_get_parameters(ConnectionHdl hdl, const Json& msg) {
  if VUNLIKELY (!has_parameters_capability() || !parameters_) {
    send_status(hdl, 2, "Parameters capability is disabled");
    return;
  }

  std::vector<std::string> names;
  std::string error;

  if VUNLIKELY (!parse_parameter_names(msg, names, error)) {
    send_status(hdl, 2, error);
    return;
  }

  std::optional<std::string_view> request_id;

  if VUNLIKELY (msg.contains("id") && !msg["id"].is_string()) {
    send_status(hdl, 2, "getParameters id must be a string");
    return;
  }

  if VLIKELY (msg.contains("id")) {
    request_id = msg["id"].get_ref<const std::string&>();
  }

  send_json(hdl, parameters_->build_parameter_values(names, request_id));
}

void FoxgloveServer::handle_set_parameters(ConnectionHdl hdl, const Json& msg) {
  if VUNLIKELY (!has_parameters_capability() || !parameters_) {
    send_status(hdl, 2, "Parameters capability is disabled");
    return;
  }

  if VUNLIKELY (msg.contains("id") && !msg["id"].is_string()) {
    send_status(hdl, 2, "setParameters id must be a string");
    return;
  }

  Json response;
  std::vector<FoxgloveParameters::ParameterEntry> delta;
  std::string error;

  if VUNLIKELY (!parameters_->apply_set_parameters(msg, response, delta, error)) {
    send_status(hdl, 2, error);
    return;
  }

  if VLIKELY (msg.contains("id")) {
    send_json(hdl, response);
  }

  if VLIKELY (!delta.empty()) {
    on_parameters_changed(delta);
  }
}

void FoxgloveServer::handle_subscribe_parameter_updates(ConnectionHdl hdl, const Json& msg) {
  if VUNLIKELY (!has_parameters_capability() || !parameters_) {
    send_status(hdl, 2, "Parameters capability is disabled");
    return;
  }

  std::vector<std::string> names;
  std::string error;

  if VUNLIKELY (!parse_parameter_names(msg, names, error)) {
    send_status(hdl, 2, error);
    return;
  }

  Json initial_values;

  {
    std::unique_lock lock(clients_mtx_);
    auto* client = find_client_unlocked(hdl);

    if VUNLIKELY (!client) {
      return;
    }

    if VUNLIKELY (names.empty()) {
      client->subscribed_all_parameters = true;
      client->parameter_subscriptions.clear();
      client->parameter_exclusions.clear();
      initial_values = parameters_->build_parameter_values({}, {});
    } else if VUNLIKELY (client->subscribed_all_parameters) {
      for (const auto& name : names) {
        client->parameter_exclusions.erase(name);
      }

      initial_values = parameters_->build_parameter_values(names, {});
    } else {
      for (const auto& name : names) {
        client->parameter_subscriptions.emplace(name);
      }

      initial_values = parameters_->build_parameter_values(names, {});
    }
  }

  if VLIKELY (initial_values.contains("parameters") && initial_values["parameters"].is_array()) {
    send_json(hdl, initial_values);
  }
}

void FoxgloveServer::handle_unsubscribe_parameter_updates(ConnectionHdl hdl, const Json& msg) {
  if VUNLIKELY (!has_parameters_capability()) {
    return;
  }

  std::vector<std::string> names;
  std::string error;

  if VUNLIKELY (!parse_parameter_names(msg, names, error)) {
    send_status(hdl, 2, error);
    return;
  }

  std::unique_lock lock(clients_mtx_);
  auto* client = find_client_unlocked(hdl);

  if VUNLIKELY (!client) {
    return;
  }

  if VUNLIKELY (names.empty()) {
    client->subscribed_all_parameters = false;
    client->parameter_subscriptions.clear();
    client->parameter_exclusions.clear();
    return;
  }

  if VUNLIKELY (client->subscribed_all_parameters) {
    for (const auto& name : names) {
      client->parameter_exclusions.emplace(name);
    }

    return;
  }

  for (const auto& name : names) {
    client->parameter_subscriptions.erase(name);
  }

  if VUNLIKELY (client->parameter_subscriptions.empty()) {
    client->subscribed_all_parameters = false;
  }
}

void FoxgloveServer::broadcast_connection_graph_update() {
  if (!config_.capabilities.connection_graph) {
    return;
  }

  Json graph;

  {
    std::unique_lock lock(info_mtx_);
    graph = build_connection_graph();

    if VLIKELY (graph["publishedTopics"] == prev_connection_graph_["publishedTopics"] &&
                graph["subscribedTopics"] == prev_connection_graph_["subscribedTopics"] &&
                graph["advertisedServices"] == prev_connection_graph_["advertisedServices"]) {
      return;
    }

    auto collect_names = [](const Json& graph_json, const char* key, std::unordered_set<std::string>& names) {
      if VUNLIKELY (!graph_json.contains(key) || !graph_json[key].is_array()) {
        return;
      }

      for (const auto& entry : graph_json[key]) {
        auto name = entry.value("name", std::string{});

        if VLIKELY (!name.empty()) {
          names.emplace(std::move(name));
        }
      }
    };

    std::unordered_set<std::string> prev_topic_names;
    std::unordered_set<std::string> current_topic_names;
    std::unordered_set<std::string> prev_service_names;
    std::unordered_set<std::string> current_service_names;

    collect_names(prev_connection_graph_, "publishedTopics", prev_topic_names);
    collect_names(prev_connection_graph_, "subscribedTopics", prev_topic_names);
    collect_names(graph, "publishedTopics", current_topic_names);
    collect_names(graph, "subscribedTopics", current_topic_names);
    collect_names(prev_connection_graph_, "advertisedServices", prev_service_names);
    collect_names(graph, "advertisedServices", current_service_names);

    std::vector<std::string> removed_topics;
    removed_topics.reserve(prev_topic_names.size());

    for (const auto& name : prev_topic_names) {
      if VUNLIKELY (current_topic_names.count(name) == 0U) {
        removed_topics.emplace_back(name);
      }
    }

    std::sort(removed_topics.begin(), removed_topics.end());

    std::vector<std::string> removed_rpcs;
    removed_rpcs.reserve(prev_service_names.size());

    for (const auto& name : prev_service_names) {
      if VUNLIKELY (current_service_names.count(name) == 0U) {
        removed_rpcs.emplace_back(name);
      }
    }

    std::sort(removed_rpcs.begin(), removed_rpcs.end());

    graph["removedTopics"] = std::move(removed_topics);
    graph["removedServices"] = std::move(removed_rpcs);

    prev_connection_graph_ = graph;
  }

  std::string payload;

  try {
    payload = graph.dump();
  } catch (const std::exception& e) {
    if VLIKELY (running_.load()) {
      MLOG_W("Failed to serialize connection graph: {}", e.what());
    }

    return;
  }

  std::vector<ConnectionPtr> targets;

  {
    std::shared_lock lock(clients_mtx_);
    targets.reserve(clients_.size());

    for (auto& client_entry : clients_) {
      auto& client = client_entry.second;

      if VLIKELY (client.subscribed_connection_graph) {
        if VLIKELY (client.conn) {
          targets.emplace_back(client.conn);
        }
      }
    }
  }

  for (const auto& conn : targets) {
    try {
      auto ec = conn->send(payload, websocketpp::frame::opcode::text);

      if VUNLIKELY (ec) {
        close_slow_client(conn, ec);

        if VLIKELY (running_.load()) {
          MLOG_W("Failed to broadcast connection graph: {}", ec.message());
        }
      }
    } catch (const std::exception& e) {
      if VLIKELY (running_.load()) {
        MLOG_W("Failed to broadcast connection graph: {}", e.what());
      }
    }
  }
}

Json FoxgloveServer::build_connection_graph() const {
  auto build_process_id = [](const ProxyAPI::Process& process) {
    if VUNLIKELY (process.host.empty()) {
      return process.name + "(" + std::to_string(process.pid) + ")";
    }

    return process.name + "(" + std::to_string(process.pid) + "@" + process.host + ")";
  };

  std::unordered_map<std::string, std::vector<std::string>> published_topics;
  std::unordered_map<std::string, std::vector<std::string>> subscribed_topics;
  std::unordered_map<std::string, std::vector<std::string>> advertised_rpcs;

  for (const auto& [url, info] : last_info_map_) {
    for (const auto& proc : info.process_list) {
      auto process_id = build_process_id(proc);

      if VLIKELY ((proc.type & kPublisher) != 0U) {
        published_topics[url].emplace_back(process_id);
      }

      if VLIKELY ((proc.type & kSubscriber) != 0U) {
        subscribed_topics[url].emplace_back(process_id);
      }

      if VLIKELY ((proc.type & kServer) != 0U) {
        advertised_rpcs[url].emplace_back(process_id);
      }
    }
  }

  Json msg;
  msg["op"] = "connectionGraphUpdate";
  msg["publishedTopics"] = build_sorted_connection_entries(published_topics, "publisherIds");
  msg["subscribedTopics"] = build_sorted_connection_entries(subscribed_topics, "subscriberIds");
  msg["advertisedServices"] = build_sorted_connection_entries(advertised_rpcs, "providerIds");
  msg["removedTopics"] = Json::array();
  msg["removedServices"] = Json::array();

  return msg;
}

void FoxgloveServer::handle_fetch_asset(ConnectionHdl hdl, const Json& msg) {
  uint32_t request_id = 0;
  std::string uri;

  if VLIKELY (msg.contains("requestId")) {
    get_json_u32(msg["requestId"], request_id);
  }

  if VLIKELY (msg.contains("uri") && msg["uri"].is_string()) {
    uri = msg["uri"].get<std::string>();
  }

  if VUNLIKELY (!config_.capabilities.assets || uri.empty() || config_.asset_dirs.empty()) {
    auto response = build_fetch_asset_response(request_id, 1, "Asset fetching not configured", nullptr, 0);
    send_binary(hdl, response);
    return;
  }

  std::string rel_path = uri;
  auto transport_pos = uri.find("://");

  if VLIKELY (transport_pos != std::string::npos) {
    rel_path = uri.substr(transport_pos + 3);
  }

  for (const auto& dir : config_.asset_dirs) {
    auto full_path = std::filesystem::path(dir) / rel_path;
    std::error_code ec;

    if VUNLIKELY (!std::filesystem::exists(full_path, ec) || ec) {
      continue;
    }

    if VUNLIKELY (!std::filesystem::is_regular_file(full_path, ec) || ec) {
      continue;
    }

    auto canonical_path = std::filesystem::weakly_canonical(full_path, ec);

    if VUNLIKELY (ec) {
      continue;
    }

    auto canonical_dir = std::filesystem::weakly_canonical(std::filesystem::path(dir), ec);

    if VUNLIKELY (ec) {
      continue;
    }

    auto dir_iter = canonical_dir.begin();
    auto path_iter = canonical_path.begin();

    for (; dir_iter != canonical_dir.end() && path_iter != canonical_path.end(); ++dir_iter, ++path_iter) {
      if VUNLIKELY (*dir_iter != *path_iter) {
        break;
      }
    }

    if VUNLIKELY (dir_iter != canonical_dir.end()) {
      MLOG_W("Asset path traversal blocked: {} -> {}", uri, canonical_path.string());
      auto response = build_fetch_asset_response(request_id, 1, "Invalid asset path", nullptr, 0);
      send_binary(hdl, response);
      return;
    }

    auto file_size = std::filesystem::file_size(full_path, ec);

    if VUNLIKELY (ec) {
      MLOG_W("Failed to stat asset {}: {}", full_path.string(), ec.message());
      continue;
    }

    constexpr size_t kMaxAssetSize = 256 * 1024 * 1024;

    if VUNLIKELY (file_size > kMaxAssetSize) {
      auto resp = build_fetch_asset_response(request_id, 1, "Asset too large", nullptr, 0);
      send_binary(hdl, resp);
      return;
    }

    auto file_data = Bytes::create(file_size);
    std::ifstream ifs(full_path, std::ios::binary);

    if VLIKELY (ifs.read(reinterpret_cast<char*>(file_data.data()), static_cast<std::streamsize>(file_size))) {
      auto response = build_fetch_asset_response(request_id, 0, "", file_data.data(), file_data.size());
      send_binary(hdl, response);
      return;
    }
  }

  auto response = build_fetch_asset_response(request_id, 1, "Asset not found: " + uri, nullptr, 0);
  send_binary(hdl, response);
}

void FoxgloveServer::send_server_info(ConnectionHdl hdl) {
  Json info;
  info["op"] = "serverInfo";
  info["name"] = config_.name;

  auto caps = Json::array();

  if VLIKELY (has_time_capability()) {
    caps.emplace_back(kCapabilityTime);
  }

  if VLIKELY (config_.capabilities.connection_graph) {
    caps.emplace_back(kCapabilityConnectionGraph);
  }

  if VLIKELY (config_.capabilities.publish && bridge_ && bridge_->can_inject()) {
    caps.emplace_back(kCapabilityClientPublish);
  }

  if VLIKELY (config_.capabilities.rpcs && rpc_ &&
              rpc_->has_rpcs([this](std::string_view url) { return is_url_allowed(url); })) {
    caps.emplace_back(kCapabilityServices);
  }

  if VLIKELY (has_parameters_capability()) {
    caps.emplace_back(kCapabilityParameters);
    caps.emplace_back(kCapabilityParametersSubscribe);
  }

  if VLIKELY (config_.capabilities.assets && !config_.asset_dirs.empty()) {
    caps.emplace_back(kCapabilityAssets);
  }

  info["capabilities"] = caps;

  if VLIKELY ((config_.capabilities.publish && bridge_ && bridge_->can_inject()) ||
              (config_.capabilities.rpcs && rpc_ &&
               rpc_->has_rpcs([this](std::string_view url) { return is_url_allowed(url); }))) {
    info["supportedEncodings"] = Json::array({"json"});
  }

  info["metadata"] = Json::object();
  info["sessionId"] = session_id_;

  send_json(hdl, info);
}

void FoxgloveServer::send_advertise(ConnectionHdl hdl) {
  Json msg;
  msg["op"] = "advertise";
  msg["channels"] = Json::array();

  {
    std::shared_lock lock(channels_mtx_);

    if VUNLIKELY (channels_.empty()) {
      return;
    }

    for (const auto& [id, ch] : channels_) {
      if VUNLIKELY (ch.is_time_only || ch.schema_name.empty()) {
        continue;
      }

      if VUNLIKELY (ch.is_control_only && (!config_.capabilities.publish || !bridge_ || !bridge_->can_inject())) {
        continue;
      }

      if VUNLIKELY (ch.is_control_only && !is_url_allowed(ch.url)) {
        continue;
      }

      msg["channels"].emplace_back(make_advertise_channel_json(ch));
    }
  }

  if VUNLIKELY (msg["channels"].empty()) {
    return;
  }

  send_json(hdl, msg);
}

void FoxgloveServer::send_advertise_rpcs(ConnectionHdl hdl) {
  if VUNLIKELY (!config_.capabilities.rpcs || !rpc_ ||
                !rpc_->has_rpcs([this](std::string_view url) { return is_url_allowed(url); })) {
    return;
  }

  Json msg;
  msg["op"] = "advertiseServices";
  msg["services"] = Json::array();

  auto repcs = rpc_->get_rpcs([this](std::string_view url) { return is_url_allowed(url); });

  for (const auto& rpc : repcs) {
    msg["services"].emplace_back(rpc);
  }

  if VLIKELY (!msg["services"].empty()) {
    send_json(hdl, msg);
  }
}

void FoxgloveServer::send_json(const ConnectionPtr& conn, const Json& msg) {
  if VUNLIKELY (!conn) {
    return;
  }

  std::string payload;

  try {
    payload = msg.dump();
  } catch (const std::exception& e) {
    if VLIKELY (running_.load()) {
      MLOG_W("Failed to serialize JSON: {}", e.what());
    }

    return;
  }

  try {
    auto ec = conn->send(payload, websocketpp::frame::opcode::text);

    if VUNLIKELY (ec) {
      close_slow_client(conn, ec);

      if VLIKELY (running_.load()) {
        MLOG_W("Failed to send JSON: {}", ec.message());
      }
    }
  } catch (const std::exception& e) {
    if VLIKELY (running_.load()) {
      MLOG_W("Failed to send JSON: {}", e.what());
    }
  }
}

void FoxgloveServer::send_json(ConnectionHdl hdl, const Json& msg) {
  ConnectionPtr conn;

  {
    std::shared_lock lock(clients_mtx_);

    if VUNLIKELY (!ws_server_) {
      return;
    }

    auto* client = find_client_unlocked(hdl);

    if VUNLIKELY (!client) {
      return;
    }

    conn = client->conn;
  }

  send_json(conn, msg);
}

void FoxgloveServer::send_binary(const ConnectionPtr& conn, const Bytes& buf) {
  if VUNLIKELY (!conn) {
    return;
  }

  try {
    auto ec = conn->send(buf.data(), buf.size(), websocketpp::frame::opcode::binary);

    if VUNLIKELY (ec) {
      close_slow_client(conn, ec);

      if VLIKELY (running_.load()) {
        MLOG_W("Failed to send binary: {}", ec.message());
      }
    }
  } catch (const std::exception& e) {
    if VLIKELY (running_.load()) {
      MLOG_W("Failed to send binary: {}", e.what());
    }
  }
}

void FoxgloveServer::send_binary(ConnectionHdl hdl, const Bytes& buf) {
  ConnectionPtr conn;

  {
    std::shared_lock lock(clients_mtx_);

    if VUNLIKELY (!ws_server_) {
      return;
    }

    auto* client = find_client_unlocked(hdl);

    if VUNLIKELY (!client) {
      return;
    }

    conn = client->conn;
  }

  send_binary(conn, buf);
}

void FoxgloveServer::send_status(ConnectionHdl hdl, int level, std::string_view message, std::string_view status_id) {
  Json status;
  status["op"] = "status";
  status["level"] = level;
  status["message"] = std::string(message);

  if VLIKELY (!status_id.empty()) {
    status["id"] = std::string(status_id);
  }

  send_json(hdl, status);
}

void FoxgloveServer::send_remove_status(ConnectionHdl hdl, const std::vector<std::string>& status_ids) {
  if VUNLIKELY (status_ids.empty()) {
    return;
  }

  Json msg;
  msg["op"] = "removeStatus";
  msg["statusIds"] = status_ids;
  send_json(hdl, msg);
}

void FoxgloveServer::send_active_statuses(ConnectionHdl hdl) {
  std::vector<std::pair<std::string, StatusInfo>> statuses;

  {
    std::shared_lock lock(status_mtx_);
    statuses.reserve(global_statuses_.size());

    for (const auto& [id, info] : global_statuses_) {
      statuses.emplace_back(id, info);
    }
  }

  for (const auto& [id, info] : statuses) {
    send_status(hdl, info.level, info.message, id);
  }
}

void FoxgloveServer::set_global_status(std::string_view status_id, int level, std::string_view message) {
  if VUNLIKELY (status_id.empty() || message.empty()) {
    return;
  }

  bool changed = false;

  {
    std::unique_lock lock(status_mtx_);
    auto& slot = global_statuses_[std::string(status_id)];

    if VUNLIKELY (slot.level != level || slot.message != message) {
      slot.level = level;
      slot.message = std::string(message);
      changed = true;
    }
  }

  if VLIKELY (changed) {
    Json status;
    status["op"] = "status";
    status["level"] = level;
    status["message"] = std::string(message);
    status["id"] = std::string(status_id);
    broadcast_json(status);
  }
}

void FoxgloveServer::clear_global_status(std::string_view status_id) {
  if VUNLIKELY (status_id.empty()) {
    return;
  }

  bool existed = false;

  {
    std::unique_lock lock(status_mtx_);
    existed = global_statuses_.erase(std::string(status_id)) > 0U;
  }

  if VLIKELY (existed) {
    broadcast_json(Json{{"op", "removeStatus"}, {"statusIds", Json::array({std::string(status_id)})}});
  }
}

bool FoxgloveServer::has_send_time_source() {
  if VLIKELY (foxglove_converter_ && foxglove_converter_->has_send_time_mapping()) {
    return true;
  }

  std::shared_lock lock(channels_mtx_);

  for (const auto& channel_entry : channels_) {
    const auto& ch = channel_entry.second;

    if VUNLIKELY (ch.is_send_time) {
      return true;
    }
  }

  return false;
}

bool FoxgloveServer::has_time_capability() {
  return config_.capabilities.time || config_.send_time || has_send_time_source();
}

bool FoxgloveServer::has_parameters_capability() const { return parameters_ && parameters_->active(); }

void FoxgloveServer::send_time(uint64_t timestamp_ns) {
  std::vector<ConnectionPtr> targets;

  {
    std::shared_lock lock(clients_mtx_);

    if VUNLIKELY (clients_.empty()) {
      return;
    }

    targets.reserve(clients_.size());

    for (auto& client_entry : clients_) {
      auto& client = client_entry.second;

      if VLIKELY (client.conn) {
        targets.emplace_back(client.conn);
      }
    }
  }

  if VUNLIKELY (targets.empty()) {
    return;
  }

  auto payload = build_time_message(timestamp_ns);

  for (const auto& conn : targets) {
    send_binary(conn, payload);
  }
}

void FoxgloveServer::broadcast_json(const Json& msg) {
  std::string payload;

  try {
    payload = msg.dump();
  } catch (const std::exception& e) {
    if VLIKELY (running_.load()) {
      MLOG_W("Failed to serialize JSON: {}", e.what());
    }

    return;
  }

  std::vector<ConnectionPtr> targets;

  {
    std::shared_lock lock(clients_mtx_);
    targets.reserve(clients_.size());

    for (auto& client_entry : clients_) {
      auto& client = client_entry.second;

      if VLIKELY (client.conn) {
        targets.emplace_back(client.conn);
      }
    }
  }

  for (const auto& conn : targets) {
    try {
      auto ec = conn->send(payload, websocketpp::frame::opcode::text);

      if VUNLIKELY (ec) {
        close_slow_client(conn, ec);

        if VLIKELY (running_.load()) {
          MLOG_W("Failed to broadcast JSON: {}", ec.message());
        }
      }
    } catch (const std::exception& e) {
      if VLIKELY (running_.load()) {
        MLOG_W("Failed to broadcast JSON: {}", e.what());
      }
    }
  }
}

void FoxgloveServer::on_parameters_changed(const std::vector<FoxgloveParameters::ParameterEntry>& delta) {
  if VUNLIKELY (!has_parameters_capability() || !parameters_ || delta.empty()) {
    return;
  }

  std::vector<std::pair<ConnectionPtr, Json>> pending;

  {
    std::shared_lock lock(clients_mtx_);
    std::vector<const FoxgloveParameters::ParameterEntry*> matched_delta;
    matched_delta.reserve(delta.size());

    for (auto& client_entry : clients_) {
      auto& client = client_entry.second;

      if VUNLIKELY (!client.subscribed_all_parameters && client.parameter_subscriptions.empty()) {
        continue;
      }

      matched_delta.clear();

      for (const auto& entry : delta) {
        if VLIKELY (client.subscribed_all_parameters) {
          if VLIKELY (client.parameter_exclusions.count(entry.name) == 0U) {
            matched_delta.emplace_back(&entry);
          }
        } else if VLIKELY (client.parameter_subscriptions.count(entry.name) > 0U) {
          matched_delta.emplace_back(&entry);
        }
      }

      if VLIKELY (!matched_delta.empty()) {
        pending.emplace_back(client.conn, FoxgloveParameters::build_parameter_delta(matched_delta));
      }
    }
  }

  for (auto& [conn, msg] : pending) {
    send_json(conn, msg);
  }
}

void FoxgloveServer::on_bridge_connected(bool connected) {
  if VUNLIKELY (!running_.load()) {
    return;
  }

  if VLIKELY (connected) {
    MLOG_I("Connected to proxy bridge in {} mode", ProxyBridge::to_string(config_.proxy_config.interface_mode));
    clear_global_status("proxy-bridge-disconnected");
    update_bridge_control();
  } else {
    std::unique_lock lifecycle_lock(channel_lifecycle_mtx_);
    if (!running_.load()) {
      return;
    }
    MLOG_W("Disconnected from proxy bridge");
    set_global_status("proxy-bridge-disconnected", 2, "Proxy bridge disconnected");

    Json unadv_msg;
    bool has_channels = false;

    {
      std::scoped_lock state_lock(clients_mtx_, channels_mtx_, sub_counts_mtx_);

      if VLIKELY (!channels_.empty()) {
        unadv_msg["op"] = "unadvertise";
        unadv_msg["channelIds"] = Json::array();

        std::vector<uint32_t> remove_ids;
        remove_ids.reserve(channels_.size());

        for (const auto& [id, ch] : channels_) {
          if VUNLIKELY (ch.is_control_only) {
            continue;
          }

          if VLIKELY (!ch.is_time_only && !ch.schema_name.empty()) {
            has_channels = true;
            unadv_msg["channelIds"].emplace_back(id);
          }

          remove_ids.emplace_back(id);
        }

        for (const auto channel_id : remove_ids) {
          auto channel_iter = channels_.find(channel_id);

          if VUNLIKELY (channel_iter == channels_.end()) {
            continue;
          }

          if VLIKELY (!channel_iter->second.url.empty()) {
            streams_.erase(channel_iter->second.url);
          }

          channels_.erase(channel_iter);
        }
      }

      for (auto& [ptr, client] : clients_) {
        client.subscription_map.clear();
      }

      url_sub_counts_.clear();
      channel_subscribers_.clear();
    }

    if VLIKELY (has_channels) {
      broadcast_json(unadv_msg);
    }

    {
      std::unique_lock lock(info_mtx_);
      last_info_map_.clear();
    }

    broadcast_connection_graph_update();

    reset_bridge_wall_time_state(last_sys_time_ns_, bridge_time_elapsed_);
    reset_bridge_session_time_anchor(session_start_sys_time_ns_);
    lifecycle_lock.unlock();
    {
      std::lock_guard lock(bridge_control_mtx_);
      bridge_control_signature_.clear();
    }
  }
}

void FoxgloveServer::on_bridge_info(const std::vector<ProxyAPI::Info>& info_list) {
  if VUNLIKELY (!running_.load()) {
    return;
  }

  update_channels(info_list);

  {
    std::unique_lock lock(info_mtx_);
    last_info_map_.clear();

    for (const auto& info : info_list) {
      if VUNLIKELY (!is_url_allowed(info.url)) {
        continue;
      }

      last_info_map_[info.url] = info;
    }
  }

  broadcast_connection_graph_update();
}

void FoxgloveServer::on_bridge_data(const ProxyAPI::Data& data) {
  if VUNLIKELY (!running_.load()) {
    return;
  }
  std::shared_ptr<Stream> stream;
  std::vector<uint32_t> channel_ids;
  bool needed = false;
  {
    std::shared_lock lock(channels_mtx_);
    const auto found = streams_.find(data.url);
    if (found == streams_.end()) {
      return;
    }
    stream = found->second;
    channel_ids = stream->channel_ids;
    for (const auto id : channel_ids) {
      const auto channel = channels_.find(id);
      needed |= channel != channels_.end() && (channel->second.is_send_time || channel->second.schema_name.empty());
    }
  }
  if (stream->route.ser != data.ser || stream->route.type != SchemaData::resolve_type(data.schema, data.ser)) {
    return;
  }
  {
    std::shared_lock lock(sub_counts_mtx_);
    for (const auto channel_id : channel_ids) {
      const auto found = channel_subscribers_.find(channel_id);
      needed |= found != channel_subscribers_.end() && !found->second.empty();
    }
  }
  if (!needed) {
    return;
  }
  auto results = foxglove_converter_->convert(stream->route, data.raw);
  for (const auto& result : results) {
    if (!result.success) {
      continue;
    }
    if (result.is_send_time && result.timestamp_ns >= 0) {
      send_time(static_cast<uint64_t>(result.timestamp_ns));
    }
    const auto channel_id = channel_ids[result.output];
    std::unique_lock lifecycle_lock(channel_lifecycle_mtx_);
    if (!running_.load()) {
      return;
    }
    Json added;
    bool changed = false;
    bool was_visible = false;
    {
      std::unique_lock lock(channels_mtx_);
      const auto found = channels_.find(channel_id);
      if (found == channels_.end()) {
        continue;
      }
      auto& channel = found->second;
      const bool plugin = stream->route.outputs[result.output].plugin;
      if (channel.schema_name != result.schema_name || channel.encoding != result.encoding ||
          channel.schema_encoding != result.schema_encoding || channel.is_send_time != result.is_send_time ||
          (plugin && channel.schema != result.schema_data)) {
        std::string schema = result.schema_data;
        if (!plugin &&
            !foxglove_converter_->resolve_schema_by_name(result.schema_name, result.schema_encoding, schema)) {
          continue;
        }
        auto next = std::move(channel);
        was_visible = !next.is_time_only && !next.schema_name.empty();
        channels_.erase(found);
        next.id = allocate_channel_id();
        next.schema_name = result.schema_name;
        next.encoding = result.encoding;
        next.schema_encoding = result.schema_encoding;
        next.schema = std::move(schema);
        next.is_send_time = result.is_send_time;
        next.is_time_only = result.encoding == "send_time";
        update_channel_schema_payload(next);
        stream->channel_ids[result.output] = next.id;
        if (!next.is_time_only) {
          added = make_advertise_channel_json(next);
        }
        channels_.emplace(next.id, std::move(next));
        changed = true;
      }
    }
    if (changed) {
      clear_channel_runtime_state(channel_id, data.url);
      if (was_visible) {
        broadcast_json(Json{{"op", "unadvertise"}, {"channelIds", Json::array({channel_id})}});
      }
      if (!added.is_null()) {
        broadcast_json(Json{{"op", "advertise"}, {"channels", Json::array({std::move(added)})}});
      }
      lifecycle_lock.unlock();
      update_bridge_control();
      continue;
    }
    lifecycle_lock.unlock();
    if (result.encoding == "send_time") {
      continue;
    }
    std::vector<std::pair<ConnectionPtr, uint32_t>> targets;
    {
      std::scoped_lock lock(clients_mtx_, sub_counts_mtx_);
      const auto found = channel_subscribers_.find(channel_id);
      if (found == channel_subscribers_.end()) {
        continue;
      }
      targets.reserve(found->second.size());
      for (const auto& subscriber : found->second) {
        const auto client = clients_.find(subscriber.client_ptr);
        if (client != clients_.end() && client->second.conn) {
          targets.emplace_back(client->second.conn, subscriber.subscription_id);
        }
      }
    }
    if (targets.empty()) {
      continue;
    }
    auto fallback = estimate_bridge_wall_time_ns(last_sys_time_ns_.load(), bridge_time_elapsed_);
    fallback = resolve_bridge_data_timestamp_ns(session_start_sys_time_ns_.load(), data.timestamp, fallback);
    auto payload = build_message_data(0, resolve_message_timestamp_ns(result.timestamp_ns, fallback),
                                      result.payload.data(), result.payload.size());
    for (const auto& target : targets) {
      write_little_endian(payload.data() + 1, target.second);
      send_binary(target.first, payload);
    }
  }
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void FoxgloveServer::on_bridge_time(uint64_t sys_time, uint64_t) {
  const auto time =
      update_bridge_wall_time_state(sys_time, last_sys_time_ns_, bridge_time_elapsed_, session_start_sys_time_ns_);

  if VLIKELY (config_.send_time) {
    send_time(time);
  }
}

void FoxgloveServer::clear_channel_runtime_state(uint32_t channel_id, std::string_view url) {
  std::scoped_lock state_lock(clients_mtx_, channels_mtx_, sub_counts_mtx_);
  size_t remaining_url_subscriptions = 0;

  for (auto& client_entry : clients_) {
    auto& client = client_entry.second;

    for (auto subscription_iter = client.subscription_map.begin();
         subscription_iter != client.subscription_map.end();) {
      if VUNLIKELY (subscription_iter->second == channel_id) {
        subscription_iter = client.subscription_map.erase(subscription_iter);
      } else {
        ++subscription_iter;
      }
    }

    for (const auto& subscription : client.subscription_map) {
      auto channel_iter = channels_.find(subscription.second);

      if VUNLIKELY (channel_iter != channels_.end() && !channel_iter->second.is_control_only &&
                    channel_iter->second.url == url) {
        ++remaining_url_subscriptions;
      }
    }
  }

  channel_subscribers_.erase(channel_id);

  if VUNLIKELY (remaining_url_subscriptions > 0U) {
    url_sub_counts_[std::string(url)] = remaining_url_subscriptions;
  } else {
    url_sub_counts_.erase(std::string(url));
  }
}

void FoxgloveServer::update_channels(const std::vector<ProxyAPI::Info>& info_list) {
  std::unique_lock lifecycle_lock(channel_lifecycle_mtx_);
  if (!running_.load()) {
    return;
  }
  std::vector<std::pair<uint32_t, std::string>> removed;
  Json removed_ids = Json::array();
  Json added = Json::array();
  {
    std::unique_lock lock(channels_mtx_);
    const auto remove_stream = [&](const Stream& stream) {
      for (const auto id : stream.channel_ids) {
        const auto channel = channels_.find(id);
        if (channel == channels_.end()) {
          continue;
        }
        removed.emplace_back(id, channel->second.url);
        if (!channel->second.is_time_only && !channel->second.schema_name.empty()) {
          removed_ids.push_back(id);
        }
        channels_.erase(channel);
      }
    };
    std::unordered_set<std::string> active;
    for (const auto& info : info_list) {
      if (info.status == ProxyAPI::kInvalid || !is_publisher_info(info) || !is_url_allowed(info.url)) {
        continue;
      }
      active.insert(info.url);
      const auto type = SchemaData::resolve_type(info.schema, info.ser);
      const auto found = streams_.find(info.url);
      if (found != streams_.end()) {
        if (found->second->route.ser == info.ser && found->second->route.type == type) {
          continue;
        }
        remove_stream(*found->second);
        streams_.erase(found);
      }
      auto stream = std::make_shared<Stream>();
      stream->route = foxglove_converter_->resolve(info.url, type, info.ser);
      if (!stream->route.valid || stream->route.outputs.empty()) {
        continue;
      }
      for (const auto& output : stream->route.outputs) {
        const auto& schema = output.schema;
        ChannelInfo channel;
        channel.id = allocate_channel_id();
        channel.topic = info.url;
        channel.url = info.url;
        channel.ser = info.ser;
        channel.schema_type = type;
        channel.schema_name = output.schema_from_payload ? std::string{} : schema.schema_name;
        channel.encoding = schema.encoding;
        channel.schema_encoding = schema.schema_encoding;
        channel.schema = schema.schema_data;
        channel.is_send_time = schema.is_send_time;
        channel.is_time_only = schema.encoding == "send_time";
        if (!channel.is_time_only && !channel.schema_name.empty()) {
          update_channel_schema_payload(channel);
          added.push_back(make_advertise_channel_json(channel));
        }
        stream->channel_ids.push_back(channel.id);
        channels_.emplace(channel.id, std::move(channel));
      }
      streams_[info.url] = std::move(stream);
    }
    for (auto iter = streams_.begin(); iter != streams_.end();) {
      if (active.find(iter->first) == active.end()) {
        remove_stream(*iter->second);
        iter = streams_.erase(iter);
      } else {
        ++iter;
      }
    }
  }
  for (const auto& entry : removed) {
    clear_channel_runtime_state(entry.first, entry.second);
  }
  if (!removed_ids.empty()) {
    broadcast_json(Json{{"op", "unadvertise"}, {"channelIds", std::move(removed_ids)}});
  }
  if (!added.empty()) {
    broadcast_json(Json{{"op", "advertise"}, {"channels", std::move(added)}});
  }
  lifecycle_lock.unlock();
  update_bridge_control();
}

void FoxgloveServer::install_publish_channels() {
  if VUNLIKELY (!vlink_convert_) {
    return;
  }

  auto publish_channels = vlink_convert_->get_publish_channels();

  if VUNLIKELY (publish_channels.empty()) {
    return;
  }

  std::unique_lock lock(channels_mtx_);

  for (const auto& publish_channel : publish_channels) {
    CommandRoute route;

    if VUNLIKELY (!vlink_convert_->resolve_route(publish_channel, route) || route.url.empty() || route.ser.empty()) {
      MLOG_W("Skip invalid configured Foxglove publish route: {}", publish_channel.topic);
      continue;
    }

    if VUNLIKELY (!is_url_allowed(route.url)) {
      MLOG_W("Skip filtered Foxglove publish route: {} -> {}", publish_channel.topic, route.url);
      continue;
    }

    const auto channel_id = allocate_channel_id();
    ChannelInfo info;
    info.id = channel_id;
    info.is_control_only = true;
    info.topic = publish_channel.topic;
    info.encoding = publish_channel.encoding;
    info.schema_name = publish_channel.schema_name;
    info.schema_encoding = publish_channel.schema_encoding;
    info.schema = publish_channel.schema;
    info.schema_type = SchemaData::resolve_type(
        SchemaData::is_valid_type(route.schema_type) ? route.schema_type : SchemaType::kUnknown, route.ser);
    info.url = route.url;
    info.ser = route.ser;
    update_channel_schema_payload(info);
    channels_[channel_id] = std::move(info);
  }
}

ProxyAPI::Control FoxgloveServer::build_bridge_control() const {
  ProxyAPI::Control ctrl;
  ctrl.mode = ProxyAPI::kAuto;
  std::unordered_set<std::string> publish_urls;
  std::unordered_set<std::string> subscribed_urls;

  {
    std::scoped_lock state_lock(channels_mtx_, sub_counts_mtx_);
    size_t publish_route_count = 0;

    for (const auto& publish_entry : publish_channels_) {
      const auto& channel_map = publish_entry.second;

      for (const auto& channel_entry : channel_map) {
        const auto& publish_channel = channel_entry.second;

        if VLIKELY (publish_channel.has_route) {
          ++publish_route_count;
        }
      }
    }

    ctrl.url_meta_list.reserve(url_sub_counts_.size() + channels_.size() + publish_route_count);

    for (const auto& [url, count] : url_sub_counts_) {
      if VUNLIKELY (count == 0) {
        continue;
      }

      const auto stream = streams_.find(url);
      if (stream == streams_.end() || stream->second->route.type == SchemaType::kUnknown) {
        continue;
      }
      if (subscribed_urls.insert(url).second) {
        ctrl.url_meta_list.push_back({url, stream->second->route.ser, stream->second->route.type, kSubscriber});
      }
    }

    for (const auto& channel_entry : channels_) {
      const auto& ch = channel_entry.second;

      if (ch.is_control_only && ch.schema_type != SchemaType::kUnknown && publish_urls.insert(ch.url).second) {
        ctrl.url_meta_list.push_back({ch.url, ch.ser, ch.schema_type, kPublisher});
      }

      if VUNLIKELY (ch.is_send_time || ch.schema_name.empty()) {
        const auto subscribe_schema_type = ch.schema_type;

        if VUNLIKELY (subscribe_schema_type == SchemaType::kUnknown) {
          continue;
        }

        if VLIKELY (subscribed_urls.insert(ch.url).second) {
          ctrl.url_meta_list.push_back({ch.url, ch.ser, subscribe_schema_type, kSubscriber});
        }
      }
    }

    for (const auto& publish_entry : publish_channels_) {
      const auto& channel_map = publish_entry.second;

      for (const auto& channel_entry : channel_map) {
        const auto& publish_channel = channel_entry.second;

        if VUNLIKELY (!publish_channel.has_route) {
          continue;
        }

        const auto& route = publish_channel.route;
        auto publish_schema_type =
            SchemaData::is_valid_type(route.schema_type) ? route.schema_type : SchemaType::kUnknown;

        if VUNLIKELY (publish_schema_type == SchemaType::kUnknown) {
          publish_schema_type = publish_channel.schema_type;
        }

        if VUNLIKELY (route.url.empty() || route.ser.empty()) {
          continue;
        }

        if VUNLIKELY (!is_url_allowed(route.url)) {
          continue;
        }

        if VUNLIKELY (publish_schema_type == SchemaType::kUnknown) {
          continue;
        }

        if VUNLIKELY (!publish_urls.insert(route.url).second) {
          continue;
        }

        ctrl.url_meta_list.push_back({route.url, route.ser, publish_schema_type, kPublisher});
      }
    }
  }

  std::sort(ctrl.url_meta_list.begin(), ctrl.url_meta_list.end(),
            [](const ProxyAPI::UrlMeta& lhs, const ProxyAPI::UrlMeta& rhs) {
              if VLIKELY (lhs.url != rhs.url) {
                return lhs.url < rhs.url;
              }

              if VLIKELY (lhs.ser != rhs.ser) {
                return lhs.ser < rhs.ser;
              }

              if VLIKELY (lhs.schema != rhs.schema) {
                return lhs.schema < rhs.schema;
              }

              return lhs.type < rhs.type;
            });

  return ctrl;
}

bool FoxgloveServer::update_bridge_control() {
  if VUNLIKELY (!bridge_ || !bridge_->can_control()) {
    return false;
  }

  std::lock_guard lock(bridge_control_mtx_);

  auto ctrl = build_bridge_control();

  auto signature = build_bridge_control_signature(ctrl);

  if VLIKELY (signature == bridge_control_signature_) {
    return true;
  }

  reset_bridge_session_time_anchor(session_start_sys_time_ns_);

  if VUNLIKELY (!bridge_->send_control(ctrl, false)) {
    MLOG_W("Failed to update Foxglove bridge control");
    return false;
  }

  bridge_control_signature_ = std::move(signature);
  return true;
}

std::vector<std::string> FoxgloveServer::get_connect_endpoints() const {
  std::vector<std::string> endpoints;

  auto append_endpoint = [this, &endpoints](const std::string& host) {
    if VUNLIKELY (host.empty()) {
      return;
    }

    auto display_host = host;

    if VUNLIKELY (display_host.find(':') != std::string::npos && display_host.front() != '[' &&
                  display_host.back() != ']') {
      display_host = "[" + display_host + "]";
    }

    const auto endpoint = "ws://" + display_host + ":" + std::to_string(config_.port);

    if VLIKELY (std::find(endpoints.begin(), endpoints.end(), endpoint) == endpoints.end()) {
      endpoints.emplace_back(endpoint);
    }
  };

  if VUNLIKELY (config_.address.empty() || config_.address == "0.0.0.0") {
    append_endpoint("127.0.0.1");

    for (const auto& ip : Utils::get_all_ipv4_address(true)) {
      if VUNLIKELY (ip == "0.0.0.0") {
        continue;
      }

      append_endpoint(ip);
    }
  } else {
    append_endpoint(config_.address);
  }

  return endpoints;
}

void FoxgloveServer::log_connect_hint() const {
  const auto endpoints = get_connect_endpoints();

  MLOG_I("*****************************************************");
  MLOG_I("* Open [https://app.foxglove.dev/] in your browser.");

  if VLIKELY (!endpoints.empty()) {
    MLOG_I("* Available endpoints:");

    for (const auto& endpoint : endpoints) {
      MLOG_I("* - {}", endpoint);
    }
  }

  MLOG_I("*****************************************************");
}

uint32_t FoxgloveServer::allocate_channel_id() { return next_channel_id_.fetch_add(1); }

ClientInfo* FoxgloveServer::find_client_unlocked(ConnectionHdl hdl, void** out_raw_ptr) {
  if VUNLIKELY (!ws_server_) {
    if VLIKELY (out_raw_ptr) {
      *out_raw_ptr = nullptr;
    }

    return nullptr;
  }

  websocketpp::lib::error_code ec;
  auto conn = ws_server_->get_con_from_hdl(hdl, ec);

  if VUNLIKELY (ec) {
    if VLIKELY (out_raw_ptr) {
      *out_raw_ptr = nullptr;
    }

    return nullptr;
  }

  auto* raw_ptr = conn.get();

  if VLIKELY (out_raw_ptr) {
    *out_raw_ptr = raw_ptr;
  }

  auto client_iter = clients_.find(raw_ptr);

  if VLIKELY (client_iter != clients_.end()) {
    return &client_iter->second;
  }

  return nullptr;
}

bool FoxgloveServer::validate_publish_route_unlocked(void* raw_ptr, uint32_t channel_id, const std::string& topic,
                                                     const std::string& schema_name, const std::string& schema_encoding,
                                                     const std::string& schema, const CommandRoute& route,
                                                     std::string& error) const {
  if VUNLIKELY (route.url.empty() || route.ser.empty()) {
    error = "Client publish route is missing target URL or serialization";
    return false;
  }

  if VUNLIKELY (!is_url_allowed(route.url)) {
    error = "Client publish route is blocked by filter: " + route.url;
    return false;
  }

  const auto schema_type = SchemaData::resolve_type(route.schema_type, route.ser);
  for (const auto& [id, channel] : channels_) {
    if (channel.is_control_only && channel.url == route.url &&
        (channel.ser != route.ser || channel.schema_type != schema_type)) {
      error = "Client publish route conflicts with configured route for URL: " + route.url;
      return false;
    }
  }

  for (const auto& [client_ptr, channel_map] : publish_channels_) {
    for (const auto& [existing_channel_id, existing_channel] : channel_map) {
      if VUNLIKELY (client_ptr == raw_ptr && existing_channel_id == channel_id) {
        continue;
      }

      if VUNLIKELY (!existing_channel.has_route) {
        continue;
      }

      if VLIKELY (existing_channel.route.url == route.url &&
                  (existing_channel.route.ser != route.ser || existing_channel.schema_type != schema_type)) {
        error = "Client publish route conflicts with an existing channel for URL: " + route.url;
        return false;
      }
    }
  }

  if VLIKELY (!route.web_channel.encoding.empty() && !route.web_channel.schema_name.empty() &&
              !route.web_channel.schema_encoding.empty() && !route.web_channel.schema.empty()) {
    if VUNLIKELY (!schema_name.empty() && schema_name != route.web_channel.schema_name) {
      error = "Client publish schema name does not match local schema for topic " + topic + ": " + schema_name;
      return false;
    }

    if VUNLIKELY (!schema_encoding.empty() && schema_encoding != route.web_channel.schema_encoding) {
      error = "Client publish schema encoding does not match local schema for topic " + topic + ": " + schema_encoding;
      return false;
    }

    if VUNLIKELY (!schema.empty() &&
                  !schemas_match(schema, route.web_channel.schema, route.web_channel.schema_encoding)) {
      error =
          "Client publish schema does not match local schema for topic " + topic + ": " + route.web_channel.schema_name;
      return false;
    }

    return true;
  }

  if VUNLIKELY (schema.empty() || schema_name.empty() || schema_encoding.empty() || !foxglove_converter_) {
    return true;
  }

  std::string expected_schema;

  if VUNLIKELY (!foxglove_converter_->resolve_schema_by_name(schema_name, schema_encoding, expected_schema)) {
    return true;
  }

  if VLIKELY (schemas_match(schema, expected_schema, schema_encoding)) {
    return true;
  }

  error = "Client publish schema does not match local schema for topic " + topic + ": " + schema_name;
  return false;
}

bool FoxgloveServer::is_url_allowed(std::string_view url) const {
  return is_allowed_by_filters(url, config_.whitelist_exact, config_.whitelist_patterns, config_.blacklist_exact,
                               config_.blacklist_patterns);
}

}  // namespace webviz
}  // namespace vlink
