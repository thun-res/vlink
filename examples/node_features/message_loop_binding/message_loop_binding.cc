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

// =============================================================================
// File: message_loop_binding.cc
//
// Demonstrates Node::attach(MessageLoop*) -- the mechanism that pins all
// callbacks (data, status, request handler, etc.) to a specific MessageLoop
// thread.
//
// Critical ordering rule: attach() MUST be called BEFORE listen() (or
// equivalent: register_status_handler, etc.). attach() rewires the
// callback dispatcher inside the node; doing it after listen() would leave
// the already-bound callback running on the default delivery thread.
//
// Why use it:
//   - Serialise callbacks across multiple nodes -- a shared loop means no
//     mutex is needed between handlers running on the same loop.
//   - Isolate work -- different loops run on different threads, so heavy
//     processing on one topic does not delay another.
//   - Predictable thread affinity -- handy when the callback touches data
//     owned by a specific worker (e.g., a UI thread).
// =============================================================================

#include <vlink/base/logger.h>
#include <vlink/vlink.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

int main() {
  // Section 1: basic attach. One loop, one subscriber.
  {
    vlink::MessageLoop loop;
    loop.set_name("my_loop");
    // Spawn the loop's worker thread; from here it can dispatch callbacks.
    loop.async_run();

    bool callback_on_loop_thread = false;

    vlink::Subscriber<std::string> sub("dds://loop_binding/basic");
    // attach BEFORE listen; otherwise the listen-installed callback would
    // not be rewired to the loop.
    sub.attach(&loop);
    sub.listen([&](const std::string& msg) {
      // Subscriber data callback. Thread: my_loop's worker thread. We can
      // confirm via loop.is_in_same_thread().
      callback_on_loop_thread = loop.is_in_same_thread();
      VLOG_I("[Sub] received on loop thread: ", msg);
    });

    vlink::Publisher<std::string> pub("dds://loop_binding/basic");
    pub.wait_for_subscribers();
    pub.publish("test_message");
    // wait_for_idle blocks until the loop has no pending tasks, ensuring
    // the callback ran before we tear down.
    loop.wait_for_idle(1000);

    VLOG_I("callback on loop thread: ", callback_on_loop_thread);

    loop.quit();
    loop.wait_for_quit();
  }

  // Section 2: multiple subscribers sharing one loop. Their callbacks are
  // serialised, so the std::atomic on `total` would actually be safe as a
  // plain int -- but we keep atomic to make the cross-thread contract
  // explicit if someone copies this code into a multi-loop context.
  {
    vlink::MessageLoop loop;
    loop.set_name("shared_loop");
    loop.async_run();

    std::atomic<int> total{0};

    vlink::Subscriber<std::string> sub1("dds://loop_binding/multi");
    sub1.attach(&loop);
    sub1.listen([&](const std::string& msg) {
      // Sub1 callback. Thread: shared_loop worker.
      total++;
      VLOG_I("[Sub1] ", msg);
    });

    vlink::Subscriber<std::string> sub2("dds://loop_binding/multi");
    sub2.attach(&loop);
    sub2.listen([&](const std::string& msg) {
      // Sub2 callback. Thread: shared_loop worker (same as sub1).
      total++;
      VLOG_I("[Sub2] ", msg);
    });

    vlink::Publisher<std::string> pub("dds://loop_binding/multi");
    pub.wait_for_subscribers();
    pub.publish("shared_loop_message");
    loop.wait_for_idle(1000);

    VLOG_I("total received (2 subs, one loop): ", total.load());

    loop.quit();
    loop.wait_for_quit();
  }

  // Section 3: separate loops -> parallel callbacks on different threads.
  {
    vlink::MessageLoop loop_a;
    loop_a.set_name("loop_a");
    loop_a.async_run();

    vlink::MessageLoop loop_b;
    loop_b.set_name("loop_b");
    loop_b.async_run();

    // Captured to compare the two threads' ids after delivery completes.
    std::thread::id tid_a;
    std::thread::id tid_b;

    vlink::Subscriber<std::string> sub_a("dds://loop_binding/isolated");
    sub_a.attach(&loop_a);
    // Sub A callback. Thread: loop_a worker.
    sub_a.listen([&](const std::string&) { tid_a = std::this_thread::get_id(); });

    vlink::Subscriber<std::string> sub_b("dds://loop_binding/isolated");
    sub_b.attach(&loop_b);
    // Sub B callback. Thread: loop_b worker (distinct from loop_a).
    sub_b.listen([&](const std::string&) { tid_b = std::this_thread::get_id(); });

    vlink::Publisher<std::string> pub("dds://loop_binding/isolated");
    pub.wait_for_subscribers();
    pub.publish("isolation_test");

    loop_a.wait_for_idle(1000);
    loop_b.wait_for_idle(1000);

    VLOG_I("different threads: ", tid_a != tid_b);

    loop_a.quit();
    loop_b.quit();
    loop_a.wait_for_quit();
    loop_b.wait_for_quit();
  }

  // Section 4: Server bound to a loop. The request handler installed via
  // listen() runs on server_loop instead of the default server delivery
  // thread.
  {
    vlink::MessageLoop server_loop;
    server_loop.set_name("server_loop");
    server_loop.async_run();

    vlink::Server<int, int> server("dds://loop_binding/rpc");
    server.attach(&server_loop);
    // Request handler. Thread: server_loop worker. Must produce the response
    // synchronously via the out-param `resp`.
    server.listen([](const int& req, int& resp) { resp = req * 2; });

    vlink::Client<int, int> client("dds://loop_binding/rpc");
    // Give the discovery handshake a moment before invoking.
    std::this_thread::sleep_for(50ms);

    auto result = client.invoke(21);

    if (result.has_value()) {
      VLOG_I("21 * 2 = ", result.value());
    }

    server_loop.quit();
    server_loop.wait_for_quit();
  }

  VLOG_I("MessageLoop binding example complete.");
  return 0;
}
