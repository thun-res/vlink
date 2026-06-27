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

#include "./impl/ack_manager.h"

#include <doctest/doctest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "../common_test.h"

TEST_SUITE("impl-AckManager") {
  TEST_CASE("create_request returns non-null token") {
    AckManager mgr;
    auto req = mgr.create_request();
    CHECK(req != nullptr);
  }

  TEST_CASE("successive tokens are distinct") {
    AckManager mgr;
    auto r1 = mgr.create_request();
    auto r2 = mgr.create_request();
    auto r3 = mgr.create_request();
    CHECK(r1 != r2);
    CHECK(r2 != r3);
    CHECK(r1 != r3);
  }

  TEST_CASE("process returns true when notified by another thread") {
    AckManager mgr;
    auto req = mgr.create_request();

    std::thread notifier([&mgr, &req]() {
      std::this_thread::sleep_for(20ms);
      mgr.notify(req);
    });

    bool result = mgr.process(req, 2000, []() { return true; });
    notifier.join();

    CHECK(result == true);
  }

  TEST_CASE("process returns false on timeout when no notify arrives") {
    AckManager mgr;
    auto req = mgr.create_request();
    bool result = mgr.process(req, 50, []() { return true; });
    CHECK(result == false);
  }

  TEST_CASE("process returns false when send callback returns false") {
    AckManager mgr;
    auto req = mgr.create_request();
    bool result = mgr.process(req, 2000, []() { return false; });
    CHECK(result == false);
  }

  TEST_CASE("process with zero timeout returns false without blocking") {
    AckManager mgr;
    auto req = mgr.create_request();
    bool result = mgr.process(req, 0, []() { return true; });
    CHECK(result == false);
  }

  TEST_CASE("process with negative timeout waits until notify") {
    AckManager mgr;
    auto req = mgr.create_request();

    std::thread notifier([&mgr, &req]() {
      std::this_thread::sleep_for(30ms);
      mgr.notify(req);
    });

    bool result = mgr.process(req, -1, []() { return true; });
    notifier.join();

    CHECK(result == true);
  }

  TEST_CASE("notify invokes optional callback before waking process") {
    AckManager mgr;
    auto req = mgr.create_request();
    bool callback_called = false;

    std::thread notifier([&]() {
      std::this_thread::sleep_for(20ms);
      mgr.notify(req, [&callback_called]() { callback_called = true; });
    });

    bool result = mgr.process(req, 2000, []() { return true; });
    notifier.join();

    CHECK(result == true);
    CHECK(callback_called == true);
  }

  TEST_CASE("notify with nullptr callback still wakes process") {
    AckManager mgr;
    auto req = mgr.create_request();

    std::thread notifier([&]() {
      std::this_thread::sleep_for(20ms);
      mgr.notify(req, nullptr);
    });

    bool result = mgr.process(req, 2000, []() { return true; });
    notifier.join();

    CHECK(result == true);
  }

  TEST_CASE("notify returns false for request not in the pending set") {
    AckManager mgr;
    auto req = mgr.create_request();
    bool notified = mgr.notify(req);
    CHECK(notified == false);
  }

  TEST_CASE("second notify on same request returns false") {
    AckManager mgr;
    auto req = mgr.create_request();
    bool first_ok = false;
    bool second_ok = false;

    std::thread notifier([&]() {
      std::this_thread::sleep_for(20ms);
      first_ok = mgr.notify(req);
      second_ok = mgr.notify(req);
    });

    (void)mgr.process(req, 2000, []() { return true; });
    notifier.join();

    CHECK(first_ok == true);
    CHECK(second_ok == false);
  }

  TEST_CASE("remove returns false for request not in the pending set") {
    AckManager mgr;
    auto req = mgr.create_request();
    CHECK(mgr.remove(req) == false);
  }

  TEST_CASE("clear wakes all blocked process calls with false") {
    AckManager mgr;
    static constexpr int kRequests = 3;

    std::vector<AckManager::RequestPtr> reqs;
    reqs.reserve(kRequests);
    for (int i = 0; i < kRequests; ++i) {
      reqs.push_back(mgr.create_request());
    }

    std::atomic<int> failed{0};
    std::vector<std::thread> threads;
    threads.reserve(kRequests);

    for (int i = 0; i < kRequests; ++i) {
      threads.emplace_back([&mgr, &reqs, i, &failed]() {
        bool ok = mgr.process(reqs[i], 5000, []() { return true; });

        if (!ok) {
          ++failed;
        }
      });
    }

    std::this_thread::sleep_for(50ms);
    mgr.clear();

    for (auto& t : threads) {
      t.join();
    }

    CHECK(failed.load() == kRequests);
  }

  TEST_CASE("process returns false immediately after clear") {
    AckManager mgr;
    mgr.clear();
    auto req = mgr.create_request();
    bool result = mgr.process(req, 2000, []() { return true; });
    CHECK(result == false);
  }

  TEST_CASE("reset_interrupted allows new requests to succeed while old generation still fails") {
    AckManager mgr;
    auto old_req = mgr.create_request();
    std::atomic<bool> old_result{true};

    std::thread waiter(
        [&]() { old_result.store(mgr.process(old_req, 5000, []() { return true; }), std::memory_order_release); });

    std::this_thread::sleep_for(50ms);
    mgr.clear();
    mgr.reset_interrupted();

    auto new_req = mgr.create_request();

    std::thread notifier([&]() {
      std::this_thread::sleep_for(20ms);
      mgr.notify(new_req);
    });

    bool new_result = mgr.process(new_req, 2000, []() { return true; });
    waiter.join();
    notifier.join();

    CHECK(new_result == true);
    CHECK(old_result.load(std::memory_order_acquire) == false);
  }

  TEST_CASE("multiple concurrent process and notify pairs all succeed") {
    AckManager mgr;
    static constexpr int kPairs = 5;

    std::vector<AckManager::RequestPtr> reqs;
    reqs.reserve(kPairs);
    for (int i = 0; i < kPairs; ++i) {
      reqs.push_back(mgr.create_request());
    }

    std::atomic<int> success{0};
    std::vector<std::thread> waiters;
    waiters.reserve(kPairs);

    for (int i = 0; i < kPairs; ++i) {
      waiters.emplace_back([&mgr, &reqs, i, &success]() {
        bool ok = mgr.process(reqs[i], 3000, []() { return true; });

        if (ok) {
          ++success;
        }
      });
    }

    std::this_thread::sleep_for(30ms);

    for (int i = kPairs - 1; i >= 0; --i) {
      mgr.notify(reqs[i]);
    }

    for (auto& t : waiters) {
      t.join();
    }

    CHECK(success.load() == kPairs);
  }
}

// NOLINTEND
