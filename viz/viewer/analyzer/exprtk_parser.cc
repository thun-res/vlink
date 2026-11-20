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

#include "./exprtk_parser.h"

#include <string>

namespace vlink::Exprtk {

#ifdef VLINK_ENABLE_EXPRTK

Parser::Parser(const std::string& expression, VariableList& variable_list) {
  symbol_table_.add_constants();

  for (auto& [name, value] : variable_list) {
    symbol_table_.add_variable(name, value);
  }

  expression_.register_symbol_table(symbol_table_);
  compiled_ = expression_.compile(expression);
}

std::optional<double> Parser::value() const {
  if (!compiled_) {
    return std::nullopt;
  }

  return expression_.value();
}

#else

Parser::Parser(const std::string& expression, VariableList& variable_list) {
  (void)expression;
  (void)variable_list;
}

std::optional<double> Parser::value() const { return std::nullopt; }

#endif

}  // namespace vlink::Exprtk
