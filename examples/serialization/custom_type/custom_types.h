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

#pragma once

#include <vlink/vlink.h>

#include <cstring>
#include <string>
#include <type_traits>

// ---------------------------------------------------------------------------
// custom_types.h
//
// Two user-defined message types that opt into the kCustomType serialization
// strategy by providing matching `operator>>(Bytes&) const` and
// `operator<<(const Bytes&)` overloads. The Serializer trait probe detects
// these member operators with higher priority than kStandardType, so even a
// trivially-copyable type (Vec3) can override the default raw-memcpy path to
// produce a stable, layout-independent wire format -- which is essential for
// cross-platform / cross-compiler interop.
//
// Wire contract is fully user-defined: VLink does not inject any framing,
// length prefix, type tag, or endian conversion. The user owns the format.
// ---------------------------------------------------------------------------

// Fixed-length custom type. POD-like, but the operator>>/<< overloads make
// the Serializer dispatch land on kCustomType instead of kStandardType.
// Why bother? Two reasons:
//   1. Endian portability -- by writing each float explicitly we could byte-
//      swap on big-endian platforms if needed (this example assumes LE).
//   2. Stable ABI -- the wire size is exactly 12 bytes regardless of any
//      future padding/alignment changes the compiler may decide to add.
struct Vec3 {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;

  // Serialize: produce exactly 12 bytes in the order x|y|z. Allocates a
  // fresh Bytes buffer so the caller owns the output independently.
  void operator>>(vlink::Bytes& out) const {  // NOLINT(google-runtime-operator)
    out = vlink::Bytes::create(sizeof(float) * 3);
    std::memcpy(out.data(), &x, sizeof(float));
    std::memcpy(out.data() + sizeof(float), &y, sizeof(float));
    std::memcpy(out.data() + sizeof(float) * 2, &z, sizeof(float));
  }

  // Deserialize: silently ignores short buffers (defensive -- a malformed
  // sample on the wire should not crash the subscriber thread).
  void operator<<(const vlink::Bytes& in) {
    if (in.size() >= sizeof(float) * 3) {
      std::memcpy(&x, in.data(), sizeof(float));
      std::memcpy(&y, in.data() + sizeof(float), sizeof(float));
      std::memcpy(&z, in.data() + sizeof(float) * 2, sizeof(float));
    }
  }
};

// Compile-time guard: confirms that defining operator>>/<< really did flip
// Vec3 from kStandardType to kCustomType. If a future Serializer rewrite
// changes the trait priority, this fires before any wire breakage.
static_assert(vlink::Serializer::get_type_of<Vec3>() == vlink::Serializer::kCustomType);

// Variable-length custom type. Wire layout: [u32 name_len][name][i32 code][f64 value].
// Demonstrates the standard pattern for variable-sized payloads: write a
// length header, then the bytes, then any fixed-size trailer. The reader
// validates the buffer size against the declared name_len BEFORE indexing
// into it to avoid out-of-bounds reads on malformed input.
struct NamedValue {
  std::string name;
  int32_t code = 0;
  double value = 0.0;

  // Serialize: one allocation sized to the exact total -- avoids reallocs
  // and gives us a contiguous buffer the transport can ship as-is.
  void operator>>(vlink::Bytes& out) const {  // NOLINT(google-runtime-operator)
    uint32_t name_len = static_cast<uint32_t>(name.size());
    size_t total = sizeof(uint32_t) + name_len + sizeof(int32_t) + sizeof(double);
    out = vlink::Bytes::create(total);

    uint8_t* ptr = out.data();
    std::memcpy(ptr, &name_len, sizeof(uint32_t));
    ptr += sizeof(uint32_t);

    if (name_len > 0) {
      std::memcpy(ptr, name.data(), name_len);
      ptr += name_len;
    }

    std::memcpy(ptr, &code, sizeof(int32_t));
    ptr += sizeof(int32_t);
    std::memcpy(ptr, &value, sizeof(double));
  }

  // Deserialize: two-stage validation -- first ensure the length header
  // is readable, then ensure the full payload fits. Silent return on
  // truncation is the convention; logging would spam if the wire is noisy.
  void operator<<(const vlink::Bytes& in) {
    if (in.size() < sizeof(uint32_t)) {
      return;
    }

    const uint8_t* ptr = in.data();
    uint32_t name_len = 0;
    std::memcpy(&name_len, ptr, sizeof(uint32_t));
    ptr += sizeof(uint32_t);

    if (in.size() < sizeof(uint32_t) + name_len + sizeof(int32_t) + sizeof(double)) {
      return;
    }

    name.assign(reinterpret_cast<const char*>(ptr), name_len);
    ptr += name_len;
    std::memcpy(&code, ptr, sizeof(int32_t));
    ptr += sizeof(int32_t);
    std::memcpy(&value, ptr, sizeof(double));
  }
};

// NamedValue is non-trivial (has std::string), so kStandardType is
// automatically off the table; the operator>>/<< picks kCustomType.
static_assert(!std::is_trivial_v<NamedValue>);
static_assert(vlink::Serializer::get_type_of<NamedValue>() == vlink::Serializer::kCustomType);
