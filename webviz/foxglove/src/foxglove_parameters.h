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

#include <vlink/base/bytes.h>
#include <vlink/base/macros.h>
#include <vlink/setter.h>

#include <atomic>
#include <map>
#include <nlohmann/json.hpp>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "proxy_bridge.h"

namespace vlink {
namespace webviz {

class FoxgloveParameters final {
 public:
  struct ParameterEntry final {
    std::string name;
    nlohmann::json value;
    std::string type;
    bool has_value{false};
  };

  struct Config final {
    std::string url;
    std::vector<ParameterEntry> values;
    ProxyBridge::TransportConfig transport;
  };

  explicit FoxgloveParameters(const Config& config);

  ~FoxgloveParameters();

  bool start();

  void stop();

  [[nodiscard]] bool enabled() const { return !config_.url.empty() || !config_.values.empty(); }

  [[nodiscard]] bool active() const { return started_.load(); }

  [[nodiscard]] std::vector<std::string> get_names() const;

  static bool parse_config_values(const nlohmann::json& parameters_root, std::vector<ParameterEntry>& out,
                                  std::string& error);

  [[nodiscard]] nlohmann::json build_parameter_values(const std::vector<std::string>& names, std::string_view id) const;

  bool apply_set_parameters(const nlohmann::json& request, nlohmann::json& response, std::vector<ParameterEntry>& delta,
                            std::string& error);

 private:
  using ParameterMap = std::map<std::string, ParameterEntry>;

  static bool is_supported_parameter_type(std::string_view type);

  static bool is_supported_parameter_value(const nlohmann::json& value);

  static bool validate_parameter_value(const nlohmann::json& value, std::string_view type, std::string& error);

  static bool parse_parameter_entry(const nlohmann::json& item, ParameterEntry& out, std::string& error);

  static bool parse_parameter_entries(const nlohmann::json& root, std::vector<ParameterEntry>& out, std::string& error);

  static nlohmann::json make_parameter_json(const ParameterEntry& entry);

  static bool encode_json_payload(const ParameterMap& state, Bytes& payload);

  static std::vector<ParameterEntry> diff_states(const ParameterMap& old_state, const ParameterMap& new_state);

  Config config_;
  Setter<Bytes>::SharedPtr setter_;
  mutable std::shared_mutex state_mtx_;
  ParameterMap state_;
  std::atomic_bool started_{false};

  VLINK_DISALLOW_COPY_AND_ASSIGN(FoxgloveParameters)
};

}  // namespace webviz
}  // namespace vlink
