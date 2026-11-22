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

#include "./transports/shm2_transport.h"

#include <doctest/doctest.h>

#include <sstream>
#include <string>

#include "../common_test.h"

TEST_SUITE("transports-Shm2Transport") {
  TEST_CASE("kDefaultMemSize is 4 KiB") { CHECK_EQ(Shm2Transport::kDefaultMemSize, 4096u); }

  TEST_CASE("kMaxMemSize is 32 mib") { CHECK_EQ(Shm2Transport::kMaxMemSize, 1024UL * 1024UL * 32UL); }

  TEST_CASE("all integer fields default correctly when only address supplied") {
    Shm2Transport transport("my_topic");

    CHECK_EQ(transport.address, "my_topic");
    CHECK(transport.event.empty());
    CHECK_EQ(transport.domain, 0);
    CHECK_EQ(transport.depth, 0);
    CHECK_EQ(transport.history, 0);
    CHECK_EQ(transport.wait, 0);
    CHECK_EQ(transport.size, Shm2Transport::kDefaultMemSize);
  }

  TEST_CASE("custom size is stored") {
    Shm2Transport transport("topic", "", 0, 0, 0, 0, 1024u * 1024u);

    CHECK_EQ(transport.size, 1024u * 1024u);
  }

  TEST_CASE("all parameters are stored") {
    Shm2Transport transport("addr", "evt", 2, 8, 3, 100, 512u);

    CHECK_EQ(transport.address, "addr");
    CHECK_EQ(transport.event, "evt");
    CHECK_EQ(transport.domain, 2);
    CHECK_EQ(transport.depth, 8);
    CHECK_EQ(transport.history, 3);
    CHECK_EQ(transport.wait, 100);
    CHECK_EQ(transport.size, 512u);
  }

  TEST_CASE("operator== holds when all seven fields match") {
    Shm2Transport a("addr", "evt", 0, 0, 0, 0, 256u);
    Shm2Transport b("addr", "evt", 0, 0, 0, 0, 256u);

    CHECK(a == b);
    CHECK_FALSE(a != b);
  }

  TEST_CASE("operator!= detects differing address") {
    Shm2Transport a("addr_a");
    Shm2Transport b("addr_b");

    CHECK(a != b);
    CHECK_FALSE(a == b);
  }

  TEST_CASE("operator!= detects differing event") {
    Shm2Transport a("addr", "evt_a");
    Shm2Transport b("addr", "evt_b");

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing domain") {
    Shm2Transport a("addr", "", 0);
    Shm2Transport b("addr", "", 1);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing depth") {
    Shm2Transport a("addr", "", 0, 1);
    Shm2Transport b("addr", "", 0, 2);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing history") {
    Shm2Transport a("addr", "", 0, 0, 0);
    Shm2Transport b("addr", "", 0, 0, 1);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing wait") {
    Shm2Transport a("addr", "", 0, 0, 0, 0);
    Shm2Transport b("addr", "", 0, 0, 0, 1);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing size") {
    Shm2Transport a("addr", "", 0, 0, 0, 0, 128u);
    Shm2Transport b("addr", "", 0, 0, 0, 0, 256u);

    CHECK(a != b);
  }

  TEST_CASE("self equality") {
    Shm2Transport a("addr", "evt", 1, 4, 2, 50, 512u);

    CHECK(a == a);
    CHECK_FALSE(a != a);
  }

  TEST_CASE("get_transport_type returns kShm2") {
    Shm2Transport transport("topic");

    CHECK(transport.get_transport_type() == TransportType::kShm2);
  }

  TEST_CASE("invalid values stream output and url fragment parsing cover branches") {
    Shm2Transport transport("addr", "evt", 1, 2, 3, 4, 1024);
    std::ostringstream oss;
    oss << transport;
    CHECK(oss.str().find("Shm2Transport:") != std::string::npos);
    CHECK(oss.str().find("[size]1024") != std::string::npos);

    CHECK_FALSE(Shm2Transport("").is_valid());
    CHECK_FALSE(Shm2Transport(std::string(81, 'a')).is_valid());

    Shm2Transport invalid_event("addr", std::string(81, 'e'));
    CHECK_FALSE(invalid_event.is_valid());

    Shm2Transport invalid_size_zero("addr");
    invalid_size_zero.size = 0;
    CHECK_FALSE(invalid_size_zero.is_valid());

    Shm2Transport invalid_size_large("addr");
    invalid_size_large.size = Shm2Transport::kMaxMemSize + 1;
    CHECK_FALSE(invalid_size_large.is_valid());

    Shm2Transport invalid_numbers("addr");
    invalid_numbers.domain = -1;
    CHECK_FALSE(invalid_numbers.is_valid());
    invalid_numbers.domain = 0;
    invalid_numbers.depth = -1;
    CHECK_FALSE(invalid_numbers.is_valid());
    invalid_numbers.depth = 0;
    invalid_numbers.history = -1;
    CHECK_FALSE(invalid_numbers.is_valid());

    Url default_size("shm2://parse/default?event=data");
    REQUIRE(default_size.parse(kPublisher));
    const auto* default_transport = static_cast<const Shm2Transport*>(default_size.get_target());
    REQUIRE(default_transport != nullptr);
    CHECK_EQ(default_transport->size, Shm2Transport::kDefaultMemSize);

    const char* invalid_urls[] = {
        "shm2://parse/bad# 1K", "shm2://parse/bad#bad",    "shm2://parse/bad#-1",
        "shm2://parse/bad#1TB", "shm2://parse/bad#1X",     "shm2://parse/bad#nan",
        "shm2://parse/bad#inf", "shm2://parse/bad#1e308G", "shm2://parse/bad#33M",
    };
    for (const auto* url_text : invalid_urls) {
      Url url(url_text);
      CHECK_FALSE(url.parse(kPublisher));
    }

    Url valid_mb("shm2://parse/valid#1MB");
    REQUIRE(valid_mb.parse(kPublisher));
    const auto* valid_transport = static_cast<const Shm2Transport*>(valid_mb.get_target());
    REQUIRE(valid_transport != nullptr);
    CHECK_EQ(valid_transport->size, 1024u * 1024u);

    Url client_wait("shm2://wait/client?event=req&wait=1#1K");
    CHECK_FALSE(client_wait.parse(kClient));
  }
}

#endif  // VLINK_SUPPORT_SHM2

// NOLINTEND
