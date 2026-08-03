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

#include "./zerocopy/point_cloud.h"

#include <doctest/doctest.h>

#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../common_test.h"

namespace {

inline void copy_assign_point_cloud(zerocopy::PointCloud& lhs, const zerocopy::PointCloud& rhs) { lhs = rhs; }

inline void move_assign_point_cloud(zerocopy::PointCloud& lhs, zerocopy::PointCloud& rhs) { lhs = std::move(rhs); }

}  // namespace

TEST_SUITE("zerocopy-PointCloud") {
  TEST_CASE("default construction yields invalid empty cloud") {
    zerocopy::PointCloud pc;

    CHECK_FALSE(pc.is_valid());
    CHECK_EQ(pc.size(), 0u);
    CHECK_EQ(pc.pack_size(), 0u);
    CHECK_FALSE(pc.is_owner());
    CHECK_EQ(pc.get_internal_data(), nullptr);
    CHECK_EQ(pc.get_reserved_size(), 0u);
  }

  TEST_CASE("sizeof is exactly 256 bytes") { CHECK_EQ(sizeof(zerocopy::PointCloud), 256u); }

  TEST_CASE("Vector3f default constructs to zero") {
    zerocopy::PointCloud::Vector3f v;

    CHECK_EQ(v.x, doctest::Approx(0.0f));
    CHECK_EQ(v.y, doctest::Approx(0.0f));
    CHECK_EQ(v.z, doctest::Approx(0.0f));
  }

  TEST_CASE("Vector3f value constructor sets all components") {
    zerocopy::PointCloud::Vector3f v(1.0f, 2.0f, 3.0f);

    CHECK_EQ(v.x, doctest::Approx(1.0f));
    CHECK_EQ(v.y, doctest::Approx(2.0f));
    CHECK_EQ(v.z, doctest::Approx(3.0f));
  }

  TEST_CASE("Vector3d default constructs to zero") {
    zerocopy::PointCloud::Vector3d v;

    CHECK_EQ(v.x, doctest::Approx(0.0));
    CHECK_EQ(v.y, doctest::Approx(0.0));
    CHECK_EQ(v.z, doctest::Approx(0.0));
  }

  TEST_CASE("Vector3d value constructor sets all components") {
    zerocopy::PointCloud::Vector3d v(1.1, 2.2, 3.3);

    CHECK_EQ(v.x, doctest::Approx(1.1));
    CHECK_EQ(v.y, doctest::Approx(2.2));
    CHECK_EQ(v.z, doctest::Approx(3.3));
  }

  TEST_CASE("create_v3f with xyz only sets pack_size 12 and correct capacity") {
    zerocopy::PointCloud pc;

    CHECK(pc.create_v3f<>(100, {}));

    CHECK(pc.is_owner());
    CHECK_EQ(pc.pack_size(), 12u);
    CHECK_EQ(pc.size(), 0u);
    CHECK_EQ(pc.get_reserved_size(), 100u);
  }

  TEST_CASE("create_v3f with extra float field sets pack_size 16") {
    zerocopy::PointCloud pc;

    CHECK(pc.create_v3f<float>(1000, {"intensity"}));

    CHECK(pc.is_owner());
    CHECK_EQ(pc.pack_size(), 16u);
    CHECK_EQ(pc.get_reserved_size(), 1000u);
  }

  TEST_CASE("create_v3d with xyz only sets pack_size 24") {
    zerocopy::PointCloud pc;

    CHECK(pc.create_v3d<>(200, {}));

    CHECK(pc.is_owner());
    CHECK_EQ(pc.pack_size(), 24u);
    CHECK_EQ(pc.get_reserved_size(), 200u);
  }

  TEST_CASE("type-safe create with three floats sets pack_size 12") {
    zerocopy::PointCloud pc;

    CHECK((pc.create<float, float, float>(10, {"a", "b", "c"})));

    CHECK_EQ(pc.pack_size(), 12u);
    CHECK_EQ(pc.get_reserved_size(), 10u);
  }

  TEST_CASE("low-level create with raw protocol params matches type-safe create") {
    zerocopy::PointCloud ref;
    ref.create<float, float, float>(10, {"a", "b", "c"});
    uint64_t size_num = ref.get_protocol_size_num();
    uint64_t type_num = ref.get_protocol_type_num();

    zerocopy::PointCloud pc;
    CHECK(pc.create(10, size_num, type_num, "a,b,c"));

    CHECK(pc.is_owner());
    CHECK_EQ(pc.pack_size(), 12u);
    CHECK_EQ(pc.get_reserved_size(), 10u);

    auto key_map = pc.get_key_map();
    CHECK_GT(key_map.count("a"), 0u);
    CHECK_GT(key_map.count("b"), 0u);
    CHECK_GT(key_map.count("c"), 0u);
  }

  TEST_CASE("push_value_v3f increments size") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<float>(10, {"intensity"}));

    CHECK(pc.push_value_v3f(1.0f, 2.0f, 3.0f, 0.5f));
    CHECK(pc.push_value_v3f(4.0f, 5.0f, 6.0f, 0.8f));

    CHECK_EQ(pc.size(), 2u);
  }

  TEST_CASE("push_value_v3f overflow returns false") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(2, {}));

    CHECK(pc.push_value_v3f(1.0f, 2.0f, 3.0f));
    CHECK(pc.push_value_v3f(4.0f, 5.0f, 6.0f));
    CHECK_FALSE(pc.push_value_v3f(7.0f, 8.0f, 9.0f));
  }

  TEST_CASE("push_value_v3f via Vector3f struct overload") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(4, {}));

    zerocopy::PointCloud::Vector3f v(7.0f, 8.0f, 9.0f);
    CHECK(pc.push_value_v3f(v));
    CHECK_EQ(pc.size(), 1u);
  }

  TEST_CASE("push_value_v3d and get_value_v3d round-trip") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3d<>(10, {}));

    CHECK(pc.push_value_v3d(1.1, 2.2, 3.3));

    double x = 0;
    double y = 0;
    double z = 0;
    CHECK(pc.get_value_v3d(x, y, z, 0));
    CHECK_EQ(x, doctest::Approx(1.1));
    CHECK_EQ(y, doctest::Approx(2.2));
    CHECK_EQ(z, doctest::Approx(3.3));
  }

  TEST_CASE("push_value_v3d via Vector3d struct overload") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3d<>(4, {}));

    zerocopy::PointCloud::Vector3d v(4.4, 5.5, 6.6);
    CHECK(pc.push_value_v3d(v));

    zerocopy::PointCloud::Vector3d out;
    CHECK(pc.get_value_v3d(out, 0));
    CHECK_EQ(out.x, doctest::Approx(4.4));
    CHECK_EQ(out.y, doctest::Approx(5.5));
    CHECK_EQ(out.z, doctest::Approx(6.6));
  }

  TEST_CASE("get_value_v3f reads correct xyz components") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<float>(10, {"intensity"}));

    pc.push_value_v3f(1.0f, 2.0f, 3.0f, 0.9f);
    pc.push_value_v3f(4.0f, 5.0f, 6.0f, 0.1f);

    float x = 0;
    float y = 0;
    float z = 0;

    CHECK(pc.get_value_v3f(x, y, z, 0));
    CHECK_EQ(x, doctest::Approx(1.0f));
    CHECK_EQ(y, doctest::Approx(2.0f));
    CHECK_EQ(z, doctest::Approx(3.0f));

    CHECK(pc.get_value_v3f(x, y, z, 1));
    CHECK_EQ(x, doctest::Approx(4.0f));
    CHECK_EQ(y, doctest::Approx(5.0f));
    CHECK_EQ(z, doctest::Approx(6.0f));
  }

  TEST_CASE("get_value_v3f out of range returns false") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(5, {}));
    pc.push_value_v3f(0.0f, 0.0f, 0.0f);

    float x = 0;
    float y = 0;
    float z = 0;
    CHECK_FALSE(pc.get_value_v3f(x, y, z, 99));
  }

  TEST_CASE("get_value_v3f via struct overload") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(4, {}));
    pc.push_value_v3f(7.0f, 8.0f, 9.0f);

    zerocopy::PointCloud::Vector3f v;
    CHECK(pc.get_value_v3f(v, 0));
    CHECK_EQ(v.x, doctest::Approx(7.0f));
    CHECK_EQ(v.y, doctest::Approx(8.0f));
    CHECK_EQ(v.z, doctest::Approx(9.0f));
  }

  TEST_CASE("get_value_v3f return overload gives correct vector") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(4, {}));
    pc.push_value_v3f(11.0f, 22.0f, 33.0f);

    auto v = pc.get_value_v3f(0u);
    CHECK_EQ(v.x, doctest::Approx(11.0f));
    CHECK_EQ(v.y, doctest::Approx(22.0f));
    CHECK_EQ(v.z, doctest::Approx(33.0f));
  }

  TEST_CASE("get_value via key_map retrieves named fields") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<float>(10, {"intensity"}));
    pc.push_value_v3f(1.5f, 2.5f, 3.5f, 0.75f);

    auto key_map = pc.get_key_map();

    CHECK_EQ(pc.get_value<float>(0u, key_map, "x"), doctest::Approx(1.5f));
    CHECK_EQ(pc.get_value<float>(0u, key_map, "y"), doctest::Approx(2.5f));
    CHECK_EQ(pc.get_value<float>(0u, key_map, "z"), doctest::Approx(3.5f));
    CHECK_EQ(pc.get_value<float>(0u, key_map, "intensity"), doctest::Approx(0.75f));

    const char key_storage[] = "x_suffix";
    const std::string_view key(key_storage, 1u);
    CHECK_EQ(pc.get_value<float>(0u, key_map, key), doctest::Approx(1.5f));
  }

  TEST_CASE("get_value with nonexistent key zeroes output and returns false") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(4, {}));
    pc.push_value_v3f(1.0f, 2.0f, 3.0f);

    auto key_map = pc.get_key_map();
    float val = 99.0f;

    CHECK_FALSE(pc.get_value<float>(val, 0u, key_map, "nonexistent"));
    CHECK_EQ(val, doctest::Approx(0.0f));
  }

  TEST_CASE("get_value with byte offset overload reads field directly") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<float>(10, {"intensity"}));
    pc.push_value_v3f(5.0f, 6.0f, 7.0f, 0.42f);

    float xv = 0;
    CHECK(pc.get_value<float>(xv, 0u, static_cast<uint16_t>(0)));
    CHECK_EQ(xv, doctest::Approx(5.0f));
  }

  TEST_CASE("resize adjusts logical point count without reallocation") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(5, {}));

    for (int i = 0; i < 5; ++i) {
      pc.push_value_v3f(static_cast<float>(i), 0.0f, 0.0f);
    }

    CHECK(pc.resize(3));
    CHECK_EQ(pc.size(), 3u);

    CHECK_FALSE(pc.resize(6));
    CHECK_EQ(pc.size(), 3u);
  }

  TEST_CASE("set_value_v3f overwrites an existing point") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(5, {}));

    for (int i = 0; i < 5; ++i) {
      pc.push_value_v3f(static_cast<float>(i), 0.0f, 0.0f);
    }

    REQUIRE(pc.resize(3));
    CHECK(pc.set_value_v3f(0u, 10.0f, 20.0f, 30.0f));

    float x = 0;
    float y = 0;
    float z = 0;
    CHECK(pc.get_value_v3f(x, y, z, 0));
    CHECK_EQ(x, doctest::Approx(10.0f));
    CHECK_EQ(y, doctest::Approx(20.0f));
    CHECK_EQ(z, doctest::Approx(30.0f));
  }

  TEST_CASE("set_value_v3f via Vector3f overload") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(3, {}));

    for (int i = 0; i < 3; ++i) {
      pc.push_value_v3f(0.0f, 0.0f, 0.0f);
    }

    REQUIRE(pc.resize(3));
    zerocopy::PointCloud::Vector3f v(1.0f, 2.0f, 3.0f);
    CHECK(pc.set_value_v3f(1u, v));

    auto out = pc.get_value_v3f(1u);
    CHECK_EQ(out.x, doctest::Approx(1.0f));
  }

  TEST_CASE("set_value_v3d overwrites an existing point") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3d<>(4, {}));

    for (int i = 0; i < 4; ++i) {
      pc.push_value_v3d(0.0, 0.0, 0.0);
    }

    REQUIRE(pc.resize(4));
    CHECK(pc.set_value_v3d(2u, 1.1, 2.2, 3.3));

    double x = 0;
    double y = 0;
    double z = 0;
    CHECK(pc.get_value_v3d(x, y, z, 2));
    CHECK_EQ(x, doctest::Approx(1.1));
  }

  TEST_CASE("fill_packed_data copies a pre-packed buffer") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<>(4, {}));

    for (int i = 0; i < 4; ++i) {
      src.push_value_v3f(static_cast<float>(i), static_cast<float>(i), static_cast<float>(i));
    }

    zerocopy::PointCloud dst;
    REQUIRE(dst.create_v3f<>(4, {}));

    CHECK(dst.fill_packed_data(src.get_internal_data(), 4));
    CHECK_EQ(dst.size(), 4u);

    float x = 0;
    float y = 0;
    float z = 0;
    CHECK(dst.get_value_v3f(x, y, z, 2));
    CHECK_EQ(x, doctest::Approx(2.0f));
  }

  TEST_CASE("fill_packed_data rejects null data or zero count") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(4, {}));

    CHECK_FALSE(pc.fill_packed_data(nullptr, 4));

    std::vector<uint8_t> buf(48, 0u);
    CHECK_FALSE(pc.fill_packed_data(buf.data(), 0));
  }

  TEST_CASE("fill_packed_data rejects count exceeding capacity") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(2, {}));

    std::vector<uint8_t> buf(60, 0u);
    CHECK_FALSE(pc.fill_packed_data(buf.data(), 5));
  }

  TEST_CASE("clear false resets size but retains buffer and schema") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(10, {}, 0, true, true));
    pc.push_value_v3f(1.0f, 2.0f, 3.0f);

    pc.clear(false);

    CHECK_EQ(pc.size(), 0u);
    CHECK_GT(pc.pack_size(), 0u);
    CHECK(pc.is_owner());
    CHECK_GT(pc.get_reserved_size(), 0u);
    CHECK(pc.get_sort());
  }

  TEST_CASE("clear true fully resets to default state") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(10, {}, 0, true, true));
    pc.push_value_v3f(1.0f, 2.0f, 3.0f);

    pc.clear(true);

    CHECK_FALSE(pc.is_valid());
    CHECK_FALSE(pc.is_owner());
    CHECK_EQ(pc.size(), 0u);
    CHECK_EQ(pc.pack_size(), 0u);
    CHECK_EQ(pc.get_reserved_size(), 0u);
    CHECK_FALSE(pc.get_sort());
  }

  TEST_CASE("shallow_copy aliases the buffer without ownership") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<>(5, {}, 0, true, true));
    src.push_value_v3f(1.0f, 2.0f, 3.0f);

    zerocopy::PointCloud dst;
    CHECK(dst.shallow_copy(src));

    CHECK_FALSE(dst.is_owner());
    CHECK_EQ(dst.size(), 1u);
    CHECK_EQ(dst.get_internal_data(), src.get_internal_data());
    CHECK(dst.get_sort());
  }

  TEST_CASE("shallow_copy releases a previous owned buffer before aliasing") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<>(5, {}));
    REQUIRE(src.push_value_v3f(1.0f, 2.0f, 3.0f));

    zerocopy::PointCloud dst;
    REQUIRE(dst.create_v3f<>(2, {}));
    REQUIRE(dst.push_value_v3f(0.0f, 0.0f, 0.0f));

    CHECK(dst.shallow_copy(src));
    CHECK_FALSE(dst.is_owner());
    CHECK_EQ(dst.get_internal_data(), src.get_internal_data());
    CHECK_EQ(dst.size(), 1u);
  }

  TEST_CASE("shallow_copy self returns false") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(4, {}));

    CHECK_FALSE(pc.shallow_copy(pc));
  }

  TEST_CASE("copy helpers reject a source that borrows the destination owned buffer") {
    zerocopy::PointCloud owner;
    REQUIRE(owner.create_v3f<>(4, {}));

    zerocopy::PointCloud borrowed;
    REQUIRE(borrowed.shallow_copy(owner));

    CHECK_FALSE(owner.shallow_copy(borrowed));
    CHECK_FALSE(owner.deep_copy(borrowed));
    CHECK(owner.is_owner());
    CHECK_EQ(owner.get_reserved_size(), 4u);
  }

  TEST_CASE("deep_copy creates owned independent copy") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<>(5, {}, 0, true, true));
    src.push_value_v3f(1.0f, 2.0f, 3.0f);
    src.push_value_v3f(4.0f, 5.0f, 6.0f);

    zerocopy::PointCloud dst;
    CHECK(dst.deep_copy(src));

    CHECK(dst.is_owner());
    CHECK_EQ(dst.size(), 2u);
    CHECK_EQ(dst.pack_size(), 12u);
    CHECK_NE(dst.get_internal_data(), src.get_internal_data());
    CHECK(dst.get_sort());
  }

  TEST_CASE("deep_copy self returns false") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(4, {}));
    REQUIRE(pc.push_value_v3f(1.0f, 2.0f, 3.0f));

    CHECK_FALSE(pc.deep_copy(pc));
  }

  TEST_CASE("move_copy transfers ownership and invalidates source") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<>(10, {}, 0, true, true));
    src.push_value_v3f(0.0f, 0.0f, 0.0f);
    const uint8_t* ptr = src.get_internal_data();

    zerocopy::PointCloud dst;
    CHECK(dst.move_copy(src));

    CHECK(dst.is_owner());
    CHECK_EQ(dst.get_internal_data(), ptr);
    CHECK_EQ(dst.size(), 1u);
    CHECK_EQ(dst.get_reserved_size(), 10u);
    CHECK(dst.get_sort());
    CHECK(dst.push_value_v3f(1.0f, 2.0f, 3.0f));
    CHECK_EQ(dst.size(), 2u);
    CHECK_FALSE(src.is_valid());
    CHECK_FALSE(src.is_owner());
    CHECK_FALSE(src.get_sort());
  }

  TEST_CASE("move_copy self returns false") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(4, {}));

    CHECK_FALSE(pc.move_copy(pc));
  }

  TEST_CASE("copy constructor performs deep copy") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<float>(10, {"intensity"}));
    src.push_value_v3f(1.0f, 2.0f, 3.0f, 0.5f);

    zerocopy::PointCloud copy(src);

    CHECK(copy.is_owner());
    CHECK_EQ(copy.size(), 1u);
    CHECK_EQ(copy.pack_size(), 16u);
    CHECK_NE(copy.get_internal_data(), src.get_internal_data());
  }

  TEST_CASE("move constructor transfers ownership") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<>(4, {}));
    src.push_value_v3f(1.0f, 2.0f, 3.0f);
    const uint8_t* ptr = src.get_internal_data();

    zerocopy::PointCloud moved(std::move(src));

    CHECK(moved.is_owner());
    CHECK_EQ(moved.get_internal_data(), ptr);
    CHECK_EQ(moved.size(), 1u);
    CHECK_FALSE(src.is_valid());
  }

  TEST_CASE("copy assignment performs deep copy") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<>(4, {}));
    src.push_value_v3f(7.0f, 8.0f, 9.0f);

    zerocopy::PointCloud dst;
    dst = src;

    CHECK(dst.is_owner());
    CHECK_EQ(dst.size(), 1u);

    float x = 0;
    float y = 0;
    float z = 0;
    CHECK(dst.get_value_v3f(x, y, z, 0));
    CHECK_EQ(x, doctest::Approx(7.0f));
  }

  TEST_CASE("move assignment transfers ownership") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<>(4, {}));
    src.push_value_v3f(1.0f, 2.0f, 3.0f);
    const uint8_t* ptr = src.get_internal_data();

    zerocopy::PointCloud dst;
    dst = std::move(src);

    CHECK(dst.is_owner());
    CHECK_EQ(dst.get_internal_data(), ptr);
    CHECK_FALSE(src.is_valid());
  }

  TEST_CASE("copy and move self-assignment are stable no-ops") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(4, {}));
    REQUIRE(pc.push_value_v3f(1.0f, 2.0f, 3.0f));
    const uint8_t* ptr = pc.get_internal_data();

    const zerocopy::PointCloud& same_copy = pc;
    copy_assign_point_cloud(pc, same_copy);
    CHECK(pc.is_owner());
    CHECK_EQ(pc.get_internal_data(), ptr);
    CHECK_EQ(pc.size(), 1u);

    zerocopy::PointCloud& same_move = pc;
    move_assign_point_cloud(pc, same_move);
    CHECK(pc.is_owner());
    CHECK_EQ(pc.get_internal_data(), ptr);
    CHECK_EQ(pc.size(), 1u);
  }

  TEST_CASE("get_key_map returns correct field names and order") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<float>(4, {"intensity"}));

    zerocopy::PointCloud::KeyList key_list;
    auto key_map = pc.get_key_map(&key_list);

    CHECK_GT(key_map.count("x"), 0u);
    CHECK_GT(key_map.count("y"), 0u);
    CHECK_GT(key_map.count("z"), 0u);
    CHECK_GT(key_map.count("intensity"), 0u);

    REQUIRE_EQ(key_list.size(), 4u);
    CHECK_EQ(key_list[0].name, "x");
    CHECK_EQ(key_list[1].name, "y");
    CHECK_EQ(key_list[2].name, "z");
    CHECK_EQ(key_list[3].name, "intensity");
  }

  TEST_CASE("protocol string helpers return non-empty strings with expected content") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<float>(10, {"intensity"}));

    std::string size_str = pc.get_protocol_size_str();
    std::string name_str = pc.get_protocol_name_str();
    std::string type_str = pc.get_protocol_type_str();

    CHECK_FALSE(size_str.empty());
    CHECK_FALSE(name_str.empty());
    CHECK_FALSE(type_str.empty());

    CHECK_NE(name_str.find("x"), std::string::npos);
    CHECK_NE(name_str.find("intensity"), std::string::npos);
  }

  TEST_CASE("get_protocol_size_num and type_num are non-zero after create") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(4, {}));

    CHECK_NE(pc.get_protocol_size_num(), 0u);
    CHECK_NE(pc.get_protocol_type_num(), 0u);
  }

  TEST_CASE("get_value_for_double_float converts float field to double") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<float>(4, {"intensity"}));
    pc.push_value_v3f(3.14f, 2.71f, 1.41f, 0.99f);

    auto key_map = pc.get_key_map();
    double x_d = pc.get_value_for_double_float(0u, key_map, "x", zerocopy::PointCloud::kFloatType);

    CHECK_EQ(x_d, doctest::Approx(3.14).epsilon(0.001));
  }

  TEST_CASE("get_value_for_print returns non-empty string for known field") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(4, {}));
    pc.push_value_v3f(1.0f, 2.0f, 3.0f);

    auto key_map = pc.get_key_map();
    std::string xs = pc.get_value_for_print(0u, key_map, "x", zerocopy::PointCloud::kFloatType);

    CHECK_FALSE(xs.empty());
  }

  TEST_CASE("serialize and deserialize round-trip preserves schema and all points") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<float>(50, {"intensity"}));

    for (int i = 0; i < 5; ++i) {
      src.push_value_v3f(static_cast<float>(i), static_cast<float>(i * 2), static_cast<float>(i * 3),
                         static_cast<float>(i) * 0.1f);
    }

    src.header.seq = 42u;

    Bytes wire;
    CHECK((src >> wire));
    CHECK(zerocopy::PointCloud::check_valid(wire));
    CHECK_EQ(wire.size(), src.get_serialized_size());
    uintptr_t serialized_pointer = 1;
    std::memcpy(&serialized_pointer, wire.data() + 8u + 208u, sizeof(serialized_pointer));
    CHECK_EQ(serialized_pointer, 0u);

    zerocopy::PointCloud dst;
    CHECK((dst << wire));

    CHECK(dst.is_valid());
    CHECK_FALSE(dst.is_owner());
    CHECK_EQ(dst.size(), 5u);
    CHECK_EQ(dst.pack_size(), 16u);
    CHECK_EQ(dst.header.seq, 42u);

    auto key_map = dst.get_key_map();

    for (int i = 0; i < 5; ++i) {
      float x = dst.get_value<float>(static_cast<size_t>(i), key_map, "x");
      float y = dst.get_value<float>(static_cast<size_t>(i), key_map, "y");
      float z = dst.get_value<float>(static_cast<size_t>(i), key_map, "z");
      float inten = dst.get_value<float>(static_cast<size_t>(i), key_map, "intensity");

      CHECK_EQ(x, doctest::Approx(static_cast<float>(i)));
      CHECK_EQ(y, doctest::Approx(static_cast<float>(i * 2)));
      CHECK_EQ(z, doctest::Approx(static_cast<float>(i * 3)));
      CHECK_EQ(inten, doctest::Approx(static_cast<float>(i) * 0.1f));
    }
  }

  TEST_CASE("check_valid rejects empty, corrupted begin magic, and corrupted end magic") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(4, {}));
    pc.push_value_v3f(1.0f, 2.0f, 3.0f);

    Bytes wire;
    pc >> wire;

    SUBCASE("empty bytes") {
      Bytes empty;
      CHECK_FALSE(zerocopy::PointCloud::check_valid(empty));
    }

    SUBCASE("corrupted begin magic") {
      wire[0] ^= 0xFFu;
      CHECK_FALSE(zerocopy::PointCloud::check_valid(wire));
    }

    SUBCASE("corrupted end magic") {
      wire[wire.size() - 1] ^= 0xFFu;
      CHECK_FALSE(zerocopy::PointCloud::check_valid(wire));
    }
  }

  TEST_CASE("get_serialized_size equals magic + version + struct + payload + magic") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(5, {}));

    for (int i = 0; i < 3; ++i) {
      pc.push_value_v3f(static_cast<float>(i), 0.0f, 0.0f);
    }

    size_t expected = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(zerocopy::PointCloud) + 3u * 12u + sizeof(uint32_t);
    CHECK_EQ(pc.get_serialized_size(), expected);
  }

  TEST_CASE("default cloud reports no compression") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(4, {}));

    CHECK_EQ(static_cast<int>(pc.get_extent()), 0);
    CHECK_FALSE(pc.get_vertical());
  }

  TEST_CASE("create with extent rewrites xyz schema to int16 and shrinks pack_size") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<float>(100, {"intensity"}, 100));

    CHECK_EQ(static_cast<int>(pc.get_extent()), 100);
    CHECK_FALSE(pc.get_vertical());
    CHECK_EQ(pc.pack_size(), 10u);

    std::string type_str = pc.get_protocol_type_str();
    CHECK_EQ(type_str.find("int16"), std::string::npos);
    CHECK_NE(type_str.find("float"), std::string::npos);
  }

  TEST_CASE("compressed v3f quantizes to int16 and dequantizes on read") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<float>(100, {"intensity"}, 10));

    pc.push_value_v3f(1.234f, -5.678f, 9.012f, 0.5f);

    auto v = pc.get_value_v3f(0u);
    CHECK_EQ(v.x, doctest::Approx(1.234).epsilon(0.001));
    CHECK_EQ(v.y, doctest::Approx(-5.678).epsilon(0.001));
    CHECK_EQ(v.z, doctest::Approx(9.012).epsilon(0.001));

    auto key_map = pc.get_key_map();
    int16_t raw_x = pc.get_value<int16_t>(0u, key_map, "x");
    int16_t raw_y = pc.get_value<int16_t>(0u, key_map, "y");
    int16_t raw_z = pc.get_value<int16_t>(0u, key_map, "z");
    CHECK_EQ(static_cast<double>(raw_x) * 10.0 / 32767.0, doctest::Approx(1.234).epsilon(0.001));
    CHECK_EQ(static_cast<double>(raw_y) * 10.0 / 32767.0, doctest::Approx(-5.678).epsilon(0.001));
    CHECK_EQ(static_cast<double>(raw_z) * 10.0 / 32767.0, doctest::Approx(9.012).epsilon(0.001));
    CHECK_EQ(pc.get_value<float>(0u, key_map, "intensity"), doctest::Approx(0.5f));
  }

  TEST_CASE("compressed v3d quantizes and dequantizes on read") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3d<>(50, {}, 100));
    CHECK_EQ(pc.pack_size(), 6u);

    pc.push_value_v3d(12.34, -56.78, 90.12);

    double x = 0;
    double y = 0;
    double z = 0;
    CHECK(pc.get_value_v3d(x, y, z, 0));
    CHECK_EQ(x, doctest::Approx(12.34).epsilon(0.001));
    CHECK_EQ(y, doctest::Approx(-56.78).epsilon(0.001));
    CHECK_EQ(z, doctest::Approx(90.12).epsilon(0.001));
  }

  TEST_CASE("compressed quantization discards points outside the open extent range") {
    SUBCASE("coordinates far outside the extent are rejected") {
      zerocopy::PointCloud pc;
      REQUIRE(pc.create_v3f<>(4, {}, 10));

      CHECK_FALSE(pc.push_value_v3f(1.0e6f, -1.0e6f, 0.0f));
      CHECK_EQ(pc.size(), 0u);
    }

    SUBCASE("coordinates exactly at +/-extent are rejected because the range is open") {
      zerocopy::PointCloud pc;
      REQUIRE(pc.create_v3f<>(4, {}, 10));

      CHECK_FALSE(pc.push_value_v3f(10.0f, 0.0f, 0.0f));
      CHECK_FALSE(pc.push_value_v3f(0.0f, -10.0f, 0.0f));
      CHECK_EQ(pc.size(), 0u);
    }

    SUBCASE("coordinates strictly inside the extent are kept and quantised") {
      zerocopy::PointCloud pc;
      REQUIRE(pc.create_v3f<>(4, {}, 10));

      CHECK(pc.push_value_v3f(9.999f, -9.999f, 0.0f));
      CHECK_EQ(pc.size(), 1u);

      auto v = pc.get_value_v3f(0u);
      CHECK_EQ(v.x, doctest::Approx(9.999).epsilon(0.001));
      CHECK_EQ(v.y, doctest::Approx(-9.999).epsilon(0.001));
      CHECK_EQ(v.z, doctest::Approx(0.0));
    }
  }

  TEST_CASE("compressed quantization discards an out-of-extent point on every v3 entry point") {
    SUBCASE("push_value_v3d") {
      zerocopy::PointCloud pc;
      REQUIRE(pc.create_v3d<>(4, {}, 10));

      CHECK_FALSE(pc.push_value_v3d(100.0, 0.0, 0.0));
      CHECK_EQ(pc.size(), 0u);

      CHECK(pc.push_value_v3d(5.0, -5.0, 1.0));
      CHECK_EQ(pc.size(), 1u);
    }

    SUBCASE("set_value_v3f and set_value_v3d leave the existing point untouched") {
      zerocopy::PointCloud pc;
      REQUIRE(pc.create_v3f<>(4, {}, 10));

      REQUIRE(pc.push_value_v3f(1.0f, 2.0f, 3.0f));
      REQUIRE(pc.resize(1));

      CHECK_FALSE(pc.set_value_v3f(0u, 100.0f, 0.0f, 0.0f));
      CHECK_FALSE(pc.set_value_v3d(0u, 0.0, 100.0, 0.0));

      auto v = pc.get_value_v3f(0u);
      CHECK_EQ(v.x, doctest::Approx(1.0).epsilon(0.001));
      CHECK_EQ(v.y, doctest::Approx(2.0).epsilon(0.001));
      CHECK_EQ(v.z, doctest::Approx(3.0).epsilon(0.001));
    }
  }

  TEST_CASE("compressed quantization discards a NaN coordinate without UB") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(4, {}, 10));

    CHECK_FALSE(pc.push_value_v3f(std::numeric_limits<float>::quiet_NaN(), 1.0f, 2.0f));
    CHECK_EQ(pc.size(), 0u);
  }

  TEST_CASE("set_value_v3f on a compressed cloud overwrites with quantized value") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(5, {}, 10));

    for (int i = 0; i < 5; ++i) {
      pc.push_value_v3f(0.0f, 0.0f, 0.0f);
    }

    REQUIRE(pc.resize(5));
    CHECK(pc.set_value_v3f(2u, 1.25f, 2.5f, 3.75f));

    auto v = pc.get_value_v3f(2u);
    CHECK_EQ(v.x, doctest::Approx(1.25).epsilon(0.001));
    CHECK_EQ(v.y, doctest::Approx(2.5).epsilon(0.001));
    CHECK_EQ(v.z, doctest::Approx(3.75).epsilon(0.001));
  }

  TEST_CASE("vertical-only layout round-trips through serialization") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<float>(50, {"intensity"}, 0, true));
    CHECK_EQ(src.pack_size(), 16u);
    CHECK(src.get_vertical());

    for (int i = 0; i < 13; ++i) {
      auto fi = static_cast<float>(i);
      src.push_value_v3f(fi, fi * 2.0f, fi * 3.0f, fi * 0.1f);
    }

    Bytes wire;
    CHECK((src >> wire));
    CHECK(zerocopy::PointCloud::check_valid(wire));
    CHECK_EQ(wire.size(), src.get_serialized_size());

    zerocopy::PointCloud dst;
    CHECK((dst << wire));

    CHECK(dst.is_owner());
    CHECK_EQ(dst.size(), 13u);
    CHECK(dst.get_vertical());

    auto key_map = dst.get_key_map();

    for (int i = 0; i < 13; ++i) {
      auto idx = static_cast<size_t>(i);
      auto fi = static_cast<float>(i);
      CHECK_EQ(dst.get_value<float>(idx, key_map, "x"), doctest::Approx(fi));
      CHECK_EQ(dst.get_value<float>(idx, key_map, "y"), doctest::Approx(fi * 2.0f));
      CHECK_EQ(dst.get_value<float>(idx, key_map, "z"), doctest::Approx(fi * 3.0f));
      CHECK_EQ(dst.get_value<float>(idx, key_map, "intensity"), doctest::Approx(fi * 0.1f));
    }
  }

  TEST_CASE("set_vertical enables vertical serialization after create") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<float>(8, {"intensity"}));
    CHECK_FALSE(src.get_vertical());

    REQUIRE(src.push_value_v3f(1.0f, 2.0f, 3.0f, 0.5f));
    REQUIRE(src.push_value_v3f(4.0f, 5.0f, 6.0f, 0.75f));

    src.set_vertical(true);
    CHECK(src.get_vertical());

    Bytes wire;
    REQUIRE((src >> wire));

    zerocopy::PointCloud dst;
    REQUIRE((dst << wire));

    CHECK(dst.is_owner());
    CHECK(dst.get_vertical());
    CHECK_EQ(dst.size(), 2u);

    auto key_map = dst.get_key_map();
    CHECK_EQ(dst.get_value<float>(0u, key_map, "x"), doctest::Approx(1.0f));
    CHECK_EQ(dst.get_value<float>(0u, key_map, "y"), doctest::Approx(2.0f));
    CHECK_EQ(dst.get_value<float>(0u, key_map, "z"), doctest::Approx(3.0f));
    CHECK_EQ(dst.get_value<float>(0u, key_map, "intensity"), doctest::Approx(0.5f));
    CHECK_EQ(dst.get_value<float>(1u, key_map, "x"), doctest::Approx(4.0f));
    CHECK_EQ(dst.get_value<float>(1u, key_map, "y"), doctest::Approx(5.0f));
    CHECK_EQ(dst.get_value<float>(1u, key_map, "z"), doctest::Approx(6.0f));
    CHECK_EQ(dst.get_value<float>(1u, key_map, "intensity"), doctest::Approx(0.75f));
  }

  TEST_CASE("spatial ordering is applied during vertical serialization without changing source order") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<float>(8, {"intensity"}, 10, true, true));
    CHECK(src.get_sort());

    REQUIRE(src.push_value_v3f(0.0f, 0.0f, 1.0f, 30.0f));
    REQUIRE(src.push_value_v3f(-1.0f, 0.0f, 0.0f, -10.0f));
    REQUIRE(src.push_value_v3f(0.0f, 1.0f, 0.0f, 20.0f));
    REQUIRE(src.push_value_v3f(1.0f, 0.0f, 0.0f, 10.0f));
    REQUIRE(src.push_value_v3f(1.0f, 0.0f, 0.0f, 11.0f));

    Bytes wire;
    REQUIRE((src >> wire));
    CHECK_EQ(src.get_value_v3f(0u).z, doctest::Approx(1.0f).epsilon(0.001));

    zerocopy::PointCloud dst;
    REQUIRE((dst << wire));
    CHECK(dst.get_sort());

    auto key_map = dst.get_key_map();
    const std::array<float, 5> expected_x{-1.0f, 1.0f, 1.0f, 0.0f, 0.0f};
    const std::array<float, 5> expected_y{0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const std::array<float, 5> expected_z{0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    const std::array<float, 5> expected_intensity{-10.0f, 10.0f, 11.0f, 20.0f, 30.0f};

    for (size_t i = 0; i < expected_x.size(); ++i) {
      const auto point = dst.get_value_v3f(i);
      CHECK_EQ(point.x, doctest::Approx(expected_x[i]).epsilon(0.001));
      CHECK_EQ(point.y, doctest::Approx(expected_y[i]).epsilon(0.001));
      CHECK_EQ(point.z, doctest::Approx(expected_z[i]).epsilon(0.001));
      CHECK_EQ(dst.get_value<float>(i, key_map, "intensity"), doctest::Approx(expected_intensity[i]));
    }
  }

  TEST_CASE("spatial sort option defaults off and supports unquantised XYZ") {
    zerocopy::PointCloud legacy;
    zerocopy::PointCloud explicit_default;
    REQUIRE(legacy.create_v3f<>(4, {}, 10, true));
    REQUIRE(explicit_default.create_v3f<>(4, {}, 10, true, false));
    REQUIRE(legacy.push_value_v3f(1.0f, 2.0f, 3.0f));
    REQUIRE(explicit_default.push_value_v3f(1.0f, 2.0f, 3.0f));
    CHECK_FALSE(legacy.get_sort());
    CHECK_FALSE(explicit_default.get_sort());

    Bytes legacy_wire;
    Bytes explicit_wire;
    REQUIRE((legacy >> legacy_wire));
    REQUIRE((explicit_default >> explicit_wire));
    REQUIRE_EQ(legacy_wire.size(), explicit_wire.size());
    CHECK_EQ(std::memcmp(legacy_wire.data(), explicit_wire.data(), legacy_wire.size()), 0);

    zerocopy::PointCloud unquantised;
    REQUIRE(unquantised.create_v3f<>(2, {}, 0, true, true));
    REQUIRE(unquantised.push_value_v3f(2.0f, 0.0f, 0.0f));
    REQUIRE(unquantised.push_value_v3f(-1.0f, 0.0f, 0.0f));

    Bytes unquantised_wire;
    REQUIRE((unquantised >> unquantised_wire));

    zerocopy::PointCloud unquantised_dst;
    REQUIRE((unquantised_dst << unquantised_wire));
    CHECK_EQ(unquantised_dst.get_value_v3f(0u).x, doctest::Approx(-1.0f));

    zerocopy::PointCloud double_src;
    REQUIRE(double_src.create_v3d<int32_t>(4, {"id"}, 0, true, true));
    REQUIRE(double_src.push_value_v3d(0.0, 0.0, 1.0, 30));
    REQUIRE(double_src.push_value_v3d(-1.0, 0.0, 0.0, -10));
    REQUIRE(double_src.push_value_v3d(0.0, 1.0, 0.0, 20));
    REQUIRE(double_src.push_value_v3d(1.0, 0.0, 0.0, 10));

    Bytes double_wire;
    REQUIRE((double_src >> double_wire));

    zerocopy::PointCloud double_dst;
    REQUIRE((double_dst << double_wire));
    auto double_keys = double_dst.get_key_map();
    const std::array<int32_t, 4> expected_id{-10, 10, 20, 30};

    for (size_t i = 0; i < expected_id.size(); ++i) {
      CHECK_EQ(double_dst.get_value<int32_t>(i, double_keys, "id"), expected_id[i]);
    }

    zerocopy::PointCloud horizontal;
    REQUIRE(horizontal.create_v3f<>(2, {}, 0, false, true));
    CHECK_FALSE(horizontal.get_sort());

    zerocopy::PointCloud unsupported;
    const std::vector<std::string> xyz{"x", "y", "z"};
    CHECK_FALSE(unsupported.create<int32_t, int32_t, int32_t>(1, xyz, 0, true, true));
    CHECK_FALSE(unsupported.create<int32_t, int32_t, int32_t>(2, xyz, 0, true, true));
    CHECK_FALSE(unsupported.create<float, double, float>(2, xyz, 0, true, true));
    CHECK_FALSE(unsupported.create(2, 0x111, 0xAAA, "x,y,z", 0, true, true));
  }

  TEST_CASE("set_vertical disables vertical serialization without changing data") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<>(8, {}, 0, true, true));
    CHECK(src.get_vertical());
    CHECK(src.get_sort());

    REQUIRE(src.push_value_v3f(7.0f, 8.0f, 9.0f));
    src.set_vertical(false);
    CHECK_FALSE(src.get_vertical());
    CHECK_FALSE(src.get_sort());

    Bytes wire;
    REQUIRE((src >> wire));

    zerocopy::PointCloud dst;
    REQUIRE((dst << wire));

    CHECK_FALSE(dst.is_owner());
    CHECK_FALSE(dst.get_vertical());
    CHECK_EQ(dst.size(), 1u);

    auto v = dst.get_value_v3f(0u);
    CHECK_EQ(v.x, doctest::Approx(7.0f));
    CHECK_EQ(v.y, doctest::Approx(8.0f));
    CHECK_EQ(v.z, doctest::Approx(9.0f));

    src.set_vertical(true);
    CHECK(src.get_vertical());
    CHECK_FALSE(src.get_sort());
  }

  TEST_CASE("extent and vertical combined round-trip") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<float>(100, {"intensity"}, 10, true));
    CHECK_EQ(src.pack_size(), 10u);

    for (int i = 0; i < 10; ++i) {
      auto fi = static_cast<float>(i);
      src.push_value_v3f(fi * 0.5f, -fi * 0.25f, fi, fi * 0.1f);
    }

    Bytes wire;
    CHECK((src >> wire));
    CHECK_EQ(wire.size(), src.get_serialized_size());

    zerocopy::PointCloud dst;
    CHECK((dst << wire));

    CHECK(dst.is_owner());
    CHECK_EQ(dst.size(), 10u);
    CHECK_EQ(static_cast<int>(dst.get_extent()), 10);
    CHECK(dst.get_vertical());

    auto key_map = dst.get_key_map();

    for (int i = 0; i < 10; ++i) {
      auto idx = static_cast<size_t>(i);
      auto fi = static_cast<float>(i);
      auto v = dst.get_value_v3f(idx);
      CHECK_EQ(v.x, doctest::Approx(fi * 0.5f).epsilon(0.001));
      CHECK_EQ(v.y, doctest::Approx(-fi * 0.25f).epsilon(0.001));
      CHECK_EQ(v.z, doctest::Approx(fi).epsilon(0.001));
      CHECK_EQ(dst.get_value<float>(idx, key_map, "intensity"), doctest::Approx(fi * 0.1f));
    }
  }

  TEST_CASE("create applies extent and vertical directly") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<float>(100, {"intensity"}, 10, true));

    CHECK_EQ(pc.pack_size(), 10u);
    CHECK_EQ(static_cast<int>(pc.get_extent()), 10);
    CHECK(pc.get_vertical());

    pc.push_value_v3f(1.234f, -5.678f, 9.012f, 0.5f);

    Bytes wire;
    CHECK((pc >> wire));

    zerocopy::PointCloud dst;
    CHECK((dst << wire));

    auto v = dst.get_value_v3f(0u);
    CHECK_EQ(v.x, doctest::Approx(1.234).epsilon(0.001));
    CHECK_EQ(v.y, doctest::Approx(-5.678).epsilon(0.001));
    CHECK_EQ(v.z, doctest::Approx(9.012).epsilon(0.001));
  }

  TEST_CASE("deep copy of a compressed cloud preserves compression state and data") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<float>(10, {"intensity"}, 10, true));
    src.push_value_v3f(1.234f, 2.0f, 3.0f, 0.5f);

    zerocopy::PointCloud copy(src);

    CHECK(copy.is_owner());
    CHECK_EQ(static_cast<int>(copy.get_extent()), 10);
    CHECK(copy.get_vertical());
    CHECK_EQ(copy.pack_size(), 10u);

    auto v = copy.get_value_v3f(0u);
    CHECK_EQ(v.x, doctest::Approx(1.234).epsilon(0.001));
  }

  TEST_CASE("move of a compressed cloud transfers compression state and clears source") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<>(10, {}, 10, true));
    src.push_value_v3f(1.0f, 2.0f, 3.0f);

    zerocopy::PointCloud moved(std::move(src));

    CHECK_EQ(static_cast<int>(moved.get_extent()), 10);
    CHECK(moved.get_vertical());
    CHECK_EQ(static_cast<int>(src.get_extent()), 0);
    CHECK_FALSE(src.get_vertical());
  }

  TEST_CASE("compressed serialized size accounts for the shrunk pack_size") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(5, {}, 10));

    for (int i = 0; i < 3; ++i) {
      pc.push_value_v3f(static_cast<float>(i), 0.0f, 0.0f);
    }

    size_t expected = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(zerocopy::PointCloud) + 3u * 6u + sizeof(uint32_t);
    CHECK_EQ(pc.get_serialized_size(), expected);
  }

  TEST_CASE("downsample marks the level and collapses co-located points on a quantised cloud") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(8, {}, /*extent=*/10));

    for (int i = 0; i < 5; ++i) {
      pc.push_value_v3f(1.0f, 2.0f, 3.0f);
    }

    CHECK_EQ(pc.size(), 5u);
    CHECK_EQ(static_cast<int>(pc.get_downsample()), 0);

    CHECK(pc.downsample(200));

    CHECK_EQ(static_cast<int>(pc.get_downsample()), 200);
    CHECK_EQ(pc.size(), 1u);
  }

  TEST_CASE("downsample voxel edge equals v_q = round(level * 128 / 255) quantisation steps") {
    SUBCASE("level 255 -> v_q 128: merges within each 128-step cell across both signs, keeps the first point") {
      zerocopy::PointCloud pc;
      REQUIRE(pc.create_v3f<>(8, {}, /*extent=*/32767));

      pc.push_value_v3f(-129.0f, 0.0f, 0.0f);
      pc.push_value_v3f(-128.0f, 0.0f, 0.0f);
      pc.push_value_v3f(-1.0f, 0.0f, 0.0f);
      pc.push_value_v3f(0.0f, 0.0f, 0.0f);
      pc.push_value_v3f(127.0f, 0.0f, 0.0f);
      pc.push_value_v3f(128.0f, 0.0f, 0.0f);
      pc.push_value_v3f(255.0f, 0.0f, 0.0f);
      pc.push_value_v3f(256.0f, 0.0f, 0.0f);

      CHECK_EQ(pc.size(), 8u);

      CHECK(pc.downsample(255));

      CHECK_EQ(static_cast<int>(pc.get_downsample()), 255);
      CHECK_EQ(pc.size(), 5u);

      float x = 0;
      float y = 0;
      float z = 0;

      CHECK(pc.get_value_v3f(x, y, z, 0));
      CHECK_EQ(x, doctest::Approx(-129.0f));

      CHECK(pc.get_value_v3f(x, y, z, 1));
      CHECK_EQ(x, doctest::Approx(-128.0f));

      CHECK(pc.get_value_v3f(x, y, z, 2));
      CHECK_EQ(x, doctest::Approx(0.0f));

      CHECK(pc.get_value_v3f(x, y, z, 3));
      CHECK_EQ(x, doctest::Approx(128.0f));

      CHECK(pc.get_value_v3f(x, y, z, 4));
      CHECK_EQ(x, doctest::Approx(256.0f));
    }

    SUBCASE("level 1 -> v_q 1: adjacent quantisation steps stay distinct across both signs") {
      zerocopy::PointCloud pc;
      REQUIRE(pc.create_v3f<>(8, {}, /*extent=*/32767));

      pc.push_value_v3f(-2.0f, 0.0f, 0.0f);
      pc.push_value_v3f(-1.0f, 0.0f, 0.0f);
      pc.push_value_v3f(0.0f, 0.0f, 0.0f);
      pc.push_value_v3f(1.0f, 0.0f, 0.0f);
      pc.push_value_v3f(2.0f, 0.0f, 0.0f);

      CHECK_EQ(pc.size(), 5u);

      CHECK(pc.downsample(1));

      CHECK_EQ(static_cast<int>(pc.get_downsample()), 1);
      CHECK_EQ(pc.size(), 5u);

      float x = 0;
      float y = 0;
      float z = 0;

      CHECK(pc.get_value_v3f(x, y, z, 0));
      CHECK_EQ(x, doctest::Approx(-2.0f));

      CHECK(pc.get_value_v3f(x, y, z, 1));
      CHECK_EQ(x, doctest::Approx(-1.0f));

      CHECK(pc.get_value_v3f(x, y, z, 2));
      CHECK_EQ(x, doctest::Approx(0.0f));

      CHECK(pc.get_value_v3f(x, y, z, 3));
      CHECK_EQ(x, doctest::Approx(1.0f));

      CHECK(pc.get_value_v3f(x, y, z, 4));
      CHECK_EQ(x, doctest::Approx(2.0f));
    }
  }

  TEST_CASE("downsample is a level-0 no-op and is rejected on an uncompressed cloud") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(4, {}));

    CHECK(pc.downsample(0));
    CHECK_EQ(static_cast<int>(pc.get_downsample()), 0);

    CHECK_FALSE(pc.downsample(100));
    CHECK_EQ(static_cast<int>(pc.get_downsample()), 0);
  }

  TEST_CASE("non-template create initializes downsample marker to zero") {
    zerocopy::PointCloud ref;
    ref.create<float, float, float>(4, {"x", "y", "z"});
    uint64_t size_num = ref.get_protocol_size_num();
    uint64_t type_num = ref.get_protocol_type_num();

    zerocopy::PointCloud quantised;
    CHECK(quantised.create(4, size_num, type_num, "x,y,z", /*extent=*/10));
    CHECK_EQ(static_cast<int>(quantised.get_downsample()), 0);

    zerocopy::PointCloud plain;
    CHECK(plain.create(4, size_num, type_num, "x,y,z", /*extent=*/0));
    CHECK_EQ(static_cast<int>(plain.get_downsample()), 0);
  }

  TEST_CASE("deserialize rejects a wire payload whose major version differs") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<>(4, {}));
    src.push_value_v3f(1.0f, 2.0f, 3.0f);

    Bytes wire;
    REQUIRE((src >> wire));
    REQUIRE(zerocopy::PointCloud::check_valid(wire));

    uint32_t foreign_version = (zerocopy::version_major(zerocopy::kWireVersion) + 1U) * 1000000U;
    std::memcpy(wire.data() + sizeof(uint32_t), &foreign_version, sizeof(foreign_version));

    CHECK_FALSE(zerocopy::PointCloud::check_valid(wire));

    zerocopy::PointCloud dst;
    CHECK_FALSE((dst << wire));
  }

  TEST_CASE("deserialize normalizes a stray downsample on a non-quantised cloud") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<>(4, {}));  // extent == 0, not quantised
    src.push_value_v3f(1.0f, 2.0f, 3.0f);

    Bytes wire;
    REQUIRE((src >> wire));

    // Forge a non-zero downsample_ byte directly in the wire snapshot.
    // Envelope = magic_begin(4) + version(4) + PointCloud struct; downsample_ sits at
    // struct offset 244, so its wire offset is 8 + 244 = 252.
    static constexpr size_t kDownsampleWireOffset = sizeof(uint32_t) + sizeof(uint32_t) + 244u;
    wire.data()[kDownsampleWireOffset] = 0x7Fu;

    zerocopy::PointCloud rx;
    REQUIRE((rx << wire));

    CHECK_EQ(static_cast<int>(rx.get_extent()), 0);
    CHECK_EQ(static_cast<int>(rx.get_downsample()), 0);
  }

  TEST_CASE("schema names filling the full 152-byte buffer are read without overrun") {
    zerocopy::PointCloud ref;
    ref.create<float, float, float, float>(4, {"a", "b", "c", "d"});
    uint64_t size_num = ref.get_protocol_size_num();
    uint64_t type_num = ref.get_protocol_type_num();

    std::string names = "x,y,z,";
    names += std::string(152 - names.size(), 'a');  // exactly 152 bytes, leaving no room for a NUL
    REQUIRE_EQ(names.size(), 152u);

    zerocopy::PointCloud pc;
    REQUIRE(pc.create(4, size_num, type_num, names));

    CHECK_EQ(pc.get_protocol_name_str().size(), 152u);
    CHECK_EQ(pc.get_protocol_name_str(), names);

    zerocopy::PointCloud::KeyList key_list;
    auto key_map = pc.get_key_map(&key_list);
    CHECK_EQ(key_list.size(), 4u);
    CHECK_EQ(key_map.size(), 4u);
  }

  TEST_CASE("downsample LUT path (size >= 65536) deduplicates by voxel") {
    SUBCASE("level 255 -> v_q 128: q=0 and q=128 fall into separate voxels") {
      zerocopy::PointCloud pc;
      REQUIRE(pc.create_v3f<>(65536, {}, /*extent=*/32767));

      for (int i = 0; i < 32768; ++i) {
        pc.push_value_v3f(0.0f, 0.0f, 0.0f);
      }

      for (int i = 0; i < 32768; ++i) {
        pc.push_value_v3f(128.0f, 0.0f, 0.0f);
      }

      CHECK_EQ(pc.size(), 65536u);
      CHECK(pc.downsample(255));
      CHECK_EQ(pc.size(), 2u);

      float x = 0;
      float y = 0;
      float z = 0;

      CHECK(pc.get_value_v3f(x, y, z, 0));
      CHECK_EQ(x, doctest::Approx(0.0f));

      CHECK(pc.get_value_v3f(x, y, z, 1));
      CHECK_EQ(x, doctest::Approx(128.0f));
    }

    SUBCASE("level 1 -> v_q 1: adjacent negative steps q=-2 and q=-1 stay distinct") {
      zerocopy::PointCloud pc;
      REQUIRE(pc.create_v3f<>(65536, {}, /*extent=*/32767));

      for (int i = 0; i < 32768; ++i) {
        pc.push_value_v3f(-2.0f, 0.0f, 0.0f);
      }

      for (int i = 0; i < 32768; ++i) {
        pc.push_value_v3f(-1.0f, 0.0f, 0.0f);
      }

      CHECK_EQ(pc.size(), 65536u);
      CHECK(pc.downsample(1));
      CHECK_EQ(pc.size(), 2u);

      float x = 0;
      float y = 0;
      float z = 0;

      CHECK(pc.get_value_v3f(x, y, z, 0));
      CHECK_EQ(x, doctest::Approx(-2.0f));

      CHECK(pc.get_value_v3f(x, y, z, 1));
      CHECK_EQ(x, doctest::Approx(-1.0f));
    }
  }

  TEST_CASE("generic push_value and get_value round-trip a mixed-type schema") {
    zerocopy::PointCloud pc;
    REQUIRE((pc.create<int32_t, uint8_t, float, double, int16_t>(4, {"a", "b", "c", "d", "e"})));

    CHECK_EQ(pc.pack_size(), 19u);
    CHECK(pc.push_value(static_cast<int32_t>(-7), static_cast<uint8_t>(200), 1.5f, 2.5, static_cast<int16_t>(-3)));
    CHECK_EQ(pc.size(), 1u);

    auto key_map = pc.get_key_map();
    CHECK_EQ(pc.get_value<int32_t>(0u, key_map, "a"), -7);
    CHECK_EQ(pc.get_value<uint8_t>(0u, key_map, "b"), 200);
    CHECK_EQ(pc.get_value<float>(0u, key_map, "c"), doctest::Approx(1.5f));
    CHECK_EQ(pc.get_value<double>(0u, key_map, "d"), doctest::Approx(2.5));
    CHECK_EQ(pc.get_value<int16_t>(0u, key_map, "e"), -3);
  }

  TEST_CASE("generic set_value overwrites a point in a mixed-type schema") {
    zerocopy::PointCloud pc;
    REQUIRE((pc.create<int32_t, float, uint8_t>(4, {"a", "b", "c"})));

    CHECK(pc.push_value(static_cast<int32_t>(1), 2.0f, static_cast<uint8_t>(3)));
    CHECK(pc.push_value(static_cast<int32_t>(3), 4.0f, static_cast<uint8_t>(5)));
    CHECK(pc.resize(2));

    CHECK(pc.set_value(0u, static_cast<int32_t>(42), 9.0f, static_cast<uint8_t>(7)));

    auto key_map = pc.get_key_map();
    CHECK_EQ(pc.get_value<int32_t>(0u, key_map, "a"), 42);
    CHECK_EQ(pc.get_value<float>(0u, key_map, "b"), doctest::Approx(9.0f));
    CHECK_EQ(pc.get_value<uint8_t>(0u, key_map, "c"), 7);
  }

  TEST_CASE("get_value_for_double_float reads every field type via offset and key_map") {
    zerocopy::PointCloud pc;
    REQUIRE((pc.create<int32_t, uint8_t, float, double, int16_t>(2, {"a", "b", "c", "d", "e"})));
    REQUIRE(pc.push_value(static_cast<int32_t>(-7), static_cast<uint8_t>(200), 1.5f, 2.5, static_cast<int16_t>(-3)));

    auto key_map = pc.get_key_map();

    CHECK_EQ(pc.get_value_for_double_float(0u, key_map["a"], zerocopy::PointCloud::kInt32Type), doctest::Approx(-7.0));
    CHECK_EQ(pc.get_value_for_double_float(0u, key_map["b"], zerocopy::PointCloud::kUint8Type), doctest::Approx(200.0));
    CHECK_EQ(pc.get_value_for_double_float(0u, key_map["c"], zerocopy::PointCloud::kFloatType), doctest::Approx(1.5));
    CHECK_EQ(pc.get_value_for_double_float(0u, key_map["d"], zerocopy::PointCloud::kDoubleType), doctest::Approx(2.5));
    CHECK_EQ(pc.get_value_for_double_float(0u, key_map["e"], zerocopy::PointCloud::kInt16Type), doctest::Approx(-3.0));

    CHECK_EQ(pc.get_value_for_double_float(0u, key_map, "c", zerocopy::PointCloud::kFloatType), doctest::Approx(1.5));
    CHECK_EQ(pc.get_value_for_double_float(0u, 0u, zerocopy::PointCloud::kUnknownType), doctest::Approx(0.0));
  }

  TEST_CASE("get_value_for_print formats every field type and is empty for unknown") {
    zerocopy::PointCloud pc;
    REQUIRE((pc.create<int32_t, uint8_t, float, double, int16_t>(2, {"a", "b", "c", "d", "e"})));
    REQUIRE(pc.push_value(static_cast<int32_t>(-7), static_cast<uint8_t>(200), 1.5f, 2.5, static_cast<int16_t>(-3)));

    auto key_map = pc.get_key_map();

    CHECK_FALSE(pc.get_value_for_print(0u, key_map["a"], zerocopy::PointCloud::kInt32Type).empty());
    CHECK_FALSE(pc.get_value_for_print(0u, key_map["b"], zerocopy::PointCloud::kUint8Type).empty());
    CHECK_FALSE(pc.get_value_for_print(0u, key_map["c"], zerocopy::PointCloud::kFloatType).empty());
    CHECK_FALSE(pc.get_value_for_print(0u, key_map["d"], zerocopy::PointCloud::kDoubleType).empty());
    CHECK_FALSE(pc.get_value_for_print(0u, key_map["e"], zerocopy::PointCloud::kInt16Type).empty());

    CHECK_FALSE(pc.get_value_for_print(0u, key_map, "c", zerocopy::PointCloud::kFloatType).empty());
    CHECK(pc.get_value_for_print(0u, 0u, zerocopy::PointCloud::kUnknownType).empty());
  }

  TEST_CASE("create rejects malformed protocol parameters") {
    zerocopy::PointCloud ref;
    REQUIRE((ref.create<float, float, float>(4, {"a", "b", "c"})));
    uint64_t size_num = ref.get_protocol_size_num();
    uint64_t type_num = ref.get_protocol_type_num();

    SUBCASE("zero size_num is rejected") {
      zerocopy::PointCloud pc;
      CHECK_FALSE(pc.create(4, 0u, type_num, "a,b,c"));
    }

    SUBCASE("name count not matching field count is rejected") {
      zerocopy::PointCloud pc;
      CHECK_FALSE(pc.create(4, size_num, type_num, "a,b"));
    }

    SUBCASE("non-quantisable xyz type with a non-zero extent is rejected") {
      zerocopy::PointCloud pc;
      CHECK_FALSE((pc.create<int32_t, int32_t, int32_t>(4, {"x", "y", "z"}, 100)));
    }

    SUBCASE("name buffer longer than the wire protocol capacity is rejected") {
      zerocopy::PointCloud pc;
      CHECK_FALSE(pc.create(4, size_num, type_num, std::string(153u, 'x')));
    }

    SUBCASE("seventeen field names are rejected") {
      uint64_t many_size_num = 0;
      uint64_t many_type_num = 0;
      std::string names;
      for (int i = 0; i < 17; ++i) {
        many_size_num = (many_size_num << 4u) | 1u;
        many_type_num = (many_type_num << 4u) | static_cast<uint64_t>(zerocopy::PointCloud::kUint8Type);
        if (!names.empty()) {
          names.push_back(',');
        }
        names += "f" + std::to_string(i);
      }

      zerocopy::PointCloud pc;
      CHECK_FALSE(pc.create(4, many_size_num, many_type_num, names));
    }

    SUBCASE("zero point capacity is accepted without allocating a data buffer") {
      zerocopy::PointCloud pc;
      REQUIRE(pc.create(0, size_num, type_num, "a,b,c", 0, true, true));
      CHECK_FALSE(pc.is_valid());
      CHECK_FALSE(pc.is_owner());
      CHECK_EQ(pc.get_reserved_size(), 0u);
      CHECK_EQ(pc.pack_size(), 12u);
      CHECK(pc.get_vertical());
      CHECK(pc.get_sort());

      Bytes wire;
      REQUIRE((pc >> wire));

      zerocopy::PointCloud dst;
      REQUIRE((dst << wire));
      CHECK_EQ(dst.size(), 0u);
      CHECK(dst.get_vertical());
      CHECK(dst.get_sort());
    }
  }

  TEST_CASE("reserved fields travel through serialization") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<float>(4, {"intensity"}, 10, true));
    pc.push_value_v3f(1.0f, 2.0f, 3.0f, 0.5f);

    pc.get_reserved() = 0x11223344u;
    pc.get_reserved2() = 0x55667788u;

    Bytes wire;
    CHECK((pc >> wire));

    zerocopy::PointCloud dst;
    CHECK((dst << wire));
    CHECK_EQ(dst.get_reserved(), 0x11223344u);
    CHECK_EQ(dst.get_reserved2(), 0x55667788u);

    wire[8u + 245u] = static_cast<uint8_t>(0x9A);
    CHECK((dst << wire));
    CHECK_FALSE(dst.get_sort());
  }

  TEST_CASE("legacy reserved byte one enables sorting for unquantised XYZ") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<>(2, {}, 0, true));
    REQUIRE(src.push_value_v3f(2.0f, 0.0f, 0.0f));
    REQUIRE(src.push_value_v3f(1.0f, 0.0f, 0.0f));

    Bytes wire;
    REQUIRE((src >> wire));
    wire[8u + 245u] = 1;

    zerocopy::PointCloud dst;
    REQUIRE((dst << wire));
    CHECK(dst.get_sort());
    CHECK_EQ(dst.get_value_v3f(0u).x, doctest::Approx(2.0f));

    Bytes sorted_wire;
    REQUIRE((dst >> sorted_wire));

    zerocopy::PointCloud sorted;
    REQUIRE((sorted << sorted_wire));
    CHECK_EQ(sorted.get_value_v3f(0u).x, doctest::Approx(1.0f));

    zerocopy::PointCloud unsupported;
    REQUIRE(unsupported.create<int32_t, int32_t, int32_t>(2, {"x", "y", "z"}, 0, true));
    REQUIRE(unsupported.push_value<int32_t, int32_t, int32_t>(2, 0, 0));
    REQUIRE(unsupported.push_value<int32_t, int32_t, int32_t>(1, 0, 0));

    Bytes unsupported_wire;
    REQUIRE((unsupported >> unsupported_wire));
    unsupported_wire[8u + 245u] = 1;

    zerocopy::PointCloud unsupported_dst;
    REQUIRE((unsupported_dst << unsupported_wire));
    CHECK_FALSE(unsupported_dst.get_sort());

    zerocopy::PointCloud mismatched;
    REQUIRE(mismatched.create(2, 0x111, 0xAAA, "x,y,z", 0, true, false));
    const std::array<uint8_t, 6> packed{};
    REQUIRE(mismatched.fill_packed_data(packed.data(), 2));

    Bytes mismatched_wire;
    REQUIRE((mismatched >> mismatched_wire));
    mismatched_wire[8u + 245u] = 1;

    zerocopy::PointCloud mismatched_dst;
    REQUIRE((mismatched_dst << mismatched_wire));
    CHECK_FALSE(mismatched_dst.get_sort());
  }

  TEST_CASE("get_key_map key list exposes type and size for each field") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<float>(4, {"intensity"}));

    zerocopy::PointCloud::KeyList key_list;
    auto key_map = pc.get_key_map(&key_list);
    auto direct_key_list = pc.get_key_list();

    CHECK_FALSE(key_map.empty());
    REQUIRE_EQ(key_list.size(), 4u);
    REQUIRE_EQ(direct_key_list.size(), key_list.size());
    CHECK_EQ(key_list[0].name, "x");
    CHECK_EQ(key_list[0].type, zerocopy::PointCloud::kFloatType);
    CHECK_EQ(key_list[0].size, 4u);
    CHECK_EQ(direct_key_list[0].name, key_list[0].name);
    CHECK_EQ(direct_key_list[0].type, key_list[0].type);
    CHECK_EQ(direct_key_list[0].size, key_list[0].size);
  }

  TEST_CASE("kZerocopyTypes marker is true") { CHECK(zerocopy::PointCloud::kZerocopyTypes); }

  TEST_CASE("set_value_v3d via Vector3d overload and get_value_v3d return overload") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3d<>(4, {}));

    pc.push_value_v3d(1.1, 2.2, 3.3);
    pc.push_value_v3d(4.4, 5.5, 6.6);
    CHECK(pc.resize(2));

    zerocopy::PointCloud::Vector3d nv(7.7, 8.8, 9.9);
    CHECK(pc.set_value_v3d(0u, nv));

    zerocopy::PointCloud::Vector3d got = pc.get_value_v3d(0u);
    CHECK_EQ(got.x, doctest::Approx(7.7));
    CHECK_EQ(got.y, doctest::Approx(8.8));
    CHECK_EQ(got.z, doctest::Approx(9.9));
  }

  TEST_CASE("deep_copy reuses an owned buffer of matching capacity in place") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<float>(4, {"i"}, 0, true, true));
    src.push_value_v3f(1.0f, 2.0f, 3.0f, 0.5f);
    src.push_value_v3f(4.0f, 5.0f, 6.0f, 0.6f);

    zerocopy::PointCloud dst;
    REQUIRE(dst.create_v3f<float>(2, {"i"}));
    dst.push_value_v3f(0.0f, 0.0f, 0.0f, 0.0f);
    dst.push_value_v3f(0.0f, 0.0f, 0.0f, 0.0f);

    const uint8_t* before = dst.get_internal_data();
    CHECK(dst.deep_copy(src));

    CHECK_EQ(dst.get_internal_data(), before);
    CHECK_EQ(dst.size(), 2u);
    CHECK(dst.get_sort());

    zerocopy::PointCloud::Vector3f v = dst.get_value_v3f(0u);
    CHECK_EQ(v.x, doctest::Approx(1.0f));
    CHECK_EQ(v.z, doctest::Approx(3.0f));
  }

  TEST_CASE("get_value_v3f out of range on a compressed cloud returns false") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<float>(4, {"i"}, 100));
    pc.push_value_v3f(1.0f, 2.0f, 3.0f, 0.5f);

    float x = 99.0f;
    float y = 99.0f;
    float z = 99.0f;
    CHECK_FALSE(pc.get_value_v3f(x, y, z, 5u));
  }

  TEST_CASE("downsample on an empty quantised cloud succeeds without changing size") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(4, {}, 100));

    CHECK(pc.downsample(3));
    CHECK_EQ(pc.size(), 0u);
  }

  TEST_CASE("downsample is rejected on a borrowed deserialized cloud") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<>(4, {}, 100));
    src.push_value_v3f(1.0f, 2.0f, 3.0f);

    Bytes wire;
    CHECK((src >> wire));

    zerocopy::PointCloud dst;
    CHECK((dst << wire));
    CHECK_FALSE(dst.is_owner());
    CHECK_FALSE(dst.downsample(2));
  }

  TEST_CASE("operator>> reuses an already correctly-sized buffer") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<float>(4, {"i"}));
    pc.push_value_v3f(1.0f, 2.0f, 3.0f, 0.5f);

    Bytes wire;
    CHECK((pc >> wire));
    size_t first_size = wire.size();

    CHECK((pc >> wire));
    CHECK_EQ(wire.size(), first_size);

    zerocopy::PointCloud dst;
    CHECK((dst << wire));
    CHECK_EQ(dst.size(), 1u);
  }

  TEST_CASE("operator>> resizes a stale output buffer before writing") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(4, {}));
    REQUIRE(pc.push_value_v3f(1.0f, 2.0f, 3.0f));

    Bytes wire = Bytes::create(1u);
    REQUIRE((pc >> wire));
    CHECK_EQ(wire.size(), pc.get_serialized_size());

    zerocopy::PointCloud dst;
    CHECK((dst << wire));
    CHECK_EQ(dst.size(), 1u);
  }

  TEST_CASE("operator<< rejects empty, truncated and over-long wire buffers") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<float>(4, {"i"}));
    pc.push_value_v3f(1.0f, 2.0f, 3.0f, 0.5f);

    Bytes wire;
    CHECK((pc >> wire));

    SUBCASE("empty buffer") {
      zerocopy::PointCloud dst;
      Bytes empty;
      CHECK_FALSE((dst << empty));
      CHECK_FALSE(dst.is_valid());
    }

    SUBCASE("truncated buffer") {
      zerocopy::PointCloud dst;
      Bytes truncated = Bytes::create(wire.size() - 1u);
      std::memcpy(truncated.data(), wire.data(), wire.size() - 1u);
      CHECK_FALSE((dst << truncated));
      CHECK_FALSE(dst.is_valid());
    }

    SUBCASE("over-long buffer") {
      zerocopy::PointCloud dst;
      Bytes extended = Bytes::create(wire.size() + 4u);
      std::memcpy(extended.data(), wire.data(), wire.size());
      CHECK_FALSE((dst << extended));
      CHECK_FALSE(dst.is_valid());
    }
  }

  TEST_CASE("operator<< rejects corrupted magic without clearing a still-owned receiver") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<>(4, {}));
    REQUIRE(src.push_value_v3f(1.0f, 2.0f, 3.0f));

    Bytes wire;
    REQUIRE((src >> wire));

    SUBCASE("begin magic") {
      wire.data()[0] ^= 0xFFu;

      zerocopy::PointCloud dst;
      REQUIRE(dst.create_v3f<>(2, {}));
      CHECK_FALSE((dst << wire));
      CHECK(dst.is_owner());
    }

    SUBCASE("end magic") {
      wire.data()[wire.size() - 1u] ^= 0xFFu;

      zerocopy::PointCloud dst;
      REQUIRE(dst.create_v3f<>(2, {}));
      CHECK_FALSE((dst << wire));
      CHECK(dst.is_owner());
    }
  }

  TEST_CASE("operator<< rejects structurally inconsistent but magic-valid wire buffers") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<float>(4, {"i"}));
    REQUIRE(src.push_value_v3f(1.0f, 2.0f, 3.0f, 0.5f));

    Bytes wire;
    REQUIRE((src >> wire));
    REQUIRE(zerocopy::PointCloud::check_valid(wire));

    SUBCASE("invalid vertical marker clears a pre-owned receiver") {
      Bytes mutated = wire;
      static constexpr size_t kVerticalWireOffset = sizeof(uint32_t) + sizeof(uint32_t) + 246u;
      mutated.data()[kVerticalWireOffset] = 2u;

      zerocopy::PointCloud dst;
      REQUIRE(dst.create_v3f<>(2, {}));
      CHECK_FALSE((dst << mutated));
      CHECK_FALSE(dst.is_valid());
      CHECK_FALSE(dst.is_owner());
    }

    SUBCASE("pack size not matching protocol field sizes is rejected") {
      Bytes mutated = wire;
      static constexpr size_t kPackSizeWireOffset = sizeof(uint32_t) + sizeof(uint32_t) + 240u;
      uint16_t invalid_pack_size = 13u;
      std::memcpy(mutated.data() + kPackSizeWireOffset, &invalid_pack_size, sizeof(invalid_pack_size));

      zerocopy::PointCloud dst;
      CHECK_FALSE((dst << mutated));
      CHECK_FALSE(dst.is_valid());
    }

    SUBCASE("serialized size mismatch after metadata copy is rejected") {
      Bytes mutated = wire;
      static constexpr size_t kSizeWireOffset = sizeof(uint32_t) + sizeof(uint32_t) + 224u;
      size_t invalid_size = 2u;
      std::memcpy(mutated.data() + kSizeWireOffset, &invalid_size, sizeof(invalid_size));

      zerocopy::PointCloud dst;
      CHECK_FALSE((dst << mutated));
      CHECK_FALSE(dst.is_valid());
    }
  }

  TEST_CASE("all scalar protocol types are printable and readable through switch helpers") {
    zerocopy::PointCloud pc;
    REQUIRE((pc.create<bool, int8_t, uint8_t, int16_t, uint16_t, int32_t, uint32_t, int64_t, uint64_t, float, double>(
        2, {"b", "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "f", "d"})));
    REQUIRE(pc.push_value(true, static_cast<int8_t>(-2), static_cast<uint8_t>(3), static_cast<int16_t>(-4),
                          static_cast<uint16_t>(5), static_cast<int32_t>(-6), static_cast<uint32_t>(7),
                          static_cast<int64_t>(-8), static_cast<uint64_t>(9), 1.5f, 2.5));

    auto key_map = pc.get_key_map();
    CHECK_EQ(pc.get_protocol_type_str(), "bool,int8,uint8,int16,uint16,int32,uint32,int64,uint64,float,double");

    CHECK_EQ(pc.get_value_for_double_float(0u, key_map["b"], zerocopy::PointCloud::kBoolType), doctest::Approx(1.0));
    CHECK_EQ(pc.get_value_for_double_float(0u, key_map["i8"], zerocopy::PointCloud::kInt8Type), doctest::Approx(-2.0));
    CHECK_EQ(pc.get_value_for_double_float(0u, key_map["u16"], zerocopy::PointCloud::kUint16Type),
             doctest::Approx(5.0));
    CHECK_EQ(pc.get_value_for_double_float(0u, key_map["u32"], zerocopy::PointCloud::kUint32Type),
             doctest::Approx(7.0));
    CHECK_EQ(pc.get_value_for_double_float(0u, key_map["i64"], zerocopy::PointCloud::kInt64Type),
             doctest::Approx(-8.0));
    CHECK_EQ(pc.get_value_for_double_float(0u, key_map["u64"], zerocopy::PointCloud::kUint64Type),
             doctest::Approx(9.0));
    CHECK_EQ(pc.get_value_for_double_float(0u, key_map, "b", zerocopy::PointCloud::kBoolType), doctest::Approx(1.0));
    CHECK_EQ(pc.get_value_for_double_float(0u, key_map, "i8", zerocopy::PointCloud::kInt8Type), doctest::Approx(-2.0));
    CHECK_EQ(pc.get_value_for_double_float(0u, key_map, "u16", zerocopy::PointCloud::kUint16Type),
             doctest::Approx(5.0));
    CHECK_EQ(pc.get_value_for_double_float(0u, key_map, "u32", zerocopy::PointCloud::kUint32Type),
             doctest::Approx(7.0));
    CHECK_EQ(pc.get_value_for_double_float(0u, key_map, "i64", zerocopy::PointCloud::kInt64Type),
             doctest::Approx(-8.0));
    CHECK_EQ(pc.get_value_for_double_float(0u, key_map, "u64", zerocopy::PointCloud::kUint64Type),
             doctest::Approx(9.0));

    CHECK_EQ(pc.get_value_for_print(0u, key_map["b"], zerocopy::PointCloud::kBoolType), "true");
    CHECK_FALSE(pc.get_value_for_print(0u, key_map["i8"], zerocopy::PointCloud::kInt8Type).empty());
    CHECK_FALSE(pc.get_value_for_print(0u, key_map["u16"], zerocopy::PointCloud::kUint16Type).empty());
    CHECK_FALSE(pc.get_value_for_print(0u, key_map["u32"], zerocopy::PointCloud::kUint32Type).empty());
    CHECK_FALSE(pc.get_value_for_print(0u, key_map["i64"], zerocopy::PointCloud::kInt64Type).empty());
    CHECK_FALSE(pc.get_value_for_print(0u, key_map["u64"], zerocopy::PointCloud::kUint64Type).empty());
    CHECK_EQ(pc.get_value_for_print(0u, key_map, "b", zerocopy::PointCloud::kBoolType), "true");
    CHECK_FALSE(pc.get_value_for_print(0u, key_map, "i8", zerocopy::PointCloud::kInt8Type).empty());
    CHECK_FALSE(pc.get_value_for_print(0u, key_map, "u16", zerocopy::PointCloud::kUint16Type).empty());
    CHECK_FALSE(pc.get_value_for_print(0u, key_map, "u32", zerocopy::PointCloud::kUint32Type).empty());
    CHECK_FALSE(pc.get_value_for_print(0u, key_map, "i64", zerocopy::PointCloud::kInt64Type).empty());
    CHECK_FALSE(pc.get_value_for_print(0u, key_map, "u64", zerocopy::PointCloud::kUint64Type).empty());
  }

  TEST_CASE("invalid point cloud state rejects mutating helpers deterministically") {
    zerocopy::PointCloud empty;
    CHECK_FALSE(empty.resize(1));
    CHECK_FALSE(empty.push_value(1.0f, 2.0f, 3.0f));

    std::vector<uint8_t> bytes(12u, 0u);
    CHECK_FALSE(empty.fill_packed_data(bytes.data(), 1u));

    float value = 99.0f;
    CHECK_FALSE(empty.get_value<float>(value, 0u, 0u));
    CHECK_EQ(value, doctest::Approx(0.0f));

    zerocopy::PointCloud pc;
    REQUIRE((pc.create<int32_t, float, uint8_t>(2, {"a", "b", "c"})));
    REQUIRE(pc.push_value(static_cast<int32_t>(1), 2.0f, static_cast<uint8_t>(3)));
    REQUIRE(pc.resize(1u));
    CHECK_FALSE(pc.set_value(5u, static_cast<int32_t>(1), 2.0f, static_cast<uint8_t>(3)));
    CHECK_FALSE(pc.set_value(0u, static_cast<int32_t>(1), 2.0f, 3.0f));
  }

  TEST_CASE("compressed v3d setter succeeds through the quantised path") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3d<>(2, {}, 100));
    REQUIRE(pc.push_value_v3d(1.0, 2.0, 3.0));
    REQUIRE(pc.resize(1u));

    CHECK(pc.set_value_v3d(0u, 4.0, 5.0, 6.0));

    double x = 0;
    double y = 0;
    double z = 0;
    CHECK(pc.get_value_v3d(x, y, z, 0u));
    CHECK_EQ(x, doctest::Approx(4.0).epsilon(0.001));
    CHECK_EQ(y, doctest::Approx(5.0).epsilon(0.001));
    CHECK_EQ(z, doctest::Approx(6.0).epsilon(0.001));
  }

  TEST_CASE("shallow_copy and create release an existing owned buffer before reuse") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<>(2, {}));
    REQUIRE(src.push_value_v3f(1.0f, 2.0f, 3.0f));

    zerocopy::PointCloud dst;
    REQUIRE(dst.create_v3f<float>(2, {"i"}));
    const uint8_t* old_ptr = dst.get_internal_data();
    REQUIRE(old_ptr != nullptr);

    CHECK(dst.shallow_copy(src));
    CHECK_FALSE(dst.is_owner());
    CHECK_EQ(dst.get_internal_data(), src.get_internal_data());

    REQUIRE(dst.create_v3d<>(1, {}));
    CHECK(dst.is_owner());
    CHECK_NE(dst.get_internal_data(), nullptr);
  }

  TEST_CASE("create can replace an existing owned buffer with a new schema") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<float>(2, {"i"}));
    REQUIRE(pc.push_value_v3f(1.0f, 2.0f, 3.0f, 0.5f));
    const uint8_t* old_ptr = pc.get_internal_data();
    REQUIRE(old_ptr != nullptr);

    REQUIRE(pc.create_v3d<>(1, {}));
    CHECK(pc.is_owner());
    CHECK_EQ(pc.pack_size(), 24u);
    CHECK_EQ(pc.size(), 0u);
    CHECK_NE(pc.get_internal_data(), nullptr);
  }

  TEST_CASE("v3 accessors reject out of range indexes for struct and compressed paths") {
    zerocopy::PointCloud f32;
    REQUIRE(f32.create_v3f<>(1, {}));
    REQUIRE(f32.push_value_v3f(1.0f, 2.0f, 3.0f));

    zerocopy::PointCloud::Vector3f vf;
    CHECK_FALSE(f32.get_value_v3f(vf, 2u));
    const size_t wrapping_f32_index = std::numeric_limits<size_t>::max() / 6u;
    CHECK_FALSE(f32.get_value_v3f(vf, wrapping_f32_index));

    float xf = 0;
    float yf = 0;
    float zf = 0;
    CHECK_FALSE(f32.get_value_v3f(xf, yf, zf, wrapping_f32_index));

    zerocopy::PointCloud f64;
    REQUIRE(f64.create_v3d<>(1, {}));
    REQUIRE(f64.push_value_v3d(1.0, 2.0, 3.0));

    double x = 0;
    double y = 0;
    double z = 0;
    CHECK_FALSE(f64.get_value_v3d(x, y, z, 2u));

    zerocopy::PointCloud::Vector3d vd;
    CHECK_FALSE(f64.get_value_v3d(vd, 2u));
    const size_t wrapping_f64_index = std::numeric_limits<size_t>::max();
    CHECK_FALSE(f64.get_value_v3d(x, y, z, wrapping_f64_index));
    CHECK_FALSE(f64.get_value_v3d(vd, wrapping_f64_index));

    zerocopy::PointCloud compressed;
    REQUIRE(compressed.create_v3d<>(1, {}, 100));
    REQUIRE(compressed.push_value_v3d(1.0, 2.0, 3.0));
    CHECK_FALSE(compressed.get_value_v3d(x, y, z, 2u));
    CHECK_FALSE(compressed.get_value_v3d(vd, 2u));
  }

  TEST_CASE("switch helpers cover remaining scalar cases and false boolean formatting") {
    zerocopy::PointCloud pc;
    REQUIRE((pc.create<bool, int8_t, uint8_t, int16_t, uint16_t, int32_t, uint32_t, int64_t, uint64_t, float, double>(
        2, {"b", "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "f", "d"})));
    REQUIRE(pc.push_value(false, static_cast<int8_t>(-2), static_cast<uint8_t>(3), static_cast<int16_t>(-4),
                          static_cast<uint16_t>(5), static_cast<int32_t>(-6), static_cast<uint32_t>(7),
                          static_cast<int64_t>(-8), static_cast<uint64_t>(9), 1.5f, 2.5));

    auto key_map = pc.get_key_map();

    CHECK_EQ(pc.get_value_for_double_float(0u, key_map["u8"], zerocopy::PointCloud::kUint8Type), doctest::Approx(3.0));
    CHECK_EQ(pc.get_value_for_double_float(0u, key_map["i16"], zerocopy::PointCloud::kInt16Type),
             doctest::Approx(-4.0));
    CHECK_EQ(pc.get_value_for_double_float(0u, key_map["i32"], zerocopy::PointCloud::kInt32Type),
             doctest::Approx(-6.0));
    CHECK_EQ(pc.get_value_for_double_float(0u, key_map["d"], zerocopy::PointCloud::kDoubleType), doctest::Approx(2.5));

    CHECK_EQ(pc.get_value_for_double_float(0u, key_map, "u8", zerocopy::PointCloud::kUint8Type), doctest::Approx(3.0));
    CHECK_EQ(pc.get_value_for_double_float(0u, key_map, "i16", zerocopy::PointCloud::kInt16Type),
             doctest::Approx(-4.0));
    CHECK_EQ(pc.get_value_for_double_float(0u, key_map, "i32", zerocopy::PointCloud::kInt32Type),
             doctest::Approx(-6.0));
    CHECK_EQ(pc.get_value_for_double_float(0u, key_map, "d", zerocopy::PointCloud::kDoubleType), doctest::Approx(2.5));
    CHECK_EQ(pc.get_value_for_double_float(0u, key_map, "d", zerocopy::PointCloud::kUnknownType), doctest::Approx(0.0));

    CHECK_EQ(pc.get_value_for_print(0u, key_map["b"], zerocopy::PointCloud::kBoolType), "false");
    CHECK_FALSE(pc.get_value_for_print(0u, key_map["u8"], zerocopy::PointCloud::kUint8Type).empty());
    CHECK_FALSE(pc.get_value_for_print(0u, key_map["i16"], zerocopy::PointCloud::kInt16Type).empty());
    CHECK_FALSE(pc.get_value_for_print(0u, key_map["i32"], zerocopy::PointCloud::kInt32Type).empty());
    CHECK_FALSE(pc.get_value_for_print(0u, key_map["d"], zerocopy::PointCloud::kDoubleType).empty());

    CHECK_EQ(pc.get_value_for_print(0u, key_map, "b", zerocopy::PointCloud::kBoolType), "false");
    CHECK_FALSE(pc.get_value_for_print(0u, key_map, "u8", zerocopy::PointCloud::kUint8Type).empty());
    CHECK_FALSE(pc.get_value_for_print(0u, key_map, "i16", zerocopy::PointCloud::kInt16Type).empty());
    CHECK_FALSE(pc.get_value_for_print(0u, key_map, "i32", zerocopy::PointCloud::kInt32Type).empty());
    CHECK_FALSE(pc.get_value_for_print(0u, key_map, "d", zerocopy::PointCloud::kDoubleType).empty());
    CHECK(pc.get_value_for_print(0u, key_map, "d", zerocopy::PointCloud::kUnknownType).empty());
  }

  TEST_CASE("operator<< rejects impossible serialized point counts before size matching") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<>(1, {}));
    REQUIRE(src.push_value_v3f(1.0f, 2.0f, 3.0f));

    Bytes wire;
    REQUIRE((src >> wire));

    static constexpr size_t kSizeWireOffset = sizeof(uint32_t) + sizeof(uint32_t) + 224u;
    size_t impossible_size = std::numeric_limits<size_t>::max();
    std::memcpy(wire.data() + kSizeWireOffset, &impossible_size, sizeof(impossible_size));

    zerocopy::PointCloud dst;
    CHECK_FALSE((dst << wire));
    CHECK_FALSE(dst.is_valid());
  }

  TEST_CASE("protocol helpers tolerate unknown type nibbles and truncated names") {
    zerocopy::PointCloud ref;
    REQUIRE((ref.create<float, float, float>(1, {"x", "y", "z"})));

    zerocopy::PointCloud unknown_type;
    REQUIRE(unknown_type.create(1, ref.get_protocol_size_num(), 0xFFFu, "x,y,z"));
    CHECK(unknown_type.get_protocol_type_str().empty());

    Bytes wire;
    REQUIRE((unknown_type >> wire));
    static constexpr size_t kNamesWireOffset = sizeof(uint32_t) + sizeof(uint32_t) + 56u;
    std::memset(wire.data() + kNamesWireOffset, 0, 152u);
    std::memcpy(wire.data() + kNamesWireOffset, "x", 1u);

    zerocopy::PointCloud dst;
    REQUIRE((dst << wire));
    zerocopy::PointCloud::KeyList key_list;
    auto key_map = dst.get_key_map(&key_list);
    CHECK_EQ(key_list.size(), 1u);
    CHECK_EQ(key_map.size(), 1u);
  }
}

// NOLINTEND
