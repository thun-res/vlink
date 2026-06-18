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

#include "./extension/terminal_stream.h"

#if defined(__unix__) && !defined(__CYGWIN__)

#include <doctest/doctest.h>

#include <limits>
#include <ostream>
#include <string>
#include <string_view>

#include "../common_test.h"

TEST_SUITE("extension-TerminalStream") {
  TEST_CASE("get returns the same singleton reference on every call") {
    TerminalStream& a = TerminalStream::get();
    TerminalStream& b = TerminalStream::get();
    CHECK_EQ(&a, &b);
  }

  TEST_CASE("kDefaultBufferSize equals 1 MiB") { CHECK_EQ(TerminalStream::kDefaultBufferSize, 1024u * 1024u * 1u); }

  TEST_CASE("init is idempotent and sets is_initialized") {
    TerminalStream& ts = TerminalStream::get();
    ts.init();
    CHECK(ts.is_initialized());
    ts.init();
    CHECK(ts.is_initialized());
  }

  TEST_CASE("is_tty is accessible after init without crash") {
    TerminalStream& ts = TerminalStream::get();
    ts.init();
    [[maybe_unused]] bool tty = ts.is_tty();
  }

  TEST_CASE("flush does not crash on empty buffer") {
    TerminalStream& ts = TerminalStream::get();
    ts.flush();
  }

  TEST_CASE("flush does not crash after writing content") {
    TerminalStream& ts = TerminalStream::get();
    ts << "test flush";
    ts.flush();
  }

  TEST_CASE("operator<< char writes without crash") { TerminalStream::get() << 'X'; }

  TEST_CASE("operator<< null const char pointer is a no-op") {
    const char* null_str = nullptr;
    TerminalStream::get() << null_str;
  }

  TEST_CASE("operator<< string types write without crash") {
    TerminalStream& ts = TerminalStream::get();
    SUBCASE("const char*") { ts << "hello terminal"; }
    SUBCASE("std::string") {
      const std::string s = "std string";
      ts << s;
    }
    SUBCASE("std::string_view") {
      const std::string_view sv = "string view";
      ts << sv;
    }
  }

  TEST_CASE("operator<< bool writes true and false without crash") {
    TerminalStream& ts = TerminalStream::get();
    ts << true;
    ts << false;
  }

  TEST_CASE("operator<< integer types write without crash") {
    TerminalStream& ts = TerminalStream::get();
    ts << 42;
    ts << -999;
    ts << 0;
    ts << 100u;
    ts << 123456L;
    ts << 9876543210LL;
    ts << 18446744073709551615ULL;
  }

  TEST_CASE("operator<< floating-point types write without crash") {
    TerminalStream& ts = TerminalStream::get();
    ts << 3.14f;
    ts << 2.718281828;
    ts << 1.41421356237L;
  }

  TEST_CASE("operator<< pointer writes without crash") {
    TerminalStream& ts = TerminalStream::get();
    void* ptr = &ts;
    ts << ptr;
    const void* null_ptr = nullptr;
    ts << null_ptr;
  }

  TEST_CASE("endl manipulator appends newline and flushes") {
    TerminalStream::get() << "before endl" << TerminalStream::endl;
  }

  TEST_CASE("flush_manip manipulator flushes without newline") {
    TerminalStream::get() << "before flush_manip" << TerminalStream::flush_manip;
  }

  TEST_CASE("std endl is accepted as manipulator alias") { TerminalStream::get() << "before std::endl" << std::endl; }

  TEST_CASE("write_raw with non-zero length writes without crash") {
    TerminalStream& ts = TerminalStream::get();
    const char data[] = "raw write test";
    ts.write_raw(data, sizeof(data) - 1);
  }

  TEST_CASE("write_raw with zero length is a no-op") {
    TerminalStream& ts = TerminalStream::get();
    const char data[] = "ignored";
    ts.write_raw(data, 0);
  }

  TEST_CASE("operator<< returns self for chaining") {
    TerminalStream& ts = TerminalStream::get();
    TerminalStream& ref = (ts << "a" << 1 << ' ' << 3.14f);
    CHECK_EQ(&ref, &ts);
  }

  TEST_CASE("VLINK_TERM_OUT macro expands to the singleton") {
    VLINK_TERM_OUT << "VLINK_TERM_OUT test" << TerminalStream::endl;
    CHECK_EQ(&VLINK_TERM_OUT, &TerminalStream::get());
  }

  TEST_CASE("integer boundary values write without crash or UB") {
    TerminalStream& ts = TerminalStream::get();
    ts << std::numeric_limits<int>::min() << TerminalStream::endl;
    ts << std::numeric_limits<int>::max() << TerminalStream::endl;
    ts << std::numeric_limits<long long>::min() << TerminalStream::endl;  // NOLINT(runtime/int,google-runtime-int)
    ts << std::numeric_limits<long long>::max() << TerminalStream::endl;  // NOLINT(runtime/int,google-runtime-int)
  }
}

#endif  // __unix__ && !__CYGWIN__

// NOLINTEND
