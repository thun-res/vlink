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

#include <vlink/base/macros.h>
#include <vlink/external/launcher_notify.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

#if defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
//
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace vlink {

static constexpr const char* kEnvNotifyAddr = "VLINK_LAUNCHER_NOTIFY_ADDR";
static constexpr const char* kEnvNotifyToken = "VLINK_LAUNCHER_NOTIFY_TOKEN";
static constexpr const char* kEnvUnitName = "VLINK_LAUNCHER_UNIT_NAME";
static constexpr const char* kEnvUnitGen = "VLINK_LAUNCHER_UNIT_GEN";
static constexpr const char* kEnvWatchdogMs = "VLINK_LAUNCHER_WATCHDOG_MS";

static constexpr size_t kMaxDatagramBytes = 4096U;
static constexpr size_t kMaxStatusBytes = 1024U;

#if defined(_WIN32) || defined(_WIN64)
static bool ensure_winsock_ready() noexcept {
  struct WinsockInit final {
    bool ok{false};
    WinsockInit() noexcept {
      WSADATA wsa{};
      ok = (::WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    }
  };

  static WinsockInit init;
  return init.ok;
}

[[nodiscard]] static bool utf8_to_wide(const char* src, std::wstring& out) noexcept {
  out.clear();

  if VUNLIKELY (src == nullptr || src[0] == '\0') {
    return false;
  }

  const int needed = ::MultiByteToWideChar(CP_UTF8, 0, src, -1, nullptr, 0);

  if (needed <= 1) {
    return false;
  }

  try {
    out.resize(static_cast<size_t>(needed));

    if (::MultiByteToWideChar(CP_UTF8, 0, src, -1, out.data(), needed) != needed) {
      out.clear();
      return false;
    }

    out.pop_back();
    return true;
  } catch (...) {
    out.clear();
    return false;
  }
}

[[nodiscard]] static bool wide_to_utf8(const wchar_t* src, int wlen, std::string& out) noexcept {
  out.clear();

  if (wlen <= 0 || src == nullptr) {
    return false;
  }

  const int needed = ::WideCharToMultiByte(CP_UTF8, 0, src, wlen, nullptr, 0, nullptr, nullptr);

  if (needed <= 0) {
    return false;
  }

  try {
    out.resize(static_cast<size_t>(needed));
    return ::WideCharToMultiByte(CP_UTF8, 0, src, wlen, out.data(), needed, nullptr, nullptr) == needed;
  } catch (...) {
    out.clear();
    return false;
  }
}
#endif

[[nodiscard]] static bool read_env_str(const char* name, std::string& out) noexcept {
  out.clear();

  if VUNLIKELY (name == nullptr || name[0] == '\0') {
    return false;
  }

#if defined(_WIN32) || defined(_WIN64)
  std::wstring wname;

  if (!utf8_to_wide(name, wname)) {
    return false;
  }

  wchar_t stack_buf[512];
  const DWORD len = ::GetEnvironmentVariableW(wname.c_str(), stack_buf, static_cast<DWORD>(512));

  if (len == 0U) {
    return false;
  }

  if (len < 512U) {
    return wide_to_utf8(stack_buf, static_cast<int>(len), out);
  }

  try {
    std::wstring wval(static_cast<size_t>(len), L'\0');
    const DWORD actual = ::GetEnvironmentVariableW(wname.c_str(), wval.data(), len);

    if (actual == 0U || actual >= len) {
      return false;
    }

    return wide_to_utf8(wval.data(), static_cast<int>(actual), out);
  } catch (...) {
    out.clear();
    return false;
  }
#else
  const char* val = std::getenv(name);

  if (val == nullptr || val[0] == '\0') {
    return false;
  }

  try {
    out.assign(val);
    return true;
  } catch (...) {
    out.clear();
    return false;
  }
#endif
}

[[nodiscard]] static bool has_non_empty_env(const char* name) noexcept {
  if VUNLIKELY (name == nullptr || name[0] == '\0') {
    return false;
  }

#if defined(_WIN32) || defined(_WIN64)
  std::wstring wname;

  if (!utf8_to_wide(name, wname)) {
    return false;
  }

  wchar_t probe[2];
  return ::GetEnvironmentVariableW(wname.c_str(), probe, 2U) > 0U;
#else
  const char* val = std::getenv(name);
  return val != nullptr && val[0] != '\0';
#endif
}

[[nodiscard]] static bool parse_u64_strict(std::string_view text, uint64_t& out) noexcept {
  if VUNLIKELY (text.empty()) {
    return false;
  }

  uint64_t val = 0U;
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto res = std::from_chars(begin, end, val, 10);

  if (res.ec != std::errc() || res.ptr != end) {
    return false;
  }

  out = val;
  return true;
}

[[nodiscard]] static std::string sanitize_single_line(std::string_view text) {
  const size_t limit = std::min(text.size(), kMaxStatusBytes);
  std::string clean;
  clean.reserve(limit);

  for (size_t i = 0U; i < limit; ++i) {
    const char ch = text[i];

    if (ch == '\r' || ch == '\n' || ch == '\0') {
      clean.push_back(' ');
    } else {
      clean.push_back(ch);
    }
  }

  return clean;
}

[[nodiscard]] static bool parse_udp_endpoint(std::string_view addr, std::string& host_out,
                                             uint16_t& port_out) noexcept {
  constexpr std::string_view kPrefix = "udp:";

  if (addr.size() <= kPrefix.size() || addr.substr(0U, kPrefix.size()) != kPrefix) {
    return false;
  }

  const std::string_view rest = addr.substr(kPrefix.size());
  const size_t colon = rest.rfind(':');

  if (colon == std::string_view::npos || colon == 0U || colon + 1U >= rest.size()) {
    return false;
  }

  uint64_t port = 0U;

  if (!parse_u64_strict(rest.substr(colon + 1U), port) || port == 0U || port > 65535U) {
    return false;
  }

  try {
    host_out.assign(rest.substr(0U, colon));
  } catch (...) {
    return false;
  }

  port_out = static_cast<uint16_t>(port);
  return true;
}

[[nodiscard]] static bool send_udp_datagram(const std::string& host, uint16_t port, const char* data,
                                            size_t len) noexcept {
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);

  if (::inet_pton(AF_INET, host.c_str(), &sa.sin_addr) != 1) {
    return false;
  }

#if defined(_WIN32) || defined(_WIN64)
  if VUNLIKELY (!ensure_winsock_ready()) {
    return false;
  }

  const SOCKET sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

  if (sock == INVALID_SOCKET) {
    return false;
  }

  u_long non_blocking = 1U;
  (void)::ioctlsocket(sock, FIONBIO, &non_blocking);

  const int sent = ::sendto(sock, data, static_cast<int>(len), 0, reinterpret_cast<const sockaddr*>(&sa),
                            static_cast<int>(sizeof(sa)));
  ::closesocket(sock);
  return sent == static_cast<int>(len);
#else
  int sock_type = SOCK_DGRAM;
#ifdef SOCK_CLOEXEC
  sock_type |= SOCK_CLOEXEC;
#endif
#ifdef SOCK_NONBLOCK
  sock_type |= SOCK_NONBLOCK;
#endif
  const int fd = ::socket(AF_INET, sock_type, 0);

  if (fd < 0) {
    return false;
  }

#ifndef SOCK_CLOEXEC
  const int fd_flags = ::fcntl(fd, F_GETFD, 0);

  if (fd_flags >= 0) {
    (void)::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
  }
#endif
#ifndef SOCK_NONBLOCK
  const int fl = ::fcntl(fd, F_GETFL, 0);

  if (fl >= 0) {
    (void)::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  }
#endif
#if defined(SO_NOSIGPIPE) && !defined(MSG_NOSIGNAL)
  const int nosig = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosig, static_cast<socklen_t>(sizeof(nosig)));
#endif

  int send_flags = 0;
#ifdef MSG_DONTWAIT
  send_flags |= MSG_DONTWAIT;
#endif
#ifdef MSG_NOSIGNAL
  send_flags |= MSG_NOSIGNAL;
#endif

  ssize_t sent = -1;

  for (int attempt = 0; attempt < 3; ++attempt) {
    sent =
        ::sendto(fd, data, len, send_flags, reinterpret_cast<const sockaddr*>(&sa), static_cast<socklen_t>(sizeof(sa)));

    if (sent >= 0 || errno != EINTR) {
      break;
    }
  }

  ::close(fd);
  return sent == static_cast<ssize_t>(len);
#endif
}

#if !defined(_WIN32) && !defined(_WIN64)
[[nodiscard]] static bool send_unix_datagram(std::string_view path, const char* data, size_t len) noexcept {
  sockaddr_un sa{};

  if VUNLIKELY (path.empty() || path.size() >= sizeof(sa.sun_path)) {
    return false;
  }

  sa.sun_family = AF_UNIX;
  std::memcpy(sa.sun_path, path.data(), path.size());
  sa.sun_path[path.size()] = '\0';

  int sock_type = SOCK_DGRAM;
#ifdef SOCK_CLOEXEC
  sock_type |= SOCK_CLOEXEC;
#endif
#ifdef SOCK_NONBLOCK
  sock_type |= SOCK_NONBLOCK;
#endif
  const int fd = ::socket(AF_UNIX, sock_type, 0);

  if (fd < 0) {
    return false;
  }

#ifndef SOCK_CLOEXEC
  const int fd_flags = ::fcntl(fd, F_GETFD, 0);

  if (fd_flags >= 0) {
    (void)::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
  }
#endif
#ifndef SOCK_NONBLOCK
  const int fl = ::fcntl(fd, F_GETFL, 0);

  if (fl >= 0) {
    (void)::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  }
#endif
#if defined(SO_NOSIGPIPE) && !defined(MSG_NOSIGNAL)
  const int nosig = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosig, static_cast<socklen_t>(sizeof(nosig)));
#endif

  int send_flags = 0;
#ifdef MSG_DONTWAIT
  send_flags |= MSG_DONTWAIT;
#endif
#ifdef MSG_NOSIGNAL
  send_flags |= MSG_NOSIGNAL;
#endif

  ssize_t sent = -1;

  for (int attempt = 0; attempt < 3; ++attempt) {
    sent =
        ::sendto(fd, data, len, send_flags, reinterpret_cast<const sockaddr*>(&sa), static_cast<socklen_t>(sizeof(sa)));

    if (sent >= 0 || errno != EINTR) {
      break;
    }
  }

  ::close(fd);
  return sent == static_cast<ssize_t>(len);
}
#endif

bool LauncherNotify::is_managed() noexcept {
  return has_non_empty_env(kEnvNotifyAddr) && has_non_empty_env(kEnvNotifyToken);
}

std::string LauncherNotify::unit_name() {
  if (!is_managed()) {
    return {};
  }

  std::string name;
  (void)read_env_str(kEnvUnitName, name);
  return name;
}

uint64_t LauncherNotify::unit_generation() noexcept {
  std::string raw;

  if (!read_env_str(kEnvUnitGen, raw)) {
    return 0U;
  }

  uint64_t gen = 0U;
  return parse_u64_strict(raw, gen) ? gen : 0U;
}

uint32_t LauncherNotify::watchdog_interval_ms() noexcept {
  std::string raw;

  if (!read_env_str(kEnvWatchdogMs, raw)) {
    return 0U;
  }

  uint64_t ms = 0U;

  if (!parse_u64_strict(raw, ms) || ms > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
    return 0U;
  }

  return static_cast<uint32_t>(ms);
}

bool LauncherNotify::notify(std::string_view state) {
  if VUNLIKELY (state.empty()) {
    return false;
  }

  std::string addr;
  std::string token;

  if (!read_env_str(kEnvNotifyAddr, addr) || !read_env_str(kEnvNotifyToken, token)) {
    return false;
  }

  constexpr std::string_view kTokenPrefix = "TOKEN=";
  const size_t total_len = kTokenPrefix.size() + token.size() + 1U + state.size();

  if VUNLIKELY (total_len > kMaxDatagramBytes) {
    return false;
  }

  std::array<char, kMaxDatagramBytes> buf;
  size_t pos = 0U;
  std::copy_n(kTokenPrefix.data(), kTokenPrefix.size(), buf.data() + pos);
  pos += kTokenPrefix.size();
  std::copy_n(token.data(), token.size(), buf.data() + pos);
  pos += token.size();
  buf[pos++] = '\n';
  std::copy_n(state.data(), state.size(), buf.data() + pos);
  pos += state.size();

#if !defined(_WIN32) && !defined(_WIN64)
  constexpr std::string_view kUnixPrefix = "unix:";

  if VLIKELY (addr.size() > kUnixPrefix.size() &&
              std::string_view(addr).substr(0U, kUnixPrefix.size()) == kUnixPrefix) {
    return send_unix_datagram(std::string_view(addr).substr(kUnixPrefix.size()), buf.data(), pos);
  }
#endif

  std::string host;
  uint16_t port = 0U;

  if (parse_udp_endpoint(addr, host, port)) {
    return send_udp_datagram(host, port, buf.data(), pos);
  }

  return false;
}

bool LauncherNotify::notify_ready(std::string_view status) {
  if (!is_managed()) {
    return false;
  }

  if (status.empty()) {
    return notify("READY=1\n");
  }

  std::string payload = "READY=1\nSTATUS=";
  payload.append(sanitize_single_line(status));
  payload.push_back('\n');
  return notify(payload);
}

bool LauncherNotify::notify_reloading(std::string_view status) {
  if (!is_managed()) {
    return false;
  }

  if (status.empty()) {
    return notify("RELOADING=1\n");
  }

  std::string payload = "RELOADING=1\nSTATUS=";
  payload.append(sanitize_single_line(status));
  payload.push_back('\n');
  return notify(payload);
}

bool LauncherNotify::notify_stopping(std::string_view status) {
  if (!is_managed()) {
    return false;
  }

  if (status.empty()) {
    return notify("STOPPING=1\n");
  }

  std::string payload = "STOPPING=1\nSTATUS=";
  payload.append(sanitize_single_line(status));
  payload.push_back('\n');
  return notify(payload);
}

bool LauncherNotify::notify_status(std::string_view status) {
  if (!is_managed()) {
    return false;
  }

  std::string payload = "STATUS=";
  payload.append(sanitize_single_line(status));
  payload.push_back('\n');
  return notify(payload);
}

bool LauncherNotify::notify_health(bool healthy, std::string_view status) {
  if (!is_managed()) {
    return false;
  }

  std::string payload = healthy ? "HEALTH=1\n" : "HEALTH=0\n";

  if (!status.empty()) {
    payload.append("STATUS=");
    payload.append(sanitize_single_line(status));
    payload.push_back('\n');
  }

  return notify(payload);
}

bool LauncherNotify::notify_watchdog() {
  if (!is_managed()) {
    return false;
  }

  return notify("WATCHDOG=1\n");
}

bool LauncherNotify::notify_watchdog_trigger(std::string_view reason) {
  if (!is_managed()) {
    return false;
  }

  if (reason.empty()) {
    return notify("WATCHDOG=trigger\n");
  }

  std::string payload = "WATCHDOG=trigger\nSTATUS=";
  payload.append(sanitize_single_line(reason));
  payload.push_back('\n');
  return notify(payload);
}

bool LauncherNotify::notify_extend_timeout(uint32_t extra_ms) {
  if (extra_ms == 0U || !is_managed()) {
    return false;
  }

  std::string payload = "EXTEND_TIMEOUT_MS=";
  payload.append(std::to_string(extra_ms));
  payload.push_back('\n');
  return notify(payload);
}

}  // namespace vlink
