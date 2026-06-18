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

#include "./extension/dynamic_data.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "../common_test.h"

namespace {}  // namespace

TEST_SUITE("extension-DynamicData") {
  TEST_CASE("static constexpr traits") {
    static constexpr bool kIsDd = DynamicData::is_vlink_dynamic_data();
    static constexpr uint8_t kOffset = DynamicData::get_offset();
    CHECK(kIsDd);
    CHECK_EQ(kOffset, 20u);
  }

  TEST_CASE("default construction yields empty state") {
    DynamicData dd;
    CHECK(dd.is_empty());
    CHECK(dd.get_type().empty());
    CHECK(dd.get_data().empty());
  }

  TEST_CASE("load and as round-trip for int") {
    SUBCASE("positive value") {
      DynamicData dd;
      dd.load("int", 42);
      CHECK_FALSE(dd.is_empty());
      CHECK_EQ(dd.as<int>(), 42);
    }
    SUBCASE("zero") {
      DynamicData dd;
      dd.load("int", 0);
      CHECK_EQ(dd.as<int>(), 0);
    }
    SUBCASE("negative value") {
      DynamicData dd;
      dd.load("int", -99);
      CHECK_EQ(dd.as<int>(), -99);
    }
  }

  TEST_CASE("load and as round-trip for double") {
    DynamicData dd;
    dd.load("dbl", 3.14);
    CHECK_EQ(dd.as<double>(), doctest::Approx(3.14));
  }

  TEST_CASE("load and as round-trip for float") {
    DynamicData dd;
    dd.load("float", 1.5f);
    CHECK_EQ(dd.as<float>(), doctest::Approx(1.5f));
  }

  TEST_CASE("load and as round-trip for bool") {
    DynamicData dd;
    dd.load("bool", true);
    CHECK(dd.as<bool>());
  }

  TEST_CASE("load and as round-trip for uint64_t") {
    DynamicData dd;
    uint64_t val = 0xFFFFFFFFFFFFFFFFULL;
    dd.load("u64", val);
    CHECK_EQ(dd.as<uint64_t>(), val);
  }

  TEST_CASE("load and as round-trip for std::string") {
    SUBCASE("non-empty string") {
      DynamicData dd;
      std::string s = "hello vlink";
      dd.load("str", s);
      CHECK_EQ(dd.as<std::string>(), "hello vlink");
    }
    SUBCASE("empty string") {
      DynamicData dd;
      std::string empty;
      dd.load("str", empty);
      CHECK(dd.as<std::string>().empty());
    }
    SUBCASE("string with special characters") {
      DynamicData dd;
      std::string s = "abc\n\t\r123";
      dd.load("str", s);
      CHECK_EQ(dd.as<std::string>(), s);
    }
  }

  TEST_CASE("convert populates output on success") {
    DynamicData dd;
    dd.load("int", 7);
    int out = 0;
    CHECK(dd.convert(out));
    CHECK_EQ(out, 7);
  }

  TEST_CASE("convert returns false on empty instance") {
    DynamicData dd;
    int val = 0;
    CHECK_FALSE(dd.convert(val));
    CHECK_EQ(val, 0);
  }

  TEST_CASE("get_type view contains the stored type name") {
    DynamicData dd;
    dd.load("custom_type", 42);
    CHECK_NE(dd.get_type().find("custom_type"), std::string_view::npos);
  }

  TEST_CASE("get_data is non-empty after load") {
    DynamicData dd;
    dd.load("int", 1);
    CHECK_FALSE(dd.get_data().empty());
  }

  TEST_CASE("load overwrites previous value") {
    DynamicData dd;
    dd.load("int", 1);
    dd.load("int", 2);
    CHECK_EQ(dd.as<int>(), 2);
  }

  TEST_CASE("load chaining returns reference to self") {
    DynamicData dd;
    DynamicData& ref = dd.load("int", 5);
    CHECK_EQ(&ref, &dd);
  }

  TEST_CASE("sequential loads with different types update type view") {
    DynamicData dd;
    dd.load("int", 10);
    CHECK_EQ(dd.as<int>(), 10);

    dd.load("dbl", 3.14);
    CHECK_EQ(dd.as<double>(), doctest::Approx(3.14));

    std::string s = "updated";
    dd.load("str", s);
    CHECK_EQ(dd.as<std::string>(), "updated");
  }

  TEST_CASE("operator== and operator!= compare raw buffer content") {
    DynamicData a;
    DynamicData b;
    a.load("int", 42);
    b.load("int", 42);
    CHECK(a == b);
    CHECK_FALSE(a != b);

    DynamicData c;
    c.load("int", 1);
    CHECK(a != c);
    CHECK_FALSE(a == c);
  }

  TEST_CASE("two default-constructed instances are equal") {
    DynamicData a;
    DynamicData b;
    CHECK(a == b);
  }

  TEST_CASE("loaded vs empty are not equal") {
    DynamicData a;
    DynamicData b;
    a.load("int", 5);
    CHECK(a != b);
  }

  TEST_CASE("operator== is reflexive") {
    DynamicData dd;
    dd.load("int", 42);
    CHECK(dd == dd);
  }

  TEST_CASE("operator!= for different types even with same numeric value") {
    DynamicData a;
    DynamicData b;
    a.load("int", 42);
    b.load("dbl", 42.0);
    CHECK(a != b);
  }

  TEST_CASE("copy construction produces equal copy") {
    DynamicData orig;
    orig.load("int", 99);

    DynamicData copy(orig);
    CHECK(copy == orig);
    CHECK_EQ(copy.as<int>(), 99);
  }

  TEST_CASE("copy assignment produces equal copy") {
    DynamicData orig;
    orig.load("int", 77);

    DynamicData copy;
    copy = orig;
    CHECK(copy == orig);
    CHECK_EQ(copy.as<int>(), 77);
  }

  TEST_CASE("move construction transfers data and leaves source empty") {
    DynamicData orig;
    orig.load("int", 55);

    DynamicData moved(std::move(orig));
    CHECK_FALSE(moved.is_empty());
    CHECK_EQ(moved.as<int>(), 55);
    CHECK(orig.is_empty());
  }

  TEST_CASE("move assignment transfers data") {
    DynamicData orig;
    orig.load("int", 33);

    DynamicData moved;
    moved = std::move(orig);
    CHECK_EQ(moved.as<int>(), 33);
  }

  TEST_CASE("operator>> serializes to bytes and operator<< deserializes back") {
    SUBCASE("int round-trip") {
      DynamicData orig;
      orig.load("int", 42);
      Bytes wire;
      bool ser_ok = (orig >> wire);
      CHECK(ser_ok);
      CHECK_FALSE(wire.empty());

      DynamicData copy;
      bool de_ok = (copy << wire);
      CHECK(de_ok);
      CHECK_EQ(copy.as<int>(), 42);
    }
    SUBCASE("double round-trip") {
      DynamicData orig;
      orig.load("dbl", 2.71828);
      Bytes wire;
      bool ser_ok = (orig >> wire);
      CHECK(ser_ok);

      DynamicData copy;
      bool de_ok = (copy << wire);
      CHECK(de_ok);
      CHECK_EQ(copy.as<double>(), doctest::Approx(2.71828));
    }
  }

  TEST_CASE("operator>> on empty instance returns false") {
    DynamicData dd;
    Bytes wire;
    bool ser_ok = (dd >> wire);
    CHECK_FALSE(ser_ok);
  }

  TEST_CASE("operator<< from empty bytes returns false") {
    DynamicData dd;
    Bytes empty;
    bool de_ok = (dd << empty);
    CHECK_FALSE(de_ok);
  }

  TEST_CASE("type view is preserved through wire serialization round-trip") {
    DynamicData orig;
    orig.load("named_int", 123);

    Bytes wire;
    bool ser_ok = (orig >> wire);
    CHECK(ser_ok);

    DynamicData copy;
    bool de_ok = (copy << wire);
    CHECK(de_ok);
    CHECK_NE(copy.get_type().find("named_int"), std::string_view::npos);
  }

  TEST_CASE("multiple wire round-trips preserve data") {
    DynamicData dd;
    dd.load("int", 42);

    for (int i = 0; i < 3; ++i) {
      Bytes wire;
      dd >> wire;

      DynamicData dd2;
      dd2 << wire;
      CHECK_EQ(dd2.as<int>(), 42);

      dd = dd2;
    }
  }
}

// NOLINTEND
