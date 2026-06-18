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

#include "./impl/intra_data.h"

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <type_traits>

#include "../common_test.h"

#ifdef VLINK_SUPPORT_INTRA

VLINK_INTRA_DATA_DECLARE(vlink::Bytes, BytesIntra)
VLINK_INTRA_DATA_DECLARE(std::string, StringIntra)

TEST_SUITE("impl-IntraData") {
  TEST_CASE("IntraDataType is default-constructible") {
    IntraDataType obj;
    (void)obj;
    CHECK(std::is_default_constructible_v<IntraDataType>);
  }

  TEST_CASE("IntraData alias is shared_ptr of IntraDataType") {
    CHECK((std::is_same_v<IntraData, std::shared_ptr<IntraDataType>>));
  }

  TEST_CASE("IntraDataType has a virtual destructor") { CHECK(std::has_virtual_destructor_v<IntraDataType>); }

  TEST_CASE("IntraData can hold the base type") {
    IntraData ptr = std::make_shared<IntraDataType>();
    CHECK(ptr != nullptr);
  }

  TEST_CASE("generated type derives from IntraDataType") {
    CHECK((std::is_base_of_v<IntraDataType, BytesIntraType>));
    CHECK((std::is_base_of_v<IntraDataType, StringIntraType>));
  }

  TEST_CASE("generated kValueType matches the target serializer type") {
    CHECK_EQ(BytesIntraType::kValueType, Serializer::get_type_of<Bytes>());
    CHECK_EQ(StringIntraType::kValueType, Serializer::get_type_of<std::string>());
  }

  TEST_CASE("generated kValueType is a supported serializer type") {
    CHECK(Serializer::is_supported(BytesIntraType::kValueType));
    CHECK(Serializer::is_supported(StringIntraType::kValueType));
  }

  TEST_CASE("get_serialized_type returns a stable value-type-dependent string") {
    const std::string s_string = StringIntraType::get_serialized_type();
    const std::string s_bytes = BytesIntraType::get_serialized_type();
    CHECK_EQ(s_string, StringIntraType::get_serialized_type());
    CHECK_EQ(s_bytes, BytesIntraType::get_serialized_type());
  }

  TEST_CASE("get_schema_type returns a valid SchemaType value") {
    static constexpr SchemaType s1 = BytesIntraType::get_schema_type();
    static constexpr SchemaType s2 = StringIntraType::get_schema_type();
    CHECK(SchemaData::is_valid_type(s1));
    CHECK(SchemaData::is_valid_type(s2));
  }

  TEST_CASE("default-constructed wrapper is empty (null)") {
    StringIntra w;
    CHECK(w.get() == nullptr);
  }

  TEST_CASE("create returns a non-null instance with default value") {
    StringIntra w = StringIntra::create();
    REQUIRE(w.get() != nullptr);
    CHECK(w->value.empty());
  }

  TEST_CASE("create returns independent instances") {
    StringIntra a = StringIntra::create();
    StringIntra b = StringIntra::create();
    REQUIRE(a.get() != nullptr);
    REQUIRE(b.get() != nullptr);
    CHECK(a.get() != b.get());

    a->value = "alpha";
    b->value = "beta";

    CHECK_EQ(a->value, "alpha");
    CHECK_EQ(b->value, "beta");
  }

  TEST_CASE("copy of wrapper shares the same underlying object") {
    StringIntra a = StringIntra::create();
    a->value = "shared";

    StringIntra b = a;
    REQUIRE_EQ(b.get(), a.get());
    CHECK_EQ(b->value, "shared");

    b->value = "modified";
    CHECK_EQ(a->value, "modified");
  }

  TEST_CASE("wrapper is constructible from a base shared_ptr") {
    std::shared_ptr<StringIntraType> base = std::make_shared<StringIntraType>();
    base->value = "from_base";

    StringIntra w(base);
    REQUIRE_EQ(w.get(), base.get());
    CHECK_EQ(w->value, "from_base");
  }

  TEST_CASE("serialize then deserialize round-trip preserves string value") {
    StringIntraType src;
    src.value = "round_trip_payload";

    Bytes bytes;
    REQUIRE(src.operator>>(bytes));

    StringIntraType dst;
    REQUIRE(dst.operator<<(bytes));
    CHECK_EQ(dst.value, "round_trip_payload");
  }

  TEST_CASE("empty string round-trip yields empty string") {
    StringIntraType src;
    Bytes bytes;
    REQUIRE(src.operator>>(bytes));

    StringIntraType dst;
    dst.value = "preset";
    REQUIRE(dst.operator<<(bytes));
    CHECK(dst.value.empty());
  }
}

#endif  // VLINK_SUPPORT_INTRA

// NOLINTEND
