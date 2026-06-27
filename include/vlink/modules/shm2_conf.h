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
 * @file shm2_conf.h
 * @brief Transport configuration for the @c shm2:// Iceoryx2 (next-generation) shared-memory transport.
 *
 * @details
 * @c Shm2Conf binds the @c shm2:// URL scheme to the Iceoryx2 shared-memory
 * transport, the next-generation successor to the @c shm:// Iceoryx backend.
 * Iceoryx2 keeps the zero-copy semantics of its predecessor but eliminates the
 * standalone RouDi daemon: every participating process maps the shared-memory
 * service directly.  This simplifies deployment in containerised and read-only
 * root-filesystem environments while preserving the same loan / release flow.
 *
 * @par Shared-Memory Layout
 * @code
 *   Iceoryx2 service (file-backed POSIX SHM segment)
 *     +-----------------------------------------------+
 *     |  Per-publisher slot pool of fixed-size chunks |
 *     |  +------+ +------+ +------+ +------+ +------+ |
 *     |  | slot | | slot | | slot | | slot | | ...  | |
 *     |  +------+ +------+ +------+ +------+ +------+ |
 *     +--------^--------------------------^-----------+
 *              | loan()                   | release()
 *              |                          |
 *         Publisher process         Subscriber process
 * @endcode
 *
 * @par Loan / Release Flow
 * @code
 *   Publisher: slot = loan(size); fill(slot); publish(slot);
 *                                            \
 *                                             \--> per-subscriber notification queue
 *                                                       \
 *   Subscriber: slot = take();                          \--> slot arrival
 *               consume(slot); release(slot);
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
 *   shm2://<address>[?event=<name>&domain=<N>&depth=<N>&history=<N>&wait=<ms>][#<size>]
 * @endcode
 *
 * | Component  | Description                                                            |
 * | ---------- | ---------------------------------------------------------------------- |
 * | @c address | Iceoryx2 service/topic name (URL host concatenated with path)          |
 * | @c event   | Optional secondary event name (@c ?event=)                             |
 * | @c domain  | Domain ID (@c ?domain=); defaults to @c 0                              |
 * | @c depth   | Queue / loan capacity override; @c 0 uses the Iceoryx2 default         |
 * | @c history | History count; URL default @c 0, or @c 1 for field nodes               |
 * | @c wait    | Blocking-wait timeout in ms; pub/sub only                              |
 * | @c size    | Per-message memory size from URL fragment (see size syntax below)      |
 *
 * @par Size Fragment Syntax
 * The URL fragment selects the per-message shared-memory chunk size.  Supported
 * suffixes are case-insensitive: @c B, @c K/@c KB, @c M/@c MB, @c G/@c GB.  The
 * value must fall in @c (0, @c kMaxMemSize].
 * @code
 *   shm2://my_topic#1M     // 1 MiB per message
 *   shm2://my_topic#512K   // 512 KiB per message
 *   shm2://my_topic        // default: 128 bytes per message
 * @endcode
 *
 * @par Backend-Specific Options
 *
 * | Option              | Purpose                                            | Default         |
 * | ------------------- | -------------------------------------------------- | --------------- |
 * | @c size             | Per-message chunk size                             | @c 128 B        |
 * | @c kMaxMemSize      | Hard upper bound on @c size                        | @c 32 MiB       |
 * | @c depth            | Slot pool capacity                                 | backend default |
 * | @c history          | Late-joining replay count                          | @c 0 / @c 1     |
 *
 * @note Compiled only when @c VLINK_SUPPORT_SHM2 is defined.
 * @note @c address and @c event must each be at most 80 characters long.
 * @note @c wait mode is only valid for @c kPublisher / @c kSubscriber endpoints;
 *       using it with RPC or field nodes causes @c parse_protocol() to return @c false.
 * @note @c is_valid() additionally requires @c size in @c (0, @c kMaxMemSize] and
 *       @c domain, @c depth, @c history to be non-negative.
 */

#pragma once

#ifdef VLINK_SUPPORT_SHM2

#include <cstdint>
#include <string>

#include "../impl/conf.h"

namespace vlink {

/**
 * @struct Shm2Conf
 * @brief Concrete @c Conf describing an Iceoryx2 endpoint addressed by an @c shm2:// URL.
 *
 * @details
 * Extends the @c ShmConf field set with a @c size parameter that controls the
 * per-message shared-memory chunk size negotiated with the Iceoryx2 service.
 */
struct VLINK_EXPORT Shm2Conf final : public Conf {
  std::string address;             ///< Iceoryx2 service/topic address (URL host plus path); maximum 80 characters.
  std::string event;               ///< Optional secondary event name; maximum 80 characters.
  int32_t domain{0};               ///< Domain identifier (non-negative).
  int32_t depth{0};                ///< Queue / loan capacity override; @c 0 keeps the Iceoryx2 default.
  int32_t history{0};              ///< History count; URL parsing defaults to @c 0, or @c 1 for setter / getter nodes.
  int32_t wait{0};                 ///< Blocking-wait timeout in milliseconds; positive values enable pub/sub wait mode.
  uint64_t size{kDefaultMemSize};  ///< Per-message shared-memory chunk size in bytes.

  /**
   * @brief Builds a @c Shm2Conf from its seven logical fields.
   *
   * @param _address  Service/topic address string; maximum 80 characters.
   * @param _event    Optional event name; maximum 80 characters; empty by default.
   * @param _domain   Domain identifier; defaults to @c 0.
   * @param _depth    Queue / loan capacity override; defaults to @c 0.
   * @param _history  History count; defaults to @c 0.
   * @param _wait     Blocking-wait timeout in ms; defaults to @c 0 (disabled).
   * @param _size     Per-message chunk size in bytes; defaults to @c kDefaultMemSize (128 B).
   */
  explicit Shm2Conf(const std::string& _address, const std::string& _event = "", int32_t _domain = 0,
                    int32_t _depth = 0, int32_t _history = 0, int32_t _wait = 0, uint64_t _size = kDefaultMemSize);

  /**
   * @brief Component-wise equality on all configuration fields.
   *
   * @param conf  Configuration to compare with.
   * @return      @c true when all seven fields match.
   */
  [[nodiscard]] bool operator==(const Shm2Conf& conf) const noexcept;

  /**
   * @brief Logical negation of @c operator==.
   *
   * @param conf  Configuration to compare with.
   * @return      @c true when any field differs from @p conf.
   */
  [[nodiscard]] bool operator!=(const Shm2Conf& conf) const noexcept;

  /**
   * @brief Reports this object's transport tag.
   *
   * @return @c TransportType::kShm2.
   */
  [[nodiscard]] TransportType get_transport_type() const override;

  static constexpr size_t kDefaultMemSize = 128U;              ///< Default per-message chunk size: 128 bytes.
  static constexpr size_t kMaxMemSize = 1024UL * 1024UL * 32;  ///< Upper bound on per-message chunk size: 32 MiB.

#ifndef VLINK_ENABLE_C_INTERFACE
  VLINK_DECLARE_GLOBAL_PROPERTY()
#endif
  VLINK_ALLOW_IMPL_TYPE(kServer | kClient | kPublisher | kSubscriber | kSetter | kGetter)
  VLINK_CONF_IMPL(Shm2Conf)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline Shm2Conf::Shm2Conf(const std::string& _address, const std::string& _event, int32_t _domain, int32_t _depth,
                          int32_t _history, int32_t _wait, uint64_t _size)
    : address(_address), event(_event), domain(_domain), depth(_depth), history(_history), wait(_wait), size(_size) {}

inline bool Shm2Conf::operator==(const Shm2Conf& conf) const noexcept {
  return address == conf.address && event == conf.event && domain == conf.domain && depth == conf.depth &&
         history == conf.history && wait == conf.wait && size == conf.size;
}

inline bool Shm2Conf::operator!=(const Shm2Conf& conf) const noexcept { return !(*this == conf); }

inline TransportType Shm2Conf::get_transport_type() const { return TransportType::kShm2; }

}  // namespace vlink

#endif
