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

#include "./transports/fdbus_transport.h"

#include <doctest/doctest.h>

#include <string>

#include "../common_test.h"

TEST_SUITE("transports-FdbusTransport") {
  TEST_CASE("default event and mode when only address supplied") {
    FdbusTransport transport("my_service");

    CHECK_EQ(transport.address, "my_service");
    CHECK(transport.event.empty());
    CHECK_EQ(transport.mode, "svc");
  }

  TEST_CASE("event parameter is stored") {
    FdbusTransport transport("my_service", "my_event");

    CHECK_EQ(transport.address, "my_service");
    CHECK_EQ(transport.event, "my_event");
    CHECK_EQ(transport.mode, "svc");
  }

  TEST_CASE("ipc mode is stored") {
    FdbusTransport transport("addr", "", "ipc");

    CHECK_EQ(transport.mode, "ipc");
  }

  TEST_CASE("all three parameters are stored") {
    FdbusTransport transport("my_address", "my_event", "ipc");

    CHECK_EQ(transport.address, "my_address");
    CHECK_EQ(transport.event, "my_event");
    CHECK_EQ(transport.mode, "ipc");
  }

  TEST_CASE("operator== holds when address and event match regardless of mode") {
    FdbusTransport a("service", "event", "svc");
    FdbusTransport b("service", "event", "ipc");

    CHECK(a == b);
    CHECK_FALSE(a != b);
  }

  TEST_CASE("operator!= detects differing address") {
    FdbusTransport a("service_a");
    FdbusTransport b("service_b");

    CHECK(a != b);
    CHECK_FALSE(a == b);
  }

  TEST_CASE("operator!= detects differing event") {
    FdbusTransport a("service", "event_a");
    FdbusTransport b("service", "event_b");

    CHECK(a != b);
  }

  TEST_CASE("equal address and event with same mode compare equal") {
    FdbusTransport a("service", "event");
    FdbusTransport b("service", "event");

    CHECK(a == b);
  }

  TEST_CASE("self equality") {
    FdbusTransport a("service", "event", "svc");

    CHECK(a == a);
    CHECK_FALSE(a != a);
  }

  TEST_CASE("empty address and event compare equal") {
    FdbusTransport a("");
    FdbusTransport b("");

    CHECK(a == b);
  }

  TEST_CASE("get_transport_type returns kFdbus") {
    FdbusTransport transport("addr");

    CHECK(transport.get_transport_type() == TransportType::kFdbus);
  }
}

#endif  // VLINK_SUPPORT_FDBUS

// NOLINTEND
