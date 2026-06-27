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

/**
 * @file schema_plugin_manager.h
 * @brief Process-wide singleton wrapper that loads and owns a @c SchemaPluginInterface implementation.
 *
 * @details
 * @c SchemaPluginManager hides the dynamic-loader plumbing required to surface a single shared
 * @c SchemaPluginInterface to every component of the running process.  It is the recommended
 * entry point for CLI tools (@c eproto, @c efbs), webviz converters, and bag writers that need
 * to resolve Protobuf and FlatBuffers metadata without each subsystem rolling its own loader.
 *
 * @par Manager state machine
 * @code
 *           +-------------+   first get(path)         +-----------+   library missing   +---------+
 *           |  not built  | ------------------------> | resolving | ------------------> | invalid |
 *           +-------------+                           +-----------+                     +---------+
 *                                                          | load ok
 *                                                          v
 *                                                     +---------+
 *                                                     |  valid  | <-- get_interface() returns plugin
 *                                                     +---------+
 *                                                          |
 *                                                          v  process exit
 *                                                     +---------+
 *                                                     | unloaded |  interface released before loader
 *                                                     +---------+
 * @endcode
 *
 * @par Resolution order
 * 1. The @p schema_plugin_path argument supplied to the very first @c get() call.
 * 2. The @c VLINK_SCHEMA_PLUGIN environment variable when the argument is empty.
 * 3. No plugin loaded (the manager reports @c is_valid() == @c false).
 *
 * @par Example
 * @code
 *   auto& mgr = vlink::SchemaPluginManager::get("/opt/vlink/libschema_plugin.so");
 *
 *   if (mgr.is_valid()) {
 *     auto plugin = mgr.get_interface();
 *     auto schema = plugin->search_schema("demo.proto.PointCloud", vlink::SchemaType::kProtobuf);
 *     VLOG_I("loaded schema: ", schema.name);
 *   }
 * @endcode
 */

#pragma once

#include <memory>
#include <string>

#include "./schema_plugin_interface.h"

namespace vlink {

/**
 * @class SchemaPluginManager
 * @brief Singleton accessor that owns a lazily loaded @c SchemaPluginInterface plugin.
 *
 * @details
 * Subsequent @c get() invocations are cheap and return the cached singleton regardless of
 * the argument passed; the very first call wins.  The destructor releases the contained
 * interface before tearing down the @c Plugin loader, ensuring the shared object outlives
 * any dependent global objects inside it.
 */
class VLINK_EXPORT SchemaPluginManager final {
 public:
  /**
   * @brief Returns the process-wide manager, building it on the first call.
   *
   * @param schema_plugin_path  Absolute path to the plugin shared object.  Empty means
   *                            fall back to the @c VLINK_SCHEMA_PLUGIN environment variable.
   * @return Reference to the singleton instance.
   */
  [[nodiscard]] static SchemaPluginManager& get(const std::string& schema_plugin_path = "");

  /**
   * @brief Reports whether a plugin was successfully loaded.
   *
   * @return @c true when @c get_interface() will yield a non-null pointer.
   */
  [[nodiscard]] bool is_valid() const;

  /**
   * @brief Returns the shared plugin instance, or @c nullptr when the manager is invalid.
   *
   * @return Shared pointer to the loaded @c SchemaPluginInterface implementation.
   */
  [[nodiscard]] std::shared_ptr<SchemaPluginInterface> get_interface() const;

 private:
  explicit SchemaPluginManager(std::string schema_plugin_path);

  ~SchemaPluginManager();

  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(SchemaPluginManager)
};
}  // namespace vlink
