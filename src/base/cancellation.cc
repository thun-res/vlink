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

#include "./base/cancellation.h"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "./base/logger.h"
#include "./base/memory_resource.h"

namespace vlink {

[[maybe_unused]] static void invoke_cancellation_callback(MoveFunction<void()>& callback) noexcept {
  if VUNLIKELY (!callback) {
    return;
  }

  try {
    callback();
  } catch (const std::exception& e) {
    CLOG_E("Cancellation callback threw an exception: %s.", e.what());
  } catch (...) {
    CLOG_E("Cancellation callback threw a non-std exception.");
  }
}

// CancellationRegistration::State
struct CancellationRegistration::State final {
  mutable std::mutex mtx;
  bool cancelled{false};
  size_t next_id{1};
  std::unordered_map<size_t, MoveFunction<void()>> callbacks;
};

// CancellationRegistration
CancellationRegistration::CancellationRegistration() noexcept = default;

CancellationRegistration::~CancellationRegistration() { reset(); }

CancellationRegistration::CancellationRegistration(CancellationRegistration&& other) noexcept
    : state_(std::move(other.state_)), id_(other.id_) {
  other.id_ = 0;
}

CancellationRegistration& CancellationRegistration::operator=(CancellationRegistration&& other) noexcept {
  if VUNLIKELY (this == &other) {
    return *this;
  }

  reset();

  state_ = std::move(other.state_);
  id_ = other.id_;
  other.id_ = 0;

  return *this;
}

void CancellationRegistration::reset() noexcept {
  if VUNLIKELY (id_ == 0) {
    return;
  }

  auto state = state_.lock();

  if VLIKELY (state) {
    std::lock_guard lock(state->mtx);

    state->callbacks.erase(id_);
  }

  id_ = 0;
  state_.reset();
}

bool CancellationRegistration::valid() const noexcept {
  if VUNLIKELY (id_ == 0) {
    return false;
  }

  auto state = state_.lock();

  if VLIKELY (state) {
    std::lock_guard lock(state->mtx);

    return state->callbacks.find(id_) != state->callbacks.end();
  }

  return false;
}

CancellationRegistration::CancellationRegistration(std::weak_ptr<State> state, size_t id) noexcept
    : state_(std::move(state)), id_(id) {}

// CancellationToken
CancellationToken::CancellationToken() noexcept = default;

bool CancellationToken::valid() const noexcept { return state_ != nullptr; }

bool CancellationToken::is_cancellation_requested() const noexcept {
  if VUNLIKELY (!state_) {
    return false;
  }

  std::lock_guard lock(state_->mtx);

  return state_->cancelled;
}

CancellationRegistration CancellationToken::register_callback(MoveFunction<void()>&& callback) const {
  if VUNLIKELY (!state_ || !callback) {
    return {};
  }

  {
    std::lock_guard lock(state_->mtx);

    if VLIKELY (!state_->cancelled) {
      const size_t id = state_->next_id++;

      state_->callbacks.emplace(id, std::move(callback));

      return CancellationRegistration{state_, id};
    }
  }

  invoke_cancellation_callback(callback);

  return {};
}

void CancellationToken::throw_if_cancellation_requested() const {
  if VUNLIKELY (is_cancellation_requested()) {
    throw Exception::OperationCancelled{};
  }
}

CancellationToken::CancellationToken(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

// CancellationSource
CancellationSource::CancellationSource() : state_(MemoryResource::make_shared<State>()) {}

CancellationToken CancellationSource::token() const noexcept { return CancellationToken{state_}; }

bool CancellationSource::is_cancellation_requested() const noexcept { return token().is_cancellation_requested(); }

bool CancellationSource::request_cancel() const {
#ifdef VLINK_ENABLE_BASE_MEMORY_RESOURCE
  std::pmr::vector<MoveFunction<void()>> callbacks(&MemoryResource::global_instance());
#else
  std::vector<MoveFunction<void()>> callbacks;
#endif

  {
    std::lock_guard lock(state_->mtx);

    if VUNLIKELY (state_->cancelled) {
      return false;
    }

    state_->cancelled = true;

    callbacks.reserve(state_->callbacks.size());

    for (auto& [id, callback] : state_->callbacks) {
      (void)id;

      callbacks.emplace_back(std::move(callback));
    }

    state_->callbacks.clear();
  }

  for (auto& callback : callbacks) {
    invoke_cancellation_callback(callback);
  }

  return true;
}

}  // namespace vlink
