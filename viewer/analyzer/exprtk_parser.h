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

#ifdef VLINK_ENABLE_EXPRTK
#include <vlink/external/exprtk_api.h>
#endif

#include <optional>
#include <string>
#include <vector>

namespace vlink::Exprtk {  // NOLINT(readability-identifier-naming)

using VariableList = std::vector<std::pair<std::string, double>>;

class Parser {
 public:
  Parser(const std::string& expression, VariableList& variable_list);

  [[nodiscard]] std::optional<double> value() const;

 private:
#ifdef VLINK_ENABLE_EXPRTK
  vlink::ExprtkSymbolTable symbol_table_;
  vlink::ExprtkExpression expression_;
  bool compiled_{false};
#endif
};

}  // namespace vlink::Exprtk
