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

#include "./zerocopy/tensor.h"

#include <doctest/doctest.h>

#include <cstring>
#include <limits>
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

TEST_SUITE("zerocopy-Tensor") {
  TEST_CASE("default construction yields invalid empty tensor") {
    zerocopy::Tensor t;

    CHECK_FALSE(t.is_valid());
    CHECK_EQ(t.size(), 0u);
    CHECK_FALSE(t.is_owner());
    CHECK_EQ(t.data(), nullptr);
    CHECK_EQ(t.dtype(), zerocopy::Tensor::kDataUnknown);
    CHECK_EQ(t.device(), zerocopy::Tensor::kDeviceCpu);
    CHECK_EQ(t.rank(), 0u);
    CHECK_EQ(t.element_size(), 0u);
    CHECK_EQ(t.num_elements(), 0u);
    CHECK_EQ(t.batch_size(), 0u);
    CHECK_EQ(t.channel(), 0u);
    CHECK_EQ(t.freq(), 0u);
    CHECK_EQ(t.quant_scale(), doctest::Approx(0.0f));
    CHECK_EQ(t.quant_zero_point(), 0);
    CHECK_EQ(t.update_time_ns(), 0u);
    CHECK(t.name().empty());
    CHECK(t.model_id().empty());
    CHECK(t.layout().empty());
  }

  TEST_CASE("sizeof is exactly 248 bytes") { CHECK_EQ(sizeof(zerocopy::Tensor), 248u); }

  TEST_CASE("kMaxRank is 8") { CHECK_EQ(static_cast<uint8_t>(zerocopy::Tensor::kMaxRank), 8u); }

  TEST_CASE("element_size_of returns byte size for each DataType") {
    CHECK_EQ(zerocopy::Tensor::element_size_of(zerocopy::Tensor::kDataUnknown), 0u);
    CHECK_EQ(zerocopy::Tensor::element_size_of(zerocopy::Tensor::kBool), 1u);
    CHECK_EQ(zerocopy::Tensor::element_size_of(zerocopy::Tensor::kInt8), 1u);
    CHECK_EQ(zerocopy::Tensor::element_size_of(zerocopy::Tensor::kUint8), 1u);
    CHECK_EQ(zerocopy::Tensor::element_size_of(zerocopy::Tensor::kInt16), 2u);
    CHECK_EQ(zerocopy::Tensor::element_size_of(zerocopy::Tensor::kUint16), 2u);
    CHECK_EQ(zerocopy::Tensor::element_size_of(zerocopy::Tensor::kFloat16), 2u);
    CHECK_EQ(zerocopy::Tensor::element_size_of(zerocopy::Tensor::kBfloat16), 2u);
    CHECK_EQ(zerocopy::Tensor::element_size_of(zerocopy::Tensor::kInt32), 4u);
    CHECK_EQ(zerocopy::Tensor::element_size_of(zerocopy::Tensor::kUint32), 4u);
    CHECK_EQ(zerocopy::Tensor::element_size_of(zerocopy::Tensor::kFloat32), 4u);
    CHECK_EQ(zerocopy::Tensor::element_size_of(zerocopy::Tensor::kInt64), 8u);
    CHECK_EQ(zerocopy::Tensor::element_size_of(zerocopy::Tensor::kUint64), 8u);
    CHECK_EQ(zerocopy::Tensor::element_size_of(zerocopy::Tensor::kFloat64), 8u);
  }

  TEST_CASE("set_dtype caches element_size") {
    zerocopy::Tensor t;

    t.set_dtype(zerocopy::Tensor::kFloat32);
    CHECK_EQ(t.dtype(), zerocopy::Tensor::kFloat32);
    CHECK_EQ(t.element_size(), 4u);

    t.set_dtype(zerocopy::Tensor::kUint8);
    CHECK_EQ(t.dtype(), zerocopy::Tensor::kUint8);
    CHECK_EQ(t.element_size(), 1u);

    t.set_dtype(zerocopy::Tensor::kFloat64);
    CHECK_EQ(t.element_size(), 8u);

    t.set_dtype(zerocopy::Tensor::kDataUnknown);
    CHECK_EQ(t.element_size(), 0u);
  }

  TEST_CASE("all Device enum values can be set and retrieved") {
    zerocopy::Tensor t;

    t.set_device(zerocopy::Tensor::kDeviceCpu);
    CHECK_EQ(t.device(), zerocopy::Tensor::kDeviceCpu);

    t.set_device(zerocopy::Tensor::kDeviceGpu);
    CHECK_EQ(t.device(), zerocopy::Tensor::kDeviceGpu);

    t.set_device(zerocopy::Tensor::kDeviceNpu);
    CHECK_EQ(t.device(), zerocopy::Tensor::kDeviceNpu);

    t.set_device(zerocopy::Tensor::kDeviceDsp);
    CHECK_EQ(t.device(), zerocopy::Tensor::kDeviceDsp);
  }

  TEST_CASE("set_shape with rank 4 fills shape, computes row-major strides and num_elements") {
    zerocopy::Tensor t;

    uint32_t shape[4] = {1u, 3u, 224u, 224u};
    t.set_shape(shape, 4);

    CHECK_EQ(t.rank(), 4u);
    CHECK_EQ(t.shape_at(0), 1u);
    CHECK_EQ(t.shape_at(1), 3u);
    CHECK_EQ(t.shape_at(2), 224u);
    CHECK_EQ(t.shape_at(3), 224u);
    CHECK_EQ(t.batch_size(), 1u);
    CHECK_EQ(t.num_elements(), 1ull * 3ull * 224ull * 224ull);

    CHECK_EQ(t.stride_at(3), 1u);
    CHECK_EQ(t.stride_at(2), 224u);
    CHECK_EQ(t.stride_at(1), 224u * 224u);
    CHECK_EQ(t.stride_at(0), 3u * 224u * 224u);
  }

  TEST_CASE("set_shape with rank 0 or null pointer zeros shape and num_elements") {
    zerocopy::Tensor t;

    uint32_t shape[2] = {4u, 8u};
    t.set_shape(shape, 2);
    REQUIRE_EQ(t.rank(), 2u);

    SUBCASE("rank 0") {
      t.set_shape(shape, 0);
      CHECK_EQ(t.rank(), 0u);
      CHECK_EQ(t.num_elements(), 0u);
      CHECK_EQ(t.batch_size(), 0u);
    }

    SUBCASE("null pointer") {
      t.set_shape(nullptr, 3);
      CHECK_EQ(t.rank(), 0u);
      CHECK_EQ(t.num_elements(), 0u);
      CHECK_EQ(t.batch_size(), 0u);
    }
  }

  TEST_CASE("set_shape clamps rank to kMaxRank") {
    zerocopy::Tensor t;

    uint32_t shape[16];
    for (int i = 0; i < 16; ++i) {
      shape[i] = 2u;
    }

    t.set_shape(shape, 16);

    CHECK_EQ(t.rank(), zerocopy::Tensor::kMaxRank);
  }

  TEST_CASE("set_shape rejects unrepresentable element counts and strides") {
    zerocopy::Tensor t;

    SUBCASE("uint64 element count overflow") {
      const uint32_t shape[4] = {65536u, 65536u, 65536u, 65536u};
      t.set_shape(shape, 4u);

      CHECK_EQ(t.rank(), 0u);
      CHECK_EQ(t.num_elements(), 0u);
      CHECK_EQ(t.batch_size(), 0u);
      CHECK_EQ(t.shape_at(0u), 0u);
      CHECK_EQ(t.stride_at(0u), 0u);
    }

    SUBCASE("uint32 stride overflow") {
      const uint32_t shape[3] = {2u, std::numeric_limits<uint32_t>::max(), 2u};
      t.set_shape(shape, 3u);

      CHECK_EQ(t.rank(), 0u);
      CHECK_EQ(t.num_elements(), 0u);
      CHECK_EQ(t.batch_size(), 0u);
      CHECK_EQ(t.shape_at(0u), 0u);
      CHECK_EQ(t.stride_at(0u), 0u);
    }
  }

  TEST_CASE("set_shape preserves representable zero-sized dimensions") {
    zerocopy::Tensor t;
    const uint32_t maximum = std::numeric_limits<uint32_t>::max();
    const uint32_t shape[4] = {maximum, maximum, maximum, 0u};
    t.set_shape(shape, 4u);

    CHECK_EQ(t.rank(), 4u);
    CHECK_EQ(t.num_elements(), 0u);
    CHECK_EQ(t.batch_size(), maximum);
    CHECK_EQ(t.shape_at(3u), 0u);
    CHECK_EQ(t.stride_at(0u), 0u);
    CHECK_EQ(t.stride_at(1u), 0u);
    CHECK_EQ(t.stride_at(2u), 0u);
    CHECK_EQ(t.stride_at(3u), 1u);
  }

  TEST_CASE("set_shape_at and set_stride_at modify single dimensions and respect bounds") {
    zerocopy::Tensor t;

    uint32_t shape[2] = {4u, 8u};
    t.set_shape(shape, 2);

    t.set_shape_at(0, 16u);
    CHECK_EQ(t.shape_at(0), 16u);

    t.set_stride_at(1, 99u);
    CHECK_EQ(t.stride_at(1), 99u);

    SUBCASE("out-of-range shape_at returns 0") { CHECK_EQ(t.shape_at(zerocopy::Tensor::kMaxRank), 0u); }

    SUBCASE("out-of-range stride_at returns 0") { CHECK_EQ(t.stride_at(zerocopy::Tensor::kMaxRank + 1u), 0u); }

    SUBCASE("out-of-range set_shape_at is a no-op") {
      t.set_shape_at(zerocopy::Tensor::kMaxRank, 7u);
      CHECK_EQ(t.shape_at(0), 16u);
    }

    SUBCASE("out-of-range set_stride_at is a no-op") {
      t.set_stride_at(zerocopy::Tensor::kMaxRank, 7u);
      CHECK_EQ(t.stride_at(1), 99u);
    }
  }

  TEST_CASE("set_name set_model_id set_layout store strings and truncate oversize") {
    zerocopy::Tensor t;

    t.set_name("image");
    CHECK_EQ(std::string(t.name()), "image");

    t.set_model_id("resnet50");
    CHECK_EQ(std::string(t.model_id()), "resnet50");

    t.set_layout("NCHW");
    CHECK_EQ(std::string(t.layout()), "NCHW");

    SUBCASE("oversize name is truncated") {
      t.set_name("this_name_is_definitely_longer_than_thirty_two_bytes_total");
      CHECK_LE(t.name().size(), 31u);
    }

    SUBCASE("oversize model_id is truncated") {
      t.set_model_id("this_id_is_definitely_longer_than_thirty_two_bytes_total");
      CHECK_LE(t.model_id().size(), 31u);
    }

    SUBCASE("oversize layout is truncated") {
      t.set_layout("this_layout_is_too_long_to_fit");
      CHECK_LE(t.layout().size(), 15u);
    }
  }

  TEST_CASE("scalar accessors round-trip") {
    zerocopy::Tensor t;

    t.set_channel(5u);
    CHECK_EQ(t.channel(), 5u);

    t.set_freq(30u);
    CHECK_EQ(t.freq(), 30u);

    t.set_batch_size(8u);
    CHECK_EQ(t.batch_size(), 8u);

    t.set_quant_scale(0.025f);
    CHECK_EQ(t.quant_scale(), doctest::Approx(0.025f));

    t.set_quant_zero_point(-128);
    CHECK_EQ(t.quant_zero_point(), -128);

    t.set_update_time_ns(987654321ull);
    CHECK_EQ(t.update_time_ns(), 987654321ull);
  }

  TEST_CASE("create succeeds for representative sizes and sets ownership") {
    size_t sz = 0;

    SUBCASE("single byte") { sz = 1; }
    SUBCASE("small") { sz = 64; }
    SUBCASE("1x3x224x224 float32") { sz = 1u * 3u * 224u * 224u * sizeof(float); }

    zerocopy::Tensor t;
    CHECK(t.create(sz));
    CHECK(t.is_valid());
    CHECK(t.is_owner());
    CHECK_EQ(t.size(), sz);
    CHECK_NE(t.data(), nullptr);
  }

  TEST_CASE("create with zero size returns false") {
    zerocopy::Tensor t;

    CHECK_FALSE(t.create(0));
    CHECK_FALSE(t.is_valid());
  }

  TEST_CASE("create replaces previous owned buffer") {
    zerocopy::Tensor t;

    REQUIRE(t.create(100));
    REQUIRE(t.create(200));

    CHECK_EQ(t.size(), 200u);
    CHECK(t.is_owner());
  }

  TEST_CASE("clear resets all fields including shape and strides") {
    zerocopy::Tensor t;

    uint32_t shape[3] = {2u, 4u, 6u};
    t.set_shape(shape, 3);
    t.set_dtype(zerocopy::Tensor::kFloat32);
    t.set_device(zerocopy::Tensor::kDeviceGpu);
    t.set_name("hidden");
    t.set_model_id("mid");
    t.set_layout("BLC");
    t.set_quant_scale(0.5f);
    t.set_quant_zero_point(7);
    t.create(48 * sizeof(float));
    t.header.seq = 11u;

    t.clear();

    CHECK_FALSE(t.is_valid());
    CHECK_FALSE(t.is_owner());
    CHECK_EQ(t.size(), 0u);
    CHECK_EQ(t.data(), nullptr);
    CHECK_EQ(t.rank(), 0u);
    CHECK_EQ(t.num_elements(), 0u);
    CHECK_EQ(t.dtype(), zerocopy::Tensor::kDataUnknown);
    CHECK_EQ(t.device(), zerocopy::Tensor::kDeviceCpu);
    CHECK_EQ(t.element_size(), 0u);
    CHECK_EQ(t.shape_at(0), 0u);
    CHECK_EQ(t.stride_at(0), 0u);
    CHECK(t.name().empty());
    CHECK(t.model_id().empty());
    CHECK(t.layout().empty());
    CHECK_EQ(t.header.seq, 0u);
  }

  TEST_CASE("clear then create again succeeds") {
    zerocopy::Tensor t;

    t.create(100);
    t.clear();
    CHECK_FALSE(t.is_valid());

    CHECK(t.create(50));
    CHECK(t.is_valid());
    CHECK_EQ(t.size(), 50u);
  }

  TEST_CASE("get_reserved3 is writable and not reset by clear") {
    zerocopy::Tensor t;

    t.get_reserved3() = 0xDEADBEEFu;
    CHECK_EQ(t.get_reserved3(), 0xDEADBEEFu);

    t.create(64);
    t.clear();

    CHECK_EQ(t.get_reserved3(), 0xDEADBEEFu);
  }

  TEST_CASE("shallow_copy from Tensor aliases buffer and copies metadata") {
    zerocopy::Tensor src;

    uint32_t shape[2] = {16u, 32u};
    src.set_shape(shape, 2);
    src.set_dtype(zerocopy::Tensor::kFloat16);
    src.set_device(zerocopy::Tensor::kDeviceNpu);
    src.set_name("feat");
    src.set_layout("BL");
    src.create(16u * 32u * 2u);

    zerocopy::Tensor dst;
    CHECK(dst.shallow_copy(src));

    CHECK(dst.is_valid());
    CHECK_FALSE(dst.is_owner());
    CHECK_EQ(dst.data(), src.data());
    CHECK_EQ(dst.size(), src.size());
    CHECK_EQ(dst.rank(), 2u);
    CHECK_EQ(dst.shape_at(0), 16u);
    CHECK_EQ(dst.shape_at(1), 32u);
    CHECK_EQ(dst.dtype(), zerocopy::Tensor::kFloat16);
    CHECK_EQ(dst.element_size(), 2u);
    CHECK_EQ(dst.device(), zerocopy::Tensor::kDeviceNpu);
    CHECK_EQ(std::string(dst.name()), "feat");
    CHECK_EQ(std::string(dst.layout()), "BL");
  }

  TEST_CASE("shallow_copy self returns false") {
    zerocopy::Tensor t;

    t.create(64);
    CHECK_FALSE(t.shallow_copy(t));
  }

  TEST_CASE("shallow_copy from raw pointer aliases the buffer") {
    std::vector<uint8_t> buf(64, 0xABu);

    zerocopy::Tensor t;
    CHECK(t.shallow_copy(buf.data(), buf.size()));

    CHECK(t.is_valid());
    CHECK_FALSE(t.is_owner());
    CHECK_EQ(t.size(), 64u);
    CHECK_EQ(t.data(), buf.data());
  }

  TEST_CASE("shallow_copy from raw pointer rejects null or zero size") {
    zerocopy::Tensor t;

    CHECK_FALSE(t.shallow_copy(nullptr, 64));

    std::vector<uint8_t> buf(8, 0xFFu);
    CHECK_FALSE(t.shallow_copy(buf.data(), 0));
  }

  TEST_CASE("shallow_copy same raw pointer returns false") {
    std::vector<uint8_t> buf(64, 0x55u);

    zerocopy::Tensor t;
    CHECK(t.shallow_copy(buf.data(), buf.size()));
    CHECK_FALSE(t.shallow_copy(buf.data(), buf.size()));
  }

  TEST_CASE("deep_copy from Tensor allocates owned copy") {
    zerocopy::Tensor src;

    uint32_t shape[2] = {4u, 8u};
    src.set_shape(shape, 2);
    src.set_dtype(zerocopy::Tensor::kFloat32);
    src.create(4u * 8u * sizeof(float));
    fill_pattern(const_cast<uint8_t*>(src.data()), src.size(), 0x6Bu);

    zerocopy::Tensor dst;
    CHECK(dst.deep_copy(src));

    CHECK(dst.is_owner());
    CHECK_EQ(dst.size(), src.size());
    CHECK_EQ(dst.rank(), 2u);
    CHECK_EQ(dst.shape_at(0), 4u);
    CHECK_EQ(dst.shape_at(1), 8u);
    CHECK_NE(dst.data(), src.data());
    CHECK(region_matches(src.data(), dst.data(), src.size()));
  }

  TEST_CASE("deep_copy into same-size owned buffer reuses memory") {
    zerocopy::Tensor src;
    src.create(128);
    fill_pattern(const_cast<uint8_t*>(src.data()), 128, 0xAAu);

    zerocopy::Tensor dst;
    dst.create(128);

    CHECK(dst.deep_copy(src));
    CHECK(dst.is_owner());
    CHECK_EQ(dst.size(), 128u);
    CHECK(region_matches(src.data(), dst.data(), 128));
  }

  TEST_CASE("deep_copy self returns false") {
    zerocopy::Tensor t;

    t.create(64);
    CHECK_FALSE(t.deep_copy(t));
  }

  TEST_CASE("deep_copy from raw pointer") {
    static constexpr size_t kN = 256;
    std::vector<uint8_t> src(kN);
    fill_pattern(src.data(), kN, 0x33u);

    zerocopy::Tensor t;
    CHECK(t.deep_copy(src.data(), kN));

    CHECK(t.is_valid());
    CHECK(t.is_owner());
    CHECK_EQ(t.size(), kN);
    CHECK(region_matches(src.data(), t.data(), kN));
  }

  TEST_CASE("deep_copy from raw pointer rejects self buffer and resizes owned storage") {
    zerocopy::Tensor t;
    REQUIRE(t.create(16));
    auto* original = const_cast<uint8_t*>(t.data());
    original[0] = 0xA5u;
    CHECK_FALSE(t.shallow_copy(original + 1, t.size() - 1));
    CHECK_FALSE(t.deep_copy(original + 1, t.size() - 1));
    CHECK_FALSE(t.deep_copy(original, t.size()));
    CHECK(t.is_owner());
    CHECK_EQ(t.data(), original);
    CHECK_EQ(t.data()[0], 0xA5u);

    zerocopy::Tensor borrowed;
    REQUIRE(borrowed.shallow_copy(original, t.size()));
    CHECK_FALSE(t.shallow_copy(borrowed));
    CHECK_FALSE(t.deep_copy(borrowed));

    std::vector<uint8_t> larger(96);
    fill_pattern(larger.data(), larger.size(), 0x61u);
    CHECK(t.deep_copy(larger.data(), larger.size()));
    CHECK(t.is_owner());
    CHECK_EQ(t.size(), larger.size());
    CHECK(region_matches(larger.data(), t.data(), larger.size()));
  }

  TEST_CASE("deep_copy from raw pointer rejects null or zero size") {
    zerocopy::Tensor t;

    CHECK_FALSE(t.deep_copy(nullptr, 64));

    std::vector<uint8_t> buf(8);
    CHECK_FALSE(t.deep_copy(buf.data(), 0));
  }

  TEST_CASE("fill_data is an alias for deep_copy from raw pointer") {
    static constexpr size_t kN = 80;
    std::vector<uint8_t> src(kN, 0x11u);

    zerocopy::Tensor t;
    CHECK(t.fill_data(src.data(), kN));

    CHECK(t.is_owner());
    CHECK(region_matches(src.data(), t.data(), kN));
  }

  TEST_CASE("move_copy transfers ownership and invalidates source") {
    zerocopy::Tensor src;

    uint32_t shape[2] = {4u, 4u};
    src.set_shape(shape, 2);
    src.set_dtype(zerocopy::Tensor::kFloat32);
    src.create(64);
    const uint8_t* ptr = src.data();

    zerocopy::Tensor dst;
    CHECK(dst.move_copy(src));

    CHECK(dst.is_owner());
    CHECK_EQ(dst.data(), ptr);
    CHECK_EQ(dst.rank(), 2u);
    CHECK_EQ(dst.dtype(), zerocopy::Tensor::kFloat32);

    CHECK_FALSE(src.is_valid());
    CHECK_FALSE(src.is_owner());
    CHECK_EQ(src.data(), nullptr);
    CHECK_EQ(src.size(), 0u);
    CHECK_EQ(src.rank(), 0u);
    CHECK_EQ(src.dtype(), zerocopy::Tensor::kDataUnknown);
    CHECK_EQ(src.element_size(), 0u);
  }

  TEST_CASE("move_copy self returns false") {
    zerocopy::Tensor t;

    t.create(32);
    CHECK_FALSE(t.move_copy(t));
  }

  TEST_CASE("copy constructor performs deep copy") {
    zerocopy::Tensor src;

    uint32_t shape[3] = {2u, 4u, 8u};
    src.set_shape(shape, 3);
    src.set_dtype(zerocopy::Tensor::kInt32);
    src.set_name("hidden");
    src.create(2u * 4u * 8u * sizeof(int32_t));
    fill_pattern(const_cast<uint8_t*>(src.data()), src.size(), 0x9Au);

    zerocopy::Tensor copy(src);

    CHECK(copy.is_owner());
    CHECK_EQ(copy.size(), src.size());
    CHECK_EQ(copy.rank(), 3u);
    CHECK_EQ(copy.dtype(), zerocopy::Tensor::kInt32);
    CHECK_EQ(copy.element_size(), 4u);
    CHECK_EQ(std::string(copy.name()), "hidden");
    CHECK_NE(copy.data(), src.data());
    CHECK(region_matches(src.data(), copy.data(), src.size()));
  }

  TEST_CASE("move constructor transfers ownership") {
    zerocopy::Tensor src;

    src.create(48);
    const uint8_t* ptr = src.data();

    zerocopy::Tensor moved(std::move(src));

    CHECK(moved.is_owner());
    CHECK_EQ(moved.size(), 48u);
    CHECK_EQ(moved.data(), ptr);
    CHECK_FALSE(src.is_valid());
  }

  TEST_CASE("copy assignment performs deep copy") {
    zerocopy::Tensor src;

    src.set_dtype(zerocopy::Tensor::kFloat32);
    src.create(80);
    fill_pattern(const_cast<uint8_t*>(src.data()), 80, 0x4Du);

    zerocopy::Tensor dst;
    dst = src;

    CHECK(dst.is_owner());
    CHECK_EQ(dst.size(), 80u);
    CHECK_EQ(dst.dtype(), zerocopy::Tensor::kFloat32);
    CHECK_EQ(dst.element_size(), 4u);
    CHECK(region_matches(src.data(), dst.data(), 80));
  }

  TEST_CASE("move assignment transfers ownership") {
    zerocopy::Tensor src;

    src.create(96);
    const uint8_t* ptr = src.data();

    zerocopy::Tensor dst;
    dst = std::move(src);

    CHECK(dst.is_owner());
    CHECK_EQ(dst.data(), ptr);
    CHECK_FALSE(src.is_valid());
  }

  TEST_CASE("serialize and deserialize round-trip preserves all fields") {
    zerocopy::Tensor src;

    uint32_t shape[4] = {1u, 3u, 8u, 16u};
    src.set_shape(shape, 4);
    src.set_dtype(zerocopy::Tensor::kFloat32);
    src.set_device(zerocopy::Tensor::kDeviceGpu);
    src.set_name("image");
    src.set_model_id("resnet18");
    src.set_layout("NCHW");
    src.set_channel(2u);
    src.set_freq(30u);
    src.set_quant_scale(0.025f);
    src.set_quant_zero_point(-128);
    src.set_update_time_ns(999u);

    static constexpr size_t kPayload = 1u * 3u * 8u * 16u * sizeof(float);
    REQUIRE(src.create(kPayload));

    fill_pattern(const_cast<uint8_t*>(src.data()), kPayload, 0xDEu);

    src.header.seq = 7u;
    std::strncpy(src.header.frame_id, "infer", sizeof(src.header.frame_id) - 1);
    src.header.frame_id[sizeof(src.header.frame_id) - 1] = '\0';
    src.header.time_meas = 111111u;
    src.header.time_pub = 222222u;

    Bytes wire;
    CHECK((src >> wire));
    CHECK(zerocopy::Tensor::check_valid(wire));
    CHECK_EQ(wire.size(), src.get_serialized_size());
    uintptr_t serialized_pointer = 1;
    std::memcpy(&serialized_pointer, wire.data() + 8u + 40u, sizeof(serialized_pointer));
    CHECK_EQ(serialized_pointer, 0u);

    zerocopy::Tensor dst;
    CHECK((dst << wire));

    CHECK(dst.is_valid());
    CHECK_FALSE(dst.is_owner());
    CHECK_EQ(dst.size(), kPayload);
    CHECK_EQ(dst.rank(), 4u);
    CHECK_EQ(dst.shape_at(0), 1u);
    CHECK_EQ(dst.shape_at(1), 3u);
    CHECK_EQ(dst.shape_at(2), 8u);
    CHECK_EQ(dst.shape_at(3), 16u);
    CHECK_EQ(dst.stride_at(3), 1u);
    CHECK_EQ(dst.dtype(), zerocopy::Tensor::kFloat32);
    CHECK_EQ(dst.element_size(), 4u);
    CHECK_EQ(dst.device(), zerocopy::Tensor::kDeviceGpu);
    CHECK_EQ(std::string(dst.name()), "image");
    CHECK_EQ(std::string(dst.model_id()), "resnet18");
    CHECK_EQ(std::string(dst.layout()), "NCHW");
    CHECK_EQ(dst.channel(), 2u);
    CHECK_EQ(dst.freq(), 30u);
    CHECK_EQ(dst.quant_scale(), doctest::Approx(0.025f));
    CHECK_EQ(dst.quant_zero_point(), -128);
    CHECK_EQ(dst.update_time_ns(), 999u);
    CHECK_EQ(dst.header.seq, 7u);
    CHECK_EQ(std::string(dst.header.frame_id), "infer");
    CHECK_EQ(dst.header.time_meas, 111111u);
    CHECK_EQ(dst.header.time_pub, 222222u);
    CHECK(region_matches(src.data(), dst.data(), kPayload));
  }

  TEST_CASE("serialize empty tensor produces valid wire buffer") {
    zerocopy::Tensor t;

    Bytes wire;
    CHECK((t >> wire));
    CHECK(zerocopy::Tensor::check_valid(wire));

    zerocopy::Tensor t2;
    CHECK((t2 << wire));
    CHECK_EQ(t2.rank(), 0u);
    CHECK_EQ(t2.num_elements(), 0u);
  }

  TEST_CASE("deserialize clamps rank to kMaxRank when wire has out-of-range value") {
    zerocopy::Tensor src;

    uint32_t shape[2] = {3u, 5u};
    src.set_shape(shape, 2);
    src.set_dtype(zerocopy::Tensor::kFloat32);
    src.create(64);

    Bytes wire;
    REQUIRE((src >> wire));

    // Corrupt rank_ inside the embedded struct snapshot to a value > kMaxRank.
    // Locate rank_ by mirroring the struct layout: after magic_begin, write a Tensor copy
    // that has rank_ tampered, then re-emit magics.
    zerocopy::Tensor tampered;
    tampered.set_shape(shape, 2);
    tampered.set_dtype(zerocopy::Tensor::kFloat32);
    tampered.create(64);

    Bytes wire2;
    REQUIRE((tampered >> wire2));

    static constexpr size_t kMagicSize = sizeof(uint32_t);
    static constexpr size_t kVersionSize = sizeof(uint32_t);
    // Tensor's wire contract fixes rank_ at byte 237 inside the embedded struct.
    static constexpr size_t kRankOffsetInTensor = 237u;
    const size_t rank_offset = kMagicSize + kVersionSize + kRankOffsetInTensor;

    REQUIRE_EQ(wire2[rank_offset], 2u);
    wire2[rank_offset] = 200u;

    zerocopy::Tensor dst;
    CHECK((dst << wire2));
    CHECK_LE(dst.rank(), zerocopy::Tensor::kMaxRank);
    CHECK_EQ(dst.rank(), zerocopy::Tensor::kMaxRank);
  }

  TEST_CASE("deserialize re-derives element_size from dtype") {
    zerocopy::Tensor src;

    src.set_dtype(zerocopy::Tensor::kFloat64);
    src.create(16);

    Bytes wire;
    REQUIRE((src >> wire));

    zerocopy::Tensor dst;
    CHECK((dst << wire));
    CHECK_EQ(dst.dtype(), zerocopy::Tensor::kFloat64);
    CHECK_EQ(dst.element_size(), 8u);
  }

  TEST_CASE("check_valid rejects empty, corrupted begin magic, and corrupted end magic") {
    zerocopy::Tensor t;
    t.create(128);

    Bytes wire;
    t >> wire;

    SUBCASE("empty bytes") {
      Bytes empty;
      CHECK_FALSE(zerocopy::Tensor::check_valid(empty));
    }

    SUBCASE("corrupted begin magic") {
      wire[0] ^= 0xFFu;
      CHECK_FALSE(zerocopy::Tensor::check_valid(wire));
    }

    SUBCASE("corrupted end magic") {
      wire[wire.size() - 1] ^= 0xFFu;
      CHECK_FALSE(zerocopy::Tensor::check_valid(wire));
    }
  }

  TEST_CASE("deserialize from too-small buffer returns false") {
    std::vector<uint8_t> raw(4, 0x00u);
    Bytes too_small(raw);

    zerocopy::Tensor t;
    CHECK_FALSE((t << too_small));
  }

  TEST_CASE("get_serialized_size equals magic + version + struct + payload + magic") {
    zerocopy::Tensor t;

    SUBCASE("empty") {
      size_t expected = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(zerocopy::Tensor) + 0u + sizeof(uint32_t);
      CHECK_EQ(t.get_serialized_size(), expected);
    }

    SUBCASE("with payload") {
      t.create(256);
      size_t expected = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(zerocopy::Tensor) + 256u + sizeof(uint32_t);
      CHECK_EQ(t.get_serialized_size(), expected);
    }
  }

  TEST_CASE("is_valid returns false when data is null or size is zero") {
    zerocopy::Tensor t;
    CHECK_FALSE(t.is_valid());

    t.create(1);
    CHECK(t.is_valid());

    t.clear();
    CHECK_FALSE(t.is_valid());
  }

  TEST_CASE("deserialize rejects a wire payload whose major version differs") {
    zerocopy::Tensor src;
    REQUIRE(src.create(64));

    Bytes wire;
    REQUIRE((src >> wire));
    REQUIRE(zerocopy::Tensor::check_valid(wire));

    uint32_t foreign_version = (zerocopy::version_major(zerocopy::kWireVersion) + 1U) * 1000000U;
    std::memcpy(wire.data() + sizeof(uint32_t), &foreign_version, sizeof(foreign_version));

    CHECK_FALSE(zerocopy::Tensor::check_valid(wire));

    zerocopy::Tensor dst;
    CHECK_FALSE((dst << wire));
  }

  TEST_CASE("wire operations reuse output and reject inconsistent embedded size") {
    zerocopy::Tensor src;
    src.set_dtype(zerocopy::Tensor::kFloat32);
    REQUIRE(src.create(32));

    Bytes wire = Bytes::create(src.get_serialized_size());
    uint8_t* original = wire.data();
    CHECK((src >> wire));
    CHECK_EQ(wire.data(), original);

    zerocopy::Tensor dst;
    REQUIRE(dst.create(4));
    REQUIRE(dst.is_owner());
    CHECK((dst << wire));
    CHECK_FALSE(dst.is_owner());
    CHECK_EQ(dst.size(), 32u);
    CHECK_EQ(dst.dtype(), zerocopy::Tensor::kFloat32);

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
