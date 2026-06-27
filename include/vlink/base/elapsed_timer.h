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
 * @file elapsed_timer.h
 * @brief Lowest-level elapsed-time primitive used by deadline tracking, profilers and task metrics.
 *
 * @details
 * @c ElapsedTimer measures how much time has passed since @c start was called, using either a
 * monotonic wall clock or process CPU time, with millisecond / microsecond / nanosecond
 * resolution.  It backs @c DeadlineTimer, @c CpuProfiler and the message-loop task latency
 * counters.
 *
 * @par API table
 *
 * | Method                  | Purpose                                       | Mutates state |
 * | ----------------------- | --------------------------------------------- | ------------- |
 * | @c start                | Latch the current time as the start reference | yes           |
 * | @c stop                 | Mark the timer inactive (start = @c -1)       | yes           |
 * | @c restart              | Read elapsed, latch new start atomically      | yes           |
 * | @c get                  | Read elapsed without changing state           | no            |
 * | @c is_active            | Report whether @c start has set the reference | no            |
 * | @c get_sys_timestamp    | Sample wall-clock time (CLOCK_REALTIME)       | no            |
 * | @c get_cpu_timestamp    | Sample monotonic time (CLOCK_MONOTONIC_RAW)   | no            |
 * | @c get_cpu_active_time  | Sample cumulative process CPU time            | no            |
 *
 * @par Clock source (Method)
 *
 * | Value              | Clock source                          | Notes                             |
 * | ------------------ | ------------------------------------- | --------------------------------- |
 * | kCpuTimestamp      | @c std::chrono::steady_clock          | Monotonic wall time, never jumps  |
 * | kCpuActiveTime     | @c getrusage / @c GetProcessTimes     | Total process user + kernel time  |
 *
 * @par Precision (Accuracy)
 *
 * | Value   | Unit         | 64-bit dynamic range    |
 * | ------- | ------------ | ----------------------- |
 * | kMilli  | milliseconds | ~292 million years      |
 * | kMicro  | microseconds | ~292 thousand years     |
 * | kNano   | nanoseconds  | ~292 years              |
 *
 * @par Example
 * @code
 *   vlink::ElapsedTimer t(vlink::ElapsedTimer::kCpuTimestamp, vlink::ElapsedTimer::kMicro);
 *   t.start();
 *   do_work();
 *   const int64_t us = t.get();           // microseconds since start; -1 if not started
 *   const int64_t since = t.restart();    // read + atomic reset
 *   t.stop();
 * @endcode
 *
 * @note The timer is @b not started by the constructor; call @c start explicitly.  The internal
 *       start time is held in a 64-byte aligned @c std::atomic<int64_t>; concurrent reads are
 *       safe, but interleaved @c start / @c stop from different threads race on the active flag.
 */

#pragma once

#include <atomic>
#include <cstdint>

#include "./macros.h"

namespace vlink {

/**
 * @class ElapsedTimer
 * @brief Atomic high-resolution timer with selectable clock source and unit.
 *
 * @details
 * Records the elapsed time between @c start and @c get / @c restart.  Marked @c final.  Copy
 * and move constructors copy the snapshot value (not exclusive ownership), allowing trivial
 * propagation through wrapper structures.
 */
class VLINK_EXPORT ElapsedTimer final {
 public:
  /**
   * @brief Clock source selector.
   */
  enum Method : uint8_t {
    kCpuTimestamp = 0,  ///< Monotonic wall clock (@c steady_clock for instance timing).
    kCpuActiveTime = 1  ///< Process CPU time (user + kernel via @c getrusage / @c GetProcessTimes).
  };

  /**
   * @brief Precision selector for stored and returned time values.
   */
  enum Accuracy : uint8_t {
    kMilli = 0,  ///< Millisecond precision.
    kMicro = 1,  ///< Microsecond precision.
    kNano = 2    ///< Nanosecond precision.
  };

  /**
   * @brief Constructs a timer with default method (@c kCpuTimestamp) and accuracy (@c kMilli).
   *
   * @details
   * The timer is inactive on construction; call @c start to begin measuring.
   */
  ElapsedTimer() noexcept;

  /**
   * @brief Constructs a timer with the given clock source and default millisecond accuracy.
   *
   * @param method  Clock source.
   */
  explicit ElapsedTimer(Method method) noexcept;

  /**
   * @brief Constructs a timer with the default clock source and the given accuracy.
   *
   * @param accuracy  Precision of returned values.
   */
  explicit ElapsedTimer(Accuracy accuracy) noexcept;

  /**
   * @brief Constructs a timer with the given clock source and accuracy.
   *
   * @param method    Clock source.
   * @param accuracy  Precision of returned values.
   */
  explicit ElapsedTimer(Method method, Accuracy accuracy) noexcept;

  /**
   * @brief Copy constructor; copies the snapshot value and configuration.
   *
   * @param target  Source timer.
   */
  ElapsedTimer(const ElapsedTimer& target) noexcept;

  /**
   * @brief Move constructor; equivalent to copy for this snapshot-valued type.
   *
   * @param target  Source timer.
   */
  ElapsedTimer(ElapsedTimer&& target) noexcept;

  /**
   * @brief Destructor.
   */
  ~ElapsedTimer() noexcept;

  /**
   * @brief Copy assignment; copies the snapshot value and configuration.
   *
   * @param target  Source timer.
   * @return Reference to @c *this.
   */
  ElapsedTimer& operator=(const ElapsedTimer& target) noexcept;

  /**
   * @brief Move assignment; equivalent to copy assignment for this snapshot-valued type.
   *
   * @return Reference to @c *this.
   */
  ElapsedTimer& operator=(ElapsedTimer&&) noexcept;

  /**
   * @brief Returns the current wall-clock timestamp.
   *
   * @details
   * On Linux uses @c clock_gettime with @c CLOCK_REALTIME when @p high_resolution is @c true,
   * falling back to @c std::chrono::system_clock otherwise.  On Windows always uses
   * @c std::chrono::system_clock.
   *
   * @param accuracy         Desired precision.  Default: @c kMilli.
   * @param high_resolution  Linux-only switch for @c clock_gettime.  Default: @c true.
   * @return Wall-clock timestamp in the requested unit.
   */
  [[nodiscard]] static uint64_t get_sys_timestamp(Accuracy accuracy = kMilli, bool high_resolution = true) noexcept;

  /**
   * @brief Returns the current monotonic CPU timestamp.
   *
   * @details
   * On Linux uses @c CLOCK_MONOTONIC_RAW (immune to NTP) when @p high_resolution is @c true;
   * otherwise uses @c std::chrono::steady_clock.  Windows always uses @c steady_clock.
   *
   * @param accuracy         Desired precision.  Default: @c kMilli.
   * @param high_resolution  Linux-only switch for @c clock_gettime.  Default: @c true.
   * @return Monotonic timestamp in the requested unit.
   */
  [[nodiscard]] static uint64_t get_cpu_timestamp(Accuracy accuracy = kMilli, bool high_resolution = true) noexcept;

  /**
   * @brief Returns the cumulative CPU time consumed by the current process.
   *
   * @details
   * On POSIX uses @c getrusage(RUSAGE_SELF); on Windows uses @c GetProcessTimes.  The value is
   * cumulative; subtract two samples to obtain a delta.
   *
   * @param accuracy  Desired precision.  Default: @c kMilli.
   * @return Process CPU time in the requested unit.
   */
  [[nodiscard]] static uint64_t get_cpu_active_time(Accuracy accuracy = kMilli) noexcept;

  /**
   * @brief Returns the clock source this timer was configured with.
   *
   * @return Configured clock source.
   */
  [[nodiscard]] Method get_method() const noexcept;

  /**
   * @brief Returns the precision this timer was configured with.
   *
   * @return Configured precision.
   */
  [[nodiscard]] Accuracy get_accuracy() const noexcept;

  /**
   * @brief Records the current time as the start reference if the timer is inactive.
   *
   * @details
   * Uses a compare-and-swap on the internal atomic so concurrent calls compete and only one wins.
   * Already-active timers are unchanged; call @c stop first to rearm.
   */
  void start() noexcept;

  /**
   * @brief Marks the timer inactive by writing @c -1 into the start reference.
   *
   * @details
   * After @c stop the timer reports @c is_active() @c == @c false and @c get() returns @c -1.
   */
  void stop() noexcept;

  /**
   * @brief Atomically reads the elapsed time and starts a new measurement from now.
   *
   * @details
   * Performs a single atomic exchange.  When the timer was already active the returned value is
   * the elapsed time since the last start; when it was inactive the return is negative (the raw
   * sentinel) and the timer is left active starting from this call.
   *
   * @return Elapsed time in the configured unit, or a negative value if the timer was inactive.
   */
  int64_t restart() noexcept;

  /**
   * @brief Reports whether the timer is currently measuring.
   *
   * @return @c true when @c start has produced a non-sentinel reference.
   */
  [[nodiscard]] bool is_active() const noexcept;

  /**
   * @brief Returns the elapsed time since @c start without altering state.
   *
   * @return Elapsed value in the configured unit, or @c -1 when inactive.
   */
  [[nodiscard]] int64_t get() const noexcept;

 private:
  alignas(64) std::atomic<int64_t> start_time_{-1};
  Method method_{kCpuTimestamp};
  Accuracy accuracy_{kMilli};
};

}  // namespace vlink
