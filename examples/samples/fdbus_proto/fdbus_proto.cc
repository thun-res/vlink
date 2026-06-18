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

// FDBus + Protobuf sample.
//
// Exercises all three VLink communication models (Method / Event / Field) over
// the fdbus:// transport, with Protobuf messages auto-serialized by VLink.
// FDBus URLs use ?event=<name> to disambiguate the channel within a service,
// so the same "phone" service can host an RPC, an event stream, and a state
// field independently. Typical engineering scenario: IPC inside an automotive
// HMI process tree where FDBus is the native bus.
//
// Prerequisite: an fdbus_ns name server must be running on the host.

#include <vlink/base/logger.h>
#include <vlink/vlink.h>

#include <chrono>
#include <thread>

#if defined(__ANDROID__) && __has_include("fdbus_proto/fdbus_proto.pb.h")
#include "fdbus_proto/fdbus_proto.pb.h"
#else
#include "fdbus_proto.pb.h"
#endif

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

int main() {
  // ======== Method model (RPC) ========
  // FDBus URL format: fdbus://<service>?event=<channel>
  vlink::Server<pb::Request, pb::Response> server("fdbus://phone?event=req");

  // Callback fires on an FDBus worker thread per request.
  server.listen([](const pb::Request& req, pb::Response& res) {
    if (req.type() == 10086) {
      res.set_value("calling...");
    }
  });

  vlink::Client<pb::Request, pb::Response> client("fdbus://phone?event=req");

  // Reports FDBus service online/offline transitions.
  client.detect_connected([](bool connected) { VLOG_I("server status:", connected); });

  pb::Request req;
  req.set_type(10086);

  auto resp = client.invoke(req);

  if (resp.has_value()) {
    VLOG_I("status:", resp.value().value());
  }

  // ======== Event model (Pub/Sub) ========
  vlink::Subscriber<pb::Message> sub("fdbus://phone?event=time");

  sub.listen([](const auto& msg) { VLOG_I("timestamp:", msg.value()); });

  vlink::Publisher<pb::Message> pub("fdbus://phone?event=time");

  pb::Message msg;
  // Block until FDBus reports a matched subscriber.
  pub.wait_for_subscribers();

  msg.set_value("00:00");
  pub.publish(msg);

  std::this_thread::sleep_for(1s);

  msg.set_value("00:01");
  pub.publish(msg);

  std::this_thread::sleep_for(1s);

  msg.set_value("00:02");
  pub.publish(msg);

  std::this_thread::sleep_for(1s);

  msg.set_value("00:03");
  pub.publish(msg);

  // ======== Field model (Setter/Getter) ========
  // Field model exposes a latest-value cache: late joiners immediately get the
  // current value. FDBus implements this as a persistent event in the daemon.
  vlink::Setter<pb::Message> setter("fdbus://phone?event=msg");

  pb::Message value;
  value.set_value("119");
  setter.set(value);

  vlink::Getter<pb::Message> getter("fdbus://phone?event=msg");

  // Allow the value to propagate through the FDBus daemon before reading back.
  std::this_thread::sleep_for(100ms);

  auto ret = getter.get();

  if (ret.has_value()) {
    VLOG_I("get phone number:", ret.value().value());
  } else {
    VLOG_W("Getter get failed.");
  }

  return 0;
}
