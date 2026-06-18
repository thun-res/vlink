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
 * @file traits.h
 * @brief Compile-time type-trait helpers used across the VLink codebase.
 *
 * @details
 * @c vlink::Traits collects a small set of self-contained template meta-functions that
 * extend the C++ standard @c <type_traits>.  Every helper follows the standard pattern of
 * either inheriting from @c std::true_type / @c std::false_type or returning a @c bool
 * from a @c constexpr function.  The macro @c VLINK_HAS_MEMBER ties into
 * @c has_member to detect named data or function members at compile time.
 *
 * Trait reference:
 *
 * | Helper                  | What it detects                                                  |
 * | ----------------------- | ---------------------------------------------------------------- |
 * | @c EmptyType            | An empty tag type used as a no-op placeholder.                   |
 * | @c ExpectFalse<T>       | Always @c std::false_type; used for dependent @c static_assert.  |
 * | @c Callable<T>          | @c T can be invoked with no arguments.                           |
 * | @c Assignable<OT,T>     | @c OT can be assigned a value of type @c T.                      |
 * | @c EqualityComparable   | @c OT supports @c operator== with @c T.                          |
 * | @c GreaterComparable    | @c OT supports both @c operator< and @c operator> with @c T.     |
 * | @c Operatorable<OT,T>   | @c OT supports stream insertion and extraction with @c T.        |
 * | @c IsAtomic<T>          | @c T is a @c std::atomic specialisation.                         |
 * | @c IsSharedPtr<T>       | @c T is a @c std::shared_ptr (or derived) specialisation.        |
 * | @c RemoveSharedPtr<T>   | Removes a single @c std::shared_ptr wrapper from @c T.           |
 * | @c has_member<T>(f)     | Compile-time member-access probe used by @c VLINK_HAS_MEMBER.    |
 * | @c is_non_char_ptr<T>() | @c T decays to a pointer other than @c char*.                    |
 * | @c is_integer<T>()      | @c T is an integer excluding @c bool and the @c char variants.   |
 * | @c is_floating<T>()     | @c T is a floating-point type.                                   |
 *
 * @par Example
 * @code
 * struct Foo { int bar; void baz() {} };
 *
 * static_assert( VLINK_HAS_MEMBER(Foo, bar));
 * static_assert(!VLINK_HAS_MEMBER(Foo, qux));
 * static_assert( VLINK_HAS_MEMBER(Foo, baz()));
 *
 * static_assert(vlink::Traits::Callable<decltype([] {})>::value);
 * static_assert(vlink::Traits::IsAtomic<std::atomic<int>>::value);
 * @endcode
 */

#pragma once

#include <atomic>
#include <memory>
#include <type_traits>

namespace vlink {

/**
 * @namespace vlink::Traits
 * @brief Collection of compile-time type-trait helpers for VLink.
 */
namespace Traits {  // NOLINT(readability-identifier-naming)

/**
 * @struct EmptyType
 * @brief Empty tag type used wherever a type parameter must be supplied but carries no payload.
 *
 * @details
 * Acts as the default detail type in places such as the Logger so the meta-programming
 * machinery has a single concrete identity to bind to.
 */
struct EmptyType {};

/**
 * @struct ExpectFalse
 * @brief Type trait that always derives from @c std::false_type for any @p T.
 *
 * @details
 * Used inside @c static_assert to produce a dependent false expression, which prevents
 * the compiler from instantiating a primary template when only a specialisation is
 * valid.
 *
 * @tparam T  Arbitrary placeholder type; the value is always false.
 */
template <typename T>
struct ExpectFalse : std::false_type {};

/**
 * @struct Callable
 * @brief Detects whether @p T can be invoked with no arguments.
 *
 * @tparam T  Type to probe.
 */
template <typename T, typename = void>
struct Callable : std::false_type {};

template <typename T>
struct Callable<T, std::void_t<decltype(std::declval<T>()())>> : std::true_type {};

/**
 * @struct Assignable
 * @brief Detects whether a value of type @p T can be assigned to an instance of @p OT.
 *
 * @tparam OT  Target type on the left of @c =.
 * @tparam T   Source type on the right of @c =.
 */
template <typename OT, typename T, typename = void>
struct Assignable : std::false_type {};

template <typename OT, typename T>
struct Assignable<OT, T, std::void_t<decltype(std::declval<OT&>() = std::declval<T>())>> : std::true_type {};

/**
 * @struct EqualityComparable
 * @brief Detects whether @p OT supports @c operator== with @p T.
 *
 * @tparam OT  Left-hand side type.
 * @tparam T   Right-hand side type.
 */
template <typename OT, typename T, typename = void>
struct EqualityComparable : std::false_type {};

template <typename OT, typename T>
struct EqualityComparable<OT, T, std::void_t<decltype(std::declval<OT>() == std::declval<T>())>> : std::true_type {};

/**
 * @struct GreaterComparable
 * @brief Detects whether @p OT supports both @c operator< and @c operator> with @p T.
 *
 * @tparam OT  Left-hand side type.
 * @tparam T   Right-hand side type.
 */
template <typename OT, typename T, typename = void>
struct GreaterComparable : ExpectFalse<OT> {};

template <typename OT, typename T>
struct GreaterComparable<
    OT, T,
    std::void_t<decltype(std::declval<OT&>() < std::declval<T>()), decltype(std::declval<OT&>() > std::declval<T&>())>>
    : std::true_type {};

/**
 * @struct Operatorable
 * @brief Detects whether @p OT supports stream insertion (<<) and extraction (>>) with @p T.
 *
 * @tparam OT  Stream-like type.
 * @tparam T   Value type appearing on the right of the stream operator.
 */
template <typename OT, typename T, typename = void>
struct Operatorable : ExpectFalse<OT> {};

template <typename OT, typename T>
struct Operatorable<OT, T,
                    std::void_t<decltype(std::declval<OT&>() << std::declval<T>()),
                                decltype(std::declval<OT&>() >> std::declval<T&>())>> : std::true_type {};

/**
 * @struct IsAtomic
 * @brief Detects whether @p T is a @c std::atomic specialisation.
 *
 * @tparam T  Candidate type.
 */
template <typename T>
struct IsAtomic : std::false_type {};

template <typename T>
struct IsAtomic<std::atomic<T>> : std::true_type {};

/**
 * @struct IsSharedPtr
 * @brief Detects whether @p T is (or derives from) a @c std::shared_ptr specialisation.
 *
 * @tparam T  Candidate type.
 */
template <typename T, typename = void>
struct IsSharedPtr : std::false_type {};

template <typename T>
struct IsSharedPtr<T, std::void_t<typename T::element_type>>
    : std::is_base_of<std::shared_ptr<typename T::element_type>, T> {};

/**
 * @struct RemoveSharedPtr
 * @brief Removes a single @c std::shared_ptr wrapper from @p T, leaving non-pointer types unchanged.
 *
 * @details
 * When @p T is a @c std::shared_ptr, @c Type is the contained element type; otherwise
 * @c Type is @p T verbatim.
 *
 * @tparam T  Source type.
 */
template <typename T, bool = IsSharedPtr<T>::value>
struct RemoveSharedPtr {
  using Type = T;  ///< Resolved type after optional shared_ptr removal.
};

template <typename T>
struct RemoveSharedPtr<T, true> {
  using Type = typename T::element_type;  ///< Element type unwrapped from the shared_ptr.
};

/**
 * @brief Compile-time probe that succeeds when @p f is a well-formed expression on a default-constructed @p T.
 *
 * @details
 * Used as a building block for @c VLINK_HAS_MEMBER.  The lambda passed as @p f must take a
 * forwarding reference and reference the candidate member with @c decltype-style sfinae.
 *
 * @tparam T  Probed type.
 * @tparam F  Lambda type.
 * @param  f  Probe lambda.
 * @return @c true when the lambda is well-formed for @p T, @c false otherwise.
 *
 * @note Prefer the @c VLINK_HAS_MEMBER macro for member-name checks.
 */
template <typename T, typename F>
[[nodiscard]] static constexpr auto has_member(F&& f) noexcept -> decltype(f(std::declval<T>()), true) {
  return true;
}

template <typename>
// NOLINTNEXTLINE(modernize-avoid-variadic-functions)
[[nodiscard]] static constexpr auto has_member(...) noexcept {
  return false;
}

/**
 * @brief Returns @c true when @p T decays to a pointer type other than @c char*.
 *
 * @details
 * Used internally by VLink serialisation to distinguish raw data pointers from
 * C-strings.
 *
 * @tparam T  Type under test.
 * @return @c true for non-char pointer types.
 */
template <typename T>
[[nodiscard]] static constexpr auto is_non_char_ptr() noexcept {
  return std::is_pointer_v<std::decay_t<T>> &&
         !std::is_same_v<std::remove_cv_t<std::remove_pointer_t<std::decay_t<T>>>, char>;
}

/**
 * @brief Returns @c true for plain integer types, excluding @c bool and every @c char variant.
 *
 * @details
 * Accepts @c short, @c int, @c long, @c long @c long and their unsigned counterparts.
 * Explicitly rejects @c bool, @c char, @c signed @c char and @c unsigned @c char.
 *
 * @tparam T  Type under test.
 * @return @c true for the accepted integer set.
 */
template <typename T>
[[nodiscard]] static constexpr bool is_integer() {
  using U = std::remove_cv_t<T>;
  return std::is_integral_v<U> && !std::is_same_v<U, bool> && !std::is_same_v<U, char> &&
         !std::is_same_v<U, signed char> && !std::is_same_v<U, unsigned char>;
}

/**
 * @brief Returns @c true when @p T is a floating-point type after stripping CV qualifiers.
 *
 * @tparam T  Type under test.
 * @return @c true for @c float, @c double, @c long @c double.
 */
template <typename T>
[[nodiscard]] static constexpr bool is_floating() {
  return std::is_floating_point_v<std::remove_cv_t<T>>;
}

}  // namespace Traits

}  // namespace vlink

////////////////////////////////////////////////////////////////
/// Macro Definitions
////////////////////////////////////////////////////////////////

/**
 * @def VLINK_HAS_MEMBER(T, member)
 * @brief Compile-time check that @p T has an accessible member named @p member.
 *
 * @details
 * Expands to a @c constexpr boolean expression.  Use call syntax (@c member()) to probe
 * for a member function rather than a data member.  Internally delegates to
 * @c vlink::Traits::has_member.
 *
 * @param T       Type to inspect.
 * @param member  Member name (or @c member() for a callable probe).
 *
 * @par Example
 * @code
 * struct Foo { int bar; void baz() {} };
 * static_assert( VLINK_HAS_MEMBER(Foo, bar));
 * static_assert(!VLINK_HAS_MEMBER(Foo, qux));
 * static_assert( VLINK_HAS_MEMBER(Foo, baz()));
 * @endcode
 */
#define VLINK_HAS_MEMBER(T, member) \
  vlink::Traits::has_member<T>([](auto&& obj) -> decltype((void)(obj.member), 0) { return 0; })
