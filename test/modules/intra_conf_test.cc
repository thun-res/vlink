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

#include "./modules/intra_conf.h"

#include <doctest/doctest.h>

#include <string>

#include "../common_test.h"

TEST_SUITE("modules-IntraConf") {
  TEST_CASE("default event pipeline and type when only address supplied") {
    IntraConf conf("my_topic");

    CHECK_EQ(conf.address, "my_topic");
    CHECK(conf.event.empty());
    CHECK_EQ(conf.pipeline, 0);
    CHECK_EQ(conf.type, "queue");
  }

  TEST_CASE("event parameter is stored") {
    IntraConf conf("my_service", "my_event");

    CHECK_EQ(conf.address, "my_service");
    CHECK_EQ(conf.event, "my_event");
    CHECK_EQ(conf.pipeline, 0);
    CHECK_EQ(conf.type, "queue");
  }

  TEST_CASE("all parameters are stored") {
    IntraConf conf("topic", "evt", 4, "direct");

    CHECK_EQ(conf.address, "topic");
    CHECK_EQ(conf.event, "evt");
    CHECK_EQ(conf.pipeline, 4);
    CHECK_EQ(conf.type, "direct");
  }

  TEST_CASE("pipeline zero is the default") {
    IntraConf conf("addr", "");

    CHECK_EQ(conf.pipeline, 0);
  }

  TEST_CASE("queue is the default type") {
    IntraConf conf("addr");

    CHECK_EQ(conf.type, "queue");
  }

  TEST_CASE("operator== holds when all fields match") {
    IntraConf a("topic", "event", 2, "queue");
    IntraConf b("topic", "event", 2, "queue");

    CHECK(a == b);
    CHECK_FALSE(a != b);
  }

  TEST_CASE("operator!= detects differing address") {
    IntraConf a("topic_a");
    IntraConf b("topic_b");

    CHECK(a != b);
    CHECK_FALSE(a == b);
  }

  TEST_CASE("operator!= detects differing event") {
    IntraConf a("topic", "event1");
    IntraConf b("topic", "event2");

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing pipeline") {
    IntraConf a("topic", "", 0);
    IntraConf b("topic", "", 4);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing type") {
    IntraConf a("topic", "", 0, "queue");
    IntraConf b("topic", "", 0, "direct");

    CHECK(a != b);
  }

  TEST_CASE("self equality") {
    IntraConf a("topic", "event", 2, "direct");

    CHECK(a == a);
    CHECK_FALSE(a != a);
  }

  TEST_CASE("get_transport_type returns kIntra") {
    IntraConf conf("topic");

    CHECK(conf.get_transport_type() == TransportType::kIntra);
  }
}

#endif  // VLINK_SUPPORT_INTRA

// NOLINTEND
