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

#include "./base/exception.h"

#include <doctest/doctest.h>

#include <stdexcept>
#include <string>

#include "../common_test.h"

TEST_SUITE("base-Exception") {
  TEST_CASE("RuntimeError is thrown and caught by its own type with correct what") {
    bool caught = false;

    try {
      throw Exception::RuntimeError("runtime failure");
    } catch (const Exception::RuntimeError& e) {
      caught = true;
      CHECK_EQ(std::string(e.what()), "runtime failure");
    }

    CHECK(caught);
  }

  TEST_CASE("RuntimeError is catchable as std::runtime_error") {
    bool caught = false;

    try {
      throw Exception::RuntimeError("base catch");
    } catch (const std::runtime_error& e) {
      caught = true;
      CHECK_EQ(std::string(e.what()), "base catch");
    }

    CHECK(caught);
  }

  TEST_CASE("OutOfRange is thrown and caught by its own type with correct what") {
    bool caught = false;

    try {
      throw Exception::OutOfRange("index out of range");
    } catch (const Exception::OutOfRange& e) {
      caught = true;
      CHECK_EQ(std::string(e.what()), "index out of range");
    }

    CHECK(caught);
  }

  TEST_CASE("OutOfRange is catchable as std::out_of_range") {
    bool caught = false;

    try {
      throw Exception::OutOfRange("oor");
    } catch (const std::out_of_range&) {
      caught = true;
    }

    CHECK(caught);
  }

  TEST_CASE("InvalidArgument is thrown and caught by its own type with correct what") {
    bool caught = false;

    try {
      throw Exception::InvalidArgument("bad argument");
    } catch (const Exception::InvalidArgument& e) {
      caught = true;
      CHECK_EQ(std::string(e.what()), "bad argument");
    }

    CHECK(caught);
  }

  TEST_CASE("InvalidArgument is catchable as std::invalid_argument") {
    bool caught = false;

    try {
      throw Exception::InvalidArgument("ia");
    } catch (const std::invalid_argument&) {
      caught = true;
    }

    CHECK(caught);
  }

  TEST_CASE("LogicError is thrown and caught by its own type with correct what") {
    bool caught = false;

    try {
      throw Exception::LogicError("logic violated");
    } catch (const Exception::LogicError& e) {
      caught = true;
      CHECK_EQ(std::string(e.what()), "logic violated");
    }

    CHECK(caught);
  }

  TEST_CASE("LogicError is catchable as std::logic_error") {
    bool caught = false;

    try {
      throw Exception::LogicError("le");
    } catch (const std::logic_error&) {
      caught = true;
    }

    CHECK(caught);
  }

  TEST_CASE("DomainError is thrown and caught by its own type with correct what") {
    bool caught = false;

    try {
      throw Exception::DomainError("domain error");
    } catch (const Exception::DomainError& e) {
      caught = true;
      CHECK_EQ(std::string(e.what()), "domain error");
    }

    CHECK(caught);
  }

  TEST_CASE("DomainError is catchable as std::domain_error") {
    bool caught = false;

    try {
      throw Exception::DomainError("de");
    } catch (const std::domain_error&) {
      caught = true;
    }

    CHECK(caught);
  }

  TEST_CASE("LengthError is thrown and caught by its own type with correct what") {
    bool caught = false;

    try {
      throw Exception::LengthError("length exceeded");
    } catch (const Exception::LengthError& e) {
      caught = true;
      CHECK_EQ(std::string(e.what()), "length exceeded");
    }

    CHECK(caught);
  }

  TEST_CASE("LengthError is catchable as std::length_error") {
    bool caught = false;

    try {
      throw Exception::LengthError("lne");
    } catch (const std::length_error&) {
      caught = true;
    }

    CHECK(caught);
  }

  TEST_CASE("RangeError is thrown and caught by its own type with correct what") {
    bool caught = false;

    try {
      throw Exception::RangeError("range error");
    } catch (const Exception::RangeError& e) {
      caught = true;
      CHECK_EQ(std::string(e.what()), "range error");
    }

    CHECK(caught);
  }

  TEST_CASE("RangeError is catchable as std::range_error") {
    bool caught = false;

    try {
      throw Exception::RangeError("re");
    } catch (const std::range_error&) {
      caught = true;
    }

    CHECK(caught);
  }

  TEST_CASE("OverflowError is thrown and caught by its own type with correct what") {
    bool caught = false;

    try {
      throw Exception::OverflowError("overflow occurred");
    } catch (const Exception::OverflowError& e) {
      caught = true;
      CHECK_EQ(std::string(e.what()), "overflow occurred");
    }

    CHECK(caught);
  }

  TEST_CASE("OverflowError is catchable as std::overflow_error") {
    bool caught = false;

    try {
      throw Exception::OverflowError("of");
    } catch (const std::overflow_error&) {
      caught = true;
    }

    CHECK(caught);
  }

  TEST_CASE("UnderflowError is thrown and caught by its own type with correct what") {
    bool caught = false;

    try {
      throw Exception::UnderflowError("underflow occurred");
    } catch (const Exception::UnderflowError& e) {
      caught = true;
      CHECK_EQ(std::string(e.what()), "underflow occurred");
    }

    CHECK(caught);
  }

  TEST_CASE("UnderflowError is catchable as std::underflow_error") {
    bool caught = false;

    try {
      throw Exception::UnderflowError("uf");
    } catch (const std::underflow_error&) {
      caught = true;
    }

    CHECK(caught);
  }

  TEST_CASE("OperationCancelled what returns the fixed cancellation message") {
    Exception::OperationCancelled ex;
    CHECK_EQ(std::string(ex.what()), "vlink operation cancelled");
  }

  TEST_CASE("OperationCancelled is catchable as std::exception") {
    bool caught = false;

    try {
      throw Exception::OperationCancelled{};
    } catch (const std::exception& e) {
      caught = true;
      CHECK_EQ(std::string(e.what()), "vlink operation cancelled");
    }

    CHECK(caught);
  }

  TEST_CASE("all exception types are catchable as std::exception") {
    auto throw_and_catch = [](auto ex) {
      bool caught = false;

      try {
        throw ex;
      } catch (const std::exception& e) {
        caught = true;
        CHECK_FALSE(std::string(e.what()).empty());
      }

      CHECK(caught);
    };

    throw_and_catch(Exception::RuntimeError("re"));
    throw_and_catch(Exception::OutOfRange("oor"));
    throw_and_catch(Exception::InvalidArgument("ia"));
    throw_and_catch(Exception::LogicError("le"));
    throw_and_catch(Exception::DomainError("de"));
    throw_and_catch(Exception::LengthError("lne"));
    throw_and_catch(Exception::RangeError("re2"));
    throw_and_catch(Exception::OverflowError("of"));
    throw_and_catch(Exception::UnderflowError("uf"));
    throw_and_catch(Exception::OperationCancelled{});
  }

  TEST_CASE("what returns the message passed to the constructor") {
    const std::string msg = "detailed error message";
    Exception::RuntimeError err(msg);
    CHECK_EQ(std::string(err.what()), msg);
  }

  TEST_CASE("exception constructed from std::string preserves the message") {
    std::string s = "from std::string";
    Exception::InvalidArgument err(s);
    CHECK_EQ(std::string(err.what()), s);
  }

  TEST_CASE("RuntimeError is not caught by a sibling LogicError handler") {
    bool caught_runtime = false;
    bool caught_logic = false;

    try {
      throw Exception::RuntimeError("runtime");
    } catch (const Exception::LogicError&) {
      caught_logic = true;
    } catch (const Exception::RuntimeError&) {
      caught_runtime = true;
    }

    CHECK(caught_runtime);
    CHECK_FALSE(caught_logic);
  }

  TEST_CASE("LogicError is not caught by std::runtime_error handler") {
    bool caught_runtime = false;
    bool caught_logic = false;

    try {
      throw Exception::LogicError("logic");
    } catch (const std::runtime_error&) {
      caught_runtime = true;
    } catch (const std::logic_error&) {
      caught_logic = true;
    }

    CHECK_FALSE(caught_runtime);
    CHECK(caught_logic);
  }
}

// NOLINTEND
