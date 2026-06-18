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

#include "./impl/url_parser.h"

#include <doctest/doctest.h>

#include <map>
#include <string>

#include "../common_test.h"

namespace {

constexpr UrlParser::Category kH = UrlParser::Category::kHierarchical;
constexpr UrlParser::Category kN = UrlParser::Category::kNonHierarchical;
constexpr UrlParser::Separator kAmp = UrlParser::Separator::kAmpersand;
constexpr UrlParser::Separator kSemi = UrlParser::Separator::kSemicolon;

}  // namespace

TEST_SUITE("impl-UrlParser") {
  TEST_CASE("simple hierarchical url is decomposed into transport host path") {
    UrlParser p("shm://vehicle/speed");

    CHECK_EQ(p.get_transport(), "shm");
    CHECK_EQ(p.get_host(), "vehicle");
    CHECK_EQ(p.get_path(), "speed");
    CHECK_EQ(p.get_port(), 0);
    CHECK(p.get_query().empty());
    CHECK(p.get_fragment().empty());
    CHECK(p.get_username().empty());
    CHECK(p.get_password().empty());
    CHECK_EQ(p.get_category(), kH);
  }

  TEST_CASE("query parameters are parsed into dictionary") {
    UrlParser p("dds://topic/sub/path?key=value&key2=val2");

    CHECK_EQ(p.get_transport(), "dds");
    CHECK_EQ(p.get_host(), "topic");
    CHECK_EQ(p.get_path(), "sub/path");

    const auto& dict = p.get_query_dictionary();
    REQUIRE_EQ(dict.count("key"), 1u);
    REQUIRE_EQ(dict.count("key2"), 1u);
    CHECK_EQ(dict.at("key"), "value");
    CHECK_EQ(dict.at("key2"), "val2");
  }

  TEST_CASE("host-colon-port is parsed correctly") {
    UrlParser p("someip://127.0.0.1:30490/0x1/method");

    CHECK_EQ(p.get_transport(), "someip");
    CHECK_EQ(p.get_host(), "127.0.0.1");
    CHECK_EQ(p.get_port(), 30490);
    CHECK_FALSE(p.get_path().empty());
  }

  TEST_CASE("host-only url with no path produces empty path") {
    UrlParser p("fdbus://my_service");

    CHECK_EQ(p.get_transport(), "fdbus");
    CHECK_EQ(p.get_host(), "my_service");
    CHECK(p.get_path().empty());
  }

  TEST_CASE("transport-only authority with no host produces empty host") {
    UrlParser p("intra://");

    CHECK_EQ(p.get_transport(), "intra");
    CHECK(p.get_host().empty());
  }

  TEST_CASE("single query parameter is placed in dictionary") {
    UrlParser p("intra://test1?event=hello");

    CHECK_EQ(p.get_transport(), "intra");
    CHECK_EQ(p.get_host(), "test1");

    const auto& dict = p.get_query_dictionary();
    REQUIRE_EQ(dict.count("event"), 1u);
    CHECK_EQ(dict.at("event"), "hello");
  }

  TEST_CASE("fragment component is extracted correctly") {
    UrlParser p("url://host/path#fragment");

    CHECK_EQ(p.get_transport(), "url");
    CHECK_EQ(p.get_fragment(), "fragment");
    CHECK(p.get_query().empty());
  }

  TEST_CASE("username and password are parsed from authority") {
    UrlParser p("transport://user:pass@host/path");

    CHECK_EQ(p.get_username(), "user");
    CHECK_EQ(p.get_password(), "pass");
    CHECK_EQ(p.get_host(), "host");
    CHECK_EQ(p.get_path(), "path");
  }

  TEST_CASE("bare user-at-host without colon throws runtime error") {
    CHECK_THROWS_AS(UrlParser("transport://user@host"), std::runtime_error);
  }

  TEST_CASE("url with all components is fully decomposed") {
    UrlParser p("dds://admin:secret@192.168.1.1:7400/vehicle/speed?qos=reliable#section1");

    CHECK_EQ(p.get_transport(), "dds");
    CHECK_EQ(p.get_username(), "admin");
    CHECK_EQ(p.get_password(), "secret");
    CHECK_EQ(p.get_host(), "192.168.1.1");
    CHECK_EQ(p.get_port(), 7400);
    CHECK_EQ(p.get_path(), "vehicle/speed");
    CHECK_EQ(p.get_fragment(), "section1");
    CHECK_EQ(p.get_query_dictionary().at("qos"), "reliable");
  }

  TEST_CASE("get_port returns 0 when no port is present") {
    UrlParser p("shm://topicname/path");

    CHECK_EQ(p.get_port(), 0);
  }

  TEST_CASE("port boundary values are parsed correctly") {
    SUBCASE("port 0") {
      UrlParser p("someip://host:0/path");
      CHECK_EQ(p.get_port(), 0);
    }

    SUBCASE("port 65535") {
      UrlParser p("someip://host:65535/path");
      CHECK_EQ(p.get_port(), 65535);
    }

    SUBCASE("port 8080") {
      UrlParser p("someip://localhost:8080");
      CHECK_EQ(p.get_port(), 8080);
      CHECK_EQ(p.get_host(), "localhost");
    }
  }

  TEST_CASE("ampersand separator splits query into individual key-value pairs") {
    UrlParser p("url://host/path?a=1&b=2", kH, kAmp);

    const auto& dict = p.get_query_dictionary();
    REQUIRE_EQ(dict.count("a"), 1u);
    REQUIRE_EQ(dict.count("b"), 1u);
    CHECK_EQ(dict.at("a"), "1");
    CHECK_EQ(dict.at("b"), "2");
  }

  TEST_CASE("semicolon separator splits query into individual key-value pairs") {
    UrlParser p("url://host/path?a=1;b=2", kH, kSemi);

    const auto& dict = p.get_query_dictionary();
    REQUIRE_EQ(dict.count("a"), 1u);
    REQUIRE_EQ(dict.count("b"), 1u);
    CHECK_EQ(dict.at("a"), "1");
    CHECK_EQ(dict.at("b"), "2");
  }

  TEST_CASE("semicolon separator handles three parameters") {
    UrlParser p("url://host/path?x=10;y=20;z=30", kH, kSemi);

    const auto& dict = p.get_query_dictionary();
    CHECK_EQ(dict.size(), 3u);
    CHECK_EQ(dict.at("x"), "10");
    CHECK_EQ(dict.at("y"), "20");
    CHECK_EQ(dict.at("z"), "30");
  }

  TEST_CASE("ampersand separator does not split on semicolon character") {
    UrlParser p("url://host/path?a=1;b=2", kH, kAmp);

    CHECK_EQ(p.get_query_dictionary().size(), 1u);
  }

  TEST_CASE("query key without equals sign is stored with empty string value") {
    UrlParser p("url://host/path?flag", kH, kAmp);

    const auto& dict = p.get_query_dictionary();
    REQUIRE_EQ(dict.count("flag"), 1u);
    CHECK(dict.at("flag").empty());
  }

  TEST_CASE("query with four key-value pairs all parsed correctly") {
    UrlParser p("dds://host/path?a=1&b=2&c=3&d=4", kH, kAmp);

    const auto& dict = p.get_query_dictionary();
    CHECK_EQ(dict.size(), 4u);
    CHECK_EQ(dict.at("a"), "1");
    CHECK_EQ(dict.at("d"), "4");
  }

  TEST_CASE("fragment is empty when not present in query url") {
    UrlParser p("shm://host/path?q=1");

    CHECK(p.get_fragment().empty());
  }

  TEST_CASE("query and fragment coexist in the same url") {
    UrlParser p("dds://host/path?key=val#frag");

    CHECK_EQ(p.get_query_dictionary().at("key"), "val");
    CHECK_EQ(p.get_fragment(), "frag");
  }

  TEST_CASE("ip address is accepted as host component") {
    UrlParser p("dds://192.168.0.1/topic");

    CHECK_EQ(p.get_host(), "192.168.0.1");
  }

  TEST_CASE("multi-segment path is stored as slash-joined string") {
    UrlParser p("dds://ns/a/b/c/d");

    CHECK_EQ(p.get_host(), "ns");
    CHECK_EQ(p.get_path(), "a/b/c/d");
  }

  TEST_CASE("path with dots hyphens and underscores is preserved") {
    SUBCASE("dots in path") {
      UrlParser p("dds://ns/path.with.dots");
      CHECK_EQ(p.get_path(), "path.with.dots");
    }

    SUBCASE("hyphens and underscores") {
      UrlParser p("shm://host/my-topic_name");
      CHECK_EQ(p.get_path(), "my-topic_name");
    }
  }

  TEST_CASE("query dictionary is empty when no query string is present") {
    UrlParser p("shm://host/path");

    CHECK(p.get_query_dictionary().empty());
  }

  TEST_CASE("to_string round-trip preserves transport host and path") {
    const std::string url = "shm://vehicle/speed";
    UrlParser p(url);
    std::string rebuilt = p.to_string();

    CHECK_FALSE(rebuilt.empty());
    CHECK_NE(rebuilt.find("shm"), std::string::npos);
    CHECK_NE(rebuilt.find("vehicle"), std::string::npos);
    CHECK_NE(rebuilt.find("speed"), std::string::npos);
  }

  TEST_CASE("to_string round-trip includes query key and value") {
    UrlParser p("dds://host/path?key=value");
    std::string s = p.to_string();

    CHECK_NE(s.find("key"), std::string::npos);
    CHECK_NE(s.find("value"), std::string::npos);
  }

  TEST_CASE("to_string round-trip includes fragment") {
    UrlParser p("url://host/path#frag");

    CHECK_NE(p.to_string().find("frag"), std::string::npos);
  }

  TEST_CASE("to_string returns non-empty for minimal url") {
    UrlParser p("intra://topic");

    CHECK_FALSE(p.to_string().empty());
  }

  TEST_CASE("non-hierarchical url stores transport and content") {
    UrlParser p("mailto:user@example.com", kN);

    CHECK_EQ(p.get_transport(), "mailto");
    CHECK_EQ(p.get_category(), kN);
    CHECK_FALSE(p.get_content().empty());
  }

  TEST_CASE("urn opaque uri is parsed as non-hierarchical") {
    UrlParser p("urn:isbn:978-3-16-148410-0", kN);

    CHECK_EQ(p.get_transport(), "urn");
    CHECK_EQ(p.get_category(), kN);
  }

  TEST_CASE("get_content throws for hierarchical url") {
    UrlParser p("dds://host/path");

    CHECK_THROWS((void)p.get_content());
  }

  TEST_CASE("category is preserved for hierarchical and non-hierarchical forms") {
    SUBCASE("hierarchical") {
      UrlParser p("shm://topic", kH);
      CHECK_EQ(p.get_category(), kH);
    }

    SUBCASE("non-hierarchical") {
      UrlParser p("data:text/plain,hello", kN);
      CHECK_EQ(p.get_category(), kN);
    }
  }

  TEST_CASE("construct from std::string produces same result as from const char pointer") {
    std::string url = "zenoh://robot/lidar/scan";
    UrlParser p(url);

    CHECK_EQ(p.get_transport(), "zenoh");
    CHECK_EQ(p.get_host(), "robot");
    CHECK_EQ(p.get_path(), "lidar/scan");
  }

  TEST_CASE("component-map constructor builds url from explicit parts") {
    std::map<UrlParser::Component, std::string> comps;
    comps[UrlParser::Component::kTransport] = "dds";
    comps[UrlParser::Component::kHost] = "my_host";
    comps[UrlParser::Component::kPath] = "my/path";

    UrlParser p(comps, kH, false);

    CHECK_EQ(p.get_transport(), "dds");
    CHECK_EQ(p.get_host(), "my_host");
  }

  TEST_CASE("component-map constructor with query produces query dictionary") {
    std::map<UrlParser::Component, std::string> comps;
    comps[UrlParser::Component::kTransport] = "intra";
    comps[UrlParser::Component::kHost] = "host";
    comps[UrlParser::Component::kPath] = "";
    comps[UrlParser::Component::kQuery] = "key=value";

    UrlParser p(comps, kH, false);

    const auto& dict = p.get_query_dictionary();
    REQUIRE_EQ(dict.count("key"), 1u);
    CHECK_EQ(dict.at("key"), "value");
  }

  TEST_CASE("component-map constructor with port stores numeric port") {
    std::map<UrlParser::Component, std::string> comps;
    comps[UrlParser::Component::kTransport] = "someip";
    comps[UrlParser::Component::kHost] = "127.0.0.1";
    comps[UrlParser::Component::kPath] = "";
    comps[UrlParser::Component::kPort] = "9090";

    UrlParser p(comps, kH, false);

    CHECK_EQ(p.get_port(), 9090);
  }

  TEST_CASE("component-map constructor with fragment stores fragment") {
    std::map<UrlParser::Component, std::string> comps;
    comps[UrlParser::Component::kTransport] = "fdbus";
    comps[UrlParser::Component::kHost] = "svc";
    comps[UrlParser::Component::kPath] = "";
    comps[UrlParser::Component::kFragment] = "ipc";

    UrlParser p(comps, kH, false);

    CHECK_EQ(p.get_fragment(), "ipc");
  }

  TEST_CASE("copy-and-replace constructor overrides selected components") {
    UrlParser original("shm://vehicle/speed");
    std::map<UrlParser::Component, std::string> repl;
    repl[UrlParser::Component::kTransport] = "dds";

    UrlParser modified(original, repl);

    CHECK_EQ(modified.get_transport(), "dds");
    CHECK_EQ(modified.get_host(), original.get_host());
    CHECK_EQ(modified.get_path(), original.get_path());
  }

  TEST_CASE("copy-and-replace constructor overrides host") {
    UrlParser original("dds://old_host/path");
    std::map<UrlParser::Component, std::string> repl;
    repl[UrlParser::Component::kHost] = "new_host";

    UrlParser modified(original, repl);

    CHECK_EQ(modified.get_host(), "new_host");
    CHECK_EQ(modified.get_transport(), "dds");
  }

  TEST_CASE("copy-and-replace constructor overrides query and rebuilds dictionary") {
    UrlParser original("dds://host/path?old=value");
    std::map<UrlParser::Component, std::string> repl;
    repl[UrlParser::Component::kQuery] = "new=data";

    UrlParser modified(original, repl);

    CHECK_EQ(modified.get_query(), "new=data");
    REQUIRE_EQ(modified.get_query_dictionary().count("new"), 1u);
    CHECK_EQ(modified.get_query_dictionary().at("new"), "data");
  }

  TEST_CASE("copy-and-replace constructor overrides fragment") {
    UrlParser original("dds://host/path#old_frag");
    std::map<UrlParser::Component, std::string> repl;
    repl[UrlParser::Component::kFragment] = "new_frag";

    UrlParser modified(original, repl);

    CHECK_EQ(modified.get_fragment(), "new_frag");
  }
}

// NOLINTEND
