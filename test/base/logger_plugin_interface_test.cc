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

#include "./base/logger_plugin_interface.h"

#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <type_traits>

#include "../common_test.h"

namespace {

class FakeLoggerPlugin final : public LoggerPluginInterface {
 public:
  FakeLoggerPlugin() = default;

  bool init(std::string_view app_name) override {
    last_app_name = std::string(app_name);
    init_called = true;
    return init_return_value;
  }

  bool log(int level, std::string_view str) override {
    last_level = level;
    last_message = std::string(str);
    log_call_count += 1;
    return log_return_value;
  }

  std::string last_app_name;
  std::string last_message;
  int last_level{0};
  int log_call_count{0};
  bool init_called{false};
  bool init_return_value{true};
  bool log_return_value{true};
};

}  // namespace

TEST_SUITE("base-LoggerPluginInterface") {
  TEST_CASE("interface is abstract and not directly instantiable") { CHECK(std::is_abstract_v<LoggerPluginInterface>); }

  TEST_CASE("interface is not copyable") {
    CHECK_FALSE(std::is_copy_constructible_v<LoggerPluginInterface>);
    CHECK_FALSE(std::is_copy_assignable_v<LoggerPluginInterface>);
  }

  TEST_CASE("concrete subclass is default constructible") { CHECK(std::is_default_constructible_v<FakeLoggerPlugin>); }

  TEST_CASE("concrete subclass derives from interface") {
    CHECK((std::is_base_of_v<LoggerPluginInterface, FakeLoggerPlugin>));
  }

  TEST_CASE("init forwards app name and returns success") {
    FakeLoggerPlugin plugin;
    plugin.init_return_value = true;

    bool ok = plugin.init("my_app");

    CHECK(ok);
    CHECK(plugin.init_called);
    CHECK_EQ(plugin.last_app_name, "my_app");
  }

  TEST_CASE("init can return false to signal failure") {
    FakeLoggerPlugin plugin;
    plugin.init_return_value = false;

    bool ok = plugin.init("any");

    CHECK_FALSE(ok);
    CHECK(plugin.init_called);
  }

  TEST_CASE("log records level and message on each call") {
    FakeLoggerPlugin plugin;

    bool ok1 = plugin.log(static_cast<int>(Logger::Level::kInfo), "hello");
    bool ok2 = plugin.log(static_cast<int>(Logger::Level::kWarn), "world");

    CHECK(ok1);
    CHECK(ok2);
    CHECK_EQ(plugin.log_call_count, 2);
    CHECK_EQ(plugin.last_level, static_cast<int>(Logger::Level::kWarn));
    CHECK_EQ(plugin.last_message, "world");
  }

  TEST_CASE("log accepts empty payload") {
    FakeLoggerPlugin plugin;

    bool ok = plugin.log(0, std::string_view{});

    CHECK(ok);
    CHECK(plugin.last_message.empty());
  }

  TEST_CASE("virtual dispatch via base pointer works correctly") {
    FakeLoggerPlugin concrete;
    LoggerPluginInterface* base = &concrete;

    CHECK(base->init("polymorphic"));
    CHECK(base->log(0, "polymorphic"));
    CHECK(concrete.init_called);
    CHECK_EQ(concrete.last_message, "polymorphic");
  }
}

// NOLINTEND
