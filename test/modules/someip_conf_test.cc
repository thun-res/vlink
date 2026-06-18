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

#include "./modules/someip_conf.h"

#include <doctest/doctest.h>

#include <set>
#include <string>

#include "../common_test.h"

TEST_SUITE("modules-SomeipConf") {
  TEST_CASE("rpc constructor stores service instance and method") {
    SomeipConf conf(0x1234, 0x5678, 0x0001);

    CHECK_EQ(conf.service, 0x1234);
    CHECK_EQ(conf.instance, 0x5678);
    CHECK_EQ(conf.method, 0x0001);
    CHECK(conf.groups.empty());
    CHECK_EQ(conf.event, 0);
    CHECK_EQ(conf.field, false);
  }

  TEST_CASE("event constructor stores service instance groups and event") {
    SomeipConf::Groups groups = {0x0001, 0x0002};
    SomeipConf conf(0x1234, 0x5678, groups, 0x0010, false);

    CHECK_EQ(conf.service, 0x1234);
    CHECK_EQ(conf.instance, 0x5678);
    CHECK_EQ(conf.groups, groups);
    CHECK_EQ(conf.event, 0x0010);
    CHECK_EQ(conf.field, false);
    CHECK_EQ(conf.method, 0);
  }

  TEST_CASE("field constructor stores field flag as true") {
    SomeipConf::Groups groups = {0x0001};
    SomeipConf conf(0x1234, 0x5678, groups, 0x0020, true);

    CHECK_EQ(conf.field, true);
    CHECK_EQ(conf.event, 0x0020);
  }

  TEST_CASE("groups set stores all provided group ids") {
    SomeipConf::Groups groups = {0x0001, 0x0002, 0x0003};
    SomeipConf conf(0x1, 0x1, groups, 0x01);

    CHECK_EQ(conf.groups.size(), 3u);
    CHECK_EQ(conf.groups.count(0x0001), 1u);
    CHECK_EQ(conf.groups.count(0x0002), 1u);
    CHECK_EQ(conf.groups.count(0x0003), 1u);
  }

  TEST_CASE("single group is stored correctly") {
    SomeipConf::Groups groups = {0x0005};
    SomeipConf conf(0x1, 0x1, groups, 0x02);

    CHECK_EQ(conf.groups.size(), 1u);
    CHECK_EQ(conf.groups.count(0x0005), 1u);
  }

  TEST_CASE("operator== holds for equal rpc configs") {
    SomeipConf a(0x1234, 0x5678, 0x0001);
    SomeipConf b(0x1234, 0x5678, 0x0001);

    CHECK(a == b);
    CHECK_FALSE(a != b);
  }

  TEST_CASE("operator== holds for equal event configs") {
    SomeipConf::Groups g = {0x0001};

    SomeipConf a(0x1234, 0x5678, g, 0x0010, false);
    SomeipConf b(0x1234, 0x5678, g, 0x0010, false);

    CHECK(a == b);
  }

  TEST_CASE("operator!= detects differing service") {
    SomeipConf a(0x0001, 0x0002, 0x0003);
    SomeipConf b(0x0002, 0x0002, 0x0003);

    CHECK(a != b);
    CHECK_FALSE(a == b);
  }

  TEST_CASE("operator!= detects differing instance") {
    SomeipConf a(0x0001, 0x0001, 0x0003);
    SomeipConf b(0x0001, 0x0002, 0x0003);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing method") {
    SomeipConf a(0x0001, 0x0002, 0x0003);
    SomeipConf b(0x0001, 0x0002, 0x0004);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing field flag") {
    SomeipConf::Groups g = {0x0001};

    SomeipConf a(0x1234, 0x5678, g, 0x0010, false);
    SomeipConf b(0x1234, 0x5678, g, 0x0010, true);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing event id") {
    SomeipConf::Groups g = {0x0001};

    SomeipConf a(0x1, 0x1, g, 0x0010, false);
    SomeipConf b(0x1, 0x1, g, 0x0020, false);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing groups") {
    SomeipConf::Groups g1 = {0x0001};
    SomeipConf::Groups g2 = {0x0002};

    SomeipConf a(0x1, 0x1, g1, 0x01, false);
    SomeipConf b(0x1, 0x1, g2, 0x01, false);

    CHECK(a != b);
  }

  TEST_CASE("self equality for rpc config") {
    SomeipConf a(0x1234, 0x5678, 0x0001);

    CHECK(a == a);
    CHECK_FALSE(a != a);
  }

  TEST_CASE("get_transport_type returns kSomeip") {
    SomeipConf conf(0x0001, 0x0001, 0x0001);

    CHECK(conf.get_transport_type() == TransportType::kSomeip);
  }
}

#endif  // VLINK_SUPPORT_SOMEIP

// NOLINTEND
