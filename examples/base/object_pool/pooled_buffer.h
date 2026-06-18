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

#pragma once

#include <vlink/base/logger.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// -----------------------------------------------------------------------------
// pooled_buffer: tiny user-defined object used by the ObjectPool example. The
// pool reuses Buffer instances across acquisitions; reset() is the user
// callback that the pool invokes on acquire/release per the configured
// policy, returning the buffer to a "fresh" state without freeing storage.
// -----------------------------------------------------------------------------
namespace pooled_buffer {

struct Buffer {
  std::vector<uint8_t> data;
  size_t used{0};

  // Ctor allocates the backing storage once per Buffer; the pool keeps the
  // Buffer alive across many borrow/return cycles so this allocation is
  // amortised. The MLOG_D fires only when the pool actually creates a new
  // slot (not on subsequent reuse).
  explicit Buffer(size_t capacity = 1024) : data(capacity, 0) { MLOG_D("  Buffer created (capacity={})", capacity); }

  // Reset is what the pool invokes per its policy. We DO NOT touch data
  // (the storage stays warm); only the logical "used" length is rolled
  // back to zero.
  void reset() {
    used = 0;
    VLOG_D("  Buffer reset");
  }
};

}  // namespace pooled_buffer
