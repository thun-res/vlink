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

// DDS + DynamicData sample.
//
// DynamicData is VLink's runtime type-erased payload container. It lets a
// single Publisher / Server endpoint carry multiple unrelated message types
// over the same topic: each frame embeds a short type tag (<= 19 chars) and
// the receiver dispatches on it. Here we use Protobuf message types and DDS
// transport, but the same pattern works for any serializer / transport.
// Typical engineering scenario: a generic "command bus" or "telemetry bus"
// where the channel schema is open-ended and would otherwise require one
// topic per message type.
//
// Core API:
//   - DynamicData().load("TypeName", value)  serialize + tag
//   - dd.get_type()                          read the type tag
//   - dd.as<T>()                             deserialize to concrete T

// DynamicData: runtime dynamic type container
#include <vlink/extension/dynamic_data.h>
// VLink core communication API
#include <vlink/base/logger.h>
#include <vlink/vlink.h>

#include <chrono>
#include <string>
#include <thread>

// Protobuf generated message types
#if defined(__ANDROID__) && __has_include("dds_dynamic/dds_dynamic.pb.h")
#include "dds_dynamic/dds_dynamic.pb.h"
#else
#include "dds_dynamic.pb.h"
#endif

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

int main() {
  // ======== Method model (RPC) + DynamicData ========
  // Server accepts arbitrary request types and answers with arbitrary response
  // types -- the contract is enforced only by the tag strings agreed between
  // peers, not by the C++ type system.
  vlink::Server<vlink::DynamicData, vlink::DynamicData> server("dds://dynamic/method");

  // listen() callback fires on a DDS worker thread per incoming request.
  server.listen([](const vlink::DynamicData& req, vlink::DynamicData& res) {
    // Dispatch on the embedded tag string.

    if (req.get_type() == "type1") {
      // Deserialize DynamicData -> pb::Request.
      if (req.as<pb::Request>().type() == 521) {
        // Respond with a string-typed result wrapped back into DynamicData.
        res = vlink::DynamicData().load("type1", "I love you");
      }
    } else if (req.get_type() == "type2") {
      // Deserialize DynamicData -> pb::Message.
      if (req.as<pb::Message>().value() == "forever") {
        // Respond with an int-typed result.
        res = vlink::DynamicData().load("type2", 1314);
      }
    }
  });

  // Client sends different request types over the same endpoint.
  vlink::Client<vlink::DynamicData, vlink::DynamicData> client("dds://dynamic/method");

  // detect_connected() callback fires when DDS discovery sees the matched Server
  // come online / offline -- useful for liveness reporting.
  client.detect_connected([](bool connected) { VLOG_I("server status:", connected); });

  // First request: pb::Request typed.
  pb::Request req1;
  req1.set_type(521);

  // load() serializes the Protobuf message via vlink::serializer and tags it.
  auto resp1 = client.invoke(vlink::DynamicData().load("type1", req1));

  if (resp1.has_value()) {
    // The server answered with a tagged std::string; deserialize accordingly.
    VLOG_I("resp1:", resp1.value().as<std::string>());
  }

  // Second request: pb::Message typed.
  pb::Message req2;
  req2.set_value("forever");

  auto resp2 = client.invoke(vlink::DynamicData().load("type2", req2));

  if (resp2.has_value()) {
    // The server answered with a tagged int.
    VLOG_I("resp2:", resp2.value().as<int>());
  }

  // ======== Event model (Pub/Sub) + DynamicData ========
  // Subscriber receives mixed-type events on a single topic.
  vlink::Subscriber<vlink::DynamicData> sub("dds://dynamic/event");

  // Callback fires on DDS worker thread per event.
  sub.listen([](const vlink::DynamicData& msg) {
    // Dispatch on tag to pick the correct deserialization target.

    if (msg.get_type() == "Request") {
      VLOG_I("msg1:", msg.as<pb::Request>().type());
    } else if (msg.get_type() == "Response") {
      VLOG_I("msg2:", msg.as<pb::Response>().value());
    }
  });

  // Publisher emits different concrete types over the same topic.
  vlink::Publisher<vlink::DynamicData> pub("dds://dynamic/event");
  pub.wait_for_subscribers();

  // Emit a Request-typed event.
  pb::Request req;
  req.set_type(521);
  pub.publish(vlink::DynamicData().load("Request", req));

  // Emit a Response-typed event on the same topic.
  pb::Response resp;
  resp.set_value("love");
  pub.publish(vlink::DynamicData().load("Response", resp));

  // Allow DDS to flush before main returns and destructs the publisher.
  std::this_thread::sleep_for(100ms);

  return 0;
}
