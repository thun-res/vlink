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

#include <rerun.hpp>

#include "../../mapping.h"

namespace vlink {
namespace webviz {

[[nodiscard]] bool write_rerun(::rerun::RecordingStream& recording, const std::string& path, std::string_view archetype,
                               const FieldReader& fields, bool is_static = false);
[[nodiscard]] bool write_rerun_native(::rerun::RecordingStream& recording, const std::string& path,
                                      const std::string& ser, const Bytes& raw, std::string_view timeline = {});
[[nodiscard]] bool validate_rerun_mapping(const MessageMapping& mapping);

}  // namespace webviz
}  // namespace vlink
