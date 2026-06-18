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

#include "./impl/url.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "../common_test.h"

TEST_SUITE("impl-Url") {
  TEST_CASE("construct from string stores the url in get_str") {
    Url url("intra://test_topic");

    CHECK_EQ(url.get_str(), "intra://test_topic");
  }

  TEST_CASE("get_target returns non-null for a supported url") {
    Url url("intra://topic");

    CHECK_NE(url.get_target(), nullptr);
  }

  TEST_CASE("get_transport_type returns kIntra for intra url") {
    Url url("intra://topic");

    CHECK_EQ(url.get_transport_type(), TransportType::kIntra);
  }

  TEST_CASE("copy constructor produces independent object with same string") {
    Url original("intra://copy_test");
    Url copy(original);

    CHECK_EQ(copy.get_str(), original.get_str());
    CHECK_NE(copy.get_target(), original.get_target());
  }

  TEST_CASE("move constructor transfers string and target") {
    Url original("intra://move_test");
    const std::string expected = original.get_str();

    Url moved(std::move(original));

    CHECK_EQ(moved.get_str(), expected);
    CHECK_NE(moved.get_target(), nullptr);
    CHECK_EQ(original.get_target(), nullptr);
  }

  TEST_CASE("copy assignment replaces destination with copy of source") {
    Url a("intra://topic_a");
    Url b("intra://topic_b");

    b = a;

    CHECK_EQ(b.get_str(), a.get_str());
    CHECK_NE(b.get_target(), a.get_target());
  }

  TEST_CASE("move assignment transfers source to destination") {
    Url a("intra://topic_a");
    Url b("intra://topic_b");
    const std::string expected = a.get_str();

    b = std::move(a);

    CHECK_EQ(b.get_str(), expected);
    CHECK_EQ(a.get_target(), nullptr);
  }

  TEST_CASE("parse kPublisher succeeds for intra url") {
    Url url("intra://parse_test");

    CHECK(url.parse(kPublisher));
  }

  TEST_CASE("parse kSubscriber succeeds for intra url") {
    Url url("intra://parse_test");

    CHECK(url.parse(kSubscriber));
  }

  TEST_CASE("parse kServer succeeds for intra url") {
    Url url("intra://server_test");

    CHECK(url.parse(kServer));
  }

  TEST_CASE("parse kClient succeeds for intra url") {
    Url url("intra://client_test");

    CHECK(url.parse(kClient));
  }

  TEST_CASE("parse kSetter succeeds for intra url") {
    Url url("intra://setter_test");

    CHECK(url.parse(kSetter));
  }

  TEST_CASE("parse kGetter succeeds for intra url") {
    Url url("intra://getter_test");

    CHECK(url.parse(kGetter));
  }

  TEST_CASE("is_valid returns true for intra url after parse") {
    Url url("intra://valid_test");
    url.parse(kPublisher);

    CHECK(url.is_valid());
  }

  TEST_CASE("get_impl_type contains kPublisher bit after parsing as publisher") {
    Url url("intra://impl_test");
    url.parse(kPublisher);

    CHECK_NE((url.get_impl_type() & kPublisher), 0);
  }

  TEST_CASE("get_str preserves url that contains query parameters") {
    Url url("intra://topic?key=value");

    CHECK_NE(url.get_str().find("intra://"), std::string::npos);
  }

  TEST_CASE("get_str preserves url with underscores in topic name") {
    Url url("intra://topic_with_underscores");

    CHECK_EQ(url.get_str(), "intra://topic_with_underscores");
    CHECK_NE(url.get_target(), nullptr);
  }

  TEST_CASE("get_transport_type returns kIntra for intra url with multi-segment path") {
    Url url("intra://ns/topic/sub");

    CHECK_EQ(url.get_transport_type(), TransportType::kIntra);
  }

#ifdef VLINK_SUPPORT_DDS
  TEST_CASE("get_transport_type returns kDds for dds url") {
    Url url("dds://namespace/topic");

    CHECK_EQ(url.get_transport_type(), TransportType::kDds);
  }
#endif

#ifdef VLINK_SUPPORT_ZENOH
  TEST_CASE("get_transport_type returns kZenoh for zenoh url") {
    Url url("zenoh://namespace/topic");

    CHECK_EQ(url.get_transport_type(), TransportType::kZenoh);
  }
#endif

  TEST_CASE("is_local_type identifies intra shm and shm2 as local") {
    CHECK(Url::is_local_type("intra://topic"));
    CHECK(Url::is_local_type("shm://topic"));
    CHECK(Url::is_local_type("shm2://topic"));
  }

  TEST_CASE("is_local_type identifies network transports as not local") {
    CHECK_FALSE(Url::is_local_type("dds://topic"));
    CHECK_FALSE(Url::is_local_type("zenoh://topic"));
    CHECK_FALSE(Url::is_local_type("ddsc://topic"));
    CHECK_FALSE(Url::is_local_type("someip://topic"));
    CHECK_FALSE(Url::is_local_type("fdbus://topic"));
    CHECK_FALSE(Url::is_local_type("mqtt://topic"));
  }

  TEST_CASE("is_local_type returns false for empty string") { CHECK_FALSE(Url::is_local_type("")); }

  TEST_CASE("is_intra_type returns true only for intra url") {
    CHECK(Url::is_intra_type("intra://topic"));
    CHECK_FALSE(Url::is_intra_type("shm://topic"));
    CHECK_FALSE(Url::is_intra_type("shm2://topic"));
    CHECK_FALSE(Url::is_intra_type("dds://topic"));
    CHECK_FALSE(Url::is_intra_type(""));
  }

  TEST_CASE("is_shm_type returns true for shm and shm2 only") {
    CHECK(Url::is_shm_type("shm://topic"));
    CHECK(Url::is_shm_type("shm2://topic"));
    CHECK_FALSE(Url::is_shm_type("intra://topic"));
    CHECK_FALSE(Url::is_shm_type("dds://topic"));
    CHECK_FALSE(Url::is_shm_type("someip://topic"));
    CHECK_FALSE(Url::is_shm_type(""));
  }

  TEST_CASE("get_sort_index returns -1 for empty string") { CHECK_EQ(Url::get_sort_index(""), -1); }

  TEST_CASE("get_sort_index returns non-negative for all known transports") {
    CHECK_GE(Url::get_sort_index("intra://test"), 0);
    CHECK_GE(Url::get_sort_index("dds://test"), 0);
    CHECK_GE(Url::get_sort_index("zenoh://test"), 0);
    CHECK_GE(Url::get_sort_index("someip://test"), 0);
    CHECK_GE(Url::get_sort_index("ddsc://test"), 0);
    CHECK_GE(Url::get_sort_index("fdbus://test"), 0);
  }

  TEST_CASE("get_sort_index assigns lower index to local transports than network transports") {
    CHECK_LT(Url::get_sort_index("intra://topic"), Url::get_sort_index("dds://topic"));
    CHECK_LT(Url::get_sort_index("shm://topic"), Url::get_sort_index("zenoh://topic"));
    CHECK_LT(Url::get_sort_index("shm2://topic"), Url::get_sort_index("dds://topic"));
  }

  TEST_CASE("get_transport_enable_flags returns non-zero when at least one transport is compiled in") {
    uint16_t flags = Url::get_transport_enable_flags();
    CHECK_NE(flags, 0);
  }

#ifdef VLINK_SUPPORT_INTRA
  TEST_CASE("get_transport_enable_flags has kEnableIntra bit set when intra is compiled in") {
    uint16_t flags = Url::get_transport_enable_flags();

    CHECK_NE((flags & Url::kEnableIntra), 0);
  }
#endif

  TEST_CASE("kEnableEmpty is zero") { CHECK_EQ(Url::kEnableEmpty, 0); }

  TEST_CASE("kEnableAll equals 0xFFFF") { CHECK_EQ(Url::kEnableAll, static_cast<uint16_t>(0xFFFF)); }

  TEST_CASE("kEnableAll includes every individual transport flag") {
    CHECK_NE((Url::kEnableAll & Url::kEnableIntra), 0);
    CHECK_NE((Url::kEnableAll & Url::kEnableShm), 0);
    CHECK_NE((Url::kEnableAll & Url::kEnableShm2), 0);
    CHECK_NE((Url::kEnableAll & Url::kEnableZenoh), 0);
    CHECK_NE((Url::kEnableAll & Url::kEnableDds), 0);
    CHECK_NE((Url::kEnableAll & Url::kEnableDdsc), 0);
    CHECK_NE((Url::kEnableAll & Url::kEnableDdsr), 0);
    CHECK_NE((Url::kEnableAll & Url::kEnableDdst), 0);
    CHECK_NE((Url::kEnableAll & Url::kEnableSomeip), 0);
    CHECK_NE((Url::kEnableAll & Url::kEnableMqtt), 0);
    CHECK_NE((Url::kEnableAll & Url::kEnableFdbus), 0);
    CHECK_NE((Url::kEnableAll & Url::kEnableQnx), 0);
  }

  TEST_CASE("individual TransportEnableFlag values are pairwise distinct") {
    CHECK_NE(Url::kEnableIntra, Url::kEnableShm);
    CHECK_NE(Url::kEnableShm, Url::kEnableShm2);
    CHECK_NE(Url::kEnableZenoh, Url::kEnableDds);
    CHECK_NE(Url::kEnableDds, Url::kEnableDdsc);
    CHECK_NE(Url::kEnableMqtt, Url::kEnableFdbus);
  }

  TEST_CASE("bitwise OR of individual flags selects only those transports") {
    uint16_t combined = Url::kEnableIntra | Url::kEnableDds;

    CHECK_NE((combined & Url::kEnableIntra), 0);
    CHECK_NE((combined & Url::kEnableDds), 0);
    CHECK_EQ((combined & Url::kEnableShm), 0);
    CHECK_EQ((combined & Url::kEnableZenoh), 0);
  }
}

// NOLINTEND
