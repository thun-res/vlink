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

#include "./base/fast_stream.h"

#include <doctest/doctest.h>

#include <iomanip>
#include <string>
#include <string_view>

#include "../common_test.h"

TEST_SUITE("base-FastStream") {
  TEST_CASE("default construction yields empty buffer") {
    FastStream s;

    CHECK_EQ(s.size(), 0u);
    CHECK(s.capacity() >= 256u);
    CHECK(s.take_view().empty());
  }

  TEST_CASE("operator<< accumulates string literals") {
    FastStream s;
    s << "hello" << " " << "world";

    CHECK_EQ(s.take_view(), "hello world");
    CHECK_EQ(s.size(), 11u);
  }

  TEST_CASE("operator<< formats integers") {
    FastStream s;
    s << 42 << " " << -7;

    CHECK_EQ(s.take_view(), "42 -7");
  }

  TEST_CASE("operator<< formats floating point via stream locale") {
    FastStream s;
    s << 3.14;

    CHECK_FALSE(s.take_view().empty());
    CHECK(s.take_view().find("3.14") != std::string_view::npos);
  }

  TEST_CASE("operator<< respects std::hex manipulator") {
    FastStream s;
    s << std::hex << 255;

    CHECK_EQ(s.take_view(), "ff");
  }

  TEST_CASE("operator<< respects std::boolalpha") {
    FastStream s;
    s << std::boolalpha << true << "/" << false;

    CHECK_EQ(s.take_view(), "true/false");
  }

  TEST_CASE("operator<< respects std::setw and std::setfill") {
    FastStream s;
    s << std::setw(5) << std::setfill('0') << 42;

    CHECK_EQ(s.take_view(), "00042");
  }

  TEST_CASE("bool without boolalpha formats as 1 and 0") {
    FastStream s;
    s << true << "/" << false;

    CHECK_EQ(s.take_view(), "1/0");
  }

  TEST_CASE("take_view returns buffer content without resetting") {
    FastStream s;
    s << "vlink";

    std::string_view v1 = s.take_view();
    std::string_view v2 = s.take_view();

    CHECK_EQ(v1, "vlink");
    CHECK_EQ(v2, "vlink");
    CHECK_EQ(s.size(), 5u);
  }

  TEST_CASE("take_view on empty stream returns empty view") {
    FastStream s;

    CHECK(s.take_view().empty());
  }

  TEST_CASE("reset clears size and retains capacity") {
    FastStream s;
    s << "some content";
    size_t cap = s.capacity();

    s.reset();

    CHECK_EQ(s.size(), 0u);
    CHECK_EQ(s.capacity(), cap);
    CHECK(s.take_view().empty());
  }

  TEST_CASE("reset allows writing fresh content") {
    FastStream s;
    s << "first";
    s.reset();
    s << "second";

    CHECK_EQ(s.take_view(), "second");
  }

  TEST_CASE("append_to adds buffer content to external string") {
    FastStream s;
    s << "appended";
    std::string target = "prefix-";

    s.append_to(target);

    CHECK_EQ(target, "prefix-appended");
    CHECK_EQ(s.size(), 8u);
  }

  TEST_CASE("append_to on empty stream leaves target unchanged") {
    FastStream s;
    std::string target = "base";

    s.append_to(target);

    CHECK_EQ(target, "base");
  }

  TEST_CASE("append_to can be called multiple times to accumulate") {
    FastStream s;
    std::string result;

    s << "part1";
    s.append_to(result);
    s.reset();

    s << "part2";
    s.append_to(result);

    CHECK_EQ(result, "part1part2");
  }

  TEST_CASE("write_raw appends raw bytes bypassing ostream formatting") {
    FastStream s;
    s.write_raw("rawbytes", 8);

    CHECK_EQ(s.take_view(), "rawbytes");
    CHECK_EQ(s.size(), 8u);
  }

  TEST_CASE("write_raw with zero length is a no-op") {
    FastStream s;
    s << "before";
    size_t sz = s.size();

    s.write_raw("ignored", 0);

    CHECK_EQ(s.size(), sz);
    CHECK_EQ(s.take_view(), "before");
  }

  TEST_CASE("write_raw can be interleaved with operator<<") {
    FastStream s;
    s << "A";
    s.write_raw("B", 1);
    s << "C";

    CHECK_EQ(s.take_view(), "ABC");
  }

  TEST_CASE("size matches length of written content") {
    FastStream s;
    std::string content = "measure me";
    s << content;

    CHECK_EQ(s.size(), content.size());
  }

  TEST_CASE("capacity is at least 256 after construction") {
    FastStream s;

    CHECK(s.capacity() >= 256u);
  }

  TEST_CASE("buffer grows beyond 256 for large content") {
    FastStream s;
    std::string large(1000, 'X');
    s << large;

    CHECK_EQ(s.size(), 1000u);
    CHECK(s.capacity() >= 1000u);

    std::string_view sv = s.take_view();
    CHECK_EQ(sv.size(), 1000u);
    CHECK_EQ(sv.front(), 'X');
    CHECK_EQ(sv.back(), 'X');
  }

  TEST_CASE("shrink_to_fit does not corrupt stream") {
    FastStream s;
    std::string large(8000, 'Y');
    s << large;

    s.reset();
    s.shrink_to_fit();

    CHECK_EQ(s.size(), 0u);

    s << "ok";
    CHECK_EQ(s.take_view(), "ok");
  }

  TEST_CASE("consecutive resets and writes are correct") {
    FastStream s;

    for (int i = 0; i < 5; ++i) {
      s << "iter" << i;
      CHECK_FALSE(s.take_view().empty());
      s.reset();
    }

    CHECK_EQ(s.size(), 0u);
  }

  TEST_CASE("stream supports std::endl") {
    FastStream s;
    s << "line" << std::endl;

    CHECK(s.size() >= 5u);
  }

  TEST_CASE("stream is not copyable") {
    CHECK_FALSE(std::is_copy_constructible_v<FastStream>);
    CHECK_FALSE(std::is_copy_assignable_v<FastStream>);
  }
}

// NOLINTEND
