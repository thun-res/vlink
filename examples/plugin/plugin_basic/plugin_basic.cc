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
// File: plugin_basic.cc
//
// Host-side demo of vlink::Plugin: dlopen a user-defined interface from a
// shared library, call methods through the abstract base, then unload.
//
// Walk-through:
//   1. Print the loader's default search paths (so user knows where to drop
//      libgreeter_plugin.so).
//   2. Construct a vlink::Plugin (one Plugin instance owns a registry of
//      loaded handles; destructing it unloads everything).
//   3. load<GreeterInterface>("greeter_plugin", 1, 0) -> resolves to
//      lib<name>.so, dlsym's the version info, validates major/minor, calls
//      vlink_plugin_create, and wraps the raw pointer in a shared_ptr<T>
//      whose custom deleter invokes vlink_plugin_destroy then dlclose().
//      Order matters: destroy first (still inside the .so) then dlclose
//      (unmaps the code that destroy lives in).
//   4. Demonstrate introspection (has_loaded, complex_id, plugin_id).
//   5. Demonstrate version-mismatch handling and reload.
// =============================================================================

#include <vlink/base/logger.h>
#include <vlink/base/plugin.h>

#include <string>

#include "greeter_interface.h"

int main() {
  // Section: print loader search paths so the user can verify lib placement.
  // default_search_path() pulls from compiled-in defaults, LD_LIBRARY_PATH
  // and VLINK_PLUGIN_PATH env vars, in priority order.
  for (const auto& p : vlink::Plugin::default_search_path()) {
    VLOG_I("search path: ", p);
  }

  // Section: construct the loader and lower its log level so we can see
  // dlopen / dlsym diagnostics during this demo.
  vlink::Plugin plugin;
  plugin.set_log_level(vlink::Logger::kInfo);

  // Section: load the plugin. (1, 0) must match VLINK_PLUGIN_DECLARE in
  // greeter_plugin.cc. The returned shared_ptr owns the dlopen handle via
  // its deleter, so simply letting it go out of scope unloads cleanly.
  auto greeter = plugin.load<GreeterInterface>("greeter_plugin", 1, 0);

  if (!greeter) {
    VLOG_E("Failed to load greeter_plugin. Make sure libgreeter_plugin.so is in a search path.");
    return 1;
  }

  // Section: call virtual methods through the abstract base. Behind the
  // scenes these are normal C++ vtable calls; the vtable lives inside the
  // .so so we must not call them after dlclose.
  VLOG_I("plugin_name(): ", greeter->plugin_name());
  VLOG_I("greet(\"VLink\"): ", greeter->greet("VLink"));
  VLOG_I("greet(\"World\"): ", greeter->greet("World"));

  // Section: introspection helpers. complex_id encodes name+version+stable_id
  // so two .so files with the same logical name but different builds can be
  // distinguished. plugin_id is the cross-compiler stable identifier produced
  // by VLINK_PLUGIN_REGISTER for the interface type.
  VLOG_I("has_loaded: ", plugin.has_loaded<GreeterInterface>("greeter_plugin"));
  VLOG_I("complex_id: ", plugin.get_plugin_complex_id<GreeterInterface>("greeter_plugin"));
  VLOG_I("plugin_id:  ", std::string(GreeterInterface::get_plugin_id()));

  // Section: explicit unload. Releasing the shared_ptr drops the last
  // reference and the deleter runs; unload() then removes the bookkeeping
  // entry from the Plugin registry. Order: shared_ptr.reset() FIRST so the
  // destructor fires while the .so is still mapped; unload() merely tidies
  // up the registry.
  greeter.reset();
  VLOG_I("unload result: ", plugin.unload<GreeterInterface>("greeter_plugin"));

  // Section: version-mismatch path. The loader walks every candidate .so,
  // reads its version info, and rejects (returns nullptr) if (major, minor)
  // does not satisfy the request. This is the safety net that prevents the
  // host from calling into an ABI-incompatible plugin build.
  auto bad = plugin.load<GreeterInterface>("greeter_plugin", 2, 0);

  if (!bad) {
    VLOG_I("version mismatch -> nullptr (expected)");
  }

  // Section: re-load the correct version to show the registry handles
  // unload->load cycles.
  auto reloaded = plugin.load<GreeterInterface>("greeter_plugin", 1, 0);

  if (reloaded) {
    VLOG_I("reload OK: ", reloaded->greet("Reload"));
    reloaded.reset();
  }

  // Section: clear() releases every entry the Plugin still owns. The
  // destructor would do the same, but explicit clear() is useful in tests.
  plugin.clear();
  VLOG_I("Plugin basic example complete.");
  return 0;
}
