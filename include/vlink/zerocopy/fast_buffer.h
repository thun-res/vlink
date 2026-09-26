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
 * @file fast_buffer.h
 * @brief Platform-independent buffer with a @c Header and a pointer-free wire envelope.
 *
 * @details
 * @c FastBuffer pairs sequencing metadata with a reference to CPU, device or
 * other provider-managed storage.  The first construction obtains the singleton
 * @c FastBufferManager; @c VLINK_FASTBUFFER_PLUGIN selects a provider, while an
 * unset or empty variable selects ordinary CPU memory.
 *
 * | Operation              | Payload behavior                   | Ownership                   |
 * | ---------------------- | ---------------------------------- | --------------------------- |
 * | @c create()            | Allocates in the selected domain   | Owns one provider reference |
 * | Copy / @c deep_copy()  | Copies through the provider        | Owns independent storage    |
 * | @c shallow_copy()      | No payload copy                    | Retains the same allocation |
 * | Move / @c move_copy()  | No payload copy                    | Transfers the reference     |
 * | Shared deserialization | Imports a sharing descriptor       | Owns one import reference   |
 * | Host deserialization   | Copies bytes into provider storage | Owns independent storage    |
 *
 * @par Memory access
 * @code
 *                 FastBuffer
 *             +----------------+
 *             | Header         |
 *             | local address -+----> CPU / GPU / DMA / virtual storage
 *             | core ref       |                     |
 *             +----------------+                     |
 *                     |                              |
 *                     +-- map()/unmap()  <--- host-accessible mapping
 *                     |
 *                     +-- copy_to_host() ---> ordinary CPU Bytes
 * @endcode
 * @c address() is an opaque local address, not a CPU pointer.  Use @c map()
 * only when supported and pair it with @c unmap(); device-only allocations
 * require explicit host transfer.  Copies and synchronization use the provider.
 *
 * @par Memory layout
 * @code
 * Offset  Size  Field (64-bit targets)
 * ------  ----  ----------------------------------
 *      0    40  Header header
 *     40    32  Buffer buffer_
 *     72     8  MetadataBuffer* metadata_
 *     80     8  uint64_t reserved_buf_
 *     88     8  uint64_t reserved_buf2_
 * ------  ----  ----------------------------------
 *  Total    96  bytes (alignas 8)
 * @endcode
 *
 * @par Wire format
 * The fixed wire header is serialized independently of the local @c Metadata
 * view and C++ object.  Pointers, ownership references and process-local device indices
 * never travel in the envelope.  Byte order follows the other zero-copy types.
 * @code
 * Metadata offset  Size  Field
 * ---------------  ----  ----------------------------------
 *               0    40  Header header
 *              40     8  uint64_t size
 *              48     8  uint64_t protocol
 *              56     1  Storage storage
 *              57     1  MemoryType memory_type
 *              58     6  uint8_t reserved1[6] (zero)
 *              64     8  uint64_t metadata_size
 *              72    16  uint64_t reserved[2]
 * ---------------  ----  ----------------------------------
 *           Total    88  bytes (alignas 8)
 *
 * [ magic_begin (4) | version (4) | wire header (88) | metadata (M) | payload (N) | magic_end (4) ]
 * @endcode
 *
 * | Wire storage | Payload                           | Availability                        |
 * | ------------ | --------------------------------- | ----------------------------------- |
 * | Shared       | Control block name and generation | Compatible provider and live source |
 * | Host         | Complete CPU-readable bytes       | No sharing descriptor required      |
 *
 * @c operator>> uses a sharing descriptor when supported, otherwise host bytes.  A shared
 * descriptor names the allocation and its generation; importing after the owner reused
 * (@c FastBufferPool) or retired the allocation fails as expired, and the message is dropped.
 * @c MessageParser reads and prints metadata without importing the allocation.
 * Variable-length metadata is ordinary CPU storage, separate from the provider
 * payload.  @c set_metadata() copies input bytes or adopts an owned rvalue;
 * @c shallow_copy() retains that metadata allocation without copying its contents.
 *
 * @par Example
 * @code
 * vlink::zerocopy::FastBuffer buffer;
 * const auto input = vlink::Bytes::create(1024);
 * if (buffer.create(input.size()) && buffer.copy_from_host(input)) {
 *   vlink::Bytes wire;
 *   if (buffer >> wire) {
 *     vlink::zerocopy::FastBuffer received;
 *     if (received << wire) {
 *       vlink::Bytes output;
 *       const bool copied = received.copy_to_host(output);
 *     }
 *   }
 * }
 * @endcode
 * Publishers recycle allocations through @ref vlink::zerocopy::FastBufferPool; the sharing contract is
 * defined by @ref vlink::FastBufferPluginInterface and @ref vlink::FastBufferManager.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "../base/bytes.h"
#include "./header.h"

namespace vlink {

namespace zerocopy {

class FastBufferPool;

/**
 * @struct FastBuffer
 * @brief Holds a local memory address and an owned provider reference.
 *
 * @details
 * Construction initializes @c FastBufferManager; operations obtain its provider without storing a plugin pointer.
 * Release buffers before manager shutdown.  Copy construction and assignment
 * deep-copy through the provider; @c shallow_copy explicitly retains the same allocation without payload
 * copying.  A retained reference survives its source object.
 * Moving transfers the reference and leaves the source empty.
 *
 * The wire format is [magic (4), version (4), fixed header (88), metadata (M), payload (N), end magic (4)] in the same
 * native byte order as other zero-copy containers.  Shared payloads name a control block, not local
 * addresses.  Host payloads contain actual bytes and are copied into provider storage on deserialization.
 * Local C++ object memory, plugin references and device indices are never serialized.
 *
 * Releasing the last local reference of an exported allocation never waits for importers.
 * Sharing lifetime follows @ref vlink::FastBufferPluginInterface.
 */
struct VLINK_EXPORT_AND_ALIGNED(8) FastBuffer final {
  /**
   * @enum MemoryType
   * @brief Allocation domain; unsupported requests fail instead of changing domains.
   */
  enum MemoryType : uint8_t {
    kMemoryDefault = 0,  ///< Provider default.
    kMemoryHost = 1,     ///< CPU memory, including provider-managed pinned memory.
    kMemoryDevice = 2,   ///< Device-local memory.
    kMemoryShared = 3,   ///< Memory shared by CPU and devices or processes.
    kMemoryDmaBuf = 4,   ///< DMA-BUF resource, possibly without a permanent mapping.
    kMemoryVirtual = 5,  ///< Provider-managed virtual memory.
  };

  /**
   * @struct Config
   * @brief Allocation request independent of image, tensor and vendor APIs.
   */
  struct Config final {
    MemoryType memory_type{kMemoryDefault};  ///< Requested allocation domain.
    int32_t device{-1};                      ///< Provider-local device, or the default.
  };

  /**
   * @enum Access
   * @brief CPU access direction for map/unmap and cache maintenance.
   */
  enum Access : uint8_t {
    kRead = 1,       ///< CPU reads.
    kWrite = 2,      ///< CPU writes.
    kReadWrite = 3,  ///< CPU reads and writes.
  };

  /**
   * @struct Buffer
   * @brief One local reference to a contiguous allocation in the provider's address space.
   */
  struct Buffer final {
    uint8_t* data{nullptr};                  ///< Optional local address; CPU accessibility requires map().
    size_t size{0};                          ///< Addressable byte count.
    void* handle{nullptr};                   ///< Core reference in FastBuffer; optional SDK state in interface calls.
    int32_t device{-1};                      ///< Plugin-local device index; never interpreted across processes.
    MemoryType memory_type{kMemoryDefault};  ///< Actual allocation domain.
  };

  /**
   * @enum Storage
   * @brief Meaning of the wire payload.
   */
  enum Storage : uint8_t {
    kStorageShared = 1,  ///< Runtime sharing descriptor; requires a compatible live provider.
    kStorageHost = 2,    ///< Inline host bytes; contains no runtime sharing handle.
  };

  /**
   * @struct Metadata
   * @brief Decoded metadata and a variable-length CPU buffer, readable without loading a plugin.
   *
   * @details
   * @c read_metadata() borrows the variable-length buffer from its input wire
   * storage.  Keep that input alive while reading @c buffer.  This C++ view is
   * not copied directly to the wire.
   */
  struct Metadata final {
    Header header;                           ///< Sequencing and timestamps.
    uint64_t size{0};                        ///< Actual payload size in bytes, excluding any descriptor.
    uint64_t protocol{0};                    ///< Descriptor protocol ID; zero for host storage.
    Storage storage{kStorageShared};         ///< Shared descriptor or inline host data.
    MemoryType memory_type{kMemoryDefault};  ///< Producer allocation domain.
    uint64_t reserved[2]{};                  ///< 16 reserved bytes, preserved across serialization and copies.
    Bytes buffer;                            ///< Variable-length CPU metadata, separate from the provider payload.
  };

  /**
   * @brief Obtains the singleton provider and constructs an empty buffer.
   */
  FastBuffer();

  /**
   * @brief Releases this reference after local device use completes.
   */
  ~FastBuffer() noexcept;

  /**
   * @brief Deep-copies @p target on the device; failure leaves this buffer empty.
   *
   * @param target Source buffer.
   */
  FastBuffer(const FastBuffer& target) noexcept;

  /**
   * @brief Transfers @p target's reference and metadata.
   *
   * @param target Source buffer, left empty.
   */
  FastBuffer(FastBuffer&& target) noexcept;

  /**
   * @brief Deep-copy-assigns @p target; failure preserves this buffer.
   *
   * @param target Source buffer.
   *
   * @return This buffer.
   */
  FastBuffer& operator=(const FastBuffer& target) noexcept;

  /**
   * @brief Move-assigns @p target; self-assignment is a no-op.
   *
   * @param target Source buffer, left empty.
   *
   * @return This buffer.
   */
  FastBuffer& operator=(FastBuffer&& target) noexcept;

  /**
   * @brief Imports shared storage or uploads host storage.
   *
   * @param bytes Complete wire envelope.
   *
   * @return Whether decoding succeeded; failure preserves this buffer.
   */
  [[nodiscard]] bool operator<<(const Bytes& bytes) noexcept;

  /**
   * @brief Serializes a sharing descriptor, or host bytes when sharing is unavailable.
   *
   * @param bytes Output storage, reused when exactly sized; incompatible loans are rejected.
   *
   * @return Whether serialization succeeded; shared mode does not copy the payload.
   */
  [[nodiscard]] bool operator>>(Bytes& bytes) const noexcept;

  /**
   * @brief Validates the complete wire envelope without loading a plugin.
   *
   * @param bytes Wire bytes.
   *
   * @return Whether metadata and payload lengths are valid.
   */
  [[nodiscard]] static bool check_valid(const Bytes& bytes) noexcept;

  /**
   * @brief Reads validated metadata without importing device memory.
   *
   * @param bytes Complete wire envelope.
   * @param metadata Output metadata, unchanged on failure.
   *
   * @return Whether the envelope is valid.
   */
  [[nodiscard]] static bool read_metadata(const Bytes& bytes, Metadata& metadata) noexcept;

  /**
   * @brief Returns the selected wire size.
   *
   * @return Envelope plus descriptor or host bytes, or zero when empty.
   */
  [[nodiscard]] size_t get_serialized_size() const noexcept;

  /**
   * @brief Reports whether this object holds a live memory reference.
   *
   * @return Whether the size and core reference are valid; a mapping is not required.
   */
  [[nodiscard]] bool is_valid() const noexcept;

  /**
   * @brief Allocates an uninitialised buffer in the provider's default memory domain.
   *
   * @param size Nonzero byte count.
   * @param device Plugin-local device index, or -1 for the default.
   *
   * @return Whether allocation succeeded; failure preserves this buffer.
   */
  [[nodiscard]] bool create(size_t size, int32_t device = -1) noexcept;

  /**
   * @brief Allocates memory in the requested domain.
   *
   * @param size Nonzero byte count.
   * @param config Memory type and provider-local device.
   *
   * @return Whether the provider supports and satisfies the request.
   */
  [[nodiscard]] bool create(size_t size, const Config& config) noexcept;

  /**
   * @brief Retains @p target's allocation without copying its payload.
   *
   * @param target Source buffer.
   *
   * @return Whether an independent reference was acquired; self-copy returns false.
   */
  [[nodiscard]] bool shallow_copy(const FastBuffer& target) noexcept;

  /**
   * @brief Allocates and copies @p target through its provider.
   *
   * @param target Source buffer.
   *
   * @return Whether copying succeeded; self-copy returns false.
   */
  [[nodiscard]] bool deep_copy(const FastBuffer& target) noexcept;

  /**
   * @brief Transfers @p target's reference and metadata.
   *
   * @param target Source buffer, left empty.
   *
   * @return Whether a move occurred; self-move returns false.
   */
  bool move_copy(FastBuffer& target) noexcept;

  /**
   * @brief Releases the memory reference and resets metadata.
   */
  void clear() noexcept;

  /**
   * @brief Waits for previously submitted local device work.
   *
   * @return Whether synchronization succeeded.
   */
  [[nodiscard]] bool synchronize() const noexcept;

  /**
   * @brief Copies the full payload to host storage.
   *
   * @param bytes Destination host buffer, reused when exactly sized.
   *
   * @return Whether the synchronous read completed.
   */
  [[nodiscard]] bool copy_to_host(Bytes& bytes) const noexcept;

  /**
   * @brief Writes the full payload from equally sized host bytes.
   *
   * @param bytes Source host storage.
   *
   * @return Whether the synchronous write completed.
   */
  [[nodiscard]] bool copy_from_host(const Bytes& bytes) noexcept;

  /**
   * @brief Returns the optional process-local address as an opaque integer.
   *
   * @return Address value, or zero when unmapped/empty; use map() for CPU access.
   */
  [[nodiscard]] uintptr_t address() const noexcept;

  /**
   * @brief Returns the payload byte count.
   *
   * @return Zero when empty.
   */
  [[nodiscard]] size_t size() const noexcept;

  /**
   * @brief Returns the plugin-local device index.
   *
   * @return Device index, or -1 when empty.
   */
  [[nodiscard]] int32_t device() const noexcept;

  /**
   * @brief Returns the actual allocation domain.
   *
   * @return Provider-reported memory type.
   */
  [[nodiscard]] MemoryType memory_type() const noexcept;

  /**
   * @brief Reports whether this object owns a plugin reference.
   *
   * @return Whether destruction must release a reference, including retained/imported buffers.
   */
  [[nodiscard]] bool is_owner() const noexcept;

  /**
   * @brief Maps the buffer for CPU access and performs required cache maintenance.
   *
   * @param access Read, write or read-write access.
   * @return Host pointer, or nullptr when mapping is unsupported or failed.
   *
   * @note Pair successful calls with @c unmap().  Finish external device work before mapping.
   */
  [[nodiscard]] uint8_t* map(Access access) const noexcept;

  /**
   * @brief Ends CPU access and publishes CPU writes when required.
   *
   * @param access Access mode used for the matching @c map().
   * @return Whether unmapping/cache maintenance completed.
   */
  [[nodiscard]] bool unmap(Access access) const noexcept;

  /**
   * @brief Returns a borrowed provider-native resource description.
   *
   * @return Provider-defined native object, valid while this reference is alive.
   */
  [[nodiscard]] const void* native_handle() const noexcept;

  /**
   * @brief Retains an externally supplied provider-native resource.
   *
   * @param handle Native object matching the loaded provider's documented type.
   * @return Whether an independent reference was acquired; unsupported imports return false.
   */
  [[nodiscard]] bool import_native(const void* handle) noexcept;

  /**
   * @brief Copies variable-length CPU metadata into owned storage.
   *
   * @param bytes Metadata bytes; empty input clears the metadata.
   *
   * @return Whether copying succeeded; failure preserves the previous metadata.
   */
  [[nodiscard]] bool set_metadata(const Bytes& bytes) noexcept;

  /**
   * @brief Adopts owned metadata bytes; borrowed input is copied into owned storage.
   *
   * @param bytes Metadata bytes moved on successful adoption.
   *
   * @return Whether metadata storage was acquired.
   */
  [[nodiscard]] bool set_metadata(Bytes&& bytes) noexcept;

  /**
   * @brief Returns the variable-length CPU metadata.
   *
   * @return Borrowed read-only bytes, valid until metadata replacement or destruction.
   */
  [[nodiscard]] const Bytes& get_metadata() const noexcept;

  /**
   * @brief Returns the first reserved field.
   *
   * @return Reference to @c reserved_buf_.
   */
  uint64_t& get_reserved() noexcept { return reserved_buf_; }

  /**
   * @brief Returns the second reserved field.
   *
   * @return Reference to @c reserved_buf2_.
   */
  uint64_t& get_reserved2() noexcept { return reserved_buf2_; }

  Header header;  ///< Sequencing and timestamp metadata prefix.

  static constexpr bool kZerocopyTypes{true};  ///< Serializer zero-copy schema marker.

 private:
  friend class FastBufferPool;

  struct MetadataBuffer;

  static MetadataBuffer* copy_metadata(const Bytes& bytes) noexcept;

  void clear_metadata() noexcept;

  Buffer buffer_;
  MetadataBuffer* metadata_{nullptr};
  uint64_t reserved_buf_{0};
  uint64_t reserved_buf2_{0};

  static constexpr uint32_t kMagicNumberBegin{0x98B7F1BA};
  static constexpr uint32_t kMagicNumberEnd{0x98B7F1BF};
};

}  // namespace zerocopy

}  // namespace vlink
