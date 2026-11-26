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

#include <vlink/base/condition_variable.h>
#include <vlink/base/elapsed_timer.h>
#include <vlink/base/helpers.h>
#include <vlink/base/logger.h>
#include <vlink/base/terminal_stream.h>
#include <vlink/base/utils.h>
#include <vlink/external/launcher_api.h>
#include <vlink/external/launcher_server.h>
#include <vlink/version.h>
//
#include <argparse/argparse.hpp>
#include <nlohmann/json.hpp>
//
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <locale>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "./launcher_common.h"

static std::atomic_bool g_terminate_requested{false};

struct SignalGuard final {
  SignalGuard() noexcept {
    g_terminate_requested.store(false, std::memory_order_release);
    vlink::Utils::register_terminate_signal([](int) { g_terminate_requested.store(true, std::memory_order_release); },
                                            false, false);
  }

  ~SignalGuard() noexcept { vlink::Utils::register_terminate_signal(nullptr, false, false); }

  SignalGuard(const SignalGuard&) = delete;
  SignalGuard& operator=(const SignalGuard&) = delete;
};

static bool parse_kv_list(const std::vector<std::string>& items, std::unordered_map<std::string, std::string>& out,
                          std::string* err) {
  out.clear();

  for (const auto& item : items) {
    const size_t eq_pos = item.find('=');

    if (eq_pos == std::string::npos || eq_pos == 0U) {
      if (err != nullptr) {
        *err = "invalid key=value pair '" + item + "'";
      }

      return false;
    }

    const bool has_colon_eq = (item[eq_pos - 1U] == ':');
    const size_t key_end = has_colon_eq ? (eq_pos - 1U) : eq_pos;

    if (key_end == 0U) {
      if (err != nullptr) {
        *err = "invalid key=value pair '" + item + "'";
      }

      return false;
    }

    out[item.substr(0U, key_end)] = item.substr(eq_pos + 1U);
  }

  return true;
}

static bool parse_strict_int64(const std::string& raw) noexcept {
  if (raw.empty() || std::isspace(static_cast<unsigned char>(raw.front())) != 0 ||
      std::isspace(static_cast<unsigned char>(raw.back())) != 0) {
    return false;
  }

  errno = 0;
  char* end = nullptr;
  (void)std::strtoll(raw.c_str(), &end, 10);
  return errno != ERANGE && end == raw.c_str() + raw.size();
}

static bool parse_strict_finite_double(const std::string& raw) noexcept {
  if (raw.empty() || std::isspace(static_cast<unsigned char>(raw.front())) != 0 ||
      std::isspace(static_cast<unsigned char>(raw.back())) != 0) {
    return false;
  }

  std::istringstream iss(raw);
  iss.imbue(std::locale::classic());
  double d = 0.0;
  char trailing = '\0';
  return static_cast<bool>(iss >> d) && !(iss >> trailing) && std::isfinite(d);
}

static bool parse_param_spec(const std::string& key, const std::string& raw_type, const std::string& value,
                             vlink::launcher::ParamEntry& entry, std::string* error_out = nullptr) {
  if (key.empty()) {
    if (error_out != nullptr) {
      *error_out = "Parameter key must not be empty.";
    }

    return false;
  }

  entry = vlink::launcher::ParamEntry{};
  entry.key = key;
  entry.value = value;

  if (raw_type == "bool") {
    if (value != "true" && value != "false") {
      if (error_out != nullptr) {
        *error_out = "Invalid bool parameter value '" + value + "' (expected true|false).";
      }

      return false;
    }

    entry.type = vlink::launcher::ParamType::kBool;
  } else if (raw_type == "int") {
    if (!parse_strict_int64(value)) {
      if (error_out != nullptr) {
        *error_out = "Invalid int parameter value '" + value + "'.";
      }

      return false;
    }

    entry.type = vlink::launcher::ParamType::kInt;
  } else if (raw_type == "double" || raw_type == "float") {
    if (!parse_strict_finite_double(value)) {
      if (error_out != nullptr) {
        *error_out = "Invalid finite double parameter value '" + value + "'.";
      }

      return false;
    }

    entry.type = vlink::launcher::ParamType::kDouble;
  } else if (raw_type == "bool_list" || raw_type == "int_list" || raw_type == "double_list" ||
             raw_type == "string_list") {
    const auto parsed = nlohmann::json::parse(value, nullptr, false);

    if (parsed.is_discarded() || !parsed.is_array()) {
      if (error_out != nullptr) {
        *error_out = "Invalid JSON array for --param_type=" + raw_type + ": '" + value + "'.";
      }

      return false;
    }

    auto check_all = [&](auto&& pred, const char* msg, vlink::launcher::ParamType pt) -> bool {
      for (const auto& elem : parsed) {
        if (!pred(elem)) {
          if (error_out != nullptr) {
            *error_out = msg;
          }

          return false;
        }
      }

      entry.type = pt;
      return true;
    };

    if (raw_type == "bool_list") {
      return check_all([](const nlohmann::json& e) { return e.is_boolean(); },
                       "All elements in bool_list must be booleans.", vlink::launcher::ParamType::kBoolList);
    }

    if (raw_type == "int_list") {
      return check_all(
          [](const nlohmann::json& e) {
            return e.is_number_integer() &&
                   !(e.is_number_unsigned() &&
                     e.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
          },
          "All elements in int_list must be 64-bit integers.", vlink::launcher::ParamType::kIntList);
    }

    if (raw_type == "double_list") {
      return check_all([](const nlohmann::json& e) { return e.is_number() && std::isfinite(e.get<double>()); },
                       "All elements in double_list must be finite numbers.", vlink::launcher::ParamType::kDoubleList);
    }

    return check_all([](const nlohmann::json& e) { return e.is_string(); },
                     "All elements in string_list must be strings.", vlink::launcher::ParamType::kStringList);
  } else if (raw_type == "string") {
    entry.type = vlink::launcher::ParamType::kString;
  } else if (raw_type.empty() || raw_type == "auto") {
    if (value == "true" || value == "false") {
      entry.type = vlink::launcher::ParamType::kBool;
    } else if (!value.empty() && value.front() == '[') {
      auto parsed = nlohmann::json::parse(value, nullptr, false);

      if (!parsed.is_discarded() && parsed.is_array()) {
        if (parsed.empty()) {
          entry.type = vlink::launcher::ParamType::kStringList;
        } else {
          bool all_bool = true;
          bool all_int = true;
          bool all_num = true;
          bool all_str = true;

          for (const auto& elem : parsed) {
            if (!elem.is_boolean()) {
              all_bool = false;
            }

            if (!elem.is_number_integer() ||
                (elem.is_number_unsigned() &&
                 elem.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))) {
              all_int = false;
            }

            if (!elem.is_number() || !std::isfinite(elem.get<double>())) {
              all_num = false;
            }

            if (!elem.is_string()) {
              all_str = false;
            }
          }

          if (all_bool) {
            entry.type = vlink::launcher::ParamType::kBoolList;
          } else if (all_int) {
            entry.type = vlink::launcher::ParamType::kIntList;
          } else if (all_num) {
            entry.type = vlink::launcher::ParamType::kDoubleList;
          } else if (all_str) {
            entry.type = vlink::launcher::ParamType::kStringList;
          } else {
            entry.type = vlink::launcher::ParamType::kString;
          }
        }
      } else {
        entry.type = vlink::launcher::ParamType::kString;
      }
    } else if (parse_strict_int64(value)) {
      entry.type = vlink::launcher::ParamType::kInt;
    } else if (parse_strict_finite_double(value)) {
      entry.type = vlink::launcher::ParamType::kDouble;
    } else {
      entry.type = vlink::launcher::ParamType::kString;
    }
  } else {
    if (error_out != nullptr) {
      *error_out = "Unknown --param_type '" + raw_type +
                   "' (expected auto|bool|int|double|string|bool_list|int_list|double_list|string_list).";
    }

    return false;
  }

  return true;
}

static bool parse_cli_level(const std::string& raw, uint8_t& out_level, std::string* error_out = nullptr) {
  if (raw.empty()) {
    out_level = 0U;
    return true;
  }

  std::string lower = raw;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  if (lower == "trace") {
    out_level = static_cast<uint8_t>(vlink::Logger::kTrace);
    return true;
  }

  if (lower == "debug") {
    out_level = static_cast<uint8_t>(vlink::Logger::kDebug);
    return true;
  }

  if (lower == "info") {
    out_level = static_cast<uint8_t>(vlink::Logger::kInfo);
    return true;
  }

  if (lower == "warn" || lower == "warning") {
    out_level = static_cast<uint8_t>(vlink::Logger::kWarn);
    return true;
  }

  if (lower == "error") {
    out_level = static_cast<uint8_t>(vlink::Logger::kError);
    return true;
  }

  if (lower == "fatal") {
    out_level = static_cast<uint8_t>(vlink::Logger::kFatal);
    return true;
  }

  if (error_out != nullptr) {
    *error_out = "Unknown --level '" + raw + "' (expected trace|debug|info|warn|error|fatal).";
  }

  return false;
}

static const char* level_short_name(uint8_t lv) noexcept {
  switch (lv) {
    case 0:
      return "TRC";
    case 1:
      return "DBG";
    case 2:
      return "INF";
    case 3:
      return "WRN";
    case 4:
      return "ERR";
    case 5:
      return "FTL";
    default:
      return "LOG";
  }
}

static const char* state_color(vlink::launcher::UnitState st, bool use_color) noexcept {
  if (!use_color) {
    return "";
  }

  switch (st) {
    case vlink::launcher::UnitState::kReady:
      return "\033[32m";
    case vlink::launcher::UnitState::kRunning:
    case vlink::launcher::UnitState::kStarting:
      return "\033[36m";
    case vlink::launcher::UnitState::kInactive:
      return "\033[34m";
    case vlink::launcher::UnitState::kRestarting:
    case vlink::launcher::UnitState::kStopping:
      return "\033[33m";
    case vlink::launcher::UnitState::kFailed:
      return "\033[31m";
    default:
      return "\033[90m";
  }
}

static std::string format_kb(uint64_t kb) {
  if (kb == 0U) {
    return "-";
  }

  return vlink::Helpers::format_file_size(kb * 1024ULL);
}

static std::string format_cpu(double cpu) {
  if (cpu < 0.0) {
    return "-";
  }

  return vlink::Helpers::double_to_string(cpu, 1) + "%";
}

static vlink::launcher::SystemSnapshot copy_snapshot_metadata(const vlink::launcher::SystemSnapshot& snap) {
  vlink::launcher::SystemSnapshot out;
  out.session_id = snap.session_id;
  out.version = snap.version;
  out.host = snap.host;
  out.server_pid = snap.server_pid;
  out.timestamp_ms = snap.timestamp_ms;
  out.uptime_ms = snap.uptime_ms;
  out.boot_duration_ms = snap.boot_duration_ms;
  out.cpu_usage = snap.cpu_usage;
  out.memory_usage = snap.memory_usage;
  out.startup_mode = snap.startup_mode;
  out.max_parallel = snap.max_parallel;
  out.log_dir = snap.log_dir;
  out.config_file = snap.config_file;
  return out;
}

static vlink::launcher::SystemSnapshot filter_snapshot(const vlink::launcher::SystemSnapshot& snap,
                                                       const std::string& filter_type, const std::string& filter_state,
                                                       const std::string& filter_group, bool failed_only) {
  vlink::launcher::SystemSnapshot filtered = copy_snapshot_metadata(snap);
  const std::string g = (!filter_group.empty() && filter_group[0] == '@') ? filter_group.substr(1U) : filter_group;

  for (const auto& u : snap.units) {
    if (!filter_type.empty() && filter_type != vlink::launcher::unit_type_to_string(u.type)) {
      continue;
    }

    if (!filter_state.empty() && filter_state != vlink::launcher::unit_state_to_string(u.state)) {
      continue;
    }

    if (failed_only && u.state != vlink::launcher::UnitState::kFailed) {
      continue;
    }

    if (!g.empty() && u.task_group != g && u.group != g) {
      continue;
    }

    filtered.units.push_back(u);
  }

  return filtered;
}

static nlohmann::ordered_json snapshot_to_json(const vlink::launcher::SystemSnapshot& snap) {
  nlohmann::ordered_json j;
  j["session_id"] = snap.session_id;
  j["version"] = snap.version;
  j["host"] = snap.host;
  j["server_pid"] = snap.server_pid;
  j["uptime_ms"] = snap.uptime_ms;
  j["boot_duration_ms"] = snap.boot_duration_ms;
  j["cpu_usage"] = snap.cpu_usage;
  j["memory_usage"] = snap.memory_usage;
  j["startup_mode"] = snap.startup_mode;
  j["max_parallel"] = snap.max_parallel;
  j["log_dir"] = snap.log_dir;
  j["config_file"] = snap.config_file;

  nlohmann::ordered_json arr = nlohmann::ordered_json::array();

  for (const auto& u : snap.units) {
    nlohmann::ordered_json uj;
    uj["name"] = u.name;
    uj["type"] = vlink::launcher::unit_type_to_string(u.type);
    uj["state"] = vlink::launcher::unit_state_to_string(u.state);
    uj["group"] = u.task_group;
    uj["scheduler"] = u.group;
    uj["slice"] = u.slice;
    uj["container"] = u.container;
    uj["cgroup_path"] = u.cgroup_path;
    uj["stage"] = u.stage;
    uj["pid"] = u.pid;
    uj["exit_code"] = u.exit_code;
    uj["restart_count"] = u.restart_count;
    uj["thread_count"] = u.thread_count;
    uj["memory_rss_kb"] = u.memory_rss_kb;
    uj["cgroup_memory_kb"] = u.cgroup_memory_kb;
    uj["cpu_usage"] = u.cpu_usage;
    uj["startup_duration_ms"] = u.startup_duration_ms;
    uj["ready_offset_ms"] = u.ready_offset_ms;
    uj["blocker_unit"] = u.blocker_unit;
    uj["healthy"] = u.healthy;
    uj["required"] = u.required;
    uj["failed_reason"] = u.failed_reason;
    uj["detail"] = u.detail;
    uj["dependencies"] = u.dependencies;
    uj["wants"] = u.wants;
    uj["binds_to"] = u.binds_to;
    uj["conflicts"] = u.conflicts;
    arr.push_back(std::move(uj));
  }

  j["units"] = std::move(arr);
  return j;
}

static void print_snapshot_table(const vlink::launcher::SystemSnapshot& snap, bool use_color,
                                 const std::string& filter_type = "", const std::string& filter_state = "",
                                 const std::string& filter_group = "", bool failed_only = false) {
  const char* reset = use_color ? "\033[0m" : "";
  const char* bold = use_color ? "\033[1m" : "";
  size_t ready_cnt = 0U;
  size_t running_cnt = 0U;
  size_t failed_cnt = 0U;

  for (const auto& u : snap.units) {
    if (u.state == vlink::launcher::UnitState::kReady) {
      ++ready_cnt;
    } else if (u.state == vlink::launcher::UnitState::kRunning || u.state == vlink::launcher::UnitState::kStarting) {
      ++running_cnt;
    } else if (u.state == vlink::launcher::UnitState::kFailed) {
      ++failed_cnt;
    }
  }

  std::cout << bold << "VLink Launcher" << reset << "  session=" << snap.session_id << "  host=" << snap.host
            << "  pid=" << snap.server_pid
            << "  uptime=" << vlink::Helpers::format_milliseconds(static_cast<int64_t>(snap.uptime_ms), false)
            << "  mode=" << snap.startup_mode;

  if (snap.max_parallel > 0U) {
    std::cout << "(" << snap.max_parallel << ")";
  }

  if (snap.boot_duration_ms > 0U) {
    std::cout << "  boot=" << snap.boot_duration_ms << "ms";
  }

  std::cout << "\n";
  std::cout << "System: cpu=" << format_cpu(snap.cpu_usage) << "  mem=" << format_cpu(snap.memory_usage)
            << "  units=" << snap.units.size() << " (ready=" << ready_cnt << ", active=" << running_cnt
            << ", failed=" << failed_cnt << ")";

  if (!snap.log_dir.empty()) {
    std::cout << "  logs=" << snap.log_dir;
  }

  std::cout << "\n\n";

  const std::vector<std::string> headers = {"UNIT", "TYPE", "STATE", "PID",       "CPU",   "RSS",
                                            "THR",  "RST",  "BOOT",  "GRP/SLICE", "DETAIL"};
  std::vector<std::vector<std::string>> rows;
  std::vector<vlink::launcher::UnitState> row_states;

  const auto filtered = filter_snapshot(snap, filter_type, filter_state, filter_group, failed_only);

  for (const auto& u : filtered.units) {
    const std::string tstr = vlink::launcher::unit_type_to_string(u.type);
    const std::string sstr = vlink::launcher::unit_state_to_string(u.state);

    std::string grp_col;

    if (!u.task_group.empty()) {
      grp_col = "@" + u.task_group;
    } else if (!u.container.empty()) {
      grp_col = "c:" + u.container;
    } else if (!u.group.empty()) {
      grp_col = u.group;
    }

    if (!u.slice.empty()) {
      if (!grp_col.empty()) {
        grp_col.push_back('/');
      }

      grp_col.append(u.slice);
    }

    if (grp_col.empty()) {
      grp_col = "-";
    }

    rows.push_back({
        u.name,
        tstr,
        sstr,
        u.pid >= 0 ? std::to_string(u.pid) : "-",
        format_cpu(u.cpu_usage),
        format_kb(u.cgroup_memory_kb > 0U ? u.cgroup_memory_kb : u.memory_rss_kb),
        u.thread_count > 0U ? std::to_string(u.thread_count) : "-",
        std::to_string(u.restart_count),
        u.startup_duration_ms > 0U ? (std::to_string(u.startup_duration_ms) + "ms") : "-",
        grp_col,
        u.failed_reason.empty() ? u.detail : (u.failed_reason + ": " + u.detail),
    });
    row_states.push_back(u.state);
  }

  if (rows.empty()) {
    std::cout << "(no matching units)\n";
    return;
  }

  std::vector<size_t> widths(headers.size(), 0U);

  for (size_t i = 0; i < headers.size(); ++i) {
    widths[i] = headers[i].size();
  }

  for (const auto& r : rows) {
    for (size_t i = 0; i < r.size(); ++i) {
      widths[i] = std::max(widths[i], r[i].size());
    }
  }

  std::cout << "  ";

  for (size_t i = 0; i < headers.size(); ++i) {
    std::cout << bold << headers[i] << reset;

    if (i + 1U < headers.size()) {
      std::cout << std::string((widths[i] - headers[i].size()) + 2U, ' ');
    }
  }

  std::cout << "\n";

  for (size_t r = 0; r < rows.size(); ++r) {
    const char* col = state_color(row_states[r], use_color);
    std::cout << col << "● " << reset;

    for (size_t i = 0; i < rows[r].size(); ++i) {
      if (i == 2U) {
        std::cout << col << rows[r][i] << reset;
      } else {
        std::cout << rows[r][i];
      }

      if (i + 1U < rows[r].size()) {
        std::cout << std::string((widths[i] - rows[r][i].size()) + 2U, ' ');
      }
    }

    std::cout << "\n";
  }
}

static void print_unit_detail_card(const vlink::launcher::UnitStatus& u,
                                   const std::vector<vlink::launcher::LogRecord>& recent_logs, bool use_color) {
  const char* col = state_color(u.state, use_color);
  const char* reset = use_color ? "\033[0m" : "";
  const char* bold = use_color ? "\033[1m" : "";

  std::cout << col << "● " << reset << bold << u.name << reset << " (" << vlink::launcher::unit_type_to_string(u.type)
            << ")\n";
  std::cout << "   State:   " << col << vlink::launcher::unit_state_to_string(u.state) << reset
            << (u.healthy ? " (healthy)" : " (degraded)");

  if (!u.failed_reason.empty()) {
    std::cout << "  reason=" << u.failed_reason;
  }

  std::cout << "\n";

  if (u.pid >= 0) {
    std::cout << "   PID:     " << u.pid << "  Restarts: " << u.restart_count << "  ExitCode: " << u.exit_code
              << "  Gen: " << u.generation << "\n";
  }

  std::cout << "   Timing:  startup=" << u.startup_duration_ms << "ms  queued=@" << u.queued_offset_ms
            << "ms  started=@" << u.started_offset_ms << "ms  ready=@" << u.ready_offset_ms << "ms";

  if (!u.blocker_unit.empty()) {
    std::cout << "  unblocked_by=" << u.blocker_unit;
  }

  std::cout << "\n";
  std::cout << "   Metrics: cpu=" << format_cpu(u.cpu_usage) << "  rss=" << format_kb(u.memory_rss_kb);

  if (u.cgroup_memory_kb > 0U) {
    std::cout << "  cgroup_mem=" << format_kb(u.cgroup_memory_kb);
  }

  if (u.thread_count > 0U) {
    std::cout << "  threads=" << u.thread_count;
  }

  std::cout << "\n";

  if (!u.task_group.empty() || !u.group.empty() || !u.slice.empty() || !u.container.empty()) {
    std::cout << "   Scope:   ";

    if (!u.task_group.empty()) {
      std::cout << "group=@" << u.task_group << "  ";
    }

    if (!u.group.empty()) {
      std::cout << "scheduler=" << u.group << "  ";
    }

    if (u.worker_index >= 0) {
      std::cout << "worker=" << u.worker_index << "  ";
    }

    if (!u.slice.empty()) {
      std::cout << "slice=" << u.slice << "  ";
    }

    if (!u.container.empty()) {
      std::cout << "container=" << u.container;
    }

    std::cout << "\n";
  }

  if (!u.cgroup_path.empty()) {
    std::cout << "   CGroup:  " << u.cgroup_path << "\n";
  }

  auto print_list = [](const char* label, const std::vector<std::string>& items) {
    if (items.empty()) {
      return;
    }

    std::cout << "   " << label;

    for (size_t i = 0; i < items.size(); ++i) {
      if (i > 0U) {
        std::cout << ", ";
      }

      std::cout << items[i];
    }

    std::cout << "\n";
  };

  print_list("Deps:     ", u.dependencies);
  print_list("Wants:    ", u.wants);
  print_list("BindsTo:  ", u.binds_to);
  print_list("Conflict: ", u.conflicts);

  if (!u.detail.empty()) {
    std::cout << "   Detail:  " << u.detail << "\n";
  }

  if (!recent_logs.empty()) {
    std::cout << "\n" << bold << "Recent logs:" << reset << "\n";

    for (const auto& rec : recent_logs) {
      std::cout << "  [" << level_short_name(rec.level) << "] " << rec.text << "\n";
    }
  }
}

static void print_blame(const vlink::launcher::SystemSnapshot& snap, bool use_color) {
  if (snap.units.empty()) {
    return;
  }

  std::vector<const vlink::launcher::UnitStatus*> sorted;
  sorted.reserve(snap.units.size());

  for (const auto& u : snap.units) {
    sorted.push_back(&u);
  }

  std::sort(sorted.begin(), sorted.end(),
            [](const vlink::launcher::UnitStatus* a, const vlink::launcher::UnitStatus* b) {
              return a->startup_duration_ms > b->startup_duration_ms;
            });

  const uint64_t max_ms = std::max<uint64_t>(1U, sorted.front()->startup_duration_ms);

  for (const auto* u : sorted) {
    const auto bar_len = static_cast<size_t>((u->startup_duration_ms * 24U) / max_ms);
    const char* col = state_color(u->state, use_color);
    const char* reset = use_color ? "\033[0m" : "";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%7" PRIu64 " ms", u->startup_duration_ms);
    std::cout << col << buf << reset << "  " << std::string(bar_len, '#') << std::string(24U - bar_len, ' ') << "  "
              << u->name << " (" << vlink::launcher::unit_type_to_string(u->type) << ")\n";
  }
}

static bool print_critical_chain(const vlink::launcher::SystemSnapshot& snap, const std::string& target,
                                 bool use_color) {
  std::unordered_map<std::string, const vlink::launcher::UnitStatus*> by_name;

  for (const auto& u : snap.units) {
    by_name[u.name] = &u;
  }

  const vlink::launcher::UnitStatus* cur = nullptr;

  if (!target.empty()) {
    auto it = by_name.find(target);

    if (it == by_name.end()) {
      std::cerr << "Unit '" << target << "' not found.\n";
      return false;
    }

    cur = it->second;
  } else {
    for (const auto& u : snap.units) {
      if (u.state != vlink::launcher::UnitState::kReady) {
        continue;
      }

      if (cur == nullptr || u.ready_offset_ms > cur->ready_offset_ms) {
        cur = &u;
      }
    }

    if (cur == nullptr) {
      for (const auto& u : snap.units) {
        if (cur == nullptr || u.ready_offset_ms > cur->ready_offset_ms) {
          cur = &u;
        }
      }
    }
  }

  if (cur == nullptr) {
    std::cout << "(no critical chain data)\n";
    return true;
  }

  std::vector<const vlink::launcher::UnitStatus*> chain;
  std::unordered_set<std::string> seen;

  while (cur != nullptr && seen.insert(cur->name).second) {
    chain.push_back(cur);

    if (cur->blocker_unit.empty()) {
      break;
    }

    auto it = by_name.find(cur->blocker_unit);
    cur = (it == by_name.end()) ? nullptr : it->second;
  }

  const char* red = use_color ? "\033[31m" : "";
  const char* reset = use_color ? "\033[0m" : "";
  std::cout << "Critical startup chain (total boot: " << snap.boot_duration_ms << "ms):\n";

  for (size_t i = 0; i < chain.size(); ++i) {
    const auto* u = chain[i];
    std::cout << std::string(i * 2U, ' ') << (i == 0U ? "" : "└─") << u->name << " @" << u->started_offset_ms << "ms "
              << red << "+" << u->startup_duration_ms << "ms" << reset << " (ready @" << u->ready_offset_ms << "ms)\n";
  }

  return true;
}

static void walk_dependencies_tree(const vlink::launcher::SystemSnapshot& snap,
                                   const std::unordered_map<std::string, const vlink::launcher::UnitStatus*>& by_name,
                                   std::unordered_set<std::string>& visited, const std::string& name,
                                   const std::string& prefix, bool is_root, bool is_last, bool reverse,
                                   bool use_color) {
  auto it = by_name.find(name);

  if (it == by_name.end()) {
    return;
  }

  const auto* u = it->second;
  const char* col = state_color(u->state, use_color);
  const char* reset = use_color ? "\033[0m" : "";
  const bool already_seen = !visited.insert(name).second;

  std::cout << prefix;

  if (!is_root) {
    std::cout << (is_last ? "└─" : "├─");
  }

  std::cout << col << "● " << reset << u->name << " [" << vlink::launcher::unit_state_to_string(u->state) << "]";

  if (already_seen) {
    std::cout << " (seen)";
  }

  std::cout << "\n";

  if (already_seen) {
    return;
  }

  std::vector<std::string> children;
  std::unordered_set<std::string> child_seen;

  auto add_child = [&](const std::string& cname) {
    if (by_name.count(cname) != 0U && child_seen.insert(cname).second) {
      children.push_back(cname);
    }
  };

  if (!reverse) {
    for (const auto& d : u->dependencies) {
      add_child(d);
    }

    for (const auto& w : u->wants) {
      add_child(w);
    }

    for (const auto& b : u->binds_to) {
      add_child(b);
    }
  } else {
    for (const auto& other : snap.units) {
      const bool in_deps =
          std::find(other.dependencies.begin(), other.dependencies.end(), name) != other.dependencies.end();
      const bool in_wants = std::find(other.wants.begin(), other.wants.end(), name) != other.wants.end();
      const bool in_binds = std::find(other.binds_to.begin(), other.binds_to.end(), name) != other.binds_to.end();

      if (in_deps || in_wants || in_binds) {
        add_child(other.name);
      }
    }
  }

  const std::string next_prefix = is_root ? std::string() : (prefix + (is_last ? "  " : "│ "));

  for (size_t i = 0U; i < children.size(); ++i) {
    walk_dependencies_tree(snap, by_name, visited, children[i], next_prefix, false, (i + 1U == children.size()),
                           reverse, use_color);
  }
}

static bool print_dependencies_tree(const vlink::launcher::SystemSnapshot& snap, const std::string& root_name,
                                    bool reverse, bool use_color) {
  std::unordered_map<std::string, const vlink::launcher::UnitStatus*> by_name;

  for (const auto& u : snap.units) {
    by_name[u.name] = &u;
  }

  std::unordered_set<std::string> visited;

  if (!root_name.empty()) {
    if (by_name.count(root_name) == 0U) {
      std::cerr << "Unit '" << root_name << "' not found.\n";
      return false;
    }

    walk_dependencies_tree(snap, by_name, visited, root_name, "", true, true, reverse, use_color);
  } else {
    for (const auto& u : snap.units) {
      visited.clear();
      walk_dependencies_tree(snap, by_name, visited, u.name, "", true, true, reverse, use_color);
    }
  }

  return true;
}

static int run_server_command(const argparse::ArgumentParser& cmd, bool container_mode) {
  vlink::LauncherServer::Config cfg;
  struct SecretKeyGuard final {
    std::string& key;

    ~SecretKeyGuard() noexcept { vlink::launcher::wipe_secret_string(key); }
  } key_guard{cfg.security_key};
  (void)key_guard;
  cfg.container_mode = container_mode;
  cfg.control_scheme = cmd.get<std::string>("--scheme");
  cfg.control_namespace = cmd.get<std::string>("--namespace");
  cfg.control_domain_id = cmd.get<int32_t>("--domain");
  cfg.security_key = cmd.get<std::string>("--key");
  cfg.bind_ip = cmd.get<std::string>("--bind_ip");
  cfg.native_mode = cmd.get<bool>("--native");
  cfg.enable_control_plane = !cmd.get<bool>("--no_control");
  cfg.color_output = !cmd.get<bool>("--plain");
  cfg.log_dir_override = cmd.get<std::string>("--log_dir");
  cfg.launcher_executable = vlink::Utils::get_app_path();

  std::string config_path;

  if (container_mode) {
    config_path = cmd.get<std::string>("--config");
    cfg.container_name = cmd.get<std::string>("--name");
    cfg.manage_console = false;
  } else {
    vlink::Utils::unset_env("VLINK_BAG_PATH");
    std::string parse_err;
    config_path = cmd.get<std::string>("config");
    cfg.record_dir_override = cmd.get<std::string>("--record_dir");
    cfg.log_plugin_override = cmd.get<std::string>("--log_plugin");

    if (!parse_kv_list(cmd.get<std::vector<std::string>>("--arg"), cfg.arg_overrides, &parse_err)) {
      std::cerr << "Error (--arg): " << parse_err << "\n";
      return 1;
    }

    cfg.log_level_override = cmd.get<std::string>("--log_level");
    cfg.disable_respawn = cmd.get<bool>("--no_respawn");
    cfg.pause_on_fail = cmd.get<bool>("--pause_on_fail");
    cfg.only_units = cmd.get<std::vector<std::string>>("--only");
    cfg.skip_units = cmd.get<std::vector<std::string>>("--skip");

    if (!parse_kv_list(cmd.get<std::vector<std::string>>("--debug_unit"), cfg.debug_prefixes, &parse_err)) {
      std::cerr << "Error (--debug_unit): " << parse_err << "\n";
      return 1;
    }

    const bool serial = cmd.get<bool>("--serial");
    const auto parallel = cmd.get<int32_t>("--parallel");

    if (parallel < 0) {
      std::cerr << "Error: --parallel must be >= 0.\n";
      return 1;
    }

    if (serial && parallel > 0) {
      std::cerr << "Error: --serial and --parallel cannot be used together.\n";
      return 1;
    }

    if (serial) {
      cfg.startup_mode_override = "serial";
      cfg.max_parallel_override = 1;
    } else if (parallel > 0) {
      cfg.max_parallel_override = parallel;
    }
  }

  vlink::LauncherServer server(cfg);
  vlink::launcher::wipe_secret_string(cfg.security_key);
  std::string err;

  if (!server.load_file(config_path, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  if (!container_mode && cmd.get<bool>("--show_args")) {
    const auto doc = nlohmann::json::parse(server.inspect_manifest_json());
    std::cout << "Declared Launch Arguments:\n";

    for (const auto& a : doc["args"]) {
      std::cout << "  " << a["name"].get<std::string>() << " = \"" << a["resolved"].get<std::string>()
                << "\" (default: \"" << a["default"].get<std::string>() << "\")";

      if (!a["description"].get<std::string>().empty()) {
        std::cout << "  -- " << a["description"].get<std::string>();
      }

      std::cout << "\n";
    }

    return 0;
  }

  if (!container_mode && cmd.get<bool>("--dry_run")) {
    const auto rep = server.validate(false);
    std::cout << (rep.valid ? "Dry-run plan valid: " : "Dry-run plan invalid: ") << rep.process_count << " processes, "
              << rep.component_count << " components, " << rep.container_count << " containers, " << rep.task_count
              << " tasks, " << rep.group_count << " groups, " << rep.slice_count << " slices, "
              << rep.scheduler_group_count << " schedulers" << (rep.has_proxy ? ", proxy=on" : "") << "\n";

    for (const auto& e : rep.errors) {
      std::cerr << "  ERROR: " << e << "\n";
    }

    for (const auto& w : rep.warnings) {
      std::cerr << "  WARN:  " << w << "\n";
    }

    std::cout << "\n" << server.export_graph(vlink::LauncherServer::GraphFormat::kAscii);
    return rep.valid ? 0 : 1;
  }

  SignalGuard signal_guard;
  (void)signal_guard;

  if (!server.start()) {
    std::cerr << "Error: failed to start LauncherServer.\n";
    return 1;
  }

  while (server.is_active()) {
    if (g_terminate_requested.load(std::memory_order_acquire)) {
      server.stop();
      break;
    }

    (void)server.wait(100);
  }

  return server.get_exit_code();
}

static int run_validate_command(const argparse::ArgumentParser& cmd) {
  vlink::Utils::unset_env("VLINK_BAG_PATH");
  vlink::LauncherServer::Config cfg;
  cfg.manage_console = false;
  cfg.enable_control_plane = false;
  std::string err;

  if (!parse_kv_list(cmd.get<std::vector<std::string>>("--arg"), cfg.arg_overrides, &err)) {
    std::cerr << "Error (--arg): " << err << "\n";
    return 1;
  }

  vlink::LauncherServer server(cfg);

  if (!server.load_file(cmd.get<std::string>("config"), &err)) {
    if (cmd.get<bool>("--json")) {
      nlohmann::ordered_json out;
      out["valid"] = false;
      out["errors"] = nlohmann::json::array({err});
      std::cout << out.dump(2) << "\n";
    } else {
      std::cerr << "INVALID: " << err << "\n";
    }

    return 1;
  }

  const auto rep = server.validate(cmd.get<bool>("--check_programs"));

  if (cmd.get<bool>("--json")) {
    nlohmann::ordered_json out;
    out["valid"] = rep.valid;
    out["processes"] = rep.process_count;
    out["components"] = rep.component_count;
    out["containers"] = rep.container_count;
    out["tasks"] = rep.task_count;
    out["groups"] = rep.group_count;
    out["slices"] = rep.slice_count;
    out["schedulers"] = rep.scheduler_group_count;
    out["proxy"] = rep.has_proxy;
    out["warnings"] = rep.warnings;
    out["errors"] = rep.errors;
    std::cout << out.dump(2) << "\n";
    return rep.valid ? 0 : 1;
  }

  for (const auto& w : rep.warnings) {
    std::cerr << "WARN:  " << w << "\n";
  }

  for (const auto& e : rep.errors) {
    std::cerr << "ERROR: " << e << "\n";
  }

  if (!rep.valid) {
    return 1;
  }

  std::cout << "OK: " << rep.process_count << " processes, " << rep.component_count << " components, "
            << rep.container_count << " containers, " << rep.task_count << " tasks, " << rep.group_count << " groups, "
            << rep.slice_count << " slices, " << rep.scheduler_group_count << " schedulers"
            << (rep.has_proxy ? ", proxy=on" : "") << "\n";
  return 0;
}

static int run_graph_command(const argparse::ArgumentParser& cmd) {
  vlink::Utils::unset_env("VLINK_BAG_PATH");
  const auto fmt_str = cmd.get<std::string>("--format");
  auto fmt = vlink::LauncherServer::GraphFormat::kAscii;

  if (fmt_str == "dot") {
    fmt = vlink::LauncherServer::GraphFormat::kDot;
  } else if (fmt_str == "mermaid") {
    fmt = vlink::LauncherServer::GraphFormat::kMermaid;
  } else if (fmt_str != "ascii") {
    std::cerr << "Error: unknown graph format '" << fmt_str << "' (expected ascii|dot|mermaid).\n";
    return 1;
  }

  vlink::LauncherServer::Config cfg;
  cfg.manage_console = false;
  cfg.enable_control_plane = false;
  std::string err;

  if (!parse_kv_list(cmd.get<std::vector<std::string>>("--arg"), cfg.arg_overrides, &err)) {
    std::cerr << "Error (--arg): " << err << "\n";
    return 1;
  }

  vlink::LauncherServer server(cfg);

  if (!server.load_file(cmd.get<std::string>("config"), &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  const std::string output = server.export_graph(fmt);
  const auto out_file = cmd.get<std::string>("--out");

  if (!out_file.empty()) {
    std::ofstream ofs(out_file, std::ios::out | std::ios::trunc);

    if (!ofs.is_open()) {
      std::cerr << "Error: cannot write to '" << out_file << "'.\n";
      return 1;
    }

    ofs << output;
    return 0;
  }

  std::cout << output;
  return 0;
}

static int run_inspect_command(const argparse::ArgumentParser& cmd) {
  vlink::Utils::unset_env("VLINK_BAG_PATH");
  vlink::LauncherServer::Config cfg;
  cfg.manage_console = false;
  cfg.enable_control_plane = false;
  std::string err;

  if (!parse_kv_list(cmd.get<std::vector<std::string>>("--arg"), cfg.arg_overrides, &err)) {
    std::cerr << "Error (--arg): " << err << "\n";
    return 1;
  }

  vlink::LauncherServer server(cfg);

  if (!server.load_file(cmd.get<std::string>("config"), &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  const std::string json_str = server.inspect_manifest_json();

  if (cmd.get<bool>("--json")) {
    std::cout << json_str << "\n";
    return 0;
  }

  const auto doc = nlohmann::json::parse(json_str);
  std::cout << "Manifest: " << doc["name"].get<std::string>() << "  mode=" << doc["startup_mode"].get<std::string>()
            << "  max_parallel=" << doc["max_parallel"].get<uint32_t>() << "\n";

  if (cmd.get<bool>("--show_args") || !doc["args"].empty()) {
    std::cout << "\nArguments:\n";

    for (const auto& a : doc["args"]) {
      std::cout << "  " << a["name"].get<std::string>() << " = " << a["resolved"].get<std::string>();

      if (!a["description"].get<std::string>().empty()) {
        std::cout << "  (" << a["description"].get<std::string>() << ")";
      }

      std::cout << "\n";
    }
  }

  std::cout << "\n" << server.export_graph(vlink::LauncherServer::GraphFormat::kAscii);
  return 0;
}

static int run_plugin_command(const argparse::ArgumentParser& cmd) {
  vlink::Utils::unset_env("VLINK_BAG_PATH");
  const auto info = vlink::LauncherServer::inspect_plugin(cmd.get<std::string>("library"));

  if (cmd.get<bool>("--json")) {
    nlohmann::ordered_json j;
    j["path"] = info.path;
    j["loaded"] = info.loaded;
    j["launcher_module"] = info.is_launcher_module;
    j["legacy_runnable"] = info.is_legacy_runnable;
    j["logger_plugin"] = info.is_logger_plugin;
    j["version"] = std::to_string(info.version_major) + "." + std::to_string(info.version_minor);
    j["components"] = info.components;
    j["error"] = info.error;
    std::cout << j.dump(2) << "\n";
    return info.loaded ? 0 : 1;
  }

  if (!info.loaded) {
    std::cerr << "Plugin '" << info.path << "' failed: " << info.error << "\n";
    return 1;
  }

  const char* plugin_type = "RunablePluginInterface (legacy)";

  if (info.is_launcher_module) {
    plugin_type = "LauncherModuleInterface";
  } else if (info.is_logger_plugin) {
    plugin_type = "LoggerPluginInterface";
  }

  std::cout << "Plugin: " << info.path << " (v" << info.version_major << "." << info.version_minor << ")\n";
  std::cout << "Type:   " << plugin_type << "\n";

  if (!info.components.empty()) {
    std::cout << "Exported components (" << info.components.size() << "):\n";

    for (const auto& c : info.components) {
      std::cout << "  - " << c << "\n";
    }
  }

  return 0;
}

static int run_init_command(const argparse::ArgumentParser& cmd) {
  const auto tpl = cmd.get<std::string>("--template");
  std::string content;

  if (tpl == "component") {
    content =
        "[launcher]\nname = \"vlink-components\"\nstartup = \"parallel\"\nstop_timeout_ms = 5000\n\n"
        "[[scheduler]]\nname = \"rt_pool\"\nmode = \"pinned\"\nqueue = \"priority\"\nthreads = 2\n\n"
        "[[scheduler]]\nname = \"compute_pool\"\nmode = \"pool\"\nqueue = \"lockfree\"\nthreads = 4\n\n"
        "[[component]]\nname = \"talker\"\nlib = \"my_components\"\nclass = \"TalkerComponent\"\n"
        "scheduler = \"rt_pool\"\npool = \"compute_pool\"\nworker = 0\nwatchdog_ms = 5000\n"
        "reload = \"component\"\n\n[component.params]\ninterval_ms = 500\n";
  } else if (tpl == "systemd") {
    content =
        "[launcher]\nname = \"vlink-stack\"\nstartup = \"max_parallel\"\nmax_parallel = 4\n"
        "stop_timeout_ms = 5000\n\n"
        "[[slice]]\nname = \"perception\"\ncpu_quota_percent = 200\nmem_max_mb = 2048\npids_max = 256\n\n"
        "[[group]]\nname = \"bringup\"\nmode = \"serial\"\nslice = \"perception\"\n\n"
        "[[process]]\nname = \"sensor_driver\"\ngroup = \"bringup\"\nstage = 0\ncmd = \"/usr/bin/env\"\n"
        "args = [\"sh\", \"-c\", \"echo READY && sleep 30\"]\nrestart = \"on_failure\"\n"
        "start_burst = 5\nstart_window_ms = 60000\nkill_mode = \"control-group\"\nreload = \"sighup\"\n"
        "\n[process.ready]\ntype = \"stdout\"\npattern = \"READY\"\ntimeout_ms = 5000\n"
        "\n[process.resources]\nslice = \"perception\"\nnice = -5\n";
  } else if (tpl == "minimal") {
    content =
        "[launcher]\nname = \"vlink-app\"\nstartup = \"parallel\"\nstop_timeout_ms = 5000\n\n"
        "[args.mode]\ndefault = \"dev\"\nchoices = [\"dev\", \"prod\"]\ndesc = \"Runtime profile\"\n\n"
        "[[process]]\nname = \"hello\"\ncmd = \"/bin/echo\"\n"
        "args = [\"Hello from VLink Launcher (${arg.mode})\"]\n";
  } else {
    std::cerr << "Error: unknown template '" << tpl << "' (expected minimal|component|systemd).\n";
    return 1;
  }

  const auto out_file = cmd.get<std::string>("--out");

  if (!out_file.empty()) {
    std::ofstream ofs(out_file, std::ios::out | std::ios::trunc);

    if (!ofs.is_open()) {
      std::cerr << "Error: cannot write '" << out_file << "'.\n";
      return 1;
    }

    ofs << content;
    std::cout << "Generated '" << out_file << "' (template=" << tpl << ").\n";
    return 0;
  }

  std::cout << content;
  return 0;
}

static int run_top_command(const argparse::ArgumentParser& cmd) {
  vlink::Utils::unset_env("VLINK_BAG_PATH");
  vlink::LauncherAPI::Config cfg;
  cfg.scheme = cmd.get<std::string>("--scheme");
  cfg.namespace_prefix = cmd.get<std::string>("--namespace");
  cfg.domain_id = static_cast<uint32_t>(std::max<int32_t>(0, cmd.get<int32_t>("--domain")));
  cfg.security_key = cmd.get<std::string>("--key");

  if (cfg.security_key.empty()) {
    cfg.security_key = vlink::Utils::get_env("VLINK_LAUNCHER_SECURITY_KEY");
  }

  cfg.bind_ip = cmd.get<std::string>("--bind_ip");
  cfg.native_mode = cmd.get<bool>("--native");
  cfg.subscribe_events = false;
  cfg.subscribe_logs = false;
  cfg.default_timeout_ms = 3000U;

  vlink::LauncherAPI api(cfg);
  vlink::launcher::wipe_secret_string(cfg.security_key);
  api.async_run();

  if (!api.wait_for_connected(3000U)) {
    std::cerr << "Error: cannot connect to LauncherServer.\n";
    return 1;
  }

  VLINK_TERM_OUT.init();
  const bool is_tty = VLINK_TERM_OUT.is_tty();

  if (!is_tty) {
    const auto res = api.get_status();

    if (!res.ok()) {
      std::cerr << "Error: " << res.message << "\n";
      return 1;
    }

    print_snapshot_table(res.snapshot, false);
    return 0;
  }

  struct TuiScreenGuard final {
    SignalGuard signal_guard;
    bool entered{false};

    TuiScreenGuard() {
      VLINK_TERM_OUT << "\033[?1049h\033[?25l" << vlink::TerminalStream::flush_manip;
      entered = true;
    }

    ~TuiScreenGuard() {
      if (entered) {
        VLINK_TERM_OUT << "\033[?25h\033[?1049l" << vlink::TerminalStream::flush_manip;
      }
    }
  } screen_guard;
  (void)screen_guard;

  std::mutex ui_mtx;
  vlink::ConditionVariable ui_cv;
  size_t selected = 0U;
  bool blame_sort = false;
  bool show_logs = false;
  bool ui_dirty = true;
  std::string status_banner;
  std::string pending_action;
  std::string pending_target;
  std::vector<std::string> displayed_names;
  std::atomic_bool quit_ui{false};

  struct TuiKeyboardGuard final {
    ~TuiKeyboardGuard() { vlink::Utils::stop_detect_keyboard(); }
  } keyboard_guard;
  (void)keyboard_guard;

  vlink::Utils::start_detect_keyboard([&](const std::string& key) {
    {
      std::lock_guard lock(ui_mtx);

      if (key == "q" || key == "Q" || key == "esc") {
        quit_ui.store(true, std::memory_order_release);
      } else if (key == "j" || key == "down") {
        ++selected;
      } else if (key == "k" || key == "up") {
        if (selected > 0U) {
          --selected;
        }
      } else if (key == "b") {
        blame_sort = !blame_sort;
        status_banner = blame_sort ? "Sorted by startup latency (blame)" : "Sorted by manifest order";
      } else if (key == "l") {
        show_logs = !show_logs;
      } else if ((key == "s" || key == "x" || key == "r" || key == "o") && !displayed_names.empty()) {
        const size_t idx = std::min(selected, displayed_names.size() - 1U);
        pending_action = key;
        pending_target = displayed_names[idx];
      }

      ui_dirty = true;
    }

    ui_cv.notify_all();
  });

  vlink::launcher::SystemSnapshot last_snap;
  bool has_last_snap = false;

  auto clamp_line = [](const std::string& text, int max_cols) -> std::string {
    if (max_cols <= 0 || text.size() <= static_cast<size_t>(max_cols)) {
      return text;
    }

    return text.substr(0U, static_cast<size_t>(max_cols));
  };

  while (!quit_ui.load(std::memory_order_acquire) && !g_terminate_requested.load(std::memory_order_acquire)) {
    auto res = api.get_status(2000U);
    bool disconnected = false;
    std::string disconnect_msg;

    if (res.ok()) {
      last_snap = std::move(res.snapshot);
      has_last_snap = true;
    } else {
      disconnected = true;
      disconnect_msg = res.message.empty() ? "Lost connection to LauncherServer" : res.message;
    }

    if (has_last_snap || disconnected) {
      auto snap = last_snap;
      std::string act;
      std::string act_target;
      bool want_blame = false;
      bool want_logs = false;
      size_t cur_sel = 0U;
      std::string banner;

      {
        std::lock_guard lock(ui_mtx);
        want_blame = blame_sort;
        want_logs = show_logs;
        act = std::move(pending_action);
        act_target = std::move(pending_target);
        pending_action.clear();
        pending_target.clear();
        ui_dirty = false;

        if (!snap.units.empty() && selected >= snap.units.size()) {
          selected = snap.units.size() - 1U;
        }

        cur_sel = selected;
        banner = status_banner;
      }

      if (want_blame) {
        std::sort(snap.units.begin(), snap.units.end(),
                  [](const vlink::launcher::UnitStatus& a, const vlink::launcher::UnitStatus& b) {
                    return a.startup_duration_ms > b.startup_duration_ms;
                  });
      }

      {
        std::lock_guard lock(ui_mtx);
        displayed_names.clear();
        displayed_names.reserve(snap.units.size());

        for (const auto& u : snap.units) {
          displayed_names.push_back(u.name);
        }
      }

      if (!disconnected && !act.empty() && !act_target.empty()) {
        vlink::launcher::CommandResult cmd_res;

        if (act == "s") {
          cmd_res = api.start_unit(act_target);
        } else if (act == "x") {
          cmd_res = api.stop_unit(act_target);
        } else if (act == "r") {
          cmd_res = api.restart_unit(act_target);
        } else if (act == "o") {
          cmd_res = api.reload_unit(act_target);
        }

        std::lock_guard lock(ui_mtx);
        status_banner = act_target + ": " + cmd_res.message;
        banner = status_banner;
      }

      const auto [cols, rows_term] = vlink::Utils::get_terminal_size();
      const int content_cols = (cols > 4) ? (cols - 2) : 0;

      std::string frame;
      frame.reserve(8192U);
      frame.append("\033[2J\033[H");
      frame.append("\033[1;36mVLink Launcher Top\033[0m  session=")
          .append(snap.session_id)
          .append("  host=")
          .append(snap.host)
          .append("  cpu=")
          .append(format_cpu(snap.cpu_usage))
          .append("  mem=")
          .append(format_cpu(snap.memory_usage))
          .append("  uptime=")
          .append(vlink::Helpers::format_milliseconds(static_cast<int64_t>(snap.uptime_ms), false))
          .append("  boot=")
          .append(std::to_string(snap.boot_duration_ms))
          .append("ms\n");
      frame.append(
          "\033[90mKeys: [j/k] Select  [s] Start  [x] Stop  [r] Restart  [o] Reload  [b] Blame  [l] Logs  [q] "
          "Quit\033[0m\n");

      if (disconnected) {
        frame.append("\033[1;31m! DISCONNECTED: ").append(clamp_line(disconnect_msg, content_cols)).append("\033[0m\n");
      } else if (!banner.empty()) {
        frame.append("\033[33m> ").append(clamp_line(banner, content_cols)).append("\033[0m\n");
      }

      const std::string header_line =
          "  UNIT                      TYPE       STATE       PID      CPU     RSS      RST   BOOT    DETAIL";
      frame.append("\n\033[1m").append(clamp_line(header_line, cols)).append("\033[0m\n");

      const int reserved = want_logs ? 14 : 7;
      const size_t max_rows = (rows_term > reserved + 2) ? static_cast<size_t>(rows_term - reserved) : 4U;
      const size_t start_row = (cur_sel >= max_rows) ? (cur_sel - max_rows + 1U) : 0U;
      const size_t end_row = std::min(snap.units.size(), start_row + max_rows);

      for (size_t i = start_row; i < end_row; ++i) {
        const auto& u = snap.units[i];
        char line_buf[512];
        const std::string pid_str = (u.pid >= 0) ? std::to_string(u.pid) : "-";
        const std::string rss = format_kb(u.cgroup_memory_kb > 0U ? u.cgroup_memory_kb : u.memory_rss_kb);
        const std::string cpu = format_cpu(u.cpu_usage);
        std::snprintf(line_buf, sizeof(line_buf),
                      "%-25.25s %-10.10s %-11.11s %-8.8s %-7.7s %-8.8s %-5u %-7" PRIu64 " %.64s", u.name.c_str(),
                      vlink::launcher::unit_type_to_string(u.type), vlink::launcher::unit_state_to_string(u.state),
                      pid_str.c_str(), cpu.c_str(), rss.c_str(), u.restart_count, u.startup_duration_ms,
                      u.detail.c_str());
        const std::string row_text = clamp_line(line_buf, content_cols);

        if (i == cur_sel) {
          frame.append("\033[7m> ").append(row_text).append("\033[0m\n");
        } else {
          frame.append(state_color(u.state, true)).append("● \033[0m").append(row_text).append("\n");
        }
      }

      if (!disconnected && want_logs && !snap.units.empty() && cur_sel < snap.units.size()) {
        vlink::launcher::LogQuery q;
        q.unit_filter = snap.units[cur_sel].name;
        q.limit = 6U;
        const auto lres = api.query_logs(q, 1000U);
        frame.append("\n\033[1mLogs [" + q.unit_filter + "]:\033[0m\n");

        for (const auto& rec : lres.logs) {
          std::string log_line = "  [";
          log_line.append(level_short_name(rec.level)).append("] ").append(rec.text);
          frame.append(clamp_line(log_line, cols)).append("\n");
        }
      }

      VLINK_TERM_OUT.write_raw(frame.data(), frame.size());
      VLINK_TERM_OUT.flush();
    }

    for (int step = 0; step < 10; ++step) {
      if (quit_ui.load(std::memory_order_acquire) || g_terminate_requested.load(std::memory_order_acquire)) {
        break;
      }

      std::unique_lock lock(ui_mtx);

      if (ui_cv.wait_for(lock, std::chrono::milliseconds(100),
                         [&]() { return ui_dirty || quit_ui.load(std::memory_order_acquire); })) {
        break;
      }
    }
  }

  return 0;
}

static int run_ctl_command(const argparse::ArgumentParser& cmd) {
  vlink::Utils::unset_env("VLINK_BAG_PATH");
  const auto action = cmd.get<std::string>("action");
  auto target = cmd.get<std::string>("--unit");
  const auto pos_targets = cmd.get<std::vector<std::string>>("target");

  if (target.empty() && !pos_targets.empty()) {
    const bool is_global_set_param =
        (action == "set-param" && pos_targets.size() == 1U && pos_targets[0].find('=') != std::string::npos);

    if (!is_global_set_param) {
      target = pos_targets[0];
    }
  }

  const uint32_t timeout_ms = static_cast<uint32_t>(std::max<int32_t>(100, cmd.get<int32_t>("--timeout")));
  const bool want_json = cmd.get<bool>("--json");

  VLINK_TERM_OUT.init();
  const bool use_color = VLINK_TERM_OUT.is_tty() && !want_json;

  vlink::LauncherAPI::Config cfg;
  cfg.scheme = cmd.get<std::string>("--scheme");
  cfg.namespace_prefix = cmd.get<std::string>("--namespace");
  cfg.domain_id = static_cast<uint32_t>(std::max<int32_t>(0, cmd.get<int32_t>("--domain")));
  cfg.security_key = cmd.get<std::string>("--key");

  if (cfg.security_key.empty()) {
    cfg.security_key = vlink::Utils::get_env("VLINK_LAUNCHER_SECURITY_KEY");
  }

  cfg.bind_ip = cmd.get<std::string>("--bind_ip");
  cfg.native_mode = cmd.get<bool>("--native");
  cfg.subscribe_events = (action == "events");
  cfg.subscribe_logs = (action == "logs");
  cfg.default_timeout_ms = timeout_ms;

  vlink::LauncherAPI api(cfg);
  vlink::launcher::wipe_secret_string(cfg.security_key);
  api.async_run();

  if (action == "events" || action == "logs") {
    uint8_t min_lv = 0U;
    std::string level_err;

    if (!parse_cli_level(cmd.get<std::string>("--level"), min_lv, &level_err)) {
      std::cerr << "Error: " << level_err << "\n";
      return 1;
    }

    const auto grep = cmd.get<std::string>("--grep");
    const bool stderr_only = cmd.get<bool>("--stderr");
    const auto tail_n = cmd.get<int32_t>("--lines");
    const bool follow = cmd.get<bool>("--follow") || (tail_n < 0);

    if (action == "logs") {
      if (!api.wait_for_connected(timeout_ms) && !follow) {
        std::cerr << "Error: failed to connect to LauncherServer.\n";
        return 1;
      }

      if (!follow) {
        vlink::launcher::LogQuery q;
        q.unit_filter = target;
        q.grep = grep;
        q.min_level = min_lv;
        q.stderr_only = stderr_only;
        q.limit = static_cast<uint32_t>(tail_n > 0 ? tail_n : 50);
        const auto hist = api.query_logs(q, timeout_ms);

        if (!hist.ok()) {
          std::cerr << "Error [" << vlink::launcher::error_code_to_string(hist.code) << "]: " << hist.message << "\n";
          return 1;
        }

        for (const auto& rec : hist.logs) {
          std::cout << "[" << rec.unit_name << "][" << level_short_name(rec.level) << "] " << rec.text << "\n";
        }

        return 0;
      }
    }

    SignalGuard signal_guard;
    (void)signal_guard;

    if (action == "events") {
      api.register_event_callback([target](const vlink::launcher::EventRecord& ev) {
        if (!target.empty() && ev.unit_name != target) {
          return;
        }

        std::cout << "[" << ev.unit_name << "] " << ev.event_name << " ("
                  << vlink::launcher::unit_state_to_string(ev.old_state) << " -> "
                  << vlink::launcher::unit_state_to_string(ev.new_state) << ") " << ev.message << std::endl;
      });
    } else {
      static constexpr size_t kMaxFollowBuffered = 4096U;

      struct FollowLogState final {
        std::mutex mtx;
        bool history_done{false};
        bool has_initial_sync{false};
        bool was_disconnected{true};
        bool need_refresh{false};
        uint64_t refresh_gen{0U};
        std::string session_id;
        std::unordered_set<std::string> group_members;
        uint64_t last_printed_seq{0U};
        std::deque<vlink::launcher::LogRecord> buffered;

        bool accept_seq(uint64_t seq) noexcept {
          if (seq == 0U) {
            return true;
          }

          if (seq <= last_printed_seq) {
            return false;
          }

          last_printed_seq = seq;
          return true;
        }
      };

      auto state = std::make_shared<FollowLogState>();
      const bool filter_by_group = !target.empty() && target[0] == '@';

      auto matches_filter_locked = [target, filter_by_group, min_lv, grep, stderr_only](
                                       const FollowLogState& st, const vlink::launcher::LogRecord& rec) -> bool {
        if (filter_by_group) {
          if (st.group_members.find(rec.unit_name) == st.group_members.end()) {
            return false;
          }
        } else if (!target.empty() && rec.unit_name != target) {
          return false;
        }

        if (rec.level < min_lv || (stderr_only && !rec.is_stderr)) {
          return false;
        }

        return grep.empty() || rec.text.find(grep) != std::string::npos;
      };

      auto sync_session_history = [&]() {
        uint64_t captured_gen = 0U;

        {
          std::lock_guard lock(state->mtx);
          captured_gen = state->refresh_gen;
        }

        std::unordered_set<std::string> refreshed_members;
        std::string current_session_id;
        const auto sres = api.get_status(timeout_ms);

        if (!sres.ok()) {
          std::lock_guard lock(state->mtx);
          state->need_refresh = true;
          return;
        }

        current_session_id = sres.snapshot.session_id;

        if (filter_by_group) {
          const std::string gname = target.substr(1U);

          for (const auto& u : sres.snapshot.units) {
            if (u.task_group == gname || u.group == gname) {
              refreshed_members.insert(u.name);
            }
          }
        }

        uint64_t after_seq = 0U;

        {
          std::lock_guard lock(state->mtx);

          if (!current_session_id.empty() && state->session_id == current_session_id) {
            after_seq = state->last_printed_seq;
          }
        }

        vlink::launcher::LogQuery q;
        q.unit_filter = target;
        q.grep = grep;
        q.min_level = min_lv;
        q.stderr_only = stderr_only;
        q.after_seq = after_seq;
        q.limit = static_cast<uint32_t>(tail_n > 0 ? tail_n : 50);
        const auto hist = api.query_logs(q, timeout_ms);

        std::lock_guard lock(state->mtx);

        if (!hist.ok() || state->refresh_gen != captured_gen) {
          state->need_refresh = true;
          return;
        }

        if (!current_session_id.empty()) {
          if (!state->session_id.empty() && state->session_id != current_session_id) {
            state->last_printed_seq = 0U;
          }

          state->session_id = std::move(current_session_id);
        }

        if (filter_by_group) {
          state->group_members = std::move(refreshed_members);
        }

        for (const auto& rec : hist.logs) {
          if (state->accept_seq(rec.seq)) {
            std::cout << "[" << rec.unit_name << "][" << level_short_name(rec.level) << "] " << rec.text << "\n";
          }
        }

        for (const auto& rec : state->buffered) {
          if (matches_filter_locked(*state, rec) && state->accept_seq(rec.seq)) {
            std::cout << "[" << rec.unit_name << "][" << level_short_name(rec.level) << "] " << rec.text << "\n";
          }
        }

        state->buffered.clear();
        state->was_disconnected = false;
        state->history_done = true;
        state->has_initial_sync = true;
        state->need_refresh = false;
        std::cout.flush();
      };

      api.register_connect_callback([state](bool connected) {
        std::lock_guard lock(state->mtx);

        if (!connected) {
          state->was_disconnected = true;
          return;
        }

        if (state->was_disconnected || state->has_initial_sync) {
          state->was_disconnected = false;
          state->history_done = false;
          ++state->refresh_gen;
          state->need_refresh = true;
        }
      });

      api.register_log_callback([state, matches_filter_locked](const vlink::launcher::LogRecord& rec) {
        std::lock_guard lock(state->mtx);

        if (!state->history_done) {
          if (state->buffered.size() >= kMaxFollowBuffered) {
            state->buffered.pop_front();
          }

          state->buffered.push_back(rec);
          return;
        }

        if (!matches_filter_locked(*state, rec) || !state->accept_seq(rec.seq)) {
          return;
        }

        std::cout << "[" << rec.unit_name << "][" << level_short_name(rec.level) << "] " << rec.text << std::endl;
      });

      if (api.is_connected()) {
        std::lock_guard lock(state->mtx);
        ++state->refresh_gen;
        state->need_refresh = true;
      }

      while (!g_terminate_requested.load(std::memory_order_acquire)) {
        bool do_refresh = false;

        {
          std::lock_guard lock(state->mtx);
          do_refresh = state->need_refresh;
        }

        if (do_refresh && api.is_connected()) {
          sync_session_history();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }

      api.register_connect_callback(nullptr);
      api.register_event_callback(nullptr);
      api.register_log_callback(nullptr);
      return 0;
    }

    while (!g_terminate_requested.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    api.register_connect_callback(nullptr);
    api.register_event_callback(nullptr);
    api.register_log_callback(nullptr);
    return 0;
  }

  if (action == "status" || action == "list-units") {
    const bool watch = (action == "status") && cmd.get<bool>("--watch");
    SignalGuard signal_guard;
    (void)signal_guard;

    do {
      const auto res = api.get_status(timeout_ms);

      if (!res.ok()) {
        std::cerr << "Error [" << vlink::launcher::error_code_to_string(res.code) << "]: " << res.message << "\n";
        return 1;
      }

      if (watch && use_color) {
        std::cout << "\033[2J\033[H";
      }

      const std::string& effective_target = (action == "list-units") ? std::string() : target;

      if (want_json) {
        if (!effective_target.empty() && effective_target[0] != '@') {
          auto single = copy_snapshot_metadata(res.snapshot);

          for (const auto& u : res.snapshot.units) {
            if (u.name == effective_target) {
              single.units.push_back(u);
              break;
            }
          }

          if (single.units.empty()) {
            std::cerr << "Unit '" << effective_target << "' not found.\n";
            return 1;
          }

          std::cout << snapshot_to_json(single).dump(2) << "\n";
        } else {
          const auto filtered =
              filter_snapshot(res.snapshot, cmd.get<std::string>("--type"), cmd.get<std::string>("--state"),
                              !effective_target.empty() ? effective_target : cmd.get<std::string>("--group"),
                              cmd.get<bool>("--failed"));
          std::cout << snapshot_to_json(filtered).dump(2) << "\n";
        }
      } else if (!effective_target.empty() && effective_target[0] != '@') {
        bool found = false;

        for (const auto& u : res.snapshot.units) {
          if (u.name == effective_target) {
            vlink::launcher::LogQuery q;
            q.unit_filter = effective_target;
            q.limit = 10U;
            const auto lres = api.query_logs(q, timeout_ms);
            print_unit_detail_card(u, lres.logs, use_color);
            found = true;
            break;
          }
        }

        if (!found) {
          std::cerr << "Unit '" << effective_target << "' not found.\n";
          return 1;
        }
      } else {
        print_snapshot_table(res.snapshot, use_color, cmd.get<std::string>("--type"), cmd.get<std::string>("--state"),
                             !effective_target.empty() ? effective_target : cmd.get<std::string>("--group"),
                             cmd.get<bool>("--failed"));
      }

      if (watch) {
        for (int i = 0; i < 10 && !g_terminate_requested.load(std::memory_order_acquire); ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      }
    } while (watch && !g_terminate_requested.load(std::memory_order_acquire));

    return 0;
  }

  if (action == "list-dependencies") {
    const auto res = api.get_status(timeout_ms);

    if (!res.ok()) {
      std::cerr << "Error: " << res.message << "\n";
      return 1;
    }

    return print_dependencies_tree(res.snapshot, target, cmd.get<bool>("--reverse"), use_color) ? 0 : 1;
  }

  if (action == "blame") {
    const auto res = api.get_status(timeout_ms);

    if (!res.ok()) {
      std::cerr << "Error: " << res.message << "\n";
      return 1;
    }

    print_blame(res.snapshot, use_color);
    return 0;
  }

  if (action == "critical-chain") {
    const auto res = api.get_status(timeout_ms);

    if (!res.ok()) {
      std::cerr << "Error: " << res.message << "\n";
      return 1;
    }

    return print_critical_chain(res.snapshot, target, use_color) ? 0 : 1;
  }

  vlink::launcher::CommandResult res;

  if (action == "start") {
    res = api.start_unit(target, timeout_ms);
  } else if (action == "stop") {
    res = api.stop_unit(target, timeout_ms);
  } else if (action == "restart") {
    res = api.restart_unit(target, timeout_ms);
  } else if (action == "reload") {
    res = api.reload_unit(target, timeout_ms);
  } else if (action == "activate") {
    res = api.activate_unit(target, timeout_ms);
  } else if (action == "deactivate") {
    res = api.deactivate_unit(target, timeout_ms);
  } else if (action == "reset-failed") {
    res = api.reset_failed(target, timeout_ms);
  } else if (action == "isolate") {
    res = api.isolate_group(target, timeout_ms);
  } else if (action == "cat") {
    res = api.get_unit_config(target, timeout_ms);

    if (res.ok()) {
      std::cout << res.message;
      return 0;
    }
  } else if (action == "resume") {
    res = api.resume_startup(timeout_ms);
  } else if (action == "unload") {
    res = api.unload_component(target, timeout_ms);
  } else if (action == "shutdown") {
    res = api.shutdown_server(timeout_ms);
  } else if (action == "params") {
    res = api.get_params(target, timeout_ms);

    if (res.ok()) {
      for (const auto& entry : res.params) {
        std::cout << entry.key << " = " << entry.value << "\n";
      }

      return 0;
    }
  } else if (action == "set-param") {
    auto key = cmd.get<std::string>("--param_key");
    auto val = cmd.get<std::string>("--param_val");

    if (key.empty() && !pos_targets.empty()) {
      const auto& kv = pos_targets.back();
      const size_t eq = kv.find('=');

      if (eq == std::string::npos || eq == 0U || (kv[eq - 1U] == ':' && eq == 1U)) {
        std::cerr << "Error: invalid set-param specification '" << kv << "' (expected key=value).\n";
        return 1;
      }

      const size_t key_end = (kv[eq - 1U] == ':') ? (eq - 1U) : eq;
      key = kv.substr(0U, key_end);
      val = kv.substr(eq + 1U);
    }

    vlink::launcher::ParamEntry entry;
    std::string param_err;

    if (!parse_param_spec(key, cmd.get<std::string>("--param_type"), val, entry, &param_err)) {
      std::cerr << "Error: " << param_err << "\n";
      return 1;
    }

    res = api.set_param(target, entry, timeout_ms);
  } else if (action == "load") {
    vlink::launcher::LoadComponentRequest req;
    req.name = target;
    req.library = cmd.get<std::string>("--lib");
    req.class_name = cmd.get<std::string>("--class");
    req.scheduler_group = cmd.get<std::string>("--scheduler");
    req.compute_pool = cmd.get<std::string>("--compute_pool");
    req.container = cmd.get<std::string>("--container");
    req.worker_index = cmd.get<int32_t>("--worker");
    req.task_priority = static_cast<uint16_t>(std::clamp<int32_t>(cmd.get<int32_t>("--priority"), 0, 65535));
    req.legacy_runnable = cmd.get<bool>("--legacy");

    std::unordered_map<std::string, std::string> raw_params;
    std::string kv_err;

    if (!parse_kv_list(cmd.get<std::vector<std::string>>("--param"), raw_params, &kv_err)) {
      std::cerr << "Error (--param): " << kv_err << "\n";
      return 1;
    }

    for (const auto& [k, v] : raw_params) {
      vlink::launcher::ParamEntry p_ent;
      std::string perr;

      if (!parse_param_spec(k, "auto", v, p_ent, &perr)) {
        std::cerr << "Error (--param): " << perr << "\n";
        return 1;
      }

      req.params.push_back(std::move(p_ent));
    }

    if (!parse_kv_list(cmd.get<std::vector<std::string>>("--url"), req.urls, &kv_err)) {
      std::cerr << "Error (--url): " << kv_err << "\n";
      return 1;
    }

    if (!parse_kv_list(cmd.get<std::vector<std::string>>("--input"), req.inputs, &kv_err)) {
      std::cerr << "Error (--input): " << kv_err << "\n";
      return 1;
    }

    if (!parse_kv_list(cmd.get<std::vector<std::string>>("--output"), req.outputs, &kv_err)) {
      std::cerr << "Error (--output): " << kv_err << "\n";
      return 1;
    }

    res = api.load_component(req, timeout_ms);
  } else if (action == "record") {
    bool enable = !cmd.get<bool>("--stop");
    auto rec_target = cmd.get<std::string>("--unit");
    const auto rec_path = cmd.get<std::string>("--path");

    if (!pos_targets.empty()) {
      if (pos_targets[0] == "start") {
        enable = true;

        if (rec_target.empty() && pos_targets.size() >= 2U) {
          rec_target = pos_targets[1];
        }
      } else if (pos_targets[0] == "stop") {
        enable = false;

        if (rec_target.empty() && pos_targets.size() >= 2U) {
          rec_target = pos_targets[1];
        }
      } else if (rec_target.empty()) {
        rec_target = pos_targets[0];
      }
    }

    res = api.set_record(rec_target, enable, rec_path, timeout_ms);
  } else {
    std::cerr << "Unknown ctl action '" << action
              << "'. Supported: status, list-units, list-dependencies, blame, critical-chain, cat, start, stop, "
                 "restart, reload, activate, deactivate, reset-failed, isolate, resume, load, unload, params, "
                 "set-param, record, events, logs, shutdown.\n";
    return 1;
  }

  if (!res.ok()) {
    std::cerr << "Error [" << vlink::launcher::error_code_to_string(res.code) << "]: " << res.message << "\n";
    return 1;
  }

  if (!res.message.empty()) {
    std::cout << res.message << "\n";
  }

  return 0;
}

static std::string extract_log_timestamp_key(std::string_view line) {
  auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
  size_t pos = 0U;

  if (!line.empty() && line.front() == '[') {
    pos = 1U;
  }

  if (line.size() >= pos + 19U && is_digit(line[pos]) && is_digit(line[pos + 1U]) && is_digit(line[pos + 2U]) &&
      is_digit(line[pos + 3U]) && line[pos + 4U] == '-' && is_digit(line[pos + 5U]) && is_digit(line[pos + 6U]) &&
      line[pos + 7U] == '-' && is_digit(line[pos + 8U]) && is_digit(line[pos + 9U]) &&
      (line[pos + 10U] == ' ' || line[pos + 10U] == 'T')) {
    size_t end = pos + 19U;

    while (end < line.size() && (is_digit(line[end]) || line[end] == '.' || line[end] == ',')) {
      ++end;
    }

    return std::string(line.substr(pos, end - pos));
  }

  if (line.size() >= pos + 14U && is_digit(line[pos]) && is_digit(line[pos + 1U]) && line[pos + 2U] == '-' &&
      is_digit(line[pos + 3U]) && is_digit(line[pos + 4U]) && line[pos + 5U] == ' ' && is_digit(line[pos + 6U]) &&
      is_digit(line[pos + 7U]) && line[pos + 8U] == ':' && is_digit(line[pos + 9U]) && is_digit(line[pos + 10U]) &&
      line[pos + 11U] == ':' && is_digit(line[pos + 12U]) && is_digit(line[pos + 13U])) {
    size_t end = pos + 14U;

    while (end < line.size() && (is_digit(line[end]) || line[end] == '.' || line[end] == ',')) {
      ++end;
    }

    return std::string(line.substr(pos, end - pos));
  }

  return {};
}

static int run_offline_logs_command(const argparse::ArgumentParser& cmd) {
  vlink::Utils::unset_env("VLINK_BAG_PATH");
  const auto log_dir = cmd.get<std::string>("dir");
  const auto unit_filter = cmd.get<std::string>("--unit");
  const auto grep = cmd.get<std::string>("--grep");
  const auto tail_n = cmd.get<int32_t>("--lines");
  const size_t limit = (tail_n > 0) ? static_cast<size_t>(tail_n) : 200U;

  std::error_code ec;
  const std::filesystem::path root(log_dir);

  if VUNLIKELY (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
    std::cerr << "Error: log directory '" << log_dir << "' not found.\n";
    return 1;
  }

  struct LogFileEntry final {
    std::filesystem::path path;
    std::string unit_tag;
    bool is_logd{false};
    bool tag_is_sanitized{false};
  };

  std::vector<LogFileEntry> files;
  bool has_non_logd = false;

  auto derive_unit_tag = [&](const std::filesystem::path& p, bool& is_sanitized) -> std::string {
    is_sanitized = false;
    std::error_code rel_ec;
    const auto rel = std::filesystem::relative(p, root, rel_ec);

    if (!rel_ec) {
      std::vector<std::string> parts;

      for (const auto& comp : rel) {
        parts.push_back(comp.string());
      }

      const size_t n = parts.size();

      for (size_t i = 0U; i + 2U < n; ++i) {
        if (parts[i] == "nodes" || parts[i] == "containers") {
          is_sanitized = true;
          return parts[i + 1U];
        }
      }

      if (n >= 2U && (parts[n - 2U] == "launcher" || parts[n - 2U] == "logd")) {
        is_sanitized = true;
        return parts[n - 2U];
      }

      if (n >= 2U && parts[n - 2U] == "captured") {
        is_sanitized = true;
        return p.stem().string();
      }
    }

    return p.stem().string();
  };

  const std::string sanitized_filter =
      unit_filter.empty() ? std::string() : vlink::launcher::sanitize_identifier(unit_filter, true, "unit");

  try {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec)) {
      if (ec) {
        ec.clear();
        continue;
      }

      if (!entry.is_regular_file(ec)) {
        continue;
      }

      const auto& p = entry.path();

      if (p.extension() != ".log") {
        continue;
      }

      bool tag_sanitized = false;
      const std::string tag = derive_unit_tag(p, tag_sanitized);
      const bool is_logd_file = (tag == "logd");

      if (!unit_filter.empty() && !is_logd_file && tag != unit_filter && tag != sanitized_filter) {
        continue;
      }

      if (!is_logd_file) {
        has_non_logd = true;
      }

      files.push_back(LogFileEntry{p, tag, is_logd_file, tag_sanitized});
    }
  } catch (const std::filesystem::filesystem_error& err) {
    std::cerr << "Error scanning log directory '" << log_dir << "': " << err.what() << "\n";
    return 1;
  }

  std::sort(files.begin(), files.end(), [](const LogFileEntry& a, const LogFileEntry& b) {
    if (a.is_logd != b.is_logd) {
      return !a.is_logd;
    }

    return a.path < b.path;
  });

  if (files.empty()) {
    std::cerr << "No matching .log files found in '" << log_dir << "'.\n";
    return 1;
  }

  struct MergedLine final {
    std::string ts_key;
    uint64_t order{0U};
    std::string formatted;
    std::string dedup_key;
  };

  const std::string logd_unit_prefix = unit_filter.empty() ? std::string() : ("- [" + unit_filter + "] ");
  std::vector<MergedLine> merged;
  std::unordered_set<std::string> non_logd_dedup;
  uint64_t seq_order = 0U;

  for (const auto& item : files) {
    std::ifstream ifs(item.path);
    std::string line;
    std::string last_ts_key;
    std::deque<MergedLine> file_tail;

    while (std::getline(ifs, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }

      if (item.is_logd && !logd_unit_prefix.empty() && line.find(logd_unit_prefix) == std::string::npos) {
        continue;
      }

      if (!grep.empty() && line.find(grep) == std::string::npos) {
        continue;
      }

      if (item.is_logd && has_non_logd) {
        const size_t marker = line.find("- [");

        if (marker != std::string::npos) {
          const size_t close = line.find("] ", marker + 3U);

          if (close != std::string::npos) {
            const std::string embedded_unit = line.substr(marker + 3U, close - (marker + 3U));
            const std::string norm_unit = vlink::launcher::sanitize_identifier(embedded_unit, true, "unit");
            const std::string body = line.substr(close + 2U);

            std::string lookup_key;
            lookup_key.reserve(norm_unit.size() + 1U + body.size());
            lookup_key.append(norm_unit).push_back('\n');
            lookup_key.append(body);

            if (non_logd_dedup.count(lookup_key) != 0U) {
              continue;
            }
          }
        }
      }

      std::string ts = extract_log_timestamp_key(line);

      if (!ts.empty()) {
        last_ts_key = ts;
      } else if (!last_ts_key.empty()) {
        ts = last_ts_key;
      }

      std::string formatted = "[";
      formatted.append(item.unit_tag).append("] ").append(line);
      std::string dedup_key;

      if (!item.is_logd) {
        std::string_view payload(line);
        size_t pos = 0U;

        for (int seg = 0; seg < 2; ++seg) {
          const size_t sep = payload.find(" - ", pos);

          if (sep == std::string_view::npos) {
            pos = 0U;
            break;
          }

          pos = sep + 3U;
        }

        const std::string norm_tag =
            item.tag_is_sanitized ? item.unit_tag : vlink::launcher::sanitize_identifier(item.unit_tag, true, "unit");
        dedup_key = norm_tag + "\n" + std::string(payload.substr(pos));
      }

      if (file_tail.size() >= limit) {
        file_tail.pop_front();
      }

      file_tail.push_back(MergedLine{std::move(ts), seq_order++, std::move(formatted), std::move(dedup_key)});
    }

    for (auto& rec : file_tail) {
      if (!item.is_logd && !rec.dedup_key.empty()) {
        non_logd_dedup.insert(std::move(rec.dedup_key));
      }

      merged.push_back(std::move(rec));
    }
  }

  std::stable_sort(merged.begin(), merged.end(), [](const MergedLine& a, const MergedLine& b) {
    const bool a_has = !a.ts_key.empty();
    const bool b_has = !b.ts_key.empty();

    if (a_has != b_has) {
      return a_has;
    }

    if (a_has && a.ts_key != b.ts_key) {
      return a.ts_key < b.ts_key;
    }

    return a.order < b.order;
  });

  const size_t start_idx = (merged.size() > limit) ? (merged.size() - limit) : 0U;

  for (size_t i = start_idx; i < merged.size(); ++i) {
    std::cout << merged[i].formatted << "\n";
  }

  return 0;
}

static int run_completion_command(const argparse::ArgumentParser& cmd) {
  const auto shell = cmd.get<std::string>("shell");

  if (shell == "bash") {
    std::cout << R"BASH(_vlink_launcher_completions() {
  local cur="${COMP_WORDS[COMP_CWORD]}"
  local prev="${COMP_WORDS[COMP_CWORD-1]}"
  local subcmd="${COMP_WORDS[1]:-}"
  local ctl_action="${COMP_WORDS[2]:-}"
  COMPREPLY=()
  if [[ ${COMP_CWORD} -eq 1 ]]; then
    COMPREPLY=( $(compgen -W "run validate graph inspect logs plugin init top ctl completion -h --help -v --version" -- "${cur}") )
    return
  fi
  case "$prev" in
    --scheme) COMPREPLY=( $(compgen -W "dds ddsc ddsr zenoh intra" -- "${cur}") ); return ;;
    --format) COMPREPLY=( $(compgen -W "ascii dot mermaid" -- "${cur}") ); return ;;
    --template) COMPREPLY=( $(compgen -W "minimal component systemd" -- "${cur}") ); return ;;
    --log_level) COMPREPLY=( $(compgen -W "trace debug info warn error fatal off" -- "${cur}") ); return ;;
    --level) COMPREPLY=( $(compgen -W "trace debug info warn error fatal" -- "${cur}") ); return ;;
    --log_plugin) COMPREPLY=( $(compgen -W "logd" -- "${cur}") ); return ;;
    --type) COMPREPLY=( $(compgen -W "component process container task proxy" -- "${cur}") ); return ;;
    --state) COMPREPLY=( $(compgen -W "pending starting running ready inactive stopping stopped failed restarting skipped" -- "${cur}") ); return ;;
    --param_type) COMPREPLY=( $(compgen -W "auto bool int double string bool_list int_list double_list string_list" -- "${cur}") ); return ;;
  esac
  case "$subcmd" in
    run)
      COMPREPLY=( $(compgen -W "--arg --dry_run --show_args --serial --parallel --only --skip --debug_unit --no_respawn --pause_on_fail --log_level --log_plugin --no_control --plain --log_dir --record_dir --scheme --namespace --domain --key --bind_ip --native -h --help" -- "${cur}") )
      ;;
    validate)
      COMPREPLY=( $(compgen -W "--arg --check_programs --json -h --help" -- "${cur}") )
      ;;
    graph)
      COMPREPLY=( $(compgen -W "--format --out -o --arg -h --help" -- "${cur}") )
      ;;
    inspect)
      COMPREPLY=( $(compgen -W "--show_args --json --arg -h --help" -- "${cur}") )
      ;;
    plugin)
      COMPREPLY=( $(compgen -W "--json -h --help" -- "${cur}") )
      ;;
    init)
      COMPREPLY=( $(compgen -W "--template --out -o -h --help" -- "${cur}") )
      ;;
    top)
      COMPREPLY=( $(compgen -W "--scheme --namespace --domain --key --bind_ip --native -h --help" -- "${cur}") )
      ;;
    logs)
      COMPREPLY=( $(compgen -W "--unit -u --grep --lines -n -h --help" -- "${cur}") )
      ;;
    completion)
      COMPREPLY=( $(compgen -W "bash zsh" -- "${cur}") )
      ;;
    ctl)
      if [[ ${COMP_CWORD} -eq 2 ]]; then
        COMPREPLY=( $(compgen -W "status list-units list-dependencies blame critical-chain cat start stop restart reload activate deactivate reset-failed isolate resume load unload params set-param record events logs shutdown" -- "${cur}") )
      elif [[ "$ctl_action" == "record" && ${COMP_CWORD} -eq 3 && "$cur" != -* ]]; then
        COMPREPLY=( $(compgen -W "start stop" -- "${cur}") )
      else
        COMPREPLY=( $(compgen -W "--unit -u --type --state --group --failed --reverse --watch -w --json --lines -n --follow -f --level --grep --stderr --lib --class --scheduler --compute_pool --container --worker --priority --legacy --param --url --input --output --param_key --param_val --param_type --path --stop --timeout --scheme --namespace --domain --key --bind_ip --native -h --help" -- "${cur}") )
      fi
      ;;
  esac
}
complete -F _vlink_launcher_completions vlink-launcher launcher
)BASH";
    return 0;
  }

  if (shell == "zsh") {
    std::cout << R"ZSH(#compdef vlink-launcher launcher
_vlink-launcher() {
  local -a subcmds=(
    'run:Launch system from TOML manifest'
    'validate:Validate TOML manifest, dependencies & conflicts'
    'graph:Export startup DAG (ascii|dot|mermaid)'
    'inspect:Inspect resolved manifest & args'
    'logs:Query persistent offline session logs'
    'plugin:Inspect exported component or logger plugin library'
    'init:Generate starter TOML launch manifest'
    'top:Interactive live TUI dashboard'
    'ctl:Control running LauncherServer (systemctl/journalctl style)'
    'completion:Print bash/zsh completion script'
    '-h:Show help'
    '--help:Show help'
    '-v:Show version'
    '--version:Show version'
  )
  local -a ctl_actions=(
    'status:Show cluster or unit status'
    'list-units:List units with filters'
    'list-dependencies:Show dependency tree'
    'blame:Rank units by startup latency'
    'critical-chain:Show slowest startup path'
    'cat:Print effective unit config'
    'start:Start unit or @group'
    'stop:Stop unit or @group'
    'restart:Restart unit or @group'
    'reload:Reload unit in place'
    'activate:Activate standby component'
    'deactivate:Suspend component to standby'
    'reset-failed:Clear failed state'
    'isolate:Switch exclusively to @group'
    'resume:Resume paused startup queue'
    'load:Dynamically load component'
    'unload:Dynamically unload component'
    'params:Query parameters'
    'set-param:Update parameter at runtime'
    'record:Start or stop active bag recording'
    'events:Stream lifecycle events'
    'logs:Query or stream logs'
    'shutdown:Gracefully stop LauncherServer'
  )
  if (( CURRENT == 2 )); then
    _describe 'command' subcmds
    return
  fi
  case "${words[2]}" in
    completion)
      if (( CURRENT == 3 )); then
        _values 'shell' bash zsh
      fi
      ;;
    ctl)
      if (( CURRENT == 3 )); then
        _describe 'ctl action' ctl_actions
      elif [[ "${words[3]}" == "record" ]] && (( CURRENT == 4 )); then
        _values 'record action' start stop
      else
        _arguments -s '(-u --unit)'{-u,--unit}'=[Target unit or @group]:target:' '--type=[Filter by type]:type:(component process container task proxy)' '--state=[Filter by state]:state:(pending starting running ready inactive stopping stopped failed restarting skipped)' '--group=[Filter by group]:group:' '--failed[Show failed units only]' '--reverse[Show reverse dependencies]' '(-w --watch)'{-w,--watch}'[Continuously refresh]' '--json[JSON output]' '(-n --lines)'{-n,--lines}'=[Tail N lines]:count:' '(-f --follow)'{-f,--follow}'[Follow live logs]' '--level=[Min level]:level:(trace debug info warn error fatal)' '--grep=[Substring filter]:pattern:' '--stderr[Stderr only]' '--lib=[Shared library for load]:lib:' '--class=[Component class name for load]:cls:' '--scheduler=[Scheduler group for load]:sched:' '--compute_pool=[Compute ThreadPool group for load]:pool:' '--container=[Target container for load]:container:' '--worker=[Pinned worker index]:idx:' '--priority=[Default task priority]:prio:' '--legacy[Load legacy RunablePluginInterface]' '*--param=[Initial param k=v]:kv:' '*--url=[Named URL k=url]:kv:' '*--input=[Input port k=url]:kv:' '*--output=[Output port k=url]:kv:' '--param_key=[Parameter key]:key:' '--param_val=[Parameter value]:val:' '--param_type=[Param type]:type:(auto bool int double string bool_list int_list double_list string_list)' '--path=[Bag output path for ctl record]:file:_files -g "*.(vcap|vcapx|vdb|vdbx)"' '--stop[Stop active recording for ctl record]' '--timeout=[RPC timeout ms]:ms:' '--scheme=[Control scheme]:scheme:(dds ddsc ddsr zenoh intra)' '--namespace=[Control namespace prefix]:ns:' '--domain=[Control domain ID]:id:' '--key=[AES-128-GCM security key]:key:' '--bind_ip=[Transport bind IP]:ip:' '--native[Restrict discovery to local host]' '(-h --help)'{-h,--help}'[Show help]'
      fi
      ;;
    run)
      _arguments -s '*:TOML manifest:_files -g "*.toml"' '*--arg=[Override arg]:kv:' '--dry_run[Dry run]' '--show_args[List args]' '--serial[Serial boot]' '--parallel=[Max parallel]:count:' '*--only=[Only targets]:target:' '*--skip=[Skip targets]:target:' '*--debug_unit=[Wrap unit with prefix]:spec:' '--no_respawn[Disable respawn]' '--pause_on_fail[Pause on failure]' '--log_level=[Console level]:level:(trace debug info warn error fatal off)' '--log_plugin=[Logger plugin]:plugin:(logd)' '--no_control[Disable control-plane endpoints]' '--plain[Disable ANSI colors]' '--log_dir=[Session log dir]:dir:_files -/' '--record_dir=[Active recording dir]:dir:_files -/' '--scheme=[Control scheme]:scheme:(dds ddsc ddsr zenoh intra)' '--namespace=[Control namespace prefix]:ns:' '--domain=[Control domain ID]:id:' '--key=[AES-128-GCM security key]:key:' '--bind_ip=[Transport bind IP]:ip:' '--native[Restrict discovery to local host]' '(-h --help)'{-h,--help}'[Show help]'
      ;;
    top)
      _arguments -s '--scheme=[Control scheme]:scheme:(dds ddsc ddsr zenoh intra)' '--namespace=[Control namespace prefix]:ns:' '--domain=[Control domain ID]:id:' '--key=[AES-128-GCM security key]:key:' '--bind_ip=[Transport bind IP]:ip:' '--native[Restrict discovery to local host]' '(-h --help)'{-h,--help}'[Show help]'
      ;;
    validate)
      _arguments -s '*:TOML manifest:_files -g "*.toml"' '*--arg=[Override arg]:kv:' '--check_programs[Check programs]' '--json[JSON output]' '(-h --help)'{-h,--help}'[Show help]'
      ;;
    graph)
      _arguments -s '*:TOML manifest:_files -g "*.toml"' '--format=[Output format]:format:(ascii dot mermaid)' '(-o --out)'{-o,--out}'=[Output file]:file:_files' '*--arg=[Override arg]:kv:' '(-h --help)'{-h,--help}'[Show help]'
      ;;
    inspect)
      _arguments -s '*:TOML manifest:_files -g "*.toml"' '--show_args[Show args]' '--json[JSON output]' '*--arg=[Override arg]:kv:' '(-h --help)'{-h,--help}'[Show help]'
      ;;
    plugin)
      _arguments -s '*:Plugin shared library:_files -g "*.(so|dylib|dll)"' '--json[JSON output]' '(-h --help)'{-h,--help}'[Show help]'
      ;;
    init)
      _arguments -s '--template=[Template]:template:(minimal component systemd)' '(-o --out)'{-o,--out}'=[Output file]:file:_files' '(-h --help)'{-h,--help}'[Show help]'
      ;;
    logs)
      _arguments -s '*:Session log directory:_files -/' '(-u --unit)'{-u,--unit}'=[Unit name]:unit:' '--grep=[Substring filter]:pattern:' '(-n --lines)'{-n,--lines}'=[Tail N lines]:count:' '(-h --help)'{-h,--help}'[Show help]'
      ;;
  esac
}
if (( $+functions[compdef] )); then
  compdef _vlink-launcher vlink-launcher launcher
fi
)ZSH";
    return 0;
  }

  std::cerr << "Unsupported shell '" << shell << "' (expected bash or zsh).\n";
  return 1;
}

static void add_transport_args(argparse::ArgumentParser& parser, bool is_server) {
  parser.add_argument("--scheme")
      .default_value(std::string(""))
      .help("Control-plane scheme (dds|ddsc|ddsr|zenoh|intra)");
  parser.add_argument("--namespace")
      .default_value(is_server ? std::string("") : std::string("launcher"))
      .help("Control-plane topic namespace prefix");
  parser.add_argument("--domain")
      .default_value(is_server ? static_cast<int32_t>(-1) : static_cast<int32_t>(0))
      .scan<'i', int32_t>()
      .help("Control-plane domain ID");
  parser.add_argument("--key").default_value(std::string("")).help("AES-128-GCM security key");
  parser.add_argument("--bind_ip").default_value(std::string("")).help("Transport bind IP");
  parser.add_argument("--native").default_value(false).implicit_value(true).help("Restrict discovery to local host");
}

int main(int argc, char* argv[]) {
  vlink::Utils::set_console_utf8_output();

  argparse::ArgumentParser program("vlink-launcher", VLINK_VERSION);
  program.add_description(
      "VLink Launcher: ROS 2 + CyberRT + systemd-grade process supervisor, component container & DAG orchestrator");

  argparse::ArgumentParser run_cmd("run");
  run_cmd.add_description("Launch a VLink system from a TOML manifest");
  run_cmd.add_argument("config").help("Path to TOML launch file");
  run_cmd.add_argument("--arg").append().default_value(std::vector<std::string>{}).help("Override arg (k=v)");
  run_cmd.add_argument("--dry_run")
      .default_value(false)
      .implicit_value(true)
      .help("Print execution plan without running");
  run_cmd.add_argument("--show_args").default_value(false).implicit_value(true).help("List declared launch arguments");
  run_cmd.add_argument("--serial").default_value(false).implicit_value(true).help("Start units strictly one at a time");
  run_cmd.add_argument("--parallel")
      .default_value(static_cast<int32_t>(0))
      .scan<'i', int32_t>()
      .help("Max concurrent unit starts");
  run_cmd.add_argument("--only")
      .append()
      .default_value(std::vector<std::string>{})
      .help("Start only unit/@group + deps");
  run_cmd.add_argument("--skip").append().default_value(std::vector<std::string>{}).help("Skip unit or @group");
  run_cmd.add_argument("--debug_unit")
      .append()
      .default_value(std::vector<std::string>{})
      .help("Wrap unit with prefix (u=gdbserver :1234)");
  run_cmd.add_argument("--no_respawn")
      .default_value(false)
      .implicit_value(true)
      .help("Disable automatic respawn on crashes");
  run_cmd.add_argument("--pause_on_fail")
      .default_value(false)
      .implicit_value(true)
      .help("Pause startup queue after first failure");
  run_cmd.add_argument("--log_level").default_value(std::string("")).help("Override console log level");
  run_cmd.add_argument("--log_plugin")
      .default_value(std::string(""))
      .help("Override LoggerPluginInterface (logd) library name or path");
  run_cmd.add_argument("--no_control")
      .default_value(false)
      .implicit_value(true)
      .help("Disable control-plane endpoints");
  run_cmd.add_argument("--plain").default_value(false).implicit_value(true).help("Disable ANSI colors");
  run_cmd.add_argument("--log_dir").default_value(std::string("")).help("Override session log directory");
  run_cmd.add_argument("--record_dir")
      .default_value(std::string(""))
      .help("Enable session active recording into directory");
  add_transport_args(run_cmd, true);

  argparse::ArgumentParser container_cmd("_container");
  container_cmd.add_description("Internal isolated component container entry point");
  container_cmd.add_argument("--name").required().help("Container unit name");
  container_cmd.add_argument("--config").required().help("Generated container TOML manifest");
  container_cmd.add_argument("--no_control").default_value(false).implicit_value(true);
  container_cmd.add_argument("--plain").default_value(true).implicit_value(true);
  container_cmd.add_argument("--log_dir").default_value(std::string(""));
  add_transport_args(container_cmd, true);

  argparse::ArgumentParser validate_cmd("validate");
  validate_cmd.add_description("Validate TOML manifest, dependencies, conflicts, slices and DAG acyclicity");
  validate_cmd.add_argument("config").help("Path to TOML launch file");
  validate_cmd.add_argument("--arg").append().default_value(std::vector<std::string>{}).help("Override arg (k=v)");
  validate_cmd.add_argument("--check_programs")
      .default_value(false)
      .implicit_value(true)
      .help("Verify executables exist");
  validate_cmd.add_argument("--json").default_value(false).implicit_value(true).help(
      "Output validation report as JSON");

  argparse::ArgumentParser graph_cmd("graph");
  graph_cmd.add_description("Export resolved startup DAG in ASCII, Graphviz DOT or Mermaid format");
  graph_cmd.add_argument("config").help("Path to TOML launch file");
  graph_cmd.add_argument("--format").default_value(std::string("ascii")).help("Output format: ascii|dot|mermaid");
  graph_cmd.add_argument("--out", "-o").default_value(std::string("")).help("Output file path (default stdout)");
  graph_cmd.add_argument("--arg").append().default_value(std::vector<std::string>{}).help("Override arg (k=v)");

  argparse::ArgumentParser inspect_cmd("inspect");
  inspect_cmd.add_description("Inspect resolved manifest, arguments, groups and startup layers");
  inspect_cmd.add_argument("config").help("Path to TOML launch file");
  inspect_cmd.add_argument("--show_args").default_value(false).implicit_value(true).help("Show argument table");
  inspect_cmd.add_argument("--json").default_value(false).implicit_value(true).help("Emit full resolved JSON");
  inspect_cmd.add_argument("--arg").append().default_value(std::vector<std::string>{}).help("Override arg (k=v)");

  argparse::ArgumentParser plugin_cmd("plugin");
  plugin_cmd.add_description("Inspect a VLink shared library for exported component or logger plugin interfaces");
  plugin_cmd.add_argument("library").help("Shared library stem or path");
  plugin_cmd.add_argument("--json").default_value(false).implicit_value(true).help("Emit JSON metadata");

  argparse::ArgumentParser init_cmd("init");
  init_cmd.add_description("Generate a starter TOML launch manifest");
  init_cmd.add_argument("--template").default_value(std::string("minimal")).help("Template: minimal|component|systemd");
  init_cmd.add_argument("--out", "-o").default_value(std::string("")).help("Write to file (default stdout)");

  argparse::ArgumentParser top_cmd("top");
  top_cmd.add_description("Interactive real-time TUI dashboard for a running LauncherServer");
  add_transport_args(top_cmd, false);

  argparse::ArgumentParser ctl_cmd("ctl");
  ctl_cmd.add_description("Inspect and control a running LauncherServer (systemctl/journalctl/systemd-analyze style)");
  ctl_cmd.add_argument("action").help(
      "status|list-units|list-dependencies|blame|critical-chain|cat|start|stop|restart|reload|activate|deactivate|"
      "reset-failed|isolate|resume|load|unload|params|set-param|record|events|logs|shutdown");
  ctl_cmd.add_argument("target")
      .nargs(argparse::nargs_pattern::any)
      .default_value(std::vector<std::string>{})
      .help("Optional unit/@group or key=value");
  ctl_cmd.add_argument("--unit", "-u").default_value(std::string("")).help("Target unit or @group");
  ctl_cmd.add_argument("--type").default_value(std::string("")).help("Filter by unit type");
  ctl_cmd.add_argument("--state").default_value(std::string("")).help("Filter by unit state");
  ctl_cmd.add_argument("--group").default_value(std::string("")).help("Filter by task/scheduler group");
  ctl_cmd.add_argument("--failed").default_value(false).implicit_value(true).help("Show failed units only");
  ctl_cmd.add_argument("--reverse").default_value(false).implicit_value(true).help("Show reverse dependencies");
  ctl_cmd.add_argument("--watch", "-w").default_value(false).implicit_value(true).help("Continuously refresh status");
  ctl_cmd.add_argument("--json").default_value(false).implicit_value(true).help("JSON output");
  ctl_cmd.add_argument("--lines", "-n")
      .default_value(static_cast<int32_t>(-1))
      .scan<'i', int32_t>()
      .help("Tail N log records");
  ctl_cmd.add_argument("--follow", "-f").default_value(false).implicit_value(true).help("Follow live log stream");
  ctl_cmd.add_argument("--level")
      .default_value(std::string(""))
      .help("Min log level (trace|debug|info|warn|error|fatal)");
  ctl_cmd.add_argument("--grep").default_value(std::string("")).help("Substring filter for logs");
  ctl_cmd.add_argument("--stderr").default_value(false).implicit_value(true).help("Show stderr logs only");
  ctl_cmd.add_argument("--lib").default_value(std::string("")).help("Shared library for load");
  ctl_cmd.add_argument("--class").default_value(std::string("")).help("Component class name for load");
  ctl_cmd.add_argument("--scheduler").default_value(std::string("")).help("Scheduler group for load");
  ctl_cmd.add_argument("--compute_pool").default_value(std::string("")).help("Compute ThreadPool group for load");
  ctl_cmd.add_argument("--container").default_value(std::string("")).help("Target container for load");
  ctl_cmd.add_argument("--worker")
      .default_value(static_cast<int32_t>(-1))
      .scan<'i', int32_t>()
      .help("Pinned worker index");
  ctl_cmd.add_argument("--priority")
      .default_value(static_cast<int32_t>(0))
      .scan<'i', int32_t>()
      .help("Default task priority");
  ctl_cmd.add_argument("--legacy").default_value(false).implicit_value(true).help("Load legacy RunablePluginInterface");
  ctl_cmd.add_argument("--param").append().default_value(std::vector<std::string>{}).help("Initial param k=v for load");
  ctl_cmd.add_argument("--url").append().default_value(std::vector<std::string>{}).help("Named URL k=url for load");
  ctl_cmd.add_argument("--input").append().default_value(std::vector<std::string>{}).help("Input port k=url for load");
  ctl_cmd.add_argument("--output")
      .append()
      .default_value(std::vector<std::string>{})
      .help("Output port k=url for load");
  ctl_cmd.add_argument("--param_key").default_value(std::string("")).help("Parameter key for set-param");
  ctl_cmd.add_argument("--param_val").default_value(std::string("")).help("Parameter value for set-param");
  ctl_cmd.add_argument("--param_type")
      .default_value(std::string("auto"))
      .help("Param type (auto|bool|int|double|string|*_list)");
  ctl_cmd.add_argument("--path").default_value(std::string("")).help("Bag output path for ctl record");
  ctl_cmd.add_argument("--stop").default_value(false).implicit_value(true).help("Stop active recording for ctl record");
  ctl_cmd.add_argument("--timeout")
      .default_value(static_cast<int32_t>(5000))
      .scan<'i', int32_t>()
      .help("RPC timeout ms");
  add_transport_args(ctl_cmd, false);

  argparse::ArgumentParser logs_cmd("logs");
  logs_cmd.add_description("Inspect persisted .log files from a launcher session directory offline");
  logs_cmd.add_argument("dir").help("Path to launcher log session directory");
  logs_cmd.add_argument("--unit", "-u").default_value(std::string("")).help("Filter by unit filename stem");
  logs_cmd.add_argument("--grep").default_value(std::string("")).help("Substring filter");
  logs_cmd.add_argument("--lines", "-n")
      .default_value(static_cast<int32_t>(200))
      .scan<'i', int32_t>()
      .help("Tail N matching lines");

  argparse::ArgumentParser completion_cmd("completion");
  completion_cmd.add_description("Print shell completion script");
  completion_cmd.add_argument("shell").help("bash or zsh");

  program.add_subparser(run_cmd);
  program.add_subparser(container_cmd);
  program.add_subparser(validate_cmd);
  program.add_subparser(graph_cmd);
  program.add_subparser(inspect_cmd);
  program.add_subparser(plugin_cmd);
  program.add_subparser(init_cmd);
  program.add_subparser(top_cmd);
  program.add_subparser(ctl_cmd);
  program.add_subparser(logs_cmd);
  program.add_subparser(completion_cmd);

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception& err) {
    std::cerr << err.what() << "\n" << program;
    return 1;
  }

  try {
    if (program.is_subcommand_used(run_cmd)) {
      return run_server_command(run_cmd, false);
    }

    if (program.is_subcommand_used(container_cmd)) {
      return run_server_command(container_cmd, true);
    }

    vlink::Logger::set_console_level(vlink::Logger::kOff);
    vlink::Logger::set_file_level(vlink::Logger::kOff);

    if (program.is_subcommand_used(validate_cmd)) {
      return run_validate_command(validate_cmd);
    }

    if (program.is_subcommand_used(graph_cmd)) {
      return run_graph_command(graph_cmd);
    }

    if (program.is_subcommand_used(inspect_cmd)) {
      return run_inspect_command(inspect_cmd);
    }

    if (program.is_subcommand_used(plugin_cmd)) {
      return run_plugin_command(plugin_cmd);
    }

    if (program.is_subcommand_used(init_cmd)) {
      return run_init_command(init_cmd);
    }

    if (program.is_subcommand_used(top_cmd)) {
      return run_top_command(top_cmd);
    }

    if (program.is_subcommand_used(ctl_cmd)) {
      return run_ctl_command(ctl_cmd);
    }

    if (program.is_subcommand_used(logs_cmd)) {
      return run_offline_logs_command(logs_cmd);
    }

    if (program.is_subcommand_used(completion_cmd)) {
      return run_completion_command(completion_cmd);
    }

    std::cout << program;
    return 0;
  } catch (const std::exception& err) {
    std::cerr << "Error: " << err.what() << "\n";
    return 1;
  }
}
