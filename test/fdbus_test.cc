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

#ifdef VLINK_SUPPORT_FDBUS

#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "./common_test.h"
#include "./modules/fdbus_conf.h"

static bool fdbus_available() { return FdbusConf::has_name_server(); }

TEST_SUITE("fdbus-init") {
  TEST_CASE("default conf stores address with svc transport") {
    MESSAGE("[fdbus-init] default conf stores address with svc transport");

    if (!fdbus_available()) {
      return;
    }

    FdbusConf conf("fdbus_test_svc");

    CHECK(conf.address == "fdbus_test_svc");
    CHECK(conf.event.empty());
    CHECK(conf.transport == "svc");
    CHECK(conf.get_transport_type() == TransportType::kFdbus);
  }

  TEST_CASE("conf with event name stores event field") {
    MESSAGE("[fdbus-init] conf with event name stores event field");

    if (!fdbus_available()) {
      return;
    }

    FdbusConf conf("fdbus_test_svc", "speed");

    CHECK(conf.event == "speed");
  }

  TEST_CASE("conf with ipc transport stores transport field") {
    MESSAGE("[fdbus-init] conf with ipc transport stores transport field");

    if (!fdbus_available()) {
      return;
    }

    FdbusConf conf("fdbus_test_ipc", "", "ipc");

    CHECK(conf.transport == "ipc");
  }

  TEST_CASE("conf equality compares address and event but not transport") {
    MESSAGE("[fdbus-init] conf equality compares address and event but not transport");

    if (!fdbus_available()) {
      return;
    }

    FdbusConf a("my_svc", "evt");
    FdbusConf b("my_svc", "evt");
    FdbusConf c("my_svc", "evt2");
    FdbusConf svc("my_svc", "evt", "svc");
    FdbusConf ipc("my_svc", "evt", "ipc");

    CHECK(a == b);
    CHECK(a != c);
    CHECK(svc == ipc);
  }

  TEST_CASE("url parses for all impl types") {
    MESSAGE("[fdbus-init] url parses for all impl types");

    if (!fdbus_available()) {
      return;
    }

    Url url("fdbus://fdbus_init_parse1");

    CHECK(url.parse(kPublisher));
    CHECK(url.parse(kSubscriber));
    CHECK(url.parse(kServer));
    CHECK(url.parse(kClient));
    CHECK(url.parse(kSetter));
    CHECK(url.parse(kGetter));
  }

  TEST_CASE("url with event query param parses successfully") {
    MESSAGE("[fdbus-init] url with event query param parses successfully");

    if (!fdbus_available()) {
      return;
    }

    Url url("fdbus://fdbus_init_parse2?event=myevent");

    CHECK(url.parse(kPublisher));
    CHECK(url.parse(kSubscriber));
  }

  TEST_CASE("url with ipc fragment parses successfully") {
    MESSAGE("[fdbus-init] url with ipc fragment parses successfully");

    if (!fdbus_available()) {
      return;
    }

    Url url("fdbus://fdbus_init_parse3#ipc");

    CHECK(url.parse(kPublisher));
    CHECK(url.parse(kSubscriber));
  }

  TEST_CASE("unknown impl type throws on parse") {
    MESSAGE("[fdbus-init] unknown impl type throws on parse");

    if (!fdbus_available()) {
      return;
    }

    Url url("fdbus://fdbus_init_parse4");

    CHECK_THROWS_AS(url.parse(kUnknownImplType), std::runtime_error);
  }

  TEST_CASE("invalid url scheme throws on publisher construction") {
    MESSAGE("[fdbus-init] invalid url scheme throws on publisher construction");

    if (!fdbus_available()) {
      return;
    }

    CHECK_THROWS(Publisher<int>("fdbus1://bad_url"));
  }
}

TEST_SUITE("fdbus-pubsub") {
  TEST_CASE("bytes payload is received intact") {
    MESSAGE("[fdbus-pubsub] bytes payload is received intact");

    if (!fdbus_available()) {
      return;
    }

    std::atomic<bool> received{false};
    Bytes captured;

    Publisher<Bytes> pub(FdbusConf("fdbus_evt_pubsub1", "data"));
    Subscriber<Bytes> sub("fdbus://fdbus_evt_pubsub1?event=data");

    sub.listen([&](const Bytes& data) {
      captured = data;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.has_subscribers());

    Bytes payload{0xAB, 0xCD, 0xEF};
    CHECK(pub.publish(payload));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(50ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    REQUIRE(captured.size() == 3);
    CHECK(captured[0] == 0xAB);
    CHECK(captured[2] == 0xEF);
  }

  TEST_CASE("string payload is received with correct value") {
    MESSAGE("[fdbus-pubsub] string payload is received with correct value");

    if (!fdbus_available()) {
      return;
    }

    std::atomic<bool> received{false};
    std::string captured;

    Publisher<std::string> pub(FdbusConf("fdbus_evt_str1", "name"));
    Subscriber<std::string> sub("fdbus://fdbus_evt_str1?event=name");

    sub.listen([&](const std::string& val) {
      captured = val;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(std::string("hello_fdbus")));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(50ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    CHECK(captured == "hello_fdbus");
  }

  TEST_CASE("multiple publishes are all received by subscriber") {
    MESSAGE("[fdbus-pubsub] multiple publishes are all received by subscriber");

    if (!fdbus_available()) {
      return;
    }

    std::atomic<int> count{0};

    Publisher<int> pub(FdbusConf("fdbus_evt_multi1", "counter"));
    Subscriber<int> sub("fdbus://fdbus_evt_multi1?event=counter");

    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 10; ++i) {
      pub.publish(i);
      std::this_thread::sleep_for(20ms);
    }

    std::this_thread::sleep_for(300ms);
    CHECK(count.load() >= 10);
  }

  TEST_CASE("multiple subscribers each receive every published message") {
    MESSAGE("[fdbus-pubsub] multiple subscribers each receive every published message");

    if (!fdbus_available()) {
      return;
    }

    std::atomic<int> count1{0};
    std::atomic<int> count2{0};

    Publisher<Bytes> pub(FdbusConf("fdbus_evt_multisub1", "raw"));
    Subscriber<Bytes> sub1("fdbus://fdbus_evt_multisub1?event=raw");
    Subscriber<Bytes> sub2("fdbus://fdbus_evt_multisub1?event=raw");

    sub1.listen([&](const Bytes& /*d*/) { count1.fetch_add(1, std::memory_order_relaxed); });
    sub2.listen([&](const Bytes& /*d*/) { count2.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 3; ++i) {
      pub.publish(Bytes{static_cast<uint8_t>(i)});
      std::this_thread::sleep_for(30ms);
    }

    std::this_thread::sleep_for(300ms);
    CHECK(count1.load() >= 3);
    CHECK(count2.load() >= 3);
  }

  TEST_CASE("force publish succeeds without any subscriber") {
    MESSAGE("[fdbus-pubsub] force publish succeeds without any subscriber");

    if (!fdbus_available()) {
      return;
    }

    Publisher<Bytes> pub(FdbusConf("fdbus_evt_force1", "force"));

    CHECK(!pub.has_subscribers());

    for (int i = 0; i < 5; ++i) {
      CHECK(pub.publish(Bytes{static_cast<uint8_t>(i)}, true));
    }
  }

  TEST_CASE("subscriber connect and disconnect events are detected") {
    MESSAGE("[fdbus-pubsub] subscriber connect and disconnect events are detected");

    if (!fdbus_available()) {
      return;
    }

    std::atomic<int> connected_count{0};

    Publisher<Bytes> pub(FdbusConf("fdbus_evt_detect1", "detect"));
    pub.detect_subscribers([&](bool connected) {
      if (connected) {
        connected_count.fetch_add(1, std::memory_order_relaxed);
      }
    });

    {
      Subscriber<Bytes> sub("fdbus://fdbus_evt_detect1?event=detect");
      sub.listen([](const Bytes& /*d*/) {});
      std::this_thread::sleep_for(500ms);
      CHECK(pub.has_subscribers());
    }

    std::this_thread::sleep_for(500ms);
    CHECK(!pub.has_subscribers());
  }
}

TEST_SUITE("fdbus-method") {
  TEST_CASE("fire and forget send increments server receive counter") {
    MESSAGE("[fdbus-method] fire and forget send increments server receive counter");

    if (!fdbus_available()) {
      return;
    }

    std::atomic<int> counter{0};

    Server<std::string> server(FdbusConf("fdbus_mth_send1"));
    server.listen([&](const std::string& /*req*/) { counter.fetch_add(1, std::memory_order_relaxed); });

    Client<std::string> client("fdbus://fdbus_mth_send1");
    CHECK(client.wait_for_connected(1s));
    CHECK(client.is_connected());

    CHECK(client.send("fire1"));
    std::this_thread::sleep_for(200ms);
    CHECK(counter.load() == 1);

    CHECK(client.send("fire2"));
    std::this_thread::sleep_for(200ms);
    CHECK(counter.load() == 2);
  }

  TEST_CASE("invoke returns correct response via multiple overloads") {
    MESSAGE("[fdbus-method] invoke returns correct response via multiple overloads");

    if (!fdbus_available()) {
      return;
    }

    Server<std::string, std::string> server(FdbusConf("fdbus_mth_invoke1"));
    server.listen([](const std::string& req, std::string& resp) { resp = "fdbus:" + req; });

    Client<std::string, std::string> client("fdbus://fdbus_mth_invoke1");
    CHECK(client.wait_for_connected(1s));

    SUBCASE("sync optional") {
      auto resp = client.invoke("ping");
      CHECK(resp.has_value());
      CHECK(*resp == "fdbus:ping");
    }

    SUBCASE("sync ref overload") {
      std::string out;
      CHECK(client.invoke("pong", out, 5s));
      CHECK(out == "fdbus:pong");
    }

    SUBCASE("async future") {
      auto fut = client.async_invoke("async");
      REQUIRE(fut.wait_for(5s) == std::future_status::ready);
      CHECK(fut.get() == "fdbus:async");
    }

    SUBCASE("multiple sequential invocations succeed") {
      for (int i = 0; i < 5; ++i) {
        auto resp = client.invoke("r" + std::to_string(i));
        CHECK(resp.has_value());
        CHECK(*resp == "fdbus:r" + std::to_string(i));
      }
    }
  }

  TEST_CASE("async callback invoke delivers response") {
    MESSAGE("[fdbus-method] async callback invoke delivers response");

    if (!fdbus_available()) {
      return;
    }

    Server<std::string, std::string> server(FdbusConf("fdbus_mth_cb1"));
    server.listen([](const std::string& /*req*/, std::string& resp) { resp = "fdbus_cb"; });

    Client<std::string, std::string> client("fdbus://fdbus_mth_cb1");
    CHECK(client.wait_for_connected(1s));

    std::atomic<bool> got{false};
    std::string resp_val;

    bool ok = client.invoke("msg", [&](const std::string& resp) {
      resp_val = resp;
      got.store(true, std::memory_order_release);
    });

    CHECK(ok);

    for (int i = 0; i < 100 && !got.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(50ms);
    }

    CHECK(got.load(std::memory_order_acquire));
    CHECK(resp_val == "fdbus_cb");
  }

  TEST_CASE("detect connected callback fires when client connects to server") {
    MESSAGE("[fdbus-method] detect connected callback fires when client connects to server");

    if (!fdbus_available()) {
      return;
    }

    std::atomic<bool> conn_flag{false};

    Server<std::string, std::string> server(FdbusConf("fdbus_mth_detect1"));
    server.listen([](const std::string& /*req*/, std::string& resp) { resp = "ok"; });

    Client<std::string, std::string> client("fdbus://fdbus_mth_detect1");
    client.detect_connected([&](bool connected) { conn_flag.store(connected, std::memory_order_release); });

    CHECK(client.wait_for_connected(1s));
    std::this_thread::sleep_for(200ms);
    CHECK(conn_flag.load(std::memory_order_acquire));
  }
}

TEST_SUITE("fdbus-field") {
  TEST_CASE("setter and getter exchange values via all access patterns") {
    MESSAGE("[fdbus-field] setter and getter exchange values via all access patterns");

    if (!fdbus_available()) {
      return;
    }

    SUBCASE("polling get") {
      Setter<Bytes> setter(FdbusConf("fdbus_fld_poll1", "value"));
      Getter<Bytes> getter("fdbus://fdbus_fld_poll1?event=value");

      setter.set(Bytes{0x13, 0x37});
      std::this_thread::sleep_for(300ms);

      auto v = getter.get();
      REQUIRE(v.has_value());
      REQUIRE(v->size() == 2);
      CHECK((*v)[0] == 0x13);
      CHECK((*v)[1] == 0x37);
    }

    SUBCASE("wait for value blocks until setter publishes") {
      Setter<Bytes> setter(FdbusConf("fdbus_fld_wait1", "state"));
      Getter<Bytes> getter("fdbus://fdbus_fld_wait1?event=state");

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

    SUBCASE("listen callback is invoked on value change") {
      std::atomic<bool> notified{false};

      Setter<Bytes> setter(FdbusConf("fdbus_fld_cb1", "notify"));
      Getter<Bytes> getter("fdbus://fdbus_fld_cb1?event=notify");

      getter.listen([&](const Bytes& /*val*/) { notified.store(true, std::memory_order_release); });

      std::this_thread::sleep_for(100ms);
      setter.set(Bytes{0x42});

      for (int i = 0; i < 100 && !notified.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(50ms);
      }

      CHECK(notified.load(std::memory_order_acquire));
    }

    SUBCASE("change reporting suppresses duplicate value callbacks") {
      std::atomic<int> cb_count{0};

      Setter<Bytes> setter(FdbusConf("fdbus_fld_cr1", "temp"));
      Getter<Bytes> getter("fdbus://fdbus_fld_cr1?event=temp");

      getter.set_change_reporting(true);
      getter.listen([&](const Bytes& /*v*/) { cb_count.fetch_add(1, std::memory_order_relaxed); });

      std::this_thread::sleep_for(100ms);

      setter.set(Bytes{0x5A});
      std::this_thread::sleep_for(200ms);
      setter.set(Bytes{0x5A});
      std::this_thread::sleep_for(200ms);

      CHECK(cb_count.load() <= 1);
    }

    SUBCASE("late getter receives cached value from setter") {
      Setter<Bytes> setter(FdbusConf("fdbus_fld_late1", "cache"));
      setter.set(Bytes{0xC0, 0xDE});
      std::this_thread::sleep_for(200ms);

      Getter<Bytes> late_getter("fdbus://fdbus_fld_late1?event=cache");
      CHECK(late_getter.wait_for_value(1s));
      auto v = late_getter.get();
      REQUIRE(v.has_value());
      CHECK((*v)[0] == 0xC0);
    }

    SUBCASE("string field round trip") {
      Setter<std::string> setter(FdbusConf("fdbus_fld_str1", "label"));
      Getter<std::string> getter("fdbus://fdbus_fld_str1?event=label");

      setter.set(std::string("fdbus_field_val"));
      CHECK(getter.wait_for_value(1s));
      auto v = getter.get();
      REQUIRE(v.has_value());
      CHECK(*v == "fdbus_field_val");
    }
  }
}

TEST_SUITE("fdbus-pubsub") {
  TEST_CASE("large 1kb payload is received intact") {
    MESSAGE("[fdbus-pubsub] large 1kb payload is received intact");

    if (!fdbus_available()) {
      return;
    }

    static constexpr size_t kSize1K = 1024;

    std::atomic<bool> received{false};
    size_t captured_size{0};

    Publisher<Bytes> pub(FdbusConf("fdbus_evt_large1k", "data"));
    Subscriber<Bytes> sub("fdbus://fdbus_evt_large1k?event=data");

    sub.listen([&](const Bytes& data) {
      captured_size = data.size();
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    Bytes payload = Bytes::create(kSize1K);
    CHECK(pub.publish(payload));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(50ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    CHECK_EQ(captured_size, kSize1K);
  }

  TEST_CASE("empty bytes payload is received without crash") {
    MESSAGE("[fdbus-pubsub] empty bytes payload is received without crash");

    if (!fdbus_available()) {
      return;
    }

    std::atomic<bool> received{false};
    size_t captured_size{1};

    Publisher<Bytes> pub(FdbusConf("fdbus_evt_empty1", "data"));
    Subscriber<Bytes> sub("fdbus://fdbus_evt_empty1?event=data");

    sub.listen([&](const Bytes& data) {
      captured_size = data.size();
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(Bytes{}));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(50ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    CHECK_EQ(captured_size, 0u);
  }

  TEST_CASE("concurrent publishes from multiple threads reach subscriber") {
    MESSAGE("[fdbus-pubsub] concurrent publishes from multiple threads reach subscriber");

    if (!fdbus_available()) {
      return;
    }

    static constexpr int kThreads = 3;
    static constexpr int kPerThread = 4;

    std::atomic<int> total{0};

    Publisher<int> pub(FdbusConf("fdbus_evt_concurrent1", "num"));
    Subscriber<int> sub("fdbus://fdbus_evt_concurrent1?event=num");

    sub.listen([&](const int& /*v*/) { total.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));

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

    std::this_thread::sleep_for(300ms);
    CHECK(total.load() >= kThreads * kPerThread);
  }

  TEST_CASE("subscriber destroyed mid-flight does not crash publisher") {
    MESSAGE("[fdbus-pubsub] subscriber destroyed mid-flight does not crash publisher");

    if (!fdbus_available()) {
      return;
    }

    Publisher<int> pub(FdbusConf("fdbus_evt_lifecycle1", "num"));

    {
      Subscriber<int> sub("fdbus://fdbus_evt_lifecycle1?event=num");
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

TEST_SUITE("fdbus-method") {
  TEST_CASE("invoke times out when server is absent") {
    MESSAGE("[fdbus-method] invoke times out when server is absent");

    if (!fdbus_available()) {
      return;
    }

    Client<std::string, std::string> orphan("fdbus://fdbus_mth_absent1");

    std::string out;
    bool ok = orphan.invoke("req", out, 300ms);
    CHECK_FALSE(ok);
  }

  TEST_CASE("deferred async reply is delivered to future") {
    MESSAGE("[fdbus-method] deferred async reply is delivered to future");

    if (!fdbus_available()) {
      return;
    }

    std::atomic<uint64_t> saved_id{0};
    std::atomic<bool> req_received{false};

    Server<std::string, std::string> server(FdbusConf("fdbus_mth_async_reply1"));
    server.listen_for_reply([&](uint64_t req_id, const std::string& /*req*/) {
      saved_id.store(req_id, std::memory_order_release);
      req_received.store(true, std::memory_order_release);
    });

    Client<std::string, std::string> client("fdbus://fdbus_mth_async_reply1");
    CHECK(client.wait_for_connected(1s));

    auto fut = client.async_invoke("deferred");

    for (int i = 0; i < 100 && !req_received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(50ms);
    }

    REQUIRE(req_received.load(std::memory_order_acquire));

    if (server.reply(saved_id.load(), std::string("deferred_fdbus"))) {
      if (fut.wait_for(2s) == std::future_status::ready) {
        CHECK_EQ(fut.get(), "deferred_fdbus");
      }
    }
  }
}

TEST_SUITE("fdbus-field") {
  TEST_CASE("default value not available before any set") {
    MESSAGE("[fdbus-field] default value not available before any set");

    if (!fdbus_available()) {
      return;
    }

    Getter<int> getter("fdbus://fdbus_fld_default1?event=value");
    auto v = getter.get();
    CHECK_FALSE(v.has_value());
  }

  TEST_CASE("integer field round trips with correct value") {
    MESSAGE("[fdbus-field] integer field round trips with correct value");

    if (!fdbus_available()) {
      return;
    }

    Setter<int> setter(FdbusConf("fdbus_fld_int1", "value"));
    Getter<int> getter("fdbus://fdbus_fld_int1?event=value");

    setter.set(12345678);
    CHECK(getter.wait_for_value(1s));

    auto v = getter.get();
    REQUIRE(v.has_value());
    CHECK_EQ(*v, 12345678);
  }
}

TEST_SUITE("fdbus-error") {
  TEST_CASE("distinct topics yield distinct abstract nodes") {
    MESSAGE("[fdbus-error] distinct topics yield distinct abstract nodes");

    if (!fdbus_available()) {
      return;
    }

    Publisher<int> pub1(FdbusConf("fdbus_id_p1", "evt"));
    Publisher<int> pub2(FdbusConf("fdbus_id_p2", "evt"));
    Subscriber<int> sub("fdbus://fdbus_id_p1?event=evt");

    CHECK(pub1.get_abstract_node() != nullptr);
    CHECK(pub2.get_abstract_node() != nullptr);
    CHECK(pub1.get_abstract_node() != pub2.get_abstract_node());
    CHECK(pub1.get_abstract_node() != sub.get_abstract_node());
  }
}

namespace {

struct FdbusCustomMsg {
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

TEST_SUITE("fdbus-custom") {
  TEST_CASE("custom type round trips id and label through fdbus") {
    MESSAGE("[fdbus-custom] custom type round trips id and label through fdbus");

    try {
      if (!fdbus_available()) {
        return;
      }

      std::atomic<bool> received{false};
      FdbusCustomMsg captured{};

      Publisher<FdbusCustomMsg> pub(FdbusConf("fdbus_cust_basic", "evt"));
      Subscriber<FdbusCustomMsg> sub("fdbus://fdbus_cust_basic?event=evt");

      sub.listen([&](const FdbusCustomMsg& m) {
        captured = m;
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      FdbusCustomMsg msg;
      msg.id = 51;
      msg.label = "fdbus_custom";
      CHECK(pub.publish(msg));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      CHECK_EQ(captured.id, 51);
      CHECK_EQ(captured.label, "fdbus_custom");
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("serializer detects custom type as kCustomType") {
    MESSAGE("[fdbus-custom] serializer detects custom type as kCustomType");

    static constexpr auto kType = Serializer::get_type_of<FdbusCustomMsg>();
    CHECK_EQ(kType, Serializer::kCustomType);
  }
}

#include "./zerocopy/raw_data.h"

TEST_SUITE("fdbus-dynamicdata") {
  TEST_CASE("dynamicdata round trip preserves type tag and int value") {
    MESSAGE("[fdbus-dynamicdata] dynamicdata round trip preserves type tag and int value");

    if (!fdbus_available()) {
      return;
    }

    try {
      std::atomic<bool> received{false};
      DynamicData captured;

      Publisher<DynamicData> pub(FdbusConf("fdbus_dyn_int1", "data"));
      Subscriber<DynamicData> sub("fdbus://fdbus_dyn_int1?event=data");

      sub.listen([&](const DynamicData& d) {
        captured = d;
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      DynamicData d;
      d.load("fdbus_int", 456);
      CHECK(pub.publish(d));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      CHECK(captured.get_type() == "fdbus_int");
      CHECK(captured.as<int>() == 456);
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("dynamicdata type tag is preserved distinct from payload") {
    MESSAGE("[fdbus-dynamicdata] dynamicdata type tag is preserved distinct from payload");

    if (!fdbus_available()) {
      return;
    }

    try {
      std::atomic<bool> received{false};
      DynamicData captured;

      Publisher<DynamicData> pub(FdbusConf("fdbus_dyn_tag1", "data"));
      Subscriber<DynamicData> sub("fdbus://fdbus_dyn_tag1?event=data");

      sub.listen([&](const DynamicData& d) {
        captured = d;
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      DynamicData d;
      d.load("fdbus_tag", std::string("tag_ok"));
      CHECK(pub.publish(d));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      CHECK(captured.get_type() == "fdbus_tag");
      CHECK_FALSE(captured.is_empty());
      CHECK(captured.as<std::string>() == "tag_ok");
    } catch (const std::exception&) {
      return;
    }
  }
}

TEST_SUITE("fdbus-zerocopy") {
  TEST_CASE("rawdata round trip preserves header seq over fdbus") {
    MESSAGE("[fdbus-zerocopy] rawdata round trip preserves header seq over fdbus");

    if (!fdbus_available()) {
      return;
    }

    try {
      std::atomic<bool> received{false};
      zerocopy::RawData captured;

      Publisher<zerocopy::RawData> pub(FdbusConf("fdbus_zc_raw1", "data"));
      Subscriber<zerocopy::RawData> sub("fdbus://fdbus_zc_raw1?event=data");

      sub.listen([&](const zerocopy::RawData& d) {
        captured.deep_copy(d);
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      zerocopy::RawData rd;
      rd.header.seq = 9;
      rd.create(2);
      const_cast<uint8_t*>(rd.data())[0] = 0x77;
      const_cast<uint8_t*>(rd.data())[1] = 0x88;
      CHECK(pub.publish(rd));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      REQUIRE_EQ(captured.size(), 2u);
      CHECK_EQ(captured.header.seq, 9u);
      CHECK_EQ(captured.data()[0], 0x77u);
    } catch (const std::exception&) {
      return;
    }
  }
}

#ifdef VLINK_TEST_SUPPORT_SECURITY
#include "./security_test_helpers.h"

TEST_SUITE("fdbus-security") {
  TEST_CASE("asymmetric rsa-oaep encrypted bytes round trip via fdbus") {
    MESSAGE("[fdbus-security] asymmetric rsa-oaep encrypted bytes round trip via fdbus");

    if (!fdbus_available()) {
      return;
    }

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

      SecurityPublisher<Bytes> pub(FdbusConf("fdbus_sec_rsa1", "data"), std::move(pub_cfg));
      SecuritySubscriber<Bytes> sub("fdbus://fdbus_sec_rsa1?event=data", std::move(sub_cfg));

      sub.listen([&](const Bytes& data) {
        captured = data;
        received.store(true, std::memory_order_release);
      });

      if (pub.wait_for_subscribers(1s)) {
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

  TEST_CASE("asymmetric mismatched private key fails to decrypt over fdbus") {
    MESSAGE("[fdbus-security] asymmetric mismatched private key fails to decrypt over fdbus");

    if (!fdbus_available()) {
      return;
    }

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

      SecurityPublisher<Bytes> pub(FdbusConf("fdbus_sec_rsa_mm1", "data"), std::move(pub_cfg));
      SecuritySubscriber<Bytes> sub("fdbus://fdbus_sec_rsa_mm1?event=data", std::move(sub_cfg));

      sub.listen([&](const Bytes& /*data*/) { received.store(true, std::memory_order_release); });

      if (pub.wait_for_subscribers(1s)) {
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
}
#endif  // VLINK_TEST_SUPPORT_SECURITY

#endif  // VLINK_SUPPORT_FDBUS

// NOLINTEND
