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
 * @file macros.h
 * @brief Cross-platform macros for visibility, branch hints, copy prevention, singletons and string helpers.
 *
 * @details
 * Every header in VLink relies on this file for ABI export decoration, alignment, branch
 * prediction hints, and class-level boilerplate.  Macros that depend on compiler features
 * gracefully degrade to no-ops on platforms that do not expose those features.
 *
 * @par Macro reference
 *
 * | Category             | Macro                                    | Purpose                                        |
 * | -------------------- | ---------------------------------------- | ---------------------------------------------- |
 * | Visibility           | @c VLINK_EXPORT                          | DLL / shared-object visibility                 |
 * | Visibility + align   | @c VLINK_EXPORT_AND_ALIGNED(n)           | Combine export with byte alignment             |
 * | Branch hints (long)  | @c VLINK_LIKELY(x), @c VLINK_UNLIKELY(x) | C++20 @c [[likely]] / @c __builtin_expect      |
 * | Branch hints (short) | @c VLIKELY(...), @c VUNLIKELY(...)       | Aliases of @c VLINK_LIKELY / @c VLINK_UNLIKELY |
 * | Class hygiene        | @c VLINK_DISALLOW_COPY_AND_ASSIGN(class) | Delete copy ctor and assignment                |
 * | Singletons           | @c VLINK_SINGLETON_CHECK(class)          | Enforce one instance per process               |
 * | Singletons           | @c VLINK_SINGLETON_DECLARE(class)        | Add @c get accessor + check + no-copy          |
 * | Stringify            | @c VLINK_MACRO_STRING_IFY(name)          | Stringify a token directly                     |
 * | Stringify (eval)     | @c VLINK_MACRO_STRING_GET(name)          | Stringify after macro expansion                |
 * | Constant check       | @c VLINK_ASSERT_CONSTANT(msg)            | @c static_assert message is a literal          |
 *
 * @par Example
 * @code
 *   class VLINK_EXPORT MyService final {
 *     VLINK_DISALLOW_COPY_AND_ASSIGN(MyService)
 *    public:
 *     MyService();
 *   };
 *
 *   if VLIKELY (ptr != nullptr) {
 *     ptr->do_something();
 *   }
 * @endcode
 *
 * @note @c VLINK_SINGLETON_CHECK enforces uniqueness at run time by tripping a
 *       @c std::atomic_flag the second time the constructor runs; the second
 *       instantiation throws @c std::runtime_error.
 */

#pragma once

// export
#undef VLINK_EXPORT
#ifdef VLINK_LIBRARY_STATIC
#define VLINK_EXPORT
#elif defined(_WIN32) || defined(__CYGWIN__)
#ifdef VLINK_LIBRARY
/// @cond INTERNAL
#define VLINK_EXPORT __declspec(dllexport)
#else
#define VLINK_EXPORT __declspec(dllimport)
/// @endcond
#endif
#else
#define VLINK_EXPORT __attribute__((visibility("default")))
#endif

// align
#undef VLINK_EXPORT_AND_ALIGNED
#ifdef _MSC_VER
#define VLINK_EXPORT_AND_ALIGNED(align_num) VLINK_EXPORT __declspec(align(align_num))
#else
/**
 * @def VLINK_EXPORT_AND_ALIGNED(align_num)
 * @brief Marks a class or variable as both exported and aligned to @p align_num bytes.
 *
 * @details
 * Expands to @c __declspec(align(align_num)) on MSVC and to
 * @c __attribute__((aligned(align_num))) on GCC / Clang.
 *
 * @param align_num  Required alignment in bytes; must be a power of two.
 */
#define VLINK_EXPORT_AND_ALIGNED(align_num) VLINK_EXPORT __attribute__((aligned(align_num)))
#endif

// win32
#if defined(_WIN32) && !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif

// msvc
#ifdef _MSC_VER
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS  // NOLINT(bugprone-reserved-identifier, readability-identifier-naming)
#endif
#pragma warning(disable : 4065)
#pragma warning(disable : 4251)
#pragma warning(disable : 4244)
#pragma warning(disable : 4267)
#pragma warning(disable : 4324)
#pragma warning(disable : 4702)
#pragma warning(disable : 4819)
#pragma warning(disable : 4840)
#pragma warning(disable : 4996)
#endif

// __has_builtin
#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

#ifdef __cplusplus
#if __cplusplus >= 202002L
/**
 * @def VLINK_LIKELY(x)
 * @brief Hints to the compiler that @p x is very likely to evaluate to @c true.
 *
 * @details
 * Expands to the C++20 @c [[likely]] attribute when available, @c __builtin_expect on older
 * GCC / Clang, and is a no-op on MSVC.
 *
 * @param x  Boolean expression to predict.
 */
#define VLINK_LIKELY(x) (x) [[likely]]
/**
 * @def VLINK_UNLIKELY(x)
 * @brief Hints to the compiler that @p x is very unlikely to evaluate to @c true.
 *
 * @details
 * Counterpart of @c VLINK_LIKELY with the opposite hint.
 *
 * @param x  Boolean expression to predict.
 */
#define VLINK_UNLIKELY(x) (x) [[unlikely]]
#else
#ifdef _WIN32
#define VLINK_LIKELY(x) (x)
#define VLINK_UNLIKELY(x) (x)
#else
#define VLINK_LIKELY(x) (__builtin_expect(!!(x), 1))
#define VLINK_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#endif
#endif
#else
#define VLINK_LIKELY(x) (x)
#define VLINK_UNLIKELY(x) (x)
#endif

/**
 * @def VLINK_DISALLOW_COPY_AND_ASSIGN(classname)
 * @brief Deletes the copy constructor and copy-assignment operator of @p classname.
 *
 * @details
 * Drop the macro inside the private section of a class to prevent accidental copying.
 *
 * @param classname  Unqualified name of the enclosing class.
 */
#define VLINK_DISALLOW_COPY_AND_ASSIGN(classname) \
  classname(const classname&) = delete;           \
  classname& operator=(const classname&) = delete;

/**
 * @def VLINK_SINGLETON_CHECK(classname)
 * @brief Embeds a static check that enforces one-instance-per-process for @p classname.
 *
 * @details
 * Combines two safeguards:
 *  - Compile-time @c static_assert that @p classname is @c final and not externally
 *    default-constructible.
 *  - Run-time @c std::atomic_flag trip that throws @c std::runtime_error on the second
 *    instantiation.
 *
 * @param classname  Unqualified name of the enclosing class.
 * @note Place inside the @c private section of the class body.
 */
#define VLINK_SINGLETON_CHECK(classname)                                                          \
 private:                                                                                         \
  struct ConstructorChecker {                                                                     \
    inline ConstructorChecker() {                                                                 \
      static_assert(std::is_final_v<classname>, "Constructor must be final.");                    \
      static_assert(!std::is_default_constructible_v<classname>, "Constructor must be private."); \
                                                                                                  \
      static std::atomic_flag flag;                                                               \
                                                                                                  \
      if VLINK_UNLIKELY (flag.test_and_set()) {                                                   \
        throw std::runtime_error("Constructor has been instantiated.");                           \
      }                                                                                           \
    }                                                                                             \
  };                                                                                              \
                                                                                                  \
  ConstructorChecker constructor_checker_;

/**
 * @def VLINK_SINGLETON_DECLARE(classname)
 * @brief Declares a Meyers singleton @c get and the matching uniqueness guards on @p classname.
 *
 * @details
 * Expands to a public static @c get returning a reference to the single instance plus the
 * @c VLINK_SINGLETON_CHECK and @c VLINK_DISALLOW_COPY_AND_ASSIGN macros.
 *
 * @param classname  Unqualified name of the singleton class.
 *
 * @par Example
 * @code
 *   class MyService final {
 *     VLINK_SINGLETON_DECLARE(MyService)
 *    public:
 *     void do_work();
 *    private:
 *     MyService() = default;
 *   };
 *
 *   MyService::get().do_work();
 * @endcode
 */
#define VLINK_SINGLETON_DECLARE(classname)    \
 public:                                      \
  inline static classname& get() {            \
    static classname singleton = classname(); \
    return singleton;                         \
  }                                           \
                                              \
 private:                                     \
  VLINK_SINGLETON_CHECK(classname)            \
  VLINK_DISALLOW_COPY_AND_ASSIGN(classname)

/**
 * @def VLINK_MACRO_STRING_IFY(name)
 * @brief Stringifies a token without performing macro expansion.
 *
 * @param name  Token to stringify.
 */
#define VLINK_MACRO_STRING_IFY(name) #name

/**
 * @def VLINK_MACRO_STRING_GET(name)
 * @brief Stringifies the expanded value of a macro (two-step expansion).
 *
 * @details
 * Use this form instead of @c VLINK_MACRO_STRING_IFY when @p name is itself a macro that should
 * be expanded before stringification.
 *
 * @param name  Macro whose expansion is stringified.
 */
#define VLINK_MACRO_STRING_GET(name) VLINK_MACRO_STRING_IFY(name)

#if __has_builtin(__builtin_constant_p)
/**
 * @def VLINK_ASSERT_CONSTANT(msg)
 * @brief Asserts at compile time that @p msg is a constant string literal.
 *
 * @details
 * Expands to a @c static_assert on toolchains that expose @c __builtin_constant_p; otherwise
 * the macro is a no-op.
 *
 * @param msg  Expression checked for compile-time constancy.
 */
#define VLINK_ASSERT_CONSTANT(msg) static_assert(__builtin_constant_p(msg), "Must be a constant string literal.");
#else
#define VLINK_ASSERT_CONSTANT(msg)
#endif

#if !defined(VLIKELY) && !defined(VUNLIKELY)
/**
 * @def VLIKELY(...)
 * @brief Short alias for @c VLINK_LIKELY.
 */
#define VLIKELY(...) VLINK_LIKELY(__VA_ARGS__)
/**
 * @def VUNLIKELY(...)
 * @brief Short alias for @c VLINK_UNLIKELY.
 */
#define VUNLIKELY(...) VLINK_UNLIKELY(__VA_ARGS__)
#endif
