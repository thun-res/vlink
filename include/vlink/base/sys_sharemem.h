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
 * @file sys_sharemem.h
 * @brief Named cross-process shared-memory region.
 *
 * @details
 * @c vlink::SysSharemem wraps the platform's shared-memory API: @c shm_open + @c mmap on
 * POSIX targets, @c CreateFileMapping + @c MapViewOfFile on Windows.  The region appears
 * in the kernel namespace under a caller-chosen name and any process that knows the name
 * can map it into its own address space.
 *
 * Memory layout exposed through @c data() and @c size():
 *
 * @verbatim
 *   +---------------------------------------------------+
 *   | byte 0                                            |
 *   |   user payload (size = total bytes returned by    |
 *   |   size())                                         |
 *   | byte size-1                                       |
 *   +---------------------------------------------------+
 * @endverbatim
 *
 * No internal header is reserved; the entire mapping is user payload.  Place any
 * synchronisation primitive (mutex, atomic counters, ring buffer indices) in the user
 * region or in a companion @c SysSemaphore.
 *
 * Lifecycle states observed by the wrapper:
 *
 * | State        | Entered by              | @c is_attached() | @c data()             |
 * | ------------ | ----------------------- | ---------------- | --------------------- |
 * | Detached     | Construction, @c detach | @c false         | @c nullptr            |
 * | Created      | @c create()             | @c true          | Pointer to mapping    |
 * | Attached     | @c attach()             | @c true          | Pointer to mapping    |
 *
 * Access modes accepted by @c create() and @c attach():
 *
 * | Mode             | POSIX flag          | Effect                              |
 * | ---------------- | ------------------- | ----------------------------------- |
 * | @c kReadOnly     | @c O_RDONLY         | Maps with @c PROT_READ              |
 * | @c kReadWrite    | @c O_RDWR           | Maps with @c PROT_READ + PROT_WRITE |
 *
 * @note
 * - POSIX names must begin with @c '/'; Windows accepts arbitrary non-empty strings.
 * - The kernel zero-fills newly created regions on the supported POSIX/Windows backends.
 * - Cross-process consistency requires external synchronisation.
 *
 * @par Example
 * @code
 * // Process A (creator)
 * vlink::SysSharemem shm;
 * shm.create("/vlink_frame", 1024 * 1024);
 * auto* frame = static_cast<FrameHeader*>(shm.data());
 * frame->seq = 0;
 *
 * // Process B (consumer)
 * vlink::SysSharemem shm;
 * shm.attach("/vlink_frame");
 * const auto* frame = static_cast<const FrameHeader*>(shm.data());
 * process_frame(*frame);
 *
 * shm.detach();
 * @endcode
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "./macros.h"

namespace vlink {

/**
 * @class SysSharemem
 * @brief Named cross-process shared-memory region with read-only or read-write mappings.
 *
 * @details
 * Manages one mapping at a time; call @c detach() before re-attaching to a different region.
 */
class VLINK_EXPORT SysSharemem final {
 public:
  /**
   * @enum Mode
   * @brief Access mode applied to the mapping.
   *
   * @details
   * Enumerator names mirror the POSIX @c O_RDONLY / @c O_RDWR open flags.  On Windows the
   * implementation translates these to the equivalent @c CreateFileMapping / @c MapViewOfFile
   * page-protection constants; the observable behaviour is identical across platforms.
   */
  enum Mode : uint8_t {
    kReadOnly = 0,  ///< Read-only mapping; writes through @c data() are undefined.
    kReadWrite = 1  ///< Read-write mapping (default).
  };

  /**
   * @brief Constructs a wrapper in the detached state.
   */
  SysSharemem();

  /**
   * @brief Destructor.  Invokes @c detach(false) when the wrapper is still attached.
   */
  ~SysSharemem();

  /**
   * @brief Creates and maps a new shared-memory region of @p size bytes under @p name.
   *
   * @details
   * Fails when a region with @p name already exists in the kernel namespace.  The supported
   * POSIX/Windows backends provide zero-initialised storage.
   *
   * @param name  Kernel-name for the region.  POSIX must begin with @c '/'.
   * @param size  Region size in bytes.
   * @param mode  Access mode for the mapping.  Default: @c kReadWrite.
   * @return @c true on success.
   */
  bool create(const std::string& name, size_t size, Mode mode = kReadWrite);

  /**
   * @brief Attaches to an existing shared-memory region previously created with the same name.
   *
   * @param name  Kernel-name of the region.
   * @param mode  Access mode for the mapping.  Default: @c kReadWrite.
   * @return @c true on success.
   */
  bool attach(const std::string& name, Mode mode = kReadWrite);

  /**
   * @brief Unmaps the region and optionally unlinks the kernel name.
   *
   * @param force  When @c true, POSIX backends call @c shm_unlink so the region is removed
   *               from the kernel namespace.  Other processes that still hold mappings keep
   *               them.  Windows merely closes the local mapping handle.  Default: @c true.
   * @return @c true on success.
   */
  bool detach(bool force = true);

  /**
   * @brief Reports whether the wrapper currently owns a mapping.
   *
   * @return @c true after a successful @c create or @c attach.
   */
  [[nodiscard]] bool is_attached() const;

  /**
   * @brief Returns a writable pointer to the start of the mapping.
   *
   * @return Mapping base pointer; @c nullptr when detached or in read-only mode.
   */
  [[nodiscard]] void* data();

  /**
   * @brief Returns a read-only pointer to the start of the mapping.
   *
   * @return Const mapping base pointer; @c nullptr when detached.
   */
  [[nodiscard]] const void* data() const;

  /**
   * @brief Returns the size of the mapping in bytes.
   *
   * @return Region size; @c 0 when detached.
   */
  [[nodiscard]] size_t size() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(SysSharemem)
};

}  // namespace vlink
