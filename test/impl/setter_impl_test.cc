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

#include "./impl/setter_impl.h"

#include <doctest/doctest.h>

#include "../common_test.h"

namespace {

class TestSetter : public SetterImpl {
 public:
  TestSetter() = default;
  ~TestSetter() override = default;

  void init() override {}
  void deinit() override {}

  void write(const Bytes& msg) override {
    last_written = msg;
    ++write_count;
  }

  void sync(SyncCallback&& cb) override {
    ++sync_count;

    if (cb) {
      cb();
    }
  }

  Bytes last_written;
  int write_count{0};
  int sync_count{0};
};

}  // namespace

TEST_SUITE("impl-SetterImpl") {
  TEST_CASE("constructor sets kSetter role") {
    TestSetter setter;

    CHECK_EQ(setter.impl_type, kSetter);
  }

  TEST_CASE("write delegates to subclass and counts calls") {
    TestSetter setter;
    Bytes data = {1, 2, 3, 4};

    setter.write(data);

    CHECK_EQ(setter.write_count, 1);
    CHECK_EQ(setter.last_written.size(), 4u);
  }

  TEST_CASE("write with empty bytes is accepted") {
    TestSetter setter;
    Bytes empty;

    setter.write(empty);

    CHECK_EQ(setter.write_count, 1);
    CHECK(setter.last_written.empty());
  }

  TEST_CASE("multiple writes increment counter independently") {
    TestSetter setter;
    Bytes a = {0x01};
    Bytes b = {0x02, 0x03};

    setter.write(a);
    setter.write(b);

    CHECK_EQ(setter.write_count, 2);
    CHECK_EQ(setter.last_written.size(), 2u);
  }

  TEST_CASE("sync fires provided callback") {
    TestSetter setter;
    bool called = false;

    setter.sync([&] { called = true; });

    CHECK(called);
    CHECK_EQ(setter.sync_count, 1);
  }

  TEST_CASE("sync with null callback does not crash") {
    TestSetter setter;

    setter.sync(nullptr);

    CHECK_EQ(setter.sync_count, 1);
  }

  TEST_CASE("sync can be called multiple times with different callbacks") {
    TestSetter setter;
    int counter = 0;

    setter.sync([&] { ++counter; });
    setter.sync([&] { ++counter; });

    CHECK_EQ(counter, 2);
    CHECK_EQ(setter.sync_count, 2);
  }

  TEST_CASE("set_property and get_property work through SetterImpl") {
    TestSetter setter;

    setter.set_property("topic", "temperature");

    CHECK_EQ(setter.get_property("topic"), "temperature");
  }

  TEST_CASE("interrupt and reset_interrupted work through SetterImpl") {
    TestSetter setter;

    CHECK_FALSE(setter.is_interrupted());

    setter.interrupt();
    CHECK(setter.is_interrupted());

    setter.reset_interrupted();
    CHECK_FALSE(setter.is_interrupted());
  }

  TEST_CASE("impl_type is kSetter not other roles") {
    TestSetter setter;

    CHECK_NE(setter.impl_type, kPublisher);
    CHECK_NE(setter.impl_type, kSubscriber);
    CHECK_NE(setter.impl_type, kServer);
    CHECK_NE(setter.impl_type, kClient);
    CHECK_EQ(setter.impl_type, kSetter);
  }

  TEST_CASE("get_property returns empty for unset key") {
    TestSetter setter;

    CHECK(setter.get_property("unset.key").empty());
  }

  TEST_CASE("get_all_properties returns map containing set keys") {
    TestSetter setter;

    setter.set_property("a", "1");
    setter.set_property("b", "2");

    auto props = setter.get_all_properties();

    CHECK_EQ(props.count("a"), 1u);
    CHECK_EQ(props.count("b"), 1u);
    CHECK_EQ(props.at("a"), "1");
    CHECK_EQ(props.at("b"), "2");
  }

  TEST_CASE("write large payload stores correct size") {
    static constexpr size_t kSize = 65536u;
    TestSetter setter;

    Bytes large = Bytes::create(kSize);
    setter.write(large);

    CHECK_EQ(setter.last_written.size(), kSize);
    CHECK_EQ(setter.write_count, 1);
  }

  TEST_CASE("sync called sequentially accumulates sync count") {
    TestSetter setter;
    int total = 0;

    for (int i = 0; i < 5; ++i) {
      setter.sync([&] { ++total; });
    }

    CHECK_EQ(total, 5);
    CHECK_EQ(setter.sync_count, 5);
  }

  TEST_CASE("write followed by sync delivers both operations") {
    TestSetter setter;
    bool synced = false;

    setter.write(Bytes{0xAB});
    setter.sync([&] { synced = true; });

    CHECK_EQ(setter.write_count, 1);
    CHECK(synced);
    CHECK_EQ(setter.sync_count, 1);
  }
}

// NOLINTEND
