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
// File: properties.cc
//
// Demonstrates the string-key / string-value property bag exposed by every
// node primitive: set_property("qos.reliability.kind", "1") etc.
//
// Why strings: properties are a uniform escape hatch that lets us configure
// QoS without exposing per-transport C++ structs in the public API. The
// keys follow a dot-separated hierarchy mirroring the underlying QoS struct
// (qos.reliability.kind -> ReliabilityQosPolicy.kind, etc.) and integers are
// passed as their decimal string ("1", "0", "200"). The node parses the
// value into the strongly-typed field when init() builds the transport,
// which is why properties must be set BEFORE init() to take effect.
//
// Key categories used below:
//   qos.reliability.kind         -- 0=BestEffort, 1=Reliable
//   qos.history.kind             -- 0=KeepLast, 1=KeepAll
//   qos.history.depth            -- ring-buffer depth for KeepLast
//   qos.durability.kind          -- 0=Volatile, 1=TransientLocal, ...
//   qos.publish_mode.kind        -- 0=Synchronous, 1=Asynchronous
//   qos.reliability.block_time   -- ms to block publish() on full queue
//   qos.reliability.heartbeat_time -- DDS heartbeat period in ms
// =============================================================================

#include <vlink/base/logger.h>
#include <vlink/vlink.h>

#include <chrono>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

int main() {
  // Section 1: set/get the QoS-related properties. We use kWithoutInit so
  // every set_property call lands BEFORE init() builds the underlying DDS
  // entity; otherwise QoS values are locked in at construction.
  {
    vlink::Publisher<std::string> pub("dds://properties/demo", vlink::InitType::kWithoutInit);

    pub.set_property("qos.reliability.kind", "1");               // Reliable
    pub.set_property("qos.history.kind", "0");                   // KeepLast
    pub.set_property("qos.history.depth", "50");                 // ring-buffer of 50
    pub.set_property("qos.durability.kind", "1");                // TransientLocal
    pub.set_property("qos.publish_mode.kind", "0");              // Synchronous
    pub.set_property("qos.reliability.block_time", "200");       // 200ms block
    pub.set_property("qos.reliability.heartbeat_time", "5000");  // 5s heartbeat

    VLOG_I("reliability.kind=", pub.get_property("qos.reliability.kind"),
           " history.depth=", pub.get_property("qos.history.depth"),
           " durability.kind=", pub.get_property("qos.durability.kind"));

    // init builds the DDS Publisher with the QoS we just configured.
    pub.init();
  }

  // Section 2: set_ser_type configures the serialiser family AND the
  // schema_type in one call (or just the type name if schema_type is omitted).
  // This is the high-level alternative to setting "serializer.type" and
  // "schema.type" properties individually.
  {
    vlink::Publisher<vlink::Bytes> pub("dds://properties/ser_type", vlink::InitType::kWithoutInit);

    pub.set_ser_type("demo.proto.Message", vlink::SchemaType::kProtobuf);
    // Re-call with only the type name: schema family (protobuf) is preserved.
    pub.set_ser_type("demo.proto.MessageV2");

    VLOG_I("ser_type=", pub.get_ser_type(), " schema_type=", static_cast<int>(pub.get_schema_type()));

    pub.init();
  }

  // Section 3: set_discovery_enabled(false) prevents ProxyServer and other
  // discovery consumers from seeing this node. Useful for "internal" nodes
  // that should not show up in visualisers or recordings.
  {
    vlink::Publisher<std::string> pub("dds://properties/discovery", vlink::InitType::kWithoutInit);
    pub.set_discovery_enabled(false);
    pub.init();
    VLOG_I("discovery-disabled publisher initialised");
  }

  // Section 4: same property mechanism on Subscriber. Reliability and
  // durability must align with the Publisher for a match -- DDS QoS
  // compatibility rules apply.
  {
    vlink::Subscriber<std::string> sub("dds://properties/qos_sub", vlink::InitType::kWithoutInit);
    sub.set_property("qos.reliability.kind", "1");
    sub.set_property("qos.history.depth", "100");
    sub.set_property("qos.durability.kind", "1");
    sub.init();

    // Data callback. Thread: subscriber delivery thread (no attach used).
    sub.listen([](const std::string& msg) { VLOG_I("received: ", msg); });

    vlink::Publisher<std::string> pub("dds://properties/qos_sub");
    pub.wait_for_subscribers();
    pub.publish("property-configured subscriber");
    std::this_thread::sleep_for(100ms);
  }

  VLOG_I("Properties example complete.");
  return 0;
}
