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

#include <vlink/base/logger.h>
#include <vlink/base/utils.h>
#include <vlink/version.h>

#include <argparse/argparse.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
  vlink::Utils::set_console_utf8_output();

  // arg parser
  argparse::ArgumentParser program("vlink-info", VLINK_VERSION, argparse::default_arguments::all);

  program.add_argument("-l", "--list_options").help("List options").default_value(false).implicit_value(true);
  program.add_epilog("Example:\n  vlink-info\n  vlink-info -l");

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    std::cerr << program << std::endl;
    return 1;
  }

  auto list_options = program.is_used("-l");

  if (list_options) {
    const std::string app_dir = vlink::Utils::get_app_dir();

    const std::vector<std::string> candidates = {
        app_dir + "/vlink-options.txt",
        app_dir + "/../etc/vlink/vlink-options.txt",
        app_dir + "/../share/vlink/vlink-options.txt",
    };

    std::string options_path;

    for (const auto& candidate : candidates) {
      try {
        if VLIKELY (std::filesystem::exists(candidate)) {
          options_path = std::filesystem::absolute(candidate).string();
          break;
        }
      } catch (const std::filesystem::filesystem_error&) {
        continue;
      }
    }

    if VUNLIKELY (options_path.empty()) {
      std::cerr << "Cannot find vlink-options.txt. Searched:" << std::endl;

      for (const auto& candidate : candidates) {
        std::cerr << "  " << candidate << std::endl;
      }

      return 1;
    }

    std::ifstream file(options_path);

    if VUNLIKELY (!file.is_open()) {
      std::cerr << "Cannot open options for path [" << options_path << "]." << std::endl;
      return 1;
    }

    std::string line;

    while (std::getline(file, line)) {
      std::cout << line << std::endl;
    }
  } else {
    std::cout << "┌──────── VLink Informations ────────────────────────────────────────────────────" << std::endl;
    std::cout << "│ Version:                     " << VLINK_VERSION << std::endl;
    std::cout << "│ Time stamp:                  " << VLINK_VERSION_TIMESTAMP << std::endl;
    std::cout << "│ Git tag:                     " << VLINK_VERSION_TAG << std::endl;
    std::cout << "│ Git commit-id:               " << VLINK_VERSION_COMMIT_ID << std::endl;
    std::cout << "└────────────────────────────────────────────────────────────────────────────────" << std::endl;
  }

  return 0;
}
