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

#include "./base/format.h"

#include <doctest/doctest.h>

#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "../common_test.h"

namespace {

template <typename... ArgsT>
std::string fmt(const char* fmt_str, const ArgsT&... args) {
  char buf[256];
  auto r = format::format_to_n(buf, sizeof(buf) - 1, fmt_str, args...);
  size_t len = r.size < sizeof(buf) - 1 ? r.size : sizeof(buf) - 1;
  return std::string(buf, len);
}

}  // namespace

TEST_SUITE("base-Format") {
  TEST_CASE("plain text with no placeholders") {
    char buf[64];
    auto r = format::format_to_n(buf, sizeof(buf) - 1, "hello world");
    buf[r.size] = '\0';

    CHECK_EQ(std::string(buf), "hello world");
    CHECK_EQ(r.size, 11u);
    CHECK_FALSE(r.truncated);
  }

  TEST_CASE("format int: zero, positive, negative, boundary") {
    CHECK_EQ(fmt("{}", 0), "0");
    CHECK_EQ(fmt("{}", 42), "42");
    CHECK_EQ(fmt("{}", -1), "-1");
    CHECK_EQ(fmt("{}", 2147483647), "2147483647");
    CHECK_EQ(fmt("{}", -2147483647 - 1), "-2147483648");
  }

  TEST_CASE("format unsigned int: zero and max") {
    CHECK_EQ(fmt("{}", 0u), "0");
    CHECK_EQ(fmt("{}", 42u), "42");
    CHECK_EQ(fmt("{}", 4294967295u), "4294967295");
  }

  TEST_CASE("format long long: large positive and negative") {
    long long big = 9000000000LL;   // NOLINT(runtime/int)
    long long neg = -9000000000LL;  // NOLINT(runtime/int)
    CHECK_EQ(fmt("{}", big), "9000000000");
    CHECK_EQ(fmt("{}", neg), "-9000000000");
    CHECK_EQ(fmt("{}", -1LL), "-1");  // NOLINT(runtime/int)
  }

  TEST_CASE("format unsigned long long: zero and large") {
    unsigned long long z = 0ULL;            // NOLINT(runtime/int)
    unsigned long long v = 18000000000ULL;  // NOLINT(runtime/int)
    CHECK_EQ(fmt("{}", z), "0");
    CHECK_EQ(fmt("{}", v), "18000000000");
  }

  TEST_CASE("format bool produces true and false") {
    CHECK_EQ(fmt("{}", true), "true");
    CHECK_EQ(fmt("{}", false), "false");
  }

  TEST_CASE("format char") {
    CHECK_EQ(fmt("{}", 'A'), "A");
    CHECK_EQ(fmt("{}", '0'), "0");
  }

  TEST_CASE("format float via %g") {
    CHECK_EQ(fmt("{}", 1.5f), "1.5");
    CHECK_EQ(fmt("{}", -2.5f), "-2.5");
    CHECK_EQ(fmt("{}", 0.0f), "0");
  }

  TEST_CASE("format double via %g") {
    CHECK_EQ(fmt("{}", 3.14), "3.14");
    CHECK_EQ(fmt("{}", 0.0), "0");
    CHECK_FALSE(fmt("{}", 1e15).empty());
  }

  TEST_CASE("format const char*") {
    CHECK_EQ(fmt("{}", "hello"), "hello");
    CHECK_EQ(fmt("{}", ""), "");
  }

  TEST_CASE("null const char* renders as (null)") {
    const char* p = nullptr;
    CHECK_EQ(fmt("{}", p), "(null)");
  }

  TEST_CASE("format std::string") {
    std::string s = "vlink";
    CHECK_EQ(fmt("{}", s), "vlink");
  }

  TEST_CASE("format empty std::string") {
    std::string s;
    CHECK(fmt("{}", s).empty());
  }

  TEST_CASE("format std::string_view") {
    std::string_view sv = "view";
    CHECK_EQ(fmt("{}", sv), "view");
  }

  TEST_CASE("format empty string_view") {
    std::string_view sv;
    CHECK(fmt("{}", sv).empty());
  }

  TEST_CASE("format pointer starts with 0x") {
    int x = 0;
    int* p = &x;
    char buf[64];
    auto r = format::format_to_n(buf, sizeof(buf) - 1, "{}", p);
    buf[r.size] = '\0';
    std::string s(buf, r.size);
    CHECK_EQ(s.substr(0, 2), "0x");
    CHECK(s.size() >= 3u);
  }

  TEST_CASE("format nullptr pointer renders as 0x0") {
    void* p = nullptr;
    char buf[64];
    auto r = format::format_to_n(buf, sizeof(buf) - 1, "{}", p);
    buf[r.size] = '\0';
    CHECK_EQ(std::string(buf, r.size), "0x0");
  }

  TEST_CASE("format enum uses underlying integral value") {
    enum class Color : int { Red = 1, Green = 2, Blue = 3 };
    CHECK_EQ(fmt("{}", Color::Red), "1");
    CHECK_EQ(fmt("{}", Color::Green), "2");
    CHECK_EQ(fmt("{}", Color::Blue), "3");
  }

  TEST_CASE("multiple {} placeholders consumed in order") {
    CHECK_EQ(fmt("{} {} {}", 1, 2, 3), "1 2 3");
    CHECK_EQ(fmt("a={} b={}", 10, 20), "a=10 b=20");
  }

  TEST_CASE("explicit index placeholders {0} and {1}") {
    char buf[64];
    auto r = format::format_to_n(buf, sizeof(buf) - 1, "{0} {1}", 10, 20);
    buf[r.size] = '\0';
    CHECK_EQ(std::string(buf, r.size), "10 20");
  }

  TEST_CASE("explicit index can repeat an argument") {
    char buf[64];
    auto r = format::format_to_n(buf, sizeof(buf) - 1, "{0} {0}", 42);
    buf[r.size] = '\0';
    CHECK_EQ(std::string(buf, r.size), "42 42");
  }

  TEST_CASE("escaped double braces {{ and }} produce literal braces") {
    CHECK_EQ(fmt("{{}}"), "{}");
    CHECK_EQ(fmt("{{open}} {{close}}"), "{open} {close}");
    CHECK_EQ(fmt("{{{}}} {}", 1, 2), "{1} 2");
  }

  TEST_CASE("trailing unmatched { is emitted as literal") {
    char buf[64];
    auto r = format::format_to_n(buf, sizeof(buf) - 1, "trailing{");
    buf[r.size] = '\0';
    CHECK_EQ(std::string(buf, r.size), "trailing{");
  }

  TEST_CASE("single } in format string is emitted as literal") {
    char buf[64];
    auto r = format::format_to_n(buf, sizeof(buf) - 1, "a}b");
    buf[r.size] = '\0';
    CHECK_EQ(std::string(buf, r.size), "a}b");
  }

  TEST_CASE("empty format string produces no output") {
    char buf[64];
    auto r = format::format_to_n(buf, sizeof(buf) - 1, "");
    CHECK_EQ(r.size, 0u);
    CHECK_FALSE(r.truncated);
  }

  TEST_CASE("truncated output sets truncated flag and reports full size") {
    char buf[5];
    auto r = format::format_to_n(buf, 4, "hello world");
    CHECK(r.truncated);
    CHECK_EQ(r.size, 11u);
  }

  TEST_CASE("zero capacity buffer reports size and truncated") {
    char buf[1];
    auto r = format::format_to_n(buf, 0, "hello");
    CHECK_EQ(r.size, 5u);
    CHECK(r.truncated);
  }

  TEST_CASE("truncation mid-argument produces partial output") {
    char buf[4];
    auto r = format::format_to_n(buf, 3, "{}", 12345);
    CHECK(r.truncated);
    CHECK_EQ(r.size, 5u);
    CHECK_EQ(buf[0], '1');
    CHECK_EQ(buf[1], '2');
    CHECK_EQ(buf[2], '3');
  }

  TEST_CASE("non-truncated output has truncated=false") {
    char buf[64];
    auto r = format::format_to_n(buf, sizeof(buf) - 1, "hi");
    CHECK_FALSE(r.truncated);
    CHECK_EQ(r.size, 2u);
  }

  TEST_CASE("format_to fixed array overload writes correctly") {
    char buf[64];
    int v = 7;
    auto r = format::format_to(buf, "x={}", v);
    CHECK_EQ(std::string(buf, r.size), "x=7");
    CHECK_FALSE(r.truncated);
  }

  TEST_CASE("format_to iterator overload with raw char pointer") {
    char buf[64];
    char* ptr = buf;
    int a = 3;
    int b = 4;
    char* end = format::format_to(ptr, "v={} w={}", a, b);
    auto written = static_cast<size_t>(end - buf);
    CHECK_EQ(std::string(buf, written), "v=3 w=4");
  }

  TEST_CASE("format_to iterator with back_inserter") {
    std::string result;
    int val = 99;
    format::format_to(std::back_inserter(result), "num={}", val);
    CHECK_EQ(result, "num=99");
  }

  TEST_CASE("mixed types in single call") {
    char buf[128];
    auto r = format::format_to_n(buf, sizeof(buf) - 1, "{} {} {} {} {}", 1, true, 'X', "str", 3.14);
    buf[r.size] = '\0';
    std::string s(buf, r.size);
    CHECK(s.find("1") != std::string::npos);
    CHECK(s.find("true") != std::string::npos);
    CHECK(s.find("X") != std::string::npos);
    CHECK(s.find("str") != std::string::npos);
    CHECK(s.find("3.14") != std::string::npos);
  }

  TEST_CASE("extra placeholders beyond argument count are skipped") {
    char buf[64];
    auto r = format::format_to_n(buf, sizeof(buf) - 1, "{} {} {}", 1);
    buf[r.size] = '\0';
    std::string s(buf, r.size);
    CHECK(s.find("1") != std::string::npos);
  }

  TEST_CASE("explicit index out of range produces no output for that placeholder") {
    char buf[64];
    auto r = format::format_to_n(buf, sizeof(buf) - 1, "{99}", 42);
    buf[r.size] = '\0';
    CHECK(std::string(buf, r.size).empty());
  }

  TEST_CASE("format short integer: positive and negative boundary") {
    short pos = 100;     // NOLINT(runtime/int)
    short neg = -32768;  // NOLINT(runtime/int)
    CHECK_EQ(fmt("{}", pos), "100");
    CHECK_EQ(fmt("{}", neg), "-32768");
  }

  TEST_CASE("format unsigned short: zero and max") {
    unsigned short z = 0;      // NOLINT(runtime/int)
    unsigned short m = 65535;  // NOLINT(runtime/int)
    CHECK_EQ(fmt("{}", z), "0");
    CHECK_EQ(fmt("{}", m), "65535");
  }

  TEST_CASE("format signed char: positive, negative, boundary") {
    signed char pos = 127;  // NOLINT(runtime/int)
    signed char neg = -5;   // NOLINT(runtime/int)
    CHECK_EQ(fmt("{}", pos), "127");
    CHECK_EQ(fmt("{}", neg), "-5");
  }

  TEST_CASE("format unsigned char: zero and max") {
    unsigned char z = 0;    // NOLINT(runtime/int)
    unsigned char m = 255;  // NOLINT(runtime/int)
    CHECK_EQ(fmt("{}", z), "0");
    CHECK_EQ(fmt("{}", m), "255");
  }

  TEST_CASE("format long and unsigned long") {
    long l = 100000L;           // NOLINT(runtime/int)
    unsigned long ul = 200000;  // NOLINT(runtime/int)
    CHECK_EQ(fmt("{}", l), "100000");
    CHECK_EQ(fmt("{}", ul), "200000");
  }

  TEST_CASE("FString implicit construction from string literal") {
    vlink::format::FString<> fs("test literal");
    std::string_view sv = fs;
    CHECK_EQ(sv, "test literal");
  }

  TEST_CASE("FString construction from std::string") {
    std::string s = "dynamic";
    vlink::format::FString<> fs(s);
    CHECK_EQ(fs.get(), "dynamic");
  }

  TEST_CASE("FString construction from std::string_view") {
    std::string_view sv = "view-based";
    vlink::format::FString<> fs(sv);
    CHECK_EQ(fs.get(), "view-based");
  }

  TEST_CASE("make_format_args captures argument count") {
    auto store = vlink::format::make_format_args(1, 2, 3);
    CHECK_EQ(store.kNumArgs, 3u);
  }
}

// NOLINTEND
