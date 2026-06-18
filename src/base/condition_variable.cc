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

#include "./base/condition_variable.h"

#ifdef VLINK_ENABLE_BASE_CONDITION

#include <pthread.h>

#include <cerrno>
#include <chrono>
#include <ctime>
#include <memory>
#include <mutex>

namespace vlink {

// ConditionVariable
ConditionVariable::ConditionVariable() noexcept {
  pthread_condattr_t attr;

  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  pthread_cond_init(&cond_, &attr);
  pthread_condattr_destroy(&attr);
}

ConditionVariable::~ConditionVariable() noexcept { pthread_cond_destroy(&cond_); }

void ConditionVariable::notify_one() noexcept { pthread_cond_signal(&cond_); }

void ConditionVariable::notify_all() noexcept { pthread_cond_broadcast(&cond_); }

void ConditionVariable::wait(std::unique_lock<std::mutex>& lock) noexcept {
  pthread_cond_wait(&cond_, lock.mutex()->native_handle());
}

ConditionVariable::native_handle_type ConditionVariable::native_handle() noexcept { return &cond_; }

std::cv_status ConditionVariable::wait_until_steady(std::unique_lock<std::mutex>& lock,
                                                    const std::chrono::steady_clock::time_point& atime) noexcept {
  if VUNLIKELY (std::chrono::steady_clock::now() >= atime) {
    return std::cv_status::timeout;
  }

  auto s = std::chrono::time_point_cast<std::chrono::seconds>(atime);
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(atime - s);

  struct timespec ts = {static_cast<std::time_t>(s.time_since_epoch().count()),
                        static_cast<long>(ns.count())};  // NOLINT(runtime/int, google-runtime-int)

  int ret = pthread_cond_timedwait(&cond_, lock.mutex()->native_handle(), &ts);

  return (ret == ETIMEDOUT) ? std::cv_status::timeout : std::cv_status::no_timeout;
}

// ConditionVariableAny
ConditionVariableAny::ConditionVariableAny() noexcept : shared_state_(std::make_shared<SharedState>()) {}

ConditionVariableAny::~ConditionVariableAny() noexcept = default;

void ConditionVariableAny::notify_one() noexcept {
  std::shared_ptr<SharedState> state = shared_state_;
  std::lock_guard lock(state->mtx);
  state->cv.notify_one();
}

void ConditionVariableAny::notify_all() noexcept {
  std::shared_ptr<SharedState> state = shared_state_;
  std::lock_guard lock(state->mtx);
  state->cv.notify_all();
}

}  // namespace vlink

#endif
