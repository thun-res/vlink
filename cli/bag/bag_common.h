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

#include <vlink/base/elapsed_timer.h>
#include <vlink/base/functional.h>
#include <vlink/base/helpers.h>
#include <vlink/base/plugin.h>
#include <vlink/extension/bag_plugin_interface.h>
#include <vlink/extension/bag_reader.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#define VLINK_BAG_UNDEFINE_NOMINMAX
#endif
#include <windows.h>
#undef ERROR
#ifdef VLINK_BAG_UNDEFINE_NOMINMAX
#undef NOMINMAX
#undef VLINK_BAG_UNDEFINE_NOMINMAX
#endif
#elif defined(__APPLE__)
#include <unistd.h>
#endif

enum TimeMethod : uint8_t {
  kUseUnknown = 0,
  kUseRelTime = 1,
  kUseLocalTime = 2,
  kUseUtcTime = 4,
};

[[maybe_unused]] static constexpr int kCollectInterval{1000};
[[maybe_unused]] static constexpr int64_t kDayMilliseconds{24 * 60 * 60 * 1000};

extern std::atomic_bool has_intra_bind;
extern std::atomic_bool data_has_changed;
extern std::atomic_bool is_paused;
extern std::atomic_bool is_broken;
extern std::atomic_bool is_play_mode;
extern std::atomic<int64_t> pause_total_time;
extern std::atomic<double> play_rate;
extern std::atomic_bool is_split_mode;
extern std::atomic<int> split_count;
extern std::atomic_bool pause_to_next_flag;
extern std::atomic<int> compress_level;
extern std::atomic<int64_t> max_task_depth;
extern std::atomic<double> max_memory_size;
extern std::atomic<size_t> total_size;
extern std::atomic_bool skip_blank;
extern std::atomic<uint8_t> time_method;
extern std::atomic<int> play_loop_index;
extern std::atomic<int> play_loop_times;
extern std::atomic_bool has_quit;
extern bool quiet_flag;
extern bool detail_flag;
extern vlink::ElapsedTimer main_elapsed_timer;
extern vlink::ElapsedTimer pause_elapsed_timer;
extern vlink::Function<int64_t()> time_callback;
extern vlink::Function<int64_t()> split_index_callback;

void start_print(int64_t start_time, int64_t total_time, int64_t date_time, bool restart);
void stop_print();
void update_print();
void reset_print();
void print_progress(double progress);

bool clone_paths_overlap(const std::filesystem::path& source, const std::filesystem::path& target,
                         bool split_name_by_time);

inline int load_bag_plugin(vlink::Plugin& plugin, const std::string& plugin_name,
                           std::shared_ptr<vlink::BagPluginInterface>& plugin_interface) {
  plugin_interface.reset();

  if (plugin_name.empty()) {
    return 0;
  }

  plugin_interface = plugin.load<vlink::BagPluginInterface>(plugin_name, 2, 0);

  if VUNLIKELY (!plugin_interface) {
    std::cerr << "Failed to load plugin (" << plugin_name << ")." << std::endl;
    return -1;
  }

  return 0;
}

template <typename BagPtrT>
static int load_and_bind_bag_plugin(vlink::Plugin& plugin, const std::string& plugin_name, const BagPtrT& bag) {
  std::shared_ptr<vlink::BagPluginInterface> plugin_interface;

  if VUNLIKELY (load_bag_plugin(plugin, plugin_name, plugin_interface) != 0) {
    return -1;
  }

  if (plugin_interface) {
    bag->bind_bag_interface(plugin_interface);
  }

  return 0;
}

[[nodiscard]] inline std::filesystem::path bag_path_from_utf8(const std::string& path) {
#ifdef _WIN32
  return std::filesystem::path(vlink::Helpers::string_to_wstring(path));
#else
  return std::filesystem::path(path);
#endif
}

[[nodiscard]] inline std::vector<std::filesystem::path> bag_member_paths(const std::filesystem::path& path,
                                                                         bool cleanup) {
  std::vector<std::filesystem::path> files{path};
  std::string suffix = path.extension().string();
  std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return std::tolower(c); });

  if ((suffix != ".vdbx" && suffix != ".vcapx") || !std::filesystem::exists(path)) {
    return files;
  }

  try {
    nlohmann::json manifest;
    std::ifstream file(path);
    file >> manifest;

    for (const auto& item : manifest["VLinkFiles"]) {
      if (cleanup && !item.is_string()) {
        continue;
      }

      const auto member = bag_path_from_utf8(item.get_ref<const std::string&>());

      if (cleanup && (member.empty() || member == "." || member == ".." || member != member.filename())) {
        continue;
      }

      files.emplace_back(path.parent_path() / member);
    }
  } catch (const nlohmann::json::exception& e) {
    if (!cleanup) {
      throw;
    }

    std::cerr << "Cannot parse existing split manifest: " << e.what() << std::endl;
  } catch (const std::ios_base::failure& e) {
    throw std::filesystem::filesystem_error(e.what(), path, e.code());
  }

  return files;
}

[[nodiscard]] inline std::vector<std::filesystem::path> bag_source_paths(const std::filesystem::path& source) {
  auto source_files = bag_member_paths(source, false);
  std::string source_suffix = source.extension().string();
  std::transform(source_suffix.begin(), source_suffix.end(), source_suffix.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  if (source_suffix == ".vdb" || source_suffix == ".vdbx") {
    const size_t source_count = source_files.size();

    for (size_t i = source_suffix == ".vdbx" ? 1 : 0; i < source_count; ++i) {
      const auto database_path = std::filesystem::weakly_canonical(source_files[i]);
#ifdef _WIN32
      const bool has_alias = std::filesystem::is_symlink(source_files[i]);
#endif

      for (const auto* suffix : {"-wal", "-shm"}) {
        auto sidecar = database_path;
        sidecar += suffix;
        source_files.emplace_back(std::move(sidecar));
#ifdef _WIN32
        if (has_alias) {
          auto alias_sidecar = source_files[i];
          alias_sidecar += suffix;
          source_files.emplace_back(std::move(alias_sidecar));
        }
#endif
      }
    }
  }

  return source_files;
}

[[nodiscard]] inline bool bag_paths_contain(const std::vector<std::filesystem::path>& paths,
                                            const std::filesystem::path& candidate) {
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
