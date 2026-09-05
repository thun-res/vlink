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

#include "./efbs_common.h"

[[maybe_unused]] bool import_fbs_from_plugin(std::shared_ptr<flatbuffers::Parser>& parser,
                                             const std::shared_ptr<vlink::SchemaPluginInterface>& schema_interface,
                                             const std::string& target_ser) {
  if (!schema_interface) {
    return false;
  }

  auto* parser_ptr = static_cast<flatbuffers::Parser*>(schema_interface->create_flatbuffers_parser(target_ser));

  if (!parser_ptr) {
    return false;
  }

  parser = std::shared_ptr<flatbuffers::Parser>(schema_interface, parser_ptr);
  return true;
}

[[maybe_unused]] void import_fbs(std::shared_ptr<flatbuffers::Parser>& parser, const std::string& target_ser,
                                 const std::filesystem::path& root_dir, const std::filesystem::path& sub_dir,
                                 bool& has_import, int depth) {
  if (parser) {
    return;
  }

  std::shared_ptr<flatbuffers::Parser> target_parser = std::make_shared<flatbuffers::Parser>();

  if VUNLIKELY (depth >= 100) {
    return;
  }

  std::vector<std::filesystem::directory_entry> file_list;

  try {
    for (const auto& entry : std::filesystem::directory_iterator(sub_dir)) {
      file_list.emplace_back(entry);
    }
  } catch (std::filesystem::filesystem_error&) {
    return;
  }

  if VUNLIKELY (file_list.empty() || file_list.size() > 1000) {
    return;
  }

  std::string root_dir_str = root_dir.string();
  std::string sub_dir_str = sub_dir.string();

  const char* include_root_dirs[] = {root_dir_str.c_str(), nullptr};
  const char* include_dirs[] = {root_dir_str.c_str(), sub_dir_str.c_str(), nullptr};

  bool ret = false;
  std::string schema_file;

  for (const auto& file : file_list) {
    if (file.is_regular_file() && file.path().extension() == ".fbs") {
      try {
        ret = flatbuffers::LoadFile(file.path().string().c_str(), false, &schema_file);

        if (!ret) {
          continue;
        }

        if (root_dir == sub_dir) {
          ret = target_parser->Parse(schema_file.c_str(), include_root_dirs);
        } else {
          ret = target_parser->Parse(schema_file.c_str(), include_dirs);
        }

        if (!ret) {
          continue;
        }

        if (target_parser->LookupStruct(target_ser)) {
          target_parser->SetRootType(target_ser.c_str());
          parser = std::move(target_parser);
          has_import = true;
          return;
        }
      } catch (std::filesystem::filesystem_error&) {
        continue;
      }
    } else if (file.is_directory()) {
      import_fbs(parser, target_ser, root_dir, file.path(), has_import, depth + 1);
    }
  }
}
