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
#include <cmath>

#include "math_types.h"

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// method_sync: synchronous Method-model (RPC) over DDS.
//
// Demonstrates:
//   - Server::listen() with (req, resp&) -- response filled in-place, sent
//     automatically when the callback returns.
//   - Three Client::invoke() overloads:
//       1) invoke(req, resp&)  -- bool ok, response via out-ref.
//       2) invoke(req)         -- std::optional<Resp> (default timeout).
//       3) invoke(req, timeout)-- std::optional<Resp> with custom timeout.
//   - All three forms BLOCK the caller until reply or timeout.
//
// Typical scenarios: command/query RPC, configuration retrieval, math/utility
// services where the caller wants the answer inline.
int main() {
  static constexpr char kUrl[] = "dds://math/calculator";

  vlink::MessageLoop server_loop;
  server_loop.set_name("server_loop");
  server_loop.async_run();

  // listen(ReqRespCallback) -- in-place reply variant. The framework sends
  // the populated `resp` automatically when the callback returns. This is
  // the synchronous server style; for deferred replies use listen_for_reply
  // (see method_async.cc).
  vlink::Server<MathRequest, MathResponse> server(kUrl);
  server.attach(&server_loop);
  // Lambda fires on server_loop thread, one invocation per request.
  server.listen([](const MathRequest& req, MathResponse& resp) {
    resp.success = true;
    switch (req.operation) {
      case 0:
        resp.result = req.x + req.y;
        break;
      case 1:
        resp.result = req.x - req.y;
        break;
      case 2:
        resp.result = req.x * req.y;
        break;
      case 3:
        if (req.y != 0.0) {
          resp.result = req.x / req.y;
        } else {
          resp.result = 0.0;
          resp.success = false;
        }
        break;
      case 4:
        resp.result = std::pow(req.x, req.y);
        break;
      default:
        resp.result = 0.0;
        resp.success = false;
        break;
    }
    VLOG_I("[server] op=", req.operation, " x=", req.x, " y=", req.y, " => ", resp.result);
  });

  vlink::Client<MathRequest, MathResponse> client(kUrl);
  // Bounded discovery handshake before the first call.
  VLOG_I("[client] wait_for_connected: ", client.wait_for_connected(2000ms));
  VLOG_I("[client] is_connected: ", client.is_connected());

  // Mode 1: invoke(req, resp&) -- response via output parameter, bool ok.
  {
    MathRequest req{10.0, 3.0, 0};
    MathResponse resp{};
    bool ok = client.invoke(req, resp);
    VLOG_I("[client] 10 + 3 = ", resp.result, " ok=", ok);
  }

  // Mode 2: invoke(req) -> std::optional<Resp> using the framework default
  // timeout. Cleanest for "I want the result or nothing" call sites.
  {
    MathRequest req{6.0, 7.0, 2};
    auto result = client.invoke(req);

    if (result.has_value()) {
      VLOG_I("[client] 6 * 7 = ", result->result);
    }
  }

  // Mode 3: invoke with explicit per-call timeout. Use when the default is
  // too strict/loose for this particular operation.
  {
    MathRequest req{100.0, 0.0, 3};
    auto result = client.invoke(req, 1000ms);

    if (result.has_value()) {
      VLOG_I("[client] 100 / 0 success=", result->success);
    }
  }

  // Sequential invocations on the same client -- each blocks until reply.
  for (int i = 1; i <= 5; ++i) {
    MathRequest req{static_cast<double>(i), static_cast<double>(i), 2};
    auto result = client.invoke(req);

    if (result.has_value()) {
      VLOG_I("[client] ", i, " * ", i, " = ", result->result);
    }
  }

  server_loop.quit();
  server_loop.wait_for_quit();

  return 0;
}
