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

#include <flatbuffers/flatbuffer_builder.h>

#include "../../mapping.h"

namespace vlink {
namespace webviz {

[[nodiscard]] bool write_foxglove_mapping(std::string_view schema, const FieldReader& fields,
                                          flatbuffers::FlatBufferBuilder& builder);
[[nodiscard]] bool write_foxglove_native(const std::string& ser, const Bytes& raw,
                                         flatbuffers::FlatBufferBuilder& builder, std::string& schema,
                                         int64_t& timestamp);
[[nodiscard]] std::string_view foxglove_schema(std::string_view name);
[[nodiscard]] bool validate_foxglove_mapping(const MessageMapping& mapping);

}  // namespace webviz
}  // namespace vlink
