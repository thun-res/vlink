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

#include "./base/cpu_profiler_guard.h"

#include <doctest/doctest.h>

#include <thread>
#include <type_traits>

#include "../common_test.h"
#include "./base/cpu_profiler.h"

TEST_SUITE("base-CpuProfilerGuard") {
  TEST_CASE("construction with nullptr does not crash") { CpuProfilerGuard guard(nullptr); }

  TEST_CASE("construction with valid profiler calls begin and end automatically") {
    CpuProfiler profiler;

    {
      CpuProfilerGuard guard(&profiler);
      std::this_thread::sleep_for(1ms);
    }

    CHECK(profiler.get() >= 0.0);
  }

  TEST_CASE("multiple guards on the same profiler accumulate correctly") {
    CpuProfiler profiler;

    for (int i = 0; i < 3; ++i) {
      CpuProfilerGuard guard(&profiler);
      std::this_thread::sleep_for(1ms);
    }

    CHECK(profiler.get() >= 0.0);
  }

  TEST_CASE("guard is non-copyable and non-movable") {
    CHECK_FALSE(std::is_copy_constructible_v<CpuProfilerGuard>);
    CHECK_FALSE(std::is_copy_assignable_v<CpuProfilerGuard>);
    CHECK_FALSE(std::is_move_constructible_v<CpuProfilerGuard>);
    CHECK_FALSE(std::is_move_assignable_v<CpuProfilerGuard>);
  }
}

// NOLINTEND
