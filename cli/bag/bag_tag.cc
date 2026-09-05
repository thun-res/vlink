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

#include <vlink/base/helpers.h>
#include <vlink/base/utils.h>
#include <vlink/extension/bag_reader.h>
#include <vlink/vlink.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include "./bag_commands.h"
#include "./bag_common.h"

int bag_tag(const std::string& path, const std::string& tag_name) {
  is_play_mode = true;

  try {
#ifdef _WIN32
    auto filesys_path = std::filesystem::path(vlink::Helpers::string_to_wstring(path));
#else
    auto filesys_path = std::filesystem::path(path);
#endif

    if VUNLIKELY (!std::filesystem::exists(filesys_path)) {
      std::cerr << "The target file not exists." << std::endl;
      has_quit = true;
      return -1;
    }

    std::string suffix = filesys_path.extension().string();
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return std::tolower(c); });

    if VUNLIKELY (suffix == ".vcap") {
      std::cerr << "Tag is not supported for single vcap." << std::endl;
      has_quit = true;
      return -1;
    }
  } catch (std::filesystem::filesystem_error& e) {
    std::cerr << e.what() << std::endl;
    has_quit = true;
    return -1;
  }

  if VUNLIKELY (tag_name.empty()) {
    std::cerr << "Tag name can not be empty." << std::endl;
    has_quit = true;
    return -1;
  }

  std::shared_ptr<vlink::BagReader> player;

  try {
    player = vlink::BagReader::create(path, false);
  } catch (vlink::Exception::RuntimeError&) {
    has_quit = true;
    return -1;
  }

  is_split_mode = player->is_split_mode();

  auto quit_function = [player](int) {
    if VUNLIKELY (has_quit.exchange(true, std::memory_order_acq_rel)) {
      return;
    }

    is_broken = true;

    if VLIKELY (player) {
      player->stop();
      player->quit(true);
    }
  };

  vlink::Utils::register_terminate_signal(quit_function, true);

  player->register_idle_handler([player_ptr = player.get()]() { player_ptr->quit(); });

  player->tag(tag_name);

  player->run();

  has_quit = true;

  player.reset();

  return 0;
}
