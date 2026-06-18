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

#include "./base/macros.h"

#include <doctest/doctest.h>

#include <stdexcept>
#include <type_traits>

#include "../common_test.h"

namespace {

class NoCopyClass {
 public:
  NoCopyClass() = default;
  VLINK_DISALLOW_COPY_AND_ASSIGN(NoCopyClass)
};

class MySingleton final {
  VLINK_SINGLETON_DECLARE(MySingleton)

 public:
  int value{0};

 private:
  MySingleton() = default;
};

}  // namespace

#define MY_TEST_TOKEN hello
#define MY_NUM_TOKEN 12345

TEST_SUITE("base-Macros") {
  TEST_CASE("VLINK_DISALLOW_COPY_AND_ASSIGN deletes copy ctor and copy assignment") {
    CHECK_FALSE(std::is_copy_constructible_v<NoCopyClass>);
    CHECK_FALSE(std::is_copy_assignable_v<NoCopyClass>);
    CHECK_FALSE(std::is_move_constructible_v<NoCopyClass>);
  }

  TEST_CASE("VLINK_SINGLETON_DECLARE get() returns the same address each time") {
    MySingleton& a = MySingleton::get();
    MySingleton& b = MySingleton::get();
    CHECK_EQ(&a, &b);
  }

  TEST_CASE("singleton value is mutable via the stable reference") {
    MySingleton& s = MySingleton::get();
    s.value = 42;
    CHECK_EQ(MySingleton::get().value, 42);
    s.value = 0;
  }

  TEST_CASE("singleton is not copyable") {
    CHECK_FALSE(std::is_copy_constructible_v<MySingleton>);
    CHECK_FALSE(std::is_copy_assignable_v<MySingleton>);
  }

  TEST_CASE("VLINK_MACRO_STRING_IFY stringifies token literally") {
    const char* s = VLINK_MACRO_STRING_IFY(hello);
    CHECK_EQ(std::string(s), "hello");
  }

  TEST_CASE("VLINK_MACRO_STRING_GET expands macro before stringifying") {
    const char* s = VLINK_MACRO_STRING_GET(MY_TEST_TOKEN);
    CHECK_EQ(std::string(s), "hello");
  }

  TEST_CASE("VLINK_MACRO_STRING_GET with numeric macro") {
    const char* s = VLINK_MACRO_STRING_GET(MY_NUM_TOKEN);
    CHECK_EQ(std::string(s), "12345");
  }

  TEST_CASE("VLIKELY preserves truthiness of true expression") {
    bool val = true;
    bool branch_taken = false;

    if VLIKELY (val) {
      branch_taken = true;
    }

    CHECK(branch_taken);
  }

  TEST_CASE("VLIKELY preserves truthiness of false expression") {
    bool val = false;
    bool branch_taken = false;

    if VLIKELY (val) {
      branch_taken = true;
    }

    CHECK_FALSE(branch_taken);
  }

  TEST_CASE("VUNLIKELY preserves truthiness of true expression") {
    bool val = true;
    bool branch_taken = false;

    if VUNLIKELY (val) {
      branch_taken = true;
    }

    CHECK(branch_taken);
  }

  TEST_CASE("VUNLIKELY preserves truthiness of false expression") {
    bool val = false;
    bool branch_taken = false;

    if VUNLIKELY (val) {
      branch_taken = true;
    }

    CHECK_FALSE(branch_taken);
  }

  TEST_CASE("VLINK_ASSERT_CONSTANT on string literal compiles without error") {
    VLINK_ASSERT_CONSTANT("constant string");
  }
}

// NOLINTEND
