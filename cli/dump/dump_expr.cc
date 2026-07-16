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

#include "./dump_expr.h"

#ifdef VLINK_ENABLE_EXPRTK
#include <vlink/external/exprtk_api.h>
#endif

#include <iostream>
#include <unordered_set>

namespace vlink::dump {

std::vector<std::string> sanitize_expr_var_names(const std::vector<std::string>& field_specs, bool warn_collisions) {
  std::vector<std::string> names(field_specs.size());
  std::unordered_set<std::string> seen;

  for (size_t i = 0; i < field_specs.size(); ++i) {
    std::string var = field_specs[i];

    for (auto& ch : var) {
      if (ch == '.' || ch == '[' || ch == ']') {
        ch = '_';
      }
    }

    while (!var.empty() && var.back() == '_') {
      var.pop_back();
    }

    if (seen.count(var) != 0) {
      std::string base = var;

      for (int suffix = 2; seen.count(var) != 0; ++suffix) {
        var = base + "_" + std::to_string(suffix);
      }

      if (warn_collisions) {
        std::cerr << "Warning: variable name collision for field '" << field_specs[i] << "', renamed to '" << var << "'"
                  << std::endl;
      }
    }

    seen.emplace(var);
    names[i] = std::move(var);
  }

  return names;
}

#ifdef VLINK_ENABLE_EXPRTK

struct ExprContext::Impl final {
  vlink::ExprtkSymbolTable symbol_table;
  std::vector<vlink::ExprtkExpression> expressions;
};

bool ExprContext::compile(const std::vector<std::string>& field_specs, const std::vector<std::string>& expr_strings,
                          bool warn_collisions) {
  compiled_ = false;
  impl_ = std::make_shared<Impl>();
  var_names_ = sanitize_expr_var_names(field_specs, warn_collisions);
  var_values_.assign(field_specs.size(), 0.0);

  impl_->symbol_table.add_constants();

  for (size_t i = 0; i < var_names_.size(); ++i) {
    if VUNLIKELY (!impl_->symbol_table.add_variable(var_names_[i], var_values_[i])) {
      std::cerr << "Invalid or reserved expression variable name for field '" << field_specs[i]
                << "': " << var_names_[i] << std::endl;
      return false;
    }
  }

  impl_->expressions.resize(expr_strings.size());

  for (size_t e = 0; e < expr_strings.size(); ++e) {
    impl_->expressions[e].register_symbol_table(impl_->symbol_table);

    if VUNLIKELY (!impl_->expressions[e].compile(expr_strings[e])) {
      std::cerr << "Failed to compile expression: " << expr_strings[e] << std::endl;
      return false;
    }
  }

  compiled_ = true;
  return true;
}

bool ExprContext::compile_single(const std::vector<std::string>& field_specs, const std::string& expr_string,
                                 bool warn_collisions) {
  return compile(field_specs, {expr_string}, warn_collisions);
}

void ExprContext::load_values(const std::vector<VariantType>& values) {
  for (size_t i = 0; i < values.size() && i < var_values_.size(); ++i) {
    double dv = 0.0;

    if (!variant_to_double(values[i], dv)) {
      dv = 0.0;
    }

    var_values_[i] = dv;
  }
}

void ExprContext::set_value(size_t index, double value) {
  if (index < var_values_.size()) {
    var_values_[index] = value;
  }
}

double ExprContext::evaluate_single() {
  if VUNLIKELY (!compiled_ || impl_->expressions.empty()) {
    return 0.0;
  }

  return impl_->expressions.front().value();
}

std::vector<double> ExprContext::evaluate_all() {
  std::vector<double> results;

  if VUNLIKELY (!compiled_) {
    return results;
  }

  results.reserve(impl_->expressions.size());

  for (auto& expr : impl_->expressions) {
    results.emplace_back(expr.value());
  }

  return results;
}

#endif

}  // namespace vlink::dump
