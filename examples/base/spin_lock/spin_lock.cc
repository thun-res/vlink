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

#include <vlink/base/elapsed_timer.h>
#include <vlink/base/logger.h>
#include <vlink/base/spin_lock.h>

#include <mutex>
#include <thread>
#include <vector>

// -----------------------------------------------------------------------------
// SpinLock example
//
// Module:   vlink/base/spin_lock.h
// Scenario: vlink::SpinLock is a tight user-space mutex built on top of
//           std::atomic_flag (test-and-set with relaxed yield). It is
//           strictly an OPTIMISATION for VERY SHORT critical sections.
// CRITICAL: inside a SpinLock critical section you MUST NOT:
//             - call any blocking syscall (read, write, malloc, futex, ...);
//             - acquire any other lock that might be held by a contender;
//             - perform any work whose duration exceeds the worst-case
//               contender's yield slice (spinning wastes one full core).
//           When in doubt, fall back to std::mutex -- it is correct under
//           all workloads and only ~5x slower in the uncontended case.
// -----------------------------------------------------------------------------
int main() {
  // Manual lock/unlock: spelled out for clarity. Prefer the RAII guard
  // (next section) in real code so the unlock cannot be missed on early
  // return / exception.
  {
    VLOG_I("=== Manual lock / unlock ===");
    vlink::SpinLock lock;
    lock.lock();
    VLOG_I("  inside critical section");
    lock.unlock();
  }

  // RAII guard: the canonical pattern. SpinLockGuard locks on construction
  // and unlocks on destruction -- exception-safe by design.
  {
    VLOG_I("=== SpinLockGuard (RAII) ===");
    vlink::SpinLock lock;
    {
      vlink::SpinLockGuard guard(lock);
      VLOG_I("  inside RAII guard");
    }

    VLOG_I("  guard destructor released");
  }

  // try_lock: returns true if the lock was acquired, false if already held.
  // Useful for opportunistic critical sections that prefer a fast skip
  // over a blocking wait.
  {
    VLOG_I("=== try_lock ===");
    vlink::SpinLock lock;
    MLOG_I("  try #1 (free)={}", lock.try_lock());
    MLOG_I("  try #2 (held)={}", lock.try_lock());
    lock.unlock();
    MLOG_I("  try #3 (after unlock)={}", lock.try_lock());
    lock.unlock();
  }

  // Multi-threaded correctness check: 4 threads * 100k increments. Without
  // the guard the counter would race; with it we must see exactly the
  // expected sum. The body of the critical section is ONE integer add --
  // the canonical SpinLock workload.
  {
    VLOG_I("=== Multi-threaded counter ===");
    vlink::SpinLock lock;
    int64_t counter = 0;
    static constexpr int kThreads = 4;
    static constexpr int kIterations = 100000;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&]() {
        for (int i = 0; i < kIterations; ++i) {
          vlink::SpinLockGuard guard(lock);
          ++counter;
        }
      });
    }

    for (auto& t : threads) {
      t.join();
    }

    int64_t expected = static_cast<int64_t>(kThreads) * kIterations;
    MLOG_I("  counter={} expected={} ok={}", counter, expected, counter == expected);
  }

  // SpinLock models BasicLockable, so std::lock_guard CTAD picks it up
  // without explicit template parameters. Use this when the surrounding
  // code is generic over the mutex type.
  {
    VLOG_I("=== std::lock_guard CTAD ===");
    vlink::SpinLock lock;
    {
      std::lock_guard guard(lock);
      VLOG_I("  inside std::lock_guard");
    }
  }

  // Micro-benchmark: spin vs std::mutex for a million uncontended
  // lock/unlock cycles. SpinLock wins because it avoids the kernel; under
  // CONTENTION the picture inverts -- spin wastes CPU while a futex
  // sleeper would yield gracefully. Choose accordingly.
  {
    VLOG_I("=== Performance comparison ===");
    vlink::SpinLock spin;
    std::mutex mtx;
    static constexpr int kOps = 1000000;

    vlink::ElapsedTimer timer(vlink::ElapsedTimer::kCpuTimestamp, vlink::ElapsedTimer::kMicro);
    timer.start();
    for (int i = 0; i < kOps; ++i) {
      vlink::SpinLockGuard guard(spin);
    }

    int64_t spin_us = timer.get();
    timer.stop();

    timer.start();
    for (int i = 0; i < kOps; ++i) {
      std::lock_guard guard(mtx);
    }

    int64_t mtx_us = timer.get();
    timer.stop();

    MLOG_I("  SpinLock   {}us / {}M ops", spin_us, kOps / 1000000);
    MLOG_I("  std::mutex {}us / {}M ops", mtx_us, kOps / 1000000);
  }

  VLOG_I("SpinLock example finished.");
  return 0;
}
