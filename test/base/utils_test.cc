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

#include "./base/utils.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "../common_test.h"

TEST_SUITE("base-Utils") {
  TEST_CASE("get_app_path returns a non-empty string") {
    std::string path = Utils::get_app_path();

    CHECK_FALSE(path.empty());
  }

  TEST_CASE("get_app_dir returns a non-empty string") {
    std::string dir = Utils::get_app_dir();

    CHECK_FALSE(dir.empty());
  }

  TEST_CASE("get_app_name returns a non-empty string") {
    std::string name = Utils::get_app_name();

    CHECK_FALSE(name.empty());
  }

  TEST_CASE("get_app_dir is a prefix of get_app_path") {
    std::string path = Utils::get_app_path();
    std::string dir = Utils::get_app_dir();

    CHECK(path.find(dir) == 0U);
  }

  TEST_CASE("get_app_path contains get_app_name") {
    std::string path = Utils::get_app_path();
    std::string name = Utils::get_app_name();

    if (!path.empty() && !name.empty()) {
      CHECK(path.find(name) != std::string::npos);
    }
  }

  TEST_CASE("get_app_name is consistent across calls") {
    std::string n1 = Utils::get_app_name();
    std::string n2 = Utils::get_app_name();

    CHECK(n1 == n2);
  }

  TEST_CASE("get_pid returns a positive value") {
    int32_t pid = Utils::get_pid();

    CHECK(pid > 0);
  }

  TEST_CASE("get_pid_str is a non-empty numeric string") {
    std::string pid_str = Utils::get_pid_str();

    CHECK_FALSE(pid_str.empty());

    for (char c : pid_str) {
      CHECK(c >= '0');
      CHECK(c <= '9');
    }
  }

  TEST_CASE("get_pid_str matches get_pid numerically") {
    int32_t pid = Utils::get_pid();
    std::string pid_str = Utils::get_pid_str();

    CHECK(std::to_string(pid) == pid_str);
  }

  TEST_CASE("multiple calls to get_pid return the same value") {
    int32_t pid1 = Utils::get_pid();
    int32_t pid2 = Utils::get_pid();

    CHECK(pid1 == pid2);
  }

  TEST_CASE("get_host_name returns a non-empty string") {
    std::string host = Utils::get_host_name();

    CHECK_FALSE(host.empty());
  }

  TEST_CASE("get_host_name is consistent across calls") {
    std::string h1 = Utils::get_host_name();
    std::string h2 = Utils::get_host_name();

    CHECK(h1 == h2);
  }

  TEST_CASE("get_tmp_dir returns a non-empty string") {
    std::string tmp = Utils::get_tmp_dir();

    CHECK_FALSE(tmp.empty());
  }

  TEST_CASE("get_tmp_dir points to an existing directory") {
    std::string tmp = Utils::get_tmp_dir();

    if (!tmp.empty()) {
      CHECK(std::filesystem::exists(tmp));
    }
  }

  TEST_CASE("get_env with missing key returns provided default value") {
    std::string val = Utils::get_env("VLINK_NONEXISTENT_KEY_12345", "fallback");

    CHECK(val == "fallback");
  }

  TEST_CASE("get_env with missing key returns empty string when no default given") {
    std::string val = Utils::get_env("VLINK_NONEXISTENT_KEY_67890");

    CHECK(val.empty());
  }

  TEST_CASE("set_env and get_env round-trip") {
    bool ok = Utils::set_env("VLINK_TEST_ROUNDTRIP", "test_value");
    CHECK(ok);

    std::string val = Utils::get_env("VLINK_TEST_ROUNDTRIP");
    CHECK(val == "test_value");

    Utils::unset_env("VLINK_TEST_ROUNDTRIP");
  }

  TEST_CASE("unset_env removes a previously set variable") {
    Utils::set_env("VLINK_TEST_UNSET", "to_be_removed");
    CHECK(Utils::get_env("VLINK_TEST_UNSET") == "to_be_removed");

    bool ok = Utils::unset_env("VLINK_TEST_UNSET");
    CHECK(ok);

    std::string val = Utils::get_env("VLINK_TEST_UNSET", "gone");
    CHECK(val == "gone");
  }

  TEST_CASE("set_env with force true overwrites existing value") {
    Utils::set_env("VLINK_TEST_FORCE", "first", true);
    Utils::set_env("VLINK_TEST_FORCE", "second", true);

    std::string val = Utils::get_env("VLINK_TEST_FORCE");
    CHECK(val == "second");

    Utils::unset_env("VLINK_TEST_FORCE");
  }

  TEST_CASE("unset_env on a nonexistent key does not crash") {
    bool ok = Utils::unset_env("VLINK_NONEXISTENT_KEY_XYZ_UNSET");
    (void)ok;
  }

  TEST_CASE("get_all_ipv4_address returns dotted-decimal strings") {
    std::vector<std::string> addrs = Utils::get_all_ipv4_address(false);

    for (const auto& addr : addrs) {
      CHECK_FALSE(addr.empty());
      CHECK(addr.find('.') != std::string::npos);
    }
  }

  TEST_CASE("get_all_ipv4_address filtered is a subset of unfiltered") {
    std::vector<std::string> all = Utils::get_all_ipv4_address(false);
    std::vector<std::string> avail = Utils::get_all_ipv4_address(true);

    CHECK(avail.size() <= all.size());
  }

  TEST_CASE("get_all_ipv6_address returns strings containing colons") {
    std::vector<std::string> addrs = Utils::get_all_ipv6_address(false);

    for (const auto& addr : addrs) {
      CHECK(addr.find(':') != std::string::npos);
    }
  }

  TEST_CASE("get_all_ipv6_address filtered is a subset of unfiltered") {
    std::vector<std::string> all = Utils::get_all_ipv6_address(false);
    std::vector<std::string> avail = Utils::get_all_ipv6_address(true);

    CHECK(avail.size() <= all.size());
  }

  TEST_CASE("get_interface_name_by_ipv4 with loopback does not crash") {
    std::string iface = Utils::get_interface_name_by_ipv4("127.0.0.1");
    (void)iface;
  }

  TEST_CASE("get_interface_name_by_ipv4 with bogus address returns empty or name") {
    std::string iface = Utils::get_interface_name_by_ipv4("192.0.2.255");
    (void)iface;
  }

  TEST_CASE("get_interface_name_by_ipv6 with loopback does not crash") {
    std::string iface = Utils::get_interface_name_by_ipv6("::1");
    (void)iface;
  }

  TEST_CASE("get_dds_default_address respects max_count") {
    static constexpr int kMax = 3;
    std::vector<std::string> addrs = Utils::get_dds_default_address(false, kMax);

    CHECK(addrs.size() <= static_cast<size_t>(kMax));
  }

  TEST_CASE("get_dds_default_address entries are non-empty dotted-decimal") {
    std::vector<std::string> addrs = Utils::get_dds_default_address();

    for (const auto& a : addrs) {
      CHECK_FALSE(a.empty());
      CHECK(a.find('.') != std::string::npos);
    }
  }

  TEST_CASE("get_dds_default_address with filter available does not crash") {
    auto addrs = Utils::get_dds_default_address(true, 10);

    for (const auto& a : addrs) {
      CHECK_FALSE(a.empty());
      CHECK(a.find('.') != std::string::npos);
    }
  }

  TEST_CASE("get_native_thread_id returns a value greater than zero") {
    uint64_t tid = Utils::get_native_thread_id();

    CHECK(tid > 0);
  }

  TEST_CASE("get_native_thread_id is consistent on the same thread") {
    uint64_t t1 = Utils::get_native_thread_id();
    uint64_t t2 = Utils::get_native_thread_id();

    CHECK(t1 == t2);
  }

  TEST_CASE("get_native_thread_id differs between threads") {
    uint64_t main_tid = Utils::get_native_thread_id();
    uint64_t other_tid = 0;

    std::thread t([&other_tid] { other_tid = Utils::get_native_thread_id(); });
    t.join();

    CHECK(main_tid != other_tid);
  }

  TEST_CASE("yield_cpu can be called repeatedly without crashing") {
    for (int i = 0; i < 100; ++i) {
      Utils::yield_cpu();
    }
  }

  TEST_CASE("set_console_utf8_output does not crash") { Utils::set_console_utf8_output(); }

  TEST_CASE("get_terminal_size does not crash") {
    auto [cols, rows] = Utils::get_terminal_size();
    (void)cols;
    (void)rows;
  }

  TEST_CASE("set_thread_name on the calling thread does not crash") {
    bool ok = Utils::set_thread_name("test_worker");
    (void)ok;
  }

  TEST_CASE("set_thread_name with empty string does not crash") {
    bool ok = Utils::set_thread_name("");
    (void)ok;
  }

  TEST_CASE("set_thread_name with long string does not crash") {
    std::string long_name(64, 'a');
    bool ok = Utils::set_thread_name(long_name);
    (void)ok;
  }

  TEST_CASE("set_thread_name with explicit thread object does not crash") {
    bool done = false;
    std::thread t([&done]() { done = true; });

    Utils::set_thread_name("named_thr", &t);
    t.join();

    CHECK(done);
  }

  TEST_CASE("set_thread_priority with sched_other policy does not crash") {
    bool ok = Utils::set_thread_priority(0, 0);
    (void)ok;
  }

  TEST_CASE("set_thread_priority with default policy does not crash") {
    bool ok = Utils::set_thread_priority(0);
    (void)ok;
  }

  TEST_CASE("set_thread_stick with zero mask returns false") {
    bool ok = Utils::set_thread_stick(0);

    CHECK_FALSE(ok);
  }

  TEST_CASE("set_thread_stick with core zero mask does not crash") {
    bool ok = Utils::set_thread_stick(0x1);
    (void)ok;
  }

  TEST_CASE("is_process_running returns false for nonexistent process") {
    bool running = Utils::is_process_running("__vlink_fake_process_xyz__");

    CHECK_FALSE(running);
  }

  TEST_CASE("is_process_running returns false for name with dot as literal") {
    bool running = Utils::is_process_running("__vlink_fake_process_._xyz__");

    CHECK_FALSE(running);
  }

  TEST_CASE("is_process_running does not execute shell metacharacters") {
    const auto marker = std::filesystem::temp_directory_path() / "vlink_is_process_shell_injection_marker";
    std::filesystem::remove(marker);

    const std::string payload = "__vlink_fake_xyz__; touch " + marker.string();
    CHECK_FALSE(Utils::is_process_running(payload));
    CHECK_FALSE(std::filesystem::exists(marker));
  }

  TEST_CASE("is_process_running for the current executable does not crash") {
    std::string app_name = Utils::get_app_name();

    if (!app_name.empty()) {
      CHECK_NOTHROW((void)Utils::is_process_running(app_name));
    }
  }

  TEST_CASE("get_timezone_diff is within plausible range") {
    int32_t diff = Utils::get_timezone_diff();

    CHECK(diff >= -720);
    CHECK(diff <= 840);
  }

  TEST_CASE("get_timezone_diff is consistent across calls") {
    int32_t d1 = Utils::get_timezone_diff();
    int32_t d2 = Utils::get_timezone_diff();

    CHECK(d1 == d2);
  }

  TEST_CASE("get_machine_id is consistent across calls") {
    std::string id1 = Utils::get_machine_id();
    std::string id2 = Utils::get_machine_id();

    CHECK(id1 == id2);
  }

  TEST_CASE("get_cpu_usage returns a non-negative value") {
    double cpu = Utils::get_cpu_usage();

    CHECK(cpu >= 0.0);
  }

  TEST_CASE("get_cpu_usage can be called multiple times without crashing") {
    double cpu1 = Utils::get_cpu_usage();
    double cpu2 = Utils::get_cpu_usage();
    (void)cpu1;
    (void)cpu2;
  }

  TEST_CASE("get_memory_usage returns a non-negative value") {
    double mem = Utils::get_memory_usage();

    CHECK(mem >= 0.0);
  }

  TEST_CASE("get_memory_usage is below 100 percent under normal conditions") {
    double mem = Utils::get_memory_usage();

    CHECK(mem < 100.0);
  }

  TEST_CASE("try_release_sys_memory does not crash") { Utils::try_release_sys_memory(); }

  TEST_CASE("wait_for_device returns true for a path that already exists") {
    bool ok = Utils::wait_for_device(Utils::get_tmp_dir(), 200, 20);

    CHECK(ok);
  }

  TEST_CASE("wait_for_device returns false when path never appears") {
    bool ok = Utils::wait_for_device("/__vlink_nonexistent_device__", 100, 20);

    CHECK_FALSE(ok);
  }
}

// NOLINTEND
