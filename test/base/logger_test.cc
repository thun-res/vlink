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

#include "./base/logger.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "../common_test.h"
#include "./base/process.h"
#include "./base/utils.h"
#include "./vlink/version.h"

namespace {

#if defined(VLINK_ENABLE_LOG_BACKEND)
constexpr bool kHasFileLoggerBackend = true;
#else
constexpr bool kHasFileLoggerBackend = false;
#endif

constexpr size_t kTimestampFileCount = 2U;

std::filesystem::path logger_tmp_dir(const std::string& name) {
  auto dir = std::filesystem::path(Utils::get_tmp_dir()) / "vlink-logger-tests" / name;
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir;
}

std::vector<std::filesystem::path> logger_files(const std::filesystem::path& root) {
  std::vector<std::filesystem::path> files;
  std::error_code error;

  if (!std::filesystem::exists(root, error)) {
    return files;
  }

  for (std::filesystem::recursive_directory_iterator iter(root, error), end; iter != end && !error;
       iter.increment(error)) {
    if (iter->is_regular_file(error) && iter->path().extension() == ".log") {
      files.emplace_back(iter->path());
    }
  }

  std::sort(files.begin(), files.end());
  return files;
}

std::string read_logger_files(const std::filesystem::path& root) {
  std::string content;

  for (const auto& path : logger_files(root)) {
    std::ifstream stream(path, std::ios::binary);
    content.append(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  }

  return content;
}

[[maybe_unused]] size_t count_logger_records(std::string_view content, std::string_view marker) {
  size_t count = 0U;
  size_t offset = 0U;

  while ((offset = content.find(marker, offset)) != std::string_view::npos) {
    ++count;
    offset += marker.size();
  }

  return count;
}

void reset_logger_dir(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::remove_all(path, error);
  std::filesystem::create_directories(path, error);
}

Logger::Level expected_logger_level(const std::string& child_case) {
  if (child_case == "level-trace") {
    return Logger::kTrace;
  }

  if (child_case == "level-debug") {
    return Logger::kDebug;
  }

  if (child_case == "level-info") {
    return Logger::kInfo;
  }

  if (child_case == "level-warn") {
    return Logger::kWarn;
  }

  if (child_case == "level-error") {
    return Logger::kError;
  }

  if (child_case == "level-fatal") {
    return Logger::kFatal;
  }

  if (child_case == "numeric-level" || child_case == "common-level") {
    return Logger::kWarn;
  }

  return Logger::kOff;
}

void run_logger_child_case(const std::string& child_case) {
  if (child_case == "first-log-recursion") {
    int callback_count = 0;
    int evaluations = 0;
    Logger::set_console_level(Logger::kInfo);
    Logger::set_file_level(Logger::kOff);
    Logger::register_console_handler([&callback_count, &evaluations](Logger::Level, std::string_view) {
      ++callback_count;
      VLOG_I("nested first-log callback ", ++evaluations);
    });

    VLOG_I("outer first-log callback");
    CHECK_EQ(callback_count, 1);
    CHECK_EQ(evaluations, 0);
    Logger::register_console_handler(nullptr);
    return;
  }

  if (child_case == "handler-exception") {
    int callback_count = 0;
    Logger::set_console_level(Logger::kInfo);
    Logger::set_file_level(Logger::kOff);
    Logger::register_console_handler([&callback_count](Logger::Level, std::string_view) {
      ++callback_count;
      throw std::runtime_error("expected handler exception");
    });

    VLOG_I("throwing handler callback");
    CHECK_EQ(callback_count, 1);
    Logger::register_console_handler(nullptr);
    return;
  }

  if (child_case.rfind("level-", 0) == 0 || child_case == "numeric-level" || child_case == "invalid-level" ||
      child_case == "range-level" || child_case == "common-level") {
    Logger::get();
    CHECK_EQ(Logger::get_console_level(), expected_logger_level(child_case));
    return;
  }

  if (child_case == "file-time-rolling") {
    Logger::init("vlink_logger_child", Utils::get_env("VLINK_LOG_DIR"));
    CHECK_EQ(Logger::get_console_level(), Logger::kTrace);
    CHECK_EQ(Logger::get_file_level(), Logger::kTrace);
    CHECK(Logger::get_console_fmt_enable());

    for (int index = 0; index < 64; ++index) {
      VLOG_I("timestamp rotation payload ", index,
             " abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz");
    }

    CHECK_THROWS(VLOG_F("fatal child log"));
    Logger::enable_backtrace(8);
    VLOG_T("backtrace retained trace");
    VLOG_I("backtrace retained info");
    Logger::dump_backtrace();
    Logger::disable_backtrace();
    Logger::flush();
    return;
  }

  if (child_case == "file-rotating") {
    Logger::init("vlink_logger_child", Utils::get_env("VLINK_LOG_DIR"));
    CHECK_EQ(Logger::get_console_level(), Logger::kOff);
    CHECK_EQ(Logger::get_file_level(), Logger::kTrace);

    for (int index = 0; index < 64; ++index) {
      VLOG_I("fixed rotation payload ", index,
             " abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz");
    }

    CHECK_THROWS(VLOG_F("fatal rotating child log"));
    Logger::flush();
    return;
  }

  if (child_case == "queue-block") {
    Logger::init("vlink_logger_child", Utils::get_env("VLINK_LOG_DIR"));
    CHECK_EQ(Logger::get_console_level(), Logger::kOff);
    CHECK_EQ(Logger::get_file_level(), Logger::kTrace);

    for (int index = 0; index < 2000; ++index) {
      VLOG_I("blocking queue record ", index);
    }

    VLOG_I("blocking queue final record");
    Logger::flush();
    return;
  }

  if (child_case == "queue-drop-oldest") {
    Logger::init("vlink_logger_child", Utils::get_env("VLINK_LOG_DIR"));
    CHECK_EQ(Logger::get_console_level(), Logger::kOff);
    CHECK_EQ(Logger::get_file_level(), Logger::kTrace);

    for (int index = 0; index < 2000; ++index) {
      VLOG_I("dropping queue record ", index);
    }

    VLOG_I("dropping queue final record");
    Logger::flush();
    return;
  }

  if (child_case == "invalid-memory-config") {
    Logger::init("vlink_logger_child", Utils::get_env("VLINK_LOG_DIR"));
    VLOG_I("logger initialized with invalid memory configuration");
    Logger::flush();
    return;
  }

  if (child_case == "missing-plugin") {
    Logger::get();
    CHECK_EQ(Logger::get_file_level(), Logger::kInfo);
    VLOG_I("missing plugin child log");
    Logger::flush();
    return;
  }

#ifndef _WIN32
  if (child_case == "invalid-file-path") {
    bool console_called = false;
    Logger::register_console_handler([&console_called](Logger::Level, std::string_view) { console_called = true; });
    Logger::init("vlink_logger_invalid_path", "/proc/vlink-logger-invalid-path");
    VLOG_I("logger remains available after file sink initialization fails");
    CHECK(console_called);
    Logger::register_console_handler(nullptr);
    Logger::flush();
    return;
  }
#endif

  FAIL("unknown logger child case");
}

void run_logger_child(const std::string& child_case, Process::EnvironmentMap environment) {
  static constexpr const char* kIsolatedEnvironmentVariables[]{
      "VLINK_LOG_LEVEL",       "VLINK_LOG_CONSOLE_LEVEL", "VLINK_LOG_FILE_LEVEL",  "VLINK_LOG_CONSOLE_UNORDER",
      "VLINK_LOG_ENABLE_UTC",  "VLINK_LOG_CONSOLE_FMT",   "VLINK_LOG_PLUGIN",      "VLINK_LOG_DIR",
      "VLINK_LOG_MAX_SIZE",    "VLINK_LOG_MAX_COUNT",     "VLINK_LOG_FLUSH_DELAY", "VLINK_LOG_STORE_STRATEGY",
      "VLINK_LOG_OPEN_APPEND", "VLINK_LOG_BLOCK_SYNC",    "VLINK_LOG_WRITE_DEPTH", "VLINK_MEMORY_LEVEL",
      "VLINK_MEMORY_PREALLOC", "VLINK_MEMORY_BATCH_SIZE",
  };

  for (const char* name : kIsolatedEnvironmentVariables) {
    environment.try_emplace(name, "");
  }

  Process child;
  child.set_process_mode(Process::kForwardedMode);
  child.set_inherit_environment(true);
  environment["VLINK_LOGGER_CHILD_CASE"] = child_case;
  child.set_environment(environment);
  child.start(Utils::get_app_path(),
              {"--test-suite=base-Logger",
               "--test-case=child process covers logger environment initialization branches", "--no-version"});
  REQUIRE(child.wait_for_finished(Process::kDefaultExecuteTimeoutMs));
  CHECK_EQ(child.get_exit_code(), 0);
}

}  // namespace

TEST_SUITE("base-Logger") {
  TEST_CASE("get returns the same instance every time") {
    Logger& a = Logger::get();
    Logger& b = Logger::get();
    CHECK_EQ(&a, &b);
  }

  TEST_CASE("init with app name does not crash") { Logger::init("test_app"); }

  TEST_CASE("init with app name and empty log path does not crash") { Logger::init("vlink_test", ""); }

  TEST_CASE("level enum values are strictly ordered") {
    CHECK(Logger::kTrace < Logger::kDebug);
    CHECK(Logger::kDebug < Logger::kInfo);
    CHECK(Logger::kInfo < Logger::kWarn);
    CHECK(Logger::kWarn < Logger::kError);
    CHECK(Logger::kError < Logger::kFatal);
    CHECK(Logger::kFatal < Logger::kOff);
  }

  TEST_CASE("kTrace is 0 and kOff is 6") {
    CHECK_EQ(static_cast<int>(Logger::kTrace), 0);
    CHECK_EQ(static_cast<int>(Logger::kOff), 6);
  }

  TEST_CASE("kMinimumLevel is kTrace") { CHECK_EQ(Logger::kMinimumLevel, Logger::kTrace); }

  TEST_CASE("kDetailLevel is kWarn") { CHECK_EQ(Logger::kDetailLevel, Logger::kWarn); }

  TEST_CASE("kLocalBufferSize is 4096") { CHECK_EQ(Logger::kLocalBufferSize, 4096); }

  TEST_CASE("set_console_level and get_console_level round-trip") {
    Logger::init("test");

    Logger::set_console_level(Logger::kInfo);
    CHECK_EQ(Logger::get_console_level(), Logger::kInfo);

    Logger::set_console_level(Logger::kDebug);
    CHECK_EQ(Logger::get_console_level(), Logger::kDebug);

    Logger::set_console_level(Logger::kWarn);
    CHECK_EQ(Logger::get_console_level(), Logger::kWarn);

    Logger::set_console_level(Logger::kTrace);
  }

  TEST_CASE("set_file_level and get_file_level round-trip") {
    Logger::init("test");

    Logger::set_file_level(Logger::kError);
    CHECK_EQ(Logger::get_file_level(), Logger::kError);

    Logger::set_file_level(Logger::kInfo);
    CHECK_EQ(Logger::get_file_level(), Logger::kInfo);

    Logger::set_file_level(Logger::kOff);
    CHECK_EQ(Logger::get_file_level(), Logger::kOff);
  }

  TEST_CASE("set_console_fmt_enable and get_console_fmt_enable round-trip") {
    Logger::init("test");

    Logger::set_console_fmt_enable(false);
    CHECK_FALSE(Logger::get_console_fmt_enable());

    Logger::set_console_fmt_enable(true);
    CHECK(Logger::get_console_fmt_enable());
  }

  TEST_CASE("set_stream_precision and get_stream_precision round-trip") {
    Logger::init("test");

    Logger::set_stream_precision(6);
    CHECK_EQ(Logger::get_stream_precision(), 6);

    Logger::set_stream_precision(2);
    CHECK_EQ(Logger::get_stream_precision(), 2);
  }

  TEST_CASE("set_stream_width and get_stream_width round-trip") {
    Logger::init("test");

    Logger::set_stream_width(10);
    CHECK_EQ(Logger::get_stream_width(), 10);

    Logger::set_stream_width(0);
    CHECK_EQ(Logger::get_stream_width(), 0);
  }

  TEST_CASE("set_stream_flag and get_stream_flag round-trip") {
    Logger::init("test");
    auto orig = Logger::get_stream_flag();

    Logger::set_stream_flag(std::ios_base::hex);
    CHECK_EQ(Logger::get_stream_flag(), std::ios_base::hex);

    Logger::set_stream_flag(orig);
  }

  TEST_CASE("is_writable returns false when both sinks are kOff") {
    Logger::init("test");
    Logger::set_console_level(Logger::kOff);
    Logger::set_file_level(Logger::kOff);

    CHECK_FALSE(Logger::is_writable(Logger::kTrace));
    CHECK_FALSE(Logger::is_writable(Logger::kDebug));
    CHECK_FALSE(Logger::is_writable(Logger::kInfo));
    CHECK_FALSE(Logger::is_writable(Logger::kWarn));
    CHECK_FALSE(Logger::is_writable(Logger::kError));

    Logger::set_console_level(Logger::kTrace);
  }

  TEST_CASE("is_writable returns true for levels at or above console level") {
    Logger::init("test");
    Logger::set_console_level(Logger::kInfo);
    Logger::set_file_level(Logger::kOff);

    CHECK(Logger::is_writable(Logger::kInfo));
    CHECK(Logger::is_writable(Logger::kWarn));
    CHECK(Logger::is_writable(Logger::kError));

    Logger::set_console_level(Logger::kTrace);
  }

  TEST_CASE("extract_filename from POSIX path") {
    CHECK_EQ(Logger::extract_filename("/home/user/project/main.cc"), "main.cc");
  }

  TEST_CASE("extract_filename from Windows path") {
    CHECK_EQ(Logger::extract_filename("C:\\src\\vlink\\main.cc"), "main.cc");  // NOLINT(modernize-raw-string-literal)
  }

  TEST_CASE("extract_filename with no separator returns whole string") {
    CHECK_EQ(Logger::extract_filename("main.cc"), "main.cc");
  }

  TEST_CASE("extract_filename with empty string returns empty view") { CHECK(Logger::extract_filename("").empty()); }

  TEST_CASE("flush does not crash") {
    Logger::init("test");
    Logger::flush();
  }

  TEST_CASE("enable_backtrace and disable_backtrace do not crash") {
    Logger::init("test");
    Logger::enable_backtrace(16);
    Logger::disable_backtrace();
  }

  TEST_CASE("dump_backtrace does not crash") {
    Logger::init("test");
    Logger::enable_backtrace(8);
    Logger::dump_backtrace();
    Logger::disable_backtrace();
  }

  TEST_CASE("is_busy returns without crashing") {
    Logger::init("test");
    bool b = Logger::is_busy();
    (void)b;
  }

  TEST_CASE("VLOG_I does not crash") {
    Logger::init("test");
    Logger::set_console_level(Logger::kInfo);
    VLOG_I("unit test info value=", 42);
  }

  TEST_CASE("VLOG_D does not crash") {
    Logger::init("test");
    Logger::set_console_level(Logger::kDebug);
    VLOG_D("debug value=", 3.14);
  }

  TEST_CASE("VLOG_W does not crash") {
    Logger::init("test");
    Logger::set_console_level(Logger::kWarn);
    VLOG_W("warning message");
  }

  TEST_CASE("VLOG_E does not crash") {
    Logger::init("test");
    Logger::set_console_level(Logger::kError);
    VLOG_E("error code=", 1);
  }

  TEST_CASE("VLOG_F throws RuntimeError") {
    Logger::init("test");
    Logger::set_console_level(Logger::kFatal);
    CHECK_THROWS(VLOG_F("fatal test message"));
  }

  TEST_CASE("MLOG_I format-style does not crash") {
    Logger::init("test");
    Logger::set_console_level(Logger::kInfo);
    MLOG_I("format value={}", 99);
  }

  TEST_CASE("MLOG_D does not crash") {
    Logger::init("test");
    Logger::set_console_level(Logger::kDebug);
    MLOG_D("mlog_debug");
  }

  TEST_CASE("MLOG_W does not crash") {
    Logger::init("test");
    Logger::set_console_level(Logger::kWarn);
    MLOG_W("mlog_warn");
  }

  TEST_CASE("MLOG_E does not crash") {
    Logger::init("test");
    Logger::set_console_level(Logger::kError);
    MLOG_E("mlog_error");
  }

  TEST_CASE("CLOG_I c-style does not crash") {
    Logger::init("test");
    Logger::set_console_level(Logger::kInfo);
    CLOG_I("c-style value=%d", 7);
  }

  TEST_CASE("CLOG_D does not crash") {
    Logger::init("test");
    Logger::set_console_level(Logger::kDebug);
    CLOG_D("clog_debug %d", 1);
  }

  TEST_CASE("CLOG_W does not crash") {
    Logger::init("test");
    Logger::set_console_level(Logger::kWarn);
    CLOG_W("clog_warn %d", 2);
  }

  TEST_CASE("CLOG_E does not crash") {
    Logger::init("test");
    Logger::set_console_level(Logger::kError);
    CLOG_E("clog_error %d", 3);
  }

  TEST_CASE("SLOG_I stream-style does not crash") {
    Logger::init("test");
    Logger::set_console_level(Logger::kInfo);
    SLOG_I << "slog_info_test";
  }

  TEST_CASE("SLOG_D does not crash") {
    Logger::init("test");
    Logger::set_console_level(Logger::kDebug);
    SLOG_D << "slog_debug_test";
  }

  TEST_CASE("SLOG_W does not crash") {
    Logger::init("test");
    Logger::set_console_level(Logger::kWarn);
    SLOG_W << "slog_warn_test";
  }

  TEST_CASE("SLOG_E does not crash") {
    Logger::init("test");
    Logger::set_console_level(Logger::kError);
    SLOG_E << "slog_error_test";
  }

  TEST_CASE("disabled ordinary log macros do not evaluate arguments") {
    Logger::init("test");
    Logger::set_console_level(Logger::kOff);
    Logger::set_file_level(Logger::kOff);

    int evaluations = 0;
    auto evaluate = [&evaluations] {
      ++evaluations;
      return evaluations;
    };

    VLOG_I("stream value=", evaluate());
    MLOG_I("format value={}", evaluate());
    CLOG_I("c-style value=%d", evaluate());
    SLOG_I << "raii value=" << evaluate();

    CHECK_EQ(evaluations, 0);
    CHECK_THROWS(VLOG_F("fatal value=", evaluate()));
    CHECK_EQ(evaluations, 1);

    Logger::set_console_level(Logger::kTrace);
  }

  TEST_CASE("register_console_handler is invoked when a message is logged") {
    Logger::init("test");
    Logger::set_console_level(Logger::kInfo);

    bool called = false;
    Logger::register_console_handler([&called](Logger::Level, std::string_view) { called = true; });

    VLOG_I("handler callback test");
    CHECK(called);

    Logger::register_console_handler(nullptr);
  }

  TEST_CASE("console handler recursion is rejected before nested formatting") {
    Logger::init("test");
    Logger::set_console_level(Logger::kInfo);
    Logger::set_file_level(Logger::kOff);

    int calls = 0;
    int evaluations = 0;
    Logger::register_console_handler([&calls, &evaluations](Logger::Level, std::string_view) {
      ++calls;
      VLOG_I("nested callback record ", ++evaluations);
    });

    VLOG_I("outer callback record");
    CHECK_EQ(calls, 1);
    CHECK_EQ(evaluations, 0);

    Logger::register_console_handler(nullptr);
  }

  TEST_CASE("register_console_handler receives the correct level") {
    Logger::init("test");
    Logger::set_console_level(Logger::kWarn);

    Logger::Level received = Logger::kTrace;
    Logger::register_console_handler([&received](Logger::Level lv, std::string_view) { received = lv; });

    VLOG_W("level check");
    CHECK_EQ(received, Logger::kWarn);

    Logger::register_console_handler(nullptr);
  }

  TEST_CASE("register_file_handler receives logged messages") {
    Logger::init("test");
    Logger::set_file_level(Logger::kInfo);

    std::atomic<int> file_calls{0};
    Logger::register_file_handler(
        [&file_calls](Logger::Level, std::string_view) { file_calls.fetch_add(1, std::memory_order_relaxed); });

    VLOG_I("file handler test message");
    Logger::flush();

    CHECK(file_calls.load() >= 1);

    Logger::register_file_handler(nullptr);
  }

  TEST_CASE("file handler recursion is rejected before nested formatting") {
    Logger::init("test");
    Logger::set_console_level(Logger::kOff);
    Logger::set_file_level(Logger::kInfo);

    int calls = 0;
    int evaluations = 0;
    Logger::register_file_handler([&calls, &evaluations](Logger::Level, std::string_view) {
      ++calls;
      VLOG_I("nested file callback record ", ++evaluations);
    });

    VLOG_I("outer file callback record");
    CHECK_EQ(calls, 1);
    CHECK_EQ(evaluations, 0);

    Logger::register_file_handler(nullptr);
  }

  TEST_CASE("direct print APIs include detail data and respect disabled levels") {
    Logger::init("test");
    Logger::set_console_level(Logger::kTrace);
    Logger::set_file_level(Logger::kOff);

    std::string received;
    Logger::Level level = Logger::kOff;
    Logger::register_console_handler([&](Logger::Level lv, std::string_view log) {
      level = lv;
      received.assign(log);
    });

    Logger::print_stream_style<Logger::kWarn>(Logger::DetailInfo{"sample.cc", 17}, "payload=", 3);
    CHECK_EQ(level, Logger::kWarn);
    CHECK(received.find("{sample.cc:17}") != std::string::npos);
    CHECK(received.find("payload=3") != std::string::npos);

    Logger::print_format_style<Logger::kError>(Logger::DetailInfo{"format.cc", 19}, "value={}", 5);
    CHECK_EQ(level, Logger::kError);
    CHECK(received.find("{format.cc:19}") != std::string::npos);
    CHECK(received.find("value=5") != std::string::npos);

    Logger::print_c_style<Logger::kInfo>(Logger::NoDetail{}, "plain c string");
    CHECK_EQ(level, Logger::kInfo);
    CHECK_EQ(received, "plain c string");

    received.clear();
    Logger::print_stream_style<Logger::kOff>(Logger::NoDetail{}, "ignored");
    CHECK(received.empty());

    Logger::register_console_handler(nullptr);
  }

  TEST_CASE("C-style formatting keeps the terminator outside truncated messages") {
    Logger::init("test");
    Logger::set_console_level(Logger::kInfo);
    Logger::set_file_level(Logger::kOff);

    std::string received;
    Logger::register_console_handler([&received](Logger::Level, std::string_view log) { received.assign(log); });

    for (size_t payload_size : {4094U, 4095U, 4096U}) {
      const std::string payload(payload_size, 'x');
      Logger::print_c_style<Logger::kInfo>(Logger::NoDetail{}, "%s", payload.c_str());

      CHECK_EQ(received.size(), std::min(payload_size, static_cast<size_t>(Logger::kLocalBufferSize - 1)));
      CHECK_EQ(received.find('\0'), std::string::npos);
    }

    Logger::register_console_handler(nullptr);
  }

  TEST_CASE("stream log macros preserve integer formatting") {
    Logger::init("test");
    Logger::set_console_level(Logger::kInfo);
    Logger::set_file_level(Logger::kOff);
    Logger::set_stream_width(0);

    std::string received;
    Logger::register_console_handler([&received](Logger::Level, std::string_view log) { received.assign(log); });

    Logger::set_stream_flag(std::ios_base::dec | std::ios_base::skipws);
    VLOG_I(std::numeric_limits<int64_t>::min(), "/", std::numeric_limits<uint64_t>::max());
    CHECK_EQ(received, "-9223372036854775808/18446744073709551615");

    SLOG_I << std::numeric_limits<int64_t>::max() << "/" << std::numeric_limits<uint64_t>::max();
    CHECK_EQ(received, "9223372036854775807/18446744073709551615");

    VLOG_I(static_cast<unsigned char>('A'));
    CHECK_EQ(received, "A");

    Logger::set_stream_flag(std::ios_base::hex);
    VLOG_I(255);
    CHECK_EQ(received, "ff");

    Logger::set_stream_flag(std::ios_base::dec | std::ios_base::skipws | std::ios_base::showpos);
    VLOG_I(42);
    CHECK_EQ(received, "+42");

    Logger::set_stream_flag(std::ios_base::dec | std::ios_base::skipws);
    Logger::set_stream_width(5);
    VLOG_I(42);
    CHECK_EQ(received, "   42");

    Logger::set_stream_width(0);
    Logger::register_console_handler(nullptr);
  }

  TEST_CASE("periodic log macros throttle before evaluating arguments") {
    Logger::init("test");
    Logger::set_console_level(Logger::kInfo);
    Logger::set_file_level(Logger::kOff);

    std::atomic<int> calls{0};
    std::atomic<int> evaluations{0};
    Logger::register_console_handler(
        [&calls](Logger::Level, std::string_view) { calls.fetch_add(1, std::memory_order_relaxed); });

    auto emit = [&evaluations] {
      VLOG_I_EVERY_MS(50, "periodic value=", evaluations.fetch_add(1, std::memory_order_relaxed));
    };

    emit();
    emit();
    CHECK_EQ(calls.load(std::memory_order_relaxed), 1);
    CHECK_EQ(evaluations.load(std::memory_order_relaxed), 1);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (;;) {
      emit();

      if (calls.load(std::memory_order_relaxed) != 1 || std::chrono::steady_clock::now() >= deadline) {
        break;
      }

      std::this_thread::yield();
    }
    CHECK_EQ(calls.load(std::memory_order_relaxed), 2);
    CHECK_EQ(evaluations.load(std::memory_order_relaxed), 2);

    Logger::register_console_handler(nullptr);
  }

  TEST_CASE("periodic log macros keep independent call-site state and allow non-positive intervals") {
    Logger::init("test");
    Logger::set_console_level(Logger::kTrace);
    Logger::set_file_level(Logger::kOff);

    std::vector<Logger::Level> levels;
    Logger::register_console_handler([&levels](Logger::Level level, std::string_view) { levels.emplace_back(level); });

    int64_t vlink_interval_ms = 60'000;
    int vlink_last_log_time_ns = 1;
    VLINK_LOG_T_EVERY_MS(60'000, "full periodic macro");
    VLOG_D_EVERY_MS(vlink_interval_ms, "macro hygiene ", vlink_last_log_time_ns);
    VLOG_I_EVERY_MS(60'000, "info periodic macro");
    VLOG_W_EVERY_MS(60'000, "warn periodic macro");
    VLOG_E_EVERY_MS(60'000, "error periodic macro");
    VLOG_I_EVERY_MS(60'000, "first independent info call site");
    VLOG_I_EVERY_MS(60'000, "second independent info call site");

    const std::vector<Logger::Level> expected_levels{Logger::kTrace, Logger::kDebug, Logger::kInfo, Logger::kWarn,
                                                     Logger::kError, Logger::kInfo,  Logger::kInfo};
    CHECK_EQ(levels, expected_levels);

    for (int interval_ms : {0, -1, 0}) {
      VLOG_T_EVERY_MS(interval_ms, "unlimited periodic ", interval_ms);
    }
    CHECK_EQ(levels.size(), 10);

    Logger::register_console_handler(nullptr);
  }

  TEST_CASE("periodic log macro is shared safely by concurrent callers") {
    Logger::init("test");
    Logger::set_console_level(Logger::kInfo);
    Logger::set_file_level(Logger::kOff);

    std::atomic<int> calls{0};
    std::atomic<bool> start{false};
    Logger::register_console_handler(
        [&calls](Logger::Level, std::string_view) { calls.fetch_add(1, std::memory_order_relaxed); });

    auto emit = [&start] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      VLOG_I_EVERY_MS(60'000, "concurrent periodic");
    };

    std::vector<std::thread> threads;
    threads.reserve(8);
    for (int i = 0; i < 8; ++i) {
      threads.emplace_back(emit);
    }

    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
      thread.join();
    }
    CHECK_EQ(calls.load(std::memory_order_relaxed), 1);

    std::atomic<uint64_t> future_time{std::numeric_limits<uint64_t>::max()};
    CHECK_FALSE(Logger::try_acquire_periodic_log(Logger::kInfo, 1, future_time));
    CHECK_EQ(future_time.load(std::memory_order_relaxed), std::numeric_limits<uint64_t>::max());

    std::atomic<uint64_t> fatal_time{0};
    CHECK_FALSE(Logger::try_acquire_periodic_log(Logger::kFatal, 1, fatal_time));
    CHECK_EQ(fatal_time.load(std::memory_order_relaxed), 0);

    Logger::register_console_handler(nullptr);
  }

  TEST_CASE("disabled periodic log does not consume its next writable period") {
    Logger::init("test");
    Logger::set_console_level(Logger::kOff);
    Logger::set_file_level(Logger::kOff);

    std::atomic<int> calls{0};
    Logger::register_console_handler(
        [&calls](Logger::Level, std::string_view) { calls.fetch_add(1, std::memory_order_relaxed); });

    auto emit = [] { VLOG_I_EVERY_MS(60'000, "periodic after enable"); };
    emit();
    Logger::set_console_level(Logger::kInfo);
    emit();
    CHECK_EQ(calls.load(std::memory_order_relaxed), 1);

    Logger::register_console_handler(nullptr);
  }

  TEST_CASE("fatal format variants throw through the shared finalize path") {
    Logger::init("test");
    Logger::set_console_level(Logger::kFatal);
    Logger::set_file_level(Logger::kOff);
    Logger::register_console_handler([](Logger::Level, std::string_view) {});

    CHECK_THROWS(MLOG_F("fatal {}", 1));
    CHECK_THROWS(CLOG_F("fatal %d", 2));
    CHECK_THROWS(SLOG_F << "fatal " << 3);

    Logger::register_console_handler(nullptr);
  }

  TEST_CASE("child process covers logger environment initialization branches") {
    const auto child_case = Utils::get_env("VLINK_LOGGER_CHILD_CASE");

    if (!child_case.empty()) {
      run_logger_child_case(child_case);
      return;
    }

    const std::vector<std::pair<std::string, std::string>> named_levels{
        {"level-trace", "Trace"}, {"level-debug", "DEBUG"}, {"level-info", "info"}, {"level-warn", "Warn"},
        {"level-error", "ERROR"}, {"level-fatal", "fatal"}, {"level-off", "Off"}};

    run_logger_child("first-log-recursion", {{"VLINK_LOG_CONSOLE_LEVEL", "Info"}, {"VLINK_LOG_FILE_LEVEL", "Off"}});
    run_logger_child("handler-exception", {{"VLINK_LOG_CONSOLE_LEVEL", "Info"}, {"VLINK_LOG_FILE_LEVEL", "Off"}});

    for (const auto& [name, value] : named_levels) {
      run_logger_child(name, {{"VLINK_LOG_CONSOLE_LEVEL", value}, {"VLINK_LOG_FILE_LEVEL", "Off"}});
    }

    run_logger_child("numeric-level", {{"VLINK_LOG_CONSOLE_LEVEL", "3"}, {"VLINK_LOG_FILE_LEVEL", "Off"}});
    run_logger_child("invalid-level", {{"VLINK_LOG_CONSOLE_LEVEL", "not-a-level"}, {"VLINK_LOG_LEVEL", "Off"}});
    run_logger_child("range-level", {{"VLINK_LOG_CONSOLE_LEVEL", "99"}, {"VLINK_LOG_FILE_LEVEL", "Off"}});

    run_logger_child("missing-plugin", {{"VLINK_LOG_FILE_LEVEL", "Info"},
                                        {"VLINK_LOG_CONSOLE_LEVEL", "Off"},
                                        {"VLINK_LOG_PLUGIN", "__missing_vlink_logger_plugin__"}});

    if constexpr (!kHasFileLoggerBackend) {
      return;
    }

    const auto timestamp_dir = logger_tmp_dir("time");
    reset_logger_dir(timestamp_dir);
    run_logger_child("file-time-rolling", {{"VLINK_LOG_CONSOLE_LEVEL", "Trace"},
                                           {"VLINK_LOG_FILE_LEVEL", "Trace"},
                                           {"VLINK_LOG_CONSOLE_FMT", "1"},
                                           {"VLINK_LOG_CONSOLE_UNORDER", "1"},
                                           {"VLINK_LOG_ENABLE_UTC", "1"},
                                           {"VLINK_LOG_DIR", timestamp_dir.generic_string() + "/"},
                                           {"VLINK_LOG_MAX_SIZE", "512"},
                                           {"VLINK_LOG_MAX_COUNT", "2"},
                                           {"VLINK_LOG_FLUSH_DELAY", "0"},
                                           {"VLINK_LOG_WRITE_DEPTH", "64"}});
    CHECK_EQ(logger_files(timestamp_dir).size(), kTimestampFileCount);
    const auto timestamp_content = read_logger_files(timestamp_dir);
    CHECK(timestamp_content.find(" UTC @") != std::string::npos);
    CHECK(timestamp_content.find("backtrace retained trace") != std::string::npos);
    CHECK(timestamp_content.find("backtrace retained info") != std::string::npos);

    const auto rotating_dir = logger_tmp_dir("rotating");
    reset_logger_dir(rotating_dir);
    run_logger_child("file-rotating", {{"VLINK_LOG_LEVEL", "Trace"},
                                       {"VLINK_LOG_CONSOLE_LEVEL", "Off"},
                                       {"VLINK_LOG_DIR", rotating_dir.string()},
                                       {"VLINK_LOG_STORE_STRATEGY", "1"},
                                       {"VLINK_LOG_OPEN_APPEND", "1"},
                                       {"VLINK_LOG_BLOCK_SYNC", "1"},
                                       {"VLINK_LOG_MAX_SIZE", "512"},
                                       {"VLINK_LOG_MAX_COUNT", "2"},
                                       {"VLINK_LOG_FLUSH_DELAY", "25"},
                                       {"VLINK_LOG_WRITE_DEPTH", "64"}});
    CHECK_EQ(logger_files(rotating_dir).size(), 3U);
    CHECK(read_logger_files(rotating_dir).find("fatal rotating child log") != std::string::npos);

#if defined(VLINK_ENABLE_LOG_BACKEND)
    const auto blocking_dir = logger_tmp_dir("queue-block");
    reset_logger_dir(blocking_dir);
    run_logger_child("queue-block", {{"VLINK_LOG_LEVEL", "Trace"},
                                     {"VLINK_LOG_CONSOLE_LEVEL", "Off"},
                                     {"VLINK_LOG_DIR", blocking_dir.string()},
                                     {"VLINK_LOG_STORE_STRATEGY", "1"},
                                     {"VLINK_LOG_BLOCK_SYNC", "1"},
                                     {"VLINK_LOG_MAX_SIZE", "1048576"},
                                     {"VLINK_LOG_MAX_COUNT", "1"},
                                     {"VLINK_LOG_FLUSH_DELAY", "1"},
                                     {"VLINK_LOG_WRITE_DEPTH", "1"}});
    const auto blocking_content = read_logger_files(blocking_dir);
    CHECK(blocking_content.find("blocking queue record 0") != std::string::npos);
    CHECK(blocking_content.find("blocking queue final record") != std::string::npos);
    CHECK_EQ(count_logger_records(blocking_content, "blocking queue record "), 2000U);

    const auto dropping_dir = logger_tmp_dir("queue-drop-oldest");
    reset_logger_dir(dropping_dir);
    run_logger_child("queue-drop-oldest", {{"VLINK_LOG_LEVEL", "Trace"},
                                           {"VLINK_LOG_CONSOLE_LEVEL", "Off"},
                                           {"VLINK_LOG_DIR", dropping_dir.string()},
                                           {"VLINK_LOG_STORE_STRATEGY", "1"},
                                           {"VLINK_LOG_BLOCK_SYNC", "0"},
                                           {"VLINK_LOG_MAX_SIZE", "1048576"},
                                           {"VLINK_LOG_MAX_COUNT", "1"},
                                           {"VLINK_LOG_FLUSH_DELAY", "500"},
                                           {"VLINK_LOG_WRITE_DEPTH", "1"}});
    const auto dropping_content = read_logger_files(dropping_dir);
    CHECK(dropping_content.find("dropping queue final record") != std::string::npos);
    CHECK(count_logger_records(dropping_content, "dropping queue record ") < 2000U);
#endif

#if defined(VLINK_ENABLE_LOG_BACKEND)
    const auto invalid_memory_dir = logger_tmp_dir("invalid-memory-config");
    reset_logger_dir(invalid_memory_dir);
    run_logger_child("invalid-memory-config", {{"VLINK_LOG_FILE_LEVEL", "Info"},
                                               {"VLINK_LOG_CONSOLE_LEVEL", "Off"},
                                               {"VLINK_LOG_DIR", invalid_memory_dir.string()},
                                               {"VLINK_LOG_STORE_STRATEGY", "1"},
                                               {"VLINK_MEMORY_LEVEL", "invalid"},
                                               {"VLINK_MEMORY_BATCH_SIZE", "invalid"}});
    CHECK(read_logger_files(invalid_memory_dir).find("logger initialized with invalid memory configuration") !=
          std::string::npos);
#endif

#ifndef _WIN32
    run_logger_child(
        "invalid-file-path",
        {{"VLINK_LOG_FILE_LEVEL", "Info"}, {"VLINK_LOG_CONSOLE_LEVEL", "Info"}, {"VLINK_LOG_STORE_STRATEGY", "1"}});
#endif
  }

  TEST_CASE("init with a non-empty log path does not crash") {
    Logger::init("vlink_logtest_app", "/tmp/vlink-logtest");
    Logger::init("test");
  }
}

// NOLINTEND
