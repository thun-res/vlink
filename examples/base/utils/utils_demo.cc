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

#include <vlink/base/logger.h>
#include <vlink/base/utils.h>

#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// Utils demo
//
// Module:   vlink/base/utils.h
// Scenario: vlink::Utils is a junk-drawer of cross-platform helpers used by
//           the rest of VLink: process / path info, env-var manipulation,
//           network interface enumeration (incl. the DDS-default-address
//           heuristic), singleton lock files, thread naming / priority /
//           tid lookup, CPU/memory usage probes, timezone offset, terminal
//           dimensions, and signal handler registration. This example fires
//           each entry-point once so the surface area is visible at a glance.
// -----------------------------------------------------------------------------
int main() {
  VLOG_I("=== VLink Utils Demo ===");

  // Process & path info: equivalent to argv[0]-based introspection but
  // robust to symlinks and chdir. machine_id is a stable per-host identifier
  // (used by some transports for cross-process discovery).
  {
    VLOG_I("--- Process & path info ---");
    VLOG_I("  pid=", vlink::Utils::get_pid(), " pid_str=", vlink::Utils::get_pid_str());
    VLOG_I("  app_path=", vlink::Utils::get_app_path());
    VLOG_I("  app_dir=", vlink::Utils::get_app_dir());
    VLOG_I("  app_name=", vlink::Utils::get_app_name());
    VLOG_I("  hostname=", vlink::Utils::get_host_name(), " tmp_dir=", vlink::Utils::get_tmp_dir(),
           " machine_id=", vlink::Utils::get_machine_id());
  }

  // Env-var management: get_env returns the default if unset; set_env
  // overwrites (third arg = overwrite_if_present). unset_env removes the
  // binding so a subsequent get_env returns the supplied default again.
  {
    VLOG_I("--- Env vars ---");
    VLOG_I("  HOME=", vlink::Utils::get_env("HOME", "/unknown"));

    vlink::Utils::set_env("VLINK_DEMO_VAR", "hello_vlink", true);
    VLOG_I("  VLINK_DEMO_VAR (set)=", vlink::Utils::get_env("VLINK_DEMO_VAR", "not_found"));

    vlink::Utils::unset_env("VLINK_DEMO_VAR");
    VLOG_I("  VLINK_DEMO_VAR (unset)=", vlink::Utils::get_env("VLINK_DEMO_VAR", "not_found"));
    VLOG_I("  NO_SUCH_VAR_12345=", vlink::Utils::get_env("NO_SUCH_VAR_12345", "default_value"));
  }

  // Network introspection: get_all_ipv4_address(true) filters to interfaces
  // currently up; get_dds_default_address ranks them by the heuristic the
  // DDS transport uses to pick its default outbound interface.
  {
    VLOG_I("--- Network ---");
    auto all = vlink::Utils::get_all_ipv4_address(false);
    VLOG_I("  all ipv4: ", all.size());

    auto up = vlink::Utils::get_all_ipv4_address(true);
    for (const auto& addr : up) {
      VLOG_I("    ", addr, " iface=", vlink::Utils::get_interface_name_by_ipv4(addr));
    }

    auto dds = vlink::Utils::get_dds_default_address(true, 3);
    for (const auto& addr : dds) {
      VLOG_I("  dds default: ", addr);
    }
  }

  // Singleton lock-file: first call grabs the lock and returns true; the
  // second call with the same name returns false (already held by us).
  // The lock is process-scoped; useful to gate "only one instance" tools.
  {
    VLOG_I("--- Singleton check ---");
    VLOG_I("  check_singleton #1=", vlink::Utils::check_singleton("vlink_utils_demo"));
    VLOG_I("  check_singleton #2=", vlink::Utils::check_singleton("vlink_utils_demo"));
  }

  // Thread helpers: name the current thread (shows up in `top -H`, gdb,
  // perf), read the native tid, twiddle scheduling priority. yield_cpu is
  // a hot-spin-friendly equivalent of std::this_thread::yield.
  {
    VLOG_I("--- Thread ---");
    VLOG_I("  set_thread_name=", vlink::Utils::set_thread_name("demo_main") ? "ok" : "fail");
    VLOG_I("  tid=", vlink::Utils::get_native_thread_id());
    VLOG_I("  set_thread_priority(0)=", vlink::Utils::set_thread_priority(0, -1, nullptr) ? "ok" : "fail");

    for (int i = 0; i < 1000; ++i) {
      vlink::Utils::yield_cpu();
    }

    VLOG_I("  1000 yield_cpu calls done");
  }

  // Resource probes: instantaneous CPU% (per process) and memory% (per
  // process or system, platform-dependent). is_process_running scans the
  // process table for the named binary.
  {
    VLOG_I("--- Resource monitoring ---");
    VLOG_I("  cpu_usage=", vlink::Utils::get_cpu_usage(), "%");
    VLOG_I("  memory_usage=", vlink::Utils::get_memory_usage(), "%");

    auto name = vlink::Utils::get_app_name();
    VLOG_I("  is_process_running(self)=", vlink::Utils::is_process_running(name));
  }

  // Timezone offset in seconds (east-of-UTC). get_terminal_size returns
  // (cols, rows); useful for laying out CLI output.
  {
    VLOG_I("--- Timestamps ---");
    int32_t tz = vlink::Utils::get_timezone_diff();
    VLOG_I("  timezone offset=", tz, "s (~", tz / 3600, "h)");

    auto [cols, rows] = vlink::Utils::get_terminal_size();
    VLOG_I("  terminal=", cols, "x", rows);
  }

  // Signal handlers. register_terminate_signal handles SIGTERM/SIGINT;
  // register_crash_signal handles SIGSEGV/SIGABRT/etc. The callbacks run
  // in a SIGNAL context -- ONLY async-signal-safe operations are allowed
  // (the logger is signal-safe for short messages but avoid allocation).
  {
    VLOG_I("--- Signals ---");
    vlink::Utils::register_terminate_signal([](int sig) { VLOG_W("Received termination: ", sig); }, false, false);
    vlink::Utils::register_crash_signal([](int sig) { VLOG_E("Crash signal: ", sig); });
    VLOG_I("  registered terminate / crash signal handlers");
  }

  // Console UTF-8: matters on Windows where the console code page defaults
  // to OEM. try_release_sys_memory hints malloc to return freed pages back
  // to the OS -- best-effort, not guaranteed by any allocator.
  vlink::Utils::set_console_utf8_output();
  vlink::Utils::try_release_sys_memory();
  VLOG_I("Utils demo completed.");
  vlink::Logger::flush();
  return 0;
}
