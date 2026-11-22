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

#ifdef VLINK_SUPPORT_MQTT

#include "./transports/mqtt_transport.h"

#include <doctest/doctest.h>

#include <sstream>
#include <string>

#include "../common_test.h"

TEST_SUITE("transports-MqttTransport") {
  TEST_CASE("default event domain qos and fragment when only address supplied") {
    MqttTransport transport("vehicle/speed");

    CHECK_EQ(transport.address, "vehicle/speed");
    CHECK(transport.event.empty());
    CHECK_EQ(transport.domain, 0);
    CHECK_EQ(transport.qos, 1);
    CHECK(transport.fragment.empty());
  }

  TEST_CASE("all parameters are stored") {
    MqttTransport transport("addr", "evt", 2, 0, "tcp://broker:1883");

    CHECK_EQ(transport.address, "addr");
    CHECK_EQ(transport.event, "evt");
    CHECK_EQ(transport.domain, 2);
    CHECK_EQ(transport.qos, 0);
    CHECK_EQ(transport.fragment, "tcp://broker:1883");
  }

  TEST_CASE("qos level 2 is stored") {
    MqttTransport transport("addr", "", 0, 2);

    CHECK_EQ(transport.qos, 2);
  }

  TEST_CASE("operator== holds when all fields match") {
    MqttTransport a("addr", "evt", 1, 2, "frag");
    MqttTransport b("addr", "evt", 1, 2, "frag");

    CHECK(a == b);
    CHECK_FALSE(a != b);
  }

  TEST_CASE("operator!= detects differing address") {
    MqttTransport a("addr_a");
    MqttTransport b("addr_b");

    CHECK(a != b);
    CHECK_FALSE(a == b);
  }

  TEST_CASE("operator!= detects differing event") {
    MqttTransport a("addr", "evt_a");
    MqttTransport b("addr", "evt_b");

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing domain") {
    MqttTransport a("addr", "", 0);
    MqttTransport b("addr", "", 1);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing qos") {
    MqttTransport a("addr", "", 0, 0);
    MqttTransport b("addr", "", 0, 2);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing fragment") {
    MqttTransport a("addr", "", 0, 1, "frag_a");
    MqttTransport b("addr", "", 0, 1, "frag_b");

    CHECK(a != b);
  }

  TEST_CASE("self equality") {
    MqttTransport a("addr", "evt", 1, 2, "frag");

    CHECK(a == a);
    CHECK_FALSE(a != a);
  }

  TEST_CASE("get_transport_type returns kMqtt") {
    MqttTransport transport("address");

    CHECK(transport.get_transport_type() == TransportType::kMqtt);
  }

  TEST_CASE("invalid values and stream output cover validation branches") {
    MqttTransport transport("addr", "evt", 3, 2, "tcp://broker");
    std::ostringstream oss;
    oss << transport;
    CHECK(oss.str().find("MqttTransport:") != std::string::npos);
    CHECK(oss.str().find("[fragment]tcp://broker") != std::string::npos);

    MqttTransport invalid_domain("addr");
    invalid_domain.domain = -1;
    CHECK_FALSE(invalid_domain.is_valid());

    MqttTransport invalid_qos_low("addr");
    invalid_qos_low.qos = -1;
    CHECK_FALSE(invalid_qos_low.is_valid());

    MqttTransport invalid_qos_high("addr");
    invalid_qos_high.qos = 3;
    CHECK_FALSE(invalid_qos_high.is_valid());

    MqttTransport empty_address("");
    CHECK_FALSE(empty_address.is_valid());
  }
}

#endif  // VLINK_SUPPORT_MQTT

// NOLINTEND
