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

#include "./impl/subscriber_impl.h"

#include <doctest/doctest.h>

#include "../common_test.h"

namespace {

class TestSubscriber : public SubscriberImpl {
 public:
  TestSubscriber() = default;
  ~TestSubscriber() override = default;

  void init() override {}
  void deinit() override {}

  using SubscriberImpl::listen;

  bool listen(MsgCallback&& cb) override {
    callback_ = std::move(cb);
    is_listened = true;
    return true;
  }

  void fire(const Bytes& data) {
    if (callback_) {
      callback_(data);
    }
  }

 private:
  MsgCallback callback_;
};

}  // namespace

TEST_SUITE("impl-SubscriberImpl") {
  TEST_CASE("constructor sets kSubscriber role") {
    TestSubscriber sub;

    CHECK_EQ(sub.impl_type, kSubscriber);
  }

  TEST_CASE("is_listened defaults to false") {
    TestSubscriber sub;

    CHECK_FALSE(sub.is_listened);
  }

  TEST_CASE("listen sets is_listened and returns true") {
    TestSubscriber sub;

    bool ok = sub.listen([](const Bytes&) {});

    CHECK(ok);
    CHECK(sub.is_listened);
  }

  TEST_CASE("listen callback fires when message is delivered") {
    TestSubscriber sub;
    Bytes received;

    sub.listen([&](const Bytes& data) { received = data; });

    Bytes msg = {1, 2, 3};
    sub.fire(msg);

    CHECK_EQ(received.size(), 3u);
  }

  TEST_CASE("listen(IntraMsgCallback) returns false in base implementation") {
    TestSubscriber sub;
    bool invoked = false;
    NodeImpl::IntraMsgCallback cb = [&](const IntraData&) { invoked = true; };

    bool ok = sub.listen(std::move(cb));

    CHECK_FALSE(ok);
    CHECK_FALSE(invoked);
  }

  TEST_CASE("set_latency_and_lost_enabled is a no-op in base implementation") {
    TestSubscriber sub;

    sub.set_latency_and_lost_enabled(true);

    CHECK_FALSE(sub.is_latency_and_lost_enabled());
  }

  TEST_CASE("is_latency_and_lost_enabled returns false by default") {
    TestSubscriber sub;

    CHECK_FALSE(sub.is_latency_and_lost_enabled());
  }

  TEST_CASE("get_latency returns 0 by default") {
    TestSubscriber sub;

    CHECK_EQ(sub.get_latency(), 0);
  }

  TEST_CASE("get_lost returns zero-initialised SampleLostInfo by default") {
    TestSubscriber sub;
    auto info = sub.get_lost();

    CHECK_EQ(info.total, 0u);
    CHECK_EQ(info.lost, 0u);
  }

  TEST_CASE("multiple messages delivered in order") {
    TestSubscriber sub;
    int calls = 0;

    sub.listen([&](const Bytes&) { ++calls; });

    Bytes msg;
    sub.fire(msg);
    sub.fire(msg);
    sub.fire(msg);

    CHECK_EQ(calls, 3);
  }

  TEST_CASE("listen callback receives payload bytes with correct size") {
    TestSubscriber sub;
    size_t received_size = 0;

    sub.listen([&](const Bytes& data) { received_size = data.size(); });

    Bytes msg = Bytes::create(128);
    sub.fire(msg);

    CHECK_EQ(received_size, 128u);
  }

  TEST_CASE("listen callback receives empty bytes without crash") {
    TestSubscriber sub;
    bool called = false;
    size_t received_size = 99;

    sub.listen([&](const Bytes& data) {
      called = true;
      received_size = data.size();
    });

    Bytes empty;
    sub.fire(empty);

    CHECK(called);
    CHECK_EQ(received_size, 0u);
  }

  TEST_CASE("fire without listen does not crash") {
    TestSubscriber sub;
    Bytes msg = {1, 2, 3};

    CHECK_NOTHROW(sub.fire(msg));
  }

  TEST_CASE("constructor sets kSubscriber not other roles") {
    TestSubscriber sub;

    CHECK_NE(sub.impl_type, kPublisher);
    CHECK_NE(sub.impl_type, kServer);
    CHECK_NE(sub.impl_type, kClient);
    CHECK_EQ(sub.impl_type, kSubscriber);
  }

  TEST_CASE("set_property and get_property round trip on subscriber") {
    TestSubscriber sub;

    sub.set_property("qos.depth", "10");
    CHECK_EQ(sub.get_property("qos.depth"), "10");
  }

  TEST_CASE("get_property returns empty for unknown key") {
    TestSubscriber sub;

    CHECK(sub.get_property("no.such.key").empty());
  }

  TEST_CASE("interrupt and reset_interrupted work on subscriber") {
    TestSubscriber sub;

    CHECK_FALSE(sub.is_interrupted());

    sub.interrupt();
    CHECK(sub.is_interrupted());

    sub.reset_interrupted();
    CHECK_FALSE(sub.is_interrupted());
  }

  TEST_CASE("listen called twice overwrites first callback") {
    TestSubscriber sub;
    int first_count = 0;
    int second_count = 0;

    sub.listen([&](const Bytes&) { ++first_count; });
    sub.listen([&](const Bytes&) { ++second_count; });

    Bytes msg;
    sub.fire(msg);

    CHECK_EQ(first_count, 0);
    CHECK_EQ(second_count, 1);
  }

  TEST_CASE("large payload fires callback once with correct size") {
    static constexpr size_t kSize = 65536u;
    TestSubscriber sub;
    size_t received_size = 0;

    sub.listen([&](const Bytes& data) { received_size = data.size(); });

    Bytes msg = Bytes::create(kSize);
    sub.fire(msg);

    CHECK_EQ(received_size, kSize);
  }
}

// NOLINTEND
