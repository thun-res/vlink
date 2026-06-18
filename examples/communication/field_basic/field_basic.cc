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

#include "vehicle_types.h"

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// field_basic: Field-model basics over DDS.
//
// Demonstrates:
//   - vlink::Setter / vlink::Getter on the "dds://" backend.
//   - Late-joiner sync: Getter created AFTER the Setter still receives the
//     cached value via wait_for_value().
//   - listen() on a Getter: callback fires on every value transition.
//   - Combined pull (get) + push (listen) usage on the same Getter.
//
// Typical scenarios: vehicle state mirroring (gear, mode), parameter sync.
int main() {
  static constexpr char kUrl[] = "dds://vehicle/gear";

  vlink::MessageLoop loop;
  loop.set_name("getter_loop");
  loop.async_run();

  // Setter retains the latest value internally. Any Getter, even one created
  // later, will fetch this cached value when it queries the field.
  vlink::Setter<GearState> setter(kUrl);
  setter.set({0, true});
  VLOG_I("[setter] initial gear=0 (Park)");

  std::this_thread::sleep_for(50ms);

  // Late-joining Getter: constructed AFTER the first set(). The field model
  // guarantees it still observes the cached value once discovery completes.
  vlink::Getter<GearState> getter(kUrl);
  getter.attach(&loop);

  if (getter.wait_for_value(2000ms)) {
    VLOG_I("[getter] wait_for_value succeeded");
  } else {
    VLOG_W("[getter] wait_for_value timed out");
  }

  // Pull style: snapshot the current cached value.
  auto current = getter.get();

  if (current.has_value()) {
    VLOG_I("[getter] current gear=", current->gear, " engaged=", current->engaged);
  }

  // Push style: callback fires on the loop thread on every value change.
  // Atomic because main() reads it after wait_for_idle().
  std::atomic<int> change_count{0};
  getter.listen([&change_count](const GearState& gear) {
    VLOG_I("[getter] changed gear=", gear.gear, " engaged=", gear.engaged);
    change_count.fetch_add(1);
  });

  GearState gears[] = {{2, true}, {3, true}, {4, true}, {5, true}, {0, true}};

  for (const auto& gear : gears) {
    setter.set(gear);
    VLOG_I("[setter] set gear=", gear.gear);
    std::this_thread::sleep_for(100ms);
  }

  // Drain any pending callbacks before reading the counter.
  loop.wait_for_idle(1000);

  auto final_val = getter.get();

  if (final_val.has_value()) {
    VLOG_I("[getter] final gear=", final_val->gear, " engaged=", final_val->engaged);
  }

  VLOG_I("change_count=", change_count.load());

  loop.quit();
  loop.wait_for_quit();

  return 0;
}
