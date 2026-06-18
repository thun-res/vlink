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

#include "./base/name_detector.h"

#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <vector>

#include "../common_test.h"

namespace {

struct MyStruct {};

class MyClass {};

template <typename TypeT>
struct MyTemplate {};

namespace detector_ns {
struct Nested {};
enum class NestedColor : int { kRed, kGreen, kBlue };
}  // namespace detector_ns

enum class Color : int { kRed = 0, kGreen = 1, kBlue = 2 };

enum class Signed8 : int8_t { kNeg = -3, kZero = 0, kPos = 5 };

enum class Wide : int { kSparseOne = 1, kSparseTen = 10, kSparseHundred = 100 };

enum Direction { kNorth, kSouth, kEast, kWest };

}  // namespace

namespace vlink::NameDetector::customize {
template <>
struct EnumRange<Wide> {
  inline static constexpr int kMin = 0;
  inline static constexpr int kMax = 200;
};
}  // namespace vlink::NameDetector::customize

TEST_SUITE("base-NameDetector") {
  TEST_CASE("is_support returns true for fundamental types") {
    CHECK(NameDetector::is_support<int>());
    CHECK(NameDetector::is_support<double>());
    CHECK(NameDetector::is_support<char>());
    CHECK(NameDetector::is_support<bool>());
    CHECK(NameDetector::is_support<void*>());
    CHECK(NameDetector::is_support<int*>());
  }

  TEST_CASE("is_support returns true for user-defined types") {
    CHECK(NameDetector::is_support<MyStruct>());
    CHECK(NameDetector::is_support<MyClass>());
    CHECK(NameDetector::is_support<Color>());
    CHECK(NameDetector::is_support<Direction>());
    CHECK(NameDetector::is_support<detector_ns::Nested>());
  }

  TEST_CASE("get returns non-empty name for fundamental types") {
    std::string_view int_name = NameDetector::get<int>();
    std::string_view double_name = NameDetector::get<double>();
    std::string_view char_name = NameDetector::get<char>();

    CHECK_FALSE(int_name.empty());
    CHECK_FALSE(double_name.empty());
    CHECK_FALSE(char_name.empty());
    CHECK(int_name.find("int") != std::string_view::npos);
    CHECK(double_name.find("double") != std::string_view::npos);
  }

  TEST_CASE("get distinguishes different fundamental types") {
    CHECK(NameDetector::get<int>() != NameDetector::get<double>());
    CHECK(NameDetector::get<int>() != NameDetector::get<char>());
    CHECK(NameDetector::get<int>() != NameDetector::get<float>());
  }

  TEST_CASE("get contains identifier for user struct and class") {
    CHECK(NameDetector::get<MyStruct>().find("MyStruct") != std::string_view::npos);
    CHECK(NameDetector::get<MyClass>().find("MyClass") != std::string_view::npos);
  }

  TEST_CASE("get strips struct or class prefix on Windows") {
#if defined(_WIN32) || defined(__CYGWIN__)
    std::string_view sv = NameDetector::get<MyStruct>();
    CHECK(sv.find("struct ") == std::string_view::npos);
    CHECK(sv.find("class ") == std::string_view::npos);
#else
    CHECK_FALSE(NameDetector::get<MyStruct>().empty());
#endif
  }

  TEST_CASE("get applies decay so cv and reference qualify the same") {
    CHECK(NameDetector::get<int>() == NameDetector::get<const int>());
    CHECK(NameDetector::get<int>() == NameDetector::get<int&>());
    CHECK(NameDetector::get<int>() == NameDetector::get<const int&>());
    CHECK(NameDetector::get<int>() == NameDetector::get<volatile int>());
  }

  TEST_CASE("get returns name for templated types containing identifier") {
    std::string_view sv = NameDetector::get<MyTemplate<int>>();
    CHECK_FALSE(sv.empty());
    CHECK(sv.find("MyTemplate") != std::string_view::npos);
  }

  TEST_CASE("get returns name for std::vector containing vector") {
    std::string_view sv = NameDetector::get<std::vector<int>>();
    CHECK_FALSE(sv.empty());
    CHECK(sv.find("vector") != std::string_view::npos);
  }

  TEST_CASE("get result is stable across calls sharing same data pointer") {
    std::string_view a = NameDetector::get<int>();
    std::string_view b = NameDetector::get<int>();
    CHECK_EQ(a, b);
    CHECK_EQ(a.data(), b.data());
    CHECK_EQ(a.size(), b.size());
  }

  TEST_CASE("get data pointer is null-terminated") {
    std::string_view sv = NameDetector::get<int>();
    REQUIRE_FALSE(sv.empty());
    CHECK_EQ(sv.data()[sv.size()], '\0');
  }

  TEST_CASE("get_enum returns identifier for scoped enum values") {
    CHECK_EQ(NameDetector::get_enum(Color::kRed), "kRed");
    CHECK_EQ(NameDetector::get_enum(Color::kGreen), "kGreen");
    CHECK_EQ(NameDetector::get_enum(Color::kBlue), "kBlue");
  }

  TEST_CASE("get_enum returns identifier for unscoped enum values") {
    CHECK_EQ(NameDetector::get_enum(kNorth), "kNorth");
    CHECK_EQ(NameDetector::get_enum(kSouth), "kSouth");
    CHECK_EQ(NameDetector::get_enum(kEast), "kEast");
    CHECK_EQ(NameDetector::get_enum(kWest), "kWest");
  }

  TEST_CASE("get_enum handles negative enumerator values") {
    CHECK_EQ(NameDetector::get_enum(Signed8::kNeg), "kNeg");
    CHECK_EQ(NameDetector::get_enum(Signed8::kZero), "kZero");
    CHECK_EQ(NameDetector::get_enum(Signed8::kPos), "kPos");
  }

  TEST_CASE("get_enum returns empty for value outside any named enumerator") {
    auto bogus = static_cast<Color>(99);
    CHECK(NameDetector::get_enum(bogus).empty());
  }

  TEST_CASE("get_enum returns empty for value outside default scanning range") {
    auto far_out = static_cast<Direction>(127);
    CHECK(NameDetector::get_enum(far_out).empty());
  }

  TEST_CASE("get_enum produces distinct names for distinct values") {
    CHECK(NameDetector::get_enum(Color::kRed) != NameDetector::get_enum(Color::kGreen));
    CHECK(NameDetector::get_enum(Color::kGreen) != NameDetector::get_enum(Color::kBlue));
  }

  TEST_CASE("get_enum honours customize EnumRange for sparse enum") {
    CHECK_EQ(NameDetector::get_enum(Wide::kSparseOne), "kSparseOne");
    CHECK_EQ(NameDetector::get_enum(Wide::kSparseTen), "kSparseTen");
    CHECK_EQ(NameDetector::get_enum(Wide::kSparseHundred), "kSparseHundred");
    CHECK(NameDetector::get_enum(static_cast<Wide>(50)).empty());
  }

  TEST_CASE("get_enum works for enums in nested namespaces") {
    CHECK_EQ(NameDetector::get_enum(detector_ns::NestedColor::kRed), "kRed");
    CHECK_EQ(NameDetector::get_enum(detector_ns::NestedColor::kGreen), "kGreen");
  }

  TEST_CASE("get and get_enum results are usable at compile time") {
    static constexpr std::string_view kIntName = NameDetector::get<int>();
    static_assert(!kIntName.empty(), "compile-time type name must be non-empty");

    static constexpr std::string_view kRedName = NameDetector::get_enum(Color::kRed);
    static_assert(kRedName == std::string_view{"kRed"}, "compile-time enum name must match");

    CHECK_FALSE(kIntName.empty());
    CHECK_EQ(kRedName, "kRed");
  }

  TEST_CASE("get_enum result is stable across repeated calls") {
    auto a = NameDetector::get_enum(Color::kRed);
    auto b = NameDetector::get_enum(Color::kRed);
    CHECK_EQ(a, b);
    CHECK_EQ(a.data(), b.data());
  }

  TEST_CASE("kCount counts enumerators within range") {
    using vlink::NameDetector::detail::kCount;
    CHECK_EQ(kCount<Color>, 3);
    CHECK(kCount<Signed8> >= 3);
    CHECK_EQ(kCount<Direction>, 4);
    CHECK_EQ(kCount<Wide>, 3);
  }
}

// NOLINTEND
