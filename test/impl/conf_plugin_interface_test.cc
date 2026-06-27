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

#include "./impl/conf_plugin_interface.h"

#include <doctest/doctest.h>

#include <memory>
#include <type_traits>

#include "../common_test.h"

namespace {

struct StubConf final : public Conf {
  TransportType transport{TransportType::kIntra};

  StubConf() = default;

  [[nodiscard]] bool is_valid() const override { return true; }

  [[nodiscard]] TransportType get_transport_type() const override { return transport; }
};

class FakeIntraPlugin final : public ConfPluginInterface {
 public:
  FakeIntraPlugin() = default;

  [[nodiscard]] TransportType get_transport_type() const override { return TransportType::kIntra; }

  [[nodiscard]] std::unique_ptr<Conf> create() const override {
    auto c = std::make_unique<StubConf>();
    c->transport = TransportType::kIntra;
    return c;
  }
};

class FakeZenohPlugin final : public ConfPluginInterface {
 public:
  FakeZenohPlugin() = default;

  [[nodiscard]] TransportType get_transport_type() const override { return TransportType::kZenoh; }

  [[nodiscard]] std::unique_ptr<Conf> create() const override {
    auto c = std::make_unique<StubConf>();
    c->transport = TransportType::kZenoh;
    return c;
  }
};

}  // namespace

TEST_SUITE("impl-ConfPluginInterface") {
  TEST_CASE("interface is abstract") { CHECK(std::is_abstract_v<ConfPluginInterface>); }

  TEST_CASE("interface is not copy-constructible") { CHECK_FALSE(std::is_copy_constructible_v<ConfPluginInterface>); }

  TEST_CASE("interface is not copy-assignable") { CHECK_FALSE(std::is_copy_assignable_v<ConfPluginInterface>); }

  TEST_CASE("get_transport_type returns the value declared by the subclass") {
    FakeIntraPlugin intra;
    FakeZenohPlugin zenoh;
    CHECK_EQ(intra.get_transport_type(), TransportType::kIntra);
    CHECK_EQ(zenoh.get_transport_type(), TransportType::kZenoh);
  }

  TEST_CASE("create returns a fresh independent Conf on each call") {
    FakeIntraPlugin plugin;
    auto a = plugin.create();
    auto b = plugin.create();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a.get() != b.get());
    CHECK_EQ(a->get_transport_type(), TransportType::kIntra);
    CHECK_EQ(b->get_transport_type(), TransportType::kIntra);
  }

  TEST_CASE("created Conf is valid and carries the correct transport type") {
    FakeZenohPlugin plugin;
    auto conf = plugin.create();
    REQUIRE(conf != nullptr);
    CHECK(conf->is_valid());
    CHECK_EQ(conf->get_transport_type(), TransportType::kZenoh);
  }

  TEST_CASE("virtual dispatch via base pointer routes to concrete implementation") {
    FakeZenohPlugin concrete;
    ConfPluginInterface* base = &concrete;
    auto conf = base->create();
    REQUIRE(conf != nullptr);
    CHECK_EQ(conf->get_transport_type(), TransportType::kZenoh);
    CHECK_EQ(base->get_transport_type(), TransportType::kZenoh);
  }

  TEST_CASE("two distinct plugin types produce Conf objects of different transport") {
    FakeIntraPlugin intra_plugin;
    FakeZenohPlugin zenoh_plugin;
    auto intra_conf = intra_plugin.create();
    auto zenoh_conf = zenoh_plugin.create();
    REQUIRE(intra_conf != nullptr);
    REQUIRE(zenoh_conf != nullptr);
    CHECK(intra_conf->get_transport_type() != zenoh_conf->get_transport_type());
  }
}

// NOLINTEND
