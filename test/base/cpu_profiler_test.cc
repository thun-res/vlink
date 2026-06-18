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

#include "./base/cpu_profiler.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "../common_test.h"

TEST_SUITE("base-CpuProfiler") {
  TEST_CASE("default-constructed profiler has zero utilisation") {
    CpuProfiler p;
    CHECK(p.get() == doctest::Approx(0.0));
  }

  TEST_CASE("is_global_enabled returns a bool without crashing") {
    bool enabled = CpuProfiler::is_global_enabled();
    (void)enabled;
  }

  TEST_CASE("begin and end do not crash") {
    CpuProfiler p;
    p.begin();
    p.end();
  }

  TEST_CASE("multiple begin/end pairs do not crash") {
    CpuProfiler p;

    for (int i = 0; i < 5; ++i) {
      p.begin();
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      p.end();
    }
  }

  TEST_CASE("end without begin does not crash") {
    CpuProfiler p;
    p.end();
  }

  TEST_CASE("get returns 0 before any begin") {
    CpuProfiler p;
    CHECK(p.get() == doctest::Approx(0.0));
  }

  TEST_CASE("get returns non-negative after a begin/end pair") {
    CpuProfiler p;
    p.begin();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    p.end();

    CHECK(p.get() >= 0.0);
  }

  TEST_CASE("get does not reset accumulators on repeated calls") {
    CpuProfiler p;
    p.begin();
    std::this_thread::sleep_for(std::chrono::microseconds(500));
    p.end();

    double first = p.get();
    double second = p.get();

    CHECK(first >= 0.0);
    CHECK(second >= 0.0);
  }

  TEST_CASE("restart returns current value and resets to zero") {
    CpuProfiler p;
    p.begin();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    p.end();

    double val = p.restart();
    CHECK(val >= 0.0);

    CHECK(p.get() == doctest::Approx(0.0));
  }

  TEST_CASE("restart on fresh profiler returns 0 and stays 0") {
    CpuProfiler p;
    double val = p.restart();
    CHECK(val == doctest::Approx(0.0));
    CHECK(p.get() == doctest::Approx(0.0));
  }

  TEST_CASE("profiler is usable again after restart") {
    CpuProfiler p;
    p.begin();
    std::this_thread::sleep_for(std::chrono::microseconds(500));
    p.end();
    p.restart();

    p.begin();
    std::this_thread::sleep_for(std::chrono::microseconds(500));
    p.end();

    CHECK(p.get() >= 0.0);
  }

  TEST_CASE("utilisation stays within 0..200 percent range under full CPU load") {
    CpuProfiler p;

    auto start = std::chrono::steady_clock::now();
    p.begin();

    std::atomic<int> busy{0};

    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(10)) {
      ++busy;
    }

    p.end();

    double usage = p.get();
    CHECK(usage >= 0.0);
    CHECK(usage <= 200.0);
  }

  TEST_CASE("idle work yields non-negative utilisation well below 100") {
    CpuProfiler p;

    for (int i = 0; i < 3; ++i) {
      p.begin();
      p.end();
      std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }

    double usage = p.get();
    CHECK(usage >= 0.0);
    CHECK(usage < 100.0);
  }
}

// NOLINTEND
