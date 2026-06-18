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

// -----------------------------------------------------------------------------
// Logger basic example
//
// Module:   vlink/base/logger.h
// Scenario: Walk every public logging entry-point shipped with VLink. Four
//           macro families are exercised:
//             VLOG_*   stream-concat style: VLOG_I("x=", x, " y=", y);
//             MLOG_*   {fmt}-style format:   MLOG_I("x={} y={}", x, y);
//             CLOG_*   printf-style:         CLOG_I("x=%d y=%d", x, y);
//             SLOG_*   RAII ostream:         SLOG_I << "x=" << x;
//           All four route through the same backend; pick whichever reads
//           best at the call-site. Console and file levels are independent;
//           set_console_level / set_file_level demote-or-promote at runtime.
// -----------------------------------------------------------------------------
int main() {
  vlink::Logger::init("logger_basic_demo", "/tmp/vlink_logger_basic.log");
  vlink::Logger::set_console_level(vlink::Logger::kTrace);
  vlink::Logger::set_file_level(vlink::Logger::kInfo);

  // VLOG_* -- stream-concat. Each argument is converted via operator<<;
  // works with any type with an ostream overload (incl. Bytes, Uuid, ...).
  VLOG_T("stream [T]: counter=", 0, " name=", "alpha");
  VLOG_D("stream [D]: temperature=", 23.5, " unit=C");
  VLOG_I("stream [I]: application started");
  VLOG_W("stream [W]: disk usage=", 91, "%");
  VLOG_E("stream [E]: failed to open config");

  // MLOG_* -- {fmt}-style. Compile-time format string checking when fmt is
  // built with FMT_ENFORCE_COMPILE_STRING; preferred for typed formatting.
  MLOG_T("format [T]: value={}, label={}", 42, "beta");
  MLOG_D("format [D]: elapsed={}ms", 150);
  MLOG_I("format [I]: connected to host={}, port={}", "192.168.1.1", 8080);
  MLOG_W("format [W]: retry {}/{}", 3, 5);
  MLOG_E("format [E]: timeout after {}ms", 5000);

  // CLOG_* -- printf-style. Useful when porting legacy code; no type safety
  // beyond the compiler's format-string checker.
  CLOG_T("c [T]: index=%d, tag=%s", 7, "gamma");
  CLOG_D("c [D]: ratio=%.2f", 3.14);
  CLOG_I("c [I]: PID=%d started", 12345);
  CLOG_W("c [W]: memory usage=%d%%", 85);
  CLOG_E("c [E]: errno=%d (%s)", 2, "No such file or directory");

  // SLOG_* -- RAII stream that flushes on destruction. Convenient for code
  // that already produces an ostream chain; the temporary destructs at the
  // end of the full-expression (semicolon), so a single SLOG_X << ... is
  // emitted as one log line.
  SLOG_T << "raii [T]: id=" << 100 << " status=ok";
  SLOG_D << "raii [D]: x=" << 1.5 << " y=" << 2.5;
  SLOG_I << "raii [I]: init complete";
  SLOG_W << "raii [W]: connection unstable";
  SLOG_E << "raii [E]: data corruption detected";

  // Dynamic console level: only messages >= the configured level reach the
  // console sink. File sink is independent so we can keep verbose audit
  // logs on disk while keeping the console quiet.
  VLOG_I("--- raising console level to kWarn ---");
  vlink::Logger::set_console_level(vlink::Logger::kWarn);
  VLOG_D("debug suppressed on console");
  VLOG_I("info suppressed on console");
  VLOG_W("warn still shown on console");
  VLOG_E("error still shown on console");

  vlink::Logger::set_console_level(vlink::Logger::kTrace);

  // Dynamic file level: tighten the file sink to errors only; INFO lines
  // still reach the console but no longer get persisted.
  vlink::Logger::set_file_level(vlink::Logger::kError);
  VLOG_I("info -> console only, not file");
  VLOG_E("error -> both console and file");

  vlink::Logger::flush();
  VLOG_I("Logger basic example finished.");
  return 0;
}
