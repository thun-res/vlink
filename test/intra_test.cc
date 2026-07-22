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

#ifdef VLINK_SUPPORT_INTRA

#include <atomic>
#include <future>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "./common_test.h"
#include "./modules/intra_conf.h"

TEST_SUITE("intra-init") {
  TEST_CASE("valid url parses for all impl types") {
    MESSAGE("[intra-init] valid url parses for all impl types");

    Url url("intra://test1?event=hi");

    CHECK(url.parse(kPublisher));
    CHECK(url.parse(kSubscriber));
    CHECK(url.parse(kServer));
    CHECK(url.parse(kClient));
  }

  TEST_CASE("unknown impl type throws on parse") {
    MESSAGE("[intra-init] unknown impl type throws on parse");

    Url url("intra://test1?event=hi");

    CHECK_THROWS_AS(url.parse(kUnknownImplType), std::runtime_error);
  }

  TEST_CASE("conf parses successfully for all impl types") {
    MESSAGE("[intra-init] conf parses successfully for all impl types");

    IntraConf conf("test1", "hi");

    CHECK(conf.parse(kPublisher));
    CHECK(conf.parse(kSubscriber));
    CHECK(conf.parse(kServer));
    CHECK(conf.parse(kClient));
  }

  TEST_CASE("conf parse accepts setter and getter impl types at runtime") {
    MESSAGE("[intra-init] conf parse accepts setter and getter impl types at runtime");

    IntraConf conf("test1", "hi");

    CHECK(conf.parse(kSetter));
    CHECK(conf.parse(kGetter));
  }

  TEST_CASE("conf equality compares all fields") {
    MESSAGE("[intra-init] conf equality compares all fields");

    IntraConf a("addr1", "ev1", 0, "queue");
    IntraConf b("addr1", "ev1", 0, "queue");
    IntraConf c("addr2", "ev1", 0, "queue");

    CHECK(a == b);
    CHECK(a != c);
  }

  TEST_CASE("conf transport type is intra") {
    MESSAGE("[intra-init] conf transport type is intra");

    IntraConf conf("addr1");

    CHECK(conf.get_transport_type() == TransportType::kIntra);
  }

  TEST_CASE("invalid url scheme throws on every public role construction") {
    MESSAGE("[intra-init] invalid url scheme throws on every public role construction");

    CHECK_THROWS(Publisher<int>("intra1://test1/event=hi"));
    CHECK_THROWS(Subscriber<int>("intra1://test1/event=hi"));
    CHECK_THROWS(Setter<int>("intra1://test1/event=hi"));
    CHECK_THROWS(Getter<int>("intra1://test1/event=hi"));
    CHECK_THROWS(Server<int>("intra1://test1/event=hi"));
    CHECK_THROWS(Server<int, int>("intra1://test1/event=hi"));
    CHECK_THROWS(Client<int>("intra1://test1/event=hi"));
    CHECK_THROWS(Client<int, int>("intra1://test1/event=hi"));
  }

  TEST_CASE("malformed url scheme throws on subscriber construction") {
    MESSAGE("[intra-init] malformed url scheme throws on subscriber construction");

    CHECK_THROWS(Subscriber<int>("intra2:/event"));
  }

  TEST_CASE("direct fragment url parses for publisher and subscriber") {
    MESSAGE("[intra-init] direct fragment url parses for publisher and subscriber");

    Url url("intra://topic#direct");

    CHECK(url.parse(kPublisher));
    CHECK(url.parse(kSubscriber));
  }

  TEST_CASE("conf with pipeline and direct type stores all fields") {
    MESSAGE("[intra-init] conf with pipeline and direct type stores all fields");

    IntraConf conf("pipeline_addr", "ev", 8, "direct");

    CHECK(conf.address == "pipeline_addr");
    CHECK(conf.event == "ev");
    CHECK(conf.pipeline == 8);
    CHECK(conf.type == "direct");
  }

  TEST_CASE("conf rejects invalid protocol state and preserves stream flags") {
    MESSAGE("[intra-init] conf rejects invalid protocol state and preserves stream flags");

    IntraConf::global_init();

    IntraConf empty_address;
    CHECK_FALSE(empty_address.is_valid());

    IntraConf invalid_type("bad_type_addr", "ev", 0, "broadcast");
    CHECK_FALSE(invalid_type.is_valid());

    IntraConf conf("stream_addr", "stream_event", 2, "direct");
    std::ostringstream out;
    out << std::hex << conf;

    const bool preserved_hex_flag = (out.flags() & std::ios_base::basefield) == std::ios_base::hex;
    CHECK(preserved_hex_flag);
    CHECK(out.str().find("stream_addr") != std::string::npos);
    CHECK(out.str().find("stream_event") != std::string::npos);
  }

  TEST_CASE("fire and forget server and client report matching ser and schema type") {
    MESSAGE("[intra-init] fire and forget server and client report matching ser and schema type");

    Server<std::string> server(IntraConf("meta_send", "null"), InitType::kWithoutInit);
    Client<std::string> client("intra://meta_send?event=null", InitType::kWithoutInit);

    CHECK(server.get_ser_type() == "string");
    CHECK(client.get_ser_type() == "string");
    CHECK(server.get_schema_type() == SchemaType::kRaw);
    CHECK(client.get_schema_type() == SchemaType::kRaw);
  }

  TEST_CASE("request response server and client report matching paired ser type") {
    MESSAGE("[intra-init] request response server and client report matching paired ser type");

    Server<std::string, std::string> server(IntraConf("meta_rpc", "null"), InitType::kWithoutInit);
    Client<std::string, std::string> client("intra://meta_rpc?event=null", InitType::kWithoutInit);

    CHECK(server.get_ser_type() == "string|string");
    CHECK(client.get_ser_type() == "string|string");
    CHECK(server.get_schema_type() == SchemaType::kRaw);
    CHECK(client.get_schema_type() == SchemaType::kRaw);
  }

  TEST_CASE("bytes request response reports empty ser type") {
    MESSAGE("[intra-init] bytes request response reports empty ser type");

    Server<Bytes, Bytes> server(IntraConf("meta_bytes_rpc", "null"), InitType::kWithoutInit);
    Client<Bytes, Bytes> client("intra://meta_bytes_rpc?event=null", InitType::kWithoutInit);

    CHECK(server.get_ser_type().empty());
    CHECK(client.get_ser_type().empty());
    CHECK(server.get_schema_type() == SchemaType::kRaw);
    CHECK(client.get_schema_type() == SchemaType::kRaw);
  }

  TEST_CASE("node factory helpers support explicit init and deinit for every intra role") {
    MESSAGE("[intra-init] node factory helpers support explicit init and deinit for every intra role");

    auto pub = Publisher<int>::create_unique("intra://factory_pub", InitType::kWithoutInit);
    auto sub = Subscriber<int>::create_shared("intra://factory_pub", InitType::kWithoutInit);
    auto server =
        Server<std::string, std::string>::create_unique("intra://factory_rpc?event=null", InitType::kWithoutInit);
    auto client =
        Client<std::string, std::string>::create_shared("intra://factory_rpc?event=null", InitType::kWithoutInit);
    auto setter = Setter<int>::create_unique("intra://factory_field", InitType::kWithoutInit);
    auto getter = Getter<int>::create_shared("intra://factory_field", InitType::kWithoutInit);

    REQUIRE(pub != nullptr);
    REQUIRE(sub != nullptr);
    REQUIRE(server != nullptr);
    REQUIRE(client != nullptr);
    REQUIRE(setter != nullptr);
    REQUIRE(getter != nullptr);

    CHECK_FALSE(pub->has_inited());
    CHECK_FALSE(sub->has_inited());
    CHECK_FALSE(server->has_inited());
    CHECK_FALSE(client->has_inited());
    CHECK_FALSE(setter->has_inited());
    CHECK_FALSE(getter->has_inited());

    CHECK(pub->init());
    CHECK(sub->init());
    CHECK(server->init());
    CHECK(client->init());
    CHECK(setter->init());
    CHECK(getter->init());

    CHECK_FALSE(pub->init());
    CHECK_FALSE(sub->init());
    CHECK_FALSE(server->init());
    CHECK_FALSE(client->init());
    CHECK_FALSE(setter->init());
    CHECK_FALSE(getter->init());

    CHECK(pub->deinit());
    CHECK(sub->deinit());
    CHECK(server->deinit());
    CHECK(client->deinit());
    CHECK(setter->deinit());
    CHECK(getter->deinit());

    CHECK_FALSE(pub->deinit());
    CHECK_FALSE(sub->deinit());
    CHECK_FALSE(server->deinit());
    CHECK_FALSE(client->deinit());
    CHECK_FALSE(setter->deinit());
    CHECK_FALSE(getter->deinit());
  }

  TEST_CASE("public listeners reject calls before init and duplicate registrations") {
    MESSAGE("[intra-init] public listeners reject calls before init and duplicate registrations");

    Subscriber<int> sub(IntraConf("guard_subscriber", "event"), InitType::kWithoutInit);
    CHECK_THROWS(sub.listen([](const int&) {}));
    REQUIRE(sub.init());
    CHECK(sub.listen([](const int&) {}));
    CHECK_THROWS(sub.listen([](const int&) {}));

    Server<std::string> fire_and_forget(IntraConf("guard_fire_server", "null"), InitType::kWithoutInit);
    CHECK_THROWS(fire_and_forget.listen([](const std::string&) {}));
    REQUIRE(fire_and_forget.init());
    CHECK(fire_and_forget.listen([](const std::string&) {}));
    CHECK_THROWS(fire_and_forget.listen([](const std::string&) {}));

    Server<std::string, std::string> sync_server(IntraConf("guard_sync_server", "null"), InitType::kWithoutInit);
    CHECK_THROWS(sync_server.listen([](const std::string&, std::string&) {}));
    REQUIRE(sync_server.init());
    CHECK(sync_server.listen([](const std::string& req, std::string& resp) { resp = req; }));
    CHECK_THROWS(sync_server.listen([](const std::string&, std::string&) {}));
    CHECK_THROWS(sync_server.reply(1, std::string("wrong_mode")));

    Server<std::string, std::string> async_server(IntraConf("guard_async_server", "null"), InitType::kWithoutInit);
    CHECK_THROWS(async_server.listen_for_reply([](uint64_t, const std::string&) {}));
    REQUIRE(async_server.init());
    CHECK(async_server.listen_for_reply([](uint64_t, const std::string&) {}));
    CHECK_THROWS(async_server.listen_for_reply([](uint64_t, const std::string&) {}));
    CHECK_FALSE(async_server.reply(777, std::string("no_request")));
  }

  TEST_CASE("client zero timeout branches remain usable when the intra server is connected") {
    MESSAGE("[intra-init] client zero timeout branches remain usable when the intra server is connected");

    Server<std::string, std::string> server(IntraConf("factory_zero_timeout", "null"));
    Client<std::string, std::string> client("intra://factory_zero_timeout?event=null");

    REQUIRE(server.listen([](const std::string& req, std::string& resp) { resp = req + ":ok"; }));
    REQUIRE(client.wait_for_connected(1s));
    CHECK(client.wait_for_connected(0ms));

    std::string response;
    REQUIRE(client.invoke(std::string("ping"), response, 0ms));
    CHECK_EQ(response, "ping:ok");

    auto optional_response = client.invoke(std::string("pong"), 0ms);
    REQUIRE(optional_response.has_value());
    CHECK_EQ(optional_response.value(), "pong:ok");
  }

  TEST_CASE("set ser type keeps schema family in sync") {
    MESSAGE("[intra-init] set ser type keeps schema family in sync");

    Publisher<Bytes> pub(IntraConf("meta_override", "null"), InitType::kWithoutInit);

    pub.set_ser_type("demo.proto.Old", SchemaType::kProtobuf);
    CHECK(pub.get_schema_type() == SchemaType::kProtobuf);

    pub.set_ser_type("demo.proto.New");
    CHECK(pub.get_schema_type() == SchemaType::kProtobuf);

    pub.set_ser_type("vlink::zerocopy::RawData");
    CHECK(pub.get_schema_type() == SchemaType::kZeroCopy);

    pub.set_ser_type("demo.custom.Payload");
    CHECK(pub.get_schema_type() == SchemaType::kUnknown);

    pub.set_ser_type("string");
    CHECK(pub.get_schema_type() == SchemaType::kRaw);
  }
}

TEST_SUITE("intra-pubsub") {
  TEST_CASE("queue mode delivers message to single subscriber") {
    MESSAGE("[intra-pubsub] queue mode delivers message to single subscriber");

    std::atomic<int> count{0};

    Publisher<std::string> pub(IntraConf("event_q", "", 0, "queue"));
    Subscriber<std::string> sub("intra://event_q");

    sub.listen([&](const std::string& /*msg*/) { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.has_subscribers());
    CHECK(pub.publish("msg1"));

    std::this_thread::sleep_for(20ms);
    CHECK(count.load() == 1);

    pub.publish("msg2");
    pub.publish("msg3");
    std::this_thread::sleep_for(20ms);

    CHECK(count.load() == 3);
  }

  TEST_CASE("direct mode delivers message to subscriber") {
    MESSAGE("[intra-pubsub] direct mode delivers message to subscriber");

    std::atomic<int> count{0};

    Publisher<std::string> pub(IntraConf("event_d", "", 0, "direct"));
    Subscriber<std::string> sub("intra://event_d#direct");

    sub.listen([&](const std::string& /*msg*/) { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish("direct_msg"));

    std::this_thread::sleep_for(20ms);
    CHECK(count.load() == 1);
  }

  TEST_CASE("direct callback may deinitialize its subscriber") {
    MESSAGE("[intra-pubsub] direct callback may deinitialize its subscriber");

    Publisher<int> pub(IntraConf("event_direct_reentrant_deinit", "", 0, "direct"));
    Subscriber<int> deinit_sub("intra://event_direct_reentrant_deinit#direct");
    Subscriber<int> remaining_sub("intra://event_direct_reentrant_deinit#direct");
    bool deinit_callback_called = false;
    bool deinit_succeeded = false;
    int remaining_callback_count = 0;

    deinit_sub.listen([&](const int&) {
      deinit_callback_called = true;
      deinit_succeeded = deinit_sub.deinit();
    });
    remaining_sub.listen([&](const int&) { ++remaining_callback_count; });

    CHECK(pub.publish(1));
    CHECK(deinit_callback_called);
    CHECK(deinit_succeeded);
    CHECK(remaining_callback_count == 1);

    CHECK(pub.publish(2));
    CHECK(remaining_callback_count == 2);
  }

  TEST_CASE("stateful direct callback keeps its stored state") {
    MESSAGE("[intra-pubsub] stateful direct callback keeps its stored state");

    Publisher<int> pub(IntraConf("event_direct_stateful_callback", "", 0, "direct"));
    Subscriber<int> sub("intra://event_direct_stateful_callback#direct");
    int observed_count = 0;

    sub.listen([count = 0, &observed_count](const int&) mutable { observed_count = ++count; });

    CHECK(pub.publish(1));
    CHECK_EQ(observed_count, 1);
    CHECK(pub.publish(2));
    CHECK_EQ(observed_count, 2);
  }

  TEST_CASE("nested direct delivery preserves the outer callback value") {
    MESSAGE("[intra-pubsub] nested direct delivery preserves the outer callback value");

    Publisher<int> outer_pub(IntraConf("event_direct_nested_outer", "", 0, "direct"));
    Publisher<int> inner_pub(IntraConf("event_direct_nested_inner", "", 0, "direct"));
    Subscriber<int> outer_sub("intra://event_direct_nested_outer#direct");
    Subscriber<int> inner_sub("intra://event_direct_nested_inner#direct");
    int outer_value_after_nested_delivery = 0;
    int inner_value = 0;

    inner_sub.listen([&](const int& value) { inner_value = value; });
    outer_sub.listen([&](const int& value) {
      CHECK(inner_pub.publish(22));
      outer_value_after_nested_delivery = value;
    });

    CHECK(outer_pub.publish(11));
    CHECK(inner_value == 22);
    CHECK(outer_value_after_nested_delivery == 11);
  }

  TEST_CASE("multiple subscribers each receive the published message") {
    MESSAGE("[intra-pubsub] multiple subscribers each receive the published message");

    std::atomic<int> count1{0};
    std::atomic<int> count2{0};

    Publisher<std::string> pub(IntraConf("event_multi", "", 0, "queue"));
    Subscriber<std::string> sub1("intra://event_multi");
    Subscriber<std::string> sub2("intra://event_multi");

    sub1.listen([&](const std::string& /*msg*/) { count1.fetch_add(1, std::memory_order_relaxed); });
    sub2.listen([&](const std::string& /*msg*/) { count2.fetch_add(1, std::memory_order_relaxed); });

    for (int i = 0; i < 50; ++i) {
      if (pub.has_subscribers()) {
        break;
      }

      std::this_thread::sleep_for(20ms);
    }

    CHECK(pub.publish("broadcast"));
    std::this_thread::sleep_for(20ms);

    CHECK(count1.load() == 1);
    CHECK(count2.load() == 1);
  }

  TEST_CASE("force publish succeeds without any subscriber") {
    MESSAGE("[intra-pubsub] force publish succeeds without any subscriber");

    Publisher<std::string> pub(IntraConf("event_force", "", 0, "queue"));

    for (int i = 0; i < 5; ++i) {
      CHECK(pub.publish("force_" + std::to_string(i), true));
    }
  }

  TEST_CASE("direct publish without subscriber reports no delivery") {
    MESSAGE("[intra-pubsub] direct publish without subscriber reports no delivery");

    Publisher<int> pub(IntraConf("event_no_subscriber", "", 0, "direct"));

    CHECK_FALSE(pub.has_subscribers());
    CHECK_FALSE(pub.wait_for_subscribers(5ms));
    CHECK_FALSE(pub.publish(7));
    CHECK_FALSE(pub.publish(7, true));
  }

  TEST_CASE("pipeline mode delivers messages to subscriber") {
    MESSAGE("[intra-pubsub] pipeline mode delivers messages to subscriber");

    std::atomic<int> count{0};

    Publisher<int> pub(IntraConf("event_pipeline1", "", 4, "queue"));
    Subscriber<int> sub("intra://event_pipeline1?pipeline=4");

    sub.listen([&](const int& /*val*/) { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 8; ++i) {
      pub.publish(i);
    }

    std::this_thread::sleep_for(20ms);
    CHECK(count.load() > 0);
  }

  TEST_CASE("bytes payload is received intact") {
    MESSAGE("[intra-pubsub] bytes payload is received intact");

    std::atomic<bool> received{false};
    Bytes captured;

    Publisher<Bytes> pub(IntraConf("event_bytes1", "", 0, "queue"));
    Subscriber<Bytes> sub("intra://event_bytes1");

    sub.listen([&](const Bytes& data) {
      captured = data;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(Bytes{1, 2, 3}));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
    REQUIRE(captured.size() == 3u);
    CHECK(captured[0] == 1);
    CHECK(captured[1] == 2);
    CHECK(captured[2] == 3);
  }

  TEST_CASE("integer payload is received with correct value") {
    MESSAGE("[intra-pubsub] integer payload is received with correct value");

    std::atomic<bool> received{false};
    std::atomic<int> captured{0};

    Publisher<int> pub(IntraConf("event_int1", "", 0, "queue"));
    Subscriber<int> sub("intra://event_int1");

    sub.listen([&](const int& val) {
      captured.store(val, std::memory_order_relaxed);
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(42));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
    CHECK(captured.load() == 42);

    SUBCASE("subsequent publishes deliver updated values") {
      for (int v : {100, 200, 300}) {
        received.store(false, std::memory_order_release);
        pub.publish(v);

        CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
        CHECK(captured.load() == v);
      }
    }
  }

  TEST_CASE("string payload is received with correct value") {
    MESSAGE("[intra-pubsub] string payload is received with correct value");

    std::atomic<bool> received{false};
    std::string captured;

    Publisher<std::string> pub(IntraConf("event_str1", "", 0, "queue"));
    Subscriber<std::string> sub("intra://event_str1");

    sub.listen([&](const std::string& val) {
      captured = val;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(std::string("hello_world")));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
    CHECK(captured == "hello_world");
  }

  TEST_CASE("subscriber connect and disconnect events are detected") {
    MESSAGE("[intra-pubsub] subscriber connect and disconnect events are detected");

    std::atomic<int> detected_count{0};

    Publisher<int> pub(IntraConf("event_detect1", "", 0, "queue"));

    pub.detect_subscribers([&](bool connected) {
      if (connected) {
        detected_count.fetch_add(1, std::memory_order_relaxed);
      }
    });

    {
      Subscriber<int> sub("intra://event_detect1");
      sub.listen([](const int& /*v*/) {});
      std::this_thread::sleep_for(20ms);
      CHECK(pub.has_subscribers());
    }

    std::this_thread::sleep_for(20ms);
    CHECK(!pub.has_subscribers());
  }

  TEST_CASE("subscriber unlisten stops later callbacks") {
    MESSAGE("[intra-pubsub] subscriber unlisten stops later callbacks");

    std::atomic<int> count{0};

    Publisher<int> pub(IntraConf("event_unlisten1", "", 0, "queue"));
    Subscriber<int> sub("intra://event_unlisten1");

    CHECK(sub.listen([&](const int&) { count.fetch_add(1, std::memory_order_relaxed); }));
    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(1));
    CHECK(common_test::wait_until([&count] { return count.load(std::memory_order_relaxed) == 1; }, 1s));

    CHECK(sub.deinit());
    CHECK_FALSE(sub.has_inited());
    CHECK(pub.publish(2, true));
    std::this_thread::sleep_for(20ms);
    CHECK_EQ(count.load(std::memory_order_relaxed), 1);
  }

  TEST_CASE("node metadata and loan fallbacks are stable for intra pubsub") {
    MESSAGE("[intra-pubsub] node metadata and loan fallbacks are stable for intra pubsub");

    Publisher<Bytes> pub(IntraConf("event_meta_loan", "", 0, "queue"));
    Subscriber<Bytes> sub("intra://event_meta_loan");

    CHECK(pub.get_transport_type() == TransportType::kIntra);
    CHECK(sub.get_transport_type() == TransportType::kIntra);
    CHECK(pub.get_abstract_node() != nullptr);
    CHECK(sub.get_abstract_node() != nullptr);
    CHECK_FALSE(pub.is_support_loan());
    CHECK_FALSE(sub.is_support_loan());
    CHECK(pub.loan(16).empty());
    CHECK_FALSE(pub.return_loan(Bytes{1, 2, 3}));
    CHECK_FALSE(sub.return_loan(Bytes{1, 2, 3}));
    CHECK_FALSE(pub.attach(nullptr));
    CHECK_FALSE(pub.detach());
    CHECK_FALSE(sub.attach(nullptr));
    CHECK_FALSE(sub.detach());
  }

  TEST_CASE("direct subscriber suspend filters delivery until resume") {
    MESSAGE("[intra-pubsub] direct subscriber suspend filters delivery until resume");

    std::atomic<int> count{0};

    Publisher<int> pub(IntraConf("event_direct_suspend", "", 0, "direct"));
    Subscriber<int> sub("intra://event_direct_suspend#direct");

    CHECK(sub.listen([&](const int&) { count.fetch_add(1, std::memory_order_relaxed); }));
    CHECK(pub.wait_for_subscribers(1s));

    CHECK(sub.suspend());
    CHECK(sub.is_suspend());
    CHECK_FALSE(pub.publish(1));
    CHECK_EQ(count.load(std::memory_order_relaxed), 0);

    CHECK(sub.resume());
    CHECK_FALSE(sub.is_suspend());
    CHECK(pub.publish(2));
    CHECK_EQ(count.load(std::memory_order_relaxed), 1);
  }

  TEST_CASE("queue subscriber suspend filters delivery until resume") {
    MESSAGE("[intra-pubsub] queue subscriber suspend filters delivery until resume");

    std::atomic<int> suspended_count{0};
    std::atomic<int> active_count{0};

    Publisher<int> pub(IntraConf("event_queue_suspend", "", 0, "queue"));
    Subscriber<int> suspended("intra://event_queue_suspend");
    Subscriber<int> active("intra://event_queue_suspend");

    CHECK(suspended.listen([&](const int&) { suspended_count.fetch_add(1, std::memory_order_release); }));
    CHECK(active.listen([&](const int&) { active_count.fetch_add(1, std::memory_order_release); }));
    CHECK(pub.wait_for_subscribers(1s));

    CHECK(suspended.suspend());
    CHECK(suspended.is_suspend());
    CHECK(pub.publish(1));
    CHECK(common_test::wait_until([&active_count] { return active_count.load(std::memory_order_acquire) == 1; }, 1s));
    CHECK_EQ(suspended_count.load(std::memory_order_acquire), 0);

    CHECK(suspended.resume());
    CHECK_FALSE(suspended.is_suspend());
    CHECK(pub.publish(2));
    CHECK(common_test::wait_until(
        [&] {
          return active_count.load(std::memory_order_acquire) == 2 &&
                 suspended_count.load(std::memory_order_acquire) == 1;
        },
        1s));
  }

  TEST_CASE("queue publish task is discarded safely after publisher destruction") {
    MESSAGE("[intra-pubsub] queue publish task is discarded safely after publisher destruction");

    std::atomic<bool> blocker_entered{false};
    std::atomic<bool> release_blocker{false};
    std::atomic<bool> sentinel_received{false};

    Publisher<int> blocker_pub(IntraConf("event_queue_destroy_blocker", "", 0, "queue"));
    Subscriber<int> blocker_sub("intra://event_queue_destroy_blocker");
    CHECK(blocker_sub.listen([&](const int&) {
      blocker_entered.store(true, std::memory_order_release);
      while (!release_blocker.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(1ms);
      }
    }));
    CHECK(blocker_pub.wait_for_subscribers(1s));
    CHECK(blocker_pub.publish(1));
    CHECK(common_test::wait_until([&blocker_entered] { return blocker_entered.load(std::memory_order_acquire); }, 1s));

    {
      Publisher<int> transient_pub(IntraConf("event_queue_destroy_transient", "", 0, "queue"));
      CHECK(transient_pub.publish(42, true));
    }

    Publisher<int> sentinel_pub(IntraConf("event_queue_destroy_sentinel", "", 0, "queue"));
    Subscriber<int> sentinel_sub("intra://event_queue_destroy_sentinel");
    CHECK(sentinel_sub.listen([&](const int&) { sentinel_received.store(true, std::memory_order_release); }));
    CHECK(sentinel_pub.wait_for_subscribers(1s));

    release_blocker.store(true, std::memory_order_release);
    CHECK(sentinel_pub.publish(7));
    CHECK(common_test::wait_until([&sentinel_received] { return sentinel_received.load(std::memory_order_acquire); },
                                  1s));
  }

  TEST_CASE("same address different events are isolated in direct pubsub") {
    MESSAGE("[intra-pubsub] same address different events are isolated in direct pubsub");

    std::atomic<int> a_count{0};
    std::atomic<int> b_count{0};
    std::atomic<int> a_last{0};
    std::atomic<int> b_last{0};

    Publisher<int> pub_a(IntraConf("event_filter_direct", "a", 0, "direct"));
    Publisher<int> pub_b(IntraConf("event_filter_direct", "b", 0, "direct"));
    Subscriber<int> sub_a("intra://event_filter_direct?event=a#direct");
    Subscriber<int> sub_b("intra://event_filter_direct?event=b#direct");

    CHECK(sub_a.listen([&](const int& value) {
      a_last.store(value, std::memory_order_relaxed);
      a_count.fetch_add(1, std::memory_order_relaxed);
    }));
    CHECK(sub_b.listen([&](const int& value) {
      b_last.store(value, std::memory_order_relaxed);
      b_count.fetch_add(1, std::memory_order_relaxed);
    }));

    CHECK(pub_a.wait_for_subscribers(1s));
    CHECK(pub_b.wait_for_subscribers(1s));
    CHECK(pub_a.publish(11));
    CHECK(pub_b.publish(22));

    CHECK_EQ(a_count.load(std::memory_order_relaxed), 1);
    CHECK_EQ(b_count.load(std::memory_order_relaxed), 1);
    CHECK_EQ(a_last.load(std::memory_order_relaxed), 11);
    CHECK_EQ(b_last.load(std::memory_order_relaxed), 22);
  }

  TEST_CASE("same address different events are isolated in queued pubsub") {
    MESSAGE("[intra-pubsub] same address different events are isolated in queued pubsub");

    std::atomic<int> a_count{0};
    std::atomic<int> b_count{0};

    Publisher<int> pub_a(IntraConf("event_filter_queue", "a", 0, "queue"));
    Publisher<int> pub_b(IntraConf("event_filter_queue", "b", 0, "queue"));
    Subscriber<int> sub_a("intra://event_filter_queue?event=a");
    Subscriber<int> sub_b("intra://event_filter_queue?event=b");

    CHECK(sub_a.listen([&](const int&) { a_count.fetch_add(1, std::memory_order_release); }));
    CHECK(sub_b.listen([&](const int&) { b_count.fetch_add(1, std::memory_order_release); }));
    CHECK(pub_a.wait_for_subscribers(1s));
    CHECK(pub_b.wait_for_subscribers(1s));

    CHECK(pub_a.publish(1));
    CHECK(pub_b.publish(2));

    CHECK(common_test::wait_until(
        [&] { return a_count.load(std::memory_order_acquire) == 1 && b_count.load(std::memory_order_acquire) == 1; },
        1s));
  }

  TEST_CASE("invalid raw bytes are dropped before typed subscriber callback") {
    MESSAGE("[intra-pubsub] invalid raw bytes are dropped before typed subscriber callback");

    std::atomic<int> delivered{0};

    Publisher<Bytes> pub(IntraConf("event_bad_typed_bytes", "", 0, "direct"));
    Subscriber<int> sub("intra://event_bad_typed_bytes#direct");

    CHECK(sub.listen([&](const int&) { delivered.fetch_add(1, std::memory_order_relaxed); }));
    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(Bytes{0x01}));

    CHECK_EQ(delivered.load(std::memory_order_relaxed), 0);
  }

  TEST_CASE("serialization round trip succeeds for available message types") {
    MESSAGE("[intra-pubsub] serialization round trip succeeds for available message types");

#if defined(VLINK_TEST_SUPPORT_PROTOBUF)
    SUBCASE("protobuf message round trip") {
      std::atomic<bool> received{false};
      pb::Message captured;

      Publisher<pb::Message> pub(IntraConf("event_pb_msg", "", 0, "queue"));
      Subscriber<pb::Message> sub("intra://event_pb_msg");

      sub.listen([&](const pb::Message& msg) {
        captured = msg;
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));

      pb::Message msg;
      msg.set_value("proto_test");
      CHECK(pub.publish(msg));

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
      CHECK(captured.value() == "proto_test");
    }
#endif

#if defined(VLINK_TEST_SUPPORT_FLATBUFFERS)
    SUBCASE("flatbuffers message round trip") {
      std::atomic<bool> received{false};

      Publisher<fbs::MessageT> pub(IntraConf("event_fbs_msg", "", 0, "queue"));
      Subscriber<fbs::MessageT> sub("intra://event_fbs_msg");

      sub.listen([&](const fbs::MessageT& /*msg*/) { received.store(true, std::memory_order_release); });

      CHECK(pub.wait_for_subscribers(1s));

      fbs::MessageT msg;
      msg.value = "flatbuffers_test";
      CHECK(pub.publish(msg));

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
    }
#endif

    SUBCASE("plain int always works") {
      std::atomic<bool> received{false};
      std::atomic<int> val{0};

      Publisher<int> pub(IntraConf("event_ser_int1", "", 0, "queue"));
      Subscriber<int> sub("intra://event_ser_int1");

      sub.listen([&](const int& v) {
        val.store(v, std::memory_order_relaxed);
        received.store(true, std::memory_order_release);
      });

      CHECK(pub.wait_for_subscribers(1s));
      pub.publish(99);

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
      CHECK(val.load() == 99);
    }
  }
}

TEST_SUITE("intra-method") {
  TEST_CASE("fire and forget send increments server receive counter") {
    MESSAGE("[intra-method] fire and forget send increments server receive counter");

    std::atomic<int> counter{0};

    Server<std::string> server(IntraConf("test_send1", "null"));
    server.listen([&](const std::string& /*req*/) { counter.fetch_add(1, std::memory_order_relaxed); });

    Client<std::string> client("intra://test_send1?event=null");

    CHECK(client.wait_for_connected(1s));
    CHECK(client.is_connected());
    CHECK(client.send("send"));

    std::this_thread::sleep_for(20ms);
    CHECK(counter.load() == 1);

    CHECK(client.send("send2"));
    std::this_thread::sleep_for(20ms);
    CHECK(counter.load() == 2);
  }

  TEST_CASE("invoke returns correct response via multiple overloads") {
    MESSAGE("[intra-method] invoke returns correct response via multiple overloads");

    Server<std::string, std::string> server(IntraConf("test_invoke2", "null"));
    server.listen([](const std::string& /*req*/, std::string& resp) { resp = "response"; });

    Client<std::string, std::string> client("intra://test_invoke2?event=null");
    CHECK(client.wait_for_connected(1s));

    SUBCASE("future based async") {
      auto fut = client.async_invoke("invoke");

      REQUIRE(fut.wait_for(5s) == std::future_status::ready);
      CHECK(fut.get() == "response");
    }

    SUBCASE("optional synchronous") {
      auto resp = client.invoke("invoke");

      CHECK(resp.has_value());
      CHECK(*resp == "response");
    }

    SUBCASE("synchronous with ref out param") {
      std::string out;
      bool ok = client.invoke("invoke", out, 5s);

      CHECK(ok);
      CHECK(out == "response");
    }

    SUBCASE("multiple sequential invocations succeed") {
      for (int i = 0; i < 5; ++i) {
        auto resp = client.invoke("invoke");

        CHECK(resp.has_value());
        CHECK(*resp == "response");
      }
    }
  }

  TEST_CASE("invoke returns empty when response type is mismatched") {
    MESSAGE("[intra-method] invoke returns empty when response type is mismatched");

    Server<Bytes, Bytes> server(IntraConf("test_bad_resp", "null"));
    server.listen([](const Bytes& /*req*/, Bytes& resp) { resp = Bytes{0x01}; });

    Client<int, int> client("intra://test_bad_resp?event=null");
    CHECK(client.wait_for_connected(1s));

    int out = 1234;
    CHECK_FALSE(client.invoke(7, out, 5s));
    CHECK(out == 1234);

    auto resp = client.invoke(7, 5s);
    CHECK_FALSE(resp.has_value());
  }

  TEST_CASE("deferred reply is not supported for intra server") {
    MESSAGE("[intra-method] deferred reply is not supported for intra server");

    Server<std::string, std::string> server(IntraConf("test_async_reply3", "null"));
    server.listen([](const std::string& /*req*/, std::string& resp) { resp = "sync_response"; });

    Client<std::string, std::string> client("intra://test_async_reply3?event=null");
    CHECK(client.wait_for_connected(1s));

    auto resp = client.invoke("request");
    CHECK(resp.has_value());
    CHECK(*resp == "sync_response");

    Server<std::string, std::string> server2(IntraConf("test_async_reply3b", "null"));
    server2.listen_for_reply([](uint64_t, const std::string&) {});
    CHECK_FALSE(server2.reply(1, std::string("x")));
  }

  TEST_CASE("async callback invoke delivers response") {
    MESSAGE("[intra-method] async callback invoke delivers response");

    Server<std::string, std::string> server(IntraConf("test_async_cb4", "null"));
    server.listen([](const std::string& /*req*/, std::string& resp) { resp = "cb_response"; });

    Client<std::string, std::string> client("intra://test_async_cb4?event=null");
    CHECK(client.wait_for_connected(1s));

    std::atomic<bool> got_response{false};
    std::string received_resp;

    bool ok = client.invoke("msg", [&](const std::string& resp) {
      received_resp = resp;
      got_response.store(true, std::memory_order_release);
    });

    CHECK(ok);

    CHECK(common_test::wait_until([&got_response] { return got_response.load(std::memory_order_acquire); }, 1s));
    CHECK(received_resp == "cb_response");
  }

  TEST_CASE("detect connected callback fires when client connects to server") {
    MESSAGE("[intra-method] detect connected callback fires when client connects to server");

    std::atomic<bool> connected_event{false};

    Server<std::string, std::string> server(IntraConf("test_detect_conn", "null"));
    server.listen([](const std::string& /*req*/, std::string& resp) { resp = "ok"; });

    Client<std::string, std::string> client("intra://test_detect_conn?event=null");
    client.detect_connected([&](bool connected) {
      if (connected) {
        connected_event.store(true, std::memory_order_release);
      }
    });

    CHECK(common_test::wait_until([&connected_event] { return connected_event.load(std::memory_order_acquire); }, 1s));
  }

  TEST_CASE("client without a server reports connection state and rpc failure") {
    MESSAGE("[intra-method] client without a server reports connection state and rpc failure");

    Client<std::string> fire_and_forget("intra://missing_fire_and_forget?event=null");
    CHECK_FALSE(fire_and_forget.is_connected());
    CHECK_FALSE(fire_and_forget.wait_for_connected(5ms));
    CHECK(fire_and_forget.send("drop"));

    Client<std::string, std::string> client("intra://missing_rpc?event=null");
    std::string out = "unchanged";
    CHECK_FALSE(client.is_connected());
    CHECK_FALSE(client.invoke("req", out, 5ms));
    CHECK_EQ(out, "unchanged");
    CHECK_FALSE(client.invoke("req", 5ms).has_value());
  }

  TEST_CASE("server and client lifecycle deinit updates connection state") {
    MESSAGE("[intra-method] server and client lifecycle deinit updates connection state");

    Server<std::string, std::string> server(IntraConf("test_lifecycle_deinit", "null"));
    server.listen([](const std::string& req, std::string& resp) { resp = req; });

    Client<std::string, std::string> client("intra://test_lifecycle_deinit?event=null");
    CHECK(client.wait_for_connected(1s));
    CHECK(client.is_connected());
    CHECK(server.get_abstract_node() != nullptr);
    CHECK(client.get_abstract_node() != nullptr);
    CHECK_FALSE(server.return_loan(Bytes{1}));
    CHECK_FALSE(client.return_loan(Bytes{1}));

    CHECK(server.deinit());
    CHECK_FALSE(server.has_inited());
    CHECK_FALSE(client.wait_for_connected(5ms));
  }

  TEST_CASE("direct server suspend blocks rpc until resume") {
    MESSAGE("[intra-method] direct server suspend blocks rpc until resume");

    Server<std::string, std::string> server(IntraConf("test_direct_rpc_suspend", "null", 0, "direct"));
    server.listen([](const std::string& req, std::string& resp) { resp = req + "_resp"; });

    Client<std::string, std::string> client(IntraConf("test_direct_rpc_suspend", "null", 0, "direct"));
    CHECK(client.wait_for_connected(1s));
    CHECK(client.is_connected());
    CHECK_FALSE(server.attach(nullptr));
    CHECK_FALSE(server.detach());
    CHECK_FALSE(client.attach(nullptr));
    CHECK_FALSE(client.detach());

    CHECK(server.suspend());
    CHECK(server.is_suspend());
    std::string out = "unchanged";
    CHECK_FALSE(client.invoke("blocked", out, 20ms));
    CHECK_EQ(out, "unchanged");

    CHECK(server.resume());
    CHECK_FALSE(server.is_suspend());
    CHECK(client.invoke("ok", out, 1s));
    CHECK_EQ(out, "ok_resp");
  }

  TEST_CASE("nested direct client callback preserves the outer response") {
    MESSAGE("[intra-method] nested direct client callback preserves the outer response");

    Server<int, int> outer_server(IntraConf("test_direct_nested_client_outer", "null", 0, "direct"));
    Server<int, int> inner_server(IntraConf("test_direct_nested_client_inner", "null", 0, "direct"));
    Client<int, int> outer_client(IntraConf("test_direct_nested_client_outer", "null", 0, "direct"));
    Client<int, int> inner_client(IntraConf("test_direct_nested_client_inner", "null", 0, "direct"));
    int outer_response_after_nested_call = 0;
    int inner_response = 0;

    CHECK(outer_server.listen([](const int& request, int& response) { response = request + 1; }));
    CHECK(inner_server.listen([](const int& request, int& response) { response = request + 1; }));
    CHECK(outer_client.wait_for_connected(1s));
    CHECK(inner_client.wait_for_connected(1s));

    CHECK(outer_client.invoke(10, [&](const int& response) {
      CHECK(inner_client.invoke(20, [&](const int& nested_response) { inner_response = nested_response; }));
      outer_response_after_nested_call = response;
    }));
    CHECK(inner_response == 21);
    CHECK(outer_response_after_nested_call == 11);
  }

  TEST_CASE("nested queue rpc on the same pipeline does not deadlock") {
    MESSAGE("[intra-method] nested queue rpc on the same pipeline does not deadlock");

    Server<int, int> inner_server(IntraConf("test_queue_nested_inner", "null", 17, "queue"));
    Client<int, int> inner_client(IntraConf("test_queue_nested_inner", "null", 17, "queue"));
    Server<int, int> outer_server(IntraConf("test_queue_nested_outer", "null", 17, "queue"));
    Client<int, int> outer_client(IntraConf("test_queue_nested_outer", "null", 17, "queue"));

    CHECK(inner_server.listen([](const int& request, int& response) { response = request + 1; }));
    CHECK(outer_server.listen([&](const int& request, int& response) {
      int inner_response = 0;
      if (inner_client.invoke(request, inner_response, 1s)) {
        response = inner_response + 1;
      }
    }));
    REQUIRE(inner_client.wait_for_connected(1s));
    REQUIRE(outer_client.wait_for_connected(1s));

    int response = 0;
    CHECK(outer_client.invoke(10, response, 1s));
    CHECK_EQ(response, 12);
  }

  TEST_CASE("direct fire and forget calls server without response callback") {
    MESSAGE("[intra-method] direct fire and forget calls server without response callback");

    std::atomic<int> count{0};

    Server<std::string> server(IntraConf("test_direct_send", "null", 0, "direct"));
    server.listen([&](const std::string& req) {
      if (req == "payload") {
        count.fetch_add(1, std::memory_order_relaxed);
      }
    });

    Client<std::string> client(IntraConf("test_direct_send", "null", 0, "direct"));
    CHECK(client.wait_for_connected(1s));
    CHECK(client.send("payload"));
    CHECK_EQ(count.load(std::memory_order_relaxed), 1);
  }

  TEST_CASE("queue fire and forget skips a suspended server and preserves later tasks") {
    MESSAGE("[intra-method] queue fire and forget skips a suspended server and preserves later tasks");

    std::atomic<int> suspended_count{0};
    std::atomic<int> sentinel_count{0};
    std::atomic<int> other_event_count{0};

    Server<std::string> suspended_server(IntraConf("test_queue_fire_suspend", "null", 0, "queue"));
    Server<std::string> other_event_server(IntraConf("test_queue_fire_suspend", "other", 0, "queue"));
    Client<std::string> client(IntraConf("test_queue_fire_suspend", "null", 0, "queue"));
    Server<std::string> sentinel_server(IntraConf("test_queue_fire_sentinel", "null", 0, "queue"));
    Client<std::string> sentinel_client(IntraConf("test_queue_fire_sentinel", "null", 0, "queue"));

    CHECK(
        suspended_server.listen([&](const std::string&) { suspended_count.fetch_add(1, std::memory_order_release); }));
    CHECK(other_event_server.listen(
        [&](const std::string&) { other_event_count.fetch_add(1, std::memory_order_release); }));
    CHECK(sentinel_server.listen([&](const std::string&) { sentinel_count.fetch_add(1, std::memory_order_release); }));
    CHECK(client.wait_for_connected(1s));
    CHECK(sentinel_client.wait_for_connected(1s));

    CHECK(suspended_server.suspend());
    CHECK(client.send("blocked"));
    CHECK(sentinel_client.send("after_blocked"));
    CHECK(
        common_test::wait_until([&sentinel_count] { return sentinel_count.load(std::memory_order_acquire) == 1; }, 1s));
    CHECK_EQ(suspended_count.load(std::memory_order_acquire), 0);
    CHECK_EQ(other_event_count.load(std::memory_order_acquire), 0);

    CHECK(suspended_server.resume());
    CHECK(client.send("resumed"));
    CHECK(sentinel_client.send("after_resumed"));
    CHECK(common_test::wait_until(
        [&] {
          return sentinel_count.load(std::memory_order_acquire) == 2 &&
                 suspended_count.load(std::memory_order_acquire) == 1;
        },
        1s));
    CHECK_EQ(other_event_count.load(std::memory_order_acquire), 0);
  }

#if defined(VLINK_TEST_SUPPORT_PROTOBUF)
  TEST_CASE("protobuf request response invoke returns correct computed value") {
    MESSAGE("[intra-method] protobuf request response invoke returns correct computed value");

    Server<pb::Request, pb::Response> server(IntraConf("test_pb_rpc", "null"));
    server.listen([](const pb::Request& req, pb::Response& resp) { resp.set_value(std::to_string(req.type() * 2)); });

    Client<pb::Request, pb::Response> client("intra://test_pb_rpc?event=null");
    CHECK(client.wait_for_connected(1s));

    pb::Request req;
    req.set_type(21);
    auto resp = client.invoke(req);

    CHECK(resp.has_value());
    CHECK(resp->value() == "42");
  }
#endif
}

TEST_SUITE("intra-field") {
  TEST_CASE("dynamic data string payload round trip") {
    MESSAGE("[intra-field] dynamic data string payload round trip");

    std::atomic<bool> received{false};
    DynamicData captured;

    Publisher<DynamicData> pub(IntraConf("event_dyn_str", "", 0, "queue"));
    Subscriber<DynamicData> sub("intra://event_dyn_str");

    sub.listen([&](const DynamicData& data) {
      captured = data;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    DynamicData data;
    data.load("type_str", std::string("hello"));
    CHECK(pub.publish(data));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
    CHECK(captured.get_type() == "type_str");
    CHECK(!captured.is_empty());
    CHECK(captured.as<std::string>() == "hello");
  }

  TEST_CASE("dynamic data int payload round trip") {
    MESSAGE("[intra-field] dynamic data int payload round trip");

    std::atomic<bool> received{false};
    DynamicData captured;

    Publisher<DynamicData> pub(IntraConf("event_dyn_int", "", 0, "queue"));
    Subscriber<DynamicData> sub("intra://event_dyn_int");

    sub.listen([&](const DynamicData& data) {
      captured = data;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    DynamicData data;
    data.load("type_int", 42);
    CHECK(pub.publish(data));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
    CHECK(captured.get_type() == "type_int");
    CHECK(captured.as<int>() == 42);
  }

  TEST_CASE("dynamic data bytes payload is received intact") {
    MESSAGE("[intra-field] dynamic data bytes payload is received intact");

    std::atomic<bool> received{false};
    DynamicData captured;

    Publisher<DynamicData> pub(IntraConf("event_dyn_bytes", "", 0, "queue"));
    Subscriber<DynamicData> sub("intra://event_dyn_bytes");

    sub.listen([&](const DynamicData& data) {
      captured = data;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    DynamicData data;
    Bytes payload{0xAA, 0xBB, 0xCC};
    data.load("type_bytes", payload);
    CHECK(pub.publish(data));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
    CHECK(captured.get_type() == "type_bytes");

    Bytes out = captured.as<Bytes>();
    REQUIRE(out.size() == 3u);
    CHECK(out[0] == 0xAA);
    CHECK(out[1] == 0xBB);
    CHECK(out[2] == 0xCC);
  }

  TEST_CASE("default constructed dynamic data is empty") {
    MESSAGE("[intra-field] default constructed dynamic data is empty");

    DynamicData dd;

    CHECK(dd.is_empty());
    CHECK(dd.get_type().empty());
  }

  TEST_CASE("direct getter suspend and resume preserve latest cached value") {
    MESSAGE("[intra-field] direct getter suspend and resume preserve latest cached value");

    std::atomic<int> getter1_count{0};
    std::atomic<int> getter2_count{0};
    std::atomic<int> getter1_value{0};
    std::atomic<int> getter2_value{0};

    Getter<int> getter1("intra://field_direct_suspend#direct");
    Getter<int> getter2("intra://field_direct_suspend#direct");
    Setter<int> setter(IntraConf("field_direct_suspend", "", 0, "direct"));

    CHECK(getter1.get_abstract_node() != nullptr);
    CHECK(getter2.get_abstract_node() != nullptr);
    CHECK(setter.get_abstract_node() != nullptr);
    CHECK_FALSE(getter1.attach(nullptr));
    CHECK_FALSE(getter1.detach());
    CHECK_FALSE(setter.attach(nullptr));
    CHECK_FALSE(setter.detach());

    CHECK(getter1.listen([&](const int& value) {
      getter1_value.store(value, std::memory_order_relaxed);
      getter1_count.fetch_add(1, std::memory_order_release);
    }));
    CHECK(getter2.listen([&](const int& value) {
      getter2_value.store(value, std::memory_order_relaxed);
      getter2_count.fetch_add(1, std::memory_order_release);
    }));

    setter.set(1);
    CHECK(common_test::wait_until(
        [&getter1_count, &getter2_count] {
          return getter1_count.load(std::memory_order_acquire) == 1 &&
                 getter2_count.load(std::memory_order_acquire) == 1;
        },
        1s));
    REQUIRE(getter1.get().has_value());
    REQUIRE(getter2.get().has_value());
    CHECK_EQ(*getter1.get(), 1);
    CHECK_EQ(*getter2.get(), 1);

    CHECK(getter1.suspend());
    CHECK(getter1.is_suspend());
    setter.set(2);
    CHECK(common_test::wait_until([&getter2_count] { return getter2_count.load(std::memory_order_acquire) == 2; }, 1s));
    CHECK_EQ(getter1_count.load(std::memory_order_acquire), 1);
    CHECK_EQ(getter1_value.load(std::memory_order_relaxed), 1);
    CHECK_EQ(getter2_value.load(std::memory_order_relaxed), 2);
    REQUIRE(getter1.get().has_value());
    CHECK_EQ(*getter1.get(), 1);

    CHECK(getter1.resume());
    CHECK_FALSE(getter1.is_suspend());
    setter.set(3);
    CHECK(common_test::wait_until(
        [&getter1_count, &getter2_count] {
          return getter1_count.load(std::memory_order_acquire) == 2 &&
                 getter2_count.load(std::memory_order_acquire) == 3;
        },
        1s));
    REQUIRE(getter1.get().has_value());
    REQUIRE(getter2.get().has_value());
    CHECK_EQ(*getter1.get(), 3);
    CHECK_EQ(*getter2.get(), 3);
  }

  TEST_CASE("nested direct getter update preserves the outer cached value") {
    MESSAGE("[intra-field] nested direct getter update preserves the outer cached value");

    Getter<int> outer_getter("intra://field_direct_nested_outer#direct");
    Getter<int> inner_getter("intra://field_direct_nested_inner#direct");
    Setter<int> outer_setter(IntraConf("field_direct_nested_outer", "", 0, "direct"));
    Setter<int> inner_setter(IntraConf("field_direct_nested_inner", "", 0, "direct"));
    int outer_callback_value_after_nested_update = 0;

    CHECK(outer_getter.listen([&](const int& value) {
      inner_setter.set(22);
      outer_callback_value_after_nested_update = value;
    }));

    outer_setter.set(11);

    REQUIRE(outer_getter.get().has_value());
    REQUIRE(inner_getter.get().has_value());
    CHECK(*outer_getter.get() == 11);
    CHECK(*inner_getter.get() == 22);
    CHECK(outer_callback_value_after_nested_update == 11);
  }
}

TEST_SUITE("intra-qos") {
  TEST_CASE("latency and lost tracking is disabled for intra transport") {
    MESSAGE("[intra-qos] latency and lost tracking is disabled for intra transport");

    Publisher<int> pub(IntraConf("event_latency1", "", 0, "queue"));
    Subscriber<int> sub("intra://event_latency1");

    sub.set_latency_and_lost_enabled(true);
    CHECK(sub.is_latency_and_lost_enabled() == false);

    std::atomic<int> count{0};
    sub.listen([&](const int& /*val*/) { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 10; ++i) {
      pub.publish(i);
      std::this_thread::sleep_for(5ms);
    }

    std::this_thread::sleep_for(20ms);
    CHECK(count.load() > 0);
    CHECK(sub.get_latency() == 0);
    CHECK(sub.get_lost().total == 0u);

    sub.set_latency_and_lost_enabled(false);
    CHECK(sub.is_latency_and_lost_enabled() == false);
  }
}

TEST_SUITE("intra-error") {
  // TEST_CASE("distinct topics yield distinct abstract nodes") {
  //   MESSAGE("[intra-error] distinct topics yield distinct abstract nodes");
  //
  //   Publisher<int> pub1(IntraConf("event_node_id1", "", 0, "queue"));
  //   Publisher<int> pub2(IntraConf("event_node_id2", "", 0, "queue"));
  //   Subscriber<int> sub1("intra://event_node_id1");
  //
  //   auto node1 = pub1.get_abstract_node();
  //   auto node2 = pub2.get_abstract_node();
  //   auto node3 = sub1.get_abstract_node();
  //
  //   CHECK(node1 != nullptr);
  //   CHECK(node2 != nullptr);
  //   CHECK(node3 != nullptr);
  //   CHECK(node1 != node2);
  //   CHECK(node1 == node3);
  // }

  TEST_CASE("unknown scheme throws on publisher construction") { CHECK_THROWS(Publisher<int>("badscheme://topic")); }

  TEST_CASE("unknown scheme throws on subscriber construction") { CHECK_THROWS(Subscriber<int>("badscheme://topic")); }

  TEST_CASE("unknown scheme throws on server construction") { CHECK_THROWS(Server<int>("badscheme://topic?event=ev")); }

  TEST_CASE("unknown scheme throws on client construction") { CHECK_THROWS(Client<int>("badscheme://topic?event=ev")); }

  TEST_CASE("empty address throws on publisher construction") { CHECK_THROWS(Publisher<int>("intra://")); }

  TEST_CASE("malformed url with no scheme delimiter throws") { CHECK_THROWS(Publisher<int>("intra_topic")); }
}

TEST_SUITE("intra-pubsub") {
  TEST_CASE("4 publishers 4 subscribers all messages delivered") {
    MESSAGE("[intra-pubsub] 4 publishers 4 subscribers all messages delivered");

    static constexpr int kPubCount = 4;
    static constexpr int kSubCount = 4;
    static constexpr int kMsgsPerPub = 10;

    std::vector<std::atomic<int>> counts(kSubCount);

    for (auto& c : counts) {
      c.store(0);
    }

    std::vector<std::unique_ptr<Publisher<int>>> pubs;
    pubs.reserve(kPubCount);

    for (int i = 0; i < kPubCount; ++i) {
      pubs.emplace_back(std::make_unique<Publisher<int>>(IntraConf("ev_n4m4", "", 0, "queue")));
    }

    std::vector<std::unique_ptr<Subscriber<int>>> subs;
    subs.reserve(kSubCount);

    for (int i = 0; i < kSubCount; ++i) {
      subs.emplace_back(std::make_unique<Subscriber<int>>("intra://ev_n4m4"));
      subs.back()->listen([&counts, i](const int& /*v*/) { counts[i].fetch_add(1, std::memory_order_relaxed); });
    }

    for (auto& pub : pubs) {
      for (int i = 0; i < 50 && !pub->has_subscribers(); ++i) {
        std::this_thread::sleep_for(20ms);
      }
    }

    for (int m = 0; m < kMsgsPerPub; ++m) {
      for (auto& pub : pubs) {
        pub->publish(m, true);
      }
    }

    std::this_thread::sleep_for(100ms);

    for (int i = 0; i < kSubCount; ++i) {
      CHECK(counts[i].load() >= kMsgsPerPub * kPubCount / kSubCount);
    }
  }

  TEST_CASE("subscriber created before publisher receives messages") {
    MESSAGE("[intra-pubsub] subscriber created before publisher receives messages");

    std::atomic<int> count{0};

    Subscriber<int> sub("intra://ev_sub_first");
    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    Publisher<int> pub(IntraConf("ev_sub_first", "", 0, "queue"));
    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 5; ++i) {
      pub.publish(i);
    }

    std::this_thread::sleep_for(50ms);
    CHECK(count.load() == 5);
  }

  TEST_CASE("publisher destroyed mid-flight does not crash subscriber") {
    MESSAGE("[intra-pubsub] publisher destroyed mid-flight does not crash subscriber");

    std::atomic<int> count{0};
    Subscriber<int> sub("intra://ev_pub_destroy");
    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    {
      Publisher<int> pub(IntraConf("ev_pub_destroy", "", 0, "queue"));
      CHECK(pub.wait_for_subscribers(1s));

      for (int i = 0; i < 5; ++i) {
        pub.publish(i);
      }
    }

    std::this_thread::sleep_for(50ms);
    CHECK(count.load() >= 0);
  }

  TEST_CASE("rapid create destroy subscriber churn does not crash") {
    MESSAGE("[intra-pubsub] rapid create destroy subscriber churn does not crash");

    Publisher<int> pub(IntraConf("ev_churn", "", 0, "queue"));

    for (int i = 0; i < 20; ++i) {
      Subscriber<int> sub("intra://ev_churn");
      sub.listen([](const int& /*v*/) {});
      pub.publish(i, true);
    }

    CHECK(true);
  }

  TEST_CASE("large payload 1 kib is received intact") {
    MESSAGE("[intra-pubsub] large payload 1 kib is received intact");

    static constexpr size_t kSize = 1024u;
    std::atomic<bool> received{false};
    size_t recv_size = 0;

    Publisher<Bytes> pub(IntraConf("ev_large_1k", "", 0, "queue"));
    Subscriber<Bytes> sub("intra://ev_large_1k");

    sub.listen([&](const Bytes& data) {
      recv_size = data.size();
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    Bytes payload = Bytes::create(kSize);

    for (size_t i = 0; i < kSize; ++i) {
      payload[i] = static_cast<uint8_t>(i & 0xFFu);
    }

    CHECK(pub.publish(payload));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
    CHECK_EQ(recv_size, kSize);
  }

  TEST_CASE("large payload 64 kib is received intact") {
    MESSAGE("[intra-pubsub] large payload 64 kib is received intact");

    static constexpr size_t kSize = 65536u;
    std::atomic<bool> received{false};
    size_t recv_size = 0;

    Publisher<Bytes> pub(IntraConf("ev_large_64k", "", 0, "queue"));
    Subscriber<Bytes> sub("intra://ev_large_64k");

    sub.listen([&](const Bytes& data) {
      recv_size = data.size();
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(Bytes::create(kSize)));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 2s));
    CHECK_EQ(recv_size, kSize);
  }

  TEST_CASE("large payload 1 mib is received intact") {
    MESSAGE("[intra-pubsub] large payload 1 mib is received intact");

    static constexpr size_t kSize = 1048576u;
    std::atomic<bool> received{false};
    size_t recv_size = 0;

    Publisher<Bytes> pub(IntraConf("ev_large_1m", "", 0, "queue"));
    Subscriber<Bytes> sub("intra://ev_large_1m");

    sub.listen([&](const Bytes& data) {
      recv_size = data.size();
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(Bytes::create(kSize)));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 4s));
    CHECK_EQ(recv_size, kSize);
  }

  TEST_CASE("empty bytes payload is received with zero size") {
    MESSAGE("[intra-pubsub] empty bytes payload is received with zero size");

    std::atomic<bool> received{false};
    size_t recv_size = 1;

    Publisher<Bytes> pub(IntraConf("ev_empty_bytes", "", 0, "queue"));
    Subscriber<Bytes> sub("intra://ev_empty_bytes");

    sub.listen([&](const Bytes& data) {
      recv_size = data.size();
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(Bytes{}));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
    CHECK_EQ(recv_size, 0u);
  }

  TEST_CASE("empty string payload is received correctly") {
    MESSAGE("[intra-pubsub] empty string payload is received correctly");

    std::atomic<bool> received{false};
    std::string captured = "notempty";

    Publisher<std::string> pub(IntraConf("ev_empty_str", "", 0, "queue"));
    Subscriber<std::string> sub("intra://ev_empty_str");

    sub.listen([&](const std::string& val) {
      captured = val;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));
    CHECK(pub.publish(std::string{}));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
    CHECK(captured.empty());
  }

  TEST_CASE("re-subscribe after destroy delivers subsequent messages") {
    MESSAGE("[intra-pubsub] re-subscribe after destroy delivers subsequent messages");

    std::atomic<int> count{0};

    Publisher<int> pub(IntraConf("ev_resub", "", 0, "queue"));

    {
      Subscriber<int> sub1("intra://ev_resub");
      sub1.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });
      CHECK(pub.wait_for_subscribers(1s));
      pub.publish(1);
      std::this_thread::sleep_for(30ms);
    }

    int after_first = count.load();
    CHECK(after_first >= 1);

    Subscriber<int> sub2("intra://ev_resub");
    sub2.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });
    CHECK(pub.wait_for_subscribers(1s));
    pub.publish(2);
    std::this_thread::sleep_for(30ms);

    CHECK(count.load() > after_first);
  }

  TEST_CASE("multiple topics single pub each subscriber only gets own topic") {
    MESSAGE("[intra-pubsub] multiple topics single pub each subscriber only gets own topic");

    std::atomic<int> count_a{0};
    std::atomic<int> count_b{0};

    Publisher<int> pub_a(IntraConf("ev_multi_topic_a", "", 0, "queue"));
    Publisher<int> pub_b(IntraConf("ev_multi_topic_b", "", 0, "queue"));
    Subscriber<int> sub_a("intra://ev_multi_topic_a");
    Subscriber<int> sub_b("intra://ev_multi_topic_b");

    sub_a.listen([&](const int& /*v*/) { count_a.fetch_add(1, std::memory_order_relaxed); });
    sub_b.listen([&](const int& /*v*/) { count_b.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub_a.wait_for_subscribers(1s));
    CHECK(pub_b.wait_for_subscribers(1s));

    pub_a.publish(1);
    pub_b.publish(2);
    std::this_thread::sleep_for(50ms);

    CHECK(count_a.load() == 1);
    CHECK(count_b.load() == 1);
  }

  TEST_CASE("concurrent publish from multiple threads all messages received") {
    MESSAGE("[intra-pubsub] concurrent publish from multiple threads all messages received");

    static constexpr int kThreads = 4;
    static constexpr int kPerThread = 25;

    std::atomic<int> count{0};

    Publisher<int> pub(IntraConf("ev_concurrent_pub", "", 0, "queue"));
    Subscriber<int> sub("intra://ev_concurrent_pub");

    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });
    CHECK(pub.wait_for_subscribers(1s));

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&] {
        for (int i = 0; i < kPerThread; ++i) {
          pub.publish(i, true);
        }
      });
    }

    for (auto& th : threads) {
      th.join();
    }

    std::this_thread::sleep_for(100ms);
    CHECK(count.load() >= kThreads * kPerThread / 2);
  }
}

TEST_SUITE("intra-method") {
  TEST_CASE("client timeout returns false without blocking") {
    MESSAGE("[intra-method] client timeout returns false without blocking");

    Client<std::string, std::string> client("intra://ev_timeout_srv?event=noserver");

    CHECK_FALSE(client.wait_for_connected(50ms));
  }

  TEST_CASE("server destroyed before reply does not deadlock client") {
    MESSAGE("[intra-method] server destroyed before reply does not deadlock client");

    std::optional<Client<std::string, std::string>> client;

    {
      Server<std::string, std::string> server(IntraConf("ev_srv_destroy", "null"));
      server.listen([](const std::string& /*req*/, std::string& resp) { resp = "ok"; });
      client.emplace("intra://ev_srv_destroy?event=null");
      CHECK(client->wait_for_connected(1s));
    }

    auto resp = client->invoke("request", 100ms);
    CHECK_FALSE(resp.has_value());
  }

  TEST_CASE("handler is invoked synchronously by intra invoke and side effects are observable") {
    MESSAGE("[intra-method] handler is invoked synchronously by intra invoke and side effects are observable");

    std::atomic<bool> handler_ran{false};

    Server<std::string, std::string> server(IntraConf("ev_srv_sideeffect", "null"));
    server.listen([&](const std::string& /*req*/, std::string& resp) {
      handler_ran.store(true, std::memory_order_release);
      resp = "ok";
    });

    Client<std::string, std::string> client("intra://ev_srv_sideeffect?event=null");
    CHECK(client.wait_for_connected(1s));

    auto resp = client.invoke("request", 500ms);
    CHECK(handler_ran.load(std::memory_order_acquire));
    REQUIRE(resp.has_value());
    CHECK_EQ(*resp, "ok");
  }

  TEST_CASE("fire and forget with multiple concurrent clients all reach server") {
    MESSAGE("[intra-method] fire and forget with multiple concurrent clients all reach server");

    static constexpr int kClients = 4;
    std::atomic<int> counter{0};

    Server<std::string> server(IntraConf("ev_multi_client", "null"));
    server.listen([&](const std::string& /*req*/) { counter.fetch_add(1, std::memory_order_relaxed); });

    std::vector<std::unique_ptr<Client<std::string>>> clients;
    clients.reserve(kClients);

    for (int i = 0; i < kClients; ++i) {
      clients.emplace_back(std::make_unique<Client<std::string>>("intra://ev_multi_client?event=null"));
    }

    for (auto& c : clients) {
      CHECK(c->wait_for_connected(1s));
    }

    for (auto& c : clients) {
      c->send("ping");
    }

    std::this_thread::sleep_for(100ms);
    CHECK(counter.load() == kClients);
  }

  TEST_CASE("synchronous invoke with short timeout reports timeout") {
    MESSAGE("[intra-method] synchronous invoke with short timeout reports timeout");

    Server<std::string, std::string> server(IntraConf("ev_invoke_short_to", "null"));
    server.listen([](const std::string& /*req*/, std::string& resp) {
      std::this_thread::sleep_for(200ms);
      resp = "late";
    });

    Client<std::string, std::string> client("intra://ev_invoke_short_to?event=null");
    CHECK(client.wait_for_connected(1s));

    auto resp = client.invoke("req", 50ms);
    CHECK_FALSE(resp.has_value());
  }

  TEST_CASE("listen_for_reply on intra server always returns false on reply") {
    MESSAGE("[intra-method] listen_for_reply on intra server always returns false on reply");

    Server<std::string, std::string> server(IntraConf("ev_async_reply_check", "null"));
    server.listen_for_reply([](uint64_t, const std::string&) {});

    CHECK_FALSE(server.reply(999, std::string("data")));
  }

  TEST_CASE("bytes request response and fire-and-forget paths are exercised directly") {
    MESSAGE("[intra-method] bytes request response and fire-and-forget paths are exercised directly");

    Server<Bytes, Bytes> server(IntraConf("ev_bytes_rpc_direct", "null", 0, "direct"));
    CHECK(server.listen([](const Bytes& req, Bytes& resp) {
      resp = Bytes::create(req.size() + 1);
      if (!req.empty()) {
        resp[0] = req[0];
      }
      resp[resp.size() - 1] = 0xEF;
    }));

    Client<Bytes, Bytes> client(IntraConf("ev_bytes_rpc_direct", "null", 0, "direct"));
    CHECK(client.wait_for_connected(1s));

    Bytes response;
    CHECK(client.invoke(Bytes{0xAB, 0xCD}, response, 1s));
    REQUIRE_EQ(response.size(), 3u);
    CHECK_EQ(response[0], 0xABu);
    CHECK_EQ(response[2], 0xEFu);

    std::atomic<int> fire_count{0};
    Server<Bytes> fire_server(IntraConf("ev_bytes_fire_direct", "null", 0, "direct"));
    CHECK(fire_server.listen([&](const Bytes& req) {
      if (req.size() == 2 && req[0] == 0x10) {
        fire_count.fetch_add(1, std::memory_order_relaxed);
      }
    }));

    Client<Bytes> fire_client(IntraConf("ev_bytes_fire_direct", "null", 0, "direct"));
    CHECK(fire_client.wait_for_connected(1s));
    CHECK(fire_client.send(Bytes{0x10, 0x20}));
    CHECK_EQ(fire_count.load(std::memory_order_relaxed), 1);
  }

  TEST_CASE("fire-and-forget client does not enter request-response callback") {
    MESSAGE("[intra-method] fire-and-forget client does not enter request-response callback");

    std::atomic<int> handled{0};
    Server<std::string, std::string> server(IntraConf("ev_sync_server_fire_client", "null", 0, "direct"));
    CHECK(server.listen([&](const std::string&, std::string& resp) {
      handled.fetch_add(1, std::memory_order_relaxed);
      resp = "unexpected";
    }));

    Client<std::string> client(IntraConf("ev_sync_server_fire_client", "null", 0, "direct"));
    CHECK(client.wait_for_connected(1s));
    CHECK(client.send("fire"));

    CHECK_EQ(handled.load(std::memory_order_acquire), 0);
  }

  TEST_CASE("same address different events are isolated for method calls") {
    MESSAGE("[intra-method] same address different events are isolated for method calls");

    Server<std::string, std::string> server_a(IntraConf("ev_method_filter", "a", 0, "direct"));
    Server<std::string, std::string> server_b(IntraConf("ev_method_filter", "b", 0, "direct"));

    CHECK(server_a.listen([](const std::string& req, std::string& resp) { resp = "a:" + req; }));
    CHECK(server_b.listen([](const std::string& req, std::string& resp) { resp = "b:" + req; }));

    Client<std::string, std::string> client_a(IntraConf("ev_method_filter", "a", 0, "direct"));
    Client<std::string, std::string> client_b(IntraConf("ev_method_filter", "b", 0, "direct"));
    Client<std::string, std::string> client_c(IntraConf("ev_method_filter", "c", 0, "direct"));

    CHECK(client_a.wait_for_connected(1s));
    CHECK(client_b.wait_for_connected(1s));
    CHECK_FALSE(client_c.wait_for_connected(20ms));

    std::string out;
    CHECK(client_a.invoke("one", out, 1s));
    CHECK_EQ(out, "a:one");
    CHECK(client_b.invoke("two", out, 1s));
    CHECK_EQ(out, "b:two");
    CHECK_FALSE(client_c.invoke("miss", out, 20ms));
  }

  TEST_CASE("server drops malformed request bytes before invoking typed handlers") {
    MESSAGE("[intra-method] server drops malformed request bytes before invoking typed handlers");

    std::atomic<int> fire_count{0};
    Server<int> fire_server(IntraConf("ev_bad_req_fire", "null", 0, "direct"));
    CHECK(fire_server.listen([&](const int&) { fire_count.fetch_add(1, std::memory_order_relaxed); }));

    Client<Bytes> fire_client(IntraConf("ev_bad_req_fire", "null", 0, "direct"));
    CHECK(fire_client.wait_for_connected(1s));
    CHECK(fire_client.send(Bytes{0x01}));
    CHECK_EQ(fire_count.load(std::memory_order_relaxed), 0);

    std::atomic<int> sync_count{0};
    Server<int, int> sync_server(IntraConf("ev_bad_req_sync", "null", 0, "direct"));
    CHECK(sync_server.listen([&](const int&, int& resp) {
      sync_count.fetch_add(1, std::memory_order_relaxed);
      resp = 1;
    }));

    Client<Bytes, Bytes> sync_client(IntraConf("ev_bad_req_sync", "null", 0, "direct"));
    CHECK(sync_client.wait_for_connected(1s));
    Bytes sync_resp;
    CHECK(sync_client.invoke(Bytes{0x02}, sync_resp, 1s));
    CHECK(sync_resp.empty());
    CHECK_EQ(sync_count.load(std::memory_order_relaxed), 0);

    std::atomic<int> async_count{0};
    Server<int, int> async_server(IntraConf("ev_bad_req_async", "null", 0, "direct"));
    CHECK(async_server.listen_for_reply(
        [&](uint64_t, const int&) { async_count.fetch_add(1, std::memory_order_relaxed); }));

    Client<Bytes, Bytes> async_client(IntraConf("ev_bad_req_async", "null", 0, "direct"));
    CHECK(async_client.wait_for_connected(1s));
    Bytes async_resp;
    CHECK(async_client.invoke(Bytes{0x03}, async_resp, 1s));
    CHECK(async_resp.empty());
    CHECK_EQ(async_count.load(std::memory_order_relaxed), 0);
  }

  TEST_CASE("client async paths report transport and response decode failures") {
    MESSAGE("[intra-method] client async paths report transport and response decode failures");

    Client<std::string, std::string> missing("intra://ev_async_missing?event=null#direct");
    auto missing_future = missing.async_invoke(std::string("request"));
    REQUIRE(missing_future.wait_for(1s) == std::future_status::ready);
    CHECK_THROWS(missing_future.get());

    Server<int, Bytes> bad_resp_server(IntraConf("ev_async_bad_response", "null", 0, "direct"));
    CHECK(bad_resp_server.listen([](const int&, Bytes& resp) { resp = Bytes{0x01}; }));

    Client<int, int> client(IntraConf("ev_async_bad_response", "null", 0, "direct"));
    CHECK(client.wait_for_connected(1s));

    std::atomic<int> callback_count{0};
    CHECK(client.invoke(7, [&](const int&) { callback_count.fetch_add(1, std::memory_order_relaxed); }));
    CHECK_EQ(callback_count.load(std::memory_order_relaxed), 0);

    auto future = client.async_invoke(8);
    REQUIRE(future.wait_for(1s) == std::future_status::ready);
    CHECK_THROWS(future.get());
  }
}

TEST_SUITE("intra-field") {
  TEST_CASE("setter get returns value after set") {
    MESSAGE("[intra-field] setter get returns value after set");

    Setter<int> setter(IntraConf("field_get_basic", "", 0, "queue"));
    Getter<int> getter("intra://field_get_basic");

    setter.set(42);

    CHECK(getter.wait_for_value(1s));

    auto val = getter.get();
    REQUIRE(val.has_value());
    CHECK_EQ(*val, 42);
  }

  TEST_CASE("getter get returns nullopt before any setter write") {
    MESSAGE("[intra-field] getter get returns nullopt before any setter write");

    Getter<int> getter(IntraConf("field_no_write", "", 0, "queue"));

    auto val = getter.get();
    CHECK_FALSE(val.has_value());
  }

  TEST_CASE("setter set before init is cached without breaking later writes") {
    MESSAGE("[intra-field] setter set before init is cached without breaking later writes");

    Setter<int> setter(IntraConf("field_deferred_snapshot", "", 0, "queue"), InitType::kWithoutInit);
    Getter<int> getter("intra://field_deferred_snapshot");

    setter.set(1234);
    REQUIRE(setter.init());
    setter.set(5678);

    CHECK(getter.wait_for_value(1s));
    auto val = getter.get();
    REQUIRE(val.has_value());
    CHECK_EQ(*val, 5678);
  }

  TEST_CASE("getter listen callback fires on setter write") {
    MESSAGE("[intra-field] getter listen callback fires on setter write");

    std::atomic<bool> received{false};
    std::atomic<int> captured{0};

    Setter<int> setter(IntraConf("field_listen_cb", "", 0, "queue"));
    Getter<int> getter("intra://field_listen_cb");

    getter.listen([&](const int& v) {
      captured.store(v, std::memory_order_relaxed);
      received.store(true, std::memory_order_release);
    });

    setter.set(77);

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
    CHECK_EQ(captured.load(), 77);
  }

  TEST_CASE("getter invokes the installed callback object without holding its value mutex") {
    MESSAGE("[intra-field] getter invokes the installed callback object without holding its value mutex");

    int observed_state = 0;
    bool called_get = false;
    Setter<int> setter(IntraConf("field_stateful_callback", "", 0, "direct"));
    Getter<int> getter(IntraConf("field_stateful_callback", "", 0, "direct"));

    REQUIRE(getter.listen([&getter, &observed_state, &called_get, state = 0](const int&) mutable {
      observed_state = ++state;
      called_get = true;
      (void)getter.get();
    }));

    setter.set(1);
    setter.set(2);

    CHECK(called_get);
    CHECK_EQ(observed_state, 2);
  }

  TEST_CASE("late getter receives cached value after setter write") {
    MESSAGE("[intra-field] late getter receives cached value after setter write");

    Setter<int> setter(IntraConf("field_late_getter", "", 0, "queue"));
    setter.set(99);

    std::this_thread::sleep_for(100ms);

    Getter<int> getter("intra://field_late_getter");
    if (getter.wait_for_value(2s)) {
      auto val = getter.get();
      REQUIRE(val.has_value());
      CHECK_EQ(*val, 99);
    }
  }

  TEST_CASE("multiple sets deliver only latest value to getter") {
    MESSAGE("[intra-field] multiple sets deliver only latest value to getter");

    std::atomic<int> last_val{0};
    std::atomic<int> call_count{0};

    Setter<int> setter(IntraConf("field_multi_set", "", 0, "queue"));
    Getter<int> getter("intra://field_multi_set");

    getter.listen([&](const int& v) {
      last_val.store(v, std::memory_order_relaxed);
      call_count.fetch_add(1, std::memory_order_relaxed);
    });

    for (int i = 1; i <= 5; ++i) {
      setter.set(i);
      std::this_thread::sleep_for(10ms);
    }

    std::this_thread::sleep_for(50ms);
    CHECK(call_count.load() >= 1);
    CHECK_EQ(last_val.load(), 5);
  }

  TEST_CASE("setter set on bytes payload delivers correct content") {
    MESSAGE("[intra-field] setter set on bytes payload delivers correct content");

    std::atomic<bool> received{false};
    Bytes captured;

    Setter<Bytes> setter(IntraConf("field_bytes_payload", "", 0, "queue"));
    Getter<Bytes> getter("intra://field_bytes_payload");

    getter.listen([&](const Bytes& data) {
      captured = data;
      received.store(true, std::memory_order_release);
    });

    Bytes payload{0x10, 0x20, 0x30};
    setter.set(payload);

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
    REQUIRE_EQ(captured.size(), 3u);
    CHECK_EQ(captured[0], 0x10u);
    CHECK_EQ(captured[1], 0x20u);
    CHECK_EQ(captured[2], 0x30u);
  }

  TEST_CASE("change reporting suppresses duplicate values") {
    MESSAGE("[intra-field] change reporting suppresses duplicate values");

    std::atomic<int> call_count{0};

    Setter<int> setter(IntraConf("field_change_rep", "", 0, "queue"));
    Getter<int> getter("intra://field_change_rep");

    getter.set_change_reporting(true);
    getter.listen([&](const int& /*v*/) { call_count.fetch_add(1, std::memory_order_relaxed); });

    setter.set(10);
    std::this_thread::sleep_for(30ms);
    int after_first = call_count.load();
    CHECK_EQ(after_first, 1);

    setter.set(10);
    std::this_thread::sleep_for(30ms);
    CHECK_EQ(call_count.load(), 1);

    setter.set(20);
    std::this_thread::sleep_for(30ms);
    CHECK_EQ(call_count.load(), 2);
  }

  TEST_CASE("getter interrupt unblocks wait_for_value") {
    MESSAGE("[intra-field] getter interrupt unblocks wait_for_value");

    Getter<int> getter(IntraConf("field_interrupt_wait", "", 0, "queue"));

    std::atomic<bool> returned{false};
    std::thread t([&] {
      getter.wait_for_value(10s);
      returned.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(30ms);
    getter.interrupt();
    t.join();

    CHECK(returned.load(std::memory_order_acquire));
  }
}

namespace {

struct IntraCustomMsg {
  int id{0};
  std::string label;
  std::vector<int> values;

  void operator>>(vlink::Bytes& out) const {
    std::string encoded = std::to_string(id) + "|" + label + "|";
    for (size_t k = 0; k < values.size(); ++k) {
      if (k > 0) {
        encoded += ",";
      }

      encoded += std::to_string(values[k]);
    }

    out = vlink::Bytes::deep_copy(reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size());
  }

  void operator<<(const vlink::Bytes& in) {
    std::string s(reinterpret_cast<const char*>(in.data()), in.size());
    auto p1 = s.find('|');
    auto p2 = s.find('|', p1 + 1);

    id = std::stoi(s.substr(0, p1));
    label = s.substr(p1 + 1, p2 - p1 - 1);

    values.clear();

    std::string rest = s.substr(p2 + 1);
    size_t pos = 0;

    while (pos < rest.size()) {
      auto comma = rest.find(',', pos);

      if (comma == std::string::npos) {
        values.push_back(std::stoi(rest.substr(pos)));
        break;
      }

      values.push_back(std::stoi(rest.substr(pos, comma - pos)));
      pos = comma + 1;
    }
  }
};

}  // namespace

struct IntraSensorReading {
  uint32_t id{0};
  uint64_t timestamp_ns{0};
  double temperature{0};
  int32_t status{0};

  bool operator==(const IntraSensorReading& o) const noexcept {
    return id == o.id && timestamp_ns == o.timestamp_ns && temperature == o.temperature && status == o.status;
  }

  bool operator>>(Bytes& bytes) const noexcept {
    bytes = Bytes::deep_copy(reinterpret_cast<const uint8_t*>(this), sizeof(IntraSensorReading));
    return true;
  }

  bool operator<<(const Bytes& bytes) noexcept {
    if (bytes.size() != sizeof(IntraSensorReading)) {
      return false;
    }

    std::memcpy(this, bytes.data(), sizeof(IntraSensorReading));
    return true;
  }
};

static_assert(std::is_standard_layout_v<IntraSensorReading>);

VLINK_INTRA_DATA_DECLARE(int, IntraIntData)
VLINK_INTRA_DATA_DECLARE(std::string, IntraStringData)
VLINK_INTRA_DATA_DECLARE(IntraSensorReading, IntraSensorData)

TEST_SUITE("intra-intradata") {
  TEST_CASE("basic round trip via VLINK_INTRA_DATA_DECLARE") {
    MESSAGE("[intra-intradata] basic round trip via VLINK_INTRA_DATA_DECLARE");

    std::atomic<bool> received{false};
    std::atomic<int> captured{0};

    Publisher<IntraIntData> pub(IntraConf("intra_id_basic", "", 0, "queue"));
    Subscriber<IntraIntData> sub("intra://intra_id_basic");

    sub.listen([&](const IntraIntData& d) {
      captured.store((*d).value, std::memory_order_relaxed);
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    auto data = IntraIntData::create();
    data->value = 42;
    CHECK(pub.publish(data));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
    CHECK_EQ(captured.load(), 42);
  }

  TEST_CASE("declared type reports non-empty serialized type name") {
    MESSAGE("[intra-intradata] declared type reports non-empty serialized type name");

    auto name = IntraStringDataType::get_serialized_type();
    CHECK_FALSE(name.empty());
  }

  TEST_CASE("bytes payload is ignored by typed intra subscriber") {
    MESSAGE("[intra-intradata] bytes payload is ignored by typed intra subscriber");

    std::atomic<int> delivered{0};

    Publisher<Bytes> pub(IntraConf("event_bad_intra_type", "", 0, "direct"));
    Subscriber<IntraIntData> sub("intra://event_bad_intra_type#direct");

    CHECK(sub.listen([&](const IntraIntData&) { delivered.fetch_add(1, std::memory_order_relaxed); }));
    CHECK(pub.wait_for_subscribers(1s));

    CHECK_FALSE(pub.publish(Bytes::from_string("not_an_int_wrapper")));

    CHECK_EQ(delivered.load(std::memory_order_relaxed), 0);
  }

  TEST_CASE("two distinct declared types coexist without interference") {
    MESSAGE("[intra-intradata] two distinct declared types coexist without interference");

    std::atomic<bool> got_int{false};
    std::atomic<bool> got_str{false};

    Publisher<IntraIntData> pub_i(IntraConf("intra_id_coexist_i", "", 0, "queue"));
    Publisher<IntraStringData> pub_s(IntraConf("intra_id_coexist_s", "", 0, "queue"));
    Subscriber<IntraIntData> sub_i("intra://intra_id_coexist_i");
    Subscriber<IntraStringData> sub_s("intra://intra_id_coexist_s");

    sub_i.listen([&](const IntraIntData& /*d*/) { got_int.store(true, std::memory_order_release); });
    sub_s.listen([&](const IntraStringData& /*d*/) { got_str.store(true, std::memory_order_release); });

    CHECK(pub_i.wait_for_subscribers(1s));
    CHECK(pub_s.wait_for_subscribers(1s));

    auto di = IntraIntData::create();
    di->value = 1;
    pub_i.publish(di);

    auto ds = IntraStringData::create();
    ds->value = "hello";
    pub_s.publish(ds);

    CHECK(common_test::wait_until(
        [&got_int, &got_str] {
          return got_int.load(std::memory_order_acquire) && got_str.load(std::memory_order_acquire);
        },
        1s));
  }

  TEST_CASE("string payload round trips correctly via declared intra type") {
    MESSAGE("[intra-intradata] string payload round trips correctly via declared intra type");

    std::atomic<bool> received{false};
    std::string captured;

    Publisher<IntraStringData> pub(IntraConf("intra_id_string", "", 0, "queue"));
    Subscriber<IntraStringData> sub("intra://intra_id_string");

    sub.listen([&](const IntraStringData& d) {
      captured = (*d).value;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    auto data = IntraStringData::create();
    data->value = "vlink_intra_test";
    CHECK(pub.publish(data));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
    CHECK_EQ(captured, "vlink_intra_test");
  }

  TEST_CASE("qos default behavior is unchanged with declared intra type") {
    MESSAGE("[intra-intradata] qos default behavior is unchanged with declared intra type");

    std::atomic<int> call_count{0};

    Publisher<IntraIntData> pub(IntraConf("intra_id_qos", "", 0, "queue"));
    Subscriber<IntraIntData> sub("intra://intra_id_qos");

    sub.listen([&](const IntraIntData& /*d*/) { call_count.fetch_add(1, std::memory_order_relaxed); });
    CHECK(pub.wait_for_subscribers(1s));

    for (int k = 0; k < 5; ++k) {
      auto d = IntraIntData::create();
      d->value = k;
      pub.publish(d);
    }

    std::this_thread::sleep_for(100ms);
    CHECK(call_count.load() >= 5);
  }

  TEST_CASE("schema type is reported consistently") {
    MESSAGE("[intra-intradata] schema type is reported consistently");

    static constexpr auto kSchemaInt = IntraIntDataType::get_schema_type();
    static constexpr auto kSchemaStr = IntraStringDataType::get_schema_type();

    CHECK_EQ(static_cast<int>(kSchemaInt), static_cast<int>(kSchemaStr));
  }

  TEST_CASE("custom struct round trips with all fields preserved") {
    MESSAGE("[intra-intradata] custom struct round trips with all fields preserved");

    std::atomic<bool> received{false};
    IntraSensorReading captured{};

    Publisher<IntraSensorData> pub(IntraConf("intra_id_struct_basic", "", 0, "queue"));
    Subscriber<IntraSensorData> sub("intra://intra_id_struct_basic");

    sub.listen([&](const IntraSensorData& d) {
      captured = (*d).value;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    auto data = IntraSensorData::create();
    data->value = IntraSensorReading{17u, 1729000000000ull, 23.75, -1};
    CHECK(pub.publish(data));

    REQUIRE(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
    CHECK_EQ(captured.id, 17u);
    CHECK_EQ(captured.timestamp_ns, 1729000000000ull);
    CHECK_EQ(captured.temperature, doctest::Approx(23.75));
    CHECK_EQ(captured.status, -1);
  }

  TEST_CASE("custom struct sequence preserves order and per-field integrity") {
    MESSAGE("[intra-intradata] custom struct sequence preserves order and per-field integrity");

    static constexpr int kCount = 8;
    std::vector<IntraSensorReading> captured;
    captured.reserve(kCount);
    std::mutex captured_mtx;
    std::atomic<int> count{0};

    Publisher<IntraSensorData> pub(IntraConf("intra_id_struct_seq", "", 0, "queue"));
    Subscriber<IntraSensorData> sub("intra://intra_id_struct_seq");

    sub.listen([&](const IntraSensorData& d) {
      std::lock_guard lock(captured_mtx);
      captured.push_back((*d).value);
      count.fetch_add(1, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < kCount; ++i) {
      auto data = IntraSensorData::create();
      data->value = IntraSensorReading{static_cast<uint32_t>(i), static_cast<uint64_t>(i) * 1000ull, 1.5 * i,
                                       static_cast<int32_t>(-i)};
      pub.publish(data);
    }

    REQUIRE(common_test::wait_until([&count] { return count.load(std::memory_order_acquire) >= kCount; }, 1s));

    std::lock_guard lock(captured_mtx);
    REQUIRE_EQ(captured.size(), static_cast<size_t>(kCount));

    for (int i = 0; i < kCount; ++i) {
      CHECK_EQ(captured[i].id, static_cast<uint32_t>(i));
      CHECK_EQ(captured[i].timestamp_ns, static_cast<uint64_t>(i) * 1000ull);
      CHECK_EQ(captured[i].temperature, doctest::Approx(1.5 * i));
      CHECK_EQ(captured[i].status, static_cast<int32_t>(-i));
    }
  }

  TEST_CASE("custom struct declared type reports stable serialized name") {
    MESSAGE("[intra-intradata] custom struct declared type reports stable serialized name");

    const std::string a = IntraSensorDataType::get_serialized_type();
    const std::string b = IntraSensorDataType::get_serialized_type();
    CHECK_EQ(a, b);
  }
}

TEST_SUITE("intra-custom") {
  TEST_CASE("custom type round trips id label and values") {
    MESSAGE("[intra-custom] custom type round trips id label and values");

    std::atomic<bool> received{false};
    IntraCustomMsg captured{};

    Publisher<IntraCustomMsg> pub(IntraConf("intra_cust_basic", "", 0, "queue"));
    Subscriber<IntraCustomMsg> sub("intra://intra_cust_basic");

    sub.listen([&](const IntraCustomMsg& m) {
      captured = m;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    IntraCustomMsg msg;
    msg.id = 7;
    msg.label = "hello";
    msg.values = {1, 2, 3};
    CHECK(pub.publish(msg));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
    CHECK_EQ(captured.id, 7);
    CHECK_EQ(captured.label, "hello");
    REQUIRE_EQ(captured.values.size(), 3u);
    CHECK_EQ(captured.values[1], 2);
  }

  TEST_CASE("custom type with nested fields survives encode decode") {
    MESSAGE("[intra-custom] custom type with nested fields survives encode decode");

    std::atomic<bool> received{false};
    IntraCustomMsg captured{};

    Publisher<IntraCustomMsg> pub(IntraConf("intra_cust_nested", "", 0, "queue"));
    Subscriber<IntraCustomMsg> sub("intra://intra_cust_nested");

    sub.listen([&](const IntraCustomMsg& m) {
      captured = m;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    IntraCustomMsg msg;
    msg.id = 99;
    msg.label = "deep_nested_label_value";
    msg.values = {10, 20, 30, 40, 50};
    pub.publish(msg);

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
    CHECK_EQ(captured.id, 99);
    REQUIRE_EQ(captured.values.size(), 5u);
    CHECK_EQ(captured.values[4], 50);
  }

  TEST_CASE("multiple subscribers all receive custom type message") {
    MESSAGE("[intra-custom] multiple subscribers all receive custom type message");

    std::atomic<int> count{0};

    Publisher<IntraCustomMsg> pub(IntraConf("intra_cust_multi", "", 0, "queue"));
    Subscriber<IntraCustomMsg> sub1("intra://intra_cust_multi");
    Subscriber<IntraCustomMsg> sub2("intra://intra_cust_multi");

    auto cb = [&](const IntraCustomMsg& /*m*/) { count.fetch_add(1, std::memory_order_relaxed); };
    sub1.listen(cb);
    sub2.listen(cb);

    CHECK(pub.wait_for_subscribers(1s));

    IntraCustomMsg msg;
    msg.id = 1;
    msg.label = "multi";
    pub.publish(msg);

    std::this_thread::sleep_for(100ms);
    CHECK(count.load() >= 2);
  }

  TEST_CASE("large custom value vector survives round trip") {
    MESSAGE("[intra-custom] large custom value vector survives round trip");

    std::atomic<bool> received{false};
    IntraCustomMsg captured{};

    Publisher<IntraCustomMsg> pub(IntraConf("intra_cust_large", "", 0, "queue"));
    Subscriber<IntraCustomMsg> sub("intra://intra_cust_large");

    sub.listen([&](const IntraCustomMsg& m) {
      captured = m;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    IntraCustomMsg msg;
    msg.id = 5;
    msg.label = "large";

    for (int k = 0; k < 100; ++k) {
      msg.values.push_back(k);
    }

    pub.publish(msg);

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
    REQUIRE_EQ(captured.values.size(), 100u);
    CHECK_EQ(captured.values[99], 99);
  }

  TEST_CASE("serializer detects custom type correctly") {
    MESSAGE("[intra-custom] serializer detects custom type correctly");

    static constexpr auto kType = Serializer::get_type_of<IntraCustomMsg>();
    CHECK_EQ(kType, Serializer::kCustomType);
  }
}

#include "./zerocopy/camera_frame.h"
#include "./zerocopy/point_cloud.h"
#include "./zerocopy/proxy_data.h"
#include "./zerocopy/raw_data.h"

TEST_SUITE("intra-dynamicdata") {
  TEST_CASE("double and float payloads round trip via dynamicdata") {
    MESSAGE("[intra-dynamicdata] double and float payloads round trip via dynamicdata");

    std::atomic<bool> received{false};
    DynamicData captured;

    Publisher<DynamicData> pub(IntraConf("dyn_numeric_type", "", 0, "queue"));
    Subscriber<DynamicData> sub("intra://dyn_numeric_type");

    sub.listen([&](const DynamicData& d) {
      captured = d;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    SUBCASE("double payload") {
      DynamicData d;
      d.load("my_dbl", 3.14);
      CHECK(pub.publish(d));

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
      double val = 0.0;
      CHECK(captured.convert(val));
      CHECK(val == 3.14);
    }

    SUBCASE("float payload") {
      DynamicData d;
      d.load("my_flt", 2.718f);
      CHECK(pub.publish(d));

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
      float val = 0.0f;
      CHECK(captured.convert(val));
      CHECK(val == 2.718f);
    }
  }

  TEST_CASE("type name is preserved after load and round trip") {
    MESSAGE("[intra-dynamicdata] type name is preserved after load and round trip");

    std::atomic<bool> received{false};
    DynamicData captured;

    Publisher<DynamicData> pub(IntraConf("dyn_typename", "", 0, "queue"));
    Subscriber<DynamicData> sub("intra://dyn_typename");

    sub.listen([&](const DynamicData& d) {
      captured = d;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    DynamicData d;
    d.load("sensor_data", 42);
    CHECK(pub.publish(d));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
    CHECK(captured.get_type() == "sensor_data");
    CHECK_FALSE(captured.is_empty());
  }

  TEST_CASE("multiple subscribers each receive the same dynamicdata") {
    MESSAGE("[intra-dynamicdata] multiple subscribers each receive the same dynamicdata");

    std::atomic<int> count{0};
    std::atomic<bool> type_ok{true};

    Publisher<DynamicData> pub(IntraConf("dyn_multisub", "", 0, "queue"));
    Subscriber<DynamicData> sub1("intra://dyn_multisub");
    Subscriber<DynamicData> sub2("intra://dyn_multisub");

    auto cb = [&](const DynamicData& d) {
      if (d.get_type() != "ping") {
        type_ok.store(false, std::memory_order_relaxed);
      }

      count.fetch_add(1, std::memory_order_relaxed);
    };

    sub1.listen(cb);
    sub2.listen(cb);

    CHECK(pub.wait_for_subscribers(1s));
    std::this_thread::sleep_for(30ms);

    DynamicData d;
    d.load("ping", 1);
    CHECK(pub.publish(d));

    std::this_thread::sleep_for(50ms);
    CHECK(count.load() >= 2);
    CHECK(type_ok.load(std::memory_order_acquire));
  }

  TEST_CASE("serializer detects dynamicdata via kDynamicType") {
    MESSAGE("[intra-dynamicdata] serializer detects dynamicdata via kDynamicType");

    static constexpr auto kType = Serializer::get_type_of<DynamicData>();
    CHECK_EQ(kType, Serializer::kDynamicType);
  }

  TEST_CASE("get data returns non-empty bytes after load") {
    MESSAGE("[intra-dynamicdata] get data returns non-empty bytes after load");

    DynamicData d;
    d.load("chk_data", 123);

    CHECK_FALSE(d.is_empty());
    CHECK_FALSE(d.get_data().empty());
    CHECK_FALSE(d.get_type().empty());
  }
}

TEST_SUITE("intra-zerocopy") {
  TEST_CASE("rawdata round trip preserves payload bytes and header seq") {
    MESSAGE("[intra-zerocopy] rawdata round trip preserves payload bytes and header seq");

    std::atomic<bool> received{false};
    zerocopy::RawData captured;

    Publisher<zerocopy::RawData> pub(IntraConf("zc_raw1", "", 0, "queue"));
    Subscriber<zerocopy::RawData> sub("intra://zc_raw1");

    sub.listen([&](const zerocopy::RawData& d) {
      captured.deep_copy(d);
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    zerocopy::RawData rd;
    rd.header.seq = 7;
    rd.create(4);
    const_cast<uint8_t*>(rd.data())[0] = 0xDE;
    const_cast<uint8_t*>(rd.data())[1] = 0xAD;
    const_cast<uint8_t*>(rd.data())[2] = 0xBE;
    const_cast<uint8_t*>(rd.data())[3] = 0xEF;
    CHECK(pub.publish(rd));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
    REQUIRE_EQ(captured.size(), 4u);
    CHECK_EQ(captured.header.seq, 7u);
    CHECK_EQ(captured.data()[0], 0xDEu);
    CHECK_EQ(captured.data()[3], 0xEFu);
  }

  TEST_CASE("cameraframe round trip with width height and format preserved") {
    MESSAGE("[intra-zerocopy] cameraframe round trip with width height and format preserved");

    std::atomic<bool> received{false};
    zerocopy::CameraFrame captured;

    Publisher<zerocopy::CameraFrame> pub(IntraConf("zc_cam1", "", 0, "queue"));
    Subscriber<zerocopy::CameraFrame> sub("intra://zc_cam1");

    sub.listen([&](const zerocopy::CameraFrame& f) {
      captured.deep_copy(f);
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    zerocopy::CameraFrame frame;
    frame.set_width(640);
    frame.set_height(480);
    frame.set_format(zerocopy::CameraFrame::kFormatNv12);
    frame.set_channel(2);
    frame.create(640 * 480);
    CHECK(pub.publish(frame));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
    CHECK_EQ(captured.width(), 640u);
    CHECK_EQ(captured.height(), 480u);
    CHECK_EQ(captured.format(), zerocopy::CameraFrame::kFormatNv12);
    CHECK_EQ(captured.channel(), 2u);
    CHECK_EQ(captured.size(), 640u * 480u);
  }

  TEST_CASE("pointcloud round trip preserves point count and xyz values") {
    MESSAGE("[intra-zerocopy] pointcloud round trip preserves point count and xyz values");

    std::atomic<bool> received{false};
    zerocopy::PointCloud captured;

    Publisher<zerocopy::PointCloud> pub(IntraConf("zc_pc1", "", 0, "queue"));
    Subscriber<zerocopy::PointCloud> sub("intra://zc_pc1");

    sub.listen([&](const zerocopy::PointCloud& pc) {
      captured.deep_copy(pc);
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<float>(10, {"intensity"}));
    REQUIRE(pc.push_value_v3f(1.0f, 2.0f, 3.0f, 0.5f));
    REQUIRE(pc.push_value_v3f(4.0f, 5.0f, 6.0f, 0.8f));
    CHECK(pub.publish(pc));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
    CHECK_EQ(captured.size(), 2u);

    auto km = captured.get_key_map();
    float x = captured.get_value<float>(0, km, "x");
    float z = captured.get_value<float>(1, km, "z");
    CHECK(x == 1.0f);
    CHECK(z == 6.0f);
  }

  TEST_CASE("proxydata round trip preserves url and hostname") {
    MESSAGE("[intra-zerocopy] proxydata round trip preserves url and hostname");

    std::atomic<bool> received{false};
    zerocopy::ProxyData captured;

    Publisher<zerocopy::ProxyData> pub(IntraConf("zc_proxy1", "", 0, "queue"));
    Subscriber<zerocopy::ProxyData> sub("intra://zc_proxy1");

    sub.listen([&](const zerocopy::ProxyData& pd) {
      captured.deep_copy(pd);
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    Bytes raw_payload{0x01, 0x02, 0x03};
    zerocopy::ProxyData pd;
    pd.set_control_id(42);
    pd.create(raw_payload, "dds://my/topic", "demo.proto.Msg", 1, "host01");
    CHECK(pub.publish(pd));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
    CHECK(captured.url() == "dds://my/topic");
    CHECK(captured.hostname() == "host01");
    CHECK_EQ(captured.control_id(), 42u);
  }
}

#if defined(VLINK_TEST_SUPPORT_FLATBUFFERS)

TEST_SUITE("intra-flatbuffers") {
  TEST_CASE("flatbuffers message round trips type and value fields") {
    MESSAGE("[intra-flatbuffers] flatbuffers message round trips type and value fields");

    std::atomic<bool> received{false};
    uint32_t captured_type = 0;
    std::string captured_value;

    Publisher<fbs::MessageT> pub(IntraConf("intra_fbs_rt", "", 0, "queue"));
    Subscriber<fbs::MessageT> sub("intra://intra_fbs_rt");

    sub.listen([&](const fbs::MessageT& m) {
      captured_type = m.type;
      captured_value = m.value;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    fbs::MessageT msg;
    msg.type = 7u;
    msg.value = "flatbuf_round_trip";
    CHECK(pub.publish(msg));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
    CHECK_EQ(captured_type, 7u);
    CHECK_EQ(captured_value, "flatbuf_round_trip");
  }

  TEST_CASE("multiple subscribers all receive flatbuffers message") {
    MESSAGE("[intra-flatbuffers] multiple subscribers all receive flatbuffers message");

    std::atomic<int> count{0};

    Publisher<fbs::MessageT> pub(IntraConf("intra_fbs_multi", "", 0, "queue"));
    Subscriber<fbs::MessageT> sub1("intra://intra_fbs_multi");
    Subscriber<fbs::MessageT> sub2("intra://intra_fbs_multi");

    auto cb = [&](const fbs::MessageT& /*m*/) { count.fetch_add(1, std::memory_order_relaxed); };
    sub1.listen(cb);
    sub2.listen(cb);

    CHECK(pub.wait_for_subscribers(1s));

    fbs::MessageT msg;
    msg.type = 1u;
    msg.value = "multi_sub";
    pub.publish(msg);

    std::this_thread::sleep_for(100ms);
    CHECK(count.load() >= 2);
  }

  TEST_CASE("large flatbuffers string value survives round trip") {
    MESSAGE("[intra-flatbuffers] large flatbuffers string value survives round trip");

    std::atomic<bool> received{false};
    std::string captured;

    Publisher<fbs::MessageT> pub(IntraConf("intra_fbs_large", "", 0, "queue"));
    Subscriber<fbs::MessageT> sub("intra://intra_fbs_large");

    sub.listen([&](const fbs::MessageT& m) {
      captured = m.value;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    fbs::MessageT msg;
    msg.type = 2u;
    msg.value = std::string(4096, 'X');
    pub.publish(msg);

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
    CHECK_EQ(captured.size(), 4096u);
    CHECK_EQ(captured[0], 'X');
  }

  TEST_CASE("flatbuffers message with empty string field delivers correctly") {
    MESSAGE("[intra-flatbuffers] flatbuffers message with empty string field delivers correctly");

    std::atomic<bool> received{false};
    std::string captured = "not_empty";

    Publisher<fbs::MessageT> pub(IntraConf("intra_fbs_empty", "", 0, "queue"));
    Subscriber<fbs::MessageT> sub("intra://intra_fbs_empty");

    sub.listen([&](const fbs::MessageT& m) {
      captured = m.value;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    fbs::MessageT msg;
    msg.type = 0u;
    msg.value = "";
    pub.publish(msg);

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
    CHECK(captured.empty());
  }
}

#endif  // VLINK_TEST_SUPPORT_FLATBUFFERS

#if defined(VLINK_TEST_SUPPORT_SECURITY)

namespace {}  // namespace

TEST_SUITE("intra-security") {
  TEST_CASE("security publisher is rejected") {
    MESSAGE("[intra-security] security publisher is rejected");

    CHECK_THROWS(SecurityPublisher<int>(IntraConf("ev_sec_pub", "", 0, "queue")));
  }

  TEST_CASE("security subscriber is rejected") {
    MESSAGE("[intra-security] security subscriber is rejected");

    CHECK_THROWS(SecuritySubscriber<int>("intra://ev_sec_sub"));
  }

  TEST_CASE("asymmetric security config is rejected") {
    MESSAGE("[intra-security] asymmetric security config is rejected");

    Security::Config config;
    config.public_key_pem = "unsupported";

    CHECK_THROWS(SecurityPublisher<Bytes>(IntraConf("ev_sec_asymmetric", "", 0, "queue"), std::move(config)));
  }
}

#endif  // VLINK_TEST_SUPPORT_SECURITY

namespace {

struct IntraPodMsg {
  int32_t a;
  int32_t b;
  uint64_t c;
};

static_assert(std::is_trivial_v<IntraPodMsg>);
static_assert(std::is_standard_layout_v<IntraPodMsg>);

}  // namespace

TEST_SUITE("intra-serializers") {
  TEST_CASE("pod struct with multiple fields round trips via intra") {
    MESSAGE("[intra-serializers] pod struct with multiple fields round trips via intra");

    std::atomic<bool> received{false};
    IntraPodMsg captured{};

    Publisher<IntraPodMsg> pub(IntraConf("ser_pod_rt", "", 0, "queue"));
    Subscriber<IntraPodMsg> sub("intra://ser_pod_rt");

    sub.listen([&](const IntraPodMsg& m) {
      captured = m;
      received.store(true, std::memory_order_release);
    });

    CHECK(pub.wait_for_subscribers(1s));

    IntraPodMsg msg;
    msg.a = -42;
    msg.b = 1337;
    msg.c = 0xDEADBEEFCAFEBABEULL;
    CHECK(pub.publish(msg));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 1s));
    CHECK_EQ(captured.a, -42);
    CHECK_EQ(captured.b, 1337);
    CHECK_EQ(captured.c, 0xDEADBEEFCAFEBABEULL);
  }

  TEST_CASE("pod struct serializer type is kStandardType") {
    MESSAGE("[intra-serializers] pod struct serializer type is kStandardType");

    static constexpr auto kType = Serializer::get_type_of<IntraPodMsg>();
    CHECK_EQ(kType, Serializer::kStandardType);
    CHECK(Serializer::is_supported(kType));
  }

  TEST_CASE("const char pointer serializer trait is kCharsType") {
    MESSAGE("[intra-serializers] const char pointer serializer trait is kCharsType");

    static constexpr auto kType = Serializer::get_type_of<const char*>();
    CHECK_EQ(kType, Serializer::kCharsType);
    CHECK(Serializer::is_supported(kType));
  }

#if defined(VLINK_TEST_SUPPORT_PROTOBUF)
  TEST_CASE("proto pointer serializer trait is kProtoPtrType") {
    MESSAGE("[intra-serializers] proto pointer serializer trait is kProtoPtrType");

    static constexpr auto kType = Serializer::get_type_of<pb::Message*>();
    CHECK_EQ(kType, Serializer::kProtoPtrType);
    CHECK(Serializer::is_supported(kType));
    CHECK_EQ(Serializer::get_schema_type<pb::Message*>(), SchemaType::kProtobuf);
  }
#endif

#if defined(VLINK_TEST_SUPPORT_FLATBUFFERS)
  TEST_CASE("flat ptr serializer trait is kFlatPtrType") {
    MESSAGE("[intra-serializers] flat ptr serializer trait is kFlatPtrType");

    static constexpr auto kType = Serializer::get_type_of<fbs::Message*>();
    CHECK_EQ(kType, Serializer::kFlatPtrType);
    CHECK(Serializer::is_supported(kType));
    CHECK_EQ(Serializer::get_schema_type<fbs::Message*>(), SchemaType::kFlatbuffers);
  }
#endif
}

TEST_SUITE("intra-lifecycle") {
  TEST_CASE("getter wait_for_value times out when no setter writes") {
    MESSAGE("[intra-lifecycle] getter wait_for_value times out when no setter writes");

    Getter<int> getter(IntraConf("lc_getter_timeout", "", 0, "queue"));
    CHECK_FALSE(getter.wait_for_value(30ms));
  }

  TEST_CASE("wait_for_subscribers with short timeout returns false when no subscriber") {
    MESSAGE("[intra-lifecycle] wait_for_subscribers with short timeout returns false when no subscriber");

    Publisher<int> pub(IntraConf("lc_no_sub_timeout", "", 0, "queue"));
    CHECK_FALSE(pub.wait_for_subscribers(30ms));
  }

  TEST_CASE("rapid create destroy publisher subscriber pairs over 50 iterations completes without crash") {
    MESSAGE(
        "[intra-lifecycle] rapid create destroy publisher subscriber pairs over 50 iterations completes without crash");

    for (int i = 0; i < 50; ++i) {
      Publisher<int> pub(IntraConf("lc_churn50", "", 0, "queue"));
      Subscriber<int> sub("intra://lc_churn50");

      REQUIRE(sub.listen([](const int& /*v*/) {}));
      REQUIRE(pub.publish(i, true));
    }
  }

  TEST_CASE("publisher constructed with kWithoutInit destructs safely without use") {
    MESSAGE("[intra-lifecycle] publisher constructed with kWithoutInit destructs safely without use");

    Publisher<int> pub(IntraConf("lc_no_init", "", 0, "queue"), InitType::kWithoutInit);
    (void)pub;
  }

  TEST_CASE("subscriber constructed with kWithoutInit destructs safely without use") {
    MESSAGE("[intra-lifecycle] subscriber constructed with kWithoutInit destructs safely without use");

    Subscriber<int> sub(IntraConf("lc_sub_no_init", "", 0, "queue"), InitType::kWithoutInit);
    (void)sub;
  }
}

TEST_SUITE("intra-qos") {
  TEST_CASE("Qos struct can be constructed and customised for intra usage") {
    MESSAGE("[intra-qos] Qos struct can be constructed and customised for intra usage");

    Qos qos;
    qos.reliability.kind = Qos::Reliability::kBestEffort;
    qos.history.kind = Qos::History::kKeepLast;
    qos.history.depth = 8;

    CHECK_EQ(qos.reliability.kind, Qos::Reliability::kBestEffort);
    CHECK_EQ(qos.history.kind, Qos::History::kKeepLast);
    CHECK_EQ(qos.history.depth, 8);
  }

  TEST_CASE("IntraConf carries pipeline depth as an integer field") {
    MESSAGE("[intra-qos] IntraConf carries pipeline depth as an integer field");

    IntraConf conf("intra_pipeline_test", "event", 16, "queue");

    CHECK_EQ(conf.pipeline, 16);
    CHECK_EQ(conf.get_transport_type(), TransportType::kIntra);
  }

  TEST_CASE("rapid publishes with small pipeline still deliver at least one sample") {
    MESSAGE("[intra-qos] rapid publishes with small pipeline still deliver at least one sample");

    std::atomic<int> count{0};

    Publisher<int> pub(IntraConf("intra_qos_smoke", "", 0, "queue"));
    Subscriber<int> sub("intra://intra_qos_smoke");

    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });
    CHECK(pub.wait_for_subscribers(1s));

    for (int i = 0; i < 4; ++i) {
      pub.publish(i);
      std::this_thread::sleep_for(10ms);
    }

    std::this_thread::sleep_for(50ms);
    CHECK(count.load() >= 1);
  }
}

#endif  // VLINK_SUPPORT_INTRA

// NOLINTEND
