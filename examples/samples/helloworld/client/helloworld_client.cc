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

// Helloworld client sample.
//
// Companion to helloworld_server.cc. Two subcommands:
//   set <l> <r>  -- one-shot RPC: send (left, right) to the Server, print sum.
//   sub          -- subscribe to the periodic event stream until Ctrl+C.
// The transport URL is resolved at runtime via helloworld_common.h and matches
// whatever scheme the server was started with. Demonstrates Client<Req,Resp>
// invoke() with a timeout and Subscriber<T> with signal-driven graceful exit
// (ConditionVariable + register_terminate_signal). Typical engineering scenario:
// a CLI tool that probes a running middleware service from the shell.

#include <vlink/base/condition_variable.h>
#include <vlink/base/elapsed_timer.h>
#include <vlink/base/logger.h>
#include <vlink/base/utils.h>
#include <vlink/vlink.h>

#include <chrono>

#if defined(__ANDROID__) && __has_include("helloworld/proto/helloworld.pb.h")
#include "helloworld/proto/helloworld.pb.h"
#else
#include "helloworld.pb.h"
#endif

#include "./helloworld_common.h"

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// Issue one RPC call: pack (left, right) into a Protobuf Request, invoke()
// synchronously with a 3s timeout, and print the returned sum.
int set(int left, int right) {
  vlink::Client<Helloworld::Request, Helloworld::Response> client(Common::get_method_url());

  // wait_for_connected blocks until discovery sees a matching Server (or 1s
  // elapses). Skipping this would make the first invoke() race with discovery.
  if (!client.wait_for_connected(1s)) {
    VLOG_W("[Client] Server not ready.");
    return -1;
  }

  Helloworld::Request req;
  req.set_left(left);
  req.set_right(right);

  Helloworld::Response resp;

  if (!client.invoke(req, resp, 3s)) {
    VLOG_W("[Client] Invoke failed.");
    return -1;
  }

  VLOG_D("[Client] Receive sum: ", resp.sum());
  return 0;
}

// Subscribe to event messages until Ctrl+C is received.
int sub() {
  vlink::Subscriber<Helloworld::Message> sub(Common::get_event_url());
  // listen() callback fires on the transport's worker thread for every event.
  sub.listen([](const Helloworld::Message& msg) { CLOG_D("[Client] Receive event: %s.", msg.detail().c_str()); });

  // Park the main thread on a condition variable. The signal handler runs in
  // async-signal context and merely calls notify_one(), letting wait() return
  // normally so destructors of the subscriber unwind cleanly on the main thread.
  std::mutex mtx;
  std::unique_lock lock(mtx);
  vlink::ConditionVariable cv;
  vlink::Utils::register_terminate_signal([&cv](int) { cv.notify_one(); });
  cv.wait(lock);

  return 0;
}

int main(int argc, char* argv[]) {
  if (argc == 2 && ::strcmp(argv[1], "sub") == 0) {
    return sub();
  }

  if (argc == 4 && ::strcmp(argv[1], "set") == 0) {
    int left = std::stoi(argv[2]);
    int right = std::stoi(argv[3]);
    return set(left, right);
  }

  VLOG_I("Usage:");
  VLOG_I(" sample_helloworld_client [sub]");
  VLOG_I(" sample_helloworld_client [set] [left_num] [right_num]");
  return 1;
}
