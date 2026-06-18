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

#include <vlink/base/cancellation.h>
#include <vlink/base/logger.h>
#include <vlink/base/message_loop.h>
#include <vlink/base/task_handle.h>
#include <vlink/base/thread_pool.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// Pretty-name the eight terminal/intermediate task states so each scenario
// reports its outcome in human-readable form.
static const char* state_name(vlink::TaskExecutionState s) {
  switch (s) {
    case vlink::TaskExecutionState::kInvalid:
      return "kInvalid";
    case vlink::TaskExecutionState::kQueued:
      return "kQueued";
    case vlink::TaskExecutionState::kRunning:
      return "kRunning";
    case vlink::TaskExecutionState::kCompleted:
      return "kCompleted";
    case vlink::TaskExecutionState::kCancelled:
      return "kCancelled";
    case vlink::TaskExecutionState::kDropped:
      return "kDropped";
    case vlink::TaskExecutionState::kRejected:
      return "kRejected";
    case vlink::TaskExecutionState::kFailed:
      return "kFailed";
  }

  return "?";
}
// -----------------------------------------------------------------------------
// TaskHandle example
//
// Module:   vlink/base/task_handle.h (paired with message_loop / thread_pool)
// Scenario: post_task_handle / post_task_with_priority_handle return a
//           TaskHandle -- a reference to the task's lifecycle. The handle
//           supports:
//             wait()         block until terminal state.
//             wait(timeout)  bounded wait, returns false on timeout.
//             cancel()       request cancellation (cooperative, polled
//                            via the inherited cancellation_token).
//             state()        snapshot of the current TaskExecutionState.
//             is_done()      shorthand for any terminal state.
//             cancellation_token() the task's OWN token (separate from
//                            any parent token wired via PostTaskOptions).
// State map: kQueued -> kRunning -> (kCompleted | kFailed | kCancelled);
//            kRejected (queue full + kReject overflow policy);
//            kDropped (kDroppable + queue overflow on certain queue types).
// IMPORTANT: handle copies share state -- cancelling one cancels them all.
//            Destroying the LAST handle does NOT cancel the task; the task
//            runs to completion regardless.
// -----------------------------------------------------------------------------
int main() {
  vlink::Logger::init("example_task_handle");

  // Happy path: post a body, wait for it. wait() blocks the caller until
  // the task reaches a terminal state. state() afterwards reports kCompleted.
  {
    VLOG_I("=== post_task_handle + wait ===");
    vlink::MessageLoop loop;
    loop.async_run();

    std::atomic_int counter{0};
    auto h = loop.post_task_handle([&counter]() {
      std::this_thread::sleep_for(30ms);
      counter.fetch_add(1);
    });
    h.wait();
    VLOG_I("  state=", state_name(h.state()), " counter=", counter.load());

    loop.quit();
    loop.wait_for_quit();
  }

  // Parent cancellation_token (via PostTaskOptions): the task inherits the
  // token; cancelling the parent makes is_cancellation_requested true on
  // the inherited copy without touching the handle. Body must POLL to
  // observe -- this is cooperative cancellation.
  {
    VLOG_I("=== Parent cancellation_token ===");
    vlink::MessageLoop loop;
    loop.async_run();

    vlink::CancellationSource parent;
    vlink::PostTaskOptions opts;
    opts.cancellation_token = parent.token();

    auto h = loop.post_task_handle(
        [token = opts.cancellation_token]() {
          while (!token.is_cancellation_requested()) {
            std::this_thread::sleep_for(5ms);
          }

          VLOG_I("  worker observed cancellation via parent token");
        },
        opts);

    std::this_thread::sleep_for(30ms);
    parent.request_cancel();
    h.wait();
    VLOG_I("  state=", state_name(h.state()));

    loop.quit();
    loop.wait_for_quit();
  }

  // Cancel while still queued: the loop never ran, so the task never
  // started. cancel() transitions kQueued -> kCancelled and the body is
  // guaranteed NOT to run.
  {
    VLOG_I("=== cancel while queued ===");
    vlink::MessageLoop loop;  // not started, task stays queued
    auto h = loop.post_task_handle([]() { VLOG_E("should not run"); });
    VLOG_I("  before cancel: ", state_name(h.state()));
    h.cancel();
    VLOG_I("  after cancel:  ", state_name(h.state()));
  }

  // Cancel during execution: the body polls its OWN token (via
  // h.cancellation_token()) and exits cleanly. Because the body returned
  // normally, the final state is kCompleted -- NOT kCancelled. kCancelled
  // is reserved for cancellation before the body ran.
  {
    VLOG_I("=== cancel during execution (own token polling) ===");
    vlink::MessageLoop loop;
    loop.async_run();

    vlink::TaskHandle h;
    h = loop.post_task_handle([&h]() {
      auto self_token = h.cancellation_token();
      uint32_t iter = 0;
      while (!self_token.is_cancellation_requested()) {
        ++iter;
        std::this_thread::sleep_for(5ms);
      }

      VLOG_I("  task saw self.cancel() after ", iter, " iterations");
    });

    std::this_thread::sleep_for(40ms);
    h.cancel();
    h.wait();
    VLOG_I("  state=", state_name(h.state()), " (kCompleted: task returned)");

    loop.quit();
    loop.wait_for_quit();
  }

  // Body throws -> kFailed. The framework catches std::exception (and
  // anything else) and stores the terminal state; the loop continues.
  {
    VLOG_I("=== Callback throws ===");
    vlink::MessageLoop loop;
    loop.async_run();

    auto h = loop.post_task_handle([]() { throw std::runtime_error("nope"); });
    h.wait();
    VLOG_I("  state=", state_name(h.state()));

    loop.quit();
    loop.wait_for_quit();
  }

  // Priority variants give back a handle just like the non-priority forms.
  // Final state is kCompleted for all three regardless of priority.
  {
    VLOG_I("=== Priority post_task_with_priority_handle ===");
    vlink::MessageLoop loop(vlink::MessageLoop::kPriorityType);
    loop.async_run();

    std::vector<vlink::TaskHandle> handles;
    handles.reserve(3);
    handles.push_back(
        loop.post_task_with_priority_handle([]() { VLOG_I("  low"); }, vlink::MessageLoop::kLowestPriority));
    handles.push_back(
        loop.post_task_with_priority_handle([]() { VLOG_I("  normal"); }, vlink::MessageLoop::kNormalPriority));
    handles.push_back(
        loop.post_task_with_priority_handle([]() { VLOG_I("  high"); }, vlink::MessageLoop::kHighestPriority));

    for (auto& h : handles) {
      h.wait();
    }

    for (size_t i = 0; i < handles.size(); ++i) {
      VLOG_I("  handle[", i, "]=", state_name(handles[i].state()));
    }

    loop.quit();
    loop.wait_for_quit();
  }

  // Posting AFTER quit + kReject overflow policy -> kRejected. The body
  // never runs; the caller observes the rejection through state().
  {
    VLOG_I("=== kReject after quit ===");
    vlink::MessageLoop loop;
    loop.async_run();
    loop.quit();
    loop.wait_for_quit();

    vlink::PostTaskOptions opts;
    opts.overflow_policy = vlink::TaskOverflowPolicy::kReject;
    auto h = loop.post_task_handle([]() { VLOG_E("should not run"); }, opts);
    VLOG_I("  state=", state_name(h.state()));
  }

  // kProtected vs kDroppable: on a queue that may drop, kProtected pins
  // the task while kDroppable lets it be evicted under overflow. Here we
  // only post two and then cancel both -- the example is structural.
  {
    VLOG_I("=== kProtected vs kDroppable ===");
    vlink::MessageLoop loop;
    loop.set_strategy(vlink::MessageLoop::kPopStrategy);

    vlink::PostTaskOptions protect_opts;
    protect_opts.drop_policy = vlink::TaskDropPolicy::kProtected;
    vlink::PostTaskOptions drop_opts;
    drop_opts.drop_policy = vlink::TaskDropPolicy::kDroppable;

    auto protected_h = loop.post_task_handle([]() {}, protect_opts);
    auto droppable_h = loop.post_task_handle([]() {}, drop_opts);
    VLOG_I("  protected=", state_name(protected_h.state()), " droppable=", state_name(droppable_h.state()));

    protected_h.cancel();
    droppable_h.cancel();
  }

  // ThreadPool group-cancel pattern: one CancellationSource is wired into
  // PostTaskOptions for many tasks. request_cancel() unblocks all of them
  // simultaneously -- the canonical way to abort an entire batch.
  {
    VLOG_I("=== ThreadPool group cancellation ===");
    vlink::ThreadPool pool(4);

    vlink::CancellationSource batch;
    vlink::PostTaskOptions opts;
    opts.cancellation_token = batch.token();

    std::vector<vlink::TaskHandle> handles;
    handles.reserve(8);
    for (int i = 0; i < 8; ++i) {
      handles.emplace_back(pool.post_task_handle(
          [token = opts.cancellation_token, i]() {
            while (!token.is_cancellation_requested()) {
              std::this_thread::sleep_for(10ms);
            }

            VLOG_I("  worker ", i, " saw cancel");
          },
          opts));
    }

    std::this_thread::sleep_for(40ms);
    batch.request_cancel();
    for (auto& h : handles) {
      h.wait();
    }

    int terminal = 0;
    for (auto& h : handles) {
      if (h.is_done()) {
        ++terminal;
      }
    }

    VLOG_I("  terminal=", terminal, "/8");
    pool.shutdown();
  }

  // Timed wait: wait(ms) returns false if the deadline expires before the
  // task finishes. The handle remains valid; calling wait() again (with a
  // longer deadline or unbounded) is fine.
  {
    VLOG_I("=== wait with timeout ===");
    vlink::MessageLoop loop;
    loop.async_run();

    auto h = loop.post_task_handle([]() { std::this_thread::sleep_for(200ms); });
    VLOG_I("  wait(50)=", h.wait(50), " state=", state_name(h.state()));
    VLOG_I("  wait(500)=", h.wait(500), " state=", state_name(h.state()));

    loop.quit();
    loop.wait_for_quit();
  }

  // Handle destruction does NOT cancel: the task still runs to completion.
  // This is intentional; otherwise fire-and-forget posting would require
  // keeping a handle alive purely to avoid spurious cancellation.
  {
    VLOG_I("=== Handle destruction does not cancel ===");
    vlink::MessageLoop loop;
    loop.async_run();

    std::atomic_bool ran{false};
    {
      auto h = loop.post_task_handle([&ran]() {
        std::this_thread::sleep_for(40ms);
        ran.store(true);
      });
    }

    loop.wait_for_idle();
    VLOG_I("  ran=", ran.load());

    loop.quit();
    loop.wait_for_quit();
  }

  // Handle copies share state: cancelling `view` cancels the task and the
  // original `h` sees the same terminal state. Useful when handing a
  // handle to another component while keeping a local copy for waiting.
  {
    VLOG_I("=== Handle copies share state ===");
    vlink::MessageLoop loop;
    loop.async_run();

    vlink::TaskHandle h;
    h = loop.post_task_handle([&h]() {
      auto self_token = h.cancellation_token();
      while (!self_token.is_cancellation_requested()) {
        std::this_thread::sleep_for(5ms);
      }
    });

    vlink::TaskHandle view = h;  // copy shares state
    std::this_thread::sleep_for(30ms);
    view.cancel();
    h.wait();
    VLOG_I("  h=", state_name(h.state()), " view=", state_name(view.state()));

    loop.quit();
    loop.wait_for_quit();
  }

  // Default-constructed handle is invalid: every operation is a safe no-op
  // returning the obvious "empty" value. Useful as a sentinel.
  {
    VLOG_I("=== Invalid handle ===");
    vlink::TaskHandle invalid;
    VLOG_I("  valid=", invalid.valid(), " state=", state_name(invalid.state()), " is_done=", invalid.is_done());
    VLOG_I("  wait(10)=", invalid.wait(10), " cancel=", invalid.cancel(),
           " token.valid=", invalid.cancellation_token().valid());
  }

  VLOG_I("example_task_handle done");
  return 0;
}
