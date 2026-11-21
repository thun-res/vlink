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

#include "./base/quantize.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <limits>

#include "../common_test.h"

TEST_SUITE("base-Quantize") {
  TEST_CASE("int16 symmetric range keeps zero centered") {
    CHECK_EQ(Quantize::encode<int16_t>(-10, 10, 0), 0);
    CHECK_EQ(Quantize::encode<int16_t>(-10, 10, -10), -32767);
    CHECK_EQ(Quantize::encode<int16_t>(-10, 10, 10), 32767);

    CHECK_EQ(Quantize::decode<double>(-10, 10, static_cast<int16_t>(0)), doctest::Approx(0.0));
    CHECK_EQ(Quantize::decode<double>(-10, 10, static_cast<int16_t>(32767)), doctest::Approx(10.0));
    CHECK_EQ(Quantize::decode<double>(-10, 10, static_cast<int16_t>(-32767)), doctest::Approx(-10.0));
  }

  TEST_CASE("int16 round-trip matches point cloud extent semantics") {
    constexpr int kExtent = 10;

    auto stored = Quantize::encode<int16_t>(-kExtent, kExtent, 1.25F);
    CHECK_EQ(stored, 4096);

    auto value = Quantize::decode<float>(-kExtent, kExtent, stored);
    CHECK_EQ(value, doctest::Approx(1.25F).epsilon(0.0001));
  }

  TEST_CASE("extent overload matches symmetric int16 encode") {
    constexpr int kExtent = 100;
    auto nan = std::numeric_limits<float>::quiet_NaN();

    CHECK_EQ(Quantize::encode<int16_t>(kExtent, -150.0F), Quantize::encode<int16_t>(-kExtent, kExtent, -150.0F));
    CHECK_EQ(Quantize::encode<int16_t>(kExtent, -100.0F), Quantize::encode<int16_t>(-kExtent, kExtent, -100.0F));
    CHECK_EQ(Quantize::encode<int16_t>(kExtent, -1.25F), Quantize::encode<int16_t>(-kExtent, kExtent, -1.25F));
    CHECK_EQ(Quantize::encode<int16_t>(kExtent, 0.0F), Quantize::encode<int16_t>(-kExtent, kExtent, 0.0F));
    CHECK_EQ(Quantize::encode<int16_t>(kExtent, 1.25F), Quantize::encode<int16_t>(-kExtent, kExtent, 1.25F));
    CHECK_EQ(Quantize::encode<int16_t>(kExtent, 100.0F), Quantize::encode<int16_t>(-kExtent, kExtent, 100.0F));
    CHECK_EQ(Quantize::encode<int16_t>(kExtent, 150.0F), Quantize::encode<int16_t>(-kExtent, kExtent, 150.0F));
    CHECK_EQ(Quantize::encode<int16_t>(kExtent, -100.0001F), Quantize::encode<int16_t>(-kExtent, kExtent, -100.0001F));
    CHECK_EQ(Quantize::encode<int16_t>(kExtent, 100.0001F), Quantize::encode<int16_t>(-kExtent, kExtent, 100.0001F));
    CHECK_EQ(Quantize::encode<int16_t>(kExtent, nan), 0);
    CHECK_EQ(Quantize::encode<int16_t>(0, 1.0F), 0);
  }

  TEST_CASE("extent overload matches symmetric int16 decode") {
    constexpr int kExtent = 100;

    CHECK_EQ(Quantize::decode<float>(kExtent, std::numeric_limits<int16_t>::lowest()),
             doctest::Approx(Quantize::decode<float>(-kExtent, kExtent, std::numeric_limits<int16_t>::lowest())));
    CHECK_EQ(Quantize::decode<float>(kExtent, static_cast<int16_t>(-32767)),
             doctest::Approx(Quantize::decode<float>(-kExtent, kExtent, static_cast<int16_t>(-32767))));
    CHECK_EQ(Quantize::decode<float>(kExtent, static_cast<int16_t>(0)),
             doctest::Approx(Quantize::decode<float>(-kExtent, kExtent, static_cast<int16_t>(0))));
    CHECK_EQ(Quantize::decode<float>(kExtent, static_cast<int16_t>(32767)),
             doctest::Approx(Quantize::decode<float>(-kExtent, kExtent, static_cast<int16_t>(32767))));
    CHECK_EQ(Quantize::decode<float>(0, static_cast<int16_t>(1)), doctest::Approx(0.0F));
  }

  TEST_CASE("encode saturates outside the requested real range") {
    CHECK_EQ(Quantize::encode<int16_t>(-10, 10, 100), std::numeric_limits<int16_t>::max());
    CHECK_EQ(Quantize::encode<int16_t>(-10, 10, -100), std::numeric_limits<int16_t>::lowest());
  }

  TEST_CASE("narrow integer ranges retain their endpoints and interior values") {
    constexpr int kMin = 16777216;
    CHECK_EQ(Quantize::encode<int32_t>(kMin, kMin + 1, kMin), -std::numeric_limits<int32_t>::max());
    CHECK_EQ(Quantize::encode<int32_t>(kMin, kMin + 1, kMin + 1), std::numeric_limits<int32_t>::max());
    CHECK_EQ(Quantize::encode<int32_t>(kMin + 1, kMin, kMin), 0);
    CHECK_EQ(Quantize::decode<int32_t>(kMin, kMin + 1, uint8_t{0}), kMin);
    CHECK_EQ(Quantize::decode<int32_t>(kMin, kMin + 1, uint8_t{255}), kMin + 1);
    CHECK_EQ(Quantize::decode<int32_t>(kMin + 1, kMin, uint8_t{255}), 0);

    constexpr auto kMax = std::numeric_limits<uint64_t>::max();
    CHECK_EQ(Quantize::encode<uint8_t>(kMax - 4, kMax, kMax - 2), 128);
    CHECK_EQ(Quantize::encode<int16_t>(kMax - 4, kMax, -1), std::numeric_limits<int16_t>::lowest());
    CHECK_EQ(Quantize::decode<uint64_t>(kMax - 4, kMax, uint8_t{128}), kMax - 2);
    CHECK_EQ(Quantize::decode<uint64_t>(kMax - 4, kMax, uint8_t{255}), kMax);
    CHECK_EQ(Quantize::decode<int64_t>(kMax - 4, kMax, uint8_t{128}), std::numeric_limits<int64_t>::max());

    constexpr auto kSignedMax = std::numeric_limits<int64_t>::max();
    CHECK_EQ(Quantize::encode<uint8_t>(kSignedMax - 3, 0x1p63, kSignedMax - 1), 128);
    CHECK_EQ(Quantize::encode<uint8_t>(-0x1p63, -kSignedMax + 3, -kSignedMax + 1), 128);
    CHECK_EQ(Quantize::decode<int64_t>(kSignedMax - 3, 0x1p63, uint8_t{128}), kSignedMax - 1);
    CHECK_EQ(Quantize::decode<int64_t>(-0x1p63, -kSignedMax + 3, uint8_t{128}), -kSignedMax + 2);
  }

  TEST_CASE("wide signed storage reserves its lowest code for negative saturation") {
    CHECK_EQ(Quantize::encode<int32_t>(-1.0F, 1.0F, -1.0F), -std::numeric_limits<int32_t>::max());
    CHECK_EQ(Quantize::encode<int32_t>(1.0F, -1.0F), -std::numeric_limits<int32_t>::max());
    CHECK_EQ(Quantize::encode<int32_t>(-1.0F, 1.0F, -2.0F), std::numeric_limits<int32_t>::lowest());
    CHECK_EQ(Quantize::encode<int32_t>(1.0F, -2.0F), std::numeric_limits<int32_t>::lowest());

    constexpr int kExtent = 16777216;
    CHECK_EQ(Quantize::encode<int32_t>(kExtent, -kExtent), -std::numeric_limits<int32_t>::max());
    CHECK_EQ(Quantize::encode<int32_t>(kExtent, -kExtent - 1), std::numeric_limits<int32_t>::lowest());
    CHECK_EQ(Quantize::encode<int32_t>(-kExtent, kExtent, -kExtent - 1), std::numeric_limits<int32_t>::lowest());

    constexpr auto kMax = std::numeric_limits<int64_t>::max();
    CHECK_EQ(Quantize::encode<int64_t>(static_cast<uint64_t>(kMax), -kMax), -kMax);
    CHECK_EQ(Quantize::encode<int64_t>(static_cast<uint64_t>(kMax), -kMax - 1), -kMax - 1);
    CHECK_EQ(Quantize::encode<int64_t>(-kMax, kMax, -kMax - 1), -kMax - 1);

    constexpr int64_t kWideExtent = int64_t{1} << 53;
    CHECK_EQ(Quantize::encode<int64_t>(0x1p53, -kWideExtent - 1), -kMax - 1);
    CHECK_EQ(Quantize::encode<int64_t>(-0x1p53, 0x1p53, -kWideExtent - 1), -kMax - 1);
    CHECK_EQ(Quantize::encode<int64_t>(static_cast<uint64_t>(kMax), -0x1p63), -kMax - 1);
    CHECK_EQ(Quantize::encode<int64_t>(std::numeric_limits<uint64_t>::max(), -0x1p64), -kMax - 1);
    CHECK_EQ(Quantize::encode<int64_t>(-kMax, kMax, -0x1p63), -kMax - 1);
    CHECK_EQ(Quantize::encode<int64_t>(false, true, false), -kMax);
  }

  TEST_CASE("decode clamps raw storage saturation values") {
    CHECK_EQ(Quantize::decode<double>(-10, 10, std::numeric_limits<int16_t>::max()), doctest::Approx(10.0));

    auto saturated_min = Quantize::decode<double>(-10, 10, std::numeric_limits<int16_t>::lowest());
    CHECK(saturated_min < -10.0);
    CHECK(saturated_min > -10.001);
  }

  TEST_CASE("uint8 mapping uses the full unsigned storage range") {
    CHECK_EQ(Quantize::encode<uint8_t>(0, 1, 0.0F), 0);
    CHECK_EQ(Quantize::encode<uint8_t>(0, 1, 1.0F), 255);
    CHECK_EQ(Quantize::encode<uint8_t>(0, 1, 0.5F), 128);

    CHECK_EQ(Quantize::decode<float>(0, 1, static_cast<uint8_t>(0)), doctest::Approx(0.0F));
    CHECK_EQ(Quantize::decode<float>(0, 1, static_cast<uint8_t>(255)), doctest::Approx(1.0F));
    CHECK_EQ(Quantize::decode<float>(0, 1, static_cast<uint8_t>(128)), doctest::Approx(128.0F / 255.0F));
  }

  TEST_CASE("invalid ranges and NaN inputs return zero") {
    CHECK_EQ(Quantize::encode<int16_t>(10, 10, 1.0F), 0);
    CHECK_EQ(Quantize::encode<int16_t>(10, -10, 1.0F), 0);
    CHECK_EQ(Quantize::decode<float>(10, 10, static_cast<int16_t>(1)), doctest::Approx(0.0F));

    auto nan = std::numeric_limits<float>::quiet_NaN();
    CHECK_EQ(Quantize::encode<int16_t>(-10, 10, nan), 0);
    CHECK_EQ(Quantize::decode<float>(nan, 10, static_cast<int16_t>(1)), doctest::Approx(0.0F));
  }

  TEST_CASE("non-finite ranges are rejected without changing infinite value saturation") {
    const auto inf = std::numeric_limits<double>::infinity();

    CHECK_EQ(Quantize::encode<int16_t>(-inf, 10.0, 0.0), 0);
    CHECK_EQ(Quantize::encode<int16_t>(-10.0, inf, 0.0), 0);
    CHECK_EQ(Quantize::encode<int16_t>(inf, 0.0), 0);
    CHECK_EQ(Quantize::decode<double>(-inf, 10.0, static_cast<int16_t>(1)), doctest::Approx(0.0));
    CHECK_EQ(Quantize::decode<double>(inf, static_cast<int16_t>(1)), doctest::Approx(0.0));

    CHECK_EQ(Quantize::encode<int16_t>(-10.0, 10.0, inf), std::numeric_limits<int16_t>::max());
    CHECK_EQ(Quantize::encode<int16_t>(-10.0, 10.0, -inf), std::numeric_limits<int16_t>::lowest());
  }

  TEST_CASE("extreme finite arithmetic preserves finite mappings") {
    const auto max = std::numeric_limits<double>::max();

    CHECK_EQ(Quantize::encode<int16_t>(-max, max, 0.0), 0);
    CHECK_EQ(Quantize::encode<uint8_t>(-max, max, 0.0), 128);
    CHECK_EQ(Quantize::encode<uint8_t>(-max, max / 2.0, -max + max / 256.0), 1);
    CHECK_EQ(Quantize::decode<double>(-max, max, static_cast<int16_t>(0)), doctest::Approx(0.0));
    CHECK_EQ(Quantize::encode<int16_t>(max, max / 2.0), 16384);
    CHECK_EQ(Quantize::decode<double>(max, static_cast<int16_t>(16384)), doctest::Approx(max / 2.0).epsilon(0.0001));
    CHECK_EQ(Quantize::decode<int>(-max, max, std::numeric_limits<int16_t>::max()), std::numeric_limits<int>::max());
    CHECK_EQ(Quantize::decode<int>(max, std::numeric_limits<int16_t>::max()), std::numeric_limits<int>::max());
  }
}

// NOLINTEND
