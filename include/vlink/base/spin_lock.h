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
 * @file spin_lock.h
 * @brief Cache-line aligned adaptive spin lock with exponential back-off.
 *
 * @details
 * @c vlink::SpinLock is a user-space mutex aimed at very short critical sections (a few
 * instructions) where a @c std::mutex context switch would dominate the cost.  It is used
 * inside @c CpuProfiler and other hot-path internals.
 *
 * The acquisition loop applies exponential back-off so contention does not saturate the
 * cache-coherence bus.  Each back-off ladder rung doubles the inner spin budget until the
 * cap is reached, then a CPU-pause instruction yields the core; once the total spin count
 * exceeds the hard ceiling the back-off action switches to a sleep:
 *
 * @verbatim
 *   round 1   | XXXX---- 8 spins   ----> yield_cpu()
 *   round 2   | XXXX-XXX 16 spins  ----> yield_cpu()
 *   round 3   | XXXXXXXX 32 spins  ----> yield_cpu()
 *   ...       |         ... up to 1024 spins per ladder rung
 *   total > 50000 spins            ----> sleep_for(10 us)  (latched warn-once)
 * @endverbatim
 *
 * Comparison with @c std::mutex:
 *
 * | Property            | @c SpinLock                            | @c std::mutex                |
 * | ------------------- | -------------------------------------- | ---------------------------- |
 * | Wait strategy       | Adaptive busy-spin + yield + sleep     | OS futex / kernel wait       |
 * | Critical-section    | Single-digit microseconds              | Any duration                 |
 * | Recursive           | No (deadlocks on re-entry)             | Use @c std::recursive_mutex  |
 * | False sharing       | Avoided via @c alignas(64)             | Implementation defined       |
 * | Header-only         | Yes (no @c VLINK_EXPORT)               | Library symbol               |
 *
 * @note
 * - The lock is non-recursive; double-locking from the same thread spins forever.
 * - The hard-ceiling warning is latched per instance so a permanently contended lock does
 *   not flood the log.
 *
 * @par Example
 * @code
 * vlink::SpinLock lock;
 *
 * lock.lock();
 * critical_section();
 * lock.unlock();
 *
 * {
 *   vlink::SpinLockGuard guard(lock);
 *   critical_section();
 * }
 * @endcode
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include "./logger.h"
#include "./macros.h"
#include "./utils.h"

#ifdef _MSC_VER
extern "C" void _mm_pause(void);
#endif

namespace vlink {

/**
 * @class SpinLock
 * @brief Adaptive spin lock satisfying the C++ @c Lockable named requirement.
 *
 * @details
 * Cache-line aligned through @c alignas(64) to prevent false sharing.  Composes with
 * @c std::lock_guard, @c std::unique_lock and the supplied @c SpinLockGuard.
 */
class SpinLock final {
 public:
  /**
   * @brief Constructs an unlocked spin lock.
   */
  SpinLock() noexcept = default;

  /**
   * @brief Destructor.  Assumes the lock has already been released.
   */
  ~SpinLock() noexcept = default;

  /**
   * @brief Acquires the lock, spinning with adaptive back-off until it becomes free.
   *
   * @details
   * The inner loop alternates @c exchange attempts with relaxed-load spins.  Each back-off
   * rung doubles the spin budget from @c 8 up to @c 1024 then yields the CPU.  After
   * @c 50000 spins the back-off action switches to a 10 us sleep and a one-time warning
   * is emitted.
   *
   * @warning Recursive acquisition by the same thread deadlocks the lock.
   */
  void lock() noexcept;

  /**
   * @brief Attempts a single non-blocking acquire of the lock.
   *
   * @return @c true when the lock was successfully acquired.
   */
  bool try_lock() noexcept;

  /**
   * @brief Releases the lock with @c memory_order_release.
   *
   * @details
   * May only be called by the thread that currently owns the lock.
   */
  void unlock() noexcept;

 private:
  static constexpr size_t kInterferenceSize = 64U;

  alignas(kInterferenceSize) std::atomic<bool> flag_{false};
  std::atomic<bool> warned_{false};

  VLINK_DISALLOW_COPY_AND_ASSIGN(SpinLock)
};

/**
 * @class SpinLockGuard
 * @brief RAII wrapper that locks a @c SpinLock on construction and unlocks on destruction.
 *
 * @details
 * Equivalent to @c std::lock_guard<SpinLock>.  Preferred over manual @c lock / @c unlock
 * because it is exception-safe.
 *
 * @par Example
 * @code
 * SpinLock my_lock;
 * {
 *   SpinLockGuard guard(my_lock);
 *   critical_section();
 * }
 * @endcode
 */
class SpinLockGuard final {
 public:
  /**
   * @brief Acquires @p lock immediately.
   *
   * @param lock  Spin lock to acquire.  Must outlive this guard.
   */
  explicit SpinLockGuard(SpinLock& lock) noexcept;

  /**
   * @brief Releases the held spin lock.
   */
  ~SpinLockGuard() noexcept;

 private:
  SpinLock& lock_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(SpinLockGuard)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline void SpinLock::lock() noexcept {
  constexpr static uint32_t kMaxSpinCount = 50000U;

  constexpr static uint16_t kInitialBackoff = 8U;
  constexpr static uint16_t kMaxBackoff = 1024U;

  uint32_t total_spin = 0;
  uint16_t spin_count = 0;
  uint16_t backoff = kInitialBackoff;

  for (;;) {
    if (!flag_.exchange(true, std::memory_order_acquire)) {
      return;
    }

    do {
      ++total_spin;

      if (++spin_count >= backoff) {
        if VUNLIKELY (total_spin > kMaxSpinCount) {
          // LCOV_EXCL_START
          // GCOVR_EXCL_START
          if (!warned_.exchange(true, std::memory_order_relaxed)) {
            VLOG_E("SpinLock: exceeded max spin count.");
          }
          std::this_thread::sleep_for(std::chrono::microseconds(10));
          // GCOVR_EXCL_STOP
          // LCOV_EXCL_STOP
        } else {
          Utils::yield_cpu();
        }

        if (backoff < kMaxBackoff) {
          backoff = static_cast<uint16_t>(backoff * 2U);
        }

        spin_count = 0;
      }
    } while (flag_.load(std::memory_order_relaxed));
  }
}

inline bool SpinLock::try_lock() noexcept {
  if VUNLIKELY (flag_.load(std::memory_order_relaxed)) {
    return false;
  }

  return !flag_.exchange(true, std::memory_order_acquire);
}

inline void SpinLock::unlock() noexcept { flag_.store(false, std::memory_order_release); }

inline SpinLockGuard::SpinLockGuard(SpinLock& lock) noexcept : lock_(lock) { lock_.lock(); }

inline SpinLockGuard::~SpinLockGuard() noexcept { lock_.unlock(); }

}  // namespace vlink
