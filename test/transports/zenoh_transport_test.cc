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

#include "./transports/zenoh_transport.h"

#include <doctest/doctest.h>

#include <map>
#include <sstream>
#include <string>

#include "../common_test.h"

TEST_SUITE("transports-ZenohTransport") {
  TEST_CASE("default event domain qos and fragment when only address supplied") {
    ZenohTransport transport("vehicle/speed");

    CHECK_EQ(transport.address, "vehicle/speed");
    CHECK(transport.event.empty());
    CHECK_EQ(transport.domain, 0);
    CHECK_EQ(transport.depth, 0);
    CHECK(transport.qos.empty());
    CHECK(transport.fragment.empty());
    CHECK(transport.shm.empty());
    CHECK(transport.shm_mode.empty());
    CHECK(transport.shm_size.empty());
    CHECK(transport.shm_threshold.empty());
    CHECK(transport.shm_loan_threshold.empty());
    CHECK(transport.shm_blocking.empty());
  }

  TEST_CASE("all constructor parameters are stored") {
    ZenohTransport transport("addr", "evt", 2, "my_qos", "frag");

    CHECK_EQ(transport.address, "addr");
    CHECK_EQ(transport.event, "evt");
    CHECK_EQ(transport.domain, 2);
    CHECK_EQ(transport.qos, "my_qos");
    CHECK_EQ(transport.fragment, "frag");
  }

  TEST_CASE("shm fields are empty after construction") {
    ZenohTransport transport("addr", "evt", 1, "qos", "frag");

    CHECK(transport.shm.empty());
    CHECK(transport.shm_mode.empty());
    CHECK(transport.shm_size.empty());
    CHECK(transport.shm_threshold.empty());
    CHECK(transport.shm_loan_threshold.empty());
    CHECK(transport.shm_blocking.empty());
  }

  TEST_CASE("operator== holds when all fields match") {
    ZenohTransport a("addr", "evt", 1, "qos", "frag");
    ZenohTransport b("addr", "evt", 1, "qos", "frag");

    CHECK(a == b);
    CHECK_FALSE(a != b);
  }

  TEST_CASE("operator!= detects differing address") {
    ZenohTransport a("addr_a");
    ZenohTransport b("addr_b");

    CHECK(a != b);
    CHECK_FALSE(a == b);
  }

  TEST_CASE("operator!= detects differing event") {
    ZenohTransport a("addr", "evt_a");
    ZenohTransport b("addr", "evt_b");

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing domain") {
    ZenohTransport a("addr", "", 0);
    ZenohTransport b("addr", "", 1);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing qos") {
    ZenohTransport a("addr", "", 0, "qos_a");
    ZenohTransport b("addr", "", 0, "qos_b");

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing fragment") {
    ZenohTransport a("addr", "", 0, "", "frag_a");
    ZenohTransport b("addr", "", 0, "", "frag_b");

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing shm field") {
    ZenohTransport a("addr");
    ZenohTransport b("addr");
    a.shm = "true";

    CHECK(a != b);
  }

  TEST_CASE("self equality") {
    ZenohTransport a("addr", "evt", 1, "qos", "frag");

    CHECK(a == a);
    CHECK_FALSE(a != a);
  }

  TEST_CASE("get_transport_type returns kZenoh") {
    ZenohTransport transport("address");

    CHECK(transport.get_transport_type() == TransportType::kZenoh);
  }

  TEST_CASE("append_properties adds shm entries when set") {
    ZenohTransport transport("addr");
    transport.shm = "true";
    transport.shm_mode = "lazy";
    transport.shm_size = "64M";

    ZenohTransport::PropertiesMap props;
    transport.append_properties(props);

    CHECK_EQ(props.at("zenoh.shm"), "true");
    CHECK_EQ(props.at("zenoh.shm_mode"), "lazy");
    CHECK_EQ(props.at("zenoh.shm_size"), "64M");
  }

  TEST_CASE("append_properties skips empty shm fields") {
    ZenohTransport transport("addr");

    ZenohTransport::PropertiesMap props;
    transport.append_properties(props);

    CHECK(props.find("zenoh.shm") == props.end());
    CHECK(props.find("zenoh.shm_mode") == props.end());
    CHECK(props.find("zenoh.shm_size") == props.end());
    CHECK(props.find("zenoh.shm_threshold") == props.end());
    CHECK(props.find("zenoh.shm_loan_threshold") == props.end());
    CHECK(props.find("zenoh.shm_blocking") == props.end());
  }

  TEST_CASE("append_properties adds shm_threshold and loan_threshold when set") {
    ZenohTransport transport("addr");
    transport.shm_threshold = "4096";
    transport.shm_loan_threshold = "1024";
    transport.shm_blocking = "false";

    ZenohTransport::PropertiesMap props;
    transport.append_properties(props);

    CHECK_EQ(props.at("zenoh.shm_threshold"), "4096");
    CHECK_EQ(props.at("zenoh.shm_loan_threshold"), "1024");
    CHECK_EQ(props.at("zenoh.shm_blocking"), "false");
  }

  TEST_CASE("register_qos accepts a valid profile name") {
    Qos qos;
    qos.valid = true;
    qos.reliability.kind = Qos::Reliability::kReliable;

    CHECK_NOTHROW(ZenohTransport::register_qos("zenoh_test_profile", qos));
  }

  TEST_CASE("invalid values qos miss and stream output cover branches") {
    ZenohTransport transport("addr", "evt", 3, "missing_qos", "tcp");
    transport.shm = "true";
    transport.shm_mode = "lazy";
    transport.shm_size = "2M";
    transport.shm_threshold = "4K";
    transport.shm_loan_threshold = "1K";
    transport.shm_blocking = "false";

    std::ostringstream oss;
    oss << transport;
    CHECK(oss.str().find("ZenohTransport:") != std::string::npos);
    CHECK(oss.str().find("[shm]true") != std::string::npos);
    CHECK(oss.str().find("[shm_blocking]false") != std::string::npos);

    CHECK_FALSE(ZenohTransport("").is_valid());

    ZenohTransport invalid_domain("addr");
    invalid_domain.domain = -1;
    CHECK_FALSE(invalid_domain.is_valid());

    ZenohTransport invalid_depth("addr");
    invalid_depth.depth = -1;
    CHECK_FALSE(invalid_depth.is_valid());

    CHECK(transport.is_valid());
  }
}

#endif  // VLINK_SUPPORT_ZENOH

// NOLINTEND
