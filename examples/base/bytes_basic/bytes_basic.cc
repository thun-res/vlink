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

#include <vlink/base/bytes.h>
#include <vlink/base/logger.h>

#include <cstring>
#include <string>
#include <vector>

// Tiny helper that prints the public ownership/state observers of a Bytes
// instance. The Bytes container hides three internal storage strategies
// (SBO, heap, external loan) behind one interface; these accessors are how
// callers reason about ownership without inspecting the raw pointer.
static void print_info(const char* label, const vlink::Bytes& b) {
  VLOG_I(label, ": size=", b.size(), " capacity=", b.capacity(), " offset=", static_cast<int>(b.offset()),
         " is_owner=", b.is_owner(), " empty=", b.empty());
}
// -----------------------------------------------------------------------------
// Bytes basic example
//
// Module:   vlink/base/bytes.h
// Scenario: First-look tour of the Bytes container that powers VLink's
//           zero-copy message payloads. Demonstrates the construction paths
//           (SBO vs heap), header reservation via the offset slot, factories
//           from std::string / std::vector / initializer_list, range-for
//           iteration, mutation (resize/reserve/shrink_to) and endianness
//           queries. Pure single-thread usage -- no transport involved.
// -----------------------------------------------------------------------------
int main() {
  VLOG_I("=== Bytes Basic Example ===");

  // SBO path: payloads <= Bytes::stack_size() (96B) live inline inside the
  // Bytes object itself -- zero heap allocation, ideal for tiny messages.
  {
    auto sbo = vlink::Bytes::create(64);
    print_info("SBO(64)", sbo);

    // Write a deterministic ramp so we can later verify byte-wise access via
    // operator[] returned a writable reference (Bytes is mutable when owned).
    for (size_t i = 0; i < sbo.size(); ++i) {
      sbo[i] = static_cast<uint8_t>(i & 0xFF);
    }

    VLOG_I("stack_size=", static_cast<int>(vlink::Bytes::stack_size()), " first=", static_cast<int>(sbo[0]),
           " last=", static_cast<int>(sbo[63]));
  }

  // Heap path: anything above the SBO threshold spills onto the memory pool
  // (or system allocator if the pool is not initialised). API is identical.
  {
    auto heap = vlink::Bytes::create(256);
    print_info("Heap(256)", heap);
    std::memset(heap.data(), 0xAB, heap.size());
    VLOG_I("first byte: 0x", std::hex, static_cast<int>(heap[0]));
  }

  // Offset slot: reserves a prefix region in front of the logical payload so
  // transport layers (DDS / SOME-IP / shm) can prepend their own headers in
  // place without an extra copy. data() points past the reserved prefix;
  // real_data() returns the true allocation base.
  {
    auto buf = vlink::Bytes::create(100, /*offset=*/8);
    print_info("create(100, offset=8)", buf);
    VLOG_I("real_size=", buf.real_size(),
           " data() == real_data()+offset: ", (buf.real_data() + buf.offset() == buf.data()));
  }

  // String factory: deep-copies the std::string contents into a fresh owning
  // Bytes (so the source string can be freed independently afterwards).
  {
    auto bytes = vlink::Bytes::from_string("Hello, VLink Bytes!");
    print_info("from_string", bytes);
    VLOG_I("to_string=\"", bytes.to_string(), "\"");
  }

  // Container ctors: vector and initializer_list both produce owning copies.
  // operator== compares byte-wise content regardless of ownership flavour.
  {
    std::vector<uint8_t> vec = {0x01, 0x02, 0x03, 0x04, 0x05};
    vlink::Bytes bytes(vec);
    VLOG_I("from vector size=", bytes.size(), " bytes == vec: ", (bytes == vec));

    vlink::Bytes magic{0xCA, 0xFE, 0xBA, 0xBE};
    VLOG_I("initializer_list[0]=0x", std::hex, static_cast<int>(magic[0]), " [3]=0x", static_cast<int>(magic[3]));
  }

  // Range-for: Bytes exposes contiguous iterators (uint8_t*) so it slots into
  // the standard library algorithms and range-based for transparently.
  {
    vlink::Bytes bytes{10, 20, 30, 40, 50};
    std::string values;
    for (uint8_t b : bytes) {
      if (!values.empty()) {
        values += ", ";
      }

      values += std::to_string(b);
    }

    VLOG_I("range-for: [", values, "]");
  }

  // Lifecycle queries: default-constructed Bytes is empty (no buffer at all),
  // clear() releases the buffer and returns the object to the empty state.
  {
    vlink::Bytes bytes;
    VLOG_I("default-constructed empty=", bytes.empty());

    bytes = vlink::Bytes::create(32);
    VLOG_I("after create(32) empty=", bytes.empty(), " size=", bytes.size());

    bytes.clear();
    VLOG_I("after clear empty=", bytes.empty(), " size=", bytes.size());
  }

  // Byte-wise equality is content-based -- two independently constructed
  // buffers with identical bytes compare equal.
  {
    auto a = vlink::Bytes::from_string("test");
    auto b = vlink::Bytes::from_string("test");
    auto c = vlink::Bytes::from_string("other");
    VLOG_I("a == b: ", (a == b), " a != c: ", (a != c));
  }

  // Sizing helpers: reserve grows capacity, resize grows the logical size,
  // shrink_to trims the logical size; all three return false when the
  // underlying storage cannot satisfy the request (e.g. non-owning alias).
  {
    auto buf = vlink::Bytes::create(10);
    VLOG_I("initial: size=", buf.size(), " cap=", buf.capacity());

    if (buf.reserve(200)) {
      VLOG_I("after reserve(200): size=", buf.size(), " cap=", buf.capacity());
    }

    if (buf.resize(150)) {
      VLOG_I("after resize(150): size=", buf.size());
    }

    if (buf.shrink_to(50)) {
      VLOG_I("after shrink_to(50): size=", buf.size());
    }
  }

  // Endianness probes: compile-time detected, used by serializers to decide
  // whether to byte-swap multi-byte fields on the wire.
  VLOG_I("is_little_endian=", vlink::Bytes::is_little_endian(), " is_big_endian=", vlink::Bytes::is_big_endian());

  VLOG_I("=== Bytes Basic Example Complete ===");
  return 0;
}
