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

#include "./impl/url.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../common_test.h"
#include "./base/process.h"
#include "./base/utils.h"

namespace {

class ExposedConf final : public Conf {
 public:
  bool parse_protocol_public(Protocol* protocol) { return parse_protocol(protocol); }

  auto create_server_public() const { return create_server(); }

  auto create_client_public() const { return create_client(); }

  auto create_publisher_public() const { return create_publisher(); }

  auto create_subscriber_public() const { return create_subscriber(); }

  auto create_setter_public() const { return create_setter(); }

  auto create_getter_public() const { return create_getter(); }
};

void run_url_child_case(const std::string& name, Process::EnvironmentMap environment) {
  environment["VLINK_URL_CHILD_CASE"] = name;
  environment.try_emplace("VLINK_URL_PLUGINS", "");

  Process child;
  child.set_process_mode(Process::kForwardedMode);
  child.set_inherit_environment(true);
  child.set_environment(environment);
  child.start(Utils::get_app_path(),
              {"--test-suite=impl-Url", "--test-case=child process covers url environment branches", "--no-version"});
  REQUIRE(child.wait_for_finished(Process::kDefaultExecuteTimeoutMs));
  CHECK_EQ(child.get_exit_code(), 0);
}

}  // namespace

TEST_SUITE("impl-Url") {
  TEST_CASE("construct from string stores the url in get_str") {
    Url url("intra://test_topic");

    CHECK_EQ(url.get_str(), "intra://test_topic");
  }

  TEST_CASE("get_target returns non-null for a supported url") {
    Url url("intra://topic");

    CHECK_NE(url.get_target(), nullptr);
  }

  TEST_CASE("get_transport_type returns kIntra for intra url") {
    Url url("intra://topic");

    CHECK_EQ(url.get_transport_type(), TransportType::kIntra);
  }

  TEST_CASE("copy constructor produces independent object with same string") {
    Url original("intra://copy_test");
    Url copy(original);

    CHECK_EQ(copy.get_str(), original.get_str());
    CHECK_NE(copy.get_target(), original.get_target());
  }

  TEST_CASE("move constructor transfers string and target") {
    Url original("intra://move_test");
    const std::string expected = original.get_str();

    Url moved(std::move(original));

    CHECK_EQ(moved.get_str(), expected);
    CHECK_NE(moved.get_target(), nullptr);
    CHECK_EQ(original.get_target(), nullptr);
  }

  TEST_CASE("copy assignment replaces destination with copy of source") {
    Url a("intra://topic_a");
    Url b("intra://topic_b");

    b = a;

    CHECK_EQ(b.get_str(), a.get_str());
    CHECK_NE(b.get_target(), a.get_target());
  }

  TEST_CASE("move assignment transfers source to destination") {
    Url a("intra://topic_a");
    Url b("intra://topic_b");
    const std::string expected = a.get_str();

    b = std::move(a);

    CHECK_EQ(b.get_str(), expected);
    CHECK_EQ(a.get_target(), nullptr);
  }

  TEST_CASE("copy and move self-assignment leave url state intact") {
    Url url("intra://self_assign");
    const Conf* target = url.get_target();

    Url& copy_ref = url;
    url = copy_ref;
    CHECK_EQ(url.get_str(), "intra://self_assign");
    CHECK_EQ(url.get_target(), target);

    Url& move_ref = url;
    url = std::move(move_ref);
    CHECK_EQ(url.get_str(), "intra://self_assign");
    CHECK_EQ(url.get_target(), target);
  }

  TEST_CASE("parse kPublisher succeeds for intra url") {
    Url url("intra://parse_test");

    CHECK(url.parse(kPublisher));
  }

  TEST_CASE("parse kSubscriber succeeds for intra url") {
    Url url("intra://parse_test");

    CHECK(url.parse(kSubscriber));
  }

  TEST_CASE("parse kServer succeeds for intra url") {
    Url url("intra://server_test");

    CHECK(url.parse(kServer));
  }

  TEST_CASE("parse kClient succeeds for intra url") {
    Url url("intra://client_test");

    CHECK(url.parse(kClient));
  }

  TEST_CASE("parse kSetter succeeds for intra url") {
    Url url("intra://setter_test");

    CHECK(url.parse(kSetter));
  }

  TEST_CASE("parse kGetter succeeds for intra url") {
    Url url("intra://getter_test");

    CHECK(url.parse(kGetter));
  }

  TEST_CASE("is_valid returns true for intra url after parse") {
    Url url("intra://valid_test");
    url.parse(kPublisher);

    CHECK(url.is_valid());
  }

  TEST_CASE("get_impl_type contains kPublisher bit after parsing as publisher") {
    Url url("intra://impl_test");
    url.parse(kPublisher);

    CHECK_NE((url.get_impl_type() & kPublisher), 0);
  }

  TEST_CASE("get_str preserves url that contains query parameters") {
    Url url("intra://topic?key=value");

    CHECK_NE(url.get_str().find("intra://"), std::string::npos);
  }

  TEST_CASE("get_str preserves url with underscores in topic name") {
    Url url("intra://topic_with_underscores");

    CHECK_EQ(url.get_str(), "intra://topic_with_underscores");
    CHECK_NE(url.get_target(), nullptr);
  }

  TEST_CASE("get_transport_type returns kIntra for intra url with multi-segment path") {
    Url url("intra://ns/topic/sub");

    CHECK_EQ(url.get_transport_type(), TransportType::kIntra);
  }

  TEST_CASE("url stream output includes impl type and address") {
    Url url("intra://print_topic");
    REQUIRE(url.parse(kPublisher));

    std::ostringstream out;
    out << url;

    CHECK_NE(out.str().find("Url:"), std::string::npos);
    CHECK_NE(out.str().find("intra://print_topic"), std::string::npos);
  }

  TEST_CASE("load_for_plugin returns nullptr for unknown transport") {
    auto plugin_conf = Url::load_for_plugin(TransportType::kUnknown);
    CHECK_EQ(plugin_conf, nullptr);
  }

  TEST_CASE("global_init accepts an explicit supported transport mask") {
#ifdef VLINK_SUPPORT_INTRA
    Url::global_init(Url::kEnableIntra);
#else
    Url::global_init(Url::kEnableEmpty);
#endif
  }

  TEST_CASE("child process covers url environment branches") {
    const auto child_case = Utils::get_env("VLINK_URL_CHILD_CASE");

    if (child_case == "plugin-autoload-disabled") {
      Url::init_plugins(Url::kEnableEmpty);
      CHECK_EQ(Url::load_for_plugin(TransportType::kIntra), nullptr);
      return;
    }

    if (child_case == "plugin-autoload-enabled") {
#if !defined(VLINK_LIBRARY_STATIC)
      Url::init_plugins(Url::kEnableEmpty);

      std::vector<TransportType> types;

#ifdef VLINK_SUPPORT_INTRA
      types.push_back(TransportType::kIntra);
#endif
#ifdef VLINK_SUPPORT_SHM
      types.push_back(TransportType::kShm);
#endif
#ifdef VLINK_SUPPORT_SHM2
      types.push_back(TransportType::kShm2);
#endif
#ifdef VLINK_SUPPORT_ZENOH
      types.push_back(TransportType::kZenoh);
#endif
#ifdef VLINK_SUPPORT_DDS
      types.push_back(TransportType::kDds);
#endif
#ifdef VLINK_SUPPORT_DDSC
      types.push_back(TransportType::kDdsc);
#endif
#ifdef VLINK_SUPPORT_DDSR
      types.push_back(TransportType::kDdsr);
#endif
#ifdef VLINK_SUPPORT_SOMEIP
      types.push_back(TransportType::kSomeip);
#endif
#ifdef VLINK_SUPPORT_MQTT
      types.push_back(TransportType::kMqtt);
#endif
#ifdef VLINK_SUPPORT_FDBUS
      types.push_back(TransportType::kFdbus);
#endif

      const std::vector<TransportType> all_types{
          TransportType::kIntra,
#if !defined(__ANDROID__)
          TransportType::kShm,    TransportType::kShm2,
#endif
          TransportType::kZenoh,  TransportType::kDds,  TransportType::kDdsc,  TransportType::kDdsr,
          TransportType::kSomeip, TransportType::kMqtt, TransportType::kFdbus,
      };

      for (const auto type : all_types) {
        auto conf = Url::load_for_plugin(type);

        if (std::find(types.begin(), types.end(), type) == types.end()) {
          if (conf) {
            CHECK_EQ(conf->get_transport_type(), type);
          }

          continue;
        }

        REQUIRE(conf != nullptr);
        CHECK_EQ(conf->get_transport_type(), type);
      }

      if (types.empty()) {
        CHECK_EQ(Url::load_for_plugin(TransportType::kUnknown), nullptr);
      } else {
        auto cached = Url::load_for_plugin(types.front());
        REQUIRE(cached != nullptr);
        CHECK_EQ(cached->get_transport_type(), types.front());
      }
#else
      CHECK_EQ(Url::load_for_plugin(TransportType::kUnknown), nullptr);
#endif
      return;
    }

    if (child_case == "plugin-autoload-sampled-once") {
      CHECK_EQ(Url::load_for_plugin(TransportType::kIntra), nullptr);
      REQUIRE(Utils::set_env("VLINK_URL_PLUGINS", "auto"));
      CHECK_EQ(Url::load_for_plugin(TransportType::kIntra), nullptr);
      return;
    }

    if (child_case == "plugin-list-sampled-once") {
      CHECK_EQ(Url::load_for_plugin(TransportType::kIntra), nullptr);
      REQUIRE(Utils::set_env("VLINK_URL_PLUGINS", "vlink-intra"));
      Url::init_plugins(Url::kEnableEmpty);
      CHECK_EQ(Url::load_for_plugin(TransportType::kIntra), nullptr);
      return;
    }

    if (child_case == "plugin-autoload-concurrent") {
#if defined(VLINK_SUPPORT_INTRA) && !defined(VLINK_LIBRARY_STATIC)
      constexpr std::size_t kThreadCount = 8;
      std::vector<std::unique_ptr<Conf>> results(kThreadCount);
      std::vector<std::thread> threads;
      threads.reserve(kThreadCount);

      for (std::size_t index = 0; index < kThreadCount; ++index) {
        threads.emplace_back([&results, index]() { results[index] = Url::load_for_plugin(TransportType::kIntra); });
      }

      for (auto& thread : threads) {
        thread.join();
      }

      for (const auto& conf : results) {
        REQUIRE(conf != nullptr);
        CHECK_EQ(conf->get_transport_type(), TransportType::kIntra);
      }
#else
      CHECK_EQ(Url::load_for_plugin(TransportType::kUnknown), nullptr);
#endif
      return;
    }

    if (child_case == "plugin-explicit-preload") {
      Url::init_plugins(Url::kEnableEmpty);

#if defined(VLINK_SUPPORT_INTRA) && !defined(VLINK_LIBRARY_STATIC)
      auto conf = Url::load_for_plugin(TransportType::kIntra);
      REQUIRE(conf != nullptr);
      CHECK_EQ(conf->get_transport_type(), TransportType::kIntra);
#else
      CHECK_EQ(Url::load_for_plugin(TransportType::kUnknown), nullptr);
#endif
      return;
    }

    if (child_case == "plugin-explicit-preload-with-linked-flag") {
      Url::init_plugins(Url::kEnableIntra);

#if defined(VLINK_SUPPORT_INTRA) && !defined(VLINK_LIBRARY_STATIC)
      auto conf = Url::load_for_plugin(TransportType::kIntra);
      REQUIRE(conf != nullptr);
      CHECK_EQ(conf->get_transport_type(), TransportType::kIntra);
#else
      CHECK_EQ(Url::load_for_plugin(TransportType::kUnknown), nullptr);
#endif
      return;
    }

    if (child_case == "url-plugins") {
      Url::init_plugins(Url::kEnableIntra);
      CHECK_EQ(Url::load_for_plugin(TransportType::kUnknown), nullptr);
      return;
    }

#ifdef VLINK_SUPPORT_DDSC
    if (child_case == "dds-bind") {
      Url url("dds://bind/topic");
      CHECK_EQ(url.get_transport_type(), TransportType::kDdsc);
      return;
    }

    if (child_case == "intra-bind") {
      Url url("intra://bind/topic");
      CHECK_EQ(url.get_transport_type(), TransportType::kDdsc);
      return;
    }
#endif

    if (!child_case.empty()) {
      return;
    }

    run_url_child_case("plugin-autoload-disabled", {});
    run_url_child_case("plugin-autoload-disabled", {{"VLINK_URL_PLUGINS", "NoNe"}});
    run_url_child_case("plugin-autoload-disabled", {{"VLINK_URL_PLUGINS", "auto "}});
    run_url_child_case("plugin-autoload-enabled", {{"VLINK_URL_PLUGINS", "auto"}});
    run_url_child_case("plugin-autoload-enabled", {{"VLINK_URL_PLUGINS", "AuTo"}});
    run_url_child_case("plugin-autoload-sampled-once", {});
    run_url_child_case("plugin-list-sampled-once", {});
    run_url_child_case("plugin-autoload-concurrent", {{"VLINK_URL_PLUGINS", "AUTO"}});
    run_url_child_case("plugin-explicit-preload", {{"VLINK_URL_PLUGINS", "vlink-intra"}});
    run_url_child_case("plugin-explicit-preload-with-linked-flag", {{"VLINK_URL_PLUGINS", "vlink-intra"}});
    run_url_child_case("url-plugins", {{"VLINK_URL_PLUGINS", "vlink-unknown vlink-intra vlink-ddsc"}});

#ifdef VLINK_SUPPORT_DDSC
    run_url_child_case("dds-bind", {{"VLINK_DDS_BIND", "ddsc"}});
    run_url_child_case("intra-bind", {{"VLINK_INTRA_BIND", "ddsc"}});
#endif
  }

  TEST_CASE("base Conf default hooks reject protocol and create no implementations") {
    ExposedConf conf;

    CHECK_FALSE(conf.is_valid());
    CHECK_EQ(conf.get_impl_type(), kUnknownImplType);
    CHECK_EQ(conf.get_transport_type(), TransportType::kUnknown);
    CHECK_FALSE(conf.parse_protocol_public(nullptr));

    REQUIRE(conf.parse(kPublisher));
    CHECK_EQ(conf.get_impl_type(), kPublisher);
    CHECK_EQ(conf.create_server_public(), nullptr);
    CHECK_EQ(conf.create_client_public(), nullptr);
    CHECK_EQ(conf.create_publisher_public(), nullptr);
    CHECK_EQ(conf.create_subscriber_public(), nullptr);
    CHECK_EQ(conf.create_setter_public(), nullptr);
    CHECK_EQ(conf.create_getter_public(), nullptr);
  }

#ifdef VLINK_SUPPORT_DDS
  TEST_CASE("get_transport_type returns kDds for dds url") {
    Url url("dds://namespace/topic");

    CHECK_EQ(url.get_transport_type(), TransportType::kDds);
  }
#endif

#if !defined(__ANDROID__)
#ifdef VLINK_SUPPORT_SHM
  TEST_CASE("get_transport_type returns kShm for shm url") {
    Url url("shm://namespace/topic");

    CHECK_EQ(url.get_transport_type(), TransportType::kShm);
  }
#else
  TEST_CASE("shm url throws when shm transport is not linked") { CHECK_THROWS((void)Url("shm://namespace/topic")); }
#endif

#ifdef VLINK_SUPPORT_SHM2
  TEST_CASE("get_transport_type returns kShm2 for shm2 url") {
    Url url("shm2://namespace/topic");

    CHECK_EQ(url.get_transport_type(), TransportType::kShm2);
  }
#else
  TEST_CASE("shm2 url throws when shm2 transport is not linked") { CHECK_THROWS((void)Url("shm2://namespace/topic")); }
#endif
#endif

#ifdef VLINK_SUPPORT_ZENOH
  TEST_CASE("get_transport_type returns kZenoh for zenoh url") {
    Url url("zenoh://namespace/topic");

    CHECK_EQ(url.get_transport_type(), TransportType::kZenoh);
  }
#else
  TEST_CASE("zenoh url throws when zenoh transport is not linked") {
    CHECK_THROWS((void)Url("zenoh://namespace/topic"));
  }
#endif

#ifdef VLINK_SUPPORT_DDSC
  TEST_CASE("get_transport_type returns kDdsc for ddsc url") {
    Url url("ddsc://namespace/topic");

    CHECK_EQ(url.get_transport_type(), TransportType::kDdsc);
  }
#else
  TEST_CASE("ddsc url throws when ddsc transport is not linked") { CHECK_THROWS((void)Url("ddsc://namespace/topic")); }
#endif

#ifdef VLINK_SUPPORT_DDSR
  TEST_CASE("get_transport_type returns kDdsr for ddsr url") {
    Url url("ddsr://namespace/topic");

    CHECK_EQ(url.get_transport_type(), TransportType::kDdsr);
  }
#else
  TEST_CASE("ddsr url throws when ddsr transport is not linked") { CHECK_THROWS((void)Url("ddsr://namespace/topic")); }
#endif

#ifdef VLINK_SUPPORT_SOMEIP
  TEST_CASE("get_transport_type returns kSomeip for someip url") {
    Url url("someip://namespace/topic");

    CHECK_EQ(url.get_transport_type(), TransportType::kSomeip);
  }
#else
  TEST_CASE("someip url throws when someip transport is not linked") {
    CHECK_THROWS((void)Url("someip://namespace/topic"));
  }
#endif

#ifdef VLINK_SUPPORT_MQTT
  TEST_CASE("get_transport_type returns kMqtt for mqtt url") {
    Url url("mqtt://namespace/topic");

    CHECK_EQ(url.get_transport_type(), TransportType::kMqtt);
  }
#else
  TEST_CASE("mqtt url throws when mqtt transport is not linked") { CHECK_THROWS((void)Url("mqtt://namespace/topic")); }
#endif

#ifdef VLINK_SUPPORT_FDBUS
  TEST_CASE("get_transport_type returns kFdbus for fdbus url") {
    Url url("fdbus://namespace/topic");

    CHECK_EQ(url.get_transport_type(), TransportType::kFdbus);
  }
#else
  TEST_CASE("fdbus url throws when fdbus transport is not linked") {
    CHECK_THROWS((void)Url("fdbus://namespace/topic"));
  }
#endif

  TEST_CASE("is_local_type identifies intra shm and shm2 as local") {
    CHECK(Url::is_local_type("intra://topic"));
    CHECK(Url::is_local_type("shm://topic"));
    CHECK(Url::is_local_type("shm2://topic"));
  }

  TEST_CASE("is_local_type identifies network transports as not local") {
    CHECK_FALSE(Url::is_local_type("dds://topic"));
    CHECK_FALSE(Url::is_local_type("zenoh://topic"));
    CHECK_FALSE(Url::is_local_type("ddsc://topic"));
    CHECK_FALSE(Url::is_local_type("someip://topic"));
    CHECK_FALSE(Url::is_local_type("fdbus://topic"));
    CHECK_FALSE(Url::is_local_type("mqtt://topic"));
  }

  TEST_CASE("is_local_type returns false for empty string") { CHECK_FALSE(Url::is_local_type("")); }

  TEST_CASE("is_intra_type returns true only for intra url") {
    CHECK(Url::is_intra_type("intra://topic"));
    CHECK_FALSE(Url::is_intra_type("shm://topic"));
    CHECK_FALSE(Url::is_intra_type("shm2://topic"));
    CHECK_FALSE(Url::is_intra_type("dds://topic"));
    CHECK_FALSE(Url::is_intra_type(""));
  }

  TEST_CASE("is_shm_type returns true for shm and shm2 only") {
    CHECK(Url::is_shm_type("shm://topic"));
    CHECK(Url::is_shm_type("shm2://topic"));
    CHECK_FALSE(Url::is_shm_type("intra://topic"));
    CHECK_FALSE(Url::is_shm_type("dds://topic"));
    CHECK_FALSE(Url::is_shm_type("someip://topic"));
    CHECK_FALSE(Url::is_shm_type(""));
  }

  TEST_CASE("get_sort_index returns -1 for empty string") { CHECK_EQ(Url::get_sort_index(""), -1); }

  TEST_CASE("get_sort_index returns non-negative for all known transports") {
    CHECK_GE(Url::get_sort_index("intra://test"), 0);
    CHECK_GE(Url::get_sort_index("dds://test"), 0);
    CHECK_GE(Url::get_sort_index("zenoh://test"), 0);
    CHECK_GE(Url::get_sort_index("someip://test"), 0);
    CHECK_GE(Url::get_sort_index("ddsc://test"), 0);
    CHECK_GE(Url::get_sort_index("fdbus://test"), 0);
  }

  TEST_CASE("get_sort_index assigns lower index to local transports than network transports") {
    CHECK_LT(Url::get_sort_index("intra://topic"), Url::get_sort_index("dds://topic"));
    CHECK_LT(Url::get_sort_index("shm://topic"), Url::get_sort_index("zenoh://topic"));
    CHECK_LT(Url::get_sort_index("shm2://topic"), Url::get_sort_index("dds://topic"));
  }

  TEST_CASE("get_sort_index maps each scheme to its transport index") {
    CHECK_EQ(Url::get_sort_index("intra://t"), static_cast<int>(TransportType::kIntra));
    CHECK_EQ(Url::get_sort_index("shm://t"), static_cast<int>(TransportType::kShm));
    CHECK_EQ(Url::get_sort_index("shm2://t"), static_cast<int>(TransportType::kShm2));
    CHECK_EQ(Url::get_sort_index("zenoh://t"), static_cast<int>(TransportType::kZenoh));
    CHECK_EQ(Url::get_sort_index("dds://t"), static_cast<int>(TransportType::kDds));
    CHECK_EQ(Url::get_sort_index("ddsf://t"), static_cast<int>(TransportType::kDds));
    CHECK_EQ(Url::get_sort_index("ddsc://t"), static_cast<int>(TransportType::kDdsc));
    CHECK_EQ(Url::get_sort_index("ddsr://t"), static_cast<int>(TransportType::kDdsr));
    CHECK_EQ(Url::get_sort_index("someip://t"), static_cast<int>(TransportType::kSomeip));
    CHECK_EQ(Url::get_sort_index("mqtt://t"), static_cast<int>(TransportType::kMqtt));
    CHECK_EQ(Url::get_sort_index("fdbus://t"), static_cast<int>(TransportType::kFdbus));
  }

  TEST_CASE("get_sort_index returns zero for an unrecognised scheme") {
    CHECK_EQ(Url::get_sort_index("unknown://t"), 0);
    CHECK_EQ(Url::get_sort_index("http://t"), 0);
  }

  TEST_CASE("get_transport_enable_flags returns non-zero when at least one transport is compiled in") {
    uint16_t flags = Url::get_transport_enable_flags();
    CHECK_NE(flags, 0);
  }

#ifdef VLINK_SUPPORT_INTRA
  TEST_CASE("get_transport_enable_flags has kEnableIntra bit set when intra is compiled in") {
    uint16_t flags = Url::get_transport_enable_flags();

    CHECK_NE((flags & Url::kEnableIntra), 0);
  }
#endif

  TEST_CASE("kEnableEmpty is zero") { CHECK_EQ(Url::kEnableEmpty, 0); }

  TEST_CASE("kEnableAll equals 0xFFFF") { CHECK_EQ(Url::kEnableAll, static_cast<uint16_t>(0xFFFF)); }

  TEST_CASE("kEnableAll includes every individual transport flag") {
    CHECK_NE((Url::kEnableAll & Url::kEnableIntra), 0);
    CHECK_NE((Url::kEnableAll & Url::kEnableShm), 0);
    CHECK_NE((Url::kEnableAll & Url::kEnableShm2), 0);
    CHECK_NE((Url::kEnableAll & Url::kEnableZenoh), 0);
    CHECK_NE((Url::kEnableAll & Url::kEnableDds), 0);
    CHECK_NE((Url::kEnableAll & Url::kEnableDdsc), 0);
    CHECK_NE((Url::kEnableAll & Url::kEnableDdsr), 0);
    CHECK_NE((Url::kEnableAll & Url::kEnableSomeip), 0);
    CHECK_NE((Url::kEnableAll & Url::kEnableMqtt), 0);
    CHECK_NE((Url::kEnableAll & Url::kEnableFdbus), 0);
  }

  TEST_CASE("individual TransportEnableFlag values are pairwise distinct") {
    CHECK_NE(Url::kEnableIntra, Url::kEnableShm);
    CHECK_NE(Url::kEnableShm, Url::kEnableShm2);
    CHECK_NE(Url::kEnableZenoh, Url::kEnableDds);
    CHECK_NE(Url::kEnableDds, Url::kEnableDdsc);
    CHECK_NE(Url::kEnableMqtt, Url::kEnableFdbus);
  }

  TEST_CASE("bitwise OR of individual flags selects only those transports") {
    uint16_t combined = Url::kEnableIntra | Url::kEnableDds;

    CHECK_NE((combined & Url::kEnableIntra), 0);
    CHECK_NE((combined & Url::kEnableDds), 0);
    CHECK_EQ((combined & Url::kEnableShm), 0);
    CHECK_EQ((combined & Url::kEnableZenoh), 0);
  }
}

// NOLINTEND
