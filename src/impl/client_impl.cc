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

#include "./impl/client_impl.h"

#include <mutex>
#include <utility>

#include "./base/condition_variable.h"
#include "./base/utils.h"

namespace vlink {

// ClientImplHelper
struct ClientImplHelper final {
  bool connected{false};
  NodeImpl::ConnectCallback connected_callback;
  ConditionVariable connected_cv;
  std::mutex mtx;
  std::recursive_mutex callback_mtx;
};

// ClientImpl
ClientImpl::~ClientImpl() = default;

void ClientImpl::interrupt() {
  NodeImpl::interrupt();

  {
    std::lock_guard sync_lock(helper_->mtx);
  }

  helper_->connected_cv.notify_all();
}

void ClientImpl::detect_connected(ConnectCallback&& callback) {
  std::unique_lock lock(helper_->callback_mtx);
  helper_->connected_callback = std::move(callback);

  if (helper_->connected) {
    auto callback_copy = helper_->connected_callback;
    lock.unlock();
    callback_copy(true);
  }
}

bool ClientImpl::wait_for_connected(std::chrono::milliseconds timeout) {
  if VLIKELY (is_connected()) {
    return true;
  }

  Utils::yield_cpu();

  std::unique_lock lock(helper_->mtx);

  reset_interrupted();

  auto predicate = [this]() -> bool { return is_connected() || is_interrupted(); };

  if VUNLIKELY (timeout.count() < 0) {
    helper_->connected_cv.wait(lock, std::move(predicate));
    return is_connected();
  } else {
    return helper_->connected_cv.wait_for(lock, timeout, std::move(predicate)) && !is_interrupted();
  }
}

void ClientImpl::update_connected() {
  Utils::yield_cpu();

  std::unique_lock lock(helper_->callback_mtx);

  if (helper_->connected == is_connected()) {
    return;
  }

  helper_->connected = !helper_->connected;

  {
    std::lock_guard sync_lock(helper_->mtx);
  }

  helper_->connected_cv.notify_all();

  if (helper_->connected_callback) {
    bool connected = helper_->connected;
    auto callback_copy = helper_->connected_callback;
    lock.unlock();
    callback_copy(connected);
  }
}

ClientImpl::ClientImpl() : NodeImpl(kClient), helper_(std::make_unique<ClientImplHelper>()) {}

}  // namespace vlink
