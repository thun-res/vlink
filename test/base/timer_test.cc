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

#include "./base/timer.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "../common_test.h"
#include "./base/message_loop.h"

TEST_SUITE("base-Timer") {
  TEST_CASE("kInfinite sentinel equals -1") { CHECK_EQ(Timer::kInfinite, -1); }

  TEST_CASE("default constructor creates detached inactive timer with 1000ms interval") {
    Timer t;

    CHECK_FALSE(t.is_active());
    CHECK(t.get_message_loop() == nullptr);
    CHECK_EQ(t.get_interval(), 1000u);
    CHECK_EQ(t.get_loop_count(), Timer::kInfinite);
  }

  TEST_CASE("constructor with loop attaches and sets detached state as inactive") {
    MessageLoop loop;
    loop.async_run();

    Timer t(&loop);

    CHECK_FALSE(t.is_active());
    CHECK_EQ(t.get_message_loop(), &loop);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("fully configured constructor stores interval and loop count") {
    MessageLoop loop;
    loop.async_run();

    Timer t(&loop, 100, 3, nullptr);

    CHECK_EQ(t.get_interval(), 100u);
    CHECK_EQ(t.get_loop_count(), 3);
    CHECK_EQ(t.get_message_loop(), &loop);
    CHECK_FALSE(t.is_active());

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("constructor without loop stores interval and loop count") {
    Timer t(200, 5, nullptr);

    CHECK_EQ(t.get_interval(), 200u);
    CHECK_EQ(t.get_loop_count(), 5);
    CHECK(t.get_message_loop() == nullptr);
    CHECK_FALSE(t.is_active());
  }

  TEST_CASE("set_interval and get_interval round-trip") {
    MessageLoop loop;
    loop.async_run();

    Timer t(&loop, 100, Timer::kInfinite, nullptr);

    t.set_interval(250);

    CHECK_EQ(t.get_interval(), 250u);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("set_loop_count and get_loop_count round-trip") {
    MessageLoop loop;
    loop.async_run();

    Timer t(&loop, 10, 3, nullptr);

    t.set_loop_count(7);
    CHECK_EQ(t.get_loop_count(), 7);

    t.set_loop_count(Timer::kInfinite);
    CHECK_EQ(t.get_loop_count(), Timer::kInfinite);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("is_active transitions false before start, true after start, false after stop") {
    MessageLoop loop;
    loop.async_run();

    Timer t(&loop, 10, Timer::kInfinite, [] {});

    CHECK_FALSE(t.is_active());

    t.start();

    CHECK(t.is_active());

    t.stop();

    CHECK_FALSE(t.is_active());

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("is_strict defaults false and toggles with set_strict") {
    MessageLoop loop;
    loop.async_run();

    Timer t(&loop, 10, Timer::kInfinite, [] {});

    CHECK_FALSE(t.is_strict());

    t.set_strict(true);
    CHECK(t.is_strict());

    t.set_strict(false);
    CHECK_FALSE(t.is_strict());

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("get_priority and set_priority round-trip") {
    MessageLoop loop;
    loop.async_run();

    Timer t(&loop, 10, Timer::kInfinite, [] {});

    t.set_priority(100);

    CHECK_EQ(t.get_priority(), 100u);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("get_message_loop returns the attached loop pointer") {
    MessageLoop loop;
    loop.async_run();

    Timer t(&loop, 10, Timer::kInfinite, [] {});

    CHECK_EQ(t.get_message_loop(), &loop);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("get_remain_loop_count returns kInfinite for infinite timer while running") {
    MessageLoop loop;
    loop.async_run();

    Timer t(&loop, 10, Timer::kInfinite, [] {});
    t.start();

    CHECK_EQ(t.get_remain_loop_count(), Timer::kInfinite);

    t.stop();

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("finite timer fires the configured number of times then stops") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<int> fire_count{0};
    Timer t(&loop, 10, 3, [&fire_count] { fire_count.fetch_add(1); });

    t.start();

    std::this_thread::sleep_for(200ms);

    CHECK_EQ(fire_count.load(), 3);
    CHECK_FALSE(t.is_active());

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("infinite timer fires many times and stop prevents further fires") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<int> count{0};
    Timer t(&loop, 50, Timer::kInfinite, [&count] { count.fetch_add(1); });

    t.start();
    std::this_thread::sleep_for(500ms);
    t.stop();
    std::this_thread::sleep_for(30ms);

    int count_at_stop = count.load();

    CHECK(count_at_stop >= 3);

    std::this_thread::sleep_for(100ms);
    CHECK_EQ(count.load(), count_at_stop);
    CHECK_FALSE(t.is_active());

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("get_invoke_count tracks cumulative fires while running") {
    MessageLoop loop;
    loop.async_run();

    Timer t(&loop, 10, Timer::kInfinite, [] {});
    t.start();

    std::this_thread::sleep_for(80ms);

    CHECK(t.get_invoke_count() >= 1u);

    t.stop();

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("restart re-arms timer and fires again") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<int> fire_count{0};
    Timer t(&loop, 10, 2, [&fire_count] { fire_count.fetch_add(1); });

    t.start();
    std::this_thread::sleep_for(150ms);

    CHECK_EQ(fire_count.load(), 2);
    CHECK_FALSE(t.is_active());

    fire_count.store(0);
    t.restart();

    CHECK(t.is_active());

    std::this_thread::sleep_for(150ms);

    CHECK_EQ(fire_count.load(), 2);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("start with replacement callback uses new callback") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<int> original_count{0};
    std::atomic<int> replacement_count{0};
    Timer t(&loop, 10, 1, [&original_count] { original_count.fetch_add(1); });

    t.start([&replacement_count] { replacement_count.fetch_add(1); });
    std::this_thread::sleep_for(500ms);

    CHECK_EQ(replacement_count.load(), 1);
    CHECK_EQ(original_count.load(), 0);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("set_callback replaces callback before start") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<int> a_count{0};
    std::atomic<int> b_count{0};
    Timer t(&loop, 10, 1, nullptr);

    t.set_callback([&a_count] { a_count.fetch_add(1); });
    t.set_callback([&b_count] { b_count.fetch_add(1); });
    t.start();

    std::this_thread::sleep_for(500ms);

    CHECK_EQ(b_count.load(), 1);
    CHECK_EQ(a_count.load(), 0);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("attach and detach bind and unbind loop pointer") {
    MessageLoop loop;
    loop.async_run();

    Timer t(10, Timer::kInfinite, nullptr);

    CHECK(t.get_message_loop() == nullptr);

    bool attached = t.attach(&loop);

    CHECK(attached);
    CHECK_EQ(t.get_message_loop(), &loop);

    bool detached = t.detach();

    CHECK(detached);
    CHECK(t.get_message_loop() == nullptr);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("re-attach to different loop changes delivery target") {
    MessageLoop loop1;
    MessageLoop loop2;
    loop1.async_run();
    loop2.async_run();

    std::atomic<int> count{0};
    Timer t(20, 1, [&count] { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK(t.attach(&loop1));
    CHECK(t.attach(&loop2));
    CHECK_EQ(t.get_message_loop(), &loop2);

    t.start();
    std::this_thread::sleep_for(120ms);

    CHECK_EQ(count.load(), 1);

    loop1.quit();
    loop2.quit();
    loop1.wait_for_quit();
    loop2.wait_for_quit();
  }

  TEST_CASE("detached timer does not fire after detach") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<int> count{0};
    Timer t(&loop, 10, Timer::kInfinite, [&count] { count.fetch_add(1); });
    t.start();

    std::this_thread::sleep_for(80ms);
    t.detach();
    std::this_thread::sleep_for(30ms);

    int count_at_detach = count.load();

    std::this_thread::sleep_for(50ms);

    CHECK_EQ(count.load(), count_at_detach);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("attach with nullptr throws") {
    Timer t;

    CHECK_THROWS(t.attach(nullptr));
  }

  TEST_CASE("call_once fires exactly once") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<int> count{0};
    bool ret = Timer::call_once(&loop, 10, [&count] { count.fetch_add(1); });

    CHECK(ret);

    std::this_thread::sleep_for(200ms);

    CHECK_EQ(count.load(), 1);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("call_once with priority parameter delivers on priority loop") {
    MessageLoop loop(MessageLoop::kPriorityType);
    loop.async_run();

    std::atomic<int> count{0};
    bool ret = Timer::call_once(&loop, 10, [&count] { count.fetch_add(1); }, 100);

    CHECK(ret);

    std::this_thread::sleep_for(80ms);

    CHECK_EQ(count.load(), 1);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("stop then start fires callback again from the beginning") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<int> count{0};
    Timer t(&loop, 10, Timer::kInfinite, [&count] { count.fetch_add(1); });

    t.start();
    std::this_thread::sleep_for(80ms);
    t.stop();
    std::this_thread::sleep_for(30ms);

    int after_stop = count.load();

    CHECK(after_stop >= 1);

    t.start();
    std::this_thread::sleep_for(80ms);
    t.stop();

    CHECK(count.load() > after_stop);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("set_loop_count changes count for next restart") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<int> count{0};
    Timer t(&loop, 10, 1, [&count] { count.fetch_add(1); });

    t.start();
    std::this_thread::sleep_for(200ms);

    CHECK_EQ(count.load(), 1);

    count.store(0);
    t.set_loop_count(3);
    t.restart();

    std::this_thread::sleep_for(200ms);

    CHECK_EQ(count.load(), 3);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("destructor stops timer automatically") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<int> count{0};

    {
      Timer t(&loop, 10, Timer::kInfinite, [&count] { count.fetch_add(1); });
      t.start();
      std::this_thread::sleep_for(50ms);
    }

    std::this_thread::sleep_for(30ms);

    int count_at_destroy = count.load();

    std::this_thread::sleep_for(50ms);

    CHECK_EQ(count.load(), count_at_destroy);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("constructor without loop then attach then start fires correctly") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<int> count{0};
    Timer t(10, 2, [&count] { count.fetch_add(1); });

    CHECK_FALSE(t.is_active());

    t.attach(&loop);
    t.start();

    CHECK(t.is_active());

    std::this_thread::sleep_for(200ms);

    CHECK_EQ(count.load(), 2);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("set_interval takes effect and timer fires with new period") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<int> count{0};
    Timer t(&loop, 200, Timer::kInfinite, [&count] { count.fetch_add(1); });

    t.start();
    std::this_thread::sleep_for(50ms);

    CHECK_EQ(count.load(), 0);

    t.stop();
    t.set_interval(10);
    t.start();

    std::this_thread::sleep_for(200ms);
    t.stop();

    CHECK(count.load() >= 3);

    loop.quit();
    loop.wait_for_quit();
  }
}

// NOLINTEND
