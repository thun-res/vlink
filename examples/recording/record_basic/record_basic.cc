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
#include <vlink/extension/bag_writer.h>
#include <vlink/vlink.h>

#include <chrono>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// ---------------------------------------------------------------------------
// record_basic.cc
//
// VLink's "drop-in recording" surface: any Publisher/Subscriber/Server/
// Client/Setter/Getter exposes set_record_path(path). Once set, every
// message that flows through the node is appended to the named bag file,
// completely transparently to the application logic.
//
// Two activation paths:
//   * per-node    -- node.set_record_path("/tmp/foo.vdb") (this file).
//   * global      -- export VLINK_BAG_PATH=/tmp/all.vdb at startup;
//                    BagWriter::global_get() returns the shared instance
//                    and every endpoint records into it automatically.
//
// File extension picks the format:
//   * .vdb   -- VLink-native (SQLite-backed if VLINK_ENABLE_SQLITE, else
//               raw fwrite) bag, optimised for VLink's serializer types.
//   * .mcap  -- Foxglove MCAP (cross-tool, lower density, broader support).
// Compression and other tunables go through the BagWriter::Config struct
// (see record_compression).
// ---------------------------------------------------------------------------

int main() {
  // ---- Section 1: per-node recording via set_record_path() ----
  // Both pub and sub record to separate files -- useful when verifying
  // that the data leaves the publisher intact AND arrives at the sub.
  vlink::Publisher<vlink::Bytes> pub("dds://record_basic/event");
  pub.set_record_path("/tmp/record_basic_pub.vdb");

  vlink::Subscriber<vlink::Bytes> sub("dds://record_basic/event");
  sub.set_record_path("/tmp/record_basic_sub.vdb");
  // Listener runs on the DDS dispatch thread. Recording happens *after*
  // delivery, so the user callback sees data even when disk is slow.
  sub.listen([](const vlink::Bytes& msg) { VLOG_I("Subscriber received: ", msg.size(), " bytes"); });

  pub.wait_for_subscribers();

  for (int i = 0; i < 10; ++i) {
    std::string payload = "message_" + std::to_string(i);
    pub.publish(vlink::Bytes::from_string(payload));
    VLOG_I("Published: ", payload);
    std::this_thread::sleep_for(100ms);
  }

  // ---- Section 2: global recording via VLINK_BAG_PATH ----
  // global_get() returns non-null iff VLINK_BAG_PATH was set at startup.
  // Tools/agents use this pattern to silently capture an entire process's
  // traffic without modifying application code.
  auto* global_writer = vlink::BagWriter::global_get();

  if (global_writer) {
    VLOG_I("Global BagWriter active -- all traffic is being recorded.");
  } else {
    VLOG_I("No global BagWriter. Set VLINK_BAG_PATH to enable.");
  }

  // ---- Section 3: method model with recording ----
  // Both request and response are persisted. Server and client share the
  // same path so the bag contains a complete RPC trace.
  vlink::Server<vlink::Bytes, vlink::Bytes> server("dds://record_basic/method");
  server.set_record_path("/tmp/record_basic_rpc.vdb");
  // Listener runs on the DDS dispatch thread; resp is sent back when the
  // callback returns.
  server.listen([](const vlink::Bytes& req, vlink::Bytes& resp) {
    VLOG_I("Server received: ", req.size(), " bytes");
    resp = vlink::Bytes::from_string("response_ok");
  });

  vlink::Client<vlink::Bytes, vlink::Bytes> client("dds://record_basic/method");
  client.set_record_path("/tmp/record_basic_rpc.vdb");

  auto resp = client.invoke(vlink::Bytes::from_string("request_data"));

  if (resp.has_value()) {
    VLOG_I("Client received response: ", resp.value().to_string());
  }

  // ---- Section 4: field model with recording ----
  // Setter writes a value; Getter pulls the latest. Both record into the
  // same bag, so replay reconstructs the field's value timeline.
  vlink::Setter<vlink::Bytes> setter("dds://record_basic/field");
  setter.set_record_path("/tmp/record_basic_field.vdb");

  vlink::Getter<vlink::Bytes> getter("dds://record_basic/field");
  getter.set_record_path("/tmp/record_basic_field.vdb");

  setter.set(vlink::Bytes::from_string("field_value_1"));
  std::this_thread::sleep_for(100ms);

  auto val = getter.get();

  if (val.has_value()) {
    VLOG_I("Getter value: ", val.value().to_string());
  }

  std::this_thread::sleep_for(500ms);
  VLOG_I("Recording complete. Check /tmp/record_basic_*.vdb files.");
  return 0;
}
