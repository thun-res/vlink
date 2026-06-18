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
 * @file runnable_plugin_interface.h
 * @brief Plugin contract for self-driving plugins that own a private @c MessageLoop thread.
 *
 * @details
 * @c RunablePluginInterface (the spelling is intentional and preserved for backwards
 * compatibility) blends @c MessageLoop with the VLink @c Plugin framework so a dynamic
 * plugin can carry its own event loop and dispatch timers, subscribers and other
 * asynchronous primitives without depending on the host's loop.
 *
 * Plugin contract:
 *
 * | Hook              | When the host calls it                  | Mandatory action                                    |
 * | ----------------- | --------------------------------------- | --------------------------------------------------- |
 * | constructor       | At @c Plugin::load time                 | Cheap construction; no thread-local work yet        |
 * | @c async_run()    | After load, on the host's thread        | Inherited from @c MessageLoop; starts the loop      |
 * | @c on_init()      | Right after @c async_run() succeeds     | Set up subscribers, timers and other live resources |
 * | @c on_deinit()    | Just before unload                      | Tear down everything created in @c on_init()        |
 * | destructor        | When the @c Plugin handle is released   | Final cleanup; loop has already been stopped        |
 *
 * Plugin lifecycle:
 *
 * @verbatim
 *   Plugin::load(...)
 *        |
 *        v
 *   constructor  -->  async_run()  -->  on_init()
 *                                          |
 *                                          v
 *                                     plugin work (loop running)
 *                                          |
 *                                          v
 *                                       on_deinit()
 *                                          |
 *                                          v
 *                                      stop loop / unload
 * @endverbatim
 *
 * @par Example
 * @code
 * // Inside the plugin shared library:
 * class MyPlugin : public vlink::RunablePluginInterface {
 *  public:
 *   void on_init()   override { ... }   // create subscribers / timers
 *   void on_deinit() override { ... }   // release everything created above
 * };
 * VLINK_PLUGIN_DECLARE(MyPlugin, 1, 0)
 *
 * // Inside the host process:
 * vlink::Plugin plugin;
 * auto instance = plugin.load<vlink::RunablePluginInterface>("my_plugin.so", 1, 0);
 * instance->async_run();
 * instance->on_init();
 * // ... let it run ...
 * instance->on_deinit();
 * @endcode
 */

#pragma once

#include "../base/message_loop.h"
#include "../base/plugin.h"

namespace vlink {

/**
 * @class RunablePluginInterface
 * @brief Abstract plugin base that already owns a @c MessageLoop event thread.
 *
 * @details
 * The plugin inherits both the @c Plugin registration machinery and a @c MessageLoop,
 * so it can post tasks, run timers and consume VLink subscriptions on its own thread.
 * Lifecycle is split between @c on_init() (called once after the loop starts) and
 * @c on_deinit() (called once before the loop is torn down).
 */
class RunablePluginInterface : public MessageLoop {
  VLINK_PLUGIN_REGISTER(RunablePluginInterface)

 protected:
  RunablePluginInterface() = default;

  ~RunablePluginInterface() override = default;

 public:
  /**
   * @brief Called by the host once the plugin's @c MessageLoop is running.
   *
   * @details
   * Override to create subscribers, timers and other primitives that require the loop
   * to be alive.  Runs on the caller's thread.
   */
  virtual void on_init() = 0;

  /**
   * @brief Called by the host just before the plugin is unloaded.
   *
   * @details
   * Override to release every resource created in @c on_init().  After this call the
   * host stops the loop and detaches the shared library.
   */
  virtual void on_deinit() = 0;

 private:
  VLINK_DISALLOW_COPY_AND_ASSIGN(RunablePluginInterface)
};

}  // namespace vlink
