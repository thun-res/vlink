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

#include "./impl/transport.h"

#include <doctest/doctest.h>

#include <map>
#include <string>
#include <type_traits>

#include "../common_test.h"

namespace {

struct TrivialTransport final : public Transport {
  TrivialTransport() = default;
};

}  // namespace

TEST_SUITE("impl-Transport") {
  TEST_CASE("default is_valid returns false") {
    TrivialTransport transport;
    CHECK_FALSE(transport.is_valid());
  }

  TEST_CASE("default get_transport_type returns kUnknown") {
    TrivialTransport transport;
    CHECK_EQ(transport.get_transport_type(), TransportType::kUnknown);
  }

  TEST_CASE("default get_impl_type returns kUnknownImplType before parse") {
    TrivialTransport transport;
    CHECK_EQ(transport.get_impl_type(), kUnknownImplType);
  }

  TEST_CASE("hash_code initialises to zero") {
    TrivialTransport transport;
    CHECK_EQ(transport.hash_code, 0u);
  }

  TEST_CASE("hash_code is writable") {
    TrivialTransport transport;
    transport.hash_code = 0xCAFEBABEu;
    CHECK_EQ(transport.hash_code, 0xCAFEBABEu);
  }

  TEST_CASE("parse caches the requested impl_type") {
    SUBCASE("kPublisher") {
      TrivialTransport transport;
      REQUIRE(transport.parse(kPublisher));
      CHECK_EQ(transport.get_impl_type(), kPublisher);
    }
    SUBCASE("kSubscriber") {
      TrivialTransport transport;
      REQUIRE(transport.parse(kSubscriber));
      CHECK_EQ(transport.get_impl_type(), kSubscriber);
    }
    SUBCASE("kSetter") {
      TrivialTransport transport;
      REQUIRE(transport.parse(kSetter));
      CHECK_EQ(transport.get_impl_type(), kSetter);
    }
    SUBCASE("kGetter") {
      TrivialTransport transport;
      REQUIRE(transport.parse(kGetter));
      CHECK_EQ(transport.get_impl_type(), kGetter);
    }
    SUBCASE("kServer") {
      TrivialTransport transport;
      REQUIRE(transport.parse(kServer));
      CHECK_EQ(transport.get_impl_type(), kServer);
    }
    SUBCASE("kClient") {
      TrivialTransport transport;
      REQUIRE(transport.parse(kClient));
      CHECK_EQ(transport.get_impl_type(), kClient);
    }
  }

  TEST_CASE("successive parse calls update the cached impl_type to the latest") {
    TrivialTransport transport;
    REQUIRE(transport.parse(kPublisher));
    REQUIRE(transport.parse(kServer));
    CHECK_EQ(transport.get_impl_type(), kServer);
  }

  TEST_CASE("PropertiesMap is std::map of string to string") {
    CHECK((std::is_same_v<Transport::PropertiesMap, std::map<std::string, std::string>>));
  }

  TEST_CASE("PropertiesMap holds arbitrary key value pairs") {
    Transport::PropertiesMap m;
    m["dds.ip"] = "127.0.0.1";
    m["zenoh.mode"] = "peer";
    CHECK_EQ(m.at("dds.ip"), "127.0.0.1");
    CHECK_EQ(m.at("zenoh.mode"), "peer");
  }

  TEST_CASE("Transport is copy-constructible as a value-type config") {
    CHECK(std::is_copy_constructible_v<TrivialTransport>);
  }
}

// NOLINTEND
