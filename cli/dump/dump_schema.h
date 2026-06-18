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

#include <vlink/extension/schema_plugin_manager.h>
#include <vlink/impl/types.h>

#include "dump_features.h"

#ifdef VLINK_HAS_PROTOBUF_COMPILER

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>

#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct SchemaRule final {
  std::string url;
  std::string ser_type;
  std::string schema_type_str;
};

struct SchemaConfig final {
  std::vector<std::string> proto_dirs;
  std::vector<std::string> fbs_dirs;
  std::vector<SchemaRule> rules;
};

bool load_schema_config(const std::string& config_path, SchemaConfig& config);

vlink::SchemaType parse_schema_type_hint(const std::string& schema_type_str);

struct UrlSchemaOverride final {
  std::string ser_type;
  vlink::SchemaType schema_type{vlink::SchemaType::kUnknown};
};

std::unordered_map<std::string, UrlSchemaOverride> build_url_schema_overrides(const SchemaConfig& config);

std::vector<std::string> collect_proto_dirs(const SchemaConfig& config, const std::string& extra_proto_dir);

struct ProtoRuntime final {
  std::shared_ptr<google::protobuf::compiler::DiskSourceTree> source_tree;
  std::shared_ptr<google::protobuf::compiler::Importer> importer;
  std::shared_ptr<google::protobuf::DynamicMessageFactory> factory;
  const google::protobuf::DescriptorPool* pool{nullptr};
  std::shared_ptr<vlink::SchemaPluginInterface> plugin;
};

ProtoRuntime load_proto_runtime(const std::vector<std::string>& proto_dirs);

std::string schema_data_key(const vlink::SchemaData& schema_data);

void collect_file_dependencies(const google::protobuf::FileDescriptor* file,
                               google::protobuf::FileDescriptorSet& fd_set, std::unordered_set<std::string>& visited);

std::vector<vlink::SchemaData> import_schemas_from_config(const SchemaConfig& config,
                                                          const std::string& extra_proto_dir,
                                                          const std::string& extra_fbs_dir);

#ifdef VLINK_HAS_FBS_COMPILER

#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/idl.h>

void import_fbs(std::shared_ptr<flatbuffers::Parser>& parser, const std::string& target_ser,
                const std::filesystem::path& root_dir, const std::filesystem::path& sub_dir, bool& has_import,
                int depth = 0);

#endif

#endif
