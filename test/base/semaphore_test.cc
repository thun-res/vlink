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

#include "./base/semaphore.h"

#include <doctest/doctest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "../common_test.h"

TEST_SUITE("base-Semaphore") {
  TEST_CASE("construction with count zero yields zero get_count") {
    Semaphore sem(0);
    CHECK_EQ(sem.get_count(), 0u);
  }

  TEST_CASE("construction with nonzero count yields that count") {
    Semaphore sem(5);
    CHECK_EQ(sem.get_count(), 5u);
  }

  TEST_CASE("kInfinite sentinel value is negative one") { CHECK_EQ(Semaphore::kInfinite, -1); }

  TEST_CASE("acquire with pre-released permits succeeds immediately and decrements") {
    Semaphore sem(3);
    CHECK(sem.acquire(1, Semaphore::kInfinite));
    CHECK_EQ(sem.get_count(), 2u);
    CHECK(sem.acquire(1, Semaphore::kInfinite));
    CHECK_EQ(sem.get_count(), 1u);
    CHECK(sem.acquire(1, Semaphore::kInfinite));
    CHECK_EQ(sem.get_count(), 0u);
  }

  TEST_CASE("release increments count by n") {
    Semaphore sem(0);
    CHECK_EQ(sem.get_count(), 0u);
    sem.release(1);
    CHECK_EQ(sem.get_count(), 1u);
    sem.release(4);
    CHECK_EQ(sem.get_count(), 5u);
  }

  TEST_CASE("acquire decrements count by n when all permits available") {
    Semaphore sem(10);
    CHECK(sem.acquire(3, Semaphore::kInfinite));
    CHECK_EQ(sem.get_count(), 7u);
    CHECK(sem.acquire(7, Semaphore::kInfinite));
    CHECK_EQ(sem.get_count(), 0u);
  }

  TEST_CASE("acquire with timeout returns false when no permits available") {
    Semaphore sem(0);
    auto t0 = std::chrono::steady_clock::now();
    bool ok = sem.acquire(1, 100);
    auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK_FALSE(ok);
    CHECK_EQ(sem.get_count(), 0u);
    CHECK(elapsed >= 80ms);
  }

  TEST_CASE("timed-out acquire does not consume permits") {
    Semaphore sem(0);
    bool ok = sem.acquire(1, 50);
    CHECK_FALSE(ok);
    sem.release(1);
    ok = sem.acquire(1, 200);
    CHECK(ok);
  }

  TEST_CASE("zero timeout acquire fails immediately when no permits") {
    Semaphore sem(0);
    bool ok = sem.acquire(1, 0);
    CHECK_FALSE(ok);
  }

  TEST_CASE("zero timeout acquire succeeds when permits available") {
    Semaphore sem(1);
    bool ok = sem.acquire(1, 0);
    CHECK(ok);
    CHECK_EQ(sem.get_count(), 0u);
  }

  TEST_CASE("release unblocks a waiting acquire") {
    Semaphore sem(0);
    std::atomic<bool> acquired{false};

    std::thread consumer([&] {
      bool ok = sem.acquire(1, Semaphore::kInfinite);
      acquired.store(ok, std::memory_order_release);
    });

    std::this_thread::sleep_for(30ms);
    CHECK_FALSE(acquired.load(std::memory_order_acquire));

    sem.release(1);
    consumer.join();
    CHECK(acquired.load(std::memory_order_acquire));
  }

  TEST_CASE("release N unblocks N waiting threads") {
    static constexpr int kN = 4;
    Semaphore sem(0);
    std::atomic<int> count{0};

    std::vector<std::thread> threads;
    threads.reserve(kN);
    for (int i = 0; i < kN; ++i) {
      threads.emplace_back([&] {
        bool ok = sem.acquire(1, Semaphore::kInfinite);
        if (ok) {
          count.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }

    std::this_thread::sleep_for(30ms);
    CHECK_EQ(count.load(), 0);

    sem.release(kN);

    for (auto& t : threads) {
      t.join();
    }

    CHECK_EQ(count.load(), kN);
    CHECK_EQ(sem.get_count(), 0u);
  }

  TEST_CASE("acquire with timeout shorter than release succeeds") {
    Semaphore sem(0);
    std::atomic<bool> acquired{false};

    std::thread producer([&] {
      std::this_thread::sleep_for(50ms);
      sem.release(1);
    });

    bool ok = sem.acquire(1, 300);
    acquired.store(ok, std::memory_order_release);
    producer.join();

    CHECK(acquired.load());
  }

  TEST_CASE("acquire multiple permits at once blocks until all available") {
    Semaphore sem(0);
    std::atomic<bool> done{false};

    std::thread producer([&] {
      std::this_thread::sleep_for(20ms);
      sem.release(1);
      std::this_thread::sleep_for(20ms);
      sem.release(1);
      std::this_thread::sleep_for(20ms);
      sem.release(3);
    });

    bool ok = sem.acquire(5, Semaphore::kInfinite);
    done.store(true, std::memory_order_release);
    producer.join();

    CHECK(ok);
    CHECK(done.load());
    CHECK_EQ(sem.get_count(), 0u);
  }

  TEST_CASE("reset restores to initial count") {
    Semaphore sem(7);
    CHECK_EQ(sem.get_count(), 7u);
    sem.reset();
    CHECK_EQ(sem.get_count(), 7u);
  }

  TEST_CASE("reset after partial acquire restores to initial count") {
    Semaphore sem(5);
    sem.acquire(2, Semaphore::kInfinite);
    CHECK_EQ(sem.get_count(), 3u);
    sem.reset();
    CHECK_EQ(sem.get_count(), 5u);
  }

  TEST_CASE("reset restores initial count regardless of current state") {
    Semaphore sem(3);
    CHECK(sem.acquire(2, Semaphore::kInfinite));
    CHECK_EQ(sem.get_count(), 1u);

    sem.reset();
    CHECK_EQ(sem.get_count(), 3u);

    sem.release(2);
    CHECK_EQ(sem.get_count(), 5u);
  }

  TEST_CASE("reset with interrupt_waiters unblocks blocked acquires returning false") {
    Semaphore sem(0);
    std::atomic<bool> returned{false};
    std::atomic<bool> result{true};

    std::thread waiter([&] {
      bool ok = sem.acquire(1, Semaphore::kInfinite);
      result.store(ok, std::memory_order_release);
      returned.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(30ms);
    CHECK_FALSE(returned.load(std::memory_order_acquire));

    sem.reset(true);
    waiter.join();

    CHECK(returned.load(std::memory_order_acquire));
    CHECK_FALSE(result.load(std::memory_order_acquire));
  }

  TEST_CASE("reset with interrupt_waiters unblocks multiple waiting threads") {
    static constexpr int kN = 3;
    Semaphore sem(0);
    std::atomic<int> interrupted{0};

    std::vector<std::thread> threads;
    threads.reserve(kN);
    for (int i = 0; i < kN; ++i) {
      threads.emplace_back([&] {
        bool ok = sem.acquire(1, Semaphore::kInfinite);
        if (!ok) {
          interrupted.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }

    std::this_thread::sleep_for(30ms);
    sem.reset(true);

    for (auto& t : threads) {
      t.join();
    }

    CHECK_EQ(interrupted.load(), kN);
  }

  TEST_CASE("get_count reflects intermediate states correctly") {
    Semaphore sem(2);
    CHECK_EQ(sem.get_count(), 2u);
    sem.acquire(1, Semaphore::kInfinite);
    CHECK_EQ(sem.get_count(), 1u);
    sem.release(3);
    CHECK_EQ(sem.get_count(), 4u);
    sem.reset();
    CHECK_EQ(sem.get_count(), 2u);
  }
}

// NOLINTEND
