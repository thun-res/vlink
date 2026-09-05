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

#include "./check_common.h"

int main(int argc, char* argv[]) {
  vlink::Utils::set_console_utf8_output();

  vlink::Logger::set_console_level(vlink::Logger::kWarn);
  vlink::Logger::set_file_level(vlink::Logger::kOff);
  vlink::Logger::init("check");

  argparse::ArgumentParser program("check", VLINK_VERSION, argparse::default_arguments::all);

  argparse::ArgumentParser diag_command("diag", VLINK_VERSION, argparse::default_arguments::help);
  diag_command.add_argument("-a", "--all").help("All case").default_value(false).implicit_value(true);
  diag_command.add_argument("-s", "--summary")
      .help("Print PASSED/WARNING/FAILED counts at the end")
      .default_value(false)
      .implicit_value(true);
  diag_command.add_argument("-f", "--filter")
      .help("Only run checks whose title contains the given substring")
      .default_value(std::string{});
  diag_command.add_description("Start automatic diagnosis");

  argparse::ArgumentParser env_command("env", VLINK_VERSION, argparse::default_arguments::help);
  env_command.add_argument("-b", "--available").help("Only available").default_value(false).implicit_value(true);
  env_command.add_argument("-p", "--prefix")
      .help("Only show variables whose name starts with the given prefix")
      .default_value(std::string{});
  env_command.add_description("Detect environment variables");

  argparse::ArgumentParser test_command("test", VLINK_VERSION, argparse::default_arguments::help);
  test_command.add_description("Run an intra:// pub/sub self-test");

  program.add_subparser(diag_command);
  program.add_subparser(env_command);
  program.add_subparser(test_command);

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;

    if (program.is_subcommand_used("diag")) {
      std::cerr << diag_command << std::endl;
    } else if (program.is_subcommand_used("env")) {
      std::cerr << env_command << std::endl;
    } else if (program.is_subcommand_used("test")) {
      std::cerr << test_command << std::endl;
    }

    return 1;
  }

  if (program.is_subcommand_used("diag")) {
    const bool all_case = diag_command.is_used("-a");
    const bool summary_case = diag_command.is_used("-s");
    const auto filter = diag_command.get<std::string>("-f");

    return check_diag(all_case, summary_case, filter);
  }

  if (program.is_subcommand_used("env")) {
    const bool available_case = env_command.is_used("-b");
    const auto prefix = env_command.get<std::string>("-p");

    return check_env(available_case, prefix);
  }

  if (program.is_subcommand_used("test")) {
    return check_test();
  }

  std::cerr << program << std::endl;

  return 1;
}
