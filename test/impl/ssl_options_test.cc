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

#include "./impl/ssl_options.h"

#include <doctest/doctest.h>

#include <string>

#include "../common_test.h"

TEST_SUITE("impl-SslOptions") {
  TEST_CASE("default construction leaves all strings empty and verify_peer true") {
    SslOptions opts;

    CHECK(opts.verify_peer);
    CHECK(opts.ca_file.empty());
    CHECK(opts.cert_file.empty());
    CHECK(opts.key_file.empty());
    CHECK(opts.key_password.empty());
    CHECK(opts.server_name.empty());
    CHECK(opts.ciphers.empty());
  }

  TEST_CASE("is_valid returns false for default-constructed options") {
    SslOptions opts;

    CHECK_FALSE(opts.is_valid());
  }

  TEST_CASE("is_valid returns true when ca_file or cert_file is set") {
    SUBCASE("ca_file alone") {
      SslOptions opts;
      opts.ca_file = "/etc/ssl/ca.pem";
      CHECK(opts.is_valid());
    }

    SUBCASE("cert_file alone") {
      SslOptions opts;
      opts.cert_file = "/etc/ssl/client.pem";
      CHECK(opts.is_valid());
    }

    SUBCASE("both ca_file and cert_file") {
      SslOptions opts;
      opts.ca_file = "/ca.pem";
      opts.cert_file = "/cert.pem";
      CHECK(opts.is_valid());
    }
  }

  TEST_CASE("is_valid returns false when only auxiliary fields are set") {
    SUBCASE("key_file only") {
      SslOptions opts;
      opts.key_file = "/key.pem";
      CHECK_FALSE(opts.is_valid());
    }

    SUBCASE("verify_peer false only") {
      SslOptions opts;
      opts.verify_peer = false;
      CHECK_FALSE(opts.is_valid());
    }

    SUBCASE("server_name only") {
      SslOptions opts;
      opts.server_name = "broker.example.com";
      CHECK_FALSE(opts.is_valid());
    }

    SUBCASE("ciphers only") {
      SslOptions opts;
      opts.ciphers = "ECDHE-RSA-AES256-GCM-SHA384";
      CHECK_FALSE(opts.is_valid());
    }
  }

  TEST_CASE("parse_from with empty property map yields default-equivalent result") {
    Conf::PropertiesMap props;
    auto opts = SslOptions::parse_from(props);

    CHECK_FALSE(opts.is_valid());
    CHECK(opts.verify_peer);
    CHECK(opts.ca_file.empty());
  }

  TEST_CASE("parse_from reads ssl.ca into ca_file") {
    Conf::PropertiesMap props;
    props["ssl.ca"] = "/path/to/ca.pem";
    auto opts = SslOptions::parse_from(props);

    CHECK(opts.is_valid());
    CHECK_EQ(opts.ca_file, "/path/to/ca.pem");
    CHECK(opts.cert_file.empty());
  }

  TEST_CASE("parse_from reads all seven ssl.* keys") {
    Conf::PropertiesMap props;
    props["ssl.ca"] = "/ca.pem";
    props["ssl.cert"] = "/cert.pem";
    props["ssl.key"] = "/key.pem";
    props["ssl.key_password"] = "s3cr3t";
    props["ssl.verify"] = "0";
    props["ssl.server_name"] = "broker.test";
    props["ssl.ciphers"] = "ECDHE-RSA-AES256-GCM-SHA384";

    auto opts = SslOptions::parse_from(props);

    CHECK(opts.is_valid());
    CHECK_EQ(opts.ca_file, "/ca.pem");
    CHECK_EQ(opts.cert_file, "/cert.pem");
    CHECK_EQ(opts.key_file, "/key.pem");
    CHECK_EQ(opts.key_password, "s3cr3t");
    CHECK_FALSE(opts.verify_peer);
    CHECK_EQ(opts.server_name, "broker.test");
    CHECK_EQ(opts.ciphers, "ECDHE-RSA-AES256-GCM-SHA384");
  }

  TEST_CASE("parse_from ssl.verify interpretation") {
    SUBCASE("value 0 disables verification") {
      Conf::PropertiesMap props;
      props["ssl.ca"] = "/ca.pem";
      props["ssl.verify"] = "0";
      auto opts = SslOptions::parse_from(props);
      CHECK_FALSE(opts.verify_peer);
    }

    SUBCASE("value 1 keeps verification enabled") {
      Conf::PropertiesMap props;
      props["ssl.ca"] = "/ca.pem";
      props["ssl.verify"] = "1";
      auto opts = SslOptions::parse_from(props);
      CHECK(opts.verify_peer);
    }

    SUBCASE("absent ssl.verify defaults to enabled") {
      Conf::PropertiesMap props;
      props["ssl.ca"] = "/ca.pem";
      auto opts = SslOptions::parse_from(props);
      CHECK(opts.verify_peer);
    }
  }

  TEST_CASE("parse_from ignores unrelated properties") {
    Conf::PropertiesMap props;
    props["dds.tcp"] = "1";
    props["mqtt.broker"] = "tcp://localhost:1883";
    props["zenoh.mode"] = "peer";

    auto opts = SslOptions::parse_from(props);

    CHECK_FALSE(opts.is_valid());
    CHECK(opts.ca_file.empty());
  }

  TEST_CASE("parse_from reads ssl.* alongside unrelated properties") {
    Conf::PropertiesMap props;
    props["dds.tcp"] = "1";
    props["ssl.ca"] = "/ca.pem";
    props["zenoh.mode"] = "peer";

    auto opts = SslOptions::parse_from(props);

    CHECK(opts.is_valid());
    CHECK_EQ(opts.ca_file, "/ca.pem");
  }

  TEST_CASE("parse_to writes nothing for default-constructed options") {
    SslOptions opts;
    Conf::PropertiesMap props;
    opts.parse_to(props);

    CHECK(props.empty());
  }

  TEST_CASE("parse_to writes all non-default fields") {
    SslOptions opts;
    opts.ca_file = "/ca.pem";
    opts.cert_file = "/cert.pem";
    opts.key_file = "/key.pem";
    opts.key_password = "pass";
    opts.verify_peer = false;
    opts.server_name = "sni.test";
    opts.ciphers = "AES256";

    Conf::PropertiesMap props;
    opts.parse_to(props);

    CHECK_EQ(props.at("ssl.ca"), "/ca.pem");
    CHECK_EQ(props.at("ssl.cert"), "/cert.pem");
    CHECK_EQ(props.at("ssl.key"), "/key.pem");
    CHECK_EQ(props.at("ssl.key_password"), "pass");
    CHECK_EQ(props.at("ssl.verify"), "0");
    CHECK_EQ(props.at("ssl.server_name"), "sni.test");
    CHECK_EQ(props.at("ssl.ciphers"), "AES256");
  }

  TEST_CASE("parse_to does not write ssl.verify when verify_peer is true") {
    SslOptions opts;
    opts.ca_file = "/ca.pem";
    opts.verify_peer = true;

    Conf::PropertiesMap props;
    opts.parse_to(props);

    CHECK(props.find("ssl.verify") == props.end());
    CHECK_EQ(props.at("ssl.ca"), "/ca.pem");
  }

  TEST_CASE("parse_to writes only non-empty string fields") {
    SslOptions opts;
    opts.ca_file = "/ca.pem";
    opts.server_name = "example.com";

    Conf::PropertiesMap props;
    opts.parse_to(props);

    CHECK_EQ(props.size(), 2u);
    CHECK(props.find("ssl.cert") == props.end());
    CHECK(props.find("ssl.key") == props.end());
  }

  TEST_CASE("parse_to preserves existing non-ssl properties in the map") {
    SslOptions opts;
    opts.ca_file = "/ca.pem";

    Conf::PropertiesMap props;
    props["dds.tcp"] = "1";
    props["zenoh.mode"] = "peer";
    opts.parse_to(props);

    CHECK_EQ(props.at("dds.tcp"), "1");
    CHECK_EQ(props.at("zenoh.mode"), "peer");
    CHECK_EQ(props.at("ssl.ca"), "/ca.pem");
  }

  TEST_CASE("parse_to then parse_from round-trips all fields") {
    SslOptions original;
    original.ca_file = "/ca.pem";
    original.cert_file = "/cert.pem";
    original.key_file = "/key.pem";
    original.key_password = "secret";
    original.verify_peer = false;
    original.server_name = "broker.test";
    original.ciphers = "ECDHE-RSA-AES128";

    Conf::PropertiesMap props;
    original.parse_to(props);
    auto restored = SslOptions::parse_from(props);

    CHECK_EQ(restored.ca_file, original.ca_file);
    CHECK_EQ(restored.cert_file, original.cert_file);
    CHECK_EQ(restored.key_file, original.key_file);
    CHECK_EQ(restored.key_password, original.key_password);
    CHECK_EQ(restored.verify_peer, original.verify_peer);
    CHECK_EQ(restored.server_name, original.server_name);
    CHECK_EQ(restored.ciphers, original.ciphers);
    CHECK_EQ(restored.is_valid(), original.is_valid());
  }

  TEST_CASE("round-trip preserves verify_peer true without writing ssl.verify key") {
    SslOptions original;
    original.ca_file = "/ca.pem";
    original.verify_peer = true;

    Conf::PropertiesMap props;
    original.parse_to(props);
    auto restored = SslOptions::parse_from(props);

    CHECK(restored.verify_peer);
  }
}

// NOLINTEND
