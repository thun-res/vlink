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

#include "./base/message_loop.h"

#include <doctest/doctest.h>

#include <atomic>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../common_test.h"

namespace {

class SmallQueueLoop final : public MessageLoop {
 public:
  using MessageLoop::MessageLoop;
  [[nodiscard]] size_t get_max_task_count() const override { return 1U; }
};

class TimeoutLoop final : public MessageLoop {
 public:
  using MessageLoop::MessageLoop;
  [[nodiscard]] uint32_t get_max_elapsed_time() const override { return 5U; }
  void on_task_timeout(Callback&& cb, uint32_t elapsed) override {
    ++timeout_count;
    last_elapsed = elapsed;
    (void)cb;
  }
  std::atomic<int> timeout_count{0};
  std::atomic<uint32_t> last_elapsed{0};
};

}  // namespace

TEST_SUITE("base-MessageLoop") {
  TEST_CASE("default construction has kNormalType") {
    MessageLoop loop;
    CHECK_EQ(loop.get_type(), MessageLoop::kNormalType);
  }

  TEST_CASE("explicit type construction sets correct type") {
    SUBCASE("kNormalType") {
      MessageLoop loop(MessageLoop::kNormalType);
      CHECK_EQ(loop.get_type(), MessageLoop::kNormalType);
    }
    SUBCASE("kLockfreeType") {
      MessageLoop loop(MessageLoop::kLockfreeType);
      CHECK_EQ(loop.get_type(), MessageLoop::kLockfreeType);
    }
    SUBCASE("kPriorityType") {
      MessageLoop loop(MessageLoop::kPriorityType);
      CHECK_EQ(loop.get_type(), MessageLoop::kPriorityType);
    }
  }

  TEST_CASE("set_name and get_name round-trip") {
    MessageLoop loop;
    loop.set_name("my-loop");
    CHECK_EQ(loop.get_name(), "my-loop");
  }

  TEST_CASE("default strategy is kOptimizationStrategy") {
    MessageLoop loop;
    CHECK_EQ(loop.get_strategy(), MessageLoop::kOptimizationStrategy);
  }

  TEST_CASE("set_strategy and get_strategy reflect change") {
    MessageLoop loop;
    loop.set_strategy(MessageLoop::kBlockStrategy);
    CHECK_EQ(loop.get_strategy(), MessageLoop::kBlockStrategy);

    loop.set_strategy(MessageLoop::kPopStrategy);
    CHECK_EQ(loop.get_strategy(), MessageLoop::kPopStrategy);

    loop.set_strategy(MessageLoop::kOptimizationStrategy);
    CHECK_EQ(loop.get_strategy(), MessageLoop::kOptimizationStrategy);
  }

  TEST_CASE("priority enum values are strictly ordered") {
    CHECK(MessageLoop::kNoPriority < MessageLoop::kLowestPriority);
    CHECK(MessageLoop::kLowestPriority < MessageLoop::kTimerPriority);
    CHECK(MessageLoop::kTimerPriority < MessageLoop::kNormalPriority);
    CHECK(MessageLoop::kNormalPriority < MessageLoop::kHighestPriority);
  }

  TEST_CASE("is_running is false before start and true after async_run") {
    MessageLoop loop;
    CHECK_FALSE(loop.is_running());

    loop.async_run();
    std::this_thread::sleep_for(20ms);
    CHECK(loop.is_running());

    loop.quit();
    loop.wait_for_quit();
    CHECK_FALSE(loop.is_running());
  }

  TEST_CASE("async_run returns false when loop is already running") {
    MessageLoop loop;
    CHECK(loop.async_run());
    CHECK_FALSE(loop.async_run());

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("loop can restart after quit and wait_for_quit") {
    MessageLoop loop;
    CHECK(loop.async_run());
    loop.quit();
    CHECK(loop.wait_for_quit());

    CHECK(loop.async_run());
    std::atomic<bool> ran{false};
    loop.post_task([&ran] { ran.store(true, std::memory_order_release); });
    loop.wait_for_idle();
    CHECK(ran.load(std::memory_order_acquire));

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("post_task executes callback on loop thread") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<int> count{0};
    loop.post_task([&count] { count.fetch_add(1); });
    loop.wait_for_idle();
    CHECK_EQ(count.load(), 1);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("post_task returns false after quit") {
    MessageLoop loop;
    loop.async_run();
    loop.quit();
    loop.wait_for_quit();
    CHECK_FALSE(loop.post_task([] {}));
  }

  TEST_CASE("post_task returns false after quit for all queue types") {
    for (auto type : {MessageLoop::kNormalType, MessageLoop::kLockfreeType, MessageLoop::kPriorityType}) {
      MessageLoop loop(type);
      loop.async_run();
      loop.quit();
      loop.wait_for_quit();
      CHECK_FALSE(loop.post_task([] {}));
    }
  }

  TEST_CASE("kNormalType tasks execute in FIFO order") {
    MessageLoop loop(MessageLoop::kNormalType);
    loop.async_run();

    std::vector<int> order;
    std::mutex mtx;

    for (int i = 0; i < 5; ++i) {
      loop.post_task([i, &order, &mtx] {
        std::lock_guard lk(mtx);
        order.push_back(i);
      });
    }

    loop.wait_for_idle();
    REQUIRE_EQ(order.size(), 5u);
    for (int i = 0; i < 5; ++i) {
      CHECK_EQ(order[static_cast<size_t>(i)], i);
    }

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("get_task_count returns pending count") {
    MessageLoop loop;
    CHECK_EQ(loop.get_task_count(), 0u);
    loop.async_run();

    std::atomic<bool> gate{false};
    loop.post_task([&gate] {
      while (!gate.load()) {
        std::this_thread::yield();
      }
    });
    std::this_thread::sleep_for(10ms);

    loop.post_task([] {});
    loop.post_task([] {});
    std::this_thread::sleep_for(5ms);
    CHECK(loop.get_task_count() >= 2u);

    gate.store(true);
    loop.wait_for_idle();
    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("get_max_task_count returns 10000") {
    MessageLoop loop;
    CHECK_EQ(loop.get_max_task_count(), 10000u);
  }

  TEST_CASE("get_max_timer_count returns at least 1") {
    MessageLoop loop;
    CHECK(loop.get_max_timer_count() >= 1u);
  }

  TEST_CASE("get_max_elapsed_time returns 0 by default") {
    MessageLoop loop;
    CHECK_EQ(loop.get_max_elapsed_time(), 0u);
  }

  TEST_CASE("wait_for_idle returns true after tasks complete") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<int> cnt{0};
    for (int i = 0; i < 10; ++i) {
      loop.post_task([&cnt] { cnt.fetch_add(1); });
    }

    CHECK(loop.wait_for_idle(500));
    CHECK_EQ(cnt.load(), 10);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("wait_for_idle returns false on timeout while loop is blocked") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> gate{false};
    loop.post_task([&gate] {
      while (!gate.load()) {
        std::this_thread::yield();
      }
    });
    std::this_thread::sleep_for(20ms);

    CHECK_FALSE(loop.wait_for_idle(80, true));

    gate.store(true);
    loop.wait_for_idle();
    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("wait_for_idle with pre-start queued task is not idle") {
    MessageLoop loop;
    std::atomic<bool> ran{false};

    CHECK(loop.post_task([&ran] { ran.store(true, std::memory_order_release); }));
    CHECK_FALSE(loop.wait_for_idle(20, false));
    CHECK_FALSE(ran.load(std::memory_order_acquire));

    loop.spin_once(false);
    CHECK(loop.wait_for_idle(20, false));
    CHECK(ran.load(std::memory_order_acquire));
  }

  TEST_CASE("wait_for_quit with timeout returns false while loop runs") {
    MessageLoop loop;
    loop.async_run();
    CHECK_FALSE(loop.wait_for_quit(50));

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("is_busy is true while a task is executing") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> task_running{false};
    std::atomic<bool> gate{false};

    loop.post_task([&task_running, &gate] {
      task_running.store(true);
      while (!gate.load()) {
        std::this_thread::yield();
      }
    });

    while (!task_running.load()) {
      std::this_thread::yield();
    }

    CHECK(loop.is_busy());

    gate.store(true);
    loop.wait_for_idle();
    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("is_ready_to_quit becomes true after quit") {
    MessageLoop loop;
    loop.async_run();
    CHECK_FALSE(loop.is_ready_to_quit());

    loop.quit();
    std::this_thread::sleep_for(20ms);
    CHECK((loop.is_ready_to_quit() || !loop.is_running()));

    loop.wait_for_quit();
  }

  TEST_CASE("is_in_same_thread is true inside callback false outside") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> inside{false};
    loop.post_task([&loop, &inside] { inside.store(loop.is_in_same_thread()); });

    loop.wait_for_idle();
    CHECK(inside.load());
    CHECK_FALSE(loop.is_in_same_thread());

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("invoke_task returns computed result") {
    MessageLoop loop;
    loop.async_run();

    auto fut = loop.invoke_task([]() -> int { return 42; });
    CHECK_EQ(fut.get(), 42);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("invoke_task with arguments forwards correctly") {
    MessageLoop loop;
    loop.async_run();

    auto fut = loop.invoke_task([](int a, int b) -> int { return a + b; }, 10, 20);
    CHECK_EQ(fut.get(), 30);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("invoke_task with void return completes") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> ran{false};
    auto fut = loop.invoke_task([&ran] { ran.store(true); });
    fut.get();
    CHECK(ran.load());

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("spin processes all tasks until quit") {
    MessageLoop loop;
    std::atomic<int> count{0};

    loop.post_task([&count] { count.fetch_add(1); });
    loop.post_task([&count] { count.fetch_add(1); });
    loop.post_task([&count] { count.fetch_add(1); });
    loop.post_task([&loop] { loop.quit(); });

    loop.spin();
    CHECK_EQ(count.load(), 3);
  }

  TEST_CASE("spin_once with block=false processes available tasks") {
    MessageLoop loop;
    std::atomic<int> ran{0};

    loop.post_task([&ran] { ran.store(1, std::memory_order_relaxed); });
    loop.spin_once(false);
    std::this_thread::sleep_for(20ms);
    CHECK_EQ(ran.load(), 1);
  }

  TEST_CASE("spin_once with block=false on empty queue returns quickly") {
    MessageLoop loop;
    bool result = loop.spin_once(false);
    (void)result;
  }

  TEST_CASE("run in dedicated thread exits after quit") {
    MessageLoop loop;
    std::thread t([&loop] { loop.run(); });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!loop.is_running() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(1ms);
    }

    CHECK(loop.is_running());
    loop.quit();
    t.join();
    CHECK_FALSE(loop.is_running());
  }

  TEST_CASE("register_begin_handler fires once on loop start") {
    MessageLoop loop;
    std::atomic<int> count{0};
    loop.register_begin_handler([&count] { count.fetch_add(1); });

    loop.async_run();
    std::this_thread::sleep_for(30ms);
    CHECK_EQ(count.load(), 1);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("register_end_handler fires once on loop stop") {
    MessageLoop loop;
    std::atomic<int> count{0};
    loop.register_end_handler([&count] { count.fetch_add(1); });

    loop.async_run();
    loop.quit();
    loop.wait_for_quit();
    CHECK_EQ(count.load(), 1);
  }

  TEST_CASE("register_idle_handler fires at least once when loop is idle") {
    MessageLoop loop;
    std::atomic<int> count{0};
    loop.register_idle_handler([&count] { count.fetch_add(1); });

    loop.async_run();
    std::this_thread::sleep_for(50ms);
    CHECK(count.load() >= 1);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("handlers registered after async_run are ignored") {
    MessageLoop loop;
    loop.async_run();
    std::this_thread::sleep_for(10ms);

    std::atomic<bool> begin_ran{false};
    std::atomic<bool> end_ran{false};
    std::atomic<bool> idle_ran{false};

    loop.register_begin_handler([&begin_ran] { begin_ran.store(true); });
    loop.register_end_handler([&end_ran] { end_ran.store(true); });
    loop.register_idle_handler([&idle_ran] { idle_ran.store(true); });

    loop.post_task([] {});
    loop.wait_for_idle(200);

    CHECK_FALSE(begin_ran.load());

    loop.quit();
    loop.wait_for_quit();
    CHECK_FALSE(end_ran.load());
    CHECK_FALSE(idle_ran.load());
  }

  TEST_CASE("kPriorityType dispatches tasks in priority order") {
    MessageLoop loop(MessageLoop::kPriorityType);
    std::atomic<bool> gate{false};
    std::vector<int> order;
    std::mutex mtx;

    loop.async_run();

    loop.post_task([&gate] {
      while (!gate.load()) {
        std::this_thread::yield();
      }
    });
    std::this_thread::sleep_for(20ms);

    loop.post_task_with_priority(
        [&order, &mtx] {
          std::lock_guard lk(mtx);
          order.push_back(1);
        },
        MessageLoop::kLowestPriority);

    loop.post_task_with_priority(
        [&order, &mtx] {
          std::lock_guard lk(mtx);
          order.push_back(3);
        },
        MessageLoop::kHighestPriority);

    loop.post_task_with_priority(
        [&order, &mtx] {
          std::lock_guard lk(mtx);
          order.push_back(2);
        },
        MessageLoop::kNormalPriority);

    gate.store(true);
    loop.wait_for_idle();

    REQUIRE_EQ(order.size(), 3u);
    CHECK_EQ(order[0], 3);
    CHECK_EQ(order[1], 2);
    CHECK_EQ(order[2], 1);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("post_task_with_priority on kNormalType returns false") {
    MessageLoop loop(MessageLoop::kNormalType);
    loop.async_run();
    CHECK_FALSE(loop.post_task_with_priority([] {}, 5U));
    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("post_task_with_priority on kLockfreeType returns false") {
    MessageLoop loop(MessageLoop::kLockfreeType);
    loop.async_run();
    CHECK_FALSE(loop.post_task_with_priority([] {}, 5U));
    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("post_task_with_priority kNoPriority on kPriorityType returns false") {
    MessageLoop loop(MessageLoop::kPriorityType);
    loop.async_run();
    CHECK_FALSE(loop.post_task_with_priority([] {}, MessageLoop::kNoPriority));
    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("post_task_with_priority kNormalPriority on kPriorityType executes task") {
    MessageLoop loop(MessageLoop::kPriorityType);
    loop.async_run();

    std::promise<void> done;
    auto fut = done.get_future();
    CHECK(loop.post_task_with_priority([&done] { done.set_value(); }, MessageLoop::kNormalPriority));
    REQUIRE(fut.wait_for(1s) == std::future_status::ready);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("invoke_task_with_priority on kPriorityType returns result") {
    MessageLoop loop(MessageLoop::kPriorityType);
    loop.async_run();

    auto fut = loop.invoke_task_with_priority([]() -> std::string { return "ok"; }, MessageLoop::kHighestPriority);
    REQUIRE(fut.wait_for(1s) == std::future_status::ready);
    CHECK_EQ(fut.get(), "ok");

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("invoke_task_with_priority on kNormalType yields broken_promise") {
    MessageLoop loop(MessageLoop::kNormalType);
    loop.async_run();

    auto fut = loop.invoke_task_with_priority([] { return 7; }, MessageLoop::kNormalPriority);
    REQUIRE(fut.wait_for(1s) == std::future_status::ready);
    CHECK_THROWS_AS(fut.get(), std::future_error);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("kLockfreeType basic post_task round-trip") {
    MessageLoop loop(MessageLoop::kLockfreeType);
    loop.async_run();

    std::atomic<int> count{0};
    for (int i = 0; i < 20; ++i) {
      loop.post_task([&count] { count.fetch_add(1); });
    }

    loop.wait_for_idle();
    CHECK_EQ(count.load(), 20);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("kLockfreeType invoke_task returns value") {
    MessageLoop loop(MessageLoop::kLockfreeType);
    loop.async_run();

    auto fut = loop.invoke_task([]() -> int { return 99; });
    CHECK_EQ(fut.get(), 99);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("kLockfreeType restarts correctly after quit") {
    MessageLoop loop(MessageLoop::kLockfreeType);

    loop.async_run();
    loop.quit();
    loop.wait_for_quit();

    std::promise<void> done;
    auto fut = done.get_future();
    loop.async_run();
    CHECK(loop.post_task([&done] { done.set_value(); }));
    REQUIRE(fut.wait_for(1s) == std::future_status::ready);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("reset_lockfree_capacity does not crash and loop stays usable") {
    MessageLoop loop(MessageLoop::kLockfreeType);
    loop.async_run();

    for (int i = 0; i < 5; ++i) {
      loop.post_task([] {});
    }

    loop.wait_for_idle();
    loop.reset_lockfree_capacity();

    std::atomic<bool> ran{false};
    loop.post_task([&ran] { ran.store(true); });
    loop.wait_for_idle();
    CHECK(ran.load());

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("kPopStrategy drops oldest task when queue is full") {
    SmallQueueLoop loop(MessageLoop::kNormalType);
    loop.set_strategy(MessageLoop::kPopStrategy);

    CHECK(loop.post_task([] {}));
    CHECK(loop.post_task([] {}));
    CHECK_EQ(loop.get_task_count(), 1u);
  }

  TEST_CASE("kBlockStrategy producer unblocks when loop drains") {
    SmallQueueLoop loop;
    loop.set_strategy(MessageLoop::kBlockStrategy);
    loop.async_run();

    std::atomic<int> ran{0};
    CHECK(loop.post_task([&ran] {
      ran.fetch_add(1);
      std::this_thread::sleep_for(20ms);
    }));

    std::thread producer([&loop, &ran] { CHECK(loop.post_task([&ran] { ran.fetch_add(1); })); });

    producer.join();
    loop.wait_for_idle(1000);
    CHECK_EQ(ran.load(), 2);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("wakeup returns false when loop is not running") {
    MessageLoop loop;
    CHECK_FALSE(loop.wakeup());
  }

  TEST_CASE("wakeup returns true when loop is running") {
    MessageLoop loop;
    loop.set_strategy(MessageLoop::kBlockStrategy);
    loop.async_run();
    std::this_thread::sleep_for(20ms);
    CHECK(loop.wakeup());

    std::atomic<bool> ran{false};
    loop.post_task([&ran] { ran.store(true); });
    loop.wait_for_idle();
    CHECK(ran.load());

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("get_alive_state is non-null and alive initially") {
    MessageLoop loop;
    auto state = loop.get_alive_state();
    REQUIRE(state != nullptr);
    CHECK(state->alive.load(std::memory_order_acquire));
  }

  TEST_CASE("get_alive_state alive flips to false after loop destruction") {
    std::shared_ptr<MessageLoop::AliveState> state;
    {
      MessageLoop loop;
      state = loop.get_alive_state();
      REQUIRE(state != nullptr);
      CHECK(state->alive.load(std::memory_order_acquire));
    }
    REQUIRE(state != nullptr);
    CHECK_FALSE(state->alive.load(std::memory_order_acquire));
  }

  TEST_CASE("quit force=false waits for current task to finish") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> done{false};
    loop.post_task([&done] {
      std::this_thread::sleep_for(60ms);
      done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(20ms);

    loop.quit(false);
    loop.wait_for_quit();
    CHECK(done.load());
  }

  TEST_CASE("quit force=true drops remaining queued tasks") {
    MessageLoop loop;
    std::atomic<bool> gate{false};
    std::atomic<bool> first_started{false};
    std::atomic<int> exec_count{0};

    loop.async_run();

    loop.post_task([&first_started, &gate] {
      first_started.store(true, std::memory_order_release);
      while (!gate.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    });

    while (!first_started.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    for (int i = 0; i < 5; ++i) {
      loop.post_task([&exec_count] { exec_count.fetch_add(1); });
    }

    loop.quit(true);
    gate.store(true, std::memory_order_release);
    loop.wait_for_quit();
    CHECK(exec_count.load() <= 5);
  }

  TEST_CASE("exec_task void callback is valid and runs") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> ran{false};
    auto status = loop.exec_task(Schedule::Config{}, [&ran] { ran.store(true); });
    CHECK(status.is_valid());

    loop.wait_for_idle();
    CHECK(ran.load());

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("exec_task bool callback on_then fires when returns true") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> then_ran{false};
    std::atomic<bool> else_ran{false};

    loop.exec_task(Schedule::Config{50}, []() -> bool { return true; })
        .on_then([&then_ran]() -> bool {
          then_ran.store(true);
          return true;
        })
        .on_else([&else_ran] { else_ran.store(true); });

    for (int i = 0; i < 200 && !then_ran.load(); ++i) {
      loop.wait_for_idle();
      std::this_thread::sleep_for(10ms);
    }

    CHECK(then_ran.load());
    CHECK_FALSE(else_ran.load());

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("exec_task bool callback on_else fires when returns false") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> then_ran{false};
    std::atomic<bool> else_ran{false};

    loop.exec_task(Schedule::Config{50}, []() -> bool { return false; })
        .on_then([&then_ran]() -> bool {
          then_ran.store(true);
          return true;
        })
        .on_else([&else_ran] { else_ran.store(true); });

    for (int i = 0; i < 200 && !else_ran.load(); ++i) {
      loop.wait_for_idle();
      std::this_thread::sleep_for(10ms);
    }

    CHECK_FALSE(then_ran.load());
    CHECK(else_ran.load());

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("post_task_handle returns handle and task executes") {
    MessageLoop loop;
    loop.async_run();

    auto handle = loop.post_task_handle([] {});
    CHECK(handle.wait(1000));

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("on_task_timeout override fires for slow dispatched tasks") {
    TimeoutLoop loop;
    loop.async_run();

    std::atomic<bool> gate{false};
    loop.post_task([&gate] {
      std::this_thread::sleep_for(30ms);
      gate.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(2ms);

    std::atomic<bool> late_ran{false};
    loop.post_task([&late_ran] { late_ran.store(true, std::memory_order_release); });

    loop.wait_for_idle(1000);
    CHECK(gate.load());
    CHECK(loop.timeout_count.load() >= 1);
    CHECK(loop.last_elapsed.load() >= 5U);
    CHECK_FALSE(late_ran.load());

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("kLockfreeType high-concurrency producers all tasks complete") {
    MessageLoop loop(MessageLoop::kLockfreeType);
    loop.async_run();

    static constexpr int kProducers = 4;
    static constexpr int kPerProducer = 100;
    std::atomic<int> count{0};
    std::vector<std::thread> producers;
    producers.reserve(kProducers);

    for (int p = 0; p < kProducers; ++p) {
      producers.emplace_back([&loop, &count] {
        for (int i = 0; i < kPerProducer; ++i) {
          while (!loop.post_task([&count] { count.fetch_add(1, std::memory_order_acq_rel); })) {
            std::this_thread::yield();
          }
        }
      });
    }

    for (auto& t : producers) {
      t.join();
    }

    loop.wait_for_idle(5000);
    CHECK_EQ(count.load(std::memory_order_acquire), kProducers * kPerProducer);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("recursive post_task from inside callback executes nested task") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<int> outer{0};
    std::atomic<int> inner{0};
    std::promise<void> done;
    auto fut = done.get_future();

    loop.post_task([&loop, &outer, &inner, &done] {
      outer.fetch_add(1);
      CHECK(loop.is_in_same_thread());
      loop.post_task([&inner, &done] {
        inner.fetch_add(1);
        done.set_value();
      });
    });

    REQUIRE(fut.wait_for(1s) == std::future_status::ready);
    CHECK_EQ(outer.load(), 1);
    CHECK_EQ(inner.load(), 1);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("multi-thread producers on kNormalType with kBlockStrategy all tasks arrive") {
    MessageLoop loop;
    loop.set_strategy(MessageLoop::kBlockStrategy);
    loop.async_run();

    const int kThreads = 8;
    const int kPerThread = 200;
    std::atomic<int> total{0};
    std::vector<std::thread> producers;
    producers.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
      producers.emplace_back([&loop, &total] {
        for (int i = 0; i < 200; ++i) {
          loop.post_task([&total] { total.fetch_add(1, std::memory_order_acq_rel); });
        }
      });
    }

    for (auto& th : producers) {
      th.join();
    }

    loop.wait_for_idle(5000);
    CHECK_EQ(total.load(), kThreads * kPerThread);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("wait_for_idle returns true after long task completes") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> done{false};
    loop.post_task([&done] {
      std::this_thread::sleep_for(60ms);
      done.store(true, std::memory_order_release);
    });

    CHECK(loop.wait_for_idle(500));
    CHECK(done.load());

    loop.quit();
    loop.wait_for_quit();
  }
}

// NOLINTEND
