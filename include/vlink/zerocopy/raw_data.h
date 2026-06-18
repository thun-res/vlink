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
 * @file raw_data.h
 * @brief Generic zero-copy byte-buffer container with a @c Header prefix.
 *
 * @details
 * @c RawData is the most lightweight container in the @c vlink::zerocopy
 * family.  It pairs an opaque payload (any application-defined byte blob:
 * Protobuf wire bytes, FlatBuffer tables, compressed frames, encrypted CAN
 * snapshots, ...) with a 40-byte @c Header so that consumers can sequence and
 * time-stamp the payload without parsing it.  Use @c RawData whenever the
 * payload schema is opaque to VLink, or when a higher-level layer wraps its
 * own framing on top.
 *
 * | Ownership mode | How to create                       | Frees on destruction  |
 * | -------------- | ----------------------------------- | --------------------- |
 * | Owned          | @c create(size)                     | Yes                   |
 * | Borrowed       | @c shallow_copy(ptr, size)          | No                    |
 * | Deserialised   | @c operator<<(bytes)                | No (borrows @c bytes) |
 *
 * @par Wire format
 * @c RawData is POD; the canonical serialiser is @c memcpy().  The @c sizeof
 * value is locked by @c static_assert and forms a permanent binary contract:
 * VLink zero-copy containers have NO forward and NO backward compatibility
 * across versions, and every field including the @c reserved slot is part of
 * that contract.
 * @code
 * static_assert(sizeof(RawData) == 64, "Sizeof must be 64 bytes.");
 * @endcode
 *
 * @par Memory layout
 * @code
 * Offset  Size  Field
 * ------  -----  --------------------------------
 *      0     40  Header header
 *     40      8  uint8_t* data_
 *     48      8  size_t   size_
 *     56      1  bool     is_owner_
 *     57      1  (padding)
 *     58      2  uint16_t reserved_buf_
 *     60      4  (tail padding to align(8))
 * ------  -----  --------------------------------
 *  Total     64  bytes (alignas 8)
 *
 * Wire envelope:
 * [ magic_begin (4) | version (4) | RawData struct (64) | payload (size_) | magic_end (4) ]
 * @endcode
 *
 * @par Reserved bytes
 * @c reserved_buf_ travels through the wire format unchanged.  It is exposed
 * via @c get_reserved() so applications can stash a flag or minor sub-type id
 * without inflating the struct, but the slot must not be redefined later --
 * doing so would silently break any peer that still expects the old meaning.
 *
 * @par Example
 * @code
 * vlink::zerocopy::RawData rd;
 * rd.header.seq      = ++seq;
 * rd.header.time_pub = vlink::time_ns();
 * rd.create(payload.size());
 * std::memcpy(const_cast<uint8_t*>(rd.data()), payload.data(), payload.size());
 *
 * vlink::Bytes wire;
 * rd >> wire;
 *
 * vlink::zerocopy::RawData rx;
 * rx << wire;
 * @endcode
 */

#pragma once

#include <cstdint>

#include "../base/bytes.h"
#include "./header.h"

namespace vlink {

namespace zerocopy {

/**
 * @struct RawData
 * @brief 64-byte POD container that wraps an opaque byte payload with a @c Header prefix.
 *
 * @details
 * The struct is locked at 64 bytes on 64-bit targets via @c static_assert;
 * 32-bit toolchains emit a build-time warning because the embedded pointer
 * and size widths shift the layout.  The wire envelope uses magic-number
 * sentinels to detect truncation and corruption before @c memcpy of the
 * struct header.
 */
struct VLINK_EXPORT_AND_ALIGNED(8) RawData final {
  /**
   * @brief Default-constructs an empty container and verifies the sizeof contract.
   */
  RawData() noexcept;

  /**
   * @brief Releases the owned buffer when @c is_owner() is @c true.
   */
  ~RawData() noexcept;

  /**
   * @brief Deep-copies the payload of @p target into a freshly allocated owned buffer.
   *
   * @param target Source container to clone.
   */
  RawData(const RawData& target) noexcept;

  /**
   * @brief Steals @p target's ownership and metadata; @p target is left empty.
   *
   * @param target Source container moved from.
   */
  RawData(RawData&& target) noexcept;

  /**
   * @brief Deep-copy-assigns from @p target; self-assignment is a no-op.
   *
   * @param target Source container to clone.
   * @return Reference to @c *this.
   */
  RawData& operator=(const RawData& target) noexcept;

  /**
   * @brief Move-assigns @p target's resources into @c *this; self-assignment is a no-op.
   *
   * @param target Source container moved from.
   * @return Reference to @c *this.
   */
  RawData& operator=(RawData&& target) noexcept;

  /**
   * @brief Deserialises a @c RawData from @p bytes using zero-copy borrowing semantics.
   *
   * @details
   * Validates the magic-number envelope and total length, then borrows the
   * payload pointer from @p bytes.  The caller must keep @p bytes alive for as
   * long as this @c RawData is used.
   *
   * @param bytes Wire buffer previously produced by @c operator>>.
   * @return @c true on success; @c false on magic mismatch or size mismatch.
   */
  bool operator<<(const Bytes& bytes) noexcept;

  /**
   * @brief Serialises the struct snapshot plus payload into @p bytes.
   *
   * @param bytes Output buffer; resized automatically when too small.
   * @return Always @c true.
   */
  bool operator>>(Bytes& bytes) const noexcept;

  /**
   * @brief Validates that @p bytes carries a well-formed @c RawData envelope.
   *
   * @param bytes Buffer to inspect.
   * @return @c true when the magic sentinels match and the minimum length holds.
   */
  [[nodiscard]] static bool check_valid(const Bytes& bytes) noexcept;

  /**
   * @brief Total bytes that @c operator>> would write for this container.
   *
   * @return @c sizeof(magic_begin) + @c sizeof(version) + @c sizeof(RawData) + @c size() + @c sizeof(magic_end).
   */
  [[nodiscard]] size_t get_serialized_size() const noexcept;

  /**
   * @brief Whether the payload pointer is non-null and the byte size is positive.
   *
   * @return @c true when the container holds usable data.
   */
  [[nodiscard]] bool is_valid() const noexcept;

  /**
   * @brief Borrows @p target's payload pointer without copying.
   *
   * @param target Source container whose buffer must outlive @c *this.
   * @return @c false on self-borrow, otherwise @c true.
   */
  bool shallow_copy(const RawData& target) noexcept;

  /**
   * @brief Allocates (or reuses) an owned buffer and copies @p target's payload.
   *
   * @param target Source container to clone.
   * @return @c false on self-copy, otherwise @c true.
   */
  bool deep_copy(const RawData& target) noexcept;

  /**
   * @brief Transfers ownership from @p target; @p target becomes empty.
   *
   * @param target Source container moved from.
   * @return @c false on self-move, otherwise @c true.
   */
  bool move_copy(RawData& target) noexcept;

  /**
   * @brief Allocates an uninitialised owned buffer of @p size bytes.
   *
   * @param size Byte count; must be non-zero.
   * @return @c false when @p size is zero, otherwise @c true.
   */
  bool create(size_t size) noexcept;

  /**
   * @brief Releases the owned buffer (if any) and zeroes the @c Header.
   */
  void clear() noexcept;

  /**
   * @brief Borrows an externally owned raw byte buffer without copying.
   *
   * @param data Non-null source pointer that must outlive @c *this.
   * @param size Buffer length in bytes; must be non-zero.
   * @return @c false on invalid arguments or unchanged pointer, otherwise @c true.
   */
  bool shallow_copy(uint8_t* data, size_t size) noexcept;

  /**
   * @brief Copies @p size bytes from @p data into an owned buffer (allocating as needed).
   *
   * @param data Non-null source pointer.
   * @param size Number of bytes to copy; must be non-zero.
   * @return @c false on invalid arguments or aliasing, otherwise @c true.
   */
  bool deep_copy(uint8_t* data, size_t size) noexcept;

  /**
   * @brief Compatibility alias for @c deep_copy(uint8_t*, size_t).
   *
   * @param data Source pointer.
   * @param size Number of bytes.
   * @return Result of the delegated @c deep_copy call.
   */
  bool fill_data(uint8_t* data, size_t size) noexcept;

  /**
   * @brief Mutable accessor for the 16-bit user-reserved field carried in the wire format.
   *
   * @return Reference to @c reserved_buf_.
   */
  [[nodiscard]] uint16_t& get_reserved() noexcept;

  /**
   * @brief Read-only pointer to the payload bytes.
   *
   * @return Pointer to payload start; may be non-null with @c size() == 0 for empty deserialised frames.
   */
  [[nodiscard]] const uint8_t* data() const noexcept;

  /**
   * @brief Payload size in bytes (0 when empty).
   *
   * @return Byte count.
   */
  [[nodiscard]] size_t size() const noexcept;

  /**
   * @brief Whether this container currently owns its buffer.
   *
   * @return @c true when the destructor would free the buffer.
   */
  [[nodiscard]] bool is_owner() const noexcept;

  Header header;  ///< Sequencing and timestamp metadata prefix.

  static constexpr bool kZerocopyTypes{true};  ///< Marker probed by the VLink type-trait machinery.

 private:
  uint8_t* data_{nullptr};
  size_t size_{0};
  bool is_owner_{false};
  uint16_t reserved_buf_{0};

  static constexpr uint32_t kMagicNumberBegin{0x98B7F11A};
  static constexpr uint32_t kMagicNumberEnd{0x98B7F11F};
};

}  // namespace zerocopy

}  // namespace vlink
