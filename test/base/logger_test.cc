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

#include <atomic>
#include <string>
#include <string_view>

#include "../common_test.h"

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

  TEST_CASE("register_console_handler is invoked when a message is logged") {
    Logger::init("test");
    Logger::set_console_level(Logger::kInfo);

    bool called = false;
    Logger::register_console_handler([&called](Logger::Level, std::string_view) { called = true; });

    VLOG_I("handler callback test");
    CHECK(called);

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
}

// NOLINTEND
