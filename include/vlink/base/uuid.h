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
 * @file uuid.h
 * @brief Value-typed RFC 4122 UUID and project random-bytes primitive.
 *
 * @details
 * @c vlink::Uuid stores 16 bytes in big-endian (network) order matching RFC 4122 section
 * 4.1.  The class is trivially comparable, parses and emits both the canonical 36-character
 * hyphenated form and the 32-character compact form, and ships with a thread-local v4
 * generator backed by @c std::mt19937.
 *
 * UUID variant and version reference:
 *
 * | Field    | Enumerator          | RFC 4122 meaning                              |
 * | -------- | ------------------- | --------------------------------------------- |
 * | Variant  | @c kNcs             | NCS backward compatibility (@c 0xxx)          |
 * | Variant  | @c kRfc             | RFC 4122 / DCE 1.1 (@c 10xx)                  |
 * | Variant  | @c kMicrosoft       | Microsoft GUID (@c 110x)                      |
 * | Variant  | @c kReserved        | Reserved (@c 111x)                            |
 * | Version  | @c kNone            | Nil UUID or invalid version nibble            |
 * | Version  | @c kTimeBased       | v1 — gregorian time + node                    |
 * | Version  | @c kDceSecurity     | v2 — DCE Security                             |
 * | Version  | @c kNameBasedMd5    | v3 — Name + MD5                               |
 * | Version  | @c kRandomBased     | v4 — Random / pseudo-random (this generator)  |
 * | Version  | @c kNameBasedSha1   | v5 — Name + SHA-1                             |
 *
 * Random v4 generation pipeline:
 *
 * @verbatim
 *   std::random_device x8 ---> std::seed_seq ---> std::mt19937 ---> uniform_int(uint32_t)
 *                                                       |
 *                                                       v
 *                                            byte extraction via shifts
 *                                                       |
 *                                                       v
 *                            set variant (octet 8 = 10xxxxxx) + version (octet 6 = 0100xxxx)
 * @endverbatim
 *
 * @note Bodies live in @c uuid.cc; only @c constexpr operations remain inline so
 *       @c Uuid stays a literal type for compile-time use.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <optional>
#include <ostream>
#include <random>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "./macros.h"

namespace vlink {

/**
 * @class Uuid
 * @brief Value-typed RFC 4122 UUID.
 *
 * @details
 * Stores 16 bytes in network byte order.  Trivially copyable and comparable; provides
 * canonical/compact text I/O and a v4 random generator built on top of a thread-local
 * @c std::mt19937 engine.
 */
class VLINK_EXPORT Uuid final {
 public:
  /**
   * @brief Underlying byte type used by the 16-byte payload.
   */
  using value_type = uint8_t;

  /**
   * @brief UUID payload length (16 bytes per RFC 4122).
   */
  static constexpr size_t kByteSize = 16U;

  /**
   * @brief Canonical 36-character textual length: @c xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx.
   */
  static constexpr size_t kStringSize = 36U;

  /**
   * @enum Variant
   * @brief UUID variant inferred from the high bits of octet 8.
   */
  enum class Variant : uint8_t {
    kNcs = 0,        ///< NCS backward compatibility (@c 0xxx).
    kRfc = 1,        ///< RFC 4122 / DCE 1.1 (@c 10xx).
    kMicrosoft = 2,  ///< Microsoft GUID (@c 110x).
    kReserved = 3,   ///< Reserved for future use (@c 111x).
  };

  /**
   * @enum Version
   * @brief UUID version inferred from the high nibble of octet 6.
   */
  enum class Version : uint8_t {
    kNone = 0,           ///< Nil UUID or invalid version nibble.
    kTimeBased = 1,      ///< v1 time-based.
    kDceSecurity = 2,    ///< v2 DCE Security.
    kNameBasedMd5 = 3,   ///< v3 Name-based MD5.
    kRandomBased = 4,    ///< v4 random (produced by @c generate_random).
    kNameBasedSha1 = 5,  ///< v5 Name-based SHA-1.
  };

  /**
   * @brief Default-constructs a nil UUID (all 16 bytes zero).
   */
  constexpr Uuid() noexcept;

  /**
   * @brief Constructs from a 16-byte array.
   *
   * @param data  Network-order UUID payload.
   */
  constexpr explicit Uuid(const std::array<value_type, kByteSize>& data) noexcept;

  /**
   * @brief Constructs from a raw C array reference of exactly 16 bytes.
   *
   * @param arr  Reference to a 16-byte array.
   */
  constexpr explicit Uuid(const value_type (&arr)[kByteSize]) noexcept;

  /**
   * @brief Constructs from a forward-iterator range of exactly 16 bytes.
   *
   * @details
   * When @c std::distance(first, last) is not @c 16 the resulting UUID is the nil value.
   * A @c static_assert enforces forward-iterator capability so input-only iterators do
   * not silently produce a misaligned UUID.
   *
   * @tparam ForwardIteratorT  Forward iterator yielding @c uint8_t-convertible values.
   * @param  first             Begin iterator.
   * @param  last              End iterator.
   */
  template <typename ForwardIteratorT>
  Uuid(ForwardIteratorT first, ForwardIteratorT last);

  /**
   * @brief Returns the variant field inferred from octet 8.
   *
   * @return Variant enumerator.
   */
  [[nodiscard]] constexpr Variant variant() const noexcept;

  /**
   * @brief Returns the version field inferred from the high nibble of octet 6.
   *
   * @return Version enumerator.
   */
  [[nodiscard]] constexpr Version version() const noexcept;

  /**
   * @brief Reports whether every byte is zero (the nil UUID).
   *
   * @return @c true when the payload is all zeros.
   */
  [[nodiscard]] constexpr bool is_nil() const noexcept;

  /**
   * @brief Provides read-only access to the underlying 16-byte payload.
   *
   * @return Const reference to the byte array.
   */
  [[nodiscard]] constexpr const std::array<value_type, kByteSize>& bytes() const noexcept;

  /**
   * @brief Formats the UUID as the canonical 36-character lowercase hyphenated string.
   *
   * @return Canonical text representation.
   */
  [[nodiscard]] std::string to_string() const noexcept;

  /**
   * @brief Formats the UUID as a 32-character lowercase hex string without hyphens.
   *
   * @return Compact text representation.
   */
  [[nodiscard]] std::string to_compact_string() const noexcept;

  /**
   * @brief Swaps contents with @p other in @c noexcept fashion.
   *
   * @param other  UUID to swap with.
   */
  void swap(Uuid& other) noexcept;

  /**
   * @brief Validates whether @p str is a well-formed UUID literal.
   *
   * @details
   * Accepts the canonical 36-character form, the 32-character compact form, and either
   * form wrapped in an outer brace pair @c "{...}".  Hyphen positions are not enforced:
   * any 32 hex digits within the allowed shape pass.
   *
   * @param str  Candidate string.
   * @return @c true when @p str is valid.
   */
  [[nodiscard]] static bool is_valid(std::string_view str) noexcept;

  /**
   * @brief Null-safe C-string overload of @c is_valid().
   *
   * @param str  NUL-terminated candidate string, or @c nullptr.
   * @return @c true when @p str is non-null and a valid UUID literal.
   */
  [[nodiscard]] static bool is_valid(const char* str) noexcept;

  /**
   * @brief Parses a UUID literal and returns the resulting value.
   *
   * @details
   * Accepts the same shapes as @c is_valid().
   *
   * @param str  Candidate string.
   * @return Parsed UUID, or @c std::nullopt on malformed input.
   */
  [[nodiscard]] static std::optional<Uuid> from_string(std::string_view str) noexcept;

  /**
   * @brief Null-safe C-string overload of @c from_string().
   *
   * @param str  NUL-terminated candidate string, or @c nullptr.
   * @return Parsed UUID, or @c std::nullopt on malformed/null input.
   */
  [[nodiscard]] static std::optional<Uuid> from_string(const char* str) noexcept;

  /**
   * @brief Generates a random v4 UUID using a thread-local seeded engine.
   *
   * @details
   * The engine is lazily seeded on first use from eight @c std::random_device samples fed
   * through a @c std::seed_seq, then reused for the rest of the thread's lifetime.  Sets
   * the RFC variant and v4 version bits before returning.
   *
   * @return Fresh v4 UUID.
   */
  [[nodiscard]] static Uuid generate_random() noexcept;

  /**
   * @brief Generates a random v4 UUID from a caller-managed engine.
   *
   * @details
   * Useful for deterministic test fixtures.  Sets the RFC variant and v4 version bits
   * identically to the no-argument overload.
   *
   * @param engine  Caller-managed engine.
   * @return Fresh v4 UUID.
   */
  [[nodiscard]] static Uuid generate_random(std::mt19937& engine) noexcept;

  /**
   * @brief Produces @p count random-device-seeded pseudo-random bytes.
   *
   * @details
   * Uses the same pipeline as @c generate_random(): @c std::seed_seq from eight
   * @c std::random_device samples, then @c std::mt19937 plus
   * @c std::uniform_int_distribution<uint32_t> emitting four bytes per draw.  Byte
   * extraction uses explicit shifts so the output is endian-deterministic.  Returns
   * an empty vector when @p count is @c 0 or allocation fails.
   *
   * @warning The underlying @c std::mt19937 engine is @b not a CSPRNG.  Observing 624
   *          consecutive 32-bit outputs allows the engine state to be reconstructed.  Do
   *          not use this for long-term secrets; prefer a dedicated CSPRNG (for example
   *          OpenSSL @c RAND_bytes).
   *
   * @param count  Number of bytes to emit.
   * @return Vector containing exactly @p count pseudo-random bytes.
   */
  [[nodiscard]] static std::vector<value_type> random_bytes(size_t count) noexcept;

  /**
   * @brief Produces @p byte_count pseudo-random bytes encoded as a lowercase hex string.
   *
   * @details
   * Length is always @c byte_count @c * @c 2.  The default of 16 bytes matches the
   * 128-bit auth-token width used by the proxy handshake.  Returns an empty string when
   * @p byte_count is @c 0 or allocation fails.  Same non-CSPRNG caveat as
   * @c random_bytes().
   *
   * @param byte_count  Number of underlying bytes.
   * @return Lowercase hex string.
   */
  [[nodiscard]] static std::string random_hex(size_t byte_count = 16U) noexcept;

  /**
   * @brief Equality comparison over the 16-byte payload.
   *
   * @param lhs  Left operand.
   * @param rhs  Right operand.
   * @return @c true when both payloads compare byte-equal.
   */
  friend bool operator==(const Uuid& lhs, const Uuid& rhs) noexcept;

  /**
   * @brief Inequality comparison over the 16-byte payload.
   *
   * @param lhs  Left operand.
   * @param rhs  Right operand.
   * @return @c true when the payloads differ in any byte.
   */
  friend bool operator!=(const Uuid& lhs, const Uuid& rhs) noexcept;

  /**
   * @brief Lexicographic less-than comparison over the 16-byte payload.
   *
   * @param lhs  Left operand.
   * @param rhs  Right operand.
   * @return @c true when @p lhs sorts strictly before @p rhs.
   */
  friend bool operator<(const Uuid& lhs, const Uuid& rhs) noexcept;

  /**
   * @brief Stream insertion delegating to @c to_string().
   *
   * @param ostream  Output stream.
   * @param id       UUID to format.
   * @return Reference to @p ostream.
   */
  VLINK_EXPORT friend std::ostream& operator<<(std::ostream& ostream, const Uuid& id);

 private:
  std::array<value_type, kByteSize> data_{};
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

template <typename ForwardIteratorT>
inline Uuid::Uuid(ForwardIteratorT first, ForwardIteratorT last) {
  static_assert(
      std::is_base_of_v<std::forward_iterator_tag, typename std::iterator_traits<ForwardIteratorT>::iterator_category>,
      "Uuid range constructor requires at least a forward iterator");

  if (std::distance(first, last) == static_cast<std::ptrdiff_t>(kByteSize)) {
    std::copy(first, last, data_.begin());
  }
}

inline constexpr Uuid::Uuid() noexcept = default;

inline constexpr Uuid::Uuid(const std::array<value_type, kByteSize>& data) noexcept : data_{data} {}

inline constexpr Uuid::Uuid(const value_type (&arr)[kByteSize]) noexcept {
  for (size_t i = 0U; i < kByteSize; ++i) {
    data_[i] = arr[i];
  }
}

inline constexpr Uuid::Variant Uuid::variant() const noexcept {
  const uint8_t octet = data_[8];

  if ((octet & 0x80U) == 0x00U) {
    return Variant::kNcs;
  }

  if ((octet & 0xC0U) == 0x80U) {
    return Variant::kRfc;
  }

  if ((octet & 0xE0U) == 0xC0U) {
    return Variant::kMicrosoft;
  }

  return Variant::kReserved;
}

inline constexpr Uuid::Version Uuid::version() const noexcept {
  switch (data_[6] & 0xF0U) {
    case 0x10U:
      return Version::kTimeBased;
    case 0x20U:
      return Version::kDceSecurity;
    case 0x30U:
      return Version::kNameBasedMd5;
    case 0x40U:
      return Version::kRandomBased;
    case 0x50U:
      return Version::kNameBasedSha1;
    default:
      return Version::kNone;
  }
}

inline constexpr bool Uuid::is_nil() const noexcept {
  for (uint8_t byte_value : data_) {
    if (byte_value != 0U) {
      return false;
    }
  }

  return true;
}

inline constexpr const std::array<Uuid::value_type, Uuid::kByteSize>& Uuid::bytes() const noexcept { return data_; }

inline void Uuid::swap(Uuid& other) noexcept { data_.swap(other.data_); }

inline bool operator==(const Uuid& lhs, const Uuid& rhs) noexcept { return lhs.data_ == rhs.data_; }

inline bool operator!=(const Uuid& lhs, const Uuid& rhs) noexcept { return lhs.data_ != rhs.data_; }

inline bool operator<(const Uuid& lhs, const Uuid& rhs) noexcept { return lhs.data_ < rhs.data_; }

}  // namespace vlink

namespace std {

/**
 * @brief @c std::hash specialisation so @c vlink::Uuid can be used inside unordered containers.
 */
template <>
struct hash<vlink::Uuid> {
  size_t operator()(const vlink::Uuid& id) const noexcept {
    const auto& data = id.bytes();
    size_t result = 0U;

    for (uint8_t byte_value : data) {
      result = (result * 131U) + static_cast<size_t>(byte_value);
    }

    return result;
  }
};

}  // namespace std
