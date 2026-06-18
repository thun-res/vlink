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

#include "./impl/calculate_sample.h"

#include <limits>
#include <mutex>

namespace vlink {

// CalculateSample
CalculateSample::CalculateSample() noexcept = default;

CalculateSample::~CalculateSample() noexcept = default;

void CalculateSample::update(uint64_t seq, uint64_t guid) noexcept {
  static constexpr int64_t kMaxLossDiff = std::numeric_limits<uint32_t>::max();

  std::lock_guard lock(mtx_);

  auto& number = number_map_[guid];

  auto lost = static_cast<int64_t>(seq - number.expected);

  if VUNLIKELY (number.expected == 0 || seq < number.expected || lost > kMaxLossDiff || seq == 0) {
    number.first = seq;
    number.expected = seq + 1;
    number.lost = 0;

    return;
  }

  number.lost += lost;
  number.expected = seq + 1;
}

uint64_t CalculateSample::get_total() const noexcept {
  std::shared_lock lock(mtx_);

  int64_t all_total = 0;

  for (const auto& [guid, number] : number_map_) {
    all_total += (number.expected - number.first);
  }

  if VUNLIKELY (all_total < 0) {
    return 0;
  }

  return all_total;
}

uint64_t CalculateSample::get_lost() const noexcept {
  std::shared_lock lock(mtx_);

  uint64_t all_lost = 0;

  for (const auto& [guid, number] : number_map_) {
    all_lost += number.lost;
  }

  return all_lost;
}

}  // namespace vlink
