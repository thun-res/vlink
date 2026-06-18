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

#include "./base/traits.h"

#include <doctest/doctest.h>

#include <atomic>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>

#include "../common_test.h"

namespace {

struct WithBar {
  int bar{0};
  void method() {}
};

struct WithoutBar {};

struct CallableObj {
  void operator()() {}
};

struct NonCallable {
  int x{0};
};

struct StreamableObj {
  int v{0};
};

[[maybe_unused]] std::ostringstream& operator<<(std::ostringstream& os, const StreamableObj& s) {
  os << s.v;
  return os;
}

[[maybe_unused]] std::ostringstream& operator>>(std::ostringstream& os, StreamableObj& s) {
  s.v = 0;
  return os;
}

}  // namespace

TEST_SUITE("base-Traits") {
  TEST_CASE("EmptyType is default-constructible with sizeof 1") {
    Traits::EmptyType e;
    (void)e;
    CHECK_EQ(sizeof(Traits::EmptyType), 1u);
  }

  TEST_CASE("ExpectFalse always yields false_type regardless of template argument") {
    CHECK_FALSE(Traits::ExpectFalse<int>::value);
    CHECK_FALSE(Traits::ExpectFalse<std::string>::value);
    CHECK_FALSE(Traits::ExpectFalse<void>::value);
    CHECK_FALSE(Traits::ExpectFalse<WithBar>::value);
  }

  TEST_CASE("Callable detects zero-argument callable types") {
    auto lam = [] {};
    CHECK(Traits::Callable<decltype(lam)>::value);
    CHECK(Traits::Callable<std::function<void()>>::value);
    CHECK(Traits::Callable<CallableObj>::value);
    CHECK(Traits::Callable<void (*)()>::value);
  }

  TEST_CASE("Callable rejects non-callable types") {
    CHECK_FALSE(Traits::Callable<int>::value);
    CHECK_FALSE(Traits::Callable<NonCallable>::value);
    CHECK_FALSE(Traits::Callable<std::string>::value);
  }

  TEST_CASE("Assignable detects valid assignment expressions") {
    CHECK((Traits::Assignable<int, int>::value));
    CHECK((Traits::Assignable<double, int>::value));
    CHECK((Traits::Assignable<std::string, const char*>::value));
  }

  TEST_CASE("Assignable rejects invalid assignment expressions") {
    CHECK_FALSE((Traits::Assignable<int, std::string>::value));
    CHECK_FALSE((Traits::Assignable<WithBar, int>::value));
  }

  TEST_CASE("EqualityComparable detects operator== support") {
    CHECK((Traits::EqualityComparable<int, int>::value));
    CHECK((Traits::EqualityComparable<std::string, std::string>::value));
    CHECK((Traits::EqualityComparable<double, int>::value));
  }

  TEST_CASE("EqualityComparable rejects types without operator==") {
    CHECK_FALSE((Traits::EqualityComparable<WithBar, WithBar>::value));
    CHECK_FALSE((Traits::EqualityComparable<WithoutBar, WithoutBar>::value));
  }

  TEST_CASE("GreaterComparable detects both less-than and greater-than support") {
    CHECK((Traits::GreaterComparable<int, int>::value));
    CHECK((Traits::GreaterComparable<double, double>::value));
    CHECK((Traits::GreaterComparable<float, float>::value));
  }

  TEST_CASE("GreaterComparable rejects types lacking comparison operators") {
    CHECK_FALSE((Traits::GreaterComparable<WithBar, WithBar>::value));
    CHECK_FALSE((Traits::GreaterComparable<WithoutBar, WithoutBar>::value));
  }

  TEST_CASE("Operatorable detects stream insertion and extraction support") {
    CHECK((Traits::Operatorable<std::ostringstream, StreamableObj>::value));
    CHECK((Traits::Operatorable<int, int>::value));
  }

  TEST_CASE("Operatorable rejects types without stream operators") {
    CHECK_FALSE((Traits::Operatorable<WithBar, WithBar>::value));
    CHECK_FALSE((Traits::Operatorable<WithoutBar, WithoutBar>::value));
  }

  TEST_CASE("IsAtomic detects std::atomic specializations") {
    CHECK(Traits::IsAtomic<std::atomic<int>>::value);
    CHECK(Traits::IsAtomic<std::atomic<bool>>::value);
    CHECK(Traits::IsAtomic<std::atomic<uint64_t>>::value);
  }

  TEST_CASE("IsAtomic rejects non-atomic types") {
    CHECK_FALSE(Traits::IsAtomic<int>::value);
    CHECK_FALSE(Traits::IsAtomic<std::string>::value);
    CHECK_FALSE(Traits::IsAtomic<WithBar>::value);
  }

  TEST_CASE("IsSharedPtr detects std::shared_ptr specializations") {
    CHECK(Traits::IsSharedPtr<std::shared_ptr<int>>::value);
    CHECK(Traits::IsSharedPtr<std::shared_ptr<WithBar>>::value);
    CHECK(Traits::IsSharedPtr<std::shared_ptr<std::string>>::value);
  }

  TEST_CASE("IsSharedPtr rejects non-shared_ptr types") {
    CHECK_FALSE(Traits::IsSharedPtr<int>::value);
    CHECK_FALSE(Traits::IsSharedPtr<std::unique_ptr<int>>::value);
    CHECK_FALSE(Traits::IsSharedPtr<std::string>::value);
  }

  TEST_CASE("RemoveSharedPtr unwraps element type from shared_ptr") {
    CHECK((std::is_same_v<Traits::RemoveSharedPtr<std::shared_ptr<int>>::Type, int>));
    CHECK((std::is_same_v<Traits::RemoveSharedPtr<std::shared_ptr<WithBar>>::Type, WithBar>));
  }

  TEST_CASE("RemoveSharedPtr leaves non-shared_ptr types unchanged") {
    CHECK((std::is_same_v<Traits::RemoveSharedPtr<int>::Type, int>));
    CHECK((std::is_same_v<Traits::RemoveSharedPtr<std::string>::Type, std::string>));
    CHECK((std::is_same_v<Traits::RemoveSharedPtr<WithBar>::Type, WithBar>));
  }

  TEST_CASE("VLINK_HAS_MEMBER detects accessible data members at compile time") {
    static constexpr bool has_bar = VLINK_HAS_MEMBER(WithBar, bar);
    static constexpr bool baz_no_bar = VLINK_HAS_MEMBER(WithoutBar, bar);

    CHECK(has_bar);
    CHECK_FALSE(baz_no_bar);
  }

  TEST_CASE("VLINK_HAS_MEMBER returns false for non-existent member names") {
    static constexpr bool result = VLINK_HAS_MEMBER(WithBar, xyz_nonexistent);

    CHECK_FALSE(result);
  }

  TEST_CASE("is_non_char_ptr returns true for non-char pointer types") {
    CHECK(Traits::is_non_char_ptr<int*>());
    CHECK(Traits::is_non_char_ptr<WithBar*>());
    CHECK(Traits::is_non_char_ptr<void*>());
    CHECK(Traits::is_non_char_ptr<double*>());
  }

  TEST_CASE("is_non_char_ptr returns false for char pointers and non-pointers") {
    CHECK_FALSE(Traits::is_non_char_ptr<char*>());
    CHECK_FALSE(Traits::is_non_char_ptr<const char*>());
    CHECK_FALSE(Traits::is_non_char_ptr<int>());
    CHECK_FALSE(Traits::is_non_char_ptr<std::string>());
  }

  TEST_CASE("is_integer returns true for short int long and unsigned variants") {
    CHECK(Traits::is_integer<short>());      // NOLINT(runtime/int)
    CHECK(Traits::is_integer<int>());        // NOLINT(runtime/int)
    CHECK(Traits::is_integer<long>());       // NOLINT(runtime/int)
    CHECK(Traits::is_integer<long long>());  // NOLINT(runtime/int)
    CHECK(Traits::is_integer<unsigned int>());
    CHECK(Traits::is_integer<uint64_t>());
    CHECK(Traits::is_integer<const int>());
  }

  TEST_CASE("is_integer returns false for bool char signed char unsigned char and floats") {
    CHECK_FALSE(Traits::is_integer<bool>());
    CHECK_FALSE(Traits::is_integer<char>());
    CHECK_FALSE(Traits::is_integer<signed char>());
    CHECK_FALSE(Traits::is_integer<unsigned char>());
    CHECK_FALSE(Traits::is_integer<float>());
    CHECK_FALSE(Traits::is_integer<double>());
  }

  TEST_CASE("is_floating returns true for float double and long double") {
    CHECK(Traits::is_floating<float>());
    CHECK(Traits::is_floating<double>());
    CHECK(Traits::is_floating<long double>());  // NOLINT(runtime/int)
    CHECK(Traits::is_floating<const float>());
    CHECK(Traits::is_floating<const double>());
  }

  TEST_CASE("is_floating returns false for integer and bool types") {
    CHECK_FALSE(Traits::is_floating<int>());
    CHECK_FALSE(Traits::is_floating<bool>());
    CHECK_FALSE(Traits::is_floating<char>());
    CHECK_FALSE(Traits::is_floating<uint64_t>());
  }
}

// NOLINTEND
