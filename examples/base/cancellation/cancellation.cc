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
#include <vlink/base/exception.h>
#include <vlink/base/logger.h>
#include <vlink/base/message_loop.h>
#include <vlink/base/task_handle.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// -----------------------------------------------------------------------------
// Cancellation example
//
// Module:   vlink/base/cancellation.h (+ task_handle.h integration)
// Scenario: Full tour of the cooperative cancellation primitive. The model:
//             - CancellationSource owns the cancellation state.
//             - CancellationToken is a cheap, copyable observer handed to
//               workers; it lets them poll, throw, or register callbacks.
//             - CancellationRegistration is the RAII handle returned by
//               register_callback; resetting it before fire unregisters.
// Key invariants exercised below:
//             - request_cancel is idempotent (returns false on second call).
//             - Callbacks registered AFTER request_cancel fire synchronously
//               on the caller's thread; those registered BEFORE fire on the
//               canceller's thread.
//             - Callback exceptions are swallowed/isolated; siblings still
//               fire. Reentrant cancellation across sources is safe.
//             - The token can be threaded into PostTaskOptions so a task
//               inherits parent cancellation without manual plumbing.
// -----------------------------------------------------------------------------
int main() {
  vlink::Logger::init("example_cancellation");

  // Polling style: simplest and most portable. The worker periodically asks
  // is_cancellation_requested and exits cooperatively. Second request_cancel
  // returns false because the state already flipped.
  {
    VLOG_I("=== Polling cancellation ===");
    vlink::CancellationSource source;
    auto token = source.token();

    std::thread worker([token]() {
      uint32_t iter = 0;
      while (!token.is_cancellation_requested()) {
        ++iter;
        std::this_thread::sleep_for(10ms);
      }

      VLOG_I("  worker exited after ", iter, " iterations");
    });

    std::this_thread::sleep_for(80ms);
    VLOG_I("  request_cancel first=", source.request_cancel(), " second=", source.request_cancel());
    worker.join();
  }

  // Structured / exception style: throw_if_cancellation_requested throws
  // OperationCancelled, which unwinds the stack normally and triggers RAII
  // dtors -- preferred when work is deeply nested.
  {
    VLOG_I("=== Structured (OperationCancelled) ===");
    vlink::CancellationSource source;
    auto token = source.token();

    std::thread worker([token]() {
      struct Raii {
        ~Raii() { VLOG_I("  RAII dtor ran during unwind"); }
      };

      try {
        Raii guard;
        for (int i = 0; i < 1000; ++i) {
          token.throw_if_cancellation_requested();
          std::this_thread::sleep_for(5ms);
        }
      } catch (const vlink::Exception::OperationCancelled& ex) {
        VLOG_I("  caught: ", ex.what());
      }
    });

    std::this_thread::sleep_for(50ms);
    source.request_cancel();
    worker.join();
  }

  // register_callback: the callback fires on whichever thread invokes
  // request_cancel (here, the main thread). reg.valid() goes false after
  // fire because the registration is consumed.
  {
    VLOG_I("=== register_callback async fire ===");
    vlink::CancellationSource source;
    auto token = source.token();

    // Counter incremented from inside the callback to prove it fired.
    std::atomic_int fired{0};
    auto reg = token.register_callback([&fired]() { fired.fetch_add(1); });
    VLOG_I("  reg.valid pre-cancel=", reg.valid());
    source.request_cancel();
    VLOG_I("  fired=", fired.load(), " reg.valid post-cancel=", reg.valid());
  }

  // Register-after-cancel: source is already cancelled, so register_callback
  // fires the callback synchronously on the caller's thread before
  // returning -- this guarantees no callback is silently dropped.
  {
    VLOG_I("=== register after cancel (sync fire) ===");
    vlink::CancellationSource source;
    auto token = source.token();
    source.request_cancel();

    const auto caller = std::this_thread::get_id();
    std::atomic<std::thread::id> fire_tid{};
    auto reg = token.register_callback([&fire_tid]() { fire_tid.store(std::this_thread::get_id()); });
    VLOG_I("  reg.valid=", reg.valid(), " sync_on_caller=", (caller == fire_tid.load()));
  }

  // reset() before fire: unregisters the callback. Useful to cancel a
  // subscription before its source ever fires.
  {
    VLOG_I("=== reset before fire ===");
    vlink::CancellationSource source;
    auto token = source.token();

    std::atomic_int fired{0};
    auto reg = token.register_callback([&fired]() { fired.fetch_add(1); });
    reg.reset();
    source.request_cancel();
    VLOG_I("  fired (expect 0)=", fired.load());
  }

  // Fan-out: one source feeds many tokens (one per observer thread). All
  // observers see cancellation simultaneously -- token copies are cheap.
  {
    VLOG_I("=== Fan-out ===");
    vlink::CancellationSource source;
    static constexpr int kObservers = 6;
    std::atomic_int joined{0};
    std::vector<std::thread> workers;
    workers.reserve(kObservers);

    for (int i = 0; i < kObservers; ++i) {
      workers.emplace_back([token = source.token(), i, &joined]() {
        while (!token.is_cancellation_requested()) {
          std::this_thread::sleep_for(5ms);
        }

        joined.fetch_add(1);
        VLOG_I("  observer ", i, " saw cancel");
      });
    }

    std::this_thread::sleep_for(40ms);
    source.request_cancel();
    for (auto& t : workers) {
      t.join();
    }

    VLOG_I("  all joined=", joined.load());
  }

  // Hierarchical cascade: a parent's cancellation propagates to children by
  // chaining register_callback. Useful for cancelling a tree of subtasks
  // without each layer holding the parent source directly.
  {
    VLOG_I("=== Parent / child cascade ===");
    vlink::CancellationSource root;
    vlink::CancellationSource middle;
    vlink::CancellationSource leaf;

    // Callbacks fire on the request_cancel caller's thread; the chain runs
    // synchronously here so by the time root.request_cancel() returns,
    // leaf has also been cancelled.
    auto reg_mid = root.token().register_callback([&middle]() {
      VLOG_I("  root fired -> cancel middle");
      middle.request_cancel();
    });
    auto reg_leaf = middle.token().register_callback([&leaf]() {
      VLOG_I("  middle fired -> cancel leaf");
      leaf.request_cancel();
    });

    auto leaf_token = leaf.token();
    root.request_cancel();
    VLOG_I("  leaf.is_cancellation_requested=", leaf_token.is_cancellation_requested());
  }

  // Sibling cancellation from within a callback: cancelling source B from
  // inside source A's callback must NOT deadlock; cancellation locking is
  // per-source.
  {
    VLOG_I("=== Sibling cancel inside callback ===");
    vlink::CancellationSource a;
    vlink::CancellationSource b;

    auto reg = a.token().register_callback([token_a = a.token(), &b]() {
      VLOG_I("  inside a.cb a.is_cancellation_requested=", token_a.is_cancellation_requested());
      b.request_cancel();
    });

    a.request_cancel();
    VLOG_I("  b.is_cancellation_requested=", b.token().is_cancellation_requested());
  }

  // Exception isolation: a throwing callback must not prevent subsequent
  // callbacks from firing. The framework swallows the exception silently.
  {
    VLOG_I("=== Callback exception isolation ===");
    vlink::CancellationSource source;
    auto token = source.token();
    auto reg1 = token.register_callback([]() { throw std::runtime_error("boom"); });
    std::atomic_bool reached{false};
    auto reg2 = token.register_callback([&reached]() { reached.store(true); });

    source.request_cancel();
    VLOG_I("  second callback still fired=", reached.load());
  }

  // Empty / default-constructed token: no source attached, so the token can
  // never report cancellation and registrations are no-ops.
  {
    VLOG_I("=== Empty token semantics ===");
    vlink::CancellationToken empty;
    std::atomic_int never{0};
    auto reg = empty.register_callback([&never]() { never.fetch_add(1); });
    VLOG_I("  empty.valid=", empty.valid(), " req=", empty.is_cancellation_requested(), " reg.valid=", reg.valid(),
           " fired=", never.load());
  }

  // Race: many registrars contend with one canceller. Some registrations
  // win the race and fire; others reset before fire. The invariant is
  // fired <= registered, which the test sanity-checks.
  {
    VLOG_I("=== Concurrent producers racing one source ===");
    vlink::CancellationSource source;
    auto token = source.token();

    std::atomic_int registered{0};
    std::atomic_int fired{0};

    std::vector<std::thread> registrars;
    registrars.reserve(8);
    for (int i = 0; i < 8; ++i) {
      registrars.emplace_back([&]() {
        auto reg = token.register_callback([&fired]() { fired.fetch_add(1); });
        registered.fetch_add(1);

        // Reset half the registrations to also stress the unregister path.
        if ((registered.load() & 1U) != 0U) {
          reg.reset();
        }
      });
    }

    std::this_thread::sleep_for(2ms);
    source.request_cancel();
    for (auto& t : registrars) {
      t.join();
    }

    VLOG_I("  registered=", registered.load(), " fired=", fired.load(), " (fired <= registered; race-dependent)");
  }

  // TaskHandle integration: passing a token through PostTaskOptions lets a
  // task observe cancellation from outside, while still completing normally
  // (returns from the callback) -- yielding a kCompleted state, not kCancelled.
  {
    VLOG_I("=== Integration with TaskHandle ===");
    vlink::MessageLoop loop;
    loop.async_run();

    vlink::CancellationSource batch;
    vlink::PostTaskOptions opts;
    opts.cancellation_token = batch.token();

    auto h = loop.post_task_handle(
        [token = opts.cancellation_token]() {
          while (!token.is_cancellation_requested()) {
            std::this_thread::sleep_for(5ms);
          }

          VLOG_I("  task observed inherited token");
        },
        opts);

    std::this_thread::sleep_for(30ms);
    batch.request_cancel();
    h.wait();

    loop.quit();
    loop.wait_for_quit();
  }

  VLOG_I("example_cancellation done");
  return 0;
}
