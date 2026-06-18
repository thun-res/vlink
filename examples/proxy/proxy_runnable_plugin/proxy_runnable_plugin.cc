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
// File: proxy_runnable_plugin.cc
//
// Shows two ways the same RunablePluginInterface plugin can be hosted:
//
//   A) Declaratively via ProxyServer::Config::runnable_list. The plugin
//      names listed there are dlopen'd, instantiated, async_run'd and
//      on_init'd by ProxyServer as part of its startup; on_deinit/quit/
//      wait_for_quit/dlclose run during shutdown. The user code never
//      touches the plugin handle -- ProxyServer owns lifetime end-to-end.
//
//   B) Standalone via vlink::Plugin, exactly like the plugin_runnable demo.
//      Useful for unit tests or for embedding the plugin without a server.
//
// This file demonstrates path (A) by *building* the config (it does not
// spin up a ProxyServer because the test environment may not have DDS
// available) and demonstrates path (B) by actually loading the plugin and
// running its lifecycle manually.
// =============================================================================

#include <vlink/base/logger.h>
#include <vlink/base/plugin.h>
#include <vlink/extension/runnable_plugin_interface.h>
#include <vlink/external/proxy_server.h>

#include <chrono>
#include <thread>

int main() {
  // Section: (A) build a ProxyServer config that would host the plugin.
  // runnable_list is a list of base names (no "lib", no ".so"). The
  // (runnable_version_major, runnable_version_minor) pair must match the
  // VLINK_PLUGIN_DECLARE(..., major, minor) inside each .so.
  vlink::ProxyServer::Config cfg;
  cfg.dds_impl = "dds";
  cfg.domain_id = 0;
  cfg.async = true;
  cfg.runnable_list = {"monitor_plugin"};
  cfg.runnable_version_major = 1;
  cfg.runnable_version_minor = 0;

  VLOG_I("Config runnable_list size: ", cfg.runnable_list.size());
  for (const auto& name : cfg.runnable_list) {
    VLOG_I("  plugin: ", name, " v", cfg.runnable_version_major, ".", cfg.runnable_version_minor);
  }

  // Section: (B) standalone load + manual lifecycle. Mirrors what
  // ProxyServer does under the hood for every name in runnable_list.
  vlink::Plugin plugin;
  auto instance = plugin.load<vlink::RunablePluginInterface>("monitor_plugin", 1, 0);

  if (instance) {
    VLOG_I("Standalone load OK");

    // async_run -> spawn plugin's MessageLoop thread.
    instance->async_run();
    // on_init -> user setup (in monitor_plugin: starts a 500ms Timer).
    instance->on_init();

    // Brief wait so the timer ticks at least once.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Reverse-order teardown: on_deinit, quit, wait_for_quit.
    instance->on_deinit();
    instance->quit();
    instance->wait_for_quit();
    VLOG_I("Standalone shutdown OK");
  } else {
    VLOG_I("monitor_plugin not found (build it from examples/plugin/plugin_runnable/).");
  }

  VLOG_I("Proxy runnable plugin example complete.");
  return 0;
}
