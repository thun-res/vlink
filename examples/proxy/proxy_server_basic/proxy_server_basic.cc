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
// File: proxy_server_basic.cc
//
// Server-side demo of vlink::ProxyServer. ProxyServer is the in-process
// daemon that discovers every vlink Publisher/Subscriber/Server/Client/
// Setter/Getter on the configured DDS domain, exposes a control + info +
// data channel to external ProxyAPI clients, and (optionally) hosts a list
// of runnable plugins inside its own thread.
//
// Hard rule: AT MOST ONE ProxyServer instance per process. Two would compete
// for the same DDS topic names and corrupt the control protocol. The class
// enforces this with an internal singleton guard.
//
// Lifetime:
//   construct(cfg)   -- wires up DDS endpoints, plugin list, internal loop
//   async_run()      -- spawns the worker thread; heartbeat (1Hz) + ProxyAPI
//                       handshake start running immediately
//   quit(drain=true) -- ask the loop to stop; "true" lets the heartbeat
//                       flush a final "going away" packet so connected
//                       ProxyAPI clients get a clean disconnect
//   wait_for_quit()  -- join the worker thread
// =============================================================================

#include <vlink/base/logger.h>
#include <vlink/external/proxy_server.h>

#include <chrono>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

int main() {
  // Section: build the minimal server config.
  //   dds_impl  -- which DDS provider to use ("dds", "ddsc", "ddsr", "ddst").
  //   domain_id -- DDS domain shared with the ProxyAPI clients we want to
  //                serve.
  //   reliable  -- DDS reliability on the data relay; off for low-latency.
  //   async     -- whether the internal MessageLoop runs on its own thread
  //                (true) or on the caller's (false; for embedding).
  //   use_iox   -- enable iceoryx shared-memory transport for the relay.
  vlink::ProxyServer::Config cfg;
  cfg.dds_impl = "dds";
  cfg.domain_id = 0;
  cfg.reliable = false;
  cfg.async = true;
  cfg.use_iox = false;

  VLOG_I("ProxyServer config: dds_impl=", cfg.dds_impl, " domain_id=", cfg.domain_id, " reliable=", cfg.reliable,
         " async=", cfg.async);

  // Section: construct + start. Construction registers the singleton; a
  // second ProxyServer here would terminate the process.
  vlink::ProxyServer server(cfg);
  server.async_run();
  VLOG_I("ProxyServer event loop started -- heartbeat + discovery active");

  // Let the server publish a few heartbeats so ProxyAPI clients in the same
  // domain have a chance to discover and connect.
  std::this_thread::sleep_for(500ms);

  // Section: shutdown. quit(true) drains the outbound queue so the last
  // heartbeat / "bye" packet reaches the wire before the thread exits.
  server.quit(true);
  server.wait_for_quit();
  VLOG_I("ProxyServer event loop stopped");

  // Section: illustrate the runnable_list field. ProxyServer can also host
  // a set of RunablePluginInterface plugins automatically: it
  // dlopen+create+async_run+on_init each plugin during construction and
  // tears them down (on_deinit, quit, wait_for_quit, dlclose) at shutdown,
  // so the user does not have to manage plugin lifetime by hand. The names
  // listed here would be searched in default_search_path with the version
  // major/minor required by runnable_version_*.
  vlink::ProxyServer::Config plugin_cfg;
  plugin_cfg.dds_impl = "dds";
  plugin_cfg.runnable_list = {"my_analysis_plugin", "my_recorder_plugin"};
  plugin_cfg.runnable_version_major = 1;
  plugin_cfg.runnable_version_minor = 0;

  VLOG_I("Example runnable_list size: ", plugin_cfg.runnable_list.size());
  for (const auto& name : plugin_cfg.runnable_list) {
    VLOG_I("  plugin: ", name, " v", plugin_cfg.runnable_version_major, ".", plugin_cfg.runnable_version_minor);
  }

  VLOG_I("ProxyServer basic example complete.");
  return 0;
}
