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
#include <vlink/base/multi_loop.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// -----------------------------------------------------------------------------
// MultiLoop example
//
// Module:   vlink/base/multi_loop.h
// Scenario: MultiLoop is the multi-threaded sibling of MessageLoop. It owns
//           N worker threads sharing one task queue (queue type configurable
//           the same way: Normal / Lockfree / Priority). Same posting API,
//           same exec_task / invoke_task semantics, but tasks distribute
//           across all workers automatically.
// Difference vs ThreadPool: MultiLoop carries the full MessageLoop API
//           (Schedule::Config, on_then/on_else/on_catch, priority), whereas
//           ThreadPool is a leaner "just run callables" pool.
// -----------------------------------------------------------------------------
int main() {
  // Basic dispatch on 4 workers. With 20 short tasks and 4 threads the
  // expected wall time is roughly ceil(20/4) * 20ms = 100ms; observe in
  // the logs that ~4 tasks log "executing" before each batch sleeps.
  {
    VLOG_I("=== Basic dispatch (4 workers) ===");
    vlink::MultiLoop loop(4);
    loop.set_name("multi_4");
    loop.async_run();

    std::atomic<int> completed{0};
    for (int i = 0; i < 20; ++i) {
      loop.post_task([i, &completed]() {
        MLOG_I("  task {} executing", i);
        std::this_thread::sleep_for(20ms);
        completed.fetch_add(1);
      });
    }

    loop.wait_for_idle();
    MLOG_I("  completed={}/20", completed.load());

    loop.quit();
    loop.wait_for_quit();
  }

  // is_in_same_thread: true only when the caller is itself one of the pool
  // workers. Useful to gate work that must not re-enter the pool (e.g. an
  // invoke_task from a worker would deadlock unless other workers are free).
  {
    VLOG_I("=== Thread identity ===");
    vlink::MultiLoop loop(2);
    loop.async_run();

    MLOG_I("  main is_in_same_thread={}", loop.is_in_same_thread());
    loop.post_task([&loop]() { MLOG_I("  worker is_in_same_thread={}", loop.is_in_same_thread()); });
    loop.wait_for_idle();

    loop.quit();
    loop.wait_for_quit();
  }

  // Priority queue type: HIGH always picked before NORMAL before LOW. With
  // 2 workers and 3 tasks the output is ordered HIGH, NORMAL, LOW (the
  // first two start essentially simultaneously, LOW runs once a worker
  // frees up).
  {
    VLOG_I("=== Priority queue ===");
    vlink::MultiLoop loop(2, vlink::MultiLoop::kPriorityType);
    loop.async_run();

    loop.post_task_with_priority([]() { VLOG_I("  [LOW]"); }, vlink::MultiLoop::kLowestPriority);
    loop.post_task_with_priority([]() { VLOG_I("  [HIGH]"); }, vlink::MultiLoop::kHighestPriority);
    loop.post_task_with_priority([]() { VLOG_I("  [NORMAL]"); }, vlink::MultiLoop::kNormalPriority);

    loop.wait_for_idle();
    loop.quit();
    loop.wait_for_quit();
  }

  // invoke_task across a pool: future.get() blocks on the caller; the body
  // runs on whichever worker pulls it off the queue. Safe from threads
  // OTHER than the pool workers (see Threading note in the header above).
  {
    VLOG_I("=== invoke_task ===");
    vlink::MultiLoop loop(4);
    loop.async_run();

    auto future = loop.invoke_task([]() -> int {
      std::this_thread::sleep_for(30ms);
      return 99;
    });
    MLOG_I("  result={}", future.get());

    loop.quit();
    loop.wait_for_quit();
  }

  // exec_task with Schedule: same chaining surface as MessageLoop --
  // on_catch handles body exceptions, delay schedules in the future.
  {
    VLOG_I("=== exec_task with Schedule ===");
    vlink::MultiLoop loop(2);
    loop.async_run();

    loop.exec_task(vlink::Schedule::Config{0}, []() { VLOG_I("  immediate exec_task"); })
        .on_catch([](std::exception& e) { MLOG_E("  exception: {}", e.what()); });

    loop.exec_task(vlink::Schedule::Config{100}, []() { VLOG_I("  delayed 100ms exec_task"); });

    std::this_thread::sleep_for(200ms);
    loop.wait_for_idle();

    loop.quit();
    loop.wait_for_quit();
  }

  // Parallel sum: classic embarrassingly-parallel reduce. The atomic
  // fetch_add serialises the writes; for real workloads prefer per-worker
  // accumulators reduced at the end (this is just illustrative).
  {
    VLOG_I("=== Parallel sum ===");
    vlink::MultiLoop loop(4);
    loop.async_run();

    std::atomic<int64_t> sum{0};
    static constexpr int kTasks = 100;
    for (int i = 1; i <= kTasks; ++i) {
      loop.post_task([i, &sum]() { sum.fetch_add(i); });
    }

    loop.wait_for_idle();
    MLOG_I("  sum 1..{} = {} (expected {})", kTasks, sum.load(), kTasks * (kTasks + 1) / 2);

    loop.quit();
    loop.wait_for_quit();
  }

  VLOG_I("MultiLoop example finished.");
  return 0;
}
