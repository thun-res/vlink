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
 * @file conf_plugin_interface.h
 * @brief Stable ABI implemented by transport plugins that ship a @c Conf factory.
 *
 * @details
 * This is an internal implementation header used by the URL routing layer and by
 * third-party transport plugins; it is not part of the public application API.
 * External plugins are shared libraries discovered at runtime through either the
 * @c VLINK_URL_PLUGINS environment variable or an explicit @c Url::init_plugins()
 * call.  Each plugin exports exactly one concrete subclass of
 * @c ConfPluginInterface; the runtime asks it for its @c TransportType and uses
 * @c create() to obtain a fresh @c Conf instance when a URL with that transport
 * is constructed.
 *
 * @par Plugin contract
 * | Member                          | Required          | Description                                       |
 * | ------------------------------- | ----------------- | ------------------------------------------------- |
 * | @c VLINK_PLUGIN_REGISTER(iface) | Yes               | Tags the interface with a stable plugin id.       |
 * | @c VLINK_PLUGIN_DECLARE(...)    | Yes (in @c .cc)   | Exports the create / destroy plugin entry points. |
 * | @c get_transport_type() const   | Override          | Reports the @c TransportType the plugin handles.  |
 * | @c create() const               | Override          | Allocates a new transport @c Conf instance.       |
 *
 * @par Lifecycle
 * @code
 *     +-----------+          +--------------+          +-----------------+
 *     | Url::ctor | -------> | init_plugins | -------> | dlopen library  |
 *     +-----------+          +------+-------+          +--------+--------+
 *                                   |                           |
 *                                   |                           v
 *                                   |               +-----------------------+
 *                                   |               | Plugin::create_object |
 *                                   |               +-----------+-----------+
 *                                   |                           |
 *                                   v                           v
 *                          +----------------+          +------------------+
 *                          | load_for_plugin| -------> | plugin->create() |
 *                          +----------------+          +--------+---------+
 *                                                               |
 *                                                               v
 *                                                        +-------------+
 *                                                        | unique<Conf>|
 *                                                        +-------------+
 * @endcode
 *
 * @par Example
 * @code
 * struct MyTransportPlugin final : public vlink::ConfPluginInterface {
 *   VLINK_PLUGIN_REGISTER(ConfPluginInterface)
 *
 *   vlink::TransportType get_transport_type() const override {
 *     return vlink::TransportType::kMyCustomTransport;
 *   }
 *
 *   std::unique_ptr<vlink::Conf> create() const override {
 *     return std::make_unique<MyTransportConf>();
 *   }
 * };
 * VLINK_PLUGIN_DECLARE(MyTransportPlugin, 1, 0)
 * @endcode
 *
 * @note Implementations must remain stateless because @c create() may be invoked
 *       repeatedly to serve several independent @c Url instances.
 */

#pragma once

#include <memory>

#include "../base/plugin.h"
#include "./conf.h"

namespace vlink {

/**
 * @struct ConfPluginInterface
 * @brief Stateless factory contract that external transport plugins must implement.
 *
 * @details
 * Subclasses are loaded from shared libraries by the VLink runtime when a URL
 * uses a transport that is not built in.  The interface intentionally exposes
 * only the two queries needed by @c Url::load_for_plugin(); plugin-specific
 * state lives inside the @c Conf instances returned by @c create().
 */
struct ConfPluginInterface {
  VLINK_PLUGIN_REGISTER(ConfPluginInterface)

 protected:
  ConfPluginInterface() = default;

  virtual ~ConfPluginInterface() = default;

 public:
  /**
   * @brief Reports the transport identifier this plugin can produce confs for.
   *
   * @details
   * Called by @c Url::load_for_plugin() to match URL transports to loaded
   * plugins.  The same identifier may be returned by at most one plugin.
   *
   * @return @c TransportType value covered by this plugin.
   */
  [[nodiscard]] virtual TransportType get_transport_type() const = 0;

  /**
   * @brief Allocates a fresh transport @c Conf instance.
   *
   * @details
   * Invoked once per @c Url constructor whose transport matches the plugin.
   * The returned object must be ready to receive @c parse() calls immediately.
   *
   * @return Heap-allocated transport @c Conf owned by the caller.
   */
  [[nodiscard]] virtual std::unique_ptr<Conf> create() const = 0;

 private:
  VLINK_DISALLOW_COPY_AND_ASSIGN(ConfPluginInterface)
};

}  // namespace vlink
