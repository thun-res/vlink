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
 * @file launcher_component.h
 * @brief Umbrella entry point for the header-only Launcher component plugin ABI.
 *
 * @details
 * Including @c <vlink/external/launcher_component.h> pulls in every header needed to write and
 * export dynamic in-process component plugins hosted by @c LauncherServer or isolated
 * @c [[container]] processes.  Callers that only need a subset (such as parameter types or the
 * host module ABI) may include the fine-grained headers under @c <vlink/external/launcher/>
 * directly to minimise compile-time dependencies.
 *
 * @par Pulled-in Headers
 *
 * | Header                            | Public Symbols                                  | Role | |
 * --------------------------------- | ----------------------------------------------- | ---------------------------- |
 * | @c launcher/param.h               | @c LauncherParamValue, @c LauncherParamMap      | Typed parameter tables | | @c
 * launcher/component.h           | @c LauncherComponent                            | Core lifecycle hooks         | |
 * @c launcher/module.h              | @c LauncherModuleInterface, export macros       | Plugin factory ABI & macros  |
 * | @c launcher/context.h             | @c LauncherContext, @c LauncherCoroutineOps     | Runtime execution context |
 * | @c launcher/port.h                | @c LauncherPublisher, @c LauncherSubscriber     | Lifecycle-gated port handles
 * | | @c launcher/timer_component.h     | @c LauncherTimerComponent                       | Periodic timer component |
 * | @c launcher/coroutine_component.h | @c LauncherCoroutineComponent                   | C++20 coroutine component |
 *
 * @par Component Lifecycle
 * @verbatim
 *   Plugin::load<LauncherModuleInterface>      (once per shared library)
 *        |
 *        v
 *   module->create_component(class_name)       (per configured instance)
 *        |
 *        v
 *   on_configure(ctx) ---> on_start(ctx) ---> on_activate(ctx) ---> [kReady / active]
 *        |                      ^                   ^                     |
 *        | init_state=          | first activate    | re-activate         | on_deactivate()
 *        | inactive             +---- [kInactive standby] <---------------+
 *        +-------------------------------^     |
 *                                              v
 *                                   (if started) on_stop() ---> on_cleanup()
 * @endverbatim
 *
 * @par Example
 * @code
 * #include <vlink/external/launcher_component.h>
 *
 * class EchoComponent final : public vlink::LauncherTimerComponent {
 *  public:
 *   EchoComponent() : vlink::LauncherTimerComponent(100U) {}
 *
 *  protected:
 *   bool on_timer_start(vlink::LauncherContext& ctx) override {
 *     pub_ = ctx.create_publisher<std::string>("out", "intra://demo/echo");
 *     return pub_.valid();
 *   }
 *
 *   bool on_tick(vlink::LauncherContext& ctx) override {
 *     (void)ctx;
 *     return pub_.publish("hello");
 *   }
 *
 *   void on_timer_stop() override { pub_.reset(); }
 *
 *  private:
 *   vlink::LauncherPublisher<std::string> pub_;
 * };
 *
 * VLINK_LAUNCHER_MODULE_BEGIN(1, 0)
 *   VLINK_LAUNCHER_EXPORT_COMPONENT("Echo", EchoComponent)
 * VLINK_LAUNCHER_MODULE_END()
 * @endcode
 *
 * @note
 * - Plugin libraries only need to include this header (or fine-grained @c launcher/ sub-headers)
 *   and link @c vlink::vlink; they never depend on @c tomlplusplus or @c vlink::launcher_server.
 * - @c LauncherCoroutineComponent and coroutine helpers on @c LauncherContext are available
 *   when @c VLINK_ENABLE_COROUTINE is defined.
 * - Legacy @c RunablePluginInterface plugins are also supported via @c legacy = true in TOML.
 *
 * @see launcher_server.h, launcher_api.h, base/plugin.h, extension/runnable_plugin_interface.h
 */

#pragma once

// NOLINTBEGIN

#include "./launcher/component.h"
#include "./launcher/context.h"
#include "./launcher/coroutine_component.h"
#include "./launcher/module.h"
#include "./launcher/param.h"
#include "./launcher/port.h"
#include "./launcher/timer_component.h"

// NOLINTEND
