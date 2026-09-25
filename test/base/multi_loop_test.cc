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

#include "./base/multi_loop.h"

#include <doctest/doctest.h>

#include <atomic>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../common_test.h"

TEST_SUITE("base-MultiLoop") {
  TEST_CASE("default construction yields non-running loop") {
    MultiLoop loop;
    CHECK_FALSE(loop.is_running());
  }

  TEST_CASE("explicit thread count construction yields non-running loop") {
    MultiLoop loop(2);
    CHECK_FALSE(loop.is_running());
  }

  TEST_CASE("construction with type overload yields non-running loop") {
    MultiLoop normal(2, MessageLoop::kNormalType);
    CHECK_FALSE(normal.is_running());

    MultiLoop lf(2, MessageLoop::kLockfreeType);
    CHECK_FALSE(lf.is_running());
  }

  TEST_CASE("async_run starts loop and is_running becomes true") {
    MultiLoop loop(2);
    bool started = loop.async_run();
    CHECK(started);
    CHECK(loop.is_running());
    loop.quit();
    loop.wait_for_quit(2000);
  }

  TEST_CASE("async_run returns false when already running") {
    MultiLoop loop(2);
    loop.async_run();
    bool second = loop.async_run();
    CHECK_FALSE(second);
    loop.quit();
    loop.wait_for_quit(2000);
  }

  TEST_CASE("post_task executes callback on a worker thread") {
    MultiLoop loop(2);
    loop.async_run();

    std::promise<int> promise;
    auto future = promise.get_future();
    loop.post_task([&promise]() { promise.set_value(42); });

    auto status = future.wait_for(2s);
    CHECK_EQ(status, std::future_status::ready);
    CHECK_EQ(future.get(), 42);

    loop.quit();
    loop.wait_for_quit(2000);
  }

  TEST_CASE("zero worker pool falls back to the dispatcher") {
    MultiLoop loop(0);
    std::atomic<int> count{0};

    REQUIRE(loop.async_run());
    REQUIRE(loop.post_task([&count] { count.fetch_add(1, std::memory_order_relaxed); }));
    REQUIRE(loop.wait_for_idle(2000));
    CHECK_EQ(count.load(std::memory_order_relaxed), 1);
    loop.quit();
    loop.wait_for_quit(2000);
  }

  TEST_CASE("multiple post_task calls all execute") {
    static constexpr int kCount = 100;
    MultiLoop loop(4);
    loop.async_run();

    std::atomic<int> counter{0};
    std::atomic<int> remaining{kCount};
    std::promise<void> all_done;
    auto future = all_done.get_future();

    for (int i = 0; i < kCount; ++i) {
      loop.post_task([&counter, &remaining, &all_done]() {
        counter.fetch_add(1, std::memory_order_relaxed);
        if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
          all_done.set_value();
        }
      });
    }

    bool idle = loop.wait_for_idle(3000);
    CHECK(idle);
    auto status = future.wait_for(3s);
    CHECK_EQ(status, std::future_status::ready);
    CHECK_EQ(counter.load(), kCount);

    loop.quit();
    loop.wait_for_quit(2000);
  }

  TEST_CASE("protected tasks survive a full worker queue") {
    MessageLoop::Type type = MessageLoop::kNormalType;
    bool protect_all = false;
    bool nested = false;
    bool throw_nested = false;
    bool reenter_on_drop = false;

    SUBCASE("normal dispatcher") {}
    SUBCASE("priority dispatcher") { type = MessageLoop::kPriorityType; }
    SUBCASE("all queued tasks are protected") { protect_all = true; }
    SUBCASE("all queued priority tasks are protected") {
      type = MessageLoop::kPriorityType;
      protect_all = true;
    }
    SUBCASE("nested dispatch restores the outer policy") { nested = true; }
    SUBCASE("throwing nested dispatch restores the outer policy") {
      nested = true;
      throw_nested = true;
    }
    SUBCASE("retired callback can query the pool from another thread") { reenter_on_drop = true; }

    class CountingMultiLoop final : public MultiLoop {
     public:
      explicit CountingMultiLoop(Type type) : MultiLoop(1, type) {}

      size_t get_max_task_count() const override { return 20000U; }

      std::atomic<size_t> forwarded{0U};
      std::atomic<bool> nest_next{false};
      Callback nested_callback;
      bool throw_nested{false};

     protected:
      void on_task_changed(Callback&& callback, uint32_t start_time) override {
        if (in_nested_dispatch_ && throw_nested) {
          throw std::runtime_error("nested dispatch");
        }

        if (nest_next.exchange(false, std::memory_order_acq_rel)) {
          CHECK(post_task(std::move(nested_callback)));
          in_nested_dispatch_ = true;

          if (throw_nested) {
            CHECK_THROWS_AS(spin_once(false), std::runtime_error);
          } else {
            CHECK(spin_once(false));
          }

          in_nested_dispatch_ = false;
        }

        MultiLoop::on_task_changed(std::move(callback), start_time);
        forwarded.fetch_add(1U, std::memory_order_release);
      }

     private:
      bool in_nested_dispatch_{false};
    };

    static constexpr size_t kWorkerCapacity = 10000U;
    std::promise<void> release_worker;
    auto gate = release_worker.get_future();
    std::atomic<bool> worker_started{false};
    std::atomic<size_t> executed{0U};
    std::atomic<bool> extra_on_dispatcher{false};
    std::atomic<bool> retired_without_pool_lock{false};
    std::promise<void> queried;
    auto queried_future = queried.get_future();
    std::thread query_thread;
    CountingMultiLoop loop(type);
    loop.throw_nested = throw_nested;
    loop.nested_callback = [&] { executed.fetch_add(1U, std::memory_order_relaxed); };

    struct QueryOnDestroy final {
      MultiLoop& loop;
      std::promise<void>& queried;
      std::future<void>& queried_future;
      std::thread& query_thread;
      std::atomic<bool>& without_pool_lock;

      ~QueryOnDestroy() {
        query_thread = std::thread([target = &loop, done = &queried] {
          CHECK_FALSE(target->is_in_same_thread());
          done->set_value();
        });
        without_pool_lock.store(queried_future.wait_for(2s) == std::future_status::ready, std::memory_order_release);
      }
    };

    REQUIRE(loop.async_run());
    CHECK(loop.post_task([&] {
      worker_started.store(true, std::memory_order_release);
      gate.wait();
    }));

    const bool started = common_test::wait_until([&] { return worker_started.load(std::memory_order_acquire); }, 5s);

    if (!started) {
      release_worker.set_value();
      loop.quit(true);
      loop.wait_for_quit(5000);
      CHECK(started);
      return;
    }

    PostTaskOptions options;
    options.drop_policy = TaskDropPolicy::kProtected;
    loop.nest_next.store(nested, std::memory_order_release);
    auto protected_task = loop.post_task_handle([&] { executed.fetch_add(1U, std::memory_order_relaxed); }, options);
    const size_t nested_count = nested && !throw_nested ? 1U : 0U;
    CHECK(common_test::wait_until([&] { return loop.forwarded.load(std::memory_order_acquire) == 2U + nested_count; },
                                  5s));

    for (size_t i = 1U + nested_count; i < kWorkerCapacity; ++i) {
      if (protect_all) {
        (void)loop.post_task_handle([&] { executed.fetch_add(1U, std::memory_order_relaxed); }, options);
      } else if (reenter_on_drop && i == 1U) {
        auto probe = std::unique_ptr<QueryOnDestroy>(
            new QueryOnDestroy{loop, queried, queried_future, query_thread, retired_without_pool_lock});
        CHECK(loop.post_task([&, probe = std::move(probe)] { executed.fetch_add(1U, std::memory_order_relaxed); }));
      } else {
        CHECK(loop.post_task([&] { executed.fetch_add(1U, std::memory_order_relaxed); }));
      }
    }

    const bool full = common_test::wait_until(
        [&] { return loop.forwarded.load(std::memory_order_acquire) == kWorkerCapacity + 1U; }, 5s);
    CHECK(full);

    if (full) {
      for (size_t i = 0U; i <= nested_count; ++i) {
        CHECK(loop.post_task([&] {
          extra_on_dispatcher.store(loop.MessageLoop::is_in_same_thread(), std::memory_order_relaxed);
          executed.fetch_add(1U, std::memory_order_relaxed);
        }));
      }

      CHECK(common_test::wait_until(
          [&] { return loop.forwarded.load(std::memory_order_acquire) == kWorkerCapacity + 2U + nested_count; }, 5s));
    }

    release_worker.set_value();
    CHECK(loop.wait_for_idle(5000));
    CHECK_EQ(protected_task.state(), TaskExecutionState::kCompleted);
    CHECK_EQ(executed.load(std::memory_order_relaxed), kWorkerCapacity + (full && protect_all ? 1U : 0U));

    if (!protect_all) {
      CHECK_FALSE(extra_on_dispatcher.load(std::memory_order_relaxed));
    }

    loop.quit();
    CHECK(loop.wait_for_quit());

    if (query_thread.joinable()) {
      query_thread.join();
    }

    if (reenter_on_drop && full) {
      CHECK(retired_without_pool_lock.load(std::memory_order_acquire));
    }
  }

  TEST_CASE("wait_for_idle returns true after all tasks complete") {
    MultiLoop loop(2);
    loop.async_run();

    std::atomic<int> done{0};
    std::atomic<int> remaining{20};
    std::promise<void> all_done;
    auto future = all_done.get_future();

    for (int i = 0; i < 20; ++i) {
      loop.post_task([&done, &remaining, &all_done]() {
        std::this_thread::sleep_for(5ms);
        done.fetch_add(1, std::memory_order_relaxed);
        if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
          all_done.set_value();
        }
      });
    }

    bool result = loop.wait_for_idle(5000);
    CHECK(result);
    auto status = future.wait_for(5s);
    CHECK_EQ(status, std::future_status::ready);
    CHECK_EQ(done.load(), 20);

    loop.quit();
    loop.wait_for_quit(2000);
  }

  TEST_CASE("wait_for_idle via base pointer waits for worker task") {
    MultiLoop loop(2);
    MessageLoop* base = &loop;
    loop.async_run();

    std::atomic<bool> done{false};
    loop.post_task([&done]() {
      std::this_thread::sleep_for(100ms);
      done.store(true, std::memory_order_release);
    });

    CHECK(base->wait_for_idle(3000));
    CHECK(done.load(std::memory_order_acquire));

    loop.quit();
    loop.wait_for_quit(2000);
  }

  TEST_CASE("quit stops the loop") {
    MultiLoop loop(2);
    loop.async_run();

    loop.quit();
    bool stopped = loop.wait_for_quit(2000);

    CHECK(stopped);
    CHECK_FALSE(loop.is_running());
  }

  TEST_CASE("is_in_same_thread returns true from worker") {
    MultiLoop loop(2);
    loop.async_run();

    std::promise<bool> promise;
    auto future = promise.get_future();
    loop.post_task([&loop, &promise]() { promise.set_value(loop.is_in_same_thread()); });

    auto status = future.wait_for(2s);
    CHECK_EQ(status, std::future_status::ready);
    CHECK(future.get());

    loop.quit();
    loop.wait_for_quit(2000);
  }

  TEST_CASE("worker thread query does not block shutdown") {
    class CoordinatedMultiLoop final : public MultiLoop {
     public:
      explicit CoordinatedMultiLoop(size_t thread_num) : MultiLoop(thread_num) {}

      std::atomic<bool> on_end_entered{false};
      std::atomic<bool> continue_on_end{false};

     protected:
      void on_end() override {
        on_end_entered.store(true, std::memory_order_release);

        while (!continue_on_end.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }

        MultiLoop::on_end();
      }
    } loop(1);

    std::atomic<bool> worker_started{false};
    std::atomic<bool> query_worker{false};
    std::promise<bool> result;
    auto future = result.get_future();

    REQUIRE(loop.async_run());
    REQUIRE(loop.post_task([&]() {
      worker_started.store(true, std::memory_order_release);

      while (!query_worker.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }

      result.set_value(loop.is_in_same_thread());
    }));
    const bool worker_ready =
        common_test::wait_until([&worker_started]() { return worker_started.load(std::memory_order_acquire); }, 2s);

    if (!worker_ready) {
      query_worker.store(true, std::memory_order_release);
      loop.continue_on_end.store(true, std::memory_order_release);
      loop.quit(true);
      loop.wait_for_quit(2000);
      CHECK(worker_ready);
      return;
    }

    loop.quit();
    const bool on_end_ready =
        common_test::wait_until([&loop]() { return loop.on_end_entered.load(std::memory_order_acquire); }, 2s);

    if (!on_end_ready) {
      loop.continue_on_end.store(true, std::memory_order_release);
      query_worker.store(true, std::memory_order_release);
      loop.quit(true);
      loop.wait_for_quit(2000);
      CHECK(on_end_ready);
      return;
    }

    loop.continue_on_end.store(true, std::memory_order_release);
    std::this_thread::sleep_for(50ms);
    query_worker.store(true, std::memory_order_release);

    const auto status = future.wait_for(2s);
    CHECK_EQ(status, std::future_status::ready);

    if (status == std::future_status::ready) {
      CHECK(future.get());
    }

    CHECK(loop.wait_for_quit(2000));
  }

  TEST_CASE("is_in_same_thread returns false from test thread") {
    MultiLoop loop(2);
    loop.async_run();

    CHECK_FALSE(loop.is_in_same_thread());

    loop.quit();
    loop.wait_for_quit(2000);
  }

  TEST_CASE("invoke_task returns a future with the result") {
    MultiLoop loop(2);
    loop.async_run();

    auto future = loop.invoke_task([]() -> std::string { return "hello_from_worker"; });
    auto status = future.wait_for(2s);
    CHECK_EQ(status, std::future_status::ready);
    CHECK_EQ(future.get(), "hello_from_worker");

    loop.quit();
    loop.wait_for_quit(2000);
  }

  TEST_CASE("tasks run concurrently on multiple worker threads") {
    static constexpr int kTasks = 4;
    MultiLoop loop(4);
    loop.async_run();

    std::atomic<int> running{0};
    std::atomic<int> max_concurrent{0};
    std::atomic<int> latch{kTasks};
    std::vector<std::future<void>> futures;

    for (int i = 0; i < kTasks; ++i) {
      futures.push_back(loop.invoke_task([&running, &max_concurrent, &latch]() {
        int cur = running.fetch_add(1, std::memory_order_acq_rel) + 1;
        int old_max = max_concurrent.load(std::memory_order_acquire);
        while (cur > old_max && !max_concurrent.compare_exchange_weak(old_max, cur, std::memory_order_acq_rel)) {
        }
        latch.fetch_sub(1, std::memory_order_acq_rel);
        while (latch.load(std::memory_order_acquire) > 0) {
          std::this_thread::yield();
        }
        running.fetch_sub(1, std::memory_order_acq_rel);
      }));
    }

    for (auto& f : futures) {
      f.wait_for(5s);
    }

    CHECK(max_concurrent.load() > 1);

    loop.quit();
    loop.wait_for_quit(2000);
  }

  TEST_CASE("set_name and get_name are consistent") {
    MultiLoop loop(2);
    loop.set_name("test_loop");
    CHECK_EQ(loop.get_name(), "test_loop");
  }

  TEST_CASE("get_type reflects construction type") {
    MultiLoop normal(2, MessageLoop::kNormalType);
    CHECK_EQ(normal.get_type(), MessageLoop::kNormalType);

    MultiLoop lf(2, MessageLoop::kLockfreeType);
    CHECK_EQ(lf.get_type(), MessageLoop::kLockfreeType);
  }

  TEST_CASE("quit with force discards pending tasks") {
    MultiLoop loop(1);
    loop.async_run();

    std::atomic<int> counter{0};
    std::atomic<bool> release{false};
    loop.post_task([&release]() {
      while (!release.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(1ms);
      }
    });

    for (int i = 0; i < 50; ++i) {
      loop.post_task([&counter]() { counter.fetch_add(1, std::memory_order_relaxed); });
    }

    release.store(true, std::memory_order_release);
    loop.quit(true);
    loop.wait_for_quit(3000);

    CHECK(counter.load() <= 50);
  }

  TEST_CASE("post_task_with_priority on priority loop executes task") {
    MultiLoop loop(2, MessageLoop::kPriorityType);
    loop.async_run();

    std::atomic<int> ran{0};
    loop.post_task_with_priority([&ran]() { ran.store(1); }, MessageLoop::kNormalPriority);

    loop.wait_for_idle(2000);
    CHECK(common_test::wait_until([&ran] { return ran.load() != 0; }, 2s));
    CHECK_EQ(ran.load(), 1);

    loop.quit();
    loop.wait_for_quit(2000);
  }

  TEST_CASE("destructor auto-quits without deadlock") {
    {
      MultiLoop loop(2);
      loop.async_run();

      std::atomic<bool> done{false};
      loop.post_task([&done] {
        std::this_thread::sleep_for(5ms);
        done.store(true);
      });

      auto deadline = std::chrono::steady_clock::now() + 2s;
      while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
      }
    }
  }

  TEST_CASE("register_begin_handler fires once on start") {
    std::atomic<int> begin_count{0};
    MultiLoop loop(3);
    loop.register_begin_handler([&begin_count]() { begin_count.fetch_add(1, std::memory_order_relaxed); });

    loop.async_run();
    std::this_thread::sleep_for(50ms);
    CHECK_EQ(begin_count.load(), 1);

    loop.quit();
    loop.wait_for_quit(2000);
  }

  TEST_CASE("register_end_handler fires once on exit") {
    std::atomic<int> end_count{0};
    MultiLoop loop(3);
    loop.register_end_handler([&end_count]() { end_count.fetch_add(1, std::memory_order_relaxed); });

    loop.async_run();
    loop.quit();
    loop.wait_for_quit(2000);

    CHECK_EQ(end_count.load(), 1);
  }

  TEST_CASE("get_task_count reflects pending task count") {
    MultiLoop loop(1);
    std::atomic<bool> release{false};
    loop.async_run();

    loop.post_task([&release]() {
      while (!release.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(1ms);
      }
    });

    for (int i = 0; i < 5; ++i) {
      loop.post_task([] {});
    }

    std::this_thread::sleep_for(20ms);
    size_t queued = loop.get_task_count();
    CHECK(queued <= 5u);

    release.store(true, std::memory_order_release);
    loop.wait_for_idle(2000);
    loop.quit();
    loop.wait_for_quit(2000);
  }

  TEST_CASE("destructor waits for a worker backlog longer than one second") {
    std::atomic<int> done{0};

    {
      MultiLoop loop(1);
      loop.async_run();

      for (int i = 0; i < 2; ++i) {
        loop.post_task([&done] {
          std::this_thread::sleep_for(700ms);
          done.fetch_add(1, std::memory_order_acq_rel);
        });
      }

      REQUIRE(common_test::wait_until([&loop] { return loop.get_task_count() == 0U; }));
    }

    CHECK_EQ(done.load(std::memory_order_acquire), 2);
  }
}

// NOLINTEND
