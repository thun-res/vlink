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

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "../common_test.h"
#include "./extension/discovery_reporter.h"
#include "./extension/discovery_viewer.h"

TEST_SUITE("extension-DiscoveryViewer") {
  TEST_CASE("filter type enum values are sequential and distinct") {
    CHECK_EQ(static_cast<uint8_t>(DiscoveryViewer::kFilterNone), 0u);
    CHECK_EQ(static_cast<uint8_t>(DiscoveryViewer::kFilterAvailable), 1u);
    CHECK_EQ(static_cast<uint8_t>(DiscoveryViewer::kFilterNative), 2u);
  }

  TEST_CASE("get_listen_address returns a non-empty string") {
    CHECK_FALSE(DiscoveryViewer::get_listen_address().empty());
  }

  TEST_CASE("convert_type maps all known role tokens") {
    CHECK_EQ(DiscoveryViewer::convert_type("Ser"), kServer);
    CHECK_EQ(DiscoveryViewer::convert_type("Cli"), kClient);
    CHECK_EQ(DiscoveryViewer::convert_type("Pub"), kPublisher);
    CHECK_EQ(DiscoveryViewer::convert_type("Sub"), kSubscriber);
    CHECK_EQ(DiscoveryViewer::convert_type("Set"), kSetter);
    CHECK_EQ(DiscoveryViewer::convert_type("Get"), kGetter);
  }

  TEST_CASE("convert_type returns kUnknownImplType for unknown tokens") {
    CHECK_EQ(DiscoveryViewer::convert_type(""), kUnknownImplType);
    CHECK_EQ(DiscoveryViewer::convert_type("xyz"), kUnknownImplType);
  }

  TEST_CASE("convert_type_to_view produces correct paired strings for single roles") {
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kPublisher), "Pub|---");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kSubscriber), "---|Sub");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kSetter), "Set|---");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kGetter), "---|Get");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kServer), "Ser|---");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kClient), "---|Cli");
  }

  TEST_CASE("convert_type_to_view produces correct paired strings for combined roles") {
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kPublisher | kSubscriber), "Pub|Sub");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kSetter | kGetter), "Set|Get");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kServer | kClient), "Ser|Cli");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kPublisher | kGetter), "Pub|Get");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kSetter | kSubscriber), "Set|Sub");
  }

  TEST_CASE("convert_type_to_view with zero type returns unknown placeholder") {
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(0u), "???|???");
  }

  TEST_CASE("convert_type_to_view with process list counts instances per role") {
    std::vector<DiscoveryViewer::Process> procs;

    DiscoveryViewer::Process pub;
    pub.type = kPublisher;
    procs.push_back(pub);

    DiscoveryViewer::Process sub;
    sub.type = kSubscriber;
    procs.push_back(sub);

    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kPublisher | kSubscriber, procs), "Pub*1|Sub*1");
  }

  TEST_CASE("convert_type_to_view with process list uses tilde for large counts") {
    std::vector<DiscoveryViewer::Process> procs;

    for (int i = 0; i < 15; ++i) {
      DiscoveryViewer::Process p;
      p.type = kPublisher;
      procs.push_back(p);
    }

    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kPublisher, procs), "Pub*~|-----");
  }

  TEST_CASE("convert_type_to_view with process list and server client") {
    std::vector<DiscoveryViewer::Process> procs;

    DiscoveryViewer::Process srv;
    srv.type = kServer;
    procs.push_back(srv);
    procs.push_back(srv);

    DiscoveryViewer::Process cli;
    cli.type = kClient;
    procs.push_back(cli);

    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kServer | kClient, procs), "Ser*2|Cli*1");
  }

  TEST_CASE("convert_type_to_view with process list and setter getter") {
    std::vector<DiscoveryViewer::Process> procs;

    DiscoveryViewer::Process setter;
    setter.type = kSetter;
    procs.push_back(setter);

    DiscoveryViewer::Process getter;
    getter.type = kGetter;
    procs.push_back(getter);
    procs.push_back(getter);

    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kSetter | kGetter, procs), "Set*1|Get*2");
  }

  TEST_CASE("convert_type_to_view with zero type and empty process list returns unknown") {
    std::vector<DiscoveryViewer::Process> procs;
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(0u, procs), "?????|?????");
  }

  TEST_CASE("process default construction has zero pid, type and profiler -1") {
    DiscoveryViewer::Process p;
    CHECK_EQ(p.pid, 0u);
    CHECK_EQ(p.type, 0u);
    CHECK_EQ(p.profiler, doctest::Approx(-1.0));
    CHECK(p.host.empty());
    CHECK(p.name.empty());
    CHECK(p.ip.empty());
  }

  TEST_CASE("process operator< orders by host then by pid") {
    DiscoveryViewer::Process a;
    a.host = "alpha";
    a.pid = 100;

    DiscoveryViewer::Process b;
    b.host = "beta";
    b.pid = 1;

    CHECK(a < b);
    CHECK_FALSE(b < a);

    DiscoveryViewer::Process c;
    c.host = "same";
    c.pid = 10;

    DiscoveryViewer::Process d;
    d.host = "same";
    d.pid = 20;

    CHECK(c < d);
    CHECK_FALSE(d < c);
  }

  TEST_CASE("info default construction has sort_index -1 and empty fields") {
    DiscoveryViewer::Info info;
    CHECK_EQ(info.sort_index, -1);
    CHECK_EQ(info.type, 0u);
    CHECK(info.url.empty());
    CHECK(info.ser_type.empty());
    CHECK_EQ(info.schema_type, SchemaType::kUnknown);
    CHECK(info.process_list.empty());
  }

  TEST_CASE("info operator< orders by sort_index then by url") {
    DiscoveryViewer::Info a;
    a.sort_index = 1;
    a.url = "intra://z";

    DiscoveryViewer::Info b;
    b.sort_index = 2;
    b.url = "intra://a";

    CHECK(a < b);
    CHECK_FALSE(b < a);

    DiscoveryViewer::Info c;
    c.sort_index = 5;
    c.url = "intra://aaa";

    DiscoveryViewer::Info d;
    d.sort_index = 5;
    d.url = "intra://bbb";

    CHECK(c < d);
    CHECK_FALSE(d < c);
  }

  TEST_CASE("construction with each filter type does not throw") {
    {
      DiscoveryViewer v(DiscoveryViewer::kFilterNone);
    }
    {
      DiscoveryViewer v(DiscoveryViewer::kFilterAvailable);
    }
    {
      DiscoveryViewer v(DiscoveryViewer::kFilterNative);
    }
  }

  TEST_CASE("get_info_list returns empty vector before any report is received") {
    DiscoveryViewer viewer(DiscoveryViewer::kFilterNone);
    CHECK(viewer.get_info_list().empty());
  }

  TEST_CASE("get_ser_type returns empty string for unknown url") {
    DiscoveryViewer viewer(DiscoveryViewer::kFilterNone);
    CHECK(viewer.get_ser_type("intra://nonexistent").empty());
  }

  TEST_CASE("get_schema_type returns kUnknown for unknown url") {
    DiscoveryViewer viewer(DiscoveryViewer::kFilterNone);
    CHECK_EQ(viewer.get_schema_type("intra://nonexistent"), SchemaType::kUnknown);
  }

  TEST_CASE("register_callback does not crash") {
    DiscoveryViewer viewer(DiscoveryViewer::kFilterNone);
    viewer.register_callback([](const std::vector<DiscoveryViewer::Info>&) {});
  }

  TEST_CASE("global_get returns the same non-null singleton on repeated calls") {
    DiscoveryViewer* g1 = DiscoveryViewer::global_get();
    DiscoveryViewer* g2 = DiscoveryViewer::global_get();
    REQUIRE_NE(g1, nullptr);
    CHECK_EQ(g1, g2);
  }
}

TEST_SUITE("extension-DiscoveryReporter") {
  TEST_CASE("construction and destruction do not crash") { DiscoveryReporter reporter; }

  TEST_CASE("global_get returns the same non-null singleton on repeated calls") {
    DiscoveryReporter* g1 = DiscoveryReporter::global_get();
    DiscoveryReporter* g2 = DiscoveryReporter::global_get();
    REQUIRE_NE(g1, nullptr);
    CHECK_EQ(g1, g2);
  }
}

// NOLINTEND
