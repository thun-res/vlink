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

#include <vlink/base/logger.h>
#include <vlink/base/uuid.h>

#include <array>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

// -----------------------------------------------------------------------------
// Uuid basic example
//
// Module:   vlink/base/uuid.h
// Scenario: Tour vlink::Uuid -- a 128-bit identifier with RFC-4122 variant
//           and version semantics. Used throughout VLink to tag nodes,
//           messages and sessions. The example covers construction from
//           raw bytes / arrays / iterators, random v4 generation (with and
//           without a caller-supplied RNG for determinism), string parsing
//           (canonical / braced / compact), validity probes, variant/version
//           detection, hashing for use in unordered containers, raw byte
//           access, and the constexpr-friendly nil construction path.
// -----------------------------------------------------------------------------
int main() {
  VLOG_I("=== Uuid Basic Example ===");

  // Default construction yields the nil UUID (all-zero bytes). is_nil is the
  // canonical "empty/sentinel" check.
  {
    vlink::Uuid id;
    VLOG_I("default: is_nil=", id.is_nil(), " to_string=", id.to_string());
  }

  // Construct from std::array<uint8_t,16>: the array is taken verbatim, no
  // variant/version bits are rewritten -- callers control the layout.
  {
    std::array<uint8_t, 16> data{0x47, 0xac, 0x10, 0xb8, 0x58, 0xcc, 0x4a, 0x3c,
                                 0x8c, 0x5b, 0x0e, 0x77, 0x88, 0x99, 0xaa, 0xbb};
    vlink::Uuid id{data};
    VLOG_I("from array: canonical=", id.to_string(), " compact=", id.to_compact_string());
  }

  // Raw C-array and iterator-range constructors. Iterator range MUST yield
  // exactly 16 bytes; anything else produces a nil UUID (validated below).
  {
    const uint8_t raw[16] = {0xfe, 0xed, 0xfa, 0xce, 0xde, 0xad, 0xbe, 0xef,
                             0xca, 0xfe, 0xba, 0xbe, 0x12, 0x34, 0x56, 0x78};
    vlink::Uuid id{raw};
    VLOG_I("from raw[16]: ", id.to_string());

    std::vector<uint8_t> source(16U, 0xa5U);
    vlink::Uuid id_v{source.begin(), source.end()};
    VLOG_I("from iter range: ", id_v.to_string());

    std::vector<uint8_t> too_short(8U, 0xffU);
    vlink::Uuid bad{too_short.begin(), too_short.end()};
    VLOG_I("from 8-byte iter: is_nil=", bad.is_nil(), " (must be 16 bytes)");
  }

  // generate_random: standard v4 / random-based UUID. Variant must be 1
  // (RFC-4122) and version must be 4 (random-based) per RFC.
  {
    vlink::Uuid id = vlink::Uuid::generate_random();
    VLOG_I("random: ", id.to_string(), " variant=", static_cast<int>(id.variant()),
           " version=", static_cast<int>(id.version()), " (expect 1=Rfc, 4=RandomBased)");
  }

  // Caller-supplied engine: seed two engines identically -> get identical
  // UUIDs (useful for reproducible tests). Advancing the engine yields a
  // different value, as expected from a deterministic PRNG.
  {
    // NOLINTNEXTLINE(bugprone-random-generator-seed)
    std::mt19937 e1(0xdeadbeefU);

    // NOLINTNEXTLINE(bugprone-random-generator-seed)
    std::mt19937 e2(0xdeadbeefU);
    vlink::Uuid a = vlink::Uuid::generate_random(e1);
    vlink::Uuid b = vlink::Uuid::generate_random(e2);
    vlink::Uuid c = vlink::Uuid::generate_random(e1);

    VLOG_I("seeded same engine: a==b=", a == b, " c!=a after engine advanced=", c != a);
  }

  // from_string accepts canonical (with hyphens), braced ({...}) and compact
  // (no hyphens) forms. Returns std::optional -- empty on parse failure.
  {
    const std::string text = "47ac10b8-58cc-4a3c-8c5b-0e778899aabb";
    auto parsed = vlink::Uuid::from_string(text);

    if (parsed.has_value()) {
      VLOG_I("canonical parsed: ", parsed->to_string(), " round-trip=", (parsed->to_string() == text));
    }

    auto braced = vlink::Uuid::from_string("{47ac10b8-58cc-4a3c-8c5b-0e778899aabb}");
    auto compact = vlink::Uuid::from_string("47ac10b858cc4a3c8c5b0e778899aabb");
    auto bad = vlink::Uuid::from_string("not-a-uuid");
    VLOG_I("braced parsed=", braced.has_value(), " compact parsed=", compact.has_value(),
           " bad parsed=", bad.has_value());
  }

  // is_valid: cheap predicate that mirrors from_string -- no allocation, no
  // optional unwrap; preferred when only validation is needed.
  {
    VLOG_I("is_valid canonical = ", vlink::Uuid::is_valid("47ac10b8-58cc-4a3c-8c5b-0e778899aabb"));
    VLOG_I("is_valid braced    = ", vlink::Uuid::is_valid("{47ac10b8-58cc-4a3c-8c5b-0e778899aabb}"));
    VLOG_I("is_valid compact   = ", vlink::Uuid::is_valid("47ac10b858cc4a3c8c5b0e778899aabb"));
    VLOG_I("is_valid empty     = ", vlink::Uuid::is_valid(""));
    VLOG_I("is_valid nullptr   = ", vlink::Uuid::is_valid(static_cast<const char*>(nullptr)));
  }

  // Manual bit-twiddling demo: octet 8 carries the variant, octet 6 carries
  // the version. Used by callers crafting deterministic UUIDs by hand.
  {
    std::array<uint8_t, 16> data{};
    data[8] = 0x80U;
    VLOG_I("octet8=0x80 variant=", static_cast<int>(vlink::Uuid{data}.variant()), " (RFC)");
    data[6] = 0x40U;
    VLOG_I("octet6=0x40 version=", static_cast<int>(vlink::Uuid{data}.version()), " (RandomBased)");
  }

  // Raw bytes accessor + compile-time constants exposed for FFI callers.
  {
    vlink::Uuid id = vlink::Uuid::generate_random();
    const auto& raw = id.bytes();
    VLOG_I("bytes[0]=", static_cast<int>(raw[0]), " bytes[6]=", static_cast<int>(raw[6]),
           " bytes[8]=", static_cast<int>(raw[8]));
    VLOG_I("kByteSize=", static_cast<size_t>(vlink::Uuid::kByteSize),
           " kStringSize=", static_cast<size_t>(vlink::Uuid::kStringSize));
  }

  // Total order via operator< and hash() let Uuid live in std::map and
  // std::unordered_set without any extra boilerplate.
  {
    vlink::Uuid nil_a;
    vlink::Uuid nil_b;
    std::array<uint8_t, 16> data{};
    data[0] = 1U;
    vlink::Uuid other{data};
    VLOG_I("nil_a == nil_b: ", nil_a == nil_b, " nil_a < other: ", nil_a < other);

    std::unordered_set<vlink::Uuid> set;
    for (int i = 0; i < 10; ++i) {
      set.insert(vlink::Uuid::generate_random());
    }

    VLOG_I("10 random UUIDs in unordered_set: size=", set.size());
  }

  // Companion utilities for non-UUID random data: arbitrary-length byte
  // buffers and hex strings (used by salt/nonce generation in security code).
  {
    auto buf = vlink::Uuid::random_bytes(8U);
    VLOG_I("random_bytes(8).size = ", buf.size());

    auto hex16 = vlink::Uuid::random_hex(16U);
    auto hex_default = vlink::Uuid::random_hex();
    VLOG_I("random_hex(16).size = ", hex16.size(), " random_hex() default size = ", hex_default.size());

    VLOG_I("random_bytes(0) empty = ", vlink::Uuid::random_bytes(0U).empty(),
           " random_hex(0) empty = ", vlink::Uuid::random_hex(0U).empty());
  }

  // swap is constant-time (memcpy of 16 bytes) -- handy for std::sort etc.
  {
    std::array<uint8_t, 16> data{};
    data[0] = 0x77U;
    vlink::Uuid a{data};
    vlink::Uuid b;
    a.swap(b);
    VLOG_I("after swap: a.is_nil=", a.is_nil(), " b[0]=", static_cast<int>(b.bytes()[0]));
  }

  // constexpr: the default ctor and the array-based ctor are constant
  // expressions, allowing UUID literals in static_assert / constexpr context.
  {
    constexpr vlink::Uuid kNilId;
    static_assert(kNilId.is_nil(), "default Uuid must be nil");
    constexpr std::array<uint8_t, 16> kSample{
        0x47, 0xac, 0x10, 0xb8, 0x58, 0xcc, 0x4a, 0x3c, 0x8c, 0x5b, 0x0e, 0x77, 0x88, 0x99, 0xaa, 0xbb,
    };
    constexpr vlink::Uuid kSampleId{kSample};
    static_assert(kSampleId.bytes()[0] == 0x47U, "constexpr bytes() must work");
    VLOG_I("constexpr static_asserts passed");
  }

  VLOG_I("=== Uuid Basic Example Complete ===");
  return 0;
}
