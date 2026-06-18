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

#include "./base/cancellation.h"

#include <doctest/doctest.h>

#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../common_test.h"

TEST_SUITE("base-Cancellation") {
  TEST_CASE("default constructed token is invalid and inert") {
    CancellationToken token;

    CHECK_FALSE(token.valid());
    CHECK_FALSE(token.is_cancellation_requested());
    CHECK_NOTHROW(token.throw_if_cancellation_requested());

    auto reg = token.register_callback([] {});
    CHECK_FALSE(reg.valid());
  }

  TEST_CASE("default constructed registration is invalid and safe to reset") {
    CancellationRegistration reg;

    CHECK_FALSE(reg.valid());
    CHECK_NOTHROW(reg.reset());
    CHECK_FALSE(reg.valid());
  }

  TEST_CASE("fresh source starts uncancelled and token is valid") {
    CancellationSource source;
    auto token = source.token();

    CHECK(token.valid());
    CHECK_FALSE(token.is_cancellation_requested());
    CHECK_FALSE(source.is_cancellation_requested());
  }

  TEST_CASE("request_cancel transitions source once and fires callbacks") {
    CancellationSource source;
    auto token = source.token();

    std::atomic<int> count{0};
    auto reg = token.register_callback([&count] { count.fetch_add(1, std::memory_order_relaxed); });
    CHECK(reg.valid());

    CHECK(source.request_cancel());
    CHECK_FALSE(source.request_cancel());
    CHECK(token.is_cancellation_requested());
    CHECK(source.is_cancellation_requested());
    CHECK_EQ(count.load(std::memory_order_relaxed), 1);
    CHECK_FALSE(reg.valid());
  }

  TEST_CASE("registration reset prevents callback from firing") {
    CancellationSource source;
    auto token = source.token();

    std::atomic<int> count{0};
    auto reg = token.register_callback([&count] { count.fetch_add(1, std::memory_order_relaxed); });
    reg.reset();

    CHECK_FALSE(reg.valid());
    CHECK(source.request_cancel());
    CHECK_EQ(count.load(std::memory_order_relaxed), 0);
  }

  TEST_CASE("registration reset is idempotent") {
    CancellationSource source;
    auto token = source.token();

    std::atomic<int> count{0};
    auto reg = token.register_callback([&count] { count.fetch_add(1, std::memory_order_relaxed); });
    reg.reset();
    CHECK_NOTHROW(reg.reset());
    CHECK_NOTHROW(reg.reset());
    CHECK_FALSE(reg.valid());

    CHECK(source.request_cancel());
    CHECK_EQ(count.load(std::memory_order_relaxed), 0);
  }

  TEST_CASE("register_callback after cancellation fires synchronously") {
    CancellationSource source;
    CHECK(source.request_cancel());

    std::atomic<bool> fired{false};
    auto reg = source.token().register_callback([&fired] { fired.store(true, std::memory_order_release); });

    CHECK_FALSE(reg.valid());
    CHECK(fired.load(std::memory_order_acquire));
  }

  TEST_CASE("register_callback after cancel fires on the calling thread") {
    CancellationSource source;
    source.request_cancel();

    const auto caller_id = std::this_thread::get_id();
    std::atomic<std::thread::id> fire_thread{};

    auto reg = source.token().register_callback(
        [&fire_thread] { fire_thread.store(std::this_thread::get_id(), std::memory_order_release); });

    CHECK_FALSE(reg.valid());
    CHECK_EQ(fire_thread.load(std::memory_order_acquire), caller_id);
  }

  TEST_CASE("move construction transfers slot ownership") {
    CancellationSource source;
    auto token = source.token();

    std::atomic<int> count{0};
    auto reg_a = token.register_callback([&count] { count.fetch_add(1, std::memory_order_relaxed); });
    auto reg_b = std::move(reg_a);

    CHECK_FALSE(reg_a.valid());  // NOLINT(bugprone-use-after-move)
    CHECK(reg_b.valid());

    CHECK(source.request_cancel());
    CHECK_EQ(count.load(std::memory_order_relaxed), 1);
  }

  TEST_CASE("registration self-move-assignment is safe") {
    CancellationSource source;
    auto token = source.token();

    std::atomic<int> count{0};
    auto reg = token.register_callback([&count] { count.fetch_add(1, std::memory_order_relaxed); });
    CHECK(reg.valid());

    auto& alias = reg;
    reg = std::move(alias);

    CHECK(reg.valid());
    CHECK(source.request_cancel());
    CHECK_EQ(count.load(std::memory_order_relaxed), 1);
  }

  TEST_CASE("copied tokens share the same cancellation state") {
    CancellationSource source;
    auto token_a = source.token();
    auto token_b = token_a;

    CHECK(token_a.valid());
    CHECK(token_b.valid());

    CHECK(source.request_cancel());
    CHECK(token_a.is_cancellation_requested());
    CHECK(token_b.is_cancellation_requested());
  }

  TEST_CASE("multiple tokens from same source all observe cancellation") {
    CancellationSource source;
    static constexpr int kTokens = 64;

    std::vector<CancellationToken> tokens;
    tokens.reserve(kTokens);
    for (int i = 0; i < kTokens; ++i) {
      tokens.emplace_back(source.token());
    }

    CHECK(source.request_cancel());

    for (auto& t : tokens) {
      CHECK(t.is_cancellation_requested());
      CHECK_THROWS_AS(t.throw_if_cancellation_requested(), Exception::OperationCancelled);
    }
  }

  TEST_CASE("all registered callbacks fire on cancel") {
    CancellationSource source;
    auto token = source.token();

    static constexpr int kN = 16;
    std::atomic<int> count{0};
    std::vector<CancellationRegistration> regs;
    regs.reserve(kN);

    for (int i = 0; i < kN; ++i) {
      regs.emplace_back(token.register_callback([&count] { count.fetch_add(1, std::memory_order_relaxed); }));
    }

    CHECK(source.request_cancel());
    CHECK_EQ(count.load(std::memory_order_relaxed), kN);

    for (auto& r : regs) {
      CHECK_FALSE(r.valid());
    }
  }

  TEST_CASE("selectively reset registrations only fire survivors") {
    CancellationSource source;
    auto token = source.token();

    std::atomic<int> fires{0};
    std::vector<CancellationRegistration> survivors;
    std::vector<CancellationRegistration> cancelled;

    for (int i = 0; i < 16; ++i) {
      auto reg = token.register_callback([&fires] { fires.fetch_add(1, std::memory_order_relaxed); });
      if ((i & 1) == 0) {
        survivors.push_back(std::move(reg));
      } else {
        cancelled.push_back(std::move(reg));
      }
    }
    for (auto& r : cancelled) {
      r.reset();
    }

    CHECK(source.request_cancel());
    CHECK_EQ(fires.load(std::memory_order_acquire), 8);
  }

  TEST_CASE("callback can query token without deadlocking") {
    CancellationSource source;
    auto token = source.token();

    std::atomic<bool> observed{false};
    auto reg = token.register_callback(
        [token, &observed] { observed.store(token.is_cancellation_requested(), std::memory_order_release); });

    CHECK(source.request_cancel());
    CHECK(observed.load(std::memory_order_acquire));
  }

  TEST_CASE("callback exceptions do not propagate and sibling callbacks still run") {
    CancellationSource source;
    auto token = source.token();

    auto throwing = token.register_callback([] { throw std::runtime_error("oops"); });
    std::atomic<int> count{0};
    auto counting = token.register_callback([&count] { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK_NOTHROW(source.request_cancel());
    CHECK_EQ(count.load(std::memory_order_relaxed), 1);
  }

  TEST_CASE("throw_if_cancellation_requested throws OperationCancelled after cancel") {
    CancellationSource source;
    source.request_cancel();

    CHECK_THROWS_AS(source.token().throw_if_cancellation_requested(), Exception::OperationCancelled);

    try {
      source.token().throw_if_cancellation_requested();
      FAIL("expected Exception::OperationCancelled");
    } catch (const Exception::OperationCancelled& e) {
      CHECK_EQ(std::string(e.what()), "vlink operation cancelled");
    }
  }

  TEST_CASE("throw_if_cancellation_requested is no-op before cancel") {
    CancellationSource source;
    auto token = source.token();

    CHECK_NOTHROW(token.throw_if_cancellation_requested());
    CHECK_FALSE(token.is_cancellation_requested());
  }

  TEST_CASE("register_callback with empty functor yields invalid registration") {
    CancellationSource source;
    auto token = source.token();

    auto reg = token.register_callback(MoveFunction<void()>{});
    CHECK_FALSE(reg.valid());

    CHECK(source.request_cancel());
  }

  TEST_CASE("CancellationSource implicit copy shares state") {
    CancellationSource a;
    CancellationSource b = a;

    auto token = a.token();
    CHECK_FALSE(token.is_cancellation_requested());

    CHECK(b.request_cancel());
    CHECK(token.is_cancellation_requested());
    CHECK(a.is_cancellation_requested());
    CHECK_FALSE(a.request_cancel());
  }

  TEST_CASE("token outlives source and remains valid but never cancels") {
    CancellationToken token;
    {
      CancellationSource source;
      token = source.token();
      CHECK(token.valid());
    }

    CHECK(token.valid());
    CHECK_FALSE(token.is_cancellation_requested());
    CHECK_NOTHROW(token.throw_if_cancellation_requested());

    std::atomic<bool> fired{false};
    auto reg = token.register_callback([&fired] { fired.store(true, std::memory_order_release); });
    CHECK(reg.valid());

    reg.reset();
    CHECK_FALSE(fired.load(std::memory_order_acquire));
  }

  TEST_CASE("cascading cancellation propagates through parent to child source") {
    CancellationSource parent;
    CancellationSource child;

    auto reg = parent.token().register_callback([&child] { child.request_cancel(); });
    CHECK(parent.request_cancel());
    CHECK(child.token().is_cancellation_requested());
  }

  TEST_CASE("three-level cascade propagates through every level") {
    CancellationSource root;
    CancellationSource mid;
    CancellationSource leaf;

    auto reg_mid = root.token().register_callback([&mid] { mid.request_cancel(); });
    auto reg_leaf = mid.token().register_callback([&leaf] { leaf.request_cancel(); });

    CHECK(root.request_cancel());
    CHECK(mid.token().is_cancellation_requested());
    CHECK(leaf.token().is_cancellation_requested());
  }

  TEST_CASE("callback firing sibling source does not self-deadlock") {
    CancellationSource a;
    CancellationSource b;
    std::atomic<bool> a_observed{false};

    auto reg = a.token().register_callback([token_a = a.token(), &b, &a_observed] {
      a_observed.store(token_a.is_cancellation_requested(), std::memory_order_release);
      b.request_cancel();
    });

    CHECK(a.request_cancel());
    CHECK(a_observed.load(std::memory_order_acquire));
    CHECK(b.token().is_cancellation_requested());
  }

  TEST_CASE("registration destructor on cancelled source is a clean no-op") {
    CancellationSource source;
    auto token = source.token();

    {
      auto reg = token.register_callback([] {});
      CHECK(reg.valid());
      CHECK(source.request_cancel());
      CHECK_FALSE(reg.valid());
    }

    CHECK(source.is_cancellation_requested());
  }

  TEST_CASE("concurrent register and cancel is race-free") {
    CancellationSource source;
    auto token = source.token();

    static constexpr int kWorkers = 8;
    static constexpr int kPerWorker = 64;
    std::atomic<int> count{0};
    std::atomic<bool> start{false};

    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (int i = 0; i < kWorkers; ++i) {
      workers.emplace_back([&] {
        while (!start.load(std::memory_order_acquire)) {
        }
        for (int j = 0; j < kPerWorker; ++j) {
          auto reg = token.register_callback([&count] { count.fetch_add(1, std::memory_order_relaxed); });
          (void)reg;
        }
      });
    }

    start.store(true, std::memory_order_release);
    std::this_thread::yield();
    CHECK(source.request_cancel());

    for (auto& w : workers) {
      w.join();
    }

    CHECK(source.is_cancellation_requested());
    CHECK(count.load(std::memory_order_relaxed) <= kWorkers * kPerWorker);
  }

  TEST_CASE("many concurrent observers all see cancellation") {
    CancellationSource source;
    static constexpr int kObs = 16;
    std::atomic<int> seen{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> workers;
    workers.reserve(kObs);
    for (int i = 0; i < kObs; ++i) {
      workers.emplace_back([token = source.token(), &seen, &go] {
        while (!go.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        while (!token.is_cancellation_requested()) {
          std::this_thread::yield();
        }
        seen.fetch_add(1, std::memory_order_acq_rel);
      });
    }

    go.store(true, std::memory_order_release);
    std::this_thread::sleep_for(5ms);
    CHECK(source.request_cancel());
    for (auto& t : workers) {
      t.join();
    }

    CHECK_EQ(seen.load(), kObs);
  }
}

// NOLINTEND
