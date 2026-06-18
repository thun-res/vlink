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
 * @file wheel_timer.h
 * @brief Hashed timing wheel for managing very large pools of concurrent timeouts.
 *
 * @details
 * @c vlink::WheelTimer implements the classic hashed-timing-wheel data structure that
 * provides O(1) insertion, O(1) removal and O(k) per-tick expiry processing, where @c k
 * is the number of timers in the current slot.  It scales comfortably to tens or hundreds
 * of thousands of independent timeouts (for example, a session manager or a connection
 * keep-alive supervisor).
 *
 * Wheel layout for a wheel with @c S slots advancing every @c interval_ms milliseconds:
 *
 * @verbatim
 *                        cursor
 *                          v
 *   +-----+-----+-----+-----+-----+-----+-----+-----+ ... +-----+
 *   |  0  |  1  |  2  |  3  |  4  |  5  |  6  |  7  | ... | S-1 |
 *   +-----+-----+-----+--+--+-----+-----+--+--+-----+ ... +-----+
 *                        |                 |
 *               handler list:     handler list:
 *                 - {key, round=0, cb}     - {key, round=2, cb}
 *                 - {key, round=1, cb}
 * @endverbatim
 *
 * Timers with timeouts longer than @c S @c * @c interval_ms wrap around using a round
 * counter that is decremented on every cursor pass.  Removal is O(1) via a key-to-slot
 * map maintained alongside the wheel.
 *
 * Lifecycle:
 * -# Construct with @c WheelTimer(slots, interval_ms).
 * -# Call @c start() to launch the worker thread.
 * -# Insert timers via @c add(); keep the returned @c Key for later removal.
 * -# Call @c stop() to terminate the worker thread.
 *
 * @note
 * - Expiry callbacks run on the wheel's worker thread; marshal results back through a
 *   @c MessageLoop or a lock when shared state is involved.
 * - @c set_catchup_limit() bounds how many missed slots are processed per tick to keep
 *   a single iteration from blocking for too long after a system sleep.
 *
 * @par Example
 * @code
 * vlink::WheelTimer wheel(256, 10);
 * wheel.start();
 *
 * auto key = wheel.add(1000, [](vlink::WheelTimer::Key k) {
 *   (void)k;
 * });
 *
 * auto repeat_key = wheel.add(500, [](vlink::WheelTimer::Key k) { (void)k; }, 500);
 *
 * wheel.remove(key);
 * wheel.stop();
 * @endcode
 */

#pragma once

#include <cstdint>
#include <memory>

#include "./functional.h"
#include "./macros.h"

namespace vlink {

/**
 * @class WheelTimer
 * @brief O(1) hashed-timing-wheel scheduler backed by an internal worker thread.
 *
 * @details
 * Owns a fixed-size slot array and advances a cursor every @c interval_ms milliseconds.
 */
class VLINK_EXPORT WheelTimer {
 public:
  /**
   * @brief Opaque handle returned by @c add() and accepted by @c remove().
   */
  using Key = int64_t;

  /**
   * @brief Callback signature invoked when a timer expires.
   *
   * @details
   * The @c Key argument lets a single lambda manage multiple timers.
   */
  using Callback = Function<void(Key)>;

  /**
   * @brief Constructs the wheel with the given resolution and capacity.
   *
   * @details
   * Both @p slots and @p interval_ms must be greater than zero; invalid values log a
   * fatal error and throw.  Call @c start() to begin advancing the wheel.
   *
   * @param slots        Number of buckets in the wheel.  Larger values shorten the
   *                     round counter for long timeouts.
   * @param interval_ms  Tick duration in milliseconds; sets the resolution of every timer.
   */
  explicit WheelTimer(uint32_t slots, uint32_t interval_ms);

  /**
   * @brief Destructor.  Calls @c stop() when the wheel is still running.
   */
  ~WheelTimer();

  /**
   * @brief Starts the worker thread and begins advancing the wheel cursor.
   */
  void start();

  /**
   * @brief Stops the wheel and joins the worker thread.
   *
   * @details
   * Pending timers do not fire after @c stop() returns.
   */
  void stop();

  /**
   * @brief Temporarily suspends timer dispatch without joining the worker thread.
   *
   * @details
   * The worker remains alive but the cursor stops advancing.  Use @c resume() to
   * continue.
   */
  void pause();

  /**
   * @brief Resumes a paused wheel; a no-op when not paused.
   */
  void resume();

  /**
   * @brief Wakes the worker thread early when it is sleeping between ticks.
   *
   * @details
   * Useful after inserting a very short timeout that should fire immediately.
   */
  void wakeup();

  /**
   * @brief Reports whether the wheel is currently running.
   *
   * @return @c true between @c start() and @c stop().
   */
  [[nodiscard]] bool is_running() const;

  /**
   * @brief Inserts a new timer into the wheel.
   *
   * @details
   * The callback runs on the worker thread after @p timeout_ms milliseconds.  When
   * @p repeat_ms is non-zero the timer is re-armed at every expiry with @p repeat_ms as
   * the next timeout.
   *
   * @param timeout_ms  Initial delay in milliseconds (rounded up to a slot boundary).
   * @param callback    Function invoked on expiry.
   * @param repeat_ms   Re-arm interval in milliseconds; @c 0 selects one-shot.  Default: @c 0.
   * @return Unique key identifying this timer entry, or @c -1 on invalid input or key
   *         allocation failure.
   */
  Key add(uint32_t timeout_ms, Callback&& callback, uint32_t repeat_ms = 0);

  /**
   * @brief Removes a timer before its callback runs.
   *
   * @param key  Key returned by @c add().
   * @return @c true when the timer existed and was removed.
   */
  bool remove(Key key);

  /**
   * @brief Returns the approximate remaining time before a timer fires.
   *
   * @details
   * The value is rounded to the wheel's tick resolution.
   *
   * @param key  Key returned by @c add().
   * @return Estimated remaining time in milliseconds, or @c 0 when @p key is unknown.
   */
  [[nodiscard]] uint32_t get_remaining_time(Key key) const;

  /**
   * @brief Caps the number of catch-up slots processed in a single tick iteration.
   *
   * @details
   * Prevents a single tick from blocking the worker for an unbounded duration when the
   * wheel falls behind (for example, after the system wakes from sleep).
   *
   * @param max_slots_to_catch_up  Maximum slots processed per tick cycle.
   */
  void set_catchup_limit(uint32_t max_slots_to_catch_up);

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(WheelTimer)
};

}  // namespace vlink
