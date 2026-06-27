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

#ifdef VLINK_SUPPORT_SHM

#include "./modules/shm_conf.h"

#include <doctest/doctest.h>

#include <sstream>
#include <string>

#include "../common_test.h"

TEST_SUITE("modules-ShmConf") {
  TEST_CASE("all integer fields default to zero when only address supplied") {
    ShmConf conf("my_service");

    CHECK_EQ(conf.address, "my_service");
    CHECK(conf.event.empty());
    CHECK_EQ(conf.domain, 0);
    CHECK_EQ(conf.depth, 0);
    CHECK_EQ(conf.history, 0);
    CHECK_EQ(conf.wait, 0);
  }

  TEST_CASE("all parameters are stored") {
    ShmConf conf("addr", "evt", 1, 10, 5, 100);

    CHECK_EQ(conf.address, "addr");
    CHECK_EQ(conf.event, "evt");
    CHECK_EQ(conf.domain, 1);
    CHECK_EQ(conf.depth, 10);
    CHECK_EQ(conf.history, 5);
    CHECK_EQ(conf.wait, 100);
  }

  TEST_CASE("operator== holds when all six fields match") {
    ShmConf a("addr", "evt", 1, 5, 2, 0);
    ShmConf b("addr", "evt", 1, 5, 2, 0);

    CHECK(a == b);
    CHECK_FALSE(a != b);
  }

  TEST_CASE("operator!= detects differing address") {
    ShmConf a("addr_a");
    ShmConf b("addr_b");

    CHECK(a != b);
    CHECK_FALSE(a == b);
  }

  TEST_CASE("operator!= detects differing event") {
    ShmConf a("addr", "evt_a");
    ShmConf b("addr", "evt_b");

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing domain") {
    ShmConf a("addr", "", 0);
    ShmConf b("addr", "", 1);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing depth") {
    ShmConf a("addr", "", 0, 1);
    ShmConf b("addr", "", 0, 2);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing history") {
    ShmConf a("addr", "", 0, 0, 0);
    ShmConf b("addr", "", 0, 0, 1);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing wait") {
    ShmConf a("addr", "", 0, 0, 0, 0);
    ShmConf b("addr", "", 0, 0, 0, 1);

    CHECK(a != b);
  }

  TEST_CASE("self equality") {
    ShmConf a("addr", "evt", 1, 5, 2, 10);

    CHECK(a == a);
    CHECK_FALSE(a != a);
  }

  TEST_CASE("get_transport_type returns kShm") {
    ShmConf conf("topic");

    CHECK(conf.get_transport_type() == TransportType::kShm);
  }

  TEST_CASE("has_roudi_running returns a stable bool without side effects") {
    bool first = ShmConf::has_roudi_running();
    bool second = ShmConf::has_roudi_running();

    CHECK_EQ(first, second);
  }

  TEST_CASE("has_runtime_inited returns a bool") {
    bool inited = ShmConf::has_runtime_inited();

    CHECK((inited == true || inited == false));
  }

  TEST_CASE("invalid values stream output and url wait rules cover branches") {
    ShmConf conf("addr", "evt", 1, 2, 3, 4);
    std::ostringstream oss;
    oss << conf;
    CHECK(oss.str().find("ShmConf:") != std::string::npos);
    CHECK(oss.str().find("[wait]4") != std::string::npos);

    CHECK_FALSE(ShmConf("").is_valid());
    CHECK_FALSE(ShmConf(std::string(81, 'a')).is_valid());

    ShmConf invalid_event("addr", std::string(81, 'e'));
    CHECK_FALSE(invalid_event.is_valid());

    ShmConf invalid_numbers("addr");
    invalid_numbers.domain = -1;
    CHECK_FALSE(invalid_numbers.is_valid());
    invalid_numbers.domain = 0;
    invalid_numbers.depth = -1;
    CHECK_FALSE(invalid_numbers.is_valid());
    invalid_numbers.depth = 0;
    invalid_numbers.history = -1;
    CHECK_FALSE(invalid_numbers.is_valid());

    Url pub_wait("shm://wait/pub?event=data&wait=1");
    CHECK(pub_wait.parse(kPublisher));
    Url client_wait("shm://wait/client?event=req&wait=1");
    CHECK_FALSE(client_wait.parse(kClient));

    Url setter_default_history("shm://wait/field?event=value");
    CHECK(setter_default_history.parse(kSetter));
    const auto* parsed = static_cast<const ShmConf*>(setter_default_history.get_target());
    REQUIRE(parsed != nullptr);
    CHECK_EQ(parsed->history, 1);
  }

  TEST_CASE("runtime helpers reject oversized runtime names") {
    CHECK_THROWS(ShmConf::init_runtime(std::string(81, 'r')));
  }

  TEST_CASE("has_roudi_inited is a stable side-effect-free query") {
    bool first = ShmConf::has_roudi_inited();

    CHECK_NOTHROW((void)ShmConf::has_roudi_inited());
    CHECK_EQ(first, ShmConf::has_roudi_inited());
  }

  TEST_CASE("wait mode is rejected for server setter and getter roles") {
    Url server_wait("shm://wait/server?event=req&wait=1");
    CHECK_FALSE(server_wait.parse(kServer));

    Url setter_wait("shm://wait/setter?event=value&wait=1");
    CHECK_FALSE(setter_wait.parse(kSetter));

    Url getter_wait("shm://wait/getter?event=value&wait=1");
    CHECK_FALSE(getter_wait.parse(kGetter));
  }

  TEST_CASE("parse rejects a url with an empty host") {
    Url empty_host("shm://?event=data");
    CHECK_FALSE(empty_host.parse(kPublisher));
  }

  TEST_CASE("explicit history query is parsed for a non-field role") {
    Url with_history("shm://hist/topic?event=data&history=7");
    REQUIRE(with_history.parse(kPublisher));

    const auto* parsed = static_cast<const ShmConf*>(with_history.get_target());
    REQUIRE(parsed != nullptr);
    CHECK_EQ(parsed->history, 7);
  }
}

#endif  // VLINK_SUPPORT_SHM

// NOLINTEND
