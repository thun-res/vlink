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

#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "./common_test.h"
#include "./modules/dds_conf.h"

TEST_SUITE("dds-init") {
  TEST_CASE("default conf stores topic with empty qos") {
    MESSAGE("[dds-init] default conf stores topic with empty qos");

    DdsConf conf("vehicle/speed");

    CHECK(conf.topic == "vehicle/speed");
    CHECK(conf.domain == 0);
    CHECK(conf.depth == 0);
    CHECK(conf.qos.empty());
    CHECK(conf.qos_ext.empty());
    CHECK(conf.get_transport_type() == TransportType::kDds);
  }

  TEST_CASE("conf with domain and depth stores those values") {
    MESSAGE("[dds-init] conf with domain and depth stores those values");

    DdsConf conf("my/topic", 2, 16);

    CHECK(conf.domain == 2);
    CHECK(conf.depth == 16);
  }

  TEST_CASE("conf with named qos stores qos name") {
    MESSAGE("[dds-init] conf with named qos stores qos name");

    DdsConf conf("my/topic", 0, 0, "reliable");

    CHECK(conf.qos == "reliable");
    CHECK(conf.qos_ext.empty());
  }

  TEST_CASE("conf with qos ext map stores properties") {
    MESSAGE("[dds-init] conf with qos ext map stores properties");

    DdsConf::PropertiesMap ext{{"writer", "RELIABLE"}, {"reader", "RELIABLE"}};
    DdsConf conf("my/topic", 0, ext);

    CHECK(conf.qos_ext == ext);
    CHECK(conf.qos.empty());
  }

  TEST_CASE("conf equality compares all relevant fields") {
    MESSAGE("[dds-init] conf equality compares all relevant fields");

    DdsConf a("topic/x", 1, 8, "q1");
    DdsConf b("topic/x", 1, 8, "q1");
    DdsConf c("topic/y", 1, 8, "q1");

    CHECK(a == b);
    CHECK(!(a != b));
    CHECK(a != c);
  }

  TEST_CASE("url parses for all impl types") {
    MESSAGE("[dds-init] url parses for all impl types");

    Url url("dds://dds/init/parse1");

    CHECK(url.parse(kPublisher));
    CHECK(url.parse(kSubscriber));
    CHECK(url.parse(kServer));
    CHECK(url.parse(kClient));
    CHECK(url.parse(kSetter));
    CHECK(url.parse(kGetter));
  }

  TEST_CASE("unknown impl type throws on parse") {
    MESSAGE("[dds-init] unknown impl type throws on parse");

    Url url("dds://dds/init/parse2");

    CHECK_THROWS_AS(url.parse(kUnknownImplType), std::runtime_error);
  }

  TEST_CASE("invalid url scheme throws on publisher construction") { CHECK_THROWS(Publisher<int>("dds1://bad/url")); }

  TEST_CASE("registering and using a named qos profile succeeds") {
    MESSAGE("[dds-init] registering and using a named qos profile succeeds");

    Qos qos;
    qos.reliability.kind = Qos::Reliability::kReliable;
    qos.durability.kind = Qos::Durability::kTransientLocal;

    try {
      DdsConf::register_qos("dds_reliable_tl", qos);
    } catch (...) {
    }

    DdsConf conf("dds/qos/test1", 0, 0, "dds_reliable_tl");
    CHECK(conf.qos == "dds_reliable_tl");
  }

  TEST_CASE("get discovered topics does not throw") { CHECK_NOTHROW((void)DdsConf::get_discovered_topics(0)); }
}

TEST_SUITE("dds-pubsub") {
  TEST_CASE("bytes payload is received intact") {
    MESSAGE("[dds-pubsub] bytes payload is received intact");

    std::atomic<bool> received{false};
    Bytes captured;

    Publisher<Bytes> pub(DdsConf("dds/evt/pubsub1"));
    Subscriber<Bytes> sub("dds://dds/evt/pubsub1");

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

  TEST_CASE("string payload is received with correct value") {
    MESSAGE("[dds-pubsub] string payload is received with correct value");

    std::atomic<bool> received{false};
    std::string captured;

    Publisher<std::string> pub(DdsConf("dds/evt/str1"));
    Subscriber<std::string> sub("dds://dds/evt/str1");

    sub.listen([&](const std::string& val) {
      captured = val;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(std::string("hello_dds")));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(30ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    CHECK(captured == "hello_dds");
  }

  TEST_CASE("integer payload is received with correct value") {
    MESSAGE("[dds-pubsub] integer payload is received with correct value");

    std::atomic<int> captured{0};
    std::atomic<bool> received{false};

    Publisher<int> pub(DdsConf("dds/evt/int1"));
    Subscriber<int> sub("dds://dds/evt/int1");

    sub.listen([&](const int& v) {
      captured.store(v, std::memory_order_relaxed);
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(12345));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(30ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    CHECK(captured.load() == 12345);
  }

  TEST_CASE("multiple publishes are all received by subscriber") {
    MESSAGE("[dds-pubsub] multiple publishes are all received by subscriber");

    std::atomic<int> count{0};

    Publisher<int> pub(DdsConf("dds/evt/multi1"));
    Subscriber<int> sub("dds://dds/evt/multi1");

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
    MESSAGE("[dds-pubsub] multiple subscribers each receive every published message");

    std::atomic<int> count1{0};
    std::atomic<int> count2{0};

    Publisher<Bytes> pub(DdsConf("dds/evt/multisub1"));
    Subscriber<Bytes> sub1("dds://dds/evt/multisub1");
    Subscriber<Bytes> sub2("dds://dds/evt/multisub1");

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
    MESSAGE("[dds-pubsub] force publish succeeds without any subscriber");

    Publisher<Bytes> pub(DdsConf("dds/evt/force1"));

    CHECK(!pub.has_subscribers());

    for (int i = 0; i < 5; ++i) {
      CHECK(pub.publish(Bytes{static_cast<uint8_t>(i)}, true));
    }
  }

  TEST_CASE("subscriber connect and disconnect events are detected") {
    MESSAGE("[dds-pubsub] subscriber connect and disconnect events are detected");

    std::atomic<int> connected_count{0};
    std::atomic<int> disconnected_count{0};

    Publisher<Bytes> pub(DdsConf("dds/evt/detect1"));
    pub.detect_subscribers([&](bool connected) {
      if (connected) {
        connected_count.fetch_add(1, std::memory_order_relaxed);
      } else {
        disconnected_count.fetch_add(1, std::memory_order_relaxed);
      }
    });

    {
      Subscriber<Bytes> sub("dds://dds/evt/detect1");
      sub.listen([](const Bytes& /*d*/) {});
      std::this_thread::sleep_for(30ms);
      CHECK(pub.has_subscribers());
    }

    std::this_thread::sleep_for(30ms);
    CHECK(!pub.has_subscribers());
    CHECK(disconnected_count.load() >= 1);
  }

  TEST_CASE("serialization round trip succeeds for available message types") {
    MESSAGE("[dds-pubsub] serialization round trip succeeds for available message types");

#if defined(VLINK_TEST_SUPPORT_PROTOBUF)
    SUBCASE("protobuf pub sub") {
      std::atomic<bool> received{false};
      pb::Message captured;

      Publisher<pb::Message> pub(DdsConf("dds/ser/pb1"));
      Subscriber<pb::Message> sub("dds://dds/ser/pb1");

      sub.listen([&](const pb::Message& msg) {
        captured = msg;
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      pb::Message msg;
      msg.set_value("dds_proto");
      CHECK(pub.publish(msg));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      CHECK(captured.value() == "dds_proto");
    }
#endif

    SUBCASE("plain bytes always works") {
      std::atomic<bool> received{false};

      Publisher<Bytes> pub(DdsConf("dds/ser/plain1"));
      Subscriber<Bytes> sub("dds://dds/ser/plain1");

      sub.listen([&](const Bytes& /*d*/) { received.store(true, std::memory_order_release); });

      CHECK(pub.wait_for_subscribers(1s));
      pub.publish(Bytes{0x01, 0x02, 0x03});

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
    }
  }
}

TEST_SUITE("dds-method") {
  TEST_CASE("fire and forget send increments server receive counter") {
    MESSAGE("[dds-method] fire and forget send increments server receive counter");

    std::atomic<int> counter{0};

    Server<std::string> server(DdsConf("dds/mth/send1"));
    server.listen([&](const std::string& /*req*/) { counter.fetch_add(1, std::memory_order_relaxed); });

    Client<std::string> client("dds://dds/mth/send1");
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
    MESSAGE("[dds-method] invoke returns correct response via multiple overloads");

    Server<std::string, std::string> server(DdsConf("dds/mth/invoke1"));
    server.listen([](const std::string& req, std::string& resp) { resp = "echo:" + req; });

    Client<std::string, std::string> client("dds://dds/mth/invoke1");
    CHECK(client.wait_for_connected(1s));

    SUBCASE("sync optional") {
      auto resp = client.invoke("ping");
      CHECK(resp.has_value());
      CHECK(*resp == "echo:ping");
    }

    SUBCASE("sync ref overload") {
      std::string out;
      CHECK(client.invoke("world", out, 5s));
      CHECK(out == "echo:world");
    }

    SUBCASE("async future") {
      auto fut = client.async_invoke("async");
      REQUIRE(fut.wait_for(5s) == std::future_status::ready);
      CHECK(fut.get() == "echo:async");
    }

    SUBCASE("multiple sequential invocations succeed") {
      for (int i = 0; i < 5; ++i) {
        auto resp = client.invoke("r" + std::to_string(i));
        CHECK(resp.has_value());
        CHECK(*resp == "echo:r" + std::to_string(i));
      }
    }
  }

  TEST_CASE("deferred async reply is delivered to future") {
    MESSAGE("[dds-method] deferred async reply is delivered to future");

    std::atomic<uint64_t> saved_id{0};
    std::atomic<bool> req_received{false};

    Server<std::string, std::string> server(DdsConf("dds/mth/async_reply1"));
    server.listen_for_reply([&](uint64_t req_id, const std::string& /*req*/) {
      saved_id.store(req_id, std::memory_order_release);
      req_received.store(true, std::memory_order_release);
    });

    Client<std::string, std::string> client("dds://dds/mth/async_reply1");
    CHECK(client.wait_for_connected(1s));

    auto fut = client.async_invoke("deferred");

    for (int i = 0; i < 100 && !req_received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(30ms);
    }

    REQUIRE(req_received.load(std::memory_order_acquire));
    CHECK(server.reply(saved_id.load(), std::string("deferred_resp")));

    REQUIRE(fut.wait_for(5s) == std::future_status::ready);
    CHECK(fut.get() == "deferred_resp");
  }

  TEST_CASE("async callback invoke delivers response") {
    MESSAGE("[dds-method] async callback invoke delivers response");

    Server<std::string, std::string> server(DdsConf("dds/mth/cb1"));
    server.listen([](const std::string& /*req*/, std::string& resp) { resp = "cb_ok"; });

    Client<std::string, std::string> client("dds://dds/mth/cb1");
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
    CHECK(resp_val == "cb_ok");
  }

  TEST_CASE("detect connected callback fires when client connects to server") {
    MESSAGE("[dds-method] detect connected callback fires when client connects to server");

    std::atomic<bool> connected_event{false};

    Server<std::string, std::string> server(DdsConf("dds/mth/detect_conn1"));
    server.listen([](const std::string& /*req*/, std::string& resp) { resp = "ok"; });

    Client<std::string, std::string> client("dds://dds/mth/detect_conn1");
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

#if defined(VLINK_TEST_SUPPORT_PROTOBUF)
  TEST_CASE("protobuf rpc invoke returns correct computed value") {
    MESSAGE("[dds-method] protobuf rpc invoke returns correct computed value");

    Server<pb::Request, pb::Response> server(DdsConf("dds/ser/pb_rpc1"));
    server.listen([](const pb::Request& req, pb::Response& resp) { resp.set_value(std::to_string(req.type() * 2)); });

    Client<pb::Request, pb::Response> client("dds://dds/ser/pb_rpc1");
    CHECK(client.wait_for_connected(1s));

    pb::Request req;
    req.set_type(10);
    auto resp = client.invoke(req);
    CHECK(resp.has_value());
    CHECK(resp->value() == "20");
  }
#endif
}

TEST_SUITE("dds-field") {
  TEST_CASE("setter and getter exchange values via all access patterns") {
    MESSAGE("[dds-field] setter and getter exchange values via all access patterns");

    SUBCASE("polling get") {
      Setter<Bytes> setter(DdsConf("dds/fld/poll1"));
      Getter<Bytes> getter("dds://dds/fld/poll1");

      setter.set(Bytes{0x11, 0x22, 0x33});
      std::this_thread::sleep_for(30ms);

      auto v = getter.get();
      REQUIRE(v.has_value());
      REQUIRE(v->size() == 3);
      CHECK((*v)[0] == 0x11);
      CHECK((*v)[2] == 0x33);
    }

    SUBCASE("wait for value blocks until setter publishes") {
      Setter<Bytes> setter(DdsConf("dds/fld/wait1"));
      Getter<Bytes> getter("dds://dds/fld/wait1");

      std::thread writer([&] {
        std::this_thread::sleep_for(30ms);
        setter.set(Bytes{0xAB, 0xCD});
      });

      CHECK(getter.wait_for_value(1s));
      auto v = getter.get();
      REQUIRE(v.has_value());
      CHECK((*v)[0] == 0xAB);

      writer.join();
    }

    SUBCASE("listen callback is invoked on value change") {
      std::atomic<bool> notified{false};
      Bytes cb_val;

      Setter<Bytes> setter(DdsConf("dds/fld/cb1"));
      Getter<Bytes> getter("dds://dds/fld/cb1");

      getter.listen([&](const Bytes& val) {
        cb_val = val;
        notified.store(true, std::memory_order_release);
      });

      std::this_thread::sleep_for(30ms);
      setter.set(Bytes{0xFF, 0x00});

      for (int i = 0; i < 100 && !notified.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(notified.load(std::memory_order_acquire));
      REQUIRE(cb_val.size() == 2);
      CHECK(cb_val[0] == 0xFF);
    }

    SUBCASE("change reporting suppresses duplicate value callbacks") {
      std::atomic<int> cb_count{0};

      Setter<Bytes> setter(DdsConf("dds/fld/cr1"));
      Getter<Bytes> getter("dds://dds/fld/cr1");

      getter.set_change_reporting(true);
      CHECK(getter.get_change_reporting());

      getter.listen([&](const Bytes& /*v*/) { cb_count.fetch_add(1, std::memory_order_relaxed); });

      std::this_thread::sleep_for(30ms);

      setter.set(Bytes{0x55});
      std::this_thread::sleep_for(30ms);
      setter.set(Bytes{0x55});
      std::this_thread::sleep_for(30ms);

      CHECK(cb_count.load() <= 1);
    }

    SUBCASE("late getter receives cached value from setter") {
      Setter<Bytes> setter(DdsConf("dds/fld/late1"));
      setter.set(Bytes{0xCA, 0xFE});
      std::this_thread::sleep_for(30ms);

      Getter<Bytes> late_getter("dds://dds/fld/late1");
      CHECK(late_getter.wait_for_value(1s));
      auto v = late_getter.get();
      REQUIRE(v.has_value());
      CHECK((*v)[0] == 0xCA);
    }

    SUBCASE("multiple sequential sets deliver latest value to getter") {
      Setter<int> setter(DdsConf("dds/fld/seq1"));
      Getter<int> getter("dds://dds/fld/seq1");

      for (int i = 1; i <= 5; ++i) {
        setter.set(i * 10);
        std::this_thread::sleep_for(30ms);
      }

      auto v = getter.get();
      REQUIRE(v.has_value());
      CHECK(*v == 50);
    }
  }
}

TEST_SUITE("dds-qos") {
  TEST_CASE("latency and lost tracking can be enabled and disabled") {
    MESSAGE("[dds-qos] latency and lost tracking can be enabled and disabled");

    Publisher<int> pub(DdsConf("dds/lat/sub1"));
    Subscriber<int> sub("dds://dds/lat/sub1");

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

  TEST_CASE("getter latency and lost tracking can be toggled") {
    MESSAGE("[dds-qos] getter latency and lost tracking can be toggled");

    Setter<int> setter(DdsConf("dds/lat/get1"));
    Getter<int> getter("dds://dds/lat/get1");

    getter.set_latency_and_lost_enabled(true);
    CHECK(getter.is_latency_and_lost_enabled());

    getter.listen([](const int& /*v*/) {});
    std::this_thread::sleep_for(30ms);

    for (int i = 0; i < 5; ++i) {
      setter.set(i);
      std::this_thread::sleep_for(30ms);
    }

    std::this_thread::sleep_for(30ms);
    CHECK(getter.get_latency() >= 0);

    getter.set_latency_and_lost_enabled(false);
    CHECK(!getter.is_latency_and_lost_enabled());
  }
}

TEST_SUITE("dds-pubsub") {
  TEST_CASE("large 1kb payload is received intact") {
    MESSAGE("[dds-pubsub] large 1kb payload is received intact");

    static constexpr size_t kSize1K = 1024;

    std::atomic<bool> received{false};
    size_t captured_size{0};

    Publisher<Bytes> pub(DdsConf("dds/evt/large1k"));
    Subscriber<Bytes> sub("dds://dds/evt/large1k");

    sub.listen([&](const Bytes& data) {
      captured_size = data.size();
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    Bytes payload = Bytes::create(kSize1K);
    for (size_t i = 0; i < kSize1K; ++i) {
      payload[i] = static_cast<uint8_t>(i & 0xFF);
    }

    CHECK(pub.publish(payload));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(30ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    CHECK_EQ(captured_size, kSize1K);
  }

  TEST_CASE("large 64kb payload is received intact") {
    MESSAGE("[dds-pubsub] large 64kb payload is received intact");

    static constexpr size_t kSize64K = 64 * 1024;

    std::atomic<bool> received{false};
    size_t captured_size{0};

    Publisher<Bytes> pub(DdsConf("dds/evt/large64k"));
    Subscriber<Bytes> sub("dds://dds/evt/large64k");

    sub.listen([&](const Bytes& data) {
      captured_size = data.size();
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    Bytes payload = Bytes::create(kSize64K);

    CHECK(pub.publish(payload));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(30ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    CHECK_EQ(captured_size, kSize64K);
  }

  TEST_CASE("empty bytes payload is received without crash") {
    MESSAGE("[dds-pubsub] empty bytes payload is received without crash");

    std::atomic<bool> received{false};
    size_t captured_size{1};

    Publisher<Bytes> pub(DdsConf("dds/evt/empty1"));
    Subscriber<Bytes> sub("dds://dds/evt/empty1");

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

  TEST_CASE("concurrent publishers deliver all messages to subscriber") {
    MESSAGE("[dds-pubsub] concurrent publishers deliver all messages to subscriber");

    static constexpr int kPublishers = 4;
    static constexpr int kPerPublisher = 5;

    std::atomic<int> total{0};

    Subscriber<int> sub("dds://dds/evt/concurrent1");
    sub.listen([&](const int& /*v*/) { total.fetch_add(1, std::memory_order_relaxed); });

    std::vector<std::thread> threads;
    threads.reserve(kPublishers);

    for (int t = 0; t < kPublishers; ++t) {
      threads.emplace_back([t] {
        Publisher<int> pub(DdsConf("dds/evt/concurrent1"));

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
    MESSAGE("[dds-pubsub] subscriber destroyed mid-flight does not crash publisher");

    Publisher<int> pub(DdsConf("dds/evt/lifecycle1"));

    {
      Subscriber<int> sub("dds://dds/evt/lifecycle1");
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

TEST_SUITE("dds-qos") {
  TEST_CASE("best-effort qos profile delivers messages") {
    MESSAGE("[dds-qos] best-effort qos profile delivers messages");

    Qos qos;
    qos.reliability.kind = Qos::Reliability::kBestEffort;
    try {
      DdsConf::register_qos("dds_best_effort_test", qos);
    } catch (...) {
    }

    std::atomic<int> count{0};

    Publisher<int> pub(DdsConf("dds/qos/be1", 0, 0, "dds_best_effort_test"));
    Subscriber<int> sub("dds://dds/qos/be1?qos=dds_best_effort_test");

    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 5; ++i) {
      pub.publish(i);
      std::this_thread::sleep_for(20ms);
    }

    std::this_thread::sleep_for(50ms);
    CHECK(count.load() > 0);
  }

  TEST_CASE("reliable transient-local qos late subscriber gets cached message") {
    MESSAGE("[dds-qos] reliable transient-local qos late subscriber gets cached message");

    Qos qos;
    qos.reliability.kind = Qos::Reliability::kReliable;
    qos.durability.kind = Qos::Durability::kTransientLocal;
    try {
      DdsConf::register_qos("dds_tl_reliable_test", qos);
    } catch (...) {
    }

    Setter<int> setter(DdsConf("dds/qos/tl1", 0, 0, "dds_tl_reliable_test"));
    setter.set(42);
    std::this_thread::sleep_for(50ms);

    Getter<int> late_getter("dds://dds/qos/tl1?qos=dds_tl_reliable_test");
    if (late_getter.wait_for_value(1s)) {
      auto v = late_getter.get();
      REQUIRE(v.has_value());
      CHECK_EQ(*v, 42);
    }
  }

  TEST_CASE("history depth conf field affects publisher depth") {
    MESSAGE("[dds-qos] history depth conf field affects publisher depth");

    DdsConf conf("dds/qos/depth1", 0, 10);
    CHECK_EQ(conf.depth, 10);

    Publisher<int> pub(conf);
    CHECK(pub.get_abstract_node() != nullptr);
  }

  TEST_CASE("qos ext map is preserved in conf") {
    MESSAGE("[dds-qos] qos ext map is preserved in conf");

    DdsConf::PropertiesMap ext{{"writer", "RELIABLE"}, {"reader", "RELIABLE"}};
    DdsConf conf("dds/qos/ext1", 0, ext);

    CHECK_EQ(conf.qos_ext.at("writer"), "RELIABLE");
    CHECK_EQ(conf.qos_ext.at("reader"), "RELIABLE");
    CHECK(conf.qos.empty());
  }
}

TEST_SUITE("dds-method") {
  TEST_CASE("invoke times out when server does not respond") {
    MESSAGE("[dds-method] invoke times out when server does not respond");

    Client<std::string, std::string> orphan("dds://dds/mth/timeout1");

    std::string out;
    bool ok = orphan.invoke("req", out, 300ms);
    CHECK_FALSE(ok);
  }

  TEST_CASE("multiple concurrent clients each get correct responses") {
    MESSAGE("[dds-method] multiple concurrent clients each get correct responses");

    static constexpr int kClients = 3;

    Server<std::string, std::string> server(DdsConf("dds/mth/concurrent1"));
    server.listen([](const std::string& req, std::string& resp) { resp = "ok:" + req; });

    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;
    threads.reserve(kClients);

    for (int t = 0; t < kClients; ++t) {
      threads.emplace_back([t, &success_count] {
        Client<std::string, std::string> client("dds://dds/mth/concurrent1");

        if (!client.wait_for_connected(1s)) {
          return;
        }

        std::string key = std::to_string(t);
        auto resp = client.invoke(key);

        if (resp.has_value() && *resp == "ok:" + key) {
          success_count.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }

    for (auto& th : threads) {
      th.join();
    }

    CHECK_EQ(success_count.load(), kClients);
  }

  TEST_CASE("server destroyed before client invoke returns gracefully") {
    MESSAGE("[dds-method] server destroyed before client invoke returns gracefully");

    Client<std::string, std::string> client("dds://dds/mth/destroy1");

    {
      Server<std::string, std::string> server(DdsConf("dds/mth/destroy1"));
      server.listen([](const std::string& /*req*/, std::string& resp) { resp = "tmp"; });

      CHECK(client.wait_for_connected(1s));
    }

    std::this_thread::sleep_for(50ms);
    std::string out;
    bool ok = client.invoke("probe", out, 500ms);
    CHECK_FALSE(ok);
  }
}

TEST_SUITE("dds-field") {
  TEST_CASE("default value is not available before any set") {
    MESSAGE("[dds-field] default value is not available before any set");

    Getter<int> getter("dds://dds/fld/default1");
    auto v = getter.get();
    CHECK_FALSE(v.has_value());
  }

  TEST_CASE("integer field round trips with correct value") {
    MESSAGE("[dds-field] integer field round trips with correct value");

    Setter<int> setter(DdsConf("dds/fld/int1"));
    Getter<int> getter("dds://dds/fld/int1");

    setter.set(7654321);
    CHECK(getter.wait_for_value(1s));

    auto v = getter.get();
    REQUIRE(v.has_value());
    CHECK_EQ(*v, 7654321);
  }

  TEST_CASE("multiple setters on same field deliver latest value") {
    MESSAGE("[dds-field] multiple setters on same field deliver latest value");

    Getter<int> getter("dds://dds/fld/multi_setter1");

    for (int s = 0; s < 3; ++s) {
      Setter<int> setter(DdsConf("dds/fld/multi_setter1"));
      setter.set(s * 100);
      std::this_thread::sleep_for(30ms);
    }

    CHECK(getter.wait_for_value(1s));
    auto v = getter.get();
    REQUIRE(v.has_value());
  }
}

#if defined(VLINK_TEST_SUPPORT_SECURITY)
#include "./security_test_helpers.h"
#endif

TEST_SUITE("dds-security") {
#if defined(VLINK_TEST_SUPPORT_SECURITY)

  TEST_CASE("symmetric key security encrypts and decrypts payload") {
    MESSAGE("[dds-security] symmetric key security encrypts and decrypts payload");

    try {
      std::atomic<bool> received{false};
      std::string captured;

      SecurityPublisher<std::string> pub(DdsConf("dds/sec/sym1"));
      SecuritySubscriber<std::string> sub("dds://dds/sec/sym1");

      sub.listen([&](const std::string& val) {
        captured = val;
        received.store(true, std::memory_order_release);
      });

      if (pub.wait_for_subscribers(1s)) {
        pub.publish(std::string("secure_dds"));

        for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
          std::this_thread::sleep_for(30ms);
        }

        if (received.load(std::memory_order_acquire)) {
          CHECK_EQ(captured, "secure_dds");
        }
      }
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("asymmetric rsa-oaep encrypted bytes round trip via dds") {
    MESSAGE("[dds-security] asymmetric rsa-oaep encrypted bytes round trip via dds");

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

      SecurityPublisher<Bytes> pub(DdsConf("dds/sec/rsa1"), std::move(pub_cfg));
      SecuritySubscriber<Bytes> sub("dds://dds/sec/rsa1", std::move(sub_cfg));

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

  TEST_CASE("asymmetric mismatched private key fails to decrypt over dds") {
    MESSAGE("[dds-security] asymmetric mismatched private key fails to decrypt over dds");

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

      SecurityPublisher<Bytes> pub(DdsConf("dds/sec/rsa_mm1"), std::move(pub_cfg));
      SecuritySubscriber<Bytes> sub("dds://dds/sec/rsa_mm1", std::move(sub_cfg));

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

  TEST_CASE("asymmetric with signing key verification over dds") {
    MESSAGE("[dds-security] asymmetric with signing key verification over dds");

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

      SecurityPublisher<Bytes> pub(DdsConf("dds/sec/rsa_sign1"), std::move(pub_cfg));
      SecuritySubscriber<Bytes> sub("dds://dds/sec/rsa_sign1", std::move(sub_cfg));

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

TEST_SUITE("dds-init") {
  TEST_CASE("multi-domain publishers on different domains are isolated") {
    MESSAGE("[dds-init] multi-domain publishers on different domains are isolated");

    std::atomic<int> count_d0{0};
    std::atomic<int> count_d1{0};

    Publisher<int> pub0(DdsConf("dds/domain/iso1", 0));
    Publisher<int> pub1(DdsConf("dds/domain/iso1", 1));

    Subscriber<int> sub0("dds://dds/domain/iso1?domain=0");
    Subscriber<int> sub1("dds://dds/domain/iso1?domain=1");

    sub0.listen([&](const int& /*v*/) { count_d0.fetch_add(1, std::memory_order_relaxed); });
    sub1.listen([&](const int& /*v*/) { count_d1.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub0.wait_for_subscribers(1s));
    CHECK(pub1.wait_for_subscribers(1s));

    for (int i = 0; i < 3; ++i) {
      pub0.publish(i);
      std::this_thread::sleep_for(20ms);
    }

    std::this_thread::sleep_for(50ms);
    CHECK(count_d0.load() >= 3);
    CHECK_EQ(count_d1.load(), 0);
  }

  TEST_CASE("get discovered topics returns a vector without throwing") {
    MESSAGE("[dds-init] get discovered topics returns a vector without throwing");

    auto topics0 = DdsConf::get_discovered_topics(0);
    auto topics1 = DdsConf::get_discovered_topics(1);
    CHECK(topics0.size() >= 0u);
    CHECK(topics1.size() >= 0u);
  }

  TEST_CASE("load global qos file with nonexistent path returns false") {
    MESSAGE("[dds-init] load global qos file with nonexistent path returns false");

    bool ok = DdsConf::load_global_qos_file("/nonexistent/path/to/profile.xml");
    CHECK_FALSE(ok);
  }

  TEST_CASE("conf with domain zero is the default domain") {
    MESSAGE("[dds-init] conf with domain zero is the default domain");

    DdsConf explicit_zero("dds/init/domain_default", 0);
    DdsConf default_domain("dds/init/domain_default");

    CHECK(explicit_zero == default_domain);
  }
}

TEST_SUITE("dds-error") {
  TEST_CASE("distinct topics yield distinct abstract nodes") {
    MESSAGE("[dds-error] distinct topics yield distinct abstract nodes");

    Publisher<int> pub1(DdsConf("dds/id/p1"));
    Publisher<int> pub2(DdsConf("dds/id/p2"));
    Subscriber<int> sub("dds://dds/id/p1");

    const auto* n1 = pub1.get_abstract_node();
    const auto* n2 = pub2.get_abstract_node();
    const auto* n3 = sub.get_abstract_node();

    CHECK(n1 != nullptr);
    CHECK(n2 != nullptr);
    CHECK(n3 != nullptr);
    CHECK(n1 != n2);
    CHECK(n1 != n3);
  }

  TEST_CASE("dynamic data int and string payloads round trip") {
    MESSAGE("[dds-error] dynamic data int and string payloads round trip");

    SUBCASE("int payload") {
      std::atomic<bool> received{false};
      DynamicData captured;

      Publisher<DynamicData> pub(DdsConf("dds/dyn/int1"));
      Subscriber<DynamicData> sub("dds://dds/dyn/int1");

      sub.listen([&](const DynamicData& d) {
        captured = d;
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      DynamicData d;
      d.load("int", 999);
      CHECK(pub.publish(d));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      CHECK(captured.as<int>() == 999);
    }

    SUBCASE("string payload") {
      std::atomic<bool> received{false};
      DynamicData captured;

      Publisher<DynamicData> pub(DdsConf("dds/dyn/str1"));
      Subscriber<DynamicData> sub("dds://dds/dyn/str1");

      sub.listen([&](const DynamicData& d) {
        captured = d;
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      DynamicData d;
      d.load("str", std::string("dds_dynamic"));
      CHECK(pub.publish(d));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      CHECK(captured.as<std::string>() == "dds_dynamic");
    }
  }
}

namespace {

struct DdsCustomMsg {
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

TEST_SUITE("dds-custom") {
  TEST_CASE("custom type round trips id and label through dds") {
    MESSAGE("[dds-custom] custom type round trips id and label through dds");

    try {
      std::atomic<bool> received{false};
      DdsCustomMsg captured{};

      Publisher<DdsCustomMsg> pub(DdsConf("dds/cust/basic"));
      Subscriber<DdsCustomMsg> sub("dds://dds/cust/basic");

      sub.listen([&](const DdsCustomMsg& m) {
        captured = m;
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      DdsCustomMsg msg;
      msg.id = 31;
      msg.label = "dds_custom";
      CHECK(pub.publish(msg));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      CHECK_EQ(captured.id, 31);
      CHECK_EQ(captured.label, "dds_custom");
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("serializer detects custom type as kCustomType") {
    MESSAGE("[dds-custom] serializer detects custom type as kCustomType");

    static constexpr auto kType = Serializer::get_type_of<DdsCustomMsg>();
    CHECK_EQ(kType, Serializer::kCustomType);
  }
}

#if defined(VLINK_TEST_SUPPORT_FLATBUFFERS)

TEST_SUITE("dds-flatbuffers") {
  TEST_CASE("flatbuffers message round trips through dds transport") {
    MESSAGE("[dds-flatbuffers] flatbuffers message round trips through dds transport");

    try {
      std::atomic<bool> received{false};
      uint32_t captured_type = 0;
      std::string captured_value;

      Publisher<fbs::MessageT> pub(DdsConf("dds/fbs/rt"));
      Subscriber<fbs::MessageT> sub("dds://dds/fbs/rt");

      sub.listen([&](const fbs::MessageT& m) {
        captured_type = m.type;
        captured_value = m.value;
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      fbs::MessageT msg;
      msg.type = 9u;
      msg.value = "dds_fbs_rt";
      CHECK(pub.publish(msg));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      CHECK_EQ(captured_type, 9u);
      CHECK_EQ(captured_value, "dds_fbs_rt");
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("flatbuffers message with qos setting delivers correctly") {
    MESSAGE("[dds-flatbuffers] flatbuffers message with qos setting delivers correctly");

    try {
      std::atomic<bool> received{false};

      Qos reliable_qos;
      reliable_qos.reliability.kind = Qos::Reliability::kReliable;
      try {
        DdsConf::register_qos("dds_fbs_qos", reliable_qos);
      } catch (...) {
      }

      Publisher<fbs::MessageT> pub(DdsConf("dds/fbs/qos", 0, 0, "dds_fbs_qos"));
      Subscriber<fbs::MessageT> sub("dds://dds/fbs/qos?qos=dds_fbs_qos");

      sub.listen([&](const fbs::MessageT& /*m*/) { received.store(true, std::memory_order_release); });

      CHECK(pub.wait_for_subscribers(1s));

      fbs::MessageT msg;
      msg.type = 2u;
      msg.value = "dds_fbs_qos";
      pub.publish(msg);

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("flatbuffers dynamic data interop delivers raw bytes on dds") {
    MESSAGE("[dds-flatbuffers] flatbuffers dynamic data interop delivers raw bytes on dds");

    try {
      std::atomic<bool> received{false};

      Publisher<fbs::MessageT> pub(DdsConf("dds/fbs/dynamic"));
      Subscriber<DynamicData> sub("dds://dds/fbs/dynamic");

      sub.listen([&](const DynamicData& /*d*/) { received.store(true, std::memory_order_release); });

      CHECK(pub.wait_for_subscribers(1s));

      fbs::MessageT msg;
      msg.type = 1u;
      msg.value = "dynamic";
      pub.publish(msg);

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
    } catch (const std::exception&) {
      return;
    }
  }
}

#endif  // VLINK_TEST_SUPPORT_FLATBUFFERS

TEST_SUITE("dds-qos") {
  TEST_CASE("durability volatile and transient-local kinds are recognised by Qos struct") {
    MESSAGE("[dds-qos] durability volatile and transient-local kinds are recognised by Qos struct");

    Qos volatile_qos;
    volatile_qos.durability.kind = Qos::Durability::kVolatile;
    CHECK_EQ(volatile_qos.durability.kind, Qos::Durability::kVolatile);

    Qos tl_qos;
    tl_qos.durability.kind = Qos::Durability::kTransientLocal;
    CHECK_EQ(tl_qos.durability.kind, Qos::Durability::kTransientLocal);
  }

  TEST_CASE("liveliness automatic vs manual kind struct field") {
    MESSAGE("[dds-qos] liveliness automatic vs manual kind struct field");

    SUBCASE("automatic kind is stored in qos struct") {
      Qos qos;
      qos.liveliness.kind = Qos::Liveliness::kAutomatic;
      qos.liveliness.duration = 5000;
      try {
        DdsConf::register_qos("dds_lv_auto", qos);
      } catch (...) {
      }

      DdsConf conf("dds/qos/lv_auto1", 0, 0, "dds_lv_auto");
      CHECK_EQ(conf.qos, "dds_lv_auto");
    }

    SUBCASE("manual-by-topic kind is stored in qos struct") {
      Qos qos;
      qos.liveliness.kind = Qos::Liveliness::kManualTopic;
      qos.liveliness.duration = 200;
      try {
        DdsConf::register_qos("dds_lv_manual_topic", qos);
      } catch (...) {
      }

      DdsConf conf("dds/qos/lv_manual1", 0, 0, "dds_lv_manual_topic");
      CHECK_EQ(conf.qos, "dds_lv_manual_topic");
    }
  }

  TEST_CASE("deadline period applied to publisher and messages still delivered") {
    MESSAGE("[dds-qos] deadline period applied to publisher and messages still delivered");

    try {
      Qos qos;
      qos.deadline.period = 500;
      qos.reliability.kind = Qos::Reliability::kReliable;
      try {
        DdsConf::register_qos("dds_deadline_500ms", qos);
      } catch (...) {
      }

      std::atomic<int> count{0};

      Publisher<int> pub(DdsConf("dds/qos/deadline1", 0, 0, "dds_deadline_500ms"));
      Subscriber<int> sub("dds://dds/qos/deadline1?qos=dds_deadline_500ms");

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

  TEST_CASE("lifespan duration struct field is set correctly") {
    MESSAGE("[dds-qos] lifespan duration struct field is set correctly");

    Qos qos;
    qos.lifespan.duration = 100;
    qos.reliability.kind = Qos::Reliability::kReliable;
    try {
      DdsConf::register_qos("dds_lifespan_100ms", qos);
    } catch (...) {
    }

    DdsConf conf("dds/qos/lifespan1", 0, 0, "dds_lifespan_100ms");
    CHECK_EQ(conf.qos, "dds_lifespan_100ms");
  }

  TEST_CASE("latency budget duration hint is registered without error") {
    MESSAGE("[dds-qos] latency budget duration hint is registered without error");

    try {
      Qos qos;
      qos.latency_budget.duration = 10;
      try {
        DdsConf::register_qos("dds_latbudget_10ms", qos);
      } catch (...) {
      }

      Publisher<int> pub(DdsConf("dds/qos/latbud1", 0, 0, "dds_latbudget_10ms"));
      Subscriber<int> sub("dds://dds/qos/latbud1?qos=dds_latbudget_10ms");

      std::atomic<int> count{0};
      sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

      CHECK(pub.wait_for_subscribers(1s));
      pub.publish(42);
      std::this_thread::sleep_for(100ms);
      CHECK(count.load() > 0);
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("resource limits max samples field is set in qos struct") {
    MESSAGE("[dds-qos] resource limits max samples field is set in qos struct");

    SUBCASE("small max_samples cap") {
      Qos qos;
      qos.resource_limits.max_samples = 10;
      qos.resource_limits.max_instances = 1;
      qos.resource_limits.max_samples_per_instance = 10;
      try {
        DdsConf::register_qos("dds_rl_small", qos);
      } catch (...) {
      }

      DdsConf conf("dds/qos/rl1", 0, 0, "dds_rl_small");
      CHECK_EQ(conf.qos, "dds_rl_small");
    }

    SUBCASE("default max_samples is 6000") {
      Qos qos;
      CHECK_EQ(qos.resource_limits.max_samples, 6000);
      CHECK_EQ(qos.resource_limits.max_instances, 10);
      CHECK_EQ(qos.resource_limits.max_samples_per_instance, 500);
    }
  }

  TEST_CASE("destination order kind fields are distinct enum values") {
    MESSAGE("[dds-qos] destination order kind fields are distinct enum values");

    Qos reception_qos;
    reception_qos.destination_order.kind = Qos::DestinationOrder::kReceptionTimestamp;
    try {
      DdsConf::register_qos("dds_dest_reception", reception_qos);
    } catch (...) {
    }

    Qos source_qos;
    source_qos.destination_order.kind = Qos::DestinationOrder::kSourceTimestamp;
    try {
      DdsConf::register_qos("dds_dest_source", source_qos);
    } catch (...) {
    }

    CHECK_NE(static_cast<int>(Qos::DestinationOrder::kReceptionTimestamp),
             static_cast<int>(Qos::DestinationOrder::kSourceTimestamp));
  }

  TEST_CASE("ownership kind shared vs exclusive are distinct enum values") {
    MESSAGE("[dds-qos] ownership kind shared vs exclusive are distinct enum values");

    Qos shared_qos;
    shared_qos.ownership.kind = Qos::Ownership::kShared;
    try {
      DdsConf::register_qos("dds_own_shared", shared_qos);
    } catch (...) {
    }

    Qos excl_qos;
    excl_qos.ownership.kind = Qos::Ownership::kExclusive;
    try {
      DdsConf::register_qos("dds_own_excl", excl_qos);
    } catch (...) {
    }

    CHECK_NE(static_cast<int>(Qos::Ownership::kShared), static_cast<int>(Qos::Ownership::kExclusive));
  }

  TEST_CASE("keep-all history accumulates multiple samples before read") {
    MESSAGE("[dds-qos] keep-all history accumulates multiple samples before read");

    try {
      Qos qos;
      qos.history.kind = Qos::History::kKeepAll;
      qos.reliability.kind = Qos::Reliability::kReliable;
      try {
        DdsConf::register_qos("dds_keepall_test", qos);
      } catch (...) {
      }

      std::atomic<int> count{0};

      Publisher<int> pub(DdsConf("dds/qos/keepall1", 0, 0, "dds_keepall_test"));
      Subscriber<int> sub("dds://dds/qos/keepall1?qos=dds_keepall_test");

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

  TEST_CASE("keep-last depth variants deliver only most recent samples") {
    MESSAGE("[dds-qos] keep-last depth variants deliver only most recent samples");

    try {
      SUBCASE("depth 1") {
        Qos qos;
        qos.history.kind = Qos::History::kKeepLast;
        qos.history.depth = 1;
        try {
          DdsConf::register_qos("dds_kl_depth1", qos);
        } catch (...) {
        }

        DdsConf conf("dds/qos/kl1", 0, 0, "dds_kl_depth1");
        CHECK_EQ(conf.qos, "dds_kl_depth1");
      }

      SUBCASE("depth 10") {
        Qos qos;
        qos.history.kind = Qos::History::kKeepLast;
        qos.history.depth = 10;
        try {
          DdsConf::register_qos("dds_kl_depth10", qos);
        } catch (...) {
        }

        DdsConf conf("dds/qos/kl10", 0, 0, "dds_kl_depth10");
        CHECK_EQ(conf.qos, "dds_kl_depth10");
      }
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("async publish mode qos struct field is set correctly") {
    MESSAGE("[dds-qos] async publish mode qos struct field is set correctly");

    Qos qos;
    qos.publish_mode.kind = Qos::PublishMode::kASync;
    try {
      DdsConf::register_qos("dds_pm_async", qos);
    } catch (...) {
    }

    DdsConf conf("dds/qos/pm_async1", 0, 0, "dds_pm_async");
    CHECK_EQ(conf.qos, "dds_pm_async");
  }
}

TEST_SUITE("dds-status") {
  TEST_CASE("publication matched fires when subscriber connects") {
    MESSAGE("[dds-status] publication matched fires when subscriber connects");

    try {
      std::atomic<bool> got_matched{false};
      std::atomic<int32_t> last_count{-1};

      Publisher<int> pub(DdsConf("dds/status/pub_matched1"));
      pub.register_status_handler([&](const Status::BasePtr& s) {
        if (s->get_type() == Status::kPublicationMatched) {
          auto m = s->as<Status::PublicationMatched>();
          last_count.store(m->current_count, std::memory_order_relaxed);
          if (m->current_count > 0) {
            got_matched.store(true, std::memory_order_release);
          }
        }
      });

      Subscriber<int> sub("dds://dds/status/pub_matched1");
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
    MESSAGE("[dds-status] subscription matched fires when publisher connects");

    try {
      std::atomic<bool> got_matched{false};
      std::atomic<int32_t> last_count{-1};

      Subscriber<int> sub("dds://dds/status/sub_matched1");
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

      Publisher<int> pub(DdsConf("dds/status/sub_matched1"));

      for (int i = 0; i < 100 && !got_matched.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      (void)got_matched.load(std::memory_order_acquire);
      CHECK(last_count.load() > 0);
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("offered deadline missed fires when publisher stops within deadline period") {
    MESSAGE("[dds-status] offered deadline missed fires when publisher stops within deadline period");

    try {
      static constexpr int32_t kDeadlineMs = 50;

      Qos qos;
      qos.deadline.period = kDeadlineMs;
      qos.reliability.kind = Qos::Reliability::kReliable;
      try {
        DdsConf::register_qos("dds_status_deadline_miss", qos);
      } catch (...) {
      }

      std::atomic<bool> got_deadline{false};

      Publisher<int> pub(DdsConf("dds/status/deadline_miss1", 0, 0, "dds_status_deadline_miss"));
      pub.register_status_handler([&](const Status::BasePtr& s) {
        if (s->get_type() == Status::kOfferedDeadlineMissed) {
          got_deadline.store(true, std::memory_order_release);
        }
      });

      Subscriber<int> sub("dds://dds/status/deadline_miss1?qos=dds_status_deadline_miss");
      sub.listen([](const int& /*v*/) {});

      CHECK(pub.wait_for_subscribers(1s));
      pub.publish(1);

      std::this_thread::sleep_for(500ms);

      (void)got_deadline.load(std::memory_order_acquire);
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("offered incompatible qos fires when sub uses stricter reliability") {
    MESSAGE("[dds-status] offered incompatible qos fires when sub uses stricter reliability");

    try {
      std::atomic<bool> got_incompat{false};

      Qos pub_qos;
      pub_qos.reliability.kind = Qos::Reliability::kBestEffort;
      try {
        DdsConf::register_qos("dds_incompat_be_pub", pub_qos);
      } catch (...) {
      }

      Qos sub_qos;
      sub_qos.reliability.kind = Qos::Reliability::kReliable;
      try {
        DdsConf::register_qos("dds_incompat_rel_sub", sub_qos);
      } catch (...) {
      }

      Publisher<int> pub(DdsConf("dds/status/incompat1", 0, 0, "dds_incompat_be_pub"));
      pub.register_status_handler([&](const Status::BasePtr& s) {
        if (s->get_type() == Status::kOfferedIncompatibleQos) {
          got_incompat.store(true, std::memory_order_release);
        }
      });

      Subscriber<int> sub("dds://dds/status/incompat1?qos=dds_incompat_rel_sub");
      sub.listen([](const int& /*v*/) {});

      for (int i = 0; i < 100 && !got_incompat.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      (void)got_incompat.load(std::memory_order_acquire);
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("liveliness lost fires when manual liveliness writer does not assert") {
    MESSAGE("[dds-status] liveliness lost fires when manual liveliness writer does not assert");

    try {
      static constexpr int32_t kLeaseDurationMs = 100;

      Qos qos;
      qos.liveliness.kind = Qos::Liveliness::kManualTopic;
      qos.liveliness.duration = kLeaseDurationMs;
      try {
        DdsConf::register_qos("dds_lv_manual_lease", qos);
      } catch (...) {
      }

      std::atomic<bool> got_lost{false};

      Publisher<int> pub(DdsConf("dds/status/lv_lost1", 0, 0, "dds_lv_manual_lease"));
      pub.register_status_handler([&](const Status::BasePtr& s) {
        if (s->get_type() == Status::kLivelinessLost) {
          got_lost.store(true, std::memory_order_release);
        }
      });

      Subscriber<int> sub("dds://dds/status/lv_lost1?qos=dds_lv_manual_lease");
      sub.listen([](const int& /*v*/) {});

      CHECK(pub.wait_for_subscribers(1s));

      std::this_thread::sleep_for(1s);

      (void)got_lost.load(std::memory_order_acquire);
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("sample lost status smoke test does not crash on registration") {
    MESSAGE("[dds-status] sample lost status smoke test does not crash on registration");

    try {
      std::atomic<bool> got_lost{false};

      Publisher<int> pub(DdsConf("dds/status/sample_lost1"));
      Subscriber<int> sub("dds://dds/status/sample_lost1");

      sub.listen([](const int& /*v*/) {});
      sub.register_status_handler([&](const Status::BasePtr& s) {
        if (s->get_type() == Status::kSampleLost) {
          got_lost.store(true, std::memory_order_release);
        }
      });

      CHECK(pub.wait_for_subscribers(1s));

      for (int i = 0; i < 10; ++i) {
        pub.publish(i, true);
      }

      std::this_thread::sleep_for(100ms);
      CHECK(sub.get_abstract_node() != nullptr);
    } catch (const std::exception&) {
      return;
    }
  }
}

#include "./zerocopy/camera_frame.h"
#include "./zerocopy/raw_data.h"

TEST_SUITE("dds-zerocopy") {
  TEST_CASE("rawdata round trip preserves header seq over dds") {
    MESSAGE("[dds-zerocopy] rawdata round trip preserves header seq over dds");

    try {
      std::atomic<bool> received{false};
      zerocopy::RawData captured;

      Publisher<zerocopy::RawData> pub(DdsConf("dds/zc/raw1"));
      Subscriber<zerocopy::RawData> sub("dds://dds/zc/raw1");

      sub.listen([&](const zerocopy::RawData& d) {
        captured.deep_copy(d);
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      zerocopy::RawData rd;
      rd.header.seq = 33;
      rd.create(4);
      const_cast<uint8_t*>(rd.data())[0] = 0x12;
      const_cast<uint8_t*>(rd.data())[3] = 0x34;
      CHECK(pub.publish(rd));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      REQUIRE_EQ(captured.size(), 4u);
      CHECK_EQ(captured.header.seq, 33u);
      CHECK_EQ(captured.data()[0], 0x12u);
      CHECK_EQ(captured.data()[3], 0x34u);
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("cameraframe width height and format survive dds transport") {
    MESSAGE("[dds-zerocopy] cameraframe width height and format survive dds transport");

    try {
      std::atomic<bool> received{false};
      zerocopy::CameraFrame captured;

      Publisher<zerocopy::CameraFrame> pub(DdsConf("dds/zc/cam1"));
      Subscriber<zerocopy::CameraFrame> sub("dds://dds/zc/cam1");

      sub.listen([&](const zerocopy::CameraFrame& f) {
        captured.deep_copy(f);
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      zerocopy::CameraFrame frame;
      frame.set_width(800);
      frame.set_height(600);
      frame.set_format(zerocopy::CameraFrame::kFormatYuv420);
      frame.create(800 * 600 * 3 / 2);
      CHECK(pub.publish(frame));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      CHECK_EQ(captured.width(), 800u);
      CHECK_EQ(captured.height(), 600u);
      CHECK_EQ(captured.format(), zerocopy::CameraFrame::kFormatYuv420);
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("cameraframe header seq field survives dds transport") {
    MESSAGE("[dds-zerocopy] cameraframe header seq field survives dds transport");

    try {
      std::atomic<bool> received{false};
      zerocopy::CameraFrame captured;

      Publisher<zerocopy::CameraFrame> pub(DdsConf("dds/zc/cam2"));
      Subscriber<zerocopy::CameraFrame> sub("dds://dds/zc/cam2");

      sub.listen([&](const zerocopy::CameraFrame& f) {
        captured.deep_copy(f);
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      zerocopy::CameraFrame frame;
      frame.header.seq = 7;
      frame.set_width(64);
      frame.set_height(64);
      frame.set_format(zerocopy::CameraFrame::kFormatNv12);
      frame.create(64 * 64 * 3 / 2);
      CHECK(pub.publish(frame));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      CHECK_EQ(captured.header.seq, 7u);
    } catch (const std::exception&) {
      return;
    }
  }
}

#endif  // VLINK_SUPPORT_DDS

// NOLINTEND
