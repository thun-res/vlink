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

// NOLINTBEGIN

#include "./base/uint128.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <sstream>
#include <unordered_map>

#include "../common_test.h"

TEST_SUITE("base-Uint128") {
  TEST_CASE("default construction yields zero in both words") {
    Uint128 v;

    CHECK_EQ(v.get_high(), 0u);
    CHECK_EQ(v.get_low(), 0u);
  }

  TEST_CASE("construct from unsigned integral zero-extends into low word") {
    SUBCASE("uint64_t") {
      Uint128 v(static_cast<uint64_t>(0xDEADBEEFu));
      CHECK_EQ(v.get_high(), 0u);
      CHECK_EQ(v.get_low(), 0xDEADBEEFu);
    }

    SUBCASE("int literal") {
      Uint128 v(42);
      CHECK_EQ(v.get_high(), 0u);
      CHECK_EQ(v.get_low(), 42u);
    }

    SUBCASE("zero") {
      Uint128 v(0);
      CHECK_EQ(v.get_high(), 0u);
      CHECK_EQ(v.get_low(), 0u);
    }
  }

  TEST_CASE("construct from explicit high and low halves stores both words") {
    Uint128 v(0xABCDu, 0x1234u);

    CHECK_EQ(v.get_high(), 0xABCDu);
    CHECK_EQ(v.get_low(), 0x1234u);
  }

  TEST_CASE("construct with UINT64_MAX in each word") {
    Uint128 low_max(0u, UINT64_MAX);
    CHECK_EQ(low_max.get_high(), 0u);
    CHECK_EQ(low_max.get_low(), UINT64_MAX);

    Uint128 high_max(UINT64_MAX, 0u);
    CHECK_EQ(high_max.get_high(), UINT64_MAX);
    CHECK_EQ(high_max.get_low(), 0u);
  }

  TEST_CASE("uint128_t alias is the same type as Uint128") {
    uint128_t v(0u, 99u);

    CHECK_EQ(v.get_low(), 99u);
  }

  TEST_CASE("equality and inequality operators work correctly") {
    Uint128 a(1u, 2u);
    Uint128 b(1u, 2u);
    Uint128 c(0u, 2u);
    Uint128 d(1u, 0u);

    CHECK(a == b);
    CHECK_FALSE(a != b);
    CHECK(a != c);
    CHECK(a != d);
    CHECK_FALSE(a == c);
  }

  TEST_CASE("less-than compares high word first then low word") {
    CHECK(Uint128(0u, 1u) < Uint128(0u, 2u));
    CHECK_FALSE(Uint128(0u, 2u) < Uint128(0u, 1u));
    CHECK(Uint128(1u, UINT64_MAX) < Uint128(2u, 0u));
    CHECK_FALSE(Uint128(0u, 1u) < Uint128(0u, 1u));
  }

  TEST_CASE("greater-than less-equal greater-equal are consistent") {
    Uint128 a(0u, 10u);
    Uint128 b(0u, 5u);
    Uint128 c(0u, 5u);

    CHECK(a > b);
    CHECK_FALSE(b > a);
    CHECK(b <= c);
    CHECK(b >= c);
    CHECK_FALSE(a <= b);
    CHECK(a >= b);
  }

  TEST_CASE("zero equals zero and all comparisons are consistent") {
    Uint128 zero;

    CHECK(zero == Uint128{});
    CHECK_FALSE(zero != Uint128{});
    CHECK_FALSE(zero < Uint128{});
    CHECK_FALSE(zero > Uint128{});
    CHECK(zero <= Uint128{});
    CHECK(zero >= Uint128{});
  }

  TEST_CASE("max value compares greater than all others") {
    Uint128 max_val(UINT64_MAX, UINT64_MAX);
    Uint128 almost_max(UINT64_MAX, UINT64_MAX - 1u);

    CHECK(max_val > almost_max);
    CHECK(almost_max < max_val);
    CHECK_FALSE(max_val < almost_max);
  }

  TEST_CASE("addition without carry produces correct sum") {
    Uint128 a(0u, 10u);
    Uint128 b(0u, 20u);
    Uint128 c = a + b;

    CHECK_EQ(c.get_high(), 0u);
    CHECK_EQ(c.get_low(), 30u);
  }

  TEST_CASE("addition carry propagates from low to high word") {
    Uint128 a(0u, UINT64_MAX);
    Uint128 b(0u, 1u);
    Uint128 c = a + b;

    CHECK_EQ(c.get_high(), 1u);
    CHECK_EQ(c.get_low(), 0u);
  }

  TEST_CASE("addition wraps at 128-bit overflow") {
    Uint128 max(UINT64_MAX, UINT64_MAX);
    Uint128 result = max + Uint128(0u, 1u);

    CHECK_EQ(result, Uint128(0u, 0u));
  }

  TEST_CASE("operator+= modifies value in place") {
    Uint128 a(0u, 5u);
    a += Uint128(0u, 3u);

    CHECK_EQ(a.get_low(), 8u);
    CHECK_EQ(a.get_high(), 0u);
  }

  TEST_CASE("subtraction without borrow produces correct difference") {
    Uint128 a(0u, 20u);
    Uint128 b(0u, 7u);
    Uint128 c = a - b;

    CHECK_EQ(c.get_low(), 13u);
    CHECK_EQ(c.get_high(), 0u);
  }

  TEST_CASE("subtraction borrow propagates from high to low word") {
    Uint128 a(1u, 0u);
    Uint128 b(0u, 1u);
    Uint128 c = a - b;

    CHECK_EQ(c.get_high(), 0u);
    CHECK_EQ(c.get_low(), UINT64_MAX);
  }

  TEST_CASE("subtraction wraps at 128-bit underflow") {
    Uint128 zero;
    Uint128 result = zero - Uint128(0u, 1u);

    CHECK_EQ(result.get_high(), UINT64_MAX);
    CHECK_EQ(result.get_low(), UINT64_MAX);
  }

  TEST_CASE("operator-= modifies value in place") {
    Uint128 a(0u, 10u);
    a -= Uint128(0u, 4u);

    CHECK_EQ(a.get_low(), 6u);
  }

  TEST_CASE("multiplication by zero yields zero") {
    Uint128 a(1u, 100u);
    CHECK_EQ(a * Uint128(0u, 0u), Uint128(0u, 0u));
  }

  TEST_CASE("multiplication by one is identity") {
    Uint128 a(2u, 7u);
    CHECK_EQ(a * Uint128(0u, 1u), a);
  }

  TEST_CASE("simple multiplication produces correct product") {
    Uint128 a(0u, 6u);
    Uint128 b(0u, 7u);
    Uint128 c = a * b;

    CHECK_EQ(c.get_low(), 42u);
    CHECK_EQ(c.get_high(), 0u);
  }

  TEST_CASE("multiplication with carry into high word") {
    Uint128 a(0u, UINT64_MAX);
    Uint128 two(0u, 2u);
    Uint128 result = a * two;

    CHECK_EQ(result.get_high(), 1u);
    CHECK_EQ(result.get_low(), UINT64_MAX - 1u);
  }

  TEST_CASE("operator*= modifies value in place") {
    Uint128 a(0u, 3u);
    a *= Uint128(0u, 4u);

    CHECK_EQ(a.get_low(), 12u);
  }

  TEST_CASE("division and modulo produce correct quotient and remainder") {
    SUBCASE("exact division") {
      Uint128 a(0u, 81u);
      Uint128 b(0u, 9u);
      CHECK_EQ(a / b, Uint128(0u, 9u));
      CHECK_EQ(a % b, Uint128(0u, 0u));
    }

    SUBCASE("division with remainder") {
      Uint128 a(0u, 17u);
      Uint128 b(0u, 5u);
      CHECK_EQ(a / b, Uint128(0u, 3u));
      CHECK_EQ(a % b, Uint128(0u, 2u));
    }

    SUBCASE("large numerator 2^64 divided by 2") {
      Uint128 a(1u, 0u);
      Uint128 b(0u, 2u);
      CHECK_EQ(a / b, Uint128(0u, static_cast<uint64_t>(1u) << 63));
    }
  }

  TEST_CASE("division and modulo by zero throw") {
    Uint128 a(0u, 10u);
    Uint128 zero;

    CHECK_THROWS(a / zero);
    CHECK_THROWS(a % zero);
    CHECK_THROWS(a /= zero);
    CHECK_THROWS(a %= zero);
  }

  TEST_CASE("in-place division and modulo modify value correctly") {
    Uint128 a(0u, 100u);
    a /= Uint128(0u, 4u);
    CHECK_EQ(a, Uint128(0u, 25u));

    Uint128 b(0u, 17u);
    b %= Uint128(0u, 5u);
    CHECK_EQ(b, Uint128(0u, 2u));
  }

  TEST_CASE("bitwise OR AND XOR NOT produce correct results") {
    SUBCASE("OR") { CHECK_EQ(Uint128(0u, 0b1010u) | Uint128(0u, 0b0101u), Uint128(0u, 0b1111u)); }

    SUBCASE("AND") { CHECK_EQ(Uint128(0u, 0b1110u) & Uint128(0u, 0b0110u), Uint128(0u, 0b0110u)); }

    SUBCASE("XOR") { CHECK_EQ(Uint128(0u, 0b1100u) ^ Uint128(0u, 0b1010u), Uint128(0u, 0b0110u)); }

    SUBCASE("NOT") {
      Uint128 all_ones = ~Uint128(0u, 0u);
      CHECK_EQ(all_ones.get_high(), UINT64_MAX);
      CHECK_EQ(all_ones.get_low(), UINT64_MAX);
    }
  }

  TEST_CASE("in-place bitwise operators modify value correctly") {
    Uint128 a(0u, 0b1000u);
    a |= Uint128(0u, 0b0111u);
    CHECK_EQ(a, Uint128(0u, 0b1111u));

    Uint128 b(0u, 0b1111u);
    b &= Uint128(0u, 0b0101u);
    CHECK_EQ(b, Uint128(0u, 0b0101u));

    Uint128 c(0u, 0b1100u);
    c ^= Uint128(0u, 0b1010u);
    CHECK_EQ(c, Uint128(0u, 0b0110u));
  }

  TEST_CASE("left shift by various amounts is correct") {
    SUBCASE("shift by 1") {
      Uint128 a(0u, 1u);
      CHECK_EQ(a << 1, Uint128(0u, 2u));
    }

    SUBCASE("shift crossing 64-bit boundary") {
      Uint128 a(0u, 1u);
      Uint128 b = a << 64;
      CHECK_EQ(b.get_high(), 1u);
      CHECK_EQ(b.get_low(), 0u);
    }

    SUBCASE("shift by 128 yields zero") { CHECK_EQ(Uint128(1u, 1u) << 128, Uint128(0u, 0u)); }

    SUBCASE("shift by 0 is no-op") {
      Uint128 a(3u, 5u);
      CHECK_EQ(a << 0, a);
    }
  }

  TEST_CASE("right shift by various amounts is correct") {
    SUBCASE("shift crossing 64-bit boundary") {
      Uint128 a(1u, 0u);
      Uint128 b = a >> 64;
      CHECK_EQ(b.get_high(), 0u);
      CHECK_EQ(b.get_low(), 1u);
    }

    SUBCASE("shift by 128 yields zero") { CHECK_EQ(Uint128(1u, 1u) >> 128, Uint128(0u, 0u)); }

    SUBCASE("shift by 0 is no-op") {
      Uint128 a(3u, 5u);
      CHECK_EQ(a >> 0, a);
    }
  }

  TEST_CASE("in-place shift operators are consistent with non-in-place") {
    Uint128 a(0u, 1u);
    a <<= 64;
    CHECK_EQ(a.get_high(), 1u);
    CHECK_EQ(a.get_low(), 0u);

    a >>= 64;
    CHECK_EQ(a.get_high(), 0u);
    CHECK_EQ(a.get_low(), 1u);

    Uint128 b(1u, 1u);
    b <<= 128;
    CHECK_EQ(b, Uint128(0u, 0u));

    Uint128 c(1u, 1u);
    c >>= 128;
    CHECK_EQ(c, Uint128(0u, 0u));
  }

  TEST_CASE("pre-increment increments by one with carry propagation") {
    Uint128 a(0u, 41u);
    ++a;
    CHECK_EQ(a.get_low(), 42u);
    CHECK_EQ(a.get_high(), 0u);

    Uint128 carry(0u, UINT64_MAX);
    ++carry;
    CHECK_EQ(carry.get_high(), 1u);
    CHECK_EQ(carry.get_low(), 0u);
  }

  TEST_CASE("post-increment returns previous value and increments in place") {
    Uint128 a(0u, 5u);
    Uint128 old = a++;

    CHECK_EQ(old.get_low(), 5u);
    CHECK_EQ(a.get_low(), 6u);
  }

  TEST_CASE("pre-decrement decrements by one with borrow propagation") {
    Uint128 a(0u, 10u);
    --a;
    CHECK_EQ(a.get_low(), 9u);

    Uint128 borrow(1u, 0u);
    --borrow;
    CHECK_EQ(borrow.get_high(), 0u);
    CHECK_EQ(borrow.get_low(), UINT64_MAX);
  }

  TEST_CASE("post-decrement returns previous value and decrements in place") {
    Uint128 a(0u, 8u);
    Uint128 old = a--;

    CHECK_EQ(old.get_low(), 8u);
    CHECK_EQ(a.get_low(), 7u);
  }

  TEST_CASE("stream output produces hexadecimal representation") {
    SUBCASE("zero") {
      Uint128 v(0u, 0u);
      std::ostringstream oss;
      oss << v;
      CHECK_EQ(oss.str(), "0x0");
    }

    SUBCASE("small value") {
      Uint128 v(0u, 42u);
      std::ostringstream oss;
      oss << v;
      CHECK_EQ(oss.str(), "0x2A");
    }
  }

  TEST_CASE("std::hash specialisation enables use as unordered_map key") {
    std::hash<Uint128> h;
    Uint128 a(1u, 2u);
    Uint128 b(1u, 2u);

    CHECK_EQ(h(a), h(b));

    std::unordered_map<Uint128, std::string> map;
    map[Uint128(0xDEADu, 0xBEEFu)] = "entry";

    CHECK_EQ(map[Uint128(0xDEADu, 0xBEEFu)], "entry");
  }

  TEST_CASE("std::hash is stable for the same value") {
    std::hash<Uint128> h;
    Uint128 z;

    CHECK_EQ(h(z), h(z));
  }
}

// NOLINTEND
