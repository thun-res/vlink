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
// File: greeter_interface.h
//
// Public interface that the host process loads from a shared library at
// runtime. The interface header is the contract shared between the host
// executable and the plugin .so: both translation units must include it and
// agree on the layout, method signatures and the "plugin id" string.
//
// vlink's plugin system uses C++ virtual dispatch across a dlopen boundary.
// For that to work safely, every interface class participates in a small
// boilerplate dance:
//   - VLINK_PLUGIN_REGISTER expands to a static get_plugin_id() that returns
//     a stable, compiler-independent identifier string. The same macro is
//     placed on the implementation class so the loader can match interface
//     to implementation across DSOs.
//   - The implementation .cc file additionally invokes VLINK_PLUGIN_DECLARE
//     which exports the extern "C" factory / destroyer / version-info symbols
//     that vlink::Plugin::load<T>() resolves via dlsym.
// =============================================================================

#ifndef EXAMPLES_PLUGIN_PLUGIN_BASIC_GREETER_INTERFACE_H_
#define EXAMPLES_PLUGIN_PLUGIN_BASIC_GREETER_INTERFACE_H_

#include <vlink/base/plugin.h>

#include <string>

// User-defined abstract interface. Anything callable across the plugin
// boundary must be a virtual method. Trivially-copyable parameters and
// std::string are safe because the .so and host are guaranteed (by build
// rules) to use the same libstdc++ ABI.
class GreeterInterface {
  // Generates a static get_plugin_id() returning a stable_id derived from the
  // interface name "GreeterInterface". The exact same macro argument MUST be
  // used on the implementation class so the loader can pair them up.
  // The stable_id is computed by the macro at compile time to avoid relying
  // on typeid/RTTI, which is not portable across compilers/linkers.
  VLINK_PLUGIN_REGISTER(GreeterInterface)

 public:
  // Virtual destructor is mandatory: vlink::Plugin::load<T> returns a
  // shared_ptr<T> whose custom deleter calls vlink_plugin_destroy() on the
  // derived object via the base pointer; without a virtual dtor we would
  // slice / leak.
  virtual ~GreeterInterface() = default;

  // Sample method: returns a greeting. Marshalled via std::string copy.
  virtual std::string greet(const std::string& name) = 0;

  // Identification helper so the host can verify the implementation type.
  virtual std::string plugin_name() const = 0;
};

#endif  // EXAMPLES_PLUGIN_PLUGIN_BASIC_GREETER_INTERFACE_H_
