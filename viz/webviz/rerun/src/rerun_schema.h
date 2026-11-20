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

#include <memory>
#include <rerun/component_descriptor.hpp>
#include <string_view>
#include <vector>

namespace arrow {
class DataType;
}

namespace vlink {
namespace webviz {

struct RerunField final {
  std::string_view archetype;
  std::string_view name;
  std::string_view component;
  const ::rerun::ComponentDescriptor* descriptor;
  const std::shared_ptr<arrow::DataType>& (*type)();
  bool batch;
};

struct RerunEnum final {
  std::string_view type;
  std::string_view name;
  uint64_t value;
};

[[nodiscard]] const std::vector<RerunField>& rerun_fields();
[[nodiscard]] const std::vector<RerunEnum>& rerun_enums();

}  // namespace webviz
}  // namespace vlink
