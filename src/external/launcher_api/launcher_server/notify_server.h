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

#pragma once

#include <vlink/base/functional.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace vlink::launcher {

struct NotifyEvent final {
  std::string unit_name;
  uint64_t generation{0};
  bool ready{false};
  bool reloading{false};
  bool stopping{false};
  bool watchdog_ping{false};
  bool watchdog_trigger{false};
  bool has_health{false};
  bool healthy{true};
  bool has_status{false};
  std::string status;
  bool has_extend_timeout{false};
  uint32_t extend_timeout_ms{0};
};

class NotifyServer final {
 public:
  using EventCallback = MoveFunction<void(NotifyEvent&&)>;

  NotifyServer() = default;
  ~NotifyServer();

  NotifyServer(const NotifyServer&) = delete;
  NotifyServer& operator=(const NotifyServer&) = delete;

  bool start(const std::string& preferred_dir, EventCallback callback);
  void stop();

  [[nodiscard]] bool is_running() const noexcept;
  [[nodiscard]] std::string address() const;

  [[nodiscard]] std::string register_unit(const std::string& unit_name, uint64_t generation);
  void unregister_unit(const std::string& unit_name, std::optional<uint64_t> expected_gen = std::nullopt);
  void clear_units();

 private:
  struct TokenEntry final {
    std::string unit_name;
    uint64_t generation{0};
  };

  void stop_locked();
  void recv_loop();
  void handle_datagram(std::string_view payload);

  std::mutex lifecycle_mtx_;
  mutable std::mutex mtx_;
  EventCallback callback_;
  std::map<std::string, TokenEntry, std::less<>> token_to_unit_;
  std::unordered_map<std::string, std::string> unit_to_token_;
  std::string address_;
  std::string uds_path_;
  std::thread recv_thread_;
  bool active_{false};
  std::atomic_bool running_{false};
  std::atomic_bool stopping_{false};

#if defined(_WIN32) || defined(_WIN64)
  uintptr_t sock_fd_{~static_cast<uintptr_t>(0U)};
  uint16_t udp_port_{0};
  bool wsa_initialized_{false};
#else
  int sock_fd_{-1};
  int wake_fds_[2]{-1, -1};
#endif
};

}  // namespace vlink::launcher
