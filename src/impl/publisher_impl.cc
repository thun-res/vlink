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

#include "./impl/publisher_impl.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>

#include "./base/condition_variable.h"
#include "./base/utils.h"

namespace vlink {

// PublisherImplHelper
struct PublisherImplHelper final {
  std::atomic_bool has_subscribers{false};
  NodeImpl::ConnectCallback connected_callback;
  ConditionVariable connected_cv;
  std::mutex mtx;
  std::recursive_mutex callback_mtx;
};

// PublisherImpl
PublisherImpl::~PublisherImpl() = default;

void PublisherImpl::interrupt() {
  {
    std::lock_guard sync_lock(helper_->mtx);
    NodeImpl::interrupt();
  }

  helper_->connected_cv.notify_all();
}

void PublisherImpl::detect_subscribers(ConnectCallback&& callback) {
  std::unique_lock lock(helper_->callback_mtx);
  helper_->connected_callback = std::move(callback);

  if (helper_->connected_callback && helper_->has_subscribers.load(std::memory_order_acquire)) {
    auto callback_copy = helper_->connected_callback;
    lock.unlock();
    callback_copy(true);
  }
}

bool PublisherImpl::wait_for_subscribers(std::chrono::milliseconds timeout) {
  const auto generation = get_interrupt_generation();
  auto interrupted = [&]() { return get_interrupt_generation() != generation; };

  if VLIKELY (has_subscribers()) {
    return !interrupted();
  }

  Utils::yield_cpu();

  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  const auto max_wait = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - start);
  const auto deadline = timeout.count() < 0 || timeout >= max_wait ? Clock::time_point::max() : start + timeout;

  std::unique_lock lock(helper_->mtx);

  if VUNLIKELY (is_interrupted() && !interrupted()) {
    reset_interrupted();
  }

  auto ready = [&]() { return helper_->has_subscribers.load(std::memory_order_acquire) || interrupted(); };

  for (;;) {
    if (deadline == Clock::time_point::max()) {
      helper_->connected_cv.wait(lock, ready);
    } else if (!helper_->connected_cv.wait_until(lock, deadline, ready)) {
      return false;
    }

    if VUNLIKELY (interrupted()) {
      return false;
    }

    lock.unlock();

    if (has_subscribers()) {
      return !interrupted();
    }

    lock.lock();

    const auto now = Clock::now();

    if (now >= deadline) {
      return false;
    }

    helper_->connected_cv.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(50)), [&]() {
      return !helper_->has_subscribers.load(std::memory_order_acquire) || interrupted();
    });
  }
}

bool PublisherImpl::write(Bytes&& msg_data) { return write(msg_data); }

bool PublisherImpl::write(const IntraData& intra_data) {
  (void)intra_data;

  VLOG_W("Function [write(const IntraData&)] is not supported.");

  return false;
}

void PublisherImpl::update_subscribers() {
  Utils::yield_cpu();

  std::unique_lock lock(helper_->callback_mtx);
  const bool has_subscribers_now = has_subscribers();

  if (helper_->has_subscribers.exchange(has_subscribers_now, std::memory_order_acq_rel) == has_subscribers_now) {
    return;
  }

  {
    std::lock_guard sync_lock(helper_->mtx);
  }

  helper_->connected_cv.notify_all();

  if (helper_->connected_callback) {
    auto callback_copy = helper_->connected_callback;
    lock.unlock();
    callback_copy(has_subscribers_now);
  }
}

PublisherImpl::PublisherImpl() : NodeImpl(kPublisher), helper_(std::make_unique<PublisherImplHelper>()) {}

}  // namespace vlink
