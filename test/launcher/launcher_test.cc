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

#include <doctest/doctest.h>
#include <vlink/base/process.h>
#include <vlink/base/utils.h>
#include <vlink/external/launcher_api.h>
#include <vlink/external/launcher_component.h>
#include <vlink/external/launcher_notify.h>
#include <vlink/external/launcher_server.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32) && !defined(_WIN64)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

static std::string make_temp_dir(const std::string& suffix) {
  const auto dir = std::filesystem::path(vlink::Utils::get_tmp_dir()) /
                   ("vlink_launcher_ut_" + vlink::Utils::get_pid_str() + "_" + suffix);
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.generic_string();
}

static std::string escape_toml(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8U);

  for (const char ch : s) {
    if (ch == '\\' || ch == '"') {
      out.push_back('\\');
    }

    out.push_back(ch);
  }

  return out;
}

static std::string read_file_all(const std::string& path) {
  std::ifstream ifs(path);
  return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

static void append_line(const std::string& path, const std::string& line) {
  if (path.empty()) {
    return;
  }

  std::ofstream ofs(path, std::ios::app);
  ofs << line << "\n";
}

static std::optional<std::filesystem::path> find_plugin_binary(const std::filesystem::path& dir) {
  std::error_code ec;
#if defined(_WIN32) || defined(_WIN64)
  const std::vector<std::string> candidates = {"launcher_test_plugin.dll", "liblauncher_test_plugin.dll"};
#elif defined(__APPLE__)
  const std::vector<std::string> candidates = {"liblauncher_test_plugin.dylib"};
#else
  const std::vector<std::string> candidates = {"liblauncher_test_plugin.so"};
#endif

  for (const auto& name : candidates) {
    const auto cand = dir / name;

    if (std::filesystem::exists(cand, ec)) {
      return cand;
    }
  }

  return std::nullopt;
}

template <typename PredT>
static bool wait_until(PredT&& pred, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  return pred();
}

static int run_cli_cmd(const std::string& program, const std::vector<std::string>& args, std::string& output,
                       int timeout_ms = 10000) {
  output.clear();
  vlink::Process proc;
  proc.set_inherit_environment(true);
  proc.set_process_mode(vlink::Process::kMergedMode);
  proc.start(program, args);

  if (!proc.wait_for_started(3000)) {
    return -1;
  }

  const bool finished = proc.wait_for_finished(timeout_ms);
  const size_t avail = proc.bytes_available_stdout();

  if (avail > 0U) {
    std::vector<uint8_t> buf;
    (void)proc.read_stdout(buf, avail);
    output.assign(reinterpret_cast<const char*>(buf.data()), buf.size());
  }

  if (!finished) {
    proc.kill();
    (void)proc.wait_for_finished(2000);
    return -2;
  }

  return proc.get_exit_code();
}

#if !defined(_WIN32) && !defined(_WIN64)
static bool send_raw_notify_datagram(const std::string& addr, const std::string& payload) {
  if (addr.rfind("unix:", 0U) == 0U) {
    const std::string sock_path = addr.substr(5U);
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);

    if (fd < 0) {
      return false;
    }

    struct sockaddr_un sa{};
    sa.sun_family = AF_UNIX;

    if (sock_path.size() >= sizeof(sa.sun_path)) {
      ::close(fd);
      return false;
    }

    std::memcpy(sa.sun_path, sock_path.data(), sock_path.size());
    const ssize_t sent = ::sendto(fd, payload.data(), payload.size(), 0, reinterpret_cast<const sockaddr*>(&sa),
                                  static_cast<socklen_t>(sizeof(sa)));
    ::close(fd);
    return sent == static_cast<ssize_t>(payload.size());
  }

  if (addr.rfind("udp:", 0U) == 0U) {
    const std::string host_port = addr.substr(4U);
    const size_t colon = host_port.rfind(':');

    if (colon == std::string::npos) {
      return false;
    }

    const std::string host = host_port.substr(0U, colon);
    const int port = std::atoi(host_port.substr(colon + 1U).c_str());
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0) {
      return false;
    }

    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<uint16_t>(port));
    (void)::inet_pton(AF_INET, host.c_str(), &sa.sin_addr);
    const ssize_t sent = ::sendto(fd, payload.data(), payload.size(), 0, reinterpret_cast<const sockaddr*>(&sa),
                                  static_cast<socklen_t>(sizeof(sa)));
    ::close(fd);
    return sent == static_cast<ssize_t>(payload.size());
  }

  return false;
}

static std::atomic_bool g_order_exit_stop{false};

static void on_order_exit_term(int sig) noexcept {
  (void)sig;
  g_order_exit_stop.store(true, std::memory_order_release);
}
#endif

TEST_SUITE("launcher") {
  TEST_CASE("param encode/decode roundtrip, strict edge cases and enum names") {
    CHECK(vlink::launcher::unit_type_name(vlink::launcher::UnitType::kProcess) == "process");
    CHECK(vlink::launcher::unit_type_name(vlink::launcher::UnitType::kComponent) == "component");
    CHECK(vlink::launcher::unit_state_name(vlink::launcher::UnitState::kReady) == "ready");
    CHECK(vlink::launcher::error_code_name(vlink::launcher::ErrorCode::kOk) == "ok");

    const std::vector<vlink::LauncherParamValue> samples = {
        true,
        int64_t{-42},
        3.141592653589793,
        std::string("hello"),
        std::vector<bool>{true, false, true},
        std::vector<int64_t>{1, -2, 3},
        std::vector<double>{0.5, 2.25},
        std::vector<std::string>{"a", "b"},
    };

    for (size_t i = 0U; i < samples.size(); ++i) {
      const auto entry = vlink::launcher::param_value_to_entry("k" + std::to_string(i), samples[i]);
      vlink::LauncherParamValue decoded;
      REQUIRE(vlink::launcher::entry_to_param_value(entry, decoded));
      CHECK(decoded == samples[i]);
    }

    vlink::LauncherParamValue out;
    CHECK_FALSE(vlink::launcher::entry_to_param_value({"d_hex", vlink::launcher::ParamType::kDouble, "0x1234"}, out));
    CHECK_FALSE(vlink::launcher::entry_to_param_value({"d_ws", vlink::launcher::ParamType::kDouble, " 1.5"}, out));
    std::string nul_str("1.5");
    nul_str.push_back('\0');
    nul_str.append("junk");
    CHECK_FALSE(vlink::launcher::entry_to_param_value({"d_nul", vlink::launcher::ParamType::kDouble, nul_str}, out));
    CHECK_FALSE(vlink::launcher::entry_to_param_value(
        {"i_u64", vlink::launcher::ParamType::kIntList, "[9223372036854775808]"}, out));
  }

  TEST_CASE("nested TOML includes, [[include]] prefix/ns/group/args, cycle detection and filters") {
    const std::string tmp = make_temp_dir("cfg_nested");
    const std::string sub_dir = tmp + "/sub";
    const std::string deep_dir = sub_dir + "/deep";
    std::error_code ec;
    std::filesystem::create_directories(deep_dir, ec);

    const std::string exe_path = vlink::Utils::get_app_path();
    REQUIRE(!exe_path.empty());

    {
      std::ofstream ofs(deep_dir + "/level2.toml");
      ofs << "[launcher]\nname = \"must_not_override_root\"\nctrl_scheme = \"bad\"\n"
          << "[[process]]\nname = \"leaf\"\ncmd = \"" << escape_toml(exe_path) << "\"\n"
          << "args = [\"${arg.mode}\", \"${this_dir}\"]\n";
    }
    {
      std::ofstream ofs(sub_dir + "/level1.toml");
      ofs << "[launcher]\nincludes = [\"deep/level2.toml\"]\n"
          << "[[process]]\nname = \"mid\"\ncmd = \"" << escape_toml(exe_path) << "\"\n"
          << "depends_on = [\"leaf\"]\n";
    }
    {
      std::ofstream ofs(sub_dir + "/cam_template.toml");
      ofs << "[[process]]\nname = \"drv\"\ncmd = \"" << escape_toml(exe_path) << "\"\n"
          << "[[process]]\nname = \"proc\"\ncmd = \"" << escape_toml(exe_path) << "\"\n"
          << "depends_on = [\"drv\"]\n"
          << "[process.params]\ncam_id = \"${arg.cam_id}\"\n";
    }

    const std::string root_path = tmp + "/root.toml";
    {
      std::ofstream ofs(root_path);
      ofs << "[args]\nmode = \"default_mode\"\n"
          << "[launcher]\nname = \"root-sys\"\nns = \"/robot\"\nctrl_scheme = \"intra\"\n"
          << "includes = [\"sub/level1.toml\"]\n\n"
          << "[[include]]\nfile = \"sub/cam_template.toml\"\nns = \"front\"\ngroup = \"cameras\"\nprefix = \"left_\"\n"
          << "[include.args]\ncam_id = \"L0\"\n\n"
          << "[[include]]\nfile = \"sub/cam_template.toml\"\nns = \"rear\"\ngroup = \"cameras\"\nprefix = \"right_\"\n"
          << "[include.args]\ncam_id = \"R1\"\n\n"
          << "[[process]]\nname = \"top\"\ncmd = \"" << escape_toml(exe_path) << "\"\n"
          << "depends_on = [\"mid\", \"left_proc\", \"right_proc\"]\n";
    }

    vlink::LauncherServer::Config cfg;
    cfg.enable_control_plane = false;
    cfg.args_override["mode"] = "override_mode";
    vlink::LauncherServer srv(cfg);
    std::string err;
    REQUIRE(srv.load_file(root_path, &err));
    CHECK(err.empty());
    CHECK(srv.validate(true).empty());

    const auto order = srv.startup_order();
    REQUIRE(order.size() == 7U);
    auto idx_of = [&](const std::string& n) {
      return static_cast<size_t>(std::find(order.begin(), order.end(), n) - order.begin());
    };
    CHECK(idx_of("leaf") < idx_of("mid"));
    CHECK(idx_of("left_drv") < idx_of("left_proc"));
    CHECK(idx_of("right_drv") < idx_of("right_proc"));
    CHECK(idx_of("mid") < idx_of("top"));
    CHECK(idx_of("left_proc") < idx_of("top"));
    CHECK(idx_of("right_proc") < idx_of("top"));

    const auto snap = srv.get_snapshot();
    bool saw_left_ns = false;
    for (const auto& u : snap.units) {
      if (u.name == "left_proc") {
        saw_left_ns = (u.ns == "/robot/front" && u.group == "cameras");
      }
    }
    CHECK(saw_left_ns);

    // Verify --only closure and --skip filtering.
    vlink::LauncherServer::Config filt_cfg;
    filt_cfg.enable_control_plane = false;
    filt_cfg.only_targets = {"left_proc"};
    vlink::LauncherServer filt_srv(filt_cfg);
    REQUIRE(filt_srv.load_file(root_path, &err));
    const auto only_order = filt_srv.startup_order();
    REQUIRE(only_order.size() == 2U);
    CHECK(only_order[0] == "left_drv");
    CHECK(only_order[1] == "left_proc");

    vlink::LauncherServer::Config skip_cfg;
    skip_cfg.enable_control_plane = false;
    skip_cfg.skip_targets = {"@cameras"};
    vlink::LauncherServer skip_srv(skip_cfg);
    REQUIRE(skip_srv.load_file(root_path, &err));
    const auto skip_order = skip_srv.startup_order();
    CHECK(std::find(skip_order.begin(), skip_order.end(), "left_drv") == skip_order.end());
    CHECK(std::find(skip_order.begin(), skip_order.end(), "right_proc") == skip_order.end());
    // "top" depends on left_proc/right_proc (@cameras), so skip cascades to "top",
    // while unrelated units ("leaf", "mid") remain active.
    CHECK(std::find(skip_order.begin(), skip_order.end(), "top") == skip_order.end());
    CHECK(std::find(skip_order.begin(), skip_order.end(), "leaf") != skip_order.end());
    CHECK(std::find(skip_order.begin(), skip_order.end(), "mid") != skip_order.end());

    // Unknown --only/--skip target must be reported by validate().
    vlink::LauncherServer::Config bad_filt_cfg;
    bad_filt_cfg.enable_control_plane = false;
    bad_filt_cfg.only_targets = {"nonexistent_unit"};
    vlink::LauncherServer bad_filt_srv(bad_filt_cfg);
    REQUIRE(bad_filt_srv.load_file(root_path, &err));
    CHECK(bad_filt_srv.validate(false).find("nonexistent_unit") != std::string::npos);

    // Circular include detection (A -> B -> A).
    {
      std::ofstream ofs(tmp + "/cyc_a.toml");
      ofs << "[launcher]\nincludes = [\"cyc_b.toml\"]\n";
    }
    {
      std::ofstream ofs(tmp + "/cyc_b.toml");
      ofs << "[launcher]\nincludes = [\"cyc_a.toml\"]\n";
    }
    vlink::LauncherServer cyc_inc_srv(cfg);
    CHECK_FALSE(cyc_inc_srv.load_file(tmp + "/cyc_a.toml", &err));
    CHECK(err.find("Circular include") != std::string::npos);

    // Non-array [process] must fail instead of silently ignoring.
    vlink::LauncherServer bad_tbl_srv(cfg);
    CHECK_FALSE(bad_tbl_srv.load_string("[process]\nname = \"oops\"\ncmd = \"/bin/sleep\"\n", tmp, &err));

    // Unit name starting with '@' and non-executable directory cmd must fail validation.
    vlink::LauncherServer bad_val_srv(cfg);
    const std::string bad_val_toml = "[[process]]\nname = \"@bad\"\ncmd = \"" + escape_toml(tmp) + "\"\n";
    REQUIRE(bad_val_srv.load_string(bad_val_toml, tmp, &err));
    const std::string verr = bad_val_srv.validate(true);
    CHECK(verr.find("@bad") != std::string::npos);
    CHECK(verr.find("not found") != std::string::npos);

    // DAG cycle detection.
    vlink::LauncherServer cyc_srv(cfg);
    REQUIRE(
        cyc_srv.load_string("[[process]]\nname = \"a\"\ncmd = \"x\"\ndepends_on = [\"b\"]\n"
                            "[[process]]\nname = \"b\"\ncmd = \"x\"\ndepends_on = [\"a\"]\n",
                            tmp, &err));
    CHECK(cyc_srv.validate(false).find("cycle") != std::string::npos);
  }

  TEST_CASE("LauncherNotify unmanaged defaults") {
    CHECK_FALSE(vlink::LauncherNotify::is_managed());
    CHECK(vlink::LauncherNotify::unit_name().empty());
    CHECK(vlink::LauncherNotify::unit_generation() == 0U);
    CHECK(vlink::LauncherNotify::watchdog_interval_ms() == 0U);
    CHECK_FALSE(vlink::LauncherNotify::notify("READY=1\n"));
    CHECK_FALSE(vlink::LauncherNotify::notify_ready("ok"));
    CHECK_FALSE(vlink::LauncherNotify::notify_watchdog());
    CHECK_FALSE(vlink::LauncherNotify::notify_health(true, "ok"));
    CHECK_FALSE(vlink::LauncherNotify::notify_status("running"));
    CHECK_FALSE(vlink::LauncherNotify::notify_reloading("reloading"));
    CHECK_FALSE(vlink::LauncherNotify::notify_stopping("stopping"));
    CHECK_FALSE(vlink::LauncherNotify::notify_watchdog_trigger("boom"));
    CHECK_FALSE(vlink::LauncherNotify::notify_extend_timeout(0U));
    CHECK_FALSE(vlink::LauncherNotify::notify_extend_timeout(1000U));
  }

  TEST_CASE("reusable LauncherComponent runs in-process (shared & dedicated), standalone binary, and launcher exec") {
    const std::string app_dir = vlink::Utils::get_app_dir();
    const std::filesystem::path dir(app_dir);
#if defined(_WIN32) || defined(_WIN64)
    const std::filesystem::path standalone_bin = dir / "launcher_test_standalone.exe";
    const std::filesystem::path launcher_bin = dir / "vlink-launcher.exe";
#else
    const std::filesystem::path standalone_bin = dir / "launcher_test_standalone";
    const std::filesystem::path launcher_bin = dir / "vlink-launcher";
#endif

    std::error_code ec;
    REQUIRE(std::filesystem::exists(standalone_bin, ec));
    REQUIRE(std::filesystem::exists(launcher_bin, ec));

    const std::string tmp = make_temp_dir("reusable_modes");
    const std::string m_shared = tmp + "/shared.log";
    const std::string m_ded = tmp + "/dedicated.log";
    const std::string m_bin = tmp + "/standalone_bin.log";
    const std::string m_exec = tmp + "/standalone_exec.log";
    const std::string ns = "ln_reuse_" + vlink::Utils::get_pid_str();

#ifdef VLINK_TEST_HAS_LAUNCHER_PLUGIN
    const auto plugin_path = find_plugin_binary(dir);
    REQUIRE(plugin_path.has_value());
    const std::string toml = R"(
[launcher]
name = "reusable-demo"
ctrl_scheme = "intra"
ctrl_namespace = ")" + ns + R"("
plugin_dirs = [")" + escape_toml(app_dir) +
                             R"("]

[[component]]
name = "node_shared"
group = "sensors"
lib = "launcher_test_plugin"
class = "ReusableTestNode"
dedicated = false
watchdog_ms = 1200
[component.params]
rate_hz = 20
label = "shared"
marker_file = ")" + escape_toml(m_shared) +
                             R"("
[component.ready]
type = "component"
timeout_ms = 5000

[[component]]
name = "node_inproc"
group = "sensors"
depends_on = ["node_shared"]
lib = "launcher_test_plugin"
class = "ReusableTestNode"
dedicated = true
watchdog_ms = 1200
[component.params]
rate_hz = 25
label = "inproc"
marker_file = ")" + escape_toml(m_ded) +
                             R"("
[component.ready]
type = "component"
timeout_ms = 5000

[[process]]
name = "node_standalone"
group = "sensors"
depends_on = ["node_inproc"]
cmd = ")" + escape_toml(standalone_bin.generic_string()) +
                             R"("
watchdog_ms = 1200
[process.params]
rate_hz = 50
label = "standalone"
marker_file = ")" + escape_toml(m_bin) +
                             R"("
[process.ready]
type = "notify"
timeout_ms = 5000

[[process]]
name = "node_exec"
depends_on = ["node_standalone"]
cmd = ")" + escape_toml(launcher_bin.generic_string()) +
                             R"("
args = ["exec", ")" + escape_toml(plugin_path->generic_string()) +
                             R"(", "ReusableTestNode"]
watchdog_ms = 1200
[process.params]
rate_hz = 75
label = "exec"
marker_file = ")" + escape_toml(m_exec) +
                             R"("
[process.ready]
type = "notify"
timeout_ms = 5000
)";
#else
    const std::string toml = R"(
[launcher]
name = "reusable-standalone-only"
ctrl_scheme = "intra"
ctrl_namespace = ")" + ns + R"("

[[process]]
name = "node_standalone"
group = "sensors"
cmd = ")" + escape_toml(standalone_bin.generic_string()) +
                             R"("
watchdog_ms = 1200
[process.params]
rate_hz = 50
label = "standalone"
marker_file = ")" + escape_toml(m_bin) +
                             R"("
[process.ready]
type = "notify"
timeout_ms = 5000
)";
#endif

    vlink::LauncherServer::Config scfg;
    vlink::LauncherServer srv(scfg);
    std::string err;
    REQUIRE(srv.load_string(toml, tmp, &err));
    REQUIRE(srv.start());

    // Loading a new manifest while running must be rejected.
    CHECK_FALSE(srv.load_string("[launcher]\nname = \"dup\"\n", tmp, &err));

    auto find_unit = [&](const std::string& name) -> std::optional<vlink::launcher::UnitStatus> {
      for (const auto& u : srv.get_snapshot().units) {
        if (u.name == name) {
          return u;
        }
      }

      return std::nullopt;
    };

#ifdef VLINK_TEST_HAS_LAUNCHER_PLUGIN
    REQUIRE(wait_until(
        [&]() {
          const auto s = find_unit("node_shared");
          const auto a = find_unit("node_inproc");
          const auto b = find_unit("node_standalone");
          const auto c = find_unit("node_exec");
          return s.has_value() && s->state == vlink::launcher::UnitState::kReady && a.has_value() &&
                 a->state == vlink::launcher::UnitState::kReady && b.has_value() &&
                 b->state == vlink::launcher::UnitState::kReady && c.has_value() &&
                 c->state == vlink::launcher::UnitState::kReady;
        },
        10000));

    CHECK(find_unit("node_shared")->detail == "reusable ready:shared");
    CHECK(find_unit("node_inproc")->detail == "reusable ready:inproc");
    CHECK(find_unit("node_standalone")->detail == "reusable ready:standalone");
    CHECK(find_unit("node_exec")->detail == "reusable ready:exec");

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(find_unit("node_shared")->state == vlink::launcher::UnitState::kReady);
    CHECK(find_unit("node_inproc")->state == vlink::launcher::UnitState::kReady);
    CHECK(find_unit("node_standalone")->state == vlink::launcher::UnitState::kReady);
    CHECK(find_unit("node_exec")->state == vlink::launcher::UnitState::kReady);
#else
    REQUIRE(wait_until(
        [&]() {
          const auto b = find_unit("node_standalone");
          return b.has_value() && b->state == vlink::launcher::UnitState::kReady;
        },
        10000));
    CHECK(find_unit("node_standalone")->detail == "reusable ready:standalone");
#endif

    // Exercise LauncherAPI callbacks + full RPC surface over intra://.
    vlink::LauncherAPI::Config acfg;
    acfg.scheme = "intra";
    acfg.namespace_prefix = ns;
    vlink::LauncherAPI api(acfg);
    std::atomic<int> conn_events{0};
    std::atomic<int> snap_events{0};
    std::atomic<int> unit_events{0};
    api.register_connect_callback([&](bool c) {
      if (c) {
        conn_events.fetch_add(1, std::memory_order_relaxed);
      }
    });
    api.register_snapshot_callback(
        [&](const vlink::launcher::SystemSnapshot&) { snap_events.fetch_add(1, std::memory_order_relaxed); });
    api.register_event_callback(
        [&](const vlink::launcher::EventRecord&) { unit_events.fetch_add(1, std::memory_order_relaxed); });
    api.async_run();
    REQUIRE(api.wait_for_connected(4000));
    CHECK(api.is_connected());
    CHECK(!api.session_id().empty());
    CHECK(conn_events.load(std::memory_order_relaxed) >= 1);

    const auto status_res = api.get_status();
    REQUIRE(status_res.ok());
    REQUIRE(status_res.snapshot.has_value());
    CHECK(api.latest_snapshot().has_value());

#ifdef VLINK_TEST_HAS_LAUNCHER_PLUGIN
    CHECK(status_res.snapshot->units.size() == 4U);

    const auto p_res = api.get_params("node_inproc");
    REQUIRE(p_res.ok());
    CHECK(!p_res.params.empty());

    CHECK(api.set_param("node_inproc", vlink::launcher::param_value_to_entry("rate_hz", int64_t{99})).ok());
    CHECK_FALSE(api.set_param("node_inproc", vlink::launcher::param_value_to_entry("reject_me", true)).ok());

    CHECK(api.reload_unit("node_inproc").ok());
    CHECK(api.reload_unit("node_shared").ok());
#if !defined(_WIN32) && !defined(_WIN64)
    CHECK(api.reload_unit("node_standalone").ok());
#endif

    CHECK(wait_until([&]() { return read_file_all(m_ded).find("RELOADED") != std::string::npos; }, 3000));
    CHECK(wait_until([&]() { return read_file_all(m_shared).find("RELOADED") != std::string::npos; }, 3000));
    CHECK(read_file_all(m_shared).find("START:node_shared:ns=:standalone=0:rate=20:label=shared") != std::string::npos);
    CHECK(read_file_all(m_ded).find("START:node_inproc:ns=:standalone=0:rate=25:label=inproc") != std::string::npos);
    CHECK(read_file_all(m_ded).find("PARAM:rate_hz") != std::string::npos);
    CHECK(read_file_all(m_bin).find("START:node_standalone:ns=:standalone=1:rate=50:label=standalone") !=
          std::string::npos);
    CHECK(read_file_all(m_exec).find("START:node_exec:ns=:standalone=1:rate=75:label=exec") != std::string::npos);

#if !defined(_WIN32) && !defined(_WIN64)
    CHECK(wait_until([&]() { return read_file_all(m_bin).find("RELOADED") != std::string::npos; }, 3000));
#endif

    // Exercise stop_unit / start_unit / restart_unit via RPC.
    CHECK(api.restart_unit("node_shared").ok());
    REQUIRE(wait_until(
        [&]() {
          const auto u = find_unit("node_shared");
          return u.has_value() && u->state == vlink::launcher::UnitState::kReady;
        },
        4000));
    CHECK(api.stop_unit("@sensors").ok());
    CHECK(api.start_unit("@sensors").ok());
    REQUIRE(wait_until(
        [&]() {
          const auto u = find_unit("node_standalone");
          return u.has_value() && u->state == vlink::launcher::UnitState::kReady;
        },
        6000));
    CHECK(snap_events.load(std::memory_order_relaxed) >= 1);
    CHECK(unit_events.load(std::memory_order_relaxed) >= 1);
#endif

    CHECK(api.shutdown_server().ok());
    CHECK(srv.wait(5000));
    api.quit(true);
    api.wait_for_quit();

#ifdef VLINK_TEST_HAS_LAUNCHER_PLUGIN
    CHECK(read_file_all(m_shared).find("STOPPED") != std::string::npos);
    CHECK(read_file_all(m_ded).find("STOPPED") != std::string::npos);
#endif
#if !defined(_WIN32) && !defined(_WIN64)
    CHECK(read_file_all(m_bin).find("STOPPED") != std::string::npos);
#endif
  }

  TEST_CASE("LauncherNotify child helper") {
    const std::string role = vlink::Utils::get_env("TEST_NOTIFY_ROLE");

    if (role.empty()) {
      return;
    }

    const std::string out_path = vlink::Utils::get_env("TEST_NOTIFY_OUT");
    const std::string err_path = vlink::Utils::get_env("TEST_NOTIFY_ERR");

    auto verify = [&](bool cond, const char* msg) {
      if (!cond) {
        append_line(err_path, std::string("FAIL:") + msg);
      }
    };

    if (role == "stdout_frag") {
      std::fwrite("PART_A_", 1U, 7U, stdout);
      std::fflush(stdout);
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
      std::fwrite("READY_TOKEN\n", 1U, 12U, stdout);
      std::fflush(stdout);
      std::this_thread::sleep_for(std::chrono::seconds(10));
      return;
    }

    if (role == "exit_ok") {
      return;
    }

    if (role == "exit_fail") {
      std::_Exit(3);
    }

    if (role == "sleep_long") {
      if (!out_path.empty()) {
        append_line(out_path, "STARTED:" + vlink::LauncherNotify::unit_name());
      }

      std::this_thread::sleep_for(std::chrono::seconds(15));
      return;
    }

    if (role == "never_ready_notify") {
      std::this_thread::sleep_for(std::chrono::seconds(10));
      return;
    }

    if (role == "never_ready_stdout") {
      std::fwrite("UNRELATED_LINE\n", 1U, 15U, stdout);
      std::fflush(stdout);
      std::this_thread::sleep_for(std::chrono::seconds(10));
      return;
    }

    if (role == "dump_env_cwd") {
      std::error_code ec;
      const auto cwd = std::filesystem::current_path(ec).generic_string();
      append_line(out_path, "CWD=" + cwd);
      append_line(out_path, "CUSTOM=" + vlink::Utils::get_env("TEST_CUSTOM_ENV"));
      append_line(out_path, "INHERITED_PATH_EMPTY=" + std::string(vlink::Utils::get_env("PATH").empty() ? "1" : "0"));
      append_line(out_path, "UNIT=" + vlink::LauncherNotify::unit_name());
      return;
    }

#if !defined(_WIN32) && !defined(_WIN64)
    if (role == "ignore_sigterm") {
      (void)std::signal(SIGTERM, SIG_IGN);
      (void)vlink::LauncherNotify::notify_ready("stubborn ready");
      std::this_thread::sleep_for(std::chrono::seconds(30));
      return;
    }

    if (role == "order_exit") {
      g_order_exit_stop.store(false, std::memory_order_release);
      struct sigaction sa{};
      sa.sa_handler = on_order_exit_term;
      sigemptyset(&sa.sa_mask);
      (void)::sigaction(SIGTERM, &sa, nullptr);
      const std::string uname = vlink::LauncherNotify::unit_name();
      append_line(out_path, "START:" + uname);
      (void)vlink::LauncherNotify::notify_ready("order ready:" + uname);

      while (!g_order_exit_stop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
      }

      append_line(out_path, "STOP:" + uname);
      std::_Exit(0);
    }

    if (role == "notify_forgery") {
      const std::string addr = vlink::Utils::get_env("VLINK_LAUNCHER_NOTIFY_ADDR");
      const std::string token = vlink::Utils::get_env("VLINK_LAUNCHER_NOTIFY_TOKEN");
      verify(!addr.empty() && !token.empty(), "forgery missing env");

      // 1) Wrong token trying to forge READY=1 (must be dropped).
      (void)send_raw_notify_datagram(addr, "TOKEN=00000000000000000000000000000000\nREADY=1\nSTATUS=forged_tok\n");
      // 2) Missing token trying to forge READY=1 (must be dropped).
      (void)send_raw_notify_datagram(addr, "READY=1\nSTATUS=no_tok\n");
      // 3) Oversized datagram (> 4096 bytes) must not crash server.
      std::string huge = "TOKEN=" + token + "\nSTATUS=";
      huge.append(5000U, 'X');
      huge.push_back('\n');
      (void)send_raw_notify_datagram(addr, huge);
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      append_line(out_path, "AFTER_FORGERY");
      std::this_thread::sleep_for(std::chrono::milliseconds(250));

      // 4) Valid token with ANSI escape + BEL + DEL control characters in STATUS.
      std::string ansi_pkt = "TOKEN=" + token + "\nREADY=1\nSTATUS=clean";
      ansi_pkt.push_back('\x1b');
      ansi_pkt.append("[31mRED");
      ansi_pkt.push_back('\x07');
      ansi_pkt.push_back('\x7f');
      ansi_pkt.append("END\n");
      verify(send_raw_notify_datagram(addr, ansi_pkt), "ansi send failed");
      std::this_thread::sleep_for(std::chrono::seconds(10));
      return;
    }
#endif

    if (role == "delay_worker") {
      verify(vlink::LauncherNotify::is_managed(), "delay_worker not managed");
      verify(vlink::LauncherNotify::notify_extend_timeout(5000U), "delay_worker extend failed");
      std::this_thread::sleep_for(std::chrono::seconds(10));
      return;
    }

    if (role == "notify_worker") {
      verify(vlink::LauncherNotify::is_managed(), "notify_worker not managed");
      verify(vlink::LauncherNotify::unit_name() == "worker", "unit_name mismatch");
      verify(vlink::LauncherNotify::watchdog_interval_ms() == 1200U, "watchdog_ms mismatch");
      const uint64_t gen = vlink::LauncherNotify::unit_generation();
      verify(gen >= 1U, "gen < 1");

      size_t launch_idx = 0U;

      if (!out_path.empty()) {
        {
          std::ifstream ifs(out_path);
          std::string line;

          while (std::getline(ifs, line)) {
            if (!line.empty()) {
              ++launch_idx;
            }
          }
        }

        append_line(out_path, std::to_string(gen));
      }

      if (launch_idx == 0U) {
        verify(vlink::LauncherNotify::notify_extend_timeout(4000U), "extend ms failed");
        verify(vlink::LauncherNotify::notify("EXTEND_TIMEOUT_USEC=4000000\n"), "extend usec failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(1400));
        verify(vlink::LauncherNotify::notify_ready("service listening"), "notify_ready failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        verify(vlink::LauncherNotify::notify_health(false, "degraded\nREADY=0"), "notify_health(false) failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        verify(vlink::LauncherNotify::notify_reloading("reloading\nREADY=0"), "notify_reloading failed");
        verify(vlink::LauncherNotify::notify_health(true, "healthy again"), "notify_health(true) failed");
        verify(vlink::LauncherNotify::notify_status("steady status"), "notify_status failed");
        verify(vlink::LauncherNotify::notify_stopping("draining"), "notify_stopping failed");

        for (int i = 0; i < 8; ++i) {
          verify(vlink::LauncherNotify::notify_watchdog(), "notify_watchdog failed");
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        std::this_thread::sleep_for(std::chrono::seconds(10));
      } else if (launch_idx == 1U) {
        verify(vlink::LauncherNotify::notify_ready("gen2 ready"), "gen2 ready failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        verify(vlink::LauncherNotify::notify_watchdog_trigger("fatal invariant broken"), "watchdog_trigger failed");
        std::this_thread::sleep_for(std::chrono::seconds(10));
      } else {
        verify(vlink::LauncherNotify::notify_ready("gen3 ready"), "gen3 ready failed");

        for (int i = 0; i < 50; ++i) {
          (void)vlink::LauncherNotify::notify_watchdog();
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
      }
    }
  }

  TEST_CASE("orchestration: multi-hop failed cascade, required exit code, fragmented stdout, after & burst limit") {
    const std::string exe_path = vlink::Utils::get_app_path();
    REQUIRE(!exe_path.empty());
    const std::string tmp = make_temp_dir("orch_faults");

    // 1) Fragmented stdout readiness + after weak ordering + autostart=false + multiple ready.type=none units.
    {
      const std::string toml = R"(
[launcher]
name = "orch-basic"
ctrl_enabled = false

[[process]]
name = "frag_proc"
cmd = ")" + escape_toml(exe_path) +
                               R"("
args = ["--test-case=LauncherNotify child helper", "--no-intro=true", "--no-version=true"]
stop_timeout_ms = 500
[process.env]
TEST_NOTIFY_ROLE = "stdout_frag"
[process.ready]
type = "stdout"
pattern = "PART_A_READY_TOKEN"
timeout_ms = 4000

[[process]]
name = "oneshot_a"
cmd = ")" + escape_toml(exe_path) +
                               R"("
args = ["--test-case=LauncherNotify child helper", "--no-intro=true", "--no-version=true"]
[process.env]
TEST_NOTIFY_ROLE = "exit_ok"

[[process]]
name = "after_b"
after = ["oneshot_a"]
cmd = ")" + escape_toml(exe_path) +
                               R"("
args = ["--test-case=LauncherNotify child helper", "--no-intro=true", "--no-version=true"]
[process.env]
TEST_NOTIFY_ROLE = "exit_ok"

[[process]]
name = "manual_only"
autostart = false
cmd = ")" + escape_toml(exe_path) +
                               R"("
args = ["--test-case=LauncherNotify child helper", "--no-intro=true", "--no-version=true"]
[process.env]
TEST_NOTIFY_ROLE = "exit_ok"
)";

      vlink::LauncherServer::Config cfg;
      cfg.enable_control_plane = false;
      vlink::LauncherServer srv(cfg);
      std::string err;
      REQUIRE(srv.load_string(toml, tmp, &err));
      REQUIRE(srv.start());

      auto find_u = [&](const std::string& n) -> std::optional<vlink::launcher::UnitStatus> {
        for (const auto& u : srv.get_snapshot().units) {
          if (u.name == n) {
            return u;
          }
        }

        return std::nullopt;
      };

      REQUIRE(wait_until(
          [&]() {
            const auto f = find_u("frag_proc");
            const auto b = find_u("after_b");
            return f.has_value() && f->state == vlink::launcher::UnitState::kReady && b.has_value() &&
                   (b->state == vlink::launcher::UnitState::kReady || b->state == vlink::launcher::UnitState::kStopped);
          },
          5000));

      CHECK(find_u("manual_only")->state == vlink::launcher::UnitState::kPending);
      CHECK(find_u("frag_proc")->generation == 1U);
      srv.stop();
      CHECK(srv.wait(5000));
    }

    // 2) Multi-hop depends_on kFailed cascade (A -> B -> C where C is required=true) + start_burst circuit breaker.
    {
      const std::string toml = R"(
[launcher]
name = "orch-cascade"
ctrl_enabled = false

[[process]]
name = "unit_a"
cmd = ")" + escape_toml(exe_path) +
                               R"("
args = ["--test-case=LauncherNotify child helper", "--no-intro=true", "--no-version=true"]
restart = "on_failure"
restart_delay_ms = 50
start_burst = 1
start_window_ms = 5000
[process.env]
TEST_NOTIFY_ROLE = "exit_fail"
[process.ready]
type = "notify"
timeout_ms = 2000

[[process]]
name = "unit_b"
depends_on = ["unit_a"]
cmd = ")" + escape_toml(exe_path) +
                               R"("
args = ["--test-case=LauncherNotify child helper", "--no-intro=true", "--no-version=true"]
[process.env]
TEST_NOTIFY_ROLE = "exit_ok"

[[process]]
name = "unit_c"
depends_on = ["unit_b"]
required = true
cmd = ")" + escape_toml(exe_path) +
                               R"("
args = ["--test-case=LauncherNotify child helper", "--no-intro=true", "--no-version=true"]
[process.env]
TEST_NOTIFY_ROLE = "exit_ok"
)";

      vlink::LauncherServer::Config cfg;
      cfg.enable_control_plane = false;
      vlink::LauncherServer srv(cfg);
      std::string err;
      REQUIRE(srv.load_string(toml, tmp, &err));
      REQUIRE(srv.start());

      // Server must self-stop when required unit_c fails via 2-hop dependency cascade, preserving exit_code == 1.
      REQUIRE(srv.wait(8000));
      CHECK(srv.get_exit_code() == 1);

      const auto snap = srv.get_snapshot();
      std::string a_reason;
      std::string b_reason;
      std::string c_reason;
      vlink::launcher::UnitState c_state = vlink::launcher::UnitState::kPending;

      for (const auto& u : snap.units) {
        if (u.name == "unit_a") {
          a_reason = u.failed_reason;
        } else if (u.name == "unit_b") {
          b_reason = u.failed_reason;
        } else if (u.name == "unit_c") {
          c_reason = u.failed_reason;
          c_state = u.state;
        }
      }

      CHECK(a_reason == "start_limit");
      CHECK(b_reason == "dependency_failed");
      CHECK(c_reason == "dependency_failed");
      CHECK(c_state == vlink::launcher::UnitState::kFailed);
      srv.stop();
      CHECK(srv.get_exit_code() == 1);
    }

#ifdef VLINK_TEST_HAS_LAUNCHER_PLUGIN
    // 3) Component on_start() failure + component watchdog timeout with start_burst circuit breaker.
    {
      const std::string app_dir = vlink::Utils::get_app_dir();
      const std::string toml = R"(
[launcher]
name = "comp-faults"
ctrl_enabled = false
plugin_dirs = [")" + escape_toml(app_dir) +
                               R"("]

[[component]]
name = "bad_start"
lib = "launcher_test_plugin"
class = "ReusableTestNode"
[component.params]
fail_start = true

[[component]]
name = "wd_hang"
lib = "launcher_test_plugin"
class = "ReusableTestNode"
watchdog_ms = 600
restart = "on_failure"
restart_delay_ms = 100
start_burst = 1
start_window_ms = 5000
[component.params]
hang_watchdog = true
[component.ready]
type = "component"
timeout_ms = 3000
)";

      vlink::LauncherServer::Config cfg;
      cfg.enable_control_plane = false;
      vlink::LauncherServer srv(cfg);
      std::string err;
      REQUIRE(srv.load_string(toml, tmp, &err));
      REQUIRE(srv.start());

      REQUIRE(wait_until(
          [&]() {
            bool bad_failed = false;
            bool wd_limited = false;

            for (const auto& u : srv.get_snapshot().units) {
              if (u.name == "bad_start" && u.state == vlink::launcher::UnitState::kFailed) {
                bad_failed = true;
              }

              if (u.name == "wd_hang" && u.state == vlink::launcher::UnitState::kFailed &&
                  u.failed_reason == "start_limit" && u.restart_count >= 1U) {
                wd_limited = true;
              }
            }

            return bad_failed && wd_limited;
          },
          8000));

      srv.stop();
      CHECK(srv.wait(5000));
    }
#endif
  }

  TEST_CASE("LauncherNotify end-to-end readiness, extend, health, watchdog timeout and trigger") {
    const std::string exe_path = vlink::Utils::get_app_path();
    REQUIRE(!exe_path.empty());

    const std::string tmp = make_temp_dir("notify_e2e");
    const std::string gen_log = tmp + "/worker_gens.txt";
    const std::string child_err_log = tmp + "/child_errors.txt";
    const std::string ns = "ln_notify_" + vlink::Utils::get_pid_str();

    const std::string toml = R"(
[launcher]
name = "notify-e2e"
ctrl_scheme = "intra"
ctrl_namespace = ")" + ns + R"("

[[process]]
name = "delay_worker"
cmd = ")" + escape_toml(exe_path) +
                             R"("
args = ["--test-case=LauncherNotify child helper", "--no-intro=true", "--no-version=true"]
stop_timeout_ms = 500
[process.env]
TEST_NOTIFY_ROLE = "delay_worker"
TEST_NOTIFY_ERR = ")" + escape_toml(child_err_log) +
                             R"("
[process.ready]
type = "delay"
delay_ms = 300

[[process]]
name = "worker"
cmd = ")" + escape_toml(exe_path) +
                             R"("
args = ["--test-case=LauncherNotify child helper", "--no-intro=true", "--no-version=true"]
watchdog_ms = 1200
restart = "on_failure"
restart_delay_ms = 100
stop_timeout_ms = 500
[process.env]
TEST_NOTIFY_ROLE = "notify_worker"
TEST_NOTIFY_OUT = ")" + escape_toml(gen_log) +
                             R"("
TEST_NOTIFY_ERR = ")" + escape_toml(child_err_log) +
                             R"("
[process.ready]
type = "notify"
timeout_ms = 1200
)";

    vlink::LauncherServer::Config cfg;
    vlink::LauncherServer srv(cfg);
    std::string err;
    REQUIRE(srv.load_string(toml, tmp, &err));

    std::mutex ev_mtx;
    std::vector<std::string> ev_names;
    bool saw_degraded = false;
    bool saw_reloading_sanitized = false;
    bool saw_ready_listening = false;
    bool saw_watchdog_timeout = false;
    bool saw_watchdog_trigger = false;
    srv.register_event_callback([&](const vlink::launcher::EventRecord& ev) {
      if (ev.unit_name == "worker") {
        std::lock_guard lock(ev_mtx);
        ev_names.push_back(ev.event_name);

        if (ev.event_name == "on_ready" && ev.message == "service listening") {
          saw_ready_listening = true;
        }

        if (ev.event_name == "on_reloading" && ev.message == "reloading READY=0") {
          saw_reloading_sanitized = true;
        }

        if (ev.event_name == "on_watchdog_timeout") {
          saw_watchdog_timeout = true;
        }

        if (ev.event_name == "on_watchdog_trigger") {
          saw_watchdog_trigger = true;
        }
      }
    });

    REQUIRE(srv.start());

    auto find_unit = [&](const std::string& name) -> std::optional<vlink::launcher::UnitStatus> {
      for (const auto& u : srv.get_snapshot().units) {
        if (u.name == name) {
          return u;
        }
      }

      return std::nullopt;
    };

    REQUIRE(wait_until(
        [&]() {
          const auto st = find_unit("worker");

          if (st.has_value() && !st->healthy && st->detail == "degraded READY=0") {
            saw_degraded = true;
          }

          return st.has_value() && st->state == vlink::launcher::UnitState::kReady && st->detail == "gen3 ready" &&
                 st->restart_count >= 2U;
        },
        20000));

    CHECK(saw_degraded);
    REQUIRE(wait_until(
        [&]() {
          const auto st = find_unit("delay_worker");
          return st.has_value() && st->state == vlink::launcher::UnitState::kReady;
        },
        4000));

    {
      std::lock_guard lock(ev_mtx);
      CHECK(saw_ready_listening);
      CHECK(saw_reloading_sanitized);
      CHECK(saw_watchdog_timeout);
      CHECK(saw_watchdog_trigger);
      CHECK(std::find(ev_names.begin(), ev_names.end(), "on_reloading") != ev_names.end());
      CHECK(std::find(ev_names.begin(), ev_names.end(), "on_stopping") != ev_names.end());
    }

    CHECK(read_file_all(child_err_log).empty());

    std::vector<uint64_t> gens;
    {
      std::ifstream ifs(gen_log);
      std::string line;

      while (std::getline(ifs, line)) {
        if (!line.empty()) {
          gens.push_back(std::stoull(line));
        }
      }
    }
    REQUIRE(gens.size() >= 3U);
    CHECK(gens[0] < gens[1]);
    CHECK(gens[1] < gens[2]);

    srv.stop();
    CHECK(srv.wait(5000));
  }

  TEST_CASE("TOML config edge cases: 16/17 include depth, multi-level prefix/ns/args, validation matrix") {
    const std::string tmp = make_temp_dir("cfg_edge_matrix");
    const std::string exe_path = vlink::Utils::get_app_path();
    REQUIRE(!exe_path.empty());

    // 1) Include depth boundary: depth 16 (root depth 0 -> inc_1 ... -> inc_16) succeeds; depth 17 fails.
    {
      for (int i = 16; i >= 1; --i) {
        std::ofstream ofs(tmp + "/d_" + std::to_string(i) + ".toml");

        if (i == 16) {
          ofs << "[[process]]\nname = \"deep16\"\ncmd = \"" << escape_toml(exe_path) << "\"\n";
        } else {
          ofs << "[launcher]\nincludes = [\"d_" << (i + 1) << ".toml\"]\n";
        }
      }

      {
        std::ofstream ofs(tmp + "/d_0.toml");
        ofs << "[launcher]\nctrl_enabled = false\nincludes = [\"d_1.toml\"]\n";
      }

      vlink::LauncherServer::Config cfg;
      cfg.enable_control_plane = false;
      vlink::LauncherServer srv16(cfg);
      std::string err;
      REQUIRE(srv16.load_file(tmp + "/d_0.toml", &err));
      CHECK(srv16.startup_order() == std::vector<std::string>{"deep16"});

      // Add a 17th level: d_16 includes d_17 -> must fail max include depth.
      {
        std::ofstream ofs(tmp + "/d_17.toml");
        ofs << "[[process]]\nname = \"deep17\"\ncmd = \"" << escape_toml(exe_path) << "\"\n";
      }
      {
        std::ofstream ofs(tmp + "/d_16.toml");
        ofs << "[launcher]\nincludes = [\"d_17.toml\"]\n";
      }

      vlink::LauncherServer srv17(cfg);
      CHECK_FALSE(srv17.load_file(tmp + "/d_0.toml", &err));
      CHECK(err.find("depth") != std::string::npos);
    }

    // 2) Multi-level [[include]] stacking ns, prefix, group, local depends_on/after remapping, args precedence.
    {
      {
        std::ofstream ofs(tmp + "/leaf_stack.toml");
        ofs << "[args]\nval = \"leaf_default\"\n"
            << "[[process]]\nname = \"drv\"\ncmd = \"" << escape_toml(exe_path) << "\"\n"
            << "depends_on = [\"global_bus\"]\n"
            << "[[process]]\nname = \"worker\"\ncmd = \"" << escape_toml(exe_path) << "\"\n"
            << "depends_on = [\"drv\"]\nafter = [\"drv\"]\n"
            << "args = [\"${arg.val}\", \"${arg.missing_keep}\"]\n";
      }
      {
        std::ofstream ofs(tmp + "/mid_stack.toml");
        ofs << "[[include]]\nfile = \"leaf_stack.toml\"\nns = \"inner\"\nprefix = \"p2_\"\n"
            << "[include.args]\nval = \"mid_arg\"\n";
      }
      {
        std::ofstream ofs(tmp + "/root_stack.toml");
        ofs << "[args]\nval = \"root_default\"\n"
            << "[launcher]\nname = \"stack\"\nns = \"/sys\"\nctrl_enabled = false\n"
            << "[[process]]\nname = \"global_bus\"\ncmd = \"" << escape_toml(exe_path) << "\"\n"
            << "[[include]]\nfile = \"mid_stack.toml\"\nns = \"sub\"\ngroup = \"g_stack\"\nprefix = \"p1_\"\n";
      }

      vlink::LauncherServer::Config cfg;
      cfg.enable_control_plane = false;
      vlink::LauncherServer srv(cfg);
      std::string err;
      REQUIRE(srv.load_file(tmp + "/root_stack.toml", &err));
      CHECK(srv.validate(true).empty());
      const auto order = srv.startup_order();
      REQUIRE(order == std::vector<std::string>{"global_bus", "p1_p2_drv", "p1_p2_worker"});

      bool verified_stacked = false;

      for (const auto& u : srv.get_snapshot().units) {
        if (u.name == "p1_p2_worker") {
          verified_stacked = (u.ns == "/sys/sub/inner" && u.group == "g_stack" &&
                              u.dependencies == std::vector<std::string>{"p1_p2_drv"});
        }
      }

      CHECK(verified_stacked);

      // Combine --only and --skip simultaneously + unknown skip target / unknown @group validation.
      vlink::LauncherServer::Config combo_cfg;
      combo_cfg.enable_control_plane = false;
      combo_cfg.only_targets = {"p1_p2_worker"};
      combo_cfg.skip_targets = {"p1_p2_drv"};
      vlink::LauncherServer combo_srv(combo_cfg);
      REQUIRE(combo_srv.load_file(tmp + "/root_stack.toml", &err));
      // only pulls {global_bus, p1_p2_drv, p1_p2_worker}; skip cascades p1_p2_drv -> p1_p2_worker, leaving global_bus.
      CHECK(combo_srv.startup_order() == std::vector<std::string>{"global_bus"});

      vlink::LauncherServer::Config bad_skip_cfg;
      bad_skip_cfg.enable_control_plane = false;
      bad_skip_cfg.skip_targets = {"@no_such_group", "no_such_unit"};
      vlink::LauncherServer bad_skip_srv(bad_skip_cfg);
      REQUIRE(bad_skip_srv.load_file(tmp + "/root_stack.toml", &err));
      const std::string berr = bad_skip_srv.validate(false);
      CHECK(berr.find("@no_such_group") != std::string::npos);
      CHECK(berr.find("no_such_unit") != std::string::npos);
    }

    // 3) Negative parse & validation matrix.
    {
      vlink::LauncherServer::Config cfg;
      cfg.enable_control_plane = false;
      std::string err;

      // Non-existent file & malformed TOML.
      {
        vlink::LauncherServer s(cfg);
        CHECK_FALSE(s.load_file(tmp + "/does_not_exist.toml", &err));
        CHECK_FALSE(s.load_string("[[process]\nname = broken\n", tmp, &err));
      }

      // Unknown keys in [launcher], [[process]], [[component]], [[include]].
      const std::vector<std::string> bad_key_manifests = {
          "[launcher]\nbogus_key = 1\n",
          "[[process]]\nname = \"p\"\ncmd = \"x\"\nbogus_proc = 1\n",
          "[[component]]\nname = \"c\"\nlib = \"l\"\nclass = \"C\"\nbogus_comp = 1\n",
          "[[include]]\nfile = \"a.toml\"\nbogus_inc = 1\n",
          "[[process]]\nname = \"p\"\ncmd = \"x\"\nrestart = \"bogus_policy\"\n",
          "[[process]]\nname = \"p\"\ncmd = \"x\"\n[process.ready]\ntype = \"bogus_ready\"\n",
      };

      for (const auto& m : bad_key_manifests) {
        vlink::LauncherServer s(cfg);
        CHECK_FALSE(s.load_string(m, tmp, &err));
        CHECK(!err.empty());
      }

      // Duplicate names across process and component.
      {
        vlink::LauncherServer s(cfg);
        REQUIRE(
            s.load_string("[[process]]\nname = \"dup\"\ncmd = \"x\"\n"
                          "[[component]]\nname = \"dup\"\nlib = \"l\"\nclass = \"C\"\n",
                          tmp, &err));
        CHECK(s.validate(false).find("Duplicate") != std::string::npos);
      }

      // Dangling depends_on & after, missing stdout pattern, missing cmd/lib/class.
      {
        vlink::LauncherServer s(cfg);
        REQUIRE(
            s.load_string("[[process]]\nname = \"p1\"\ncmd = \"\"\ndepends_on = [\"ghost_dep\"]\n"
                          "after = [\"ghost_after\"]\n[process.ready]\ntype = \"stdout\"\n"
                          "[[component]]\nname = \"c1\"\nlib = \"\"\nclass = \"\"\n",
                          tmp, &err));
        const std::string verr = s.validate(false);
        CHECK(verr.find("ghost_dep") != std::string::npos);
        CHECK(verr.find("ghost_after") != std::string::npos);
        CHECK(verr.find("pattern") != std::string::npos);
        CHECK(verr.find("p1") != std::string::npos);
        CHECK(verr.find("c1") != std::string::npos);
      }
    }
  }

  TEST_CASE("process runtime: cwd, inherit_env=false, output=none, reverse DAG shutdown, SIGKILL timeout") {
    const std::string exe_path = vlink::Utils::get_app_path();
    REQUIRE(!exe_path.empty());
    const std::string tmp = make_temp_dir("proc_runtime");
    const std::string sub_cwd = tmp + "/custom_cwd";
    std::error_code ec;
    std::filesystem::create_directories(sub_cwd, ec);

    // 1) Verify cwd, inherit_env=false, output=none, ${arg.*} and ${this_dir} expansion at runtime.
    {
      const std::string dump_out = tmp + "/env_cwd.txt";
      const std::string asan_opts = vlink::Utils::get_env("ASAN_OPTIONS");
      std::string toml = R"(
[args]
tag = "default_tag"

[launcher]
name = "env-cwd"
ctrl_enabled = false

[[process]]
name = "env_probe"
cmd = ")" + escape_toml(exe_path) +
                         R"("
args = ["--test-case=LauncherNotify child helper", "--no-intro=true", "--no-version=true"]
cwd = "${this_dir}/custom_cwd"
inherit_env = false
output = "none"
[process.env]
TEST_NOTIFY_ROLE = "dump_env_cwd"
TEST_NOTIFY_OUT = ")" + escape_toml(dump_out) +
                         R"("
TEST_CUSTOM_ENV = "val_${arg.tag}"
)";
      if (!asan_opts.empty()) {
        toml += "ASAN_OPTIONS = \"" + escape_toml(asan_opts) + "\"\n";
      }

      vlink::LauncherServer::Config cfg;
      cfg.enable_control_plane = false;
      cfg.args_override["tag"] = "overridden";
      vlink::LauncherServer srv(cfg);
      std::string err;
      REQUIRE(srv.load_string(toml, tmp, &err));
      REQUIRE(srv.start());

      REQUIRE(wait_until([&]() { return read_file_all(dump_out).find("UNIT=env_probe") != std::string::npos; }, 5000));
      const std::string content = read_file_all(dump_out);
      CHECK(content.find("custom_cwd") != std::string::npos);
      CHECK(content.find("CUSTOM=val_overridden") != std::string::npos);
#if !defined(_WIN32) && !defined(_WIN64)
      CHECK(content.find("INHERITED_PATH_EMPTY=1") != std::string::npos);
#endif
      srv.stop();
      CHECK(srv.wait(5000));
    }

#if !defined(_WIN32) && !defined(_WIN64)
    // 2) Reverse-DAG shutdown order (p_a <- p_b <- p_c => STOP:p_c before STOP:p_b before STOP:p_a)
    //    AND stubborn process ignoring SIGTERM is force-killed after stop_timeout_ms.
    {
      const std::string order_log = tmp + "/order.txt";
      const std::string toml = R"(
[launcher]
name = "reverse-shutdown"
ctrl_enabled = false
shutdown_timeout_ms = 5000

[[process]]
name = "p_a"
cmd = ")" + escape_toml(exe_path) +
                               R"("
args = ["--test-case=LauncherNotify child helper", "--no-intro=true", "--no-version=true"]
[process.env]
TEST_NOTIFY_ROLE = "order_exit"
TEST_NOTIFY_OUT = ")" + escape_toml(order_log) +
                               R"("
[process.ready]
type = "notify"
timeout_ms = 4000

[[process]]
name = "p_b"
depends_on = ["p_a"]
cmd = ")" + escape_toml(exe_path) +
                               R"("
args = ["--test-case=LauncherNotify child helper", "--no-intro=true", "--no-version=true"]
[process.env]
TEST_NOTIFY_ROLE = "order_exit"
TEST_NOTIFY_OUT = ")" + escape_toml(order_log) +
                               R"("
[process.ready]
type = "notify"
timeout_ms = 4000

[[process]]
name = "p_c"
depends_on = ["p_b"]
cmd = ")" + escape_toml(exe_path) +
                               R"("
args = ["--test-case=LauncherNotify child helper", "--no-intro=true", "--no-version=true"]
[process.env]
TEST_NOTIFY_ROLE = "order_exit"
TEST_NOTIFY_OUT = ")" + escape_toml(order_log) +
                               R"("
[process.ready]
type = "notify"
timeout_ms = 4000

[[process]]
name = "stubborn"
cmd = ")" + escape_toml(exe_path) +
                               R"("
args = ["--test-case=LauncherNotify child helper", "--no-intro=true", "--no-version=true"]
stop_timeout_ms = 300
[process.env]
TEST_NOTIFY_ROLE = "ignore_sigterm"
[process.ready]
type = "notify"
timeout_ms = 4000
)";

      vlink::LauncherServer::Config cfg;
      cfg.enable_control_plane = false;
      vlink::LauncherServer srv(cfg);
      std::string err;
      REQUIRE(srv.load_string(toml, tmp, &err));
      REQUIRE(srv.start());

      REQUIRE(wait_until(
          [&]() {
            size_t ready_cnt = 0U;

            for (const auto& u : srv.get_snapshot().units) {
              if (u.state == vlink::launcher::UnitState::kReady) {
                ++ready_cnt;
              }
            }

            return ready_cnt == 4U;
          },
          6000));

      const auto t0 = std::chrono::steady_clock::now();
      srv.stop();
      CHECK(srv.wait(5000));
      const auto elapsed_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
      // Stubborn process ignores SIGTERM for 30s; stop_timeout_ms=300 must SIGKILL it well under 4s.
      CHECK(elapsed_ms < 4000);

      const std::string log_text = read_file_all(order_log);
      const size_t pos_sa = log_text.find("START:p_a");
      const size_t pos_sb = log_text.find("START:p_b");
      const size_t pos_sc = log_text.find("START:p_c");
      const size_t pos_ec = log_text.find("STOP:p_c");
      const size_t pos_eb = log_text.find("STOP:p_b");
      const size_t pos_ea = log_text.find("STOP:p_a");
      REQUIRE(pos_sa != std::string::npos);
      REQUIRE(pos_sb != std::string::npos);
      REQUIRE(pos_sc != std::string::npos);
      REQUIRE(pos_ec != std::string::npos);
      REQUIRE(pos_eb != std::string::npos);
      REQUIRE(pos_ea != std::string::npos);
      CHECK(pos_sa < pos_sb);
      CHECK(pos_sb < pos_sc);
      CHECK(pos_ec < pos_eb);
      CHECK(pos_eb < pos_ea);
    }
#endif
  }

  TEST_CASE("restart policies, disable_respawn, after weak ordering, delay_ms cancel, readiness timeouts") {
    const std::string exe_path = vlink::Utils::get_app_path();
    REQUIRE(!exe_path.empty());
    const std::string tmp = make_temp_dir("policies_timeouts");

    // 1) Restart policies (no / on_failure-exit0 / always+burst), after weak ordering over failed upstream,
    //    delay_ms cancellation without ghost launch, and notify/stdout/component readiness timeouts.
    {
      const std::string delayed_log = tmp + "/delayed.txt";
      const std::string app_dir = vlink::Utils::get_app_dir();
      std::string toml = R"(
[launcher]
name = "policies"
ctrl_enabled = false
plugin_dirs = [")" + escape_toml(app_dir) +
                         R"("]

[[process]]
name = "fail_no_restart"
cmd = ")" + escape_toml(exe_path) +
                         R"("
args = ["--test-case=LauncherNotify child helper", "--no-intro=true", "--no-version=true"]
restart = "no"
[process.env]
TEST_NOTIFY_ROLE = "exit_fail"

[[process]]
name = "after_weak"
after = ["fail_no_restart"]
cmd = ")" + escape_toml(exe_path) +
                         R"("
args = ["--test-case=LauncherNotify child helper", "--no-intro=true", "--no-version=true"]
[process.env]
TEST_NOTIFY_ROLE = "exit_ok"

[[process]]
name = "on_fail_clean"
cmd = ")" + escape_toml(exe_path) +
                         R"("
args = ["--test-case=LauncherNotify child helper", "--no-intro=true", "--no-version=true"]
restart = "on_failure"
[process.env]
TEST_NOTIFY_ROLE = "exit_ok"

[[process]]
name = "always_burst"
cmd = ")" + escape_toml(exe_path) +
                         R"("
args = ["--test-case=LauncherNotify child helper", "--no-intro=true", "--no-version=true"]
restart = "always"
restart_delay_ms = 50
start_burst = 2
start_window_ms = 5000
[process.env]
TEST_NOTIFY_ROLE = "exit_ok"

[[process]]
name = "delayed_cancel"
delay_ms = 900
cmd = ")" + escape_toml(exe_path) +
                         R"("
args = ["--test-case=LauncherNotify child helper", "--no-intro=true", "--no-version=true"]
[process.env]
TEST_NOTIFY_ROLE = "sleep_long"
TEST_NOTIFY_OUT = ")" + escape_toml(delayed_log) +
                         R"("

[[process]]
name = "notify_timeout"
cmd = ")" + escape_toml(exe_path) +
                         R"("
args = ["--test-case=LauncherNotify child helper", "--no-intro=true", "--no-version=true"]
stop_timeout_ms = 400
[process.env]
TEST_NOTIFY_ROLE = "never_ready_notify"
[process.ready]
type = "notify"
timeout_ms = 450

[[process]]
name = "stdout_timeout"
cmd = ")" + escape_toml(exe_path) +
                         R"("
args = ["--test-case=LauncherNotify child helper", "--no-intro=true", "--no-version=true"]
stop_timeout_ms = 400
[process.env]
TEST_NOTIFY_ROLE = "never_ready_stdout"
[process.ready]
type = "stdout"
pattern = "NEVER_PRINTED"
timeout_ms = 450
)";
#ifdef VLINK_TEST_HAS_LAUNCHER_PLUGIN
      toml += R"(
[[component]]
name = "comp_timeout"
lib = "launcher_test_plugin"
class = "ReusableTestNode"
[component.params]
never_ready = true
[component.ready]
type = "component"
timeout_ms = 450
)";
#endif

      vlink::LauncherServer::Config cfg;
      cfg.enable_control_plane = false;
      vlink::LauncherServer srv(cfg);
      std::string err;
      REQUIRE(srv.load_string(toml, tmp, &err));
      REQUIRE(srv.start());

      // Cancel delayed_cancel well before its 900ms delay_ms fires.
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      CHECK(srv.stop_unit("delayed_cancel").ok());

      auto find_u = [&](const std::string& n) -> std::optional<vlink::launcher::UnitStatus> {
        for (const auto& u : srv.get_snapshot().units) {
          if (u.name == n) {
            return u;
          }
        }

        return std::nullopt;
      };

      auto check_policies_done = [&]() {
        const auto fnr = find_u("fail_no_restart");
        const auto aw = find_u("after_weak");
        const auto ofc = find_u("on_fail_clean");
        const auto ab = find_u("always_burst");
        const auto nt = find_u("notify_timeout");
        const auto st = find_u("stdout_timeout");
        bool ok = fnr && fnr->state == vlink::launcher::UnitState::kFailed && aw &&
                  aw->state == vlink::launcher::UnitState::kStopped && ofc &&
                  ofc->state == vlink::launcher::UnitState::kStopped && ab &&
                  ab->state == vlink::launcher::UnitState::kFailed && nt &&
                  nt->state == vlink::launcher::UnitState::kFailed && st &&
                  st->state == vlink::launcher::UnitState::kFailed;
#ifdef VLINK_TEST_HAS_LAUNCHER_PLUGIN
        const auto ct = find_u("comp_timeout");
        ok = ok && ct && ct->state == vlink::launcher::UnitState::kFailed;
#endif
        return ok;
      };
      REQUIRE(wait_until(check_policies_done, 8000));

      CHECK(find_u("fail_no_restart")->restart_count == 0U);
      CHECK(find_u("on_fail_clean")->restart_count == 0U);
      CHECK(find_u("always_burst")->restart_count >= 2U);
      CHECK(find_u("always_burst")->failed_reason == "start_limit");
      CHECK(find_u("notify_timeout")->failed_reason == "ready_timeout");
      CHECK(find_u("stdout_timeout")->failed_reason == "ready_timeout");
#ifdef VLINK_TEST_HAS_LAUNCHER_PLUGIN
      CHECK(find_u("comp_timeout")->failed_reason == "ready_timeout");
#endif

      // Ensure delayed_cancel stayed stopped past 900ms (no ghost launch).
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      CHECK(find_u("delayed_cancel")->state == vlink::launcher::UnitState::kStopped);
      CHECK(read_file_all(delayed_log).empty());

      srv.stop();
      CHECK(srv.wait(5000));
    }

    // 2) Config::disable_respawn suppresses restart = "always".
    {
      const std::string toml = R"(
[launcher]
name = "no-respawn"
ctrl_enabled = false

[[process]]
name = "suppressed"
cmd = ")" + escape_toml(exe_path) +
                               R"("
args = ["--test-case=LauncherNotify child helper", "--no-intro=true", "--no-version=true"]
restart = "always"
restart_delay_ms = 50
[process.env]
TEST_NOTIFY_ROLE = "exit_fail"
)";
      vlink::LauncherServer::Config cfg;
      cfg.enable_control_plane = false;
      cfg.disable_respawn = true;
      vlink::LauncherServer srv(cfg);
      std::string err;
      REQUIRE(srv.load_string(toml, tmp, &err));
      REQUIRE(srv.start());

      REQUIRE(wait_until(
          [&]() {
            const auto snap = srv.get_snapshot();
            return snap.units.size() == 1U && snap.units[0].state == vlink::launcher::UnitState::kFailed;
          },
          5000));
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      CHECK(srv.get_snapshot().units[0].restart_count == 0U);
      srv.stop();
      CHECK(srv.wait(5000));
    }
  }

  TEST_CASE("component full context: all 8 param types, self-stop (inproc & standalone), MinimalTestNode defaults") {
    const std::string app_dir = vlink::Utils::get_app_dir();
    const std::filesystem::path dir(app_dir);
#if defined(_WIN32) || defined(_WIN64)
    const std::filesystem::path standalone_bin = dir / "launcher_test_standalone.exe";
#else
    const std::filesystem::path standalone_bin = dir / "launcher_test_standalone";
#endif
    const std::string tmp = make_temp_dir("comp_full_ctx");
    const std::string m_inproc = tmp + "/inproc_all.log";
    const std::string m_proc = tmp + "/proc_all.log";

    std::string toml = R"(
[launcher]
name = "full-ctx"
ns = "/robot"
ctrl_enabled = false
plugin_dirs = [")" + escape_toml(app_dir) +
                       R"("]

[[process]]
name = "proc_selfstop"
ns = "nav"
cmd = ")" + escape_toml(standalone_bin.generic_string()) +
                       R"("
watchdog_ms = 2000
[process.params]
verify_all_params = true
self_stop_ms = 250
marker_file = ")" + escape_toml(m_proc) +
                       R"("
p_bool = true
p_int = -42
p_double = 3.14159
p_str = "hello"
p_bool_list = [true, false]
p_int_list = [1, -2, 3]
p_double_list = [0.5, 2.5]
p_str_list = ["x", "y"]
[process.ready]
type = "notify"
timeout_ms = 5000
)";

#ifdef VLINK_TEST_HAS_LAUNCHER_PLUGIN
    toml += R"(
[[component]]
name = "comp_selfstop"
ns = "nav"
lib = "launcher_test_plugin"
class = "ReusableTestNode"
watchdog_ms = 2000
[component.params]
verify_all_params = true
self_stop_ms = 250
marker_file = ")" +
            escape_toml(m_inproc) + R"("
p_bool = true
p_int = -42
p_double = 3.14159
p_str = "hello"
p_bool_list = [true, false]
p_int_list = [1, -2, 3]
p_double_list = [0.5, 2.5]
p_str_list = ["x", "y"]
[component.ready]
type = "component"
timeout_ms = 5000

[[component]]
name = "comp_minimal"
lib = "launcher_test_plugin"
class = "MinimalTestNode"
[component.ready]
type = "component"
timeout_ms = 4000
)";
#endif

    vlink::LauncherServer::Config cfg;
    cfg.enable_control_plane = false;
    vlink::LauncherServer srv(cfg);
    std::string err;
    REQUIRE(srv.load_string(toml, tmp, &err));
    REQUIRE(srv.start());

    auto find_u = [&](const std::string& n) -> std::optional<vlink::launcher::UnitStatus> {
      for (const auto& u : srv.get_snapshot().units) {
        if (u.name == n) {
          return u;
        }
      }

      return std::nullopt;
    };

    auto check_ctx_done = [&]() {
      const auto p = find_u("proc_selfstop");
      bool ok = p && p->state == vlink::launcher::UnitState::kStopped;
#ifdef VLINK_TEST_HAS_LAUNCHER_PLUGIN
      const auto c = find_u("comp_selfstop");
      const auto m = find_u("comp_minimal");
      ok = ok && c && c->state == vlink::launcher::UnitState::kStopped && m &&
           m->state == vlink::launcher::UnitState::kReady;
#endif
      return ok;
    };
    REQUIRE(wait_until(check_ctx_done, 8000));

    const std::string p_log = read_file_all(m_proc);
    CHECK(p_log.find("ALL_PARAMS_OK") != std::string::npos);
    CHECK(p_log.find(":wd=2000:stopping=0") != std::string::npos);
    CHECK(p_log.find("STOPPED:stopping=1") != std::string::npos);

#ifdef VLINK_TEST_HAS_LAUNCHER_PLUGIN
    const std::string c_log = read_file_all(m_inproc);
    CHECK(c_log.find("START:comp_selfstop:ns=/robot/nav:standalone=0") != std::string::npos);
    CHECK(c_log.find("ALL_PARAMS_OK") != std::string::npos);
    CHECK(c_log.find(":wd=2000:stopping=0") != std::string::npos);
    CHECK(c_log.find("STOPPED:stopping=1") != std::string::npos);

    // MinimalTestNode uses base LauncherComponent default on_reload() / on_param_changed() (both return true).
    CHECK(srv.reload_unit("comp_minimal").ok());
    CHECK(srv.set_param("comp_minimal", vlink::launcher::param_value_to_entry("any_key", int64_t{7})).ok());
#endif

    srv.stop();
    CHECK(srv.wait(5000));
  }

#if !defined(_WIN32) && !defined(_WIN64)
  TEST_CASE("NotifyServer security: forged token, missing token, oversized datagram, ANSI sanitization") {
    const std::string exe_path = vlink::Utils::get_app_path();
    REQUIRE(!exe_path.empty());
    const std::string tmp = make_temp_dir("notify_sec");
    const std::string out_log = tmp + "/forge_out.txt";
    const std::string err_log = tmp + "/forge_err.txt";

    const std::string toml = R"(
[launcher]
name = "notify-sec"
ctrl_enabled = false

[[process]]
name = "forge_proc"
cmd = ")" + escape_toml(exe_path) +
                             R"("
args = ["--test-case=LauncherNotify child helper", "--no-intro=true", "--no-version=true"]
stop_timeout_ms = 500
[process.env]
TEST_NOTIFY_ROLE = "notify_forgery"
TEST_NOTIFY_OUT = ")" + escape_toml(out_log) +
                             R"("
TEST_NOTIFY_ERR = ")" + escape_toml(err_log) +
                             R"("
[process.ready]
type = "notify"
timeout_ms = 5000
)";

    vlink::LauncherServer::Config cfg;
    cfg.enable_control_plane = false;
    vlink::LauncherServer srv(cfg);
    std::string err;
    REQUIRE(srv.load_string(toml, tmp, &err));
    REQUIRE(srv.start());

    // Wait until child has sent all forged/oversized packets and written AFTER_FORGERY.
    REQUIRE(wait_until([&]() { return read_file_all(out_log).find("AFTER_FORGERY") != std::string::npos; }, 5000));
    // Unit MUST NOT have become ready from forged packets!
    {
      const auto snap = srv.get_snapshot();
      REQUIRE(snap.units.size() == 1U);
      CHECK(snap.units[0].state != vlink::launcher::UnitState::kReady);
      CHECK(snap.units[0].detail.find("forged_tok") == std::string::npos);
      CHECK(snap.units[0].detail.find("no_tok") == std::string::npos);
    }

    // Then valid packet with ANSI escape arrives -> unit becomes ready with sanitized detail (no ESC/BEL/DEL).
    REQUIRE(wait_until(
        [&]() {
          const auto snap = srv.get_snapshot();
          return snap.units.size() == 1U && snap.units[0].state == vlink::launcher::UnitState::kReady;
        },
        5000));
    const std::string detail = srv.get_snapshot().units[0].detail;
    CHECK(detail == "clean [31mRED  END");
    CHECK(detail.find('\x1b') == std::string::npos);
    CHECK(detail.find('\x07') == std::string::npos);
    CHECK(detail.find('\x7f') == std::string::npos);
    CHECK(read_file_all(err_log).empty());

    srv.stop();
    CHECK(srv.wait(5000));
  }
#endif

  TEST_CASE("control plane: full ErrorCode matrix, disconnect/reconnect, security_key auth, concurrent stress") {
    const std::string app_dir = vlink::Utils::get_app_dir();
    const std::string exe_path = vlink::Utils::get_app_path();
    const std::string tmp = make_temp_dir("ctrl_matrix");
    const std::string ns = "ln_ctrl_" + vlink::Utils::get_pid_str();

    std::string toml = R"(
[launcher]
name = "ctrl-matrix"
ctrl_scheme = "intra"
ctrl_namespace = ")" + ns +
                       R"("
plugin_dirs = [")" + escape_toml(app_dir) +
                       R"("]

[[process]]
name = "p_idle"
autostart = false
cmd = ")" + escape_toml(exe_path) +
                       R"("
args = ["--test-case=LauncherNotify child helper", "--no-intro=true", "--no-version=true"]
stop_timeout_ms = 400
[process.env]
TEST_NOTIFY_ROLE = "sleep_long"
)";
#ifdef VLINK_TEST_HAS_LAUNCHER_PLUGIN
    toml += R"(
[[component]]
name = "c_live"
lib = "launcher_test_plugin"
class = "ReusableTestNode"
[component.params]
rate_hz = 10
[component.ready]
type = "component"
timeout_ms = 4000
)";
#endif

    // 1) Never-started server & API clean destruction + disconnected RPC error codes.
    {
      vlink::LauncherServer idle_srv;
      CHECK_FALSE(idle_srv.is_active());
      CHECK(idle_srv.start_unit("x").code == vlink::launcher::ErrorCode::kNotRunning);
      idle_srv.stop();

      vlink::LauncherAPI::Config dcfg;
      dcfg.scheme = "intra";
      dcfg.namespace_prefix = ns + "_nobody";
      dcfg.default_timeout_ms = 300;
      vlink::LauncherAPI idle_api(dcfg);
      CHECK_FALSE(idle_api.is_connected());
      CHECK(idle_api.get_status().code == vlink::launcher::ErrorCode::kDisconnected);
    }

    // 2) Live server ErrorCode matrix + disconnect/reconnect across server restart.
    vlink::LauncherAPI::Config acfg;
    acfg.scheme = "intra";
    acfg.namespace_prefix = ns;
    acfg.default_timeout_ms = 1500;
    vlink::LauncherAPI api(acfg);
    std::atomic<int> connects{0};
    std::atomic<int> disconnects{0};
    api.register_connect_callback([&](bool c) {
      if (c) {
        connects.fetch_add(1, std::memory_order_relaxed);
      } else {
        disconnects.fetch_add(1, std::memory_order_relaxed);
      }
    });
    api.async_run();

    std::string session_1;
    {
      vlink::LauncherServer srv1;
      std::string err;
      REQUIRE(srv1.load_string(toml, tmp, &err));
      REQUIRE(srv1.start());
      CHECK(srv1.is_active());
      REQUIRE(api.wait_for_connected(4000));
      session_1 = api.session_id();
      CHECK(!session_1.empty());

      // ErrorCode matrix: kNotFound, kNotRunning, kAlreadyRunning, kUnsupported, get_params("").
      CHECK(api.start_unit("no_such").code == vlink::launcher::ErrorCode::kNotFound);
      CHECK(api.stop_unit("@no_group").code == vlink::launcher::ErrorCode::kNotFound);
      CHECK(api.restart_unit("no_such").code == vlink::launcher::ErrorCode::kNotFound);
      CHECK(api.reload_unit("no_such").code == vlink::launcher::ErrorCode::kNotFound);
      CHECK(api.get_params("no_such").code == vlink::launcher::ErrorCode::kNotFound);
      CHECK(api.stop_unit("p_idle").code == vlink::launcher::ErrorCode::kNotRunning);
      CHECK(api.reload_unit("p_idle").code == vlink::launcher::ErrorCode::kNotRunning);
      CHECK(api.set_param("p_idle", vlink::launcher::param_value_to_entry("k", int64_t{1})).code ==
            vlink::launcher::ErrorCode::kUnsupported);

#ifdef VLINK_TEST_HAS_LAUNCHER_PLUGIN
      REQUIRE(wait_until(
          [&]() {
            const auto st = api.get_status();
            if (!st.ok() || !st.snapshot.has_value()) {
              return false;
            }
            for (const auto& u : st.snapshot->units) {
              if (u.name == "c_live" && u.state == vlink::launcher::UnitState::kReady) {
                return true;
              }
            }
            return false;
          },
          5000));
      CHECK(api.start_unit("c_live").code == vlink::launcher::ErrorCode::kAlreadyRunning);
      const auto all_params = api.get_params("");
      REQUIRE(all_params.ok());
      CHECK(!all_params.params.empty());

      // Concurrent stress: 4 worker threads hammering server/API while server shuts down.
      std::atomic_bool stop_stress{false};
      std::vector<std::thread> workers;
      for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&, t]() {
          for (int iter = 0; iter < 15 && !stop_stress.load(std::memory_order_acquire); ++iter) {
            (void)srv1.get_snapshot();
            (void)srv1.get_params("c_live");
            (void)srv1.set_param("c_live", vlink::launcher::param_value_to_entry("rate_hz", int64_t{10 + t + iter}));
            if (iter % 5 == 0) {
              (void)srv1.reload_unit("c_live");
            }
          }
        });
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      srv1.request_stop(0);
      srv1.stop();
      srv1.stop();  // Idempotent double-stop.
      stop_stress.store(true, std::memory_order_release);
      for (auto& th : workers) {
        th.join();
      }
#else
      srv1.stop();
#endif
      CHECK(srv1.wait(5000));
    }

    // Wait for client to detect disconnect, then bring up srv2 and verify seamless reconnect with a new session_id.
    REQUIRE(wait_until([&]() { return !api.is_connected(); }, 8000));
    CHECK(disconnects.load(std::memory_order_relaxed) >= 1);

    {
      vlink::LauncherServer srv2;
      std::string err;
      REQUIRE(srv2.load_string(toml, tmp, &err));
      REQUIRE(srv2.start());
      REQUIRE(api.wait_for_connected(5000));
      CHECK(api.session_id() != session_1);
      CHECK(connects.load(std::memory_order_relaxed) >= 2);
      srv2.stop();
      CHECK(srv2.wait(5000));
    }

    api.quit(true);
    api.wait_for_quit();

    // 3) Security key authentication (zenoh) or fail-closed check when built without security.
    {
      const std::string sec_ns = "ln_sec_" + vlink::Utils::get_pid_str();
      const std::string sec_toml = "[launcher]\nname = \"sec\"\nctrl_scheme = \"zenoh\"\nctrl_namespace = \"" + sec_ns +
                                   "\"\nctrl_domain = 91\n";
      vlink::LauncherServer::Config scfg;
      scfg.security_key_override = "0123456789abcdef0123456789abcdef";
      vlink::LauncherServer sec_srv(scfg);
      std::string err;
      REQUIRE(sec_srv.load_string(sec_toml, tmp, &err));
#ifdef VLINK_TEST_SUPPORT_SECURITY
      REQUIRE(sec_srv.start());

      // Matching key succeeds.
      vlink::LauncherAPI::Config ok_cfg;
      ok_cfg.scheme = "zenoh";
      ok_cfg.namespace_prefix = sec_ns;
      ok_cfg.domain_id = 91U;
      ok_cfg.security_key = "0123456789abcdef0123456789abcdef";
      ok_cfg.default_timeout_ms = 2000;
      vlink::LauncherAPI ok_api(ok_cfg);
      ok_api.async_run();
      REQUIRE(ok_api.wait_for_connected(5000));
      CHECK(ok_api.get_status().ok());
      ok_api.quit(true);
      ok_api.wait_for_quit();

      // Wrong key must NOT connect or execute RPC.
      vlink::LauncherAPI::Config bad_cfg = ok_cfg;
      bad_cfg.security_key = "fedcba9876543210fedcba9876543210";
      bad_cfg.default_timeout_ms = 600;
      vlink::LauncherAPI bad_api(bad_cfg);
      bad_api.async_run();
      CHECK_FALSE(bad_api.wait_for_connected(1500));
      CHECK_FALSE(bad_api.get_status().ok());
      bad_api.quit(true);
      bad_api.wait_for_quit();

      sec_srv.stop();
      CHECK(sec_srv.wait(5000));
#else
      // Fail-closed when VLINK_ENABLE_SECURITY is OFF.
      CHECK_FALSE(sec_srv.start());
#endif
    }
  }

  TEST_CASE("vlink-launcher CLI full command matrix and exit codes") {
    const std::string app_dir = vlink::Utils::get_app_dir();
    const std::filesystem::path dir(app_dir);
#if defined(_WIN32) || defined(_WIN64)
    const std::filesystem::path launcher_bin = dir / "vlink-launcher.exe";
    const std::filesystem::path standalone_bin = dir / "launcher_test_standalone.exe";
#else
    const std::filesystem::path launcher_bin = dir / "vlink-launcher";
    const std::filesystem::path standalone_bin = dir / "launcher_test_standalone";
#endif
    std::error_code ec;
    REQUIRE(std::filesystem::exists(launcher_bin, ec));
    const std::string cli = launcher_bin.generic_string();
    const std::string tmp = make_temp_dir("cli_matrix");
    std::string out;

    // 1) Basic help/version/error exit codes.
    CHECK(run_cli_cmd(cli, {"--help"}, out) == 0);
    CHECK(out.find("Usage:") != std::string::npos);
    CHECK(run_cli_cmd(cli, {"--version"}, out) == 0);
    CHECK(out.find("vlink-launcher") != std::string::npos);
    CHECK(run_cli_cmd(cli, {"nonexistent-cmd"}, out) == 2);
    CHECK(run_cli_cmd(cli, {"plugin"}, out) == 2);
    CHECK(run_cli_cmd(cli, {"plugin", tmp + "/missing_plugin.so"}, out) == 1);
    CHECK(run_cli_cmd(cli, {"validate"}, out) == 2);
    CHECK(run_cli_cmd(cli, {"validate", tmp + "/missing.toml", "--bad-flag"}, out) == 2);

#ifdef VLINK_TEST_HAS_LAUNCHER_PLUGIN
    const auto plugin_path = find_plugin_binary(dir);
    REQUIRE(plugin_path.has_value());
    CHECK(run_cli_cmd(cli, {"plugin", plugin_path->generic_string()}, out) == 0);
    CHECK(out.find("ReusableTestNode") != std::string::npos);
    CHECK(out.find("MinimalTestNode") != std::string::npos);

    // exec with invalid class must exit non-zero.
    CHECK(run_cli_cmd(cli, {"exec", plugin_path->generic_string(), "NoSuchClass"}, out) != 0);

    // 2) Full live CLI control-plane workflow over zenoh.
    const std::string cli_ns = "ln_cli_" + vlink::Utils::get_pid_str();
    const std::string manifest_path = tmp + "/cli_live.toml";
    {
      std::ofstream ofs(manifest_path);
      ofs << "[args]\nrate = \"15\"\n\n"
          << "[launcher]\nname = \"cli-live\"\nctrl_scheme = \"zenoh\"\nctrl_namespace = \"" << cli_ns
          << "\"\nctrl_domain = 88\nshutdown_timeout_ms = 5000\n\n"
          << "[[component]]\nname = \"c_node\"\ngroup = \"all\"\nlib = \"" << escape_toml(plugin_path->generic_string())
          << "\"\nclass = \"ReusableTestNode\"\n[component.params]\nrate_hz = 15\nlabel = \"${arg.rate}\"\n"
          << "[component.ready]\ntype = \"component\"\ntimeout_ms = 5000\n\n"
          << "[[process]]\nname = \"p_node\"\ngroup = \"all\"\ndepends_on = [\"c_node\"]\ncmd = \""
          << escape_toml(standalone_bin.generic_string())
          << "\"\n[process.ready]\ntype = \"notify\"\ntimeout_ms = 5000\n";
    }

    CHECK(run_cli_cmd(cli, {"validate", manifest_path, "--arg=rate=30", "--check-programs"}, out) == 0);
    CHECK(out.find("Manifest valid.") != std::string::npos);
    CHECK(run_cli_cmd(cli, {"run", manifest_path, "--dry-run", "--only=c_node"}, out) == 0);
    CHECK(out.find("1. c_node") != std::string::npos);
    CHECK(out.find("p_node") == std::string::npos);

    vlink::Process srv_proc;
    srv_proc.set_inherit_environment(true);
    srv_proc.set_process_mode(vlink::Process::kMergedMode);
    srv_proc.start(cli, {"run", manifest_path, "--arg", "rate=30"});
    REQUIRE(srv_proc.wait_for_started(3000));

    // Wait until status command sees both units ready.
    const std::vector<std::string> ctrl_args = {"--scheme", "zenoh", "--ns", cli_ns, "--domain", "88"};
    auto with_ctrl = [&](std::vector<std::string> base) {
      base.insert(base.end(), ctrl_args.begin(), ctrl_args.end());
      return base;
    };

    REQUIRE(wait_until(
        [&]() {
          return run_cli_cmd(cli, with_ctrl({"status"}), out, 4000) == 0 && out.find("c_node") != std::string::npos &&
                 out.find("p_node") != std::string::npos && out.find("ready") != std::string::npos;
        },
        8000));

    CHECK(run_cli_cmd(cli, with_ctrl({"params", "c_node"}), out) == 0);
    CHECK(out.find("rate_hz") != std::string::npos);
    CHECK(run_cli_cmd(cli, with_ctrl({"set-param", "c_node", "rate_hz=42"}), out) == 0);
    CHECK(run_cli_cmd(cli, with_ctrl({"set-param", "c_node", "flag=true"}), out) == 0);
    CHECK(run_cli_cmd(cli, with_ctrl({"set-param", "c_node", "gain=1.25"}), out) == 0);
    CHECK(run_cli_cmd(cli, with_ctrl({"set-param", "c_node", "tag=cli_ok"}), out) == 0);
    CHECK(run_cli_cmd(cli, with_ctrl({"reload", "c_node"}), out) == 0);
    CHECK(run_cli_cmd(cli, with_ctrl({"restart", "c_node"}), out) == 0);
    CHECK(run_cli_cmd(cli, with_ctrl({"stop", "p_node"}), out) == 0);
    CHECK(run_cli_cmd(cli, with_ctrl({"start", "p_node"}), out) == 0);

    // Stream live events via `vlink-launcher events`, trigger reload until event line arrives, then terminate.
    vlink::Process ev_proc;
    ev_proc.set_inherit_environment(true);
    ev_proc.set_process_mode(vlink::Process::kMergedMode);
    ev_proc.start(cli, with_ctrl({"events"}));
    REQUIRE(ev_proc.wait_for_started(3000));
    std::string ev_output;
    auto last_trigger = std::chrono::steady_clock::now() - std::chrono::milliseconds(400);
    CHECK(wait_until(
        [&]() {
          const auto now = std::chrono::steady_clock::now();
          if (now - last_trigger >= std::chrono::milliseconds(600)) {
            last_trigger = now;
            (void)run_cli_cmd(cli, with_ctrl({"reload", "c_node"}), out, 3000);
          }
          const size_t avail = ev_proc.bytes_available_stdout();
          if (avail > 0U) {
            std::vector<uint8_t> buf;
            (void)ev_proc.read_stdout(buf, avail);
            ev_output.append(reinterpret_cast<const char*>(buf.data()), buf.size());
          }
          return ev_output.find("c_node") != std::string::npos && ev_output.find("on_reloaded") != std::string::npos;
        },
        8000));
    ev_proc.terminate();
    (void)ev_proc.wait_for_finished(3000);

    CHECK(run_cli_cmd(cli, with_ctrl({"shutdown"}), out) == 0);
    REQUIRE(srv_proc.wait_for_finished(6000));
    CHECK(srv_proc.get_exit_code() == 0);
#endif
  }

  TEST_CASE("TOML enabled flag, params_file (.toml/.json), URL remaps, resolve_url & large params spill") {
    const std::string app_dir = vlink::Utils::get_app_dir();
    const std::filesystem::path dir(app_dir);
#if defined(_WIN32) || defined(_WIN64)
    const std::filesystem::path standalone_bin = dir / "launcher_test_standalone.exe";
    const std::filesystem::path launcher_bin = dir / "vlink-launcher.exe";
#else
    const std::filesystem::path standalone_bin = dir / "launcher_test_standalone";
    const std::filesystem::path launcher_bin = dir / "vlink-launcher";
#endif
    const std::string tmp = make_temp_dir("enabled_params_remap");

    // 1) enabled flag on [[include]], [[process]], [[component]] (native bool, ${arg.*}, !${arg.*}).
    {
      {
        std::ofstream ofs(tmp + "/sim_sub.toml");
        ofs << "[[process]]\nname = \"sim_node\"\ncmd = \"" << escape_toml(standalone_bin.generic_string()) << "\"\n";
      }
      {
        std::ofstream ofs(tmp + "/hw_sub.toml");
        ofs << "[[process]]\nname = \"hw_node\"\ncmd = \"" << escape_toml(standalone_bin.generic_string()) << "\"\n";
      }
      const std::string root = tmp + "/cond.toml";
      {
        std::ofstream ofs(root);
        ofs << "[args]\nuse_sim = \"false\"\n\n"
            << "[launcher]\nname = \"cond\"\nctrl_enabled = false\n\n"
            << "[[include]]\nfile = \"sim_sub.toml\"\nenabled = \"${arg.use_sim}\"\n\n"
            << "[[include]]\nfile = \"hw_sub.toml\"\nenabled = \"!${arg.use_sim}\"\n\n"
            << "[[process]]\nname = \"always_off\"\nenabled = false\ncmd = \""
            << escape_toml(standalone_bin.generic_string()) << "\"\n";
      }

      vlink::LauncherServer::Config cfg;
      cfg.enable_control_plane = false;
      std::string err;

      // Default use_sim=false => hw_node enabled, sim_node & always_off skipped.
      {
        vlink::LauncherServer srv(cfg);
        REQUIRE(srv.load_file(root, &err));
        CHECK(srv.startup_order() == std::vector<std::string>{"hw_node"});
      }

      // Override use_sim=true => sim_node enabled, hw_node skipped.
      cfg.args_override["use_sim"] = "true";
      {
        vlink::LauncherServer srv(cfg);
        REQUIRE(srv.load_file(root, &err));
        CHECK(srv.startup_order() == std::vector<std::string>{"sim_node"});
      }

      // Invalid enabled expression must fail load.
      {
        vlink::LauncherServer srv(cfg);
        CHECK_FALSE(srv.load_string("[[process]]\nname = \"bad\"\ncmd = \"x\"\nenabled = \"maybe\"\n", tmp, &err));
        CHECK(err.find("enabled") != std::string::npos);
      }
    }

    // 2) params_file (.toml + .json overlay + inline precedence), resolve_url() & remaps,
    //    >16 KiB params spill to VLINK_LAUNCHER_PARAMS_FILE, and short plugin name via VLINK_LAUNCHER_PLUGIN_DIRS.
    {
      {
        std::ofstream ofs(tmp + "/base_params.toml");
        ofs << "[params]\nrate_hz = 11\nlabel = \"from_toml_${arg.env_tag}\"\n"
            << "probe_url = \"dds://sensor/lidar/event/0\"\n";
      }
      {
        std::ofstream ofs(tmp + "/override_params.json");
        ofs << "{\"rate_hz\": 33, \"json_only\": \"ok_${arg.env_tag}\"}\n";
      }

      const std::string large_str(20000U, 'Z');
      {
        std::ofstream ofs(tmp + "/large_params.json");
        ofs << "{\"rate_hz\": 44, \"probe_url\": \"dds://sensor/lidar/event/0\", \"large_blob\": \"" << large_str
            << "\"}\n";
      }

      const std::string m_inproc_auto = tmp + "/url_inproc_auto.log";
      const std::string m_inproc_remap = tmp + "/url_inproc_remap.log";
      const std::string m_proc_auto = tmp + "/url_proc_auto.log";
      const std::string m_proc_remap = tmp + "/url_proc_remap.log";
      const std::string m_exec_short = tmp + "/url_exec_short.log";

      {
        std::ofstream ofs(tmp + "/sub_remap.toml");
        ofs << "[[process]]\nname = \"proc_remap\"\ncmd = \"" << escape_toml(standalone_bin.generic_string()) << "\"\n"
            << "params_file = [\"base_params.toml\", \"override_params.json\"]\n"
            << "[process.params]\nmarker_file = \"" << escape_toml(m_proc_remap) << "\"\nlabel = \"inline_wins\"\n"
            << "[process.ready]\ntype = \"notify\"\ntimeout_ms = 5000\n";
      }

      std::string root_toml = R"(
[args]
env_tag = "prod"

[launcher]
name = "url-remap-demo"
ctrl_enabled = false
plugin_dirs = [")" + escape_toml(app_dir) +
                              R"("]

[[include]]
file = "sub_remap.toml"
[include.remaps]
"dds://sensor/lidar/event/0" = "shm2://front/lidar/event/0"

[[process]]
name = "proc_auto_large"
cmd = ")" + escape_toml(standalone_bin.generic_string()) +
                              R"("
params_file = "large_params.json"
[process.params]
marker_file = ")" + escape_toml(m_proc_auto) +
                              R"("
[process.ready]
type = "notify"
timeout_ms = 5000
)";

#ifdef VLINK_TEST_HAS_LAUNCHER_PLUGIN
      root_toml += R"(
[[component]]
name = "comp_auto"
lib = "launcher_test_plugin"
class = "ReusableTestNode"
params_file = "base_params.toml"
[component.params]
marker_file = ")" + escape_toml(m_inproc_auto) +
                   R"("
[component.ready]
type = "component"
timeout_ms = 5000

[[component]]
name = "comp_remap"
lib = "launcher_test_plugin"
class = "ReusableTestNode"
params_file = ["base_params.toml", "override_params.json"]
[component.params]
marker_file = ")" + escape_toml(m_inproc_remap) +
                   R"("
[component.remaps]
"dds://sensor/lidar/event/0" = "shm2://front/lidar/event/0"
[component.ready]
type = "component"
timeout_ms = 5000

[[process]]
name = "exec_short_lib"
cmd = ")" + escape_toml(launcher_bin.generic_string()) +
                   R"("
args = ["exec", "launcher_test_plugin", "ReusableTestNode", "--remap", "dds://sensor/lidar/event/0=zenoh://exec/lidar/event/0"]
[process.params]
probe_url = "dds://sensor/lidar/event/0"
marker_file = ")" + escape_toml(m_exec_short) +
                   R"("
[process.ready]
type = "notify"
timeout_ms = 5000
)";
#endif

      vlink::LauncherServer::Config cfg;
      cfg.enable_control_plane = false;
      vlink::LauncherServer srv(cfg);
      std::string err;
      REQUIRE(srv.load_string(root_toml, tmp, &err));
      REQUIRE(srv.start());

      const size_t expected_ready =
#ifdef VLINK_TEST_HAS_LAUNCHER_PLUGIN
          5U;
#else
          2U;
#endif
      REQUIRE(wait_until(
          [&]() {
            size_t cnt = 0U;

            for (const auto& u : srv.get_snapshot().units) {
              if (u.state == vlink::launcher::UnitState::kReady) {
                ++cnt;
              }
            }

            return cnt == expected_ready;
          },
          10000));

      // Standalone without remap keeps dds:// AND receives 20000-byte spilled param via VLINK_LAUNCHER_PARAMS_FILE.
      const std::string proc_auto_log = read_file_all(m_proc_auto);
      CHECK(proc_auto_log.find("URL:dds://sensor/lidar/event/0") != std::string::npos);
      CHECK(proc_auto_log.find("LARGE_LEN:20000") != std::string::npos);

      // Standalone via [[include]] inherits [include.remaps], JSON overrides TOML rate_hz=33, inline label wins.
      const std::string proc_remap_log = read_file_all(m_proc_remap);
      CHECK(proc_remap_log.find("URL:shm2://front/lidar/event/0") != std::string::npos);
      CHECK(proc_remap_log.find(":rate=33:label=inline_wins:") != std::string::npos);

#ifdef VLINK_TEST_HAS_LAUNCHER_PLUGIN
      // In-process without remap automatically rewrites dds:// -> intra://.
      const std::string inproc_auto_log = read_file_all(m_inproc_auto);
      CHECK(inproc_auto_log.find("URL:intra://sensor/lidar/event/0") != std::string::npos);
      CHECK(inproc_auto_log.find(":rate=11:label=from_toml_prod:") != std::string::npos);

      // In-process with explicit [component.remaps] returns remapped URL verbatim.
      const std::string inproc_remap_log = read_file_all(m_inproc_remap);
      CHECK(inproc_remap_log.find("URL:shm2://front/lidar/event/0") != std::string::npos);
      CHECK(inproc_remap_log.find(":rate=33:") != std::string::npos);

      // `vlink-launcher exec launcher_test_plugin` resolves short lib via VLINK_LAUNCHER_PLUGIN_DIRS + applies --remap.
      const std::string exec_log = read_file_all(m_exec_short);
      CHECK(exec_log.find("URL:zenoh://exec/lidar/event/0") != std::string::npos);
#endif

      srv.stop();
      CHECK(srv.wait(5000));

      // Missing params_file must fail load.
      vlink::LauncherServer bad_pf(cfg);
      CHECK_FALSE(
          bad_pf.load_string("[[process]]\nname = \"x\"\ncmd = \"y\"\nparams_file = \"no_such.toml\"\n", tmp, &err));
      CHECK(err.find("params_file") != std::string::npos);
    }
  }

#ifdef VLINK_TEST_HAS_LAUNCHER_PLUGIN
  TEST_CASE("vlink-launcher host isolated component group: fault isolation, aggregated notify/watchdog/reload") {
    const std::string app_dir = vlink::Utils::get_app_dir();
    const std::filesystem::path dir(app_dir);
#if defined(_WIN32) || defined(_WIN64)
    const std::filesystem::path launcher_bin = dir / "vlink-launcher.exe";
#else
    const std::filesystem::path launcher_bin = dir / "vlink-launcher";
#endif
    std::error_code ec;
    REQUIRE(std::filesystem::exists(launcher_bin, ec));

    const std::string tmp = make_temp_dir("host_group");
    const std::string m_a = tmp + "/host_a.log";
    const std::string m_b = tmp + "/host_b.log";
    const std::string group_toml = tmp + "/perception_group.toml";

    {
      std::ofstream ofs(group_toml);
      ofs << "[launcher]\nname = \"perception-host\"\nplugin_dirs = [\"" << escape_toml(app_dir) << "\"]\n\n"
          << "[[component]]\nname = \"host_a\"\nlib = \"launcher_test_plugin\"\nclass = \"ReusableTestNode\"\n"
          << "[component.params]\nlabel = \"ha\"\nprobe_url = \"dds://perception/cam/event/0\"\nmarker_file = \""
          << escape_toml(m_a) << "\"\n[component.ready]\ntype = \"component\"\ntimeout_ms = 5000\n\n"
          << "[[component]]\nname = \"host_b\"\ndepends_on = [\"host_a\"]\nlib = \"launcher_test_plugin\"\n"
          << "class = \"ReusableTestNode\"\n[component.params]\nlabel = \"hb\"\n"
          << "probe_url = \"dds://perception/cam/event/0\"\nmarker_file = \"" << escape_toml(m_b)
          << "\"\n[component.ready]\ntype = \"component\"\ntimeout_ms = 5000\n";
    }

    const std::string parent_toml = R"(
[launcher]
name = "parent-orch"
ctrl_enabled = false

[[process]]
name = "perception_host"
cmd = ")" + escape_toml(launcher_bin.generic_string()) +
                                    R"("
args = ["host", ")" + escape_toml(group_toml) +
                                    R"("]
watchdog_ms = 1200
[process.ready]
type = "notify"
timeout_ms = 6000
)";

    vlink::LauncherServer::Config cfg;
    cfg.enable_control_plane = false;
    vlink::LauncherServer srv(cfg);
    std::string err;
    REQUIRE(srv.load_string(parent_toml, tmp, &err));
    REQUIRE(srv.start());

    REQUIRE(wait_until(
        [&]() {
          const auto snap = srv.get_snapshot();
          return snap.units.size() == 1U && snap.units[0].state == vlink::launcher::UnitState::kReady;
        },
        8000));

    const auto snap = srv.get_snapshot();
    REQUIRE(snap.units.size() == 1U);
    CHECK(snap.units[0].detail == "host ready (2 unit(s))");
    CHECK(snap.units[0].pid > 0);
    CHECK(snap.units[0].pid != snap.server_pid);

    // Both components live inside the isolated host subprocess (same PID == host_pid) and auto-use intra://.
    const std::string expected_pid_tag = ":pid=" + std::to_string(snap.units[0].pid);
    CHECK(read_file_all(m_a).find(expected_pid_tag) != std::string::npos);
    CHECK(read_file_all(m_b).find(expected_pid_tag) != std::string::npos);
    CHECK(read_file_all(m_a).find("URL:intra://perception/cam/event/0") != std::string::npos);
    CHECK(read_file_all(m_b).find("URL:intra://perception/cam/event/0") != std::string::npos);

    // Stay healthy beyond watchdog_ms=1200 via aggregated host watchdog pings.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(srv.get_snapshot().units[0].state == vlink::launcher::UnitState::kReady);
    CHECK(srv.get_snapshot().units[0].healthy);

#if !defined(_WIN32) && !defined(_WIN64)
    // Reloading the host process sends SIGHUP -> host reloads both hosted components.
    CHECK(srv.reload_unit("perception_host").ok());
    CHECK(wait_until([&]() { return read_file_all(m_a).find("RELOADED") != std::string::npos; }, 4000));
    CHECK(wait_until([&]() { return read_file_all(m_b).find("RELOADED") != std::string::npos; }, 4000));
#endif

    srv.stop();
    CHECK(srv.wait(6000));
#if !defined(_WIN32) && !defined(_WIN64)
    CHECK(read_file_all(m_a).find("STOPPED") != std::string::npos);
    CHECK(read_file_all(m_b).find("STOPPED") != std::string::npos);
#endif
  }
#endif

  TEST_CASE("scheduler, queue/strategy, LauncherTimerComponent & C++20 LauncherCoroutineComponent") {
    const std::string app_dir = vlink::Utils::get_app_dir();
    const std::filesystem::path dir(app_dir);
#if defined(_WIN32) || defined(_WIN64)
    const std::filesystem::path standalone_bin = dir / "launcher_test_standalone.exe";
    const std::filesystem::path launcher_bin = dir / "vlink-launcher.exe";
#else
    const std::filesystem::path standalone_bin = dir / "launcher_test_standalone";
    const std::filesystem::path launcher_bin = dir / "vlink-launcher";
#endif
    const std::string tmp = make_temp_dir("sched_queue_timer_coro");

    // 1) Negative TOML validation for scheduler/queue/threads.
    {
      vlink::LauncherServer::Config cfg;
      cfg.enable_control_plane = false;
      std::string err;

      const std::vector<std::string> bad_manifests = {
          "[[component]]\nname = \"c\"\nlib = \"l\"\nclass = \"C\"\nscheduler = \"bogus\"\n",
          "[[component]]\nname = \"c\"\nlib = \"l\"\nclass = \"C\"\nqueue = \"bogus\"\n",
          "[[component]]\nname = \"c\"\nlib = \"l\"\nclass = \"C\"\nqueue_strategy = \"bogus\"\n",
          "[[component]]\nname = \"c\"\nlib = \"l\"\nclass = \"C\"\ndedicated = true\nscheduler = \"multi\"\n",
          "[[component]]\nname = \"c\"\nlib = \"l\"\nclass = \"C\"\ndedicated = false\nscheduler = \"dedicated\"\n",
          "[[component]]\nname = \"c\"\nlib = \"l\"\nclass = \"C\"\nscheduler = \"multi\"\nthreads = 0\n",
          "[[component]]\nname = \"c\"\nlib = \"l\"\nclass = \"C\"\nscheduler = \"multi\"\nthreads = 1\n",
          "[[component]]\nname = \"c\"\nlib = \"l\"\nclass = \"C\"\nscheduler = \"multi\"\nthreads = 99\n",
      };

      for (const auto& m : bad_manifests) {
        vlink::LauncherServer s(cfg);
        CHECK_FALSE(s.load_string(m, tmp, &err));
        CHECK(!err.empty());
      }
    }

    // 2) Live runtime verification: shared lockfree/pop, multi priority/block, standalone multi lockfree/pop,
    //    LauncherTimerComponent (inproc + hot period_ms + standalone), and C++20 LauncherCoroutineComponent.
    const std::string m_shared_lf = tmp + "/shared_lf.log";
    const std::string m_multi_prio = tmp + "/multi_prio.log";
    const std::string m_proc_multi = tmp + "/proc_multi.log";
    const std::string m_timer_inproc = tmp + "/timer_inproc.log";
    const std::string m_timer_exec = tmp + "/timer_exec.log";
    const std::string m_coro_inproc = tmp + "/coro_inproc.log";
    const std::string m_coro_exec = tmp + "/coro_exec.log";

    std::string toml = R"(
[launcher]
name = "sched-demo"
ctrl_enabled = false
plugin_dirs = [")" + escape_toml(app_dir) +
                       R"("]

[[process]]
name = "proc_multi_lf"
cmd = ")" + escape_toml(standalone_bin.generic_string()) +
                       R"("
scheduler = "multi"
threads = 3
queue = "lockfree"
queue_strategy = "pop"
[process.params]
marker_file = ")" + escape_toml(m_proc_multi) +
                       R"("
[process.ready]
type = "notify"
timeout_ms = 5000
)";

    size_t expected_ready = 1U;

#ifdef VLINK_TEST_HAS_LAUNCHER_PLUGIN
    toml += R"(
[[component]]
name = "comp_shared_lf"
lib = "launcher_test_plugin"
class = "ReusableTestNode"
scheduler = "shared"
queue = "lockfree"
queue_strategy = "pop"
[component.params]
marker_file = ")" +
            escape_toml(m_shared_lf) + R"("
[component.ready]
type = "component"
timeout_ms = 5000

[[component]]
name = "comp_multi_prio"
lib = "launcher_test_plugin"
class = "ReusableTestNode"
scheduler = "multi"
threads = 3
queue = "priority"
queue_strategy = "block"
[component.params]
marker_file = ")" +
            escape_toml(m_multi_prio) + R"("
[component.ready]
type = "component"
timeout_ms = 5000

[[component]]
name = "timer_inproc"
lib = "launcher_test_plugin"
class = "TimerTestNode"
watchdog_ms = 800
[component.params]
period_ms = 50
marker_file = ")" +
            escape_toml(m_timer_inproc) + R"("
[component.ready]
type = "component"
timeout_ms = 5000

[[process]]
name = "timer_exec"
cmd = ")" + escape_toml(launcher_bin.generic_string()) +
            R"("
args = ["exec", "launcher_test_plugin", "TimerTestNode"]
watchdog_ms = 800
[process.params]
period_ms = 50
marker_file = ")" +
            escape_toml(m_timer_exec) + R"("
[process.ready]
type = "notify"
timeout_ms = 5000
)";
    expected_ready += 4U;

#ifdef VLINK_ENABLE_COROUTINE
    toml += R"(
[[component]]
name = "coro_inproc"
lib = "launcher_test_plugin"
class = "CoroTestNode"
watchdog_ms = 800
[component.params]
marker_file = ")" +
            escape_toml(m_coro_inproc) + R"("
[component.ready]
type = "component"
timeout_ms = 5000

[[process]]
name = "coro_exec"
cmd = ")" + escape_toml(launcher_bin.generic_string()) +
            R"("
args = ["exec", "launcher_test_plugin", "CoroTestNode"]
watchdog_ms = 800
[process.params]
marker_file = ")" +
            escape_toml(m_coro_exec) + R"("
[process.ready]
type = "notify"
timeout_ms = 5000
)";
    expected_ready += 2U;
#endif
#endif

    vlink::LauncherServer::Config cfg;
    cfg.enable_control_plane = false;
    vlink::LauncherServer srv(cfg);
    std::string err;
    REQUIRE(srv.load_string(toml, tmp, &err));
    REQUIRE(srv.start());

    auto all_ready = [&]() {
      size_t cnt = 0U;

      for (const auto& u : srv.get_snapshot().units) {
        if (u.state == vlink::launcher::UnitState::kReady) {
          ++cnt;
        }
      }

      return cnt == expected_ready;
    };
    REQUIRE(wait_until(all_ready, 10000));

    // Verify queue type / strategy / worker threads parity across in-process and standalone.
    CHECK(read_file_all(m_proc_multi).find(":qtype=1:qstrat=1:threads=3") != std::string::npos);

#ifdef VLINK_TEST_HAS_LAUNCHER_PLUGIN
    CHECK(read_file_all(m_shared_lf).find(":qtype=1:qstrat=1:threads=1") != std::string::npos);
    CHECK(read_file_all(m_multi_prio).find(":qtype=2:qstrat=2:threads=3") != std::string::npos);

    // TimerTestNode fires periodically in both modes & supports live period_ms update.
    CHECK(wait_until(
        [&]() { return read_file_all(m_timer_inproc).find("TICK:3:period=50:standalone=0") != std::string::npos; },
        3000));
    CHECK(wait_until(
        [&]() { return read_file_all(m_timer_exec).find("TICK:3:period=50:standalone=1") != std::string::npos; },
        3000));
    CHECK_FALSE(srv.set_param("timer_inproc", vlink::launcher::param_value_to_entry("period_ms", int64_t{0})).ok());
    CHECK(srv.set_param("timer_inproc", vlink::launcher::param_value_to_entry("period_ms", int64_t{25})).ok());
    CHECK(wait_until(
        [&]() { return read_file_all(m_timer_inproc).find(":period=25:standalone=0") != std::string::npos; }, 3000));

#ifdef VLINK_ENABLE_COROUTINE
    CHECK(wait_until(
        [&]() { return read_file_all(m_coro_inproc).find("CORO_STEP:3:standalone=0") != std::string::npos; }, 3000));
    CHECK(wait_until([&]() { return read_file_all(m_coro_exec).find("CORO_STEP:3:standalone=1") != std::string::npos; },
                     3000));
#endif
#endif

    srv.stop();
    CHECK(srv.wait(6000));

#ifdef VLINK_TEST_HAS_LAUNCHER_PLUGIN
    CHECK(read_file_all(m_timer_inproc).find("TIMER_STOP") != std::string::npos);
#if !defined(_WIN32) && !defined(_WIN64)
    CHECK(read_file_all(m_timer_exec).find("TIMER_STOP") != std::string::npos);
#endif
#ifdef VLINK_ENABLE_COROUTINE
    CHECK(read_file_all(m_coro_inproc).find("CORO_DONE:") != std::string::npos);
#if !defined(_WIN32) && !defined(_WIN64)
    CHECK(read_file_all(m_coro_exec).find("CORO_DONE:") != std::string::npos);
#endif
#endif
#endif
  }
}

// NOLINTEND
