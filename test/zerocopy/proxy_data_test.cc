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

#include "./zerocopy/proxy_data.h"

#include <doctest/doctest.h>

#include <cstring>
#include <string>

#include "../common_test.h"
#include "./zerocopy/header.h"

namespace {

constexpr size_t kProxyDataWireStructOffset = sizeof(uint32_t) + sizeof(uint32_t);
constexpr size_t kProxyDataSizeOffset = 8u;
constexpr size_t kProxyDataDataSizeOffset = 44u;
constexpr size_t kProxyDataSerPosOffset = 56u;
constexpr size_t kProxyDataHostnamePosOffset = 64u;
constexpr size_t kProxyDataHostnameSizeOffset = 68u;

template <typename T>
void write_proxy_data_field(Bytes& wire, size_t offset, T value) {
  std::memcpy(wire.data() + kProxyDataWireStructOffset + offset, &value, sizeof(value));
}

Bytes make_proxy_data_wire() {
  Bytes raw = Bytes::create(4u);
  raw[0] = 0x11u;
  raw[1] = 0x22u;
  raw[2] = 0x33u;
  raw[3] = 0x44u;

  zerocopy::ProxyData src;
  src.create(raw, "topic", "ser", static_cast<uint32_t>(SchemaType::kRaw), "host");

  Bytes wire;
  CHECK((src >> wire));
  return wire;
}

}  // namespace

TEST_SUITE("zerocopy-ProxyData") {
  TEST_CASE("default construction yields invalid empty object") {
    zerocopy::ProxyData pd;

    CHECK_FALSE(pd.is_valid());
    CHECK_EQ(pd.size(), 0u);
    CHECK_FALSE(pd.is_owner());
    CHECK_EQ(pd.control_id(), 0u);
    CHECK_EQ(pd.mode(), 0u);
    CHECK_EQ(pd.timestamp(), 0LL);
    CHECK_EQ(pd.seq(), 0LL);
    CHECK_EQ(pd.schema(), 0u);
    CHECK(pd.raw().empty());
    CHECK(pd.url().empty());
    CHECK(pd.ser().empty());
    CHECK(pd.hostname().empty());
  }

  TEST_CASE("sizeof is exactly 80 bytes") { CHECK_EQ(sizeof(zerocopy::ProxyData), 80u); }

  TEST_CASE("all scalar setter/getter pairs round-trip") {
    zerocopy::ProxyData pd;

    pd.set_control_id(42u);
    CHECK_EQ(pd.control_id(), 42u);

    pd.set_mode(7u);
    CHECK_EQ(pd.mode(), 7u);

    pd.set_timestamp(1234567890LL);
    CHECK_EQ(pd.timestamp(), 1234567890LL);

    pd.set_seq(999LL);
    CHECK_EQ(pd.seq(), 999LL);

    pd.set_schema(5u);
    CHECK_EQ(pd.schema(), 5u);
  }

  TEST_CASE("create with all fields populates url ser hostname and raw") {
    Bytes raw = Bytes::create(32);

    for (size_t i = 0; i < 32; ++i) {
      raw.data()[i] = static_cast<uint8_t>(i);
    }

    zerocopy::ProxyData pd;
    pd.set_control_id(1u);
    pd.set_timestamp(5000LL);
    pd.create(raw, "dds://my/topic", "protobuf", static_cast<uint32_t>(SchemaType::kProtobuf), "host01");

    CHECK(pd.is_valid());
    CHECK(pd.is_owner());
    CHECK_EQ(pd.url(), "dds://my/topic");
    CHECK_EQ(pd.ser(), "protobuf");
    CHECK_EQ(pd.schema(), static_cast<uint32_t>(SchemaType::kProtobuf));
    CHECK_EQ(pd.hostname(), "host01");

    Bytes raw_view = pd.raw();
    CHECK_EQ(raw_view.size(), 32u);
    CHECK_EQ(std::memcmp(raw_view.data(), raw.data(), 32), 0);
  }

  TEST_CASE("create without hostname leaves hostname empty") {
    Bytes raw = Bytes::create(8);

    zerocopy::ProxyData pd;
    pd.create(raw, "intra://topic", "raw");

    CHECK(pd.is_valid());
    CHECK_EQ(pd.url(), "intra://topic");
    CHECK_EQ(pd.ser(), "raw");
    CHECK_EQ(pd.schema(), 0u);
    CHECK(pd.hostname().empty());
  }

  TEST_CASE("create with empty Bytes still sets url ser hostname") {
    Bytes empty;

    zerocopy::ProxyData pd;
    pd.create(empty, "shm://topic", "bytes", 0u, "myhost");

    CHECK_EQ(pd.url(), "shm://topic");
    CHECK_EQ(pd.ser(), "bytes");
    CHECK_EQ(pd.hostname(), "myhost");
  }

  TEST_CASE("clear resets all fields to zero") {
    Bytes raw = Bytes::create(64);

    zerocopy::ProxyData pd;
    pd.create(raw, "dds://clear_test", "protobuf");
    pd.set_control_id(99u);

    pd.clear();

    CHECK_FALSE(pd.is_valid());
    CHECK_EQ(pd.size(), 0u);
    CHECK_FALSE(pd.is_owner());
    CHECK_EQ(pd.control_id(), 0u);
    CHECK(pd.url().empty());
    CHECK(pd.ser().empty());
    CHECK(pd.hostname().empty());
  }

  TEST_CASE("shallow_copy aliases the tail buffer") {
    Bytes raw = Bytes::create(8);

    zerocopy::ProxyData src;
    src.create(raw, "dds://a", "raw");

    zerocopy::ProxyData dst;
    CHECK(dst.shallow_copy(src));

    CHECK_FALSE(dst.is_owner());
    CHECK_EQ(dst.url(), "dds://a");
    CHECK_EQ(dst.ser(), "raw");
  }

  TEST_CASE("shallow_copy self returns false") {
    Bytes raw = Bytes::create(8);

    zerocopy::ProxyData pd;
    pd.create(raw, "dds://a", "raw");

    CHECK_FALSE(pd.shallow_copy(pd));
  }

  TEST_CASE("deep_copy creates owned independent copy") {
    Bytes raw = Bytes::create(16);
    raw.data()[0] = 0xABu;

    zerocopy::ProxyData src;
    src.create(raw, "dds://b", "protobuf", static_cast<uint32_t>(SchemaType::kProtobuf), "host");

    zerocopy::ProxyData dst;
    CHECK(dst.deep_copy(src));

    CHECK(dst.is_owner());
    CHECK_EQ(dst.url(), "dds://b");
    CHECK_EQ(dst.ser(), "protobuf");
    CHECK_EQ(dst.schema(), static_cast<uint32_t>(SchemaType::kProtobuf));
    CHECK_EQ(dst.hostname(), "host");
  }

  TEST_CASE("deep_copy self returns false") {
    Bytes raw = Bytes::create(8);

    zerocopy::ProxyData pd;
    pd.create(raw, "dds://c", "raw");

    CHECK_FALSE(pd.deep_copy(pd));
  }

  TEST_CASE("move_copy transfers ownership and invalidates source") {
    Bytes raw = Bytes::create(16);

    zerocopy::ProxyData src;
    src.create(raw, "dds://d", "bytes");

    zerocopy::ProxyData dst;
    CHECK(dst.move_copy(src));

    CHECK(dst.is_owner());
    CHECK_EQ(dst.url(), "dds://d");
    CHECK_FALSE(src.is_valid());
  }

  TEST_CASE("move_copy self returns false") {
    Bytes raw = Bytes::create(8);

    zerocopy::ProxyData pd;
    pd.create(raw, "dds://e", "raw");

    CHECK_FALSE(pd.move_copy(pd));
  }

  TEST_CASE("copy constructor performs deep copy") {
    Bytes raw = Bytes::create(32);
    raw.data()[0] = 0x42u;

    zerocopy::ProxyData src;
    src.create(raw, "dds://copy_test", "protobuf", static_cast<uint32_t>(SchemaType::kProtobuf), "host_copy");

    zerocopy::ProxyData copy(src);

    CHECK(copy.is_owner());
    CHECK_EQ(copy.url(), "dds://copy_test");
    CHECK_EQ(copy.hostname(), "host_copy");
    CHECK_EQ(copy.schema(), static_cast<uint32_t>(SchemaType::kProtobuf));
  }

  TEST_CASE("move constructor transfers ownership") {
    Bytes raw = Bytes::create(16);

    zerocopy::ProxyData src;
    src.create(raw, "dds://move_test", "raw");

    zerocopy::ProxyData moved(std::move(src));

    CHECK(moved.is_owner());
    CHECK_EQ(moved.url(), "dds://move_test");
    CHECK_FALSE(src.is_valid());
  }

  TEST_CASE("copy assignment performs deep copy") {
    Bytes raw = Bytes::create(16);

    zerocopy::ProxyData src;
    src.create(raw, "dds://assign_test", "bytes");

    zerocopy::ProxyData dst;
    dst = src;

    CHECK(dst.is_owner());
    CHECK_EQ(dst.url(), "dds://assign_test");
  }

  TEST_CASE("move assignment transfers ownership") {
    Bytes raw = Bytes::create(16);

    zerocopy::ProxyData src;
    src.create(raw, "dds://move_assign_test", "raw");

    zerocopy::ProxyData dst;
    dst = std::move(src);

    CHECK(dst.is_owner());
    CHECK_EQ(dst.url(), "dds://move_assign_test");
    CHECK_FALSE(src.is_valid());
  }

  TEST_CASE("serialize and deserialize round-trip preserves all fields") {
    const std::string payload_str = "hello proxy data test payload";
    Bytes raw = Bytes::create(payload_str.size());
    std::memcpy(raw.data(), payload_str.data(), payload_str.size());

    zerocopy::ProxyData src;
    src.set_control_id(10u);
    src.set_mode(2u);
    src.set_timestamp(9999LL);
    src.set_seq(42LL);
    src.create(raw, "dds://sensor/lidar", "protobuf", static_cast<uint32_t>(SchemaType::kProtobuf), "vehicle01");

    Bytes wire;
    CHECK((src >> wire));
    CHECK_FALSE(wire.empty());
    CHECK(zerocopy::ProxyData::check_valid(wire));
    CHECK_EQ(wire.size(), src.get_serialized_size());

    zerocopy::ProxyData dst;
    CHECK((dst << wire));

    CHECK(dst.is_valid());
    CHECK_FALSE(dst.is_owner());
    CHECK_EQ(dst.control_id(), 10u);
    CHECK_EQ(dst.mode(), 2u);
    CHECK_EQ(dst.timestamp(), 9999LL);
    CHECK_EQ(dst.seq(), 42LL);
    CHECK_EQ(dst.url(), "dds://sensor/lidar");
    CHECK_EQ(dst.ser(), "protobuf");
    CHECK_EQ(dst.schema(), static_cast<uint32_t>(SchemaType::kProtobuf));
    CHECK_EQ(dst.hostname(), "vehicle01");

    Bytes raw_view = dst.raw();
    REQUIRE_EQ(raw_view.size(), payload_str.size());
    CHECK_EQ(std::memcmp(raw_view.data(), payload_str.data(), payload_str.size()), 0);
  }

  TEST_CASE("serialization reuses a correctly sized output buffer") {
    Bytes raw = Bytes::create(4u);

    zerocopy::ProxyData src;
    src.create(raw, "dds://reuse", "raw", static_cast<uint32_t>(SchemaType::kRaw), "host");

    Bytes wire = Bytes::create(src.get_serialized_size());
    uint8_t* original = wire.data();

    CHECK((src >> wire));
    CHECK_EQ(wire.data(), original);
    CHECK(zerocopy::ProxyData::check_valid(wire));
  }

  TEST_CASE("deserialize rejects empty and inconsistent wire layouts") {
    SUBCASE("empty payload") {
      zerocopy::ProxyData dst;
      CHECK_FALSE((dst << Bytes()));
      CHECK_FALSE(dst.is_valid());
    }

    SUBCASE("declared payload size does not match wire size") {
      Bytes wire = make_proxy_data_wire();
      write_proxy_data_field<size_t>(wire, kProxyDataSizeOffset, 17u);

      zerocopy::ProxyData dst;
      CHECK_FALSE((dst << wire));
      CHECK_FALSE(dst.is_valid());
    }

    SUBCASE("raw region does not end at url region") {
      Bytes wire = make_proxy_data_wire();
      write_proxy_data_field<uint32_t>(wire, kProxyDataDataSizeOffset, 5u);

      zerocopy::ProxyData dst;
      CHECK_FALSE((dst << wire));
      CHECK_FALSE(dst.is_valid());
    }

    SUBCASE("url region does not end at serializer region") {
      Bytes wire = make_proxy_data_wire();
      write_proxy_data_field<uint32_t>(wire, kProxyDataSerPosOffset, 10u);

      zerocopy::ProxyData dst;
      CHECK_FALSE((dst << wire));
      CHECK_FALSE(dst.is_valid());
    }

    SUBCASE("serializer region does not end at hostname region") {
      Bytes wire = make_proxy_data_wire();
      write_proxy_data_field<uint32_t>(wire, kProxyDataHostnamePosOffset, 13u);

      zerocopy::ProxyData dst;
      CHECK_FALSE((dst << wire));
      CHECK_FALSE(dst.is_valid());
    }

    SUBCASE("hostname region does not end at payload size") {
      Bytes wire = make_proxy_data_wire();
      write_proxy_data_field<uint32_t>(wire, kProxyDataHostnameSizeOffset, 5u);

      zerocopy::ProxyData dst;
      CHECK_FALSE((dst << wire));
      CHECK_FALSE(dst.is_valid());
    }
  }

  TEST_CASE("deserialize releases a previous owned buffer before borrowing wire data") {
    Bytes raw = Bytes::create(4u);
    zerocopy::ProxyData dst;
    dst.create(raw, "dds://owned", "raw");
    REQUIRE(dst.is_owner());

    Bytes wire = make_proxy_data_wire();
    REQUIRE((dst << wire));

    CHECK_FALSE(dst.is_owner());
    CHECK_EQ(dst.url(), "topic");
    CHECK_EQ(dst.ser(), "ser");
    CHECK_EQ(dst.hostname(), "host");
  }

  TEST_CASE("schema field survives round-trip for every SchemaType family") {
    const uint32_t schema_values[] = {
        static_cast<uint32_t>(SchemaType::kUnknown),     static_cast<uint32_t>(SchemaType::kRaw),
        static_cast<uint32_t>(SchemaType::kZeroCopy),    static_cast<uint32_t>(SchemaType::kProtobuf),
        static_cast<uint32_t>(SchemaType::kFlatbuffers),
    };

    for (uint32_t schema : schema_values) {
      Bytes raw = Bytes::create(4);
      raw.data()[0] = 0x01u;

      zerocopy::ProxyData src;
      src.create(raw, "dds://roundtrip/schema", "ser_name", schema, "hostX");
      CHECK_EQ(src.schema(), schema);

      Bytes wire;
      REQUIRE((src >> wire));

      zerocopy::ProxyData dst;
      REQUIRE((dst << wire));
      CHECK_EQ(dst.schema(), schema);
      CHECK_EQ(dst.url(), "dds://roundtrip/schema");
      CHECK_EQ(dst.hostname(), "hostX");
    }
  }

  TEST_CASE("check_valid rejects empty, corrupted begin magic, and corrupted end magic") {
    Bytes raw = Bytes::create(16);

    zerocopy::ProxyData pd;
    pd.create(raw, "dds://topic", "raw");

    Bytes wire;
    pd >> wire;

    SUBCASE("empty bytes") {
      Bytes empty;
      CHECK_FALSE(zerocopy::ProxyData::check_valid(empty));
    }

    SUBCASE("corrupted begin magic") {
      wire[0] ^= 0xFFu;
      CHECK_FALSE(zerocopy::ProxyData::check_valid(wire));
    }

    SUBCASE("corrupted end magic") {
      wire[wire.size() - 1] ^= 0xFFu;
      CHECK_FALSE(zerocopy::ProxyData::check_valid(wire));
    }
  }

  TEST_CASE("get_serialized_size equals magic + version + struct + payload + magic") {
    Bytes raw = Bytes::create(20);

    zerocopy::ProxyData pd;
    pd.create(raw, "url", "ser");

    size_t expected = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(zerocopy::ProxyData) + pd.size() + sizeof(uint32_t);
    CHECK_EQ(pd.get_serialized_size(), expected);
  }

  TEST_CASE("is_valid checks internal region consistency") {
    zerocopy::ProxyData pd;
    CHECK_FALSE(pd.is_valid());

    Bytes raw = Bytes::create(8);
    pd.create(raw, "url", "ser");
    CHECK(pd.is_valid());

    pd.clear();
    CHECK_FALSE(pd.is_valid());
  }

  TEST_CASE("reserved fields are writable, survive copy / serialization, and reset on clear") {
    Bytes raw = Bytes::create(8);

    zerocopy::ProxyData src;
    src.create(raw, "dds://r", "ser");
    src.get_reserved() = 0xABu;
    src.get_reserved2() = 0xBEEFu;

    CHECK_EQ(static_cast<int>(src.get_reserved()), 0xAB);
    CHECK_EQ(static_cast<int>(src.get_reserved2()), 0xBEEF);

    Bytes wire;
    REQUIRE((src >> wire));

    zerocopy::ProxyData dst;
    REQUIRE((dst << wire));
    CHECK_EQ(static_cast<int>(dst.get_reserved()), 0xAB);
    CHECK_EQ(static_cast<int>(dst.get_reserved2()), 0xBEEF);

    zerocopy::ProxyData copy(src);
    CHECK_EQ(static_cast<int>(copy.get_reserved()), 0xAB);
    CHECK_EQ(static_cast<int>(copy.get_reserved2()), 0xBEEF);

    src.clear();
    CHECK_EQ(static_cast<int>(src.get_reserved()), 0);
    CHECK_EQ(static_cast<int>(src.get_reserved2()), 0);
  }

  TEST_CASE("deserialize rejects a wire payload whose major version differs") {
    Bytes raw = Bytes::create(8);

    zerocopy::ProxyData src;
    src.create(raw, "dds://r", "ser");

    Bytes wire;
    REQUIRE((src >> wire));
    REQUIRE(zerocopy::ProxyData::check_valid(wire));

    uint32_t foreign_version = (zerocopy::version_major(zerocopy::kWireVersion) + 1U) * 1000000U;
    std::memcpy(wire.data() + sizeof(uint32_t), &foreign_version, sizeof(foreign_version));

    CHECK_FALSE(zerocopy::ProxyData::check_valid(wire));

    zerocopy::ProxyData dst;
    CHECK_FALSE((dst << wire));
  }

  TEST_CASE("assignment operators are safe under self-assignment") {
    Bytes raw = Bytes::create(8u);
    zerocopy::ProxyData pd;
    pd.create(raw, "dds://self", "ser");
    REQUIRE(pd.is_valid());

    zerocopy::ProxyData& alias = pd;
    pd = alias;
    CHECK(pd.is_valid());

    pd = std::move(alias);
    CHECK(pd.is_valid());
  }

  TEST_CASE("deep_copy reuses an owned buffer of matching size in place") {
    Bytes raw_a = Bytes::create(8u);
    Bytes raw_b = Bytes::create(8u);

    zerocopy::ProxyData src;
    zerocopy::ProxyData dst;
    src.create(raw_a, "dds://aaa", "ser");
    dst.create(raw_b, "dds://bbb", "ser");
    REQUIRE(src.is_valid());
    REQUIRE(dst.is_valid());

    REQUIRE_EQ(src.size(), dst.size());
    REQUIRE(dst.is_owner());

    CHECK(dst.deep_copy(src));
    CHECK(dst.is_valid());
    CHECK(dst.is_owner());
    CHECK_EQ(dst.size(), src.size());
  }

  TEST_CASE("copy helpers release previous owned buffers before aliasing or moving") {
    Bytes raw_a = Bytes::create(4u);
    Bytes raw_b = Bytes::create(4u);

    zerocopy::ProxyData src;
    src.create(raw_a, "topic", "ser", static_cast<uint32_t>(SchemaType::kRaw), "host");

    zerocopy::ProxyData dst;
    dst.create(raw_b, "old", "raw");
    REQUIRE(dst.is_owner());

    CHECK(dst.shallow_copy(src));
    CHECK_FALSE(dst.is_owner());
    CHECK_EQ(dst.url(), "topic");

    zerocopy::ProxyData move_dst;
    move_dst.create(raw_b, "old2", "raw");
    REQUIRE(move_dst.is_owner());

    CHECK(move_dst.move_copy(src));
    CHECK(move_dst.is_owner());
    CHECK_FALSE(src.is_valid());
    CHECK_EQ(move_dst.hostname(), "host");
  }
}

// NOLINTEND
