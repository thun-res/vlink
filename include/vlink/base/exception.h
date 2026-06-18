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

/**
 * @file exception.h
 * @brief Thin @c final wrappers around the standard exception hierarchy used by VLink.
 *
 * @details
 * Each VLink exception type is a @c final subclass of the corresponding standard exception base
 * and lives inside the @c vlink::Exception namespace to avoid clashing with application code.
 * Constructors and @c what() are inherited verbatim; only @c OperationCancelled overrides
 * @c what() with a fixed identifier string.
 *
 * @par Class hierarchy
 *
 * | VLink class                       | Standard base               | Typical usage                          |
 * | --------------------------------- | --------------------------- | -------------------------------------- |
 * | @c Exception::RuntimeError        | @c std::runtime_error       | Generic runtime failure (logger fatal) |
 * | @c Exception::OutOfRange          | @c std::out_of_range        | Index or iterator out of bounds        |
 * | @c Exception::InvalidArgument     | @c std::invalid_argument    | Bad function argument                  |
 * | @c Exception::LogicError          | @c std::logic_error         | Violated precondition                  |
 * | @c Exception::DomainError         | @c std::domain_error        | Value outside the function domain      |
 * | @c Exception::LengthError         | @c std::length_error        | Exceeded implementation limit          |
 * | @c Exception::RangeError          | @c std::range_error         | Arithmetic range error                 |
 * | @c Exception::OverflowError       | @c std::overflow_error      | Arithmetic overflow                    |
 * | @c Exception::UnderflowError      | @c std::underflow_error     | Arithmetic underflow                   |
 * | @c Exception::OperationCancelled  | @c std::exception           | Cooperative cancellation observed      |
 *
 * @note
 * @c Logger throws @c Exception::RuntimeError whenever a @c kFatal message is emitted.  The
 * logger flushes all sinks before throwing so the application can catch the exception and
 * shut down cleanly.
 *
 * @par Example
 * @code
 *   try {
 *     VLOG_F("Critical failure: ", reason);
 *   } catch (const vlink::Exception::RuntimeError& e) {
 *     std::cerr << e.what() << "\n";
 *     shutdown_cleanup();
 *   }
 * @endcode
 */

#pragma once

#include <exception>
#include <stdexcept>

namespace vlink {

/**
 * @namespace vlink::Exception
 * @brief Container namespace for VLink exception types.
 */
namespace Exception {  // NOLINT(readability-identifier-naming)

/**
 * @class RuntimeError
 * @brief Generic runtime failure; thrown by the logger on @c kFatal messages.
 */
class RuntimeError final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/**
 * @class OutOfRange
 * @brief Indicates an index or iterator that is outside the legal range.
 */
class OutOfRange final : public std::out_of_range {
 public:
  using std::out_of_range::out_of_range;
};

/**
 * @class InvalidArgument
 * @brief Indicates that a function received an argument with an invalid value.
 */
class InvalidArgument final : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/**
 * @class LogicError
 * @brief Indicates a violated program logic precondition.
 */
class LogicError final : public std::logic_error {
 public:
  using std::logic_error::logic_error;
};

/**
 * @class DomainError
 * @brief Indicates a value outside the mathematical domain of a function.
 */
class DomainError final : public std::domain_error {
 public:
  using std::domain_error::domain_error;
};

/**
 * @class LengthError
 * @brief Indicates an attempt to exceed an implementation size limit.
 */
class LengthError final : public std::length_error {
 public:
  using std::length_error::length_error;
};

/**
 * @class RangeError
 * @brief Indicates an arithmetic range error.
 */
class RangeError final : public std::range_error {
 public:
  using std::range_error::range_error;
};

/**
 * @class OverflowError
 * @brief Indicates an arithmetic overflow.
 */
class OverflowError final : public std::overflow_error {
 public:
  using std::overflow_error::overflow_error;
};

/**
 * @class UnderflowError
 * @brief Indicates an arithmetic underflow.
 */
class UnderflowError final : public std::underflow_error {
 public:
  using std::underflow_error::underflow_error;
};

/**
 * @class OperationCancelled
 * @brief Marker exception thrown when a cooperative cancellation request is observed.
 */
class OperationCancelled final : public std::exception {
 public:
  using std::exception::exception;

  /**
   * @brief Returns a fixed explanatory message identifying the exception.
   *
   * @return Static null-terminated string.
   */
  [[nodiscard]] const char* what() const noexcept override { return "vlink operation cancelled"; }
};

}  // namespace Exception

}  // namespace vlink
