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
 * @file fast_buffer_plugin_interface.h
 * @brief Vendor-independent allocation and sharing contract for @c zerocopy::FastBuffer.
 *
 * @details
 * @c FastBufferPluginInterface separates native allocation and host access from
 * the platform that allocates the storage.  Implementations may use ordinary or
 * pinned CPU memory, device memory, shared memory, DMA-BUF or virtual allocations.
 * Unsupported allocation domains and optional operations report failure.
 *
 * | Operation                | Methods                                           | Contract                       |
 * | ------------------------ | ------------------------------------------------- | ------------------------------ |
 * | Allocation and ownership | @c create(), @c release()                         | One owned reference per result |
 * | CPU mapping              | @c map(), @c unmap()                              | Explicit access/cache scope    |
 * | Native API integration   | @c native_handle(), @c import_native()            | Provider-defined local object  |
 * | Completion               | @c synchronize()                                  | Wait for tracked work          |
 * | Payload copy             | @c copy(), @c copy_to_host(), @c copy_from_host() | Synchronous completion         |
 * | Descriptor transport     | @c export_handle(), @c import_handle()            | No local pointer on the wire   |
 * | Descriptor size          | @c get_handle_size()                              | Per-allocation support         |
 * | Dead-owner cleanup       | @c destroy_handle()                               | Named OS resources only        |
 *
 * @par Sharing lifecycle
 * @code
 *   Publishing process                        Subscribing process
 *   ------------------                        -------------------
 *   pool acquire --> generation + 1
 *        |
 *   device/CPU writes
 *        |
 *   manager export -- name + generation --> manager import
 *        |                                  claim reader slot, check generation
 *        |                                  SDK import_handle()
 *   drop reference (never waits)                     |
 *        |                                  device use or map/unmap
 *   next acquire: purge dead readers,                |
 *   busy while live readers remain          release(): SDK close, clear slot
 *        |
 *   final release: retire, free when the reader table is empty
 * @endcode
 *
 * A @c zerocopy::FastBuffer::Buffer address belongs to its local process and address space.
 * CPU callers must use @c map() or @c copy_to_host(); a non-null device address
 * is not proof of CPU accessibility.  Native objects and numeric file descriptors
 * likewise require provider-specific import or OS handle transfer.
 *
 * @par Plugin registration
 * @code
 * class MyFastBufferPlugin final : public vlink::FastBufferPluginInterface {
 *   // Implement the allocation, access and sharing contract below.
 * };
 * VLINK_PLUGIN_DECLARE(MyFastBufferPlugin, 1, 0)
 * @endcode
 * Set @c VLINK_FASTBUFFER_PLUGIN before the process creates its first buffer.
 * The interface ABI and the descriptor protocol ID are separate compatibility
 * contracts; the latter identifies interoperable native descriptors within the core envelope.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "../base/plugin.h"
#include "./fast_buffer.h"

namespace vlink {

/**
 * @class FastBufferPluginInterface
 * @brief Supplies platform buffers without exposing a vendor runtime to VLink.
 *
 * @details
 * All methods may run on different host threads; implementations select the resource's device/context.
 * zerocopy::FastBuffer::Buffer addresses are process-local and must never be dereferenced by VLink on the CPU.  Handles
 * are plugin-owned allocation or import references, not wire data.  Failed create/import calls leave their
 * output empty and release partial resources.  Successful calls return one reference, released exactly once by the
 * host.  Size and descriptor length remain constant for that reference.
 *
 * Export is idempotent and creates no unclaimed reference.  Publishing is not an import acknowledgement:
 * a descriptor stays importable until the owner reuses or retires the allocation, after which import
 * fails as expired and the message is dropped like a queue overflow.  FastBufferManager owns local
 * references and a shared-memory control block per exported allocation with a generation number and a
 * reader table keyed by process identity; the native descriptor travels inside that block.  No operation
 * waits for another process, and dead readers or owners are detected by PID plus start time.  Providers
 * whose runtime keeps imported allocations alive independently of the exporter still rely on the reader
 * table to know when an allocation may be overwritten.  Descriptors identify the host, device and
 * synchronization state as required by the backend.  OS handles require real handle transfer; a numeric
 * FD or device address is insufficient.
 *
 * Published contents are immutable until all readers finish.  The manager synchronizes before export;
 * descriptors identify resources and stay stable for each local reference.  The manager
 * synchronizes before final release; an allocation is reused only through zerocopy::FastBufferPool once no
 * local holder or live importer references it, and freed only once its reader table is empty.  External device
 * work must be completed by its owner before export, host access or release unless the provider explicitly
 * supports waiting for it.  CPU cache maintenance is not a device completion fence.  A crashed importer is
 * purged on the owner's next reclaim; forced exporter termination invalidates allocations whose SDK requires
 * its survival; imports that attach a new mapping detect the dead owner and call @c destroy_handle() for
 * named resources the operating system would otherwise keep.  Release buffers before manager shutdown.
 *
 * Implementations use @c VLINK_PLUGIN_DECLARE(Implementation, 1, 0).  The descriptor protocol identity
 * is separate from this interface ABI.  Compatible providers may share a protocol; unrelated providers
 * must use different nonzero IDs.  No CUDA, image format or operating-system type is required here.
 */
class FastBufferPluginInterface {
  VLINK_PLUGIN_REGISTER(FastBufferPluginInterface)

 protected:
  FastBufferPluginInterface() = default;

  virtual ~FastBufferPluginInterface() = default;

 public:
  /**
   * @brief Begins CPU access and invalidates stale host cache lines when reading.
   *
   * @param buffer Reference to map.
   * @param access CPU access direction.
   *
   * @return Host address, or nullptr when unsupported or failed.
   */
  [[nodiscard]] virtual uint8_t* map(const zerocopy::FastBuffer::Buffer& buffer,
                                     zerocopy::FastBuffer::Access access) noexcept = 0;

  /**
   * @brief Ends CPU access and flushes modified host cache lines when writing.
   *
   * @param buffer Previously mapped reference.
   * @param access Matching CPU access direction.
   *
   * @return Whether the operation succeeded; unsupported operations return false.
   */
  [[nodiscard]] virtual bool unmap(const zerocopy::FastBuffer::Buffer& buffer,
                                   zerocopy::FastBuffer::Access access) noexcept = 0;

  /**
   * @brief Returns a borrowed native resource for integration with the provider's device APIs.
   *
   * @param buffer Live reference.
   *
   * @return Provider-defined native object, or nullptr when unavailable.
   */
  [[nodiscard]] virtual const void* native_handle(const zerocopy::FastBuffer::Buffer& buffer) const noexcept {
    (void)buffer;
    return nullptr;
  }

  /**
   * @brief Retains a provider-native resource without taking the caller's reference.
   *
   * @param handle Provider-defined native object.
   * @param buffer Empty output reference; failure leaves it empty.
   *
   * @return Whether import succeeded; unsupported operations return false.
   */
  [[nodiscard]] virtual bool import_native(const void* handle, zerocopy::FastBuffer::Buffer& buffer) noexcept {
    (void)handle;
    (void)buffer;
    return false;
  }

  /**
   * @brief Returns the nonzero identity of the descriptor protocol, including its ABI version.
   *
   * @return Stable protocol ID shared only by interoperable implementations.
   */
  [[nodiscard]] virtual uint64_t get_protocol_id() const noexcept = 0;

  /**
   * @brief Allocates an uninitialised buffer in the requested memory domain.
   *
   * @param size Requested nonzero byte count.
   * @param config Requested memory domain and device.
   * @param buffer Empty output reference; success returns exactly @p size addressable bytes.
   *
   * @return Whether allocation succeeded.
   */
  [[nodiscard]] virtual bool create(size_t size, const zerocopy::FastBuffer::Config& config,
                                    zerocopy::FastBuffer::Buffer& buffer) noexcept = 0;

  /**
   * @brief Closes the SDK resource and clears it on success; the manager has completed local work.
   *
   * @param buffer Live reference to release; implementations report runtime failures in their logs.
   *
   * @return Whether the SDK reference was closed.  Failure preserves the reference and its buffer.
   */
  [[nodiscard]] virtual bool release(zerocopy::FastBuffer::Buffer& buffer) noexcept = 0;

  /**
   * @brief Waits until previously submitted work using this reference has completed.
   *
   * @param buffer Live reference whose device/context is selected by the plugin.
   *
   * @return Whether synchronization succeeded.
   */
  [[nodiscard]] virtual bool synchronize(const zerocopy::FastBuffer::Buffer& buffer) noexcept = 0;

  /**
   * @brief Copies between equal-sized buffers and completes the copy before returning.
   *
   * @param source Source reference; pending writes must be observed.
   * @param destination Independently allocated destination.
   *
   * @return Whether the copy succeeded; cross-device support is implementation-defined.
   */
  [[nodiscard]] virtual bool copy(const zerocopy::FastBuffer::Buffer& source,
                                  const zerocopy::FastBuffer::Buffer& destination) noexcept = 0;

  /**
   * @brief Reads the complete payload into an exactly sized host view.
   *
   * @param source Source reference; pending writes must be observed.
   * @param destination Writable host storage; its pointer and size must not be replaced.
   *
   * @return Whether the synchronous read completed.
   */
  [[nodiscard]] virtual bool copy_to_host(const zerocopy::FastBuffer::Buffer& source, Bytes& destination) noexcept = 0;

  /**
   * @brief Writes a complete host payload and completes the transfer before returning.
   *
   * @param source Host bytes, valid only for the duration of this call.
   * @param destination Equal-sized destination reference.
   *
   * @return Whether the synchronous write completed.
   */
  [[nodiscard]] virtual bool copy_from_host(const Bytes& source,
                                            const zerocopy::FastBuffer::Buffer& destination) noexcept = 0;

  /**
   * @brief Returns the fixed descriptor byte count for a live reference.
   *
   * @param buffer Reference to export.
   *
   * @return Nonzero descriptor size, or zero when sharing is unsupported.
   */
  [[nodiscard]] virtual size_t get_handle_size(const zerocopy::FastBuffer::Buffer& buffer) const noexcept {
    (void)buffer;
    return 0;
  }

  /**
   * @brief Exports the native descriptor, stable for this local reference; the manager handles synchronization.
   *
   * @param buffer Source reference, retained by the publisher until receivers have imported it.
   * @param descriptor Exactly sized host view; its pointer and size must not be replaced.
   *
   * @return Whether export succeeded.
   */
  [[nodiscard]] virtual bool export_handle(const zerocopy::FastBuffer::Buffer& buffer, Bytes& descriptor) noexcept {
    (void)buffer;
    (void)descriptor;
    return false;
  }

  /**
   * @brief Validates a native descriptor and acquires one local SDK reference.
   *
   * @param descriptor Untrusted wire bytes; the input storage is not retained.
   * @param buffer Empty output reference with an address valid in the importing process.
   *
   * @return Whether the protocol, host, device, allocation and synchronization are usable.
   */
  [[nodiscard]] virtual bool import_handle(const Bytes& descriptor, zerocopy::FastBuffer::Buffer& buffer) noexcept {
    (void)descriptor;
    (void)buffer;
    return false;
  }

  /**
   * @brief Removes named operating-system resources of a descriptor whose exporting process has died.
   *
   * @param descriptor Native descriptor produced by @c export_handle() in the dead process.
   *
   * @return Whether resources were removed; providers whose runtime reclaims them on process exit return false.
   *
   * @note Several processes may call this for the same descriptor; tolerate resources that are already gone.
   */
  [[nodiscard]] virtual bool destroy_handle(const Bytes& descriptor) noexcept {
    (void)descriptor;
    return false;
  }

 private:
  VLINK_DISALLOW_COPY_AND_ASSIGN(FastBufferPluginInterface)
};

}  // namespace vlink
