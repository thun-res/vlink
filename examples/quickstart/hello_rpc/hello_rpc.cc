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

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// Request / response are PODs -- dispatched via vlink "Standard" serializer.
struct CalcRequest {
  int a;
  int b;
  char op;
};

struct CalcResponse {
  int result;
};

// hello_rpc: minimal Method-model (RPC) walkthrough.
//
// Demonstrates:
//   - vlink::Server<Req, Resp> / vlink::Client<Req, Resp> over "intra://".
//   - Synchronous, in-place reply via the (req, resp&) listen() form.
//   - client.invoke(req) blocking call returning std::optional<Resp>.
//
// Typical scenarios: command/control RPC, on-demand queries, request-driven
// configuration. Use this style when the caller needs the result inline.
int main() {
  static constexpr char kUrl[] = "intra://hello/rpc";

  // Loop drives server-side request callbacks on a dedicated thread.
  vlink::MessageLoop loop;
  loop.async_run();

  vlink::Server<CalcRequest, CalcResponse> server(kUrl);
  // attach() MUST run before listen(): listen() activates dispatch, and the
  // server needs a loop bound first to know which thread fires callbacks.
  server.attach(&loop);
  // Lambda invoked on the loop thread once per incoming request. Filling
  // `resp` in-place auto-sends the response when the callback returns --
  // this is the synchronous "listen" mode (vs. listen_for_reply / async).
  server.listen([](const CalcRequest& req, CalcResponse& resp) {
    switch (req.op) {
      case '+':
        resp.result = req.a + req.b;
        break;
      case '-':
        resp.result = req.a - req.b;
        break;
      case '*':
        resp.result = req.a * req.b;
        break;
      default:
        resp.result = 0;
        break;
    }
    VLOG_I("[server] ", req.a, " ", req.op, " ", req.b, " = ", resp.result);
  });

  vlink::Client<CalcRequest, CalcResponse> client(kUrl);
  // Discovery handshake: ensures Server is reachable before invoke(), avoiding
  // a wasted timeout on the very first call.
  client.wait_for_connected();

  CalcRequest req{10, 3, '+'};
  // invoke(): blocks the *calling* thread until the response arrives (or the
  // built-in default timeout expires). Returns std::optional, empty on failure.
  auto resp = client.invoke(req);

  if (resp.has_value()) {
    VLOG_I("[client] 10 + 3 = ", resp->result);
  } else {
    VLOG_W("[client] invoke failed");
  }

  loop.quit();
  loop.wait_for_quit();

  return 0;
}
