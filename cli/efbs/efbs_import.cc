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

#include <flatbuffers/util.h>

#include <fstream>
#include <iterator>
#include <mutex>
#include <system_error>

#include "./efbs_common.h"

#ifdef _WIN32
static bool flatbuffers_utf8_file_exists(const char* name) {
  if (name == nullptr) {
    return false;
  }

  std::error_code ec;
  const auto path = utf8_to_path(name);
  return !path.empty() && std::filesystem::is_regular_file(path, ec) && !ec;
}

static bool flatbuffers_utf8_load_file(const char* name, bool binary, std::string* buf) {
  if (name == nullptr || buf == nullptr) {
    return false;
  }

  const auto path = utf8_to_path(name);

  if (path.empty()) {
    return false;
  }

  std::ifstream ifs(path, binary ? std::ios::binary : std::ios::in);

  if (!ifs.is_open()) {
    return false;
  }

  buf->assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
  return !ifs.bad();
}

static void ensure_flatbuffers_utf8_file_hooks() {
  static std::once_flag once_flag;
  std::call_once(once_flag, []() {
    flatbuffers::SetFileExistsFunction(flatbuffers_utf8_file_exists);
    flatbuffers::SetLoadFileFunction(flatbuffers_utf8_load_file);
  });
}
#endif

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
  if (parser || depth >= 100) {
    return;
  }

#ifdef _WIN32
  ensure_flatbuffers_utf8_file_hooks();
#endif

  std::vector<std::filesystem::directory_entry> file_list;

  try {
    for (const auto& entry : std::filesystem::directory_iterator(sub_dir)) {
      if VUNLIKELY (file_list.size() >= 1000) {
        break;
      }

      file_list.emplace_back(entry);
    }
  } catch (std::filesystem::filesystem_error&) {
    return;
  }

  if VUNLIKELY (file_list.empty()) {
    return;
  }

  std::string root_dir_str = vlink::Helpers::path_to_string(root_dir);
  std::string sub_dir_str = vlink::Helpers::path_to_string(sub_dir);

  const char* include_root_dirs[] = {root_dir_str.c_str(), nullptr};
  const char* include_dirs[] = {root_dir_str.c_str(), sub_dir_str.c_str(), nullptr};

  std::string schema_file;

  for (const auto& file : file_list) {
    std::error_code ec;

    if (file.is_regular_file(ec) && !ec && file.path().extension() == ".fbs") {
      try {
        std::ifstream fbs_stream(file.path(), std::ios::binary);

        if VUNLIKELY (!fbs_stream.is_open()) {
          continue;
        }

        schema_file.assign(std::istreambuf_iterator<char>(fbs_stream), std::istreambuf_iterator<char>());

        if VUNLIKELY (fbs_stream.bad()) {
          continue;
        }

        auto target_parser = std::make_shared<flatbuffers::Parser>();
        bool ret = (root_dir == sub_dir) ? target_parser->Parse(schema_file.c_str(), include_root_dirs)
                                         : target_parser->Parse(schema_file.c_str(), include_dirs);

        if (!ret) {
          continue;
        }

        if (target_parser->LookupStruct(target_ser)) {
          if (!target_parser->SetRootType(target_ser.c_str())) {
            continue;
          }

          parser = std::move(target_parser);
          has_import = true;
          return;
        }
      } catch (std::filesystem::filesystem_error&) {
        continue;
      }
    } else {
      ec.clear();
      const bool is_symlink = file.is_symlink(ec);

      if (!ec && !is_symlink && file.is_directory(ec) && !ec) {
        import_fbs(parser, target_ser, root_dir, file.path(), has_import, depth + 1);

        if (parser) {
          return;
        }
      }
    }
  }
}
