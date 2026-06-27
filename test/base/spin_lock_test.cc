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

// NOLINTBEGIN

#include "./base/spin_lock.h"

#include <doctest/doctest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "../common_test.h"

TEST_SUITE("base-SpinLock") {
  TEST_CASE("default constructed lock is unlocked so try_lock succeeds") {
    SpinLock lk;
    bool acquired = lk.try_lock();
    CHECK(acquired);
    lk.unlock();
  }

  TEST_CASE("lock and unlock basic sequence leaves lock acquirable again") {
    SpinLock lk;
    lk.lock();
    lk.unlock();
    CHECK(lk.try_lock());
    lk.unlock();
  }

  TEST_CASE("try_lock returns false when lock is already held") {
    SpinLock lk;
    lk.lock();
    bool second = lk.try_lock();
    CHECK_FALSE(second);
    lk.unlock();
  }

  TEST_CASE("try_lock returns true on unlocked lock") {
    SpinLock lk;
    CHECK(lk.try_lock());
    lk.unlock();
  }

  TEST_CASE("repeated lock unlock cycles work correctly") {
    SpinLock lk;
    for (int i = 0; i < 100; ++i) {
      lk.lock();
      lk.unlock();
    }
    CHECK(lk.try_lock());
    lk.unlock();
  }

  TEST_CASE("SpinLockGuard acquires on construction and releases on destruction") {
    SpinLock lk;
    {
      SpinLockGuard guard(lk);
      CHECK_FALSE(lk.try_lock());
    }
    CHECK(lk.try_lock());
    lk.unlock();
  }

  TEST_CASE("SpinLockGuard usable in lambda for increments") {
    SpinLock lk;
    int value = 0;
    auto inc = [&]() {
      SpinLockGuard guard(lk);
      ++value;
    };
    inc();
    inc();
    inc();
    CHECK_EQ(value, 3);
  }

  TEST_CASE("try_lock under contention from another thread fails") {
    SpinLock lk;
    std::atomic<bool> lock_held{false};
    std::atomic<bool> can_release{false};

    std::thread holder([&]() {
      lk.lock();
      lock_held.store(true, std::memory_order_release);
      while (!can_release.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      lk.unlock();
    });

    while (!lock_held.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    bool acquired = lk.try_lock();
    CHECK_FALSE(acquired);

    can_release.store(true, std::memory_order_release);
    holder.join();

    CHECK(lk.try_lock());
    lk.unlock();
  }

  TEST_CASE("lock acquires after another thread releases") {
    SpinLock lk;
    std::atomic<bool> ready{false};
    std::atomic<bool> released{false};

    std::thread helper([&]() {
      lk.lock();
      ready.store(true, std::memory_order_release);
      std::this_thread::sleep_for(10ms);
      released.store(true, std::memory_order_release);
      lk.unlock();
    });

    while (!ready.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    lk.lock();
    CHECK(released.load(std::memory_order_acquire));
    lk.unlock();

    helper.join();
  }

  TEST_CASE("concurrent increments with SpinLockGuard produce correct total") {
    SpinLock lk;
    int counter = 0;
    static constexpr int kThreads = 8;
    static constexpr int kIncrementsPerThread = 10000;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
      workers.emplace_back([&]() {
        for (int j = 0; j < kIncrementsPerThread; ++j) {
          SpinLockGuard guard(lk);
          ++counter;
        }
      });
    }
    for (auto& w : workers) {
      w.join();
    }
    CHECK_EQ(counter, kThreads * kIncrementsPerThread);
  }

  TEST_CASE("concurrent increments with manual lock unlock produce correct total") {
    SpinLock lk;
    int counter = 0;
    static constexpr int kThreads = 4;
    static constexpr int kIncrementsPerThread = 5000;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
      workers.emplace_back([&]() {
        for (int j = 0; j < kIncrementsPerThread; ++j) {
          lk.lock();
          ++counter;
          lk.unlock();
        }
      });
    }
    for (auto& w : workers) {
      w.join();
    }
    CHECK_EQ(counter, kThreads * kIncrementsPerThread);
  }

  TEST_CASE("high contention stress test completes without deadlock") {
    SpinLock lk;
    std::atomic<int> total{0};
    static constexpr int kThreads = 16;
    static constexpr int kOps = 2000;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
      workers.emplace_back([&]() {
        for (int j = 0; j < kOps; ++j) {
          SpinLockGuard g(lk);
          total.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }
    for (auto& w : workers) {
      w.join();
    }
    CHECK_EQ(total.load(), kThreads * kOps);
  }

  TEST_CASE("SpinLock alignment is 64 bytes for cache-line separation") { CHECK_EQ(alignof(SpinLock), 64u); }

  TEST_CASE("unlock on already-unlocked lock does not crash") {
    SpinLock lk;
    lk.unlock();
    CHECK(lk.try_lock());
    lk.unlock();
  }
}

// NOLINTEND
