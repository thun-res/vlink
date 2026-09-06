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
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../common_test.h"
#include "./base/utils.h"

#ifdef __linux__
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#endif

#if defined(__SANITIZE_ADDRESS__)
#define VLINK_TEST_ADDRESS_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define VLINK_TEST_ADDRESS_SANITIZER 1
#endif
#endif

#ifndef VLINK_TEST_ADDRESS_SANITIZER
#define VLINK_TEST_ADDRESS_SANITIZER 0
#endif

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
    std::string tmp_dir = Utils::get_tmp_dir();
    proc.set_working_directory(tmp_dir);
    CHECK_EQ(proc.get_working_directory(), tmp_dir);
  }

  TEST_CASE("set and get max buffer size are consistent") {
    Process proc;
    proc.set_max_buffer_size(65536);
    CHECK_EQ(proc.get_max_buffer_size(), 65536u);

    proc.set_max_buffer_size(1024 * 1024);
    CHECK_EQ(proc.get_max_buffer_size(), 1024u * 1024u);

    proc.set_max_buffer_size(0);
    CHECK(proc.get_max_buffer_size() >= 1024u * 1024u);
  }

  TEST_CASE("idle process read APIs return empty results") {
    Process proc;
    std::string text = "unchanged";
    std::vector<uint8_t> data = {1, 2, 3};

    CHECK_EQ(proc.bytes_available_stdout(), 0u);
    CHECK_EQ(proc.bytes_available_stderr(), 0u);
    CHECK_FALSE(proc.can_read_line_stdout());
    CHECK_FALSE(proc.can_read_line_stderr());

    CHECK_FALSE(proc.read_line_stdout(text));
    CHECK(text.empty());
    text = "unchanged";
    CHECK_FALSE(proc.read_line_stderr(text));
    CHECK(text.empty());

    CHECK_EQ(proc.read_stdout(data, 10), 0u);
    CHECK(data.empty());
    data = {1, 2, 3};
    CHECK_EQ(proc.read_stderr(data, 10), 0u);
    CHECK(data.empty());

    data = {1, 2, 3};
    CHECK_FALSE(proc.read_all_output(data));
    CHECK(data.empty());
    data = {1, 2, 3};
    CHECK_FALSE(proc.read_all_error(data));
    CHECK(data.empty());
    data = {1, 2, 3};
    CHECK_FALSE(proc.read_all(data));
    CHECK(data.empty());

    text = "unchanged";
    CHECK_FALSE(proc.read_all_output(text));
    CHECK(text.empty());
    text = "unchanged";
    CHECK_FALSE(proc.read_all_error(text));
    CHECK(text.empty());
    text = "unchanged";
    CHECK_FALSE(proc.read_all(text));
    CHECK(text.empty());

    CHECK_EQ(proc.write(std::string("unused"), 0), 0u);
    CHECK_EQ(proc.write(std::vector<uint8_t>{1, 2, 3}, 0), 0u);
  }

  TEST_CASE("start current test binary with invalid working directory reports start error") {
    Process proc;
    proc.set_process_mode(Process::kForwardedMode);
    const auto missing_dir =
        (std::filesystem::path(Utils::get_tmp_dir()) / ("vlink_process_missing_" + Utils::get_pid_str())).string();
    proc.set_working_directory(missing_dir);

    proc.start(Utils::get_app_path(),
               {"--test-suite=base-Process", "--test-case=__vlink_no_such_process_case__", "--no-version"});
    REQUIRE(proc.wait_for_finished(Process::kDefaultWaitTimeoutMs));
    CHECK_EQ(proc.get_error(), Process::kStartError);
    CHECK_FALSE(proc.is_running());
  }

#if defined(__linux__) && defined(__x86_64__)

  TEST_CASE("execute true returns zero exit code") {
    int code = Process::execute("/bin/true", {}, 5000);
    CHECK_EQ(code, 0);
  }

  TEST_CASE("execute false returns non-zero exit code") {
    int code = Process::execute("/bin/false", {}, 5000);
    CHECK_EQ(code, 1);
  }

  TEST_CASE("execute with arguments honours exit code") {
    int code = Process::execute("/bin/sh", {"-c", "exit 42"}, 5000);
    CHECK_EQ(code, 42);
  }

  TEST_CASE("start and wait_for_finished complete for /bin/sleep") {
    Process proc;
    proc.set_process_mode(Process::kForwardedMode);

    proc.start("/bin/sleep", {"1"});
    bool started = proc.wait_for_started(3000);
    REQUIRE(started);

    bool finished = proc.wait_for_finished(3000);
    REQUIRE(finished);

    CHECK_EQ(proc.get_state(), Process::kNotRunningState);
    CHECK_EQ(proc.get_exit_status(), Process::kNormalExitStatus);
    CHECK_EQ(proc.get_exit_code(), 0);
  }

  TEST_CASE("start_command parses command string and runs it") {
    Process proc;
    proc.set_process_mode(Process::kForwardedMode);

    proc.start_command("/bin/true");
    bool finished = proc.wait_for_finished(3000);
    REQUIRE(finished);
    CHECK_EQ(proc.get_exit_code(), 0);
  }

  TEST_CASE("start_command with empty command records a start error") {
    Process proc;
    proc.start_command("");
    CHECK_EQ(proc.get_error(), Process::kStartError);
    CHECK_FALSE(proc.is_running());
  }

  TEST_CASE("start_command preserves quoted empty positional arguments") {
    Process proc;
    proc.start_command(
        "/bin/sh -c 'printf \"%s|%s|%s|%s|%s\" \"$#\" \"$1\" \"$2\" \"$3\" \"$4\"' sh \"\" middle '' \"\"");
    REQUIRE(proc.wait_for_finished(3000));

    std::string output;
    REQUIRE(proc.read_all_output(output));
    CHECK_EQ(output, "4||middle||");
  }

  TEST_CASE("active reads report truncation and invoke the error callback outside the buffer lock") {
    for (int reader = 0; reader < 10; ++reader) {
      Process proc;
      proc.set_max_buffer_size(4);
      int error_count = 0;
      Process::Error reported_error = Process::kNoError;
      proc.register_error_callback([&](Process::Error error) {
        reported_error = error;
        ++error_count;
        CHECK(proc.bytes_available_stdout() <= 4u);
        CHECK(proc.bytes_available_stderr() <= 4u);
      });
      proc.register_state_changed_callback([&](Process::State state) {
        if (state != Process::kRunningState) {
          return;
        }

        siginfo_t info{};
        int ret;
        do {
          ret = ::waitid(P_PID, static_cast<id_t>(proc.get_process_id()), &info, WEXITED | WNOWAIT);
        } while (ret < 0 && errno == EINTR);
        REQUIRE_EQ(ret, 0);

        std::string text;
        std::vector<uint8_t> bytes;
        switch (reader) {
          case 0:
            CHECK(proc.read_line_stdout(text));
            break;
          case 1:
            CHECK(proc.read_line_stderr(text));
            break;
          case 2:
            CHECK_EQ(proc.read_stdout(bytes, 4), 4u);
            break;
          case 3:
            CHECK_EQ(proc.read_stderr(bytes, 4), 4u);
            break;
          case 4:
            CHECK(proc.read_all_output(bytes));
            break;
          case 5:
            CHECK(proc.read_all_error(bytes));
            break;
          case 6:
            CHECK(proc.read_all(bytes));
            break;
          case 7:
            CHECK(proc.read_all_output(text));
            break;
          case 8:
            CHECK(proc.read_all_error(text));
            break;
          case 9:
            CHECK(proc.read_all(text));
            break;
        }
        CHECK_EQ(proc.get_error(), Process::kBufferOverflowError);
        CHECK_EQ(reported_error, Process::kBufferOverflowError);
        CHECK_EQ(error_count, 1);
      });
      proc.start("/bin/sh", {"-c", "printf abcdefgh; printf 12345678 >&2"});
      REQUIRE(proc.wait_for_finished(3000));
      proc.close();
      CHECK_EQ(error_count, 1);
    }
  }

  TEST_CASE("partial output reads preserve unread prefixes across string and vector drains") {
    Process proc;
    proc.start("/bin/sh", {"-c", "printf 'a\\nb\\nc\\n'; printf '1\\n2\\n3\\n' >&2"});
    REQUIRE(proc.wait_for_finished(3000));
    std::string line;
    REQUIRE(proc.read_line_stdout(line));
    CHECK_EQ(line, "a\n");
    REQUIRE(proc.read_line_stderr(line));
    CHECK_EQ(line, "1\n");
    CHECK_EQ(proc.bytes_available_stdout(), 4u);
    CHECK_EQ(proc.bytes_available_stderr(), 4u);
    CHECK(proc.can_read_line_stdout());
    CHECK(proc.can_read_line_stderr());

    std::vector<uint8_t> part;
    CHECK_EQ(proc.read_stdout(part, 1), 1u);
    CHECK_EQ(part, std::vector<uint8_t>{'b'});
    CHECK_EQ(proc.read_stderr(part, 1), 1u);
    CHECK_EQ(part, std::vector<uint8_t>{'2'});

    SUBCASE("combined vector") {
      REQUIRE(proc.read_all(part));
      CHECK_EQ(std::string(part.begin(), part.end()), "\nc\n\n3\n");
    }
    SUBCASE("combined string") {
      REQUIRE(proc.read_all(line));
      CHECK_EQ(line, "\nc\n\n3\n");
    }
    SUBCASE("separate vectors") {
      REQUIRE(proc.read_all_output(part));
      CHECK_EQ(std::string(part.begin(), part.end()), "\nc\n");
      REQUIRE(proc.read_all_error(part));
      CHECK_EQ(std::string(part.begin(), part.end()), "\n3\n");
    }
    SUBCASE("separate strings") {
      REQUIRE(proc.read_all_output(line));
      CHECK_EQ(line, "\nc\n");
      REQUIRE(proc.read_all_error(line));
      CHECK_EQ(line, "\n3\n");
    }
    CHECK_EQ(proc.bytes_available_stdout(), 0u);
    CHECK_EQ(proc.bytes_available_stderr(), 0u);
  }

  TEST_CASE("consumed output capacity can hold later data without false truncation") {
    Process proc;
    proc.set_max_buffer_size(8);
    proc.start("/bin/cat");
    REQUIRE_EQ(proc.write("abcdef"), 6u);
    REQUIRE(common_test::wait_until([&] { return proc.bytes_available_stdout() == 6u; }, 1000ms));

    std::vector<uint8_t> part;
    REQUIRE_EQ(proc.read_stdout(part, 4), 4u);
    CHECK_EQ(std::string(part.begin(), part.end()), "abcd");
    REQUIRE_EQ(proc.write("ghijkl"), 6u);
    REQUIRE(common_test::wait_until([&] { return proc.bytes_available_stdout() == 8u; }, 1000ms));
    proc.close_write_channel();
    REQUIRE(proc.wait_for_finished(3000));

    std::string output;
    REQUIRE(proc.read_all_output(output));
    CHECK_EQ(output, "efghijkl");
    CHECK_EQ(proc.get_error(), Process::kNoError);
  }

  TEST_CASE("child descriptor cleanup closes unrelated inherited descriptors") {
    int original = ::open("/dev/null", O_RDONLY);
    REQUIRE(original >= 0);
    const int inherited = ::fcntl(original, F_DUPFD, 100);
    ::close(original);
    REQUIRE(inherited >= 100);

    Process proc;
    proc.start("/bin/sh", {"-c", "test ! -e /proc/self/fd/" + std::to_string(inherited)});
    ::close(inherited);
    REQUIRE(proc.wait_for_finished(3000));
    CHECK_EQ(proc.get_exit_code(), 0);
  }

  TEST_CASE("wait helpers return immediately for a process that was never started") {
    Process proc;

    CHECK_FALSE(proc.wait_for_started(0));
    CHECK(proc.wait_for_finished(0));
    CHECK_FALSE(proc.wait_for_ready_read(0));
  }

  TEST_CASE("start_command handles quotes and escaped whitespace") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start_command("/bin/echo \"two words\" escaped\\ value");
    REQUIRE(proc.wait_for_finished(3000));

    std::string output;
    proc.read_all_output(output);
    CHECK(output.find("two words") != std::string::npos);
    CHECK(output.find("escaped value") != std::string::npos);
  }

  TEST_CASE("start_command preserves common escape sequences") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start_command("/bin/printf line\\nnext\\tvalue");
    REQUIRE(proc.wait_for_finished(3000));

    std::string output;
    proc.read_all_output(output);
    CHECK(output.find("line\nnext\tvalue") != std::string::npos);
  }

  TEST_CASE("start_command preserves quote slash carriage-return and unknown escapes") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start_command("/bin/printf %s \"a\\rb\\\\c\\\"d\\'e\\q\"");
    REQUIRE(proc.wait_for_finished(3000));

    std::string output;
    proc.read_all_output(output);
    CHECK(output.find("a\r") != std::string::npos);
    CHECK(output.find("b\\c\"d'e\\q") != std::string::npos);
  }

  TEST_CASE("start_command preserves single quotes and trailing escapes") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start_command("/bin/echo 'single quoted' trailing\\");
    REQUIRE(proc.wait_for_finished(3000));

    std::string output;
    proc.read_all_output(output);
    CHECK(output.find("single quoted") != std::string::npos);
    CHECK(output.find("trailing\\") != std::string::npos);
  }

  TEST_CASE("start_command with only whitespace records a start error") {
    Process proc;
    proc.start_command(" \t  ");
    CHECK_EQ(proc.get_error(), Process::kStartError);
    CHECK_FALSE(proc.is_running());
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
    REQUIRE(proc.wait_for_finished(3000));

    std::vector<uint8_t> buf;
    proc.read_all_output(buf);
    CHECK_FALSE(buf.empty());
  }

  TEST_CASE("read_line_stdout returns a line after echo") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/echo", {"line1"});
    REQUIRE(proc.wait_for_finished(3000));

    REQUIRE(proc.can_read_line_stdout());

    std::string line;
    bool ok = proc.read_line_stdout(line);
    CHECK(ok);
    CHECK_FALSE(line.empty());
  }

  TEST_CASE("read_line_stdout returns buffered data without a newline") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/sh", {"-c", "printf no_newline"});
    REQUIRE(proc.wait_for_finished(3000));

    std::string line;
    CHECK(proc.read_line_stdout(line));
    CHECK_EQ(line, "no_newline");
  }

  TEST_CASE("process id is positive while running and resets to -1 after") {
    Process proc;
    proc.set_process_mode(Process::kForwardedMode);
    CHECK_EQ(proc.get_process_id(), -1);

    proc.start("/bin/sleep", {"1"});
    REQUIRE(proc.wait_for_started(3000));

    int64_t pid = proc.get_process_id();
    CHECK(pid > 0);

    REQUIRE(proc.wait_for_finished(3000));
    CHECK_EQ(proc.get_process_id(), -1);
  }

  TEST_CASE("state_changed callback fires during process lifecycle") {
    Process proc;
    proc.set_process_mode(Process::kForwardedMode);

    std::atomic<int> change_count{0};
    proc.register_state_changed_callback([&](Process::State) { change_count.fetch_add(1, std::memory_order_relaxed); });

    proc.start("/bin/true");
    REQUIRE(proc.wait_for_finished(3000));

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
    REQUIRE(proc.wait_for_finished(3000));
    REQUIRE(common_test::wait_until(
        [&exit_code, &exit_status] {
          return exit_code.load(std::memory_order_relaxed) == 0 &&
                 exit_status.load(std::memory_order_relaxed) == static_cast<int>(Process::kNormalExitStatus);
        },
        500ms));

    CHECK_EQ(exit_code.load(), 0);
    CHECK_EQ(exit_status.load(), static_cast<int>(Process::kNormalExitStatus));
  }

  TEST_CASE("starting non-existent program leaves process not running") {
    Process proc;
    proc.set_process_mode(Process::kForwardedMode);

    std::filesystem::path missing_program =
        std::filesystem::path(vlink::Utils::get_tmp_dir()) / "nonexistent_program_xyz_vlink_test";
    proc.start(missing_program.string());
    REQUIRE(proc.wait_for_finished(3000));

    CHECK_FALSE(proc.is_running());
  }

  TEST_CASE("starting while already running reports a start error") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/sleep", {"1"});
    REQUIRE(proc.wait_for_started(3000));
    proc.start("/bin/true");
    CHECK_EQ(proc.get_error(), Process::kStartError);

    proc.kill();
    REQUIRE(proc.wait_for_finished(3000));
  }

  TEST_CASE("start_detached returns true for a valid program") {
    bool ok = Process::start_detached("/bin/true");
    CHECK(ok);
  }

  TEST_CASE("start_detached returns false for a non-existent program") {
    std::filesystem::path missing_program =
        std::filesystem::path(vlink::Utils::get_tmp_dir()) / "nonexistent_program_xyz_vlink_test";
    bool ok = Process::start_detached(missing_program.string());
    CHECK_FALSE(ok);
  }

  TEST_CASE("start_detached with arguments returns true") {
    bool ok = Process::start_detached("/bin/sleep", {"0"});
    CHECK(ok);
  }

  TEST_CASE("execute returns minus one when the process times out") {
    int code = Process::execute("/bin/sleep", {"1"}, 1);
    CHECK_EQ(code, -1);
  }

  TEST_CASE("execute returns minus one when the program cannot start") {
    int code = Process::execute("/nonexistent_program_xyz_vlink_test", {}, 100);
    CHECK_EQ(code, -1);
  }

  TEST_CASE("start with custom path without inherit environment exits zero") {
    Process proc;
    proc.set_process_mode(Process::kForwardedMode);
    proc.set_inherit_environment(false);
    proc.set_environment({{"PATH", "/bin:/usr/bin"}});

    proc.start("sh", {"-c", "exit 0"});
    REQUIRE(proc.wait_for_finished(3000));
    CHECK_EQ(proc.get_exit_code(), 0);
  }

  TEST_CASE("start without PATH does not resolve bare program names") {
    Process proc;
    proc.set_process_mode(Process::kForwardedMode);
    proc.set_inherit_environment(false);
    proc.set_environment({});

    proc.start("sh", {"-c", "exit 0"});
    REQUIRE(proc.wait_for_finished(3000));
    CHECK_EQ(proc.get_error(), Process::kStartError);
    CHECK_FALSE(proc.is_running());
  }

  TEST_CASE("start with empty PATH does not resolve bare program names") {
    Process proc;
    proc.set_process_mode(Process::kForwardedMode);
    proc.set_inherit_environment(false);
    proc.set_environment({{"PATH", ""}});

    proc.start("sh", {"-c", "exit 0"});
    REQUIRE(proc.wait_for_finished(3000));
    CHECK_EQ(proc.get_error(), Process::kStartError);
    CHECK_FALSE(proc.is_running());
  }

  TEST_CASE("start resolves empty PATH segment relative to working directory") {
    Process proc;
    proc.set_process_mode(Process::kForwardedMode);
    proc.set_inherit_environment(false);
    proc.set_environment({{"PATH", ":/usr/bin"}});
    proc.set_working_directory("/bin");

    proc.start("sh", {"-c", "exit 0"});
    REQUIRE(proc.wait_for_finished(3000));
    CHECK_EQ(proc.get_exit_code(), 0);
  }

  TEST_CASE("start keeps explicit slash program path without PATH lookup") {
    Process proc;
    proc.set_process_mode(Process::kForwardedMode);
    proc.set_inherit_environment(false);
    proc.set_environment({{"PATH", "/path/that/must/not/be/used"}});

    proc.start("/bin/sh", {"-c", "exit 0"});
    REQUIRE(proc.wait_for_finished(3000));
    CHECK_EQ(proc.get_exit_code(), 0);
  }

  TEST_CASE("start resolves later PATH entry after skipping missing directories") {
    Process proc;
    proc.set_process_mode(Process::kForwardedMode);
    proc.set_inherit_environment(false);
    proc.set_environment({{"PATH", "/no/such/vlink-process-test-dir:/bin"}});

    proc.start("sh", {"-c", "exit 0"});
    REQUIRE(proc.wait_for_finished(3000));
    CHECK_EQ(proc.get_exit_code(), 0);
  }

  TEST_CASE("start with inherited environment applies explicit overrides") {
    Process proc;
    proc.set_process_mode(Process::kForwardedMode);
    proc.set_inherit_environment(true);
    proc.set_environment({{"VLINK_PROCESS_TEST_ENV", "yes"}});

    proc.start("/bin/sh", {"-c", "test \"$VLINK_PROCESS_TEST_ENV\" = yes"});
    REQUIRE(proc.wait_for_finished(Process::kInfinite));
    CHECK_EQ(proc.get_exit_code(), 0);
  }

  TEST_CASE("invalid working directory reports a start error") {
    Process proc;
    proc.set_process_mode(Process::kForwardedMode);
    proc.set_working_directory("/path/that/does/not/exist/vlink_process_test");

    proc.start("/bin/true");
    REQUIRE(proc.wait_for_finished(3000));
    CHECK_EQ(proc.get_error(), Process::kStartError);
    CHECK_FALSE(proc.is_running());
  }

  TEST_CASE("kill terminates a running process") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/sleep", {"60"});
    REQUIRE(proc.wait_for_started(3000));
    CHECK(proc.is_running());

    proc.kill();
    bool finished = proc.wait_for_finished(3000);
    REQUIRE(finished);
    CHECK_FALSE(proc.is_running());
  }

  TEST_CASE("terminate stops a running process without forcing kill") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/sleep", {"60"});
    REQUIRE(proc.wait_for_started(Process::kInfinite));
    proc.terminate();
    REQUIRE(proc.wait_for_finished(3000));

    CHECK_FALSE(proc.is_running());
    CHECK_EQ(proc.get_exit_status(), Process::kCrashExitStatus);
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
    REQUIRE(proc.wait_for_finished(3000));

    std::string output;
    proc.read_all_output(output);
    CHECK(output.find("hello_stdin") != std::string::npos);
  }

  TEST_CASE("exit_status is kNormalExitStatus for a non-zero clean exit") {
    Process proc;
    proc.set_process_mode(Process::kForwardedMode);

    proc.start("/bin/sh", {"-c", "exit 2"});
    REQUIRE(proc.wait_for_finished(3000));

    CHECK_EQ(proc.get_exit_code(), 2);
    CHECK_EQ(proc.get_exit_status(), Process::kNormalExitStatus);
  }

  TEST_CASE("read_all_error contains stderr from sh redirect") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/sh", {"-c", "echo stderr_msg >&2"});
    REQUIRE(proc.wait_for_finished(3000));

    std::string err_out;
    proc.read_all_error(err_out);
    CHECK_FALSE(err_out.empty());
    CHECK(err_out.find("stderr_msg") != std::string::npos);
  }

  TEST_CASE("read_line_stderr returns a single stderr line") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/sh", {"-c", "printf 'err_line\\n' >&2"});
    REQUIRE(proc.wait_for_finished(3000));
    REQUIRE(proc.can_read_line_stderr());

    std::string line;
    CHECK(proc.read_line_stderr(line));
    CHECK_EQ(line, "err_line\n");
  }

  TEST_CASE("read_line_stderr returns buffered data without a newline") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/sh", {"-c", "printf err_no_newline >&2"});
    REQUIRE(proc.wait_for_finished(3000));

    std::string line;
    CHECK(proc.read_line_stderr(line));
    CHECK_EQ(line, "err_no_newline");
  }

  TEST_CASE("read_stdout and read_stderr return zero after buffers are consumed") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/sh", {"-c", "printf out; printf err >&2"});
    REQUIRE(proc.wait_for_finished(3000));

    std::vector<uint8_t> out;
    std::vector<uint8_t> err;
    CHECK_EQ(proc.read_stdout(out, 16u), 3u);
    CHECK_EQ(proc.read_stderr(err, 16u), 3u);
    CHECK_EQ(proc.read_stdout(out, 16u), 0u);
    CHECK(out.empty());
    CHECK_EQ(proc.read_stderr(err, 16u), 0u);
    CHECK(err.empty());
  }

  TEST_CASE("kMergedMode read_all contains merged output") {
    Process proc;
    proc.set_process_mode(Process::kMergedMode);

    proc.start("/bin/sh", {"-c", "echo merged_line; echo merged_err >&2"});
    REQUIRE(proc.wait_for_finished(3000));

    std::string output;
    proc.read_all(output);
    CHECK_FALSE(output.empty());
    CHECK(output.find("merged_line") != std::string::npos);
    CHECK(output.find("merged_err") != std::string::npos);
    CHECK_EQ(proc.get_error(), Process::kNoError);
  }

  TEST_CASE("read_all vector combines stdout and stderr") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/sh", {"-c", "printf out; printf err >&2"});
    REQUIRE(proc.wait_for_finished(3000));

    std::vector<uint8_t> data;
    CHECK(proc.read_all(data));
    std::string combined(data.begin(), data.end());
    CHECK(combined.find("out") != std::string::npos);
    CHECK(combined.find("err") != std::string::npos);
  }

  TEST_CASE("read_all_error into vector consumes stderr buffer") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/sh", {"-c", "printf err_vector_all >&2"});
    REQUIRE(proc.wait_for_finished(3000));

    std::vector<uint8_t> data;
    CHECK(proc.read_all_error(data));
    CHECK_FALSE(data.empty());
    CHECK_FALSE(proc.read_all_error(data));
    CHECK(data.empty());
  }

  TEST_CASE("bytes_available_stdout is non-zero after echo output") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/echo", {"available_test"});
    REQUIRE(proc.wait_for_finished(3000));

    size_t avail = proc.bytes_available_stdout();
    CHECK(avail > 0u);
  }

  TEST_CASE("read_stdout into vector returns non-empty result") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/echo", {"read_vector"});
    REQUIRE(proc.wait_for_finished(3000));

    std::vector<uint8_t> buf;
    size_t n = proc.read_stdout(buf, 64u);
    CHECK(n > 0u);
    CHECK_EQ(buf.size(), n);
  }

  TEST_CASE("read_stderr into vector returns non-empty result") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/sh", {"-c", "printf err_vector >&2"});
    REQUIRE(proc.wait_for_finished(3000));

    std::vector<uint8_t> buf;
    size_t n = proc.read_stderr(buf, 64u);
    CHECK(n > 0u);
    CHECK_EQ(buf.size(), n);
    CHECK_EQ(static_cast<char>(buf.front()), 'e');
  }

  TEST_CASE("read_stdout can consume data in smaller chunks") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/sh", {"-c", "printf abcdef"});
    REQUIRE(proc.wait_for_finished(3000));

    std::vector<uint8_t> first;
    CHECK_EQ(proc.read_stdout(first, 3u), 3u);
    CHECK_EQ(first.size(), 3u);
    CHECK_EQ(static_cast<char>(first[0]), 'a');

    std::string rest;
    proc.read_all_output(rest);
    CHECK_EQ(rest, "def");
  }

  TEST_CASE("ready_read_stdout callback fires at least once") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    std::atomic<int> called{0};
    proc.register_ready_read_stdout_callback([&called]() { called.fetch_add(1, std::memory_order_relaxed); });

    proc.start("/bin/echo", {"callback_test"});
    REQUIRE(proc.wait_for_finished(3000));
    REQUIRE(common_test::wait_until([&called] { return called.load(std::memory_order_relaxed) >= 1; }, 500ms));

    CHECK(called.load() >= 1);
  }

  TEST_CASE("ready_read_stderr callback fires at least once") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    std::atomic<int> called{0};
    proc.register_ready_read_stderr_callback([&called]() { called.fetch_add(1, std::memory_order_relaxed); });

    proc.start("/bin/sh", {"-c", "echo callback_err >&2"});
    REQUIRE(proc.wait_for_finished(3000));
    REQUIRE(common_test::wait_until([&called] { return called.load(std::memory_order_relaxed) >= 1; }, 500ms));

    CHECK(called.load() >= 1);
  }

  TEST_CASE("error callback fires when start fails") {
    Process proc;
    proc.set_process_mode(Process::kForwardedMode);

    std::atomic<bool> error_called{false};
    proc.register_error_callback(
        [&error_called](Process::Error) { error_called.store(true, std::memory_order_relaxed); });

    std::filesystem::path missing_program =
        std::filesystem::path(vlink::Utils::get_tmp_dir()) / "nonexistent_binary_vlink_xyz_test";
    proc.start(missing_program.string());
    REQUIRE(proc.wait_for_finished(3000));
    REQUIRE(common_test::wait_until([&error_called] { return error_called.load(std::memory_order_relaxed); }, 500ms));

    CHECK(error_called.load());
  }

  TEST_CASE("kForwardedOutputMode buffers stderr while forwarding stdout") {
    Process proc;
    proc.set_process_mode(Process::kForwardedOutputMode);

    proc.start("/bin/sh", {"-c", "echo out; echo err >&2"});
    bool finished = proc.wait_for_finished(3000);
    REQUIRE(finished);
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
    REQUIRE(finished);
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
    REQUIRE(proc.wait_for_finished(3000));

    CHECK_EQ(proc.bytes_available_stdout(), first_size);
    CHECK_EQ(proc.get_error(), Process::kBufferOverflowError);
  }

  TEST_CASE("exact max buffer fill is not reported as overflow") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);
    proc.set_max_buffer_size(6);

    proc.start("/bin/sh", {"-c", "printf abcdef"});
    REQUIRE(proc.wait_for_finished(3000));

    std::string output;
    proc.read_all_output(output);
    CHECK_EQ(output, "abcdef");
    CHECK_NE(proc.get_error(), Process::kBufferOverflowError);
  }

  TEST_CASE("small max buffer truncates stdout and stderr without growing buffers") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);
    proc.set_max_buffer_size(4);

    proc.start("/bin/sh", {"-c", "printf abcdefgh; printf 12345678 >&2"});
    REQUIRE(proc.wait_for_finished(3000));

    CHECK_EQ(proc.get_error(), Process::kBufferOverflowError);
    CHECK(proc.bytes_available_stdout() <= 4u);
    CHECK(proc.bytes_available_stderr() <= 4u);
  }

  TEST_CASE("write vector overload echoed by cat") {
    Process proc;
    proc.start("/bin/cat", {});

    std::vector<uint8_t> input_data = {'h', 'e', 'l', 'l', 'o', '\n'};
    auto wrote_size = proc.write(input_data, 2000);
    if (wrote_size > 0) {
      proc.close_write_channel();
      REQUIRE(proc.wait_for_finished(3000));
      std::string output;
      proc.read_all_output(output);
      CHECK(output.find("hello") != std::string::npos);
    } else {
      proc.kill();
      CHECK(proc.wait_for_finished(1000));
    }
  }

  TEST_CASE("write string times out when the child does not read stdin") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/sleep", {"2"});
    REQUIRE(proc.wait_for_started(3000));

    std::string large(1024 * 1024, 'x');
    const auto written = proc.write(large, 10);
    CHECK(written < large.size());
    CHECK_EQ(proc.get_error(), Process::kTimedOutError);

    proc.kill();
    REQUIRE(proc.wait_for_finished(3000));
  }

  TEST_CASE("write vector times out when the child does not read stdin") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/sleep", {"2"});
    REQUIRE(proc.wait_for_started(3000));

    std::vector<uint8_t> large(1024 * 1024, static_cast<uint8_t>('x'));
    const auto written = proc.write(large, 10);
    CHECK(written < large.size());
    CHECK_EQ(proc.get_error(), Process::kTimedOutError);

    proc.kill();
    REQUIRE(proc.wait_for_finished(3000));
  }

  TEST_CASE("write empty string and vector return zero while running") {
    Process proc;
    proc.start("/bin/cat", {});
    REQUIRE(proc.wait_for_started(3000));

    CHECK_EQ(proc.write(std::string(), 100), 0u);
    CHECK_EQ(proc.write(std::vector<uint8_t>{}, 100), 0u);

    proc.close_write_channel();
    REQUIRE(proc.wait_for_finished(3000));
  }

  TEST_CASE("wait_for_ready_read times out while a silent process is running") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/sleep", {"1"});
    REQUIRE(proc.wait_for_started(3000));

    CHECK_FALSE(proc.wait_for_ready_read(20));

    proc.kill();
    REQUIRE(proc.wait_for_finished(3000));
  }

  TEST_CASE("forwarded mode has no captured descriptors for wait_for_ready_read") {
    Process proc;
    proc.set_process_mode(Process::kForwardedMode);

    proc.start("/bin/sleep", {"1"});
    REQUIRE(proc.wait_for_started(3000));

    CHECK_FALSE(proc.wait_for_ready_read(20));

    proc.kill();
    REQUIRE(proc.wait_for_finished(3000));
  }

  TEST_CASE("write after closing stdin reports no bytes written") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/cat", {});
    REQUIRE(proc.wait_for_started(3000));
    proc.close_write_channel();

    CHECK_EQ(proc.write("ignored", 100), 0u);
    REQUIRE(proc.wait_for_finished(3000));
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
    REQUIRE(proc.wait_for_finished(3000));

    REQUIRE(common_test::wait_until([&read_in_callback] { return read_in_callback.load(std::memory_order_acquire); },
                                    500ms));

    std::lock_guard lock(output_mtx);
    CHECK(output.find("callback_read") != std::string::npos);
  }

  TEST_CASE("wait_for_ready_read supports an infinite timeout") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);

    proc.start("/bin/sh", {"-c", "sleep 0.1; printf infinite_ready"});
    REQUIRE(proc.wait_for_started(3000));

    CHECK(proc.wait_for_ready_read(Process::kInfinite));

    REQUIRE(proc.wait_for_finished(3000));
    std::string output;
    CHECK(proc.read_all_output(output));
    CHECK(output.find("infinite_ready") != std::string::npos);
  }

  TEST_CASE("wait_for_started covers starting-state timeout and infinite wait") {
    Process proc;
    proc.set_process_mode(Process::kForwardedMode);

    std::thread infinite_waiter;
    std::atomic_bool infinite_wait_started{false};
    std::atomic_bool infinite_wait_result{false};
    bool timed_wait_result = true;

    proc.register_state_changed_callback([&](Process::State state) {
      if (state != Process::kStartingState) {
        return;
      }

      infinite_waiter = std::thread([&] {
        infinite_wait_started.store(true, std::memory_order_release);
        infinite_wait_result.store(proc.wait_for_started(Process::kInfinite), std::memory_order_release);
      });

      REQUIRE(common_test::wait_until([&] { return infinite_wait_started.load(std::memory_order_acquire); }, 500ms));
      timed_wait_result = proc.wait_for_started(1);
    });

    proc.start("/bin/true");
    REQUIRE(proc.wait_for_finished(3000));

    if (infinite_waiter.joinable()) {
      infinite_waiter.join();
    }

    CHECK_FALSE(timed_wait_result);
    CHECK(infinite_wait_result.load(std::memory_order_acquire));
  }

  TEST_CASE("write string and vector support an infinite timeout") {
    Process string_proc;
    string_proc.set_process_mode(Process::kSeparateMode);
    string_proc.start("/bin/cat", {});
    REQUIRE(string_proc.wait_for_started(3000));

    const std::string text = "infinite_string\n";
    CHECK_EQ(string_proc.write(text, Process::kInfinite), text.size());
    string_proc.close_write_channel();
    REQUIRE(string_proc.wait_for_finished(3000));

    std::string string_output;
    CHECK(string_proc.read_all_output(string_output));
    CHECK(string_output.find("infinite_string") != std::string::npos);

    Process vector_proc;
    vector_proc.set_process_mode(Process::kSeparateMode);
    vector_proc.start("/bin/cat", {});
    REQUIRE(vector_proc.wait_for_started(3000));

    const std::vector<uint8_t> data{'i', 'n', 'f', 'i', 'n', 'i', 't', 'e', '_', 'v', 'e', 'c', 't', 'o', 'r', '\n'};
    CHECK_EQ(vector_proc.write(data, Process::kInfinite), data.size());
    vector_proc.close_write_channel();
    REQUIRE(vector_proc.wait_for_finished(3000));

    std::string vector_output;
    CHECK(vector_proc.read_all_output(vector_output));
    CHECK(vector_output.find("infinite_vector") != std::string::npos);
  }

  TEST_CASE("child process ignores SIGTERM until killed") {
    if (Utils::get_env("VLINK_PROCESS_IGNORE_TERM_CHILD") != "1") {
      return;
    }

    std::signal(SIGTERM, [](int) {});
    std::cout << "ready" << std::endl;
    const auto deadline = std::chrono::steady_clock::now() + 30s;

    while (std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(50ms);
    }

    std::_Exit(0);
  }

  TEST_CASE("close without force leaves a SIGTERM ignoring process running") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);
    proc.set_inherit_environment(true);
    proc.set_environment({{"VLINK_PROCESS_IGNORE_TERM_CHILD", "1"}});

    proc.start(Utils::get_app_path(),
               {"--test-suite=base-Process", "--test-case=child process ignores SIGTERM until killed", "--no-version"});
    REQUIRE(proc.wait_for_started(3000));
    REQUIRE(proc.wait_for_ready_read(3000));

    proc.close(false);
    CHECK(proc.is_running());

    proc.kill();
    REQUIRE(proc.wait_for_finished(3000));
  }

  TEST_CASE("close with force kills a SIGTERM ignoring process") {
    Process proc;
    proc.set_process_mode(Process::kSeparateMode);
    proc.set_inherit_environment(true);
    proc.set_environment({{"VLINK_PROCESS_IGNORE_TERM_CHILD", "1"}});

    proc.start(Utils::get_app_path(),
               {"--test-suite=base-Process", "--test-case=child process ignores SIGTERM until killed", "--no-version"});
    REQUIRE(proc.wait_for_started(3000));
    REQUIRE(proc.wait_for_ready_read(3000));

    proc.close(true);
    CHECK_FALSE(proc.is_running());
  }

#endif
}

// NOLINTEND
