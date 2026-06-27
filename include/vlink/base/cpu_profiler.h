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

/**
 * @file cpu_profiler.h
 * @brief Per-instance CPU utilisation sampler with an env-driven global enable gate.
 *
 * @details
 * @c CpuProfiler measures the fraction of wall-clock time spent inside paired
 * @c begin / @c end intervals.  Two @c ElapsedTimer instances back the computation: one accrues
 * active intervals; the other captures the total wall-clock span since the first @c begin.  The
 * reported utilisation is therefore the ratio of accumulated active time to elapsed wall time.
 *
 * @par Phases and events
 *
 * | Phase / event       | Source                         | Effect on counters                      |
 * | ------------------- | ------------------------------ | --------------------------------------- |
 * | First @c begin      | active timer + timestamp timer | Both timers start                       |
 * | Subsequent @c begin | active timer @c restart        | Resets the active baseline              |
 * | @c end              | active timer elapsed           | Adds positive delta to @c total_active_ |
 * | @c get              | read-only                      | Returns @c (active / elapsed) * 100     |
 * | @c restart          | read + reset                   | Returns the value then zeroes counters  |
 *
 * @par RAII guard pattern
 *
 * @verbatim
 *   vlink::CpuProfiler profiler;
 *   --------------------------------------------------------
 *   {
 *     vlink::CpuProfilerGuard guard(&profiler);  // begin()
 *     do_work();
 *   }                                            // end()
 *   const double percent = profiler.get();
 *   --------------------------------------------------------
 * @endverbatim
 *
 * @par Global enable gate
 * @c VLINK_PROFILER_ENABLE is read on the first call to @c is_global_enabled and cached for the
 * remainder of the process lifetime.  Set it to @c "1" to enable; any other value disables.
 *
 * @par Example
 * @code
 *   if (vlink::CpuProfiler::is_global_enabled()) {
 *     for (auto& item : work_items) {
 *       profiler.begin();
 *       process(item);
 *       profiler.end();
 *     }
 *     const double pct = profiler.restart();
 *     publish(pct);
 *   }
 * @endcode
 */

#pragma once

#include <atomic>
#include <cstdint>

#include "./elapsed_timer.h"
#include "./macros.h"
#include "./spin_lock.h"

namespace vlink {

/**
 * @class CpuProfiler
 * @brief Tracks active CPU time as a percentage of wall-clock time using a @c SpinLock guard.
 *
 * @details
 * Each @c begin / @c end pair contributes one active interval to the running total.  @c get
 * returns the ratio of active to elapsed time; @c restart returns the same value and clears the
 * accumulators atomically.  Non-copyable; each profiler tracks a single logical stream.
 */
class VLINK_EXPORT CpuProfiler final {
 public:
  /**
   * @brief Constructs a profiler with zeroed accumulators and no active interval.
   */
  CpuProfiler() noexcept;

  /**
   * @brief Destructor.
   */
  ~CpuProfiler() noexcept;

  /**
   * @brief Reports whether profiling is enabled via the @c VLINK_PROFILER_ENABLE environment variable.
   *
   * @details
   * The environment is sampled lazily on the first call and cached for the process lifetime.
   * Use the return value to gate expensive sampling around @c begin / @c end pairs.
   *
   * @return @c true when @c VLINK_PROFILER_ENABLE is set to @c "1".
   */
  [[nodiscard]] static bool is_global_enabled() noexcept;

  /**
   * @brief Opens a new active interval.
   *
   * @details
   * Restarts the internal active timer and, on the first call, also starts the wall-clock timer
   * used as the denominator in @c get.  Acquires the internal @c SpinLock briefly.
   */
  void begin() noexcept;

  /**
   * @brief Closes the current active interval and accrues its elapsed time.
   *
   * @details
   * Reads the active timer's elapsed value and adds non-negative deltas to @c total_active_.
   * Acquires the internal @c SpinLock briefly.
   */
  void end() noexcept;

  /**
   * @brief Returns the current CPU utilisation ratio as a percentage.
   *
   * @details
   * Computed as @c (total_active_ns @c / @c total_elapsed_ns) @c * @c 100.0.  Returns @c 0.0
   * when no time has elapsed yet.  Values can exceed @c 100 on multi-core systems where the
   * sum of active intervals outruns wall-clock time on one core.
   *
   * @return Utilisation percentage; @c 0.0 when no data has accrued.
   */
  [[nodiscard]] double get() const noexcept;

  /**
   * @brief Returns the current utilisation and atomically zeroes all accumulators.
   *
   * @details
   * Equivalent to reading @c get and then resetting both timers.  Acquires the internal
   * @c SpinLock briefly.
   *
   * @return Utilisation percentage accrued since construction or the last @c restart.
   */
  double restart() noexcept;

 private:
  alignas(64) std::atomic<uint64_t> total_active_{0};
  ElapsedTimer cpu_active_timer_{ElapsedTimer::kCpuActiveTime, ElapsedTimer::kNano};
  ElapsedTimer cpu_timestamp_timer_{ElapsedTimer::kCpuTimestamp, ElapsedTimer::kNano};
  SpinLock spin_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(CpuProfiler)
};

}  // namespace vlink
