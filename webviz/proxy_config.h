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

#include <argparse/argparse.hpp>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

#include "proxy_bridge.h"

namespace vlink {
namespace webviz {

class ProxyConfigHelper final {
 public:
  using Json = nlohmann::json;

  static void add_arguments(argparse::ArgumentParser& program);

  static bool apply_config(const Json& root, const std::filesystem::path& config_file,
                           const argparse::ArgumentParser& program, ProxyBridge::Config& config, std::string& error);

  static bool apply_arguments(const argparse::ArgumentParser& program, ProxyBridge::Config& config, std::string& error);

  static bool validate(const ProxyBridge::Config& config, std::string& error);

 private:
  static std::string normalize_token(std::string_view value);

  static bool is_valid_dds_impl(std::string_view value);

  static bool parse_role(std::string_view value, ProxyAPI::Role& role);

  static bool parse_iox_monitoring(std::string_view value, bool& enabled);
};

}  // namespace webviz
}  // namespace vlink
