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

#include <doctest/doctest.h>
#include <vlink/base/helpers.h>
#include <vlink/zerocopy/message_parser.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include "../common_test.h"

TEST_SUITE("zerocopy-MessageParser") {
  TEST_CASE("type detection requires an exact or delimited suffix") {
    using Type = zerocopy::MessageParser::Type;

    CHECK_EQ(zerocopy::MessageParser::detect_type("RawData"), Type::kRawData);
    CHECK_EQ(zerocopy::MessageParser::detect_type("vlink::zerocopy::Tensor"), Type::kTensor);
    CHECK_EQ(zerocopy::MessageParser::detect_type("schema/ObjectArray"), Type::kObjectArray);
    CHECK_EQ(zerocopy::MessageParser::detect_type("NotRawData"), Type::kUnknown);
    CHECK_EQ(zerocopy::MessageParser::detect_type("RawDataExtra"), Type::kUnknown);
  }

  TEST_CASE("raw and proxy fields preserve strings, bytes, and integer precision") {
    zerocopy::RawData source;
    REQUIRE(source.create(3));
    source.header.seq = std::numeric_limits<uint32_t>::max();
    uint8_t payload[] = {1, 2, 3};
    REQUIRE(source.fill_data(payload, sizeof(payload)));

    Bytes wire;
    const bool serialized = source >> wire;
    REQUIRE(serialized);

    zerocopy::MessageParser parser;
    REQUIRE(parser.parse("vlink::zerocopy::RawData", wire));
    CHECK_EQ(parser.type(), zerocopy::MessageParser::Type::kRawData);

    zerocopy::MessageParser::Value value;
    REQUIRE(parser.value("header.seq", value));
    REQUIRE(std::holds_alternative<uint64_t>(value));
    CHECK_EQ(std::get<uint64_t>(value), std::numeric_limits<uint32_t>::max());

    double numeric = 0;
    bool precision_loss = false;
    REQUIRE(parser.numeric("header.seq", numeric, &precision_loss));
    CHECK_FALSE(precision_loss);

    REQUIRE(parser.value("data", value));
    REQUIRE(std::holds_alternative<Bytes>(value));
    CHECK_EQ(std::get<Bytes>(value).size(), 3U);

    zerocopy::ProxyData proxy;
    proxy.create(Bytes::deep_copy(source.data(), source.size()), "intra://source", "RawData");
    const bool proxy_serialized = proxy >> wire;
    REQUIRE(proxy_serialized);
    REQUIRE(parser.parse(zerocopy::MessageParser::Type::kProxyData, wire));

    std::string text;
    REQUIRE(parser.text("url", text));
    CHECK_EQ(text, "intra://source");
    REQUIRE(parser.text("ser", text));
    CHECK_EQ(text, "RawData");
  }

  TEST_CASE("camera and audio parse through the same typed store") {
    zerocopy::CameraFrame camera;
    REQUIRE(camera.create(4));
    camera.set_width(2);
    camera.set_height(2);

    Bytes wire;
    const bool camera_serialized = camera >> wire;
    REQUIRE(camera_serialized);

    zerocopy::MessageParser parser;
    REQUIRE(parser.parse(zerocopy::MessageParser::Type::kCameraFrame, wire));

    const auto camera_fields = parser.fields();
    const auto format_field = std::find_if(camera_fields.begin(), camera_fields.end(),
                                           [](const auto& field) { return field.name == "format"; });
    REQUIRE(format_field != camera_fields.end());
    CHECK_EQ(format_field->enum_kind, zerocopy::MessageParser::EnumKind::kEnumCameraFormat);

    double numeric = 0;
    REQUIRE(parser.numeric("width", numeric));
    CHECK_EQ(numeric, 2);

    zerocopy::MessageFormatOptions format_options;
    format_options.enum_name = true;
    const std::string formatted = zerocopy::format_message(parser, format_options);
    CHECK(formatted.find("header {\n") != std::string::npos);
    CHECK(formatted.find("width: 2\n") != std::string::npos);
    CHECK(formatted.find("format: ") != std::string::npos);
    CHECK(formatted.find("reserved") == std::string::npos);

    zerocopy::AudioFrame audio;
    REQUIRE(audio.create(4));
    audio.set_language("en");
    const bool audio_serialized = audio >> wire;
    REQUIRE(audio_serialized);
    REQUIRE(parser.parse("AudioFrame", wire));

    std::string text;
    REQUIRE(parser.text("language", text));
    CHECK_EQ(text, "en");

    Bytes invalid;
    CHECK_FALSE(parser.parse("AudioFrame", invalid));
    CHECK_FALSE(parser.valid());
  }

  TEST_CASE("point and object collections expose dynamic and indexed fields") {
    zerocopy::PointCloud cloud;
    REQUIRE(cloud.create_v3f<float>(1, {"intensity"}));
    REQUIRE(cloud.push_value_v3f(1.0F, 2.0F, 3.0F, 0.75F));

    Bytes wire;
    const bool cloud_serialized = cloud >> wire;
    REQUIRE(cloud_serialized);

    zerocopy::MessageParser parser;
    REQUIRE(parser.parse(zerocopy::MessageParser::Type::kPointCloud, wire));
    CHECK_EQ(parser.collection_size("points"), 1U);

    double numeric = 0;
    REQUIRE(parser.numeric("data", 0, "x", numeric));
    CHECK_EQ(numeric, doctest::Approx(1.0));
    REQUIRE(parser.numeric("data[0].intensity", numeric));
    CHECK_EQ(numeric, doctest::Approx(0.75));
    CHECK_FALSE(parser.numeric("data[1].x", numeric));
    CHECK_FALSE(parser.numeric("data[0]x", numeric));

    zerocopy::ObjectArray objects;
    REQUIRE(objects.create(1));
    zerocopy::ObjectArray::Object object;
    std::strncpy(object.label, "vehicle", sizeof(object.label) - 1);
    object.position[0] = 4.0F;
    object.yaw = 0.5F;
    object.track_id = 42;
    object.reserved_buf = 9;
    REQUIRE(objects.push_value(object));
    const bool objects_serialized = objects >> wire;
    REQUIRE(objects_serialized);
    REQUIRE(parser.parse(zerocopy::MessageParser::Type::kObjectArray, wire));
    CHECK_EQ(parser.collection_size("objects"), 1U);

    std::string text;
    REQUIRE(parser.text("objects", 0, "label", text));
    CHECK_EQ(text, "vehicle");
    REQUIRE(parser.numeric("data[0].position_x", numeric));
    CHECK_EQ(numeric, doctest::Approx(4.0));
    REQUIRE(parser.numeric("data[0].yaw", numeric));
    CHECK_EQ(numeric, doctest::Approx(0.5));
    REQUIRE(parser.numeric("data[0].reserved", numeric));
    CHECK_EQ(numeric, 9);
  }

  TEST_CASE("grid and tensor payload elements are bounds checked") {
    zerocopy::OccupancyGrid grid;
    grid.set_width(2);
    grid.set_height(1);
    grid.set_cell_type(zerocopy::OccupancyGrid::kCellUint16);
    REQUIRE(grid.create(sizeof(uint16_t) * 2));
    uint16_t cells[] = {17, 65000};
    REQUIRE(grid.fill_data(reinterpret_cast<uint8_t*>(cells), sizeof(cells)));

    Bytes wire;
    const bool grid_serialized = grid >> wire;
    REQUIRE(grid_serialized);

    zerocopy::MessageParser parser;
    REQUIRE(parser.parse(zerocopy::MessageParser::Type::kOccupancyGrid, wire));
    CHECK_EQ(parser.collection_size("cells"), 2U);

    double numeric = 0;
    REQUIRE(parser.numeric("data[1]", numeric));
    CHECK_EQ(numeric, 65000);
    CHECK_FALSE(parser.numeric("data[2]", numeric));

    zerocopy::Tensor tensor;
    tensor.set_dtype(zerocopy::Tensor::kInt64);
    const uint32_t shape[] = {2};
    tensor.set_shape(shape, 1);
    REQUIRE(tensor.create(sizeof(int64_t) * 2));
    int64_t elements[] = {7, 9007199254740993LL};
    REQUIRE(tensor.fill_data(reinterpret_cast<uint8_t*>(elements), sizeof(elements)));
    const bool tensor_serialized = tensor >> wire;
    REQUIRE(tensor_serialized);
    REQUIRE(parser.parse(zerocopy::MessageParser::Type::kTensor, wire));
    CHECK_EQ(parser.collection_size("elements"), 2U);

    zerocopy::MessageParser::Value value;
    REQUIRE(parser.value("data[1]", value));
    REQUIRE(std::holds_alternative<int64_t>(value));
    CHECK_EQ(std::get<int64_t>(value), 9007199254740993LL);

    bool precision_loss = false;
    REQUIRE(parser.numeric("data[1]", numeric, &precision_loss));
    CHECK(precision_loss);
    REQUIRE(parser.numeric("shape[0]", numeric));
    CHECK_EQ(numeric, 2);
    CHECK_FALSE(parser.numeric("shape[0].invalid", numeric));
    CHECK_FALSE(parser.numeric("shape", 0, "invalid", numeric));

    parser.clear();
    CHECK_FALSE(parser.valid());
    CHECK_FALSE(parser.value("size", value));
  }

  TEST_CASE("tensor converts every supported element type") {
    struct ElementCase final {
      zerocopy::Tensor::DataType type;
      std::vector<uint8_t> bytes;
      double expected;
    };

    const auto bytes_of = [](auto value) {
      std::vector<uint8_t> bytes(sizeof(value));
      std::memcpy(bytes.data(), &value, sizeof(value));
      return bytes;
    };

    const std::vector<ElementCase> cases = {
        {zerocopy::Tensor::kBool, bytes_of(uint8_t{1}), 1.0},
        {zerocopy::Tensor::kInt8, bytes_of(int8_t{-2}), -2.0},
        {zerocopy::Tensor::kUint8, bytes_of(uint8_t{250}), 250.0},
        {zerocopy::Tensor::kInt16, bytes_of(int16_t{-1234}), -1234.0},
        {zerocopy::Tensor::kUint16, bytes_of(uint16_t{60000}), 60000.0},
        {zerocopy::Tensor::kInt32, bytes_of(int32_t{-123456}), -123456.0},
        {zerocopy::Tensor::kUint32, bytes_of(uint32_t{345678}), 345678.0},
        {zerocopy::Tensor::kInt64, bytes_of(int64_t{-1234567}), -1234567.0},
        {zerocopy::Tensor::kUint64, bytes_of(uint64_t{1234567}), 1234567.0},
        {zerocopy::Tensor::kFloat16, bytes_of(uint16_t{0x3C00}), 1.0},
        {zerocopy::Tensor::kBfloat16, bytes_of(uint16_t{0x3F80}), 1.0},
        {zerocopy::Tensor::kFloat32, bytes_of(1.5F), 1.5},
        {zerocopy::Tensor::kFloat64, bytes_of(2.5), 2.5},
    };

    for (const auto& element : cases) {
      zerocopy::Tensor tensor;
      tensor.set_dtype(element.type);
      const uint32_t shape[] = {1};
      tensor.set_shape(shape, 1);
      auto bytes = element.bytes;
      REQUIRE(tensor.fill_data(bytes.data(), bytes.size()));

      Bytes wire;
      const bool serialized = tensor >> wire;
      REQUIRE(serialized);

      zerocopy::MessageParser parser;
      REQUIRE(parser.parse(zerocopy::MessageParser::Type::kTensor, wire));

      double value = 0.0;
      REQUIRE(parser.numeric("data[0]", value));
      CHECK_EQ(value, doctest::Approx(element.expected));
    }
  }

  TEST_CASE("all header-bearing messages expose the common header and proxy does not") {
    const auto verify_header = [](auto& message, zerocopy::MessageParser::Type type, Bytes& wire) {
      std::strncpy(message.header.frame_id, "sensor_frame", sizeof(message.header.frame_id) - 1);
      message.header.seq = 17;
      message.header.reserved = 23;
      message.header.time_meas = 9007199254740993ULL;
      message.header.time_pub = 9007199254740995ULL;

      const bool serialized = message >> wire;
      REQUIRE(serialized);

      zerocopy::MessageParser parser;
      REQUIRE(parser.parse(type, wire));

      zerocopy::MessageParser::Value value;
      REQUIRE(parser.value("header.frame_id", value));
      CHECK_EQ(std::get<std::string>(value), "sensor_frame");
      REQUIRE(parser.value("header.seq", value));
      CHECK_EQ(std::get<uint64_t>(value), 17U);
      REQUIRE(parser.value("header.reserved", value));
      CHECK_EQ(std::get<uint64_t>(value), 23U);
      REQUIRE(parser.value("header.time_meas", value));
      CHECK_EQ(std::get<uint64_t>(value), 9007199254740993ULL);
      REQUIRE(parser.value("header.time_pub", value));
      CHECK_EQ(std::get<uint64_t>(value), 9007199254740995ULL);

      const auto fields = parser.fields();

      const auto reserved =
          std::find_if(fields.begin(), fields.end(), [](const auto& field) { return field.name == "header.reserved"; });
      const auto time_meas = std::find_if(fields.begin(), fields.end(),
                                          [](const auto& field) { return field.name == "header.time_meas"; });
      REQUIRE(reserved != fields.end());
      REQUIRE(time_meas != fields.end());
      CHECK(reserved->is_reserved);
      CHECK(time_meas->is_time);

      for (const std::string_view name :
           {"header.frame_id", "header.seq", "header.reserved", "header.time_meas", "header.time_pub"}) {
        CHECK(std::any_of(fields.begin(), fields.end(), [name](const auto& field) { return field.name == name; }));
      }
    };

    Bytes wire;

    zerocopy::RawData raw;
    REQUIRE(raw.create(1));
    verify_header(raw, zerocopy::MessageParser::Type::kRawData, wire);

    zerocopy::CameraFrame camera;
    REQUIRE(camera.create(1));
    verify_header(camera, zerocopy::MessageParser::Type::kCameraFrame, wire);

    zerocopy::PointCloud cloud;
    REQUIRE(cloud.create_v3f<>(1));
    REQUIRE(cloud.push_value_v3f(1.0F, 2.0F, 3.0F));
    verify_header(cloud, zerocopy::MessageParser::Type::kPointCloud, wire);

    zerocopy::OccupancyGrid grid;
    grid.set_width(1);
    grid.set_height(1);
    grid.set_cell_type(zerocopy::OccupancyGrid::kCellUint8);
    REQUIRE(grid.create(1));
    verify_header(grid, zerocopy::MessageParser::Type::kOccupancyGrid, wire);

    zerocopy::Tensor tensor;
    tensor.set_dtype(zerocopy::Tensor::kUint8);
    const uint32_t shape[] = {1};
    tensor.set_shape(shape, 1);
    REQUIRE(tensor.create(1));
    verify_header(tensor, zerocopy::MessageParser::Type::kTensor, wire);

    zerocopy::ObjectArray objects;
    REQUIRE(objects.create(1));
    REQUIRE(objects.push_value(zerocopy::ObjectArray::Object{}));
    verify_header(objects, zerocopy::MessageParser::Type::kObjectArray, wire);

    zerocopy::AudioFrame audio;
    REQUIRE(audio.create(1));
    verify_header(audio, zerocopy::MessageParser::Type::kAudioFrame, wire);

    zerocopy::ProxyData proxy;
    const uint8_t proxy_payload[] = {'x'};
    proxy.create(Bytes::deep_copy(proxy_payload, sizeof(proxy_payload)), "intra://source", "RawData");
    const bool proxy_serialized = proxy >> wire;
    REQUIRE(proxy_serialized);

    zerocopy::MessageParser parser;
    REQUIRE(parser.parse(zerocopy::MessageParser::Type::kProxyData, wire));

    zerocopy::MessageParser::Value value;
    CHECK_FALSE(parser.value("header.frame_id", value));
    const auto proxy_fields = parser.fields();
    CHECK(std::none_of(proxy_fields.begin(), proxy_fields.end(),
                       [](const auto& field) { return vlink::Helpers::has_startwith(field.name, "header."); }));
  }

  TEST_CASE("point schema handles non-terminated fields, unknown widths, and compressed coordinates") {
    constexpr uint64_t kUnknownSizes = UINT64_C(0x1248);

    zerocopy::PointCloud unknown;
    REQUIRE(unknown.create(1, kUnknownSizes, 0, "u1,u2,u4,u8"));
    REQUIRE(unknown.push_value(uint8_t{250}, int16_t{-1234}, 1.5F, 2.5));

    Bytes wire;
    const bool unknown_serialized = unknown >> wire;
    REQUIRE(unknown_serialized);

    zerocopy::MessageParser parser;
    REQUIRE(parser.parse(zerocopy::MessageParser::Type::kPointCloud, wire));

    const auto fields = parser.element_fields("points");
    REQUIRE_EQ(fields.size(), 4U);

    CHECK_EQ(fields[0].native_type, zerocopy::PointCloud::kUnknownType);
    CHECK_EQ(fields[0].storage_size, sizeof(uint8_t));
    CHECK_EQ(fields[0].byte_offset, 0U);
    CHECK_EQ(fields[0].element_index, 0U);
    CHECK_EQ(fields[1].storage_size, sizeof(int16_t));
    CHECK_EQ(fields[1].byte_offset, sizeof(uint8_t));
    CHECK_EQ(fields[1].element_index, 1U);
    CHECK_EQ(fields[2].storage_size, sizeof(float));
    CHECK_EQ(fields[2].byte_offset, sizeof(uint8_t) + sizeof(int16_t));
    CHECK_EQ(fields[3].storage_size, sizeof(double));

    const char field_buffer[] = {'u', '1', 'x'};
    const std::string_view non_terminated_field(field_buffer, 2);
    zerocopy::MessageParser::Value value;
    REQUIRE(parser.value("points", 0, non_terminated_field, value));
    CHECK_EQ(std::get<uint64_t>(value), 250U);
    REQUIRE(parser.value("points", 0, fields[0], value));
    CHECK_EQ(std::get<uint64_t>(value), 250U);
    REQUIRE(parser.value("points", 0, "u2", value));
    CHECK_EQ(std::get<int64_t>(value), -1234);
    REQUIRE(parser.value("points", 0, "u4", value));
    CHECK_EQ(std::get<double>(value), doctest::Approx(1.5));
    REQUIRE(parser.value("points", 0, "u8", value));
    CHECK_EQ(std::get<double>(value), doctest::Approx(2.5));

    constexpr uint64_t kOpaqueSizes = UINT64_C(0x333);
    zerocopy::PointCloud opaque;
    REQUIRE(opaque.create(1, kOpaqueSizes, 0, "a,b,c"));
    const uint8_t opaque_data[9]{};
    REQUIRE(opaque.fill_packed_data(opaque_data, 1));
    REQUIRE((opaque >> wire));
    REQUIRE(parser.parse(zerocopy::MessageParser::Type::kPointCloud, wire));

    const auto opaque_fields = parser.element_fields("points");
    REQUIRE_EQ(opaque_fields.size(), 3U);

    for (const auto& field : opaque_fields) {
      CHECK_EQ(field.type, zerocopy::MessageParser::ValueType::kValueUnknown);
      CHECK_FALSE(parser.value("points", 0, field, value));
    }

    zerocopy::PointCloud compressed;
    REQUIRE(compressed.create_v3f<>(1, {}, 100));
    REQUIRE(compressed.push_value_v3f(1.25F, -2.5F, 3.75F));
    const bool compressed_serialized = compressed >> wire;
    REQUIRE(compressed_serialized);
    REQUIRE(parser.parse(zerocopy::MessageParser::Type::kPointCloud, wire));

    CHECK_FALSE(parser.value("points", 0, fields[0], value));
    CHECK_FALSE(parser.value("points", 0, "u1", value));

    const auto compressed_fields = parser.element_fields("points");
    REQUIRE_EQ(compressed_fields.size(), 3U);

    for (size_t i = 0; i < compressed_fields.size(); ++i) {
      CHECK_EQ(compressed_fields[i].native_type, zerocopy::PointCloud::kFloatType);
      CHECK_EQ(compressed_fields[i].storage_size, sizeof(int16_t));
    }

    double coordinate = 0;
    REQUIRE(parser.numeric("points", 0, "x", coordinate));
    CHECK_EQ(coordinate, doctest::Approx(1.25).epsilon(0.01));
    REQUIRE(parser.numeric("points", 0, compressed_fields[0], coordinate));
    CHECK_EQ(coordinate, doctest::Approx(1.25).epsilon(0.01));
    REQUIRE(parser.numeric("points", 0, "y", coordinate));
    CHECK_EQ(coordinate, doctest::Approx(-2.5).epsilon(0.01));
    REQUIRE(parser.numeric("points", 0, "z", coordinate));
    CHECK_EQ(coordinate, doctest::Approx(3.75).epsilon(0.01));

    zerocopy::MessageParser moved(std::move(parser));
    REQUIRE(moved.numeric("points", 0, compressed_fields[1], coordinate));
    CHECK_EQ(coordinate, doctest::Approx(-2.5).epsilon(0.01));

    Bytes invalid;
    CHECK_FALSE(moved.parse(zerocopy::MessageParser::Type::kPointCloud, invalid));
    CHECK(moved.element_fields("points").empty());
    CHECK_FALSE(moved.value("points", 0, compressed_fields[0], value));
  }

  TEST_CASE("audio frame exposes 24-bit packed pcm samples with sign extension") {
    zerocopy::AudioFrame audio;
    audio.set_format(zerocopy::AudioFrame::kFormatPcmS24);
    audio.set_bit_depth(24);
    REQUIRE(audio.create(sizeof(uint8_t) * 6));

    uint8_t samples[] = {0x01, 0x00, 0x00, 0xFE, 0xFF, 0xFF};
    REQUIRE(audio.fill_data(samples, sizeof(samples)));

    Bytes wire;
    const bool serialized = audio >> wire;
    REQUIRE(serialized);

    zerocopy::MessageParser parser;
    REQUIRE(parser.parse(zerocopy::MessageParser::Type::kAudioFrame, wire));
    CHECK_EQ(parser.collection_size("data"), 2U);

    const auto fields = parser.element_fields("data");
    REQUIRE_EQ(fields.size(), 1U);
    CHECK_EQ(fields.front().storage_size, 3U);

    zerocopy::MessageParser::Value value;
    REQUIRE(parser.value("data", 0, "value", value));
    CHECK_EQ(std::get<int64_t>(value), 1);
    REQUIRE(parser.value("data", 1, "value", value));
    CHECK_EQ(std::get<int64_t>(value), -2);
    CHECK_FALSE(parser.value("data", 2, "value", value));
  }
}

// NOLINTEND
