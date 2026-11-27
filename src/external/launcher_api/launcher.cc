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
#include <vlink/base/plugin.h>
#include <vlink/base/utils.h>
#include <vlink/external/launcher_api.h>
#include <vlink/external/launcher_component.h>
#include <vlink/external/launcher_notify.h>
#include <vlink/external/launcher_server.h>
#include <vlink/version.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#if defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "./launcher_common.h"

static_assert(std::atomic_bool::is_always_lock_free, "CLI signal flag must be lock-free");

static std::atomic_bool g_stop_signal{false};
static std::atomic_bool g_reload_signal{false};

static void on_cli_signal(int sig) noexcept {
#if !defined(_WIN32) && !defined(_WIN64)
  if (sig == SIGHUP) {
    g_reload_signal.store(true, std::memory_order_release);
    return;
  }
#else
  (void)sig;
#endif
  g_stop_signal.store(true, std::memory_order_release);
}

#if defined(_WIN32) || defined(_WIN64)
static BOOL WINAPI on_cli_console_ctrl(DWORD ctrl_type) noexcept {
  switch (ctrl_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
      g_stop_signal.store(true, std::memory_order_release);
      return TRUE;
    default:
      return FALSE;
  }
}
#endif

static void install_cli_signal_handlers(bool handle_reload = false) noexcept {
#if defined(_WIN32) || defined(_WIN64)
  (void)handle_reload;
  (void)::SetConsoleCtrlHandler(on_cli_console_ctrl, TRUE);
  (void)std::signal(SIGINT, on_cli_signal);
  (void)std::signal(SIGTERM, on_cli_signal);
#else
  struct sigaction sa{};
  sa.sa_handler = on_cli_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  (void)::sigaction(SIGINT, &sa, nullptr);
  (void)::sigaction(SIGTERM, &sa, nullptr);

  if (handle_reload) {
    (void)::sigaction(SIGHUP, &sa, nullptr);
  }
#endif
}

static void wipe_argv_secret(char* arg_ptr) noexcept {
  if (arg_ptr == nullptr) {
    return;
  }

  const size_t len = std::strlen(arg_ptr);
  volatile char* p = arg_ptr;

  for (size_t i = 0U; i < len; ++i) {
    p[i] = '\0';
  }
}

static void print_usage() {
  std::cout << "vlink-launcher " << VLINK_VERSION << "\n\n"
            << "Usage:\n"
            << "  vlink-launcher run <manifest.toml> [--arg k=v]... [--only target]... [--skip target]...\n"
            << "                     [--dry-run] [--scheme S] [--ns N] [--domain D] [--key K]\n"
            << "                     [--no-ctrl] [--no-respawn]\n"
            << "  vlink-launcher host <manifest.toml> [--arg k=v]... [--only target]... [--skip target]...\n"
            << "                      [--dry-run] [--no-respawn]\n"
            << "  vlink-launcher validate <manifest.toml> [--arg k=v]... [--check-programs]\n"
            << "  vlink-launcher exec <lib> [class] [--name N] [--ns NS] [--param k=v]... [--remap a=b]...\n"
            << "                      [--scheduler S] [--threads N] [--queue Q] [--queue-strategy S]\n"
            << "  vlink-launcher plugin <lib>\n"
            << "  vlink-launcher status [ctrl opts]\n"
            << "  vlink-launcher events [ctrl opts]\n"
            << "  vlink-launcher start|stop|restart|reload <unit|@group> [ctrl opts]\n"
            << "  vlink-launcher params [unit] [ctrl opts]\n"
            << "  vlink-launcher set-param <unit> <key=value> [ctrl opts]\n"
            << "  vlink-launcher shutdown [ctrl opts]\n\n"
            << "Control options:\n"
            << "  --scheme <dds|ddsc|ddsr|zenoh|intra>  Control-plane transport scheme\n"
            << "  --ns <prefix>                         Control-plane namespace (default: launcher)\n"
            << "  --domain <id>                         Domain ID (default: 0)\n"
            << "  --key <secret>                        AES-128-GCM key (or VLINK_LAUNCHER_SECURITY_KEY)\n"
            << "  --timeout <ms>                        RPC timeout in ms (default: 3000)\n";
}

static bool parse_u32(std::string_view text, uint32_t& out) noexcept {
  if (text.empty()) {
    return false;
  }

  uint32_t v = 0U;
  const auto res = std::from_chars(text.data(), text.data() + text.size(), v, 10);

  if (res.ec != std::errc() || res.ptr != text.data() + text.size()) {
    return false;
  }

  out = v;
  return true;
}

static bool split_option(std::string_view arg, std::string_view opt_name, int& idx, int argc, char** argv,
                         std::string& val_out, bool& missing_val, bool wipe_secret = false) {
  missing_val = false;

  if (arg == opt_name) {
    if (idx + 1 >= argc || argv[idx + 1] == nullptr) {
      missing_val = true;
      return true;
    }

    val_out = argv[++idx];

    if (wipe_secret) {
      wipe_argv_secret(argv[idx]);
    }

    return true;
  }

  if (arg.size() > opt_name.size() && arg.substr(0U, opt_name.size()) == opt_name && arg[opt_name.size()] == '=') {
    val_out = std::string(arg.substr(opt_name.size() + 1U));

    if (wipe_secret) {
      wipe_argv_secret(argv[idx] + opt_name.size() + 1U);
    }

    return true;
  }

  return false;
}

static bool parse_kv_arg(std::string_view kv, std::string& key_out, std::string& val_out) {
  const size_t eq = kv.find('=');

  if (eq == std::string_view::npos || eq == 0U) {
    return false;
  }

  key_out = std::string(kv.substr(0U, eq));
  val_out = std::string(kv.substr(eq + 1U));
  return true;
}

static vlink::launcher::ParamEntry parse_param_kv(const std::string& kv, bool& ok) {
  vlink::launcher::ParamEntry entry;
  const size_t eq = kv.find('=');

  if (eq == std::string::npos || eq == 0U) {
    ok = false;
    return entry;
  }

  entry.key = kv.substr(0U, eq);
  const std::string raw = kv.substr(eq + 1U);
  ok = true;

  if (raw == "true" || raw == "false") {
    entry.type = vlink::launcher::ParamType::kBool;
    entry.value = raw;
    return entry;
  }

  int64_t iv = 0;
  const auto ires = std::from_chars(raw.data(), raw.data() + raw.size(), iv, 10);

  if (ires.ec == std::errc() && ires.ptr == raw.data() + raw.size()) {
    entry.type = vlink::launcher::ParamType::kInt;
    entry.value = raw;
    return entry;
  }

  if (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front())) == 0 &&
      std::isspace(static_cast<unsigned char>(raw.back())) == 0) {
    const size_t start = (raw[0] == '+' || raw[0] == '-') ? 1U : 0U;
    const bool is_hex =
        (start + 1U < raw.size() && raw[start] == '0' && (raw[start + 1U] == 'x' || raw[start + 1U] == 'X'));

    if (!is_hex) {
      char* end = nullptr;
      const double dv = std::strtod(raw.c_str(), &end);

      if (end == raw.c_str() + raw.size() && std::isfinite(dv)) {
        entry.type = vlink::launcher::ParamType::kDouble;
        entry.value = raw;
        return entry;
      }
    }
  }

  entry.type = vlink::launcher::ParamType::kString;
  entry.value = raw;
  return entry;
}

static void print_snapshot(const vlink::launcher::SystemSnapshot& snap) {
  std::cout << "Session: " << snap.session_id << "  Host: " << snap.host << "  PID: " << snap.server_pid
            << "  Uptime: " << snap.uptime_ms << "ms\n";
  std::cout << std::left << std::setw(20) << "UNIT" << std::setw(12) << "TYPE" << std::setw(12) << "STATE"
            << std::setw(10) << "PID" << std::setw(10) << "RESTARTS" << std::setw(10) << "HEALTH"
            << "DETAIL\n";

  for (const auto& u : snap.units) {
    std::cout << std::left << std::setw(20) << u.name << std::setw(12) << vlink::launcher::unit_type_name(u.type)
              << std::setw(12) << vlink::launcher::unit_state_name(u.state) << std::setw(10) << u.pid << std::setw(10)
              << u.restart_count << std::setw(10) << (u.healthy ? "ok" : "degraded") << u.detail << "\n";
  }
}

static int run_control_command(const std::string& cmd, int argc, char** argv) {
  vlink::LauncherAPI::Config cfg;
  cfg.scheme = std::string(vlink::launcher::default_scheme());
  cfg.security_key = vlink::Utils::get_env("VLINK_LAUNCHER_SECURITY_KEY");
  std::vector<std::string> positional;

  auto cleanup_key = [&]() { vlink::launcher::wipe_secret_string(cfg.security_key); };

  for (int i = 2; i < argc; ++i) {
    const std::string_view arg(argv[i]);

    if (arg == "-h" || arg == "--help") {
      cleanup_key();
      print_usage();
      return 0;
    }

    std::string val;
    bool missing = false;

    if (split_option(arg, "--scheme", i, argc, argv, val, missing)) {
      if (missing) {
        std::cerr << "Missing value for --scheme.\n";
        cleanup_key();
        return 2;
      }

      cfg.scheme = std::move(val);
    } else if (split_option(arg, "--ns", i, argc, argv, val, missing)) {
      if (missing) {
        std::cerr << "Missing value for --ns.\n";
        cleanup_key();
        return 2;
      }

      cfg.namespace_prefix = std::move(val);
    } else if (split_option(arg, "--domain", i, argc, argv, val, missing)) {
      uint32_t d = 0U;

      if (missing || !parse_u32(val, d)) {
        std::cerr << "Invalid --domain value.\n";
        cleanup_key();
        return 2;
      }

      cfg.domain_id = d;
    } else if (split_option(arg, "--key", i, argc, argv, val, missing, true)) {
      if (missing) {
        std::cerr << "Missing value for --key.\n";
        cleanup_key();
        return 2;
      }

      cfg.security_key = std::move(val);
    } else if (split_option(arg, "--timeout", i, argc, argv, val, missing)) {
      uint32_t t = 0U;

      if (missing || !parse_u32(val, t) || t == 0U) {
        std::cerr << "Invalid --timeout value.\n";
        cleanup_key();
        return 2;
      }

      cfg.default_timeout_ms = t;
    } else if (!arg.empty() && arg[0] == '-') {
      std::cerr << "Unknown option: " << arg << "\n";
      cleanup_key();
      return 2;
    } else {
      positional.emplace_back(arg);
    }
  }

  vlink::launcher::ParamEntry set_entry;

  if (cmd == "status" || cmd == "events" || cmd == "shutdown") {
    if (!positional.empty()) {
      std::cerr << "Unexpected positional argument: " << positional[0] << "\n";
      cleanup_key();
      return 2;
    }
  } else if (cmd == "start" || cmd == "stop" || cmd == "restart" || cmd == "reload") {
    if (positional.size() != 1U) {
      std::cerr << "Usage: vlink-launcher " << cmd << " <unit|@group> [ctrl opts]\n";
      cleanup_key();
      return 2;
    }
  } else if (cmd == "params") {
    if (positional.size() > 1U) {
      std::cerr << "Usage: vlink-launcher params [unit] [ctrl opts]\n";
      cleanup_key();
      return 2;
    }
  } else if (cmd == "set-param") {
    if (positional.size() != 2U) {
      std::cerr << "Usage: vlink-launcher set-param <unit> <key=value> [ctrl opts]\n";
      cleanup_key();
      return 2;
    }

    bool kv_ok = false;
    set_entry = parse_param_kv(positional[1], kv_ok);

    if (!kv_ok) {
      std::cerr << "Invalid key=value format.\n";
      cleanup_key();
      return 2;
    }
  }

  vlink::LauncherAPI api(cfg);
  cleanup_key();

  if (cmd == "events") {
    g_stop_signal.store(false, std::memory_order_release);
    install_cli_signal_handlers();

    api.register_event_callback([](const vlink::launcher::EventRecord& ev) {
      std::cout << "[" << ev.timestamp_ms << "] " << ev.unit_name << " ("
                << vlink::launcher::unit_type_name(ev.unit_type) << ") "
                << vlink::launcher::unit_state_name(ev.old_state) << " -> "
                << vlink::launcher::unit_state_name(ev.new_state) << " [" << ev.event_name << "] " << ev.message << "\n"
                << std::flush;
    });

    api.async_run();
    (void)api.wait_for_connected(std::min<uint32_t>(cfg.default_timeout_ms, 2000U));

    while (!g_stop_signal.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    api.quit(true);
    api.wait_for_quit();
    return 0;
  }

  api.async_run();
  (void)api.wait_for_connected(std::min<uint32_t>(cfg.default_timeout_ms, 2000U));

  vlink::launcher::CommandResult res;

  if (cmd == "status") {
    res = api.get_status();

    if (res.ok() && res.snapshot.has_value()) {
      print_snapshot(*res.snapshot);
    }
  } else if (cmd == "start") {
    res = api.start_unit(positional[0]);
  } else if (cmd == "stop") {
    res = api.stop_unit(positional[0]);
  } else if (cmd == "restart") {
    res = api.restart_unit(positional[0]);
  } else if (cmd == "reload") {
    res = api.reload_unit(positional[0]);
  } else if (cmd == "params") {
    const std::string unit = positional.empty() ? "" : positional[0];
    res = api.get_params(unit);

    if (res.ok()) {
      for (const auto& p : res.params) {
        std::cout << p.key << " = " << p.value << "\n";
      }
    }
  } else if (cmd == "set-param") {
    res = api.set_param(positional[0], set_entry);
  } else if (cmd == "shutdown") {
    res = api.shutdown_server();
  }

  api.quit(true);
  api.wait_for_quit();

  if (!res.ok()) {
    std::cerr << "Error [" << vlink::launcher::error_code_name(res.code) << "]: " << res.message << "\n";
    return 1;
  }

  if (cmd != "status" && cmd != "params" && !res.message.empty()) {
    std::cout << res.message << "\n";
  }

  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage();
    return 1;
  }

  const std::string cmd(argv[1]);

  if (cmd == "-h" || cmd == "--help" || cmd == "help") {
    print_usage();
    return 0;
  }

  if (cmd == "-v" || cmd == "--version") {
    std::cout << "vlink-launcher " << VLINK_VERSION << "\n";
    return 0;
  }

  if (cmd == "plugin") {
    if (argc < 3 || std::string_view(argv[2]) == "-h" || std::string_view(argv[2]) == "--help") {
      std::cout << "Usage: vlink-launcher plugin <lib>\n";
      return (argc >= 3) ? 0 : 2;
    }

    if (argc > 3) {
      std::cerr << "Unexpected argument: " << argv[3] << "\n";
      return 2;
    }

    vlink::Plugin loader;
    auto mod = loader.load<vlink::LauncherModuleInterface>(argv[2], vlink::LauncherModuleInterface::kVersionMajor,
                                                           vlink::LauncherModuleInterface::kVersionMinor);

    if (!mod) {
      std::cerr << "Failed to load launcher component plugin: " << argv[2] << "\n";
      return 1;
    }

    const auto classes = mod->list_components();
    std::cout << "Plugin: " << argv[2] << " (" << classes.size() << " component(s))\n";

    for (const auto& c : classes) {
      std::cout << "  - " << c << "\n";
    }

    return 0;
  }

  if (cmd == "exec") {
    if (argc < 3 || std::string_view(argv[2]) == "-h" || std::string_view(argv[2]) == "--help") {
      std::cout << "Usage: vlink-launcher exec <lib> [class] [--name N] [--ns NS] [--param k=v]... [--remap a=b]...\n"
                << "                           [--scheduler S] [--threads N] [--queue Q] [--queue-strategy S]\n";
      return (argc >= 3) ? 0 : 2;
    }

    const std::string lib(argv[2]);
    std::string cls;
    int forward_start = 3;

    if (argc >= 4 && argv[3][0] != '-') {
      cls = argv[3];
      forward_start = 4;
    }

    std::vector<char*> sub_argv;
    sub_argv.push_back(argv[0]);

    for (int i = forward_start; i < argc; ++i) {
      sub_argv.push_back(argv[i]);
    }

    const int sub_argc = static_cast<int>(sub_argv.size());
    sub_argv.push_back(nullptr);
    return vlink::run_launcher_component_from_library(sub_argc, sub_argv.data(), lib, cls);
  }

  if (cmd == "validate") {
    if (argc < 3 || std::string_view(argv[2]) == "-h" || std::string_view(argv[2]) == "--help") {
      std::cout << "Usage: vlink-launcher validate <manifest.toml> [--arg k=v]... [--check-programs]\n";
      return (argc >= 3) ? 0 : 2;
    }

    const std::string file(argv[2]);
    bool check_programs = false;
    vlink::LauncherServer::Config cfg;
    cfg.enable_control_plane = false;

    for (int i = 3; i < argc; ++i) {
      const std::string_view arg(argv[i]);
      std::string val;
      bool missing = false;

      if (arg == "-h" || arg == "--help") {
        print_usage();
        return 0;
      }

      if (arg == "--check-programs") {
        check_programs = true;
      } else if (split_option(arg, "--arg", i, argc, argv, val, missing)) {
        std::string k;
        std::string v;

        if (missing || !parse_kv_arg(val, k, v)) {
          std::cerr << "Invalid --arg format (expected k=v).\n";
          return 2;
        }

        cfg.args_override[k] = std::move(v);
      } else {
        std::cerr << "Unknown option: " << arg << "\n";
        return 2;
      }
    }

    vlink::LauncherServer srv(cfg);
    std::string err;

    if (!srv.load_file(file, &err)) {
      std::cerr << err << "\n";
      return 1;
    }

    const std::string verr = srv.validate(check_programs);

    if (!verr.empty()) {
      std::cerr << verr << "\n";
      return 1;
    }

    std::cout << "Manifest valid.\n";
    return 0;
  }

  if (cmd == "run" || cmd == "host") {
    const bool is_host = (cmd == "host");

    if (argc < 3 || std::string_view(argv[2]) == "-h" || std::string_view(argv[2]) == "--help") {
      std::cout << "Usage: vlink-launcher " << cmd << " <manifest.toml> [options]\n";
      return (argc >= 3) ? 0 : 2;
    }

    const std::string file(argv[2]);
    bool dry_run = false;
    vlink::LauncherServer::Config cfg;

    if (is_host) {
      cfg.enable_control_plane = false;
    }

    auto cleanup_key = [&]() { vlink::launcher::wipe_secret_string(cfg.security_key_override); };

    for (int i = 3; i < argc; ++i) {
      const std::string_view arg(argv[i]);
      std::string val;
      bool missing = false;

      if (arg == "-h" || arg == "--help") {
        cleanup_key();
        print_usage();
        return 0;
      }

      if (!is_host && split_option(arg, "--scheme", i, argc, argv, val, missing)) {
        if (missing) {
          std::cerr << "Missing value for --scheme.\n";
          cleanup_key();
          return 2;
        }

        cfg.control_scheme_override = std::move(val);
      } else if (!is_host && split_option(arg, "--ns", i, argc, argv, val, missing)) {
        if (missing) {
          std::cerr << "Missing value for --ns.\n";
          cleanup_key();
          return 2;
        }

        cfg.control_namespace_override = std::move(val);
      } else if (!is_host && split_option(arg, "--domain", i, argc, argv, val, missing)) {
        uint32_t d = 0U;

        if (missing || !parse_u32(val, d) || d > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
          std::cerr << "Invalid --domain value.\n";
          cleanup_key();
          return 2;
        }

        cfg.control_domain_override = static_cast<int32_t>(d);
      } else if (!is_host && split_option(arg, "--key", i, argc, argv, val, missing, true)) {
        if (missing) {
          std::cerr << "Missing value for --key.\n";
          cleanup_key();
          return 2;
        }

        cfg.security_key_override = std::move(val);
      } else if (split_option(arg, "--arg", i, argc, argv, val, missing)) {
        std::string k;
        std::string v;

        if (missing || !parse_kv_arg(val, k, v)) {
          std::cerr << "Invalid --arg format (expected k=v).\n";
          cleanup_key();
          return 2;
        }

        cfg.args_override[k] = std::move(v);
      } else if (split_option(arg, "--only", i, argc, argv, val, missing)) {
        if (missing || val.empty()) {
          std::cerr << "Missing value for --only.\n";
          cleanup_key();
          return 2;
        }

        cfg.only_targets.push_back(std::move(val));
      } else if (split_option(arg, "--skip", i, argc, argv, val, missing)) {
        if (missing || val.empty()) {
          std::cerr << "Missing value for --skip.\n";
          cleanup_key();
          return 2;
        }

        cfg.skip_targets.push_back(std::move(val));
      } else if (arg == "--dry-run") {
        dry_run = true;
      } else if (arg == "--no-ctrl") {
        cfg.enable_control_plane = false;
      } else if (arg == "--no-respawn") {
        cfg.disable_respawn = true;
      } else {
        std::cerr << "Unknown option: " << arg << "\n";
        cleanup_key();
        return 2;
      }
    }

    vlink::LauncherServer srv(cfg);
    cleanup_key();
    std::string err;

    if (!srv.load_file(file, &err)) {
      std::cerr << err << "\n";
      return 1;
    }

    const std::string verr = srv.validate(false);

    if (!verr.empty()) {
      std::cerr << verr << "\n";
      return 1;
    }

    const auto active_units = srv.startup_order();

    if (dry_run) {
      std::cout << "Dry-run startup order (" << active_units.size() << " unit(s)):\n";

      for (size_t i = 0U; i < active_units.size(); ++i) {
        std::cout << "  " << (i + 1U) << ". " << active_units[i] << "\n";
      }

      return 0;
    }

    g_stop_signal.store(false, std::memory_order_release);
    g_reload_signal.store(false, std::memory_order_release);
    install_cli_signal_handlers(is_host);

    const bool notify_managed = is_host && vlink::LauncherNotify::is_managed();
    std::mutex ready_mtx;
    std::unordered_set<std::string> settled_units;

    if (notify_managed) {
      srv.register_event_callback([&](const vlink::launcher::EventRecord& ev) {
        if (ev.new_state == vlink::launcher::UnitState::kReady ||
            ev.new_state == vlink::launcher::UnitState::kStopped ||
            ev.new_state == vlink::launcher::UnitState::kFailed) {
          std::lock_guard lock(ready_mtx);
          settled_units.insert(ev.unit_name);
        }
      });
    }

    if (!srv.start()) {
      std::cerr << "Failed to start LauncherServer.\n";
      return 1;
    }

    const uint32_t host_wd_ms = notify_managed ? vlink::LauncherNotify::watchdog_interval_ms() : 0U;
    const uint32_t ping_interval_ms = (host_wd_ms > 0U) ? std::max<uint32_t>(50U, host_wd_ms / 3U) : 0U;
    bool host_ready_sent = false;
    bool host_healthy_state = true;
    bool stop_requested = false;
    auto last_wd_ping = std::chrono::steady_clock::now();

    auto maybe_ping_watchdog = [&]() {
      if (ping_interval_ms == 0U || stop_requested) {
        return;
      }

      const auto now = std::chrono::steady_clock::now();

      if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_wd_ping).count() >=
          static_cast<int64_t>(ping_interval_ms)) {
        last_wd_ping = now;
        (void)vlink::LauncherNotify::notify_watchdog();
      }
    };

    while (!srv.wait(50)) {
      if (!stop_requested && g_stop_signal.exchange(false, std::memory_order_acq_rel)) {
        stop_requested = true;

        if (notify_managed) {
          (void)vlink::LauncherNotify::notify_stopping("stopping host units");
        }

        srv.request_stop(0);
        continue;
      }

      if (is_host && !stop_requested && g_reload_signal.exchange(false, std::memory_order_acq_rel)) {
        if (notify_managed) {
          (void)vlink::LauncherNotify::notify_reloading("reloading host units");
          maybe_ping_watchdog();
        }

        const auto snap = srv.get_snapshot();

        for (const auto& u : snap.units) {
          if (u.state == vlink::launcher::UnitState::kReady || u.state == vlink::launcher::UnitState::kRunning) {
            (void)srv.reload_unit(u.name);

            if (notify_managed) {
              maybe_ping_watchdog();
            }
          }
        }
      }

      if (notify_managed && !stop_requested) {
        const auto snap = srv.get_snapshot();
        size_t settled_cnt = 0U;
        bool all_healthy = true;
        std::string degraded_detail;

        {
          std::lock_guard lock(ready_mtx);

          for (const auto& u : snap.units) {
            if (u.state == vlink::launcher::UnitState::kReady || u.state == vlink::launcher::UnitState::kStopped ||
                u.state == vlink::launcher::UnitState::kFailed) {
              settled_units.insert(u.name);
            }
          }

          for (const auto& name : active_units) {
            if (settled_units.count(name) != 0U) {
              ++settled_cnt;
            }
          }
        }

        for (const auto& u : snap.units) {
          if (!u.healthy || u.state == vlink::launcher::UnitState::kFailed) {
            all_healthy = false;

            if (degraded_detail.empty()) {
              degraded_detail =
                  u.name + ":" + (u.detail.empty() ? std::string(vlink::launcher::unit_state_name(u.state)) : u.detail);
            }
          }
        }

        if (!host_ready_sent && settled_cnt == active_units.size()) {
          host_ready_sent = true;
          (void)vlink::LauncherNotify::notify_ready("host ready (" + std::to_string(active_units.size()) + " unit(s))");

          if (!all_healthy) {
            host_healthy_state = false;
            (void)vlink::LauncherNotify::notify_health(false, "degraded: " + degraded_detail);
          }
        } else if (host_ready_sent && all_healthy != host_healthy_state) {
          host_healthy_state = all_healthy;
          (void)vlink::LauncherNotify::notify_health(all_healthy,
                                                     all_healthy ? "host healthy" : ("degraded: " + degraded_detail));
        }

        maybe_ping_watchdog();
      }
    }

    srv.stop();
    return srv.get_exit_code();
  }

  if (cmd == "status" || cmd == "events" || cmd == "start" || cmd == "stop" || cmd == "restart" || cmd == "reload" ||
      cmd == "params" || cmd == "set-param" || cmd == "shutdown") {
    return run_control_command(cmd, argc, argv);
  }

  std::cerr << "Unknown command: " << cmd << "\n";
  print_usage();
  return 2;
}
