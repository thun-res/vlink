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

#include <vlink/base/logger.h>
#include <vlink/base/macros.h>
#include <vlink/base/plugin.h>
#include <vlink/extension/convert_plugin_interface.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "./webviz_app_utils.h"
#include "./webviz_types.h"

namespace vlink {
namespace webviz {

using Json = nlohmann::json;

inline bool is_valid_timestamp_unit(std::string_view unit) {
  return unit == "s" || unit == "ms" || unit == "us" || unit == "ns";
}

inline bool is_json_ser(std::string_view ser) { return ser == "json"; }

inline bool is_text_ser(std::string_view ser) { return ser == "text" || ser == "std::string" || ser == "string"; }

inline bool is_flatbuffers_encoding(std::string_view encoding) {
  return encoding == "flatbuffers" || encoding == "flatbuffer" || encoding == "fbs" || encoding == "bfbs";
}

inline bool is_target_encoding_compatible(std::string_view ser, std::string_view encoding) {
  if VLIKELY (encoding == "json") {
    return is_json_ser(ser);
  }

  if VLIKELY (encoding == "text") {
    return is_text_ser(ser);
  }

  if VLIKELY (encoding == "protobuf" || is_flatbuffers_encoding(encoding)) {
    return !ser.empty() && !is_json_ser(ser) && !is_text_ser(ser);
  }

  return false;
}

inline bool parse_timestamp_unit(const Json& obj, std::string_view key, std::string_view path,
                                 std::string_view entry_name, std::string& out) {
  out = obj.value(std::string(key), std::string("us"));

  if VUNLIKELY (!is_valid_timestamp_unit(out)) {
    MLOG_W("Invalid {} in {}: {} must be one of s/ms/us/ns", entry_name, path, key);
    return false;
  }

  return true;
}

inline bool parse_field_mappings(const Json& obj, std::string_view path, std::string_view entry_name,
                                 std::vector<FieldMapping>& out) {
  if VUNLIKELY (obj.contains("field_mappings") && !obj["field_mappings"].is_array()) {
    MLOG_W("Invalid {} in {}: field_mappings must be an array", entry_name, path);
    return false;
  }

  if VLIKELY (obj.contains("field_mappings")) {
    out.reserve(out.size() + obj["field_mappings"].size());

    for (const auto& fm : obj["field_mappings"]) {
      if VUNLIKELY (!fm.is_object()) {
        MLOG_W("Invalid {} in {}: field_mappings entries must be objects", entry_name, path);
        return false;
      }

      FieldMapping field;
      field.source = fm.value("source", std::string());
      field.target = fm.value("target", std::string());
      field.expression = fm.value("expression", std::string());

      if VLIKELY (fm.contains("default_value")) {
        field.has_default_value = true;

        if VLIKELY (fm["default_value"].is_string()) {
          field.default_value = fm["default_value"].get<std::string>();
          field.default_value_is_string = true;
        } else {
          field.default_value = fm["default_value"].dump();
        }
      }

      if VUNLIKELY (field.target.empty()) {
        MLOG_W("Invalid {} in {}: field_mappings target must not be empty", entry_name, path);
        return false;
      }

      if VUNLIKELY (field.source.empty() && field.expression.empty() && !field.has_default_value) {
        MLOG_W("Invalid {} in {}: field_mappings entry requires source, expression, or default_value", entry_name,
               path);
        return false;
      }

      out.emplace_back(std::move(field));
    }
  }

  return true;
}

inline bool parse_url_selector(const Json& obj, std::string_view path, std::string_view entry_name,
                               UrlSelector& selector, bool required = false) {
  selector = UrlSelector{};

  if VUNLIKELY (!obj.contains("url")) {
    if VLIKELY (!required) {
      return true;
    }

    MLOG_W("Invalid {} in {}: missing url", entry_name, path);
    return false;
  }

  const auto& value = obj["url"];
  selector.configured = true;

  if VLIKELY (value.is_string()) {
    auto url = value.get<std::string>();

    if VUNLIKELY (url.empty()) {
      MLOG_W("Invalid {} in {}: url must not be empty", entry_name, path);
      return false;
    }

    selector.whitelist_exact.emplace_back(std::move(url));
  } else if (value.is_array()) {
    for (const auto& item : value) {
      if VUNLIKELY (!item.is_string()) {
        MLOG_W("Invalid {} in {}: url array entries must be strings", entry_name, path);
        return false;
      }

      auto url = item.get<std::string>();

      if VUNLIKELY (url.empty()) {
        MLOG_W("Invalid {} in {}: url array entries must not be empty", entry_name, path);
        return false;
      }

      selector.whitelist_exact.emplace_back(std::move(url));
    }
  } else if (value.is_object()) {
    if VUNLIKELY (!append_json_filter_value(value, "whitelist", selector.whitelist_exact,
                                            selector.whitelist_patterns) ||
                  !append_json_filter_value(value, "blacklist", selector.blacklist_exact,
                                            selector.blacklist_patterns)) {
      MLOG_W("Invalid {} in {}: url whitelist/blacklist must be a string or string array", entry_name, path);
      return false;
    }
  } else {
    MLOG_W("Invalid {} in {}: url must be a string, array, or object", entry_name, path);
    return false;
  }

  normalize_url_selector(selector);

  if VUNLIKELY (selector.whitelist_exact.empty() && selector.whitelist_patterns.empty() &&
                selector.blacklist_exact.empty() && selector.blacklist_patterns.empty()) {
    MLOG_W("Invalid {} in {}: url selector must not be empty", entry_name, path);
    return false;
  }

  return true;
}

template <typename ParseOneT>
bool load_json_entries(const std::string& path, std::string_view not_found_prefix, std::string_view parse_error_prefix,
                       ParseOneT&& parse_one) {
  std::error_code ec;

  if VUNLIKELY (!std::filesystem::exists(path, ec) || ec) {
    MLOG_W("{}: {}", not_found_prefix, path);
    return false;
  }

  std::ifstream ifs(path);

  if VUNLIKELY (!ifs.is_open()) {
    MLOG_W("Failed to open JSON file: {}", path);
    return false;
  }

  try {
    Json root;
    ifs >> root;

    if VLIKELY (root.is_array()) {
      bool all_ok = true;

      for (const auto& item : root) {
        if VUNLIKELY (!parse_one(item)) {
          all_ok = false;
        }
      }

      return all_ok;
    }

    if VUNLIKELY (!root.is_object()) {
      MLOG_W("Invalid JSON root in {}: expected object or array", path);
      return false;
    }

    return parse_one(root);
  } catch (const std::exception& e) {
    MLOG_E("{} {}: {}", parse_error_prefix, path, e.what());
    return false;
  }
}

inline bool load_convert_plugin(const std::string& plugin_path, const std::string& plugin_config, Plugin& plugin_loader,
                                std::shared_ptr<ConvertPluginInterface>& plugin) {
  if VUNLIKELY (plugin_path.empty()) {
    return false;
  }

  plugin = plugin_loader.load<ConvertPluginInterface>(plugin_path, 4, 0);

  if VUNLIKELY (!plugin) {
    MLOG_E("Failed to load convert plugin: {}", plugin_path);
    return false;
  }

  if VUNLIKELY (!plugin->init(plugin_config)) {
    MLOG_E("Convert plugin init failed");
    plugin.reset();
    return false;
  }

  return true;
}

}  // namespace webviz
}  // namespace vlink
