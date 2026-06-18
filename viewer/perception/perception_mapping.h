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

// NOLINTBEGIN

#pragma once

#include "./perception_config.h"
#include "./perception_model.h"

namespace google {
namespace protobuf {
class Message;
}  // namespace protobuf
}  // namespace google

namespace flatbuffers {
class Table;
}  // namespace flatbuffers

namespace reflection {
struct Schema;
struct Object;
}  // namespace reflection

namespace perception {
namespace mapping {

void decode_proto(const google::protobuf::Message& root, const PerceptionConfig::MappingRule& rule, Layer& out);

void decode_fbs(const flatbuffers::Table& root, const reflection::Schema& schema, const reflection::Object& root_obj,
                const PerceptionConfig::MappingRule& rule, Layer& out);

void decode_hud_proto(const google::protobuf::Message& root, const PerceptionConfig::MappingRule& rule,
                      std::vector<HudField>& out);

void decode_hud_fbs(const flatbuffers::Table& root, const reflection::Schema& schema,
                    const reflection::Object& root_obj, const PerceptionConfig::MappingRule& rule,
                    std::vector<HudField>& out);

}  // namespace mapping
}  // namespace perception

// NOLINTEND
