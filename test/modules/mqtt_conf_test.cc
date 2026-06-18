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

#include "./modules/mqtt_conf.h"

#include <doctest/doctest.h>

#include <string>

#include "../common_test.h"

TEST_SUITE("modules-MqttConf") {
  TEST_CASE("default event domain qos and fragment when only address supplied") {
    MqttConf conf("vehicle/speed");

    CHECK_EQ(conf.address, "vehicle/speed");
    CHECK(conf.event.empty());
    CHECK_EQ(conf.domain, 0);
    CHECK_EQ(conf.qos, 1);
    CHECK(conf.fragment.empty());
  }

  TEST_CASE("all parameters are stored") {
    MqttConf conf("addr", "evt", 2, 0, "tcp://broker:1883");

    CHECK_EQ(conf.address, "addr");
    CHECK_EQ(conf.event, "evt");
    CHECK_EQ(conf.domain, 2);
    CHECK_EQ(conf.qos, 0);
    CHECK_EQ(conf.fragment, "tcp://broker:1883");
  }

  TEST_CASE("qos level 2 is stored") {
    MqttConf conf("addr", "", 0, 2);

    CHECK_EQ(conf.qos, 2);
  }

  TEST_CASE("operator== holds when all fields match") {
    MqttConf a("addr", "evt", 1, 2, "frag");
    MqttConf b("addr", "evt", 1, 2, "frag");

    CHECK(a == b);
    CHECK_FALSE(a != b);
  }

  TEST_CASE("operator!= detects differing address") {
    MqttConf a("addr_a");
    MqttConf b("addr_b");

    CHECK(a != b);
    CHECK_FALSE(a == b);
  }

  TEST_CASE("operator!= detects differing event") {
    MqttConf a("addr", "evt_a");
    MqttConf b("addr", "evt_b");

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing domain") {
    MqttConf a("addr", "", 0);
    MqttConf b("addr", "", 1);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing qos") {
    MqttConf a("addr", "", 0, 0);
    MqttConf b("addr", "", 0, 2);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing fragment") {
    MqttConf a("addr", "", 0, 1, "frag_a");
    MqttConf b("addr", "", 0, 1, "frag_b");

    CHECK(a != b);
  }

  TEST_CASE("self equality") {
    MqttConf a("addr", "evt", 1, 2, "frag");

    CHECK(a == a);
    CHECK_FALSE(a != a);
  }

  TEST_CASE("get_transport_type returns kMqtt") {
    MqttConf conf("address");

    CHECK(conf.get_transport_type() == TransportType::kMqtt);
  }
}

#endif  // VLINK_SUPPORT_MQTT

// NOLINTEND
