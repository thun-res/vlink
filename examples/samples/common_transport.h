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
#include <vlink/base/utils.h>

#include <string>

namespace Common {  // NOLINT(readability-identifier-naming)

// Resolve a transport URL for a sample topic.
//
// The whole point of this helper is that VLink's URL scheme (intra:// / shm://
// / dds:// / ddsc:// / fdbus:// / someip:// ...) decouples application
// code from the underlying transport. Most topic-style backends can reuse the
// same path with another scheme; protocol-specific backends may require a full
// address. The samples ask this helper to compose or select that URL at runtime.
//
//   env_var       : full-URL override (highest priority). If set, used verbatim.
//   transport_env : selects scheme: "dds" / "ddsc" / "someip" / "shm" / "fdbus".
//   topic         : topic/path used by URL-style schemes (dds/ddsc/shm/fdbus).
//   someip_url    : SOME/IP requires numeric service/instance/event IDs rather than
//                   a free-form path, so the caller must pre-format it; this
//                   helper only forwards it when transport == "someip".
inline std::string get_transport_url(const std::string& env_var, const std::string& transport_env,
                                     const std::string& topic, const std::string& someip_url = "") {
  // Priority 1: full URL override via env_var (e.g. METHOD_URL=zenoh://foo).
  auto url = vlink::Utils::get_env(env_var);

  if (!url.empty()) {
    VLOG_I(env_var, "=", url);
    return url;
  }

  // Priority 2: scheme-only selector via transport_env. Defaults to "dds".
  auto transport = vlink::Utils::get_env(transport_env, "dds");

  if (transport == "dds") {
    url = "dds://" + topic;
  } else if (transport == "ddsc") {
    url = "ddsc://" + topic;
  } else if (transport == "shm") {
    url = "shm://" + topic;
  } else if (transport == "fdbus") {
    url = "fdbus://" + topic;
  } else if (transport == "someip") {
    // SOME/IP cannot use the path-style topic; fall back to dds:// if the
    // caller forgot to supply a properly-formed SOME/IP URL.
    url = someip_url.empty() ? "dds://" + topic : someip_url;
  } else {
    url = "dds://" + topic;
  }

  VLOG_I(env_var, "=", url);
  return url;
}

}  // namespace Common
