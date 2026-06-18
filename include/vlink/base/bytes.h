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
 * @file bytes.h
 * @brief Canonical 128-byte binary payload carrier with inline storage, multi-mode ownership and LZAV compression.
 *
 * @details
 * Every binary buffer that crosses a VLink boundary (publish, subscribe, RPC argument, field value,
 * proxy snapshot) flows through a @c vlink::Bytes object.  The class fuses five orthogonal concerns
 * into a single fixed-size 128-byte structure: small-buffer optimisation, ownership tagging,
 * loaned-memory tracking, prefix-offset reservation and a compression / encoding utility surface.
 *
 * @par Ownership model
 *
 * | Factory                       | Owns memory | Frees on destroy | Aliases source pointer | Typical caller        |
 * | ----------------------------- | ----------- | ---------------- | ---------------------- | --------------------- |
 * | @c Bytes::create              | yes         | yes              | no                     | Fresh allocation      |
 * | @c Bytes::shallow_copy        | no          | no               | yes                    | Zero-copy view        |
 * | @c Bytes::deep_copy           | when sized  | when sized       | no                     | Detached owned copy   |
 * | @c Bytes::loan_internal       | no (loaned) | no               | yes                    | Iceoryx zero-copy     |
 * | @c Bytes::shallow_copy_ptr    | no          | no               | yes                    | Opaque pointer wrap   |
 *
 * @par Memory layout (logical)
 *
 * @verbatim
 *  +------------------------------------------------------------------+
 *  |                          Bytes (128 B)                           |
 *  +-------------------------+-------+----+------+-------+------------+
 *  | stack_data_ (96 B SBO)  | owner | ln | off  | size  | capacity   |
 *  +-------------------------+-------+----+------+-------+------------+
 *                                              |
 *                                              | (when size > 96 B)
 *                                              v
 *                                +--------------------------------+
 *                                |  heap buffer from MemoryPool   |
 *                                |  [ offset prefix ][ payload ]  |
 *                                +--------------------------------+
 * @endverbatim
 *
 * @par Compression frame layout
 *
 * @verbatim
 *  byte:    0   1   2   3   4   5   6   7   8                       N-4  N-3  N-2  N-1
 *         +---+---+---+---+---+---+---+---+---+ - - - - - - - - - +----+----+----+----+
 *  field: | header magic  | original size BE  |  LZAV payload     |  footer magic     |
 *         +---+---+---+---+---+---+---+---+---+ - - - - - - - - - +----+----+----+----+
 *           17  49  B2  6F                                            A7   05   ED   71
 * @endverbatim
 *
 * Buffers below or equal to @c kStackSize (96 bytes) reside entirely in @c stack_data_; only larger
 * payloads pull from @c MemoryPool::global_instance() through @c bytes_malloc.  The total object
 * footprint is fixed at 128 bytes regardless of payload size.  An @c offset prefix reserves space
 * at the head of the buffer so transport adapters can prepend protocol headers without realloc;
 * @c data() returns @c real_data() @c + @c offset().
 *
 * @par Example
 * @code
 *   vlink::Bytes a = vlink::Bytes::create(64);            // SBO path, no heap allocation
 *   std::memcpy(a.data(), payload, a.size());
 *
 *   vlink::Bytes view = vlink::Bytes::shallow_copy(ext, ext_size);   // zero-copy alias
 *
 *   auto packed = vlink::Bytes::compress_data(a.data(), a.size());
 *   if (vlink::Bytes::is_compress_data(packed.data(), packed.size())) {
 *     vlink::Bytes plain = vlink::Bytes::uncompress_data(packed.data(), packed.size());
 *   }
 *
 *   const uint32_t crc = vlink::Bytes::get_crc_32(a);
 *   const std::string base64 = vlink::Bytes::encode_to_base64(a);
 *   vlink::Bytes round_trip = vlink::Bytes::decode_from_base64(base64);
 * @endcode
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "./macros.h"

namespace vlink {

/**
 * @class Bytes
 * @brief Fixed-size 128-byte buffer holder with SBO, five ownership modes and integrated codecs.
 *
 * @details
 * Implements VLink's universal binary carrier.  Small payloads live inside the embedded
 * @c stack_data_ array; larger payloads spill to the global @c MemoryPool.  Ownership is encoded
 * in two flags (@c is_owner_, @c is_loaned_) so the destructor can route to the correct release
 * primitive: heap free, iceoryx loan release, or no-op for shallow aliases.  All public functions
 * are @c noexcept; failure is reported via empty return values.
 */
class VLINK_EXPORT Bytes final {  // size == 128 bytes
 public:
  /**
   * @brief Eagerly constructs the process-wide @c MemoryPool that backs heap allocations.
   *
   * @details
   * @c Bytes::bytes_malloc routes through @c MemoryPool::global_instance().  Calling this once
   * at program start front-loads the singleton construction cost and respects
   * @c VLINK_MEMORY_LEVEL / @c VLINK_MEMORY_PREALLOC.  Subsequent calls are idempotent no-ops.
   */
  static void init_memory_pool() noexcept;

  /**
   * @brief Releases every empty chunk currently cached by the @c Bytes memory pool.
   *
   * @details
   * Forwards to @c MemoryPool::global_instance().trim().  Only chunks whose blocks are entirely
   * on their tier's free list are returned to the system allocator; chunks still backing a live
   * @c Bytes instance are preserved.  Lifetime statistics and the geometric chunk-growth state
   * are kept intact.
   *
   * @note Safe to invoke concurrently with allocations and frees; treat as a periodic maintenance
   *       call rather than a hot-path primitive.
   */
  static void release_memory_pool() noexcept;

  /**
   * @brief Allocates a raw aligned byte buffer through the global memory pool.
   *
   * @param size  Number of bytes requested.
   * @return Pointer to the newly allocated buffer, or @c nullptr on upstream OOM.
   * @note The same @p size value must be passed to the matching @c bytes_free call.
   */
  [[nodiscard]] static uint8_t* bytes_malloc(size_t size) noexcept;

  /**
   * @brief Returns a buffer previously obtained from @c bytes_malloc to the pool.
   *
   * @param ptr   Pointer returned by @c bytes_malloc.  @c nullptr is a no-op.
   * @param size  Original size; must match the value passed to @c bytes_malloc.
   */
  static void bytes_free(uint8_t* ptr, size_t size) noexcept;

  /**
   * @brief Allocates an owned buffer of the requested size with an optional header offset.
   *
   * @details
   * Payloads up to @c kStackSize stay in @c stack_data_; larger payloads use the memory pool.
   * The content is left uninitialised.  When @p offset is non-zero the first @p offset bytes of
   * the backing buffer are reserved so transport layers can prepend frame headers in place;
   * @c data() then points past the reserved prefix.
   *
   * @param size    Number of usable payload bytes after construction.
   * @param offset  Header bytes reserved before the payload region.  Default: @c 0.
   * @return New owning @c Bytes instance.
   */
  [[nodiscard]] static Bytes create(size_t size, uint8_t offset = 0) noexcept;

  /**
   * @brief Wraps an external mutable buffer as a non-owning alias.
   *
   * @details
   * Performs no allocation and no copy.  The caller guarantees the lifetime of @p data exceeds
   * the lifetime of the returned object.
   *
   * @param data  External buffer to alias.
   * @param size  Length of the buffer in bytes.
   * @return Non-owning @c Bytes pointing at @p data.
   */
  [[nodiscard]] static Bytes shallow_copy(uint8_t* data, size_t size) noexcept;

  /**
   * @brief Wraps an external read-only buffer as a non-owning alias.
   *
   * @details
   * Identical to the mutable overload; the @c const pointer is stored verbatim through a
   * @c const_cast so the non-const @c data() accessor returns the same address.
   *
   * @param data  External read-only buffer to alias.
   * @param size  Length of the buffer in bytes.
   * @return Non-owning @c Bytes pointing at @p data.
   */
  [[nodiscard]] static Bytes shallow_copy(const uint8_t* data, size_t size) noexcept;

  /**
   * @brief Wraps an opaque pointer as a zero-size pointer carrier.
   *
   * @details
   * Sets @c size() and @c offset() to @c 0 so @c is_ptr() reports @c true.  The wrapped pointer
   * is retrieved through @c to_ptr<T>(); ownership stays with the caller.
   *
   * @param data  Opaque pointer value to embed.
   * @return Non-owning, zero-size @c Bytes carrying @p data.
   */
  [[nodiscard]] static Bytes shallow_copy_ptr(void* data) noexcept;

  /**
   * @brief Produces an owned copy of an external mutable buffer.
   *
   * @details
   * Allocates a fresh buffer and @c memcpy s @p size bytes from @p data into it.  When the source
   * is empty (null pointer or zero size) and @p offset is @c 0 the result is empty and non-owning;
   * with a non-zero @p offset only the prefix region is allocated.
   *
   * @param data    Source buffer.
   * @param size    Number of bytes to copy.
   * @param offset  Header bytes reserved in the new buffer.  Default: @c 0.
   * @return Owning @c Bytes containing the copied payload.
   */
  [[nodiscard]] static Bytes deep_copy(uint8_t* data, size_t size, uint8_t offset = 0) noexcept;

  /**
   * @brief Produces an owned copy of an external read-only buffer.
   *
   * @details
   * Read-only overload of @c deep_copy(uint8_t*, size_t, uint8_t).  Ownership rules for empty
   * sources match the mutable overload exactly.
   *
   * @param data    Source read-only buffer.
   * @param size    Number of bytes to copy.
   * @param offset  Header bytes reserved in the new buffer.  Default: @c 0.
   * @return Owning @c Bytes containing the copied payload.
   */
  [[nodiscard]] static Bytes deep_copy(const uint8_t* data, size_t size, uint8_t offset = 0) noexcept;

  /**
   * @brief Wraps an iceoryx-loaned mutable payload as a non-owning, non-aliasing carrier.
   *
   * @details
   * Marks @c is_loaned() as @c true so the destructor skips the free call -- the underlying
   * memory is owned by RouDi.  Used internally by the @c shm:// transport backend.
   *
   * @param data  Pointer to the iceoryx chunk payload.
   * @param size  Length of the payload in bytes.
   * @return Loaned @c Bytes instance.
   */
  [[nodiscard]] static Bytes loan_internal(uint8_t* data, size_t size) noexcept;

  /**
   * @brief Wraps an iceoryx-loaned read-only payload as a non-owning, non-aliasing carrier.
   *
   * @param data  Pointer to the read-only iceoryx chunk payload.
   * @param size  Length of the payload in bytes.
   * @return Loaned @c Bytes instance.
   */
  [[nodiscard]] static Bytes loan_internal(const uint8_t* data, size_t size) noexcept;

  /**
   * @brief Builds an owned @c Bytes from the bytes of a UTF-8 string.
   *
   * @param str     Source string; copied byte-for-byte.
   * @param offset  Header bytes reserved before the payload.  Default: @c 0.
   * @return Owning @c Bytes containing @p str.  Empty input with zero offset yields an empty result.
   */
  [[nodiscard]] static Bytes from_string(const std::string& str, uint8_t offset = 0) noexcept;

  /**
   * @brief Parses a user-typed hex literal into a @c Bytes payload.
   *
   * @details
   * Accepts space-separated byte tokens (@c "1A @c 2B"), a contiguous even-length hex run with
   * or without a @c 0x / @c 0X prefix (@c "0x1A2B"), or mixed forms.  Returns an empty result on
   * parse failure and sets @p ok to @c false.
   *
   * @param str  Source hex string.
   * @param ok   Optional pointer set to @c true on success and @c false on failure.
   * @return Parsed @c Bytes, or an empty value on failure.
   */
  [[nodiscard]] static Bytes from_user_input(const std::string& str, bool* ok = nullptr) noexcept;

  /**
   * @brief Renders a raw byte array as space-separated uppercase hex tokens.
   *
   * @param value  Pointer to the source buffer.
   * @param size   Number of bytes to render.
   * @return Hex string such as @c "1A B2 C3" for the input @c {0x1A, @c 0xB2, @c 0xC3}.
   */
  [[nodiscard]] static std::string convert_to_hex_str(const uint8_t* value, size_t size) noexcept;

  /**
   * @brief Returns a new owned @c Bytes with the byte order of @p target reversed.
   *
   * @param target  Source buffer to reverse.
   * @return New owned buffer with reversed byte order.
   */
  [[nodiscard]] static Bytes reverse_order(const Bytes& target) noexcept;

  /**
   * @brief Encodes a payload as a standard Base-64 ASCII string.
   *
   * @param target  Source buffer.
   * @return Base-64 string representation.
   */
  [[nodiscard]] static std::string encode_to_base64(const Bytes& target) noexcept;

  /**
   * @brief Decodes a Base-64 ASCII string back into a binary payload.
   *
   * @param target  Base-64 source string.
   * @return Decoded @c Bytes, or an empty value on invalid input.
   */
  [[nodiscard]] static Bytes decode_from_base64(const std::string& target) noexcept;

  /**
   * @brief Computes the CRC-32 (ISO-HDLC) checksum of @p target.
   *
   * @param target  Source buffer.
   * @return 32-bit CRC-32 value.
   */
  [[nodiscard]] static uint32_t get_crc_32(const Bytes& target) noexcept;

  /**
   * @brief Computes the CRC-64 (ECMA-182) checksum of @p target.
   *
   * @param target  Source buffer.
   * @return 64-bit CRC-64 value.
   */
  [[nodiscard]] static uint64_t get_crc_64(const Bytes& target) noexcept;

  /**
   * @brief Constructs an empty, non-owning carrier with no payload.
   *
   * @details
   * @c data() is @c nullptr and @c size() is @c 0; both @c is_owner() and @c is_loaned() are
   * @c false.  The SBO region is zero-initialised.
   */
  Bytes() noexcept;

  /**
   * @brief Copy constructor; converts any source into an owned deep copy.
   *
   * @details
   * Allocates a fresh buffer through the memory pool and copies @p target's bytes into it when
   * @p target carries data.  Empty inputs yield an empty non-owning result.  The copy is always
   * an owner regardless of the source's ownership tags -- aliasing and loaned semantics are not
   * preserved by this constructor.
   *
   * @param target  Source buffer.
   * @note To keep aliasing or loaned semantics use the explicit factory methods
   *       (@c shallow_copy / @c loan_internal) instead of the copy constructor.
   */
  Bytes(const Bytes& target) noexcept;

  /**
   * @brief Move constructor; transfers payload, ownership flags and prefix from @p target.
   *
   * @param target  Source buffer left in the empty state after the move.
   */
  Bytes(Bytes&& target) noexcept;

  /**
   * @brief Constructs an owned buffer from an initialiser list of bytes.
   *
   * @param list  Byte values to copy into the new buffer.
   */
  Bytes(const std::initializer_list<uint8_t>& list) noexcept;

  /**
   * @brief Constructs an owned buffer from a @c std::vector<uint8_t> by deep copy.
   *
   * @param data  Source vector; its contents are copied verbatim.
   */
  explicit Bytes(const std::vector<uint8_t>& data) noexcept;

  /**
   * @brief Destructor; releases owned heap storage and ignores loaned / shallow buffers.
   */
  ~Bytes() noexcept;

  /**
   * @brief Copy assignment; converts any source into an owned deep copy of @p target.
   *
   * @details
   * Releases the current buffer first when this instance owns one.  Empty sources produce an
   * empty non-owning result.
   *
   * @param target  Source buffer.
   * @return Reference to @c *this.
   */
  Bytes& operator=(const Bytes& target) noexcept;

  /**
   * @brief Move assignment; releases the current buffer and adopts @p target's state.
   *
   * @param target  Source buffer left empty after the move.
   * @return Reference to @c *this.
   */
  Bytes& operator=(Bytes&& target) noexcept;

  /**
   * @brief Replaces the payload with a deep copy of @p data.
   *
   * @param data  Source vector.
   * @return Reference to @c *this.
   */
  Bytes& operator=(const std::vector<uint8_t>& data) noexcept;

  /**
   * @brief Byte-wise equality comparison with another @c Bytes.
   *
   * @param target  Right-hand operand.
   * @return @c true when sizes and payload bytes match exactly.
   */
  [[nodiscard]] bool operator==(const Bytes& target) const noexcept;

  /**
   * @brief Byte-wise inequality comparison with another @c Bytes.
   *
   * @param target  Right-hand operand.
   * @return @c true when either the sizes or the payload bytes differ.
   */
  [[nodiscard]] bool operator!=(const Bytes& target) const noexcept;

  /**
   * @brief Byte-wise equality comparison with a @c std::vector<uint8_t>.
   *
   * @param data  Right-hand operand.
   * @return @c true when sizes and bytes match exactly.
   */
  [[nodiscard]] bool operator==(const std::vector<uint8_t>& data) const noexcept;

  /**
   * @brief Byte-wise inequality comparison with a @c std::vector<uint8_t>.
   *
   * @param data  Right-hand operand.
   * @return @c true when either the sizes or the bytes differ.
   */
  [[nodiscard]] bool operator!=(const std::vector<uint8_t>& data) const noexcept;

  /**
   * @brief Mutable indexed access into the payload region.
   *
   * @details
   * Resolves to @c real_data()[offset() @c + @c index].  No bounds checking is performed; pass
   * indices in @c [0, size()).
   *
   * @param index  Zero-based logical offset within the payload.
   * @return Reference to the byte at @p index.
   */
  [[nodiscard]] uint8_t& operator[](size_t index) noexcept;

  /**
   * @brief Read-only indexed access into the payload region.
   *
   * @param index  Zero-based logical offset within the payload.
   * @return Const reference to the byte at @p index.
   */
  [[nodiscard]] const uint8_t& operator[](size_t index) const noexcept;

  /**
   * @brief Returns a mutable pointer to the start of the payload (post-offset).
   *
   * @return Pointer to the first payload byte, or @c nullptr when empty.
   */
  [[nodiscard]] uint8_t* data() noexcept;

  /**
   * @brief Returns a read-only pointer to the start of the payload (post-offset).
   *
   * @return Pointer to the first payload byte, or @c nullptr when empty.
   */
  [[nodiscard]] const uint8_t* data() const noexcept;

  /**
   * @brief Returns a mutable pointer to the very beginning of the backing buffer.
   *
   * @details
   * @c real_data() points at the prefix region; @c real_data() @c + @c offset() equals @c data().
   *
   * @return Pointer to the raw buffer origin, or @c nullptr when empty.
   */
  [[nodiscard]] uint8_t* real_data() noexcept;

  /**
   * @brief Returns a read-only pointer to the very beginning of the backing buffer.
   *
   * @return Pointer to the raw buffer origin, or @c nullptr when empty.
   */
  [[nodiscard]] const uint8_t* real_data() const noexcept;

  /**
   * @brief Returns the size of the payload region in bytes.
   *
   * @return Number of payload bytes (excluding the prefix offset).
   */
  [[nodiscard]] size_t size() const noexcept;

  /**
   * @brief Returns the size of the used backing region (payload plus prefix offset).
   *
   * @return @c size() @c + @c offset().
   */
  [[nodiscard]] size_t real_size() const noexcept;

  /**
   * @brief Returns the allocated capacity of the backing buffer.
   *
   * @details
   * SBO buffers report @c kStackSize; pool-allocated buffers report the rounded allocation size.
   *
   * @return Capacity in bytes; always @c >= @c real_size().
   */
  [[nodiscard]] size_t capacity() const noexcept;

  /**
   * @brief Returns the reserved header offset preceding the payload.
   *
   * @return Offset in bytes.
   */
  [[nodiscard]] uint8_t offset() const noexcept;

  /**
   * @brief Reports whether this instance owns and will free its storage.
   *
   * @return @c true for objects produced by @c create / @c deep_copy and surviving copy/move
   *         assignments that produced an owned deep copy.
   */
  [[nodiscard]] bool is_owner() const noexcept;

  /**
   * @brief Reports whether the buffer is an iceoryx loan that VLink must not free.
   *
   * @return @c true for objects produced by @c loan_internal.
   */
  [[nodiscard]] bool is_loaned() const noexcept;

  /**
   * @brief Reports whether the buffer is logically empty.
   *
   * @return @c true when @c data() is @c nullptr and @c size() is @c 0.
   */
  [[nodiscard]] bool empty() const noexcept;

  /**
   * @brief Returns a mutable iterator to the first payload byte.
   *
   * @return Pointer to the first payload byte, or @c nullptr when empty.
   */
  [[nodiscard]] uint8_t* begin() noexcept;

  /**
   * @brief Returns a read-only iterator to the first payload byte.
   *
   * @return Pointer to the first payload byte, or @c nullptr when empty.
   */
  [[nodiscard]] const uint8_t* begin() const noexcept;

  /**
   * @brief Returns a mutable iterator one past the last payload byte.
   *
   * @return End pointer, or @c nullptr when empty.
   */
  [[nodiscard]] uint8_t* end() noexcept;

  /**
   * @brief Returns a read-only iterator one past the last payload byte.
   *
   * @return End pointer, or @c nullptr when empty.
   */
  [[nodiscard]] const uint8_t* end() const noexcept;

  /**
   * @brief Returns a mutable iterator to the start of the raw backing region.
   *
   * @return Pointer equal to @c real_data().
   */
  [[nodiscard]] uint8_t* real_begin() noexcept;

  /**
   * @brief Returns a read-only iterator to the start of the raw backing region.
   *
   * @return Pointer equal to @c real_data().
   */
  [[nodiscard]] const uint8_t* real_begin() const noexcept;

  /**
   * @brief Returns a mutable iterator one past the prefix-plus-payload region.
   *
   * @return End pointer for the used backing region, or @c nullptr when empty.
   */
  [[nodiscard]] uint8_t* real_end() noexcept;

  /**
   * @brief Returns a read-only iterator one past the prefix-plus-payload region.
   *
   * @return End pointer for the used backing region, or @c nullptr when empty.
   */
  [[nodiscard]] const uint8_t* real_end() const noexcept;

  /**
   * @brief Reports whether this carrier merely wraps an opaque pointer.
   *
   * @details
   * A pointer-only wrapper satisfies @c data_ @c != @c nullptr, @c size_ @c == @c 0,
   * @c offset_ @c == @c 0 and @c is_owner_ @c == @c false.  Retrieve the underlying pointer via
   * @c to_ptr<T>().
   *
   * @return @c true when this is a pointer-only wrapper.
   */
  [[nodiscard]] bool is_ptr() const noexcept;

  /**
   * @brief Copies the payload region into a new @c std::vector<uint8_t>.
   *
   * @return Vector mirroring @c data()[0, size()).
   */
  [[nodiscard]] std::vector<uint8_t> to_raw_data() const noexcept;

  /**
   * @brief Materialises the payload region as a new @c std::string.
   *
   * @return Owning string with the payload bytes.
   */
  [[nodiscard]] std::string to_string() const noexcept;

  /**
   * @brief Returns a non-owning @c std::string_view into the payload region.
   *
   * @details
   * The view is valid until the next mutation of this @c Bytes instance or its destruction.
   *
   * @return View covering @c data()[0, size()).
   */
  [[nodiscard]] std::string_view to_string_view() const noexcept;

  /**
   * @brief Reinterprets the backing pointer as @c T*.
   *
   * @details
   * Equivalent to @c reinterpret_cast<T*>(real_data()).  Caller is responsible for alignment.
   *
   * @tparam T  Target pointee type.  Defaults to @c void.
   * @return Pointer to @c T, or @c nullptr when empty.
   */
  template <typename T = void>
  [[nodiscard]] T* to_ptr() const noexcept;

  /**
   * @brief Returns the SBO threshold below which payloads stay inline.
   *
   * @return @c kStackSize (@c 96).
   */
  [[nodiscard]] static constexpr uint8_t stack_size() noexcept;

  /**
   * @brief Returns @c true at compile time when the platform is little-endian.
   *
   * @return @c true on x86 / arm-le / Windows; @c false on big-endian targets.
   */
  [[nodiscard]] static constexpr bool is_little_endian() noexcept;

  /**
   * @brief Returns @c true at compile time when the platform is big-endian.
   *
   * @return Logical negation of @c is_little_endian().
   */
  [[nodiscard]] static constexpr bool is_big_endian() noexcept;

  /**
   * @brief Detects whether a raw byte buffer matches the VLink LZAV compression frame layout.
   *
   * @details
   * Validates the 4-byte header magic (@c 17 @c 49 @c B2 @c 6F), the 4-byte footer magic
   * (@c A7 @c 05 @c ED @c 71) and that the buffer is at least 13 bytes long.
   *
   * @param data  Pointer to the buffer to inspect.
   * @param size  Length of the buffer.
   * @return @c true when both magics match and the size precondition holds.
   */
  [[nodiscard]] static bool is_compress_data(const uint8_t* data, size_t size) noexcept;

  /**
   * @brief Compresses a payload using LZAV and wraps it in the VLink compression frame.
   *
   * @details
   * Emits the layout shown in the file-level diagram.  Inputs larger than @c 1 @c MiB
   * (@c kMaxCompressCacheSize) are rejected and produce an empty result.
   *
   * @param data       Source pointer.
   * @param size       Source length in bytes.
   * @param high_ratio @c true to use LZAV's high-compression preset; default @c false.
   * @return Compressed framed buffer, or an empty @c Bytes on failure.
   */
  [[nodiscard]] static Bytes compress_data(const uint8_t* data, size_t size, bool high_ratio = false) noexcept;

  /**
   * @brief Decompresses an LZAV-framed buffer back into its original payload.
   *
   * @details
   * Strips the header / size / footer fields and feeds the LZAV payload to @c lzav_decompress.
   * When @p check_valid is @c true the magics are verified up front; invalid magics yield an
   * empty result.  Stored original sizes of @c 0 or above @c 256 @c MiB are also rejected.
   *
   * @param data         Framed source pointer.
   * @param size         Framed source length in bytes.
   * @param check_valid  @c true to verify magics before decompressing.  Default: @c true.
   * @return Decompressed payload, or an empty @c Bytes on failure.
   */
  [[nodiscard]] static Bytes uncompress_data(const uint8_t* data, size_t size, bool check_valid = true) noexcept;

  /**
   * @brief Releases owned storage and resets all metadata to the empty state.
   *
   * @details
   * When @c is_owner() is @c true the backing buffer is returned to the pool; loaned and
   * shallow carriers are simply forgotten.  After the call @c empty() reports @c true.
   */
  void clear() noexcept;

  /**
   * @brief Truncates the logical payload size in place without reallocating.
   *
   * @details
   * Valid only for owned buffers.  @p size must be @c <= current @c size(); the backing
   * capacity is unchanged.
   *
   * @param size  New logical size in bytes.
   * @return @c true on success; @c false for non-owned buffers or oversized requests.
   */
  [[nodiscard]] bool shrink_to(size_t size) noexcept;

  /**
   * @brief Ensures the backing capacity is at least @p new_capacity bytes.
   *
   * @details
   * Valid only for owned buffers.  When the current capacity already satisfies the request the
   * call is a no-op; otherwise a fresh buffer is allocated and the existing payload copied.
   *
   * @param new_capacity  Minimum required capacity.
   * @return @c true on success; @c false for non-owned buffers or allocation failure.
   */
  [[nodiscard]] bool reserve(size_t new_capacity) noexcept;

  /**
   * @brief Resizes the logical payload region to @p size bytes.
   *
   * @details
   * Valid only for owned buffers.  When @p size exceeds the current capacity @c reserve is
   * invoked first.  Newly exposed bytes are uninitialised unless the build defines
   * @c VLINK_BYTES_MEM_RESET.
   *
   * @param size  New payload size in bytes.
   * @return @c true on success; @c false for non-owned buffers or reallocation failure.
   */
  [[nodiscard]] bool resize(size_t size) noexcept;

  /**
   * @brief Replaces this instance with a non-owning alias of @p bytes.
   *
   * @details
   * Releases any owned storage first.  Copies @p bytes's raw pointer, size and offset without
   * copying the payload.  The result is non-owning regardless of @p bytes's loaned tag; use
   * @c loan_internal directly to preserve loaned semantics.
   *
   * @param bytes  Source carrier to alias.
   * @return Reference to @c *this.
   */
  Bytes& shallow_copy(const Bytes& bytes) noexcept;

  /**
   * @brief Replaces this instance with an owned deep copy of @p bytes.
   *
   * @details
   * Releases any owned storage first, then allocates and populates a fresh buffer when
   * @p bytes carries data.  Empty sources leave this instance empty and non-owning.
   *
   * @param bytes  Source carrier to copy.
   * @return Reference to @c *this.
   */
  Bytes& deep_copy(const Bytes& bytes) noexcept;

  /**
   * @brief Converts an existing non-owning carrier into an owning one in place.
   *
   * @details
   * Owners are returned unchanged.  Empty carriers are cleared.  Otherwise a fresh buffer is
   * allocated and the current payload copied into it.
   *
   * @return Reference to @c *this.
   */
  Bytes& deep_copy_self() noexcept;

  /**
   * @brief Stream insertion operator; prints the payload as space-separated hex bytes.
   *
   * @param ostream  Target output stream.
   * @param target   Buffer to print.
   * @return Reference to @p ostream.
   */
  VLINK_EXPORT friend std::ostream& operator<<(std::ostream& ostream, const Bytes& target) noexcept;

 private:
  enum Type : uint8_t {
    kCreate = 0,
    kShallowCopy = 1,
    kDeepCopy = 2,
    kMove = 3,
  };

  Bytes(Type type, uint8_t* data, size_t size, uint8_t offset, bool loaned) noexcept;

  void process_type(Type type, uint8_t* data, size_t size, uint8_t offset, bool loaned, Bytes* tmp = nullptr) noexcept;

  static constexpr uint8_t kStackSize{96};
  alignas(std::max_align_t) uint8_t stack_data_[kStackSize]{0};
  bool is_owner_{false};
  bool is_loaned_{false};
  uint8_t offset_{0};
  uint8_t* data_{nullptr};
  size_t size_{0};
  size_t capacity_{0};
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline uint8_t& Bytes::operator[](size_t index) noexcept { return data_[offset_ + index]; }

inline const uint8_t& Bytes::operator[](size_t index) const noexcept { return data_[offset_ + index]; }

inline uint8_t* Bytes::data() noexcept { return data_ ? (data_ + offset_) : nullptr; }

inline const uint8_t* Bytes::data() const noexcept { return data_ ? (data_ + offset_) : nullptr; }

inline uint8_t* Bytes::real_data() noexcept { return data_; }

inline const uint8_t* Bytes::real_data() const noexcept { return data_; }

inline size_t Bytes::size() const noexcept { return size_; }

inline size_t Bytes::real_size() const noexcept { return size_ + offset_; }

inline size_t Bytes::capacity() const noexcept { return capacity_; }

inline uint8_t Bytes::offset() const noexcept { return offset_; }

inline bool Bytes::is_owner() const noexcept { return is_owner_; }

inline bool Bytes::is_loaned() const noexcept { return is_loaned_; }

inline bool Bytes::empty() const noexcept { return data_ == nullptr && size_ == 0; }

inline uint8_t* Bytes::begin() noexcept { return data_ ? (data_ + offset_) : nullptr; }

inline const uint8_t* Bytes::begin() const noexcept { return data_ ? (data_ + offset_) : nullptr; }

inline uint8_t* Bytes::end() noexcept { return data_ ? (data_ + offset_ + size_) : nullptr; }

inline const uint8_t* Bytes::end() const noexcept { return data_ ? (data_ + offset_ + size_) : nullptr; }

inline uint8_t* Bytes::real_begin() noexcept { return data_; }

inline const uint8_t* Bytes::real_begin() const noexcept { return data_; }

inline uint8_t* Bytes::real_end() noexcept { return data_ ? (data_ + offset_ + size_) : nullptr; }

inline const uint8_t* Bytes::real_end() const noexcept { return data_ ? (data_ + offset_ + size_) : nullptr; }

inline bool Bytes::is_ptr() const noexcept { return data_ != nullptr && size_ == 0 && offset_ == 0 && !is_owner_; }

template <typename T>
inline T* Bytes::to_ptr() const noexcept {
  return reinterpret_cast<T*>(data_);
}

inline constexpr uint8_t Bytes::stack_size() noexcept { return kStackSize; }

inline constexpr bool Bytes::is_little_endian() noexcept {
#if defined(_WIN32) || defined(__LITTLE_ENDIAN__) || \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
  return true;
#elif defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
  return false;
#else
  return true;
#endif
}

inline constexpr bool Bytes::is_big_endian() noexcept { return !is_little_endian(); }

}  // namespace vlink
