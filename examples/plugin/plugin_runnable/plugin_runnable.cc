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
// File: plugin_runnable.cc
//
// Standalone host that exercises a RunablePluginInterface plugin manually
// (without involving ProxyServer). This is useful for unit-testing a plugin
// in isolation: you can construct it, drive its lifecycle, and tear it down
// the same way ProxyServer would, but with full control over timing.
//
// Sequence:
//   load()         -- dlopen and instantiate
//   async_run()    -- spawn the plugin's internal MessageLoop thread
//   on_init()      -- run plugin setup (registers timer in this example)
//   sleep(3s)      -- let the timer fire a few times
//   on_deinit()    -- run plugin teardown
//   quit()         -- ask the loop to stop
//   wait_for_quit()-- join the worker thread
//   reset()        -- destroy the plugin instance (deleter runs in .so,
//                      then dlclose() unmaps the .so)
//   clear()        -- drop bookkeeping in the Plugin registry
// =============================================================================

#include <vlink/base/logger.h>
#include <vlink/base/plugin.h>
#include <vlink/extension/runnable_plugin_interface.h>

#include <chrono>
#include <string>
#include <thread>

int main() {
  // Print the interface's stable plugin_id so the user can correlate it with
  // VLINK_PLUGIN_REGISTER in monitor_plugin.cc.
  VLOG_I("RunablePluginInterface plugin_id: ", std::string(vlink::RunablePluginInterface::get_plugin_id()));

  // Section: open the loader and reduce verbosity so the demo output is clean.
  vlink::Plugin plugin;
  plugin.set_log_level(vlink::Logger::kInfo);

  // Section: load monitor_plugin v1.0 -- must match VLINK_PLUGIN_DECLARE.
  auto monitor = plugin.load<vlink::RunablePluginInterface>("monitor_plugin", 1, 0);

  if (!monitor) {
    VLOG_E("Failed to load monitor_plugin. Ensure libmonitor_plugin.so is accessible.");
    return 1;
  }

  // Section: drive the runnable lifecycle by hand.
  // async_run starts the loop thread inside the .so; subsequent on_init/
  // on_deinit calls can install timers / subscribers that use that thread.
  monitor->async_run();
  monitor->on_init();

  // Let the plugin's timer tick about 6 times (500ms interval).
  std::this_thread::sleep_for(std::chrono::seconds(3));

  // Tear down in the reverse order. on_deinit releases plugin-owned state
  // first, then quit() flags the loop to stop, then wait_for_quit() joins.
  monitor->on_deinit();
  monitor->quit();
  monitor->wait_for_quit();

  // Section: release. monitor.reset() invokes the shared_ptr deleter which
  // calls vlink_plugin_destroy inside the .so then dlclose's it. clear()
  // removes the now-empty entry from the registry.
  monitor.reset();
  plugin.clear();

  VLOG_I("Plugin runnable example complete.");
  return 0;
}
