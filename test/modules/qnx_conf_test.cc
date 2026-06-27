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

#ifdef VLINK_SUPPORT_QNX

#include "./modules/qnx_conf.h"

#include <doctest/doctest.h>

#include <string>

#include "../common_test.h"

TEST_SUITE("modules-QnxConf") {
  TEST_CASE("default event when only address supplied") {
    QnxConf conf("my_channel");

    CHECK_EQ(conf.address, "my_channel");
    CHECK(conf.event.empty());
  }

  TEST_CASE("event parameter is stored") {
    QnxConf conf("my_channel", "my_event");

    CHECK_EQ(conf.address, "my_channel");
    CHECK_EQ(conf.event, "my_event");
  }

  TEST_CASE("operator== holds when both fields match") {
    QnxConf a("channel", "event");
    QnxConf b("channel", "event");

    CHECK(a == b);
    CHECK_FALSE(a != b);
  }

  TEST_CASE("operator!= detects differing address") {
    QnxConf a("channel_a");
    QnxConf b("channel_b");

    CHECK(a != b);
    CHECK_FALSE(a == b);
  }

  TEST_CASE("operator!= detects differing event") {
    QnxConf a("channel", "event_a");
    QnxConf b("channel", "event_b");

    CHECK(a != b);
  }

  TEST_CASE("empty event differs from non-empty event") {
    QnxConf a("channel");
    QnxConf b("channel", "event");

    CHECK(a != b);
  }

  TEST_CASE("both empty address and event compare equal") {
    QnxConf a("");
    QnxConf b("");

    CHECK(a == b);
  }

  TEST_CASE("self equality") {
    QnxConf a("channel", "event");

    CHECK(a == a);
    CHECK_FALSE(a != a);
  }

  TEST_CASE("get_transport_type returns kQnx") {
    QnxConf conf("channel");

    CHECK(conf.get_transport_type() == TransportType::kQnx);
  }
}

#endif  // VLINK_SUPPORT_QNX

// NOLINTEND
