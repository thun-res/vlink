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

#include <mutex>
#include <set>

#include "./base/memory_resource.h"

namespace vlink {

// AckManager
AckManager::AckManager() noexcept = default;

AckManager::~AckManager() noexcept = default;

AckManager::RequestPtr AckManager::create_request() noexcept {
  std::unique_lock manager_lock(mtx_);

  auto request = MemoryResource::make_shared<Request>();

  request->seq = request_seq_++;
  request->generation = generation_;

  return request;
}

bool AckManager::process(RequestPtr request, int ms, ProcessCallback&& process_callback) noexcept {
  {
    std::lock_guard manager_lock(mtx_);

    if VUNLIKELY (is_interrupted_ || request->generation != generation_) {
      return false;
    }

    request_set_.emplace(request);
  }

  if VLIKELY (process_callback) {
    if VUNLIKELY (!process_callback()) {
      remove(request);
      return false;
    }
  } else {
    remove(request);
    return false;
  }

  bool ret = true;

  {
    std::unique_lock lock(request->mtx);

    auto predicate = [this, &request]() -> bool {
      std::lock_guard manager_lock(mtx_);
      return is_interrupted_ || request->generation != generation_ || request_set_.count(request) == 0;
    };

    if VUNLIKELY (ms < 0) {
      request->cv.wait(lock, predicate);
    } else {
      ret = request->cv.wait_for(lock, std::chrono::milliseconds(ms), predicate);
    }
  }

  {
    std::lock_guard manager_lock(mtx_);
    request_set_.erase(request);

    if VUNLIKELY (is_interrupted_ || request->generation != generation_) {
      return false;
    }
  }

  return ret;
}

bool AckManager::notify(RequestPtr request, NotifyCallback&& notify_callback) noexcept {
  {
    std::lock_guard manager_lock(mtx_);

    if VUNLIKELY (request_set_.erase(request) == 0) {
      return false;
    }
  }

  std::lock_guard lock(request->mtx);

  if VLIKELY (notify_callback) {
    notify_callback();
  }

  request->cv.notify_one();

  return true;
}

bool AckManager::remove(RequestPtr request) noexcept {
  std::lock_guard manager_lock(mtx_);

  return request_set_.erase(request) != 0;
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
    request->cv.notify_all();
  }
}

void AckManager::reset_interrupted() noexcept {
  std::lock_guard manager_lock(mtx_);

  is_interrupted_ = false;
}

}  // namespace vlink
