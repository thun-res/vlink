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

#include "dump_schema.h"

#ifdef VLINK_HAS_PROTOBUF_COMPILER

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <system_error>

#include "dump_context.h"
#include "dump_extract.h"

bool load_schema_config(const std::string& config_path, SchemaConfig& config) {
  std::ifstream file(config_path);

  if VUNLIKELY (!file.is_open()) {
    std::cerr << "Failed to open schema config: " << config_path << std::endl;
    return false;
  }

  try {
    auto json = nlohmann::json::parse(file);

    if (json.contains("proto_dirs") && json["proto_dirs"].is_array()) {
      for (const auto& dir : json["proto_dirs"]) {
        if (dir.is_string()) {
          config.proto_dirs.emplace_back(dir.get<std::string>());
        }
      }
    }

    if (json.contains("fbs_dirs") && json["fbs_dirs"].is_array()) {
      for (const auto& dir : json["fbs_dirs"]) {
        if (dir.is_string()) {
          config.fbs_dirs.emplace_back(dir.get<std::string>());
        }
      }
    }

    if (json.contains("rules") && json["rules"].is_array()) {
      for (const auto& rule : json["rules"]) {
        SchemaRule r;
        r.url = rule.value("url", "");
        r.ser_type = rule.value("ser_type", "");
        r.schema_type_str = rule.value("schema_type", "");

        if (!r.url.empty() && !r.ser_type.empty()) {
          config.rules.emplace_back(std::move(r));
        }
      }
    }
  } catch (const nlohmann::json::exception& e) {
    std::cerr << "Failed to parse schema config: " << e.what() << std::endl;
    return false;
  }

  return true;
}

vlink::SchemaType parse_schema_type_hint(const std::string& schema_type_str) {
  std::string lower = schema_type_str;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });

  if (lower == "protobuf" || lower == "proto") {
    return vlink::SchemaType::kProtobuf;
  }

  if (lower == "flatbuffers" || lower == "flatbuffer" || lower == "fbs") {
    return vlink::SchemaType::kFlatbuffers;
  }

  if (lower == "zerocopy" || lower == "zero_copy") {
    return vlink::SchemaType::kZeroCopy;
  }

  if (lower == "raw") {
    return vlink::SchemaType::kRaw;
  }

  return vlink::SchemaType::kUnknown;
}

std::unordered_map<std::string, UrlSchemaOverride> build_url_schema_overrides(const SchemaConfig& config) {
  std::unordered_map<std::string, UrlSchemaOverride> overrides;

  for (const auto& rule : config.rules) {
    if (!rule.url.empty() && !rule.ser_type.empty()) {
      overrides[rule.url] = UrlSchemaOverride{rule.ser_type, parse_schema_type_hint(rule.schema_type_str)};
    }
  }

  return overrides;
}

std::vector<std::string> collect_proto_dirs(const SchemaConfig& config, const std::string& extra_proto_dir) {
  std::vector<std::string> proto_dirs = config.proto_dirs;

  if (!extra_proto_dir.empty()) {
    proto_dirs.emplace_back(extra_proto_dir);
  }

  return proto_dirs;
}

ProtoRuntime load_proto_runtime(const std::vector<std::string>& proto_dirs) {
  ProtoRuntime runtime;
  bool has_import = false;

  for (const auto& proto_dir : proto_dirs) {
    auto filesys_dir = std::filesystem::path(proto_dir);
    std::error_code fs_ec;

    if (!std::filesystem::exists(filesys_dir, fs_ec) || fs_ec || !std::filesystem::is_directory(filesys_dir, fs_ec) ||
        fs_ec) {
      continue;
    }

    if (!runtime.factory) {
      runtime.factory = std::make_shared<google::protobuf::DynamicMessageFactory>();
      runtime.source_tree = std::make_shared<google::protobuf::compiler::DiskSourceTree>();
      runtime.importer = std::make_shared<google::protobuf::compiler::Importer>(runtime.source_tree.get(), nullptr);
    }

    runtime.source_tree->MapPath("", filesys_dir.string());
    import_protos(runtime.importer.get(), filesys_dir, filesys_dir, has_import);
  }

  if (has_import && runtime.importer) {
    runtime.pool = runtime.importer->pool();
  }

  auto plugin = vlink::SchemaPluginManager::get().get_interface();

  if (plugin) {
    runtime.plugin = plugin;

    if (!runtime.factory) {
      runtime.factory = std::make_shared<google::protobuf::DynamicMessageFactory>();
    }
  }

  return runtime;
}

std::string schema_data_key(const vlink::SchemaData& schema_data) {
  auto resolved = vlink::SchemaData::resolve_type(schema_data.schema_type, schema_data.name, schema_data.encoding);
  return schema_data.name + ":" + std::to_string(static_cast<int>(resolved)) + ":" + schema_data.encoding;
}

void collect_file_dependencies(const google::protobuf::FileDescriptor* file,
                               google::protobuf::FileDescriptorSet& fd_set, std::unordered_set<std::string>& visited) {
  if VUNLIKELY (!file) {
    return;
  }

  std::string file_name(file->name());

  if VUNLIKELY (visited.count(file_name) != 0) {
    return;
  }

  visited.emplace(file_name);

  for (int i = 0; i < file->dependency_count(); ++i) {
    collect_file_dependencies(file->dependency(i), fd_set, visited);
  }

  file->CopyTo(fd_set.add_file());
}

std::vector<vlink::SchemaData> import_schemas_from_config(const SchemaConfig& config,
                                                          const std::string& extra_proto_dir,
                                                          const std::string& extra_fbs_dir) {
  std::vector<vlink::SchemaData> result;

  auto all_proto_dirs = collect_proto_dirs(config, extra_proto_dir);

  std::vector<std::string> all_fbs_dirs = config.fbs_dirs;

  if (!extra_fbs_dir.empty()) {
    all_fbs_dirs.emplace_back(extra_fbs_dir);
  }

  std::unordered_set<std::string> imported_names;

  auto plugin = vlink::SchemaPluginManager::get().get_interface();

  if (plugin) {
    for (const auto& rule : config.rules) {
      if (imported_names.count(rule.ser_type) != 0) {
        continue;
      }

      auto hint = parse_schema_type_hint(rule.schema_type_str);
      auto schema = plugin->search_schema(rule.ser_type, hint);

      if (!schema.data.empty() && (hint == vlink::SchemaType::kUnknown || schema.schema_type == hint)) {
        result.emplace_back(std::move(schema));
        imported_names.emplace(rule.ser_type);
      }
    }
  }

  for (const auto& proto_dir : all_proto_dirs) {
    std::error_code fs_ec;

    if (!std::filesystem::exists(proto_dir, fs_ec) || fs_ec || !std::filesystem::is_directory(proto_dir, fs_ec) ||
        fs_ec) {
      continue;
    }

    auto source_tree = std::make_shared<google::protobuf::compiler::DiskSourceTree>();
    source_tree->MapPath("", proto_dir);

    auto importer = std::make_shared<google::protobuf::compiler::Importer>(source_tree.get(), nullptr);

    bool has_import = false;
    auto filesys_proto_dir = std::filesystem::path(proto_dir);
    import_protos(importer.get(), filesys_proto_dir, filesys_proto_dir, has_import);

    if (!has_import) {
      continue;
    }

    const auto* pool = importer->pool();

    for (const auto& rule : config.rules) {
      if (parse_schema_type_hint(rule.schema_type_str) != vlink::SchemaType::kProtobuf) {
        continue;
      }

      if (imported_names.count(rule.ser_type) != 0) {
        continue;
      }

      const auto* descriptor = pool->FindMessageTypeByName(rule.ser_type);

      if (!descriptor) {
        continue;
      }

      google::protobuf::FileDescriptorSet fd_set;
      std::unordered_set<std::string> visited;
      collect_file_dependencies(descriptor->file(), fd_set, visited);

      std::string serialized;

      if VUNLIKELY (!fd_set.SerializeToString(&serialized)) {
        continue;
      }

      vlink::SchemaData schema;
      schema.name = rule.ser_type;
      schema.encoding = "protobuf";
      schema.schema_type = vlink::SchemaType::kProtobuf;
      schema.data = vlink::Bytes::deep_copy(reinterpret_cast<const uint8_t*>(serialized.data()), serialized.size());

      result.emplace_back(std::move(schema));
      imported_names.emplace(rule.ser_type);

      if (!vlink::dump::DumpContext::get().quiet_flag) {
        std::cout << "Imported schema: " << rule.ser_type << " (protobuf)" << std::endl;
      }
    }
  }

#ifdef VLINK_HAS_FBS_COMPILER

  for (const auto& fbs_dir : all_fbs_dirs) {
    std::error_code fbs_ec;

    if (!std::filesystem::exists(fbs_dir, fbs_ec) || fbs_ec || !std::filesystem::is_directory(fbs_dir, fbs_ec) ||
        fbs_ec) {
      continue;
    }

    for (const auto& rule : config.rules) {
      if (parse_schema_type_hint(rule.schema_type_str) != vlink::SchemaType::kFlatbuffers) {
        continue;
      }

      if (imported_names.count(rule.ser_type) != 0) {
        continue;
      }

      std::shared_ptr<flatbuffers::Parser> parser;
      bool has_import = false;
      auto fbs_path = std::filesystem::path(fbs_dir);
      import_fbs(parser, rule.ser_type, fbs_path, fbs_path, has_import);

      if (!parser) {
        continue;
      }

      parser->Serialize();
      const auto& buf = parser->builder_.GetBufferPointer();
      auto buf_size = parser->builder_.GetSize();

      if (buf && buf_size > 0) {
        vlink::SchemaData schema;
        schema.name = rule.ser_type;
        schema.encoding = "flatbuffers";
        schema.schema_type = vlink::SchemaType::kFlatbuffers;
        schema.data = vlink::Bytes::deep_copy(buf, buf_size);

        result.emplace_back(std::move(schema));
        imported_names.emplace(rule.ser_type);

        if (!vlink::dump::DumpContext::get().quiet_flag) {
          std::cout << "Imported schema: " << rule.ser_type << " (flatbuffers)" << std::endl;
        }
      }
    }
  }

#endif

  return result;
}

#ifdef VLINK_HAS_FBS_COMPILER

static constexpr int kMaxSchemaImportDepth = 100;
static constexpr size_t kMaxSchemaDirEntries = 1000;

void import_fbs(std::shared_ptr<flatbuffers::Parser>& parser, const std::string& target_ser,
                const std::filesystem::path& root_dir, const std::filesystem::path& sub_dir, bool& has_import,
                int depth) {
  if VUNLIKELY (parser || depth >= kMaxSchemaImportDepth) {
    return;
  }

  auto target_parser = std::make_shared<flatbuffers::Parser>();

  std::vector<std::filesystem::directory_entry> file_list;

  try {
    for (const auto& entry : std::filesystem::directory_iterator(sub_dir)) {
      file_list.emplace_back(entry);
    }
  } catch (std::filesystem::filesystem_error&) {
    return;
  }

  if VUNLIKELY (file_list.empty() || file_list.size() > kMaxSchemaDirEntries) {
    return;
  }

  std::string root_dir_str = root_dir.string();
  std::string sub_dir_str = sub_dir.string();
  const char* include_root_dirs[] = {root_dir_str.c_str(), nullptr};
  const char* include_dirs[] = {root_dir_str.c_str(), sub_dir_str.c_str(), nullptr};

  std::string schema_file;

  for (const auto& file : file_list) {
    try {
      if (file.is_regular_file() && file.path().extension() == ".fbs") {
        if VUNLIKELY (!flatbuffers::LoadFile(file.path().string().c_str(), false, &schema_file)) {
          continue;
        }

        bool ret = (root_dir == sub_dir) ? target_parser->Parse(schema_file.c_str(), include_root_dirs)
                                         : target_parser->Parse(schema_file.c_str(), include_dirs);

        if VUNLIKELY (!ret) {
          continue;
        }

        if VUNLIKELY (target_parser->LookupStruct(target_ser)) {
          if (!target_parser->SetRootType(target_ser.c_str())) {
            continue;
          }

          parser = std::move(target_parser);
          has_import = true;
          return;
        }
      } else if (file.is_directory()) {
        import_fbs(parser, target_ser, root_dir, file.path(), has_import, depth + 1);
      }
    } catch (std::filesystem::filesystem_error&) {
      continue;
    }
  }
}

#endif

#endif
