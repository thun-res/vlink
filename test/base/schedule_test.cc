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

#include "./base/schedule.h"

#include <doctest/doctest.h>

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "../common_test.h"
#include "./base/message_loop.h"

TEST_SUITE("base-Schedule") {
  TEST_CASE("Config default constructor sets all fields to zero") {
    Schedule::Config cfg;
    CHECK_EQ(cfg.delay_ms, 0u);
    CHECK_EQ(cfg.priority, 0u);
    CHECK_EQ(cfg.schedule_timeout_ms, 0u);
    CHECK_EQ(cfg.execution_timeout_ms, 0u);
  }

  TEST_CASE("Config explicit constructor sets all fields") {
    Schedule::Config cfg(100, 50, 200, 300);
    CHECK_EQ(cfg.delay_ms, 100u);
    CHECK_EQ(cfg.priority, 50u);
    CHECK_EQ(cfg.schedule_timeout_ms, 200u);
    CHECK_EQ(cfg.execution_timeout_ms, 300u);
  }

  TEST_CASE("Config explicit constructor with only delay defaults others to zero") {
    Schedule::Config cfg(50);
    CHECK_EQ(cfg.delay_ms, 50u);
    CHECK_EQ(cfg.priority, 0u);
    CHECK_EQ(cfg.schedule_timeout_ms, 0u);
    CHECK_EQ(cfg.execution_timeout_ms, 0u);
  }

  TEST_CASE("Config explicit constructor with delay and priority") {
    Schedule::Config cfg(10, 5);
    CHECK_EQ(cfg.delay_ms, 10u);
    CHECK_EQ(cfg.priority, 5u);
    CHECK_EQ(cfg.schedule_timeout_ms, 0u);
    CHECK_EQ(cfg.execution_timeout_ms, 0u);
  }

  TEST_CASE("Status default construction is valid") {
    Schedule::Status s;
    CHECK(s.is_valid());
  }

  TEST_CASE("Status set_valid and is_valid are consistent") {
    Schedule::Status s;
    s.set_valid(true);
    CHECK(s.is_valid());

    s.set_valid(false);
    CHECK_FALSE(s.is_valid());
  }

  TEST_CASE("Status is move constructible") {
    Schedule::Status s;
    s.set_valid(true);
    Schedule::Status s2(std::move(s));
    CHECK(s2.is_valid());
  }

  TEST_CASE("Status is move assignable") {
    Schedule::Status s;
    s.set_valid(true);
    Schedule::Status s2;
    s2 = std::move(s);
    CHECK(s2.is_valid());
  }

  TEST_CASE("Status on_schedule_timeout chaining returns self reference") {
    Schedule::Callback wrapper;
    auto status = Schedule::process(Schedule::Config{}, []() {}, wrapper);
    auto& ref = status.on_schedule_timeout([]() {});
    CHECK_EQ(&ref, &status);
  }

  TEST_CASE("Status on_execution_timeout chaining returns self reference") {
    Schedule::Callback wrapper;
    auto status = Schedule::process(Schedule::Config{}, []() {}, wrapper);
    auto& ref = status.on_execution_timeout([]() {});
    CHECK_EQ(&ref, &status);
  }

  TEST_CASE("Status on_catch chaining returns self reference") {
    Schedule::Callback wrapper;
    auto status = Schedule::process(Schedule::Config{}, []() {}, wrapper);
    auto& ref = status.on_catch([](std::exception&) {});
    CHECK_EQ(&ref, &status);
  }

  TEST_CASE("process with void callback wrapper fires the callback") {
    std::atomic<bool> ran{false};
    Schedule::Callback wrapper;
    auto status = Schedule::process(Schedule::Config{}, [&ran]() { ran.store(true); }, wrapper);

    CHECK(status.is_valid());
    REQUIRE(wrapper != nullptr);

    wrapper();
    CHECK(ran.load());
  }

  TEST_CASE("two process wrappers are independent") {
    std::atomic<int> count{0};
    Schedule::Callback w1;
    Schedule::Callback w2;
    (void)Schedule::process(Schedule::Config{}, [&count]() { count.fetch_add(1); }, w1);
    (void)Schedule::process(Schedule::Config{}, [&count]() { count.fetch_add(10); }, w2);

    w1();
    w2();
    CHECK_EQ(count.load(), 11);
  }

  TEST_CASE("process_with_ret on_then fires when callback returns true") {
    std::atomic<bool> then_ran{false};
    std::atomic<bool> else_ran{false};
    Schedule::Callback wrapper;
    auto ret_status = Schedule::process_with_ret(Schedule::Config{}, []() -> bool { return true; }, wrapper);

    ret_status
        .on_then([&then_ran]() -> bool {
          then_ran.store(true);
          return true;
        })
        .on_else([&else_ran]() { else_ran.store(true); });

    REQUIRE(wrapper != nullptr);
    wrapper();

    CHECK(then_ran.load());
    CHECK_FALSE(else_ran.load());
  }

  TEST_CASE("process_with_ret on_else fires when callback returns false") {
    std::atomic<bool> then_ran{false};
    std::atomic<bool> else_ran{false};
    Schedule::Callback wrapper;
    auto ret_status = Schedule::process_with_ret(Schedule::Config{}, []() -> bool { return false; }, wrapper);

    ret_status
        .on_then([&then_ran]() -> bool {
          then_ran.store(true);
          return true;
        })
        .on_else([&else_ran]() { else_ran.store(true); });

    REQUIRE(wrapper != nullptr);
    wrapper();

    CHECK_FALSE(then_ran.load());
    CHECK(else_ran.load());
  }

  TEST_CASE("RetStatus on_then chaining returns RetStatus reference") {
    Schedule::Callback wrapper;
    auto ret_status = Schedule::process_with_ret(Schedule::Config{}, []() -> bool { return true; }, wrapper);
    auto& ref = ret_status.on_then([]() -> bool { return true; });
    CHECK_EQ(&ref, &ret_status);
  }

  TEST_CASE("chained on_then stops when one callback returns false") {
    std::atomic<int> count{0};
    Schedule::Callback wrapper;
    auto ret_status = Schedule::process_with_ret(Schedule::Config{}, []() -> bool { return true; }, wrapper);

    ret_status
        .on_then([&count]() -> bool {
          count.fetch_add(1);
          return false;
        })
        .on_then([&count]() -> bool {
          count.fetch_add(1);
          return true;
        });

    REQUIRE(wrapper != nullptr);
    wrapper();
    CHECK_EQ(count.load(), 1);
  }

  TEST_CASE("RetStatus on_catch fires and on_then on_else do not on exception") {
    std::atomic<bool> caught{false};
    std::atomic<bool> then_ran{false};
    std::atomic<bool> else_ran{false};
    Schedule::Callback wrapper;
    auto ret_status =
        Schedule::process_with_ret(Schedule::Config{}, []() -> bool { throw std::runtime_error("fail"); }, wrapper);

    ret_status
        .on_then([&then_ran]() -> bool {
          then_ran.store(true);
          return true;
        })
        .on_else([&else_ran]() { else_ran.store(true); })
        .on_catch([&caught](std::exception&) { caught.store(true); });

    REQUIRE(wrapper != nullptr);
    wrapper();

    CHECK(caught.load());
    CHECK_FALSE(then_ran.load());
    CHECK_FALSE(else_ran.load());
  }

  TEST_CASE("exec_task void callback runs and status is valid") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> ran{false};
    auto status = loop.exec_task(Schedule::Config{}, [&ran]() { ran.store(true); });
    CHECK(status.is_valid());
    loop.wait_for_idle();
    CHECK(ran.load());

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("delay_ms defers execution") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> ran{false};
    auto status = loop.exec_task(Schedule::Config{80}, [&ran]() { ran.store(true); });
    CHECK(status.is_valid());

    std::this_thread::sleep_for(40ms);
    CHECK_FALSE(ran.load());

    std::this_thread::sleep_for(80ms);
    CHECK(ran.load());

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("delay_ms zero posts immediately") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> ran{false};
    loop.exec_task(Schedule::Config{0}, [&ran]() { ran.store(true); });
    loop.wait_for_idle();
    CHECK(ran.load());

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("multiple exec_task void callbacks all run") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<int> count{0};
    for (int i = 0; i < 5; ++i) {
      loop.exec_task(Schedule::Config{}, [&count]() { count.fetch_add(1); });
    }
    loop.wait_for_idle();
    CHECK_EQ(count.load(), 5);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("on_execution_timeout fires when task exceeds limit") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> timeout_fired{false};
    loop.exec_task(Schedule::Config{50, 0, 0, 30}, []() { std::this_thread::sleep_for(100ms); })
        .on_execution_timeout([&timeout_fired]() { timeout_fired.store(true); });

    std::this_thread::sleep_for(220ms);
    CHECK(timeout_fired.load());

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("on_execution_timeout does not fire when task is fast") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> timeout_fired{false};
    loop.exec_task(Schedule::Config{0, 0, 0, 200}, []() {}).on_execution_timeout([&timeout_fired]() {
      timeout_fired.store(true);
    });
    loop.wait_for_idle();
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(timeout_fired.load());

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("on_schedule_timeout fires when task waits too long in queue") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> sched_timeout{false};
    std::atomic<bool> gate{false};

    loop.post_task([&gate]() {
      while (!gate.load()) {
        std::this_thread::yield();
      }
    });

    std::this_thread::sleep_for(30ms);

    loop.exec_task(Schedule::Config{0, 0, 50, 0}, []() {}).on_schedule_timeout([&sched_timeout]() {
      sched_timeout.store(true);
    });

    std::this_thread::sleep_for(120ms);
    gate.store(true);
    loop.wait_for_idle();

    CHECK(sched_timeout.load());

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("on_schedule_timeout does not fire when task starts promptly") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> sched_timeout{false};
    loop.exec_task(Schedule::Config{0, 0, 200, 0}, []() {}).on_schedule_timeout([&sched_timeout]() {
      sched_timeout.store(true);
    });
    loop.wait_for_idle();
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(sched_timeout.load());

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("on_catch fires when void callback throws std::exception") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> caught{false};
    std::string caught_msg;
    std::mutex msg_mtx;

    loop.exec_task(Schedule::Config{100}, []() { throw std::runtime_error("test error"); })
        .on_catch([&caught, &caught_msg, &msg_mtx](std::exception& e) {
          std::lock_guard lk(msg_mtx);
          caught.store(true);
          caught_msg = e.what();
        });

    std::this_thread::sleep_for(150ms);

    CHECK(caught.load());
    {
      std::lock_guard lk(msg_mtx);
      CHECK_EQ(caught_msg, "test error");
    }

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("on_catch does not fire when no exception is thrown") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> caught{false};
    loop.exec_task(Schedule::Config{}, []() {}).on_catch([&caught](std::exception&) { caught.store(true); });
    loop.wait_for_idle();
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(caught.load());

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("exec_task bool callback on_then fires when returns true") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> then_ran{false};
    std::atomic<bool> else_ran{false};

    loop.exec_task(Schedule::Config{}, []() -> bool { return true; })
        .on_then([&then_ran]() -> bool {
          then_ran.store(true);
          return true;
        })
        .on_else([&else_ran]() { else_ran.store(true); });

    loop.wait_for_idle();
    CHECK(then_ran.load());
    CHECK_FALSE(else_ran.load());

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("RetStatus on_catch fires on exception in bool callback") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> caught{false};
    std::atomic<bool> then_ran{false};

    loop.exec_task(Schedule::Config{100},
                   []() -> bool {
                     throw std::logic_error("boom");
                     return true;
                   })
        .on_then([&then_ran]() -> bool {
          then_ran.store(true);
          return true;
        })
        .on_catch([&caught](std::exception&) { caught.store(true); });

    loop.wait_for_idle();
    for (int i = 0; i < 100 && !caught.load(); ++i) {
      std::this_thread::sleep_for(20ms);
    }

    CHECK(caught.load());
    CHECK_FALSE(then_ran.load());

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("Config priority used by kPriorityType loop for ordering") {
    MessageLoop loop(MessageLoop::kPriorityType);
    loop.async_run();

    std::atomic<bool> gate{false};
    std::vector<int> order;
    std::mutex mtx;

    loop.post_task([&gate]() {
      while (!gate.load()) {
        std::this_thread::yield();
      }
    });
    std::this_thread::sleep_for(20ms);

    loop.exec_task(Schedule::Config{0, MessageLoop::kLowestPriority}, [&order, &mtx]() {
      std::lock_guard lk(mtx);
      order.push_back(1);
    });
    loop.exec_task(Schedule::Config{0, MessageLoop::kHighestPriority}, [&order, &mtx]() {
      std::lock_guard lk(mtx);
      order.push_back(3);
    });
    loop.exec_task(Schedule::Config{0, MessageLoop::kNormalPriority}, [&order, &mtx]() {
      std::lock_guard lk(mtx);
      order.push_back(2);
    });

    gate.store(true);
    loop.wait_for_idle();

    REQUIRE_EQ(order.size(), 3u);
    CHECK_EQ(order[0], 3);
    CHECK_EQ(order[1], 2);
    CHECK_EQ(order[2], 1);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("chained on_then all fire when each returns true") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<int> chain_count{0};
    loop.exec_task(Schedule::Config{50}, []() -> bool { return true; })
        .on_then([&chain_count]() -> bool {
          chain_count.fetch_add(1);
          return true;
        })
        .on_then([&chain_count]() -> bool {
          chain_count.fetch_add(1);
          return true;
        })
        .on_then([&chain_count]() -> bool {
          chain_count.fetch_add(1);
          return true;
        });

    for (int attempt = 0; attempt < 300 && chain_count.load() < 3; ++attempt) {
      loop.wait_for_idle();
      std::this_thread::sleep_for(10ms);
    }

    CHECK_EQ(chain_count.load(), 3);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("on_then registered after dispatch is ignored") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<int> first_then_count{0};
    std::atomic<int> late_then_count{0};

    auto status = loop.exec_task(Schedule::Config{}, []() -> bool { return true; });
    status.on_then([&first_then_count]() -> bool {
      first_then_count.fetch_add(1);
      return true;
    });

    for (int attempt = 0; attempt < 300 && first_then_count.load() == 0; ++attempt) {
      loop.wait_for_idle();
      std::this_thread::sleep_for(10ms);
    }

    REQUIRE_EQ(first_then_count.load(), 1);
    status.on_then([&late_then_count]() -> bool {
      late_then_count.fetch_add(1);
      return true;
    });

    CHECK_EQ(late_then_count.load(), 0);
    loop.wait_for_idle();
    CHECK_EQ(late_then_count.load(), 0);

    loop.quit();
    loop.wait_for_quit();
  }
}

// NOLINTEND
