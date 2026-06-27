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

#ifdef VLINK_SUPPORT_FDBUS

#include "./modules/fdbus_conf.h"

#include <doctest/doctest.h>

#include <string>

#include "../common_test.h"

TEST_SUITE("modules-FdbusConf") {
  TEST_CASE("default event and transport when only address supplied") {
    FdbusConf conf("my_service");

    CHECK_EQ(conf.address, "my_service");
    CHECK(conf.event.empty());
    CHECK_EQ(conf.transport, "svc");
  }

  TEST_CASE("event parameter is stored") {
    FdbusConf conf("my_service", "my_event");

    CHECK_EQ(conf.address, "my_service");
    CHECK_EQ(conf.event, "my_event");
    CHECK_EQ(conf.transport, "svc");
  }

  TEST_CASE("ipc transport mode is stored") {
    FdbusConf conf("addr", "", "ipc");

    CHECK_EQ(conf.transport, "ipc");
  }

  TEST_CASE("all three parameters are stored") {
    FdbusConf conf("my_address", "my_event", "ipc");

    CHECK_EQ(conf.address, "my_address");
    CHECK_EQ(conf.event, "my_event");
    CHECK_EQ(conf.transport, "ipc");
  }

  TEST_CASE("operator== holds when address and event match regardless of transport") {
    FdbusConf a("service", "event", "svc");
    FdbusConf b("service", "event", "ipc");

    CHECK(a == b);
    CHECK_FALSE(a != b);
  }

  TEST_CASE("operator!= detects differing address") {
    FdbusConf a("service_a");
    FdbusConf b("service_b");

    CHECK(a != b);
    CHECK_FALSE(a == b);
  }

  TEST_CASE("operator!= detects differing event") {
    FdbusConf a("service", "event_a");
    FdbusConf b("service", "event_b");

    CHECK(a != b);
  }

  TEST_CASE("equal address and event with same transport compare equal") {
    FdbusConf a("service", "event");
    FdbusConf b("service", "event");

    CHECK(a == b);
  }

  TEST_CASE("self equality") {
    FdbusConf a("service", "event", "svc");

    CHECK(a == a);
    CHECK_FALSE(a != a);
  }

  TEST_CASE("empty address and event compare equal") {
    FdbusConf a("");
    FdbusConf b("");

    CHECK(a == b);
  }

  TEST_CASE("get_transport_type returns kFdbus") {
    FdbusConf conf("addr");

    CHECK(conf.get_transport_type() == TransportType::kFdbus);
  }
}

#endif  // VLINK_SUPPORT_FDBUS

// NOLINTEND
