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

// Helloworld server sample.
//
// Demonstrates both the Method model (RPC) and the Event model (pub/sub) of VLink
// in a single process, using Protobuf-generated messages (Helloworld::Request,
// Helloworld::Response, Helloworld::Message). The transport backend is selected
// at runtime via environment variables in helloworld_common.h (defaults to DDS),
// so the same binary works over dds://, ddsc://, shm://, fdbus://, qnx://, or
// someip:// without recompilation -- this is the canonical VLink URL-driven
// dispatch pattern. Typical engineering scenario: a long-lived service that
// answers synchronous RPC requests while periodically broadcasting status events
// to many subscribers.

#include <vlink/base/logger.h>
#include <vlink/base/message_loop.h>
#include <vlink/base/utils.h>
#include <vlink/vlink.h>

#include <iostream>

#if defined(__ANDROID__) && __has_include("helloworld/proto/helloworld.pb.h")
#include "helloworld/proto/helloworld.pb.h"
#else
#include "helloworld.pb.h"
#endif

#include "./helloworld_common.h"

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;

  // Singleton guard: prevent two server instances from competing for the same
  // transport endpoint (DDS topic / SHM segment / SOME/IP service id). The
  // underlying primitive is a named OS lock keyed on "helloworld_server".
  if (!vlink::Utils::check_singleton("helloworld_server")) {
    std::cerr << "Program has started." << std::endl;
    return 1;
  }

  // The MessageLoop drives Timer ticks and any other deferred work on this
  // thread. We bind it to SIGINT/SIGTERM via register_terminate_signal so the
  // process exits gracefully -- loop.quit() unblocks loop.run() and lets
  // RAII destructors close transports cleanly instead of being killed mid-IO.
  vlink::MessageLoop message_loop;
  vlink::Utils::register_terminate_signal([&message_loop](int) { message_loop.quit(); });

  // ======== Method model (RPC) ========
  // Server<Req, Resp> auto-detects Protobuf serialization from the message
  // type. The listen() callback runs on the transport's internal worker thread
  // (NOT the MessageLoop), so keep it short and thread-safe.
  vlink::Server<Helloworld::Request, Helloworld::Response> server(Common::get_method_url());
  server.listen([](const Helloworld::Request& req, Helloworld::Response& resp) {
    CLOG_D("[Server] Receive left = %d, right = %d.", req.left(), req.right());
    resp.set_sum(req.left() + req.right());
  });

  // ======== Event model (Pub/Sub) ========
  // Publisher constructed with the resolved URL; serialization is again driven
  // by the Protobuf trait detection inside vlink::serializer.
  vlink::Publisher<Helloworld::Message> pub(Common::get_event_url());

  // Periodic broadcast every 100ms. Timer must be attach()-ed to a MessageLoop
  // before start(); the callback fires on the loop thread.
  vlink::Timer timer;
  timer.attach(&message_loop);
  timer.set_interval(100);
  timer.set_loop_count(vlink::Timer::kInfinite);

  int index = 0;
  timer.start([&pub, &index]() {
    index++;
    Helloworld::Message msg;
    msg.set_detail("hello_world_" + std::to_string(index));
    pub.publish(msg);
  });

  // Blocks until quit() is invoked by the signal handler.
  message_loop.run();

  return 0;
}
