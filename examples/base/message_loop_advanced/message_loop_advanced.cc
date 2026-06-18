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
#include <string>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// -----------------------------------------------------------------------------
// MessageLoop advanced example
//
// Module:   vlink/base/message_loop.h (+ Schedule)
// Scenario: Tour the optional behaviour knobs:
//             - Queue type:     Normal / Lockfree / Priority.
//             - Dispatch strat: Block (cond_var wait) vs Pop (busy spin).
//             - exec_task:      Schedule::Config-based dispatch returning a
//                               chainable Status (on_then/on_else/on_catch).
//             - invoke_task:    fire-and-await -- returns std::future<T>.
// CAUTION:  invoke_task BLOCKS the caller on future.get(). Calling
//           invoke_task on a single-thread loop FROM that same loop's
//           thread DEADLOCKS -- the worker would need to free itself to
//           run the requested task. Always invoke from a different thread.
// -----------------------------------------------------------------------------
int main() {
  // Three queue flavours: Normal is the default mutex+cond_var queue;
  // Lockfree uses MPMC ring buffers (lower contention, fixed capacity);
  // Priority lets post_task_with_priority slot a task ahead of lower
  // priorities. Priority is the only type that honours per-task priority.
  {
    VLOG_I("=== Queue types ===");

    vlink::MessageLoop normal_loop(vlink::MessageLoop::kNormalType);
    normal_loop.set_name("normal");
    normal_loop.async_run();
    normal_loop.post_task([]() { VLOG_I("  [Normal] task"); });
    normal_loop.wait_for_idle();
    MLOG_I("  Normal strategy={}", static_cast<int>(normal_loop.get_strategy()));
    normal_loop.quit();
    normal_loop.wait_for_quit();

    vlink::MessageLoop lockfree_loop(vlink::MessageLoop::kLockfreeType);
    lockfree_loop.set_name("lockfree");
    lockfree_loop.async_run();
    lockfree_loop.post_task([]() { VLOG_I("  [Lockfree] task"); });
    lockfree_loop.wait_for_idle();
    lockfree_loop.quit();
    lockfree_loop.wait_for_quit();

    vlink::MessageLoop priority_loop(vlink::MessageLoop::kPriorityType);
    priority_loop.set_name("priority");
    priority_loop.async_run();
    priority_loop.post_task_with_priority([]() { VLOG_I("  [Priority] low"); }, vlink::MessageLoop::kLowestPriority);
    priority_loop.post_task_with_priority([]() { VLOG_I("  [Priority] high"); }, vlink::MessageLoop::kHighestPriority);
    priority_loop.post_task_with_priority([]() { VLOG_I("  [Priority] normal"); }, vlink::MessageLoop::kNormalPriority);
    priority_loop.wait_for_idle();
    priority_loop.quit();
    priority_loop.wait_for_quit();
  }

  // Dispatch strategy: kBlockStrategy sleeps on a condition variable when
  // the queue is empty (low CPU when idle). kPopStrategy busy-spins for
  // ultra-low-latency wake-up at the cost of 100% CPU on the loop thread.
  // Switch dynamically based on workload phase.
  {
    VLOG_I("=== Dispatch strategies ===");
    vlink::MessageLoop loop;
    loop.async_run();

    loop.set_strategy(vlink::MessageLoop::kBlockStrategy);
    loop.post_task([]() { VLOG_I("  kBlockStrategy"); });
    loop.wait_for_idle();

    loop.set_strategy(vlink::MessageLoop::kPopStrategy);
    loop.post_task([]() { VLOG_I("  kPopStrategy"); });
    loop.wait_for_idle();

    loop.quit();
    loop.wait_for_quit();
  }

  // exec_task with Schedule::Config: returns a Status object that chains
  // observers for the various failure modes (schedule timeout = waited too
  // long in queue, execution timeout = body ran too long, on_catch =
  // body threw). Delay 0 means "as soon as possible".
  {
    VLOG_I("=== exec_task with Schedule::Config ===");
    vlink::MessageLoop loop(vlink::MessageLoop::kPriorityType);
    loop.async_run();

    loop.exec_task(vlink::Schedule::Config{0, 100}, []() { VLOG_I("  immediate, priority=100"); });

    loop.exec_task(vlink::Schedule::Config{200}, []() { VLOG_I("  delayed 200ms"); })
        .on_schedule_timeout([]() { VLOG_W("  schedule timeout"); })
        .on_execution_timeout([]() { VLOG_W("  execution timeout"); })
        .on_catch([](std::exception& e) { MLOG_E("  caught: {}", e.what()); });

    std::this_thread::sleep_for(300ms);
    loop.wait_for_idle();

    loop.quit();
    loop.wait_for_quit();
  }

  // Bool-returning body + on_then / on_else: a tiny state machine. on_then
  // chains run sequentially while each returns true; first false stops the
  // chain and triggers on_else. Use for conditional pipelines without
  // hand-writing if/else dispatch on the loop thread.
  {
    VLOG_I("=== on_then / on_else chaining ===");
    vlink::MessageLoop loop;
    loop.async_run();

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
          VLOG_I("  on_then(2)");
          return true;
        })
        .on_else([]() { VLOG_I("  on_else (NOT called)"); });

    loop.wait_for_idle();

    loop.exec_task(vlink::Schedule::Config{},
                   []() -> bool {
                     VLOG_I("  bool task -> false");
                     return false;
                   })
        .on_then([]() -> bool {
          VLOG_I("  on_then (NOT called)");
          return true;
        })
        .on_else([]() { VLOG_I("  on_else: failure path"); });

    loop.wait_for_idle();
    loop.quit();
    loop.wait_for_quit();
  }

  // invoke_task: synchronous request/response across the loop boundary.
  // future.get() BLOCKS the caller until the loop thread runs the body.
  // Calling this from inside the same loop's thread deadlocks -- always
  // dispatch from a different thread.
  {
    VLOG_I("=== invoke_task ===");
    vlink::MessageLoop loop;
    loop.async_run();

    auto fut_int = loop.invoke_task([]() -> int {
      VLOG_I("  computing on loop thread...");
      return 42;
    });
    MLOG_I("  invoke result: {}", fut_int.get());

    auto fut_str = loop.invoke_task([]() -> std::string { return "hello from loop"; });
    MLOG_I("  invoke string: {}", fut_str.get());

    loop.quit();
    loop.wait_for_quit();
  }

  // Priority-aware invoke: when the queue is priority-typed, high-priority
  // invokes jump ahead of lower-priority pending tasks. Get-order is
  // arbitrary because both futures resolve independently.
  {
    VLOG_I("=== invoke_task_with_priority ===");
    vlink::MessageLoop loop(vlink::MessageLoop::kPriorityType);
    loop.async_run();

    auto high = loop.invoke_task_with_priority(
        []() -> int {
          VLOG_I("  high priority invoke");
          return 1;
        },
        vlink::MessageLoop::kHighestPriority);

    auto low = loop.invoke_task_with_priority(
        []() -> int {
          VLOG_I("  low priority invoke");
          return 2;
        },
        vlink::MessageLoop::kLowestPriority);

    MLOG_I("  high={} low={}", high.get(), low.get());

    loop.quit();
    loop.wait_for_quit();
  }

  VLOG_I("MessageLoop advanced example finished.");
  return 0;
}
