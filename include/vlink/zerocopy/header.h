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
 * @file header.h
 * @brief Fixed-size timestamp and sequencing prefix embedded by every VLink zero-copy container.
 *
 * @details
 * @c Header is the foundation of the @c vlink::zerocopy family.  Every other
 * zero-copy container (@c RawData, @c CameraFrame, @c AudioFrame, @c PointCloud,
 * @c Tensor, @c OccupancyGrid, @c ObjectArray) embeds a @c Header as its first
 * public member so that consumers always know where to read the per-frame
 * @c seq, @c frame_id, and dual timestamps regardless of the payload type.
 *
 * | Concern         | Field                | Purpose                                          |
 * | --------------- | -------------------- | ------------------------------------------------ |
 * | Identity        | @c frame_id          | Source sensor / coordinate-frame label (16 char) |
 * | Sequencing      | @c seq               | Monotonically increasing per-publisher counter   |
 * | Acquisition     | @c time_meas         | Time the producer captured the data (ns)         |
 * | Publication     | @c time_pub          | Time the producer dispatched the message (ns)    |
 *
 * @par Wire format
 * The struct is POD; @c memcpy() over its 40 bytes is the canonical serialiser.
 * The @c sizeof() value is locked by @c static_assert and forms a permanent
 * contract: the @c vlink::zerocopy::* containers have no forward and no backward
 * binary compatibility.  Reserved bytes count as part of the contract too.
 * @code
 * static_assert(sizeof(Header) == 40, "Sizeof must be 40 bytes.");
 * @endcode
 *
 * @par Memory layout
 * @code
 * Offset  Size  Field
 * ------  ----  ------------------
 *      0    16  frame_id[16]
 *     16     4  seq
 *     20     4  reserved
 *     24     8  time_meas
 *     32     8  time_pub
 * ------  ----  ------------------
 *  Total    40  bytes (alignas 8)
 * @endcode
 *
 * @par Reserved bytes
 * The 4-byte @c reserved slot at offset 20 is part of the wire contract.  It
 * MUST NOT be repurposed by application code: reading it back after future
 * library upgrades would silently corrupt data.  Treat the byte as opaque.
 *
 * @par Example
 * @code
 * vlink::zerocopy::RawData raw;
 * std::strncpy(raw.header.frame_id, "lidar_top", sizeof(raw.header.frame_id) - 1);
 * raw.header.seq       = next_seq++;
 * raw.header.time_meas = sensor_ns;
 * raw.header.time_pub  = vlink::time_ns();
 *
 * auto label = raw.header.frame_id_view();
 * @endcode
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>

#include "../base/macros.h"
#include "../version.h"

namespace vlink {

namespace zerocopy {

/**
 * @brief Encoded VLink library version stamped into every zero-copy wire envelope.
 *
 * @details
 * The library version triple is packed as decimal @c major*1000000 + minor*1000 + patch
 * (each component in @c 0..100), e.g. version @c 2.10.8 encodes as the integer @c 2010008.
 * Producers write this @c uint32_t immediately after the begin-magic sentinel; consumers
 * reject any payload whose @b major component differs from the local @c kWireVersion (see
 * @c version_major()).  Minor and patch components are carried for diagnostics but are not
 * gated.
 */
inline constexpr uint32_t kWireVersion = (static_cast<uint32_t>(VLINK_VERSION_MAJOR) * 1000000U) +
                                         (static_cast<uint32_t>(VLINK_VERSION_MINOR) * 1000U) +
                                         static_cast<uint32_t>(VLINK_VERSION_PATCH);

/**
 * @brief Extracts the major component from an encoded zero-copy wire version.
 *
 * @param version Encoded version as produced by @c kWireVersion.
 * @return Major component, i.e. @c version / 1000000.
 */
inline constexpr uint32_t version_major(uint32_t version) noexcept { return version / 1000000U; }

/**
 * @struct Header
 * @brief 40-byte timestamp / sequencing metadata prefix shared by all zero-copy containers.
 *
 * @details
 * The struct is intentionally pure POD with 8-byte alignment so that it can be
 * @c memcpy'd as part of a larger container's wire snapshot.  The @c sizeof
 * value is locked at 40 bytes via @c static_assert; producers and consumers
 * on incompatible toolchains must keep this contract intact.
 */
struct VLINK_EXPORT_AND_ALIGNED(8) Header final {
  char frame_id[16]{"unknown"};  ///< Coordinate-frame / sensor identifier; not necessarily NUL-terminated.
  uint32_t seq{0};               ///< Monotonically increasing per-publisher sequence; wraps at UINT32_MAX.
  uint32_t reserved{0};          ///< Reserved bytes; part of the wire contract, must not be repurposed.
  uint64_t time_meas{0};         ///< Capture / acquisition timestamp in nanoseconds since the UNIX epoch.
  uint64_t time_pub{0};          ///< Dispatch / publish timestamp in nanoseconds since the UNIX epoch.

  /**
   * @brief Default constructor that asserts the 40-byte sizeof contract.
   *
   * @details
   * The @c static_assert inside the constructor body guarantees that any
   * accidental field reordering or padding change breaks the build instead of
   * silently corrupting the wire format.
   */
  Header() noexcept;

  /**
   * @brief Returns a bounded view over @c frame_id so consumers cannot read past the buffer.
   *
   * @details
   * @c frame_id may be filled to its full 16-byte capacity without a terminator,
   * so this helper uses @c strnlen to compute the view length safely.
   *
   * @return Non-owning view over the populated portion of @c frame_id.
   */
  [[nodiscard]] std::string_view frame_id_view() const noexcept {
    return {frame_id, ::strnlen(frame_id, sizeof(frame_id))};
  }
};

}  // namespace zerocopy

}  // namespace vlink
