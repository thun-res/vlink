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

#include "./extension/runnable_plugin_interface.h"

#include <doctest/doctest.h>

#include <atomic>
#include <type_traits>

#include "../common_test.h"

namespace {

class FakeRunnablePlugin final : public RunablePluginInterface {
 public:
  FakeRunnablePlugin() = default;

  void on_init() override { init_count.fetch_add(1, std::memory_order_relaxed); }

  void on_deinit() override { deinit_count.fetch_add(1, std::memory_order_relaxed); }

  std::atomic<int> init_count{0};
  std::atomic<int> deinit_count{0};
};

}  // namespace

TEST_SUITE("extension-RunablePluginInterface") {
  TEST_CASE("interface is abstract") { CHECK(std::is_abstract_v<RunablePluginInterface>); }

  TEST_CASE("interface inherits from MessageLoop") { CHECK(std::is_base_of_v<MessageLoop, RunablePluginInterface>); }

  TEST_CASE("interface is not copy-constructible") {
    CHECK_FALSE(std::is_copy_constructible_v<RunablePluginInterface>);
  }

  TEST_CASE("interface is not copy-assignable") { CHECK_FALSE(std::is_copy_assignable_v<RunablePluginInterface>); }

  TEST_CASE("on_init is called and increments counter") {
    FakeRunnablePlugin plugin;
    CHECK_EQ(plugin.init_count.load(), 0);
    plugin.on_init();
    CHECK_EQ(plugin.init_count.load(), 1);
  }

  TEST_CASE("on_deinit is called and increments counter") {
    FakeRunnablePlugin plugin;
    CHECK_EQ(plugin.deinit_count.load(), 0);
    plugin.on_deinit();
    CHECK_EQ(plugin.deinit_count.load(), 1);
  }

  TEST_CASE("on_init and on_deinit track independent call counts") {
    FakeRunnablePlugin plugin;
    plugin.on_init();
    plugin.on_init();
    plugin.on_deinit();
    CHECK_EQ(plugin.init_count.load(), 2);
    CHECK_EQ(plugin.deinit_count.load(), 1);
  }

  TEST_CASE("virtual dispatch through base pointer reaches concrete implementation") {
    FakeRunnablePlugin concrete;
    RunablePluginInterface* base = &concrete;
    base->on_init();
    base->on_deinit();
    CHECK_EQ(concrete.init_count.load(), 1);
    CHECK_EQ(concrete.deinit_count.load(), 1);
  }
}

// NOLINTEND
