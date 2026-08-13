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

#include <vlink/zerocopy/raw_data.h>

#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "../tools/autosar/test/generated_someip_features.h"
#include "../tools/autosar/test/generated_someip_types.h"
#include "./common_test.h"
#include "./extension/someip_serializer.h"
#include "./impl/intra_data.h"

struct PodMsg {
  int x;
  float y;
  double z;
};

static_assert(std::is_trivial_v<PodMsg>, "PodMsg must be trivial");
static_assert(std::is_standard_layout_v<PodMsg>, "PodMsg must be standard-layout");

struct CustomMsg {
  int32_t value{0};

  void operator>>(vlink::Bytes& out) const {
    out = vlink::Bytes::create(sizeof(int32_t));
    std::memcpy(out.data(), &value, sizeof(int32_t));
  }

  void operator<<(const vlink::Bytes& in) {
    if (in.size() == sizeof(int32_t)) {
      std::memcpy(&value, in.data(), sizeof(int32_t));
    }
  }
};

struct StreamMsg {
  int number{0};
};

inline std::stringstream& operator<<(std::stringstream& ss, const StreamMsg& m) {
  ss << m.number;
  return ss;
}

inline std::stringstream& operator>>(std::stringstream& ss, StreamMsg& m) {
  ss >> m.number;
  return ss;
}

struct AnotherCustom {
  uint8_t byte{0};

  void operator>>(vlink::Bytes& out) const { out = vlink::Bytes{byte}; }

  void operator<<(const vlink::Bytes& in) {
    if (!in.empty()) {
      byte = in.data()[0];
    }
  }
};

struct SizedCustom {
  int32_t value{0};
  bool replace_output{false};
  bool fail{false};

  [[nodiscard]] size_t get_serialized_size() const noexcept { return sizeof(value); }

  bool operator>>(vlink::Bytes& out) const {
    if (fail) {
      return false;
    }

    if (replace_output) {
      out = vlink::Bytes::create(sizeof(value));
    }

    if (out.size() != sizeof(value)) {
      return false;
    }

    std::memcpy(out.data(), &value, sizeof(value));
    return true;
  }

  bool operator<<(const vlink::Bytes& in) {
    if (in.size() != sizeof(value)) {
      return false;
    }

    std::memcpy(&value, in.data(), sizeof(value));
    return true;
  }
};

enum class SomeipMode : uint16_t {
  kManual = 0x1234,
  kAutomatic = 0xABCD,
};

struct SomeipScalars {
  uint8_t byte{0};
  uint16_t word{0};
  int32_t signed_value{0};
  float ratio{0.0F};
  bool enabled{false};
  SomeipMode mode{SomeipMode::kManual};

  VLINK_SOMEIP_ENDIAN_BIG
  VLINK_SOMEIP_FIELDS(byte, word, signed_value, ratio, enabled, mode)
};

struct SomeipChild {
  int16_t delta{0};
  std::string label;

  VLINK_SOMEIP_FIELDS(delta, label)
};

struct SomeipMessage {
  uint8_t state{0};
  std::string name;
  std::vector<uint16_t> samples;
  std::array<uint32_t, 2> limits{};
  SomeipChild child;
  vlink::Bytes payload;

  VLINK_SOMEIP_FIELDS(state, name, samples, limits, child, payload)
};

struct SomeipWideScalars {
  uint32_t unsigned32{0};
  uint64_t unsigned64{0};
  int8_t signed8{0};
  int16_t signed16{0};
  int64_t signed64{0};
  double ratio{0.0};

  VLINK_SOMEIP_FIELDS(unsigned32, unsigned64, signed8, signed16, signed64, ratio)
};

struct SomeipContainers {
  std::vector<bool> flags;
  std::string text;
  std::vector<uint8_t> values;
  std::array<uint16_t, 2> fixed{};

  VLINK_SOMEIP_FIELDS(flags, text, values, fixed)
};

struct SomeipFixedWithTail {
  std::array<uint16_t, 2> fixed{};
  uint8_t tail{0};

  VLINK_SOMEIP_FIELDS(fixed, tail)
};

struct SomeipNestedContainers {
  std::array<std::array<uint16_t, 2>, 2> matrix{};
  std::vector<SomeipChild> children;

  VLINK_SOMEIP_FIELDS(matrix, children)
};

struct SomeipChildArray {
  std::array<SomeipChild, 2> children;

  VLINK_SOMEIP_FIELDS(children)
};

struct SomeipVectorOnly {
  std::vector<uint16_t> values;

  VLINK_SOMEIP_FIELDS(values)
};

struct SomeipBoundedVectorWithTail {
  std::vector<uint16_t> values;
  uint8_t tail{0};

  VLINK_SOMEIP_FIELDS(VLINK_SOMEIP_LENGTH_MAX(values, 1U, 2U), tail)
};

struct SomeipDynamicMatrix {
  std::vector<std::vector<uint16_t>> values;

  VLINK_SOMEIP_FIELDS(values)
};

struct SomeipArrayLengthChild {
  std::vector<uint8_t> values;

  VLINK_SOMEIP_FIELDS(VLINK_SOMEIP_LENGTH(values, 2U))
};

struct SomeipArrayLengths {
  std::vector<std::vector<uint16_t>> dynamic;
  std::array<std::array<uint8_t, 2>, 2> fixed{};
  std::vector<std::vector<uint8_t>> partial;
  std::vector<SomeipArrayLengthChild> children;

  VLINK_SOMEIP_FIELDS(VLINK_SOMEIP_ARRAY_LENGTH(dynamic, 1U, 2U), VLINK_SOMEIP_ARRAY_LENGTH(fixed, 0U, 1U),
                      VLINK_SOMEIP_ARRAY_LENGTH(partial, 1U), VLINK_SOMEIP_ARRAY_LENGTH(children, 1U))
};

struct SomeipAlignedArrayLengths {
  uint8_t prefix{0};
  std::vector<std::vector<uint8_t>> values;
  uint8_t suffix{0};

  VLINK_SOMEIP_ALIGNMENT(4U)
  VLINK_SOMEIP_FIELDS(prefix, VLINK_SOMEIP_ARRAY_LENGTH(values, 1U, 1U), suffix)
};

struct SomeipZeroByteArrayElements {
  std::vector<std::array<uint8_t, 0>> values;

  VLINK_SOMEIP_FIELDS(VLINK_SOMEIP_ARRAY_LENGTH(values, 1U, 0U))
};

struct SomeipBytesItem {
  vlink::Bytes value;

  VLINK_SOMEIP_FIELDS(value)
};

struct SomeipBytesItems {
  std::vector<SomeipBytesItem> values;

  VLINK_SOMEIP_FIELDS(values)
};

struct SomeipText {
  std::string value;

  VLINK_SOMEIP_FIELDS(value)
};

struct SomeipAlignedText {
  uint16_t prefix{0};
  std::string value;
  uint32_t suffix{0};

  VLINK_SOMEIP_ALIGNMENT(32U)
  VLINK_SOMEIP_FIELDS(prefix, VLINK_SOMEIP_LENGTH(value, 1U), suffix)
};

struct SomeipAlignedChildren {
  std::vector<SomeipChild> values;
  uint8_t suffix{0};

  VLINK_SOMEIP_ALIGNMENT(8U)
  VLINK_SOMEIP_FIELDS(values, suffix)
};

struct SomeipLengthFields {
  std::string short_name;
  std::string name;
  std::vector<uint16_t> values;
  std::array<uint8_t, 2> fixed{};
  uint8_t counter{0};

  VLINK_SOMEIP_FIELDS(VLINK_SOMEIP_LENGTH(short_name, 1U), VLINK_SOMEIP_LENGTH(name, 2U),
                      VLINK_SOMEIP_LENGTH(values, 4U), VLINK_SOMEIP_LENGTH(fixed, 0U), counter)
};

struct SomeipLittleEndian {
  uint16_t word{0};
  std::vector<uint16_t> values;
  uint32_t suffix{0};

  VLINK_SOMEIP_ENDIAN_LITTLE
  VLINK_SOMEIP_FIELDS(word, VLINK_SOMEIP_LENGTH(values, 2U), suffix)
};

struct SomeipSizedChild {
  int16_t delta{0};
  std::string label;

  VLINK_SOMEIP_STRUCT_LENGTH(1U)
  VLINK_SOMEIP_FIELDS(delta, label)
};

struct SomeipSizedMessage {
  SomeipSizedChild child;
  uint8_t suffix{0};

  VLINK_SOMEIP_STRUCT_LENGTH(2U)
  VLINK_SOMEIP_FIELDS(child, suffix)
};

struct SomeipMapMessage {
  std::map<uint16_t, std::string> table;
  uint8_t suffix{0};

  VLINK_SOMEIP_ALIGNMENT(4U)
  VLINK_SOMEIP_FIELDS(VLINK_SOMEIP_LENGTH(table, 2U), suffix)
};

struct SomeipHashedMap {
  std::unordered_map<uint32_t, uint16_t> index;

  VLINK_SOMEIP_FIELDS(index)
};

struct SomeipWideText {
  std::u16string name;
  std::u16string label;

  VLINK_SOMEIP_FIELDS(VLINK_SOMEIP_UTF16_MAX(name, 2U, vlink::SomeipSerializer::Endian::kBig, 2U),
                      VLINK_SOMEIP_UTF16_MAX(label, 1U, vlink::SomeipSerializer::Endian::kLittle, 1U))
};

struct SomeipWideDefault {
  std::u16string name;

  VLINK_SOMEIP_FIELDS(name)
};

struct SomeipUnionMessage {
  std::variant<uint16_t, std::string> choice;
  uint8_t tail{0};

  VLINK_SOMEIP_FIELDS(VLINK_SOMEIP_UNION(choice, 4U, 2U), tail)
};

struct SomeipPlainUnion {
  std::variant<uint8_t, uint32_t> value;

  VLINK_SOMEIP_FIELDS(value)
};

struct SomeipAlignedUnion {
  std::variant<uint16_t> value;
  uint8_t suffix{0};

  VLINK_SOMEIP_ALIGNMENT(4U)
  VLINK_SOMEIP_FIELDS(value, suffix)
};

struct SomeipNullableUnion {
  std::variant<std::monostate, uint16_t, std::string> value;
  uint8_t suffix{0};

  VLINK_SOMEIP_ALIGNMENT(4U)
  VLINK_SOMEIP_FIELDS(VLINK_SOMEIP_UNION(value, 2U, 1U), suffix)
};

struct SomeipUnframedUnion {
  std::variant<uint16_t, int16_t> value;
  uint8_t suffix{0};

  VLINK_SOMEIP_ALIGNMENT(4U)
  VLINK_SOMEIP_FIELDS(VLINK_SOMEIP_UNION(value, 0U, 1U), suffix)
};

struct SomeipFixedStrings {
  std::string name;
  std::u16string title;

  VLINK_SOMEIP_FIELDS(VLINK_SOMEIP_FIXED_STRING(name, 8U, 0U), VLINK_SOMEIP_FIXED_UTF16_LE(title, 8U, 1U))
};

struct SomeipTlvFixedStrings {
  std::optional<std::string> name;
  std::u16string title;

  VLINK_SOMEIP_STRUCT_LENGTH(1U)
  VLINK_SOMEIP_FIELDS(VLINK_SOMEIP_TLV_FIXED_STRING(1, name, 8U, 1U, vlink::SomeipSerializer::Endian::kBig),
                      VLINK_SOMEIP_TLV_STATIC_FIXED_STRING(2, title, 8U, 1U, vlink::SomeipSerializer::Endian::kBig))
};

struct SomeipTlvMessage {
  uint16_t id{0};
  std::optional<std::string> label;
  std::vector<uint8_t> data;

  VLINK_SOMEIP_STRUCT_LENGTH(2U)
  VLINK_SOMEIP_FIELDS(VLINK_SOMEIP_TLV_LENGTH(1, id, 2U), VLINK_SOMEIP_TLV_LENGTH_MAX(2, label, 2U, 1U),
                      VLINK_SOMEIP_TLV_LENGTH_MAX(3, data, 2U, 2U))
};

struct SomeipTlvChild {
  uint8_t code{0};

  VLINK_SOMEIP_STRUCT_LENGTH(1U)
  VLINK_SOMEIP_FIELDS(VLINK_SOMEIP_TLV_LENGTH(1, code, 1U))
};

struct SomeipTlvNested {
  std::optional<SomeipTlvChild> child;
  uint8_t tail{0};

  VLINK_SOMEIP_STRUCT_LENGTH(1U)
  VLINK_SOMEIP_FIELDS(VLINK_SOMEIP_TLV_LENGTH(1, child, 1U), VLINK_SOMEIP_TLV_LENGTH(2, tail, 1U))
};

struct SomeipTlvLengthMode {
  std::vector<uint8_t> data;

  VLINK_SOMEIP_STRUCT_LENGTH(4U)
  VLINK_SOMEIP_FIELDS(VLINK_SOMEIP_TLV_LENGTH(1, data, 4U))
};

struct SomeipTlvStaticLength {
  std::vector<uint8_t> data;

  VLINK_SOMEIP_STRUCT_LENGTH(2U)
  VLINK_SOMEIP_FIELDS(VLINK_SOMEIP_TLV_STATIC_LENGTH(1, data, 2U))
};

struct SomeipTlvDefaults {
  uint16_t id{0};
  std::vector<uint8_t> data;

  VLINK_SOMEIP_STRUCT_LENGTH(4U)
  VLINK_SOMEIP_FIELDS(VLINK_SOMEIP_TLV(1, id), VLINK_SOMEIP_TLV(2, data))
};

struct SomeipTlvMatrix {
  std::vector<std::vector<uint8_t>> values;
  std::optional<std::vector<std::vector<uint8_t>>> extra;

  VLINK_SOMEIP_STRUCT_LENGTH(1U)
  VLINK_SOMEIP_FIELDS(VLINK_SOMEIP_TLV_ARRAY_LENGTH(1, values, 1U, 1U), VLINK_SOMEIP_TLV_ARRAY_LENGTH(2, extra, 1U, 1U))
};

#ifdef VLINK_HAS_CDR
struct SomeipCdrHybrid {
  uint8_t value{0};

  VLINK_SOMEIP_FIELDS(value)

  void serialize(eprosima::fastcdr::Cdr& cdr);

  void deserialize(eprosima::fastcdr::Cdr& cdr);
};
#endif

VLINK_INTRA_DATA_DECLARE(vlink::zerocopy::RawData, WrappedRawData)

TEST_SUITE("ser-types") {
  TEST_CASE("bytes maps to kBytesType") {
    static constexpr auto t = Serializer::get_type_of<Bytes>();
    CHECK(t == Serializer::kBytesType);
    CHECK(Serializer::is_supported(t));
  }

  TEST_CASE("std string maps to kStringType") {
    static constexpr auto t = Serializer::get_type_of<std::string>();
    CHECK(t == Serializer::kStringType);
    CHECK(Serializer::is_supported(t));
  }

  TEST_CASE("pod struct maps to kStandardType") {
    static constexpr auto t = Serializer::get_type_of<PodMsg>();
    CHECK(t == Serializer::kStandardType);
    CHECK(Serializer::is_supported(t));
  }

  TEST_CASE("custom serializable type maps to kCustomType") {
    static constexpr auto t = Serializer::get_type_of<CustomMsg>();
    CHECK(t == Serializer::kCustomType);
    CHECK(Serializer::is_supported(t));
  }

  TEST_CASE("someip fields map to kSomeipType") {
    static constexpr auto t = Serializer::get_type_of<SomeipMessage>();
    CHECK(t == Serializer::kSomeipType);
    CHECK(Serializer::is_someip_type<SomeipMessage>());
    CHECK(Serializer::is_custom_type<SomeipMessage>());
    CHECK(Serializer::is_supported(t));
  }

  TEST_CASE("someip shared ptr maps to kSomeipType") {
    static constexpr auto t = Serializer::get_type_of<std::shared_ptr<SomeipMessage>>();
    CHECK(t == Serializer::kSomeipType);
  }

#ifdef VLINK_HAS_CDR
  TEST_CASE("someip marker takes precedence over CDR members") {
    CHECK(Serializer::is_someip_type<SomeipCdrHybrid>());
    CHECK(Serializer::is_cdr_type<SomeipCdrHybrid>());
    CHECK(Serializer::get_type_of<SomeipCdrHybrid>() == Serializer::kSomeipType);
  }
#endif

  TEST_CASE("another custom type also maps to kCustomType") {
    static constexpr auto t = Serializer::get_type_of<AnotherCustom>();
    CHECK(t == Serializer::kCustomType);
    CHECK(Serializer::is_supported(t));
  }

  TEST_CASE("kUnknownType is the only unsupported type") {
    CHECK_FALSE(Serializer::is_supported(Serializer::kUnknownType));
  }

  TEST_CASE("all named serializer types are supported") {
    CHECK(Serializer::is_supported(Serializer::kBytesType));
    CHECK(Serializer::is_supported(Serializer::kDynamicType));
    CHECK(Serializer::is_supported(Serializer::kCustomType));
    CHECK(Serializer::is_supported(Serializer::kSomeipType));
    CHECK(Serializer::is_supported(Serializer::kCdrType));
    CHECK(Serializer::is_supported(Serializer::kProtoType));
    CHECK(Serializer::is_supported(Serializer::kProtoPtrType));
    CHECK(Serializer::is_supported(Serializer::kFlatTableType));
    CHECK(Serializer::is_supported(Serializer::kFlatPtrType));
    CHECK(Serializer::is_supported(Serializer::kFlatBuilderType));
    CHECK(Serializer::is_supported(Serializer::kStringType));
    CHECK(Serializer::is_supported(Serializer::kCharsType));
    CHECK(Serializer::is_supported(Serializer::kStreamType));
    CHECK(Serializer::is_supported(Serializer::kStandardType));
    CHECK(Serializer::is_supported(Serializer::kStandardPtrType));
  }

  TEST_CASE("std string infers raw schema family") {
    static constexpr auto schema_type = Serializer::get_schema_type<std::string>();
    CHECK(schema_type == vlink::SchemaType::kRaw);
  }

  TEST_CASE("bytes infers raw schema family") {
    static constexpr auto schema_type = Serializer::get_schema_type<Bytes>();
    CHECK(schema_type == vlink::SchemaType::kRaw);
  }

  TEST_CASE("pod struct infers raw schema family") {
    static constexpr auto schema_type = Serializer::get_schema_type<PodMsg>();
    CHECK(schema_type == vlink::SchemaType::kRaw);
  }

  TEST_CASE("custom codec infers raw schema family") {
    static constexpr auto schema_type = Serializer::get_schema_type<CustomMsg>();
    CHECK(schema_type == vlink::SchemaType::kRaw);
  }

  TEST_CASE("someip codec infers raw schema family") {
    static constexpr auto schema_type = Serializer::get_schema_type<SomeipMessage>();
    CHECK(schema_type == vlink::SchemaType::kRaw);
  }

  TEST_CASE("stream codec infers raw schema family") {
    static constexpr auto schema_type = Serializer::get_schema_type<StreamMsg>();
    CHECK(schema_type == vlink::SchemaType::kRaw);
  }

  TEST_CASE("zerocopy payload infers zerocopy schema family") {
    static constexpr auto schema_type = Serializer::get_schema_type<vlink::zerocopy::RawData>();
    CHECK(schema_type == vlink::SchemaType::kZeroCopy);
  }

  TEST_CASE("intra wrapper preserves inner zerocopy schema family") {
    static constexpr auto schema_type = Serializer::get_schema_type<WrappedRawData>();
    CHECK(schema_type == vlink::SchemaType::kZeroCopy);
  }

  TEST_CASE("serializer type ordinal values follow the public order") {
    CHECK(static_cast<uint8_t>(Serializer::kUnknownType) == 0U);
    CHECK(static_cast<uint8_t>(Serializer::kBytesType) == 1U);
    CHECK(static_cast<uint8_t>(Serializer::kDynamicType) == 2U);
    CHECK(static_cast<uint8_t>(Serializer::kCustomType) == 3U);
    CHECK(static_cast<uint8_t>(Serializer::kSomeipType) == 4U);
    CHECK(static_cast<uint8_t>(Serializer::kCdrType) == 5U);
    CHECK(static_cast<uint8_t>(Serializer::kProtoType) == 6U);
    CHECK(static_cast<uint8_t>(Serializer::kProtoPtrType) == 7U);
    CHECK(static_cast<uint8_t>(Serializer::kFlatTableType) == 8U);
    CHECK(static_cast<uint8_t>(Serializer::kFlatPtrType) == 9U);
    CHECK(static_cast<uint8_t>(Serializer::kFlatBuilderType) == 10U);
    CHECK(static_cast<uint8_t>(Serializer::kStringType) == 11U);
    CHECK(static_cast<uint8_t>(Serializer::kCharsType) == 12U);
    CHECK(static_cast<uint8_t>(Serializer::kStreamType) == 13U);
    CHECK(static_cast<uint8_t>(Serializer::kStandardType) == 14U);
    CHECK(static_cast<uint8_t>(Serializer::kStandardPtrType) == 15U);
  }
}

TEST_SUITE("ser-someip") {
  TEST_CASE("generated AUTOSAR fixed strings remain wire compatible") {
    vlink::autosar::features::Payload source;
    source.text8 = "A";
    source.text16 = u"B";

    vlink::Bytes encoded;
    REQUIRE(Serializer::serialize(source, encoded));
    const std::array<uint8_t, 23> expected = {
        0x0BU, 0xEFU, 0xBBU, 0xBFU, 0x41U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x0AU, 0xFEU, 0xFFU, 0x00U, 0x42U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    };
    REQUIRE(encoded.size() == expected.size());
    CHECK(std::memcmp(encoded.data(), expected.data(), expected.size()) == 0);

    vlink::autosar::features::Payload target;
    REQUIRE(Serializer::deserialize(encoded, target));
    CHECK(target.text8 == source.text8);
    CHECK(target.text16 == source.text16);

    source.text8 = "ABCDE";
    CHECK_FALSE(Serializer::serialize(source, encoded));
    source.text8 = "A";
    source.text16 = u"ABC";
    REQUIRE(Serializer::serialize(source, encoded));
    source.text16.push_back(u'D');
    CHECK_FALSE(Serializer::serialize(source, encoded));

    const vlink::Bytes oversized{0x0B, 0xEF, 0xBB, 0xBF, 'A',  'B',  'C',  'D',  'E',  0x00, 0x00, 0x00,
                                 0x0A, 0xFE, 0xFF, 0x00, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    CHECK_FALSE(Serializer::deserialize(oversized, target));
  }

  TEST_CASE("generated AUTOSAR TLV fixed strings remain wire compatible") {
    vlink::autosar::features::TlvPayload source;
    source.text8 = "A";
    source.text16 = u"B";

    vlink::Bytes encoded;
    REQUIRE(Serializer::serialize(source, encoded));
    const std::array<uint8_t, 28> expected = {
        0x1BU, 0x50U, 0x01U, 0x0BU, 0xEFU, 0xBBU, 0xBFU, 0x41U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x50U, 0x02U, 0x0AU, 0xFEU, 0xFFU, 0x00U, 0x42U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    };
    REQUIRE(encoded.size() == expected.size());
    CHECK(std::memcmp(encoded.data(), expected.data(), expected.size()) == 0);

    vlink::autosar::features::TlvPayload target;
    REQUIRE(Serializer::deserialize(encoded, target));
    CHECK(target.text8 == source.text8);
    CHECK(target.text16 == source.text16);

    source.text8 = "ABCDE";
    CHECK_FALSE(Serializer::serialize(source, encoded));

    const vlink::Bytes oversized{0x1B, 0x50, 0x01, 0x0B, 0xEF, 0xBB, 0xBF, 'A',  'B',  'C',  'D',  'E',  0x00, 0x00,
                                 0x00, 0x50, 0x02, 0x0A, 0xFE, 0xFF, 0x00, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    CHECK_FALSE(Serializer::deserialize(oversized, target));
  }

  TEST_CASE("generated AUTOSAR static TLV fixed strings remain wire compatible") {
    vlink::autosar::features::StaticTlvPayload source;
    source.text8 = "A";

    vlink::Bytes encoded;
    REQUIRE(Serializer::serialize(source, encoded));
    const std::array<uint8_t, 15> expected = {
        0x0EU, 0x40U, 0x01U, 0x0BU, 0xEFU, 0xBBU, 0xBFU, 0x41U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    };
    REQUIRE(encoded.size() == expected.size());
    CHECK(std::memcmp(encoded.data(), expected.data(), expected.size()) == 0);

    vlink::autosar::features::StaticTlvPayload target;
    REQUIRE(Serializer::deserialize(encoded, target));
    CHECK(target.text8 == source.text8);

    source.text8 = "ABCDE";
    CHECK_FALSE(Serializer::serialize(source, encoded));
  }

  TEST_CASE("generated AUTOSAR default and wire format remain executable") {
    auto source = vlink::autosar::VehicleState::make_default();

    CHECK(source.sequence == 7U);
    CHECK(source.valid);
    CHECK(source.mode == vlink::autosar::GearMode::kDrive);
    CHECK(source.temperature == doctest::Approx(20.5F));
    CHECK(source.name == "parked");
    CHECK(source.position.x == -5);
    CHECK(source.position.y == 8);
    CHECK(source.samples == vlink::autosar::SampleWindow{10U, 20U, 30U, 40U});
    REQUIRE(source.objects.size() == 2U);
    CHECK(source.objects[0].x == 1);
    CHECK(source.objects[0].y == 2);
    CHECK(source.objects[1].x == -3);
    CHECK(source.objects[1].y == 4);
    REQUIRE(source.payload.size() == 4U);
    CHECK(source.payload.data()[0] == 0xDEU);
    CHECK(source.payload.data()[1] == 0xADU);
    CHECK(source.payload.data()[2] == 0xBEU);
    CHECK(source.payload.data()[3] == 0xEFU);
    CHECK(source.matrix == std::array<std::array<uint16_t, 3>, 2>{{{1U, 2U, 3U}, {4U, 5U, 6U}}});

    source.sequence = 0x01020304U;
    source.temperature = 36.5F;
    source.name = "vehicle";
    source.position = {-120, 450};

    vlink::Bytes encoded;
    REQUIRE(Serializer::serialize(source, encoded));
    const std::array<uint8_t, 94> expected = {
        0x00U, 0x5CU, 0x04U, 0x03U, 0x02U, 0x01U, 0x01U, 0x01U, 0x00U, 0x00U, 0x12U, 0x42U, 0x00U, 0x0BU, 0xEFU, 0xBBU,
        0xBFU, 0x76U, 0x65U, 0x68U, 0x69U, 0x63U, 0x6CU, 0x65U, 0x00U, 0x00U, 0x08U, 0x88U, 0xFFU, 0xFFU, 0xFFU, 0xC2U,
        0x01U, 0x00U, 0x00U, 0x0AU, 0x00U, 0x14U, 0x00U, 0x1EU, 0x00U, 0x28U, 0x00U, 0x00U, 0x14U, 0x00U, 0x08U, 0x01U,
        0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x08U, 0xFDU, 0xFFU, 0xFFU, 0xFFU, 0x04U, 0x00U, 0x00U,
        0x00U, 0x04U, 0xDEU, 0xADU, 0xBEU, 0xEFU, 0x00U, 0x00U, 0x00U, 0x14U, 0x00U, 0x00U, 0x00U, 0x06U, 0x01U, 0x00U,
        0x02U, 0x00U, 0x03U, 0x00U, 0x00U, 0x00U, 0x00U, 0x06U, 0x04U, 0x00U, 0x05U, 0x00U, 0x06U, 0x00U,
    };
    REQUIRE(encoded.size() == expected.size());
    CHECK(std::memcmp(encoded.data(), expected.data(), expected.size()) == 0);

    const auto golden = vlink::Bytes::shallow_copy(expected.data(), expected.size());
    vlink::autosar::VehicleState target;
    REQUIRE(Serializer::deserialize(golden, target));
    CHECK(target.sequence == source.sequence);
    CHECK(target.valid == source.valid);
    CHECK(target.mode == source.mode);
    CHECK(target.temperature == doctest::Approx(source.temperature));
    CHECK(target.name == source.name);
    CHECK(target.position.x == source.position.x);
    CHECK(target.position.y == source.position.y);
    CHECK(target.samples == source.samples);
    REQUIRE(target.objects.size() == source.objects.size());
    CHECK(target.objects[0].x == source.objects[0].x);
    CHECK(target.objects[0].y == source.objects[0].y);
    CHECK(target.objects[1].x == source.objects[1].x);
    CHECK(target.objects[1].y == source.objects[1].y);
    CHECK(target.payload == source.payload);
    CHECK(target.matrix == source.matrix);

    source.name = std::string{"v\xC3\xA9hicle", 8U};
    REQUIRE(Serializer::serialize(source, encoded));
    REQUIRE(Serializer::deserialize(encoded, target));
    CHECK(target.name == source.name);

    source.name = "vehicleX";
    CHECK(source.get_serialized_size() == 0U);
    CHECK_FALSE(Serializer::serialize(source, encoded));
    source.name = "vehicle";

    std::vector<uint8_t> oversized(expected.begin(), expected.end());
    oversized[1] = 0x67U;
    oversized[44] = 0x1EU;
    oversized.insert(oversized.begin() + 65, {0x00U, 0x08U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U});
    oversized[75] = 0x05U;
    oversized.insert(oversized.begin() + 80, 0xAAU);
    const auto oversized_data = vlink::Bytes::shallow_copy(oversized.data(), oversized.size());
    REQUIRE(Serializer::deserialize(oversized_data, target));
    REQUIRE(target.objects.size() == 2U);
    CHECK(target.payload == source.payload);
    CHECK(target.matrix == source.matrix);

    std::vector<uint8_t> long_name(expected.begin(), expected.end());
    long_name[1] = 0x5DU;
    long_name[13] = 0x0CU;
    long_name.insert(long_name.begin() + 24, static_cast<uint8_t>('X'));
    const auto long_name_data = vlink::Bytes::shallow_copy(long_name.data(), long_name.size());
    CHECK_FALSE(Serializer::deserialize(long_name_data, target));

    source.objects.emplace_back();
    CHECK(source.get_serialized_size() == 0U);
    CHECK_FALSE(Serializer::serialize(source, encoded));
    source.objects.pop_back();

    REQUIRE(source.payload.resize(5U));
    CHECK(source.get_serialized_size() == 0U);
    CHECK_FALSE(Serializer::serialize(source, encoded));
  }

  TEST_CASE("serializes scalars and enums in big endian order") {
    SomeipScalars source;
    source.byte = 0x12;
    source.word = 0x3456;
    source.signed_value = -2;
    source.ratio = 1.0F;
    source.enabled = true;
    source.mode = SomeipMode::kAutomatic;

    vlink::Bytes data;
    REQUIRE(Serializer::serialize(source, data));

    const std::array<uint8_t, 14> expected = {0x12, 0x34, 0x56, 0xFF, 0xFF, 0xFF, 0xFE,
                                              0x3F, 0x80, 0x00, 0x00, 0x01, 0xAB, 0xCD};
    REQUIRE(data.size() == expected.size());
    CHECK(std::memcmp(data.data(), expected.data(), expected.size()) == 0);

    const auto golden = vlink::Bytes::shallow_copy(expected.data(), expected.size());
    SomeipScalars target;
    REQUIRE(Serializer::deserialize(golden, target));
    CHECK(target.byte == source.byte);
    CHECK(target.word == source.word);
    CHECK(target.signed_value == source.signed_value);
    CHECK(target.ratio == source.ratio);
    CHECK(target.enabled == source.enabled);
    CHECK(target.mode == source.mode);
  }

  TEST_CASE("macro generated bytes operators serialize and deserialize") {
    SomeipChild source;
    source.delta = -9;
    source.label = "operator";

    vlink::Bytes data;
    REQUIRE((source >> data));

    SomeipChild target;
    REQUIRE((target << data));
    CHECK(target.delta == source.delta);
    CHECK(target.label == source.label);
  }

  TEST_CASE("aligns fields after variable size data from the SOME/IP message start") {
    SomeipAlignedText source;
    source.prefix = 0x1234U;
    source.value = "x";
    source.suffix = 0xAABBCCDDU;

    vlink::Bytes data;
    REQUIRE(Serializer::serialize(source, data));

    const std::array<uint8_t, 20> expected = {0x12, 0x34, 0x05, 0xEF, 0xBB, 0xBF, 'x',  0x00, 0x00, 0x00,
                                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAA, 0xBB, 0xCC, 0xDD};
    REQUIRE(data.size() == expected.size());
    CHECK(std::memcmp(data.data(), expected.data(), expected.size()) == 0);

    SomeipAlignedText target;
    REQUIRE(Serializer::deserialize(data, target));
    CHECK(target.prefix == source.prefix);
    CHECK(target.value == source.value);
    CHECK(target.suffix == source.suffix);
  }

  TEST_CASE("aligns variable size array elements") {
    SomeipAlignedChildren source;
    source.values.resize(2U);
    source.values[0].delta = 0x0102;
    source.values[0].label = "a";
    source.values[1].delta = 0x0304;
    source.values[1].label = "bc";
    source.suffix = 0x7FU;

    vlink::Bytes data;
    REQUIRE(Serializer::serialize(source, data));

    const std::array<uint8_t, 33> expected = {0x00, 0x00, 0x00, 0x18, 0x01, 0x02, 0x00, 0x00, 0x00, 0x05, 0xEF,
                                              0xBB, 0xBF, 'a',  0x00, 0x00, 0x03, 0x04, 0x00, 0x00, 0x00, 0x06,
                                              0xEF, 0xBB, 0xBF, 'b',  'c',  0x00, 0x00, 0x00, 0x00, 0x00, 0x7F};
    REQUIRE(data.size() == expected.size());
    CHECK(std::memcmp(data.data(), expected.data(), expected.size()) == 0);

    SomeipAlignedChildren target;
    REQUIRE(Serializer::deserialize(data, target));
    REQUIRE(target.values.size() == source.values.size());
    CHECK(target.values[0].delta == source.values[0].delta);
    CHECK(target.values[0].label == source.values[0].label);
    CHECK(target.values[1].delta == source.values[1].delta);
    CHECK(target.values[1].label == source.values[1].label);
    CHECK(target.suffix == source.suffix);
  }

  TEST_CASE("uses configured length field widths") {
    SomeipLengthFields source;
    source.short_name = "a";
    source.name = "bc";
    source.values = {0x1234U, 0xABCDU};
    source.fixed = {0x56U, 0x78U};
    source.counter = 0x9AU;

    vlink::Bytes data;
    REQUIRE(Serializer::serialize(source, data));

    const std::array<uint8_t, 25> expected = {0x05, 0xEF, 0xBB, 0xBF, 'a',  0x00, 0x00, 0x06, 0xEF,
                                              0xBB, 0xBF, 'b',  'c',  0x00, 0x00, 0x00, 0x00, 0x04,
                                              0x12, 0x34, 0xAB, 0xCD, 0x56, 0x78, 0x9A};
    REQUIRE(data.size() == expected.size());
    CHECK(std::memcmp(data.data(), expected.data(), expected.size()) == 0);

    SomeipLengthFields target;
    REQUIRE(Serializer::deserialize(data, target));
    CHECK(target.short_name == source.short_name);
    CHECK(target.name == source.name);
    CHECK(target.values == source.values);
    CHECK(target.fixed == source.fixed);
    CHECK(target.counter == source.counter);
  }

  TEST_CASE("uses little endian payload values and big endian length fields") {
    SomeipLittleEndian source;
    source.word = 0x1234U;
    source.values = {0x0102U, 0x0304U};
    source.suffix = 0xAABBCCDDU;

    vlink::Bytes data;
    REQUIRE(Serializer::serialize(source, data));

    const std::array<uint8_t, 12> expected = {0x34, 0x12, 0x00, 0x04, 0x02, 0x01, 0x04, 0x03, 0xDD, 0xCC, 0xBB, 0xAA};
    REQUIRE(data.size() == expected.size());
    CHECK(std::memcmp(data.data(), expected.data(), expected.size()) == 0);

    SomeipLittleEndian target;
    REQUIRE(Serializer::deserialize(data, target));
    CHECK(target.word == source.word);
    CHECK(target.values == source.values);
    CHECK(target.suffix == source.suffix);
  }

  TEST_CASE("uses configured structure length fields") {
    SomeipSizedMessage source;
    source.child.delta = 0x0102;
    source.child.label = "a";
    source.suffix = 0x7FU;

    vlink::Bytes data;
    REQUIRE(Serializer::serialize(source, data));

    const std::array<uint8_t, 15> expected = {0x00, 0x0D, 0x0B, 0x01, 0x02, 0x00, 0x00, 0x00,
                                              0x05, 0xEF, 0xBB, 0xBF, 'a',  0x00, 0x7F};
    REQUIRE(data.size() == expected.size());
    CHECK(std::memcmp(data.data(), expected.data(), expected.size()) == 0);

    SomeipSizedMessage target;
    REQUIRE(Serializer::deserialize(data, target));
    CHECK(target.child.delta == source.child.delta);
    CHECK(target.child.label == source.child.label);
    CHECK(target.suffix == source.suffix);

    const std::array<uint8_t, 19> extended = {0x00, 0x11, 0x0D, 0x01, 0x02, 0x00, 0x00, 0x00, 0x05, 0xEF,
                                              0xBB, 0xBF, 'a',  0x00, 0xDE, 0xAD, 0x7F, 0xBE, 0xEF};
    const auto extended_data = vlink::Bytes::shallow_copy(extended.data(), extended.size());
    SomeipSizedMessage extended_target;
    REQUIRE(Serializer::deserialize(extended_data, extended_target));
    CHECK(extended_target.child.delta == source.child.delta);
    CHECK(extended_target.child.label == source.child.label);
    CHECK(extended_target.suffix == source.suffix);
  }

  TEST_CASE("serializes wide scalar values") {
    SomeipWideScalars source;
    source.unsigned32 = 0x01234567U;
    source.unsigned64 = 0x0123456789ABCDEFULL;
    source.signed8 = -2;
    source.signed16 = -3;
    source.signed64 = -4;
    source.ratio = 1.0;

    vlink::Bytes data;
    REQUIRE((source >> data));

    const std::array<uint8_t, 31> expected = {0x01, 0x23, 0x45, 0x67, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD,
                                              0xEF, 0xFE, 0xFF, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                              0xFC, 0x3F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    REQUIRE(data.size() == expected.size());
    CHECK(std::memcmp(data.data(), expected.data(), expected.size()) == 0);

    const auto golden = vlink::Bytes::shallow_copy(expected.data(), expected.size());
    SomeipWideScalars target;
    REQUIRE((target << golden));
    CHECK(target.unsigned32 == source.unsigned32);
    CHECK(target.unsigned64 == source.unsigned64);
    CHECK(target.signed8 == source.signed8);
    CHECK(target.signed16 == source.signed16);
    CHECK(target.signed64 == source.signed64);
    CHECK(target.ratio == source.ratio);
  }

  TEST_CASE("serializes bool vectors empty values and fixed arrays") {
    SomeipContainers source;
    source.flags = {true, false, true};
    source.fixed = {0x1234, 0xABCD};

    vlink::Bytes data;
    REQUIRE((source >> data));

    const std::array<uint8_t, 27> expected = {0x00, 0x00, 0x00, 0x03, 0x01, 0x00, 0x01, 0x00, 0x00,
                                              0x00, 0x04, 0xEF, 0xBB, 0xBF, 0x00, 0x00, 0x00, 0x00,
                                              0x00, 0x00, 0x00, 0x00, 0x04, 0x12, 0x34, 0xAB, 0xCD};
    REQUIRE(data.size() == expected.size());
    CHECK(std::memcmp(data.data(), expected.data(), expected.size()) == 0);

    const auto golden = vlink::Bytes::shallow_copy(expected.data(), expected.size());
    SomeipContainers target;
    REQUIRE((target << golden));
    CHECK(target.flags == source.flags);
    CHECK(target.text.empty());
    CHECK(target.values.empty());
    CHECK(target.fixed == source.fixed);

    target.flags.assign(5U, false);
    target.values = {0xAAU, 0xBBU};
    const size_t flag_capacity = target.flags.capacity();
    REQUIRE((target << golden));
    CHECK(target.flags == source.flags);
    CHECK(target.flags.capacity() == flag_capacity);
    CHECK(target.values.empty());
  }

  TEST_CASE("deserializes bool from the lowest bit") {
    SomeipScalars source;
    vlink::Bytes data;
    REQUIRE((source >> data));

    data.data()[11] = 0x02;
    SomeipScalars target;
    REQUIRE((target << data));
    CHECK_FALSE(target.enabled);

    data.data()[11] = 0x03;
    REQUIRE((target << data));
    CHECK(target.enabled);
  }

  TEST_CASE("serializes strings arrays nested structs and bytes") {
    SomeipMessage source;
    source.state = 0x7F;
    source.name = "abc";
    source.samples = {0x1234, 0xABCD};
    source.limits = {0x01020304, 0xA0B0C0D0};
    source.child.delta = -2;
    source.child.label = "x";
    source.payload = vlink::Bytes{0xDE, 0xAD};

    vlink::Bytes data;
    REQUIRE(Serializer::serialize(source, data));
    CHECK(Serializer::get_serialized_size(source) == 49);

    const std::array<uint8_t, 49> expected = {
        0x7F, 0x00, 0x00, 0x00, 0x07, 0xEF, 0xBB, 0xBF, 'a',  'b',  'c',  0x00, 0x00, 0x00, 0x00, 0x04, 0x12,
        0x34, 0xAB, 0xCD, 0x00, 0x00, 0x00, 0x08, 0x01, 0x02, 0x03, 0x04, 0xA0, 0xB0, 0xC0, 0xD0, 0xFF, 0xFE,
        0x00, 0x00, 0x00, 0x05, 0xEF, 0xBB, 0xBF, 'x',  0x00, 0x00, 0x00, 0x00, 0x02, 0xDE, 0xAD};
    REQUIRE(data.size() == expected.size());
    CHECK(std::memcmp(data.data(), expected.data(), expected.size()) == 0);

    const auto golden = vlink::Bytes::shallow_copy(expected.data(), expected.size());
    SomeipMessage target;
    REQUIRE(Serializer::deserialize(golden, target));
    CHECK(target.state == source.state);
    CHECK(target.name == source.name);
    CHECK(target.samples == source.samples);
    CHECK(target.limits == source.limits);
    CHECK(target.child.delta == source.child.delta);
    CHECK(target.child.label == source.child.label);
    CHECK(target.payload == source.payload);
  }

  TEST_CASE("writes into an exact transport loan through serializer dispatch") {
    SomeipScalars source;
    source.word = 0x1234;

    std::array<uint8_t, 14> storage{};
    vlink::Bytes data;
    bool loan_called = false;
    size_t requested_size = 0;

    REQUIRE(Serializer::serialize_to_transport<Serializer::kSomeipType>(
        source, data, TransportType::kShm2, true, [&storage, &loan_called, &requested_size](size_t size) {
          loan_called = true;
          requested_size = size;

          if (size != storage.size()) {
            return vlink::Bytes{};
          }

          return vlink::Bytes::loan_internal(storage.data(), size);
        }));
    CHECK(loan_called);
    CHECK(requested_size == storage.size());
    CHECK(data.is_loaned());
    CHECK(data.data() == storage.data());

    const std::array<uint8_t, 14> expected = {0x00, 0x12, 0x34, 0x00, 0x00, 0x00, 0x00,
                                              0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x34};
    CHECK(std::memcmp(storage.data(), expected.data(), expected.size()) == 0);
  }

  TEST_CASE("does not overwrite a shallow destination alias") {
    SomeipScalars source;
    source.word = 0x1234;

    std::array<uint8_t, 14> storage{};
    storage.fill(0xCC);
    const auto original = storage;
    auto data = vlink::Bytes::shallow_copy(storage.data(), storage.size());

    REQUIRE(vlink::SomeipSerializer::serialize(source, data));
    CHECK(data.is_owner());
    CHECK(data.data() != storage.data());
    CHECK(storage == original);
  }

  TEST_CASE("writes directly after a requested payload offset") {
    SomeipScalars source;
    source.byte = 0x42;

    vlink::Bytes data;
    REQUIRE(Serializer::serialize<Serializer::kSomeipType>(source, data, TransportType::kUnknown, 4U));
    CHECK(data.is_owner());
    CHECK(data.offset() == 4U);
    CHECK(data.size() == 14U);
    CHECK(data.data()[0] == source.byte);
  }

  TEST_CASE("rejects a nonzero offset for zero-offset loaned storage") {
    SomeipScalars source;
    std::array<uint8_t, 14> storage{};
    auto data = vlink::Bytes::loan_internal(storage.data(), storage.size());

    CHECK_FALSE(Serializer::serialize<Serializer::kSomeipType>(source, data, TransportType::kUnknown, 4U));
    CHECK(data.is_loaned());
    CHECK(data.data() == storage.data());
  }

  TEST_CASE("supports shared ptr round trip") {
    auto source = std::make_shared<SomeipChild>();
    source->delta = -8;
    source->label = "shared";

    vlink::Bytes data;
    REQUIRE(Serializer::serialize(source, data));

    auto target = std::make_shared<SomeipChild>();
    REQUIRE(Serializer::deserialize(data, target));
    CHECK(target->delta == source->delta);
    CHECK(target->label == source->label);
  }

  TEST_CASE("rejects null shared ptr sources and destinations") {
    const std::shared_ptr<SomeipChild> source;
    vlink::Bytes data;

    CHECK(Serializer::get_serialized_size(source) == 0U);
    CHECK_FALSE(Serializer::serialize(source, data));

    const vlink::Bytes encoded{0x00, 0x00, 0x00, 0x00, 0x04, 0xEF, 0xBB, 0xBF, 0x00};
    std::shared_ptr<SomeipChild> target;
    CHECK_FALSE(Serializer::deserialize(encoded, target));
  }

  TEST_CASE("reuses pool backed output and bytes field capacity") {
    SomeipMessage source;
    source.payload = vlink::Bytes::create(256U);
    REQUIRE_FALSE(source.payload.empty());
    std::memset(source.payload.data(), 0xA5, source.payload.size());

    vlink::Bytes data;
    REQUIRE((source >> data));
    auto* output_storage = data.real_data();
    REQUIRE(output_storage != nullptr);

    REQUIRE(source.payload.shrink_to(128U));
    const size_t expected_size = Serializer::get_serialized_size(source);
    REQUIRE((source >> data));
    CHECK(data.real_data() == output_storage);
    CHECK(data.size() == expected_size);

    SomeipMessage target;
    target.payload = vlink::Bytes::create(512U);
    auto* field_storage = target.payload.real_data();
    REQUIRE(field_storage != nullptr);

    REQUIRE((target << data));
    CHECK(target.payload.real_data() == field_storage);
    CHECK(target.payload == source.payload);
  }

  TEST_CASE("rejects a truncated payload") {
    SomeipMessage target;

    const vlink::Bytes truncated{0x01, 0x00, 0x00, 0x00, 0x04, 0xEF, 0xBB};
    CHECK_FALSE(Serializer::deserialize(truncated, target));
  }

  TEST_CASE("accepts compatible fields appended to a top level structure") {
    SomeipScalars source;
    source.byte = 0x42;

    vlink::Bytes valid;
    REQUIRE(Serializer::serialize(source, valid));

    auto extended = vlink::Bytes::create(valid.size() + 1U);
    std::memcpy(extended.data(), valid.data(), valid.size());
    extended.data()[valid.size()] = 0xAA;

    SomeipScalars target;
    REQUIRE(Serializer::deserialize(extended, target));
    CHECK(target.byte == source.byte);
  }

  TEST_CASE("rejects invalid utf8 strings") {
    SomeipChild source;
    source.label = std::string{"\xC0\xAF", 2};

    vlink::Bytes data;
    CHECK_FALSE((source >> data));

    const vlink::Bytes invalid_bom{0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0xBB, 0xBF, 'x', 0x00};
    SomeipChild target;
    target.label = "unchanged";
    CHECK_FALSE((target << invalid_bom));
    CHECK(target.label == "unchanged");

    const vlink::Bytes invalid_content{0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0xEF, 0xBB, 0xBF, 0xC0, 0xAF, 0x00};
    CHECK_FALSE((target << invalid_content));
  }

  TEST_CASE("accepts valid multibyte utf8 and rejects incomplete framing") {
    SomeipText source;
    source.value = std::string{"\xC2\xA2\xE4\xB8\xAD\xF0\x9F\x98\x80", 9};

    vlink::Bytes data;
    REQUIRE((source >> data));

    const std::array<uint8_t, 17> expected = {0x00, 0x00, 0x00, 0x0D, 0xEF, 0xBB, 0xBF, 0xC2, 0xA2,
                                              0xE4, 0xB8, 0xAD, 0xF0, 0x9F, 0x98, 0x80, 0x00};
    REQUIRE(data.size() == expected.size());
    CHECK(std::memcmp(data.data(), expected.data(), expected.size()) == 0);

    const auto golden = vlink::Bytes::shallow_copy(expected.data(), expected.size());
    SomeipText target;
    REQUIRE((target << golden));
    CHECK(target.value == source.value);

    source.value = std::string{"\xE4\xB8", 2};
    CHECK_FALSE((source >> data));

    const vlink::Bytes missing_terminator{0x00, 0x00, 0x00, 0x04, 0xEF, 0xBB, 0xBF, 'a'};
    CHECK_FALSE((target << missing_terminator));

    source.value = std::string{"a\0b", 3};
    REQUIRE((source >> data));

    const std::array<uint8_t, 11> embedded_null = {0x00, 0x00, 0x00, 0x07, 0xEF, 0xBB, 0xBF, 'a', 0x00, 'b', 0x00};
    REQUIRE(data.size() == embedded_null.size());
    CHECK(std::memcmp(data.data(), embedded_null.data(), embedded_null.size()) == 0);

    REQUIRE((target << data));
    CHECK(target.value == source.value);
  }

  TEST_CASE("does not duplicate a leading byte order mark in a dynamic string") {
    SomeipText utf8_source;
    utf8_source.value = std::string{
        "\xEF\xBB\xBF"
        "A",
        4};

    vlink::Bytes utf8_data;
    REQUIRE((utf8_source >> utf8_data));
    const std::array<uint8_t, 9> utf8_expected = {
        0x00, 0x00, 0x00, 0x05, 0xEF, 0xBB, 0xBF, 'A', 0x00,
    };
    REQUIRE(utf8_data.size() == utf8_expected.size());
    CHECK(std::memcmp(utf8_data.data(), utf8_expected.data(), utf8_expected.size()) == 0);

    SomeipText utf8_target;
    REQUIRE((utf8_target << utf8_data));
    CHECK(utf8_target.value == "A");

    SomeipWideDefault utf16_source;
    utf16_source.name = u"\uFEFFA";

    vlink::Bytes utf16_data;
    REQUIRE((utf16_source >> utf16_data));
    const std::array<uint8_t, 10> utf16_expected = {
        0x00, 0x00, 0x00, 0x06, 0xFE, 0xFF, 0x00, 0x41, 0x00, 0x00,
    };
    REQUIRE(utf16_data.size() == utf16_expected.size());
    CHECK(std::memcmp(utf16_data.data(), utf16_expected.data(), utf16_expected.size()) == 0);

    SomeipWideDefault utf16_target;
    REQUIRE((utf16_target << utf16_data));
    CHECK(utf16_target.name == u"A");
  }

  TEST_CASE("enforces UTF-8 scalar boundary sequences") {
    SomeipText source;
    vlink::Bytes data;

    const std::array<std::string, 4> valid = {std::string{"\xE0\xA0\x80", 3}, std::string{"\xED\x9F\xBF", 3},
                                              std::string{"\xF0\x90\x80\x80", 4}, std::string{"\xF4\x8F\xBF\xBF", 4}};
    for (const auto& value : valid) {
      source.value = value;
      REQUIRE((source >> data));

      SomeipText target;
      REQUIRE((target << data));
      CHECK(target.value == value);
    }

    const std::array<std::string, 5> invalid = {std::string{"\xE0\x9F\x80", 3}, std::string{"\xED\xA0\x80", 3},
                                                std::string{"\xF0\x8F\xBF\xBF", 4}, std::string{"\xF4\x90\x80\x80", 4},
                                                std::string{"\xE1\x80\x41", 3}};
    for (const auto& value : invalid) {
      source.value = value;
      CHECK_FALSE((source >> data));

      data = vlink::Bytes::create(value.size() + 8U);
      REQUIRE_FALSE(data.empty());
      const uint32_t length = static_cast<uint32_t>(value.size() + 4U);
      data.data()[0] = static_cast<uint8_t>(length >> 24U);
      data.data()[1] = static_cast<uint8_t>(length >> 16U);
      data.data()[2] = static_cast<uint8_t>(length >> 8U);
      data.data()[3] = static_cast<uint8_t>(length);
      data.data()[4] = 0xEF;
      data.data()[5] = 0xBB;
      data.data()[6] = 0xBF;
      std::memcpy(data.data() + 7U, value.data(), value.size());
      data.data()[data.size() - 1U] = 0x00;

      SomeipText target;
      CHECK_FALSE((target << data));
    }
  }

  TEST_CASE("rejects a fixed array with too few serialized elements") {
    const vlink::Bytes malformed{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0xEF, 0xBB, 0xBF,
                                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x12, 0x34};

    SomeipContainers target;
    CHECK_FALSE((target << malformed));
  }

  TEST_CASE("skips unknown fixed array elements before decoding the next field") {
    const vlink::Bytes extended{0x00, 0x00, 0x00, 0x06, 0x12, 0x34, 0xAB, 0xCD, 0x56, 0x78, 0x9A};

    SomeipFixedWithTail target;
    REQUIRE((target << extended));
    CHECK(target.fixed == std::array<uint16_t, 2>{0x1234, 0xABCD});
    CHECK(target.tail == 0x9A);
  }

  TEST_CASE("serializes nested arrays and variable length structure elements") {
    SomeipNestedContainers source;
    source.matrix = {{{0x0001, 0x0002}, {0x0003, 0x0004}}};
    source.children = {{0x0001, "a"}, {-2, "bc"}};

    vlink::Bytes data;
    REQUIRE((source >> data));

    const std::array<uint8_t, 47> expected = {0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02,
                                              0x00, 0x00, 0x00, 0x04, 0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x00, 0x17,
                                              0x00, 0x01, 0x00, 0x00, 0x00, 0x05, 0xEF, 0xBB, 0xBF, 'a',  0x00, 0xFF,
                                              0xFE, 0x00, 0x00, 0x00, 0x06, 0xEF, 0xBB, 0xBF, 'b',  'c',  0x00};
    REQUIRE(data.size() == expected.size());
    CHECK(std::memcmp(data.data(), expected.data(), expected.size()) == 0);

    const auto golden = vlink::Bytes::shallow_copy(expected.data(), expected.size());
    SomeipNestedContainers target;
    REQUIRE((target << golden));
    CHECK(target.matrix == source.matrix);
    REQUIRE(target.children.size() == source.children.size());
    CHECK(target.children[0].delta == source.children[0].delta);
    CHECK(target.children[0].label == source.children[0].label);
    CHECK(target.children[1].delta == source.children[1].delta);
    CHECK(target.children[1].label == source.children[1].label);
  }

  TEST_CASE("serializes a fixed array of nested structures") {
    SomeipChildArray source;
    source.children = {{{1, "a"}, {-2, "bc"}}};

    vlink::Bytes data;
    REQUIRE((source >> data));

    const std::array<uint8_t, 27> expected = {0x00, 0x00, 0x00, 0x17, 0x00, 0x01, 0x00, 0x00, 0x00,
                                              0x05, 0xEF, 0xBB, 0xBF, 'a',  0x00, 0xFF, 0xFE, 0x00,
                                              0x00, 0x00, 0x06, 0xEF, 0xBB, 0xBF, 'b',  'c',  0x00};
    REQUIRE(data.size() == expected.size());
    CHECK(std::memcmp(data.data(), expected.data(), expected.size()) == 0);

    const auto golden = vlink::Bytes::shallow_copy(expected.data(), expected.size());
    SomeipChildArray target;
    REQUIRE((target << golden));
    CHECK(target.children[0].delta == source.children[0].delta);
    CHECK(target.children[0].label == source.children[0].label);
    CHECK(target.children[1].delta == source.children[1].delta);
    CHECK(target.children[1].label == source.children[1].label);
  }

  TEST_CASE("serializes every dimension of a dynamic array with its own byte length") {
    SomeipDynamicMatrix source;
    source.values = {{0x0001, 0x0002}, {0x0003}};

    vlink::Bytes data;
    REQUIRE((source >> data));

    const std::array<uint8_t, 18> expected = {0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00, 0x04, 0x00,
                                              0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x03};
    REQUIRE(data.size() == expected.size());
    CHECK(std::memcmp(data.data(), expected.data(), expected.size()) == 0);

    SomeipDynamicMatrix target;
    REQUIRE((target << data));
    CHECK(target.values == source.values);
  }

  TEST_CASE("uses configured length widths for nested array dimensions") {
    SomeipArrayLengths source;
    source.dynamic = {{0x0001, 0x0002}, {0x0003}};
    source.fixed = {{{0x04, 0x05}, {0x06, 0x07}}};
    source.partial = {{0x08, 0x09}};
    source.children = {{{0x0A, 0x0B, 0x0C}}};

    vlink::Bytes data;
    REQUIRE((source >> data));

    const std::array<uint8_t, 30> expected = {0x0A, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00, 0x02, 0x00,
                                              0x03, 0x02, 0x04, 0x05, 0x02, 0x06, 0x07, 0x06, 0x00, 0x00,
                                              0x00, 0x02, 0x08, 0x09, 0x05, 0x00, 0x03, 0x0A, 0x0B, 0x0C};
    REQUIRE(data.size() == expected.size());
    CHECK(std::memcmp(data.data(), expected.data(), expected.size()) == 0);

    SomeipArrayLengths target;
    REQUIRE((target << data));
    CHECK(target.dynamic == source.dynamic);
    CHECK(target.fixed == source.fixed);
    CHECK(target.partial == source.partial);
    REQUIRE(target.children.size() == source.children.size());
    CHECK(target.children[0].values == source.children[0].values);
  }

  TEST_CASE("aligns after a configured multidimensional array") {
    SomeipAlignedArrayLengths source;
    source.prefix = 0xAAU;
    source.values = {{0x01U, 0x02U}};
    source.suffix = 0xBBU;

    vlink::Bytes data;
    REQUIRE((source >> data));

    const std::array<uint8_t, 9> expected = {0xAA, 0x03, 0x02, 0x01, 0x02, 0x00, 0x00, 0x00, 0xBB};
    REQUIRE(data.size() == expected.size());
    CHECK(std::memcmp(data.data(), expected.data(), expected.size()) == 0);

    SomeipAlignedArrayLengths target;
    REQUIRE((target << data));
    CHECK(target.prefix == source.prefix);
    CHECK(target.values == source.values);
    CHECK(target.suffix == source.suffix);
  }

  TEST_CASE("rejects dynamic arrays whose elements encode to zero bytes") {
    SomeipZeroByteArrayElements source;
    source.values.resize(1U);

    vlink::Bytes data;
    CHECK_FALSE((source >> data));
  }

  TEST_CASE("reuses storage inside existing dynamic array elements") {
    SomeipBytesItems source;
    source.values.resize(1U);
    source.values[0].value = vlink::Bytes::create(128U);
    REQUIRE_FALSE(source.values[0].value.empty());
    std::memset(source.values[0].value.data(), 0xA5, source.values[0].value.size());

    vlink::Bytes data;
    REQUIRE((source >> data));

    SomeipBytesItems target;
    target.values.resize(1U);
    target.values[0].value = vlink::Bytes::create(512U);
    auto* storage = target.values[0].value.real_data();
    REQUIRE(storage != nullptr);

    REQUIRE((target << data));
    CHECK(target.values.size() == 1U);
    CHECK(target.values[0].value.real_data() == storage);
    CHECK(target.values[0].value == source.values[0].value);

    REQUIRE((target << data));
    CHECK(target.values[0].value.real_data() == storage);
  }

  TEST_CASE("rejects a dynamic array ending inside an element") {
    const vlink::Bytes malformed{0x00, 0x00, 0x00, 0x03, 0x12, 0x34, 0x56};

    SomeipVectorOnly target;
    CHECK_FALSE((target << malformed));
  }

  TEST_CASE("skips a partial excess dynamic array element after reaching the maximum") {
    const vlink::Bytes compatible{0x05, 0x00, 0x01, 0x00, 0x02, 0xAA, 0x7F};

    SomeipBoundedVectorWithTail target;
    REQUIRE((target << compatible));
    CHECK(target.values == std::vector<uint16_t>{0x0001U, 0x0002U});
    CHECK(target.tail == 0x7FU);

    const vlink::Bytes malformed{0x03, 0x00, 0x01, 0xAA, 0x7F};
    CHECK_FALSE((target << malformed));
  }

  TEST_CASE("normalizes a reused bytes field decoded from an empty array") {
    SomeipMessage source;
    source.payload.clear();

    vlink::Bytes data;
    REQUIRE((source >> data));

    SomeipMessage target;
    target.payload = vlink::Bytes::create(256U);
    REQUIRE_FALSE(target.payload.empty());

    REQUIRE((target << data));
    CHECK(target.payload.empty());
    CHECK(target.payload.size() == 0U);
  }

  TEST_CASE("rejects payloads that cannot be represented by the SOME/IP message length") {
    const uint8_t byte = 0U;
    const auto oversized = vlink::Bytes::shallow_copy(&byte, vlink::SomeipSerializer::kMaxPayloadSize + 1U);

    SomeipText target;
    CHECK_FALSE((target << oversized));
  }

  TEST_CASE("serializes map entries with a configured length width") {
    SomeipMapMessage source;
    source.table = {{0x0102U, "a"}, {0x0304U, "bc"}};
    source.suffix = 0x7FU;

    vlink::Bytes data;
    REQUIRE((source >> data));
    CHECK(source.get_serialized_size() == data.size());

    const std::array<uint8_t, 29> expected = {0x00, 0x17, 0x01, 0x02, 0x00, 0x00, 0x00, 0x05, 0xEF, 0xBB,
                                              0xBF, 'a',  0x00, 0x03, 0x04, 0x00, 0x00, 0x00, 0x06, 0xEF,
                                              0xBB, 0xBF, 'b',  'c',  0x00, 0x00, 0x00, 0x00, 0x7F};
    REQUIRE(data.size() == expected.size());
    CHECK(std::memcmp(data.data(), expected.data(), expected.size()) == 0);

    SomeipMapMessage target;
    target.table = {{0xFFFFU, "stale"}};
    REQUIRE((target << data));
    CHECK(target.table == source.table);
    CHECK(target.suffix == source.suffix);
  }

  TEST_CASE("round trips an unordered map and rejects duplicate keys") {
    SomeipHashedMap source;
    source.index = {{0x01020304U, 0x0506U}, {0x0A0B0C0DU, 0x0E0FU}};

    vlink::Bytes data;
    REQUIRE((source >> data));

    SomeipHashedMap target;
    REQUIRE((target << data));
    CHECK(target.index == source.index);

    const vlink::Bytes duplicate{0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x01,
                                 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x03};
    CHECK_FALSE((target << duplicate));
  }

  TEST_CASE("serializes utf16 strings in both byte orders") {
    SomeipWideText source;
    source.name = u"AB";
    source.label = u"€";

    vlink::Bytes data;
    REQUIRE((source >> data));
    CHECK(source.get_serialized_size() == data.size());

    const std::array<uint8_t, 17> expected = {0x00, 0x08, 0xFE, 0xFF, 0x00, 0x41, 0x00, 0x42, 0x00,
                                              0x00, 0x06, 0xFF, 0xFE, 0xAC, 0x20, 0x00, 0x00};
    REQUIRE(data.size() == expected.size());
    CHECK(std::memcmp(data.data(), expected.data(), expected.size()) == 0);

    SomeipWideText target;
    REQUIRE((target << data));
    CHECK(target.name == source.name);
    CHECK(target.label == source.label);

    source.name = u"\U0001D11E";
    source.name.push_back(u'A');
    REQUIRE((source >> data));
    source.name.push_back(u'B');
    CHECK_FALSE((source >> data));
  }

  TEST_CASE("round trips utf16 surrogate pairs and rejects malformed input") {
    SomeipWideDefault source;
    source.name = u"\U0001d11e";

    vlink::Bytes data;
    REQUIRE((source >> data));

    SomeipWideDefault target;
    REQUIRE((target << data));
    CHECK(target.name == source.name);

    source.name.assign(1U, static_cast<char16_t>(0xD800U));
    CHECK_FALSE((source >> data));

    SomeipWideDefault malformed_target;
    const vlink::Bytes wrong_bom{0x00, 0x00, 0x00, 0x06, 0xFF, 0xFE, 0x00, 0x41, 0x00, 0x00};
    CHECK_FALSE((malformed_target << wrong_bom));

    const vlink::Bytes odd_length{0x00, 0x00, 0x00, 0x07, 0xFE, 0xFF, 0x00, 0x41, 0x00, 0x00, 0xAA};
    REQUIRE((malformed_target << odd_length));
    CHECK(malformed_target.name == u"A");

    const vlink::Bytes odd_without_terminator{0x00, 0x00, 0x00, 0x05, 0xFE, 0xFF, 0x41, 0x00, 0x00};
    CHECK_FALSE((malformed_target << odd_without_terminator));

    const vlink::Bytes no_terminator{0x00, 0x00, 0x00, 0x06, 0xFE, 0xFF, 0x00, 0x41, 0x00, 0x41};
    CHECK_FALSE((malformed_target << no_terminator));
  }

  TEST_CASE("serializes union alternatives with selector values") {
    SomeipUnionMessage source;
    source.choice = static_cast<uint16_t>(0x1234U);
    source.tail = 0x7FU;

    vlink::Bytes data;
    REQUIRE((source >> data));
    CHECK(source.get_serialized_size() == data.size());

    const std::array<uint8_t, 9> value_wire = {0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x12, 0x34, 0x7F};
    REQUIRE(data.size() == value_wire.size());
    CHECK(std::memcmp(data.data(), value_wire.data(), value_wire.size()) == 0);

    source.choice = std::string("a");
    REQUIRE((source >> data));

    const std::array<uint8_t, 16> string_wire = {0x00, 0x00, 0x00, 0x09, 0x00, 0x02, 0x00, 0x00,
                                                 0x00, 0x05, 0xEF, 0xBB, 0xBF, 'a',  0x00, 0x7F};
    REQUIRE(data.size() == string_wire.size());
    CHECK(std::memcmp(data.data(), string_wire.data(), string_wire.size()) == 0);

    SomeipUnionMessage target;
    REQUIRE((target << data));
    CHECK(target.choice == source.choice);
    CHECK(target.tail == source.tail);

    const vlink::Bytes unknown_selector{0x00, 0x00, 0x00, 0x02, 0x00, 0x03, 0x12, 0x34, 0x7F};
    CHECK_FALSE((target << unknown_selector));

    const vlink::Bytes null_selector{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F};
    CHECK_FALSE((target << null_selector));

    const vlink::Bytes padded{0x00, 0x00, 0x00, 0x03, 0x00, 0x01, 0x12, 0x34, 0x00, 0x7F};
    REQUIRE((target << padded));
    REQUIRE(std::holds_alternative<uint16_t>(target.choice));
    CHECK(std::get<uint16_t>(target.choice) == 0x1234U);
    CHECK(target.tail == 0x7FU);
  }

  TEST_CASE("encodes plain unions without monostate from selector one") {
    SomeipPlainUnion source;
    source.value = static_cast<uint8_t>(0x05U);

    vlink::Bytes data;
    REQUIRE((source >> data));

    const std::array<uint8_t, 9> first_wire = {0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x05};
    REQUIRE(data.size() == first_wire.size());
    CHECK(std::memcmp(data.data(), first_wire.data(), first_wire.size()) == 0);

    source.value = 0xAABBCCDDU;
    REQUIRE((source >> data));

    const std::array<uint8_t, 12> second_wire = {0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
                                                 0x00, 0x02, 0xAA, 0xBB, 0xCC, 0xDD};
    REQUIRE(data.size() == second_wire.size());
    CHECK(std::memcmp(data.data(), second_wire.data(), second_wire.size()) == 0);

    SomeipPlainUnion target;
    REQUIRE((target << data));
    CHECK(target.value == source.value);

    const vlink::Bytes null_selector{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    CHECK_FALSE((target << null_selector));
  }

  TEST_CASE("includes union element padding in its length") {
    SomeipAlignedUnion source;
    source.value = static_cast<uint16_t>(0x1234U);
    source.suffix = 0x7FU;

    vlink::Bytes data;
    REQUIRE((source >> data));

    const std::array<uint8_t, 13> expected = {0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
                                              0x01, 0x12, 0x34, 0x00, 0x00, 0x7F};
    REQUIRE(data.size() == expected.size());
    CHECK(std::memcmp(data.data(), expected.data(), expected.size()) == 0);

    SomeipAlignedUnion target;
    REQUIRE((target << data));
    CHECK(target.value == source.value);
    CHECK(target.suffix == source.suffix);
  }

  TEST_CASE("supports an explicit null union alternative") {
    SomeipNullableUnion source;
    source.suffix = 0x7FU;

    vlink::Bytes data;
    REQUIRE((source >> data));

    const std::array<uint8_t, 5> null_expected = {0x00, 0x00, 0x00, 0x00, 0x7F};
    REQUIRE(data.size() == null_expected.size());
    CHECK(std::memcmp(data.data(), null_expected.data(), null_expected.size()) == 0);

    SomeipNullableUnion target;
    target.value = uint16_t{0x1234U};
    REQUIRE((target << data));
    CHECK(std::holds_alternative<std::monostate>(target.value));
    CHECK(target.suffix == source.suffix);

    source.value = uint16_t{0x1234U};
    REQUIRE((source >> data));
    const std::array<uint8_t, 9> value_expected = {0x00, 0x05, 0x01, 0x12, 0x34, 0x00, 0x00, 0x00, 0x7F};
    REQUIRE(data.size() == value_expected.size());
    CHECK(std::memcmp(data.data(), value_expected.data(), value_expected.size()) == 0);
    REQUIRE((target << data));
    CHECK(std::get<uint16_t>(target.value) == 0x1234U);

    const vlink::Bytes null_with_payload{0x00, 0x01, 0x00, 0x00, 0x7F};
    CHECK_FALSE((target << null_with_payload));
  }

  TEST_CASE("supports equal-size union alternatives without a length field") {
    SomeipUnframedUnion source;
    source.value = uint16_t{0x1234U};
    source.suffix = 0x7FU;

    vlink::Bytes data;
    REQUIRE((source >> data));
    const std::array<uint8_t, 4> expected = {0x01, 0x12, 0x34, 0x7F};
    REQUIRE(data.size() == expected.size());
    CHECK(std::memcmp(data.data(), expected.data(), expected.size()) == 0);

    SomeipUnframedUnion target;
    REQUIRE((target << data));
    CHECK(std::get<uint16_t>(target.value) == 0x1234U);
    CHECK(target.suffix == source.suffix);
  }

  TEST_CASE("serializes fixed UTF-8 and UTF-16 strings") {
    SomeipFixedStrings source;
    source.name = "A";
    source.title = u"A";

    vlink::Bytes data;
    REQUIRE((source >> data));

    const std::array<uint8_t, 17> expected = {0xEF, 0xBB, 0xBF, 0x41, 0x00, 0x00, 0x00, 0x00, 0x08,
                                              0xFF, 0xFE, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00};
    REQUIRE(data.size() == expected.size());
    CHECK(std::memcmp(data.data(), expected.data(), expected.size()) == 0);

    SomeipFixedStrings target;
    REQUIRE((target << data));
    CHECK(target.name == source.name);
    CHECK(target.title == source.title);

    source.name = "ABCDE";
    CHECK_FALSE((source >> data));

    const vlink::Bytes short_compatible{0xEF, 0xBB, 0xBF, 0x41, 0x00, 0x00, 0x00, 0x00,
                                        0x06, 0xFF, 0xFE, 0x41, 0x00, 0x00, 0x00};
    REQUIRE((target << short_compatible));
    CHECK(target.name == "A");
    CHECK(target.title == u"A");

    const vlink::Bytes truncated{0xEF, 0xBB, 0xBF, 0x41, 0x00};
    CHECK_FALSE((target << truncated));
  }

  TEST_CASE("serializes dynamic and static TLV fixed strings") {
    SomeipTlvFixedStrings source;
    source.name = "A";
    source.title = u"B";

    vlink::Bytes data;
    REQUIRE((source >> data));

    const std::array<uint8_t, 23> expected = {
        0x16, 0x50, 0x01, 0x08, 0xEF, 0xBB, 0xBF, 0x41, 0x00, 0x00, 0x00, 0x00,
        0x40, 0x02, 0x08, 0xFE, 0xFF, 0x00, 0x42, 0x00, 0x00, 0x00, 0x00,
    };
    REQUIRE(data.size() == expected.size());
    CHECK(std::memcmp(data.data(), expected.data(), expected.size()) == 0);

    SomeipTlvFixedStrings target;
    REQUIRE((target << data));
    CHECK(target.name == source.name);
    CHECK(target.title == source.title);

    const vlink::Bytes overlong{0x0C, 0x40, 0x02, 0x09, 0xFE, 0xFF, 0x00, 0x42, 0x00, 0x00, 0x00, 0x00, 0xAA};
    CHECK_FALSE((target << overlong));

    source.name.reset();
    REQUIRE((source >> data));
    const std::array<uint8_t, 12> absent = {0x0B, 0x40, 0x02, 0x08, 0xFE, 0xFF, 0x00, 0x42, 0x00, 0x00, 0x00, 0x00};
    REQUIRE(data.size() == absent.size());
    CHECK(std::memcmp(data.data(), absent.data(), absent.size()) == 0);
    REQUIRE((target << data));
    CHECK_FALSE(target.name.has_value());

    source.title = u"ABC";
    CHECK_FALSE((source >> data));
  }

  TEST_CASE("serializes tlv members with tags and optional omission") {
    SomeipTlvMessage source;
    source.id = 0x0102U;
    source.label = "a";
    source.data = {0xAAU, 0xBBU};

    vlink::Bytes data;
    REQUIRE((source >> data));
    CHECK(source.get_serialized_size() == data.size());

    const std::array<uint8_t, 19> expected = {0x00, 0x11, 0x10, 0x01, 0x01, 0x02, 0x50, 0x02, 0x05, 0xEF,
                                              0xBB, 0xBF, 'a',  0x00, 0x50, 0x03, 0x02, 0xAA, 0xBB};
    REQUIRE(data.size() == expected.size());
    CHECK(std::memcmp(data.data(), expected.data(), expected.size()) == 0);

    source.label.reset();
    REQUIRE((source >> data));
    CHECK(source.get_serialized_size() == data.size());

    const std::array<uint8_t, 11> omitted = {0x00, 0x09, 0x10, 0x01, 0x01, 0x02, 0x50, 0x03, 0x02, 0xAA, 0xBB};
    REQUIRE(data.size() == omitted.size());
    CHECK(std::memcmp(data.data(), omitted.data(), omitted.size()) == 0);

    SomeipTlvMessage target;
    target.label = "stale";
    REQUIRE((target << data));
    CHECK(target.id == source.id);
    CHECK_FALSE(target.label.has_value());
    CHECK(target.data == source.data);

    source.label = "ab";
    CHECK_FALSE((source >> data));
    source.label.reset();
    source.data.push_back(0xCCU);
    CHECK_FALSE((source >> data));

    const vlink::Bytes oversized_data{0x00, 0x0A, 0x10, 0x01, 0x01, 0x02, 0x50, 0x03, 0x03, 0xAA, 0xBB, 0xCC};
    REQUIRE((target << oversized_data));
    CHECK((target.data == std::vector<uint8_t>{0xAAU, 0xBBU}));

    const vlink::Bytes oversized_label{0x00, 0x12, 0x10, 0x01, 0x01, 0x02, 0x50, 0x02, 0x06, 0xEF,
                                       0xBB, 0xBF, 'a',  'b',  0x00, 0x50, 0x03, 0x02, 0xAA, 0xBB};
    CHECK_FALSE((target << oversized_label));
  }

  TEST_CASE("skips unknown tlv members and validates required ones") {
    SomeipTlvMessage target;

    const vlink::Bytes with_unknown{0x00, 0x11, 0x10, 0x01, 0x01, 0x02, 0x50, 0x07, 0x02, 0xDE,
                                    0xAD, 0x00, 0x08, 0x55, 0x50, 0x03, 0x02, 0xAA, 0xBB};
    REQUIRE((target << with_unknown));
    CHECK(target.id == 0x0102U);
    CHECK_FALSE(target.label.has_value());
    CHECK((target.data == std::vector<uint8_t>{0xAAU, 0xBBU}));

    const vlink::Bytes known_wire_four{0x00, 0x0A, 0x10, 0x01, 0x01, 0x02, 0x40, 0x03, 0x00, 0x02, 0xCC, 0xDD};
    REQUIRE((target << known_wire_four));
    CHECK((target.data == std::vector<uint8_t>{0xCCU, 0xDDU}));

    const vlink::Bytes unknown_wire_four{0x00, 0x0F, 0x10, 0x01, 0x01, 0x02, 0x40, 0x09, 0x00,
                                         0x02, 0xAA, 0xBB, 0x50, 0x03, 0x02, 0xAA, 0xBB};
    REQUIRE((target << unknown_wire_four));
    CHECK(target.id == 0x0102U);
    CHECK((target.data == std::vector<uint8_t>{0xAAU, 0xBBU}));

    const vlink::Bytes unknown_wire_four_at_edges{0x00, 0x15, 0x40, 0x09, 0x00, 0x02, 0xAA, 0xBB,
                                                  0x10, 0x01, 0x01, 0x02, 0x50, 0x03, 0x02, 0xCC,
                                                  0xDD, 0x40, 0x0A, 0x00, 0x02, 0xEE, 0xFF};
    REQUIRE((target << unknown_wire_four_at_edges));
    CHECK(target.id == 0x0102U);
    CHECK((target.data == std::vector<uint8_t>{0xCCU, 0xDDU}));

    const vlink::Bytes missing_required{0x00, 0x05, 0x50, 0x03, 0x02, 0xAA, 0xBB};
    CHECK_FALSE((target << missing_required));

    const vlink::Bytes duplicate_member{0x00, 0x0D, 0x10, 0x01, 0x01, 0x02, 0x10, 0x01,
                                        0x0A, 0x0B, 0x50, 0x03, 0x02, 0xAA, 0xBB};
    CHECK_FALSE((target << duplicate_member));

    const vlink::Bytes duplicate_optional{0x00, 0x19, 0x10, 0x01, 0x01, 0x02, 0x50, 0x02, 0x05,
                                          0xEF, 0xBB, 0xBF, 'a',  0x00, 0x50, 0x02, 0x05, 0xEF,
                                          0xBB, 0xBF, 'b',  0x00, 0x50, 0x03, 0x02, 0xAA, 0xBB};
    CHECK_FALSE((target << duplicate_optional));

    const vlink::Bytes wrong_wire_type{0x00, 0x08, 0x00, 0x01, 0x01, 0x50, 0x03, 0x02, 0xAA, 0xBB};
    CHECK_FALSE((target << wrong_wire_type));
  }

  TEST_CASE("round trips nested tlv structures") {
    SomeipTlvNested source;
    source.child.emplace();
    source.child->code = 0x5AU;
    source.tail = 0x7FU;

    vlink::Bytes data;
    REQUIRE((source >> data));
    CHECK(source.get_serialized_size() == data.size());

    const std::array<uint8_t, 10> expected = {0x09, 0x50, 0x01, 0x03, 0x00, 0x01, 0x5A, 0x00, 0x02, 0x7F};
    REQUIRE(data.size() == expected.size());
    CHECK(std::memcmp(data.data(), expected.data(), expected.size()) == 0);

    SomeipTlvNested target;
    REQUIRE((target << data));
    REQUIRE(target.child.has_value());
    CHECK(target.child->code == source.child->code);
    CHECK(target.tail == source.tail);

    const vlink::Bytes without_child{0x03, 0x00, 0x02, 0x7F};
    REQUIRE((target << without_child));
    CHECK_FALSE(target.child.has_value());
    CHECK(target.tail == 0x7FU);

    SomeipTlvChild top_level;
    top_level.code = 0x5AU;

    vlink::Bytes child_data;
    REQUIRE((top_level >> child_data));

    const std::array<uint8_t, 4> child_wire = {0x03, 0x00, 0x01, 0x5A};
    REQUIRE(child_data.size() == child_wire.size());
    CHECK(std::memcmp(child_data.data(), child_wire.data(), child_wire.size()) == 0);

    const vlink::Bytes reserved_tag{0x03, 0x80, 0x01, 0x5A};
    CHECK_FALSE((top_level << reserved_tag));
  }

  TEST_CASE("selects dynamic tlv length widths from the encoded body size") {
    SomeipTlvLengthMode source;
    source.data.resize(255U, 0xAAU);

    vlink::Bytes data;
    REQUIRE((source >> data));
    CHECK(data.data()[4] == 0x50U);
    CHECK(data.data()[6] == 0xFFU);

    source.data.resize(256U, 0xBBU);
    REQUIRE((source >> data));
    CHECK(data.data()[4] == 0x60U);
    CHECK(data.data()[6] == 0x01U);
    CHECK(data.data()[7] == 0x00U);

    source.data.resize(65535U, 0xCCU);
    REQUIRE((source >> data));
    CHECK(data.data()[4] == 0x60U);
    CHECK(data.data()[6] == 0xFFU);
    CHECK(data.data()[7] == 0xFFU);

    source.data.resize(65536U, 0xDDU);
    REQUIRE((source >> data));
    CHECK(data.data()[4] == 0x70U);
    CHECK(data.data()[6] == 0x00U);
    CHECK(data.data()[7] == 0x01U);
    CHECK(data.data()[8] == 0x00U);
    CHECK(data.data()[9] == 0x00U);

    SomeipTlvLengthMode target;
    REQUIRE((target << data));
    CHECK(target.data == source.data);
  }

  TEST_CASE("emits wire type four for static tlv lengths") {
    SomeipTlvStaticLength source;
    source.data = {0xAAU, 0xBBU};

    vlink::Bytes data;
    REQUIRE((source >> data));

    const std::array<uint8_t, 8> expected = {0x00, 0x06, 0x40, 0x01, 0x00, 0x02, 0xAA, 0xBB};
    REQUIRE(data.size() == expected.size());
    CHECK(std::memcmp(data.data(), expected.data(), expected.size()) == 0);

    SomeipTlvStaticLength target;
    REQUIRE((target << data));
    CHECK(target.data == source.data);
  }

  TEST_CASE("uses default widths for bare tlv fields") {
    SomeipTlvDefaults source;
    source.id = 0x0102U;
    source.data = {0xAAU, 0xBBU};

    vlink::Bytes data;
    REQUIRE((source >> data));

    const std::array<uint8_t, 13> expected = {0x00, 0x00, 0x00, 0x09, 0x10, 0x01, 0x01,
                                              0x02, 0x50, 0x02, 0x02, 0xAA, 0xBB};
    REQUIRE(data.size() == expected.size());
    CHECK(std::memcmp(data.data(), expected.data(), expected.size()) == 0);

    SomeipTlvDefaults target;
    REQUIRE((target << data));
    CHECK(target.id == source.id);
    CHECK(target.data == source.data);
  }

  TEST_CASE("inherits tlv length width in multidimensional arrays") {
    SomeipTlvMatrix source;
    source.values = {{0xAAU}, {0xBBU, 0xCCU}};

    vlink::Bytes data;
    REQUIRE((source >> data));

    const std::array<uint8_t, 9> expected = {0x08, 0x50, 0x01, 0x05, 0x01, 0xAA, 0x02, 0xBB, 0xCC};
    REQUIRE(data.size() == expected.size());
    CHECK(std::memcmp(data.data(), expected.data(), expected.size()) == 0);

    SomeipTlvMatrix target;
    REQUIRE((target << data));
    CHECK(target.values == source.values);
    CHECK_FALSE(target.extra.has_value());

    source.extra = std::vector<std::vector<uint8_t>>{{0xDDU}};
    REQUIRE((source >> data));
    REQUIRE((target << data));
    CHECK(target.extra == source.extra);
  }
}

TEST_SUITE("ser-predicates") {
  TEST_CASE("is_bytes_type is true only for Bytes") {
    CHECK(Serializer::is_bytes_type<Bytes>());
    CHECK_FALSE(Serializer::is_bytes_type<std::string>());
    CHECK_FALSE(Serializer::is_bytes_type<PodMsg>());
  }

  TEST_CASE("is_string_type is true only for std string") {
    CHECK(Serializer::is_string_type<std::string>());
    CHECK_FALSE(Serializer::is_string_type<Bytes>());
    CHECK_FALSE(Serializer::is_string_type<PodMsg>());
  }

  TEST_CASE("is_custom_type detects operator based codecs") {
    CHECK(Serializer::is_custom_type<CustomMsg>());
    CHECK(Serializer::is_custom_type<AnotherCustom>());
    CHECK_FALSE(Serializer::is_custom_type<PodMsg>());
    CHECK_FALSE(Serializer::is_custom_type<std::string>());
    CHECK_FALSE(Serializer::is_custom_type<Bytes>());
  }

  TEST_CASE("is_standard_type is true only for plain pod") {
    CHECK(Serializer::is_standard_type<PodMsg>());
    CHECK_FALSE(Serializer::is_standard_type<std::string>());
    CHECK_FALSE(Serializer::is_standard_type<Bytes>());
    CHECK_FALSE(Serializer::is_standard_type<CustomMsg>());
  }

  TEST_CASE("is_standard_ptr_type detects raw pod pointer") {
    CHECK(Serializer::is_standard_ptr_type<PodMsg*>());
    CHECK_FALSE(Serializer::is_standard_ptr_type<PodMsg>());
    CHECK_FALSE(Serializer::is_standard_ptr_type<std::string*>());
  }

  TEST_CASE("is_chars_type detects char pointer types") {
    CHECK(Serializer::is_chars_type<const char*>());
    CHECK(Serializer::is_chars_type<char*>());
    CHECK(Serializer::is_chars_type<decltype("literal")>());
    CHECK_FALSE(Serializer::is_chars_type<std::string>());
    CHECK_FALSE(Serializer::is_chars_type<std::string_view>());
    CHECK_FALSE(Serializer::is_chars_type<volatile char*>());
  }

  TEST_CASE("is_dynamic_type is false for all common types") {
    CHECK_FALSE(Serializer::is_dynamic_type<int>());
    CHECK_FALSE(Serializer::is_dynamic_type<std::string>());
    CHECK_FALSE(Serializer::is_dynamic_type<Bytes>());
    CHECK_FALSE(Serializer::is_dynamic_type<PodMsg>());
  }

  TEST_CASE("is_proto_type is false for non proto types") {
    CHECK_FALSE(Serializer::is_proto_type<int>());
    CHECK_FALSE(Serializer::is_proto_type<std::string>());
    CHECK_FALSE(Serializer::is_proto_type<PodMsg>());
  }

  TEST_CASE("is_proto_ptr_type is false for non proto pointer types") {
    CHECK_FALSE(Serializer::is_proto_ptr_type<int*>());
    CHECK_FALSE(Serializer::is_proto_ptr_type<PodMsg*>());
  }

  TEST_CASE("is_flat_table_type is false for non flat types") {
    CHECK_FALSE(Serializer::is_flat_table_type<int>());
    CHECK_FALSE(Serializer::is_flat_table_type<PodMsg>());
  }

  TEST_CASE("is_flat_builder_type is false for non builder types") {
    CHECK_FALSE(Serializer::is_flat_builder_type<int>());
    CHECK_FALSE(Serializer::is_flat_builder_type<std::string>());
  }

  TEST_CASE("is_flat_ptr_type is false for non flat ptr types") {
    CHECK_FALSE(Serializer::is_flat_ptr_type<int*>());
    CHECK_FALSE(Serializer::is_flat_ptr_type<PodMsg*>());
  }

  TEST_CASE("is_cdr_type is false for non cdr types") {
    CHECK_FALSE(Serializer::is_cdr_type<int>());
    CHECK_FALSE(Serializer::is_cdr_type<std::string>());
    CHECK_FALSE(Serializer::is_cdr_type<PodMsg>());
  }

  TEST_CASE("is_stream_type is false for bytes") { CHECK_FALSE(Serializer::is_stream_type<Bytes>()); }

  TEST_CASE("stream type is detected as a supported type") {
    static constexpr auto t = Serializer::get_type_of<StreamMsg>();
    CHECK(Serializer::is_supported(t));
  }
}

TEST_SUITE("ser-bytes") {
  TEST_CASE("bytes round trips through serialize and deserialize") {
    Bytes original{0x01, 0x02, 0x03, 0x04, 0x05};

    Bytes serialized;
    bool ok = Serializer::serialize(original, serialized);
    CHECK(ok);
    CHECK_FALSE(serialized.empty());
    CHECK(serialized.size() == original.size());

    Bytes result;
    bool dok = Serializer::deserialize(serialized, result);
    CHECK(dok);
    CHECK(result == original);
  }

  TEST_CASE("empty bytes round trips through serialize and deserialize") {
    Bytes empty{};

    Bytes serialized;
    Serializer::serialize(empty, serialized);

    Bytes result;
    Serializer::deserialize(serialized, result);
    CHECK(result.empty());
  }

  TEST_CASE("heap allocated bytes round trips correctly") {
    auto original = Bytes::create(8);

    for (size_t i = 0; i < 8; ++i) {
      original.data()[i] = static_cast<uint8_t>(i * 11);
    }

    Bytes serialized;
    REQUIRE(Serializer::serialize(original, serialized));

    Bytes result;
    REQUIRE(Serializer::deserialize(serialized, result));
    CHECK(result == original);
  }

  TEST_CASE("large bytes beyond stack size round trips correctly") {
    static constexpr size_t kLarge = 1024;
    auto original = Bytes::create(kLarge);

    for (size_t i = 0; i < kLarge; ++i) {
      original.data()[i] = static_cast<uint8_t>(i & 0xFF);
    }

    Bytes serialized;
    REQUIRE(Serializer::serialize(original, serialized));
    CHECK(serialized.size() == kLarge);

    Bytes result;
    REQUIRE(Serializer::deserialize(serialized, result));
    CHECK(result == original);
  }

  TEST_CASE("bytes created from string round trips correctly") {
    auto original = Bytes::from_string("test data");

    Bytes serialized;
    REQUIRE(Serializer::serialize(original, serialized));

    Bytes result;
    REQUIRE(Serializer::deserialize(serialized, result));
    CHECK(result == original);
  }

  TEST_CASE("get serialized size returns 0 for bytes") {
    Bytes b{0x01, 0x02, 0x03};
    CHECK(Serializer::get_serialized_size(b) == 0u);
  }

  TEST_CASE("get serialized type returns empty string for bytes") {
    CHECK(Serializer::get_serialized_type<Bytes>().empty());
  }
}

TEST_SUITE("ser-string") {
  TEST_CASE("string round trips through serialize and deserialize") {
    std::string original = "hello world";

    Bytes serialized;
    bool ok = Serializer::serialize(original, serialized);
    CHECK(ok);
    CHECK(serialized.size() == original.size());

    std::string result;
    bool dok = Serializer::deserialize(serialized, result);
    CHECK(dok);
    CHECK(result == original);
  }

  TEST_CASE("empty string round trips correctly") {
    std::string original;

    Bytes serialized;
    Serializer::serialize(original, serialized);

    std::string result;
    Serializer::deserialize(serialized, result);
    CHECK(result.empty());
  }

  TEST_CASE("string with special characters round trips correctly") {
    std::string original = "line1\nline2\ttab\r\n";

    Bytes serialized;
    REQUIRE(Serializer::serialize(original, serialized));

    std::string result;
    REQUIRE(Serializer::deserialize(serialized, result));
    CHECK(result == original);
  }

  TEST_CASE("long string round trips and preserves length") {
    std::string original(1024, 'x');

    Bytes serialized;
    REQUIRE(Serializer::serialize(original, serialized));
    CHECK(serialized.size() == 1024u);

    std::string result;
    REQUIRE(Serializer::deserialize(serialized, result));
    CHECK(result == original);
  }

  TEST_CASE("get serialized type returns string for std string") {
    CHECK(Serializer::get_serialized_type<std::string>() == "string");
  }

  TEST_CASE("get serialized size returns 0 for string") {
    std::string s = "hello";
    CHECK(Serializer::get_serialized_size(s) == 0u);
  }

  TEST_CASE("multiple strings serialize into independent buffers") {
    std::string s1 = "first";
    std::string s2 = "second";
    std::string s3 = "third";

    Bytes b1, b2, b3;
    CHECK(Serializer::serialize(s1, b1));
    CHECK(Serializer::serialize(s2, b2));
    CHECK(Serializer::serialize(s3, b3));

    CHECK(b1.size() == 5u);
    CHECK(b2.size() == 6u);
    CHECK(b3.size() == 5u);

    std::string r1, r2, r3;
    CHECK(Serializer::deserialize(b1, r1));
    CHECK(Serializer::deserialize(b2, r2));
    CHECK(Serializer::deserialize(b3, r3));
    CHECK(r1 == "first");
    CHECK(r2 == "second");
    CHECK(r3 == "third");
  }
}

TEST_SUITE("ser-chars") {
  TEST_CASE("chars keep the existing serialization wire format") {
    const char* original = "abc";
    Bytes serialized;

    REQUIRE(Serializer::serialize(original, serialized));
    REQUIRE_EQ(serialized.size(), 3u);
    CHECK(std::memcmp(serialized.data(), original, serialized.size()) == 0);
  }

  TEST_CASE("chars deserialization is rejected in favor of string ownership") {
    const std::array<uint8_t, 3> storage{'a', 'b', 'c'};
    const auto payload = Bytes::shallow_copy(storage.data(), storage.size());
    const Bytes empty;
    char* mutable_result = nullptr;
    const char* const_result = nullptr;

    CHECK_FALSE(Serializer::deserialize(payload, mutable_result));
    CHECK_FALSE(Serializer::deserialize(payload, const_result));
    CHECK_FALSE(Serializer::deserialize(empty, mutable_result));
    CHECK_FALSE(Serializer::deserialize(empty, const_result));
    CHECK(mutable_result == nullptr);
    CHECK(const_result == nullptr);
  }

  TEST_CASE("null chars source is rejected") {
    const char* source = nullptr;
    Bytes serialized;

    CHECK_FALSE(Serializer::serialize(source, serialized));
    CHECK(serialized.empty());
  }

  TEST_CASE("chars arrays stop at the first terminator") {
    const char source[]{'a', 'b', '\0', 'c'};
    Bytes serialized;

    REQUIRE(Serializer::serialize(source, serialized));
    REQUIRE_EQ(serialized.size(), 2u);
    CHECK(std::memcmp(serialized.data(), source, serialized.size()) == 0);
  }

  TEST_CASE("unterminated chars arrays are rejected within their extent") {
    const char source[]{'a', 'b', 'c'};
    Bytes serialized;

    CHECK_FALSE(Serializer::serialize(source, serialized));
    CHECK(serialized.empty());
  }
}

TEST_SUITE("ser-stream") {
  TEST_CASE("consecutive stream conversions use independent local state") {
    const StreamMsg first{12};
    const StreamMsg second{345};
    Bytes first_bytes;
    Bytes second_bytes;

    REQUIRE(Serializer::serialize(first, first_bytes));
    REQUIRE(Serializer::serialize(second, second_bytes));

    StreamMsg first_result;
    StreamMsg second_result;
    REQUIRE(Serializer::deserialize(first_bytes, first_result));
    REQUIRE(Serializer::deserialize(second_bytes, second_result));
    CHECK(first_result.number == first.number);
    CHECK(second_result.number == second.number);
  }

  TEST_CASE("failed stream conversion does not affect the next conversion") {
    const Bytes invalid = Bytes::from_string("invalid");
    const Bytes valid = Bytes::from_string("42");
    StreamMsg result;

    CHECK_FALSE(Serializer::deserialize(invalid, result));
    REQUIRE(Serializer::deserialize(valid, result));
    CHECK(result.number == 42);
  }
}

TEST_SUITE("ser-pod") {
  TEST_CASE("pod struct round trips through serialize and deserialize") {
    PodMsg original{42, 3.14f, 2.718281828};

    Bytes serialized;
    bool ok = Serializer::serialize(original, serialized);
    CHECK(ok);
    CHECK(serialized.size() == sizeof(PodMsg));

    PodMsg result{};
    bool dok = Serializer::deserialize(serialized, result);
    CHECK(dok);
    CHECK(result.x == original.x);
    CHECK(result.y == doctest::Approx(original.y));
    CHECK(result.z == doctest::Approx(original.z));
  }

  TEST_CASE("zero value pod struct round trips correctly") {
    PodMsg original{0, 0.0f, 0.0};

    Bytes serialized;
    REQUIRE(Serializer::serialize(original, serialized));

    PodMsg result{99, 1.0f, 1.0};
    REQUIRE(Serializer::deserialize(serialized, result));
    CHECK(result.x == 0);
    CHECK(result.y == doctest::Approx(0.0f));
    CHECK(result.z == doctest::Approx(0.0));
  }

  TEST_CASE("pod struct with negative values round trips correctly") {
    PodMsg original{-1, -3.14f, -2.71828};

    Bytes serialized;
    REQUIRE(Serializer::serialize(original, serialized));

    PodMsg result{};
    REQUIRE(Serializer::deserialize(serialized, result));
    CHECK(result.x == -1);
    CHECK(result.y == doctest::Approx(-3.14f));
    CHECK(result.z == doctest::Approx(-2.71828));
  }

  TEST_CASE("serialized size of pod struct equals sizeof") {
    PodMsg pod{1, 2.0f, 3.0};
    Bytes serialized;
    REQUIRE(Serializer::serialize(pod, serialized));
    CHECK(serialized.size() == sizeof(PodMsg));
  }

  TEST_CASE("deserialize fails when buffer is too small for pod") {
    Bytes bad = Bytes::create(1);
    bad.data()[0] = 0xFF;

    PodMsg result{};
    bool ok = Serializer::deserialize(bad, result);
    CHECK_FALSE(ok);
  }

  TEST_CASE("get serialized size returns 0 for pod") {
    PodMsg pod{};
    CHECK(Serializer::get_serialized_size(pod) == 0u);
  }

  TEST_CASE("get serialized type for pod returns non empty name") {
    auto t = Serializer::get_serialized_type<PodMsg>();
    CHECK_FALSE(t.empty());
  }

  TEST_CASE("pod pointer type name normalizes to pointee type name") {
    auto value_name = Serializer::get_serialized_type<PodMsg>();
    auto pointer_name = Serializer::get_serialized_type<PodMsg*>();
    CHECK(pointer_name == value_name);
  }

  TEST_CASE("int type maps to kStandardType") {
    static constexpr auto t = Serializer::get_type_of<int>();
    CHECK(t == Serializer::kStandardType);
  }

  TEST_CASE("int round trips through serialize and deserialize") {
    int original = 42;
    Bytes serialized;
    bool ok = Serializer::serialize(original, serialized);
    CHECK(ok);
    CHECK(!serialized.empty());

    int result = 0;
    bool dok = Serializer::deserialize(serialized, result);
    CHECK(dok);
    CHECK(result == 42);
  }

  TEST_CASE("negative int round trips correctly") {
    int original = -12345;
    Bytes serialized;
    REQUIRE(Serializer::serialize(original, serialized));

    int result = 0;
    REQUIRE(Serializer::deserialize(serialized, result));
    CHECK(result == -12345);
  }
}

TEST_SUITE("ser-pod") {
  TEST_CASE("pod pointer round trips via shallow copy") {
    PodMsg original{7, 1.5f, 0.1};
    const PodMsg* src = &original;

    Bytes serialized;
    bool ok = Serializer::serialize(src, serialized);
    CHECK(ok);
    CHECK(serialized.size() == sizeof(PodMsg));

    PodMsg* dest = nullptr;
    bool dok = Serializer::deserialize(serialized, dest);
    CHECK(dok);
    REQUIRE(dest != nullptr);
    CHECK(dest->x == original.x);
    CHECK(dest->y == doctest::Approx(original.y));
  }

  TEST_CASE("shared ptr to pod round trips correctly") {
    auto sp = std::make_shared<PodMsg>();
    sp->x = 123;
    sp->y = 4.56f;
    sp->z = 7.89;

    Bytes serialized;
    bool ok = Serializer::serialize(sp, serialized);
    CHECK(ok);
    CHECK(serialized.size() == sizeof(PodMsg));

    auto sp_out = std::make_shared<PodMsg>();
    bool dok = Serializer::deserialize(serialized, sp_out);
    CHECK(dok);
    CHECK(sp_out->x == 123);
    CHECK(sp_out->y == doctest::Approx(4.56f));
    CHECK(sp_out->z == doctest::Approx(7.89));
  }
}

TEST_SUITE("ser-custom") {
  TEST_CASE("custom msg round trips through serialize and deserialize") {
    CustomMsg original;
    original.value = 0x12345678;

    Bytes serialized;
    bool ok = Serializer::serialize(original, serialized);
    CHECK(ok);
    CHECK(serialized.size() == sizeof(int32_t));

    CustomMsg result;
    bool dok = Serializer::deserialize(serialized, result);
    CHECK(dok);
    CHECK(result.value == original.value);
  }

  TEST_CASE("custom msg with negative value round trips correctly") {
    CustomMsg original;
    original.value = -999;

    Bytes serialized;
    REQUIRE(Serializer::serialize(original, serialized));

    CustomMsg result;
    REQUIRE(Serializer::deserialize(serialized, result));
    CHECK(result.value == -999);
  }

  TEST_CASE("another custom single byte type round trips correctly") {
    AnotherCustom original;
    original.byte = 0xAB;

    Bytes serialized;
    REQUIRE(Serializer::serialize(original, serialized));

    AnotherCustom result;
    REQUIRE(Serializer::deserialize(serialized, result));
    CHECK(result.byte == 0xAB);
  }

  TEST_CASE("get serialized size returns 0 for custom type") {
    CustomMsg m;
    m.value = 42;
    CHECK(Serializer::get_serialized_size(m) == 0u);
  }

  TEST_CASE("unknown-size custom codec skips the transport loan") {
    CustomMsg original{42};
    Bytes serialized;
    bool loan_called = false;

    REQUIRE(Serializer::serialize_to_transport<Serializer::kCustomType>(original, serialized, TransportType::kShm2,
                                                                        true, [&loan_called](size_t) {
                                                                          loan_called = true;
                                                                          return Bytes::create(16u);
                                                                        }));
    CHECK_FALSE(loan_called);
    CHECK(serialized.is_owner());
    CHECK_EQ(serialized.size(), sizeof(original.value));
  }

  TEST_CASE("sized custom codec writes into an exact transport loan") {
    SizedCustom original{0x12345678, false};
    std::array<uint8_t, sizeof(int32_t)> storage{};
    Bytes serialized;

    REQUIRE(Serializer::serialize_to_transport<Serializer::kCustomType>(
        original, serialized, TransportType::kShm2, true,
        [&storage](size_t size) { return Bytes::loan_internal(storage.data(), size); }));
    CHECK(serialized.is_loaned());
    CHECK_EQ(serialized.data(), storage.data());
    CHECK_EQ(serialized.size(), storage.size());
    CHECK(std::memcmp(serialized.data(), &original.value, sizeof(original.value)) == 0);
  }

  TEST_CASE("sized custom codec writes into exact owning transport storage") {
    SizedCustom original{0x12345678, false};
    Bytes serialized;

    REQUIRE(Serializer::serialize_to_transport<Serializer::kCustomType>(
        original, serialized, TransportType::kShm2, true, [](size_t size) { return Bytes::create(size); }));
    CHECK(serialized.is_owner());
    CHECK_FALSE(serialized.is_loaned());
    CHECK_EQ(serialized.size(), sizeof(original.value));
    CHECK(std::memcmp(serialized.data(), &original.value, sizeof(original.value)) == 0);
  }

  TEST_CASE("sized custom codec rejects an oversized transport loan") {
    SizedCustom original{42, false};
    std::array<uint8_t, sizeof(int32_t) + 1u> storage{};
    Bytes serialized;

    CHECK_FALSE(Serializer::serialize_to_transport<Serializer::kCustomType>(
        original, serialized, TransportType::kShm2, true,
        [&storage](size_t) { return Bytes::loan_internal(storage.data(), storage.size()); }));
    CHECK(serialized.is_loaned());
    CHECK_EQ(serialized.data(), storage.data());
    CHECK_EQ(serialized.size(), storage.size());
  }

  TEST_CASE("sized custom codec cannot replace a transport loan") {
    SizedCustom original{42, true};
    std::array<uint8_t, sizeof(int32_t)> storage{};
    Bytes serialized;

    CHECK_FALSE(Serializer::serialize_to_transport<Serializer::kCustomType>(
        original, serialized, TransportType::kShm2, true,
        [&storage](size_t size) { return Bytes::loan_internal(storage.data(), size); }));
    CHECK(serialized.is_loaned());
    CHECK_EQ(serialized.data(), storage.data());
    CHECK_EQ(serialized.size(), storage.size());
  }

  TEST_CASE("sized custom codec failure preserves the transport loan") {
    SizedCustom original{42, false, true};
    std::array<uint8_t, sizeof(int32_t)> storage{};
    Bytes serialized;

    CHECK_FALSE(Serializer::serialize_to_transport<Serializer::kCustomType>(
        original, serialized, TransportType::kShm2, true,
        [&storage](size_t size) { return Bytes::loan_internal(storage.data(), size); }));
    CHECK(serialized.is_loaned());
    CHECK_EQ(serialized.data(), storage.data());
    CHECK_EQ(serialized.size(), storage.size());
  }
}

TEST_SUITE("ser-convert") {
  TEST_CASE("bytes to bytes conversion is a shallow copy") {
    Bytes src{0xAA, 0xBB, 0xCC};
    Bytes dst;
    bool ok = Serializer::convert(src, dst);
    CHECK(ok);
    CHECK(dst == src);
  }

  TEST_CASE("string converts to bytes preserving content length") {
    std::string s = "convert_test";
    Bytes dst;
    bool ok = Serializer::convert(s, dst);
    CHECK(ok);
    CHECK(dst.size() == s.size());
  }

  TEST_CASE("bytes converts to string preserving content") {
    std::string expected = "round_trip";
    Bytes src = Bytes::from_string(expected);
    std::string dst;
    bool ok = Serializer::convert(src, dst);
    CHECK(ok);
    CHECK(dst == expected);
  }

  TEST_CASE("pod converts to bytes and back via convert") {
    PodMsg original{100, 2.5f, 9.99};
    Bytes bytes;
    bool ok = Serializer::convert(original, bytes);
    CHECK(ok);
    CHECK(bytes.size() == sizeof(PodMsg));

    PodMsg result{};
    bool dok = Serializer::convert(bytes, result);
    CHECK(dok);
    CHECK(result.x == 100);
    CHECK(result.y == doctest::Approx(2.5f));
  }
}

TEST_SUITE("ser-deref") {
  TEST_CASE("deref on value type returns the value itself") {
    int v = 42;
    auto& ref = Serializer::deref(v);
    CHECK(ref == 42);
  }

  TEST_CASE("deref on shared ptr returns the underlying value") {
    auto sp = std::make_shared<PodMsg>();
    sp->x = 99;
    auto& ref = Serializer::deref(sp);
    CHECK(ref.x == 99);
  }
}

TEST_SUITE("ser-zerocopy") {
  TEST_CASE("zerocopy pointer type name normalizes to pointee type name") {
    auto value_name = Serializer::get_serialized_type<vlink::zerocopy::RawData>();
    auto pointer_name = Serializer::get_serialized_type<vlink::zerocopy::RawData*>();
    CHECK(pointer_name == value_name);
  }
}

#ifdef VLINK_TEST_SUPPORT_PROTOBUF

TEST_SUITE("ser-proto") {
  TEST_CASE("protobuf message maps to kProtoType") {
    static constexpr auto t = Serializer::get_type_of<pb::Message>();
    CHECK(t == Serializer::kProtoType);
    CHECK(Serializer::is_supported(t));
  }

  TEST_CASE("protobuf message round trips through serialize and deserialize") {
    pb::Message original;
    original.set_value("test_proto");
    original.set_type(42);

    Bytes serialized;
    bool ok = Serializer::serialize(original, serialized);
    CHECK(ok);
    CHECK_FALSE(serialized.empty());

    pb::Message result;
    bool dok = Serializer::deserialize(serialized, result);
    CHECK(dok);
    CHECK(result.value() == "test_proto");
    CHECK(result.type() == 42u);
  }

  TEST_CASE("empty protobuf message round trips correctly") {
    pb::Message original;

    Bytes serialized;
    REQUIRE(Serializer::serialize(original, serialized));

    pb::Message result;
    result.set_value("old");
    bool dok = Serializer::deserialize(serialized, result);
    CHECK(dok);
    CHECK(result.value().empty());
  }
}

#endif  // VLINK_TEST_SUPPORT_PROTOBUF

#ifdef VLINK_TEST_SUPPORT_FLATBUFFERS

namespace {

Bytes finish_flat_message(const char* value) {
  common_test::FlatMessageBuilder builder(value);
  builder.fbb_.Finish(builder.Finish());
  return Bytes::deep_copy(builder.fbb_.GetBufferPointer(), builder.fbb_.GetSize());
}

}  // namespace

TEST_SUITE("ser-flatbuffers") {
  TEST_CASE("flatbuffers message maps to kFlatTableType") {
    static constexpr auto t = Serializer::get_type_of<fbs::MessageT>();
    CHECK(t == Serializer::kFlatTableType);
    CHECK(Serializer::is_supported(t));
  }

  TEST_CASE("flatbuffers message round trips through serialize and deserialize") {
    fbs::MessageT original;
    original.value = "test_fbs";
    original.type = 99;

    Bytes serialized;
    bool ok = Serializer::serialize(original, serialized);
    CHECK(ok);
    CHECK_FALSE(serialized.empty());

    fbs::MessageT result;
    bool dok = Serializer::deserialize(serialized, result);
    CHECK(dok);
    CHECK(result.value == "test_fbs");
    CHECK(result.type == 99u);
  }

  TEST_CASE("shared flatbuffers message round trips through serialize and deserialize") {
    auto original = std::make_shared<fbs::MessageT>();
    original->value = "shared_fbs";
    original->type = 100;

    Bytes serialized;
    REQUIRE(Serializer::serialize(original, serialized));

    auto result = std::make_shared<fbs::MessageT>();
    REQUIRE(Serializer::deserialize(serialized, result));
    CHECK(result->value == "shared_fbs");
    CHECK(result->type == 100u);
  }

  TEST_CASE("flatbuffers builder has no pre finish loan size hint") {
    common_test::FlatMessageBuilder builder("flat_builder");

    CHECK(Serializer::get_type_of<common_test::FlatMessageBuilder>() == Serializer::kFlatBuilderType);
    CHECK_EQ(Serializer::get_serialized_size(builder), 0u);
  }

  TEST_CASE("flatbuffers builder serializes to an owning copy") {
    const auto expected = finish_flat_message("flat_builder_loan");
    common_test::FlatMessageBuilder builder("flat_builder_loan");
    Bytes serialized;

    REQUIRE(Serializer::serialize<Serializer::kFlatBuilderType>(builder, serialized));
    CHECK(serialized.is_owner());
    CHECK_FALSE(serialized.is_loaned());
    CHECK_EQ(serialized.size(), expected.size());
    CHECK_EQ(serialized.size(), builder.fbb_.GetSize());
    CHECK_NE(serialized.data(), builder.fbb_.GetBufferPointer());
    CHECK(std::memcmp(serialized.data(), expected.data(), expected.size()) == 0);
  }

  TEST_CASE("flatbuffers builder rejects a loan without changing it or finishing") {
    std::array<uint8_t, 64> storage{};
    auto loan = Bytes::loan_internal(storage.data(), storage.size());
    auto* loan_data = loan.data();
    common_test::FlatMessageBuilder builder("flat_builder_mismatch");
    const auto size_before = builder.fbb_.GetSize();

    CHECK_FALSE(Serializer::serialize<Serializer::kFlatBuilderType>(builder, loan));
    CHECK(loan.is_loaned());
    CHECK_EQ(loan.data(), loan_data);
    CHECK_EQ(loan.size(), storage.size());
    CHECK_EQ(builder.fbb_.GetSize(), size_before);

    Bytes serialized;
    CHECK(Serializer::serialize<Serializer::kFlatBuilderType>(builder, serialized));
    CHECK_FALSE(serialized.empty());
  }

  TEST_CASE("flatbuffers builder transport path finishes before taking an exact loan") {
    common_test::FlatMessageBuilder builder("flat_builder_transport");
    std::vector<uint8_t> storage;
    Bytes serialized;
    size_t loan_calls = 0;

    REQUIRE(Serializer::serialize_to_transport<Serializer::kFlatBuilderType>(
        builder, serialized, TransportType::kShm2, true, [&storage, &loan_calls](size_t size) {
          ++loan_calls;
          storage.resize(size);
          return Bytes::loan_internal(storage.data(), storage.size());
        }));
    CHECK_EQ(loan_calls, 1u);
    CHECK(serialized.is_loaned());
    CHECK_EQ(serialized.size(), builder.fbb_.GetSize());
    CHECK_EQ(serialized.data(), storage.data());
    CHECK(std::memcmp(serialized.data(), builder.fbb_.GetBufferPointer(), serialized.size()) == 0);
  }

  TEST_CASE("flatbuffers builder transport path accepts exact owning fallback storage") {
    common_test::FlatMessageBuilder builder("flat_builder_fallback");
    Bytes serialized;

    REQUIRE(Serializer::serialize_to_transport<Serializer::kFlatBuilderType>(
        builder, serialized, TransportType::kZenoh, true, [](size_t size) { return Bytes::create(size); }));
    CHECK(serialized.is_owner());
    CHECK_FALSE(serialized.is_loaned());
    CHECK_EQ(serialized.size(), builder.fbb_.GetSize());
    CHECK(std::memcmp(serialized.data(), builder.fbb_.GetBufferPointer(), serialized.size()) == 0);
  }

  TEST_CASE("flatbuffers builder transport allocation failure keeps its final size") {
    common_test::FlatMessageBuilder builder("flat_builder_failed_loan");
    Bytes serialized;
    size_t requested_size = 0U;

    CHECK_FALSE(Serializer::serialize_to_transport<Serializer::kFlatBuilderType>(
        builder, serialized, TransportType::kShm2, true, [&requested_size](size_t size) {
          requested_size = size;
          return Bytes{};
        }));
    CHECK(requested_size > 0U);
    CHECK_EQ(builder.fbb_.GetSize(), requested_size);
    CHECK(serialized.empty());
  }
}

#endif  // VLINK_TEST_SUPPORT_FLATBUFFERS

// NOLINTEND
