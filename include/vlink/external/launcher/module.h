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
 * @file module.h
 * @brief Dynamic plugin module factory ABI and component export macros for Launcher libraries.
 *
 * @details
 * Defines @c LauncherModuleInterface loaded via @c vlink::Plugin::load() and the registration
 * macros @c VLINK_LAUNCHER_MODULE_BEGIN, @c VLINK_LAUNCHER_EXPORT_COMPONENT, and
 * @c VLINK_LAUNCHER_MODULE_END.
 *
 * @see component.h, launcher_component.h, base/plugin.h
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../../base/macros.h"
#include "../../base/plugin.h"
#include "./component.h"

namespace vlink {

/**
 * @class LauncherModuleInterface
 * @brief Factory plugin interface exported by shared libraries hosting @c LauncherComponent types.
 *
 * @details
 * Loaded once per shared library via @c vlink::Plugin::load\<LauncherModuleInterface\>().  The
 * host queries @c list_components() and calls @c create_component() / @c destroy_component()
 * to spawn any number of independent component instances from the same library.
 */
class LauncherModuleInterface {
  VLINK_PLUGIN_REGISTER_BY_ID(LauncherModuleInterface, "vlink.launcher.module")

 public:
  /**
   * @brief Major interface ABI version required by the host.
   */
  static constexpr uint16_t kVersionMajor = 1;

  /**
   * @brief Minimum minor interface ABI version required by the host.
   */
  static constexpr uint16_t kVersionMinor = 0;

  /**
   * @brief Virtual destructor.
   */
  virtual ~LauncherModuleInterface() = default;

  /**
   * @brief Lists all component class names exported by this shared library.
   *
   * @return Vector of registered class names.
   */
  [[nodiscard]] virtual std::vector<std::string> list_components() const = 0;

  /**
   * @brief Instantiates a component registered under @p class_name.
   *
   * @param class_name  Registered component class name; empty instantiates the sole class when unique.
   * @return            Raw owning pointer to release via @c destroy_component(), or @c nullptr on miss.
   */
  [[nodiscard]] virtual LauncherComponent* create_component(std::string_view class_name) const = 0;

  /**
   * @brief Destroys a component instance previously created by @c create_component().
   *
   * @param component  Instance pointer; @c nullptr is a no-op.
   */
  virtual void destroy_component(LauncherComponent* component) const noexcept = 0;

 protected:
  LauncherModuleInterface() = default;

 private:
  VLINK_DISALLOW_COPY_AND_ASSIGN(LauncherModuleInterface)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

namespace detail {

class LauncherModuleBase : public LauncherModuleInterface {
 public:
  using FactoryFn = LauncherComponent* (*)();

  [[nodiscard]] std::vector<std::string> list_components() const override {
    std::vector<std::string> names;
    names.reserve(factories_.size());

    for (const auto& entry : factories_) {
      names.push_back(entry.first);
    }

    return names;
  }

  [[nodiscard]] LauncherComponent* create_component(std::string_view class_name) const override {
    if (class_name.empty() && factories_.size() == 1U) {
      return factories_.begin()->second();
    }

    const auto iter = factories_.find(std::string(class_name));

    if (iter == factories_.end()) {
      return nullptr;
    }

    return iter->second();
  }

  void destroy_component(LauncherComponent* component) const noexcept override { delete component; }

 protected:
  std::unordered_map<std::string, FactoryFn> factories_;
};

}  // namespace detail

}  // namespace vlink

/**
 * @def VLINK_LAUNCHER_MODULE_BEGIN(VersionMajor, VersionMinor)
 * @brief Opens a @c LauncherModuleInterface factory registration block inside a plugin @c .cc file.
 *
 * @details
 * Must appear at most once per shared library at namespace or global scope; paired with
 * @c VLINK_LAUNCHER_MODULE_END() to emit the @c VLINK_PLUGIN_DECLARE entry points.
 *
 * @param VersionMajor  Exported interface ABI major version (must equal @c LauncherModuleInterface::kVersionMajor).
 * @param VersionMinor  Exported interface ABI minor version (must be @c >= @c LauncherModuleInterface::kVersionMinor).
 */
// NOLINTBEGIN
#define VLINK_LAUNCHER_MODULE_BEGIN(VersionMajor, VersionMinor)                                                   \
  class VLinkLauncherModuleImpl final : public vlink::detail::LauncherModuleBase {                                \
   public:                                                                                                        \
    static constexpr uint16_t kModuleMajor = (VersionMajor);                                                      \
    static constexpr uint16_t kModuleMinor = (VersionMinor);                                                      \
    static_assert(kModuleMajor == vlink::LauncherModuleInterface::kVersionMajor,                                  \
                  "Launcher module major ABI version must match vlink::LauncherModuleInterface::kVersionMajor."); \
    VLinkLauncherModuleImpl() { register_all(); }                                                                 \
                                                                                                                  \
   private:                                                                                                       \
    void register_all();                                                                                          \
  };                                                                                                              \
  inline void VLinkLauncherModuleImpl::register_all() {
// NOLINTEND

/**
 * @def VLINK_LAUNCHER_EXPORT_COMPONENT(ClassName, ComponentType)
 * @brief Registers @p ComponentType under @p ClassName inside a @c VLINK_LAUNCHER_MODULE_BEGIN block.
 *
 * @param ClassName      String literal class name referenced by TOML @c class or dynamic load requests.
 * @param ComponentType  Default-constructible concrete class deriving from @c vlink::LauncherComponent.
 */
// NOLINTBEGIN
#define VLINK_LAUNCHER_EXPORT_COMPONENT(ClassName, ComponentType) \
  factories_.emplace((ClassName), []() -> vlink::LauncherComponent* { return new ComponentType(); });
// NOLINTEND

/**
 * @def VLINK_LAUNCHER_MODULE_END()
 * @brief Closes the module registration block and exports the standard @c VLINK_PLUGIN_DECLARE ABI.
 */
// NOLINTBEGIN
#define VLINK_LAUNCHER_MODULE_END()                                                    \
  }                                                                                    \
  VLINK_PLUGIN_DECLARE(VLinkLauncherModuleImpl, VLinkLauncherModuleImpl::kModuleMajor, \
                       VLinkLauncherModuleImpl::kModuleMinor)
// NOLINTEND
