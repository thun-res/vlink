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

// SOME/IP + FlatBuffers sample.
//
// Exercises all three VLink models (Method / Event / Field) over the
// someip:// transport (vsomeip backend) with FlatBuffers payloads. SOME/IP
// URL format:
//   someip://ServiceID/InstanceID?method=MethodID        -- method model
//   someip://ServiceID/InstanceID?groups=GID&event=EID   -- event/field model
//   add &field=1                                         -- field semantics
// FlatBuffers exposes two flavours that VLink picks up automatically:
//   - fbs::RequestT (NativeTable / Object API) maps to kFlatTableType: value
//     semantics, data is copied during (de)serialization.
//   - fbs::Request* (Table pointer) maps to kFlatPtrType: zero-copy read of
//     the wire buffer; pointer is only valid for the duration of the callback.
// Typical engineering scenario: AUTOSAR Adaptive / automotive Ethernet service
// stack where SOME/IP is mandated and FlatBuffers' zero-copy read minimises
// per-frame CPU cost.
//
// Prerequisite: the vsomeip daemon must be running.

// VLink core communication API
#include <vlink/base/logger.h>
#include <vlink/vlink.h>

#include <atomic>
#include <chrono>
#include <thread>

// FlatBuffers generated message types
#if __has_include("./someip_flat.fbs.hpp")
#include "./someip_flat.fbs.hpp"
#else
#include "./someip_flat_generated.h"
#endif

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

int main() {
  // ======== Method model (RPC, fire-and-forget) ========
  // Server-only-receives mode: Server<ReqT> without a RespT template parameter.
  std::atomic_bool flag{false};

  // Server receives a FlatBuffers Table pointer -- kFlatPtrType -- so the
  // callback reads directly from the wire buffer with no copy.
  vlink::Server<fbs::Request*> server("someip://0x1/0x2?method=0x3");

  // Callback fires on a vsomeip worker thread; the req pointer is only valid
  // inside the callback (lifetime tied to the incoming SOME/IP frame).
  server.listen([&flag](const fbs::Request* req) {
    VLOG_I("type:", req->type());
    flag = true;
  });

  // Client uses NativeTable type (Object API) -- VLink serializes it for us.
  vlink::Client<fbs::RequestT> client("someip://0x1/0x2?method=0x3");

  fbs::RequestT req;
  req.type = 100;

  // Block until SOME/IP service discovery sees the Server.
  client.wait_for_connected();

  // Fire-and-forget send (Client<ReqT> with no RespT -> send-mode).
  client.send(req);

  // Wait for the Server callback to flip the flag.
  while (!flag) {
    std::this_thread::sleep_for(1s);
  }

  // ======== Event model (Pub/Sub) ========
  // Asymmetric typing is idiomatic: Subscriber takes a pointer (zero-copy
  // read), Publisher takes the value type (Object API for easy construction).
  vlink::Subscriber<fbs::Message*> sub("someip://0x1/0x3?groups=0x1|0x2&event=0x3");

  // Pointer lifetime: valid only inside the callback. Do not capture, cache,
  // or hand off to other threads -- the underlying buffer is recycled on
  // return.
  sub.listen([](const fbs::Message* msg) { VLOG_I("event:", msg->type(), ", value:", msg->value()->c_str()); });

  vlink::Publisher<fbs::MessageT> pub("someip://0x1/0x3?groups=0x1|0x2&event=0x3");

  fbs::MessageT msg;

  // Give SOME/IP service discovery time to match subscribers.
  std::this_thread::sleep_for(1s);

  // Publish three FlatBuffers events back to back.
  msg.type = 1;
  msg.value = "one";
  pub.publish(msg);

  std::this_thread::sleep_for(1s);
  msg.type = 2;
  msg.value = "two";
  pub.publish(msg);

  std::this_thread::sleep_for(1s);

  msg.type = 3;
  msg.value = "three";
  pub.publish(msg);

  // ======== Field model (Getter/Setter) ========
  // The &field=1 query parameter switches SOME/IP into field semantics
  // (notifier + getter pattern): a late-joining Getter receives the latest
  // value immediately.
  vlink::Setter<fbs::MessageT> setter("someip://0x1/0x4?groups=0x1|0x2&event=0x4&field=1");

  fbs::MessageT value;
  value.type = 1000;
  value.value = "hi";
  setter.set(value);

  vlink::Getter<fbs::MessageT> getter("someip://0x1/0x4?groups=0x1|0x2&event=0x4&field=1");

  // Allow the notifier to settle before reading back.
  std::this_thread::sleep_for(100ms);

  // Read the latest field value.
  auto ret = getter.get();

  if (ret.has_value()) {
    VLOG_I("Getter type:", ret.value().type);
    VLOG_I("Getter value:", ret.value().value);
  } else {
    VLOG_W("Getter get failed.");
  }

  return 0;
}
