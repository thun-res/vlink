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

#ifdef VLINK_SUPPORT_DDS

#include "./modules/dds_conf.h"

#include <doctest/doctest.h>

#include <string>

#include "../common_test.h"

TEST_SUITE("modules-DdsConf") {
  TEST_CASE("default domain depth and qos when only topic supplied") {
    DdsConf conf("vehicle/speed");

    CHECK_EQ(conf.topic, "vehicle/speed");
    CHECK_EQ(conf.domain, 0);
    CHECK_EQ(conf.depth, 0);
    CHECK(conf.qos.empty());
    CHECK(conf.qos_ext.empty());
  }

  TEST_CASE("explicit domain and depth are stored") {
    DdsConf conf("my_topic", 5, 10);

    CHECK_EQ(conf.topic, "my_topic");
    CHECK_EQ(conf.domain, 5);
    CHECK_EQ(conf.depth, 10);
    CHECK(conf.qos.empty());
  }

  TEST_CASE("named qos profile is stored") {
    DdsConf conf("my_topic", 2, 0, "fast_profile");

    CHECK_EQ(conf.qos, "fast_profile");
    CHECK(conf.qos_ext.empty());
  }

  TEST_CASE("qos_ext constructor stores property map") {
    DdsConf::PropertiesMap ext;
    ext["pub"] = "pub_profile";
    ext["sub"] = "sub_profile";

    DdsConf conf("my_topic", 1, ext);

    CHECK_EQ(conf.domain, 1);
    CHECK(conf.qos.empty());
    CHECK_FALSE(conf.qos_ext.empty());
    CHECK_EQ(conf.qos_ext.at("pub"), "pub_profile");
    CHECK_EQ(conf.qos_ext.at("sub"), "sub_profile");
  }

  TEST_CASE("operator== holds when all fields match") {
    DdsConf a("topic", 1, 5, "q");
    DdsConf b("topic", 1, 5, "q");

    CHECK(a == b);
    CHECK_FALSE(a != b);
  }

  TEST_CASE("operator!= detects differing topic") {
    DdsConf a("topic_a", 0, 0);
    DdsConf b("topic_b", 0, 0);

    CHECK(a != b);
    CHECK_FALSE(a == b);
  }

  TEST_CASE("operator!= detects differing domain") {
    DdsConf a("topic", 0, 0);
    DdsConf b("topic", 1, 0);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing depth") {
    DdsConf a("topic", 0, 1);
    DdsConf b("topic", 0, 2);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing qos name") {
    DdsConf a("topic", 0, 0, "qos_a");
    DdsConf b("topic", 0, 0, "qos_b");

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing qos_ext") {
    DdsConf::PropertiesMap ext_a;
    ext_a["pub"] = "x";
    DdsConf::PropertiesMap ext_b;
    ext_b["pub"] = "y";

    DdsConf a("topic", 0, ext_a);
    DdsConf b("topic", 0, ext_b);

    CHECK(a != b);
  }

  TEST_CASE("get_transport_type returns kDds") {
    DdsConf conf("topic");

    CHECK(conf.get_transport_type() == TransportType::kDds);
  }

  TEST_CASE("register_qos accepts a valid profile name") {
    Qos qos;
    qos.reliability.kind = Qos::Reliability::kReliable;

    CHECK_NOTHROW(DdsConf::register_qos("dds_test_profile", qos));
  }
}

#endif  // VLINK_SUPPORT_DDS

// NOLINTEND
