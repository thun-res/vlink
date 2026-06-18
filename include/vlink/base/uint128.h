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
 * @file uint128.h
 * @brief Portable 128-bit unsigned integer with native-fastpath multiplication.
 *
 * @details
 * @c vlink::Uint128 stores the value as a pair of @c uint64_t halves (@c high_, @c low_).
 * On GCC and Clang 64-bit targets the multiplication, division and modulo paths delegate
 * to the compiler-native @c __uint128_t for maximum performance; on platforms without
 * that builtin the portable fallback (@c mul_u128_fallback / @c u128_divmod) is used.
 *
 * Supported operator set:
 *
 * | Category   | Operators                              | Notes                                |
 * | ---------- | -------------------------------------- | ------------------------------------ |
 * | Arithmetic | + - * / % += -= *= /= %=               | Division throws on divisor @c 0      |
 * | Bitwise    | OR AND XOR NOT shift compound-assign   | Shifts clamp to @c [0, @c 128]       |
 * | Comparison | == != < > <= >=                        | Lexicographic over (high, low)       |
 * | Increment  | ++ -- (prefix and postfix)             | Carry/borrow crosses the 64-bit line |
 * | Stream     | operator<<(std::ostream&)              | Lowercase hexadecimal output         |
 *
 * @par Implicit construction from integral types
 * The single-argument constructor is intentionally non-explicit so integral literals can
 * flow into @c Uint128 transparently.  Signed source values are sign-extended, unsigned
 * values are zero-extended, and @c __uint128_t source values are split into halves.
 *
 * @par Conversion back to @c __uint128_t
 * An explicit @c operator @c __uint128_t() is provided on platforms that expose the type.
 *
 * @par std::hash specialisation
 * A @c std::hash<vlink::Uint128> specialisation at the bottom of this file enables use
 * inside @c std::unordered_map and @c std::unordered_set.
 *
 * @par Example
 * @code
 * vlink::Uint128 a(0, UINT64_MAX);
 * vlink::Uint128 b(1, 0);
 * vlink::Uint128 c = a + vlink::Uint128(0, 1);
 * assert(c == b);
 *
 * std::unordered_map<vlink::uint128_t, std::string> map;
 * map[vlink::Uint128(0xDEAD, 0xBEEF)] = "key";
 * @endcode
 */

#pragma once

#include <cstdint>
#include <iostream>
#include <type_traits>
#include <utility>

#include "./macros.h"

namespace vlink {

/**
 * @class Uint128
 * @brief 128-bit unsigned integer represented as two 64-bit halves with full operator support.
 *
 * @details
 * Logical layout is @c (high_ @c << @c 64) @c | @c low_.  All arithmetic propagates carry
 * and borrow across the 64-bit boundary so the observable behaviour matches a true
 * 128-bit unsigned integer.
 */
class Uint128 final {
 public:
  /**
   * @brief Default-constructs a zero-valued @c Uint128.
   */
  Uint128() noexcept = default;

  /**
   * @brief Constructs from an integral-like type @p T (implicit on purpose).
   *
   * @details
   * - When @c T is @c __uint128_t (where available) the source is split into halves.
   * - When @c T is signed the source is sign-extended: a negative value yields
   *   @c high_ @c = @c ~uint64_t{0} and @c low_ @c = the two's-complement bit pattern.
   * - When @c T is unsigned the source is zero-extended into @c low_ with @c high_ @c = @c 0.
   * - For any other @p T both halves keep their default value of @c 0; no diagnostic is
   *   emitted because integrality is intentionally not asserted here.
   *
   * @tparam T  Source type (integral or @c __uint128_t).
   * @param v   Source value.
   *
   * @note Non-explicit by design so integral literals interoperate naturally.
   */
  template <typename T>
  constexpr Uint128(T v) noexcept;  // NOLINT(google-explicit-constructor)

  /**
   * @brief Constructs from explicit high and low halves.
   *
   * @param high  Upper 64 bits.
   * @param low   Lower 64 bits.
   */
  explicit Uint128(uint64_t high, uint64_t low) noexcept;

#if defined(__SIZEOF_INT128__)
  /**
   * @brief Converts to the compiler-native @c __uint128_t type.
   *
   * @details
   * Only available where @c __uint128_t exists (GCC/Clang 64-bit targets).
   *
   * @return Reconstructed native 128-bit value.
   */
  explicit operator __uint128_t() const noexcept;
#endif

  /**
   * @brief Returns the sum of @c *this and @p other.
   *
   * @param other  Right operand.
   * @return New value equal to @c *this + @p other (wraps on overflow).
   */
  Uint128 operator+(const Uint128& other) const noexcept;

  /**
   * @brief Returns the difference of @c *this and @p other.
   *
   * @param other  Right operand.
   * @return New value equal to @c *this - @p other (wraps on underflow).
   */
  Uint128 operator-(const Uint128& other) const noexcept;

  /**
   * @brief Returns the product of @c *this and @p other.
   *
   * @details
   * Delegates to native @c __uint128_t multiplication when available; otherwise falls
   * back to @c mul_u128_fallback.
   *
   * @param other  Right operand.
   * @return Low 128 bits of the true product.
   */
  Uint128 operator*(const Uint128& other) const noexcept;

  /**
   * @brief Returns the quotient of @c *this divided by @p other.
   *
   * @param other  Divisor.
   * @return Quotient.
   *
   * @throws std::domain_error when @p other is zero.
   */
  Uint128 operator/(const Uint128& other) const;

  /**
   * @brief Returns the remainder of @c *this divided by @p other.
   *
   * @param other  Divisor.
   * @return Remainder.
   *
   * @throws std::domain_error when @p other is zero.
   */
  Uint128 operator%(const Uint128& other) const;

  /**
   * @brief Adds @p other to @c *this in-place with carry propagation.
   *
   * @param other  Right operand.
   * @return Reference to @c *this.
   */
  Uint128& operator+=(const Uint128& other) noexcept;

  /**
   * @brief Subtracts @p other from @c *this in-place with borrow propagation.
   *
   * @param other  Right operand.
   * @return Reference to @c *this.
   */
  Uint128& operator-=(const Uint128& other) noexcept;

  /**
   * @brief Multiplies @c *this by @p other in-place.
   *
   * @param other  Right operand.
   * @return Reference to @c *this.
   */
  Uint128& operator*=(const Uint128& other) noexcept;

  /**
   * @brief Divides @c *this by @p other in-place.
   *
   * @param other  Divisor.
   * @return Reference to @c *this.
   *
   * @throws std::domain_error when @p other is zero.
   */
  Uint128& operator/=(const Uint128& other);

  /**
   * @brief Computes @c *this modulo @p other in-place.
   *
   * @param other  Divisor.
   * @return Reference to @c *this.
   *
   * @throws std::domain_error when @p other is zero.
   */
  Uint128& operator%=(const Uint128& other);

  /**
   * @brief Returns the bitwise OR of @c *this and @p other.
   *
   * @param other  Right operand.
   * @return New value with each bit set if it is set in either operand.
   */
  Uint128 operator|(const Uint128& other) const noexcept;

  /**
   * @brief Returns the bitwise AND of @c *this and @p other.
   *
   * @param other  Right operand.
   * @return New value with each bit set only when set in both operands.
   */
  Uint128 operator&(const Uint128& other) const noexcept;

  /**
   * @brief Returns the bitwise XOR of @c *this and @p other.
   *
   * @param other  Right operand.
   * @return New value with each bit set when the bits differ between operands.
   */
  Uint128 operator^(const Uint128& other) const noexcept;

  /**
   * @brief Returns the bitwise complement of @c *this.
   *
   * @return New value with all 128 bits inverted.
   */
  Uint128 operator~() const noexcept;

  /**
   * @brief Returns @c *this shifted left by @p shift bits.
   *
   * @details
   * Shift values @c <= @c 0 leave the value unchanged; @c >= @c 128 produce zero; values
   * in @c [64, @c 127] move bits across the 64-bit boundary.
   *
   * @param shift  Bit positions to shift.
   * @return Shifted value.
   */
  Uint128 operator<<(int shift) const noexcept;

  /**
   * @brief Returns @c *this shifted right by @p shift bits (logical, zero-fill).
   *
   * @details
   * Same edge-case rules as @c operator<<.
   *
   * @param shift  Bit positions to shift.
   * @return Shifted value.
   */
  Uint128 operator>>(int shift) const noexcept;

  /**
   * @brief Applies bitwise OR with @p other in-place.
   *
   * @param other  Right operand.
   * @return Reference to @c *this.
   */
  Uint128& operator|=(const Uint128& other) noexcept;

  /**
   * @brief Applies bitwise AND with @p other in-place.
   *
   * @param other  Right operand.
   * @return Reference to @c *this.
   */
  Uint128& operator&=(const Uint128& other) noexcept;

  /**
   * @brief Applies bitwise XOR with @p other in-place.
   *
   * @param other  Right operand.
   * @return Reference to @c *this.
   */
  Uint128& operator^=(const Uint128& other) noexcept;

  /**
   * @brief Shifts @c *this left by @p shift bits in-place.
   *
   * @param shift  Bit positions to shift.
   * @return Reference to @c *this.
   */
  Uint128& operator<<=(int shift) noexcept;

  /**
   * @brief Shifts @c *this right by @p shift bits in-place (logical, zero-fill).
   *
   * @param shift  Bit positions to shift.
   * @return Reference to @c *this.
   */
  Uint128& operator>>=(int shift) noexcept;

  /**
   * @brief Returns @c true when both halves of @c *this and @p other are equal.
   *
   * @param other  Right operand.
   * @return Equality result.
   */
  [[nodiscard]] bool operator==(const Uint128& other) const noexcept;

  /**
   * @brief Returns @c true when any half differs.
   *
   * @param other  Right operand.
   * @return Inequality result.
   */
  [[nodiscard]] bool operator!=(const Uint128& other) const noexcept;

  /**
   * @brief Returns @c true when @c *this is strictly less than @p other.
   *
   * @details
   * Compares the high half first; ties are broken by the low half.
   *
   * @param other  Right operand.
   * @return Comparison result.
   */
  [[nodiscard]] bool operator<(const Uint128& other) const noexcept;

  /**
   * @brief Returns @c true when @c *this is strictly greater than @p other.
   *
   * @param other  Right operand.
   * @return Comparison result.
   */
  [[nodiscard]] bool operator>(const Uint128& other) const noexcept;

  /**
   * @brief Returns @c true when @c *this is less than or equal to @p other.
   *
   * @param other  Right operand.
   * @return Comparison result.
   */
  [[nodiscard]] bool operator<=(const Uint128& other) const noexcept;

  /**
   * @brief Returns @c true when @c *this is greater than or equal to @p other.
   *
   * @param other  Right operand.
   * @return Comparison result.
   */
  [[nodiscard]] bool operator>=(const Uint128& other) const noexcept;

  /**
   * @brief Pre-increment with carry across the 64-bit boundary.
   *
   * @return Reference to the incremented value.
   */
  Uint128& operator++() noexcept;

  /**
   * @brief Post-increment returning a copy of the value prior to incrementing.
   *
   * @return Pre-increment value.
   */
  Uint128 operator++(int) noexcept;

  /**
   * @brief Pre-decrement with borrow across the 64-bit boundary.
   *
   * @return Reference to the decremented value.
   */
  Uint128& operator--() noexcept;

  /**
   * @brief Post-decrement returning a copy of the value prior to decrementing.
   *
   * @return Pre-decrement value.
   */
  Uint128 operator--(int) noexcept;

  /**
   * @brief Returns the upper 64 bits of the stored value.
   *
   * @return High half.
   */
  [[nodiscard]] uint64_t get_high() const noexcept;

  /**
   * @brief Returns the lower 64 bits of the stored value.
   *
   * @return Low half.
   */
  [[nodiscard]] uint64_t get_low() const noexcept;

  /**
   * @brief Writes the lowercase hexadecimal representation of @p value to @p os.
   *
   * @param os     Output stream.
   * @param value  Value to print.
   * @return Reference to @p os.
   */
  VLINK_EXPORT friend std::ostream& operator<<(std::ostream& os, const Uint128& value) noexcept;

 private:
  VLINK_EXPORT static int clz64(uint64_t x) noexcept;

  VLINK_EXPORT static void mul_64_128(uint64_t a, uint64_t b, uint64_t& hi, uint64_t& lo) noexcept;

  VLINK_EXPORT static uint64_t add64_carry(uint64_t a, uint64_t b, uint64_t& carry_out) noexcept;

  VLINK_EXPORT static void add_128_with64(uint64_t& high, uint64_t& low, uint64_t add_low,
                                          uint64_t add_high = 0) noexcept;

  VLINK_EXPORT static Uint128 mul_u128_fallback(const Uint128& x, const Uint128& y) noexcept;

  VLINK_EXPORT static std::pair<Uint128, Uint128> u128_divmod(const Uint128& dividend, const Uint128& divisor);

  uint64_t high_{0};
  uint64_t low_{0};
};

/**
 * @brief Convenience alias matching the lowercase fixed-width style of the standard integer types.
 */
using uint128_t = Uint128;

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline Uint128::Uint128(uint64_t high, uint64_t low) noexcept : high_(high), low_(low) {}

#if defined(__SIZEOF_INT128__)
inline Uint128::operator __uint128_t() const noexcept { return (static_cast<__uint128_t>(high_) << 64) | low_; }
#endif

inline Uint128 Uint128::operator+(const Uint128& other) const noexcept {
  Uint128 r = *this;
  r += other;

  return r;
}

inline Uint128 Uint128::operator-(const Uint128& other) const noexcept {
  Uint128 r = *this;
  r -= other;

  return r;
}

inline Uint128 Uint128::operator*(const Uint128& other) const noexcept {
  Uint128 r = *this;
  r *= other;

  return r;
}

inline Uint128 Uint128::operator/(const Uint128& other) const { return u128_divmod(*this, other).first; }

inline Uint128 Uint128::operator%(const Uint128& other) const { return u128_divmod(*this, other).second; }

inline Uint128& Uint128::operator+=(const Uint128& other) noexcept {
  uint64_t old_low = low_;

  low_ += other.low_;

  uint64_t carry = (low_ < old_low) ? 1 : 0;

  high_ += other.high_ + carry;

  return *this;
}

inline Uint128& Uint128::operator-=(const Uint128& other) noexcept {
  uint64_t borrow = (low_ < other.low_) ? 1 : 0;

  low_ -= other.low_;
  high_ = high_ - other.high_ - borrow;

  return *this;
}

inline Uint128& Uint128::operator*=(const Uint128& other) noexcept {
#if defined(__SIZEOF_INT128__)
  __uint128_t lhs = (static_cast<__uint128_t>(high_) << 64) | low_;
  __uint128_t rhs = (static_cast<__uint128_t>(other.high_) << 64) | other.low_;
  __uint128_t res = lhs * rhs;

  high_ = static_cast<uint64_t>(res >> 64);
  low_ = static_cast<uint64_t>(res);
#else
  Uint128 r = mul_u128_fallback(*this, other);

  high_ = r.high_;
  low_ = r.low_;
#endif
  return *this;
}

inline Uint128& Uint128::operator/=(const Uint128& other) {
  auto qr = u128_divmod(*this, other);

  high_ = qr.first.high_;
  low_ = qr.first.low_;

  return *this;
}

inline Uint128& Uint128::operator%=(const Uint128& other) {
  auto qr = u128_divmod(*this, other);

  high_ = qr.second.high_;
  low_ = qr.second.low_;

  return *this;
}

inline Uint128 Uint128::operator|(const Uint128& other) const noexcept {
  return Uint128(high_ | other.high_, low_ | other.low_);
}

inline Uint128 Uint128::operator&(const Uint128& other) const noexcept {
  return Uint128(high_ & other.high_, low_ & other.low_);
}

inline Uint128 Uint128::operator^(const Uint128& other) const noexcept {
  return Uint128(high_ ^ other.high_, low_ ^ other.low_);
}

inline Uint128 Uint128::operator~() const noexcept { return Uint128(~high_, ~low_); }

inline Uint128 Uint128::operator<<(int shift) const noexcept {
  if (shift <= 0) {
    return *this;
  }

  if (shift >= 128) {
    return Uint128(0, 0);
  }

  if (shift >= 64) {
    return Uint128(low_ << (shift - 64), 0);
  }

  uint64_t new_high = (high_ << shift) | (low_ >> (64 - shift));
  uint64_t new_low = low_ << shift;

  return Uint128(new_high, new_low);
}

inline Uint128 Uint128::operator>>(int shift) const noexcept {
  if (shift <= 0) {
    return *this;
  }

  if (shift >= 128) {
    return Uint128(0, 0);
  }

  if (shift >= 64) {
    return Uint128(0, high_ >> (shift - 64));
  }

  uint64_t new_low = (low_ >> shift) | (high_ << (64 - shift));
  uint64_t new_high = high_ >> shift;

  return Uint128(new_high, new_low);
}

inline Uint128& Uint128::operator|=(const Uint128& other) noexcept {
  high_ |= other.high_;
  low_ |= other.low_;

  return *this;
}

inline Uint128& Uint128::operator&=(const Uint128& other) noexcept {
  high_ &= other.high_;
  low_ &= other.low_;

  return *this;
}

inline Uint128& Uint128::operator^=(const Uint128& other) noexcept {
  high_ ^= other.high_;
  low_ ^= other.low_;

  return *this;
}

inline Uint128& Uint128::operator<<=(int shift) noexcept {
  if (shift <= 0) {
    return *this;
  }

  if (shift >= 128) {
    high_ = 0;
    low_ = 0;

    return *this;
  }

  if (shift >= 64) {
    high_ = (low_ << (shift - 64));
    low_ = 0;
  } else {
    uint64_t nh = (high_ << shift) | (low_ >> (64 - shift));
    uint64_t nl = low_ << shift;

    high_ = nh;
    low_ = nl;
  }

  return *this;
}

inline Uint128& Uint128::operator>>=(int shift) noexcept {
  if (shift <= 0) {
    return *this;
  }

  if (shift >= 128) {
    high_ = 0;
    low_ = 0;
    return *this;
  }

  if (shift >= 64) {
    low_ = (high_ >> (shift - 64));
    high_ = 0;
  } else {
    uint64_t nl = (low_ >> shift) | (high_ << (64 - shift));
    uint64_t nh = high_ >> shift;

    high_ = nh;
    low_ = nl;
  }

  return *this;
}

inline bool Uint128::operator==(const Uint128& other) const noexcept {
  return high_ == other.high_ && low_ == other.low_;
}

inline bool Uint128::operator!=(const Uint128& other) const noexcept { return !(*this == other); }

inline bool Uint128::operator<(const Uint128& other) const noexcept {
  return (high_ < other.high_) || (high_ == other.high_ && low_ < other.low_);
}

inline bool Uint128::operator>(const Uint128& other) const noexcept { return other < *this; }

inline bool Uint128::operator<=(const Uint128& other) const noexcept { return !(other < *this); }

inline bool Uint128::operator>=(const Uint128& other) const noexcept { return !(*this < other); }

inline Uint128& Uint128::operator++() noexcept {
  uint64_t old_low = low_;

  low_ += 1;

  if (low_ < old_low) {
    ++high_;
  }

  return *this;
}

inline Uint128 Uint128::operator++(int) noexcept {
  Uint128 tmp = *this;

  ++(*this);

  return tmp;
}

inline Uint128& Uint128::operator--() noexcept {
  uint64_t old_low = low_;
  low_ -= 1;

  if (old_low == 0) {
    --high_;
  }

  return *this;
}

inline Uint128 Uint128::operator--(int) noexcept {
  Uint128 tmp = *this;

  --(*this);

  return tmp;
}

inline uint64_t Uint128::get_high() const noexcept { return high_; }

inline uint64_t Uint128::get_low() const noexcept { return low_; }

template <typename T>
inline constexpr Uint128::Uint128(T v) noexcept {
  // static_assert(std::is_integral_v<T>, "Uint128(T): T must be an integral type");

#if defined(__SIZEOF_INT128__)
  if constexpr (std::is_same_v<__uint128_t, T>) {
    high_ = static_cast<uint64_t>(v >> 64);
    low_ = static_cast<uint64_t>(v);

    return;
  }
#endif

  if constexpr (std::is_constructible_v<uint64_t, T>) {
    if constexpr (std::is_signed_v<T>) {
      high_ = (v < 0) ? ~uint64_t{0} : uint64_t{0};
      low_ = static_cast<uint64_t>(static_cast<int64_t>(v));
    } else {
      high_ = 0;
      low_ = v;
    }
  }
}

}  // namespace vlink

namespace std {

/**
 * @brief @c std::hash specialisation enabling @c vlink::Uint128 keys in unordered containers.
 *
 * @details
 * The hash combines both 64-bit halves so @c std::unordered_map, @c std::unordered_set and
 * similar containers can key on a @c vlink::Uint128 directly.
 */
template <>
struct hash<vlink::Uint128> {
  /**
   * @brief Hashes @p value into a @c size_t.
   *
   * @param value  Value to hash.
   * @return Hash derived from both halves.
   */
  VLINK_EXPORT size_t operator()(const vlink::Uint128& value) const noexcept;
};

}  // namespace std
