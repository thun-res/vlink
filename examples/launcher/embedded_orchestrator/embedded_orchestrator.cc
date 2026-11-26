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
// File: embedded_orchestrator.cc
//
// Example 3 -- Programmatic C++ orchestration embedding vlink::LauncherServer
// and vlink::LauncherAPI over an in-process intra:// control plane.
//
// Demonstrates:
//   1. Static manifest validation, ASCII DAG/dataflow export & plugin inspection
//   2. Real-time lifecycle event subscription and in-process LauncherAPI attach
//   3. Effective config inspection (get_unit_config), live param update & reload
//   4. Zero-downtime active/standby failover (cruise_primary <-> cruise_backup)
//   5. Controlled mode switching via isolate_group("@parking") keeping @core online
//   6. Runtime component hot-plug with declarative input port binding (load/unload)
//   7. Ring-buffer log query (query_logs) and per-unit startup blame summary
//   8. Graceful asynchronous shutdown via request_stop() + wait()
// =============================================================================

#include <vlink/base/macros.h>
#include <vlink/base/utils.h>
#include <vlink/external/launcher_api.h>
#include <vlink/external/launcher_server.h>
//
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

template <typename PredT>
static bool wait_until(PredT&& pred, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }

  return pred();
}

static vlink::launcher::UnitState find_state(const vlink::launcher::SystemSnapshot& snap, const std::string& name) {
  for (const auto& u : snap.units) {
    if (u.name == name) {
      return u.state;
    }
  }

  return vlink::launcher::UnitState::kPending;
}

int main(int argc, char** argv) {
  std::string manifest_path;

  if (argc > 1) {
    manifest_path = argv[1];
  } else {
    manifest_path = (std::filesystem::path(vlink::Utils::get_app_dir()) / "embedded_orchestrator.toml").string();
  }

  // Section: inspect exported plugin classes and validate manifest offline.
  const auto plugin_info = vlink::LauncherServer::inspect_plugin("example_launcher_embedded_components");

  if (plugin_info.loaded) {
    std::cout << "=== Plugin Inspection (" << plugin_info.path << " v" << plugin_info.version_major << "."
              << plugin_info.version_minor << ") ===\n";

    for (const auto& cls : plugin_info.components) {
      std::cout << "  - " << cls << "\n";
    }
  }

  vlink::LauncherServer::Config srv_cfg;
  srv_cfg.manage_console = false;
  srv_cfg.enable_control_plane = true;

  vlink::LauncherServer server(srv_cfg);
  std::string err;

  if (!server.load_file(manifest_path, &err)) {
    std::cerr << "Failed to load manifest '" << manifest_path << "': " << err << "\n";
    return 1;
  }

  const auto report = server.validate(false);

  if (!report.valid) {
    for (const auto& e : report.errors) {
      std::cerr << "Validation error: " << e << "\n";
    }

    return 1;
  }

  std::cout << "\n=== Static Startup & Dataflow Graph ===\n"
            << server.export_graph(vlink::LauncherServer::GraphFormat::kAscii) << "\n";

  // Section: subscribe to lifecycle transitions, boot server and connect API.
  server.register_event_callback([](const vlink::launcher::EventRecord& ev) {
    std::cout << "[event] " << ev.unit_name << ": " << ev.event_name << " ("
              << vlink::launcher::unit_state_to_string(ev.old_state) << " -> "
              << vlink::launcher::unit_state_to_string(ev.new_state) << ")\n";
  });

  if (!server.start()) {
    std::cerr << "Failed to start embedded LauncherServer.\n";
    return 1;
  }

  vlink::LauncherAPI::Config api_cfg;
  api_cfg.scheme = "intra";
  api_cfg.namespace_prefix = "embedded_demo";
  api_cfg.default_timeout_ms = 3000U;

  vlink::LauncherAPI api(api_cfg);
  api.async_run();

  if (!api.wait_for_connected(3000U)) {
    std::cerr << "Timed out connecting in-process LauncherAPI.\n";
    server.stop();
    return 1;
  }

  const bool booted = wait_until(
      [&]() {
        const auto snap = server.get_snapshot();
        return find_state(snap, "vehicle_bridge") == vlink::launcher::UnitState::kReady &&
               find_state(snap, "cruise_primary") == vlink::launcher::UnitState::kReady &&
               find_state(snap, "cruise_backup") == vlink::launcher::UnitState::kInactive &&
               find_state(snap, "odom_feed") == vlink::launcher::UnitState::kReady &&
               find_state(snap, "parking_planner") == vlink::launcher::UnitState::kSkipped;
      },
      5000);

  if (!booted) {
    std::cerr << "Timed out waiting for cold boot steady state.\n";
    server.stop();
    return 1;
  }

  // Section: Step 1 -- inspect effective config, update parameter and reload.
  std::cout << "\n=== Step 1: Config inspection, live param update & reload ===\n";
  const auto cfg_res = api.get_unit_config("cruise_primary");
  std::cout << "get_unit_config(cruise_primary) bytes=" << cfg_res.message.size() << "\n";

  const auto p_res = api.set_param("cruise_primary", vlink::launcher::encode_param_entry("target_speed_mps", 18.5));
  std::cout << "set_param(cruise_primary.target_speed_mps=18.5): " << p_res.message << "\n";

  const auto r_res = api.reload_unit("cruise_primary");
  std::cout << "reload_unit(cruise_primary): " << r_res.message << "\n";

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // Section: Step 2 -- zero-downtime active/standby failover.
  std::cout << "\n=== Step 2: Zero-downtime active/standby failover (primary <-> backup) ===\n";
  const auto act_res = api.activate_unit("cruise_backup");

  if (!act_res.ok() ||
      !wait_until(
          [&]() { return find_state(server.get_snapshot(), "cruise_backup") == vlink::launcher::UnitState::kReady; },
          3000)) {
    std::cerr << "Failed to activate cruise_backup: " << act_res.message << "\n";
    server.stop();
    return 1;
  }

  const auto deact_res = api.deactivate_unit("cruise_primary");

  if (!deact_res.ok()) {
    std::cerr << "Failed to deactivate cruise_primary: " << deact_res.message << "\n";
    server.stop();
    return 1;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // Section: Step 3 -- controlled mode isolation (@driving -> @parking, @core stays up).
  std::cout << "\n=== Step 3: Controlled mode switch via isolate_group(\"@parking\") ===\n";
  const auto iso_res = api.isolate_group("@parking");
  std::cout << "isolate_group(@parking): " << iso_res.message << "\n";

  if (!iso_res.ok() || !wait_until(
                           [&]() {
                             const auto snap = server.get_snapshot();
                             return find_state(snap, "parking_planner") == vlink::launcher::UnitState::kReady &&
                                    find_state(snap, "vehicle_bridge") == vlink::launcher::UnitState::kReady;
                           },
                           3000)) {
    std::cerr << "Timed out waiting for @parking mode.\n";
    server.stop();
    return 1;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // Section: Step 4 -- runtime plugin hot-plug with declarative input port binding.
  std::cout << "\n=== Step 4: Hot-plug TelemetryTap with declarative input port binding ===\n";
  vlink::launcher::LoadComponentRequest load_req;
  load_req.name = "dynamic_telemetry";
  load_req.library = "example_launcher_embedded_components";
  load_req.class_name = "TelemetryTap";
  load_req.scheduler_group = "control_pinned";
  load_req.worker_index = 1;
  load_req.inputs["cmd"] = "intra://embedded/vehicle/cmd";
  const auto l_res = api.load_component(load_req);
  std::cout << "load_component(dynamic_telemetry): " << l_res.message << "\n";

  const auto dyn_cfg = api.get_unit_config("dynamic_telemetry");
  std::cout << "get_unit_config(dynamic_telemetry) bytes=" << dyn_cfg.message.size() << "\n";

  std::this_thread::sleep_for(std::chrono::milliseconds(350));

  const auto u_res = api.unload_component("dynamic_telemetry");
  std::cout << "unload_component(dynamic_telemetry): " << u_res.message << "\n";

  // Section: Step 5 -- ring-buffer log query and cold-boot blame readout.
  std::cout << "\n=== Step 5: Ring-buffer log query & startup blame summary ===\n";
  vlink::launcher::LogQuery log_q;
  log_q.limit = 8U;
  const auto log_res = api.query_logs(log_q);
  std::cout << "query_logs returned " << log_res.logs.size() << " recent entries.\n";

  auto snap = server.get_snapshot();
  std::sort(snap.units.begin(), snap.units.end(),
            [](const auto& a, const auto& b) { return a.startup_duration_ms > b.startup_duration_ms; });

  std::cout << "Boot duration: " << snap.boot_duration_ms << " ms\n";

  for (const auto& u : snap.units) {
    std::cout << "  - " << u.name << " [" << vlink::launcher::unit_state_to_string(u.state)
              << "] startup=" << u.startup_duration_ms << " ms\n";
  }

  // Section: Step 6 -- graceful asynchronous shutdown.
  std::cout << "\n=== Step 6: Graceful asynchronous shutdown ===\n";
  server.request_stop(0);
  (void)server.wait(5000);
  std::cout << "Embedded orchestrator exited cleanly.\n";
  return 0;
}
