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

// DDS-C (Cyclone DDS) + Protobuf sample.
//
// Same shape as dds_idl.cc but uses the ddsc:// transport (Cyclone DDS) and
// Protobuf message types. VLink's serializer trait auto-detects pb::Message
// classes and uses Protobuf wire format -- no DdsConf::register_url required
// because the framework wraps Protobuf into an opaque CDR-compatible payload.
// Typical engineering scenario: Cyclone-DDS-based system where teams prefer
// Protobuf schemas (versioning, ecosystem tooling) over native DDS IDL.

#include <vlink/base/logger.h>
#include <vlink/vlink.h>

#include <chrono>
#include <thread>

#if defined(__ANDROID__) && __has_include("ddsc_proto/ddsc_proto.pb.h")
#include "ddsc_proto/ddsc_proto.pb.h"
#else
#include "ddsc_proto.pb.h"
#endif

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

int main() {
  // ======== Method model (RPC) ========
  vlink::Server<pb::Request, pb::Response> server("ddsc://phone/method");

  // Callback fires on a Cyclone DDS worker thread per request.
  server.listen([](const pb::Request& req, pb::Response& res) {
    if (req.type() == 10086) {
      res.set_value("calling...");
    }
  });

  vlink::Client<pb::Request, pb::Response> client("ddsc://phone/method");

  // Reports Cyclone DDS discovery match transitions.
  client.detect_connected([](bool connected) { VLOG_I("server status:", connected); });

  pb::Request req;
  req.set_type(10086);

  auto resp = client.invoke(req);

  if (resp.has_value()) {
    VLOG_I("status:", resp.value().value());
  }

  // ======== Event model (Pub/Sub) ========
  vlink::Subscriber<pb::Message> sub("ddsc://phone/event");
  // Generic-lambda style callback; receives by const-ref.
  sub.listen([](const auto& msg) { VLOG_I("timestamp:", msg.value()); });

  vlink::Publisher<pb::Message> pub("ddsc://phone/event");
  pb::Message msg;
  // Block until at least one subscriber matched, so the first publish is not lost.
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

  return 0;
}
