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

#ifdef VLINK_SUPPORT_SHM2

#include "./modules/shm2_conf.h"

#include <doctest/doctest.h>

#include <string>

#include "../common_test.h"

TEST_SUITE("modules-Shm2Conf") {
  TEST_CASE("kDefaultMemSize is 128 bytes") { CHECK_EQ(Shm2Conf::kDefaultMemSize, 128u); }

  TEST_CASE("kMaxMemSize is 32 mib") { CHECK_EQ(Shm2Conf::kMaxMemSize, 1024UL * 1024UL * 32UL); }

  TEST_CASE("all integer fields default correctly when only address supplied") {
    Shm2Conf conf("my_topic");

    CHECK_EQ(conf.address, "my_topic");
    CHECK(conf.event.empty());
    CHECK_EQ(conf.domain, 0);
    CHECK_EQ(conf.depth, 0);
    CHECK_EQ(conf.history, 0);
    CHECK_EQ(conf.wait, 0);
    CHECK_EQ(conf.size, Shm2Conf::kDefaultMemSize);
  }

  TEST_CASE("custom size is stored") {
    Shm2Conf conf("topic", "", 0, 0, 0, 0, 1024u * 1024u);

    CHECK_EQ(conf.size, 1024u * 1024u);
  }

  TEST_CASE("all parameters are stored") {
    Shm2Conf conf("addr", "evt", 2, 8, 3, 100, 512u);

    CHECK_EQ(conf.address, "addr");
    CHECK_EQ(conf.event, "evt");
    CHECK_EQ(conf.domain, 2);
    CHECK_EQ(conf.depth, 8);
    CHECK_EQ(conf.history, 3);
    CHECK_EQ(conf.wait, 100);
    CHECK_EQ(conf.size, 512u);
  }

  TEST_CASE("operator== holds when all seven fields match") {
    Shm2Conf a("addr", "evt", 0, 0, 0, 0, 256u);
    Shm2Conf b("addr", "evt", 0, 0, 0, 0, 256u);

    CHECK(a == b);
    CHECK_FALSE(a != b);
  }

  TEST_CASE("operator!= detects differing address") {
    Shm2Conf a("addr_a");
    Shm2Conf b("addr_b");

    CHECK(a != b);
    CHECK_FALSE(a == b);
  }

  TEST_CASE("operator!= detects differing event") {
    Shm2Conf a("addr", "evt_a");
    Shm2Conf b("addr", "evt_b");

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing domain") {
    Shm2Conf a("addr", "", 0);
    Shm2Conf b("addr", "", 1);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing depth") {
    Shm2Conf a("addr", "", 0, 1);
    Shm2Conf b("addr", "", 0, 2);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing history") {
    Shm2Conf a("addr", "", 0, 0, 0);
    Shm2Conf b("addr", "", 0, 0, 1);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing wait") {
    Shm2Conf a("addr", "", 0, 0, 0, 0);
    Shm2Conf b("addr", "", 0, 0, 0, 1);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing size") {
    Shm2Conf a("addr", "", 0, 0, 0, 0, 128u);
    Shm2Conf b("addr", "", 0, 0, 0, 0, 256u);

    CHECK(a != b);
  }

  TEST_CASE("self equality") {
    Shm2Conf a("addr", "evt", 1, 4, 2, 50, 512u);

    CHECK(a == a);
    CHECK_FALSE(a != a);
  }

  TEST_CASE("get_transport_type returns kShm2") {
    Shm2Conf conf("topic");

    CHECK(conf.get_transport_type() == TransportType::kShm2);
  }
}

#endif  // VLINK_SUPPORT_SHM2

// NOLINTEND
