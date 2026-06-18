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
// File: greeter_plugin.cc
//
// Implementation translation unit that becomes libgreeter_plugin.so. It is
// dlopen()'d by the host (plugin_basic.cc) at runtime; no static linking.
//
// Three things must line up between this file and greeter_interface.h:
//   1. The class derives publicly from GreeterInterface so the host can use
//      a base-class pointer.
//   2. VLINK_PLUGIN_REGISTER is invoked with the SAME argument as on the
//      interface (the interface name, not the implementation name). That
//      installs the plugin_id matching the host's expectation.
//   3. VLINK_PLUGIN_DECLARE exports the C ABI entry points the loader needs:
//        - vlink_plugin_create  -> new GreeterImpl
//        - vlink_plugin_destroy -> delete the base pointer
//        - vlink_plugin_get_version_info -> returns major/minor/commit
//      Version numbers (1, 0) are compared against the (major, minor) passed
//      to Plugin::load<T>(name, major, minor). A mismatch causes load() to
//      return nullptr rather than dispatch into ABI-incompatible code.
// =============================================================================

#include "greeter_interface.h"

class GreeterImpl : public GreeterInterface {
  // Must repeat the SAME interface name here so the loader can match this
  // .so to the interface declared in the host. Passing "GreeterImpl" here
  // by mistake would make Plugin::load<GreeterInterface>() fail with
  // "plugin_id mismatch".
  VLINK_PLUGIN_REGISTER(GreeterInterface)

 public:
  std::string greet(const std::string& name) override { return "Hello, " + name + "!"; }

  std::string plugin_name() const override { return "GreeterImpl"; }
};

// Emits extern "C" factory + destroyer + version metadata. Arguments:
//   GreeterImpl -> concrete type to instantiate on create
//   1           -> major version (semantic-version-style break gate)
//   0           -> minor version (compatible additions)
// The host requests (1, 0) and gets a valid handle; (2, 0) is rejected by the
// loader to protect against ABI drift.
VLINK_PLUGIN_DECLARE(GreeterImpl, 1, 0)
