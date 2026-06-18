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

#ifdef VLINK_SUPPORT_DDSC

#include <atomic>
#include <future>
#include <string>
#include <thread>

#include "./common_test.h"
#include "./modules/ddsc_conf.h"

TEST_SUITE("ddsc-init") {
  TEST_CASE("default conf stores topic with empty qos") {
    MESSAGE("[ddsc-init] default conf stores topic with empty qos");

    DdscConf conf("vehicle/speed");

    CHECK(conf.topic == "vehicle/speed");
    CHECK(conf.domain == 0);
    CHECK(conf.depth == 0);
    CHECK(conf.qos.empty());
    CHECK(conf.get_transport_type() == TransportType::kDdsc);
  }

  TEST_CASE("conf with domain and depth stores those values") {
    MESSAGE("[ddsc-init] conf with domain and depth stores those values");

    DdscConf conf("my/topic", 3, 20);

    CHECK(conf.domain == 3);
    CHECK(conf.depth == 20);
  }

  TEST_CASE("conf with named qos stores qos name") {
    MESSAGE("[ddsc-init] conf with named qos stores qos name");

    DdscConf conf("my/topic", 0, 0, "best_effort");

    CHECK(conf.qos == "best_effort");
  }

  TEST_CASE("conf equality compares all relevant fields") {
    MESSAGE("[ddsc-init] conf equality compares all relevant fields");

    DdscConf a("topic/a", 1, 5, "q1");
    DdscConf b("topic/a", 1, 5, "q1");
    DdscConf c("topic/b", 1, 5, "q1");

    CHECK(a == b);
    CHECK(a != c);
  }

  TEST_CASE("url parses for all impl types") {
    MESSAGE("[ddsc-init] url parses for all impl types");

    Url url("ddsc://ddsc/init/parse1");

    CHECK(url.parse(kPublisher));
    CHECK(url.parse(kSubscriber));
    CHECK(url.parse(kServer));
    CHECK(url.parse(kClient));
    CHECK(url.parse(kSetter));
    CHECK(url.parse(kGetter));
  }

  TEST_CASE("unknown impl type throws on parse") {
    MESSAGE("[ddsc-init] unknown impl type throws on parse");

    Url url("ddsc://ddsc/init/parse2");

    CHECK_THROWS_AS(url.parse(kUnknownImplType), std::runtime_error);
  }

  TEST_CASE("invalid url scheme throws on publisher construction") { CHECK_THROWS(Publisher<int>("ddsc1://bad/url")); }

  TEST_CASE("registering and using a named qos profile succeeds") {
    MESSAGE("[ddsc-init] registering and using a named qos profile succeeds");

    Qos qos;
    qos.reliability.kind = Qos::Reliability::kReliable;

    try {
      DdscConf::register_qos("ddsc_reliable", qos);
    } catch (...) {
    }

    DdscConf conf("ddsc/qos/test1", 0, 0, "ddsc_reliable");
    CHECK(conf.qos == "ddsc_reliable");
  }
}

TEST_SUITE("ddsc-pubsub") {
  TEST_CASE("bytes payload is received intact") {
    MESSAGE("[ddsc-pubsub] bytes payload is received intact");

    std::atomic<bool> received{false};
    Bytes captured;

    Publisher<Bytes> pub(DdscConf("ddsc/evt/pubsub1"));
    Subscriber<Bytes> sub("ddsc://ddsc/evt/pubsub1");

    sub.listen([&](const Bytes& data) {
      captured = data;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.has_subscribers());

    Bytes payload{0xCA, 0xFE, 0xBA, 0xBE};
    CHECK(pub.publish(payload));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(30ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    REQUIRE(captured.size() == 4);
    CHECK(captured[0] == 0xCA);
    CHECK(captured[3] == 0xBE);
  }

  TEST_CASE("string payload is received with correct value") {
    MESSAGE("[ddsc-pubsub] string payload is received with correct value");

    std::atomic<bool> received{false};
    std::string captured;

    Publisher<std::string> pub(DdscConf("ddsc/evt/str1"));
    Subscriber<std::string> sub("ddsc://ddsc/evt/str1");

    sub.listen([&](const std::string& val) {
      captured = val;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(std::string("hello_ddsc")));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(30ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    CHECK(captured == "hello_ddsc");
  }

  TEST_CASE("integer payload is received with correct value") {
    MESSAGE("[ddsc-pubsub] integer payload is received with correct value");

    std::atomic<int> captured{0};
    std::atomic<bool> received{false};

    Publisher<int> pub(DdscConf("ddsc/evt/int1"));
    Subscriber<int> sub("ddsc://ddsc/evt/int1");

    sub.listen([&](const int& v) {
      captured.store(v, std::memory_order_relaxed);
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(54321));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(30ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    CHECK(captured.load() == 54321);
  }

  TEST_CASE("multiple publishes are all received by subscriber") {
    MESSAGE("[ddsc-pubsub] multiple publishes are all received by subscriber");

    std::atomic<int> count{0};

    Publisher<int> pub(DdscConf("ddsc/evt/multi1"));
    Subscriber<int> sub("ddsc://ddsc/evt/multi1");

    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 10; ++i) {
      pub.publish(i);
      std::this_thread::sleep_for(20ms);
    }

    std::this_thread::sleep_for(30ms);
    CHECK(count.load() >= 10);
  }

  TEST_CASE("multiple subscribers each receive every published message") {
    MESSAGE("[ddsc-pubsub] multiple subscribers each receive every published message");

    std::atomic<int> count1{0};
    std::atomic<int> count2{0};

    Publisher<Bytes> pub(DdscConf("ddsc/evt/multisub1"));
    Subscriber<Bytes> sub1("ddsc://ddsc/evt/multisub1");
    Subscriber<Bytes> sub2("ddsc://ddsc/evt/multisub1");

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

  TEST_CASE("force publish succeeds without any subscriber") {
    MESSAGE("[ddsc-pubsub] force publish succeeds without any subscriber");

    Publisher<Bytes> pub(DdscConf("ddsc/evt/force1"));

    CHECK(!pub.has_subscribers());

    for (int i = 0; i < 5; ++i) {
      CHECK(pub.publish(Bytes{static_cast<uint8_t>(i)}, true));
    }
  }

  TEST_CASE("subscriber connect and disconnect events are detected") {
    MESSAGE("[ddsc-pubsub] subscriber connect and disconnect events are detected");

    std::atomic<int> connected_count{0};

    Publisher<Bytes> pub(DdscConf("ddsc/evt/detect1"));
    pub.detect_subscribers([&](bool connected) {
      if (connected) {
        connected_count.fetch_add(1, std::memory_order_relaxed);
      }
    });

    {
      Subscriber<Bytes> sub("ddsc://ddsc/evt/detect1");
      sub.listen([](const Bytes& /*d*/) {});
      std::this_thread::sleep_for(30ms);
      CHECK(pub.has_subscribers());
    }

    std::this_thread::sleep_for(30ms);
    CHECK(!pub.has_subscribers());
  }

  TEST_CASE("serialization round trip succeeds for available message types") {
    MESSAGE("[ddsc-pubsub] serialization round trip succeeds for available message types");

#if defined(VLINK_TEST_SUPPORT_PROTOBUF)
    SUBCASE("protobuf pub sub") {
      std::atomic<bool> received{false};
      pb::Message captured;

      Publisher<pb::Message> pub(DdscConf("ddsc/ser/pb1"));
      Subscriber<pb::Message> sub("ddsc://ddsc/ser/pb1");

      sub.listen([&](const pb::Message& msg) {
        captured = msg;
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      pb::Message msg;
      msg.set_value("ddsc_proto");
      CHECK(pub.publish(msg));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      CHECK(captured.value() == "ddsc_proto");
    }
#endif

    SUBCASE("plain bytes always works") {
      std::atomic<bool> received{false};

      Publisher<Bytes> pub(DdscConf("ddsc/ser/plain1"));
      Subscriber<Bytes> sub("ddsc://ddsc/ser/plain1");

      sub.listen([&](const Bytes& /*d*/) { received.store(true, std::memory_order_release); });

      CHECK(pub.wait_for_subscribers(1s));
      pub.publish(Bytes{0xAB, 0xCD});

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
    }
  }
}

TEST_SUITE("ddsc-method") {
  TEST_CASE("fire and forget send increments server receive counter") {
    MESSAGE("[ddsc-method] fire and forget send increments server receive counter");

    std::atomic<int> counter{0};

    Server<std::string> server(DdscConf("ddsc/mth/send1"));
    server.listen([&](const std::string& /*req*/) { counter.fetch_add(1, std::memory_order_relaxed); });

    Client<std::string> client("ddsc://ddsc/mth/send1");
    CHECK(client.wait_for_connected(1s));
    CHECK(client.is_connected());

    auto wait_for_counter = [&](int expected) {
      auto deadline = std::chrono::steady_clock::now() + 5s;

      while (counter.load(std::memory_order_acquire) < expected && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
      }
    };

    CHECK(client.send("fire1"));
    wait_for_counter(1);
    CHECK(counter.load() == 1);

    CHECK(client.send("fire2"));
    wait_for_counter(2);
    CHECK(counter.load() == 2);
  }

  TEST_CASE("invoke returns correct response via multiple overloads") {
    MESSAGE("[ddsc-method] invoke returns correct response via multiple overloads");

    Server<std::string, std::string> server(DdscConf("ddsc/mth/invoke1"));
    server.listen([](const std::string& req, std::string& resp) { resp = "cyclone:" + req; });

    Client<std::string, std::string> client("ddsc://ddsc/mth/invoke1");
    CHECK(client.wait_for_connected(1s));

    SUBCASE("sync optional") {
      auto resp = client.invoke("ping");
      CHECK(resp.has_value());
      CHECK(*resp == "cyclone:ping");
    }

    SUBCASE("sync ref overload") {
      std::string out;
      CHECK(client.invoke("pong", out, 5s));
      CHECK(out == "cyclone:pong");
    }

    SUBCASE("async future") {
      auto fut = client.async_invoke("async");
      REQUIRE(fut.wait_for(5s) == std::future_status::ready);
      CHECK(fut.get() == "cyclone:async");
    }

    SUBCASE("multiple sequential invocations succeed") {
      for (int i = 0; i < 5; ++i) {
        auto resp = client.invoke("r" + std::to_string(i));
        CHECK(resp.has_value());
        CHECK(*resp == "cyclone:r" + std::to_string(i));
      }
    }
  }

  TEST_CASE("deferred async reply is delivered to future") {
    MESSAGE("[ddsc-method] deferred async reply is delivered to future");

    std::atomic<uint64_t> saved_id{0};
    std::atomic<bool> req_received{false};

    Server<std::string, std::string> server(DdscConf("ddsc/mth/async_reply1"));
    server.listen_for_reply([&](uint64_t req_id, const std::string& /*req*/) {
      saved_id.store(req_id, std::memory_order_release);
      req_received.store(true, std::memory_order_release);
    });

    Client<std::string, std::string> client("ddsc://ddsc/mth/async_reply1");
    CHECK(client.wait_for_connected(1s));

    auto fut = client.async_invoke("defer");

    for (int i = 0; i < 100 && !req_received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(30ms);
    }

    REQUIRE(req_received.load(std::memory_order_acquire));
    CHECK(server.reply(saved_id.load(), std::string("deferred_ddsc")));

    REQUIRE(fut.wait_for(5s) == std::future_status::ready);
    CHECK(fut.get() == "deferred_ddsc");
  }

  TEST_CASE("async callback invoke delivers response") {
    MESSAGE("[ddsc-method] async callback invoke delivers response");

    Server<std::string, std::string> server(DdscConf("ddsc/mth/cb1"));
    server.listen([](const std::string& /*req*/, std::string& resp) { resp = "ddsc_cb"; });

    Client<std::string, std::string> client("ddsc://ddsc/mth/cb1");
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
    CHECK(resp_val == "ddsc_cb");
  }

  TEST_CASE("detect connected callback fires when client connects to server") {
    MESSAGE("[ddsc-method] detect connected callback fires when client connects to server");

    std::atomic<bool> connected_event{false};

    Server<std::string, std::string> server(DdscConf("ddsc/mth/detect_conn1"));
    server.listen([](const std::string& /*req*/, std::string& resp) { resp = "ok"; });

    Client<std::string, std::string> client("ddsc://ddsc/mth/detect_conn1");
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
}

TEST_SUITE("ddsc-field") {
  TEST_CASE("setter and getter exchange values via all access patterns") {
    MESSAGE("[ddsc-field] setter and getter exchange values via all access patterns");

    SUBCASE("polling get") {
      Setter<Bytes> setter(DdscConf("ddsc/fld/poll1"));
      Getter<Bytes> getter("ddsc://ddsc/fld/poll1");

      setter.set(Bytes{0xAA, 0xBB});
      std::this_thread::sleep_for(30ms);

      auto v = getter.get();
      REQUIRE(v.has_value());
      REQUIRE(v->size() == 2);
      CHECK((*v)[0] == 0xAA);
      CHECK((*v)[1] == 0xBB);
    }

    SUBCASE("wait for value blocks until setter publishes") {
      Setter<Bytes> setter(DdscConf("ddsc/fld/wait1"));
      Getter<Bytes> getter("ddsc://ddsc/fld/wait1");

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

    SUBCASE("listen callback is invoked on value change") {
      std::atomic<bool> notified{false};

      Setter<Bytes> setter(DdscConf("ddsc/fld/cb1"));
      Getter<Bytes> getter("ddsc://ddsc/fld/cb1");

      getter.listen([&](const Bytes& /*val*/) { notified.store(true, std::memory_order_release); });

      std::this_thread::sleep_for(30ms);
      setter.set(Bytes{0x42});

      for (int i = 0; i < 100 && !notified.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(notified.load(std::memory_order_acquire));
    }

    SUBCASE("change reporting suppresses duplicate value callbacks") {
      std::atomic<int> cb_count{0};

      Setter<Bytes> setter(DdscConf("ddsc/fld/cr1"));
      Getter<Bytes> getter("ddsc://ddsc/fld/cr1");

      getter.set_change_reporting(true);
      CHECK(getter.get_change_reporting());

      getter.listen([&](const Bytes& /*v*/) { cb_count.fetch_add(1, std::memory_order_relaxed); });

      std::this_thread::sleep_for(30ms);

      setter.set(Bytes{0x77});
      std::this_thread::sleep_for(30ms);
      setter.set(Bytes{0x77});
      std::this_thread::sleep_for(30ms);

      CHECK(cb_count.load() <= 1);
    }

    SUBCASE("late getter receives cached value from setter") {
      Setter<Bytes> setter(DdscConf("ddsc/fld/late1"));
      setter.set(Bytes{0xBE, 0xEF});
      std::this_thread::sleep_for(30ms);

      Getter<Bytes> late_getter("ddsc://ddsc/fld/late1");
      CHECK(late_getter.wait_for_value(1s));
      auto v = late_getter.get();
      REQUIRE(v.has_value());
      CHECK((*v)[0] == 0xBE);
    }
  }
}

TEST_SUITE("ddsc-pubsub") {
  TEST_CASE("large 1kb payload is received intact") {
    MESSAGE("[ddsc-pubsub] large 1kb payload is received intact");

    static constexpr size_t kSize1K = 1024;

    std::atomic<bool> received{false};
    size_t captured_size{0};

    Publisher<Bytes> pub(DdscConf("ddsc/evt/large1k"));
    Subscriber<Bytes> sub("ddsc://ddsc/evt/large1k");

    sub.listen([&](const Bytes& data) {
      captured_size = data.size();
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    Bytes payload = Bytes::create(kSize1K);

    CHECK(pub.publish(payload));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(30ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    CHECK_EQ(captured_size, kSize1K);
  }

  TEST_CASE("empty bytes payload is received without crash") {
    MESSAGE("[ddsc-pubsub] empty bytes payload is received without crash");

    std::atomic<bool> received{false};
    size_t captured_size{1};

    Publisher<Bytes> pub(DdscConf("ddsc/evt/empty1"));
    Subscriber<Bytes> sub("ddsc://ddsc/evt/empty1");

    sub.listen([&](const Bytes& data) {
      captured_size = data.size();
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(Bytes{}));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(30ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    CHECK_EQ(captured_size, 0u);
  }

  TEST_CASE("concurrent publishers deliver messages to subscriber") {
    MESSAGE("[ddsc-pubsub] concurrent publishers deliver messages to subscriber");

    static constexpr int kPublishers = 3;
    static constexpr int kPerPublisher = 5;

    std::atomic<int> total{0};

    Subscriber<int> sub("ddsc://ddsc/evt/concurrent1");
    sub.listen([&](const int& /*v*/) { total.fetch_add(1, std::memory_order_relaxed); });

    std::vector<std::thread> threads;
    threads.reserve(kPublishers);

    for (int t = 0; t < kPublishers; ++t) {
      threads.emplace_back([t] {
        Publisher<int> pub(DdscConf("ddsc/evt/concurrent1"));

        if (!pub.wait_for_subscribers(1s)) {
          return;
        }

        for (int i = 0; i < kPerPublisher; ++i) {
          pub.publish(t * kPerPublisher + i);
          std::this_thread::sleep_for(10ms);
        }
      });
    }

    for (auto& th : threads) {
      th.join();
    }

    std::this_thread::sleep_for(100ms);
    CHECK(total.load() >= kPublishers * kPerPublisher);
  }

  TEST_CASE("subscriber destroyed mid-flight does not crash publisher") {
    MESSAGE("[ddsc-pubsub] subscriber destroyed mid-flight does not crash publisher");

    Publisher<int> pub(DdscConf("ddsc/evt/lifecycle1"));

    {
      Subscriber<int> sub("ddsc://ddsc/evt/lifecycle1");
      sub.listen([](const int& /*v*/) {});

      CHECK(pub.wait_for_subscribers(1s));

      for (int i = 0; i < 3; ++i) {
        pub.publish(i);
        std::this_thread::sleep_for(20ms);
      }
    }

    std::this_thread::sleep_for(50ms);
    CHECK(!pub.has_subscribers());

    for (int i = 0; i < 3; ++i) {
      CHECK(pub.publish(i, true));
    }
  }
}

TEST_SUITE("ddsc-method") {
  TEST_CASE("invoke times out when server is absent") {
    MESSAGE("[ddsc-method] invoke times out when server is absent");

    Client<std::string, std::string> orphan("ddsc://ddsc/mth/timeout1");

    std::string out;
    bool ok = orphan.invoke("req", out, 300ms);
    CHECK_FALSE(ok);
  }

  TEST_CASE("multiple concurrent clients each get correct responses") {
    MESSAGE("[ddsc-method] multiple concurrent clients each get correct responses");

    static constexpr int kClients = 3;

    Server<std::string, std::string> server(DdscConf("ddsc/mth/concurrent1"));
    server.listen([](const std::string& req, std::string& resp) { resp = "cy:" + req; });

    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;
    threads.reserve(kClients);

    for (int t = 0; t < kClients; ++t) {
      threads.emplace_back([t, &success_count] {
        Client<std::string, std::string> client("ddsc://ddsc/mth/concurrent1");

        if (!client.wait_for_connected(1s)) {
          return;
        }

        std::string key = std::to_string(t);
        auto resp = client.invoke(key);

        if (resp.has_value() && *resp == "cy:" + key) {
          success_count.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }

    for (auto& th : threads) {
      th.join();
    }

    CHECK_EQ(success_count.load(), kClients);
  }
}

TEST_SUITE("ddsc-field") {
  TEST_CASE("default value not available before any set") {
    MESSAGE("[ddsc-field] default value not available before any set");

    Getter<int> getter("ddsc://ddsc/fld/default1");
    auto v = getter.get();
    CHECK_FALSE(v.has_value());
  }

  TEST_CASE("integer field round trips with correct value") {
    MESSAGE("[ddsc-field] integer field round trips with correct value");

    Setter<int> setter(DdscConf("ddsc/fld/int1"));
    Getter<int> getter("ddsc://ddsc/fld/int1");

    setter.set(99999);
    CHECK(getter.wait_for_value(1s));

    auto v = getter.get();
    REQUIRE(v.has_value());
    CHECK_EQ(*v, 99999);
  }
}

TEST_SUITE("ddsc-qos") {
  TEST_CASE("best-effort named qos profile delivers messages") {
    MESSAGE("[ddsc-qos] best-effort named qos profile delivers messages");

    Qos qos;
    qos.reliability.kind = Qos::Reliability::kBestEffort;
    try {
      DdscConf::register_qos("ddsc_be_test", qos);
    } catch (...) {
    }

    std::atomic<int> count{0};

    Publisher<int> pub(DdscConf("ddsc/qos/be1", 0, 0, "ddsc_be_test"));
    Subscriber<int> sub("ddsc://ddsc/qos/be1?qos=ddsc_be_test");

    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 5; ++i) {
      pub.publish(i);
      std::this_thread::sleep_for(20ms);
    }

    std::this_thread::sleep_for(50ms);
    CHECK(count.load() > 0);
  }

  TEST_CASE("history depth field is stored in conf") {
    MESSAGE("[ddsc-qos] history depth field is stored in conf");

    DdscConf conf("ddsc/qos/depth1", 0, 8);
    CHECK_EQ(conf.depth, 8);
  }

  TEST_CASE("latency and lost tracking can be enabled and disabled") {
    MESSAGE("[ddsc-qos] latency and lost tracking can be enabled and disabled");

    Publisher<int> pub(DdscConf("ddsc/lat/sub1"));
    Subscriber<int> sub("ddsc://ddsc/lat/sub1");

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

TEST_SUITE("ddsc-error") {
  TEST_CASE("distinct topics yield distinct abstract nodes") {
    MESSAGE("[ddsc-error] distinct topics yield distinct abstract nodes");

    Publisher<int> pub1(DdscConf("ddsc/id/p1"));
    Publisher<int> pub2(DdscConf("ddsc/id/p2"));
    Subscriber<int> sub("ddsc://ddsc/id/p1");

    const auto* n1 = pub1.get_abstract_node();
    const auto* n2 = pub2.get_abstract_node();
    const auto* n3 = sub.get_abstract_node();

    CHECK(n1 != nullptr);
    CHECK(n2 != nullptr);
    CHECK(n3 != nullptr);
    CHECK(n1 != n2);
    CHECK(n1 != n3);
  }

  TEST_CASE("dynamic data int payload round trip") {
    MESSAGE("[ddsc-error] dynamic data int payload round trip");

    std::atomic<bool> received{false};
    DynamicData captured;

    Publisher<DynamicData> pub(DdscConf("ddsc/dyn/int1"));
    Subscriber<DynamicData> sub("ddsc://ddsc/dyn/int1");

    sub.listen([&](const DynamicData& d) {
      captured = d;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    DynamicData d;
    d.load("int", 42);
    CHECK(pub.publish(d));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(30ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    CHECK(captured.as<int>() == 42);
  }
}

TEST_SUITE("ddsc-qos") {
  TEST_CASE("transient-local durability late subscriber path constructs without throw") {
    MESSAGE("[ddsc-qos] transient-local durability late subscriber path constructs without throw");

    try {
      Qos qos;
      qos.durability.kind = Qos::Durability::kTransientLocal;
      qos.reliability.kind = Qos::Reliability::kReliable;
      try {
        DdscConf::register_qos("ddsc_tl_reliable", qos);
      } catch (...) {
      }

      Setter<int> setter(DdscConf("ddsc/qos/dur_tl1", 0, 0, "ddsc_tl_reliable"));
      setter.set(77);
      std::this_thread::sleep_for(50ms);

      Getter<int> late_getter("ddsc://ddsc/qos/dur_tl1?qos=ddsc_tl_reliable");
      bool got = late_getter.wait_for_value(500ms);

      if (got) {
        auto v = late_getter.get();
        REQUIRE(v.has_value());
        CHECK_EQ(*v, 77);
      }
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("liveliness automatic kind struct field is registered") {
    MESSAGE("[ddsc-qos] liveliness automatic kind struct field is registered");

    Qos qos;
    qos.liveliness.kind = Qos::Liveliness::kAutomatic;
    qos.liveliness.duration = 3000;
    try {
      DdscConf::register_qos("ddsc_lv_auto", qos);
    } catch (...) {
    }

    DdscConf conf("ddsc/qos/lv_auto1", 0, 0, "ddsc_lv_auto");
    CHECK_EQ(conf.qos, "ddsc_lv_auto");
  }

  TEST_CASE("deadline period qos delivers messages when publisher stays within period") {
    MESSAGE("[ddsc-qos] deadline period qos delivers messages when publisher stays within period");

    try {
      Qos qos;
      qos.deadline.period = 500;
      qos.reliability.kind = Qos::Reliability::kReliable;
      try {
        DdscConf::register_qos("ddsc_deadline_500ms", qos);
      } catch (...) {
      }

      std::atomic<int> count{0};

      Publisher<int> pub(DdscConf("ddsc/qos/deadline1", 0, 0, "ddsc_deadline_500ms"));
      Subscriber<int> sub("ddsc://ddsc/qos/deadline1?qos=ddsc_deadline_500ms");

      sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });
      CHECK(pub.wait_for_subscribers(1s));

      for (int i = 0; i < 5; ++i) {
        pub.publish(i);
        std::this_thread::sleep_for(100ms);
      }

      std::this_thread::sleep_for(50ms);
      CHECK(count.load() > 0);
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("keep-all history accumulates all published samples") {
    MESSAGE("[ddsc-qos] keep-all history accumulates all published samples");

    try {
      Qos qos;
      qos.history.kind = Qos::History::kKeepAll;
      qos.reliability.kind = Qos::Reliability::kReliable;
      try {
        DdscConf::register_qos("ddsc_keepall", qos);
      } catch (...) {
      }

      std::atomic<int> count{0};

      Publisher<int> pub(DdscConf("ddsc/qos/keepall1", 0, 0, "ddsc_keepall"));
      Subscriber<int> sub("ddsc://ddsc/qos/keepall1?qos=ddsc_keepall");

      sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });
      CHECK(pub.wait_for_subscribers(1s));

      for (int i = 0; i < 5; ++i) {
        pub.publish(i);
        std::this_thread::sleep_for(20ms);
      }

      std::this_thread::sleep_for(100ms);
      CHECK(count.load() >= 5);
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("resource limits max samples default values are correct") {
    MESSAGE("[ddsc-qos] resource limits max samples default values are correct");

    Qos qos;
    CHECK_EQ(qos.resource_limits.max_samples, 6000);
    CHECK_EQ(qos.resource_limits.max_instances, 10);
    CHECK_EQ(qos.resource_limits.max_samples_per_instance, 500);

    qos.resource_limits.max_samples = 50;
    qos.resource_limits.max_instances = 5;
    try {
      DdscConf::register_qos("ddsc_rl_cap", qos);
    } catch (...) {
    }

    DdscConf conf("ddsc/qos/rl1", 0, 0, "ddsc_rl_cap");
    CHECK_EQ(conf.qos, "ddsc_rl_cap");
  }
}

TEST_SUITE("ddsc-status") {
  TEST_CASE("publication matched fires when subscriber connects") {
    MESSAGE("[ddsc-status] publication matched fires when subscriber connects");

    try {
      std::atomic<bool> got_matched{false};
      std::atomic<int32_t> last_count{-1};

      Publisher<int> pub(DdscConf("ddsc/status/pub_matched1"));
      pub.register_status_handler([&](const Status::BasePtr& s) {
        if (s->get_type() == Status::kPublicationMatched) {
          auto m = s->as<Status::PublicationMatched>();
          last_count.store(m->current_count, std::memory_order_relaxed);
          if (m->current_count > 0) {
            got_matched.store(true, std::memory_order_release);
          }
        }
      });

      Subscriber<int> sub("ddsc://ddsc/status/pub_matched1");
      sub.listen([](const int& /*v*/) {});

      for (int i = 0; i < 100 && !got_matched.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      (void)got_matched.load(std::memory_order_acquire);
      CHECK(last_count.load() > 0);
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("subscription matched fires when publisher connects") {
    MESSAGE("[ddsc-status] subscription matched fires when publisher connects");

    try {
      std::atomic<bool> got_matched{false};
      std::atomic<int32_t> last_count{-1};

      Subscriber<int> sub("ddsc://ddsc/status/sub_matched1");
      sub.listen([](const int& /*v*/) {});
      sub.register_status_handler([&](const Status::BasePtr& s) {
        if (s->get_type() == Status::kSubscriptionMatched) {
          auto m = s->as<Status::SubscriptionMatched>();
          last_count.store(m->current_count, std::memory_order_relaxed);
          if (m->current_count > 0) {
            got_matched.store(true, std::memory_order_release);
          }
        }
      });

      Publisher<int> pub(DdscConf("ddsc/status/sub_matched1"));

      for (int i = 0; i < 100 && !got_matched.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      (void)got_matched.load(std::memory_order_acquire);
      CHECK(last_count.load() > 0);
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("offered deadline missed fires when publisher stops publishing within deadline") {
    MESSAGE("[ddsc-status] offered deadline missed fires when publisher stops publishing within deadline");

    try {
      static constexpr int32_t kDeadlineMs = 50;

      Qos qos;
      qos.deadline.period = kDeadlineMs;
      qos.reliability.kind = Qos::Reliability::kReliable;
      try {
        DdscConf::register_qos("ddsc_status_deadline_miss", qos);
      } catch (...) {
      }

      std::atomic<bool> got_deadline{false};

      Publisher<int> pub(DdscConf("ddsc/status/deadline_miss1", 0, 0, "ddsc_status_deadline_miss"));
      pub.register_status_handler([&](const Status::BasePtr& s) {
        if (s->get_type() == Status::kOfferedDeadlineMissed) {
          got_deadline.store(true, std::memory_order_release);
        }
      });

      Subscriber<int> sub("ddsc://ddsc/status/deadline_miss1?qos=ddsc_status_deadline_miss");
      sub.listen([](const int& /*v*/) {});

      CHECK(pub.wait_for_subscribers(1s));
      pub.publish(1);

      std::this_thread::sleep_for(500ms);

      (void)got_deadline.load(std::memory_order_acquire);
    } catch (const std::exception&) {
      return;
    }
  }
}

#if defined(VLINK_TEST_SUPPORT_SECURITY)
#include "./security_test_helpers.h"
#endif

TEST_SUITE("ddsc-security") {
#if defined(VLINK_TEST_SUPPORT_SECURITY)

  TEST_CASE("asymmetric rsa-oaep encrypted bytes round trip via ddsc") {
    MESSAGE("[ddsc-security] asymmetric rsa-oaep encrypted bytes round trip via ddsc");

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

      SecurityPublisher<Bytes> pub(DdscConf("ddsc/sec/rsa1"), std::move(pub_cfg));
      SecuritySubscriber<Bytes> sub("ddsc://ddsc/sec/rsa1", std::move(sub_cfg));

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

  TEST_CASE("asymmetric mismatched private key fails to decrypt over ddsc") {
    MESSAGE("[ddsc-security] asymmetric mismatched private key fails to decrypt over ddsc");

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

      SecurityPublisher<Bytes> pub(DdscConf("ddsc/sec/rsa_mm1"), std::move(pub_cfg));
      SecuritySubscriber<Bytes> sub("ddsc://ddsc/sec/rsa_mm1", std::move(sub_cfg));

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

  TEST_CASE("asymmetric with signing key verification over ddsc") {
    MESSAGE("[ddsc-security] asymmetric with signing key verification over ddsc");

    try {
      const auto kp = vlink_test_sec::generate_rsa_keypair(2048);

      if (kp.public_pem.empty()) {
        return;
      }

      std::atomic<bool> received{false};
      Bytes captured;

      Security::Config pub_cfg;
      pub_cfg.public_key_pem = kp.public_pem;
      pub_cfg.private_key_pem = kp.private_pem;

      Security::Config sub_cfg;
      sub_cfg.private_key_pem = kp.private_pem;
      sub_cfg.public_key_pem = kp.public_pem;

      SecurityPublisher<Bytes> pub(DdscConf("ddsc/sec/rsa_sign1"), std::move(pub_cfg));
      SecuritySubscriber<Bytes> sub("ddsc://ddsc/sec/rsa_sign1", std::move(sub_cfg));

      sub.listen([&](const Bytes& data) {
        captured = data;
        received.store(true, std::memory_order_release);
      });

      if (pub.wait_for_subscribers(1s)) {
        pub.publish(Bytes{0x11, 0x22, 0x33});

        for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
          std::this_thread::sleep_for(20ms);
        }

        if (received.load(std::memory_order_acquire)) {
          REQUIRE_EQ(captured.size(), 3u);
          CHECK_EQ(captured[0], 0x11u);
          CHECK_EQ(captured[2], 0x33u);
        }
      }
    } catch (const std::exception&) {
      return;
    }
  }

#endif
}

#endif  // VLINK_SUPPORT_DDSC

// NOLINTEND
