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

#include "./base/utils.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "./base/condition_variable.h"

#if __has_include(<unistd.h>)
#include <unistd.h>
#endif

#ifdef _WIN32

#include <unordered_map>

#ifndef _WINSOCK2API_
#include <winsock2.h>
#endif

#include <Windows.h>
#include <Ws2tcpip.h>
#include <iphlpapi.h>
#include <iptypes.h>
#include <mbctype.h>
#include <process.h>
#include <psapi.h>
#include <tlhelp32.h>

// #pragma comment(lib, "Iphlpapi.lib")
// #pragma comment(lib, "ws2_32.lib")

#include <codecvt>
#include <locale>

#else

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>

#include <climits>
#include <cstdlib>
#include <cstring>

#if defined(__linux__)
#include <malloc.h>
#include <sys/syscall.h>
#elif defined(__FreeBSD__)
#include <pthread_np.h>
#include <sys/sysctl.h>
#elif defined(__ANDROID__)
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#elif defined(__QNX__)
#include <process.h>
#endif

#endif

#ifdef __CYGWIN__
#include <Windows.h>
#endif

namespace vlink {

namespace Utils {

std::string get_app_path() noexcept {
  static std::string file_path;
  static std::once_flag flag;

  static auto trim_trailing = [](char* buffer, size_t size) -> size_t {
    while (size > 0 && (buffer[size - 1] == '\n' || buffer[size - 1] == '\r' || buffer[size - 1] == ' ' ||
                        buffer[size - 1] == '\0')) {
      --size;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    buffer[size] = '\0';

    return size;
  };

  static auto resolve_path = [](const char* path, std::string& result) -> bool {
#if defined(_WIN32) || defined(__CYGWIN__)
    char resolved[MAX_PATH] = {0};
    auto len = ::GetFullPathNameA(path, sizeof(resolved), resolved, nullptr);

    if (len > 0 && len < MAX_PATH) {
      result.assign(resolved, len);
      std::replace(result.begin(), result.end(), '\\', '/');
      return true;
    }
#else
    char resolved[PATH_MAX] = {0};

    if (::realpath(path, resolved) != nullptr) {
      result.assign(resolved);
      return true;
    }
#endif

    return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  };

  std::call_once(flag, []() {
#if defined(_WIN32) || defined(__CYGWIN__)
    char buffer[MAX_PATH] = {0};
    auto size = ::GetModuleFileNameA(nullptr, buffer, sizeof(buffer));

    if VLIKELY (size > 0 && size < MAX_PATH) {
      trim_trailing(buffer, size);

      if (!resolve_path(buffer, file_path)) {
        file_path.assign(buffer);
        std::replace(file_path.begin(), file_path.end(), '\\', '/');
      }
    }
#elif defined(__linux__)
    char buffer[PATH_MAX] = {0};
    auto size = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);

    if VLIKELY (size > 0 && size < PATH_MAX) {
      trim_trailing(buffer, static_cast<size_t>(size));

      if (!resolve_path(buffer, file_path)) {
        file_path.assign(buffer);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }
    }
#elif defined(__FreeBSD__)
    char buffer[PATH_MAX] = {0};
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
    size_t len = sizeof(buffer);

    if VLIKELY (sysctl(mib, 4, buffer, &len, nullptr, 0) == 0 && len > 0) {
      if (buffer[len - 1] == '\0') {
        --len;
      }

      trim_trailing(buffer, len);

      if (!resolve_path(buffer, file_path)) {
        file_path.assign(buffer);
      }
    }
#elif defined(__QNX__)
    char buffer[PATH_MAX] = {0};
    FILE* fp = ::fopen("/proc/self/exefile", "r");

    if VLIKELY (fp) {
      auto size = ::fread(buffer, 1, sizeof(buffer) - 1, fp);
      ::fclose(fp);

      trim_trailing(buffer, size);

      if (!resolve_path(buffer, file_path)) {
        file_path.assign(buffer);
      }
    }
#elif defined(__APPLE__)
    char buffer[PATH_MAX] = {0};
    uint32_t size = sizeof(buffer);

    if VLIKELY (::_NSGetExecutablePath(buffer, &size) == 0) {
      trim_trailing(buffer, std::strlen(buffer));

      if (!resolve_path(buffer, file_path)) {
        file_path.assign(buffer);
      }
    }
#endif
  });

  return file_path;
}

std::string get_app_dir() noexcept {
  std::string path = get_app_path();

  if VUNLIKELY (path.empty()) {
    return path;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  return path.substr(0, path.rfind('/'));
}

std::string get_app_name() noexcept {
  std::string path = get_app_path();

  if VUNLIKELY (path.empty()) {
    return path;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  std::string app_name = path.substr(path.rfind('/') + 1, path.length());

#if defined(_WIN32) || defined(__CYGWIN__)
  static std::string exe_suffix = ".exe";

  if (app_name.length() > exe_suffix.length() &&
      app_name.compare(app_name.length() - exe_suffix.length(), exe_suffix.length(), exe_suffix) == 0) {
    app_name = app_name.substr(0, app_name.length() - exe_suffix.length());
  }
#endif

  return app_name;
}

std::string get_host_name() noexcept {
  char hostname[256];

  if VLIKELY (::gethostname(hostname, sizeof(hostname)) == 0) {
    return hostname;
  } else {
    return "";  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }
}

int32_t get_pid() noexcept {
#ifdef _WIN32
  return static_cast<int32_t>(_getpid());
#else
  return static_cast<int32_t>(getpid());
#endif
}

std::string get_pid_str() noexcept {
  static auto pid = get_pid();

  static auto pid_str = std::to_string(pid);

  return pid_str;
}

std::string get_tmp_dir() noexcept {
  static std::string env_tmp_dir = get_env("VLINK_TMP_DIR");

  if (!env_tmp_dir.empty()) {
    return env_tmp_dir;
  }

  std::string tmp_dir;

#if defined(__QNX__)
  tmp_dir = "/var/log";
#else
  try {
    tmp_dir = std::filesystem::temp_directory_path().string();
  } catch (std::filesystem::filesystem_error&) {  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    tmp_dir = "/tmp";                             // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
  std::replace(tmp_dir.begin(), tmp_dir.end(), '\\', '/');

  if (!tmp_dir.empty() && tmp_dir.back() == '/') {
    tmp_dir.pop_back();
  }
#endif

  return tmp_dir;
}

std::string get_env(const std::string& key, const std::string& default_value) noexcept {
#ifdef _WIN32
  int wlen = MultiByteToWideChar(CP_UTF8, 0, key.c_str(), -1, nullptr, 0);

  if (wlen == 0) {
    return default_value;
  }

  std::wstring wname(wlen, L'\0');

  if (MultiByteToWideChar(CP_UTF8, 0, key.c_str(), -1, wname.data(), wlen) == 0) {
    return default_value;
  }

  DWORD size = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);

  if (size == 0) {
    return default_value;
  }

  std::wstring wvalue(size, L'\0');

  if (GetEnvironmentVariableW(wname.c_str(), wvalue.data(), size) == 0) {
    return default_value;
  }

  int len = WideCharToMultiByte(CP_UTF8, 0, wvalue.c_str(), -1, nullptr, 0, nullptr, nullptr);

  if (len == 0) {
    return default_value;
  }

  std::string value(len, '\0');

  if (WideCharToMultiByte(CP_UTF8, 0, wvalue.c_str(), -1, value.data(), len, nullptr, nullptr) == 0) {
    return default_value;
  }

  if (!value.empty() && value.back() == '\0') {
    value.pop_back();
  }

  std::replace(value.begin(), value.end(), '\\', '/');

  return value;
#else
  const char* value = std::getenv(key.c_str());

  if (!value) {
    return default_value;
  }

  return std::string(value);
#endif
}

bool set_env(const std::string& key, const std::string& value, bool force) noexcept {
#ifdef _WIN32
  (void)force;

  return ::_putenv_s(key.c_str(), value.c_str()) == 0;
#else
  return ::setenv(key.c_str(), value.c_str(), static_cast<int>(force)) == 0;
#endif
}

bool unset_env(const std::string& key) noexcept {
#ifdef _WIN32
  return ::SetEnvironmentVariable(key.c_str(), nullptr);  // NOLINT(readability-implicit-bool-conversion)
#else
  return ::unsetenv(key.c_str()) == 0;
#endif
}

bool is_ignored_iface_name(const char* name) noexcept {
  if VUNLIKELY (!name) {
    return true;
  }

  switch (name[0]) {
    case 'l': {
      // if (std::strcmp(name, "lo") == 0) {
      //   return true;
      // }

      // if (std::strncmp(name, "lo0", 3) == 0) {
      //   return true;
      // }

      if (std::strncmp(name, "l2tp", 4) == 0) {
        return true;
      }

      if (std::strncmp(name, "lxc", 3) == 0) {
        return true;
      }

      if (std::strncmp(name, "llw", 3) == 0) {
        return true;
      }

      return false;
    }

    case 'v': {
      if (std::strncmp(name, "vmnet", 5) == 0) {
        return true;
      }

      if (std::strncmp(name, "veth", 4) == 0) {
        return true;
      }

      if (std::strncmp(name, "virbr", 5) == 0) {
        return true;
      }

      if (std::strncmp(name, "vboxnet", 7) == 0) {
        return true;
      }

      if (std::strncmp(name, "vti", 3) == 0) {
        return true;
      }

      if (std::strncmp(name, "vrf", 3) == 0) {
        return true;
      }

      if (std::strncmp(name, "vxlan", 5) == 0) {
        return true;
      }

      return false;
    }

    case 'b': {
      if (std::strncmp(name, "br-", 3) == 0) {
        return true;
      }

      if (std::strncmp(name, "bridge", 6) == 0) {
        return true;
      }

      return false;
    }

    case 'd': {
      if (std::strncmp(name, "docker", 6) == 0) {
        return true;
      }

      return false;
    }

    case 'p': {
      if (std::strncmp(name, "podman", 6) == 0) {
        return true;
      }

      if (std::strncmp(name, "ppp", 3) == 0) {
        return true;
      }

      if (std::strncmp(name, "pptp", 4) == 0) {
        return true;
      }

      if (std::strncmp(name, "patch-", 6) == 0) {
        return true;
      }

      return false;
    }

    case 'o': {
      if (std::strncmp(name, "ovs", 3) == 0) {
        return true;
      }

      return false;
    }

    case 'c': {
      if (std::strncmp(name, "cni", 3) == 0) {
        return true;
      }

      if (std::strncmp(name, "cali", 4) == 0) {
        return true;
      }

      if (std::strncmp(name, "cri", 3) == 0) {
        return true;
      }

      return false;
    }

    case 'f': {
      if (std::strncmp(name, "flannel", 7) == 0) {
        return true;
      }

      if (std::strncmp(name, "fwln", 4) == 0) {
        return true;
      }

      if (std::strncmp(name, "fwpr", 4) == 0) {
        return true;
      }

      return false;
    }

    case 'q': {
      if (std::strncmp(name, "qbr", 3) == 0) {
        return true;
      }

      if (std::strncmp(name, "qvb", 3) == 0) {
        return true;
      }

      if (std::strncmp(name, "qvo", 3) == 0) {
        return true;
      }

      if (std::strncmp(name, "qr-", 3) == 0) {
        return true;
      }

      if (std::strncmp(name, "qg-", 3) == 0) {
        return true;
      }

      return false;
    }

    case 't': {
      if (std::strncmp(name, "tun", 3) == 0) {
        return true;
      }

      if (std::strncmp(name, "tap", 3) == 0) {
        return true;
      }

      if (std::strncmp(name, "tailscale", 9) == 0) {
        return true;
      }

      return false;
    }

    case 'w': {
      if (std::strncmp(name, "wg", 2) == 0) {
        return true;
      }

      if (std::strncmp(name, "weave", 5) == 0) {
        return true;
      }

      if (std::strncmp(name, "wlanmon", 7) == 0) {
        return true;
      }

      return false;
    }

    case 'z': {
      if (std::strncmp(name, "zt", 2) == 0) {
        return true;
      }

      if (std::strncmp(name, "zerotier", 8) == 0) {
        return true;
      }

      return false;
    }

    case 'i': {
      if (std::strncmp(name, "ipip", 4) == 0) {
        return true;
      }

      if (std::strncmp(name, "ip_vti", 6) == 0) {
        return true;
      }

      if (std::strncmp(name, "ip6_vti", 7) == 0) {
        return true;
      }

      if (std::strncmp(name, "ipvlan", 6) == 0) {
        return true;
      }

      return false;
    }

    case 'g': {
      if (std::strncmp(name, "gre", 3) == 0) {
        return true;
      }

      if (std::strncmp(name, "gretap", 6) == 0) {
        return true;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      if (std::strncmp(name, "gif", 3) == 0) {
        return true;
      }

      return false;
    }

    case 'e': {
      if (std::strncmp(name, "erspan", 6) == 0) {
        return true;
      }

      return false;
    }

    case 'k': {
      if (std::strncmp(name, "kube-ipvs", 9) == 0) {
        return true;
      }

      return false;
    }

    case 'm': {
      if (std::strncmp(name, "macvlan", 7) == 0) {
        return true;
      }

      if (std::strncmp(name, "mon", 3) == 0) {
        return true;
      }

      return false;
    }

    case 's': {
      if (std::strncmp(name, "sit", 3) == 0) {
        return true;
      }

      return false;
    }

    case 'u': {
      if (std::strncmp(name, "utun", 4) == 0) {
        return true;
      }

      return false;
    }

    case 'a': {
      if (std::strncmp(name, "awdl", 4) == 0) {
        return true;
      }

      return false;
    }

    default: {
      return false;
    }
  }
}

std::vector<std::string> get_all_ipv4_address(bool filter_available) noexcept {
  std::vector<std::string> ip_addresses;

#ifdef _WIN32
  PIP_ADAPTER_ADDRESSES p_addresses = nullptr;
  ULONG out_buf_len = 0;
  DWORD ret = 0;

  ret = ::GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, p_addresses, &out_buf_len);

  if (ret == ERROR_BUFFER_OVERFLOW) {
    // NOLINTNEXTLINE(bugprone-unhandled-exception-at-new)
    p_addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(new char[out_buf_len]);

    ret = ::GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, p_addresses, &out_buf_len);

    if (ret == NO_ERROR) {
      char ip_buffer[INET_ADDRSTRLEN];

      for (auto* p_curr_addresses = p_addresses; p_curr_addresses != nullptr;
           p_curr_addresses = p_curr_addresses->Next) {
        if (p_curr_addresses->OperStatus != IfOperStatusUp) {
          continue;
        }

        if (filter_available) {
          bool is_physical_eth_or_wifi =
              (p_curr_addresses->IfType == IF_TYPE_ETHERNET_CSMACD) || (p_curr_addresses->IfType == IF_TYPE_IEEE80211);

          bool is_virtual_or_tunnel_iftype =
              (p_curr_addresses->IfType == IF_TYPE_TUNNEL || p_curr_addresses->IfType == IF_TYPE_PPP ||
               p_curr_addresses->IfType == IF_TYPE_PROP_VIRTUAL);

          if (!is_physical_eth_or_wifi && is_virtual_or_tunnel_iftype) {
            continue;
          }
        }

        for (auto* p_unicast = p_curr_addresses->FirstUnicastAddress; p_unicast != nullptr;
             p_unicast = p_unicast->Next) {
          sockaddr* sockaddr = p_unicast->Address.lpSockaddr;

          if VLIKELY (::inet_ntop(AF_INET, &(reinterpret_cast<sockaddr_in*>(sockaddr)->sin_addr), ip_buffer,
                                  INET_ADDRSTRLEN) != nullptr) {
            ip_addresses.emplace_back(ip_buffer);
          }
        }
      }
    }

    delete[] reinterpret_cast<char*>(p_addresses);
  }
#else
  struct ifaddrs* if_addr_struct = nullptr;
  struct ifaddrs* ifa = nullptr;
  void* tmp_addr_ptr = nullptr;

  ::getifaddrs(&if_addr_struct);

  char ip_buffer[INET_ADDRSTRLEN];

  for (ifa = if_addr_struct; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr || !(ifa->ifa_flags & IFF_UP)) {
      continue;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if (ifa->ifa_addr->sa_family != AF_INET) {
      continue;
    }

    if (filter_available && is_ignored_iface_name(ifa->ifa_name)) {
      continue;
    }

    tmp_addr_ptr = &(reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr)->sin_addr);

    if VLIKELY (::inet_ntop(AF_INET, tmp_addr_ptr, ip_buffer, INET_ADDRSTRLEN) != nullptr) {
      ip_addresses.emplace_back(ip_buffer);
    }
  }

  if (if_addr_struct != nullptr) {
    ::freeifaddrs(if_addr_struct);
  }
#endif

  return ip_addresses;
}

std::vector<std::string> get_all_ipv6_address(bool filter_available) noexcept {
  std::vector<std::string> ip_addresses;

#ifdef _WIN32
  PIP_ADAPTER_ADDRESSES p_addresses = nullptr;
  ULONG out_buf_len = 0;
  DWORD ret = 0;

  ret = ::GetAdaptersAddresses(AF_INET6, GAA_FLAG_INCLUDE_PREFIX, nullptr, p_addresses, &out_buf_len);

  if (ret == ERROR_BUFFER_OVERFLOW) {
    // NOLINTNEXTLINE(bugprone-unhandled-exception-at-new)
    p_addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(new char[out_buf_len]);

    ret = ::GetAdaptersAddresses(AF_INET6, GAA_FLAG_INCLUDE_PREFIX, nullptr, p_addresses, &out_buf_len);

    if (ret == NO_ERROR) {
      char ip_buffer[INET6_ADDRSTRLEN];

      for (auto* p_curr_addresses = p_addresses; p_curr_addresses != nullptr;
           p_curr_addresses = p_curr_addresses->Next) {
        if (p_curr_addresses->OperStatus != IfOperStatusUp) {
          continue;
        }

        if (filter_available) {
          bool is_physical_eth_or_wifi =
              (p_curr_addresses->IfType == IF_TYPE_ETHERNET_CSMACD) || (p_curr_addresses->IfType == IF_TYPE_IEEE80211);

          bool is_virtual_or_tunnel_iftype =
              (p_curr_addresses->IfType == IF_TYPE_TUNNEL || p_curr_addresses->IfType == IF_TYPE_PPP ||
               p_curr_addresses->IfType == IF_TYPE_PROP_VIRTUAL);

          if (!is_physical_eth_or_wifi && is_virtual_or_tunnel_iftype) {
            continue;
          }
        }

        for (auto* p_unicast = p_curr_addresses->FirstUnicastAddress; p_unicast != nullptr;
             p_unicast = p_unicast->Next) {
          sockaddr* sockaddr = p_unicast->Address.lpSockaddr;

          if VLIKELY (::inet_ntop(AF_INET6, &(reinterpret_cast<sockaddr_in6*>(sockaddr)->sin6_addr), ip_buffer,
                                  INET6_ADDRSTRLEN) != nullptr) {
            ip_addresses.emplace_back(ip_buffer);
          }
        }
      }
    }

    delete[] reinterpret_cast<char*>(p_addresses);
  }
#else
  struct ifaddrs* if_addr_struct = nullptr;
  struct ifaddrs* ifa = nullptr;
  void* tmp_addr_ptr = nullptr;

  ::getifaddrs(&if_addr_struct);

  char ip_buffer[INET6_ADDRSTRLEN];

  for (ifa = if_addr_struct; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr || !(ifa->ifa_flags & IFF_UP)) {
      continue;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if (ifa->ifa_addr->sa_family != AF_INET6) {
      continue;
    }

    if (filter_available && is_ignored_iface_name(ifa->ifa_name)) {
      continue;
    }

    tmp_addr_ptr = &(reinterpret_cast<struct sockaddr_in6*>(ifa->ifa_addr)->sin6_addr);

    if VLIKELY (::inet_ntop(AF_INET6, tmp_addr_ptr, ip_buffer, INET6_ADDRSTRLEN) != nullptr) {
      ip_addresses.emplace_back(ip_buffer);
    }
  }

  if (if_addr_struct != nullptr) {
    ::freeifaddrs(if_addr_struct);
  }
#endif

  return ip_addresses;
}

std::string get_interface_name_by_ipv4(const std::string& ipv4) noexcept {
#ifdef _WIN32
  ULONG buf_size = 15 * 1024;
  std::vector<BYTE> buf(buf_size);
  PIP_ADAPTER_ADDRESSES adapters = nullptr;

  ULONG ret = ERROR_BUFFER_OVERFLOW;

  for (int retry = 0; retry < 3 && ret == ERROR_BUFFER_OVERFLOW; ++retry) {
    buf.resize(buf_size);
    adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
    ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &buf_size);
  }

  if (ret != NO_ERROR) {
    return {};
  }

  char addr_str[INET_ADDRSTRLEN] = {};

  for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
    for (auto* ua = adapter->FirstUnicastAddress; ua; ua = ua->Next) {
      auto* sa = ua->Address.lpSockaddr;

      if (sa->sa_family != AF_INET) {
        continue;
      }

      addr_str[0] = '\0';
      ::inet_ntop(AF_INET, &(reinterpret_cast<sockaddr_in*>(sa)->sin_addr), addr_str, sizeof(addr_str));

      if (ipv4 == addr_str) {
        int len = WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1, nullptr, 0, nullptr, nullptr);

        if (len <= 0) {
          return {};
        }

        std::string name(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1, name.data(), len, nullptr, nullptr);

        return name;
      }
    }
  }
#else
  struct ifaddrs* ifaddr = nullptr;

  if (::getifaddrs(&ifaddr) != 0) {
    return {};  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  char addr_str[INET_ADDRSTRLEN] = {};

  for (auto* ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || !ifa->ifa_name) {
      continue;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if (ifa->ifa_addr->sa_family != AF_INET) {
      continue;
    }

    addr_str[0] = '\0';
    ::inet_ntop(AF_INET, &(reinterpret_cast<sockaddr_in*>(ifa->ifa_addr)->sin_addr), addr_str, sizeof(addr_str));

    if (ipv4 == addr_str) {
      std::string name(ifa->ifa_name);
      ::freeifaddrs(ifaddr);

      return name;
    }
  }

  ::freeifaddrs(ifaddr);
#endif
  return {};
}

std::string get_interface_name_by_ipv6(const std::string& ipv6) noexcept {
#ifdef _WIN32
  ULONG buf_size = 15 * 1024;
  std::vector<BYTE> buf(buf_size);
  PIP_ADAPTER_ADDRESSES adapters = nullptr;

  ULONG ret = ERROR_BUFFER_OVERFLOW;

  for (int retry = 0; retry < 3 && ret == ERROR_BUFFER_OVERFLOW; ++retry) {
    buf.resize(buf_size);
    adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
    ret = GetAdaptersAddresses(AF_INET6, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &buf_size);
  }

  if (ret != NO_ERROR) {
    return {};
  }

  char addr_str[INET6_ADDRSTRLEN] = {};

  for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
    for (auto* ua = adapter->FirstUnicastAddress; ua; ua = ua->Next) {
      auto* sa = ua->Address.lpSockaddr;

      if (sa->sa_family != AF_INET6) {
        continue;
      }

      addr_str[0] = '\0';
      ::inet_ntop(AF_INET6, &(reinterpret_cast<sockaddr_in6*>(sa)->sin6_addr), addr_str, sizeof(addr_str));

      if (ipv6 == addr_str) {
        int len = WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1, nullptr, 0, nullptr, nullptr);

        if (len <= 0) {
          return {};
        }

        std::string name(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1, name.data(), len, nullptr, nullptr);

        return name;
      }
    }
  }
#else
  struct ifaddrs* ifaddr = nullptr;

  if (::getifaddrs(&ifaddr) != 0) {
    return {};  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  char addr_str[INET6_ADDRSTRLEN] = {};

  for (auto* ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || !ifa->ifa_name) {
      continue;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if (ifa->ifa_addr->sa_family != AF_INET6) {
      continue;
    }

    addr_str[0] = '\0';
    ::inet_ntop(AF_INET6, &(reinterpret_cast<sockaddr_in6*>(ifa->ifa_addr)->sin6_addr), addr_str, sizeof(addr_str));

    if (ipv6 == addr_str) {
      std::string name(ifa->ifa_name);
      ::freeifaddrs(ifaddr);

      return name;
    }
  }

  ::freeifaddrs(ifaddr);
#endif
  return {};
}

std::vector<std::string> get_dds_default_address(bool filter_available, int max_count) noexcept {
  static std::vector<std::string> all = get_all_ipv4_address(filter_available);

  std::vector<std::string> list;

  list.reserve(max_count);

  for (const auto& ip : all) {
    if (ip == "127.0.0.1") {
      list.emplace_back("127.0.0.1");
      break;
    }
  }

  for (const auto& ip : all) {
    if (ip == "127.0.0.1") {
      continue;
    } else if (ip == "0.0.0.0") {
      continue;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    } else if (ip == "10.0.0.100") {
      continue;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    list.emplace_back(ip);

    if (list.size() >= static_cast<size_t>(max_count)) {
      break;
    }
  }

  return list;
}

bool check_singleton(const std::string& program_name) noexcept {
  std::string check_str;

  if (program_name.empty()) {
    check_str = get_app_name();
  } else {
    check_str = program_name;
  }

#if defined(_WIN32)
  HANDLE h_object = ::CreateMutex(nullptr, FALSE, check_str.c_str());

  if (::GetLastError() == ERROR_ALREADY_EXISTS) {
    ::CloseHandle(h_object);
    return false;
  }
#elif defined(_POSIX_VERSION)
  std::string lock_dir;

  static std::string env_lock_dir = get_env("VLINK_LOCK_DIR");

  if (env_lock_dir.empty()) {
#if defined(__QNX__)
    lock_dir = "/var/lock";
#elif defined(__ANDROID__)
    lock_dir = "/data/local/tmp";
#else
    lock_dir = get_tmp_dir();
#endif
  } else {
    lock_dir = env_lock_dir;
  }

  try {
    if (!std::filesystem::exists(lock_dir)) {
      std::filesystem::create_directories(lock_dir);
    }
    // LCOV_EXCL_START GCOVR_EXCL_START
  } catch (std::filesystem::filesystem_error&) {
    std::cerr << "Can not create lock dir." << std::endl;
    return false;
  }
  // LCOV_EXCL_STOP GCOVR_EXCL_STOP

  std::string lock_path = lock_dir + "/" + check_str + ".lock";

  int fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0666);

  if VUNLIKELY (fd < 0) {
    return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if VUNLIKELY (::flock(fd, LOCK_EX | LOCK_NB) == -1) {
    ::close(fd);
    return false;
  }
#endif

  return true;
}

bool wait_for_device(const std::string& path, int timeout_ms, int poll_ms) noexcept {
  if VUNLIKELY (path.empty()) {
    return false;
  }

  auto start_time = std::chrono::steady_clock::now();

  for (;;) {
    try {
      if (std::filesystem::exists(path)) {
        return true;
      }
    } catch (std::filesystem::filesystem_error&) {  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

    auto current_time = std::chrono::steady_clock::now();
    auto elapsed_time = std::chrono::duration_cast<std::chrono::milliseconds>(current_time - start_time);

    if VUNLIKELY (timeout_ms >= 0 && elapsed_time.count() > timeout_ms) {
      return false;
    }

    if VLIKELY (poll_ms >= 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
    }
  }

  return false;
}

void set_console_utf8_output() noexcept {
#ifdef _WIN32
  ::SetConsoleOutputCP(CP_UTF8);
  ::SetConsoleCP(CP_UTF8);
  // _setmbcp(CP_UTF8);
  // SetThreadLocale(MAKELCID(MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT), SORT_DEFAULT));
#endif
}

bool set_thread_name(const std::string& name, std::thread* thread) noexcept {
  std::thread::native_handle_type native_handle;
#ifdef _MSC_VER
  std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
  std::wstring wname = converter.from_bytes(name);

  if (thread) {
    native_handle = thread->native_handle();
  } else {
    native_handle = ::GetCurrentThread();
  }

  return SUCCEEDED(::SetThreadDescription(native_handle, wname.c_str()));
#elif defined(__linux__) || defined(__QNX__)

  if (thread) {
    native_handle = thread->native_handle();
  } else {
    native_handle = ::pthread_self();
  }

  return ::pthread_setname_np(native_handle, name.c_str()) == 0;
#else
  (void)name;
  (void)thread;
  (void)native_handle;
  // ::fprintf(stderr, "Not support pthread_setname_np.\n");
  // ::fflush(stderr);

  return false;
#endif
}

bool set_thread_priority(int priority_level, int policy, std::thread* thread) noexcept {
  std::thread::native_handle_type native_handle;
#ifdef _MSC_VER
  (void)policy;

  if (thread) {
    native_handle = thread->native_handle();
  } else {
    native_handle = ::GetCurrentThread();
  }

  return ::SetThreadPriority(native_handle, priority_level) != FALSE;
#elif defined(__linux__) || defined(__QNX__) || defined(__APPLE__)

  if (thread) {
    native_handle = thread->native_handle();
  } else {
    native_handle = ::pthread_self();
  }

  sched_param sch;
  int default_policy = 0;

  ::pthread_getschedparam(native_handle, &default_policy, &sch);

  if (policy < 0) {
    policy = default_policy;
  }

  sch.sched_priority = priority_level;

  return ::pthread_setschedparam(native_handle, policy, &sch) == 0;
#else
  (void)priority_level;
  (void)policy;
  (void)thread;
  (void)native_handle;
  // ::fprintf(stderr, "Not support pthread_setschedparam.\n");
  // ::fflush(stderr);

  return false;
#endif
}

bool set_thread_stick(uint32_t core_mask, std::thread* thread) noexcept {
  std::thread::native_handle_type native_handle;

#ifdef _MSC_VER

  if (thread) {
    native_handle = thread->native_handle();
  } else {
    native_handle = ::GetCurrentThread();
  }

  return ::SetThreadAffinityMask(native_handle, core_mask) != 0;
#elif defined(__linux__) && !defined(__ANDROID__)

  if VUNLIKELY (core_mask == 0) {
    return false;
  }

  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);

  bool has_cpu = false;
  for (uint32_t cpu = 0; cpu < 32 && cpu < CPU_SETSIZE; ++cpu) {
    if ((core_mask & (1U << cpu)) != 0) {
      CPU_SET(cpu, &cpuset);
      has_cpu = true;
    }
  }

  if VUNLIKELY (!has_cpu) {
    return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if (thread) {
    native_handle = thread->native_handle();
  } else {
    native_handle = ::pthread_self();
  }

  return ::pthread_setaffinity_np(native_handle, sizeof(cpu_set_t), &cpuset) == 0;
#else
  (void)core_mask;
  (void)thread;
  (void)native_handle;
  // ::fprintf(stderr, "Not support pthread_setaffinity_np.\n");
  // ::fflush(stderr);

  return false;
#endif
}

uint64_t get_native_thread_id() noexcept {
#if defined(_WIN32)
  return static_cast<uint64_t>(::GetCurrentThreadId());
#elif defined(__linux__)
  return static_cast<uint64_t>(::syscall(SYS_gettid));
#elif defined(__FreeBSD__)
  return static_cast<uint64_t>(::pthread_getthreadid_np());
#elif defined(__ANDROID__)
#if defined(__ANDROID_API__) && (__ANDROID_API__ < 21)
#define SYS_gettid __NR_gettid
#endif
  return static_cast<uint64_t>(::syscall(SYS_gettid));
#elif defined(__APPLE__)
  uint64_t tid = 0;

#ifdef MAC_OS_X_VERSION_MAX_ALLOWED
  {
#if (MAC_OS_X_VERSION_MAX_ALLOWED < 1060) || defined(__POWERPC__)
    tid = ::pthread_mach_thread_np(pthread_self());
#elif MAC_OS_X_VERSION_MIN_REQUIRED < 1060

    if (&::pthread_threadid_np) {
      ::pthread_threadid_np(nullptr, &tid);
    } else {
      tid = ::pthread_mach_thread_np(pthread_self());
    }
#else
    ::pthread_threadid_np(nullptr, &tid);
#endif
  }
#else
  ::pthread_threadid_np(nullptr, &tid);
#endif
  return static_cast<size_t>(tid);
#elif defined(__QNX__)
  return ::gettid();
#else
  return static_cast<uint64_t>(std::hash<std::thread::id>()(std::this_thread::get_id()));
#endif
}

// SignalHelper
struct SignalHelper final {
  bool is_async{false};
  bool pass_through{false};
  MoveFunction<void(int)> terminate_callback{nullptr};
  MoveFunction<void(int)> crash_callback{nullptr};

  static SignalHelper& get() {
    static SignalHelper instance;
    return instance;
  }

  static void on_terminate(int signal) {
    static auto& instance = SignalHelper::get();

    if (instance.terminate_callback) {
      if (instance.is_async) {
        std::thread thread([signal]() { instance.terminate_callback(signal); });  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        thread.detach();                                                          // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      } else {                                                                    // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        instance.terminate_callback(signal);
      }

#ifdef __unix__

      if (!instance.pass_through) {
        ::signal(signal, SIG_IGN);
      }
#endif
    }
  }

  // LCOV_EXCL_START GCOVR_EXCL_START
  static void on_crash(int signal) {
    static auto& instance = SignalHelper::get();

    if (instance.crash_callback) {
      instance.crash_callback(signal);
    }
  }
  // LCOV_EXCL_STOP GCOVR_EXCL_STOP

 private:
  ~SignalHelper() = default;
};

void register_terminate_signal(MoveFunction<void(int)>&& callback, bool is_async, bool pass_through) noexcept {
  static auto& instance = SignalHelper::get();

  instance.terminate_callback = std::move(callback);
  instance.is_async = is_async;
  instance.pass_through = pass_through;

#ifdef _WIN32
  static constexpr size_t kTerminateSignals[] = {SIGINT, SIGTERM};

  for (auto signal : kTerminateSignals) {
    ::signal(signal, SignalHelper::on_terminate);
  }
#else

  static constexpr size_t kTerminateSignals[] = {SIGINT, SIGTERM, SIGHUP};

  struct sigaction act{};

#ifdef SA_RESTART
  act.sa_flags = SA_RESTART;
#else
  act.sa_flags = 0;
#endif

  act.sa_handler = SignalHelper::on_terminate;

#ifdef __APPLE__
  act.sa_mask = 0;
#else
  ::sigemptyset(&act.sa_mask);
#endif

  for (auto signal : kTerminateSignals) {
    ::sigaction(signal, nullptr, nullptr);
    ::sigaction(signal, &act, nullptr);
  }
#endif
}

void register_crash_signal(MoveFunction<void(int)>&& callback) noexcept {
  static auto& instance = SignalHelper::get();

  instance.crash_callback = std::move(callback);

#ifdef _WIN32
  static constexpr size_t kCrashSignals[] = {SIGABRT, SIGSEGV, SIGFPE, SIGILL};

  for (auto signal : kCrashSignals) {
    ::signal(signal, SignalHelper::on_crash);
  }
#else

  static constexpr size_t kCrashSignals[] = {SIGABRT, SIGSEGV, SIGFPE, SIGILL, SIGBUS, SIGSYS};

  struct sigaction act{};

  act.sa_flags = 0;
  act.sa_handler = SignalHelper::on_crash;

#ifdef __APPLE__
  act.sa_mask = 0;
#else
  ::sigemptyset(&act.sa_mask);
#endif

  for (auto signal : kCrashSignals) {
    ::sigaction(signal, nullptr, nullptr);
    ::sigaction(signal, &act, nullptr);
  }
#endif
}

// KeyboardHelper
struct KeyboardHelper final {
  std::atomic_bool quit_flag{false};
  std::atomic_bool has_detect{false};

  std::mutex mtx;
  ConditionVariable cv;
  std::thread thread;
  MoveFunction<void(const std::string& key)> callback;

  static KeyboardHelper& get() {
    static KeyboardHelper instance;
    return instance;
  }

 private:
  KeyboardHelper() = default;
};

void start_detect_keyboard(MoveFunction<void(const std::string& key)>&& callback, int poll_ms) noexcept {
  static auto& instance = KeyboardHelper::get();

  if (instance.has_detect.load(std::memory_order_acquire)) {
    return;
  }

  if (callback) {
    instance.callback = std::move(callback);
  }

  if VUNLIKELY (!instance.callback) {
    return;
  }

#ifndef _WIN32
  termios last_ttystate{};

  if VUNLIKELY (::tcgetattr(STDIN_FILENO, &last_ttystate) != 0) {
    return;
  }

  termios tmp_ttystate = last_ttystate;
  tmp_ttystate.c_lflag &= ~(ICANON | ECHO);
  tmp_ttystate.c_cc[VTIME] = 0;
  tmp_ttystate.c_cc[VMIN] = 1;

  if VUNLIKELY (::tcsetattr(STDIN_FILENO, TCSANOW, &tmp_ttystate) != 0) {
    return;
  }

  int flags = fcntl(STDIN_FILENO, F_GETFL, 0);

  if VUNLIKELY (flags == -1 || ::fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) == -1) {
    ::tcsetattr(STDIN_FILENO, TCSANOW, &last_ttystate);
    return;
  }
#endif

  instance.quit_flag.store(false, std::memory_order_release);

#ifdef _WIN32
  auto detector = [poll_ms]() {
#else
  auto detector = [poll_ms, last_ttystate, flags]() {
#endif
    std::unique_lock lock(instance.mtx);

#ifdef _WIN32
    HANDLE hconsole = ::GetStdHandle(STD_INPUT_HANDLE);

    DWORD hmode = 0;
    DWORD last_hmode = 0;

    ::GetConsoleMode(hconsole, &hmode);
    last_hmode = hmode;

    hmode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
    ::SetConsoleMode(hconsole, hmode);

    while (!instance.quit_flag.load(std::memory_order_acquire)) {
      INPUT_RECORD input_record[128];
      DWORD events = 0;

      auto wait_ret = ::WaitForSingleObject(hconsole, poll_ms / 2);

      if VLIKELY (wait_ret != WAIT_OBJECT_0) {
        instance.cv.wait_for(lock, std::chrono::milliseconds(poll_ms / 2));
        continue;
      }

      if (::ReadConsoleInput(hconsole, input_record, 128, &events)) {
        for (DWORD i = 0; i < events; ++i) {
          if (input_record[i].EventType == KEY_EVENT && input_record[i].Event.KeyEvent.bKeyDown) {
            std::string key;
            auto& event = input_record[i].Event.KeyEvent;

            if (event.uChar.AsciiChar >= 'A' && event.uChar.AsciiChar <= 'Z') {
              key = std::string(1, std::tolower(event.uChar.AsciiChar));
            } else {
              switch (event.wVirtualKeyCode) {
                case VK_UP:
                  key = "up";
                  break;
                case VK_DOWN:
                  key = "down";
                  break;
                case VK_LEFT:
                  key = "left";
                  break;
                case VK_RIGHT:
                  key = "right";
                  break;
                case VK_HOME:
                  key = "home";
                  break;
                case VK_END:
                  key = "end";
                  break;
                case VK_PRIOR:
                  key = "pgup";
                  break;
                case VK_NEXT:
                  key = "pgdown";
                  break;
                case VK_ESCAPE:
                  key = "esc";
                  break;
                case VK_BACK:
                  key = "backspace";
                  break;
                default: {
                  char ascii_char = event.uChar.AsciiChar;
                  if (std::isprint(ascii_char)) {
                    key = std::string(1, std::tolower(ascii_char));
                  } else if (ascii_char == '\n' || ascii_char == '\r') {
                    key = "enter";
                  }
                  break;
                }
              }
            }

            if (!key.empty()) {
              instance.callback(key);
            }
          }
        }
      }
    }

    ::SetConsoleMode(hconsole, last_hmode);

    ::FlushConsoleInputBuffer(hconsole);

#else
    int pending_escape_polls = 0;
    std::string pending_input;
    pending_input.reserve(64);

    while (!instance.quit_flag.load(std::memory_order_acquire)) {
      char buffer[64] = {0};
      ssize_t size =
          ::read(STDIN_FILENO, &buffer, sizeof(buffer));  // NOLINT(clang-analyzer-unix.BlockInCriticalSection)

      if VUNLIKELY (size > 0) {
        pending_escape_polls = 0;
        pending_input.append(buffer, static_cast<size_t>(size));

        size_t index = 0;

        while (index < pending_input.size()) {
          if (pending_input[index] == '\033') {
            if (index + 1 >= pending_input.size()) {
              break;
            }

            if (pending_input[index + 1] == 'O') {
              if (index + 2 >= pending_input.size()) {
                break;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
              }

              switch (pending_input[index + 2]) {
                case 'A':
                  instance.callback("up");
                  break;
                case 'B':
                  instance.callback("down");
                  break;
                case 'C':
                  instance.callback("right");
                  break;
                case 'D':
                  instance.callback("left");
                  break;
                case 'H':
                  instance.callback("home");
                  break;
                case 'F':
                  instance.callback("end");
                  break;
                default:  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                  break;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
              }

              index += 3;

              continue;
            }

            if (pending_input[index + 1] == '[') {
              size_t seq_end = index + 2;

              while (seq_end < pending_input.size() &&
                     !(pending_input[seq_end] >= '@' && pending_input[seq_end] <= '~')) {
                ++seq_end;
              }

              if (seq_end >= pending_input.size()) {
                break;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
              }

              const char key = pending_input[seq_end];

              if (key >= 'A' && key <= 'Z') {
                switch (key) {
                  case 'A':
                    instance.callback("up");
                    break;
                  case 'B':
                    instance.callback("down");
                    break;
                  case 'C':
                    instance.callback("right");
                    break;
                  case 'D':
                    instance.callback("left");
                    break;
                  case 'H':
                    instance.callback("home");
                    break;
                  case 'F':
                    instance.callback("end");
                    break;
                  default:  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                    break;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                }
                index = seq_end + 1;
                continue;
              }

              if (key == '~') {
                const std::string code = pending_input.substr(index + 2, seq_end - (index + 2));

                if (code == "1" || code == "7") {
                  instance.callback("home");
                } else if (code == "4" || code == "8") {
                  instance.callback("end");
                } else if (code == "5") {
                  instance.callback("pgup");
                } else if (code == "6") {
                  instance.callback("pgdown");
                }

                index = seq_end + 1;

                continue;
              }

              index = seq_end + 1;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

              continue;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
            }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

            instance.callback("esc");
            ++index;

            continue;
          }

          const auto ch = static_cast<unsigned char>(pending_input[index]);

          if (ch == '\n' || ch == '\r') {
            instance.callback("enter");
          } else if (ch == 0x7f || ch == 0x08) {
            instance.callback("backspace");
          } else if (std::isprint(ch)) {
            instance.callback(std::string(1, static_cast<char>(std::tolower(ch))));
          }

          ++index;
        }

        if (index > 0) {
          pending_input.erase(0, index);
        }
      } else if (pending_input == "\033") {
        ++pending_escape_polls;

        if (pending_escape_polls >= 2) {
          instance.callback("esc");
          pending_input.clear();
          pending_escape_polls = 0;
        }
      }

      instance.cv.wait_for(lock, std::chrono::milliseconds(poll_ms));
    }

    ::fcntl(STDIN_FILENO, F_SETFL, flags);
    ::tcsetattr(STDIN_FILENO, TCSANOW, &last_ttystate);
#endif
  };

  try {
    instance.thread = std::thread(std::move(detector));
  } catch (...) {  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
#ifndef _WIN32
    ::fcntl(STDIN_FILENO, F_SETFL, flags);               // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    ::tcsetattr(STDIN_FILENO, TCSANOW, &last_ttystate);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
#endif
    return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  instance.has_detect.store(true, std::memory_order_release);
}

void stop_detect_keyboard() noexcept {
  static auto& instance = KeyboardHelper::get();

  bool expected = true;

  if VUNLIKELY (!instance.has_detect.compare_exchange_strong(expected, false, std::memory_order_acq_rel,
                                                             std::memory_order_relaxed)) {
    return;
  }

  {
    std::lock_guard lock(instance.mtx);
    instance.quit_flag.store(true, std::memory_order_release);
  }

  instance.cv.notify_all();

  if VLIKELY (instance.thread.joinable()) {
    instance.thread.join();
  }
}

std::pair<int, int> get_terminal_size() noexcept {
#ifdef _WIN32
  CONSOLE_SCREEN_BUFFER_INFO size;

  if VLIKELY (::GetConsoleScreenBufferInfo(::GetStdHandle(STD_OUTPUT_HANDLE), &size)) {
    return std::pair<int, int>{size.srWindow.Right - size.srWindow.Left + 1,
                               size.srWindow.Bottom - size.srWindow.Top + 1};
  }
#else
  struct winsize size;

  if VLIKELY (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0 && size.ws_row > 0) {
    return std::pair<int, int>{size.ws_col, size.ws_row};  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if VLIKELY (::ioctl(STDERR_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0 && size.ws_row > 0) {
    return std::pair<int, int>{size.ws_col, size.ws_row};  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if VLIKELY (::ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0 && size.ws_row > 0) {
    return std::pair<int, int>{size.ws_col, size.ws_row};  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  {
    int fd = ::open("/dev/tty", O_RDONLY);

    if VLIKELY (fd >= 0) {
      // LCOV_EXCL_START GCOVR_EXCL_START
      if VLIKELY (::ioctl(fd, TIOCGWINSZ, &size) == 0 && size.ws_col > 0 && size.ws_row > 0) {
        ::close(fd);
        return std::pair<int, int>{size.ws_col, size.ws_row};
      }

      ::close(fd);
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    }
  }
#endif

  return std::pair<int, int>{-1, -1};
}

double get_cpu_usage() noexcept {
  static std::mutex sample_mtx;
  std::lock_guard sample_lock(sample_mtx);

#ifdef _WIN32
  FILETIME idle_time;
  FILETIME kernel_time;
  FILETIME user_time;
  GetSystemTimes(&idle_time, &kernel_time, &user_time);

  ULARGE_INTEGER idle;
  ULARGE_INTEGER kernel;
  ULARGE_INTEGER user;
  idle.LowPart = idle_time.dwLowDateTime;
  idle.HighPart = idle_time.dwHighDateTime;
  kernel.LowPart = kernel_time.dwLowDateTime;
  kernel.HighPart = kernel_time.dwHighDateTime;
  user.LowPart = user_time.dwLowDateTime;
  user.HighPart = user_time.dwHighDateTime;

  static ULARGE_INTEGER last_idle_time = {{0, 0}};
  static ULARGE_INTEGER last_kernel_time = {{0, 0}};
  static ULARGE_INTEGER last_user_time = {{0, 0}};

  ULARGE_INTEGER total;
  total.QuadPart = (kernel.QuadPart - last_kernel_time.QuadPart) + (user.QuadPart - last_user_time.QuadPart);
  ULARGE_INTEGER idle_diff;
  idle_diff.QuadPart = idle.QuadPart - last_idle_time.QuadPart;

  last_idle_time = idle;
  last_kernel_time = kernel;
  last_user_time = user;

  if VUNLIKELY (total.QuadPart == 0) {
    return -1;
  }

  return (1.0 - (static_cast<double>(idle_diff.QuadPart) / total.QuadPart)) * 100.0;
#elif defined(__linux__)
  std::ifstream cpu_file("/proc/stat");
  std::string line;
  std::getline(cpu_file, line);

  thread_local std::istringstream iss;
  iss.clear();
  iss.str(line);

  std::string cpu;
  uint64_t user = 0;
  uint64_t nice = 0;
  uint64_t system = 0;
  uint64_t idle = 0;
  uint64_t io_wait = 0;
  uint64_t irq = 0;
  uint64_t soft_irq = 0;
  uint64_t steal = 0;

  iss >> cpu >> user >> nice >> system >> idle;

  if VUNLIKELY (!iss || cpu != "cpu") {
    return -1;
  }

  iss >> io_wait >> irq >> soft_irq >> steal;

  static uint64_t last_idle = 0;
  static uint64_t last_total = 0;

  uint64_t idle_all = idle + io_wait;
  uint64_t total = user + nice + system + idle_all + irq + soft_irq + steal;

  if VUNLIKELY (idle_all < last_idle || total < last_total) {
    last_idle = idle_all;
    last_total = total;
    return -1;
  }

  uint64_t idle_diff = idle_all - last_idle;
  uint64_t total_diff = total - last_total;

  last_idle = idle_all;
  last_total = total;

  if VUNLIKELY (total_diff == 0 || idle_diff > total_diff) {
    return -1;
  }

  return (1.0 - static_cast<double>(idle_diff) / total_diff) * 100.0;
#elif defined(__APPLE__)
  host_cpu_load_info_data_t cpu_info;
  mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
  kern_return_t kr = host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t)&cpu_info, &count);

  if VUNLIKELY (kr != KERN_SUCCESS) {
    return -1;
  }

  static uint64_t last_user = 0;
  static uint64_t last_nice = 0;
  static uint64_t last_system = 0;
  static uint64_t last_idle = 0;

  uint64_t user = cpu_info.cpu_ticks[CPU_STATE_USER];
  uint64_t nice = cpu_info.cpu_ticks[CPU_STATE_NICE];
  uint64_t system = cpu_info.cpu_ticks[CPU_STATE_SYSTEM];
  uint64_t idle = cpu_info.cpu_ticks[CPU_STATE_IDLE];

  uint64_t total = (user - last_user) + (nice - last_nice) + (system - last_system) + (idle - last_idle);
  uint64_t idle_diff = idle - last_idle;

  last_user = user;
  last_nice = nice;
  last_system = system;
  last_idle = idle;

  if VUNLIKELY (total == 0) {
    return -1;
  }

  return (1.0 - static_cast<double>(idle_diff) / total) * 100.0;
#else
  return -1;
#endif
}

double get_memory_usage() noexcept {
#ifdef _WIN32
  MEMORYSTATUSEX mem_status;
  mem_status.dwLength = sizeof(MEMORYSTATUSEX);
  GlobalMemoryStatusEx(&mem_status);

  if VUNLIKELY (mem_status.ullTotalPhys == 0) {
    return -1;
  }

  return (static_cast<double>(mem_status.ullTotalPhys - mem_status.ullAvailPhys) / mem_status.ullTotalPhys) * 100.0;
#elif defined(__linux__)
  std::ifstream mem_file("/proc/meminfo");

  if (!mem_file.is_open()) {
    return -1;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  std::string key;
  int64_t mem_total = 0;
  int64_t mem_free = 0;
  int64_t mem_available = 0;
  int64_t buffers = 0;
  int64_t cached = 0;
  bool has_mem_available = false;

  while (mem_file >> key) {
    if (key == "MemTotal:") {
      mem_file >> mem_total;
    } else if (key == "MemAvailable:") {
      mem_file >> mem_available;
      has_mem_available = true;
    } else if (key == "MemFree:") {
      mem_file >> mem_free;
    } else if (key == "Buffers:") {
      mem_file >> buffers;
    } else if (key == "Cached:") {
      mem_file >> cached;
    }

    mem_file.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
  }

  if VUNLIKELY (mem_total == 0) {
    return -1;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if (!has_mem_available) {
    mem_available = mem_free + buffers + cached;
  }

  return (static_cast<double>(mem_total - mem_available) / mem_total) * 100.0;
#elif defined(__APPLE__)
  vm_size_t page_size;
  mach_port_t mach_port = mach_host_self();
  mach_msg_type_number_t count = HOST_VM_INFO_COUNT;
  vm_statistics_data_t vm_stats;

  if VUNLIKELY (host_page_size(mach_port, &page_size) != KERN_SUCCESS) {
    return -1;
  }

  if VUNLIKELY (host_statistics(mach_port, HOST_VM_INFO, (host_info_t)&vm_stats, &count) != KERN_SUCCESS) {
    return -1;
  }

  uint64_t free_memory = static_cast<uint64_t>(vm_stats.free_count) * page_size;
  uint64_t active_memory = static_cast<uint64_t>(vm_stats.active_count) * page_size;
  uint64_t inactive_memory = static_cast<uint64_t>(vm_stats.inactive_count) * page_size;
  uint64_t wired_memory = static_cast<uint64_t>(vm_stats.wire_count) * page_size;
  uint64_t total_memory = free_memory + active_memory + inactive_memory + wired_memory;

  if VUNLIKELY (total_memory == 0) {
    return -1;
  }

  return (static_cast<double>(active_memory + inactive_memory + wired_memory) / total_memory) * 100.0;
#else
  return -1;
#endif
}

bool is_process_running(const std::string& process_name) noexcept {
  if VUNLIKELY (process_name.empty()) {
    return false;
  }

#ifdef _WIN32
  HANDLE hsnapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

  if VUNLIKELY (hsnapshot == INVALID_HANDLE_VALUE) {
    return false;
  }

  PROCESSENTRY32 pe32;
  pe32.dwSize = sizeof(PROCESSENTRY32);

  if (::Process32First(hsnapshot, &pe32)) {
    do {
      if (process_name == pe32.szExeFile) {
        ::CloseHandle(hsnapshot);
        return true;
      }
    } while (::Process32Next(hsnapshot, &pe32));
  }

  ::CloseHandle(hsnapshot);
  return false;
#else
  const bool safe_name = std::all_of(process_name.begin(), process_name.end(), [](unsigned char c) {
    return std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.';
  });

  if VUNLIKELY (!safe_name) {
    return false;
  }

  std::string process_pattern;
  process_pattern.reserve(process_name.size() + 4U);

  for (char c : process_name) {
    if (c == '.') {
      process_pattern.append("\\.");
    } else {
      process_pattern.push_back(c);
    }
  }

  std::ostringstream oss;

#if defined(__QNX__)
  oss << "pidin | grep -- '" << process_pattern << "' | grep -v grep > /dev/null";
#elif defined(__ANDROID__)
  oss << "ps -A | grep -- '" << process_pattern << "' | grep -v grep > /dev/null";
#elif defined(__APPLE__)
  oss << "pgrep -x -- '" << process_pattern << "' > /dev/null 2>&1";
#elif defined(__linux__)
  oss << "pgrep -x -- '" << process_pattern << "' > /dev/null 2>&1";
#else
  oss << "ps aux | grep -- '" << process_pattern << "' | grep -v grep > /dev/null";
#endif

  std::string command = oss.str();

  // NOLINTNEXTLINE(bugprone-command-processor)
  return std::system(command.c_str()) == 0;
#endif
}

int32_t get_timezone_diff() noexcept {
#ifdef _WIN32
  TIME_ZONE_INFORMATION tzi;
  DWORD result = GetTimeZoneInformation(&tzi);
  int bias = tzi.Bias;

  if (result == TIME_ZONE_ID_DAYLIGHT) {
    bias += tzi.DaylightBias;
  } else if (result == TIME_ZONE_ID_STANDARD) {
    bias += tzi.StandardBias;
  }

  return -bias;
#else
  std::time_t now = std::time(nullptr);
  std::tm local_tm = *std::localtime(&now);
#ifdef __APPLE__
  return local_tm.tm_gmtoff / 60;
#else
  std::tm gm_tm = *std::gmtime(&now);
  int diff_seconds = std::mktime(&local_tm) - std::mktime(&gm_tm);

  return diff_seconds / 60;
#endif
#endif
}

void try_release_sys_memory() noexcept {
#if defined(__linux__) && !defined(__ANDROID__)
  malloc_trim(0);
#endif
}

std::string get_machine_id() noexcept {
#ifdef _WIN32
  HKEY hkey;
  const char* subkey = "SOFTWARE\\Microsoft\\Cryptography";
  const char* value_name = "MachineGuid";

  char value[256];
  DWORD value_length = sizeof(value);
  LONG result;

  result = ::RegOpenKeyExA(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ | KEY_WOW64_64KEY, &hkey);

  if (result != ERROR_SUCCESS) {
    return "";
  }

  result = ::RegQueryValueExA(hkey, value_name, nullptr, nullptr, reinterpret_cast<LPBYTE>(value), &value_length);
  ::RegCloseKey(hkey);

  if (result == ERROR_SUCCESS) {
    return std::string(value, value_length - 1);
  }
#elif defined(__linux__)
  const char* paths[] = {
      "/etc/machine-id",
      "/var/lib/dbus/machine-id",
  };

  std::string id;

  for (const char* path : paths) {
    try {
      if (!std::filesystem::exists(path)) {
        // LCOV_EXCL_START GCOVR_EXCL_START
        continue;
      }
    } catch (std::filesystem::filesystem_error&) {
      continue;
    }
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP

    std::ifstream file(path);

    if (file) {
      std::getline(file, id);

      id.erase(std::remove_if(id.begin(), id.end(), ::isspace), id.end());

      if (!id.empty()) {
        return id;
      }
    }
  }
#endif

  return std::string();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
}

}  // namespace Utils

}  // namespace vlink
