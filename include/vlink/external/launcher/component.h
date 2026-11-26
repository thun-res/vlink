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
 * @file component.h
 * @brief Base lifecycle interface implemented by in-process Launcher components.
 *
 * @details
 * Declares @c LauncherComponent with minimal compile-time dependencies (forward-declaring
 * @c LauncherContext) so plugin headers and host registries can reference component types
 * without including transport or execution-context details.
 *
 * @see context.h, module.h, timer_component.h, coroutine_component.h
 */

#pragma once

#include <string_view>

#include "../../base/macros.h"
#include "./param.h"

namespace vlink {

class LauncherContext;

/**
 * @class LauncherComponent
 * @brief Base class for in-process components hosted by @c LauncherServer or isolated containers.
 *
 * @details
 * Lifecycle hooks execute on the component's assigned scheduler @c MessageLoop unless noted
 * otherwise.  Components should keep constructors cheap and acquire live VLink primitives inside
 * @c on_configure() or @c on_start(), releasing them symmetrically in @c on_stop() or
 * @c on_cleanup().
 */
class LauncherComponent {
 public:
  /**
   * @brief Virtual destructor.
   */
  virtual ~LauncherComponent() = default;

  /**
   * @brief Configures the component from injected parameters prior to starting.
   *
   * @param ctx  Runtime context providing parameters, URL remap, and scheduler handles.
   * @return     @c true on success; @c false aborts startup of this component.
   */
  virtual bool on_configure(LauncherContext& ctx);

  /**
   * @brief Starts the component (creates publishers/subscribers/timers or spawns coroutines).
   *
   * @param ctx  Runtime context.
   * @return     @c true on success; @c false marks startup failed.
   */
  virtual bool on_start(LauncherContext& ctx) = 0;

  /**
   * @brief Stops active work, timers, and communication endpoints before teardown.
   */
  virtual void on_stop() = 0;

  /**
   * @brief Releases remaining resources after @c on_stop().
   */
  virtual void on_cleanup();

  /**
   * @brief Transitions a configured standby component into active operation.
   *
   * @details
   * Optional hook invoked after @c on_start() during normal launch, or upon @c ctl @c activate
   * when resuming from @c UnitState::kInactive standby (on the first activation from
   * @c init_state="inactive", the host invokes @c on_start() before @c on_activate()).  Default
   * implementation is a no-op returning @c true.
   *
   * @param ctx  Runtime context.
   * @return     @c true on success; @c false keeps the component inactive.
   */
  virtual bool on_activate(LauncherContext& ctx);

  /**
   * @brief Suspends active publishing and processing while retaining configured resources.
   *
   * @details
   * Optional hook invoked upon @c ctl @c deactivate (entering @c UnitState::kInactive) and
   * before @c on_stop() when tearing down an active component.  Default implementation is a
   * no-op.
   */
  virtual void on_deactivate();

  /**
   * @brief Invoked on a live @c reload command for in-place reconfiguration.
   *
   * @param ctx  Runtime context with updated parameters.
   * @return     @c true when reload succeeded.
   */
  virtual bool on_reload(LauncherContext& ctx);

  /**
   * @brief Invoked when a parameter is updated dynamically through the control plane.
   *
   * @param key    Dot-delimited parameter name.
   * @param value  Proposed new value.
   * @return       @c true to accept and commit the new value; @c false to reject the update.
   */
  virtual bool on_param_changed(std::string_view key, const LauncherParamValue& value);

 protected:
  LauncherComponent() = default;

 private:
  VLINK_DISALLOW_COPY_AND_ASSIGN(LauncherComponent)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline bool LauncherComponent::on_configure(LauncherContext& ctx) {
  (void)ctx;
  return true;
}

inline void LauncherComponent::on_cleanup() {}

inline bool LauncherComponent::on_activate(LauncherContext& ctx) {
  (void)ctx;
  return true;
}

inline void LauncherComponent::on_deactivate() {}

inline bool LauncherComponent::on_reload(LauncherContext& ctx) {
  (void)ctx;
  return true;
}

inline bool LauncherComponent::on_param_changed(std::string_view key, const LauncherParamValue& value) {
  (void)key;
  (void)value;
  return true;
}

}  // namespace vlink
