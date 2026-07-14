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

#include "./zerocopy/camera_frame.h"

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

TEST_SUITE("zerocopy-CameraFrame") {
  TEST_CASE("default construction yields invalid empty frame") {
    zerocopy::CameraFrame frame;

    CHECK_FALSE(frame.is_valid());
    CHECK_EQ(frame.format(), zerocopy::CameraFrame::kFormatUnknown);
    CHECK_EQ(frame.stream(), zerocopy::CameraFrame::kStreamUnknown);
    CHECK_EQ(frame.width(), 0u);
    CHECK_EQ(frame.height(), 0u);
    CHECK_EQ(frame.freq(), 0u);
    CHECK_EQ(frame.channel(), 0u);
    CHECK_EQ(frame.size(), 0u);
    CHECK_EQ(frame.data(), nullptr);
    CHECK_FALSE(frame.is_owner());
  }

  TEST_CASE("sizeof is exactly 80 bytes") { CHECK_EQ(sizeof(zerocopy::CameraFrame), 80u); }

  TEST_CASE("all metadata accessors round-trip") {
    zerocopy::CameraFrame frame;

    frame.set_width(1920u);
    CHECK_EQ(frame.width(), 1920u);

    frame.set_height(1080u);
    CHECK_EQ(frame.height(), 1080u);

    frame.set_freq(30u);
    CHECK_EQ(frame.freq(), 30u);

    frame.set_channel(2u);
    CHECK_EQ(frame.channel(), 2u);

    frame.set_format(zerocopy::CameraFrame::kFormatNv12);
    CHECK_EQ(frame.format(), zerocopy::CameraFrame::kFormatNv12);

    frame.set_stream(zerocopy::CameraFrame::kStreamI);
    CHECK_EQ(frame.stream(), zerocopy::CameraFrame::kStreamI);
  }

  TEST_CASE("all Format enum values can be set and retrieved") {
    struct FormatCase {
      zerocopy::CameraFrame::Format format;
      uint8_t value;
    };

    const FormatCase formats[] = {
        {zerocopy::CameraFrame::kFormatUnknown, 0U},
        {zerocopy::CameraFrame::kFormatYuv420, 1U},
        {zerocopy::CameraFrame::kFormatYuv422, 2U},
        {zerocopy::CameraFrame::kFormatYuv444, 3U},
        {zerocopy::CameraFrame::kFormatNv12, 4U},
        {zerocopy::CameraFrame::kFormatNv21, 5U},
        {zerocopy::CameraFrame::kFormatYuyv, 6U},
        {zerocopy::CameraFrame::kFormatYvyu, 7U},
        {zerocopy::CameraFrame::kFormatUyvy, 8U},
        {zerocopy::CameraFrame::kFormatVyuy, 9U},
        {zerocopy::CameraFrame::kFormatBgr888Packed, 10U},
        {zerocopy::CameraFrame::kFormatRgb888Packed, 11U},
        {zerocopy::CameraFrame::kFormatRgb888Planar, 12U},
        {zerocopy::CameraFrame::kFormatMono8, 13U},
        {zerocopy::CameraFrame::kFormatMono16, 14U},
        {zerocopy::CameraFrame::kFormatRgba8888Packed, 15U},
        {zerocopy::CameraFrame::kFormatBgra8888Packed, 16U},
        {zerocopy::CameraFrame::kFormatUint8C1, 20U},
        {zerocopy::CameraFrame::kFormatUint8C2, 21U},
        {zerocopy::CameraFrame::kFormatUint8C3, 22U},
        {zerocopy::CameraFrame::kFormatUint8C4, 23U},
        {zerocopy::CameraFrame::kFormatInt8C1, 24U},
        {zerocopy::CameraFrame::kFormatInt8C2, 25U},
        {zerocopy::CameraFrame::kFormatInt8C3, 26U},
        {zerocopy::CameraFrame::kFormatInt8C4, 27U},
        {zerocopy::CameraFrame::kFormatUint16C1, 28U},
        {zerocopy::CameraFrame::kFormatUint16C2, 29U},
        {zerocopy::CameraFrame::kFormatUint16C3, 30U},
        {zerocopy::CameraFrame::kFormatUint16C4, 31U},
        {zerocopy::CameraFrame::kFormatInt16C1, 32U},
        {zerocopy::CameraFrame::kFormatInt16C2, 33U},
        {zerocopy::CameraFrame::kFormatInt16C3, 34U},
        {zerocopy::CameraFrame::kFormatInt16C4, 35U},
        {zerocopy::CameraFrame::kFormatInt32C1, 36U},
        {zerocopy::CameraFrame::kFormatInt32C2, 37U},
        {zerocopy::CameraFrame::kFormatInt32C3, 38U},
        {zerocopy::CameraFrame::kFormatInt32C4, 39U},
        {zerocopy::CameraFrame::kFormatFloat32C1, 40U},
        {zerocopy::CameraFrame::kFormatFloat32C2, 41U},
        {zerocopy::CameraFrame::kFormatFloat32C3, 42U},
        {zerocopy::CameraFrame::kFormatFloat32C4, 43U},
        {zerocopy::CameraFrame::kFormatFloat64C1, 44U},
        {zerocopy::CameraFrame::kFormatFloat64C2, 45U},
        {zerocopy::CameraFrame::kFormatFloat64C3, 46U},
        {zerocopy::CameraFrame::kFormatFloat64C4, 47U},
        {zerocopy::CameraFrame::kFormatBayerRggb8, 60U},
        {zerocopy::CameraFrame::kFormatBayerBggr8, 61U},
        {zerocopy::CameraFrame::kFormatBayerGbrg8, 62U},
        {zerocopy::CameraFrame::kFormatBayerGrbg8, 63U},
        {zerocopy::CameraFrame::kFormatBayerRggb16, 64U},
        {zerocopy::CameraFrame::kFormatBayerBggr16, 65U},
        {zerocopy::CameraFrame::kFormatBayerGbrg16, 66U},
        {zerocopy::CameraFrame::kFormatBayerGrbg16, 67U},
        {zerocopy::CameraFrame::kFormatJpeg, 101U},
        {zerocopy::CameraFrame::kFormatH264, 102U},
        {zerocopy::CameraFrame::kFormatH265, 103U},
        {zerocopy::CameraFrame::kFormatMjpeg, 104U},
        {zerocopy::CameraFrame::kFormatPng, 105U},
        {zerocopy::CameraFrame::kFormatWebp, 106U},
        {zerocopy::CameraFrame::kFormatH266, 107U},
        {zerocopy::CameraFrame::kFormatAv1, 108U},
    };

    for (const auto& item : formats) {
      CHECK_EQ(static_cast<uint8_t>(item.format), item.value);
      zerocopy::CameraFrame f;
      f.set_format(item.format);
      CHECK_EQ(f.format(), item.format);
    }
  }

  TEST_CASE("encoding helpers cover common ROS OpenCV and codec names") {
    CHECK_EQ(zerocopy::CameraFrame::format_from_encoding("rgb8"), zerocopy::CameraFrame::kFormatRgb888Packed);
    CHECK_EQ(zerocopy::CameraFrame::format_from_encoding("BGR8"), zerocopy::CameraFrame::kFormatBgr888Packed);
    CHECK_EQ(zerocopy::CameraFrame::format_from_encoding("mono16"), zerocopy::CameraFrame::kFormatMono16);
    CHECK_EQ(zerocopy::CameraFrame::format_from_encoding("32FC1"), zerocopy::CameraFrame::kFormatFloat32C1);
    CHECK_EQ(zerocopy::CameraFrame::format_from_encoding("bayer-rggb8"), zerocopy::CameraFrame::kFormatBayerRggb8);
    CHECK_EQ(zerocopy::CameraFrame::format_from_encoding("hevc"), zerocopy::CameraFrame::kFormatH265);
    CHECK_EQ(zerocopy::CameraFrame::format_from_encoding("jpg"), zerocopy::CameraFrame::kFormatJpeg);
    CHECK_EQ(zerocopy::CameraFrame::format_from_encoding("webp"), zerocopy::CameraFrame::kFormatWebp);
    CHECK_EQ(zerocopy::CameraFrame::encoding_from_format(zerocopy::CameraFrame::kFormatFloat32C1), "32FC1");
    CHECK_EQ(zerocopy::CameraFrame::encoding_from_format(zerocopy::CameraFrame::kFormatUnknown), "unknown");
  }

  TEST_CASE("all Stream enum values can be set and retrieved") {
    zerocopy::CameraFrame f;

    f.set_stream(zerocopy::CameraFrame::kStreamI);
    CHECK_EQ(f.stream(), zerocopy::CameraFrame::kStreamI);

    f.set_stream(zerocopy::CameraFrame::kStreamP);
    CHECK_EQ(f.stream(), zerocopy::CameraFrame::kStreamP);

    f.set_stream(zerocopy::CameraFrame::kStreamB);
    CHECK_EQ(f.stream(), zerocopy::CameraFrame::kStreamB);

    f.set_stream(zerocopy::CameraFrame::kStreamUnknown);
    CHECK_EQ(f.stream(), zerocopy::CameraFrame::kStreamUnknown);
  }

  TEST_CASE("create succeeds for representative sizes and sets ownership") {
    size_t sz = 0;

    SUBCASE("single byte") { sz = 1; }
    SUBCASE("small") { sz = 64; }
    SUBCASE("NV12 1080p") { sz = 1920u * 1080u * 3u / 2u; }

    zerocopy::CameraFrame frame;
    CHECK(frame.create(sz));
    CHECK(frame.is_valid());
    CHECK(frame.is_owner());
    CHECK_EQ(frame.size(), sz);
    CHECK_NE(frame.data(), nullptr);
  }

  TEST_CASE("create with zero size returns false") {
    zerocopy::CameraFrame frame;

    CHECK_FALSE(frame.create(0));
    CHECK_FALSE(frame.is_valid());
  }

  TEST_CASE("create replaces previous owned buffer") {
    zerocopy::CameraFrame frame;

    REQUIRE(frame.create(100));
    REQUIRE(frame.create(200));

    CHECK_EQ(frame.size(), 200u);
    CHECK(frame.is_owner());
  }

  TEST_CASE("get_serialized_size equals magic + version + struct + payload + magic") {
    zerocopy::CameraFrame frame;

    SUBCASE("empty") {
      size_t expected = sizeof(uint32_t) + sizeof(uint32_t) + 80u + 0u + sizeof(uint32_t);
      CHECK_EQ(frame.get_serialized_size(), expected);
    }

    SUBCASE("with payload") {
      frame.create(1000);
      size_t expected = sizeof(uint32_t) + sizeof(uint32_t) + 80u + 1000u + sizeof(uint32_t);
      CHECK_EQ(frame.get_serialized_size(), expected);
    }
  }

  TEST_CASE("clear resets all fields") {
    zerocopy::CameraFrame frame;

    frame.set_width(1280u);
    frame.set_height(720u);
    frame.set_format(zerocopy::CameraFrame::kFormatH264);
    frame.create(1000);
    frame.header.seq = 99u;

    frame.clear();

    CHECK_FALSE(frame.is_valid());
    CHECK_FALSE(frame.is_owner());
    CHECK_EQ(frame.size(), 0u);
    CHECK_EQ(frame.data(), nullptr);
    CHECK_EQ(frame.width(), 0u);
    CHECK_EQ(frame.height(), 0u);
    CHECK_EQ(frame.header.seq, 0u);
  }

  TEST_CASE("clear then create again succeeds") {
    zerocopy::CameraFrame frame;

    frame.set_width(100u);
    frame.create(100);
    frame.clear();

    CHECK_FALSE(frame.is_valid());
    CHECK_EQ(frame.width(), 0u);

    CHECK(frame.create(50));
    CHECK(frame.is_valid());
    CHECK_EQ(frame.size(), 50u);
  }

  TEST_CASE("shallow_copy from CameraFrame aliases buffer and copies metadata") {
    zerocopy::CameraFrame src;

    src.set_width(1920u);
    src.set_height(1080u);
    src.set_format(zerocopy::CameraFrame::kFormatH265);
    src.set_stream(zerocopy::CameraFrame::kStreamP);
    src.set_freq(60u);
    src.set_channel(3u);
    src.create(256);

    zerocopy::CameraFrame dst;
    CHECK(dst.shallow_copy(src));

    CHECK_FALSE(dst.is_owner());
    CHECK_EQ(dst.data(), src.data());
    CHECK_EQ(dst.size(), 256u);
    CHECK_EQ(dst.width(), 1920u);
    CHECK_EQ(dst.height(), 1080u);
    CHECK_EQ(dst.format(), zerocopy::CameraFrame::kFormatH265);
    CHECK_EQ(dst.stream(), zerocopy::CameraFrame::kStreamP);
    CHECK_EQ(dst.freq(), 60u);
    CHECK_EQ(dst.channel(), 3u);
  }

  TEST_CASE("shallow_copy self returns false") {
    zerocopy::CameraFrame frame;

    frame.create(64);
    CHECK_FALSE(frame.shallow_copy(frame));
  }

  TEST_CASE("shallow_copy from raw pointer aliases the pointer") {
    std::vector<uint8_t> buf(200, 0xCAu);

    zerocopy::CameraFrame frame;
    CHECK(frame.shallow_copy(buf.data(), buf.size()));

    CHECK_FALSE(frame.is_owner());
    CHECK_EQ(frame.size(), 200u);
    CHECK_EQ(frame.data(), buf.data());
  }

  TEST_CASE("shallow_copy from raw pointer rejects null or zero size") {
    zerocopy::CameraFrame frame;

    CHECK_FALSE(frame.shallow_copy(nullptr, 64));

    std::vector<uint8_t> buf(8, 0xFFu);
    CHECK_FALSE(frame.shallow_copy(buf.data(), 0));
  }

  TEST_CASE("shallow_copy same raw pointer returns false") {
    std::vector<uint8_t> buf(64, 0x55u);
    zerocopy::CameraFrame frame;

    CHECK(frame.shallow_copy(buf.data(), buf.size()));
    CHECK_FALSE(frame.shallow_copy(buf.data(), buf.size()));
  }

  TEST_CASE("deep_copy from CameraFrame allocates owned copy") {
    zerocopy::CameraFrame src;

    src.set_width(320u);
    src.set_height(240u);
    src.set_format(zerocopy::CameraFrame::kFormatYuv420);
    src.create(320u * 240u * 3u / 2u);
    fill_pattern(const_cast<uint8_t*>(src.data()), src.size(), 0x6Bu);

    zerocopy::CameraFrame dst;
    CHECK(dst.deep_copy(src));

    CHECK(dst.is_owner());
    CHECK_EQ(dst.size(), src.size());
    CHECK_EQ(dst.width(), 320u);
    CHECK_EQ(dst.height(), 240u);
    CHECK(region_matches(src.data(), dst.data(), src.size()));
  }

  TEST_CASE("deep_copy into same-size owned buffer reuses memory") {
    zerocopy::CameraFrame src;
    src.create(64);
    fill_pattern(const_cast<uint8_t*>(src.data()), 64, 0xAAu);

    zerocopy::CameraFrame dst;
    dst.create(64);

    CHECK(dst.deep_copy(src));
    CHECK(dst.is_owner());
    CHECK_EQ(dst.size(), 64u);
    CHECK(region_matches(src.data(), dst.data(), 64));
  }

  TEST_CASE("deep_copy self returns false") {
    zerocopy::CameraFrame frame;

    frame.create(64);
    CHECK_FALSE(frame.deep_copy(frame));
  }

  TEST_CASE("deep_copy from raw pointer") {
    static constexpr size_t kN = 150;
    std::vector<uint8_t> src(kN);
    fill_pattern(src.data(), kN, 0xE7u);

    zerocopy::CameraFrame frame;
    CHECK(frame.deep_copy(src.data(), kN));

    CHECK(frame.is_owner());
    CHECK_EQ(frame.size(), kN);
    CHECK(region_matches(src.data(), frame.data(), kN));
  }

  TEST_CASE("deep_copy from raw pointer rejects self buffer and resizes owned storage") {
    zerocopy::CameraFrame frame;
    REQUIRE(frame.create(16));
    CHECK_FALSE(frame.deep_copy(const_cast<uint8_t*>(frame.data()), frame.size()));

    std::vector<uint8_t> larger(64);
    fill_pattern(larger.data(), larger.size(), 0x31u);
    CHECK(frame.deep_copy(larger.data(), larger.size()));
    CHECK(frame.is_owner());
    CHECK_EQ(frame.size(), larger.size());
    CHECK(region_matches(larger.data(), frame.data(), larger.size()));
  }

  TEST_CASE("deep_copy from raw pointer rejects null or zero size") {
    zerocopy::CameraFrame frame;

    CHECK_FALSE(frame.deep_copy(nullptr, 64));

    std::vector<uint8_t> buf(8);
    CHECK_FALSE(frame.deep_copy(buf.data(), 0));
  }

  TEST_CASE("fill_data is an alias for deep_copy from raw pointer") {
    static constexpr size_t kN = 80;
    std::vector<uint8_t> src(kN, 0x11u);

    zerocopy::CameraFrame frame;
    CHECK(frame.fill_data(src.data(), kN));

    CHECK(frame.is_owner());
    CHECK(region_matches(src.data(), frame.data(), kN));
  }

  TEST_CASE("move_copy transfers ownership and invalidates source") {
    zerocopy::CameraFrame src;

    src.set_width(640u);
    src.set_height(480u);
    src.set_format(zerocopy::CameraFrame::kFormatYuv422);
    src.create(640u * 480u * 2u);
    const uint8_t* ptr = src.data();

    zerocopy::CameraFrame dst;
    CHECK(dst.move_copy(src));

    CHECK(dst.is_owner());
    CHECK_EQ(dst.data(), ptr);
    CHECK_EQ(dst.width(), 640u);
    CHECK_FALSE(src.is_valid());
    CHECK_FALSE(src.is_owner());
    CHECK_EQ(src.data(), nullptr);
    CHECK_EQ(src.size(), 0u);
  }

  TEST_CASE("move_copy self returns false") {
    zerocopy::CameraFrame frame;

    frame.create(64);
    CHECK_FALSE(frame.move_copy(frame));
  }

  TEST_CASE("copy constructor performs deep copy") {
    zerocopy::CameraFrame src;

    src.set_width(800u);
    src.set_height(600u);
    src.set_format(zerocopy::CameraFrame::kFormatJpeg);
    src.set_stream(zerocopy::CameraFrame::kStreamB);
    src.create(800u * 600u);
    fill_pattern(const_cast<uint8_t*>(src.data()), src.size(), 0xBCu);

    zerocopy::CameraFrame copy(src);

    CHECK(copy.is_owner());
    CHECK_EQ(copy.width(), 800u);
    CHECK_EQ(copy.height(), 600u);
    CHECK_EQ(copy.format(), zerocopy::CameraFrame::kFormatJpeg);
    CHECK_EQ(copy.stream(), zerocopy::CameraFrame::kStreamB);
    CHECK_EQ(copy.size(), src.size());
    CHECK_NE(copy.data(), src.data());
    CHECK(region_matches(src.data(), copy.data(), src.size()));
  }

  TEST_CASE("move constructor transfers ownership") {
    zerocopy::CameraFrame src;

    src.create(300);
    const uint8_t* ptr = src.data();

    zerocopy::CameraFrame moved(std::move(src));

    CHECK(moved.is_owner());
    CHECK_EQ(moved.data(), ptr);
    CHECK_EQ(moved.size(), 300u);
    CHECK_FALSE(src.is_valid());
  }

  TEST_CASE("copy assignment performs deep copy") {
    zerocopy::CameraFrame src;

    src.set_format(zerocopy::CameraFrame::kFormatNv21);
    src.create(120);

    zerocopy::CameraFrame dst;
    dst = src;

    CHECK(dst.is_owner());
    CHECK_EQ(dst.format(), zerocopy::CameraFrame::kFormatNv21);
    CHECK_EQ(dst.size(), 120u);
    CHECK_NE(dst.data(), src.data());
  }

  TEST_CASE("move assignment transfers ownership") {
    zerocopy::CameraFrame src;

    src.create(400);
    const uint8_t* ptr = src.data();

    zerocopy::CameraFrame dst;
    dst = std::move(src);

    CHECK(dst.is_owner());
    CHECK_EQ(dst.data(), ptr);
    CHECK_FALSE(src.is_valid());
  }

  TEST_CASE("serialize and deserialize round-trip preserves all fields") {
    zerocopy::CameraFrame src;

    src.set_width(640u);
    src.set_height(480u);
    src.set_format(zerocopy::CameraFrame::kFormatRgb888Packed);
    src.set_freq(25u);
    src.set_channel(1u);
    src.set_stream(zerocopy::CameraFrame::kStreamI);

    static constexpr size_t kPixelSize = 640u * 480u * 3u;
    REQUIRE(src.create(kPixelSize));

    fill_pattern(const_cast<uint8_t*>(src.data()), kPixelSize, 0xDEu);
    src.header.seq = 7u;
    std::strncpy(src.header.frame_id, "cam_0", sizeof(src.header.frame_id) - 1);
    src.header.frame_id[sizeof(src.header.frame_id) - 1] = '\0';
    src.header.time_meas = 111111u;
    src.header.time_pub = 222222u;

    Bytes wire;
    CHECK((src >> wire));
    CHECK(zerocopy::CameraFrame::check_valid(wire));
    CHECK_EQ(wire.size(), src.get_serialized_size());

    zerocopy::CameraFrame dst;
    CHECK((dst << wire));

    CHECK(dst.is_valid());
    CHECK_FALSE(dst.is_owner());
    CHECK_EQ(dst.width(), 640u);
    CHECK_EQ(dst.height(), 480u);
    CHECK_EQ(dst.format(), zerocopy::CameraFrame::kFormatRgb888Packed);
    CHECK_EQ(dst.freq(), 25u);
    CHECK_EQ(dst.channel(), 1u);
    CHECK_EQ(dst.size(), kPixelSize);
    CHECK_EQ(dst.header.seq, 7u);
    CHECK_EQ(std::string(dst.header.frame_id), "cam_0");
    CHECK_EQ(dst.header.time_meas, 111111u);
    CHECK_EQ(dst.header.time_pub, 222222u);
    CHECK(region_matches(src.data(), dst.data(), kPixelSize));
  }

  TEST_CASE("serialize empty frame produces valid wire buffer") {
    zerocopy::CameraFrame frame;

    Bytes wire;
    CHECK((frame >> wire));
    CHECK(zerocopy::CameraFrame::check_valid(wire));

    zerocopy::CameraFrame frame2;
    CHECK((frame2 << wire));
    CHECK_EQ(frame2.width(), 0u);
    CHECK_EQ(frame2.height(), 0u);
  }

  TEST_CASE("check_valid rejects empty, corrupted begin magic, and corrupted end magic") {
    zerocopy::CameraFrame frame;
    frame.create(128);

    Bytes wire;
    frame >> wire;

    SUBCASE("empty bytes") {
      Bytes empty;
      CHECK_FALSE(zerocopy::CameraFrame::check_valid(empty));
    }

    SUBCASE("corrupted begin magic") {
      wire[0] ^= 0xFFu;
      CHECK_FALSE(zerocopy::CameraFrame::check_valid(wire));
    }

    SUBCASE("corrupted end magic") {
      wire[wire.size() - 1] ^= 0xFFu;
      CHECK_FALSE(zerocopy::CameraFrame::check_valid(wire));
    }
  }

  TEST_CASE("deserialize from too-small buffer returns false") {
    std::vector<uint8_t> raw(4, 0x00u);
    Bytes too_small(raw);

    zerocopy::CameraFrame frame;
    CHECK_FALSE((frame << too_small));
  }

  TEST_CASE("get_reserved is writable and not reset by clear") {
    zerocopy::CameraFrame frame;

    frame.get_reserved() = 0xDEADBEEFu;
    CHECK_EQ(frame.get_reserved(), 0xDEADBEEFu);

    frame.create(64);
    frame.clear();

    CHECK_EQ(frame.get_reserved(), 0xDEADBEEFu);
  }

  TEST_CASE("deserialize rejects a wire payload whose major version differs") {
    zerocopy::CameraFrame src;
    REQUIRE(src.create(64));

    Bytes wire;
    REQUIRE((src >> wire));
    REQUIRE(zerocopy::CameraFrame::check_valid(wire));

    uint32_t foreign_version = (zerocopy::version_major(zerocopy::kWireVersion) + 1U) * 1000000U;
    std::memcpy(wire.data() + sizeof(uint32_t), &foreign_version, sizeof(foreign_version));

    CHECK_FALSE(zerocopy::CameraFrame::check_valid(wire));

    zerocopy::CameraFrame dst;
    CHECK_FALSE((dst << wire));
  }

  TEST_CASE("wire operations reuse output and reject inconsistent embedded size") {
    zerocopy::CameraFrame src;
    src.set_width(8u);
    src.set_height(4u);
    REQUIRE(src.create(32));

    Bytes wire = Bytes::create(src.get_serialized_size());
    uint8_t* original = wire.data();
    CHECK((src >> wire));
    CHECK_EQ(wire.data(), original);

    zerocopy::CameraFrame dst;
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
