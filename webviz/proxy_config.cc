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

#include "proxy_config.h"

#include <vlink/base/helpers.h>
#include <vlink/base/utils.h>

#include <algorithm>
#include <cctype>

namespace vlink {
namespace webviz {

void ProxyConfigHelper::add_arguments(argparse::ArgumentParser& program) {
  program.add_argument("--proxy_interface")
      .help("Proxy bridge interface mode: proxy_api or proxy_server")
      .default_value(std::string("proxy_api"));

  program.add_argument("--proxy_role")
      .help("Proxy bridge role: controller or listener")
      .default_value(std::string("controller"));

  program.add_argument("--proxy_domain_id")
      .help("Default DDS domain id for the webviz process")
      .default_value(0)
      .scan<'i', int>();

  program.add_argument("--proxy_dds_impl")
      .help("DDS implementation used by proxy_api channels")
      .default_value(std::string("dds"));

  program.add_argument("--proxy_bind_ip").help("Bind DDS sockets to this IP address").default_value(std::string(""));

  program.add_argument("--proxy_peer_ip").help("Unicast peer IP for DDS discovery").default_value(std::string(""));

  program.add_argument("--proxy_buf_size")
      .help("Socket send/receive buffer size in bytes")
      .default_value(0)
      .scan<'i', int>();

  program.add_argument("--proxy_mtu_size").help("DDS MTU size in bytes").default_value(0).scan<'i', int>();

  program.add_argument("--proxy_native")
      .help("Restrict bridge DDS traffic to loopback")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--proxy_tcp")
      .help("Use TCP transport for DDS channels")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--proxy_key").help("Security key for proxy_api mode").default_value(std::string(""));

  program.add_argument("--proxy_reliable")
      .help("Use reliable proxy_api data channels")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--proxy_direct")
      .help("Use direct SHM data channels in proxy_api mode")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--proxy_no_match_version")
      .help("Disable version matching in proxy_api mode")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--proxy_data_callback_mode")
      .help("Proxy bridge data callback dispatch mode: direct or queued")
      .default_value(std::string("queued"));

  program.add_argument("--proxy_max_packet_size")
      .help("Maximum forwarded payload size in MiB for proxy_server mode")
      .default_value(0.0)
      .scan<'g', double>();

  program.add_argument("--proxy_use_iox")
      .help("Start an embedded Iceoryx RouDi for proxy_server mode")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--proxy_iox_config")
      .help("Iceoryx TOML config path for proxy_server mode")
      .default_value(std::string(""));

  program.add_argument("--proxy_iox_strategy")
      .help("Iceoryx memory strategy for proxy_server mode")
      .default_value(1)
      .scan<'i', int>();

  program.add_argument("--proxy_iox_monitoring")
      .help("Iceoryx monitoring mode for proxy_server mode: on or off")
      .default_value(std::string("on"));
}

bool ProxyConfigHelper::apply_config(const Json& root, const std::filesystem::path& config_file,
                                     const argparse::ArgumentParser& program, ProxyBridge::Config& config,
                                     std::string& error) {
  if VUNLIKELY (!root.contains("proxy")) {
    return true;
  }

  const auto& proxy = root["proxy"];

  if VUNLIKELY (!proxy.is_object()) {
    error = "proxy must be an object";
    return false;
  }

  if VLIKELY (!program.is_used("--proxy_interface") && proxy.contains("interface_mode")) {
    if VUNLIKELY (!proxy["interface_mode"].is_string()) {
      error = "proxy.interface_mode must be a string";
      return false;
    }

    if VUNLIKELY (!ProxyBridge::parse_interface_mode(proxy["interface_mode"].get<std::string>(),
                                                     config.interface_mode)) {
      error = "proxy.interface_mode must be 'proxy_api' or 'proxy_server'";
      return false;
    }
  }

  if VLIKELY (!program.is_used("--proxy_role") && proxy.contains("role")) {
    if VUNLIKELY (!proxy["role"].is_string()) {
      error = "proxy.role must be a string";
      return false;
    }

    if VUNLIKELY (!parse_role(proxy["role"].get<std::string>(), config.transport.role)) {
      error = "proxy.role must be 'controller' or 'listener'";
      return false;
    }
  }

  if VLIKELY (!program.is_used("--proxy_data_callback_mode") && proxy.contains("data_callback_mode")) {
    if VUNLIKELY (!proxy["data_callback_mode"].is_string()) {
      error = "proxy.data_callback_mode must be a string";
      return false;
    }

    if VUNLIKELY (!ProxyBridge::parse_data_callback_mode(proxy["data_callback_mode"].get<std::string>(),
                                                         config.data_callback_mode)) {
      error = "proxy.data_callback_mode must be 'direct' or 'queued'";
      return false;
    }
  }

  if VLIKELY (!program.is_used("--proxy_domain_id") && proxy.contains("domain_id")) {
    if VUNLIKELY (!proxy["domain_id"].is_number_integer()) {
      error = "proxy.domain_id must be an integer";
      return false;
    }

    config.transport.domain_id = proxy["domain_id"].get<int>();
  }

  if VLIKELY (!program.is_used("--proxy_dds_impl") && proxy.contains("dds_impl")) {
    if VUNLIKELY (!proxy["dds_impl"].is_string()) {
      error = "proxy.dds_impl must be a string";
      return false;
    }

    config.transport.dds_impl = normalize_token(proxy["dds_impl"].get<std::string>());
  }

  if VLIKELY (!program.is_used("--proxy_native") && proxy.contains("native")) {
    if VUNLIKELY (!proxy["native"].is_boolean()) {
      error = "proxy.native must be a boolean";
      return false;
    }

    config.transport.native = proxy["native"].get<bool>();
  }

  if VLIKELY (!program.is_used("--proxy_tcp") && proxy.contains("enable_tcp")) {
    if VUNLIKELY (!proxy["enable_tcp"].is_boolean()) {
      error = "proxy.enable_tcp must be a boolean";
      return false;
    }

    config.transport.enable_tcp = proxy["enable_tcp"].get<bool>();
  }

  if VLIKELY (!program.is_used("--proxy_bind_ip") && proxy.contains("bind_ip")) {
    if VUNLIKELY (!proxy["bind_ip"].is_string()) {
      error = "proxy.bind_ip must be a string";
      return false;
    }

    config.transport.bind_ip = proxy["bind_ip"].get<std::string>();
  }

  if VLIKELY (!program.is_used("--proxy_peer_ip") && proxy.contains("peer_ip")) {
    if VUNLIKELY (!proxy["peer_ip"].is_string()) {
      error = "proxy.peer_ip must be a string";
      return false;
    }

    config.transport.peer_ip = proxy["peer_ip"].get<std::string>();
  }

  if VLIKELY (!program.is_used("--proxy_buf_size") && proxy.contains("buf_size")) {
    if VUNLIKELY (!proxy["buf_size"].is_number_integer()) {
      error = "proxy.buf_size must be an integer";
      return false;
    }

    config.transport.buf_size = proxy["buf_size"].get<int>();
  }

  if VLIKELY (!program.is_used("--proxy_mtu_size") && proxy.contains("mtu_size")) {
    if VUNLIKELY (!proxy["mtu_size"].is_number_integer()) {
      error = "proxy.mtu_size must be an integer";
      return false;
    }

    config.transport.mtu_size = proxy["mtu_size"].get<int>();
  }

  if VLIKELY (proxy.contains("api")) {
    const auto& api = proxy["api"];

    if VUNLIKELY (!api.is_object()) {
      error = "proxy.api must be an object";
      return false;
    }

    if VLIKELY (!program.is_used("--proxy_key") && api.contains("security_key")) {
      if VUNLIKELY (!api["security_key"].is_string()) {
        error = "proxy.api.security_key must be a string";
        return false;
      }

      config.api.security_key = api["security_key"].get<std::string>();
    }

    if VLIKELY (!program.is_used("--proxy_reliable") && api.contains("reliable")) {
      if VUNLIKELY (!api["reliable"].is_boolean()) {
        error = "proxy.api.reliable must be a boolean";
        return false;
      }

      config.api.reliable = api["reliable"].get<bool>();
    }

    if VLIKELY (!program.is_used("--proxy_direct") && api.contains("direct")) {
      if VUNLIKELY (!api["direct"].is_boolean()) {
        error = "proxy.api.direct must be a boolean";
        return false;
      }

      config.api.direct = api["direct"].get<bool>();
    }

    if VLIKELY (!program.is_used("--proxy_no_match_version") && api.contains("match_version")) {
      if VUNLIKELY (!api["match_version"].is_boolean()) {
        error = "proxy.api.match_version must be a boolean";
        return false;
      }

      config.api.match_version = api["match_version"].get<bool>();
    }
  }

  if VLIKELY (proxy.contains("server")) {
    const auto& server = proxy["server"];

    if VUNLIKELY (!server.is_object()) {
      error = "proxy.server must be an object";
      return false;
    }

    if VLIKELY (!program.is_used("--proxy_max_packet_size") && server.contains("max_packet_size")) {
      if VUNLIKELY (!server["max_packet_size"].is_number()) {
        error = "proxy.server.max_packet_size must be a number";
        return false;
      }

      config.server.max_packet_size = server["max_packet_size"].get<double>();
    }

    if VLIKELY (!program.is_used("--proxy_use_iox") && server.contains("use_iox")) {
      if VUNLIKELY (!server["use_iox"].is_boolean()) {
        error = "proxy.server.use_iox must be a boolean";
        return false;
      }

      config.server.use_iox = server["use_iox"].get<bool>();
    }

    if VLIKELY (!program.is_used("--proxy_iox_config") && server.contains("iox_config")) {
      if VUNLIKELY (!server["iox_config"].is_string()) {
        error = "proxy.server.iox_config must be a string";
        return false;
      }

      config.server.iox_config = server["iox_config"].get<std::string>();

      if VLIKELY (!config.server.iox_config.empty() && !std::filesystem::path(config.server.iox_config).is_absolute() &&
                  !config_file.empty()) {
        config.server.iox_config =
            Helpers::path_to_string(config_file.parent_path() / std::filesystem::path(config.server.iox_config));
      }
    }

    if VLIKELY (!program.is_used("--proxy_iox_strategy") && server.contains("iox_strategy")) {
      if VUNLIKELY (!server["iox_strategy"].is_number_integer()) {
        error = "proxy.server.iox_strategy must be an integer";
        return false;
      }

      config.server.iox_strategy = server["iox_strategy"].get<int>();
    }

    if VLIKELY (!program.is_used("--proxy_iox_monitoring") && server.contains("iox_monitoring")) {
      if VLIKELY (server["iox_monitoring"].is_boolean()) {
        config.server.iox_monitoring = server["iox_monitoring"].get<bool>();
      } else if VLIKELY (server["iox_monitoring"].is_string()) {
        if VUNLIKELY (!parse_iox_monitoring(server["iox_monitoring"].get<std::string>(),
                                            config.server.iox_monitoring)) {
          error = "proxy.server.iox_monitoring must be true/false or 'on'/'off'";
          return false;
        }
      } else {
        error = "proxy.server.iox_monitoring must be true/false or 'on'/'off'";
        return false;
      }
    }
  }

  return true;
}

bool ProxyConfigHelper::apply_arguments(const argparse::ArgumentParser& program, ProxyBridge::Config& config,
                                        std::string& error) {
  if VUNLIKELY (program.is_used("--proxy_interface")) {
    if VUNLIKELY (!ProxyBridge::parse_interface_mode(program.get<std::string>("--proxy_interface"),
                                                     config.interface_mode)) {
      error = "Invalid --proxy_interface, expected 'proxy_api' or 'proxy_server'";
      return false;
    }
  }

  if VUNLIKELY (program.is_used("--proxy_role")) {
    if VUNLIKELY (!parse_role(program.get<std::string>("--proxy_role"), config.transport.role)) {
      error = "Invalid --proxy_role, expected 'controller' or 'listener'";
      return false;
    }
  }

  if VUNLIKELY (program.is_used("--proxy_data_callback_mode")) {
    if VUNLIKELY (!ProxyBridge::parse_data_callback_mode(program.get<std::string>("--proxy_data_callback_mode"),
                                                         config.data_callback_mode)) {
      error = "Invalid --proxy_data_callback_mode, expected 'direct' or 'queued'";
      return false;
    }
  }

  if VUNLIKELY (program.is_used("--proxy_domain_id")) {
    config.transport.domain_id = program.get<int>("--proxy_domain_id");
  }

  if VUNLIKELY (program.is_used("--proxy_dds_impl")) {
    config.transport.dds_impl = normalize_token(program.get<std::string>("--proxy_dds_impl"));
  }

  if VUNLIKELY (program.is_used("--proxy_native")) {
    config.transport.native = program.get<bool>("--proxy_native");
  }

  if VUNLIKELY (program.is_used("--proxy_tcp")) {
    config.transport.enable_tcp = program.get<bool>("--proxy_tcp");
  }

  if VUNLIKELY (program.is_used("--proxy_bind_ip")) {
    config.transport.bind_ip = program.get<std::string>("--proxy_bind_ip");
  }

  if VUNLIKELY (program.is_used("--proxy_peer_ip")) {
    config.transport.peer_ip = program.get<std::string>("--proxy_peer_ip");
  }

  if VUNLIKELY (program.is_used("--proxy_buf_size")) {
    config.transport.buf_size = program.get<int>("--proxy_buf_size");
  }

  if VUNLIKELY (program.is_used("--proxy_mtu_size")) {
    config.transport.mtu_size = program.get<int>("--proxy_mtu_size");
  }

  if VUNLIKELY (program.is_used("--proxy_key")) {
    config.api.security_key = program.get<std::string>("--proxy_key");
  }

  if VUNLIKELY (program.is_used("--proxy_reliable")) {
    config.api.reliable = program.get<bool>("--proxy_reliable");
  }

  if VUNLIKELY (program.is_used("--proxy_direct")) {
    config.api.direct = program.get<bool>("--proxy_direct");
  }

  if VUNLIKELY (program.is_used("--proxy_no_match_version")) {
    config.api.match_version = !program.get<bool>("--proxy_no_match_version");
  }

  if VUNLIKELY (program.is_used("--proxy_max_packet_size")) {
    config.server.max_packet_size = program.get<double>("--proxy_max_packet_size");
  }

  if VUNLIKELY (program.is_used("--proxy_use_iox")) {
    config.server.use_iox = program.get<bool>("--proxy_use_iox");
  }

  if VUNLIKELY (program.is_used("--proxy_iox_config")) {
    config.server.iox_config = program.get<std::string>("--proxy_iox_config");
  }

  if VUNLIKELY (program.is_used("--proxy_iox_strategy")) {
    config.server.iox_strategy = program.get<int>("--proxy_iox_strategy");
  }

  if VUNLIKELY (program.is_used("--proxy_iox_monitoring")) {
    if VUNLIKELY (!parse_iox_monitoring(program.get<std::string>("--proxy_iox_monitoring"),
                                        config.server.iox_monitoring)) {
      error = "Invalid --proxy_iox_monitoring, expected 'on' or 'off'";
      return false;
    }
  }

  return true;
}

bool ProxyConfigHelper::validate(const ProxyBridge::Config& config, std::string& error) {
  if VUNLIKELY (config.transport.domain_id < 0 || config.transport.domain_id > 255) {
    error = "proxy_domain_id must be in [0, 255]";
    return false;
  }

  if VUNLIKELY (config.transport.buf_size < 0) {
    error = "proxy_buf_size must be >= 0";
    return false;
  }

  if VUNLIKELY (config.transport.mtu_size < 0) {
    error = "proxy_mtu_size must be >= 0";
    return false;
  }

  if VUNLIKELY (!config.transport.dds_impl.empty() && !is_valid_dds_impl(config.transport.dds_impl)) {
    error = "proxy_dds_impl must be one of: dds, ddsc, ddsr, ddst";
    return false;
  }

  if VUNLIKELY (config.interface_mode == ProxyBridge::kProxyServer && config.transport.role != ProxyAPI::kController) {
    error = "proxy_server mode only supports proxy_role=controller";
    return false;
  }

  if VUNLIKELY (config.server.max_packet_size < 0.0) {
    error = "proxy_max_packet_size must be >= 0";
    return false;
  }

  if VUNLIKELY (config.server.iox_strategy < 1 || config.server.iox_strategy > 3) {
    error = "proxy_iox_strategy must be 1, 2, or 3";
    return false;
  }

  if VUNLIKELY (config.interface_mode == ProxyBridge::kProxyServer && config.api.reliable) {
    error = "proxy_api.reliable is only valid in proxy_api mode";
    return false;
  }

  if VUNLIKELY (config.interface_mode == ProxyBridge::kProxyServer && config.api.direct) {
    error = "proxy_api.direct is only valid in proxy_api mode";
    return false;
  }

#ifndef VLINK_SUPPORT_SHM

  if VUNLIKELY (config.interface_mode == ProxyBridge::kProxyServer && config.server.use_iox) {
    error = "proxy_server.use_iox requires VLINK_SUPPORT_SHM";
    return false;
  }
#endif

  return true;
}

std::string ProxyConfigHelper::normalize_token(std::string_view value) {
  std::string normalized(value);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return normalized;
}

bool ProxyConfigHelper::is_valid_dds_impl(std::string_view value) {
  const auto normalized = normalize_token(value);
  return normalized == "dds" || normalized == "ddsc" || normalized == "ddsr" || normalized == "ddst";
}

bool ProxyConfigHelper::parse_role(std::string_view value, ProxyAPI::Role& role) {
  const auto normalized = normalize_token(value);

  if VLIKELY (normalized == "controller") {
    role = ProxyAPI::kController;
    return true;
  }

  if VLIKELY (normalized == "listener") {
    role = ProxyAPI::kListener;
    return true;
  }

  return false;
}

bool ProxyConfigHelper::parse_iox_monitoring(std::string_view value, bool& enabled) {
  const auto normalized = normalize_token(value);

  if VLIKELY (normalized == "on") {
    enabled = true;
    return true;
  }

  if VLIKELY (normalized == "off") {
    enabled = false;
    return true;
  }

  return false;
}

}  // namespace webviz
}  // namespace vlink
