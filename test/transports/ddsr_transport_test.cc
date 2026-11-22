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

#include "./transports/ddsr_transport.h"

#include <doctest/doctest.h>

#include <string>

#include "../common_test.h"

TEST_SUITE("transports-DdsrTransport") {
  TEST_CASE("default domain depth and qos when only topic supplied") {
    DdsrTransport transport("vehicle/speed");

    CHECK_EQ(transport.topic, "vehicle/speed");
    CHECK_EQ(transport.domain, 0);
    CHECK_EQ(transport.depth, 0);
    CHECK(transport.qos.empty());
    CHECK(transport.qos_ext.empty());
  }

  TEST_CASE("explicit domain and depth are stored") {
    DdsrTransport transport("my_topic", 2, 15);

    CHECK_EQ(transport.topic, "my_topic");
    CHECK_EQ(transport.domain, 2);
    CHECK_EQ(transport.depth, 15);
    CHECK(transport.qos.empty());
  }

  TEST_CASE("named qos profile is stored") {
    DdsrTransport transport("my_topic", 0, 0, "rti_qos");

    CHECK_EQ(transport.qos, "rti_qos");
    CHECK(transport.qos_ext.empty());
  }

  TEST_CASE("qos_ext constructor stores property map") {
    DdsrTransport::PropertiesMap ext;
    ext["writer"] = "writer_profile";
    ext["reader"] = "reader_profile";

    DdsrTransport transport("my_topic", 2, ext);

    CHECK_EQ(transport.topic, "my_topic");
    CHECK_EQ(transport.domain, 2);
    CHECK_EQ(transport.depth, 0);
    CHECK(transport.qos.empty());
    CHECK_FALSE(transport.qos_ext.empty());
    CHECK_EQ(transport.qos_ext.at("writer"), "writer_profile");
    CHECK_EQ(transport.qos_ext.at("reader"), "reader_profile");
  }

  TEST_CASE("operator== holds when all fields match") {
    DdsrTransport a("topic", 1, 5, "q");
    DdsrTransport b("topic", 1, 5, "q");

    CHECK(a == b);
    CHECK_FALSE(a != b);
  }

  TEST_CASE("operator!= detects differing topic") {
    DdsrTransport a("topic_a");
    DdsrTransport b("topic_b");

    CHECK(a != b);
    CHECK_FALSE(a == b);
  }

  TEST_CASE("operator!= detects differing domain") {
    DdsrTransport a("topic", 0, 0);
    DdsrTransport b("topic", 3, 0);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing depth") {
    DdsrTransport a("topic", 0, 5);
    DdsrTransport b("topic", 0, 10);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing qos name") {
    DdsrTransport a("topic", 0, 0, "qos_a");
    DdsrTransport b("topic", 0, 0, "qos_b");

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing qos_ext") {
    DdsrTransport::PropertiesMap ext_a;
    ext_a["writer"] = "x";
    DdsrTransport::PropertiesMap ext_b;
    ext_b["writer"] = "y";

    DdsrTransport a("topic", 0, ext_a);
    DdsrTransport b("topic", 0, ext_b);

    CHECK(a != b);
  }

  TEST_CASE("self equality") {
    DdsrTransport a("topic", 1, 5, "qos");

    CHECK(a == a);
    CHECK_FALSE(a != a);
  }

  TEST_CASE("get_transport_type returns kDdsr") {
    DdsrTransport transport("topic");

    CHECK(transport.get_transport_type() == TransportType::kDdsr);
  }

  TEST_CASE("register_qos accepts a valid profile name") {
    Qos qos;
    qos.reliability.kind = Qos::Reliability::kReliable;

    CHECK_NOTHROW(DdsrTransport::register_qos("ddsr_test_profile", qos));
  }
}

#endif  // VLINK_SUPPORT_DDSR

// NOLINTEND
