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

#include "./impl/conf.h"

#include <doctest/doctest.h>

#include <map>
#include <string>
#include <type_traits>

#include "../common_test.h"

namespace {

struct TrivialConf final : public Conf {
  TrivialConf() = default;
};

}  // namespace

TEST_SUITE("impl-Conf") {
  TEST_CASE("default is_valid returns false") {
    TrivialConf conf;
    CHECK_FALSE(conf.is_valid());
  }

  TEST_CASE("default get_transport_type returns kUnknown") {
    TrivialConf conf;
    CHECK_EQ(conf.get_transport_type(), TransportType::kUnknown);
  }

  TEST_CASE("default get_impl_type returns kUnknownImplType before parse") {
    TrivialConf conf;
    CHECK_EQ(conf.get_impl_type(), kUnknownImplType);
  }

  TEST_CASE("hash_code initialises to zero") {
    TrivialConf conf;
    CHECK_EQ(conf.hash_code, 0u);
  }

  TEST_CASE("hash_code is writable") {
    TrivialConf conf;
    conf.hash_code = 0xCAFEBABEu;
    CHECK_EQ(conf.hash_code, 0xCAFEBABEu);
  }

  TEST_CASE("parse caches the requested impl_type") {
    SUBCASE("kPublisher") {
      TrivialConf conf;
      REQUIRE(conf.parse(kPublisher));
      CHECK_EQ(conf.get_impl_type(), kPublisher);
    }
    SUBCASE("kSubscriber") {
      TrivialConf conf;
      REQUIRE(conf.parse(kSubscriber));
      CHECK_EQ(conf.get_impl_type(), kSubscriber);
    }
    SUBCASE("kSetter") {
      TrivialConf conf;
      REQUIRE(conf.parse(kSetter));
      CHECK_EQ(conf.get_impl_type(), kSetter);
    }
    SUBCASE("kGetter") {
      TrivialConf conf;
      REQUIRE(conf.parse(kGetter));
      CHECK_EQ(conf.get_impl_type(), kGetter);
    }
    SUBCASE("kServer") {
      TrivialConf conf;
      REQUIRE(conf.parse(kServer));
      CHECK_EQ(conf.get_impl_type(), kServer);
    }
    SUBCASE("kClient") {
      TrivialConf conf;
      REQUIRE(conf.parse(kClient));
      CHECK_EQ(conf.get_impl_type(), kClient);
    }
  }

  TEST_CASE("successive parse calls update the cached impl_type to the latest") {
    TrivialConf conf;
    REQUIRE(conf.parse(kPublisher));
    REQUIRE(conf.parse(kServer));
    CHECK_EQ(conf.get_impl_type(), kServer);
  }

  TEST_CASE("PropertiesMap is std::map of string to string") {
    CHECK((std::is_same_v<Conf::PropertiesMap, std::map<std::string, std::string>>));
  }

  TEST_CASE("PropertiesMap holds arbitrary key value pairs") {
    Conf::PropertiesMap m;
    m["dds.ip"] = "127.0.0.1";
    m["zenoh.mode"] = "peer";
    CHECK_EQ(m.at("dds.ip"), "127.0.0.1");
    CHECK_EQ(m.at("zenoh.mode"), "peer");
  }

  TEST_CASE("Conf is copy-constructible as a value-type config") { CHECK(std::is_copy_constructible_v<TrivialConf>); }
}

// NOLINTEND
