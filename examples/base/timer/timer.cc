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
#include <vlink/base/message_loop.h>
#include <vlink/base/timer.h>
#include <vlink/base/utils.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// -----------------------------------------------------------------------------
// Timer example
//
// Module:   vlink/base/timer.h (paired with message_loop.h)
// Scenario: vlink::Timer wraps a periodic / one-shot callback dispatched on
//           an owning MessageLoop. Construction forms:
//             Timer(loop, interval, loop_count, callback) -- fully wired.
//             Timer(interval, loop_count)                 -- detached; attach
//                                                            later via attach().
//           kInfinite (= -1) for loop_count means repeat forever; positive
//           values cap the firing count. call_once() is a static helper for
//           a single-shot delayed callback without keeping a Timer alive.
// Threading: the callback ALWAYS runs on the attached loop's thread. Re-
//           attaching with detach()+attach() lets you migrate a Timer
//           between loops at runtime; stop() and start() then govern firing.
// -----------------------------------------------------------------------------
int main() {
  // Repeating timer: 100ms interval, kInfinite loop_count. The atomic
  // counter is touched from the loop thread but read from main; relaxed
  // ordering is acceptable for the tick count.
  {
    VLOG_I("=== Repeating timer (kInfinite) ===");
    vlink::MessageLoop loop;
    loop.async_run();

    std::atomic<int> count{0};
    vlink::Timer timer(&loop, 100, vlink::Timer::kInfinite, [&count]() {
      int c = count.fetch_add(1) + 1;
      MLOG_I("  tick #{}", c);
    });
    timer.start();
    MLOG_I("  active={} interval={}ms", timer.is_active(), timer.get_interval());

    std::this_thread::sleep_for(550ms);
    timer.stop();
    MLOG_I("  stopped after {} ticks", count.load());

    loop.quit();
    loop.wait_for_quit();
  }

  // Single-fire timer: loop_count=1 means the timer auto-stops after one
  // shot. call_once is the same thing without owning a Timer instance --
  // ideal for "schedule this lambda once, then forget".
  {
    VLOG_I("=== Single-fire (loop_count=1) ===");
    vlink::MessageLoop loop;
    loop.async_run();

    std::atomic<bool> fired{false};
    vlink::Timer once(&loop, 150, 1, [&fired]() {
      VLOG_I("  single-fire callback");
      fired.store(true);
    });
    once.start();
    std::this_thread::sleep_for(250ms);
    MLOG_I("  fired={} active={}", fired.load(), once.is_active());

    vlink::Timer::call_once(&loop, 100, []() { VLOG_I("  call_once fired"); });
    std::this_thread::sleep_for(200ms);

    loop.quit();
    loop.wait_for_quit();
  }

  // Bounded loop_count: fires exactly 3 times then auto-stops. restart()
  // re-arms with the original count -- the counter resets back to zero
  // before the second burst.
  {
    VLOG_I("=== Bounded loop_count + restart ===");
    vlink::MessageLoop loop;
    loop.async_run();

    std::atomic<int> count{0};
    vlink::Timer timer(&loop, 80, 3, [&count]() { count.fetch_add(1); });
    timer.start();
    std::this_thread::sleep_for(400ms);
    MLOG_I("  after 3 ticks: count={} active={}", count.load(), timer.is_active());

    count.store(0);
    timer.restart();
    std::this_thread::sleep_for(400ms);
    MLOG_I("  after restart: count={}", count.load());

    loop.quit();
    loop.wait_for_quit();
  }

  // Migration between loops: construct detached, attach to loop_a, fire,
  // detach, re-attach to loop_b. set_callback rebinds the body each time.
  // The callback ALWAYS runs on whichever loop is currently attached --
  // re-attaching while running risks racing the in-flight callback; here
  // we stop() between phases to make the migration deterministic.
  {
    VLOG_I("=== attach / detach across loops ===");
    vlink::MessageLoop loop_a;
    vlink::MessageLoop loop_b;
    loop_a.set_name("loop_a");
    loop_b.set_name("loop_b");
    loop_a.async_run();
    loop_b.async_run();

    std::atomic<int> a_count{0};
    std::atomic<int> b_count{0};

    vlink::Timer timer(100, vlink::Timer::kInfinite);
    timer.attach(&loop_a);
    timer.set_callback([&a_count]() { a_count.fetch_add(1); });
    timer.start();
    std::this_thread::sleep_for(350ms);
    timer.stop();
    MLOG_I("  loop_a ticks={}", a_count.load());

    timer.detach();
    timer.attach(&loop_b);
    timer.set_callback([&b_count]() { b_count.fetch_add(1); });
    timer.start();
    std::this_thread::sleep_for(350ms);
    timer.stop();
    MLOG_I("  loop_b ticks={}", b_count.load());

    loop_a.quit();
    loop_b.quit();
    loop_a.wait_for_quit();
    loop_b.wait_for_quit();
  }

  // Dynamic interval change: set_interval and set_loop_count take effect
  // on the NEXT start/restart -- the running phase is unaffected. The two
  // phases below cleanly demonstrate the rate change (100ms -> 50ms) and
  // count cap (kInfinite -> 6).
  {
    VLOG_I("=== Dynamic interval change ===");
    vlink::MessageLoop loop;
    loop.async_run();

    std::atomic<int> count{0};
    vlink::Timer timer(&loop, 100, vlink::Timer::kInfinite, [&count]() { count.fetch_add(1); });
    timer.start();
    std::this_thread::sleep_for(250ms);
    timer.stop();
    MLOG_I("  phase 1 (100ms): {} ticks", count.load());

    count.store(0);
    timer.set_interval(50);
    timer.set_loop_count(6);
    timer.restart();
    std::this_thread::sleep_for(450ms);
    MLOG_I("  phase 2 (50ms x 6): {} ticks", count.load());

    loop.quit();
    loop.wait_for_quit();
  }

  VLOG_I("Timer example finished.");
  return 0;
}
