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

#include <vlink/zerocopy/audio_frame.h>
#include <vlink/zerocopy/camera_frame.h>
#include <vlink/zerocopy/object_array.h>
#include <vlink/zerocopy/occupancy_grid.h>
#include <vlink/zerocopy/point_cloud.h>
#include <vlink/zerocopy/raw_data.h>
#include <vlink/zerocopy/tensor.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "dump_features.h"
#include "dump_types.h"

#ifdef VLINK_HAS_PROTOBUF_COMPILER

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/message.h>

void import_protos(google::protobuf::compiler::Importer* importer, const std::filesystem::path& root_dir,
                   const std::filesystem::path& sub_dir, bool& has_import, int depth = 0);

bool extract_proto_value(const google::protobuf::Message& message, const std::vector<std::string>& path_parts,
                         size_t depth, VariantType& result);

#endif

bool match_zerocopy_type(const std::string& ser, std::string_view type_name);

std::string format_zerocopy_header(const vlink::zerocopy::Header& header);

std::string format_raw_data(const vlink::zerocopy::RawData& raw_data);

std::string format_camera_frame(const vlink::zerocopy::CameraFrame& frame);

std::string format_point_cloud(const vlink::zerocopy::PointCloud& pc);

std::string format_occupancy_grid(const vlink::zerocopy::OccupancyGrid& og);

std::string format_tensor(const vlink::zerocopy::Tensor& tensor);

std::string format_object_array(const vlink::zerocopy::ObjectArray& arr);

std::string format_audio_frame(const vlink::zerocopy::AudioFrame& frame);

bool extract_zerocopy_header_value(const vlink::zerocopy::Header& header, const std::string& field,
                                   VariantType& result);

bool extract_zerocopy_value(const std::string& ser, const vlink::Bytes& bytes, const std::string& field,
                            VariantType& result);

std::string format_zerocopy_message(const std::string& ser, const vlink::Bytes& bytes);

vlink::Bytes extract_zerocopy_binary(const std::string& ser, const vlink::Bytes& bytes, const std::string& field);

bool write_pcd_file(const std::string& file_path, const vlink::zerocopy::PointCloud& pc);
