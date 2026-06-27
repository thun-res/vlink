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

#include <algorithm>
#include <cstdint>
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
    CHECK_EQ(DiscoveryViewer::convert_type("pub"), kUnknownImplType);
    CHECK_EQ(DiscoveryViewer::convert_type(" Pub"), kUnknownImplType);
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
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kPublisher | kSetter), "Pub|---");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kPublisher | kSetter | kSubscriber), "Pub|Sub");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kPublisher | kSetter | kGetter), "Pub|Get");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kPublisher | kSetter | kSubscriber | kGetter), "Pub|Sub");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kSubscriber | kGetter), "---|Sub");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kSubscriber | kGetter | kPublisher), "Pub|Sub");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kSubscriber | kGetter | kSetter), "Set|Sub");
  }

  TEST_CASE("convert_type_to_view with zero type returns unknown placeholder") {
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(0u), "???|???");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kServer | kPublisher), "???|???");
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

  TEST_CASE("convert_type_to_view with process list covers single role labels") {
    std::vector<DiscoveryViewer::Process> procs;

    DiscoveryViewer::Process pub;
    pub.type = kPublisher;
    procs.push_back(pub);

    DiscoveryViewer::Process sub;
    sub.type = kSubscriber;
    procs.push_back(sub);

    DiscoveryViewer::Process setter;
    setter.type = kSetter;
    procs.push_back(setter);

    DiscoveryViewer::Process getter;
    getter.type = kGetter;
    procs.push_back(getter);

    DiscoveryViewer::Process server;
    server.type = kServer;
    procs.push_back(server);

    DiscoveryViewer::Process client;
    client.type = kClient;
    procs.push_back(client);

    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kPublisher, procs), "Pub*1|-----");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kSubscriber, procs), "-----|Sub*1");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kSetter, procs), "Set*1|-----");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kGetter, procs), "-----|Get*1");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kServer, procs), "Ser*1|-----");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kClient, procs), "-----|Cli*1");
  }

  TEST_CASE("convert_type_to_view with process list covers mixed publisher setter getter combinations") {
    std::vector<DiscoveryViewer::Process> procs;

    for (auto type : {kPublisher, kSetter, kSubscriber, kGetter, kServer, kClient}) {
      DiscoveryViewer::Process process;
      process.type = type;
      procs.push_back(process);
    }

    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kPublisher | kGetter, procs), "Pub*1|Get*1");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kSetter | kSubscriber, procs), "Set*1|Sub*1");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kPublisher | kSetter, procs), "Pub*2|-----");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kPublisher | kSetter | kSubscriber, procs), "Pub*2|Sub*1");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kPublisher | kSetter | kGetter, procs), "Pub*2|Get*1");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kPublisher | kSetter | kSubscriber | kGetter, procs), "Pub*2|Sub*2");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kSubscriber | kGetter, procs), "-----|Sub*2");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kSubscriber | kGetter | kPublisher, procs), "Pub*1|Sub*2");
    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kSubscriber | kGetter | kSetter, procs), "Set*1|Sub*2");
  }

  TEST_CASE("convert_type_to_view with process list returns unknown for unsupported combinations") {
    std::vector<DiscoveryViewer::Process> procs;
    DiscoveryViewer::Process process;
    process.type = kPublisher;
    procs.push_back(process);

    CHECK_EQ(DiscoveryViewer::convert_type_to_view(kServer | kPublisher, procs), "?????|?????");
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

    DiscoveryViewer::Process by_type_a;
    by_type_a.type = kPublisher;
    DiscoveryViewer::Process by_type_b;
    by_type_b.type = kSubscriber;
    CHECK(by_type_a < by_type_b);

    DiscoveryViewer::Process by_ip_a;
    by_ip_a.host = "same";
    by_ip_a.ip = "10.0.0.1";
    DiscoveryViewer::Process by_ip_b;
    by_ip_b.host = "same";
    by_ip_b.ip = "10.0.0.2";
    CHECK(by_ip_a < by_ip_b);

    DiscoveryViewer::Process by_name_a;
    by_name_a.host = "same";
    by_name_a.ip = "same";
    by_name_a.name = "alpha";
    DiscoveryViewer::Process by_name_b;
    by_name_b.host = "same";
    by_name_b.ip = "same";
    by_name_b.name = "beta";
    CHECK(by_name_a < by_name_b);

    DiscoveryViewer::Process by_pid_a = by_name_a;
    by_pid_a.name = "same";
    by_pid_a.pid = 1;
    DiscoveryViewer::Process by_pid_b = by_pid_a;
    by_pid_b.pid = 2;
    CHECK(by_pid_a < by_pid_b);

    CHECK_FALSE(by_pid_a < by_pid_a);
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

    DiscoveryViewer::Info by_type_a;
    by_type_a.type = kPublisher;
    DiscoveryViewer::Info by_type_b;
    by_type_b.type = kSubscriber;
    CHECK(by_type_a < by_type_b);

    DiscoveryViewer::Info by_schema_a;
    by_schema_a.type = kPublisher;
    by_schema_a.sort_index = 1;
    by_schema_a.url = "same";
    by_schema_a.schema_type = SchemaType::kRaw;
    DiscoveryViewer::Info by_schema_b = by_schema_a;
    by_schema_b.schema_type = SchemaType::kProtobuf;
    CHECK(by_schema_a < by_schema_b);

    DiscoveryViewer::Info by_ser_a = by_schema_a;
    by_ser_a.schema_type = SchemaType::kRaw;
    by_ser_a.ser_type = "alpha";
    DiscoveryViewer::Info by_ser_b = by_ser_a;
    by_ser_b.ser_type = "beta";
    CHECK(by_ser_a < by_ser_b);

    DiscoveryViewer::Info by_process_a = by_ser_a;
    by_process_a.ser_type = "same";
    DiscoveryViewer::Process process_a;
    process_a.pid = 1;
    by_process_a.process_list.push_back(process_a);
    DiscoveryViewer::Info by_process_b = by_process_a;
    by_process_b.process_list.front().pid = 2;
    CHECK(by_process_a < by_process_b);

    CHECK_FALSE(by_process_a < by_process_a);
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
}

TEST_SUITE("extension-DiscoveryReporter") {
  TEST_CASE("construction and destruction do not crash") { DiscoveryReporter reporter; }
}

// NOLINTEND
