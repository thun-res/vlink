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
#include <mutex>
#include <new>
#include <stdexcept>

#include "./base/utils.h"

namespace vlink {

// MpmcQueueBase
MpmcQueueBase::MpmcQueueBase(size_t capacity) : capacity_(capacity) {
  if VUNLIKELY (capacity_ < 1U) {
    throw_mpmc_invalid_capacity();
  }
}

void MpmcQueueBase::throw_mpmc_invalid_capacity() { throw std::invalid_argument("capacity < 1U"); }

void MpmcQueueBase::throw_mpmc_alignment_failure() { throw std::bad_alloc(); }

size_t MpmcQueueBase::capacity() const noexcept { return capacity_; }

size_t MpmcQueueBase::size(bool real) const noexcept {
  static auto safe_diff = [](size_t h, size_t t) -> size_t { return (h >= t) ? (h - t) : 0U; };

  if (real) {
    static constexpr size_t kMaxRetry = 50U;

    size_t retry_cnt = 0;

    size_t t = tail_.load(kMemoryOrderAcquire);
    size_t h = head_.load(kMemoryOrderAcquire);

    while (retry_cnt < kMaxRetry) {
      size_t t2 = tail_.load(kMemoryOrderAcquire);

      if (t == t2) {
        return safe_diff(h, t);
      }

      t = t2;
      h = head_.load(kMemoryOrderAcquire);
      retry_cnt++;

      Utils::yield_cpu();
    }

    return safe_diff(h, t);
  }

  auto h = head_.load(kMemoryOrderAcquire);
  auto t = tail_.load(kMemoryOrderAcquire);

  return safe_diff(h, t);
}

bool MpmcQueueBase::empty(bool real) const noexcept { return size(real) == 0; }

bool MpmcQueueBase::is_full(bool real) const noexcept { return size(real) >= capacity_; }

bool MpmcQueueBase::wait_not_empty(std::chrono::milliseconds timeout) noexcept {
  if (!empty(true)) {
    return true;
  }

  std::unique_lock lock(cv_mtx_);

  bool ret = true;

  if (timeout == std::chrono::milliseconds(0)) {
    cv_not_empty_.wait(lock, [this]() { return !empty(true) || quit_flag_.value.load(kMemoryOrderAcquire); });
  } else {
    ret = cv_not_empty_.wait_for(lock, timeout,
                                 [this]() { return !empty(true) || quit_flag_.value.load(kMemoryOrderAcquire); });
  }

  if VUNLIKELY (quit_flag_.value.load(kMemoryOrderAcquire)) {
    return false;
  }

  return ret;
}

bool MpmcQueueBase::wait_not_full(std::chrono::milliseconds timeout) noexcept {
  if (!is_full(true)) {
    return true;
  }

  std::unique_lock lock(cv_mtx_);

  bool ret = true;

  if (timeout == std::chrono::milliseconds(0)) {
    cv_not_full_.wait(lock, [this]() { return !is_full(true) || quit_flag_.value.load(kMemoryOrderAcquire); });
  } else {
    ret = cv_not_full_.wait_for(lock, timeout,
                                [this]() { return !is_full(true) || quit_flag_.value.load(kMemoryOrderAcquire); });
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

}  // namespace vlink
