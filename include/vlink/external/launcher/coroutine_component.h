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
 * @file coroutine_component.h
 * @brief C++20 coroutine component base class built on @c LauncherComponent and @c vlink::Co.
 *
 * @details
 * Defines @c LauncherCoroutineComponent when @c VLINK_ENABLE_COROUTINE is enabled, spawning
 * @c on_coro_run() onto the component's assigned @c MessageLoop with host unload guarding.
 *
 * @see component.h, context.h, launcher_component.h
 */

#pragma once

#include "../../base/coroutine.h"
#include "./component.h"
#include "./context.h"

namespace vlink {

#ifdef VLINK_ENABLE_COROUTINE
/**
 * @class LauncherCoroutineComponent
 * @brief Declarative C++20 coroutine component base class driven by @c vlink::Co.
 *
 * @details
 * Spawns @c on_coro_run(ctx) onto the component's assigned @c MessageLoop with an automatic
 * unload guard.  When the host stops or unloads the component, the cancellation token fires
 * immediately (waking any @c ctx.co_sleep() or @c ctx.co_await_stop()) and host teardown waits
 * for the coroutine frame to unwind before invoking @c destroy_component().
 */
class LauncherCoroutineComponent : public LauncherComponent {
 public:
  /**
   * @brief Virtual destructor.
   */
  ~LauncherCoroutineComponent() override = default;

  /**
   * @brief Invokes @c on_coro_start() and spawns @c on_coro_run() onto @c ctx.loop().
   *
   * @param ctx  Component execution context.
   * @return     @c true when setup succeeded and the coroutine was spawned.
   */
  bool on_start(LauncherContext& ctx) override;

  /**
   * @brief Delegates shutdown cleanup to @c on_coro_stop().
   */
  void on_stop() override;

 protected:
  LauncherCoroutineComponent() = default;

  /**
   * @brief Optional synchronous setup hook invoked during @c on_start() before spawning @c on_coro_run().
   *
   * @param ctx  Component execution context.
   * @return     @c true on success.
   */
  virtual bool on_coro_start(LauncherContext& ctx);

  /**
   * @brief Main coroutine body executed on the component's @c MessageLoop.
   *
   * @param ctx  Runtime context providing @c co_sleep(), @c co_await_stop(), and @c co_exec().
   * @return     Coroutine task.
   */
  virtual Co::Task<void> on_coro_run(LauncherContext& ctx) = 0;

  /**
   * @brief Optional stop hook invoked during @c on_stop() after cancellation has been requested.
   */
  virtual void on_coro_stop();
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline bool LauncherCoroutineComponent::on_start(LauncherContext& ctx) {
  if (!on_coro_start(ctx)) {
    return false;
  }

  if VUNLIKELY (!ctx.co_spawn(on_coro_run(ctx))) {
    on_coro_stop();
    return false;
  }

  return true;
}

inline void LauncherCoroutineComponent::on_stop() { on_coro_stop(); }

inline bool LauncherCoroutineComponent::on_coro_start(LauncherContext& ctx) {
  (void)ctx;
  return true;
}

inline void LauncherCoroutineComponent::on_coro_stop() {}
#endif

}  // namespace vlink
