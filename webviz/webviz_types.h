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

#include <vlink/base/macros.h>

#include <exception>
#include <string>
#include <vector>

namespace vlink {
namespace webviz {

struct UrlSelector final {
  bool configured{false};
  std::vector<std::string> whitelist_exact;
  std::vector<std::string> whitelist_patterns;
  std::vector<std::string> blacklist_exact;
  std::vector<std::string> blacklist_patterns;
};

struct FieldMapping final {
  std::string source;
  std::string target;
  std::string expression;
  std::string default_value;
  bool has_default_value{false};
  bool default_value_is_string{false};
};

inline bool try_parse_numeric_default(const FieldMapping& mapping, double& value) {
  if VUNLIKELY (!mapping.has_default_value) {
    return false;
  }

  if VUNLIKELY (!mapping.default_value_is_string) {
    if VUNLIKELY (mapping.default_value == "true") {
      value = 1.0;
      return true;
    }

    if VUNLIKELY (mapping.default_value == "false" || mapping.default_value == "null") {
      value = 0.0;
      return true;
    }
  }

  try {
    value = std::stod(mapping.default_value);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

}  // namespace webviz
}  // namespace vlink
