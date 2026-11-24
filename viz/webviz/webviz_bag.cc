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

#include "./webviz_bag.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <vector>

#include "./webviz_app_utils.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <unistd.h>
#endif

namespace vlink {
namespace webviz {

static std::vector<std::filesystem::path> bag_source_paths(const std::filesystem::path& source) {
  std::vector<std::filesystem::path> source_files{source};
  std::string suffix = source.extension().string();
  std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return std::tolower(c); });

  if (suffix == ".vdbx" || suffix == ".vcapx") {
    std::ifstream file(source);
    nlohmann::json manifest;
    file >> manifest;

    for (const auto& item : manifest.at("VLinkFiles")) {
      source_files.emplace_back(source.parent_path() / utf8_to_path(item.get_ref<const std::string&>()));
    }
  }

  if (suffix == ".vdb" || suffix == ".vdbx") {
    const size_t source_count = source_files.size();

    for (size_t i = suffix == ".vdbx" ? 1 : 0; i < source_count; ++i) {
      const auto database_path = std::filesystem::weakly_canonical(source_files[i]);
#ifdef _WIN32
      const bool has_alias = std::filesystem::is_symlink(source_files[i]);
#endif

      for (const auto* sidecar_suffix : {"-wal", "-shm"}) {
        auto sidecar = database_path;
        sidecar += sidecar_suffix;
        source_files.emplace_back(std::move(sidecar));
#ifdef _WIN32
        if (has_alias) {
          auto alias_sidecar = source_files[i];
          alias_sidecar += sidecar_suffix;
          source_files.emplace_back(std::move(alias_sidecar));
        }
#endif
      }
    }
  }

  return source_files;
}

static bool bag_paths_contain(const std::vector<std::filesystem::path>& paths, const std::filesystem::path& candidate) {
  auto target = std::filesystem::absolute(candidate);
  bool exists = std::filesystem::exists(target);

  while (!exists && std::filesystem::is_symlink(target)) {
    target = target.parent_path() / std::filesystem::read_symlink(target);
    exists = std::filesystem::exists(target);
  }

  const auto resolved = std::filesystem::weakly_canonical(target);
#if defined(_WIN32) || defined(__APPLE__)
  std::string suffix;

  if (!exists) {
    const auto name = vlink::Helpers::path_to_string(resolved.filename());

    if (name.size() > 4 && name[name.size() - 4] == '-') {
      suffix = name.substr(name.size() - 4);
      std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return std::tolower(c); });
    }
  }

  const bool missing_sidecar = suffix == "-wal" || suffix == "-shm";
#endif

  for (const auto& input : paths) {
    const auto source = std::filesystem::weakly_canonical(std::filesystem::absolute(input));

    if (source == resolved ||
        (exists && std::filesystem::exists(input) && std::filesystem::equivalent(input, candidate))) {
      return true;
    }

#if defined(_WIN32) || defined(__APPLE__)
    if (missing_sidecar) {
#ifdef _WIN32
      if (::CompareStringOrdinal(source.filename().c_str(), -1, resolved.filename().c_str(), -1, TRUE) != CSTR_EQUAL) {
        continue;
      }
#else
      const auto name = source.filename().native();

      if (!vlink::Helpers::has_endwith(name, suffix) ||
          (!vlink::Helpers::has_endwith(resolved.native(), suffix) &&
           ::pathconf(source.parent_path().c_str(), _PC_CASE_SENSITIVE) == 1)) {
        continue;
      }

      const auto db = source.parent_path() / name.substr(0, name.size() - suffix.size());
      auto output = resolved.native();
      output.resize(output.size() - suffix.size());

      if (!std::filesystem::exists(db) || !std::filesystem::exists(output) ||
          !std::filesystem::equivalent(db, output)) {
        continue;
      }
#endif

      if (std::filesystem::exists(resolved.parent_path()) &&
          std::filesystem::equivalent(source.parent_path(), resolved.parent_path())) {
        return true;
      }
    }
#endif
  }

  return false;
}

std::shared_ptr<BagReader> open_conversion_bag(const std::string& input_path, const std::string& output_path) {
  try {
    const auto source = utf8_to_path(input_path);
    const auto target = utf8_to_path(output_path);

    const auto sources = bag_source_paths(source);
    auto reader = BagReader::create(input_path);

    if VUNLIKELY (!reader) {
      std::cerr << "Failed to open bag file: " << input_path << std::endl;
      return nullptr;
    }

    if VUNLIKELY (bag_paths_contain(sources, target)) {
      std::cerr << "Input and output refer to the same file: " << output_path << std::endl;
      return nullptr;
    }

    return reader;
  } catch (const std::exception& e) {
    std::cerr << "Failed to prepare conversion from " << input_path << " to " << output_path << ": " << e.what()
              << std::endl;
    return nullptr;
  }
}

}  // namespace webviz
}  // namespace vlink
