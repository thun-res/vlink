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

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// -----------------------------------------------------------------------------
// MessageLoop basic example
//
// Module:   vlink/base/message_loop.h
// Scenario: Cover the four ways to drive a single-threaded MessageLoop:
//             - async_run() spawns an owned worker thread (most common).
//             - run()      blocks the caller's thread (use from main() or a
//                          user-managed thread). quit() must come from elsewhere.
//             - spin_once() lets an external event loop integrate with VLink
//                          by manually pumping queued tasks one at a time.
//           Also demonstrates lifecycle handlers (begin/end/idle) and the
//           public state query surface (is_running, get_type, get_task_count).
// Threading: every posted callback runs on the loop's owning thread.
//            wait_for_idle() blocks until the queue drains; quit + wait_for_quit
//            is the canonical shutdown sequence.
// -----------------------------------------------------------------------------
int main() {
  // async_run: the loop owns a worker thread. post_task is non-blocking.
  // wait_for_idle blocks the caller until the queue drains -- safe to call
  // before quit() to ensure all submitted work has executed.
  {
    VLOG_I("=== async_run + post_task ===");
    vlink::MessageLoop loop;
    loop.set_name("basic_loop");
    loop.async_run();

    for (int i = 0; i < 5; ++i) {
      // The lambda is captured by value (i) and runs on the loop thread.
      loop.post_task([i]() { MLOG_I("  task {} on loop thread", i); });
    }

    loop.wait_for_idle();
    VLOG_I("  all tasks completed");

    loop.quit();
    loop.wait_for_quit();
  }

  // Lifecycle handlers: begin_handler fires once when the worker enters
  // run(); end_handler fires once just before run() returns; idle_handler
  // fires every time the queue drains. The idle hook MUST be cheap -- it
  // runs every spin and a slow handler starves real tasks.
  {
    VLOG_I("=== Lifecycle handlers ===");
    vlink::MessageLoop loop;
    loop.set_name("handler_loop");
    loop.register_begin_handler([]() { VLOG_I("  [begin] thread started"); });
    loop.register_end_handler([]() { VLOG_I("  [end] thread exiting"); });
    loop.register_idle_handler([]() {
      // fires every time the queue drains; keep it cheap
    });

    loop.async_run();
    loop.post_task([]() { VLOG_I("  task between begin/end handlers"); });
    loop.wait_for_idle();

    loop.quit();
    loop.wait_for_quit();
  }

  // run(): blocking variant. The caller's thread becomes the loop thread.
  // quit() must therefore come from another thread (here, the poster
  // helper) or from a posted task.
  {
    VLOG_I("=== Blocking run() ===");
    vlink::MessageLoop loop;
    loop.set_name("blocking_loop");

    // The helper thread posts work and eventually requests quit. By that
    // time, loop.run() has been blocking the main thread for 100ms.
    std::thread poster([&loop]() {
      std::this_thread::sleep_for(50ms);
      loop.post_task([]() { VLOG_I("  posted from helper #1"); });
      loop.post_task([]() { VLOG_I("  posted from helper #2"); });
      std::this_thread::sleep_for(50ms);
      loop.quit();
    });

    loop.run();
    poster.join();
    VLOG_I("  run() returned after quit()");
  }

  // spin_once: caller drives one round of task processing. Used to embed
  // VLink into a foreign main loop (e.g. GUI toolkit, game engine) that
  // already owns the thread.
  {
    VLOG_I("=== spin_once ===");
    vlink::MessageLoop loop;
    loop.set_name("spin_loop");

    std::atomic<int> processed{0};
    for (int i = 0; i < 3; ++i) {
      loop.post_task([&processed, i]() {
        MLOG_I("  spin_once task {}", i);
        processed.fetch_add(1);
      });
    }

    // false = non-blocking spin: process whatever is queued and return.
    while (processed.load() < 3) {
      loop.spin_once(false);
    }

    MLOG_I("  processed {} tasks via spin_once", processed.load());
  }

  // Public state surface: is_running flips true after async_run/run and
  // false after wait_for_quit. get_task_count is a snapshot of the queue
  // depth -- racey by design (only meaningful as a coarse load indicator).
  {
    VLOG_I("=== State queries ===");
    vlink::MessageLoop loop;

    MLOG_I("  before run: is_running={}", loop.is_running());
    loop.async_run();
    MLOG_I("  after async_run: is_running={} type={} tasks={}", loop.is_running(), static_cast<int>(loop.get_type()),
           loop.get_task_count());

    loop.quit();
    loop.wait_for_quit();
    MLOG_I("  after quit: is_running={}", loop.is_running());
  }

  VLOG_I("MessageLoop basic example finished.");
  return 0;
}
