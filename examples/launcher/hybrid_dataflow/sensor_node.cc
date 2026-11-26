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
// File: sensor_node.cc
//
// Example 2 -- Standalone external sensor process managed by vlink-launcher.
//
// Reads its target VLink publish URL from --out or the launcher-injected
// VLINK_OUTPUT_SCAN / VLINK_URL_SCAN environment variables (declared in
// hybrid_dataflow.toml), emits a stdout readiness token ("READY sensor=..."),
// and streams periodic sensor telemetry until signalled to stop.
// =============================================================================

#include <vlink/base/macros.h>
#include <vlink/base/utils.h>
#include <vlink/publisher.h>
//
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

static std::atomic_bool g_stop{false};

int main(int argc, char** argv) {
  std::string sensor_name = "sensor_0";
  std::string out_url = vlink::Utils::get_env("VLINK_OUTPUT_SCAN", "");
  int period_ms = 250;
  bool check_only = false;

  // Section: parse CLI overrides supplied by hybrid_dataflow.toml.
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];

    if (arg == "--check") {
      check_only = true;
    } else if (arg == "--name" && i + 1 < argc) {
      sensor_name = argv[++i];
    } else if (arg == "--out" && i + 1 < argc) {
      out_url = argv[++i];
    } else if (arg == "--period_ms" && i + 1 < argc) {
      char* end_ptr = nullptr;
      const auto parsed = static_cast<int64_t>(std::strtoll(argv[++i], &end_ptr, 10));

      if (end_ptr != nullptr && *end_ptr == '\0' && parsed >= 20 && parsed <= 60000) {
        period_ms = static_cast<int>(parsed);
      }
    }
  }

  if (check_only) {
    std::cout << "Preflight OK: sensor binary ready.\n" << std::flush;
    return 0;
  }

  if (out_url.empty()) {
    out_url = vlink::Utils::get_env("VLINK_URL_SCAN", "dds://hybrid/sensor/scan");
  }

  vlink::Utils::register_terminate_signal([](int) { g_stop.store(true, std::memory_order_release); }, false, false);

  // Section: create publisher and emit stdout readiness marker.
  vlink::Publisher<std::string> pub(out_url);
  std::cout << "READY sensor=" << sensor_name << " url=" << out_url << "\n" << std::flush;

  uint64_t seq = 0U;

  while (!g_stop.load(std::memory_order_acquire)) {
    ++seq;
    const std::string payload =
        sensor_name + ":dist=" + std::to_string(1.5 + static_cast<double>(seq % 4U)) + ":seq=" + std::to_string(seq);
    (void)pub.publish(payload);

    const int slices = std::max(1, period_ms / 25);

    for (int s = 0; s < slices && !g_stop.load(std::memory_order_acquire); ++s) {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  }

  vlink::Utils::register_terminate_signal(nullptr, false, false);
  return 0;
}
