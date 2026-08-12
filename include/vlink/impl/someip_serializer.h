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
 * Implements the AUTOSAR R25-11 non-TLV payload deployment selected by
 * @c VLINK_SOMEIP_FIELDS: big-endian scalar values, one-byte alignment,
 * four-byte string, dynamic-array, and fixed-array length fields, no structure
 * length field, and UTF-8 strings framed by a BOM and null terminator.  The
 * SOME/IP message header is outside this codec.
 *
 * Non-template cursor and primitive operations are implemented once in
 * @c src/impl/someip_serializer.cc.  Only container traversal and user-defined
 * structure expansion remain templates because their types are application
 * defined.
 *
 * @par Codec flow
 * @verbatim
 *  Serializer::kSomeipType
 *             |
 *             v
 *  generated operator >> / <<
 *             |
 *             v
 *  SomeipSerializer::serialize / deserialize
 *             |
 *             v
 *  ordered write_value / read_value traversal
 *             |
 *             v
 *        Writer / Reader  <---->  Bytes
 * @endverbatim
 *
 * @par Payload layout
 *
 * | C++ field                       | SOME/IP payload representation                         |
 * | ------------------------------- | ------------------------------------------------------ |
 * | @c bool                         | One byte; the encoder emits @c 0 or @c 1.              |
 * | Fixed-width integer or enum     | Natural width, most-significant byte first.            |
 * | @c float / @c double            | IEEE 754 binary32 / binary64 in big-endian order.      |
 * | @c std::string                  | 32-bit length, UTF-8 BOM, content, null terminator.    |
 * | @c Bytes / @c vector / @c array | 32-bit byte length followed by encoded content.        |
 * | Macro-declared nested structure | Declared fields in order, without a structure length.  |
 *
 * String lengths include the three-byte UTF-8 BOM and trailing null byte.
 * Container lengths count encoded bytes rather than elements.  No implicit
 * alignment padding is inserted between fields.  The complete payload is
 * limited to @c UINT32_MAX-8 bytes by the SOME/IP message Length field.
 *
 * @par Length-delimited field
 * @verbatim
 *  byte:   0       1       2       3       4                     4 + N
 *        +-------+-------+-------+-------+-----------------------------+
 *        |          payload length N (big-endian)       | N-byte body |
 *        +-------+-------+-------+-------+-----------------------------+
 * @endverbatim
 *
 * @par Failure model
 *
 * Encoding and decoding report format errors, size overflow, and @c Bytes
 * storage failure through their boolean return value.  Standard container
 * allocation failures retain their normal exception contract.  Unsupported
 * field types are rejected at compile time.
 *
 * @par Supported profile
 *
 * This helper intentionally implements the selected non-TLV deployment only.
 * Fixed arrays use the optional four-byte length field for compatible growth.
 * Dynamic UTF-8 strings preserve embedded null bytes; only the final byte is
 * interpreted as the configured terminator.
 * Because structures have no length field, unknown trailing structure fields
 * can be ignored only at the top-level payload boundary; nested structures and
 * array elements require the same deployed field layout on both endpoints.
 * The helper does not provide configurable byte order, alignment, length-field
 * widths, structure length fields, UTF-16 or fixed-length strings, unions, C++
 * bit-fields, optional TLV members, or deployment maximum-length constraints.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "../base/bytes.h"
#include "../base/macros.h"
#include "../base/traits.h"

namespace vlink {

namespace SomeipSerializer {  // NOLINT(readability-identifier-naming)

/**
 * @brief Maximum payload size representable by a SOME/IP message.
 *
 * @details
 * The 32-bit SOME/IP Length field includes the eight bytes from Request ID
 * through Return Code, leaving this many bytes for the payload.
 */
inline constexpr size_t kMaxPayloadSize = std::numeric_limits<uint32_t>::max() - 8U;

/**
 * @class Writer
 * @brief Bounds-checked big-endian SOME/IP payload writer.
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
   * @param data      Destination buffer, or @c nullptr for size-only mode.
   * @param capacity  Number of writable bytes, or the maximum size to measure.
   */
  Writer(uint8_t* data, size_t capacity) noexcept;

  /**
   * @brief Returns the number of bytes consumed by this writer.
   *
   * @return Current zero-based write position in bytes.
   */
  [[nodiscard]] size_t position() const noexcept;

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
   * @brief Appends the low @p width bytes of @p value in big-endian order.
   *
   * @param value  Unsigned value containing the bits to encode.
   * @param width  Encoded width in bytes; must be in the range @c [1,8].
   * @return @c true on success; @c false for an invalid width or insufficient storage.
   */
  [[nodiscard]] bool append_unsigned(uint64_t value, size_t width) noexcept;

  /**
   * @brief Rewrites a previously reserved four-byte length field.
   *
   * @details
   * The size-only pass performs no write and returns success.  This operation
   * does not change the current writer position.
   *
   * @param position  Offset of the reserved length field.
   * @param value     Length value encoded in big-endian order.
   * @return @c true on success; @c false when the field lies outside the buffer.
   */
  [[nodiscard]] bool patch_uint32(size_t position, uint32_t value) noexcept;

  /**
   * @brief Reserves a four-byte length field and records its payload start.
   *
   * @param length_position  Output offset of the reserved length field.
   * @param data_position    Output offset of the first length-delimited byte.
   * @return @c true on success; @c false when four bytes cannot be reserved.
   */
  [[nodiscard]] bool begin_length_delimited(size_t& length_position, size_t& data_position) noexcept;

  /**
   * @brief Finalizes a reserved length field from the current position.
   *
   * @details
   * Stores @c position()-@p data_position as an unsigned 32-bit byte count at
   * @p length_position.  The current position is not changed.
   *
   * @param length_position  Offset returned by @c begin_length_delimited().
   * @param data_position    Payload start returned by @c begin_length_delimited().
   * @return @c true on success; @c false on an invalid range or 32-bit length overflow.
   */
  [[nodiscard]] bool end_length_delimited(size_t length_position, size_t data_position) noexcept;

 private:
  uint8_t* data_{nullptr};
  size_t capacity_{0};
  size_t position_{0};
};

/**
 * @class Reader
 * @brief Bounds-checked big-endian SOME/IP payload reader.
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
   * @param data  Source payload.
   * @param size  Total source size in bytes.
   */
  Reader(const uint8_t* data, size_t size) noexcept;

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
   * @brief Reads a @p width-byte unsigned integer in big-endian order.
   *
   * @param value  Output value; modified only after the range is validated.
   * @param width  Encoded width in bytes; must be in the range @c [1,8].
   * @param end    Exclusive enclosing boundary, not greater than @c size().
   * @return @c true on success; @c false for an invalid width or range.
   */
  [[nodiscard]] bool read_unsigned(uint64_t& value, size_t width, size_t end) noexcept;

  /**
   * @brief Reads a four-byte length and returns its validated end position.
   *
   * @details
   * The prefix is a big-endian byte count.  On success, @p value_end is the
   * exclusive end of the declared body and the reader points to its first byte.
   *
   * @param end        Exclusive enclosing boundary.
   * @param value_end  Output exclusive boundary of the length-delimited body.
   * @return @c true on success; @c false when the prefix or declared body exceeds @p end.
   */
  [[nodiscard]] bool begin_length_delimited(size_t end, size_t& value_end) noexcept;

 private:
  const uint8_t* data_{nullptr};
  size_t size_{0};
  size_t position_{0};
};

/**
 * @name Primitive SOME/IP encoders
 * @brief Encodes scalar, string, and byte-buffer fields into @p writer.
 *
 * @details
 * Integer overloads preserve the source bit pattern and use big-endian byte
 * order.  Floating-point overloads require IEEE 754 storage.  Strings include
 * a 32-bit byte length, UTF-8 BOM, and null terminator; invalid UTF-8 is
 * rejected.  @c Bytes uses a 32-bit byte length followed by its payload.
 *
 * @param writer  Destination cursor.
 * @param value   Field value to encode.
 * @return @c true on success; @c false on invalid data, overflow, or insufficient storage.
 * @{
 */
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
VLINK_EXPORT bool write_value(Writer& writer, const std::string& value) noexcept;
VLINK_EXPORT bool write_value(Writer& writer, const Bytes& value) noexcept;
/**
 * @}
 */

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
 * @brief Encodes a dynamic array with a 32-bit byte-length prefix.
 *
 * @details
 * The prefix counts encoded bytes, not elements.  The @c vector<bool>
 * specialization is traversed by value to avoid proxy-reference dispatch.
 *
 * @tparam T           Element type supported by @c write_value().
 * @tparam AllocatorT  Vector allocator type.
 * @param writer       Destination cursor.
 * @param value        Array elements to encode in order.
 * @return @c true on success; @c false on overflow or element encoding failure.
 */
template <typename T, typename AllocatorT>
inline bool write_value(Writer& writer, const std::vector<T, AllocatorT>& value) noexcept;

/**
 * @brief Encodes a fixed-size array with a 32-bit byte-length prefix.
 *
 * @tparam T      Element type supported by @c write_value().
 * @tparam SizeT  Compile-time element count.
 * @param writer  Destination cursor.
 * @param value   Array elements to encode in order.
 * @return @c true on success; @c false on overflow or element encoding failure.
 */
template <typename T, size_t SizeT>
inline bool write_value(Writer& writer, const std::array<T, SizeT>& value) noexcept;

/**
 * @name Primitive SOME/IP decoders
 * @brief Decodes scalar, string, and byte-buffer fields from @p reader.
 *
 * @details
 * Integer and floating-point overloads reverse the encoder wire format.
 * Boolean values use the low bit of the encoded byte.  String input must have
 * a valid 32-bit byte length, UTF-8 BOM, null terminator, and UTF-8 content.
 * @c Bytes reuses sufficient owning capacity or allocates owned storage for
 * its declared payload.
 *
 * @param reader  Source cursor.
 * @param value   Destination field, updated on success.
 * @param end     Exclusive boundary of the enclosing field or payload.
 * @return @c true on success; @c false when the encoded field is malformed or truncated.
 * @{
 */
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
VLINK_EXPORT bool read_value(Reader& reader, std::string& value, size_t end);
VLINK_EXPORT bool read_value(Reader& reader, Bytes& value, size_t end) noexcept;
/**
 * @}
 */

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
inline bool read_value(Reader& reader, T& value, size_t end);

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
 * @param reader       Source cursor.
 * @param value        Destination vector whose existing elements are reused when possible.
 * @param end          Exclusive boundary of the enclosing field or payload.
 * @return @c true on success; @c false for an invalid length or element encoding.
 */
template <typename T, typename AllocatorT>
inline bool read_value(Reader& reader, std::vector<T, AllocatorT>& value, size_t end);

/**
 * @brief Decodes a byte-length-delimited fixed-size array.
 *
 * @details
 * Exactly @c SizeT elements are decoded.  Remaining bytes in a longer remote
 * representation are skipped as required for compatible fixed-array growth;
 * truncated content is rejected.
 *
 * @tparam T      Element type supported by @c read_value().
 * @tparam SizeT  Compile-time element count.
 * @param reader  Source cursor.
 * @param value   Destination array.
 * @param end     Exclusive boundary of the enclosing field or payload.
 * @return @c true on success; @c false when the length cannot contain all elements.
 */
template <typename T, size_t SizeT>
inline bool read_value(Reader& reader, std::array<T, SizeT>& value, size_t end);

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
inline bool deserialize(const Bytes& src, T& des);

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

template <typename T, typename AllocatorT>
inline bool write_value(Writer& writer, const std::vector<T, AllocatorT>& value) noexcept {
  size_t length_position = 0;
  size_t data_position = 0;

  if VUNLIKELY (!writer.begin_length_delimited(length_position, data_position)) {
    return false;
  }

  if constexpr (std::is_same_v<T, bool>) {
    for (bool element : value) {
      if VUNLIKELY (!write_value(writer, element)) {
        return false;
      }
    }
  } else {
    for (const auto& element : value) {
      if VUNLIKELY (!write_value(writer, element)) {
        return false;
      }
    }
  }

  return writer.end_length_delimited(length_position, data_position);
}

template <typename T, size_t SizeT>
inline bool write_value(Writer& writer, const std::array<T, SizeT>& value) noexcept {
  size_t length_position = 0;
  size_t data_position = 0;

  if VUNLIKELY (!writer.begin_length_delimited(length_position, data_position)) {
    return false;
  }

  for (const auto& element : value) {
    if VUNLIKELY (!write_value(writer, element)) {
      return false;
    }
  }

  return writer.end_length_delimited(length_position, data_position);
}

template <typename T>
inline bool write_value(Writer& writer, const T& value) noexcept {
  if constexpr (std::is_enum_v<T>) {
    using UnderlyingType = std::underlying_type_t<T>;

    static_assert(std::is_unsigned_v<UnderlyingType> && !std::is_same_v<UnderlyingType, bool>,
                  "SOME/IP enums must use uint8_t, uint16_t, uint32_t, or uint64_t storage.");
    return write_value(writer, static_cast<UnderlyingType>(value));
  } else if constexpr (VLINK_HAS_MEMBER(T, is_vlink_someip_type())) {
    using FieldsType = decltype(value.vlink_someip_fields());

    static_assert(std::tuple_size_v<FieldsType> > 0U, "SOME/IP structures must declare at least one field.");

    return std::apply([&writer](const auto&... field) noexcept { return (write_value(writer, field) && ...); },
                      value.vlink_someip_fields());
  } else {
    static_assert(Traits::ExpectFalse<T>(), "Unsupported SOME/IP field type.");

    return false;
  }
}

template <typename T, typename AllocatorT>
inline bool read_value(Reader& reader, std::vector<T, AllocatorT>& value, size_t end) {
  size_t array_end = 0;

  if VUNLIKELY (!reader.begin_length_delimited(end, array_end)) {
    return false;
  }

  if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) {
    constexpr size_t kElementSize = sizeof(T);
    const size_t byte_length = array_end - reader.position();

    if VUNLIKELY (byte_length % kElementSize != 0U) {
      return false;
    }

    value.reserve(byte_length / kElementSize);
  }

  size_t index = 0U;

  while (reader.position() < array_end) {
    const size_t element_position = reader.position();

    if constexpr (std::is_same_v<T, bool>) {
      bool element = false;

      if VUNLIKELY (!read_value(reader, element, array_end) || reader.position() == element_position) {
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

      if VUNLIKELY (!read_value(reader, value[index], array_end) || reader.position() == element_position) {
        return false;
      }
    }

    ++index;
  }

  value.resize(index);

  return true;
}

template <typename T, size_t SizeT>
inline bool read_value(Reader& reader, std::array<T, SizeT>& value, size_t end) {
  size_t array_end = 0;

  if VUNLIKELY (!reader.begin_length_delimited(end, array_end)) {
    return false;
  }

  for (auto& element : value) {
    if VUNLIKELY (!read_value(reader, element, array_end)) {
      return false;
    }
  }

  return reader.skip(array_end - reader.position(), array_end);
}

template <typename T>
inline bool read_value(Reader& reader, T& value, size_t end) {
  if constexpr (std::is_enum_v<T>) {
    using UnderlyingType = std::underlying_type_t<T>;

    static_assert(std::is_unsigned_v<UnderlyingType> && !std::is_same_v<UnderlyingType, bool>,
                  "SOME/IP enums must use uint8_t, uint16_t, uint32_t, or uint64_t storage.");
    UnderlyingType raw = 0;

    if VUNLIKELY (!read_value(reader, raw, end)) {
      return false;
    }

    value = static_cast<T>(raw);

    return true;
  } else if constexpr (VLINK_HAS_MEMBER(T, is_vlink_someip_type())) {
    using FieldsType = decltype(value.vlink_someip_fields());

    static_assert(std::tuple_size_v<FieldsType> > 0U, "SOME/IP structures must declare at least one field.");

    return std::apply([&reader, end](auto&... field) { return (read_value(reader, field, end) && ...); },
                      value.vlink_someip_fields());
  } else {
    static_assert(Traits::ExpectFalse<T>(), "Unsupported SOME/IP field type.");

    return false;
  }
}

template <typename T>
inline size_t get_serialized_size(const T& src) noexcept {
  Writer writer(nullptr, std::numeric_limits<size_t>::max());

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

    Writer writer(des.data(), des.size());

    if VUNLIKELY (!write_value(writer, src) || writer.position() != des.size()) {
      return false;
    }

    return true;
  }

  Writer size_writer(nullptr, std::numeric_limits<size_t>::max());

  if VUNLIKELY (!write_value(size_writer, src)) {
    return false;
  }

  const size_t size = size_writer.position();

  if (des.is_owner() && des.offset() == offset && size <= des.capacity()) {
    if VUNLIKELY (!des.resize(size)) {
      return false;
    }
  } else {
    des = Bytes::create(size, offset);
  }

  if VUNLIKELY (size > 0U && !des.data()) {
    return false;
  }

  Writer writer(des.data(), des.size());

  if VUNLIKELY (!write_value(writer, src) || writer.position() != size) {
    return false;
  }

  return true;
}

template <typename T>
inline bool deserialize(const Bytes& src, T& des) {
  if VUNLIKELY (src.size() > kMaxPayloadSize) {
    return false;
  }

  Reader reader(src.data(), src.size());

  return read_value(reader, des, reader.size());
}

}  // namespace SomeipSerializer

}  // namespace vlink

////////////////////////////////////////////////////////////////
/// Macro Definitions
////////////////////////////////////////////////////////////////

/**
 * @def VLINK_SOMEIP_FIELDS
 * @brief Declares a structure as a SOME/IP payload and lists its fields.
 *
 * @details
 * Place the macro in the public section of a structure.  It generates the
 * SOME/IP marker, ordered field access, an exact-size query, and the VLink
 * custom @c Bytes operators.  Fields may be fixed-width numeric values,
 * unsigned fixed-width enums, @c std::string, @c Bytes, @c std::vector,
 * @c std::array, or nested structures using the same macro.  At least one
 * field must be listed.  The generated deserializer requires its input storage
 * not to overlap any storage reachable from the destination structure.  Its
 * serializer likewise requires output storage not to overlap any storage
 * reachable from the source structure.
 */
// clang-format off
#define VLINK_SOMEIP_FIELDS(...)                                                                      \
  [[nodiscard]] static constexpr bool is_vlink_someip_type() noexcept {                               \
    return true;                                                                                      \
  }                                                                                                   \
                                                                                                      \
  [[nodiscard]] auto vlink_someip_fields() noexcept {                                                 \
    return std::tie(__VA_ARGS__);                                                                     \
  }                                                                                                   \
                                                                                                      \
  [[nodiscard]] auto vlink_someip_fields() const noexcept {                                           \
    return std::tie(__VA_ARGS__);                                                                     \
  }                                                                                                   \
                                                                                                      \
  [[nodiscard]] size_t get_serialized_size() const noexcept {                                         \
    return vlink::SomeipSerializer::get_serialized_size(*this);                                       \
  }                                                                                                   \
                                                                                                      \
  bool operator>>(vlink::Bytes& out) const {                                                          \
    return vlink::SomeipSerializer::serialize(*this, out);                                            \
  }                                                                                                   \
                                                                                                      \
  bool operator<<(const vlink::Bytes& in) {                                                           \
    return vlink::SomeipSerializer::deserialize(in, *this);                                           \
  }
// clang-format on
