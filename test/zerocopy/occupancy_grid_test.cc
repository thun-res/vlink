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

#include "./zerocopy/occupancy_grid.h"

#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "../common_test.h"

namespace {

void fill_pattern(uint8_t* buf, size_t n, uint8_t seed = 0xA5u) {
  for (size_t i = 0; i < n; ++i) {
    buf[i] = static_cast<uint8_t>(seed + static_cast<uint8_t>(i & 0xFFu));
  }
}

bool region_matches(const uint8_t* a, const uint8_t* b, size_t n) { return std::memcmp(a, b, n) == 0; }

}  // namespace

TEST_SUITE("zerocopy-OccupancyGrid") {
  TEST_CASE("default construction yields invalid empty grid") {
    zerocopy::OccupancyGrid og;

    CHECK_FALSE(og.is_valid());
    CHECK_EQ(og.size(), 0u);
    CHECK_FALSE(og.is_owner());
    CHECK_EQ(og.data(), nullptr);
    CHECK_EQ(og.width(), 0u);
    CHECK_EQ(og.height(), 0u);
    CHECK_EQ(og.channel(), 0u);
    CHECK_EQ(og.freq(), 0u);
    CHECK_EQ(og.valid_cell_count(), 0u);
    CHECK_EQ(og.resolution(), doctest::Approx(0.0f));
    CHECK_EQ(og.origin_x(), doctest::Approx(0.0f));
    CHECK_EQ(og.origin_y(), doctest::Approx(0.0f));
    CHECK_EQ(og.origin_z(), doctest::Approx(0.0f));
    CHECK_EQ(og.origin_yaw(), doctest::Approx(0.0f));
    CHECK_EQ(og.value_min(), doctest::Approx(0.0f));
    CHECK_EQ(og.value_max(), doctest::Approx(0.0f));
    CHECK_EQ(og.default_value(), 0);
    CHECK_EQ(og.occupied_threshold(), doctest::Approx(0.0f));
    CHECK_EQ(og.free_threshold(), doctest::Approx(0.0f));
    CHECK_EQ(og.cell_type(), zerocopy::OccupancyGrid::kCellUnknown);
    CHECK_EQ(og.cell_size(), 0u);
    CHECK_EQ(og.update_time_ns(), 0u);
    CHECK(og.map_id().empty());
  }

  TEST_CASE("sizeof is exactly 152 bytes") { CHECK_EQ(sizeof(zerocopy::OccupancyGrid), 152u); }

  TEST_CASE("cell_size_of returns byte size for each CellType") {
    CHECK_EQ(zerocopy::OccupancyGrid::cell_size_of(zerocopy::OccupancyGrid::kCellUnknown), 0u);
    CHECK_EQ(zerocopy::OccupancyGrid::cell_size_of(zerocopy::OccupancyGrid::kCellInt8), 1u);
    CHECK_EQ(zerocopy::OccupancyGrid::cell_size_of(zerocopy::OccupancyGrid::kCellUint8), 1u);
    CHECK_EQ(zerocopy::OccupancyGrid::cell_size_of(zerocopy::OccupancyGrid::kCellUint16), 2u);
    CHECK_EQ(zerocopy::OccupancyGrid::cell_size_of(zerocopy::OccupancyGrid::kCellFloat32), 4u);
  }

  TEST_CASE("cell_size returns cached size derived from cell_type") {
    zerocopy::OccupancyGrid og;

    og.set_cell_type(zerocopy::OccupancyGrid::kCellInt8);
    CHECK_EQ(og.cell_size(), 1u);

    og.set_cell_type(zerocopy::OccupancyGrid::kCellUint16);
    CHECK_EQ(og.cell_size(), 2u);

    og.set_cell_type(zerocopy::OccupancyGrid::kCellFloat32);
    CHECK_EQ(og.cell_size(), 4u);

    og.set_cell_type(zerocopy::OccupancyGrid::kCellUnknown);
    CHECK_EQ(og.cell_size(), 0u);
  }

  TEST_CASE("all metadata accessors round-trip") {
    zerocopy::OccupancyGrid og;

    og.set_width(400u);
    CHECK_EQ(og.width(), 400u);

    og.set_height(300u);
    CHECK_EQ(og.height(), 300u);

    og.set_channel(7u);
    CHECK_EQ(og.channel(), 7u);

    og.set_freq(10u);
    CHECK_EQ(og.freq(), 10u);

    og.set_valid_cell_count(12345u);
    CHECK_EQ(og.valid_cell_count(), 12345u);

    og.set_resolution(0.05f);
    CHECK_EQ(og.resolution(), doctest::Approx(0.05f));

    og.set_origin_x(-10.0f);
    CHECK_EQ(og.origin_x(), doctest::Approx(-10.0f));

    og.set_origin_y(-20.0f);
    CHECK_EQ(og.origin_y(), doctest::Approx(-20.0f));

    og.set_origin_z(1.5f);
    CHECK_EQ(og.origin_z(), doctest::Approx(1.5f));

    og.set_origin_yaw(1.57f);
    CHECK_EQ(og.origin_yaw(), doctest::Approx(1.57f));

    og.set_value_min(-1.0f);
    CHECK_EQ(og.value_min(), doctest::Approx(-1.0f));

    og.set_value_max(100.0f);
    CHECK_EQ(og.value_max(), doctest::Approx(100.0f));

    og.set_default_value(-1);
    CHECK_EQ(og.default_value(), -1);

    og.set_occupied_threshold(0.65f);
    CHECK_EQ(og.occupied_threshold(), doctest::Approx(0.65f));

    og.set_free_threshold(0.20f);
    CHECK_EQ(og.free_threshold(), doctest::Approx(0.20f));

    og.set_cell_type(zerocopy::OccupancyGrid::kCellInt8);
    CHECK_EQ(og.cell_type(), zerocopy::OccupancyGrid::kCellInt8);

    og.set_update_time_ns(1234567890ull);
    CHECK_EQ(og.update_time_ns(), 1234567890ull);
  }

  TEST_CASE("set_map_id stores identifier and truncates oversize input") {
    zerocopy::OccupancyGrid og;

    og.set_map_id("local_map");
    CHECK_EQ(std::string(og.map_id()), "local_map");

    SUBCASE("oversize input is truncated to fit") {
      og.set_map_id("this_string_is_definitely_longer_than_sixteen_bytes");
      CHECK_LE(og.map_id().size(), 15u);
    }

    SUBCASE("empty input clears identifier") {
      og.set_map_id("");
      CHECK(og.map_id().empty());
    }
  }

  TEST_CASE("create succeeds for representative sizes and sets ownership") {
    size_t sz = 0;

    SUBCASE("single byte") { sz = 1; }
    SUBCASE("small") { sz = 64; }
    SUBCASE("typical 400x400 int8") { sz = 400u * 400u; }

    zerocopy::OccupancyGrid og;
    CHECK(og.create(sz));
    CHECK(og.is_valid());
    CHECK(og.is_owner());
    CHECK_EQ(og.size(), sz);
    CHECK_NE(og.data(), nullptr);
  }

  TEST_CASE("create with zero size returns false") {
    zerocopy::OccupancyGrid og;

    CHECK_FALSE(og.create(0));
    CHECK_FALSE(og.is_valid());
  }

  TEST_CASE("create replaces previous owned buffer") {
    zerocopy::OccupancyGrid og;

    REQUIRE(og.create(100));
    CHECK_EQ(og.size(), 100u);

    REQUIRE(og.create(200));
    CHECK_EQ(og.size(), 200u);
    CHECK(og.is_owner());
  }

  TEST_CASE("clear resets all fields including header") {
    zerocopy::OccupancyGrid og;

    og.set_width(400u);
    og.set_height(300u);
    og.set_cell_type(zerocopy::OccupancyGrid::kCellFloat32);
    og.set_map_id("local_map");
    og.set_update_time_ns(99999u);
    og.create(1024);
    og.header.seq = 11u;

    og.clear();

    CHECK_FALSE(og.is_valid());
    CHECK_FALSE(og.is_owner());
    CHECK_EQ(og.size(), 0u);
    CHECK_EQ(og.data(), nullptr);
    CHECK_EQ(og.width(), 0u);
    CHECK_EQ(og.height(), 0u);
    CHECK_EQ(og.cell_type(), zerocopy::OccupancyGrid::kCellUnknown);
    CHECK(og.map_id().empty());
    CHECK_EQ(og.update_time_ns(), 0u);
    CHECK_EQ(og.header.seq, 0u);
  }

  TEST_CASE("clear then create again succeeds") {
    zerocopy::OccupancyGrid og;

    og.create(100);
    og.clear();
    CHECK_FALSE(og.is_valid());

    CHECK(og.create(50));
    CHECK(og.is_valid());
    CHECK_EQ(og.size(), 50u);
  }

  TEST_CASE("get_reserved2 is writable and not reset by clear") {
    zerocopy::OccupancyGrid og;

    og.get_reserved2() = 0xDEADBEEFu;
    CHECK_EQ(og.get_reserved2(), 0xDEADBEEFu);

    og.create(64);
    og.clear();

    CHECK_EQ(og.get_reserved2(), 0xDEADBEEFu);
  }

  TEST_CASE("shallow_copy from OccupancyGrid aliases buffer and copies metadata") {
    zerocopy::OccupancyGrid src;

    src.set_width(400u);
    src.set_height(300u);
    src.set_resolution(0.05f);
    src.set_origin_x(-10.0f);
    src.set_origin_y(-20.0f);
    src.set_origin_yaw(0.5f);
    src.set_cell_type(zerocopy::OccupancyGrid::kCellUint8);
    src.set_default_value(255);
    src.set_value_min(0.0f);
    src.set_value_max(255.0f);
    src.set_occupied_threshold(0.7f);
    src.set_free_threshold(0.2f);
    src.set_map_id("global");
    src.create(256);

    zerocopy::OccupancyGrid dst;
    CHECK(dst.shallow_copy(src));

    CHECK(dst.is_valid());
    CHECK_FALSE(dst.is_owner());
    CHECK_EQ(dst.size(), 256u);
    CHECK_EQ(dst.data(), src.data());
    CHECK_EQ(dst.width(), 400u);
    CHECK_EQ(dst.height(), 300u);
    CHECK_EQ(dst.resolution(), doctest::Approx(0.05f));
    CHECK_EQ(dst.cell_type(), zerocopy::OccupancyGrid::kCellUint8);
    CHECK_EQ(dst.default_value(), 255);
    CHECK_EQ(dst.occupied_threshold(), doctest::Approx(0.7f));
    CHECK_EQ(dst.free_threshold(), doctest::Approx(0.2f));
    CHECK_EQ(std::string(dst.map_id()), "global");
  }

  TEST_CASE("shallow_copy self returns false") {
    zerocopy::OccupancyGrid og;

    og.create(32);
    CHECK_FALSE(og.shallow_copy(og));
  }

  TEST_CASE("shallow_copy from raw pointer aliases the buffer") {
    std::vector<uint8_t> buf(64, 0xABu);

    zerocopy::OccupancyGrid og;
    CHECK(og.shallow_copy(buf.data(), buf.size()));

    CHECK(og.is_valid());
    CHECK_FALSE(og.is_owner());
    CHECK_EQ(og.size(), 64u);
    CHECK_EQ(og.data(), buf.data());
  }

  TEST_CASE("shallow_copy from raw pointer rejects null or zero size") {
    zerocopy::OccupancyGrid og;

    CHECK_FALSE(og.shallow_copy(nullptr, 64));

    std::vector<uint8_t> buf(8, 0xFFu);
    CHECK_FALSE(og.shallow_copy(buf.data(), 0));
  }

  TEST_CASE("shallow_copy same raw pointer returns false") {
    std::vector<uint8_t> buf(64, 0x55u);

    zerocopy::OccupancyGrid og;
    CHECK(og.shallow_copy(buf.data(), buf.size()));
    CHECK_FALSE(og.shallow_copy(buf.data(), buf.size()));
  }

  TEST_CASE("deep_copy from OccupancyGrid allocates owned copy") {
    zerocopy::OccupancyGrid src;

    src.set_width(64u);
    src.set_height(64u);
    src.set_cell_type(zerocopy::OccupancyGrid::kCellInt8);
    src.create(64u * 64u);
    fill_pattern(const_cast<uint8_t*>(src.data()), src.size(), 0x6Bu);

    zerocopy::OccupancyGrid dst;
    CHECK(dst.deep_copy(src));

    CHECK(dst.is_valid());
    CHECK(dst.is_owner());
    CHECK_EQ(dst.size(), src.size());
    CHECK_EQ(dst.width(), 64u);
    CHECK_EQ(dst.height(), 64u);
    CHECK_NE(dst.data(), src.data());
    CHECK(region_matches(src.data(), dst.data(), src.size()));
  }

  TEST_CASE("deep_copy into same-size owned buffer reuses memory") {
    zerocopy::OccupancyGrid src;
    src.create(128);
    fill_pattern(const_cast<uint8_t*>(src.data()), 128, 0xAAu);

    zerocopy::OccupancyGrid dst;
    dst.create(128);

    CHECK(dst.deep_copy(src));
    CHECK(dst.is_owner());
    CHECK_EQ(dst.size(), 128u);
    CHECK(region_matches(src.data(), dst.data(), 128));
  }

  TEST_CASE("deep_copy self returns false") {
    zerocopy::OccupancyGrid og;

    og.create(64);
    CHECK_FALSE(og.deep_copy(og));
  }

  TEST_CASE("deep_copy from raw pointer") {
    static constexpr size_t kN = 256;
    std::vector<uint8_t> src(kN);
    fill_pattern(src.data(), kN, 0x33u);

    zerocopy::OccupancyGrid og;
    CHECK(og.deep_copy(src.data(), kN));

    CHECK(og.is_valid());
    CHECK(og.is_owner());
    CHECK_EQ(og.size(), kN);
    CHECK(region_matches(src.data(), og.data(), kN));
  }

  TEST_CASE("deep_copy from raw pointer rejects self buffer and resizes owned storage") {
    zerocopy::OccupancyGrid og;
    REQUIRE(og.create(16));
    auto* original = const_cast<uint8_t*>(og.data());
    original[0] = 0xA5u;
    CHECK_FALSE(og.shallow_copy(original + 1, og.size() - 1));
    CHECK_FALSE(og.deep_copy(original + 1, og.size() - 1));
    CHECK_FALSE(og.deep_copy(original, og.size()));
    CHECK(og.is_owner());
    CHECK_EQ(og.data(), original);
    CHECK_EQ(og.data()[0], 0xA5u);

    zerocopy::OccupancyGrid borrowed;
    REQUIRE(borrowed.shallow_copy(original, og.size()));
    CHECK_FALSE(og.shallow_copy(borrowed));
    CHECK_FALSE(og.deep_copy(borrowed));

    std::vector<uint8_t> larger(128);
    fill_pattern(larger.data(), larger.size(), 0x51u);
    CHECK(og.deep_copy(larger.data(), larger.size()));
    CHECK(og.is_owner());
    CHECK_EQ(og.size(), larger.size());
    CHECK(region_matches(larger.data(), og.data(), larger.size()));
  }

  TEST_CASE("deep_copy from raw pointer rejects null or zero size") {
    zerocopy::OccupancyGrid og;

    CHECK_FALSE(og.deep_copy(nullptr, 64));

    std::vector<uint8_t> buf(8);
    CHECK_FALSE(og.deep_copy(buf.data(), 0));
  }

  TEST_CASE("fill_data is an alias for deep_copy from raw pointer") {
    static constexpr size_t kN = 80;
    std::vector<uint8_t> src(kN, 0xFEu);

    zerocopy::OccupancyGrid og;
    CHECK(og.fill_data(src.data(), kN));

    CHECK(og.is_owner());
    CHECK(region_matches(src.data(), og.data(), kN));
  }

  TEST_CASE("fill_data rejects null or zero size") {
    zerocopy::OccupancyGrid og;

    CHECK_FALSE(og.fill_data(nullptr, 64));

    std::vector<uint8_t> buf(8);
    CHECK_FALSE(og.fill_data(buf.data(), 0));
  }

  TEST_CASE("move_copy transfers ownership and invalidates source") {
    zerocopy::OccupancyGrid src;

    src.set_width(50u);
    src.set_height(50u);
    src.set_cell_type(zerocopy::OccupancyGrid::kCellUint16);
    src.create(50u * 50u * 2u);
    const uint8_t* original_ptr = src.data();

    zerocopy::OccupancyGrid dst;
    CHECK(dst.move_copy(src));

    CHECK(dst.is_valid());
    CHECK(dst.is_owner());
    CHECK_EQ(dst.data(), original_ptr);
    CHECK_EQ(dst.width(), 50u);
    CHECK_EQ(dst.cell_type(), zerocopy::OccupancyGrid::kCellUint16);

    CHECK_FALSE(src.is_valid());
    CHECK_FALSE(src.is_owner());
    CHECK_EQ(src.data(), nullptr);
    CHECK_EQ(src.size(), 0u);
    CHECK_EQ(src.width(), 0u);
    CHECK_EQ(src.cell_type(), zerocopy::OccupancyGrid::kCellUnknown);
  }

  TEST_CASE("move_copy self returns false") {
    zerocopy::OccupancyGrid og;

    og.create(32);
    CHECK_FALSE(og.move_copy(og));
  }

  TEST_CASE("copy constructor performs deep copy") {
    zerocopy::OccupancyGrid src;

    src.set_width(10u);
    src.set_height(10u);
    src.set_cell_type(zerocopy::OccupancyGrid::kCellFloat32);
    src.set_map_id("scratch");
    src.create(10u * 10u * 4u);
    fill_pattern(const_cast<uint8_t*>(src.data()), src.size(), 0x9Au);

    zerocopy::OccupancyGrid copy(src);

    CHECK(copy.is_owner());
    CHECK_EQ(copy.size(), src.size());
    CHECK_EQ(copy.width(), 10u);
    CHECK_EQ(copy.height(), 10u);
    CHECK_EQ(copy.cell_type(), zerocopy::OccupancyGrid::kCellFloat32);
    CHECK_EQ(std::string(copy.map_id()), "scratch");
    CHECK_NE(copy.data(), src.data());
    CHECK(region_matches(src.data(), copy.data(), src.size()));
  }

  TEST_CASE("move constructor transfers ownership") {
    zerocopy::OccupancyGrid src;

    src.create(48);
    fill_pattern(const_cast<uint8_t*>(src.data()), 48, 0x5Cu);
    const uint8_t* ptr = src.data();

    zerocopy::OccupancyGrid moved(std::move(src));

    CHECK(moved.is_owner());
    CHECK_EQ(moved.size(), 48u);
    CHECK_EQ(moved.data(), ptr);
    CHECK_FALSE(src.is_valid());
  }

  TEST_CASE("copy assignment performs deep copy") {
    zerocopy::OccupancyGrid src;

    src.set_cell_type(zerocopy::OccupancyGrid::kCellInt8);
    src.create(80);
    fill_pattern(const_cast<uint8_t*>(src.data()), 80, 0x4Du);

    zerocopy::OccupancyGrid dst;
    dst = src;

    CHECK(dst.is_owner());
    CHECK_EQ(dst.size(), 80u);
    CHECK_EQ(dst.cell_type(), zerocopy::OccupancyGrid::kCellInt8);
    CHECK(region_matches(src.data(), dst.data(), 80));
  }

  TEST_CASE("move assignment transfers ownership") {
    zerocopy::OccupancyGrid src;

    src.create(96);
    const uint8_t* ptr = src.data();

    zerocopy::OccupancyGrid dst;
    dst = std::move(src);

    CHECK(dst.is_owner());
    CHECK_EQ(dst.data(), ptr);
    CHECK_FALSE(src.is_valid());
  }

  TEST_CASE("serialize and deserialize round-trip preserves all fields") {
    zerocopy::OccupancyGrid src;

    src.set_width(40u);
    src.set_height(30u);
    src.set_channel(2u);
    src.set_freq(5u);
    src.set_valid_cell_count(99u);
    src.set_resolution(0.1f);
    src.set_origin_x(-5.0f);
    src.set_origin_y(-7.5f);
    src.set_origin_z(0.5f);
    src.set_origin_yaw(0.785f);
    src.set_value_min(-1.0f);
    src.set_value_max(100.0f);
    src.set_default_value(-1);
    src.set_occupied_threshold(0.65f);
    src.set_free_threshold(0.20f);
    src.set_cell_type(zerocopy::OccupancyGrid::kCellInt8);
    src.set_map_id("global_map");
    src.set_update_time_ns(987654321ull);

    static constexpr size_t kCells = 40u * 30u;
    REQUIRE(src.create(kCells));

    fill_pattern(const_cast<uint8_t*>(src.data()), kCells, 0xDEu);

    src.header.seq = 7u;
    std::strncpy(src.header.frame_id, "map", sizeof(src.header.frame_id) - 1);
    src.header.frame_id[sizeof(src.header.frame_id) - 1] = '\0';
    src.header.time_meas = 111111u;
    src.header.time_pub = 222222u;

    Bytes wire;
    CHECK((src >> wire));
    CHECK(zerocopy::OccupancyGrid::check_valid(wire));
    CHECK_EQ(wire.size(), src.get_serialized_size());
    uintptr_t serialized_pointer = 1;
    std::memcpy(&serialized_pointer, wire.data() + 8u + 40u, sizeof(serialized_pointer));
    CHECK_EQ(serialized_pointer, 0u);

    zerocopy::OccupancyGrid dst;
    CHECK((dst << wire));

    CHECK(dst.is_valid());
    CHECK_FALSE(dst.is_owner());
    CHECK_EQ(dst.size(), kCells);
    CHECK_EQ(dst.width(), 40u);
    CHECK_EQ(dst.height(), 30u);
    CHECK_EQ(dst.channel(), 2u);
    CHECK_EQ(dst.freq(), 5u);
    CHECK_EQ(dst.valid_cell_count(), 99u);
    CHECK_EQ(dst.resolution(), doctest::Approx(0.1f));
    CHECK_EQ(dst.origin_x(), doctest::Approx(-5.0f));
    CHECK_EQ(dst.origin_y(), doctest::Approx(-7.5f));
    CHECK_EQ(dst.origin_z(), doctest::Approx(0.5f));
    CHECK_EQ(dst.origin_yaw(), doctest::Approx(0.785f));
    CHECK_EQ(dst.value_min(), doctest::Approx(-1.0f));
    CHECK_EQ(dst.value_max(), doctest::Approx(100.0f));
    CHECK_EQ(dst.default_value(), -1);
    CHECK_EQ(dst.occupied_threshold(), doctest::Approx(0.65f));
    CHECK_EQ(dst.free_threshold(), doctest::Approx(0.20f));
    CHECK_EQ(dst.cell_type(), zerocopy::OccupancyGrid::kCellInt8);
    CHECK_EQ(std::string(dst.map_id()), "global_map");
    CHECK_EQ(dst.update_time_ns(), 987654321ull);
    CHECK_EQ(dst.header.seq, 7u);
    CHECK_EQ(std::string(dst.header.frame_id), "map");
    CHECK_EQ(dst.header.time_meas, 111111u);
    CHECK_EQ(dst.header.time_pub, 222222u);
    CHECK(region_matches(src.data(), dst.data(), kCells));
  }

  TEST_CASE("serialize empty grid produces valid wire buffer") {
    zerocopy::OccupancyGrid og;

    Bytes wire;
    CHECK((og >> wire));
    CHECK(zerocopy::OccupancyGrid::check_valid(wire));

    zerocopy::OccupancyGrid og2;
    CHECK((og2 << wire));
    CHECK_EQ(og2.width(), 0u);
    CHECK_EQ(og2.height(), 0u);
  }

  TEST_CASE("check_valid rejects empty, corrupted begin magic, and corrupted end magic") {
    zerocopy::OccupancyGrid og;
    og.create(128);

    Bytes wire;
    og >> wire;

    SUBCASE("empty bytes") {
      Bytes empty;
      CHECK_FALSE(zerocopy::OccupancyGrid::check_valid(empty));
    }

    SUBCASE("corrupted begin magic") {
      wire[0] ^= 0xFFu;
      CHECK_FALSE(zerocopy::OccupancyGrid::check_valid(wire));
    }

    SUBCASE("corrupted end magic") {
      wire[wire.size() - 1] ^= 0xFFu;
      CHECK_FALSE(zerocopy::OccupancyGrid::check_valid(wire));
    }
  }

  TEST_CASE("deserialize from too-small buffer returns false") {
    std::vector<uint8_t> raw(4, 0x00u);
    Bytes too_small(raw);

    zerocopy::OccupancyGrid og;
    CHECK_FALSE((og << too_small));
  }

  TEST_CASE("get_serialized_size equals magic + version + struct + payload + magic") {
    zerocopy::OccupancyGrid og;

    SUBCASE("empty") {
      size_t expected = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(zerocopy::OccupancyGrid) + 0u + sizeof(uint32_t);
      CHECK_EQ(og.get_serialized_size(), expected);
    }

    SUBCASE("with payload") {
      og.create(100);
      size_t expected = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(zerocopy::OccupancyGrid) + 100u + sizeof(uint32_t);
      CHECK_EQ(og.get_serialized_size(), expected);
    }
  }

  TEST_CASE("is_valid returns false when data is null or size is zero") {
    zerocopy::OccupancyGrid og;
    CHECK_FALSE(og.is_valid());

    og.create(1);
    CHECK(og.is_valid());

    og.clear();
    CHECK_FALSE(og.is_valid());
  }

  TEST_CASE("deserialize rejects a wire payload whose major version differs") {
    zerocopy::OccupancyGrid src;
    REQUIRE(src.create(64));

    Bytes wire;
    REQUIRE((src >> wire));
    REQUIRE(zerocopy::OccupancyGrid::check_valid(wire));

    uint32_t foreign_version = (zerocopy::version_major(zerocopy::kWireVersion) + 1U) * 1000000U;
    std::memcpy(wire.data() + sizeof(uint32_t), &foreign_version, sizeof(foreign_version));

    CHECK_FALSE(zerocopy::OccupancyGrid::check_valid(wire));

    zerocopy::OccupancyGrid dst;
    CHECK_FALSE((dst << wire));
  }

  TEST_CASE("wire operations reuse output and reject inconsistent embedded size") {
    zerocopy::OccupancyGrid src;
    src.set_width(8u);
    src.set_height(4u);
    REQUIRE(src.create(32));

    Bytes wire = Bytes::create(src.get_serialized_size());
    uint8_t* original = wire.data();
    CHECK((src >> wire));
    CHECK_EQ(wire.data(), original);

    zerocopy::OccupancyGrid dst;
    REQUIRE(dst.create(4));
    REQUIRE(dst.is_owner());
    CHECK((dst << wire));
    CHECK_FALSE(dst.is_owner());
    CHECK_EQ(dst.size(), 32u);
    CHECK_EQ(dst.width(), 8u);
    CHECK_EQ(dst.height(), 4u);

    CHECK_FALSE((dst << Bytes()));

    static constexpr size_t kWireStructOffset = sizeof(uint32_t) + sizeof(uint32_t);
    static constexpr size_t kSizeOffset = 48u;
    const size_t impossible_size = src.size() + 1u;
    std::memcpy(wire.data() + kWireStructOffset + kSizeOffset, &impossible_size, sizeof(impossible_size));

    CHECK_FALSE((dst << wire));
    CHECK_FALSE(dst.is_valid());
  }
}

// NOLINTEND
