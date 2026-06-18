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

#pragma once

#include <vlink/base/logger.h>
#include <vlink/base/thread_pool.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

// -----------------------------------------------------------------------------
// parallel_tasks: reusable ThreadPool demo blocks. Header-only so the same
// patterns can be embedded in other tests / examples without a separate
// compilation unit.
// -----------------------------------------------------------------------------
namespace parallel_tasks {

// 10 quick-sleep tasks: a fan-out followed by a polling barrier. The
// atomic counter doubles as the "all done" signal so we don't need any
// extra synchronisation primitive.
inline void demo_basic_parallel(vlink::ThreadPool& pool) {
  std::atomic<int> completed{0};
  for (int i = 0; i < 10; ++i) {
    pool.post_task([i, &completed]() {
      MLOG_I("  task {} running", i);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      completed.fetch_add(1);
    });
  }

  while (completed.load() < 10) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  MLOG_I("  completed={}/10", completed.load());
}

// invoke_task: classic "scatter, then gather via futures" pattern. SAFE
// only when the caller is NOT itself a pool worker; the worker thread
// calling future.get() needs another worker to actually run the body --
// see the deadlock note in thread_pool.cc.
inline void demo_invoke_tasks(vlink::ThreadPool& pool) {
  std::vector<std::future<int>> futures;
  futures.reserve(8);
  for (int i = 0; i < 8; ++i) {
    futures.push_back(pool.invoke_task([i]() -> int {
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      return i * i;
    }));
  }

  for (int i = 0; i < 8; ++i) {
    MLOG_I("  invoke[{}] = {}", i, futures[i].get());
  }
}

// Lockfree pool variant: same workload, different queue. Useful when many
// producers want low-latency posting without contending on a single mutex.
// The sleep at the end is a coarse barrier; production code should use
// invoke_task / TaskHandle::wait for deterministic joins.
inline void demo_lockfree_sum() {
  vlink::ThreadPool pool(4, vlink::ThreadPool::kLockfreeType);
  std::atomic<int> sum{0};
  for (int i = 1; i <= 100; ++i) {
    pool.post_task([i, &sum]() { sum.fetch_add(i); });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  MLOG_I("  sum 1..100 = {} (expected 5050)", sum.load());
}

}  // namespace parallel_tasks
