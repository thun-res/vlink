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

#include "notify_server.h"

#include <vlink/base/logger.h>
#include <vlink/base/macros.h>
#include <vlink/base/utils.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <utility>

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
//
#include <wincrypt.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace vlink::launcher {

static constexpr size_t kMaxDatagramBytes = 4096U;
static constexpr size_t kMaxStatusBytes = 1024U;
static constexpr uint32_t kMinExtendTimeoutMs = 100U;
static constexpr uint32_t kMaxExtendTimeoutMs = 600000U;

static std::string generate_token_hex(size_t byte_count) {
  std::array<uint8_t, 32> buf{};
  const size_t n = std::min(byte_count, buf.size());
  bool filled = false;

#if defined(_WIN32) || defined(_WIN64)
  HCRYPTPROV prov = 0;

  if (::CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) != FALSE) {
    if (::CryptGenRandom(prov, static_cast<DWORD>(n), buf.data()) != FALSE) {
      filled = true;
    }

    ::CryptReleaseContext(prov, 0);
  }
#else
  const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);

  if (fd >= 0) {
    size_t total = 0U;

    while (total < n) {
      const ssize_t got = ::read(fd, buf.data() + total, n - total);

      if (got > 0) {
        total += static_cast<size_t>(got);
      } else if (got < 0 && errno == EINTR) {
        continue;
      } else {
        break;
      }
    }

    ::close(fd);
    filled = (total == n);
  }
#endif

  if VUNLIKELY (!filled) {
    static std::atomic<uint64_t> fallback_seq{1U};
    const auto now_ns = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const uint64_t seq = fallback_seq.fetch_add(1U, std::memory_order_relaxed);
    std::random_device rd;
    std::seed_seq seed{rd(),
                       rd(),
                       static_cast<uint32_t>(now_ns),
                       static_cast<uint32_t>(now_ns >> 32U),
                       static_cast<uint32_t>(seq),
                       static_cast<uint32_t>(seq >> 32U)};
    std::mt19937_64 rng(seed);

    for (size_t i = 0U; i < n; i += 8U) {
      const uint64_t word = rng();
      const size_t chunk = std::min<size_t>(8U, n - i);
      std::memcpy(buf.data() + i, &word, chunk);
    }
  }

  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(n * 2U);

  for (size_t i = 0U; i < n; ++i) {
    out.push_back(kHex[(buf[i] >> 4U) & 0x0FU]);
    out.push_back(kHex[buf[i] & 0x0FU]);
  }

  return out;
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

[[nodiscard]] static std::string sanitize_status_text(std::string_view text) {
  const size_t limit = std::min(text.size(), kMaxStatusBytes);
  std::string clean;
  clean.reserve(limit);

  for (size_t i = 0U; i < limit; ++i) {
    const auto uch = static_cast<unsigned char>(text[i]);

    if (uch < 0x20U || uch == 0x7fU) {
      clean.push_back(' ');
    } else {
      clean.push_back(static_cast<char>(uch));
    }
  }

  return clean;
}

#if !defined(_WIN32) && !defined(_WIN64)
static constexpr size_t kSafeUnixPathMax = 96U;

static void set_fd_flags(int fd, bool non_blocking) noexcept {
  const int fd_flags = ::fcntl(fd, F_GETFD, 0);

  if (fd_flags >= 0) {
    (void)::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
  }

  if (non_blocking) {
    const int fl = ::fcntl(fd, F_GETFL, 0);

    if (fl >= 0) {
      (void)::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
  }
}

[[nodiscard]] static int try_bind_unix_dgram(const std::string& path) noexcept {
  sockaddr_un sa{};

  if (path.empty() || path.size() >= sizeof(sa.sun_path) || path.size() > kSafeUnixPathMax) {
    return -1;
  }

  const int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);

  if (fd < 0) {
    return -1;
  }

  set_fd_flags(fd, true);
  (void)::unlink(path.c_str());

  sa.sun_family = AF_UNIX;
  std::memcpy(sa.sun_path, path.data(), path.size());
  sa.sun_path[path.size()] = '\0';

  if (::bind(fd, reinterpret_cast<const sockaddr*>(&sa), static_cast<socklen_t>(sizeof(sa))) != 0) {
    ::close(fd);
    return -1;
  }

  (void)::chmod(path.c_str(), S_IRUSR | S_IWUSR);
  return fd;
}

[[nodiscard]] static int try_bind_udp_loopback(uint16_t& port_out) noexcept {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);

  if (fd < 0) {
    return -1;
  }

  set_fd_flags(fd, true);

  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = 0U;
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (::bind(fd, reinterpret_cast<const sockaddr*>(&sa), static_cast<socklen_t>(sizeof(sa))) != 0) {
    ::close(fd);
    return -1;
  }

  auto slen = static_cast<socklen_t>(sizeof(sa));

  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &slen) != 0) {
    ::close(fd);
    return -1;
  }

  port_out = ntohs(sa.sin_port);
  return fd;
}
#endif

NotifyServer::~NotifyServer() { stop(); }

bool NotifyServer::start(const std::string& preferred_dir, EventCallback callback) {
  std::lock_guard lifecycle_lock(lifecycle_mtx_);
  stop_locked();

  std::string bound_addr;
  std::string bound_uds;

#if defined(_WIN32) || defined(_WIN64)
  (void)preferred_dir;
  WSADATA wsa{};

  if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    VLOG_W("Launcher NotifyServer: WSAStartup failed.");
    return false;
  }

  const SOCKET sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

  if (sock == INVALID_SOCKET) {
    (void)::WSACleanup();
    VLOG_W("Launcher NotifyServer: Failed to create UDP loopback socket.");
    return false;
  }

  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = 0U;
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (::bind(sock, reinterpret_cast<const sockaddr*>(&sa), static_cast<int>(sizeof(sa))) != 0) {
    ::closesocket(sock);
    (void)::WSACleanup();
    VLOG_W("Launcher NotifyServer: Failed to bind UDP loopback socket.");
    return false;
  }

  int slen = static_cast<int>(sizeof(sa));

  if (::getsockname(sock, reinterpret_cast<sockaddr*>(&sa), &slen) != 0) {
    ::closesocket(sock);
    (void)::WSACleanup();
    return false;
  }

  const uint16_t port = ntohs(sa.sin_port);
  u_long non_blocking = 1U;
  (void)::ioctlsocket(sock, FIONBIO, &non_blocking);

  sock_fd_ = static_cast<uintptr_t>(sock);
  udp_port_ = port;
  wsa_initialized_ = true;
  bound_addr = "udp:127.0.0.1:" + std::to_string(port);
#else
  if (::pipe(wake_fds_) != 0) {
    VLOG_W("Launcher NotifyServer: Failed to create wake pipe.");
    return false;
  }

  set_fd_flags(wake_fds_[0], true);
  set_fd_flags(wake_fds_[1], true);

  int fd = -1;

  if (!preferred_dir.empty()) {
    const std::string candidate = preferred_dir + "/notify.sock";
    fd = try_bind_unix_dgram(candidate);

    if (fd >= 0) {
      bound_uds = candidate;
      bound_addr = "unix:" + candidate;
    }
  }

  if (fd < 0) {
    const std::string tmp_dir = Utils::get_tmp_dir();
    const std::string suffix = generate_token_hex(4U);
    const std::string candidate = tmp_dir + "/vln_" + Utils::get_pid_str() + "_" + suffix + ".sock";
    fd = try_bind_unix_dgram(candidate);

    if (fd >= 0) {
      bound_uds = candidate;
      bound_addr = "unix:" + candidate;
    }
  }

  if (fd < 0) {
    uint16_t port = 0U;
    fd = try_bind_udp_loopback(port);

    if (fd >= 0) {
      bound_addr = "udp:127.0.0.1:" + std::to_string(port);
      VLOG_W("Launcher NotifyServer: Falling back to loopback UDP notify socket on ", bound_addr);
    }
  }

  if (fd < 0) {
    ::close(wake_fds_[0]);
    ::close(wake_fds_[1]);
    wake_fds_[0] = -1;
    wake_fds_[1] = -1;
    VLOG_W("Launcher NotifyServer: Failed to bind Unix or loopback UDP notify socket.");
    return false;
  }

  sock_fd_ = fd;
#endif

  {
    std::lock_guard lock(mtx_);
    callback_ = std::move(callback);
    address_ = std::move(bound_addr);
    uds_path_ = std::move(bound_uds);
    token_to_unit_.clear();
    unit_to_token_.clear();
    active_ = true;
  }

  stopping_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);

  try {
    recv_thread_ = std::thread([this] { recv_loop(); });
  } catch (...) {
    stop_locked();
    VLOG_W("Launcher NotifyServer: Failed to spawn recv thread.");
    return false;
  }

  return true;
}

void NotifyServer::stop() {
  std::lock_guard lifecycle_lock(lifecycle_mtx_);
  stop_locked();
}

void NotifyServer::stop_locked() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }

  stopping_.store(true, std::memory_order_release);

#if defined(_WIN32) || defined(_WIN64)
  if (udp_port_ != 0U) {
    const SOCKET wake = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (wake != INVALID_SOCKET) {
      sockaddr_in sa{};
      sa.sin_family = AF_INET;
      sa.sin_port = htons(udp_port_);
      sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      const char byte = 0;
      (void)::sendto(wake, &byte, 1, 0, reinterpret_cast<const sockaddr*>(&sa), static_cast<int>(sizeof(sa)));
      ::closesocket(wake);
    }
  }
#else
  if (wake_fds_[1] >= 0) {
    const char byte = 1;
    (void)::write(wake_fds_[1], &byte, 1U);
  }
#endif

  if (recv_thread_.joinable()) {
    recv_thread_.join();
  }

  std::string path_to_unlink;

  {
    std::lock_guard lock(mtx_);
    active_ = false;
    callback_ = nullptr;
    address_.clear();
    path_to_unlink = std::move(uds_path_);
    uds_path_.clear();
    token_to_unit_.clear();
    unit_to_token_.clear();
  }

#if defined(_WIN32) || defined(_WIN64)
  if (sock_fd_ != ~static_cast<uintptr_t>(0U)) {
    ::closesocket(static_cast<SOCKET>(sock_fd_));
    sock_fd_ = ~static_cast<uintptr_t>(0U);
  }

  udp_port_ = 0U;

  if (wsa_initialized_) {
    (void)::WSACleanup();
    wsa_initialized_ = false;
  }
#else
  if (sock_fd_ >= 0) {
    ::close(sock_fd_);
    sock_fd_ = -1;
  }

  if (wake_fds_[0] >= 0) {
    ::close(wake_fds_[0]);
    wake_fds_[0] = -1;
  }

  if (wake_fds_[1] >= 0) {
    ::close(wake_fds_[1]);
    wake_fds_[1] = -1;
  }

  if (!path_to_unlink.empty()) {
    (void)::unlink(path_to_unlink.c_str());
  }
#endif
}

bool NotifyServer::is_running() const noexcept { return running_.load(std::memory_order_acquire); }

std::string NotifyServer::address() const {
  std::lock_guard lock(mtx_);
  return address_;
}

std::string NotifyServer::register_unit(const std::string& unit_name, uint64_t generation) {
  if VUNLIKELY (unit_name.empty() || !running_.load(std::memory_order_acquire)) {
    return {};
  }

  std::string token = generate_token_hex(16U);

  {
    std::lock_guard lock(mtx_);

    if VUNLIKELY (!active_) {
      return {};
    }

    auto old_it = unit_to_token_.find(unit_name);

    if (old_it != unit_to_token_.end()) {
      token_to_unit_.erase(old_it->second);
    }

    while (token_to_unit_.count(token) != 0U) {
      token = generate_token_hex(16U);
    }

    unit_to_token_[unit_name] = token;
    token_to_unit_[token] = TokenEntry{unit_name, generation};
  }

  return token;
}

void NotifyServer::unregister_unit(const std::string& unit_name, std::optional<uint64_t> expected_gen) {
  std::lock_guard lock(mtx_);
  auto uit = unit_to_token_.find(unit_name);

  if (uit == unit_to_token_.end()) {
    return;
  }

  if (expected_gen.has_value()) {
    auto tit = token_to_unit_.find(uit->second);

    if (tit != token_to_unit_.end() && tit->second.generation != *expected_gen) {
      return;
    }
  }

  token_to_unit_.erase(uit->second);
  unit_to_token_.erase(uit);
}

void NotifyServer::clear_units() {
  std::lock_guard lock(mtx_);
  token_to_unit_.clear();
  unit_to_token_.clear();
}

void NotifyServer::recv_loop() {
  char buf[kMaxDatagramBytes];

#if defined(_WIN32) || defined(_WIN64)
  const auto sock = static_cast<SOCKET>(sock_fd_);

  while (!stopping_.load(std::memory_order_acquire)) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(sock, &read_fds);

    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 200000;

    const int ready = ::select(0, &read_fds, nullptr, nullptr, &tv);

    if (ready <= 0) {
      continue;
    }

    while (!stopping_.load(std::memory_order_acquire)) {
      const int n = ::recvfrom(sock, buf, static_cast<int>(sizeof(buf)), 0, nullptr, nullptr);

      if (n <= 0) {
        break;
      }

      handle_datagram(std::string_view(buf, static_cast<size_t>(n)));
    }
  }
#else
  while (!stopping_.load(std::memory_order_acquire)) {
    pollfd pfds[2]{};
    pfds[0].fd = sock_fd_;
    pfds[0].events = POLLIN;
    pfds[1].fd = wake_fds_[0];
    pfds[1].events = POLLIN;

    const int rc = ::poll(pfds, 2, -1);

    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }

      break;
    }

    if VUNLIKELY (((pfds[0].revents | pfds[1].revents) & POLLNVAL) != 0) {
      break;
    }

    if ((pfds[1].revents & POLLIN) != 0) {
      char drain[64];

      while (::read(wake_fds_[0], drain, sizeof(drain)) > 0) {
      }

      if (stopping_.load(std::memory_order_acquire)) {
        break;
      }
    } else if VUNLIKELY ((pfds[1].revents & (POLLERR | POLLHUP)) != 0) {
      break;
    }

    if VUNLIKELY ((pfds[0].revents & POLLERR) != 0 && (pfds[0].revents & POLLIN) == 0) {
      break;
    }

    if ((pfds[0].revents & POLLIN) != 0) {
      while (!stopping_.load(std::memory_order_acquire)) {
        const ssize_t n = ::recvfrom(sock_fd_, buf, sizeof(buf), 0, nullptr, nullptr);

        if (n < 0) {
          if (errno == EINTR) {
            continue;
          }

          break;
        }

        if (n == 0) {
          continue;
        }

        handle_datagram(std::string_view(buf, static_cast<size_t>(n)));
      }
    }
  }
#endif
}

void NotifyServer::handle_datagram(std::string_view payload) {
  if VUNLIKELY (payload.empty()) {
    return;
  }

  std::string_view token;
  NotifyEvent ev;
  bool has_action = false;

  size_t pos = 0U;

  while (pos < payload.size()) {
    size_t nl = payload.find('\n', pos);

    if (nl == std::string_view::npos) {
      nl = payload.size();
    }

    std::string_view line = payload.substr(pos, nl - pos);
    pos = nl + 1U;

    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1U);
    }

    if (line.empty()) {
      continue;
    }

    const size_t eq = line.find('=');

    if (eq == std::string_view::npos || eq == 0U) {
      continue;
    }

    const std::string_view key = line.substr(0U, eq);
    const std::string_view val = line.substr(eq + 1U);

    if (key == "TOKEN") {
      if (token.empty()) {
        token = val;
      }
    } else if (key == "READY") {
      if (val == "1") {
        ev.ready = true;
        has_action = true;
      }
    } else if (key == "RELOADING") {
      if (val == "1") {
        ev.reloading = true;
        has_action = true;
      }
    } else if (key == "STOPPING") {
      if (val == "1") {
        ev.stopping = true;
        has_action = true;
      }
    } else if (key == "WATCHDOG") {
      if (val == "1") {
        ev.watchdog_ping = true;
        has_action = true;
      } else if (val == "trigger") {
        ev.watchdog_trigger = true;
        has_action = true;
      }
    } else if (key == "HEALTH") {
      if (val == "1" || val == "true") {
        ev.has_health = true;
        ev.healthy = true;
        has_action = true;
      } else if (val == "0" || val == "false") {
        ev.has_health = true;
        ev.healthy = false;
        has_action = true;
      }
    } else if (key == "STATUS") {
      ev.has_status = true;
      ev.status = sanitize_status_text(val);
      has_action = true;
    } else if (key == "EXTEND_TIMEOUT_MS") {
      uint64_t ms = 0U;

      if (parse_u64_strict(val, ms) && ms > 0U) {
        ev.has_extend_timeout = true;
        ev.extend_timeout_ms =
            static_cast<uint32_t>(std::clamp<uint64_t>(ms, kMinExtendTimeoutMs, kMaxExtendTimeoutMs));
        has_action = true;
      }
    } else if (key == "EXTEND_TIMEOUT_USEC") {
      uint64_t us = 0U;

      if (parse_u64_strict(val, us) && us > 0U) {
        const uint64_t ms =
            (us > (std::numeric_limits<uint64_t>::max() - 999U)) ? kMaxExtendTimeoutMs : ((us + 999U) / 1000U);
        ev.has_extend_timeout = true;
        ev.extend_timeout_ms =
            static_cast<uint32_t>(std::clamp<uint64_t>(ms, kMinExtendTimeoutMs, kMaxExtendTimeoutMs));
        has_action = true;
      }
    }
  }

  if VUNLIKELY (token.empty() || !has_action) {
    return;
  }

  {
    std::lock_guard lock(mtx_);
    auto it = token_to_unit_.find(token);

    if VUNLIKELY (it == token_to_unit_.end() || !callback_) {
      return;
    }

    ev.unit_name = it->second.unit_name;
    ev.generation = it->second.generation;
  }

  if VLIKELY (callback_) {
    callback_(std::move(ev));
  }
}

}  // namespace vlink::launcher
