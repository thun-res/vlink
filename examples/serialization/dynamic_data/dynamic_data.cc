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
#include <vlink/extension/dynamic_data.h>
#include <vlink/vlink.h>

#include <chrono>
#include <string>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// ---------------------------------------------------------------------------
// dynamic_data.cc
//
// DynamicData is VLink's "envelope" message type for heterogeneous topics.
// Wire layout:
//   [20-byte type-name header (NUL-padded)][serialized payload bytes]
// The type-name string is a runtime tag that the subscriber inspects via
// get_type() to dispatch to the right typed accessor (as<T>() / convert()).
// The payload itself is encoded with the same Serializer that the typed
// Publisher<T>/Subscriber<T> would use, so DynamicData can carry any type
// the static serializer supports (POD, std::string, Bytes, custom...).
//
// Common use cases:
//   * A single topic carrying multiple message families (sensor multiplex).
//   * Bridges/proxies that forward messages without knowing their static T.
//   * Bag tools that replay arbitrary topics by reading the type tag.
// ---------------------------------------------------------------------------

struct Temperature {
  double celsius;
  uint32_t sensor_id;
};

struct Pressure {
  double hpa;
  uint32_t sensor_id;
};

// DynamicData has its own routing tag (kDynamicType) so the Serializer
// knows to prefix the 20-byte type header before the payload bytes.
static_assert(vlink::Serializer::get_type_of<vlink::DynamicData>() == vlink::Serializer::kDynamicType);

int main() {
  vlink::MessageLoop loop;

  // ---- In-memory load/retrieve (no transport involved) ----
  // load(name, value) stores the type tag and serializes the value into the
  // internal byte buffer. as<T>() returns a fresh T constructed by running
  // the inverse Serializer<T> on the stored bytes.
  vlink::DynamicData dd_str;
  dd_str.load("StringMsg", std::string("Hello from DynamicData!"));
  VLOG_I("[Basic] type=", dd_str.get_type(), " value=\"", dd_str.as<std::string>(), "\"");

  // convert(T&) is the by-reference variant of as<T>() -- useful when the
  // caller already owns the destination (avoids the extra move from as<T>()).
  vlink::DynamicData dd_temp;
  dd_temp.load("Temperature", Temperature{36.6, 42});
  Temperature restored_temp{};
  dd_temp.convert(restored_temp);
  VLOG_I("[Basic] type=", dd_temp.get_type(), " celsius=", restored_temp.celsius,
         " sensor_id=", restored_temp.sensor_id);

  // ---- Multi-type on a single topic URL ----
  // The subscriber receives every sample, regardless of payload type, and
  // routes via get_type(). This is the canonical pattern for "any message"
  // topics like /diagnostics or /status.
  int msg_count = 0;
  vlink::Subscriber<vlink::DynamicData> sub("dds://example/dynamic/multi");
  sub.attach(&loop);
  // Callback runs on `loop`'s thread once per delivered sample.
  sub.listen([&msg_count](const vlink::DynamicData& dd) {
    msg_count++;
    std::string type_name(dd.get_type());

    if (type_name == "Temperature") {
      auto t = dd.as<Temperature>();
      VLOG_I("[Sub] #", msg_count, " Temperature celsius=", t.celsius, " sensor=", t.sensor_id);
    } else if (type_name == "Pressure") {
      auto p = dd.as<Pressure>();
      VLOG_I("[Sub] #", msg_count, " Pressure hpa=", p.hpa, " sensor=", p.sensor_id);
    } else if (type_name == "Status") {
      VLOG_I("[Sub] #", msg_count, " Status=\"", dd.as<std::string>(), "\"");
    }
  });

  vlink::Publisher<vlink::DynamicData> pub("dds://example/dynamic/multi");
  pub.attach(&loop);

  // One-shot 50ms timer publishes a heterogeneous burst. Timer callback runs
  // on `loop`'s thread, same as the subscriber callback.
  vlink::Timer timer(&loop, 50, 1);
  timer.start([&pub]() {
    vlink::DynamicData dd1;
    dd1.load("Temperature", Temperature{22.5, 1});
    pub.publish(dd1);

    vlink::DynamicData dd2;
    dd2.load("Pressure", Pressure{1013.25, 2});
    pub.publish(dd2);

    vlink::DynamicData dd3;
    dd3.load("Status", std::string("all_sensors_ok"));
    pub.publish(dd3);

    vlink::DynamicData dd4;
    dd4.load("Temperature", Temperature{-5.0, 3});
    pub.publish(dd4);
  });

  loop.async_run();
  loop.wait_for_idle(1000);
  loop.quit();
  loop.wait_for_quit();

  // ---- Wire-format round-trip via operator>>/<< ----
  // Demonstrates the raw codec: dd >> bytes serializes [20-byte tag][payload];
  // bytes >> dd parses it back. Used by bag tools / proxies that operate on
  // the wire buffer directly without going through pub/sub.
  vlink::DynamicData dd_wire;
  dd_wire.load("TestType", Temperature{100.0, 99});

  vlink::Bytes wire_bytes;
  dd_wire >> wire_bytes;

  vlink::DynamicData dd_from_wire;
  dd_from_wire << wire_bytes;

  Temperature from_wire{};
  dd_from_wire.convert(from_wire);
  VLOG_I("[Wire] size=", wire_bytes.size(), " type=", dd_from_wire.get_type(), " celsius=", from_wire.celsius,
         " sensor=", from_wire.sensor_id);

  // ---- Equality comparison ----
  // operator== compares both the type tag AND the serialized payload bytes,
  // so two DynamicData values are equal iff they carry the same logical
  // message. Useful for dedup / change-detection in subscribers.
  vlink::DynamicData a;
  vlink::DynamicData b;
  a.load("Test", 42);
  b.load("Test", 42);
  VLOG_I("[Eq] same=", a == b);

  b.load("Test", 99);
  VLOG_I("[Eq] differ=", a != b);

  VLOG_I("[Summary] received=", msg_count);
  return 0;
}
