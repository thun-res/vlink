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

#include "./extension/discovery_viewer.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <ctime>
#include <exception>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "./base/bytes.h"
#include "./base/elapsed_timer.h"
#include "./base/helpers.h"
#include "./base/logger.h"
#include "./base/utils.h"
#include "./impl/url.h"

#if __has_include(<unistd.h>)
#include <unistd.h>
#endif

#ifdef _WIN32
#include <Winsock2.h>
#include <ws2tcpip.h>
#else
#ifdef __QNX__
#include <sys/time.h>
#endif
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

[[maybe_unused]] static constexpr int kCollectInterval = 500;
[[maybe_unused]] static constexpr int kReportInterval = 500;
[[maybe_unused]] static constexpr int kReportTimeout = kReportInterval * 4;
[[maybe_unused]] static constexpr size_t kMaxTaskSize = 50000U;
[[maybe_unused]] static constexpr uint32_t kMaxElapsedTime = 1000;
[[maybe_unused]] static constexpr int kBroadcastBindPort = 51694;
[[maybe_unused]] static constexpr size_t kBufferSize = 1024 * 1024U;

#if VLINK_DISCOVERY_MULTICAST
[[maybe_unused]] static constexpr const char* kBroadcastAddress = "239.255.0.100";
#else
[[maybe_unused]] static constexpr const char* kBroadcastAddress = "255.255.255.255";
#endif

[[maybe_unused]] static std::string node_count_to_string(size_t node_count) {
  if (node_count > 9) {
    return std::string("~");
  }

  return std::to_string(node_count);
}

// DiscoveryViewer::Impl
struct DiscoveryViewer::Impl final {
  struct Comparator final {
    bool operator()(const DiscoveryViewer::Info& lhs, const DiscoveryViewer::Info& rhs) const {
      if (lhs.sort_index < rhs.sort_index) {
        return true;
      } else if (lhs.sort_index > rhs.sort_index) {
        return false;
      }

      if (lhs.url < rhs.url) {
        return true;
      } else if (lhs.url > rhs.url) {
        return false;
      }

      if (lhs.type < rhs.type) {
        return true;
      } else if (lhs.type > rhs.type) {
        return false;
      }

      if (lhs.schema_type < rhs.schema_type) {
        return true;
      } else if (lhs.schema_type > rhs.schema_type) {
        return false;
      }

      if (lhs.ser_type < rhs.ser_type) {
        return true;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      } else if (lhs.ser_type > rhs.ser_type) {
        return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      return lhs.process_list < rhs.process_list;
    }
  };

  DiscoveryViewer::FilterType filter_type{DiscoveryViewer::kFilterNone};
  std::string native_hostname;

  std::map<DiscoveryViewer::Info, ElapsedTimer, Comparator> info_map;
  std::map<std::pair<int, uint32_t>, std::string> process_map;
  std::unordered_map<std::string, std::string> ser_map;
  std::unordered_map<std::string, SchemaType> schema_type_map;
  std::vector<DiscoveryViewer::Info> info_list;

  std::recursive_mutex mtx;
  std::shared_mutex ser_mtx;
  bool info_dirty{false};

  DiscoveryViewer::Callback callback;
  std::thread thread;
  std::vector<uint8_t> buffer;
  Timer timer;

  SocketHandle sock{kInvalidSocket};
  sockaddr_in address;
  bool enable_native_discovery{false};
#ifdef _WIN32
  bool winsock_initialized{false};
#endif
};

// DiscoveryViewer::Process
bool DiscoveryViewer::Process::operator<(const DiscoveryViewer::Process& target) const noexcept {
  if (type < target.type) {
    return true;
  } else if (type > target.type) {
    return false;
  } else if (host < target.host) {
    return true;
  } else if (host > target.host) {
    return false;
  } else if (ip < target.ip) {
    return true;
  } else if (ip > target.ip) {
    return false;
  } else if (name < target.name) {
    return true;
  } else if (name > target.name) {
    return false;
  } else {
    return pid < target.pid;
  }
}

// DiscoveryViewer::Info
bool DiscoveryViewer::Info::operator<(const DiscoveryViewer::Info& target) const noexcept {
  if (type < target.type) {
    return true;
  } else if (type > target.type) {
    return false;
  } else if (sort_index < target.sort_index) {
    return true;
  } else if (sort_index > target.sort_index) {
    return false;
  } else if (url < target.url) {
    return true;
  } else if (url > target.url) {
    return false;
  } else if (schema_type < target.schema_type) {
    return true;
  } else if (schema_type > target.schema_type) {
    return false;
  } else if (ser_type < target.ser_type) {
    return true;
  } else if (ser_type > target.ser_type) {
    return false;
  } else {
    return process_list < target.process_list;
  }
}

// DiscoveryViewer
ImplType DiscoveryViewer::convert_type(std::string_view str) {
  if (str == "Ser") {
    return kServer;
  } else if (str == "Cli") {
    return kClient;
  } else if (str == "Pub") {
    return kPublisher;
  } else if (str == "Sub") {
    return kSubscriber;
  } else if (str == "Set") {
    return kSetter;
  } else if (str == "Get") {
    return kGetter;
  } else {
    return kUnknownImplType;
  }
}

std::string DiscoveryViewer::convert_type_to_view(uint32_t type) {
  switch (type) {
    case kPublisher | kSubscriber:
      return "Pub|Sub";
    case kSetter | kGetter:
      return "Set|Get";
    case kServer | kClient:
      return "Ser|Cli";
    case kPublisher:
      return "Pub|---";
    case kSubscriber:
      return "---|Sub";
    case kSetter:
      return "Set|---";
    case kGetter:
      return "---|Get";
    case kServer:
      return "Ser|---";
    case kClient:
      return "---|Cli";
    case kPublisher | kGetter:
      return "Pub|Get";
    case kSetter | kSubscriber:
      return "Set|Sub";
    default:
      if (type == (kPublisher | kSetter)) {
        return "Pub|---";
      } else if (type == (kPublisher | kSetter | kSubscriber)) {
        return "Pub|Sub";
      } else if (type == (kPublisher | kSetter | kGetter)) {
        return "Pub|Get";
      } else if (type == (kPublisher | kSetter | kSubscriber | kGetter)) {
        return "Pub|Sub";
      } else if (type == (kSubscriber | kGetter)) {
        return "---|Sub";
      } else if (type == (kSubscriber | kGetter | kPublisher)) {
        return "Pub|Sub";
      } else if (type == (kSubscriber | kGetter | kSetter)) {
        return "Set|Sub";
      } else {
        return "???|???";
      }
  }
}

std::string DiscoveryViewer::convert_type_to_view(uint32_t type, const std::vector<Process>& process_list) {
  int left_cnt = 0;
  int right_cnt = 0;

  switch (type) {
    case kPublisher | kSubscriber:
      for (const auto& process : process_list) {
        if (process.type == kPublisher) {
          ++left_cnt;
        } else if (process.type == kSubscriber) {
          ++right_cnt;
        }
      }

      return "Pub*" + node_count_to_string(left_cnt) + "|Sub*" + node_count_to_string(right_cnt);
    case kSetter | kGetter:
      for (const auto& process : process_list) {
        if (process.type == kSetter) {
          ++left_cnt;
        } else if (process.type == kGetter) {
          ++right_cnt;
        }
      }

      return "Set*" + node_count_to_string(left_cnt) + "|Get*" + node_count_to_string(right_cnt);
    case kServer | kClient:
      for (const auto& process : process_list) {
        if (process.type == kServer) {
          ++left_cnt;
        } else if (process.type == kClient) {
          ++right_cnt;
        }
      }

      return "Ser*" + node_count_to_string(left_cnt) + "|Cli*" + node_count_to_string(right_cnt);
    case kPublisher:
      for (const auto& process : process_list) {
        if (process.type == kPublisher) {
          ++left_cnt;
        }
      }

      return "Pub*" + node_count_to_string(left_cnt) + "|-----";
    case kSubscriber:
      for (const auto& process : process_list) {
        if (process.type == kSubscriber) {
          ++right_cnt;
        }
      }

      return "-----|Sub*" + node_count_to_string(right_cnt);
    case kSetter:
      for (const auto& process : process_list) {
        if (process.type == kSetter) {
          ++left_cnt;
        }
      }

      return "Set*" + node_count_to_string(left_cnt) + "|-----";
    case kGetter:
      for (const auto& process : process_list) {
        if (process.type == kGetter) {
          ++right_cnt;
        }
      }

      return "-----|Get*" + node_count_to_string(right_cnt);
    case kServer:
      for (const auto& process : process_list) {
        if (process.type == kServer) {
          ++left_cnt;
        }
      }

      return "Ser*" + node_count_to_string(left_cnt) + "|-----";
    case kClient:
      for (const auto& process : process_list) {
        if (process.type == kClient) {
          ++right_cnt;
        }
      }

      return "-----|Cli*" + node_count_to_string(right_cnt);

    case kPublisher | kGetter:
      for (const auto& process : process_list) {
        if (process.type == kPublisher) {
          ++left_cnt;
        } else if (process.type == kGetter) {
          ++right_cnt;
        }
      }

      return "Pub*" + node_count_to_string(left_cnt) + "|Get*" + node_count_to_string(right_cnt);

    case kSetter | kSubscriber:
      for (const auto& process : process_list) {
        if (process.type == kSetter) {
          ++left_cnt;
        } else if (process.type == kSubscriber) {
          ++right_cnt;
        }
      }

      return "Set*" + node_count_to_string(left_cnt) + "|Sub*" + node_count_to_string(right_cnt);
    default:
      if (type == (kPublisher | kSetter)) {
        for (const auto& process : process_list) {
          if (process.type == kPublisher || process.type == kSetter) {
            ++left_cnt;
          }
        }

        return "Pub*" + node_count_to_string(left_cnt) + "|-----";
      } else if (type == (kPublisher | kSetter | kSubscriber)) {
        for (const auto& process : process_list) {
          if (process.type == kPublisher || process.type == kSetter) {
            ++left_cnt;
          } else if (process.type == kSubscriber) {
            ++right_cnt;
          }
        }

        return "Pub*" + node_count_to_string(left_cnt) + "|Sub*" + node_count_to_string(right_cnt);
      } else if (type == (kPublisher | kSetter | kGetter)) {
        for (const auto& process : process_list) {
          if (process.type == kPublisher || process.type == kSetter) {
            ++left_cnt;
          } else if (process.type == kGetter) {
            ++right_cnt;
          }
        }

        return "Pub*" + node_count_to_string(left_cnt) + "|Get*" + node_count_to_string(right_cnt);
      } else if (type == (kPublisher | kSetter | kSubscriber | kGetter)) {
        for (const auto& process : process_list) {
          if (process.type == kPublisher || process.type == kSetter) {
            ++left_cnt;
          } else if (process.type == kSubscriber || process.type == kGetter) {
            ++right_cnt;
          }
        }

        return "Pub*" + node_count_to_string(left_cnt) + "|Sub*" + node_count_to_string(right_cnt);
      } else if (type == (kSubscriber | kGetter)) {
        for (const auto& process : process_list) {
          if (process.type == kSubscriber || process.type == kGetter) {
            ++right_cnt;
          }
        }

        return "-----|Sub*" + node_count_to_string(right_cnt);
      } else if (type == (kSubscriber | kGetter | kPublisher)) {
        for (const auto& process : process_list) {
          if (process.type == kPublisher) {
            ++left_cnt;
          } else if (process.type == kSubscriber || process.type == kGetter) {
            ++right_cnt;
          }
        }

        return "Pub*" + node_count_to_string(left_cnt) + "|Sub*" + node_count_to_string(right_cnt);
      } else if (type == (kSubscriber | kGetter | kSetter)) {
        for (const auto& process : process_list) {
          if (process.type == kSetter) {
            ++left_cnt;
          } else if (process.type == kSubscriber || process.type == kGetter) {
            ++right_cnt;
          }
        }

        return "Set*" + node_count_to_string(left_cnt) + "|Sub*" + node_count_to_string(right_cnt);
      } else {
        return "?????|?????";
      }
  }
}

std::string DiscoveryViewer::get_listen_address() { return kBroadcastAddress; }

DiscoveryViewer::DiscoveryViewer(FilterType type) : impl_(std::make_unique<Impl>()) {
  set_name("DiscoveryViewer");

  const std::string native_discovery = Utils::get_env("VLINK_DISCOVER_NATIVE");

  if (native_discovery == "1") {
    impl_->enable_native_discovery = true;
  }

  impl_->ser_map.reserve(128);
  impl_->schema_type_map.reserve(128);

  impl_->filter_type = type;
  impl_->native_hostname = Utils::get_host_name();

#ifdef _WIN32
  ::WSADATA wsa_data;

  if VUNLIKELY (::WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    VLOG_F("DiscoveryViewer: Failed to initialize winsock.");
    return;
  }

  impl_->winsock_initialized = true;
#endif

  // timer
  impl_->timer.set_interval(kCollectInterval);
  impl_->timer.set_loop_count(Timer::kInfinite);
  impl_->timer.attach(this);

  impl_->timer.start([this]() { process_timeout(); });

  // socket init
  impl_->sock = ::socket(AF_INET, SOCK_DGRAM, 0);

  if VUNLIKELY (impl_->sock == kInvalidSocket) {
    VLOG_F("DiscoveryViewer: Failed to create socket.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return;                                               // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

#ifdef _WIN32
  DWORD timeout = 100U;

  if VUNLIKELY (::setsockopt(impl_->sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                             sizeof(timeout)) < 0) {
    VLOG_F("DiscoveryViewer: Failed to set receive timeout.");
    return;
  }
#else
  timeval timeout;
  std::memset(&timeout, 0, sizeof(timeout));
  timeout.tv_sec = 0U;
  timeout.tv_usec = 1000 * 100U;

  if VUNLIKELY (::setsockopt(impl_->sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                             sizeof(timeout)) < 0) {
    VLOG_F("DiscoveryViewer: Failed to set receive timeout.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return;                                                     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }
#endif

  int enable_reuse_addr = 1;

  if VUNLIKELY (::setsockopt(impl_->sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enable_reuse_addr),
                             sizeof(enable_reuse_addr)) < 0) {
    VLOG_F("DiscoveryViewer: Failed to set reuse address option.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return;                                                          // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

#ifdef SO_REUSEPORT
  int enable_reuse_port = 1;

  if VUNLIKELY (::setsockopt(impl_->sock, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&enable_reuse_port),
                             sizeof(enable_reuse_port)) < 0) {
    VLOG_F("DiscoveryViewer: Failed to set reuse port option.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return;                                                       // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }
#endif

  std::memset(&impl_->address, 0, sizeof(impl_->address));
  impl_->address.sin_family = AF_INET;
  impl_->address.sin_port = htons(kBroadcastBindPort);
  impl_->address.sin_addr.s_addr = htonl(INADDR_ANY);

  if VUNLIKELY (::bind(impl_->sock, reinterpret_cast<sockaddr*>(&impl_->address), sizeof(impl_->address)) < 0) {
    VLOG_F("DiscoveryViewer: Failed to bind socket.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return;                                             // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

#if VLINK_DISCOVERY_MULTICAST
  ip_mreq mreq;
  std::memset(&mreq, 0, sizeof(mreq));

  if (impl_->enable_native_discovery) {
    mreq.imr_multiaddr.s_addr = inet_addr(kBroadcastAddress);
    mreq.imr_interface.s_addr = inet_addr("127.0.0.1");

    if VUNLIKELY (::setsockopt(impl_->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq),
                               sizeof(mreq)) < 0) {
      VLOG_F("DiscoveryViewer: Failed to send multicast to 127.0.0.1.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      return;                                                             // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
  } else {
    mreq.imr_multiaddr.s_addr = inet_addr(kBroadcastAddress);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);

    if VUNLIKELY (::setsockopt(impl_->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq),
                               sizeof(mreq)) < 0) {
#ifdef __QNX__
      CLOG_F(
          "DiscoveryViewer: Failed to set multicast, please add address [%s] to target device. "
          "\nExamples(QNX): route add -host %s -interface eth0.",
          kBroadcastAddress, kBroadcastAddress);
#elif defined(__APPLE__)
      CLOG_F(
          "DiscoveryViewer: Failed to set multicast, please add address [%s] to target device. "
          "\nExamples(MACOS): route add -net %s -interface eth0.",
          kBroadcastAddress, kBroadcastAddress);
#else
      CLOG_F(  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          "DiscoveryViewer: Failed to set multicast, please add address [%s] to target device. "
          "\nExamples(Linux): route add %s eth0.",
          kBroadcastAddress, kBroadcastAddress);
#endif
      return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
  }

#else
  int enable_broadcast = 1;

  if VUNLIKELY (::setsockopt(impl_->sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&enable_broadcast),
                             sizeof(enable_broadcast)) < 0) {
    VLOG_F("DiscoveryViewer: Failed to enable broadcast.");
    return;
  }
#endif

  impl_->buffer.resize(kBufferSize);
  std::memset(impl_->buffer.data(), 0, impl_->buffer.size());

  static auto global_ip_set = []() {
    auto list = Utils::get_all_ipv4_address();
    return std::unordered_set<std::string>(list.begin(), list.end());
  }();

  // socket listen
  impl_->thread = std::thread([this]() {
    sockaddr_in target_address;
    socklen_t target_address_len = sizeof(target_address);
    std::memset(&target_address, 0, target_address_len);

    char target_ip[INET_ADDRSTRLEN] = {0};

    for (;;) {
      int size = ::recvfrom(impl_->sock, reinterpret_cast<char*>(impl_->buffer.data()), impl_->buffer.size(), 0,
                            reinterpret_cast<sockaddr*>(&target_address), &target_address_len);

      if VUNLIKELY (is_ready_to_quit()) {
        break;
      }

      if VUNLIKELY (size <= 0 || static_cast<size_t>(size) > impl_->buffer.size()) {
        continue;
      }

      if VUNLIKELY (::inet_ntop(AF_INET, &target_address.sin_addr, target_ip, INET_ADDRSTRLEN) == nullptr) {
        continue;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      if (impl_->filter_type == kFilterNative) {
        if (global_ip_set.count(target_ip) == 0) {
          continue;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }
      }

      std::string message = Bytes::shallow_copy(impl_->buffer.data(), size).to_string();

      if VUNLIKELY (!is_running()) {
        Utils::yield_cpu();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        continue;            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      post_task([this, message, target_ip_str = std::string(target_ip)]() {
#if VLINK_DISCOVERY_OFFLINE
        if VUNLIKELY (Helpers::has_startwith(message, "offline")) {
          auto offline_list_view = Helpers::split_view(message, '\n');

          if VUNLIKELY (offline_list_view.size() < 2) {
            return;
          }

          if VUNLIKELY (offline_list_view.at(0) != "offline") {
            return;
          }

          auto process_view = offline_list_view.at(1);

          auto process_list_view = Helpers::split_view(process_view, ':');

          if VUNLIKELY (process_list_view.size() < 3) {
            return;
          }

          uint32_t process_pid = 0;

          const std::string hostname = Helpers::unescape_field(process_list_view.at(0));

          auto process_pid_view = process_list_view.at(1);

          auto [ptr, error] =
              std::from_chars(process_pid_view.data(), process_pid_view.data() + process_pid_view.size(), process_pid);

          if VUNLIKELY (error != std::errc{}) {
            process_pid = 0;
          }

          const std::string process_name = Helpers::unescape_field(process_list_view.at(2));

          process_offline(hostname, process_pid, process_name);

          return;
        }
#endif

        auto message_list_view = Helpers::split_view(message, '\n');

        if VUNLIKELY (message_list_view.empty()) {
          return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }

        {
          std::lock_guard lock(impl_->mtx);

          for (auto str : message_list_view) {
            auto list = Helpers::split_view(str, ' ');

            if VUNLIKELY (list.size() < 5) {
              continue;
            }

            ImplType type = convert_type(list.at(0));
            auto url_view = list.at(1);
            auto ser_type_view = list.at(2);
            auto schema_type_view = list.at(3);
            auto process_view = list.at(4);

            uint32_t schema_type_num = 0;
            SchemaType schema_type = SchemaType::kUnknown;
            uint32_t process_pid = 0;
            double profiler = -1;

            {
              auto [ptr, error] = std::from_chars(schema_type_view.data(),
                                                  schema_type_view.data() + schema_type_view.size(), schema_type_num);

              if VUNLIKELY (error != std::errc{}) {
                schema_type_num = 0;
              }

              if (error == std::errc{}) {
                auto parsed_schema_type = static_cast<SchemaType>(schema_type_num);

                if (SchemaData::is_valid_type(parsed_schema_type)) {
                  schema_type = parsed_schema_type;
                }
              }
            }

            auto process_list_view = Helpers::split_view(process_view, ':');

            if (process_list_view.size() < 3) {
              continue;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
            }

            std::string url = Helpers::unescape_field(url_view);
            std::string ser_type = Helpers::unescape_field(ser_type_view);
            std::string hostname = Helpers::unescape_field(process_list_view.at(0));

            if (impl_->filter_type == kFilterAvailable) {
              if (hostname != impl_->native_hostname) {
                if (Url::is_local_type(url)) {
                  continue;
                }
              }
            } else if (impl_->filter_type == kFilterNative) {
              if (hostname != impl_->native_hostname) {
                continue;
              }
            }

            auto process_pid_view = process_list_view.at(1);

            {
              auto [ptr, error] = std::from_chars(process_pid_view.data(),
                                                  process_pid_view.data() + process_pid_view.size(), process_pid);

              if VUNLIKELY (error != std::errc{}) {
                process_pid = 0;
              }
            }

            std::string process_name = Helpers::unescape_field(process_list_view.at(2));

            if (process_list_view.size() >= 4) {
              auto profiler_view = process_list_view.at(3);

              try {
                profiler = std::stod(std::string(profiler_view));
              } catch (std::exception&) {
                profiler = -1;
              }
            }

            if VUNLIKELY (type == kUnknownImplType || url.empty()) {
              continue;
            }

            int sort_index = Url::get_sort_index(url);

            if VUNLIKELY (sort_index < 0) {
              continue;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
            }

            if (ser_type == "{}") {
              ser_type.clear();
            }

            Info info{sort_index,  type,
                      url,         ser_type,
                      schema_type, {Process{type, hostname, process_pid, process_name, target_ip_str, profiler}}};

            auto [iter, inserted] = impl_->info_map.try_emplace(std::move(info), ElapsedTimer{});
            iter->second.restart();

            if (!inserted) {
              auto& existing_info = const_cast<Info&>(iter->first);
              existing_info.process_list[0].profiler = profiler;
            }

            impl_->info_dirty = true;

            if (!url.empty()) {
              std::string merged_ser_type;
              SchemaType merged_schema_type = SchemaType::kUnknown;
              bool has_ser_conflict = false;
              bool has_schema_conflict = false;

              for (const auto& [active_info, active_timer] : impl_->info_map) {
                (void)active_timer;

                if (active_info.url != url) {
                  continue;
                }

                if (!active_info.ser_type.empty()) {
                  if (merged_ser_type.empty() || merged_ser_type == "Bytes") {
                    merged_ser_type = active_info.ser_type;
                  } else if (active_info.ser_type != "Bytes" && active_info.ser_type != merged_ser_type) {
                    has_ser_conflict = true;
                  }
                }

                if (has_schema_conflict || active_info.schema_type == SchemaType::kUnknown) {
                  continue;
                }

                if (merged_schema_type == SchemaType::kUnknown) {
                  merged_schema_type = active_info.schema_type;
                } else if (merged_schema_type != active_info.schema_type) {
                  merged_schema_type = SchemaType::kUnknown;
                  has_schema_conflict = true;
                }
              }

              if VUNLIKELY (has_ser_conflict) {
                CLOG_W(
                    "DiscoveryViewer: Different ser: url = %s, current_ser = %s, new_ser = %s, process_name = %s, "
                    "process_pid = %u.",
                    url.c_str(), merged_ser_type.c_str(), ser_type.c_str(), process_name.c_str(), process_pid);
                merged_ser_type.clear();
              }

              std::lock_guard ser_lock(impl_->ser_mtx);

              if (!merged_ser_type.empty()) {
                impl_->ser_map[url] = merged_ser_type;
              } else {
                impl_->ser_map.erase(url);
              }

              impl_->schema_type_map[url] = merged_schema_type;
            }
          }
        }

        report_list();
      });
    }
  });
}

DiscoveryViewer::~DiscoveryViewer() {
  quit(true);

  if VLIKELY (impl_->sock != kInvalidSocket) {
#ifdef _WIN32
    ::shutdown(impl_->sock, SD_BOTH);
#else
    ::shutdown(impl_->sock, SHUT_RDWR);
#endif
  }

  wait_for_quit();

  if VLIKELY (impl_->thread.joinable()) {
    impl_->thread.join();
  }

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

  impl_->buffer.clear();
}

void DiscoveryViewer::register_callback(Callback&& callback) {
  std::lock_guard lock(impl_->mtx);
  impl_->callback = std::move(callback);
}

std::vector<DiscoveryViewer::Info> DiscoveryViewer::get_info_list() {
  std::lock_guard lock(impl_->mtx);
  return impl_->info_list;
}

std::string DiscoveryViewer::get_ser_type(const std::string& url) const {
  std::shared_lock lock(impl_->ser_mtx);
  auto iter = impl_->ser_map.find(url);

  if VLIKELY (iter != impl_->ser_map.end()) {
    return iter->second;
  }

  return {};
}

SchemaType DiscoveryViewer::get_schema_type(const std::string& url) const {
  std::shared_lock lock(impl_->ser_mtx);
  auto iter = impl_->schema_type_map.find(url);

  if VLIKELY (iter != impl_->schema_type_map.end()) {
    return iter->second;
  }

  return SchemaType::kUnknown;
}

size_t DiscoveryViewer::get_max_task_count() const { return kMaxTaskSize; }

uint32_t DiscoveryViewer::get_max_elapsed_time() const { return kMaxElapsedTime; }

void DiscoveryViewer::on_begin() { MessageLoop::on_begin(); }

void DiscoveryViewer::on_end() { MessageLoop::on_end(); }

void DiscoveryViewer::process_timeout() {
  std::vector<DiscoveryViewer::Info> erase_list;

  {
    std::lock_guard lock(impl_->mtx);

    for (const auto& [info, elapsed] : impl_->info_map) {
      if (elapsed.get() > kReportTimeout || !elapsed.is_active()) {
        erase_list.emplace_back(info);
      }
    }

    if (!erase_list.empty()) {
      for (const auto& info : erase_list) {
        impl_->info_map.erase(info);
      }

      impl_->info_dirty = true;
    }
  }

  report_list();
}

// LCOV_EXCL_START GCOVR_EXCL_START
void DiscoveryViewer::process_offline(std::string_view hostname, uint32_t pid, std::string_view process_name) {
  std::vector<std::pair<DiscoveryViewer::Info, DiscoveryViewer::Info>> updated_list;

  {
    std::lock_guard lock(impl_->mtx);

    for (const auto& [info, timer] : impl_->info_map) {
      auto it = std::find_if(info.process_list.begin(), info.process_list.end(),
                             [pid, process_name, hostname](const auto& process) {
                               return process.pid == pid && process.name == process_name && process.host == hostname;
                             });

      if (it != info.process_list.end()) {
        DiscoveryViewer::Info new_info = info;
        auto offset = std::distance(info.process_list.begin(), it);
        auto it_copy = new_info.process_list.begin() + offset;

        new_info.process_list.erase(it_copy);
        updated_list.emplace_back(info, std::move(new_info));
      }
    }

    if (updated_list.empty()) {
      return;
    }

    for (const auto& [old_info, new_info] : updated_list) {
      impl_->info_map.erase(old_info);

      if (!new_info.process_list.empty()) {
        auto [it, inserted] = impl_->info_map.try_emplace(new_info, ElapsedTimer{});
        it->second.restart();
      }
    }

    impl_->info_dirty = true;
  }

  report_list();
}
// LCOV_EXCL_STOP GCOVR_EXCL_STOP

void DiscoveryViewer::sort_url() {
  impl_->info_list.clear();
  std::unordered_map<std::string, std::string> next_ser_map;
  std::unordered_map<std::string, SchemaType> next_schema_type_map;
  std::unordered_set<std::string> ser_conflict_urls;
  std::unordered_set<std::string> schema_conflict_urls;
  next_ser_map.reserve(impl_->info_map.size());
  next_schema_type_map.reserve(impl_->info_map.size());
  ser_conflict_urls.reserve(impl_->info_map.size());
  schema_conflict_urls.reserve(impl_->info_map.size());

  for (const auto& [info, timer] : impl_->info_map) {
    if VLIKELY (!impl_->info_list.empty()) {
      auto& merged = impl_->info_list.back();

      if (merged.url == info.url) {
        merged.type |= info.type;
        merged.process_list.insert(merged.process_list.end(), info.process_list.begin(), info.process_list.end());

        if (!info.ser_type.empty() && ser_conflict_urls.count(info.url) == 0) {
          if (merged.ser_type.empty() || (merged.ser_type == "Bytes" && info.ser_type != "Bytes")) {
            merged.ser_type = info.ser_type;
          } else if (merged.ser_type != "Bytes" && info.ser_type != merged.ser_type) {
            merged.ser_type.clear();
            ser_conflict_urls.emplace(info.url);
          }
        }

        if (info.schema_type != SchemaType::kUnknown && schema_conflict_urls.count(info.url) == 0) {
          if (merged.schema_type == SchemaType::kUnknown) {
            merged.schema_type = info.schema_type;
          } else if (merged.schema_type != info.schema_type) {
            merged.schema_type = SchemaType::kUnknown;
            schema_conflict_urls.emplace(info.url);
          }
        }

        continue;
      }
    }

    impl_->info_list.emplace_back(info);
  }

  for (auto& info : impl_->info_list) {
    if (info.process_list.size() > 1) {
      std::sort(info.process_list.begin(), info.process_list.end());
      info.process_list.erase(std::unique(info.process_list.begin(), info.process_list.end(),
                                          [](const auto& lhs, const auto& rhs) {
                                            return lhs.type == rhs.type && lhs.host == rhs.host && lhs.pid == rhs.pid &&
                                                   lhs.name == rhs.name && lhs.ip == rhs.ip;
                                          }),
                              info.process_list.end());
    }

    auto& ser_type = next_ser_map[info.url];
    auto& schema_type = next_schema_type_map[info.url];

    if (ser_conflict_urls.count(info.url) != 0) {
      ser_type.clear();
    } else if ((ser_type.empty() || ser_type == "Bytes") && !info.ser_type.empty()) {
      ser_type = info.ser_type;
    }

    if (schema_conflict_urls.count(info.url) != 0) {
      schema_type = SchemaType::kUnknown;
    } else if (info.schema_type != SchemaType::kUnknown) {
      if (schema_type == SchemaType::kUnknown) {
        schema_type = info.schema_type;
      } else if (schema_type != info.schema_type) {  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        schema_type = SchemaType::kUnknown;          // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        schema_conflict_urls.emplace(info.url);      // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }
    }
  }

  {
    std::unique_lock lock(impl_->ser_mtx);
    impl_->ser_map.swap(next_ser_map);
    impl_->schema_type_map.swap(next_schema_type_map);
  }
}

void DiscoveryViewer::report_list() {
  Callback callback;
  std::vector<Info> info_list;

  {
    std::lock_guard lock(impl_->mtx);

    if (!impl_->info_dirty) {
      return;
    }

    sort_url();

    impl_->info_dirty = false;
    callback = impl_->callback;
    info_list = impl_->info_list;
  }

  if (callback) {
    callback(info_list);
  }
}

}  // namespace vlink
