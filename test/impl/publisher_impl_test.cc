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

#include "./impl/publisher_impl.h"

#include <doctest/doctest.h>

#include <atomic>
#include <thread>

#include "../common_test.h"

namespace {

class TestPublisher : public PublisherImpl {
 public:
  TestPublisher() = default;
  ~TestPublisher() override = default;

  void init() override {}
  void deinit() override {}

  bool has_subscribers() const override { return has_subs_.load(); }

  bool write(const Bytes& /*msg*/) override {
    ++write_count;
    return true;
  }

  void set_has_subs(bool v) { has_subs_ = v; }

  int write_count{0};

 private:
  std::atomic_bool has_subs_{false};
};

}  // namespace

TEST_SUITE("impl-PublisherImpl") {
  TEST_CASE("constructor sets kPublisher role") {
    TestPublisher pub;

    CHECK_EQ(pub.impl_type, kPublisher);
  }

  TEST_CASE("has_subscribers returns false on default construction") {
    TestPublisher pub;

    CHECK_FALSE(pub.has_subscribers());
  }

  TEST_CASE("detect_subscribers does not fire when no subscribers present") {
    TestPublisher pub;
    bool called = false;

    pub.detect_subscribers([&](bool) { called = true; });

    CHECK_FALSE(called);
  }

  TEST_CASE("detect_subscribers fires immediately when subscribers already present") {
    TestPublisher pub;
    pub.set_has_subs(true);
    pub.update_subscribers();

    bool called = false;
    bool value = false;

    pub.detect_subscribers([&](bool v) {
      called = true;
      value = v;
    });

    CHECK(called);
    CHECK(value);
  }

  TEST_CASE("update_subscribers fires callback when subscriber state changes") {
    TestPublisher pub;
    int count = 0;
    bool last = false;

    pub.detect_subscribers([&](bool v) {
      ++count;
      last = v;
    });

    pub.set_has_subs(true);
    pub.update_subscribers();

    CHECK_EQ(count, 1);
    CHECK(last);

    pub.set_has_subs(false);
    pub.update_subscribers();

    CHECK_EQ(count, 2);
    CHECK_FALSE(last);
  }

  TEST_CASE("update_subscribers is no-op when state unchanged") {
    TestPublisher pub;
    int count = 0;

    pub.detect_subscribers([&](bool) { ++count; });

    pub.update_subscribers();
    pub.update_subscribers();

    CHECK_EQ(count, 0);
  }

  TEST_CASE("wait_for_subscribers returns true immediately when already subscribed") {
    TestPublisher pub;
    pub.set_has_subs(true);

    CHECK(pub.wait_for_subscribers(100ms));
  }

  TEST_CASE("wait_for_subscribers times out when no subscribers appear") {
    TestPublisher pub;

    bool result = pub.wait_for_subscribers(50ms);

    CHECK_FALSE(result);
  }

  TEST_CASE("wait_for_subscribers returns true when subscriber appears mid-wait") {
    TestPublisher pub;

    std::thread t([&] {
      std::this_thread::sleep_for(20ms);
      pub.set_has_subs(true);
      pub.update_subscribers();
    });

    bool result = pub.wait_for_subscribers(500ms);
    t.join();

    CHECK(result);
  }

  TEST_CASE("interrupt sets interrupted flag and unblocks wait") {
    TestPublisher pub;

    std::thread t([&] {
      std::this_thread::sleep_for(20ms);
      pub.interrupt();
    });

    pub.wait_for_subscribers(2000ms);
    t.join();

    CHECK(pub.is_interrupted());
  }

  TEST_CASE("write(IntraData) returns false in base implementation") {
    TestPublisher pub;
    IntraData data = std::make_shared<IntraDataType>();

    bool result = static_cast<PublisherImpl&>(pub).write(data);

    CHECK_FALSE(result);
  }

  TEST_CASE("write(Bytes) delegates to subclass override") {
    TestPublisher pub;
    Bytes msg = Bytes::create(4);

    pub.write(msg);

    CHECK_EQ(pub.write_count, 1);
  }

  TEST_CASE("multiple writes increment write_count independently") {
    TestPublisher pub;
    Bytes msg = Bytes::create(8);

    for (int i = 0; i < 10; ++i) {
      pub.write(msg);
    }

    CHECK_EQ(pub.write_count, 10);
  }

  TEST_CASE("write with empty bytes does not crash") {
    TestPublisher pub;
    Bytes empty;

    pub.write(empty);

    CHECK_EQ(pub.write_count, 1);
  }

  TEST_CASE("detect_subscribers callback fires on transition true then false") {
    TestPublisher pub;
    std::vector<bool> events;

    pub.detect_subscribers([&](bool v) { events.push_back(v); });

    pub.set_has_subs(true);
    pub.update_subscribers();
    pub.set_has_subs(false);
    pub.update_subscribers();

    REQUIRE_EQ(events.size(), 2u);
    CHECK(events[0]);
    CHECK_FALSE(events[1]);
  }

  TEST_CASE("detect_subscribers replaces previous callback") {
    TestPublisher pub;
    int first_count = 0;
    int second_count = 0;

    pub.detect_subscribers([&](bool) { ++first_count; });
    pub.detect_subscribers([&](bool) { ++second_count; });

    pub.set_has_subs(true);
    pub.update_subscribers();

    CHECK_EQ(first_count, 0);
    CHECK_EQ(second_count, 1);
  }

  TEST_CASE("concurrent update_subscribers calls are safe") {
    TestPublisher pub;
    std::atomic<int> count{0};

    pub.detect_subscribers([&](bool) { count.fetch_add(1, std::memory_order_relaxed); });

    pub.set_has_subs(true);

    std::vector<std::thread> threads;
    threads.reserve(4);

    for (int i = 0; i < 4; ++i) {
      threads.emplace_back([&] { pub.update_subscribers(); });
    }

    for (auto& t : threads) {
      t.join();
    }

    CHECK(count.load() >= 1);
  }

  TEST_CASE("reset_interrupted clears interrupted flag") {
    TestPublisher pub;

    pub.interrupt();
    CHECK(pub.is_interrupted());

    pub.reset_interrupted();
    CHECK_FALSE(pub.is_interrupted());
  }

  TEST_CASE("wait_for_subscribers with negative timeout blocks until subscriber") {
    TestPublisher pub;

    std::thread t([&] {
      std::this_thread::sleep_for(30ms);
      pub.set_has_subs(true);
      pub.update_subscribers();
    });

    bool result = pub.wait_for_subscribers(-1ms);
    t.join();

    CHECK(result);
  }

  TEST_CASE("impl_type is kPublisher not kSubscriber or other roles") {
    TestPublisher pub;

    CHECK_NE(pub.impl_type, kSubscriber);
    CHECK_NE(pub.impl_type, kServer);
    CHECK_NE(pub.impl_type, kClient);
    CHECK_EQ(pub.impl_type, kPublisher);
  }

  TEST_CASE("set_property and get_property persist on publisher") {
    TestPublisher pub;

    pub.set_property("custom.key", "custom.value");
    CHECK_EQ(pub.get_property("custom.key"), "custom.value");
  }

  TEST_CASE("get_property returns empty for unset key") {
    TestPublisher pub;

    CHECK(pub.get_property("nonexistent.key").empty());
  }
}

// NOLINTEND
