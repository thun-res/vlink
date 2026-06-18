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

#include "./base/object_pool.h"

#include <doctest/doctest.h>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "../common_test.h"

namespace {

struct Widget {
  int value{0};
  bool reset_called{false};
};

}  // namespace

TEST_SUITE("base-ObjectPool") {
  TEST_CASE("default construction yields empty pool") {
    auto pool = std::make_shared<ObjectPool<Widget>>();
    CHECK_EQ(pool->size(), 0u);
    CHECK_EQ(pool->borrowed(), 0u);
    CHECK_EQ(pool->total_created(), 0u);
    CHECK_EQ(pool->max_size(), 0u);
  }

  TEST_CASE("pre-fill with initial size populates pool") {
    auto pool = std::make_shared<ObjectPool<Widget>>([] { return std::make_unique<Widget>(); }, 4u);
    CHECK_EQ(pool->size(), 4u);
    CHECK_EQ(pool->borrowed(), 0u);
    CHECK_EQ(pool->total_created(), 4u);
  }

  TEST_CASE("max_size is recorded correctly") {
    auto pool = std::make_shared<ObjectPool<Widget>>([] { return std::make_unique<Widget>(); }, 2u, 8u);
    CHECK_EQ(pool->max_size(), 8u);
  }

  TEST_CASE("initial_size greater than max_size throws invalid_argument") {
    CHECK_THROWS_AS(((void)std::make_shared<ObjectPool<Widget>>([] { return std::make_unique<Widget>(); }, 5u, 3u)),
                    std::invalid_argument);
  }

  TEST_CASE("factory returning nullptr throws runtime_error on pre-fill") {
    CHECK_THROWS_AS(((void)std::make_shared<ObjectPool<Widget>>([] { return std::unique_ptr<Widget>{}; }, 1u)),
                    std::runtime_error);
  }

  TEST_CASE("get returns non-null pointer") {
    auto pool = std::make_shared<ObjectPool<Widget>>();
    auto obj = pool->get();
    CHECK(obj.get() != nullptr);
  }

  TEST_CASE("get increments borrowed count") {
    auto pool = std::make_shared<ObjectPool<Widget>>();
    auto obj = pool->get();
    CHECK_EQ(pool->borrowed(), 1u);
  }

  TEST_CASE("unique_ptr destruction returns object to pool") {
    auto pool = std::make_shared<ObjectPool<Widget>>();
    {
      auto obj = pool->get();
      CHECK_EQ(pool->borrowed(), 1u);
      CHECK_EQ(pool->size(), 0u);
    }
    CHECK_EQ(pool->borrowed(), 0u);
    CHECK_EQ(pool->size(), 1u);
  }

  TEST_CASE("multiple get calls each increment borrowed") {
    auto pool = std::make_shared<ObjectPool<Widget>>();
    auto a = pool->get();
    auto b = pool->get();
    auto c = pool->get();
    CHECK_EQ(pool->borrowed(), 3u);
    CHECK_EQ(pool->total_created(), 3u);
  }

  TEST_CASE("pool grows without bound when max_size is zero") {
    auto pool = std::make_shared<ObjectPool<Widget>>();
    std::vector<decltype(pool->get())> objs;
    for (int i = 0; i < 50; ++i) {
      objs.emplace_back(pool->get());
    }
    CHECK_EQ(pool->borrowed(), 50u);
    CHECK_EQ(pool->total_created(), 50u);
  }

  TEST_CASE("exhausted pool throws runtime_error") {
    auto pool = std::make_shared<ObjectPool<Widget>>([] { return std::make_unique<Widget>(); }, 0u, 2u);
    auto a = pool->get();
    auto b = pool->get();
    CHECK_THROWS_AS((void)pool->get(), std::runtime_error);
  }

  TEST_CASE("releasing one object allows another get from exhausted pool") {
    auto pool = std::make_shared<ObjectPool<Widget>>([] { return std::make_unique<Widget>(); }, 0u, 1u);
    {
      auto obj = pool->get();
      CHECK_EQ(pool->borrowed(), 1u);
      CHECK_THROWS_AS((void)pool->get(), std::runtime_error);
    }
    auto obj2 = pool->get();
    CHECK(obj2.get() != nullptr);
  }

  TEST_CASE("returned object is reused on next get") {
    auto pool = std::make_shared<ObjectPool<Widget>>();
    Widget* first_ptr = nullptr;
    {
      auto obj = pool->get();
      first_ptr = obj.get();
      obj->value = 99;
    }
    auto obj2 = pool->get();
    CHECK_EQ(obj2.get(), first_ptr);
    CHECK_EQ(obj2->value, 99);
    CHECK_EQ(pool->total_created(), 1u);
  }

  TEST_CASE("repeated acquire-release cycles do not grow total_created") {
    auto pool = std::make_shared<ObjectPool<Widget>>();
    for (int i = 0; i < 10; ++i) {
      auto obj = pool->get();
      (void)obj;
    }
    CHECK_EQ(pool->total_created(), 1u);
  }

  TEST_CASE("get_shared returns non-null shared_ptr") {
    auto pool = std::make_shared<ObjectPool<Widget>>();
    auto obj = pool->get_shared();
    CHECK(obj.get() != nullptr);
  }

  TEST_CASE("get_shared increments borrowed") {
    auto pool = std::make_shared<ObjectPool<Widget>>();
    auto obj = pool->get_shared();
    CHECK_EQ(pool->borrowed(), 1u);
  }

  TEST_CASE("object returned to pool when last shared_ptr reference dropped") {
    auto pool = std::make_shared<ObjectPool<Widget>>();
    {
      auto obj = pool->get_shared();
      auto obj2 = obj;
      CHECK_EQ(pool->borrowed(), 1u);
    }
    CHECK_EQ(pool->borrowed(), 0u);
    CHECK_EQ(pool->size(), 1u);
  }

  TEST_CASE("shared_ptr copies keep object alive and return once") {
    auto pool = std::make_shared<ObjectPool<Widget>>();
    {
      auto obj1 = pool->get_shared();
      auto obj2 = obj1;
      auto obj3 = obj2;
      CHECK_EQ(pool->borrowed(), 1u);
      obj1.reset();
      CHECK_EQ(pool->borrowed(), 1u);
      obj2.reset();
      CHECK_EQ(pool->borrowed(), 1u);
    }
    CHECK_EQ(pool->borrowed(), 0u);
    CHECK_EQ(pool->size(), 1u);
  }

  TEST_CASE("borrow returns non-null raw pointer") {
    auto pool = std::make_shared<ObjectPool<Widget>>();
    Widget* raw = pool->borrow();
    REQUIRE(raw != nullptr);
    pool->give_back(raw);
  }

  TEST_CASE("give_back returns object to pool") {
    auto pool = std::make_shared<ObjectPool<Widget>>();
    Widget* raw = pool->borrow();
    CHECK_EQ(pool->borrowed(), 1u);
    pool->give_back(raw);
    CHECK_EQ(pool->borrowed(), 0u);
    CHECK_EQ(pool->size(), 1u);
  }

  TEST_CASE("give_back with nullptr is a no-op") {
    auto pool = std::make_shared<ObjectPool<Widget>>();
    pool->give_back(nullptr);
    CHECK_EQ(pool->size(), 0u);
    CHECK_EQ(pool->borrowed(), 0u);
  }

  TEST_CASE("kPolicyNone reset callback not called on acquire or release") {
    int reset_count = 0;
    auto pool = std::make_shared<ObjectPool<Widget>>([] { return std::make_unique<Widget>(); }, 0u, 0u,
                                                     [&reset_count](Widget&) { ++reset_count; },
                                                     ObjectPool<Widget>::kPolicyNone);
    {
      auto obj = pool->get();
      (void)obj;
    }
    CHECK_EQ(reset_count, 0);
  }

  TEST_CASE("kPolicyRelease reset callback called on return") {
    int reset_count = 0;
    auto pool = std::make_shared<ObjectPool<Widget>>([] { return std::make_unique<Widget>(); }, 0u, 0u,
                                                     [&reset_count](Widget& w) {
                                                       ++reset_count;
                                                       w.reset_called = true;
                                                     },
                                                     ObjectPool<Widget>::kPolicyRelease);
    {
      auto obj = pool->get();
      (void)obj;
    }
    CHECK_EQ(reset_count, 1);
  }

  TEST_CASE("kPolicyRelease reset called on initial pre-fill objects") {
    int reset_count = 0;
    auto pool = std::make_shared<ObjectPool<Widget>>([] { return std::make_unique<Widget>(); }, 3u, 0u,
                                                     [&reset_count](Widget&) { ++reset_count; },
                                                     ObjectPool<Widget>::kPolicyRelease);
    CHECK_EQ(reset_count, 3);
  }

  TEST_CASE("kPolicyAcquire reset callback called on acquire not on release") {
    int reset_count = 0;
    auto pool = std::make_shared<ObjectPool<Widget>>([] { return std::make_unique<Widget>(); }, 0u, 0u,
                                                     [&reset_count](Widget&) { ++reset_count; },
                                                     ObjectPool<Widget>::kPolicyAcquire);
    {
      auto obj = pool->get();
      CHECK_EQ(reset_count, 1);
    }
    CHECK_EQ(reset_count, 1);
  }

  TEST_CASE("kPolicyBoth reset called on both acquire and release") {
    int reset_count = 0;
    auto pool = std::make_shared<ObjectPool<Widget>>([] { return std::make_unique<Widget>(); }, 0u, 0u,
                                                     [&reset_count](Widget&) { ++reset_count; },
                                                     ObjectPool<Widget>::kPolicyBoth);
    {
      auto obj = pool->get();
      CHECK_EQ(reset_count, 1);
    }
    CHECK_EQ(reset_count, 2);
  }

  TEST_CASE("pre-fill with kPolicyAcquire does not call reset on construction") {
    int reset_count = 0;
    auto pool = std::make_shared<ObjectPool<Widget>>([] { return std::make_unique<Widget>(); }, 3u, 0u,
                                                     [&reset_count](Widget&) { ++reset_count; },
                                                     ObjectPool<Widget>::kPolicyAcquire);
    CHECK_EQ(reset_count, 0);
    CHECK_EQ(pool->size(), 3u);
  }

  TEST_CASE("pre-fill with kPolicyBoth calls reset on pre-fill objects") {
    int reset_count = 0;
    auto pool = std::make_shared<ObjectPool<Widget>>([] { return std::make_unique<Widget>(); }, 2u, 0u,
                                                     [&reset_count](Widget&) { ++reset_count; },
                                                     ObjectPool<Widget>::kPolicyBoth);
    CHECK_EQ(reset_count, 2);
  }

  TEST_CASE("stats reflect correct counters after construction") {
    auto pool = std::make_shared<ObjectPool<Widget>>([] { return std::make_unique<Widget>(); }, 2u, 10u);
    auto s = pool->stats();
    CHECK_EQ(s.pool_size, 2u);
    CHECK_EQ(s.borrowed, 0u);
    CHECK_EQ(s.total_created, 2u);
    CHECK_EQ(s.max_size, 10u);
  }

  TEST_CASE("stats update correctly after get and release") {
    auto pool = std::make_shared<ObjectPool<Widget>>();
    {
      auto obj = pool->get();
      auto s = pool->stats();
      CHECK_EQ(s.borrowed, 1u);
      CHECK_EQ(s.pool_size, 0u);
    }
    auto s = pool->stats();
    CHECK_EQ(s.borrowed, 0u);
    CHECK_EQ(s.pool_size, 1u);
    CHECK_EQ(s.total_created, 1u);
  }

  TEST_CASE("stats after mixed get borrow and get_shared operations") {
    auto pool = std::make_shared<ObjectPool<Widget>>([] { return std::make_unique<Widget>(); }, 2u, 10u);
    auto obj1 = pool->get();
    Widget* raw = pool->borrow();
    auto obj2 = pool->get_shared();

    auto s = pool->stats();
    CHECK_EQ(s.borrowed, 3u);
    CHECK(s.total_created >= 3u);

    pool->give_back(raw);
    obj1.reset();
    obj2.reset();

    s = pool->stats();
    CHECK_EQ(s.borrowed, 0u);
    CHECK_EQ(s.pool_size, 3u);
  }

  TEST_CASE("max_size zero means unlimited") {
    auto pool = std::make_shared<ObjectPool<Widget>>([] { return std::make_unique<Widget>(); }, 0u, 0u);
    CHECK_EQ(pool->max_size(), 0u);
    std::vector<decltype(pool->get())> objs;
    for (int i = 0; i < 100; ++i) {
      objs.emplace_back(pool->get());
    }
    CHECK_EQ(pool->borrowed(), 100u);
  }

  TEST_CASE("factory returning nullptr on demand throws runtime_error") {
    auto pool = std::make_shared<ObjectPool<Widget>>([] { return std::unique_ptr<Widget>{}; }, 0u, 0u);
    CHECK_THROWS_AS((void)pool->get(), std::runtime_error);
  }

  TEST_CASE("factory returning nullptr on borrow throws runtime_error") {
    auto pool = std::make_shared<ObjectPool<Widget>>([] { return std::unique_ptr<Widget>{}; }, 0u, 0u);
    CHECK_THROWS_AS((void)pool->borrow(), std::runtime_error);
  }

  TEST_CASE("factory returning nullptr on get_shared throws runtime_error") {
    auto pool = std::make_shared<ObjectPool<Widget>>([] { return std::unique_ptr<Widget>{}; }, 0u, 0u);
    CHECK_THROWS_AS((void)pool->get_shared(), std::runtime_error);
  }

  TEST_CASE("exhausted pool on borrow throws runtime_error") {
    auto pool = std::make_shared<ObjectPool<Widget>>([] { return std::make_unique<Widget>(); }, 0u, 1u);
    Widget* raw = pool->borrow();
    CHECK_THROWS_AS((void)pool->borrow(), std::runtime_error);
    pool->give_back(raw);
  }

  TEST_CASE("exhausted pool on get_shared throws runtime_error") {
    auto pool = std::make_shared<ObjectPool<Widget>>([] { return std::make_unique<Widget>(); }, 0u, 1u);
    auto obj = pool->get_shared();
    CHECK_THROWS_AS((void)pool->get_shared(), std::runtime_error);
  }

  TEST_CASE("reset failure on release discards object without exhausting pool") {
    bool throw_on_release = true;
    auto pool = std::make_shared<ObjectPool<Widget>>([] { return std::make_unique<Widget>(); }, 0u, 1u,
                                                     [&throw_on_release](Widget&) {
                                                       if (throw_on_release) {
                                                         throw std::runtime_error("reset failed");
                                                       }
                                                     },
                                                     ObjectPool<Widget>::kPolicyRelease);
    {
      auto obj = pool->get();
      REQUIRE(obj.get() != nullptr);
    }
    CHECK_EQ(pool->borrowed(), 0u);
    CHECK_EQ(pool->size(), 0u);

    throw_on_release = false;
    auto obj2 = pool->get();
    CHECK(obj2.get() != nullptr);
  }

  TEST_CASE("unique_ptr deletes object safely when pool is destroyed first") {
    std::unique_ptr<Widget, ObjectPool<Widget>::PoolDeleter> handle;
    {
      auto pool = std::make_shared<ObjectPool<Widget>>();
      handle = pool->get();
    }
    handle.reset();
  }

  TEST_CASE("concurrent get and release does not corrupt state") {
    auto pool = std::make_shared<ObjectPool<Widget>>([] { return std::make_unique<Widget>(); }, 0u, 0u);
    static constexpr int kThreads = 4;
    static constexpr int kIter = 100;
    std::atomic<int> errors{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&]() {
        for (int i = 0; i < kIter; ++i) {
          try {
            auto obj = pool->get();
            obj->value = i;
          } catch (std::exception&) {
            ++errors;
          }
        }
      });
    }
    for (auto& th : threads) {
      th.join();
    }
    CHECK_EQ(errors.load(), 0);
    CHECK_EQ(pool->borrowed(), 0u);
  }

  TEST_CASE("concurrent borrow and give_back does not corrupt state") {
    auto pool = std::make_shared<ObjectPool<Widget>>([] { return std::make_unique<Widget>(); }, 0u, 0u);
    static constexpr int kThreads = 4;
    static constexpr int kIter = 50;
    std::atomic<int> errors{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&]() {
        for (int i = 0; i < kIter; ++i) {
          try {
            Widget* raw = pool->borrow();
            raw->value = i;
            pool->give_back(raw);
          } catch (std::exception&) {
            errors.fetch_add(1);
          }
        }
      });
    }
    for (auto& th : threads) {
      th.join();
    }
    CHECK_EQ(errors.load(), 0);
    CHECK_EQ(pool->borrowed(), 0u);
  }

  TEST_CASE("concurrent get_shared does not corrupt state") {
    auto pool = std::make_shared<ObjectPool<Widget>>([] { return std::make_unique<Widget>(); }, 0u, 0u);
    static constexpr int kThreads = 4;
    static constexpr int kIter = 50;
    std::atomic<int> errors{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&]() {
        for (int i = 0; i < kIter; ++i) {
          try {
            auto obj = pool->get_shared();
            obj->value = i;
          } catch (std::exception&) {
            errors.fetch_add(1);
          }
        }
      });
    }
    for (auto& th : threads) {
      th.join();
    }
    CHECK_EQ(errors.load(), 0);
    CHECK_EQ(pool->borrowed(), 0u);
  }

  TEST_CASE("concurrent access with max_size respects total limit") {
    auto pool = std::make_shared<ObjectPool<Widget>>([] { return std::make_unique<Widget>(); }, 4u, 8u);
    static constexpr int kThreads = 4;
    static constexpr int kIter = 20;
    std::atomic<int> exhaustion_errors{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&]() {
        for (int i = 0; i < kIter; ++i) {
          try {
            auto obj = pool->get();
            obj->value = i;
            std::this_thread::yield();
          } catch (const std::runtime_error&) {
            exhaustion_errors.fetch_add(1);
          }
        }
      });
    }
    for (auto& th : threads) {
      th.join();
    }
    CHECK_EQ(pool->borrowed(), 0u);
    CHECK(pool->total_created() <= 8u);
  }
}

// NOLINTEND
