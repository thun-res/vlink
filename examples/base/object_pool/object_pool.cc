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

#include <vlink/base/logger.h>
#include <vlink/base/object_pool.h>

#include <stdexcept>
#include <vector>

#include "pooled_buffer.h"

using pooled_buffer::Buffer;

// -----------------------------------------------------------------------------
// ObjectPool example
//
// Module:   vlink/base/object_pool.h
// Scenario: ObjectPool<T> recycles heap-constructed T instances behind a
//           handful of APIs:
//             get()        -> unique_ptr<T, PoolDeleter> (RAII return).
//             get_shared() -> shared_ptr<T> (shared ownership, return on
//                             last ref).
//             borrow/give_back -> raw pointer round-trip (manual lifetime).
//           Reset policies decide when the user-supplied reset callback
//           fires: kPolicyRelease (default, on return), kPolicyAcquire
//           (on hand-out), kPolicyBoth, kPolicyNone. The pool grows up to
//           the configured max size; further get() throws when exhausted.
// -----------------------------------------------------------------------------
int main() {
  // get() returns an RAII handle; once it goes out of scope the buffer is
  // reset (per kPolicyRelease) and returned to the pool. The pool size
  // dropped from 2 to 1 during use and bounced back to 2 after release.
  {
    VLOG_I("=== RAII get() ===");
    auto pool = std::make_shared<vlink::ObjectPool<Buffer>>([]() { return std::make_unique<Buffer>(4096); },  // factory
                                                            2, 8, [](Buffer& b) { b.reset(); },
                                                            vlink::ObjectPool<Buffer>::kPolicyRelease);
    MLOG_I("  initial: pool={} borrowed={} total={}", pool->size(), pool->borrowed(), pool->total_created());

    {
      auto buf = pool->get();
      buf->used = 100;
      MLOG_I("  during use: pool={} borrowed={}", pool->size(), pool->borrowed());
    }

    MLOG_I("  after return: pool={} borrowed={}", pool->size(), pool->borrowed());
  }

  // get_shared(): shared_ptr ownership lets multiple holders extend the
  // buffer's lifetime. The buffer goes back to the pool when the last
  // shared_ptr drops, not when the first one does.
  {
    VLOG_I("=== get_shared() ===");
    auto pool = std::make_shared<vlink::ObjectPool<Buffer>>([]() { return std::make_unique<Buffer>(2048); }, 1, 4);

    {
      auto shared_buf = pool->get_shared();
      shared_buf->used = 50;

      // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
      auto copy = shared_buf;
      MLOG_I("  use_count={}", copy.use_count());
    }

    MLOG_I("  after release: pool={}", pool->size());
  }

  // borrow / give_back: raw-pointer interface for cases where the consumer
  // cannot use unique_ptr/shared_ptr (FFI boundary, intrusive structs). The
  // caller is on the hook to call give_back exactly once -- forgetting it
  // leaks the slot until the pool itself is destroyed.
  {
    VLOG_I("=== borrow / give_back ===");
    auto pool = std::make_shared<vlink::ObjectPool<Buffer>>([]() { return std::make_unique<Buffer>(512); }, 0, 4);

    Buffer* raw = pool->borrow();
    raw->used = 256;
    MLOG_I("  during borrow: borrowed={}", pool->borrowed());

    pool->give_back(raw);
    MLOG_I("  after give_back: pool={} borrowed={}", pool->size(), pool->borrowed());
  }

  // Reset policies decide when the user-provided reset callback fires.
  // kPolicyAcquire wipes on hand-out (consumers always see a clean state).
  // kPolicyBoth wipes on both ends (paranoid; useful for objects holding
  // sensitive data). kPolicyNone disables the callback entirely.
  {
    VLOG_I("=== Reset policies ===");

    auto pool_acq = std::make_shared<vlink::ObjectPool<Buffer>>([]() { return std::make_unique<Buffer>(128); }, 1, 4,
                                                                [](Buffer& b) { b.reset(); },
                                                                vlink::ObjectPool<Buffer>::kPolicyAcquire);
    auto a = pool_acq->get();
    MLOG_I("  kPolicyAcquire: buf.used={}", a->used);

    auto pool_both = std::make_shared<vlink::ObjectPool<Buffer>>([]() { return std::make_unique<Buffer>(128); }, 1, 4,
                                                                 [](Buffer& b) { b.reset(); },
                                                                 vlink::ObjectPool<Buffer>::kPolicyBoth);
    {
      auto b = pool_both->get();
      b->used = 42;
    }

    MLOG_I("  kPolicyBoth: object reset on acquire and release");

    auto pool_none = std::make_shared<vlink::ObjectPool<Buffer>>([]() { return std::make_unique<Buffer>(128); }, 1, 4,
                                                                 nullptr, vlink::ObjectPool<Buffer>::kPolicyNone);
    MLOG_I("  kPolicyNone: no reset callback");
  }

  // stats() is a cheap snapshot of the bookkeeping fields. Hold the
  // handles in a vector to keep the borrowed counter at 5 across the dump.
  {
    VLOG_I("=== Statistics ===");
    auto pool = std::make_shared<vlink::ObjectPool<Buffer>>([]() { return std::make_unique<Buffer>(256); }, 3, 10);

    auto stats = pool->stats();
    MLOG_I("  pool={} borrowed={} total={} max={}", stats.pool_size, stats.borrowed, stats.total_created,
           stats.max_size);

    std::vector<std::unique_ptr<Buffer, vlink::ObjectPool<Buffer>::PoolDeleter>> handles;
    handles.reserve(5);
    for (int i = 0; i < 5; ++i) {
      handles.emplace_back(pool->get());
    }

    stats = pool->stats();
    MLOG_I("  after 5 borrows: pool={} borrowed={} total={}", stats.pool_size, stats.borrowed, stats.total_created);

    handles.clear();
    stats = pool->stats();
    MLOG_I("  after return all: pool={} borrowed={}", stats.pool_size, stats.borrowed);
  }

  // Exhaustion: with max=2 and 2 outstanding borrows, the third get()
  // throws std::runtime_error. Callers should choose either:
  //   - catch the throw and fall back, or
  //   - configure a larger max from the start.
  {
    VLOG_I("=== Pool exhaustion ===");
    auto pool = std::make_shared<vlink::ObjectPool<Buffer>>([]() { return std::make_unique<Buffer>(64); }, 0, 2);

    auto a = pool->get();
    auto b = pool->get();
    MLOG_I("  borrowed 2/2");

    try {
      auto c = pool->get();
    } catch (const std::runtime_error& e) {
      MLOG_I("  exhausted: {}", e.what());
    }
  }

  VLOG_I("ObjectPool example finished.");
  return 0;
}
