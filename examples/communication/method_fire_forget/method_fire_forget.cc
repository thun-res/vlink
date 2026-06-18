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

#include <vlink/base/logger.h>
#include <vlink/vlink.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "log_types.h"

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// method_fire_forget: one-way Method-model (no response).
//
// Demonstrates:
//   - vlink::Server<Req> / vlink::Client<Req>  -- single template parameter,
//     i.e. no response type. listen() callback receives only (const Req&);
//     client.send() returns immediately without awaiting any reply.
//   - Three usage scenarios: logging sink, notification commands, and a
//     high-throughput burst to stress the queue.
//
// Typical scenarios: logs, telemetry events, command broadcast where the
// caller cannot afford to block. Lower overhead than full RPC because no
// reply path is constructed.
int main() {
  vlink::MessageLoop server_loop;
  server_loop.set_name("server_loop");
  server_loop.async_run();

  // ---------- Scenario 1: log collector ----------
  // Server<Req> with only one template arg is the fire-and-forget form.
  // listen() callback signature is (const Req&); there is no resp parameter
  // and the framework does NOT generate a reply path.
  vlink::Server<LogEntry> log_server("dds://logging/collector");
  log_server.attach(&server_loop);

  std::atomic<int> log_count{0};
  // Callback fires on server_loop thread once per incoming LogEntry.
  log_server.listen([&log_count](const LogEntry& entry) {
    static constexpr const char* kLevelStr[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    const char* lvl = (entry.level >= 0 && entry.level <= 3) ? kLevelStr[entry.level] : "?";
    VLOG_I("[log-server] [", lvl, "] source=", entry.source_id, " ts=", entry.timestamp);
    log_count.fetch_add(1);
  });

  // Client<Req> -- send() is the fire-and-forget counterpart to invoke().
  // It returns a bool indicating queue/transport submission success only;
  // it does NOT wait for the server callback to run.
  vlink::Client<LogEntry> log_client("dds://logging/collector");
  log_client.wait_for_connected(2000ms);

  for (int i = 0; i < 5; ++i) {
    LogEntry entry{};
    entry.level = i % 4;
    entry.source_id = 100 + i;
    entry.timestamp = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());

    bool ok = log_client.send(entry);
    VLOG_I("[log-client] sent #", i, " ok=", ok);
  }

  // No reply to await -- only need a brief drain window for the loop to run
  // every callback before we read the counter.
  std::this_thread::sleep_for(200ms);
  VLOG_I("[log-server] received=", log_count.load());

  // ---------- Scenario 2: notification commands ----------
  // Identical pattern, distinct URL. Useful for one-way command broadcast
  // (e.g. "show toast", "rotate key") where there is no meaningful response.
  vlink::Server<NotifyCommand> notify_server("dds://control/notifications");
  notify_server.attach(&server_loop);

  std::atomic<int> cmd_count{0};
  notify_server.listen([&cmd_count](const NotifyCommand& cmd) {
    VLOG_I("[notify-server] cmd=", cmd.command_id, " target=", cmd.target_id, " payload=", cmd.payload);
    cmd_count.fetch_add(1);
  });

  vlink::Client<NotifyCommand> notify_client("dds://control/notifications");
  notify_client.wait_for_connected(2000ms);

  NotifyCommand commands[] = {{1, 10, 100}, {2, 10, 50}, {3, 20, 0}, {4, 30, 1}, {5, 10, 0}};

  for (const auto& cmd : commands) {
    bool ok = notify_client.send(cmd);
    VLOG_I("[notify-client] sent cmd=", cmd.command_id, " ok=", ok);
  }

  std::this_thread::sleep_for(200ms);
  VLOG_I("[notify-server] received=", cmd_count.load());

  // ---------- Scenario 3: high-throughput burst ----------
  // Stress the dispatch queue with kBurstSize tight-loop sends. With
  // fire-and-forget there is no RPC round-trip to throttle the producer,
  // so transient queue overflow is possible -- compare sent vs. received.
  std::atomic<int> burst_count{0};
  vlink::Server<LogEntry> burst_server("dds://telemetry/burst");
  burst_server.attach(&server_loop);
  burst_server.listen([&burst_count](const LogEntry&) { burst_count.fetch_add(1); });

  vlink::Client<LogEntry> burst_client("dds://telemetry/burst");
  burst_client.wait_for_connected(2000ms);

  static constexpr int kBurstSize = 100;
  int send_ok = 0;

  for (int i = 0; i < kBurstSize; ++i) {
    LogEntry entry{1, i, static_cast<int64_t>(i)};

    if (burst_client.send(entry)) {
      ++send_ok;
    }
  }

  // 500ms drain window: enough for the loop to chew through 100 callbacks
  // even under load. If burst_count < send_ok, the transport dropped some.
  std::this_thread::sleep_for(500ms);
  VLOG_I("[burst] sent=", send_ok, "/", kBurstSize, " received=", burst_count.load(), "/", kBurstSize);

  server_loop.quit();
  server_loop.wait_for_quit();

  return 0;
}
