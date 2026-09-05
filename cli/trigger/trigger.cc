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

#include "./trigger_common.h"

int main(int argc, char* argv[]) {
  vlink::Utils::set_console_utf8_output();
  // vlink::Logger::set_console_level(vlink::Logger::kOff);
  // vlink::Logger::set_file_level(vlink::Logger::kOff);
  vlink::Logger::init("vlink-trigger");

  vlink::Utils::unset_env("VLINK_BAG_PATH");

  argparse::ArgumentParser program("vlink-trigger", VLINK_VERSION, argparse::default_arguments::all);
  program.add_description("VLink in-memory trigger recorder (event data recorder). Note: multicast/broadcast [" +
                          vlink::DiscoveryViewer::get_listen_address() + "] may be required.");

  argparse::ArgumentParser daemon_command("daemon", VLINK_VERSION, argparse::default_arguments::help);
  daemon_command.add_argument("-c", "--config").help("Optional config json path").default_value(std::string());
  daemon_command.add_argument("-n", "--native")
      .help("Native mode: local-host discovery + dds.ip from VLINK_DDS_NATIVE_IP (default 127.0.0.1)")
      .default_value(false)
      .implicit_value(true);
  daemon_command.add_argument("--bag_plugin")
      .help("Bag reorder plugin library name (overrides config)")
      .default_value(std::string());
  daemon_command.add_argument("--trigger_plugin")
      .help("Trigger lifecycle plugin library name (overrides config)")
      .default_value(std::string());
  daemon_command.add_argument("--trigger_plugin_config")
      .help("Opaque configuration string for the trigger plugin (overrides config)")
      .default_value(std::string());
  daemon_command.add_description("Run the trigger recorder daemon");
  daemon_command.add_epilog(
      "Example:\n  vlink-trigger daemon\n  vlink-trigger daemon -c /etc/vlink/trigger/trigger.json");

  argparse::ArgumentParser dump_command("dump", VLINK_VERSION, argparse::default_arguments::help);
  dump_command.add_argument("-m", "--method_url")
      .help("Daemon control-plane URL (matches the daemon's method_url)")
      .default_value(std::string(kDefaultMethodUrl));
  dump_command.add_argument("-o", "--out_file")
      .help("Output file path (empty: auto under dump_dir; external paths allowed by default)")
      .default_value(std::string());
  dump_command.add_argument("-r", "--reason").help("Trigger reason (stored as bag tag)").default_value(std::string());
  dump_command.add_argument("-n", "--name").help("Output file name hint").default_value(std::string());
  dump_command.add_argument("--pre")
      .help("Pre window ms (shrink only; -1 keeps configured)")
      .scan<'d', int64_t>()
      .default_value(static_cast<int64_t>(-1));
  dump_command.add_argument("--post")
      .help("Post window ms (shrink only; -1 keeps configured)")
      .scan<'d', int64_t>()
      .default_value(static_cast<int64_t>(-1));
  dump_command.add_argument("-u", "--urls")
      .help("Exact URL prefilter, empty is all")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  dump_command.add_argument("-i", "--filter")
      .help("URL keyword filter, comma-separated or quoted space-separated")
      .default_value(std::string());
  dump_command.add_argument("-k", "--black").help("Blacklist mode").default_value(false).implicit_value(true);
  dump_command.add_description("Trigger a dump on a running daemon");
  dump_command.add_epilog("Example:\n  vlink-trigger dump -r hard-brake -o /tmp/vlink-trigger/edr.vdb");

  program.add_subparser(daemon_command);
  program.add_subparser(dump_command);

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;

    if (program.is_subcommand_used("daemon")) {
      std::cerr << daemon_command << std::endl;
    } else if (program.is_subcommand_used("dump")) {
      std::cerr << dump_command << std::endl;
    } else {
      std::cerr << program << std::endl;
    }

    return 1;
  }

  if (program.is_subcommand_used("daemon")) {
    DaemonArguments arguments;
    arguments.config_path = daemon_command.get<std::string>("-c");
    arguments.native_mode = daemon_command.is_used("-n");

    if (daemon_command.is_used("--bag_plugin")) {
      arguments.bag_plugin_lib = daemon_command.get<std::string>("--bag_plugin");
    }

    if (daemon_command.is_used("--trigger_plugin")) {
      arguments.trigger_plugin_lib = daemon_command.get<std::string>("--trigger_plugin");
    }

    if (daemon_command.is_used("--trigger_plugin_config")) {
      arguments.trigger_plugin_config = daemon_command.get<std::string>("--trigger_plugin_config");
    }

    return run_daemon(arguments);
  }

  if (program.is_subcommand_used("dump")) {
    return run_dump(dump_command.get<std::string>("-m"), dump_command.get<std::string>("-o"),
                    dump_command.get<std::string>("-r"), dump_command.get<std::string>("-n"),
                    dump_command.get<int64_t>("--pre"), dump_command.get<int64_t>("--post"),
                    dump_command.get<std::vector<std::string>>("-u"), dump_command.get<std::string>("-i"),
                    dump_command.is_used("-k"));
  }

  std::cerr << program << std::endl;

  return 1;
}
