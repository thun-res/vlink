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
 * @file shm_conf.h
 * @brief Transport configuration for the @c shm:// Iceoryx shared-memory transport.
 *
 * @details
 * @c ShmConf binds the @c shm:// URL scheme to the Eclipse Iceoryx zero-copy
 * shared-memory transport.  Publishers @c loan() a memory chunk directly from the
 * RouDi-managed memory pool, fill it in place, and @c publish() it; subscribers
 * receive the chunk by pointer and release it back to the pool when finished.  No
 * payload bytes are ever copied between processes, which makes this the
 * lowest-latency option for large messages on a single host.
 *
 * @par Shared-Memory Layout
 * @code
 *   RouDi (daemon)
 *     +-----------------------------------------------+
 *     |  Mempool: fixed-size chunk segments           |
 *     |  +------+ +------+ +------+ +------+ +------+ |
 *     |  | chk0 | | chk1 | | chk2 | | chk3 | | ...  | |
 *     |  +------+ +------+ +------+ +------+ +------+ |
 *     +--------^--------------------------^-----------+
 *              | loan()                   | release()
 *              |                          |
 *         Publisher process         Subscriber process
 * @endcode
 *
 * @par Loan / Release Flow
 * @code
 *   Publisher: chunk = loan(size); fill(chunk); publish(chunk);
 *                                            \
 *                                             \--> queue (per topic in RouDi)
 *                                                       \
 *   Subscriber: chunk = take();                          \--> chunk arrival
 *               consume(chunk); release(chunk);
 * @endcode
 *
 * @par Supported Node Types
 *
 * | Publisher | Subscriber | Server | Client | Getter | Setter |
 * | :-------: | :--------: | :----: | :----: | :----: | :----: |
 * | yes       | yes        | yes    | yes    | yes    | yes    |
 *
 * @par URL Format
 * @code
 *   shm://<address>[?event=<name>&domain=<N>&depth=<N>&history=<N>&wait=<ms>]
 * @endcode
 *
 * | Component  | Description                                                            |
 * | ---------- | ---------------------------------------------------------------------- |
 * | @c address | Iceoryx service/topic name (URL host concatenated with path)           |
 * | @c event   | Optional secondary event name (@c ?event=)                             |
 * | @c domain  | Iceoryx domain ID (@c ?domain=); defaults to @c 0                      |
 * | @c depth   | Queue capacity override; @c 0 uses the Iceoryx default                 |
 * | @c history | History count (@c ?history=); @c 0 from URL, or @c 1 for field nodes   |
 * | @c wait    | Blocking-wait timeout in ms for pub/sub; not valid for RPC or fields   |
 *
 * @par RouDi Lifecycle
 * RouDi is the Iceoryx memory-manager daemon.  It can either be started externally
 * (preferred for production) or hosted in-process for tests and single-process tools.
 * @code
 *   // Option A: embedded RouDi (single-process tools):
 *   vlink::ShmConf::init_roudi();
 *
 *   // Option B: connect this process to an external RouDi daemon:
 *   vlink::ShmConf::init_runtime("my_app");
 *
 *   // Option C: one-line preflight that picks whichever is needed:
 *   vlink::ShmConf::auto_init_roudi();
 *
 *   // ... create shm:// nodes ...
 *
 *   vlink::ShmConf::deinit_runtime();
 * @endcode
 *
 * @note Compiled only when @c VLINK_SUPPORT_SHM is defined.
 * @note Iceoryx caps @c address and @c event at 80 characters each.
 * @note @c wait mode is only valid for @c kPublisher / @c kSubscriber; using it
 *       with RPC or field nodes causes @c parse_protocol() to return @c false.
 */

#pragma once

#ifdef VLINK_SUPPORT_SHM

#include <cstdint>
#include <string>

#include "../impl/conf.h"

namespace vlink {

/**
 * @struct ShmConf
 * @brief Concrete @c Conf describing an Iceoryx shared-memory endpoint addressed by a @c shm:// URL.
 *
 * @details
 * Captures the service/topic address, optional event filter, Iceoryx domain ID,
 * queue capacity override, history count, and blocking-wait timeout.  Both
 * @c address and @c event are limited to 80 characters by Iceoryx naming
 * constraints.
 */
struct VLINK_EXPORT ShmConf final : public Conf {
  std::string address;  ///< Iceoryx service/topic address (URL host plus path); maximum 80 characters.
  std::string event;    ///< Optional secondary event name; maximum 80 characters.
  int32_t domain{0};    ///< Iceoryx domain identifier (non-negative).
  int32_t depth{0};     ///< Queue capacity override; @c 0 keeps the Iceoryx default.
  int32_t history{0};   ///< History count; URL parsing defaults to @c 0, or @c 1 for setter / getter nodes.
  int32_t wait{0};      ///< Blocking-wait timeout in milliseconds; positive values enable pub/sub wait mode.

  /**
   * @brief Builds a @c ShmConf from its six logical fields.
   *
   * @param _address  Service/topic address string; maximum 80 characters.
   * @param _event    Optional event name; maximum 80 characters; empty by default.
   * @param _domain   Iceoryx domain identifier; defaults to @c 0.
   * @param _depth    Queue capacity override; defaults to @c 0.
   * @param _history  History count; defaults to @c 0.
   * @param _wait     Blocking-wait timeout in milliseconds; defaults to @c 0 (disabled).
   */
  explicit ShmConf(const std::string& _address, const std::string& _event = "", int32_t _domain = 0, int32_t _depth = 0,
                   int32_t _history = 0, int32_t _wait = 0);

  /**
   * @brief Component-wise equality on all configuration fields.
   *
   * @param conf  Configuration to compare with.
   * @return      @c true when every field of @c *this matches @p conf.
   */
  [[nodiscard]] bool operator==(const ShmConf& conf) const noexcept;

  /**
   * @brief Logical negation of @c operator==.
   *
   * @param conf  Configuration to compare with.
   * @return      @c true when any field differs from @p conf.
   */
  [[nodiscard]] bool operator!=(const ShmConf& conf) const noexcept;

  /**
   * @brief Reports this object's transport tag.
   *
   * @return @c TransportType::kShm.
   */
  [[nodiscard]] TransportType get_transport_type() const override;

  /**
   * @brief Reports whether an in-process RouDi was started in this process.
   *
   * @details
   * Delegates to @c ShmFactory::has_roudi_inited(); meaningful only after a call
   * to @c init_roudi() inside the same process.
   *
   * @return @c true when an embedded RouDi instance is running here.
   */
  [[nodiscard]] static bool has_roudi_inited();

  /**
   * @brief Reports whether the Iceoryx runtime has been initialised for this process.
   *
   * @details
   * Delegates to @c ShmFactory::has_runtime_inited(); returns @c true after a
   * successful @c init_runtime() call or after @c global_init() automatically
   * initialises the runtime.
   *
   * @return @c true when the Iceoryx runtime is ready to create endpoints.
   */
  [[nodiscard]] static bool has_runtime_inited();

  /**
   * @brief Cheap, non-invasive probe for a reachable Iceoryx RouDi daemon.
   *
   * @details
   * Intended for CLI diagnostics, bench preflight, and pre-launch hints.  Does
   * not create an Iceoryx runtime, does not open any port, does not mutate any
   * global state, and is safe to call repeatedly from any thread.
   *
   * @return @c true when RouDi (or a process embedding RouDi such as
   *         @c vlink-proxy) is running on the local host, otherwise @c false.
   *
   * @see ShmFactory::has_roudi_running()
   */
  [[nodiscard]] static bool has_roudi_running();

  /**
   * @brief One-line preflight that guarantees a RouDi is available for @c shm:// nodes.
   *
   * @details
   * On POSIX, an in-process RouDi is started automatically when none is detected.
   * On Windows, when no external RouDi is detected, returns @c false only if
   * @p same_process_from_roudi is @c true; otherwise the runtime is initialised
   * without starting RouDi.  Cached across calls.
   *
   * @note Calls @c init_runtime() during preflight and tears it down when the
   *       cached manager is destroyed.
   *
   * @param same_process_from_roudi  @c true when RouDi runs in this same process
   *                                 (see @c init_roudi()).
   * @return @c true when RouDi is ready for use, @c false otherwise.
   *
   * @see ShmFactory::auto_init_roudi()
   */
  [[nodiscard]] static bool auto_init_roudi(bool same_process_from_roudi = false);

  /**
   * @brief Starts an embedded Iceoryx RouDi daemon inside the current process.
   *
   * @details
   * Use this when no external RouDi process is running and a single process must
   * act as both the memory manager and a participant.  Must be invoked before any
   * @c shm:// endpoint is created.
   *
   * @param config_path        Path to a RouDi XML configuration file; empty selects defaults.
   * @param memory_strategy    Memory pooling strategy; @c 0 selects the default strategy.
   * @param monitoring_enable  @c true to enable process-monitoring inside RouDi.
   */
  static void init_roudi(const std::string& config_path = "", int memory_strategy = 0, bool monitoring_enable = true);

  /**
   * @brief Registers this process with the Iceoryx RouDi daemon.
   *
   * @details
   * Must be called once before any @c shm:// endpoint is created when connecting
   * to an external RouDi.  The @p name should be unique across all processes
   * connecting to the same RouDi instance; Iceoryx truncates it to the runtime
   * name capacity.
   *
   * @param name                     Process registration name; empty selects a generated name.
   * @param same_process_from_roudi  @c true when RouDi runs in this same process
   *                                 (see @c init_roudi()).
   */
  static void init_runtime(const std::string& name = "", bool same_process_from_roudi = false);

  /**
   * @brief Unregisters this process from the Iceoryx RouDi daemon.
   *
   * @details
   * Releases all Iceoryx resources associated with the current process.  Must be
   * called before process exit when @c init_runtime() has been used.
   */
  static void deinit_runtime();

#ifndef VLINK_ENABLE_C_INTERFACE
  VLINK_DECLARE_GLOBAL_PROPERTY()
#endif
  VLINK_ALLOW_IMPL_TYPE(kServer | kClient | kPublisher | kSubscriber | kSetter | kGetter)
  VLINK_CONF_IMPL(ShmConf)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline ShmConf::ShmConf(const std::string& _address, const std::string& _event, int32_t _domain, int32_t _depth,
                        int32_t _history, int32_t _wait)
    : address(_address), event(_event), domain(_domain), depth(_depth), history(_history), wait(_wait) {}

inline bool ShmConf::operator==(const ShmConf& conf) const noexcept {
  return address == conf.address && event == conf.event && domain == conf.domain && depth == conf.depth &&
         history == conf.history && wait == conf.wait;
}

inline bool ShmConf::operator!=(const ShmConf& conf) const noexcept { return !(*this == conf); }

inline TransportType ShmConf::get_transport_type() const { return TransportType::kShm; }

}  // namespace vlink

#endif
