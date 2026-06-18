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

#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Demonstrates the custom-handler hook: every console line is mirrored into
// our own sink (here, an in-memory vector plus a styled std::cout dump).
// Handlers run on whichever thread emitted the log -- so the captured_logs_
// vector is guarded by a mutex.
class CustomLogHandler {
 public:
  void install() {
    vlink::Logger::register_console_handler(
        [this](vlink::Logger::Level level, std::string_view message) { handle(level, message); });
  }

  size_t captured_count() const {
    std::lock_guard lock(mutex_);
    return captured_logs_.size();
  }

 private:
  void handle(vlink::Logger::Level level, std::string_view message) {
    {
      std::lock_guard lock(mutex_);
      captured_logs_.emplace_back(message);
    }

    const char* prefix = "?";
    switch (level) {
      case vlink::Logger::kTrace:
        prefix = "TRACE";
        break;
      case vlink::Logger::kDebug:
        prefix = "DEBUG";
        break;
      case vlink::Logger::kInfo:
        prefix = "INFO ";
        break;
      case vlink::Logger::kWarn:
        prefix = "WARN ";
        break;
      case vlink::Logger::kError:
        prefix = "ERROR";
        break;
      case vlink::Logger::kFatal:
        prefix = "FATAL";
        break;
      default:
        break;
    }

    std::cout << "[CustomHandler][" << prefix << "] " << message << std::endl;
  }

  mutable std::mutex mutex_;
  std::vector<std::string> captured_logs_;
};
// -----------------------------------------------------------------------------
// Logger advanced example
//
// Module:   vlink/base/logger.h
// Scenario: Exercise the post-init customisation points: registering a
//           custom console handler, the backtrace ring buffer (cheap
//           trace-level logging that is only flushed on demand), the FATAL
//           level that throws std::runtime_error after logging, the
//           is_writable level guard for skipping expensive payload
//           formatting, console ANSI toggle and stream-precision tuning.
// CAUTION:  VLOG_F / MLOG_F LOG the message and then THROW
//           std::runtime_error -- never call them in a destructor or noexcept
//           function. The example wraps both in try/catch to demonstrate
//           recovery and to keep the demo running.
// -----------------------------------------------------------------------------
int main() {
  vlink::Logger::init("logger_advanced_demo");
  vlink::Logger::set_console_level(vlink::Logger::kTrace);

  // Install a custom console handler: every console-bound line will also
  // call into handle() above. Useful for forwarding logs into a GUI panel
  // or a remote sink without touching the default file/console pipeline.
  CustomLogHandler handler;
  handler.install();
  VLOG_I("custom handler installed");
  MLOG_D("captured so far: {}", handler.captured_count());

  // Backtrace ring buffer: trace-level lines are buffered in memory (cheap,
  // no formatting) but NOT emitted. dump_backtrace flushes the most recent
  // N lines on demand -- ideal for printing context only when an error hits.
  vlink::Logger::enable_backtrace(10);
  VLOG_T("bt 1: system initializing");
  VLOG_D("bt 2: loading configuration");
  VLOG_I("bt 3: configuration loaded");
  VLOG_D("bt 4: connecting to database");
  VLOG_I("bt 5: database connected");

  VLOG_W("about to dump backtrace...");
  vlink::Logger::dump_backtrace();
  vlink::Logger::disable_backtrace();

  // VLOG_F: log the fatal message AND throw std::runtime_error. Catch is
  // mandatory unless the caller is willing to terminate; the design lets
  // top-level supervisors decide whether to restart, exit, or recover.
  try {
    VLOG_F("fatal: critical subsystem failure, code=", 0xDEAD);
  } catch (const std::runtime_error& e) {
    VLOG_I("caught from VLOG_F: ", e.what());
  }

  try {
    MLOG_F("fatal: op {} status {}", "connect", -1);
  } catch (const std::runtime_error& e) {
    VLOG_I("caught from MLOG_F: ", e.what());
  }

  // Compile-time constants expose the build's level configuration. WARN/
  // ERROR records automatically inject {file:line}; INFO and below do not.
  MLOG_I("kMinimumLevel={} kDetailLevel={}", static_cast<int>(vlink::Logger::kMinimumLevel),
         static_cast<int>(vlink::Logger::kDetailLevel));
  VLOG_W("warn includes {file:line} automatically");
  VLOG_E("error includes {file:line} automatically");

  // is_writable guard: skips the expensive payload preparation when the
  // sink would drop the message anyway. The guard is a single atomic load,
  // far cheaper than constructing the payload string speculatively.
  if (vlink::Logger::is_writable(vlink::Logger::kDebug)) {
    std::string expensive = "computed-only-when-debug-is-active";
    VLOG_D("expensive: ", expensive);
  }

  // ANSI colour toggle: handy when redirecting to a non-tty or when the
  // host terminal does not handle escape sequences.
  vlink::Logger::set_console_fmt_enable(false);
  VLOG_I("ANSI color disabled");
  vlink::Logger::set_console_fmt_enable(true);
  VLOG_I("ANSI color re-enabled");

  // Stream precision controls the float/double formatting of VLOG_* lines.
  // MLOG_* / CLOG_* are unaffected -- they use their own format spec.
  vlink::Logger::set_stream_precision(4);
  VLOG_I("precision 4: pi=", 3.14159265);
  vlink::Logger::set_stream_precision(2);
  VLOG_I("precision 2: pi=", 3.14159265);

  MLOG_I("total captured by custom handler: {}", handler.captured_count());

  vlink::Logger::flush();
  VLOG_I("Logger advanced example finished.");
  return 0;
}
