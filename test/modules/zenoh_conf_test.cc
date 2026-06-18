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

#ifdef VLINK_SUPPORT_ZENOH

#include "./modules/zenoh_conf.h"

#include <doctest/doctest.h>

#include <map>
#include <string>

#include "../common_test.h"

TEST_SUITE("modules-ZenohConf") {
  TEST_CASE("default event domain qos and fragment when only address supplied") {
    ZenohConf conf("vehicle/speed");

    CHECK_EQ(conf.address, "vehicle/speed");
    CHECK(conf.event.empty());
    CHECK_EQ(conf.domain, 0);
    CHECK_EQ(conf.depth, 0);
    CHECK(conf.qos.empty());
    CHECK(conf.fragment.empty());
    CHECK(conf.shm.empty());
    CHECK(conf.shm_mode.empty());
    CHECK(conf.shm_size.empty());
    CHECK(conf.shm_threshold.empty());
    CHECK(conf.shm_loan_threshold.empty());
    CHECK(conf.shm_blocking.empty());
  }

  TEST_CASE("all constructor parameters are stored") {
    ZenohConf conf("addr", "evt", 2, "my_qos", "frag");

    CHECK_EQ(conf.address, "addr");
    CHECK_EQ(conf.event, "evt");
    CHECK_EQ(conf.domain, 2);
    CHECK_EQ(conf.qos, "my_qos");
    CHECK_EQ(conf.fragment, "frag");
  }

  TEST_CASE("shm fields are empty after construction") {
    ZenohConf conf("addr", "evt", 1, "qos", "frag");

    CHECK(conf.shm.empty());
    CHECK(conf.shm_mode.empty());
    CHECK(conf.shm_size.empty());
    CHECK(conf.shm_threshold.empty());
    CHECK(conf.shm_loan_threshold.empty());
    CHECK(conf.shm_blocking.empty());
  }

  TEST_CASE("operator== holds when all fields match") {
    ZenohConf a("addr", "evt", 1, "qos", "frag");
    ZenohConf b("addr", "evt", 1, "qos", "frag");

    CHECK(a == b);
    CHECK_FALSE(a != b);
  }

  TEST_CASE("operator!= detects differing address") {
    ZenohConf a("addr_a");
    ZenohConf b("addr_b");

    CHECK(a != b);
    CHECK_FALSE(a == b);
  }

  TEST_CASE("operator!= detects differing event") {
    ZenohConf a("addr", "evt_a");
    ZenohConf b("addr", "evt_b");

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing domain") {
    ZenohConf a("addr", "", 0);
    ZenohConf b("addr", "", 1);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing qos") {
    ZenohConf a("addr", "", 0, "qos_a");
    ZenohConf b("addr", "", 0, "qos_b");

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing fragment") {
    ZenohConf a("addr", "", 0, "", "frag_a");
    ZenohConf b("addr", "", 0, "", "frag_b");

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing shm field") {
    ZenohConf a("addr");
    ZenohConf b("addr");
    a.shm = "true";

    CHECK(a != b);
  }

  TEST_CASE("self equality") {
    ZenohConf a("addr", "evt", 1, "qos", "frag");

    CHECK(a == a);
    CHECK_FALSE(a != a);
  }

  TEST_CASE("get_transport_type returns kZenoh") {
    ZenohConf conf("address");

    CHECK(conf.get_transport_type() == TransportType::kZenoh);
  }

  TEST_CASE("append_properties adds shm entries when set") {
    ZenohConf conf("addr");
    conf.shm = "true";
    conf.shm_mode = "lazy";
    conf.shm_size = "64M";

    ZenohConf::PropertiesMap props;
    conf.append_properties(props);

    CHECK_EQ(props.at("zenoh.shm"), "true");
    CHECK_EQ(props.at("zenoh.shm_mode"), "lazy");
    CHECK_EQ(props.at("zenoh.shm_size"), "64M");
  }

  TEST_CASE("append_properties skips empty shm fields") {
    ZenohConf conf("addr");

    ZenohConf::PropertiesMap props;
    conf.append_properties(props);

    CHECK(props.find("zenoh.shm") == props.end());
    CHECK(props.find("zenoh.shm_mode") == props.end());
    CHECK(props.find("zenoh.shm_size") == props.end());
    CHECK(props.find("zenoh.shm_threshold") == props.end());
    CHECK(props.find("zenoh.shm_loan_threshold") == props.end());
    CHECK(props.find("zenoh.shm_blocking") == props.end());
  }

  TEST_CASE("append_properties adds shm_threshold and loan_threshold when set") {
    ZenohConf conf("addr");
    conf.shm_threshold = "4096";
    conf.shm_loan_threshold = "1024";
    conf.shm_blocking = "false";

    ZenohConf::PropertiesMap props;
    conf.append_properties(props);

    CHECK_EQ(props.at("zenoh.shm_threshold"), "4096");
    CHECK_EQ(props.at("zenoh.shm_loan_threshold"), "1024");
    CHECK_EQ(props.at("zenoh.shm_blocking"), "false");
  }

  TEST_CASE("register_qos accepts a valid profile name") {
    Qos qos;
    qos.reliability.kind = Qos::Reliability::kReliable;

    CHECK_NOTHROW(ZenohConf::register_qos("zenoh_test_profile", qos));
  }
}

#endif  // VLINK_SUPPORT_ZENOH

// NOLINTEND
