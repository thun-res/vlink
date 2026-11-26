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
#include <vlink/base/coroutine.h>
#include <vlink/base/logger_plugin_interface.h>
#include <vlink/base/plugin.h>
#include <vlink/base/utils.h>
#include <vlink/extension/bag_reader.h>
#include <vlink/external/launcher_api.h>
#include <vlink/external/launcher_server.h>
#include <vlink/publisher.h>
#include <vlink/subscriber.h>
//
#include <nlohmann/json.hpp>
//
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32) && !defined(_WIN64)
#include <csignal>
#endif

static std::string make_temp_dir(const std::string& suffix) {
  const auto dir = std::filesystem::path(vlink::Utils::get_tmp_dir()) /
                   ("vlink_launcher_ut_" + vlink::Utils::get_pid_str() + "_" + suffix);
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.generic_string();
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

TEST_SUITE("launcher") {
  TEST_CASE("param encode/decode lossless double and list roundtrip") {
    const double precise = 1.0000001;
    const auto d_entry = vlink::launcher::encode_param_entry("precision", precise);
    vlink::LauncherParamValue d_out;
    REQUIRE(vlink::launcher::decode_param_entry(d_entry, d_out));
    CHECK(std::abs(std::get<double>(d_out) - precise) < 1e-12);

    const std::vector<int64_t> ints{10, -20, 30};
    const auto i_entry = vlink::launcher::encode_param_entry("ints", ints);
    vlink::LauncherParamValue i_out;
    REQUIRE(vlink::launcher::decode_param_entry(i_entry, i_out));
    CHECK(std::get<std::vector<int64_t>>(i_out) == ints);

    const std::vector<std::string> strs{"alpha", "beta"};
    const auto s_entry = vlink::launcher::encode_param_entry("strs", strs);
    vlink::LauncherParamValue s_out;
    REQUIRE(vlink::launcher::decode_param_entry(s_entry, s_out));
    CHECK(std::get<std::vector<std::string>>(s_out) == strs);
  }

  TEST_CASE("manifest parsing: systemd deps, slices, groups, conditions, choices, conflicts, cycle detection") {
    const std::string valid_toml = R"(
[launcher]
name = "ut-manifest"
namespace = "nav"
startup = "max_parallel"
max_parallel = 2

[args.profile]
default = "sim"
choices = ["sim", "real"]
desc = "Runtime profile"

[env]
GLOBAL_ENV = "ok"

[remaps]
"intra://nav/in" = "intra://nav/remapped"

[[slice]]
name = "core"
cpu_quota_percent = 200
mem_max_mb = 512
mem_high_mb = 256
pids_max = 64

[[group]]
name = "bringup"
mode = "serial"
slice = "core"

[[scheduler]]
name = "compute"
mode = "pool"
queue = "lockfree"
threads = 2

[[process]]
name = "a"
group = "bringup"
stage = 0
cmd = "/bin/echo"
args = ["a"]
condition = "${arg.profile} == sim && !false"

[[process]]
name = "b"
group = "bringup"
stage = 1
cmd = "/bin/echo"
args = ["b"]
after = ["a"]
requires = ["a"]
wants = ["c"]

[[process]]
name = "c"
cmd = "/bin/echo"
args = ["c"]
autostart = false
)";

    vlink::LauncherServer::Config cfg;
    cfg.enable_control_plane = false;
    cfg.manage_console = false;
    vlink::LauncherServer server(cfg);
    std::string err;
    REQUIRE(server.load_string(valid_toml, ".", &err));

    const auto rep = server.validate(false);
    CHECK(rep.valid);
    CHECK(rep.slice_count == 1U);
    CHECK(rep.group_count == 1U);
    CHECK(rep.process_count == 3U);
    CHECK(rep.scheduler_group_count == 1U);

    CHECK(server.export_graph(vlink::LauncherServer::GraphFormat::kDot).find("digraph") != std::string::npos);
    CHECK(server.export_graph(vlink::LauncherServer::GraphFormat::kAscii).find("nav/a") != std::string::npos);
    CHECK(server.export_graph(vlink::LauncherServer::GraphFormat::kMermaid).find("flowchart TD") != std::string::npos);
    CHECK(server.inspect_manifest_json().find("ut-manifest") != std::string::npos);

    const std::string bad_choice_toml = R"(
[args.profile]
default = "sim"
choices = ["sim", "real"]
)";
    vlink::LauncherServer::Config bad_cfg;
    bad_cfg.enable_control_plane = false;
    bad_cfg.manage_console = false;
    bad_cfg.arg_overrides["profile"] = "invalid";
    vlink::LauncherServer bad_srv(bad_cfg);
    CHECK_FALSE(bad_srv.load_string(bad_choice_toml, ".", &err));

    const std::string cycle_toml = R"(
[[process]]
name = "x"
cmd = "/bin/echo"
after = ["y"]

[[process]]
name = "y"
cmd = "/bin/echo"
after = ["x"]
)";
    vlink::LauncherServer cycle_srv(cfg);
    CHECK_FALSE(cycle_srv.load_string(cycle_toml, ".", &err));

    const std::string conflict_toml = R"(
[[process]]
name = "p1"
cmd = "/bin/echo"
conflicts = ["p2"]

[[process]]
name = "p2"
cmd = "/bin/echo"
)";
    vlink::LauncherServer conf_srv(cfg);
    CHECK_FALSE(conf_srv.load_string(conflict_toml, ".", &err));

    const std::string bad_enum_toml = R"(
[[process]]
name = "p1"
cmd = "/bin/echo"
output = "bogus_output"
)";
    vlink::LauncherServer enum_srv(cfg);
    CHECK_FALSE(enum_srv.load_string(bad_enum_toml, ".", &err));
  }

  TEST_CASE("in-process components, thread_pool, coroutine, reload, isolate, logs and LauncherAPI control plane") {
    const std::string tmp = make_temp_dir("comp_ctl");
    const std::string plugin_dir = vlink::Utils::get_app_dir();
    const std::string ctrl_ns = "ut_lch_" + vlink::Utils::get_pid_str();

    const auto pinfo = vlink::LauncherServer::inspect_plugin("launcher_test_plugin");
    CHECK(pinfo.loaded);
    CHECK(pinfo.is_launcher_module);

    const std::string manifest = R"(
[launcher]
name = "ut-components"
startup = "parallel"
ctrl_scheme = "intra"
ctrl_namespace = ")" + ctrl_ns +
                                 R"("
plugin_dirs = [")" + plugin_dir + R"("]

[logger]
dir = ")" + tmp + R"("
history = 512

[[group]]
name = "core"
mode = "serial"

[[scheduler]]
name = "choreo"
mode = "pinned"
queue = "priority"
threads = 2

[[scheduler]]
name = "compute_lf"
mode = "pool"
queue = "lockfree"
threads = 2

[[component]]
name = "listener"
group = "core"
lib = "launcher_test_plugin"
class = "TestListener"
scheduler = "choreo"
worker = 0
[component.params]
topic = "intra://ut/launcher/msg"
tags = ["t1", "t2"]
[component.ready]
type = "component"
timeout_ms = 3000

[[component]]
name = "talker"
group = "core"
lib = "launcher_test_plugin"
class = "TestTalker"
scheduler = "choreo"
pool = "compute_lf"
worker = 1
watchdog_ms = 5000
reload = "component"
depends_on = ["listener"]
[component.params]
topic = "intra://ut/launcher/msg"
interval_ms = 40
prefix = "hello"

[[component]]
name = "legacy_worker"
lib = "launcher_legacy_plugin"
legacy = true
depends_on = ["listener"]
)";

    vlink::LauncherServer::Config srv_cfg;
    srv_cfg.manage_console = false;
    srv_cfg.log_dir_override = tmp;

    vlink::LauncherServer server(srv_cfg);
    std::string err;
    REQUIRE(server.load_string(manifest, tmp, &err));
    REQUIRE(server.start());

    REQUIRE(wait_until(
        [&]() {
          const auto snap = server.get_snapshot();
          size_t ready_cnt = 0U;

          for (const auto& u : snap.units) {
            if (u.state == vlink::launcher::UnitState::kReady) {
              ++ready_cnt;
            }
          }

          return ready_cnt == 3U;
        },
        4000));

    vlink::LauncherAPI::Config api_cfg;
    api_cfg.scheme = "intra";
    api_cfg.namespace_prefix = ctrl_ns;
    api_cfg.subscribe_events = true;
    api_cfg.subscribe_logs = true;

    vlink::LauncherAPI api(api_cfg);
    api.async_run();
    REQUIRE(api.wait_for_connected(3000U));

    const auto status_res = api.get_status(3000U);
    REQUIRE(status_res.ok());
    CHECK(status_res.snapshot.units.size() == 3U);

    const auto reload_res = api.reload_unit("talker", 3000U);
    CHECK(reload_res.ok());

    const auto cat_res = api.get_unit_config("talker", 3000U);
    CHECK(cat_res.ok());
    CHECK(cat_res.message.find("TestTalker") != std::string::npos);

    vlink::launcher::ParamEntry upd;
    upd.key = "prefix";
    upd.type = vlink::launcher::ParamType::kString;
    upd.value = "updated";
    CHECK(api.set_param("talker", upd, 3000U).ok());

    vlink::launcher::ParamEntry reject_upd;
    reject_upd.key = "reject_me";
    reject_upd.type = vlink::launcher::ParamType::kBool;
    reject_upd.value = "true";
    CHECK_FALSE(api.set_param("talker", reject_upd, 3000U).ok());

    vlink::launcher::LoadComponentRequest fail_req;
    fail_req.name = "failing_comp";
    fail_req.library = "launcher_test_plugin";
    fail_req.class_name = "TestFailOnStart";
    fail_req.auto_start = true;
    CHECK_FALSE(api.load_component(fail_req, 3000U).ok());

#ifdef VLINK_ENABLE_COROUTINE
    vlink::launcher::LoadComponentRequest coro_req;
    coro_req.name = "coro_worker";
    coro_req.library = "launcher_test_plugin";
    coro_req.class_name = "TestCoro";
    coro_req.auto_start = true;
    REQUIRE(api.load_component(coro_req, 3000U).ok());
    CHECK(api.unload_component("coro_worker", 3000U).ok());
#endif

    const auto iso_res = api.isolate_group("@core", 3000U);
    CHECK(iso_res.ok());

    const auto snap_after_iso = api.get_status(3000U);
    REQUIRE(snap_after_iso.ok());

    for (const auto& u : snap_after_iso.snapshot.units) {
      if (u.name == "legacy_worker") {
        CHECK(u.state == vlink::launcher::UnitState::kStopped);
      }
    }

    CHECK(api.shutdown_server(3000U).ok());
    CHECK(server.wait(4000));
  }

#if !defined(_WIN32) && !defined(_WIN64)
  TEST_CASE("process supervision: stdout probe, serial mode, start_limit circuit breaker and binds_to cascade") {
    const std::string tmp = make_temp_dir("proc_sup");

    const std::string manifest = R"(
[launcher]
name = "ut-process"
startup = "serial"
ctrl = false

[logger]
dir = ")" + tmp + R"("

[[task]]
name = "init_task"
kind = "oneshot"
cmd = "/bin/sh"
args = ["-c", "echo INIT_OK"]
output = "log"

[[process]]
name = "svc_main"
cmd = "/bin/sh"
args = ["-c", "echo SVC_READY && sleep 30"]
depends_on = ["init_task"]
output = "log"
[process.ready]
type = "stdout"
pattern = "SVC_READY"
timeout_ms = 3000

[[process]]
name = "svc_bound"
cmd = "/bin/sh"
args = ["-c", "sleep 30"]
after = ["svc_main"]
binds_to = ["svc_main"]
output = "log"

[[process]]
name = "flaky"
cmd = "/bin/sh"
args = ["-c", "exit 2"]
after = ["svc_bound"]
restart = "on_failure"
restart_delay_ms = 50
restart_max_ms = 50
restart_backoff = 1.0
start_burst = 2
start_window_ms = 60000
output = "log"
)";

    vlink::LauncherServer::Config cfg;
    cfg.manage_console = false;
    cfg.enable_control_plane = false;
    cfg.log_dir_override = tmp;

    vlink::LauncherServer server(cfg);
    std::string err;
    REQUIRE(server.load_string(manifest, tmp, &err));
    REQUIRE(server.start());

    REQUIRE(wait_until(
        [&]() {
          const auto snap = server.get_snapshot();
          bool svc_ready = false;
          bool bound_ready = false;
          bool flaky_limited = false;

          for (const auto& u : snap.units) {
            if (u.name == "svc_main" && u.state == vlink::launcher::UnitState::kReady) {
              svc_ready = true;
            }

            if (u.name == "svc_bound" && u.state == vlink::launcher::UnitState::kReady) {
              bound_ready = true;
            }

            if (u.name == "flaky" && u.state == vlink::launcher::UnitState::kFailed &&
                u.failed_reason == "start_limit") {
              flaky_limited = true;
            }
          }

          return svc_ready && bound_ready && flaky_limited;
        },
        6000));

    CHECK(server.stop_unit("svc_main").ok());
    const auto snap = server.get_snapshot();

    for (const auto& u : snap.units) {
      if (u.name == "svc_bound") {
        CHECK(u.state == vlink::launcher::UnitState::kStopped);
      }
    }

    CHECK(server.reset_failed("flaky").ok());
    server.stop();
  }
#endif

  TEST_CASE("for_each template expansion and inactive lifecycle") {
    const std::string manifest = R"(
[launcher]
name = "ut_foreach"
ctrl = false

[args]
cams = "front,rear"

[[process]]
name = "cam_${item}_${index}"
for_each = "${arg.cams}"
cmd = "echo"
args = ["${item}"]
stage = 0

[[process]]
name = "worker_${index}"
count = 2
cmd = "echo"
args = ["w${index}"]
stage = 1
)";

    vlink::LauncherServer::Config cfg;
    cfg.manage_console = false;
    cfg.enable_control_plane = false;

    vlink::LauncherServer server(cfg);
    std::string err;
    REQUIRE(server.load_string(manifest, ".", &err));
    const auto rep = server.validate(false);
    REQUIRE(rep.valid);
    CHECK(rep.process_count == 4U);

    const auto snap = server.get_snapshot();
    bool has_front = false;
    bool has_rear = false;
    bool has_w1 = false;

    for (const auto& u : snap.units) {
      if (u.name == "cam_front_0") {
        has_front = true;
      } else if (u.name == "cam_rear_1") {
        has_rear = true;
      } else if (u.name == "worker_1") {
        has_w1 = true;
      }
    }

    CHECK(has_front);
    CHECK(has_rear);
    CHECK(has_w1);
  }

  TEST_CASE("inactive component lifecycle, reactivate readiness, isolate and request_stop") {
    const std::string tmp = make_temp_dir("inactive_lifecycle");
    const std::string plugin_dir = vlink::Utils::get_app_dir();

    const std::string manifest = R"(
[launcher]
name = "ut-inactive"
startup = "parallel"
ctrl = false
plugin_dirs = [")" + plugin_dir + R"("]

[logger]
dir = ")" + tmp + R"("

[[group]]
name = "keep_grp"
mode = "parallel"

[[group]]
name = "drop_grp"
mode = "parallel"

[[scheduler]]
name = "choreo"
mode = "pinned"
queue = "normal"
threads = 2

[[component]]
name = "talker_standby"
group = "keep_grp"
lib = "launcher_test_plugin"
class = "TestTalker"
scheduler = "choreo"
worker = "0"
init_state = "inactive"

[component.ready]
type = "component"
timeout_ms = 3000

[[component]]
name = "listener_drop"
group = "drop_grp"
lib = "launcher_test_plugin"
class = "TestListener"
scheduler = "choreo"
worker = "1"
init_state = "inactive"
)";

    vlink::LauncherServer::Config cfg;
    cfg.manage_console = false;
    cfg.enable_control_plane = false;
    cfg.log_dir_override = tmp;

    vlink::LauncherServer server(cfg);
    std::string err;
    REQUIRE(server.load_string(manifest, tmp, &err));
    REQUIRE(server.start());

    auto unit_state = [&](const std::string& uname) {
      const auto snap = server.get_snapshot();

      for (const auto& u : snap.units) {
        if (u.name == uname) {
          return u.state;
        }
      }

      return vlink::launcher::UnitState::kPending;
    };

    REQUIRE(wait_until(
        [&]() {
          return unit_state("talker_standby") == vlink::launcher::UnitState::kInactive &&
                 unit_state("listener_drop") == vlink::launcher::UnitState::kInactive;
        },
        4000));

    REQUIRE(server.isolate_group("keep_grp").ok());
    CHECK(unit_state("talker_standby") == vlink::launcher::UnitState::kInactive);
    CHECK(unit_state("listener_drop") == vlink::launcher::UnitState::kStopped);

    REQUIRE(server.activate_unit("talker_standby").ok());
    REQUIRE(wait_until([&]() { return unit_state("talker_standby") == vlink::launcher::UnitState::kReady; }, 3000));

    REQUIRE(server.deactivate_unit("talker_standby").ok());
    CHECK(unit_state("talker_standby") == vlink::launcher::UnitState::kInactive);

    REQUIRE(server.activate_unit("talker_standby").ok());
    REQUIRE(wait_until([&]() { return unit_state("talker_standby") == vlink::launcher::UnitState::kReady; }, 3000));

    server.request_stop(0);
    REQUIRE(server.wait(5000));
    CHECK_FALSE(server.is_active());
  }

  TEST_CASE("validation: cross-container host cycle, count cap, worker_index template and intra container guard") {
    vlink::LauncherServer::Config cfg;
    cfg.manage_console = false;
    cfg.enable_control_plane = false;

    const std::string cross_cycle = R"(
[launcher]
name = "ut-cross-cycle"
ctrl = true
ctrl_scheme = "dds"

[[container]]
name = "host_a"
after = ["proc_mid"]

[[process]]
name = "proc_mid"
cmd = "/bin/echo"
after = ["comp_in_a"]

[[component]]
name = "comp_in_a"
container = "host_a"
lib = "launcher_test_plugin"
class = "TestListener"
)";
    vlink::LauncherServer srv_cycle(cfg);
    std::string err;
    CHECK_FALSE(srv_cycle.load_string(cross_cycle, ".", &err));

    const std::string intra_container = R"(
[launcher]
name = "ut-intra-container"
ctrl = true
ctrl_scheme = "intra"

[[container]]
name = "host_a"
)";
    vlink::LauncherServer srv_intra(cfg);
    CHECK_FALSE(srv_intra.load_string(intra_container, ".", &err));

    const std::string huge_count = R"(
[launcher]
name = "ut-huge-count"
ctrl = false

[[process]]
name = "w_${index}"
count = 257
cmd = "/bin/echo"
)";
    vlink::LauncherServer srv_count(cfg);
    CHECK_FALSE(srv_count.load_string(huge_count, ".", &err));

    const std::string tpl_worker = R"(
[launcher]
name = "ut-worker-tpl"
ctrl = false

[[scheduler]]
name = "choreo"
mode = "pinned"
threads = 2

[[component]]
name = "c_${index}"
count = 2
lib = "launcher_test_plugin"
class = "TestListener"
scheduler = "choreo"
worker = "${index}"
)";
    vlink::LauncherServer srv_tpl(cfg);
    REQUIRE(srv_tpl.load_string(tpl_worker, ".", &err));
    const auto rep = srv_tpl.validate(false);
    CHECK(rep.valid);
    CHECK(rep.component_count == 2U);
  }

  TEST_CASE("declarative TOML urls, inputs/outputs, dataflow graph and cross-process intra warning") {
    const std::string tmp = make_temp_dir("declarative_urls");
    const std::string plugin_dir = vlink::Utils::get_app_dir();

    const std::string manifest = R"(
[launcher]
name = "ut-dataflow"
ctrl = false
plugin_dirs = [")" + plugin_dir + R"("]

[args.transport]
default = "intra"

[urls]
chatter = "${arg.transport}://ut/dataflow/chatter"
status_rpc = "dds://ut/dataflow/status"

[logger]
dir = ")" + tmp + R"("

[[scheduler]]
name = "df_sched"
mode = "shared"
threads = 1

[[component]]
name = "pub_comp"
lib = "launcher_test_plugin"
class = "TestTalker"
scheduler = "df_sched"
[component.outputs]
chatter = "${url.chatter}"
[component.params]
period_ms = 30

[[component]]
name = "sub_comp"
lib = "launcher_test_plugin"
class = "TestListener"
scheduler = "df_sched"
[component.inputs]
chatter = "${url.chatter}"
[component.urls]
status_rpc = "${url.status_rpc}"

[[process]]
name = "ext_consumer"
cmd = "/bin/echo"
args = ["${url.chatter}", "${url.status_rpc}"]
autostart = false
[process.inputs]
chatter = "${url.chatter}"
[process.urls]
status_rpc = "${url.status_rpc}"
)";

    vlink::LauncherServer::Config cfg;
    cfg.manage_console = false;
    cfg.enable_control_plane = false;
    cfg.log_dir_override = tmp;

    vlink::LauncherServer server(cfg);
    std::string err;
    REQUIRE(server.load_string(manifest, tmp, &err));
    const auto rep = server.validate(false);
    REQUIRE(rep.valid);

    bool found_intra_warn = false;

    for (const auto& w : rep.warnings) {
      if (w.find("intra://") != std::string::npos && w.find("ext_consumer") != std::string::npos) {
        found_intra_warn = true;
      }
    }

    CHECK(found_intra_warn);

    const std::string ascii = server.export_graph(vlink::LauncherServer::GraphFormat::kAscii);
    CHECK(ascii.find("Dataflow Topology:") != std::string::npos);
    CHECK(ascii.find("pub_comp.chatter --[intra://ut/dataflow/chatter]--> sub_comp.chatter") != std::string::npos);

    const std::string mermaid = server.export_graph(vlink::LauncherServer::GraphFormat::kMermaid);
    CHECK(mermaid.find("intra://ut/dataflow/chatter") != std::string::npos);

    const std::string json_str = server.inspect_manifest_json();
    CHECK(json_str.find("\"dataflow\"") != std::string::npos);
    CHECK(json_str.find("dds://ut/dataflow/status") != std::string::npos);

    REQUIRE(server.start());
    REQUIRE(wait_until(
        [&]() {
          const auto snap = server.get_snapshot();
          bool pub_ok = false;
          bool sub_ok = false;

          for (const auto& u : snap.units) {
            if (u.name == "pub_comp" && u.state == vlink::launcher::UnitState::kReady) {
              pub_ok = true;
            }

            if (u.name == "sub_comp" && u.state == vlink::launcher::UnitState::kReady) {
              sub_ok = true;
            }
          }

          return pub_ok && sub_ok;
        },
        3000));

    server.stop();

    const std::string bad_url_manifest = R"(
[launcher]
name = "ut-bad-url"
ctrl = false

[urls]
broken = "not_a_valid_url"
)";
    vlink::LauncherServer bad_srv(cfg);
    CHECK_FALSE(bad_srv.load_string(bad_url_manifest, ".", &err));
  }

  TEST_CASE("managed lifecycle ports, TaskHandle kFailed, dynamic load ports and live get_unit_config") {
    const std::string tmp = make_temp_dir("managed_ports");
    const std::string plugin_dir = vlink::Utils::get_app_dir();

    const std::string manifest = R"(
[launcher]
name = "ut-managed"
ctrl = false
plugin_dirs = [")" + plugin_dir + R"("]

[logger]
dir = ")" + tmp + R"("

[[scheduler]]
name = "default"
mode = "shared"
threads = 1

[[scheduler]]
name = "compute"
mode = "pool"
threads = 2
)";

    vlink::LauncherServer::Config cfg;
    cfg.manage_console = false;
    cfg.enable_control_plane = false;
    cfg.log_dir_override = tmp;

    vlink::LauncherServer server(cfg);
    std::string err;
    REQUIRE(server.load_string(manifest, tmp, &err));
    REQUIRE(server.start());

    std::atomic<int> received{0};
    vlink::Subscriber<std::string> out_sub("intra://launcher/ut/dyn_port/out");
    out_sub.listen([&](const std::string&) { received.fetch_add(1, std::memory_order_relaxed); });
    vlink::Publisher<std::string> in_pub("intra://launcher/ut/dyn_port/in");

    vlink::launcher::LoadComponentRequest req;
    req.name = "dyn_managed";
    req.library = "launcher_test_plugin";
    req.class_name = "TestManagedPort";
    req.scheduler_group = "default";
    req.compute_pool = "compute";
    req.inputs["in"] = "intra://launcher/ut/dyn_port/in";
    req.outputs["out"] = "intra://launcher/ut/dyn_port/out";
    req.params.push_back(vlink::launcher::encode_param_entry("gain", 1.5));

    const auto load_res = server.load_component(req);
    REQUIRE(load_res.ok());

    REQUIRE(wait_until([&]() { return received.load(std::memory_order_relaxed) >= 1; }, 2000));

    (void)in_pub.publish("hello");
    REQUIRE(wait_until([&]() { return received.load(std::memory_order_relaxed) >= 2; }, 2000));

    const auto throw_res =
        server.set_param("dyn_managed", vlink::launcher::encode_param_entry("probe_throw_task", true));
    CHECK(throw_res.ok());

    const auto gain_res = server.set_param("dyn_managed", vlink::launcher::encode_param_entry("gain", 2.75));
    REQUIRE(gain_res.ok());

    const auto live_cfg = server.get_unit_config("dyn_managed");
    REQUIRE(live_cfg.ok());
    CHECK(live_cfg.message.find("pool = 'compute'") != std::string::npos);
    CHECK(live_cfg.message.find("intra://launcher/ut/dyn_port/in") != std::string::npos);
    CHECK(live_cfg.message.find("2.75") != std::string::npos);

    REQUIRE(server.deactivate_unit("dyn_managed").ok());
    const int before_inactive = received.load(std::memory_order_relaxed);
    const auto suppressed = server.set_param("dyn_managed", vlink::launcher::encode_param_entry("probe_publish", true));
    CHECK_FALSE(suppressed.ok());
    (void)in_pub.publish("while_inactive");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(received.load(std::memory_order_relaxed) == before_inactive);
    CHECK(server.set_param("dyn_managed", vlink::launcher::encode_param_entry("expect_recv_count", int64_t{1})).ok());

    REQUIRE(server.activate_unit("dyn_managed").ok());
    REQUIRE(wait_until([&]() { return received.load(std::memory_order_relaxed) > before_inactive; }, 2000));

    REQUIRE(server.unload_component("dyn_managed").ok());
    server.stop();
  }

  TEST_CASE("validation: cross-stage container projected deadlock rejected") {
    vlink::LauncherServer::Config cfg;
    cfg.manage_console = false;
    cfg.enable_control_plane = false;

    const std::string cross_stage_deadlock = R"(
[launcher]
name = "ut-cross-stage-container"
ctrl = true
ctrl_scheme = "dds"

[[container]]
name = "host_c"
stage = 0

[[process]]
name = "late_proc"
stage = 1
cmd = "/bin/echo"

[[component]]
name = "comp_in_c"
container = "host_c"
lib = "launcher_test_plugin"
class = "TestListener"
after = ["late_proc"]
)";
    vlink::LauncherServer srv(cfg);
    std::string err;
    CHECK_FALSE(srv.load_string(cross_stage_deadlock, ".", &err));
  }

  TEST_CASE("group precedence (global < group < unit), include scoping, and oneshot custom ok_codes") {
    const std::string tmp = make_temp_dir("precedence_inc");
    const std::string child_toml_path = tmp + "/child_inc.toml";

    {
      std::ofstream ofs(child_toml_path, std::ios::out | std::ios::trunc);
      ofs << R"(
[env]
CHILD_ONLY_ENV = "from_child"
SHARED_E = "from_child"

[params]
child_only_p = "from_child"

[[component]]
name = "child_comp"
lib = "launcher_test_plugin"
class = "TestTalker"
)";
    }

    vlink::LauncherServer::Config cfg;
    cfg.manage_console = false;
    cfg.enable_control_plane = false;

    const std::string manifest = R"(
[launcher]
name = "ut-precedence"
ctrl = false

[params]
shared_k = "from_global"
unit_k = "from_global"
global_only = "keep_global"

[env]
SHARED_E = "from_global"
UNIT_E = "from_global"

[remaps]
"intra://ns" = "intra://global_ns"
"intra://b" = "intra://global_b"

[[group]]
name = "g1"
[group.params]
shared_k = "from_group"
unit_k = "from_group"
[group.env]
SHARED_E = "from_group"
UNIT_E = "from_group"
[group.remaps]
"intra://ns/a" = "intra://group_a"
"intra://b" = "intra://group_b"

[[component]]
name = "comp_g1"
group = "g1"
lib = "launcher_test_plugin"
class = "TestTalker"
[component.params]
unit_k = "from_unit"
[component.outputs]
out_a = "intra://ns/a"
out_b = "intra://b"
out_c = "intra://ns/c"
[component.remaps]
"intra://b" = "intra://unit_b"

[[include]]
path = ")" + child_toml_path + R"("
)";

    vlink::LauncherServer srv(cfg);
    std::string err;
    REQUIRE(srv.load_string(manifest, tmp, &err));

    const auto comp_cfg = srv.get_unit_config("comp_g1");
    REQUIRE(comp_cfg.ok());
    CHECK(comp_cfg.message.find("shared_k = 'from_group'") != std::string::npos);
    CHECK(comp_cfg.message.find("unit_k = 'from_unit'") != std::string::npos);
    CHECK(comp_cfg.message.find("global_only = 'keep_global'") != std::string::npos);
    CHECK(comp_cfg.message.find("child_only_p") == std::string::npos);

    const auto child_cfg = srv.get_unit_config("child_comp");
    REQUIRE(child_cfg.ok());
    CHECK(child_cfg.message.find("child_only_p = 'from_child'") != std::string::npos);
    CHECK(child_cfg.message.find("global_only = 'keep_global'") != std::string::npos);

    const auto doc = nlohmann::json::parse(srv.inspect_manifest_json());
    bool checked_comp = false;

    for (const auto& u : doc["units"]) {
      if (u["name"] == "comp_g1") {
        checked_comp = true;
        CHECK(u["outputs"]["out_a"] == "intra://group_a");
        CHECK(u["outputs"]["out_b"] == "intra://unit_b");
        CHECK(u["outputs"]["out_c"] == "intra://global_ns/c");
      }
    }

    CHECK(checked_comp);

#if !defined(_WIN32) && !defined(_WIN64)
    const std::string oneshot_toml = R"(
[launcher]
name = "ut-oneshot-custom"
ctrl = false

[[task]]
name = "prep_exit7"
kind = "oneshot"
autostart = false
cmd = "/bin/sh"
args = ["-c", "exit 7"]
ok_codes = [7]
timeout_ms = 5000

[[component]]
name = "after_prep"
autostart = false
lib = "launcher_test_plugin"
class = "TestListener"
depends_on = ["prep_exit7"]
)";
    vlink::LauncherServer os_srv(cfg);
    REQUIRE(os_srv.load_string(oneshot_toml, tmp, &err));
    REQUIRE(os_srv.start());

    const auto start_res = os_srv.start_unit("after_prep");
    CHECK(start_res.ok());

    const bool ready = wait_until(
        [&]() {
          const auto snap = os_srv.get_snapshot();
          bool prep_stopped = false;
          bool comp_ready = false;

          for (const auto& u : snap.units) {
            if (u.name == "prep_exit7" && u.state == vlink::launcher::UnitState::kStopped && u.exit_code == 7) {
              prep_stopped = true;
            } else if (u.name == "after_prep" && u.state == vlink::launcher::UnitState::kReady) {
              comp_ready = true;
            }
          }

          return prep_stopped && comp_ready;
        },
        4000);
    CHECK(ready);
    os_srv.stop();
#endif
  }

  TEST_CASE("active recording: LauncherPublisher force_publish, dynamic set_record, and intra safety") {
    const std::string tmp = make_temp_dir("active_record");
    const std::string intra_bag = tmp + "/intra_should_not_crash.vcap";

    vlink::LauncherServer::Config cfg;
    cfg.manage_console = false;
    cfg.enable_control_plane = false;
    cfg.log_dir_override = tmp + "/logs";

    const std::string intra_toml = R"(
[launcher]
name = "ut-rec-intra"
ctrl = false

[record]
enabled = true
dir = ")" + tmp + R"(/bags"
format = "vcap"
force = true

[[component]]
name = "intra_rec"
lib = "launcher_test_plugin"
class = "TestManagedPort"
record = true
record_path = ")" + intra_bag + R"("
[component.outputs]
out = "intra://launcher/ut/rec_intra/out"
[component.inputs]
in = "intra://launcher/ut/rec_intra/in"
)";

    {
      vlink::LauncherServer srv(cfg);
      std::string err;
      REQUIRE(srv.load_string(intra_toml, tmp, &err));
      REQUIRE(srv.start());
      REQUIRE(wait_until(
          [&]() {
            const auto snap = srv.get_snapshot();
            return !snap.units.empty() && snap.units[0].state == vlink::launcher::UnitState::kReady;
          },
          3000));
      CHECK(srv.set_param("intra_rec", vlink::launcher::encode_param_entry("probe_publish", true)).ok());
      CHECK(srv.set_record("intra_rec", false).ok());
      srv.stop();
    }

#if defined(VLINK_SUPPORT_DDS) || defined(VLINK_SUPPORT_DDSC) || defined(VLINK_SUPPORT_ZENOH)
#if defined(VLINK_SUPPORT_DDS)
    const std::string rec_scheme = "dds";
#elif defined(VLINK_SUPPORT_DDSC)
    const std::string rec_scheme = "ddsc";
#else
    const std::string rec_scheme = "zenoh";
#endif
    const std::string bag1 = tmp + "/comp_boot.vcap";
    const std::string bag2 = tmp + "/comp_dyn.vcap";
    const std::string rec_domain = std::to_string(100 + (static_cast<int>(vlink::Utils::get_pid()) % 80));
    const std::string out_url =
        rec_scheme + "://launcher/ut/rec_" + vlink::Utils::get_pid_str() + "/out?domain=" + rec_domain;
    const std::string in_url =
        rec_scheme + "://launcher/ut/rec_" + vlink::Utils::get_pid_str() + "/in?domain=" + rec_domain;

    const std::string rec_toml = R"(
[launcher]
name = "ut-rec-active"
ctrl = false

[record]
enabled = false
dir = ")" + tmp + R"(/bags"
format = "vcap"
force = true

[[component]]
name = "rec_pub"
lib = "launcher_test_plugin"
class = "TestManagedPort"
record = true
record_path = ")" + bag1 + R"("
record_force = true
[component.outputs]
out = ")" + out_url + R"("
[component.inputs]
in = ")" + in_url + R"("
)";

    {
      vlink::LauncherServer srv(cfg);
      std::string err;
      REQUIRE(srv.load_string(rec_toml, tmp, &err));
      REQUIRE(srv.start());
      REQUIRE(wait_until(
          [&]() {
            const auto snap = srv.get_snapshot();
            return !snap.units.empty() && snap.units[0].state == vlink::launcher::UnitState::kReady;
          },
          5000));

      CHECK(srv.set_param("rec_pub", vlink::launcher::encode_param_entry("probe_publish", true)).ok());
      CHECK(srv.set_param("rec_pub", vlink::launcher::encode_param_entry("probe_force_publish", std::string("frame_b")))
                .ok());
      std::this_thread::sleep_for(std::chrono::milliseconds(150));

      REQUIRE(srv.set_record("rec_pub", false).ok());

      REQUIRE(srv.set_record("rec_pub", true, bag2).ok());
      CHECK(srv.set_param("rec_pub", vlink::launcher::encode_param_entry("probe_force_publish", std::string("dyn_1")))
                .ok());
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      REQUIRE(srv.set_record("rec_pub", false).ok());
      srv.stop();
    }

    auto reader1 = vlink::BagReader::create(bag1);
    REQUIRE(reader1 != nullptr);
    size_t count1 = 0U;
    vlink::Frame frame;

    while (reader1->read_next(frame)) {
      if (frame.url.find("rec_") != std::string::npos) {
        ++count1;
      }
    }

    CHECK(count1 >= 2U);

    auto reader2 = vlink::BagReader::create(bag2);
    REQUIRE(reader2 != nullptr);
    size_t count2 = 0U;

    while (reader2->read_next(frame)) {
      if (frame.url.find("rec_") != std::string::npos) {
        ++count2;
      }
    }

    CHECK(count2 >= 1U);
#endif
  }

  TEST_CASE("logger plugin (logd): inspect_plugin, host/component/captured routing, strict fail, fallback & override") {
    struct ScopedLogdEnv final {
      ~ScopedLogdEnv() {
        vlink::Utils::unset_env("VLINK_TEST_LOGD_OUT");
        vlink::Utils::unset_env("VLINK_TEST_LOGD_FAIL_INIT");
      }
    } env_guard;
    (void)env_guard;

    const std::string tmp = make_temp_dir("logd_plugin");
    const std::string plugin_dir = vlink::Utils::get_app_dir();
    const std::string logd_file = tmp + "/logd_sink.txt";

    const auto linfo = vlink::LauncherServer::inspect_plugin("launcher_logger_plugin");
    CHECK(linfo.loaded);
    CHECK(linfo.is_logger_plugin);
    CHECK_FALSE(linfo.is_launcher_module);
    CHECK_FALSE(linfo.is_legacy_runnable);
    CHECK(linfo.version_major == 1U);
    CHECK(linfo.version_minor == 0U);

    const auto missing_info = vlink::LauncherServer::inspect_plugin("__missing_vlink_plugin_xyz__");
    CHECK_FALSE(missing_info.loaded);
    CHECK_FALSE(missing_info.error.empty());

    vlink::Utils::set_env("VLINK_TEST_LOGD_OUT", logd_file);
    vlink::Utils::unset_env("VLINK_TEST_LOGD_FAIL_INIT");

    {
      std::string manifest = R"(
[launcher]
name = "ut-logd-stack"
ctrl = false
plugin_dirs = [")" + plugin_dir +
                             R"("]

[logger]
dir = ")" + tmp + R"(/logs"
console_level = "error"
file_level = "debug"
plugin = "launcher_logger_plugin"
plugin_strict = true
plugin_capture = true

[[component]]
name = "logd_talker"
lib = "launcher_test_plugin"
class = "TestTalker"
[component.params]
topic = "intra://ut/logd/msg"
interval_ms = 50
)";
#if !defined(_WIN32) && !defined(_WIN64)
      manifest += R"(
[[task]]
name = "logd_echo"
kind = "oneshot"
cmd = "/bin/sh"
args = ["-c", "echo LOGD_CAPTURED_LINE"]
output = "log"
)";
#endif

      vlink::LauncherServer::Config cfg;
      cfg.manage_console = false;
      cfg.enable_control_plane = false;
      cfg.log_dir_override = tmp + "/logs";

      vlink::LauncherServer srv(cfg);
      std::string err;
      REQUIRE(srv.load_string(manifest, tmp, &err));
      REQUIRE(srv.start());

      const bool logd_ready = wait_until(
          [&]() {
            const auto snap = srv.get_snapshot();
            bool comp_ready = false;
#if !defined(_WIN32) && !defined(_WIN64)
            bool task_done = false;
#else
            bool task_done = true;
#endif

            for (const auto& u : snap.units) {
              if (u.name == "logd_talker" && u.state == vlink::launcher::UnitState::kReady) {
                comp_ready = true;
#if !defined(_WIN32) && !defined(_WIN64)
              } else if (u.name == "logd_echo" && u.state == vlink::launcher::UnitState::kStopped) {
                task_done = true;
#endif
              }
            }

            return comp_ready && task_done;
          },
          4000);
      REQUIRE(logd_ready);

      VLOG_I("UT_LOGD_HOST_INFO_LINE");
      srv.stop();
    }

    {
      std::ifstream ifs(logd_file);
      std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
      CHECK(content.find("INIT ") != std::string::npos);
      CHECK(content.find("app=ut-logd-stack") != std::string::npos);
      CHECK(content.find("UT_LOGD_HOST_INFO_LINE") != std::string::npos);
#if !defined(_WIN32) && !defined(_WIN64)
      CHECK(content.find("[logd_echo] LOGD_CAPTURED_LINE") != std::string::npos);
#endif
      CHECK(content.find("FLUSH ") != std::string::npos);
    }

    {
      const std::string strict_missing = R"(
[launcher]
name = "ut-logd-strict"
ctrl = false

[logger]
file_level = "info"
plugin = "__nonexistent_logd_plugin__"
plugin_strict = true
)";
      vlink::LauncherServer::Config cfg;
      cfg.manage_console = false;
      cfg.enable_control_plane = false;
      cfg.log_dir_override = tmp + "/logs_strict";
      vlink::LauncherServer srv(cfg);
      std::string err;
      REQUIRE(srv.load_string(strict_missing, tmp, &err));
      CHECK_FALSE(srv.start());
      CHECK_FALSE(srv.is_active());
    }

    {
      vlink::Utils::set_env("VLINK_TEST_LOGD_FAIL_INIT", "1");
      const std::string strict_init_fail = R"(
[launcher]
name = "ut-logd-fail-init"
ctrl = false
plugin_dirs = [")" + plugin_dir + R"("]

[logger]
file_level = "info"
plugin = "launcher_logger_plugin"
plugin_strict = true
)";
      vlink::LauncherServer::Config cfg;
      cfg.manage_console = false;
      cfg.enable_control_plane = false;
      cfg.log_dir_override = tmp + "/logs_init_fail";
      vlink::LauncherServer srv(cfg);
      std::string err;
      REQUIRE(srv.load_string(strict_init_fail, tmp, &err));
      CHECK_FALSE(srv.start());
      CHECK_FALSE(srv.is_active());
      vlink::Utils::unset_env("VLINK_TEST_LOGD_FAIL_INIT");
    }

    {
      const std::string nonstrict_missing = R"(
[launcher]
name = "ut-logd-fallback"
ctrl = false

[logger]
file_level = "info"
plugin = "__nonexistent_logd_plugin__"
plugin_strict = false
)";
      vlink::LauncherServer::Config cfg;
      cfg.manage_console = false;
      cfg.enable_control_plane = false;
      cfg.log_dir_override = tmp + "/logs_fallback";
      vlink::LauncherServer srv(cfg);
      std::string err;
      REQUIRE(srv.load_string(nonstrict_missing, tmp, &err));
      REQUIRE(srv.start());
      CHECK(srv.is_active());
      srv.stop();
    }

    {
      const std::string override_out = tmp + "/logd_override.txt";
      vlink::Utils::set_env("VLINK_TEST_LOGD_OUT", override_out);
      const std::string no_plugin_toml = R"(
[launcher]
name = "ut-logd-override"
ctrl = false
plugin_dirs = [")" + plugin_dir + R"("]

[logger]
file_level = "info"
)";
      vlink::LauncherServer::Config cfg;
      cfg.manage_console = false;
      cfg.enable_control_plane = false;
      cfg.log_dir_override = tmp + "/logs_override";
      cfg.log_plugin_override = "launcher_logger_plugin";
      vlink::LauncherServer srv(cfg);
      std::string err;
      REQUIRE(srv.load_string(no_plugin_toml, tmp, &err));
      REQUIRE(srv.start());
      VLOG_I("UT_LOGD_OVERRIDE_MSG");
      srv.stop();

      std::ifstream ifs(override_out);
      std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
      CHECK(content.find("UT_LOGD_OVERRIDE_MSG") != std::string::npos);
    }

    vlink::Utils::unset_env("VLINK_TEST_LOGD_OUT");
    vlink::Utils::unset_env("VLINK_TEST_LOGD_FAIL_INIT");
  }

  TEST_CASE("short TOML canonical keys, alias conflict checks, validation edge cases and example manifest") {
    const std::string tmp = make_temp_dir("short_toml");
    const std::string params_file = tmp + "/extra_params.toml";
    {
      std::ofstream ofs(params_file, std::ios::out | std::ios::trunc);
      ofs << "gain = 3.25\nflags = [true, false]\nnums = [1, 2, 3]\nfloats = [1.5, 2.5]\n";
    }

    const std::string short_manifest = R"(
[launcher]
name = "ut-short"
namespace = "app"
startup = "max_parallel"
max_parallel = 3
target = "@core"
stop_timeout_ms = 4000
ctrl = true
ctrl_scheme = "dds"
ctrl_namespace = "short_ctl"
ctrl_domain = 5
key = "short_secret_key"
bind_ip = "127.0.0.1"
native = true
remote_load = true

[args.env_mode]
default = "sim"
choices = ["sim", "prod"]
desc = "Environment mode"

[logger]
console_level = "info"
file_level = "debug"
max_size_mb = 16
max_files = 4
flush_ms = 200
history = 512
fixed_name = true
append = true
utc = true
console_fmt = true
capture_file = true
strip_ansi = true

[record]
enabled = true
dir = ")" + tmp + R"(/bags"
format = "vcap"
tag = "short"
force = true

[proxy]
enabled = true
name = "px"
cmd = "vlink-proxy"
dds = "dds"
domain = 5
iox = false
extra_args = ["--verbose"]
stage = -100
ready_delay_ms = 100
stop_timeout_ms = 2000

[[slice]]
name = "s1"
cpu_quota_percent = 150
cpu_weight = 400
mem_max_mb = 512
mem_high_mb = 256
pids_max = 64
oom_adj = -10
nice = 5
cpus = [0]
nofile = 1024
vmem_max_mb = 2048
core_max_mb = 0
strict = false

[[group]]
name = "core"
namespace = "sub"
mode = "serial"
slice = "s1"

[[scheduler]]
name = "pin_sched"
mode = "pinned"
queue = "priority"
strategy = "pop"
threads = 2
cpus = [0]
policy = "other"
priority = 0
max_elapsed_ms = 20

[[scheduler]]
name = "pool_sched"
mode = "pool"
queue = "lockfree"
strategy = "block"
threads = 2

[[container]]
name = "box"
group = "core"
stop_timeout_ms = 3000
restart = true
restart_delay_ms = 100
restart_max_ms = 500
restart_backoff = 1.2
start_burst = 3
start_window_ms = 10000
ok_codes = [0]
no_restart_codes = [42]

[[component]]
name = "c1"
group = "core"
container = "box"
lib = "launcher_test_plugin"
class = "TestTalker"
scheduler = "pin_sched"
pool = "pool_sched"
worker = 0
priority = 5
watchdog_ms = 3000
init_state = "inactive"
record = true
record_force = true
params_file = ")" + params_file + R"("
on_start = []
on_ready = []
on_error = []

[[process]]
name = "p1"
group = "core"
cmd = "/bin/echo"
args = ["ok"]
prefix = ""
stop_timeout_ms = 2500
restart = "on_failure"
restart_delay_ms = 100
restart_max_ms = 500
restart_backoff = 1.5
start_burst = 4
start_window_ms = 30000
ok_codes = [0, 3]
no_restart_codes = [9]
[process.liveness]
type = "file"
path = "/tmp/fake_live"
interval_ms = 500
failure_threshold = 2
restart_on_failure = false

[[task]]
name = "t1"
kind = "oneshot"
cmd = "/bin/echo"
args = ["done"]
ok_codes = [0, 7]
timeout_ms = 2000
)";

    vlink::LauncherServer::Config cfg;
    cfg.manage_console = false;
    cfg.enable_control_plane = false;

    vlink::LauncherServer srv(cfg);
    std::string err;
    REQUIRE(srv.load_string(short_manifest, tmp, &err));
    const auto rep = srv.validate(false);
    CHECK(rep.valid);
    CHECK(rep.has_proxy);
    CHECK(rep.slice_count == 1U);
    CHECK(rep.group_count == 1U);
    CHECK(rep.scheduler_group_count == 2U);
    CHECK(rep.container_count == 1U);
    CHECK(rep.component_count == 1U);
    CHECK(rep.process_count == 1U);
    CHECK(rep.task_count == 1U);

    CHECK(srv.get_unit_config("app/px").ok());
    CHECK(srv.get_unit_config("app/box").ok());
    CHECK(srv.get_unit_config("app/p1").ok());
    CHECK(srv.get_unit_config("app/t1").ok());

    auto expect_invalid = [&](const std::string& toml_str) {
      vlink::LauncherServer bad(cfg);
      std::string bad_err;
      CHECK_FALSE(bad.load_string(toml_str, tmp, &bad_err));
    };

    expect_invalid("[launcher]\nstartup = \"serial\"\nstartup_mode = \"parallel\"\n");
    expect_invalid("[logger]\nmax_size_mb = 5\nmax_file_size_mb = 5\n");
    expect_invalid("[[component]]\nname = \"c\"\nlib = \"a\"\nlibrary = \"b\"\nclass = \"C\"\n");
    expect_invalid("[[process]]\nname = \"p\"\ncmd = \"echo\"\nrestart = \"always\"\nrestart_policy = \"no\"\n");
    expect_invalid("[[process]]\nname = \"p\"\ncmd = \"echo\"\nmax_restarts = 2\nstart_burst = 2\n");
    expect_invalid("[[slice]]\nname = \"s\"\nmem_max_mb = 100\nmem_high_mb = 200\n");
    expect_invalid("[[slice]]\nname = \"s\"\ncpu_weight = 99999\n");
    expect_invalid("[[slice]]\nname = \"s\"\noom_adj = 2000\n");
    expect_invalid("[[slice]]\nname = \"s\"\nnice = 30\n");
    expect_invalid("[[slice]]\nname = \"s\"\nnofile = -1\n");
    expect_invalid("[record]\nformat = \"mp4\"\n");
    expect_invalid("[[process]]\nname = \"p\"\ncmd = \"echo\"\nrecord_path = \"/tmp/bad.txt\"\n");
    expect_invalid(
        "[[component]]\nname = \"c\"\nlib = \"x\"\nclass = \"Y\"\nrecord_path = \"/tmp/shared.vcap\"\n"
        "[[process]]\nname = \"p\"\ncmd = \"echo\"\nrecord_path = \"/tmp/shared.vcap\"\n");
    expect_invalid("[[process]]\nname = \"p\"\ncmd = \"echo\"\nok_codes = [300]\n");
    expect_invalid("[[component]]\nname = \"c\"\nlib = \"x\"\nlegacy = true\nwatchdog_ms = 1000\n");
    expect_invalid("[[component]]\nname = \"c\"\nlib = \"x\"\nlegacy = true\nreload = \"component\"\n");
    expect_invalid("[[component]]\nname = \"c\"\nlib = \"x\"\nlegacy = true\ninit_state = \"inactive\"\n");
    expect_invalid("[[component]]\nname = \"c\"\nlib = \"x\"\nreload = \"sighup\"\n");
    expect_invalid("[[process]]\nname = \"p\"\ncmd = \"echo\"\nreload = \"component\"\n");
    expect_invalid("[launcher]\nctrl_scheme = \"intra\"\nkey = \"secret\"\n");
    expect_invalid("[logger]\nconsole_level = \"invalid_lv\"\n");
    expect_invalid("[[scheduler]]\nname = \"s\"\nmode = \"bad_mode\"\n");
    expect_invalid("[[scheduler]]\nname = \"s\"\nqueue = \"bad_q\"\n");
    expect_invalid("[[scheduler]]\nname = \"s\"\nstrategy = \"bad_s\"\n");
    expect_invalid("[[scheduler]]\nname = \"s\"\npolicy = \"bad_p\"\n");
    expect_invalid("[[task]]\nname = \"t\"\nkind = \"bad_kind\"\ncmd = \"echo\"\n");
    expect_invalid("[[process]]\nname = \"p\"\ncmd = \"echo\"\n[process.liveness]\ntype = \"stdout\"\n");
    expect_invalid("[launcher]\nstartup = \"max_parallel\"\nmax_parallel = 0\n");

    const std::string inc_a = tmp + "/cyc_a.toml";
    const std::string inc_b = tmp + "/cyc_b.toml";
    {
      std::ofstream fa(inc_a);
      fa << "[[include]]\npath = \"" << inc_b << "\"\n";
      std::ofstream fb(inc_b);
      fb << "[[include]]\npath = \"" << inc_a << "\"\n";
    }
    vlink::LauncherServer cyc_srv(cfg);
    CHECK_FALSE(cyc_srv.load_file(inc_a, &err));
  }

#if !defined(_WIN32) && !defined(_WIN64)
  TEST_CASE("LauncherAPI full RPCs, LogManager queries, debug filters, triggers, periodic task & inactive set_record") {
    const std::string tmp = make_temp_dir("full_rpcs");
    const std::string plugin_dir = vlink::Utils::get_app_dir();
    const std::string ctrl_ns = "ut_full_" + vlink::Utils::get_pid_str();
    const std::string ready_file = tmp + "/probe_ready.flag";
    const std::string trig_file = tmp + "/triggered.flag";

    const std::string manifest = R"(
[launcher]
name = "ut-full-rpc"
ctrl_scheme = "intra"
ctrl_namespace = ")" + ctrl_ns +
                                 R"("
plugin_dirs = [")" + plugin_dir + R"("]

[params]
global_num = 7
global_flag = true

[logger]
dir = ")" + tmp + R"(/logs"
console_level = "info"
file_level = "debug"
history = 256

[[scheduler]]
name = "ded"
mode = "dedicated"
queue = "normal"

[[scheduler]]
name = "ml"
mode = "multi"
queue = "lockfree"
threads = 2

[[component]]
name = "standby_c"
lib = "launcher_test_plugin"
class = "TestManagedPort"
scheduler = "ded"
init_state = "inactive"
[component.outputs]
out = "intra://ut/full/out"
[component.inputs]
in = "intra://ut/full/in"

[[component]]
name = "multi_c"
lib = "launcher_test_plugin"
class = "TestListener"
scheduler = "ml"
[component.params]
topic = "intra://ut/full/multi"
[component.ready]
type = "delay"
delay_ms = 50
timeout_ms = 2000

[[process]]
name = "file_waiter"
cmd = "/bin/sh"
args = ["-c", "touch )" + ready_file +
                                 R"( && echo WAITER_UP && sleep 30"]
output = "log"
reload = "sighup"
on_ready = ["triggered_task"]
[process.ready]
type = "file"
path = ")" + ready_file + R"("
timeout_ms = 4000
[process.resources]
nice = 0
nofile = 1024

[[task]]
name = "triggered_task"
kind = "oneshot"
autostart = false
cmd = "/bin/sh"
args = ["-c", "echo TRIGGERED_OK > )" +
                                 trig_file + R"( && echo TRIGGERED_STDOUT"]
output = "log"

[[task]]
name = "tick_task"
kind = "periodic"
cmd = "/bin/echo"
args = ["TICK_BEAT"]
interval_ms = 120
timeout_ms = 2000
output = "log"
)";

    vlink::LauncherServer::Config srv_cfg;
    srv_cfg.manage_console = false;
    srv_cfg.log_dir_override = tmp + "/logs";
    srv_cfg.log_level_override = "debug";

    vlink::LauncherServer server(srv_cfg);
    std::string err;
    REQUIRE(server.load_string(manifest, tmp, &err));

    std::atomic<size_t> local_events{0};
    std::atomic<size_t> local_logs{0};
    server.register_event_callback(
        [&](const vlink::launcher::EventRecord&) { local_events.fetch_add(1U, std::memory_order_relaxed); });
    server.register_log_callback(
        [&](const vlink::launcher::LogRecord&) { local_logs.fetch_add(1U, std::memory_order_relaxed); });

    REQUIRE(server.start());
    CHECK_FALSE(server.get_log_dir().empty());

    REQUIRE(wait_until(
        [&]() {
          const auto snap = server.get_snapshot();
          bool standby_ok = false;
          bool multi_ok = false;
          bool waiter_ok = false;

          for (const auto& u : snap.units) {
            if (u.name == "standby_c" && u.state == vlink::launcher::UnitState::kInactive) {
              standby_ok = true;
            } else if (u.name == "multi_c" && u.state == vlink::launcher::UnitState::kReady) {
              multi_ok = true;
            } else if (u.name == "file_waiter" && u.state == vlink::launcher::UnitState::kReady) {
              waiter_ok = true;
            }
          }

          return standby_ok && multi_ok && waiter_ok && std::filesystem::exists(trig_file);
        },
        5000));

    vlink::LauncherAPI::Config api_cfg;
    api_cfg.scheme = "intra";
    api_cfg.namespace_prefix = ctrl_ns;
    api_cfg.subscribe_events = true;
    api_cfg.subscribe_logs = true;

    vlink::LauncherAPI api(api_cfg);
    std::atomic<size_t> api_events{0};
    std::atomic<size_t> api_logs{0};
    api.register_event_callback(
        [&](const vlink::launcher::EventRecord&) { api_events.fetch_add(1U, std::memory_order_relaxed); });
    api.register_log_callback(
        [&](const vlink::launcher::LogRecord&) { api_logs.fetch_add(1U, std::memory_order_relaxed); });
    api.async_run();
    REQUIRE(api.wait_for_connected(3000U));

    CHECK(api.set_record("standby_c", true, tmp + "/standby.vcap", 3000U).ok());
    CHECK(api.activate_unit("standby_c", 3000U).ok());
    CHECK(api.set_record("standby_c", false, "", 3000U).ok());
    CHECK(api.set_record("file_waiter", true, "", 3000U).code == vlink::launcher::ErrorCode::kUnsupported);
    CHECK(api.set_record("standby_c", true, tmp + "/bad.txt", 3000U).code ==
          vlink::launcher::ErrorCode::kInvalidArgument);

    const auto gp = api.get_params("", 3000U);
    CHECK(gp.ok());
    CHECK(gp.params.size() >= 2U);
    CHECK(api.set_param("", vlink::launcher::encode_param_entry("global_num", int64_t{42}), 3000U).ok());
    CHECK(api.get_params("multi_c", 3000U).ok());

    CHECK(api.reload_unit("file_waiter", 3000U).ok());
    CHECK(api.restart_unit("multi_c", 3000U).ok());
    CHECK(api.stop_unit("tick_task", 3000U).ok());
    CHECK(api.start_unit("tick_task", 3000U).ok());
    CHECK(api.reset_failed("", 3000U).ok());
    CHECK(api.resume_startup(3000U).ok());

    vlink::launcher::LogQuery lq;
    lq.unit_filter = "file_waiter";
    lq.grep = "WAITER_UP";
    lq.limit = 50U;
    const auto lres = api.query_logs(lq, 3000U);
    CHECK(lres.ok());
    CHECK_FALSE(lres.logs.empty());

    vlink::launcher::LogQuery lq_after;
    lq_after.after_seq = 1U;
    lq_after.limit = 20U;
    CHECK(api.query_logs(lq_after, 3000U).ok());

    CHECK(local_events.load(std::memory_order_relaxed) > 0U);
    CHECK(local_logs.load(std::memory_order_relaxed) > 0U);
    CHECK(wait_until([&]() { return api_events.load(std::memory_order_relaxed) > 0U; }, 2000));
    CHECK(api.shutdown_server(3000U).ok());
    CHECK(server.wait(5000));
  }
#endif

#if !defined(_WIN32) && !defined(_WIN64)
  TEST_CASE("built-in logd plugin direct and LauncherServer collector end-to-end") {
    struct ScopedEnvGuard final {
      std::vector<std::string> keys;

      ~ScopedEnvGuard() {
        for (const auto& k : keys) {
          vlink::Utils::unset_env(k);
        }
      }
    };

    const std::string bin_dir = vlink::Utils::get_app_dir();
    const std::string lib_dir = bin_dir + "/../lib";

#if defined(__APPLE__)
    const std::string logd_file = "libvlink-launcher_logd.dylib";
#else
    const std::string logd_file = "libvlink-launcher_logd.so";
#endif

    std::string logd_lib;

    for (const auto& dir : {lib_dir, bin_dir}) {
      const auto cand = std::filesystem::path(dir) / logd_file;

      if (std::filesystem::is_regular_file(cand)) {
        logd_lib = cand.generic_string();
        break;
      }
    }

    REQUIRE_FALSE(logd_lib.empty());

    const auto pinfo = vlink::LauncherServer::inspect_plugin(logd_lib);
    CHECK(pinfo.loaded);
    CHECK(pinfo.is_logger_plugin);

    {
      vlink::Utils::unset_env("VLINK_LOGD_URL");
      vlink::Plugin loader;
      loader.set_log_level(vlink::Logger::kOff);
      auto plugin = loader.load<vlink::LoggerPluginInterface>(logd_lib, 1, 0);
      REQUIRE(plugin != nullptr);
      CHECK_FALSE(plugin->init("missing_url_app"));
      plugin.reset();
      loader.clear();
    }

    const std::string tmp = make_temp_dir("builtin_logd");
    const std::string ctrl_ns = "ut_builtin_logd_" + vlink::Utils::get_pid_str();
    const std::string manifest =
        "[launcher]\n"
        "name = \"builtin-logd-ut\"\n"
        "ctrl_scheme = \"intra\"\n"
        "ctrl_namespace = \"" +
        ctrl_ns +
        "\"\n"
        "plugin_dirs = [\"" +
        bin_dir + "\", \"" + lib_dir +
        "\"]\n"
        "\n"
        "[logger]\n"
        "console_level = \"warn\"\n"
        "file_level = \"debug\"\n"
        "plugin = \"logd\"\n"
        "plugin_strict = true\n"
        "plugin_capture = true\n"
        "\n"
        "[[component]]\n"
        "name = \"talker_logd\"\n"
        "lib = \"launcher_test_plugin\"\n"
        "class = \"TestTalker\"\n"
        "[component.params]\n"
        "topic = \"intra://ut_builtin_logd/msg\"\n"
        "interval_ms = 50\n"
        "\n"
        "[[task]]\n"
        "name = \"cap_task\"\n"
        "kind = \"oneshot\"\n"
        "cmd = \"/bin/sh\"\n"
        "args = [\"-c\", \"echo BUILTIN_LOGD_CAPTURED\"]\n"
        "output = \"log\"\n";

    vlink::LauncherServer::Config srv_cfg;
    srv_cfg.manage_console = false;
    srv_cfg.log_dir_override = tmp + "/logs";

    vlink::LauncherServer server(srv_cfg);
    std::string err;
    REQUIRE(server.load_string(manifest, tmp, &err));
    REQUIRE(server.start());

    REQUIRE(wait_until(
        [&]() {
          const auto snap = server.get_snapshot();
          bool comp_ok = false;
          bool task_ok = false;

          for (const auto& u : snap.units) {
            if (u.name == "talker_logd" && u.state == vlink::launcher::UnitState::kReady) {
              comp_ok = true;
            } else if (u.name == "cap_task" && u.state == vlink::launcher::UnitState::kStopped) {
              task_ok = true;
            }
          }

          return comp_ok && task_ok;
        },
        5000));

    {
      ScopedEnvGuard env_guard{{"VLINK_LOGD_URL", "VLINK_LOGD_UNIT", "VLINK_LOGD_QUEUE"}};
      const std::string logd_url = "intra://" + ctrl_ns + "/logd";
      vlink::Utils::set_env("VLINK_LOGD_URL", logd_url);
      vlink::Utils::set_env("VLINK_LOGD_UNIT", "remote_ext_node");
      vlink::Utils::set_env("VLINK_LOGD_QUEUE", "128");

      vlink::Plugin loader;
      loader.set_log_level(vlink::Logger::kOff);
      auto plugin = loader.load<vlink::LoggerPluginInterface>(logd_lib, 1, 0);
      REQUIRE(plugin != nullptr);
      REQUIRE(plugin->init("fallback_app"));
      CHECK(plugin->log(vlink::Logger::kInfo, "REMOTE_LOGD_PAYLOAD\n"));
      CHECK(plugin->log(vlink::Logger::kError, std::string(10000U, 'x')));
      plugin->flush();
      plugin.reset();
      loader.clear();
    }

    REQUIRE(wait_until(
        [&]() {
          vlink::launcher::LogQuery q;
          q.unit_filter = "remote_ext_node";
          q.grep = "REMOTE_LOGD_PAYLOAD";
          const auto r = server.query_logs(q);
          return r.ok() && !r.logs.empty();
        },
        3000));

    {
      vlink::launcher::LogQuery q;
      q.unit_filter = "remote_ext_node";
      q.grep = "...[truncated]";
      const auto r = server.query_logs(q);
      CHECK(r.ok());
      CHECK_FALSE(r.logs.empty());
    }

    const std::string logd_dir = server.get_log_dir() + "/logd";
    server.stop();
    CHECK(server.wait(5000));

    REQUIRE(std::filesystem::exists(logd_dir));
    std::string combined;

    for (const auto& entry : std::filesystem::directory_iterator(logd_dir)) {
      if (entry.is_regular_file()) {
        std::ifstream ifs(entry.path());
        combined.append(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
      }
    }

    CHECK(combined.find("[remote_ext_node] REMOTE_LOGD_PAYLOAD") != std::string::npos);
    CHECK(combined.find("[cap_task] BUILTIN_LOGD_CAPTURED") != std::string::npos);

    REQUIRE(server.start());
    server.stop();
    CHECK(server.wait(5000));
  }
#endif

  TEST_CASE(
      "Round-2 and Round-3 regressions: remap order, group.members inheritance, validation and record collisions") {
    const std::string tmp = make_temp_dir("regressions_r3");
    const std::string bin_dir = vlink::Utils::get_app_dir();

    {
      const std::string toml =
          "[launcher]\n"
          "name = \"remap-order-ut\"\n"
          "\n"
          "[remaps]\n"
          "\"dds://a/b\" = \"dds://first\"\n"
          "\"dds://a\" = \"dds://second\"\n"
          "\n"
          "[[group]]\n"
          "name = \"disabled_grp\"\n"
          "condition = false\n"
          "autostart = false\n"
          "members = [\"vlink_proxy\", \"worker_p\"]\n"
          "[group.env]\n"
          "GRP_INHERITED = \"yes\"\n"
          "\n"
          "[proxy]\n"
          "enabled = true\n"
          "name = \"vlink_proxy\"\n"
          "\n"
          "[[process]]\n"
          "name = \"worker_p\"\n"
          "cmd = \"/bin/true\"\n"
          "[process.outputs]\n"
          "out = \"dds://a/b/x\"\n";

      vlink::LauncherServer::Config cfg;
      cfg.manage_console = false;
      cfg.enable_control_plane = false;
      cfg.log_dir_override = tmp + "/logs_inherit";
      vlink::LauncherServer srv(cfg);
      std::string err;
      REQUIRE(srv.load_string(toml, tmp, &err));
      const auto j = nlohmann::json::parse(srv.inspect_manifest_json());
      REQUIRE(j.contains("units"));
      bool found_proc = false;
      bool found_proxy = false;

      for (const auto& u : j["units"]) {
        if (u["name"] == "worker_p") {
          found_proc = true;
          CHECK(u["group"] == "disabled_grp");
          CHECK(u["condition"] == false);
          CHECK(u["autostart"] == false);
          CHECK(u["outputs"]["out"] == "dds://second/b/x");
        } else if (u["name"] == "vlink_proxy") {
          found_proxy = true;
          CHECK(u["group"] == "disabled_grp");
          CHECK(u["condition"] == false);
          CHECK(u["autostart"] == false);
        }
      }

      CHECK(found_proc);
      CHECK(found_proxy);
      REQUIRE(srv.start());
      REQUIRE(wait_until(
          [&]() {
            const auto snap = srv.get_snapshot();
            bool proxy_skipped = false;
            bool proc_skipped = false;

            for (const auto& u : snap.units) {
              if (u.name == "vlink_proxy" && u.state == vlink::launcher::UnitState::kSkipped) {
                proxy_skipped = true;
              } else if (u.name == "worker_p" && u.state == vlink::launcher::UnitState::kSkipped) {
                proc_skipped = true;
              }
            }

            return proxy_skipped && proc_skipped;
          },
          3000));
      srv.stop();
      CHECK(srv.wait(5000));
    }

    {
      const std::string bad_codes =
          "[launcher]\nname = \"bad-codes\"\n"
          "[[process]]\nname = \"p\"\ncmd = \"/bin/true\"\nrestart = \"always\"\nno_restart_codes = 42\n";
      vlink::LauncherServer srv;
      std::string err;
      CHECK_FALSE(srv.load_string(bad_codes, tmp, &err));
      CHECK(err.find("array") != std::string::npos);
    }

    {
      const std::string bad_comp_ready =
          "[launcher]\nname = \"bad-comp-ready\"\n"
          "[[component]]\nname = \"c\"\nlib = \"launcher_test_plugin\"\nclass = \"TestTalker\"\n"
          "[component.ready]\ntype = \"stdout\"\npattern = \"OK\"\n";
      vlink::LauncherServer srv;
      std::string err;
      CHECK_FALSE(srv.load_string(bad_comp_ready, tmp, &err));
      CHECK(err.find("stdout") != std::string::npos);
    }

    {
      const std::string abs_bag = std::filesystem::absolute(std::filesystem::path(tmp) / "dup.vcap").string();
      const std::string bad_bag_collision =
          "[launcher]\nname = \"dup-bag\"\n"
          "[record]\ndir = \"" +
          tmp +
          "\"\n"
          "[[component]]\nname = \"c\"\nlib = \"launcher_test_plugin\"\nclass = \"TestTalker\"\n"
          "record_path = \"dup.vcap\"\n"
          "[[process]]\nname = \"p\"\ncmd = \"/bin/true\"\nrecord_path = \"" +
          abs_bag + "\"\n";
      vlink::LauncherServer srv;
      std::string err;
      CHECK_FALSE(srv.load_string(bad_bag_collision, tmp, &err));
      CHECK(err.find("record_path") != std::string::npos);
    }

#if !defined(_WIN32) && !defined(_WIN64)
    {
      const std::string ctrl_ns = "ut_rec_coll_" + vlink::Utils::get_pid_str();
      const std::string shared_bag = tmp + "/shared_runtime.vcap";
      const std::string manifest =
          "[launcher]\nname = \"rt-rec-ut\"\nctrl_scheme = \"intra\"\nctrl_namespace = \"" + ctrl_ns +
          "\"\nplugin_dirs = [\"" + bin_dir +
          "\"]\n"
          "[[component]]\nname = \"c1\"\nlib = \"launcher_test_plugin\"\nclass = \"TestTalker\"\n"
          "[component.params]\ntopic = \"intra://rt_rec/t\"\ninterval_ms = 100\n"
          "[[process]]\nname = \"p1\"\ncmd = \"/bin/sleep\"\nargs = [\"10\"]\nrecord_path = \"" +
          shared_bag + "\"\n";

      vlink::LauncherServer::Config srv_cfg;
      srv_cfg.manage_console = false;
      srv_cfg.log_dir_override = tmp + "/logs";

      vlink::LauncherServer server(srv_cfg);
      std::string err;
      REQUIRE(server.load_string(manifest, tmp, &err));
      REQUIRE(server.start());
      REQUIRE(wait_until(
          [&]() {
            const auto snap = server.get_snapshot();
            bool c1 = false;
            bool p1 = false;

            for (const auto& u : snap.units) {
              if (u.name == "c1" && u.state == vlink::launcher::UnitState::kReady) {
                c1 = true;
              } else if (u.name == "p1" && u.state == vlink::launcher::UnitState::kReady) {
                p1 = true;
              }
            }

            return c1 && p1;
          },
          5000));

      const auto coll_res = server.set_record("c1", true, shared_bag);
      CHECK(coll_res.code == vlink::launcher::ErrorCode::kInvalidArgument);
      server.stop();
      CHECK(server.wait(5000));
    }
#endif
  }

  TEST_CASE("LauncherAPI string helpers, codecs, callbacks, security guards, resources, metrics and context helpers") {
    using vlink::launcher::ErrorCode;
    using vlink::launcher::UnitState;
    using vlink::launcher::UnitType;

    CHECK(std::string(vlink::launcher::unit_type_to_string(UnitType::kProcess)) == "process");
    CHECK(std::string(vlink::launcher::unit_type_to_string(UnitType::kComponent)) == "component");
    CHECK(std::string(vlink::launcher::unit_type_to_string(UnitType::kContainer)) == "container");
    CHECK(std::string(vlink::launcher::unit_type_to_string(UnitType::kTask)) == "task");
    CHECK(std::string(vlink::launcher::unit_type_to_string(UnitType::kProxy)) == "proxy");
    CHECK(std::string(vlink::launcher::unit_type_to_string(static_cast<UnitType>(99))) == "unknown");

    for (const auto st : {UnitState::kPending, UnitState::kStarting, UnitState::kRunning, UnitState::kReady,
                          UnitState::kStopping, UnitState::kStopped, UnitState::kRestarting, UnitState::kFailed,
                          UnitState::kSkipped, UnitState::kInactive, static_cast<UnitState>(99)}) {
      CHECK_FALSE(std::string(vlink::launcher::unit_state_to_string(st)).empty());
    }

    for (const auto ec :
         {ErrorCode::kOk, ErrorCode::kInvalidArgument, ErrorCode::kNotFound, ErrorCode::kAlreadyExists,
          ErrorCode::kAlreadyRunning, ErrorCode::kNotRunning, ErrorCode::kDependencyFailed, ErrorCode::kUnsupported,
          ErrorCode::kTimeout, ErrorCode::kDisconnected, ErrorCode::kVersionMismatch, ErrorCode::kUnauthorized,
          ErrorCode::kInternalError, static_cast<ErrorCode>(99)}) {
      CHECK_FALSE(std::string(vlink::launcher::error_code_to_string(ec)).empty());
    }

    {
      vlink::LauncherParamValue out;
      CHECK(vlink::launcher::decode_param_entry(vlink::launcher::encode_param_entry("b", true), out));
      CHECK(std::get<bool>(out));
      CHECK(vlink::launcher::decode_param_entry(
          vlink::launcher::encode_param_entry("bl", std::vector<bool>{true, false}), out));
      CHECK(std::get<std::vector<bool>>(out).size() == 2U);
      CHECK(vlink::launcher::decode_param_entry(
          vlink::launcher::encode_param_entry("dl", std::vector<double>{1.25, 2.5}), out));
      CHECK(std::get<std::vector<double>>(out).size() == 2U);

      vlink::launcher::ParamEntry bad_int;
      bad_int.key = "bad";
      bad_int.type = vlink::launcher::ParamType::kInt;
      bad_int.value = "not_an_int";
      CHECK_FALSE(vlink::launcher::decode_param_entry(bad_int, out));

      vlink::launcher::ParamEntry bad_dbl;
      bad_dbl.key = "bad_d";
      bad_dbl.type = vlink::launcher::ParamType::kDouble;
      bad_dbl.value = "not_a_double";
      CHECK_FALSE(vlink::launcher::decode_param_entry(bad_dbl, out));

      vlink::launcher::ParamEntry bad_json;
      bad_json.key = "bad_j";
      bad_json.type = vlink::launcher::ParamType::kIntList;
      bad_json.value = "{not_array}";
      CHECK_FALSE(vlink::launcher::decode_param_entry(bad_json, out));
    }

    {
      vlink::LauncherAPI::Config bad_sec;
      bad_sec.scheme = "intra";
      bad_sec.security_key = "not_allowed_on_intra";
      vlink::LauncherAPI bad_api(bad_sec);
      CHECK_FALSE(bad_api.is_connected());
      CHECK(bad_api.get_status(100U).code == ErrorCode::kUnsupported);
    }

#if !defined(_WIN32) && !defined(_WIN64)
    const std::string tmp = make_temp_dir("cov_boost");
    const std::string plugin_dir = vlink::Utils::get_app_dir();
    const std::string ctrl_ns = "ut_cov_" + vlink::Utils::get_pid_str();

    std::string manifest = R"(
[launcher]
name = "ut-cov-boost"
ctrl_scheme = "intra"
ctrl_namespace = ")" + ctrl_ns +
                           R"("
plugin_dirs = [")" + plugin_dir +
                           R"("]

[[slice]]
name = "res_slice"
cpus = [0]
nice = 0
nofile = 1024
vmem_max_mb = 4096
core_max_mb = 0
oom_adj = 0
strict = false

[[scheduler]]
name = "pin_s"
mode = "pinned"
queue = "priority"
strategy = "pop"
threads = 2
cpus = [0]
policy = "other"
priority = 0
max_elapsed_ms = 50

[[scheduler]]
name = "pool_s"
mode = "pool"
queue = "lockfree"
strategy = "block"
threads = 2

[[group]]
name = "active_g"
slice = "res_slice"

[[group]]
name = "standby_g"
autostart = false

[[component]]
name = "ctx_comp"
group = "active_g"
lib = "launcher_test_plugin"
class = "TestManagedPort"
scheduler = "pin_s"
pool = "pool_s"
worker = 0
priority = 4
[component.params]
p_bool = true
p_bool_str = "1"
p_bool_false_str = "false"
p_int = 42
p_int_str = "99"
p_double = 1.5
p_double_str = "2.5"
p_bool_list = [true, false]
p_int_list = [10, 20]
p_double_list = [1.5, 2.5]
p_str_list = ["a", "b"]

[[process]]
name = "res_proc"
group = "active_g"
cmd = "/bin/sh"
args = ["-c", "sleep 20 & sleep 20"]
kill_mode = "control-group"
stop_signal = "sigint"
stop_timeout_ms = 1000
output = "log"

[[process]]
name = "standby_proc"
group = "standby_g"
cmd = "/bin/sleep"
args = ["20"]
kill_mode = "mixed"
stop_signal = "sigterm"
stop_timeout_ms = 1000
)";

#ifdef VLINK_ENABLE_COROUTINE
    manifest += R"(
[[component]]
name = "coro_c"
group = "active_g"
lib = "launcher_test_plugin"
class = "TestCoro"
scheduler = "pin_s"
worker = 1
watchdog_ms = 2000
)";
#endif

    vlink::LauncherServer::Config srv_cfg;
    srv_cfg.manage_console = false;
    srv_cfg.log_dir_override = tmp + "/logs";

    vlink::LauncherServer server(srv_cfg);
    std::string err;
    REQUIRE(server.load_string(manifest, tmp, &err));
    REQUIRE(server.start());

    REQUIRE(wait_until(
        [&]() {
          const auto snap = server.get_snapshot();
          bool comp_ready = false;
          bool proc_ready = false;

          for (const auto& u : snap.units) {
            if (u.name == "ctx_comp" && u.state == UnitState::kReady) {
              comp_ready = true;
            } else if (u.name == "res_proc" && u.state == UnitState::kReady && u.pid > 0) {
              proc_ready = true;
            }
          }

          return comp_ready && proc_ready;
        },
        5000));

    vlink::LauncherAPI::Config api_cfg;
    api_cfg.scheme = "intra";
    api_cfg.namespace_prefix = ctrl_ns;
    api_cfg.subscribe_events = true;
    api_cfg.subscribe_logs = true;

    vlink::LauncherAPI api(api_cfg);
    std::atomic<int> conn_events{0};
    std::atomic<int> snap_events{0};
    api.register_connect_callback([&](bool) { conn_events.fetch_add(1, std::memory_order_relaxed); });
    api.register_snapshot_callback(
        [&](const vlink::launcher::SystemSnapshot&) { snap_events.fetch_add(1, std::memory_order_relaxed); });
    api.async_run();
    REQUIRE(api.wait_for_connected(3000U));

    CHECK(api.set_param("ctx_comp", vlink::launcher::encode_param_entry("probe_param_helpers", true), 3000U).ok());
    CHECK(api.get_unit_config("ctx_comp", 3000U).ok());
    CHECK(api.get_unit_config("res_proc", 3000U).ok());

    CHECK(wait_until(
        [&]() {
          const auto st = api.get_status(2000U);

          if (!st.ok()) {
            return false;
          }

          for (const auto& u : st.snapshot.units) {
            if (u.name == "res_proc" && u.memory_rss_kb > 0U && u.thread_count >= 1U) {
              return true;
            }
          }

          return false;
        },
        3000));

    CHECK(api.deactivate_unit("ctx_comp", 3000U).ok());
    CHECK(api.activate_unit("ctx_comp", 3000U).ok());

    vlink::launcher::LoadComponentRequest lreq;
    lreq.name = "dyn_c";
    lreq.library = "launcher_test_plugin";
    lreq.class_name = "TestListener";
    lreq.scheduler_group = "pin_s";
    lreq.compute_pool = "pool_s";
    lreq.auto_start = true;
    CHECK(api.load_component(lreq, 3000U).ok());
    CHECK(api.unload_component("dyn_c", 3000U).ok());

    CHECK(api.isolate_group("standby_g", 5000U).ok());
    CHECK(wait_until(
        [&]() {
          const auto st = api.get_status(2000U);
          bool standby_on = false;
          bool res_off = false;

          for (const auto& u : st.snapshot.units) {
            if (u.name == "standby_proc" && u.state == UnitState::kReady) {
              standby_on = true;
            } else if (u.name == "res_proc" && u.state == UnitState::kStopped) {
              res_off = true;
            }
          }

          return standby_on && res_off;
        },
        4000));

    CHECK(conn_events.load(std::memory_order_relaxed) >= 1);
    CHECK(snap_events.load(std::memory_order_relaxed) >= 1);
    const auto cached = api.get_cached_snapshot();
    CHECK(cached.has_value());

    if (cached.has_value()) {
      CHECK_FALSE(cached->session_id.empty());
    }

    CHECK(api.shutdown_server(3000U).ok());
    CHECK(server.wait(5000));
#endif
  }

  TEST_CASE(
      "audit regressions: condition AST, schema validation, escape, cycle path, reload/reset, opt-out URL & overflow") {
    const std::string tmp = make_temp_dir("audit_reg");
    const std::string plugin_dir = vlink::Utils::get_app_dir();
    const std::string ctrl_ns = "ut_reg_" + vlink::Utils::get_pid_str();

    vlink::LauncherServer::Config cfg;
    cfg.manage_console = false;
    cfg.enable_control_plane = false;
    cfg.log_dir_override = tmp + "/logs";

    auto expect_load_fail = [&](const std::string& toml_text,
                                const std::unordered_map<std::string, std::string>& overrides = {}) {
      auto local_cfg = cfg;
      local_cfg.arg_overrides = overrides;
      vlink::LauncherServer srv(local_cfg);
      std::string err;
      CHECK_FALSE(srv.load_string(toml_text, tmp, &err));
      CHECK_FALSE(err.empty());
    };

    // 1. Strict schema validation rejects unknown keys across tables
    expect_load_fail("[launcher]\nname = \"x\"\nunknown_launcher_key = 1\n");
    expect_load_fail("[logger]\nunknown_logger_key = true\n");
    expect_load_fail("[[component]]\nname = \"c\"\nlib = \"l\"\nclass = \"C\"\ntypo_key = 1\n");
    expect_load_fail("[[component]]\nname = \"c\"\nlib = \"l\"\nclass = \"C\"\n[component.ready]\nbad_ready_key = 1\n");

    // 2. Condition AST parses before substitution (no logic injection, rejects malformed/non-string)
    expect_load_fail("[[component]]\nname = \"c\"\nlib = \"l\"\nclass = \"C\"\ncondition = 123\n");
    expect_load_fail("[[component]]\nname = \"c\"\nlib = \"l\"\nclass = \"C\"\ncondition = \"!= foo\"\n");
    expect_load_fail("[[component]]\nname = \"c\"\nlib = \"l\"\nclass = \"C\"\ncondition = \"(true\"\n");

    {
      const std::string cond_toml = R"(
[launcher]
name = "ut-cond-ast"
ctrl = false

[args.empty_val]
default = ""

[args.inj]
default = "safe"

[[component]]
name = "empty_neq_kept"
lib = "launcher_test_plugin"
class = "TestListener"
condition = "${arg.empty_val} != foo"

[[component]]
name = "inj_blocked"
lib = "launcher_test_plugin"
class = "TestListener"
condition = "${arg.inj} == safe"

[[component]]
name = "escaped_param"
lib = "launcher_test_plugin"
class = "TestListener"
[component.params]
raw_tpl = "$${arg.inj}"
mixed_nums = [1, 2.5, 3]
)";
      auto cond_cfg = cfg;
      cond_cfg.arg_overrides = {{"inj", "nope || true"}};
      vlink::LauncherServer srv(cond_cfg);
      std::string err;
      REQUIRE(srv.load_string(cond_toml, tmp, &err));
      const auto j = nlohmann::json::parse(srv.inspect_manifest_json());
      bool has_empty_neq = false;
      bool has_inj = false;

      for (const auto& u : j["units"]) {
        if (u["name"] == "empty_neq_kept") {
          has_empty_neq = u["condition"].get<bool>();
        } else if (u["name"] == "inj_blocked") {
          has_inj = u["condition"].get<bool>();
        }
      }

      CHECK(has_empty_neq);
      CHECK_FALSE(has_inj);

      // Verify $${...} escape preserved literal "${arg.inj}" and mixed [1, 2.5, 3] promoted to double list
      const auto p_res = srv.get_params("escaped_param");
      REQUIRE(p_res.ok());
      bool saw_raw_tpl = false;
      bool saw_mixed_double = false;

      for (const auto& p : p_res.params) {
        if (p.key == "raw_tpl") {
          saw_raw_tpl = (p.value == "${arg.inj}");
        } else if (p.key == "mixed_nums") {
          vlink::LauncherParamValue val;
          REQUIRE(vlink::launcher::decode_param_entry(p, val));
          const auto* dl = std::get_if<std::vector<double>>(&val);
          saw_mixed_double = (dl != nullptr && dl->size() == 3U && std::abs((*dl)[1] - 2.5) < 1e-9);
        }
      }

      CHECK(saw_raw_tpl);
      CHECK(saw_mixed_double);
    }

    // 3. Duplicate explicit [[scheduler]] name = "default" rejected
    {
      const std::string dup_default = R"(
[[scheduler]]
name = "default"
mode = "shared"

[[scheduler]]
name = "default"
mode = "multi"
threads = 2
)";
      vlink::LauncherServer srv(cfg);
      std::string err;
      CHECK_FALSE(srv.load_string(dup_default, tmp, &err));
      CHECK(err.find("Duplicate scheduler group 'default'") != std::string::npos);
    }

    // 4. Concrete DFS cycle path reported in validation errors
    {
      const std::string cyc_toml = R"(
[[component]]
name = "cyc_a"
lib = "launcher_test_plugin"
class = "TestListener"
after = ["cyc_b"]

[[component]]
name = "cyc_b"
lib = "launcher_test_plugin"
class = "TestListener"
after = ["cyc_a"]
)";
      vlink::LauncherServer srv(cfg);
      std::string err;
      CHECK_FALSE(srv.load_string(cyc_toml, tmp, &err));
      CHECK(err.find("->") != std::string::npos);
      CHECK(err.find("cyc_a") != std::string::npos);
      CHECK(err.find("cyc_b") != std::string::npos);
    }

    // 5. Absolute URL opt-out (scheme:///path) bypasses per-unit namespace prefixing
    {
      const std::string abs_url_toml = R"(
[launcher]
name = "ut-abs-url"
namespace = "fleet"

[[component]]
name = "cam_front"
namespace = "perception/cam_front"
lib = "launcher_test_plugin"
class = "TestListener"
[component.outputs]
scan = "intra:///fleet/perception/scan"
local_dbg = "intra://debug"
)";
      vlink::LauncherServer srv(cfg);
      std::string err;
      REQUIRE(srv.load_string(abs_url_toml, tmp, &err));
      const auto cfg_res = srv.get_unit_config("fleet/cam_front");
      REQUIRE(cfg_res.ok());
      CHECK(cfg_res.message.find("intra:///fleet/perception/scan") != std::string::npos);

      const std::string paired_toml = abs_url_toml + R"(
[[component]]
name = "fusion"
namespace = "perception"
lib = "launcher_test_plugin"
class = "TestListener"
[component.inputs]
scan = "intra:///fleet/perception/scan"
)";
      vlink::LauncherServer srv2(cfg);
      REQUIRE(srv2.load_string(paired_toml, tmp, &err));
      const auto j2 = nlohmann::json::parse(srv2.inspect_manifest_json());
      bool found_edge = false;

      for (const auto& e : j2["dataflow"]) {
        if (e["from"] == "fleet/cam_front" && e["to"] == "fleet/fusion" &&
            e["url"] == "intra://fleet/perception/scan") {
          found_edge = true;
          break;
        }
      }

      CHECK(found_edge);
    }

    // 6. Runtime regressions: reload="none" -> kUnsupported, kInactive set_param, reset_failed kNotFound & on_reset,
    //    and LauncherTimerComponent uint32_t overflow rejection
    {
      const std::string rt_toml = R"(
[launcher]
name = "ut-rt-reg"
ctrl_scheme = "intra"
ctrl_namespace = ")" + ctrl_ns + R"("
plugin_dirs = [")" + plugin_dir + R"("]

[[component]]
name = "no_reload_c"
lib = "launcher_test_plugin"
class = "TestTalker"
reload = "none"

[[component]]
name = "standby_param_c"
lib = "launcher_test_plugin"
class = "TestManagedPort"
init_state = "inactive"

[[component]]
name = "fail_c"
lib = "launcher_test_plugin"
class = "TestFailOnStart"

[[component]]
name = "overflow_timer_c"
lib = "launcher_test_plugin"
class = "TestTalker"
[component.params]
interval_ms = 5000000000
)";
      vlink::LauncherServer::Config srv_cfg;
      srv_cfg.manage_console = false;
      srv_cfg.log_dir_override = tmp + "/rt_logs";
      vlink::LauncherServer srv(srv_cfg);
      std::string err;
      REQUIRE(srv.load_string(rt_toml, tmp, &err));

      std::atomic<bool> saw_reset_event{false};
      srv.register_event_callback([&](const vlink::launcher::EventRecord& ev) {
        if (ev.unit_name == "fail_c" && ev.event_name == "on_reset") {
          saw_reset_event.store(true, std::memory_order_release);
        }
      });

      REQUIRE(srv.start());
      REQUIRE(wait_until(
          [&]() {
            const auto snap = srv.get_snapshot();
            bool r1 = false;
            bool r2 = false;
            bool r3 = false;
            bool r4 = false;

            for (const auto& u : snap.units) {
              if (u.name == "no_reload_c" && u.state == vlink::launcher::UnitState::kReady) {
                r1 = true;
              } else if (u.name == "standby_param_c" && u.state == vlink::launcher::UnitState::kInactive) {
                r2 = true;
              } else if (u.name == "fail_c" && u.state == vlink::launcher::UnitState::kFailed) {
                r3 = true;
              } else if (u.name == "overflow_timer_c" && u.state == vlink::launcher::UnitState::kFailed) {
                r4 = true;
              }
            }

            return r1 && r2 && r3 && r4;
          },
          4000));

      CHECK(srv.reload_unit("no_reload_c").code == vlink::launcher::ErrorCode::kUnsupported);
      CHECK(srv.set_param("standby_param_c", vlink::launcher::encode_param_entry("dyn_k", int64_t{7})).ok());
      CHECK(srv.reset_failed("nonexistent_unit").code == vlink::launcher::ErrorCode::kNotFound);
      CHECK(srv.reset_failed("fail_c").ok());
      CHECK(wait_until([&]() { return saw_reset_event.load(std::memory_order_acquire); }, 1000));
      srv.stop();
    }

#if defined(VLINK_ENABLE_SECURITY) && (defined(VLINK_SUPPORT_DDS) || defined(VLINK_SUPPORT_DDSC))
    // 7. DDS loopback + AES security_key control-plane roundtrip & remote load_component policy
    {
      const uint32_t sec_domain =
          100U + (static_cast<uint32_t>(std::hash<std::string>{}(vlink::Utils::get_pid_str())) % 80U);
      const std::string sec_ns = "ut_sec_" + vlink::Utils::get_pid_str();
      const std::string sec_key = "ut-aes-control-secret-2026";
      const std::string sec_toml = R"(
[launcher]
name = "ut-dds-sec"
ctrl_scheme = "dds"
ctrl_namespace = ")" + sec_ns + R"("
ctrl_domain = )" + std::to_string(sec_domain) +
                                   R"(
key = ")" + sec_key + R"("
bind_ip = "127.0.0.1"
native = true
remote_load = false
plugin_dirs = [")" + plugin_dir + R"("]

[[component]]
name = "sec_comp"
lib = "launcher_test_plugin"
class = "TestListener"
)";
      vlink::LauncherServer::Config srv_cfg;
      srv_cfg.manage_console = false;
      srv_cfg.log_dir_override = tmp + "/sec_logs";
      vlink::LauncherServer srv(srv_cfg);
      std::string err;
      REQUIRE(srv.load_string(sec_toml, tmp, &err));
      REQUIRE(srv.start());

      vlink::LauncherAPI::Config api_cfg;
      api_cfg.scheme = "dds";
      api_cfg.namespace_prefix = sec_ns;
      api_cfg.domain_id = sec_domain;
      api_cfg.security_key = sec_key;
      api_cfg.bind_ip = "127.0.0.1";
      api_cfg.native_mode = true;
      vlink::LauncherAPI api(api_cfg);
      api.async_run();
      REQUIRE(api.wait_for_connected(5000U));

      const auto st = api.get_status(3000U);
      REQUIRE(st.ok());
      CHECK(st.snapshot.units.size() == 1U);

      vlink::launcher::LoadComponentRequest lreq;
      lreq.name = "remote_dyn";
      lreq.library = "launcher_test_plugin";
      lreq.class_name = "TestListener";
      CHECK(api.load_component(lreq, 3000U).ok());
      CHECK(api.unload_component("remote_dyn", 3000U).ok());

      CHECK(api.shutdown_server(3000U).ok());
      CHECK(srv.wait(5000));
    }
#endif

    // 8. Regression checks: overflow decoding, strict schema/type/condition, compute_pool mode, remap ///,
    //    liveness enabled=false, second start() restartability, and legacy plugin stop+restart reload
    {
      // 8a. kIntList uint64 > INT64_MAX rejected
      vlink::launcher::ParamEntry over_list;
      over_list.key = "big";
      over_list.type = vlink::launcher::ParamType::kIntList;
      over_list.value = "[9223372036854775808]";
      vlink::LauncherParamValue decoded;
      CHECK_FALSE(vlink::launcher::decode_param_entry(over_list, decoded));

      // 8b. Strict schema rejects [process] single-table, wrong resource type, MiB overflow, chained !=, bad [args.x],
      //     and non-pool compute_pool
      expect_load_fail("[process]\nname = \"p\"\ncmd = \"/bin/true\"\n");
      expect_load_fail("[[process]]\nname = \"p\"\ncmd = \"/bin/true\"\n[process.resources]\nmem_max_mb = \"64\"\n");
      expect_load_fail(
          "[[process]]\nname = \"p\"\ncmd = \"/bin/true\"\n[process.resources]\nvmem_max_mb = 17592186044416\n");
      expect_load_fail("[[component]]\nname = \"c\"\nlib = \"l\"\nclass = \"C\"\ncondition = \"a != b !=\"\n");
      expect_load_fail("[args.x]\ndefault = \"1\"\nchoice = [\"1\"]\n");
      expect_load_fail("[params]\nmixed = [true, 1]\n");
      expect_load_fail(
          "[[process]]\nname = \"p\"\ncmd = \"/bin/true\"\n[process.resources]\ncpu_quota_percent = 12.5\n");
      expect_load_fail("[[process]]\nname = \"p\"\ncmd = \"/bin/true\"\n[process.ready]\ndelay_ms = 4294967296\n");
      expect_load_fail(
          "[[scheduler]]\nname = \"shr\"\nmode = \"shared\"\n"
          "[[component]]\nname = \"c\"\nlib = \"l\"\nclass = \"C\"\npool = \"shr\"\n");

      // 8c. [liveness].enabled = false accepted, [args] $${...} unescaped, remap target scheme:/// normalized
      const std::string ext_toml = R"(
[launcher]
name = "ut-ext-reg"
ctrl = false
namespace = "bot"

[args.tpl]
default = "$${pid}"

[remaps]
"dds://local" = "dds:///shared"

[[component]]
name = "c_ext"
lib = "launcher_test_plugin"
class = "TestListener"
[component.params]
arg_val = "${arg.tpl}"
[component.outputs]
out = "dds://local"

[[process]]
name = "p_live_off"
cmd = "/bin/true"
[process.liveness]
enabled = false
type = "file"
path = "/tmp/nonexistent"
)";
      vlink::LauncherServer srv_ext(cfg);
      std::string err;
      REQUIRE(srv_ext.load_string(ext_toml, tmp, &err));
      const auto pre_start = srv_ext.start_unit("bot/p_live_off");
      CHECK(pre_start.code == vlink::launcher::ErrorCode::kInvalidArgument);
      const auto params_res = srv_ext.get_params("bot/c_ext");
      REQUIRE(params_res.ok());
      bool saw_unescaped_arg = false;

      for (const auto& p : params_res.params) {
        if (p.key == "arg_val") {
          saw_unescaped_arg = (p.value == "${pid}");
        }
      }

      CHECK(saw_unescaped_arg);
      const std::string insp_json = srv_ext.inspect_manifest_json();
      CHECK(insp_json.find("dds://shared") != std::string::npos);
      CHECK(insp_json.find("dds:///shared") == std::string::npos);

      // 8d. Second start() after stop() re-launches root units cleanly
      const std::string restart_toml = R"(
[launcher]
name = "ut-second-start"
ctrl = false
plugin_dirs = [")" + plugin_dir + R"("]

[[component]]
name = "c_again"
lib = "launcher_test_plugin"
class = "TestListener"
)";
      vlink::LauncherServer srv_re(cfg);
      REQUIRE(srv_re.load_string(restart_toml, tmp, &err));
      REQUIRE(srv_re.start());
      REQUIRE(wait_until(
          [&] {
            const auto s = srv_re.get_snapshot();
            return !s.units.empty() && s.units[0].state == vlink::launcher::UnitState::kReady;
          },
          5000));
      srv_re.stop();

      REQUIRE(srv_re.start());
      REQUIRE(wait_until(
          [&] {
            const auto s = srv_re.get_snapshot();
            return !s.units.empty() && s.units[0].state == vlink::launcher::UnitState::kReady;
          },
          5000));
      srv_re.stop();

      // 8e. Legacy RunablePluginInterface stop + restart / unload + reload succeeds cleanly
      const std::string leg_toml = R"(
[launcher]
name = "ut-legacy-reload"
ctrl = false
plugin_dirs = [")" + plugin_dir + R"("]

[[component]]
name = "leg_static"
lib = "launcher_legacy_plugin"
legacy = true
)";
      vlink::LauncherServer srv_leg(cfg);
      REQUIRE(srv_leg.load_string(leg_toml, tmp, &err));
      REQUIRE(srv_leg.start());
      REQUIRE(wait_until(
          [&] {
            const auto s = srv_leg.get_snapshot();
            return !s.units.empty() && s.units[0].state == vlink::launcher::UnitState::kReady;
          },
          5000));
      REQUIRE(srv_leg.unload_component("leg_static").ok());
      vlink::launcher::LoadComponentRequest leg_req;
      leg_req.name = "leg_dyn";
      leg_req.library = "launcher_legacy_plugin";
      leg_req.legacy_runnable = true;
      REQUIRE(srv_leg.load_component(leg_req).ok());
      REQUIRE(srv_leg.unload_component("leg_dyn").ok());
      srv_leg.stop();
    }
  }

  TEST_CASE("hardening P0: watchdog executor hang isolation, shared poison vs dedicated fresh-loop respawn") {
    const std::string tmp = make_temp_dir("p0_watchdog_hang");
    const std::string plugin_dir = vlink::Utils::get_app_dir();
    const std::string ctrl_ns = "ut_wd_" + vlink::Utils::get_pid_str();
    const std::string gate_shared = tmp + "/gate_shared.flag";
    const std::string gate_ded = tmp + "/gate_ded.flag";
    const std::string hang_once = tmp + "/hang_once.flag";

    const std::string manifest = R"(
[launcher]
name = "ut-p0-watchdog"
ctrl_scheme = "intra"
ctrl_namespace = ")" + ctrl_ns + R"("
stop_timeout_ms = 2000
plugin_dirs = [")" + plugin_dir + R"("]

[logger]
dir = ")" + tmp + R"(/logs"

[[scheduler]]
name = "healthy_sched"
mode = "dedicated"

[[scheduler]]
name = "poison_shared"
mode = "shared"
threads = 1

[[scheduler]]
name = "respawn_ded"
mode = "dedicated"

[[component]]
name = "healthy_sibling"
lib = "launcher_test_plugin"
class = "TestTalker"
scheduler = "healthy_sched"
watchdog_ms = 2000
[component.params]
topic = "intra://ut/wd/healthy"
interval_ms = 50

[[component]]
name = "hung_shared"
lib = "launcher_test_plugin"
class = "TestHangingWatchdog"
scheduler = "poison_shared"
watchdog_ms = 250
restart = "on_failure"
restart_delay_ms = 50
[component.params]
gate_file = ")" + gate_shared + R"("
max_block_ms = 8000

[[component]]
name = "hung_dedicated"
lib = "launcher_test_plugin"
class = "TestHangingWatchdog"
scheduler = "respawn_ded"
watchdog_ms = 250
restart = "on_failure"
restart_delay_ms = 80
[component.params]
gate_file = ")" + gate_ded + R"("
hang_once_file = ")" + hang_once +
                                 R"("
max_block_ms = 8000
)";

    vlink::LauncherServer::Config srv_cfg;
    srv_cfg.manage_console = false;
    srv_cfg.log_dir_override = tmp + "/logs";

    vlink::LauncherServer server(srv_cfg);
    std::string err;
    REQUIRE(server.load_string(manifest, tmp, &err));
    REQUIRE(server.start());

    vlink::LauncherAPI::Config api_cfg;
    api_cfg.scheme = "intra";
    api_cfg.namespace_prefix = ctrl_ns;
    vlink::LauncherAPI api(api_cfg);
    api.async_run();
    REQUIRE(api.wait_for_connected(3000U));

    REQUIRE(wait_until(
        [&]() {
          const auto res = api.get_status(1500U);

          if (!res.ok()) {
            return false;
          }

          bool sibling_ready = false;
          bool shared_poisoned = false;
          bool ded_recovered = false;

          for (const auto& u : res.snapshot.units) {
            if (u.name == "healthy_sibling" && u.state == vlink::launcher::UnitState::kReady) {
              sibling_ready = true;
            } else if (u.name == "hung_shared" && u.state == vlink::launcher::UnitState::kFailed &&
                       u.failed_reason == "watchdog_executor_hung") {
              shared_poisoned = true;
            } else if (u.name == "hung_dedicated" && u.state == vlink::launcher::UnitState::kReady &&
                       u.restart_count >= 1U) {
              ded_recovered = true;
            }
          }

          return sibling_ready && shared_poisoned && ded_recovered;
        },
        6000));

    CHECK(api.is_connected());
    const auto st_now = api.get_status(1500U);
    REQUIRE(st_now.ok());

    {
      std::ofstream f1(gate_shared, std::ios::out | std::ios::trunc);
      f1 << "release\n";
      std::ofstream f2(gate_ded, std::ios::out | std::ios::trunc);
      f2 << "release\n";
    }

    api.quit(true);
    (void)api.wait_for_quit(3000, false);
    server.stop();
    CHECK(server.wait(5000));
  }

  TEST_CASE("hardening P1: unified event-triggered requires closure, kSkipped blocking & part_of partial restart") {
    const std::string tmp = make_temp_dir("p1_deps_partof");
    const std::string plugin_dir = vlink::Utils::get_app_dir();

    const std::string dep_manifest = R"(
[launcher]
name = "ut-p1-deps"
ctrl = false
plugin_dirs = [")" + plugin_dir + R"("]

[logger]
dir = ")" + tmp + R"(/logs"

[[component]]
name = "trigger_src"
lib = "launcher_test_plugin"
class = "TestListener"
on_ready = ["target_b"]

[[component]]
name = "prereq_c"
lib = "launcher_test_plugin"
class = "TestListener"
autostart = false

[[component]]
name = "target_b"
lib = "launcher_test_plugin"
class = "TestListener"
autostart = false
requires = ["prereq_c"]
after = ["prereq_c"]

[[component]]
name = "disabled_dep"
lib = "launcher_test_plugin"
class = "TestListener"
condition = false

[[component]]
name = "blocked_by_skipped"
lib = "launcher_test_plugin"
class = "TestListener"
requires = ["disabled_dep"]
after = ["disabled_dep"]
)";

    vlink::LauncherServer::Config cfg;
    cfg.manage_console = false;
    cfg.enable_control_plane = false;
    cfg.log_dir_override = tmp + "/logs";

    {
      vlink::LauncherServer srv(cfg);
      std::string err;
      REQUIRE(srv.load_string(dep_manifest, tmp, &err));
      REQUIRE(srv.start());

      REQUIRE(wait_until(
          [&]() {
            const auto snap = srv.get_snapshot();
            bool c_ready = false;
            bool b_ready = false;
            bool blocked_failed = false;

            for (const auto& u : snap.units) {
              if (u.name == "prereq_c" && u.state == vlink::launcher::UnitState::kReady) {
                c_ready = true;
              } else if (u.name == "target_b" && u.state == vlink::launcher::UnitState::kReady) {
                b_ready = true;
              } else if (u.name == "blocked_by_skipped" && u.state == vlink::launcher::UnitState::kSkipped &&
                         u.failed_reason == "dependency_failed") {
                blocked_failed = true;
              }
            }

            return c_ready && b_ready && blocked_failed;
          },
          4000));
      srv.stop();
    }

    const std::string part_of_manifest = R"(
[launcher]
name = "ut-p1-partof"
ctrl = false
plugin_dirs = [")" + plugin_dir + R"("]

[logger]
dir = ")" + tmp + R"(/partof_logs"

[[component]]
name = "main_svc"
lib = "launcher_test_plugin"
class = "TestListener"

[[component]]
name = "member_standby"
lib = "launcher_test_plugin"
class = "TestManagedPort"
init_state = "inactive"
part_of = ["main_svc"]

[[component]]
name = "member_flaky"
lib = "launcher_test_plugin"
class = "TestListener"
part_of = ["main_svc"]
start_burst = 1
start_window_ms = 60000
)";

    {
      vlink::LauncherServer srv(cfg);
      std::string err;
      REQUIRE(srv.load_string(part_of_manifest, tmp, &err));
      REQUIRE(srv.start());

      REQUIRE(wait_until(
          [&]() {
            const auto snap = srv.get_snapshot();
            bool m_ok = false;
            bool s_inact = false;
            bool f_ok = false;

            for (const auto& u : snap.units) {
              if (u.name == "main_svc" && u.state == vlink::launcher::UnitState::kReady) {
                m_ok = true;
              } else if (u.name == "member_standby" && u.state == vlink::launcher::UnitState::kInactive) {
                s_inact = true;
              } else if (u.name == "member_flaky" && u.state == vlink::launcher::UnitState::kReady) {
                f_ok = true;
              }
            }

            return m_ok && s_inact && f_ok;
          },
          4000));

      const auto rres = srv.restart_unit("main_svc");
      CHECK_FALSE(rres.ok());
      CHECK(rres.message.find("member_flaky") != std::string::npos);

      const auto snap = srv.get_snapshot();

      for (const auto& u : snap.units) {
        if (u.name == "member_standby") {
          CHECK(u.state == vlink::launcher::UnitState::kInactive);
        }
      }

      srv.stop();
    }
  }

  TEST_CASE(
      "hardening P1: strict TOML types, capacity 1024 guard, DOT/Mermaid escaping, CPU isolation & loan/timeout") {
    const std::string tmp = make_temp_dir("p1_strict_cpu_loan");
    const std::string plugin_dir = vlink::Utils::get_app_dir();

    vlink::LauncherServer::Config cfg;
    cfg.manage_console = false;
    cfg.enable_control_plane = false;
    cfg.log_dir_override = tmp + "/logs";

    auto expect_fail = [&](const std::string& toml_text) {
      vlink::LauncherServer s(cfg);
      std::string e;
      CHECK_FALSE(s.load_string(toml_text, tmp, &e));
      CHECK_FALSE(e.empty());
    };

    expect_fail("[[process]]\nname = \"p\"\ncmd = \"/bin/echo\"\nrequires = \"calibration\"\n");
    expect_fail("[[process]]\nname = \"p\"\ncmd = \"/bin/echo\"\nrequires = [\"ok\", 123]\n");
    expect_fail("[[process]]\nname = \"p\"\ncmd = \"/bin/echo\"\nautostart = \"false\"\n");
    expect_fail("[launcher]\nstartup = \"max_parallel\"\nmax_parallel = 4294967296\n");
    expect_fail("[launcher]\nstartup_mode = \"serial\"\n");

    {
      std::string cap_toml =
          "[launcher]\nname = \"ut-cap-1024\"\nctrl = false\nplugin_dirs = [\"" + plugin_dir + "\"]\n";

      for (int i = 0; i < 4; ++i) {
        cap_toml += "[[process]]\nname = \"p" + std::to_string(i) +
                    "_${index}\"\ncount = 256\ncmd = \"/bin/true\"\nautostart = false\n";
      }

      vlink::LauncherServer srv(cfg);
      std::string err;
      REQUIRE(srv.load_string(cap_toml, tmp, &err));
      REQUIRE(srv.start());

      vlink::launcher::LoadComponentRequest over;
      over.name = "overflow_1025";
      over.library = "launcher_test_plugin";
      over.class_name = "TestListener";
      const auto res = srv.load_component(over);
      CHECK_FALSE(res.ok());
      CHECK(res.code == vlink::launcher::ErrorCode::kInvalidArgument);
      CHECK(srv.get_snapshot().units.size() == 1024U);
      srv.stop();
    }

    {
      const std::string esc_toml = R"(
[launcher]
name = "ut-escape"
ctrl = false

[[component]]
name = "cam/front"
lib = "launcher_test_plugin"
class = "TestListener"
[component.outputs]
out = "dds://a/b?tag=x&k=y"

[[component]]
name = "cam_front"
lib = "launcher_test_plugin"
class = "TestListener"
after = ["cam/front"]
[component.inputs]
in = "dds://a/b?tag=x&k=y"

[[component]]
name = "remap_c"
namespace = "bot"
lib = "launcher_test_plugin"
class = "TestListener"
[component.remaps]
"dds://bot/local" = "dds:///shared"
[component.outputs]
out = "dds://local"
)";
      vlink::LauncherServer srv(cfg);
      std::string err;
      REQUIRE(srv.load_string(esc_toml, tmp, &err));
      const std::string dot = srv.export_graph(vlink::LauncherServer::GraphFormat::kDot);
      const std::string mmd = srv.export_graph(vlink::LauncherServer::GraphFormat::kMermaid);
      const std::string json_str = srv.inspect_manifest_json();
      CHECK(dot.find("digraph") != std::string::npos);
      CHECK(mmd.find("flowchart TD") != std::string::npos);
      CHECK(mmd.find("u_cam_s_front") != std::string::npos);
      CHECK(mmd.find("u_cam__front") != std::string::npos);
      CHECK(mmd.find("u_cam_s_front --> u_cam__front") != std::string::npos);
      CHECK(json_str.find("dds://shared") != std::string::npos);
      CHECK(json_str.find("dds:///shared") == std::string::npos);
    }

    {
      const std::string rt_toml = R"(
[launcher]
name = "ut-cpu-loan-timeout"
ctrl = false
plugin_dirs = [")" + plugin_dir + R"("]

[logger]
dir = ")" + tmp + R"(/cpu_logs"

[[scheduler]]
name = "ded_busy"
mode = "dedicated"

[[scheduler]]
name = "ded_idle"
mode = "dedicated"

[[scheduler]]
name = "multi_tuned"
mode = "multi"
threads = 2
cpus = [0]
policy = "other"

[[scheduler]]
name = "timeout_sched"
mode = "dedicated"
max_elapsed_ms = 5

[[component]]
name = "busy_c"
lib = "launcher_test_plugin"
class = "TestCpuIsolation"
scheduler = "ded_busy"
[component.params]
busy = true

[[component]]
name = "idle_c"
lib = "launcher_test_plugin"
class = "TestCpuIsolation"
scheduler = "ded_idle"
[component.params]
busy = false

[[component]]
name = "multi_tuned_c"
lib = "launcher_test_plugin"
class = "TestListener"
scheduler = "multi_tuned"

[[component]]
name = "loan_timeout_c"
lib = "launcher_test_plugin"
class = "TestTimeoutAndLoan"
scheduler = "timeout_sched"
)";
      vlink::LauncherServer srv(cfg);
      std::string err;
      REQUIRE(srv.load_string(rt_toml, tmp, &err));
      REQUIRE(srv.start());

      REQUIRE(wait_until(
          [&]() {
            const auto snap = srv.get_snapshot();
            size_t ready = 0U;

            for (const auto& u : snap.units) {
              if (u.state == vlink::launcher::UnitState::kReady) {
                ++ready;
              }
            }

            return ready == 4U;
          },
          4000));

      CHECK(srv.set_param("loan_timeout_c", vlink::launcher::encode_param_entry("probe_loan", true)).ok());
      CHECK(srv.set_param("loan_timeout_c", vlink::launcher::encode_param_entry("probe_timeout_exec", true)).ok());
      CHECK(wait_until(
          [&]() {
            return srv.set_param("loan_timeout_c", vlink::launcher::encode_param_entry("expect_timeout_exec", true))
                .ok();
          },
          2000));

      REQUIRE(srv.deactivate_unit("loan_timeout_c").ok());
      CHECK(srv.set_param("loan_timeout_c", vlink::launcher::encode_param_entry("probe_loan_inactive", true)).ok());

      double busy_cpu = -1.0;
      double idle_cpu = -1.0;
      (void)wait_until(
          [&]() {
            const auto snap = srv.get_snapshot();

            for (const auto& u : snap.units) {
              if (u.name == "busy_c") {
                busy_cpu = u.cpu_usage;
              } else if (u.name == "idle_c") {
                idle_cpu = u.cpu_usage;
              }
            }

            return busy_cpu > 5.0;
          },
          3000);

      CHECK(busy_cpu > 5.0);
      CHECK(idle_cpu < 15.0);
      srv.stop();
    }
  }

#if !defined(_WIN32) && !defined(_WIN64)
  TEST_CASE("hardening P1: real process tree reclamation on natural crash & pre-exec strict rlimit") {
    const std::string tmp = make_temp_dir("p1_proc_tree");
    const std::string gpid_file = tmp + "/grandchild.pid";
    const std::string nofile_out = tmp + "/nofile.txt";

    const std::string manifest = R"(
[launcher]
name = "ut-proc-tree"
ctrl = false

[logger]
dir = ")" + tmp + R"(/logs"

[[process]]
name = "spawner_crash"
cmd = "/bin/sh"
args = ["-c", "sleep 60 & echo $! > )" +
                                 gpid_file + R"(; sleep 0.2; exit 1"]
kill_mode = "control-group"
restart = "no"
output = "log"

[[task]]
name = "preexec_rlimit"
kind = "oneshot"
cmd = "/bin/sh"
args = ["-c", "ulimit -n > )" + nofile_out +
                                 R"("]
output = "log"
[task.resources]
nofile = 64
strict = true
)";

    vlink::LauncherServer::Config cfg;
    cfg.manage_console = false;
    cfg.enable_control_plane = false;
    cfg.log_dir_override = tmp + "/logs";

    vlink::LauncherServer srv(cfg);
    std::string err;
    REQUIRE(srv.load_string(manifest, tmp, &err));
    REQUIRE(srv.start());

    REQUIRE(wait_until(
        [&]() {
          const auto snap = srv.get_snapshot();
          bool crash_failed = false;
          bool rlimit_done = false;

          for (const auto& u : snap.units) {
            if (u.name == "spawner_crash" && u.state == vlink::launcher::UnitState::kFailed) {
              crash_failed = true;
            } else if (u.name == "preexec_rlimit" && u.state == vlink::launcher::UnitState::kStopped) {
              rlimit_done = true;
            }
          }

          return crash_failed && rlimit_done;
        },
        5000));

    REQUIRE(std::filesystem::exists(gpid_file));
    int gpid = -1;
    {
      std::ifstream ifs(gpid_file);
      ifs >> gpid;
    }
    REQUIRE(gpid > 1);
    const bool grandchild_reaped = wait_until([&]() { return ::kill(static_cast<pid_t>(gpid), 0) != 0; }, 2000);
    CHECK(grandchild_reaped);

    if (!grandchild_reaped) {
      (void)::kill(static_cast<pid_t>(gpid), SIGKILL);
    }

    REQUIRE(std::filesystem::exists(nofile_out));
    std::string nofile_val;
    {
      std::ifstream ifs(nofile_out);
      ifs >> nofile_val;
    }
    CHECK(nofile_val == "64");

    srv.stop();
  }

#if defined(VLINK_SUPPORT_DDS) || defined(VLINK_SUPPORT_DDSC) || defined(VLINK_SUPPORT_ZENOH)
  TEST_CASE("hardening P1: real child container named-component readiness & binds_to cascade") {
    const std::string bin_dir = vlink::Utils::get_app_dir();
    const std::filesystem::path launcher_bin = std::filesystem::path(bin_dir) / "vlink-launcher";

    if (!std::filesystem::exists(launcher_bin)) {
      return;
    }

#if defined(VLINK_SUPPORT_DDS)
    const std::string scheme = "dds";
#elif defined(VLINK_SUPPORT_DDSC)
    const std::string scheme = "ddsc";
#else
    const std::string scheme = "zenoh";
#endif
    const uint32_t domain_id = 120U + (static_cast<uint32_t>(vlink::Utils::get_pid()) % 60U);
    const std::string tmp = make_temp_dir("p1_real_container");
    const std::string ctrl_ns = "ut_cnt_" + vlink::Utils::get_pid_str();
    const std::string ready_flag = tmp + "/inner_ready.flag";

    const std::string manifest = R"(
[launcher]
name = "ut-real-container"
ctrl = true
ctrl_scheme = ")" + scheme + R"("
ctrl_namespace = ")" + ctrl_ns + R"("
ctrl_domain = )" + std::to_string(domain_id) +
                                 R"(
bind_ip = "127.0.0.1"
native = true
plugin_dirs = [")" + bin_dir + R"("]

[logger]
dir = ")" + tmp + R"(/logs"

[[group]]
name = "inner_grp"

[[container]]
name = "worker_box"
output = "log"

[[component]]
name = "inner_slow"
group = "inner_grp"
container = "worker_box"
lib = "launcher_test_plugin"
class = "TestSlowReady"
[component.params]
ready_file = ")" + ready_flag + R"("
[component.ready]
type = "component"
timeout_ms = 10000

[[component]]
name = "host_consumer"
lib = "launcher_test_plugin"
class = "TestListener"
depends_on = ["inner_slow"]
binds_to = ["inner_slow"]
)";

    vlink::LauncherServer::Config cfg;
    cfg.manage_console = false;
    cfg.log_dir_override = tmp + "/logs";
    cfg.launcher_executable = launcher_bin.string();

    vlink::LauncherServer srv(cfg);
    std::string err;
    REQUIRE(srv.load_string(manifest, tmp, &err));
    REQUIRE(srv.start());

    auto state_of = [&](const std::string& uname) {
      const auto snap = srv.get_snapshot();

      for (const auto& u : snap.units) {
        if (u.name == uname) {
          return u.state;
        }
      }

      return vlink::launcher::UnitState::kPending;
    };

    REQUIRE(wait_until([&]() { return state_of("worker_box") == vlink::launcher::UnitState::kReady; }, 10000));

    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(state_of("host_consumer") == vlink::launcher::UnitState::kPending);

    {
      std::ofstream ofs(ready_flag, std::ios::out | std::ios::trunc);
      ofs << "go\n";
    }

    REQUIRE(wait_until([&]() { return state_of("host_consumer") == vlink::launcher::UnitState::kReady; }, 8000));

    CHECK(srv.stop_unit("inner_slow").ok());
    REQUIRE(wait_until([&]() { return state_of("host_consumer") == vlink::launcher::UnitState::kStopped; }, 6000));

    CHECK(srv.start_unit("host_consumer").ok());
    REQUIRE(wait_until([&]() { return state_of("host_consumer") == vlink::launcher::UnitState::kReady; }, 8000));

    srv.stop();
    CHECK(srv.wait(6000));
  }
#endif

  TEST_CASE("hardening P2: CLI subcommands smoke (validate, graph, completion, offline logs exact unit)") {
    const std::string bin_dir = vlink::Utils::get_app_dir();
    const std::filesystem::path launcher_bin = std::filesystem::path(bin_dir) / "vlink-launcher";

    if (!std::filesystem::exists(launcher_bin)) {
      return;
    }

    const std::string tmp = make_temp_dir("p2_cli_smoke");
    const std::string log_dir = tmp + "/session";
    std::filesystem::create_directories(log_dir + "/captured");
    {
      std::ofstream f1(log_dir + "/captured/cam_1.log");
      f1 << "2026-11-26 10:00:01.000 [INF] CAM1_LINE\ncontinuation without timestamp\n";
      std::ofstream f2(log_dir + "/captured/cam_10.log");
      f2 << "unprefixed header line\n2026-11-26 10:00:00.500 [INF] CAM10_LINE\n";
    }

    const std::string out_file = tmp + "/cli_out.txt";
    const std::string cmd_logs =
        "\"" + launcher_bin.string() + "\" logs \"" + log_dir + "\" -u cam_1 > \"" + out_file + "\"";
    CHECK(std::system(cmd_logs.c_str()) == 0);

    std::ifstream ifs(out_file);
    const std::string logs_out((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    CHECK(logs_out.find("CAM1_LINE") != std::string::npos);
    CHECK(logs_out.find("CAM10_LINE") == std::string::npos);

    const std::string comp_out = tmp + "/comp.zsh";
    const std::string cmd_comp = "\"" + launcher_bin.string() + "\" completion zsh > \"" + comp_out + "\"";
    CHECK(std::system(cmd_comp.c_str()) == 0);
    std::ifstream cfs(comp_out);
    const std::string zsh_txt((std::istreambuf_iterator<char>(cfs)), std::istreambuf_iterator<char>());
    CHECK(zsh_txt.find("CURRENT == 3") != std::string::npos);
  }
#endif

  TEST_CASE("coverage: config strict aliases, conditions, UDS/remap URLs, container stage DAG & serial group") {
    auto load_err = [](const std::string& toml) -> std::string {
      vlink::LauncherServer::Config cfg;
      cfg.manage_console = false;
      vlink::LauncherServer srv(cfg);
      std::string err;
      const bool ok = srv.load_string(toml, "", &err);
      return ok ? std::string() : err;
    };

    CHECK(!load_err("[launcher]\nctrl_scheme = "
                    "\"intra\"\n[[process]]\nname=\"p\"\ncmd=\"/bin/true\"\nsigterm_timeout_ms=100\n")
               .empty());
    CHECK(!load_err("[launcher]\nctrl_scheme = \"intra\"\n[[process]]\nname=\"p\"\ncommand=\"/bin/true\"\n").empty());
    CHECK(!load_err("[launcher]\nctrl_scheme = \"intra\"\n[[component]]\nname=\"c\"\nlibrary=\"x\"\nclass=\"Y\"\n")
               .empty());
    CHECK(
        !load_err(
             "[launcher]\nctrl_scheme = \"intra\"\n[[component]]\nname=\"c\"\nlib=\"x\"\nclass=\"Y\"\nworker_index=0\n")
             .empty());
    CHECK(!load_err("[launcher]\nenable_control_plane = false\n").empty());
    CHECK(!load_err("[launcher]\ncontrol_scheme = \"intra\"\n").empty());

    CHECK(
        !load_err("[launcher]\nctrl_scheme = \"intra\"\n[[process]]\nname=\"p\"\ncmd=\"/bin/true\"\ncondition=\"()\"\n")
             .empty());
    CHECK(!load_err("[launcher]\nctrl_scheme = "
                    "\"intra\"\n[[process]]\nname=\"p\"\ncmd=\"/bin/true\"\ncondition=\"(true) extra\"\n")
               .empty());
    CHECK(!load_err(
               "[launcher]\nctrl_scheme = \"intra\"\n[[process]]\nname=\"p\"\ncmd=\"/bin/true\"\ncondition=\"a ==\"\n")
               .empty());
    CHECK(load_err(R"TOML(
[launcher]
ctrl_scheme = "intra"
[args]
mode = "sim"
[[process]]
name = "cond_ok"
cmd = "/bin/true"
condition = "(false || true) && !(${arg.mode} == prod)"
)TOML")
              .empty());

    const std::string stage_cycle_err = load_err(R"(
[launcher]
ctrl_scheme = "dds"
[[container]]
name = "box"
[[component]]
name = "c1"
container = "box"
lib = "launcher_test_plugin"
class = "TestRollback"
stage = 0
after = ["c2"]
[[component]]
name = "c2"
container = "box"
lib = "launcher_test_plugin"
class = "TestRollback"
stage = 1
)");
    CHECK(stage_cycle_err.find("local ordering cycle") != std::string::npos);

    const std::string uds_tmp = make_temp_dir("cov_uds_remap");
    const std::string uds_out = uds_tmp + "/urls.txt";
    const std::string uds_remap_toml = R"(
[launcher]
ctrl_scheme = "intra"
namespace = "bot"

[remaps]
"dds://shared" = "dds://remapped"

[[task]]
name = "u1"
cmd = "/bin/sh"
args = ["-c", "printf '%s|%s|%s\n' \"$VLINK_URL_SOCK_LO\" \"$VLINK_URL_SOCK_HI\" \"$VLINK_URL_SHARED_EP\" > \")" +
                                       uds_out + R"(\""]
[task.urls]
sock_lo = "uds:///tmp/a.sock"
sock_hi = "UDS:///tmp/b.sock"
shared_ep = "dds:///shared/x"
)";
    vlink::LauncherServer::Config cfg;
    cfg.manage_console = false;
    cfg.log_dir_override = uds_tmp + "/logs";
    vlink::LauncherServer srv(cfg);
    std::string err;
    REQUIRE(srv.load_string(uds_remap_toml, uds_tmp, &err));
    const auto cfg_res = srv.get_unit_config("bot/u1");
    REQUIRE(cfg_res.ok());
#if !defined(_WIN32) && !defined(_WIN64)
    REQUIRE(srv.start());
    REQUIRE(wait_until([&]() { return std::filesystem::exists(uds_out); }, 4000));
    srv.stop();
    CHECK(srv.wait(4000));
    std::ifstream ufs(uds_out);
    const std::string uline((std::istreambuf_iterator<char>(ufs)), std::istreambuf_iterator<char>());
    CHECK(uline.find("uds:///tmp/a.sock|UDS:///tmp/b.sock|dds://remapped/x") != std::string::npos);
#endif
  }

  TEST_CASE("coverage: LauncherTimerComponent malformed interval rejection, runtime update & fail_tick") {
    const std::string plugin_dir = vlink::Utils::get_app_dir();
    const std::string tmp = make_temp_dir("cov_timer");
    const std::string ns = "ut_cov_timer_" + vlink::Utils::get_pid_str();

    const std::string toml = R"(
[launcher]
ctrl_scheme = "intra"
ctrl_namespace = ")" + ns + R"("
plugin_dirs = [")" + plugin_dir +
                             R"("]

[[component]]
name = "bad_timer_str"
lib = "launcher_test_plugin"
class = "TestTimerEdge"
[component.params]
interval_ms = "abc"

[[component]]
name = "bad_timer_zero"
lib = "launcher_test_plugin"
class = "TestTimerEdge"
[component.params]
interval_ms = 0

[[component]]
name = "good_timer"
lib = "launcher_test_plugin"
class = "TestTimerEdge"
[component.params]
interval_ms = 30
)";

    vlink::LauncherServer::Config cfg;
    cfg.manage_console = false;
    cfg.log_dir_override = tmp + "/logs";
    vlink::LauncherServer srv(cfg);
    std::string err;
    REQUIRE(srv.load_string(toml, tmp, &err));
    REQUIRE(srv.start());

    auto find_unit = [&](const std::string& name) -> vlink::launcher::UnitStatus {
      const auto snap = srv.get_snapshot();

      for (const auto& u : snap.units) {
        if (u.name == name) {
          return u;
        }
      }

      return {};
    };

    REQUIRE(wait_until(
        [&]() {
          return find_unit("bad_timer_str").state == vlink::launcher::UnitState::kFailed &&
                 find_unit("bad_timer_zero").state == vlink::launcher::UnitState::kFailed &&
                 find_unit("good_timer").state == vlink::launcher::UnitState::kReady;
        },
        6000));

    CHECK(srv.set_param("good_timer", vlink::launcher::encode_param_entry("interval_ms", int64_t{15})).ok());
    CHECK(!srv.set_param("good_timer", vlink::launcher::encode_param_entry("interval_ms", int64_t{0})).ok());
    CHECK(!srv.set_param("good_timer", vlink::launcher::encode_param_entry("interval_ms", std::string("bad"))).ok());
    CHECK(srv.set_param("good_timer", vlink::launcher::encode_param_entry("fail_tick", true)).ok());
    REQUIRE(wait_until(
        [&]() {
          const auto u = find_unit("good_timer");
          return !u.healthy && u.detail.find("timer tick returned false") != std::string::npos;
        },
        3000));

    srv.stop();
    CHECK(srv.wait(5000));
  }

  TEST_CASE("coverage: ComponentHost ports/move, timeout rollback, exception recovery & kDeferredDrain") {
    const std::string plugin_dir = vlink::Utils::get_app_dir();
    const std::string tmp = make_temp_dir("cov_rollback");
    const std::string ns = "ut_cov_rb_" + vlink::Utils::get_pid_str();
    const std::string marker = tmp + "/markers.txt";
    const std::string drain_marker = tmp + "/drain_markers.txt";

    const std::string toml = R"(
[launcher]
ctrl_scheme = "intra"
ctrl_namespace = ")" + ns + R"("
plugin_dirs = [")" + plugin_dir +
                             R"("]
remote_load = true

[[scheduler]]
name = "rb_sched"
mode = "pinned"
threads = 2

[[component]]
name = "rb_comp"
lib = "launcher_test_plugin"
class = "TestRollback"
scheduler = "rb_sched"
init_state = "inactive"
reload = "component"
[component.params]
marker_file = ")" + marker + R"("
test_ports = true

[[component]]
name = "drain_comp"
lib = "launcher_test_plugin"
class = "TestRollback"
scheduler = "rb_sched"
[component.params]
marker_file = ")" + drain_marker +
                             R"("
drain_sleep_ms = 350
)";

    vlink::LauncherServer::Config cfg;
    cfg.manage_console = false;
    cfg.log_dir_override = tmp + "/logs";
    vlink::LauncherServer srv(cfg);
    std::string err;
    REQUIRE(srv.load_string(toml, tmp, &err));
    REQUIRE(srv.start());

    auto state_of = [&](const std::string& name) {
      for (const auto& u : srv.get_snapshot().units) {
        if (u.name == name) {
          return u.state;
        }
      }

      return vlink::launcher::UnitState::kPending;
    };

    REQUIRE(wait_until(
        [&]() {
          return state_of("rb_comp") == vlink::launcher::UnitState::kInactive &&
                 state_of("drain_comp") == vlink::launcher::UnitState::kReady;
        },
        6000));

    CHECK(srv.set_param("rb_comp", vlink::launcher::encode_param_entry("fail_activate", true)).ok());
    CHECK(!srv.activate_unit("rb_comp").ok());
    CHECK(state_of("rb_comp") == vlink::launcher::UnitState::kInactive);

    CHECK(srv.set_param("rb_comp", vlink::launcher::encode_param_entry("fail_activate", false)).ok());
    CHECK(srv.set_param("rb_comp", vlink::launcher::encode_param_entry("throw_activate", true)).ok());
    CHECK(!srv.activate_unit("rb_comp").ok());
    CHECK(state_of("rb_comp") == vlink::launcher::UnitState::kInactive);

    CHECK(srv.set_param("rb_comp", vlink::launcher::encode_param_entry("throw_activate", false)).ok());
    CHECK(srv.activate_unit("rb_comp").ok());
    CHECK(state_of("rb_comp") == vlink::launcher::UnitState::kReady);

    CHECK(srv.set_param("rb_comp", vlink::launcher::encode_param_entry("reject_reload", true)).ok());
    CHECK(!srv.reload_unit("rb_comp").ok());

    CHECK(srv.set_param("rb_comp", vlink::launcher::encode_param_entry("reject_param", true)).ok());
    CHECK(!srv.set_param("rb_comp", vlink::launcher::encode_param_entry("activate_delay_ms", int64_t{999})).ok());
    CHECK(srv.set_param("rb_comp", vlink::launcher::encode_param_entry("reject_param", false)).ok());

    CHECK(srv.set_param("rb_comp", vlink::launcher::encode_param_entry("throw_deactivate", true)).ok());
    CHECK(!srv.deactivate_unit("rb_comp").ok());
    CHECK(state_of("rb_comp") == vlink::launcher::UnitState::kReady);
    CHECK(srv.set_param("rb_comp", vlink::launcher::encode_param_entry("throw_deactivate", false)).ok());

    CHECK(srv.deactivate_unit("rb_comp").ok());
    CHECK(state_of("rb_comp") == vlink::launcher::UnitState::kInactive);

    auto read_all = [](const std::string& p) {
      std::ifstream ifs(p);
      return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    };
    CHECK(wait_until([&]() { return read_all(marker).find("RECV:zero_copy_ok") != std::string::npos; }, 3000));

    CHECK(srv.stop_unit("drain_comp").ok());
    CHECK(wait_until(
        [&]() {
          const std::string content = read_all(drain_marker);
          return content.find("DRAIN_TASK_DONE") != std::string::npos && content.find("CLEANUP") != std::string::npos;
        },
        4000));

    CHECK(srv.start_unit("drain_comp").ok());
    CHECK(state_of("drain_comp") == vlink::launcher::UnitState::kReady);

    srv.stop();
    CHECK(srv.wait(5000));
  }

  TEST_CASE("coverage: pause_on_fail resume, not-running load, schedule_respawn cancel & API session switch") {
    const std::string plugin_dir = vlink::Utils::get_app_dir();
    const std::string tmp = make_temp_dir("cov_orch");
    const std::string ns = "/ut_cov_orch_" + vlink::Utils::get_pid_str() + "/";

    const std::string toml = R"(
[launcher]
ctrl_scheme = "intra"
ctrl_namespace = ")" + ns + R"("
plugin_dirs = [")" + plugin_dir +
                             R"("]
remote_load = true

[[component]]
name = "stage0_fail"
lib = "launcher_test_plugin"
class = "TestFailOnStart"
stage = 0

[[component]]
name = "stage1_ok"
lib = "launcher_test_plugin"
class = "TestRollback"
stage = 1
)";

    vlink::LauncherServer::Config cfg;
    cfg.manage_console = false;
    cfg.pause_on_fail = true;
    cfg.log_dir_override = tmp + "/logs";
    vlink::LauncherServer srv(cfg);
    std::string err;
    REQUIRE(srv.load_string(toml, tmp, &err));

    vlink::launcher::LoadComponentRequest pre_req;
    pre_req.name = "too_early";
    pre_req.library = "launcher_test_plugin";
    pre_req.class_name = "TestRollback";
    CHECK(srv.load_component(pre_req).code == vlink::launcher::ErrorCode::kNotRunning);

    std::atomic<bool> paused_seen{false};
    srv.register_event_callback([&](const vlink::launcher::EventRecord& ev) {
      if (ev.event_name == "on_startup_paused") {
        paused_seen.store(true, std::memory_order_release);
      }
    });

    REQUIRE(srv.start());
    REQUIRE(wait_until([&]() { return paused_seen.load(std::memory_order_acquire); }, 5000));

    auto state_of = [&](const std::string& name) {
      for (const auto& u : srv.get_snapshot().units) {
        if (u.name == name) {
          return u.state;
        }
      }

      return vlink::launcher::UnitState::kPending;
    };
    CHECK(state_of("stage1_ok") == vlink::launcher::UnitState::kPending);

    vlink::LauncherAPI::Config acfg;
    acfg.scheme = "intra";
    acfg.namespace_prefix = ns;
    acfg.default_timeout_ms = 2000;
    vlink::LauncherAPI api(acfg);
    api.async_run();
    REQUIRE(api.wait_for_connected(4000));

    const std::string first_sid = srv.get_snapshot().session_id;
    REQUIRE(api.resume_startup(3000).ok());
    REQUIRE(wait_until([&]() { return state_of("stage1_ok") == vlink::launcher::UnitState::kReady; }, 5000));

    const auto snap1 = api.get_cached_snapshot();
    CHECK((!snap1.has_value() || snap1->session_id == first_sid));

    srv.stop();
    REQUIRE(srv.wait(5000));

    REQUIRE(srv.start());
    const std::string second_sid = srv.get_snapshot().session_id;
    CHECK(second_sid != first_sid);
    REQUIRE(wait_until(
        [&]() {
          const auto st = api.get_status(1000);
          return st.ok() && st.snapshot.session_id == second_sid;
        },
        5000));
    const auto snap2 = api.get_cached_snapshot();
    CHECK((!snap2.has_value() || snap2->session_id == second_sid));

    api.quit(false);
    api.wait_for_quit();
    srv.stop();
    CHECK(srv.wait(5000));

    vlink::LauncherAPI::Config disc_cfg;
    disc_cfg.scheme = "intra";
    disc_cfg.namespace_prefix = "ut_cov_nonexistent_" + vlink::Utils::get_pid_str();
    disc_cfg.default_timeout_ms = 200;
    vlink::LauncherAPI disc_api(disc_cfg);
    disc_api.async_run();
    CHECK(!disc_api.wait_for_connected(200));
    CHECK(disc_api.get_status(200).code == vlink::launcher::ErrorCode::kDisconnected);
    disc_api.quit(false);
    disc_api.wait_for_quit();
  }

  TEST_CASE("coverage: remote security gates (unauthorized load & set_record traversal)") {
    const std::string plugin_dir = vlink::Utils::get_app_dir();
    const std::string tmp = make_temp_dir("cov_sec");
    const std::string ns = "ut_cov_sec_" + vlink::Utils::get_pid_str();
    const uint32_t dom = static_cast<uint32_t>(140U + (std::hash<std::string>{}(ns) % 40U));

    const std::string toml = R"(
[launcher]
ctrl_scheme = "dds"
ctrl_namespace = ")" + ns + R"("
ctrl_domain = )" + std::to_string(dom) +
                             R"(
native = true
remote_load = false
plugin_dirs = [")" + plugin_dir +
                             R"("]

[record]
dir = ")" + tmp + R"(/rec"

[[component]]
name = "sec_comp"
lib = "launcher_test_plugin"
class = "TestRollback"
)";

    vlink::LauncherServer::Config cfg;
    cfg.manage_console = false;
    cfg.log_dir_override = tmp + "/logs";
    vlink::LauncherServer srv(cfg);
    std::string err;
    REQUIRE(srv.load_string(toml, tmp, &err));
    REQUIRE(srv.start());

    vlink::LauncherAPI::Config acfg;
    acfg.scheme = "dds";
    acfg.namespace_prefix = ns;
    acfg.domain_id = dom;
    acfg.native_mode = true;
    acfg.default_timeout_ms = 3000;
    vlink::LauncherAPI api(acfg);
    api.async_run();
    REQUIRE(api.wait_for_connected(6000));

    vlink::launcher::LoadComponentRequest lreq;
    lreq.name = "unauth_dyn";
    lreq.library = "launcher_test_plugin";
    lreq.class_name = "TestRollback";
    CHECK(api.load_component(lreq, 3000).code == vlink::launcher::ErrorCode::kUnauthorized);

    CHECK(api.set_record("sec_comp", true, "../escape.vcap", 3000).code == vlink::launcher::ErrorCode::kUnauthorized);
    CHECK(api.set_record("sec_comp", true, "/tmp/abs_escape.vcap", 3000).code ==
          vlink::launcher::ErrorCode::kUnauthorized);
    CHECK(api.set_record("sec_comp", true, "sub/safe.vcap", 3000).ok());
    CHECK(api.set_record("sec_comp", false, "", 3000).ok());

    api.quit(false);
    api.wait_for_quit();
    srv.stop();
    CHECK(srv.wait(5000));
  }

#if !defined(_WIN32) && !defined(_WIN64)
  TEST_CASE("coverage: ProcessSupervisor fast-exit, respawn cancel, SIGINT/SIGKILL & env/remap injection") {
    const std::string tmp = make_temp_dir("cov_proc");
    const std::string ns = "ut_cov_proc_" + vlink::Utils::get_pid_str();
    const std::string env_out = tmp + "/env_dump.txt";
    const std::string int_flag = tmp + "/got_sigint.txt";

    const std::string toml = R"(
[launcher]
ctrl_scheme = "intra"
ctrl_namespace = ")" + ns + R"("

[env]
LAYER_VAR = "global"
GLOBAL_ONLY = "g1"

[remaps]
"dds://raw/topic" = "dds://remapped/topic"

[[group]]
name = "env_grp"
members = ["env_task"]
[group.env]
LAYER_VAR = "group"
GROUP_ONLY = "gr1"

[[task]]
name = "fast_oneshot"
cmd = "/bin/true"

[[task]]
name = "env_task"
cmd = "/bin/sh"
args = ["-c", "printf 'LAYER=%s|G=%s|GR=%s|P=%s|URL=%s|REMAP=%s\n' \"$LAYER_VAR\" \"$GLOBAL_ONLY\" \"$GROUP_ONLY\" \"$PROC_ONLY\" \"$VLINK_URL_DATA\" \"$VLINK_URL_REMAP\" > \")" +
                             env_out + R"(\"; cat \"$VLINK_URL_REMAP\" >> \")" + env_out + R"(\""]
[task.env]
LAYER_VAR = "process"
PROC_ONLY = "p1"
[task.urls]
data = "dds://sensor/imu"

[[process]]
name = "fast_crash"
cmd = "/bin/false"
restart = "on_failure"
restart_delay_ms = 50
start_burst = 2
start_window_ms = 10000

[[process]]
name = "respawn_cancel_req"
cmd = "/bin/false"
required = true
restart = "on_failure"
restart_delay_ms = 600
start_burst = 5

[[process]]
name = "sigint_proc"
cmd = "/bin/sh"
args = ["-c", "trap 'echo GOT_INT > \")" +
                             int_flag + R"(\"; exit 0' INT; echo INT_READY; sleep 30 & wait $!"]
stop_signal = "SIGINT"
stop_timeout_ms = 2000
[process.ready]
type = "stdout"
pattern = "INT_READY"

[[process]]
name = "sigkill_proc"
cmd = "/bin/sh"
args = ["-c", "trap '' TERM; echo KILL_READY; while true; do sleep 1; done"]
stop_timeout_ms = 250
[process.ready]
type = "stdout"
pattern = "KILL_READY"
)";

    vlink::LauncherServer::Config cfg;
    cfg.manage_console = false;
    cfg.log_dir_override = tmp + "/logs";
    vlink::LauncherServer srv(cfg);
    std::string err;
    REQUIRE(srv.load_string(toml, tmp, &err));
    REQUIRE(srv.start());

    auto state_of = [&](const std::string& name) {
      for (const auto& u : srv.get_snapshot().units) {
        if (u.name == name) {
          return u.state;
        }
      }

      return vlink::launcher::UnitState::kPending;
    };

    REQUIRE(
        wait_until([&]() { return state_of("respawn_cancel_req") == vlink::launcher::UnitState::kRestarting; }, 4000));
    CHECK(srv.stop_unit("respawn_cancel_req").ok());
    CHECK(state_of("respawn_cancel_req") == vlink::launcher::UnitState::kStopped);
    CHECK(srv.is_active());

    REQUIRE(wait_until(
        [&]() {
          return state_of("fast_oneshot") == vlink::launcher::UnitState::kStopped &&
                 state_of("env_task") == vlink::launcher::UnitState::kStopped &&
                 state_of("fast_crash") == vlink::launcher::UnitState::kFailed &&
                 state_of("sigint_proc") == vlink::launcher::UnitState::kReady &&
                 state_of("sigkill_proc") == vlink::launcher::UnitState::kReady;
        },
        6000));

    std::ifstream ifs(env_out);
    const std::string dump((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    CHECK(dump.find("LAYER=process|G=g1|GR=gr1|P=p1|URL=dds://sensor/imu|REMAP=") != std::string::npos);
    CHECK(dump.find("dds://remapped/topic") != std::string::npos);

    CHECK(srv.stop_unit("sigint_proc").ok());
    CHECK(std::filesystem::exists(int_flag));

    CHECK(srv.stop_unit("sigkill_proc").ok());
    CHECK(state_of("sigkill_proc") == vlink::launcher::UnitState::kStopped);

    srv.stop();
    CHECK(srv.wait(5000));
  }

  TEST_CASE("coverage: remote_req_preds without after topo order & isolate_group container autostart=false") {
    const std::string bin_dir = vlink::Utils::get_app_dir();
    const std::filesystem::path launcher_bin = std::filesystem::path(bin_dir) / "vlink-launcher";

    if (!std::filesystem::exists(launcher_bin)) {
      return;
    }

    const std::string tmp = make_temp_dir("cov_container_topo");
    const std::string ns = "ut_cov_ctopo_" + vlink::Utils::get_pid_str();
    const uint32_t dom = static_cast<uint32_t>(180U + (std::hash<std::string>{}(ns) % 40U));

    const std::string toml = R"(
[launcher]
ctrl_scheme = "dds"
ctrl_namespace = ")" + ns + R"("
ctrl_domain = )" + std::to_string(dom) +
                             R"(
native = true
plugin_dirs = [")" + bin_dir +
                             R"("]

[[group]]
name = "box_only"
members = ["box"]

[[group]]
name = "consumer_only"
members = ["early_consumer"]

[[component]]
name = "early_consumer"
lib = "launcher_test_plugin"
class = "TestRollback"
autostart = false
requires = ["inner_a"]

[[container]]
name = "box"

[[component]]
name = "inner_a"
container = "box"
lib = "launcher_test_plugin"
class = "TestRollback"

[[component]]
name = "inner_b"
container = "box"
lib = "launcher_test_plugin"
class = "TestRollback"

[[component]]
name = "inner_manual"
container = "box"
lib = "launcher_test_plugin"
class = "TestRollback"
autostart = false
)";

    vlink::LauncherServer::Config cfg;
    cfg.manage_console = false;
    cfg.log_dir_override = tmp + "/logs";
    vlink::LauncherServer srv(cfg);
    std::string err;
    REQUIRE(srv.load_string(toml, tmp, &err));
    REQUIRE(srv.start());

    auto state_of = [&](const std::string& name) {
      for (const auto& u : srv.get_snapshot().units) {
        if (u.name == name) {
          return u.state;
        }
      }

      return vlink::launcher::UnitState::kPending;
    };

    REQUIRE(wait_until([&]() { return state_of("inner_a") == vlink::launcher::UnitState::kReady; }, 10000));
    CHECK(state_of("inner_manual") == vlink::launcher::UnitState::kSkipped);

    CHECK(srv.isolate_group("@box_only").ok());
    CHECK(state_of("inner_a") == vlink::launcher::UnitState::kReady);
    CHECK(state_of("inner_manual") == vlink::launcher::UnitState::kSkipped);

    CHECK(srv.stop_unit("box").ok());
    REQUIRE(wait_until([&]() { return state_of("box") == vlink::launcher::UnitState::kStopped; }, 6000));

    CHECK(srv.start_unit("early_consumer").ok());
    REQUIRE(wait_until(
        [&]() {
          return state_of("box") == vlink::launcher::UnitState::kReady &&
                 state_of("early_consumer") == vlink::launcher::UnitState::kReady;
        },
        10000));

    CHECK(srv.isolate_group("@consumer_only").ok());
    REQUIRE(wait_until(
        [&]() {
          return state_of("inner_a") == vlink::launcher::UnitState::kReady &&
                 state_of("inner_b") == vlink::launcher::UnitState::kStopped;
        },
        8000));

    srv.stop();
    CHECK(srv.wait(6000));
  }
#endif

#ifdef VLINK_ENABLE_COROUTINE
  TEST_CASE("coverage: C++20 LauncherCoroutineComponent fail_start, throw recovery & co_sleep cancel") {
    const std::string plugin_dir = vlink::Utils::get_app_dir();
    const std::string tmp = make_temp_dir("cov_coro_edge");
    const std::string ns = "ut_cov_coro_" + vlink::Utils::get_pid_str();
    const std::string cancel_marker = tmp + "/coro_cancel.txt";
    const std::string throw_marker = tmp + "/coro_throw.txt";

    const std::string toml = R"(
[launcher]
ctrl_scheme = "intra"
ctrl_namespace = ")" + ns + R"("
plugin_dirs = [")" + plugin_dir +
                             R"("]

[[component]]
name = "coro_fail_start"
lib = "launcher_test_plugin"
class = "TestCoroEdge"
[component.params]
fail_start = true

[[component]]
name = "coro_throw"
lib = "launcher_test_plugin"
class = "TestCoroEdge"
[component.params]
throw_run = true
marker_file = ")" + throw_marker +
                             R"("

[[component]]
name = "coro_cancel"
lib = "launcher_test_plugin"
class = "TestCoroEdge"
[component.params]
marker_file = ")" + cancel_marker +
                             R"("
)";

    vlink::LauncherServer::Config cfg;
    cfg.manage_console = false;
    cfg.log_dir_override = tmp + "/logs";
    vlink::LauncherServer srv(cfg);
    std::string err;
    REQUIRE(srv.load_string(toml, tmp, &err));
    REQUIRE(srv.start());

    auto state_of = [&](const std::string& name) {
      for (const auto& u : srv.get_snapshot().units) {
        if (u.name == name) {
          return u.state;
        }
      }

      return vlink::launcher::UnitState::kPending;
    };

    REQUIRE(wait_until(
        [&]() {
          return state_of("coro_fail_start") == vlink::launcher::UnitState::kFailed &&
                 state_of("coro_throw") == vlink::launcher::UnitState::kReady &&
                 state_of("coro_cancel") == vlink::launcher::UnitState::kReady;
        },
        6000));

    CHECK(srv.stop_unit("coro_cancel").ok());
    CHECK(srv.stop_unit("coro_throw").ok());

    auto read_all = [](const std::string& p) {
      std::ifstream ifs(p);
      return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    };
    CHECK(wait_until(
        [&]() {
          const std::string c = read_all(cancel_marker);
          return c.find("EXEC_42") != std::string::npos && c.find("SLEEP_CANCELLED") != std::string::npos &&
                 c.find("CORO_STOPPED") != std::string::npos;
        },
        4000));
    CHECK(read_all(throw_marker).find("EXEC_42") != std::string::npos);

    srv.stop();
    CHECK(srv.wait(5000));
  }
#endif
}

// NOLINTEND
