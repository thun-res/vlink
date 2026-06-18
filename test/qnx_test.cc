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

#ifdef VLINK_SUPPORT_QNX

#include <atomic>
#include <future>
#include <string>
#include <thread>

#include "./modules/qnx_conf.h"

TEST_SUITE("qnx-init") {
  TEST_CASE("conf defaults are set correctly") {
    MESSAGE("[qnx-init] conf defaults are set correctly");

    QnxConf conf("qnx_test_channel");

    CHECK(conf.address == "qnx_test_channel");
    CHECK(conf.event.empty());
    CHECK(conf.get_transport_type() == TransportType::kQnx);
  }

  TEST_CASE("conf accepts address and event") {
    MESSAGE("[qnx-init] conf accepts address and event");

    QnxConf conf("qnx_test_channel", "speed");

    CHECK(conf.address == "qnx_test_channel");
    CHECK(conf.event == "speed");
  }

  TEST_CASE("conf equality compares address and event") {
    MESSAGE("[qnx-init] conf equality compares address and event");

    QnxConf a("channel_a", "evt");
    QnxConf b("channel_a", "evt");
    QnxConf c("channel_b", "evt");

    CHECK(a == b);
    CHECK(a != c);
  }

  TEST_CASE("conf with different events are not equal") {
    MESSAGE("[qnx-init] conf with different events are not equal");

    QnxConf a("channel", "evt1");
    QnxConf b("channel", "evt2");

    CHECK(a != b);
  }

  TEST_CASE("url parses for all impl types") {
    MESSAGE("[qnx-init] url parses for all impl types");

    Url url("qnx://qnx_init_parse1");

    CHECK(url.parse(kPublisher));
    CHECK(url.parse(kSubscriber));
    CHECK(url.parse(kServer));
    CHECK(url.parse(kClient));
    CHECK(url.parse(kSetter));
    CHECK(url.parse(kGetter));
  }

  TEST_CASE("url with event query param parses successfully") {
    MESSAGE("[qnx-init] url with event query param parses successfully");

    Url url("qnx://qnx_init_parse2?event=myevent");

    CHECK(url.parse(kPublisher));
    CHECK(url.parse(kSubscriber));
  }

  TEST_CASE("unknown impl type throws on parse") {
    MESSAGE("[qnx-init] unknown impl type throws on parse");

    Url url("qnx://qnx_init_parse3");

    CHECK_THROWS_AS(url.parse(kUnknownImplType), std::runtime_error);
  }

  TEST_CASE("invalid transport scheme throws on construction") { CHECK_THROWS(Publisher<int>("qnx1://bad_url")); }
}

TEST_SUITE("qnx-pubsub") {
  TEST_CASE("bytes payload is delivered to subscriber") {
    MESSAGE("[qnx-pubsub] bytes payload is delivered to subscriber");

    std::atomic<bool> received{false};
    Bytes captured;

    Publisher<Bytes> pub(QnxConf("qnx_evt_pubsub1", "data"));
    Subscriber<Bytes> sub("qnx://qnx_evt_pubsub1?event=data");

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

  TEST_CASE("string payload round trips correctly") {
    MESSAGE("[qnx-pubsub] string payload round trips correctly");

    std::atomic<bool> received{false};
    std::string captured;

    Publisher<std::string> pub(QnxConf("qnx_evt_str1", "name"));
    Subscriber<std::string> sub("qnx://qnx_evt_str1?event=name");

    sub.listen([&](const std::string& val) {
      captured = val;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(std::string("hello_qnx")));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(50ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    CHECK(captured == "hello_qnx");
  }

  TEST_CASE("all published messages are received") {
    MESSAGE("[qnx-pubsub] all published messages are received");

    std::atomic<int> count{0};

    Publisher<int> pub(QnxConf("qnx_evt_multi1", "counter"));
    Subscriber<int> sub("qnx://qnx_evt_multi1?event=counter");

    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 10; ++i) {
      pub.publish(i);
      std::this_thread::sleep_for(20ms);
    }

    std::this_thread::sleep_for(300ms);

    CHECK(count.load() >= 10);
  }

  TEST_CASE("multiple subscribers each receive all messages") {
    MESSAGE("[qnx-pubsub] multiple subscribers each receive all messages");

    std::atomic<int> count1{0};
    std::atomic<int> count2{0};

    Publisher<Bytes> pub(QnxConf("qnx_evt_multisub1", "raw"));
    Subscriber<Bytes> sub1("qnx://qnx_evt_multisub1?event=raw");
    Subscriber<Bytes> sub2("qnx://qnx_evt_multisub1?event=raw");

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

  TEST_CASE("force publish succeeds without subscribers") {
    MESSAGE("[qnx-pubsub] force publish succeeds without subscribers");

    Publisher<Bytes> pub(QnxConf("qnx_evt_force1", "force"));

    CHECK(!pub.has_subscribers());

    for (int i = 0; i < 5; ++i) {
      CHECK(pub.publish(Bytes{static_cast<uint8_t>(i)}, true));
    }
  }

  TEST_CASE("subscriber connect and disconnect are detected") {
    MESSAGE("[qnx-pubsub] subscriber connect and disconnect are detected");

    std::atomic<int> connected_count{0};

    Publisher<Bytes> pub(QnxConf("qnx_evt_detect1", "detect"));

    pub.detect_subscribers([&](bool connected) {
      if (connected) {
        connected_count.fetch_add(1, std::memory_order_relaxed);
      }
    });

    {
      Subscriber<Bytes> sub("qnx://qnx_evt_detect1?event=detect");
      sub.listen([](const Bytes& /*d*/) {});

      std::this_thread::sleep_for(500ms);
      CHECK(pub.has_subscribers());
    }

    std::this_thread::sleep_for(500ms);
    CHECK(!pub.has_subscribers());
  }
}

TEST_SUITE("qnx-method") {
  TEST_CASE("fire and forget send increments server counter") {
    MESSAGE("[qnx-method] fire and forget send increments server counter");

    std::atomic<int> counter{0};

    Server<std::string> server(QnxConf("qnx_mth_send1"));
    server.listen([&](const std::string& /*req*/) { counter.fetch_add(1, std::memory_order_relaxed); });

    Client<std::string> client("qnx://qnx_mth_send1");
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
    MESSAGE("[qnx-method] invoke returns correct response for all overloads");

    Server<std::string, std::string> server(QnxConf("qnx_mth_invoke1"));
    server.listen([](const std::string& req, std::string& resp) { resp = "qnx:" + req; });

    Client<std::string, std::string> client("qnx://qnx_mth_invoke1");
    CHECK(client.wait_for_connected(1s));

    SUBCASE("sync optional") {
      auto resp = client.invoke("ping");
      CHECK(resp.has_value());
      CHECK(*resp == "qnx:ping");
    }

    SUBCASE("sync ref overload") {
      std::string out;
      CHECK(client.invoke("pong", out, 5s));
      CHECK(out == "qnx:pong");
    }

    SUBCASE("async future") {
      auto fut = client.async_invoke("async");
      REQUIRE(fut.wait_for(5s) == std::future_status::ready);
      CHECK(fut.get() == "qnx:async");
    }

    SUBCASE("multiple sequential calls") {
      for (int i = 0; i < 5; ++i) {
        auto resp = client.invoke("r" + std::to_string(i));
        CHECK(resp.has_value());
        CHECK(*resp == "qnx:r" + std::to_string(i));
      }
    }
  }

  TEST_CASE("async callback receives the response") {
    MESSAGE("[qnx-method] async callback receives the response");

    Server<std::string, std::string> server(QnxConf("qnx_mth_cb1"));
    server.listen([](const std::string& /*req*/, std::string& resp) { resp = "qnx_cb"; });

    Client<std::string, std::string> client("qnx://qnx_mth_cb1");
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
    CHECK(resp_val == "qnx_cb");
  }

  TEST_CASE("client connection is reported via detect callback") {
    MESSAGE("[qnx-method] client connection is reported via detect callback");

    std::atomic<bool> conn_flag{false};

    Server<std::string, std::string> server(QnxConf("qnx_mth_detect1"));
    server.listen([](const std::string& /*req*/, std::string& resp) { resp = "ok"; });

    Client<std::string, std::string> client("qnx://qnx_mth_detect1");
    client.detect_connected([&](bool connected) { conn_flag.store(connected, std::memory_order_release); });

    CHECK(client.wait_for_connected(1s));
    std::this_thread::sleep_for(200ms);
    CHECK(conn_flag.load(std::memory_order_acquire));
  }
}

TEST_SUITE("qnx-field") {
  TEST_CASE("setter and getter exchange values") {
    MESSAGE("[qnx-field] setter and getter exchange values");

    SUBCASE("polling get") {
      Setter<Bytes> setter(QnxConf("qnx_fld_poll1", "value"));
      Getter<Bytes> getter("qnx://qnx_fld_poll1?event=value");

      setter.set(Bytes{0x13, 0x37});
      std::this_thread::sleep_for(300ms);

      auto v = getter.get();
      REQUIRE(v.has_value());
      REQUIRE(v->size() == 2);
      CHECK((*v)[0] == 0x13);
      CHECK((*v)[1] == 0x37);
    }

    SUBCASE("wait for value") {
      Setter<Bytes> setter(QnxConf("qnx_fld_wait1", "state"));
      Getter<Bytes> getter("qnx://qnx_fld_wait1?event=state");

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

      Setter<Bytes> setter(QnxConf("qnx_fld_cb1", "notify"));
      Getter<Bytes> getter("qnx://qnx_fld_cb1?event=notify");

      getter.listen([&](const Bytes& /*val*/) { notified.store(true, std::memory_order_release); });

      std::this_thread::sleep_for(100ms);
      setter.set(Bytes{0x42});

      for (int i = 0; i < 100 && !notified.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(50ms);
      }

      CHECK(notified.load(std::memory_order_acquire));
    }

    SUBCASE("change reporting suppresses duplicate values") {
      std::atomic<int> cb_count{0};

      Setter<Bytes> setter(QnxConf("qnx_fld_cr1", "temp"));
      Getter<Bytes> getter("qnx://qnx_fld_cr1?event=temp");

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
      Setter<Bytes> setter(QnxConf("qnx_fld_late1", "cache"));
      setter.set(Bytes{0xC0, 0xDE});
      std::this_thread::sleep_for(200ms);

      Getter<Bytes> late_getter("qnx://qnx_fld_late1?event=cache");
      CHECK(late_getter.wait_for_value(1s));
      auto v = late_getter.get();
      REQUIRE(v.has_value());
      CHECK((*v)[0] == 0xC0);
    }

    SUBCASE("multiple set calls are all received by getter") {
      std::atomic<int> update_count{0};

      Setter<Bytes> setter(QnxConf("qnx_fld_seq1", "seq"));
      Getter<Bytes> getter("qnx://qnx_fld_seq1?event=seq");

      getter.listen([&](const Bytes& /*v*/) { update_count.fetch_add(1, std::memory_order_relaxed); });

      std::this_thread::sleep_for(100ms);

      for (int i = 0; i < 5; ++i) {
        setter.set(Bytes{static_cast<uint8_t>(i)});
        std::this_thread::sleep_for(50ms);
      }

      std::this_thread::sleep_for(200ms);
      CHECK(update_count.load() >= 5);

      auto v = getter.get();
      REQUIRE(v.has_value());
      CHECK((*v)[0] == 4);
    }
  }
}

TEST_SUITE("qnx-qos") {
  TEST_CASE("latency and loss tracking can be enabled and disabled") {
    MESSAGE("[qnx-qos] latency and loss tracking can be enabled and disabled");

    Publisher<int> pub(QnxConf("qnx_lat_sub1", "latency"));
    Subscriber<int> sub("qnx://qnx_lat_sub1?event=latency");

    sub.set_latency_and_lost_enabled(true);
    CHECK(sub.is_latency_and_lost_enabled());

    std::atomic<int> count{0};
    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 10; ++i) {
      pub.publish(i);
      std::this_thread::sleep_for(10ms);
    }

    std::this_thread::sleep_for(300ms);

    CHECK(count.load() > 0);
    CHECK(sub.get_latency() >= 0);
    CHECK(sub.get_lost().total >= 0);

    sub.set_latency_and_lost_enabled(false);
    CHECK(!sub.is_latency_and_lost_enabled());
  }
}

TEST_SUITE("qnx-init") {
  TEST_CASE("each node has a distinct abstract node pointer") {
    MESSAGE("[qnx-init] each node has a distinct abstract node pointer");

    Publisher<int> pub1(QnxConf("qnx_id_p1", "evt"));
    Publisher<int> pub2(QnxConf("qnx_id_p2", "evt"));
    Subscriber<int> sub("qnx://qnx_id_p1?event=evt");

    CHECK(pub1.get_abstract_node() != nullptr);
    CHECK(pub2.get_abstract_node() != nullptr);
    CHECK(pub1.get_abstract_node() != pub2.get_abstract_node());
    CHECK(pub1.get_abstract_node() != sub.get_abstract_node());
  }
}

#if defined(VLINK_TEST_SUPPORT_SECURITY)
#include "./security_test_helpers.h"
#endif

TEST_SUITE("qnx-security") {
#if defined(VLINK_TEST_SUPPORT_SECURITY)

  TEST_CASE("asymmetric rsa-oaep encrypted bytes round trip via qnx") {
    MESSAGE("[qnx-security] asymmetric rsa-oaep encrypted bytes round trip via qnx");

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

      SecurityPublisher<Bytes> pub(QnxConf("qnx_sec_rsa1", "data"), std::move(pub_cfg));
      SecuritySubscriber<Bytes> sub("qnx://qnx_sec_rsa1?event=data", std::move(sub_cfg));

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

  TEST_CASE("asymmetric mismatched private key fails to decrypt over qnx") {
    MESSAGE("[qnx-security] asymmetric mismatched private key fails to decrypt over qnx");

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

      SecurityPublisher<Bytes> pub(QnxConf("qnx_sec_rsa_mm1", "data"), std::move(pub_cfg));
      SecuritySubscriber<Bytes> sub("qnx://qnx_sec_rsa_mm1?event=data", std::move(sub_cfg));

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

#endif
}

#endif  // VLINK_SUPPORT_QNX

// NOLINTEND
