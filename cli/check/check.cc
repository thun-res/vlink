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
#include <vlink/base/helpers.h>
#include <vlink/base/message_loop.h>
#include <vlink/base/utils.h>
#include <vlink/version.h>
#include <vlink/vlink.h>
#ifdef VLINK_SUPPORT_SHM
#include <vlink/modules/fdbus_conf.h>
#include <vlink/modules/shm_conf.h>
#endif

#include <algorithm>
#include <argparse/argparse.hpp>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/resource.h>
#include <sys/wait.h>
#endif

#if defined(__linux__) || defined(__ANDROID__)
#include <net/if.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

struct EnvInfo final {
  std::string env_key;
  std::string env_value;
  std::string description;
  bool has_set{false};
};

enum class DiagType : uint8_t {
  kPass = 0,
  kWarning = 1,
  kFailed = 2,
};

[[maybe_unused]] static constexpr int kTitleWidth = 50;
[[maybe_unused]] static constexpr int kStatusPassPad = 50;
[[maybe_unused]] static constexpr int kStatusWarnPad = 49;
[[maybe_unused]] static constexpr int kStatusFailPad = 50;
[[maybe_unused]] static constexpr int kMulticastDiscovery[] = {239, 255, 0, 100};
[[maybe_unused]] static constexpr int kMulticastDds[] = {239, 255, 0, 1};

[[maybe_unused]] static const char kColorReset[] = "\033[0m";
[[maybe_unused]] static const char kColorPass[] = "\033[32m";
[[maybe_unused]] static const char kColorWarn[] = "\033[33m";
[[maybe_unused]] static const char kColorFail[] = "\033[31m";
[[maybe_unused]] static const char kColorInfo[] = "\033[36m";
[[maybe_unused]] static const char kColorHeader[] = "\033[44;37;1m";

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
#else
  return run_cmd_output("route -n");
#endif
}

struct DiagContext final {
  vlink::MessageLoop* loop{nullptr};
  std::atomic_bool stop_flag{false};
  std::atomic<int> passed_count{0};
  std::atomic<int> warning_count{0};
  std::atomic<int> failed_count{0};
  std::atomic<DiagType> last_type{DiagType::kFailed};
  std::mutex mtx;
  vlink::ConditionVariable cv;
  std::string detail;
  std::string filter;
};

static bool diag_accepted(const DiagContext& ctx, const std::string& title) {
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
  ctx.loop->post_task([&ctx, title]() {
    std::unique_lock lock(ctx.mtx);

    ctx.cv.wait(lock, [&ctx]() -> bool { return !ctx.stop_flag || ctx.loop->is_ready_to_quit(); });

    std::cout << title << std::string(kTitleWidth - title.size(), ' ');
    std::cout << "......";
    std::cout.flush();

    ctx.cv.wait(lock, [&ctx]() -> bool { return ctx.stop_flag || ctx.loop->is_ready_to_quit(); });

    ctx.stop_flag = false;

    std::cout << "\033[2K\r";

    const int detail_len = static_cast<int>(ctx.detail.size());

    switch (ctx.last_type) {
      case DiagType::kPass:
        std::cout << kColorPass;
        std::cout << title << std::string(kTitleWidth - title.size(), ' ') << "PASSED";
        std::cout << std::string(std::max(kStatusPassPad - detail_len, 2), ' ');
        std::cout << ctx.detail << kColorReset << std::endl;
        break;

      case DiagType::kWarning:
        std::cout << kColorWarn;
        std::cout << title << std::string(kTitleWidth - title.size(), ' ') << "WARNING";
        std::cout << std::string(std::max(kStatusWarnPad - detail_len, 2), ' ');
        std::cout << ctx.detail << kColorReset << std::endl;
        break;

      case DiagType::kFailed:
        std::cout << kColorFail;
        std::cout << title << std::string(kTitleWidth - title.size(), ' ') << "FAILED";
        std::cout << std::string(std::max(kStatusFailPad - detail_len, 2), ' ');
        std::cout << ctx.detail << kColorReset << std::endl;
        break;

      default:
        break;
    }
  });

  ctx.loop->wait_for_quit(delay_ms);
}

static void end_diag(DiagContext& ctx, DiagType type, const std::string& detail) {
  {
    std::lock_guard lock(ctx.mtx);
    ctx.stop_flag = true;
    ctx.last_type = type;
    ctx.detail = detail;
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

  ctx.cv.notify_one();
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

  const auto dds_ip_list = vlink::Helpers::get_split_string(dds_ip, ';');
  const auto ipv4_address = vlink::Utils::get_all_ipv4_address(false);

  const bool available = std::any_of(ipv4_address.begin(), ipv4_address.end(), [&dds_ip_list](const std::string& ip) {
    return std::any_of(dds_ip_list.begin(), dds_ip_list.end(), [&ip](const std::string& entry) { return entry == ip; });
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

  const auto split = vlink::Helpers::get_split_string(dds_ip, ';');

  if VUNLIKELY (split.empty()) {
    end_diag(ctx, DiagType::kFailed, "VLINK_DDS_IP=" + dds_ip + " has no IP entries");
    return;
  }

  const auto& first_ip = split.front();
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

  size_t scan_pos = 0;

  while ((scan_pos = result.find(needle, scan_pos)) != std::string::npos) {
    scan_pos += needle.size();

    if (scan_pos >= result.size() || !std::isdigit(result[scan_pos])) {
      end_diag(ctx, DiagType::kPass, "Found " + needle);
      return;
    }
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
  const std::string command_str = vlink::Utils::get_app_dir() + "/vlink-list.exe -nc";

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
#if defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__)
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
    const auto split = vlink::Helpers::get_split_string(dds_ip, ';');

    if (!split.empty()) {
      ip = split.front();
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

  vlink::Utils::register_terminate_signal([&message_loop, &ctx](int) {
    message_loop.quit();
    ctx.cv.notify_one();
  });

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

#if defined(VLINK_SUPPORT_DDS) || defined(VLINK_SUPPORT_DDSC) || defined(VLINK_SUPPORT_DDSR) || \
    defined(VLINK_SUPPORT_DDST)
  run_check(ctx, "* Check VLink DDS IP available...", 100, [&ctx]() { check_dds_ip(ctx); });
  run_check(ctx, "* Check VLink DDS interface...", 100, [&ctx]() { check_dds_interface(ctx); });
  run_check(ctx, "* Check VLink DDS interface MTU...", 100, [&ctx]() { check_interface_mtu(ctx); });
#endif

  run_check(ctx, "* Check VLink multicast address...", 100,
            [&ctx]() { check_multicast_address(ctx, kMulticastDiscovery, false); });

#if defined(VLINK_SUPPORT_DDS) || defined(VLINK_SUPPORT_DDSC) || defined(VLINK_SUPPORT_DDSR) || \
    defined(VLINK_SUPPORT_DDST)
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
  run_check(ctx, "* Check VLINK_CYCLONEDDS_URI...", 100,
            [&ctx]() { check_file_env(ctx, "VLINK_CYCLONEDDS_URI", false); });
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
      {"* Check dump running...", "dump", "vlink-dump", "vlink-dump.exe", false, "Dump"},
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
    message_loop.post_task([]() { std::cout << std::endl; });

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

#ifdef VLINK_ENABLE_CLI_DUMP
    check_flag(ctx, "- Check cli-dump enabled...", "VLINK_ENABLE_CLI_DUMP", true);
#else
    check_flag(ctx, "- Check cli-dump enabled...", "VLINK_ENABLE_CLI_DUMP", false);
#endif

#ifdef VLINK_ENABLE_CLI_CHECK
    check_flag(ctx, "- Check cli-check enabled...", "VLINK_ENABLE_CLI_CHECK", true);
#else
    check_flag(ctx, "- Check cli-check enabled...", "VLINK_ENABLE_CLI_CHECK", false);
#endif

#ifdef VLINK_ENABLE_LOG_QUI
    check_flag(ctx, "- Check quilog enabled...", "VLINK_ENABLE_LOG_QUI", true);
#else
    check_flag(ctx, "- Check quilog enabled...", "VLINK_ENABLE_LOG_QUI", false);
#endif

#ifdef VLINK_ENABLE_LOG_SPD
    check_flag(ctx, "- Check spdlog enabled...", "VLINK_ENABLE_LOG_SPD", true);
#else
    check_flag(ctx, "- Check spdlog enabled...", "VLINK_ENABLE_LOG_SPD", false);
#endif

#ifdef VLINK_ENABLE_LOG_DLT
    check_flag(ctx, "- Check dltlog enabled...", "VLINK_ENABLE_LOG_DLT", true);
#else
    check_flag(ctx, "- Check dltlog enabled...", "VLINK_ENABLE_LOG_DLT", false);
#endif

#ifdef VLINK_ENABLE_LOG_NAT
    check_flag(ctx, "- Check natlog enabled...", "VLINK_ENABLE_LOG_NAT", true);
#else
    check_flag(ctx, "- Check natlog enabled...", "VLINK_ENABLE_LOG_NAT", false);
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

int check_env(bool available_case, const std::string& prefix) {
  std::vector<EnvInfo> env_list = {
      {"VLINK_PROTO_DIR", "", "Directory scanned for .proto schemas by the Protobuf registry.", false},
      {"VLINK_FBS_DIR", "", "Directory scanned for .fbs schemas by the FlatBuffers registry.", false},
      {"VLINK_SCHEMA_PLUGIN", "", "Name of the dynamic plugin that loads Protobuf/FlatBuffers schemas.", false},
      {"VLINK_TMP_DIR", "", "Override directory used for temporary files.", false},
      {"VLINK_LOCK_DIR", "", "Override directory used for singleton lock files.", false},
      {"VLINK_MEMORY_LEVEL", "",
       "MemoryPool tier level (0..9, default 3). 0 = bypass; 1..9 select built-in pyramid (higher = more "
       "resident memory, fewer upstream allocs). Honoured only when Bytes::init_memory_pool() is called.",
       false},
      {"VLINK_MEMORY_PREALLOC", "",
       "Set to 1 to fill every tier to its full blocks_per_chunk quota when the global MemoryPool is built "
       "(best-effort).",
       false},
      {"VLINK_PLUGIN_DIR", "", "Directory searched by vlink::Plugin when loading dynamic modules.", false},
      {"VLINK_URL_PLUGINS", "",
       "Semicolon separated transport plugin library names to dlopen on start (e.g. vlink-zenoh).", false},
      {"VLINK_URL_REMAP", "", "Path to a JSON file describing URL rewrite rules applied at node creation.", false},

      {"VLINK_LOG_LEVEL", "",
       "Global log level: TRACE=0, DEBUG=1, INFO=2, WARN=3, ERROR=4, FATAL=5, OFF=6 (disable all output).", false},
      {"VLINK_LOG_CONSOLE_LEVEL", "", "Override of the log level used for console sink only.", false},
      {"VLINK_LOG_FILE_LEVEL", "", "Override of the log level used for the file sink only.", false},
      {"VLINK_LOG_CONSOLE_UNORDER", "",
       "When set to 1 disables ordered console output (faster, may interleave across threads).", false},
      {"VLINK_LOG_CONSOLE_FMT", "",
       "When set to 1 enables VLink's extended console formatting (timestamp / thread / level color); empty/0 = "
       "minimal format. Boolean toggle, not a template string.",
       false},
      {"VLINK_LOG_DIR", "", "Directory where rotating log files are written.", false},
      {"VLINK_LOG_ENABLE_UTC", "", "When set to 1 prints timestamps in UTC instead of local time.", false},
      {"VLINK_LOG_MAX_SIZE", "", "Maximum size in bytes per log file before rotation (default 10485760 = 10 MiB).",
       false},
      {"VLINK_LOG_MAX_COUNT", "", "Maximum number of rotated log files to retain.", false},
      {"VLINK_LOG_FLUSH_DELAY", "",
       "Periodic async-flush interval in milliseconds (default 500). >0 also flushes immediately at ERROR. "
       "Set to 0 to disable periodic flush and flush every record (TRACE-level flush).",
       false},
      {"VLINK_LOG_PLUGIN", "",
       "Custom logger plugin base name (no 'lib' prefix or '.so' suffix); implements LoggerPluginInterface and is "
       "resolved through Plugin::default_search_path().",
       false},
      {"VLINK_LOG_STORE_STRATEGY", "",
       "When set to 1 uses spdlog's size-based rotating file sink (VLINK_LOG_MAX_SIZE per file); default empty = "
       "VLink's time-rolling sink (one file per period). Only honored when spdlog backend is enabled.",
       false},
      {"VLINK_LOG_OPEN_APPEND", "",
       "When set to 1 appends to the previous log file on start instead of truncating; default 0.", false},
      {"VLINK_LOG_BLOCK_SYNC", "",
       "When set to 1 blocks producer threads if the async log queue is full (prevents drops); default 0 = drop "
       "oldest. spdlog backend only.",
       false},
      {"VLINK_LOG_WRITE_DEPTH", "", "Depth of the async log backend (spdlog / quill) thread-pool queue.", false},

      {"VLINK_BAG_PATH", "",
       "Activates the process-global BagWriter (BagWriter::global_get()) at the given .vdb/.vcap path. All "
       "Publisher/Setter messages are auto-recorded transparently. CLI tools (vlink-bag/dump/eproto/efbs/list/"
       "monitor/bench) explicitly unset this on startup to avoid recursive recording.",
       false},
      {"VLINK_BAG_TAG", "", "User tag stored in bag metadata to label the recording session (default 'Empty').", false},

      {"VLINK_DISCOVER_DISABLE", "",
       "When set to 1 disables the global discovery reporter (no cross-process visibility).", false},
      {"VLINK_DISCOVER_NATIVE", "",
       "When set to 1 restricts discovery multicast to the loopback interface (same-host only).", false},
      {"VLINK_PROFILER_ENABLE", "",
       "Toggles the built-in CpuProfiler (1=enable, 0=disable). Default depends on the compile-time macro "
       "VLINK_PROFILER_DEFAULT_STATE (0 in upstream).",
       false},
      {"VLINK_QOS_CONFIG", "", "Path to the VLink QoS profile file consumed by the extension layer.", false},

#ifdef VLINK_SUPPORT_INTRA
      {"VLINK_INTRA_BIND", "", "Rebinds intra:// URLs to another transport scheme (e.g. dds, shm) for this process.",
       false},
#endif

#if defined(VLINK_SUPPORT_DDS) || defined(VLINK_SUPPORT_DDSC) || defined(VLINK_SUPPORT_DDSR) || \
    defined(VLINK_SUPPORT_DDST)
      {"VLINK_DDS_BIND", "",
       "Rebinds dds:// URLs to a specific DDS backend at runtime: dds (Fast-DDS) / ddsf (alias of dds) / ddsc "
       "(CycloneDDS) / ddsr (RTI) / ddst (TravoDDS). Empty = no rebind.",
       false},
      {"VLINK_DDS_DEBUG", "",
       "When set to 1 raises Fast-DDS / CycloneDDS / RTI / TravoDDS internal log verbosity (Error -> Info / "
       "FATAL -> ALL / ERROR -> STATUS_ALL / 0xffff respectively); default 0.",
       false},
      {"VLINK_DDS_EVENT_QOS", "", "Default QoS profile name for DDS Publisher/Subscriber nodes.", false},
      {"VLINK_DDS_METHOD_QOS", "", "Default QoS profile name for DDS Client/Server nodes.", false},
      {"VLINK_DDS_FIELD_QOS", "", "Default QoS profile name for DDS Setter/Getter nodes.", false},
      {"VLINK_DDS_DOMAIN", "", "DDS domain id for this process (valid range 0-232).", false},
      {"VLINK_DDS_IP", "", "Semicolon separated unicast IPv4 list advertised by DDS discovery.", false},
      {"VLINK_DDS_IP_FILTER", "", "When set to 1 filters VLINK_DDS_IP down to addresses currently present on the host.",
       false},
      {"VLINK_DDS_MULTICAST_IP", "", "Override of the DDS default discovery multicast address.", false},
      {"VLINK_DDS_PEER", "", "Initial peer list for DDS participant discovery.", false},
      {"VLINK_DDS_BUF", "", "DDS socket send/recv buffer size hint in bytes.", false},
      {"VLINK_DDS_MTU", "", "Maximum DDS payload size in bytes before fragmentation.", false},
      {"VLINK_DDS_UDP", "", "Toggles UDP transport for DDS (default 1 = enabled; set 0 to disable).", false},
      {"VLINK_DDS_TCP", "", "Toggles TCP transport for DDS (default 0; set 1 to enable).", false},
      {"VLINK_DDS_SHM", "", "Toggles same-host shared-memory transport inside DDS (default 0; set 1 to enable).",
       false},
      {"VLINK_DDS_LESS_MEMORY", "",
       "When set to 1 trims DDS participant memory footprint at the cost of throughput; default 0.", false},
#endif

#ifdef VLINK_SUPPORT_DDS
      {"VLINK_FASTDDS_QOS_FILE", "", "Path to a Fast-DDS XML QoS profile file.", false},
#endif

#ifdef VLINK_SUPPORT_DDSC
      {"VLINK_CYCLONEDDS_URI", "", "Cyclone DDS config URI (file://..., <CycloneDDS>... inline XML).", false},
#endif

#ifdef VLINK_SUPPORT_DDST
      {"VLINK_TRAVODDS_QOS_FILE", "", "Path to the TravoDDS (ddst://) QoS profile file.", false},
#endif

#ifdef VLINK_SUPPORT_SHM
      {"VLINK_SHM_DEBUG", "", "When set to 1 enables verbose logs in the SHM / iceoryx factory.", false},
      {"VLINK_SHM_DEPTH", "", "Queue depth used when creating iceoryx publishers / subscribers.", false},
#endif

#ifdef VLINK_SUPPORT_SHM2
      {"VLINK_SHM2_DEBUG", "", "When set to 1 enables verbose logs in the SHM2 / iceoryx2 factory.", false},
      {"VLINK_SHM2_DEPTH", "", "Queue depth used when creating iceoryx2 publishers / subscribers.", false},
      {"VLINK_SHM2_CONFIG", "", "Path to the iceoryx2 TOML configuration file.", false},
      {"VLINK_SHM2_NOTIFY_EVERY", "",
       "Notify-every-N coalescing for shm2:// publishers (default 1; raise to amortize wakeups).", false},
#endif

#ifdef VLINK_SUPPORT_ZENOH
      {"VLINK_ZENOH_CONFIG", "", "Path to the Zenoh JSON5 session configuration file.", false},
      {"VLINK_ZENOH_DOMAIN", "", "Zenoh domain id (numeric, used to scope key expressions).", false},
      {"VLINK_ZENOH_MODE", "", "Zenoh session mode: peer / client / router (default peer).", false},
      {"VLINK_ZENOH_IP", "", "Bind IP address advertised by the Zenoh session.", false},
      {"VLINK_ZENOH_PEER", "", "Initial peer endpoint list (e.g. tcp/host:7447).", false},
      {"VLINK_ZENOH_LISTEN", "", "Listen endpoint list for incoming Zenoh connections.", false},
      {"VLINK_ZENOH_MULTICAST", "", "Zenoh multicast scout address (default 239.255.0.100).", false},
      {"VLINK_ZENOH_MULTICAST_IF", "", "Network interface used for Zenoh multicast scouting.", false},
      {"VLINK_ZENOH_MULTICAST_TTL", "", "TTL for Zenoh multicast scouting packets.", false},
      {"VLINK_ZENOH_GOSSIP", "", "When set to 1 enables Zenoh gossip discovery (default 1).", false},
      {"VLINK_ZENOH_RX_BUF", "", "Zenoh receive buffer size hint in bytes.", false},
      {"VLINK_ZENOH_MAX_MSG", "", "Maximum Zenoh message size in bytes before fragmentation.", false},
      {"VLINK_ZENOH_TX_QUEUE_DATA", "", "Zenoh data send queue depth.", false},
      {"VLINK_ZENOH_TX_QUEUE_RT", "", "Zenoh real-time send queue depth.", false},
      {"VLINK_ZENOH_LOWLATENCY", "", "When set to 1 enables Zenoh low-latency tuning (default 0).", false},
      {"VLINK_ZENOH_QOS", "", "When set to 1 enables Zenoh QoS network priorities (default 1).", false},
      {"VLINK_ZENOH_COMPRESSION", "", "When set to 1 enables Zenoh payload compression (default 0).", false},
      {"VLINK_ZENOH_TIMESTAMPS", "", "When set to 1 attaches HLC timestamps to Zenoh samples (default 0).", false},
      {"VLINK_ZENOH_BATCH_ENABLED", "",
       "When set to 'true' enables batch publishing on the Zenoh session (default true).", false},
      {"VLINK_ZENOH_BATCH_TIME_LIMIT_MS", "", "Zenoh batch coalescing window in milliseconds (default 1).", false},
      {"VLINK_ZENOH_ALLOWED_LOCALITY", "",
       "Zenoh allowed origin: 'local' (session-local only) / 'remote' (remote only) / other (any). "
       "Requires Z_FEATURE_UNSTABLE_API. Default 'any'.",
       false},
      {"VLINK_ZENOH_EVENT_QOS", "", "Default QoS profile name for Zenoh Publisher/Subscriber nodes.", false},
      {"VLINK_ZENOH_METHOD_QOS", "", "Default QoS profile name for Zenoh Client/Server nodes.", false},
      {"VLINK_ZENOH_FIELD_QOS", "", "Default QoS profile name for Zenoh Setter/Getter nodes.", false},
      {"VLINK_ZENOH_SHM", "",
       "When set to 1 enables Zenoh shared-memory transport (default 0; "
       "requires zenoh-c built with Z_FEATURE_SHARED_MEMORY + Z_FEATURE_UNSTABLE_API).",
       false},
      {"VLINK_ZENOH_SHM_MODE", "", "Zenoh SHM provider init mode: 'init' (default, eager) / 'lazy'.", false},
      {"VLINK_ZENOH_SHM_SIZE", "",
       "Zenoh SHM transport pool size; accepts B/K/M/G suffix (transport_optimization/pool_size).", false},
      {"VLINK_ZENOH_SHM_THRESHOLD", "",
       "Zenoh auto-SHM promotion threshold in bytes (transport_optimization/message_size_threshold).", false},
      {"VLINK_ZENOH_SHM_LOAN_THRESHOLD", "",
       "Minimum size for which Node::loan() returns a Zenoh SHM buffer; smaller sizes fall back to "
       "Bytes::create heap (default 8192, accepts B/K/M/G).",
       false},
      {"VLINK_ZENOH_SHM_BLOCKING", "",
       "When set to 1 Node::loan() blocks waiting for SHM GC + defrag on pool exhaustion (default 0 = non-blocking).",
       false},
#endif

#ifdef VLINK_SUPPORT_MQTT
      {"VLINK_MQTT_BROKER", "", "MQTT broker endpoint (default tcp://localhost:1883).", false},
      {"VLINK_MQTT_CLIENT_ID", "", "MQTT client id prefix (default 'vlink_mqtt'; PID/UUID suffix appended).", false},
      {"VLINK_MQTT_DOMAIN", "", "Domain id mixed into MQTT topic prefixes for tenant isolation.", false},
      {"VLINK_MQTT_KEEPALIVE", "", "MQTT keepalive interval in seconds (default 60).", false},
      {"VLINK_MQTT_QOS", "", "MQTT default QoS level for published messages (0 / 1 / 2).", false},
#endif

#ifdef VLINK_SUPPORT_SOMEIP
      {"VLINK_SOMEIP_CFG", "", "Path to the vSomeIP JSON configuration file.", false},
#endif

      {"VLINK_SSL_VERIFY", "", "Enable TLS certificate verification.", false},
      {"VLINK_SSL_CA", "", "Path to the TLS CA bundle.", false},
      {"VLINK_SSL_CERT", "", "Path to the TLS client certificate.", false},
      {"VLINK_SSL_KEY", "", "Path to the TLS private key.", false},
      {"VLINK_SSL_KEY_PASS", "", "Password used to decrypt the TLS private key.", false},
      {"VLINK_SSL_CIPHERS", "", "Allowed TLS cipher list.", false},
      {"VLINK_SSL_SNI", "", "Server Name Indication override for TLS.", false},
  };

  int set_count = 0;
  int shown_count = 0;

  for (auto& info : env_list) {
    if (!prefix.empty() && !vlink::Helpers::has_startwith(info.env_key, prefix)) {
      continue;
    }

    std::string env_value = vlink::Utils::get_env(info.env_key);

    if (!env_value.empty()) {
      info.has_set = true;
      ++set_count;
    }

    info.env_value = std::move(env_value);

    if (info.has_set) {
      std::cout << kColorPass;
      std::cout << "[" << info.env_key << "]: ";
      std::cout << info.env_value;
      std::cout << kColorReset << std::endl;

      std::cout << info.description << std::endl << std::endl;
      ++shown_count;
      continue;
    }

    if (available_case) {
      continue;
    }

    std::cout << kColorFail;
    std::cout << "[" << info.env_key << "]";
    std::cout << kColorReset << std::endl;

    std::cout << info.description << std::endl << std::endl;
    ++shown_count;
  }

  std::cout << kColorInfo << "Summary: " << set_count << "/" << env_list.size() << " VLink environment variables set";

  if (!prefix.empty()) {
    std::cout << " (shown " << shown_count << " matching prefix \"" << prefix << "\")";
  }

  std::cout << kColorReset << std::endl;

  return 0;
}

enum class TestOutcome : uint8_t {
  kPassed = 0,
  kWarning = 1,
  kFailed = 2,
};

std::string g_test_current_title;  // NOLINT(runtime/string)

void print_test_row(const std::string& label) {
  std::string title = "* Test " + label + "...";

  if (title.size() < static_cast<size_t>(kTitleWidth)) {
    title.append(kTitleWidth - title.size(), ' ');
  }

  g_test_current_title = title;

  std::cout << title << "......";
  std::cout.flush();

  std::this_thread::sleep_for(std::chrono::milliseconds(120));
}

void print_outcome(TestOutcome outcome, const std::string& detail) {
  const int detail_len = static_cast<int>(detail.size());

  std::cout << "\033[2K\r";

  switch (outcome) {
    case TestOutcome::kPassed:
      std::cout << kColorPass << g_test_current_title << "PASSED";
      std::cout << std::string(std::max(kStatusPassPad - detail_len, 2), ' ');
      std::cout << detail << kColorReset << std::endl;
      break;
    case TestOutcome::kWarning:
      std::cout << kColorWarn << g_test_current_title << "WARNING";
      std::cout << std::string(std::max(kStatusWarnPad - detail_len, 2), ' ');
      std::cout << detail << kColorReset << std::endl;
      break;
    case TestOutcome::kFailed:
      std::cout << kColorFail << g_test_current_title << "FAILED";
      std::cout << std::string(std::max(kStatusFailPad - detail_len, 2), ' ');
      std::cout << detail << kColorReset << std::endl;
      break;
    default:
      break;
  }
}

std::pair<TestOutcome, std::string> run_event_roundtrip(const std::string& url, int timeout_ms, int message_count,
                                                        bool external_allowed) {
  std::atomic<int> received_count{0};
  std::atomic<bool> mismatch{false};

  try {
    vlink::Subscriber<std::string> sub(url);
    sub.listen([&received_count, &mismatch, message_count](const std::string& payload) {
      const int idx = received_count.load();

      if VUNLIKELY (idx >= message_count) {
        return;
      }

      const auto expected = std::string("payload-") + std::to_string(idx);

      if VUNLIKELY (payload != expected) {
        mismatch = true;
      }

      ++received_count;
    });

    vlink::Publisher<std::string> pub(url);

    if VUNLIKELY (!pub.wait_for_subscribers(std::chrono::milliseconds(timeout_ms))) {
      if (external_allowed) {
        return {TestOutcome::kWarning, "no subscriber within " + std::to_string(timeout_ms) + "ms"};
      }

      return {TestOutcome::kFailed, "no subscriber within " + std::to_string(timeout_ms) + "ms"};
    }

    for (int i = 0; i < message_count; ++i) {
      pub.publish(std::string("payload-") + std::to_string(i));
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (received_count < message_count && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  } catch (const std::exception& e) {
    return {TestOutcome::kFailed, std::string("exception: ") + e.what()};
  } catch (...) {
    return {TestOutcome::kFailed, "unknown exception during node construction"};
  }

  if VUNLIKELY (mismatch) {
    return {TestOutcome::kFailed, "payload mismatch"};
  }

  if VUNLIKELY (received_count < message_count) {
    if (external_allowed && received_count == 0) {
      return {TestOutcome::kWarning,
              "delivered " + std::to_string(received_count) + "/" + std::to_string(message_count)};
    }

    return {TestOutcome::kFailed, "delivered " + std::to_string(received_count) + "/" + std::to_string(message_count)};
  }

  return {TestOutcome::kPassed, std::to_string(received_count) + "/" + std::to_string(message_count) + " messages"};
}

std::pair<TestOutcome, std::string> run_method_roundtrip(const std::string& url, int timeout_ms,
                                                         bool external_allowed) {
  try {
    vlink::Server<std::string, std::string> server(url);
    server.listen([](const std::string& req, std::string& resp) {
      resp = "echo:" + req;
      return true;
    });

    vlink::Client<std::string, std::string> client(url);

    if VUNLIKELY (!client.wait_for_connected(std::chrono::milliseconds(timeout_ms))) {
      if (external_allowed) {
        return {TestOutcome::kWarning, "server not reachable within " + std::to_string(timeout_ms) + "ms"};
      }

      return {TestOutcome::kFailed, "server not reachable within " + std::to_string(timeout_ms) + "ms"};
    }

    std::string resp;

    if VUNLIKELY (!client.invoke(std::string("ping"), resp, std::chrono::milliseconds(timeout_ms))) {
      return {TestOutcome::kFailed, "invoke timed out"};
    }

    if VUNLIKELY (resp != "echo:ping") {
      return {TestOutcome::kFailed, "unexpected response '" + resp + "'"};
    }

    return {TestOutcome::kPassed, "1/1 calls"};
  } catch (const std::exception& e) {
    return {TestOutcome::kFailed, std::string("exception: ") + e.what()};
  } catch (...) {
    return {TestOutcome::kFailed, "unknown exception during node construction"};
  }
}

std::pair<TestOutcome, std::string> run_field_roundtrip(const std::string& url, int timeout_ms, bool external_allowed) {
  try {
    vlink::Getter<int> getter(url);
    vlink::Setter<int> setter(url);
    setter.set(42);

    if VUNLIKELY (!getter.wait_for_value(std::chrono::milliseconds(timeout_ms))) {
      if (external_allowed) {
        return {TestOutcome::kWarning, "getter did not receive a value in " + std::to_string(timeout_ms) + "ms"};
      }

      return {TestOutcome::kFailed, "getter did not receive a value in " + std::to_string(timeout_ms) + "ms"};
    }

    const auto value = getter.get();

    if VUNLIKELY (!value.has_value() || *value != 42) {
      return {TestOutcome::kFailed, "expected 42, got " + (value ? std::to_string(*value) : std::string("nullopt"))};
    }

    return {TestOutcome::kPassed, "1/1 values"};
  } catch (const std::exception& e) {
    return {TestOutcome::kFailed, std::string("exception: ") + e.what()};
  } catch (...) {
    return {TestOutcome::kFailed, "unknown exception during node construction"};
  }
}

TestOutcome run_paradigm_event() {
  print_test_row("EVENT  intra");

  const auto [outcome, detail] = run_event_roundtrip("intra://check/paradigm/event", 2000, 5, false);
  print_outcome(outcome, detail);
  return outcome;
}

TestOutcome run_paradigm_method() {
  print_test_row("METHOD intra");

  const auto [outcome, detail] = run_method_roundtrip("intra://check/paradigm/method", 2000, false);
  print_outcome(outcome, detail);
  return outcome;
}

TestOutcome run_paradigm_field() {
  print_test_row("FIELD  intra");

  const auto [outcome, detail] = run_field_roundtrip("intra://check/paradigm/field", 2000, false);
  print_outcome(outcome, detail);
  return outcome;
}

TestOutcome run_module_event_test(const char* label, const std::string& url, int timeout_ms, bool external_allowed,
                                  bool precondition_met, const char* skip_reason) {
  print_test_row(label);

  if (!precondition_met) {
    const std::string detail = std::string("skipped -- ") + (skip_reason ? skip_reason : "precondition not met");
    print_outcome(TestOutcome::kWarning, detail);
    return TestOutcome::kWarning;
  }

  const auto [outcome, detail] = run_event_roundtrip(url, timeout_ms, 5, external_allowed);
  print_outcome(outcome, detail);
  return outcome;
}

TestOutcome run_module_method_test(const char* label, const std::string& url, int timeout_ms, bool external_allowed,
                                   bool precondition_met, const char* skip_reason) {
  print_test_row(label);

  if (!precondition_met) {
    const std::string detail = std::string("skipped -- ") + (skip_reason ? skip_reason : "precondition not met");
    print_outcome(TestOutcome::kWarning, detail);
    return TestOutcome::kWarning;
  }

  const auto [outcome, detail] = run_method_roundtrip(url, timeout_ms, external_allowed);
  print_outcome(outcome, detail);
  return outcome;
}

TestOutcome run_module_field_test(const char* label, const std::string& url, int timeout_ms, bool external_allowed,
                                  bool precondition_met, const char* skip_reason) {
  print_test_row(label);

  if (!precondition_met) {
    const std::string detail = std::string("skipped -- ") + (skip_reason ? skip_reason : "precondition not met");
    print_outcome(TestOutcome::kWarning, detail);
    return TestOutcome::kWarning;
  }

  const auto [outcome, detail] = run_field_roundtrip(url, timeout_ms, external_allowed);
  print_outcome(outcome, detail);
  return outcome;
}

int check_test() {
  std::cout << kColorHeader;
  std::cout << "[TITLE]" << std::string(kTitleWidth - 8, ' ') << "[STATUS]" << std::string(41, ' ') << "[DETAIL]";
  std::cout << kColorReset << std::endl;

  int passed = 0;
  int warned = 0;
  int failed = 0;

  auto account = [&passed, &warned, &failed](TestOutcome o) {
    switch (o) {
      case TestOutcome::kPassed:
        ++passed;
        break;
      case TestOutcome::kWarning:
        ++warned;
        break;
      case TestOutcome::kFailed:
        ++failed;
        break;
    }
  };

  account(run_paradigm_event());
  account(run_paradigm_method());
  account(run_paradigm_field());

#ifdef VLINK_SUPPORT_INTRA
  account(run_module_event_test("EVENT  intra://", "intra://check/module/intra/event", 1500, false, true, nullptr));
  account(run_module_method_test("METHOD intra://", "intra://check/module/intra/method", 1500, false, true, nullptr));
  account(run_module_field_test("FIELD  intra://", "intra://check/module/intra/field", 1500, false, true, nullptr));
#endif

#ifdef VLINK_SUPPORT_SHM
  {
    const bool roudi_ok = vlink::ShmConf::auto_init_roudi(true);
    account(run_module_event_test("EVENT  shm://", "shm://check/module/shm/event", 2000, true, roudi_ok,
                                  "iox-roudi not running"));
    account(run_module_method_test("METHOD shm://", "shm://check/module/shm/method", 2000, true, roudi_ok,
                                   "iox-roudi not running"));
    account(run_module_field_test("FIELD  shm://", "shm://check/module/shm/field", 2000, true, roudi_ok,
                                  "iox-roudi not running"));
  }
#endif

#ifdef VLINK_SUPPORT_SHM2
  {
    bool ok = true;

    try {
      const auto info = std::filesystem::space("/dev/shm");
      ok = info.available >= 64ULL * 1024ULL * 1024ULL;
    } catch (std::exception&) {
      ok = false;
    }

    account(run_module_event_test("EVENT  shm2://", "shm2://check/module/shm2/event", 2000, true, ok,
                                  "/dev/shm < 64MB or unavailable"));
    account(run_module_method_test("METHOD shm2://", "shm2://check/module/shm2/method", 2000, true, ok,
                                   "/dev/shm < 64MB or unavailable"));
    account(run_module_field_test("FIELD  shm2://", "shm2://check/module/shm2/field", 2000, true, ok,
                                  "/dev/shm < 64MB or unavailable"));
  }
#endif

#ifdef VLINK_SUPPORT_DDS
  account(run_module_event_test("EVENT  dds://", "dds://check/module/dds/event", 3000, true, true, nullptr));
  account(run_module_method_test("METHOD dds://", "dds://check/module/dds/method", 3000, true, true, nullptr));
  account(run_module_field_test("FIELD  dds://", "dds://check/module/dds/field", 3000, true, true, nullptr));
#endif

#ifdef VLINK_SUPPORT_DDSC
  account(run_module_event_test("EVENT  ddsc://", "ddsc://check/module/ddsc/event", 3000, true, true, nullptr));
  account(run_module_method_test("METHOD ddsc://", "ddsc://check/module/ddsc/method", 3000, true, true, nullptr));
  account(run_module_field_test("FIELD  ddsc://", "ddsc://check/module/ddsc/field", 3000, true, true, nullptr));
#endif

#ifdef VLINK_SUPPORT_DDSR
  account(run_module_event_test("EVENT  ddsr://", "ddsr://check/module/ddsr/event", 3000, true, true, nullptr));
  account(run_module_method_test("METHOD ddsr://", "ddsr://check/module/ddsr/method", 3000, true, true, nullptr));
  account(run_module_field_test("FIELD  ddsr://", "ddsr://check/module/ddsr/field", 3000, true, true, nullptr));
#endif

#ifdef VLINK_SUPPORT_DDST
  account(run_module_event_test("EVENT  ddst://", "ddst://check/module/ddst/event", 3000, true, true, nullptr));
  account(run_module_method_test("METHOD ddst://", "ddst://check/module/ddst/method", 3000, true, true, nullptr));
  account(run_module_field_test("FIELD  ddst://", "ddst://check/module/ddst/field", 3000, true, true, nullptr));
#endif

#ifdef VLINK_SUPPORT_ZENOH
  account(run_module_event_test("EVENT  zenoh://", "zenoh://check/module/zenoh/event", 3000, true, true, nullptr));
  account(run_module_method_test("METHOD zenoh://", "zenoh://check/module/zenoh/method", 3000, true, true, nullptr));
  account(run_module_field_test("FIELD  zenoh://", "zenoh://check/module/zenoh/field", 3000, true, true, nullptr));
#endif

#ifdef VLINK_SUPPORT_SOMEIP
  account(run_module_event_test("EVENT  someip://", "someip://0x5566/0x5486?groups=0x8&event=0x9", 2000, true, true,
                                nullptr));
#endif

#ifdef VLINK_SUPPORT_MQTT
  {
    const auto broker = vlink::Utils::get_env("VLINK_MQTT_BROKER");
    account(run_module_event_test("EVENT  mqtt://", "mqtt://check/module/mqtt", 2000, true, !broker.empty(),
                                  "VLINK_MQTT_BROKER not set"));
  }
#endif

#ifdef VLINK_SUPPORT_FDBUS
  {
    const bool name_server_ok = vlink::FdbusConf::has_name_server();
    account(run_module_event_test("EVENT  fdbus://", "fdbus://check/module/fdbus/event", 2000, true, name_server_ok,
                                  "name_server not running"));
    account(run_module_method_test("METHOD fdbus://", "fdbus://check/module/fdbus/method", 2000, true, name_server_ok,
                                   "name_server not running"));
    account(run_module_field_test("FIELD  fdbus://", "fdbus://check/module/fdbus/field", 2000, true, name_server_ok,
                                  "name_server not running"));
  }
#endif

#ifdef VLINK_SUPPORT_QNX
  account(run_module_event_test("EVENT  qnx://", "qnx://check/module/qnx/event", 2000, true, true, nullptr));
  account(run_module_method_test("METHOD qnx://", "qnx://check/module/qnx/method", 2000, true, true, nullptr));
  account(run_module_field_test("FIELD  qnx://", "qnx://check/module/qnx/field", 2000, true, true, nullptr));
#endif

  std::cout << std::endl;
  std::cout << kColorInfo << "Summary: " << kColorReset;
  std::cout << kColorPass << passed << " PASSED" << kColorReset << ", ";
  std::cout << kColorWarn << warned << " WARNING" << kColorReset << ", ";
  std::cout << kColorFail << failed << " FAILED" << kColorReset << std::endl;

  return failed;
}

int main(int argc, char* argv[]) {
  vlink::Utils::set_console_utf8_output();

  vlink::Logger::set_console_level(vlink::Logger::kWarn);
  vlink::Logger::set_file_level(vlink::Logger::kOff);
  vlink::Logger::init("check");

  argparse::ArgumentParser program("check", VLINK_VERSION, argparse::default_arguments::all);

  argparse::ArgumentParser diag_command("diag", VLINK_VERSION, argparse::default_arguments::help);
  diag_command.add_argument("-a", "--all").help("All case").default_value(false).implicit_value(true);
  diag_command.add_argument("-s", "--summary")
      .help("Print PASSED/WARNING/FAILED counts at the end")
      .default_value(false)
      .implicit_value(true);
  diag_command.add_argument("-f", "--filter")
      .help("Only run checks whose title contains the given substring")
      .default_value(std::string{});
  diag_command.add_description("Start automatic diagnosis");

  argparse::ArgumentParser env_command("env", VLINK_VERSION, argparse::default_arguments::help);
  env_command.add_argument("-b", "--available").help("Only available").default_value(false).implicit_value(true);
  env_command.add_argument("-p", "--prefix")
      .help("Only show variables whose name starts with the given prefix")
      .default_value(std::string{});
  env_command.add_description("Detect environment variables");

  argparse::ArgumentParser test_command("test", VLINK_VERSION, argparse::default_arguments::help);
  test_command.add_description("Run an intra:// pub/sub self-test");

  program.add_subparser(diag_command);
  program.add_subparser(env_command);
  program.add_subparser(test_command);

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;

    if (program.is_subcommand_used("diag")) {
      std::cerr << diag_command << std::endl;
    } else if (program.is_subcommand_used("env")) {
      std::cerr << env_command << std::endl;
    } else if (program.is_subcommand_used("test")) {
      std::cerr << test_command << std::endl;
    }

    return 1;
  }

  if (program.is_subcommand_used("diag")) {
    const bool all_case = diag_command.is_used("-a");
    const bool summary_case = diag_command.is_used("-s");
    const auto filter = diag_command.get<std::string>("-f");

    return check_diag(all_case, summary_case, filter);
  }

  if (program.is_subcommand_used("env")) {
    const bool available_case = env_command.is_used("-b");
    const auto prefix = env_command.get<std::string>("-p");

    return check_env(available_case, prefix);
  }

  if (program.is_subcommand_used("test")) {
    return check_test();
  }

  std::cerr << program << std::endl;

  return 1;
}
