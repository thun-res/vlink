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
#include <vlink/base/process.h>

#include <string>

// -----------------------------------------------------------------------------
// Process demo
//
// Module:   vlink/base/process.h
// Scenario: vlink::Process is a cross-platform child-process API (a thin
//           equivalent of QProcess). The example walks every common usage:
//             - Synchronous Process::execute (block until child exits).
//             - Async start + wait_for_started / wait_for_finished.
//             - kSeparateMode (capture stdout/stderr) vs kForwardedMode
//               (inherit parent's streams).
//             - Stdin writing via write() + close_write_channel().
//             - Line-by-line reading with can_read_line_stdout.
//             - terminate vs kill (SIGTERM vs SIGKILL on POSIX).
//             - finished callback + start_detached (fire-and-forget child).
// CAUTION:  Most APIs return a bool; ignore the return at your peril (the
//           child may have failed to fork/exec). All wait_for_* methods take
//           a millisecond timeout and return false on timeout.
// -----------------------------------------------------------------------------
int main() {
  VLOG_I("=== VLink Process Demo ===");

  // Process::execute is the simplest entry point: spawn, wait, return
  // exit code. Useful for build-script-style usage where streams are not
  // captured.
  {
    VLOG_I("--- Synchronous execute ---");
    VLOG_I("  echo exit=", vlink::Process::execute("/bin/echo", {"hello", "from", "vlink"}, 5000));
    VLOG_I("  ls /tmp exit=", vlink::Process::execute("/bin/ls", {"/tmp"}, 5000));
    VLOG_I("  bad cmd exit=", vlink::Process::execute("/bin/no_such_command_xyz", {}, 3000));
  }

  // kSeparateMode plumbs stdout/stderr into pipes that the parent can read.
  // wait_for_started ensures fork+exec succeeded before we trust the pid.
  {
    VLOG_I("--- Async stdout capture ---");
    vlink::Process proc;
    proc.set_process_mode(vlink::Process::kSeparateMode);
    proc.start("/bin/echo", {"VLink Process API works!"});

    bool started = proc.wait_for_started(3000);
    VLOG_I("  started=", started ? "yes" : "no");

    if (started) {
      VLOG_I("  pid=", proc.get_process_id());
      proc.wait_for_finished(5000);

      // read_all_output drains the stdout pipe AFTER the child exits.
      std::string output;
      proc.read_all_output(output);
      VLOG_I("  stdout: ", output);
      VLOG_I("  exit_code=", proc.get_exit_code(),
             " exit_status=", proc.get_exit_status() == vlink::Process::kNormalExitStatus ? "normal" : "crash");
    }
  }

  // Separate stderr: shell prints to both streams; we drain both pipes
  // independently. read_all_error mirrors read_all_output but for stderr.
  {
    VLOG_I("--- Separate stderr ---");
    vlink::Process proc;
    proc.set_process_mode(vlink::Process::kSeparateMode);
    proc.start("/bin/sh", {"-c", "echo stdout_msg && echo stderr_msg >&2"});
    proc.wait_for_started(3000);
    proc.wait_for_finished(5000);

    std::string out;
    std::string err;
    proc.read_all_output(out);
    proc.read_all_error(err);
    VLOG_I("  stdout: ", out);
    VLOG_I("  stderr: ", err);
    VLOG_I("  exit_code=", proc.get_exit_code());
  }

  // Stdin writing: feed bytes to the child, then close_write_channel to
  // send EOF -- otherwise /bin/cat would block forever waiting for more.
  {
    VLOG_I("--- Stdin write (pipe) ---");
    vlink::Process proc;
    proc.set_process_mode(vlink::Process::kSeparateMode);
    proc.start("/bin/cat", {});
    proc.wait_for_started(3000);

    std::string input = "Hello from parent via pipe!\n";
    VLOG_I("  bytes written=", proc.write(input));
    // Without this close the child waits for more stdin and never exits.
    proc.close_write_channel();
    proc.wait_for_finished(5000);

    std::string output;
    proc.read_all_output(output);
    VLOG_I("  cat echoed: ", output);
  }

  // Line-by-line reading: useful for streaming output of long-running
  // commands. can_read_line_stdout returns true while a complete line is
  // available in the buffer.
  {
    VLOG_I("--- Line-by-line reading ---");
    vlink::Process proc;
    proc.set_process_mode(vlink::Process::kSeparateMode);
    proc.start("/bin/sh", {"-c", "echo line1 && echo line2 && echo line3"});
    proc.wait_for_started(3000);
    proc.wait_for_finished(5000);

    int n = 0;
    std::string line;
    while (proc.can_read_line_stdout()) {
      proc.read_line_stdout(line);
      ++n;
      VLOG_I("  line ", n, ": ", line);
    }

    VLOG_I("  total lines=", n);
  }

  // terminate (SIGTERM) gives the child a chance to clean up. If it
  // refuses to exit within the timeout, escalate to kill (SIGKILL) which
  // is uncatchable. Always follow terminate with a bounded wait.
  {
    VLOG_I("--- terminate / kill ---");
    vlink::Process proc;
    proc.set_process_mode(vlink::Process::kForwardedMode);
    proc.start("/bin/sleep", {"30"});
    proc.wait_for_started(3000);
    VLOG_I("  started pid=", proc.get_process_id(), " running=", proc.is_running());

    proc.terminate();
    bool finished = proc.wait_for_finished(3000);
    VLOG_I("  after terminate: finished=", finished);

    if (!finished) {
      proc.kill();
      finished = proc.wait_for_finished(3000);
      VLOG_I("  after kill: finished=", finished);
    }

    VLOG_I("  final running=", proc.is_running(), " exit_code=", proc.get_exit_code());
  }

  // finished_callback fires when the child exits (from the SIGCHLD reaper
  // thread, NOT the caller's thread -- keep the body short and thread-safe).
  // start_detached forks a fully detached child whose lifetime is no
  // longer tracked -- ideal for fire-and-forget launches.
  {
    VLOG_I("--- Callbacks + detached ---");
    vlink::Process proc;
    proc.set_process_mode(vlink::Process::kSeparateMode);
    proc.register_finished_callback([](int code, vlink::Process::ExitStatus status) {
      VLOG_I("  [cb] finished code=", code,
             " status=", status == vlink::Process::kNormalExitStatus ? "normal" : "crash");
    });
    proc.start("/bin/echo", {"callback test"});
    proc.wait_for_finished(5000);

    VLOG_I("  start_detached(/bin/true)=", vlink::Process::start_detached("/bin/true", {}) ? "ok" : "fail");
  }

  VLOG_I("Process demo completed.");
  vlink::Logger::flush();
  return 0;
}
