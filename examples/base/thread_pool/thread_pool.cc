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
#include <vlink/base/thread_pool.h>

#include <chrono>
#include <thread>

#include "parallel_tasks.h"

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// -----------------------------------------------------------------------------
// ThreadPool example
//
// Module:   vlink/base/thread_pool.h
// Scenario: ThreadPool is a leaner sibling of MultiLoop -- N workers pulling
//           from a shared queue. Unlike MultiLoop it does not carry the
//           Schedule/exec_task pipeline; it is built for raw "fan work out,
//           join via futures" workloads. Demos live in parallel_tasks.h so
//           the same patterns are reusable from other test code.
// CAUTION:  invoke_task returns std::future and future.get() BLOCKS. If a
//           worker thread itself calls invoke_task on the same pool and
//           then awaits the future, AND no other workers are free to run
//           the new task, the pool DEADLOCKS. Always invoke from outside
//           the pool, or guarantee free workers exist.
// -----------------------------------------------------------------------------
int main() {
  // Basic parallel: 10 tasks across 4 workers. demo_basic_parallel polls
  // until all complete -- a stand-in for "run the batch and join". The
  // pool destructor will drain remaining work on shutdown.
  {
    VLOG_I("=== Basic parallel ===");
    vlink::ThreadPool pool(4);
    pool.set_name("worker_pool");
    parallel_tasks::demo_basic_parallel(pool);
  }

  // invoke_task with future: each call returns std::future<int>; gather
  // results in submission order. SAFE here because the caller is the main
  // thread, NOT a pool worker (see deadlock note above).
  {
    VLOG_I("=== invoke_task with future ===");
    vlink::ThreadPool pool(4);
    parallel_tasks::demo_invoke_tasks(pool);
  }

  // Lockfree queue: kLockfreeType swaps the default mutex+cond_var queue
  // for an MPMC ring buffer; lower contention but fixed capacity.
  {
    VLOG_I("=== Lockfree queue ===");
    parallel_tasks::demo_lockfree_sum();
  }

  // Strategy + state queries. kBlockStrategy makes workers sleep on a
  // condition variable when idle; kPopStrategy busy-spins for the lowest
  // possible wake-up latency at 100% CPU per worker. set_strategy can be
  // toggled mid-flight. is_in_work_thread is the equivalent of MultiLoop's
  // is_in_same_thread predicate.
  {
    VLOG_I("=== Strategy + state ===");
    vlink::ThreadPool pool(2);
    pool.set_name("query_pool");
    MLOG_I("  name={} type={} strategy={} max_tasks={}", pool.get_name(), static_cast<int>(pool.get_type()),
           static_cast<int>(pool.get_strategy()), pool.get_max_task_count());

    pool.set_strategy(vlink::ThreadPool::kBlockStrategy);
    MLOG_I("  strategy after change={}", static_cast<int>(pool.get_strategy()));

    pool.post_task([]() { VLOG_I("  task on block-strategy pool"); });
    std::this_thread::sleep_for(50ms);

    MLOG_I("  is_in_work_thread (main)={}", pool.is_in_work_thread());
    pool.post_task([&pool]() { MLOG_I("  is_in_work_thread (worker)={}", pool.is_in_work_thread()); });
    std::this_thread::sleep_for(50ms);

    pool.shutdown();
  }

  VLOG_I("ThreadPool example finished.");
  return 0;
}
