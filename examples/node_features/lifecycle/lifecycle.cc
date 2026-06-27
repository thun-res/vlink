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
// File: lifecycle.cc
//
// Walk through every lifecycle knob shared by all six node primitives
// (Publisher / Subscriber / Server / Client / Setter / Getter).
//
// Two construction-time modes control when transport resources are created:
//   kWithInit (default) -- the constructor immediately calls init(). Suitable
//                          when you do not need to tweak QoS / ser_type /
//                          properties before the transport exists.
//   kWithoutInit        -- the constructor only stores the URL + type info.
//                          The user then calls set_property / set_ser_type /
//                          set_discovery_enabled etc., and only then init().
//
// Recommended pattern (used in section "Recommended lifecycle pattern" below):
//   construct(kWithoutInit) -> set_property() ... -> init() -> listen() ... -> deinit()
//
// Difference between deinit() and interrupt():
//   deinit()    -- tears down the transport; has_inited() becomes false;
//                  publish/listen become no-ops; re-callable.
//   interrupt() -- wakes up any blocked wait_for_*() calls (returns false)
//                  but leaves the transport up and callbacks active.
// =============================================================================

#include <vlink/base/logger.h>
#include <vlink/vlink.h>

#include <chrono>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

int main() {
  // Section: default construction. Equivalent to passing kWithInit; init()
  // runs inside the constructor so has_inited() is already true.
  {
    vlink::Publisher<std::string> pub("dds://lifecycle/auto");
    VLOG_I("auto: has_inited=", pub.has_inited());
  }

  // Section: deferred init. kWithoutInit defers transport creation until
  // init(), giving us a window to apply property-based QoS overrides that
  // MUST be set before the transport is created (after init they are
  // ignored for fields that are immutable at runtime).
  {
    vlink::Publisher<std::string> pub("dds://lifecycle/deferred", vlink::InitType::kWithoutInit);
    VLOG_I("deferred before init: has_inited=", pub.has_inited());

    pub.set_property("qos.reliability.kind", "1");
    pub.set_property("qos.history.depth", "10");

    pub.init();
    VLOG_I("deferred after init: has_inited=", pub.has_inited());
    VLOG_I("publish returned: ", pub.publish("Hello from deferred node"));
  }

  // Section: explicit deinit / re-init. After deinit(), publish() should
  // fail (false) because there is no transport behind the scenes. init()
  // brings it back, with whatever properties were last configured.
  {
    vlink::Publisher<std::string> pub("dds://lifecycle/deinit_demo");
    pub.deinit();
    VLOG_I("after deinit: has_inited=", pub.has_inited(), " publish=", pub.publish("should fail"));
    pub.init();
    VLOG_I("after re-init: has_inited=", pub.has_inited());
  }

  // Section: interrupt(). Demonstrates the non-destructive wake-up. A
  // separate thread fires interrupt() 50ms in; the main thread is blocked
  // in wait_for_subscribers(1000ms) and returns false immediately rather
  // than after 1000ms. The transport is still live afterwards; only the
  // waiter was unblocked.
  {
    vlink::Publisher<std::string> pub("dds://lifecycle/interrupt_demo");

    // Wake-up thread. Sleeps briefly then signals the publisher. Runs on a
    // user-spawned std::thread, not on a vlink loop.
    std::thread waker([&pub]() {
      std::this_thread::sleep_for(50ms);
      pub.interrupt();
    });

    bool ok = pub.wait_for_subscribers(1000ms);
    VLOG_I("wait_for_subscribers returned ", ok, " (false because interrupt() fired)");
    waker.join();
  }

  // Section: the recommended end-to-end pattern. construct(kWithoutInit) ->
  // set_property -> init -> listen -> deinit.
  {
    vlink::Subscriber<std::string> sub("dds://lifecycle/pattern", vlink::InitType::kWithoutInit);
    sub.set_property("qos.reliability.kind", "1");
    sub.set_property("qos.history.depth", "50");
    sub.init();
    // listen() installs the data callback. It runs on the subscriber's
    // delivery thread (or on the attached MessageLoop if one was attached).
    sub.listen([](const std::string& msg) { VLOG_I("[Pattern] received: ", msg); });
    sub.deinit();
  }

  // Section: kWithoutInit applies uniformly to all six primitive types. Each
  // can be constructed deferred and then init()-ed; has_inited() is false
  // for every one of them prior to init.
  {
    vlink::Publisher<std::string> pub("dds://lifecycle/all", vlink::InitType::kWithoutInit);
    vlink::Subscriber<std::string> sub("dds://lifecycle/all", vlink::InitType::kWithoutInit);
    vlink::Setter<int> setter("dds://lifecycle/all_field", vlink::InitType::kWithoutInit);
    vlink::Getter<int> getter("dds://lifecycle/all_field", vlink::InitType::kWithoutInit);
    vlink::Server<int, int> server("dds://lifecycle/all_rpc", vlink::InitType::kWithoutInit);
    vlink::Client<int, int> client("dds://lifecycle/all_rpc", vlink::InitType::kWithoutInit);

    VLOG_I("kWithoutInit on all 6 primitives: pub=", pub.has_inited(), " sub=", sub.has_inited(),
           " setter=", setter.has_inited(), " getter=", getter.has_inited(), " server=", server.has_inited(),
           " client=", client.has_inited());
  }

  VLOG_I("Lifecycle example complete.");
  return 0;
}
