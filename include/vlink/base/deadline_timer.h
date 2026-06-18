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
 * @file deadline_timer.h
 * @brief Atomic absolute-deadline timer used for lock-free timeout tracking on hot paths.
 *
 * @details
 * @c DeadlineTimer stores a single absolute monotonic timestamp inside a 64-bit atomic, which
 * makes deadline checks branch-light and concurrent reads safe without external locking.  It is
 * used inside connection bookkeeping and RPC request tracking to detect expired operations.
 *
 * @par State diagram
 *
 * @verbatim
 *   +--------+ set_deadline / set_deadline_abs +---------+ now >= deadline +----------+
 *   |  idle  | ------------------------------> |  armed  | --------------> | expired  |
 *   | (zero) |                                 |         |                 +----------+
 *   +--------+ <------- reset() --------------- +---------+                       |
 *      ^                                                                          |
 *      +-----------------------  reset() / set 0 --------------------------------+
 * @endverbatim
 *
 * @par Comparison vs the periodic @c vlink::Timer
 *
 * | Aspect            | @c vlink::DeadlineTimer    | @c vlink::Timer                     |
 * | ----------------- | -------------------------- | ----------------------------------- |
 * | Purpose           | Track a single deadline    | Schedule repeating callbacks        |
 * | Backing storage   | One @c atomic<uint64_t>    | Loop-managed timer list             |
 * | Owns a thread     | No                         | Owned by an attached @c MessageLoop |
 * | Cost per check    | One atomic load            | Insertion / removal on the loop     |
 * | Cancellation      | @c reset()                 | @c detach() on the timer instance   |
 *
 * @par Example
 * @code
 *   vlink::DeadlineTimer t(200);                          // 200 ms from now
 *   while (!t.has_expired()) {
 *     process_events();
 *   }
 *   const int64_t left = t.remaining_time();              // 0 once past deadline
 *
 *   const uint64_t now = vlink::ElapsedTimer::get_cpu_timestamp();
 *   t.set_deadline_abs(now + 500);                        // absolute reset
 * @endcode
 */

#pragma once

#include <atomic>
#include <cstdint>

#include "./elapsed_timer.h"
#include "./macros.h"

namespace vlink {

/**
 * @class DeadlineTimer
 * @brief Cache-line aligned atomic deadline counter with millisecond / microsecond / nanosecond precision.
 *
 * @details
 * Reuses @c ElapsedTimer::Accuracy for time precision.  Reads and writes go through a single
 * @c std::atomic<uint64_t> aligned to 64 bytes to avoid false sharing when several timers share
 * a structure.  An invalid (unset) timer stores @c 0 and never reports expiry.
 */
class VLINK_EXPORT DeadlineTimer final {
 public:
  /**
   * @brief Precision unit alias inherited from @c ElapsedTimer::Accuracy.
   */
  using Accuracy = ElapsedTimer::Accuracy;

  /**
   * @brief Constructs an invalid timer with no deadline set.
   *
   * @details
   * @c is_valid returns @c false and @c has_expired returns @c false until @c set_deadline or
   * @c set_deadline_abs is called.
   */
  DeadlineTimer() noexcept;

  /**
   * @brief Constructs a timer that expires @p interval units in the future.
   *
   * @details
   * The absolute deadline is computed as the current monotonic timestamp plus @p interval.  A
   * non-positive @p interval leaves the timer invalid.
   *
   * @param interval  Duration until expiry in @p accuracy units.
   * @param accuracy  Precision of stored timestamps.  Default: @c ElapsedTimer::kMilli.
   */
  explicit DeadlineTimer(int64_t interval, Accuracy accuracy = ElapsedTimer::kMilli) noexcept;

  /**
   * @brief Copy constructor; copies the stored deadline and accuracy.
   *
   * @param other  Source timer.
   */
  DeadlineTimer(const DeadlineTimer& other) noexcept;

  /**
   * @brief Move constructor; equivalent to copy because the timer holds only POD state.
   *
   * @param other  Source timer.
   */
  DeadlineTimer(DeadlineTimer&& other) noexcept;

  /**
   * @brief Destructor.
   */
  ~DeadlineTimer() noexcept;

  /**
   * @brief Copy assignment; replaces the stored deadline and accuracy.
   *
   * @param other  Source timer.
   * @return Reference to @c *this.
   */
  DeadlineTimer& operator=(const DeadlineTimer& other) noexcept;

  /**
   * @brief Move assignment; equivalent to copy assignment for this POD-state type.
   *
   * @param other  Source timer.
   * @return Reference to @c *this.
   */
  DeadlineTimer& operator=(DeadlineTimer&& other) noexcept;

  /**
   * @brief Sets the deadline @p interval units after the current monotonic timestamp.
   *
   * @details
   * A non-positive @p interval clears the timer (equivalent to @c reset).
   *
   * @param interval  Duration until expiry in the configured accuracy unit.
   */
  void set_deadline(int64_t interval) noexcept;

  /**
   * @brief Sets an absolute deadline timestamp directly.
   *
   * @details
   * @p abs_deadline must be expressed in the same unit and time base as the configured accuracy;
   * obtain it via @c ElapsedTimer::get_cpu_timestamp(accuracy).
   *
   * @param abs_deadline  Absolute monotonic deadline.
   */
  void set_deadline_abs(uint64_t abs_deadline) noexcept;

  /**
   * @brief Resets the timer to the invalid state.
   *
   * @details
   * After @c reset, @c is_valid returns @c false and @c has_expired returns @c false.
   */
  void reset() noexcept;

  /**
   * @brief Returns the stored absolute deadline timestamp.
   *
   * @return Deadline value in the configured accuracy unit.  @c 0 indicates an invalid timer.
   */
  [[nodiscard]] uint64_t deadline() const noexcept;

  /**
   * @brief Returns the time remaining until the deadline.
   *
   * @details
   * Computed as @c (deadline - current_cpu_timestamp); clamped to @c 0 once the deadline has
   * been reached or when the timer is invalid.
   *
   * @return Remaining time in the configured accuracy unit; @c 0 when invalid or expired.
   */
  [[nodiscard]] int64_t remaining_time() const noexcept;

  /**
   * @brief Reports whether the current time has passed the stored deadline.
   *
   * @return @c true when expired; always @c false for invalid timers.
   */
  [[nodiscard]] bool has_expired() const noexcept;

  /**
   * @brief Reports whether a deadline has been set.
   *
   * @return @c true when @c deadline() is non-zero.
   */
  [[nodiscard]] bool is_valid() const noexcept;

  /**
   * @brief Returns the precision configured for this timer.
   *
   * @return Accuracy unit chosen at construction.
   */
  [[nodiscard]] Accuracy get_accuracy() const noexcept;

 private:
  alignas(64) std::atomic<uint64_t> deadline_{0};
  Accuracy accuracy_{ElapsedTimer::kMilli};
};

}  // namespace vlink
