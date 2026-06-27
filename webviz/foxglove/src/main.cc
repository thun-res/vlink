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

#include "foxglove_server.h"
#include "proxy_config.h"
#include "webviz_app_utils.h"

using Json = nlohmann::json;

int main(int argc, char* argv[]) {
  vlink::Utils::set_console_utf8_output();

  argparse::ArgumentParser program("vlink-foxglove", VLINK_VERSION, argparse::default_arguments::all);
  program.add_description("Bridge VLink topics and RPCs to Foxglove using proxy_api or local proxy_server mode.");

  program.add_argument("-p", "--port").help("WebSocket server port").default_value(8765).scan<'i', int>();

  program.add_argument("-a", "--address").help("WebSocket bind address").default_value(std::string("0.0.0.0"));

  program.add_argument("-c", "--config")
      .help("Path to root config file (foxglove_config.json)")
      .default_value(std::string(""));

  program.add_argument("--name").help("Foxglove server name").default_value(std::string("vlink-foxglove"));

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
      .help("Path to message conversion plugin shared library")
      .default_value(std::string(""));

  program.add_argument("--convert_plugin_config")
      .help("Configuration string for the conversion plugin")
      .default_value(std::string(""));

  program.add_argument("--vlink_msgs")
      .help("Path(s) to vlink_msgs mapping files")
      .nargs(argparse::nargs_pattern::any)
      .default_value(std::vector<std::string>{});

  program.add_argument("--foxglove_msgs")
      .help("Path(s) to foxglove_msgs publish route config files")
      .nargs(argparse::nargs_pattern::any)
      .default_value(std::vector<std::string>{});

  program.add_argument("--rpc_msgs")
      .help("Path(s) to Foxglove rpc service config files")
      .nargs(argparse::nargs_pattern::any)
      .default_value(std::vector<std::string>{});

  program.add_argument("--send_time")
      .help("Send time updates to frontend clients")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--parameters_url")
      .help("VLink url used for Foxglove parameters capability")
      .default_value(std::string(""));

  program.add_argument("-i", "--filter")
      .help("Case-insensitive URL substring filter, comma-separated or quoted space-separated")
      .default_value(std::string(""));

  program.add_argument("-k", "--black")
      .help("Use -i/--filter as blacklist mode")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--allow_multiple")
      .help("Allow multiple instances to run simultaneously")
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

  if VUNLIKELY (!program.get<bool>("--allow_multiple") && !vlink::Utils::check_singleton("vlink-foxglove")) {
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
  auto foxglove_msgs = program.get<std::vector<std::string>>("--foxglove_msgs");
  auto rpc_msgs = program.get<std::vector<std::string>>("--rpc_msgs");
  auto config_file = program.get<std::string>("--config");
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
  vlink::webviz::normalize_paths(foxglove_msgs);
  vlink::webviz::normalize_paths(rpc_msgs);
#endif

  vlink::webviz::FoxgloveServer::Config config;
  config.address = program.get<std::string>("--address");
  config.config_file = config_file;
  config.name = program.get<std::string>("--name");
  config.proto_dir = proto_dir;
  config.fbs_dir = fbs_dir;
  config.schema_plugin_path = schema_plugin_path;
  config.convert_plugin_path = convert_plugin_path;
  config.convert_plugin_config = convert_plugin_config;
  config.vlink_msgs = std::move(vlink_msgs);
  config.foxglove_msgs = std::move(foxglove_msgs);
  config.rpc_msgs = std::move(rpc_msgs);
  config.parameters.url = program.get<std::string>("--parameters_url");

  bool publish_configured = false;
  bool rpcs_configured = false;
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

      if VUNLIKELY (root.contains("supported_encodings")) {
        std::cerr << "Invalid config file " << config_file
                  << ": supported_encodings has been removed; Foxglove frontend publish/service encodings are fixed "
                     "to json"
                  << std::endl;
        return 1;
      }

      if VUNLIKELY (!program.is_used("--port") && root.contains("port")) {
        config_port = root["port"].get<int>();
      }

      if VUNLIKELY (!program.is_used("--address") && root.contains("address")) {
        config.address = root["address"].get<std::string>();
      }

      if VUNLIKELY (!program.is_used("--name") && root.contains("name")) {
        config.name = root["name"].get<std::string>();
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

      if VUNLIKELY (root.contains("capabilities") && !root["capabilities"].is_object()) {
        std::cerr << "Invalid config file " << config_file << ": capabilities must be an object" << std::endl;
        return 1;
      }

      if VLIKELY (root.contains("capabilities") && root["capabilities"].is_object()) {
        const auto& capabilities = root["capabilities"];
        config.capabilities.time = capabilities.value("time", config.capabilities.time);
        config.capabilities.connection_graph =
            capabilities.value("connection_graph", config.capabilities.connection_graph);
        config.capabilities.assets = capabilities.value("assets", config.capabilities.assets);

        if VLIKELY (capabilities.contains("publish")) {
          config.capabilities.publish = capabilities["publish"].get<bool>();
          publish_configured = true;
        }

        if VLIKELY (capabilities.contains("rpcs")) {
          config.capabilities.rpcs = capabilities["rpcs"].get<bool>();
          rpcs_configured = true;
        }
      }

      if VUNLIKELY (root.contains("parameters") && !root["parameters"].is_object()) {
        std::cerr << "Invalid config file " << config_file << ": parameters must be an object" << std::endl;
        return 1;
      }

      if VLIKELY (root.contains("parameters") && root["parameters"].is_object()) {
        const auto& parameters = root["parameters"];

        if VLIKELY (!program.is_used("--parameters_url") && parameters.contains("url")) {
          if VUNLIKELY (!parameters["url"].is_string()) {
            std::cerr << "Invalid config file " << config_file << ": parameters.url must be a string" << std::endl;
            return 1;
          }

          config.parameters.url = parameters["url"].get<std::string>();
        }

        std::string parameters_error;

        if VUNLIKELY (!vlink::webviz::FoxgloveParameters::parse_config_values(parameters, config.parameters.values,
                                                                              parameters_error)) {
          std::cerr << "Invalid config file " << config_file << ": " << parameters_error << std::endl;
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

      if VLIKELY (!program.is_used("--send_time") && root.contains("send_time")) {
        config.send_time = root["send_time"].get<bool>();
      }

      if VLIKELY (!program.is_used("--vlink_msgs")) {
        config.vlink_msgs.clear();
        if VUNLIKELY (!vlink::webviz::append_config_paths(root, "vlink_msgs", config_dir, config.vlink_msgs)) {
          std::cerr << "Invalid config file " << config_file << ": vlink_msgs must be an array of strings" << std::endl;
          return 1;
        }
      }

      if VLIKELY (!program.is_used("--foxglove_msgs")) {
        config.foxglove_msgs.clear();
        if VUNLIKELY (!vlink::webviz::append_config_paths(root, "foxglove_msgs", config_dir, config.foxglove_msgs)) {
          std::cerr << "Invalid config file " << config_file << ": foxglove_msgs must be an array of strings"
                    << std::endl;
          return 1;
        }
      }

      if VLIKELY (!program.is_used("--rpc_msgs")) {
        config.rpc_msgs.clear();
        if VUNLIKELY (!vlink::webviz::append_config_paths(root, "rpc_msgs", config_dir, config.rpc_msgs)) {
          std::cerr << "Invalid config file " << config_file << ": rpc_msgs must be an array of strings" << std::endl;
          return 1;
        }
      }

      if VUNLIKELY (root.contains("asset_dirs") && !root["asset_dirs"].is_array()) {
        std::cerr << "Invalid config file " << config_file << ": asset_dirs must be an array" << std::endl;
        return 1;
      }

      if VLIKELY (root.contains("asset_dirs") && root["asset_dirs"].is_array()) {
        config.asset_dirs.clear();
        if VUNLIKELY (!vlink::webviz::append_config_paths(root, "asset_dirs", config_dir, config.asset_dirs)) {
          std::cerr << "Invalid config file " << config_file << ": asset_dirs must be an array of strings" << std::endl;
          return 1;
        }
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

  if VUNLIKELY (program.is_used("--send_time")) {
    config.send_time = program.get<bool>("--send_time");
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

  if VUNLIKELY (!publish_configured && !config.foxglove_msgs.empty()) {
    config.capabilities.publish = true;
  }

  if VUNLIKELY (!rpcs_configured) {
    config.capabilities.rpcs = !config.rpc_msgs.empty();
  }

#ifdef _WIN32
  vlink::webviz::normalize_path(config.proto_dir);
  vlink::webviz::normalize_path(config.fbs_dir);
  vlink::webviz::normalize_path(config.schema_plugin_path);
  vlink::webviz::normalize_path(config.convert_plugin_path);
  vlink::webviz::normalize_path(config.proxy_config.server.iox_config);
  vlink::webviz::normalize_paths(config.vlink_msgs);
  vlink::webviz::normalize_paths(config.foxglove_msgs);
  vlink::webviz::normalize_paths(config.rpc_msgs);
  vlink::webviz::normalize_paths(config.asset_dirs);
#endif

  if VUNLIKELY (!vlink::webviz::validate_port(config_port, "--port", proxy_error)) {
    std::cerr << proxy_error << std::endl;
    return 1;
  }

  config.port = static_cast<uint16_t>(config_port);

  if VUNLIKELY (config.name.empty()) {
    std::cerr << "--name must not be empty" << std::endl;
    return 1;
  }

  if VUNLIKELY (config.address.empty()) {
    std::cerr << "--address must not be empty" << std::endl;
    return 1;
  }

  vlink::Logger::init("vlink-foxglove");

  for (auto asset_dir_iter = config.asset_dirs.begin(); asset_dir_iter != config.asset_dirs.end();) {
    std::error_code ec;
    const auto path = std::filesystem::path(*asset_dir_iter);

    if VUNLIKELY (!std::filesystem::exists(path, ec) || !std::filesystem::is_directory(path, ec)) {
      MLOG_W("Ignoring asset_dirs entry that is not an existing directory: {}", *asset_dir_iter);
      asset_dir_iter = config.asset_dirs.erase(asset_dir_iter);
    } else {
      ++asset_dir_iter;
    }
  }

  if VUNLIKELY (config.capabilities.assets && config.asset_dirs.empty()) {
    MLOG_W("capabilities.assets is enabled but asset_dirs is empty; disabling asset capability");
    config.capabilities.assets = false;
  }

  if VUNLIKELY (!config.capabilities.publish && !config.foxglove_msgs.empty()) {
    MLOG_W(
        "foxglove publish mappings are configured but capabilities.publish is disabled; "
        "frontend publish will be rejected");
  }

  if VUNLIKELY (config.capabilities.publish && !config.convert_plugin_path.empty() && config.foxglove_msgs.empty()) {
    MLOG_W(
        "capabilities.publish is enabled with convert_plugin only; "
        "publish channels will not be proactively advertised with schema");
  }

  if VUNLIKELY (!config.capabilities.rpcs && !config.rpc_msgs.empty()) {
    MLOG_W("rpc_msgs are configured but capabilities.rpcs is disabled; RPC calls will be hidden");
  }

  vlink::Utils::unset_env("VLINK_BAG_PATH");

  vlink::webviz::FoxgloveServer server(config);
  vlink::Utils::register_terminate_signal([&server](int) { server.stop(); });

  if VUNLIKELY (!server.start()) {
    return 1;
  }

  return 0;
}
