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

#include <vlink/base/functional.h>
#include <vlink/base/macros.h>
#include <vlink/external/proxy_api.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace vlink {
class MessageLoop;

namespace webviz {

class ProxyBridge {
 public:
  enum InterfaceMode : uint8_t {
    kProxyApi = 0,
    kProxyServer = 1,
  };

  enum DataCallbackMode : uint8_t {
    kDirect = 0,
    kQueued = 1,
  };

  struct TransportConfig final {
    ProxyAPI::Role role{ProxyAPI::kController};
    int domain_id{0};
    std::string dds_impl{"dds"};
    bool native{false};
    bool enable_tcp{false};
    std::string bind_ip;
    std::string peer_ip;
    int buf_size{0};
    int mtu_size{0};
  };

  struct ApiConfig final {
    std::string security_key;
    bool reliable{false};
    bool direct{false};
    bool match_version{true};
  };

  struct ServerConfig final {
    double max_packet_size{0.0};
    bool use_iox{false};
    bool iox_monitoring{true};
    int iox_strategy{1};
    std::string iox_config;
  };

  struct Config final {
    InterfaceMode interface_mode{kProxyApi};
    DataCallbackMode data_callback_mode{kQueued};
    TransportConfig transport;
    ApiConfig api;
    ServerConfig server;
  };

  using ConnectCallback = MoveFunction<void(bool connected)>;
  using ErrorCallback = MoveFunction<void(ProxyAPI::Error error)>;
  using TimeCallback = MoveFunction<void(uint64_t sys_time, uint64_t boot_time)>;
  using InfoCallback = MoveFunction<void(const std::vector<ProxyAPI::Info>& info_list)>;
  using DataCallback = MoveFunction<void(const ProxyAPI::Data& data)>;

  ProxyBridge(const Config& config, MessageLoop* data_callback_loop);

  virtual ~ProxyBridge();

  static std::unique_ptr<ProxyBridge> create(const Config& config, MessageLoop* data_callback_loop);

  static const char* to_string(InterfaceMode mode);

  static bool parse_interface_mode(std::string_view value, InterfaceMode& mode);

  static bool parse_data_callback_mode(std::string_view value, DataCallbackMode& mode);

  static bool is_dds_transport(std::string_view url);

  template <typename NodeT>
  static void apply_transport(NodeT& node, const TransportConfig& config, bool disable_discovery = false) {
    if VUNLIKELY (disable_discovery) {
      node.set_discovery_enabled(false);
    }

    const auto& url = node.get_url();

    if VUNLIKELY (!is_dds_transport(url)) {
      return;
    }

    if VUNLIKELY (config.native) {
      node.set_property("dds.ip", "127.0.0.1");
    } else if VLIKELY (!config.bind_ip.empty()) {
      node.set_property("dds.ip", config.bind_ip);
    }

    if VLIKELY (!config.peer_ip.empty()) {
      node.set_property("dds.peer", config.peer_ip);
    }

    if VLIKELY (config.buf_size > 0) {
      node.set_property("dds.buf", std::to_string(config.buf_size));
    }

    if VLIKELY (config.mtu_size > 0) {
      node.set_property("dds.mtu", std::to_string(config.mtu_size));
    }

    node.set_property("dds.tcp", config.enable_tcp ? "1" : "0");
  }

  virtual bool start() = 0;

  virtual void stop() = 0;

  virtual void register_connect_callback(ConnectCallback&& callback) = 0;

  virtual void register_error_callback(ErrorCallback&& callback) = 0;

  virtual void register_time_callback(TimeCallback&& callback) = 0;

  virtual void register_info_callback(InfoCallback&& callback) = 0;

  virtual void register_data_callback(DataCallback&& callback);

  virtual bool send_control(const ProxyAPI::Control& control, bool async = true) = 0;

  virtual bool send_data(const ProxyAPI::Data& data) = 0;

  virtual const Config& get_config() const = 0;

  virtual bool can_control() const = 0;

  virtual bool can_inject() const = 0;

  virtual bool is_connected() const = 0;

  virtual std::string get_proxy_version() const = 0;

  virtual std::unordered_set<std::string> get_proxy_hostnames() const = 0;

 protected:
  void dispatch_data_callback(const ProxyAPI::Data& data);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(ProxyBridge)
};

}  // namespace webviz
}  // namespace vlink
