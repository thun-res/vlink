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

// =============================================================================
// File: actuator_node.cc
//
// Example 2 -- Standalone external actuator process managed by vlink-launcher.
//
// Reads its subscribed control URL and RPC status endpoint from --in / --rpc or
// launcher-injected VLINK_INPUT_CMD_VEL / VLINK_URL_STATUS_RPC environment
// variables (declared in hybrid_dataflow.toml), applies incoming commands, and
// serves health queries from SafetySupervisor via vlink::Server.
// =============================================================================

#include <vlink/base/macros.h>
#include <vlink/base/utils.h>
#include <vlink/server.h>
#include <vlink/subscriber.h>
//
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

static std::atomic_bool g_stop{false};

int main(int argc, char** argv) {
  std::string cmd_url = vlink::Utils::get_env("VLINK_INPUT_CMD_VEL", "");
  std::string rpc_url = vlink::Utils::get_env("VLINK_URL_STATUS_RPC", "");

  // Section: parse CLI URL arguments supplied by hybrid_dataflow.toml.
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];

    if (arg == "--in" && i + 1 < argc) {
      cmd_url = argv[++i];
    } else if (arg == "--rpc" && i + 1 < argc) {
      rpc_url = argv[++i];
    }
  }

  if (cmd_url.empty()) {
    cmd_url = "dds://hybrid/control/cmd_vel";
  }

  if (rpc_url.empty()) {
    rpc_url = "dds://hybrid/actuator/status";
  }

  vlink::Utils::register_terminate_signal([](int) { g_stop.store(true, std::memory_order_release); }, false, false);

  std::atomic<uint64_t> applied{0U};

  // Section: subscribe to control commands and serve status RPC queries.
  vlink::Subscriber<std::string> sub(cmd_url);
  sub.listen([&applied](const std::string& cmd) {
    const uint64_t count = applied.fetch_add(1U, std::memory_order_relaxed) + 1U;

    if (count % 5U == 1U) {
      std::cout << "[actuator] applied #" << count << ": " << cmd << "\n" << std::flush;
    }
  });

  vlink::Server<std::string, std::string> rpc_server(rpc_url);
  rpc_server.listen([&applied](const std::string& req, std::string& resp) {
    resp = "OK applied=" + std::to_string(applied.load(std::memory_order_relaxed)) + " req=" + req;
  });

  std::cout << "READY actuator in=" << cmd_url << " rpc=" << rpc_url << "\n" << std::flush;

  while
    VLIKELY(!g_stop.load(std::memory_order_acquire)) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }

  vlink::Utils::register_terminate_signal(nullptr, false, false);
  return 0;
}
