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

#include "./impl/getter_impl.h"

#include <doctest/doctest.h>

#include "../common_test.h"
#include "./base/bytes.h"
#include "./impl/types.h"

namespace {

class TestGetterImpl : public GetterImpl {
 public:
  TestGetterImpl() = default;
  ~TestGetterImpl() override = default;

  void init() override {}
  void deinit() override {}

  bool listen(MsgCallback&& callback) override {
    callback_ = std::move(callback);
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

TEST_SUITE("impl-GetterImpl") {
  TEST_CASE("constructor sets kGetter impl_type") {
    TestGetterImpl getter;
    CHECK_EQ(getter.impl_type, kGetter);
  }

  TEST_CASE("is_listened defaults to false") {
    TestGetterImpl getter;
    CHECK(getter.is_listened == false);
  }

  TEST_CASE("listen sets is_listened to true") {
    TestGetterImpl getter;
    getter.listen([](const Bytes&) {});
    CHECK(getter.is_listened == true);
  }

  TEST_CASE("listen returns true on success") {
    TestGetterImpl getter;
    bool result = getter.listen([](const Bytes&) {});
    CHECK(result == true);
  }

  TEST_CASE("listen callback fires when value is updated") {
    TestGetterImpl getter;
    bool received = false;
    getter.listen([&](const Bytes&) { received = true; });

    Bytes data;
    getter.fire(data);

    CHECK(received == true);
  }

  TEST_CASE("listen callback receives the correct payload") {
    TestGetterImpl getter;
    size_t received_size = 0;
    getter.listen([&](const Bytes& b) { received_size = b.size(); });

    Bytes msg = {10, 20, 30};
    getter.fire(msg);

    CHECK_EQ(received_size, 3u);
  }

  TEST_CASE("callback fires multiple times for multiple updates") {
    TestGetterImpl getter;
    int count = 0;
    getter.listen([&](const Bytes&) { ++count; });

    Bytes data;
    getter.fire(data);
    getter.fire(data);
    getter.fire(data);

    CHECK_EQ(count, 3);
  }

  TEST_CASE("base set_latency_and_lost_enabled is a no-op") {
    TestGetterImpl getter;
    getter.set_latency_and_lost_enabled(true);
    CHECK(getter.is_latency_and_lost_enabled() == false);
  }

  TEST_CASE("is_latency_and_lost_enabled returns false by default") {
    TestGetterImpl getter;
    CHECK(getter.is_latency_and_lost_enabled() == false);
  }

  TEST_CASE("get_latency returns zero by default") {
    TestGetterImpl getter;
    CHECK_EQ(getter.get_latency(), 0);
  }

  TEST_CASE("get_lost returns zero-initialised SampleLostInfo by default") {
    TestGetterImpl getter;
    auto info = getter.get_lost();
    CHECK_EQ(info.total, 0u);
    CHECK_EQ(info.lost, 0u);
  }

  TEST_CASE("disabling latency tracking when it was never enabled is a no-op") {
    TestGetterImpl getter;
    getter.set_latency_and_lost_enabled(false);
    CHECK(getter.is_latency_and_lost_enabled() == false);
  }

  TEST_CASE("impl_type is kGetter not other roles") {
    TestGetterImpl getter;

    CHECK_NE(getter.impl_type, kPublisher);
    CHECK_NE(getter.impl_type, kSubscriber);
    CHECK_NE(getter.impl_type, kServer);
    CHECK_NE(getter.impl_type, kClient);
    CHECK_NE(getter.impl_type, kSetter);
    CHECK_EQ(getter.impl_type, kGetter);
  }

  TEST_CASE("fire without listen does not crash") {
    TestGetterImpl getter;
    Bytes data = {1, 2, 3};

    CHECK_NOTHROW(getter.fire(data));
  }

  TEST_CASE("is_listened becomes true after listen is called") {
    TestGetterImpl getter;
    CHECK_FALSE(getter.is_listened);

    getter.listen([](const Bytes&) {});
    CHECK(getter.is_listened);
  }

  TEST_CASE("listen callback receives payload with correct content") {
    TestGetterImpl getter;
    Bytes captured;

    getter.listen([&](const Bytes& data) { captured = data; });

    Bytes msg = {0xDE, 0xAD, 0xBE, 0xEF};
    getter.fire(msg);

    REQUIRE_EQ(captured.size(), 4u);
    CHECK_EQ(captured[0], 0xDEu);
    CHECK_EQ(captured[3], 0xEFu);
  }

  TEST_CASE("listen callback fires for empty bytes without crash") {
    TestGetterImpl getter;
    bool called = false;
    size_t received_size = 99;

    getter.listen([&](const Bytes& data) {
      called = true;
      received_size = data.size();
    });

    Bytes empty;
    getter.fire(empty);

    CHECK(called);
    CHECK_EQ(received_size, 0u);
  }

  TEST_CASE("set_property and get_property round trip on getter") {
    TestGetterImpl getter;

    getter.set_property("field.name", "temperature");
    CHECK_EQ(getter.get_property("field.name"), "temperature");
  }

  TEST_CASE("get_property returns empty for unset key") {
    TestGetterImpl getter;

    CHECK(getter.get_property("unknown.key").empty());
  }

  TEST_CASE("interrupt and reset_interrupted work on getter") {
    TestGetterImpl getter;

    CHECK_FALSE(getter.is_interrupted());

    getter.interrupt();
    CHECK(getter.is_interrupted());

    getter.reset_interrupted();
    CHECK_FALSE(getter.is_interrupted());
  }

  TEST_CASE("large payload fires callback once with correct size") {
    static constexpr size_t kSize = 65536u;
    TestGetterImpl getter;
    size_t received_size = 0;

    getter.listen([&](const Bytes& data) { received_size = data.size(); });

    Bytes large = Bytes::create(kSize);
    getter.fire(large);

    CHECK_EQ(received_size, kSize);
  }

  TEST_CASE("get_all_properties returns map containing set keys") {
    TestGetterImpl getter;

    getter.set_property("x", "10");
    getter.set_property("y", "20");

    auto props = getter.get_all_properties();

    CHECK_EQ(props.count("x"), 1u);
    CHECK_EQ(props.count("y"), 1u);
  }
}

// NOLINTEND
