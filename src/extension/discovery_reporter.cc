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

#include "./extension/discovery_reporter.h"

#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "./base/cpu_profiler.h"
#include "./base/helpers.h"
#include "./base/logger.h"
#include "./base/utils.h"
#include "./impl/node_impl.h"
#include "./impl/types.h"
#include "./version.h"

#if __has_include(<unistd.h>)
#include <unistd.h>
#endif

#ifdef _WIN32
#include <Winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#define VLINK_DISCOVERY_MULTICAST 1
#define VLINK_DISCOVERY_OFFLINE 0

namespace vlink {

#ifdef _WIN32
using SocketHandle = SOCKET;
static constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
static constexpr SocketHandle kInvalidSocket = -1;
#endif

[[maybe_unused]] static constexpr int kReportFirstInterval = 100;
[[maybe_unused]] static constexpr int kReportInterval = 500;
[[maybe_unused]] static constexpr size_t kMaxTaskSize = 10000U;
[[maybe_unused]] static constexpr uint32_t kMaxElapsedTime = 1000;
[[maybe_unused]] static constexpr int kBroadcastSendPort = 51694;
[[maybe_unused]] static constexpr int kSendTTL = 3;
[[maybe_unused]] static constexpr int kMaxMtuSize = 1450;

#if VLINK_DISCOVERY_MULTICAST
[[maybe_unused]] static constexpr const char* kBroadcastAddress = "239.255.0.100";
#else
[[maybe_unused]] static constexpr const char* kBroadcastAddress = "255.255.255.255";
#endif

template <typename T>
[[maybe_unused]] static std::string_view convert_type(const T& t) {
  switch (t) {
    case kServer:
      return "Ser";
    case kClient:
      return "Cli";
    case kPublisher:
      return "Pub";
    case kSubscriber:
      return "Sub";
    case kSetter:
      return "Set";
    case kGetter:
      return "Get";
    default:         // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      return "Unk";  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }
}

// DiscoveryReporter::Impl
struct DiscoveryReporter::Impl final {
  std::unordered_set<NodeImpl*> info_set;
  std::mutex mtx;
  std::vector<std::string> message_list;
  Timer timer;
  std::string runtime_version;
  std::string local_message;
  bool is_profiler_enabled{false};
  int64_t seq{0};

  SocketHandle sock{kInvalidSocket};
  sockaddr_in address;
  bool enable_native_discovery{false};
#ifdef _WIN32
  bool winsock_initialized{false};
#endif
};

DiscoveryReporter::DiscoveryReporter() : impl_(std::make_unique<Impl>()) {
  set_name("DiscoveryReporter");

  const std::string native_discovery = Utils::get_env("VLINK_DISCOVER_NATIVE");

  if (native_discovery == "1") {
    impl_->enable_native_discovery = true;
  }

  impl_->runtime_version = Version{VLINK_VERSION_MAJOR, VLINK_VERSION_MINOR, VLINK_VERSION_PATCH}.to_string();

  impl_->local_message =
      Helpers::escape_field(get_host_name()) + ":" + Utils::get_pid_str() + ":" + Helpers::escape_field(get_app_name());

  impl_->is_profiler_enabled = CpuProfiler::is_global_enabled();

#ifdef _WIN32
  ::WSADATA wsa_data;

  if VUNLIKELY (::WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    VLOG_F("DiscoveryReporter: Failed to initialize winsock.");
    return;
  }

  impl_->winsock_initialized = true;
#endif
  impl_->sock = ::socket(AF_INET, SOCK_DGRAM, 0);

  if VUNLIKELY (impl_->sock == kInvalidSocket) {
    VLOG_F("DiscoveryReporter: Failed to create socket.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return;                                                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

#ifdef IP_TTL

  if VUNLIKELY (::setsockopt(impl_->sock, IPPROTO_IP, IP_TTL, reinterpret_cast<const char*>(&kSendTTL),
                             sizeof(kSendTTL)) < 0) {
    VLOG_F("DiscoveryReporter: Failed to set TTL option.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return;                                                  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }
#endif

#if !VLINK_DISCOVERY_MULTICAST
  int enable_broadcast = 1;

  if VUNLIKELY (::setsockopt(impl_->sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&enable_broadcast),
                             sizeof(enable_broadcast)) < 0) {
    VLOG_F("DiscoveryReporter: Failed to enable broadcast.");
    return;
  }
#endif

  std::memset(&impl_->address, 0, sizeof(impl_->address));

  impl_->address.sin_family = AF_INET;
  impl_->address.sin_addr.s_addr = inet_addr(kBroadcastAddress);

  if (impl_->enable_native_discovery) {
    struct in_addr local_interface;
    local_interface.s_addr = inet_addr("127.0.0.1");

    if VUNLIKELY (::setsockopt(impl_->sock, IPPROTO_IP, IP_MULTICAST_IF,
                               reinterpret_cast<const char*>(&local_interface), sizeof(local_interface)) < 0) {
      VLOG_F("DiscoveryReporter: Failed to set multicast interface to loopback.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      return;                                                                       // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
  }

  impl_->address.sin_port = htons(kBroadcastSendPort);

  impl_->timer.set_interval(kReportFirstInterval);
  impl_->timer.set_loop_count(Timer::kInfinite);
  impl_->timer.attach(this);
  impl_->timer.start([this]() {
    send_report();

    impl_->timer.set_interval(kReportInterval);
  });

  post_task([this]() { send_report(); });
}

DiscoveryReporter::~DiscoveryReporter() {
#ifdef _WIN32
  if (Utils::is_terminating()) {
    (void)impl_.release();
    return;
  }
#endif

  impl_->timer.stop();

  quit(true);

  wait_for_quit();

#if VLINK_DISCOVERY_OFFLINE
  send_offline();
#endif

  if VLIKELY (impl_->sock != kInvalidSocket) {
#ifdef _WIN32
    ::closesocket(impl_->sock);
#else
    ::close(impl_->sock);
#endif

    impl_->sock = kInvalidSocket;
  }

#ifdef _WIN32
  if VLIKELY (impl_->winsock_initialized) {
    ::WSACleanup();
    impl_->winsock_initialized = false;
  }
#endif
}

void DiscoveryReporter::add(NodeImpl* node) {
  std::lock_guard lock(impl_->mtx);
  impl_->info_set.emplace(node);

  if (!impl_->is_profiler_enabled) {
    rebuild_message();
  }
}

void DiscoveryReporter::remove(NodeImpl* node) {
  std::lock_guard lock(impl_->mtx);
  impl_->info_set.erase(node);

  if (!impl_->is_profiler_enabled) {
    rebuild_message();
  }
}

size_t DiscoveryReporter::get_max_task_count() const { return kMaxTaskSize; }

uint32_t DiscoveryReporter::get_max_elapsed_time() const { return kMaxElapsedTime; }

void DiscoveryReporter::on_begin() { MessageLoop::on_begin(); }

void DiscoveryReporter::on_end() { MessageLoop::on_end(); }

void DiscoveryReporter::rebuild_message() {
  impl_->message_list.clear();

  std::string message_pack;
  message_pack.reserve(kMaxMtuSize);

  std::map<std::tuple<int, std::string, std::string, SchemaType>, std::pair<bool, double>> profiler_value_map;

  if (impl_->is_profiler_enabled) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    for (auto* node : impl_->info_set) {
      const std::string& trim_url = Helpers::trim_string(node->url);
      const std::string& trim_ser_type = Helpers::trim_string(node->ser_type);

      auto& value =
          profiler_value_map[std::make_tuple(node->impl_type, trim_url, trim_ser_type, node->schema_type)].second;

      if (node->profiler) {
        value += node->profiler->restart();
      }
    }
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  for (auto* node : impl_->info_set) {
    const std::string& trim_url = Helpers::trim_string(node->url);
    const std::string& trim_ser_type = Helpers::trim_string(node->ser_type);

    if VUNLIKELY (trim_url.empty()) {
      continue;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if VUNLIKELY (trim_url.size() > 300) {
      VLOG_F("DiscoveryReporter: Url is too long [", trim_url, "].");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      return;                                                          // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if VUNLIKELY (trim_ser_type.size() > 300) {
      VLOG_F("DiscoveryReporter: Ser type is too long [", trim_ser_type, "].");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      return;                                                                    // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    std::string message;
    message.append(convert_type(node->impl_type)).append(" ");

    auto& profiler_result =
        profiler_value_map[std::make_tuple(node->impl_type, trim_url, trim_ser_type, node->schema_type)];

    if (profiler_result.first) {
      continue;
    } else {
      profiler_result.first = true;

      const std::string escaped_url = Helpers::escape_field(trim_url);
      const std::string escaped_ser_type = trim_ser_type.empty() ? "{}" : Helpers::escape_field(trim_ser_type);

      message.append(escaped_url).append(" ");
      message.append(escaped_ser_type).append(" ");
      message.append(std::to_string(static_cast<uint32_t>(node->schema_type))).append(" ");
      message.append(impl_->local_message);

      if (impl_->is_profiler_enabled) {
        message.append(":" + Helpers::double_to_string(profiler_result.second, 4));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      message.append("\n");
    }

    if VUNLIKELY (message.size() > kMaxMtuSize) {
      VLOG_F("DiscoveryReporter: Discovery message is too long [", trim_url, "].");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      return;                                                                        // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if VUNLIKELY (message_pack.size() + message.size() > kMaxMtuSize) {
      if VLIKELY (!message_pack.empty()) {               // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        impl_->message_list.emplace_back(message_pack);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      message_pack.clear();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    message_pack.append(message);
  }

  if (!message_pack.empty()) {
    impl_->message_list.emplace_back(std::move(message_pack));
  }
}

void DiscoveryReporter::send_report() {
  std::lock_guard lock(impl_->mtx);

  if (impl_->is_profiler_enabled) {
    rebuild_message();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  for (const auto& message : impl_->message_list) {
    if VUNLIKELY (::sendto(impl_->sock, message.c_str(), message.length(), 0,
                           reinterpret_cast<struct sockaddr*>(&impl_->address), sizeof(impl_->address)) < 0) {
      // VLOG_W("Failed to send broadcast message.");
      Utils::yield_cpu();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      continue;            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
  }

  ++impl_->seq;
}

// LCOV_EXCL_START GCOVR_EXCL_START
void DiscoveryReporter::send_offline() {
  std::lock_guard lock(impl_->mtx);

  static std::string offline_message = "offline\n" + impl_->local_message;

  if VUNLIKELY (::sendto(impl_->sock, offline_message.c_str(), offline_message.length(), 0,
                         reinterpret_cast<struct sockaddr*>(&impl_->address), sizeof(impl_->address)) < 0) {
    // VLOG_W("Failed to send broadcast message.");
    Utils::yield_cpu();
  }
}
// LCOV_EXCL_STOP GCOVR_EXCL_STOP

const std::string& DiscoveryReporter::get_host_name() {
  static std::string host_name = [] {
    std::string name = Utils::get_host_name();

    if (name.size() > 50) {
      name.resize(50);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    return name;
  }();

  return host_name;
}

const std::string& DiscoveryReporter::get_app_name() {
  static std::string app_name = [] {
    std::string name = Utils::get_app_name();

    if (name.size() > 50) {
      name.resize(50);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    return name;
  }();

  return app_name;
}

}  // namespace vlink
