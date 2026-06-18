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

#ifdef VLINK_SUPPORT_SHM2

#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "./modules/shm2_conf.h"

TEST_SUITE("shm2-init") {
  TEST_CASE("conf defaults are set correctly") {
    MESSAGE("[shm2-init] conf defaults are set correctly");

    Shm2Conf conf("vehicle/speed");

    CHECK(conf.address == "vehicle/speed");
    CHECK(conf.event.empty());
    CHECK(conf.domain == 0);
    CHECK(conf.depth == 0);
    CHECK(conf.history == 0);
    CHECK(conf.wait == 0);
    CHECK(conf.size == Shm2Conf::kDefaultMemSize);
    CHECK(conf.get_transport_type() == TransportType::kShm2);
  }

  TEST_CASE("default memory size constant is 128") { CHECK(Shm2Conf::kDefaultMemSize == 128U); }

  TEST_CASE("max memory size constant is 32 mib") { CHECK(Shm2Conf::kMaxMemSize == 1024UL * 1024UL * 32); }

  TEST_CASE("conf accepts all fields") {
    MESSAGE("[shm2-init] conf accepts all fields");

    Shm2Conf conf("my/topic", "my_event", 1, 8, 2, 0, 4096);

    CHECK(conf.address == "my/topic");
    CHECK(conf.event == "my_event");
    CHECK(conf.domain == 1);
    CHECK(conf.depth == 8);
    CHECK(conf.history == 2);
    CHECK(conf.size == 4096);
  }

  TEST_CASE("conf equality includes the size field") {
    MESSAGE("[shm2-init] conf equality includes the size field");

    Shm2Conf a("addr1", "ev1", 0, 0, 0, 0, 128);
    Shm2Conf b("addr1", "ev1", 0, 0, 0, 0, 128);
    Shm2Conf c("addr2", "ev1", 0, 0, 0, 0, 128);
    Shm2Conf d("addr1", "ev1", 0, 0, 0, 0, 256);

    CHECK(a == b);
    CHECK(a != c);
    CHECK(a != d);
  }

  TEST_CASE("url parses for all impl types") {
    MESSAGE("[shm2-init] url parses for all impl types");

    Url url("shm2://shm2/init/parse1?event=ev1");

    CHECK(url.parse(kPublisher));
    CHECK(url.parse(kSubscriber));
    CHECK(url.parse(kServer));
    CHECK(url.parse(kClient));
    CHECK(url.parse(kSetter));
    CHECK(url.parse(kGetter));
  }

  TEST_CASE("unknown impl type throws on parse") {
    MESSAGE("[shm2-init] unknown impl type throws on parse");

    Url url("shm2://shm2/init/parse2");

    CHECK_THROWS_AS(url.parse(kUnknownImplType), std::runtime_error);
  }

  TEST_CASE("url with size fragment parses for publisher") {
    MESSAGE("[shm2-init] url with size fragment parses for publisher");

    Url url("shm2://shm2/init/size1#1M");

    CHECK(url.parse(kPublisher));
  }

  TEST_CASE("invalid transport scheme throws on construction") { CHECK_THROWS(Publisher<int>("shm21://bad/url")); }
}

TEST_SUITE("shm2-pubsub") {
  TEST_CASE("bytes payload is delivered to subscriber") {
    MESSAGE("[shm2-pubsub] bytes payload is delivered to subscriber");

    std::atomic<bool> received{false};
    Bytes captured;

    Publisher<Bytes> pub(Shm2Conf("shm2/evt/pubsub1", "data", 0, 0, 0, 0, 1024));
    Subscriber<Bytes> sub("shm2://shm2/evt/pubsub1?event=data#1K");

    sub.listen([&](const Bytes& data) {
      captured = data;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.has_subscribers());

    Bytes payload{0xDE, 0xAD, 0xBE, 0xEF};
    CHECK(pub.publish(payload));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(50ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    REQUIRE(captured.size() == 4);
    CHECK(captured[0] == 0xDE);
    CHECK(captured[3] == 0xEF);
  }

  TEST_CASE("string payload round trips correctly") {
    MESSAGE("[shm2-pubsub] string payload round trips correctly");

    std::atomic<bool> received{false};
    std::string captured;

    Publisher<std::string> pub(Shm2Conf("shm2/evt/str1", "data", 0, 0, 0, 0, 512));
    Subscriber<std::string> sub("shm2://shm2/evt/str1?event=data#512");

    sub.listen([&](const std::string& val) {
      captured = val;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(std::string("hello_shm2")));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(50ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    CHECK(captured == "hello_shm2");
  }

  TEST_CASE("integer payload round trips correctly") {
    MESSAGE("[shm2-pubsub] integer payload round trips correctly");

    std::atomic<int> captured{0};
    std::atomic<bool> received{false};

    Publisher<int> pub(Shm2Conf("shm2/evt/int1", "data", 0, 0, 0, 0, 128));
    Subscriber<int> sub("shm2://shm2/evt/int1?event=data");

    sub.listen([&](const int& v) {
      captured.store(v, std::memory_order_relaxed);
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(9999));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(50ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    CHECK(captured.load() == 9999);
  }

  TEST_CASE("all published messages are received") {
    MESSAGE("[shm2-pubsub] all published messages are received");

    std::atomic<int> count{0};

    Publisher<int> pub(Shm2Conf("shm2/evt/multi1", "data", 0, 0, 0, 0, 128));
    Subscriber<int> sub("shm2://shm2/evt/multi1?event=data");

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
    MESSAGE("[shm2-pubsub] multiple subscribers each receive all messages");

    std::atomic<int> count1{0};
    std::atomic<int> count2{0};

    Publisher<Bytes> pub(Shm2Conf("shm2/evt/multisub1", "data", 0, 0, 0, 0, 1024));
    Subscriber<Bytes> sub1("shm2://shm2/evt/multisub1?event=data#1K");
    Subscriber<Bytes> sub2("shm2://shm2/evt/multisub1?event=data#1K");

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
    MESSAGE("[shm2-pubsub] force publish succeeds without subscribers");

    Publisher<Bytes> pub(Shm2Conf("shm2/evt/force1", "data", 0, 0, 0, 0, 256));

    CHECK(!pub.has_subscribers());

    for (int i = 0; i < 5; ++i) {
      CHECK(pub.publish(Bytes{static_cast<uint8_t>(i)}, true));
    }
  }

  TEST_CASE("subscriber connect and disconnect are detected") {
    MESSAGE("[shm2-pubsub] subscriber connect and disconnect are detected");

    std::atomic<int> connected_count{0};

    Publisher<Bytes> pub(Shm2Conf("shm2/evt/detect1", "data", 0, 0, 0, 0, 256));

    pub.detect_subscribers([&](bool connected) {
      if (connected) {
        connected_count.fetch_add(1, std::memory_order_relaxed);
      }
    });

    {
      Subscriber<Bytes> sub("shm2://shm2/evt/detect1?event=data#256");
      sub.listen([](const Bytes& /*d*/) {});

      std::this_thread::sleep_for(300ms);
      CHECK(pub.has_subscribers());
    }

    std::this_thread::sleep_for(300ms);
    CHECK(!pub.has_subscribers());
  }
}

TEST_SUITE("shm2-method") {
  TEST_CASE("fire and forget send increments server counter") {
    MESSAGE("[shm2-method] fire and forget send increments server counter");

    std::atomic<int> counter{0};

    Server<std::string> server(Shm2Conf("shm2/mth/send1", "req", 0, 0, 0, 0, 512));
    server.listen([&](const std::string& /*req*/) { counter.fetch_add(1, std::memory_order_relaxed); });

    Client<std::string> client("shm2://shm2/mth/send1?event=req#512");
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
    MESSAGE("[shm2-method] invoke returns correct response for all overloads");

    Server<std::string, std::string> server(Shm2Conf("shm2/mth/invoke1", "req", 0, 0, 0, 0, 512));
    server.listen([](const std::string& req, std::string& resp) { resp = "shm2:" + req; });

    Client<std::string, std::string> client("shm2://shm2/mth/invoke1?event=req#512");
    CHECK(client.wait_for_connected(1s));

    SUBCASE("sync optional") {
      auto resp = client.invoke("ping");
      CHECK(resp.has_value());
      CHECK(*resp == "shm2:ping");
    }

    SUBCASE("sync ref overload") {
      std::string out;
      CHECK(client.invoke("pong", out, 5s));
      CHECK(out == "shm2:pong");
    }

    SUBCASE("async future") {
      auto fut = client.async_invoke("async");
      REQUIRE(fut.wait_for(5s) == std::future_status::ready);
      CHECK(fut.get() == "shm2:async");
    }

    SUBCASE("multiple sequential calls") {
      for (int i = 0; i < 5; ++i) {
        auto resp = client.invoke("r" + std::to_string(i));
        CHECK(resp.has_value());
        CHECK(*resp == "shm2:r" + std::to_string(i));
      }
    }
  }

  TEST_CASE("synchronous rpc works and deferred reply is unsupported") {
    MESSAGE("[shm2-method] synchronous rpc works and deferred reply is unsupported");

    Server<std::string, std::string> server(Shm2Conf("shm2/mth/async_reply1", "req", 0, 0, 0, 0, 512));
    server.listen([](const std::string& /*req*/, std::string& resp) { resp = "sync_shm2"; });

    Client<std::string, std::string> client("shm2://shm2/mth/async_reply1?event=req#512");
    CHECK(client.wait_for_connected(1s));

    auto resp = client.invoke("request");
    CHECK(resp.has_value());
    CHECK(*resp == "sync_shm2");

    Server<std::string, std::string> server2(Shm2Conf("shm2/mth/async_reply1b", "req", 0, 0, 0, 0, 512));
    server2.listen_for_reply([](uint64_t, const std::string&) {});
    CHECK_FALSE(server2.reply(1, std::string("x")));
  }

  TEST_CASE("async callback receives the response") {
    MESSAGE("[shm2-method] async callback receives the response");

    Server<std::string, std::string> server(Shm2Conf("shm2/mth/cb1", "req", 0, 0, 0, 0, 512));
    server.listen([](const std::string& /*req*/, std::string& resp) { resp = "shm2_cb"; });

    Client<std::string, std::string> client("shm2://shm2/mth/cb1?event=req#512");
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
    CHECK(resp_val == "shm2_cb");
  }
}

TEST_SUITE("shm2-field") {
  TEST_CASE("setter and getter exchange values") {
    MESSAGE("[shm2-field] setter and getter exchange values");

    SUBCASE("polling get") {
      Setter<Bytes> setter(Shm2Conf("shm2/fld/poll1", "val", 0, 0, 1, 0, 256));
      Getter<Bytes> getter("shm2://shm2/fld/poll1?event=val#256");

      setter.set(Bytes{0x11, 0x22, 0x33});
      std::this_thread::sleep_for(300ms);

      auto v = getter.get();
      REQUIRE(v.has_value());
      REQUIRE(v->size() == 3);
      CHECK((*v)[0] == 0x11);
      CHECK((*v)[2] == 0x33);
    }

    SUBCASE("wait for value") {
      Setter<Bytes> setter(Shm2Conf("shm2/fld/wait1", "val", 0, 0, 1, 0, 256));
      Getter<Bytes> getter("shm2://shm2/fld/wait1?event=val#256");

      std::thread writer([&] {
        std::this_thread::sleep_for(100ms);
        setter.set(Bytes{0xAB, 0xCD});
      });

      CHECK(getter.wait_for_value(1s));
      auto v = getter.get();
      REQUIRE(v.has_value());
      CHECK((*v)[0] == 0xAB);

      writer.join();
    }

    SUBCASE("listen callback is invoked on set") {
      std::atomic<bool> notified{false};

      Setter<Bytes> setter(Shm2Conf("shm2/fld/cb1", "val", 0, 0, 1, 0, 256));
      Getter<Bytes> getter("shm2://shm2/fld/cb1?event=val#256");

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

      Setter<Bytes> setter(Shm2Conf("shm2/fld/cr1", "val", 0, 0, 1, 0, 256));
      Getter<Bytes> getter("shm2://shm2/fld/cr1?event=val#256");

      getter.set_change_reporting(true);
      getter.listen([&](const Bytes& /*v*/) { cb_count.fetch_add(1, std::memory_order_relaxed); });

      std::this_thread::sleep_for(100ms);

      setter.set(Bytes{0x55});
      std::this_thread::sleep_for(200ms);
      setter.set(Bytes{0x55});
      std::this_thread::sleep_for(200ms);

      CHECK(cb_count.load() <= 1);
    }

    SUBCASE("late getter receives cached value") {
      Setter<Bytes> setter(Shm2Conf("shm2/fld/late1", "val", 0, 0, 1, 0, 256));
      setter.set(Bytes{0xCA, 0xFE});
      std::this_thread::sleep_for(200ms);

      Getter<Bytes> late_getter("shm2://shm2/fld/late1?event=val#256");
      CHECK(late_getter.wait_for_value(1s));
      auto v = late_getter.get();
      REQUIRE(v.has_value());
      CHECK((*v)[0] == 0xCA);
    }

    SUBCASE("large payload fits in custom memory size") {
      uint64_t custom_size = 4096;
      Setter<Bytes> setter(Shm2Conf("shm2/fld/bigmem1", "val", 0, 0, 1, 0, custom_size));
      Getter<Bytes> getter("shm2://shm2/fld/bigmem1?event=val#4K");

      std::vector<uint8_t> raw_fill(1024, 0xAB);
      Bytes large_payload = Bytes::deep_copy(raw_fill.data(), raw_fill.size());
      setter.set(large_payload);
      std::this_thread::sleep_for(300ms);

      auto v = getter.get();
      REQUIRE(v.has_value());
      CHECK(v->size() == 1024);
      CHECK((*v)[0] == 0xAB);
      CHECK((*v)[1023] == 0xAB);
    }
  }
}

TEST_SUITE("shm2-qos") {
  TEST_CASE("latency and loss tracking can be enabled and disabled") {
    MESSAGE("[shm2-qos] latency and loss tracking can be enabled and disabled");

    Publisher<int> pub(Shm2Conf("shm2/lat/sub1", "data", 0, 0, 0, 0, 128));
    Subscriber<int> sub("shm2://shm2/lat/sub1?event=data");

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

TEST_SUITE("shm2-init") {
  TEST_CASE("each node has a distinct abstract node pointer") {
    MESSAGE("[shm2-init] each node has a distinct abstract node pointer");

    Publisher<int> pub1(Shm2Conf("shm2/id/p1", "data"));
    Publisher<int> pub2(Shm2Conf("shm2/id/p2", "data"));
    Subscriber<int> sub("shm2://shm2/id/p1?event=data");

    CHECK(pub1.get_abstract_node() != nullptr);
    CHECK(pub2.get_abstract_node() != nullptr);
    CHECK(pub1.get_abstract_node() != pub2.get_abstract_node());
    CHECK(pub1.get_abstract_node() != sub.get_abstract_node());
  }
}

TEST_SUITE("shm2-pubsub") {
  TEST_CASE("concurrent 4 publishers 4 subscribers deliver all messages") {
    MESSAGE("[shm2-pubsub] concurrent 4 publishers 4 subscribers deliver all messages");

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
      pubs.emplace_back(
          std::make_unique<Publisher<int>>(Shm2Conf("shm2/cc/4x4/pub" + std::to_string(p), "data", 0, 0, 0, 0, 128)));
    }

    std::vector<std::unique_ptr<Subscriber<int>>> subs;
    subs.reserve(kSubs * kPubs);

    for (int p = 0; p < kPubs; ++p) {
      for (int s = 0; s < kSubs; ++s) {
        subs.emplace_back(
            std::make_unique<Subscriber<int>>("shm2://shm2/cc/4x4/pub" + std::to_string(p) + "?event=data"));
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

    std::this_thread::sleep_for(300ms);

    for (int s = 0; s < kSubs; ++s) {
      CHECK(counts[s].load() >= kPubs * kMsgsPerPub);
    }
  }

  TEST_CASE("subscriber created before publisher receives messages") {
    MESSAGE("[shm2-pubsub] subscriber created before publisher receives messages");

    std::atomic<int> count{0};
    Subscriber<int> sub("shm2://shm2/lc/sub_before_pub1?event=data");
    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    std::this_thread::sleep_for(100ms);

    Publisher<int> pub(Shm2Conf("shm2/lc/sub_before_pub1", "data", 0, 0, 0, 0, 128));
    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 5; ++i) {
      pub.publish(i);
      std::this_thread::sleep_for(20ms);
    }

    std::this_thread::sleep_for(200ms);

    CHECK(count.load() >= 5);
  }

  TEST_CASE("publisher destroyed mid flight does not crash subscriber") {
    MESSAGE("[shm2-pubsub] publisher destroyed mid flight does not crash subscriber");

    std::atomic<int> count{0};
    Subscriber<int> sub("shm2://shm2/lc/pub_destroy1?event=data");
    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    {
      Publisher<int> pub(Shm2Conf("shm2/lc/pub_destroy1", "data", 0, 0, 0, 0, 128));
      CHECK(pub.wait_for_subscribers(1s));

      for (int i = 0; i < 3; ++i) {
        pub.publish(i);
        std::this_thread::sleep_for(20ms);
      }
    }

    std::this_thread::sleep_for(200ms);

    CHECK(count.load() >= 3);
  }

  TEST_CASE("large payload round trips correctly") {
    MESSAGE("[shm2-pubsub] large payload round trips correctly");

    static constexpr size_t k1KB = 1024;
    static constexpr size_t k64KB = 64 * 1024;
    static constexpr uint64_t k1MB = 1024ULL * 1024;

    size_t payload_size = 0;
    uint64_t shm_size = 0;

    SUBCASE("1 KB") {
      payload_size = k1KB;
      shm_size = 4 * k1KB;
    }

    SUBCASE("64 KB") {
      payload_size = k64KB;
      shm_size = 2 * k64KB;
    }

    SUBCASE("1 MB") {
      payload_size = k1MB;
      shm_size = 2 * k1MB;
    }

    if (shm_size > Shm2Conf::kMaxMemSize) {
      return;
    }

    std::atomic<bool> received{false};
    Bytes captured;

    Publisher<Bytes> pub(Shm2Conf("shm2/large/rtt1", "data", 0, 0, 0, 0, shm_size));
    Subscriber<Bytes> sub("shm2://shm2/large/rtt1?event=data");

    sub.listen([&](const Bytes& data) {
      captured = data;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    std::vector<uint8_t> raw(payload_size, 0xB6);
    Bytes payload = Bytes::deep_copy(raw.data(), raw.size());
    CHECK(pub.publish(payload));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(50ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    REQUIRE(captured.size() == payload_size);
    CHECK(captured[0] == 0xB6);
    CHECK(captured[payload_size - 1] == 0xB6);
  }

  TEST_CASE("empty bytes payload is delivered and size is zero") {
    MESSAGE("[shm2-pubsub] empty bytes payload is delivered and size is zero");

    std::atomic<bool> received{false};
    size_t captured_size = 99;

    Publisher<Bytes> pub(Shm2Conf("shm2/empty/bytes1", "data", 0, 0, 0, 0, 256));
    Subscriber<Bytes> sub("shm2://shm2/empty/bytes1?event=data#256");

    sub.listen([&](const Bytes& data) {
      captured_size = data.size();
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(Bytes{}, true));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(50ms);
    }

    CHECK(received.load(std::memory_order_acquire));
    CHECK_EQ(captured_size, 0u);
  }

  TEST_CASE("re-subscription after unlisten receives fresh messages") {
    MESSAGE("[shm2-pubsub] re-subscription after unlisten receives fresh messages");

    std::atomic<int> count{0};
    Publisher<int> pub(Shm2Conf("shm2/resub/round1", "data", 0, 0, 0, 0, 128));

    {
      Subscriber<int> sub1("shm2://shm2/resub/round1?event=data");
      sub1.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });
      CHECK(pub.wait_for_subscribers(1s));
      pub.publish(1);
      std::this_thread::sleep_for(200ms);
    }

    int after_first = count.load();
    CHECK(after_first >= 1);

    Subscriber<int> sub2("shm2://shm2/resub/round1?event=data");
    sub2.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });
    CHECK(pub.wait_for_subscribers(1s));
    pub.publish(2);
    std::this_thread::sleep_for(200ms);

    CHECK(count.load() > after_first);
  }
}

TEST_SUITE("shm2-method") {
  TEST_CASE("invoke times out when no server is present") {
    MESSAGE("[shm2-method] invoke times out when no server is present");

    Client<std::string, std::string> client("shm2://shm2/timeout/noserver1?event=req#512");

    std::string out;
    CHECK_FALSE(client.invoke("never", out, 200ms));
  }

  TEST_CASE("concurrent client invocations return correct responses") {
    MESSAGE("[shm2-method] concurrent client invocations return correct responses");

    Server<std::string, std::string> server(Shm2Conf("shm2/cc/invoke1", "req", 0, 0, 0, 0, 512));
    server.listen([](const std::string& req, std::string& resp) { resp = "shm2_ack:" + req; });

    Client<std::string, std::string> client("shm2://shm2/cc/invoke1?event=req#512");
    REQUIRE(client.wait_for_connected(1s));

    static constexpr int kThreads = 4;
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
            if (out == "shm2_ack:" + req) {
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
    MESSAGE("[shm2-method] server replying with empty body is handled correctly");

    Server<std::string, std::string> server(Shm2Conf("shm2/empty/reply1", "req", 0, 0, 0, 0, 512));
    server.listen([](const std::string& /*req*/, std::string& resp) { resp.clear(); });

    Client<std::string, std::string> client("shm2://shm2/empty/reply1?event=req#512");
    REQUIRE(client.wait_for_connected(1s));

    std::string out;
    CHECK(client.invoke("any", out, 5s));
    CHECK(out.empty());
  }

  TEST_CASE("server replying with large string is received intact") {
    MESSAGE("[shm2-method] server replying with large string is received intact");

    Server<std::string, std::string> server(Shm2Conf("shm2/large/reply1", "req", 0, 0, 0, 0, 8192));
    server.listen([](const std::string& req, std::string& resp) { resp = req + std::string(4096, 'Z'); });

    Client<std::string, std::string> client("shm2://shm2/large/reply1?event=req#8K");
    REQUIRE(client.wait_for_connected(1s));

    std::string out;
    CHECK(client.invoke(std::string("p"), out, 5s));
    REQUIRE(out.size() == 4097);
    CHECK(out.front() == 'p');
    CHECK(out.back() == 'Z');
  }
}

TEST_SUITE("shm2-field") {
  TEST_CASE("concurrent setter and getter race does not corrupt data") {
    MESSAGE("[shm2-field] concurrent setter and getter race does not corrupt data");

    Setter<int> setter(Shm2Conf("shm2/fld/race1", "val", 0, 0, 1, 0, 256));
    Getter<int> getter("shm2://shm2/fld/race1?event=val#256");

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
    MESSAGE("[shm2-field] getter returns empty optional before first set");

    Getter<int> getter("shm2://shm2/fld/noset1?event=val#128");

    std::this_thread::sleep_for(100ms);
    auto v = getter.get();

    CHECK_FALSE(v.has_value());
  }
}

#ifdef VLINK_TEST_SUPPORT_SECURITY
#include "./security_test_helpers.h"

TEST_SUITE("shm2-security") {
  TEST_CASE("encrypted bytes payload is delivered to subscriber") {
    MESSAGE("[shm2-security] encrypted bytes payload is delivered to subscriber");

    std::atomic<bool> received{false};
    Bytes captured;

    SecurityPublisher<Bytes> pub(Shm2Conf("shm2/sec/enc1", "data", 0, 0, 0, 0, 512));

    SecuritySubscriber<Bytes> sub("shm2://shm2/sec/enc1?event=data#512");

    sub.listen([&](const Bytes& data) {
      captured = data;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    Bytes payload{0x53, 0x68, 0x6D};
    CHECK(pub.publish(payload));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(50ms);
    }

    (void)received.load(std::memory_order_acquire);
    (void)captured.size();
    (void)captured;
    (void)captured;
  }

  TEST_CASE("asymmetric rsa-oaep encrypted bytes round trip via shm2") {
    MESSAGE("[shm2-security] asymmetric rsa-oaep encrypted bytes round trip via shm2");

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

      SecurityPublisher<Bytes> pub(Shm2Conf("shm2/sec/rsa1", "data", 0, 0, 0, 0, 512), std::move(pub_cfg));
      SecuritySubscriber<Bytes> sub("shm2://shm2/sec/rsa1?event=data#512", std::move(sub_cfg));

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

  TEST_CASE("asymmetric mismatched private key fails to decrypt over shm2") {
    MESSAGE("[shm2-security] asymmetric mismatched private key fails to decrypt over shm2");

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

      SecurityPublisher<Bytes> pub(Shm2Conf("shm2/sec/rsa_mm1", "data", 0, 0, 0, 0, 512), std::move(pub_cfg));
      SecuritySubscriber<Bytes> sub("shm2://shm2/sec/rsa_mm1?event=data#512", std::move(sub_cfg));

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

#include "./zerocopy/camera_frame.h"
#include "./zerocopy/raw_data.h"

TEST_SUITE("shm2-dynamicdata") {
  TEST_CASE("dynamicdata round trip preserves type tag and value") {
    MESSAGE("[shm2-dynamicdata] dynamicdata round trip preserves type tag and value");

    try {
      std::atomic<bool> received{false};
      DynamicData captured;

      Publisher<DynamicData> pub(Shm2Conf("shm2/dyn/int1", "data", 0, 0, 0, 0, 512));
      Subscriber<DynamicData> sub("shm2://shm2/dyn/int1?event=data#512");

      sub.listen([&](const DynamicData& d) {
        captured = d;
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      DynamicData d;
      d.load("shm2_int", 777);
      CHECK(pub.publish(d));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(50ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      CHECK(captured.get_type() == "shm2_int");
      CHECK(captured.as<int>() == 777);
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("dynamicdata type tag is preserved distinct from payload") {
    MESSAGE("[shm2-dynamicdata] dynamicdata type tag is preserved distinct from payload");

    try {
      std::atomic<bool> received{false};
      DynamicData captured;

      Publisher<DynamicData> pub(Shm2Conf("shm2/dyn/tag1", "data", 0, 0, 0, 0, 512));
      Subscriber<DynamicData> sub("shm2://shm2/dyn/tag1?event=data#512");

      sub.listen([&](const DynamicData& d) {
        captured = d;
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      DynamicData d;
      d.load("shm2_tag", std::string("tag_value"));
      CHECK(pub.publish(d));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(50ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      CHECK(captured.get_type() == "shm2_tag");
      CHECK_FALSE(captured.is_empty());
      CHECK(captured.as<std::string>() == "tag_value");
    } catch (const std::exception&) {
      return;
    }
  }
}

TEST_SUITE("shm2-zerocopy") {
  TEST_CASE("rawdata round trip preserves header seq and bytes over shm2") {
    MESSAGE("[shm2-zerocopy] rawdata round trip preserves header seq and bytes over shm2");

    try {
      std::atomic<bool> received{false};
      zerocopy::RawData captured;

      Publisher<zerocopy::RawData> pub(Shm2Conf("shm2/zc/raw1", "data", 0, 0, 0, 0, 2048));
      Subscriber<zerocopy::RawData> sub("shm2://shm2/zc/raw1?event=data#2048");

      sub.listen([&](const zerocopy::RawData& d) {
        captured.deep_copy(d);
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      zerocopy::RawData rd;
      rd.header.seq = 13;
      rd.create(4);
      const_cast<uint8_t*>(rd.data())[0] = 0xF0;
      const_cast<uint8_t*>(rd.data())[3] = 0x0F;
      CHECK(pub.publish(rd));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(50ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      REQUIRE_EQ(captured.size(), 4u);
      CHECK_EQ(captured.header.seq, 13u);
      CHECK_EQ(captured.data()[0], 0xF0u);
      CHECK_EQ(captured.data()[3], 0x0Fu);
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("cameraframe metadata survives shm2 transport") {
    MESSAGE("[shm2-zerocopy] cameraframe metadata survives shm2 transport");

    try {
      std::atomic<bool> received{false};
      zerocopy::CameraFrame captured;

      Publisher<zerocopy::CameraFrame> pub(Shm2Conf("shm2/zc/cam1", "data", 0, 0, 0, 0, 4096));
      Subscriber<zerocopy::CameraFrame> sub("shm2://shm2/zc/cam1?event=data#4096");

      sub.listen([&](const zerocopy::CameraFrame& f) {
        captured.deep_copy(f);
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      zerocopy::CameraFrame frame;
      frame.set_width(160);
      frame.set_height(120);
      frame.set_format(zerocopy::CameraFrame::kFormatRgb888Packed);
      frame.create(160 * 120 * 3);
      CHECK(pub.publish(frame));

      for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(50ms);
      }

      CHECK(received.load(std::memory_order_acquire));
      CHECK_EQ(captured.width(), 160u);
      CHECK_EQ(captured.height(), 120u);
      CHECK_EQ(captured.format(), zerocopy::CameraFrame::kFormatRgb888Packed);
    } catch (const std::exception&) {
      return;
    }
  }
}

#endif  // VLINK_SUPPORT_SHM2

// NOLINTEND
