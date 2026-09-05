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

int run_dump(const std::string& method_url, const std::string& out_file, const std::string& reason,
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
