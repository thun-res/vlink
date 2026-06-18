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
 * @file semaphore.h
 * @brief Counting semaphore confined to a single process.
 *
 * @details
 * @c vlink::Semaphore offers classic Dijkstra P/V semantics for in-process synchronisation,
 * built on top of @c std::mutex and the project @c ConditionVariable (which uses
 * @c CLOCK_MONOTONIC on Linux so NTP-induced jumps do not skew @c wait timeouts).  It is
 * the right primitive for permit-style throttling, bounded-queue back-pressure, and
 * producer/consumer handoff.
 *
 * Wait/post interaction between two threads:
 *
 * @verbatim
 *   Producer                        Consumer
 *      |                               |
 *      | release(n) -- counter+=n ---> |
 *      |                               | acquire(m): blocks while counter<m
 *      |                               | counter -= m; resumes
 * @endverbatim
 *
 * @note
 * - @c release() acquires the internal mutex, so it must not be called from a signal
 *   handler.  Use @c SysSemaphore or @c sem_post directly for signal-context posts.
 * - @c reset(true) wakes every blocked waiter and returns @c false to them; use only
 *   during controlled shutdown.
 *
 * @par Example
 * @code
 * vlink::Semaphore sem(0);
 *
 * std::thread producer([&] {
 *   do_work();
 *   sem.release();
 * });
 *
 * if (sem.acquire(1, 100)) {
 *   consume_result();
 * } else {
 *   handle_timeout();
 * }
 *
 * producer.join();
 * @endcode
 */

#pragma once

#include <memory>

#include "./macros.h"

namespace vlink {

/**
 * @class Semaphore
 * @brief In-process counting semaphore with an optional acquire timeout.
 *
 * @details
 * The internal counter starts at the value supplied to the constructor; @c acquire
 * decrements it (blocking when not enough permits are available) and @c release
 * increments it while waking blocked acquirers.
 */
class VLINK_EXPORT Semaphore final {
 public:
  /**
   * @brief Sentinel value for @c acquire() meaning wait indefinitely.
   */
  static constexpr int kInfinite{-1};

  /**
   * @brief Constructs a semaphore with the given initial permit count.
   *
   * @param count  Initial permit count.  Default: @c 0.
   */
  explicit Semaphore(size_t count = 0) noexcept;

  /**
   * @brief Destructor.  Wakes every blocked acquirer before tearing down internal state.
   */
  ~Semaphore() noexcept;

  /**
   * @brief Decrements the counter by @p n, blocking when not enough permits are available.
   *
   * @details
   * The call blocks while the counter is less than @p n.  A timeout returns @c false
   * without modifying the counter.  When @c reset(true) is invoked while a thread is
   * blocked, the wait returns @c false and the counter is restored to the initial value.
   *
   * @param n           Number of permits requested.  Default: @c 1.
   * @param timeout_ms  Maximum wait in milliseconds; @c kInfinite waits forever.
   * @return @c true when the permits were acquired; @c false on timeout or reset.
   */
  bool acquire(size_t n = 1, int timeout_ms = kInfinite) noexcept;

  /**
   * @brief Increments the counter by @p n and wakes blocked acquirers.
   *
   * @details
   * Broadcasts the condition variable so every blocked acquirer can re-evaluate the
   * permit count.  Safe to call from any thread.
   *
   * @param n  Number of permits to add.  Default: @c 1.
   */
  void release(size_t n = 1) noexcept;

  /**
   * @brief Restores the counter to the initial value supplied to the constructor.
   *
   * @details
   * When @p interrupt_waiters is @c true every blocked acquirer is woken and its call
   * returns @c false, which is the recommended way to release waiters during shutdown.
   *
   * @param interrupt_waiters  When @c true, wakes blocked acquirers.  Default: @c false.
   */
  void reset(bool interrupt_waiters = false) noexcept;

  /**
   * @brief Returns a non-atomic snapshot of the current counter value.
   *
   * @details
   * The value may change immediately after the call returns; treat the result as a
   * diagnostic-only hint.
   *
   * @return Current permit count snapshot.
   */
  [[nodiscard]] size_t get_count() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(Semaphore)
};

}  // namespace vlink
