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

#include "./impl/ack_manager.h"

#include <memory>
#include <mutex>

namespace vlink {

// AckManager
AckManager::AckManager() noexcept = default;

AckManager::~AckManager() noexcept = default;

AckManager::RequestPtr AckManager::create_request() noexcept {
  std::unique_lock manager_lock(mtx_);

  auto request = std::make_shared<Request>();

  request->seq = request_seq_++;
  request->generation = generation_;

  return request;
}

bool AckManager::process(RequestPtr request, int ms, ProcessCallback&& process_callback) noexcept {
  auto deadline = std::chrono::steady_clock::time_point::max();

  if VLIKELY (ms >= 0) {
    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  }

  {
    std::lock_guard lock(request->mtx);
    std::lock_guard manager_lock(mtx_);

    if VUNLIKELY (is_interrupted_ || request->generation != generation_) {
      return false;
    }

    if VUNLIKELY (request->status != Request::Status::kCreated) {
      return false;
    }

    request->deadline = deadline;
    request->status = Request::Status::kPending;
    request_set_.emplace(request);
  }

  const bool sent = process_callback && process_callback();

  std::unique_lock lock(request->mtx);

  auto predicate = [&request]() -> bool { return request->status != Request::Status::kPending; };

  if VLIKELY (sent) {
    if VUNLIKELY (ms < 0) {
      request->cv.wait(lock, predicate);
    } else {
      request->cv.wait_until(lock, request->deadline, predicate);
    }
  }

  {
    std::lock_guard manager_lock(mtx_);
    request_set_.erase(request);
  }

  if (request->status == Request::Status::kPending) {
    request->status = Request::Status::kCancelled;
  }

  return sent && request->status == Request::Status::kAcknowledged;
}

bool AckManager::notify(RequestPtr request, NotifyCallback&& notify_callback) noexcept {
  if VUNLIKELY (!request) {
    return false;
  }

  std::lock_guard lock(request->mtx);

  if VUNLIKELY (request->status != Request::Status::kPending) {
    return false;
  }

  const auto acknowledged_at = std::chrono::steady_clock::now();

  if VUNLIKELY (acknowledged_at >= request->deadline) {
    request->status = Request::Status::kCancelled;
    request->cv.notify_one();
    return false;
  }

  if VLIKELY (notify_callback) {
    notify_callback();
  }

  request->status = Request::Status::kAcknowledged;
  request->cv.notify_one();

  return true;
}

bool AckManager::remove(RequestPtr request) noexcept {
  if VUNLIKELY (!request) {
    return false;
  }

  std::lock_guard lock(request->mtx);

  if VUNLIKELY (request->status != Request::Status::kPending) {
    return false;
  }

  {
    std::lock_guard manager_lock(mtx_);

    if (request_set_.erase(request) == 0) {
      return false;
    }
  }

  request->status = Request::Status::kCancelled;
  request->cv.notify_one();

  return true;
}

void AckManager::clear() noexcept {
  decltype(request_set_) temp_set;

  {
    std::lock_guard manager_lock(mtx_);

    is_interrupted_ = true;
    ++generation_;

    temp_set.swap(request_set_);
  }

  for (const auto& request : temp_set) {
    std::lock_guard lock(request->mtx);
    if (request->status == Request::Status::kPending) {
      request->status = Request::Status::kCancelled;
      request->cv.notify_all();
    }
  }
}

void AckManager::reset_interrupted() noexcept {
  std::lock_guard manager_lock(mtx_);

  is_interrupted_ = false;
}

}  // namespace vlink
