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
#include <vlink/extension/trigger_recorder.h>
#include <vlink/version.h>
#include <vlink/vlink.h>

#include <argparse/argparse.hpp>
#include <nlohmann/json.hpp>
//
#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

static constexpr char kMethodUrl[] = "dds://trigger/method";

static std::string to_lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });

  return text;
}

static bool parse_config(const std::string& path, vlink::TriggerRecorder::Config& config, std::string& method_url) {
  std::ifstream file(path);

  if (!file.is_open()) {
    std::cerr << "Cannot open config: " << path << std::endl;
    return false;
  }

  nlohmann::json data;

  try {
    file >> data;

    auto mb_to_bytes = [](double megabytes) -> int64_t { return static_cast<int64_t>(megabytes * 1024.0 * 1024.0); };

    if (data.contains("method_url")) {
      method_url = data.at("method_url").get<std::string>();
    }

    if (data.contains("dump_dir")) {
      config.dump_dir = data.at("dump_dir").get<std::string>();
    }

    if (data.contains("file_type")) {
      const std::string file_type = to_lower(data.at("file_type").get<std::string>());

      if (file_type == "vcap" || file_type == "mcap") {
        config.file_type = vlink::TriggerRecorder::kVcap;
      } else if (file_type == "vdb") {
        config.file_type = vlink::TriggerRecorder::kVdb;
      } else {
        std::cerr << "Warning: unknown file_type '" << file_type << "', using vdb." << std::endl;
        config.file_type = vlink::TriggerRecorder::kVdb;
      }
    }

    if (data.contains("default_pre_ms")) {
      config.default_pre_ms = data.at("default_pre_ms").get<int64_t>();
    }

    if (data.contains("default_post_ms")) {
      config.default_post_ms = data.at("default_post_ms").get<int64_t>();
    }

    if (data.contains("default_max_packet_size")) {
      config.default_max_packet_size = mb_to_bytes(data.at("default_max_packet_size").get<double>());
    }

    if (data.contains("default_max_size")) {
      config.default_max_size = mb_to_bytes(data.at("default_max_size").get<double>());
    }

    if (data.contains("max_cache_size")) {
      config.max_cache_size = mb_to_bytes(data.at("max_cache_size").get<double>());
    }

    if (data.contains("retention_guard_ms")) {
      config.retention_guard_ms = data.at("retention_guard_ms").get<int64_t>();
    }

    if (data.contains("max_dump_file_count")) {
      config.max_dump_file_count = data.at("max_dump_file_count").get<int>();
    }

    if (data.contains("enable_compress")) {
      config.enable_compress = data.at("enable_compress").get<bool>();
    }

    if (data.contains("busy_skip_data")) {
      config.busy_skip_data = data.at("busy_skip_data").get<bool>();
    }

    if (data.contains("destroy_on_offline")) {
      config.destroy_on_offline = data.at("destroy_on_offline").get<bool>();
    }

    if (data.contains("overflow")) {
      const std::string overflow = to_lower(data.at("overflow").get<std::string>());

      if (overflow == "drop" || overflow == "drop_newest") {
        config.overflow = vlink::TriggerRecorder::kDropNewest;
      } else if (overflow == "cover" || overflow == "cover_oldest") {
        config.overflow = vlink::TriggerRecorder::kCoverOldest;
      } else {
        std::cerr << "Warning: unknown overflow '" << overflow << "', using cover." << std::endl;
        config.overflow = vlink::TriggerRecorder::kCoverOldest;
      }
    }

    if (data.contains("sleep_interval_mb")) {
      config.sleep_interval = mb_to_bytes(data.at("sleep_interval_mb").get<double>());
    }

    if (data.contains("sleep_time_ms")) {
      config.sleep_time_ms = data.at("sleep_time_ms").get<int64_t>();
    }

    if (data.contains("dds_ip")) {
      config.dds_ip = data.at("dds_ip").get<std::string>();
    }

    if (data.contains("discovery_filter")) {
      const std::string filter = to_lower(data.at("discovery_filter").get<std::string>());

      if (filter == "native") {
        config.discovery_filter = vlink::DiscoveryViewer::kFilterNative;
      } else if (filter == "none") {
        config.discovery_filter = vlink::DiscoveryViewer::kFilterNone;
      } else if (filter == "available") {
        config.discovery_filter = vlink::DiscoveryViewer::kFilterAvailable;
      } else {
        std::cerr << "Warning: unknown discovery_filter '" << filter << "', using available." << std::endl;
        config.discovery_filter = vlink::DiscoveryViewer::kFilterAvailable;
      }
    }

    if (data.contains("whitelist")) {
      config.whitelist = data.at("whitelist").get<std::vector<std::string>>();
    }

    if (data.contains("blacklist")) {
      config.blacklist = data.at("blacklist").get<std::vector<std::string>>();
    }

    if (data.contains("bag_plugin")) {
      config.bag_plugin_lib = data.at("bag_plugin").get<std::string>();
    }

    if (data.contains("bag_plugin_dir")) {
      config.bag_plugin_dir = data.at("bag_plugin_dir").get<std::string>();
    }

    if (data.contains("bag_plugin_major")) {
      config.bag_plugin_major = data.at("bag_plugin_major").get<uint16_t>();
    }

    if (data.contains("bag_plugin_minor")) {
      config.bag_plugin_minor = data.at("bag_plugin_minor").get<uint16_t>();
    }

    if (data.contains("url_overrides")) {
      for (const auto& [url, item] : data.at("url_overrides").get_ref<const nlohmann::json::object_t&>()) {
        vlink::TriggerRecorder::UrlConfig uc;

        if (item.contains("pre_ms")) {
          uc.pre_ms = item.at("pre_ms").get<int64_t>();
        }

        if (item.contains("post_ms")) {
          uc.post_ms = item.at("post_ms").get<int64_t>();
        }

        if (item.contains("max_packet_size")) {
          uc.max_packet_size = mb_to_bytes(item.at("max_packet_size").get<double>());
        }

        if (item.contains("max_size")) {
          uc.max_size = mb_to_bytes(item.at("max_size").get<double>());
        }

        if (item.contains("only_front")) {
          uc.only_front = item.at("only_front").get<bool>();
        }

        if (item.contains("only_back")) {
          uc.only_back = item.at("only_back").get<bool>();
        }

        config.url_overrides.emplace(url, uc);
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "Parse config failed: " << e.what() << std::endl;
    return false;
  }

  return true;
}

static int run_daemon(const std::string& config_path, bool native_mode) {
  vlink::TriggerRecorder::Config config;
  std::string method_url = kMethodUrl;

  if (!parse_config(config_path, config, method_url)) {
    return 1;
  }

  if (native_mode) {
    config.discovery_filter = vlink::DiscoveryViewer::kFilterNative;

    if (config.dds_ip.empty()) {
      config.dds_ip = "127.0.0.1";
    }
  }

  if VUNLIKELY (!vlink::Utils::check_singleton("vlink-trigger")) {
    std::cerr << "vlink-trigger is already running." << std::endl;
    return 1;
  }

  vlink::TriggerRecorder recorder(config);

  if VUNLIKELY (!recorder.start()) {
    std::cerr << "Failed to start trigger recorder." << std::endl;
    return 1;
  }

  std::shared_ptr<vlink::Server<std::string, std::string>> server;

  try {
    server = vlink::Server<std::string, std::string>::create_shared(method_url);
  } catch (const std::exception& e) {
    std::cerr << "Invalid method_url (" << method_url << "): " << e.what() << std::endl;
    recorder.stop();
    return 1;
  }

  const bool listening = server->listen([&recorder](const std::string& request, std::string& response) {
    nlohmann::json resp;
    resp["ok"] = false;
    resp["error_code"] = 0;
    resp["error_string"] = "";

    try {
      const nlohmann::json req = nlohmann::json::parse(request);

      if (req.value("type", std::string()) != "dump") {
        resp["error_code"] = 3;
        resp["error_string"] = "Unknown request type";
        response = resp.dump();

        return;
      }

      vlink::TriggerRecorder::TriggerParams params;
      params.out_file = req.value("out_file", std::string());
      params.reason = req.value("reason", std::string());
      params.name_hint = req.value("name_hint", std::string());
      params.pre_ms = req.value("pre_ms", static_cast<int64_t>(-1));
      params.post_ms = req.value("post_ms", static_cast<int64_t>(-1));
      params.filter_str = req.value("filter_str", std::string());
      params.black_mode = req.value("black_mode", false);

      if (req.contains("filter_urls")) {
        for (const auto& url : req.at("filter_urls").get_ref<const nlohmann::json::array_t&>()) {
          params.filter_urls.emplace(url.get<std::string>());
        }
      }

      const bool ok = recorder.trigger(params);

      resp["ok"] = ok;

      if VUNLIKELY (!ok) {
        resp["error_code"] = 1;
        resp["error_string"] = "Trigger rejected (busy or not running)";
      }
    } catch (const std::exception& e) {
      resp["ok"] = false;
      resp["error_code"] = 4;
      resp["error_string"] = std::string("Invalid request: ") + e.what();
    }

    response = resp.dump();
  });

  if VUNLIKELY (!listening) {
    std::cerr << "Failed to listen on " << method_url << "." << std::endl;
    recorder.stop();
    return 1;
  }

  std::mutex quit_mtx;
  std::condition_variable quit_cv;
  bool quit_flag = false;

  vlink::Utils::register_terminate_signal(
      [&](int) {
        {
          std::lock_guard lock(quit_mtx);
          quit_flag = true;
        }

        quit_cv.notify_all();
      },
      true);

  VLOG_I("vlink-trigger daemon started, method_url=", method_url);

  {
    std::unique_lock lock(quit_mtx);
    quit_cv.wait(lock, [&] { return quit_flag; });
  }

  server.reset();
  recorder.stop();

  return 0;
}

static int run_trigger(const std::string& method_url, const std::string& out_file, const std::string& reason,
                       const std::string& name_hint, int64_t pre_ms, int64_t post_ms,
                       const std::vector<std::string>& filter_url_list, const std::string& filter_str,
                       bool black_mode) {
  std::unique_ptr<vlink::Client<std::string, std::string>> client;

  try {
    client = std::make_unique<vlink::Client<std::string, std::string>>(method_url);
  } catch (const std::exception& e) {
    std::cerr << "Invalid method_url (" << method_url << "): " << e.what() << std::endl;
    return 1;
  }

  if (!client->wait_for_connected(std::chrono::milliseconds(3000))) {
    std::cerr << "Trigger daemon is not ready." << std::endl;
    return 1;
  }

  nlohmann::json req;
  req["type"] = "dump";
  req["out_file"] = out_file;
  req["reason"] = reason;
  req["name_hint"] = name_hint;
  req["pre_ms"] = pre_ms;
  req["post_ms"] = post_ms;
  req["filter_str"] = filter_str;
  req["black_mode"] = black_mode;
  req["filter_urls"] = filter_url_list;

  std::string response;

  if (!client->invoke(req.dump(), response, std::chrono::seconds(10))) {
    std::cerr << "Trigger invoke failed." << std::endl;
    return 1;
  }

  try {
    const nlohmann::json resp = nlohmann::json::parse(response);

    if (!resp.value("ok", false)) {
      std::cerr << "Trigger failed: " << resp.value("error_string", std::string()) << " (code "
                << resp.value("error_code", 0) << ")" << std::endl;
      return 1;
    }
  } catch (const std::exception& e) {
    std::cerr << "Invalid response: " << e.what() << std::endl;
    return 1;
  }

  std::cout << "Trigger accepted." << std::endl;

  return 0;
}

int main(int argc, char* argv[]) {
  vlink::Utils::set_console_utf8_output();

  vlink::Logger::set_file_level(vlink::Logger::kOff);
  vlink::Logger::init("vlink-trigger");

  vlink::Utils::unset_env("VLINK_BAG_PATH");

  argparse::ArgumentParser program("vlink-trigger", VLINK_VERSION, argparse::default_arguments::all);
  program.add_description("VLink in-memory trigger recorder (event data recorder). Note: multicast/broadcast [" +
                          vlink::DiscoveryViewer::get_listen_address() + "] may be required.");

  argparse::ArgumentParser daemon_command("daemon", VLINK_VERSION, argparse::default_arguments::help);
  daemon_command.add_argument("-c", "--config").help("Config json path").required();
  daemon_command.add_argument("-n", "--native")
      .help("Native mode: local-host discovery + dds.ip=127.0.0.1 (unless configured)")
      .default_value(false)
      .implicit_value(true);
  daemon_command.add_description("Run the trigger recorder daemon");
  daemon_command.add_epilog("Example:\n  vlink-trigger daemon -c /etc/vlink/trigger/trigger.json");

  argparse::ArgumentParser trigger_command("trigger", VLINK_VERSION, argparse::default_arguments::help);
  trigger_command.add_argument("-m", "--method_url")
      .help("Daemon control-plane URL (matches the daemon's method_url)")
      .default_value(std::string(kMethodUrl));
  trigger_command.add_argument("-o", "--out_file")
      .help("Output file path (empty: auto under dump_dir)")
      .default_value(std::string());
  trigger_command.add_argument("-r", "--reason")
      .help("Trigger reason (stored as bag tag)")
      .default_value(std::string());
  trigger_command.add_argument("-n", "--name").help("Output file name hint").default_value(std::string());
  trigger_command.add_argument("--pre")
      .help("Pre window ms (shrink only; -1 keeps configured)")
      .scan<'d', int64_t>()
      .default_value(static_cast<int64_t>(-1));
  trigger_command.add_argument("--post")
      .help("Post window ms (shrink only; -1 keeps configured)")
      .scan<'d', int64_t>()
      .default_value(static_cast<int64_t>(-1));
  trigger_command.add_argument("-u", "--url")
      .help("Filter: dump only these exact urls")
      .nargs(argparse::nargs_pattern::any);
  trigger_command.add_argument("-i", "--filter")
      .help("Filter urls by space-separated substrings")
      .default_value(std::string());
  trigger_command.add_argument("-k", "--black")
      .help("Blacklist mode for --filter")
      .default_value(false)
      .implicit_value(true);
  trigger_command.add_description("Send a trigger to a running daemon");
  trigger_command.add_epilog("Example:\n  vlink-trigger trigger -r hard-brake -o /tmp/edr.vdb");

  program.add_subparser(daemon_command);
  program.add_subparser(trigger_command);

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;

    if (program.is_subcommand_used("daemon")) {
      std::cerr << daemon_command << std::endl;
    } else if (program.is_subcommand_used("trigger")) {
      std::cerr << trigger_command << std::endl;
    } else {
      std::cerr << program << std::endl;
    }

    return 1;
  }

  if (program.is_subcommand_used("daemon")) {
    return run_daemon(daemon_command.get<std::string>("-c"), daemon_command.is_used("-n"));
  }

  if (program.is_subcommand_used("trigger")) {
    std::vector<std::string> filter_url_list;

    if (trigger_command.is_used("-u")) {
      filter_url_list = trigger_command.get<std::vector<std::string>>("-u");
    }

    return run_trigger(trigger_command.get<std::string>("-m"), trigger_command.get<std::string>("-o"),
                       trigger_command.get<std::string>("-r"), trigger_command.get<std::string>("-n"),
                       trigger_command.get<int64_t>("--pre"), trigger_command.get<int64_t>("--post"), filter_url_list,
                       trigger_command.get<std::string>("-i"), trigger_command.is_used("-k"));
  }

  std::cerr << program << std::endl;

  return 1;
}
