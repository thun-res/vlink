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

#ifdef VLINK_SUPPORT_DDS
#include <vlink/modules/dds_conf.h>
#endif

#include <chrono>
#include <cstring>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// ---------------------------------------------------------------------------
// qos_basics.cc
//
// QoS (Quality of Service) is a per-topic policy bundle that tunes
// reliability/latency/storage trade-offs. The vlink::Qos struct aggregates
// independent sub-policies:
//
//   reliability  -- BestEffort (drop on contention, lowest latency) vs
//                   Reliable (retransmit, with block_time/heartbeat_time).
//   history      -- KeepLast(depth) keeps only the N most-recent samples;
//                   KeepAll keeps everything (must be paired with
//                   ResourceLimits to prevent OOM on slow consumers).
//   durability   -- Volatile (no replay), TransientLocal (writer caches
//                   for late joiners), Transient/Persistent (broker/disk).
//   publish_mode -- Sync (publish() blocks until network handoff) vs
//                   ASync (queue + return). Sync is needed for hard-RT
//                   command paths; ASync is the default.
//   deadline     -- period (ms) a writer must publish within; misses are
//                   reported as a violation event.
//   lifespan     -- max age (ms) of a sample before the reader discards it.
//   resource_limits -- caps on samples/instances/sample-per-instance.
//
// Profiles are registered per-backend (DdsConf / DdscConf / ...). The URL
// query "?qos=name" then references the registered profile by name. Only
// DDS-family + Zenoh use QoS; intra/shm/mqtt/someip/fdbus/qnx ignore it.
// ---------------------------------------------------------------------------

static void print_qos(const vlink::Qos& qos) {
  static constexpr const char* kDur[] = {"Volatile", "TransientLocal", "Transient", "Persistent"};
  VLOG_I("QoS=", qos.name, " valid=", qos.valid,
         " reliability=", qos.reliability.kind == vlink::Qos::Reliability::kBestEffort ? "BestEffort" : "Reliable",
         " history=", qos.history.kind == vlink::Qos::History::kKeepLast ? "KeepLast" : "KeepAll",
         " depth=", qos.history.depth, " durability=", kDur[qos.durability.kind],
         " publish_mode=", qos.publish_mode.kind == vlink::Qos::PublishMode::kSync ? "Sync" : "ASync",
         " deadline=", qos.deadline.period, "ms lifespan=", qos.lifespan.duration, "ms");
}
int main() {
  // ---- Defaults ----
  // A default-constructed Qos has valid=false, which means the backend
  // skips the profile altogether and uses its own internal defaults. Set
  // valid=true to opt in; otherwise none of the other fields take effect.
  vlink::Qos defaults;
  print_qos(defaults);

  // ---- Sensor profile: best-effort, shallow history, async publish ----
  // Tuned for high-rate sampled data (lidar/imu/camera): dropping a frame
  // is cheaper than blocking the producer. depth=5 holds a small ring so
  // late readers see *some* recent samples without unbounded memory.
  vlink::Qos sensor_qos;
  std::strncpy(sensor_qos.name, "sensor", sizeof(sensor_qos.name) - 1);
  sensor_qos.valid = true;
  sensor_qos.reliability.kind = vlink::Qos::Reliability::kBestEffort;
  sensor_qos.history.kind = vlink::Qos::History::kKeepLast;
  sensor_qos.history.depth = 5;
  sensor_qos.durability.kind = vlink::Qos::Durability::kVolatile;
  sensor_qos.publish_mode.kind = vlink::Qos::PublishMode::kASync;
  sensor_qos.deadline.period = 100;
  sensor_qos.lifespan.duration = 500;
  print_qos(sensor_qos);

#ifdef VLINK_SUPPORT_DDS
  // register_qos installs the profile in DdsConf's name table. The URL
  // "?qos=sensor" then resolves to this Qos struct at attach time. Note
  // that registration is independent per backend: each transport family
  // has its own register_qos function with its own name space.
  vlink::DdsConf::register_qos("sensor", sensor_qos);

  vlink::Publisher<std::string> pub("dds://sensor/lidar_data?qos=sensor");
  vlink::Subscriber<std::string> sub("dds://sensor/lidar_data?qos=sensor");
  // Listener runs on the subscriber's internal dispatch thread (no loop
  // attached in this example -- DDS spins its own).
  sub.listen([](const std::string& msg) { VLOG_I("Received with sensor QoS:", msg); });
  pub.wait_for_subscribers(2s);
  pub.publish("lidar_frame_001");
  std::this_thread::sleep_for(100ms);
#else
  VLOG_W("DDS module not available; skipping registration.");
#endif

  // ---- Command profile: reliable, KeepAll, transient-local, sync publish ----
  // Tuned for control messages where every sample MUST be delivered, and
  // late joiners must see the most recent command. block_time caps how
  // long publish() may stall waiting for ACKs before reporting failure.
  // KeepAll without a paired resource_limits would risk OOM in production
  // -- see qos_history_depth for the safer pattern.
  vlink::Qos cmd_qos;
  std::strncpy(cmd_qos.name, "command", sizeof(cmd_qos.name) - 1);
  cmd_qos.valid = true;
  cmd_qos.reliability.kind = vlink::Qos::Reliability::kReliable;
  cmd_qos.reliability.block_time = 500;
  cmd_qos.reliability.heartbeat_time = 1000;
  cmd_qos.history.kind = vlink::Qos::History::kKeepAll;
  cmd_qos.durability.kind = vlink::Qos::Durability::kTransientLocal;
  cmd_qos.publish_mode.kind = vlink::Qos::PublishMode::kSync;
  print_qos(cmd_qos);

#ifdef VLINK_SUPPORT_DDS
  vlink::DdsConf::register_qos("command", cmd_qos);

  // Method model (RPC) also honours QoS -- reliability + sync semantics
  // matter even more for request/response than for pub/sub.
  vlink::Server<std::string, std::string> server("dds://control/brake?qos=command");
  // Server callback runs on the DDS dispatch thread; resp is written in
  // place and returned to the client.
  server.listen([](const std::string& req, std::string& resp) { resp = "ACK:" + req; });

  vlink::Client<std::string, std::string> client("dds://control/brake?qos=command");

  if (client.wait_for_connected(2s)) {
    // invoke() blocks until the server's listen() returns or the QoS
    // block_time fires.
    auto resp = client.invoke("emergency_stop");

    if (resp.has_value()) {
      VLOG_I("Command response:", resp.value());
    }
  }
#endif

  // Registration map per transport.
  VLOG_I("Registration: dds=DdsConf::register_qos, ddsc=DdscConf::register_qos,",
         " ddsr=DdsrConf::register_qos, ddst=DdstConf::register_qos, zenoh=ZenohConf::register_qos");
  VLOG_I("intra/shm/someip/mqtt/fdbus do not use Qos profiles");

  return 0;
}
