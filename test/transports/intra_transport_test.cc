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

#ifdef VLINK_SUPPORT_INTRA

#include "./transports/intra_transport.h"

#include <doctest/doctest.h>

#include <sstream>
#include <string>

#include "../common_test.h"

TEST_SUITE("transports-IntraTransport") {
  TEST_CASE("default event pipeline and type when only address supplied") {
    IntraTransport transport("my_topic");

    CHECK_EQ(transport.address, "my_topic");
    CHECK(transport.event.empty());
    CHECK_EQ(transport.pipeline, 0);
    CHECK_EQ(transport.type, "queue");
  }

  TEST_CASE("event parameter is stored") {
    IntraTransport transport("my_service", "my_event");

    CHECK_EQ(transport.address, "my_service");
    CHECK_EQ(transport.event, "my_event");
    CHECK_EQ(transport.pipeline, 0);
    CHECK_EQ(transport.type, "queue");
  }

  TEST_CASE("all parameters are stored") {
    IntraTransport transport("topic", "evt", 4, "direct");

    CHECK_EQ(transport.address, "topic");
    CHECK_EQ(transport.event, "evt");
    CHECK_EQ(transport.pipeline, 4);
    CHECK_EQ(transport.type, "direct");
  }

  TEST_CASE("pipeline zero is the default") {
    IntraTransport transport("addr", "");

    CHECK_EQ(transport.pipeline, 0);
  }

  TEST_CASE("queue is the default type") {
    IntraTransport transport("addr");

    CHECK_EQ(transport.type, "queue");
  }

  TEST_CASE("operator== holds when all fields match") {
    IntraTransport a("topic", "event", 2, "queue");
    IntraTransport b("topic", "event", 2, "queue");

    CHECK(a == b);
    CHECK_FALSE(a != b);
  }

  TEST_CASE("operator!= detects differing address") {
    IntraTransport a("topic_a");
    IntraTransport b("topic_b");

    CHECK(a != b);
    CHECK_FALSE(a == b);
  }

  TEST_CASE("operator!= detects differing event") {
    IntraTransport a("topic", "event1");
    IntraTransport b("topic", "event2");

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing pipeline") {
    IntraTransport a("topic", "", 0);
    IntraTransport b("topic", "", 4);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing type") {
    IntraTransport a("topic", "", 0, "queue");
    IntraTransport b("topic", "", 0, "direct");

    CHECK(a != b);
  }

  TEST_CASE("self equality") {
    IntraTransport a("topic", "event", 2, "direct");

    CHECK(a == a);
    CHECK_FALSE(a != a);
  }

  TEST_CASE("get_transport_type returns kIntra") {
    IntraTransport transport("topic");

    CHECK(transport.get_transport_type() == TransportType::kIntra);
  }

  TEST_CASE("invalid configs url fragments and stream output cover branches") {
    IntraTransport transport("addr", "evt", 2, "direct");
    std::ostringstream oss;
    oss << transport;
    CHECK(oss.str().find("IntraTransport:") != std::string::npos);
    CHECK(oss.str().find("[pipeline]2") != std::string::npos);

    CHECK_FALSE(IntraTransport("").is_valid());
    CHECK_FALSE(IntraTransport("addr", "", 0, "bad").is_valid());

    Url direct_url("intra://svc/path?event=evt&pipeline=3#direct");
    REQUIRE(direct_url.parse(kPublisher));
    const auto* parsed = static_cast<const IntraTransport*>(direct_url.get_target());
    REQUIRE(parsed != nullptr);
    CHECK_EQ(parsed->address, "svc/path");
    CHECK_EQ(parsed->event, "evt");
    CHECK_EQ(parsed->pipeline, 3);
    CHECK_EQ(parsed->type, "direct");

    CHECK_FALSE(Url("intra://").parse(kPublisher));
  }
}

#endif  // VLINK_SUPPORT_INTRA

// NOLINTEND
