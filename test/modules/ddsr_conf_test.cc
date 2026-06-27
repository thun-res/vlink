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

#ifdef VLINK_SUPPORT_DDSR

#include "./modules/ddsr_conf.h"

#include <doctest/doctest.h>

#include <string>

#include "../common_test.h"

TEST_SUITE("modules-DdsrConf") {
  TEST_CASE("default domain depth and qos when only topic supplied") {
    DdsrConf conf("vehicle/speed");

    CHECK_EQ(conf.topic, "vehicle/speed");
    CHECK_EQ(conf.domain, 0);
    CHECK_EQ(conf.depth, 0);
    CHECK(conf.qos.empty());
    CHECK(conf.qos_ext.empty());
  }

  TEST_CASE("explicit domain and depth are stored") {
    DdsrConf conf("my_topic", 2, 15);

    CHECK_EQ(conf.topic, "my_topic");
    CHECK_EQ(conf.domain, 2);
    CHECK_EQ(conf.depth, 15);
    CHECK(conf.qos.empty());
  }

  TEST_CASE("named qos profile is stored") {
    DdsrConf conf("my_topic", 0, 0, "rti_qos");

    CHECK_EQ(conf.qos, "rti_qos");
    CHECK(conf.qos_ext.empty());
  }

  TEST_CASE("qos_ext constructor stores property map") {
    DdsrConf::PropertiesMap ext;
    ext["writer"] = "writer_profile";
    ext["reader"] = "reader_profile";

    DdsrConf conf("my_topic", 2, ext);

    CHECK_EQ(conf.topic, "my_topic");
    CHECK_EQ(conf.domain, 2);
    CHECK_EQ(conf.depth, 0);
    CHECK(conf.qos.empty());
    CHECK_FALSE(conf.qos_ext.empty());
    CHECK_EQ(conf.qos_ext.at("writer"), "writer_profile");
    CHECK_EQ(conf.qos_ext.at("reader"), "reader_profile");
  }

  TEST_CASE("operator== holds when all fields match") {
    DdsrConf a("topic", 1, 5, "q");
    DdsrConf b("topic", 1, 5, "q");

    CHECK(a == b);
    CHECK_FALSE(a != b);
  }

  TEST_CASE("operator!= detects differing topic") {
    DdsrConf a("topic_a");
    DdsrConf b("topic_b");

    CHECK(a != b);
    CHECK_FALSE(a == b);
  }

  TEST_CASE("operator!= detects differing domain") {
    DdsrConf a("topic", 0, 0);
    DdsrConf b("topic", 3, 0);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing depth") {
    DdsrConf a("topic", 0, 5);
    DdsrConf b("topic", 0, 10);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing qos name") {
    DdsrConf a("topic", 0, 0, "qos_a");
    DdsrConf b("topic", 0, 0, "qos_b");

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing qos_ext") {
    DdsrConf::PropertiesMap ext_a;
    ext_a["writer"] = "x";
    DdsrConf::PropertiesMap ext_b;
    ext_b["writer"] = "y";

    DdsrConf a("topic", 0, ext_a);
    DdsrConf b("topic", 0, ext_b);

    CHECK(a != b);
  }

  TEST_CASE("self equality") {
    DdsrConf a("topic", 1, 5, "qos");

    CHECK(a == a);
    CHECK_FALSE(a != a);
  }

  TEST_CASE("get_transport_type returns kDdsr") {
    DdsrConf conf("topic");

    CHECK(conf.get_transport_type() == TransportType::kDdsr);
  }

  TEST_CASE("register_qos accepts a valid profile name") {
    Qos qos;
    qos.reliability.kind = Qos::Reliability::kReliable;

    CHECK_NOTHROW(DdsrConf::register_qos("ddsr_test_profile", qos));
  }
}

#endif  // VLINK_SUPPORT_DDSR

// NOLINTEND
