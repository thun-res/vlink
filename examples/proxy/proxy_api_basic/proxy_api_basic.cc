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
// File: proxy_api_basic.cc
//
// Client-side demo of vlink::ProxyAPI. A ProxyAPI talks (over DDS) to one or
// more vlink::ProxyServer instances on the same domain_id and asks them to
// stream topic metadata + raw payloads. It is the building block for
// visualisers, recorders, dashboards, and CLI tools.
//
// Two roles are defined:
//   kController -- may send Control requests; ProxyServer issues a token at
//                  handshake time so multiple controllers do not fight over
//                  the active Mode.
//   kListener   -- read-only; receives info/data the controller has unlocked
//                  but cannot change the mode.
//
// The 8 ProxyAPI::Mode values encode "what does the server expose":
//   kOffline            -- disconnected; server releases every subscription
//   kObserveOne         -- observe a single topic from the URL list
//   kObserveAll         -- observe every discovered topic on the network
//   kRecord             -- record data from topics in the URL list
//   kPlay               -- injection/playback, usually fed by recorded data
//   kEdit               -- forward data injected by the controller
//   kAuto               -- observe specified URLs + accept controller injection
//   kAutoAndObserveAll  -- kAuto + observe every topic
// =============================================================================

#include <vlink/base/logger.h>
#include <vlink/external/proxy_api.h>

#include <chrono>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

int main() {
  // Section: build the Config. Every field is a stable public knob:
  //   role          -- Controller vs Listener (see file header)
  //   dds_impl      -- backing DDS provider id: "dds" / "ddsc" / "ddsr" / ...
  //   domain_id     -- DDS domain; must match the ProxyServer's domain
  //   reliable      -- DDS reliability QoS for the control channel
  //   match_version -- if true, refuse to connect to a server whose vlink
  //                    version differs (defensive against ABI drift).
  vlink::ProxyAPI::Config cfg;
  cfg.role = vlink::ProxyAPI::kController;
  cfg.dds_impl = "dds";
  cfg.domain_id = 0;
  cfg.reliable = false;
  cfg.match_version = true;

  VLOG_I("role=", cfg.role == vlink::ProxyAPI::kController ? "Controller" : "Listener");
  VLOG_I("dds_impl=", cfg.dds_impl, " domain_id=", cfg.domain_id);

  vlink::ProxyAPI api(cfg);

  // Section: connect callback. Fires when the underlying transport
  // discovers / loses a ProxyServer. Thread: ProxyAPI internal event loop;
  // do NOT block here.
  api.register_connect_callback([](bool connected) { VLOG_I("[ProxyAPI] connection: ", connected ? "up" : "down"); });

  // Error callback. Fires on protocol errors (version mismatch, token
  // rejection, heartbeat timeout). Thread: ProxyAPI internal event loop.
  api.register_error_callback(
      [](vlink::ProxyAPI::Error error) { VLOG_I("[ProxyAPI] error code: ", static_cast<int>(error)); });

  // Info callback: invoked roughly at 1Hz with the latest topic catalogue
  // (URL, frequency, schema, etc.). Thread: ProxyAPI internal event loop.
  api.register_info_callback([](const std::vector<vlink::ProxyAPI::Info>& info_list) {
    VLOG_I("[ProxyAPI] info for ", info_list.size(), " topics");
    for (const auto& info : info_list) {
      VLOG_I("  topic: ", info.url, " freq=", info.freq, " Hz");
    }
  });

  // Data callback: invoked for every payload routed through the server.
  // Thread: ProxyAPI internal event loop. Heavy work belongs on another
  // thread to avoid stalling the receive pipeline.
  api.register_data_callback(
      [](const vlink::ProxyAPI::Data& data) { VLOG_I("[ProxyAPI] data from: ", data.url, " size=", data.raw.size()); });

  // Section: start the worker thread. After this point the callbacks above
  // may be invoked at any time.
  api.async_run();

  // Section: ask the server to enter "stream me all topic info" mode. The
  // token handshake happens transparently inside send_control: ProxyAPI
  // presents its session token, the server validates it is still the active
  // controller, and only then changes mode. Returns false if the server
  // rejected (e.g., another controller is already active).
  vlink::ProxyAPI::Control ctrl;
  ctrl.mode = vlink::ProxyAPI::kObserveAll;
  VLOG_I("send_control(kObserveAll) returned: ", api.send_control(ctrl));

  // Section: introspection getters. current_mode reflects the last value
  // acknowledged by the server (NOT the value we requested). is_connected
  // is driven by the 1Hz heartbeat exchange. is_support_shm / is_enable_filter
  // surface server-side capability flags published in the info stream.
  VLOG_I("current_mode=", static_cast<int>(api.get_current_mode()), " connected=", api.is_connected(),
         " shm=", api.is_support_shm(), " filter=", api.is_enable_filter());

  // Give the event loop a moment to receive at least one info/data callback
  // before tearing down.
  std::this_thread::sleep_for(500ms);

  // Section: graceful shutdown. quit() flags the loop, wait_for_quit joins.
  // Callbacks will not fire after wait_for_quit returns.
  api.quit();
  api.wait_for_quit();

  VLOG_I("ProxyAPI basic example complete.");
  return 0;
}
