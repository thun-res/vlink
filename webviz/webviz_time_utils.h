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

#include <vlink/base/elapsed_timer.h>
#include <vlink/base/macros.h>

#include <atomic>
#include <cstdint>
#include <limits>

namespace vlink {
namespace webviz {

inline uint64_t micros_to_nanos_saturated(uint64_t timestamp_us) {
  if VLIKELY (timestamp_us <= std::numeric_limits<uint64_t>::max() / 1000) {
    return timestamp_us * 1000;
  }

  return std::numeric_limits<uint64_t>::max();
}

inline uint64_t add_nanos_saturated(uint64_t lhs_ns, uint64_t rhs_ns) {
  if VLIKELY (rhs_ns <= std::numeric_limits<uint64_t>::max() - lhs_ns) {
    return lhs_ns + rhs_ns;
  }

  return std::numeric_limits<uint64_t>::max();
}

struct BridgeWallTimeState final {
  uint64_t last_sys_time_ns{0};
  uint64_t last_boot_time_ns{0};
};

inline BridgeWallTimeState make_bridge_wall_time_state(uint64_t sys_time_us, uint64_t boot_time_us) {
  BridgeWallTimeState state;
  state.last_sys_time_ns = micros_to_nanos_saturated(sys_time_us);
  state.last_boot_time_ns = micros_to_nanos_saturated(boot_time_us);
  return state;
}

inline void reset_bridge_wall_time_state(std::atomic<uint64_t>& last_sys_time_ns, ElapsedTimer& bridge_time_elapsed) {
  last_sys_time_ns.store(0);
  bridge_time_elapsed.stop();
}

inline void reset_bridge_session_time_anchor(std::atomic<uint64_t>& session_start_sys_time_ns) {
  session_start_sys_time_ns.store(0);
}

inline void update_bridge_wall_time_state(uint64_t sys_time_us, uint64_t boot_time_us,
                                          std::atomic<uint64_t>& last_sys_time_ns, ElapsedTimer& bridge_time_elapsed,
                                          std::atomic<uint64_t>* session_start_sys_time_ns = nullptr) {
  auto state = make_bridge_wall_time_state(sys_time_us, boot_time_us);
  last_sys_time_ns.store(state.last_sys_time_ns);

  if VLIKELY (session_start_sys_time_ns != nullptr) {
    auto expected = static_cast<uint64_t>(0);
    session_start_sys_time_ns->compare_exchange_strong(expected, state.last_sys_time_ns);
  }

  bridge_time_elapsed.restart();
}

inline uint64_t estimate_bridge_wall_time_ns(uint64_t last_sys_time_ns, const ElapsedTimer& bridge_time_elapsed) {
  if VUNLIKELY (last_sys_time_ns == 0) {
    return 0;
  }

  auto elapsed_ns = bridge_time_elapsed.get();

  if VUNLIKELY (elapsed_ns < 0) {
    return last_sys_time_ns;
  }

  return add_nanos_saturated(last_sys_time_ns, static_cast<uint64_t>(elapsed_ns));
}

inline uint64_t resolve_message_timestamp_ns(int64_t message_timestamp_ns, uint64_t fallback_timestamp_ns) {
  if VLIKELY (message_timestamp_ns >= 0) {
    return static_cast<uint64_t>(message_timestamp_ns);
  }

  return fallback_timestamp_ns;
}

inline uint64_t resolve_bridge_data_timestamp_ns(uint64_t session_start_sys_time_ns, int64_t data_timestamp_us,
                                                 uint64_t fallback_timestamp_ns) {
  if VLIKELY (data_timestamp_us >= 0 && session_start_sys_time_ns > 0) {
    return add_nanos_saturated(session_start_sys_time_ns,
                               micros_to_nanos_saturated(static_cast<uint64_t>(data_timestamp_us)));
  }

  return fallback_timestamp_ns;
}

}  // namespace webviz
}  // namespace vlink
