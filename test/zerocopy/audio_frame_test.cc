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

#include "./zerocopy/audio_frame.h"

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

TEST_SUITE("zerocopy-AudioFrame") {
  TEST_CASE("default construction yields invalid empty frame") {
    zerocopy::AudioFrame frame;

    CHECK_FALSE(frame.is_valid());
    CHECK_EQ(frame.size(), 0u);
    CHECK_FALSE(frame.is_owner());
    CHECK_EQ(frame.data(), nullptr);
    CHECK_EQ(frame.format(), zerocopy::AudioFrame::kFormatUnknown);
    CHECK_EQ(frame.layout(), zerocopy::AudioFrame::kLayoutUnknown);
    CHECK_EQ(frame.sample_rate(), 0u);
    CHECK_EQ(frame.num_samples(), 0u);
    CHECK_EQ(frame.num_channels(), 0u);
    CHECK_EQ(frame.bit_depth(), 0u);
    CHECK_EQ(frame.bitrate(), 0u);
    CHECK_EQ(frame.channel(), 0u);
    CHECK_EQ(frame.freq(), 0u);
    CHECK_EQ(frame.duration_ns(), 0u);
    CHECK_EQ(frame.update_time_ns(), 0u);
    CHECK(frame.codec().empty());
    CHECK(frame.language().empty());
  }

  TEST_CASE("sizeof is exactly 128 bytes") { CHECK_EQ(sizeof(zerocopy::AudioFrame), 128u); }

  TEST_CASE("all metadata accessors round-trip") {
    zerocopy::AudioFrame frame;

    frame.set_sample_rate(48000u);
    CHECK_EQ(frame.sample_rate(), 48000u);

    frame.set_num_samples(960u);
    CHECK_EQ(frame.num_samples(), 960u);

    frame.set_num_channels(2u);
    CHECK_EQ(frame.num_channels(), 2u);

    frame.set_bit_depth(16u);
    CHECK_EQ(frame.bit_depth(), 16u);

    frame.set_bitrate(128000u);
    CHECK_EQ(frame.bitrate(), 128000u);

    frame.set_channel(3u);
    CHECK_EQ(frame.channel(), 3u);

    frame.set_freq(50u);
    CHECK_EQ(frame.freq(), 50u);

    frame.set_duration_ns(20000000ull);
    CHECK_EQ(frame.duration_ns(), 20000000ull);

    frame.set_update_time_ns(1234567890ull);
    CHECK_EQ(frame.update_time_ns(), 1234567890ull);

    frame.set_format(zerocopy::AudioFrame::kFormatPcmS16);
    CHECK_EQ(frame.format(), zerocopy::AudioFrame::kFormatPcmS16);

    frame.set_layout(zerocopy::AudioFrame::kLayoutInterleaved);
    CHECK_EQ(frame.layout(), zerocopy::AudioFrame::kLayoutInterleaved);
  }

  TEST_CASE("all Format enum values can be set and retrieved") {
    const zerocopy::AudioFrame::Format formats[] = {
        zerocopy::AudioFrame::kFormatPcmS16, zerocopy::AudioFrame::kFormatPcmS24, zerocopy::AudioFrame::kFormatPcmS32,
        zerocopy::AudioFrame::kFormatPcmF32, zerocopy::AudioFrame::kFormatPcmU8,  zerocopy::AudioFrame::kFormatOpus,
        zerocopy::AudioFrame::kFormatAac,    zerocopy::AudioFrame::kFormatMp3,    zerocopy::AudioFrame::kFormatFlac,
    };

    for (auto fmt : formats) {
      zerocopy::AudioFrame f;
      f.set_format(fmt);
      CHECK_EQ(f.format(), fmt);
    }
  }

  TEST_CASE("all Layout enum values can be set and retrieved") {
    zerocopy::AudioFrame f;

    f.set_layout(zerocopy::AudioFrame::kLayoutInterleaved);
    CHECK_EQ(f.layout(), zerocopy::AudioFrame::kLayoutInterleaved);

    f.set_layout(zerocopy::AudioFrame::kLayoutPlanar);
    CHECK_EQ(f.layout(), zerocopy::AudioFrame::kLayoutPlanar);

    f.set_layout(zerocopy::AudioFrame::kLayoutUnknown);
    CHECK_EQ(f.layout(), zerocopy::AudioFrame::kLayoutUnknown);
  }

  TEST_CASE("set_codec stores string and truncates oversize input") {
    zerocopy::AudioFrame f;

    f.set_codec("OPUS");
    CHECK_EQ(std::string(f.codec()), "OPUS");

    SUBCASE("oversize codec is truncated to fit") {
      f.set_codec("this_codec_name_is_definitely_longer_than_sixteen_bytes");
      CHECK_LE(f.codec().size(), 15u);
    }

    SUBCASE("empty input clears codec") {
      f.set_codec("");
      CHECK(f.codec().empty());
    }
  }

  TEST_CASE("set_language stores tag and truncates oversize input") {
    zerocopy::AudioFrame f;

    f.set_language("en");
    CHECK_EQ(std::string(f.language()), "en");

    SUBCASE("oversize language is truncated to fit") {
      f.set_language("very_long_language_tag");
      CHECK_LE(f.language().size(), 7u);
    }

    SUBCASE("empty input clears language") {
      f.set_language("");
      CHECK(f.language().empty());
    }
  }

  TEST_CASE("create succeeds for representative sizes and sets ownership") {
    size_t sz = 0;

    SUBCASE("single byte") { sz = 1; }
    SUBCASE("small") { sz = 64; }
    SUBCASE("opus 20ms 48kHz stereo") { sz = 960u * 2u * sizeof(int16_t); }

    zerocopy::AudioFrame frame;
    CHECK(frame.create(sz));
    CHECK(frame.is_valid());
    CHECK(frame.is_owner());
    CHECK_EQ(frame.size(), sz);
    CHECK_NE(frame.data(), nullptr);
  }

  TEST_CASE("create with zero size returns false") {
    zerocopy::AudioFrame frame;

    CHECK_FALSE(frame.create(0));
    CHECK_FALSE(frame.is_valid());
  }

  TEST_CASE("create replaces previous owned buffer") {
    zerocopy::AudioFrame frame;

    REQUIRE(frame.create(100));
    REQUIRE(frame.create(200));

    CHECK_EQ(frame.size(), 200u);
    CHECK(frame.is_owner());
  }

  TEST_CASE("clear resets all fields") {
    zerocopy::AudioFrame frame;

    frame.set_sample_rate(48000u);
    frame.set_num_channels(2u);
    frame.set_bit_depth(16u);
    frame.set_format(zerocopy::AudioFrame::kFormatPcmS16);
    frame.set_layout(zerocopy::AudioFrame::kLayoutInterleaved);
    frame.set_codec("PCM");
    frame.set_language("en");
    frame.set_duration_ns(20000000ull);
    frame.create(1024);
    frame.header.seq = 99u;

    frame.clear();

    CHECK_FALSE(frame.is_valid());
    CHECK_FALSE(frame.is_owner());
    CHECK_EQ(frame.size(), 0u);
    CHECK_EQ(frame.data(), nullptr);
    CHECK_EQ(frame.sample_rate(), 0u);
    CHECK_EQ(frame.num_channels(), 0u);
    CHECK_EQ(frame.bit_depth(), 0u);
    CHECK_EQ(frame.format(), zerocopy::AudioFrame::kFormatUnknown);
    CHECK_EQ(frame.layout(), zerocopy::AudioFrame::kLayoutUnknown);
    CHECK(frame.codec().empty());
    CHECK(frame.language().empty());
    CHECK_EQ(frame.duration_ns(), 0u);
    CHECK_EQ(frame.header.seq, 0u);
  }

  TEST_CASE("clear then create again succeeds") {
    zerocopy::AudioFrame frame;

    frame.create(100);
    frame.clear();
    CHECK_FALSE(frame.is_valid());

    CHECK(frame.create(50));
    CHECK(frame.is_valid());
    CHECK_EQ(frame.size(), 50u);
  }

  TEST_CASE("get_reserved2 is writable and not reset by clear") {
    zerocopy::AudioFrame frame;

    frame.get_reserved2() = 0xDEADBEEFu;
    CHECK_EQ(frame.get_reserved2(), 0xDEADBEEFu);

    frame.create(64);
    frame.clear();

    CHECK_EQ(frame.get_reserved2(), 0xDEADBEEFu);
  }

  TEST_CASE("shallow_copy from AudioFrame aliases buffer and copies metadata") {
    zerocopy::AudioFrame src;

    src.set_sample_rate(44100u);
    src.set_num_channels(2u);
    src.set_num_samples(1024u);
    src.set_bit_depth(24u);
    src.set_format(zerocopy::AudioFrame::kFormatPcmS24);
    src.set_layout(zerocopy::AudioFrame::kLayoutPlanar);
    src.set_codec("PCM");
    src.set_language("zh");
    src.create(256);

    zerocopy::AudioFrame dst;
    CHECK(dst.shallow_copy(src));

    CHECK(dst.is_valid());
    CHECK_FALSE(dst.is_owner());
    CHECK_EQ(dst.data(), src.data());
    CHECK_EQ(dst.size(), 256u);
    CHECK_EQ(dst.sample_rate(), 44100u);
    CHECK_EQ(dst.num_channels(), 2u);
    CHECK_EQ(dst.num_samples(), 1024u);
    CHECK_EQ(dst.bit_depth(), 24u);
    CHECK_EQ(dst.format(), zerocopy::AudioFrame::kFormatPcmS24);
    CHECK_EQ(dst.layout(), zerocopy::AudioFrame::kLayoutPlanar);
    CHECK_EQ(std::string(dst.codec()), "PCM");
    CHECK_EQ(std::string(dst.language()), "zh");
  }

  TEST_CASE("shallow_copy self returns false") {
    zerocopy::AudioFrame frame;

    frame.create(64);
    CHECK_FALSE(frame.shallow_copy(frame));
  }

  TEST_CASE("shallow_copy from raw pointer aliases the buffer") {
    std::vector<uint8_t> buf(200, 0xCAu);

    zerocopy::AudioFrame frame;
    CHECK(frame.shallow_copy(buf.data(), buf.size()));

    CHECK_FALSE(frame.is_owner());
    CHECK_EQ(frame.size(), 200u);
    CHECK_EQ(frame.data(), buf.data());
  }

  TEST_CASE("shallow_copy from raw pointer rejects null or zero size") {
    zerocopy::AudioFrame frame;

    CHECK_FALSE(frame.shallow_copy(nullptr, 64));

    std::vector<uint8_t> buf(8, 0xFFu);
    CHECK_FALSE(frame.shallow_copy(buf.data(), 0));
  }

  TEST_CASE("shallow_copy same raw pointer returns false") {
    std::vector<uint8_t> buf(64, 0x55u);

    zerocopy::AudioFrame frame;
    CHECK(frame.shallow_copy(buf.data(), buf.size()));
    CHECK_FALSE(frame.shallow_copy(buf.data(), buf.size()));
  }

  TEST_CASE("deep_copy from AudioFrame allocates owned copy") {
    zerocopy::AudioFrame src;

    src.set_sample_rate(48000u);
    src.set_num_channels(2u);
    src.set_format(zerocopy::AudioFrame::kFormatPcmS16);
    src.create(960u * 2u * sizeof(int16_t));
    fill_pattern(const_cast<uint8_t*>(src.data()), src.size(), 0x6Bu);

    zerocopy::AudioFrame dst;
    CHECK(dst.deep_copy(src));

    CHECK(dst.is_owner());
    CHECK_EQ(dst.size(), src.size());
    CHECK_EQ(dst.sample_rate(), 48000u);
    CHECK_EQ(dst.num_channels(), 2u);
    CHECK_NE(dst.data(), src.data());
    CHECK(region_matches(src.data(), dst.data(), src.size()));
  }

  TEST_CASE("deep_copy into same-size owned buffer reuses memory") {
    zerocopy::AudioFrame src;
    src.create(128);
    fill_pattern(const_cast<uint8_t*>(src.data()), 128, 0xAAu);

    zerocopy::AudioFrame dst;
    dst.create(128);

    CHECK(dst.deep_copy(src));
    CHECK(dst.is_owner());
    CHECK_EQ(dst.size(), 128u);
    CHECK(region_matches(src.data(), dst.data(), 128));
  }

  TEST_CASE("deep_copy self returns false") {
    zerocopy::AudioFrame frame;

    frame.create(64);
    CHECK_FALSE(frame.deep_copy(frame));
  }

  TEST_CASE("deep_copy from raw pointer") {
    static constexpr size_t kN = 150;
    std::vector<uint8_t> src(kN);
    fill_pattern(src.data(), kN, 0xE7u);

    zerocopy::AudioFrame frame;
    CHECK(frame.deep_copy(src.data(), kN));

    CHECK(frame.is_owner());
    CHECK_EQ(frame.size(), kN);
    CHECK(region_matches(src.data(), frame.data(), kN));
  }

  TEST_CASE("deep_copy from raw pointer rejects null or zero size") {
    zerocopy::AudioFrame frame;

    CHECK_FALSE(frame.deep_copy(nullptr, 64));

    std::vector<uint8_t> buf(8);
    CHECK_FALSE(frame.deep_copy(buf.data(), 0));
  }

  TEST_CASE("fill_data is an alias for deep_copy from raw pointer") {
    static constexpr size_t kN = 80;
    std::vector<uint8_t> src(kN, 0x11u);

    zerocopy::AudioFrame frame;
    CHECK(frame.fill_data(src.data(), kN));

    CHECK(frame.is_owner());
    CHECK(region_matches(src.data(), frame.data(), kN));
  }

  TEST_CASE("move_copy transfers ownership and invalidates source") {
    zerocopy::AudioFrame src;

    src.set_sample_rate(16000u);
    src.set_num_channels(1u);
    src.set_format(zerocopy::AudioFrame::kFormatPcmS16);
    src.create(16000u * 2u);
    const uint8_t* ptr = src.data();

    zerocopy::AudioFrame dst;
    CHECK(dst.move_copy(src));

    CHECK(dst.is_owner());
    CHECK_EQ(dst.data(), ptr);
    CHECK_EQ(dst.sample_rate(), 16000u);

    CHECK_FALSE(src.is_valid());
    CHECK_FALSE(src.is_owner());
    CHECK_EQ(src.data(), nullptr);
    CHECK_EQ(src.size(), 0u);
    CHECK_EQ(src.sample_rate(), 0u);
    CHECK_EQ(src.format(), zerocopy::AudioFrame::kFormatUnknown);
    CHECK_EQ(src.layout(), zerocopy::AudioFrame::kLayoutUnknown);
  }

  TEST_CASE("move_copy self returns false") {
    zerocopy::AudioFrame frame;

    frame.create(64);
    CHECK_FALSE(frame.move_copy(frame));
  }

  TEST_CASE("copy constructor performs deep copy") {
    zerocopy::AudioFrame src;

    src.set_sample_rate(48000u);
    src.set_num_channels(2u);
    src.set_format(zerocopy::AudioFrame::kFormatOpus);
    src.set_codec("OPUS");
    src.set_language("en");
    src.create(640);
    fill_pattern(const_cast<uint8_t*>(src.data()), src.size(), 0xBCu);

    zerocopy::AudioFrame copy(src);

    CHECK(copy.is_owner());
    CHECK_EQ(copy.sample_rate(), 48000u);
    CHECK_EQ(copy.num_channels(), 2u);
    CHECK_EQ(copy.format(), zerocopy::AudioFrame::kFormatOpus);
    CHECK_EQ(std::string(copy.codec()), "OPUS");
    CHECK_EQ(std::string(copy.language()), "en");
    CHECK_EQ(copy.size(), src.size());
    CHECK_NE(copy.data(), src.data());
    CHECK(region_matches(src.data(), copy.data(), src.size()));
  }

  TEST_CASE("move constructor transfers ownership") {
    zerocopy::AudioFrame src;

    src.create(300);
    const uint8_t* ptr = src.data();

    zerocopy::AudioFrame moved(std::move(src));

    CHECK(moved.is_owner());
    CHECK_EQ(moved.data(), ptr);
    CHECK_EQ(moved.size(), 300u);
    CHECK_FALSE(src.is_valid());
  }

  TEST_CASE("copy assignment performs deep copy") {
    zerocopy::AudioFrame src;

    src.set_format(zerocopy::AudioFrame::kFormatAac);
    src.create(120);

    zerocopy::AudioFrame dst;
    dst = src;

    CHECK(dst.is_owner());
    CHECK_EQ(dst.format(), zerocopy::AudioFrame::kFormatAac);
    CHECK_EQ(dst.size(), 120u);
    CHECK_NE(dst.data(), src.data());
  }

  TEST_CASE("move assignment transfers ownership") {
    zerocopy::AudioFrame src;

    src.create(400);
    const uint8_t* ptr = src.data();

    zerocopy::AudioFrame dst;
    dst = std::move(src);

    CHECK(dst.is_owner());
    CHECK_EQ(dst.data(), ptr);
    CHECK_FALSE(src.is_valid());
  }

  TEST_CASE("serialize and deserialize round-trip preserves all fields") {
    zerocopy::AudioFrame src;

    src.set_sample_rate(48000u);
    src.set_num_samples(960u);
    src.set_num_channels(2u);
    src.set_bit_depth(16u);
    src.set_bitrate(64000u);
    src.set_channel(3u);
    src.set_freq(50u);
    src.set_format(zerocopy::AudioFrame::kFormatPcmS16);
    src.set_layout(zerocopy::AudioFrame::kLayoutInterleaved);
    src.set_codec("PCM");
    src.set_language("zh");
    src.set_duration_ns(20000000ull);
    src.set_update_time_ns(987654321ull);

    static constexpr size_t kPayload = 960u * 2u * sizeof(int16_t);
    REQUIRE(src.create(kPayload));

    fill_pattern(const_cast<uint8_t*>(src.data()), kPayload, 0xDEu);

    src.header.seq = 7u;
    std::strncpy(src.header.frame_id, "mic_0", sizeof(src.header.frame_id) - 1);
    src.header.frame_id[sizeof(src.header.frame_id) - 1] = '\0';
    src.header.time_meas = 111111u;
    src.header.time_pub = 222222u;

    Bytes wire;
    CHECK((src >> wire));
    CHECK(zerocopy::AudioFrame::check_valid(wire));
    CHECK_EQ(wire.size(), src.get_serialized_size());

    zerocopy::AudioFrame dst;
    CHECK((dst << wire));

    CHECK(dst.is_valid());
    CHECK_FALSE(dst.is_owner());
    CHECK_EQ(dst.size(), kPayload);
    CHECK_EQ(dst.sample_rate(), 48000u);
    CHECK_EQ(dst.num_samples(), 960u);
    CHECK_EQ(dst.num_channels(), 2u);
    CHECK_EQ(dst.bit_depth(), 16u);
    CHECK_EQ(dst.bitrate(), 64000u);
    CHECK_EQ(dst.channel(), 3u);
    CHECK_EQ(dst.freq(), 50u);
    CHECK_EQ(dst.format(), zerocopy::AudioFrame::kFormatPcmS16);
    CHECK_EQ(dst.layout(), zerocopy::AudioFrame::kLayoutInterleaved);
    CHECK_EQ(std::string(dst.codec()), "PCM");
    CHECK_EQ(std::string(dst.language()), "zh");
    CHECK_EQ(dst.duration_ns(), 20000000ull);
    CHECK_EQ(dst.update_time_ns(), 987654321ull);
    CHECK_EQ(dst.header.seq, 7u);
    CHECK_EQ(std::string(dst.header.frame_id), "mic_0");
    CHECK_EQ(dst.header.time_meas, 111111u);
    CHECK_EQ(dst.header.time_pub, 222222u);
    CHECK(region_matches(src.data(), dst.data(), kPayload));
  }

  TEST_CASE("serialize empty frame produces valid wire buffer") {
    zerocopy::AudioFrame frame;

    Bytes wire;
    CHECK((frame >> wire));
    CHECK(zerocopy::AudioFrame::check_valid(wire));

    zerocopy::AudioFrame frame2;
    CHECK((frame2 << wire));
    CHECK_EQ(frame2.sample_rate(), 0u);
    CHECK_EQ(frame2.num_channels(), 0u);
  }

  TEST_CASE("check_valid rejects empty, corrupted begin magic, and corrupted end magic") {
    zerocopy::AudioFrame frame;
    frame.create(128);

    Bytes wire;
    frame >> wire;

    SUBCASE("empty bytes") {
      Bytes empty;
      CHECK_FALSE(zerocopy::AudioFrame::check_valid(empty));
    }

    SUBCASE("corrupted begin magic") {
      wire[0] ^= 0xFFu;
      CHECK_FALSE(zerocopy::AudioFrame::check_valid(wire));
    }

    SUBCASE("corrupted end magic") {
      wire[wire.size() - 1] ^= 0xFFu;
      CHECK_FALSE(zerocopy::AudioFrame::check_valid(wire));
    }
  }

  TEST_CASE("deserialize from too-small buffer returns false") {
    std::vector<uint8_t> raw(4, 0x00u);
    Bytes too_small(raw);

    zerocopy::AudioFrame frame;
    CHECK_FALSE((frame << too_small));
  }

  TEST_CASE("get_serialized_size equals magic + version + struct + payload + magic") {
    zerocopy::AudioFrame frame;

    SUBCASE("empty") {
      size_t expected = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(zerocopy::AudioFrame) + 0u + sizeof(uint32_t);
      CHECK_EQ(frame.get_serialized_size(), expected);
    }

    SUBCASE("with payload") {
      frame.create(1000);
      size_t expected = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(zerocopy::AudioFrame) + 1000u + sizeof(uint32_t);
      CHECK_EQ(frame.get_serialized_size(), expected);
    }
  }

  TEST_CASE("is_valid returns false when data is null or size is zero") {
    zerocopy::AudioFrame frame;
    CHECK_FALSE(frame.is_valid());

    frame.create(1);
    CHECK(frame.is_valid());

    frame.clear();
    CHECK_FALSE(frame.is_valid());
  }

  TEST_CASE("deserialize rejects a wire payload whose major version differs") {
    zerocopy::AudioFrame src;
    REQUIRE(src.create(64));

    Bytes wire;
    REQUIRE((src >> wire));
    REQUIRE(zerocopy::AudioFrame::check_valid(wire));

    uint32_t foreign_version = (zerocopy::version_major(zerocopy::kWireVersion) + 1U) * 1000000U;
    std::memcpy(wire.data() + sizeof(uint32_t), &foreign_version, sizeof(foreign_version));

    CHECK_FALSE(zerocopy::AudioFrame::check_valid(wire));

    zerocopy::AudioFrame dst;
    CHECK_FALSE((dst << wire));
  }
}

// NOLINTEND
