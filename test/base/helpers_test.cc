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

#include "./base/helpers.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "../common_test.h"

TEST_SUITE("base-Helpers") {
  TEST_CASE("to_int: valid and invalid inputs") {
    SUBCASE("positive decimal") { CHECK_EQ(Helpers::to_int("42"), 42); }
    SUBCASE("negative decimal") { CHECK_EQ(Helpers::to_int("-7"), -7); }
    SUBCASE("zero") { CHECK_EQ(Helpers::to_int("0"), 0); }
    SUBCASE("hex 0x prefix") { CHECK_EQ(Helpers::to_int("0xff"), 255); }
    SUBCASE("octal prefix") { CHECK_EQ(Helpers::to_int("010"), 8); }
    SUBCASE("leading whitespace") { CHECK_EQ(Helpers::to_int("  42"), 42); }
    SUBCASE("invalid string returns default 0") { CHECK_EQ(Helpers::to_int("abc"), 0); }
    SUBCASE("invalid string returns supplied default") { CHECK_EQ(Helpers::to_int("xyz", 99), 99); }
    SUBCASE("empty string returns default") { CHECK_EQ(Helpers::to_int("", -1), -1); }
    SUBCASE("partial numeric returns default") { CHECK_EQ(Helpers::to_int("42abc"), 0); }
    SUBCASE("trailing whitespace returns default") { CHECK_EQ(Helpers::to_int("42 ", -1), -1); }
    SUBCASE("overflow returns default") { CHECK_EQ(Helpers::to_int("99999999999999999999", -1), -1); }
  }

  TEST_CASE("to_long: valid and invalid inputs") {
    SUBCASE("large positive") { CHECK_EQ(Helpers::to_long("123456789"), 123456789LL); }
    SUBCASE("negative") { CHECK_EQ(Helpers::to_long("-100"), -100LL); }
    SUBCASE("zero") { CHECK_EQ(Helpers::to_long("0"), 0LL); }
    SUBCASE("hex prefix") { CHECK_EQ(Helpers::to_long("0xff"), 255LL); }
    SUBCASE("empty returns default") { CHECK_EQ(Helpers::to_long("", 42), 42LL); }
    SUBCASE("invalid returns default") { CHECK_EQ(Helpers::to_long("bad", -1), -1LL); }
    SUBCASE("overflow returns default") { CHECK_EQ(Helpers::to_long("99999999999999999999", -1), -1LL); }
    SUBCASE("full parse with offset 0") { CHECK_EQ(Helpers::to_long("12", 0, 0), 12LL); }
    SUBCASE("partial parse with offset returns default") { CHECK_EQ(Helpers::to_long("1234", -1, 2), -1LL); }
    SUBCASE("negative with offset 0") { CHECK_EQ(Helpers::to_long("-999", 0, 0), -999LL); }
  }

  TEST_CASE("double_to_string: various values") {
    SUBCASE("default precision has decimal point") {
      std::string s = Helpers::double_to_string(3.14);
      CHECK_FALSE(s.empty());
      CHECK(s.find('.') != std::string::npos);
    }
    SUBCASE("zero") {
      std::string s = Helpers::double_to_string(0.0);
      CHECK_FALSE(s.empty());
    }
    SUBCASE("negative") {
      std::string s = Helpers::double_to_string(-1.5);
      CHECK(s.find('-') != std::string::npos);
    }
    SUBCASE("custom precision 0 omits decimal") {
      std::string s = Helpers::double_to_string(3.7, 0);
      CHECK_FALSE(s.empty());
    }
    SUBCASE("high precision") {
      std::string s = Helpers::double_to_string(1.0 / 3.0, 10);
      auto dot_pos = s.find('.');
      REQUIRE(dot_pos != std::string::npos);
      CHECK(s.size() - dot_pos - 1 >= 10u);
    }
    SUBCASE("large value") { CHECK_FALSE(Helpers::double_to_string(1e18).empty()); }
  }

  TEST_CASE("hash_combine: determinism and differentiation") {
    SUBCASE("same inputs produce same hash") {
      uint64_t h1 = Helpers::hash_combine(100ULL, 200ULL);
      uint64_t h2 = Helpers::hash_combine(100ULL, 200ULL);
      CHECK_EQ(h1, h2);
    }
    SUBCASE("order matters") {
      uint64_t h1 = Helpers::hash_combine(1ULL, 2ULL);
      uint64_t h2 = Helpers::hash_combine(2ULL, 1ULL);
      CHECK_NE(h1, h2);
    }
    SUBCASE("zero inputs does not crash") {
      uint64_t h = Helpers::hash_combine(0ULL, 0ULL);
      (void)h;
    }
  }

  TEST_CASE("replace_string: substitution behavior") {
    SUBCASE("single occurrence") {
      std::string s = "hello world";
      Helpers::replace_string(s, "world", "VLink");
      CHECK_EQ(s, "hello VLink");
    }
    SUBCASE("all occurrences") {
      std::string s = "aaa";
      Helpers::replace_string(s, "a", "bb");
      CHECK_EQ(s, "bbbbbb");
    }
    SUBCASE("no match leaves unchanged") {
      std::string s = "hello";
      Helpers::replace_string(s, "xyz", "abc");
      CHECK_EQ(s, "hello");
    }
    SUBCASE("empty from string is no-op") {
      std::string s = "hello";
      Helpers::replace_string(s, "", "X");
      CHECK_EQ(s, "hello");
    }
    SUBCASE("replace with empty removes occurrences") {
      std::string s = "hello world";
      Helpers::replace_string(s, "world", "");
      CHECK_EQ(s, "hello ");
    }
    SUBCASE("replace with longer string") {
      std::string s = "ab";
      Helpers::replace_string(s, "a", "xyz");
      CHECK_EQ(s, "xyzb");
    }
    SUBCASE("replace with same string") {
      std::string s = "hello";
      Helpers::replace_string(s, "hello", "hello");
      CHECK_EQ(s, "hello");
    }
    SUBCASE("empty source string is no-op") {
      std::string s;
      Helpers::replace_string(s, "a", "b");
      CHECK(s.empty());
    }
  }

  TEST_CASE("trim_string: whitespace removal") {
    SUBCASE("leading spaces") { CHECK_EQ(Helpers::trim_string("   hello"), "hello"); }
    SUBCASE("trailing spaces") { CHECK_EQ(Helpers::trim_string("hello   "), "hello"); }
    SUBCASE("both sides") { CHECK_EQ(Helpers::trim_string("  hello  "), "hello"); }
    SUBCASE("empty string") { CHECK_EQ(Helpers::trim_string(""), ""); }
    SUBCASE("all whitespace") { CHECK_EQ(Helpers::trim_string(" \t\r\n "), ""); }
    SUBCASE("no whitespace") { CHECK_EQ(Helpers::trim_string("hello"), "hello"); }
    SUBCASE("tabs and newlines") { CHECK_EQ(Helpers::trim_string("\t\nhello\t\n"), "hello"); }
    SUBCASE("single character") { CHECK_EQ(Helpers::trim_string("x"), "x"); }
    SUBCASE("inner whitespace preserved") { CHECK_EQ(Helpers::trim_string("  a b  "), "a b"); }
  }

  TEST_CASE("get_split_string: splitting behavior") {
    SUBCASE("normal split") {
      auto parts = Helpers::get_split_string("a,b,c", ',');
      REQUIRE_EQ(parts.size(), 3u);
      CHECK_EQ(parts[0], "a");
      CHECK_EQ(parts[1], "b");
      CHECK_EQ(parts[2], "c");
    }
    SUBCASE("no delimiter returns single element") {
      auto parts = Helpers::get_split_string("hello", ',');
      REQUIRE_EQ(parts.size(), 1u);
      CHECK_EQ(parts[0], "hello");
    }
    SUBCASE("empty string returns empty vector") {
      auto parts = Helpers::get_split_string("", ',');
      CHECK(parts.empty());
    }
    SUBCASE("consecutive delimiters skip empty tokens") {
      auto parts = Helpers::get_split_string("a,,b", ',');
      REQUIRE_EQ(parts.size(), 2u);
      CHECK_EQ(parts[0], "a");
      CHECK_EQ(parts[1], "b");
    }
    SUBCASE("leading delimiter skips empty token") {
      auto parts = Helpers::get_split_string(",a", ',');
      REQUIRE_EQ(parts.size(), 1u);
      CHECK_EQ(parts[0], "a");
    }
    SUBCASE("trailing delimiter skips trailing empty") {
      auto parts = Helpers::get_split_string("a,b,", ',');
      REQUIRE_EQ(parts.size(), 2u);
      CHECK_EQ(parts[0], "a");
      CHECK_EQ(parts[1], "b");
    }
    SUBCASE("delimiter only string returns empty") {
      auto parts = Helpers::get_split_string(",", ',');
      CHECK(parts.empty());
    }
    SUBCASE("multiple consecutive delimiters return empty") {
      auto parts = Helpers::get_split_string(",,,", ',');
      CHECK(parts.empty());
    }
    SUBCASE("single character string") {
      auto parts = Helpers::get_split_string("x", ',');
      REQUIRE_EQ(parts.size(), 1u);
      CHECK_EQ(parts[0], "x");
    }
  }

  TEST_CASE("get_split_string_view: splitting into views") {
    SUBCASE("basic split") {
      std::string src = "x:y:z";
      auto parts = Helpers::get_split_string_view(src, ':');
      REQUIRE_EQ(parts.size(), 3u);
      CHECK_EQ(parts[0], "x");
      CHECK_EQ(parts[1], "y");
      CHECK_EQ(parts[2], "z");
    }
    SUBCASE("no delimiter returns one view") {
      std::string src = "hello";
      auto parts = Helpers::get_split_string_view(src, '/');
      REQUIRE_EQ(parts.size(), 1u);
      CHECK_EQ(parts[0], "hello");
    }
    SUBCASE("empty string returns empty") {
      auto parts = Helpers::get_split_string_view("", ':');
      CHECK(parts.empty());
    }
    SUBCASE("consecutive delimiters skip empty") {
      auto parts = Helpers::get_split_string_view("a::b", ':');
      REQUIRE_EQ(parts.size(), 2u);
      CHECK_EQ(parts[0], "a");
      CHECK_EQ(parts[1], "b");
    }
    SUBCASE("trailing delimiter skips trailing empty") {
      auto parts = Helpers::get_split_string_view("a:b:", ':');
      REQUIRE_EQ(parts.size(), 2u);
      CHECK_EQ(parts[0], "a");
      CHECK_EQ(parts[1], "b");
    }
  }

  TEST_CASE("get_pair_string: key-value parsing with trim") {
    SUBCASE("basic split") {
      auto p = Helpers::get_pair_string("key=value", '=');
      CHECK_EQ(p.first, "key");
      CHECK_EQ(p.second, "value");
    }
    SUBCASE("no delimiter returns whole in first, empty second") {
      auto p = Helpers::get_pair_string("noeq", '=');
      CHECK_EQ(p.first, "noeq");
      CHECK(p.second.empty());
    }
    SUBCASE("splits at first delimiter only") {
      auto p = Helpers::get_pair_string("a=b=c", '=');
      CHECK_EQ(p.first, "a");
      CHECK_EQ(p.second, "b=c");
    }
    SUBCASE("spaces are trimmed") {
      auto p = Helpers::get_pair_string("key = value", '=');
      CHECK_EQ(p.first, "key");
      CHECK_EQ(p.second, "value");
    }
    SUBCASE("empty string returns empty pair") {
      auto p = Helpers::get_pair_string("", '=');
      CHECK(p.first.empty());
      CHECK(p.second.empty());
    }
    SUBCASE("delimiter at start") {
      auto p = Helpers::get_pair_string(",b", ',');
      CHECK(p.first.empty());
      CHECK_EQ(p.second, "b");
    }
  }

  TEST_CASE("get_pair_string_view: key-value parsing without trim") {
    SUBCASE("basic split") {
      auto p = Helpers::get_pair_string_view("key=value", '=');
      CHECK_EQ(p.first, "key");
      CHECK_EQ(p.second, "value");
    }
    SUBCASE("no delimiter") {
      auto p = Helpers::get_pair_string_view("noeq", '=');
      CHECK_EQ(p.first, "noeq");
      CHECK(p.second.empty());
    }
    SUBCASE("splits at first delimiter") {
      auto p = Helpers::get_pair_string_view("a=b=c", '=');
      CHECK_EQ(p.first, "a");
      CHECK_EQ(p.second, "b=c");
    }
    SUBCASE("empty string") {
      auto p = Helpers::get_pair_string_view("", '=');
      CHECK(p.first.empty());
      CHECK(p.second.empty());
    }
    SUBCASE("delimiter at start") {
      auto p = Helpers::get_pair_string_view(",b", ',');
      CHECK(p.first.empty());
      CHECK_EQ(p.second, "b");
    }
    SUBCASE("delimiter at end") {
      auto p = Helpers::get_pair_string_view("a,", ',');
      CHECK_EQ(p.first, "a");
      CHECK(p.second.empty());
    }
    SUBCASE("views reference original buffer") {
      std::string original = "hello:world";
      auto p = Helpers::get_pair_string_view(original, ':');
      CHECK(p.first.data() >= original.data());
      CHECK(p.second.data() >= original.data());
    }
    SUBCASE("no trim applied") {
      auto p = Helpers::get_pair_string_view(" key = value ", '=');
      CHECK_EQ(p.first, " key ");
      CHECK_EQ(p.second, " value ");
    }
  }

  TEST_CASE("escape_field and unescape_field: round-trip") {
    SUBCASE("plain field unchanged") { CHECK_EQ(Helpers::escape_field("plain_text-123"), "plain_text-123"); }
    SUBCASE("escapes special characters") {
      CHECK_EQ(Helpers::escape_field("host name:app%\n\r"), "host%20name%3Aapp%25%0A%0D");
    }
    SUBCASE("unescapes valid sequences") {
      CHECK_EQ(Helpers::unescape_field("host%20name%3Aapp%25%0A%0D"), "host name:app%\n\r");
    }
    SUBCASE("invalid percent sequences kept literal") { CHECK_EQ(Helpers::unescape_field("a%ZZ%b%2"), "a%ZZ%b%2"); }
    SUBCASE("round trip for complex URL-like string") {
      const std::string input = "dds://topic with spaces:field%name";
      CHECK_EQ(Helpers::unescape_field(Helpers::escape_field(input)), input);
    }
  }

  TEST_CASE("get_hash_code: determinism and numeric strings") {
    SUBCASE("same string produces same hash") {
      CHECK_EQ(Helpers::get_hash_code("hello"), Helpers::get_hash_code("hello"));
    }
    SUBCASE("different strings produce different hashes") {
      CHECK_NE(Helpers::get_hash_code("hello"), Helpers::get_hash_code("world"));
    }
    SUBCASE("empty string returns 0") { CHECK_EQ(Helpers::get_hash_code(""), 0u); }
    SUBCASE("numeric string returns parsed number") { CHECK_EQ(Helpers::get_hash_code("42"), 42u); }
    SUBCASE("non-numeric string returns non-zero hash") { CHECK_NE(Helpers::get_hash_code("hello"), 0u); }
  }

  TEST_CASE("format_milliseconds: time formatting") {
    SUBCASE("two minutes five seconds without millis") {
      CHECK_FALSE(Helpers::format_milliseconds(125000LL, false).empty());
    }
    SUBCASE("two minutes five seconds with millis") {
      CHECK_FALSE(Helpers::format_milliseconds(125000LL, true).empty());
    }
    SUBCASE("zero duration") { CHECK_FALSE(Helpers::format_milliseconds(0LL, false).empty()); }
    SUBCASE("exact hour boundary") {
      std::string s = Helpers::format_milliseconds(3600000LL, false);
      CHECK(s.find("01:00:00") != std::string::npos);
    }
    SUBCASE("with millis shows extra precision") {
      std::string s = Helpers::format_milliseconds(1500LL, true);
      CHECK(s.find("500") != std::string::npos);
    }
  }

  TEST_CASE("format_date: timestamp to readable string") {
    SUBCASE("epoch zero produces valid non-empty string") { CHECK_FALSE(Helpers::format_date(0LL).empty()); }
    SUBCASE("2020-01-01 UTC known timestamp contains year") {
      int64_t ts = 1577836800LL * 1000000000LL;
      std::string s = Helpers::format_date(ts);
      CHECK(s.find("2020") != std::string::npos);
      CHECK(s.find(":") != std::string::npos);
    }
    SUBCASE("millisecond precision shown") {
      int64_t ts = 1577836800LL * 1000000000LL + 123000000LL;
      std::string s = Helpers::format_date(ts);
      CHECK(s.find(".123") != std::string::npos);
    }
  }

  TEST_CASE("format_time_diff: human-readable time difference") {
    SUBCASE("small ms shows non-empty") { CHECK_FALSE(Helpers::format_time_diff(250).empty()); }
    SUBCASE("large ms shows non-empty") { CHECK_FALSE(Helpers::format_time_diff(5000000).empty()); }
    SUBCASE("zero does not crash") { CHECK_FALSE(Helpers::format_time_diff(0).empty()); }
    SUBCASE("negative has minus sign") {
      std::string s = Helpers::format_time_diff(-5000);
      CHECK_EQ(s[0], '-');
    }
    SUBCASE("exact second") { CHECK_FALSE(Helpers::format_time_diff(1000).empty()); }
  }

  TEST_CASE("format_hex_number: hexadecimal formatting") {
    SUBCASE("signed 255 has 0x prefix") {
      std::string s = Helpers::format_hex_number(static_cast<int64_t>(255));
      CHECK_EQ(s.substr(0, 2), "0x");
    }
    SUBCASE("unsigned 255 has 0x prefix") {
      std::string s = Helpers::format_hex_number(static_cast<uint64_t>(255u));
      CHECK_EQ(s.substr(0, 2), "0x");
    }
    SUBCASE("signed 0 has 0x prefix") {
      std::string s = Helpers::format_hex_number(static_cast<int64_t>(0));
      CHECK_EQ(s.substr(0, 2), "0x");
    }
    SUBCASE("signed 16 produces 0x10") { CHECK_EQ(Helpers::format_hex_number(static_cast<int64_t>(16)), "0x10"); }
    SUBCASE("unsigned 0xFFFF produces 0xFFFF") {
      CHECK_EQ(Helpers::format_hex_number(static_cast<uint64_t>(0xFFFF)), "0xFFFF");
    }
    SUBCASE("signed negative has 0x prefix") {
      std::string s = Helpers::format_hex_number(static_cast<int64_t>(-1));
      CHECK_EQ(s.substr(0, 2), "0x");
    }
  }

  TEST_CASE("format_file_size: human-readable size") {
    SUBCASE("bytes range has B") {
      std::string s = Helpers::format_file_size(512u);
      CHECK_FALSE(s.empty());
      CHECK(s.find('B') != std::string::npos);
    }
    SUBCASE("kilobytes range has K") {
      std::string s = Helpers::format_file_size(1536u);
      CHECK(s.find('K') != std::string::npos);
    }
    SUBCASE("exact 1 KB") { CHECK(Helpers::format_file_size(1024u).find('K') != std::string::npos); }
    SUBCASE("megabytes range has M") {
      CHECK(Helpers::format_file_size(1024u * 1024u * 2u).find('M') != std::string::npos);
    }
    SUBCASE("exact 1 MB") { CHECK(Helpers::format_file_size(1024u * 1024u).find('M') != std::string::npos); }
    SUBCASE("gigabytes range has G") {
      CHECK(Helpers::format_file_size(static_cast<size_t>(2) * 1024 * 1024 * 1024).find('G') != std::string::npos);
    }
    SUBCASE("zero size does not crash") { CHECK_FALSE(Helpers::format_file_size(0u).empty()); }
  }

  TEST_CASE("format_rate_size: human-readable bandwidth") {
    SUBCASE("below 1 KB shows B/s") { CHECK(Helpers::format_rate_size(500u).find("B/s") != std::string::npos); }
    SUBCASE("kilobytes range shows KB/s") { CHECK(Helpers::format_rate_size(2048u).find("KB/s") != std::string::npos); }
    SUBCASE("megabytes range has M") { CHECK(Helpers::format_rate_size(1024u * 1024u).find('M') != std::string::npos); }
    SUBCASE("gigabytes range shows GB/s") {
      CHECK(Helpers::format_rate_size(static_cast<size_t>(2) * 1024 * 1024 * 1024).find("GB/s") != std::string::npos);
    }
    SUBCASE("zero rate does not crash") { CHECK_FALSE(Helpers::format_rate_size(0u).empty()); }
  }

  TEST_CASE("convert_date_to_timestamp: parsing date strings") {
    SUBCASE("valid date with time returns positive") {
      CHECK(Helpers::convert_date_to_timestamp("2026/01/01 00:00:00") > 0);
    }
    SUBCASE("date with milliseconds delimiter") {
      CHECK(Helpers::convert_date_to_timestamp("2026/01/01 00:00:00:500") > 0);
    }
    SUBCASE("invalid date string returns -1") { CHECK_EQ(Helpers::convert_date_to_timestamp("not-a-date"), -1); }
    SUBCASE("empty string returns -1") { CHECK_EQ(Helpers::convert_date_to_timestamp(""), -1); }
    SUBCASE("wrong milliseconds separator returns -1") {
      CHECK_EQ(Helpers::convert_date_to_timestamp("2026/01/01 00:00:00.500"), -1);
    }
    SUBCASE("out-of-range milliseconds returns -1") {
      CHECK_EQ(Helpers::convert_date_to_timestamp("2026/01/01 00:00:00:1500"), -1);
    }
  }

  TEST_CASE("has_startwith: prefix checking") {
    SUBCASE("string starts with prefix literal") {
      std::string s = "hello world";
      CHECK(Helpers::has_startwith(s, "hello"));
    }
    SUBCASE("string does not start with wrong prefix") {
      std::string s = "world hello";
      CHECK_FALSE(Helpers::has_startwith(s, "hello"));
    }
    SUBCASE("empty string does not start with non-empty prefix") {
      std::string s;
      CHECK_FALSE(Helpers::has_startwith(s, "hello"));
    }
    SUBCASE("exact match") { CHECK(Helpers::has_startwith(std::string("hello"), "hello")); }
    SUBCASE("prefix longer than string") { CHECK_FALSE(Helpers::has_startwith(std::string("hi"), "hello")); }
    SUBCASE("string_view: starts with non-empty prefix") {
      CHECK(Helpers::has_startwith(std::string_view("foobar"), std::string_view("foo")));
    }
    SUBCASE("string_view: wrong prefix") {
      CHECK_FALSE(Helpers::has_startwith(std::string_view("foobar"), std::string_view("bar")));
    }
    SUBCASE("string_view: empty prefix always true") {
      CHECK(Helpers::has_startwith(std::string_view("foobar"), std::string_view("")));
    }
    SUBCASE("string_view: empty string with empty prefix") {
      CHECK(Helpers::has_startwith(std::string_view(""), std::string_view("")));
    }
    SUBCASE("string_view: empty string with non-empty prefix") {
      CHECK_FALSE(Helpers::has_startwith(std::string_view(""), std::string_view("x")));
    }
  }

  TEST_CASE("has_endwith: suffix checking") {
    SUBCASE("string ends with suffix literal") { CHECK(Helpers::has_endwith(std::string("hello world"), "world")); }
    SUBCASE("string does not end with wrong suffix") {
      CHECK_FALSE(Helpers::has_endwith(std::string("hello world"), "hello"));
    }
    SUBCASE("empty string does not end with non-empty suffix") {
      CHECK_FALSE(Helpers::has_endwith(std::string(""), "x"));
    }
    SUBCASE("exact match") { CHECK(Helpers::has_endwith(std::string("world"), "world")); }
    SUBCASE("suffix longer than string") { CHECK_FALSE(Helpers::has_endwith(std::string("hi"), "hello")); }
    SUBCASE("string_view: ends with suffix") {
      CHECK(Helpers::has_endwith(std::string_view("foobar"), std::string_view("bar")));
    }
    SUBCASE("string_view: wrong suffix") {
      CHECK_FALSE(Helpers::has_endwith(std::string_view("foobar"), std::string_view("foo")));
    }
    SUBCASE("string_view: empty suffix always true") {
      CHECK(Helpers::has_endwith(std::string_view("foobar"), std::string_view("")));
    }
    SUBCASE("string_view: empty string with non-empty suffix") {
      CHECK_FALSE(Helpers::has_endwith(std::string_view(""), std::string_view("x")));
    }
  }

  TEST_CASE("contains_substring: substring search") {
    SUBCASE("substring present") { CHECK(Helpers::contains_substring("hello world", "world")); }
    SUBCASE("substring not present") { CHECK_FALSE(Helpers::contains_substring("hello world", "xyz")); }
    SUBCASE("empty needle always returns true") { CHECK(Helpers::contains_substring("hello", "")); }
    SUBCASE("empty haystack with non-empty needle") { CHECK_FALSE(Helpers::contains_substring("", "x")); }
    SUBCASE("both empty returns true") { CHECK(Helpers::contains_substring("", "")); }
    SUBCASE("needle equals haystack") { CHECK(Helpers::contains_substring("hello", "hello")); }
    SUBCASE("needle longer than haystack") { CHECK_FALSE(Helpers::contains_substring("hi", "hello")); }
    SUBCASE("at start") { CHECK(Helpers::contains_substring("hello world", "hello")); }
    SUBCASE("at end") { CHECK(Helpers::contains_substring("hello world", "world")); }
    SUBCASE("single char needle") { CHECK(Helpers::contains_substring("abc", "b")); }
    SUBCASE("repeated pattern") { CHECK(Helpers::contains_substring("abcabc", "cab")); }
  }

  TEST_CASE("string_to_wstring and wstring_to_string: ASCII round-trip") {
    SUBCASE("ASCII string") {
      std::string original = "hello";
      std::wstring wide = Helpers::string_to_wstring(original);
      std::string back = Helpers::wstring_to_string(wide);
      CHECK_EQ(back, original);
    }
    SUBCASE("empty string") {
      std::string original;
      std::wstring wide = Helpers::string_to_wstring(original);
      std::string back = Helpers::wstring_to_string(wide);
      CHECK_EQ(back, original);
    }
  }

  TEST_CASE("string_local_to_utf8 and string_utf8_to_local: identity on POSIX") {
    SUBCASE("non-empty") { CHECK_EQ(Helpers::string_local_to_utf8("hello"), "hello"); }
    SUBCASE("empty") { CHECK_EQ(Helpers::string_local_to_utf8(""), ""); }
    SUBCASE("utf8 to local non-empty") { CHECK_EQ(Helpers::string_utf8_to_local("world"), "world"); }
    SUBCASE("utf8 to local empty") { CHECK_EQ(Helpers::string_utf8_to_local(""), ""); }
  }

  TEST_CASE("path_to_string: filesystem path to string") {
    SUBCASE("temp path with component") {
      std::filesystem::path p = std::filesystem::temp_directory_path() / "test";
      std::string s = Helpers::path_to_string(p);
      CHECK_FALSE(s.empty());
      CHECK(s.find("test") != std::string::npos);
    }
    SUBCASE("empty path does not crash") {
      std::filesystem::path p = "";
      std::string s = Helpers::path_to_string(p);
      (void)s;
    }
  }
}

// NOLINTEND
