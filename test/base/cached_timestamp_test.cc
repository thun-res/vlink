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

#include "./base/cached_timestamp.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "../common_test.h"

TEST_SUITE("base-CachedTimestamp") {
  TEST_CASE("get returns non-empty string_view") {
    CachedTimestamp ts;
    std::string_view sv = ts.get();

    CHECK_FALSE(sv.empty());
  }

  TEST_CASE("default format produces 18-character timestamp") {
    CachedTimestamp ts;
    std::string_view sv = ts.get();

    CHECK_EQ(sv.size(), 18u);
  }

  TEST_CASE("default format has expected separators at fixed positions") {
    CachedTimestamp ts;
    std::string_view sv = ts.get();

    REQUIRE_EQ(sv.size(), 18u);
    CHECK_EQ(sv[2], '-');
    CHECK_EQ(sv[5], ' ');
    CHECK_EQ(sv[8], ':');
    CHECK_EQ(sv[11], ':');
    CHECK_EQ(sv[14], '.');
  }

  TEST_CASE("all digit positions contain ASCII numerals") {
    CachedTimestamp ts;
    std::string_view sv = ts.get();
    REQUIRE_EQ(sv.size(), 18u);

    static const size_t kDigitPos[] = {0, 1, 3, 4, 6, 7, 9, 10, 12, 13, 15, 16, 17};
    for (size_t pos : kDigitPos) {
      CHECK(sv[pos] >= '0');
      CHECK(sv[pos] <= '9');
    }
  }

  TEST_CASE("millisecond field is within [000, 999]") {
    CachedTimestamp ts;
    std::string_view sv = ts.get();
    REQUIRE_EQ(sv.size(), 18u);

    int ms = (sv[15] - '0') * 100 + (sv[16] - '0') * 10 + (sv[17] - '0');
    CHECK(ms >= 0);
    CHECK(ms <= 999);
  }

  TEST_CASE("repeated calls return consistent format") {
    CachedTimestamp ts;
    for (int i = 0; i < 10; ++i) {
      std::string_view sv = ts.get();
      CHECK_EQ(sv.size(), 18u);
      CHECK_EQ(sv[2], '-');
      CHECK_EQ(sv[14], '.');
    }
  }

  TEST_CASE("get with use_utc=true returns same-length string") {
    CachedTimestamp ts;
    std::string_view sv = ts.get("%02d-%02d %02d:%02d:%02d.%03d", true);

    CHECK_EQ(sv.size(), 18u);
  }

  TEST_CASE("rapid successive calls do not crash") {
    CachedTimestamp ts;
    for (int i = 0; i < 1000; ++i) {
      std::string_view sv = ts.get();
      CHECK_FALSE(sv.empty());
    }
  }

  TEST_CASE("value copied to string remains valid across calls") {
    CachedTimestamp ts;
    std::string copy1 = std::string(ts.get());
    std::string copy2 = std::string(ts.get());

    CHECK_EQ(copy1.size(), 18u);
    CHECK_EQ(copy2.size(), 18u);
  }

  TEST_CASE("multiple instances are independent") {
    CachedTimestamp ts1;
    CachedTimestamp ts2;

    CHECK_EQ(ts1.get().size(), 18u);
    CHECK_EQ(ts2.get().size(), 18u);
  }

  TEST_CASE("concurrent access from multiple threads does not crash") {
    CachedTimestamp ts;
    static constexpr int kThreads = 4;
    static constexpr int kIter = 200;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&ts]() {
        for (int i = 0; i < kIter; ++i) {
          std::string copy = std::string(ts.get());
          CHECK_EQ(copy.size(), 18u);
        }
      });
    }

    for (auto& thr : threads) {
      thr.join();
    }
  }

  TEST_CASE("get produces well-formed timestamp after crossing a second boundary") {
    CachedTimestamp ts;
    std::string first = std::string(ts.get());

    std::this_thread::sleep_for(1050ms);

    std::string second = std::string(ts.get());

    CHECK_EQ(first.size(), 18u);
    CHECK_EQ(second.size(), 18u);
    CHECK_EQ(second[2], '-');
    CHECK_EQ(second[5], ' ');
    CHECK_EQ(second[14], '.');
  }
}

TEST_SUITE("base-CachedTimestamp") {
  TEST_CASE("get_at handles epoch rollback and custom formats") {
    using Clock = std::chrono::system_clock;
    CachedTimestamp timestamp;
    const auto at = [](int64_t ms) { return Clock::time_point(std::chrono::milliseconds(ms)); };
    constexpr auto kFormat = "%02d-%02d %02d:%02d:%02d.%03d";
    CachedTimestamp local_cold, local_warm;
    (void)local_warm.get_at(at(1500));
    CHECK(local_cold.get_at(at(500)) == local_warm.get_at(at(500)));
    CHECK(timestamp.get_at(at(500), kFormat, true) == "01-01 00:00:00.500");
    CHECK(timestamp.get_at(at(400), kFormat, true) == "01-01 00:00:00.400");
#if !defined(_WIN32)
    CHECK(timestamp.get_at(at(-500), kFormat, true) == "12-31 23:59:59.500");
    CHECK(timestamp.get_at(at(-400), kFormat, true) == "12-31 23:59:59.600");
    CHECK(timestamp.get_at(at(-1), kFormat, true) == "12-31 23:59:59.999");
#endif
#if defined(__linux__)
    if (sizeof(std::time_t) == sizeof(int64_t)) {
      CHECK_FALSE(timestamp.get_at(Clock::time_point::min(), kFormat, true).empty());
    }
#endif
    CHECK(timestamp.get_at(at(3600500), kFormat, true) == "01-01 01:00:00.500");
    CHECK(timestamp.get_at(at(500), kFormat, true) == "01-01 00:00:00.500");
    CHECK(timestamp.get_at(at(500), "%d/%d %d:%d:%d ms=%d!", true) == "1/1 0:0:0 ms=500!");
    CHECK(timestamp.get_at(at(600), "%d/%d %d:%d:%d ms=%d!", true) == "1/1 0:0:0 ms=600!");
    CHECK(timestamp.get_at(at(700), kFormat, true) == "01-01 00:00:00.700");
    CHECK(timestamp.get_at(at(800), "%d", true) == "1");
    CHECK(timestamp.get_at(at(900), kFormat, true) == "01-01 00:00:00.900");
    CHECK(timestamp.get_at(at(900), "%0100d", true).empty());
    CHECK(timestamp.get_at(at(950), kFormat, true) == "01-01 00:00:00.950");
  }
}

// NOLINTEND
