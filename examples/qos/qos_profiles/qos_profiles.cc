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

#include <vlink/base/logger.h>
#include <vlink/extension/qos.h>
#include <vlink/extension/qos_profile.h>
#include <vlink/vlink.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// ---------------------------------------------------------------------------
// qos_profiles.cc
//
// vlink::QosProfile ships a battery of preset Qos structs covering the
// common engineering scenarios (Event/Method/Field/Sensor/Parameter/...)
// so application code rarely needs to hand-roll a profile. The presets
// are constexpr-friendly and valid=true out of the box.
//
// Two ways to apply a preset:
//   * Use it directly as the topic's QoS (just pass `kSensor`).
//   * Copy + tweak ("baseline + delta") -- start from a preset, edit a
//     single field, register under a new name. Avoids re-specifying all
//     orthogonal policies.
//
// The QosProfile::get_available_qos_map() registry merges the built-in
// presets with anything loaded from VLINK_QOS_CONFIG (a JSON file path).
// This lets ops swap profiles without rebuilding the application.
// ---------------------------------------------------------------------------

static void log_profile(const char* label, const vlink::Qos& qos) {
  static constexpr const char* kDur[] = {"Volatile", "TransientLocal", "Transient", "Persistent"};
  static constexpr const char* kPrio[] = {"", "RealTime", "High", "", "Normal", "", "Low", "Background"};
  VLOG_I(label, ": ", qos.reliability.kind == vlink::Qos::Reliability::kBestEffort ? "BestEffort" : "Reliable", ", ",
         qos.history.kind == vlink::Qos::History::kKeepLast ? "KeepLast" : "KeepAll", "(depth=", qos.history.depth,
         "), ", kDur[qos.durability.kind], ", ",
         qos.publish_mode.kind == vlink::Qos::PublishMode::kSync ? "Sync" : "ASync",
         ", Priority=", kPrio[qos.additions.priority], qos.additions.is_express ? ", Express" : "");
}
int main() {
  // ---- Built-in presets ----
  // Each preset bakes a coherent reliability/history/durability/priority
  // mix tuned to its use case. Order in the dump roughly mirrors usage
  // frequency in real deployments.
  log_profile("kEvent    ", vlink::QosProfile::kEvent);
  log_profile("kMethod   ", vlink::QosProfile::kMethod);
  log_profile("kField    ", vlink::QosProfile::kField);
  log_profile("kSensor   ", vlink::QosProfile::kSensor);
  log_profile("kParameter", vlink::QosProfile::kParameter);
  log_profile("kService  ", vlink::QosProfile::kService);
  log_profile("kClock    ", vlink::QosProfile::kClock);
  log_profile("kStatic   ", vlink::QosProfile::kStatic);
  log_profile("kLight    ", vlink::QosProfile::kLight);
  log_profile("kPoor     ", vlink::QosProfile::kPoor);
  log_profile("kBetter   ", vlink::QosProfile::kBetter);
  log_profile("kBest     ", vlink::QosProfile::kBest);
  log_profile("kLarge    ", vlink::QosProfile::kLarge);
  log_profile("kAlarm    ", vlink::QosProfile::kAlarm);
  log_profile("kCommand  ", vlink::QosProfile::kCommand);
  log_profile("kLog      ", vlink::QosProfile::kLog);

  // ---- Runtime lookup by profile name ----
  // The map merges built-in presets with anything VLINK_QOS_CONFIG loaded.
  // Tools / proxies use this to discover what's available without knowing
  // the static enum.
  const auto& qos_map = vlink::QosProfile::get_available_qos_map();
  VLOG_I("Profiles registered: ", qos_map.size());

  auto it = qos_map.find("sensor");

  if (it != qos_map.end()) {
    log_profile("lookup(sensor)", it->second);
  }

  // ---- Pub/Sub plumbing demo (backend default qos, not kSensor) ----
  // NOTE: this URL omits ?qos=, so the DDS backend uses its built-in default
  // (kEvent) -- it does NOT auto-match kSensor from the topic name "sensor".
  // To actually apply kSensor, add ?qos=sensor to the URL (see the README).
  // The pub/sub below only demonstrates plumbing, not the kSensor profile.
  // Atomic counter because the DDS dispatch thread races the main-thread sleep.
  std::atomic<int> received{0};
  vlink::Subscriber<std::string> sub("dds://qos_profiles/sensor");
  // Listener runs on the DDS dispatch thread.
  sub.listen([&received](const std::string& msg) {
    (void)msg;
    received++;
  });

  vlink::Publisher<std::string> pub("dds://qos_profiles/sensor");
  pub.wait_for_subscribers();

  for (int i = 0; i < 50; ++i) {
    pub.publish("sensor_data_" + std::to_string(i));
  }

  std::this_thread::sleep_for(100ms);
  VLOG_I("pub/sub demo (backend default qos, not kSensor): sent 50, received", received.load());

  // ---- Copy + customise a preset ("baseline + delta" pattern) ----
  // Start from kSensor, then override only the three fields that differ
  // for our use case. Avoids restating reliability/durability/etc.
  vlink::Qos custom_sensor = vlink::QosProfile::kSensor;
  std::strncpy(custom_sensor.name, "custom_sensor", sizeof(custom_sensor.name) - 1);
  custom_sensor.history.depth = 50;
  custom_sensor.reliability.kind = vlink::Qos::Reliability::kReliable;
  custom_sensor.deadline.period = 100;
  log_profile("custom_sensor", custom_sensor);

  // ---- Environment-driven JSON profile loading ----
  // If set, VLink reads the JSON at startup and merges named profiles
  // into get_available_qos_map(). Lets ops swap profiles without rebuild.
  const char* qos_config = std::getenv("VLINK_QOS_CONFIG");

  if (qos_config != nullptr) {
    VLOG_I("VLINK_QOS_CONFIG=", qos_config, " (custom profiles merged into the preset map)");
  } else {
    VLOG_I("VLINK_QOS_CONFIG unset. Point it at a JSON file to add named profiles at startup.");
  }

  VLOG_I("Selection: camera/lidar=kSensor, control=kEvent, RPC=kMethod, state=kField,",
         " diag=kPoor, clock=kClock, map=kLarge, params=kParameter,", " alarm=kAlarm, command=kCommand, log=kLog");
  return 0;
}
