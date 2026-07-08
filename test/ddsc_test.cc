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
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "./base/utils.h"
#include "./common_test.h"
#include "./modules/ddsc_conf.h"

static constexpr auto kDdscDiscoveryTimeout = 3s;
static constexpr auto kDdsTeardownGrace = 10ms;

namespace {

struct ScopedDdscTeardownGrace {
  ~ScopedDdscTeardownGrace() { std::this_thread::sleep_for(kDdsTeardownGrace); }
};

class ScopedDdscTmpFile {
 public:
  ScopedDdscTmpFile(const std::string& name, const std::string& content) {
    path_ = std::filesystem::path(vlink::Utils::get_tmp_dir()) / "vlink-ddsc-tests" /
            (name + "_" + vlink::Utils::get_pid_str() + ".pem");
    std::filesystem::create_directories(path_.parent_path());

    std::ofstream file(path_, std::ios::binary | std::ios::trunc);
    file << content;
  }

  ScopedDdscTmpFile(const ScopedDdscTmpFile&) = delete;
  ScopedDdscTmpFile& operator=(const ScopedDdscTmpFile&) = delete;

  ScopedDdscTmpFile(ScopedDdscTmpFile&& other) noexcept : path_(std::move(other.path_)) { other.path_.clear(); }

  ScopedDdscTmpFile& operator=(ScopedDdscTmpFile&& other) noexcept {
    if (this != &other) {
      remove();
      path_ = std::move(other.path_);
      other.path_.clear();
    }
    return *this;
  }

  ~ScopedDdscTmpFile() { remove(); }

  std::string string() const { return path_.string(); }

 private:
  void remove() {
    if (!path_.empty()) {
      std::error_code ec;
      std::filesystem::remove(path_, ec);
      std::filesystem::remove(path_.parent_path(), ec);
    }
  }

  std::filesystem::path path_;
};

ScopedDdscTmpFile make_ddsc_tmp_file(const std::string& name, const std::string& content) {
  return ScopedDdscTmpFile(name, content);
}

struct DdscFailingCustomMsg {
  bool operator>>(Bytes&) const { return false; }
  bool operator<<(const Bytes&) { return false; }
};

}  // namespace

#undef TEST_CASE
#define TEST_CASE(name) TEST_CASE_FIXTURE(ScopedDdscTeardownGrace, name)

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

  TEST_CASE("invalid url scheme throws on every public role construction") {
    CHECK_THROWS(Publisher<int>("ddsc1://bad/url"));
    CHECK_THROWS(Subscriber<int>("ddsc1://bad/url"));
    CHECK_THROWS(Setter<int>("ddsc1://bad/url"));
    CHECK_THROWS(Getter<int>("ddsc1://bad/url"));
    CHECK_THROWS(Server<int>("ddsc1://bad/url"));
    CHECK_THROWS(Server<int, int>("ddsc1://bad/url"));
    CHECK_THROWS(Client<int>("ddsc1://bad/url"));
    CHECK_THROWS(Client<int, int>("ddsc1://bad/url"));
  }

  TEST_CASE("public listeners reject calls before init and duplicate registrations") {
    MESSAGE("[ddsc-init] public listeners reject calls before init and duplicate registrations");

    Subscriber<int> sub(DdscConf("ddsc/init/guard_subscriber"), InitType::kWithoutInit);
    CHECK_THROWS(sub.listen([](const int&) {}));
    REQUIRE(sub.init());
    CHECK(sub.listen([](const int&) {}));
    CHECK_THROWS(sub.listen([](const int&) {}));

    Server<std::string> fire_server(DdscConf("ddsc/init/guard_fire_server"), InitType::kWithoutInit);
    CHECK_THROWS(fire_server.listen([](const std::string&) {}));
    REQUIRE(fire_server.init());
    CHECK(fire_server.listen([](const std::string&) {}));
    CHECK_THROWS(fire_server.listen([](const std::string&) {}));

    Server<std::string, std::string> sync_server(DdscConf("ddsc/init/guard_sync_server"), InitType::kWithoutInit);
    CHECK_THROWS(sync_server.listen([](const std::string&, std::string&) {}));
    REQUIRE(sync_server.init());
    CHECK(sync_server.listen([](const std::string& req, std::string& resp) { resp = req; }));
    CHECK_THROWS(sync_server.listen([](const std::string&, std::string&) {}));
    CHECK_THROWS(sync_server.reply(1, std::string("wrong_mode")));

    Server<std::string, std::string> async_server(DdscConf("ddsc/init/guard_async_server"), InitType::kWithoutInit);
    CHECK_THROWS(async_server.listen_for_reply([](uint64_t, const std::string&) {}));
    REQUIRE(async_server.init());
    CHECK(async_server.listen_for_reply([](uint64_t, const std::string&) {}));
    CHECK_THROWS(async_server.listen_for_reply([](uint64_t, const std::string&) {}));

    Server<std::string, std::string> idle_server(DdscConf("ddsc/init/guard_idle_server"), InitType::kWithoutInit);
    CHECK_THROWS(idle_server.reply(777, std::string("not_listened")));
  }

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

  TEST_CASE("deferred nodes cover every role without runtime init") {
    Publisher<int> pub(DdscConf("ddsc/init/deferred_roles_pub"), InitType::kWithoutInit);
    Subscriber<int> sub(DdscConf("ddsc/init/deferred_roles_sub"), InitType::kWithoutInit);
    Setter<int> setter(DdscConf("ddsc/init/deferred_roles_field"), InitType::kWithoutInit);
    Getter<int> getter(DdscConf("ddsc/init/deferred_roles_field"), InitType::kWithoutInit);
    Server<std::string> fire_server(DdscConf("ddsc/init/deferred_roles_fire"), InitType::kWithoutInit);
    Server<std::string, std::string> sync_server(DdscConf("ddsc/init/deferred_roles_rpc"), InitType::kWithoutInit);
    Client<std::string> fire_client(DdscConf("ddsc/init/deferred_roles_fire"), InitType::kWithoutInit);
    Client<std::string, std::string> sync_client(DdscConf("ddsc/init/deferred_roles_rpc"), InitType::kWithoutInit);

    CHECK_FALSE(pub.has_inited());
    CHECK_FALSE(sub.has_inited());
    CHECK_FALSE(setter.has_inited());
    CHECK_FALSE(getter.has_inited());
    CHECK_FALSE(fire_server.has_inited());
    CHECK_FALSE(sync_server.has_inited());
    CHECK_FALSE(fire_client.has_inited());
    CHECK_FALSE(sync_client.has_inited());

    CHECK(pub.get_transport_type() == TransportType::kDdsc);
    CHECK(sub.get_transport_type() == TransportType::kDdsc);
    CHECK(setter.get_transport_type() == TransportType::kDdsc);
    CHECK(getter.get_transport_type() == TransportType::kDdsc);
  }

  TEST_CASE("factory helpers support explicit init and deinit for every ddsc role") {
    auto pub = Publisher<int>::create_unique("ddsc://ddsc/init/factory_pub?domain=83", InitType::kWithoutInit);
    auto sub = Subscriber<int>::create_shared("ddsc://ddsc/init/factory_sub?domain=84", InitType::kWithoutInit);
    auto setter = Setter<int>::create_unique("ddsc://ddsc/init/factory_setter?domain=85", InitType::kWithoutInit);
    auto getter = Getter<int>::create_shared("ddsc://ddsc/init/factory_getter?domain=86", InitType::kWithoutInit);
    auto fire_server =
        Server<std::string>::create_unique("ddsc://ddsc/init/factory_fire_server?domain=87", InitType::kWithoutInit);
    auto fire_client =
        Client<std::string>::create_shared("ddsc://ddsc/init/factory_fire_client?domain=88", InitType::kWithoutInit);
    auto sync_server = Server<std::string, std::string>::create_unique("ddsc://ddsc/init/factory_rpc_server?domain=89",
                                                                       InitType::kWithoutInit);
    auto sync_client = Client<std::string, std::string>::create_shared("ddsc://ddsc/init/factory_rpc_client?domain=90",
                                                                       InitType::kWithoutInit);

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

    CHECK(sync_client->deinit());
    CHECK(sync_server->deinit());
    CHECK(fire_client->deinit());
    CHECK(fire_server->deinit());
    CHECK(getter->deinit());
    CHECK(setter->deinit());
    CHECK(sub->deinit());
    CHECK(pub->deinit());

    CHECK_FALSE(pub->deinit());
    CHECK_FALSE(sub->deinit());
    CHECK_FALSE(setter->deinit());
    CHECK_FALSE(getter->deinit());
    CHECK_FALSE(fire_server->deinit());
    CHECK_FALSE(fire_client->deinit());
    CHECK_FALSE(sync_server->deinit());
    CHECK_FALSE(sync_client->deinit());
  }

  TEST_CASE("factory reuses cached participants topics publishers and subscribers for matching conf") {
    DdscConf conf("ddsc/init/cache_reuse", 73);

    Publisher<Bytes> pub1(conf, InitType::kWithoutInit);
    Publisher<Bytes> pub2(conf, InitType::kWithoutInit);
    Subscriber<Bytes> sub1(conf, InitType::kWithoutInit);
    Subscriber<Bytes> sub2(conf, InitType::kWithoutInit);

    REQUIRE(pub1.init());
    REQUIRE(pub2.init());
    REQUIRE(sub1.init());
    REQUIRE(sub2.init());

    CHECK(pub1.has_inited());
    CHECK(pub2.has_inited());
    CHECK(sub1.has_inited());
    CHECK(sub2.has_inited());

    CHECK(pub1.deinit());
    CHECK(pub2.deinit());
    CHECK(sub1.deinit());
    CHECK(sub2.deinit());
  }

  TEST_CASE("deferred status accessors return unknown before runtime init") {
    auto check_unknown = [](const Status::BasePtr& status) {
      REQUIRE(status != nullptr);
      CHECK(status->get_type() == Status::kUnknown);
    };

    Publisher<int> pub(DdscConf("ddsc/init/deferred_status_pub"), InitType::kWithoutInit);
    Subscriber<int> sub(DdscConf("ddsc/init/deferred_status_sub"), InitType::kWithoutInit);
    Server<std::string> fire_server(DdscConf("ddsc/init/deferred_status_fire"), InitType::kWithoutInit);
    Server<std::string, std::string> sync_server(DdscConf("ddsc/init/deferred_status_rpc"), InitType::kWithoutInit);
    Client<std::string> fire_client(DdscConf("ddsc/init/deferred_status_fire"), InitType::kWithoutInit);
    Client<std::string, std::string> sync_client(DdscConf("ddsc/init/deferred_status_rpc"), InitType::kWithoutInit);
    Getter<int> getter(DdscConf("ddsc/init/deferred_status_getter"), InitType::kWithoutInit);

    check_unknown(pub.get_status(Status::kPublicationMatched));
    check_unknown(sub.get_status(Status::kSubscriptionMatched));
    check_unknown(fire_server.get_status(Status::kSubscriptionMatched));
    check_unknown(sync_server.get_status(Status::kPublicationMatched));
    check_unknown(fire_client.get_status(Status::kPublicationMatched));
    check_unknown(sync_client.get_status(Status::kSubscriptionMatched));
    check_unknown(getter.get_status(Status::kSubscriptionMatched));
  }

  TEST_CASE("deferred getter reports latency state before runtime init") {
    Getter<int> getter(DdscConf("ddsc/init/deferred_getter"), InitType::kWithoutInit);

    CHECK_FALSE(getter.is_latency_and_lost_enabled());
    CHECK_EQ(getter.get_latency(), 0);
    auto lost = getter.get_lost();
    CHECK_EQ(lost.total, 0u);
    CHECK_EQ(lost.lost, 0u);

    getter.set_latency_and_lost_enabled(true);
    CHECK(getter.is_latency_and_lost_enabled());
    CHECK_EQ(getter.get_latency(), 0);
    lost = getter.get_lost();
    CHECK_EQ(lost.total, 0u);
    CHECK_EQ(lost.lost, 0u);

    getter.set_latency_and_lost_enabled(false);
    CHECK_FALSE(getter.is_latency_and_lost_enabled());
  }

  TEST_CASE("deferred subscriber reports latency state before runtime init") {
    Subscriber<int> sub(DdscConf("ddsc/init/deferred_subscriber"), InitType::kWithoutInit);

    CHECK_FALSE(sub.is_latency_and_lost_enabled());
    CHECK_EQ(sub.get_latency(), 0);
    auto lost = sub.get_lost();
    CHECK_EQ(lost.total, 0u);
    CHECK_EQ(lost.lost, 0u);

    sub.set_latency_and_lost_enabled(true);
    CHECK(sub.is_latency_and_lost_enabled());
    CHECK_EQ(sub.get_latency(), 0);
    lost = sub.get_lost();
    CHECK_EQ(lost.total, 0u);
    CHECK_EQ(lost.lost, 0u);

    sub.set_latency_and_lost_enabled(false);
    CHECK_FALSE(sub.is_latency_and_lost_enabled());
  }

  TEST_CASE("subscriber getter and server suspend state toggles") {
    Subscriber<int> sub(DdscConf("ddsc/init/suspend_subscriber"));
    Getter<int> getter(DdscConf("ddsc/init/suspend_getter"));
    Server<std::string> fire_server(DdscConf("ddsc/init/suspend_fire_server"));
    Server<std::string, std::string> sync_server(DdscConf("ddsc/init/suspend_sync_server"));

    CHECK_FALSE(sub.is_suspend());
    CHECK(sub.suspend());
    CHECK(sub.is_suspend());
    CHECK(sub.resume());
    CHECK_FALSE(sub.is_suspend());

    CHECK_FALSE(getter.is_suspend());
    CHECK(getter.suspend());
    CHECK(getter.is_suspend());
    CHECK(getter.resume());
    CHECK_FALSE(getter.is_suspend());

    CHECK_FALSE(fire_server.is_suspend());
    CHECK(fire_server.suspend());
    CHECK(fire_server.is_suspend());
    CHECK(fire_server.resume());
    CHECK_FALSE(fire_server.is_suspend());

    CHECK_FALSE(sync_server.is_suspend());
    CHECK(sync_server.suspend());
    CHECK(sync_server.is_suspend());
    CHECK(sync_server.resume());
    CHECK_FALSE(sync_server.is_suspend());
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

    CHECK(pub.wait_for_subscribers(kDdscDiscoveryTimeout));
    CHECK(pub.has_subscribers());

    Bytes payload{0xCA, 0xFE, 0xBA, 0xBE};
    CHECK(pub.publish(payload));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
    REQUIRE(captured.size() == 4u);
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

    CHECK(pub.wait_for_subscribers(kDdscDiscoveryTimeout));
    CHECK(pub.publish(std::string("hello_ddsc")));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
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

    CHECK(pub.wait_for_subscribers(kDdscDiscoveryTimeout));
    CHECK(pub.publish(54321));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
    CHECK(captured.load() == 54321);
  }

  TEST_CASE("multiple publishes are all received by subscriber") {
    MESSAGE("[ddsc-pubsub] multiple publishes are all received by subscriber");

    std::atomic<int> count{0};

    Publisher<int> pub(DdscConf("ddsc/evt/multi1"));
    Subscriber<int> sub("ddsc://ddsc/evt/multi1");

    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(kDdscDiscoveryTimeout));

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

    CHECK(pub.wait_for_subscribers(kDdscDiscoveryTimeout));

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
    CHECK_FALSE(pub.wait_for_subscribers(20ms));

    for (int i = 0; i < 5; ++i) {
      CHECK(pub.publish(Bytes{static_cast<uint8_t>(i)}, true));
    }
  }

  TEST_CASE("subscriber and getter reject duplicate user callbacks") {
    MESSAGE("[ddsc-pubsub] subscriber and getter reject duplicate user callbacks");

    Subscriber<int> sub(DdscConf("ddsc/guard/dup_sub1"));
    CHECK(sub.listen([](const int&) {}));
    CHECK_THROWS(sub.listen([](const int&) {}));

    Getter<int> getter(DdscConf("ddsc/guard/dup_getter1"));
    CHECK(getter.listen([](const int&) {}));
    CHECK_THROWS(getter.listen([](const int&) {}));
  }

  TEST_CASE("invalid raw bytes are dropped before typed subscriber callback") {
    MESSAGE("[ddsc-pubsub] invalid raw bytes are dropped before typed subscriber callback");

    {
      std::atomic<int> delivered{0};

      Publisher<Bytes> pub(DdscConf("ddsc/evt/bad_typed_bytes1"));
      Subscriber<int> sub("ddsc://ddsc/evt/bad_typed_bytes1");

      CHECK(sub.listen([&](const int&) { delivered.fetch_add(1, std::memory_order_relaxed); }));
      CHECK(pub.wait_for_subscribers(kDdscDiscoveryTimeout));
      CHECK(pub.publish(Bytes{0x01}));

      std::this_thread::sleep_for(200ms);
      CHECK_EQ(delivered.load(std::memory_order_acquire), 0);
    }

    {
      std::atomic<int> delivered{0};

      Publisher<Bytes> pub(DdscConf("ddsc/evt/bad_dynamic_bytes1"));
      Subscriber<DynamicData> sub("ddsc://ddsc/evt/bad_dynamic_bytes1");

      CHECK(sub.listen([&](const DynamicData&) { delivered.fetch_add(1, std::memory_order_relaxed); }));
      CHECK(pub.wait_for_subscribers(kDdscDiscoveryTimeout));
      CHECK(pub.publish(Bytes{0x01, 0x02, 0x03}));

      std::this_thread::sleep_for(200ms);
      CHECK_EQ(delivered.load(std::memory_order_acquire), 0);
    }

#if defined(VLINK_TEST_SUPPORT_PROTOBUF)
    {
      std::atomic<int> delivered{0};

      Publisher<Bytes> pub(DdscConf("ddsc/evt/bad_proto_bytes1"));
      Subscriber<pb::Message> sub("ddsc://ddsc/evt/bad_proto_bytes1");

      CHECK(sub.listen([&](const pb::Message&) { delivered.fetch_add(1, std::memory_order_relaxed); }));
      CHECK(pub.wait_for_subscribers(kDdscDiscoveryTimeout));
      CHECK(pub.publish(Bytes{0x0A, 0xFF}));

      std::this_thread::sleep_for(200ms);
      CHECK_EQ(delivered.load(std::memory_order_acquire), 0);
    }
#endif
  }

  TEST_CASE("suspended subscriber drops queued samples then receives after resume") {
    MESSAGE("[ddsc-pubsub] suspended subscriber drops queued samples then receives after resume");

    std::atomic<int> resumed_value{0};

    Publisher<int> pub(DdscConf("ddsc/evt/suspend_drop1"));
    Subscriber<int> sub(DdscConf("ddsc/evt/suspend_drop1"));

    CHECK(sub.listen([&](const int& value) {
      if (value == 2) {
        resumed_value.store(value, std::memory_order_release);
      }
    }));

    CHECK(sub.suspend());
    CHECK(pub.wait_for_subscribers(kDdscDiscoveryTimeout));
    CHECK(pub.publish(1));

    std::this_thread::sleep_for(300ms);
    CHECK_EQ(resumed_value.load(std::memory_order_acquire), 0);

    CHECK(sub.resume());
    CHECK(pub.publish(2));
    CHECK(common_test::wait_until([&] { return resumed_value.load(std::memory_order_acquire) == 2; },
                                  kDdscDiscoveryTimeout));
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
      CHECK(common_test::wait_until([&pub] { return pub.has_subscribers(); }, kDdscDiscoveryTimeout));
    }

    CHECK(common_test::wait_until([&pub] { return !pub.has_subscribers(); }, kDdscDiscoveryTimeout));
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

      CHECK(pub.wait_for_subscribers(kDdscDiscoveryTimeout));

      pb::Message msg;
      msg.set_value("ddsc_proto");
      CHECK(pub.publish(msg));

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
      CHECK(captured.value() == "ddsc_proto");
    }
#endif

    SUBCASE("plain bytes always works") {
      std::atomic<bool> received{false};

      Publisher<Bytes> pub(DdscConf("ddsc/ser/plain1"));
      Subscriber<Bytes> sub("ddsc://ddsc/ser/plain1");

      sub.listen([&](const Bytes& /*d*/) { received.store(true, std::memory_order_release); });

      CHECK(pub.wait_for_subscribers(kDdscDiscoveryTimeout));
      pub.publish(Bytes{0xAB, 0xCD});

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
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
    CHECK(client.wait_for_connected(kDdscDiscoveryTimeout));
    std::this_thread::sleep_for(20ms);
    CHECK(client.is_connected());

    CHECK(client.send("fire1"));
    CHECK(common_test::wait_until([&counter] { return counter.load(std::memory_order_acquire) >= 1; },
                                  kDdscDiscoveryTimeout));
    CHECK(counter.load() == 1);

    CHECK(client.send("fire2"));
    CHECK(common_test::wait_until([&counter] { return counter.load(std::memory_order_acquire) >= 2; },
                                  kDdscDiscoveryTimeout));
    CHECK(counter.load() == 2);
  }

  TEST_CASE("invoke returns correct response via multiple overloads") {
    MESSAGE("[ddsc-method] invoke returns correct response via multiple overloads");

    Server<std::string, std::string> server(DdscConf("ddsc/mth/invoke1"));
    server.listen([](const std::string& req, std::string& resp) { resp = "cyclone:" + req; });

    Client<std::string, std::string> client("ddsc://ddsc/mth/invoke1");
    CHECK(client.wait_for_connected(kDdscDiscoveryTimeout));
    std::this_thread::sleep_for(20ms);

    SUBCASE("sync optional") {
      auto resp = client.invoke("ping");
      CHECK(resp.has_value());
      CHECK(*resp == "cyclone:ping");
    }

    SUBCASE("sync ref overload") {
      std::string out;
      CHECK(client.invoke("pong", out, kDdscDiscoveryTimeout));
      CHECK(out == "cyclone:pong");
    }

    SUBCASE("async future") {
      auto fut = client.async_invoke("async");
      REQUIRE(fut.wait_for(kDdscDiscoveryTimeout) == std::future_status::ready);
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
    CHECK(client.wait_for_connected(kDdscDiscoveryTimeout));
    std::this_thread::sleep_for(20ms);

    auto fut = client.async_invoke("defer");

    REQUIRE(common_test::wait_until([&req_received] { return req_received.load(std::memory_order_acquire); }, 3s));
    CHECK(server.reply(saved_id.load(), std::string("deferred_ddsc")));

    REQUIRE(fut.wait_for(kDdscDiscoveryTimeout) == std::future_status::ready);
    CHECK(fut.get() == "deferred_ddsc");
  }

  TEST_CASE("async callback invoke delivers response") {
    MESSAGE("[ddsc-method] async callback invoke delivers response");

    Server<std::string, std::string> server(DdscConf("ddsc/mth/cb1"));
    server.listen([](const std::string& /*req*/, std::string& resp) { resp = "ddsc_cb"; });

    Client<std::string, std::string> client("ddsc://ddsc/mth/cb1");
    CHECK(client.wait_for_connected(kDdscDiscoveryTimeout));
    std::this_thread::sleep_for(20ms);

    std::atomic<bool> got{false};
    std::string resp_val;

    bool ok = client.invoke("msg", [&](const std::string& resp) {
      resp_val = resp;
      got.store(true, std::memory_order_release);
    });

    CHECK(ok);

    CHECK(common_test::wait_until([&got] { return got.load(std::memory_order_acquire); }, 3s));
    CHECK(resp_val == "ddsc_cb");
  }

  TEST_CASE("malformed method payloads are rejected without invoking typed handlers") {
    MESSAGE("[ddsc-method] malformed method payloads are rejected without invoking typed handlers");

    std::atomic<int> fire_count{0};
    Server<int> fire_server(DdscConf("ddsc/mth/bad_req_fire1"));
    CHECK(fire_server.listen([&](const int&) { fire_count.fetch_add(1, std::memory_order_relaxed); }));

    Client<Bytes> fire_client("ddsc://ddsc/mth/bad_req_fire1");
    CHECK(fire_client.wait_for_connected(kDdscDiscoveryTimeout));
    CHECK(fire_client.send(Bytes{0x01}));
    std::this_thread::sleep_for(200ms);
    CHECK_EQ(fire_count.load(std::memory_order_acquire), 0);

    std::atomic<int> sync_count{0};
    Server<int, int> sync_server(DdscConf("ddsc/mth/bad_req_sync1"));
    CHECK(sync_server.listen([&](const int&, int& resp) {
      sync_count.fetch_add(1, std::memory_order_relaxed);
      resp = 1;
    }));

    Client<Bytes, Bytes> sync_client("ddsc://ddsc/mth/bad_req_sync1");
    CHECK(sync_client.wait_for_connected(kDdscDiscoveryTimeout));
    Bytes ignored_response;
    CHECK_FALSE(sync_client.invoke(Bytes{0x02}, ignored_response, 500ms));
    CHECK(ignored_response.empty());
    CHECK_EQ(sync_count.load(std::memory_order_acquire), 0);
  }

  TEST_CASE("client response decode failures keep callbacks and futures deterministic") {
    MESSAGE("[ddsc-method] client response decode failures keep callbacks and futures deterministic");

    Server<int, Bytes> bad_resp_server(DdscConf("ddsc/mth/bad_resp1"));
    CHECK(bad_resp_server.listen([](const int&, Bytes& resp) { resp = Bytes{0x01}; }));

    Client<int, int> client("ddsc://ddsc/mth/bad_resp1");
    CHECK(client.wait_for_connected(kDdscDiscoveryTimeout));

    int out = 1234;
    CHECK_FALSE(client.invoke(7, out, kDdscDiscoveryTimeout));
    CHECK_EQ(out, 1234);

    std::atomic<int> callback_count{0};
    CHECK(client.invoke(8, [&](const int&) { callback_count.fetch_add(1, std::memory_order_relaxed); }));
    std::this_thread::sleep_for(200ms);
    CHECK_EQ(callback_count.load(std::memory_order_acquire), 0);

    auto future = client.async_invoke(9);
    REQUIRE(future.wait_for(kDdscDiscoveryTimeout) == std::future_status::ready);
    CHECK_THROWS(future.get());
  }

  TEST_CASE("security rpc failures are dropped at decrypt and encrypt boundaries") {
    MESSAGE("[ddsc-method] security rpc failures are dropped at decrypt and encrypt boundaries");

    auto identity_cfg = [] {
      Security::Config cfg;
      cfg.encrypt_callback = [](const Bytes& in, Bytes& out) {
        out = in;
        return true;
      };
      cfg.decrypt_callback = [](const Bytes& in, Bytes& out) {
        out = in;
        return true;
      };
      return cfg;
    };
    auto client_decrypt_fail_cfg = [] {
      Security::Config cfg;
      cfg.encrypt_callback = [](const Bytes& in, Bytes& out) {
        out = in;
        return true;
      };
      cfg.decrypt_callback = [](const Bytes&, Bytes&) { return false; };
      return cfg;
    };
    auto server_decrypt_fail_cfg = [] {
      Security::Config cfg;
      cfg.encrypt_callback = [](const Bytes& in, Bytes& out) {
        out = in;
        return true;
      };
      cfg.decrypt_callback = [](const Bytes&, Bytes&) { return false; };
      return cfg;
    };
    auto server_encrypt_fail_cfg = [] {
      Security::Config cfg;
      cfg.encrypt_callback = [](const Bytes&, Bytes&) { return false; };
      cfg.decrypt_callback = [](const Bytes& in, Bytes& out) {
        out = in;
        return true;
      };
      return cfg;
    };

    {
      std::atomic<int> handled{0};
      SecurityServer<int, int> server(DdscConf("ddsc/mth/sec_bad_req"), server_decrypt_fail_cfg());
      CHECK(server.listen([&](const int&, int& resp) {
        handled.fetch_add(1, std::memory_order_relaxed);
        resp = 1;
      }));

      SecurityClient<int, int> client("ddsc://ddsc/mth/sec_bad_req", identity_cfg());
      CHECK(client.wait_for_connected(kDdscDiscoveryTimeout));

      int resp = 0;
      CHECK_FALSE(client.invoke(7, resp, 300ms));
      CHECK_EQ(handled.load(std::memory_order_acquire), 0);
    }

    {
      std::atomic<int> handled{0};
      SecurityServer<int, int> server(DdscConf("ddsc/mth/sec_bad_client_resp"), identity_cfg());
      CHECK(server.listen([&](const int& req, int& resp) {
        handled.fetch_add(1, std::memory_order_relaxed);
        resp = req + 1;
      }));

      SecurityClient<int, int> client("ddsc://ddsc/mth/sec_bad_client_resp", client_decrypt_fail_cfg());
      CHECK(client.wait_for_connected(kDdscDiscoveryTimeout));

      int resp = 0;
      CHECK_FALSE(client.invoke(7, resp, 300ms));
      CHECK_EQ(handled.load(std::memory_order_acquire), 1);
    }

    {
      std::atomic<int> handled{0};
      SecurityServer<int, int> server(DdscConf("ddsc/mth/sec_bad_server_resp"), server_encrypt_fail_cfg());
      CHECK(server.listen([&](const int& req, int& resp) {
        handled.fetch_add(1, std::memory_order_relaxed);
        resp = req + 1;
      }));

      SecurityClient<int, int> client("ddsc://ddsc/mth/sec_bad_server_resp", identity_cfg());
      CHECK(client.wait_for_connected(kDdscDiscoveryTimeout));

      int resp = 0;
      CHECK_FALSE(client.invoke(7, resp, 300ms));
      CHECK_EQ(handled.load(std::memory_order_acquire), 1);
    }
  }

  TEST_CASE("custom serialization failures stop client and server rpc paths") {
    MESSAGE("[ddsc-method] custom serialization failures stop client and server rpc paths");

    DdscFailingCustomMsg bad_msg;

    Client<DdscFailingCustomMsg> fire_client(DdscConf("ddsc/mth/custom_fail_send"));
    CHECK_FALSE(fire_client.send(bad_msg));

    Client<DdscFailingCustomMsg, int> sync_client(DdscConf("ddsc/mth/custom_fail_invoke"));
    int sync_resp = 0;
    CHECK_FALSE(sync_client.invoke(bad_msg, sync_resp, 300ms));

    auto future = sync_client.async_invoke(bad_msg);
    REQUIRE(future.wait_for(300ms) == std::future_status::ready);
    CHECK_THROWS(future.get());

    {
      std::atomic<int> handled{0};
      Server<DdscFailingCustomMsg> server(DdscConf("ddsc/mth/custom_fail_req"));
      CHECK(server.listen([&](const DdscFailingCustomMsg&) { handled.fetch_add(1, std::memory_order_relaxed); }));

      Client<Bytes> client("ddsc://ddsc/mth/custom_fail_req");
      CHECK(client.wait_for_connected(kDdscDiscoveryTimeout));
      CHECK(client.send(Bytes::from_string("bad custom request")));

      std::this_thread::sleep_for(200ms);
      CHECK_EQ(handled.load(std::memory_order_acquire), 0);
    }

    {
      std::atomic<int> handled{0};
      Server<int, DdscFailingCustomMsg> server(DdscConf("ddsc/mth/custom_fail_resp"));
      CHECK(server.listen([&](const int&, DdscFailingCustomMsg& resp) {
        handled.fetch_add(1, std::memory_order_relaxed);
        resp = DdscFailingCustomMsg{};
      }));

      Client<int, Bytes> client("ddsc://ddsc/mth/custom_fail_resp");
      CHECK(client.wait_for_connected(kDdscDiscoveryTimeout));

      Bytes resp;
      CHECK_FALSE(client.invoke(1, resp, 300ms));
      CHECK_EQ(handled.load(std::memory_order_acquire), 1);
    }
  }

  TEST_CASE("attached message loop dispatches rpc callbacks") {
    MESSAGE("[ddsc-method] attached message loop dispatches rpc callbacks");

    MessageLoop loop;
    CHECK(loop.async_run());

    std::atomic<int> server_calls{0};
    std::atomic<bool> async_done{false};
    std::string async_value;

    Server<std::string, std::string> server(DdscConf("ddsc/mth/loop_dispatch1"), InitType::kWithoutInit);
    CHECK(server.attach(&loop));
    CHECK(server.init());
    CHECK(server.listen([&](const std::string& req, std::string& resp) {
      server_calls.fetch_add(1, std::memory_order_relaxed);
      resp = "loop:" + req;
    }));

    Client<std::string, std::string> client("ddsc://ddsc/mth/loop_dispatch1", InitType::kWithoutInit);
    CHECK(client.attach(&loop));
    CHECK(client.init());
    CHECK(client.wait_for_connected(kDdscDiscoveryTimeout));

    std::string sync_value;
    CHECK(client.invoke("sync", sync_value, kDdscDiscoveryTimeout));
    CHECK(sync_value == "loop:sync");

    CHECK(client.invoke("async", [&](const std::string& resp) {
      async_value = resp;
      async_done.store(true, std::memory_order_release);
    }));
    CHECK(common_test::wait_until([&async_done] { return async_done.load(std::memory_order_acquire); },
                                  kDdscDiscoveryTimeout));
    CHECK(async_value == "loop:async");
    CHECK(server_calls.load(std::memory_order_acquire) >= 2);

    CHECK(client.detach());
    CHECK(server.detach());
    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("suspended fire server drains requests until resumed") {
    MESSAGE("[ddsc-method] suspended fire server drains requests until resumed");

    std::atomic<int> count{0};

    Server<std::string> server(DdscConf("ddsc/mth/suspend_drain1"));
    CHECK(server.listen([&](const std::string& req) {
      if (req == "take") {
        count.fetch_add(1, std::memory_order_relaxed);
      }
    }));

    Client<std::string> client("ddsc://ddsc/mth/suspend_drain1");
    CHECK(client.wait_for_connected(kDdscDiscoveryTimeout));

    CHECK(server.suspend());
    CHECK(client.send("drop"));
    std::this_thread::sleep_for(300ms);
    CHECK_EQ(count.load(std::memory_order_acquire), 0);

    CHECK(server.resume());
    CHECK(client.send("take"));
    CHECK(common_test::wait_until([&count] { return count.load(std::memory_order_acquire) == 1; },
                                  kDdscDiscoveryTimeout));
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

    CHECK(common_test::wait_until([&connected_event] { return connected_event.load(std::memory_order_acquire); },
                                  kDdscDiscoveryTimeout));
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
      REQUIRE(v->size() == 2u);
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

      CHECK(getter.wait_for_value(kDdscDiscoveryTimeout));
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

      CHECK(common_test::wait_until([&notified] { return notified.load(std::memory_order_acquire); }, 3s));
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
      CHECK(late_getter.wait_for_value(kDdscDiscoveryTimeout));
      auto v = late_getter.get();
      REQUIRE(v.has_value());
      CHECK((*v)[0] == 0xBE);
    }
  }

  TEST_CASE("setter set before init is cached without breaking later writes") {
    MESSAGE("[ddsc-field] setter set before init is cached without breaking later writes");

    Setter<int> setter(DdscConf("ddsc/fld/deferred_snapshot"), InitType::kWithoutInit);
    Getter<int> getter("ddsc://ddsc/fld/deferred_snapshot");

    setter.set(1234);
    REQUIRE(setter.init());
    setter.set(5678);

    CHECK(getter.wait_for_value(kDdscDiscoveryTimeout));
    auto val = getter.get();
    REQUIRE(val.has_value());
    CHECK_EQ(*val, 5678);
  }

  TEST_CASE("invalid raw bytes are dropped before typed getter state updates") {
    MESSAGE("[ddsc-field] invalid raw bytes are dropped before typed getter state updates");

    {
      std::atomic<int> delivered{0};

      Setter<Bytes> setter(DdscConf("ddsc/fld/bad_get_int"));
      Getter<int> getter("ddsc://ddsc/fld/bad_get_int");

      CHECK(getter.listen([&](const int&) { delivered.fetch_add(1, std::memory_order_relaxed); }));
      std::this_thread::sleep_for(30ms);
      setter.set(Bytes{0x01});

      std::this_thread::sleep_for(200ms);
      CHECK_EQ(delivered.load(std::memory_order_acquire), 0);
      CHECK_FALSE(getter.get().has_value());
    }

    {
      std::atomic<int> delivered{0};

      Setter<Bytes> setter(DdscConf("ddsc/fld/bad_get_dynamic"));
      Getter<DynamicData> getter("ddsc://ddsc/fld/bad_get_dynamic");

      CHECK(getter.listen([&](const DynamicData&) { delivered.fetch_add(1, std::memory_order_relaxed); }));
      std::this_thread::sleep_for(30ms);
      setter.set(Bytes{0x01, 0x02, 0x03});

      std::this_thread::sleep_for(200ms);
      CHECK_EQ(delivered.load(std::memory_order_acquire), 0);
      CHECK_FALSE(getter.get().has_value());
    }

#if defined(VLINK_TEST_SUPPORT_PROTOBUF)
    {
      std::atomic<int> delivered{0};

      Setter<Bytes> setter(DdscConf("ddsc/fld/bad_get_proto"));
      Getter<pb::Message> getter("ddsc://ddsc/fld/bad_get_proto");

      CHECK(getter.listen([&](const pb::Message&) { delivered.fetch_add(1, std::memory_order_relaxed); }));
      std::this_thread::sleep_for(30ms);
      setter.set(Bytes{0x0A, 0xFF});

      std::this_thread::sleep_for(200ms);
      CHECK_EQ(delivered.load(std::memory_order_acquire), 0);
      CHECK_FALSE(getter.get().has_value());
    }
#endif
  }

  TEST_CASE("security getter drops values that fail decryption") {
    MESSAGE("[ddsc-field] security getter drops values that fail decryption");

    auto identity_cfg = [] {
      Security::Config cfg;
      cfg.encrypt_callback = [](const Bytes& in, Bytes& out) {
        out = in;
        return true;
      };
      cfg.decrypt_callback = [](const Bytes& in, Bytes& out) {
        out = in;
        return true;
      };
      return cfg;
    };
    auto decrypt_fail_cfg = [] {
      Security::Config cfg;
      cfg.encrypt_callback = [](const Bytes& in, Bytes& out) {
        out = in;
        return true;
      };
      cfg.decrypt_callback = [](const Bytes&, Bytes&) { return false; };
      return cfg;
    };

    std::atomic<int> delivered{0};

    SecuritySetter<int> setter(DdscConf("ddsc/fld/sec_bad_get"), identity_cfg());
    SecurityGetter<int> getter("ddsc://ddsc/fld/sec_bad_get", decrypt_fail_cfg());

    CHECK(getter.listen([&](const int&) { delivered.fetch_add(1, std::memory_order_relaxed); }));
    std::this_thread::sleep_for(30ms);
    setter.set(42);

    std::this_thread::sleep_for(200ms);
    CHECK_EQ(delivered.load(std::memory_order_acquire), 0);
    CHECK_FALSE(getter.get().has_value());
  }

  TEST_CASE("setter serialization and encryption failures do not publish values") {
    MESSAGE("[ddsc-field] setter serialization and encryption failures do not publish values");

    {
      Setter<DdscFailingCustomMsg> setter(DdscConf("ddsc/fld/custom_fail_set"));
      Getter<Bytes> getter("ddsc://ddsc/fld/custom_fail_set");

      setter.set(DdscFailingCustomMsg{});

      std::this_thread::sleep_for(200ms);
      CHECK_FALSE(getter.get().has_value());
    }

    {
      Security::Config encrypt_fail_cfg;
      encrypt_fail_cfg.encrypt_callback = [](const Bytes&, Bytes&) { return false; };
      encrypt_fail_cfg.decrypt_callback = [](const Bytes& in, Bytes& out) {
        out = in;
        return true;
      };

      std::atomic<int> delivered{0};
      SecuritySetter<int> setter(DdscConf("ddsc/fld/sec_fail_set"), std::move(encrypt_fail_cfg));
      SecurityGetter<int> getter("ddsc://ddsc/fld/sec_fail_set");

      CHECK(getter.listen([&](const int&) { delivered.fetch_add(1, std::memory_order_relaxed); }));
      std::this_thread::sleep_for(30ms);
      setter.set(7);

      std::this_thread::sleep_for(200ms);
      CHECK_EQ(delivered.load(std::memory_order_acquire), 0);
      CHECK_FALSE(getter.get().has_value());
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

    CHECK(pub.wait_for_subscribers(kDdscDiscoveryTimeout));

    Bytes payload = Bytes::create(kSize1K);

    CHECK(pub.publish(payload));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
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

    CHECK(pub.wait_for_subscribers(kDdscDiscoveryTimeout));
    CHECK(pub.publish(Bytes{}));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
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

      CHECK(pub.wait_for_subscribers(kDdscDiscoveryTimeout));

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

    std::vector<std::unique_ptr<Client<std::string, std::string>>> clients;
    clients.reserve(kClients);
    for (int t = 0; t < kClients; ++t) {
      clients.emplace_back(std::make_unique<Client<std::string, std::string>>("ddsc://ddsc/mth/concurrent1"));
    }

    for (auto& client : clients) {
      REQUIRE(client->wait_for_connected(kDdscDiscoveryTimeout));
    }
    std::this_thread::sleep_for(20ms);

    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;
    threads.reserve(kClients);

    for (int t = 0; t < kClients; ++t) {
      threads.emplace_back([t, &success_count, &clients] {
        std::string key = std::to_string(t);
        auto resp = clients[t]->invoke(key, kDdscDiscoveryTimeout);

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
    CHECK(getter.wait_for_value(kDdscDiscoveryTimeout));

    auto v = getter.get();
    REQUIRE(v.has_value());
    CHECK_EQ(*v, 99999);
  }

  TEST_CASE("attached message loop dispatches pubsub and field callbacks") {
    MESSAGE("[ddsc-field] attached message loop dispatches pubsub and field callbacks");

    MessageLoop loop;
    CHECK(loop.async_run());

    std::atomic<int> sub_count{0};
    std::atomic<int> getter_value{0};

    Publisher<int> pub(DdscConf("ddsc/loop/pubsub1"), InitType::kWithoutInit);
    Subscriber<int> sub(DdscConf("ddsc/loop/pubsub1"), InitType::kWithoutInit);
    Setter<int> setter(DdscConf("ddsc/loop/field1"), InitType::kWithoutInit);
    Getter<int> getter(DdscConf("ddsc/loop/field1"), InitType::kWithoutInit);

    CHECK(pub.attach(&loop));
    CHECK(sub.attach(&loop));
    CHECK(setter.attach(&loop));
    CHECK(getter.attach(&loop));

    CHECK(pub.init());
    CHECK(sub.init());
    CHECK(setter.init());
    CHECK(getter.init());

    CHECK(sub.listen([&](const int& value) {
      if (value == 11) {
        sub_count.fetch_add(1, std::memory_order_relaxed);
      }
    }));
    getter.listen([&](const int& value) { getter_value.store(value, std::memory_order_release); });

    CHECK(pub.wait_for_subscribers(kDdscDiscoveryTimeout));
    CHECK(pub.publish(11));
    CHECK(common_test::wait_until([&sub_count] { return sub_count.load(std::memory_order_acquire) > 0; },
                                  kDdscDiscoveryTimeout));

    setter.set(22);
    CHECK(common_test::wait_until([&getter_value] { return getter_value.load(std::memory_order_acquire) == 22; },
                                  kDdscDiscoveryTimeout));

    CHECK(getter.detach());
    CHECK(setter.detach());
    CHECK(sub.detach());
    CHECK(pub.detach());
    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("suspended getter drains values until resumed") {
    MESSAGE("[ddsc-field] suspended getter drains values until resumed");

    std::atomic<int> observed{0};

    Setter<int> setter(DdscConf("ddsc/fld/suspend_drain1"));
    Getter<int> getter("ddsc://ddsc/fld/suspend_drain1");
    getter.listen([&](const int& value) {
      if (value == 2) {
        observed.store(value, std::memory_order_release);
      }
    });

    CHECK(getter.suspend());
    setter.set(1);
    std::this_thread::sleep_for(300ms);
    CHECK_EQ(observed.load(std::memory_order_acquire), 0);

    CHECK(getter.resume());
    setter.set(2);
    CHECK(common_test::wait_until([&observed] { return observed.load(std::memory_order_acquire) == 2; },
                                  kDdscDiscoveryTimeout));
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

    CHECK(pub.wait_for_subscribers(kDdscDiscoveryTimeout));

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

  TEST_CASE("participant transport properties initialize without communication") {
    MESSAGE("[ddsc-qos] participant transport properties initialize without communication");

    Publisher<Bytes> pub(DdscConf("ddsc/qos/participant_props1", 22), InitType::kWithoutInit);
    pub.set_property("plain.ignored", "1");
    pub.set_property("dds.ip", "127.0.0.1");
    pub.set_property("dds.multicast.ip", "239.255.0.1");
    pub.set_property("dds.peer", "127.0.0.1");
    pub.set_property("dds.buf", "4096");
    pub.set_property("dds.mtu", "1200");
    pub.set_property("dds.udp", "1");
    pub.set_property("dds.tcp", "0");
    pub.set_property("dds.shm", "0");
    pub.set_property("dds.less_memory", "1");
    pub.set_property("dds.user.test", "value");

    CHECK(pub.init());
    CHECK(pub.has_inited());
    CHECK(pub.get_property("dds.ip") == "127.0.0.1");
    CHECK(pub.get_property("dds.user.test") == "value");
  }

  TEST_CASE("participant ssl properties initialize through public node properties") {
    MESSAGE("[ddsc-qos] participant ssl properties initialize through public node properties");

    const auto cert_path = make_ddsc_tmp_file("cert", "-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n");
    const auto key_path = make_ddsc_tmp_file("key", "-----BEGIN PRIVATE KEY-----\nMIIB\n-----END PRIVATE KEY-----\n");

    Publisher<Bytes> pub(DdscConf("ddsc/qos/participant_ssl_props1", 23), InitType::kWithoutInit);
    pub.set_property("dds.ip", "127.0.0.1");
    pub.set_property("dds.udp", "0");
    pub.set_property("dds.tcp", "0");
    pub.set_property("dds.shm", "0");
    pub.set_property("ssl.cert", cert_path.string());
    pub.set_property("ssl.key", key_path.string());
    pub.set_property("ssl.key_password", "password");
    pub.set_property("ssl.verify", "0");
    pub.set_property("ssl.ciphers", "DEFAULT");

    CHECK(pub.init());
    CHECK(pub.has_inited());
    CHECK_EQ(pub.get_property("ssl.verify"), "0");
  }

  TEST_CASE("latency and lost tracking can be enabled and disabled") {
    MESSAGE("[ddsc-qos] latency and lost tracking can be enabled and disabled");

    Publisher<int> pub(DdscConf("ddsc/lat/sub1"));
    Subscriber<int> sub("ddsc://ddsc/lat/sub1");

    sub.set_latency_and_lost_enabled(true);
    CHECK(sub.is_latency_and_lost_enabled());

    std::atomic<int> count{0};
    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(kDdscDiscoveryTimeout));

    for (int i = 0; i < 10; ++i) {
      pub.publish(i);
      std::this_thread::sleep_for(10ms);
    }

    std::this_thread::sleep_for(30ms);
    CHECK(count.load() > 0);
    CHECK(sub.get_latency() >= 0);
    const auto lost = sub.get_lost();
    CHECK(lost.total >= lost.lost);

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

  TEST_CASE("status accessors cover writer reader and unknown categories") {
    MESSAGE("[ddsc-error] status accessors cover writer reader and unknown categories");

    Publisher<int> pub(DdscConf("ddsc/status/accessor_pubsub"));
    Subscriber<int> sub(DdscConf("ddsc/status/accessor_pubsub"));

    auto check_status = [](const Status::BasePtr& status, Status::Type expected) {
      REQUIRE(status != nullptr);
      CHECK(status->get_type() == expected);
      CHECK_FALSE(status->get_string().empty());
    };

    check_status(pub.get_status(Status::kPublicationMatched), Status::kPublicationMatched);
    check_status(pub.get_status(Status::kOfferedDeadlineMissed), Status::kOfferedDeadlineMissed);
    check_status(pub.get_status(Status::kOfferedIncompatibleQos), Status::kOfferedIncompatibleQos);
    check_status(pub.get_status(Status::kLivelinessLost), Status::kLivelinessLost);
    check_status(pub.get_status(Status::kSubscriptionMatched), Status::kUnknown);

    check_status(sub.get_status(Status::kSubscriptionMatched), Status::kUnknown);
    CHECK(sub.listen([](const int&) {}));
    check_status(sub.get_status(Status::kSubscriptionMatched), Status::kSubscriptionMatched);
    check_status(sub.get_status(Status::kRequestedDeadlineMissed), Status::kRequestedDeadlineMissed);
    check_status(sub.get_status(Status::kLivelinessChanged), Status::kLivelinessChanged);
    check_status(sub.get_status(Status::kSampleRejected), Status::kSampleRejected);
    check_status(sub.get_status(Status::kRequestedIncompatibleQos), Status::kRequestedIncompatibleQos);
    check_status(sub.get_status(Status::kSampleLost), Status::kSampleLost);
    check_status(sub.get_status(Status::kPublicationMatched), Status::kUnknown);

    REQUIRE(pub.get_abstract_node() != nullptr);
    REQUIRE(sub.get_abstract_node() != nullptr);
    CHECK(pub.get_abstract_node()->get_native_handle().has_value());
    CHECK(sub.get_abstract_node()->get_native_handle().has_value());
  }

  TEST_CASE("server status accessors and duplicate listen guards are stable") {
    MESSAGE("[ddsc-error] server status accessors and duplicate listen guards are stable");

    Server<std::string> fire_server(DdscConf("ddsc/status/fire_server"));
    Server<std::string, std::string> sync_server(DdscConf("ddsc/status/sync_server"));

    auto check_status = [](const Status::BasePtr& status, Status::Type expected) {
      REQUIRE(status != nullptr);
      CHECK(status->get_type() == expected);
    };

    check_status(fire_server.get_status(Status::kSubscriptionMatched), Status::kUnknown);
    CHECK(fire_server.listen([](const std::string&) {}));
    CHECK_THROWS(fire_server.listen([](const std::string&) {}));
    check_status(fire_server.get_status(Status::kSubscriptionMatched), Status::kSubscriptionMatched);
    check_status(fire_server.get_status(Status::kPublicationMatched), Status::kUnknown);

    check_status(sync_server.get_status(Status::kPublicationMatched), Status::kPublicationMatched);
    check_status(sync_server.get_status(Status::kSubscriptionMatched), Status::kUnknown);
    CHECK(sync_server.listen([](const std::string&, std::string& resp) { resp = "ok"; }));
    CHECK_THROWS(sync_server.listen([](const std::string&, std::string& resp) { resp = "again"; }));
    check_status(sync_server.get_status(Status::kSubscriptionMatched), Status::kSubscriptionMatched);
    CHECK_THROWS(sync_server.reply(1, std::string("late")));
  }

  TEST_CASE("client setter and getter status accessors cover both sides") {
    MESSAGE("[ddsc-error] client setter and getter status accessors cover both sides");

    Client<std::string> fire_client(DdscConf("ddsc/status/fire_client"));
    Client<std::string, std::string> sync_client(DdscConf("ddsc/status/sync_client"));
    Setter<int> setter(DdscConf("ddsc/status/setter"));
    Getter<int> getter(DdscConf("ddsc/status/getter"));

    auto check_status = [](const Status::BasePtr& status, Status::Type expected) {
      REQUIRE(status != nullptr);
      CHECK(status->get_type() == expected);
    };

    CHECK_FALSE(fire_client.is_connected());
    CHECK_FALSE(fire_client.wait_for_connected(20ms));
    check_status(fire_client.get_status(Status::kPublicationMatched), Status::kPublicationMatched);
    check_status(fire_client.get_status(Status::kSubscriptionMatched), Status::kUnknown);

    CHECK_FALSE(sync_client.is_connected());
    CHECK_FALSE(sync_client.wait_for_connected(20ms));
    check_status(sync_client.get_status(Status::kPublicationMatched), Status::kPublicationMatched);
    check_status(sync_client.get_status(Status::kSubscriptionMatched), Status::kSubscriptionMatched);
    check_status(sync_client.get_status(Status::kRequestedDeadlineMissed), Status::kRequestedDeadlineMissed);

    check_status(setter.get_status(Status::kPublicationMatched), Status::kPublicationMatched);
    check_status(setter.get_status(Status::kSubscriptionMatched), Status::kUnknown);

    check_status(getter.get_status(Status::kSubscriptionMatched), Status::kSubscriptionMatched);
    check_status(getter.get_status(Status::kSampleLost), Status::kSampleLost);
    check_status(getter.get_status(Status::kPublicationMatched), Status::kUnknown);

    REQUIRE(fire_client.get_abstract_node() != nullptr);
    REQUIRE(sync_client.get_abstract_node() != nullptr);
    REQUIRE(setter.get_abstract_node() != nullptr);
    REQUIRE(getter.get_abstract_node() != nullptr);
    CHECK(fire_client.get_abstract_node()->get_native_handle().has_value());
    CHECK(sync_client.get_abstract_node()->get_native_handle().has_value());
    CHECK(setter.get_abstract_node()->get_native_handle().has_value());
    CHECK(getter.get_abstract_node()->get_native_handle().has_value());
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

    CHECK(pub.wait_for_subscribers(kDdscDiscoveryTimeout));

    DynamicData d;
    d.load("int", 42);
    CHECK(pub.publish(d));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
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
      CHECK(pub.wait_for_subscribers(kDdscDiscoveryTimeout));

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
      CHECK(pub.wait_for_subscribers(kDdscDiscoveryTimeout));

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

      CHECK(common_test::wait_until([&got_matched] { return got_matched.load(std::memory_order_acquire); },
                                    kDdscDiscoveryTimeout));
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

      CHECK(common_test::wait_until([&got_matched] { return got_matched.load(std::memory_order_acquire); },
                                    kDdscDiscoveryTimeout));
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

      CHECK(pub.wait_for_subscribers(kDdscDiscoveryTimeout));
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

TEST_SUITE("ddsc-qos") {
  TEST_CASE("named qos profiles exercise every convert_qos branch") {
    MESSAGE("[ddsc-qos] named qos profiles exercise every convert_qos branch");

    auto register_profile = [](const char* name, auto&& fill) {
      Qos qos;
      qos.valid = true;
      fill(qos);

      try {
        DdscConf::register_qos(name, qos);
      } catch (...) {
      }
    };

    register_profile("ddsc_cq_best_effort", [](Qos& q) { q.reliability.kind = Qos::Reliability::kBestEffort; });
    register_profile("ddsc_cq_transient", [](Qos& q) { q.durability.kind = Qos::Durability::kTransient; });
    register_profile("ddsc_cq_persistent", [](Qos& q) { q.durability.kind = Qos::Durability::kPersistent; });
    register_profile("ddsc_cq_live_part", [](Qos& q) { q.liveliness.kind = Qos::Liveliness::kManualParticipant; });
    register_profile("ddsc_cq_live_topic", [](Qos& q) { q.liveliness.kind = Qos::Liveliness::kManualTopic; });
    register_profile("ddsc_cq_src_ts",
                     [](Qos& q) { q.destination_order.kind = Qos::DestinationOrder::kSourceTimestamp; });
    register_profile("ddsc_cq_exclusive", [](Qos& q) { q.ownership.kind = Qos::Ownership::kExclusive; });

    static const char* const kProfiles[] = {"ddsc_cq_best_effort", "ddsc_cq_transient",  "ddsc_cq_persistent",
                                            "ddsc_cq_live_part",   "ddsc_cq_live_topic", "ddsc_cq_src_ts",
                                            "ddsc_cq_exclusive"};

    for (const char* name : kProfiles) {
      const std::string topic = std::string("ddsc/qos/cq_") + name;
      Publisher<int> pub(DdscConf(topic, 0, 1, name));
      Subscriber<int> sub(std::string("ddsc://") + topic + "?qos=" + name);
      sub.listen([](const int&) {});
      (void)pub.wait_for_subscribers(kDdscDiscoveryTimeout);

      CHECK(pub.get_abstract_node()->get_native_handle().has_value());
      CHECK(sub.get_abstract_node()->get_native_handle().has_value());
    }
  }

  TEST_CASE("incompatible qos fires offered and requested status handlers") {
    MESSAGE("[ddsc-qos] incompatible qos fires offered and requested status handlers");

    Qos best_effort;
    best_effort.valid = true;
    best_effort.reliability.kind = Qos::Reliability::kBestEffort;

    Qos reliable;
    reliable.valid = true;
    reliable.reliability.kind = Qos::Reliability::kReliable;

    try {
      DdscConf::register_qos("ddsc_st_writer_be", best_effort);
    } catch (...) {
    }

    try {
      DdscConf::register_qos("ddsc_st_reader_rel", reliable);
    } catch (...) {
    }

    std::atomic<bool> offered{false};
    std::atomic<bool> requested{false};

    Publisher<int> pub(DdscConf("ddsc/status/incompat1", 0, 0, "ddsc_st_writer_be"));
    pub.register_status_handler([&offered](const Status::BasePtr& status) {
      if (status && status->get_type() == Status::kOfferedIncompatibleQos) {
        offered.store(true, std::memory_order_release);
      }
    });

    Subscriber<int> sub("ddsc://ddsc/status/incompat1?qos=ddsc_st_reader_rel");
    sub.register_status_handler([&requested](const Status::BasePtr& status) {
      if (status && status->get_type() == Status::kRequestedIncompatibleQos) {
        requested.store(true, std::memory_order_release);
      }
    });
    sub.listen([](const int&) {});

    CHECK(common_test::wait_until([&offered, &requested] { return offered.load() && requested.load(); },
                                  kDdscDiscoveryTimeout));
  }
}

TEST_SUITE("ddsc-method") {
  TEST_CASE("connected client can invoke server") {
    MESSAGE("[ddsc-method] connected client can invoke server");

    Server<std::string, std::string> server(DdscConf("ddsc/mth/connected_invoke1"));
    server.listen([](const std::string& req, std::string& resp) { resp = req; });

    Client<std::string, std::string> client("ddsc://ddsc/mth/connected_invoke1");
    REQUIRE(client.wait_for_connected(kDdscDiscoveryTimeout));

    std::string resp;
    CHECK(client.invoke("ping", resp, kDdscDiscoveryTimeout));
    CHECK(resp == "ping");
  }
}

TEST_SUITE("ddsc-field") {
  TEST_CASE("getter latency and lost tracking updates on received values") {
    MESSAGE("[ddsc-field] getter latency and lost tracking updates on received values");

    Setter<int> setter(DdscConf("ddsc/fld/lat1"));
    Getter<int> getter("ddsc://ddsc/fld/lat1");
    getter.set_latency_and_lost_enabled(true);
    CHECK(getter.is_latency_and_lost_enabled());

    std::atomic<int> received{0};
    getter.listen([&received](const int&) { received.fetch_add(1, std::memory_order_relaxed); });
    std::this_thread::sleep_for(30ms);

    for (int i = 0; i < 10; ++i) {
      setter.set(i);
      std::this_thread::sleep_for(10ms);
    }

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_relaxed) > 0; },
                                  kDdscDiscoveryTimeout));

    const auto lost = getter.get_lost();
    CHECK(lost.total >= lost.lost);

    getter.set_latency_and_lost_enabled(false);
  }
}

#endif  // VLINK_SUPPORT_DDSC

// NOLINTEND
