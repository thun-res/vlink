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

#include <limits>
#include <string>
#include <vector>

#include "../common_test.h"

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
    REQUIRE(pc.create_v3f<>(10, {}));
    pc.push_value_v3f(1.0f, 2.0f, 3.0f);

    pc.clear(false);

    CHECK_EQ(pc.size(), 0u);
    CHECK_GT(pc.pack_size(), 0u);
    CHECK(pc.is_owner());
    CHECK_GT(pc.get_reserved_size(), 0u);
  }

  TEST_CASE("clear true fully resets to default state") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(10, {}));
    pc.push_value_v3f(1.0f, 2.0f, 3.0f);

    pc.clear(true);

    CHECK_FALSE(pc.is_valid());
    CHECK_FALSE(pc.is_owner());
    CHECK_EQ(pc.size(), 0u);
    CHECK_EQ(pc.pack_size(), 0u);
    CHECK_EQ(pc.get_reserved_size(), 0u);
  }

  TEST_CASE("shallow_copy aliases the buffer without ownership") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<>(5, {}));
    src.push_value_v3f(1.0f, 2.0f, 3.0f);

    zerocopy::PointCloud dst;
    CHECK(dst.shallow_copy(src));

    CHECK_FALSE(dst.is_owner());
    CHECK_EQ(dst.size(), 1u);
    CHECK_EQ(dst.get_internal_data(), src.get_internal_data());
  }

  TEST_CASE("shallow_copy self returns false") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(4, {}));

    CHECK_FALSE(pc.shallow_copy(pc));
  }

  TEST_CASE("deep_copy creates owned independent copy") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<>(5, {}));
    src.push_value_v3f(1.0f, 2.0f, 3.0f);
    src.push_value_v3f(4.0f, 5.0f, 6.0f);

    zerocopy::PointCloud dst;
    CHECK(dst.deep_copy(src));

    CHECK(dst.is_owner());
    CHECK_EQ(dst.size(), 2u);
    CHECK_EQ(dst.pack_size(), 12u);
    CHECK_NE(dst.get_internal_data(), src.get_internal_data());
  }

  TEST_CASE("deep_copy self returns false") {
    zerocopy::PointCloud pc;
    REQUIRE(pc.create_v3f<>(4, {}));

    CHECK_FALSE(pc.deep_copy(pc));
  }

  TEST_CASE("move_copy transfers ownership and invalidates source") {
    zerocopy::PointCloud src;
    REQUIRE(src.create_v3f<>(10, {}));
    src.push_value_v3f(0.0f, 0.0f, 0.0f);
    const uint8_t* ptr = src.get_internal_data();

    zerocopy::PointCloud dst;
    CHECK(dst.move_copy(src));

    CHECK(dst.is_owner());
    CHECK_EQ(dst.get_internal_data(), ptr);
    CHECK_EQ(dst.size(), 1u);
    CHECK_FALSE(src.is_valid());
    CHECK_FALSE(src.is_owner());
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
    CHECK_NE(type_str.find("int16"), std::string::npos);
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
}

// NOLINTEND
