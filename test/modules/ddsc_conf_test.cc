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

#include "./modules/ddsc_conf.h"

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
#include "./impl/conf.h"

TEST_SUITE("modules-DdscConf") {
  TEST_CASE("default domain depth and qos when only topic supplied") {
    DdscConf conf("vehicle/speed");

    CHECK_EQ(conf.topic, "vehicle/speed");
    CHECK_EQ(conf.domain, 0);
    CHECK_EQ(conf.depth, 0);
    CHECK(conf.qos.empty());
  }

  TEST_CASE("explicit domain and depth are stored") {
    DdscConf conf("my_topic", 3, 8);

    CHECK_EQ(conf.topic, "my_topic");
    CHECK_EQ(conf.domain, 3);
    CHECK_EQ(conf.depth, 8);
    CHECK(conf.qos.empty());
  }

  TEST_CASE("named qos profile is stored") {
    DdscConf conf("my_topic", 1, 0, "cyclone_profile");

    CHECK_EQ(conf.qos, "cyclone_profile");
  }

  TEST_CASE("operator== holds when all fields match") {
    DdscConf a("topic", 2, 4, "q");
    DdscConf b("topic", 2, 4, "q");

    CHECK(a == b);
    CHECK_FALSE(a != b);
  }

  TEST_CASE("operator!= detects differing topic") {
    DdscConf a("topic_a");
    DdscConf b("topic_b");

    CHECK(a != b);
    CHECK_FALSE(a == b);
  }

  TEST_CASE("operator!= detects differing domain") {
    DdscConf a("topic", 0, 0);
    DdscConf b("topic", 1, 0);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing depth") {
    DdscConf a("topic", 0, 1);
    DdscConf b("topic", 0, 2);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing qos name") {
    DdscConf a("topic", 0, 0, "qos_a");
    DdscConf b("topic", 0, 0, "qos_b");

    CHECK(a != b);
  }

  TEST_CASE("self equality") {
    DdscConf a("topic", 1, 5, "qos");

    CHECK(a == a);
    CHECK_FALSE(a != a);
  }

  TEST_CASE("get_transport_type returns kDdsc") {
    DdscConf conf("topic");

    CHECK(conf.get_transport_type() == TransportType::kDdsc);
  }

  TEST_CASE("register_qos accepts a valid profile name") {
    Qos qos;
    qos.reliability.kind = Qos::Reliability::kReliable;
    qos.valid = true;

    CHECK_NOTHROW(DdscConf::register_qos("ddsc_test_profile", qos));
    CHECK(DdscConf("topic", 0, 0, "ddsc_test_profile").is_valid());
  }

  TEST_CASE("register_qos rejects reserved and duplicate names") {
    Qos qos;
    qos.reliability.kind = Qos::Reliability::kReliable;

    CHECK_THROWS(DdscConf::register_qos("depth", qos));
    CHECK_NOTHROW(DdscConf::register_qos("ddsc_duplicate_profile_for_test", qos));
    CHECK_THROWS(DdscConf::register_qos("ddsc_duplicate_profile_for_test", qos));
  }

  TEST_CASE("invalid values qos miss and stream output cover branches") {
    DdscConf conf("topic", 2, 3, "qos_name");
    std::ostringstream oss;
    oss << conf;
    CHECK(oss.str().find("DdscConf:") != std::string::npos);
    CHECK(oss.str().find("[topic]topic") != std::string::npos);

    DdscConf empty_topic("");
    CHECK_FALSE(empty_topic.is_valid());

    DdscConf invalid_domain("topic");
    invalid_domain.domain = -1;
    CHECK_FALSE(invalid_domain.is_valid());

    DdscConf missing_qos("topic", 0, 0, "missing_ddsc_qos_for_test");
    CHECK(missing_qos.is_valid());
  }

  TEST_CASE("url parsing derives topic domain depth and qos") {
    Url nested("ddsc://vehicle/speed?domain=4&depth=6&qos=service");
    REQUIRE(nested.parse(kPublisher));
    const auto* nested_conf = static_cast<const DdscConf*>(nested.get_target());
    REQUIRE(nested_conf != nullptr);
    CHECK_EQ(nested_conf->topic, "vehicle/speed");
    CHECK_EQ(nested_conf->domain, 4);
    CHECK_EQ(nested_conf->depth, 6);
    CHECK_EQ(nested_conf->qos, "service");

    Url host_only("ddsc://vehicle_state");
    REQUIRE(host_only.parse(kSubscriber));
    const auto* host_conf = static_cast<const DdscConf*>(host_only.get_target());
    REQUIRE(host_conf != nullptr);
    CHECK_EQ(host_conf->topic, "vehicle_state");
    CHECK_EQ(host_conf->domain, 0);
    CHECK_EQ(host_conf->depth, 0);
    CHECK(host_conf->qos.empty());

    CHECK_FALSE(Url("ddsc://").parse(kPublisher));
    CHECK_THROWS_AS(Url("not-a-url").parse(kPublisher), std::runtime_error);
  }
}

#endif  // VLINK_SUPPORT_DDSC

// NOLINTEND
