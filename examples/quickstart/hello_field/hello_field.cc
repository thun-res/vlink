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
//   - The Getter caches the most recently received value.
//   - Pull-style get() returning std::optional<T>.
//
// Typical scenarios: configuration and mode/state mirroring where only the
// latest value matters. Intermediate transitions may be coalesced.
int main() {
  static constexpr char kUrl[] = "intra://hello/field";

  // Intra does not replay a value written before this Getter exists. Create the
  // reader first; late-join replay on durable transports depends on backend QoS.
  vlink::Getter<SensorConfig> getter(kUrl);

  vlink::Setter<SensorConfig> setter(kUrl);
  setter.set({100, 25.0F});
  VLOG_I("[setter] rate=100 threshold=25.0");

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
