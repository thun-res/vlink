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
#include <vlink/base/logger.h>
#include <vlink/base/macros.h>
#include <vlink/base/utils.h>
#include <vlink/version.h>

#include <argparse/argparse.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <vector>

#include "proxy_config.h"
#include "rerun_server.h"
#include "webviz_app_utils.h"

using Json = nlohmann::json;

int main(int argc, char* argv[]) {
  vlink::Utils::set_console_utf8_output();

  argparse::ArgumentParser program("vlink-rerun", VLINK_VERSION, argparse::default_arguments::all);
  program.add_description("Bridge VLink topics to Rerun using proxy_api or local proxy_server mode.");

  program.add_argument("-m", "--mode")
      .help("Rerun mode: spawn, connect, serve, save")
      .default_value(std::string("spawn"));

  program.add_argument("-a", "--address")
      .help("gRPC address for connect mode")
      .default_value(std::string("rerun+http://127.0.0.1:9876/proxy"));

  program.add_argument("--bind_ip").help("Bind IP for serve mode").default_value(std::string("0.0.0.0"));

  program.add_argument("-p", "--port").help("Port for serve mode").default_value(9876).scan<'i', int>();

  program.add_argument("--save_path").help("Save path for save mode (.rrd file)").default_value(std::string(""));

  program.add_argument("--recording_id").help("Optional Rerun recording ID").default_value(std::string(""));

  program.add_argument("-c", "--config")
      .help("Path to root config file (rerun_config.json)")
      .default_value(std::string(""));

  program.add_argument("--name").help("Rerun server name").default_value(std::string("vlink-rerun"));

  program.add_argument("--proto_dir")
      .help("Directory containing VLink .proto files for dynamic parsing")
      .default_value(std::string(""));

  program.add_argument("--fbs_dir")
      .help("Directory containing VLink .fbs files for dynamic FlatBuffers parsing")
      .default_value(std::string(""));

  program.add_argument("--schema_plugin")
      .help(
          "Path to schema plugin shared library (imports protobuf/flatbuffers schemas; alternative to "
          "--proto_dir/--fbs_dir)")
      .default_value(std::string(""));

  program.add_argument("--convert_plugin")
      .help("Path to a VLink/webviz conversion plugin shared library")
      .default_value(std::string(""));

  program.add_argument("--convert_plugin_config")
      .help("Configuration string for the convert plugin")
      .default_value(std::string(""));

  program.add_argument("--vlink_msgs")
      .help("Path(s) to vlink_msgs mapping files")
      .nargs(argparse::nargs_pattern::any)
      .default_value(std::vector<std::string>{});

  program.add_argument("--spawn_memory_limit")
      .help("Viewer memory limit for spawn mode (for example 16GB or 75%)")
      .default_value(std::string("75%"));

  program.add_argument("--spawn_server_memory_limit")
      .help("gRPC server memory limit inside the spawned viewer")
      .default_value(std::string("1GiB"));

  program.add_argument("--spawn_hide_welcome_screen")
      .help("Hide the Rerun welcome screen in spawn mode")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--spawn_no_detach")
      .help("Do not detach the spawned Rerun process")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--spawn_executable_name")
      .help("Executable name used in spawn mode")
      .default_value(std::string("rerun"));

  program.add_argument("--spawn_executable_path")
      .help("Absolute executable path used in spawn mode")
      .default_value(std::string(""));

  program.add_argument("--serve_memory_limit")
      .help("gRPC server memory limit for serve mode")
      .default_value(std::string("1GiB"));

  program.add_argument("--playback_behavior")
      .help("Serve playback behavior: oldest_first or newest_first")
      .default_value(std::string("oldest_first"));

  program.add_argument("--sequence_timeline").help("Sequence timeline name").default_value(std::string("seq"));

  program.add_argument("--timestamp_timeline").help("Timestamp timeline name").default_value(std::string("timestamp"));

  program.add_argument("--disable_sequence_timeline")
      .help("Disable sequence timeline logging")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--disable_timestamp_timeline")
      .help("Disable timestamp timeline logging")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--allow_multiple")
      .help("Allow multiple instances to run simultaneously")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("-i", "--filter")
      .help("Case-insensitive substring filter for url, separated by spaces or commas")
      .default_value(std::string(""));

  program.add_argument("-k", "--black")
      .help("Use -i/--filter as blacklist mode")
      .default_value(false)
      .implicit_value(true);

  vlink::webviz::ProxyConfigHelper::add_arguments(program);

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    std::cerr << program;
    return 1;
  }

  if VUNLIKELY (!program.get<bool>("--allow_multiple") && !vlink::Utils::check_singleton("vlink-rerun")) {
    std::cerr << "Program has started." << std::endl;
    return 1;
  }

  auto proto_dir = vlink::webviz::resolve_arg_or_env(program.get<std::string>("--proto_dir"), "VLINK_PROTO_DIR");
  auto fbs_dir = vlink::webviz::resolve_arg_or_env(program.get<std::string>("--fbs_dir"), "VLINK_FBS_DIR");
  auto schema_plugin_path =
      vlink::webviz::resolve_arg_or_env(program.get<std::string>("--schema_plugin"), "VLINK_SCHEMA_PLUGIN");
  auto convert_plugin_path =
      vlink::webviz::resolve_arg_or_env(program.get<std::string>("--convert_plugin"), "VLINK_CONVERT_PLUGIN");

  auto convert_plugin_config = program.get<std::string>("--convert_plugin_config");
  auto vlink_msgs = program.get<std::vector<std::string>>("--vlink_msgs");
  auto config_file = program.get<std::string>("--config");
  std::string config_error;
  int config_port = program.get<int>("--port");

  if VUNLIKELY (program.is_used("--config")) {
    std::error_code ec;

    if VUNLIKELY (config_file.empty() || !std::filesystem::exists(config_file, ec) || ec) {
      std::cerr << "Config file not found: " << program.get<std::string>("--config") << std::endl;
      return 1;
    }
  }

#ifdef _WIN32
  vlink::webviz::normalize_path(proto_dir);
  vlink::webviz::normalize_path(fbs_dir);
  vlink::webviz::normalize_path(schema_plugin_path);
  vlink::webviz::normalize_path(convert_plugin_path);
  vlink::webviz::normalize_path(config_file);
  vlink::webviz::normalize_paths(vlink_msgs);
#endif

  vlink::webviz::RerunServer::Config config;
  {
    const auto normalized_mode = vlink::webviz::normalize_token(program.get<std::string>("--mode"));

    if VLIKELY (normalized_mode == "spawn") {
      config.mode = vlink::webviz::RerunServer::kSpawn;
    } else if (normalized_mode == "connect") {
      config.mode = vlink::webviz::RerunServer::kConnect;
    } else if (normalized_mode == "serve") {
      config.mode = vlink::webviz::RerunServer::kServe;
    } else if (normalized_mode == "save") {
      config.mode = vlink::webviz::RerunServer::kSave;
    } else {
      std::cerr << "Invalid --mode, expected one of: spawn, connect, serve, save" << std::endl;
      return 1;
    }
  }
  config.address = program.get<std::string>("--address");
  config.bind_ip = program.get<std::string>("--bind_ip");
  config.save_path = program.get<std::string>("--save_path");
  config.name = program.get<std::string>("--name");
  config.recording_id = program.get<std::string>("--recording_id");
  config.config_file = config_file;
  config.proto_dir = proto_dir;
  config.fbs_dir = fbs_dir;
  config.schema_plugin_path = schema_plugin_path;
  config.convert_plugin_path = convert_plugin_path;
  config.convert_plugin_config = convert_plugin_config;
  config.vlink_msgs = std::move(vlink_msgs);
  config.spawn_memory_limit = program.get<std::string>("--spawn_memory_limit");
  config.spawn_server_memory_limit = program.get<std::string>("--spawn_server_memory_limit");
  config.spawn_hide_welcome_screen = program.get<bool>("--spawn_hide_welcome_screen");
  config.spawn_detach_process = !program.get<bool>("--spawn_no_detach");
  config.spawn_executable_name = program.get<std::string>("--spawn_executable_name");
  config.spawn_executable_path = program.get<std::string>("--spawn_executable_path");
  config.serve_memory_limit = program.get<std::string>("--serve_memory_limit");
  config.playback_behavior = program.get<std::string>("--playback_behavior");
  config.sequence_timeline = program.get<std::string>("--sequence_timeline");
  config.timestamp_timeline = program.get<std::string>("--timestamp_timeline");
  config.use_sequence_timeline = !program.get<bool>("--disable_sequence_timeline");
  config.use_timestamp_timeline = !program.get<bool>("--disable_timestamp_timeline");

  std::string proxy_error;
  std::error_code config_ec;

  if VLIKELY (!config_file.empty() && std::filesystem::exists(config_file, config_ec) && !config_ec) {
    std::ifstream ifs(config_file);

    if VUNLIKELY (!ifs.is_open()) {
      std::cerr << "Failed to open config file: " << config_file << std::endl;
      return 1;
    }

    try {
      Json root;
      ifs >> root;
      const auto config_dir = std::filesystem::path(config_file).parent_path();

      if VLIKELY (!program.is_used("--mode") && root.contains("mode")) {
        const auto normalized_mode = vlink::webviz::normalize_token(root["mode"].get<std::string>());

        if VLIKELY (normalized_mode == "spawn") {
          config.mode = vlink::webviz::RerunServer::kSpawn;
        } else if (normalized_mode == "connect") {
          config.mode = vlink::webviz::RerunServer::kConnect;
        } else if (normalized_mode == "serve") {
          config.mode = vlink::webviz::RerunServer::kServe;
        } else if (normalized_mode == "save") {
          config.mode = vlink::webviz::RerunServer::kSave;
        } else {
          std::cerr << "Invalid mode in " << config_file << ", expected one of: spawn, connect, serve, save"
                    << std::endl;
          return 1;
        }
      }

      if VLIKELY (!program.is_used("--address") && root.contains("address")) {
        config.address = root["address"].get<std::string>();
      }

      if VLIKELY (!program.is_used("--bind_ip") && root.contains("bind_ip")) {
        config.bind_ip = root["bind_ip"].get<std::string>();
      }

      if VLIKELY (!program.is_used("--port") && root.contains("port")) {
        config_port = root["port"].get<int>();
      }

      if VLIKELY (!program.is_used("--save_path") && root.contains("save_path")) {
        config.save_path = vlink::webviz::resolve_relative_path(config_dir, root["save_path"].get<std::string>());
      }

      if VLIKELY (!program.is_used("--name") && root.contains("name")) {
        config.name = root["name"].get<std::string>();
      }

      if VLIKELY (!program.is_used("--recording_id") && root.contains("recording_id")) {
        config.recording_id = root["recording_id"].get<std::string>();
      }

      if VUNLIKELY (root.contains("filter") && !root["filter"].is_object()) {
        std::cerr << "Invalid config file " << config_file << ": filter must be an object" << std::endl;
        return 1;
      }

      if VLIKELY (root.contains("filter") && root["filter"].is_object()) {
        const auto& filter = root["filter"];
        config.whitelist_exact.clear();
        config.whitelist_patterns.clear();
        config.blacklist_exact.clear();
        config.blacklist_patterns.clear();
        if VUNLIKELY (!vlink::webviz::append_json_filter_value(filter, "whitelist", config.whitelist_exact,
                                                               config.whitelist_patterns) ||
                      !vlink::webviz::append_json_filter_value(filter, "blacklist", config.blacklist_exact,
                                                               config.blacklist_patterns)) {
          std::cerr << "Invalid filter config in " << config_file
                    << ": filter.whitelist/filter.blacklist must be a string or string array" << std::endl;
          return 1;
        }
      }

      if VLIKELY (!program.is_used("--proto_dir") && proto_dir.empty() && root.contains("proto_dir")) {
        config.proto_dir = vlink::webviz::resolve_relative_path(config_dir, root["proto_dir"].get<std::string>());
      }

      if VLIKELY (!program.is_used("--fbs_dir") && fbs_dir.empty() && root.contains("fbs_dir")) {
        config.fbs_dir = vlink::webviz::resolve_relative_path(config_dir, root["fbs_dir"].get<std::string>());
      }

      if VLIKELY (!program.is_used("--schema_plugin") && schema_plugin_path.empty() &&
                  root.contains("schema_plugin_path")) {
        config.schema_plugin_path =
            vlink::webviz::resolve_relative_path(config_dir, root["schema_plugin_path"].get<std::string>());
      }

      if VLIKELY (!program.is_used("--convert_plugin") && convert_plugin_path.empty() &&
                  root.contains("convert_plugin_path")) {
        config.convert_plugin_path =
            vlink::webviz::resolve_relative_path(config_dir, root["convert_plugin_path"].get<std::string>());
      }

      if VLIKELY (!program.is_used("--convert_plugin_config") && convert_plugin_config.empty() &&
                  root.contains("convert_plugin_config")) {
        config.convert_plugin_config = root["convert_plugin_config"].get<std::string>();
      }

      if VUNLIKELY (!vlink::webviz::ProxyConfigHelper::apply_config(root, std::filesystem::path(config_file), program,
                                                                    config.proxy_config, proxy_error)) {
        std::cerr << "Invalid proxy config in " << config_file << ": " << proxy_error << std::endl;
        return 1;
      }

      if VLIKELY (!program.is_used("--vlink_msgs")) {
        config.vlink_msgs.clear();
        if VUNLIKELY (!vlink::webviz::append_config_paths(root, "vlink_msgs", config_dir, config.vlink_msgs)) {
          std::cerr << "Invalid config file " << config_file << ": vlink_msgs must be an array of strings" << std::endl;
          return 1;
        }
      }

      if VLIKELY (!program.is_used("--spawn_memory_limit") && root.contains("spawn_memory_limit")) {
        config.spawn_memory_limit = root["spawn_memory_limit"].get<std::string>();
      }

      if VLIKELY (!program.is_used("--spawn_server_memory_limit") && root.contains("spawn_server_memory_limit")) {
        config.spawn_server_memory_limit = root["spawn_server_memory_limit"].get<std::string>();
      }

      if VLIKELY (!program.is_used("--spawn_hide_welcome_screen") && root.contains("spawn_hide_welcome_screen")) {
        config.spawn_hide_welcome_screen = root["spawn_hide_welcome_screen"].get<bool>();
      }

      if VLIKELY (!program.is_used("--spawn_no_detach") && root.contains("spawn_detach_process")) {
        config.spawn_detach_process = root["spawn_detach_process"].get<bool>();
      }

      if VLIKELY (!program.is_used("--spawn_executable_name") && root.contains("spawn_executable_name")) {
        config.spawn_executable_name = root["spawn_executable_name"].get<std::string>();
      }

      if VLIKELY (!program.is_used("--spawn_executable_path") && root.contains("spawn_executable_path")) {
        config.spawn_executable_path =
            vlink::webviz::resolve_relative_path(config_dir, root["spawn_executable_path"].get<std::string>());
      }

      if VLIKELY (!program.is_used("--serve_memory_limit") && root.contains("serve_memory_limit")) {
        config.serve_memory_limit = root["serve_memory_limit"].get<std::string>();
      }

      if VLIKELY (!program.is_used("--playback_behavior") && root.contains("playback_behavior")) {
        config.playback_behavior = root["playback_behavior"].get<std::string>();
      }

      if VLIKELY (!program.is_used("--sequence_timeline") && root.contains("sequence_timeline")) {
        config.sequence_timeline = root["sequence_timeline"].get<std::string>();
      }

      if VLIKELY (!program.is_used("--timestamp_timeline") && root.contains("timestamp_timeline")) {
        config.timestamp_timeline = root["timestamp_timeline"].get<std::string>();
      }

      if VLIKELY (!program.is_used("--disable_sequence_timeline") && root.contains("use_sequence_timeline")) {
        config.use_sequence_timeline = root["use_sequence_timeline"].get<bool>();
      }

      if VLIKELY (!program.is_used("--disable_timestamp_timeline") && root.contains("use_timestamp_timeline")) {
        config.use_timestamp_timeline = root["use_timestamp_timeline"].get<bool>();
      }
    } catch (const std::exception& e) {
      std::cerr << "Failed to parse config file " << config_file << ": " << e.what() << std::endl;
      return 1;
    }
  }

  if VUNLIKELY (!vlink::webviz::ProxyConfigHelper::apply_arguments(program, config.proxy_config, proxy_error)) {
    std::cerr << proxy_error << std::endl;
    return 1;
  }

  if VUNLIKELY (!vlink::webviz::ProxyConfigHelper::validate(config.proxy_config, proxy_error)) {
    std::cerr << proxy_error << std::endl;
    return 1;
  }

  if VUNLIKELY (program.is_used("--filter")) {
    auto cli_filter_tokens = std::vector<std::string>{};
    vlink::webviz::append_filter_patterns(cli_filter_tokens, program.get<std::string>("--filter"));

    if VLIKELY (!cli_filter_tokens.empty()) {
      auto& target_filters = program.get<bool>("--black") ? config.blacklist_patterns : config.whitelist_patterns;
      target_filters.insert(target_filters.end(), std::make_move_iterator(cli_filter_tokens.begin()),
                            std::make_move_iterator(cli_filter_tokens.end()));
    }
  }

  vlink::webviz::normalize_exact_filters(config.whitelist_exact);
  vlink::webviz::normalize_filter_patterns(config.whitelist_patterns);
  vlink::webviz::normalize_exact_filters(config.blacklist_exact);
  vlink::webviz::normalize_filter_patterns(config.blacklist_patterns);

#ifdef _WIN32
  vlink::webviz::normalize_path(config.proto_dir);
  vlink::webviz::normalize_path(config.fbs_dir);
  vlink::webviz::normalize_path(config.schema_plugin_path);
  vlink::webviz::normalize_path(config.convert_plugin_path);
  vlink::webviz::normalize_path(config.spawn_executable_path);
  vlink::webviz::normalize_path(config.save_path);
  vlink::webviz::normalize_path(config.proxy_config.server.iox_config);
  vlink::webviz::normalize_paths(config.vlink_msgs);
#endif

  config.playback_behavior = vlink::webviz::normalize_token(config.playback_behavior);

  if VLIKELY (config.mode == vlink::webviz::RerunServer::kSpawn || config.mode == vlink::webviz::RerunServer::kServe) {
    if VUNLIKELY (!vlink::webviz::validate_port(config_port, "--port", config_error)) {
      std::cerr << config_error << std::endl;
      return 1;
    }
  }

  if VLIKELY (config_port > 0 && config_port <= 65535) {
    config.port = static_cast<uint16_t>(config_port);
  }

  if VUNLIKELY (config.name.empty()) {
    std::cerr << "--name must not be empty" << std::endl;
    return 1;
  }

  if VUNLIKELY (config.playback_behavior != "oldest_first" && config.playback_behavior != "newest_first") {
    std::cerr << "--playback_behavior must be 'oldest_first' or 'newest_first'" << std::endl;
    return 1;
  }

  if VUNLIKELY (config.use_sequence_timeline && config.sequence_timeline.empty()) {
    std::cerr << "--sequence_timeline must not be empty when sequence timeline is enabled" << std::endl;
    return 1;
  }

  if VUNLIKELY (config.use_timestamp_timeline && config.timestamp_timeline.empty()) {
    std::cerr << "--timestamp_timeline must not be empty when timestamp timeline is enabled" << std::endl;
    return 1;
  }

  if VLIKELY (config.mode == vlink::webviz::RerunServer::kSpawn) {
    if VUNLIKELY (config.spawn_executable_name.empty() && config.spawn_executable_path.empty()) {
      std::cerr << "spawn mode requires --spawn_executable_name or --spawn_executable_path" << std::endl;
      return 1;
    }
  } else if (config.mode == vlink::webviz::RerunServer::kConnect) {
    if VUNLIKELY (config.address.empty()) {
      std::cerr << "connect mode requires --address" << std::endl;
      return 1;
    }
  } else if (config.mode == vlink::webviz::RerunServer::kServe) {
    if VUNLIKELY (config.bind_ip.empty()) {
      std::cerr << "serve mode requires --bind_ip" << std::endl;
      return 1;
    }
  } else if VUNLIKELY (config.save_path.empty()) {
    std::cerr << "save mode requires --save_path" << std::endl;
    return 1;
  }

  vlink::Logger::init("vlink-rerun");
  vlink::Utils::unset_env("VLINK_BAG_PATH");
  vlink::Utils::set_env("RUST_LOG", "off");

  vlink::webviz::RerunServer server(config);
  vlink::Utils::register_terminate_signal([&server](int) { server.stop(); });

  if VUNLIKELY (!server.start()) {
    return 1;
  }

  return 0;
}
