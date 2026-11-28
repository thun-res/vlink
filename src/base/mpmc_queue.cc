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

#include "./base/mpmc_queue.h"

#include <chrono>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>

#include "./base/utils.h"

namespace vlink {

// MpmcQueueBase
size_t MpmcQueueBase::capacity() const noexcept { return capacity_; }

size_t MpmcQueueBase::size(bool real) const noexcept {
  static auto safe_diff = [](size_t h, size_t t) -> size_t {
    return (h >= t) ? (h - t) : 0U;
  };  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

  if (real) {
    static constexpr size_t kMaxRetry = 50U;

    size_t retry_cnt = 0;

    size_t t = tail_.load(kMemoryOrderRelaxed);
    size_t h = head_.load(kMemoryOrderRelaxed);

    while (retry_cnt < kMaxRetry) {
      size_t t2 = tail_.load(kMemoryOrderRelaxed);

      if (t == t2) {
        return safe_diff(h, t);
      }

      // LCOV_EXCL_START GCOVR_EXCL_START
      t = t2;
      h = head_.load(kMemoryOrderRelaxed);
      retry_cnt++;

      Utils::yield_cpu();
    }

    return safe_diff(h, t);
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  auto h = head_.load(kMemoryOrderRelaxed);
  auto t = tail_.load(kMemoryOrderRelaxed);

  return safe_diff(h, t);
}

bool MpmcQueueBase::empty(bool real) const noexcept { return size(real) == 0; }

bool MpmcQueueBase::is_full(bool real) const noexcept { return size(real) >= capacity_; }

bool MpmcQueueBase::wait_not_empty(std::chrono::milliseconds timeout) noexcept {
  if (!empty(true)) {
    return !quit_flag_.value.load(kMemoryOrderAcquire);
  }

  std::unique_lock lock(cv_mtx_);

  bool ret = true;

  if (timeout == std::chrono::milliseconds(0)) {
    cv_not_empty_.wait(lock, [this]() { return !empty(true) || quit_flag_.value.load(kMemoryOrderAcquire); });
  } else {
    ret = cv_not_empty_.wait_for(lock, timeout, [this]() {
      return !empty(true) || quit_flag_.value.load(kMemoryOrderAcquire);
    });  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if VUNLIKELY (quit_flag_.value.load(kMemoryOrderAcquire)) {
    return false;
  }

  return ret;
}

bool MpmcQueueBase::wait_not_full(std::chrono::milliseconds timeout) noexcept {
  if (!is_full(true)) {
    return !quit_flag_.value.load(kMemoryOrderAcquire);
  }

  std::unique_lock lock(cv_mtx_);

  bool ret = true;

  if (timeout == std::chrono::milliseconds(0)) {
    cv_not_full_.wait(lock, [this]() { return !is_full(true) || quit_flag_.value.load(kMemoryOrderAcquire); });
  } else {
    ret = cv_not_full_.wait_for(lock, timeout, [this]() {
      return !is_full(true) || quit_flag_.value.load(kMemoryOrderAcquire);
    });  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if VUNLIKELY (quit_flag_.value.load(kMemoryOrderAcquire)) {
    return false;
  }

  return ret;
}

void MpmcQueueBase::notify_to_quit() noexcept {
  std::lock_guard lock(cv_mtx_);

  quit_flag_.value.store(true, kMemoryOrderRelease);

  cv_not_empty_.notify_all();
  cv_not_full_.notify_all();
}

MpmcQueueBase::MpmcQueueBase(size_t capacity) : capacity_(capacity) {
  if VUNLIKELY (capacity_ < 1U || capacity_ == std::numeric_limits<size_t>::max()) {
    throw_mpmc_invalid_capacity();
  }

  mask_ = capacity_ - 1U;
  is_power_of_two_ = (capacity_ & mask_) == 0U;

  if (is_power_of_two_) {
    size_t value = capacity_;

    while (value > 1U) {
      value >>= 1U;
      ++shift_;
    }

    return;
  }

  if constexpr (sizeof(size_t) == 8U) {
    const auto divisor = static_cast<uint64_t>(capacity_);
    uint8_t floor_log2 = 0U;
    uint64_t probe = divisor;

    while (probe > 1U) {
      probe >>= 1U;
      ++floor_log2;
    }

    auto divide_shifted = [divisor](uint64_t initial_rem) noexcept -> std::pair<uint64_t, uint64_t> {
      uint64_t quotient = 0U;
      uint64_t rem = initial_rem;

      for (int bit = 63; bit >= 0; --bit) {
        const bool high_bit = (rem >> 63U) != 0U;
        rem <<= 1U;

        if (high_bit || rem >= divisor) {
          rem -= divisor;
          quotient |= (uint64_t{1} << static_cast<unsigned int>(bit));
        }
      }

      return {quotient, rem};
    };

    const uint64_t pow_l = uint64_t{1} << floor_log2;
    const auto [q1, r1] = divide_shifted(pow_l);
    const uint64_t e1 = divisor - r1;

    if (e1 <= pow_l) {
      div_magic_ = q1 + 1U;
      div_shift_ = floor_log2;
      div_add_ = false;
    } else {
      const uint64_t rem_start = (pow_l - divisor) + pow_l;
      const auto [q2, r2] = divide_shifted(rem_start);
      (void)r2;

      div_magic_ = q2 + 1U;
      div_shift_ = floor_log2;
      div_add_ = true;
    }
  }
}

void MpmcQueueBase::throw_mpmc_invalid_capacity() { throw std::invalid_argument("capacity must be in [1, SIZE_MAX)"); }

void MpmcQueueBase::throw_mpmc_alignment_failure() { throw std::bad_alloc(); }

}  // namespace vlink
