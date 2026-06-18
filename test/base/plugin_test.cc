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

#include "./base/plugin.h"

#include <doctest/doctest.h>

#include <string>

#include "../common_test.h"

class MyTestInterface {
  VLINK_PLUGIN_REGISTER_BY_ID(MyTestInterface, "test.my_interface")

 public:
  virtual ~MyTestInterface() = default;

  virtual int compute(int x) = 0;
};

class AnotherInterface {
  VLINK_PLUGIN_REGISTER_BY_ID(AnotherInterface, "test.another_interface")

 public:
  virtual ~AnotherInterface() = default;

  virtual void run() = 0;
};

TEST_SUITE("base-Plugin") {
  TEST_CASE("plugin can be constructed and destroyed without crash") {
    Plugin plugin;
    (void)plugin;
  }

  TEST_CASE("set_log_level and get_log_level are consistent") {
    Plugin plugin;
    plugin.set_log_level(Logger::Level::kWarn);
    CHECK_EQ(plugin.get_log_level(), Logger::Level::kWarn);

    plugin.set_log_level(Logger::Level::kDebug);
    CHECK_EQ(plugin.get_log_level(), Logger::Level::kDebug);
  }

  TEST_CASE("clear on an empty plugin does not crash") {
    Plugin plugin;
    plugin.clear();
    CHECK_FALSE(plugin.has_loaded<MyTestInterface>("anything"));
  }

  TEST_CASE("default_search_path returns a non-empty deque") {
    auto paths = Plugin::default_search_path();
    CHECK_FALSE(paths.empty());
  }

  TEST_CASE("all entries in default_search_path are non-empty strings") {
    auto paths = Plugin::default_search_path();
    for (const auto& p : paths) {
      CHECK_FALSE(p.empty());
    }
  }

  TEST_CASE("get_plugin_complex_id format is lib_name at plugin_id") {
    Plugin plugin;
    std::string id = plugin.get_plugin_complex_id<MyTestInterface>("my_lib");
    CHECK_EQ(id, "my_lib@test.my_interface");
  }

  TEST_CASE("get_plugin_complex_id contains lib name") {
    Plugin plugin;
    std::string id = plugin.get_plugin_complex_id<MyTestInterface>("my_lib");
    CHECK(id.find("my_lib") != std::string::npos);
  }

  TEST_CASE("get_plugin_complex_id contains interface id") {
    Plugin plugin;
    std::string id = plugin.get_plugin_complex_id<MyTestInterface>("my_lib");
    CHECK(id.find("test.my_interface") != std::string::npos);
  }

  TEST_CASE("different interface types produce different complex ids") {
    Plugin plugin;
    std::string id1 = plugin.get_plugin_complex_id<MyTestInterface>("lib");
    std::string id2 = plugin.get_plugin_complex_id<AnotherInterface>("lib");
    CHECK_NE(id1, id2);
  }

  TEST_CASE("has_loaded returns false for unloaded library") {
    Plugin plugin;
    CHECK_FALSE(plugin.has_loaded<MyTestInterface>("nonexistent_lib"));
  }

  TEST_CASE("unload returns false for library that was never loaded") {
    Plugin plugin;
    bool result = plugin.unload<MyTestInterface>("nonexistent_lib");
    CHECK_FALSE(result);
  }

  TEST_CASE("load returns nullptr when library does not exist") {
    Plugin plugin;
    auto result = plugin.load<MyTestInterface>("__definitely_does_not_exist__", 1, 0);
    CHECK_EQ(result, nullptr);
  }

  TEST_CASE("has_loaded returns false after failed load") {
    Plugin plugin;
    (void)plugin.load<MyTestInterface>("__definitely_does_not_exist__", 1, 0);
    CHECK_FALSE(plugin.has_loaded<MyTestInterface>("__definitely_does_not_exist__"));
  }

  TEST_CASE("process_plugin_internal matching id and version returns true") {
    bool ok = Plugin::process_plugin_internal("my_lib", "test.my_interface", 1, 0, "test.my_interface", 1, 0,
                                              static_cast<uint8_t>(Logger::Level::kWarn));
    CHECK(ok);
  }

  TEST_CASE("process_plugin_internal mismatched plugin id returns false") {
    bool ok = Plugin::process_plugin_internal("my_lib", "test.my_interface", 1, 0, "test.other_interface", 1, 0,
                                              static_cast<uint8_t>(Logger::Level::kWarn));
    CHECK_FALSE(ok);
  }

  TEST_CASE("process_plugin_internal mismatched major version returns false") {
    bool ok = Plugin::process_plugin_internal("my_lib", "test.my_interface", 2, 0, "test.my_interface", 1, 0,
                                              static_cast<uint8_t>(Logger::Level::kWarn));
    CHECK_FALSE(ok);
  }

  TEST_CASE("process_plugin_internal target minor newer than local minor returns false") {
    bool ok = Plugin::process_plugin_internal("my_lib", "test.my_interface", 1, 0, "test.my_interface", 1, 1,
                                              static_cast<uint8_t>(Logger::Level::kWarn));
    CHECK_FALSE(ok);
  }

  TEST_CASE("process_plugin_internal empty lib name is accepted when ids and versions match") {
    bool ok = Plugin::process_plugin_internal("", "test.my_interface", 1, 0, "test.my_interface", 1, 0,
                                              static_cast<uint8_t>(Logger::Level::kWarn));
    CHECK(ok);
  }

  TEST_CASE("VLINK_PLUGIN_REGISTER_BY_ID macro sets expected plugin id") {
    static constexpr std::string_view id = MyTestInterface::get_plugin_id();
    CHECK_EQ(id, "test.my_interface");
  }

  TEST_CASE("get_plugin_id is non-empty") {
    static constexpr std::string_view id = MyTestInterface::get_plugin_id();
    CHECK_FALSE(id.empty());
  }

  TEST_CASE("two interfaces registered by id have distinct plugin ids") {
    static constexpr std::string_view id1 = MyTestInterface::get_plugin_id();
    static constexpr std::string_view id2 = AnotherInterface::get_plugin_id();
    CHECK_NE(id1, id2);
  }
}

// NOLINTEND
