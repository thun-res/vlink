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

#include "./impl/someip_serializer.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <limits>

TEST_SUITE("impl-SomeipSerializer") {
  TEST_CASE("size-only writer advances without reading source storage") {
    vlink::SomeipSerializer::Writer writer(nullptr, 8U);

    REQUIRE(writer.append(nullptr, 3U));
    REQUIRE(writer.append_unsigned(0x1234U, sizeof(uint16_t)));
    CHECK(writer.position() == 5U);

    CHECK_FALSE(writer.append(nullptr, 4U));
    CHECK(writer.position() == 5U);

    size_t length_position = 0U;
    size_t data_position = 0U;
    CHECK_FALSE(writer.begin_length_delimited(length_position, data_position));
    CHECK(writer.position() == 5U);
  }

  TEST_CASE("writer backfills a byte-count length without moving its cursor") {
    std::array<uint8_t, 8> storage{};
    vlink::SomeipSerializer::Writer writer(storage.data(), storage.size());

    size_t length_position = 0U;
    size_t data_position = 0U;
    REQUIRE(writer.begin_length_delimited(length_position, data_position));
    REQUIRE(vlink::SomeipSerializer::write_value(writer, uint16_t{0x1234U}));
    REQUIRE(vlink::SomeipSerializer::write_value(writer, true));
    REQUIRE(writer.end_length_delimited(length_position, data_position));
    CHECK(writer.position() == 7U);

    REQUIRE(vlink::SomeipSerializer::write_value(writer, uint8_t{0xAAU}));

    const std::array<uint8_t, 8> expected = {0x00, 0x00, 0x00, 0x03, 0x12, 0x34, 0x01, 0xAA};
    CHECK(storage == expected);
    CHECK(writer.position() == expected.size());

    CHECK_FALSE(writer.patch_uint32(5U, 1U));
    CHECK_FALSE(writer.append_unsigned(0U, 0U));
    CHECK_FALSE(writer.append_unsigned(0U, sizeof(uint64_t) + 1U));
    CHECK(writer.position() == expected.size());
  }

  TEST_CASE("size-only writer enforces the SOME/IP message payload limit") {
    CHECK(vlink::SomeipSerializer::kMaxPayloadSize == static_cast<size_t>(std::numeric_limits<uint32_t>::max()) - 8U);

    vlink::SomeipSerializer::Writer writer(nullptr, vlink::SomeipSerializer::kMaxPayloadSize + 1U);

    REQUIRE(writer.append(nullptr, vlink::SomeipSerializer::kMaxPayloadSize));
    CHECK(writer.position() == vlink::SomeipSerializer::kMaxPayloadSize);
    CHECK_FALSE(writer.append(nullptr, 1U));
    CHECK(writer.position() == vlink::SomeipSerializer::kMaxPayloadSize);
  }

  TEST_CASE("reader keeps a nested field inside its declared boundary") {
    const std::array<uint8_t, 8> storage = {0x00, 0x00, 0x00, 0x03, 0x12, 0x34, 0x01, 0xAA};
    vlink::SomeipSerializer::Reader reader(storage.data(), storage.size());

    size_t field_end = 0U;
    REQUIRE(reader.begin_length_delimited(reader.size(), field_end));
    CHECK(field_end == 7U);

    uint16_t word = 0U;
    bool flag = false;
    REQUIRE(vlink::SomeipSerializer::read_value(reader, word, field_end));
    REQUIRE(vlink::SomeipSerializer::read_value(reader, flag, field_end));
    CHECK(word == 0x1234U);
    CHECK(flag);
    CHECK(reader.position() == field_end);

    uint8_t tail = 0U;
    CHECK_FALSE(vlink::SomeipSerializer::read_value(reader, tail, field_end));
    CHECK(reader.position() == field_end);

    REQUIRE(vlink::SomeipSerializer::read_value(reader, tail, reader.size()));
    CHECK(tail == 0xAAU);
    CHECK(reader.current_data() == nullptr);
  }

  TEST_CASE("reader read and skip failures leave the cursor unchanged") {
    const std::array<uint8_t, 4> storage = {0x10, 0x20, 0x30, 0x40};
    vlink::SomeipSerializer::Reader reader(storage.data(), storage.size());

    std::array<uint8_t, 2> prefix{};
    REQUIRE(reader.read(prefix.data(), prefix.size(), reader.size()));
    CHECK(prefix == std::array<uint8_t, 2>{0x10, 0x20});
    CHECK(reader.position() == 2U);

    std::array<uint8_t, 2> rejected = {0xAA, 0xBB};
    CHECK_FALSE(reader.read(rejected.data(), rejected.size(), 3U));
    CHECK(rejected == std::array<uint8_t, 2>{0xAA, 0xBB});
    CHECK(reader.position() == 2U);

    CHECK_FALSE(reader.skip(2U, 3U));
    CHECK(reader.position() == 2U);
    REQUIRE(reader.skip(1U, reader.size()));
    CHECK(reader.position() == 3U);

    uint64_t value = 0x55U;
    CHECK_FALSE(reader.read_unsigned(value, sizeof(uint16_t), reader.size()));
    CHECK(value == 0x55U);
    CHECK(reader.position() == 3U);

    REQUIRE(reader.read_unsigned(value, sizeof(uint8_t), reader.size()));
    CHECK(value == 0x40U);
    CHECK(reader.position() == reader.size());
  }

  TEST_CASE("reader rejects a length body larger than its enclosing range") {
    const std::array<uint8_t, 6> storage = {0x00, 0x00, 0x00, 0x05, 0x12, 0x34};
    vlink::SomeipSerializer::Reader reader(storage.data(), storage.size());

    size_t field_end = 0x55U;
    CHECK_FALSE(reader.begin_length_delimited(reader.size(), field_end));

    const std::array<uint8_t, 8> nested_storage = {0x00, 0x00, 0x00, 0x04, 0x12, 0x34, 0x56, 0x78};
    vlink::SomeipSerializer::Reader nested_reader(nested_storage.data(), nested_storage.size());
    CHECK_FALSE(nested_reader.begin_length_delimited(6U, field_end));

    vlink::SomeipSerializer::Reader empty_reader(nullptr, 0U);
    uint64_t value = 0xAAU;
    CHECK_FALSE(empty_reader.read_unsigned(value, sizeof(uint8_t), empty_reader.size()));
    CHECK(value == 0xAAU);
    CHECK(empty_reader.current_data() == nullptr);
  }

  TEST_CASE("primitive decoder does not modify a value when its boundary is too short") {
    const std::array<uint8_t, 4> storage = {0x12, 0x34, 0x56, 0x78};
    vlink::SomeipSerializer::Reader reader(storage.data(), storage.size());

    uint32_t value = 0xAABBCCDDU;
    CHECK_FALSE(vlink::SomeipSerializer::read_value(reader, value, 3U));
    CHECK(value == 0xAABBCCDDU);
    CHECK(reader.position() == 0U);

    REQUIRE(vlink::SomeipSerializer::read_value(reader, value, reader.size()));
    CHECK(value == 0x12345678U);
    CHECK(reader.position() == reader.size());
  }
}

// NOLINTEND
