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

#ifdef VLINK_SUPPORT_DDST

#include "./modules/ddst_conf.h"

#include <doctest/doctest.h>

#include <string>

#include "../common_test.h"

TEST_SUITE("modules-DdstConf") {
  TEST_CASE("default domain depth and qos when only topic supplied") {
    DdstConf conf("vehicle/speed");

    CHECK_EQ(conf.topic, "vehicle/speed");
    CHECK_EQ(conf.domain, 0);
    CHECK_EQ(conf.depth, 0);
    CHECK(conf.qos.empty());
    CHECK(conf.qos_ext.empty());
  }

  TEST_CASE("explicit domain and depth are stored") {
    DdstConf conf("my_topic", 4, 8);

    CHECK_EQ(conf.topic, "my_topic");
    CHECK_EQ(conf.domain, 4);
    CHECK_EQ(conf.depth, 8);
    CHECK(conf.qos.empty());
  }

  TEST_CASE("named qos profile is stored") {
    DdstConf conf("my_topic", 0, 0, "travo_qos");

    CHECK_EQ(conf.qos, "travo_qos");
    CHECK(conf.qos_ext.empty());
  }

  TEST_CASE("qos_ext constructor stores property map") {
    DdstConf::PropertiesMap ext;
    ext["pub"] = "pub_profile";
    ext["sub"] = "sub_profile";

    DdstConf conf("my_topic", 1, ext);

    CHECK_EQ(conf.topic, "my_topic");
    CHECK_EQ(conf.domain, 1);
    CHECK_EQ(conf.depth, 0);
    CHECK(conf.qos.empty());
    CHECK_FALSE(conf.qos_ext.empty());
    CHECK_EQ(conf.qos_ext.at("pub"), "pub_profile");
    CHECK_EQ(conf.qos_ext.at("sub"), "sub_profile");
  }

  TEST_CASE("operator== holds when all fields match") {
    DdstConf a("topic", 2, 10, "qos");
    DdstConf b("topic", 2, 10, "qos");

    CHECK(a == b);
    CHECK_FALSE(a != b);
  }

  TEST_CASE("operator!= detects differing topic") {
    DdstConf a("topic_a");
    DdstConf b("topic_b");

    CHECK(a != b);
    CHECK_FALSE(a == b);
  }

  TEST_CASE("operator!= detects differing domain") {
    DdstConf a("topic", 0, 0);
    DdstConf b("topic", 1, 0);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing depth") {
    DdstConf a("topic", 0, 5);
    DdstConf b("topic", 0, 10);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing qos name") {
    DdstConf a("topic", 0, 0, "qos_a");
    DdstConf b("topic", 0, 0, "qos_b");

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing qos_ext") {
    DdstConf::PropertiesMap ext_a;
    ext_a["pub"] = "profile1";
    DdstConf::PropertiesMap ext_b;
    ext_b["pub"] = "profile2";

    DdstConf a("topic", 0, ext_a);
    DdstConf b("topic", 0, ext_b);

    CHECK(a != b);
  }

  TEST_CASE("equal qos_ext compare equal") {
    DdstConf::PropertiesMap ext;
    ext["pub"] = "profile";

    DdstConf a("topic", 0, ext);
    DdstConf b("topic", 0, ext);

    CHECK(a == b);
  }

  TEST_CASE("self equality") {
    DdstConf a("topic", 1, 5, "qos");

    CHECK(a == a);
    CHECK_FALSE(a != a);
  }

  TEST_CASE("get_transport_type returns kDdst") {
    DdstConf conf("topic");

    CHECK(conf.get_transport_type() == TransportType::kDdst);
  }

  TEST_CASE("register_qos accepts a valid profile name") {
    Qos qos;
    qos.reliability.kind = Qos::Reliability::kReliable;

    CHECK_NOTHROW(DdstConf::register_qos("ddst_test_profile", qos));
  }
}

#endif  // VLINK_SUPPORT_DDST

// NOLINTEND
