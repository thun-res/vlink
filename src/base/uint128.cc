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

#include "./base/uint128.h"

#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <utility>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace vlink {

std::ostream& operator<<(std::ostream& os, const Uint128& value) noexcept {
  std::ios_base::fmtflags f(os.flags());

  os << "0x" << std::uppercase << std::hex << std::setfill('0');

  if (value.high_ != 0) {
    os << value.high_ << std::setw(16) << value.low_;
  } else {
    os << value.low_;
  }

  os.flags(f);

  return os;
}

// Uint128
int Uint128::clz64(uint64_t x) noexcept {
#if defined(__GNUG__) || defined(__clang__)
  return x ? __builtin_clzll(x) : 64;
#elif defined(_MSC_VER)
  unsigned long idx;  // NOLINT(runtime/int, google-runtime-int)

  if (_BitScanReverse64(&idx, x)) {
    return 63 - static_cast<int>(idx);
  }

  return 64;
#else

  if (x == 0) {
    return 64;
  }

  int n = 0;

  while ((x & (1ull << 63)) == 0) {
    x <<= 1;
    ++n;
  }

  return n;
#endif
}

void Uint128::mul_64_128(uint64_t a, uint64_t b, uint64_t& hi, uint64_t& lo) noexcept {
#if defined(__SIZEOF_INT128__)
  __uint128_t p = static_cast<__uint128_t>(a) * static_cast<__uint128_t>(b);

  hi = static_cast<uint64_t>(p >> 64);
  lo = static_cast<uint64_t>(p);
#else
  const uint64_t a0 = static_cast<uint32_t>(a);
  const uint64_t a1 = a >> 32;
  const uint64_t b0 = static_cast<uint32_t>(b);
  const uint64_t b1 = b >> 32;

  const uint64_t p00 = a0 * b0;
  const uint64_t p01 = a0 * b1;
  const uint64_t p10 = a1 * b0;
  const uint64_t p11 = a1 * b1;

  uint64_t carry = 0;
  uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFULL) + (p10 & 0xFFFFFFFFULL);
  lo = (p00 & 0xFFFFFFFFULL) | (mid << 32);
  carry = (mid >> 32);

  hi = p11 + (p01 >> 32) + (p10 >> 32) + carry;
#endif
}

uint64_t Uint128::add64_carry(uint64_t a, uint64_t b, uint64_t& carry_out) noexcept {
  uint64_t s = a + b;

  carry_out = (s < a) ? 1 : 0;

  return s;
}

void Uint128::add_128_with64(uint64_t& high, uint64_t& low, uint64_t add_low, uint64_t add_high) noexcept {
  uint64_t c = 0;

  low = add64_carry(low, add_low, c);

  high = high + add_high + c;
}

Uint128 Uint128::mul_u128_fallback(const Uint128& x, const Uint128& y) noexcept {
  uint64_t xh = x.get_high();
  uint64_t xl = x.get_low();
  uint64_t yh = y.get_high();
  uint64_t yl = y.get_low();

  uint64_t p0_hi = 0;
  uint64_t p0_lo = 0;
  mul_64_128(xl, yl, p0_hi, p0_lo);

  uint64_t p1_hi = 0;
  uint64_t p1_lo = 0;
  mul_64_128(xl, yh, p1_hi, p1_lo);

  uint64_t p2_hi = 0;
  uint64_t p2_lo = 0;
  mul_64_128(xh, yl, p2_hi, p2_lo);

  uint64_t p3_hi = 0;
  uint64_t p3_lo = 0;
  mul_64_128(xh, yh, p3_hi, p3_lo);

  uint64_t low = p0_lo;
  uint64_t high = p0_hi;

  uint64_t c1 = 0;
  high = add64_carry(high, p1_lo, c1);
  uint64_t c2 = 0;
  high = add64_carry(high, p2_lo, c2);

  (void)p1_hi;
  (void)p2_hi;
  (void)p3_hi;
  (void)c1;
  (void)c2;

  return Uint128(high, low);
}

std::pair<Uint128, Uint128> Uint128::u128_divmod(const Uint128& dividend, const Uint128& divisor) {
  if VUNLIKELY (divisor.get_high() == 0 && divisor.get_low() == 0) {
    throw std::domain_error("Uint128 division by zero");
  }

#if defined(__SIZEOF_INT128__)
  __uint128_t a = (static_cast<__uint128_t>(dividend.get_high()) << 64) | dividend.get_low();
  __uint128_t b = (static_cast<__uint128_t>(divisor.get_high()) << 64) | divisor.get_low();

  __uint128_t q = a / b;
  __uint128_t r = a % b;

  return {Uint128(static_cast<uint64_t>(q >> 64), static_cast<uint64_t>(q)),
          Uint128(static_cast<uint64_t>(r >> 64), static_cast<uint64_t>(r))};
#else
  Uint128 zero{0};

  if (dividend < divisor) {
    return {zero, dividend};
  }

  if (dividend == divisor) {
    return {Uint128{1}, zero};
  }

  Uint128 quotient{0};
  Uint128 remainder{0};

  for (int i = 127; i >= 0; --i) {
    remainder <<= 1;
    Uint128 bit_mask = (i >= 64) ? Uint128{1ULL << (i - 64), 0} : Uint128{0, 1ULL << i};

    if ((dividend & bit_mask) != zero) {
      remainder |= Uint128{1};
    }

    if (remainder >= divisor) {
      remainder -= divisor;
      quotient |= bit_mask;
    }
  }

  return {quotient, remainder};
#endif
}

}  // namespace vlink

namespace std {
size_t hash<vlink::Uint128>::operator()(const vlink::Uint128& value) const noexcept {
  uint64_t h = value.get_high();
  uint64_t l = value.get_low();

  h ^= l;
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;

  return static_cast<size_t>(h);
}
}  // namespace std
