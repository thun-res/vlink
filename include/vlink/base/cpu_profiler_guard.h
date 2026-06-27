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
 * @file cpu_profiler_guard.h
 * @brief Stack-based RAII bracket that pairs every @c CpuProfiler::begin with a guaranteed end.
 *
 * @details
 * @c CpuProfilerGuard ties the @c begin / @c end pair of a @c CpuProfiler to scope lifetime so
 * the active interval closes even when the protected region exits through an exception.
 *
 * @par RAII pattern
 *
 * @verbatim
 *   scope entry +---------------------------+ scope exit
 *   ----------> | CpuProfilerGuard guard(p) | ---------->
 *               +-----------+---------------+
 *                           | ctor: p->begin()
 *                           v
 *                       active interval
 *                           |
 *                           | dtor: p->end()
 *                           v
 *                       interval closed
 * @endverbatim
 *
 * The guard accepts a @c nullptr profiler so call sites can be guarded uniformly while a
 * higher-level switch (typically @c CpuProfiler::is_global_enabled) decides whether the work is
 * worth measuring.
 *
 * @par Example
 * @code
 *   vlink::CpuProfiler profiler;
 *
 *   void process_frame() {
 *     vlink::CpuProfilerGuard guard(&profiler);   // profiler.begin()
 *     do_frame_work();
 *   }                                             // profiler.end() on scope exit
 *
 *   const double utilisation = profiler.get();
 * @endcode
 *
 * @note Non-copyable and non-movable; the guard must live on the stack of the protected region.
 */

#pragma once

#include "./macros.h"

namespace vlink {

/**
 * @class CpuProfilerGuard
 * @brief Scope guard that opens and closes a @c CpuProfiler active interval.
 *
 * @details
 * Calls @c CpuProfiler::begin in its constructor and @c CpuProfiler::end in its destructor.
 * A null profiler pointer is accepted; both ends become no-ops in that case.
 */
class VLINK_EXPORT CpuProfilerGuard final {
 public:
  /**
   * @brief Constructs the guard and opens the active interval on @p profiler.
   *
   * @param profiler  Target profiler; @c nullptr disables both ends.
   */
  explicit CpuProfilerGuard(class CpuProfiler* profiler) noexcept;

  /**
   * @brief Destructor; closes the active interval on the bound profiler.
   */
  ~CpuProfilerGuard() noexcept;

 private:
  class CpuProfiler* profiler_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(CpuProfilerGuard)
};

}  // namespace vlink
