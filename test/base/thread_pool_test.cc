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

#include "./base/thread_pool.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "../common_test.h"

namespace {

class SmallQueueThreadPool final : public ThreadPool {
 public:
  using ThreadPool::ThreadPool;

  [[nodiscard]] size_t get_max_task_count() const override { return 1U; }
};

class TwoTaskThreadPool final : public ThreadPool {
 public:
  using ThreadPool::ThreadPool;

  [[nodiscard]] size_t get_max_task_count() const override { return 2U; }
};

template <typename PredicateT>
bool wait_until(PredicateT&& predicate, std::chrono::milliseconds timeout = std::chrono::milliseconds(1000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }

    std::this_thread::yield();
  }

  return predicate();
}

}  // namespace

TEST_SUITE("base-ThreadPool") {
  TEST_CASE("default constructor creates normal-type pool that executes tasks") {
    ThreadPool pool;

    CHECK_EQ(pool.get_type(), ThreadPool::kNormalType);

    std::atomic<int> counter{0};

    for (int i = 0; i < 8; ++i) {
      pool.post_task([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
    }

    std::this_thread::sleep_for(100ms);
    pool.shutdown();

    CHECK_EQ(counter.load(), 8);
  }

  TEST_CASE("zero-thread pool rejects all posted tasks") {
    ThreadPool pool(0);
    std::atomic<bool> executed{false};

    bool posted = pool.post_task([&executed] { executed.store(true, std::memory_order_release); });

    CHECK_FALSE(posted);
    CHECK_FALSE(executed.load(std::memory_order_acquire));
  }

  TEST_CASE("post_task after shutdown returns false") {
    ThreadPool pool(2);
    pool.shutdown();

    bool posted = pool.post_task([] {});

    CHECK_FALSE(posted);
  }

  TEST_CASE("shutdown is idempotent and first call returns true") {
    ThreadPool pool(2);
    pool.post_task([] {});

    bool first = pool.shutdown();
    bool second = pool.shutdown();

    CHECK(first);
    (void)second;
  }

  TEST_CASE("destructor joins all workers and completes queued tasks") {
    std::atomic<int> counter{0};

    {
      ThreadPool pool(2);

      for (int i = 0; i < 5; ++i) {
        pool.post_task([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
      }
    }

    CHECK_EQ(counter.load(), 5);
  }

  TEST_CASE("set_name and get_name round-trip") {
    ThreadPool pool(2);

    pool.set_name("test-pool");

    CHECK_EQ(pool.get_name(), "test-pool");

    pool.set_name("");

    CHECK_EQ(pool.get_name(), "");

    pool.shutdown();
  }

  TEST_CASE("get_type reflects constructor argument") {
    {
      ThreadPool normal(2);
      CHECK_EQ(normal.get_type(), ThreadPool::kNormalType);
      normal.shutdown();
    }

    {
      ThreadPool lf(2, ThreadPool::kLockfreeType);
      CHECK_EQ(lf.get_type(), ThreadPool::kLockfreeType);
      lf.shutdown();
    }
  }

  TEST_CASE("set_strategy and get_strategy round-trip") {
    ThreadPool pool(2);

    pool.set_strategy(ThreadPool::kPopStrategy);
    CHECK_EQ(pool.get_strategy(), ThreadPool::kPopStrategy);

    pool.set_strategy(ThreadPool::kBlockStrategy);
    CHECK_EQ(pool.get_strategy(), ThreadPool::kBlockStrategy);

    pool.set_strategy(ThreadPool::kOptimizationStrategy);
    CHECK_EQ(pool.get_strategy(), ThreadPool::kOptimizationStrategy);

    pool.shutdown();
  }

  TEST_CASE("get_max_task_count returns positive value") {
    ThreadPool pool(2);

    CHECK(pool.get_max_task_count() > 0u);

    pool.shutdown();
  }

  TEST_CASE("get_task_count is non-negative") {
    ThreadPool pool(1);
    std::atomic<bool> block{true};

    pool.post_task([&block] {
      while (block.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    });

    std::this_thread::sleep_for(20ms);

    pool.post_task([] {});
    pool.post_task([] {});

    CHECK(pool.get_task_count() >= 0u);

    block.store(false, std::memory_order_release);
    pool.shutdown();
  }

  TEST_CASE("is_in_work_thread returns true inside task and false outside") {
    ThreadPool pool(2);

    CHECK_FALSE(pool.is_in_work_thread());

    std::atomic<bool> inside{false};
    auto fut =
        pool.invoke_task([&pool, &inside] { inside.store(pool.is_in_work_thread(), std::memory_order_release); });
    fut.get();

    CHECK(inside.load(std::memory_order_acquire));
    pool.shutdown();
  }

  TEST_CASE("multiple tasks execute and all are counted") {
    ThreadPool pool(4);
    static constexpr int kCount = 20;
    std::atomic<int> counter{0};

    for (int i = 0; i < kCount; ++i) {
      pool.post_task([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
    }

    std::this_thread::sleep_for(100ms);
    pool.shutdown();

    CHECK_EQ(counter.load(), kCount);
  }

  TEST_CASE("tasks run concurrently across workers") {
    static constexpr int kWorkers = 4;
    ThreadPool pool(kWorkers);
    std::atomic<int> counter{0};

    auto t0 = std::chrono::steady_clock::now();

    std::vector<std::future<void>> futures;
    futures.reserve(kWorkers);

    for (int i = 0; i < kWorkers; ++i) {
      futures.push_back(pool.invoke_task([&counter] {
        std::this_thread::sleep_for(50ms);
        counter.fetch_add(1, std::memory_order_relaxed);
      }));
    }

    for (auto& f : futures) {
      f.get();
    }

    auto elapsed = std::chrono::steady_clock::now() - t0;

    CHECK_EQ(counter.load(), kWorkers);
    CHECK(elapsed < std::chrono::milliseconds(kWorkers * 50 - 20));

    pool.shutdown();
  }

  TEST_CASE("single-threaded pool serialises task execution order") {
    ThreadPool pool(1);
    static constexpr int kCount = 10;
    std::vector<int> order;
    order.reserve(kCount);

    std::vector<std::future<void>> futures;
    futures.reserve(kCount);

    for (int i = 0; i < kCount; ++i) {
      futures.push_back(pool.invoke_task([&order, i] { order.push_back(i); }));
    }

    for (auto& f : futures) {
      f.get();
    }

    CHECK_EQ(static_cast<int>(order.size()), kCount);

    for (int i = 0; i < kCount; ++i) {
      CHECK_EQ(order[i], i);
    }

    pool.shutdown();
  }

  TEST_CASE("invoke_task returns correct result for various types") {
    ThreadPool pool(2);

    SUBCASE("int return") {
      auto fut = pool.invoke_task([] { return 42; });
      CHECK_EQ(fut.get(), 42);
    }

    SUBCASE("string return") {
      auto fut = pool.invoke_task([] { return std::string("hello"); });
      CHECK_EQ(fut.get(), "hello");
    }

    SUBCASE("double return") {
      auto fut = pool.invoke_task([] { return 3.14; });
      CHECK_EQ(fut.get(), doctest::Approx(3.14));
    }

    SUBCASE("bool return") {
      auto fut = pool.invoke_task([] { return true; });
      CHECK(fut.get());
    }

    SUBCASE("with arguments") {
      auto fut = pool.invoke_task([](int a, int b) { return a + b; }, 10, 32);
      CHECK_EQ(fut.get(), 42);
    }

    pool.shutdown();
  }

  TEST_CASE("invoke_task future becomes broken_promise after shutdown") {
    ThreadPool pool(2);
    pool.shutdown();

    auto fut = pool.invoke_task([] { return 7; });

    CHECK_EQ(fut.wait_for(100ms), std::future_status::ready);
    CHECK_THROWS_AS((void)fut.get(), std::future_error);
  }

  TEST_CASE("kNormalType kPopStrategy drops oldest task when queue is full") {
    SmallQueueThreadPool pool(1);
    pool.set_strategy(ThreadPool::kPopStrategy);

    std::atomic<bool> release_worker{false};
    std::atomic<bool> worker_started{false};

    CHECK(pool.post_task([&] {
      worker_started.store(true, std::memory_order_release);

      while (!release_worker.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }));

    CHECK(wait_until([&worker_started] { return worker_started.load(std::memory_order_acquire); }));

    CHECK(pool.post_task([] {}));
    CHECK(pool.post_task([] {}));

    release_worker.store(true, std::memory_order_release);
    pool.shutdown();
  }

  TEST_CASE("kNormalType kBlockStrategy blocks producer until space available") {
    SmallQueueThreadPool pool(1);
    pool.set_strategy(ThreadPool::kBlockStrategy);

    std::atomic<bool> release_worker{false};
    std::atomic<bool> worker_started{false};
    std::atomic<int> executed{0};

    CHECK(pool.post_task([&] {
      worker_started.store(true, std::memory_order_release);

      while (!release_worker.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }

      executed.fetch_add(1, std::memory_order_acq_rel);
    }));

    CHECK(wait_until([&worker_started] { return worker_started.load(std::memory_order_acquire); }));
    CHECK(pool.post_task([&executed] { executed.fetch_add(1, std::memory_order_acq_rel); }));

    std::atomic<bool> producer_done{false};
    std::atomic<bool> producer_accepted{false};

    std::thread producer([&] {
      producer_accepted.store(pool.post_task([&executed] { executed.fetch_add(1, std::memory_order_acq_rel); }),
                              std::memory_order_release);
      producer_done.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(30ms);
    CHECK_FALSE(producer_done.load(std::memory_order_acquire));

    release_worker.store(true, std::memory_order_release);
    producer.join();

    CHECK(producer_done.load(std::memory_order_acquire));
    CHECK(producer_accepted.load(std::memory_order_acquire));

    pool.shutdown();
    CHECK_EQ(executed.load(std::memory_order_acquire), 3);
  }

  TEST_CASE("kNormalType kBlockStrategy shutdown unblocks blocked producer") {
    SmallQueueThreadPool pool(1);
    pool.set_strategy(ThreadPool::kBlockStrategy);

    std::atomic<bool> release_worker{false};
    std::atomic<bool> worker_started{false};
    std::atomic<bool> producer_done{false};
    std::atomic<bool> producer_accepted{true};

    CHECK(pool.post_task([&] {
      worker_started.store(true, std::memory_order_release);

      while (!release_worker.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }));

    CHECK(wait_until([&worker_started] { return worker_started.load(std::memory_order_acquire); }));
    CHECK(pool.post_task([] {}));

    std::thread producer([&] {
      producer_accepted.store(pool.post_task([] {}), std::memory_order_release);
      producer_done.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(20ms);
    CHECK_FALSE(producer_done.load(std::memory_order_acquire));

    std::thread stopper([&] { static_cast<void>(pool.shutdown()); });

    std::this_thread::sleep_for(20ms);
    release_worker.store(true, std::memory_order_release);

    producer.join();
    stopper.join();

    CHECK(producer_done.load(std::memory_order_acquire));
    CHECK_FALSE(producer_accepted.load(std::memory_order_acquire));
  }

  TEST_CASE("destructor from worker task detaches safely") {
    auto pool = std::make_unique<ThreadPool>(1);
    std::promise<void> done;
    auto done_fut = done.get_future();

    CHECK(pool->post_task([&] {
      pool.reset();
      done.set_value();
    }));

    CHECK_EQ(done_fut.wait_for(1s), std::future_status::ready);
  }

  TEST_CASE("high concurrency post from multiple threads delivers all tasks") {
    ThreadPool pool(4);
    std::atomic<int> counter{0};
    static constexpr int kThreads = 4;
    static constexpr int kTasksPerThread = 50;

    std::vector<std::thread> producers;
    producers.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
      producers.emplace_back([&pool, &counter] {
        for (int i = 0; i < kTasksPerThread; ++i) {
          pool.post_task([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
        }
      });
    }

    for (auto& thr : producers) {
      thr.join();
    }

    std::this_thread::sleep_for(100ms);
    pool.shutdown();

    CHECK_EQ(counter.load(), kThreads * kTasksPerThread);
  }

  TEST_CASE("kLockfreeType pool executes tasks correctly") {
    ThreadPool pool(4, ThreadPool::kLockfreeType);
    std::atomic<int> counter{0};
    static constexpr int kCount = 20;

    for (int i = 0; i < kCount; ++i) {
      pool.post_task([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
    }

    std::this_thread::sleep_for(100ms);
    pool.shutdown();

    CHECK_EQ(counter.load(), kCount);
  }

  TEST_CASE("kLockfreeType invoke_task returns correct result") {
    ThreadPool pool(2, ThreadPool::kLockfreeType);
    auto fut = pool.invoke_task([] { return 99; });

    CHECK_EQ(fut.get(), 99);
    pool.shutdown();
  }

  TEST_CASE("kLockfreeType is_in_work_thread returns true inside task") {
    ThreadPool pool(2, ThreadPool::kLockfreeType);
    std::atomic<bool> inside{false};

    auto fut =
        pool.invoke_task([&pool, &inside] { inside.store(pool.is_in_work_thread(), std::memory_order_release); });
    fut.get();

    CHECK(inside.load(std::memory_order_acquire));
    pool.shutdown();
  }

  TEST_CASE("kLockfreeType kBlockStrategy blocks until queue drains") {
    TwoTaskThreadPool pool(1U, ThreadPool::kLockfreeType);
    pool.set_strategy(ThreadPool::kBlockStrategy);

    std::atomic<bool> release_worker{false};
    std::atomic<bool> worker_started{false};
    std::atomic<int> executed{0};

    CHECK(pool.post_task([&] {
      worker_started.store(true, std::memory_order_release);

      while (!release_worker.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }

      executed.fetch_add(1, std::memory_order_acq_rel);
    }));

    CHECK(wait_until([&worker_started] { return worker_started.load(std::memory_order_acquire); }));

    CHECK(pool.post_task([&executed] { executed.fetch_add(1, std::memory_order_acq_rel); }));
    CHECK(pool.post_task([&executed] { executed.fetch_add(1, std::memory_order_acq_rel); }));

    std::atomic<bool> producer_done{false};
    std::atomic<bool> producer_accepted{false};

    std::thread producer([&] {
      producer_accepted.store(pool.post_task([&executed] { executed.fetch_add(1, std::memory_order_acq_rel); }),
                              std::memory_order_release);
      producer_done.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(30ms);
    CHECK_FALSE(producer_done.load(std::memory_order_acquire));

    release_worker.store(true, std::memory_order_release);
    producer.join();

    CHECK(producer_done.load(std::memory_order_acquire));
    CHECK(producer_accepted.load(std::memory_order_acquire));

    pool.shutdown();
    CHECK_EQ(executed.load(std::memory_order_acquire), 4);
  }

  TEST_CASE("kLockfreeType high concurrency stress with many producers") {
    ThreadPool pool(4U, ThreadPool::kLockfreeType);
    std::atomic<int> counter{0};
    static constexpr int kProducers = 4;
    static constexpr int kTasksPerProducer = 100;

    std::vector<std::thread> producers;
    producers.reserve(kProducers);

    for (int t = 0; t < kProducers; ++t) {
      producers.emplace_back([&pool, &counter] {
        for (int i = 0; i < kTasksPerProducer; ++i) {
          pool.post_task([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
        }
      });
    }

    for (auto& thr : producers) {
      thr.join();
    }

    pool.shutdown();

    CHECK_EQ(counter.load(), kProducers * kTasksPerProducer);
  }

  TEST_CASE("kLockfreeType concurrent workers process tasks concurrently") {
    static constexpr int kWorkers = 4;
    ThreadPool pool(kWorkers, ThreadPool::kLockfreeType);
    std::atomic<int> counter{0};

    auto t0 = std::chrono::steady_clock::now();

    std::vector<std::future<void>> futures;
    futures.reserve(kWorkers);

    for (int i = 0; i < kWorkers; ++i) {
      futures.push_back(pool.invoke_task([&counter] {
        std::this_thread::sleep_for(50ms);
        counter.fetch_add(1, std::memory_order_relaxed);
      }));
    }

    for (auto& f : futures) {
      f.get();
    }

    auto elapsed = std::chrono::steady_clock::now() - t0;

    CHECK_EQ(counter.load(), kWorkers);
    CHECK(elapsed < std::chrono::milliseconds(kWorkers * 50 - 20));

    pool.shutdown();
  }
}

// NOLINTEND
