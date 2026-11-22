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

#ifdef VLINK_SUPPORT_DDSC

#include "./transports/ddsc_transport.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <shared_mutex>
#include <sstream>
#include <string>

#include "../common_test.h"
#include "./extension/qos.h"
#include "./impl/transport.h"

TEST_SUITE("transports-DdscTransport") {
  TEST_CASE("default domain depth and qos when only topic supplied") {
    DdscTransport transport("vehicle/speed");

    CHECK_EQ(transport.topic, "vehicle/speed");
    CHECK_EQ(transport.domain, 0);
    CHECK_EQ(transport.depth, 0);
    CHECK(transport.qos.empty());
  }

  TEST_CASE("explicit domain and depth are stored") {
    DdscTransport transport("my_topic", 3, 8);

    CHECK_EQ(transport.topic, "my_topic");
    CHECK_EQ(transport.domain, 3);
    CHECK_EQ(transport.depth, 8);
    CHECK(transport.qos.empty());
  }

  TEST_CASE("named qos profile is stored") {
    DdscTransport transport("my_topic", 1, 0, "cyclone_profile");

    CHECK_EQ(transport.qos, "cyclone_profile");
  }

  TEST_CASE("operator== holds when all fields match") {
    DdscTransport a("topic", 2, 4, "q");
    DdscTransport b("topic", 2, 4, "q");

    CHECK(a == b);
    CHECK_FALSE(a != b);
  }

  TEST_CASE("operator!= detects differing topic") {
    DdscTransport a("topic_a");
    DdscTransport b("topic_b");

    CHECK(a != b);
    CHECK_FALSE(a == b);
  }

  TEST_CASE("operator!= detects differing domain") {
    DdscTransport a("topic", 0, 0);
    DdscTransport b("topic", 1, 0);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing depth") {
    DdscTransport a("topic", 0, 1);
    DdscTransport b("topic", 0, 2);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing qos name") {
    DdscTransport a("topic", 0, 0, "qos_a");
    DdscTransport b("topic", 0, 0, "qos_b");

    CHECK(a != b);
  }

  TEST_CASE("self equality") {
    DdscTransport a("topic", 1, 5, "qos");

    CHECK(a == a);
    CHECK_FALSE(a != a);
  }

  TEST_CASE("get_transport_type returns kDdsc") {
    DdscTransport transport("topic");

    CHECK(transport.get_transport_type() == TransportType::kDdsc);
  }

  TEST_CASE("register_qos accepts a valid profile name") {
    Qos qos;
    qos.reliability.kind = Qos::Reliability::kReliable;
    qos.valid = true;

    CHECK_NOTHROW(DdscTransport::register_qos("ddsc_test_profile", qos));
    CHECK(DdscTransport("topic", 0, 0, "ddsc_test_profile").is_valid());
  }

  TEST_CASE("register_qos rejects reserved and duplicate names") {
    Qos qos;
    qos.reliability.kind = Qos::Reliability::kReliable;

    CHECK_THROWS(DdscTransport::register_qos("depth", qos));
    CHECK_NOTHROW(DdscTransport::register_qos("ddsc_duplicate_profile_for_test", qos));
    CHECK_THROWS(DdscTransport::register_qos("ddsc_duplicate_profile_for_test", qos));
  }

  TEST_CASE("invalid values qos miss and stream output cover branches") {
    DdscTransport transport("topic", 2, 3, "qos_name");
    std::ostringstream oss;
    oss << transport;
    CHECK(oss.str().find("DdscTransport:") != std::string::npos);
    CHECK(oss.str().find("[topic]topic") != std::string::npos);

    DdscTransport empty_topic("");
    CHECK_FALSE(empty_topic.is_valid());

    DdscTransport invalid_domain("topic");
    invalid_domain.domain = -1;
    CHECK_FALSE(invalid_domain.is_valid());

    DdscTransport missing_qos("topic", 0, 0, "missing_ddsc_qos_for_test");
    CHECK(missing_qos.is_valid());
  }

  TEST_CASE("url parsing derives topic domain depth and qos") {
    Url nested("ddsc://vehicle/speed?domain=4&depth=6&qos=service");
    REQUIRE(nested.parse(kPublisher));
    const auto* nested_transport = static_cast<const DdscTransport*>(nested.get_target());
    REQUIRE(nested_transport != nullptr);
    CHECK_EQ(nested_transport->topic, "vehicle/speed");
    CHECK_EQ(nested_transport->domain, 4);
    CHECK_EQ(nested_transport->depth, 6);
    CHECK_EQ(nested_transport->qos, "service");

    Url host_only("ddsc://vehicle_state");
    REQUIRE(host_only.parse(kSubscriber));
    const auto* host_transport = static_cast<const DdscTransport*>(host_only.get_target());
    REQUIRE(host_transport != nullptr);
    CHECK_EQ(host_transport->topic, "vehicle_state");
    CHECK_EQ(host_transport->domain, 0);
    CHECK_EQ(host_transport->depth, 0);
    CHECK(host_transport->qos.empty());

    CHECK_FALSE(Url("ddsc://").parse(kPublisher));
    CHECK_THROWS_AS(Url("not-a-url").parse(kPublisher), std::runtime_error);
  }
}

#endif  // VLINK_SUPPORT_DDSC

// NOLINTEND
