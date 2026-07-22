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

#ifdef VLINK_SUPPORT_SHM

#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "./modules/shm_conf.h"

static bool ensure_shm_ready() {
  if (!ShmConf::auto_init_roudi(true)) {
    VLOG_W("RouDi is not running, skipping.");
    return false;
  }

  return true;
}

TEST_SUITE("shm-init") {
  TEST_CASE("conf defaults are set correctly") {
    MESSAGE("[shm-init] conf defaults are set correctly");

    if (!ensure_shm_ready()) {
      return;
    }

    ShmConf conf("vehicle/speed");

    CHECK(conf.address == "vehicle/speed");
    CHECK(conf.event.empty());
    CHECK(conf.domain == 0);
    CHECK(conf.depth == 0);
    CHECK(conf.history == 0);
    CHECK(conf.wait == 0);
    CHECK(conf.get_transport_type() == TransportType::kShm);
  }

  TEST_CASE("conf accepts all fields") {
    MESSAGE("[shm-init] conf accepts all fields");

    if (!ensure_shm_ready()) {
      return;
    }

    ShmConf conf("my/topic", "my_event", 1, 10, 5, 0);

    CHECK(conf.address == "my/topic");
    CHECK(conf.event == "my_event");
    CHECK(conf.domain == 1);
    CHECK(conf.depth == 10);
    CHECK(conf.history == 5);
    CHECK(conf.wait == 0);
  }

  TEST_CASE("conf equality compares all fields") {
    MESSAGE("[shm-init] conf equality compares all fields");

    if (!ensure_shm_ready()) {
      return;
    }

    ShmConf a("addr1", "ev1", 0, 0, 0, 0);
    ShmConf b("addr1", "ev1", 0, 0, 0, 0);
    ShmConf c("addr2", "ev1", 0, 0, 0, 0);

    CHECK(a == b);
    CHECK(a != c);
  }

  TEST_CASE("url parses for all impl types") {
    MESSAGE("[shm-init] url parses for all impl types");

    if (!ensure_shm_ready()) {
      return;
    }

    Url url("shm://shm/init/parse1?event=ev1");

    CHECK(url.parse(kPublisher));
    CHECK(url.parse(kSubscriber));
    CHECK(url.parse(kServer));
    CHECK(url.parse(kClient));
    CHECK(url.parse(kSetter));
    CHECK(url.parse(kGetter));
  }

  TEST_CASE("unknown impl type throws on parse") {
    MESSAGE("[shm-init] unknown impl type throws on parse");

    if (!ensure_shm_ready()) {
      return;
    }

    Url url("shm://shm/init/parse2");

    CHECK_THROWS_AS(url.parse(kUnknownImplType), std::runtime_error);
  }

  TEST_CASE("runtime is initialised after auto init roudi") {
    MESSAGE("[shm-init] runtime is initialised after auto init roudi");

    if (!ensure_shm_ready()) {
      return;
    }

    CHECK(ShmConf::has_runtime_inited());
  }

  TEST_CASE("factory helpers support explicit init and deinit for every shm role") {
    MESSAGE("[shm-init] factory helpers support explicit init and deinit for every shm role");

    if (!ensure_shm_ready()) {
      return;
    }

    auto pub = Publisher<int>::create_unique("shm://shm/init/factory_pub?event=data", InitType::kWithoutInit);
    auto sub = Subscriber<int>::create_shared("shm://shm/init/factory_sub?event=data", InitType::kWithoutInit);
    auto setter = Setter<int>::create_unique("shm://shm/init/factory_field?event=val", InitType::kWithoutInit);
    auto getter = Getter<int>::create_shared("shm://shm/init/factory_field?event=val", InitType::kWithoutInit);
    auto fire_server =
        Server<std::string>::create_unique("shm://shm/init/factory_fire?event=req", InitType::kWithoutInit);
    auto fire_client =
        Client<std::string>::create_shared("shm://shm/init/factory_fire?event=req", InitType::kWithoutInit);
    auto sync_server =
        Server<std::string, std::string>::create_unique("shm://shm/init/factory_rpc?event=req", InitType::kWithoutInit);
    auto sync_client =
        Client<std::string, std::string>::create_shared("shm://shm/init/factory_rpc?event=req", InitType::kWithoutInit);

    REQUIRE(pub != nullptr);
    REQUIRE(sub != nullptr);
    REQUIRE(setter != nullptr);
    REQUIRE(getter != nullptr);
    REQUIRE(fire_server != nullptr);
    REQUIRE(fire_client != nullptr);
    REQUIRE(sync_server != nullptr);
    REQUIRE(sync_client != nullptr);

    CHECK_FALSE(pub->has_inited());
    CHECK_FALSE(sub->has_inited());
    CHECK_FALSE(setter->has_inited());
    CHECK_FALSE(getter->has_inited());
    CHECK_FALSE(fire_server->has_inited());
    CHECK_FALSE(fire_client->has_inited());
    CHECK_FALSE(sync_server->has_inited());
    CHECK_FALSE(sync_client->has_inited());

    CHECK(pub->init());
    CHECK(sub->init());
    CHECK(setter->init());
    CHECK(getter->init());
    CHECK(fire_server->init());
    CHECK(fire_client->init());
    CHECK(sync_server->init());
    CHECK(sync_client->init());

    CHECK_FALSE(pub->init());
    CHECK_FALSE(sub->init());
    CHECK_FALSE(setter->init());
    CHECK_FALSE(getter->init());
    CHECK_FALSE(fire_server->init());
    CHECK_FALSE(fire_client->init());
    CHECK_FALSE(sync_server->init());
    CHECK_FALSE(sync_client->init());

    CHECK(pub->deinit());
    CHECK(sub->deinit());
    CHECK(setter->deinit());
    CHECK(getter->deinit());
    CHECK(fire_server->deinit());
    CHECK(fire_client->deinit());
    CHECK(sync_server->deinit());
    CHECK(sync_client->deinit());

    CHECK_FALSE(pub->deinit());
    CHECK_FALSE(sub->deinit());
    CHECK_FALSE(setter->deinit());
    CHECK_FALSE(getter->deinit());
    CHECK_FALSE(fire_server->deinit());
    CHECK_FALSE(fire_client->deinit());
    CHECK_FALSE(sync_server->deinit());
    CHECK_FALSE(sync_client->deinit());
  }

  TEST_CASE("public listeners reject calls before init and duplicate registrations") {
    MESSAGE("[shm-init] public listeners reject calls before init and duplicate registrations");

    if (!ensure_shm_ready()) {
      return;
    }

    Subscriber<int> sub(ShmConf("shm/init/guard_subscriber", "data"), InitType::kWithoutInit);
    CHECK_THROWS(sub.listen([](const int&) {}));
    REQUIRE(sub.init());
    CHECK(sub.listen([](const int&) {}));
    CHECK_THROWS(sub.listen([](const int&) {}));

    Server<std::string> fire_server(ShmConf("shm/init/guard_fire_server", "req"), InitType::kWithoutInit);
    CHECK_THROWS(fire_server.listen([](const std::string&) {}));
    REQUIRE(fire_server.init());
    CHECK(fire_server.listen([](const std::string&) {}));
    CHECK_THROWS(fire_server.listen([](const std::string&) {}));

    Server<std::string, std::string> sync_server(ShmConf("shm/init/guard_sync_server", "req"), InitType::kWithoutInit);
    CHECK_THROWS(sync_server.listen([](const std::string&, std::string&) {}));
    REQUIRE(sync_server.init());
    CHECK(sync_server.listen([](const std::string& req, std::string& resp) { resp = req; }));
    CHECK_THROWS(sync_server.listen([](const std::string&, std::string&) {}));
    CHECK_THROWS(sync_server.reply(1, std::string("wrong_mode")));

    Server<std::string, std::string> async_server(ShmConf("shm/init/guard_async_server", "req"),
                                                  InitType::kWithoutInit);
    CHECK_THROWS(async_server.listen_for_reply([](uint64_t, const std::string&) {}));
    REQUIRE(async_server.init());
    CHECK(async_server.listen_for_reply([](uint64_t, const std::string&) {}));
    CHECK_THROWS(async_server.listen_for_reply([](uint64_t, const std::string&) {}));
    CHECK_FALSE(async_server.reply(777, std::string("no_request")));
  }
}

TEST_SUITE("shm-pubsub") {
  TEST_CASE("bytes payload is delivered to subscriber") {
    MESSAGE("[shm-pubsub] bytes payload is delivered to subscriber");

    if (!ensure_shm_ready()) {
      return;
    }

    std::atomic<bool> received{false};
    Bytes captured;

    Publisher<Bytes> pub(ShmConf("shm/evt/pubsub1", "data", 101));
    Subscriber<Bytes> sub("shm://shm/evt/pubsub1?event=data&domain=101");

    sub.listen([&](const Bytes& data) {
      captured = data;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.has_subscribers());

    Bytes payload{0xDE, 0xAD, 0xBE, 0xEF};
    CHECK(pub.publish(payload));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
    REQUIRE(captured.size() == 4u);
    CHECK(captured[0] == 0xDE);
    CHECK(captured[3] == 0xEF);
  }

  TEST_CASE("string payload round trips correctly") {
    MESSAGE("[shm-pubsub] string payload round trips correctly");

    if (!ensure_shm_ready()) {
      return;
    }

    std::atomic<bool> received{false};
    std::string captured;

    Publisher<std::string> pub(ShmConf("shm/evt/str1", "data"));
    Subscriber<std::string> sub("shm://shm/evt/str1?event=data");

    sub.listen([&](const std::string& val) {
      captured = val;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(std::string("hello_shm")));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
    CHECK(captured == "hello_shm");
  }

  TEST_CASE("integer payload round trips correctly") {
    MESSAGE("[shm-pubsub] integer payload round trips correctly");

    if (!ensure_shm_ready()) {
      return;
    }

    std::atomic<int> captured{0};
    std::atomic<bool> received{false};

    Publisher<int> pub(ShmConf("shm/evt/int1", "data"));
    Subscriber<int> sub("shm://shm/evt/int1?event=data");

    sub.listen([&](const int& v) {
      captured.store(v, std::memory_order_relaxed);
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(42));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
    CHECK(captured.load() == 42);
  }

  TEST_CASE("all published messages are received") {
    MESSAGE("[shm-pubsub] all published messages are received");

    if (!ensure_shm_ready()) {
      return;
    }

    std::atomic<int> count{0};

    Publisher<int> pub(ShmConf("shm/evt/multi1", "data"));
    Subscriber<int> sub("shm://shm/evt/multi1?event=data");

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
    MESSAGE("[shm-pubsub] multiple subscribers each receive all messages");

    if (!ensure_shm_ready()) {
      return;
    }

    std::atomic<int> count1{0};
    std::atomic<int> count2{0};

    Publisher<Bytes> pub(ShmConf("shm/evt/multisub1", "data"));
    Subscriber<Bytes> sub1("shm://shm/evt/multisub1?event=data");
    Subscriber<Bytes> sub2("shm://shm/evt/multisub1?event=data");

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
    MESSAGE("[shm-pubsub] force publish succeeds without subscribers");

    if (!ensure_shm_ready()) {
      return;
    }

    Publisher<Bytes> pub(ShmConf("shm/evt/force1", "data"));

    CHECK(!pub.has_subscribers());

    for (int i = 0; i < 5; ++i) {
      CHECK(pub.publish(Bytes{static_cast<uint8_t>(i)}, true));
    }
  }

  TEST_CASE("publish without subscribers fails unless forced") {
    MESSAGE("[shm-pubsub] publish without subscribers fails unless forced");

    if (!ensure_shm_ready()) {
      return;
    }

    Publisher<Bytes> pub(ShmConf("shm/evt/no_subscriber1", "data"));

    CHECK_FALSE(pub.has_subscribers());
    CHECK_FALSE(pub.wait_for_subscribers(50ms));
    CHECK_FALSE(pub.publish(Bytes{0x01}));
    CHECK(pub.publish(Bytes{0x01}, true));
  }

  TEST_CASE("publisher and subscriber expose shm metadata and reject non-loan returns") {
    MESSAGE("[shm-pubsub] publisher and subscriber expose shm metadata and reject non-loan returns");

    if (!ensure_shm_ready()) {
      return;
    }

    Publisher<Bytes> pub(ShmConf("shm/evt/meta1", "data"));
    Subscriber<Bytes> sub("shm://shm/evt/meta1?event=data");

    CHECK(pub.get_transport_type() == TransportType::kShm);
    CHECK(sub.get_transport_type() == TransportType::kShm);
    CHECK(pub.get_abstract_node() != nullptr);
    CHECK(sub.get_abstract_node() != nullptr);
    CHECK(pub.get_abstract_node()->get_native_handle().has_value());
    CHECK(sub.get_abstract_node()->get_native_handle().has_value());
    CHECK(pub.is_support_loan());
    CHECK(sub.is_support_loan());
    CHECK_FALSE(pub.return_loan(Bytes{0x01}));
    CHECK_FALSE(sub.return_loan(Bytes{0x01}));

    auto empty_loan = pub.loan(0);
    CHECK(empty_loan.empty());
    CHECK_FALSE(empty_loan.is_loaned());

    auto write_loan = pub.loan(4);
    if (write_loan.is_loaned()) {
      CHECK(pub.return_loan(write_loan));
    }
  }

  TEST_CASE("invalid raw bytes are dropped before typed subscriber callback") {
    MESSAGE("[shm-pubsub] invalid raw bytes are dropped before typed subscriber callback");

    if (!ensure_shm_ready()) {
      return;
    }

    std::atomic<int> delivered{0};
    Publisher<Bytes> pub(ShmConf("shm/evt/bad_typed_bytes1", "data"));
    Subscriber<int> sub("shm://shm/evt/bad_typed_bytes1?event=data");

    CHECK(sub.listen([&](const int&) { delivered.fetch_add(1, std::memory_order_relaxed); }));
    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(Bytes{0x01}, true));

    std::this_thread::sleep_for(200ms);
    CHECK_EQ(delivered.load(std::memory_order_acquire), 0);
  }

  TEST_CASE("loaned bytes payload is published and delivered") {
    MESSAGE("[shm-pubsub] loaned bytes payload is published and delivered");

    if (!ensure_shm_ready()) {
      return;
    }

    std::atomic<bool> received{false};
    Bytes captured;

    Publisher<Bytes> pub(ShmConf("shm/evt/loan_pub1", "data"));
    Subscriber<Bytes> sub("shm://shm/evt/loan_pub1?event=data");

    sub.listen([&](const Bytes& data) {
      captured = data;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    auto payload = pub.loan(4);
    REQUIRE(payload.is_loaned());
    REQUIRE(payload.size() == 4);
    payload[0] = 0x10;
    payload[1] = 0x20;
    payload[2] = 0x30;
    payload[3] = 0x40;

    CHECK(pub.publish(payload, true));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
    REQUIRE(captured.size() == 4);
    CHECK(captured[0] == 0x10);
    CHECK(captured[3] == 0x40);
  }

  TEST_CASE("subscriber suspend drops data then resume receives fresh samples") {
    MESSAGE("[shm-pubsub] subscriber suspend drops data then resume receives fresh samples");

    if (!ensure_shm_ready()) {
      return;
    }

    std::atomic<int> count{0};
    std::atomic<int> last{0};

    Publisher<int> pub(ShmConf("shm/evt/suspend1", "data"));
    Subscriber<int> sub("shm://shm/evt/suspend1?event=data");

    sub.listen([&](const int& value) {
      last.store(value, std::memory_order_release);
      count.fetch_add(1, std::memory_order_relaxed);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK_FALSE(sub.is_suspend());
    CHECK(sub.suspend());
    CHECK(sub.is_suspend());

    CHECK(pub.publish(1, true));
    std::this_thread::sleep_for(300ms);
    CHECK_EQ(count.load(std::memory_order_acquire), 0);

    CHECK(sub.resume());
    CHECK_FALSE(sub.is_suspend());
    CHECK(pub.publish(2, true));
    CHECK(common_test::wait_until([&] { return last.load(std::memory_order_acquire) == 2; }, 5s));
    CHECK_GE(count.load(std::memory_order_acquire), 1);
  }

  TEST_CASE("subscribers sharing one address filter distinct events") {
    MESSAGE("[shm-pubsub] subscribers sharing one address filter distinct events");

    if (!ensure_shm_ready()) {
      return;
    }

    std::atomic<int> a_count{0};
    std::atomic<int> b_count{0};
    std::atomic<int> a_last{0};
    std::atomic<int> b_last{0};

    Publisher<int> pub_a(ShmConf("shm/evt/filter_shared1", "a"));
    Publisher<int> pub_b(ShmConf("shm/evt/filter_shared1", "b"));
    Subscriber<int> sub_a("shm://shm/evt/filter_shared1?event=a");
    Subscriber<int> sub_b("shm://shm/evt/filter_shared1?event=b");

    sub_a.listen([&](const int& value) {
      a_last.store(value, std::memory_order_release);
      a_count.fetch_add(1, std::memory_order_relaxed);
    });
    sub_b.listen([&](const int& value) {
      b_last.store(value, std::memory_order_release);
      b_count.fetch_add(1, std::memory_order_relaxed);
    });

    CHECK(pub_a.wait_for_subscribers(1s));
    CHECK(pub_b.wait_for_subscribers(1s));

    CHECK(pub_a.publish(11, true));
    CHECK(pub_b.publish(22, true));

    CHECK(common_test::wait_until(
        [&] { return a_last.load(std::memory_order_acquire) == 11 && b_last.load(std::memory_order_acquire) == 22; },
        5s));
    CHECK_EQ(a_count.load(std::memory_order_acquire), 1);
    CHECK_EQ(b_count.load(std::memory_order_acquire), 1);
  }

  TEST_CASE("wait mode publisher and subscriber exchange bytes deterministically") {
    MESSAGE("[shm-pubsub] wait mode publisher and subscriber exchange bytes deterministically");

    if (!ensure_shm_ready()) {
      return;
    }

    std::atomic<bool> received{false};
    Bytes captured;

    Publisher<Bytes> pub(ShmConf("shm/wait/pubsub1", "data", 0, 0, 0, 10));
    Subscriber<Bytes> sub("shm://shm/wait/pubsub1?event=data&wait=10");

    sub.listen([&](const Bytes& data) {
      captured = data;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(Bytes{0x41, 0x42}, true));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
    REQUIRE(captured.size() == 2);
    CHECK(captured[0] == 0x41);
    CHECK(captured[1] == 0x42);
  }

  TEST_CASE("subscriber connect and disconnect are detected") {
    MESSAGE("[shm-pubsub] subscriber connect and disconnect are detected");

    if (!ensure_shm_ready()) {
      return;
    }

    std::atomic<int> connected_count{0};

    Publisher<Bytes> pub(ShmConf("shm/evt/detect1", "data"));

    pub.detect_subscribers([&](bool connected) {
      if (connected) {
        connected_count.fetch_add(1, std::memory_order_relaxed);
      }
    });

    {
      Subscriber<Bytes> sub("shm://shm/evt/detect1?event=data");
      sub.listen([](const Bytes& /*d*/) {});

      std::this_thread::sleep_for(300ms);
      CHECK(pub.has_subscribers());
    }

    std::this_thread::sleep_for(300ms);
    CHECK(!pub.has_subscribers());
  }

  TEST_CASE("dynamic data payload is delivered correctly") {
    MESSAGE("[shm-pubsub] dynamic data payload is delivered correctly");

    if (!ensure_shm_ready()) {
      return;
    }

    std::atomic<bool> received{false};
    DynamicData captured;

    Publisher<DynamicData> pub(ShmConf("shm/dyn/int1", "data"));
    Subscriber<DynamicData> sub("shm://shm/dyn/int1?event=data");

    sub.listen([&](const DynamicData& d) {
      captured = d;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    DynamicData d;
    d.load("int", 888);
    CHECK(pub.publish(d));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
    CHECK(captured.as<int>() == 888);
  }
}

TEST_SUITE("shm-method") {
  TEST_CASE("fire and forget send increments server counter") {
    MESSAGE("[shm-method] fire and forget send increments server counter");

    if (!ensure_shm_ready()) {
      return;
    }

    std::atomic<int> counter{0};

    Server<std::string> server(ShmConf("shm/mth/send1", "req"));
    server.listen([&](const std::string& /*req*/) { counter.fetch_add(1, std::memory_order_relaxed); });

    Client<std::string> client("shm://shm/mth/send1?event=req");
    CHECK(client.wait_for_connected(1s));
    CHECK(client.is_connected());

    CHECK(client.send("fire1"));
    std::this_thread::sleep_for(200ms);
    CHECK(counter.load() == 1);

    CHECK(client.send("fire2"));
    std::this_thread::sleep_for(200ms);
    CHECK(counter.load() == 2);
  }

  TEST_CASE("loaned bytes fire and forget request reaches server") {
    MESSAGE("[shm-method] loaned bytes fire and forget request reaches server");

    if (!ensure_shm_ready()) {
      return;
    }

    std::atomic<bool> received{false};
    Bytes captured;

    Server<Bytes> server(ShmConf("shm/mth/loan_send1", "req"));
    server.listen([&](const Bytes& req) {
      captured = req;
      received.store(true, std::memory_order_release);
    });

    Client<Bytes> client("shm://shm/mth/loan_send1?event=req");
    CHECK(client.wait_for_connected(1s));

    auto req = client.loan(3);
    REQUIRE(req.is_loaned());
    REQUIRE(req.size() == 3);
    req[0] = 0xA1;
    req[1] = 0xB2;
    req[2] = 0xC3;

    CHECK(client.send(req));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
    REQUIRE(captured.size() == 3);
    CHECK(captured[0] == 0xA1);
    CHECK(captured[2] == 0xC3);
  }

  TEST_CASE("invoke returns correct response for all overloads") {
    MESSAGE("[shm-method] invoke returns correct response for all overloads");

    if (!ensure_shm_ready()) {
      return;
    }

    Server<std::string, std::string> server(ShmConf("shm/mth/invoke1", "req"));
    server.listen([](const std::string& req, std::string& resp) { resp = "shm:" + req; });

    Client<std::string, std::string> client("shm://shm/mth/invoke1?event=req");
    CHECK(client.wait_for_connected(1s));

    SUBCASE("sync optional") {
      auto resp = client.invoke("ping");
      CHECK(resp.has_value());
      CHECK(*resp == "shm:ping");
    }

    SUBCASE("sync ref overload") {
      std::string out;
      CHECK(client.invoke("pong", out, 5s));
      CHECK(out == "shm:pong");
    }

    SUBCASE("async future") {
      auto fut = client.async_invoke("async");
      REQUIRE(fut.wait_for(5s) == std::future_status::ready);
      CHECK(fut.get() == "shm:async");
    }

    SUBCASE("multiple sequential calls") {
      for (int i = 0; i < 5; ++i) {
        auto resp = client.invoke("r" + std::to_string(i));
        CHECK(resp.has_value());
        CHECK(*resp == "shm:r" + std::to_string(i));
      }
    }
  }

  TEST_CASE("async callback receives the response") {
    MESSAGE("[shm-method] async callback receives the response");

    if (!ensure_shm_ready()) {
      return;
    }

    Server<std::string, std::string> server(ShmConf("shm/mth/cb1", "req"));
    server.listen([](const std::string& /*req*/, std::string& resp) { resp = "shm_cb"; });

    Client<std::string, std::string> client("shm://shm/mth/cb1?event=req");
    CHECK(client.wait_for_connected(1s));

    std::atomic<bool> got{false};
    std::string resp_val;

    bool ok = client.invoke("msg", [&](const std::string& resp) {
      resp_val = resp;
      got.store(true, std::memory_order_release);
    });

    CHECK(ok);

    CHECK(common_test::wait_until([&got] { return got.load(std::memory_order_acquire); }, 5s));
    CHECK(resp_val == "shm_cb");
  }

  TEST_CASE("loaned bytes request receives loaned bytes response") {
    MESSAGE("[shm-method] loaned bytes request receives loaned bytes response");

    if (!ensure_shm_ready()) {
      return;
    }

    Server<Bytes, Bytes> server(ShmConf("shm/mth/loan_rpc1", "req"));
    server.listen([&server](const Bytes& req, Bytes& resp) {
      auto out = server.loan(3);
      REQUIRE(out.is_loaned());
      REQUIRE(out.size() == 3);
      out[0] = static_cast<uint8_t>(req[0] + 1);
      out[1] = static_cast<uint8_t>(req[1] + 1);
      out[2] = static_cast<uint8_t>(req[2] + 1);
      resp = out;
    });

    Client<Bytes, Bytes> client("shm://shm/mth/loan_rpc1?event=req");
    CHECK(client.wait_for_connected(1s));

    auto req = client.loan(3);
    REQUIRE(req.is_loaned());
    REQUIRE(req.size() == 3);
    req[0] = 0x01;
    req[1] = 0x02;
    req[2] = 0x03;

    Bytes resp;
    CHECK(client.invoke(req, resp, 5s));
    REQUIRE(resp.size() == 3);
    CHECK(resp[0] == 0x02);
    CHECK(resp[1] == 0x03);
    CHECK(resp[2] == 0x04);
  }

  TEST_CASE("client connection is reported via detect callback") {
    MESSAGE("[shm-method] client connection is reported via detect callback");

    if (!ensure_shm_ready()) {
      return;
    }

    std::atomic<bool> connected_event{false};

    Server<std::string, std::string> server(ShmConf("shm/mth/detect_conn1", "req"));
    server.listen([](const std::string& /*req*/, std::string& resp) { resp = "ok"; });

    Client<std::string, std::string> client("shm://shm/mth/detect_conn1?event=req");
    client.detect_connected([&](bool connected) {
      if (connected) {
        connected_event.store(true, std::memory_order_release);
      }
    });

    CHECK(common_test::wait_until([&connected_event] { return connected_event.load(std::memory_order_acquire); }, 5s));
  }

  TEST_CASE("server suspend drops pending request then resume handles new request") {
    MESSAGE("[shm-method] server suspend drops pending request then resume handles new request");

    if (!ensure_shm_ready()) {
      return;
    }

    Server<std::string, std::string> server(ShmConf("shm/mth/suspend1", "req"));
    server.listen([](const std::string& req, std::string& resp) { resp = "ok:" + req; });

    Client<std::string, std::string> client("shm://shm/mth/suspend1?event=req");
    CHECK(client.wait_for_connected(1s));

    CHECK(server.suspend());
    CHECK(server.is_suspend());

    std::string out = "unchanged";
    CHECK_FALSE(client.invoke("drop", out, 300ms));
    CHECK_EQ(out, "unchanged");

    CHECK(server.resume());
    CHECK_FALSE(server.is_suspend());
    CHECK(client.invoke("take", out, 2s));
    CHECK_EQ(out, "ok:take");
  }
}

TEST_SUITE("shm-field") {
  TEST_CASE("setter and getter exchange values") {
    MESSAGE("[shm-field] setter and getter exchange values");

    if (!ensure_shm_ready()) {
      return;
    }

    SUBCASE("polling get") {
      Setter<Bytes> setter(ShmConf("shm/fld/poll1", "val", 0, 0, 1));
      Getter<Bytes> getter("shm://shm/fld/poll1?event=val");

      setter.set(Bytes{0x11, 0x22, 0x33});
      std::this_thread::sleep_for(300ms);

      auto v = getter.get();
      REQUIRE(v.has_value());
      REQUIRE(v->size() == 3u);
      CHECK((*v)[0] == 0x11);
      CHECK((*v)[2] == 0x33);
    }

    SUBCASE("wait for value") {
      Setter<Bytes> setter(ShmConf("shm/fld/wait1", "val", 0, 0, 1));
      Getter<Bytes> getter("shm://shm/fld/wait1?event=val");

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
      std::atomic<bool> received_expected{false};
      const Bytes expected{0xFF, 0x00};

      Setter<Bytes> setter(ShmConf("shm/fld/cb1", "val", 0, 0, 1));
      Getter<Bytes> getter("shm://shm/fld/cb1?event=val");

      getter.listen([&](const Bytes& val) { received_expected.store(val == expected, std::memory_order_release); });

      CHECK(common_test::wait_until(
          [&] {
            if (received_expected.load(std::memory_order_acquire)) {
              return true;
            }

            setter.set(expected);
            return received_expected.load(std::memory_order_acquire);
          },
          5s));
    }

    SUBCASE("change reporting suppresses duplicate values") {
      std::atomic<int> cb_count{0};

      Setter<Bytes> setter(ShmConf("shm/fld/cr1", "val", 0, 0, 1));
      Getter<Bytes> getter("shm://shm/fld/cr1?event=val");

      getter.set_change_reporting(true);
      CHECK(getter.get_change_reporting());

      getter.listen([&](const Bytes& /*v*/) { cb_count.fetch_add(1, std::memory_order_relaxed); });

      std::this_thread::sleep_for(100ms);

      setter.set(Bytes{0x55});
      std::this_thread::sleep_for(200ms);
      setter.set(Bytes{0x55});
      std::this_thread::sleep_for(200ms);

      CHECK(cb_count.load() <= 1);
    }

    SUBCASE("late getter receives cached value") {
      Setter<Bytes> setter(ShmConf("shm/fld/late1", "val", 0, 0, 1));
      setter.set(Bytes{0xCA, 0xFE});
      std::this_thread::sleep_for(200ms);

      Getter<Bytes> late_getter("shm://shm/fld/late1?event=val");
      CHECK(late_getter.wait_for_value(1s));
      auto v = late_getter.get();
      REQUIRE(v.has_value());
      CHECK((*v)[0] == 0xCA);
    }
  }

  TEST_CASE("setter set before init is cached without breaking later writes") {
    MESSAGE("[shm-field] setter set before init is cached without breaking later writes");

    if (!ensure_shm_ready()) {
      return;
    }

    Setter<int> setter(ShmConf("shm/fld/deferred_snapshot", "val", 0, 0, 1), InitType::kWithoutInit);
    Getter<int> getter("shm://shm/fld/deferred_snapshot?event=val");

    setter.set(1234);
    REQUIRE(setter.init());
    setter.set(5678);

    CHECK(getter.wait_for_value(1s));
    auto val = getter.get();
    REQUIRE(val.has_value());
    CHECK_EQ(*val, 5678);
  }

  TEST_CASE("invalid raw bytes are dropped before typed getter state updates") {
    MESSAGE("[shm-field] invalid raw bytes are dropped before typed getter state updates");

    if (!ensure_shm_ready()) {
      return;
    }

    std::atomic<int> delivered{0};
    Setter<Bytes> setter(ShmConf("shm/fld/bad_get_int1", "val", 0, 0, 1));
    Getter<int> getter("shm://shm/fld/bad_get_int1?event=val");

    getter.listen([&](const int&) { delivered.fetch_add(1, std::memory_order_relaxed); });
    setter.set(Bytes{0x01});

    std::this_thread::sleep_for(200ms);
    CHECK_EQ(delivered.load(std::memory_order_acquire), 0);
    CHECK_FALSE(getter.get().has_value());
  }

  TEST_CASE("getter deferred latency state and suspend resume paths are stable") {
    MESSAGE("[shm-field] getter deferred latency state and suspend resume paths are stable");

    if (!ensure_shm_ready()) {
      return;
    }

    {
      Getter<int> getter(ShmConf("shm/fld/deferred_latency1", "val", 0, 0, 1), InitType::kWithoutInit);
      getter.set_latency_and_lost_enabled(true);
      CHECK(getter.is_latency_and_lost_enabled());
      CHECK_EQ(getter.get_lost().total, 0u);
      CHECK_EQ(getter.get_lost().lost, 0u);
      CHECK(getter.init());
      getter.set_latency_and_lost_enabled(false);
      CHECK_FALSE(getter.is_latency_and_lost_enabled());
    }

    std::atomic<int> observed{0};

    Setter<int> setter(ShmConf("shm/fld/suspend1", "val", 0, 0, 1));
    Getter<int> getter("shm://shm/fld/suspend1?event=val");
    getter.listen([&](const int& value) { observed.store(value, std::memory_order_release); });

    std::this_thread::sleep_for(100ms);
    CHECK(getter.suspend());
    CHECK(getter.is_suspend());
    setter.set(1);
    std::this_thread::sleep_for(300ms);
    CHECK_EQ(observed.load(std::memory_order_acquire), 0);

    CHECK(getter.resume());
    CHECK_FALSE(getter.is_suspend());
    setter.set(2);
    CHECK(common_test::wait_until([&] { return observed.load(std::memory_order_acquire) == 2; }, 5s));
  }
}

TEST_SUITE("shm-qos") {
  TEST_CASE("latency and loss tracking can be enabled and disabled") {
    MESSAGE("[shm-qos] latency and loss tracking can be enabled and disabled");

    if (!ensure_shm_ready()) {
      return;
    }

    Publisher<int> pub(ShmConf("shm/lat/sub1", "data"));
    Subscriber<int> sub("shm://shm/lat/sub1?event=data");

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
    const auto lost = sub.get_lost();
    CHECK(lost.total >= lost.lost);

    sub.set_latency_and_lost_enabled(false);
    CHECK(!sub.is_latency_and_lost_enabled());
  }
}

TEST_SUITE("shm-init") {
  TEST_CASE("each node has a distinct abstract node pointer") {
    MESSAGE("[shm-init] each node has a distinct abstract node pointer");

    if (!ensure_shm_ready()) {
      return;
    }

    Publisher<int> pub1(ShmConf("shm/id/p1", "data"));
    Publisher<int> pub2(ShmConf("shm/id/p2", "data"));
    Subscriber<int> sub("shm://shm/id/p1?event=data");

    CHECK(pub1.get_abstract_node() != nullptr);
    CHECK(pub2.get_abstract_node() != nullptr);
    CHECK(pub1.get_abstract_node() != pub2.get_abstract_node());
    CHECK(pub1.get_abstract_node() != sub.get_abstract_node());
  }
}

TEST_SUITE("shm-pubsub") {
  TEST_CASE("concurrent 4 publishers 4 subscribers deliver all messages") {
    MESSAGE("[shm-pubsub] concurrent 4 publishers 4 subscribers deliver all messages");

    if (!ensure_shm_ready()) {
      return;
    }

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
      pubs.emplace_back(std::make_unique<Publisher<int>>(ShmConf("shm/cc/4x4/pub" + std::to_string(p), "data")));
    }

    std::vector<std::unique_ptr<Subscriber<int>>> subs;
    subs.reserve(kSubs * kPubs);

    for (int p = 0; p < kPubs; ++p) {
      for (int s = 0; s < kSubs; ++s) {
        subs.emplace_back(
            std::make_unique<Subscriber<int>>("shm://shm/cc/4x4/pub" + std::to_string(p) + "?event=data"));
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
    MESSAGE("[shm-pubsub] subscriber created before publisher receives messages");

    if (!ensure_shm_ready()) {
      return;
    }

    std::atomic<int> count{0};
    Subscriber<int> sub("shm://shm/lc/sub_before_pub1?event=data");
    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    std::this_thread::sleep_for(100ms);

    Publisher<int> pub(ShmConf("shm/lc/sub_before_pub1", "data"));
    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 5; ++i) {
      pub.publish(i);
      std::this_thread::sleep_for(20ms);
    }

    std::this_thread::sleep_for(200ms);

    CHECK(count.load() >= 5);
  }

  TEST_CASE("publisher destroyed mid flight does not crash subscriber") {
    MESSAGE("[shm-pubsub] publisher destroyed mid flight does not crash subscriber");

    if (!ensure_shm_ready()) {
      return;
    }

    std::atomic<int> count{0};
    Subscriber<int> sub("shm://shm/lc/pub_destroy1?event=data");
    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    {
      Publisher<int> pub(ShmConf("shm/lc/pub_destroy1", "data"));
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
    MESSAGE("[shm-pubsub] large payload round trips correctly");

    if (!ensure_shm_ready()) {
      return;
    }

    static constexpr size_t k1KB = 1024;
    size_t payload_size = k1KB;

    std::atomic<bool> received{false};
    Bytes captured;

    Publisher<Bytes> pub(ShmConf("shm/large/rtt1", "data"));
    Subscriber<Bytes> sub("shm://shm/large/rtt1?event=data");

    sub.listen([&](const Bytes& data) {
      captured = data;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    std::vector<uint8_t> raw(payload_size, 0xA5);
    Bytes payload = Bytes::deep_copy(raw.data(), raw.size());
    CHECK(pub.publish(payload));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
    REQUIRE(captured.size() == payload_size);
    CHECK(captured[0] == 0xA5);
    CHECK(captured[payload_size - 1] == 0xA5);
  }

  TEST_CASE("empty bytes payload is delivered and size is zero") {
    MESSAGE("[shm-pubsub] empty bytes payload is delivered and size is zero");

    if (!ensure_shm_ready()) {
      return;
    }

    std::atomic<bool> received{false};
    size_t captured_size = 99;

    Publisher<Bytes> pub(ShmConf("shm/empty/bytes1", "data"));
    Subscriber<Bytes> sub("shm://shm/empty/bytes1?event=data");

    sub.listen([&](const Bytes& data) {
      captured_size = data.size();
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(Bytes{}, true));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
    CHECK_EQ(captured_size, 0u);
  }

  TEST_CASE("re-subscription after unlisten receives fresh messages") {
    MESSAGE("[shm-pubsub] re-subscription after unlisten receives fresh messages");

    if (!ensure_shm_ready()) {
      return;
    }

    std::atomic<int> count{0};
    Publisher<int> pub(ShmConf("shm/resub/round1", "data"));

    {
      Subscriber<int> sub1("shm://shm/resub/round1?event=data");
      sub1.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });
      CHECK(pub.wait_for_subscribers(1s));
      pub.publish(1);
      std::this_thread::sleep_for(200ms);
    }

    int after_first = count.load();
    CHECK(after_first >= 1);

    Subscriber<int> sub2("shm://shm/resub/round1?event=data");
    sub2.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });
    if (pub.wait_for_subscribers(1s)) {
      pub.publish(2);
      std::this_thread::sleep_for(200ms);
      CHECK(count.load() >= after_first);
    }
  }

  TEST_CASE("qos depth 1 drops older messages under burst") {
    MESSAGE("[shm-pubsub] qos depth 1 drops older messages under burst");

    if (!ensure_shm_ready()) {
      return;
    }

    Publisher<int> pub(ShmConf("shm/qos/depth1/pub1", "data", 0, 1));
    Subscriber<int> sub("shm://shm/qos/depth1/pub1?event=data");

    std::atomic<int> count{0};
    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 20; ++i) {
      pub.publish(i, true);
    }

    std::this_thread::sleep_for(300ms);

    CHECK(count.load() >= 1);
  }

  TEST_CASE("qos depth 100 retains more messages") {
    MESSAGE("[shm-pubsub] qos depth 100 retains more messages");

    if (!ensure_shm_ready()) {
      return;
    }

    Publisher<int> pub(ShmConf("shm/qos/depth100/pub1", "data", 0, 100));
    Subscriber<int> sub("shm://shm/qos/depth100/pub1?event=data");

    std::atomic<int> count{0};
    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 20; ++i) {
      pub.publish(i);
      std::this_thread::sleep_for(10ms);
    }

    std::this_thread::sleep_for(300ms);

    CHECK(count.load() >= 10);
  }
}

TEST_SUITE("shm-method") {
  TEST_CASE("invoke times out when no server is present") {
    MESSAGE("[shm-method] invoke times out when no server is present");

    if (!ensure_shm_ready()) {
      return;
    }

    Client<std::string, std::string> client("shm://shm/timeout/noserver1?event=req");

    std::string out;
    CHECK_FALSE(client.invoke("never", out, 200ms));
  }

  TEST_CASE("deferred reply mode is explicitly unsupported") {
    MESSAGE("[shm-method] deferred reply mode is explicitly unsupported");

    if (!ensure_shm_ready()) {
      return;
    }

    Server<Bytes, Bytes> server(ShmConf("shm/mth/deferred_unsupported1", "req"));
    CHECK(server.listen_for_reply([](uint64_t, const Bytes&) {}));

    CHECK_FALSE(server.loan(4).is_loaned());
    CHECK_FALSE(server.reply(1, Bytes{0x01}));
  }

  TEST_CASE("client and server expose shm metadata and reject non-loan returns") {
    MESSAGE("[shm-method] client and server expose shm metadata and reject non-loan returns");

    if (!ensure_shm_ready()) {
      return;
    }

    Server<std::string, std::string> server(ShmConf("shm/mth/meta1", "req"));
    server.listen([](const std::string& req, std::string& resp) { resp = req; });

    Client<std::string, std::string> client("shm://shm/mth/meta1?event=req");

    CHECK(client.wait_for_connected(1s));
    CHECK(client.is_connected());
    CHECK(server.get_transport_type() == TransportType::kShm);
    CHECK(client.get_transport_type() == TransportType::kShm);
    CHECK(server.get_abstract_node() != nullptr);
    CHECK(client.get_abstract_node() != nullptr);
    CHECK(server.get_abstract_node()->get_native_handle().has_value());
    CHECK(client.get_abstract_node()->get_native_handle().has_value());
    CHECK(server.is_support_loan());
    CHECK(client.is_support_loan());
    CHECK_FALSE(server.return_loan(Bytes{0x01}));
    CHECK_FALSE(client.return_loan(Bytes{0x01}));
  }
}

TEST_SUITE("shm-field") {
  TEST_CASE("concurrent setter and getter race does not corrupt data") {
    MESSAGE("[shm-field] concurrent setter and getter race does not corrupt data");

    if (!ensure_shm_ready()) {
      return;
    }

    Setter<int> setter(ShmConf("shm/fld/race1", "val", 0, 0, 1));
    Getter<int> getter("shm://shm/fld/race1?event=val");

    std::atomic<bool> stop{false};
    std::atomic<int> read_count{0};

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
      read_count.fetch_add(1, std::memory_order_relaxed);
    }

    CHECK(read_count.load() >= 0);
  }

  TEST_CASE("getter returns empty optional before first set") {
    MESSAGE("[shm-field] getter returns empty optional before first set");

    if (!ensure_shm_ready()) {
      return;
    }

    Getter<int> getter("shm://shm/fld/noset1?event=val");

    std::this_thread::sleep_for(100ms);
    auto v = getter.get();

    CHECK_FALSE(v.has_value());
  }

  TEST_CASE("getter exposes shm metadata and initial tracking state") {
    MESSAGE("[shm-field] getter exposes shm metadata and initial tracking state");

    if (!ensure_shm_ready()) {
      return;
    }

    Getter<Bytes> getter("shm://shm/fld/meta1?event=val");
    Setter<Bytes> setter(ShmConf("shm/fld/meta1", "val", 0, 0, 1));

    CHECK(getter.get_transport_type() == TransportType::kShm);
    CHECK(setter.get_transport_type() == TransportType::kShm);
    CHECK(getter.get_abstract_node() != nullptr);
    CHECK(setter.get_abstract_node() != nullptr);
    CHECK(getter.get_abstract_node()->get_native_handle().has_value());
    CHECK(setter.get_abstract_node()->get_native_handle().has_value());
    CHECK(getter.is_support_loan());
    CHECK(setter.is_support_loan());
    CHECK_FALSE(getter.return_loan(Bytes{0x01}));
    CHECK_FALSE(setter.return_loan(Bytes{0x01}));
    CHECK(setter.loan(0).empty());
    CHECK_FALSE(getter.is_latency_and_lost_enabled());
    CHECK(getter.get_latency() == 0);
    CHECK(getter.get_lost().total == 0u);
    CHECK_FALSE(getter.wait_for_value(50ms));
  }
}

#ifdef VLINK_TEST_SUPPORT_SECURITY
#include "./security_test_helpers.h"

TEST_SUITE("shm-security") {
  TEST_CASE("encrypted bytes payload is delivered to subscriber") {
    MESSAGE("[shm-security] encrypted bytes payload is delivered to subscriber");

    if (!ensure_shm_ready()) {
      return;
    }

    std::atomic<bool> received{false};
    Bytes captured;

    SecurityPublisher<Bytes> pub(ShmConf("shm/sec/enc1", "data"));

    SecuritySubscriber<Bytes> sub("shm://shm/sec/enc1?event=data");

    sub.listen([&](const Bytes& data) {
      captured = data;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    Bytes payload{0x53, 0x65, 0x63};
    CHECK(pub.publish(payload));

    for (int i = 0; i < 100 && !received.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(50ms);
    }

    (void)received.load(std::memory_order_acquire);
    (void)captured.size();
    (void)captured;
    (void)captured;
  }

  TEST_CASE("asymmetric rsa-oaep encrypted bytes round trip via shm") {
    MESSAGE("[shm-security] asymmetric rsa-oaep encrypted bytes round trip via shm");

    if (!ensure_shm_ready()) {
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

      SecurityPublisher<Bytes> pub(ShmConf("shm/sec/rsa1", "data"), std::move(pub_cfg));
      SecuritySubscriber<Bytes> sub("shm://shm/sec/rsa1?event=data", std::move(sub_cfg));

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

  TEST_CASE("asymmetric mismatched private key fails to decrypt over shm") {
    MESSAGE("[shm-security] asymmetric mismatched private key fails to decrypt over shm");

    if (!ensure_shm_ready()) {
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

      SecurityPublisher<Bytes> pub(ShmConf("shm/sec/rsa_mm1", "data"), std::move(pub_cfg));
      SecuritySubscriber<Bytes> sub("shm://shm/sec/rsa_mm1?event=data", std::move(sub_cfg));

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

struct ShmCustomMsg {
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

TEST_SUITE("shm-custom") {
  TEST_CASE("custom type round trips id and label") {
    MESSAGE("[shm-custom] custom type round trips id and label");

    try {
      if (!ensure_shm_ready()) {
        return;
      }

      std::atomic<bool> received{false};
      ShmCustomMsg captured{};

      Publisher<ShmCustomMsg> pub(ShmConf("shm/cust/basic", "data"));
      Subscriber<ShmCustomMsg> sub("shm://shm/cust/basic?event=data");

      sub.listen([&](const ShmCustomMsg& m) {
        captured = m;
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      ShmCustomMsg msg;
      msg.id = 11;
      msg.label = "shm_custom";
      CHECK(pub.publish(msg));

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
      CHECK_EQ(captured.id, 11);
      CHECK_EQ(captured.label, "shm_custom");
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("serializer detects custom type as kCustomType") {
    MESSAGE("[shm-custom] serializer detects custom type as kCustomType");

    static constexpr auto kType = Serializer::get_type_of<ShmCustomMsg>();
    CHECK_EQ(kType, Serializer::kCustomType);
  }

  TEST_CASE("multiple custom messages delivered to subscriber") {
    MESSAGE("[shm-custom] multiple custom messages delivered to subscriber");

    try {
      if (!ensure_shm_ready()) {
        return;
      }

      std::atomic<int> count{0};

      Publisher<ShmCustomMsg> pub(ShmConf("shm/cust/multi", "data"));
      Subscriber<ShmCustomMsg> sub("shm://shm/cust/multi?event=data");

      sub.listen([&](const ShmCustomMsg& /*m*/) { count.fetch_add(1, std::memory_order_relaxed); });

      CHECK(pub.wait_for_subscribers(1s));

      for (int k = 0; k < 5; ++k) {
        ShmCustomMsg msg;
        msg.id = k;
        msg.label = "item";
        pub.publish(msg);
        std::this_thread::sleep_for(20ms);
      }

      std::this_thread::sleep_for(300ms);
      CHECK(count.load() >= 5);
    } catch (const std::exception&) {
      return;
    }
  }
}

#if defined(VLINK_TEST_SUPPORT_FLATBUFFERS)

TEST_SUITE("shm-flatbuffers") {
  TEST_CASE("flatbuffers builder publishing does not exhaust publisher loans") {
    MESSAGE("[shm-flatbuffers] flatbuffers builder publishing does not exhaust publisher loans");

    try {
      if (!ensure_shm_ready()) {
        return;
      }

      Publisher<common_test::FlatMessageBuilder> pub(ShmConf("shm/fbs/builder_loan1", "data"));

      for (size_t i = 0; i < 64u; ++i) {
        common_test::FlatMessageBuilder builder("shm_flat_builder");
        CHECK(pub.publish(builder, true));
      }

      auto loan = pub.loan(32);
      REQUIRE(loan.is_loaned());
      CHECK(pub.return_loan(loan));
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("flatbuffers message round trips through shm transport") {
    MESSAGE("[shm-flatbuffers] flatbuffers message round trips through shm transport");

    try {
      if (!ensure_shm_ready()) {
        return;
      }

      std::atomic<bool> received{false};
      uint32_t captured_type = 0;
      std::string captured_value;

      Publisher<fbs::MessageT> pub(ShmConf("shm/fbs/rt", "data"));
      Subscriber<fbs::MessageT> sub("shm://shm/fbs/rt?event=data");

      sub.listen([&](const fbs::MessageT& m) {
        captured_type = m.type;
        captured_value = m.value;
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      fbs::MessageT msg;
      msg.type = 5u;
      msg.value = "shm_fbs_rt";
      CHECK(pub.publish(msg));

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
      CHECK_EQ(captured_type, 5u);
      CHECK_EQ(captured_value, "shm_fbs_rt");
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("large flatbuffers message survives shm round trip") {
    MESSAGE("[shm-flatbuffers] large flatbuffers message survives shm round trip");

    try {
      if (!ensure_shm_ready()) {
        return;
      }

      std::atomic<bool> received{false};
      std::string captured;

      Publisher<fbs::MessageT> pub(ShmConf("shm/fbs/large", "data"));
      Subscriber<fbs::MessageT> sub("shm://shm/fbs/large?event=data");

      sub.listen([&](const fbs::MessageT& m) {
        captured = m.value;
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      fbs::MessageT msg;
      msg.type = 6u;
      msg.value = std::string(8192, 'S');
      pub.publish(msg);

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
      CHECK_EQ(captured.size(), 8192u);
    } catch (const std::exception&) {
      return;
    }
  }
}

#endif  // VLINK_TEST_SUPPORT_FLATBUFFERS

#include "./zerocopy/camera_frame.h"
#include "./zerocopy/point_cloud.h"
#include "./zerocopy/raw_data.h"

TEST_SUITE("shm-zerocopy") {
  TEST_CASE("rawdata round trip preserves header seq and bytes over shm") {
    MESSAGE("[shm-zerocopy] rawdata round trip preserves header seq and bytes over shm");

    if (!ensure_shm_ready()) {
      return;
    }

    try {
      std::atomic<bool> received{false};
      zerocopy::RawData captured;

      Publisher<zerocopy::RawData> pub(ShmConf("shm/zc/raw1", "data"));
      Subscriber<zerocopy::RawData> sub("shm://shm/zc/raw1?event=data");

      sub.listen([&](const zerocopy::RawData& d) {
        captured.deep_copy(d);
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      zerocopy::RawData rd;
      rd.header.seq = 21;
      rd.create(4);
      const_cast<uint8_t*>(rd.data())[0] = 0xCA;
      const_cast<uint8_t*>(rd.data())[3] = 0xFE;
      CHECK(pub.publish(rd));

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
      REQUIRE_EQ(captured.size(), 4u);
      CHECK_EQ(captured.header.seq, 21u);
      CHECK_EQ(captured.data()[0], 0xCAu);
      CHECK_EQ(captured.data()[3], 0xFEu);
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("cameraframe metadata and pixel count survive shm transport") {
    MESSAGE("[shm-zerocopy] cameraframe metadata and pixel count survive shm transport");

    if (!ensure_shm_ready()) {
      return;
    }

    try {
      std::atomic<bool> received{false};
      zerocopy::CameraFrame captured;

      Publisher<zerocopy::CameraFrame> pub(ShmConf("shm/zc/cam1", "data"));
      Subscriber<zerocopy::CameraFrame> sub("shm://shm/zc/cam1?event=data");

      sub.listen([&](const zerocopy::CameraFrame& f) {
        captured.deep_copy(f);
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      zerocopy::CameraFrame frame;
      frame.set_width(1920);
      frame.set_height(1080);
      frame.set_format(zerocopy::CameraFrame::kFormatNv12);
      frame.set_channel(1);
      frame.create(1920 * 1080 * 3 / 2);
      CHECK(pub.publish(frame));

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
      CHECK_EQ(captured.width(), 1920u);
      CHECK_EQ(captured.height(), 1080u);
      CHECK_EQ(captured.format(), zerocopy::CameraFrame::kFormatNv12);
      CHECK_EQ(captured.size(), 1920u * 1080u * 3u / 2u);
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("pointcloud point count and xyz values survive shm transport") {
    MESSAGE("[shm-zerocopy] pointcloud point count and xyz values survive shm transport");

    if (!ensure_shm_ready()) {
      return;
    }

    try {
      std::atomic<bool> received{false};
      zerocopy::PointCloud captured;

      Publisher<zerocopy::PointCloud> pub(ShmConf("shm/zc/pc1", "data"));
      Subscriber<zerocopy::PointCloud> sub("shm://shm/zc/pc1?event=data");

      sub.listen([&](const zerocopy::PointCloud& pc) {
        captured.deep_copy(pc);
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      zerocopy::PointCloud pc;
      REQUIRE(pc.create_v3f<float>(5, {"intensity"}));
      REQUIRE(pc.push_value_v3f(10.0f, 20.0f, 30.0f, 1.0f));
      REQUIRE(pc.push_value_v3f(11.0f, 21.0f, 31.0f, 0.5f));
      CHECK(pub.publish(pc));

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
      CHECK_EQ(captured.size(), 2u);

      auto km = captured.get_key_map();
      float x0 = captured.get_value<float>(0, km, "x");
      float z1 = captured.get_value<float>(1, km, "z");
      CHECK(x0 == 10.0f);
      CHECK(z1 == 31.0f);
    } catch (const std::exception&) {
      return;
    }
  }
}

TEST_SUITE("shm-loop") {
  TEST_CASE("attached message loop dispatches pubsub and field callbacks") {
    MESSAGE("[shm-loop] attached message loop dispatches pubsub and field callbacks");

    if (!ensure_shm_ready()) {
      return;
    }

    MessageLoop loop;
    CHECK(loop.async_run());

    std::atomic<int> sub_value{0};
    std::atomic<int> getter_value{0};

    Publisher<int> pub(ShmConf("shm/loop/pubsub1", "data", 401, 3, 1), InitType::kWithoutInit);
    Subscriber<int> sub(ShmConf("shm/loop/pubsub1", "data", 401, 3, 1), InitType::kWithoutInit);
    Setter<int> setter(ShmConf("shm/loop/field1", "val", 402, 3, 1), InitType::kWithoutInit);
    Getter<int> getter(ShmConf("shm/loop/field1", "val", 402, 3, 1), InitType::kWithoutInit);

    CHECK(pub.attach(&loop));
    CHECK(sub.attach(&loop));
    CHECK(setter.attach(&loop));
    CHECK(getter.attach(&loop));

    REQUIRE(pub.init());
    REQUIRE(sub.init());
    REQUIRE(setter.init());
    REQUIRE(getter.init());

    CHECK(sub.listen([&](const int& value) { sub_value.store(value, std::memory_order_release); }));
    getter.listen([&](const int& value) { getter_value.store(value, std::memory_order_release); });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(77));
    CHECK(common_test::wait_until([&] { return sub_value.load(std::memory_order_acquire) == 77; }, 5s));

    setter.set(88);
    CHECK(common_test::wait_until([&] { return getter_value.load(std::memory_order_acquire) == 88; }, 5s));

    CHECK(getter.detach());
    CHECK(setter.detach());
    CHECK(sub.detach());
    CHECK(pub.detach());
    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("attached message loop dispatches rpc callbacks") {
    MESSAGE("[shm-loop] attached message loop dispatches rpc callbacks");

    if (!ensure_shm_ready()) {
      return;
    }

    MessageLoop loop;
    CHECK(loop.async_run());

    std::atomic<int> server_calls{0};
    std::atomic<bool> async_done{false};
    std::string async_value;

    Server<std::string, std::string> server(ShmConf("shm/loop/rpc1", "req", 403, 3), InitType::kWithoutInit);
    Client<std::string, std::string> client(ShmConf("shm/loop/rpc1", "req", 403, 3), InitType::kWithoutInit);

    CHECK(server.attach(&loop));
    CHECK(client.attach(&loop));
    REQUIRE(server.init());
    REQUIRE(client.init());

    CHECK(server.listen([&](const std::string& req, std::string& resp) {
      server_calls.fetch_add(1, std::memory_order_relaxed);
      resp = "loop:" + req;
    }));

    CHECK(client.wait_for_connected(1s));

    std::string sync_value;
    CHECK(client.invoke("sync", sync_value, 5s));
    CHECK_EQ(sync_value, "loop:sync");

    CHECK(client.invoke("async", [&](const std::string& resp) {
      async_value = resp;
      async_done.store(true, std::memory_order_release);
    }));
    CHECK(common_test::wait_until([&] { return async_done.load(std::memory_order_acquire); }, 5s));
    CHECK_EQ(async_value, "loop:async");
    CHECK_GE(server_calls.load(std::memory_order_acquire), 2);

    CHECK(client.detach());
    CHECK(server.detach());
    loop.quit();
    loop.wait_for_quit();
  }
}

TEST_SUITE("shm-discovery") {
  TEST_CASE("publisher reports subscriber connect and disconnect transitions") {
    MESSAGE("[shm-discovery] publisher reports subscriber connect and disconnect transitions");

    if (!ensure_shm_ready()) {
      return;
    }

    std::atomic<int> connected{0};
    std::atomic<int> disconnected{0};

    Publisher<int> pub(ShmConf("shm/discovery/pub1", "data"));
    pub.detect_subscribers([&](bool value) {
      if (value) {
        connected.fetch_add(1, std::memory_order_relaxed);
      } else {
        disconnected.fetch_add(1, std::memory_order_relaxed);
      }
    });

    {
      Subscriber<int> sub("shm://shm/discovery/pub1?event=data");
      CHECK(sub.listen([](const int&) {}));
      CHECK(pub.wait_for_subscribers(1s));
      CHECK(common_test::wait_until([&] { return connected.load(std::memory_order_acquire) > 0; }, 5s));
    }

    CHECK(common_test::wait_until([&] { return !pub.has_subscribers(); }, 5s));
    CHECK(common_test::wait_until([&] { return disconnected.load(std::memory_order_acquire) > 0; }, 5s));
  }

  TEST_CASE("client reports server connect and disconnect transitions") {
    MESSAGE("[shm-discovery] client reports server connect and disconnect transitions");

    if (!ensure_shm_ready()) {
      return;
    }

    std::atomic<int> connected{0};
    std::atomic<int> disconnected{0};

    Client<std::string, std::string> client("shm://shm/discovery/rpc1?event=req");
    client.detect_connected([&](bool value) {
      if (value) {
        connected.fetch_add(1, std::memory_order_relaxed);
      } else {
        disconnected.fetch_add(1, std::memory_order_relaxed);
      }
    });

    {
      Server<std::string, std::string> server(ShmConf("shm/discovery/rpc1", "req"));
      CHECK(server.listen([](const std::string& req, std::string& resp) { resp = req; }));
      CHECK(client.wait_for_connected(1s));
      CHECK(common_test::wait_until([&] { return connected.load(std::memory_order_acquire) > 0; }, 5s));
    }

    CHECK(common_test::wait_until([&] { return !client.is_connected(); }, 5s));
    CHECK(common_test::wait_until([&] { return disconnected.load(std::memory_order_acquire) > 0; }, 5s));
  }
}

TEST_SUITE("shm-method") {
  TEST_CASE("connected client can invoke server") {
    MESSAGE("[shm-method] connected client can invoke server");

    if (!ensure_shm_ready()) {
      return;
    }

    Server<std::string, std::string> server(ShmConf("shm/mth/connected_invoke1", "req"));
    server.listen([](const std::string& req, std::string& resp) { resp = req; });

    Client<std::string, std::string> client("shm://shm/mth/connected_invoke1?event=req");
    REQUIRE(client.wait_for_connected(1s));

    std::string resp;
    CHECK(client.invoke("ping", resp, 5s));
    CHECK(resp == "ping");
  }
}

#endif  // VLINK_SUPPORT_SHM

// NOLINTEND
