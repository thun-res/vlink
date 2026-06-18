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

#include <vlink/base/elapsed_timer.h>
#include <vlink/base/logger.h>

#include <chrono>
#include <cmath>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// -----------------------------------------------------------------------------
// ElapsedTimer example
//
// Module:   vlink/base/elapsed_timer.h
// Scenario: ElapsedTimer is a stopwatch with two clock sources --
//             - kCpuTimestamp:  monotonic wall time (includes sleep/IO).
//             - kCpuActiveTime: per-thread CPU time (excludes blocked time).
//           Accuracy is selectable at construction (ms / us / ns). The
//           example covers each accuracy tier, both clock sources, restart()
//           lap measurement, the static timestamp utilities exposed for
//           free-standing instrumentation, and configuration query methods.
// -----------------------------------------------------------------------------
int main() {
  // Default ctor = wall clock, millisecond accuracy. start/stop bracket the
  // measurement; get() returns elapsed since the last start().
  {
    VLOG_I("=== Default (ms) ===");
    vlink::ElapsedTimer timer;
    timer.start();
    std::this_thread::sleep_for(100ms);
    MLOG_I("  elapsed={}ms (expect ~100)", timer.get());
    timer.stop();
    MLOG_I("  is_active after stop={}", timer.is_active());
  }

  // Microsecond accuracy: same clock source, finer reading. Sleep is still
  // captured because the source is kCpuTimestamp (wall time).
  {
    VLOG_I("=== Microsecond ===");
    vlink::ElapsedTimer timer(vlink::ElapsedTimer::kCpuTimestamp, vlink::ElapsedTimer::kMicro);
    timer.start();
    std::this_thread::sleep_for(50ms);
    MLOG_I("  elapsed={}us (expect ~50000)", timer.get());
    timer.stop();
  }

  // Nanosecond accuracy: only meaningful for CPU-bound microbenchmarks; the
  // sqrt loop guarantees the elapsed time is dominated by computation rather
  // than scheduling jitter. `volatile` prevents the compiler from eliding
  // the loop body when the result is otherwise unused.
  {
    VLOG_I("=== Nanosecond ===");
    vlink::ElapsedTimer timer(vlink::ElapsedTimer::kCpuTimestamp, vlink::ElapsedTimer::kNano);
    timer.start();

    volatile double sum = 0;
    for (int i = 0; i < 100000; ++i) {
      sum += std::sqrt(static_cast<double>(i));
    }

    MLOG_I("  CPU work elapsed={}ns sum={}", timer.get(), sum);
    timer.stop();
  }

  // kCpuActiveTime: thread CPU time only -- if the thread sleeps or blocks
  // on IO, elapsed does NOT advance. Used to attribute work cost to a
  // worker without contamination from scheduling delays.
  {
    VLOG_I("=== CPU active time ===");
    vlink::ElapsedTimer timer(vlink::ElapsedTimer::kCpuActiveTime, vlink::ElapsedTimer::kMicro);
    timer.start();

    volatile double sum = 0;
    for (int i = 0; i < 1000000; ++i) {
      sum += std::sqrt(static_cast<double>(i));
    }

    MLOG_I("  CPU active={}us sum={}", timer.get(), sum);
    timer.stop();
  }

  // restart() atomically reads the current elapsed value and resets the
  // start time -- canonical pattern for measuring sequential laps without
  // missing time between stop() and start().
  {
    VLOG_I("=== restart ===");
    vlink::ElapsedTimer timer(vlink::ElapsedTimer::kCpuTimestamp, vlink::ElapsedTimer::kMilli);
    timer.start();
    std::this_thread::sleep_for(100ms);
    MLOG_I("  lap1={}ms", timer.restart());
    std::this_thread::sleep_for(50ms);
    MLOG_I("  lap2={}ms", timer.restart());
    timer.stop();
  }

  // Static utility wrappers: read the underlying clocks without owning a
  // timer instance. Useful for one-shot instrumentation in headers / hot
  // paths where ctor/dtor cost matters.
  {
    VLOG_I("=== Static utilities ===");
    MLOG_I("  sys ms     = {}", vlink::ElapsedTimer::get_sys_timestamp(vlink::ElapsedTimer::kMilli));
    MLOG_I("  cpu ms     = {}", vlink::ElapsedTimer::get_cpu_timestamp(vlink::ElapsedTimer::kMilli));
    MLOG_I("  cpu active = {}us", vlink::ElapsedTimer::get_cpu_active_time(vlink::ElapsedTimer::kMicro));
    MLOG_I("  sys ns     = {}", vlink::ElapsedTimer::get_sys_timestamp(vlink::ElapsedTimer::kNano));
  }

  // Configuration queries: a never-started timer reports get()==0 and
  // is_active==false. get_method/get_accuracy expose what was passed at
  // construction time -- handy for generic test harnesses.
  {
    VLOG_I("=== Config query ===");
    vlink::ElapsedTimer timer(vlink::ElapsedTimer::kCpuActiveTime, vlink::ElapsedTimer::kNano);
    MLOG_I("  method={} (0=CpuTimestamp 1=CpuActiveTime)", static_cast<int>(timer.get_method()));
    MLOG_I("  accuracy={} (0=Milli 1=Micro 2=Nano)", static_cast<int>(timer.get_accuracy()));
    MLOG_I("  is_active before start={} get={}", timer.is_active(), timer.get());
  }

  VLOG_I("ElapsedTimer example finished.");
  return 0;
}
