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

#include "./check_common.h"

enum class DiagType : uint8_t {
  kPass = 0,
  kWarning = 1,
  kFailed = 2,
};

[[maybe_unused]] static std::string run_cmd_output(const std::string& cmd) {
  std::array<char, 256> buffer;
  std::string result;

#ifdef _WIN32
  // NOLINTNEXTLINE(bugprone-command-processor)
  FILE* pipe = ::_popen(cmd.c_str(), "r");
#else
  // NOLINTNEXTLINE(bugprone-command-processor)
  FILE* pipe = ::popen(cmd.c_str(), "r");
#endif

  if VUNLIKELY (!pipe) {
    return result;
  }

  while (::fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
    result += buffer.data();
  }

#ifdef _WIN32
  ::_pclose(pipe);
#else
  ::pclose(pipe);
#endif

  return result;
}

[[maybe_unused]] static std::string get_route_table_output() {
#ifdef _WIN32
  return run_cmd_output("route print");
#elif defined(__linux__) || defined(__ANDROID__)
  auto out = run_cmd_output("ip route");

  if VUNLIKELY (out.empty()) {
    out = run_cmd_output("route -n");
  }

  return out;
#elif defined(__APPLE__)
  return run_cmd_output("netstat -rn");
#elif defined(__QNX__)
  return run_cmd_output("netstat -rn");
#else
  return run_cmd_output("route -n");
#endif
}

struct DiagContext final {
  vlink::MessageLoop* loop{nullptr};
  std::atomic<int> passed_count{0};
  std::atomic<int> warning_count{0};
  std::atomic<int> failed_count{0};
  std::string title;
  std::string filter;
};

static bool diag_accepted(const DiagContext& ctx, const std::string& title) {
  if (ctx.loop != nullptr && ctx.loop->is_ready_to_quit()) {
    return false;
  }

  if VLIKELY (ctx.filter.empty()) {
    return true;
  }

  auto tolower_copy = [](std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
  };

  return tolower_copy(title).find(tolower_copy(ctx.filter)) != std::string::npos;
}

static void begin_diag(DiagContext& ctx, const std::string& title, int delay_ms) {
  ctx.title = title;
  std::cout << title << std::string(kTitleWidth - title.size(), ' ');
  std::cout << "......";
  std::cout.flush();

  ctx.loop->wait_for_quit(delay_ms);
}

static void end_diag(DiagContext& ctx, DiagType type, const std::string& detail) {
  std::cout << "\033[2K\r";

  const int detail_len = static_cast<int>(detail.size());

  switch (type) {
    case DiagType::kPass:
      std::cout << kColorPass;
      std::cout << ctx.title << std::string(kTitleWidth - ctx.title.size(), ' ') << "PASSED";
      std::cout << std::string(std::max(kStatusPassPad - detail_len, 2), ' ');
      std::cout << detail << kColorReset << std::endl;
      break;

    case DiagType::kWarning:
      std::cout << kColorWarn;
      std::cout << ctx.title << std::string(kTitleWidth - ctx.title.size(), ' ') << "WARNING";
      std::cout << std::string(std::max(kStatusWarnPad - detail_len, 2), ' ');
      std::cout << detail << kColorReset << std::endl;
      break;

    case DiagType::kFailed:
      std::cout << kColorFail;
      std::cout << ctx.title << std::string(kTitleWidth - ctx.title.size(), ' ') << "FAILED";
      std::cout << std::string(std::max(kStatusFailPad - detail_len, 2), ' ');
      std::cout << detail << kColorReset << std::endl;
      break;

    default:
      break;
  }

  switch (type) {
    case DiagType::kPass:
      ++ctx.passed_count;
      break;
    case DiagType::kWarning:
      ++ctx.warning_count;
      break;
    case DiagType::kFailed:
      ++ctx.failed_count;
      break;
    default:
      break;
  }
}

static void run_check(DiagContext& ctx, const std::string& title, int delay_ms,
                      const vlink::Function<void()>& check_fn) {
  if VUNLIKELY (!diag_accepted(ctx, title)) {
    return;
  }

  begin_diag(ctx, title, delay_ms);
  check_fn();
}

void check_ipv4_addresses(DiagContext& ctx) {
  auto ipv4_address = vlink::Utils::get_all_ipv4_address(false);

  if VUNLIKELY (ipv4_address.empty()) {
    end_diag(ctx, DiagType::kFailed, "Empty IP address");
    return;
  }

  if VUNLIKELY (ipv4_address.size() == 1 && ipv4_address.at(0) == "127.0.0.1") {
    end_diag(ctx, DiagType::kFailed, "Only find lo");
    return;
  }

  end_diag(ctx, DiagType::kPass, "Found " + std::to_string(ipv4_address.size()) + " IP Address");
}

void check_hostname(DiagContext& ctx) {
  const auto host = vlink::Utils::get_host_name();

  if VUNLIKELY (host.empty()) {
    end_diag(ctx, DiagType::kWarning, "Hostname is empty");
    return;
  }

  end_diag(ctx, DiagType::kPass, host);
}

void check_machine_id(DiagContext& ctx) {
  const auto id = vlink::Utils::get_machine_id();

  if VUNLIKELY (id.empty()) {
    end_diag(ctx, DiagType::kWarning, "Machine id is empty");
    return;
  }

  end_diag(ctx, DiagType::kPass, id);
}

void check_dds_ip(DiagContext& ctx) {
  const auto dds_ip = vlink::Utils::get_env("VLINK_DDS_IP");

  if (dds_ip.empty()) {
    end_diag(ctx, DiagType::kWarning, "VLINK_DDS_IP is empty");
    return;
  }

  const auto dds_ip_list = vlink::Helpers::split_any_view(dds_ip);
  const auto ipv4_address = vlink::Utils::get_all_ipv4_address(false);

  const bool available = std::any_of(ipv4_address.begin(), ipv4_address.end(), [&dds_ip_list](const std::string& ip) {
    return std::any_of(dds_ip_list.begin(), dds_ip_list.end(),
                       [&ip](std::string_view entry) { return entry == std::string_view(ip.data(), ip.size()); });
  });

  if VLIKELY (available) {
    end_diag(ctx, DiagType::kPass, dds_ip + " is valid");
  } else {
    end_diag(ctx, DiagType::kFailed, dds_ip + " is invalid");
  }
}

void check_dds_interface(DiagContext& ctx) {
  const auto dds_ip = vlink::Utils::get_env("VLINK_DDS_IP");

  if (dds_ip.empty()) {
    end_diag(ctx, DiagType::kWarning, "VLINK_DDS_IP is empty");
    return;
  }

  const auto split = vlink::Helpers::split_any_view(dds_ip);

  if VUNLIKELY (split.empty()) {
    end_diag(ctx, DiagType::kFailed, "VLINK_DDS_IP=" + dds_ip + " has no IP entries");
    return;
  }

  const auto first_ip_view = split.front();
  const std::string first_ip(first_ip_view);
  const auto iface = vlink::Utils::get_interface_name_by_ipv4(first_ip);

  if VUNLIKELY (iface.empty()) {
    end_diag(ctx, DiagType::kFailed, "No interface for " + first_ip);
    return;
  }

  end_diag(ctx, DiagType::kPass, first_ip + " on " + iface);
}

void check_multicast_address(DiagContext& ctx, const int (&octets)[4], bool warn_on_missing) {
  const std::string needle = std::to_string(octets[0]) + '.' + std::to_string(octets[1]) + '.' +
                             std::to_string(octets[2]) + '.' + std::to_string(octets[3]);

  const auto result = get_route_table_output();

  if VUNLIKELY (result.empty()) {
    end_diag(ctx, DiagType::kFailed, "Cannot read route table");
    return;
  }

  auto is_address_character = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '.'; };

  size_t scan_pos = 0;

  while ((scan_pos = result.find(needle, scan_pos)) != std::string::npos) {
    const size_t match_end = scan_pos + needle.size();
    const bool valid_left = scan_pos == 0 || !is_address_character(result[scan_pos - 1]);
    const bool valid_right = match_end >= result.size() || !is_address_character(result[match_end]);

    if (valid_left && valid_right) {
      end_diag(ctx, DiagType::kPass, "Found " + needle);
      return;
    }

    scan_pos = match_end;
  }

  if (warn_on_missing) {
    end_diag(ctx, DiagType::kWarning, "Cannot find " + needle);
  } else {
    end_diag(ctx, DiagType::kFailed, "Cannot find " + needle);
  }
}

void check_log_dir_space(DiagContext& ctx) {
  std::string log_dir = vlink::Utils::get_env("VLINK_LOG_DIR");

  if (log_dir.empty()) {
    log_dir = vlink::Utils::get_tmp_dir();
  }

  try {
    auto space_info = std::filesystem::space(log_dir);

    if VLIKELY (space_info.available >= 1024ULL * 1024ULL * 1024ULL) {
      end_diag(
          ctx, DiagType::kPass,
          "Available: " + vlink::Helpers::double_to_string(space_info.available / 1024.0 / 1024.0 / 1024.0, 2) + "GB");
      return;
    }

    if VLIKELY (space_info.available >= 1024ULL * 1024ULL) {
      end_diag(ctx, DiagType::kPass,
               "Available: " + vlink::Helpers::double_to_string(space_info.available / 1024.0 / 1024.0, 2) + "MB");
      return;
    }

    end_diag(ctx, DiagType::kFailed,
             "Available: " + vlink::Helpers::double_to_string(space_info.available / 1024.0 / 1024.0, 2) + "MB");
  } catch (const std::filesystem::filesystem_error& e) {
    end_diag(ctx, DiagType::kFailed, e.what());
  }
}

void check_log_dir_writable(DiagContext& ctx) {
  std::string log_dir = vlink::Utils::get_env("VLINK_LOG_DIR");

  if (log_dir.empty()) {
    log_dir = vlink::Utils::get_tmp_dir();
  }

  const auto probe = std::filesystem::path(log_dir) / (".check_" + vlink::Utils::get_pid_str());

  std::ofstream ofs(probe, std::ios::out | std::ios::trunc);

  if VUNLIKELY (!ofs.is_open()) {
    end_diag(ctx, DiagType::kFailed, "Cannot write to " + log_dir);
    return;
  }

  ofs << "check" << std::endl;
  ofs.close();

  std::error_code ec;
  std::filesystem::remove(probe, ec);

  end_diag(ctx, DiagType::kPass, log_dir);
}

void check_directory_env(DiagContext& ctx, const std::string& env_key, bool warn_if_missing) {
  const auto dir = vlink::Utils::get_env(env_key);

  if (dir.empty()) {
    end_diag(ctx, warn_if_missing ? DiagType::kWarning : DiagType::kPass, env_key + " is empty");
    return;
  }

  std::error_code ec;

  if VUNLIKELY (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
    end_diag(ctx, DiagType::kFailed, dir + " is not a directory");
    return;
  }

  end_diag(ctx, DiagType::kPass, dir);
}

void check_file_env(DiagContext& ctx, const std::string& env_key, bool warn_if_missing) {
  const auto file = vlink::Utils::get_env(env_key);

  if (file.empty()) {
    end_diag(ctx, warn_if_missing ? DiagType::kWarning : DiagType::kPass, env_key + " is empty");
    return;
  }

  std::error_code ec;

  if VUNLIKELY (!std::filesystem::exists(file, ec) || !std::filesystem::is_regular_file(file, ec)) {
    end_diag(ctx, DiagType::kFailed, file + " is not a file");
    return;
  }

  end_diag(ctx, DiagType::kPass, file);
}

double sample_avg(const vlink::Function<double()>& fn, vlink::MessageLoop* loop, int times, int interval_ms) {
  double total = 0;

  for (int i = 0; i < times; ++i) {
    total += fn();

    if (i + 1 < times) {
      loop->wait_for_quit(interval_ms);
    }
  }

  return total / static_cast<double>(times);
}

void check_cpu_usage(DiagContext& ctx) {
  const double samples = sample_avg(&vlink::Utils::get_cpu_usage, ctx.loop, 4, 300);

  if VUNLIKELY (std::isnan(samples)) {
    end_diag(ctx, DiagType::kFailed, "Get failed");
    return;
  }

  if VUNLIKELY (samples < 0) {
    end_diag(ctx, DiagType::kFailed, "Not supported");
    return;
  }

  const auto detail = "Usage " + vlink::Helpers::double_to_string(samples, 2) + "%";

  if VUNLIKELY (samples > 90) {
    end_diag(ctx, DiagType::kFailed, detail);
  } else if (samples > 50) {
    end_diag(ctx, DiagType::kWarning, detail);
  } else {
    end_diag(ctx, DiagType::kPass, detail);
  }
}

void check_memory_usage(DiagContext& ctx) {
  const double samples = sample_avg(&vlink::Utils::get_memory_usage, ctx.loop, 4, 300);

  if VUNLIKELY (std::isnan(samples)) {
    end_diag(ctx, DiagType::kFailed, "Get failed");
    return;
  }

  if VUNLIKELY (samples < 0) {
    end_diag(ctx, DiagType::kFailed, "Not supported");
    return;
  }

  const auto detail = "Usage " + vlink::Helpers::double_to_string(samples, 2) + "%";

  if VUNLIKELY (samples > 90) {
    end_diag(ctx, DiagType::kFailed, detail);
  } else if (samples > 50) {
    end_diag(ctx, DiagType::kWarning, detail);
  } else {
    end_diag(ctx, DiagType::kPass, detail);
  }
}

struct ProcessCheck final {
  std::string title;
  std::string linux_primary;
  std::string linux_fallback;
  std::string windows_name;
  bool required{false};
  std::string label;
};

bool is_process_variant_running(const ProcessCheck& pc) {
#ifdef _WIN32
  return vlink::Utils::is_process_running(pc.windows_name);
#else
  return vlink::Utils::is_process_running(pc.linux_primary) ||
         (!pc.linux_fallback.empty() && vlink::Utils::is_process_running(pc.linux_fallback));
#endif
}

void check_process(DiagContext& ctx, const ProcessCheck& pc) {
  const bool running = is_process_variant_running(pc);

  if (pc.required) {
    if (running) {
      end_diag(ctx, DiagType::kPass, pc.label + " is running");
    } else {
      end_diag(ctx, DiagType::kFailed, pc.label + " is not running");
    }
    return;
  }

  if (running) {
    end_diag(ctx, DiagType::kWarning, pc.label + " is running");
  } else {
    end_diag(ctx, DiagType::kPass, pc.label + " is not running");
  }
}

void check_others_running(DiagContext& ctx) {
#ifdef _WIN32
  const std::string command_str = "\"" + vlink::Utils::get_app_dir() + "/vlink-list.exe\" -nc";

  int exit_code = _wsystem(vlink::Helpers::string_to_wstring(command_str).c_str());

  if VUNLIKELY (exit_code < 0 || exit_code > 250) {
    end_diag(ctx, DiagType::kFailed, "List running failed");
  } else if (exit_code == 0) {
    end_diag(ctx, DiagType::kPass, "No vlink user process running");
  } else {
    end_diag(ctx, DiagType::kWarning, std::to_string(exit_code) + " vlink user processes exist");
  }
#else
  const std::string command_str = "\"" + vlink::Utils::get_app_dir() + "/vlink-list\" -nc";

  // NOLINTNEXTLINE(bugprone-command-processor)
  int status = std::system(command_str.c_str());

  if VUNLIKELY (status < 0) {
    end_diag(ctx, DiagType::kFailed, "List running failed");
    return;
  }

  if VLIKELY (WIFEXITED(status)) {
    const int exit_code = WEXITSTATUS(status);

    if VUNLIKELY (exit_code < 0 || exit_code > 250) {
      end_diag(ctx, DiagType::kFailed, "List running failed");
    } else if (exit_code == 0) {
      end_diag(ctx, DiagType::kPass, "No vlink user process running");
    } else {
      end_diag(ctx, DiagType::kWarning, std::to_string(exit_code) + " vlink user processes exist");
    }

    return;
  }

  end_diag(ctx, DiagType::kFailed, "List running failed");
#endif
}

void check_singleton_conflict(DiagContext& ctx) {
  const bool ok = vlink::Utils::check_singleton("check-diag-probe");

  if VLIKELY (ok) {
    end_diag(ctx, DiagType::kPass, "No singleton conflict");
  } else {
    end_diag(ctx, DiagType::kWarning, "Lock dir may be contended");
  }
}

void check_cpu_cores(DiagContext& ctx) {
  const unsigned cores = std::thread::hardware_concurrency();

  if VUNLIKELY (cores == 0) {
    end_diag(ctx, DiagType::kWarning, "Unknown core count");
    return;
  }

  end_diag(ctx, DiagType::kPass, std::to_string(cores) + " cores");
}

void check_flag(DiagContext& ctx, const std::string& title, const std::string& name, bool is_on) {
  if VUNLIKELY (!diag_accepted(ctx, title)) {
    return;
  }

  begin_diag(ctx, title, 50);

  if (is_on) {
    end_diag(ctx, DiagType::kPass, name + " is ON");
  } else {
    end_diag(ctx, DiagType::kWarning, name + " is OFF");
  }
}

[[maybe_unused]] std::string read_first_line(const std::string& path) {
  std::ifstream ifs(path);

  if VUNLIKELY (!ifs.is_open()) {
    return {};
  }

  std::string line;

  while (std::getline(ifs, line)) {
    if VLIKELY (!line.empty()) {
      return vlink::Helpers::trim_string(line);
    }
  }

  return {};
}

void check_vlink_version(DiagContext& ctx) { end_diag(ctx, DiagType::kPass, VLINK_VERSION); }

void check_platform_info(DiagContext& ctx) {
#if defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__) || defined(__QNX__)
  const auto kernel = vlink::Helpers::trim_string(run_cmd_output("uname -sr"));

  if VUNLIKELY (kernel.empty()) {
    end_diag(ctx, DiagType::kWarning, "uname failed");
    return;
  }

  end_diag(ctx, DiagType::kPass, kernel);
#elif defined(_WIN32)
  end_diag(ctx, DiagType::kPass, "Windows");
#else
  end_diag(ctx, DiagType::kWarning, "Unknown platform");
#endif
}

void check_shm_space(DiagContext& ctx) {
#if defined(__linux__) || defined(__ANDROID__)
  const char* shm_path = "/dev/shm";

  try {
    auto info = std::filesystem::space(shm_path);

    if VUNLIKELY (info.available < 64ULL * 1024ULL * 1024ULL) {
      end_diag(ctx, DiagType::kFailed,
               "Only " + vlink::Helpers::double_to_string(info.available / 1024.0 / 1024.0, 2) + "MB free");
      return;
    }

    if VUNLIKELY (info.available < 512ULL * 1024ULL * 1024ULL) {
      end_diag(ctx, DiagType::kWarning,
               vlink::Helpers::double_to_string(info.available / 1024.0 / 1024.0, 2) + "MB free (< 512MB)");
      return;
    }

    end_diag(ctx, DiagType::kPass,
             vlink::Helpers::double_to_string(info.available / 1024.0 / 1024.0 / 1024.0, 2) + "GB free");
  } catch (const std::filesystem::filesystem_error& e) {
    end_diag(ctx, DiagType::kFailed, e.what());
  }
#else
  end_diag(ctx, DiagType::kPass, "Not applicable on this platform");
#endif
}

void check_sysctl_buffer(DiagContext& ctx, const std::string& path, int64_t warn_below) {
#if defined(__linux__) || defined(__ANDROID__)
  const auto line = read_first_line(path);

  if VUNLIKELY (line.empty()) {
    end_diag(ctx, DiagType::kWarning, "Cannot read " + path);
    return;
  }

  const int64_t value = vlink::Helpers::to_long(line);
  const std::string human = vlink::Helpers::format_file_size(static_cast<size_t>(value));

  if VUNLIKELY (value < warn_below) {
    end_diag(ctx, DiagType::kWarning, human + " (< " + vlink::Helpers::format_file_size(warn_below) + ")");
    return;
  }

  end_diag(ctx, DiagType::kPass, human);
#else
  (void)path;
  (void)warn_below;
  end_diag(ctx, DiagType::kPass, "Not applicable on this platform");
#endif
}

void check_rp_filter(DiagContext& ctx) {
#if defined(__linux__) || defined(__ANDROID__)
  const auto value = read_first_line("/proc/sys/net/ipv4/conf/all/rp_filter");

  if (value.empty()) {
    end_diag(ctx, DiagType::kWarning, "Cannot read rp_filter");
    return;
  }

  if (value == "1") {
    end_diag(ctx, DiagType::kWarning, "rp_filter=1 (strict) may drop multicast");
    return;
  }

  end_diag(ctx, DiagType::kPass, "rp_filter=" + value);
#else
  end_diag(ctx, DiagType::kPass, "Not applicable on this platform");
#endif
}

[[maybe_unused]] void check_rlimit(DiagContext& ctx, int resource_id, const std::string& label, uint64_t warn_below) {
#ifndef _WIN32
  struct rlimit rl{};

  if VUNLIKELY (::getrlimit(resource_id, &rl) != 0) {
    end_diag(ctx, DiagType::kWarning, "getrlimit(" + label + ") failed");
    return;
  }

  if (rl.rlim_cur == RLIM_INFINITY) {
    end_diag(ctx, DiagType::kPass, label + "=unlimited");
    return;
  }

  const std::string detail = label + "=" + std::to_string(rl.rlim_cur);

  if VUNLIKELY (rl.rlim_cur < warn_below) {
    end_diag(ctx, DiagType::kWarning, detail + " (< " + std::to_string(warn_below) + ")");
    return;
  }

  end_diag(ctx, DiagType::kPass, detail);
#else
  (void)resource_id;
  (void)warn_below;
  end_diag(ctx, DiagType::kPass, label + "=N/A on Windows");
#endif
}

void check_time_sync(DiagContext& ctx) {
#if defined(__linux__) || defined(__ANDROID__)

  const bool ok = vlink::Utils::is_process_running("chronyd") || vlink::Utils::is_process_running("ntpd") ||
                  vlink::Utils::is_process_running("systemd-timesyncd");

  if VLIKELY (ok) {
    end_diag(ctx, DiagType::kPass, "Time sync daemon running");
    return;
  }

  end_diag(ctx, DiagType::kWarning, "No chrony/ntpd/timesyncd running");
#else
  end_diag(ctx, DiagType::kPass, "Not applicable on this platform");
#endif
}

void check_dds_domain_range(DiagContext& ctx) {
  const auto value = vlink::Utils::get_env("VLINK_DDS_DOMAIN");

  if (value.empty()) {
    end_diag(ctx, DiagType::kPass, "VLINK_DDS_DOMAIN unset (default)");
    return;
  }

  const int domain = vlink::Helpers::to_int(value, -1);

  if VUNLIKELY (domain < 0 || domain > 232) {
    end_diag(ctx, DiagType::kFailed, "VLINK_DDS_DOMAIN=" + value + " out of [0,232]");
    return;
  }

  end_diag(ctx, DiagType::kPass, "VLINK_DDS_DOMAIN=" + value);
}

void check_log_level_range(DiagContext& ctx) {
  const auto value = vlink::Utils::get_env("VLINK_LOG_LEVEL");

  if (value.empty()) {
    end_diag(ctx, DiagType::kPass, "VLINK_LOG_LEVEL unset (default)");
    return;
  }

  static constexpr std::array<std::string_view, 21> kNamedLevels = {
      "Trace", "TRACE", "trace", "Debug", "DEBUG", "debug", "Info",  "INFO", "info", "Warn", "WARN",
      "warn",  "Error", "ERROR", "error", "Fatal", "FATAL", "fatal", "Off",  "OFF",  "off",
  };

  if (std::find(kNamedLevels.begin(), kNamedLevels.end(), value) != kNamedLevels.end()) {
    end_diag(ctx, DiagType::kPass, "VLINK_LOG_LEVEL=" + value);
    return;
  }

  const int level = vlink::Helpers::to_int(value, -1);

  if VUNLIKELY (level < 0 || level > 6) {
    end_diag(ctx, DiagType::kFailed, "VLINK_LOG_LEVEL=" + value + " out of [0,6]");
    return;
  }

  end_diag(ctx, DiagType::kPass, "VLINK_LOG_LEVEL=" + value);
}

void check_roudi_running(DiagContext& ctx) {
#ifdef VLINK_SUPPORT_SHM

  if (vlink::ShmConf::has_roudi_running()) {
    end_diag(ctx, DiagType::kPass, "RouDi management segment detected");
    return;
  }

  end_diag(ctx, DiagType::kWarning, "iox-roudi not running (shm:// needs it)");
#else
  end_diag(ctx, DiagType::kPass, "shm module not compiled in");
#endif
}

#ifdef VLINK_SUPPORT_FDBUS
void check_name_server_running(DiagContext& ctx) {
  if (vlink::FdbusConf::has_name_server()) {
    end_diag(ctx, DiagType::kPass, "FDBus name_server detected");
    return;
  }

  end_diag(ctx, DiagType::kWarning, "name_server not running (fdbus:// needs it)");
}
#endif

#ifdef VLINK_SUPPORT_MQTT
void check_mqtt_broker(DiagContext& ctx) {
  const auto broker = vlink::Utils::get_env("VLINK_MQTT_BROKER");

  if (!broker.empty()) {
    end_diag(ctx, DiagType::kPass, "VLINK_MQTT_BROKER=" + broker);
    return;
  }

  end_diag(ctx, DiagType::kWarning, "VLINK_MQTT_BROKER not set (mqtt:// needs it)");
}
#endif

void check_interface_mtu(DiagContext& ctx) {
#if defined(__linux__) || defined(__ANDROID__)
  const auto dds_ip = vlink::Utils::get_env("VLINK_DDS_IP");

  std::string ip;

  if (dds_ip.empty()) {
    const auto addrs = vlink::Utils::get_all_ipv4_address(true);

    for (const auto& addr : addrs) {
      if (addr != "127.0.0.1") {
        ip = addr;
        break;
      }
    }
  } else {
    const auto split = vlink::Helpers::split_any_view(dds_ip);

    if (!split.empty()) {
      ip = std::string(split.front());
    }
  }

  if VUNLIKELY (ip.empty()) {
    end_diag(ctx, DiagType::kWarning, "No non-loopback IP");
    return;
  }

  const auto iface = vlink::Utils::get_interface_name_by_ipv4(ip);

  if VUNLIKELY (iface.empty()) {
    end_diag(ctx, DiagType::kWarning, "No interface for " + ip);
    return;
  }

  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);

  if VUNLIKELY (fd < 0) {
    end_diag(ctx, DiagType::kWarning, "socket() failed");
    return;
  }

  struct ifreq ifr{};
  std::snprintf(ifr.ifr_name, IFNAMSIZ, "%s", iface.c_str());

  if VUNLIKELY (::ioctl(fd, SIOCGIFMTU, &ifr) < 0) {
    ::close(fd);
    end_diag(ctx, DiagType::kWarning, "ioctl(SIOCGIFMTU) failed on " + iface);
    return;
  }

  const int mtu = ifr.ifr_mtu;
  ::close(fd);

  const std::string detail = iface + " mtu=" + std::to_string(mtu);

  if VUNLIKELY (mtu < 1280) {
    end_diag(ctx, DiagType::kFailed, detail + " (< 1280)");
    return;
  }

  if VUNLIKELY (mtu < 1500) {
    end_diag(ctx, DiagType::kWarning, detail + " (< 1500)");
    return;
  }

  end_diag(ctx, DiagType::kPass, detail);
#else
  end_diag(ctx, DiagType::kPass, "Not applicable on this platform");
#endif
}

int check_diag(bool all_case, bool show_summary, const std::string& filter) {
  vlink::MessageLoop message_loop;

  DiagContext ctx;
  ctx.loop = &message_loop;
  ctx.filter = filter;

  vlink::Utils::register_terminate_signal([&message_loop](int) { message_loop.quit(); });

  message_loop.async_run();

  std::cout << kColorHeader;
  std::cout << "[TITLE]" << std::string(kTitleWidth - 8, ' ') << "[STATUS]" << std::string(41, ' ') << "[DETAIL]";
  std::cout << kColorReset << std::endl;

  run_check(ctx, "* Check vlink version...", 50, [&ctx]() { check_vlink_version(ctx); });
  run_check(ctx, "* Check platform info...", 50, [&ctx]() { check_platform_info(ctx); });
  run_check(ctx, "* Check hostname...", 50, [&ctx]() { check_hostname(ctx); });
  run_check(ctx, "* Check machine id...", 50, [&ctx]() { check_machine_id(ctx); });
  run_check(ctx, "* Check cpu cores...", 50, [&ctx]() { check_cpu_cores(ctx); });

  run_check(ctx, "* Check available IP addresses...", 100, [&ctx]() { check_ipv4_addresses(ctx); });

#if defined(VLINK_SUPPORT_DDS) || defined(VLINK_SUPPORT_DDSC) || defined(VLINK_SUPPORT_DDSR)
  run_check(ctx, "* Check VLink DDS IP available...", 100, [&ctx]() { check_dds_ip(ctx); });
  run_check(ctx, "* Check VLink DDS interface...", 100, [&ctx]() { check_dds_interface(ctx); });
  run_check(ctx, "* Check VLink DDS interface MTU...", 100, [&ctx]() { check_interface_mtu(ctx); });
#endif

  run_check(ctx, "* Check VLink multicast address...", 100,
            [&ctx]() { check_multicast_address(ctx, kMulticastDiscovery, false); });

#if defined(VLINK_SUPPORT_DDS) || defined(VLINK_SUPPORT_DDSC) || defined(VLINK_SUPPORT_DDSR)
  run_check(ctx, "* Check DDS multicast address...", 100,
            [&ctx]() { check_multicast_address(ctx, kMulticastDds, true); });
  run_check(ctx, "* Check VLINK_DDS_DOMAIN range...", 50, [&ctx]() { check_dds_domain_range(ctx); });
#endif

  run_check(ctx, "* Check net.core.rmem_max...", 50,
            [&ctx]() { check_sysctl_buffer(ctx, "/proc/sys/net/core/rmem_max", 2 * 1024 * 1024); });
  run_check(ctx, "* Check net.core.wmem_max...", 50,
            [&ctx]() { check_sysctl_buffer(ctx, "/proc/sys/net/core/wmem_max", 2 * 1024 * 1024); });
  run_check(ctx, "* Check rp_filter...", 50, [&ctx]() { check_rp_filter(ctx); });

#if !defined(_WIN32) && !defined(__CYGWIN__)
  run_check(ctx, "* Check RLIMIT_NOFILE...", 50, [&ctx]() { check_rlimit(ctx, RLIMIT_NOFILE, "nofile", 4096); });
  run_check(ctx, "* Check RLIMIT_MEMLOCK...", 50,
            [&ctx]() { check_rlimit(ctx, RLIMIT_MEMLOCK, "memlock", 64UL * 1024UL * 1024UL); });
#endif

  run_check(ctx, "* Check available space for log dir...", 500, [&ctx]() { check_log_dir_space(ctx); });
  run_check(ctx, "* Check log dir writable...", 100, [&ctx]() { check_log_dir_writable(ctx); });

#if defined(VLINK_SUPPORT_SHM) || defined(VLINK_SUPPORT_SHM2)
  run_check(ctx, "* Check /dev/shm space...", 100, [&ctx]() { check_shm_space(ctx); });
#endif

  run_check(ctx, "* Check VLINK_PLUGIN_DIR...", 100, [&ctx]() { check_directory_env(ctx, "VLINK_PLUGIN_DIR", false); });
  run_check(ctx, "* Check VLINK_PROTO_DIR...", 100, [&ctx]() { check_directory_env(ctx, "VLINK_PROTO_DIR", false); });
  run_check(ctx, "* Check VLINK_FBS_DIR...", 100, [&ctx]() { check_directory_env(ctx, "VLINK_FBS_DIR", false); });
  run_check(ctx, "* Check VLINK_QOS_CONFIG...", 100, [&ctx]() { check_file_env(ctx, "VLINK_QOS_CONFIG", false); });

#ifdef VLINK_SUPPORT_DDS
  run_check(ctx, "* Check VLINK_FASTDDS_QOS_FILE...", 100,
            [&ctx]() { check_file_env(ctx, "VLINK_FASTDDS_QOS_FILE", false); });
#endif

#ifdef VLINK_SUPPORT_DDSC
  run_check(ctx, "* Check VLINK_CYCLONEDDS_URI...", 100, [&ctx]() {
    if (vlink::Utils::get_env("VLINK_CYCLONEDDS_URI").empty()) {
      end_diag(ctx, DiagType::kPass, "VLINK_CYCLONEDDS_URI is empty");
    } else {
      end_diag(ctx, DiagType::kWarning, "URI configured; validation is performed by Cyclone DDS at startup");
    }
  });
#endif

  run_check(ctx, "* Check VLINK_LOG_LEVEL value...", 50, [&ctx]() { check_log_level_range(ctx); });

  run_check(ctx, "* Check cpu usage...", 100, [&ctx]() { check_cpu_usage(ctx); });
  run_check(ctx, "* Check memory usage...", 100, [&ctx]() { check_memory_usage(ctx); });
  run_check(ctx, "* Check singleton lock...", 100, [&ctx]() { check_singleton_conflict(ctx); });
  run_check(ctx, "* Check time sync daemon...", 100, [&ctx]() { check_time_sync(ctx); });

#ifdef VLINK_SUPPORT_SHM
  run_check(ctx, "* Check iox-roudi running...", 100, [&ctx]() { check_roudi_running(ctx); });
#endif

#ifdef VLINK_SUPPORT_FDBUS
  run_check(ctx, "* Check fdbus name_server running...", 100, [&ctx]() { check_name_server_running(ctx); });
#endif

#ifdef VLINK_SUPPORT_MQTT
  run_check(ctx, "* Check mqtt broker configured...", 50, [&ctx]() { check_mqtt_broker(ctx); });
#endif

  const std::vector<ProcessCheck> process_checks = {
      {"* Check proxy running...", "proxy", "vlink-proxy", "vlink-proxy.exe", true, "Proxy"},
      {"* Check bag running...", "bag", "vlink-bag", "vlink-bag.exe", false, "Bag"},
      {"* Check trigger running...", "trigger", "vlink-trigger", "vlink-trigger.exe", false, "Trigger"},
      {"* Check parse running...", "parse", "vlink-parse", "vlink-parse.exe", false, "Parse"},
      {"* Check eproto running...", "eproto", "vlink-eproto", "vlink-eproto.exe", false, "Eproto"},
      {"* Check efbs running...", "efbs", "vlink-efbs", "vlink-efbs.exe", false, "Efbs"},
      {"* Check monitor running...", "monitor", "vlink-monitor", "vlink-monitor.exe", false, "Monitor"},
      {"* Check viewer running...", "viewer", "vlink-viewer", "vlink-viewer.exe", false, "Viewer"},
      {"* Check player running...", "player", "vlink-player", "vlink-player.exe", false, "Player"},
      {"* Check analyzer running...", "analyzer", "vlink-analyzer", "vlink-analyzer.exe", false, "Analyzer"},
      {"* Check bench running...", "bench", "vlink-bench", "vlink-bench.exe", false, "Bench"},
      {"* Check webviz running...", "webviz", "vlink-webviz", "vlink-webviz.exe", false, "Webviz"},
  };

  for (const auto& pc : process_checks) {
    run_check(ctx, pc.title, 100, [&ctx, &pc]() { check_process(ctx, pc); });
  }

  run_check(ctx, "* Check others running...", 100, [&ctx]() { check_others_running(ctx); });

  if (all_case) {
    std::cout << std::endl;

#ifdef VLINK_ENABLE_CXX_STD_20
    check_flag(ctx, "- Check cxx_20 enabled...", "VLINK_ENABLE_CXX_STD_20", true);
#else
    check_flag(ctx, "- Check cxx_20 enabled...", "VLINK_ENABLE_CXX_STD_20", false);
#endif

#ifdef VLINK_ENABLE_C_API
    check_flag(ctx, "- Check c_api enabled...", "VLINK_ENABLE_C_API", true);
#else
    check_flag(ctx, "- Check c_api enabled...", "VLINK_ENABLE_C_API", false);
#endif

#ifdef VLINK_ENABLE_SECURITY
    check_flag(ctx, "- Check security enabled...", "VLINK_ENABLE_SECURITY", true);
#else
    check_flag(ctx, "- Check security enabled...", "VLINK_ENABLE_SECURITY", false);
#endif

#ifdef VLINK_ENABLE_SQLITE
    check_flag(ctx, "- Check sqlite enabled...", "VLINK_ENABLE_SQLITE", true);
#else
    check_flag(ctx, "- Check sqlite enabled...", "VLINK_ENABLE_SQLITE", false);
#endif

#ifdef VLINK_ENABLE_ZSTD
    check_flag(ctx, "- Check zstd enabled...", "VLINK_ENABLE_ZSTD", true);
#else
    check_flag(ctx, "- Check zstd enabled...", "VLINK_ENABLE_ZSTD", false);
#endif

#ifdef VLINK_ENABLE_LOG_BACKEND
    check_flag(ctx, "- Check logger backend...", "VLINK_ENABLE_LOG_BACKEND", true);
#else
    check_flag(ctx, "- Check logger backend...", "VLINK_ENABLE_LOG_BACKEND", false);
#endif

#ifdef VLINK_ENABLE_CLI_INFO
    check_flag(ctx, "- Check cli-info enabled...", "VLINK_ENABLE_CLI_INFO", true);
#else
    check_flag(ctx, "- Check cli-info enabled...", "VLINK_ENABLE_CLI_INFO", false);
#endif

#ifdef VLINK_ENABLE_CLI_BAG
    check_flag(ctx, "- Check cli-bag enabled...", "VLINK_ENABLE_CLI_BAG", true);
#else
    check_flag(ctx, "- Check cli-bag enabled...", "VLINK_ENABLE_CLI_BAG", false);
#endif

#ifdef VLINK_ENABLE_CLI_TRIGGER
    check_flag(ctx, "- Check cli-trigger enabled...", "VLINK_ENABLE_CLI_TRIGGER", true);
#else
    check_flag(ctx, "- Check cli-trigger enabled...", "VLINK_ENABLE_CLI_TRIGGER", false);
#endif

#ifdef VLINK_ENABLE_CLI_EPROTO
    check_flag(ctx, "- Check cli-eproto enabled...", "VLINK_ENABLE_CLI_EPROTO", true);
#else
    check_flag(ctx, "- Check cli-eproto enabled...", "VLINK_ENABLE_CLI_EPROTO", false);
#endif

#ifdef VLINK_ENABLE_CLI_EFBS
    check_flag(ctx, "- Check cli-efbs enabled...", "VLINK_ENABLE_CLI_EFBS", true);
#else
    check_flag(ctx, "- Check cli-efbs enabled...", "VLINK_ENABLE_CLI_EFBS", false);
#endif

#ifdef VLINK_ENABLE_CLI_LIST
    check_flag(ctx, "- Check cli-list enabled...", "VLINK_ENABLE_CLI_LIST", true);
#else
    check_flag(ctx, "- Check cli-list enabled...", "VLINK_ENABLE_CLI_LIST", false);
#endif

#ifdef VLINK_ENABLE_CLI_MONITOR
    check_flag(ctx, "- Check cli-monitor enabled...", "VLINK_ENABLE_CLI_MONITOR", true);
#else
    check_flag(ctx, "- Check cli-monitor enabled...", "VLINK_ENABLE_CLI_MONITOR", false);
#endif

#ifdef VLINK_ENABLE_CLI_PARSE
    check_flag(ctx, "- Check cli-parse enabled...", "VLINK_ENABLE_CLI_PARSE", true);
#else
    check_flag(ctx, "- Check cli-parse enabled...", "VLINK_ENABLE_CLI_PARSE", false);
#endif

#ifdef VLINK_ENABLE_CLI_CHECK
    check_flag(ctx, "- Check cli-check enabled...", "VLINK_ENABLE_CLI_CHECK", true);
#else
    check_flag(ctx, "- Check cli-check enabled...", "VLINK_ENABLE_CLI_CHECK", false);
#endif

#ifdef VLINK_ENABLE_CLI_BENCH
    check_flag(ctx, "- Check cli-bench enabled...", "VLINK_ENABLE_CLI_BENCH", true);
#else
    check_flag(ctx, "- Check cli-bench enabled...", "VLINK_ENABLE_CLI_BENCH", false);
#endif

#ifdef VLINK_ENABLE_PROXY
    check_flag(ctx, "- Check proxy enabled...", "VLINK_ENABLE_PROXY", true);
#else
    check_flag(ctx, "- Check proxy enabled...", "VLINK_ENABLE_PROXY", false);
#endif

#ifdef VLINK_ENABLE_VIEWER
    check_flag(ctx, "- Check viewer enabled...", "VLINK_ENABLE_VIEWER", true);
#else
    check_flag(ctx, "- Check viewer enabled...", "VLINK_ENABLE_VIEWER", false);
#endif

#ifdef VLINK_ENABLE_EXAMPLES
    check_flag(ctx, "- Check examples enabled...", "VLINK_ENABLE_EXAMPLES", true);
#else
    check_flag(ctx, "- Check examples enabled...", "VLINK_ENABLE_EXAMPLES", false);
#endif

#ifdef VLINK_ENABLE_TEST
    check_flag(ctx, "- Check test enabled...", "VLINK_ENABLE_TEST", true);
#else
    check_flag(ctx, "- Check test enabled...", "VLINK_ENABLE_TEST", false);
#endif
  }

  message_loop.wait_for_quit(100);
  message_loop.quit();
  message_loop.wait_for_quit();

  if (show_summary) {
    std::cout << std::endl;
    std::cout << kColorInfo << "Summary: " << kColorReset;
    std::cout << kColorPass << ctx.passed_count << " PASSED" << kColorReset << ", ";
    std::cout << kColorWarn << ctx.warning_count << " WARNING" << kColorReset << ", ";
    std::cout << kColorFail << ctx.failed_count << " FAILED" << kColorReset;
    std::cout << std::endl;
  }

  return ctx.failed_count;
}
