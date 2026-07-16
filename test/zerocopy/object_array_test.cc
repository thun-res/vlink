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

#include "./zerocopy/object_array.h"

#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "../common_test.h"

TEST_SUITE("zerocopy-ObjectArray") {
  TEST_CASE("default construction yields invalid empty array") {
    zerocopy::ObjectArray arr;

    CHECK_FALSE(arr.is_valid());
    CHECK_EQ(arr.count(), 0u);
    CHECK_EQ(arr.pack_size(), 0u);
    CHECK_EQ(arr.capacity(), 0u);
    CHECK_FALSE(arr.is_owner());
    CHECK_EQ(arr.data(), nullptr);
    CHECK_EQ(arr.channel(), 0u);
    CHECK_EQ(arr.freq(), 0u);
    CHECK_EQ(arr.update_time_ns(), 0u);
    CHECK(arr.source_id().empty());
    CHECK_EQ(arr.objects(0u), nullptr);
  }

  TEST_CASE("sizeof container is exactly 112 bytes") { CHECK_EQ(sizeof(zerocopy::ObjectArray), 112u); }

  TEST_CASE("sizeof Object is exactly 144 bytes") { CHECK_EQ(sizeof(zerocopy::ObjectArray::Object), 144u); }

  TEST_CASE("alignof Object is exactly 4 bytes") {
    static_assert(alignof(zerocopy::ObjectArray::Object) == 4u,
                  "Object must be alignas(4) -- wire payload at offset 4+112=116 is only 4-byte-aligned.");

    CHECK_EQ(alignof(zerocopy::ObjectArray::Object), 4u);
  }

  TEST_CASE("Object default construction zeros all fields") {
    zerocopy::ObjectArray::Object obj;

    CHECK_EQ(std::string(obj.label), "");
    CHECK_EQ(obj.position[0], doctest::Approx(0.0f));
    CHECK_EQ(obj.position[1], doctest::Approx(0.0f));
    CHECK_EQ(obj.position[2], doctest::Approx(0.0f));
    CHECK_EQ(obj.yaw, doctest::Approx(0.0f));
    CHECK_EQ(obj.size[0], doctest::Approx(0.0f));
    CHECK_EQ(obj.size[1], doctest::Approx(0.0f));
    CHECK_EQ(obj.size[2], doctest::Approx(0.0f));
    CHECK_EQ(obj.yaw_rate, doctest::Approx(0.0f));
    CHECK_EQ(obj.velocity[0], doctest::Approx(0.0f));
    CHECK_EQ(obj.velocity[1], doctest::Approx(0.0f));
    CHECK_EQ(obj.velocity[2], doctest::Approx(0.0f));
    CHECK_EQ(obj.score, doctest::Approx(0.0f));
    CHECK_EQ(obj.acceleration[0], doctest::Approx(0.0f));
    CHECK_EQ(obj.existence_probability, doctest::Approx(0.0f));
    CHECK_EQ(obj.class_id, 0u);
    CHECK_EQ(obj.track_id, 0u);
    CHECK_EQ(obj.age, 0u);
    CHECK_EQ(obj.num_observations, 0u);
    CHECK_EQ(obj.motion_state, zerocopy::ObjectArray::kMotionUnknown);
    CHECK_EQ(obj.source_type, zerocopy::ObjectArray::kSourceUnknown);
    CHECK_EQ(obj.subtype_id, 0u);
    CHECK_EQ(obj.reserved_buf, 0u);
  }

  TEST_CASE("all metadata accessors round-trip") {
    zerocopy::ObjectArray arr;

    arr.set_channel(2u);
    CHECK_EQ(arr.channel(), 2u);

    arr.set_freq(20u);
    CHECK_EQ(arr.freq(), 20u);

    arr.set_update_time_ns(1234567890ull);
    CHECK_EQ(arr.update_time_ns(), 1234567890ull);

    arr.set_source_id("fusion_v2");
    CHECK_EQ(std::string(arr.source_id()), "fusion_v2");
  }

  TEST_CASE("set_source_id truncates oversize input") {
    zerocopy::ObjectArray arr;

    arr.set_source_id("this_source_id_is_longer_than_sixteen_bytes");
    CHECK_LE(arr.source_id().size(), 15u);
  }

  TEST_CASE("create succeeds for representative counts and sets ownership") {
    size_t count = 0;

    SUBCASE("single slot") { count = 1; }
    SUBCASE("small") { count = 8; }
    SUBCASE("typical") { count = 256; }

    zerocopy::ObjectArray arr;
    CHECK(arr.create(count));
    CHECK(arr.is_owner());
    CHECK_EQ(arr.pack_size(), sizeof(zerocopy::ObjectArray::Object));
    CHECK_EQ(arr.capacity(), count * sizeof(zerocopy::ObjectArray::Object));
    CHECK_EQ(arr.count(), 0u);
    CHECK_NE(arr.data(), nullptr);
  }

  TEST_CASE("create with zero count returns false") {
    zerocopy::ObjectArray arr;

    CHECK_FALSE(arr.create(0));
    CHECK_FALSE(arr.is_owner());
  }

  TEST_CASE("create replaces previous owned buffer") {
    zerocopy::ObjectArray arr;

    REQUIRE(arr.create(4));
    REQUIRE(arr.create(16));

    CHECK_EQ(arr.capacity(), 16u * sizeof(zerocopy::ObjectArray::Object));
    CHECK(arr.is_owner());
  }

  TEST_CASE("push_value appends Object records and updates count") {
    zerocopy::ObjectArray arr;
    REQUIRE(arr.create(4));

    zerocopy::ObjectArray::Object obj;
    obj.position[0] = 1.0f;
    obj.position[1] = 2.0f;
    obj.position[2] = 3.0f;
    obj.class_id = 1u;
    obj.track_id = 42u;

    CHECK(arr.push_value(obj));
    CHECK_EQ(arr.count(), 1u);

    obj.position[0] = 4.0f;
    obj.track_id = 43u;
    CHECK(arr.push_value(obj));
    CHECK_EQ(arr.count(), 2u);

    CHECK(arr.is_valid());
  }

  TEST_CASE("push_value overflow returns false when capacity is exhausted") {
    zerocopy::ObjectArray arr;
    REQUIRE(arr.create(2));

    zerocopy::ObjectArray::Object obj;
    CHECK(arr.push_value(obj));
    CHECK(arr.push_value(obj));
    CHECK_FALSE(arr.push_value(obj));
    CHECK_EQ(arr.count(), 2u);
  }

  TEST_CASE("push_value on non-owner returns false") {
    zerocopy::ObjectArray arr;

    zerocopy::ObjectArray::Object obj;
    CHECK_FALSE(arr.push_value(obj));
  }

  TEST_CASE("get_value reads back pushed Object") {
    zerocopy::ObjectArray arr;
    REQUIRE(arr.create(4));

    zerocopy::ObjectArray::Object src;
    std::strncpy(src.label, "car", sizeof(src.label) - 1);
    src.position[0] = 12.0f;
    src.position[1] = 0.3f;
    src.position[2] = 0.0f;
    src.size[0] = 4.5f;
    src.size[1] = 1.8f;
    src.size[2] = 1.6f;
    src.yaw = 0.1f;
    src.velocity[0] = 8.5f;
    src.score = 0.95f;
    src.class_id = 1u;
    src.track_id = 42u;
    src.motion_state = zerocopy::ObjectArray::kMotionMoving;
    src.source_type = zerocopy::ObjectArray::kSourceFusion;
    src.subtype_id = 7u;
    REQUIRE(arr.push_value(src));

    zerocopy::ObjectArray::Object out;
    CHECK(arr.get_value(0u, out));

    CHECK_EQ(std::string(out.label), "car");
    CHECK_EQ(out.position[0], doctest::Approx(12.0f));
    CHECK_EQ(out.position[1], doctest::Approx(0.3f));
    CHECK_EQ(out.size[0], doctest::Approx(4.5f));
    CHECK_EQ(out.yaw, doctest::Approx(0.1f));
    CHECK_EQ(out.velocity[0], doctest::Approx(8.5f));
    CHECK_EQ(out.score, doctest::Approx(0.95f));
    CHECK_EQ(out.class_id, 1u);
    CHECK_EQ(out.track_id, 42u);
    CHECK_EQ(out.motion_state, zerocopy::ObjectArray::kMotionMoving);
    CHECK_EQ(out.source_type, zerocopy::ObjectArray::kSourceFusion);
    CHECK_EQ(out.subtype_id, 7u);
  }

  TEST_CASE("get_value out of range returns false and zeros the Object") {
    zerocopy::ObjectArray arr;
    REQUIRE(arr.create(2));

    zerocopy::ObjectArray::Object obj;
    obj.class_id = 5u;
    arr.push_value(obj);

    zerocopy::ObjectArray::Object out;
    out.class_id = 99u;
    CHECK_FALSE(arr.get_value(99u, out));
    CHECK_EQ(out.class_id, 0u);
  }

  TEST_CASE("get_value return overload yields zero-initialised on out of range") {
    zerocopy::ObjectArray arr;
    REQUIRE(arr.create(2));

    zerocopy::ObjectArray::Object obj;
    obj.class_id = 5u;
    obj.track_id = 11u;
    arr.push_value(obj);

    zerocopy::ObjectArray::Object out = arr.get_value(0u);
    CHECK_EQ(out.class_id, 5u);
    CHECK_EQ(out.track_id, 11u);

    zerocopy::ObjectArray::Object out_bad = arr.get_value(99u);
    CHECK_EQ(out_bad.class_id, 0u);
  }

  TEST_CASE("set_value overwrites an existing record") {
    zerocopy::ObjectArray arr;
    REQUIRE(arr.create(4));

    zerocopy::ObjectArray::Object obj;
    obj.class_id = 1u;
    arr.push_value(obj);
    arr.push_value(obj);

    zerocopy::ObjectArray::Object updated;
    updated.class_id = 9u;
    updated.track_id = 77u;
    CHECK(arr.set_value(0u, updated));

    zerocopy::ObjectArray::Object out;
    CHECK(arr.get_value(0u, out));
    CHECK_EQ(out.class_id, 9u);
    CHECK_EQ(out.track_id, 77u);
  }

  TEST_CASE("set_value out of range returns false") {
    zerocopy::ObjectArray arr;
    REQUIRE(arr.create(4));

    zerocopy::ObjectArray::Object obj;
    arr.push_value(obj);

    zerocopy::ObjectArray::Object updated;
    CHECK_FALSE(arr.set_value(99u, updated));
  }

  TEST_CASE("resize adjusts logical count without reallocation") {
    zerocopy::ObjectArray arr;
    REQUIRE(arr.create(5));

    zerocopy::ObjectArray::Object obj;
    for (int i = 0; i < 5; ++i) {
      arr.push_value(obj);
    }
    CHECK_EQ(arr.count(), 5u);

    CHECK(arr.resize(3));
    CHECK_EQ(arr.count(), 3u);

    CHECK_FALSE(arr.resize(99));
    CHECK_EQ(arr.count(), 3u);
  }

  TEST_CASE("resize on non-owner returns false") {
    zerocopy::ObjectArray arr;

    CHECK_FALSE(arr.resize(1));
  }

  TEST_CASE("objects accessor returns aligned pointer to record by index") {
    zerocopy::ObjectArray arr;
    REQUIRE(arr.create(4));

    zerocopy::ObjectArray::Object obj;
    obj.class_id = 1u;
    obj.track_id = 10u;
    arr.push_value(obj);

    obj.class_id = 2u;
    obj.track_id = 20u;
    arr.push_value(obj);

    const zerocopy::ObjectArray::Object* obj0 = arr.objects(0u);
    const zerocopy::ObjectArray::Object* obj1 = arr.objects(1u);

    REQUIRE_NE(obj0, nullptr);
    REQUIRE_NE(obj1, nullptr);
    CHECK_EQ(obj0->class_id, 1u);
    CHECK_EQ(obj0->track_id, 10u);
    CHECK_EQ(obj1->class_id, 2u);
    CHECK_EQ(obj1->track_id, 20u);

    // Verify alignment for Object (alignas(4)) at every slot.
    CHECK_EQ(reinterpret_cast<uintptr_t>(obj0) % alignof(zerocopy::ObjectArray::Object), 0u);
    CHECK_EQ(reinterpret_cast<uintptr_t>(obj1) % alignof(zerocopy::ObjectArray::Object), 0u);
  }

  TEST_CASE("objects out of range returns nullptr") {
    zerocopy::ObjectArray arr;
    REQUIRE(arr.create(2));

    zerocopy::ObjectArray::Object obj;
    arr.push_value(obj);

    CHECK_EQ(arr.objects(99u), nullptr);
  }

  TEST_CASE("clear resets all fields") {
    zerocopy::ObjectArray arr;

    arr.set_source_id("fusion");
    arr.set_channel(3u);
    arr.set_freq(20u);
    arr.create(8);

    zerocopy::ObjectArray::Object obj;
    arr.push_value(obj);
    arr.header.seq = 11u;

    arr.clear();

    CHECK_FALSE(arr.is_valid());
    CHECK_FALSE(arr.is_owner());
    CHECK_EQ(arr.count(), 0u);
    CHECK_EQ(arr.pack_size(), 0u);
    CHECK_EQ(arr.capacity(), 0u);
    CHECK_EQ(arr.data(), nullptr);
    CHECK_EQ(arr.channel(), 0u);
    CHECK_EQ(arr.freq(), 0u);
    CHECK(arr.source_id().empty());
    CHECK_EQ(arr.header.seq, 0u);
  }

  TEST_CASE("clear then create again succeeds") {
    zerocopy::ObjectArray arr;

    arr.create(4);
    arr.clear();
    CHECK_FALSE(arr.is_valid());

    CHECK(arr.create(2));
    CHECK_EQ(arr.capacity(), 2u * sizeof(zerocopy::ObjectArray::Object));
  }

  TEST_CASE("get_reserved3 is writable and not reset by clear") {
    zerocopy::ObjectArray arr;

    arr.get_reserved3() = 0xDEADBEEFu;
    CHECK_EQ(arr.get_reserved3(), 0xDEADBEEFu);

    arr.create(4);
    arr.clear();

    CHECK_EQ(arr.get_reserved3(), 0xDEADBEEFu);
  }

  TEST_CASE("shallow_copy aliases buffer and zeroes capacity on borrowed instance") {
    zerocopy::ObjectArray src;
    REQUIRE(src.create(8));

    zerocopy::ObjectArray::Object obj;
    obj.class_id = 1u;
    src.push_value(obj);
    src.push_value(obj);
    src.set_source_id("fusion");

    zerocopy::ObjectArray dst;
    CHECK(dst.shallow_copy(src));

    CHECK(dst.is_valid());
    CHECK_FALSE(dst.is_owner());
    CHECK_EQ(dst.data(), src.data());
    CHECK_EQ(dst.count(), 2u);
    CHECK_EQ(dst.pack_size(), sizeof(zerocopy::ObjectArray::Object));
    // Borrowed instance does not own a buffer, so capacity must reset to zero.
    CHECK_EQ(dst.capacity(), 0u);
    CHECK_EQ(std::string(dst.source_id()), "fusion");
  }

  TEST_CASE("shallow_copy self returns false") {
    zerocopy::ObjectArray arr;
    REQUIRE(arr.create(4));

    CHECK_FALSE(arr.shallow_copy(arr));
  }

  TEST_CASE("deep_copy creates owned independent copy") {
    zerocopy::ObjectArray src;
    REQUIRE(src.create(4));

    zerocopy::ObjectArray::Object obj;
    obj.class_id = 1u;
    obj.track_id = 10u;
    src.push_value(obj);
    obj.track_id = 20u;
    src.push_value(obj);

    zerocopy::ObjectArray dst;
    CHECK(dst.deep_copy(src));

    CHECK(dst.is_valid());
    CHECK(dst.is_owner());
    CHECK_EQ(dst.count(), 2u);
    CHECK_EQ(dst.pack_size(), sizeof(zerocopy::ObjectArray::Object));
    CHECK_NE(dst.data(), src.data());

    zerocopy::ObjectArray::Object out;
    CHECK(dst.get_value(1u, out));
    CHECK_EQ(out.track_id, 20u);
  }

  TEST_CASE("deep_copy self returns false") {
    zerocopy::ObjectArray arr;
    REQUIRE(arr.create(4));

    CHECK_FALSE(arr.deep_copy(arr));
  }

  TEST_CASE("move_copy transfers ownership and invalidates source") {
    zerocopy::ObjectArray src;
    REQUIRE(src.create(4));

    zerocopy::ObjectArray::Object obj;
    src.push_value(obj);
    const uint8_t* ptr = src.data();

    zerocopy::ObjectArray dst;
    CHECK(dst.move_copy(src));

    CHECK(dst.is_owner());
    CHECK_EQ(dst.data(), ptr);
    CHECK_EQ(dst.count(), 1u);
    CHECK_EQ(dst.capacity(), 4u * sizeof(zerocopy::ObjectArray::Object));

    CHECK_FALSE(src.is_valid());
    CHECK_FALSE(src.is_owner());
    CHECK_EQ(src.data(), nullptr);
    CHECK_EQ(src.count(), 0u);
    CHECK_EQ(src.pack_size(), 0u);
    CHECK_EQ(src.capacity(), 0u);
  }

  TEST_CASE("move_copy self returns false") {
    zerocopy::ObjectArray arr;
    REQUIRE(arr.create(4));

    CHECK_FALSE(arr.move_copy(arr));
  }

  TEST_CASE("copy constructor performs deep copy") {
    zerocopy::ObjectArray src;
    REQUIRE(src.create(4));

    zerocopy::ObjectArray::Object obj;
    obj.class_id = 7u;
    src.push_value(obj);
    src.set_source_id("fusion");

    zerocopy::ObjectArray copy(src);

    CHECK(copy.is_owner());
    CHECK_EQ(copy.count(), 1u);
    CHECK_EQ(copy.pack_size(), sizeof(zerocopy::ObjectArray::Object));
    CHECK_NE(copy.data(), src.data());
    CHECK_EQ(std::string(copy.source_id()), "fusion");

    zerocopy::ObjectArray::Object out;
    CHECK(copy.get_value(0u, out));
    CHECK_EQ(out.class_id, 7u);
  }

  TEST_CASE("move constructor transfers ownership") {
    zerocopy::ObjectArray src;
    REQUIRE(src.create(4));

    zerocopy::ObjectArray::Object obj;
    src.push_value(obj);
    const uint8_t* ptr = src.data();

    zerocopy::ObjectArray moved(std::move(src));

    CHECK(moved.is_owner());
    CHECK_EQ(moved.data(), ptr);
    CHECK_EQ(moved.count(), 1u);
    CHECK_EQ(moved.capacity(), 4u * sizeof(zerocopy::ObjectArray::Object));
    CHECK_FALSE(src.is_valid());
  }

  TEST_CASE("copy assignment performs deep copy") {
    zerocopy::ObjectArray src;
    REQUIRE(src.create(4));

    zerocopy::ObjectArray::Object obj;
    obj.class_id = 3u;
    src.push_value(obj);

    zerocopy::ObjectArray dst;
    dst = src;

    CHECK(dst.is_owner());
    CHECK_EQ(dst.count(), 1u);
    CHECK_NE(dst.data(), src.data());

    zerocopy::ObjectArray::Object out;
    CHECK(dst.get_value(0u, out));
    CHECK_EQ(out.class_id, 3u);
  }

  TEST_CASE("move assignment transfers ownership") {
    zerocopy::ObjectArray src;
    REQUIRE(src.create(4));

    zerocopy::ObjectArray::Object obj;
    src.push_value(obj);
    const uint8_t* ptr = src.data();

    zerocopy::ObjectArray dst;
    dst = std::move(src);

    CHECK(dst.is_owner());
    CHECK_EQ(dst.data(), ptr);
    CHECK_EQ(dst.capacity(), 4u * sizeof(zerocopy::ObjectArray::Object));
    CHECK_FALSE(src.is_valid());
  }

  TEST_CASE("serialize and deserialize round-trip preserves all records") {
    zerocopy::ObjectArray src;
    REQUIRE(src.create(8));

    src.set_source_id("fusion");
    src.set_channel(2u);
    src.set_freq(20u);
    src.set_update_time_ns(999u);

    for (uint32_t i = 0; i < 5u; ++i) {
      zerocopy::ObjectArray::Object obj;
      std::strncpy(obj.label, "car", sizeof(obj.label) - 1);
      obj.position[0] = static_cast<float>(i);
      obj.position[1] = static_cast<float>(i) * 0.5f;
      obj.position[2] = 0.0f;
      obj.size[0] = 4.5f;
      obj.size[1] = 1.8f;
      obj.size[2] = 1.6f;
      obj.yaw = 0.01f * static_cast<float>(i);
      obj.velocity[0] = static_cast<float>(i) * 1.5f;
      obj.score = 0.9f;
      obj.class_id = 1u;
      obj.track_id = 100u + i;
      obj.age = i;
      obj.motion_state = zerocopy::ObjectArray::kMotionMoving;
      obj.source_type = zerocopy::ObjectArray::kSourceFusion;
      obj.subtype_id = static_cast<uint16_t>(i);
      REQUIRE(src.push_value(obj));
    }

    src.header.seq = 7u;
    std::strncpy(src.header.frame_id, "world", sizeof(src.header.frame_id) - 1);
    src.header.frame_id[sizeof(src.header.frame_id) - 1] = '\0';
    src.header.time_meas = 111111u;
    src.header.time_pub = 222222u;

    Bytes wire;
    CHECK((src >> wire));
    CHECK(zerocopy::ObjectArray::check_valid(wire));
    CHECK_EQ(wire.size(), src.get_serialized_size());

    zerocopy::ObjectArray dst;
    CHECK((dst << wire));

    CHECK(dst.is_valid());
    CHECK_FALSE(dst.is_owner());
    CHECK_EQ(dst.capacity(), 0u);
    CHECK_EQ(dst.count(), 5u);
    CHECK_EQ(dst.pack_size(), sizeof(zerocopy::ObjectArray::Object));
    CHECK_EQ(std::string(dst.source_id()), "fusion");
    CHECK_EQ(dst.channel(), 2u);
    CHECK_EQ(dst.freq(), 20u);
    CHECK_EQ(dst.update_time_ns(), 999u);
    CHECK_EQ(dst.header.seq, 7u);
    CHECK_EQ(std::string(dst.header.frame_id), "world");
    CHECK_EQ(dst.header.time_meas, 111111u);
    CHECK_EQ(dst.header.time_pub, 222222u);

    for (uint32_t i = 0; i < 5u; ++i) {
      const zerocopy::ObjectArray::Object* obj = dst.objects(i);
      REQUIRE_NE(obj, nullptr);
      CHECK_EQ(std::string(obj->label), "car");
      CHECK_EQ(obj->position[0], doctest::Approx(static_cast<float>(i)));
      CHECK_EQ(obj->track_id, 100u + i);
      CHECK_EQ(obj->age, i);
      CHECK_EQ(obj->motion_state, zerocopy::ObjectArray::kMotionMoving);
      CHECK_EQ(obj->source_type, zerocopy::ObjectArray::kSourceFusion);
      CHECK_EQ(obj->subtype_id, static_cast<uint16_t>(i));

      // Verify alignment of every record pointer.
      CHECK_EQ(reinterpret_cast<uintptr_t>(obj) % alignof(zerocopy::ObjectArray::Object), 0u);
    }
  }

  TEST_CASE("serialize empty array produces valid wire buffer") {
    zerocopy::ObjectArray arr;

    Bytes wire;
    CHECK((arr >> wire));
    CHECK(zerocopy::ObjectArray::check_valid(wire));

    zerocopy::ObjectArray arr2;
    CHECK((arr2 << wire));
    CHECK_EQ(arr2.count(), 0u);
    CHECK_EQ(arr2.pack_size(), 0u);
  }

  TEST_CASE("check_valid rejects empty, corrupted begin magic, and corrupted end magic") {
    zerocopy::ObjectArray arr;
    REQUIRE(arr.create(4));

    zerocopy::ObjectArray::Object obj;
    arr.push_value(obj);

    Bytes wire;
    arr >> wire;

    SUBCASE("empty bytes") {
      Bytes empty;
      CHECK_FALSE(zerocopy::ObjectArray::check_valid(empty));
    }

    SUBCASE("corrupted begin magic") {
      wire[0] ^= 0xFFu;
      CHECK_FALSE(zerocopy::ObjectArray::check_valid(wire));
    }

    SUBCASE("corrupted end magic") {
      wire[wire.size() - 1] ^= 0xFFu;
      CHECK_FALSE(zerocopy::ObjectArray::check_valid(wire));
    }
  }

  TEST_CASE("deserialize from too-small buffer returns false") {
    std::vector<uint8_t> raw(4, 0x00u);
    Bytes too_small(raw);

    zerocopy::ObjectArray arr;
    CHECK_FALSE((arr << too_small));
  }

  TEST_CASE("get_serialized_size equals magic + version + struct + records + magic") {
    zerocopy::ObjectArray arr;

    SUBCASE("empty") {
      size_t expected = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(zerocopy::ObjectArray) + 0u + sizeof(uint32_t);
      CHECK_EQ(arr.get_serialized_size(), expected);
    }

    SUBCASE("with three records") {
      REQUIRE(arr.create(8));
      zerocopy::ObjectArray::Object obj;
      arr.push_value(obj);
      arr.push_value(obj);
      arr.push_value(obj);

      size_t expected = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(zerocopy::ObjectArray) +
                        3u * sizeof(zerocopy::ObjectArray::Object) + sizeof(uint32_t);
      CHECK_EQ(arr.get_serialized_size(), expected);
    }
  }

  TEST_CASE("is_valid returns false when data is null or count or pack_size is zero") {
    zerocopy::ObjectArray arr;
    CHECK_FALSE(arr.is_valid());

    REQUIRE(arr.create(2));
    // Allocated but empty: count_ == 0, so is_valid is false.
    CHECK_FALSE(arr.is_valid());

    zerocopy::ObjectArray::Object obj;
    arr.push_value(obj);
    CHECK(arr.is_valid());

    arr.clear();
    CHECK_FALSE(arr.is_valid());
  }

  TEST_CASE("deserialize rejects a wire payload whose major version differs") {
    zerocopy::ObjectArray src;
    REQUIRE(src.create(4));

    Bytes wire;
    REQUIRE((src >> wire));
    REQUIRE(zerocopy::ObjectArray::check_valid(wire));

    uint32_t foreign_version = (zerocopy::version_major(zerocopy::kWireVersion) + 1U) * 1000000U;
    std::memcpy(wire.data() + sizeof(uint32_t), &foreign_version, sizeof(foreign_version));

    CHECK_FALSE(zerocopy::ObjectArray::check_valid(wire));

    zerocopy::ObjectArray dst;
    CHECK_FALSE((dst << wire));
  }

  TEST_CASE("wire operations reuse output and reject inconsistent embedded count") {
    zerocopy::ObjectArray::Object obj;
    std::strncpy(obj.label, "car", sizeof(obj.label) - 1);

    zerocopy::ObjectArray src;
    src.set_source_id("fusion");
    REQUIRE(src.create(2));
    REQUIRE(src.push_value(obj));

    Bytes wire = Bytes::create(src.get_serialized_size());
    uint8_t* original = wire.data();
    CHECK((src >> wire));
    CHECK_EQ(wire.data(), original);

    zerocopy::ObjectArray dst;
    REQUIRE(dst.create(1));
    REQUIRE(dst.is_owner());
    CHECK((dst << wire));
    CHECK_FALSE(dst.is_owner());
    CHECK_EQ(dst.count(), 1u);
    CHECK_EQ(dst.source_id(), "fusion");

    CHECK_FALSE((dst << Bytes()));

    static constexpr size_t kWireStructOffset = sizeof(uint32_t) + sizeof(uint32_t);
    static constexpr size_t kCountOffset = 88u;
    const uint32_t impossible_count = src.count() + 1u;
    std::memcpy(wire.data() + kWireStructOffset + kCountOffset, &impossible_count, sizeof(impossible_count));

    CHECK_FALSE((dst << wire));
    CHECK_FALSE(dst.is_valid());
  }

  TEST_CASE("deserialize rejects an overflowing record count with a valid pack size") {
    zerocopy::ObjectArray src;
    Bytes wire;
    REQUIRE((src >> wire));

    static constexpr size_t kWireStructOffset = sizeof(uint32_t) + sizeof(uint32_t);
    static constexpr size_t kCountOffset = 88u;
    static constexpr size_t kPackSizeOffset = 92u;
    const uint32_t count = 1U << 28U;
    const uint32_t pack_size = sizeof(zerocopy::ObjectArray::Object);
    std::memcpy(wire.data() + kWireStructOffset + kCountOffset, &count, sizeof(count));
    std::memcpy(wire.data() + kWireStructOffset + kPackSizeOffset, &pack_size, sizeof(pack_size));

    zerocopy::ObjectArray dst;
    REQUIRE(dst.create(1));
    CHECK_FALSE((dst << wire));
    CHECK_FALSE(dst.is_valid());
    CHECK_EQ(dst.count(), 0u);
    CHECK_EQ(dst.pack_size(), 0u);
    CHECK_EQ(dst.data(), nullptr);
  }
}

// NOLINTEND
