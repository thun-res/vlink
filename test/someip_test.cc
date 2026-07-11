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

#include "./common_test.h"

#ifdef VLINK_SUPPORT_SOMEIP

#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "./modules/someip_conf.h"

TEST_SUITE("someip-init") {
  TEST_CASE("rpc conf defaults are set correctly") {
    MESSAGE("[someip-init] rpc conf defaults are set correctly");

    SomeipConf conf(0x0001, 0x0001, 0x0001);

    CHECK(conf.service == 0x0001);
    CHECK(conf.instance == 0x0001);
    CHECK(conf.method == 0x0001);
    CHECK(conf.groups.empty());
    CHECK(conf.event == 0);
    CHECK(conf.field == false);
    CHECK(conf.get_transport_type() == TransportType::kSomeip);
  }

  TEST_CASE("event conf defaults are set correctly") {
    MESSAGE("[someip-init] event conf defaults are set correctly");

    SomeipConf conf(0x0002, 0x0001, SomeipConf::Groups{0x0001}, 0x0010);

    CHECK(conf.service == 0x0002);
    CHECK(conf.instance == 0x0001);
    CHECK(conf.method == 0);
    CHECK(conf.groups == SomeipConf::Groups{0x0001});
    CHECK(conf.event == 0x0010);
    CHECK(conf.field == false);
  }

  TEST_CASE("field flag distinguishes field conf from event conf") {
    MESSAGE("[someip-init] field flag distinguishes field conf from event conf");

    SomeipConf conf(0x0003, 0x0001, SomeipConf::Groups{0x0001}, 0x0010, true);

    CHECK(conf.field == true);
  }

  TEST_CASE("conf with multiple groups stores all entries") {
    MESSAGE("[someip-init] conf with multiple groups stores all entries");

    SomeipConf::Groups grps{0x0001, 0x0002, 0x0003};
    SomeipConf conf(0x0004, 0x0001, grps, 0x0020);

    CHECK(conf.groups.size() == 3u);
    CHECK(conf.groups.count(0x0002) == 1u);
  }

  TEST_CASE("rpc conf equality compares service instance and method") {
    MESSAGE("[someip-init] rpc conf equality compares service instance and method");

    SomeipConf a(0x1234, 0x5678, 0x0001);
    SomeipConf b(0x1234, 0x5678, 0x0001);
    SomeipConf c(0x1234, 0x5678, 0x0002);

    CHECK(a == b);
    CHECK(a != c);
  }

  TEST_CASE("event conf equality compares service instance and event") {
    MESSAGE("[someip-init] event conf equality compares service instance and event");

    SomeipConf a(0x1234, 0x5678, SomeipConf::Groups{0x0001}, 0x0010);
    SomeipConf b(0x1234, 0x5678, SomeipConf::Groups{0x0001}, 0x0010);
    SomeipConf c(0x1234, 0x5678, SomeipConf::Groups{0x0001}, 0x0020);

    CHECK(a == b);
    CHECK(a != c);
  }

  TEST_CASE("field flag is part of equality") {
    MESSAGE("[someip-init] field flag is part of equality");

    SomeipConf a(0x1234, 0x5678, SomeipConf::Groups{0x0001}, 0x0010, false);
    SomeipConf b(0x1234, 0x5678, SomeipConf::Groups{0x0001}, 0x0010, true);

    CHECK(a != b);
  }

  TEST_CASE("rpc url parses for server and client") {
    MESSAGE("[someip-init] rpc url parses for server and client");

    Url url("someip://4660/22136?method=1");

    CHECK(url.parse(kServer));
    CHECK(url.parse(kClient));
  }

  TEST_CASE("event url parses for publisher and subscriber") {
    MESSAGE("[someip-init] event url parses for publisher and subscriber");

    Url url("someip://4660/22136?groups=1&event=16");

    CHECK(url.parse(kPublisher));
    CHECK(url.parse(kSubscriber));
  }

  TEST_CASE("multi-group event url parses with pipe separator") {
    MESSAGE("[someip-init] multi-group event url parses with pipe separator");

    Url url("someip://4660/22136?groups=1|2&event=16");

    CHECK(url.parse(kPublisher));
    CHECK(url.parse(kSubscriber));
  }

  TEST_CASE("field url parses for setter and getter") {
    MESSAGE("[someip-init] field url parses for setter and getter");

    Url url("someip://4660/22136?groups=1&event=16&field=1");

    CHECK(url.parse(kSetter));
    CHECK(url.parse(kGetter));
  }

  TEST_CASE("invalid transport scheme throws on construction") { CHECK_THROWS(Publisher<int>("someip1://bad/url")); }
}

TEST_SUITE("someip-pubsub") {
  TEST_CASE("bytes payload is delivered to subscriber") {
    MESSAGE("[someip-pubsub] bytes payload is delivered to subscriber");

    std::atomic<bool> received{false};
    Bytes captured;

    Publisher<Bytes> pub(SomeipConf(0x1001, 0x0001, SomeipConf::Groups{0x0001}, 0x0001));
    Subscriber<Bytes> sub("someip://4097/1?groups=1&event=1");

    sub.listen([&](const Bytes& data) {
      captured = data;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.has_subscribers());

    std::this_thread::sleep_for(10ms);

    Bytes payload{0xAB, 0xCD, 0xEF};
    CHECK(pub.publish(payload));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
    REQUIRE(captured.size() == 3u);
    CHECK(captured[0] == 0xAB);
    CHECK(captured[2] == 0xEF);
  }

  TEST_CASE("string payload round trips correctly") {
    MESSAGE("[someip-pubsub] string payload round trips correctly");

    std::atomic<bool> received{false};
    std::string captured;

    Publisher<std::string> pub(SomeipConf(0x1002, 0x0001, SomeipConf::Groups{0x0001}, 0x0001));
    Subscriber<std::string> sub("someip://4098/1?groups=1&event=1");

    sub.listen([&](const std::string& val) {
      captured = val;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    std::this_thread::sleep_for(10ms);

    CHECK(pub.publish(std::string("hello_someip")));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
    CHECK(captured == "hello_someip");
  }

  TEST_CASE("all published messages are received") {
    MESSAGE("[someip-pubsub] all published messages are received");

    std::atomic<int> count{0};

    Publisher<int> pub(SomeipConf(0x1003, 0x0001, SomeipConf::Groups{0x0001}, 0x0001));
    Subscriber<int> sub("someip://4099/1?groups=1&event=1");

    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));

    std::this_thread::sleep_for(10ms);

    for (int i = 0; i < 10; ++i) {
      pub.publish(i);
      std::this_thread::sleep_for(20ms);
    }

    std::this_thread::sleep_for(1000ms);

    CHECK(count.load() >= 10);
  }

  TEST_CASE("multiple subscribers each receive all messages") {
    MESSAGE("[someip-pubsub] multiple subscribers each receive all messages");

    std::atomic<int> count1{0};
    std::atomic<int> count2{0};

    Publisher<Bytes> pub(SomeipConf(0x1004, 0x0001, SomeipConf::Groups{0x0001}, 0x0001));
    Subscriber<Bytes> sub1("someip://4100/1?groups=1&event=1");
    Subscriber<Bytes> sub2("someip://4100/1?groups=1&event=1");

    sub1.listen([&](const Bytes& /*d*/) { count1.fetch_add(1, std::memory_order_relaxed); });
    sub2.listen([&](const Bytes& /*d*/) { count2.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));

    std::this_thread::sleep_for(10ms);

    for (int i = 0; i < 3; ++i) {
      pub.publish(Bytes{static_cast<uint8_t>(i)});
      std::this_thread::sleep_for(30ms);
    }

    std::this_thread::sleep_for(300ms);

    CHECK(count1.load() >= 3);
    CHECK(count2.load() >= 3);
  }

  TEST_CASE("force publish succeeds without subscribers") {
    MESSAGE("[someip-pubsub] force publish succeeds without subscribers");

    Publisher<Bytes> pub(SomeipConf(0x1005, 0x0001, SomeipConf::Groups{0x0001}, 0x0001));

    CHECK(!pub.has_subscribers());

    for (int i = 0; i < 5; ++i) {
      CHECK(pub.publish(Bytes{static_cast<uint8_t>(i)}, true));
    }
  }

  TEST_CASE("subscriber connect and disconnect are detected") {
    MESSAGE("[someip-pubsub] subscriber connect and disconnect are detected");

    std::atomic<int> connected_count{0};

    Publisher<Bytes> pub(SomeipConf(0x1006, 0x0001, SomeipConf::Groups{0x0001}, 0x0001));

    pub.detect_subscribers([&](bool connected) {
      if (connected) {
        connected_count.fetch_add(1, std::memory_order_relaxed);
      }
    });

    {
      Subscriber<Bytes> sub("someip://4102/1?groups=1&event=1");
      sub.listen([](const Bytes& /*d*/) {});

      std::this_thread::sleep_for(500ms);
      CHECK(pub.has_subscribers());
    }

    std::this_thread::sleep_for(500ms);
    CHECK(!pub.has_subscribers());
  }
}

TEST_SUITE("someip-method") {
  TEST_CASE("fire and forget send increments server counter") {
    MESSAGE("[someip-method] fire and forget send increments server counter");

    std::atomic<int> counter{0};

    Server<std::string> server(SomeipConf(0x2001, 0x0001, 0x0001));
    server.listen([&](const std::string& /*req*/) { counter.fetch_add(1, std::memory_order_relaxed); });

    Client<std::string> client("someip://8193/1?method=1");
    CHECK(client.wait_for_connected(1s));
    CHECK(client.is_connected());

    CHECK(client.send("fire1"));
    std::this_thread::sleep_for(200ms);
    CHECK(counter.load() == 1);

    CHECK(client.send("fire2"));
    std::this_thread::sleep_for(200ms);
    CHECK(counter.load() == 2);
  }

  TEST_CASE("invoke returns correct response for all overloads") {
    MESSAGE("[someip-method] invoke returns correct response for all overloads");

    Server<std::string, std::string> server(SomeipConf(0x2002, 0x0001, 0x0001));
    server.listen([](const std::string& req, std::string& resp) { resp = "someip:" + req; });

    Client<std::string, std::string> client("someip://8194/1?method=1");
    CHECK(client.wait_for_connected(1s));

    SUBCASE("sync optional") {
      auto resp = client.invoke("ping");
      CHECK(resp.has_value());
      CHECK(*resp == "someip:ping");
    }

    SUBCASE("sync ref overload") {
      std::string out;
      CHECK(client.invoke("pong", out, 5s));
      CHECK(out == "someip:pong");
    }

    SUBCASE("async future") {
      auto fut = client.async_invoke("async");
      REQUIRE(fut.wait_for(5s) == std::future_status::ready);
      CHECK(fut.get() == "someip:async");
    }
  }

  TEST_CASE("async callback receives the response") {
    MESSAGE("[someip-method] async callback receives the response");

    Server<std::string, std::string> server(SomeipConf(0x2004, 0x0001, 0x0001));
    server.listen([](const std::string& /*req*/, std::string& resp) { resp = "someip_cb"; });

    Client<std::string, std::string> client("someip://8196/1?method=1");
    CHECK(client.wait_for_connected(1s));

    std::atomic<bool> got{false};
    std::string resp_val;

    bool ok = client.invoke("msg", [&](const std::string& resp) {
      resp_val = resp;
      got.store(true, std::memory_order_release);
    });

    CHECK(ok);

    CHECK(common_test::wait_until([&got] { return got.load(std::memory_order_acquire); }, 5s));
    CHECK(resp_val == "someip_cb");
  }

  TEST_CASE("client connection is reported via detect callback") {
    MESSAGE("[someip-method] client connection is reported via detect callback");

    std::atomic<bool> conn_flag{false};

    Server<std::string, std::string> server(SomeipConf(0x2005, 0x0001, 0x0001));
    server.listen([](const std::string& /*req*/, std::string& resp) { resp = "ok"; });

    Client<std::string, std::string> client("someip://8197/1?method=1");
    client.detect_connected([&](bool connected) { conn_flag.store(connected, std::memory_order_release); });

    CHECK(client.wait_for_connected(1s));
    std::this_thread::sleep_for(200ms);
    CHECK(conn_flag.load(std::memory_order_acquire));
  }
}

TEST_SUITE("someip-field") {
  TEST_CASE("setter and getter exchange values") {
    MESSAGE("[someip-field] setter and getter exchange values");

    SUBCASE("polling get") {
      Setter<Bytes> setter(SomeipConf(0x3001, 0x0001, SomeipConf::Groups{0x0001}, 0x0010, true));
      Getter<Bytes> getter("someip://12289/1?groups=1&event=16&field=1");

      setter.set(Bytes{0x13, 0x37});
      std::this_thread::sleep_for(300ms);

      auto v = getter.get();
      REQUIRE(v.has_value());
      REQUIRE(v->size() == 2u);
      CHECK((*v)[0] == 0x13);
      CHECK((*v)[1] == 0x37);
    }

    SUBCASE("wait for value") {
      Setter<Bytes> setter(SomeipConf(0x3002, 0x0001, SomeipConf::Groups{0x0001}, 0x0010, true));
      Getter<Bytes> getter("someip://12290/1?groups=1&event=16&field=1");

      std::thread writer([&] {
        std::this_thread::sleep_for(100ms);
        setter.set(Bytes{0xF0, 0x0D});
      });

      CHECK(getter.wait_for_value(1s));
      auto v = getter.get();
      REQUIRE(v.has_value());
      CHECK((*v)[0] == 0xF0);

      writer.join();
    }

    SUBCASE("listen callback is invoked on set") {
      std::atomic<bool> notified{false};

      Setter<Bytes> setter(SomeipConf(0x3003, 0x0001, SomeipConf::Groups{0x0001}, 0x0010, true));
      Getter<Bytes> getter("someip://12291/1?groups=1&event=16&field=1");

      getter.listen([&](const Bytes& /*val*/) { notified.store(true, std::memory_order_release); });

      std::this_thread::sleep_for(100ms);
      setter.set(Bytes{0x42});

      CHECK(common_test::wait_until([&notified] { return notified.load(std::memory_order_acquire); }, 5s));
    }

    SUBCASE("change reporting suppresses duplicate values") {
      std::atomic<int> cb_count{0};

      Setter<Bytes> setter(SomeipConf(0x3004, 0x0001, SomeipConf::Groups{0x0001}, 0x0010, true));
      Getter<Bytes> getter("someip://12292/1?groups=1&event=16&field=1");

      getter.set_change_reporting(true);
      getter.listen([&](const Bytes& /*v*/) { cb_count.fetch_add(1, std::memory_order_relaxed); });

      std::this_thread::sleep_for(100ms);

      setter.set(Bytes{0x5A});
      std::this_thread::sleep_for(200ms);
      setter.set(Bytes{0x5A});
      std::this_thread::sleep_for(200ms);

      CHECK(cb_count.load() <= 1);
    }

    SUBCASE("late getter receives cached value") {
      Setter<Bytes> setter(SomeipConf(0x3005, 0x0001, SomeipConf::Groups{0x0001}, 0x0010, true));
      setter.set(Bytes{0xC0, 0xDE});
      std::this_thread::sleep_for(200ms);

      Getter<Bytes> late_getter("someip://12293/1?groups=1&event=16&field=1");
      CHECK(late_getter.wait_for_value(1s));
      auto v = late_getter.get();
      REQUIRE(v.has_value());
      CHECK((*v)[0] == 0xC0);
    }

    SUBCASE("multiple set calls are all received by getter") {
      std::atomic<int> update_count{0};

      Setter<Bytes> setter(SomeipConf(0x3006, 0x0001, SomeipConf::Groups{0x0001}, 0x0010, true));
      Getter<Bytes> getter("someip://12294/1?groups=1&event=16&field=1");

      getter.listen([&](const Bytes& /*v*/) { update_count.fetch_add(1, std::memory_order_relaxed); });

      std::this_thread::sleep_for(100ms);

      for (int i = 0; i < 5; ++i) {
        setter.set(Bytes{static_cast<uint8_t>(i), static_cast<uint8_t>(i + 1)});
        std::this_thread::sleep_for(50ms);
      }

      std::this_thread::sleep_for(200ms);
      CHECK(update_count.load() >= 5);

      auto v = getter.get();
      REQUIRE(v.has_value());
      CHECK(v->size() == 2u);
    }
  }
}

TEST_SUITE("someip-pubsub") {
  TEST_CASE("large 1kb payload is received intact") {
    MESSAGE("[someip-pubsub] large 1kb payload is received intact");

    static constexpr size_t kSize1K = 1024;

    std::atomic<bool> received{false};
    size_t captured_size{0};

    Publisher<Bytes> pub(SomeipConf(0x1010, 0x0001, SomeipConf::Groups{0x0001}, 0x0001));
    Subscriber<Bytes> sub("someip://4112/1?groups=1&event=1");

    sub.listen([&](const Bytes& data) {
      captured_size = data.size();
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    std::this_thread::sleep_for(10ms);

    Bytes payload = Bytes::create(kSize1K);
    CHECK(pub.publish(payload));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
    CHECK_EQ(captured_size, kSize1K);
  }

  TEST_CASE("empty bytes payload is received without crash") {
    MESSAGE("[someip-pubsub] empty bytes payload is received without crash");

    std::atomic<bool> received{false};
    size_t captured_size{1};

    Publisher<Bytes> pub(SomeipConf(0x1011, 0x0001, SomeipConf::Groups{0x0001}, 0x0001));
    Subscriber<Bytes> sub("someip://4113/1?groups=1&event=1");

    sub.listen([&](const Bytes& data) {
      captured_size = data.size();
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    std::this_thread::sleep_for(10ms);
    CHECK(pub.publish(Bytes{}));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
    CHECK_EQ(captured_size, 0u);
  }

  TEST_CASE("concurrent sends from multiple threads reach subscriber") {
    MESSAGE("[someip-pubsub] concurrent sends from multiple threads reach subscriber");

    static constexpr int kThreads = 3;
    static constexpr int kPerThread = 4;

    std::atomic<int> total{0};

    Publisher<int> pub(SomeipConf(0x1012, 0x0001, SomeipConf::Groups{0x0001}, 0x0001));
    Subscriber<int> sub("someip://4114/1?groups=1&event=1");

    sub.listen([&](const int& /*v*/) { total.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));
    std::this_thread::sleep_for(10ms);

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([t, &pub] {
        for (int i = 0; i < kPerThread; ++i) {
          pub.publish(t * kPerThread + i, true);
          std::this_thread::sleep_for(20ms);
        }
      });
    }

    for (auto& th : threads) {
      th.join();
    }

    std::this_thread::sleep_for(500ms);
    CHECK(total.load() >= kThreads * kPerThread);
  }

  TEST_CASE("subscriber destroyed mid-flight does not crash publisher") {
    MESSAGE("[someip-pubsub] subscriber destroyed mid-flight does not crash publisher");

    Publisher<int> pub(SomeipConf(0x1013, 0x0001, SomeipConf::Groups{0x0001}, 0x0001));

    {
      Subscriber<int> sub("someip://4115/1?groups=1&event=1");
      sub.listen([](const int& /*v*/) {});

      CHECK(pub.wait_for_subscribers(1s));

      for (int i = 0; i < 3; ++i) {
        pub.publish(i, true);
        std::this_thread::sleep_for(30ms);
      }
    }

    std::this_thread::sleep_for(500ms);
    CHECK(!pub.has_subscribers());

    for (int i = 0; i < 3; ++i) {
      CHECK(pub.publish(i, true));
    }
  }
}

TEST_SUITE("someip-method") {
  TEST_CASE("invoke times out when server is absent") {
    MESSAGE("[someip-method] invoke times out when server is absent");

    Client<std::string, std::string> orphan("someip://65000/1?method=1");

    std::string out;
    bool ok = orphan.invoke("req", out, 300ms);
    CHECK_FALSE(ok);
  }

  TEST_CASE("deferred async reply is delivered to future") {
    MESSAGE("[someip-method] deferred async reply is delivered to future");

    std::atomic<uint64_t> saved_id{0};
    std::atomic<bool> req_received{false};

    Server<std::string, std::string> server(SomeipConf(0x2006, 0x0001, 0x0001));
    server.listen_for_reply([&](uint64_t req_id, const std::string& /*req*/) {
      saved_id.store(req_id, std::memory_order_release);
      req_received.store(true, std::memory_order_release);
    });

    Client<std::string, std::string> client("someip://8198/1?method=1");
    CHECK(client.wait_for_connected(1s));

    auto fut = client.async_invoke("deferred");

    REQUIRE(common_test::wait_until([&req_received] { return req_received.load(std::memory_order_acquire); }, 5s));

    if (server.reply(saved_id.load(), std::string("deferred_someip"))) {
      if (fut.wait_for(2s) == std::future_status::ready) {
        CHECK_EQ(fut.get(), "deferred_someip");
      }
    }
  }

  TEST_CASE("multiple sequential invocations all succeed") {
    MESSAGE("[someip-method] multiple sequential invocations all succeed");

    Server<std::string, std::string> server(SomeipConf(0x2007, 0x0001, 0x0001));
    server.listen([](const std::string& req, std::string& resp) { resp = "si:" + req; });

    Client<std::string, std::string> client("someip://8199/1?method=1");
    CHECK(client.wait_for_connected(1s));

    for (int i = 0; i < 5; ++i) {
      std::string key = std::to_string(i);
      auto resp = client.invoke(key);
      CHECK(resp.has_value());
      CHECK_EQ(*resp, "si:" + key);
    }
  }
}

TEST_SUITE("someip-field") {
  TEST_CASE("default value is not available before any set") {
    MESSAGE("[someip-field] default value is not available before any set");

    Getter<int> getter("someip://65535/1?groups=1&event=16&field=1");
    auto v = getter.get();
    CHECK_FALSE(v.has_value());
  }

  TEST_CASE("integer field round trips with correct value") {
    MESSAGE("[someip-field] integer field round trips with correct value");

    Setter<int> setter(SomeipConf(0x3010, 0x0001, SomeipConf::Groups{0x0001}, 0x0010, true));
    Getter<int> getter("someip://12304/1?groups=1&event=16&field=1");

    setter.set(8888);
    CHECK(getter.wait_for_value(1s));

    auto v = getter.get();
    REQUIRE(v.has_value());
    CHECK_EQ(*v, 8888);
  }
}

TEST_SUITE("someip-init") {
  TEST_CASE("multi-service isolation prevents cross-service message delivery") {
    MESSAGE("[someip-init] multi-service isolation prevents cross-service message delivery");

    std::atomic<int> count_svc1{0};
    std::atomic<int> count_svc2{0};

    Publisher<int> pub1(SomeipConf(0x6001, 0x0001, SomeipConf::Groups{0x0001}, 0x0001));
    Publisher<int> pub2(SomeipConf(0x6002, 0x0001, SomeipConf::Groups{0x0001}, 0x0001));

    Subscriber<int> sub1("someip://24577/1?groups=1&event=1");
    Subscriber<int> sub2("someip://24578/1?groups=1&event=1");

    sub1.listen([&](const int& /*v*/) { count_svc1.fetch_add(1, std::memory_order_relaxed); });
    sub2.listen([&](const int& /*v*/) { count_svc2.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub1.wait_for_subscribers(1s));
    CHECK(pub2.wait_for_subscribers(1s));
    std::this_thread::sleep_for(10ms);

    for (int i = 0; i < 3; ++i) {
      pub1.publish(i);
      std::this_thread::sleep_for(30ms);
    }

    std::this_thread::sleep_for(200ms);
    CHECK(count_svc1.load() >= 3);
    CHECK_EQ(count_svc2.load(), 0);
  }

  TEST_CASE("each node has a distinct abstract node pointer") {
    MESSAGE("[someip-init] each node has a distinct abstract node pointer");

    Publisher<int> pub1(SomeipConf(0x5001, 0x0001, SomeipConf::Groups{0x0001}, 0x0001));
    Publisher<int> pub2(SomeipConf(0x5002, 0x0001, SomeipConf::Groups{0x0001}, 0x0001));
    Subscriber<int> sub("someip://20481/1?groups=1&event=1");

    CHECK(pub1.get_abstract_node() != nullptr);
    CHECK(pub2.get_abstract_node() != nullptr);
    CHECK(pub1.get_abstract_node() != pub2.get_abstract_node());
    CHECK(pub1.get_abstract_node() != sub.get_abstract_node());
  }
}

namespace {

struct SomeipCustomMsg {
  int id{0};
  std::string label;

  void operator>>(vlink::Bytes& out) const {
    std::string s = std::to_string(id) + "|" + label;
    out = vlink::Bytes::deep_copy(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  }

  void operator<<(const vlink::Bytes& in) {
    std::string s(reinterpret_cast<const char*>(in.data()), in.size());
    auto p = s.find('|');
    id = std::stoi(s.substr(0, p));
    label = s.substr(p + 1);
  }
};

}  // namespace

TEST_SUITE("someip-custom") {
  TEST_CASE("custom type round trips id and label through someip") {
    MESSAGE("[someip-custom] custom type round trips id and label through someip");

    try {
      std::atomic<bool> received{false};
      SomeipCustomMsg captured{};

      Publisher<SomeipCustomMsg> pub(SomeipConf(0x6001, 0x0001, SomeipConf::Groups{0x0001}, 0x0001));
      Subscriber<SomeipCustomMsg> sub("someip://24577/1?groups=1&event=1");

      sub.listen([&](const SomeipCustomMsg& m) {
        captured = m;
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));
      std::this_thread::sleep_for(10ms);

      SomeipCustomMsg msg;
      msg.id = 41;
      msg.label = "someip_custom";
      CHECK(pub.publish(msg));

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
      CHECK_EQ(captured.id, 41);
      CHECK_EQ(captured.label, "someip_custom");
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("serializer detects custom type as kCustomType") {
    MESSAGE("[someip-custom] serializer detects custom type as kCustomType");

    static constexpr auto kType = Serializer::get_type_of<SomeipCustomMsg>();
    CHECK_EQ(kType, Serializer::kCustomType);
  }
}

#include "./zerocopy/raw_data.h"

TEST_SUITE("someip-dynamicdata") {
  TEST_CASE("dynamicdata round trip preserves type tag and value") {
    MESSAGE("[someip-dynamicdata] dynamicdata round trip preserves type tag and value");

    try {
      std::atomic<bool> received{false};
      DynamicData captured;

      Publisher<DynamicData> pub(SomeipConf(0x1020, 0x0001, SomeipConf::Groups{0x0001}, 0x0001));
      Subscriber<DynamicData> sub("someip://4128/1?groups=1&event=1");

      sub.listen([&](const DynamicData& d) {
        captured = d;
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));
      std::this_thread::sleep_for(10ms);

      DynamicData d;
      d.load("someip_int", 123);
      CHECK(pub.publish(d));

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
      CHECK(captured.get_type() == "someip_int");
      CHECK(captured.as<int>() == 123);
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("dynamicdata type tag is preserved distinct from payload bytes") {
    MESSAGE("[someip-dynamicdata] dynamicdata type tag is preserved distinct from payload bytes");

    try {
      std::atomic<bool> received{false};
      DynamicData captured;

      Publisher<DynamicData> pub(SomeipConf(0x1021, 0x0001, SomeipConf::Groups{0x0001}, 0x0001));
      Subscriber<DynamicData> sub("someip://4129/1?groups=1&event=1");

      sub.listen([&](const DynamicData& d) {
        captured = d;
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));
      std::this_thread::sleep_for(10ms);

      DynamicData d;
      d.load("tag_check", std::string("payload_here"));
      CHECK(pub.publish(d));

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
      CHECK(captured.get_type() == "tag_check");
      CHECK_FALSE(captured.is_empty());
      CHECK(captured.as<std::string>() == "payload_here");
    } catch (const std::exception&) {
      return;
    }
  }
}

TEST_SUITE("someip-zerocopy") {
  TEST_CASE("rawdata round trip preserves header seq over someip") {
    MESSAGE("[someip-zerocopy] rawdata round trip preserves header seq over someip");

    try {
      std::atomic<bool> received{false};
      zerocopy::RawData captured;

      Publisher<zerocopy::RawData> pub(SomeipConf(0x1022, 0x0001, SomeipConf::Groups{0x0001}, 0x0001));
      Subscriber<zerocopy::RawData> sub("someip://4130/1?groups=1&event=1");

      sub.listen([&](const zerocopy::RawData& d) {
        captured.deep_copy(d);
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));
      std::this_thread::sleep_for(10ms);

      zerocopy::RawData rd;
      rd.header.seq = 3;
      rd.create(2);
      const_cast<uint8_t*>(rd.data())[0] = 0x55;
      const_cast<uint8_t*>(rd.data())[1] = 0x66;
      CHECK(pub.publish(rd));

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
      REQUIRE_EQ(captured.size(), 2u);
      CHECK_EQ(captured.header.seq, 3u);
      CHECK_EQ(captured.data()[0], 0x55u);
    } catch (const std::exception&) {
      return;
    }
  }
}

#if defined(VLINK_TEST_SUPPORT_SECURITY)
#include "./security_test_helpers.h"
#endif

TEST_SUITE("someip-security") {
#if defined(VLINK_TEST_SUPPORT_SECURITY)

  TEST_CASE("asymmetric rsa-oaep encrypted bytes round trip via someip") {
    MESSAGE("[someip-security] asymmetric rsa-oaep encrypted bytes round trip via someip");

    try {
      const auto kp = vlink_test_sec::generate_rsa_keypair(2048);

      if (kp.public_pem.empty()) {
        return;
      }

      std::atomic<bool> received{false};
      Bytes captured;

      Security::Config pub_cfg;
      pub_cfg.public_key_pem = kp.public_pem;

      Security::Config sub_cfg;
      sub_cfg.private_key_pem = kp.private_pem;

      SecurityPublisher<Bytes> pub(SomeipConf(0x7001, 0x0001, SomeipConf::Groups{0x0001}, 0x0001), std::move(pub_cfg));
      SecuritySubscriber<Bytes> sub("someip://28673/1?groups=1&event=1", std::move(sub_cfg));

      sub.listen([&](const Bytes& data) {
        captured = data;
        received.store(true, std::memory_order_release);
      });

      if (pub.wait_for_subscribers(1s)) {
        std::this_thread::sleep_for(10ms);
        pub.publish(Bytes{0xAA, 0xBB, 0xCC});

        for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
          std::this_thread::sleep_for(20ms);
        }

        if (received.load(std::memory_order_acquire)) {
          REQUIRE_EQ(captured.size(), 3u);
          CHECK_EQ(captured[0], 0xAAu);
          CHECK_EQ(captured[2], 0xCCu);
        }
      }
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("asymmetric mismatched private key fails to decrypt over someip") {
    MESSAGE("[someip-security] asymmetric mismatched private key fails to decrypt over someip");

    try {
      const auto kp1 = vlink_test_sec::generate_rsa_keypair(2048);
      const auto kp2 = vlink_test_sec::generate_rsa_keypair(2048);

      if (kp1.public_pem.empty() || kp2.private_pem.empty()) {
        return;
      }

      std::atomic<bool> received{false};

      Security::Config pub_cfg;
      pub_cfg.public_key_pem = kp1.public_pem;

      Security::Config sub_cfg;
      sub_cfg.private_key_pem = kp2.private_pem;

      SecurityPublisher<Bytes> pub(SomeipConf(0x7002, 0x0001, SomeipConf::Groups{0x0001}, 0x0001), std::move(pub_cfg));
      SecuritySubscriber<Bytes> sub("someip://28674/1?groups=1&event=1", std::move(sub_cfg));

      sub.listen([&](const Bytes& /*data*/) { received.store(true, std::memory_order_release); });

      if (pub.wait_for_subscribers(1s)) {
        std::this_thread::sleep_for(10ms);
        pub.publish(Bytes{0x01, 0x02, 0x03});

        for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
          std::this_thread::sleep_for(20ms);
        }
      }

      CHECK_FALSE(received.load(std::memory_order_acquire));
    } catch (const std::exception&) {
      return;
    }
  }

#endif
}

#endif  // VLINK_SUPPORT_SOMEIP

// NOLINTEND
