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

#include <chrono>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// Cached "field" value (latest-wins state). POD, Standard serializer.
struct SensorConfig {
  int sample_rate_hz;
  float threshold;
};

// hello_field: minimal Field-model (latest-value) walkthrough.
//
// Demonstrates:
//   - vlink::Setter<T> / vlink::Getter<T> on "intra://".
//   - Late-joiner semantics: a Getter created AFTER set() still receives the
//     cached value via wait_for_value() -- the framework retains "current".
//   - Pull-style get() returning std::optional<T>.
//
// Typical scenarios: configuration, mode/state mirroring, anything where only
// the latest value matters and consumers should never miss a transition.
int main() {
  static constexpr char kUrl[] = "intra://hello/field";

  // Publish the initial value before any getter exists.
  vlink::Setter<SensorConfig> setter(kUrl);
  setter.set({100, 25.0F});
  VLOG_I("[setter] rate=100 threshold=25.0");

  // Late-joining Getter: constructed AFTER set(). It must still observe the
  // cached value through wait_for_value(), which is the field-model guarantee.
  vlink::Getter<SensorConfig> getter(kUrl);
  getter.wait_for_value(1000ms);

  // Pull semantics: get() returns std::optional, empty if no value yet.
  auto value = getter.get();

  if (value.has_value()) {
    VLOG_I("[getter] rate=", value->sample_rate_hz, " threshold=", value->threshold);
  } else {
    VLOG_W("[getter] no value");
  }

  // Update the cached value; subsequent get() observes it.
  setter.set({500, 30.5F});
  // Brief sleep so the new value has propagated through the field cache before
  // we read again. This example uses pull-only (no listen()), so we cannot
  // synchronize on a callback.
  std::this_thread::sleep_for(50ms);

  value = getter.get();

  if (value.has_value()) {
    VLOG_I("[getter] rate=", value->sample_rate_hz, " threshold=", value->threshold);
  }

  return 0;
}
