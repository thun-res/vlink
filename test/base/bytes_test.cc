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

#include "./base/bytes.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "../common_test.h"

static void fill_pattern(Bytes& b) {
  for (size_t i = 0; i < b.size(); ++i) {
    b.data()[i] = static_cast<uint8_t>(i & 0xFFU);
  }
}

TEST_SUITE("base-Bytes") {
  TEST_CASE("default construction yields empty object") {
    Bytes b;

    CHECK(b.empty());
    CHECK_EQ(b.size(), 0u);
    CHECK_EQ(b.real_size(), 0u);
    CHECK_EQ(b.capacity(), 0u);
    CHECK_EQ(b.offset(), 0u);
    CHECK(b.data() == nullptr);
    CHECK(b.real_data() == nullptr);
    CHECK_FALSE(b.is_owner());
    CHECK_FALSE(b.is_loaned());
    CHECK_FALSE(b.is_ptr());
    CHECK(b.begin() == nullptr);
    CHECK(b.end() == nullptr);
    CHECK(b.real_begin() == nullptr);
    CHECK(b.real_end() == nullptr);
  }

  TEST_CASE("initializer_list construction copies bytes") {
    Bytes b{0x01u, 0x02u, 0x03u};

    REQUIRE_EQ(b.size(), 3u);
    CHECK_FALSE(b.empty());
    CHECK(b.is_owner());
    REQUIRE(b.data() != nullptr);
    CHECK_EQ(b.data()[0], 0x01u);
    CHECK_EQ(b.data()[1], 0x02u);
    CHECK_EQ(b.data()[2], 0x03u);
  }

  TEST_CASE("vector construction deep-copies content") {
    std::vector<uint8_t> vec{0xAAu, 0xBBu, 0xCCu, 0xDDu};
    Bytes b(vec);

    REQUIRE_EQ(b.size(), 4u);
    CHECK(b.is_owner());
    CHECK(b == vec);
  }

  TEST_CASE("create allocates buffer for representative sizes") {
    size_t sz = 0;
    SUBCASE("small SBO") { sz = 32u; }
    SUBCASE("at boundary") { sz = Bytes::stack_size(); }
    SUBCASE("heap size") { sz = 200u; }

    Bytes b = Bytes::create(sz);

    REQUIRE(b.data() != nullptr);
    CHECK_EQ(b.size(), sz);
    CHECK(b.is_owner());
    CHECK_FALSE(b.is_loaned());
    CHECK_FALSE(b.is_ptr());
    CHECK_FALSE(b.empty());
    CHECK(b.capacity() >= sz);
  }

  TEST_CASE("create with offset reserves prefix region") {
    Bytes b = Bytes::create(32u, 4u);

    CHECK_EQ(b.size(), 32u);
    CHECK_EQ(b.offset(), 4u);
    CHECK_EQ(b.real_size(), 36u);
    CHECK(b.data() == b.real_data() + 4u);
    CHECK(b.is_owner());
  }

  TEST_CASE("SBO boundary: stack size stays on stack, stack_size+1 goes to heap") {
    Bytes sbo = Bytes::create(Bytes::stack_size());
    Bytes heap = Bytes::create(Bytes::stack_size() + 1u);

    CHECK_EQ(sbo.capacity(), Bytes::stack_size());
    CHECK(heap.capacity() > Bytes::stack_size());
  }

  TEST_CASE("create rejects impossible allocation size") {
    Bytes b = Bytes::create(std::numeric_limits<size_t>::max(), 1u);

    CHECK(b.empty());
    CHECK(b.data() == nullptr);
  }

  TEST_CASE("shallow_copy aliases external mutable buffer") {
    std::vector<uint8_t> ext{0x10u, 0x20u, 0x30u};
    Bytes b = Bytes::shallow_copy(ext.data(), ext.size());

    CHECK_EQ(b.size(), 3u);
    CHECK_FALSE(b.is_owner());
    CHECK_FALSE(b.is_loaned());
    CHECK(b.data() == ext.data());

    b.data()[0] = 0xFFu;
    CHECK_EQ(ext[0], 0xFFu);
  }

  TEST_CASE("shallow_copy wraps const buffer without copying") {
    const uint8_t src[] = {0x01u, 0x02u, 0x03u};
    Bytes b = Bytes::shallow_copy(src, 3u);

    CHECK_EQ(b.size(), 3u);
    CHECK_FALSE(b.is_owner());
    REQUIRE(b.data() != nullptr);
    CHECK_EQ(b.data()[2], 0x03u);
  }

  TEST_CASE("shallow_copy_ptr wraps opaque pointer with zero size") {
    int sentinel = 42;
    Bytes b = Bytes::shallow_copy_ptr(&sentinel);

    CHECK(b.is_ptr());
    CHECK_EQ(b.size(), 0u);
    CHECK_EQ(b.offset(), 0u);
    CHECK_FALSE(b.is_owner());
    CHECK(b.to_ptr<int>() == &sentinel);
  }

  TEST_CASE("deep_copy produces independent owned copy") {
    uint8_t src[] = {0xAAu, 0xBBu, 0xCCu};
    Bytes b = Bytes::deep_copy(src, 3u);

    CHECK_EQ(b.size(), 3u);
    CHECK(b.is_owner());
    CHECK(static_cast<void*>(b.data()) != static_cast<void*>(src));
    CHECK_EQ(b.data()[0], 0xAAu);

    src[0] = 0x00u;
    CHECK_EQ(b.data()[0], 0xAAu);
  }

  TEST_CASE("deep_copy with offset reserves prefix in owned copy") {
    const uint8_t src[] = {0xA1u, 0xA2u};
    Bytes b = Bytes::deep_copy(src, 2u, 8u);

    CHECK_EQ(b.offset(), 8u);
    CHECK_EQ(b.size(), 2u);
    CHECK_EQ(b.real_size(), 10u);
    CHECK_EQ(b.data()[0], 0xA1u);
  }

  TEST_CASE("deep_copy on null/empty produces empty non-owning object") {
    Bytes b = Bytes::deep_copy(static_cast<const uint8_t*>(nullptr), 0u);

    CHECK(b.empty());
  }

  TEST_CASE("loan_internal marks object as loaned and non-owning") {
    uint8_t buf[] = {0x01u, 0x02u, 0x03u};
    Bytes loaned = Bytes::loan_internal(buf, 3u);

    CHECK(loaned.is_loaned());
    CHECK_FALSE(loaned.is_owner());
    CHECK_EQ(loaned.size(), 3u);
    CHECK(loaned.data() == buf);

    loaned.data()[0] = 0xFFu;
    CHECK_EQ(buf[0], 0xFFu);
  }

  TEST_CASE("loan_internal const marks object as loaned") {
    const uint8_t buf[] = {0xFFu, 0xFEu};
    Bytes loaned = Bytes::loan_internal(buf, 2u);

    CHECK(loaned.is_loaned());
    CHECK_FALSE(loaned.is_owner());
    CHECK_EQ(loaned.size(), 2u);
  }

  TEST_CASE("from_string copies string bytes") {
    std::string s = "hello";
    Bytes b = Bytes::from_string(s);

    REQUIRE_EQ(b.size(), s.size());
    CHECK(b.is_owner());
    CHECK_EQ(b.to_string(), s);
  }

  TEST_CASE("from_string with offset reserves prefix") {
    std::string s = "world";
    Bytes b = Bytes::from_string(s, 4u);

    CHECK_EQ(b.size(), s.size());
    CHECK_EQ(b.offset(), 4u);
    CHECK_EQ(b.to_string(), s);
  }

  TEST_CASE("from_user_input parses valid hex tokens") {
    bool ok = false;

    SUBCASE("space-separated lowercase") {
      Bytes b = Bytes::from_user_input("01 02 03", &ok);
      CHECK(ok);
      REQUIRE_EQ(b.size(), 3u);
      CHECK_EQ(b.data()[0], 0x01u);
      CHECK_EQ(b.data()[2], 0x03u);
    }

    SUBCASE("uppercase hex") {
      Bytes b = Bytes::from_user_input("AB CD EF", &ok);
      CHECK(ok);
      REQUIRE_EQ(b.size(), 3u);
      CHECK_EQ(b.data()[0], 0xABu);
    }

    SUBCASE("0x-prefixed contiguous") {
      Bytes b = Bytes::from_user_input("0xDEAD", &ok);
      CHECK(ok);
      REQUIRE_EQ(b.size(), 2u);
      CHECK_EQ(b.data()[0], 0xDEu);
      CHECK_EQ(b.data()[1], 0xADu);
    }
  }

  TEST_CASE("from_user_input rejects malformed input") {
    bool ok = true;

    SUBCASE("invalid chars") {
      Bytes b = Bytes::from_user_input("ZZ QQ", &ok);
      CHECK_FALSE(ok);
      CHECK(b.empty());
    }

    SUBCASE("partially invalid token") {
      Bytes b = Bytes::from_user_input("1G", &ok);
      CHECK_FALSE(ok);
      CHECK(b.empty());
    }
  }

  TEST_CASE("from_user_input accepts null ok pointer") {
    Bytes b = Bytes::from_user_input("FF", nullptr);
    CHECK_EQ(b.size(), 1u);
  }

  TEST_CASE("convert_to_hex_str produces readable hex") {
    const uint8_t data[] = {0x1Au, 0xB2u};
    std::string hex = Bytes::convert_to_hex_str(data, 2u);

    CHECK_FALSE(hex.empty());
    CHECK(hex.find('1') != std::string::npos);
    bool has_a = hex.find('A') != std::string::npos || hex.find('a') != std::string::npos;
    CHECK(has_a);
  }

  TEST_CASE("reverse_order reverses byte sequence") {
    Bytes orig{0x01u, 0x02u, 0x03u, 0x04u};
    Bytes rev = Bytes::reverse_order(orig);

    REQUIRE_EQ(rev.size(), orig.size());
    CHECK_EQ(rev.data()[0], 0x04u);
    CHECK_EQ(rev.data()[3], 0x01u);
    CHECK_EQ(orig.data()[0], 0x01u);
  }

  TEST_CASE("reverse_order on empty and single-byte buffers") {
    SUBCASE("empty") {
      Bytes rev = Bytes::reverse_order(Bytes{});
      CHECK(rev.empty());
    }

    SUBCASE("single byte") {
      Bytes orig{0x42u};
      Bytes rev = Bytes::reverse_order(orig);
      REQUIRE_EQ(rev.size(), 1u);
      CHECK_EQ(rev.data()[0], 0x42u);
    }
  }

  TEST_CASE("encode_to_base64 and decode_from_base64 round-trip") {
    SUBCASE("small buffer") {
      Bytes original{0xDEu, 0xADu, 0xBEu, 0xEFu};
      std::string encoded = Bytes::encode_to_base64(original);
      CHECK_FALSE(encoded.empty());
      Bytes decoded = Bytes::decode_from_base64(encoded);
      REQUIRE_EQ(decoded.size(), original.size());
      CHECK(decoded == original);
    }

    SUBCASE("larger buffer with pattern") {
      Bytes original = Bytes::create(48u);
      fill_pattern(original);
      std::string encoded = Bytes::encode_to_base64(original);
      Bytes decoded = Bytes::decode_from_base64(encoded);
      REQUIRE_EQ(decoded.size(), original.size());
      CHECK(decoded == original);
    }

    SUBCASE("empty bytes encodes to empty or trivial string") {
      Bytes empty;
      std::string encoded = Bytes::encode_to_base64(empty);
      Bytes decoded = Bytes::decode_from_base64(encoded);
      CHECK_EQ(decoded.size(), 0u);
    }
  }

  TEST_CASE("decode_from_base64 rejects invalid input") {
    CHECK(Bytes::decode_from_base64("!!!invalid!!!").empty());
    CHECK(Bytes::decode_from_base64("TQ=A").empty());
    CHECK(Bytes::decode_from_base64("T===").empty());
  }

  TEST_CASE("get_crc_32 is deterministic and sensitive to content") {
    Bytes a{0x01u, 0x02u, 0x03u};
    Bytes b{0x01u, 0x02u, 0x03u};
    Bytes c{0x01u, 0x02u, 0x04u};

    CHECK_EQ(Bytes::get_crc_32(a), Bytes::get_crc_32(b));
    CHECK_NE(Bytes::get_crc_32(a), Bytes::get_crc_32(c));
    CHECK_EQ(Bytes::get_crc_32(Bytes{}), 0x00000000u);
  }

  TEST_CASE("get_crc_32 matches CRC-32/ISO-HDLC reference vectors") {
    auto crc_of = [](const std::string& s) {
      return Bytes::get_crc_32(Bytes::deep_copy(reinterpret_cast<const uint8_t*>(s.data()), s.size()));
    };

    CHECK_EQ(crc_of("a"), 0xE8B7BE43u);
    CHECK_EQ(crc_of("abc"), 0x352441C2u);
    CHECK_EQ(crc_of("123456789"), 0xCBF43926u);
    CHECK_EQ(crc_of("The quick brown fox jumps over the lazy dog"), 0x414FA339u);
  }

  TEST_CASE("get_crc_64 is deterministic and sensitive to content") {
    Bytes a{0x01u, 0x02u, 0x03u};
    Bytes b{0x01u, 0x02u, 0x03u};
    Bytes c{0x01u, 0x02u, 0x04u};

    CHECK_EQ(Bytes::get_crc_64(a), Bytes::get_crc_64(b));
    CHECK_NE(Bytes::get_crc_64(a), Bytes::get_crc_64(c));
    CHECK_EQ(Bytes::get_crc_64(Bytes{}), 0x0000000000000000ull);
  }

  TEST_CASE("get_crc_64 matches CRC-64/ECMA-182 reference vector") {
    auto crc_of = [](const std::string& s) {
      return Bytes::get_crc_64(Bytes::deep_copy(reinterpret_cast<const uint8_t*>(s.data()), s.size()));
    };

    // Canonical check value from the CRC catalogue for CRC-64/ECMA-182.
    CHECK_EQ(crc_of("123456789"), 0x6C40DF5F0B497347ull);
  }

  TEST_CASE("compress_data and uncompress_data round-trip") {
    Bytes original = Bytes::create(512u);
    std::memset(original.data(), 0x5Au, 512u);

    Bytes compressed = Bytes::compress_data(original.data(), original.size());
    REQUIRE_FALSE(compressed.empty());
    CHECK(Bytes::is_compress_data(compressed.data(), compressed.size()));

    Bytes decompressed = Bytes::uncompress_data(compressed.data(), compressed.size());
    REQUIRE_EQ(decompressed.size(), original.size());
    CHECK_EQ(std::memcmp(decompressed.data(), original.data(), original.size()), 0);
  }

  TEST_CASE("compress_data high-ratio mode round-trip") {
    Bytes original = Bytes::create(256u);
    fill_pattern(original);

    Bytes compressed = Bytes::compress_data(original.data(), original.size(), true);
    REQUIRE_FALSE(compressed.empty());
    CHECK(Bytes::is_compress_data(compressed.data(), compressed.size()));

    Bytes decompressed = Bytes::uncompress_data(compressed.data(), compressed.size());
    REQUIRE_EQ(decompressed.size(), original.size());
    CHECK_EQ(std::memcmp(decompressed.data(), original.data(), original.size()), 0);
  }

  TEST_CASE("is_compress_data rejects uncompressed and too-small buffers") {
    Bytes plain{0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u, 0x08u};
    CHECK_FALSE(Bytes::is_compress_data(plain.data(), plain.size()));

    const uint8_t tiny[] = {0x17u, 0x49u};
    CHECK_FALSE(Bytes::is_compress_data(tiny, 2u));
  }

  TEST_CASE("uncompress_data with invalid magic returns empty") {
    Bytes junk{0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u};
    CHECK(Bytes::uncompress_data(junk.data(), junk.size(), true).empty());
  }

  TEST_CASE("to_string and to_string_view reflect content") {
    std::string text = "VLink";
    Bytes b = Bytes::from_string(text);

    CHECK_EQ(b.to_string(), text);
    CHECK_EQ(b.to_string_view(), text);
    CHECK(b.to_string_view().data() == reinterpret_cast<const char*>(b.data()));
  }

  TEST_CASE("to_raw_data returns independent vector copy") {
    Bytes b{0x11u, 0x22u, 0x33u};
    std::vector<uint8_t> vec = b.to_raw_data();

    REQUIRE_EQ(vec.size(), 3u);
    CHECK_EQ(vec[0], 0x11u);
    CHECK_EQ(vec[1], 0x22u);
    CHECK_EQ(vec[2], 0x33u);
  }

  TEST_CASE("operator[] provides read-write access") {
    Bytes b{0x10u, 0x20u, 0x30u};

    CHECK_EQ(b[0], 0x10u);
    b[1] = 0xFFu;
    CHECK_EQ(b[1], 0xFFu);
  }

  TEST_CASE("operator== and operator!= compare byte content") {
    Bytes a{0x01u, 0x02u, 0x03u};
    Bytes b{0x01u, 0x02u, 0x03u};
    Bytes c{0x01u, 0x02u, 0x04u};

    CHECK(a == b);
    CHECK_FALSE(a != b);
    CHECK(a != c);
    CHECK_FALSE(a == c);

    std::vector<uint8_t> vec{0x01u, 0x02u, 0x03u};
    CHECK(a == vec);
    CHECK_FALSE(a != vec);

    Bytes empty_a;
    Bytes empty_b;
    CHECK(empty_a == empty_b);
  }

  TEST_CASE("begin and end span the user data region") {
    Bytes b{0x0Au, 0x0Bu, 0x0Cu};

    CHECK(b.begin() == b.data());
    CHECK(b.end() == b.data() + b.size());

    size_t count = 0;
    for (const uint8_t* it = b.begin(); it != b.end(); ++it) {
      ++count;
    }
    CHECK_EQ(count, b.size());
  }

  TEST_CASE("real_begin and real_end span the full backing buffer with offset") {
    Bytes b = Bytes::create(10u, 4u);

    CHECK(b.real_begin() == b.real_data());
    CHECK(b.real_end() == b.real_data() + b.real_size());
    CHECK_EQ(b.real_end() - b.real_begin(), static_cast<ptrdiff_t>(b.real_size()));
  }

  TEST_CASE("copy constructor deep-copies to independent owned buffer") {
    Bytes original{0x01u, 0x02u, 0x03u};
    Bytes copy(original);

    CHECK(copy.is_owner());
    CHECK(copy == original);
    CHECK(copy.data() != original.data());

    copy.data()[0] = 0xFFu;
    CHECK_EQ(original.data()[0], 0x01u);
  }

  TEST_CASE("copy constructor on shallow alias always deep-copies") {
    uint8_t ext[] = {0x11u, 0x22u};
    Bytes alias = Bytes::shallow_copy(ext, 2u);
    Bytes copy(alias);

    CHECK(copy.is_owner());
    CHECK(copy.data() != ext);
    CHECK_EQ(copy.size(), 2u);
    CHECK_EQ(copy.data()[0], 0x11u);
  }

  TEST_CASE("move constructor transfers ownership and empties source") {
    Bytes original = Bytes::create(32u);
    fill_pattern(original);

    Bytes moved(std::move(original));

    CHECK_EQ(moved.size(), 32u);
    CHECK(moved.is_owner());
    REQUIRE(moved.data() != nullptr);
    CHECK_EQ(moved.data()[0], 0x00u);
    CHECK(original.empty());  // NOLINT(bugprone-use-after-move)
  }

  TEST_CASE("move of heap-allocated Bytes transfers pointer without reallocation") {
    static constexpr size_t kHeapSize = 200u;
    Bytes a = Bytes::create(kHeapSize);
    fill_pattern(a);
    const uint8_t* old_real = a.real_data();

    Bytes b(std::move(a));

    CHECK_EQ(b.size(), kHeapSize);
    CHECK(b.real_data() == old_real);
    CHECK(a.empty());  // NOLINT(bugprone-use-after-move)
  }

  TEST_CASE("copy assignment replaces content with deep copy") {
    Bytes a{0x01u, 0x02u};
    Bytes b{0xFFu};
    b = a;

    CHECK_EQ(b.size(), 2u);
    CHECK(b.is_owner());
    CHECK(b == a);
    CHECK(b.data() != a.data());
  }

  TEST_CASE("move assignment transfers ownership and empties source") {
    Bytes a{0x10u, 0x20u, 0x30u};
    Bytes b;
    b = std::move(a);

    CHECK_EQ(b.size(), 3u);
    CHECK_EQ(b.data()[0], 0x10u);
    CHECK(a.empty());  // NOLINT(bugprone-use-after-move)
  }

  TEST_CASE("vector assignment deep-copies from vector") {
    std::vector<uint8_t> vec{0xA1u, 0xA2u, 0xA3u};
    Bytes b;
    b = vec;

    REQUIRE_EQ(b.size(), 3u);
    CHECK(b.is_owner());
    CHECK(b == vec);
  }

  TEST_CASE("clear resets to empty state") {
    Bytes b = Bytes::create(64u);
    CHECK_FALSE(b.empty());

    b.clear();

    CHECK(b.empty());
    CHECK(b.data() == nullptr);
    CHECK_EQ(b.size(), 0u);
  }

  TEST_CASE("shrink_to reduces logical size on owned buffers") {
    Bytes b = Bytes::create(16u);
    fill_pattern(b);

    CHECK(b.shrink_to(8u));
    CHECK_EQ(b.size(), 8u);
    CHECK_EQ(b.data()[0], 0x00u);
    CHECK_EQ(b.data()[7], 0x07u);
  }

  TEST_CASE("shrink_to with size larger than current returns false") {
    Bytes b = Bytes::create(8u);
    CHECK_FALSE(b.shrink_to(16u));
    CHECK_EQ(b.size(), 8u);
  }

  TEST_CASE("reserve grows capacity without changing logical size") {
    Bytes b = Bytes::create(10u);
    size_t old_cap = b.capacity();

    CHECK(b.reserve(old_cap + 100u));
    CHECK(b.capacity() >= old_cap + 100u);
    CHECK_EQ(b.size(), 10u);
  }

  TEST_CASE("reserve is no-op when capacity already sufficient") {
    Bytes b = Bytes::create(64u);
    size_t cap_before = b.capacity();

    CHECK(b.reserve(10u));
    CHECK_EQ(b.capacity(), cap_before);
  }

  TEST_CASE("resize updates logical size and grows capacity as needed") {
    Bytes b = Bytes::create(8u);

    CHECK(b.resize(32u));
    CHECK_EQ(b.size(), 32u);
    CHECK(b.capacity() >= 32u);

    CHECK(b.resize(16u));
    CHECK_EQ(b.size(), 16u);
  }

  TEST_CASE("instance shallow_copy makes non-owning alias") {
    Bytes owner = Bytes::create(8u);
    fill_pattern(owner);

    Bytes alias;
    alias.shallow_copy(owner);

    CHECK_FALSE(alias.is_owner());
    CHECK(alias.data() == owner.data());
    CHECK_EQ(alias.size(), owner.size());
  }

  TEST_CASE("instance deep_copy produces independent owner") {
    Bytes original = Bytes::create(8u);
    fill_pattern(original);

    Bytes copy;
    copy.deep_copy(original);

    CHECK(copy.is_owner());
    CHECK_EQ(copy.size(), original.size());
    CHECK(copy == original);
    CHECK(copy.data() != original.data());
  }

  TEST_CASE("deep_copy_self converts non-owning alias to owner") {
    uint8_t ext[] = {0x10u, 0x20u, 0x30u};
    Bytes alias = Bytes::shallow_copy(ext, 3u);
    CHECK_FALSE(alias.is_owner());

    alias.deep_copy_self();

    CHECK(alias.is_owner());
    CHECK(alias.data() != ext);
    CHECK_EQ(alias.data()[0], 0x10u);
  }

  TEST_CASE("deep_copy_self on already-owned buffer is a no-op") {
    Bytes owner = Bytes::create(4u);
    fill_pattern(owner);

    owner.deep_copy_self();

    CHECK(owner.is_owner());
    CHECK_EQ(owner.size(), 4u);
    CHECK_EQ(owner.data()[0], 0x00u);
  }

  TEST_CASE("is_ptr returns false for normal owned buffer") { CHECK_FALSE(Bytes::create(8u).is_ptr()); }

  TEST_CASE("to_ptr reinterprets real_data pointer") {
    Bytes b{0x01u, 0x02u, 0x03u, 0x04u};
    CHECK(b.to_ptr<uint8_t>() == b.real_data());
  }

  TEST_CASE("stack_size returns 96") { CHECK_EQ(Bytes::stack_size(), 96u); }

  TEST_CASE("endianness helpers are mutually exclusive and cover all platforms") {
    CHECK((Bytes::is_little_endian() || Bytes::is_big_endian()));
    CHECK_NE(Bytes::is_little_endian(), Bytes::is_big_endian());
  }

  TEST_CASE("ostream operator produces non-empty hex output") {
    Bytes b{0x01u, 0x02u, 0x03u};
    std::ostringstream oss;
    oss << b;
    CHECK_FALSE(oss.str().empty());
  }

  TEST_CASE("bytes_malloc and bytes_free are usable for raw pool access") {
    static constexpr size_t kSize = 128u;
    uint8_t* mem = Bytes::bytes_malloc(kSize);
    REQUIRE(mem != nullptr);

    for (size_t i = 0; i < kSize; ++i) {
      mem[i] = static_cast<uint8_t>(i & 0xFFu);
    }

    for (size_t i = 0; i < kSize; ++i) {
      CHECK_EQ(mem[i], static_cast<uint8_t>(i & 0xFFu));
    }

    Bytes::bytes_free(mem, kSize);
  }

  TEST_CASE("init_memory_pool and release_memory_pool do not invalidate live buffers") {
    Bytes::init_memory_pool();

    Bytes pinned = Bytes::create(2048u);
    REQUIRE(pinned.data() != nullptr);
    std::memset(pinned.data(), 0xA5u, pinned.size());

    {
      Bytes scratch_a = Bytes::create(2048u);
      Bytes scratch_b = Bytes::create(64u * 1024u);
      REQUIRE(scratch_a.data() != nullptr);
      REQUIRE(scratch_b.data() != nullptr);
    }

    Bytes::release_memory_pool();

    for (size_t i = 0; i < pinned.size(); ++i) {
      CHECK_EQ(pinned.data()[i], 0xA5u);
    }

    Bytes after = Bytes::create(2048u);
    REQUIRE(after.data() != nullptr);
  }

  TEST_CASE("copy assignment via shallow alias handles aliased source correctly") {
    Bytes owner = Bytes::create(Bytes::stack_size() + 32u, 8u);
    REQUIRE(owner.real_data() != nullptr);

    for (size_t i = 0; i < owner.real_size(); ++i) {
      owner.real_data()[i] = static_cast<uint8_t>(i & 0xFFu);
    }

    Bytes alias;
    alias.shallow_copy(owner);
    owner = alias;

    CHECK(owner.is_owner());
    CHECK_EQ(owner.size(), alias.size());
  }
}

// NOLINTEND
