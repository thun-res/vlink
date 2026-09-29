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

#include <vlink/base/helpers.h>
#include <vlink/base/logger.h>
#include <vlink/base/utils.h>
#include <vlink/extension/discovery_viewer.h>
#include <vlink/external/proxy_server.h>
#include <vlink/version.h>

#include <argparse/argparse.hpp>
#include <nlohmann/json.hpp>
//
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

static constexpr auto kMaxPacketSizeMb =
    (static_cast<double>(std::numeric_limits<size_t>::max()) + 1.0) / (1024.0 * 1024.0);

template <typename ValueT>
static bool read_config_option(const argparse::ArgumentParser& program, const nlohmann::json& root, const char* key,
                               ValueT& value) {
  const std::string option = std::string("--") + key;
  const auto iter = root.find(key);

  if (program.is_used(option) || iter == root.end()) {
    value = program.get<ValueT>(option);
    return true;
  }

  if constexpr (std::is_integral_v<ValueT> && !std::is_same_v<ValueT, bool>) {
    if VUNLIKELY (!iter->is_number_integer()) {
      return false;
    }

    if (iter->is_number_unsigned()) {
      if VUNLIKELY (iter->get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<ValueT>::max())) {
        return false;
      }
    } else {
      const auto number = iter->get<int64_t>();

      if VUNLIKELY (number < static_cast<int64_t>(std::numeric_limits<ValueT>::min()) ||
                    number > static_cast<int64_t>(std::numeric_limits<ValueT>::max())) {
        return false;
      }
    }
  }

  try {
    iter->get_to(value);
    return true;
  } catch (const nlohmann::json::exception&) {
    return false;
  }
}

int main(int argc, char* argv[]) {
  vlink::Utils::set_console_utf8_output();

  // arg parser
  argparse::ArgumentParser program("vlink-proxy", VLINK_VERSION, argparse::default_arguments::all);

  program.add_description("Note: You may need to add multicast/broadcast [" +
                          vlink::DiscoveryViewer::get_listen_address() + ":" +
                          std::to_string(vlink::DiscoveryViewer::get_listen_port()) + "]");

  program.add_argument("-a", "--async").help("Async mode").default_value(false).implicit_value(true);
  program.add_argument("-r", "--reliable").help("Reliable mode").default_value(false).implicit_value(true);
  program.add_argument("-t", "--tcp").help("Tcp mode").default_value(false).implicit_value(true);
  program.add_argument("-g", "--direct").help("Direct mode").default_value(false).implicit_value(true);
  program.add_argument("-d", "--domain_id")
      .help("Domain id(0~255)")
      .scan<'d', int>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<int>(0));
  program.add_argument("-k", "--key").help("Security key").default_value(std::string());
  program.add_argument("-b", "--bind_ip").help("Bind ip address").default_value(std::string());
  program.add_argument("-p", "--peer_ip").help("Peer ip address").default_value(std::string());
  program.add_argument("-s", "--buf_size")
      .help("Set DDS(TX/RX) buffer size")
      .scan<'d', uint32_t>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<uint32_t>(0U));
  program.add_argument("-e", "--mtu_size")
      .help("Set DDS MTU size")
      .scan<'d', uint32_t>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<uint32_t>(0U));
  program.add_argument("-n", "--native").help("Native mode").default_value(false).implicit_value(true);
  program.add_argument("--config").help("Proxy JSON configuration file").default_value(std::string());
  program.add_argument("-x", "--max_packet_size")
      .help("Max packet size(MB)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(4.0));
  program.add_argument("-c", "--iox_config").help("IOX config path").default_value(std::string());
  program.add_argument("-l", "--iox_strategy")
      .help("IOX Memory Strategy (1: Mini, 2: Low, 3: Middle, 4: High)")
      .scan<'d', int>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<int>(3));
  program.add_argument("-m", "--iox_monitoring")
      .help("IOX enable monitoring mode('on'/'off'), default is 'on'")
      .default_value(std::string("on"));

#if defined(VLINK_SUPPORT_DDS)
  program.add_argument("--dds_impl").help("Select dds type('dds'/'ddsc')").default_value(std::string("dds"));
#elif defined(VLINK_SUPPORT_DDSC)
  program.add_argument("--dds_impl").help("Select dds type('dds'/'ddsc')").default_value(std::string("ddsc"));
#else
  program.add_argument("--dds_impl").help("Select dds type('dds'/'ddsc')").default_value(std::string(""));
#endif

  program.add_argument("--bridge_domain_id")
      .help("Bridge a remote proxy domain (0~255) as Controller, preserving selected topic URLs")
      .scan<'d', int>();
  program.add_argument("--bridge_security_key").help("Remote proxy security key").default_value(std::string());
  program.add_argument("--bridge_filter")
      .help("Source URL substrings, separated by spaces or commas (case-insensitive)")
      .default_value(std::string("shm://,shm2://,intra://"));
  program.add_argument("--bridge_subscribe")
      .help("Subscribe to local publishers and forward to discovered remote subscribers")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--bridge_allow_ip").help("Bridge local DDS bind IP").default_value(std::string());
  program.add_argument("--bridge_peer_ip").help("Remote proxy discovery IP").default_value(std::string());
  program.add_argument("--bridge_dds_impl")
      .help("Remote DDS implementation (default: --dds_impl)")
      .default_value(std::string());
  program.add_argument("--bridge_reliable")
      .help("Remote proxy uses reliable data transport")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--bridge_enable_tcp")
      .help("Remote proxy uses TCP data transport")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--runnable")
      .help("Load runnable plugins")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);

  program.add_epilog("Example:\n  vlink-proxy -l 3");

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }

  vlink::ProxyServer::Config proxy_config;
  auto config_path = std::filesystem::path(program.get<std::string>("--config"));
  auto root = nlohmann::json::object();

  if (program.is_used("--config")) {
    std::ifstream file(config_path);

    if VUNLIKELY (!file.is_open()) {
      std::cerr << "Cannot open proxy config file." << std::endl;
      return 1;
    }

    try {
      file >> root;
    } catch (const nlohmann::json::parse_error& error) {
      std::cerr << "Invalid proxy JSON at byte " << error.byte << "." << std::endl;
      return 1;
    } catch (const nlohmann::json::exception&) {
      std::cerr << "Invalid proxy JSON value." << std::endl;
      return 1;
    }

    if VUNLIKELY (!root.is_object()) {
      std::cerr << "Proxy config root must be a JSON object." << std::endl;
      return 1;
    }
  }

  const auto read = [&](const char* key, auto& value) {
    if VUNLIKELY (!read_config_option(program, root, key, value)) {
      std::cerr << "Invalid proxy config field: " << key << std::endl;
      return false;
    }

    return true;
  };
  std::string iox_monitoring;

  if VUNLIKELY (!read("async", proxy_config.async) || !read("reliable", proxy_config.reliable) ||
                !read("tcp", proxy_config.enable_tcp) || !read("direct", proxy_config.direct) ||
                !read("domain_id", proxy_config.domain_id) || !read("key", proxy_config.security_key) ||
                !read("bind_ip", proxy_config.bind_ip) || !read("peer_ip", proxy_config.peer_ip) ||
                !read("buf_size", proxy_config.buf_size) || !read("mtu_size", proxy_config.mtu_size) ||
                !read("native", proxy_config.native_mode) || !read("max_packet_size", proxy_config.max_packet_size) ||
                !read("dds_impl", proxy_config.dds_impl) || !read("iox_config", proxy_config.iox_config) ||
                !read("iox_strategy", proxy_config.iox_strategy) || !read("iox_monitoring", iox_monitoring) ||
                !read("runnable", proxy_config.runnable_list)) {
    return 1;
  }

  proxy_config.use_iox =
      program.is_used("-c") || program.is_used("-l") || root.contains("iox_config") || root.contains("iox_strategy");

  vlink::ProxyAPI::Config remote;

  if VUNLIKELY (!read("bridge_security_key", remote.security_key) || !read("bridge_allow_ip", remote.allow_ip) ||
                !read("bridge_peer_ip", remote.peer_ip) || !read("bridge_dds_impl", remote.dds_impl) ||
                !read("bridge_reliable", remote.reliable) || !read("bridge_enable_tcp", remote.enable_tcp) ||
                !read("bridge_filter", proxy_config.bridge_filter) ||
                !read("bridge_subscribe", proxy_config.bridge_subscribe)) {
    return 1;
  }

  if (program.is_used("--bridge_domain_id") || root.contains("bridge_domain_id")) {
    if VUNLIKELY (!read("bridge_domain_id", remote.domain_id)) {
      return 1;
    }

    if (remote.dds_impl.empty()) {
      remote.dds_impl = proxy_config.dds_impl;
    }

    if VUNLIKELY (remote.domain_id < 0 || remote.domain_id > 255 || remote.domain_id == proxy_config.domain_id) {
      std::cerr << "Bridge requires a different remote domain (0~255)." << std::endl;
      return 1;
    }

    if VUNLIKELY (proxy_config.direct) {
      std::cerr << "Bridge requires non-direct transport on both proxies." << std::endl;
      return 1;
    }

    proxy_config.bridge = std::move(remote);
  }

  if VUNLIKELY (proxy_config.domain_id < 0 || proxy_config.domain_id > 255) {
    std::cerr << "Invalid domain id." << std::endl;
    std::cerr << program << std::endl;
    return 1;
  }

  if VUNLIKELY (proxy_config.iox_strategy < 1 || proxy_config.iox_strategy > 4) {
    std::cerr << "Invalid IOX memory strategy." << std::endl;
    std::cerr << program << std::endl;
    return 1;
  }

  if VUNLIKELY (!std::isfinite(proxy_config.max_packet_size) || proxy_config.max_packet_size < 0.0 ||
                proxy_config.max_packet_size >= kMaxPacketSizeMb) {
    std::cerr << "Invalid max packet size." << std::endl;
    std::cerr << program << std::endl;
    return 1;
  }

  try {
    if (!program.is_used("-c") && !proxy_config.iox_config.empty()) {
      proxy_config.iox_config =
          vlink::Helpers::path_to_string(config_path.parent_path() / std::filesystem::u8path(proxy_config.iox_config));
    }
#ifdef _WIN32
    else {
      proxy_config.iox_config = vlink::Helpers::path_to_string(std::filesystem::path(proxy_config.iox_config));
    }
#endif
  } catch (std::filesystem::filesystem_error& e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }

#ifdef VLINK_SUPPORT_SHM
  if VLIKELY (iox_monitoring == "on" || iox_monitoring == "ON" || iox_monitoring == "On") {
    proxy_config.iox_monitoring = true;
  } else if (iox_monitoring == "off" || iox_monitoring == "OFF" || iox_monitoring == "Off") {
    proxy_config.iox_monitoring = false;
  } else {
    std::cerr << "Invalid input for [-m, --iox_monitoring]." << std::endl;
    std::cerr << program << std::endl;
    return 1;
  }
#else
  (void)iox_monitoring;

  if VUNLIKELY (proxy_config.use_iox) {
    std::cerr << "RouDi for shm is not supported." << std::endl;
    return 1;
  }
#endif

  if VUNLIKELY (!vlink::Utils::check_singleton("vlink-proxy-" + std::to_string(proxy_config.domain_id))) {
    std::cerr << "A proxy for this domain has already started." << std::endl;
    return 1;
  }

  // init
  // vlink::Logger::set_console_level(vlink::Logger::kOff);
  // vlink::Logger::set_file_level(vlink::Logger::kOff);
  vlink::Logger::init("vlink-proxy");

  // env
  vlink::Utils::unset_env("VLINK_BAG_PATH");
  // vlink::Utils::set_env("VLINK_DISCOVER_DISABLE", "1");

  CLOG_I("Start proxy. [Domain id: %d].", proxy_config.domain_id);

  vlink::ProxyServer proxy_server(proxy_config);

  vlink::Utils::register_terminate_signal([&proxy_server](int) { proxy_server.quit(true); });

  proxy_server.run();

  return 0;
}
