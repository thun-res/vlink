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

#include "./zerocopy/header.h"

#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <type_traits>

#include "../common_test.h"

TEST_SUITE("zerocopy-Header") {
  TEST_CASE("default construction initialises all fields to zero or known defaults") {
    zerocopy::Header h;

    CHECK_EQ(h.seq, 0u);
    CHECK_EQ(std::string(h.frame_id), "unknown");
    CHECK_EQ(h.time_meas, 0u);
    CHECK_EQ(h.time_pub, 0u);
    CHECK_EQ(h.reserved, 0u);
  }

  TEST_CASE("sizeof is exactly 40 bytes") { CHECK_EQ(sizeof(zerocopy::Header), 40u); }

  TEST_CASE("is standard layout and trivially copyable") {
    CHECK(std::is_standard_layout_v<zerocopy::Header>);
    CHECK(std::is_trivially_copyable_v<zerocopy::Header>);
  }

  TEST_CASE("seq field round-trip including boundary values") {
    zerocopy::Header h;

    h.seq = 42u;
    CHECK_EQ(h.seq, 42u);

    h.seq = 0xFFFFFFFFu;
    CHECK_EQ(h.seq, 0xFFFFFFFFu);

    h.seq = 0u;
    CHECK_EQ(h.seq, 0u);
  }

  TEST_CASE("frame_id stores short and max-length strings") {
    zerocopy::Header h;

    const size_t max_len = sizeof(h.frame_id) - 1;

    SUBCASE("short string") {
      const char* s = "cam_front";
      const size_t n = std::min(std::strlen(s), max_len);
      std::memcpy(h.frame_id, s, n);
      h.frame_id[n] = '\0';
      CHECK_EQ(std::string(h.frame_id), "cam_front");
    }

    SUBCASE("15-char string fills buffer") {
      const char* s = "123456789012345";
      const size_t n = std::min(std::strlen(s), max_len);
      std::memcpy(h.frame_id, s, n);
      h.frame_id[n] = '\0';
      CHECK_EQ(std::string(h.frame_id), "123456789012345");
    }
  }

  TEST_CASE("time_meas and time_pub accept max uint64 values") {
    zerocopy::Header h;

    h.time_meas = 0xFFFFFFFFFFFFFFFFULL;
    CHECK_EQ(h.time_meas, 0xFFFFFFFFFFFFFFFFULL);

    h.time_pub = 0xFFFFFFFFFFFFFFFFULL;
    CHECK_EQ(h.time_pub, 0xFFFFFFFFFFFFFFFFULL);
  }

  TEST_CASE("reserved field is writable") {
    zerocopy::Header h;

    h.reserved = 0xDEADBEEFu;
    CHECK_EQ(h.reserved, 0xDEADBEEFu);
  }

  TEST_CASE("all fields can hold max boundary values simultaneously") {
    zerocopy::Header h;

    h.seq = 0xFFFFFFFFu;
    std::memset(h.frame_id, 0xFF, sizeof(h.frame_id));
    h.time_meas = 0xFFFFFFFFFFFFFFFFULL;
    h.time_pub = 0xFFFFFFFFFFFFFFFFULL;
    h.reserved = 0xFFFFFFFFu;

    CHECK_EQ(h.seq, 0xFFFFFFFFu);
    CHECK_EQ(static_cast<uint8_t>(h.frame_id[0]), 0xFFu);
    CHECK_EQ(h.time_meas, 0xFFFFFFFFFFFFFFFFULL);
    CHECK_EQ(h.time_pub, 0xFFFFFFFFFFFFFFFFULL);
    CHECK_EQ(h.reserved, 0xFFFFFFFFu);
  }

  TEST_CASE("copy constructor produces independent copy") {
    zerocopy::Header a;

    a.seq = 10u;
    std::strncpy(a.frame_id, "cam_rear", sizeof(a.frame_id) - 1);
    a.frame_id[sizeof(a.frame_id) - 1] = '\0';
    a.time_meas = 300u;
    a.time_pub = 400u;
    a.reserved = 7u;

    zerocopy::Header b = a;

    CHECK_EQ(b.seq, 10u);
    CHECK_EQ(std::string(b.frame_id), "cam_rear");
    CHECK_EQ(b.time_meas, 300u);
    CHECK_EQ(b.time_pub, 400u);
    CHECK_EQ(b.reserved, 7u);

    b.seq = 99u;
    CHECK_EQ(a.seq, 10u);
  }

  TEST_CASE("copy assignment produces independent copy") {
    zerocopy::Header a;

    a.seq = 5u;
    a.time_pub = 12345u;

    zerocopy::Header b;
    b = a;

    CHECK_EQ(b.seq, 5u);
    CHECK_EQ(b.time_pub, 12345u);

    b.seq = 0u;
    CHECK_EQ(a.seq, 5u);
  }

  TEST_CASE("move constructor copies all fields") {
    zerocopy::Header a;

    a.seq = 100u;
    std::strncpy(a.frame_id, "radar_0", sizeof(a.frame_id) - 1);
    a.frame_id[sizeof(a.frame_id) - 1] = '\0';
    a.time_meas = 300u;
    a.time_pub = 400u;
    a.reserved = 500u;

    zerocopy::Header b = std::move(a);

    CHECK_EQ(b.seq, 100u);
    CHECK_EQ(std::string(b.frame_id), "radar_0");
    CHECK_EQ(b.time_meas, 300u);
    CHECK_EQ(b.time_pub, 400u);
    CHECK_EQ(b.reserved, 500u);
  }

  TEST_CASE("move assignment copies all fields") {
    zerocopy::Header a;

    a.seq = 11u;
    a.time_meas = 22u;

    zerocopy::Header b;
    b = std::move(a);

    CHECK_EQ(b.seq, 11u);
    CHECK_EQ(b.time_meas, 22u);
  }

  TEST_CASE("multiple default-constructed headers share identical initial state") {
    zerocopy::Header h1;
    zerocopy::Header h2;

    CHECK_EQ(h1.seq, h2.seq);
    CHECK_EQ(std::string(h1.frame_id), std::string(h2.frame_id));
    CHECK_EQ(h1.time_meas, h2.time_meas);
    CHECK_EQ(h1.time_pub, h2.time_pub);
    CHECK_EQ(h1.reserved, h2.reserved);
  }

  TEST_CASE("binary memcpy round-trip preserves all fields") {
    zerocopy::Header h;

    h.seq = 0xAABBCCDDu;
    std::strncpy(h.frame_id, "test_frame", sizeof(h.frame_id) - 1);
    h.frame_id[sizeof(h.frame_id) - 1] = '\0';
    h.time_meas = 0x5566778899AABBCCULL;
    h.time_pub = 0xDDEEFF0011223344ULL;
    h.reserved = 0u;

    uint8_t buf[40] = {};
    std::memcpy(buf, &h, sizeof(h));

    zerocopy::Header h2;
    std::memcpy(&h2, buf, sizeof(h2));

    CHECK_EQ(h2.seq, 0xAABBCCDDu);
    CHECK_EQ(std::string(h2.frame_id), "test_frame");
    CHECK_EQ(h2.time_meas, 0x5566778899AABBCCULL);
    CHECK_EQ(h2.time_pub, 0xDDEEFF0011223344ULL);
    CHECK_EQ(h2.reserved, 0u);
  }
}

// NOLINTEND
