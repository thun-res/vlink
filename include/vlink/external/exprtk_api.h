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
 * @file exprtk_api.h
 * @brief Thin, ABI-stable facade over the ExprTk mathematical expression engine.
 *
 * @details
 * ExprTk (@c thirdparty/exprtk/include/exprtk/exprtk.hpp) is a header-only template
 * library of roughly 46k lines.  Every translation unit that includes it instantiates
 * the full parser/optimizer/evaluator for @c double, which previously caused the same
 * engine to be compiled into nine separate object files (CLI dump, analyzer, perception,
 * the three webviz converters, and so on).  The result was severe object-code bloat and
 * long compile times.
 *
 * This header exposes the small subset of ExprTk that VLink actually uses behind two
 * opaque PIMPL classes.  The real ExprTk templates are compiled exactly once, inside the
 * @c vlink::exprtk_api shared library; consumers include only this header and link
 * @c vlink::exprtk_api, so ExprTk never enters their translation units again.
 *
 * @par Usage model
 * The classes mirror the ExprTk compile-then-evaluate workflow one-to-one:
 *   1. Build a @ref vlink::ExprtkSymbolTable, registering constants and variables.
 *      Variables are bound @b by-reference, so the caller-owned storage must outlive the
 *      symbol table and must not be relocated (e.g. reserve a @c std::vector up front).
 *   2. Create a @ref vlink::ExprtkExpression and register the symbol table into it.
 *   3. Call @ref vlink::ExprtkExpression::compile once.
 *   4. Update the bound variable storage and call @ref vlink::ExprtkExpression::value
 *      as many times as needed; each call re-reads the current variable values.
 *
 * @code
 * // Evaluate "a + b * 2" with a == 3, b == 4  ->  11
 * double a = 3.0;
 * double b = 4.0;
 *
 * vlink::ExprtkSymbolTable symbols;
 * symbols.add_constants();           // pi, epsilon, inf, ...
 * symbols.add_variable("a", a);      // bound by reference
 * symbols.add_variable("b", b);      // bound by reference
 *
 * vlink::ExprtkExpression expr;
 * expr.register_symbol_table(symbols);
 *
 * if (expr.compile("a + b * 2")) {
 *   double result = expr.value();    // 11.0
 *   b = 10.0;
 *   result = expr.value();           // 23.0, no recompilation needed
 * }
 * @endcode
 *
 * @note The value type is fixed to @c double, which matches every existing VLink call
 *       site.  Only a single @ref vlink::ExprtkSymbolTable may be registered into a given
 *       @ref vlink::ExprtkExpression, but one symbol table may be registered into many
 *       expressions to share variable storage.
 */

#pragma once

#undef VLINK_EXPRTK_API_EXPORT
#ifdef VLINK_EXPRTK_API_LIBRARY_STATIC
#define VLINK_EXPRTK_API_EXPORT
#elif defined(_WIN32) || defined(__CYGWIN__)
#ifdef VLINK_EXPRTK_API_LIBRARY
#define VLINK_EXPRTK_API_EXPORT __declspec(dllexport)
#else
#define VLINK_EXPRTK_API_EXPORT __declspec(dllimport)
#endif
#else
#define VLINK_EXPRTK_API_EXPORT __attribute__((visibility("default")))
#endif

#include <memory>
#include <string>

#include "../base/macros.h"

namespace vlink {

class ExprtkExpression;

/**
 * @class ExprtkSymbolTable
 * @brief Opaque wrapper around @c exprtk::symbol_table<double>.
 *
 * @details
 * Holds the named constants and variables an expression may reference.  Variables are
 * registered by reference: the symbol table stores the address of the supplied
 * @c double, so the referenced storage must stay alive and at a fixed address for the
 * whole lifetime of the symbol table and any expression compiled against it.
 *
 * The class is move-only; the underlying ExprTk symbol table lives on the heap behind a
 * @c std::unique_ptr, so moving the wrapper never relocates it.
 */
class VLINK_EXPRTK_API_EXPORT ExprtkSymbolTable final {
 public:
  /**
   * @brief Construct an empty symbol table.
   */
  ExprtkSymbolTable();

  /**
   * @brief Destroy the symbol table and release the underlying ExprTk state.
   */
  ~ExprtkSymbolTable();

  /**
   * @brief Move constructor; transfers ownership of the underlying ExprTk state.
   */
  ExprtkSymbolTable(ExprtkSymbolTable&&) noexcept;

  /**
   * @brief Move assignment; transfers ownership of the underlying ExprTk state.
   */
  ExprtkSymbolTable& operator=(ExprtkSymbolTable&&) noexcept;

  /**
   * @brief Register the standard ExprTk constants (@c pi, @c epsilon, @c inf, ...).
   *
   * @details Mirrors @c exprtk::symbol_table<double>::add_constants().
   */
  void add_constants();

  /**
   * @brief Register a named variable bound by reference.
   *
   * @param name  Identifier used inside expression strings.
   * @param value Reference to caller-owned storage; its address is captured, so it must
   *              outlive this table and must not be relocated after registration.
   * @return @c true if the variable was added, @c false on a duplicate or invalid name.
   */
  bool add_variable(const std::string& name, double& value);

 private:
  friend class ExprtkExpression;

  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(ExprtkSymbolTable)
};

/**
 * @class ExprtkExpression
 * @brief Opaque wrapper around @c exprtk::expression<double> plus its compiler.
 *
 * @details
 * Bundles a compiled expression and the one-shot parser used to build it.  Register a
 * @ref ExprtkSymbolTable, call @ref compile once, then call @ref value repeatedly; the
 * referenced symbol table must outlive the expression.
 *
 * The class is move-only; the underlying ExprTk expression lives on the heap behind a
 * @c std::unique_ptr, so moving the wrapper never relocates it.
 */
class VLINK_EXPRTK_API_EXPORT ExprtkExpression final {
 public:
  /**
   * @brief Construct an empty, uncompiled expression.
   */
  ExprtkExpression();

  /**
   * @brief Destroy the expression and release the underlying ExprTk state.
   */
  ~ExprtkExpression();

  /**
   * @brief Move constructor; transfers ownership of the underlying ExprTk state.
   */
  ExprtkExpression(ExprtkExpression&&) noexcept;

  /**
   * @brief Move assignment; transfers ownership of the underlying ExprTk state.
   */
  ExprtkExpression& operator=(ExprtkExpression&&) noexcept;

  /**
   * @brief Bind a symbol table to this expression.
   *
   * @param symbol_table The table whose constants and variables the expression resolves
   *                      against.  It must outlive this expression; only its address is
   *                      referenced.  Call before @ref compile.
   */
  void register_symbol_table(ExprtkSymbolTable& symbol_table);

  /**
   * @brief Compile an expression string against the registered symbol table.
   *
   * @param expression The ExprTk expression source to compile.
   * @return @c true on success; @c false if the string failed to parse, in which case
   *         @ref value must not be called.
   *
   * @note Intended to be called once per object.  A registered symbol table is required.
   */
  bool compile(const std::string& expression);

  /**
   * @brief Evaluate the compiled expression using the current variable values.
   *
   * @return The evaluated result.  Undefined if @ref compile has not returned @c true.
   */
  double value() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(ExprtkExpression)
};

}  // namespace vlink
