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

#ifdef VLINK_SUPPORT_DDS

#include "./transports/dds_transport.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "../common_test.h"
#include "./base/functional.h"
#include "./extension/qos.h"
#include "./impl/transport.h"

TEST_SUITE("transports-DdsTransport") {
  TEST_CASE("default domain depth and qos when only topic supplied") {
    DdsTransport transport("vehicle/speed");

    CHECK_EQ(transport.topic, "vehicle/speed");
    CHECK_EQ(transport.domain, 0);
    CHECK_EQ(transport.depth, 0);
    CHECK(transport.qos.empty());
    CHECK(transport.qos_ext.empty());
  }

  TEST_CASE("explicit domain and depth are stored") {
    DdsTransport transport("my_topic", 5, 10);

    CHECK_EQ(transport.topic, "my_topic");
    CHECK_EQ(transport.domain, 5);
    CHECK_EQ(transport.depth, 10);
    CHECK(transport.qos.empty());
  }

  TEST_CASE("named qos profile is stored") {
    DdsTransport transport("my_topic", 2, 0, "fast_profile");

    CHECK_EQ(transport.qos, "fast_profile");
    CHECK(transport.qos_ext.empty());
  }

  TEST_CASE("qos_ext constructor stores property map") {
    DdsTransport::PropertiesMap ext;
    ext["pub"] = "pub_profile";
    ext["sub"] = "sub_profile";

    DdsTransport transport("my_topic", 1, ext);

    CHECK_EQ(transport.domain, 1);
    CHECK(transport.qos.empty());
    CHECK_FALSE(transport.qos_ext.empty());
    CHECK_EQ(transport.qos_ext.at("pub"), "pub_profile");
    CHECK_EQ(transport.qos_ext.at("sub"), "sub_profile");
  }

  TEST_CASE("operator== holds when all fields match") {
    DdsTransport a("topic", 1, 5, "q");
    DdsTransport b("topic", 1, 5, "q");

    CHECK(a == b);
    CHECK_FALSE(a != b);
  }

  TEST_CASE("operator!= detects differing topic") {
    DdsTransport a("topic_a", 0, 0);
    DdsTransport b("topic_b", 0, 0);

    CHECK(a != b);
    CHECK_FALSE(a == b);
  }

  TEST_CASE("operator!= detects differing domain") {
    DdsTransport a("topic", 0, 0);
    DdsTransport b("topic", 1, 0);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing depth") {
    DdsTransport a("topic", 0, 1);
    DdsTransport b("topic", 0, 2);

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing qos name") {
    DdsTransport a("topic", 0, 0, "qos_a");
    DdsTransport b("topic", 0, 0, "qos_b");

    CHECK(a != b);
  }

  TEST_CASE("operator!= detects differing qos_ext") {
    DdsTransport::PropertiesMap ext_a;
    ext_a["pub"] = "x";
    DdsTransport::PropertiesMap ext_b;
    ext_b["pub"] = "y";

    DdsTransport a("topic", 0, ext_a);
    DdsTransport b("topic", 0, ext_b);

    CHECK(a != b);
  }

  TEST_CASE("get_transport_type returns kDds") {
    DdsTransport transport("topic");

    CHECK(transport.get_transport_type() == TransportType::kDds);
  }

  TEST_CASE("register_qos accepts a valid profile name") {
    Qos qos;
    qos.reliability.kind = Qos::Reliability::kReliable;
    qos.valid = true;

    CHECK_NOTHROW(DdsTransport::register_qos("dds_test_profile", qos));
    CHECK(DdsTransport("topic", 0, 0, "dds_test_profile").is_valid());
  }

  TEST_CASE("public discovery and qos-file helpers tolerate empty runtime state") {
    const auto topics = DdsTransport::get_discovered_topics(250);
    CHECK(topics.empty());

    const auto missing_file = (std::filesystem::path(Utils::get_tmp_dir()) / "missing_dds_qos_profile.xml").string();
    CHECK_FALSE(DdsTransport::load_global_qos_file(missing_file));
  }

  TEST_CASE("register_qos rejects reserved and duplicate names") {
    Qos qos;
    qos.reliability.kind = Qos::Reliability::kReliable;

    CHECK_THROWS(DdsTransport::register_qos("depth", qos));
    CHECK_NOTHROW(DdsTransport::register_qos("dds_duplicate_profile_for_test", qos));
    CHECK_THROWS(DdsTransport::register_qos("dds_duplicate_profile_for_test", qos));
  }

  TEST_CASE("invalid values helpers and stream output cover branches") {
    Url valid_url("dds://host/path?domain=2&depth=3");
    REQUIRE(valid_url.parse(kPublisher));
    const auto* parsed = static_cast<const DdsTransport*>(valid_url.get_target());
    REQUIRE(parsed != nullptr);
    CHECK_EQ(parsed->topic, "host/path");
    CHECK_EQ(parsed->domain, 2);
    CHECK_EQ(parsed->depth, 3);

    CHECK_FALSE(Url("dds:///missing-host").parse(kPublisher));

    DdsTransport::PropertiesMap qos_ext;
    qos_ext["pub"] = "pub_profile";
    qos_ext["custom"] = "ignored_profile";
    DdsTransport transport("topic", 1, qos_ext);

    std::ostringstream oss;
    oss << transport;
    CHECK(oss.str().find("DdsTransport:") != std::string::npos);
    CHECK(oss.str().find("[topic]topic") != std::string::npos);
    CHECK(oss.str().find("(pub)pub_profile") != std::string::npos);

    DdsTransport empty_topic("");
    CHECK_FALSE(empty_topic.is_valid());

    DdsTransport invalid_domain("topic");
    invalid_domain.domain = -1;
    CHECK_FALSE(invalid_domain.is_valid());

    DdsTransport invalid_combo("topic", 0, 0, "qos");
    invalid_combo.qos_ext["pub"] = "pub_profile";
    CHECK_FALSE(invalid_combo.is_valid());

    DdsTransport missing_qos("topic", 0, 0, "missing_dds_qos_for_test");
    CHECK(missing_qos.is_valid());
  }

  TEST_CASE("url parsing derives the DDS topic from host and path") {
    Url nested("dds://vehicle/speed");
    REQUIRE(nested.parse(kPublisher));
    const auto* nested_transport = static_cast<const DdsTransport*>(nested.get_target());
    REQUIRE(nested_transport != nullptr);
    CHECK_EQ(nested_transport->topic, "vehicle/speed");

    Url host_only("dds://my_service");
    REQUIRE(host_only.parse(kPublisher));
    const auto* host_transport = static_cast<const DdsTransport*>(host_only.get_target());
    REQUIRE(host_transport != nullptr);
    CHECK_EQ(host_transport->topic, "my_service");

    CHECK_FALSE(Url("dds://").parse(kPublisher));
    CHECK_THROWS_AS(Url("not-a-url").parse(kPublisher), std::runtime_error);
  }

  TEST_CASE("url parsing strips query when deriving topic") {
    Url nested("dds://vehicle/speed?domain=1");
    REQUIRE(nested.parse(kPublisher));
    const auto* nested_transport = static_cast<const DdsTransport*>(nested.get_target());
    REQUIRE(nested_transport != nullptr);
    CHECK_EQ(nested_transport->topic, "vehicle/speed");
    CHECK_EQ(nested_transport->domain, 1);

    Url host_only("dds://vehicle?domain=1");
    REQUIRE(host_only.parse(kPublisher));
    const auto* host_transport = static_cast<const DdsTransport*>(host_only.get_target());
    REQUIRE(host_transport != nullptr);
    CHECK_EQ(host_transport->topic, "vehicle");
    CHECK_EQ(host_transport->domain, 1);

    CHECK_FALSE(Url("dds://?domain=1").parse(kPublisher));
  }

  TEST_CASE("url parsing separates named qos from qos extension keys") {
    Url named("dds://vehicle/state?domain=7&depth=9&qos=event");
    REQUIRE(named.parse(kSubscriber));
    const auto* named_transport = static_cast<const DdsTransport*>(named.get_target());
    REQUIRE(named_transport != nullptr);
    CHECK_EQ(named_transport->topic, "vehicle/state");
    CHECK_EQ(named_transport->domain, 7);
    CHECK_EQ(named_transport->depth, 9);
    CHECK_EQ(named_transport->qos, "event");
    CHECK(named_transport->qos_ext.empty());

    Url ext("dds://v/s?part=part_qos&topic=topic_qos&pub=pub_qos&sub=sub_qos&writer=writer_qos&reader=reader_qos");
    REQUIRE(ext.parse(kPublisher));
    const auto* ext_transport = static_cast<const DdsTransport*>(ext.get_target());
    REQUIRE(ext_transport != nullptr);
    CHECK(ext_transport->qos.empty());
    CHECK_EQ(ext_transport->qos_ext.at("part"), "part_qos");
    CHECK_EQ(ext_transport->qos_ext.at("topic"), "topic_qos");
    CHECK_EQ(ext_transport->qos_ext.at("pub"), "pub_qos");
    CHECK_EQ(ext_transport->qos_ext.at("sub"), "sub_qos");
    CHECK_EQ(ext_transport->qos_ext.at("writer"), "writer_qos");
    CHECK_EQ(ext_transport->qos_ext.at("reader"), "reader_qos");
  }
}

#endif  // VLINK_SUPPORT_DDS

// NOLINTEND
