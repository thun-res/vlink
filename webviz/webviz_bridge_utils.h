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

#include <vlink/base/logger.h>
#include <vlink/base/macros.h>
#include <vlink/version.h>

#include <algorithm>
#include <string>
#include <vector>

#include "proxy_bridge.h"

namespace vlink {
namespace webviz {

inline bool is_publisher_info(const ProxyAPI::Info& info) { return (info.type & kPublisher) != 0U; }

inline std::string proxy_bridge_error_message(const ProxyBridge& bridge, ProxyAPI::Error error) {
  switch (error) {
    case ProxyAPI::kNoError:
      return {};
    case ProxyAPI::kControlError:
      return "Proxy error: another controller started, current loss of control";
    case ProxyAPI::kModeError:
      return "Proxy error: the current mode does not match the target mode";
    case ProxyAPI::kReliableCompError:
      return "Proxy error: reliable mode is incompatible with proxy server";
    case ProxyAPI::kTcpCompError:
      return "Proxy error: TCP mode is incompatible with proxy server";
    case ProxyAPI::kDirectCompError:
      return "Proxy error: direct mode is incompatible with proxy server";
    case ProxyAPI::kMultiProxyError: {
      auto hostnames = bridge.get_proxy_hostnames();
      std::string hosts;

      for (const auto& hostname : hostnames) {
        if VLIKELY (!hosts.empty()) {
          hosts += ", ";
        }

        hosts += hostname;
      }

      return "Proxy error: multiple proxy servers or proxy identity mismatch detected [" + hosts + "]";
    }
    case ProxyAPI::kVersionCompError: {
      auto proxy_ver = bridge.get_proxy_version();
      return "Proxy error: version mismatch, proxy [" + (proxy_ver.empty() ? std::string("unknown") : proxy_ver) +
             "] vs client [" + VLINK_VERSION + "]";
    }
    case ProxyAPI::kTokenError:
      return "Proxy error: authentication handshake failed or same-server token mismatch";
    case ProxyAPI::kUnknownError:
      return "Proxy error: unknown or unclassified startup/runtime failure";
    default:
      return "Proxy error: unknown error code " + std::to_string(static_cast<int>(error));
  }
}

inline void log_proxy_bridge_error(const ProxyBridge& bridge, ProxyAPI::Error error) {
  auto message = proxy_bridge_error_message(bridge, error);

  if VUNLIKELY (message.empty()) {
    return;
  }

  if VLIKELY (error == ProxyAPI::kUnknownError || error == ProxyAPI::kControlError || error == ProxyAPI::kModeError ||
              error == ProxyAPI::kReliableCompError || error == ProxyAPI::kTcpCompError ||
              error == ProxyAPI::kDirectCompError || error == ProxyAPI::kMultiProxyError ||
              error == ProxyAPI::kVersionCompError || error == ProxyAPI::kTokenError) {
    MLOG_E("{}", message);
  } else {
    MLOG_W("{}", message);
  }
}

inline std::string build_bridge_control_signature(const ProxyAPI::Control& control) {
  std::vector<std::string> entries;
  entries.reserve(control.url_meta_list.size());

  for (const auto& meta : control.url_meta_list) {
    if VUNLIKELY (meta.url.empty()) {
      continue;
    }

    std::string entry;
    entry.reserve(meta.url.size() + meta.ser.size() + 32U);
    entry.append(meta.url);
    entry.push_back('\n');
    entry.append(meta.ser);
    entry.push_back('\n');
    entry.append(std::to_string(static_cast<int>(meta.schema)));
    entry.push_back('\n');
    entry.append(std::to_string(static_cast<int>(meta.type)));
    entries.emplace_back(std::move(entry));
  }

  std::sort(entries.begin(), entries.end());
  entries.erase(std::unique(entries.begin(), entries.end()), entries.end());

  std::string signature = std::to_string(static_cast<int>(control.mode));
  signature.push_back('|');
  signature.append(control.filter_str);
  signature.push_back('|');

  for (const auto& entry : entries) {
    signature.append(entry);
    signature.push_back('\n');
  }

  return signature;
}

}  // namespace webviz
}  // namespace vlink
