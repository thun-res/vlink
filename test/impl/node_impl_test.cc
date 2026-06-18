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

#include "./impl/node_impl.h"

#include <doctest/doctest.h>

#include <any>
#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "../common_test.h"
#include "./base/message_loop.h"
#include "./extension/status_detail.h"
#include "./impl/types.h"

namespace {

class TestNodeImpl : public NodeImpl {
 public:
  explicit TestNodeImpl(ImplType type = kPublisher) : NodeImpl(type) {}
  ~TestNodeImpl() override = default;

  void init() override { init_called = true; }
  void deinit() override { deinit_called = true; }

  bool init_called{false};
  bool deinit_called{false};
};

}  // namespace

TEST_SUITE("impl-AbstractNode") {
  TEST_CASE("get_native_handle returns any containing nullptr by default") {
    class ConcreteAbstractNode : public AbstractNode {
     public:
      ConcreteAbstractNode() = default;
      ~ConcreteAbstractNode() override = default;
    };

    ConcreteAbstractNode node;
    auto handle = node.get_native_handle();
    CHECK(handle.has_value());
    CHECK(std::any_cast<std::nullptr_t>(handle) == nullptr);
  }
}

TEST_SUITE("impl-NodeImpl") {
  TEST_CASE("constructor stores the given impl_type") {
    SUBCASE("kPublisher") {
      TestNodeImpl n(kPublisher);
      CHECK_EQ(n.impl_type, kPublisher);
    }
    SUBCASE("kSubscriber") {
      TestNodeImpl n(kSubscriber);
      CHECK_EQ(n.impl_type, kSubscriber);
    }
    SUBCASE("kClient") {
      TestNodeImpl n(kClient);
      CHECK_EQ(n.impl_type, kClient);
    }
    SUBCASE("kServer") {
      TestNodeImpl n(kServer);
      CHECK_EQ(n.impl_type, kServer);
    }
    SUBCASE("kGetter") {
      TestNodeImpl n(kGetter);
      CHECK_EQ(n.impl_type, kGetter);
    }
    SUBCASE("kSetter") {
      TestNodeImpl n(kSetter);
      CHECK_EQ(n.impl_type, kSetter);
    }
  }

  TEST_CASE("public member fields have correct defaults on construction") {
    TestNodeImpl node;
    CHECK(node.url.empty());
    CHECK(node.ser_type.empty());
    CHECK_EQ(node.transport_type, TransportType::kUnknown);
    CHECK(node.is_cdr_type == false);
    CHECK(node.is_security_type == false);
    CHECK(node.is_discovery_enabled == true);
    CHECK(node.profiler == nullptr);
    CHECK(node.security == nullptr);
    CHECK(node.has_suspend == false);
  }

  TEST_CASE("is_support_loan returns false by default") {
    TestNodeImpl node;
    CHECK(node.is_support_loan() == false);
  }

  TEST_CASE("loan returns an empty Bytes by default") {
    TestNodeImpl node;
    auto bytes = node.loan(1024);
    CHECK(bytes.empty());
  }

  TEST_CASE("return_loan returns false by default") {
    TestNodeImpl node;
    Bytes b;
    CHECK(node.return_loan(b) == false);
  }

  TEST_CASE("set_manual_unloan does not crash") {
    TestNodeImpl node;
    node.set_manual_unloan(true);
    node.set_manual_unloan(false);
  }

  TEST_CASE("suspend returns false by default") {
    TestNodeImpl node;
    CHECK(node.suspend() == false);
  }

  TEST_CASE("resume returns false by default") {
    TestNodeImpl node;
    CHECK(node.resume() == false);
  }

  TEST_CASE("is_suspend returns false by default") {
    TestNodeImpl node;
    CHECK(node.is_suspend() == false);
  }

  TEST_CASE("is_interrupted returns false on construction") {
    TestNodeImpl node;
    CHECK(node.is_interrupted() == false);
  }

  TEST_CASE("interrupt sets the interrupted flag") {
    TestNodeImpl node;
    node.interrupt();
    CHECK(node.is_interrupted() == true);
  }

  TEST_CASE("reset_interrupted clears the interrupted flag") {
    TestNodeImpl node;
    node.interrupt();
    node.reset_interrupted();
    CHECK(node.is_interrupted() == false);
  }

  TEST_CASE("multiple interrupt calls are idempotent") {
    TestNodeImpl node;
    node.interrupt();
    node.interrupt();
    CHECK(node.is_interrupted() == true);
    node.reset_interrupted();
    CHECK(node.is_interrupted() == false);
  }

  TEST_CASE("set and get a property round-trips correctly") {
    TestNodeImpl node;
    node.set_property("key", "value");
    CHECK_EQ(node.get_property("key"), "value");
  }

  TEST_CASE("get missing property returns empty string") {
    TestNodeImpl node;
    CHECK(node.get_property("missing").empty());
  }

  TEST_CASE("overwriting a property replaces the value") {
    TestNodeImpl node;
    node.set_property("k", "old");
    node.set_property("k", "new");
    CHECK_EQ(node.get_property("k"), "new");
  }

  TEST_CASE("get_all_properties returns empty map initially") {
    TestNodeImpl node;
    CHECK(node.get_all_properties().empty());
  }

  TEST_CASE("get_all_properties returns snapshot of all entries") {
    TestNodeImpl node;
    node.set_property("a", "1");
    node.set_property("b", "2");
    auto props = node.get_all_properties();
    CHECK_EQ(props.size(), 2u);
    CHECK_EQ(props.at("a"), "1");
    CHECK_EQ(props.at("b"), "2");
  }

  TEST_CASE("get_message_loop returns nullptr initially") {
    TestNodeImpl node;
    CHECK(node.get_message_loop() == nullptr);
  }

  TEST_CASE("attach to message loop succeeds on first call") {
    TestNodeImpl node;
    MessageLoop loop;
    CHECK(node.attach(&loop) == true);
    CHECK(node.get_message_loop() == &loop);
  }

  TEST_CASE("attach fails when a different loop is already attached") {
    TestNodeImpl node;
    MessageLoop loop1;
    MessageLoop loop2;
    CHECK(node.attach(&loop1) == true);
    CHECK(node.attach(&loop2) == false);
    CHECK_EQ(node.get_message_loop(), &loop1);
  }

  TEST_CASE("detach returns false when no loop is attached") {
    TestNodeImpl node;
    CHECK(node.detach() == false);
  }

  TEST_CASE("attach then detach leaves message_loop null") {
    TestNodeImpl node;
    MessageLoop loop;
    loop.async_run();
    CHECK(node.attach(&loop) == true);
    CHECK(node.detach() == true);
    CHECK(node.get_message_loop() == nullptr);
    loop.quit();
  }

  TEST_CASE("re-attach succeeds after detach") {
    TestNodeImpl node;
    MessageLoop loop;
    loop.async_run();
    node.attach(&loop);
    node.detach();
    CHECK(node.attach(&loop) == true);
    CHECK_EQ(node.get_message_loop(), &loop);
    node.detach();
    loop.quit();
  }

  TEST_CASE("register_status_handler on non-DDS transport is a no-op") {
    TestNodeImpl node;
    node.transport_type = TransportType::kIntra;
    node.register_status_handler([](const Status::BasePtr&) {});
    CHECK(node.has_register_status() == false);
  }

  TEST_CASE("register_status_handler on DDS transport stores the handler") {
    TestNodeImpl node;
    node.transport_type = TransportType::kDds;
    node.register_status_handler([](const Status::BasePtr&) {});
    CHECK(node.has_register_status() == true);
  }

  TEST_CASE("register_status_handler works for all DDS family transports") {
    for (auto tt : {TransportType::kDds, TransportType::kDdsc, TransportType::kDdsr, TransportType::kDdst}) {
      TestNodeImpl node;
      node.transport_type = tt;
      node.register_status_handler([](const Status::BasePtr&) {});
      CHECK(node.has_register_status() == true);
    }
  }

  TEST_CASE("has_register_status returns false for non-DDS transports") {
    TestNodeImpl node;
    node.transport_type = TransportType::kShm;
    CHECK(node.has_register_status() == false);
  }

  TEST_CASE("call_status on DDS transport invokes the callback directly") {
    TestNodeImpl node;
    node.transport_type = TransportType::kDds;
    Status::Type received = Status::kUnknown;
    node.register_status_handler([&](const Status::BasePtr& ptr) { received = ptr->get_type(); });

    auto status = std::make_shared<Status::PublicationMatched>();
    node.call_status(status);

    CHECK_EQ(received, Status::kPublicationMatched);
  }

  TEST_CASE("call_status dispatched via attached message loop") {
    TestNodeImpl node;
    node.transport_type = TransportType::kDds;
    MessageLoop loop;
    loop.async_run();
    node.attach(&loop);

    std::atomic_bool called{false};
    node.register_status_handler([&](const Status::BasePtr&) { called = true; });
    node.call_status(std::make_shared<Status::PublicationMatched>());

    std::this_thread::sleep_for(50ms);
    CHECK(called == true);

    node.detach();
    loop.quit();
  }

  TEST_CASE("get_conf returns nullptr by default") {
    TestNodeImpl node;
    CHECK(node.get_conf() == nullptr);
  }

  TEST_CASE("get_abstract_node returns nullptr by default") {
    TestNodeImpl node;
    CHECK(node.get_abstract_node() == nullptr);
  }

  TEST_CASE("get_status returns unknown status by default") {
    TestNodeImpl node;
    auto status = node.get_status(Status::kPublicationMatched);
    REQUIRE(status != nullptr);
    CHECK_EQ(status->get_type(), Status::kUnknown);
  }

  TEST_CASE("get_target_conf returns nullptr when get_conf returns nullptr") {
    TestNodeImpl node;
    CHECK(node.get_target_conf<Conf>() == nullptr);
  }

  TEST_CASE("discovery reporting is enabled by default") {
    TestNodeImpl node;
    CHECK(node.get_discovery_enabled() == true);
  }

  TEST_CASE("set_discovery_enabled false is reflected by getter") {
    TestNodeImpl node;
    node.set_discovery_enabled(false);
    CHECK(node.get_discovery_enabled() == false);
  }

  TEST_CASE("set_discovery_enabled toggle round-trips correctly") {
    TestNodeImpl node;
    node.set_discovery_enabled(false);
    node.set_discovery_enabled(true);
    CHECK(node.get_discovery_enabled() == true);
  }

  TEST_CASE("set_record_path with empty string does not crash") {
    TestNodeImpl node;
    node.set_record_path("");
  }

  TEST_CASE("try_record without a recorder does not crash") {
    TestNodeImpl node;
    node.url = "intra://test";
    node.transport_type = TransportType::kIntra;
    Bytes data;
    node.try_record(ActionType::kUnknownAction, data);
  }

  TEST_CASE("init_ext and deinit_ext do not crash for intra transport") {
    TestNodeImpl node;
    node.url = "intra://test_topic";
    node.transport_type = TransportType::kIntra;
    node.init_ext();
    node.deinit_ext();
  }

  TEST_CASE("init_ext with discovery disabled does not crash") {
    TestNodeImpl node;
    node.url = "dds://test_topic";
    node.transport_type = TransportType::kDds;
    node.set_discovery_enabled(false);
    node.init_ext();
    node.deinit_ext();
  }

  TEST_CASE("global_init is safe to call multiple times") {
    NodeImpl::global_init();
    NodeImpl::global_init();
  }

  TEST_CASE("has_suspend defaults to false and is writable") {
    TestNodeImpl node;
    CHECK(node.has_suspend == false);
    node.has_suspend = true;
    CHECK(node.has_suspend == true);
  }

  TEST_CASE("concurrent property writes and reads are race-free") {
    static constexpr int kIters = 200;

    TestNodeImpl node;

    std::thread writer([&] {
      for (int i = 0; i < kIters; ++i) {
        node.set_property("concurrent_key", std::to_string(i));
      }
    });

    std::thread reader([&] {
      for (int i = 0; i < kIters; ++i) {
        (void)node.get_property("concurrent_key");
      }
    });

    writer.join();
    reader.join();

    auto val = node.get_property("concurrent_key");
    CHECK_FALSE(val.empty());
  }

  TEST_CASE("interrupt from a separate thread is seen by is_interrupted") {
    TestNodeImpl node;

    std::thread t([&] { node.interrupt(); });

    t.join();

    CHECK(node.is_interrupted() == true);
    node.reset_interrupted();
    CHECK(node.is_interrupted() == false);
  }

  TEST_CASE("multiple attach-detach cycles leave message loop null") {
    TestNodeImpl node;
    MessageLoop loop;
    loop.async_run();

    for (int i = 0; i < 3; ++i) {
      CHECK(node.attach(&loop) == true);
      CHECK(node.detach() == true);
      CHECK(node.get_message_loop() == nullptr);
    }

    loop.quit();
  }

  TEST_CASE("set_property with many distinct keys all round-trip") {
    static constexpr int kKeys = 20;

    TestNodeImpl node;

    for (int i = 0; i < kKeys; ++i) {
      node.set_property("key" + std::to_string(i), "val" + std::to_string(i));
    }

    for (int i = 0; i < kKeys; ++i) {
      CHECK_EQ(node.get_property("key" + std::to_string(i)), "val" + std::to_string(i));
    }

    CHECK_EQ(node.get_all_properties().size(), static_cast<size_t>(kKeys));
  }

  TEST_CASE("schema_type defaults to unknown") {
    TestNodeImpl node;
    CHECK_EQ(node.schema_type, SchemaType::kUnknown);
  }

  TEST_CASE("register_status_handler on kDdsr and kDdst transports stores handler") {
    for (auto tt : {TransportType::kDdsr, TransportType::kDdst}) {
      TestNodeImpl node;
      node.transport_type = tt;
      node.register_status_handler([](const Status::BasePtr&) {});
      CHECK(node.has_register_status() == true);
    }
  }

  TEST_CASE("set_discovery_enabled propagates through multiple nodes independently") {
    TestNodeImpl node1;
    TestNodeImpl node2;

    node1.set_discovery_enabled(false);

    CHECK(node1.get_discovery_enabled() == false);
    CHECK(node2.get_discovery_enabled() == true);
  }

  TEST_CASE("deinit_ext after init_ext does not crash for dds transport") {
    TestNodeImpl node;
    node.url = "dds://test_topic_ext";
    node.transport_type = TransportType::kDds;
    node.set_discovery_enabled(false);
    node.init_ext();
    node.deinit_ext();
    node.init_ext();
    node.deinit_ext();
  }

  TEST_CASE("try_record with empty bytes and various transport types does not crash") {
    for (auto tt : {TransportType::kIntra, TransportType::kShm, TransportType::kDds}) {
      TestNodeImpl node;
      node.url = "intra://test_record";
      node.transport_type = tt;
      Bytes empty;
      node.try_record(ActionType::kUnknownAction, empty);
    }
  }

  TEST_CASE("check_version does not throw") {
    TestNodeImpl node;
    Version v_zero{0, 0, 0};
    Version v_neg{-1, -1, -1};
    CHECK_NOTHROW((void)node.check_version(v_zero));
    CHECK_NOTHROW((void)node.check_version(v_neg));
  }
}

TEST_SUITE("impl-NodeImpl") {
  TEST_CASE("register_status_handler on non-dds transport is a no-op without crash") {
    for (auto tt : {TransportType::kIntra, TransportType::kShm, TransportType::kShm2, TransportType::kZenoh}) {
      TestNodeImpl node;
      node.transport_type = tt;
      CHECK_NOTHROW(node.register_status_handler([](const Status::BasePtr&) {}));
      CHECK(node.has_register_status() == false);
    }
  }

  TEST_CASE("second register_status_handler replaces the first callback") {
    TestNodeImpl node;
    node.transport_type = TransportType::kDds;

    std::atomic<int> first_count{0};
    std::atomic<int> second_count{0};

    node.register_status_handler([&](const Status::BasePtr&) { first_count.fetch_add(1, std::memory_order_relaxed); });
    node.register_status_handler([&](const Status::BasePtr&) { second_count.fetch_add(1, std::memory_order_relaxed); });

    auto s = std::make_shared<Status::PublicationMatched>();
    node.call_status(s);

    CHECK_EQ(first_count.load(), 0);
    CHECK_EQ(second_count.load(), 1);
  }

  TEST_CASE("call_status delivers correct type for each status kind") {
    TestNodeImpl node;
    node.transport_type = TransportType::kDds;

    std::atomic<int> received_type{-1};
    node.register_status_handler([&](const Status::BasePtr& ptr) {
      received_type.store(static_cast<int>(ptr->get_type()), std::memory_order_relaxed);
    });

    SUBCASE("subscription matched") {
      node.call_status(std::make_shared<Status::SubscriptionMatched>());
      CHECK_EQ(received_type.load(), static_cast<int>(Status::kSubscriptionMatched));
    }

    SUBCASE("offered deadline missed") {
      node.call_status(std::make_shared<Status::OfferedDeadlineMissed>());
      CHECK_EQ(received_type.load(), static_cast<int>(Status::kOfferedDeadlineMissed));
    }

    SUBCASE("liveliness lost") {
      node.call_status(std::make_shared<Status::LivelinessLost>());
      CHECK_EQ(received_type.load(), static_cast<int>(Status::kLivelinessLost));
    }
  }
}

// NOLINTEND
