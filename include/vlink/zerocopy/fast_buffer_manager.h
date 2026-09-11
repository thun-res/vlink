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
 * dependencies in VLink.  The manager owns local references and, in the default system mode,
 * cross-process control blocks.  Custom-mode providers supply their own cross-process lifecycle.
 * Buffers without sharing descriptors do not allocate IPC state.
 * The registry lock only protects lookup and ownership changes.  Native IPC import/close use
 * resource-ID lock stripes; collisions serialize unrelated resources.  Non-final local release
 * uses atomic reference counting, and first export is serialized per resource.
 *
 * | First-use environment          | Provider                     | Manager state |
 * | ------------------------------ | ---------------------------- | ------------- |
 * | Unset or empty                 | Built-in ordinary CPU memory | Valid         |
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
 *                 unset/empty          name/path
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

/**
 * @class FastBufferManager
 * @brief Loads one @c FastBufferPluginInterface on first use.
 *
 * @details
 * Reads @c VLINK_FASTBUFFER_PLUGIN once.  Empty or missing values select ordinary CPU memory.
 * Explicit plugin load failures do not select CPU storage or retry.  Names and paths follow @c Plugin.
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

  [[nodiscard]] bool create(size_t size, const zerocopy::FastBuffer::Config& config,
                            zerocopy::FastBuffer::Buffer& buffer) noexcept;

  void retain(const zerocopy::FastBuffer::Buffer& source, zerocopy::FastBuffer::Buffer& buffer) noexcept;

  void release(zerocopy::FastBuffer::Buffer& buffer) noexcept;

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
