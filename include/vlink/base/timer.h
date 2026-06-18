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
 * @file timer.h
 * @brief MessageLoop-driven periodic or one-shot timer with priority support.
 *
 * @details
 * @c vlink::Timer dispatches its callback on a @c MessageLoop thread, so the callback
 * runs serially with every other task posted to the same loop and no additional
 * synchronisation is required inside the callback body.
 *
 * Timer modes determined by @c loop_count:
 *
 * | Mode      | @c loop_count value | Behaviour                                    |
 * | --------- | ------------------- | -------------------------------------------- |
 * | One-shot  | @c 1                | Fires exactly once after one full interval   |
 * | Counted   | @c 2 ... @c INT_MAX | Fires the configured number of times         |
 * | Repeating | @c kInfinite (-1)   | Fires forever until @c stop() is called      |
 *
 * Key features:
 * - Strict mode: when the loop falls behind, missed ticks are dispatched immediately on
 *   the next iteration so the long-term cadence is preserved.
 * - Configurable dispatch priority for use with @c kPriorityType message loops.
 * - When @c interval_ms is @c 0 the internal interval clamps to @c kMinInterval
 *   (10000 ns = 10 us) to avoid pathological busy-spinning.
 * - The static @c call_once() helper posts a fire-and-forget one-shot without managing a
 *   @c Timer object.
 *
 * @par Lifecycle
 * Construct -> @c attach() (if not done at construction) -> @c start() -> repeated ticks
 * -> @c stop() / @c restart() / destruction.
 *
 * @par Example
 * @code
 * vlink::MessageLoop loop;
 * vlink::Timer timer(&loop, 500, vlink::Timer::kInfinite, [] {
 *   handle_tick();
 * });
 * timer.start();
 * loop.run();
 * @endcode
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "./functional.h"
#include "./macros.h"

namespace vlink {

/**
 * @class Timer
 * @brief Periodic or one-shot timer that fires on an associated @c MessageLoop thread.
 *
 * @details
 * Owns an internal implementation that integrates with the loop's tick scheduling so the
 * callback runs in-line with every other task posted to the loop.
 */
class VLINK_EXPORT Timer final {
 public:
  /**
   * @brief Callback signature invoked on every tick.
   */
  using Callback = MoveFunction<void()>;

  /**
   * @brief Sentinel @c loop_count value meaning fire forever.
   */
  static constexpr int kInfinite{-1};

  /**
   * @brief Constructs a detached timer with a 1000 ms interval and no callback.
   *
   * @details
   * Use @c attach() and @c set_callback() before @c start().
   */
  explicit Timer();

  /**
   * @brief Constructs a timer attached to @p message_loop with the default interval and no callback.
   *
   * @param message_loop  Loop that will dispatch ticks.
   */
  explicit Timer(class MessageLoop* message_loop);

  /**
   * @brief Constructs a fully configured timer attached to a loop.
   *
   * @param message_loop  Loop that will dispatch ticks.
   * @param interval_ms   Tick interval in milliseconds; @c 0 clamps to @c kMinInterval.
   * @param loop_count    Number of ticks to fire; @c kInfinite repeats forever.  Default: @c kInfinite.
   * @param callback      Callback to install.  Default: empty.
   */
  explicit Timer(class MessageLoop* message_loop, uint32_t interval_ms, int32_t loop_count = kInfinite,
                 Callback&& callback = nullptr);

  /**
   * @brief Constructs a detached timer with explicit interval and loop count.
   *
   * @param interval_ms  Tick interval in milliseconds; @c 0 clamps to @c kMinInterval.
   * @param loop_count   Number of ticks to fire; @c kInfinite repeats forever.  Default: @c kInfinite.
   * @param callback     Callback to install.  Default: empty.
   */
  explicit Timer(uint32_t interval_ms, int32_t loop_count = kInfinite, Callback&& callback = nullptr);

  /**
   * @brief Destructor.  Calls @c stop() and detaches the timer.
   */
  ~Timer();

  /**
   * @brief Posts a fire-and-forget one-shot timer to a loop.
   *
   * @details
   * The timer fires exactly once @p interval_ms milliseconds later and is then destroyed
   * automatically.  Convenient when lifetime management of a long-lived @c Timer object
   * would be inconvenient.
   *
   * @param message_loop  Loop that will dispatch the tick.
   * @param interval_ms   Delay in milliseconds.
   * @param callback      Callback to invoke.
   * @param priority      Dispatch priority; @c 0 keeps the default.  Default: @c 0.
   * @return @c true on success.
   */
  static bool call_once(class MessageLoop* message_loop, uint32_t interval_ms, Callback&& callback,
                        uint16_t priority = 0);

  /**
   * @brief Reports whether the timer is currently armed.
   *
   * @return @c true when the timer is running.
   */
  [[nodiscard]] bool is_active() const;

  /**
   * @brief Reports whether strict catch-up mode is enabled.
   *
   * @return @c true when missed ticks are fired back-to-back.
   */
  [[nodiscard]] bool is_strict() const;

  /**
   * @brief Returns the configured tick interval in milliseconds.
   *
   * @return Interval.
   */
  [[nodiscard]] uint32_t get_interval() const;

  /**
   * @brief Returns the configured total tick count.
   *
   * @return Loop count; @c kInfinite for indefinite repetition.
   */
  [[nodiscard]] int32_t get_loop_count() const;

  /**
   * @brief Returns the number of ticks remaining before automatic stop.
   *
   * @return Remaining tick count; @c kInfinite when repeating forever.
   */
  [[nodiscard]] int32_t get_remain_loop_count() const;

  /**
   * @brief Returns the number of ticks dispatched since the last @c start() or @c restart().
   *
   * @return Dispatched tick count.
   */
  [[nodiscard]] uint64_t get_invoke_count() const;

  /**
   * @brief Returns the dispatch priority used with @c kPriorityType loops.
   *
   * @return Priority value.
   */
  [[nodiscard]] uint16_t get_priority() const;

  /**
   * @brief Returns the loop the timer is currently attached to.
   *
   * @return Loop pointer; @c nullptr when detached.
   */
  [[nodiscard]] class MessageLoop* get_message_loop() const;

  /**
   * @brief Attaches the timer to @p message_loop.
   *
   * @details
   * Must be called before @c start() when no loop was provided at construction.  A null
   * pointer logs a fatal error and throws.  Re-attaching detaches from any previous loop.
   *
   * @param message_loop  Loop to attach to.
   * @return @c true on success.
   */
  bool attach(class MessageLoop* message_loop);

  /**
   * @brief Detaches the timer from its loop and stops it first.
   *
   * @return @c true on success.
   */
  bool detach();

  /**
   * @brief Arms the timer and starts dispatching ticks.
   *
   * @details
   * When @p callback is non-null it replaces the existing callback.  The first tick fires
   * after one full interval, not immediately.  Starting a timer with no callback is a
   * no-op for dispatch purposes.
   *
   * @param callback  Optional callback replacement.  Default: keep the existing callback.
   */
  void start(Callback&& callback = nullptr);

  /**
   * @brief Resets the tick count and re-arms the timer as if newly started.
   */
  void restart();

  /**
   * @brief Disarms the timer without destroying it.
   *
   * @details
   * After @c stop() the timer can be re-armed by another @c start().
   */
  void stop();

  /**
   * @brief Enables or disables strict catch-up firing.
   *
   * @param strict  @c true to fire missed ticks back-to-back.  Default state is @c false.
   */
  void set_strict(bool strict);

  /**
   * @brief Updates the tick interval.
   *
   * @details
   * Takes effect immediately for an active timer: the processed tick count is recomputed
   * against the existing start time and the loop is woken.  @p interval_ms @c == @c 0
   * clamps the internal interval to @c kMinInterval (10000 ns = 10 us).
   *
   * @param interval_ms  New interval in milliseconds.
   */
  void set_interval(uint32_t interval_ms);

  /**
   * @brief Updates the total tick count.
   *
   * @param loop_count  New loop count; @c kInfinite for indefinite repetition.
   */
  void set_loop_count(int32_t loop_count);

  /**
   * @brief Replaces the callback dispatched on every tick.
   *
   * @param callback  New callback.
   */
  void set_callback(Callback&& callback);

  /**
   * @brief Updates the dispatch priority.
   *
   * @details
   * Only effective when the associated loop is of type @c kPriorityType.
   *
   * @param priority  Priority value; higher fires sooner.
   */
  void set_priority(uint16_t priority);

 private:
  void run_callback();

  void begin_in_flight();

  void end_in_flight();

  void wait_for_idle();

  void clear();

  void force_to_start();

  void set_remain_loop_count(int32_t loop_count) const;

  void sub_remain_loop_count() const;

  void set_invoke_count(uint64_t invoke_count) const;

  uint64_t get_start_time() const;

  bool is_once_type() const;

  bool has_callback() const;

  Callback take_callback();

  std::shared_ptr<std::atomic_bool> get_alive_flag() const;

  friend class MessageLoop;
  struct Impl;
  std::unique_ptr<Impl> impl_;
  static constexpr uint32_t kMinInterval{10'000U};

  VLINK_DISALLOW_COPY_AND_ASSIGN(Timer)
};

}  // namespace vlink
