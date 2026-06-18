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

#include "./extension/schema_plugin_manager.h"

#include <memory>
#include <string>

#include "./base/utils.h"

namespace vlink {

// SchemaPluginManager
struct SchemaPluginManager::Impl {
  Plugin plugin;
  std::shared_ptr<SchemaPluginInterface> interface;
};

SchemaPluginManager& SchemaPluginManager::get(const std::string& schema_plugin_path) {
  static SchemaPluginManager global_schema_plugin(schema_plugin_path);
  return global_schema_plugin;
}

bool SchemaPluginManager::is_valid() const { return impl_->interface != nullptr; }

std::shared_ptr<SchemaPluginInterface> SchemaPluginManager::get_interface() const { return impl_->interface; }

SchemaPluginManager::SchemaPluginManager(std::string schema_plugin_path) : impl_(std::make_unique<Impl>()) {
  if (schema_plugin_path.empty()) {
    schema_plugin_path = Utils::get_env("VLINK_SCHEMA_PLUGIN");

    if (schema_plugin_path.empty()) {
      return;
    }
  }

  impl_->interface = impl_->plugin.load<SchemaPluginInterface>(schema_plugin_path, 1, 0);

  if VLIKELY (impl_->interface) {
    auto version_info = impl_->interface->get_version_info();

    VLOG_D("");
    VLOG_D("##########################################################");
    VLOG_D("#  Plugin Name: ", version_info.name);
    VLOG_D("#  Version:     ", version_info.version);
    VLOG_D("#  Timestamp:   ", version_info.timestamp);
    VLOG_D("#  Tag:         ", version_info.tag);
    VLOG_D("#  Commit:      ", version_info.commit_id);
    VLOG_D("##########################################################");
    VLOG_D("");
  }
}

SchemaPluginManager::~SchemaPluginManager() {
  impl_->interface.reset();
  impl_->plugin.clear();
}

}  // namespace vlink
