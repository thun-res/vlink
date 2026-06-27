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

#include <vlink/external/exprtk_api.h>

#include <exprtk/exprtk.hpp>
#include <string>

namespace vlink {

struct ExprtkSymbolTable::Impl final {
  exprtk::symbol_table<double> symbol_table;
};

struct ExprtkExpression::Impl final {
  exprtk::expression<double> expression;
  bool compiled{false};
};

ExprtkSymbolTable::ExprtkSymbolTable() : impl_(std::make_unique<Impl>()) {}

ExprtkSymbolTable::~ExprtkSymbolTable() = default;

ExprtkSymbolTable::ExprtkSymbolTable(ExprtkSymbolTable&&) noexcept = default;

ExprtkSymbolTable& ExprtkSymbolTable::operator=(ExprtkSymbolTable&&) noexcept = default;

void ExprtkSymbolTable::add_constants() { impl_->symbol_table.add_constants(); }

bool ExprtkSymbolTable::add_variable(const std::string& name, double& value) {
  return impl_->symbol_table.add_variable(name, value);
}

ExprtkExpression::ExprtkExpression() : impl_(std::make_unique<Impl>()) {}

ExprtkExpression::~ExprtkExpression() = default;

ExprtkExpression::ExprtkExpression(ExprtkExpression&&) noexcept = default;

ExprtkExpression& ExprtkExpression::operator=(ExprtkExpression&&) noexcept = default;

void ExprtkExpression::register_symbol_table(ExprtkSymbolTable& symbol_table) {
  impl_->expression.register_symbol_table(symbol_table.impl_->symbol_table);
}

bool ExprtkExpression::compile(const std::string& expression) {
  exprtk::parser<double> parser;
  impl_->compiled = parser.compile(expression, impl_->expression);

  return impl_->compiled;
}

double ExprtkExpression::value() const {
  if (!impl_->compiled) {
    return 0.0;
  }

  return impl_->expression.value();
}

}  // namespace vlink
