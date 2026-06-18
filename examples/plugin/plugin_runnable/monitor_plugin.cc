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

// =============================================================================
// File: monitor_plugin.cc
//
// Implementation .so for the runnable-plugin demo. RunablePluginInterface is
// vlink's contract for plugins that need their own MessageLoop, i.e. plugins
// that wire up timers / subscribers / sockets and need a thread to dispatch
// callbacks. Internally RunablePluginInterface inherits from MessageLoop, so
// the implementation IS-A loop: calling async_run() on it spawns the worker
// thread, and any Timer / Subscriber attached with "this" will deliver
// callbacks on that thread.
//
// Lifecycle observed by the host:
//   async_run()    -- spawn worker thread, loop starts pumping
//   on_init()      -- user hook; runs on the loop thread when invoked from
//                     ProxyServer, but the example invokes it directly so it
//                     runs on the caller's thread. Either way, callbacks
//                     installed here will execute on the loop thread.
//   ... ticks ...
//   on_deinit()    -- user hook; release resources owned by the plugin
//   quit()         -- ask the loop to stop pumping
//   wait_for_quit()-- join the worker thread
// =============================================================================

#include <vlink/base/elapsed_timer.h>
#include <vlink/base/logger.h>
#include <vlink/base/timer.h>
#include <vlink/extension/runnable_plugin_interface.h>

#include <atomic>
#include <memory>

class MonitorPlugin : public vlink::RunablePluginInterface {
  // Register against the interface name so the host's
  // load<RunablePluginInterface>() can match this .so.
  VLINK_PLUGIN_REGISTER(vlink::RunablePluginInterface)

 public:
  // on_init: user-supplied setup hook. When invoked from ProxyServer, runs
  // on the plugin's MessageLoop thread (post-async_run). Here it creates a
  // periodic Timer attached to "this" (the loop), so the timer callback
  // executes on the loop thread.
  void on_init() override {
    VLOG_I("[MonitorPlugin] on_init -- creating 500ms timer");
    tick_count_.store(0);

    // Timer(parent_loop, interval_ms, kInfinite, callback). The first arg is
    // "this" because RunablePluginInterface IS-A MessageLoop; the callback
    // therefore fires on the loop thread, serialised with any other
    // callbacks bound to the same loop.
    timer_ = std::make_unique<vlink::Timer>(this, 500, vlink::Timer::kInfinite, [this]() {
      // Timer callback: runs on the loop thread (this == loop). Atomic
      // is overkill here but documents the cross-thread contract.
      int count = tick_count_.fetch_add(1) + 1;
      int64_t elapsed_us = uptime_.get();
      VLOG_I("[MonitorPlugin] tick #", count, " uptime=", elapsed_us / 1000, " ms");
    });

    uptime_.start();
    timer_->start();
  }

  // on_deinit: user-supplied teardown hook. Stop the timer BEFORE clearing
  // it so any in-flight callback completes. Both happen on whatever thread
  // the host calls on_deinit() from, which is why we stop synchronously.
  void on_deinit() override {
    VLOG_I("[MonitorPlugin] on_deinit -- stopping timer");

    if (timer_) {
      timer_->stop();
      timer_.reset();
    }

    int64_t total_us = uptime_.get();
    uptime_.stop();
    VLOG_I("[MonitorPlugin] total ticks=", tick_count_.load(), " uptime=", total_us / 1000, " ms");
  }

 private:
  // Owned by this plugin so its lifetime is strictly inside on_init/on_deinit.
  std::unique_ptr<vlink::Timer> timer_;
  // Tick counter incremented by the timer callback; atomic so the host
  // thread can read tick_count_.load() safely during teardown.
  std::atomic<int> tick_count_{0};
  // High-resolution wall-clock for uptime reporting.
  vlink::ElapsedTimer uptime_{vlink::ElapsedTimer::kCpuTimestamp, vlink::ElapsedTimer::kMicro};
};

// Export factory / destroyer / version (1, 0) -- the host must request
// load<RunablePluginInterface>("monitor_plugin", 1, 0) to match.
VLINK_PLUGIN_DECLARE(MonitorPlugin, 1, 0)
