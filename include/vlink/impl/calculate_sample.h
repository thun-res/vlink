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
 * @file calculate_sample.h
 * @brief Per-publisher gap detection used to expose sample loss statistics to subscribers.
 *
 * @details
 * This is an internal implementation header used by the public subscriber / getter
 * templates and their backend @c NodeImpl classes; it is not part of the user API.
 * @c CalculateSample inspects the monotonic sequence numbers attached to incoming
 * frames and accumulates how many samples have been expected and how many have
 * been skipped, on a per-sender basis.
 *
 * @par Role
 * | Caller                                   | What it does with @c CalculateSample           |
 * | ---------------------------------------- | ---------------------------------------------- |
 * | @c SubscriberImpl / @c GetterImpl        | Owns one instance when latency tracking is on. |
 * | Transport backend receive thread         | Calls @c update(seq, guid) for every frame.    |
 * | @c get_lost() / proxy / discovery layer  | Reads cumulative counters for reporting.       |
 *
 * @par Sizing summary
 * Counters are stored per GUID in a hash map, so memory grows linearly with the
 * number of observed senders.  Each entry contains three @c uint64_t values, and
 * the table is guarded by a @c std::shared_mutex so reads from multiple reader
 * threads do not contend with each other.
 *
 * @par Algorithm
 * - The first call for a GUID seeds @c first and @c expected with the observed
 *   sequence number and records zero loss.
 * - Subsequent in-order frames increment @c expected by one.
 * - When @c seq is ahead of @c expected by less than @c UINT32_MAX, the gap is
 *   added to @c lost and @c expected jumps to @c seq + 1.
 * - A gap of @c UINT32_MAX or larger is treated as a sender restart and the
 *   state is re-seeded with no extra loss recorded.
 *
 * @note @c get_total() returns the sum of @c (expected - first) across all
 *       GUIDs; @c get_lost() returns the sum of @c lost counters.
 */

#pragma once

#include <cstdint>
#include <shared_mutex>
#include <unordered_map>

#include "../base/macros.h"

namespace vlink {

/**
 * @class CalculateSample
 * @brief Tracks expected and lost sample counts grouped by sender GUID.
 *
 * @details
 * One instance is created per subscriber / getter when latency-and-loss tracking
 * is enabled.  The @c guid parameter of @c update() lets a single receiver
 * account for multiple senders without their streams interfering.
 */
class VLINK_EXPORT CalculateSample final {
 public:
  /**
   * @brief Constructs an empty counter.
   */
  CalculateSample() noexcept;

  /**
   * @brief Releases all per-sender state.
   */
  ~CalculateSample() noexcept;

  /**
   * @brief Records a sequence number received from @p guid.
   *
   * @details
   * Updates the per-sender counters according to the algorithm described at file
   * scope.  Acquires the exclusive side of the shared mutex.
   *
   * @param seq   Sequence number reported by the transport.
   * @param guid  Sender identifier; pass @c 0 when there is exactly one source.
   */
  void update(uint64_t seq, uint64_t guid = 0) noexcept;

  /**
   * @brief Returns the total number of expected samples across all senders.
   *
   * @details
   * Computed as the sum of @c (expected - first) over every tracked GUID.  The
   * value includes both received and lost samples.
   *
   * @return Number of expected samples; @c 0 when no data has been observed yet.
   */
  [[nodiscard]] uint64_t get_total() const noexcept;

  /**
   * @brief Returns the cumulative number of lost samples across all senders.
   *
   * @return Aggregate loss count since construction.
   */
  [[nodiscard]] uint64_t get_lost() const noexcept;

 private:
  struct Number final {
    uint64_t first{0};
    uint64_t expected{0};
    uint64_t lost{0};
  };

  std::unordered_map<uint64_t, Number> number_map_;
  mutable std::shared_mutex mtx_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(CalculateSample)
};

}  // namespace vlink
