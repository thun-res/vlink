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

#include <chrono>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// -----------------------------------------------------------------------------
// Schedule example
//
// Module:   vlink/base/schedule.h (via MessageLoop::exec_task)
// Scenario: Schedule::Config bundles the four scheduling knobs --
//             delay_ms             when to start (0 = immediately).
//             priority             ordering hint for priority queues.
//             schedule_timeout_ms  max queue wait before giving up.
//             execution_timeout_ms max body runtime before reporting timeout.
//           exec_task returns a Status / RetStatus chainable object:
//             on_then(cb)         next step if body returned true.
//             on_else(cb)         step if body returned false.
//             on_catch(cb)        body threw std::exception.
//             on_schedule_timeout queue wait exceeded schedule_timeout_ms.
//             on_execution_timeout body exceeded execution_timeout_ms.
//           Chain callbacks run on the loop thread; they may themselves
//           return bool to keep cascading.
// -----------------------------------------------------------------------------
int main() {
  // Config constructors: default, fully-specified, and delay-only. The full
  // form is positional (delay, priority, schedule_to, execution_to).
  {
    VLOG_I("=== Schedule::Config ===");
    vlink::Schedule::Config def;
    MLOG_I("  default: delay={}ms prio={} sched_to={}ms exec_to={}ms", def.delay_ms, def.priority,
           def.schedule_timeout_ms, def.execution_timeout_ms);

    vlink::Schedule::Config full(100, 200, 5000, 3000);
    MLOG_I("  full: delay={}ms prio={} sched_to={}ms exec_to={}ms", full.delay_ms, full.priority,
           full.schedule_timeout_ms, full.execution_timeout_ms);

    vlink::Schedule::Config delay_only(50);
    MLOG_I("  delay only: delay={}ms", delay_only.delay_ms);
  }

  // Void-returning body: produces a plain Status (no on_then/on_else). The
  // 100ms-delayed task demonstrates schedule_to/execution_to wiring -- both
  // limits are set wide enough that neither timeout fires.
  {
    VLOG_I("=== Void callback (Status) ===");
    vlink::MessageLoop loop;
    loop.async_run();

    loop.exec_task(vlink::Schedule::Config{}, []() { VLOG_I("  immediate void task"); })
        .on_catch([](std::exception& e) { MLOG_E("  exception: {}", e.what()); });
    loop.wait_for_idle();

    loop.exec_task(vlink::Schedule::Config{100, 0, 500, 500}, []() { VLOG_I("  delayed 100ms void task"); })
        .on_schedule_timeout([]() { VLOG_W("  schedule timeout"); })
        .on_execution_timeout([]() { VLOG_W("  execution timeout"); })
        .on_catch([](std::exception& e) { MLOG_E("  caught: {}", e.what()); });

    std::this_thread::sleep_for(300ms);
    loop.wait_for_idle();

    loop.quit();
    loop.wait_for_quit();
  }

  // Bool-returning body produces a RetStatus -- the on_then chain runs in
  // order while each returns true. First false short-circuits to on_else.
  // The "returns false" case verifies on_then is never invoked.
  {
    VLOG_I("=== Bool callback (RetStatus) ===");
    vlink::MessageLoop loop;
    loop.async_run();

    VLOG_I("  --- returns true ---");
    loop.exec_task(vlink::Schedule::Config{},
                   []() -> bool {
                     VLOG_I("  bool task -> true");
                     return true;
                   })
        .on_then([]() -> bool {
          VLOG_I("  on_then(1)");
          return true;
        })
        .on_then([]() -> bool {
          VLOG_I("  on_then(2) -> false stops chain");
          return false;
        })
        .on_then([]() -> bool {
          VLOG_I("  on_then(3) NOT called");
          return true;
        })
        .on_else([]() { VLOG_I("  on_else: triggered after on_then(2) false"); });
    loop.wait_for_idle();

    VLOG_I("  --- returns false ---");
    loop.exec_task(vlink::Schedule::Config{},
                   []() -> bool {
                     VLOG_I("  bool task -> false");
                     return false;
                   })
        .on_then([]() -> bool {
          VLOG_I("  on_then NOT called");
          return true;
        })
        .on_else([]() { VLOG_I("  on_else: failure path"); });

    loop.wait_for_idle();
    loop.quit();
    loop.wait_for_quit();
  }

  // Exception handling: on_catch receives the std::exception by reference.
  // The body's throw does NOT propagate to the loop thread's run() -- the
  // framework intercepts it and routes to on_catch.
  {
    VLOG_I("=== Exception handling ===");
    vlink::MessageLoop loop;
    loop.async_run();

    loop.exec_task(vlink::Schedule::Config{}, []() { throw std::runtime_error("simulated"); })
        .on_catch([](std::exception& e) { MLOG_I("  on_catch caught: '{}'", e.what()); });
    loop.wait_for_idle();

    loop.quit();
    loop.wait_for_quit();
  }

  // Priority scheduling: only meaningful on a kPriorityType queue.
  // HIGHEST runs first, then NORMAL, then LOWEST -- demonstrated by the
  // log ordering.
  {
    VLOG_I("=== Priority scheduling ===");
    vlink::MessageLoop loop(vlink::MessageLoop::kPriorityType);
    loop.async_run();

    loop.exec_task(vlink::Schedule::Config{0, vlink::MessageLoop::kLowestPriority}, []() { VLOG_I("  [LOW]"); });
    loop.exec_task(vlink::Schedule::Config{0, vlink::MessageLoop::kHighestPriority}, []() { VLOG_I("  [HIGH]"); });
    loop.exec_task(vlink::Schedule::Config{0, vlink::MessageLoop::kNormalPriority}, []() { VLOG_I("  [NORMAL]"); });

    loop.wait_for_idle();
    loop.quit();
    loop.wait_for_quit();
  }

  // is_valid: returns false for default-constructed Status and true for
  // Status returned by a successful exec_task. Used by client code to
  // detect "task was rejected" before chaining callbacks.
  {
    VLOG_I("=== Status validity ===");
    vlink::MessageLoop loop;
    loop.async_run();

    auto status = loop.exec_task(vlink::Schedule::Config{}, []() { VLOG_I("  task body"); });
    MLOG_I("  is_valid={}", status.is_valid());
    loop.wait_for_idle();

    loop.quit();
    loop.wait_for_quit();
  }

  VLOG_I("Schedule example finished.");
  return 0;
}
