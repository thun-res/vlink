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
 * @file fast_buffer_manager.h
 * @brief Process-wide provider loader and shared-resource lifetime manager.
 *
 * @details
 * @c FastBufferManager owns the provider shared by every @c zerocopy::FastBuffer
 * in the process.  Construction of the first buffer resolves the provider through
 * this singleton; nodes and CLI tools use the same selection without vendor SDK
 * dependencies in VLink.  The manager owns local references and one cross-process control block
 * per exported allocation; buffers without sharing descriptors allocate no IPC state.
 *
 * | First-use environment          | Provider                     | Manager state |
 * | ------------------------------ | ---------------------------- | ------------- |
 * | Unset or empty                 | Built-in ordinary CPU memory | Valid         |
 * | @c shm                         | Built-in shared CPU memory   | Valid         |
 * | Compatible plugin name or path | Loaded plugin                | Valid         |
 * | Missing or incompatible plugin | None                         | Invalid       |
 *
 * @par Manager state machine
 * @code
 *                           first get()
 *                                |
 *                                v
 *                  read VLINK_FASTBUFFER_PLUGIN
 *                       /                 \
 *                 unset/empty/shm      name/path
 *                     |                    |
 *                     v                    v
 *              CPU implementation     Plugin::load()
 *                     |               /         \
 *                     |             valid      failed
 *                     v               v           v
 *                   shared provider             invalid
 *                     |
 *                     v
 *         FastBuffer --> core Resource
 *                           |
 *                 local / remote references
 *                           |
 *                       SDK Buffer
 *                     |
 *                     v
 *          release references before unloading
 * @endcode
 *
 * @par Sharing control block
 * Each exported allocation owns one named shared-memory segment; the wire descriptor names it and
 * carries the allocation's current generation.
 * @code
 *   Control block (shared memory)             Wire descriptor (40 bytes)
 *   +-------------------------------+         +-----------------+
 *   | protocol / size / desc size   |         | name[32]        |
 *   | owner identity                |         | generation      |
 *   | generation        (atomic)    |         +-----------------+
 *   | readers[32]       (atomic)    |
 *   +-------------------------------+
 *   | provider native descriptor    |
 *   +-------------------------------+
 * @endcode
 * A process identity is its PID combined with the kernel start time, so a reused PID never
 * impersonates a dead process.  On Linux the block also records the owner's PID namespace: a process
 * in another namespace cannot judge liveness, so its imports fail and its sweeps skip the allocation.
 * Each importing process claims one @c readers slot per allocation;
 * further imports in that process share the local resource and only raise its local reference count.
 * An import that attaches a new mapping fails when the reader table is full or the owner process is
 * dead; the importer then removes the dead owner's control segment and asks the provider to destroy the
 * native resource it names.  Re-imports served by a local resource only raise its reference count.  On
 * Linux and QNX, manager construction also sweeps control segments of the same protocol left behind by
 * dead owners; other protocols are left for a process of their own provider.
 *
 * @par Generation handshake
 * Reclaiming an allocation for reuse (@c zerocopy::FastBufferPool) or retiring it increments
 * @c generation before the owner inspects @c readers; an importer claims its slot before comparing
 * @c generation with the descriptor.  Both sides use sequentially consistent operations so that at
 * least one observes the other: the owner sees the claim and reports the allocation busy, or the
 * importer sees the newer generation and fails as expired.  In-process importers instead compare
 * the generation and raise the local reference count under the registry mutex, which the owner also
 * holds while checking that count.  An expired import drops the message exactly like a queue
 * overflow, so a publisher never needs an import acknowledgement.
 *
 * @par Lifetime without waiting
 * Final owner release retires the allocation: the generation is incremented, dead readers are purged
 * by identity, and the provider resource is freed immediately when no live reader remains.  Otherwise
 * the allocation joins a retired list that later create or release calls reclaim; no call ever
 * waits for another process.  Retired allocations still imported by live processes at manager shutdown
 * are logged and released anyway, which is what process exit would do moments later; importers keep the
 * mappings they already hold where the platform allows it.
 *
 * @par Locks
 * The registry mutex protects lookup, ownership changes and the retired list.  Native import and
 * close of one name, and the first export of one allocation, are serialized by lock stripes outside
 * the registry mutex.  Non-final local release is a lock-free reference decrement.
 *
 * Resolution happens once.  Changing the environment after the first @c get()
 * has no effect; an explicit load failure does not select another provider.
 * The manager owns the provider; returned pointers are borrowed.  Release buffers
 * before manager shutdown, as with the recording plugin manager.
 *
 * @par Example
 * @code
 * auto& manager = vlink::FastBufferManager::get();
 * if (manager.is_valid()) {
 *   auto* provider = manager.get_interface();
 *   VLOG_I("FastBuffer protocol: ", provider->get_protocol_id());
 * }
 * @endcode
 */

#pragma once

#include <memory>

#include "./fast_buffer_plugin_interface.h"

namespace vlink {

namespace zerocopy {

class FastBufferPool;

}  // namespace zerocopy

/**
 * @class FastBufferManager
 * @brief Loads one @c FastBufferPluginInterface on first use.
 *
 * @details
 * Reads @c VLINK_FASTBUFFER_PLUGIN once.  Empty or missing values select ordinary CPU memory and
 * @c shm selects shared CPU memory.  Explicit plugin load failures do not select CPU storage or retry.
 * Names and paths follow @c Plugin.
 */
class VLINK_EXPORT FastBufferManager final {
 public:
  /**
   * @brief Returns the lazily constructed process-wide manager.
   *
   * @return Singleton reference.
   */
  [[nodiscard]] static FastBufferManager& get();

  /**
   * @brief Reports whether the built-in or loaded provider is available.
   *
   * @return Whether @c get_interface() returns a non-null pointer.
   */
  [[nodiscard]] bool is_valid() const;

  /**
   * @brief Returns the provider owned by this manager.
   *
   * @return Borrowed provider pointer, or nullptr when unavailable.
   */
  [[nodiscard]] FastBufferPluginInterface* get_interface() const;

 private:
  friend struct zerocopy::FastBuffer;
  friend class zerocopy::FastBufferPool;

  [[nodiscard]] bool create(size_t size, const zerocopy::FastBuffer::Config& config,
                            zerocopy::FastBuffer::Buffer& buffer) noexcept;

  static void retain(const zerocopy::FastBuffer::Buffer& source, zerocopy::FastBuffer::Buffer& buffer) noexcept;

  void release(zerocopy::FastBuffer::Buffer& buffer) noexcept;

  [[nodiscard]] bool reclaim(const zerocopy::FastBuffer::Buffer& buffer) noexcept;

  [[nodiscard]] bool import_native(const void* handle, zerocopy::FastBuffer::Buffer& buffer) noexcept;

  [[nodiscard]] size_t get_handle_size(const zerocopy::FastBuffer::Buffer& buffer) const noexcept;

  [[nodiscard]] bool export_handle(const zerocopy::FastBuffer::Buffer& buffer, Bytes& descriptor) noexcept;

  [[nodiscard]] bool import_handle(const Bytes& descriptor, zerocopy::FastBuffer::Buffer& buffer) noexcept;

  [[nodiscard]] static const zerocopy::FastBuffer::Buffer& native_buffer(
      const zerocopy::FastBuffer::Buffer& buffer) noexcept;

  FastBufferManager();

  ~FastBufferManager();

  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(FastBufferManager)
};

}  // namespace vlink
