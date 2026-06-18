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

#include <vlink/base/bytes.h>
#include <vlink/base/elapsed_timer.h>
#include <vlink/base/logger.h>
#include <vlink/base/macros.h>
#include <vlink/base/message_loop.h>

//
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

//
#include "foxglove_converter.h"
#include "foxglove_parameters.h"
#include "foxglove_rpc.h"
#include "proxy_bridge.h"
#include "vlink_convert.h"

//
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

namespace vlink {
namespace webviz {

using WsServer = websocketpp::server<websocketpp::config::asio>;
using ConnectionHdl = websocketpp::connection_hdl;
using ConnectionPtr = WsServer::connection_ptr;
using MessagePtr = WsServer::message_ptr;
using Json = nlohmann::json;

struct ClientInfo final {
  uint64_t id{0};
  ConnectionHdl hdl;
  ConnectionPtr conn;
  std::string name;
  std::unordered_map<uint32_t, uint32_t> subscription_map;
  std::unordered_set<std::string> parameter_subscriptions;
  std::unordered_set<std::string> parameter_exclusions;
  bool subscribed_all_parameters{false};
  bool subscribed_connection_graph{false};
};

struct ChannelInfo final {
  uint32_t id{0};
  bool is_send_time{false};
  bool is_time_only{false};
  bool is_control_only{false};
  std::string topic;
  std::string encoding;
  std::string schema_name;
  std::string schema;
  std::string schema_encoding;
  std::string schema_base64;
  std::string url;
  std::string ser;
  SchemaType schema_type{SchemaType::kUnknown};
};

// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
class FoxgloveServer final : public MessageLoop {
 public:
  struct Capabilities final {
    bool time{true};
    bool connection_graph{true};
    bool publish{false};
    bool rpcs{false};
    bool assets{false};
  };

  // NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
  struct Config final {
    uint16_t port{8765};
    std::string address{"0.0.0.0"};
    std::string name{"vlink-foxglove"};
    std::string config_file;
    std::string proto_dir;
    std::string fbs_dir;
    std::string schema_plugin_path;
    std::string convert_plugin_path;
    std::string convert_plugin_config;
    std::vector<std::string> vlink_msgs;
    std::vector<std::string> foxglove_msgs;
    std::vector<std::string> rpc_msgs;
    std::vector<std::string> whitelist_exact;
    std::vector<std::string> whitelist_patterns;
    std::vector<std::string> blacklist_exact;
    std::vector<std::string> blacklist_patterns;
    std::vector<std::string> asset_dirs;
    ProxyBridge::Config proxy_config;
    FoxgloveParameters::Config parameters;
    Capabilities capabilities;
    bool send_time{false};
  };

  explicit FoxgloveServer(const Config& config);

  ~FoxgloveServer() override;

  bool start();

  void stop();

  [[nodiscard]] size_t get_max_task_count() const override;

 private:
  bool init_websocket();

  bool init_bridge();

  static bool get_json_u32(const Json& value, uint32_t& out);

  static Json make_advertise_channel_json(const ChannelInfo& channel_info);

  static void update_channel_schema_payload(ChannelInfo& channel_info);

  static bool is_binary_schema_encoding(std::string_view schema_encoding);

  static bool schemas_match(std::string_view provided_schema, std::string_view expected_schema,
                            std::string_view schema_encoding);

  static bool parse_parameter_names(const Json& msg, std::vector<std::string>& out, std::string& error);

  static Json build_sorted_connection_entries(std::unordered_map<std::string, std::vector<std::string>>& groups,
                                              const char* value_key);

  void on_ws_open(ConnectionHdl hdl);

  void on_ws_close(ConnectionHdl hdl);

  void on_ws_message(ConnectionHdl hdl, MessagePtr msg);

  void handle_json_message(ConnectionHdl hdl, const std::string& payload);

  void handle_binary_message(ConnectionHdl hdl, const std::string& payload);

  void handle_subscribe(ConnectionHdl hdl, const Json& msg);

  void handle_unsubscribe(ConnectionHdl hdl, const Json& msg);

  void handle_publish_advertise(ConnectionHdl hdl, const Json& msg);

  void handle_publish_unadvertise(ConnectionHdl hdl, const Json& msg);

  void handle_publish_message(ConnectionHdl hdl, const std::string& payload);

  void handle_rpc_call_request(ConnectionHdl hdl, const std::string& payload);

  void handle_subscribe_connection_graph(ConnectionHdl hdl);

  void handle_unsubscribe_connection_graph(ConnectionHdl hdl);

  void handle_get_parameters(ConnectionHdl hdl, const Json& msg);

  void handle_set_parameters(ConnectionHdl hdl, const Json& msg);

  void handle_subscribe_parameter_updates(ConnectionHdl hdl, const Json& msg);

  void handle_unsubscribe_parameter_updates(ConnectionHdl hdl, const Json& msg);

  void broadcast_connection_graph_update();

  Json build_connection_graph() const;

  void handle_fetch_asset(ConnectionHdl hdl, const Json& msg);

  void send_server_info(ConnectionHdl hdl);

  void send_advertise(ConnectionHdl hdl);

  void send_advertise_rpcs(ConnectionHdl hdl);

  void send_json(const ConnectionPtr& conn, const Json& msg);

  void send_json(ConnectionHdl hdl, const Json& msg);

  void send_binary(const ConnectionPtr& conn, const Bytes& buf);

  void send_binary(ConnectionHdl hdl, const Bytes& buf);

  void send_status(ConnectionHdl hdl, int level, std::string_view message, std::string_view status_id = {});

  void send_remove_status(ConnectionHdl hdl, const std::vector<std::string>& status_ids);

  void send_active_statuses(ConnectionHdl hdl);

  void set_global_status(std::string_view status_id, int level, std::string_view message);

  void clear_global_status(std::string_view status_id);

  bool has_send_time_source();

  bool has_time_capability();

  bool has_parameters_capability() const;

  void send_time(uint64_t timestamp_ns);

  void broadcast_json(const Json& msg);

  bool should_process_bridge_data(const std::string& url);
  void rebuild_active_bridge_urls();
  void rebuild_active_bridge_urls_locked();

  void on_parameters_changed(const std::vector<FoxgloveParameters::ParameterEntry>& delta);

  void on_bridge_connected(bool connected);

  void on_bridge_info(const std::vector<ProxyAPI::Info>& info_list);

  void on_bridge_data(const ProxyAPI::Data& data);

  void on_bridge_time(uint64_t sys_time, uint64_t boot_time);

  void clear_channel_runtime_state(uint32_t channel_id, std::string_view url);

  void move_channel_runtime_state(uint32_t old_channel_id, uint32_t new_channel_id);

  void update_channels(const std::vector<ProxyAPI::Info>& info_list);

  void install_publish_channels();

  ProxyAPI::Control build_bridge_control() const;

  bool update_bridge_control();

  std::vector<std::string> get_connect_endpoints() const;

  void log_connect_hint() const;

  uint32_t allocate_channel_id();

  ClientInfo* find_client_unlocked(ConnectionHdl hdl, void** out_raw_ptr = nullptr);

  bool validate_publish_route_unlocked(void* raw_ptr, uint32_t channel_id, const std::string& topic,
                                       const std::string& schema_name, const std::string& schema_encoding,
                                       const std::string& schema, const CommandRoute& route, std::string& error) const;

  bool is_url_allowed(std::string_view url) const;

  Config config_;
  std::unique_ptr<WsServer> ws_server_;
  std::unique_ptr<ProxyBridge> bridge_;
  std::unique_ptr<FoxgloveConverter> foxglove_converter_;
  std::unique_ptr<VlinkConvert> vlink_convert_;
  std::unique_ptr<FoxgloveRpc> rpc_;
  std::unique_ptr<FoxgloveParameters> parameters_;

  mutable std::shared_mutex clients_mtx_;
  std::unordered_map<void*, ClientInfo> clients_;

  struct PublishChannel final {
    std::string topic;
    std::string encoding;
    std::string schema_name;
    std::string schema_encoding;
    std::string schema;
    SchemaType schema_type{SchemaType::kUnknown};
    bool has_route{false};
    CommandRoute route;
  };

  mutable std::shared_mutex channels_mtx_;
  std::unordered_map<uint32_t, ChannelInfo> channels_;
  std::unordered_map<std::string, uint32_t> url_to_channel_id_;
  std::unordered_map<void*, std::unordered_map<uint32_t, PublishChannel>> publish_channels_;

  struct ChannelSubscriber final {
    void* client_ptr{nullptr};
    uint32_t subscription_id{0};
  };

  mutable std::shared_mutex sub_counts_mtx_;
  std::unordered_map<std::string, uint32_t> url_sub_counts_;

  std::unordered_map<uint32_t, std::vector<ChannelSubscriber>> channel_subscribers_;
  mutable std::shared_mutex active_bridge_urls_mtx_;
  std::unordered_set<std::string> active_bridge_urls_;
  std::atomic<uint64_t> active_bridge_urls_generation_{0};

  mutable std::shared_mutex info_mtx_;
  std::unordered_map<std::string, ProxyAPI::Info> last_info_map_;
  Json prev_connection_graph_;

  struct StatusInfo final {
    int level{0};
    std::string message;
  };
  mutable std::shared_mutex status_mtx_;
  std::unordered_map<std::string, StatusInfo> global_statuses_;

  std::atomic<uint32_t> next_channel_id_{1};
  std::atomic<uint64_t> next_client_id_{1};
  std::atomic<uint32_t> client_count_{0};
  std::atomic_bool running_{false};
  std::atomic<uint64_t> last_sys_time_ns_{0};
  std::atomic<uint64_t> session_start_sys_time_ns_{0};
  ElapsedTimer bridge_time_elapsed_{ElapsedTimer::kCpuTimestamp, ElapsedTimer::kNano};
  mutable std::mutex bridge_control_mtx_;
  std::string bridge_control_signature_;
  std::string session_id_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(FoxgloveServer)
};

}  // namespace webviz
}  // namespace vlink
