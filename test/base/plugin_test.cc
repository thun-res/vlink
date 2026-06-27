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

#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#include "../common_test.h"
#include "./base/utils.h"
#include "./impl/conf_plugin_interface.h"

namespace {

std::string plugin_library_filename(const std::string& lib_name) {
#if defined(_WIN32) || defined(_WIN64)
  return lib_name + ".dll";
#elif defined(__APPLE__)
  return "lib" + lib_name + ".dylib";
#else
  return "lib" + lib_name + ".so";
#endif
}

std::deque<std::string> plugin_runtime_search_paths() {
  const auto app_dir = std::filesystem::path(Utils::get_app_dir());
  return {app_dir.string(), (app_dir / ".." / "lib").lexically_normal().string()};
}

bool has_runtime_plugin(const std::string& lib_name, const std::deque<std::string>& paths) {
  const auto filename = plugin_library_filename(lib_name);
  for (const auto& dir : paths) {
    if (std::filesystem::exists(std::filesystem::path(dir) / filename)) {
      return true;
    }
  }

  return false;
}

class ScopedPluginTmpFile {
 public:
  ScopedPluginTmpFile(const std::string& filename, const std::string& content) {
    dir_ = std::filesystem::path(Utils::get_tmp_dir()) / "vlink-plugin-tests" /
           (Utils::get_pid_str() + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir_);
    path_ = dir_ / filename;

    std::ofstream stream(path_, std::ios::out | std::ios::trunc);
    stream << content;
  }

  ScopedPluginTmpFile(const ScopedPluginTmpFile&) = delete;
  ScopedPluginTmpFile& operator=(const ScopedPluginTmpFile&) = delete;

  ~ScopedPluginTmpFile() {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::string directory_string() const { return dir_.string(); }

 private:
  std::filesystem::path dir_;
  std::filesystem::path path_;
};

class ScopedPluginTmpDir {
 public:
  explicit ScopedPluginTmpDir(const std::string& prefix) {
    path_ = std::filesystem::path(Utils::get_tmp_dir()) / prefix /
            (Utils::get_pid_str() + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path_);
  }

  ScopedPluginTmpDir(const ScopedPluginTmpDir&) = delete;
  ScopedPluginTmpDir& operator=(const ScopedPluginTmpDir&) = delete;

  ~ScopedPluginTmpDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

class ScopedPluginEnv {
 public:
  ScopedPluginEnv(std::string key, std::string value) : key_(std::move(key)) {
    const char* old_value = std::getenv(key_.c_str());
    if (old_value != nullptr) {
      had_old_value_ = true;
      old_value_ = old_value;
    }

    Utils::set_env(key_, value, true);
  }

  ScopedPluginEnv(const ScopedPluginEnv&) = delete;
  ScopedPluginEnv& operator=(const ScopedPluginEnv&) = delete;

  ~ScopedPluginEnv() {
    if (had_old_value_) {
      Utils::set_env(key_, old_value_, true);
    } else {
      Utils::unset_env(key_);
    }
  }

 private:
  std::string key_;
  std::string old_value_;
  bool had_old_value_{false};
};

}  // namespace

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

  TEST_CASE("default_search_path prepends VLINK_PLUGIN_DIR entries in order") {
    const ScopedPluginTmpDir temp_dir("vlink-plugin-dir-tests");
    const auto& base_dir = temp_dir.path();
    const auto first_dir = base_dir / "first";
    const auto second_dir = base_dir / "second";
    const auto third_dir = base_dir / "third";
    const std::string first = first_dir.generic_string();
    const std::string second = second_dir.generic_string();
    const std::string third = third_dir.generic_string();
    std::filesystem::create_directories(first_dir);
    std::filesystem::create_directories(second_dir);
    std::filesystem::create_directories(third_dir);
    const ScopedPluginEnv env("VLINK_PLUGIN_DIR", " " + first + ", " + second + " " + third + " ");

    auto paths = Plugin::default_search_path();

    REQUIRE(paths.size() >= 3u);
    CHECK_EQ(paths[0], first);
    CHECK_EQ(paths[1], second);
    CHECK_EQ(paths[2], third);
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

  TEST_CASE("load returns nullptr for empty library name") {
    Plugin plugin;
    auto result = plugin.load<MyTestInterface>("", 1, 0, "", {});
    CHECK_EQ(result, nullptr);
  }

  TEST_CASE("load scans explicit directory name before failing") {
    Plugin plugin;
    auto result =
        plugin.load<MyTestInterface>("__definitely_does_not_exist__", 1, 0, "plugins", {vlink::Utils::get_tmp_dir()});
    CHECK_EQ(result, nullptr);
  }

  TEST_CASE("load returns nullptr when the create symbol name is missing") {
    Plugin plugin;
    auto result = plugin.load<MyTestInterface>("__definitely_does_not_exist__", 1, 0, "", {vlink::Utils::get_tmp_dir()},
                                               "missing_create");
    CHECK_EQ(result, nullptr);
  }

  TEST_CASE("load returns nullptr when an existing candidate is not a dynamic library") {
    const ScopedPluginTmpFile file(plugin_library_filename("not_a_library"), "not a shared library");

    Plugin plugin;
    auto result = plugin.load<MyTestInterface>("not_a_library", 1, 0, "", {file.directory_string()});
    CHECK_EQ(result, nullptr);
  }

  TEST_CASE("load returns nullptr when a real library lacks the requested create symbol") {
    const auto lib_dir = std::filesystem::path(Utils::get_app_dir()) / ".." / "lib";
    const auto vlink_lib = lib_dir / plugin_library_filename("vlink");
    if (!std::filesystem::exists(vlink_lib)) {
      return;
    }

    Plugin plugin;
    auto result = plugin.load<MyTestInterface>("vlink", 1, 0, "", {lib_dir.string()});
    CHECK_EQ(result, nullptr);
  }

#ifdef VLINK_SUPPORT_INTRA
  TEST_CASE("load unload and destroy succeed for an installed conf plugin") {
    const auto search_paths = plugin_runtime_search_paths();
    if (!has_runtime_plugin("vlink-intra", search_paths)) {
      return;
    }

    Plugin plugin;
    plugin.set_log_level(Logger::Level::kWarn);

    auto first = plugin.load<ConfPluginInterface>("vlink-intra", 1, 0, "", search_paths);
    REQUIRE(first != nullptr);
    CHECK(plugin.has_loaded<ConfPluginInterface>("vlink-intra"));
    CHECK_EQ(first->get_transport_type(), TransportType::kIntra);

    auto duplicate = plugin.load<ConfPluginInterface>("vlink-intra", 1, 0, "", search_paths);
    CHECK_EQ(duplicate, nullptr);

    auto conf = first->create();
    REQUIRE(conf != nullptr);
    CHECK_EQ(conf->get_transport_type(), TransportType::kIntra);

    CHECK(plugin.unload<ConfPluginInterface>("vlink-intra"));
    CHECK_FALSE(plugin.has_loaded<ConfPluginInterface>("vlink-intra"));

    first.reset();
  }

  TEST_CASE("load returns nullptr when the plugin factory rejects the requested interface") {
    const auto search_paths = plugin_runtime_search_paths();
    if (!has_runtime_plugin("vlink-intra", search_paths)) {
      return;
    }

    Plugin plugin;
    auto wrong_interface = plugin.load<MyTestInterface>("vlink-intra", 1, 0, "", search_paths);
    CHECK_EQ(wrong_interface, nullptr);
    CHECK_FALSE(plugin.has_loaded<MyTestInterface>("vlink-intra"));
  }
#endif

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

  TEST_CASE("process_plugin_internal empty target id returns false") {
    bool ok = Plugin::process_plugin_internal("my_lib", "test.my_interface", 1, 0, "", 1, 0,
                                              static_cast<uint8_t>(Logger::Level::kWarn));
    CHECK_FALSE(ok);
  }

  TEST_CASE("process_plugin_internal accepts older target minor") {
    bool ok = Plugin::process_plugin_internal("my_lib", "test.my_interface", 1, 2, "test.my_interface", 1, 1,
                                              static_cast<uint8_t>(Logger::Level::kWarn));
    CHECK(ok);
  }

  TEST_CASE("process_plugin_internal info log path still returns true") {
    bool ok = Plugin::process_plugin_internal("my_lib", "test.my_interface", 1, 0, "test.my_interface", 1, 0,
                                              static_cast<uint8_t>(Logger::Level::kInfo));
    CHECK(ok);
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
