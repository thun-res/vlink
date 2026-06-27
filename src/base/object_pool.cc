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

#include "./base/object_pool.h"

#include <mutex>
#include <sstream>
#include <stdexcept>

namespace vlink {

// ObjectPoolBase
ObjectPoolBase::ObjectPoolBase(size_t max_size, size_t initial_size, Policy policy)
    : policy_(policy), max_size_(max_size) {
  if VUNLIKELY (max_size_ > 0 && initial_size > max_size_) {
    throw_invalid_size();
  }
}

size_t ObjectPoolBase::borrowed() const {
  std::lock_guard lock(mutex_);

  return borrowed_;
}

size_t ObjectPoolBase::total_created() const {
  std::lock_guard lock(mutex_);

  return total_created_;
}

size_t ObjectPoolBase::max_size() const noexcept { return max_size_; }

void ObjectPoolBase::safe_dec_borrowed_and_live() noexcept {
  std::lock_guard lock(mutex_);

  if (borrowed_ > 0) {
    --borrowed_;
  }

  if (live_count_ > 0) {
    --live_count_;
  }
}

bool ObjectPoolBase::should_reset_on_acquire() const noexcept {
  return policy_ == kPolicyAcquire || policy_ == kPolicyBoth;
}

bool ObjectPoolBase::should_reset_on_release() const noexcept {
  return policy_ == kPolicyRelease || policy_ == kPolicyBoth;
}

void ObjectPoolBase::throw_invalid_size() { throw std::invalid_argument("initial_size exceeds max_size"); }

void ObjectPoolBase::throw_factory_null_pre_fill() {
  throw std::runtime_error("FactoryCallback returned nullptr during pre-fill");
}

void ObjectPoolBase::throw_factory_null() { throw std::runtime_error("FactoryCallback returned nullptr"); }

void ObjectPoolBase::throw_exhausted(size_t pool_size) const {
  std::ostringstream oss;

  oss << "ObjectPool exhausted: max_size=" << max_size_ << " live_count=" << live_count_
      << " total_created=" << total_created_ << " borrowed=" << borrowed_ << " pool_size=" << pool_size;

  throw std::runtime_error(oss.str());
}

}  // namespace vlink
