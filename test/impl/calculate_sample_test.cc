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

#include "./impl/calculate_sample.h"

#include <doctest/doctest.h>

#include <thread>
#include <vector>

#include "../common_test.h"

TEST_SUITE("impl-CalculateSample") {
  TEST_CASE("default constructed state is zero") {
    CalculateSample cs;
    CHECK_EQ(cs.get_total(), 0u);
    CHECK_EQ(cs.get_lost(), 0u);
  }

  TEST_CASE("first message initialises state with no loss") {
    CalculateSample cs;
    cs.update(1, 0);
    CHECK_EQ(cs.get_total(), 1u);
    CHECK_EQ(cs.get_lost(), 0u);
  }

  TEST_CASE("consecutive messages produce no loss") {
    CalculateSample cs;
    cs.update(1, 0);
    cs.update(2, 0);
    cs.update(3, 0);
    CHECK_EQ(cs.get_total(), 3u);
    CHECK_EQ(cs.get_lost(), 0u);
  }

  TEST_CASE("single gap of one is detected") {
    CalculateSample cs;
    cs.update(1, 0);
    cs.update(3, 0);
    CHECK_EQ(cs.get_total(), 3u);
    CHECK_EQ(cs.get_lost(), 1u);
  }

  TEST_CASE("single gap of five is detected") {
    CalculateSample cs;
    cs.update(1, 0);
    cs.update(7, 0);
    CHECK_EQ(cs.get_lost(), 5u);
  }

  TEST_CASE("multiple gaps accumulate correctly") {
    CalculateSample cs;
    cs.update(1, 0);
    cs.update(3, 0);
    cs.update(6, 0);
    cs.update(10, 0);
    CHECK_EQ(cs.get_lost(), 6u);
  }

  TEST_CASE("update with default guid argument equals guid zero") {
    CalculateSample cs;
    cs.update(5);
    cs.update(6);
    cs.update(7);
    CHECK_EQ(cs.get_total(), 3u);
    CHECK_EQ(cs.get_lost(), 0u);
  }

  TEST_CASE("wrap-around sequence treated as reset rather than massive loss") {
    CalculateSample cs;
    cs.update(100, 0);
    cs.update(0, 0);
    CHECK_EQ(cs.get_lost(), 0u);
  }

  TEST_CASE("two senders are tracked independently") {
    CalculateSample cs;
    cs.update(1, 100);
    cs.update(2, 100);
    cs.update(1, 200);
    cs.update(3, 200);
    CHECK_EQ(cs.get_lost(), 1u);
    CHECK_EQ(cs.get_total(), 5u);
  }

  TEST_CASE("three senders with gaps accumulate total loss") {
    CalculateSample cs;
    cs.update(1, 1);
    cs.update(3, 1);
    cs.update(1, 2);
    cs.update(4, 2);
    cs.update(1, 3);
    cs.update(6, 3);
    CHECK_EQ(cs.get_lost(), 7u);
  }

  TEST_CASE("clean sender does not interfere with gapped sender total") {
    CalculateSample cs;
    cs.update(1, 1);
    cs.update(2, 1);
    cs.update(3, 1);
    cs.update(10, 2);
    cs.update(15, 2);
    CHECK_EQ(cs.get_lost(), 4u);
    CHECK_EQ(cs.get_total(), 9u);
  }

  TEST_CASE("get_total sums contributions from all guids") {
    CalculateSample cs;
    cs.update(1, 10);
    cs.update(2, 10);
    cs.update(1, 20);
    cs.update(2, 20);
    CHECK_EQ(cs.get_total(), 4u);
    CHECK_EQ(cs.get_lost(), 0u);
  }

  TEST_CASE("concurrent updates from multiple threads do not crash or lose count") {
    CalculateSample cs;
    static constexpr int kThreads = 4;
    static constexpr int kUpdates = 100;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&cs, t]() {
        auto guid = static_cast<uint64_t>(t + 1);

        for (int i = 0; i < kUpdates; ++i) {
          cs.update(static_cast<uint64_t>(i + 1), guid);
        }
      });
    }

    for (auto& th : threads) {
      th.join();
    }

    CHECK_EQ(cs.get_lost(), 0u);
    CHECK_EQ(cs.get_total(), static_cast<uint64_t>(kThreads * kUpdates));
  }

  TEST_CASE("concurrent reads and writes do not deadlock") {
    CalculateSample cs;

    std::thread writer([&cs]() {
      for (int i = 1; i <= 50; ++i) {
        cs.update(static_cast<uint64_t>(i), 0);
      }
    });

    std::thread reader([&cs]() {
      for (int i = 0; i < 50; ++i) {
        (void)cs.get_total();
        (void)cs.get_lost();
      }
    });

    writer.join();
    reader.join();

    CHECK(cs.get_total() <= 50u);
  }
}

// NOLINTEND
