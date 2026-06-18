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

#include "./zerocopy/raw_data.h"

#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "../common_test.h"

namespace {

void fill_pattern(uint8_t* buf, size_t n, uint8_t seed = 0xA5) {
  for (size_t i = 0; i < n; ++i) {
    buf[i] = static_cast<uint8_t>(seed + static_cast<uint8_t>(i & 0xFFu));
  }
}

bool region_matches(const uint8_t* a, const uint8_t* b, size_t n) { return std::memcmp(a, b, n) == 0; }

}  // namespace

TEST_SUITE("zerocopy-RawData") {
  TEST_CASE("default construction yields invalid empty object") {
    zerocopy::RawData rd;

    CHECK_FALSE(rd.is_valid());
    CHECK_EQ(rd.size(), 0u);
    CHECK_FALSE(rd.is_owner());
    CHECK_EQ(rd.data(), nullptr);
    CHECK_EQ(rd.get_reserved(), 0u);
  }

  TEST_CASE("sizeof is exactly 64 bytes") { CHECK_EQ(sizeof(zerocopy::RawData), 64u); }

  TEST_CASE("create succeeds and sets ownership for representative sizes") {
    size_t sz = 0;

    SUBCASE("single byte") { sz = 1; }
    SUBCASE("small") { sz = 64; }
    SUBCASE("medium") { sz = 1024; }
    SUBCASE("large") { sz = 65536; }

    zerocopy::RawData rd;
    CHECK(rd.create(sz));
    CHECK(rd.is_valid());
    CHECK(rd.is_owner());
    CHECK_EQ(rd.size(), sz);
    CHECK_NE(rd.data(), nullptr);
  }

  TEST_CASE("create with zero size returns false") {
    zerocopy::RawData rd;

    CHECK_FALSE(rd.create(0));
    CHECK_FALSE(rd.is_valid());
  }

  TEST_CASE("create replaces previous owned buffer") {
    zerocopy::RawData rd;

    REQUIRE(rd.create(100));
    CHECK_EQ(rd.size(), 100u);

    REQUIRE(rd.create(200));
    CHECK_EQ(rd.size(), 200u);
    CHECK(rd.is_owner());
  }

  TEST_CASE("header fields are preserved across create") {
    zerocopy::RawData rd;

    rd.header.seq = 5u;
    std::strncpy(rd.header.frame_id, "raw_0", sizeof(rd.header.frame_id) - 1);
    rd.header.frame_id[sizeof(rd.header.frame_id) - 1] = '\0';
    rd.header.time_meas = 12345u;
    rd.header.time_pub = 67890u;

    rd.create(256);

    CHECK_EQ(rd.header.seq, 5u);
    CHECK_EQ(std::string(rd.header.frame_id), "raw_0");
    CHECK_EQ(rd.header.time_meas, 12345u);
    CHECK_EQ(rd.header.time_pub, 67890u);
  }

  TEST_CASE("clear resets all fields including header and reserved") {
    zerocopy::RawData rd;

    rd.create(200);
    rd.header.seq = 99u;
    rd.get_reserved() = 0x1234u;
    REQUIRE(rd.is_valid());

    rd.clear();

    CHECK_FALSE(rd.is_valid());
    CHECK_FALSE(rd.is_owner());
    CHECK_EQ(rd.size(), 0u);
    CHECK_EQ(rd.data(), nullptr);
    CHECK_EQ(rd.header.seq, 0u);
    CHECK_EQ(rd.get_reserved(), 0u);
  }

  TEST_CASE("clear then create again succeeds") {
    zerocopy::RawData rd;

    rd.create(100);
    rd.clear();
    CHECK_FALSE(rd.is_valid());

    CHECK(rd.create(50));
    CHECK(rd.is_valid());
    CHECK_EQ(rd.size(), 50u);
  }

  TEST_CASE("reserved round-trip including boundary values") {
    zerocopy::RawData rd;

    rd.get_reserved() = 0x1234u;
    CHECK_EQ(rd.get_reserved(), 0x1234u);

    rd.get_reserved() = 0xFFFFu;
    CHECK_EQ(rd.get_reserved(), 0xFFFFu);

    rd.get_reserved() = 0u;
    CHECK_EQ(rd.get_reserved(), 0u);
  }

  TEST_CASE("shallow_copy from RawData aliases the buffer") {
    zerocopy::RawData src;

    src.create(256);
    fill_pattern(const_cast<uint8_t*>(src.data()), 256, 0x11u);

    zerocopy::RawData dst;
    CHECK(dst.shallow_copy(src));

    CHECK(dst.is_valid());
    CHECK_FALSE(dst.is_owner());
    CHECK_EQ(dst.size(), 256u);
    CHECK_EQ(dst.data(), src.data());
  }

  TEST_CASE("shallow_copy self returns false") {
    zerocopy::RawData rd;

    rd.create(32);
    CHECK_FALSE(rd.shallow_copy(rd));
  }

  TEST_CASE("shallow_copy from raw pointer aliases the buffer") {
    std::vector<uint8_t> buf(64, 0xABu);

    zerocopy::RawData rd;
    CHECK(rd.shallow_copy(buf.data(), buf.size()));

    CHECK(rd.is_valid());
    CHECK_FALSE(rd.is_owner());
    CHECK_EQ(rd.size(), 64u);
    CHECK_EQ(rd.data(), buf.data());
  }

  TEST_CASE("shallow_copy from raw pointer rejects null or zero size") {
    zerocopy::RawData rd;

    CHECK_FALSE(rd.shallow_copy(nullptr, 64));

    std::vector<uint8_t> buf(8, 0xFFu);
    CHECK_FALSE(rd.shallow_copy(buf.data(), 0));
  }

  TEST_CASE("shallow_copy same pointer returns false") {
    std::vector<uint8_t> buf(64, 0x55u);

    zerocopy::RawData rd;
    CHECK(rd.shallow_copy(buf.data(), buf.size()));
    CHECK_FALSE(rd.shallow_copy(buf.data(), buf.size()));
  }

  TEST_CASE("deep_copy from RawData allocates owned copy") {
    zerocopy::RawData src;

    src.create(128);
    fill_pattern(const_cast<uint8_t*>(src.data()), 128, 0xCCu);

    zerocopy::RawData dst;
    CHECK(dst.deep_copy(src));

    CHECK(dst.is_valid());
    CHECK(dst.is_owner());
    CHECK_EQ(dst.size(), 128u);
    CHECK(region_matches(src.data(), dst.data(), 128));
  }

  TEST_CASE("deep_copy into same-size owned buffer reuses memory") {
    zerocopy::RawData src;
    src.create(64);
    fill_pattern(const_cast<uint8_t*>(src.data()), 64, 0xAAu);

    zerocopy::RawData dst;
    dst.create(64);

    CHECK(dst.deep_copy(src));
    CHECK(dst.is_owner());
    CHECK_EQ(dst.size(), 64u);
    CHECK(region_matches(src.data(), dst.data(), 64));
  }

  TEST_CASE("deep_copy self returns false") {
    zerocopy::RawData rd;

    rd.create(64);
    CHECK_FALSE(rd.deep_copy(rd));
  }

  TEST_CASE("deep_copy from raw pointer") {
    static constexpr size_t kN = 128;
    std::vector<uint8_t> src(kN);
    fill_pattern(src.data(), kN, 0x33u);

    zerocopy::RawData rd;
    CHECK(rd.deep_copy(src.data(), kN));

    CHECK(rd.is_valid());
    CHECK(rd.is_owner());
    CHECK_EQ(rd.size(), kN);
    CHECK(region_matches(src.data(), rd.data(), kN));
  }

  TEST_CASE("deep_copy from raw pointer rejects null or zero size") {
    zerocopy::RawData rd;

    CHECK_FALSE(rd.deep_copy(nullptr, 64));

    std::vector<uint8_t> buf(8);
    CHECK_FALSE(rd.deep_copy(buf.data(), 0));
  }

  TEST_CASE("fill_data is an alias for deep_copy") {
    static constexpr size_t kN = 32;
    std::vector<uint8_t> src(kN, 0xFEu);

    zerocopy::RawData rd;
    CHECK(rd.fill_data(src.data(), kN));

    CHECK(rd.is_owner());
    CHECK(region_matches(src.data(), rd.data(), kN));
  }

  TEST_CASE("fill_data rejects null pointer and zero size") {
    zerocopy::RawData rd;

    CHECK_FALSE(rd.fill_data(nullptr, 64));

    std::vector<uint8_t> buf(8);
    CHECK_FALSE(rd.fill_data(buf.data(), 0));
  }

  TEST_CASE("move_copy transfers ownership and invalidates source") {
    zerocopy::RawData src;

    src.create(100);
    fill_pattern(const_cast<uint8_t*>(src.data()), 100, 0x77u);
    const uint8_t* original_ptr = src.data();

    zerocopy::RawData dst;
    CHECK(dst.move_copy(src));

    CHECK(dst.is_valid());
    CHECK(dst.is_owner());
    CHECK_EQ(dst.data(), original_ptr);
    CHECK_EQ(dst.size(), 100u);

    CHECK_FALSE(src.is_valid());
    CHECK_FALSE(src.is_owner());
    CHECK_EQ(src.data(), nullptr);
    CHECK_EQ(src.size(), 0u);
  }

  TEST_CASE("move_copy self returns false") {
    zerocopy::RawData rd;

    rd.create(32);
    CHECK_FALSE(rd.move_copy(rd));
  }

  TEST_CASE("copy constructor performs deep copy") {
    zerocopy::RawData src;

    src.create(64);
    fill_pattern(const_cast<uint8_t*>(src.data()), 64, 0x9Au);
    src.get_reserved() = 0x5678u;

    zerocopy::RawData copy(src);

    CHECK(copy.is_owner());
    CHECK_EQ(copy.size(), 64u);
    CHECK_NE(copy.data(), src.data());
    CHECK(region_matches(src.data(), copy.data(), 64));
    CHECK_EQ(copy.get_reserved(), 0x5678u);
  }

  TEST_CASE("move constructor transfers ownership") {
    zerocopy::RawData src;

    src.create(48);
    fill_pattern(const_cast<uint8_t*>(src.data()), 48, 0x5Cu);
    src.get_reserved() = 0x9ABCu;
    const uint8_t* ptr = src.data();

    zerocopy::RawData moved(std::move(src));

    CHECK(moved.is_owner());
    CHECK_EQ(moved.size(), 48u);
    CHECK_EQ(moved.data(), ptr);
    CHECK_EQ(moved.get_reserved(), 0x9ABCu);
    CHECK_FALSE(src.is_valid());
  }

  TEST_CASE("copy assignment performs deep copy") {
    zerocopy::RawData src;

    src.create(80);
    fill_pattern(const_cast<uint8_t*>(src.data()), 80, 0x4Du);

    zerocopy::RawData dst;
    dst = src;

    CHECK(dst.is_owner());
    CHECK_EQ(dst.size(), 80u);
    CHECK(region_matches(src.data(), dst.data(), 80));
  }

  TEST_CASE("move assignment transfers ownership") {
    zerocopy::RawData src;

    src.create(96);
    const uint8_t* ptr = src.data();

    zerocopy::RawData dst;
    dst = std::move(src);

    CHECK(dst.is_owner());
    CHECK_EQ(dst.data(), ptr);
    CHECK_FALSE(src.is_valid());
  }

  TEST_CASE("serialize and deserialize round-trip preserves all data") {
    static constexpr size_t kSize = 512;

    zerocopy::RawData rd;
    rd.header.seq = 1u;
    rd.header.time_pub = 999999u;
    rd.get_reserved() = 0xBEEFu;
    rd.create(kSize);
    fill_pattern(const_cast<uint8_t*>(rd.data()), kSize, 0x55u);

    Bytes wire;
    CHECK((rd >> wire));
    CHECK(zerocopy::RawData::check_valid(wire));
    CHECK_EQ(wire.size(), rd.get_serialized_size());

    zerocopy::RawData rd2;
    CHECK((rd2 << wire));

    CHECK(rd2.is_valid());
    CHECK_EQ(rd2.size(), kSize);
    CHECK_FALSE(rd2.is_owner());
    CHECK_EQ(rd2.header.seq, 1u);
    CHECK_EQ(rd2.header.time_pub, 999999u);
    CHECK_EQ(rd2.get_reserved(), 0xBEEFu);
    CHECK(region_matches(rd.data(), rd2.data(), kSize));
  }

  TEST_CASE("header fields survive serialization round-trip") {
    zerocopy::RawData rd;

    rd.header.seq = 42u;
    std::strncpy(rd.header.frame_id, "sensor_1", sizeof(rd.header.frame_id) - 1);
    rd.header.frame_id[sizeof(rd.header.frame_id) - 1] = '\0';
    rd.header.time_meas = 100000u;
    rd.header.time_pub = 200000u;
    rd.create(16);

    Bytes wire;
    rd >> wire;

    zerocopy::RawData rd2;
    rd2 << wire;

    CHECK_EQ(rd2.header.seq, 42u);
    CHECK_EQ(std::string(rd2.header.frame_id), "sensor_1");
    CHECK_EQ(rd2.header.time_meas, 100000u);
    CHECK_EQ(rd2.header.time_pub, 200000u);
  }

  TEST_CASE("check_valid rejects empty, corrupted begin magic, and corrupted end magic") {
    zerocopy::RawData rd;
    rd.create(128);

    Bytes wire;
    rd >> wire;

    SUBCASE("empty bytes") {
      Bytes empty;
      CHECK_FALSE(zerocopy::RawData::check_valid(empty));
    }

    SUBCASE("corrupted begin magic") {
      wire[0] ^= 0xFFu;
      CHECK_FALSE(zerocopy::RawData::check_valid(wire));
    }

    SUBCASE("corrupted end magic") {
      wire[wire.size() - 1] ^= 0xFFu;
      CHECK_FALSE(zerocopy::RawData::check_valid(wire));
    }
  }

  TEST_CASE("deserialize from too-small buffer returns false") {
    std::vector<uint8_t> raw(4, 0x00u);
    Bytes too_small(raw);

    zerocopy::RawData rd;
    CHECK_FALSE((rd << too_small));
  }

  TEST_CASE("serialize empty RawData produces valid wire buffer") {
    zerocopy::RawData rd;

    Bytes wire;
    CHECK((rd >> wire));
    CHECK(zerocopy::RawData::check_valid(wire));
  }

  TEST_CASE("get_serialized_size equals magic + version + struct + payload + magic") {
    zerocopy::RawData rd;

    SUBCASE("empty") {
      size_t expected = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(zerocopy::RawData) + 0u + sizeof(uint32_t);
      CHECK_EQ(rd.get_serialized_size(), expected);
    }

    SUBCASE("with payload") {
      rd.create(100);
      size_t expected = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(zerocopy::RawData) + 100u + sizeof(uint32_t);
      CHECK_EQ(rd.get_serialized_size(), expected);
    }
  }

  TEST_CASE("is_valid returns false when data is null or size is zero") {
    zerocopy::RawData rd;
    CHECK_FALSE(rd.is_valid());

    rd.create(1);
    CHECK(rd.is_valid());

    rd.clear();
    CHECK_FALSE(rd.is_valid());
  }

  TEST_CASE("deserialize rejects a wire payload whose major version differs") {
    zerocopy::RawData src;
    REQUIRE(src.create(64));

    Bytes wire;
    REQUIRE((src >> wire));
    REQUIRE(zerocopy::RawData::check_valid(wire));

    uint32_t foreign_version = (zerocopy::version_major(zerocopy::kWireVersion) + 1U) * 1000000U;
    std::memcpy(wire.data() + sizeof(uint32_t), &foreign_version, sizeof(foreign_version));

    CHECK_FALSE(zerocopy::RawData::check_valid(wire));

    zerocopy::RawData dst;
    CHECK_FALSE((dst << wire));
  }
}

// NOLINTEND
