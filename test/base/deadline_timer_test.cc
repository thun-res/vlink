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

#include "./base/deadline_timer.h"

#include <doctest/doctest.h>

#include <thread>

#include "../common_test.h"
#include "./base/elapsed_timer.h"

TEST_SUITE("base-DeadlineTimer") {
  TEST_CASE("default constructor yields invalid timer with deadline zero") {
    DeadlineTimer t;
    CHECK_FALSE(t.is_valid());
    CHECK_EQ(t.deadline(), 0U);
    CHECK_FALSE(t.has_expired());
  }

  TEST_CASE("interval constructor with positive value yields valid unexpired timer") {
    DeadlineTimer t(1000);
    CHECK(t.is_valid());
    CHECK(t.deadline() > 0U);
    CHECK_FALSE(t.has_expired());
    CHECK(t.remaining_time() > 0);
  }

  TEST_CASE("interval constructor with kMicro accuracy") {
    DeadlineTimer t(100000, ElapsedTimer::kMicro);
    CHECK(t.is_valid());
    CHECK_FALSE(t.has_expired());
    CHECK_EQ(t.get_accuracy(), ElapsedTimer::kMicro);
  }

  TEST_CASE("interval constructor with kNano accuracy") {
    DeadlineTimer t(100000000LL, ElapsedTimer::kNano);
    CHECK(t.is_valid());
    CHECK_FALSE(t.has_expired());
    CHECK_EQ(t.get_accuracy(), ElapsedTimer::kNano);
  }

  TEST_CASE("default accuracy is kMilli") {
    DeadlineTimer t(100);
    CHECK_EQ(t.get_accuracy(), ElapsedTimer::kMilli);
  }

  TEST_CASE("set_deadline makes timer valid") {
    DeadlineTimer t;
    CHECK_FALSE(t.is_valid());

    t.set_deadline(500);
    CHECK(t.is_valid());
  }

  TEST_CASE("set_deadline with zero resets timer to invalid state") {
    DeadlineTimer t;
    t.set_deadline(0);
    CHECK_FALSE(t.is_valid());
    CHECK_FALSE(t.has_expired());
  }

  TEST_CASE("set_deadline produces positive remaining time for large interval") {
    DeadlineTimer t;
    t.set_deadline(10000);
    CHECK(t.remaining_time() > 0);
  }

  TEST_CASE("set_deadline called twice advances the deadline") {
    DeadlineTimer t;
    t.set_deadline(100);
    uint64_t first = t.deadline();

    std::this_thread::sleep_for(10ms);
    t.set_deadline(100);
    uint64_t second = t.deadline();

    CHECK(second > first);
  }

  TEST_CASE("set_deadline_abs makes timer valid and stores the exact value") {
    DeadlineTimer t;
    uint64_t abs = 123456789U;
    t.set_deadline_abs(abs);

    CHECK(t.is_valid());
    CHECK_EQ(t.deadline(), abs);
  }

  TEST_CASE("set_deadline_abs with a future timestamp yields unexpired timer") {
    DeadlineTimer t;
    uint64_t abs = ElapsedTimer::get_cpu_timestamp() + 5000;
    t.set_deadline_abs(abs);

    CHECK(t.is_valid());
    CHECK_FALSE(t.has_expired());
  }

  TEST_CASE("set_deadline_abs with a past timestamp yields expired timer") {
    DeadlineTimer t;
    uint64_t past = ElapsedTimer::get_cpu_timestamp() - 1U;
    t.set_deadline_abs(past);

    CHECK(t.has_expired());
  }

  TEST_CASE("reset invalidates a previously set timer") {
    DeadlineTimer t(500);
    CHECK(t.is_valid());

    t.reset();
    CHECK_FALSE(t.is_valid());
    CHECK_EQ(t.deadline(), 0U);
  }

  TEST_CASE("reset then set_deadline restores valid timer") {
    DeadlineTimer t(100);
    t.reset();
    t.set_deadline(200);

    CHECK(t.is_valid());
  }

  TEST_CASE("timer expires after sleeping longer than the deadline") {
    DeadlineTimer t(50);
    std::this_thread::sleep_for(100ms);

    CHECK(t.has_expired());
  }

  TEST_CASE("remaining_time decreases over time") {
    DeadlineTimer t(2000);
    int64_t r1 = t.remaining_time();

    std::this_thread::sleep_for(20ms);
    int64_t r2 = t.remaining_time();

    CHECK(r2 < r1);
  }

  TEST_CASE("remaining_time is zero or negative after expiry") {
    DeadlineTimer t(30);
    std::this_thread::sleep_for(60ms);

    CHECK(t.remaining_time() <= 0);
  }

  TEST_CASE("copy constructor preserves deadline and validity") {
    DeadlineTimer orig(1000);
    DeadlineTimer copy(orig);  // NOLINT(performance-unnecessary-copy-initialization)

    CHECK(copy.is_valid());
    CHECK_EQ(copy.deadline(), orig.deadline());
  }

  TEST_CASE("move constructor transfers deadline") {
    DeadlineTimer orig(1000);
    uint64_t dl = orig.deadline();

    DeadlineTimer moved(std::move(orig));
    CHECK(moved.is_valid());
    CHECK_EQ(moved.deadline(), dl);
  }

  TEST_CASE("copy assignment preserves deadline") {
    DeadlineTimer a(500);
    DeadlineTimer b;
    b = a;

    CHECK(b.is_valid());
    CHECK_EQ(b.deadline(), a.deadline());
  }

  TEST_CASE("move assignment transfers deadline") {
    DeadlineTimer a(500);
    uint64_t dl = a.deadline();

    DeadlineTimer b;
    b = std::move(a);

    CHECK(b.is_valid());
    CHECK_EQ(b.deadline(), dl);
  }
}

// NOLINTEND
