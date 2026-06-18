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
// File: status_monitoring.cc
//
// Demonstrates the two observability hooks every node primitive exposes:
//
//   1. get_cpu_usage() -- returns the per-node CPU consumption (0.0..N.0,
//      where N == #cores). Returns -1 when the optional profiler is not
//      compiled / enabled (controlled by VLINK_PROFILER_ENABLE env var or
//      the CPU_PROFILER cmake option). The number is sampled by a global
//      profiler thread, not computed on demand.
//
//   2. register_status_handler() -- a sink for transport-level events. The
//      StatusType enum maps onto the DDS standard listener events (and the
//      vlink intra:// transport translates internally), e.g.:
//        kSampleLost, kRequestedDeadlineMissed, kIncompatibleQos,
//        kLivenessChanged, kSubscriptionMatched, kPublicationMatched,
//        kInconsistentTopic, ...
//      Each Status::BasePtr carries the type-specific payload (counts,
//      handles, missed deadlines).
//
// Status events fire on the node's delivery thread by default; attaching
// the node to a MessageLoop redirects them there.
// =============================================================================

#include <vlink/base/cpu_profiler.h>
#include <vlink/base/logger.h>
#include <vlink/vlink.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

int main() {
  // Section 1: get_cpu_usage. Will print -1 in the typical case unless the
  // user has VLINK_PROFILER_ENABLE=1 in the environment AND a compatible
  // build.
  {
    vlink::Publisher<std::string> pub("dds://status/cpu");
    double cpu_usage = pub.get_cpu_usage();
    VLOG_I("get_cpu_usage()=", cpu_usage, cpu_usage < 0 ? " (profiler disabled; export VLINK_PROFILER_ENABLE=1)" : "");
  }

  // Section 2: register_status_handler on a Subscriber, bound to a custom
  // loop so the handler is observed on a predictable thread.
  {
    vlink::MessageLoop loop;
    loop.set_name("status_loop");
    loop.async_run();

    std::atomic<int> events{0};

    vlink::Subscriber<std::string> sub("dds://status/handler");
    // attach before listen so callbacks (data AND status) land on status_loop.
    sub.attach(&loop);
    // Status handler. Triggered on: subscriber discovery (kSubscriptionMatched
    // when a publisher appears), liveness changes, QoS mismatches, sample
    // losses. Thread: status_loop worker thread.
    sub.register_status_handler([&](vlink::Status::BasePtr status) {
      events++;
      VLOG_I("[Status] type=", static_cast<int>(status->get_type()));
    });

    // Data handler. Thread: status_loop worker (same as the status handler).
    sub.listen([](const std::string& msg) { VLOG_I("[Sub] ", msg); });

    vlink::Publisher<std::string> pub("dds://status/handler");
    pub.wait_for_subscribers();

    // Burst of publishes -- mostly used to give the status handler a chance
    // to fire (kSubscriptionMatched, possibly liveness updates).
    for (int i = 0; i < 5; ++i) {
      pub.publish("status_test_" + std::to_string(i));
    }

    std::this_thread::sleep_for(200ms);
    VLOG_I("status events received: ", events.load());

    loop.quit();
    loop.wait_for_quit();
  }

  // Section 3: register_status_handler on a Publisher. Triggered on:
  // kPublicationMatched when a subscriber appears, liveness lost when a
  // subscriber disappears, kOfferedIncompatibleQos when a matching attempt
  // fails. Thread: publisher delivery thread (no attach used here).
  {
    vlink::Publisher<std::string> pub("dds://status/pub_monitor");
    pub.register_status_handler(
        [](vlink::Status::BasePtr status) { VLOG_I("[PubStatus] type=", static_cast<int>(status->get_type())); });

    vlink::Subscriber<std::string> sub("dds://status/pub_monitor");
    // Empty data handler -- we only care about the matching event firing
    // on the publisher's status handler.
    sub.listen([](const std::string&) {});

    pub.wait_for_subscribers();
    std::this_thread::sleep_for(100ms);
  }

  VLOG_I("Status monitoring example complete.");
  return 0;
}
