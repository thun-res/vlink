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

#include "./base/semaphore.h"

#include <chrono>
#include <mutex>

#include "./base/condition_variable.h"

namespace vlink {

// Semaphore::Impl
struct Semaphore::Impl final {
  mutable std::mutex mtx;
  ConditionVariable cv;
  size_t count{0};
  size_t initial_count{0};
  size_t in_flight_count{0};
  bool quit_flag{false};
};

// Semaphore
Semaphore::Semaphore(size_t count) noexcept : impl_(std::make_unique<Impl>()) {
  impl_->count = count;
  impl_->initial_count = count;
}

Semaphore::~Semaphore() noexcept {
  std::unique_lock lock(impl_->mtx);
  impl_->quit_flag = true;
  impl_->cv.notify_all();
  impl_->cv.wait(lock, [this]() -> bool { return impl_->in_flight_count == 0; });
}

bool Semaphore::acquire(size_t n, int timeout_ms) noexcept {
  std::unique_lock lock(impl_->mtx);
  ++impl_->in_flight_count;

  auto predicate = [this, n] { return impl_->count >= n || impl_->quit_flag; };

  bool acquired;

  if (timeout_ms < 0) {
    impl_->cv.wait(lock, predicate);
    acquired = (impl_->count >= n);
  } else {
    acquired = impl_->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), predicate) && (impl_->count >= n);
  }

  bool result;

  if VUNLIKELY (impl_->quit_flag) {
    result = false;
  } else {
    if (acquired) {
      impl_->count -= n;
    }

    result = acquired;
  }

  if (--impl_->in_flight_count == 0) {
    impl_->cv.notify_all();
  }

  return result;
}

void Semaphore::release(size_t n) noexcept {
  {
    std::lock_guard lock(impl_->mtx);
    impl_->count += n;
  }

  impl_->cv.notify_all();
}

void Semaphore::reset(bool interrupt_waiters) noexcept {
  {
    std::lock_guard lock(impl_->mtx);
    impl_->count = impl_->initial_count;
    impl_->quit_flag = interrupt_waiters;
  }

  if (interrupt_waiters || impl_->initial_count > 0) {
    impl_->cv.notify_all();
  }
}

size_t Semaphore::get_count() const noexcept {
  std::lock_guard lock(impl_->mtx);

  return impl_->count;
}

}  // namespace vlink
