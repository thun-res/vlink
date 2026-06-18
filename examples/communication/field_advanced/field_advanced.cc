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
#include <vlink/vlink.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "config_types.h"

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// field_advanced: advanced Field-model features over the "ddsc://" backend.
//
// Demonstrates:
//   - set_change_reporting(true): de-duplicate identical consecutive values
//     so listen() fires only on *real* transitions.
//   - Late-joiner one-shot sync via wait_for_value().
//   - Multi-Getter fan-out on the same URL with optional latency/loss stats.
//
// Typical scenarios: configuration distribution where duplicate writes are
// common and listeners should only react to genuine change events.
int main() {
  vlink::MessageLoop loop;
  loop.set_name("main_loop");
  loop.async_run();

  // ---------- Change-reporting (de-duplication) ----------
  // set_change_reporting(true) makes the Getter compare each incoming value
  // against the last delivered one and suppress callback invocation when they
  // compare equal. The semantic is "callback on TRANSITION only", not on
  // every set(). Must be configured before listen() to take effect uniformly.
  vlink::Setter<BrightnessConfig> setter("ddsc://display/brightness");
  setter.set({50, false});

  std::this_thread::sleep_for(50ms);

  vlink::Getter<BrightnessConfig> getter_cr("ddsc://display/brightness");
  getter_cr.attach(&loop);
  getter_cr.set_change_reporting(true);

  std::atomic<int> cr_count{0};
  // Callback runs on the loop thread; only fires when the new value differs.
  getter_cr.listen([&cr_count](const BrightnessConfig& cfg) {
    VLOG_I("[cr-getter] changed level=", cfg.level, " auto=", cfg.auto_mode);
    cr_count.fetch_add(1);
  });
  getter_cr.wait_for_value(1000ms);

  // 5 set() calls but only 2 *transitions* (50->75, 75->100). With change
  // reporting on, expect ~2 callback invocations (initial value may add 1).
  setter.set({50, false});
  std::this_thread::sleep_for(50ms);
  setter.set({50, false});
  std::this_thread::sleep_for(50ms);
  setter.set({75, false});
  std::this_thread::sleep_for(50ms);
  setter.set({75, false});
  std::this_thread::sleep_for(50ms);
  setter.set({100, true});
  std::this_thread::sleep_for(50ms);

  loop.wait_for_idle(1000);
  VLOG_I("[cr-getter] change_reporting=", getter_cr.get_change_reporting(), " callbacks=", cr_count.load(),
         " (expect ~2)");

  // ---------- Late-joiner sync ----------
  // A Getter created long after the last set() should still observe the cached
  // value (latest-wins semantics of the field model).
  vlink::Getter<BrightnessConfig> late_getter("ddsc://display/brightness");
  late_getter.attach(&loop);
  // listen() also fires once on the cached value when discovery completes.
  late_getter.listen([](const BrightnessConfig& cfg) {
    VLOG_I("[late-getter] sync received level=", cfg.level, " auto=", cfg.auto_mode);
  });

  if (late_getter.wait_for_value(2000ms)) {
    auto val = late_getter.get();
    if (val.has_value()) {
      VLOG_I("[late-getter] get(): level=", val->level, " auto=", val->auto_mode);
    }
  }

  // ---------- Multi-Getter fan-out + per-sample latency on g3 ----------
  vlink::Setter<int> multi_setter("ddsc://config/volume");

  std::atomic<int> c1{0};
  std::atomic<int> c2{0};
  std::atomic<int> c3{0};

  vlink::Getter<int> g1("ddsc://config/volume");
  g1.attach(&loop);
  g1.listen([&c1](const int& v) {
    VLOG_I("[g1] volume=", v);
    c1.fetch_add(1);
  });

  vlink::Getter<int> g2("ddsc://config/volume");
  g2.attach(&loop);
  g2.listen([&c2](const int&) { c2.fetch_add(1); });

  // set_latency_and_lost_enabled(true) MUST be called BEFORE listen() so
  // the per-sample timing infrastructure is configured before delivery starts.
  vlink::Getter<int> g3("ddsc://config/volume");
  g3.attach(&loop);
  g3.set_latency_and_lost_enabled(true);
  g3.listen([&g3, &c3](const int& v) {
    c3.fetch_add(1);
    VLOG_I("[g3] volume=", v, " latency=", g3.get_latency(), "us");
  });

  std::this_thread::sleep_for(50ms);

  for (int vol = 0; vol <= 100; vol += 25) {
    multi_setter.set(vol);
    std::this_thread::sleep_for(50ms);
  }

  loop.wait_for_idle(1000);

  // Cumulative loss accounting -- only meaningful because latency_and_lost
  // was enabled before listen().
  vlink::SampleLostInfo lost = g3.get_lost();
  VLOG_I("g1=", c1.load(), " g2=", c2.load(), " g3=", c3.load(), " (g3 total=", lost.total, " lost=", lost.lost, ")");

  loop.quit();
  loop.wait_for_quit();

  return 0;
}
