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

#include "./base/timer.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

#include "./base/condition_variable.h"
#include "./base/logger.h"
#include "./base/memory_pool.h"
#include "./base/memory_resource.h"
#include "./base/message_loop.h"

namespace vlink {

// Timer::Impl
struct Timer::Impl final {  // NOLINT(clang-analyzer-optin.performance.Padding)
  alignas(64) std::atomic_bool is_busy{false};
  alignas(64) std::atomic<uint32_t> in_flight_count{0};
  alignas(64) std::atomic<uint64_t> start_time{0};
  alignas(64) std::atomic<int32_t> remain_loop_count{Timer::kInfinite};
  alignas(64) std::atomic<uint64_t> invoke_count{0};

  std::atomic<int32_t> loop_count{Timer::kInfinite};
  std::atomic<uint32_t> interval{1000U};
  std::atomic<uint16_t> priority{MessageLoop::kTimerPriority};
  std::atomic<MessageLoop*> message_loop{nullptr};
  std::atomic_bool is_strict{false};

  bool is_once_type{false};

  Timer::Callback callback{nullptr};

  std::mutex mtx;
  std::recursive_mutex recursive_mtx;
  ConditionVariable cv;

  std::shared_ptr<std::atomic_bool> alive_flag{MemoryResource::make_shared<std::atomic_bool>(true)};
};

// Timer
Timer::Timer() : impl_(std::make_unique<Impl>()) { MemoryPool::global_instance(); }

Timer::Timer(MessageLoop* message_loop) : impl_(std::make_unique<Impl>()) {
  MemoryPool::global_instance();
  attach(message_loop);
}

Timer::Timer(MessageLoop* message_loop, uint32_t interval_ms, int32_t loop_count, Callback&& callback)
    : impl_(std::make_unique<Impl>()) {
  impl_->interval = interval_ms;
  impl_->loop_count = loop_count;
  impl_->remain_loop_count = loop_count;
  impl_->callback = std::move(callback);

  MemoryPool::global_instance();

  attach(message_loop);
}

Timer::Timer(uint32_t interval_ms, int32_t loop_count, Callback&& callback) : impl_(std::make_unique<Impl>()) {
  impl_->interval = interval_ms;
  impl_->loop_count = loop_count;
  impl_->remain_loop_count = loop_count;
  impl_->callback = std::move(callback);

  MemoryPool::global_instance();
}

Timer::~Timer() {
  impl_->alive_flag->store(false);

  MessageLoop* message_loop = impl_->message_loop.load();

  if (message_loop && !impl_->is_once_type) {
    const bool should_wait = message_loop->is_running() && !message_loop->is_in_same_thread();

    detach();

    if (should_wait) {
      wait_for_idle();
    }
  }
}

bool Timer::call_once(MessageLoop* message_loop, uint32_t interval_ms, Callback&& callback, uint16_t priority) {
  if VUNLIKELY (!callback) {
    VLOG_E("Timer: Callback is null for call_once.");
    return false;
  }

  auto& pool = MemoryPool::global_instance();
  void* mem = pool.allocate(sizeof(Timer), alignof(Timer));

  if VUNLIKELY (!mem) {
    VLOG_E("Timer: MemoryPool allocate failed for call_once.");
    return false;
  }

  auto* timer = new (mem) Timer(interval_ms, 1, std::move(callback));

  timer->impl_->is_once_type = true;

  if (priority > 0) {
    timer->set_priority(priority);
  }

  if (timer->attach(message_loop)) {
    timer->start();
    return true;
  }

  timer->~Timer();
  pool.deallocate(mem, sizeof(Timer), alignof(Timer));

  return false;
}

bool Timer::is_active() const { return impl_->start_time != 0; }

bool Timer::is_strict() const { return impl_->is_strict; }

uint32_t Timer::get_interval() const { return impl_->interval; }

int32_t Timer::get_loop_count() const { return impl_->loop_count; }

int32_t Timer::get_remain_loop_count() const { return impl_->remain_loop_count; }

uint64_t Timer::get_invoke_count() const { return impl_->invoke_count; }

uint16_t Timer::get_priority() const { return impl_->priority; }

MessageLoop* Timer::get_message_loop() const { return impl_->message_loop.load(); }

bool Timer::attach(MessageLoop* message_loop) {
  if VUNLIKELY (!message_loop) {
    VLOG_F("Timer: MessageLoop is null.");
    return false;
  }

  MessageLoop* old_message_loop = impl_->message_loop.load();

  if (old_message_loop == message_loop) {
    return true;
  }

  if (old_message_loop) {
    stop();
    impl_->message_loop.store(nullptr);
    old_message_loop->remove_timer(this);
  }

  if VUNLIKELY (!message_loop->add_timer(this)) {
    return false;
  }

  impl_->message_loop.store(message_loop);
  return true;
}

bool Timer::detach() {
  if VUNLIKELY (impl_->is_once_type) {
    return true;
  }

  MessageLoop* message_loop = impl_->message_loop.load();

  if (message_loop) {
    stop();
    impl_->message_loop.store(nullptr);

    return message_loop->remove_timer(this);
  }

  return false;
}

void Timer::start(Callback&& callback) {
  if (callback) {
    std::lock_guard lock(impl_->recursive_mtx);
    impl_->callback = std::move(callback);
  }

  if (!is_active() && impl_->remain_loop_count != 0) {
    force_to_start();
  }
}

void Timer::restart() {
  impl_->remain_loop_count = impl_->loop_count.load();
  force_to_start();
}

void Timer::stop() {
  impl_->start_time = 0;
  impl_->invoke_count = 0;
}

void Timer::set_strict(bool strict) { impl_->is_strict = strict; }

void Timer::set_interval(uint32_t interval_ms) {
  uint32_t old_interval = impl_->interval.exchange(interval_ms, std::memory_order_acq_rel);

  if (old_interval == interval_ms) {
    return;
  }

  uint64_t interval_nano = interval_ms == 0 ? kMinInterval : static_cast<uint64_t>(interval_ms) * 1000'000U;

  uint64_t start_snapshot = impl_->start_time.load(std::memory_order_acquire);

  if (start_snapshot != 0) {
    uint64_t now_ns = MessageLoop::get_current_nano_time();

    if VLIKELY (now_ns >= start_snapshot) {
      impl_->invoke_count = (now_ns - start_snapshot) / interval_nano;
    }

    MessageLoop* message_loop = impl_->message_loop.load();

    if (message_loop) {
      message_loop->wakeup();
    }
  }
}

void Timer::set_loop_count(int32_t loop_count) {
  int32_t old_loop_count = impl_->loop_count.exchange(loop_count, std::memory_order_acq_rel);

  if (old_loop_count == loop_count) {
    return;
  }

  impl_->remain_loop_count = loop_count;

  if (is_active()) {
    MessageLoop* message_loop = impl_->message_loop.load();

    if (message_loop) {
      message_loop->wakeup();
    }
  }
}

void Timer::set_callback(Callback&& callback) {
  std::lock_guard lock(impl_->recursive_mtx);
  impl_->callback = std::move(callback);
}

void Timer::run_callback() {
  {
    std::lock_guard recursive_lock(impl_->recursive_mtx);
    std::lock_guard lock(impl_->mtx);

    impl_->is_busy = true;

    if VLIKELY (impl_->callback) {
      impl_->callback();
    }

    impl_->is_busy = false;
  }

  impl_->cv.notify_all();
}

void Timer::begin_in_flight() { impl_->in_flight_count.fetch_add(1, std::memory_order_acq_rel); }

void Timer::end_in_flight() {
  if (impl_->in_flight_count.fetch_sub(1, std::memory_order_acq_rel) == 1U) {
    std::lock_guard lock(impl_->mtx);
    impl_->cv.notify_all();
  }
}

void Timer::wait_for_idle() {
  std::unique_lock lock(impl_->mtx);
  impl_->cv.wait(lock, [this]() -> bool {
    return !impl_->is_busy && impl_->in_flight_count.load(std::memory_order_acquire) == 0;
  });
}

void Timer::clear() { impl_->message_loop.store(nullptr); }

void Timer::force_to_start() {
  if VUNLIKELY (!has_callback()) {
    VLOG_E("Timer: Callback is not set.");
    return;
  }

  impl_->start_time = MessageLoop::get_current_nano_time();
  impl_->invoke_count = 0;

  MessageLoop* message_loop = impl_->message_loop.load();

  if VLIKELY (message_loop) {
    message_loop->wakeup();
  } else {
    VLOG_E("Timer: MessageLoop is not attached.");
  }
}

void Timer::set_remain_loop_count(int32_t loop_count) const { impl_->remain_loop_count = loop_count; }

void Timer::sub_remain_loop_count() const {
  if (impl_->remain_loop_count <= 0) {
    return;
  }

  --impl_->remain_loop_count;
}

void Timer::set_invoke_count(uint64_t invoke_count) const { impl_->invoke_count = invoke_count; }

void Timer::set_priority(uint16_t priority) { impl_->priority = priority; }

uint64_t Timer::get_start_time() const { return impl_->start_time; }

bool Timer::is_once_type() const { return impl_->is_once_type; }

bool Timer::has_callback() const {
  std::lock_guard lock(impl_->recursive_mtx);
  return impl_->callback != nullptr;
}

Timer::Callback Timer::take_callback() {
  std::lock_guard lock(impl_->recursive_mtx);
  return std::exchange(impl_->callback, nullptr);
}

std::shared_ptr<std::atomic_bool> Timer::get_alive_flag() const { return impl_->alive_flag; }

}  // namespace vlink
