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
#include <mutex>

namespace vlink {
namespace webviz {

struct BridgeWallClock final {
  mutable std::mutex write_mtx;
  std::atomic<uint64_t> seq{0};
  std::atomic<uint64_t> wall_ns{0};
  std::atomic<uint64_t> steady_ns{0};

  void reset() {
    std::lock_guard lock(write_mtx);
    const auto s = seq.load(std::memory_order_relaxed);
    seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    wall_ns.store(0, std::memory_order_relaxed);
    steady_ns.store(0, std::memory_order_relaxed);
    seq.store(s + 2, std::memory_order_release);
  }

  void update(uint64_t sys_time_ns) {
    const auto steady = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano, false);
    std::lock_guard lock(write_mtx);
    const auto s = seq.load(std::memory_order_relaxed);
    seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    wall_ns.store(sys_time_ns, std::memory_order_relaxed);
    steady_ns.store(steady, std::memory_order_relaxed);
    seq.store(s + 2, std::memory_order_release);
  }

  uint64_t estimate_ns() const {
    uint64_t wall = 0;
    uint64_t steady = 0;

    for (;;) {
      const auto s1 = seq.load(std::memory_order_acquire);

      if VUNLIKELY ((s1 & 1U) != 0U) {
        continue;
      }

      wall = wall_ns.load(std::memory_order_relaxed);
      steady = steady_ns.load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);

      if VLIKELY (seq.load(std::memory_order_relaxed) == s1) {
        break;
      }
    }

    if VUNLIKELY (wall == 0) {
      return 0;
    }

    const auto now_steady = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano, false);

    if VUNLIKELY (now_steady <= steady) {
      return wall;
    }

    const auto elapsed_ns = now_steady - steady;
    return elapsed_ns <= std::numeric_limits<uint64_t>::max() - wall ? wall + elapsed_ns
                                                                     : std::numeric_limits<uint64_t>::max();
  }
};

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

inline void reset_bridge_wall_time_state(BridgeWallClock& clock) { clock.reset(); }

inline void reset_bridge_session_time_anchor(std::atomic<uint64_t>& session_start_sys_time_ns) {
  session_start_sys_time_ns.store(0);
}

inline uint64_t update_bridge_wall_time_state(uint64_t sys_time_us, BridgeWallClock& clock,
                                              std::atomic<uint64_t>& session_start_sys_time_ns) {
  const auto time = micros_to_nanos_saturated(sys_time_us);
  clock.update(time);
  uint64_t expected = 0;
  session_start_sys_time_ns.compare_exchange_strong(expected, time);
  return time;
}

inline uint64_t estimate_bridge_wall_time_ns(const BridgeWallClock& clock) { return clock.estimate_ns(); }

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
