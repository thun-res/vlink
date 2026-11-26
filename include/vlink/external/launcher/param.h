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
 * @file param.h
 * @brief Strongly-typed parameter value and map aliases shared by Launcher components and control APIs.
 *
 * @details
 * Defines @c LauncherParamValue and @c LauncherParamMap with standard-library headers only,
 * allowing control-plane headers such as @c launcher_api.h to exchange typed parameters
 * without pulling in component runtime or pub/sub headers.
 *
 * @see launcher_component.h, launcher_api.h
 */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace vlink {

/**
 * @brief Scalar or homogeneous-list value stored in a launcher component parameter map.
 */
using LauncherParamValue = std::variant<bool, int64_t, double, std::string, std::vector<bool>, std::vector<int64_t>,
                                        std::vector<double>, std::vector<std::string>>;

/**
 * @brief Flat dot-delimited parameter table (e.g. @c "sensor.rate_hz").
 */
using LauncherParamMap = std::unordered_map<std::string, LauncherParamValue>;

}  // namespace vlink
