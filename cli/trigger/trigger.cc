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

#include <vlink/base/condition_variable.h>
#include <vlink/base/logger.h>
#include <vlink/base/plugin.h>
#include <vlink/base/utils.h>
#include <vlink/extension/bag_plugin_interface.h>
#include <vlink/extension/trigger_plugin_interface.h>
#include <vlink/extension/trigger_recorder.h>
#include <vlink/version.h>
#include <vlink/vlink.h>

#include <algorithm>
#include <argparse/argparse.hpp>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <vector>

static constexpr char kDefaultMethodUrl[] = "dds://trigger/method";

struct DaemonOptions final {
  std::string method_url{kDefaultMethodUrl};
  bool allow_outside_dir{true};
  std::string bag_plugin_lib;
  std::string bag_plugin_dir;
  std::string trigger_plugin_lib;
  std::string trigger_plugin_dir;
  std::string trigger_plugin_config;
};

struct DaemonArguments final {
  std::string config_path;
  bool native_mode{false};
  std::optional<std::string> bag_plugin_lib;
  std::optional<std::string> trigger_plugin_lib;
  std::optional<std::string> trigger_plugin_config;
};

static std::string to_lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });

  return text;
}

static bool mb_to_bytes(double megabytes, const std::string& field_name, int64_t& result) {
  constexpr int64_t kBytesPerMegabyte = 1024LL * 1024LL;
  constexpr int64_t kMaxMegabytes = std::numeric_limits<int64_t>::max() / kBytesPerMegabyte;

  if VUNLIKELY (!std::isfinite(megabytes)) {
    std::cerr << field_name << " must be finite." << std::endl;
    return false;
  } else if VUNLIKELY (megabytes < 0.0) {
    std::cerr << field_name << " must be non-negative." << std::endl;
    return false;
  } else if VUNLIKELY (megabytes > static_cast<double>(kMaxMegabytes)) {
    std::cerr << field_name << " exceeds the int64 byte range." << std::endl;
    return false;
  }

  result = static_cast<int64_t>(megabytes * static_cast<double>(kBytesPerMegabyte));

  return true;
}

static bool is_valid_trigger_window(int64_t value) {
  return value == -1 || (value >= 0 && value <= vlink::TriggerRecorder::kMaxWindowMs);
}

static bool parse_trigger_window(const nlohmann::json& request, const char* field_name, int64_t& result,
                                 std::string& error) {
  if (!request.contains(field_name)) {
    result = -1;
    return true;
  }

  const auto& value = request.at(field_name);

  if (value.is_number_unsigned()) {
    const auto unsigned_value = value.get<uint64_t>();

    if VUNLIKELY (unsigned_value > static_cast<uint64_t>(vlink::TriggerRecorder::kMaxWindowMs)) {
      error = std::string(field_name) + " exceeds the supported millisecond range";
      return false;
    }

    result = static_cast<int64_t>(unsigned_value);
    return true;
  }

  if VUNLIKELY (!value.is_number_integer()) {
    error = std::string(field_name) + " must be an integer";
    return false;
  }

  const auto signed_value = value.get<int64_t>();

  if VUNLIKELY (!is_valid_trigger_window(signed_value)) {
    error = std::string(field_name) + " must be -1 or a non-negative millisecond value";
    return false;
  }

  result = signed_value;

  return true;
}

static bool path_is_within(const std::filesystem::path& root, const std::filesystem::path& candidate) {
  auto root_iter = root.begin();
  auto candidate_iter = candidate.begin();

  for (; root_iter != root.end(); ++root_iter, ++candidate_iter) {
    if VUNLIKELY (candidate_iter == candidate.end() || *candidate_iter != *root_iter) {
      return false;
    }
  }

  return true;
}

static bool validate_out_file(const std::string& requested, const std::string& dump_dir,
                              const std::string& expected_suffix, bool allow_outside_dir, std::string& normalized,
                              std::string& error) {
  if (requested.empty()) {
    normalized.clear();
    return true;
  }

  const std::filesystem::path requested_path(requested);

  if VUNLIKELY (to_lower(requested_path.extension().string()) != expected_suffix) {
    error = "out_file suffix must be " + expected_suffix;
    return false;
  }

  std::error_code ec;
  auto root = std::filesystem::absolute(std::filesystem::path(dump_dir), ec);

  if VUNLIKELY (ec) {
    error = "cannot resolve dump_dir: " + ec.message();
    return false;
  }

  root = std::filesystem::weakly_canonical(root, ec);

  if VUNLIKELY (ec) {
    error = "cannot canonicalize dump_dir: " + ec.message();
    return false;
  }

  auto candidate = requested_path.is_absolute() ? requested_path : root / requested_path;
  candidate = std::filesystem::weakly_canonical(candidate, ec);

  if VUNLIKELY (ec) {
    error = "cannot canonicalize out_file: " + ec.message();
    return false;
  }

  if VUNLIKELY (!allow_outside_dir && !path_is_within(root, candidate)) {
    error = "out_file must be inside dump_dir unless allow_outside_dir is true";
    return false;
  }

  normalized = candidate.string();

  return true;
}

static bool parse_config(const std::string& path, vlink::TriggerRecorder::Config& config, DaemonOptions& options) {
  std::ifstream file(path);

  if VUNLIKELY (!file.is_open()) {
    std::cerr << "Cannot open config: " << path << std::endl;
    return false;
  }

  nlohmann::json data;

  try {
    file >> data;

    if VUNLIKELY (!data.is_object()) {
      std::cerr << "Config root must be a JSON object." << std::endl;
      return false;
    }

    if (data.contains("method_url")) {
      options.method_url = data.at("method_url").get<std::string>();
    }

    if (data.contains("dump_dir")) {
      config.dump_dir = data.at("dump_dir").get<std::string>();
    }

    if (data.contains("allow_outside_dir")) {
      options.allow_outside_dir = data.at("allow_outside_dir").get<bool>();
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
      if VUNLIKELY (!mb_to_bytes(data.at("default_max_packet_size").get<double>(), "default_max_packet_size",
                                 config.default_max_packet_size)) {
        return false;
      }
    }

    if (data.contains("default_max_size")) {
      if VUNLIKELY (!mb_to_bytes(data.at("default_max_size").get<double>(), "default_max_size",
                                 config.default_max_size)) {
        return false;
      }
    }

    if (data.contains("max_cache_size")) {
      if VUNLIKELY (!mb_to_bytes(data.at("max_cache_size").get<double>(), "max_cache_size", config.max_cache_size)) {
        return false;
      }
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
        std::cerr << "Warning: unknown overflow '" << overflow << "', using drop." << std::endl;
        config.overflow = vlink::TriggerRecorder::kDropNewest;
      }
    }

    if (data.contains("sleep_interval_mb")) {
      if VUNLIKELY (!mb_to_bytes(data.at("sleep_interval_mb").get<double>(), "sleep_interval_mb",
                                 config.sleep_interval)) {
        return false;
      }
    }

    if (data.contains("sleep_time_ms")) {
      config.sleep_time_ms = data.at("sleep_time_ms").get<int64_t>();
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
      options.bag_plugin_lib = data.at("bag_plugin").get<std::string>();
    }

    if (data.contains("bag_plugin_dir")) {
      options.bag_plugin_dir = data.at("bag_plugin_dir").get<std::string>();
    }

    if (data.contains("trigger_plugin")) {
      options.trigger_plugin_lib = data.at("trigger_plugin").get<std::string>();
    }

    if (data.contains("trigger_plugin_dir")) {
      options.trigger_plugin_dir = data.at("trigger_plugin_dir").get<std::string>();
    }

    if (data.contains("trigger_plugin_config")) {
      options.trigger_plugin_config = data.at("trigger_plugin_config").get<std::string>();
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
          if VUNLIKELY (!mb_to_bytes(item.at("max_packet_size").get<double>(),
                                     "url_overrides." + url + ".max_packet_size", uc.max_packet_size)) {
            return false;
          }
        }

        if (item.contains("max_size")) {
          if VUNLIKELY (!mb_to_bytes(item.at("max_size").get<double>(), "url_overrides." + url + ".max_size",
                                     uc.max_size)) {
            return false;
          }
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

static int run_daemon(const DaemonArguments& arguments) {
  vlink::TriggerRecorder::Config config;
  DaemonOptions options;

  if VUNLIKELY (!arguments.config_path.empty() && !parse_config(arguments.config_path, config, options)) {
    return 1;
  }

  if (arguments.bag_plugin_lib) {
    options.bag_plugin_lib = *arguments.bag_plugin_lib;
  }

  if (arguments.trigger_plugin_lib) {
    options.trigger_plugin_lib = *arguments.trigger_plugin_lib;
  }

  if (arguments.trigger_plugin_config) {
    options.trigger_plugin_config = *arguments.trigger_plugin_config;
  }

  if (config.dump_dir.empty()) {
    config.dump_dir = vlink::Utils::get_tmp_dir() + "/vlink-trigger";
  }

  std::error_code dump_dir_error;
  const auto absolute_dump_dir = std::filesystem::absolute(config.dump_dir, dump_dir_error);

  if VUNLIKELY (dump_dir_error) {
    std::cerr << "Cannot resolve dump_dir: " << dump_dir_error.message() << std::endl;
    return 1;
  }

  config.dump_dir = absolute_dump_dir.lexically_normal().string();

  if (arguments.native_mode) {
    config.discovery_filter = vlink::DiscoveryViewer::kFilterNative;
  }

  if VUNLIKELY (!vlink::Utils::check_singleton("vlink-trigger")) {
    std::cerr << "vlink-trigger is already running." << std::endl;
    return 1;
  }

  vlink::Plugin plugin_loader;
  std::shared_ptr<vlink::BagPluginInterface> bag_plugin;
  std::shared_ptr<vlink::TriggerPluginInterface> trigger_plugin;

  if (!options.bag_plugin_lib.empty()) {
    bag_plugin = plugin_loader.load<vlink::BagPluginInterface>(options.bag_plugin_lib, 2, 0, options.bag_plugin_dir);

    if VUNLIKELY (!bag_plugin) {
      std::cerr << "Failed to load bag plugin '" << options.bag_plugin_lib << "'." << std::endl;
      return 1;
    }
  }

  if (!options.trigger_plugin_lib.empty()) {
    trigger_plugin =
        plugin_loader.load<vlink::TriggerPluginInterface>(options.trigger_plugin_lib, 2, 0, options.trigger_plugin_dir);

    if VUNLIKELY (!trigger_plugin) {
      std::cerr << "Failed to load trigger plugin '" << options.trigger_plugin_lib << "'." << std::endl;
      return 1;
    }

    if VUNLIKELY (!trigger_plugin->init(options.trigger_plugin_config)) {
      std::cerr << "Failed to initialize trigger plugin '" << options.trigger_plugin_lib << "'." << std::endl;
      return 1;
    }
  }

  std::unique_ptr<vlink::TriggerRecorder> recorder;

  try {
    recorder = std::make_unique<vlink::TriggerRecorder>(
        config, [native_mode = arguments.native_mode](const std::string& url, vlink::InitType type) {
          auto sub = vlink::TriggerRecorder::RawSub::create_shared(url, type);

          if (native_mode && sub) {
            sub->set_property("dds.ip", "127.0.0.1");
          }

          return sub;
        });
  } catch (const std::exception& e) {
    std::cerr << "Failed to initialize trigger recorder: " << e.what() << std::endl;
    return 1;
  }

  if (trigger_plugin) {
    recorder->bind_trigger_interface(trigger_plugin);
  }

  if (bag_plugin) {
    recorder->bind_bag_interface(bag_plugin);
  }

  if VUNLIKELY (!recorder->async_run()) {
    std::cerr << "Failed to start trigger recorder." << std::endl;
    return 1;
  }

  recorder->invoke_task([]() {}).wait();

  std::shared_ptr<vlink::Server<std::string, std::string>> server;

  try {
    server = vlink::Server<std::string, std::string>::create_shared(options.method_url);
  } catch (const std::exception& e) {
    std::cerr << "Invalid method_url (" << options.method_url << "): " << e.what() << std::endl;
    recorder->quit();
    recorder->wait_for_quit();
    return 1;
  }

  server->set_safety_quit(true);

  const bool allow_outside_dir = options.allow_outside_dir;
  const std::string expected_suffix = config.file_type == vlink::TriggerRecorder::kVcap ? ".vcap" : ".vdb";
  const std::string dump_dir = config.dump_dir;

  const bool listening = server->listen(
      [&recorder, allow_outside_dir, dump_dir, expected_suffix](const std::string& request, std::string& response) {
        nlohmann::json resp;
        resp["ok"] = false;
        resp["error_code"] = 0;
        resp["error_string"] = "";

        try {
          const nlohmann::json req = nlohmann::json::parse(request);

          if VUNLIKELY (req.value("type", std::string()) != "dump") {
            resp["error_code"] = 3;
            resp["error_string"] = "Unknown request type";
            VLOG_W("vlink-trigger: unknown request type");
            response = resp.dump();

            return;
          }

          vlink::TriggerRecorder::TriggerParams params;
          const std::string requested_out_file = req.value("out_file", std::string());
          std::string request_error;

          if VUNLIKELY (!validate_out_file(requested_out_file, dump_dir, expected_suffix, allow_outside_dir,
                                           params.out_file, request_error) ||
                        !parse_trigger_window(req, "pre_ms", params.pre_ms, request_error) ||
                        !parse_trigger_window(req, "post_ms", params.post_ms, request_error)) {
            resp["error_code"] = 4;
            resp["error_string"] = "Invalid request: " + request_error;
            VLOG_W("vlink-trigger: invalid dump request: ", request_error);
            response = resp.dump();
            return;
          }

          params.reason = req.value("reason", std::string());
          params.name_hint = req.value("name_hint", std::string());
          params.filter_str = req.value("filter_str", std::string());
          params.black_mode = req.value("black_mode", false);

          if (req.contains("whitelist")) {
            for (const auto& url : req.at("whitelist").get_ref<const nlohmann::json::array_t&>()) {
              params.whitelist.emplace(url.get<std::string>());
            }
          }

          if (req.contains("blacklist")) {
            for (const auto& url : req.at("blacklist").get_ref<const nlohmann::json::array_t&>()) {
              params.blacklist.emplace(url.get<std::string>());
            }
          }

          VLOG_I("vlink-trigger: dump request received reason=", params.reason, " pre_ms=", params.pre_ms,
                 " post_ms=", params.post_ms, " whitelist=", params.whitelist.size(),
                 " blacklist=", params.blacklist.size());

          std::string accepted_out_file;
          const bool ok = recorder->dump(params, accepted_out_file);

          resp["ok"] = ok;

          if VUNLIKELY (!ok) {
            resp["error_code"] = 1;
            resp["error_string"] = "Recorder is busy or not running";
            VLOG_W("vlink-trigger: dump request rejected: recorder is busy or not running");
          } else {
            resp["out_file"] = accepted_out_file;
            VLOG_I("vlink-trigger: dump request accepted -> ", accepted_out_file);
          }
        } catch (const std::exception& e) {
          resp["ok"] = false;
          resp["error_code"] = 4;
          resp["error_string"] = std::string("Invalid request: ") + e.what();
          VLOG_W("vlink-trigger: invalid dump request: ", e.what());
        }

        response = resp.dump();
      });

  if VUNLIKELY (!listening) {
    std::cerr << "Failed to listen on " << options.method_url << "." << std::endl;
    recorder->quit();
    recorder->wait_for_quit();
    return 1;
  }

  std::mutex quit_mtx;
  vlink::ConditionVariable quit_cv;
  bool quit_flag = false;

  vlink::Utils::register_terminate_signal(
      [&quit_mtx, &quit_cv, &quit_flag](int) {
        {
          std::lock_guard lock(quit_mtx);
          quit_flag = true;
        }

        quit_cv.notify_all();
      },
      true);

  VLOG_I("vlink-trigger daemon started, method_url=", options.method_url);

  {
    std::unique_lock lock(quit_mtx);
    quit_cv.wait(lock, [&quit_flag] { return quit_flag; });
  }

  server.reset();

  while (recorder->is_dumping()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  recorder->quit();
  recorder->wait_for_quit();

  return 0;
}

static int run_dump(const std::string& method_url, const std::string& out_file, const std::string& reason,
                    const std::string& name_hint, int64_t pre_ms, int64_t post_ms,
                    const std::vector<std::string>& filter_url_list, const std::string& filter_str, bool black_mode) {
  if VUNLIKELY (!is_valid_trigger_window(pre_ms) || !is_valid_trigger_window(post_ms)) {
    std::cerr << "--pre and --post must each be -1 or a non-negative millisecond value within the recorder's "
                 "supported range."
              << std::endl;
    return 1;
  }

  std::unique_ptr<vlink::Client<std::string, std::string>> client;

  try {
    client = std::make_unique<vlink::Client<std::string, std::string>>(method_url);
  } catch (const std::exception& e) {
    std::cerr << "Invalid method_url (" << method_url << "): " << e.what() << std::endl;
    return 1;
  }

  if VUNLIKELY (!client->wait_for_connected(std::chrono::milliseconds(3000))) {
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
  req["whitelist"] = black_mode ? std::vector<std::string>() : filter_url_list;
  req["blacklist"] = black_mode ? filter_url_list : std::vector<std::string>();

  std::string response;

  if VUNLIKELY (!client->invoke(req.dump(), response, std::chrono::seconds(10))) {
    std::cerr << "Dump invoke failed." << std::endl;
    return 1;
  }

  std::string accepted_out_file;

  try {
    const nlohmann::json resp = nlohmann::json::parse(response);

    if VUNLIKELY (!resp.value("ok", false)) {
      std::cerr << "Dump rejected: " << resp.value("error_string", std::string()) << " (code "
                << resp.value("error_code", 0) << ")" << std::endl;
      return 1;
    }

    accepted_out_file = resp.value("out_file", std::string());

    if VUNLIKELY (accepted_out_file.empty()) {
      std::cerr << "Invalid response: missing out_file" << std::endl;
      return 1;
    }
  } catch (const std::exception& e) {
    std::cerr << "Invalid response: " << e.what() << std::endl;
    return 1;
  }

  std::cout << "Dump accepted: " << accepted_out_file << std::endl;

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
  daemon_command.add_argument("-c", "--config").help("Optional config json path").default_value(std::string());
  daemon_command.add_argument("-n", "--native")
      .help("Native mode: local-host discovery + dds.ip=127.0.0.1")
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
