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

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "../common_test.h"
#include "./base/process.h"

#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

class ScopedUtilsTempPath {
 public:
  ScopedUtilsTempPath(const std::string& name, bool create_directory = false)
      : path_(std::filesystem::path(Utils::get_tmp_dir()) / name) {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
    if (create_directory) {
      std::filesystem::create_directories(path_, ec);
    }
  }

  ScopedUtilsTempPath(const ScopedUtilsTempPath&) = delete;
  ScopedUtilsTempPath& operator=(const ScopedUtilsTempPath&) = delete;

  ~ScopedUtilsTempPath() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::filesystem::path& path() const { return path_; }
  std::string string() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

class ScopedUtilsEnv {
 public:
  ScopedUtilsEnv(std::string key, std::string value) : key_(std::move(key)), old_value_(Utils::get_env(key_)) {
    Utils::set_env(key_, value, true);
  }

  ScopedUtilsEnv(const ScopedUtilsEnv&) = delete;
  ScopedUtilsEnv& operator=(const ScopedUtilsEnv&) = delete;

  ~ScopedUtilsEnv() {
    if (old_value_.empty()) {
      Utils::unset_env(key_);
    } else {
      Utils::set_env(key_, old_value_, true);
    }
  }

 private:
  std::string key_;
  std::string old_value_;
};

#if defined(__linux__) || defined(__APPLE__)
bool write_all_to_fd(int fd, const char* data, size_t size) {
  size_t written = 0;
  while (written < size) {
    const auto ret = ::write(fd, data + written, size - written);
    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (ret == 0) {
      return false;
    }
    written += static_cast<size_t>(ret);
  }
  return true;
}
#endif

}  // namespace

TEST_SUITE("base-Utils") {
  TEST_CASE("child process covers environment driven singleton helpers") {
    const auto child_case = Utils::get_env("VLINK_UTILS_CHILD_CASE");

    if (child_case == "tmp-dir-env") {
      CHECK_EQ(Utils::get_tmp_dir(), Utils::get_env("VLINK_TMP_DIR"));
      return;
    }

    if (child_case == "lock-dir-default") {
      const std::string name = "vlink_utils_child_singleton_" + Utils::get_pid_str();
      CHECK(Utils::check_singleton(name));
      return;
    }

    const ScopedUtilsTempPath tmp_dir("vlink-utils-child-tmp", true);

    const std::vector<Process::EnvironmentMap> child_envs{
        {{"VLINK_UTILS_CHILD_CASE", "tmp-dir-env"}, {"VLINK_TMP_DIR", tmp_dir.string()}},
        {{"VLINK_UTILS_CHILD_CASE", "lock-dir-default"}, {"VLINK_LOCK_DIR", ""}}};

    for (const auto& environment : child_envs) {
      Process child;
      child.set_process_mode(Process::kForwardedMode);
      child.set_inherit_environment(true);
      child.set_environment(environment);
      child.start(Utils::get_app_path(),
                  {"--test-suite=base-Utils", "--test-case=child process covers environment driven singleton helpers",
                   "--no-version"});
      REQUIRE(child.wait_for_finished(Process::kDefaultExecuteTimeoutMs));
      CHECK_EQ(child.get_exit_code(), 0);
    }
  }

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

  TEST_CASE("set_env with force false preserves existing value on POSIX") {
    Utils::set_env("VLINK_TEST_NO_FORCE", "first", true);
    bool ok = Utils::set_env("VLINK_TEST_NO_FORCE", "second", false);
    CHECK(ok);

#if defined(_WIN32)
    CHECK_EQ(Utils::get_env("VLINK_TEST_NO_FORCE"), "second");
#else
    CHECK_EQ(Utils::get_env("VLINK_TEST_NO_FORCE"), "first");
#endif

    Utils::unset_env("VLINK_TEST_NO_FORCE");
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

  TEST_CASE("is_ignored_iface_name filters virtual and tunnel interface prefixes") {
    struct Sample final {
      const char* name;
      bool ignored;
    };

    const Sample samples[] = {
        {nullptr, true},    {"l2tp0", true},     {"lxcbr0", true},     {"llw0", true},       {"lo", false},
        {"vmnet1", true},   {"veth123", true},   {"virbr0", true},     {"vboxnet0", true},   {"vti0", true},
        {"vrf-blue", true}, {"vxlan100", true},  {"br-test", true},    {"bridge0", true},    {"docker0", true},
        {"podman1", true},  {"ppp0", true},      {"pptp0", true},      {"patch-int", true},  {"ovs-system", true},
        {"cni0", true},     {"cali123", true},   {"cri0", true},       {"flannel.1", true},  {"fwln1", true},
        {"fwpr1", true},    {"qbr1", true},      {"qvb1", true},       {"qvo1", true},       {"qr-1", true},
        {"qg-1", true},     {"tun0", true},      {"tap0", true},       {"tailscale0", true}, {"wg0", true},
        {"weave0", true},   {"wlanmon0", true},  {"zt0", true},        {"zerotier0", true},  {"ipip0", true},
        {"ip_vti0", true},  {"ip6_vti0", true},  {"ipvlan0", true},    {"gre0", true},       {"gretap0", true},
        {"gif0", true},     {"erspan0", true},   {"kube-ipvs0", true}, {"macvlan0", true},   {"mon0", true},
        {"sit0", true},     {"utun0", true},     {"awdl0", true},      {"eth0", false},      {"en0", false},
        {"wlan0", false},   {"bond0", false},    {"xfrm0", false},     {"vio0", false},      {"dummy0", false},
        {"p2p0", false},    {"onboard0", false}, {"can0", false},      {"foo0", false},      {"queue0", false},
        {"tty0", false},    {"zebra0", false},   {"ib0", false},       {"gpu0", false},      {"kvm0", false},
        {"media0", false},  {"storage0", false}, {"usb0", false},      {"audio0", false},
    };

    for (const auto& sample : samples) {
      CHECK_EQ(Utils::is_ignored_iface_name(sample.name), sample.ignored);
    }
  }

  TEST_CASE("get_interface_name_by_ipv4 with loopback does not crash") {
    std::string iface = Utils::get_interface_name_by_ipv4("127.0.0.1");
    (void)iface;
  }

  TEST_CASE("get_interface_name_by_ipv4 resolves at least one discovered address") {
    const auto addrs = Utils::get_all_ipv4_address(false);

    for (const auto& addr : addrs) {
      const auto iface = Utils::get_interface_name_by_ipv4(addr);
      CHECK_FALSE(iface.empty());
      return;
    }
  }

  TEST_CASE("get_interface_name_by_ipv4 with bogus address returns empty or name") {
    std::string iface = Utils::get_interface_name_by_ipv4("192.0.2.255");
    (void)iface;
  }

  TEST_CASE("get_interface_name_by_ipv6 with loopback does not crash") {
    std::string iface = Utils::get_interface_name_by_ipv6("::1");
    (void)iface;
  }

  TEST_CASE("get_interface_name_by_ipv6 resolves discovered addresses when present") {
    const auto addrs = Utils::get_all_ipv6_address(false);

    for (const auto& addr : addrs) {
      const auto iface = Utils::get_interface_name_by_ipv6(addr);
      CHECK_FALSE(iface.empty());
      return;
    }
  }

  TEST_CASE("get_interface_name_by_ipv6 with bogus address returns empty") {
    CHECK(Utils::get_interface_name_by_ipv6("2001:db8::ffff").empty());
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

  TEST_CASE("set_thread_priority with explicit thread object does not crash") {
    std::atomic_bool keep_running{true};
    std::thread worker([&keep_running]() {
      while (keep_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(1ms);
      }
    });

    bool ok = Utils::set_thread_priority(0, 0, &worker);
    (void)ok;

    keep_running.store(false, std::memory_order_release);
    worker.join();
  }

  TEST_CASE("set_thread_stick with zero mask returns false") {
    bool ok = Utils::set_thread_stick(0);

    CHECK_FALSE(ok);
  }

  TEST_CASE("set_thread_stick with core zero mask does not crash") {
    bool ok = Utils::set_thread_stick(0x1);
    (void)ok;
  }

  TEST_CASE("set_thread_stick with explicit thread object does not crash") {
    std::atomic_bool keep_running{true};
    std::thread worker([&keep_running]() {
      while (keep_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(1ms);
      }
    });

    bool ok = Utils::set_thread_stick(0x1, &worker);
    (void)ok;

    keep_running.store(false, std::memory_order_release);
    worker.join();
  }

  TEST_CASE("is_process_running returns false for nonexistent process") {
    bool running = Utils::is_process_running("__vlink_fake_process_xyz__");

    CHECK_FALSE(running);
  }

  TEST_CASE("is_process_running returns false for name with dot as literal") {
    bool running = Utils::is_process_running("__vlink_fake_process_._xyz__");

    CHECK_FALSE(running);
  }

  TEST_CASE("is_process_running returns false for empty name") { CHECK_FALSE(Utils::is_process_running("")); }

  TEST_CASE("is_process_running does not execute shell metacharacters") {
    const ScopedUtilsTempPath marker("vlink_is_process_shell_injection_marker");

    const std::string payload = "__vlink_fake_xyz__; touch " + marker.string();
    CHECK_FALSE(Utils::is_process_running(payload));
    CHECK_FALSE(std::filesystem::exists(marker.path()));
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

  TEST_CASE("check_singleton honors an explicit lock directory") {
    const ScopedUtilsTempPath lock_dir("vlink-utils-lock-dir-" + Utils::get_pid_str());
    const ScopedUtilsEnv lock_env("VLINK_LOCK_DIR", lock_dir.string());

    const std::string name = "vlink_utils_singleton_lock_dir_" + Utils::get_pid_str();
    CHECK(Utils::check_singleton(name));

#if !defined(_WIN32)
    CHECK(std::filesystem::exists(lock_dir.path()));
#endif
  }

  TEST_CASE("check_singleton accepts a unique program name") {
    const std::string name = "vlink_utils_singleton_" + Utils::get_pid_str();
    CHECK(Utils::check_singleton(name));
  }

  TEST_CASE("check_singleton rejects a duplicate program name in the same process") {
    const std::string name = "vlink_utils_singleton_dup_" + Utils::get_pid_str();
    CHECK(Utils::check_singleton(name));
    CHECK_FALSE(Utils::check_singleton(name));
  }

  TEST_CASE("check_singleton accepts the default program name") { CHECK(Utils::check_singleton()); }

  TEST_CASE("wait_for_device returns true for a path that already exists") {
    bool ok = Utils::wait_for_device(Utils::get_tmp_dir(), 200, 20);

    CHECK(ok);
  }

  TEST_CASE("wait_for_device returns false for an empty path") { CHECK_FALSE(Utils::wait_for_device("", 0, 0)); }

  TEST_CASE("wait_for_device returns false when path never appears") {
    const ScopedUtilsTempPath missing("vlink_nonexistent_device");
    bool ok = Utils::wait_for_device(missing.string(), 100, 20);

    CHECK_FALSE(ok);
  }

  TEST_CASE("wait_for_device with negative poll interval still times out") {
    const ScopedUtilsTempPath missing("vlink_nonexistent_device_negative_poll");

    CHECK_FALSE(Utils::wait_for_device(missing.string(), 0, -1));
  }

  TEST_CASE("wait_for_device returns true when a file appears before timeout") {
    const ScopedUtilsTempPath marker("vlink_wait_for_device_" + Utils::get_pid_str());

    std::thread creator([path = marker.path()]() {
      std::this_thread::sleep_for(20ms);
      std::ofstream file(path.string());
      file << "ready";
    });

    CHECK(Utils::wait_for_device(marker.string(), 1000, 5));
    creator.join();
  }

  TEST_CASE("signal registration accepts terminate and crash callbacks") {
    Utils::register_terminate_signal([](int) {}, false, false);
    Utils::register_terminate_signal([](int) {}, true, false);
    Utils::register_crash_signal([](int) {});
  }

  TEST_CASE("keyboard detector can start without a callback and stop repeatedly") {
    Utils::stop_detect_keyboard();
    Utils::start_detect_keyboard(nullptr, 1);
    Utils::start_detect_keyboard(nullptr, 1);
    Utils::stop_detect_keyboard();
    Utils::stop_detect_keyboard();
  }

#if defined(__linux__) || defined(__APPLE__)
  TEST_CASE("keyboard detector parses printable and escape input from a pty") {
    int saved_stdin = ::dup(STDIN_FILENO);
    if (saved_stdin < 0) {
      return;
    }

    int master_fd = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master_fd < 0) {
      ::close(saved_stdin);
      return;
    }

    if (::grantpt(master_fd) != 0 || ::unlockpt(master_fd) != 0) {
      ::close(master_fd);
      ::close(saved_stdin);
      return;
    }

    char* slave_name = ::ptsname(master_fd);
    if (slave_name == nullptr) {
      ::close(master_fd);
      ::close(saved_stdin);
      return;
    }

    int slave_fd = ::open(slave_name, O_RDWR | O_NOCTTY);
    if (slave_fd < 0) {
      ::close(master_fd);
      ::close(saved_stdin);
      return;
    }

    if (::dup2(slave_fd, STDIN_FILENO) < 0) {
      ::close(slave_fd);
      ::close(master_fd);
      ::close(saved_stdin);
      return;
    }

    std::mutex keys_mtx;
    std::vector<std::string> keys;

    Utils::start_detect_keyboard(
        [&keys_mtx, &keys](const std::string& key) {
          std::lock_guard lock(keys_mtx);
          keys.emplace_back(key);
        },
        1);

    const char lone_escape[] = "\033";
    REQUIRE(write_all_to_fd(master_fd, lone_escape, sizeof(lone_escape) - 1));

    CHECK(common_test::wait_until(
        [&keys_mtx, &keys]() {
          std::lock_guard lock(keys_mtx);
          return std::find(keys.begin(), keys.end(), "esc") != keys.end();
        },
        1000ms));

    const char input[] =
        "A\r\177\033OA\033OB\033OC\033OD\033OH\033OF\033[A\033[B\033[C\033[D\033[H\033[F"
        "\033[1~\033[4~\033[5~\033[6~\033[9~\033X";
    REQUIRE(write_all_to_fd(master_fd, input, sizeof(input) - 1));

    CHECK(common_test::wait_until(
        [&keys_mtx, &keys]() {
          std::lock_guard lock(keys_mtx);
          return keys.size() >= 22;
        },
        1000ms));

    Utils::stop_detect_keyboard();
    (void)::dup2(saved_stdin, STDIN_FILENO);

    std::vector<std::string> observed;
    {
      std::lock_guard lock(keys_mtx);
      observed = keys;
    }

    CHECK(std::find(observed.begin(), observed.end(), "a") != observed.end());
    CHECK(std::find(observed.begin(), observed.end(), "enter") != observed.end());
    CHECK(std::find(observed.begin(), observed.end(), "backspace") != observed.end());
    CHECK(std::find(observed.begin(), observed.end(), "up") != observed.end());
    CHECK(std::find(observed.begin(), observed.end(), "down") != observed.end());
    CHECK(std::find(observed.begin(), observed.end(), "right") != observed.end());
    CHECK(std::find(observed.begin(), observed.end(), "left") != observed.end());
    CHECK(std::find(observed.begin(), observed.end(), "home") != observed.end());
    CHECK(std::find(observed.begin(), observed.end(), "end") != observed.end());
    CHECK(std::find(observed.begin(), observed.end(), "pgup") != observed.end());
    CHECK(std::find(observed.begin(), observed.end(), "pgdown") != observed.end());
    CHECK(std::find(observed.begin(), observed.end(), "esc") != observed.end());
    CHECK(std::find(observed.begin(), observed.end(), "x") != observed.end());

    ::close(slave_fd);
    ::close(master_fd);
    ::close(saved_stdin);
  }
#endif
}

// NOLINTEND
