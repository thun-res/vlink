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

#include "./base/uuid.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../common_test.h"

static_assert(Uuid{}.is_nil(), "default-constructed Uuid must be nil at compile time");
static_assert(Uuid{}.variant() == Uuid::Variant::kNcs, "nil UUID must report NCS variant at compile time");
static_assert(Uuid{}.version() == Uuid::Version::kNone, "nil UUID must report no version at compile time");
static_assert(Uuid::kByteSize == 16U, "RFC 4122 UUIDs are 16 bytes");
static_assert(Uuid::kStringSize == 36U, "canonical UUID string is 36 characters");

namespace {

constexpr std::array<uint8_t, 16> kSampleBytes{
    0x47, 0xac, 0x10, 0xb8, 0x58, 0xcc, 0x4a, 0x3c, 0x8c, 0x5b, 0x0e, 0x77, 0x88, 0x99, 0xaa, 0xbb,
};

constexpr Uuid kSampleUuid{kSampleBytes};

static_assert(!kSampleUuid.is_nil(), "non-zero bytes must not be nil at compile time");
static_assert(kSampleUuid.bytes()[0] == 0x47U, "bytes() must be constexpr-accessible");
static_assert(kSampleUuid.bytes()[15] == 0xbbU, "bytes()[15] must be constexpr-accessible");

}  // namespace

TEST_SUITE("base-Uuid") {
  TEST_CASE("default constructed uuid is nil and serialises to all-zero string") {
    Uuid id;

    CHECK(id.is_nil());
    CHECK(id.to_string() == "00000000-0000-0000-0000-000000000000");
    CHECK(id.to_compact_string() == "00000000000000000000000000000000");
  }

  TEST_CASE("nil uuid reports ncs variant and no version") {
    Uuid id;

    CHECK(id.variant() == Uuid::Variant::kNcs);
    CHECK(id.version() == Uuid::Version::kNone);
  }

  TEST_CASE("constructs from std::array and preserves all bytes") {
    Uuid id{kSampleBytes};

    CHECK_FALSE(id.is_nil());
    CHECK(id.bytes() == kSampleBytes);
  }

  TEST_CASE("constructs from raw c array and preserves all bytes") {
    uint8_t raw[16] = {0x47, 0xac, 0x10, 0xb8, 0x58, 0xcc, 0x4a, 0x3c, 0x8c, 0x5b, 0x0e, 0x77, 0x88, 0x99, 0xaa, 0xbb};
    Uuid id{raw};

    for (size_t i = 0; i < 16; ++i) {
      CHECK(id.bytes()[i] == raw[i]);
    }
  }

  TEST_CASE("constructs from iterator range of exactly 16 bytes") {
    std::vector<uint8_t> src(16, 0xa5);
    Uuid id{src.begin(), src.end()};

    for (size_t i = 0; i < 16; ++i) {
      CHECK(id.bytes()[i] == 0xa5U);
    }
  }

  TEST_CASE("iterator range with fewer than 16 bytes leaves nil state") {
    std::vector<uint8_t> too_short(8, 0xff);
    Uuid id{too_short.begin(), too_short.end()};

    CHECK(id.is_nil());
  }

  TEST_CASE("iterator range with more than 16 bytes leaves nil state") {
    std::vector<uint8_t> too_long(32, 0xff);
    Uuid id{too_long.begin(), too_long.end()};

    CHECK(id.is_nil());
  }

  TEST_CASE("iterator range accepts raw pointer pair") {
    const uint8_t buffer[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    Uuid id{std::begin(buffer), std::end(buffer)};

    CHECK(id.bytes()[0] == 1U);
    CHECK(id.bytes()[15] == 16U);
  }

  TEST_CASE("copy and move preserve the payload") {
    std::array<uint8_t, 16> data{};
    data[5] = 0xcd;
    Uuid src{data};

    Uuid copied = src;
    Uuid moved = std::move(src);

    CHECK(copied.bytes()[5] == 0xcdU);
    CHECK(moved.bytes()[5] == 0xcdU);
  }

  TEST_CASE("variant detection covers all four cases") {
    std::array<uint8_t, 16> data{};

    SUBCASE("ncs variant for bit pattern 0xxx") {
      for (uint8_t octet : {uint8_t{0x00}, uint8_t{0x40}, uint8_t{0x7f}}) {
        data[8] = octet;
        CHECK(Uuid{data}.variant() == Uuid::Variant::kNcs);
      }
    }

    SUBCASE("rfc variant for bit pattern 10xx") {
      for (uint8_t octet : {uint8_t{0x80}, uint8_t{0xbf}}) {
        data[8] = octet;
        CHECK(Uuid{data}.variant() == Uuid::Variant::kRfc);
      }
    }

    SUBCASE("microsoft variant for bit pattern 110x") {
      for (uint8_t octet : {uint8_t{0xc0}, uint8_t{0xdf}}) {
        data[8] = octet;
        CHECK(Uuid{data}.variant() == Uuid::Variant::kMicrosoft);
      }
    }

    SUBCASE("reserved variant for bit pattern 111x") {
      for (uint8_t octet : {uint8_t{0xe0}, uint8_t{0xff}}) {
        data[8] = octet;
        CHECK(Uuid{data}.variant() == Uuid::Variant::kReserved);
      }
    }
  }

  TEST_CASE("version detection covers all rfc version values") {
    std::array<uint8_t, 16> data{};

    data[6] = 0x10;
    CHECK(Uuid{data}.version() == Uuid::Version::kTimeBased);

    data[6] = 0x20;
    CHECK(Uuid{data}.version() == Uuid::Version::kDceSecurity);

    data[6] = 0x30;
    CHECK(Uuid{data}.version() == Uuid::Version::kNameBasedMd5);

    data[6] = 0x40;
    CHECK(Uuid{data}.version() == Uuid::Version::kRandomBased);

    data[6] = 0x50;
    CHECK(Uuid{data}.version() == Uuid::Version::kNameBasedSha1);

    data[6] = 0x70;
    CHECK(Uuid{data}.version() == Uuid::Version::kNone);

    data[6] = 0xf0;
    CHECK(Uuid{data}.version() == Uuid::Version::kNone);
  }

  TEST_CASE("to_string produces canonical 36-character hyphenated lowercase hex") {
    Uuid id{kSampleBytes};
    const auto str = id.to_string();

    CHECK(str.size() == 36U);
    CHECK(str == "47ac10b8-58cc-4a3c-8c5b-0e778899aabb");
    CHECK(str[8] == '-');
    CHECK(str[13] == '-');
    CHECK(str[18] == '-');
    CHECK(str[23] == '-');
  }

  TEST_CASE("to_string of nil is all zeros with hyphens") {
    Uuid id;

    CHECK(id.to_string() == "00000000-0000-0000-0000-000000000000");
  }

  TEST_CASE("to_compact_string produces 32-character lowercase hex without hyphens") {
    Uuid id{kSampleBytes};
    const auto str = id.to_compact_string();

    CHECK(str.size() == 32U);
    CHECK(str == "47ac10b858cc4a3c8c5b0e778899aabb");
    CHECK(str.find('-') == std::string::npos);
  }

  TEST_CASE("to_compact_string of nil is 32 zeros") {
    Uuid id;

    CHECK(id.to_compact_string() == "00000000000000000000000000000000");
  }

  TEST_CASE("stream insertion matches to_string") {
    Uuid id{kSampleBytes};
    std::ostringstream oss;
    oss << id;

    CHECK(oss.str() == id.to_string());
  }

  TEST_CASE("from_string round-trips canonical form") {
    const std::string text = "47ac10b8-58cc-4a3c-8c5b-0e778899aabb";
    auto parsed = Uuid::from_string(text);

    REQUIRE(parsed.has_value());
    CHECK(parsed->to_string() == text);
  }

  TEST_CASE("from_string accepts uppercase hex and normalises to lowercase") {
    auto parsed = Uuid::from_string("47AC10B8-58CC-4A3C-8C5B-0E778899AABB");

    REQUIRE(parsed.has_value());
    CHECK(parsed->to_string() == "47ac10b8-58cc-4a3c-8c5b-0e778899aabb");
  }

  TEST_CASE("from_string accepts mixed-case hex digits") {
    auto parsed = Uuid::from_string("47Ac10B8-58cc-4A3c-8C5b-0e778899AaBb");

    REQUIRE(parsed.has_value());
    CHECK(parsed->to_string() == "47ac10b8-58cc-4a3c-8c5b-0e778899aabb");
  }

  TEST_CASE("from_string accepts braced canonical form") {
    auto parsed = Uuid::from_string("{47ac10b8-58cc-4a3c-8c5b-0e778899aabb}");

    REQUIRE(parsed.has_value());
    CHECK(parsed->to_string() == "47ac10b8-58cc-4a3c-8c5b-0e778899aabb");
  }

  TEST_CASE("from_string accepts 32-character compact form without hyphens") {
    auto parsed = Uuid::from_string("47ac10b858cc4a3c8c5b0e778899aabb");

    REQUIRE(parsed.has_value());
    CHECK(parsed->to_compact_string() == "47ac10b858cc4a3c8c5b0e778899aabb");
  }

  TEST_CASE("from_string accepts braced compact form") {
    auto parsed = Uuid::from_string("{47ac10b858cc4a3c8c5b0e778899aabb}");

    REQUIRE(parsed.has_value());
    CHECK(parsed->to_compact_string() == "47ac10b858cc4a3c8c5b0e778899aabb");
  }

  TEST_CASE("from_string is permissive about hyphen positions in 36-char input") {
    auto parsed = Uuid::from_string("47-ac10b858cc-4a3c-8c5b-0e7788-99aabb");

    REQUIRE(parsed.has_value());
    CHECK(parsed->to_compact_string() == "47ac10b858cc4a3c8c5b0e778899aabb");
  }

  TEST_CASE("from_string rejects various malformed inputs") {
    SUBCASE("empty string") { CHECK_FALSE(Uuid::from_string("").has_value()); }

    SUBCASE("single character") { CHECK_FALSE(Uuid::from_string("0").has_value()); }

    SUBCASE("empty braces") { CHECK_FALSE(Uuid::from_string("{}").has_value()); }

    SUBCASE("too short hex") { CHECK_FALSE(Uuid::from_string("47ac10b8").has_value()); }

    SUBCASE("31 hex chars, one short") {
      CHECK_FALSE(Uuid::from_string("47ac10b8-58cc-4a3c-8c5b-0e778899aab").has_value());
    }

    SUBCASE("33 hex chars, one extra") {
      CHECK_FALSE(Uuid::from_string("47ac10b858cc4a3c8c5b0e778899aabb0").has_value());
    }

    SUBCASE("non-hex character") { CHECK_FALSE(Uuid::from_string("47ac10b8-58cc-4a3c-8c5b-0e778899aabZ").has_value()); }

    SUBCASE("embedded whitespace") {
      CHECK_FALSE(Uuid::from_string("47ac10b8 58cc 4a3c 8c5b 0e778899aabb").has_value());
    }

    SUBCASE("missing closing brace") {
      CHECK_FALSE(Uuid::from_string("{47ac10b8-58cc-4a3c-8c5b-0e778899aabb").has_value());
    }

    SUBCASE("all hyphens") { CHECK_FALSE(Uuid::from_string("--------------------------------").has_value()); }
  }

  TEST_CASE("is_valid agrees with from_string for good inputs") {
    CHECK(Uuid::is_valid("47ac10b8-58cc-4a3c-8c5b-0e778899aabb"));
    CHECK(Uuid::is_valid("{47ac10b8-58cc-4a3c-8c5b-0e778899aabb}"));
    CHECK(Uuid::is_valid("47ac10b858cc4a3c8c5b0e778899aabb"));
  }

  TEST_CASE("is_valid rejects malformed inputs") {
    CHECK_FALSE(Uuid::is_valid(""));
    CHECK_FALSE(Uuid::is_valid("not-a-uuid"));
    CHECK_FALSE(Uuid::is_valid("47ac10b8-58cc-4a3c-8c5b-0e778899aabZ"));
    CHECK_FALSE(Uuid::is_valid("{}"));
  }

  TEST_CASE("from_string nullptr overload returns nullopt") {
    const char* null_ptr = nullptr;

    CHECK_FALSE(Uuid::from_string(null_ptr).has_value());
  }

  TEST_CASE("is_valid nullptr overload returns false") {
    const char* null_ptr = nullptr;

    CHECK_FALSE(Uuid::is_valid(null_ptr));
  }

  TEST_CASE("from_string c-string overload stops at first nul byte") {
    const char buffer[] = "47ac10b8-58cc-4a3c-8c5b-0e778899aabb\0junk-trailing";
    auto parsed = Uuid::from_string(static_cast<const char*>(buffer));

    REQUIRE(parsed.has_value());
    CHECK(parsed->to_string() == "47ac10b8-58cc-4a3c-8c5b-0e778899aabb");
  }

  TEST_CASE("is_valid c-string overload accepts canonical form") {
    CHECK(Uuid::is_valid("47ac10b8-58cc-4a3c-8c5b-0e778899aabb"));
  }

  TEST_CASE("equality and inequality operators") {
    Uuid a;
    Uuid b;
    std::array<uint8_t, 16> data{};
    data[0] = 1;
    Uuid c{data};

    CHECK(a == b);
    CHECK_FALSE(a != b);
    CHECK(a != c);
    CHECK_FALSE(a == c);
  }

  TEST_CASE("less-than operator uses lexicographic byte order") {
    std::array<uint8_t, 16> low{};
    std::array<uint8_t, 16> high{};
    high[0] = 1;

    Uuid lo{low};
    Uuid hi{high};

    CHECK(lo < hi);
    CHECK_FALSE(hi < lo);
    CHECK_FALSE(lo < lo);
  }

  TEST_CASE("ordering compares from byte index zero first") {
    std::array<uint8_t, 16> first{};
    std::array<uint8_t, 16> second{};
    first[15] = 0xff;
    second[0] = 0x01;

    CHECK(Uuid{first} < Uuid{second});
  }

  TEST_CASE("swap exchanges payloads") {
    std::array<uint8_t, 16> data{};
    data[0] = 0x11;
    Uuid a{data};
    Uuid b;

    a.swap(b);

    CHECK(a.is_nil());
    CHECK(b.bytes()[0] == 0x11U);
  }

  TEST_CASE("std::swap works on uuid") {
    std::array<uint8_t, 16> data{};
    data[0] = 0x77;
    Uuid a{data};
    Uuid b;

    std::swap(a, b);

    CHECK(a.is_nil());
    CHECK(b.bytes()[0] == 0x77U);
  }

  TEST_CASE("generate_random produces rfc variant and v4 version") {
    Uuid id = Uuid::generate_random();

    CHECK(id.variant() == Uuid::Variant::kRfc);
    CHECK(id.version() == Uuid::Version::kRandomBased);
    CHECK_FALSE(id.is_nil());
  }

  TEST_CASE("two consecutive generate_random calls yield different uuids") {
    Uuid a = Uuid::generate_random();
    Uuid b = Uuid::generate_random();

    CHECK(a != b);
  }

  TEST_CASE("generate_random with supplied engine is deterministic for fixed seed") {
    std::mt19937 engine_a(0xdeadbeef);
    std::mt19937 engine_b(0xdeadbeef);

    Uuid a = Uuid::generate_random(engine_a);
    Uuid b = Uuid::generate_random(engine_b);

    CHECK(a == b);
    CHECK(a.variant() == Uuid::Variant::kRfc);
    CHECK(a.version() == Uuid::Version::kRandomBased);
  }

  TEST_CASE("generate_random with supplied engine advances state on each call") {
    std::mt19937 engine(0xdeadbeef);
    Uuid a = Uuid::generate_random(engine);
    Uuid c = Uuid::generate_random(engine);

    CHECK(c != a);
    CHECK(c.variant() == Uuid::Variant::kRfc);
    CHECK(c.version() == Uuid::Version::kRandomBased);
  }

  TEST_CASE("rfc variant and v4 version bits hold across 256 random samples") {
    for (int i = 0; i < 256; ++i) {
      Uuid id = Uuid::generate_random();

      CHECK(id.variant() == Uuid::Variant::kRfc);
      CHECK(id.version() == Uuid::Version::kRandomBased);
    }
  }

  TEST_CASE("1000 random uuids have no collisions in ordered set") {
    std::set<Uuid> seen;

    for (int i = 0; i < 1000; ++i) {
      seen.insert(Uuid::generate_random());
    }

    CHECK(seen.size() == 1000U);
  }

  TEST_CASE("std::hash gives same result for equal uuids") {
    std::array<uint8_t, 16> data{};
    data[3] = 0x42;
    Uuid a{data};
    Uuid b{data};

    CHECK(std::hash<Uuid>{}(a) == std::hash<Uuid>{}(b));
  }

  TEST_CASE("1000 random uuids have no hash collisions in unordered_set") {
    std::unordered_set<Uuid> set;

    for (int i = 0; i < 1000; ++i) {
      set.insert(Uuid::generate_random());
    }

    CHECK(set.size() == 1000U);
  }

  TEST_CASE("uuid is usable as unordered_map key") {
    std::unordered_map<Uuid, int> map;
    Uuid a = Uuid::generate_random();
    Uuid b = Uuid::generate_random();

    map.emplace(a, 1);
    map.emplace(b, 2);

    REQUIRE(map.size() == 2U);
    CHECK(map.at(a) == 1);
    CHECK(map.at(b) == 2);
  }

  TEST_CASE("random_bytes returns the exact requested count") {
    SUBCASE("small counts") {
      for (size_t n : {size_t{1}, size_t{3}, size_t{7}, size_t{15}}) {
        CHECK(Uuid::random_bytes(n).size() == n);
      }
    }

    SUBCASE("aligned counts") {
      for (size_t n : {size_t{4}, size_t{8}, size_t{16}, size_t{32}, size_t{64}}) {
        CHECK(Uuid::random_bytes(n).size() == n);
      }
    }

    SUBCASE("unaligned larger counts") {
      for (size_t n : {size_t{17}, size_t{127}, size_t{128}}) {
        CHECK(Uuid::random_bytes(n).size() == n);
      }
    }
  }

  TEST_CASE("random_bytes returns empty vector for count zero") { CHECK(Uuid::random_bytes(0).empty()); }

  TEST_CASE("random_bytes produces different output on successive calls") {
    auto a = Uuid::random_bytes(32);
    auto b = Uuid::random_bytes(32);

    CHECK(a != b);
  }

  TEST_CASE("random_hex returns lowercase hex of double the byte count length") {
    auto hex = Uuid::random_hex(16);

    CHECK(hex.size() == 32U);

    for (char ch : hex) {
      const bool is_lower_hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
      CHECK(is_lower_hex);
    }
  }

  TEST_CASE("random_hex default overload emits 32 lowercase hex characters") {
    auto hex = Uuid::random_hex();

    CHECK(hex.size() == 32U);
  }

  TEST_CASE("random_hex handles odd byte counts correctly") {
    auto hex = Uuid::random_hex(5);

    CHECK(hex.size() == 10U);
  }

  TEST_CASE("random_hex returns empty string for zero byte count") { CHECK(Uuid::random_hex(0).empty()); }

  TEST_CASE("random_hex 16-byte output round-trips through from_string") {
    auto hex = Uuid::random_hex(16);
    auto parsed = Uuid::from_string(hex);

    REQUIRE(parsed.has_value());
    CHECK(parsed->to_compact_string() == hex);
  }

  TEST_CASE("random_hex produces different output on successive calls") {
    auto a = Uuid::random_hex(16);
    auto b = Uuid::random_hex(16);

    CHECK(a != b);
  }

  TEST_CASE("to_string then from_string round-trips for many random uuids") {
    for (int i = 0; i < 256; ++i) {
      Uuid id = Uuid::generate_random();
      auto parsed = Uuid::from_string(id.to_string());

      REQUIRE(parsed.has_value());
      CHECK(*parsed == id);
    }
  }

  TEST_CASE("to_compact_string then from_string round-trips for many random uuids") {
    for (int i = 0; i < 256; ++i) {
      Uuid id = Uuid::generate_random();
      auto parsed = Uuid::from_string(id.to_compact_string());

      REQUIRE(parsed.has_value());
      CHECK(*parsed == id);
    }
  }
}

// NOLINTEND
