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
#include <utility>
#include <vector>

static constexpr char kDefaultMethodUrl[] = "dds://trigger/method";

struct DaemonArguments final {
  std::string config_path;
  bool native_mode{false};
  std::optional<std::string> bag_plugin_lib;
  std::optional<std::string> trigger_plugin_lib;
  std::optional<std::string> trigger_plugin_config;
};

bool is_valid_trigger_window(int64_t value);

int run_dump(const std::string& method_url, const std::string& out_file, const std::string& reason,
             const std::string& name_hint, int64_t pre_ms, int64_t post_ms,
             const std::vector<std::string>& filter_url_list, const std::string& filter_str, bool black_mode);

int run_daemon(const DaemonArguments& arguments);
