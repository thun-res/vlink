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

#include <string>

#include "../common_transport.h"

namespace Common {  // NOLINT(readability-identifier-naming)

// Ping direction: ping process publishes here, pong process subscribes.
// PING_URL overrides PING_TRANSPORT scheme selection; default is dds://.
inline std::string get_ping_url() {
  return get_transport_url("PING_URL", "PING_TRANSPORT", "ping_pong/ping", "someip://0x05/0x06?groups=0x7&event=0x8");
}

// Pong direction: pong process publishes here, ping process subscribes.
// Two separate topics avoid a publisher receiving its own messages.
inline std::string get_pong_url() {
  return get_transport_url("PONG_URL", "PONG_TRANSPORT", "ping_pong/pong", "someip://0x05/0x06?groups=0x7&event=0x9");
}

}  // namespace Common
