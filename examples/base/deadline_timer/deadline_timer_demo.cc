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

#include <vlink/base/deadline_timer.h>
#include <vlink/base/logger.h>

#include <chrono>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// -----------------------------------------------------------------------------
// DeadlineTimer demo
//
// Module:   vlink/base/deadline_timer.h
// Scenario: DeadlineTimer is a passive "absolute / relative deadline reached?"
//           predicate -- there is no callback, no thread, no signalling. It
//           is the right tool for budgeting loops (e.g. retry-within-200ms)
//           and timeout detection. Construction stamps the deadline against
//           the monotonic CPU clock; has_expired() / remaining_time() poll
//           without locking. The example covers default construction,
//           relative / absolute deadlines at ms and us accuracy, reset+reuse
//           and two practical loop patterns.
// -----------------------------------------------------------------------------
int main() {
  VLOG_I("=== VLink DeadlineTimer Demo ===");

  // Default-constructed timer has no deadline at all -- is_valid() is false
  // and has_expired() returns false (no deadline to expire against).
  {
    VLOG_I("--- Default construction ---");
    vlink::DeadlineTimer timer;
    VLOG_I("  is_valid=", timer.is_valid(), " has_expired=", timer.has_expired(), " deadline=", timer.deadline());
  }

  // Relative deadline: 200 ms from "now". remaining_time() decreases as
  // wall-clock advances; once it crosses zero, has_expired() flips true.
  {
    VLOG_I("--- Relative deadline 200ms ---");
    vlink::DeadlineTimer timer(200, vlink::ElapsedTimer::kMilli);
    VLOG_I("  remaining=", timer.remaining_time(), "ms has_expired=", timer.has_expired());

    std::this_thread::sleep_for(50ms);
    VLOG_I("  after 50ms: remaining=", timer.remaining_time(), "ms expired=", timer.has_expired());

    std::this_thread::sleep_for(200ms);
    VLOG_I("  after 250ms total: remaining=", timer.remaining_time(), "ms expired=", timer.has_expired());
  }

  // Absolute deadline: caller computes the wall-clock target from
  // get_cpu_timestamp + offset and hands it in. Useful when several
  // operations share one deadline computed earlier.
  {
    VLOG_I("--- Absolute deadline ---");
    vlink::DeadlineTimer timer;
    uint64_t now_ms = vlink::ElapsedTimer::get_cpu_timestamp(vlink::ElapsedTimer::kMilli);
    timer.set_deadline_abs(now_ms + 150);
    VLOG_I("  now=", now_ms, "ms deadline_abs=", now_ms + 150, "ms remaining=", timer.remaining_time(), "ms");

    std::this_thread::sleep_for(200ms);
    VLOG_I("  after 200ms expired=", timer.has_expired());
  }

  // reset() clears the deadline -- is_valid() flips false. set_deadline()
  // re-arms it; this is the canonical reuse pattern for a thread-local
  // DeadlineTimer that gets re-bound per request.
  {
    VLOG_I("--- Reset + reuse ---");
    vlink::DeadlineTimer timer(100);
    VLOG_I("  initial valid=", timer.is_valid(), " remaining=", timer.remaining_time(), "ms");

    timer.reset();
    VLOG_I("  after reset valid=", timer.is_valid());

    timer.set_deadline(300);
    VLOG_I("  after set_deadline(300) valid=", timer.is_valid(), " remaining=", timer.remaining_time(), "ms");
  }

  // Microsecond accuracy: the underlying clock is the same -- only the unit
  // of the deadline and the unit returned by remaining_time() change.
  {
    VLOG_I("--- Microsecond accuracy ---");
    vlink::DeadlineTimer timer(50000, vlink::ElapsedTimer::kMicro);
    VLOG_I("  accuracy=", static_cast<int>(timer.get_accuracy()), " remaining=", timer.remaining_time(), "us");

    std::this_thread::sleep_for(20ms);
    VLOG_I("  after 20ms: remaining=", timer.remaining_time(), "us");

    std::this_thread::sleep_for(40ms);
    VLOG_I("  after 60ms: expired=", timer.has_expired());
  }

  // Copy / assignment: deadlines are plain value-types; copies share the
  // same absolute deadline but no other state (no shared mutable storage).
  {
    VLOG_I("--- Copy semantics ---");
    vlink::DeadlineTimer original(500);

    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    vlink::DeadlineTimer copy_a(original);
    vlink::DeadlineTimer copy_b;
    copy_b = original;
    VLOG_I("  original remaining=", original.remaining_time(), "ms equal=", (copy_a.deadline() == copy_b.deadline()));
  }

  // Practical pattern: poll some external state and bail out either on
  // success or when the deadline trips. The body never touches sleep with
  // an unbounded duration.
  {
    VLOG_I("--- Practical: timeout detection ---");
    vlink::DeadlineTimer deadline(100);
    int iter = 0;
    bool received = false;
    while (!deadline.has_expired()) {
      ++iter;

      if (iter == 5) {
        received = true;
        break;
      }

      std::this_thread::sleep_for(10ms);
    }

    if (received) {
      VLOG_I("  got response after ", iter, " iterations, remaining=", deadline.remaining_time(), "ms");
    } else {
      VLOG_I("  timeout after ", iter, " iterations");
    }
  }

  // Retry-within-budget pattern: a single deadline governs N attempts. Each
  // attempt is allowed to take whatever time it wants as long as the
  // overall budget remains.
  {
    VLOG_I("--- Practical: retry within 200ms ---");
    vlink::DeadlineTimer overall(200);
    int attempt = 0;
    while (!overall.has_expired()) {
      ++attempt;

      if (attempt >= 4) {
        VLOG_I("  success on attempt ", attempt);
        break;
      }

      VLOG_I("  attempt ", attempt, " failed, remaining=", overall.remaining_time(), "ms");
      std::this_thread::sleep_for(30ms);
    }

    if (overall.has_expired()) {
      VLOG_W("  retries exhausted");
    }
  }

  VLOG_I("DeadlineTimer demo completed.");
  vlink::Logger::flush();
  return 0;
}
