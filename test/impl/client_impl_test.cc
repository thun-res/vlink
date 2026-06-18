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

#include "./impl/client_impl.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "../common_test.h"
#include "./base/bytes.h"
#include "./impl/types.h"

namespace {

class TestClientImpl : public ClientImpl {
 public:
  TestClientImpl() = default;
  ~TestClientImpl() override = default;

  void init() override {}
  void deinit() override {}

  [[nodiscard]] bool is_connected() const override { return connected_.load(); }

  bool call(const Bytes& /*req_data*/, MsgCallback&& /*callback*/, std::chrono::milliseconds /*timeout*/) override {
    return false;
  }

  void set_connected(bool v) { connected_ = v; }

 private:
  std::atomic_bool connected_{false};
};

}  // namespace

TEST_SUITE("impl-ClientImpl") {
  TEST_CASE("constructor sets kClient impl_type") {
    TestClientImpl client;
    CHECK_EQ(client.impl_type, kClient);
  }

  TEST_CASE("is_connected returns false on construction") {
    TestClientImpl client;
    CHECK(client.is_connected() == false);
  }

  TEST_CASE("is_resp_type defaults to false") {
    TestClientImpl client;
    CHECK(client.is_resp_type == false);
  }

  TEST_CASE("is_interrupted returns false on construction") {
    TestClientImpl client;
    CHECK(client.is_interrupted() == false);
  }

  TEST_CASE("detect_connected callback not called when not connected") {
    TestClientImpl client;
    bool called = false;
    client.detect_connected([&](bool) { called = true; });
    CHECK(called == false);
  }

  TEST_CASE("detect_connected callback fires immediately when already connected") {
    TestClientImpl client;
    client.set_connected(true);
    client.update_connected();

    bool called = false;
    bool received = false;
    client.detect_connected([&](bool v) {
      called = true;
      received = v;
    });

    CHECK(called == true);
    CHECK(received == true);
  }

  TEST_CASE("detect_connected callback fires when server appears after registration") {
    TestClientImpl client;
    std::atomic_bool called{false};
    std::atomic_bool received{false};

    client.detect_connected([&](bool v) {
      called = true;
      received = v;
    });

    CHECK(called == false);

    client.set_connected(true);
    client.update_connected();

    CHECK(called == true);
    CHECK(received == true);
  }

  TEST_CASE("detect_connected callback fires on disconnect after connect") {
    TestClientImpl client;
    int count = 0;
    bool last = false;

    client.set_connected(true);
    client.update_connected();

    client.detect_connected([&](bool v) {
      ++count;
      last = v;
    });

    CHECK(count == 1);
    CHECK(last == true);

    client.set_connected(false);
    client.update_connected();

    CHECK(count == 2);
    CHECK(last == false);
  }

  TEST_CASE("update_connected is no-op when state has not changed") {
    TestClientImpl client;
    int count = 0;
    client.detect_connected([&](bool) { ++count; });

    client.update_connected();

    CHECK(count == 0);
  }

  TEST_CASE("update_connected fires callback on each state transition") {
    TestClientImpl client;
    int count = 0;
    client.detect_connected([&](bool) { ++count; });

    client.set_connected(true);
    client.update_connected();
    CHECK(count == 1);

    client.set_connected(false);
    client.update_connected();
    CHECK(count == 2);
  }

  TEST_CASE("wait_for_connected returns true immediately when already connected") {
    TestClientImpl client;
    client.set_connected(true);
    CHECK(client.wait_for_connected(100ms) == true);
  }

  TEST_CASE("wait_for_connected returns false on timeout when not connected") {
    TestClientImpl client;
    auto start = std::chrono::steady_clock::now();
    bool result = client.wait_for_connected(50ms);
    auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(result == false);
    CHECK(elapsed >= 40ms);
  }

  TEST_CASE("wait_for_connected returns true when server appears during wait") {
    TestClientImpl client;

    std::thread t([&]() {
      std::this_thread::sleep_for(20ms);
      client.set_connected(true);
      client.update_connected();
    });

    bool result = client.wait_for_connected(500ms);
    t.join();
    CHECK(result == true);
  }

  TEST_CASE("interrupt sets interrupted flag") {
    TestClientImpl client;
    client.interrupt();
    CHECK(client.is_interrupted() == true);
  }

  TEST_CASE("interrupt unblocks wait_for_connected") {
    TestClientImpl client;
    std::atomic_bool done{false};

    std::thread t([&]() {
      client.wait_for_connected(-1ms);
      done = true;
    });

    std::this_thread::sleep_for(20ms);
    client.interrupt();
    t.join();

    CHECK(done == true);
    CHECK(client.is_interrupted() == true);
  }

  TEST_CASE("reset_interrupted clears the interrupted flag") {
    TestClientImpl client;
    client.interrupt();
    CHECK(client.is_interrupted() == true);
    client.reset_interrupted();
    CHECK(client.is_interrupted() == false);
  }

  TEST_CASE("impl_type is kClient not other roles") {
    TestClientImpl client;

    CHECK_NE(client.impl_type, kPublisher);
    CHECK_NE(client.impl_type, kSubscriber);
    CHECK_NE(client.impl_type, kServer);
    CHECK_EQ(client.impl_type, kClient);
  }

  TEST_CASE("set_property and get_property persist on client") {
    TestClientImpl client;

    client.set_property("timeout.ms", "500");
    CHECK_EQ(client.get_property("timeout.ms"), "500");
  }

  TEST_CASE("get_property returns empty for unset key") {
    TestClientImpl client;

    CHECK(client.get_property("no.such.key").empty());
  }

  TEST_CASE("detect_connected fires on multiple transitions") {
    TestClientImpl client;
    std::vector<bool> events;

    client.detect_connected([&](bool v) { events.push_back(v); });

    client.set_connected(true);
    client.update_connected();
    client.set_connected(false);
    client.update_connected();
    client.set_connected(true);
    client.update_connected();

    REQUIRE_EQ(events.size(), 3u);
    CHECK(events[0]);
    CHECK_FALSE(events[1]);
    CHECK(events[2]);
  }

  TEST_CASE("is_resp_type can be set and read back") {
    TestClientImpl client;

    client.is_resp_type = true;
    CHECK(client.is_resp_type == true);

    client.is_resp_type = false;
    CHECK(client.is_resp_type == false);
  }

  TEST_CASE("concurrent update_connected calls do not crash") {
    TestClientImpl client;
    std::atomic<int> count{0};

    client.detect_connected([&](bool) { count.fetch_add(1, std::memory_order_relaxed); });
    client.set_connected(true);

    std::vector<std::thread> threads;
    threads.reserve(4);

    for (int i = 0; i < 4; ++i) {
      threads.emplace_back([&] { client.update_connected(); });
    }

    for (auto& t : threads) {
      t.join();
    }

    CHECK(count.load() >= 1);
  }

  TEST_CASE("wait_for_connected with infinite timeout unblocks on interrupt") {
    TestClientImpl client;
    std::atomic<bool> returned{false};

    std::thread t([&] {
      client.wait_for_connected(-1ms);
      returned.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(30ms);
    client.interrupt();
    t.join();

    CHECK(returned.load(std::memory_order_acquire));
    CHECK(client.is_interrupted());
  }

  TEST_CASE("call always returns false in base implementation") {
    TestClientImpl client;
    Bytes req;
    bool called = false;

    bool ok = client.call(req, [&](const Bytes&) { called = true; }, 100ms);

    CHECK_FALSE(ok);
    CHECK_FALSE(called);
  }
}

// NOLINTEND
