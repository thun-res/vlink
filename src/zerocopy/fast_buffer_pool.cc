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

#include "./zerocopy/fast_buffer_pool.h"

#include <utility>
#include <vector>

#include "./zerocopy/fast_buffer_manager.h"

namespace vlink {

namespace zerocopy {

bool FastBufferPool::create(size_t size, size_t depth, const FastBuffer::Config& config) noexcept {
  std::vector<FastBuffer> slots;

  if VUNLIKELY (size == 0 || depth == 0 || depth > slots.max_size()) {
    return false;
  }

  slots.reserve(depth);

  for (size_t index = 0; index < depth; ++index) {
    if VUNLIKELY (!slots.emplace_back().create(size, config)) {
      return false;
    }
  }

  slots_ = std::move(slots);
  next_ = 0;
  return true;
}

bool FastBufferPool::acquire(FastBuffer& buffer) noexcept {
  buffer.clear();

  const size_t depth = slots_.size();
  auto& manager = FastBufferManager::get();

  for (size_t offset = 0; offset < depth; ++offset) {
    const size_t index = (next_ + offset) % depth;
    const FastBuffer& slot = slots_[index];

    if (!manager.reclaim(slot.buffer_)) {
      continue;
    }

    next_ = (index + 1) % depth;
    return buffer.shallow_copy(slot);
  }

  return false;
}

void FastBufferPool::clear() noexcept {
  slots_.clear();
  next_ = 0;
}

}  // namespace zerocopy

}  // namespace vlink
