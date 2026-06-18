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

#include "./base/elapsed_timer.h"

#include <doctest/doctest.h>

#include <thread>

#include "../common_test.h"

static void busy_wait_ms(int ms) {
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);

  while (std::chrono::steady_clock::now() < end) {
  }
}

TEST_SUITE("base-ElapsedTimer") {
  TEST_CASE("get_sys_timestamp returns positive value for all accuracies") {
    uint64_t ms = ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMilli);
    uint64_t us = ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMicro);
    uint64_t ns = ElapsedTimer::get_sys_timestamp(ElapsedTimer::kNano);

    CHECK(ms > 0U);
    CHECK(us > 0U);
    CHECK(ns > 0U);
  }

  TEST_CASE("get_sys_timestamp with high_resolution=false returns positive value") {
    uint64_t ts = ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMilli, false);
    CHECK(ts > 0U);
  }

  TEST_CASE("get_sys_timestamp is non-decreasing over time") {
    uint64_t t1 = ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMilli);
    std::this_thread::sleep_for(5ms);
    uint64_t t2 = ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMilli);

    CHECK(t2 >= t1);
  }

  TEST_CASE("get_cpu_timestamp returns positive value for all accuracies") {
    uint64_t ms = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);
    uint64_t us = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMicro);
    uint64_t ns = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano);

    CHECK(ms > 0U);
    CHECK(us > 0U);
    CHECK(ns > 0U);
  }

  TEST_CASE("get_cpu_timestamp with high_resolution=false returns positive value") {
    uint64_t ts = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli, false);
    CHECK(ts > 0U);
  }

  TEST_CASE("get_cpu_timestamp is monotonically non-decreasing") {
    uint64_t t1 = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMicro);
    busy_wait_ms(5);
    uint64_t t2 = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMicro);

    CHECK(t2 >= t1);
  }

  TEST_CASE("get_cpu_active_time returns positive value for all accuracies") {
    busy_wait_ms(5);
    uint64_t ms = ElapsedTimer::get_cpu_active_time(ElapsedTimer::kMilli);
    uint64_t us = ElapsedTimer::get_cpu_active_time(ElapsedTimer::kMicro);
    uint64_t ns = ElapsedTimer::get_cpu_active_time(ElapsedTimer::kNano);

    CHECK(ms > 0U);
    CHECK(us > 0U);
    CHECK(ns > 0U);
  }

  TEST_CASE("accuracy units are ordered: nano value > micro value > milli value") {
    uint64_t ms = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);
    uint64_t us = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMicro);
    uint64_t ns = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano);

    CHECK(us >= ms);
    CHECK(ns >= us);
  }

  TEST_CASE("default constructor yields inactive timer with kCpuTimestamp and kMilli") {
    ElapsedTimer t;
    CHECK_FALSE(t.is_active());
    CHECK_EQ(t.get(), -1);
    CHECK_EQ(t.get_method(), ElapsedTimer::kCpuTimestamp);
    CHECK_EQ(t.get_accuracy(), ElapsedTimer::kMilli);
  }

  TEST_CASE("constructor(Method) sets method and uses default kMilli accuracy") {
    ElapsedTimer t(ElapsedTimer::kCpuActiveTime);
    CHECK_EQ(t.get_method(), ElapsedTimer::kCpuActiveTime);
    CHECK_EQ(t.get_accuracy(), ElapsedTimer::kMilli);
    CHECK_FALSE(t.is_active());
  }

  TEST_CASE("constructor(Accuracy) sets accuracy and uses default kCpuTimestamp method") {
    ElapsedTimer t(ElapsedTimer::kMicro);
    CHECK_EQ(t.get_method(), ElapsedTimer::kCpuTimestamp);
    CHECK_EQ(t.get_accuracy(), ElapsedTimer::kMicro);
    CHECK_FALSE(t.is_active());
  }

  TEST_CASE("constructor(Method, Accuracy) sets both fields") {
    ElapsedTimer t(ElapsedTimer::kCpuActiveTime, ElapsedTimer::kNano);
    CHECK_EQ(t.get_method(), ElapsedTimer::kCpuActiveTime);
    CHECK_EQ(t.get_accuracy(), ElapsedTimer::kNano);
    CHECK_FALSE(t.is_active());
  }

  TEST_CASE("accuracy accessor matches constructor argument for all values") {
    ElapsedTimer t_milli(ElapsedTimer::kMilli);
    ElapsedTimer t_micro(ElapsedTimer::kMicro);
    ElapsedTimer t_nano(ElapsedTimer::kNano);

    CHECK_EQ(t_milli.get_accuracy(), ElapsedTimer::kMilli);
    CHECK_EQ(t_micro.get_accuracy(), ElapsedTimer::kMicro);
    CHECK_EQ(t_nano.get_accuracy(), ElapsedTimer::kNano);
  }

  TEST_CASE("start makes timer active and get returns non-negative") {
    ElapsedTimer t;
    t.start();

    CHECK(t.is_active());
    CHECK(t.get() >= 0);
  }

  TEST_CASE("get before start returns -1") {
    ElapsedTimer t;
    CHECK_EQ(t.get(), -1);
    CHECK_FALSE(t.is_active());
  }

  TEST_CASE("stop deactivates timer and subsequent get returns -1") {
    ElapsedTimer t;
    t.start();
    CHECK(t.is_active());

    t.stop();
    CHECK_FALSE(t.is_active());
    CHECK_EQ(t.get(), -1);
  }

  TEST_CASE("start is idempotent when already active") {
    ElapsedTimer t;
    t.start();
    int64_t first_read = t.get();

    t.start();

    CHECK(t.is_active());
    CHECK(t.get() >= first_read);
  }

  TEST_CASE("elapsed time increases while timer is running") {
    ElapsedTimer t(ElapsedTimer::kCpuTimestamp, ElapsedTimer::kMilli);
    t.start();

    int64_t t1 = t.get();
    std::this_thread::sleep_for(10ms);
    int64_t t2 = t.get();

    CHECK(t2 >= t1);
  }

  TEST_CASE("elapsed time in microseconds is measurable after busy-wait") {
    ElapsedTimer t(ElapsedTimer::kCpuTimestamp, ElapsedTimer::kMicro);
    t.start();
    busy_wait_ms(10);
    int64_t elapsed = t.get();

    CHECK(elapsed >= 0);
    CHECK(elapsed >= 1000);
  }

  TEST_CASE("get while running returns at least 10 ms after sleep") {
    ElapsedTimer t(ElapsedTimer::kMilli);
    t.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    int64_t elapsed = t.get();
    CHECK(elapsed >= 10);

    t.stop();
  }

  TEST_CASE("stop then get returns -1") {
    ElapsedTimer t(ElapsedTimer::kMilli);
    t.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    t.stop();

    CHECK_EQ(t.get(), -1);

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_EQ(t.get(), -1);
  }

  TEST_CASE("is_active transitions correctly across start and stop") {
    ElapsedTimer t;
    CHECK_FALSE(t.is_active());

    t.start();
    CHECK(t.is_active());

    t.stop();
    CHECK_FALSE(t.is_active());
  }

  TEST_CASE("kMicro accuracy returns at least 5000 us after 10 ms sleep") {
    ElapsedTimer t(ElapsedTimer::kMicro);
    t.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    int64_t us = t.get();
    CHECK(us >= 5000);

    t.stop();
  }

  TEST_CASE("restart returns elapsed time and resets the timer") {
    ElapsedTimer t(ElapsedTimer::kCpuTimestamp, ElapsedTimer::kMilli);
    t.start();
    std::this_thread::sleep_for(10ms);

    int64_t elapsed = t.restart();

    CHECK(elapsed >= 0);
    CHECK(t.is_active());
    CHECK(t.get() >= 0);
  }

  TEST_CASE("restart without prior start activates the timer") {
    ElapsedTimer t;
    (void)t.restart();

    CHECK(t.is_active());
  }

  TEST_CASE("stop then start cycles work correctly") {
    ElapsedTimer t;
    t.start();
    busy_wait_ms(5);
    t.stop();

    CHECK_FALSE(t.is_active());
    CHECK_EQ(t.get(), -1);

    t.start();
    CHECK(t.is_active());
    CHECK(t.get() >= 0);
  }

  TEST_CASE("kCpuActiveTime method start and get work") {
    ElapsedTimer t(ElapsedTimer::kCpuActiveTime, ElapsedTimer::kMilli);
    t.start();
    CHECK(t.is_active());

    busy_wait_ms(10);

    CHECK(t.get() >= 0);
  }

  TEST_CASE("copy constructor preserves method, accuracy, and active state") {
    ElapsedTimer original(ElapsedTimer::kCpuTimestamp, ElapsedTimer::kMicro);
    original.start();

    ElapsedTimer copy(original);
    CHECK_EQ(copy.get_method(), original.get_method());
    CHECK_EQ(copy.get_accuracy(), original.get_accuracy());
    CHECK_EQ(copy.is_active(), original.is_active());
  }

  TEST_CASE("copy assignment preserves method and accuracy") {
    ElapsedTimer src(ElapsedTimer::kCpuActiveTime, ElapsedTimer::kNano);
    ElapsedTimer dst;
    dst = src;

    CHECK_EQ(dst.get_method(), ElapsedTimer::kCpuActiveTime);
    CHECK_EQ(dst.get_accuracy(), ElapsedTimer::kNano);
  }

  TEST_CASE("move constructor preserves method, accuracy, and active state") {
    ElapsedTimer original(ElapsedTimer::kCpuTimestamp, ElapsedTimer::kMicro);
    original.start();

    bool was_active = original.is_active();
    ElapsedTimer moved(std::move(original));

    CHECK_EQ(moved.get_method(), ElapsedTimer::kCpuTimestamp);
    CHECK_EQ(moved.get_accuracy(), ElapsedTimer::kMicro);
    CHECK_EQ(moved.is_active(), was_active);
  }

  TEST_CASE("kNano accuracy gives larger raw values than kMilli for same interval") {
    ElapsedTimer t_ms(ElapsedTimer::kCpuTimestamp, ElapsedTimer::kMilli);
    ElapsedTimer t_ns(ElapsedTimer::kCpuTimestamp, ElapsedTimer::kNano);

    t_ms.start();
    t_ns.start();
    busy_wait_ms(5);

    int64_t elapsed_ms = t_ms.get();
    int64_t elapsed_ns = t_ns.get();

    CHECK(elapsed_ms >= 0);
    CHECK(elapsed_ns >= 0);
    CHECK(elapsed_ns > elapsed_ms);
  }
}

// NOLINTEND
