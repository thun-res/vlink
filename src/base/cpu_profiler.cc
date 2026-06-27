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

#include "./base/cpu_profiler.h"

#include <string>

#include "./base/utils.h"

#define VLINK_PROFILER_DEFAULT_STATE 0

namespace vlink {

[[maybe_unused]] static bool get_profiler_enabled_for_env() {
#if VLINK_PROFILER_DEFAULT_STATE
  std::string enable_str = Utils::get_env("VLINK_PROFILER_ENABLE", "1");
#else
  std::string enable_str = Utils::get_env("VLINK_PROFILER_ENABLE", "0");
#endif

  return enable_str == "1";
}

// CpuProfiler
CpuProfiler::CpuProfiler() noexcept = default;

CpuProfiler::~CpuProfiler() noexcept = default;

bool CpuProfiler::is_global_enabled() noexcept {
  static bool profiler_enable = get_profiler_enabled_for_env();
  return profiler_enable;
}

void CpuProfiler::begin() noexcept {
  SpinLockGuard lock(spin_);

  cpu_active_timer_.restart();
  cpu_timestamp_timer_.start();
}

void CpuProfiler::end() noexcept {
  SpinLockGuard lock(spin_);

  auto active = cpu_active_timer_.restart();

  if VUNLIKELY (active < 0) {
    return;
  }

  total_active_.fetch_add(active, std::memory_order_acq_rel);
}

double CpuProfiler::get() const noexcept {
  auto total_timestamp = cpu_timestamp_timer_.get();

  if VUNLIKELY (total_timestamp <= 0) {
    return 0;
  }

  return static_cast<double>(total_active_.load(std::memory_order_acquire)) / static_cast<double>(total_timestamp) *
         100.0;
}

double CpuProfiler::restart() noexcept {
  SpinLockGuard lock(spin_);

  auto value = get();

  cpu_active_timer_.restart();
  cpu_timestamp_timer_.restart();

  total_active_.store(0, std::memory_order_release);

  return value;
}

}  // namespace vlink
