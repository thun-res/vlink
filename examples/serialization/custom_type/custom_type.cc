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

#include "custom_types.h"

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// ---------------------------------------------------------------------------
// custom_type.cc
//
// Exercises the kCustomType serialization path with two user-defined types
// declared in custom_types.h:
//   * Vec3       -> fixed 12-byte layout written via operator>>/<<.
//   * NamedValue -> variable-length [u32 len][name][i32][f64] payload.
// The example also performs a manual Serializer::serialize/deserialize round
// trip after the pub/sub run, proving the wire codec works standalone (which
// is what bag writers and custom transports rely on).
// ---------------------------------------------------------------------------

int main() {
  vlink::MessageLoop loop;

  // ---- Vec3 pub/sub ----
  // Fixed-size custom payload. Subscriber callback runs on `loop`'s thread.
  int vec3_count = 0;
  vlink::Subscriber<Vec3> vec3_sub("dds://example/custom/vec3");
  vec3_sub.attach(&loop);
  vec3_sub.listen([&vec3_count](const Vec3& v) {
    vec3_count++;
    VLOG_I("[Vec3] #", vec3_count, " x=", v.x, " y=", v.y, " z=", v.z);
  });

  vlink::Publisher<Vec3> vec3_pub("dds://example/custom/vec3");
  vec3_pub.attach(&loop);

  // ---- NamedValue pub/sub (variable-length payload) ----
  // Same shape as Vec3 but the wire size depends on the embedded string.
  // Callback fires on `loop`'s thread.
  int nv_count = 0;
  vlink::Subscriber<NamedValue> nv_sub("dds://example/custom/named");
  nv_sub.attach(&loop);
  nv_sub.listen([&nv_count](const NamedValue& nv) {
    nv_count++;
    VLOG_I("[NamedValue] #", nv_count, " name=\"", nv.name, "\" code=", nv.code, " value=", nv.value);
  });

  vlink::Publisher<NamedValue> nv_pub("dds://example/custom/named");
  nv_pub.attach(&loop);

  // One-shot timer (50ms, repeat=1) drives a burst of publishes on the loop
  // thread. Running publishes from the timer keeps producer + consumer on
  // the same thread, which avoids any cross-thread sync between them.
  vlink::Timer timer(&loop, 50, 1);
  timer.start([&]() {
    vec3_pub.publish(Vec3{1.0F, 2.0F, 3.0F});
    vec3_pub.publish(Vec3{-0.5F, 100.0F, -999.0F});

    NamedValue nv1;
    nv1.name = "temperature";
    nv1.code = 100;
    nv1.value = 36.6;
    nv_pub.publish(nv1);

    // Edge case: empty name exercises the name_len==0 branch in operator<<.
    NamedValue nv2;
    nv2.name = "";
    nv2.code = -1;
    nv_pub.publish(nv2);

    // Long name -- verifies length-prefix correctness beyond SSO threshold.
    NamedValue nv3;
    nv3.name = "a_very_long_sensor_name_that_exceeds_the_typical_short_string_optimization_threshold";
    nv3.code = 42;
    nv3.value = 3.14159;  // NOLINT(modernize-use-std-numbers)
    nv_pub.publish(nv3);
  });

  loop.async_run();
  loop.wait_for_idle(1000);
  loop.quit();
  loop.wait_for_quit();

  // ---- Manual round-trip verification ----
  // Direct Serializer use bypasses pub/sub entirely. This is the same code
  // path used by BagWriter/BagReader when persisting custom types to disk.
  NamedValue original;
  original.name = "test_field";
  original.code = 777;
  original.value = 2.71828;  // NOLINT(modernize-use-std-numbers)

  vlink::Bytes buf;
  vlink::Serializer::serialize(original, buf);

  NamedValue restored;
  vlink::Serializer::deserialize(buf, restored);
  VLOG_I("[RoundTrip] name_match=", original.name == restored.name, " code_match=", original.code == restored.code,
         " value_match=", original.value == restored.value);

  VLOG_I("[Summary] Vec3=", vec3_count, " NamedValue=", nv_count);
  return 0;
}
