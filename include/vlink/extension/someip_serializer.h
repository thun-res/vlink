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
 * @file someip_serializer.h
 * @brief SOME/IP payload codec helpers and template field traversal.
 *
 * @details
 * Implements the AUTOSAR R25-11 payload deployment selected by
 * @c VLINK_SOMEIP_FIELDS: configurable payload byte order, alignment from the
 * SOME/IP message start, field and structure length widths, UTF-8 and UTF-16
 * strings framed by a BOM and null terminator, associative maps, unions with
 * a type selector, and TLV structures with optional members and
 * self-describing unknown-member skipping.  Length fields, TLV tags, and
 * union type selectors keep network byte order.  The SOME/IP message header
 * is outside this codec and limits a payload to @c UINT32_MAX-8 bytes.
 *
 * Non-template cursor and primitive operations are implemented once in
 * @c src/extension/someip_serializer.cc; only container traversal and structure
 * expansion remain templates.  Failures are reported through boolean return
 * values, and unsupported field types are rejected at compile time.  Mixed
 * or opaque field byte order and C++ bit-fields are not supported; deployment
 * maximum-length constraints may be declared by the corresponding field
 * wrappers; the wire format details are documented in
 * @c doc/03-serialization.md.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "../base/bytes.h"
#include "../base/macros.h"
#include "../base/traits.h"

namespace vlink {

/**
 * @namespace vlink::SomeipSerializer
 * @brief AUTOSAR SOME/IP payload codec for macro-declared structures.
 */
namespace SomeipSerializer {  // NOLINT(readability-identifier-naming)

/**
 * @brief Maximum payload size representable by a SOME/IP message.
 *
 * @details
 * The 32-bit SOME/IP Length field includes the eight bytes from Request ID
 * through Return Code, leaving this many bytes for the payload.
 */
static constexpr size_t kMaxPayloadSize = std::numeric_limits<uint32_t>::max() - 8U;

/**
 * @enum Endian
 * @brief Byte order used for SOME/IP payload scalar values.
 */
enum class Endian : uint8_t {
  kBig = 0,
  kLittle = 1,
};

/**
 * @class Writer
 * @brief Bounds-checked SOME/IP payload writer.
 *
 * @details
 * A null destination performs a size-only pass.  A non-null destination never
 * grows or replaces storage, which allows exact transport loans to be written
 * in place.  Every successful append advances @c position() by the encoded
 * byte count; a failed append leaves the cursor unchanged.  The writer never
 * advances beyond @c kMaxPayloadSize.
 */
class VLINK_EXPORT Writer final {
 public:
  /**
   * @brief Creates a writer over @p data with @p capacity bytes.
   *
   * @details
   * Passing @c nullptr selects size-only mode.  Otherwise @p data must remain
   * valid for @p capacity bytes throughout the writer lifetime.  Values above
   * @c kMaxPayloadSize are clamped to that wire-format limit.  The writer does
   * not own or initialize the destination storage.
   *
   * @param data       Destination buffer, or @c nullptr for size-only mode.
   * @param capacity   Number of writable bytes, or the maximum size to measure.
   * @param alignment  AUTOSAR payload alignment: 1, 2, 4, 8, 16, or 32 bytes.
   * @param endian     Byte order for payload scalar values.
   */
  Writer(uint8_t* data, size_t capacity, size_t alignment = 1U, Endian endian = Endian::kBig) noexcept;

  /**
   * @brief Returns the number of bytes consumed by this writer.
   *
   * @return Current zero-based write position in bytes.
   */
  [[nodiscard]] size_t position() const noexcept;

  /**
   * @brief Reports whether this writer only measures encoded size.
   *
   * @return @c true when no destination buffer is attached.
   */
  [[nodiscard]] bool is_size_only() const noexcept;

  /**
   * @brief Creates a size-only cursor at the current position.
   *
   * @return A writer preserving capacity, position, alignment, and byte order
   *         without a destination buffer.
   */
  [[nodiscard]] Writer size_only() const noexcept;

  /**
   * @brief Appends @p size bytes without alignment padding.
   *
   * @details
   * Size-only mode advances the cursor without reading @p data.  In write mode,
   * @p data must identify at least @p size readable bytes when @p size is not
   * zero and must not overlap the destination range written by this operation.
   *
   * @param data  Source bytes.
   * @param size  Number of bytes to append.
   * @return @c true on success; @c false when the destination is too small.
   */
  [[nodiscard]] bool append(const uint8_t* data, size_t size) noexcept;

  /**
   * @brief Appends the low @p width bytes of @p value in the configured byte order.
   *
   * @param value  Unsigned value containing the bits to encode.
   * @param width  Encoded width in bytes; must be in the range @c [1,8].
   * @return @c true on success; @c false for an invalid width or insufficient storage.
   */
  [[nodiscard]] bool append_unsigned(uint64_t value, size_t width) noexcept;

  /**
   * @brief Appends the low @p width bytes of @p value in an explicit byte order.
   *
   * @details
   * Wire-format metadata such as TLV tags and union type selectors keeps
   * network byte order independently of the configured payload byte order.
   *
   * @param value   Unsigned value containing the bits to encode.
   * @param width   Encoded width in bytes; must be in the range @c [1,8].
   * @param endian  Byte order for this value only.
   * @return @c true on success; @c false for an invalid width or insufficient storage.
   */
  [[nodiscard]] bool append_unsigned(uint64_t value, size_t width, Endian endian) noexcept;

  /**
   * @brief Reserves a length field and records its payload start.
   *
   * @param length_position  Output offset of the reserved length field.
   * @param data_position    Output offset of the first length-delimited byte.
   * @param width            Encoded width in bytes; must be 1, 2, or 4.
   * @return @c true on success; @c false when the field cannot be reserved.
   */
  [[nodiscard]] bool begin_length_delimited(size_t& length_position, size_t& data_position,
                                            size_t width = sizeof(uint32_t)) noexcept;

  /**
   * @brief Finalizes a reserved length field from the current position.
   *
   * @details
   * Stores @c position()-@p data_position as an unsigned byte count at
   * @p length_position.  The current position is not changed.
   *
   * @param length_position  Offset returned by @c begin_length_delimited().
   * @param data_position    Payload start returned by @c begin_length_delimited().
   * @param width            Encoded width in bytes; must be 1, 2, or 4.
   * @return @c true on success; @c false on an invalid range or length overflow.
   */
  [[nodiscard]] bool end_length_delimited(size_t length_position, size_t data_position,
                                          size_t width = sizeof(uint32_t)) noexcept;

  /**
   * @brief Pads the current position to the configured AUTOSAR alignment.
   *
   * @return @c true on success; @c false when the destination is too small.
   */
  [[nodiscard]] bool align() noexcept;

 private:
  uint8_t* data_{nullptr};
  size_t capacity_{0};
  size_t position_{0};
  size_t alignment_{1U};
  Endian endian_{Endian::kBig};
};

/**
 * @class Reader
 * @brief Bounds-checked SOME/IP payload reader.
 *
 * @details
 * Every operation accepts an enclosing end position, so nested arrays cannot
 * consume bytes belonging to their parent structure.  Successful operations
 * advance the cursor.  The cursor state after a failed compound field is not
 * specified; a top-level failure may leave already decoded destination fields
 * updated.
 */
class VLINK_EXPORT Reader final {
 public:
  /**
   * @brief Creates a reader over an immutable @p size-byte payload.
   *
   * @details
   * The reader does not own the source storage.  When @p size is non-zero,
   * @p data must remain valid throughout the reader lifetime.
   *
   * @param data       Source payload.
   * @param size       Total source size in bytes.
   * @param alignment  AUTOSAR payload alignment: 1, 2, 4, 8, 16, or 32 bytes.
   * @param endian     Byte order for payload scalar values.
   */
  Reader(const uint8_t* data, size_t size, size_t alignment = 1U, Endian endian = Endian::kBig) noexcept;

  /**
   * @brief Returns the current read position.
   *
   * @return Current zero-based read position in bytes.
   */
  [[nodiscard]] size_t position() const noexcept;

  /**
   * @brief Returns the total payload size.
   *
   * @return Source payload size supplied to the constructor.
   */
  [[nodiscard]] size_t size() const noexcept;

  /**
   * @brief Returns the current byte pointer, or @c nullptr at the end.
   *
   * @return Pointer to the unread byte at @c position(), or @c nullptr when no bytes remain.
   */
  [[nodiscard]] const uint8_t* current_data() const noexcept;

  /**
   * @brief Reads @p size bytes without exceeding @p end.
   *
   * @details
   * The destination range must not overlap the source range read by this
   * operation.
   *
   * @param data  Destination buffer; must hold @p size bytes when non-zero.
   * @param size  Number of bytes to copy.
   * @param end   Exclusive enclosing boundary, not greater than @c size().
   * @return @c true on success; @c false when the requested range is invalid.
   */
  [[nodiscard]] bool read(uint8_t* data, size_t size, size_t end) noexcept;

  /**
   * @brief Advances by @p size bytes without exceeding @p end.
   *
   * @param size  Number of bytes to skip.
   * @param end   Exclusive enclosing boundary, not greater than @c size().
   * @return @c true on success; @c false when the requested range is invalid.
   */
  [[nodiscard]] bool skip(size_t size, size_t end) noexcept;

  /**
   * @brief Reads a @p width-byte unsigned integer in the configured byte order.
   *
   * @param value  Output value; modified only after the range is validated.
   * @param width  Encoded width in bytes; must be in the range @c [1,8].
   * @param end    Exclusive enclosing boundary, not greater than @c size().
   * @return @c true on success; @c false for an invalid width or range.
   */
  [[nodiscard]] bool read_unsigned(uint64_t& value, size_t width, size_t end) noexcept;

  /**
   * @brief Reads a @p width-byte unsigned integer in an explicit byte order.
   *
   * @details
   * Wire-format metadata such as TLV tags and union type selectors keeps
   * network byte order independently of the configured payload byte order.
   *
   * @param value   Output value; modified only after the range is validated.
   * @param width   Encoded width in bytes; must be in the range @c [1,8].
   * @param endian  Byte order for this value only.
   * @param end     Exclusive enclosing boundary, not greater than @c size().
   * @return @c true on success; @c false for an invalid width or range.
   */
  [[nodiscard]] bool read_unsigned(uint64_t& value, size_t width, Endian endian, size_t end) noexcept;

  /**
   * @brief Reads a length field and returns its validated end position.
   *
   * @details
   * The prefix is a big-endian byte count.  On success, @p value_end is the
   * exclusive end of the declared body and the reader points to its first byte.
   *
   * @param end        Exclusive enclosing boundary.
   * @param value_end  Output exclusive boundary of the length-delimited body.
   * @param width      Encoded width in bytes; must be 1, 2, or 4.
   * @return @c true on success; @c false when the prefix or declared body exceeds @p end.
   */
  [[nodiscard]] bool begin_length_delimited(size_t end, size_t& value_end, size_t width = sizeof(uint32_t)) noexcept;

  /**
   * @brief Skips bytes up to the configured AUTOSAR alignment.
   *
   * @param end  Exclusive enclosing boundary.
   * @return @c true on success; @c false when the padding exceeds @p end.
   */
  [[nodiscard]] bool align(size_t end) noexcept;

 private:
  const uint8_t* data_{nullptr};
  size_t size_{0};
  size_t position_{0};
  size_t alignment_{1U};
  Endian endian_{Endian::kBig};
};

/**
 * @brief Encodes an enum or macro-declared nested SOME/IP structure.
 *
 * @details
 * Enums must use an unsigned fixed-width underlying type.  Nested structures
 * are expanded in the exact field order supplied to
 * @c VLINK_SOMEIP_FIELDS.
 *
 * @tparam T  Enum or macro-declared SOME/IP structure type.
 * @param writer  Destination cursor.
 * @param value   Field value to encode.
 * @return @c true on success; @c false when a nested field cannot be encoded.
 */
template <typename T>
inline bool write_value(Writer& writer, const T& value) noexcept;

/**
 * @brief Encodes a dynamic array with a configurable byte-length prefix.
 *
 * @details
 * The prefix counts encoded bytes, not elements.  The @c vector<bool>
 * specialization is traversed by value to avoid proxy-reference dispatch.
 * An element that encodes to zero bytes is rejected because its count cannot
 * be represented by the byte-length prefix.
 *
 * @tparam T           Element type supported by @c write_value().
 * @tparam AllocatorT  Vector allocator type.
 * @tparam InnerLenT   Optional length widths for the inner array dimensions.
 * @tparam MaxT        Maximum encoded element count.
 * @param writer       Destination cursor.
 * @param value        Array elements to encode in order.
 * @param len          Length field width in bytes; must be 1, 2, or 4.
 * @return @c true on success; @c false on overflow or element encoding failure.
 */
template <size_t MaxT = std::numeric_limits<size_t>::max(), typename T, typename AllocatorT, size_t... InnerLenT>
inline bool write_value(Writer& writer, const std::vector<T, AllocatorT>& value, size_t len = sizeof(uint32_t),
                        std::index_sequence<InnerLenT...> = {}) noexcept;

/**
 * @brief Encodes a fixed-size array with an optional byte-length prefix.
 *
 * @tparam T          Element type supported by @c write_value().
 * @tparam SizeT      Compile-time element count.
 * @tparam InnerLenT  Optional length widths for the inner array dimensions.
 * @param writer  Destination cursor.
 * @param value   Array elements to encode in order.
 * @param len     Length field width in bytes, or zero to omit it.
 * @return @c true on success; @c false on overflow or element encoding failure.
 */
template <typename T, size_t SizeT, size_t... InnerLenT>
inline bool write_value(Writer& writer, const std::array<T, SizeT>& value, size_t len = sizeof(uint32_t),
                        std::index_sequence<InnerLenT...> = {}) noexcept;

/**
 * @brief Encodes an associative map with a configurable byte-length prefix.
 *
 * @details
 * The prefix counts encoded bytes.  Every entry is encoded as its key
 * followed immediately by its mapped value, without padding inside or between
 * entries; nested containers inside an entry use the default deployment.
 * Iteration order of an @c std::unordered_map selects the encoded entry order.
 *
 * @tparam KeyT        Key type supported by @c write_value().
 * @tparam ValueT      Mapped type supported by @c write_value().
 * @param writer       Destination cursor.
 * @param value        Map entries to encode.
 * @param len          Length field width in bytes; must be 1, 2, or 4.
 * @return @c true on success; @c false on overflow or entry encoding failure.
 * @{
 */
template <typename KeyT, typename ValueT, typename CompareT, typename AllocatorT>
inline bool write_value(Writer& writer, const std::map<KeyT, ValueT, CompareT, AllocatorT>& value,
                        size_t len = sizeof(uint32_t)) noexcept;

template <typename KeyT, typename ValueT, typename HashT, typename EqualT, typename AllocatorT>
inline bool write_value(Writer& writer, const std::unordered_map<KeyT, ValueT, HashT, EqualT, AllocatorT>& value,
                        size_t len = sizeof(uint32_t)) noexcept;
/**
 * @}
 */

/**
 * @brief Encodes a union with a byte length and a type selector.
 *
 * @details
 * The wire format is the data byte length, the type selector, and the encoded
 * alternative.  The length includes alternative padding and excludes the
 * selector.  Selector values identify
 * alternatives in declaration order from one.  When the first alternative is
 * @c std::monostate, it represents the empty union with selector zero.
 *
 * @tparam AlternativeT  Alternatives supported by @c write_value().
 * @param writer      Destination cursor.
 * @param value       Union value to encode.
 * @param len         Length field width in bytes; must be 1, 2, or 4.
 * @param type_width  Type selector width in bytes; must be 1, 2, or 4.
 * @return @c true on success; @c false on overflow or alternative encoding failure.
 */
template <typename... AlternativeT>
inline bool write_value(Writer& writer, const std::variant<AlternativeT...>& value, size_t len = sizeof(uint32_t),
                        size_t type_width = sizeof(uint32_t)) noexcept;

/**
 * @brief Decodes an enum or macro-declared nested SOME/IP structure.
 *
 * @details
 * Enums must use an unsigned fixed-width underlying type.  Nested structure
 * fields are decoded in the order supplied to @c VLINK_SOMEIP_FIELDS.
 *
 * @tparam T  Enum or macro-declared SOME/IP structure type.
 * @param reader  Source cursor.
 * @param value   Destination field.
 * @param end     Exclusive boundary of the enclosing field or payload.
 * @return @c true on success; @c false when a nested field cannot be decoded.
 */
template <typename T>
inline bool read_value(Reader& reader, T& value, size_t end) noexcept;

/**
 * @brief Decodes a byte-length-delimited dynamic array.
 *
 * @details
 * Existing elements are decoded in place so their internal capacity can be reused; new
 * elements are appended only as needed.  On success, the vector is resized to the decoded
 * element count.  A failure may leave updated elements and untouched old tail elements.
 *
 * @tparam T           Default-constructible element type supported by @c read_value().
 * @tparam AllocatorT  Vector allocator type.
 * @tparam InnerLenT   Optional length widths for the inner array dimensions.
 * @tparam MaxT        Maximum decoded element count; remaining bytes are skipped.
 * @param reader       Source cursor.
 * @param value        Destination vector whose existing elements are reused when possible.
 * @param end          Exclusive boundary of the enclosing field or payload.
 * @param len          Length field width in bytes; must be 1, 2, or 4.
 * @return @c true on success; @c false for an invalid length or element encoding.
 */
template <size_t MaxT = std::numeric_limits<size_t>::max(), typename T, typename AllocatorT, size_t... InnerLenT>
inline bool read_value(Reader& reader, std::vector<T, AllocatorT>& value, size_t end, size_t len = sizeof(uint32_t),
                       std::index_sequence<InnerLenT...> = {}) noexcept;

/**
 * @brief Decodes a byte-length-delimited fixed-size array.
 *
 * @details
 * Exactly @c SizeT elements are decoded.  Remaining bytes in a longer remote
 * representation are skipped as required for compatible fixed-array growth;
 * truncated content is rejected.
 *
 * @tparam T          Element type supported by @c read_value().
 * @tparam SizeT      Compile-time element count.
 * @tparam InnerLenT  Optional length widths for the inner array dimensions.
 * @param reader  Source cursor.
 * @param value   Destination array.
 * @param end     Exclusive boundary of the enclosing field or payload.
 * @param len     Length field width in bytes, or zero when omitted.
 * @return @c true on success; @c false when the length cannot contain all elements.
 */
template <typename T, size_t SizeT, size_t... InnerLenT>
inline bool read_value(Reader& reader, std::array<T, SizeT>& value, size_t end, size_t len = sizeof(uint32_t),
                       std::index_sequence<InnerLenT...> = {}) noexcept;

/**
 * @brief Decodes a byte-length-delimited associative map.
 *
 * @details
 * The map is cleared and rebuilt from the encoded entries.  A duplicate key
 * is rejected as malformed input.
 *
 * @tparam KeyT        Default-constructible key type supported by @c read_value().
 * @tparam ValueT      Default-constructible mapped type supported by @c read_value().
 * @param reader       Source cursor.
 * @param value        Destination map.
 * @param end          Exclusive boundary of the enclosing field or payload.
 * @param len          Length field width in bytes; must be 1, 2, or 4.
 * @return @c true on success; @c false for an invalid length or entry encoding.
 * @{
 */
template <typename KeyT, typename ValueT, typename CompareT, typename AllocatorT>
inline bool read_value(Reader& reader, std::map<KeyT, ValueT, CompareT, AllocatorT>& value, size_t end,
                       size_t len = sizeof(uint32_t)) noexcept;

template <typename KeyT, typename ValueT, typename HashT, typename EqualT, typename AllocatorT>
inline bool read_value(Reader& reader, std::unordered_map<KeyT, ValueT, HashT, EqualT, AllocatorT>& value, size_t end,
                       size_t len = sizeof(uint32_t)) noexcept;
/**
 * @}
 */

/**
 * @brief Decodes a union with a byte length and a type selector.
 *
 * @details
 * Selector zero selects a first @c std::monostate alternative when present.
 * Every other selector must identify a declared alternative, which is decoded
 * in place of the previous value.  Declared
 * bytes past the decoded alternative are skipped for compatible peers that
 * pad the union body.
 *
 * @tparam AlternativeT  Default-constructible alternatives supported by @c read_value().
 * @param reader      Source cursor.
 * @param value       Destination union.
 * @param end         Exclusive boundary of the enclosing field or payload.
 * @param len         Length field width in bytes; must be 1, 2, or 4.
 * @param type_width  Type selector width in bytes; must be 1, 2, or 4.
 * @return @c true on success; @c false for an unknown selector or malformed body.
 */
template <typename... AlternativeT>
inline bool read_value(Reader& reader, std::variant<AlternativeT...>& value, size_t end, size_t len = sizeof(uint32_t),
                       size_t type_width = sizeof(uint32_t)) noexcept;

/**
 * @brief Returns the exact SOME/IP payload size for @p src.
 *
 * @details
 * Executes the normal encoder in size-only mode, so validation and 32-bit
 * length limits match @c serialize().  No destination buffer is allocated.
 *
 * @tparam T  Macro-declared SOME/IP structure type.
 * @param src  Source structure to measure.
 * @return Exact encoded byte count; @c 0 when validation or size calculation fails.
 */
template <typename T>
[[nodiscard]] inline size_t get_serialized_size(const T& src) noexcept;

/**
 * @brief Serializes @p src into @p des using the SOME/IP payload format.
 *
 * @details
 * A loaned destination must already have the exact encoded size and is never
 * replaced.  Sufficient owning capacity with a matching offset is reused;
 * otherwise exact-size storage is allocated after a size-only pass.
 * Destination storage must not overlap any storage reachable from @p src.
 *
 * @tparam T  Macro-declared SOME/IP structure type.
 * @param src  Source structure.
 * @param des  Destination @c Bytes buffer.
 * @return @c true on success; @c false on validation, sizing, or storage failure.
 */
template <typename T>
inline bool serialize(const T& src, Bytes& des) noexcept;

/**
 * @brief Serializes @p src with reserved bytes before the payload.
 *
 * @details
 * This overload is used by the generic serializer when a transport header
 * offset is requested.  The payload is written directly after the reserved
 * prefix without an intermediate payload allocation or deep copy.  The
 * destination storage must not overlap any storage reachable from @p src.
 *
 * @tparam T  Macro-declared SOME/IP structure type.
 * @param src     Source structure.
 * @param des     Destination @c Bytes buffer.
 * @param offset  Number of bytes reserved before the payload.
 * @return @c true on success; @c false on validation, sizing, or storage failure.
 */
template <typename T>
inline bool serialize(const T& src, Bytes& des, uint8_t offset) noexcept;

/**
 * @brief Deserializes a complete SOME/IP payload from @p src into @p des.
 *
 * @details
 * Fields are decoded directly into @p des so existing string and vector
 * capacity can be reused.  Remaining top-level bytes are ignored for
 * compatible extension at the end of a standard structure.  A parse failure
 * may leave fields decoded before the failure updated.  Source storage must
 * not overlap any storage reachable from @p des.
 *
 * @tparam T  Macro-declared SOME/IP structure type.
 * @param src  Complete source payload.
 * @param des  Destination structure.
 * @return @c true on success; @c false for malformed input.
 */
template <typename T>
inline bool deserialize(const Bytes& src, T& des) noexcept;

/**
 * @brief Reports whether @p T encodes with a runtime-variable byte length.
 *
 * @tparam T  Field type as listed in @c VLINK_SOMEIP_FIELDS.
 * @return @c true when the encoded size depends on the field value.
 */
template <typename T>
[[nodiscard]] constexpr bool is_variable_size() noexcept;

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

namespace detail {

VLINK_EXPORT bool write_value(Writer& writer, bool value) noexcept;
VLINK_EXPORT bool write_value(Writer& writer, uint8_t value) noexcept;
VLINK_EXPORT bool write_value(Writer& writer, uint16_t value) noexcept;
VLINK_EXPORT bool write_value(Writer& writer, uint32_t value) noexcept;
VLINK_EXPORT bool write_value(Writer& writer, uint64_t value) noexcept;
VLINK_EXPORT bool write_value(Writer& writer, int8_t value) noexcept;
VLINK_EXPORT bool write_value(Writer& writer, int16_t value) noexcept;
VLINK_EXPORT bool write_value(Writer& writer, int32_t value) noexcept;
VLINK_EXPORT bool write_value(Writer& writer, int64_t value) noexcept;
VLINK_EXPORT bool write_value(Writer& writer, float value) noexcept;
VLINK_EXPORT bool write_value(Writer& writer, double value) noexcept;
VLINK_EXPORT bool write_value(Writer& writer, const std::string& value, size_t len = sizeof(uint32_t),
                              size_t maximum = std::numeric_limits<size_t>::max()) noexcept;
VLINK_EXPORT bool write_value(Writer& writer, const Bytes& value, size_t len = sizeof(uint32_t),
                              size_t maximum = std::numeric_limits<size_t>::max()) noexcept;
VLINK_EXPORT bool write_value(Writer& writer, const std::u16string& value, size_t len = sizeof(uint32_t),
                              Endian encoding = Endian::kBig,
                              size_t maximum = std::numeric_limits<size_t>::max()) noexcept;
VLINK_EXPORT bool write_fixed_value(Writer& writer, const std::string& value, size_t size, size_t len,
                                    size_t maximum = std::numeric_limits<size_t>::max()) noexcept;
VLINK_EXPORT bool write_fixed_value(Writer& writer, const std::u16string& value, size_t size, size_t len,
                                    Endian encoding, size_t maximum = std::numeric_limits<size_t>::max()) noexcept;
VLINK_EXPORT bool read_value(Reader& reader, bool& value, size_t end) noexcept;
VLINK_EXPORT bool read_value(Reader& reader, uint8_t& value, size_t end) noexcept;
VLINK_EXPORT bool read_value(Reader& reader, uint16_t& value, size_t end) noexcept;
VLINK_EXPORT bool read_value(Reader& reader, uint32_t& value, size_t end) noexcept;
VLINK_EXPORT bool read_value(Reader& reader, uint64_t& value, size_t end) noexcept;
VLINK_EXPORT bool read_value(Reader& reader, int8_t& value, size_t end) noexcept;
VLINK_EXPORT bool read_value(Reader& reader, int16_t& value, size_t end) noexcept;
VLINK_EXPORT bool read_value(Reader& reader, int32_t& value, size_t end) noexcept;
VLINK_EXPORT bool read_value(Reader& reader, int64_t& value, size_t end) noexcept;
VLINK_EXPORT bool read_value(Reader& reader, float& value, size_t end) noexcept;
VLINK_EXPORT bool read_value(Reader& reader, double& value, size_t end) noexcept;
VLINK_EXPORT bool read_value(Reader& reader, std::string& value, size_t end, size_t len = sizeof(uint32_t),
                             size_t maximum = std::numeric_limits<size_t>::max()) noexcept;
VLINK_EXPORT bool read_value(Reader& reader, Bytes& value, size_t end, size_t len = sizeof(uint32_t),
                             size_t maximum = std::numeric_limits<size_t>::max()) noexcept;
VLINK_EXPORT bool read_value(Reader& reader, std::u16string& value, size_t end, size_t len = sizeof(uint32_t),
                             Endian encoding = Endian::kBig,
                             size_t maximum = std::numeric_limits<size_t>::max()) noexcept;
VLINK_EXPORT bool read_fixed_value(Reader& reader, std::string& value, size_t end, size_t size, size_t len,
                                   size_t maximum = std::numeric_limits<size_t>::max()) noexcept;
VLINK_EXPORT bool read_fixed_value(Reader& reader, std::u16string& value, size_t end, size_t size, size_t len,
                                   Endian encoding, size_t maximum = std::numeric_limits<size_t>::max()) noexcept;
VLINK_EXPORT bool read_value_body(Reader& reader, std::string& value, size_t end,
                                  size_t maximum = std::numeric_limits<size_t>::max()) noexcept;
VLINK_EXPORT bool read_value_body(Reader& reader, std::u16string& value, size_t end, Endian encoding = Endian::kBig,
                                  size_t maximum = std::numeric_limits<size_t>::max()) noexcept;
VLINK_EXPORT bool read_value_body(Reader& reader, Bytes& value, size_t end,
                                  size_t maximum = std::numeric_limits<size_t>::max()) noexcept;
VLINK_EXPORT bool read_tag(Reader& reader, uint8_t& wire_type, uint16_t& data_id, size_t end) noexcept;
VLINK_EXPORT bool read_union_frame(Reader& reader, size_t len, size_t type_width, size_t end, uint64_t& selector,
                                   size_t& data_end) noexcept;
VLINK_EXPORT bool write_value_body(Writer& writer, const std::string& value,
                                   size_t maximum = std::numeric_limits<size_t>::max()) noexcept;
VLINK_EXPORT bool write_value_body(Writer& writer, const std::u16string& value, Endian encoding = Endian::kBig,
                                   size_t maximum = std::numeric_limits<size_t>::max()) noexcept;
VLINK_EXPORT bool write_value_body(Writer& writer, const Bytes& value,
                                   size_t maximum = std::numeric_limits<size_t>::max()) noexcept;
VLINK_EXPORT bool write_tag(Writer& writer, uint8_t wire_type, uint16_t data_id) noexcept;
VLINK_EXPORT bool write_union_frame(Writer& writer, uint64_t selector, size_t len, size_t type_width,
                                    size_t& length_position, size_t& content_position) noexcept;
VLINK_EXPORT bool begin_tlv_body(Reader& reader, uint8_t wire_type, size_t configured_len, size_t end,
                                 size_t& body_end) noexcept;
VLINK_EXPORT bool begin_tlv_complex(Writer& writer, uint16_t data_id, size_t len, bool dynamic, size_t& length_position,
                                    size_t& data_position) noexcept;
VLINK_EXPORT bool skip_tlv_value(Reader& reader, uint8_t wire_type, size_t configured_len, size_t end) noexcept;
VLINK_EXPORT void report_check_available_failure() noexcept;
VLINK_EXPORT bool prepare_destination(Bytes& des, size_t size, uint8_t offset) noexcept;

}  // namespace detail

template <typename T>
inline bool read_value_body(Reader& reader, T& value, size_t end) noexcept;

template <typename T>
inline bool write_value_body(Writer& writer, const T& value) noexcept;

template <typename T>
[[nodiscard]] constexpr size_t get_alignment() noexcept {
  if constexpr (VLINK_HAS_MEMBER(T, vlink_someip_alignment())) {
    constexpr size_t kAlignment = T::vlink_someip_alignment();

    static_assert(kAlignment == 1U || kAlignment == 2U || kAlignment == 4U || kAlignment == 8U || kAlignment == 16U ||
                      kAlignment == 32U,
                  "SOME/IP alignment must be 1, 2, 4, 8, 16, or 32 bytes.");
    return kAlignment;
  }

  return 1U;
}

template <typename T>
[[nodiscard]] constexpr Endian get_endian() noexcept {
  if constexpr (VLINK_HAS_MEMBER(T, vlink_someip_endian())) {
    return T::vlink_someip_endian();
  }

  return Endian::kBig;
}

template <typename T>
[[nodiscard]] constexpr size_t get_struct_length() noexcept {
  if constexpr (VLINK_HAS_MEMBER(T, vlink_someip_struct_length())) {
    constexpr size_t kLength = T::vlink_someip_struct_length();

    static_assert(kLength == 0U || kLength == 1U || kLength == 2U || kLength == 4U,
                  "SOME/IP structure length field width must be 0, 1, 2, or 4 bytes.");
    return kLength;
  }

  return 0U;
}

template <typename T>
[[nodiscard]] constexpr bool is_map(const T*) noexcept {
  return false;
}

template <typename KeyT, typename ValueT, typename CompareT, typename AllocatorT>
[[nodiscard]] constexpr bool is_map(const std::map<KeyT, ValueT, CompareT, AllocatorT>*) noexcept {
  return true;
}

template <typename KeyT, typename ValueT, typename HashT, typename EqualT, typename AllocatorT>
[[nodiscard]] constexpr bool is_map(const std::unordered_map<KeyT, ValueT, HashT, EqualT, AllocatorT>*) noexcept {
  return true;
}

template <typename T>
[[nodiscard]] constexpr bool is_vector(const T*) noexcept {
  return false;
}

template <typename T, typename AllocatorT>
[[nodiscard]] constexpr bool is_vector(const std::vector<T, AllocatorT>*) noexcept {
  return true;
}

template <typename T>
[[nodiscard]] constexpr bool is_variant(const T*) noexcept {
  return false;
}

template <typename... AlternativeT>
[[nodiscard]] constexpr bool is_variant(const std::variant<AlternativeT...>*) noexcept {
  return true;
}

template <typename T>
[[nodiscard]] constexpr bool is_optional(const T*) noexcept {
  return false;
}

template <typename T>
[[nodiscard]] constexpr bool is_optional(const std::optional<T>*) noexcept {
  return true;
}

template <typename T>
[[nodiscard]] constexpr bool supports_maximum(const T*) noexcept {
  using ValueType = std::remove_cv_t<T>;

  return std::is_same_v<ValueType, std::string> || std::is_same_v<ValueType, std::u16string> ||
         std::is_same_v<ValueType, Bytes> || is_vector(static_cast<const ValueType*>(nullptr));
}

template <typename T>
[[nodiscard]] constexpr bool supports_maximum(const std::optional<T>*) noexcept {
  return supports_maximum(static_cast<const T*>(nullptr));
}

static constexpr uint8_t kComplexWireType = 0xFFU;

template <typename T>
[[nodiscard]] constexpr uint8_t get_fixed_wire_type() noexcept {
  using ValueType = std::remove_cv_t<T>;

  if constexpr (std::is_enum_v<ValueType>) {
    return get_fixed_wire_type<std::underlying_type_t<ValueType>>();
  } else if constexpr (std::is_arithmetic_v<ValueType>) {
    if constexpr (sizeof(ValueType) == sizeof(uint8_t)) {
      return 0U;
    } else if constexpr (sizeof(ValueType) == sizeof(uint16_t)) {
      return 1U;
    } else if constexpr (sizeof(ValueType) == sizeof(uint32_t)) {
      return 2U;
    } else {
      return 3U;
    }
  } else {
    return kComplexWireType;
  }
}

template <typename... AlternativeT>
[[nodiscard]] constexpr bool validate_union_alternatives() noexcept {
  constexpr size_t kNullCount = (static_cast<size_t>(std::is_same_v<AlternativeT, std::monostate>) + ... + 0U);

  static_assert(kNullCount <= 1U, "SOME/IP unions accept at most one std::monostate alternative.");
  static_assert(
      kNullCount == 0U || std::is_same_v<std::tuple_element_t<0U, std::tuple<AlternativeT...>>, std::monostate>,
      "SOME/IP union std::monostate must be the first alternative for selector zero.");
  static_assert(kNullCount == 0U || sizeof...(AlternativeT) > 1U,
                "SOME/IP unions require at least one non-null alternative.");

  return true;
}

template <typename FirstT, typename... AlternativeT>
[[nodiscard]] constexpr bool union_alternatives_have_equal_size(const std::variant<FirstT, AlternativeT...>*) noexcept {
  return (std::is_arithmetic_v<FirstT> || std::is_enum_v<FirstT>) &&
         ((std::is_arithmetic_v<AlternativeT> || std::is_enum_v<AlternativeT>) && ...) &&
         ((sizeof(FirstT) == sizeof(AlternativeT)) && ...);
}

template <typename... AlternativeT>
[[nodiscard]] constexpr bool has_null_union_alternative() noexcept {
  return std::is_same_v<std::tuple_element_t<0U, std::tuple<AlternativeT...>>, std::monostate>;
}

template <size_t LenT, typename T>
[[nodiscard]] constexpr bool supports_length_field(const T*) noexcept {
  return LenT > 0U && (std::is_same_v<T, std::string> || std::is_same_v<T, std::u16string> ||
                       std::is_same_v<T, Bytes> || is_map(static_cast<const T*>(nullptr)));
}

template <size_t LenT, typename T, typename AllocatorT>
[[nodiscard]] constexpr bool supports_length_field(const std::vector<T, AllocatorT>*) noexcept {
  return LenT > 0U;
}

template <size_t LenT, typename T, size_t SizeT>
[[nodiscard]] constexpr bool supports_length_field(const std::array<T, SizeT>*) noexcept {
  return true;
}

template <size_t LenT, typename T, size_t MaxT = std::numeric_limits<size_t>::max()>
struct LengthField final {
  static_assert(MaxT == std::numeric_limits<size_t>::max() || supports_maximum(static_cast<const T*>(nullptr)),
                "SOME/IP maximum applies only to string, u16string, Bytes, and vector fields.");

  T& value;
};

template <typename T, size_t LenT, size_t... InnerLenT>
struct ArrayLengthField final {
  using ValueType = std::remove_cv_t<T>;

  static_assert((LenT == 0U || LenT == 1U || LenT == 2U || LenT == 4U) &&
                    ((InnerLenT == 0U || InnerLenT == 1U || InnerLenT == 2U || InnerLenT == 4U) && ...),
                "SOME/IP array length field width must be 0, 1, 2, or 4 bytes.");
  static_assert(!std::is_same_v<ValueType, std::string> && !std::is_same_v<ValueType, std::u16string> &&
                    !std::is_same_v<ValueType, Bytes> && !is_map(static_cast<const ValueType*>(nullptr)) &&
                    supports_length_field<LenT>(static_cast<const ValueType*>(nullptr)),
                "VLINK_SOMEIP_ARRAY_LENGTH supports vector widths 1/2/4 and array widths 0/1/2/4.");

  T& value;
};

template <size_t LenT, Endian EncodingT, typename T, size_t MaxT = std::numeric_limits<size_t>::max()>
struct Utf16Field final {
  static_assert(LenT == 1U || LenT == 2U || LenT == 4U, "SOME/IP UTF-16 length field width must be 1, 2, or 4 bytes.");
  static_assert(std::is_same_v<std::remove_cv_t<T>, std::u16string>,
                "VLINK_SOMEIP_UTF16 requires a std::u16string field.");

  T& value;
};

template <size_t SizeT, size_t LenT, Endian EncodingT, typename T, size_t MaxT = std::numeric_limits<size_t>::max()>
struct FixedStringField final {
  using ValueType = std::remove_cv_t<T>;

  static_assert(LenT == 0U || LenT == 1U || LenT == 2U || LenT == 4U,
                "SOME/IP fixed string length field width must be 0, 1, 2, or 4 bytes.");
  static_assert(std::is_same_v<ValueType, std::string> || std::is_same_v<ValueType, std::u16string>,
                "VLINK_SOMEIP_FIXED_STRING requires a std::string or std::u16string field.");
  static_assert(SizeT >= 4U && (std::is_same_v<ValueType, std::string> || SizeT % 2U == 0U),
                "SOME/IP fixed strings must hold a BOM and terminator; UTF-16 sizes must be even.");
  static_assert(LenT == 0U || SizeT <= ((uint64_t{1U} << (LenT * 8U)) - 1U),
                "SOME/IP fixed string size does not fit its length field.");

  T& value;
};

template <size_t LenT, size_t TypeT, typename T>
struct UnionField final {
  static_assert(LenT == 0U || LenT == 1U || LenT == 2U || LenT == 4U,
                "SOME/IP union length field width must be 0, 1, 2, or 4 bytes.");
  static_assert(TypeT == 1U || TypeT == 2U || TypeT == 4U,
                "SOME/IP union type selector width must be 1, 2, or 4 bytes.");
  static_assert(is_variant(static_cast<const std::remove_cv_t<T>*>(nullptr)),
                "VLINK_SOMEIP_UNION requires a std::variant field.");
  static_assert(LenT != 0U || union_alternatives_have_equal_size(static_cast<const std::remove_cv_t<T>*>(nullptr)),
                "SOME/IP unions without a length field require equal-size arithmetic or enum alternatives.");

  T& value;
};

template <uint16_t DataIdT, size_t LenT, bool DynamicT, typename T, size_t MaxT = std::numeric_limits<size_t>::max()>
struct TlvField final {
  static constexpr uint16_t kDataId = DataIdT;
  static constexpr size_t kLengthWidth = LenT;

  static_assert(DataIdT <= 0x0FFFU, "SOME/IP TLV data IDs must fit the 12-bit tag field.");
  static_assert(LenT == 1U || LenT == 2U || LenT == 4U, "SOME/IP TLV length field width must be 1, 2, or 4 bytes.");
  static_assert(MaxT == std::numeric_limits<size_t>::max() || supports_maximum(static_cast<const T*>(nullptr)),
                "SOME/IP maximum applies only to string, u16string, Bytes, and vector fields.");

  T& value;
};

template <uint16_t DataIdT, size_t LenT, bool DynamicT, size_t SizeT, Endian EncodingT, typename T,
          size_t MaxT = std::numeric_limits<size_t>::max()>
struct TlvFixedStringField final {
  using ValueType = std::remove_cv_t<T>;
  static constexpr bool kUtf16 =
      std::is_same_v<ValueType, std::u16string> || std::is_same_v<ValueType, std::optional<std::u16string>>;

  static constexpr uint16_t kDataId = DataIdT;
  static constexpr size_t kLengthWidth = LenT;

  static_assert(DataIdT <= 0x0FFFU, "SOME/IP TLV data IDs must fit the 12-bit tag field.");
  static_assert(LenT == 1U || LenT == 2U || LenT == 4U, "SOME/IP TLV length field width must be 1, 2, or 4 bytes.");
  static_assert(std::is_same_v<ValueType, std::string> || std::is_same_v<ValueType, std::u16string> ||
                    std::is_same_v<ValueType, std::optional<std::string>> ||
                    std::is_same_v<ValueType, std::optional<std::u16string>>,
                "VLINK_SOMEIP_TLV_FIXED_STRING requires a string or optional string field.");
  static_assert(SizeT >= 4U && (!kUtf16 || SizeT % 2U == 0U),
                "SOME/IP fixed strings must hold a BOM and terminator; UTF-16 sizes must be even.");
  static_assert(DynamicT || SizeT <= ((uint64_t{1U} << (LenT * 8U)) - 1U),
                "SOME/IP static TLV fixed string size does not fit its length field.");

  T& value;
};

template <uint16_t DataIdT, size_t LenT, bool DynamicT, typename T, size_t... InnerLenT>
struct TlvArrayField final {
  static constexpr uint16_t kDataId = DataIdT;
  static constexpr size_t kLengthWidth = LenT;

  static_assert(DataIdT <= 0x0FFFU, "SOME/IP TLV data IDs must fit the 12-bit tag field.");
  static_assert(LenT == 1U || LenT == 2U || LenT == 4U, "SOME/IP TLV length field width must be 1, 2, or 4 bytes.");
  static_assert(((InnerLenT == LenT) && ...), "SOME/IP TLV array dimensions must use the structure length width.");

  T& value;
};

template <typename... FieldT>
[[nodiscard]] inline auto make_fields(FieldT&&... field) noexcept {
  return std::tuple<FieldT...>(std::forward<FieldT>(field)...);
}

template <size_t LenT, size_t MaxT = std::numeric_limits<size_t>::max(), typename T>
[[nodiscard]] inline auto length_field(T& value) noexcept {
  using ValueType = std::remove_cv_t<T>;

  static_assert(LenT == 0U || LenT == 1U || LenT == 2U || LenT == 4U,
                "SOME/IP length field width must be 0, 1, 2, or 4 bytes.");
  static_assert(supports_length_field<LenT>(static_cast<const ValueType*>(nullptr)),
                "VLINK_SOMEIP_LENGTH supports string, u16string, Bytes, vector, and map widths 1/2/4 or array "
                "widths 0/1/2/4.");

  return LengthField<LenT, T, MaxT>{value};
}

template <size_t LenT, size_t... InnerLenT, typename T>
[[nodiscard]] inline auto array_length_field(T& value) noexcept {
  return ArrayLengthField<T, LenT, InnerLenT...>{value};
}

template <size_t LenT, Endian EncodingT, size_t MaxT = std::numeric_limits<size_t>::max(), typename T>
[[nodiscard]] inline auto utf16_field(T& value) noexcept {
  return Utf16Field<LenT, EncodingT, T, MaxT>{value};
}

template <size_t SizeT, size_t LenT, Endian EncodingT = Endian::kBig, size_t MaxT = std::numeric_limits<size_t>::max(),
          typename T>
[[nodiscard]] inline auto fixed_string_field(T& value) noexcept {
  return FixedStringField<SizeT, LenT, EncodingT, T, MaxT>{value};
}

template <size_t LenT, size_t TypeT, typename T>
[[nodiscard]] inline auto union_field(T& value) noexcept {
  return UnionField<LenT, TypeT, T>{value};
}

template <uint16_t DataIdT, size_t LenT = sizeof(uint32_t), bool DynamicT = true,
          size_t MaxT = std::numeric_limits<size_t>::max(), typename T>
[[nodiscard]] inline auto tlv_field(T& value) noexcept {
  return TlvField<DataIdT, LenT, DynamicT, T, MaxT>{value};
}

template <uint16_t DataIdT, size_t LenT, bool DynamicT, size_t SizeT, Endian EncodingT,
          size_t MaxT = std::numeric_limits<size_t>::max(), typename T>
[[nodiscard]] inline auto tlv_fixed_string_field(T& value) noexcept {
  return TlvFixedStringField<DataIdT, LenT, DynamicT, SizeT, EncodingT, T, MaxT>{value};
}

template <uint16_t DataIdT, size_t LenT, bool DynamicT, size_t... InnerLenT, typename T>
[[nodiscard]] inline auto tlv_array_field(T& value) noexcept {
  return TlvArrayField<DataIdT, LenT, DynamicT, T, InnerLenT...>{value};
}

template <typename T>
[[nodiscard]] constexpr bool is_tlv_field(const T*) noexcept {
  return false;
}

template <uint16_t DataIdT, size_t LenT, bool DynamicT, typename T, size_t MaxT>
[[nodiscard]] constexpr bool is_tlv_field(const TlvField<DataIdT, LenT, DynamicT, T, MaxT>*) noexcept {
  return true;
}

template <uint16_t DataIdT, size_t LenT, bool DynamicT, size_t SizeT, Endian EncodingT, typename T, size_t MaxT>
[[nodiscard]] constexpr bool is_tlv_field(
    const TlvFixedStringField<DataIdT, LenT, DynamicT, SizeT, EncodingT, T, MaxT>*) noexcept {
  return true;
}

template <uint16_t DataIdT, size_t LenT, bool DynamicT, typename T, size_t... InnerLenT>
[[nodiscard]] constexpr bool is_tlv_field(const TlvArrayField<DataIdT, LenT, DynamicT, T, InnerLenT...>*) noexcept {
  return true;
}

template <typename TupleT, size_t... IndexT>
[[nodiscard]] constexpr size_t count_tlv_fields(std::index_sequence<IndexT...>) noexcept {
  return (static_cast<size_t>(is_tlv_field(
              static_cast<const std::remove_reference_t<std::tuple_element_t<IndexT, TupleT>>*>(nullptr))) +
          ... + 0U);
}

template <typename TupleT, size_t... IndexT>
[[nodiscard]] constexpr bool tlv_data_ids_are_unique(std::index_sequence<IndexT...>) noexcept {
  constexpr size_t kFieldCount = sizeof...(IndexT);
  const uint16_t ids[kFieldCount] = {std::remove_reference_t<std::tuple_element_t<IndexT, TupleT>>::kDataId...};

  for (size_t left = 0U; left < kFieldCount; ++left) {
    for (size_t right = left + 1U; right < kFieldCount; ++right) {
      if (ids[left] == ids[right]) {
        return false;
      }
    }
  }

  return true;
}

template <size_t LenT, typename TupleT, size_t... IndexT>
[[nodiscard]] constexpr bool tlv_length_widths_are_equal(std::index_sequence<IndexT...>) noexcept {
  return ((std::remove_reference_t<std::tuple_element_t<IndexT, TupleT>>::kLengthWidth == LenT) && ...);
}

template <typename TupleT, size_t... IndexT>
[[nodiscard]] constexpr bool fields_are_variable(std::index_sequence<IndexT...>) noexcept {
  return (is_variable_size<std::remove_reference_t<std::tuple_element_t<IndexT, TupleT>>>() || ...);
}

template <typename T, typename AllocatorT>
[[nodiscard]] constexpr bool is_variable_size(const std::vector<T, AllocatorT>*) noexcept {
  return true;
}

template <typename T, size_t SizeT>
[[nodiscard]] constexpr bool is_variable_size(const std::array<T, SizeT>*) noexcept {
  return is_variable_size<T>();
}

template <size_t LenT, typename T, size_t MaxT>
[[nodiscard]] constexpr bool is_variable_size(const LengthField<LenT, T, MaxT>*) noexcept {
  return is_variable_size<T>();
}

template <typename T, size_t LenT, size_t... InnerLenT>
[[nodiscard]] constexpr bool is_variable_size(const ArrayLengthField<T, LenT, InnerLenT...>*) noexcept {
  return is_variable_size<T>();
}

template <typename KeyT, typename ValueT, typename CompareT, typename AllocatorT>
[[nodiscard]] constexpr bool is_variable_size(const std::map<KeyT, ValueT, CompareT, AllocatorT>*) noexcept {
  return true;
}

template <typename KeyT, typename ValueT, typename HashT, typename EqualT, typename AllocatorT>
[[nodiscard]] constexpr bool is_variable_size(
    const std::unordered_map<KeyT, ValueT, HashT, EqualT, AllocatorT>*) noexcept {
  return true;
}

template <typename... AlternativeT>
[[nodiscard]] constexpr bool is_variable_size(const std::variant<AlternativeT...>*) noexcept {
  return true;
}

template <size_t LenT, Endian EncodingT, typename T, size_t MaxT>
[[nodiscard]] constexpr bool is_variable_size(const Utf16Field<LenT, EncodingT, T, MaxT>*) noexcept {
  return true;
}

template <size_t SizeT, size_t LenT, Endian EncodingT, typename T, size_t MaxT>
[[nodiscard]] constexpr bool is_variable_size(const FixedStringField<SizeT, LenT, EncodingT, T, MaxT>*) noexcept {
  return false;
}

template <size_t LenT, size_t TypeT, typename T>
[[nodiscard]] constexpr bool is_variable_size(const UnionField<LenT, TypeT, T>*) noexcept {
  return LenT != 0U;
}

template <uint16_t DataIdT, size_t LenT, bool DynamicT, typename T, size_t MaxT>
[[nodiscard]] constexpr bool is_variable_size(const TlvField<DataIdT, LenT, DynamicT, T, MaxT>*) noexcept {
  return true;
}

template <uint16_t DataIdT, size_t LenT, bool DynamicT, size_t SizeT, Endian EncodingT, typename T, size_t MaxT>
[[nodiscard]] constexpr bool is_variable_size(
    const TlvFixedStringField<DataIdT, LenT, DynamicT, SizeT, EncodingT, T, MaxT>*) noexcept {
  return true;
}

template <typename T>
[[nodiscard]] constexpr bool is_variable_size(const T*) noexcept {
  if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::u16string> || std::is_same_v<T, Bytes>) {
    return true;
  } else if constexpr (VLINK_HAS_MEMBER(T, is_vlink_someip_type())) {
    using FieldsType = decltype(std::declval<T&>().get_vlink_someip_fields());

    return fields_are_variable<FieldsType>(std::make_index_sequence<std::tuple_size_v<FieldsType>>{});
  }

  return false;
}

template <typename T>
[[nodiscard]] constexpr bool is_variable_size() noexcept {
  using ValueType = std::remove_cv_t<std::remove_reference_t<T>>;

  return is_variable_size(static_cast<const ValueType*>(nullptr));
}

template <typename T>
inline bool write_value(Writer& writer, const T& value) noexcept {
  using ValueType = std::remove_cv_t<T>;

  if constexpr (std::is_enum_v<ValueType>) {
    using UnderlyingType = std::underlying_type_t<ValueType>;

    static_assert(std::is_unsigned_v<UnderlyingType> && !std::is_same_v<UnderlyingType, bool>,
                  "SOME/IP enums must use uint8_t, uint16_t, uint32_t, or uint64_t storage.");
    return detail::write_value(writer, static_cast<UnderlyingType>(value));
  } else if constexpr (std::is_arithmetic_v<ValueType> || std::is_same_v<ValueType, std::string> ||
                       std::is_same_v<ValueType, Bytes>) {
    return detail::write_value(writer, value);
  } else if constexpr (std::is_same_v<ValueType, std::u16string>) {
    return detail::write_value(writer, value, sizeof(uint32_t), Endian::kBig);
  } else if constexpr (VLINK_HAS_MEMBER(ValueType, is_vlink_someip_type())) {
    static constexpr size_t kStructLength = get_struct_length<ValueType>();

    if constexpr (VLINK_HAS_MEMBER(ValueType, check_available())) {
      static_assert(noexcept(value.check_available()), "SOME/IP check_available() must be noexcept.");
      if VUNLIKELY (!value.check_available()) {
        detail::report_check_available_failure();

        return false;
      }
    }

    size_t length_position = 0U;
    size_t data_position = 0U;

    if constexpr (kStructLength > 0U) {
      if VUNLIKELY (!writer.begin_length_delimited(length_position, data_position, kStructLength)) {
        return false;
      }
    }

    if VUNLIKELY (!write_value_body(writer, value)) {
      return false;
    }

    return kStructLength == 0U || writer.end_length_delimited(length_position, data_position, kStructLength);
  } else {
    static_assert(Traits::ExpectFalse<T>(), "Unsupported SOME/IP field type.");

    return false;
  }
}

template <size_t MaxT, typename T, typename AllocatorT, size_t... InnerLenT>
inline bool write_value(Writer& writer, const std::vector<T, AllocatorT>& value, size_t len,
                        std::index_sequence<InnerLenT...>) noexcept {
  if VUNLIKELY (value.size() > MaxT) {
    return false;
  }

  size_t length_position = 0U;
  size_t data_position = 0U;

  return writer.begin_length_delimited(length_position, data_position, len) &&
         write_value_body(writer, value, std::index_sequence<InnerLenT...>{}) &&
         writer.end_length_delimited(length_position, data_position, len);
}

template <typename T, size_t SizeT, size_t... InnerLenT>
inline bool write_value(Writer& writer, const std::array<T, SizeT>& value, size_t len,
                        std::index_sequence<InnerLenT...>) noexcept {
  size_t length_position = 0U;
  size_t data_position = 0U;

  if VUNLIKELY (len > 0U && !writer.begin_length_delimited(length_position, data_position, len)) {
    return false;
  }

  if VUNLIKELY (!write_value_body(writer, value, std::index_sequence<InnerLenT...>{})) {
    return false;
  }

  return len == 0U || writer.end_length_delimited(length_position, data_position, len);
}

template <typename KeyT, typename ValueT, typename CompareT, typename AllocatorT>
inline bool write_value(Writer& writer, const std::map<KeyT, ValueT, CompareT, AllocatorT>& value,
                        size_t len) noexcept {
  size_t length_position = 0U;
  size_t data_position = 0U;

  return writer.begin_length_delimited(length_position, data_position, len) && write_map_entries(writer, value) &&
         writer.end_length_delimited(length_position, data_position, len);
}

template <typename KeyT, typename ValueT, typename HashT, typename EqualT, typename AllocatorT>
inline bool write_value(Writer& writer, const std::unordered_map<KeyT, ValueT, HashT, EqualT, AllocatorT>& value,
                        size_t len) noexcept {
  size_t length_position = 0U;
  size_t data_position = 0U;

  return writer.begin_length_delimited(length_position, data_position, len) && write_map_entries(writer, value) &&
         writer.end_length_delimited(length_position, data_position, len);
}

template <typename... AlternativeT>
inline bool write_value(Writer& writer, const std::variant<AlternativeT...>& value, size_t len,
                        size_t type_width) noexcept {
  static_assert(validate_union_alternatives<AlternativeT...>(), "SOME/IP union alternatives are invalid.");

  if VUNLIKELY (value.valueless_by_exception()) {
    return false;
  }

  static constexpr bool kHasNull = has_null_union_alternative<AlternativeT...>();
  const uint64_t selector = value.index() + (kHasNull ? 0U : 1U);

  size_t length_position = 0U;
  size_t content_position = 0U;

  if (len == 0U) {
    return writer.append_unsigned(selector, type_width, Endian::kBig) &&
           write_union_alternative(writer, value, std::make_index_sequence<sizeof...(AlternativeT)>{});
  }

  if VUNLIKELY (!detail::write_union_frame(writer, selector, len, type_width, length_position, content_position) ||
                !write_union_alternative(writer, value, std::make_index_sequence<sizeof...(AlternativeT)>{})) {
    return false;
  }

  if constexpr (kHasNull) {
    if (value.index() == 0U) {
      return writer.end_length_delimited(length_position, content_position, len);
    }
  }

  return writer.align() && writer.end_length_delimited(length_position, content_position, len);
}

template <typename T>
inline bool read_value(Reader& reader, T& value, size_t end) noexcept {
  using ValueType = std::remove_cv_t<T>;

  if constexpr (std::is_enum_v<ValueType>) {
    using UnderlyingType = std::underlying_type_t<ValueType>;

    static_assert(std::is_unsigned_v<UnderlyingType> && !std::is_same_v<UnderlyingType, bool>,
                  "SOME/IP enums must use uint8_t, uint16_t, uint32_t, or uint64_t storage.");
    UnderlyingType raw = 0;

    if VUNLIKELY (!detail::read_value(reader, raw, end)) {
      return false;
    }

    value = static_cast<T>(raw);

    return true;
  } else if constexpr (std::is_arithmetic_v<ValueType> || std::is_same_v<ValueType, std::string> ||
                       std::is_same_v<ValueType, Bytes>) {
    return detail::read_value(reader, value, end);
  } else if constexpr (std::is_same_v<ValueType, std::u16string>) {
    return detail::read_value(reader, value, end, sizeof(uint32_t), Endian::kBig);
  } else if constexpr (VLINK_HAS_MEMBER(ValueType, is_vlink_someip_type())) {
    static constexpr size_t kStructLength = get_struct_length<ValueType>();

    size_t struct_end = end;

    if constexpr (kStructLength > 0U) {
      if VUNLIKELY (!reader.begin_length_delimited(end, struct_end, kStructLength)) {
        return false;
      }
    }

    if VUNLIKELY (!read_value_body(reader, value, struct_end)) {
      return false;
    }

    return kStructLength == 0U || reader.skip(struct_end - reader.position(), struct_end);
  } else {
    static_assert(Traits::ExpectFalse<T>(), "Unsupported SOME/IP field type.");

    return false;
  }
}

template <size_t MaxT, typename T, typename AllocatorT, size_t... InnerLenT>
inline bool read_value(Reader& reader, std::vector<T, AllocatorT>& value, size_t end, size_t len,
                       std::index_sequence<InnerLenT...>) noexcept {
  size_t array_end = 0U;

  return reader.begin_length_delimited(end, array_end, len) &&
         read_value_body<MaxT>(reader, value, array_end, std::index_sequence<InnerLenT...>{});
}

template <typename T, size_t SizeT, size_t... InnerLenT>
inline bool read_value(Reader& reader, std::array<T, SizeT>& value, size_t end, size_t len,
                       std::index_sequence<InnerLenT...>) noexcept {
  size_t array_end = end;

  if VUNLIKELY (len > 0U && !reader.begin_length_delimited(end, array_end, len)) {
    return false;
  }

  if VUNLIKELY (!read_value_body(reader, value, array_end, std::index_sequence<InnerLenT...>{})) {
    return false;
  }

  return len == 0U || reader.skip(array_end - reader.position(), array_end);
}

template <typename KeyT, typename ValueT, typename CompareT, typename AllocatorT>
inline bool read_value(Reader& reader, std::map<KeyT, ValueT, CompareT, AllocatorT>& value, size_t end,
                       size_t len) noexcept {
  size_t map_end = 0U;

  return reader.begin_length_delimited(end, map_end, len) && read_map_entries(reader, value, map_end);
}

template <typename KeyT, typename ValueT, typename HashT, typename EqualT, typename AllocatorT>
inline bool read_value(Reader& reader, std::unordered_map<KeyT, ValueT, HashT, EqualT, AllocatorT>& value, size_t end,
                       size_t len) noexcept {
  size_t map_end = 0U;

  return reader.begin_length_delimited(end, map_end, len) && read_map_entries(reader, value, map_end);
}

template <typename... AlternativeT>
inline bool read_value(Reader& reader, std::variant<AlternativeT...>& value, size_t end, size_t len,
                       size_t type_width) noexcept {
  static_assert(validate_union_alternatives<AlternativeT...>(), "SOME/IP union alternatives are invalid.");

  uint64_t selector = 0U;
  size_t data_end = 0U;

  if (len == 0U) {
    return reader.read_unsigned(selector, type_width, Endian::kBig, end) &&
           read_union_alternative(reader, value, selector, end, std::make_index_sequence<sizeof...(AlternativeT)>{});
  }

  if VUNLIKELY (!detail::read_union_frame(reader, len, type_width, end, selector, data_end) ||
                (selector == 0U && reader.position() != data_end) ||
                !read_union_alternative(reader, value, selector, data_end,
                                        std::make_index_sequence<sizeof...(AlternativeT)>{})) {
    return false;
  }

  return reader.skip(data_end - reader.position(), data_end);
}

template <typename T>
inline size_t get_serialized_size(const T& src) noexcept {
  Writer writer(nullptr, std::numeric_limits<size_t>::max(), get_alignment<T>(), get_endian<T>());

  return write_value(writer, src) ? writer.position() : 0U;
}

template <typename T>
inline bool serialize(const T& src, Bytes& des) noexcept {
  return serialize(src, des, 0U);
}

template <typename T>
inline bool serialize(const T& src, Bytes& des, uint8_t offset) noexcept {
  if (des.is_loaned()) {
    if VUNLIKELY (des.offset() != offset) {
      return false;
    }

    Writer writer(des.data(), des.size(), get_alignment<T>(), get_endian<T>());

    return write_value(writer, src) && writer.position() == des.size();
  }

  Writer size_writer(nullptr, std::numeric_limits<size_t>::max(), get_alignment<T>(), get_endian<T>());

  if VUNLIKELY (!write_value(size_writer, src) || !detail::prepare_destination(des, size_writer.position(), offset)) {
    return false;
  }

  Writer writer(des.data(), des.size(), get_alignment<T>(), get_endian<T>());

  return write_value(writer, src) && writer.position() == des.size();
}

template <typename T>
inline bool deserialize(const Bytes& src, T& des) noexcept {
  if VUNLIKELY (src.size() > kMaxPayloadSize) {
    return false;
  }

  Reader reader(src.data(), src.size(), get_alignment<T>(), get_endian<T>());

  return read_value(reader, des, reader.size());
}

template <size_t LenT, typename T, size_t MaxT>
inline bool read_value(Reader& reader, LengthField<LenT, T, MaxT>& field, size_t end) noexcept {
  using ValueType = std::remove_cv_t<T>;

  if constexpr (std::is_same_v<ValueType, std::string> || std::is_same_v<ValueType, Bytes>) {
    return detail::read_value(reader, field.value, end, LenT, MaxT);
  } else if constexpr (std::is_same_v<ValueType, std::u16string>) {
    return detail::read_value(reader, field.value, end, LenT, Endian::kBig, MaxT);
  } else if constexpr (is_vector(static_cast<const ValueType*>(nullptr))) {
    return read_value<MaxT>(reader, field.value, end, LenT, std::index_sequence<>{});
  } else if constexpr (is_map(static_cast<const ValueType*>(nullptr))) {
    return read_value(reader, field.value, end, LenT);
  } else {
    return read_value(reader, field.value, end, LenT, std::index_sequence<>{});
  }
}

template <typename T, size_t LenT, size_t... InnerLenT>
inline bool read_value(Reader& reader, ArrayLengthField<T, LenT, InnerLenT...>& field, size_t end) noexcept {
  return read_value(reader, field.value, end, LenT, std::index_sequence<InnerLenT...>{});
}

template <size_t LenT, Endian EncodingT, typename T, size_t MaxT>
inline bool read_value(Reader& reader, Utf16Field<LenT, EncodingT, T, MaxT>& field, size_t end) noexcept {
  return detail::read_value(reader, field.value, end, LenT, EncodingT, MaxT);
}

template <size_t SizeT, size_t LenT, Endian EncodingT, typename T, size_t MaxT>
inline bool read_value(Reader& reader, FixedStringField<SizeT, LenT, EncodingT, T, MaxT>& field, size_t end) noexcept {
  if constexpr (std::is_same_v<std::remove_cv_t<T>, std::string>) {
    return detail::read_fixed_value(reader, field.value, end, SizeT, LenT, MaxT);
  } else {
    return detail::read_fixed_value(reader, field.value, end, SizeT, LenT, EncodingT, MaxT);
  }
}

template <size_t SizeT, size_t LenT, Endian EncodingT, typename T, size_t MaxT>
inline bool read_value_body(Reader& reader, FixedStringField<SizeT, LenT, EncodingT, T, MaxT>& field,
                            size_t end) noexcept {
  if VUNLIKELY (reader.position() > end || end - reader.position() > SizeT) {
    return false;
  }

  const size_t size = end - reader.position();

  if constexpr (std::is_same_v<std::remove_cv_t<T>, std::string>) {
    return detail::read_fixed_value(reader, field.value, end, size, 0U, MaxT);
  } else {
    return detail::read_fixed_value(reader, field.value, end, size, 0U, EncodingT, MaxT);
  }
}

template <size_t LenT, size_t TypeT, typename T>
inline bool read_value(Reader& reader, UnionField<LenT, TypeT, T>& field, size_t end) noexcept {
  return read_value(reader, field.value, end, LenT, TypeT);
}

template <typename TupleT, size_t... IndexT>
inline bool read_fields(Reader& reader, TupleT& fields, size_t end, std::index_sequence<IndexT...>) noexcept {
  static constexpr size_t kFieldCount = std::tuple_size_v<TupleT>;

  return ([&reader, &fields, end]() {
    if VUNLIKELY (!read_value(reader, std::get<IndexT>(fields), end)) {
      return false;
    }

    using FieldType = std::remove_reference_t<std::tuple_element_t<IndexT, TupleT>>;

    return IndexT + 1U == kFieldCount || !is_variable_size<FieldType>() || reader.align(end);
  }() && ...);
}

template <size_t MaxT = std::numeric_limits<size_t>::max(), typename T, typename AllocatorT, size_t... InnerLenT>
inline bool read_value_body(Reader& reader, std::vector<T, AllocatorT>& value, size_t end,
                            std::index_sequence<InnerLenT...> = {}) noexcept {
  if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) {
    static constexpr size_t kElementSize = sizeof(T);

    const size_t byte_length = end - reader.position();
    const size_t element_count = byte_length / kElementSize;

    if VUNLIKELY (byte_length % kElementSize != 0U && element_count < MaxT) {
      return false;
    }

    value.reserve(element_count < MaxT ? element_count : MaxT);
  }

  size_t index = 0U;

  while (reader.position() < end && index < MaxT) {
    const size_t element_position = reader.position();

    if constexpr (sizeof...(InnerLenT) > 0U) {
      if (index == value.size()) {
        value.emplace_back();
      }

      ArrayLengthField<T, InnerLenT...> field{value[index]};

      if VUNLIKELY (!read_value(reader, field, end) || reader.position() == element_position) {
        return false;
      }
    } else if constexpr (std::is_same_v<T, bool>) {
      bool element = false;

      if VUNLIKELY (!read_value(reader, element, end) || reader.position() == element_position) {
        return false;
      }

      if (index < value.size()) {
        value[index] = element;
      } else {
        value.emplace_back(element);
      }
    } else {
      if (index == value.size()) {
        value.emplace_back();
      }

      if VUNLIKELY (!read_value(reader, value[index], end) || reader.position() == element_position) {
        return false;
      }
    }

    ++index;

    if constexpr (is_variable_size<T>()) {
      if VUNLIKELY (reader.position() < end && !reader.align(end)) {
        return false;
      }
    }
  }

  value.resize(index);

  return reader.skip(end - reader.position(), end);
}

template <typename T, size_t SizeT, size_t... InnerLenT>
inline bool read_value_body(Reader& reader, std::array<T, SizeT>& value, size_t end,
                            std::index_sequence<InnerLenT...> = {}) noexcept {
  for (size_t index = 0U; index < SizeT; ++index) {
    if constexpr (sizeof...(InnerLenT) > 0U) {
      ArrayLengthField<T, InnerLenT...> field{value[index]};

      if VUNLIKELY (!read_value(reader, field, end)) {
        return false;
      }
    } else {
      if VUNLIKELY (!read_value(reader, value[index], end)) {
        return false;
      }
    }

    if constexpr (is_variable_size<T>()) {
      if VUNLIKELY (index + 1U < SizeT && !reader.align(end)) {
        return false;
      }
    }
  }

  return true;
}

template <typename KeyT, typename ValueT, typename CompareT, typename AllocatorT>
inline bool read_value_body(Reader& reader, std::map<KeyT, ValueT, CompareT, AllocatorT>& value, size_t end) noexcept {
  return read_map_entries(reader, value, end);
}

template <typename KeyT, typename ValueT, typename HashT, typename EqualT, typename AllocatorT>
inline bool read_value_body(Reader& reader, std::unordered_map<KeyT, ValueT, HashT, EqualT, AllocatorT>& value,
                            size_t end) noexcept {
  return read_map_entries(reader, value, end);
}

template <typename T, size_t LenT, size_t... InnerLenT>
inline bool read_value_body(Reader& reader, ArrayLengthField<T, LenT, InnerLenT...>& field, size_t end) noexcept {
  return read_value_body(reader, field.value, end, std::index_sequence<InnerLenT...>{});
}

template <typename... AlternativeT>
inline bool read_value_body(Reader& reader, std::variant<AlternativeT...>& value, size_t end) noexcept {
  static_assert(validate_union_alternatives<AlternativeT...>(), "SOME/IP union alternatives are invalid.");

  uint64_t selector = 0U;

  if VUNLIKELY (!reader.read_unsigned(selector, sizeof(uint32_t), Endian::kBig, end)) {
    return false;
  }

  return (selector != 0U || reader.position() == end) &&
         read_union_alternative(reader, value, selector, end, std::make_index_sequence<sizeof...(AlternativeT)>{});
}

template <typename MapT>
inline bool read_map_entries(Reader& reader, MapT& value, size_t end) noexcept {
  value.clear();

  while (reader.position() < end) {
    const size_t entry_position = reader.position();

    typename MapT::key_type key{};

    if VUNLIKELY (!read_value(reader, key, end)) {
      return false;
    }

    typename MapT::mapped_type mapped{};

    if VUNLIKELY (!read_value(reader, mapped, end) || reader.position() == entry_position) {
      return false;
    }

    if VUNLIKELY (!value.emplace(std::move(key), std::move(mapped)).second) {
      return false;
    }
  }

  return true;
}

template <size_t IndexT, typename... AlternativeT>
inline bool read_union_element(Reader& reader, std::variant<AlternativeT...>& value, size_t data_end) noexcept {
  if constexpr (IndexT == 0U && has_null_union_alternative<AlternativeT...>()) {
    value.template emplace<IndexT>();
    return true;
  } else {
    return read_value(reader, value.template emplace<IndexT>(), data_end);
  }
}

template <typename... AlternativeT, size_t... IndexT>
inline bool read_union_alternative(Reader& reader, std::variant<AlternativeT...>& value, uint64_t selector,
                                   size_t data_end, std::index_sequence<IndexT...>) noexcept {
  static constexpr bool kHasNull = has_null_union_alternative<AlternativeT...>();

  return ((selector == IndexT + (kHasNull ? 0U : 1U) && read_union_element<IndexT>(reader, value, data_end)) || ...);
}

template <size_t LenT, size_t MaxT = std::numeric_limits<size_t>::max(), typename T>
inline bool read_tlv_value(Reader& reader, T& value, uint8_t wire_type, size_t end) noexcept {
  static constexpr uint8_t kWireType = get_fixed_wire_type<T>();

  if constexpr (kWireType != kComplexWireType) {
    if VUNLIKELY (wire_type != kWireType) {
      return false;
    }

    return read_value(reader, value, end);
  } else {
    size_t body_end = 0U;

    if VUNLIKELY (!detail::begin_tlv_body(reader, wire_type, LenT, end, body_end)) {
      return false;
    }

    bool body_ok = false;

    if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, Bytes>) {
      body_ok = detail::read_value_body(reader, value, body_end, MaxT);
    } else if constexpr (std::is_same_v<T, std::u16string>) {
      body_ok = detail::read_value_body(reader, value, body_end, Endian::kBig, MaxT);
    } else if constexpr (is_vector(static_cast<const T*>(nullptr))) {
      body_ok = read_value_body<MaxT>(reader, value, body_end);
    } else {
      body_ok = read_value_body(reader, value, body_end);
    }

    return body_ok && reader.skip(body_end - reader.position(), body_end);
  }
}

template <uint16_t DataIdT, size_t LenT, bool DynamicT, typename T, size_t MaxT>
inline bool read_tlv_field(Reader& reader, TlvField<DataIdT, LenT, DynamicT, T, MaxT>& field, uint8_t wire_type,
                           size_t end) noexcept {
  using ValueType = std::remove_cv_t<std::remove_reference_t<T>>;

  if constexpr (is_optional(static_cast<const ValueType*>(nullptr))) {
    if (!field.value.has_value()) {
      field.value.emplace();
    }

    return read_tlv_value<LenT, MaxT>(reader, *field.value, wire_type, end);
  } else {
    return read_tlv_value<LenT, MaxT>(reader, field.value, wire_type, end);
  }
}

template <uint16_t DataIdT, size_t LenT, bool DynamicT, size_t SizeT, Endian EncodingT, typename T, size_t MaxT>
inline bool read_tlv_field(Reader& reader,
                           TlvFixedStringField<DataIdT, LenT, DynamicT, SizeT, EncodingT, T, MaxT>& field,
                           uint8_t wire_type, size_t end) noexcept {
  using ValueType = std::remove_cv_t<T>;
  size_t body_end = 0U;

  if VUNLIKELY (!detail::begin_tlv_body(reader, wire_type, LenT, end, body_end) ||
                body_end - reader.position() > SizeT) {
    return false;
  }

  if constexpr (is_optional(static_cast<const ValueType*>(nullptr))) {
    if (!field.value.has_value()) {
      field.value.emplace();
    }

    auto fixed = fixed_string_field<SizeT, 0U, EncodingT, MaxT>(*field.value);
    return read_value_body(reader, fixed, body_end);
  } else {
    auto fixed = fixed_string_field<SizeT, 0U, EncodingT, MaxT>(field.value);
    return read_value_body(reader, fixed, body_end);
  }
}

template <uint16_t DataIdT, size_t LenT, bool DynamicT, typename T, size_t... InnerLenT>
inline bool read_tlv_field(Reader& reader, TlvArrayField<DataIdT, LenT, DynamicT, T, InnerLenT...>& field,
                           uint8_t wire_type, size_t end) noexcept {
  using ValueType = std::remove_cv_t<T>;

  if constexpr (is_optional(static_cast<const ValueType*>(nullptr))) {
    if (!field.value.has_value()) {
      field.value.emplace();
    }

    auto array = array_length_field<LenT, InnerLenT...>(*field.value);
    return read_tlv_value<LenT>(reader, array, wire_type, end);
  } else {
    auto array = array_length_field<LenT, InnerLenT...>(field.value);
    return read_tlv_value<LenT>(reader, array, wire_type, end);
  }
}

template <typename T>
inline bool finish_missing_tlv_value(T& value) noexcept {
  using ValueType = std::remove_cv_t<std::remove_reference_t<T>>;

  if constexpr (is_optional(static_cast<const ValueType*>(nullptr))) {
    value.reset();

    return true;
  } else {
    return false;
  }
}

template <size_t LenT, typename TupleT, size_t... IndexT>
inline bool read_tlv_fields(Reader& reader, TupleT& fields, size_t end, std::index_sequence<IndexT...>) noexcept {
  static constexpr size_t kFieldCount = sizeof...(IndexT);

  std::array<bool, kFieldCount> seen{};

  while (reader.position() < end) {
    uint8_t wire_type = 0U;
    uint16_t data_id = 0U;

    if VUNLIKELY (!detail::read_tag(reader, wire_type, data_id, end)) {
      return false;
    }

    bool field_ok = true;

    const bool matched = ([&]() {
      using FieldType = std::remove_reference_t<std::tuple_element_t<IndexT, TupleT>>;

      if (FieldType::kDataId != data_id) {
        return false;
      }
      if VUNLIKELY (seen[IndexT]) {
        field_ok = false;

        return true;
      }

      seen[IndexT] = true;
      field_ok = read_tlv_field(reader, std::get<IndexT>(fields), wire_type, end);

      return true;
    }() || ...);

    if (matched) {
      if VUNLIKELY (!field_ok) {
        return false;
      }
    } else if VUNLIKELY (!detail::skip_tlv_value(reader, wire_type, LenT, end)) {
      return false;
    }
  }

  bool required_ok = true;

  ((seen[IndexT] || (required_ok = finish_missing_tlv_value(std::get<IndexT>(fields).value) && required_ok)), ...);

  return required_ok;
}

template <typename T>
inline bool read_value_body(Reader& reader, T& value, size_t end) noexcept {
  if constexpr (VLINK_HAS_MEMBER(T, is_vlink_someip_type())) {
    using FieldsType = decltype(value.get_vlink_someip_fields());

    static_assert(std::tuple_size_v<FieldsType> > 0U, "SOME/IP structures must declare at least one field.");

    static constexpr size_t kFieldCount = std::tuple_size_v<FieldsType>;
    static constexpr size_t kTlvCount = count_tlv_fields<FieldsType>(std::make_index_sequence<kFieldCount>{});

    static_assert(kTlvCount == 0U || kTlvCount == kFieldCount,
                  "SOME/IP structures must tag either every field or no field with VLINK_SOMEIP_TLV.");
    static_assert(kTlvCount == 0U || get_struct_length<T>() > 0U,
                  "SOME/IP TLV structures must declare a non-zero VLINK_SOMEIP_STRUCT_LENGTH.");
    static_assert(kTlvCount == 0U || get_alignment<T>() == 1U, "SOME/IP TLV structures require 1-byte alignment.");

    auto fields = value.get_vlink_someip_fields();

    if constexpr (kTlvCount > 0U) {
      static_assert(tlv_data_ids_are_unique<FieldsType>(std::make_index_sequence<kFieldCount>{}),
                    "SOME/IP TLV data IDs must be unique within a structure.");
      static_assert(
          tlv_length_widths_are_equal<get_struct_length<T>(), FieldsType>(std::make_index_sequence<kFieldCount>{}),
          "SOME/IP TLV fields must use the structure length width.");

      return read_tlv_fields<get_struct_length<T>()>(reader, fields, end, std::make_index_sequence<kFieldCount>{});
    } else {
      return read_fields(reader, fields, end, std::make_index_sequence<kFieldCount>{});
    }
  } else {
    static_assert(Traits::ExpectFalse<T>(), "Unsupported SOME/IP TLV member type.");

    return false;
  }
}

template <size_t LenT, typename T, size_t MaxT>
inline bool write_value(Writer& writer, const LengthField<LenT, T, MaxT>& field) noexcept {
  using ValueType = std::remove_cv_t<T>;

  if constexpr (std::is_same_v<ValueType, std::string> || std::is_same_v<ValueType, Bytes>) {
    return detail::write_value(writer, field.value, LenT, MaxT);
  } else if constexpr (std::is_same_v<ValueType, std::u16string>) {
    return detail::write_value(writer, field.value, LenT, Endian::kBig, MaxT);
  } else if constexpr (is_vector(static_cast<const ValueType*>(nullptr))) {
    return write_value<MaxT>(writer, field.value, LenT, std::index_sequence<>{});
  } else if constexpr (is_map(static_cast<const ValueType*>(nullptr))) {
    return write_value(writer, field.value, LenT);
  } else {
    return write_value(writer, field.value, LenT, std::index_sequence<>{});
  }
}

template <typename T, size_t LenT, size_t... InnerLenT>
inline bool write_value(Writer& writer, const ArrayLengthField<T, LenT, InnerLenT...>& field) noexcept {
  return write_value(writer, field.value, LenT, std::index_sequence<InnerLenT...>{});
}

template <size_t LenT, Endian EncodingT, typename T, size_t MaxT>
inline bool write_value(Writer& writer, const Utf16Field<LenT, EncodingT, T, MaxT>& field) noexcept {
  return detail::write_value(writer, field.value, LenT, EncodingT, MaxT);
}

template <size_t SizeT, size_t LenT, Endian EncodingT, typename T, size_t MaxT>
inline bool write_value(Writer& writer, const FixedStringField<SizeT, LenT, EncodingT, T, MaxT>& field) noexcept {
  if constexpr (std::is_same_v<std::remove_cv_t<T>, std::string>) {
    return detail::write_fixed_value(writer, field.value, SizeT, LenT, MaxT);
  } else {
    return detail::write_fixed_value(writer, field.value, SizeT, LenT, EncodingT, MaxT);
  }
}

template <size_t SizeT, size_t LenT, Endian EncodingT, typename T, size_t MaxT>
inline bool write_value_body(Writer& writer, const FixedStringField<SizeT, LenT, EncodingT, T, MaxT>& field) noexcept {
  if constexpr (std::is_same_v<std::remove_cv_t<T>, std::string>) {
    return detail::write_fixed_value(writer, field.value, SizeT, 0U, MaxT);
  } else {
    return detail::write_fixed_value(writer, field.value, SizeT, 0U, EncodingT, MaxT);
  }
}

template <size_t LenT, size_t TypeT, typename T>
inline bool write_value(Writer& writer, const UnionField<LenT, TypeT, T>& field) noexcept {
  return write_value(writer, field.value, LenT, TypeT);
}

template <typename TupleT, size_t... IndexT>
inline bool write_fields(Writer& writer, const TupleT& fields, std::index_sequence<IndexT...>) noexcept {
  static constexpr size_t kFieldCount = std::tuple_size_v<TupleT>;

  return ([&writer, &fields]() noexcept {
    if VUNLIKELY (!write_value(writer, std::get<IndexT>(fields))) {
      return false;
    }

    using FieldType = std::remove_reference_t<std::tuple_element_t<IndexT, TupleT>>;

    return IndexT + 1U == kFieldCount || !is_variable_size<FieldType>() || writer.align();
  }() && ...);
}

template <typename T, typename AllocatorT, size_t... InnerLenT>
inline bool write_value_body(Writer& writer, const std::vector<T, AllocatorT>& value,
                             std::index_sequence<InnerLenT...> = {}) noexcept {
  for (size_t index = 0U; index < value.size(); ++index) {
    const size_t element_position = writer.position();

    if constexpr (sizeof...(InnerLenT) > 0U) {
      const ArrayLengthField<const T, InnerLenT...> field{value[index]};

      if VUNLIKELY (!write_value(writer, field)) {
        return false;
      }
    } else if constexpr (std::is_same_v<T, bool>) {
      if VUNLIKELY (!write_value(writer, static_cast<bool>(value[index]))) {
        return false;
      }
    } else if VUNLIKELY (!write_value(writer, value[index])) {
      return false;
    }

    if VUNLIKELY (writer.position() == element_position) {
      return false;
    }

    if constexpr (is_variable_size<T>()) {
      if VUNLIKELY (index + 1U < value.size() && !writer.align()) {
        return false;
      }
    }
  }

  return true;
}

template <typename T, size_t SizeT, size_t... InnerLenT>
inline bool write_value_body(Writer& writer, const std::array<T, SizeT>& value,
                             std::index_sequence<InnerLenT...> = {}) noexcept {
  for (size_t index = 0U; index < SizeT; ++index) {
    if constexpr (sizeof...(InnerLenT) > 0U) {
      const ArrayLengthField<const T, InnerLenT...> field{value[index]};

      if VUNLIKELY (!write_value(writer, field)) {
        return false;
      }
    } else {
      if VUNLIKELY (!write_value(writer, value[index])) {
        return false;
      }
    }

    if constexpr (is_variable_size<T>()) {
      if VUNLIKELY (index + 1U < SizeT && !writer.align()) {
        return false;
      }
    }
  }

  return true;
}

template <typename KeyT, typename ValueT, typename CompareT, typename AllocatorT>
inline bool write_value_body(Writer& writer, const std::map<KeyT, ValueT, CompareT, AllocatorT>& value) noexcept {
  return write_map_entries(writer, value);
}

template <typename KeyT, typename ValueT, typename HashT, typename EqualT, typename AllocatorT>
inline bool write_value_body(Writer& writer,
                             const std::unordered_map<KeyT, ValueT, HashT, EqualT, AllocatorT>& value) noexcept {
  return write_map_entries(writer, value);
}

template <typename T, size_t LenT, size_t... InnerLenT>
inline bool write_value_body(Writer& writer, const ArrayLengthField<T, LenT, InnerLenT...>& field) noexcept {
  return write_value_body(writer, field.value, std::index_sequence<InnerLenT...>{});
}

template <typename... AlternativeT>
inline bool write_value_body(Writer& writer, const std::variant<AlternativeT...>& value) noexcept {
  static_assert(validate_union_alternatives<AlternativeT...>(), "SOME/IP union alternatives are invalid.");

  if VUNLIKELY (value.valueless_by_exception()) {
    return false;
  }

  static constexpr bool kHasNull = has_null_union_alternative<AlternativeT...>();
  const uint64_t selector = value.index() + (kHasNull ? 0U : 1U);

  return writer.append_unsigned(selector, sizeof(uint32_t), Endian::kBig) &&
         write_union_alternative(writer, value, std::make_index_sequence<sizeof...(AlternativeT)>{});
}

template <typename T>
inline bool write_value_body(Writer& writer, const T& value) noexcept {
  if constexpr (VLINK_HAS_MEMBER(T, is_vlink_someip_type())) {
    using FieldsType = decltype(value.get_vlink_someip_fields());

    static_assert(std::tuple_size_v<FieldsType> > 0U, "SOME/IP structures must declare at least one field.");

    static constexpr size_t kFieldCount = std::tuple_size_v<FieldsType>;
    static constexpr size_t kTlvCount = count_tlv_fields<FieldsType>(std::make_index_sequence<kFieldCount>{});

    static_assert(kTlvCount == 0U || kTlvCount == kFieldCount,
                  "SOME/IP structures must tag either every field or no field with VLINK_SOMEIP_TLV.");
    static_assert(kTlvCount == 0U || get_struct_length<T>() > 0U,
                  "SOME/IP TLV structures must declare a non-zero VLINK_SOMEIP_STRUCT_LENGTH.");
    static_assert(kTlvCount == 0U || get_alignment<T>() == 1U, "SOME/IP TLV structures require 1-byte alignment.");

    const auto fields = value.get_vlink_someip_fields();

    if constexpr (kTlvCount > 0U) {
      static_assert(tlv_data_ids_are_unique<FieldsType>(std::make_index_sequence<kFieldCount>{}),
                    "SOME/IP TLV data IDs must be unique within a structure.");
      static_assert(
          tlv_length_widths_are_equal<get_struct_length<T>(), FieldsType>(std::make_index_sequence<kFieldCount>{}),
          "SOME/IP TLV fields must use the structure length width.");

      return write_tlv_fields(writer, fields, std::make_index_sequence<kFieldCount>{});
    } else {
      return write_fields(writer, fields, std::make_index_sequence<kFieldCount>{});
    }
  } else {
    static_assert(Traits::ExpectFalse<T>(), "Unsupported SOME/IP TLV member type.");

    return false;
  }
}

template <typename MapT>
inline bool write_map_entries(Writer& writer, const MapT& value) noexcept {
  for (const auto& entry : value) {
    const size_t entry_position = writer.position();

    if VUNLIKELY (!write_value(writer, entry.first)) {
      return false;
    }

    if VUNLIKELY (!write_value(writer, entry.second)) {
      return false;
    }

    if VUNLIKELY (writer.position() == entry_position) {
      return false;
    }
  }

  return true;
}

template <size_t IndexT, typename... AlternativeT>
inline bool write_union_element(Writer& writer, const std::variant<AlternativeT...>& value) noexcept {
  if constexpr (IndexT == 0U && has_null_union_alternative<AlternativeT...>()) {
    return true;
  } else {
    return write_value(writer, std::get<IndexT>(value));
  }
}

template <typename... AlternativeT, size_t... IndexT>
inline bool write_union_alternative(Writer& writer, const std::variant<AlternativeT...>& value,
                                    std::index_sequence<IndexT...>) noexcept {
  return ((value.index() != IndexT || write_union_element<IndexT>(writer, value)) && ...);
}

template <uint16_t DataIdT, size_t LenT, bool DynamicT, size_t MaxT = std::numeric_limits<size_t>::max(), typename T>
inline bool write_tlv_value(Writer& writer, const T& value) noexcept {
  static constexpr uint8_t kWireType = get_fixed_wire_type<T>();

  if constexpr (kWireType != kComplexWireType) {
    return detail::write_tag(writer, kWireType, DataIdT) && write_value(writer, value);
  } else {
    const auto write_body = [&value](Writer& target) noexcept {
      if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, Bytes>) {
        return detail::write_value_body(target, value, MaxT);
      } else if constexpr (std::is_same_v<T, std::u16string>) {
        return detail::write_value_body(target, value, Endian::kBig, MaxT);
      } else if constexpr (is_vector(static_cast<const T*>(nullptr))) {
        if VUNLIKELY (value.size() > MaxT) {
          return false;
        }

        return write_value_body(target, value);
      } else {
        return write_value_body(target, value);
      }
    };

    size_t length_width = LenT;
    size_t body_size = 0U;

    if constexpr (DynamicT) {
      Writer sizing = writer.size_only();
      const size_t start = sizing.position();

      if VUNLIKELY (!write_body(sizing)) {
        return false;
      }

      body_size = sizing.position() - start;

      if VLIKELY (body_size <= std::numeric_limits<uint8_t>::max()) {
        length_width = 1U;
      } else if (body_size <= std::numeric_limits<uint16_t>::max()) {
        length_width = 2U;
      } else {
        length_width = 4U;
      }
    }

    size_t length_position = 0U;
    size_t data_position = 0U;

    if VUNLIKELY (!detail::begin_tlv_complex(writer, DataIdT, length_width, DynamicT, length_position, data_position)) {
      return false;
    }

    if constexpr (DynamicT) {
      if (writer.is_size_only()) {
        return writer.append(nullptr, body_size) &&
               writer.end_length_delimited(length_position, data_position, length_width);
      }
    }

    return write_body(writer) && writer.end_length_delimited(length_position, data_position, length_width);
  }
}

template <uint16_t DataIdT, size_t LenT, bool DynamicT, typename T, size_t MaxT>
inline bool write_tlv_field(Writer& writer, const TlvField<DataIdT, LenT, DynamicT, T, MaxT>& field) noexcept {
  using ValueType = std::remove_cv_t<std::remove_reference_t<T>>;

  if constexpr (is_optional(static_cast<const ValueType*>(nullptr))) {
    if (!field.value.has_value()) {
      return true;
    }

    return write_tlv_value<DataIdT, LenT, DynamicT, MaxT>(writer, *field.value);
  } else {
    return write_tlv_value<DataIdT, LenT, DynamicT, MaxT>(writer, field.value);
  }
}

template <uint16_t DataIdT, size_t LenT, bool DynamicT, size_t SizeT, Endian EncodingT, typename T, size_t MaxT>
inline bool write_tlv_field(
    Writer& writer, const TlvFixedStringField<DataIdT, LenT, DynamicT, SizeT, EncodingT, T, MaxT>& field) noexcept {
  using ValueType = std::remove_cv_t<T>;

  const auto write_fixed = [&writer](const auto& value) noexcept {
    size_t length_width = LenT;
    if constexpr (DynamicT) {
      if constexpr (SizeT <= std::numeric_limits<uint8_t>::max()) {
        length_width = 1U;
      } else if constexpr (SizeT <= std::numeric_limits<uint16_t>::max()) {
        length_width = 2U;
      } else {
        length_width = 4U;
      }
    }

    size_t length_position = 0U;
    size_t data_position = 0U;
    const auto fixed = fixed_string_field<SizeT, 0U, EncodingT, MaxT>(value);

    return detail::begin_tlv_complex(writer, DataIdT, length_width, DynamicT, length_position, data_position) &&
           write_value_body(writer, fixed) && writer.end_length_delimited(length_position, data_position, length_width);
  };

  if constexpr (is_optional(static_cast<const ValueType*>(nullptr))) {
    if (!field.value.has_value()) {
      return true;
    }

    return write_fixed(*field.value);
  } else {
    return write_fixed(field.value);
  }
}

template <uint16_t DataIdT, size_t LenT, bool DynamicT, typename T, size_t... InnerLenT>
inline bool write_tlv_field(Writer& writer,
                            const TlvArrayField<DataIdT, LenT, DynamicT, T, InnerLenT...>& field) noexcept {
  using ValueType = std::remove_cv_t<T>;

  if constexpr (is_optional(static_cast<const ValueType*>(nullptr))) {
    if (!field.value.has_value()) {
      return true;
    }

    const auto array = array_length_field<LenT, InnerLenT...>(*field.value);
    return write_tlv_value<DataIdT, LenT, DynamicT>(writer, array);
  } else {
    const auto array = array_length_field<LenT, InnerLenT...>(field.value);
    return write_tlv_value<DataIdT, LenT, DynamicT>(writer, array);
  }
}

template <typename TupleT, size_t... IndexT>
inline bool write_tlv_fields(Writer& writer, const TupleT& fields, std::index_sequence<IndexT...>) noexcept {
  return (write_tlv_field(writer, std::get<IndexT>(fields)) && ...);
}

}  // namespace SomeipSerializer

}  // namespace vlink

////////////////////////////////////////////////////////////////
/// Macro Definitions
////////////////////////////////////////////////////////////////

/**
 * @def VLINK_SOMEIP_ALIGNMENT
 * @brief Selects the AUTOSAR alignment (1, 2, 4, 8, 16, or 32 bytes) for a
 *        top-level payload structure; the default is one byte.
 */
#define VLINK_SOMEIP_ALIGNMENT(alignment) \
  [[nodiscard]] static constexpr size_t vlink_someip_alignment() noexcept { return alignment; }

/**
 * @def VLINK_SOMEIP_ENDIAN
 * @brief Selects the payload scalar byte order for a top-level structure;
 *        the default is @c Endian::kBig.
 */
#define VLINK_SOMEIP_ENDIAN(endian) \
  [[nodiscard]] static constexpr vlink::SomeipSerializer::Endian vlink_someip_endian() noexcept { return endian; }

/**
 * @def VLINK_SOMEIP_ENDIAN_BIG
 * @brief Selects big-endian SOME/IP payload scalar values.
 */
#define VLINK_SOMEIP_ENDIAN_BIG VLINK_SOMEIP_ENDIAN(vlink::SomeipSerializer::Endian::kBig)

/**
 * @def VLINK_SOMEIP_ENDIAN_LITTLE
 * @brief Selects little-endian SOME/IP payload scalar values.
 */
#define VLINK_SOMEIP_ENDIAN_LITTLE VLINK_SOMEIP_ENDIAN(vlink::SomeipSerializer::Endian::kLittle)

/**
 * @def VLINK_SOMEIP_STRUCT_LENGTH
 * @brief Selects the structure length field width: 0 (default, omitted), 1,
 *        2, or 4 bytes; TLV structures must select a non-zero width.
 */
#define VLINK_SOMEIP_STRUCT_LENGTH(width) \
  [[nodiscard]] static constexpr size_t vlink_someip_struct_length() noexcept { return width; }

/**
 * @def VLINK_SOMEIP_LENGTH
 * @brief Wraps one UTF-8/UTF-16 string, @c Bytes, vector, or map field with
 *        a 1, 2, or 4-byte length width; a fixed array may also use zero to
 *        omit it.  Unwrapped length-delimited fields use four bytes.
 */
#define VLINK_SOMEIP_LENGTH(field, width) vlink::SomeipSerializer::length_field<width>(field)

/**
 * @def VLINK_SOMEIP_LENGTH_MAX
 * @brief Adds a maximum character, element, or byte count to a length-delimited field.
 */
#define VLINK_SOMEIP_LENGTH_MAX(field, width, maximum) vlink::SomeipSerializer::length_field<width, maximum>(field)

/**
 * @def VLINK_SOMEIP_ARRAY_LENGTH
 * @brief Wraps one nested array field with per-dimension length widths from
 *        outermost to innermost; unlisted dimensions keep four bytes and a
 *        nested structure keeps its own field configuration.
 */
#define VLINK_SOMEIP_ARRAY_LENGTH(field, ...) vlink::SomeipSerializer::array_length_field<__VA_ARGS__>(field)

/**
 * @def VLINK_SOMEIP_UTF16
 * @brief Wraps one @c std::u16string field with a 1, 2, or 4-byte length
 *        width and an explicit code unit byte order; unwrapped fields use
 *        four bytes and UTF-16BE.
 */
#define VLINK_SOMEIP_UTF16(field, width, endian) vlink::SomeipSerializer::utf16_field<width, endian>(field)

/**
 * @def VLINK_SOMEIP_UTF16_MAX
 * @brief Adds a maximum Unicode character count to an explicitly encoded UTF-16 field.
 */
#define VLINK_SOMEIP_UTF16_MAX(field, width, endian, maximum) \
  vlink::SomeipSerializer::utf16_field<width, endian, maximum>(field)

/**
 * @def VLINK_SOMEIP_UTF16_BE
 * @brief Selects a UTF-16BE string field with a configurable length width.
 */
#define VLINK_SOMEIP_UTF16_BE(field, width) VLINK_SOMEIP_UTF16(field, width, vlink::SomeipSerializer::Endian::kBig)

/**
 * @def VLINK_SOMEIP_UTF16_LE
 * @brief Selects a UTF-16LE string field with a configurable length width.
 */
#define VLINK_SOMEIP_UTF16_LE(field, width) VLINK_SOMEIP_UTF16(field, width, vlink::SomeipSerializer::Endian::kLittle)

/**
 * @def VLINK_SOMEIP_FIXED_STRING
 * @brief Selects a fixed-size UTF-8 string; @p size includes BOM, content,
 *        terminator, and zero padding, while @p width may be 0, 1, 2, or 4.
 */
#define VLINK_SOMEIP_FIXED_STRING(field, size, width) vlink::SomeipSerializer::fixed_string_field<size, width>(field)

/**
 * @def VLINK_SOMEIP_FIXED_STRING_MAX
 * @brief Adds a maximum Unicode character count to a fixed-size UTF-8 string.
 */
#define VLINK_SOMEIP_FIXED_STRING_MAX(field, size, width, maximum) \
  vlink::SomeipSerializer::fixed_string_field<size, width, vlink::SomeipSerializer::Endian::kBig, maximum>(field)

/**
 * @def VLINK_SOMEIP_FIXED_UTF16_BE
 * @brief Selects a fixed-size UTF-16BE string using the specified total byte size and length width.
 */
#define VLINK_SOMEIP_FIXED_UTF16_BE(field, size, width) \
  vlink::SomeipSerializer::fixed_string_field<size, width, vlink::SomeipSerializer::Endian::kBig>(field)

/**
 * @def VLINK_SOMEIP_FIXED_UTF16_BE_MAX
 * @brief Adds a maximum Unicode character count to a fixed-size UTF-16BE string.
 */
#define VLINK_SOMEIP_FIXED_UTF16_BE_MAX(field, size, width, maximum) \
  vlink::SomeipSerializer::fixed_string_field<size, width, vlink::SomeipSerializer::Endian::kBig, maximum>(field)

/**
 * @def VLINK_SOMEIP_FIXED_UTF16_LE
 * @brief Selects a fixed-size UTF-16LE string using the specified total byte size and length width.
 */
#define VLINK_SOMEIP_FIXED_UTF16_LE(field, size, width) \
  vlink::SomeipSerializer::fixed_string_field<size, width, vlink::SomeipSerializer::Endian::kLittle>(field)

/**
 * @def VLINK_SOMEIP_FIXED_UTF16_LE_MAX
 * @brief Adds a maximum Unicode character count to a fixed-size UTF-16LE string.
 */
#define VLINK_SOMEIP_FIXED_UTF16_LE_MAX(field, size, width, maximum) \
  vlink::SomeipSerializer::fixed_string_field<size, width, vlink::SomeipSerializer::Endian::kLittle, maximum>(field)

/**
 * @def VLINK_SOMEIP_UNION
 * @brief Wraps one @c std::variant field with 1, 2, or 4-byte length and
 *        type selector widths; the length excludes the selector, selectors
 *        count alternatives in declaration order from one.  A first
 *        @c std::monostate explicitly enables selector zero.  Unwrapped fields
 *        use four bytes.
 */
#define VLINK_SOMEIP_UNION(field, length_width, type_width) \
  vlink::SomeipSerializer::union_field<length_width, type_width>(field)

/**
 * @def VLINK_SOMEIP_TLV
 * @brief Declares one TLV member with its unique 12-bit data ID; every field
 *        of a TLV structure must be tagged and the structure must declare
 *        @c VLINK_SOMEIP_STRUCT_LENGTH.  Complex members select a 1, 2, or
 *        4-byte length from their actual size, absent @c std::optional members are
 *        omitted and reset on input, unknown data IDs are skipped, and a
 *        repeated known data ID rejects the input.  Nested length-delimited
 *        values require their corresponding field macros.
 */
#define VLINK_SOMEIP_TLV(data_id, field) vlink::SomeipSerializer::tlv_field<data_id>(field)

/**
 * @def VLINK_SOMEIP_TLV_LENGTH
 * @brief Declares one dynamically sized TLV member and configures the 1, 2,
 *        or 4-byte structure width used when receiving an unknown wire type 4;
 *        fixed-width members still declare this shared width but emit wire types 0–3.
 */
#define VLINK_SOMEIP_TLV_LENGTH(data_id, field, width) vlink::SomeipSerializer::tlv_field<data_id, width>(field)

/**
 * @def VLINK_SOMEIP_TLV_LENGTH_MAX
 * @brief Adds a maximum character, element, or byte count to a dynamically sized TLV member.
 */
#define VLINK_SOMEIP_TLV_LENGTH_MAX(data_id, field, width, maximum) \
  vlink::SomeipSerializer::tlv_field<data_id, width, true, maximum>(field)

/**
 * @def VLINK_SOMEIP_TLV_FIXED_STRING
 * @brief Declares a dynamic TLV fixed string with a total byte size, structure width, and UTF-16 byte order.
 */
#define VLINK_SOMEIP_TLV_FIXED_STRING(data_id, field, size, width, endian) \
  vlink::SomeipSerializer::tlv_fixed_string_field<data_id, width, true, size, endian>(field)

/**
 * @def VLINK_SOMEIP_TLV_FIXED_STRING_MAX
 * @brief Adds a maximum Unicode character count to a dynamic TLV fixed string.
 */
#define VLINK_SOMEIP_TLV_FIXED_STRING_MAX(data_id, field, size, width, endian, maximum) \
  vlink::SomeipSerializer::tlv_fixed_string_field<data_id, width, true, size, endian, maximum>(field)

/**
 * @def VLINK_SOMEIP_TLV_ARRAY_LENGTH
 * @brief Declares a multidimensional TLV array member whose inner dimensions
 *        inherit the structure-wide length width.
 */
#define VLINK_SOMEIP_TLV_ARRAY_LENGTH(data_id, field, width, ...) \
  vlink::SomeipSerializer::tlv_array_field<data_id, width, true, __VA_ARGS__>(field)

/**
 * @def VLINK_SOMEIP_TLV_STATIC_LENGTH
 * @brief Declares one complex TLV member that emits wire type 4 with the
 *        configured 1, 2, or 4-byte static structure width; fixed-width members
 *        still declare this shared width but emit wire types 0–3.
 */
#define VLINK_SOMEIP_TLV_STATIC_LENGTH(data_id, field, width) \
  vlink::SomeipSerializer::tlv_field<data_id, width, false>(field)

/**
 * @def VLINK_SOMEIP_TLV_STATIC_LENGTH_MAX
 * @brief Adds a maximum character, element, or byte count to a static-length TLV member.
 */
#define VLINK_SOMEIP_TLV_STATIC_LENGTH_MAX(data_id, field, width, maximum) \
  vlink::SomeipSerializer::tlv_field<data_id, width, false, maximum>(field)

/**
 * @def VLINK_SOMEIP_TLV_STATIC_FIXED_STRING
 * @brief Declares a static TLV fixed string with a total byte size, structure width, and UTF-16 byte order.
 */
#define VLINK_SOMEIP_TLV_STATIC_FIXED_STRING(data_id, field, size, width, endian) \
  vlink::SomeipSerializer::tlv_fixed_string_field<data_id, width, false, size, endian>(field)

/**
 * @def VLINK_SOMEIP_TLV_STATIC_FIXED_STRING_MAX
 * @brief Adds a maximum Unicode character count to a static TLV fixed string.
 */
#define VLINK_SOMEIP_TLV_STATIC_FIXED_STRING_MAX(data_id, field, size, width, endian, maximum) \
  vlink::SomeipSerializer::tlv_fixed_string_field<data_id, width, false, size, endian, maximum>(field)

/**
 * @def VLINK_SOMEIP_TLV_STATIC_ARRAY_LENGTH
 * @brief Declares a multidimensional static-length TLV array member whose
 *        inner dimensions inherit the structure-wide length width.
 */
#define VLINK_SOMEIP_TLV_STATIC_ARRAY_LENGTH(data_id, field, width, ...) \
  vlink::SomeipSerializer::tlv_array_field<data_id, width, false, __VA_ARGS__>(field)

/**
 * @def VLINK_SOMEIP_FIELDS
 * @brief Declares a structure as a SOME/IP payload and lists its fields in
 *        wire-format order.
 *
 * @details
 * Generates the SOME/IP marker, ordered field access, an exact-size query, a
 * @c serialize() member with an explicit offset parameter, a @c deserialize()
 * member that reads the @c Bytes payload region, and the @c vlink::Bytes stream
 * operators.  Fields may be fixed-width
 * numeric values, unsigned fixed-width enums, @c std::string,
 * @c std::u16string, @c Bytes, @c std::vector, @c std::array, @c std::map,
 * @c std::unordered_map, @c std::variant, nested structures using the same
 * macro, or TLV members declared with @c VLINK_SOMEIP_TLV.  At least one
 * field must be listed, and buffers passed to the generated operators must
 * not overlap storage reachable from the structure.
 */
#define VLINK_SOMEIP_FIELDS(...)                                                                                      \
  [[nodiscard]] static constexpr bool is_vlink_someip_type() noexcept { return true; }                                \
                                                                                                                      \
  [[nodiscard]] auto get_vlink_someip_fields() noexcept { return vlink::SomeipSerializer::make_fields(__VA_ARGS__); } \
                                                                                                                      \
  [[nodiscard]] auto get_vlink_someip_fields() const noexcept {                                                       \
    return vlink::SomeipSerializer::make_fields(__VA_ARGS__);                                                         \
  }                                                                                                                   \
                                                                                                                      \
  [[nodiscard]] size_t get_serialized_size() const noexcept {                                                         \
    return vlink::SomeipSerializer::get_serialized_size(*this);                                                       \
  }                                                                                                                   \
                                                                                                                      \
  bool serialize(vlink::Bytes& out, uint8_t offset = 0U) const noexcept {                                             \
    return vlink::SomeipSerializer::serialize(*this, out, offset);                                                    \
  }                                                                                                                   \
                                                                                                                      \
  bool deserialize(const vlink::Bytes& in) noexcept { return vlink::SomeipSerializer::deserialize(in, *this); }       \
                                                                                                                      \
  bool operator>>(vlink::Bytes& out) const noexcept { return serialize(out); }                                        \
                                                                                                                      \
  bool operator<<(const vlink::Bytes& in) noexcept { return deserialize(in); }
