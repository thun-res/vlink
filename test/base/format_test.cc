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

#include <charconv>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "../common_test.h"

#if defined(_GLIBCXX_RELEASE) && _GLIBCXX_RELEASE < 11
enum class TestCharsFormat : uint8_t { scientific = 1U, fixed = 2U, hex = 4U, general = 3U };
#else
using TestCharsFormat = std::chars_format;
#endif

template <typename FloatT, typename = void>
struct TestHasFloatToChars final : std::false_type {};

template <typename FloatT>
struct TestHasFloatToChars<FloatT,
                           std::void_t<decltype(std::to_chars(std::declval<char*>(), std::declval<char*>(),
                                                              std::declval<FloatT>(), TestCharsFormat::general, 6))>>
    final : std::true_type {};

static constexpr bool kTestHasFloatToChars = TestHasFloatToChars<double>::value;

namespace {

template <typename... ArgsT>
std::string fmt(const char* fmt_str, const ArgsT&... args) {
  char buf[256];
  auto r = format::format_to_n(buf, sizeof(buf) - 1, fmt_str, args...);
  size_t len = r.size < sizeof(buf) - 1 ? r.size : sizeof(buf) - 1;
  return std::string(buf, len);
}

}  // namespace

namespace format_as_test {

struct Meters final {
  double value{0.0};
};

constexpr double format_as(const Meters& meters) noexcept { return meters.value; }

enum class Level : int { kInfo = 1 };

constexpr int format_as(Level level) noexcept { return static_cast<int>(level) * 10; }

struct Tag final {
  std::string name;
};

inline const std::string& format_as(const Tag& tag) noexcept { return tag.name; }

}  // namespace format_as_test

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

  TEST_CASE("spec width pads with default alignment per type") {
    CHECK_EQ(fmt("{:5}", 42), "   42");
    CHECK_EQ(fmt("{:8}", 3.14), "    3.14");
    CHECK_EQ(fmt("{:8}", "abc"), "abc     ");
    CHECK_EQ(fmt("{:3}", 'x'), "x  ");
    CHECK_EQ(fmt("{:6}", true), "true  ");
  }

  TEST_CASE("spec width smaller than or equal to content is a no-op") {
    CHECK_EQ(fmt("{:2}", 12345), "12345");
    CHECK_EQ(fmt("{:5}", 12345), "12345");
  }

  TEST_CASE("spec explicit alignment with default fill") {
    CHECK_EQ(fmt("{:<5}", 42), "42   ");
    CHECK_EQ(fmt("{:>5}", 42), "   42");
    CHECK_EQ(fmt("{:^5}", 42), " 42  ");
    CHECK_EQ(fmt("{:>8}", "abc"), "     abc");
  }

  TEST_CASE("spec custom fill characters") {
    CHECK_EQ(fmt("{:*<6}", 42), "42****");
    CHECK_EQ(fmt("{:*>6}", 42), "****42");
    CHECK_EQ(fmt("{:*^6}", 42), "**42**");
    CHECK_EQ(fmt("{:*^7}", "ab"), "**ab***");
    CHECK_EQ(fmt("{:0<5}", 42), "42000");
  }

  TEST_CASE("spec sign handling for integers") {
    CHECK_EQ(fmt("{:+}", 42), "+42");
    CHECK_EQ(fmt("{:+}", -42), "-42");
    CHECK_EQ(fmt("{: }", 42), " 42");
    CHECK_EQ(fmt("{: }", -42), "-42");
    CHECK_EQ(fmt("{:-}", 42), "42");
    CHECK_EQ(fmt("{:+}", 0), "+0");
    CHECK_EQ(fmt("{:+}", 42u), "+42");
  }

  TEST_CASE("spec binary octal and hex presentations") {
    CHECK_EQ(fmt("{:b}", 10), "1010");
    CHECK_EQ(fmt("{:o}", 64), "100");
    CHECK_EQ(fmt("{:x}", 255), "ff");
    CHECK_EQ(fmt("{:X}", 255), "FF");
    CHECK_EQ(fmt("{:d}", 42), "42");
    CHECK_EQ(fmt("{:x}", -26), "-1a");
  }

  TEST_CASE("spec alternate form prefixes") {
    CHECK_EQ(fmt("{:#b}", 5), "0b101");
    CHECK_EQ(fmt("{:#B}", 5), "0B101");
    CHECK_EQ(fmt("{:#o}", 8), "010");
    CHECK_EQ(fmt("{:#x}", 255), "0xff");
    CHECK_EQ(fmt("{:#X}", 255), "0XFF");
    CHECK_EQ(fmt("{:#x}", -26), "-0x1a");
  }

  TEST_CASE("spec alternate form of zero values") {
    CHECK_EQ(fmt("{:#b}", 0), "0b0");
    CHECK_EQ(fmt("{:#o}", 0), "0");
    CHECK_EQ(fmt("{:#x}", 0), "0x0");
  }

  TEST_CASE("spec zero padding for integers") {
    CHECK_EQ(fmt("{:05}", 42), "00042");
    CHECK_EQ(fmt("{:05}", -42), "-0042");
    CHECK_EQ(fmt("{:+06}", 42), "+00042");
    CHECK_EQ(fmt("{:#06x}", 255), "0x00ff");
    CHECK_EQ(fmt("{:#010x}", 255), "0x000000ff");
  }

  TEST_CASE("spec explicit alignment cancels zero padding") { CHECK_EQ(fmt("{:<05}", 42), "42   "); }

  TEST_CASE("spec zero padding is ignored for strings") { CHECK_EQ(fmt("{:05}", "ab"), "ab   "); }

  TEST_CASE("spec integer boundary values in every base") {
    const auto min64 = std::numeric_limits<long long>::min();           // NOLINT(runtime/int)
    const auto max64 = std::numeric_limits<unsigned long long>::max();  // NOLINT(runtime/int)
    CHECK_EQ(fmt("{:d}", min64), "-9223372036854775808");
    CHECK_EQ(fmt("{:x}", min64), "-8000000000000000");
    CHECK_EQ(fmt("{:x}", max64), "ffffffffffffffff");
    CHECK_EQ(fmt("{:b}", 255ull), "11111111");
  }

  TEST_CASE("spec char presentation of integers") {
    CHECK_EQ(fmt("{:c}", 65), "A");
    CHECK_EQ(fmt("{:c}", 97), "a");
    CHECK_EQ(fmt("{:^3c}", 65), " A ");
  }

  TEST_CASE("spec integer presentations of char") {
    CHECK_EQ(fmt("{:d}", 'A'), "65");
    CHECK_EQ(fmt("{:x}", 'A'), "41");
    CHECK_EQ(fmt("{:#x}", 'A'), "0x41");
    CHECK_EQ(fmt("{:c}", 'z'), "z");
    CHECK_EQ(fmt("{:f}", 'A'), "A");
  }

  TEST_CASE("spec presentations of bool") {
    CHECK_EQ(fmt("{:d}", true), "1");
    CHECK_EQ(fmt("{:d}", false), "0");
    CHECK_EQ(fmt("{:#x}", true), "0x1");
    CHECK_EQ(fmt("{:s}", true), "true");
    CHECK_EQ(fmt("{:>7}", false), "  false");
    CHECK_EQ(fmt("{:05d}", true), "00001");
    CHECK_EQ(fmt("{:f}", true), "true");
    CHECK_EQ(fmt("{:>7f}", false), "  false");
  }

  TEST_CASE("spec fixed float precision") {
    CHECK_EQ(fmt("{:.2f}", 3.14159), "3.14");
    CHECK_EQ(fmt("{:.2f}", -3.14159), "-3.14");
    CHECK_EQ(fmt("{:.0f}", 2.6), "3");
    CHECK_EQ(fmt("{:f}", 1.5), "1.500000");
    CHECK_EQ(fmt("{:F}", 1.5), "1.500000");
    CHECK_EQ(fmt("{:+.1f}", 4.26), "+4.3");
  }

  TEST_CASE("spec scientific float presentation") {
    CHECK_EQ(fmt("{:e}", 314.15), "3.141500e+02");
    CHECK_EQ(fmt("{:.1e}", 314.15), "3.1e+02");
    CHECK_EQ(fmt("{:E}", 314.15), "3.141500E+02");
  }

  TEST_CASE("spec general float presentation") {
    CHECK_EQ(fmt("{:g}", 0.00001), "1e-05");
    CHECK_EQ(fmt("{:G}", 0.00001), "1E-05");
    CHECK_EQ(fmt("{:g}", 100000.0), "100000");
    CHECK_EQ(fmt("{:g}", 1000000.0), "1e+06");
    CHECK_EQ(fmt("{:.3}", 3.14159), "3.14");
    CHECK_EQ(fmt("{:.3g}", 3.14159), "3.14");
  }

  TEST_CASE("spec hex float presentation") {
    if constexpr (kTestHasFloatToChars) {
      CHECK_EQ(fmt("{:a}", 255.0), "1.fep+7");
      CHECK_EQ(fmt("{:a}", 1.0), "1p+0");
      CHECK_EQ(fmt("{:A}", 255.0), "1.FEP+7");
      CHECK_EQ(fmt("{:.2a}", 1.0), "1.00p+0");
    }
  }

  TEST_CASE("spec alternate float forms guarantee the decimal point") {
    CHECK_EQ(fmt("{:#.0f}", 3.0), "3.");
    CHECK_EQ(fmt("{:#g}", 1.0), "1.00000");
    CHECK_EQ(fmt("{:#.0e}", 3.0), "3.e+00");

    if constexpr (kTestHasFloatToChars) {
      CHECK_EQ(fmt("{:#.2}", 0.5), "0.5");
      CHECK_EQ(fmt("{:#.2}", 6.02e23), "6.e+23");
      CHECK_EQ(fmt("{:#}", 1.0), "1.");
    }
  }

  TEST_CASE("spec additional edge coverage") {
    CHECK_EQ(fmt("{:+<5}", 42), "42+++");
    CHECK_EQ(fmt("{:#d}", 42), "42");
    CHECK_EQ(fmt("{9:x}", 1), "");
    CHECK_EQ(fmt("{:.2f}", -0.0), "-0.00");
    CHECK_EQ(fmt("{:08}", -42LL), "-0000042");
    CHECK_EQ(fmt("{:5}", ""), "     ");
  }

  TEST_CASE("spec float width zero padding and alignment") {
    CHECK_EQ(fmt("{:10.2f}", 3.14159), "      3.14");
    CHECK_EQ(fmt("{:<10.2f}", 3.14159), "3.14      ");
    CHECK_EQ(fmt("{:08.2f}", -3.14159), "-0003.14");
    CHECK_EQ(fmt("{:06}", 3.5), "0003.5");
  }

  TEST_CASE("spec infinity and nan are never zero padded") {
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    CHECK_EQ(fmt("{:f}", inf), "inf");
    CHECK_EQ(fmt("{:f}", -inf), "-inf");
    CHECK_EQ(fmt("{:F}", inf), "INF");
    CHECK_EQ(fmt("{:+f}", inf), "+inf");
    CHECK_EQ(fmt("{:f}", nan), "nan");
    CHECK_EQ(fmt("{:E}", nan), "NAN");
    CHECK_EQ(fmt("{:06f}", inf), "   inf");
    CHECK_EQ(fmt("{:10f}", inf), "       inf");
  }

  TEST_CASE("spec float argument uses float precision semantics") {
    CHECK_EQ(fmt("{:.2f}", 3.14f), "3.14");
    CHECK_EQ(fmt("{:}", 3.14f), "3.14");
  }

  TEST_CASE("spec string precision truncates") {
    CHECK_EQ(fmt("{:.3}", "hello"), "hel");
    CHECK_EQ(fmt("{:.10}", "hi"), "hi");
    CHECK_EQ(fmt("{:.0}", "hello"), "");
    CHECK_EQ(fmt("{:8.3}", "hello"), "hel     ");
    CHECK_EQ(fmt("{:>8.3}", "hello"), "     hel");
    CHECK_EQ(fmt("{:^9.5}", "abcdefgh"), "  abcde  ");
    CHECK_EQ(fmt("{:.3}", std::string("hello")), "hel");
    CHECK_EQ(fmt("{:.3}", std::string_view("hello")), "hel");
  }

  TEST_CASE("spec null cstring is padded as (null)") {
    const char* null_str = nullptr;
    CHECK_EQ(fmt("{:>8}", null_str), "  (null)");
  }

  TEST_CASE("spec pointer presentation and padding") {
    const auto* ptr = reinterpret_cast<const void*>(0x1f);
    CHECK_EQ(fmt("{:p}", ptr), "0x1f");
    CHECK_EQ(fmt("{:12}", ptr), "        0x1f");
    CHECK_EQ(fmt("{:012}", ptr), "0x000000001f");
    CHECK_EQ(fmt("{:<8}", reinterpret_cast<const void*>(0x1)), "0x1     ");
    CHECK_EQ(fmt("{:6}", static_cast<const void*>(nullptr)), "   0x0");
  }

  TEST_CASE("spec enum uses the underlying integer presentation") {
    enum class SpecColor : int { kBlue = 26 };
    CHECK_EQ(fmt("{:#x}", SpecColor::kBlue), "0x1a");
    CHECK_EQ(fmt("{:04}", SpecColor::kBlue), "0026");
  }

  TEST_CASE("spec with positional indexes") {
    CHECK_EQ(fmt("{0:>4}|{1:<4}", 1, 2), "   1|2   ");
    CHECK_EQ(fmt("{0:x} {0:d} {0:#o}", 26), "1a 26 032");
  }

  TEST_CASE("empty spec matches default formatting") {
    CHECK_EQ(fmt("{:}", 42), "42");
    CHECK_EQ(fmt("{:}", true), "true");
    CHECK_EQ(fmt("{:}", "x"), "x");
    CHECK_EQ(fmt("{:}", 2.5), "2.5");
  }

  TEST_CASE("escaped braces are not parsed as specs") {
    CHECK_EQ(fmt("{{:d}}"), "{:d}");
    CHECK_EQ(fmt("{{{:d}}}", 7), "{7}");
  }

  TEST_CASE("plain and spec placeholders mix in one format string") {
    CHECK_EQ(fmt("a={} b={:04}", 1, 2), "a=1 b=0002");
  }

  TEST_CASE("spec lenient handling of inapplicable fields") {
    CHECK_EQ(fmt("{:q}", 42), "42");
    CHECK_EQ(fmt("{:5q}", 42), "   42");
    CHECK_EQ(fmt("{:.3}", 12345), "12345");
    CHECK_EQ(fmt("{:s}", 42), "42");
    CHECK_EQ(fmt("{:.f}", 2.6), "3");
    CHECK_EQ(fmt("{:>}", 42), "42");
  }

  TEST_CASE("unterminated spec placeholder is dropped") { CHECK_EQ(fmt("x{:>5", 42), "x"); }

  TEST_CASE("spec output truncation keeps size and flag") {
    char buf[6];
    auto r = format::format_to_n(buf, 5, "{:08}", 42);
    CHECK_EQ(r.size, 8u);
    CHECK(r.truncated);
    CHECK_EQ(std::string(buf, 5), "00000");
  }

  TEST_CASE("spec huge width remains bounded for a fixed output buffer") {
    char out[1];
    auto r = format::format_to_n(out, sizeof(out), "{:999999999}", 1);
    CHECK_EQ(r.size, 999999999U);
    CHECK(r.truncated);
    CHECK_EQ(out[0], ' ');
  }

  TEST_CASE("spec works through format_to array and iterator overloads") {
    char out[8];
    auto r = format::format_to(out, "{:03}", 7);
    CHECK_EQ(std::string(out, r.size), "007");

    std::string s;
    format::format_to(std::back_inserter(s), "{:^5}", 7);
    CHECK_EQ(s, "  7  ");
  }

  TEST_CASE("spec combined fill sign width and precision") {
    CHECK_EQ(fmt("{:*>+10.2f}", 3.14159), "*****+3.14");
    CHECK_EQ(fmt("{:+#012.3e}", 271.828), "+002.718e+02");
    CHECK_EQ(fmt("{:12}", 1), "           1");
  }

  TEST_CASE("spec small integer types") {
    CHECK_EQ(fmt("{:#x}", static_cast<unsigned char>(255)), "0xff");
    CHECK_EQ(fmt("{:04}", static_cast<short>(-3)), "-003");  // NOLINT(runtime/int)
  }

  TEST_CASE("spec char presentation respects the char range") {
    if (std::numeric_limits<char>::is_signed) {
      CHECK_EQ(fmt("{:c}", -5), std::string(1, static_cast<char>(-5)));
    } else {
      CHECK_EQ(fmt("{:c}", -5), "-5");
    }

    CHECK_EQ(fmt("{:c}", 300), "300");
    CHECK_EQ(fmt("{:c}", -300), "-300");
    CHECK_EQ(fmt("{:c}", true), "1");
    CHECK_EQ(fmt("{:c}", false), "0");
    CHECK_EQ(fmt("{:s}", 'z'), "z");
  }

  TEST_CASE("spec lenient float with integral presentation types") {
    CHECK_EQ(fmt("{:x}", 2.5), "2.5");
    CHECK_EQ(fmt("{:d}", 2.5), "2.5");
    CHECK_EQ(fmt("{:s}", 2.5), "2.5");
  }

  TEST_CASE("spec negative alternate binary and octal") {
    CHECK_EQ(fmt("{:#b}", -5), "-0b101");
    CHECK_EQ(fmt("{:#o}", -8), "-010");
    CHECK_EQ(fmt("{:b}", std::numeric_limits<long long>::min()),  // NOLINT(runtime/int)
             "-1" + std::string(63, '0'));
    CHECK_EQ(fmt("{:o}", std::numeric_limits<long long>::min()),  // NOLINT(runtime/int)
             "-1000000000000000000000");
  }

  TEST_CASE("spec float precision is clamped to 340 digits") {
    char big[1024];
    auto r = format::format_to_n(big, sizeof(big), "{:.400f}", 1.5);
    CHECK_EQ(r.size, 342u);
  }

  TEST_CASE("spec float through the iterator overload") {
    std::string s;
    format::format_to(std::back_inserter(s), "{:>8.2f}", 3.14159);
    CHECK_EQ(s, "    3.14");
  }

  TEST_CASE("spec integer presentations of char use the code unit value") {
    CHECK_EQ(fmt("{:x}", '\x80'), "80");
    CHECK_EQ(fmt("{:d}", '\x80'), "128");
    CHECK_EQ(fmt("{:#x}", '\xff'), "0xff");
  }

  TEST_CASE("spec empty string_view is padded") { CHECK_EQ(fmt("{:5}", std::string_view{}), "     "); }

  TEST_CASE("spec bool char presentation keeps the remaining fields") {
    CHECK_EQ(fmt("{:*>6c}", true), "*****1");
    CHECK_EQ(fmt("{:05c}", false), "00000");
    CHECK_EQ(fmt("{:+c}", true), "+1");
  }

  TEST_CASE("spec char presentation of values above the signed char range") {
    if (std::numeric_limits<char>::is_signed) {
      CHECK_EQ(fmt("{:c}", 200), "200");
    } else {
      CHECK_EQ(fmt("{:c}", 200), std::string(1, static_cast<char>(200)));
    }
  }

  TEST_CASE("dynamic width consumes the next argument") {
    CHECK_EQ(fmt("{:{}}", 42, 5), "   42");
    CHECK_EQ(fmt("{:{}}", 42, 0), "42");
    CHECK_EQ(fmt("{:<{}}", "ab", 5), "ab   ");
    CHECK_EQ(fmt("{:0{}}", 42, 6), "000042");
    CHECK_EQ(fmt("{:{}x}", 255, 6), "    ff");
  }

  TEST_CASE("dynamic precision consumes the next argument") {
    CHECK_EQ(fmt("{:.{}f}", 3.14159, 2), "3.14");
    CHECK_EQ(fmt("{:.{}}", "hello", 3), "hel");
  }

  TEST_CASE("dynamic width and precision combine in order") {
    CHECK_EQ(fmt("{:{}.{}f}", 3.14159, 10, 3), "     3.142");
    CHECK_EQ(fmt("{:{}} {}", 1, 3, 9), "  1 9");
  }

  TEST_CASE("dynamic references support positional indexes") {
    CHECK_EQ(fmt("{0:{1}}", 7, 4), "   7");
    CHECK_EQ(fmt("{1:{0}}", 4, 7), "   7");
  }

  TEST_CASE("dynamic references degrade leniently") {
    CHECK_EQ(fmt("{:{}}", 42, -5), "42");
    CHECK_EQ(fmt("{:{}}", 42, "x"), "42");
    CHECK_EQ(fmt("{:{}}", 42), "42");
    CHECK_EQ(fmt("{:.{}f}", 2.5, -1), "2.500000");
  }

  TEST_CASE("format long double end to end") {
    CHECK_EQ(fmt("{}", 2.5L), "2.5");
    CHECK_EQ(fmt("{:.2f}", 3.14159L), "3.14");
    CHECK_EQ(fmt("{:e}", 1.5L), "1.500000e+00");
    CHECK_EQ(fmt("{:8.3f}", -2.5L), "  -2.500");
  }

  TEST_CASE("format nullptr renders as 0x0") {
    CHECK_EQ(fmt("{}", nullptr), "0x0");
    CHECK_EQ(fmt("{:>6}", nullptr), "   0x0");
  }

  TEST_CASE("format returns an allocated string") {
    CHECK_EQ(format::format("x={} y={:.1f}", 3, 4.5), "x=3 y=4.5");
    CHECK_EQ(format::format("{:>6}", "ab"), "    ab");
    CHECK_EQ(format::format("{:{}}", 42, 5), "   42");
    CHECK(format::format("").empty());
  }

  TEST_CASE("format string crosses the stack buffer boundary") {
    const std::string exact = format::format("{:>256}", "x");
    CHECK_EQ(exact.size(), 256u);
    CHECK_EQ(exact.back(), 'x');

    const std::string beyond = format::format("{:>257}", "x");
    CHECK_EQ(beyond.size(), 257u);
    CHECK_EQ(beyond.back(), 'x');
    CHECK_EQ(beyond[0], ' ');
  }

  TEST_CASE("long double fixed output covers the full exponent range") {
    char big[6000];
    auto r = format::format_to_n(big, sizeof(big), "{:.0f}", std::numeric_limits<long double>::max());
    CHECK_EQ(r.size, static_cast<size_t>(std::numeric_limits<long double>::max_exponent10) + 1u);
    CHECK_FALSE(r.truncated);

    r = format::format_to_n(big, sizeof(big), "{:.340f}", -std::numeric_limits<long double>::max());
    CHECK_EQ(r.size, static_cast<size_t>(std::numeric_limits<long double>::max_exponent10) + 343u);
    CHECK_FALSE(r.truncated);
  }

  TEST_CASE("malformed nested references do not fail") {
    CHECK_EQ(fmt("{:{x}}", 26), "1a}");
    CHECK_EQ(fmt("{:{:{}}}", 42), "42}");
    CHECK_EQ(fmt("{:{0}}", 5), "    5");
  }

  TEST_CASE("format_as maps custom types to formattable values") {
    CHECK_EQ(fmt("{}", format_as_test::Meters{2.5}), "2.5");
    CHECK_EQ(fmt("{:.1f}", format_as_test::Meters{2.5}), "2.5");
    CHECK_EQ(fmt("{}", format_as_test::Level::kInfo), "10");
    CHECK_EQ(fmt("{:04}", format_as_test::Level::kInfo), "0010");

    const format_as_test::Tag tag{"core"};
    CHECK_EQ(fmt("{}/{:>6}", tag, tag), "core/  core");
    CHECK_EQ(format::format("{}", format_as_test::Meters{1.5}), "1.5");
  }

  TEST_CASE("debug presentation escapes strings") {
    CHECK_EQ(fmt("{:?}", "ab"), "\"ab\"");
    CHECK_EQ(fmt("{:?}", "a\"b\\"), "\"a\\\"b\\\\\"");
    CHECK_EQ(fmt("{:?}", "a\tb\n"), "\"a\\tb\\n\"");
    CHECK_EQ(fmt("{:?}", "\x01\x1f\x7f"), "\"\\u{1}\\u{1f}\\u{7f}\"");
    CHECK_EQ(fmt("{:>10?}", "ab"), "      \"ab\"");
    CHECK_EQ(fmt("{:.0?}", "ab"), "\"\"");
    CHECK_EQ(fmt("{:.2?}", "a\nb"), "\"a\\n\"");
  }

  TEST_CASE("debug presentation escapes chars") {
    CHECK_EQ(fmt("{:?}", 'q'), "'q'");
    CHECK_EQ(fmt("{:?}", '\''), "'\\''");
    CHECK_EQ(fmt("{:?}", '\n'), "'\\n'");
    CHECK_EQ(fmt("{:.1?}", 'q'), "'q'");
  }
}

// NOLINTEND
