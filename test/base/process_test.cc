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

// NOLINTBEGIN

#include "./base/process.h"

#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../common_test.h"

TEST_SUITE("base-Process") {
  TEST_CASE("initial state of freshly constructed process object") {
    Process proc;
    CHECK_EQ(proc.get_state(), Process::kNotRunningState);
    CHECK_EQ(proc.get_error(), Process::kNoError);
    CHECK_EQ(proc.get_exit_code(), -1);
    CHECK_FALSE(proc.is_running());
#ifndef _WIN32
    CHECK_EQ(proc.get_process_id(), -1);
#endif
  }

  TEST_CASE("enum constant values match documented integers") {
    CHECK_EQ(static_cast<int>(Process::kNotRunningState), 0);
    CHECK_EQ(static_cast<int>(Process::kStartingState), 1);
    CHECK_EQ(static_cast<int>(Process::kRunningState), 2);

    CHECK_EQ(static_cast<int>(Process::kNormalExitStatus), 0);
    CHECK_EQ(static_cast<int>(Process::kCrashExitStatus), 1);

    CHECK_EQ(static_cast<int>(Process::kNoError), 0);
    CHECK_EQ(static_cast<int>(Process::kUnknownError), 1);
    CHECK_EQ(static_cast<int>(Process::kStartError), 2);

    CHECK_EQ(static_cast<int>(Process::kSeparateMode), 0);
    CHECK_EQ(static_cast<int>(Process::kMergedMode), 1);
    CHECK_EQ(static_cast<int>(Process::kForwardedMode), 2);
    CHECK_EQ(static_cast<int>(Process::kForwardedOutputMode), 3);
    CHECK_EQ(static_cast<int>(Process::kForwardedErrorMode), 4);
  }

  TEST_CASE("kInfinite sentinel value is negative one") { CHECK_EQ(Process::kInfinite, -1); }

  TEST_CASE("default timeout constants have expected values") {
    CHECK_EQ(Process::kDefaultWaitTimeoutMs, 3000);
    CHECK_EQ(Process::kDefaultWriteTimeoutMs, 5000);
    CHECK_EQ(Process::kDefaultExecuteTimeoutMs, 30000);
    CHECK_EQ(Process::kDestructorWaitTimeoutMs, 5000);
  }

  TEST_CASE("set and get process mode are consistent") {
    Process proc;

    proc.set_process_mode(Process::kSeparateMode);
    CHECK_EQ(proc.get_process_mode(), Process::kSeparateMode);

    proc.set_process_mode(Process::kMergedMode);
    CHECK_EQ(proc.get_process_mode(), Process::kMergedMode);

    proc.set_process_mode(Process::kForwardedMode);
    CHECK_EQ(proc.get_process_mode(), Process::kForwardedMode);
  }

  TEST_CASE("inherit environment flag default is false and is settable") {
    Process proc;
    CHECK_FALSE(proc.get_inherit_environment());

    proc.set_inherit_environment(false);
    CHECK_FALSE(proc.get_inherit_environment());

    proc.set_inherit_environment(true);
    CHECK(proc.get_inherit_environment());
  }

  TEST_CASE("set and get environment map preserves entries") {
    Process proc;
    Process::EnvironmentMap env{{"MY_VAR", "hello"}, {"OTHER", "42"}};
    proc.set_environment(env);

    auto got = proc.get_environment();
    CHECK_EQ(got["MY_VAR"], "hello");
    CHECK_EQ(got["OTHER"], "42");
  }

  TEST_CASE("set and get working directory are consistent") {
    Process proc;
    std::string tmp_dir = std::filesystem::temp_directory_path().string();
    proc.set_working_directory(tmp_dir);
    CHECK_EQ(proc.get_working_directory(), tmp_dir);
  }

  TEST_CASE("set and get max buffer size are consistent") {
    Process proc;
    proc.set_max_buffer_size(65536);
    CHECK_EQ(proc.get_max_buffer_size(), 65536u);

    proc.set_max_buffer_size(1024 * 1024);
    CHECK_EQ(proc.get_max_buffer_size(), 1024u * 1024u);
  }

#if defined(__linux__) && defined(__x86_64__)

  TEST_CASE("execute true returns zero exit code") {
    int code = Process::execute("/bin/true", {}, 5000);
    CHECK_EQ(code, 0);
  }

  TEST_CASE("execute false returns non-zero exit code") {
    int code = Process::execute("/bin/false", {}, 5000);
    CHECK_NE(code, 0);
  }

  TEST_CASE("execute with arguments honours exit code") {
    int code = Process::execute("/bin/sh", {"-c", "exit 42"}, 5000);
    CHECK_EQ(code, 42);
  }

  TEST_CASE("start and wait_for_finished complete for /bin/true") {
    Process proc;
    proc.set_process_mode(Process::kForwardedMode);

    proc.start("/bin/true");
    bool started = proc.wait_for_started(3000);
    CHECK(started);

    bool finished = proc.wait_for_finished(3000);
    CHECK(finished);

    CHECK_EQ(proc.get_state(), Process::kNotRunningState);
    CHECK_EQ(proc.get_exit_status(), Process::kNormalExitStatus);
    CHECK_EQ(proc.get_exit_code(), 0);
  }

  TEST_CASE("start_command parses command string and runs it") {
    Process proc;
    proc.set_process_mode(Process::kForwardedMode);

    proc.start_command("/bin/true");
    bool finished = proc.wait_for_finished(3000);
    CHECK(finished);
    CHECK_EQ(proc.get_exit_code(), 0);
  }

  TEST_CASE("read_all_output contains stdout from echo") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/echo", {"hello"});
    REQUIRE(proc.wait_for_finished(3000));

    std::string output;
    proc.read_all_output(output);
    CHECK_FALSE(output.empty());
    CHECK(output.find("hello") != std::string::npos);
  }

  TEST_CASE("read_all_output into vector is non-empty after echo") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/echo", {"data"});
    proc.wait_for_finished(3000);

    std::vector<uint8_t> buf;
    proc.read_all_output(buf);
    CHECK_FALSE(buf.empty());
  }

  TEST_CASE("read_line_stdout returns a line after echo") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/echo", {"line1"});
    REQUIRE(proc.wait_for_finished(3000));

    if (proc.can_read_line_stdout()) {
      std::string line;
      bool ok = proc.read_line_stdout(line);
      CHECK(ok);
      CHECK_FALSE(line.empty());
    }
  }

  TEST_CASE("process id is positive while running and resets to -1 after") {
    Process proc;
    proc.set_process_mode(Process::kForwardedMode);
    CHECK_EQ(proc.get_process_id(), -1);

    proc.start("/bin/sleep", {"0.1"});
    REQUIRE(proc.wait_for_started(3000));

    int64_t pid = proc.get_process_id();
    CHECK(pid > 0);

    proc.wait_for_finished(3000);
    CHECK_EQ(proc.get_process_id(), -1);
  }

  TEST_CASE("state_changed callback fires during process lifecycle") {
    Process proc;
    proc.set_process_mode(Process::kForwardedMode);

    std::atomic<int> change_count{0};
    proc.register_state_changed_callback([&](Process::State) { change_count.fetch_add(1, std::memory_order_relaxed); });

    proc.start("/bin/true");
    proc.wait_for_finished(3000);

    CHECK(change_count.load() >= 1);
  }

  TEST_CASE("finished callback fires with exit code zero") {
    Process proc;
    proc.set_process_mode(Process::kForwardedMode);

    std::atomic<int> exit_code{-1};
    std::atomic<int> exit_status{-1};
    proc.register_finished_callback([&](int code, Process::ExitStatus status) {
      exit_code.store(code, std::memory_order_relaxed);
      exit_status.store(static_cast<int>(status), std::memory_order_relaxed);
    });

    proc.start("/bin/true");
    proc.wait_for_finished(3000);
    std::this_thread::sleep_for(100ms);

    CHECK_EQ(exit_code.load(), 0);
    CHECK_EQ(exit_status.load(), static_cast<int>(Process::kNormalExitStatus));
  }

  TEST_CASE("starting non-existent program leaves process not running") {
    Process proc;
    proc.set_process_mode(Process::kForwardedMode);

    proc.start("/nonexistent_program_xyz_vlink_test");
    proc.wait_for_finished(3000);

    CHECK_FALSE(proc.is_running());
  }

  TEST_CASE("start_detached returns true for a valid program") {
    bool ok = Process::start_detached("/bin/true");
    CHECK(ok);
  }

  TEST_CASE("start_detached returns false for a non-existent program") {
    bool ok = Process::start_detached("/nonexistent_program_xyz_vlink_test");
    CHECK_FALSE(ok);
  }

  TEST_CASE("start_detached with arguments returns true") {
    bool ok = Process::start_detached("/bin/sleep", {"0"});
    CHECK(ok);
  }

  TEST_CASE("start with custom path without inherit environment exits zero") {
    Process proc;
    proc.set_process_mode(Process::kForwardedMode);
    proc.set_inherit_environment(false);
    proc.set_environment({{"PATH", "/bin:/usr/bin"}});

    proc.start("sh", {"-c", "exit 0"});
    CHECK(proc.wait_for_finished(3000));
    CHECK_EQ(proc.get_exit_code(), 0);
  }

  TEST_CASE("kill terminates a running process") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/sleep", {"60"});
    REQUIRE(proc.wait_for_started(3000));
    CHECK(proc.is_running());

    proc.kill();
    bool finished = proc.wait_for_finished(3000);
    CHECK(finished);
    CHECK_FALSE(proc.is_running());
  }

  TEST_CASE("close force-terminates a running process") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/sleep", {"60"});
    REQUIRE(proc.wait_for_started(3000));

    proc.close(true);
    CHECK_FALSE(proc.is_running());
  }

  TEST_CASE("write to stdin is echoed by cat") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/cat", {});
    REQUIRE(proc.wait_for_started(3000));

    std::string msg = "hello_stdin\n";
    size_t written = proc.write(msg, 3000);
    CHECK_EQ(written, msg.size());

    proc.close_write_channel();
    proc.wait_for_finished(3000);

    std::string output;
    proc.read_all_output(output);
    CHECK(output.find("hello_stdin") != std::string::npos);
  }

  TEST_CASE("exit_status is kNormalExitStatus for a non-zero clean exit") {
    Process proc;
    proc.set_process_mode(Process::kForwardedMode);

    proc.start("/bin/sh", {"-c", "exit 2"});
    proc.wait_for_finished(3000);

    CHECK_EQ(proc.get_exit_code(), 2);
    CHECK_EQ(proc.get_exit_status(), Process::kNormalExitStatus);
  }

  TEST_CASE("read_all_error contains stderr from sh redirect") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/sh", {"-c", "echo stderr_msg >&2"});
    proc.wait_for_finished(3000);

    std::string err_out;
    proc.read_all_error(err_out);
    CHECK_FALSE(err_out.empty());
    CHECK(err_out.find("stderr_msg") != std::string::npos);
  }

  TEST_CASE("kMergedMode read_all contains merged output") {
    Process proc;
    proc.set_process_mode(Process::kMergedMode);

    proc.start("/bin/sh", {"-c", "echo merged_line"});
    proc.wait_for_finished(3000);

    std::string output;
    proc.read_all(output);
    CHECK_FALSE(output.empty());
    CHECK(output.find("merged_line") != std::string::npos);
    CHECK_EQ(proc.get_error(), Process::kNoError);
  }

  TEST_CASE("bytes_available_stdout is non-zero after echo output") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/echo", {"available_test"});
    proc.wait_for_finished(3000);

    size_t avail = proc.bytes_available_stdout();
    CHECK(avail > 0u);
  }

  TEST_CASE("read_stdout into vector returns non-empty result") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/echo", {"read_vector"});
    proc.wait_for_finished(3000);

    std::vector<uint8_t> buf;
    size_t n = proc.read_stdout(buf, 64u);
    CHECK(n > 0u);
    CHECK_EQ(buf.size(), n);
  }

  TEST_CASE("ready_read_stdout callback fires at least once") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    std::atomic<int> called{0};
    proc.register_ready_read_stdout_callback([&called]() { called.fetch_add(1, std::memory_order_relaxed); });

    proc.start("/bin/echo", {"callback_test"});
    proc.wait_for_finished(3000);
    std::this_thread::sleep_for(100ms);

    CHECK(called.load() >= 1);
  }

  TEST_CASE("error callback fires when start fails") {
    Process proc;
    proc.set_process_mode(Process::kForwardedMode);

    std::atomic<bool> error_called{false};
    proc.register_error_callback(
        [&error_called](Process::Error) { error_called.store(true, std::memory_order_relaxed); });

    proc.start("/nonexistent_binary_vlink_xyz_test");
    proc.wait_for_finished(3000);
    std::this_thread::sleep_for(200ms);

    CHECK(error_called.load());
  }

  TEST_CASE("kForwardedOutputMode buffers stderr while forwarding stdout") {
    Process proc;
    proc.set_process_mode(Process::kForwardedOutputMode);

    proc.start("/bin/sh", {"-c", "echo out; echo err >&2"});
    bool finished = proc.wait_for_finished(3000);
    CHECK(finished);
    CHECK_EQ(proc.get_exit_code(), 0);

    std::string err;
    proc.read_all_error(err);
    CHECK(err.find("err") != std::string::npos);
  }

  TEST_CASE("kForwardedErrorMode buffers stdout while forwarding stderr") {
    Process proc;
    proc.set_process_mode(Process::kForwardedErrorMode);

    proc.start("/bin/sh", {"-c", "echo out; echo err >&2"});
    bool finished = proc.wait_for_finished(3000);
    CHECK(finished);
    CHECK_EQ(proc.get_exit_code(), 0);

    std::string out;
    proc.read_all_output(out);
    CHECK(out.find("out") != std::string::npos);
  }

  TEST_CASE("buffer overflow error set when output exceeds max_buffer_size") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);
    proc.set_max_buffer_size(1024);

    proc.start("/bin/sh", {"-c", "printf abcdef; sleep 0.5; printf ghij"});
    REQUIRE(proc.wait_for_ready_read(2000));
    const size_t first_size = proc.bytes_available_stdout();
    REQUIRE(first_size > 1u);

    proc.set_max_buffer_size(first_size - 1u);
    CHECK(proc.wait_for_finished(3000));

    CHECK_EQ(proc.bytes_available_stdout(), first_size);
    CHECK_EQ(proc.get_error(), Process::kBufferOverflowError);
  }

  TEST_CASE("exact max buffer fill is not reported as overflow") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);
    proc.set_max_buffer_size(6);

    proc.start("/bin/sh", {"-c", "printf abcdef"});
    CHECK(proc.wait_for_finished(3000));

    std::string output;
    proc.read_all_output(output);
    CHECK_EQ(output, "abcdef");
    CHECK_NE(proc.get_error(), Process::kBufferOverflowError);
  }

  TEST_CASE("write vector overload echoed by cat") {
    Process proc;
    proc.start("/bin/cat", {});

    std::vector<uint8_t> input_data = {'h', 'e', 'l', 'l', 'o', '\n'};
    auto wrote_size = proc.write(input_data, 2000);
    if (wrote_size > 0) {
      proc.close_write_channel();
      proc.wait_for_finished(3000);
      std::string output;
      proc.read_all_output(output);
      CHECK(output.find("hello") != std::string::npos);
    } else {
      proc.kill();
      proc.wait_for_finished(1000);
    }
  }

  TEST_CASE("ready_read callback can read buffered output") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    std::atomic<bool> read_in_callback{false};
    std::string output;
    std::mutex output_mtx;

    proc.register_ready_read_stdout_callback([&]() {
      std::string local;
      if (proc.read_all_output(local)) {
        std::lock_guard lock(output_mtx);
        output += local;
        read_in_callback.store(true, std::memory_order_release);
      }
    });

    proc.start("/bin/sh", {"-c", "echo callback_read"});
    CHECK(proc.wait_for_finished(3000));

    for (int i = 0; i < 50 && !read_in_callback.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(10ms);
    }

    std::lock_guard lock(output_mtx);
    CHECK(output.find("callback_read") != std::string::npos);
  }

#endif
}

// NOLINTEND
