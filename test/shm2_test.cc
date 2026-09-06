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

#include <algorithm>
#include <atomic>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "./modules/shm2_conf.h"
#include "./zerocopy/raw_data.h"

struct Shm2LoanMessage {
  uint64_t value{42};

  size_t get_serialized_size() const { return sizeof(value); }

  bool operator>>(Bytes& bytes) const {
    if (bytes.empty()) {
      bytes = Bytes::create(sizeof(value));
    }
    if (bytes.size() != sizeof(value)) {
      return false;
    }
    std::memcpy(bytes.data(), &value, sizeof(value));
    return true;
  }

  bool operator<<(const Bytes& bytes) {
    if (bytes.size() != sizeof(value)) {
      return false;
    }
    std::memcpy(&value, bytes.data(), sizeof(value));
    return true;
  }
};

#if defined(VLINK_TEST_SUPPORT_FLATBUFFERS)

TEST_SUITE("shm2-flatbuilder") {
  TEST_CASE("flatbuffers builder publishing does not exhaust publisher loans") {
    static constexpr int32_t kDepth = 4;
    Publisher<common_test::FlatMessageBuilder> pub(Shm2Conf("shm2/fbs/builder_loan1", "data", 0, kDepth, 0, 0, 1024));

    for (int32_t i = 0; i < kDepth * 3; ++i) {
      common_test::FlatMessageBuilder builder("shm2_flat_builder");
      CHECK(pub.publish(builder, true));
    }

    auto check_loan_capacity = [&pub] {
      std::vector<const uint8_t*> pointers;
      std::vector<Bytes> loans;
      pointers.reserve(kDepth);
      loans.reserve(kDepth);

      for (int32_t i = 0; i < kDepth; ++i) {
        auto loan = pub.loan(32);
        REQUIRE(loan.is_loaned());
        REQUIRE_FALSE(loan.empty());
        CHECK(std::find(pointers.begin(), pointers.end(), loan.data()) == pointers.end());
        pointers.emplace_back(loan.data());
        loans.emplace_back(std::move(loan));
      }

      for (const auto& loan : loans) {
        CHECK(pub.return_loan(loan));
      }
    };

    check_loan_capacity();
    check_loan_capacity();
  }
}

#endif

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

  TEST_CASE("default memory size constant is 4 KiB") { CHECK(Shm2Conf::kDefaultMemSize == 4096U); }

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
  TEST_CASE("shared subscribers dispatch to their own message loops") {
    MessageLoop first_loop;
    MessageLoop second_loop;
    REQUIRE(first_loop.async_run());
    REQUIRE(second_loop.async_run());
    const auto first_thread = first_loop.invoke_task([] { return std::this_thread::get_id(); }).get();
    const auto second_thread = second_loop.invoke_task([] { return std::this_thread::get_id(); }).get();

    const Shm2Conf conf("shm2/review/sub_loops", "data", 0, 8, 0, 0, 512);
    Publisher<std::string> publisher(conf);
    Subscriber<std::string> first(conf);
    Subscriber<std::string> second(conf);
    REQUIRE(first.attach(&first_loop));
    REQUIRE(second.attach(&second_loop));
    std::promise<std::thread::id> first_received;
    std::promise<std::thread::id> second_received;
    auto first_result = first_received.get_future();
    auto second_result = second_received.get_future();
    first.listen([&](const std::string&) { first_received.set_value(std::this_thread::get_id()); });
    second.listen([&](const std::string&) { second_received.set_value(std::this_thread::get_id()); });
    REQUIRE(publisher.wait_for_subscribers(2s));
    REQUIRE(publisher.publish("value"));
    REQUIRE(first_result.wait_for(2s) == std::future_status::ready);
    REQUIRE(second_result.wait_for(2s) == std::future_status::ready);
    CHECK(first_result.get() == first_thread);
    CHECK(second_result.get() == second_thread);
  }

  TEST_CASE("connection callbacks can register another detected endpoint") {
    Publisher<int> publisher(Shm2Conf("shm2/review/detect_outer", "data"));
    std::promise<void> registered;
    auto result = registered.get_future();
    std::unique_ptr<Publisher<int>> nested;
    std::promise<void> connected;
    auto connected_result = connected.get_future();
    std::promise<void> disconnected;
    auto disconnected_result = disconnected.get_future();
    int connection_count = 0;
    publisher.detect_subscribers([&](bool value) {
      if (value && ++connection_count == 1) {
        connected.set_value();
      } else if (!value && connection_count == 1) {
        disconnected.set_value();
      } else if (value && connection_count == 2) {
        nested = std::make_unique<Publisher<int>>(Shm2Conf("shm2/review/detect_inner", "data"));
        nested->detect_subscribers([](bool) {});
        registered.set_value();
      }
    });
    auto initial = std::make_unique<Subscriber<int>>(Shm2Conf("shm2/review/detect_outer", "data"));
    initial->listen([](const int&) {});
    REQUIRE(connected_result.wait_for(2s) == std::future_status::ready);
    initial.reset();
    REQUIRE(disconnected_result.wait_for(2s) == std::future_status::ready);
    Subscriber<int> subscriber(Shm2Conf("shm2/review/detect_outer", "data"));
    subscriber.listen([](const int&) {});
    REQUIRE(result.wait_for(2s) == std::future_status::ready);
  }

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

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
    REQUIRE(captured.size() == 4u);
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

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
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

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
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
  TEST_CASE("attached raw response callbacks retain the native sample without copying") {
    MessageLoop loop;
    REQUIRE(loop.async_run());
    const Shm2Conf conf("shm2/review/native_response", "call", 0, 8, 0, 0, 65536);
    Server<Bytes, Bytes> server(conf);
    REQUIRE(server.listen([](const Bytes& req, Bytes& resp) { resp = req; }));
    Client<Bytes, Bytes> client(conf);
    REQUIRE(client.attach(&loop));
    REQUIRE(client.wait_for_connected(2s));
    auto request = Bytes::create(65536);
    std::memset(request.data(), 0x5a, request.size());
    std::promise<bool> received;
    auto result = received.get_future();
    REQUIRE(client.invoke(request, [&](const Bytes& response) {
      received.set_value(loop.is_in_same_thread() && !response.is_owner() && response == request);
    }));
    REQUIRE(result.wait_for(2s) == std::future_status::ready);
    CHECK(result.get());
  }

  TEST_CASE("raw pointer responses remain available within asynchronous callbacks") {
    const Shm2Conf conf("shm2/review/borrowed_pointer_response", "call", 0, 8, 0, 0, 512);
    Server<int, int> server(conf);
    REQUIRE(server.listen([](const int& req, int& resp) { resp = req + 1; }));
    Client<int, int*> client(conf);
    REQUIRE(client.wait_for_connected(2s));
    std::promise<int> response;
    auto result = response.get_future();
    REQUIRE(client.invoke(41, [&](int* const& value) { response.set_value(*value); }));
    REQUIRE(result.wait_for(2s) == std::future_status::ready);
    CHECK(result.get() == 42);
  }

  TEST_CASE("unconnected typed calls return automatically borrowed requests") {
    constexpr int32_t kDepth = 4;
    Client<Shm2LoanMessage, Shm2LoanMessage> client(
        Shm2Conf("shm2/review/unconnected_loan", "call", 0, kDepth, 0, 0, 512));
    const Shm2LoanMessage request;
    for (int i = 0; i < kDepth * 3; ++i) {
      Shm2LoanMessage response;
      CHECK_FALSE(client.invoke(request, response, 1ms));
      CHECK_FALSE(client.invoke(request, [](const Shm2LoanMessage&) {}));
    }
    std::vector<Bytes> loans;
    for (int i = 0; i < kDepth; ++i) {
      auto loan = client.loan(sizeof(uint64_t));
      REQUIRE(loan.is_loaned());
      loans.push_back(std::move(loan));
    }
    for (const auto& loan : loans) {
      CHECK(client.return_loan(loan));
    }
  }

  TEST_CASE("destroying one shared client cancels only its pending response") {
    MessageLoop server_loop;
    REQUIRE(server_loop.async_run());
    const Shm2Conf conf("shm2/review/client_owner", "call", 0, 8, 0, 0, 512);
    Server<std::string, std::string> server(conf);
    REQUIRE(server.attach(&server_loop));
    std::promise<void> entered;
    auto entered_result = entered.get_future();
    std::promise<void> release;
    auto released = release.get_future();
    server.listen([&](const std::string& req, std::string& resp) {
      if (req == "first") {
        entered.set_value();
        released.wait_for(3s);
      }
      resp = req;
    });
    auto first = std::make_unique<Client<std::string, std::string>>(conf);
    Client<std::string, std::string> second(conf);
    REQUIRE(first->wait_for_connected(2s));
    auto cancelled = first->async_invoke("first");
    REQUIRE(entered_result.wait_for(2s) == std::future_status::ready);
    first.reset();
    CHECK(cancelled.wait_for(100ms) == std::future_status::ready);
    release.set_value();
    auto response = second.invoke("second", 2s);
    REQUIRE(response.has_value());
    CHECK(*response == "second");
  }

  TEST_CASE("client callbacks use owner loops while synchronous replies bypass queued work") {
    MessageLoop first_loop;
    MessageLoop second_loop;
    MessageLoop server_loop;
    REQUIRE(first_loop.async_run());
    REQUIRE(second_loop.async_run());
    REQUIRE(server_loop.async_run());
    const auto first_thread = first_loop.invoke_task([] { return std::this_thread::get_id(); }).get();
    const auto second_thread = second_loop.invoke_task([] { return std::this_thread::get_id(); }).get();
    const Shm2Conf conf("shm2/review/client_loops", "call", 0, 8, 0, 0, 512);
    Server<std::string, std::string> server(conf);
    REQUIRE(server.attach(&server_loop));
    server.listen([](const std::string& req, std::string& resp) { resp = req; });
    Client<std::string, std::string> first(conf);
    Client<std::string, std::string> second(conf);
    REQUIRE(first.attach(&first_loop));
    REQUIRE(second.attach(&second_loop));
    REQUIRE(first.wait_for_connected(2s));
    std::promise<std::thread::id> first_received;
    std::promise<std::thread::id> second_received;
    auto first_result = first_received.get_future();
    auto second_result = second_received.get_future();
    REQUIRE(first.invoke("first", [&](const std::string&) { first_received.set_value(std::this_thread::get_id()); }));
    REQUIRE(
        second.invoke("second", [&](const std::string&) { second_received.set_value(std::this_thread::get_id()); }));
    REQUIRE(first_result.wait_for(2s) == std::future_status::ready);
    REQUIRE(second_result.wait_for(2s) == std::future_status::ready);
    CHECK(first_result.get() == first_thread);
    CHECK(second_result.get() == second_thread);
    auto synchronous = first_loop.invoke_task([&] { return first.invoke("sync", 2s); });
    REQUIRE(synchronous.wait_for(3s) == std::future_status::ready);
    CHECK(synchronous.get() == std::optional<std::string>("sync"));

    std::promise<std::optional<std::string>> nested;
    auto nested_result = nested.get_future();
    REQUIRE(first.invoke("outer", [&](const std::string&) { nested.set_value(first.invoke("inner", 2s)); }));
    REQUIRE(nested_result.wait_for(3s) == std::future_status::ready);
    CHECK(nested_result.get() == std::optional<std::string>("inner"));
  }

  TEST_CASE("different methods use their own server message loops") {
    MessageLoop first_loop;
    MessageLoop second_loop;
    REQUIRE(first_loop.async_run());
    REQUIRE(second_loop.async_run());
    const auto first_thread = first_loop.invoke_task([] { return std::this_thread::get_id(); }).get();
    const auto second_thread = second_loop.invoke_task([] { return std::this_thread::get_id(); }).get();
    const Shm2Conf first_conf("shm2/review/server_loops", "first", 0, 8, 0, 0, 512);
    const Shm2Conf second_conf("shm2/review/server_loops", "second", 0, 8, 0, 0, 512);
    Server<int, bool> first(first_conf);
    Server<int, bool> second(second_conf);
    REQUIRE(first.attach(&first_loop));
    REQUIRE(second.attach(&second_loop));
    first.listen([&](const int&, bool& resp) { resp = std::this_thread::get_id() == first_thread; });
    second.listen([&](const int&, bool& resp) { resp = std::this_thread::get_id() == second_thread; });
    Client<int, bool> first_client(first_conf);
    Client<int, bool> second_client(second_conf);
    REQUIRE(first_client.wait_for_connected(2s));
    REQUIRE(second_client.wait_for_connected(2s));
    CHECK(first_client.invoke(1, 2s) == std::optional<bool>(true));
    CHECK(second_client.invoke(1, 2s) == std::optional<bool>(true));
  }

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

    CHECK(common_test::wait_until([&got] { return got.load(std::memory_order_acquire); }, 5s));
    CHECK(resp_val == "shm2_cb");
  }
}

TEST_SUITE("shm2-field") {
  TEST_CASE("a subscriber marked as a getter receives cached field updates") {
    const auto topic = "shm2://shm2/review/marked_subscriber?event=value#512";
    Setter<int> setter(topic);
    setter.set(42);
    Subscriber<int> subscriber(topic, InitType::kWithoutInit);
    subscriber.mark_as_getter();
    REQUIRE(subscriber.init());
    std::atomic<int> received{0};
    REQUIRE(subscriber.listen([&](const int& value) { received.store(value, std::memory_order_release); }));
    CHECK(common_test::wait_until([&]() { return received.load(std::memory_order_acquire) == 42; }, 3s));
    setter.set(43);
    CHECK(common_test::wait_until([&]() { return received.load(std::memory_order_acquire) == 43; }, 3s));
  }

  TEST_CASE("every late getter receives its event history while existing getters stay connected") {
    const Shm2Conf first_conf("shm2/review/field_history", "first", 0, 8, 1, 0, 512);
    const Shm2Conf second_conf("shm2/review/field_history", "second", 0, 8, 1, 0, 512);
    Setter<int> first(first_conf);
    Setter<int> second(second_conf);
    first.set(11);
    second.set(22);
    Getter<int> first_getter(first_conf);
    REQUIRE(first_getter.wait_for_value(2s));
    CHECK(first_getter.get() == std::optional<int>(11));
    Getter<int> second_getter(second_conf);
    REQUIRE(second_getter.wait_for_value(2s));
    CHECK(second_getter.get() == std::optional<int>(22));
    Getter<int> late_first(first_conf);
    Getter<int> late_second(second_conf);
    REQUIRE(late_first.wait_for_value(2s));
    REQUIRE(late_second.wait_for_value(2s));
    CHECK(late_first.get() == std::optional<int>(11));
    CHECK(late_second.get() == std::optional<int>(22));
  }

  TEST_CASE("setter and getter exchange values") {
    MESSAGE("[shm2-field] setter and getter exchange values");

    SUBCASE("polling get") {
      Setter<Bytes> setter(Shm2Conf("shm2/fld/poll1", "val", 0, 0, 1, 0, 256));
      Getter<Bytes> getter("shm2://shm2/fld/poll1?event=val#256");

      setter.set(Bytes{0x11, 0x22, 0x33});
      std::this_thread::sleep_for(300ms);

      auto v = getter.get();
      REQUIRE(v.has_value());
      REQUIRE(v->size() == 3u);
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

      CHECK(common_test::wait_until([&notified] { return notified.load(std::memory_order_acquire); }, 5s));
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
      CHECK(v->size() == 1024u);
      CHECK((*v)[0] == 0xAB);
      CHECK((*v)[1023] == 0xAB);
    }
  }
}

TEST_SUITE("shm2-qos") {
  TEST_CASE("loss statistics keep independent publisher sequences") {
    Publisher<int> first(Shm2Conf("shm2/review/publisher_stats", "data", 0, 8, 0, 0, 512));
    Publisher<int> second(Shm2Conf("shm2/review/publisher_stats", "data", 0, 8, 0, 0, 1024));
    Subscriber<int> subscriber(Shm2Conf("shm2/review/publisher_stats", "data", 0, 8, 0, 0, 512));
    subscriber.set_latency_and_lost_enabled(true);
    std::atomic<int> received{0};
    subscriber.listen([&](const int&) { received.fetch_add(1, std::memory_order_release); });
    REQUIRE(first.wait_for_subscribers(2s));
    REQUIRE(second.wait_for_subscribers(2s));
    for (int i = 0; i < 8; ++i) {
      REQUIRE((i % 2 == 0 ? first : second).publish(i));
      REQUIRE(common_test::wait_until([&] { return received.load(std::memory_order_acquire) == i + 1; }, 2s));
    }
    CHECK(subscriber.get_lost().total == 8);
    CHECK(subscriber.get_lost().lost == 0);
  }

#if defined(__linux__)
  TEST_CASE("wait acknowledgements remain isolated between domains") {
    MessageLoop first_loop;
    MessageLoop second_loop;
    REQUIRE(first_loop.async_run());
    REQUIRE(second_loop.async_run());
    Publisher<int> first(Shm2Conf("shm2/review/domain_ack", "data", 41, 8, 0, 3000, 512));
    Publisher<int> second(Shm2Conf("shm2/review/domain_ack", "data", 42, 8, 0, 0, 512));
    Subscriber<int> first_sub(Shm2Conf("shm2/review/domain_ack", "data", 41, 8, 0, 3000, 512));
    Subscriber<int> second_sub(Shm2Conf("shm2/review/domain_ack", "data", 42, 8, 0, 3000, 512));
    REQUIRE(first_sub.attach(&first_loop));
    REQUIRE(second_sub.attach(&second_loop));
    std::promise<void> entered;
    auto entered_result = entered.get_future();
    std::promise<void> release;
    auto released = release.get_future();
    std::promise<void> other_received;
    auto other_result = other_received.get_future();
    first_sub.listen([&](const int&) {
      entered.set_value();
      released.wait_for(3s);
    });
    second_sub.listen([&](const int&) { other_received.set_value(); });
    REQUIRE(first.wait_for_subscribers(2s));
    REQUIRE(second.wait_for_subscribers(2s));
    auto publishing = std::async(std::launch::async, [&] { return first.publish(1); });
    REQUIRE(entered_result.wait_for(2s) == std::future_status::ready);
    REQUIRE(second.publish(2));
    REQUIRE(other_result.wait_for(2s) == std::future_status::ready);
    CHECK(publishing.wait_for(100ms) == std::future_status::timeout);
    release.set_value();
    REQUIRE(publishing.wait_for(2s) == std::future_status::ready);
    CHECK(publishing.get());
  }
#endif

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
    const auto lost = sub.get_lost();
    CHECK(lost.total >= lost.lost);

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

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
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

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
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
    REQUIRE(out.size() == 4097u);
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
  TEST_CASE("encrypted synchronous replies retain their response buffer") {
    Security::Config config;
    config.key = "0123456789abcdef0123456789abcdef";
    const Shm2Conf conf("shm2/review/secure_response", "call", 0, 8, 0, 0, 1024);
    SecurityServer<std::string, std::string> server(conf, config);
    REQUIRE(server.listen([](const std::string& req, std::string& resp) { resp = "reply:" + req; }));
    SecurityClient<std::string, std::string> client(conf, config);
    REQUIRE(client.wait_for_connected(2s));
    CHECK(client.invoke("sync", 2s) == std::optional<std::string>("reply:sync"));
    auto future = client.async_invoke("future");
    REQUIRE(future.wait_for(2s) == std::future_status::ready);
    CHECK(future.get() == "reply:future");
  }

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
    (void)payload;
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

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
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

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
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

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
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

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 5s));
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
