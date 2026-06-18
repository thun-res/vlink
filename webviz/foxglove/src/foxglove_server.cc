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

#include "foxglove_server.h"

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
#include "foxglove_protocol.h"
#include "webviz_app_utils.h"
#include "webviz_bridge_utils.h"
#include "webviz_time_utils.h"

namespace vlink {
namespace webviz {

static constexpr size_t kMaxTaskDepth = 10000U;

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

  if VLIKELY (!config.parameters.url.empty() || !config.parameters.values.empty()) {
    parameters_ = std::make_unique<FoxgloveParameters>(config.parameters);
  }

  if VLIKELY (config_.capabilities.publish && vlink_convert_) {
    install_publish_channels();
  }
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

FoxgloveServer::~FoxgloveServer() { stop(); }

bool FoxgloveServer::start() {
  if VUNLIKELY (running_.exchange(true)) {
    return true;
  }

  if VUNLIKELY (!async_run()) {
    running_.store(false);
    return false;
  }

  if VUNLIKELY (!init_bridge()) {
    quit(false);
    wait_for_quit();
    running_.store(false);
    return false;
  }

  if VUNLIKELY (parameters_ && !parameters_->start()) {
    running_.store(false);
    if VLIKELY (bridge_) {
      bridge_->stop();
    }
    quit(false);
    wait_for_quit();
    return false;
  }

  if VUNLIKELY (!init_websocket()) {
    running_.store(false);
    if VLIKELY (bridge_) {
      bridge_->stop();
    }
    if VLIKELY (parameters_) {
      parameters_->stop();
    }
    quit(false);
    wait_for_quit();
    return false;
  }

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

  client_count_.store(0);
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
      client_count_.store(0);

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
        url_to_channel_id_.erase(channel_iter->second.url);
      }

      channel_iter = channels_.erase(channel_iter);
    }
  }

  {
    std::unique_lock lock(active_bridge_urls_mtx_);
    active_bridge_urls_.clear();
  }

  active_bridge_urls_generation_.fetch_add(1);

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

    if VUNLIKELY (!should_process_bridge_data(data.url)) {
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

void FoxgloveServer::on_ws_open(ConnectionHdl hdl) {
  auto conn = ws_server_->get_con_from_hdl(hdl);
  auto* raw_ptr = conn.get();
  bool need_update = false;

  {
    std::unique_lock lock(clients_mtx_);
    ClientInfo client;
    client.id = next_client_id_.fetch_add(1);
    client.hdl = hdl;
    client.conn = conn;
    client.name = conn->get_remote_endpoint();
    clients_[raw_ptr] = std::move(client);
    client_count_.store(static_cast<uint32_t>(clients_.size()));
  }

  if VLIKELY (config_.capabilities.publish && bridge_ && bridge_->can_inject() && vlink_convert_) {
    std::unique_lock lock(channels_mtx_);
    auto& client_channels = publish_channels_[raw_ptr];

    for (const auto& [channel_id, channel] : channels_) {
      if VUNLIKELY (!channel.is_control_only || !is_url_allowed(channel.url)) {
        continue;
      }

      CommandChannel route_channel;
      route_channel.topic = channel.topic;
      route_channel.encoding = channel.encoding;
      route_channel.schema_name = channel.schema_name;
      route_channel.schema_encoding = channel.schema_encoding;
      route_channel.schema = channel.schema;

      CommandRoute route;

      if VUNLIKELY (!vlink_convert_->resolve_route(route_channel, route) || route.url.empty() || route.ser.empty()) {
        MLOG_W("Failed to initialize control publish route for {}", channel.topic);
        continue;
      }

      PublishChannel publish_channel;
      publish_channel.topic = channel.topic;
      publish_channel.encoding = channel.encoding;
      publish_channel.schema_name = channel.schema_name;
      publish_channel.schema_encoding = channel.schema_encoding;
      publish_channel.schema = channel.schema;
      publish_channel.schema_type = SchemaData::resolve_type(
          SchemaData::is_valid_type(route.schema_type) ? route.schema_type : SchemaType::kUnknown, route.ser);

      if VUNLIKELY (publish_channel.schema_type == SchemaType::kUnknown) {
        publish_channel.schema_type = SchemaData::resolve_type(channel.schema_type, channel.ser);
      }

      publish_channel.has_route = true;
      publish_channel.route = std::move(route);
      client_channels[channel_id] = std::move(publish_channel);
    }

    if VUNLIKELY (client_channels.empty()) {
      publish_channels_.erase(raw_ptr);
    } else {
      need_update = true;
    }
  }

  send_server_info(hdl);
  send_active_statuses(hdl);
  send_advertise(hdl);
  send_advertise_rpcs(hdl);

  if VUNLIKELY (need_update) {
    update_bridge_control();
  }
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
      client_count_.store(static_cast<uint32_t>(clients_.size()));
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
    rebuild_active_bridge_urls();
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

  {
    std::scoped_lock state_lock(clients_mtx_, channels_mtx_, sub_counts_mtx_);
    void* raw_ptr = nullptr;
    auto* client = find_client_unlocked(hdl, &raw_ptr);

    if VUNLIKELY (!client) {
      return;
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

      if VUNLIKELY (old_sub_iter != client->subscription_map.end() && old_sub_iter->second != channel_id) {
        auto old_ch_id = old_sub_iter->second;
        auto old_channel_iter = channels_.find(old_ch_id);

        if VLIKELY (old_channel_iter != channels_.end()) {
          if VLIKELY (!old_channel_iter->second.is_control_only) {
            auto old_sub_count_iter = url_sub_counts_.find(old_channel_iter->second.url);

            if VLIKELY (old_sub_count_iter != url_sub_counts_.end()) {
              if VLIKELY (old_sub_count_iter->second <= 1) {
                url_sub_counts_.erase(old_sub_count_iter);
                need_update = true;
              } else {
                --old_sub_count_iter->second;
              }
            }
          }
        }

        auto old_subscriber_iter = channel_subscribers_.find(old_ch_id);

        if VLIKELY (old_subscriber_iter != channel_subscribers_.end()) {
          auto& old_subs = old_subscriber_iter->second;
          old_subs.erase(std::remove_if(old_subs.begin(), old_subs.end(),
                                        [raw_ptr, sub_id](const ChannelSubscriber& s) {
                                          return s.client_ptr == raw_ptr && s.subscription_id == sub_id;
                                        }),
                         old_subs.end());

          if VLIKELY (old_subs.empty()) {
            channel_subscribers_.erase(old_subscriber_iter);
          }
        }
      }

      auto sub_iter = client->subscription_map.find(sub_id);
      bool is_new = (sub_iter == client->subscription_map.end() || sub_iter->second != channel_id);

      if VLIKELY (is_new) {
        auto channel_iter = channels_.find(channel_id);

        if VLIKELY (channel_iter != channels_.end()) {
          if VUNLIKELY (channel_iter->second.is_control_only) {
            MLOG_W("Ignoring subscribe to publish-only control channel: {}", channel_iter->second.topic);
            continue;
          }

          client->subscription_map[sub_id] = channel_id;

          channel_subscribers_[channel_id].push_back({raw_ptr, sub_id});

          auto& count = url_sub_counts_[channel_iter->second.url];

          if VLIKELY (count == 0) {
            need_update = true;
          }

          ++count;
        } else {
          MLOG_W("Subscribe to unknown channel_id: {}", channel_id);
        }
      }
    }
  }

  if VLIKELY (need_update) {
    rebuild_active_bridge_urls();
    update_bridge_control();
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
    rebuild_active_bridge_urls();
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

      if VUNLIKELY (id == 0) {
        MLOG_W("Frontend publish channel advertised without valid id for topic: {}", publish_channel.topic);
        continue;
      }

      auto control_channel_iter = channels_.find(id);

      if VUNLIKELY (control_channel_iter != channels_.end() && control_channel_iter->second.is_control_only) {
        MLOG_W("Frontend publish channel id {} conflicts with server-advertised control channel {}", id,
               control_channel_iter->second.topic);
        pending_statuses.emplace_back(2,
                                      "Client publish channel id conflicts with a server-advertised control channel");
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
    rebuild_active_bridge_urls();
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
        auto control_channel_iter = channels_.find(id);

        if VUNLIKELY (control_channel_iter != channels_.end() && control_channel_iter->second.is_control_only) {
          MLOG_W("Ignoring unadvertise for server-owned control channel {}: {}", id,
                 control_channel_iter->second.topic);
          continue;
        }

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
      auto channel_iter = channels_.find(bin_msg.channel_or_service_id);

      if VUNLIKELY (channel_iter == channels_.end() || !channel_iter->second.is_control_only) {
        MLOG_W("Client message for unknown channel: {}", bin_msg.channel_or_service_id);
        status_message = "Client message references an unknown publish channel";
        lookup_failed = true;
      }

      if VLIKELY (!lookup_failed) {
        CommandChannel route_channel;
        route_channel.topic = channel_iter->second.topic;
        route_channel.encoding = channel_iter->second.encoding;
        route_channel.schema_name = channel_iter->second.schema_name;
        route_channel.schema_encoding = channel_iter->second.schema_encoding;
        route_channel.schema = channel_iter->second.schema;
        route_schema_type = channel_iter->second.schema_type;

        if VUNLIKELY (!vlink_convert_ || !vlink_convert_->resolve_route(route_channel, route)) {
          MLOG_W("Failed to resolve control publish route: {}", route_channel.topic);
          status_message = "Client channel has no active publish route: " + route_channel.topic;
          lookup_failed = true;
        }

        if VLIKELY (!lookup_failed) {
          const auto resolved_route_schema_type =
              SchemaData::is_valid_type(route.schema_type) ? route.schema_type : SchemaType::kUnknown;

          if VLIKELY (resolved_route_schema_type != SchemaType::kUnknown) {
            route_schema_type = resolved_route_schema_type;
          }
        }

        if VLIKELY (!lookup_failed) {
          std::string route_error;
          if VUNLIKELY (!validate_publish_route_unlocked(raw_ptr, bin_msg.channel_or_service_id, route_channel.topic,
                                                         route_channel.schema_name, route_channel.schema_encoding,
                                                         route_channel.schema, route, route_error)) {
            if VLIKELY (!route_error.empty()) {
              status_message = std::move(route_error);
            } else {
              status_message = "Client channel has no active publish route: " + route_channel.topic;
            }
            lookup_failed = true;
          }
        }
      }
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

  std::string request_id;

  if VUNLIKELY (msg.contains("id") && !msg["id"].is_string()) {
    send_status(hdl, 2, "getParameters id must be a string");
    return;
  }

  if VLIKELY (msg.contains("id")) {
    request_id = msg["id"].get<std::string>();
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
    } else {
      client->subscribed_all_parameters = false;
      client->parameter_subscriptions.clear();
      client->parameter_exclusions.clear();

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

void FoxgloveServer::broadcast_connection_graph_update() {
  {
    std::shared_lock lock(clients_mtx_);
    bool has_subscribers = false;

    for (const auto& client_entry : clients_) {
      const auto& client = client_entry.second;

      if VLIKELY (client.subscribed_connection_graph) {
        has_subscribers = true;
        break;
      }
    }

    if VUNLIKELY (!has_subscribers) {
      return;
    }
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

  thread_local std::vector<ConnectionPtr> targets;
  targets.clear();

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
      if VUNLIKELY (ch.is_time_only) {
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
  thread_local std::vector<ConnectionPtr> targets;
  targets.clear();

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

  thread_local std::vector<ConnectionPtr> targets;
  targets.clear();

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

  bool has_removals = false;

  for (const auto& entry : delta) {
    if VUNLIKELY (!entry.has_value) {
      has_removals = true;
      break;
    }
  }

  std::vector<std::pair<ConnectionPtr, Json>> pending;

  {
    std::shared_lock lock(clients_mtx_);

    for (auto& client_entry : clients_) {
      auto& client = client_entry.second;

      if VUNLIKELY (!client.subscribed_all_parameters && client.parameter_subscriptions.empty()) {
        continue;
      }

      std::vector<std::string> matched_names;
      matched_names.reserve(delta.size());

      for (const auto& entry : delta) {
        if VLIKELY (client.subscribed_all_parameters) {
          if VLIKELY (client.parameter_exclusions.count(entry.name) == 0U) {
            matched_names.emplace_back(entry.name);
          }
        } else if VLIKELY (client.parameter_subscriptions.count(entry.name) > 0U) {
          matched_names.emplace_back(entry.name);
        }
      }

      if VLIKELY (!matched_names.empty()) {
        if VUNLIKELY (has_removals) {
          if VLIKELY (client.subscribed_all_parameters) {
            auto current_names = parameters_->get_names();
            current_names.erase(std::remove_if(current_names.begin(), current_names.end(),
                                               [&client](const std::string& name) {
                                                 return client.parameter_exclusions.count(name) > 0U;
                                               }),
                                current_names.end());
            pending.emplace_back(client.conn, parameters_->build_parameter_values(current_names, {}));
          } else {
            std::sort(matched_names.begin(), matched_names.end());
            matched_names.erase(std::unique(matched_names.begin(), matched_names.end()), matched_names.end());
            pending.emplace_back(client.conn, parameters_->build_parameter_values(matched_names, {}));
          }
        } else {
          pending.emplace_back(client.conn, parameters_->build_parameter_values(matched_names, {}));
        }
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

          if VLIKELY (!ch.is_time_only) {
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
            url_to_channel_id_.erase(channel_iter->second.url);
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
      prev_connection_graph_ = Json{};
    }

    reset_bridge_wall_time_state(last_sys_time_ns_, bridge_time_elapsed_);
    reset_bridge_session_time_anchor(session_start_sys_time_ns_);
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

  uint32_t channel_id = 0;
  bool is_send_time_ch = false;
  bool is_time_only_ch = false;
  ChannelSubscriber single_target;
  bool has_single_target = false;
  thread_local std::vector<ChannelSubscriber> targets;
  targets.clear();

  {
    std::unique_lock lock(channels_mtx_);
    auto url_iter = url_to_channel_id_.find(data.url);

    if VUNLIKELY (url_iter == url_to_channel_id_.end()) {
      return;
    }

    channel_id = url_iter->second;

    auto channel_iter = channels_.find(channel_id);

    if VLIKELY (channel_iter != channels_.end()) {
      is_send_time_ch = channel_iter->second.is_send_time;
      is_time_only_ch = channel_iter->second.is_time_only;
    }
  }

  FoxgloveMessage result;
  bool has_result = false;

  if VUNLIKELY (is_send_time_ch) {
    result = foxglove_converter_->convert(data.url, data.schema, data.ser, data.raw);
    has_result = true;

    if VLIKELY (result.timestamp_ns >= 0) {
      send_time(static_cast<uint64_t>(result.timestamp_ns));
    }

    if VUNLIKELY (is_time_only_ch) {
      return;
    }
  }

  {
    std::unique_lock sc_lock(sub_counts_mtx_);
    auto subscriber_iter = channel_subscribers_.find(channel_id);

    if VUNLIKELY (subscriber_iter == channel_subscribers_.end() || subscriber_iter->second.empty()) {
      return;
    }

    if VLIKELY (subscriber_iter->second.size() == 1U) {
      single_target = subscriber_iter->second.front();
      has_single_target = true;
    } else {
      targets.reserve(subscriber_iter->second.size());
      targets.assign(subscriber_iter->second.begin(), subscriber_iter->second.end());
    }
  }

  if VUNLIKELY (!has_result) {
    result = foxglove_converter_->convert(data.url, data.schema, data.ser, data.raw);
  }

  if VUNLIKELY (!result.success) {
    return;
  }

  bool schema_changed = false;
  Json remove_msg;
  Json add_msg;
  std::string updated_topic;
  std::string updated_schema;
  uint32_t old_channel_id = 0;
  uint32_t new_channel_id = 0;
  bool move_channel_needed = false;

  if VUNLIKELY (!result.schema_name.empty()) {
    std::unique_lock lock(channels_mtx_);
    auto channel_iter = channels_.find(channel_id);

    if VLIKELY (channel_iter != channels_.end()) {
      auto& ch = channel_iter->second;
      const bool schema_identity_changed = ch.schema_name != result.schema_name || ch.encoding != result.encoding ||
                                           ch.schema_encoding != result.schema_encoding;

      if VUNLIKELY (schema_identity_changed || ch.schema.empty()) {
        std::string schema_data = result.schema_data;

        if VLIKELY (!schema_data.empty() || foxglove_converter_->resolve_schema_by_name(
                                                result.schema_name, result.schema_encoding, schema_data)) {
          const bool advertise_changed = schema_identity_changed || ch.schema != schema_data;

          if VUNLIKELY (advertise_changed) {
            auto old_id = ch.id;
            auto new_id = allocate_channel_id();
            auto updated = ch;
            updated.id = new_id;
            updated.schema_name = result.schema_name;
            updated.encoding = result.encoding;
            updated.schema_encoding = result.schema_encoding;
            updated.schema = schema_data;

            update_channel_schema_payload(updated);

            channels_.erase(channel_iter);
            channels_[new_id] = updated;
            url_to_channel_id_[data.url] = new_id;
            schema_changed = true;
            updated_topic = updated.topic;
            updated_schema = result.schema_name;
            old_channel_id = old_id;
            new_channel_id = new_id;
            move_channel_needed = true;

            remove_msg["op"] = "unadvertise";
            remove_msg["channelIds"] = Json::array({old_id});

            add_msg["op"] = "advertise";
            add_msg["channels"] = Json::array();

            add_msg["channels"].emplace_back(make_advertise_channel_json(updated));
          }
        }
      }
    }
  }

  if VUNLIKELY (schema_changed) {
    if VUNLIKELY (move_channel_needed) {
      move_channel_runtime_state(old_channel_id, new_channel_id);
    }

    broadcast_json(remove_msg);
    broadcast_json(add_msg);
  }

  auto fallback_timestamp_ns = estimate_bridge_wall_time_ns(last_sys_time_ns_.load(), bridge_time_elapsed_);
  fallback_timestamp_ns =
      resolve_bridge_data_timestamp_ns(session_start_sys_time_ns_.load(), data.timestamp, fallback_timestamp_ns);
  auto timestamp_ns = resolve_message_timestamp_ns(result.timestamp_ns, fallback_timestamp_ns);

  if VLIKELY (has_single_target) {
    ConnectionPtr conn;

    {
      std::shared_lock lock(clients_mtx_);
      auto client_iter = clients_.find(single_target.client_ptr);

      if VLIKELY (client_iter != clients_.end()) {
        conn = client_iter->second.conn;
      }
    }

    if VUNLIKELY (!conn) {
      return;
    }

    auto payload =
        build_message_data(single_target.subscription_id, timestamp_ns, result.payload.data(), result.payload.size());
    send_binary(conn, payload);
  } else {
    auto payload_size = result.payload.size();
    auto buf = Bytes::create(1 + 4 + 8 + payload_size);
    auto* ptr = buf.data();

    ptr[0] = static_cast<uint8_t>(ServerBinaryOpcode::kMessageData);
    std::memcpy(ptr + 5, &timestamp_ns, 8);

    if VLIKELY (payload_size > 0) {
      std::memcpy(ptr + 13, result.payload.data(), payload_size);
    }

    thread_local std::vector<std::pair<ConnectionPtr, uint32_t>> send_targets;
    send_targets.clear();
    send_targets.reserve(targets.size());

    {
      std::shared_lock lock(clients_mtx_);

      for (const auto& target : targets) {
        auto client_iter = clients_.find(target.client_ptr);

        if VUNLIKELY (client_iter == clients_.end() || !client_iter->second.conn) {
          continue;
        }

        send_targets.emplace_back(client_iter->second.conn, target.subscription_id);
      }
    }

    for (const auto& target : send_targets) {
      try {
        std::memcpy(ptr + 1, &target.second, 4);
        auto ec = target.first->send(buf.data(), buf.size(), websocketpp::frame::opcode::binary);

        if VUNLIKELY (ec) {
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
  }
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void FoxgloveServer::on_bridge_time(uint64_t sys_time, uint64_t boot_time) {
  auto state = make_bridge_wall_time_state(sys_time, boot_time);
  update_bridge_wall_time_state(sys_time, boot_time, last_sys_time_ns_, bridge_time_elapsed_,
                                &session_start_sys_time_ns_);

  if VLIKELY (config_.send_time) {
    send_time(state.last_sys_time_ns);
  }
}

void FoxgloveServer::clear_channel_runtime_state(uint32_t channel_id, std::string_view url) {
  {
    std::unique_lock lock(clients_mtx_);

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
    }
  }

  {
    std::unique_lock sc_lock(sub_counts_mtx_);
    channel_subscribers_.erase(channel_id);
    url_sub_counts_.erase(std::string(url));
  }

  rebuild_active_bridge_urls();
}

void FoxgloveServer::move_channel_runtime_state(uint32_t old_channel_id, uint32_t new_channel_id) {
  if VUNLIKELY (old_channel_id == 0 || new_channel_id == 0 || old_channel_id == new_channel_id) {
    return;
  }

  {
    std::unique_lock lock(clients_mtx_);

    for (auto& client_entry : clients_) {
      auto& client = client_entry.second;

      for (auto& subscription_entry : client.subscription_map) {
        auto& channel_id = subscription_entry.second;

        if VUNLIKELY (channel_id == old_channel_id) {
          channel_id = new_channel_id;
        }
      }
    }
  }

  {
    std::unique_lock sc_lock(sub_counts_mtx_);
    auto old_subscriber_iter = channel_subscribers_.find(old_channel_id);

    if VUNLIKELY (old_subscriber_iter == channel_subscribers_.end()) {
      return;
    }

    auto& target = channel_subscribers_[new_channel_id];
    auto& source = old_subscriber_iter->second;
    target.insert(target.end(), std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
    channel_subscribers_.erase(old_subscriber_iter);
  }

  rebuild_active_bridge_urls();
}

void FoxgloveServer::update_channels(const std::vector<ProxyAPI::Info>& info_list) {
  struct RemovedChannelState final {
    uint32_t id{0};
    uint32_t replacement_id{0};
    std::string url;
  };

  std::vector<ChannelInfo> new_channels;
  std::vector<ChannelInfo> updated_channels;
  std::vector<RemovedChannelState> removed_channel_states;
  std::vector<uint32_t> removed_advertised_ids;
  bool need_rpc_refresh = false;
  bool need_subscription_refresh = false;

  {
    std::unique_lock lock(channels_mtx_);

    std::unordered_set<std::string> active_urls;

    for (const auto& info : info_list) {
      if VUNLIKELY (info.status == ProxyAPI::kInvalid) {
        continue;
      }

      if VUNLIKELY (!is_url_allowed(info.url)) {
        continue;
      }

      if VUNLIKELY (!is_publisher_info(info)) {
        continue;
      }

      active_urls.insert(info.url);

      auto existing_url_iter = url_to_channel_id_.find(info.url);

      if VLIKELY (existing_url_iter != url_to_channel_id_.end()) {
        auto channel_iter = channels_.find(existing_url_iter->second);

        if VUNLIKELY (channel_iter == channels_.end()) {
          url_to_channel_id_.erase(existing_url_iter);
          continue;
        }

        auto& ch = channel_iter->second;
        const bool ser_changed = ch.ser != info.ser;
        const auto reported_schema_type = SchemaData::is_valid_type(info.schema) ? info.schema : SchemaType::kUnknown;
        const auto effective_schema_type = SchemaData::resolve_type(reported_schema_type, info.ser);

        const bool schema_type_changed = ch.schema_type != effective_schema_type;

        if VUNLIKELY (ser_changed) {
          ch.ser = info.ser;
          need_subscription_refresh = true;
        }

        std::string schema_name;
        std::string encoding;
        std::string schema_encoding;
        std::string schema_data;
        bool is_send_time = false;

        if VLIKELY (foxglove_converter_->get_schema_info(info.url, effective_schema_type, info.ser, schema_name,
                                                         encoding, schema_encoding, schema_data, &is_send_time)) {
          const bool was_send_time = ch.is_send_time;
          const bool was_time_only = ch.is_time_only;
          const bool is_time_only = (encoding == "send_time");
          const bool advertise_changed = ch.encoding != encoding || ch.schema_name != schema_name ||
                                         ch.schema_encoding != schema_encoding || ch.schema != schema_data ||
                                         was_time_only != is_time_only;
          const bool metadata_changed =
              advertise_changed || was_send_time != is_send_time || ser_changed || schema_type_changed;

          if VUNLIKELY (metadata_changed) {
            ch.encoding = encoding;
            ch.schema_name = schema_name;
            ch.schema = schema_data;
            ch.schema_encoding = schema_encoding;
            ch.schema_type = effective_schema_type;
            ch.is_send_time = is_send_time;
            ch.is_time_only = is_time_only;

            update_channel_schema_payload(ch);
          }

          if VUNLIKELY (was_send_time != is_send_time) {
            need_subscription_refresh = true;
          }

          if VUNLIKELY (schema_type_changed) {
            need_subscription_refresh = true;
          }

          if VUNLIKELY (advertise_changed) {
            auto old_id = ch.id;
            auto new_id = allocate_channel_id();
            auto updated = ch;
            updated.id = new_id;

            update_channel_schema_payload(updated);

            channels_.erase(channel_iter);
            channels_[new_id] = updated;
            url_to_channel_id_[info.url] = new_id;
            removed_channel_states.push_back({old_id, is_time_only ? 0U : new_id, info.url});

            if VUNLIKELY (!was_time_only) {
              removed_advertised_ids.emplace_back(old_id);
            }

            if VLIKELY (!is_time_only) {
              updated_channels.emplace_back(updated);
            }

            need_subscription_refresh = true;
          }
        } else {
          active_urls.erase(info.url);
          removed_channel_states.push_back({ch.id, 0, info.url});

          if VUNLIKELY (!ch.is_time_only) {
            removed_advertised_ids.emplace_back(ch.id);
          } else {
            need_rpc_refresh = true;
          }

          channels_.erase(channel_iter);
          url_to_channel_id_.erase(existing_url_iter);
        }

        continue;
      }

      std::string schema_name;
      std::string encoding;
      std::string schema_encoding;
      std::string schema_data;
      bool is_send_time = false;

      if VUNLIKELY (!foxglove_converter_->get_schema_info(info.url, info.schema, info.ser, schema_name, encoding,
                                                          schema_encoding, schema_data, &is_send_time)) {
        continue;
      }

      auto id = allocate_channel_id();

      ChannelInfo ch;
      ch.id = id;
      ch.topic = info.url;
      ch.encoding = encoding;
      ch.schema_name = schema_name;
      ch.schema = schema_data;
      ch.schema_encoding = schema_encoding;
      ch.schema_type = SchemaData::resolve_type(
          SchemaData::is_valid_type(info.schema) ? info.schema : SchemaType::kUnknown, info.ser);
      ch.is_send_time = is_send_time;
      ch.is_time_only = (encoding == "send_time");

      update_channel_schema_payload(ch);

      ch.url = info.url;
      ch.ser = info.ser;

      channels_[id] = ch;
      url_to_channel_id_[info.url] = id;

      if VUNLIKELY (ch.is_send_time) {
        need_subscription_refresh = true;
      }

      if VLIKELY (!ch.is_time_only) {
        new_channels.emplace_back(ch);
      }
    }

    for (auto url_iter = url_to_channel_id_.begin(); url_iter != url_to_channel_id_.end();) {
      if VUNLIKELY (active_urls.find(url_iter->first) == active_urls.end()) {
        auto channel_iter = channels_.find(url_iter->second);

        if VLIKELY (channel_iter != channels_.end()) {
          removed_channel_states.push_back({url_iter->second, 0, url_iter->first});

          if VUNLIKELY (!channel_iter->second.is_time_only) {
            removed_advertised_ids.emplace_back(url_iter->second);
          }

          channels_.erase(channel_iter);
        }

        url_iter = url_to_channel_id_.erase(url_iter);
      } else {
        ++url_iter;
      }
    }
  }

  if VUNLIKELY (!removed_advertised_ids.empty()) {
    Json msg;
    msg["op"] = "unadvertise";
    msg["channelIds"] = removed_advertised_ids;
    broadcast_json(msg);
  }

  if VUNLIKELY (!removed_channel_states.empty()) {
    for (const auto& state : removed_channel_states) {
      if VUNLIKELY (state.replacement_id != 0) {
        move_channel_runtime_state(state.id, state.replacement_id);
      } else {
        clear_channel_runtime_state(state.id, state.url);
      }
    }
  }

  if VLIKELY (!new_channels.empty()) {
    Json msg;
    msg["op"] = "advertise";
    msg["channels"] = Json::array();

    for (const auto& ch : new_channels) {
      msg["channels"].emplace_back(make_advertise_channel_json(ch));
    }

    broadcast_json(msg);
  }

  if VLIKELY (!updated_channels.empty()) {
    Json msg;
    msg["op"] = "advertise";
    msg["channels"] = Json::array();

    for (const auto& ch : updated_channels) {
      msg["channels"].emplace_back(make_advertise_channel_json(ch));
    }

    broadcast_json(msg);
  }

  if VLIKELY (need_subscription_refresh || !removed_channel_states.empty() || need_rpc_refresh) {
    update_bridge_control();
  }

  rebuild_active_bridge_urls();
}

bool FoxgloveServer::should_process_bridge_data(const std::string& url) {
  struct ActiveBridgeUrlCacheEntry final {
    const FoxgloveServer* owner{nullptr};
    uint64_t generation{0};
    std::string url;
    bool active{false};
  };

  thread_local std::array<ActiveBridgeUrlCacheEntry, 8> cache_entries;

  if VUNLIKELY (client_count_.load() == 0U) {
    return false;
  }

  const auto generation = active_bridge_urls_generation_.load();
  const auto cache_index = std::hash<std::string_view>{}(url) % cache_entries.size();
  auto& cache_entry = cache_entries[cache_index];

  if VLIKELY (cache_entry.owner == this && cache_entry.generation == generation && cache_entry.url == url) {
    return cache_entry.active;
  }

  std::shared_lock lock(active_bridge_urls_mtx_);
  const auto active = active_bridge_urls_.find(url) != active_bridge_urls_.end();
  cache_entry.owner = this;
  cache_entry.generation = generation;
  cache_entry.url = url;
  cache_entry.active = active;
  return active;
}

void FoxgloveServer::rebuild_active_bridge_urls() {
  std::scoped_lock state_lock(channels_mtx_, sub_counts_mtx_);
  rebuild_active_bridge_urls_locked();
}

void FoxgloveServer::rebuild_active_bridge_urls_locked() {
  std::unordered_set<std::string> next_active_bridge_urls;
  next_active_bridge_urls.reserve(url_sub_counts_.size() + channels_.size());

  for (const auto& [url, count] : url_sub_counts_) {
    if VLIKELY (count > 0U) {
      next_active_bridge_urls.emplace(url);
    }
  }

  for (const auto& channel_entry : channels_) {
    const auto& channel = channel_entry.second;

    if VLIKELY (channel.is_send_time && !channel.url.empty()) {
      next_active_bridge_urls.emplace(channel.url);
    }
  }

  {
    std::unique_lock lock(active_bridge_urls_mtx_);
    active_bridge_urls_.swap(next_active_bridge_urls);
  }

  active_bridge_urls_generation_.fetch_add(1);
}

uint32_t FoxgloveServer::allocate_channel_id() { return next_channel_id_.fetch_add(1); }

ClientInfo* FoxgloveServer::find_client_unlocked(ConnectionHdl hdl, void** out_raw_ptr) {
  if VUNLIKELY (!ws_server_) {
    if VLIKELY (out_raw_ptr) {
      *out_raw_ptr = nullptr;
    }

    return nullptr;
  }

  auto conn = ws_server_->get_con_from_hdl(hdl);
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

  for (const auto& [client_ptr, channel_map] : publish_channels_) {
    for (const auto& [existing_channel_id, existing_channel] : channel_map) {
      if VUNLIKELY (client_ptr == raw_ptr && existing_channel_id == channel_id) {
        continue;
      }

      if VUNLIKELY (!existing_channel.has_route) {
        continue;
      }

      if VLIKELY (existing_channel.route.url == route.url && existing_channel.route.ser != route.ser) {
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

      auto channel_id_iter = url_to_channel_id_.find(url);

      if VUNLIKELY (channel_id_iter == url_to_channel_id_.end()) {
        continue;
      }

      auto channel_iter = channels_.find(channel_id_iter->second);

      if VLIKELY (channel_iter != channels_.end()) {
        const auto subscribe_schema_type = channel_iter->second.schema_type;

        if VUNLIKELY (subscribe_schema_type == SchemaType::kUnknown) {
          continue;
        }

        if VLIKELY (subscribed_urls.insert(channel_iter->second.url).second) {
          ctrl.url_meta_list.push_back(
              {channel_iter->second.url, channel_iter->second.ser, subscribe_schema_type, kSubscriber});
        }
      }
    }

    for (const auto& channel_entry : channels_) {
      const auto& ch = channel_entry.second;

      if VUNLIKELY (ch.is_send_time) {
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

bool FoxgloveServer::is_url_allowed(std::string_view url) const {
  return is_allowed_by_filters_cached(this, url, config_.whitelist_exact, config_.whitelist_patterns,
                                      config_.blacklist_exact, config_.blacklist_patterns);
}

}  // namespace webviz
}  // namespace vlink
