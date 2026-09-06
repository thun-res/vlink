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
  TEST_CASE("completed acknowledgement survives clear before send returns") {
    AckManager manager;
    auto request = manager.create_request();
    CHECK(manager.process(request, 2000, [&] {
      CHECK(AckManager::notify(request));
      CHECK_FALSE(manager.remove(request));
      manager.clear();
      return true;
    }));
    CHECK_FALSE(AckManager::notify(request));
  }

  TEST_CASE("cancelled request can reject notification after its manager is destroyed") {
    AckManager::RequestPtr request;
    {
      AckManager manager;
      request = manager.create_request();
      CHECK_FALSE(manager.process(request, 2000, [&] {
        manager.clear();
        return true;
      }));
    }
    bool filled = false;
    CHECK_FALSE(AckManager::notify(request, [&] { filled = true; }));
    CHECK_FALSE(filled);
  }

  TEST_CASE("concurrent notifications fill a pending request only once") {
    AckManager manager;
    auto request = manager.create_request();
    std::atomic<int> fills{0};
    std::atomic<int> acknowledgements{0};
    std::thread first;
    std::thread second;
    CHECK(manager.process(request, 2000, [&] {
      auto notify = [&] {
        if (AckManager::notify(request, [&] { fills.fetch_add(1, std::memory_order_relaxed); })) {
          acknowledgements.fetch_add(1, std::memory_order_relaxed);
        }
      };
      first = std::thread(notify);
      second = std::thread(notify);
      return true;
    }));
    first.join();
    second.join();
    CHECK(fills.load(std::memory_order_relaxed) == 1);
    CHECK(acknowledgements.load(std::memory_order_relaxed) == 1);
  }

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

  TEST_CASE("process accepts a synchronous notify before the deadline") {
    AckManager mgr;
    auto req = mgr.create_request();
    bool result = mgr.process(req, 2000, [&]() { return mgr.notify(req); });
    CHECK(result == true);
  }

  TEST_CASE("process keeps a notify received before the deadline when its callback finishes later") {
    AckManager mgr;
    auto req = mgr.create_request();
    bool notify_result = false;
    bool callback_finished = false;

    bool result = mgr.process(req, 500, [&]() {
      notify_result = mgr.notify(req, [&]() {
        std::this_thread::sleep_for(550ms);
        callback_finished = true;
      });
      return true;
    });

    CHECK(result);
    CHECK(notify_result);
    CHECK(callback_finished);
  }

  TEST_CASE("process rejects a synchronous notify after the deadline") {
    AckManager mgr;
    auto req = mgr.create_request();
    bool notify_result = true;
    bool callback_called = false;

    bool result = mgr.process(req, 0, [&]() {
      notify_result = mgr.notify(req, [&]() { callback_called = true; });
      return true;
    });

    CHECK_FALSE(result);
    CHECK_FALSE(notify_result);
    CHECK_FALSE(callback_called);
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
    std::atomic<bool> process_started{false};
    std::atomic<bool> callback_started{false};
    std::atomic<bool> callback_finished{false};
    std::atomic<bool> process_returned{false};
    std::atomic<bool> release_callback{false};
    bool result = false;

    std::thread processor([&]() {
      result = mgr.process(req, 2000, [&process_started]() {
        process_started.store(true, std::memory_order_release);
        return true;
      });
      process_returned.store(true, std::memory_order_release);
    });

    const bool process_ready =
        common_test::wait_until([&process_started]() { return process_started.load(std::memory_order_acquire); }, 2s);

    if (!process_ready) {
      mgr.clear();
      processor.join();
      CHECK(process_ready);
      return;
    }

    std::thread notifier([&]() {
      mgr.notify(req, [&]() {
        callback_started.store(true, std::memory_order_release);

        while (!release_callback.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }

        callback_finished.store(true, std::memory_order_release);
      });
    });

    const bool callback_ready =
        common_test::wait_until([&callback_started]() { return callback_started.load(std::memory_order_acquire); }, 2s);

    if (!callback_ready) {
      release_callback.store(true, std::memory_order_release);
      mgr.clear();
      notifier.join();
      processor.join();
      CHECK(callback_ready);
      return;
    }

    CHECK_FALSE(process_returned.load(std::memory_order_acquire));

    release_callback.store(true, std::memory_order_release);
    notifier.join();
    processor.join();

    CHECK(result);
    CHECK(callback_finished.load(std::memory_order_acquire));
    CHECK(process_returned.load(std::memory_order_acquire));
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
    CHECK_FALSE(mgr.notify(nullptr));
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
    CHECK_FALSE(mgr.remove(nullptr));
  }

  TEST_CASE("remove wakes a blocked process call with false") {
    AckManager mgr;
    auto req = mgr.create_request();
    std::atomic<bool> process_started{false};
    bool result = true;

    std::thread processor([&]() {
      result = mgr.process(req, -1, [&]() {
        process_started.store(true, std::memory_order_release);
        return true;
      });
    });

    const bool process_ready =
        common_test::wait_until([&]() { return process_started.load(std::memory_order_acquire); }, 2s);
    if (!process_ready) {
      mgr.clear();
      processor.join();
      CHECK(process_ready);
      return;
    }
    CHECK(mgr.remove(req));
    processor.join();

    CHECK_FALSE(result);
  }

  TEST_CASE("failed send waits for a concurrent notify callback to finish") {
    AckManager mgr;
    auto req = mgr.create_request();
    std::atomic<bool> send_started{false};
    std::atomic<bool> release_send{false};
    std::atomic<bool> fill_started{false};
    std::atomic<bool> release_fill{false};
    std::atomic<bool> process_returned{false};
    bool result = true;

    std::thread processor([&]() {
      result = mgr.process(req, -1, [&]() {
        send_started.store(true, std::memory_order_release);
        while (!release_send.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        return false;
      });
      process_returned.store(true, std::memory_order_release);
    });

    const bool send_ready = common_test::wait_until([&]() { return send_started.load(std::memory_order_acquire); }, 2s);
    if (!send_ready) {
      release_send.store(true, std::memory_order_release);
      mgr.clear();
      processor.join();
      CHECK(send_ready);
      return;
    }

    std::thread notifier([&]() {
      mgr.notify(req, [&]() {
        fill_started.store(true, std::memory_order_release);
        while (!release_fill.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
      });
    });

    const bool fill_ready = common_test::wait_until([&]() { return fill_started.load(std::memory_order_acquire); }, 2s);
    if (!fill_ready) {
      release_send.store(true, std::memory_order_release);
      release_fill.store(true, std::memory_order_release);
      mgr.clear();
      notifier.join();
      processor.join();
      CHECK(fill_ready);
      return;
    }
    release_send.store(true, std::memory_order_release);
    CHECK_FALSE(common_test::wait_until([&]() { return process_returned.load(std::memory_order_acquire); }, 20ms));

    release_fill.store(true, std::memory_order_release);
    notifier.join();
    processor.join();

    CHECK_FALSE(result);
    CHECK(process_returned.load(std::memory_order_acquire));
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
