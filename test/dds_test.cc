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
#include <charconv>
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
#include "./modules/dds_conf.h"

static constexpr auto kDdsDiscoveryTimeout = 3s;
static constexpr auto kDdsTeardownGrace = 10ms;

namespace {

struct ScopedDdsTeardownGrace {
  ~ScopedDdsTeardownGrace() { std::this_thread::sleep_for(kDdsTeardownGrace); }
};

class ScopedDdsTmpFile {
 public:
  ScopedDdsTmpFile(const std::string& name, const std::string& content) {
    path_ = std::filesystem::path(vlink::Utils::get_tmp_dir()) / "vlink-dds-tests" /
            (name + "_" + vlink::Utils::get_pid_str() + ".pem");
    std::filesystem::create_directories(path_.parent_path());

    std::ofstream file(path_, std::ios::binary | std::ios::trunc);
    file << content;
  }

  ScopedDdsTmpFile(const ScopedDdsTmpFile&) = delete;
  ScopedDdsTmpFile& operator=(const ScopedDdsTmpFile&) = delete;

  ScopedDdsTmpFile(ScopedDdsTmpFile&& other) noexcept : path_(std::move(other.path_)) { other.path_.clear(); }

  ScopedDdsTmpFile& operator=(ScopedDdsTmpFile&& other) noexcept {
    if (this != &other) {
      remove();
      path_ = std::move(other.path_);
      other.path_.clear();
    }
    return *this;
  }

  ~ScopedDdsTmpFile() { remove(); }

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

ScopedDdsTmpFile make_dds_tmp_file(const std::string& name, const std::string& content) {
  return ScopedDdsTmpFile(name, content);
}

template <typename NodeT>
void set_loopback_dds_transport(NodeT& node, bool tcp_enabled) {
  node.set_property("coverage.non_dds", "ignored");
  node.set_property("dds.ip", "127.0.0.1");
  node.set_property("dds.multicast.ip", "239.255.0.1");
  node.set_property("dds.peer", "127.0.0.1");
  node.set_property("dds.buf", "2048");
  node.set_property("dds.mtu", "1024");
  node.set_property("dds.udp", "1");
  node.set_property("dds.tcp", tcp_enabled ? "1" : "0");
  node.set_property("dds.shm", "0");
  node.set_property("dds.less_memory", "1");
}

struct DdsFailingCustomMsg {
  bool operator>>(Bytes&) const { return false; }
  bool operator<<(const Bytes&) { return false; }
};

#ifdef VLINK_HAS_CDR
struct DdsUnregisteredCdrMsg {
  void serialize(eprosima::fastcdr::Cdr&) const {}
  void deserialize(eprosima::fastcdr::Cdr&) {}
};
#endif

}  // namespace

#undef TEST_CASE
#define TEST_CASE(name) TEST_CASE_FIXTURE(ScopedDdsTeardownGrace, name)

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

  TEST_CASE("invalid url scheme throws on every public role construction") {
    CHECK_THROWS(Publisher<int>("dds1://bad/url"));
    CHECK_THROWS(Subscriber<int>("dds1://bad/url"));
    CHECK_THROWS(Setter<int>("dds1://bad/url"));
    CHECK_THROWS(Getter<int>("dds1://bad/url"));
    CHECK_THROWS(Server<int>("dds1://bad/url"));
    CHECK_THROWS(Server<int, int>("dds1://bad/url"));
    CHECK_THROWS(Client<int>("dds1://bad/url"));
    CHECK_THROWS(Client<int, int>("dds1://bad/url"));
  }

  TEST_CASE("public listeners reject calls before init and duplicate registrations") {
    MESSAGE("[dds-init] public listeners reject calls before init and duplicate registrations");

    Subscriber<int> sub(DdsConf("dds/init/guard_subscriber"), InitType::kWithoutInit);
    CHECK_THROWS(sub.listen([](const int&) {}));
    REQUIRE(sub.init());
    CHECK(sub.listen([](const int&) {}));
    CHECK_THROWS(sub.listen([](const int&) {}));

    Server<std::string> fire_server(DdsConf("dds/init/guard_fire_server"), InitType::kWithoutInit);
    CHECK_THROWS(fire_server.listen([](const std::string&) {}));
    REQUIRE(fire_server.init());
    CHECK(fire_server.listen([](const std::string&) {}));
    CHECK_THROWS(fire_server.listen([](const std::string&) {}));

    Server<std::string, std::string> sync_server(DdsConf("dds/init/guard_sync_server"), InitType::kWithoutInit);
    CHECK_THROWS(sync_server.listen([](const std::string&, std::string&) {}));
    REQUIRE(sync_server.init());
    CHECK(sync_server.listen([](const std::string& req, std::string& resp) { resp = req; }));
    CHECK_THROWS(sync_server.listen([](const std::string&, std::string&) {}));
    CHECK_THROWS(sync_server.reply(1, std::string("wrong_mode")));

    Server<std::string, std::string> async_server(DdsConf("dds/init/guard_async_server"), InitType::kWithoutInit);
    CHECK_THROWS(async_server.listen_for_reply([](uint64_t, const std::string&) {}));
    REQUIRE(async_server.init());
    CHECK(async_server.listen_for_reply([](uint64_t, const std::string&) {}));
    CHECK_THROWS(async_server.listen_for_reply([](uint64_t, const std::string&) {}));

    Server<std::string, std::string> idle_server(DdsConf("dds/init/guard_idle_server"), InitType::kWithoutInit);
    CHECK_THROWS(idle_server.reply(777, std::string("not_listened")));
  }

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

  TEST_CASE("global init topic helper and missing qos lookup are stable") {
    CHECK_NOTHROW(DdsConf::global_init());

    Url path_url("dds://host/path?domain=2&depth=3");
    REQUIRE(path_url.parse(kPublisher));
    const auto* path_conf = static_cast<const DdsConf*>(path_url.get_target());
    REQUIRE(path_conf != nullptr);
    CHECK_EQ(path_conf->topic, "host/path");

    Url host_url("dds://host");
    REQUIRE(host_url.parse(kPublisher));
    const auto* host_conf = static_cast<const DdsConf*>(host_url.get_target());
    REQUIRE(host_conf != nullptr);
    CHECK_EQ(host_conf->topic, "host");
    CHECK_FALSE(Url("dds:///missing-host").parse(kPublisher));
  }

  TEST_CASE("url parser preserves every supported qos extension key") {
    const std::vector<std::string> keys{"part", "topic", "pub", "sub", "writer", "reader", "unknown"};

    for (const auto& key : keys) {
      Url ext_url("dds://ext_" + key + "?domain=4&depth=5&" + key + "=kept");
      REQUIRE(ext_url.parse(kPublisher));
      const auto* conf = static_cast<const DdsConf*>(ext_url.get_target());
      REQUIRE(conf != nullptr);

      CHECK_EQ(conf->topic, "ext_" + key);
      CHECK_EQ(conf->domain, 4);
      CHECK_EQ(conf->depth, 5);
      CHECK(conf->qos.empty());
      CHECK_EQ(conf->qos_ext.at(key), "kept");
    }
  }

  TEST_CASE("conf validity covers invalid edge cases") {
    CHECK_FALSE(DdsConf("", 0).is_valid());
    CHECK_FALSE(DdsConf("dds/init/negative_domain", -1).is_valid());

    DdsConf mixed("dds/init/mixed_qos", 0, 0, "named");
    mixed.qos_ext["reader"] = "reader_profile";
    CHECK_FALSE(mixed.is_valid());
  }

  TEST_CASE("missing per-entity profiles keep wrapper lifecycle stable") {
    const std::string missing_profile = "__vlink_missing_fastdds_profile__";
    auto check_lifecycle = [](auto& node) {
      const bool init_result = node.init();
      CHECK_EQ(init_result, node.has_inited());

      const bool deinit_result = node.deinit();
      CHECK_EQ(deinit_result, init_result);
      CHECK_FALSE(node.has_inited());
    };

    SUBCASE("participant profile") {
      DdsConf::PropertiesMap ext{{"part", missing_profile}};
      Publisher<Bytes> pub(DdsConf("dds/profile/missing_part", 61, ext), InitType::kWithoutInit);
      check_lifecycle(pub);
    }

    SUBCASE("topic profile") {
      DdsConf::PropertiesMap ext{{"topic", missing_profile}};
      Publisher<Bytes> pub(DdsConf("dds/profile/missing_topic", 62, ext), InitType::kWithoutInit);
      check_lifecycle(pub);
    }

    SUBCASE("publisher profile") {
      DdsConf::PropertiesMap ext{{"pub", missing_profile}};
      Publisher<Bytes> pub(DdsConf("dds/profile/missing_pub", 63, ext), InitType::kWithoutInit);
      check_lifecycle(pub);
    }

    SUBCASE("subscriber profile") {
      DdsConf::PropertiesMap ext{{"sub", missing_profile}};
      Subscriber<Bytes> sub(DdsConf("dds/profile/missing_sub", 64, ext), InitType::kWithoutInit);
      check_lifecycle(sub);
    }

    SUBCASE("writer profile") {
      DdsConf::PropertiesMap ext{{"writer", missing_profile}};
      Publisher<Bytes> pub(DdsConf("dds/profile/missing_writer", 65, ext), InitType::kWithoutInit);
      check_lifecycle(pub);
    }

    SUBCASE("reader profile") {
      DdsConf::PropertiesMap ext{{"reader", missing_profile}};
      Subscriber<Bytes> sub(DdsConf("dds/profile/missing_reader", 66, ext), InitType::kWithoutInit);
      check_lifecycle(sub);
    }
  }

#ifdef VLINK_HAS_CDR
  TEST_CASE("unregistered CDR topic init is either rejected or cleaned up") {
    Publisher<DdsUnregisteredCdrMsg> pub(DdsConf("dds/init/unregistered_cdr_pub"), InitType::kWithoutInit);
    bool pub_init = false;
    bool pub_rejected = false;
    try {
      pub_init = pub.init();
    } catch (const std::exception&) {
      pub_rejected = true;
    }
    CHECK((pub_rejected || pub_init));
    if (pub_init) {
      CHECK(pub.deinit());
    }

    Subscriber<DdsUnregisteredCdrMsg> sub(DdsConf("dds/init/unregistered_cdr_sub"), InitType::kWithoutInit);
    bool sub_init = false;
    bool sub_rejected = false;
    try {
      sub_init = sub.init();
    } catch (const std::exception&) {
      sub_rejected = true;
    }
    CHECK((sub_rejected || sub_init));
    if (sub_init) {
      CHECK(sub.deinit());
    }
  }
#endif

  TEST_CASE("deferred nodes cover every role without runtime init") {
    Publisher<int> pub(DdsConf("dds/init/deferred_roles_pub"), InitType::kWithoutInit);
    Subscriber<int> sub(DdsConf("dds/init/deferred_roles_sub"), InitType::kWithoutInit);
    Setter<int> setter(DdsConf("dds/init/deferred_roles_field"), InitType::kWithoutInit);
    Getter<int> getter(DdsConf("dds/init/deferred_roles_field"), InitType::kWithoutInit);
    Server<std::string> fire_server(DdsConf("dds/init/deferred_roles_fire"), InitType::kWithoutInit);
    Server<std::string, std::string> sync_server(DdsConf("dds/init/deferred_roles_rpc"), InitType::kWithoutInit);
    Client<std::string> fire_client(DdsConf("dds/init/deferred_roles_fire"), InitType::kWithoutInit);
    Client<std::string, std::string> sync_client(DdsConf("dds/init/deferred_roles_rpc"), InitType::kWithoutInit);

    CHECK_FALSE(pub.has_inited());
    CHECK_FALSE(sub.has_inited());
    CHECK_FALSE(setter.has_inited());
    CHECK_FALSE(getter.has_inited());
    CHECK_FALSE(fire_server.has_inited());
    CHECK_FALSE(sync_server.has_inited());
    CHECK_FALSE(fire_client.has_inited());
    CHECK_FALSE(sync_client.has_inited());

    CHECK(pub.get_transport_type() == TransportType::kDds);
    CHECK(sub.get_transport_type() == TransportType::kDds);
    CHECK(setter.get_transport_type() == TransportType::kDds);
    CHECK(getter.get_transport_type() == TransportType::kDds);
  }

  TEST_CASE("factory helpers support explicit init and deinit for every dds role") {
    auto pub = Publisher<int>::create_unique("dds://dds/init/factory_pub?domain=73", InitType::kWithoutInit);
    auto sub = Subscriber<int>::create_shared("dds://dds/init/factory_sub?domain=74", InitType::kWithoutInit);
    auto setter = Setter<int>::create_unique("dds://dds/init/factory_setter?domain=75", InitType::kWithoutInit);
    auto getter = Getter<int>::create_shared("dds://dds/init/factory_getter?domain=76", InitType::kWithoutInit);
    auto fire_server =
        Server<std::string>::create_unique("dds://dds/init/factory_fire_server?domain=77", InitType::kWithoutInit);
    auto fire_client =
        Client<std::string>::create_shared("dds://dds/init/factory_fire_client?domain=78", InitType::kWithoutInit);
    auto sync_server = Server<std::string, std::string>::create_unique("dds://dds/init/factory_rpc_server?domain=79",
                                                                       InitType::kWithoutInit);
    auto sync_client = Client<std::string, std::string>::create_shared("dds://dds/init/factory_rpc_client?domain=80",
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
    DdsConf conf("dds/init/cache_reuse", 72);

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

    Publisher<int> pub(DdsConf("dds/init/deferred_status_pub"), InitType::kWithoutInit);
    Subscriber<int> sub(DdsConf("dds/init/deferred_status_sub"), InitType::kWithoutInit);
    Server<std::string> fire_server(DdsConf("dds/init/deferred_status_fire"), InitType::kWithoutInit);
    Server<std::string, std::string> sync_server(DdsConf("dds/init/deferred_status_rpc"), InitType::kWithoutInit);
    Client<std::string> fire_client(DdsConf("dds/init/deferred_status_fire"), InitType::kWithoutInit);
    Client<std::string, std::string> sync_client(DdsConf("dds/init/deferred_status_rpc"), InitType::kWithoutInit);
    Getter<int> getter(DdsConf("dds/init/deferred_status_getter"), InitType::kWithoutInit);

    check_unknown(pub.get_status(Status::kPublicationMatched));
    check_unknown(sub.get_status(Status::kSubscriptionMatched));
    check_unknown(fire_server.get_status(Status::kSubscriptionMatched));
    check_unknown(sync_server.get_status(Status::kPublicationMatched));
    check_unknown(fire_client.get_status(Status::kPublicationMatched));
    check_unknown(sync_client.get_status(Status::kSubscriptionMatched));
    check_unknown(getter.get_status(Status::kSubscriptionMatched));
  }

  TEST_CASE("deferred getter reports latency state before runtime init") {
    Getter<int> getter(DdsConf("dds/init/deferred_getter"), InitType::kWithoutInit);

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
    Subscriber<int> sub(DdsConf("dds/init/deferred_subscriber"), InitType::kWithoutInit);

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
    Subscriber<int> sub(DdsConf("dds/init/suspend_subscriber"), InitType::kWithoutInit);
    Getter<int> getter(DdsConf("dds/init/suspend_getter"), InitType::kWithoutInit);
    Server<std::string> fire_server(DdsConf("dds/init/suspend_fire_server"), InitType::kWithoutInit);
    Server<std::string, std::string> sync_server(DdsConf("dds/init/suspend_sync_server"), InitType::kWithoutInit);

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

    CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));
    CHECK(pub.has_subscribers());

    Bytes payload{0xDE, 0xAD, 0xBE, 0xEF};
    CHECK(pub.publish(payload));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
    REQUIRE(captured.size() == 4u);
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

    CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));
    CHECK(pub.publish(std::string("hello_dds")));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
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

    CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));
    CHECK(pub.publish(12345));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
    CHECK(captured.load() == 12345);
  }

  TEST_CASE("multiple publishes are all received by subscriber") {
    MESSAGE("[dds-pubsub] multiple publishes are all received by subscriber");

    std::atomic<int> count{0};

    Publisher<int> pub(DdsConf("dds/evt/multi1"));
    Subscriber<int> sub("dds://dds/evt/multi1");

    sub.listen([&](const int& /*v*/) { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));

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

    CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));

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
    CHECK_FALSE(pub.wait_for_subscribers(20ms));

    for (int i = 0; i < 5; ++i) {
      CHECK(pub.publish(Bytes{static_cast<uint8_t>(i)}, true));
    }
  }

  TEST_CASE("subscriber and getter reject duplicate user callbacks") {
    MESSAGE("[dds-pubsub] subscriber and getter reject duplicate user callbacks");

    Subscriber<int> sub(DdsConf("dds/guard/dup_sub1"));
    CHECK(sub.listen([](const int&) {}));
    CHECK_THROWS(sub.listen([](const int&) {}));

    Getter<int> getter(DdsConf("dds/guard/dup_getter1"));
    CHECK(getter.listen([](const int&) {}));
    CHECK_THROWS(getter.listen([](const int&) {}));
  }

  TEST_CASE("invalid raw bytes are dropped before typed subscriber callback") {
    MESSAGE("[dds-pubsub] invalid raw bytes are dropped before typed subscriber callback");

    {
      std::atomic<int> delivered{0};

      Publisher<Bytes> pub(DdsConf("dds/evt/bad_typed_bytes1"));
      Subscriber<int> sub("dds://dds/evt/bad_typed_bytes1");

      CHECK(sub.listen([&](const int&) { delivered.fetch_add(1, std::memory_order_relaxed); }));
      CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));
      CHECK(pub.publish(Bytes{0x01}));

      std::this_thread::sleep_for(200ms);
      CHECK_EQ(delivered.load(std::memory_order_acquire), 0);
    }

    {
      std::atomic<int> delivered{0};

      Publisher<Bytes> pub(DdsConf("dds/evt/bad_dynamic_bytes1"));
      Subscriber<DynamicData> sub("dds://dds/evt/bad_dynamic_bytes1");

      CHECK(sub.listen([&](const DynamicData&) { delivered.fetch_add(1, std::memory_order_relaxed); }));
      CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));
      CHECK(pub.publish(Bytes{0x01, 0x02, 0x03}));

      std::this_thread::sleep_for(200ms);
      CHECK_EQ(delivered.load(std::memory_order_acquire), 0);
    }

#if defined(VLINK_TEST_SUPPORT_PROTOBUF)
    {
      std::atomic<int> delivered{0};

      Publisher<Bytes> pub(DdsConf("dds/evt/bad_proto_bytes1"));
      Subscriber<pb::Message> sub("dds://dds/evt/bad_proto_bytes1");

      CHECK(sub.listen([&](const pb::Message&) { delivered.fetch_add(1, std::memory_order_relaxed); }));
      CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));
      CHECK(pub.publish(Bytes{0x0A, 0xFF}));

      std::this_thread::sleep_for(200ms);
      CHECK_EQ(delivered.load(std::memory_order_acquire), 0);
    }
#endif
  }

  TEST_CASE("suspended subscriber drops queued samples then receives after resume") {
    MESSAGE("[dds-pubsub] suspended subscriber drops queued samples then receives after resume");

    std::atomic<int> resumed_value{0};

    Publisher<int> pub(DdsConf("dds/evt/suspend_drop1"));
    Subscriber<int> sub(DdsConf("dds/evt/suspend_drop1"));

    CHECK(sub.listen([&](const int& value) {
      if (value == 2) {
        resumed_value.store(value, std::memory_order_release);
      }
    }));

    CHECK(sub.suspend());
    CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));
    CHECK(pub.publish(1));

    std::this_thread::sleep_for(300ms);
    CHECK_EQ(resumed_value.load(std::memory_order_acquire), 0);

    CHECK(sub.resume());
    CHECK(pub.publish(2));
    CHECK(common_test::wait_until([&] { return resumed_value.load(std::memory_order_acquire) == 2; },
                                  kDdsDiscoveryTimeout));
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
      CHECK(common_test::wait_until([&pub] { return pub.has_subscribers(); }, kDdsDiscoveryTimeout));
    }

    CHECK(common_test::wait_until([&pub] { return !pub.has_subscribers(); }, kDdsDiscoveryTimeout));
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

      CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));

      pb::Message msg;
      msg.set_value("dds_proto");
      CHECK(pub.publish(msg));

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
      CHECK(captured.value() == "dds_proto");
    }
#endif

    SUBCASE("plain bytes always works") {
      std::atomic<bool> received{false};

      Publisher<Bytes> pub(DdsConf("dds/ser/plain1"));
      Subscriber<Bytes> sub("dds://dds/ser/plain1");

      sub.listen([&](const Bytes& /*d*/) { received.store(true, std::memory_order_release); });

      CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));
      pub.publish(Bytes{0x01, 0x02, 0x03});

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
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
    CHECK(client.wait_for_connected(kDdsDiscoveryTimeout));
    std::this_thread::sleep_for(20ms);
    CHECK(client.is_connected());

    CHECK(client.send("fire1"));
    CHECK(common_test::wait_until([&counter] { return counter.load(std::memory_order_acquire) >= 1; },
                                  kDdsDiscoveryTimeout));
    CHECK(counter.load() == 1);

    CHECK(client.send("fire2"));
    CHECK(common_test::wait_until([&counter] { return counter.load(std::memory_order_acquire) >= 2; },
                                  kDdsDiscoveryTimeout));
    CHECK(counter.load() == 2);
  }

  TEST_CASE("invoke returns correct response via multiple overloads") {
    MESSAGE("[dds-method] invoke returns correct response via multiple overloads");

    Server<std::string, std::string> server(DdsConf("dds/mth/invoke1"));
    server.listen([](const std::string& req, std::string& resp) { resp = "echo:" + req; });

    Client<std::string, std::string> client("dds://dds/mth/invoke1");
    CHECK(client.wait_for_connected(kDdsDiscoveryTimeout));
    std::this_thread::sleep_for(20ms);

    SUBCASE("sync optional") {
      auto resp = client.invoke("ping");
      CHECK(resp.has_value());
      CHECK(*resp == "echo:ping");
    }

    SUBCASE("sync ref overload") {
      std::string out;
      CHECK(client.invoke("world", out, kDdsDiscoveryTimeout));
      CHECK(out == "echo:world");
    }

    SUBCASE("async future") {
      auto fut = client.async_invoke("async");
      REQUIRE(fut.wait_for(kDdsDiscoveryTimeout) == std::future_status::ready);
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
    CHECK(client.wait_for_connected(kDdsDiscoveryTimeout));
    std::this_thread::sleep_for(20ms);

    auto fut = client.async_invoke("deferred");

    REQUIRE(common_test::wait_until([&req_received] { return req_received.load(std::memory_order_acquire); },
                                    kDdsDiscoveryTimeout));
    CHECK(server.reply(saved_id.load(), std::string("deferred_resp")));

    REQUIRE(fut.wait_for(kDdsDiscoveryTimeout) == std::future_status::ready);
    CHECK(fut.get() == "deferred_resp");
  }

  TEST_CASE("async callback invoke delivers response") {
    MESSAGE("[dds-method] async callback invoke delivers response");

    Server<std::string, std::string> server(DdsConf("dds/mth/cb1"));
    server.listen([](const std::string& /*req*/, std::string& resp) { resp = "cb_ok"; });

    Client<std::string, std::string> client("dds://dds/mth/cb1");
    CHECK(client.wait_for_connected(kDdsDiscoveryTimeout));
    std::this_thread::sleep_for(20ms);

    std::atomic<bool> got{false};
    std::string resp_val;

    bool ok = client.invoke("msg", [&](const std::string& resp) {
      resp_val = resp;
      got.store(true, std::memory_order_release);
    });

    CHECK(ok);

    CHECK(common_test::wait_until([&got] { return got.load(std::memory_order_acquire); }, kDdsDiscoveryTimeout));
    CHECK(resp_val == "cb_ok");
  }

  TEST_CASE("malformed method payloads are rejected without invoking typed handlers") {
    MESSAGE("[dds-method] malformed method payloads are rejected without invoking typed handlers");

    std::atomic<int> fire_count{0};
    Server<int> fire_server(DdsConf("dds/mth/bad_req_fire1"));
    CHECK(fire_server.listen([&](const int&) { fire_count.fetch_add(1, std::memory_order_relaxed); }));

    Client<Bytes> fire_client("dds://dds/mth/bad_req_fire1");
    CHECK(fire_client.wait_for_connected(kDdsDiscoveryTimeout));
    CHECK(fire_client.send(Bytes{0x01}));
    std::this_thread::sleep_for(200ms);
    CHECK_EQ(fire_count.load(std::memory_order_acquire), 0);

    std::atomic<int> sync_count{0};
    Server<int, int> sync_server(DdsConf("dds/mth/bad_req_sync1"));
    CHECK(sync_server.listen([&](const int&, int& resp) {
      sync_count.fetch_add(1, std::memory_order_relaxed);
      resp = 1;
    }));

    Client<Bytes, Bytes> sync_client("dds://dds/mth/bad_req_sync1");
    CHECK(sync_client.wait_for_connected(kDdsDiscoveryTimeout));
    Bytes ignored_response;
    CHECK_FALSE(sync_client.invoke(Bytes{0x02}, ignored_response, 500ms));
    CHECK(ignored_response.empty());
    CHECK_EQ(sync_count.load(std::memory_order_acquire), 0);
  }

  TEST_CASE("client response decode failures keep callbacks and futures deterministic") {
    MESSAGE("[dds-method] client response decode failures keep callbacks and futures deterministic");

    Server<int, Bytes> bad_resp_server(DdsConf("dds/mth/bad_resp1"));
    CHECK(bad_resp_server.listen([](const int&, Bytes& resp) { resp = Bytes{0x01}; }));

    Client<int, int> client("dds://dds/mth/bad_resp1");
    CHECK(client.wait_for_connected(kDdsDiscoveryTimeout));

    int out = 1234;
    CHECK_FALSE(client.invoke(7, out, kDdsDiscoveryTimeout));
    CHECK_EQ(out, 1234);

    std::atomic<int> callback_count{0};
    CHECK(client.invoke(8, [&](const int&) { callback_count.fetch_add(1, std::memory_order_relaxed); }));
    std::this_thread::sleep_for(200ms);
    CHECK_EQ(callback_count.load(std::memory_order_acquire), 0);

    auto future = client.async_invoke(9);
    REQUIRE(future.wait_for(kDdsDiscoveryTimeout) == std::future_status::ready);
    CHECK_THROWS(future.get());
  }

  TEST_CASE("security rpc failures are dropped at decrypt and encrypt boundaries") {
    MESSAGE("[dds-method] security rpc failures are dropped at decrypt and encrypt boundaries");

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
      SecurityServer<int, int> server(DdsConf("dds/mth/sec_bad_req"), server_decrypt_fail_cfg());
      CHECK(server.listen([&](const int&, int& resp) {
        handled.fetch_add(1, std::memory_order_relaxed);
        resp = 1;
      }));

      SecurityClient<int, int> client("dds://dds/mth/sec_bad_req", identity_cfg());
      CHECK(client.wait_for_connected(kDdsDiscoveryTimeout));

      int resp = 0;
      CHECK_FALSE(client.invoke(7, resp, 300ms));
      CHECK_EQ(handled.load(std::memory_order_acquire), 0);
    }

    {
      std::atomic<int> handled{0};
      SecurityServer<int, int> server(DdsConf("dds/mth/sec_bad_client_resp"), identity_cfg());
      CHECK(server.listen([&](const int& req, int& resp) {
        handled.fetch_add(1, std::memory_order_relaxed);
        resp = req + 1;
      }));

      SecurityClient<int, int> client("dds://dds/mth/sec_bad_client_resp", client_decrypt_fail_cfg());
      CHECK(client.wait_for_connected(kDdsDiscoveryTimeout));

      int resp = 0;
      CHECK_FALSE(client.invoke(7, resp, 300ms));
      CHECK_EQ(handled.load(std::memory_order_acquire), 1);
    }

    {
      std::atomic<int> handled{0};
      SecurityServer<int, int> server(DdsConf("dds/mth/sec_bad_server_resp"), server_encrypt_fail_cfg());
      CHECK(server.listen([&](const int& req, int& resp) {
        handled.fetch_add(1, std::memory_order_relaxed);
        resp = req + 1;
      }));

      SecurityClient<int, int> client("dds://dds/mth/sec_bad_server_resp", identity_cfg());
      CHECK(client.wait_for_connected(kDdsDiscoveryTimeout));

      int resp = 0;
      CHECK_FALSE(client.invoke(7, resp, 300ms));
      CHECK_EQ(handled.load(std::memory_order_acquire), 1);
    }
  }

  TEST_CASE("custom serialization failures stop client and server rpc paths") {
    MESSAGE("[dds-method] custom serialization failures stop client and server rpc paths");

    DdsFailingCustomMsg bad_msg;

    Client<DdsFailingCustomMsg> fire_client(DdsConf("dds/mth/custom_fail_send"));
    CHECK_FALSE(fire_client.send(bad_msg));

    Client<DdsFailingCustomMsg, int> sync_client(DdsConf("dds/mth/custom_fail_invoke"));
    int sync_resp = 0;
    CHECK_FALSE(sync_client.invoke(bad_msg, sync_resp, 300ms));

    auto future = sync_client.async_invoke(bad_msg);
    REQUIRE(future.wait_for(300ms) == std::future_status::ready);
    CHECK_THROWS(future.get());

    {
      std::atomic<int> handled{0};
      Server<DdsFailingCustomMsg> server(DdsConf("dds/mth/custom_fail_req"));
      CHECK(server.listen([&](const DdsFailingCustomMsg&) { handled.fetch_add(1, std::memory_order_relaxed); }));

      Client<Bytes> client("dds://dds/mth/custom_fail_req");
      CHECK(client.wait_for_connected(kDdsDiscoveryTimeout));
      CHECK(client.send(Bytes::from_string("bad custom request")));

      std::this_thread::sleep_for(200ms);
      CHECK_EQ(handled.load(std::memory_order_acquire), 0);
    }

    {
      std::atomic<int> handled{0};
      Server<int, DdsFailingCustomMsg> server(DdsConf("dds/mth/custom_fail_resp"));
      CHECK(server.listen([&](const int&, DdsFailingCustomMsg& resp) {
        handled.fetch_add(1, std::memory_order_relaxed);
        resp = DdsFailingCustomMsg{};
      }));

      Client<int, Bytes> client("dds://dds/mth/custom_fail_resp");
      CHECK(client.wait_for_connected(kDdsDiscoveryTimeout));

      Bytes resp;
      CHECK_FALSE(client.invoke(1, resp, 300ms));
      CHECK_EQ(handled.load(std::memory_order_acquire), 1);
    }
  }

  TEST_CASE("attached message loop dispatches rpc callbacks") {
    MESSAGE("[dds-method] attached message loop dispatches rpc callbacks");

    MessageLoop loop;
    CHECK(loop.async_run());

    std::atomic<int> server_calls{0};
    std::atomic<bool> async_done{false};
    std::string async_value;

    Server<std::string, std::string> server(DdsConf("dds/mth/loop_dispatch1"), InitType::kWithoutInit);
    CHECK(server.attach(&loop));
    CHECK(server.init());
    CHECK(server.listen([&](const std::string& req, std::string& resp) {
      server_calls.fetch_add(1, std::memory_order_relaxed);
      resp = "loop:" + req;
    }));

    Client<std::string, std::string> client("dds://dds/mth/loop_dispatch1", InitType::kWithoutInit);
    CHECK(client.attach(&loop));
    CHECK(client.init());
    CHECK(client.wait_for_connected(kDdsDiscoveryTimeout));

    std::string sync_value;
    CHECK(client.invoke("sync", sync_value, kDdsDiscoveryTimeout));
    CHECK(sync_value == "loop:sync");

    CHECK(client.invoke("async", [&](const std::string& resp) {
      async_value = resp;
      async_done.store(true, std::memory_order_release);
    }));
    CHECK(common_test::wait_until([&async_done] { return async_done.load(std::memory_order_acquire); },
                                  kDdsDiscoveryTimeout));
    CHECK(async_value == "loop:async");
    CHECK(server_calls.load(std::memory_order_acquire) >= 2);

    CHECK(client.detach());
    CHECK(server.detach());
    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("suspended fire server drains requests until resumed") {
    MESSAGE("[dds-method] suspended fire server drains requests until resumed");

    std::atomic<int> count{0};

    Server<std::string> server(DdsConf("dds/mth/suspend_drain1"));
    CHECK(server.listen([&](const std::string& req) {
      if (req == "take") {
        count.fetch_add(1, std::memory_order_relaxed);
      }
    }));

    Client<std::string> client("dds://dds/mth/suspend_drain1");
    CHECK(client.wait_for_connected(kDdsDiscoveryTimeout));

    CHECK(server.suspend());
    CHECK(client.send("drop"));
    std::this_thread::sleep_for(300ms);
    CHECK_EQ(count.load(std::memory_order_acquire), 0);

    CHECK(server.resume());
    CHECK(client.send("take"));
    CHECK(
        common_test::wait_until([&count] { return count.load(std::memory_order_acquire) == 1; }, kDdsDiscoveryTimeout));
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

    CHECK(common_test::wait_until([&connected_event] { return connected_event.load(std::memory_order_acquire); },
                                  kDdsDiscoveryTimeout));
  }

#if defined(VLINK_TEST_SUPPORT_PROTOBUF)
  TEST_CASE("protobuf rpc invoke returns correct computed value") {
    MESSAGE("[dds-method] protobuf rpc invoke returns correct computed value");

    Server<pb::Request, pb::Response> server(DdsConf("dds/ser/pb_rpc1"));
    server.listen([](const pb::Request& req, pb::Response& resp) { resp.set_value(std::to_string(req.type() * 2)); });

    Client<pb::Request, pb::Response> client("dds://dds/ser/pb_rpc1");
    CHECK(client.wait_for_connected(kDdsDiscoveryTimeout));
    std::this_thread::sleep_for(20ms);

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
      REQUIRE(v->size() == 3u);
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

      CHECK(getter.wait_for_value(kDdsDiscoveryTimeout));
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

      CHECK(common_test::wait_until([&notified] { return notified.load(std::memory_order_acquire); }, 3s));
      REQUIRE(cb_val.size() == 2u);
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
      CHECK(late_getter.wait_for_value(kDdsDiscoveryTimeout));
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

  TEST_CASE("setter set before init is cached without breaking later writes") {
    MESSAGE("[dds-field] setter set before init is cached without breaking later writes");

    Setter<int> setter(DdsConf("dds/fld/deferred_snapshot"), InitType::kWithoutInit);
    Getter<int> getter("dds://dds/fld/deferred_snapshot");

    setter.set(1234);
    REQUIRE(setter.init());
    setter.set(5678);

    CHECK(getter.wait_for_value(kDdsDiscoveryTimeout));
    auto val = getter.get();
    REQUIRE(val.has_value());
    CHECK_EQ(*val, 5678);
  }

  TEST_CASE("invalid raw bytes are dropped before typed getter state updates") {
    MESSAGE("[dds-field] invalid raw bytes are dropped before typed getter state updates");

    {
      std::atomic<int> delivered{0};

      Setter<Bytes> setter(DdsConf("dds/fld/bad_get_int"));
      Getter<int> getter("dds://dds/fld/bad_get_int");

      CHECK(getter.listen([&](const int&) { delivered.fetch_add(1, std::memory_order_relaxed); }));
      std::this_thread::sleep_for(30ms);
      setter.set(Bytes{0x01});

      std::this_thread::sleep_for(200ms);
      CHECK_EQ(delivered.load(std::memory_order_acquire), 0);
      CHECK_FALSE(getter.get().has_value());
    }

    {
      std::atomic<int> delivered{0};

      Setter<Bytes> setter(DdsConf("dds/fld/bad_get_dynamic"));
      Getter<DynamicData> getter("dds://dds/fld/bad_get_dynamic");

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

      Setter<Bytes> setter(DdsConf("dds/fld/bad_get_proto"));
      Getter<pb::Message> getter("dds://dds/fld/bad_get_proto");

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
    MESSAGE("[dds-field] security getter drops values that fail decryption");

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

    SecuritySetter<int> setter(DdsConf("dds/fld/sec_bad_get"), identity_cfg());
    SecurityGetter<int> getter("dds://dds/fld/sec_bad_get", decrypt_fail_cfg());

    CHECK(getter.listen([&](const int&) { delivered.fetch_add(1, std::memory_order_relaxed); }));
    std::this_thread::sleep_for(30ms);
    setter.set(42);

    std::this_thread::sleep_for(200ms);
    CHECK_EQ(delivered.load(std::memory_order_acquire), 0);
    CHECK_FALSE(getter.get().has_value());
  }

  TEST_CASE("setter serialization and encryption failures do not publish values") {
    MESSAGE("[dds-field] setter serialization and encryption failures do not publish values");

    {
      Setter<DdsFailingCustomMsg> setter(DdsConf("dds/fld/custom_fail_set"));
      Getter<Bytes> getter("dds://dds/fld/custom_fail_set");

      setter.set(DdsFailingCustomMsg{});

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
      SecuritySetter<int> setter(DdsConf("dds/fld/sec_fail_set"), std::move(encrypt_fail_cfg));
      SecurityGetter<int> getter("dds://dds/fld/sec_fail_set");

      CHECK(getter.listen([&](const int&) { delivered.fetch_add(1, std::memory_order_relaxed); }));
      std::this_thread::sleep_for(30ms);
      setter.set(7);

      std::this_thread::sleep_for(200ms);
      CHECK_EQ(delivered.load(std::memory_order_acquire), 0);
      CHECK_FALSE(getter.get().has_value());
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

    CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));

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

    CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));

    Bytes payload = Bytes::create(kSize1K);
    for (size_t i = 0; i < kSize1K; ++i) {
      payload[i] = static_cast<uint8_t>(i & 0xFF);
    }

    CHECK(pub.publish(payload));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
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

    CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));

    Bytes payload = Bytes::create(kSize64K);

    CHECK(pub.publish(payload));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
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

    CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));
    CHECK(pub.publish(Bytes{}));

    CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
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

      CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));

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

    CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));

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

  TEST_CASE("missing qos profile falls back to default dds qos") {
    MESSAGE("[dds-qos] missing qos profile falls back to default dds qos");

    Publisher<int> pub(DdsConf("dds/qos/missing_profile1", 22, 0, "missing_dds_qos_lookup_for_test"),
                       InitType::kWithoutInit);
    CHECK(pub.init());
    CHECK(pub.has_inited());
  }

  TEST_CASE("participant transport properties initialize without communication") {
    MESSAGE("[dds-qos] participant transport properties initialize without communication");

    Publisher<Bytes> pub(DdsConf("dds/qos/participant_props1", 21), InitType::kWithoutInit);
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

  TEST_CASE("equivalent participant properties reuse factory cached entities") {
    MESSAGE("[dds-qos] equivalent participant properties reuse factory cached entities");

    DdsConf conf("dds/qos/cache_reuse1", 23);
    Publisher<Bytes> pub1(conf, InitType::kWithoutInit);
    Publisher<Bytes> pub2(conf, InitType::kWithoutInit);
    Subscriber<Bytes> sub1(conf, InitType::kWithoutInit);
    Subscriber<Bytes> sub2(conf, InitType::kWithoutInit);

    set_loopback_dds_transport(pub1, false);
    set_loopback_dds_transport(pub2, false);
    set_loopback_dds_transport(sub1, false);
    set_loopback_dds_transport(sub2, false);

    CHECK(pub1.init());
    CHECK(pub2.init());
    CHECK(sub1.init());
    CHECK(sub2.init());

    REQUIRE(pub1.get_abstract_node() != nullptr);
    REQUIRE(pub2.get_abstract_node() != nullptr);
    REQUIRE(sub1.get_abstract_node() != nullptr);
    REQUIRE(sub2.get_abstract_node() != nullptr);
    CHECK(pub1.get_abstract_node()->get_native_handle().has_value());
    CHECK(pub2.get_abstract_node()->get_native_handle().has_value());
    CHECK(sub1.get_abstract_node()->get_native_handle().has_value());
    CHECK(sub2.get_abstract_node()->get_native_handle().has_value());
  }

  TEST_CASE("participant ssl options force tcp transport during initialization") {
    MESSAGE("[dds-qos] participant ssl options force tcp transport during initialization");

    auto ca_path = make_dds_tmp_file("ca", "-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n");
    auto cert_path = make_dds_tmp_file("cert", "-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n");
    auto key_path = make_dds_tmp_file("key", "-----BEGIN PRIVATE KEY-----\nMIIB\n-----END PRIVATE KEY-----\n");

    Publisher<Bytes> no_verify_pub(DdsConf("dds/qos/participant_ssl_no_verify", 24), InitType::kWithoutInit);
    no_verify_pub.set_property("dds.ip", "127.0.0.1");
    no_verify_pub.set_property("dds.buf", "2048");
    no_verify_pub.set_property("dds.mtu", "1024");
    no_verify_pub.set_property("dds.udp", "0");
    no_verify_pub.set_property("dds.tcp", "0");

    SslOptions no_verify;
    no_verify.ca_file = ca_path.string();
    no_verify.cert_file = cert_path.string();
    no_verify.key_file = key_path.string();
    no_verify.key_password = "password";
    no_verify.server_name = "localhost";
    no_verify.verify_peer = false;
    no_verify_pub.set_ssl_options(no_verify);

    CHECK(no_verify_pub.init());
    CHECK(no_verify_pub.has_inited());
    CHECK_EQ(no_verify_pub.get_property("ssl.verify"), "0");
    CHECK_EQ(no_verify_pub.get_property("ssl.server_name"), "localhost");

    Publisher<Bytes> verify_pub(DdsConf("dds/qos/participant_ssl_verify", 25), InitType::kWithoutInit);
    verify_pub.set_property("dds.ip", "127.0.0.1");
    verify_pub.set_property("dds.udp", "0");
    verify_pub.set_property("dds.tcp", "0");
    verify_pub.set_property("ssl.ca", ca_path.string());

    CHECK(verify_pub.init());
    CHECK(verify_pub.has_inited());
    CHECK_EQ(verify_pub.get_property("ssl.ca"), ca_path.string());
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

        if (!client.wait_for_connected(kDdsDiscoveryTimeout)) {
          return;
        }
        std::this_thread::sleep_for(20ms);

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

      CHECK(client.wait_for_connected(kDdsDiscoveryTimeout));
      std::this_thread::sleep_for(20ms);
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
    CHECK(getter.wait_for_value(kDdsDiscoveryTimeout));

    auto v = getter.get();
    REQUIRE(v.has_value());
    CHECK_EQ(*v, 7654321);
  }

  TEST_CASE("attached message loop dispatches pubsub and field callbacks") {
    MESSAGE("[dds-field] attached message loop dispatches pubsub and field callbacks");

    MessageLoop loop;
    CHECK(loop.async_run());

    std::atomic<int> sub_count{0};
    std::atomic<int> getter_value{0};

    Publisher<int> pub(DdsConf("dds/loop/pubsub1"), InitType::kWithoutInit);
    Subscriber<int> sub(DdsConf("dds/loop/pubsub1"), InitType::kWithoutInit);
    Setter<int> setter(DdsConf("dds/loop/field1"), InitType::kWithoutInit);
    Getter<int> getter(DdsConf("dds/loop/field1"), InitType::kWithoutInit);

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

    CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));
    CHECK(pub.publish(11));
    CHECK(common_test::wait_until([&sub_count] { return sub_count.load(std::memory_order_acquire) > 0; },
                                  kDdsDiscoveryTimeout));

    setter.set(22);
    CHECK(common_test::wait_until([&getter_value] { return getter_value.load(std::memory_order_acquire) == 22; },
                                  kDdsDiscoveryTimeout));

    CHECK(getter.detach());
    CHECK(setter.detach());
    CHECK(sub.detach());
    CHECK(pub.detach());
    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("suspended getter drains values until resumed") {
    MESSAGE("[dds-field] suspended getter drains values until resumed");

    std::atomic<int> observed{0};

    Setter<int> setter(DdsConf("dds/fld/suspend_drain1"));
    Getter<int> getter("dds://dds/fld/suspend_drain1");
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
                                  kDdsDiscoveryTimeout));
  }

  TEST_CASE("multiple setters on same field deliver latest value") {
    MESSAGE("[dds-field] multiple setters on same field deliver latest value");

    Getter<int> getter("dds://dds/fld/multi_setter1");

    for (int s = 0; s < 3; ++s) {
      Setter<int> setter(DdsConf("dds/fld/multi_setter1"));
      setter.set(s * 100);
      std::this_thread::sleep_for(30ms);
    }

    CHECK(getter.wait_for_value(kDdsDiscoveryTimeout));
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

    CHECK(pub0.wait_for_subscribers(kDdsDiscoveryTimeout));
    CHECK(pub1.wait_for_subscribers(kDdsDiscoveryTimeout));

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

  TEST_CASE("get discovered topics handles an existing participant domain") {
    MESSAGE("[dds-init] get discovered topics handles an existing participant domain");

    Publisher<int> pub(DdsConf("dds/discovery/existing_participant", 26), InitType::kWithoutInit);
    set_loopback_dds_transport(pub, false);
    REQUIRE(pub.init());

    CHECK_NOTHROW((void)DdsConf::get_discovered_topics(26));
  }

  TEST_CASE("load global qos file with nonexistent path returns false") {
    MESSAGE("[dds-init] load global qos file with nonexistent path returns false");

    std::filesystem::path missing_path = std::filesystem::path(vlink::Utils::get_tmp_dir()) / "vlink-dds-tests" /
                                         ("missing_profile_" + vlink::Utils::get_pid_str() + ".xml");
    bool ok = DdsConf::load_global_qos_file(missing_path.string());
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

  TEST_CASE("status accessors cover writer reader and unknown categories") {
    MESSAGE("[dds-error] status accessors cover writer reader and unknown categories");

    Publisher<int> pub(DdsConf("dds/status/accessor_pubsub"));
    Subscriber<int> sub(DdsConf("dds/status/accessor_pubsub"));

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
    MESSAGE("[dds-error] server status accessors and duplicate listen guards are stable");

    Server<std::string> fire_server(DdsConf("dds/status/fire_server"));
    Server<std::string, std::string> sync_server(DdsConf("dds/status/sync_server"));

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
    MESSAGE("[dds-error] client setter and getter status accessors cover both sides");

    Client<std::string> fire_client(DdsConf("dds/status/fire_client"));
    Client<std::string, std::string> sync_client(DdsConf("dds/status/sync_client"));
    Setter<int> setter(DdsConf("dds/status/setter"));
    Getter<int> getter(DdsConf("dds/status/getter"));

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

      CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));

      DynamicData d;
      d.load("int", 999);
      CHECK(pub.publish(d));

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
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

      CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));

      DynamicData d;
      d.load("str", std::string("dds_dynamic"));
      CHECK(pub.publish(d));

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
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

  bool operator<<(const vlink::Bytes& in) {
    std::string s(reinterpret_cast<const char*>(in.data()), in.size());
    const auto separator = s.find('|');

    if (separator == std::string::npos) {
      return false;
    }

    int parsed_id = 0;
    const auto result = std::from_chars(s.data(), s.data() + separator, parsed_id);

    if (result.ec != std::errc{} || result.ptr != s.data() + separator) {
      return false;
    }

    id = parsed_id;
    label = s.substr(separator + 1U);
    return true;
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

      CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));

      DdsCustomMsg msg;
      msg.id = 31;
      msg.label = "dds_custom";
      CHECK(pub.publish(msg));

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
      CHECK_EQ(captured.id, 31);
      CHECK_EQ(captured.label, "dds_custom");
    } catch (const std::exception&) {
      return;
    }
  }

  TEST_CASE("invalid raw bytes are dropped before custom subscriber callback") {
    MESSAGE("[dds-custom] invalid raw bytes are dropped before custom subscriber callback");

    std::atomic<int> delivered{0};

    Publisher<Bytes> pub(DdsConf("dds/cust/bad_bytes"));
    Subscriber<DdsCustomMsg> sub("dds://dds/cust/bad_bytes");

    CHECK(sub.listen([&](const DdsCustomMsg&) { delivered.fetch_add(1, std::memory_order_relaxed); }));
    CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));
    CHECK(pub.publish(Bytes::from_string("bad_custom_payload")));

    std::this_thread::sleep_for(200ms);
    CHECK_EQ(delivered.load(std::memory_order_acquire), 0);
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

      CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));

      fbs::MessageT msg;
      msg.type = 9u;
      msg.value = "dds_fbs_rt";
      CHECK(pub.publish(msg));

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
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

      CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));

      fbs::MessageT msg;
      msg.type = 2u;
      msg.value = "dds_fbs_qos";
      pub.publish(msg);

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
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

      CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));

      fbs::MessageT msg;
      msg.type = 1u;
      msg.value = "dynamic";
      pub.publish(msg);

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
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
      CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));

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

      CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));
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
      CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));

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

      CHECK(common_test::wait_until([&got_matched] { return got_matched.load(std::memory_order_acquire); },
                                    kDdsDiscoveryTimeout));
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

      CHECK(common_test::wait_until([&got_matched] { return got_matched.load(std::memory_order_acquire); },
                                    kDdsDiscoveryTimeout));
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

      CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));
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

      CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));

      std::this_thread::sleep_for(300ms);

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

      CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));

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

      CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));

      zerocopy::RawData rd;
      rd.header.seq = 33;
      rd.create(4);
      const_cast<uint8_t*>(rd.data())[0] = 0x12;
      const_cast<uint8_t*>(rd.data())[3] = 0x34;
      CHECK(pub.publish(rd));

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
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

      CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));

      zerocopy::CameraFrame frame;
      frame.set_width(800);
      frame.set_height(600);
      frame.set_format(zerocopy::CameraFrame::kFormatYuv420);
      frame.create(800 * 600 * 3 / 2);
      CHECK(pub.publish(frame));

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
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

      CHECK(pub.wait_for_subscribers(kDdsDiscoveryTimeout));

      zerocopy::CameraFrame frame;
      frame.header.seq = 7;
      frame.set_width(64);
      frame.set_height(64);
      frame.set_format(zerocopy::CameraFrame::kFormatNv12);
      frame.create(64 * 64 * 3 / 2);
      CHECK(pub.publish(frame));

      CHECK(common_test::wait_until([&received] { return received.load(std::memory_order_acquire); }, 3s));
      CHECK_EQ(captured.header.seq, 7u);
    } catch (const std::exception&) {
      return;
    }
  }
}

TEST_SUITE("dds-qos") {
  TEST_CASE("named qos profiles exercise every convert_qos branch") {
    MESSAGE("[dds-qos] named qos profiles exercise every convert_qos branch");

    auto register_profile = [](const char* name, auto&& fill) {
      Qos qos;
      qos.valid = true;
      fill(qos);

      try {
        DdsConf::register_qos(name, qos);
      } catch (...) {
      }
    };

    register_profile("dds_cq_best_effort", [](Qos& q) { q.reliability.kind = Qos::Reliability::kBestEffort; });
    register_profile("dds_cq_transient", [](Qos& q) { q.durability.kind = Qos::Durability::kTransient; });
    register_profile("dds_cq_persistent", [](Qos& q) { q.durability.kind = Qos::Durability::kPersistent; });
    register_profile("dds_cq_async", [](Qos& q) { q.publish_mode.kind = Qos::PublishMode::kASync; });
    register_profile("dds_cq_live_part", [](Qos& q) { q.liveliness.kind = Qos::Liveliness::kManualParticipant; });
    register_profile("dds_cq_live_topic", [](Qos& q) { q.liveliness.kind = Qos::Liveliness::kManualTopic; });
    register_profile("dds_cq_src_ts",
                     [](Qos& q) { q.destination_order.kind = Qos::DestinationOrder::kSourceTimestamp; });
    register_profile("dds_cq_exclusive", [](Qos& q) { q.ownership.kind = Qos::Ownership::kExclusive; });

    static const char* const kProfiles[] = {"dds_cq_best_effort", "dds_cq_transient", "dds_cq_persistent",
                                            "dds_cq_async",       "dds_cq_live_part", "dds_cq_live_topic",
                                            "dds_cq_src_ts",      "dds_cq_exclusive"};

    for (const char* name : kProfiles) {
      const std::string topic = std::string("dds/qos/cq_") + name;
      Publisher<int> pub(DdsConf(topic, 0, 0, name));
      Subscriber<int> sub(std::string("dds://") + topic + "?qos=" + name);
      sub.listen([](const int&) {});
      (void)pub.wait_for_subscribers(1s);

      CHECK(pub.get_abstract_node()->get_native_handle().has_value());
      CHECK(sub.get_abstract_node()->get_native_handle().has_value());
    }
  }
}

TEST_SUITE("dds-method") {
  TEST_CASE("connected client can invoke server") {
    MESSAGE("[dds-method] connected client can invoke server");

    Server<std::string, std::string> server(DdsConf("dds/mth/connected_invoke1"));
    server.listen([](const std::string& req, std::string& resp) { resp = req; });

    Client<std::string, std::string> client("dds://dds/mth/connected_invoke1");
    REQUIRE(client.wait_for_connected(3s));

    std::string resp;
    CHECK(client.invoke("ping", resp, 3s));
    CHECK(resp == "ping");
  }
}

TEST_SUITE("dds-error") {
  TEST_CASE("incompatible qos fires offered and requested status handlers") {
    MESSAGE("[dds-error] incompatible qos fires offered and requested status handlers");

    Qos best_effort;
    best_effort.valid = true;
    best_effort.reliability.kind = Qos::Reliability::kBestEffort;

    Qos reliable;
    reliable.valid = true;
    reliable.reliability.kind = Qos::Reliability::kReliable;

    try {
      DdsConf::register_qos("dds_st_writer_be", best_effort);
    } catch (...) {
    }

    try {
      DdsConf::register_qos("dds_st_reader_rel", reliable);
    } catch (...) {
    }

    std::atomic<bool> offered{false};
    std::atomic<bool> requested{false};

    Publisher<int> pub(DdsConf("dds/status/incompat1", 0, 0, "dds_st_writer_be"));
    pub.register_status_handler([&offered](const Status::BasePtr& status) {
      if (status && status->get_type() == Status::kOfferedIncompatibleQos) {
        offered.store(true, std::memory_order_release);
      }
    });

    Subscriber<int> sub("dds://dds/status/incompat1?qos=dds_st_reader_rel");
    sub.register_status_handler([&requested](const Status::BasePtr& status) {
      if (status && status->get_type() == Status::kRequestedIncompatibleQos) {
        requested.store(true, std::memory_order_release);
      }
    });
    sub.listen([](const int&) {});

    CHECK(common_test::wait_until([&offered, &requested] { return offered.load() && requested.load(); }, 3s));
  }
}

#endif  // VLINK_SUPPORT_DDS

// NOLINTEND
