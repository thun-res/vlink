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

#pragma once

#include "./parse_features.h"

#ifdef VLINK_ENABLE_EXPRTK

#include <memory>
#include <string>
#include <vector>

#include "./parse_types.h"

#ifdef _WIN32
#undef min
#undef max
#undef GetMessage
#undef ERROR
#endif

namespace vlink::parse {

std::vector<std::string> sanitize_expr_var_names(const std::vector<std::string>& field_specs, bool warn_collisions);

class ExprContext final {
 public:
  ExprContext() = default;
  ExprContext(const ExprContext&) = delete;
  ExprContext& operator=(const ExprContext&) = delete;
  ExprContext(ExprContext&&) = delete;
  ExprContext& operator=(ExprContext&&) = delete;
  ~ExprContext() = default;

  bool compile(const std::vector<std::string>& field_specs, const std::vector<std::string>& expr_strings,
               bool warn_collisions = true);

  bool compile_single(const std::vector<std::string>& field_specs, const std::string& expr_string,
                      bool warn_collisions = true);

  void load_values(const std::vector<VariantType>& values);

  void set_value(size_t index, double value);

  double evaluate_single();

  std::vector<double> evaluate_all();

  bool ready() const { return compiled_; }

  size_t variable_count() const { return var_values_.size(); }

  const std::vector<std::string>& var_names() const { return var_names_; }

 private:
  struct Impl;

  std::shared_ptr<Impl> impl_;
  std::vector<std::string> var_names_;
  std::vector<double> var_values_;
  bool compiled_{false};
};

}  // namespace vlink::parse

#else

#include <string>
#include <vector>

#include "parse_types.h"

namespace vlink::parse {

std::vector<std::string> sanitize_expr_var_names(const std::vector<std::string>& field_specs, bool warn_collisions);

class ExprContext final {
 public:
  bool compile(const std::vector<std::string>& field_specs, const std::vector<std::string>& expr_strings,
               bool warn_collisions = true) const {
    (void)field_specs;
    (void)expr_strings;
    (void)warn_collisions;
    return enabled_;
  }

  bool compile_single(const std::vector<std::string>& field_specs, const std::string& expr_string,
                      bool warn_collisions = true) const {
    (void)field_specs;
    (void)expr_string;
    (void)warn_collisions;
    return enabled_;
  }

  void load_values(const std::vector<VariantType>& values) {
    (void)values;
    (void)enabled_;
  }

  void set_value(size_t index, double value) {
    (void)index;
    (void)value;
    (void)enabled_;
  }

  double evaluate_single() const { return enabled_ ? 1.0 : 0.0; }

  std::vector<double> evaluate_all() const { return enabled_ ? std::vector<double>{0.0} : std::vector<double>{}; }

  bool ready() const { return enabled_; }

  size_t variable_count() const { return enabled_ ? 1 : 0; }

  const std::vector<std::string>& var_names() const {
    static const std::vector<std::string> kEmpty;
    (void)enabled_;
    return kEmpty;
  }

 private:
  bool enabled_{false};
};

}  // namespace vlink::parse

#endif
