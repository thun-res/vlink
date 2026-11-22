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

#include "./impl/transport_plugin_interface.h"

#include <doctest/doctest.h>

#include <memory>
#include <type_traits>

#include "../common_test.h"

namespace {

struct StubTransport final : public Transport {
  TransportType transport{TransportType::kIntra};

  StubTransport() = default;

  [[nodiscard]] bool is_valid() const override { return true; }

  [[nodiscard]] TransportType get_transport_type() const override { return transport; }
};

class FakeIntraPlugin final : public TransportPluginInterface {
 public:
  FakeIntraPlugin() = default;

  [[nodiscard]] TransportType get_transport_type() const override { return TransportType::kIntra; }

  [[nodiscard]] std::unique_ptr<Transport> create() const override {
    auto c = std::make_unique<StubTransport>();
    c->transport = TransportType::kIntra;
    return c;
  }
};

class FakeZenohPlugin final : public TransportPluginInterface {
 public:
  FakeZenohPlugin() = default;

  [[nodiscard]] TransportType get_transport_type() const override { return TransportType::kZenoh; }

  [[nodiscard]] std::unique_ptr<Transport> create() const override {
    auto c = std::make_unique<StubTransport>();
    c->transport = TransportType::kZenoh;
    return c;
  }
};

}  // namespace

TEST_SUITE("impl-TransportPluginInterface") {
  TEST_CASE("interface is abstract") { CHECK(std::is_abstract_v<TransportPluginInterface>); }

  TEST_CASE("interface is not copy-constructible") {
    CHECK_FALSE(std::is_copy_constructible_v<TransportPluginInterface>);
  }

  TEST_CASE("interface is not copy-assignable") { CHECK_FALSE(std::is_copy_assignable_v<TransportPluginInterface>); }

  TEST_CASE("get_transport_type returns the value declared by the subclass") {
    FakeIntraPlugin intra;
    FakeZenohPlugin zenoh;
    CHECK_EQ(intra.get_transport_type(), TransportType::kIntra);
    CHECK_EQ(zenoh.get_transport_type(), TransportType::kZenoh);
  }

  TEST_CASE("create returns a fresh independent Transport on each call") {
    FakeIntraPlugin plugin;
    auto a = plugin.create();
    auto b = plugin.create();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a.get() != b.get());
    CHECK_EQ(a->get_transport_type(), TransportType::kIntra);
    CHECK_EQ(b->get_transport_type(), TransportType::kIntra);
  }

  TEST_CASE("created Transport is valid and carries the correct transport type") {
    FakeZenohPlugin plugin;
    auto transport = plugin.create();
    REQUIRE(transport != nullptr);
    CHECK(transport->is_valid());
    CHECK_EQ(transport->get_transport_type(), TransportType::kZenoh);
  }

  TEST_CASE("virtual dispatch via base pointer routes to concrete implementation") {
    FakeZenohPlugin concrete;
    TransportPluginInterface* base = &concrete;
    auto transport = base->create();
    REQUIRE(transport != nullptr);
    CHECK_EQ(transport->get_transport_type(), TransportType::kZenoh);
    CHECK_EQ(base->get_transport_type(), TransportType::kZenoh);
  }

  TEST_CASE("two distinct plugin types produce Transport objects of different transport types") {
    FakeIntraPlugin intra_plugin;
    FakeZenohPlugin zenoh_plugin;
    auto intra_transport = intra_plugin.create();
    auto zenoh_transport = zenoh_plugin.create();
    REQUIRE(intra_transport != nullptr);
    REQUIRE(zenoh_transport != nullptr);
    CHECK(intra_transport->get_transport_type() != zenoh_transport->get_transport_type());
  }
}

// NOLINTEND
