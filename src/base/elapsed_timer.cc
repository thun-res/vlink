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

#include "./base/elapsed_timer.h"

#include <chrono>

#ifdef _WIN32
#include <Windows.h>
#else
#include <sys/resource.h>
#include <sys/time.h>
#endif

namespace vlink {

#ifndef _WIN32
[[maybe_unused]] static uint64_t get_high_resolution_sys_timestamp() {
  ::timespec ts;

  ::clock_gettime(CLOCK_REALTIME, &ts);

  return (ts.tv_sec * 1000'000'000ULL) + ts.tv_nsec;
}

[[maybe_unused]] static uint64_t get_high_resolution_cpu_timestamp() {
  ::timespec ts;

#ifdef __linux__
  ::clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
#endif

  return (ts.tv_sec * 1000'000'000ULL) + ts.tv_nsec;
}
#endif

template <typename TypeT, typename TimeT, typename ReturnT>
static ReturnT get_current_time() noexcept {
  return static_cast<ReturnT>(std::chrono::duration_cast<TimeT>(TypeT::now().time_since_epoch()).count());
}

// ElapsedTimer
ElapsedTimer::ElapsedTimer() noexcept = default;

ElapsedTimer::ElapsedTimer(Method method) noexcept : method_(method) {}

ElapsedTimer::ElapsedTimer(Accuracy accuracy) noexcept : accuracy_(accuracy) {}

ElapsedTimer::ElapsedTimer(Method method, Accuracy accuracy) noexcept : method_(method), accuracy_(accuracy) {}

ElapsedTimer::ElapsedTimer(const ElapsedTimer& target) noexcept
    : start_time_(target.start_time_.load(std::memory_order_acquire)),
      method_(target.method_),
      accuracy_(target.accuracy_) {}

ElapsedTimer::ElapsedTimer(ElapsedTimer&& target) noexcept
    : start_time_(target.start_time_.load(std::memory_order_acquire)),
      method_(target.method_),
      accuracy_(target.accuracy_) {}

ElapsedTimer::~ElapsedTimer() noexcept = default;

ElapsedTimer& ElapsedTimer::operator=(const ElapsedTimer& target) noexcept {
  if VUNLIKELY (this == &target) {
    return *this;
  }

  method_ = target.method_;
  accuracy_ = target.accuracy_;
  start_time_.store(target.start_time_.load(std::memory_order_acquire), std::memory_order_release);

  return *this;
}

ElapsedTimer& ElapsedTimer::operator=(ElapsedTimer&& target) noexcept {
  if VUNLIKELY (this == &target) {
    return *this;
  }

  method_ = target.method_;
  accuracy_ = target.accuracy_;
  start_time_.store(target.start_time_.load(std::memory_order_acquire), std::memory_order_release);

  return *this;
}

uint64_t ElapsedTimer::get_sys_timestamp(Accuracy accuracy, bool high_resolution) noexcept {
#ifdef _WIN32
  (void)high_resolution;

  switch (accuracy) {
    case kMilli:
      return get_current_time<std::chrono::system_clock, std::chrono::milliseconds, uint64_t>();
    case kMicro:
      return get_current_time<std::chrono::system_clock, std::chrono::microseconds, uint64_t>();
    case kNano:
      return get_current_time<std::chrono::system_clock, std::chrono::nanoseconds, uint64_t>();
    default:
      return 0;
  }
#else

  if (high_resolution) {
    switch (accuracy) {
      case kMilli:
        return get_high_resolution_sys_timestamp() / 1000'000U;
      case kMicro:
        return get_high_resolution_sys_timestamp() / 1000U;
      case kNano:
        return get_high_resolution_sys_timestamp();
      default:
        return 0;
    }
  } else {
    switch (accuracy) {
      case kMilli:
        return get_current_time<std::chrono::system_clock, std::chrono::milliseconds, uint64_t>();
      case kMicro:
        return get_current_time<std::chrono::system_clock, std::chrono::microseconds, uint64_t>();
      case kNano:
        return get_current_time<std::chrono::system_clock, std::chrono::nanoseconds, uint64_t>();
      default:
        return 0;
    }
  }
#endif
}

uint64_t ElapsedTimer::get_cpu_timestamp(Accuracy accuracy, bool high_resolution) noexcept {
#ifdef _WIN32
  (void)high_resolution;

  switch (accuracy) {
    case kMilli:
      return get_current_time<std::chrono::steady_clock, std::chrono::milliseconds, uint64_t>();
    case kMicro:
      return get_current_time<std::chrono::steady_clock, std::chrono::microseconds, uint64_t>();
    case kNano:
      return get_current_time<std::chrono::steady_clock, std::chrono::nanoseconds, uint64_t>();
    default:
      return 0;
  }
#else

  if (high_resolution) {
    switch (accuracy) {
      case kMilli:
        return get_high_resolution_cpu_timestamp() / 1000'000U;
      case kMicro:
        return get_high_resolution_cpu_timestamp() / 1000U;
      case kNano:
        return get_high_resolution_cpu_timestamp();
      default:
        return 0;
    }
  } else {
    switch (accuracy) {
      case kMilli:
        return get_current_time<std::chrono::steady_clock, std::chrono::milliseconds, uint64_t>();
      case kMicro:
        return get_current_time<std::chrono::steady_clock, std::chrono::microseconds, uint64_t>();
      case kNano:
        return get_current_time<std::chrono::steady_clock, std::chrono::nanoseconds, uint64_t>();
      default:
        return 0;
    }
  }
#endif
}

uint64_t ElapsedTimer::get_cpu_active_time(Accuracy accuracy) noexcept {
#ifdef _WIN32
  FILETIME creation_time;
  FILETIME exit_time;
  FILETIME kernel_time;
  FILETIME user_time;

  if VUNLIKELY (!GetProcessTimes(GetCurrentProcess(), &creation_time, &exit_time, &kernel_time, &user_time)) {
    return 0;
  }

  static constexpr int kFileTimeToNano = 100;

  uint64_t user_time_ns =
      (static_cast<uint64_t>(user_time.dwHighDateTime) << 32 | user_time.dwLowDateTime) * kFileTimeToNano;
  uint64_t kernel_time_ns =
      (static_cast<uint64_t>(kernel_time.dwHighDateTime) << 32 | kernel_time.dwLowDateTime) * kFileTimeToNano;
  uint64_t total_time_ns = user_time_ns + kernel_time_ns;

  switch (accuracy) {
    case kMilli:
      return total_time_ns / 1000'000U;
    case kMicro:
      return total_time_ns / 1000U;
    case kNano:
      return total_time_ns;
    default:
      return total_time_ns / 1000'000U;
  }
#else
  struct rusage usage;

  if VUNLIKELY (::getrusage(RUSAGE_SELF, &usage) != 0) {
    return 0;
  }

  uint64_t user_time = (static_cast<uint64_t>(usage.ru_utime.tv_sec) * 1000'000U) + usage.ru_utime.tv_usec;
  uint64_t system_time = (static_cast<uint64_t>(usage.ru_stime.tv_sec) * 1000'000U) + usage.ru_stime.tv_usec;
  uint64_t total_time = user_time + system_time;

  switch (accuracy) {
    case kMilli:
      return total_time / 1000U;
    case kMicro:
      return total_time;
    case kNano:
      return total_time * 1000U;
    default:
      return total_time / 1000U;
  }
#endif
}

ElapsedTimer::Method ElapsedTimer::get_method() const noexcept { return method_; }

ElapsedTimer::Accuracy ElapsedTimer::get_accuracy() const noexcept { return accuracy_; }

void ElapsedTimer::start() noexcept {
  int64_t new_value;
  int64_t expected = -1;

  if (method_ == kCpuTimestamp) {
    new_value = get_cpu_timestamp(accuracy_, false);
  } else {
    new_value = get_cpu_active_time(accuracy_);
  }

  start_time_.compare_exchange_strong(expected, new_value, std::memory_order_acq_rel);
}

void ElapsedTimer::stop() noexcept { start_time_.store(-1, std::memory_order_release); }

int64_t ElapsedTimer::restart() noexcept {
  int64_t new_time;

  if (method_ == kCpuTimestamp) {
    new_time = get_cpu_timestamp(accuracy_, false);
  } else {
    new_time = get_cpu_active_time(accuracy_);
  }

  int64_t old_time = start_time_.exchange(new_time, std::memory_order_release);

  if (old_time < 0) {
    return old_time;
  } else {
    return new_time - old_time;
  }
}

bool ElapsedTimer::is_active() const noexcept { return start_time_.load(std::memory_order_acquire) >= 0; }

int64_t ElapsedTimer::get() const noexcept {
  int64_t start_time = start_time_.load(std::memory_order_acquire);

  if (start_time < 0) {
    return start_time;
  }

  if (method_ == kCpuTimestamp) {
    return get_cpu_timestamp(accuracy_, false) - start_time;
  } else {
    return get_cpu_active_time(accuracy_) - start_time;
  }
}

}  // namespace vlink
