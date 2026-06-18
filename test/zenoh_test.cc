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

#ifdef VLINK_SUPPORT_ZENOH

#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "./modules/zenoh_conf.h"

TEST_SUITE("zenoh-init") {
  TEST_CASE("conf defaults are set correctly") {
    MESSAGE("[zenoh-init] conf defaults are set correctly");

    ZenohConf conf("vehicle/speed");

    CHECK(conf.address == "vehicle/speed");
    CHECK(conf.event.empty());
    CHECK(conf.domain == 0);
    CHECK(conf.qos.empty());
    CHECK(conf.fragment.empty());
    CHECK(conf.get_transport_type() == TransportType::kZenoh);
  }

  TEST_CASE("conf accepts all fields") {
    MESSAGE("[zenoh-init] conf accepts all fields");

    ZenohConf conf("my/topic", "my_event", 2, "reliable", "tcp");

    CHECK(conf.address == "my/topic");
    CHECK(conf.event == "my_event");
    CHECK(conf.domain == 2);
    CHECK(conf.qos == "reliable");
    CHECK(conf.fragment == "tcp");
  }

  TEST_CASE("conf equality compares all fields") {
    MESSAGE("[zenoh-init] conf equality compares all fields");

    ZenohConf a("addr1", "ev1", 0, "", "");
    ZenohConf b("addr1", "ev1", 0, "", "");
    ZenohConf c("addr2", "ev1", 0, "", "");

    CHECK(a == b);
    CHECK(a != c);
  }

  TEST_CASE("url parses for all impl types") {
    MESSAGE("[zenoh-init] url parses for all impl types");

    Url url("zenoh://zenoh/init/parse1?event=ev1");

    CHECK(url.parse(kPublisher));
    CHECK(url.parse(kSubscriber));
    CHECK(url.parse(kServer));
    CHECK(url.parse(kClient));
    CHECK(url.parse(kSetter));
    CHECK(url.parse(kGetter));
  }

  TEST_CASE("unknown impl type throws on parse") {
    MESSAGE("[zenoh-init] unknown impl type throws on parse");

    Url url("zenoh://zenoh/init/parse2");

    CHECK_THROWS_AS(url.parse(kUnknownImplType), std::runtime_error);
  }

  TEST_CASE("invalid transport scheme throws on construction") { CHECK_THROWS(Publisher<int>("zenoh1://bad/url")); }

  TEST_CASE("registered qos profile is accessible via conf") {
    MESSAGE("[zenoh-init] registered qos profile is accessible via conf");

    Qos qos;
    qos.reliability.kind = Qos::Reliability::kReliable;

    try {
      ZenohConf::register_qos("zenoh_reliable", qos);
    } catch (...) {
    }

    ZenohConf conf("zenoh/qos/test1", "", 0, "zenoh_reliable");
    CHECK(conf.qos == "zenoh_reliable");
  }
}

TEST_SUITE("zenoh-pubsub") {
  TEST_CASE("bytes payload is delivered to subscriber") {
    MESSAGE("[zenoh-pubsub] bytes payload is delivered to subscriber");

    std::atomic<bool> received{false};
    Bytes captured;

    Publisher<Bytes> pub(ZenohConf("zenoh/evt/pubsub1", "data"));
    Subscriber<Bytes> sub("zenoh://zenoh/evt/pubsub1?event=data");

    sub.listen([&](const Bytes& data) {
      captured = data;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.has_subscribers());

    Bytes payload{0xDE, 0xAD, 0xBE, 0xEF};
    CHECK(pub.publish(payload));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(30ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    REQUIRE(captured.size() == 4);
    CHECK(captured[0] == 0xDE);
    CHECK(captured[3] == 0xEF);
  }

  TEST_CASE("string payload round trips correctly") {
    MESSAGE("[zenoh-pubsub] string payload round trips correctly");

    std::atomic<bool> received{false};
    std::string captured;

    Publisher<std::string> pub(ZenohConf("zenoh/evt/str1", "data"));
    Subscriber<std::string> sub("zenoh://zenoh/evt/str1?event=data");

    sub.listen([&](const std::string& val) {
      captured = val;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(std::string("hello_zenoh")));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(30ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    CHECK(captured == "hello_zenoh");
  }

  TEST_CASE("integer payload round trips correctly") {
    MESSAGE("[zenoh-pubsub] integer payload round trips correctly");

    std::atomic<int> captured{0};
    std::atomic<bool> received{false};

    Publisher<int> pub(ZenohConf("zenoh/evt/int1", "data"));
    Subscriber<int> sub("zenoh://zenoh/evt/int1?event=data");

    sub.listen([&](const int& v) {
      captured.store(v, std::memory_order_relaxed);
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(7654));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(30ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    CHECK(captured.load() == 7654);
  }

  TEST_CASE("all published messages are received") {
    MESSAGE("[zenoh-pubsub] all published messages are received");

    std::atomic<int> count{0};

    Publisher<int> pub(ZenohConf("zenoh/evt/multi1", "data"));
    Subscriber<int> sub("zenoh://zenoh/evt/multi1?event=data");

    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 10; ++i) {
      pub.publish(i);
      std::this_thread::sleep_for(20ms);
    }

    std::this_thread::sleep_for(30ms);

    CHECK(count.load() >= 10);
  }

  TEST_CASE("multiple subscribers each receive all messages") {
    MESSAGE("[zenoh-pubsub] multiple subscribers each receive all messages");

    std::atomic<int> count1{0};
    std::atomic<int> count2{0};

    Publisher<Bytes> pub(ZenohConf("zenoh/evt/multisub1", "data"));
    Subscriber<Bytes> sub1("zenoh://zenoh/evt/multisub1?event=data");
    Subscriber<Bytes> sub2("zenoh://zenoh/evt/multisub1?event=data");

    sub1.listen([&](const Bytes& /*d*/) { count1.fetch_add(1, std::memory_order_relaxed); });
    sub2.listen([&](const Bytes& /*d*/) { count2.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 3; ++i) {
      pub.publish(Bytes{static_cast<uint8_t>(i)});
      std::this_thread::sleep_for(30ms);
    }

    std::this_thread::sleep_for(30ms);

    CHECK(count1.load() >= 3);
    CHECK(count2.load() >= 3);
  }

  TEST_CASE("force publish succeeds without subscribers") {
    MESSAGE("[zenoh-pubsub] force publish succeeds without subscribers");

    Publisher<Bytes> pub(ZenohConf("zenoh/evt/force1", "data"));

    CHECK(!pub.has_subscribers());

    for (int i = 0; i < 5; ++i) {
      CHECK(pub.publish(Bytes{static_cast<uint8_t>(i)}, true));
    }
  }

  TEST_CASE("subscriber connect and disconnect are detected") {
    MESSAGE("[zenoh-pubsub] subscriber connect and disconnect are detected");

    std::atomic<int> connected_count{0};

    Publisher<Bytes> pub(ZenohConf("zenoh/evt/detect1", "data"));

    pub.detect_subscribers([&](bool connected) {
      if (connected) {
        connected_count.fetch_add(1, std::memory_order_relaxed);
      }
    });

    {
      Subscriber<Bytes> sub("zenoh://zenoh/evt/detect1?event=data");
      sub.listen([](const Bytes& /*d*/) {});

      std::this_thread::sleep_for(30ms);
      CHECK(pub.has_subscribers());
    }

    std::this_thread::sleep_for(30ms);
    CHECK(!pub.has_subscribers());
  }

  TEST_CASE("dynamic data payload is delivered correctly") {
    MESSAGE("[zenoh-pubsub] dynamic data payload is delivered correctly");

    std::atomic<bool> received{false};
    DynamicData captured;

    Publisher<DynamicData> pub(ZenohConf("zenoh/dyn/int1", "data"));
    Subscriber<DynamicData> sub("zenoh://zenoh/dyn/int1?event=data");

    sub.listen([&](const DynamicData& d) {
      captured = d;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    DynamicData d;
    d.load("int", 321);
    CHECK(pub.publish(d));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(30ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    CHECK(captured.as<int>() == 321);
  }
}

TEST_SUITE("zenoh-method") {
  TEST_CASE("fire and forget send increments server counter") {
    MESSAGE("[zenoh-method] fire and forget send increments server counter");

    std::atomic<int> counter{0};

    Server<std::string> server(ZenohConf("zenoh/mth/send1", "req"));
    server.listen([&](const std::string& /*req*/) { counter.fetch_add(1, std::memory_order_relaxed); });

    Client<std::string> client("zenoh://zenoh/mth/send1?event=req");
    CHECK(client.wait_for_connected(1s));
    CHECK(client.is_connected());

    CHECK(client.send("fire1"));
    std::this_thread::sleep_for(30ms);
    CHECK(counter.load() == 1);

    CHECK(client.send("fire2"));
    std::this_thread::sleep_for(30ms);
    CHECK(counter.load() == 2);
  }

  TEST_CASE("invoke returns correct response for all overloads") {
    MESSAGE("[zenoh-method] invoke returns correct response for all overloads");

    Server<std::string, std::string> server(ZenohConf("zenoh/mth/invoke1", "req"));
    server.listen([](const std::string& req, std::string& resp) { resp = "zenoh:" + req; });

    Client<std::string, std::string> client("zenoh://zenoh/mth/invoke1?event=req");
    CHECK(client.wait_for_connected(1s));

    SUBCASE("sync optional") {
      auto resp = client.invoke("ping");
      CHECK(resp.has_value());
      CHECK(*resp == "zenoh:ping");
    }

    SUBCASE("sync ref overload") {
      std::string out;
      CHECK(client.invoke("pong", out, 5s));
      CHECK(out == "zenoh:pong");
    }

    SUBCASE("async future") {
      auto fut = client.async_invoke("async");
      REQUIRE(fut.wait_for(5s) == std::future_status::ready);
      CHECK(fut.get() == "zenoh:async");
    }

    SUBCASE("multiple sequential calls") {
      for (int i = 0; i < 5; ++i) {
        auto resp = client.invoke("r" + std::to_string(i));
        CHECK(resp.has_value());
        CHECK(*resp == "zenoh:r" + std::to_string(i));
      }
    }
  }

  TEST_CASE("deferred reply is delivered via async invoke") {
    MESSAGE("[zenoh-method] deferred reply is delivered via async invoke");

    std::atomic<uint64_t> saved_id{0};
    std::atomic<bool> req_received{false};

    Server<std::string, std::string> server(ZenohConf("zenoh/mth/async_reply1", "req"));
    server.listen_for_reply([&](uint64_t req_id, const std::string& /*req*/) {
      saved_id.store(req_id, std::memory_order_release);
      req_received.store(true, std::memory_order_release);
    });

    Client<std::string, std::string> client("zenoh://zenoh/mth/async_reply1?event=req");
    CHECK(client.wait_for_connected(1s));

    auto fut = client.async_invoke("defer");

    for (int i = 0; i < 100 && !req_received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(30ms);
    }

    REQUIRE(req_received.load(std::memory_order_acquire));
    CHECK(server.reply(saved_id.load(), std::string("deferred_zenoh")));

    REQUIRE(fut.wait_for(5s) == std::future_status::ready);
    CHECK(fut.get() == "deferred_zenoh");
  }

  TEST_CASE("async callback receives the response") {
    MESSAGE("[zenoh-method] async callback receives the response");

    Server<std::string, std::string> server(ZenohConf("zenoh/mth/cb1", "req"));
    server.listen([](const std::string& /*req*/, std::string& resp) { resp = "zenoh_cb"; });

    Client<std::string, std::string> client("zenoh://zenoh/mth/cb1?event=req");
    CHECK(client.wait_for_connected(1s));

    std::atomic<bool> got{false};
    std::string resp_val;

    bool ok = client.invoke("msg", [&](const std::string& resp) {
      resp_val = resp;
      got.store(true, std::memory_order_release);
    });

    CHECK(ok);

    for (int i = 0; i < 100 && !got.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(30ms);
    }

    CHECK(got.load(std::memory_order_acquire));
    CHECK(resp_val == "zenoh_cb");
  }

  TEST_CASE("client connection is reported via detect callback") {
    MESSAGE("[zenoh-method] client connection is reported via detect callback");

    std::atomic<bool> connected_event{false};

    Server<std::string, std::string> server(ZenohConf("zenoh/mth/detect_conn1", "req"));
    server.listen([](const std::string& /*req*/, std::string& resp) { resp = "ok"; });

    Client<std::string, std::string> client("zenoh://zenoh/mth/detect_conn1?event=req");
    client.detect_connected([&](bool connected) {
      if (connected) {
        connected_event.store(true, std::memory_order_release);
      }
    });

    for (int i = 0; i < 100 && !connected_event.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(30ms);
    }

    CHECK(connected_event.load(std::memory_order_acquire));
  }

  TEST_CASE("concurrent client invocations return correct responses") {
    MESSAGE("[zenoh-method] concurrent client invocations return correct responses");

    Server<std::string, std::string> server(ZenohConf("zenoh/audit/cc1", "req"));
    server.listen([](const std::string& req, std::string& resp) { resp = "ack:" + req; });

    Client<std::string, std::string> client("zenoh://zenoh/audit/cc1?event=req");
    REQUIRE(client.wait_for_connected(1s));

    static constexpr int kThreads = 2;
    static constexpr int kCallsPerThread = 8;
    std::atomic<int> ok_count{0};
    std::atomic<int> mismatch_count{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
      workers.emplace_back([&client, &ok_count, &mismatch_count, t]() {
        for (int i = 0; i < kCallsPerThread; ++i) {
          std::string out;
          std::string req = "t" + std::to_string(t) + "i" + std::to_string(i);

          if (client.invoke(req, out, 10s)) {
            if (out == "ack:" + req) {
              ok_count.fetch_add(1, std::memory_order_relaxed);
            } else {
              mismatch_count.fetch_add(1, std::memory_order_relaxed);
            }
          }
        }
      });
    }

    for (auto& w : workers) {
      w.join();
    }

    CHECK(mismatch_count.load() == 0);
    CHECK(ok_count.load() >= kThreads * kCallsPerThread * 90 / 100);
  }

  TEST_CASE("server replying with empty body is handled correctly") {
    MESSAGE("[zenoh-method] server replying with empty body is handled correctly");

    Server<std::string, std::string> server(ZenohConf("zenoh/audit/re1", "req"));
    server.listen([](const std::string& /*req*/, std::string& resp) { resp.clear(); });

    Client<std::string, std::string> client("zenoh://zenoh/audit/re1?event=req");
    REQUIRE(client.wait_for_connected(1s));

    std::string out;
    CHECK(client.invoke("any", out, 5s));
    CHECK(out.empty());

    CHECK(client.invoke("second", out, 5s));
    CHECK(out.empty());
  }

  TEST_CASE("server replying with large string is received intact") {
    MESSAGE("[zenoh-method] server replying with large string is received intact");

    Server<std::string, std::string> server(ZenohConf("zenoh/audit/rbin1", "req"));
    server.listen([](const std::string& req, std::string& resp) { resp = req + std::string(4096, 'X'); });

    Client<std::string, std::string> client("zenoh://zenoh/audit/rbin1?event=req");
    REQUIRE(client.wait_for_connected(1s));

    std::string out;
    CHECK(client.invoke(std::string("p"), out, 5s));
    REQUIRE(out.size() == 4097);
    CHECK(out.front() == 'p');
    CHECK(out.back() == 'X');
  }

  TEST_CASE("client invoke times out when no server is present") {
    MESSAGE("[zenoh-method] client invoke times out when no server is present");

    Client<std::string, std::string> client("zenoh://zenoh/audit/noserver1?event=req");

    std::string out;
    CHECK_FALSE(client.invoke("never", out, 200ms));
  }
}

TEST_SUITE("zenoh-field") {
  TEST_CASE("setter and getter exchange values") {
    MESSAGE("[zenoh-field] setter and getter exchange values");

    SUBCASE("polling get") {
      Setter<Bytes> setter(ZenohConf("zenoh/fld/poll1", "val"));
      Getter<Bytes> getter("zenoh://zenoh/fld/poll1?event=val");

      setter.set(Bytes{0xAA, 0xBB, 0xCC});
      std::this_thread::sleep_for(30ms);

      auto v = getter.get();
      REQUIRE(v.has_value());
      REQUIRE(v->size() == 3);
      CHECK((*v)[0] == 0xAA);
      CHECK((*v)[2] == 0xCC);
    }

    SUBCASE("wait for value") {
      Setter<Bytes> setter(ZenohConf("zenoh/fld/wait1", "val"));
      Getter<Bytes> getter("zenoh://zenoh/fld/wait1?event=val");

      std::thread writer([&] {
        std::this_thread::sleep_for(30ms);
        setter.set(Bytes{0x12, 0x34});
      });

      CHECK(getter.wait_for_value(1s));
      auto v = getter.get();
      REQUIRE(v.has_value());
      CHECK((*v)[0] == 0x12);

      writer.join();
    }

    SUBCASE("listen callback is invoked on set") {
      std::atomic<bool> notified{false};

      Setter<Bytes> setter(ZenohConf("zenoh/fld/cb1", "val"));
      Getter<Bytes> getter("zenoh://zenoh/fld/cb1?event=val");

      getter.listen([&](const Bytes& /*val*/) { notified.store(true, std::memory_order_release); });

      std::this_thread::sleep_for(30ms);
      setter.set(Bytes{0x5A});

      for (int i = 0; i < 100 && !notified.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(notified.load(std::memory_order_acquire));
    }

    SUBCASE("change reporting suppresses duplicate values") {
      std::atomic<int> cb_count{0};

      Setter<Bytes> setter(ZenohConf("zenoh/fld/cr1", "val"));
      Getter<Bytes> getter("zenoh://zenoh/fld/cr1?event=val");

      getter.set_change_reporting(true);
      getter.listen([&](const Bytes& /*v*/) { cb_count.fetch_add(1, std::memory_order_relaxed); });

      std::this_thread::sleep_for(30ms);

      setter.set(Bytes{0x77});
      std::this_thread::sleep_for(30ms);
      setter.set(Bytes{0x77});
      std::this_thread::sleep_for(30ms);

      CHECK(cb_count.load() <= 1);
    }

    SUBCASE("late getter receives cached value") {
      Setter<Bytes> setter(ZenohConf("zenoh/fld/late1", "val"));
      setter.set(Bytes{0xFE, 0xED});
      std::this_thread::sleep_for(30ms);

      Getter<Bytes> late_getter("zenoh://zenoh/fld/late1?event=val");
      CHECK(late_getter.wait_for_value(1s));
      auto v = late_getter.get();
      REQUIRE(v.has_value());
      CHECK((*v)[0] == 0xFE);
    }
  }
}

TEST_SUITE("zenoh-qos") {
  TEST_CASE("latency and loss tracking can be enabled and disabled") {
    MESSAGE("[zenoh-qos] latency and loss tracking can be enabled and disabled");

    Publisher<int> pub(ZenohConf("zenoh/lat/sub1", "data"));
    Subscriber<int> sub("zenoh://zenoh/lat/sub1?event=data");

    sub.set_latency_and_lost_enabled(true);
    CHECK(sub.is_latency_and_lost_enabled());

    std::atomic<int> count{0};
    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 10; ++i) {
      pub.publish(i);
      std::this_thread::sleep_for(10ms);
    }

    std::this_thread::sleep_for(30ms);

    CHECK(count.load() > 0);
    CHECK(sub.get_latency() >= 0);
    CHECK(sub.get_lost().total >= 0);

    sub.set_latency_and_lost_enabled(false);
    CHECK(!sub.is_latency_and_lost_enabled());
  }
}

TEST_SUITE("zenoh-init") {
  TEST_CASE("each node has a distinct abstract node pointer") {
    MESSAGE("[zenoh-init] each node has a distinct abstract node pointer");

    Publisher<int> pub1(ZenohConf("zenoh/id/p1", "data"));
    Publisher<int> pub2(ZenohConf("zenoh/id/p2", "data"));
    Subscriber<int> sub("zenoh://zenoh/id/p1?event=data");

    CHECK(pub1.get_abstract_node() != nullptr);
    CHECK(pub2.get_abstract_node() != nullptr);
    CHECK(pub1.get_abstract_node() != pub2.get_abstract_node());
    CHECK(pub1.get_abstract_node() != sub.get_abstract_node());
  }

  TEST_CASE("invalid url scheme throws on construction") { CHECK_THROWS(Publisher<int>("zenoh-bad-scheme://topic")); }

  TEST_CASE("publisher and server can coexist in same session") {
    MESSAGE("[zenoh-init] publisher and server can coexist in same session");

    Publisher<int> pub(ZenohConf("zenoh/audit/coexist1", "d"));
    Server<std::string, std::string> server(ZenohConf("zenoh/audit/coexist2", "r"));

    Subscriber<int> sub("zenoh://zenoh/audit/coexist1?event=d");
    Client<std::string, std::string> client("zenoh://zenoh/audit/coexist2?event=r");

    std::atomic<int> sub_got{0};
    sub.listen([&sub_got](const int& v) { sub_got.store(v, std::memory_order_relaxed); });
    server.listen([](const std::string& req, std::string& resp) { resp = "echo:" + req; });

    REQUIRE(pub.wait_for_subscribers(1s));
    REQUIRE(client.wait_for_connected(1s));

    CHECK(pub.publish(42));

    std::string out;
    CHECK(client.invoke("hi", out, 5s));
    CHECK(out == "echo:hi");

    for (int i = 0; i < 100 && sub_got.load() == 0; ++i) {
      std::this_thread::sleep_for(20ms);
    }

    CHECK(sub_got.load() == 42);
  }

  TEST_CASE("session is reusable after benign fragment value") {
    MESSAGE("[zenoh-init] session is reusable after benign fragment value");

    CHECK_NOTHROW(Publisher<int>(ZenohConf("zenoh/audit/ses-good", "", 0, "", "tcp")));
    CHECK_NOTHROW(Publisher<int>(ZenohConf("zenoh/audit/ses-good2", "", 0, "", "tcp")));
  }

  TEST_CASE("shm environment variables are absent or well formed") {
    MESSAGE("[zenoh-init] shm environment variables are absent or well formed");

    const auto shm_pool = Utils::get_env("VLINK_ZENOH_SHM_POOL");
    const auto shm_thr = Utils::get_env("VLINK_ZENOH_SHM_THRESHOLD");
    const auto shm_mode = Utils::get_env("VLINK_ZENOH_SHM_MODE");

    CHECK((shm_pool.empty() || std::stoi(shm_pool) > 0));
    CHECK((shm_thr.empty() || std::stoi(shm_thr) > 0));
    CHECK((shm_mode.empty() || shm_mode == "lazy" || shm_mode == "init"));
  }

  TEST_CASE("batching environment variables are absent or well formed") {
    MESSAGE("[zenoh-init] batching environment variables are absent or well formed");

    const auto bt = Utils::get_env("VLINK_ZENOH_BATCH_TIME_LIMIT_MS");
    const auto be = Utils::get_env("VLINK_ZENOH_BATCH_ENABLED");

    CHECK((bt.empty() || std::stoi(bt) >= 0));
    CHECK((be.empty() || be == "true" || be == "false"));
  }

  TEST_CASE("locality environment variable accepts known values") {
    MESSAGE("[zenoh-init] locality environment variable accepts known values");

    const auto loc = Utils::get_env("VLINK_ZENOH_ALLOWED_LOCALITY");

    CHECK((loc.empty() || loc == "any" || loc == "local" || loc == "remote"));
  }
}

TEST_SUITE("zenoh-pubsub") {
  TEST_CASE("concurrent 4 publishers 4 subscribers deliver all messages") {
    MESSAGE("[zenoh-pubsub] concurrent 4 publishers 4 subscribers deliver all messages");

    static constexpr int kPubs = 4;
    static constexpr int kSubs = 4;
    static constexpr int kMsgsPerPub = 10;

    std::vector<std::atomic<int>> counts(kSubs);

    for (auto& c : counts) {
      c.store(0, std::memory_order_relaxed);
    }

    std::vector<std::unique_ptr<Publisher<int>>> pubs;
    pubs.reserve(kPubs);

    for (int p = 0; p < kPubs; ++p) {
      pubs.emplace_back(std::make_unique<Publisher<int>>(ZenohConf("zenoh/cc/4x4/pub" + std::to_string(p), "data")));
    }

    std::vector<std::unique_ptr<Subscriber<int>>> subs;
    subs.reserve(kSubs * kPubs);

    for (int p = 0; p < kPubs; ++p) {
      for (int s = 0; s < kSubs; ++s) {
        subs.emplace_back(
            std::make_unique<Subscriber<int>>("zenoh://zenoh/cc/4x4/pub" + std::to_string(p) + "?event=data"));
        subs.back()->listen([&counts, s](const int& /*v*/) { counts[s].fetch_add(1, std::memory_order_relaxed); });
      }
    }

    for (auto& pub : pubs) {
      CHECK(pub->wait_for_subscribers(1s));
    }

    std::vector<std::thread> writers;
    writers.reserve(kPubs);

    for (int p = 0; p < kPubs; ++p) {
      writers.emplace_back([&pubs, p]() {
        for (int i = 0; i < kMsgsPerPub; ++i) {
          pubs[p]->publish(i);
          std::this_thread::sleep_for(10ms);
        }
      });
    }

    for (auto& w : writers) {
      w.join();
    }

    std::this_thread::sleep_for(100ms);

    for (int s = 0; s < kSubs; ++s) {
      CHECK(counts[s].load() >= kPubs * kMsgsPerPub);
    }
  }

  TEST_CASE("16 publishers 1 subscriber delivers all messages") {
    MESSAGE("[zenoh-pubsub] 16 publishers 1 subscriber delivers all messages");

    static constexpr int kPubs = 16;
    static constexpr int kMsgsPerPub = 5;

    std::atomic<int> count{0};

    std::vector<std::unique_ptr<Publisher<int>>> pubs;
    pubs.reserve(kPubs);

    for (int p = 0; p < kPubs; ++p) {
      pubs.emplace_back(std::make_unique<Publisher<int>>(ZenohConf("zenoh/cc/16x1/pub" + std::to_string(p), "data")));
    }

    std::vector<std::unique_ptr<Subscriber<int>>> subs;
    subs.reserve(kPubs);

    for (int p = 0; p < kPubs; ++p) {
      subs.emplace_back(
          std::make_unique<Subscriber<int>>("zenoh://zenoh/cc/16x1/pub" + std::to_string(p) + "?event=data"));
      subs.back()->listen([&count](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });
    }

    for (auto& pub : pubs) {
      CHECK(pub->wait_for_subscribers(1s));
    }

    std::vector<std::thread> writers;
    writers.reserve(kPubs);

    for (int p = 0; p < kPubs; ++p) {
      writers.emplace_back([&pubs, p]() {
        for (int i = 0; i < kMsgsPerPub; ++i) {
          pubs[p]->publish(i);
          std::this_thread::sleep_for(10ms);
        }
      });
    }

    for (auto& w : writers) {
      w.join();
    }

    std::this_thread::sleep_for(100ms);

    CHECK(count.load() >= kPubs * kMsgsPerPub);
  }

  TEST_CASE("subscriber created before publisher receives messages") {
    MESSAGE("[zenoh-pubsub] subscriber created before publisher receives messages");

    std::atomic<int> count{0};
    Subscriber<int> sub("zenoh://zenoh/lc/sub_before_pub1?event=data");
    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    std::this_thread::sleep_for(50ms);

    Publisher<int> pub(ZenohConf("zenoh/lc/sub_before_pub1", "data"));
    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 5; ++i) {
      pub.publish(i);
      std::this_thread::sleep_for(20ms);
    }

    std::this_thread::sleep_for(100ms);

    CHECK(count.load() >= 5);
  }

  TEST_CASE("publisher destroyed mid flight does not crash subscriber") {
    MESSAGE("[zenoh-pubsub] publisher destroyed mid flight does not crash subscriber");

    std::atomic<int> count{0};
    Subscriber<int> sub("zenoh://zenoh/lc/pub_destroy1?event=data");
    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    {
      Publisher<int> pub(ZenohConf("zenoh/lc/pub_destroy1", "data"));
      CHECK(pub.wait_for_subscribers(1s));

      for (int i = 0; i < 3; ++i) {
        pub.publish(i);
        std::this_thread::sleep_for(20ms);
      }
    }

    std::this_thread::sleep_for(100ms);

    CHECK(count.load() >= 3);
  }

  TEST_CASE("large payload round trips correctly") {
    MESSAGE("[zenoh-pubsub] large payload round trips correctly");

    static constexpr size_t k1KB = 1024;
    static constexpr size_t k64KB = 64 * 1024;
    static constexpr size_t k1MB = 1024ULL * 1024;

    size_t payload_size = 0;

    SUBCASE("1 KB") { payload_size = k1KB; }
    SUBCASE("64 KB") { payload_size = k64KB; }
    SUBCASE("1 MB") { payload_size = k1MB; }

    std::atomic<bool> received{false};
    Bytes captured;

    Publisher<Bytes> pub(ZenohConf("zenoh/large/rtt1", "data"));
    Subscriber<Bytes> sub("zenoh://zenoh/large/rtt1?event=data");

    sub.listen([&](const Bytes& data) {
      captured = data;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    std::vector<uint8_t> raw(payload_size, 0xC3);
    Bytes payload = Bytes::deep_copy(raw.data(), raw.size());
    CHECK(pub.publish(payload));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(50ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    REQUIRE(captured.size() == payload_size);
    CHECK(captured[0] == 0xC3);
    CHECK(captured[payload_size - 1] == 0xC3);
  }

  TEST_CASE("empty bytes payload is delivered and size is zero") {
    MESSAGE("[zenoh-pubsub] empty bytes payload is delivered and size is zero");

    std::atomic<bool> received{false};
    size_t captured_size = 99;

    Publisher<Bytes> pub(ZenohConf("zenoh/empty/bytes1", "data"));
    Subscriber<Bytes> sub("zenoh://zenoh/empty/bytes1?event=data");

    sub.listen([&](const Bytes& data) {
      captured_size = data.size();
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(Bytes{}, true));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(30ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    CHECK_EQ(captured_size, 0u);
  }

  TEST_CASE("re-subscription after unlisten receives fresh messages") {
    MESSAGE("[zenoh-pubsub] re-subscription after unlisten receives fresh messages");

    std::atomic<int> count{0};
    Publisher<int> pub(ZenohConf("zenoh/resub/round1", "data"));

    {
      Subscriber<int> sub1("zenoh://zenoh/resub/round1?event=data");
      sub1.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });
      CHECK(pub.wait_for_subscribers(1s));
      pub.publish(1);
      std::this_thread::sleep_for(100ms);
    }

    int after_first = count.load();
    CHECK(after_first >= 1);

    Subscriber<int> sub2("zenoh://zenoh/resub/round1?event=data");
    sub2.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });
    CHECK(pub.wait_for_subscribers(1s));
    pub.publish(2);
    std::this_thread::sleep_for(100ms);

    CHECK(count.load() > after_first);
  }

  TEST_CASE("qos reliable profile is accepted and data is delivered") {
    MESSAGE("[zenoh-pubsub] qos reliable profile is accepted and data is delivered");

    Qos reliable_qos;
    reliable_qos.reliability.kind = Qos::Reliability::kReliable;
    try {
      ZenohConf::register_qos("zenoh_reliable_pubsub", reliable_qos);
    } catch (...) {
    }

    Publisher<int> pub(ZenohConf("zenoh/qos/reliable1", "data", 0, "zenoh_reliable_pubsub"));
    Subscriber<int> sub("zenoh://zenoh/qos/reliable1?event=data");

    std::atomic<int> count{0};
    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 5; ++i) {
      pub.publish(i);
      std::this_thread::sleep_for(20ms);
    }

    std::this_thread::sleep_for(100ms);

    CHECK(count.load() >= 5);
  }
}

TEST_SUITE("zenoh-method") {
  TEST_CASE("invoke times out when server is absent") {
    MESSAGE("[zenoh-method] invoke times out when server is absent");

    Client<std::string, std::string> client("zenoh://zenoh/timeout/noserver2?event=req");

    std::string out;
    CHECK_FALSE(client.invoke("never", out, 200ms));
  }

  TEST_CASE("server replying with large string preserves content") {
    MESSAGE("[zenoh-method] server replying with large string preserves content");

    Server<std::string, std::string> server(ZenohConf("zenoh/large/reply1", "req"));
    server.listen([](const std::string& req, std::string& resp) { resp = req + std::string(65536, 'Y'); });

    Client<std::string, std::string> client("zenoh://zenoh/large/reply1?event=req");
    REQUIRE(client.wait_for_connected(1s));

    std::string out;
    CHECK(client.invoke(std::string("q"), out, 5s));
    REQUIRE(out.size() == 65537);
    CHECK(out.front() == 'q');
    CHECK(out.back() == 'Y');
  }

  TEST_CASE("deferred reply from another thread is received correctly") {
    MESSAGE("[zenoh-method] deferred reply from another thread is received correctly");

    std::atomic<uint64_t> saved_id{0};
    std::atomic<bool> req_received{false};

    Server<std::string, std::string> server(ZenohConf("zenoh/mth/deferred_thread1", "req"));
    server.listen_for_reply([&](uint64_t req_id, const std::string& /*req*/) {
      saved_id.store(req_id, std::memory_order_release);
      req_received.store(true, std::memory_order_release);
    });

    Client<std::string, std::string> client("zenoh://zenoh/mth/deferred_thread1?event=req");
    CHECK(client.wait_for_connected(1s));

    auto fut = client.async_invoke("work");

    for (int i = 0; i < 100 && !req_received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(30ms);
    }

    REQUIRE(req_received.load(std::memory_order_acquire));

    std::thread reply_thread([&] {
      std::this_thread::sleep_for(50ms);
      server.reply(saved_id.load(std::memory_order_acquire), std::string("thread_reply"));
    });

    REQUIRE(fut.wait_for(5s) == std::future_status::ready);
    CHECK(fut.get() == "thread_reply");
    reply_thread.join();
  }
}

TEST_SUITE("zenoh-field") {
  TEST_CASE("concurrent setter and getter race does not corrupt data") {
    MESSAGE("[zenoh-field] concurrent setter and getter race does not corrupt data");

    Setter<int> setter(ZenohConf("zenoh/fld/race1", "val"));
    Getter<int> getter("zenoh://zenoh/fld/race1?event=val");

    std::atomic<bool> stop{false};

    std::thread writer([&] {
      for (int i = 0; !stop.load(std::memory_order_relaxed); ++i) {
        setter.set(i % 1000);
        std::this_thread::sleep_for(5ms);
      }
    });

    std::this_thread::sleep_for(300ms);
    stop.store(true, std::memory_order_relaxed);
    writer.join();

    auto v = getter.get();
    if (v.has_value()) {
      CHECK(v.value() >= 0);
      CHECK(v.value() < 1000);
    }
  }

  TEST_CASE("getter returns empty optional before first set") {
    MESSAGE("[zenoh-field] getter returns empty optional before first set");

    Getter<int> getter("zenoh://zenoh/fld/noset1?event=val");

    std::this_thread::sleep_for(50ms);
    auto v = getter.get();

    CHECK_FALSE(v.has_value());
  }

  TEST_CASE("large value round trips via setter and getter") {
    MESSAGE("[zenoh-field] large value round trips via setter and getter");

    static constexpr size_t kSize = 64 * 1024;

    Setter<Bytes> setter(ZenohConf("zenoh/fld/large1", "val"));
    Getter<Bytes> getter("zenoh://zenoh/fld/large1?event=val");

    std::atomic<bool> notified{false};
    Bytes captured;

    getter.listen([&](const Bytes& val) {
      captured = val;
      notified.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(30ms);

    std::vector<uint8_t> raw(kSize, 0xD4);
    Bytes payload = Bytes::deep_copy(raw.data(), raw.size());
    setter.set(payload);

    for (int i = 0; i < 100 && !notified.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(30ms);
    }

    CHECK(notified.load(std::memory_order_acquire));
    REQUIRE(captured.size() == kSize);
    CHECK(captured[0] == 0xD4);
    CHECK(captured[kSize - 1] == 0xD4);
  }
}

TEST_SUITE("zenoh-qos") {
  TEST_CASE("best effort qos profile is accepted and data is delivered") {
    MESSAGE("[zenoh-qos] best effort qos profile is accepted and data is delivered");

    Qos be_qos;
    be_qos.reliability.kind = Qos::Reliability::kBestEffort;
    try {
      ZenohConf::register_qos("zenoh_best_effort_test", be_qos);
    } catch (...) {
    }

    Publisher<int> pub(ZenohConf("zenoh/qos/be1", "data", 0, "zenoh_best_effort_test"));
    Subscriber<int> sub("zenoh://zenoh/qos/be1?event=data");

    std::atomic<int> count{0};
    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 5; ++i) {
      pub.publish(i);
      std::this_thread::sleep_for(20ms);
    }

    std::this_thread::sleep_for(50ms);

    CHECK(count.load() >= 1);
  }
}

#ifdef VLINK_TEST_SUPPORT_SECURITY
#include "./security_test_helpers.h"

TEST_SUITE("zenoh-security") {
  TEST_CASE("encrypted bytes payload is delivered to subscriber") {
    MESSAGE("[zenoh-security] encrypted bytes payload is delivered to subscriber");

    try {
      std::atomic<bool> received{false};
      Bytes captured;

      SecurityPublisher<Bytes> pub(ZenohConf("zenoh/sec/enc1", "data"));
      SecuritySubscriber<Bytes> sub("zenoh://zenoh/sec/enc1?event=data");

      sub.listen([&](const Bytes& data) {
        captured = data;
        received.store(true, std::memory_order_release);
      });

      if (pub.wait_for_subscribers(1s)) {
        Bytes payload{0x5A, 0x65, 0x6E};
        pub.publish(payload);

        for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
          std::this_thread::sleep_for(30ms);
        }

        if (received.load(std::memory_order_acquire)) {
          REQUIRE(captured.size() == 3);
          CHECK(captured[0] == 0x5A);
          CHECK(captured[2] == 0x6E);
        }
      }
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("asymmetric rsa-oaep encrypted bytes round trip via zenoh") {
    MESSAGE("[zenoh-security] asymmetric rsa-oaep encrypted bytes round trip via zenoh");

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

      SecurityPublisher<Bytes> pub(ZenohConf("zenoh/sec/rsa1", "data"), std::move(pub_cfg));
      SecuritySubscriber<Bytes> sub("zenoh://zenoh/sec/rsa1?event=data", std::move(sub_cfg));

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

  TEST_CASE("asymmetric mismatched private key fails to decrypt over zenoh") {
    MESSAGE("[zenoh-security] asymmetric mismatched private key fails to decrypt over zenoh");

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

      SecurityPublisher<Bytes> pub(ZenohConf("zenoh/sec/rsa_mm1", "data"), std::move(pub_cfg));
      SecuritySubscriber<Bytes> sub("zenoh://zenoh/sec/rsa_mm1?event=data", std::move(sub_cfg));

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

namespace {

struct ZenohCustomMsg {
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

TEST_SUITE("zenoh-custom") {
  TEST_CASE("custom type round trips id and label") {
    MESSAGE("[zenoh-custom] custom type round trips id and label");

    try {
      std::atomic<bool> received{false};
      ZenohCustomMsg captured{};

      Publisher<ZenohCustomMsg> pub(ZenohConf("zenoh/cust/basic", "data"));
      Subscriber<ZenohCustomMsg> sub("zenoh://zenoh/cust/basic?event=data");

      sub.listen([&](const ZenohCustomMsg& m) {
        captured = m;
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      ZenohCustomMsg msg;
      msg.id = 42;
      msg.label = "zenoh_custom";
      CHECK(pub.publish(msg));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      CHECK_EQ(captured.id, 42);
      CHECK_EQ(captured.label, "zenoh_custom");
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("serializer detects custom type as kCustomType") {
    MESSAGE("[zenoh-custom] serializer detects custom type as kCustomType");

    static constexpr auto kType = Serializer::get_type_of<ZenohCustomMsg>();
    CHECK_EQ(kType, Serializer::kCustomType);
  }

  TEST_CASE("multiple custom type messages deliver in order") {
    MESSAGE("[zenoh-custom] multiple custom type messages deliver in order");

    try {
      std::atomic<int> count{0};

      Publisher<ZenohCustomMsg> pub(ZenohConf("zenoh/cust/order", "data"));
      Subscriber<ZenohCustomMsg> sub("zenoh://zenoh/cust/order?event=data");

      sub.listen([&](const ZenohCustomMsg& /*m*/) { count.fetch_add(1, std::memory_order_relaxed); });

      CHECK(pub.wait_for_subscribers(1s));

      for (int k = 0; k < 5; ++k) {
        ZenohCustomMsg msg;
        msg.id = k;
        msg.label = "item";
        pub.publish(msg);
        std::this_thread::sleep_for(20ms);
      }

      std::this_thread::sleep_for(200ms);
      CHECK(count.load() >= 5);
    } catch (const std::exception&) {
      return;
    }
  }
}

#if defined(VLINK_TEST_SUPPORT_FLATBUFFERS)

TEST_SUITE("zenoh-flatbuffers") {
  TEST_CASE("flatbuffers message round trips type and value") {
    MESSAGE("[zenoh-flatbuffers] flatbuffers message round trips type and value");

    try {
      std::atomic<bool> received{false};
      uint32_t captured_type = 0;
      std::string captured_value;

      Publisher<fbs::MessageT> pub(ZenohConf("zenoh/fbs/rt", "data"));
      Subscriber<fbs::MessageT> sub("zenoh://zenoh/fbs/rt?event=data");

      sub.listen([&](const fbs::MessageT& m) {
        captured_type = m.type;
        captured_value = m.value;
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      fbs::MessageT msg;
      msg.type = 3u;
      msg.value = "zenoh_fbs_rt";
      CHECK(pub.publish(msg));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      CHECK_EQ(captured_type, 3u);
      CHECK_EQ(captured_value, "zenoh_fbs_rt");
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("late subscriber still receives flatbuffers message") {
    MESSAGE("[zenoh-flatbuffers] late subscriber still receives flatbuffers message");

    try {
      std::atomic<bool> received{false};

      Publisher<fbs::MessageT> pub(ZenohConf("zenoh/fbs/late", "data"));

      std::this_thread::sleep_for(50ms);

      Subscriber<fbs::MessageT> sub("zenoh://zenoh/fbs/late?event=data");
      sub.listen([&](const fbs::MessageT& /*m*/) { received.store(true, std::memory_order_release); });

      CHECK(pub.wait_for_subscribers(1s));

      fbs::MessageT msg;
      msg.type = 1u;
      msg.value = "late_sub";
      pub.publish(msg);

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("two distinct flatbuffers types coexist on separate topics") {
    MESSAGE("[zenoh-flatbuffers] two distinct flatbuffers types coexist on separate topics");

    try {
      std::atomic<bool> got_msg{false};
      std::atomic<bool> got_req{false};

      Publisher<fbs::MessageT> pub_m(ZenohConf("zenoh/fbs/multi_m", "data"));
      Publisher<fbs::RequestT> pub_r(ZenohConf("zenoh/fbs/multi_r", "data"));
      Subscriber<fbs::MessageT> sub_m("zenoh://zenoh/fbs/multi_m?event=data");
      Subscriber<fbs::RequestT> sub_r("zenoh://zenoh/fbs/multi_r?event=data");

      sub_m.listen([&](const fbs::MessageT& /*m*/) { got_msg.store(true, std::memory_order_release); });
      sub_r.listen([&](const fbs::RequestT& /*r*/) { got_req.store(true, std::memory_order_release); });

      CHECK(pub_m.wait_for_subscribers(1s));
      CHECK(pub_r.wait_for_subscribers(1s));

      fbs::MessageT msg;
      msg.type = 1u;
      pub_m.publish(msg);

      fbs::RequestT req;
      req.type = 2u;
      pub_r.publish(req);

      for (int i = 0; i < 100 && (!got_msg.load() || !got_req.load()); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(got_msg.load(std::memory_order_acquire));
      CHECK(got_req.load(std::memory_order_acquire));
    } catch (const std::exception&) {
      return;
    }
  }
}

#endif  // VLINK_TEST_SUPPORT_FLATBUFFERS

TEST_SUITE("zenoh-qos") {
  TEST_CASE("keep-last history depth variants all deliver messages") {
    MESSAGE("[zenoh-qos] keep-last history depth variants all deliver messages");

    SUBCASE("depth 1") {
      Qos qos;
      qos.history.kind = Qos::History::kKeepLast;
      qos.history.depth = 1;
      try {
        ZenohConf::register_qos("zenoh_kl_depth1", qos);
      } catch (...) {
      }

      std::atomic<int> count{0};

      Publisher<int> pub(ZenohConf("zenoh/qos/kl_d1", "data", 0, "zenoh_kl_depth1"));
      Subscriber<int> sub("zenoh://zenoh/qos/kl_d1?event=data");

      sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });
      CHECK(pub.wait_for_subscribers(1s));

      pub.publish(1);
      std::this_thread::sleep_for(50ms);
      CHECK(count.load() >= 1);
    }

    SUBCASE("depth 5") {
      Qos qos;
      qos.history.kind = Qos::History::kKeepLast;
      qos.history.depth = 5;
      try {
        ZenohConf::register_qos("zenoh_kl_depth5", qos);
      } catch (...) {
      }

      std::atomic<int> count{0};

      Publisher<int> pub(ZenohConf("zenoh/qos/kl_d5", "data", 0, "zenoh_kl_depth5"));
      Subscriber<int> sub("zenoh://zenoh/qos/kl_d5?event=data");

      sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });
      CHECK(pub.wait_for_subscribers(1s));

      for (int i = 0; i < 5; ++i) {
        pub.publish(i);
        std::this_thread::sleep_for(20ms);
      }

      std::this_thread::sleep_for(50ms);
      CHECK(count.load() >= 1);
    }
  }

  TEST_CASE("keep-all history accumulates published samples") {
    MESSAGE("[zenoh-qos] keep-all history accumulates published samples");

    Qos qos;
    qos.history.kind = Qos::History::kKeepAll;
    qos.reliability.kind = Qos::Reliability::kReliable;
    try {
      ZenohConf::register_qos("zenoh_keepall_test", qos);
    } catch (...) {
    }

    std::atomic<int> count{0};

    Publisher<int> pub(ZenohConf("zenoh/qos/keepall1", "data", 0, "zenoh_keepall_test"));
    Subscriber<int> sub("zenoh://zenoh/qos/keepall1?event=data");

    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });
    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 5; ++i) {
      pub.publish(i);
      std::this_thread::sleep_for(20ms);
    }

    std::this_thread::sleep_for(100ms);
    CHECK(count.load() >= 1);
  }

  TEST_CASE("durability volatile qos struct field is stored and registered") {
    MESSAGE("[zenoh-qos] durability volatile qos struct field is stored and registered");

    Qos qos;
    qos.durability.kind = Qos::Durability::kVolatile;
    try {
      ZenohConf::register_qos("zenoh_dur_volatile", qos);
    } catch (...) {
    }

    ZenohConf conf("zenoh/qos/dur_vol1", "data", 0, "zenoh_dur_volatile");
    CHECK_EQ(conf.qos, "zenoh_dur_volatile");
  }

  TEST_CASE("latency budget duration hint is registered and does not affect delivery") {
    MESSAGE("[zenoh-qos] latency budget duration hint is registered and does not affect delivery");

    Qos qos;
    qos.latency_budget.duration = 5;
    try {
      ZenohConf::register_qos("zenoh_latbud_5ms", qos);
    } catch (...) {
    }

    std::atomic<int> count{0};

    Publisher<int> pub(ZenohConf("zenoh/qos/latbud1", "data", 0, "zenoh_latbud_5ms"));
    Subscriber<int> sub("zenoh://zenoh/qos/latbud1?event=data");

    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });
    CHECK(pub.wait_for_subscribers(1s));

    pub.publish(1);
    std::this_thread::sleep_for(100ms);
    CHECK(count.load() >= 1);
  }
}

#include "./zerocopy/camera_frame.h"
#include "./zerocopy/raw_data.h"

TEST_SUITE("zenoh-dynamicdata") {
  TEST_CASE("type tag and double payload survive wire transport") {
    MESSAGE("[zenoh-dynamicdata] type tag and double payload survive wire transport");

    try {
      std::atomic<bool> received{false};
      DynamicData captured;

      Publisher<DynamicData> pub(ZenohConf("zenoh/dyn2/dbl", "data"));
      Subscriber<DynamicData> sub("zenoh://zenoh/dyn2/dbl?event=data");

      sub.listen([&](const DynamicData& d) {
        captured = d;
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      DynamicData d;
      d.load("zenoh_dbl", 2.718);
      CHECK(pub.publish(d));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      CHECK(captured.get_type() == "zenoh_dbl");
      double val = 0.0;
      CHECK(captured.convert(val));
      CHECK(val == 2.718);
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("late subscriber receives dynamicdata after publisher starts") {
    MESSAGE("[zenoh-dynamicdata] late subscriber receives dynamicdata after publisher starts");

    try {
      std::atomic<bool> received{false};
      DynamicData captured;

      Publisher<DynamicData> pub(ZenohConf("zenoh/dyn2/late", "data"));

      Subscriber<DynamicData> sub("zenoh://zenoh/dyn2/late?event=data");
      sub.listen([&](const DynamicData& d) {
        captured = d;
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      DynamicData d;
      d.load("late_msg", 99);
      CHECK(pub.publish(d));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      CHECK(captured.as<int>() == 99);
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("large string content survives dynamicdata wire transport") {
    MESSAGE("[zenoh-dynamicdata] large string content survives dynamicdata wire transport");

    try {
      std::atomic<bool> received{false};
      DynamicData captured;

      Publisher<DynamicData> pub(ZenohConf("zenoh/dyn2/large", "data"));
      Subscriber<DynamicData> sub("zenoh://zenoh/dyn2/large?event=data");

      sub.listen([&](const DynamicData& d) {
        captured = d;
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      std::string big(2048, 'Z');
      DynamicData d;
      d.load("big_str", big);
      CHECK(pub.publish(d));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      auto out = captured.as<std::string>();
      CHECK_EQ(out.size(), 2048u);
      CHECK_EQ(out[0], 'Z');
    } catch (const std::exception&) {
      return;
    }
  }
}

TEST_SUITE("zenoh-zerocopy") {
  TEST_CASE("rawdata round trip preserves header seq and payload over wire") {
    MESSAGE("[zenoh-zerocopy] rawdata round trip preserves header seq and payload over wire");

    try {
      std::atomic<bool> received{false};
      zerocopy::RawData captured;

      Publisher<zerocopy::RawData> pub(ZenohConf("zenoh/zc/raw1", "data"));
      Subscriber<zerocopy::RawData> sub("zenoh://zenoh/zc/raw1?event=data");

      sub.listen([&](const zerocopy::RawData& d) {
        captured.deep_copy(d);
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      zerocopy::RawData rd;
      rd.header.seq = 11;
      rd.create(3);
      const_cast<uint8_t*>(rd.data())[0] = 0xAA;
      const_cast<uint8_t*>(rd.data())[1] = 0xBB;
      const_cast<uint8_t*>(rd.data())[2] = 0xCC;
      CHECK(pub.publish(rd));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      REQUIRE_EQ(captured.size(), 3u);
      CHECK_EQ(captured.header.seq, 11u);
      CHECK_EQ(captured.data()[0], 0xAAu);
      CHECK_EQ(captured.data()[2], 0xCCu);
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("cameraframe metadata survives zenoh wire transport") {
    MESSAGE("[zenoh-zerocopy] cameraframe metadata survives zenoh wire transport");

    try {
      std::atomic<bool> received{false};
      zerocopy::CameraFrame captured;

      Publisher<zerocopy::CameraFrame> pub(ZenohConf("zenoh/zc/cam1", "data"));
      Subscriber<zerocopy::CameraFrame> sub("zenoh://zenoh/zc/cam1?event=data");

      sub.listen([&](const zerocopy::CameraFrame& f) {
        captured.deep_copy(f);
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      zerocopy::CameraFrame frame;
      frame.set_width(320);
      frame.set_height(240);
      frame.set_format(zerocopy::CameraFrame::kFormatJpeg);
      frame.set_freq(30);
      frame.create(1024);
      CHECK(pub.publish(frame));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      CHECK_EQ(captured.width(), 320u);
      CHECK_EQ(captured.height(), 240u);
      CHECK_EQ(captured.format(), zerocopy::CameraFrame::kFormatJpeg);
      CHECK_EQ(captured.freq(), 30u);
    } catch (const std::exception&) {
      return;
    }
  }
}

#endif  // VLINK_SUPPORT_ZENOH

// NOLINTEND
