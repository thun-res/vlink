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
#include <vlink/vlink.h>

#ifdef VLINK_SUPPORT_DDS
#include <vlink/modules/dds_conf.h>
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// ---------------------------------------------------------------------------
// qos_history_depth.cc
//
// Focused walkthrough of the History + ResourceLimits sub-policies, the
// two QoS knobs that dominate memory footprint:
//
//   History.kind == kKeepLast (depth = N):
//     Ring of the N most-recent samples per instance. Old samples drop
//     when the ring fills. Bounded memory regardless of consumer speed.
//
//   History.kind == kKeepAll:
//     No drops -- every sample queued until the reader takes it. Risk of
//     unbounded growth if the consumer can't keep up. Always pair with
//     ResourceLimits to enforce a hard cap (max_samples /
//     max_samples_per_instance / max_instances), otherwise a slow
//     subscriber will OOM the writer.
//
// The URL `?depth=` query overrides the profile depth per endpoint --
// useful for asymmetric setups where the writer wants a small history
// (cheap state) but the reader needs a larger backlog (analysis window).
// ---------------------------------------------------------------------------

// Builder helper -- name + depth define a KeepLast profile entirely.
static vlink::Qos make_keep_last(const char* name, uint32_t depth) {
  vlink::Qos qos;
  std::strncpy(qos.name, name, sizeof(qos.name) - 1);
  qos.valid = true;
  qos.history.kind = vlink::Qos::History::kKeepLast;
  qos.history.depth = depth;
  return qos;
}
int main() {
  // ---- KeepLast variants ----
  // depth=1   -> "current state" pattern, equivalent to Setter/Getter.
  // depth=10  -> short command/control streams (last few ticks).
  // depth=100 -> wide analysis window (multiple seconds at 10-100Hz).
  auto depth1 = make_keep_last("depth1", 1);
  auto depth10 = make_keep_last("depth10", 10);
  auto depth100 = make_keep_last("depth100", 100);

#ifdef VLINK_SUPPORT_DDS
  vlink::DdsConf::register_qos("depth1", depth1);
  vlink::DdsConf::register_qos("depth10", depth10);
  vlink::DdsConf::register_qos("depth100", depth100);
  VLOG_I("Registered depth1, depth10, depth100 profiles.");
#endif

  // ---- KeepAll with ResourceLimits so the buffer is bounded ----
  // max_samples              -- total across all instances.
  // max_samples_per_instance -- per-key cap, prevents one hot instance
  //                             from starving the others.
  // Without these, KeepAll is essentially "OOM eventually".
  vlink::Qos keep_all;
  std::strncpy(keep_all.name, "keepall", sizeof(keep_all.name) - 1);
  keep_all.valid = true;
  keep_all.history.kind = vlink::Qos::History::kKeepAll;
  keep_all.resource_limits.max_samples = 10000;
  keep_all.resource_limits.max_samples_per_instance = 5000;
  VLOG_I("KeepAll: max_samples=", keep_all.resource_limits.max_samples);

#ifdef VLINK_SUPPORT_DDS
  vlink::DdsConf::register_qos("keepall", keep_all);

  // ---- URL ?depth= overrides the profile value per endpoint ----
  // Asymmetric setup: writer keeps a shallow 5-deep ring (cheap), reader
  // requests 50-deep so late subscribers can catch up further.
  vlink::Publisher<std::string> pub("dds://qos/depth_demo?depth=5");
  vlink::Subscriber<std::string> sub("dds://qos/depth_demo?depth=50");

  std::atomic<int> count{0};
  // Listener runs on the DDS dispatch thread; atomic counter keeps the
  // tally thread-safe with the main-thread reads below.
  sub.listen([&count](const std::string& msg) {
    int c = ++count;

    if (c <= 3) {
      VLOG_I("Received:", msg);
    }
  });

  pub.wait_for_subscribers(2s);

  for (int i = 1; i <= 20; ++i) {
    pub.publish("depth-msg-" + std::to_string(i));
  }

  std::this_thread::sleep_for(300ms);
  VLOG_I("Published 20, received ", count.load());
#endif

  // Selection guide and sizing rule of thumb.
  VLOG_I("depth=1 -> current state | depth=5-10 -> control | depth=20-50 -> sensor",
         " | depth=100+ -> recording | KeepAll -> audit");
  VLOG_I("Memory budget: topics * depth * avg_msg_size (depth=1000 * 1MB = 1GB!)");
  return 0;
}
