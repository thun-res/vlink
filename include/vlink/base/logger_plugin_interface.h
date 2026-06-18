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
 * @file logger_plugin_interface.h
 * @brief Abstract contract for shared-library logger backends loaded via the VLink @c Plugin system.
 *
 * @details
 * A logger plugin is a shared library that exports a factory through @c VLINK_PLUGIN_DECLARE and
 * embeds @c VLINK_PLUGIN_REGISTER inside its concrete subclass.  At runtime the host application
 * uses @c vlink::Plugin::load<LoggerPluginInterface> to construct an instance and forwards every
 * log record to its @c log method.
 *
 * @par Plugin contract
 *
 * | Hook                     | Direction        | Contract                                         |
 * | ------------------------ | ---------------- | ------------------------------------------------ |
 * | @c VLINK_PLUGIN_REGISTER | inside the class | Wires factory and destructor pointers            |
 * | @c VLINK_PLUGIN_DECLARE  | translation unit | Exposes the factory and the version metadata     |
 * | @c init                  | host -> plugin   | Called once after construction with the app name |
 * | @c log                   | host -> plugin   | Called per record after passing level filters    |
 * | Destructor               | host -> plugin   | Called when the host releases the plugin handle  |
 *
 * @par Lifecycle
 *
 * @verbatim
 *  host                          plugin
 *  ----                          ------
 *  Plugin::load<...> ----------> factory creates instance
 *  init(app_name)    ---------->  initialise backend
 *  log(level, msg)   ---------->  forward to backend     (repeated)
 *  Plugin destroy    ---------->  destroy instance
 * @endverbatim
 *
 * @par Loading example
 * @code
 *   vlink::Plugin plugin;
 *   auto backend = plugin.load<vlink::LoggerPluginInterface>("my_logger_plugin", 1, 0);
 *
 *   if (backend) {
 *     backend->init("my_app");
 *     backend->log(vlink::Logger::kInfo, "Hello from plugin!");
 *   }
 * @endcode
 *
 * @par Implementation example
 * @code
 *   class MyLogger : public vlink::LoggerPluginInterface {
 *    public:
 *     bool init(std::string_view app_name) override { return true; }
 *     bool log(int level, std::string_view str)  override { return write_to_backend(level, str); }
 *   };
 *   VLINK_PLUGIN_DECLARE(MyLogger, 1, 0)
 * @endcode
 *
 * @note @p level mirrors @c vlink::Logger::Level values.  Both methods must avoid throwing
 *       across the plugin boundary; thrown exceptions there cause undefined behaviour.
 *
 * @see Plugin, Logger
 */

#pragma once

#include <string_view>

#include "./plugin.h"

namespace vlink {

/**
 * @class LoggerPluginInterface
 * @brief Pure-virtual interface that every shared-library logger backend implements.
 *
 * @details
 * Concrete subclasses use @c VLINK_PLUGIN_REGISTER internally to expose their factory and
 * destructor functions to the @c Plugin loader.  Instances are owned by the host application
 * for the duration of the plugin handle.
 */
class LoggerPluginInterface {
  VLINK_PLUGIN_REGISTER(LoggerPluginInterface)

 protected:
  LoggerPluginInterface() = default;

  virtual ~LoggerPluginInterface() = default;

 public:
  /**
   * @brief Initialises the backend immediately after the plugin instance is constructed.
   *
   * @param app_name  Calling application name; may inform log labels or file paths.
   * @return @c true on success; @c false to indicate the plugin must not be used further.
   */
  virtual bool init(std::string_view app_name) = 0;

  /**
   * @brief Writes a single record to the backend.
   *
   * @details
   * Implementations should be non-blocking; @p str remains valid only for the duration of the
   * call.
   *
   * @param level  Severity using @c vlink::Logger::Level values.
   * @param str    Fully formatted record.
   * @return @c true on success; @c false to indicate a write error.
   */
  virtual bool log(int level, std::string_view str) = 0;

 private:
  VLINK_DISALLOW_COPY_AND_ASSIGN(LoggerPluginInterface)
};

}  // namespace vlink
