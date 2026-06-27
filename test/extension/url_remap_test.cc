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

#include "./extension/url_remap.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#include "../common_test.h"
#include "./base/utils.h"

namespace {

class ScopedRemapFile {
 public:
  explicit ScopedRemapFile(const std::string& content) {
    static std::uint32_t sequence = 0;
    path = std::filesystem::path(vlink::Utils::get_tmp_dir()) /
           ("vlink_url_remap_test_" + vlink::Utils::get_pid_str() + "_" + std::to_string(sequence++) + ".json");
    std::ofstream f(path, std::ios::trunc);
    f << content;
  }

  ScopedRemapFile(const ScopedRemapFile&) = delete;
  ScopedRemapFile& operator=(const ScopedRemapFile&) = delete;

  ScopedRemapFile(ScopedRemapFile&& other) noexcept : path(std::move(other.path)) { other.path.clear(); }

  ScopedRemapFile& operator=(ScopedRemapFile&& other) noexcept {
    if (this != &other) {
      remove();
      path = std::move(other.path);
      other.path.clear();
    }
    return *this;
  }

  ~ScopedRemapFile() { remove(); }

  std::string string() const { return path.string(); }

 private:
  void remove() {
    if (!path.empty()) {
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }
  }

  std::filesystem::path path;
};

ScopedRemapFile write_temp_remap(const std::string& content) { return ScopedRemapFile(content); }

}  // namespace

TEST_SUITE("extension-UrlRemap") {
  TEST_CASE("default construction yields invalid empty state") {
    UrlRemap remap;
    CHECK_FALSE(remap.is_valid());
    CHECK(remap.get_error_string().empty());
    CHECK_FALSE(remap.is_enable_log());
  }

  TEST_CASE("load non-existent file returns false and sets error string") {
    UrlRemap remap;
    const bool ok =
        remap.load((std::filesystem::path(Utils::get_tmp_dir()) / "definitely_does_not_exist_xyz.json").string());
    CHECK_FALSE(ok);
    CHECK_FALSE(remap.is_valid());
    CHECK_FALSE(remap.get_error_string().empty());
  }

  TEST_CASE("load valid JSON returns true and marks instance valid") {
    const std::string json = R"({"intra://sensor/lidar": "dds://vehicle/lidar"})";
    const auto file = write_temp_remap(json);
    UrlRemap remap;
    CHECK(remap.load(file.string()));
    CHECK(remap.is_valid());
    CHECK(remap.get_error_string().empty());
  }

  TEST_CASE("load malformed JSON returns false and leaves instance invalid") {
    const auto file = write_temp_remap("{ not valid json !!!");
    UrlRemap remap;
    CHECK_FALSE(remap.load(file.string()));
    CHECK_FALSE(remap.is_valid());
  }

  TEST_CASE("second load without unload returns false") {
    const std::string json = R"({"intra://a": "dds://a"})";
    const auto file = write_temp_remap(json);
    UrlRemap remap;
    REQUIRE(remap.load(file.string()));
    CHECK_FALSE(remap.load(file.string()));
  }

  TEST_CASE("unload when loaded returns true and invalidates the instance") {
    const std::string json = R"({"intra://a": "dds://a"})";
    const auto file = write_temp_remap(json);
    UrlRemap remap;
    REQUIRE(remap.load(file.string()));
    CHECK(remap.unload());
    CHECK_FALSE(remap.is_valid());
  }

  TEST_CASE("unload when not loaded returns false") {
    UrlRemap remap;
    CHECK_FALSE(remap.unload());
  }

  TEST_CASE("reload atomically replaces the current configuration") {
    const std::string json1 = R"({"intra://a": "dds://a"})";
    const std::string json2 = R"({"intra://b": "dds://b"})";
    const auto file1 = write_temp_remap(json1);
    UrlRemap remap;
    REQUIRE(remap.load(file1.string()));

    const auto file2 = write_temp_remap(json2);
    CHECK(remap.reload(file2.string()));
    CHECK(remap.is_valid());
    CHECK_EQ(remap.convert("intra://b"), "dds://b");
  }

  TEST_CASE("reload with non-existent file leaves remap invalid") {
    const std::string json = R"({"intra://a": "dds://a"})";
    const auto file = write_temp_remap(json);
    UrlRemap remap;
    REQUIRE(remap.load(file.string()));
    CHECK_FALSE(
        remap.reload((std::filesystem::path(Utils::get_tmp_dir()) / "no_such_file_for_reload_xyz.json").string()));
    CHECK_FALSE(remap.is_valid());
  }

  TEST_CASE("convert returns remapped URL when a rule matches") {
    const std::string json = R"({
      "intra://sensor/lidar": "dds://vehicle/lidar",
      "shm://camera/front": "zenoh://camera/front"
    })";
    const auto file = write_temp_remap(json);
    UrlRemap remap;
    REQUIRE(remap.load(file.string()));
    CHECK_EQ(remap.convert("intra://sensor/lidar"), "dds://vehicle/lidar");
    CHECK_EQ(remap.convert("shm://camera/front"), "zenoh://camera/front");
  }

  TEST_CASE("convert returns original URL when no rule matches") {
    const std::string json = R"({"intra://sensor/lidar": "dds://vehicle/lidar"})";
    const auto file = write_temp_remap(json);
    UrlRemap remap;
    REQUIRE(remap.load(file.string()));
    const std::string url = "dds://no/match/here";
    CHECK_EQ(remap.convert(url), url);
  }

  TEST_CASE("convert returns original URL when not loaded") {
    UrlRemap remap;
    const std::string url = "intra://some/topic";
    CHECK_EQ(remap.convert(url), url);
  }

  TEST_CASE("convert caches result so repeated lookups are consistent") {
    const std::string json = R"({"intra://my/topic": "dds://my/topic"})";
    const auto file = write_temp_remap(json);
    UrlRemap remap;
    REQUIRE(remap.load(file.string()));
    const std::string r1 = remap.convert("intra://my/topic");
    const std::string r2 = remap.convert("intra://my/topic");
    CHECK_EQ(r1, r2);
    CHECK_EQ(r1, "dds://my/topic");
  }

  TEST_CASE("convert uses substring matching against the input URL") {
    const std::string json = R"({"sensor/lidar": "vehicle/lidar"})";
    const auto file = write_temp_remap(json);
    UrlRemap remap;
    REQUIRE(remap.load(file.string()));
    CHECK_EQ(remap.convert("intra://sensor/lidar"), "vehicle/lidar");
  }

  TEST_CASE("unload clears the conversion cache") {
    const std::string json = R"({"intra://a": "dds://a"})";
    const auto file = write_temp_remap(json);
    UrlRemap remap;
    REQUIRE(remap.load(file.string()));
    (void)remap.convert("intra://a");
    remap.unload();
    const std::string url = "intra://a";
    CHECK_EQ(remap.convert(url), url);
  }

  TEST_CASE("set_enable_log toggles the log flag") {
    UrlRemap remap;
    CHECK_FALSE(remap.is_enable_log());
    remap.set_enable_log(true);
    CHECK(remap.is_enable_log());
    remap.set_enable_log(false);
    CHECK_FALSE(remap.is_enable_log());
  }
}

// NOLINTEND
