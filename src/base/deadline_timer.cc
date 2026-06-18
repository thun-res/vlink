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

#include "./base/deadline_timer.h"

namespace vlink {

// DeadlineTimer
DeadlineTimer::DeadlineTimer() noexcept = default;

DeadlineTimer::DeadlineTimer(int64_t interval, Accuracy accuracy) noexcept : accuracy_(accuracy) {
  set_deadline(interval);
}

DeadlineTimer::DeadlineTimer(const DeadlineTimer& other) noexcept
    : deadline_(other.deadline_.load(std::memory_order_acquire)), accuracy_(other.accuracy_) {}

DeadlineTimer::DeadlineTimer(DeadlineTimer&& other) noexcept
    : deadline_(other.deadline_.load(std::memory_order_acquire)), accuracy_(other.accuracy_) {}

DeadlineTimer::~DeadlineTimer() noexcept = default;

DeadlineTimer& DeadlineTimer::operator=(const DeadlineTimer& other) noexcept {
  if VUNLIKELY (this == &other) {
    return *this;
  }

  accuracy_ = other.accuracy_;
  deadline_.store(other.deadline_.load(std::memory_order_acquire), std::memory_order_release);

  return *this;
}

DeadlineTimer& DeadlineTimer::operator=(DeadlineTimer&& other) noexcept {
  if VUNLIKELY (this == &other) {
    return *this;
  }

  accuracy_ = other.accuracy_;
  deadline_.store(other.deadline_.load(std::memory_order_acquire), std::memory_order_release);

  return *this;
}

void DeadlineTimer::set_deadline(int64_t interval) noexcept {
  if VUNLIKELY (interval <= 0) {
    deadline_.store(0, std::memory_order_release);
    return;
  }

  // Use the same monotonic time source as set_deadline_abs() callers are
  // expected to use via ElapsedTimer::get_cpu_timestamp(accuracy_).
  uint64_t start_time = ElapsedTimer::get_cpu_timestamp(accuracy_);
  uint64_t new_deadline = start_time + static_cast<uint64_t>(interval);

  deadline_.store(new_deadline, std::memory_order_release);
}

void DeadlineTimer::set_deadline_abs(uint64_t abs_deadline) noexcept {
  deadline_.store(abs_deadline, std::memory_order_release);
}

void DeadlineTimer::reset() noexcept { deadline_.store(0, std::memory_order_release); }

uint64_t DeadlineTimer::deadline() const noexcept { return deadline_.load(std::memory_order_acquire); }

int64_t DeadlineTimer::remaining_time() const noexcept {
  uint64_t deadline_val = deadline_.load(std::memory_order_acquire);

  if (deadline_val == 0) {
    return 0;
  }

  uint64_t now = ElapsedTimer::get_cpu_timestamp(accuracy_);

  if (now >= deadline_val) {
    return 0;
  }

  return static_cast<int64_t>(deadline_val - now);
}

bool DeadlineTimer::has_expired() const noexcept {
  uint64_t deadline_val = deadline_.load(std::memory_order_acquire);

  if (deadline_val == 0) {
    return false;
  }

  uint64_t now = ElapsedTimer::get_cpu_timestamp(accuracy_);

  return now >= deadline_val;
}

bool DeadlineTimer::is_valid() const noexcept { return deadline_.load(std::memory_order_acquire) != 0; }

DeadlineTimer::Accuracy DeadlineTimer::get_accuracy() const noexcept { return accuracy_; }

}  // namespace vlink
