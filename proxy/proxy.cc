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
//
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
  vlink::Utils::set_console_utf8_output();

  // arg parser
  argparse::ArgumentParser program("vlink-proxy", VLINK_VERSION, argparse::default_arguments::all);

  program.add_description("Note: You may need to add multicast/broadcast [" +
                          vlink::DiscoveryViewer::get_listen_address() + "]");

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
  program.add_argument("-x", "--max_packet_size")
      .help("Max packet size(MB)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(4.0));
  program.add_argument("-c", "--iox_config").help("IOX config path").default_value(std::string());
  program.add_argument("-l", "--iox_strategy")
      .help("IOX Memory Strategy (1: Low memory, 2: Middle memory 3: High memory)")
      .scan<'d', int>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<int>(2));
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

  program.add_argument("--runnable")
      .help("Load runnable plugins")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);

  program.add_epilog("Example:\n  vlink-proxy -c");

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }

  // check singleton

  if VUNLIKELY (!vlink::Utils::check_singleton("vlink-proxy")) {
    std::cerr << "Program has started." << std::endl;
    return 1;
  }

  vlink::ProxyServer::Config proxy_config;

  proxy_config.async = program.is_used("-a");
  proxy_config.reliable = program.is_used("-r");
  proxy_config.enable_tcp = program.is_used("-t");
  proxy_config.direct = program.is_used("-g");
  proxy_config.domain_id = program.get<int>("-d");
  proxy_config.security_key = program.get<std::string>("-k");
  proxy_config.bind_ip = program.get<std::string>("-b");
  proxy_config.peer_ip = program.get<std::string>("-p");
  proxy_config.buf_size = program.get<uint32_t>("-s");
  proxy_config.mtu_size = program.get<uint32_t>("-e");
  proxy_config.native_mode = program.is_used("-n");
  proxy_config.max_packet_size = program.get<double>("-x");
  proxy_config.dds_impl = program.get<std::string>("--dds_impl");

  proxy_config.use_iox = program.is_used("-c");
  proxy_config.iox_config = program.get<std::string>("-c");
  proxy_config.iox_strategy = program.get<int>("-l");
  auto iox_monitoring = program.get<std::string>("-m");

  proxy_config.runnable_list = program.get<std::vector<std::string>>("--runnable");

  if VUNLIKELY (proxy_config.domain_id < 0 || proxy_config.domain_id > 255) {
    std::cerr << "Invalid domain id." << std::endl;
    std::cerr << program << std::endl;
    return 1;
  }

#ifdef _WIN32
  try {
    proxy_config.iox_config = vlink::Helpers::path_to_string(std::filesystem::path(proxy_config.iox_config));
  } catch (std::filesystem::filesystem_error& e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }
#endif

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
