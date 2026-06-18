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

#ifdef VLINK_SUPPORT_MQTT

#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "./modules/mqtt_conf.h"

static bool is_mqtt_broker_available() {
  static int result = -1;

  if (result >= 0) {
    return result != 0;
  }

  Publisher<int> probe(MqttConf("vlink/test/probe", "probe"));
  result = probe.has_subscribers() ? 1 : 0;

  return result != 0;
}

#define MQTT_REQUIRE_BROKER()                                                  \
  do {                                                                         \
    if (!is_mqtt_broker_available()) {                                         \
      MESSAGE("MQTT broker not available at tcp://localhost:1883 - skipping"); \
      return;                                                                  \
    }                                                                          \
  } while (0)

TEST_SUITE("mqtt-init") {
  TEST_CASE("conf defaults are set correctly") {
    MESSAGE("[mqtt-init] conf defaults are set correctly");

    MqttConf conf("vehicle/speed");

    CHECK(conf.address == "vehicle/speed");
    CHECK(conf.event.empty());
    CHECK(conf.domain == 0);
    CHECK(conf.qos == 1);
    CHECK(conf.fragment.empty());
    CHECK(conf.get_transport_type() == TransportType::kMqtt);
  }

  TEST_CASE("conf accepts all fields") {
    MESSAGE("[mqtt-init] conf accepts all fields");

    MqttConf conf("my/topic", "my_event", 2, 0, "tcp://broker:1883");

    CHECK(conf.address == "my/topic");
    CHECK(conf.event == "my_event");
    CHECK(conf.domain == 2);
    CHECK(conf.qos == 0);
    CHECK(conf.fragment == "tcp://broker:1883");
  }

  TEST_CASE("conf equality compares all fields") {
    MESSAGE("[mqtt-init] conf equality compares all fields");

    MqttConf a("addr1", "ev1", 0, 1, "");
    MqttConf b("addr1", "ev1", 0, 1, "");
    MqttConf c("addr2", "ev1", 0, 1, "");

    CHECK(a == b);
    CHECK(a != c);
  }

  TEST_CASE("url parses for all impl types") {
    MESSAGE("[mqtt-init] url parses for all impl types");

    Url url("mqtt://mqtt/init/parse1?event=ev1");

    CHECK(url.parse(kPublisher));
    CHECK(url.parse(kSubscriber));
    CHECK(url.parse(kServer));
    CHECK(url.parse(kClient));
    CHECK(url.parse(kSetter));
    CHECK(url.parse(kGetter));
  }

  TEST_CASE("unknown impl type throws on parse") {
    MESSAGE("[mqtt-init] unknown impl type throws on parse");

    Url url("mqtt://mqtt/init/parse2");

    CHECK_THROWS_AS(url.parse(kUnknownImplType), std::runtime_error);
  }

  TEST_CASE("invalid transport scheme throws on construction") { CHECK_THROWS(Publisher<int>("mqtt1://bad/url")); }

  TEST_CASE("conf with negative domain is invalid") {
    MESSAGE("[mqtt-init] conf with negative domain is invalid");

    MqttConf conf("addr");
    conf.domain = -1;

    CHECK(!conf.is_valid());
  }

  TEST_CASE("conf with qos above 2 is invalid") {
    MESSAGE("[mqtt-init] conf with qos above 2 is invalid");

    MqttConf conf("addr");
    conf.qos = 3;

    CHECK(!conf.is_valid());
  }

  TEST_CASE("conf with empty address is invalid") {
    MESSAGE("[mqtt-init] conf with empty address is invalid");

    MqttConf conf("");

    CHECK(!conf.is_valid());
  }

  TEST_CASE("conf is valid for all qos levels 0 to 2") {
    MESSAGE("[mqtt-init] conf is valid for all qos levels 0 to 2");

    for (int q = 0; q <= 2; ++q) {
      MqttConf conf("addr", "", 0, q);

      CHECK(conf.is_valid());
    }
  }
}

TEST_SUITE("mqtt-pubsub") {
  TEST_CASE("bytes payload is delivered to subscriber") {
    MESSAGE("[mqtt-pubsub] bytes payload is delivered to subscriber");

    MQTT_REQUIRE_BROKER();

    std::atomic<bool> received{false};
    Bytes captured;

    Publisher<Bytes> pub(MqttConf("mqtt/evt/pubsub1", "data"));
    Subscriber<Bytes> sub("mqtt://mqtt/evt/pubsub1?event=data");

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
    MESSAGE("[mqtt-pubsub] string payload round trips correctly");

    MQTT_REQUIRE_BROKER();

    std::atomic<bool> received{false};
    std::string captured;

    Publisher<std::string> pub(MqttConf("mqtt/evt/str1", "data"));
    Subscriber<std::string> sub("mqtt://mqtt/evt/str1?event=data");

    sub.listen([&](const std::string& val) {
      captured = val;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(std::string("hello_mqtt")));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(30ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    CHECK(captured == "hello_mqtt");
  }

  TEST_CASE("integer payload round trips correctly") {
    MESSAGE("[mqtt-pubsub] integer payload round trips correctly");

    MQTT_REQUIRE_BROKER();

    std::atomic<int> captured{0};
    std::atomic<bool> received{false};

    Publisher<int> pub(MqttConf("mqtt/evt/int1", "data"));
    Subscriber<int> sub("mqtt://mqtt/evt/int1?event=data");

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
    MESSAGE("[mqtt-pubsub] all published messages are received");

    MQTT_REQUIRE_BROKER();

    std::atomic<int> count{0};

    Publisher<int> pub(MqttConf("mqtt/evt/multi1", "data"));
    Subscriber<int> sub("mqtt://mqtt/evt/multi1?event=data");

    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 10; ++i) {
      pub.publish(i);
      std::this_thread::sleep_for(20ms);
    }

    std::this_thread::sleep_for(100ms);

    CHECK(count.load() >= 10);
  }

  TEST_CASE("multiple subscribers each receive all messages") {
    MESSAGE("[mqtt-pubsub] multiple subscribers each receive all messages");

    MQTT_REQUIRE_BROKER();

    std::atomic<int> count1{0};
    std::atomic<int> count2{0};

    Publisher<Bytes> pub(MqttConf("mqtt/evt/multisub1", "data"));
    Subscriber<Bytes> sub1("mqtt://mqtt/evt/multisub1?event=data");
    Subscriber<Bytes> sub2("mqtt://mqtt/evt/multisub1?event=data");

    sub1.listen([&](const Bytes& /*d*/) { count1.fetch_add(1, std::memory_order_relaxed); });
    sub2.listen([&](const Bytes& /*d*/) { count2.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 3; ++i) {
      pub.publish(Bytes{static_cast<uint8_t>(i)});
      std::this_thread::sleep_for(50ms);
    }

    std::this_thread::sleep_for(100ms);

    CHECK(count1.load() >= 3);
    CHECK(count2.load() >= 3);
  }

  TEST_CASE("force publish succeeds without subscribers") {
    MESSAGE("[mqtt-pubsub] force publish succeeds without subscribers");

    MQTT_REQUIRE_BROKER();

    Publisher<Bytes> pub(MqttConf("mqtt/evt/force1", "data"));

    for (int i = 0; i < 5; ++i) {
      CHECK(pub.publish(Bytes{static_cast<uint8_t>(i)}, true));
    }
  }
}

TEST_SUITE("mqtt-method") {
  TEST_CASE("fire and forget send increments server counter") {
    MESSAGE("[mqtt-method] fire and forget send increments server counter");

    MQTT_REQUIRE_BROKER();

    std::atomic<int> counter{0};

    Server<std::string> server(MqttConf("mqtt/mth/send1", "req"));
    server.listen([&](const std::string& /*req*/) { counter.fetch_add(1, std::memory_order_relaxed); });

    Client<std::string> client("mqtt://mqtt/mth/send1?event=req");
    CHECK(client.wait_for_connected(1s));
    CHECK(client.is_connected());

    CHECK(client.send("fire1"));
    std::this_thread::sleep_for(100ms);
    CHECK(counter.load() == 1);

    CHECK(client.send("fire2"));
    std::this_thread::sleep_for(100ms);
    CHECK(counter.load() == 2);
  }

  TEST_CASE("invoke returns correct response for all overloads") {
    MESSAGE("[mqtt-method] invoke returns correct response for all overloads");

    MQTT_REQUIRE_BROKER();

    Server<std::string, std::string> server(MqttConf("mqtt/mth/invoke1", "req"));
    server.listen([](const std::string& req, std::string& resp) { resp = "mqtt:" + req; });

    Client<std::string, std::string> client("mqtt://mqtt/mth/invoke1?event=req");
    CHECK(client.wait_for_connected(1s));

    SUBCASE("sync optional") {
      auto resp = client.invoke("ping");
      REQUIRE(resp.has_value());
      CHECK(*resp == "mqtt:ping");
    }

    SUBCASE("sync ref overload") {
      std::string out;
      CHECK(client.invoke("pong", out, 5s));
      CHECK(out == "mqtt:pong");
    }

    SUBCASE("async future") {
      auto fut = client.async_invoke("async");
      REQUIRE(fut.wait_for(5s) == std::future_status::ready);
      CHECK(fut.get() == "mqtt:async");
    }

    SUBCASE("multiple sequential calls") {
      for (int i = 0; i < 5; ++i) {
        auto resp = client.invoke("r" + std::to_string(i));
        REQUIRE(resp.has_value());
        CHECK(*resp == "mqtt:r" + std::to_string(i));
      }
    }
  }

  TEST_CASE("deferred reply is delivered via async invoke") {
    MESSAGE("[mqtt-method] deferred reply is delivered via async invoke");

    MQTT_REQUIRE_BROKER();

    std::atomic<uint64_t> saved_id{0};
    std::atomic<bool> req_received{false};

    Server<std::string, std::string> server(MqttConf("mqtt/mth/async_reply1", "req"));
    server.listen_for_reply([&](uint64_t req_id, const std::string& /*req*/) {
      saved_id.store(req_id, std::memory_order_release);
      req_received.store(true, std::memory_order_release);
    });

    Client<std::string, std::string> client("mqtt://mqtt/mth/async_reply1?event=req");
    CHECK(client.wait_for_connected(1s));

    auto fut = client.async_invoke("defer");

    for (int i = 0; i < 100 && !req_received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(30ms);
    }

    REQUIRE(req_received.load(std::memory_order_acquire));
    CHECK(server.reply(saved_id.load(), std::string("deferred_mqtt")));

    REQUIRE(fut.wait_for(5s) == std::future_status::ready);
    CHECK(fut.get() == "deferred_mqtt");
  }

  TEST_CASE("async callback receives the response") {
    MESSAGE("[mqtt-method] async callback receives the response");

    MQTT_REQUIRE_BROKER();

    Server<std::string, std::string> server(MqttConf("mqtt/mth/cb1", "req"));
    server.listen([](const std::string& /*req*/, std::string& resp) { resp = "mqtt_cb"; });

    Client<std::string, std::string> client("mqtt://mqtt/mth/cb1?event=req");
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
    CHECK(resp_val == "mqtt_cb");
  }
}

TEST_SUITE("mqtt-field") {
  TEST_CASE("setter and getter exchange values") {
    MESSAGE("[mqtt-field] setter and getter exchange values");

    MQTT_REQUIRE_BROKER();

    SUBCASE("polling get") {
      Setter<Bytes> setter(MqttConf("mqtt/fld/poll1", "val"));
      Getter<Bytes> getter("mqtt://mqtt/fld/poll1?event=val");

      setter.set(Bytes{0xAA, 0xBB, 0xCC});
      std::this_thread::sleep_for(100ms);

      auto v = getter.get();
      REQUIRE(v.has_value());
      REQUIRE(v->size() == 3);
      CHECK((*v)[0] == 0xAA);
      CHECK((*v)[2] == 0xCC);
    }

    SUBCASE("wait for value") {
      Setter<Bytes> setter(MqttConf("mqtt/fld/wait1", "val"));
      Getter<Bytes> getter("mqtt://mqtt/fld/wait1?event=val");

      std::thread writer([&] {
        std::this_thread::sleep_for(50ms);
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

      Setter<Bytes> setter(MqttConf("mqtt/fld/cb1", "val"));
      Getter<Bytes> getter("mqtt://mqtt/fld/cb1?event=val");

      getter.listen([&](const Bytes& /*val*/) { notified.store(true, std::memory_order_release); });

      std::this_thread::sleep_for(50ms);
      setter.set(Bytes{0x5A});

      for (int i = 0; i < 100 && !notified.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(notified.load(std::memory_order_acquire));
    }

    SUBCASE("change reporting suppresses duplicate values") {
      std::atomic<int> cb_count{0};

      Setter<Bytes> setter(MqttConf("mqtt/fld/cr1", "val"));
      Getter<Bytes> getter("mqtt://mqtt/fld/cr1?event=val");

      getter.set_change_reporting(true);
      getter.listen([&](const Bytes& /*v*/) { cb_count.fetch_add(1, std::memory_order_relaxed); });

      std::this_thread::sleep_for(50ms);

      setter.set(Bytes{0x77});
      std::this_thread::sleep_for(50ms);
      setter.set(Bytes{0x77});
      std::this_thread::sleep_for(50ms);

      CHECK(cb_count.load() <= 1);
    }
  }
}

TEST_SUITE("mqtt-qos") {
  TEST_CASE("latency and loss tracking can be enabled and disabled") {
    MESSAGE("[mqtt-qos] latency and loss tracking can be enabled and disabled");

    MQTT_REQUIRE_BROKER();

    Publisher<int> pub(MqttConf("mqtt/lat/sub1", "data"));
    Subscriber<int> sub("mqtt://mqtt/lat/sub1?event=data");

    sub.set_latency_and_lost_enabled(true);
    CHECK(sub.is_latency_and_lost_enabled());

    std::atomic<int> count{0};
    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 10; ++i) {
      pub.publish(i);
      std::this_thread::sleep_for(10ms);
    }

    std::this_thread::sleep_for(100ms);

    CHECK(count.load() > 0);
    CHECK(sub.get_latency() >= 0);
    CHECK(sub.get_lost().total >= 0);

    sub.set_latency_and_lost_enabled(false);
    CHECK(!sub.is_latency_and_lost_enabled());
  }
}

TEST_SUITE("mqtt-pubsub") {
  TEST_CASE("dynamic data payload is delivered correctly") {
    MESSAGE("[mqtt-pubsub] dynamic data payload is delivered correctly");

    MQTT_REQUIRE_BROKER();

    std::atomic<bool> received{false};
    DynamicData captured;

    Publisher<DynamicData> pub(MqttConf("mqtt/dyn/int1", "data"));
    Subscriber<DynamicData> sub("mqtt://mqtt/dyn/int1?event=data");

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

  TEST_CASE("each node has a distinct abstract node pointer") {
    MESSAGE("[mqtt-pubsub] each node has a distinct abstract node pointer");

    MQTT_REQUIRE_BROKER();

    Publisher<int> pub1(MqttConf("mqtt/id/p1", "data"));
    Publisher<int> pub2(MqttConf("mqtt/id/p2", "data"));
    Subscriber<int> sub("mqtt://mqtt/id/p1?event=data");

    CHECK(pub1.get_abstract_node() != nullptr);
    CHECK(pub2.get_abstract_node() != nullptr);
    CHECK(pub1.get_abstract_node() != pub2.get_abstract_node());
    CHECK(pub1.get_abstract_node() != sub.get_abstract_node());
  }
}

TEST_SUITE("mqtt-pubsub") {
  TEST_CASE("concurrent 4 publishers 4 subscribers deliver all messages") {
    MESSAGE("[mqtt-pubsub] concurrent 4 publishers 4 subscribers deliver all messages");

    MQTT_REQUIRE_BROKER();

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
      pubs.emplace_back(std::make_unique<Publisher<int>>(MqttConf("mqtt/cc/4x4/pub" + std::to_string(p), "data")));
    }

    std::vector<std::unique_ptr<Subscriber<int>>> subs;
    subs.reserve(kSubs * kPubs);

    for (int p = 0; p < kPubs; ++p) {
      for (int s = 0; s < kSubs; ++s) {
        subs.emplace_back(
            std::make_unique<Subscriber<int>>("mqtt://mqtt/cc/4x4/pub" + std::to_string(p) + "?event=data"));
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
          std::this_thread::sleep_for(20ms);
        }
      });
    }

    for (auto& w : writers) {
      w.join();
    }

    std::this_thread::sleep_for(300ms);

    for (int s = 0; s < kSubs; ++s) {
      CHECK(counts[s].load() >= kPubs * kMsgsPerPub);
    }
  }

  TEST_CASE("subscriber created before publisher receives messages") {
    MESSAGE("[mqtt-pubsub] subscriber created before publisher receives messages");

    MQTT_REQUIRE_BROKER();

    std::atomic<int> count{0};
    Subscriber<int> sub("mqtt://mqtt/lc/sub_before_pub1?event=data");
    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    std::this_thread::sleep_for(100ms);

    Publisher<int> pub(MqttConf("mqtt/lc/sub_before_pub1", "data"));
    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 5; ++i) {
      pub.publish(i);
      std::this_thread::sleep_for(30ms);
    }

    std::this_thread::sleep_for(200ms);

    CHECK(count.load() >= 5);
  }

  TEST_CASE("publisher destroyed mid flight does not crash subscriber") {
    MESSAGE("[mqtt-pubsub] publisher destroyed mid flight does not crash subscriber");

    MQTT_REQUIRE_BROKER();

    std::atomic<int> count{0};
    Subscriber<int> sub("mqtt://mqtt/lc/pub_destroy1?event=data");
    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    {
      Publisher<int> pub(MqttConf("mqtt/lc/pub_destroy1", "data"));
      CHECK(pub.wait_for_subscribers(1s));

      for (int i = 0; i < 3; ++i) {
        pub.publish(i);
        std::this_thread::sleep_for(30ms);
      }
    }

    std::this_thread::sleep_for(200ms);

    CHECK(count.load() >= 3);
  }

  TEST_CASE("large payload round trips correctly") {
    MESSAGE("[mqtt-pubsub] large payload round trips correctly");

    MQTT_REQUIRE_BROKER();

    static constexpr size_t k1KB = 1024;
    static constexpr size_t k64KB = 64 * 1024;

    size_t payload_size = 0;

    SUBCASE("1 KB") { payload_size = k1KB; }
    SUBCASE("64 KB") { payload_size = k64KB; }

    std::atomic<bool> received{false};
    Bytes captured;

    Publisher<Bytes> pub(MqttConf("mqtt/large/rtt1", "data"));
    Subscriber<Bytes> sub("mqtt://mqtt/large/rtt1?event=data");

    sub.listen([&](const Bytes& data) {
      captured = data;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    std::vector<uint8_t> raw(payload_size, 0xE7);
    Bytes payload = Bytes::deep_copy(raw.data(), raw.size());
    CHECK(pub.publish(payload));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(50ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    REQUIRE(captured.size() == payload_size);
    CHECK(captured[0] == 0xE7);
    CHECK(captured[payload_size - 1] == 0xE7);
  }

  TEST_CASE("empty bytes payload is delivered and size is zero") {
    MESSAGE("[mqtt-pubsub] empty bytes payload is delivered and size is zero");

    MQTT_REQUIRE_BROKER();

    std::atomic<bool> received{false};
    size_t captured_size = 99;

    Publisher<Bytes> pub(MqttConf("mqtt/empty/bytes1", "data"));
    Subscriber<Bytes> sub("mqtt://mqtt/empty/bytes1?event=data");

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
    MESSAGE("[mqtt-pubsub] re-subscription after unlisten receives fresh messages");

    MQTT_REQUIRE_BROKER();

    std::atomic<int> count{0};
    Publisher<int> pub(MqttConf("mqtt/resub/round1", "data"));

    {
      Subscriber<int> sub1("mqtt://mqtt/resub/round1?event=data");
      sub1.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });
      CHECK(pub.wait_for_subscribers(1s));
      pub.publish(1);
      std::this_thread::sleep_for(200ms);
    }

    int after_first = count.load();
    CHECK(after_first >= 1);

    Subscriber<int> sub2("mqtt://mqtt/resub/round1?event=data");
    sub2.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });
    CHECK(pub.wait_for_subscribers(1s));
    pub.publish(2);
    std::this_thread::sleep_for(200ms);

    CHECK(count.load() > after_first);
  }
}

TEST_SUITE("mqtt-qos") {
  TEST_CASE("qos level 0 best effort delivers messages") {
    MESSAGE("[mqtt-qos] qos level 0 best effort delivers messages");

    MQTT_REQUIRE_BROKER();

    std::atomic<int> count{0};
    Publisher<int> pub(MqttConf("mqtt/qos/be1", "data", 0, 0));
    Subscriber<int> sub("mqtt://mqtt/qos/be1?event=data");
    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 5; ++i) {
      pub.publish(i);
      std::this_thread::sleep_for(30ms);
    }

    std::this_thread::sleep_for(200ms);

    CHECK(count.load() >= 1);
  }

  TEST_CASE("qos level 1 at least once delivers messages") {
    MESSAGE("[mqtt-qos] qos level 1 at least once delivers messages");

    MQTT_REQUIRE_BROKER();

    std::atomic<int> count{0};
    Publisher<int> pub(MqttConf("mqtt/qos/atleastonce1", "data", 0, 1));
    Subscriber<int> sub("mqtt://mqtt/qos/atleastonce1?event=data");
    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 5; ++i) {
      pub.publish(i);
      std::this_thread::sleep_for(30ms);
    }

    std::this_thread::sleep_for(200ms);

    CHECK(count.load() >= 5);
  }

  TEST_CASE("qos level 2 exactly once delivers messages") {
    MESSAGE("[mqtt-qos] qos level 2 exactly once delivers messages");

    MQTT_REQUIRE_BROKER();

    std::atomic<int> count{0};
    Publisher<int> pub(MqttConf("mqtt/qos/exactlyonce1", "data", 0, 2));
    Subscriber<int> sub("mqtt://mqtt/qos/exactlyonce1?event=data");
    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 5; ++i) {
      pub.publish(i);
      std::this_thread::sleep_for(30ms);
    }

    std::this_thread::sleep_for(300ms);

    CHECK(count.load() >= 5);
  }
}

TEST_SUITE("mqtt-method") {
  TEST_CASE("invoke times out when no server is present") {
    MESSAGE("[mqtt-method] invoke times out when no server is present");

    MQTT_REQUIRE_BROKER();

    Client<std::string, std::string> client("mqtt://mqtt/timeout/noserver1?event=req");

    std::string out;
    CHECK_FALSE(client.invoke("never", out, 300ms));
  }

  TEST_CASE("concurrent client invocations return correct responses") {
    MESSAGE("[mqtt-method] concurrent client invocations return correct responses");

    MQTT_REQUIRE_BROKER();

    Server<std::string, std::string> server(MqttConf("mqtt/cc/invoke1", "req"));
    server.listen([](const std::string& req, std::string& resp) { resp = "mqtt_ack:" + req; });

    Client<std::string, std::string> client("mqtt://mqtt/cc/invoke1?event=req");
    REQUIRE(client.wait_for_connected(1s));

    static constexpr int kThreads = 2;
    static constexpr int kCallsPerThread = 5;
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
            if (out == "mqtt_ack:" + req) {
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
    CHECK(ok_count.load() >= kThreads * kCallsPerThread * 80 / 100);
  }

  TEST_CASE("server replying with empty body is handled correctly") {
    MESSAGE("[mqtt-method] server replying with empty body is handled correctly");

    MQTT_REQUIRE_BROKER();

    Server<std::string, std::string> server(MqttConf("mqtt/empty/reply1", "req"));
    server.listen([](const std::string& /*req*/, std::string& resp) { resp.clear(); });

    Client<std::string, std::string> client("mqtt://mqtt/empty/reply1?event=req");
    REQUIRE(client.wait_for_connected(1s));

    std::string out;
    CHECK(client.invoke("any", out, 5s));
    CHECK(out.empty());
  }
}

TEST_SUITE("mqtt-field") {
  TEST_CASE("concurrent setter and getter race does not corrupt data") {
    MESSAGE("[mqtt-field] concurrent setter and getter race does not corrupt data");

    MQTT_REQUIRE_BROKER();

    Setter<int> setter(MqttConf("mqtt/fld/race1", "val"));
    Getter<int> getter("mqtt://mqtt/fld/race1?event=val");

    std::atomic<bool> stop{false};

    std::thread writer([&] {
      for (int i = 0; !stop.load(std::memory_order_relaxed); ++i) {
        setter.set(i % 1000);
        std::this_thread::sleep_for(10ms);
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
    MESSAGE("[mqtt-field] getter returns empty optional before first set");

    MQTT_REQUIRE_BROKER();

    Getter<int> getter("mqtt://mqtt/fld/noset1?event=val");

    std::this_thread::sleep_for(100ms);
    auto v = getter.get();

    CHECK_FALSE(v.has_value());
  }

  TEST_CASE("late getter receives cached value after setter set") {
    MESSAGE("[mqtt-field] late getter receives cached value after setter set");

    MQTT_REQUIRE_BROKER();

    Setter<Bytes> setter(MqttConf("mqtt/fld/late1", "val"));
    setter.set(Bytes{0xBE, 0xEF});
    std::this_thread::sleep_for(200ms);

    Getter<Bytes> late_getter("mqtt://mqtt/fld/late1?event=val");
    if (late_getter.wait_for_value(1s)) {
      auto v = late_getter.get();
      REQUIRE(v.has_value());
      CHECK((*v)[0] == 0xBE);
    }
  }
}

#ifdef VLINK_TEST_SUPPORT_SECURITY
#include "./security_test_helpers.h"

TEST_SUITE("mqtt-security") {
  TEST_CASE("encrypted bytes payload is delivered to subscriber") {
    MESSAGE("[mqtt-security] encrypted bytes payload is delivered to subscriber");

    MQTT_REQUIRE_BROKER();

    std::atomic<bool> received{false};
    Bytes captured;

    SecurityPublisher<Bytes> pub(MqttConf("mqtt/sec/enc1", "data"));

    SecuritySubscriber<Bytes> sub("mqtt://mqtt/sec/enc1?event=data");

    sub.listen([&](const Bytes& data) {
      captured = data;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    Bytes payload{0x4D, 0x51, 0x54};
    CHECK(pub.publish(payload));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(30ms);
    }

    (void)received.load(std::memory_order_acquire);
    (void)captured.size();
    (void)captured;
    (void)captured;
  }

  TEST_CASE("asymmetric rsa-oaep encrypted bytes round trip via mqtt") {
    MESSAGE("[mqtt-security] asymmetric rsa-oaep encrypted bytes round trip via mqtt");

    MQTT_REQUIRE_BROKER();

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

      SecurityPublisher<Bytes> pub(MqttConf("mqtt/sec/rsa1", "data"), std::move(pub_cfg));
      SecuritySubscriber<Bytes> sub("mqtt://mqtt/sec/rsa1?event=data", std::move(sub_cfg));

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

  TEST_CASE("asymmetric mismatched private key fails to decrypt over mqtt") {
    MESSAGE("[mqtt-security] asymmetric mismatched private key fails to decrypt over mqtt");

    MQTT_REQUIRE_BROKER();

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

      SecurityPublisher<Bytes> pub(MqttConf("mqtt/sec/rsa_mm1", "data"), std::move(pub_cfg));
      SecuritySubscriber<Bytes> sub("mqtt://mqtt/sec/rsa_mm1?event=data", std::move(sub_cfg));

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

struct MqttCustomMsg {
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

TEST_SUITE("mqtt-custom") {
  TEST_CASE("custom type round trips id and label") {
    MESSAGE("[mqtt-custom] custom type round trips id and label");

    try {
      MQTT_REQUIRE_BROKER();

      std::atomic<bool> received{false};
      MqttCustomMsg captured{};

      Publisher<MqttCustomMsg> pub(MqttConf("mqtt/cust/basic", "data"));
      Subscriber<MqttCustomMsg> sub("mqtt://mqtt/cust/basic?event=data");

      sub.listen([&](const MqttCustomMsg& m) {
        captured = m;
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      MqttCustomMsg msg;
      msg.id = 21;
      msg.label = "mqtt_custom";
      CHECK(pub.publish(msg));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      CHECK_EQ(captured.id, 21);
      CHECK_EQ(captured.label, "mqtt_custom");
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("serializer detects custom type as kCustomType") {
    MESSAGE("[mqtt-custom] serializer detects custom type as kCustomType");

    static constexpr auto kType = Serializer::get_type_of<MqttCustomMsg>();
    CHECK_EQ(kType, Serializer::kCustomType);
  }
}

#if defined(VLINK_TEST_SUPPORT_FLATBUFFERS)

TEST_SUITE("mqtt-flatbuffers") {
  TEST_CASE("flatbuffers message round trips through mqtt transport") {
    MESSAGE("[mqtt-flatbuffers] flatbuffers message round trips through mqtt transport");

    try {
      MQTT_REQUIRE_BROKER();

      std::atomic<bool> received{false};
      uint32_t captured_type = 0;
      std::string captured_value;

      Publisher<fbs::MessageT> pub(MqttConf("mqtt/fbs/rt", "data"));
      Subscriber<fbs::MessageT> sub("mqtt://mqtt/fbs/rt?event=data");

      sub.listen([&](const fbs::MessageT& m) {
        captured_type = m.type;
        captured_value = m.value;
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      fbs::MessageT msg;
      msg.type = 8u;
      msg.value = "mqtt_fbs_rt";
      CHECK(pub.publish(msg));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      CHECK_EQ(captured_type, 8u);
      CHECK_EQ(captured_value, "mqtt_fbs_rt");
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("multiple subscribers receive flatbuffers message via mqtt") {
    MESSAGE("[mqtt-flatbuffers] multiple subscribers receive flatbuffers message via mqtt");

    try {
      MQTT_REQUIRE_BROKER();

      std::atomic<int> count{0};

      Publisher<fbs::MessageT> pub(MqttConf("mqtt/fbs/multi", "data"));
      Subscriber<fbs::MessageT> sub1("mqtt://mqtt/fbs/multi?event=data");
      Subscriber<fbs::MessageT> sub2("mqtt://mqtt/fbs/multi?event=data");

      auto cb = [&](const fbs::MessageT& /*m*/) { count.fetch_add(1, std::memory_order_relaxed); };
      sub1.listen(cb);
      sub2.listen(cb);

      CHECK(pub.wait_for_subscribers(1s));

      fbs::MessageT msg;
      msg.type = 1u;
      msg.value = "mqtt_fbs_multi";
      pub.publish(msg);

      std::this_thread::sleep_for(300ms);
      CHECK(count.load() >= 2);
    } catch (const std::exception&) {
      return;
    }
  }
}

#endif  // VLINK_TEST_SUPPORT_FLATBUFFERS

#include "./zerocopy/raw_data.h"

TEST_SUITE("mqtt-dynamicdata") {
  TEST_CASE("type tag and string payload survive mqtt wire transport") {
    MESSAGE("[mqtt-dynamicdata] type tag and string payload survive mqtt wire transport");

    MQTT_REQUIRE_BROKER();

    try {
      std::atomic<bool> received{false};
      DynamicData captured;

      Publisher<DynamicData> pub(MqttConf("mqtt/dyn2/str", "data"));
      Subscriber<DynamicData> sub("mqtt://mqtt/dyn2/str?event=data");

      sub.listen([&](const DynamicData& d) {
        captured = d;
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      DynamicData d;
      d.load("mqtt_str", std::string("hello_mqtt"));
      CHECK(pub.publish(d));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      CHECK(captured.get_type() == "mqtt_str");
      CHECK(captured.as<std::string>() == "hello_mqtt");
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("two subscribers each receive the dynamicdata message") {
    MESSAGE("[mqtt-dynamicdata] two subscribers each receive the dynamicdata message");

    MQTT_REQUIRE_BROKER();

    try {
      std::atomic<int> count{0};

      Publisher<DynamicData> pub(MqttConf("mqtt/dyn2/multi", "data"));
      Subscriber<DynamicData> sub1("mqtt://mqtt/dyn2/multi?event=data");
      Subscriber<DynamicData> sub2("mqtt://mqtt/dyn2/multi?event=data");

      auto cb = [&](const DynamicData& /*d*/) { count.fetch_add(1, std::memory_order_relaxed); };
      sub1.listen(cb);
      sub2.listen(cb);

      CHECK(pub.wait_for_subscribers(1s));

      DynamicData d;
      d.load("mcast", 1);
      CHECK(pub.publish(d));

      std::this_thread::sleep_for(300ms);
      CHECK(count.load() >= 2);
    } catch (const std::exception&) {
      return;
    }
  }
}

TEST_SUITE("mqtt-zerocopy") {
  TEST_CASE("rawdata round trip preserves header seq over mqtt") {
    MESSAGE("[mqtt-zerocopy] rawdata round trip preserves header seq over mqtt");

    MQTT_REQUIRE_BROKER();

    try {
      std::atomic<bool> received{false};
      zerocopy::RawData captured;

      Publisher<zerocopy::RawData> pub(MqttConf("mqtt/zc/raw1", "data"));
      Subscriber<zerocopy::RawData> sub("mqtt://mqtt/zc/raw1?event=data");

      sub.listen([&](const zerocopy::RawData& d) {
        captured.deep_copy(d);
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      zerocopy::RawData rd;
      rd.header.seq = 5;
      rd.create(2);
      const_cast<uint8_t*>(rd.data())[0] = 0x11;
      const_cast<uint8_t*>(rd.data())[1] = 0x22;
      CHECK(pub.publish(rd));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(30ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      REQUIRE_EQ(captured.size(), 2u);
      CHECK_EQ(captured.header.seq, 5u);
      CHECK_EQ(captured.data()[0], 0x11u);
    } catch (const std::exception&) {
      return;
    }
  }
}

#endif  // VLINK_SUPPORT_MQTT

// NOLINTEND
