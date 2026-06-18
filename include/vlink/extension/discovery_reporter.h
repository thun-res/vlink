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
 * @file discovery_reporter.h
 * @brief Process-wide discovery broadcaster that advertises live VLink endpoints.
 *
 * @details
 * @c DiscoveryReporter is the announce side of the VLink discovery subsystem.  It runs
 * on a private @c MessageLoop and emits periodic UDP multicast (default @c 239.255.0.100)
 * payloads that describe every @c NodeImpl currently registered in the process: their
 * URLs, communication kind, host name, PID, application name and an optional CPU usage
 * sample.  Listeners on the network -- typically @c DiscoveryViewer instances or the
 * @c vlink-cli tool -- consume those payloads to rebuild a live endpoint topology.
 *
 * Discovery flow:
 *
 * @verbatim
 *   NodeImpl::ctor  ----> DiscoveryReporter::add()
 *                              |
 *                              v
 *                        +-----------+   periodic timer    UDP multicast
 *                        |  loop     | ------------------> 239.255.0.100
 *                        +-----------+
 *                              ^                                  |
 *                              |                                  v
 *   NodeImpl::dtor  ----> remove()                          DiscoveryViewer
 * @endverbatim
 *
 * The singleton is owned by the VLink runtime: @c global_get() lazily creates the
 * reporter on first call and the process tears it down at exit.  @c add() and
 * @c remove() are invoked automatically by every @c NodeImpl constructor/destructor;
 * normal user code should rarely touch this class directly.
 *
 * @par Example
 * @code
 * // Disable discovery entirely:
 * //   export VLINK_DISCOVER_DISABLE=1
 *
 * // Tooling code path (e.g. vlink-cli): just consume the singleton.
 * if (auto* reporter = vlink::DiscoveryReporter::global_get(); reporter != nullptr) {
 *   reporter->async_run();   // started automatically in normal runtimes
 * }
 * @endcode
 *
 * @note Discovery rides on a dedicated UDP socket and is independent of any VLink
 * transport backend (intra/shm/dds/zenoh/...).  Set @c VLINK_DISCOVER_DISABLE=1 to opt
 * out for a given process.
 */

#pragma once

#include <memory>
#include <string>

#include "../base/macros.h"
#include "../base/message_loop.h"

namespace vlink {

class NodeImpl;

/**
 * @class DiscoveryReporter
 * @brief Periodic multicast broadcaster of the process's active VLink endpoints.
 *
 * @details
 * Lifecycle is normally managed by the VLink runtime.  Custom tooling may construct an
 * additional instance but most consumers rely on @c global_get() instead.
 */
class VLINK_EXPORT DiscoveryReporter : public MessageLoop {
 public:
  /**
   * @brief Returns the lazily-created process-global reporter.
   *
   * @details
   * Returns @c nullptr when the environment variable @c VLINK_DISCOVER_DISABLE is set to
   * @c "1", in which case discovery is suppressed for the whole process.
   *
   * @return Raw pointer to the global reporter, or @c nullptr when discovery is disabled.
   */
  static DiscoveryReporter* global_get();

  /**
   * @brief Builds the reporter, opens its UDP socket and arms the periodic timer.
   *
   * @details
   * The @c MessageLoop is left idle; callers are responsible for calling @c async_run()
   * (the global singleton does this automatically).
   */
  DiscoveryReporter();

  /**
   * @brief Stops the loop and releases the UDP socket.
   *
   * @details
   * An offline notification is only emitted when offline reporting is compiled in; the
   * default build omits it.
   */
  ~DiscoveryReporter() override;

  /**
   * @brief Registers a node so that subsequent broadcasts include it.
   *
   * @details
   * Invoked automatically by @c NodeImpl's constructor.
   *
   * @param node Node endpoint to track.
   */
  void add(NodeImpl* node);

  /**
   * @brief Unregisters a node so that subsequent broadcasts exclude it.
   *
   * @details
   * Invoked automatically by @c NodeImpl's destructor.
   *
   * @param node Node endpoint to drop.
   */
  void remove(NodeImpl* node);

 protected:
  size_t get_max_task_count() const override;

  uint32_t get_max_elapsed_time() const override;

  void on_begin() override;

  void on_end() override;

 private:
  void rebuild_message();

  void send_report();

  void send_offline();

  static const std::string& get_host_name();

  static const std::string& get_app_name();

  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(DiscoveryReporter)
};

}  // namespace vlink
