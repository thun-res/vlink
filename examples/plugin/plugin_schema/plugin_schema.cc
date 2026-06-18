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

// =============================================================================
// File: plugin_schema.cc
//
// Demonstrates SchemaPluginInterface: a vlink-defined plugin contract that
// lets a third-party .so supply protobuf / flatbuffers descriptors and
// serialized schema bytes WITHOUT the host having to link protobuf or
// flatbuffers itself.
//
// Why type-erasure: vlink_core must remain free of a hard dependency on
// protobuf / flatbuffers (some downstream users do not want either). To
// achieve that, SchemaPluginInterface returns "const void*" for protobuf
// Descriptor* and Message*, plus an opaque parser handle for flatbuffers.
// Callers that own those headers cast back to the real type; callers that
// do not still get the binary schema bytes via search_schema().
//
// Two access paths are demonstrated:
//   1. demo_direct_load(): construct a vlink::Plugin and load a single
//      schema .so manually.
//   2. demo_manager(): use vlink::SchemaPluginManager singleton, which is
//      auto-populated by vlink core when VLINK_SCHEMA_PLUGIN env var is set.
//      Other vlink components (BagWriter, BagReader, ProxyServer, etc.) use
//      this manager internally to discover schemas.
// =============================================================================

#include <vlink/base/logger.h>
#include <vlink/base/plugin.h>
#include <vlink/extension/schema_plugin_interface.h>
#include <vlink/extension/schema_plugin_manager.h>

#include <cstdlib>
#include <string>

// Demo 1: explicit Plugin::load. Useful when an app wants to manage the
// schema plugin's lifetime itself rather than via the singleton.
static void demo_direct_load() {
  VLOG_I("=== Direct Plugin::load<SchemaPluginInterface> ===");

  vlink::Plugin plugin;
  plugin.set_log_level(vlink::Logger::kInfo);

  // Allow override via env so this demo works with any schema .so the
  // user has built.
  const char* env_name = std::getenv("VLINK_SCHEMA_PLUGIN");
  std::string lib_name = env_name ? env_name : "vlink_schema_plugin";

  VLOG_I("Attempting to load: ", lib_name);
  VLOG_I("plugin_id: ", std::string(vlink::SchemaPluginInterface::get_plugin_id()));

  // load<SchemaPluginInterface>: dlopen + dlsym + version check against
  // VLINK_PLUGIN_DECLARE(..., 1, 0) in the plugin's .cc.
  auto schema_plugin = plugin.load<vlink::SchemaPluginInterface>(lib_name, 1, 0);

  if (!schema_plugin) {
    VLOG_W("Schema plugin not available (protobuf dependency required).");
    VLOG_I("Set VLINK_SCHEMA_PLUGIN=<so_path> to enable this demo.");
    return;
  }

  // Section: identify the plugin -- name / version / git commit. Useful when
  // multiple schema plugins coexist in the build.
  auto ver = schema_plugin->get_version_info();
  VLOG_I("plugin: ", ver.name, " v", ver.version, " commit=", ver.commit_id);

  const std::string type_name = "example.SensorData";

  // search_protobuf_descriptor returns a void* that callers with protobuf
  // headers may cast to google::protobuf::Descriptor*. Returning void* keeps
  // SchemaPluginInterface independent of protobuf's public API.
  auto* desc = schema_plugin->search_protobuf_descriptor(type_name);
  VLOG_I("search_protobuf_descriptor(\"", type_name, "\"): ", desc ? "found" : "not found");

  // search_schema returns the SERIALIZED schema bytes (FileDescriptorSet for
  // protobuf, BFBS for flatbuffers, etc.) plus an encoding tag. Anyone can
  // consume this without linking protobuf -- it is just bytes.
  auto schema = schema_plugin->search_schema(type_name, vlink::SchemaType::kProtobuf);

  if (!schema.data.empty()) {
    VLOG_I("schema: ", schema.name, " encoding=", schema.encoding, " ", schema.data.size(), " bytes");
  } else {
    VLOG_I("schema: not found");
  }

  // Same type-erasure trick for Message factories.
  auto* msg = schema_plugin->create_protobuf_message(type_name);
  VLOG_I("create_protobuf_message: ", msg ? "ok" : "not found");

  // Flatbuffers binary schema (BFBS) lookup; opaque pointer same idea.
  auto* fbs_schema = schema_plugin->search_flatbuffers_schema("example.SensorFrame");
  VLOG_I("search_flatbuffers_schema: ", fbs_schema ? "found" : "not found");

  // Manual teardown: drop the handle, then clear() removes the registry
  // entry. Same .so / dlclose contract as plugin_basic.
  schema_plugin.reset();
  plugin.clear();
}

// Demo 2: singleton manager. vlink core uses this internally so that any
// component (BagWriter, BagReader, web visualisation) can look up schemas
// without each owning its own Plugin instance.
static void demo_manager() {
  VLOG_I("=== SchemaPluginManager singleton ===");

  vlink::SchemaPluginManager& mgr = vlink::SchemaPluginManager::get();

  if (!mgr.is_valid()) {
    VLOG_W("No plugin loaded (set VLINK_SCHEMA_PLUGIN).");
    return;
  }

  // get_interface returns the SchemaPluginInterface* the manager loaded at
  // startup based on VLINK_SCHEMA_PLUGIN.
  auto iface = mgr.get_interface();

  if (!iface) {
    VLOG_E("get_interface() returned nullptr");
    return;
  }

  auto ver = iface->get_version_info();
  VLOG_I("Manager plugin: ", ver.name, " v", ver.version);

  // Same search_schema API as before, just routed through the singleton.
  auto schema = iface->search_schema("example.SensorData", vlink::SchemaType::kProtobuf);
  VLOG_I("search_schema: ", schema.data.empty() ? "not found" : std::to_string(schema.data.size()) + " bytes");
}

int main() {
  // File-level intro to the SchemaPluginInterface surface.
  VLOG_I("SchemaPluginInterface capabilities:");
  VLOG_I("  search_protobuf_descriptor / search_schema / get_all_schemas");
  VLOG_I("  create_protobuf_message / search_flatbuffers_schema / create_flatbuffers_parser");

  demo_direct_load();
  demo_manager();

  VLOG_I("Schema plugin example complete.");
  return 0;
}
