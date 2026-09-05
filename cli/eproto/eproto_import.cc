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

#include "./eproto_common.h"

#ifdef VLINK_HAS_PROTOBUF_COMPILER

[[maybe_unused]] void import_protos(google::protobuf::compiler::Importer* importer,
                                    const std::filesystem::path& root_dir, const std::filesystem::path& sub_dir,
                                    bool& has_import, int depth) {
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

  for (const auto& file : file_list) {
    if (file.is_regular_file() && file.path().extension() == ".proto") {
      try {
#ifdef _WIN32
        auto relative_path = vlink::Helpers::path_to_string(std::filesystem::relative(file.path(), root_dir));
        std::replace(relative_path.begin(), relative_path.end(), '\\', '/');
#else
        auto relative_path = std::filesystem::relative(file.path(), root_dir).string();
#endif
        const auto* ptr = importer->Import(relative_path);

        if (ptr) {
          has_import = true;
        }
      } catch (std::filesystem::filesystem_error&) {
        continue;
      }
    } else if (file.is_directory()) {
      import_protos(importer, root_dir, file.path(), has_import, depth + 1);
    }
  }
}

#endif
