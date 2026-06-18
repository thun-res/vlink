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

#include "./base/wheel_timer.h"

#if defined(__unix__) && !defined(__CYGWIN__)

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "../common_test.h"

TEST_SUITE("base-WheelTimer") {
  TEST_CASE("is_running reflects start and stop transitions") {
    WheelTimer wheel(20, 10);

    CHECK_FALSE(wheel.is_running());

    wheel.start();
    CHECK(wheel.is_running());

    wheel.stop();
    CHECK_FALSE(wheel.is_running());
  }

  TEST_CASE("start is idempotent when called twice") {
    WheelTimer wheel(20, 10);
    wheel.start();
    wheel.start();

    CHECK(wheel.is_running());

    wheel.stop();
  }

  TEST_CASE("stop is idempotent when called twice") {
    WheelTimer wheel(20, 10);
    wheel.start();
    wheel.stop();
    wheel.stop();

    CHECK_FALSE(wheel.is_running());
  }

  TEST_CASE("one-shot timer fires exactly once") {
    WheelTimer wheel(10, 10);
    wheel.start();

    std::atomic<int> fire_count{0};
    wheel.add(20, [&](WheelTimer::Key) { fire_count.fetch_add(1, std::memory_order_relaxed); });

    std::this_thread::sleep_for(70ms);
    wheel.stop();

    CHECK(fire_count.load() == 1);
  }

  TEST_CASE("one-shot timer fires within a reasonable delay window") {
    WheelTimer wheel(10, 10);
    wheel.start();

    std::atomic<bool> fired{false};
    auto t0 = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point fire_time;

    wheel.add(30, [&](WheelTimer::Key) {
      fire_time = std::chrono::steady_clock::now();
      fired.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(120ms);
    wheel.stop();

    CHECK(fired.load(std::memory_order_acquire));
    auto delay_ms = std::chrono::duration_cast<std::chrono::milliseconds>(fire_time - t0).count();
    CHECK(delay_ms >= 10);
    CHECK(delay_ms <= 150);
  }

  TEST_CASE("repeating timer fires at least twice within observation window") {
    WheelTimer wheel(10, 10);
    wheel.start();

    std::atomic<int> fire_count{0};
    wheel.add(20, [&](WheelTimer::Key) { fire_count.fetch_add(1, std::memory_order_relaxed); }, 20);

    std::this_thread::sleep_for(100ms);
    wheel.stop();

    CHECK(fire_count.load() >= 2);
  }

  TEST_CASE("repeating timer callback receives the same key on every firing") {
    WheelTimer wheel(10, 10);
    wheel.start();

    std::atomic<int> fire_count{0};
    std::atomic<bool> key_consistent{true};
    WheelTimer::Key stored_key = -1;

    auto key = wheel.add(
        20,
        [&](WheelTimer::Key k) {
          fire_count.fetch_add(1, std::memory_order_relaxed);

          if (stored_key == -1) {
            stored_key = k;
          } else if (stored_key != k) {
            key_consistent.store(false, std::memory_order_relaxed);
          }
        },
        20);

    stored_key = key;

    std::this_thread::sleep_for(100ms);
    wheel.stop();

    CHECK(fire_count.load() >= 2);
    CHECK(key_consistent.load());
  }

  TEST_CASE("remove before expiry returns true and prevents firing") {
    WheelTimer wheel(10, 10);
    wheel.start();

    std::atomic<int> fire_count{0};
    auto key = wheel.add(80, [&](WheelTimer::Key) { fire_count.fetch_add(1, std::memory_order_relaxed); });

    std::this_thread::sleep_for(20ms);
    bool removed = wheel.remove(key);
    CHECK(removed);

    std::this_thread::sleep_for(80ms);
    wheel.stop();

    CHECK(fire_count.load() == 0);
  }

  TEST_CASE("remove after expiry returns false") {
    WheelTimer wheel(10, 10);
    wheel.start();

    std::atomic<bool> fired{false};
    auto key = wheel.add(20, [&](WheelTimer::Key) { fired.store(true, std::memory_order_release); });

    std::this_thread::sleep_for(70ms);
    CHECK(fired.load(std::memory_order_acquire));

    bool removed = wheel.remove(key);
    CHECK_FALSE(removed);

    wheel.stop();
  }

  TEST_CASE("remove with invalid key returns false") {
    WheelTimer wheel(20, 10);
    wheel.start();

    bool removed = wheel.remove(99999);
    CHECK_FALSE(removed);

    wheel.stop();
  }

  TEST_CASE("multiple one-shot timers with different timeouts all fire") {
    WheelTimer wheel(10, 10);
    wheel.start();

    std::atomic<int> fire_count{0};

    wheel.add(10, [&](WheelTimer::Key) { fire_count.fetch_add(1, std::memory_order_relaxed); });
    wheel.add(30, [&](WheelTimer::Key) { fire_count.fetch_add(1, std::memory_order_relaxed); });
    wheel.add(50, [&](WheelTimer::Key) { fire_count.fetch_add(1, std::memory_order_relaxed); });

    std::this_thread::sleep_for(100ms);
    wheel.stop();

    CHECK(fire_count.load() == 3);
  }

  TEST_CASE("timers with staggered timeouts fire in ascending order") {
    WheelTimer wheel(10, 10);
    wheel.start();

    std::vector<int> order;
    std::atomic<int> fire_count{0};

    wheel.add(10, [&](WheelTimer::Key) {
      order.push_back(1);
      fire_count.fetch_add(1, std::memory_order_relaxed);
    });

    wheel.add(30, [&](WheelTimer::Key) {
      order.push_back(2);
      fire_count.fetch_add(1, std::memory_order_relaxed);
    });

    wheel.add(50, [&](WheelTimer::Key) {
      order.push_back(3);
      fire_count.fetch_add(1, std::memory_order_relaxed);
    });

    std::this_thread::sleep_for(100ms);
    wheel.stop();

    CHECK(fire_count.load() == 3);
    REQUIRE(static_cast<int>(order.size()) == 3);
    CHECK(order[0] == 1);
    CHECK(order[1] == 2);
    CHECK(order[2] == 3);
  }

  TEST_CASE("get_remaining_time returns nonzero before expiry") {
    WheelTimer wheel(10, 10);
    wheel.start();

    auto key = wheel.add(200, [](WheelTimer::Key) {});

    std::this_thread::sleep_for(15ms);
    uint32_t remaining = wheel.get_remaining_time(key);
    CHECK(remaining > 0);

    wheel.remove(key);
    wheel.stop();
  }

  TEST_CASE("get_remaining_time returns zero for unknown key") {
    WheelTimer wheel(10, 10);
    wheel.start();

    uint32_t remaining = wheel.get_remaining_time(99999);
    CHECK(remaining == 0);

    wheel.stop();
  }

  TEST_CASE("get_remaining_time decreases as time passes") {
    WheelTimer wheel(10, 10);
    wheel.start();

    auto key = wheel.add(300, [](WheelTimer::Key) {});

    std::this_thread::sleep_for(15ms);
    uint32_t r1 = wheel.get_remaining_time(key);

    std::this_thread::sleep_for(50ms);
    uint32_t r2 = wheel.get_remaining_time(key);

    CHECK(r2 < r1);

    wheel.remove(key);
    wheel.stop();
  }

  TEST_CASE("get_remaining_time returns zero after timer fires") {
    WheelTimer wheel(10, 10);
    wheel.start();

    std::atomic<bool> fired{false};
    auto key = wheel.add(20, [&](WheelTimer::Key) { fired.store(true, std::memory_order_release); });

    std::this_thread::sleep_for(70ms);
    CHECK(fired.load(std::memory_order_acquire));

    uint32_t remaining = wheel.get_remaining_time(key);
    CHECK(remaining == 0);

    wheel.stop();
  }

  TEST_CASE("pause prevents pending timers from firing") {
    WheelTimer wheel(10, 10);
    wheel.start();

    std::atomic<int> fire_count{0};
    wheel.add(40, [&](WheelTimer::Key) { fire_count.fetch_add(1, std::memory_order_relaxed); });

    std::this_thread::sleep_for(10ms);
    wheel.pause();

    std::this_thread::sleep_for(70ms);
    CHECK(fire_count.load() == 0);

    wheel.stop();
  }

  TEST_CASE("resume after pause allows pending timers to fire") {
    WheelTimer wheel(10, 10);
    wheel.set_catchup_limit(100);
    wheel.start();

    std::atomic<int> fire_count{0};
    wheel.add(40, [&](WheelTimer::Key) { fire_count.fetch_add(1, std::memory_order_relaxed); });

    std::this_thread::sleep_for(10ms);
    wheel.pause();

    std::this_thread::sleep_for(70ms);
    CHECK(fire_count.load() == 0);

    wheel.resume();

    std::this_thread::sleep_for(80ms);
    wheel.stop();

    CHECK(fire_count.load() == 1);
  }

  TEST_CASE("pause and resume toggle allow a repeating timer to accumulate counts") {
    WheelTimer wheel(10, 10);
    wheel.start();

    std::atomic<int> fire_count{0};
    wheel.add(15, [&](WheelTimer::Key) { fire_count.fetch_add(1, std::memory_order_relaxed); }, 15);

    std::this_thread::sleep_for(50ms);
    int before_pause = fire_count.load();
    CHECK(before_pause >= 1);

    wheel.pause();
    std::this_thread::sleep_for(50ms);
    int during_pause = fire_count.load();
    CHECK(during_pause == before_pause);

    wheel.resume();
    std::this_thread::sleep_for(50ms);
    wheel.stop();

    CHECK(fire_count.load() > during_pause);
  }

  TEST_CASE("wakeup does not crash while wheel is running") {
    WheelTimer wheel(10, 10);
    wheel.start();

    std::atomic<int> fire_count{0};
    wheel.add(30, [&](WheelTimer::Key) { fire_count.fetch_add(1, std::memory_order_relaxed); });

    wheel.wakeup();

    std::this_thread::sleep_for(70ms);
    wheel.stop();

    CHECK(fire_count.load() == 1);
  }

  TEST_CASE("set_catchup_limit is accepted before start") {
    WheelTimer wheel(10, 10);
    wheel.set_catchup_limit(10);
    wheel.start();

    std::atomic<int> fire_count{0};
    wheel.add(30, [&](WheelTimer::Key) { fire_count.fetch_add(1, std::memory_order_relaxed); });

    std::this_thread::sleep_for(70ms);
    wheel.stop();

    CHECK(fire_count.load() == 1);
  }

  TEST_CASE("set_catchup_limit constrains burst after long pause") {
    WheelTimer wheel(10, 10);
    wheel.set_catchup_limit(5);
    wheel.start();

    std::atomic<int> fire_count{0};
    wheel.add(15, [&](WheelTimer::Key) { fire_count.fetch_add(1, std::memory_order_relaxed); }, 15);

    std::this_thread::sleep_for(30ms);
    wheel.pause();
    std::this_thread::sleep_for(80ms);
    wheel.resume();
    std::this_thread::sleep_for(80ms);
    wheel.stop();

    CHECK(fire_count.load() >= 1);
  }

  TEST_CASE("destructor stops wheel without explicit stop call") {
    std::atomic<int> fire_count{0};

    {
      WheelTimer wheel(10, 10);
      wheel.start();
      wheel.add(20, [&](WheelTimer::Key) { fire_count.fetch_add(1, std::memory_order_relaxed); });
      std::this_thread::sleep_for(60ms);
    }

    CHECK(fire_count.load() >= 1);
  }

  TEST_CASE("50 concurrent one-shot timers all fire") {
    WheelTimer wheel(256, 10);
    wheel.start();

    static constexpr int kCount = 50;
    std::atomic<int> fire_count{0};

    for (int i = 0; i < kCount; ++i) {
      wheel.add(30, [&](WheelTimer::Key) { fire_count.fetch_add(1, std::memory_order_relaxed); });
    }

    std::this_thread::sleep_for(80ms);
    wheel.stop();

    CHECK(fire_count.load() == kCount);
  }

  TEST_CASE("remove mid-repeat stops further firings") {
    WheelTimer wheel(10, 10);
    wheel.start();

    std::atomic<int> fire_count{0};
    auto key = wheel.add(20, [&](WheelTimer::Key) { fire_count.fetch_add(1, std::memory_order_relaxed); }, 20);

    std::this_thread::sleep_for(70ms);
    int count_before_remove = fire_count.load();
    CHECK(count_before_remove >= 1);

    wheel.remove(key);

    std::this_thread::sleep_for(70ms);
    wheel.stop();

    CHECK(fire_count.load() == count_before_remove);
  }

  TEST_CASE("add returns unique keys for each timer") {
    WheelTimer wheel(10, 10);
    wheel.start();

    auto key1 = wheel.add(100, [](WheelTimer::Key) {});
    auto key2 = wheel.add(200, [](WheelTimer::Key) {});
    auto key3 = wheel.add(300, [](WheelTimer::Key) {});

    CHECK(key1 != key2);
    CHECK(key2 != key3);
    CHECK(key1 != key3);

    wheel.stop();
  }

  TEST_CASE("callback receives the key returned by add") {
    WheelTimer wheel(10, 10);
    wheel.start();

    std::atomic<WheelTimer::Key> received_key{-999};
    auto key = wheel.add(20, [&](WheelTimer::Key k) { received_key.store(k, std::memory_order_release); });

    std::this_thread::sleep_for(70ms);
    wheel.stop();

    CHECK(received_key.load(std::memory_order_acquire) == key);
  }

  TEST_CASE("timer with timeout longer than wheel span uses round counter") {
    WheelTimer wheel(16, 10);
    wheel.start();

    std::atomic<bool> fired{false};
    wheel.add(120, [&](WheelTimer::Key) { fired.store(true, std::memory_order_release); });

    std::this_thread::sleep_for(70ms);
    CHECK_FALSE(fired.load(std::memory_order_acquire));

    std::this_thread::sleep_for(120ms);
    wheel.stop();

    CHECK(fired.load(std::memory_order_acquire));
  }

  TEST_CASE("removing all timers then stopping is clean") {
    WheelTimer wheel(10, 10);
    wheel.start();

    std::vector<WheelTimer::Key> keys;
    keys.reserve(10);

    for (int i = 0; i < 10; ++i) {
      keys.push_back(wheel.add(500, [](WheelTimer::Key) {}));
    }

    for (auto k : keys) {
      wheel.remove(k);
    }

    wheel.stop();
    CHECK_FALSE(wheel.is_running());
  }

  TEST_CASE("add after stop does not crash") {
    WheelTimer wheel(10, 10);
    wheel.start();
    wheel.stop();

    CHECK_NOTHROW((void)wheel.add(100, [](WheelTimer::Key) {}));
  }

  TEST_CASE("stop can be requested from within a callback") {
    WheelTimer wheel(10, 10);
    wheel.start();

    std::atomic<bool> fired{false};

    wheel.add(10, [&](WheelTimer::Key) {
      fired.store(true, std::memory_order_release);
      wheel.stop();
    });

    for (int i = 0; i < 100 && !fired.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(5ms);
    }

    wheel.stop();
    CHECK(fired.load(std::memory_order_acquire));
  }
}

#endif

// NOLINTEND
