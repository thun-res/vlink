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

#include <vlink/zerocopy/message_parser.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "./parse_features.h"
#include "./parse_types.h"

#ifdef VLINK_HAS_PROTOBUF_COMPILER

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/message.h>

void import_protos(google::protobuf::compiler::Importer* importer, const std::filesystem::path& root_dir,
                   const std::filesystem::path& sub_dir, bool& has_import, int depth = 0);

bool extract_proto_value(const google::protobuf::Message& message, const std::vector<std::string>& path_parts,
                         size_t depth, VariantType& result);

#endif

bool extract_zerocopy_value(const std::string& ser, const vlink::Bytes& bytes, const std::string& field,
                            VariantType& result);

bool extract_zerocopy_value(const vlink::zerocopy::MessageParser& parser, const std::string& field,
                            VariantType& result);

std::string format_zerocopy_message(const std::string& ser, const vlink::Bytes& bytes);

vlink::Bytes extract_zerocopy_binary(const vlink::zerocopy::MessageParser& parser, const std::string& field);

bool write_pcd_file(const std::string& file_path, const vlink::zerocopy::PointCloud& point_cloud);
