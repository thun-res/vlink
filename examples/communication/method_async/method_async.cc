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

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

#include "translate_types.h"

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// method_async: asynchronous Method-model (RPC) over Zenoh.
//
// Demonstrates:
//   - Server::listen_for_reply((req_id, req)) -- deferred reply via reply().
//     The server is NOT required to respond inside the callback; it can hand
//     `req_id` off to a worker pool, async I/O, etc. and call reply() later.
//   - Client::detect_connected -- async observer of transport readiness.
//   - Client::invoke(req, cb) -- callback-style fire-and-await (non-blocking).
//   - Client::async_invoke(req) -> std::future<Resp> -- future-based.
//   - Mixed sync (invoke) + async (async_invoke) usage on one client.
//
// Typical scenarios: long-running services (DB lookups, network gateways)
// where the server cannot block the dispatch thread.
int main() {
  static constexpr char kUrl[] = "zenoh://translate/service";

  vlink::MessageLoop server_loop;
  server_loop.set_name("server_loop");
  server_loop.async_run();

  // listen_for_reply: receives (req_id, req). The KEY difference from the
  // sync listen() form is that the response is decoupled from the callback
  // return -- you call server.reply(req_id, resp) later, possibly from a
  // different thread. This lets the dispatch thread return immediately and
  // continue serving other requests while the work is offloaded.
  vlink::Server<TranslateRequest, TranslateResponse> server(kUrl);
  server.attach(&server_loop);
  // Callback fires on the server_loop thread. Here we reply synchronously
  // for brevity, but real usage would defer reply() to a worker pool.
  server.listen_for_reply([&server](uint64_t req_id, const TranslateRequest& req) {
    VLOG_I("[server] word_id=", req.word_id, " lang=", req.target_lang, " req_id=", req_id);

    TranslateResponse resp{};
    resp.word_id = req.word_id;
    resp.target_lang = req.target_lang;
    resp.result_code = (req.word_id > 0 && req.word_id <= 100) ? 0 : 1;

    // reply(req_id, resp) is the deferred-send hook -- it can be invoked from
    // any thread, at any time after the listen_for_reply callback received
    // `req_id`. Returns false if the request already timed out client-side.
    bool ok = server.reply(req_id, resp);
    VLOG_I("[server] reply req_id=", req_id, " ok=", ok);
  });

  // detect_connected: async observer of client-side transport readiness.
  // Callback fires when the underlying connection toggles. Like
  // detect_subscribers, this is asynchronous -- the call returns immediately.
  vlink::Client<TranslateRequest, TranslateResponse> client(kUrl);
  client.detect_connected([](bool connected) { VLOG_I("[client] connected=", connected); });
  client.wait_for_connected(2000ms);

  // ---------- Pattern 1: callback-style invoke (non-blocking) ----------
  // invoke(req, cb) returns immediately. The callback is later invoked on a
  // framework-internal thread when the reply arrives (or never, on timeout).
  // We use an atomic + spin to wait for all callbacks to land.
  std::atomic<int> cb_done{0};

  for (int i = 1; i <= 3; ++i) {
    TranslateRequest req{i, i % 3};
    client.invoke(req, [i, &cb_done](const TranslateResponse& resp) {
      VLOG_I("[client] callback #", i, " word_id=", resp.word_id, " code=", resp.result_code);
      cb_done.fetch_add(1);
    });
  }

  // Bounded busy-wait so we don't proceed before the 3 async callbacks fire.
  for (int wait = 0; wait < 50 && cb_done.load() < 3; ++wait) {
    std::this_thread::sleep_for(20ms);
  }

  // ---------- Pattern 2: future-based async_invoke ----------
  // async_invoke(req) -> std::future<Resp>. Caller can fan out N requests in
  // parallel and join them with future.get() in any order.
  std::vector<std::future<TranslateResponse>> futures;
  futures.reserve(5);

  for (int i = 10; i <= 14; ++i) {
    futures.push_back(client.async_invoke(TranslateRequest{i, 1}));
  }

  // future.get() blocks until that specific reply arrives.
  for (size_t i = 0; i < futures.size(); ++i) {
    TranslateResponse resp = futures[i].get();
    VLOG_I("[client] future #", i, " word_id=", resp.word_id, " code=", resp.result_code);
  }

  // ---------- Pattern 3: mixed sync + async on the same client ----------
  // The three styles (invoke, invoke+cb, async_invoke) all coexist and can be
  // interleaved freely on a single Client instance.
  auto sync_result = client.invoke(TranslateRequest{50, 2});

  if (sync_result.has_value()) {
    VLOG_I("[client] sync word_id=", sync_result->word_id, " code=", sync_result->result_code);
  }

  TranslateResponse async_resp = client.async_invoke(TranslateRequest{51, 0}).get();
  VLOG_I("[client] async word_id=", async_resp.word_id, " code=", async_resp.result_code);

  server_loop.quit();
  server_loop.wait_for_quit();

  return 0;
}
