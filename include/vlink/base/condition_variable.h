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
 * @file condition_variable.h
 * @brief Monotonic-clock condition variable replacement immune to system clock jumps.
 *
 * @details
 * Older libstdc++ implementations route every @c std::condition_variable timed wait through
 * @c CLOCK_REALTIME, so an NTP step or manual date change can spuriously wake or starve waiters
 * (GCC PR 41861 / DR 887).  On POSIX systems this header substitutes a hand-rolled
 * @c vlink::ConditionVariable backed by a @c pthread_cond_t configured with
 * @c pthread_condattr_setclock @c (..., @c CLOCK_MONOTONIC).  On Windows the bug does not apply
 * and the names alias to the standard library types verbatim.
 *
 * @par API surface vs @c std::condition_variable
 *
 * | Aspect            | @c std::condition_variable              | @c vlink::ConditionVariable                |
 * | ----------------- | --------------------------------------- | ------------------------------------------ |
 * | Backing clock     | @c CLOCK_REALTIME (libstdc++)           | @c CLOCK_MONOTONIC via @c pthread_condattr |
 * | Timed waits       | Sensitive to wall clock jumps           | Immune to wall clock jumps                 |
 * | Copy / move       | Deleted                                 | Deleted                                    |
 * | Public methods    | wait / wait_for / wait_until / notify_* | Same signatures, same return types         |
 * | Native handle     | @c pthread_cond_t*                      | @c pthread_cond_t*                         |
 *
 * @par Wait / notify sequence
 *
 * @verbatim
 *   producer thread                consumer thread
 *   ---------------                 ---------------
 *   lock(mtx)                       lock(mtx)
 *   state = ready                   cv.wait(lock, predicate)
 *   unlock(mtx)                       releases mtx, blocks
 *   cv.notify_one()  ----------->     wakes; reacquires mtx
 *                                     re-checks predicate
 *                                     returns to caller
 * @endverbatim
 *
 * Both @c ConditionVariable and @c ConditionVariableAny accept any @c std::chrono clock type;
 * non-steady clock arguments are projected onto @c steady_clock at entry and re-checked at exit
 * for timeout fidelity.  Convenience type aliases @c vlink::condition_variable and
 * @c vlink::condition_variable_any are also exposed.
 *
 * @par Example
 * @code
 *   std::mutex mtx;
 *   vlink::ConditionVariable cv;
 *   bool ready = false;
 *
 *   // Consumer:
 *   {
 *     std::unique_lock lock(mtx);
 *     cv.wait_for(lock, std::chrono::milliseconds(200), [&] { return ready; });
 *   }
 *
 *   // Producer:
 *   {
 *     std::lock_guard lock(mtx);
 *     ready = true;
 *   }
 *   cv.notify_one();
 * @endcode
 */

#pragma once

#include <condition_variable>

#if !defined(VLINK_ENABLE_BASE_CONDITION) && defined(__unix__) && !defined(__CYGWIN__)
#define VLINK_ENABLE_BASE_CONDITION
#endif

#ifdef VLINK_ENABLE_BASE_CONDITION
#include <pthread.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <utility>

#include "./macros.h"

namespace vlink {

/**
 * @class ConditionVariable
 * @brief pthread-backed condition variable using @c CLOCK_MONOTONIC for all timed waits.
 *
 * @details
 * Provides the same public interface as @c std::condition_variable.  Internally owns a single
 * @c pthread_cond_t configured at construction with @c pthread_condattr_setclock so the kernel
 * uses the monotonic clock when timing out waiters; wall-clock arguments are projected onto the
 * monotonic clock to preserve fidelity.
 */
class VLINK_EXPORT ConditionVariable final {
 public:
  /**
   * @brief Native handle type returned by @c native_handle.
   */
  using native_handle_type = pthread_cond_t*;

  ConditionVariable(const ConditionVariable&) noexcept = delete;

  ConditionVariable& operator=(const ConditionVariable&) noexcept = delete;

  /**
   * @brief Constructs and initialises the underlying @c pthread_cond_t with @c CLOCK_MONOTONIC.
   */
  ConditionVariable() noexcept;

  /**
   * @brief Destroys the underlying @c pthread_cond_t.
   */
  ~ConditionVariable() noexcept;

  /**
   * @brief Wakes a single thread currently blocked on this condition variable.
   */
  void notify_one() noexcept;

  /**
   * @brief Wakes every thread currently blocked on this condition variable.
   */
  void notify_all() noexcept;

  /**
   * @brief Atomically releases @p lock and suspends until a notification arrives.
   *
   * @param lock  Held lock to release across the wait.
   */
  void wait(std::unique_lock<std::mutex>& lock) noexcept;

  /**
   * @brief Waits in a predicate loop until @p p reports @c true.
   *
   * @tparam PredicateT  Nullary callable returning @c bool.
   * @param lock  Held lock to release across the wait.
   * @param p     Predicate evaluated on each wakeup.
   */
  template <typename PredicateT>
  void wait(std::unique_lock<std::mutex>& lock, PredicateT p) noexcept;

  /**
   * @brief Steady-clock wait_until overload; the deadline is honoured directly.
   *
   * @tparam DurationT  Duration type of the time point.
   * @param lock   Held lock to release across the wait.
   * @param atime  Absolute steady-clock deadline.
   * @return @c std::cv_status::timeout when the deadline passed, otherwise @c no_timeout.
   */
  template <typename DurationT>
  std::cv_status wait_until(std::unique_lock<std::mutex>& lock,
                            const std::chrono::time_point<std::chrono::steady_clock, DurationT>& atime) noexcept;

  /**
   * @brief System-clock wait_until overload; the deadline is projected onto steady-clock.
   *
   * @tparam DurationT  Duration type of the time point.
   * @param lock   Held lock to release across the wait.
   * @param atime  Absolute system-clock deadline.
   * @return @c std::cv_status::timeout when the deadline passed, otherwise @c no_timeout.
   */
  template <typename DurationT>
  std::cv_status wait_until(std::unique_lock<std::mutex>& lock,
                            const std::chrono::time_point<std::chrono::system_clock, DurationT>& atime) noexcept;

  /**
   * @brief Generic clock wait_until overload; the deadline is projected onto steady-clock.
   *
   * @tparam ClockT     Clock type of the time point.
   * @tparam DurationT  Duration type of the time point.
   * @param lock   Held lock to release across the wait.
   * @param atime  Absolute deadline in @c ClockT.
   * @return @c std::cv_status::timeout or @c no_timeout.
   */
  template <typename ClockT, typename DurationT>
  std::cv_status wait_until(std::unique_lock<std::mutex>& lock,
                            const std::chrono::time_point<ClockT, DurationT>& atime) noexcept;

  /**
   * @brief Predicate-driven wait_until that exits when @p p reports @c true or the deadline elapses.
   *
   * @tparam ClockT      Clock type of the deadline.
   * @tparam DurationT   Duration type of the deadline.
   * @tparam PredicateT  Nullary callable returning @c bool.
   * @param lock   Held lock to release across the wait.
   * @param atime  Absolute deadline.
   * @param p      Predicate evaluated on each wakeup.
   * @return Final value of @p p when the call returns.
   */
  template <typename ClockT, typename DurationT, typename PredicateT>
  bool wait_until(std::unique_lock<std::mutex>& lock, const std::chrono::time_point<ClockT, DurationT>& atime,
                  PredicateT p) noexcept;

  /**
   * @brief Relative wait_for overload anchored on the steady-clock.
   *
   * @tparam RepT     Duration representation.
   * @tparam PeriodT  Duration period.
   * @param lock   Held lock to release across the wait.
   * @param rtime  Maximum wait duration.
   * @return @c std::cv_status::timeout or @c no_timeout.
   */
  template <typename RepT, typename PeriodT>
  std::cv_status wait_for(std::unique_lock<std::mutex>& lock,
                          const std::chrono::duration<RepT, PeriodT>& rtime) noexcept;

  /**
   * @brief Predicate-driven wait_for that exits when @p p reports @c true or @p rtime elapses.
   *
   * @tparam RepT       Duration representation.
   * @tparam PeriodT    Duration period.
   * @tparam PredicateT Nullary callable returning @c bool.
   * @param lock   Held lock to release across the wait.
   * @param rtime  Maximum wait duration.
   * @param p      Predicate evaluated on each wakeup.
   * @return Final value of @p p when the call returns.
   */
  template <typename RepT, typename PeriodT, typename PredicateT>
  bool wait_for(std::unique_lock<std::mutex>& lock, const std::chrono::duration<RepT, PeriodT>& rtime,
                PredicateT p) noexcept;

  /**
   * @brief Returns the underlying @c pthread_cond_t pointer.
   *
   * @return Pointer to the internal native condition variable.
   */
  [[nodiscard]] native_handle_type native_handle() noexcept;

 private:
  template <typename ToDurT, typename RepT, typename PeriodT>
  static constexpr ToDurT ceil(const std::chrono::duration<RepT, PeriodT>& d) noexcept;

  template <typename TpT, typename UpT>
  static constexpr TpT ceil_impl(const TpT& t, const UpT& u) noexcept;

  std::cv_status wait_until_steady(std::unique_lock<std::mutex>& lock,
                                   const std::chrono::steady_clock::time_point& atime) noexcept;

  pthread_cond_t cond_{};
};

/**
 * @class ConditionVariableAny
 * @brief Monotonic-clock condition variable accepting any @c BasicLockable.
 *
 * @details
 * Mirrors @c std::condition_variable_any while routing all timed waits through the same
 * pthread-backed monotonic condition variable used by @c ConditionVariable.  An internal
 * @c std::mutex pairs with the shared cv so arbitrary lockables can be unlocked across the
 * wait and relocked on return.
 *
 * @note Behaviour is unspecified if destruction races with any other member function (matches
 *       @c std::condition_variable_any per @c [thread.condition.condvarany]).
 */
class VLINK_EXPORT ConditionVariableAny final {
 public:
  ConditionVariableAny(const ConditionVariableAny&) noexcept = delete;

  ConditionVariableAny& operator=(const ConditionVariableAny&) noexcept = delete;

  /**
   * @brief Constructs the shared state and underlying condition variable.
   */
  ConditionVariableAny() noexcept;

  /**
   * @brief Destructor.
   */
  ~ConditionVariableAny() noexcept;

  /**
   * @brief Wakes a single thread blocked on this condition variable.
   */
  void notify_one() noexcept;

  /**
   * @brief Wakes every thread blocked on this condition variable.
   */
  void notify_all() noexcept;

  /**
   * @brief Atomically releases @p lock and suspends until a notification arrives.
   *
   * @tparam LockT  Any @c BasicLockable type.
   * @param lock  Held lock to release across the wait.
   */
  template <typename LockT>
  void wait(LockT& lock) noexcept;

  /**
   * @brief Predicate wait variant that loops until @p p returns @c true.
   *
   * @tparam LockT       Any @c BasicLockable type.
   * @tparam PredicateT  Nullary callable returning @c bool.
   * @param lock  Held lock to release across the wait.
   * @param p     Predicate evaluated on each wakeup.
   */
  template <typename LockT, typename PredicateT>
  void wait(LockT& lock, PredicateT p) noexcept;

  /**
   * @brief Generic clock wait_until variant.
   *
   * @tparam LockT      Any @c BasicLockable type.
   * @tparam ClockT     Clock type of the deadline.
   * @tparam DurationT  Duration type of the deadline.
   * @param lock   Held lock to release across the wait.
   * @param atime  Absolute deadline.
   * @return @c std::cv_status::timeout or @c no_timeout.
   */
  template <typename LockT, typename ClockT, typename DurationT>
  std::cv_status wait_until(LockT& lock, const std::chrono::time_point<ClockT, DurationT>& atime) noexcept;

  /**
   * @brief Predicate-driven wait_until variant.
   *
   * @tparam LockT       Any @c BasicLockable type.
   * @tparam ClockT      Clock type of the deadline.
   * @tparam DurationT   Duration type of the deadline.
   * @tparam PredicateT  Nullary callable returning @c bool.
   * @param lock   Held lock to release across the wait.
   * @param atime  Absolute deadline.
   * @param p      Predicate evaluated on each wakeup.
   * @return Final value of @p p when the call returns.
   */
  template <typename LockT, typename ClockT, typename DurationT, typename PredicateT>
  bool wait_until(LockT& lock, const std::chrono::time_point<ClockT, DurationT>& atime, PredicateT p) noexcept;

  /**
   * @brief Relative wait_for variant.
   *
   * @tparam LockT    Any @c BasicLockable type.
   * @tparam RepT     Duration representation.
   * @tparam PeriodT  Duration period.
   * @param lock   Held lock to release across the wait.
   * @param rtime  Maximum wait duration.
   * @return @c std::cv_status::timeout or @c no_timeout.
   */
  template <typename LockT, typename RepT, typename PeriodT>
  std::cv_status wait_for(LockT& lock, const std::chrono::duration<RepT, PeriodT>& rtime) noexcept;

  /**
   * @brief Predicate-driven wait_for variant.
   *
   * @tparam LockT       Any @c BasicLockable type.
   * @tparam RepT        Duration representation.
   * @tparam PeriodT     Duration period.
   * @tparam PredicateT  Nullary callable returning @c bool.
   * @param lock   Held lock to release across the wait.
   * @param rtime  Maximum wait duration.
   * @param p      Predicate evaluated on each wakeup.
   * @return Final value of @p p when the call returns.
   */
  template <typename LockT, typename RepT, typename PeriodT, typename PredicateT>
  bool wait_for(LockT& lock, const std::chrono::duration<RepT, PeriodT>& rtime, PredicateT p) noexcept;

 private:
  template <typename ToDurT, typename RepT, typename PeriodT>
  static constexpr ToDurT ceil(const std::chrono::duration<RepT, PeriodT>& d) noexcept;

  template <typename TpT, typename UpT>
  static constexpr TpT ceil_impl(const TpT& t, const UpT& u) noexcept;

  template <typename LockT, typename DurationT>
  std::cv_status wait_until_impl(LockT& lock,
                                 const std::chrono::time_point<std::chrono::steady_clock, DurationT>& atime) noexcept;

  struct SharedState final {
    std::mutex mtx;
    ConditionVariable cv;
  };

  std::shared_ptr<SharedState> shared_state_;
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

template <typename PredicateT>
inline void ConditionVariable::wait(std::unique_lock<std::mutex>& lock, PredicateT p) noexcept {
  while (!p()) {
    wait(lock);
  }
}

template <typename DurationT>
inline std::cv_status ConditionVariable::wait_until(
    std::unique_lock<std::mutex>& lock,
    const std::chrono::time_point<std::chrono::steady_clock, DurationT>& atime) noexcept {
  return wait_until_steady(lock, std::chrono::time_point_cast<std::chrono::steady_clock::duration>(atime));
}

template <typename DurationT>
inline std::cv_status ConditionVariable::wait_until(
    std::unique_lock<std::mutex>& lock,
    const std::chrono::time_point<std::chrono::system_clock, DurationT>& atime) noexcept {
  return wait_until<std::chrono::system_clock, DurationT>(lock, atime);
}

template <typename ClockT, typename DurationT>
inline std::cv_status ConditionVariable::wait_until(std::unique_lock<std::mutex>& lock,
                                                    const std::chrono::time_point<ClockT, DurationT>& atime) noexcept {
  const typename ClockT::time_point c_entry = ClockT::now();
  const std::chrono::steady_clock::time_point s_entry = std::chrono::steady_clock::now();
  const auto delta = atime - c_entry;
  const auto s_atime = s_entry + ceil<std::chrono::steady_clock::duration>(delta);

  if (wait_until_steady(lock, s_atime) == std::cv_status::no_timeout) {
    return std::cv_status::no_timeout;
  }

  if (ClockT::now() < atime) {
    return std::cv_status::no_timeout;
  }

  return std::cv_status::timeout;
}

template <typename ClockT, typename DurationT, typename PredicateT>
inline bool ConditionVariable::wait_until(std::unique_lock<std::mutex>& lock,
                                          const std::chrono::time_point<ClockT, DurationT>& atime,
                                          PredicateT p) noexcept {
  while (!p()) {
    if (wait_until(lock, atime) == std::cv_status::timeout) {
      return p();
    }
  }

  return true;
}

template <typename RepT, typename PeriodT>
inline std::cv_status ConditionVariable::wait_for(std::unique_lock<std::mutex>& lock,
                                                  const std::chrono::duration<RepT, PeriodT>& rtime) noexcept {
  return wait_until(lock, std::chrono::steady_clock::now() + ceil<std::chrono::steady_clock::duration>(rtime));
}

template <typename RepT, typename PeriodT, typename PredicateT>
inline bool ConditionVariable::wait_for(std::unique_lock<std::mutex>& lock,
                                        const std::chrono::duration<RepT, PeriodT>& rtime, PredicateT p) noexcept {
  return wait_until(lock, std::chrono::steady_clock::now() + ceil<std::chrono::steady_clock::duration>(rtime),
                    std::move(p));
}

template <typename ToDurT, typename RepT, typename PeriodT>
inline constexpr ToDurT ConditionVariable::ceil(const std::chrono::duration<RepT, PeriodT>& d) noexcept {
  return ceil_impl(std::chrono::duration_cast<ToDurT>(d), d);
}

template <typename TpT, typename UpT>
inline constexpr TpT ConditionVariable::ceil_impl(const TpT& t, const UpT& u) noexcept {
  return (t < u) ? (t + TpT{1}) : t;
}

template <typename LockT>
inline void ConditionVariableAny::wait(LockT& lock) noexcept {
  std::shared_ptr<SharedState> state = shared_state_;
  std::unique_lock internal_lock(state->mtx);
  lock.unlock();

  struct UnlockGuard final {
    LockT& lock_ref;

    ~UnlockGuard() noexcept {
      try {
        lock_ref.lock();
      } catch (std::exception&) {
      }
    }
  } guard{lock};

  state->cv.wait(internal_lock);
}

template <typename LockT, typename PredicateT>
inline void ConditionVariableAny::wait(LockT& lock, PredicateT p) noexcept {
  while (!p()) {
    wait(lock);
  }
}

template <typename LockT, typename ClockT, typename DurationT>
inline std::cv_status ConditionVariableAny::wait_until(
    LockT& lock, const std::chrono::time_point<ClockT, DurationT>& atime) noexcept {
  if constexpr (std::is_same_v<ClockT, std::chrono::steady_clock>) {
    return wait_until_impl(lock, atime);
  } else {
    const typename ClockT::time_point c_entry = ClockT::now();
    const std::chrono::steady_clock::time_point s_entry = std::chrono::steady_clock::now();
    const auto delta = atime - c_entry;
    const auto s_atime = s_entry + ceil<std::chrono::steady_clock::duration>(delta);

    if (wait_until_impl(lock, s_atime) == std::cv_status::no_timeout) {
      return std::cv_status::no_timeout;
    }

    if (ClockT::now() < atime) {
      return std::cv_status::no_timeout;
    }

    return std::cv_status::timeout;
  }
}

template <typename LockT, typename ClockT, typename DurationT, typename PredicateT>
inline bool ConditionVariableAny::wait_until(LockT& lock, const std::chrono::time_point<ClockT, DurationT>& atime,
                                             PredicateT p) noexcept {
  while (!p()) {
    if (wait_until(lock, atime) == std::cv_status::timeout) {
      return p();
    }
  }

  return true;
}

template <typename LockT, typename RepT, typename PeriodT>
inline std::cv_status ConditionVariableAny::wait_for(LockT& lock,
                                                     const std::chrono::duration<RepT, PeriodT>& rtime) noexcept {
  return wait_until(lock, std::chrono::steady_clock::now() + ceil<std::chrono::steady_clock::duration>(rtime));
}

template <typename LockT, typename RepT, typename PeriodT, typename PredicateT>
inline bool ConditionVariableAny::wait_for(LockT& lock, const std::chrono::duration<RepT, PeriodT>& rtime,
                                           PredicateT p) noexcept {
  return wait_until(lock, std::chrono::steady_clock::now() + ceil<std::chrono::steady_clock::duration>(rtime),
                    std::move(p));
}

template <typename ToDurT, typename RepT, typename PeriodT>
inline constexpr ToDurT ConditionVariableAny::ceil(const std::chrono::duration<RepT, PeriodT>& d) noexcept {
  return ceil_impl(std::chrono::duration_cast<ToDurT>(d), d);
}

template <typename TpT, typename UpT>
inline constexpr TpT ConditionVariableAny::ceil_impl(const TpT& t, const UpT& u) noexcept {
  return (t < u) ? (t + TpT{1}) : t;
}

template <typename LockT, typename DurationT>
inline std::cv_status ConditionVariableAny::wait_until_impl(
    LockT& lock, const std::chrono::time_point<std::chrono::steady_clock, DurationT>& atime) noexcept {
  if (std::chrono::steady_clock::now() >= atime) {
    return std::cv_status::timeout;
  }

  std::shared_ptr<SharedState> state = shared_state_;
  std::unique_lock internal_lock(state->mtx);
  lock.unlock();

  struct UnlockGuard final {
    LockT& lock_ref;

    ~UnlockGuard() noexcept {
      try {
        lock_ref.lock();
      } catch (std::exception&) {
      }
    }
  } guard{lock};

  return state->cv.wait_until(internal_lock, atime);
}

/**
 * @typedef condition_variable
 * @brief Snake-case alias for @c ConditionVariable.
 */
using condition_variable = ConditionVariable;

/**
 * @typedef condition_variable_any
 * @brief Snake-case alias for @c ConditionVariableAny.
 */
using condition_variable_any = ConditionVariableAny;

}  // namespace vlink

#else

namespace vlink {

using ConditionVariable = std::condition_variable;
using ConditionVariableAny = std::condition_variable_any;
using condition_variable = ConditionVariable;
using condition_variable_any = ConditionVariableAny;

}  // namespace vlink

#endif
