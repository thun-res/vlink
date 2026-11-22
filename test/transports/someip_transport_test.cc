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

#ifdef VLINK_SUPPORT_SOMEIP

#include "./transports/someip_transport.h"

#include <doctest/doctest.h>

#include <set>
#include <sstream>
#include <string>

#include "../common_test.h"

TEST_SUITE("transports-SomeipTransport") {
  TEST_CASE("rpc constructor stores service instance and method") {
    SomeipTransport transport(0x1234, 0x5678, 0x0001);

    CHECK_EQ(transport.service, 0x1234);
    CHECK_EQ(transport.instance, 0x5678);
    CHECK_EQ(transport.method, 0x0001);
    CHECK(transport.groups.empty());
    CHECK_EQ(transport.event, 0);
    CHECK_EQ(transport.field, false);
  }

  TEST_CASE("event constructor stores service instance groups and event") {
    SomeipTransport::Groups groups = {0x0001, 0x0002};
    SomeipTransport transport(0x1234, 0x5678, groups, 0x0010, false);

    CHECK_EQ(transport.service, 0x1234);
    CHECK_EQ(transport.instance, 0x5678);
    CHECK_EQ(transport.groups, groups);
    CHECK_EQ(transport.event, 0x0010);
    CHECK_EQ(transport.field, false);
    CHECK_EQ(transport.method, 0);
  }

  TEST_CASE("field constructor stores field flag as true") {
    SomeipTransport::Groups groups = {0x0001};
    SomeipTransport transport(0x1234, 0x5678, groups, 0x0020, true);

    CHECK_EQ(transport.field, true);
    CHECK_EQ(transport.event, 0x0020);
  }

  TEST_CASE("groups set stores all provided group ids") {
    SomeipTransport::Groups groups = {0x0001, 0x0002, 0x0003};
    SomeipTransport transport(0x1, 0x1, groups, 0x01);

    CHECK_EQ(transport.groups.size(), 3u);
    CHECK_EQ(transport.groups.count(0x0001), 1u);
    CHECK_EQ(transport.groups.count(0x0002), 1u);
    CHECK_EQ(transport.groups.count(0x0003), 1u);
  }

  TEST_CASE("single group is stored correctly") {
    SomeipTransport::Groups groups = {0x0005};
    SomeipTransport transport(0x1, 0x1, groups, 0x02);

    CHECK_EQ(transport.groups.size(), 1u);
    CHECK_EQ(transport.groups.count(0x0005), 1u);
  }

  TEST_CASE("operator== holds for equal rpc configs") {
    SomeipTransport a(0x1234, 0x5678, 0x0001);
    SomeipTransport b(0x1234, 0x5678, 0x0001);

    CHECK(a == b);
    CHECK_FALSE(a != b);
  }

  TEST_CASE("operator== holds for equal event configs") {
    SomeipTransport::Groups g = {0x0001};

    SomeipTransport a(0x1234, 0x5678, g, 0x0010, false);
    SomeipTransport b(0x1234, 0x5678, g, 0x0010, false);

    CHECK(a == b);
  }

  TEST_CASE("operator!= detects differing service") {
    SomeipTransport a(0x0001, 0x0002, 0x0003);
    SomeipTransport b(0x0002, 0x0002, 0x0003);

    CHECK(a != b);
    CHECK_FALSE(a == b);
  }

  TEST_CASE("operator!= detects differing instance") {
    SomeipTransport a(0x0001, 0x0001, 0x0003);
    SomeipTransport b(0x0001, 0x0002, 0x0003);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing method") {
    SomeipTransport a(0x0001, 0x0002, 0x0003);
    SomeipTransport b(0x0001, 0x0002, 0x0004);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing field flag") {
    SomeipTransport::Groups g = {0x0001};

    SomeipTransport a(0x1234, 0x5678, g, 0x0010, false);
    SomeipTransport b(0x1234, 0x5678, g, 0x0010, true);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing event id") {
    SomeipTransport::Groups g = {0x0001};

    SomeipTransport a(0x1, 0x1, g, 0x0010, false);
    SomeipTransport b(0x1, 0x1, g, 0x0020, false);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing groups") {
    SomeipTransport::Groups g1 = {0x0001};
    SomeipTransport::Groups g2 = {0x0002};

    SomeipTransport a(0x1, 0x1, g1, 0x01, false);
    SomeipTransport b(0x1, 0x1, g2, 0x01, false);

    CHECK(a != b);
  }

  TEST_CASE("self equality for rpc config") {
    SomeipTransport a(0x1234, 0x5678, 0x0001);

    CHECK(a == a);
    CHECK_FALSE(a != a);
  }

  TEST_CASE("get_transport_type returns kSomeip") {
    SomeipTransport transport(0x0001, 0x0001, 0x0001);

    CHECK(transport.get_transport_type() == TransportType::kSomeip);
  }

  TEST_CASE("invalid configs url parsing and stream output cover branches") {
    SomeipTransport::Groups groups = {0x0001, 0x0002};
    SomeipTransport transport(0x1234, 0x5678, groups, 0x0099, true);

    std::ostringstream transport_stream;
    transport_stream << transport;
    CHECK(transport_stream.str().find("SomeipTransport:") != std::string::npos);
    CHECK(transport_stream.str().find("0x0001") != std::string::npos);
    CHECK(transport_stream.str().find("|") != std::string::npos);
    CHECK(transport_stream.str().find("[field]1") != std::string::npos);

    CHECK_FALSE(SomeipTransport(0, 1, 1).is_valid());
    CHECK_FALSE(SomeipTransport(1, 0, 1).is_valid());

    SomeipTransport pub_without_group(1, 1, SomeipTransport::Groups{}, 0, false);
    CHECK(pub_without_group.parse(kPublisher));
    CHECK_FALSE(pub_without_group.is_valid());

    SomeipTransport setter_without_field(1, 1, groups, 1, false);
    CHECK(setter_without_field.parse(kSetter));
    CHECK_FALSE(setter_without_field.is_valid());

    Url rpc("someip://4660/22136?method=1");
    CHECK(rpc.parse(kClient));

    Url event("someip://4660/22136?groups=1|2||3&event=16&field=1");
    REQUIRE(event.parse(kGetter));
    const auto* parsed = static_cast<const SomeipTransport*>(event.get_target());
    REQUIRE(parsed != nullptr);
    CHECK_EQ(parsed->groups.size(), 3u);
    CHECK_EQ(parsed->event, 16);
    CHECK(parsed->field);

    CHECK_FALSE(Url("someip://4660").parse(kClient));
    CHECK_FALSE(Url("someip:///22136?method=1").parse(kClient));
  }
}

#endif  // VLINK_SUPPORT_SOMEIP

// NOLINTEND
