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

/**
 * @file fast_buffer_pool.h
 * @brief Fixed-depth publisher pool that recycles @c FastBuffer allocations by generation.
 *
 * @details
 * A publisher of shared descriptors must neither overwrite an allocation that importers still read nor
 * wait for them.  @c FastBufferPool owns @c depth allocations of one size and hands out the next one
 * whose readers have all released it.  The manager increments that allocation's generation first, so
 * descriptors published earlier for it fail to import as expired instead of observing overwritten
 * bytes.  Dead reader processes are purged by identity, so a crashed subscriber never pins a slot.
 *
 * | Call                         | Effect                                                        |
 * | ---------------------------- | ------------------------------------------------------------- |
 * | @c create(size, depth)       | Allocates @c depth uninitialised buffers through the provider |
 * | @c acquire(buffer)           | Clears @p buffer, then retains the next reusable slot         |
 * | @c clear()                   | Releases the slots; retained buffers keep theirs alive        |
 *
 * @par Publish cycle
 * @code
 * vlink::zerocopy::FastBufferPool pool;
 * vlink::zerocopy::FastBuffer frame;
 * if (pool.create(frame_size, 4)) {
 *   while (running) {
 *     if (pool.acquire(frame)) {
 *       fill(frame);          // map()/unmap() or device work
 *       pub.publish(frame);   // shared descriptor when the provider supports it
 *     }                       // otherwise every slot is still being read: skip this frame
 *   }                         // dropping frame returns its slot without waiting
 * }
 * @endcode
 * Choose @c depth larger than the deepest subscriber queue plus the frames a subscriber retains; slower
 * subscribers then drop expired frames rather than stalling the publisher.  One thread drives a pool.
 */

#pragma once

#include <cstddef>
#include <vector>

#include "./fast_buffer.h"

namespace vlink {

namespace zerocopy {

/**
 * @class FastBufferPool
 * @brief Owns a fixed set of equally sized allocations and recycles them in round-robin order.
 */
class VLINK_EXPORT FastBufferPool final {
 public:
  FastBufferPool() noexcept = default;

  ~FastBufferPool() noexcept = default;

  /**
   * @brief Allocates @p depth buffers of @p size bytes; failure leaves the pool unchanged.
   *
   * @param size Nonzero byte count of every slot.
   * @param depth Nonzero slot count.
   * @param config Memory type and provider-local device shared by every slot.
   *
   * @return Whether every slot was allocated.
   */
  [[nodiscard]] bool create(size_t size, size_t depth, const FastBuffer::Config& config = {}) noexcept;

  /**
   * @brief Clears @p buffer and retains the next slot that no local holder or live importer references.
   *
   * @param buffer Output reference; empty when no slot is reusable.
   *
   * @return Whether a slot was acquired.
   */
  [[nodiscard]] bool acquire(FastBuffer& buffer) noexcept;

  /**
   * @brief Returns the slot count.
   *
   * @return Zero before @c create().
   */
  [[nodiscard]] size_t depth() const noexcept { return slots_.size(); }

  /**
   * @brief Returns the byte count of every slot.
   *
   * @return Zero before @c create().
   */
  [[nodiscard]] size_t size() const noexcept { return slots_.empty() ? 0 : slots_.front().size(); }

  /**
   * @brief Releases the pool's slot references.
   */
  void clear() noexcept;

 private:
  std::vector<FastBuffer> slots_;
  size_t next_{0};

  VLINK_DISALLOW_COPY_AND_ASSIGN(FastBufferPool)
};

}  // namespace zerocopy

}  // namespace vlink
