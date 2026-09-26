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

#include "./impl/abstract_factory.h"

#include <doctest/doctest.h>

#include <memory>

#include "../common_test.h"

namespace {

class FactoryTestNode final : public NodeImpl {
 public:
  FactoryTestNode() : NodeImpl(kSubscriber) {}

  void init() override {}
  void deinit() override {}
};

class FactoryTestObject final : public AbstractObject<int> {
 public:
  FactoryTestObject() = default;
  ~FactoryTestObject() override = default;
};

}  // namespace

TEST_SUITE("impl-AbstractFactory") {
  TEST_CASE("owner dispatch preserves mutable callback state and cancels after removal") {
    FactoryTestObject object;
    FactoryTestNode owner;
    REQUIRE(object.add_impl(&owner));
    int value = 0;
    REQUIRE(object.register_msg_callback(&owner, [state = 0, &value](const Bytes&) mutable { value = ++state; }));
    CHECK(object.invoke_msg_callback(&owner, Bytes{}));
    CHECK(object.invoke_msg_callback(&owner, Bytes{}));
    CHECK_EQ(value, 2);
    CHECK(object.remove_impl(&owner));
    CHECK_FALSE(object.invoke_msg_callback(&owner, Bytes{}));
    CHECK_FALSE(object.invoke_callback(&owner, [&] { value = -1; }));
    CHECK_EQ(value, 2);
  }

  TEST_CASE("owner dispatch keeps a self-removed callable alive until it returns") {
    FactoryTestObject object;
    FactoryTestNode owner;
    REQUIRE(object.add_impl(&owner));
    auto lifetime = std::make_shared<int>(7);
    std::weak_ptr<int> weak_lifetime = lifetime;
    bool retained = false;
    REQUIRE(object.register_msg_callback(&owner, [&, lifetime](const Bytes&) {
      object.remove_impl(&owner);
      retained = !weak_lifetime.expired() && *lifetime == 7;
    }));
    lifetime.reset();
    CHECK(object.invoke_msg_callback(&owner, Bytes{}));
    CHECK(retained);
    CHECK(weak_lifetime.expired());
  }

  TEST_CASE("owner dispatch visits the request handler of the registered owner only") {
    FactoryTestObject object;
    FactoryTestNode owner;
    FactoryTestNode other;
    REQUIRE(object.add_impl(&owner));
    uint64_t seen_seq = 0;
    REQUIRE(object.register_req_resp_callback(&owner, [&seen_seq](uint64_t seq, const Bytes&, Bytes* response) {
      seen_seq = seq;
      if (response) {
        *response = Bytes::from_string("ack");
      }
    }));
    NodeImpl* visited = nullptr;
    Bytes response;
    CHECK(object.invoke_req_resp_callback(&owner, [&](NodeImpl* impl, const auto& handler) {
      visited = impl;
      handler(42, Bytes{}, &response);
    }));
    CHECK_EQ(visited, &owner);
    CHECK_EQ(seen_seq, 42U);
    CHECK_EQ(response.size(), 3U);
    CHECK_FALSE(object.invoke_req_resp_callback(&other, [&](NodeImpl*, const auto&) { seen_seq = 0; }));
    CHECK_EQ(seen_seq, 42U);
  }
}

// NOLINTEND
