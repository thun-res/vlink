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
 * @file transport_plugin_interface.h
 * @brief Stable ABI implemented by transport plugins that ship a @c Transport factory.
 *
 * @details
 * This is an internal implementation header used by the URL routing layer and by
 * out-of-tree plugins for recognized transport backends; it is not part of the
 * public application API.  External plugins are shared libraries made available
 * according to the process-wide @c VLINK_TRANSPORT_PLUGINS value: an explicit transport
 * list is preloaded, while a complete value of @c auto (case-insensitive) enables
 * first-use loading of the fixed library for a recognized transport.  An empty
 * value or the case-insensitive value @c none disables plugin loading.  Each
 * plugin exports exactly one concrete subclass of @c TransportPluginInterface; the
 * runtime asks it for its existing @c TransportType and uses @c create() to
 * obtain a fresh @c Transport instance when a URL with that transport is constructed.
 * The complete setting is sampled once when the process-wide plugin manager is
 * initialized.  If @c VLINK_TRANSPORT_PLUGINS is unset or empty, @c VLINK_URL_PLUGINS supplies the value.
 *
 * @par Plugin contract
 * | Member                          | Required          | Description                                       |
 * | ------------------------------- | ----------------- | ------------------------------------------------- |
 * | @c VLINK_PLUGIN_REGISTER(iface) | Yes               | Tags the interface with a stable plugin id.       |
 * | @c VLINK_PLUGIN_DECLARE(...)    | Yes (in @c .cc)   | Exports the create / destroy plugin entry points. |
 * | @c get_transport_type() const   | Override          | Reports the @c TransportType the plugin handles.  |
 * | @c create() const               | Override          | Allocates a new @c Transport instance.            |
 *
 * @par Lifecycle
 * @code
 *   first Url/plugin use -> sample VLINK_TRANSPORT_PLUGINS
 *       |              |                 |
 *       | list         | auto            | empty / none
 *       v              v                 v
 *   explicit       first-use load      disabled
 *   preload        vlink-<transport>
 *       |              |
 *       +-----> validate type -> registry -> plugin->create()
 *                                              |
 *                                              v
 *                                       unique_ptr<Transport>
 * @endcode
 *
 * @par Loading constraints
 * In list mode, @c VLINK_TRANSPORT_PLUGINS accepts recognized transport names,
 * not arbitrary plugin names.  For example, @c zenoh maps to the fixed library
 * base name @c vlink-zenoh and to the existing @c TransportType::kZenoh.  The
 * on-demand path derives the same fixed name and is enabled only when the entire
 * sampled value equals @c auto, ignoring case.  Mode values cannot be combined
 * with an explicit list.  Unknown transport names are rejected before
 * @c Plugin::load() is called, linked transports take precedence over plugins,
 * and @c TransportType::kUnknown is never dispatched to this interface.  New URL
 * schemes therefore require core enum, URL mapping, and backend creation support
 * before this interface can be used.
 *
 * @note Implementations must remain stateless because @c create() may be invoked
 *       repeatedly to serve several independent @c Url instances.
 */

#pragma once

#include <memory>

#include "../base/plugin.h"
#include "./transport.h"

namespace vlink {

/**
 * @struct TransportPluginInterface
 * @brief Stateless factory contract that external recognized-transport plugins must implement.
 *
 * @details
 * Subclasses may be explicitly preloaded from a transport list in
 * @c VLINK_TRANSPORT_PLUGINS or loaded on first use of a recognized, unlinked transport
 * when its complete value is @c auto (case-insensitive).  The interface cannot
 * register a new @c TransportType or URL scheme; it only supplies @c Transport
 * instances for existing transport identifiers.  It intentionally exposes only
 * the two queries needed by @c Url::load_for_plugin(); plugin-specific state
 * lives inside the @c Transport instances returned by @c create().
 */
struct TransportPluginInterface {
  VLINK_PLUGIN_REGISTER(TransportPluginInterface)

 protected:
  TransportPluginInterface() = default;

  virtual ~TransportPluginInterface() = default;

 public:
  /**
   * @brief Returns the backend identifier of instances created by this plugin.
   *
   * @details
   * Called by @c Url::load_for_plugin() to match URL transports to loaded
   * plugins.  The same identifier may be returned by at most one plugin.
   *
   * @return @c TransportType value covered by this plugin.
   */
  [[nodiscard]] virtual TransportType get_transport_type() const = 0;

  /**
   * @brief Allocates a fresh @c Transport instance.
   *
   * @details
   * Invoked once per @c Url constructor whose transport matches the plugin.
   * The returned object must be ready to receive @c parse() calls immediately.
   *
   * @return Heap-allocated @c Transport owned by the caller.
   */
  [[nodiscard]] virtual std::unique_ptr<Transport> create() const = 0;

 private:
  VLINK_DISALLOW_COPY_AND_ASSIGN(TransportPluginInterface)
};

}  // namespace vlink
