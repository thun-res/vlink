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

#include <iostream>
#include <string>

// -----------------------------------------------------------------------------
// Bytes advanced example
//
// Module:   vlink/base/bytes.h
// Scenario: Exercise the post-construction utilities that ship with the Bytes
//           container -- LZAV compression (used by bag recording / network
//           transports when ENABLE_ZSTD is off), Base64 textual encoding,
//           CRC-32 integrity hashing, hex<->bytes parsing for CLI/config
//           round-trips, in-place mutation primitives, and explicit memory
//           pool initialisation (which makes subsequent Bytes::create calls
//           draw from the pooled tiers configured by VLINK_MEMORY_LEVEL).
// -----------------------------------------------------------------------------
int main() {
  VLOG_I("=== Bytes Advanced Example ===");

  // Compression: LZAV is a fast, dependency-free compressor bundled with
  // VLink. The high_ratio flag trades CPU for a smaller payload; both modes
  // round-trip losslessly through uncompress_data().
  {
    std::string original(500, 'A');
    for (size_t i = 0; i < original.size(); ++i) {
      original[i] = static_cast<char>('A' + (i % 26));
    }

    auto src = vlink::Bytes::from_string(original);
    auto compressed = vlink::Bytes::compress_data(src.data(), src.size());
    auto hi_compressed = vlink::Bytes::compress_data(src.data(), src.size(), /*high_ratio=*/true);
    auto decompressed = vlink::Bytes::uncompress_data(compressed.data(), compressed.size());

    MLOG_I("compress: orig={} std={} hi={} ratio={:.3f}", src.size(), compressed.size(), hi_compressed.size(),
           static_cast<double>(compressed.size()) / static_cast<double>(src.size()));
    // is_compress_data inspects a magic prefix, so receivers can autodetect
    // whether decompression is required without out-of-band metadata.
    VLOG_I("is_compress_data=", vlink::Bytes::is_compress_data(compressed.data(), compressed.size()),
           " round-trip ok=", (decompressed.to_string() == original));
  }

  // Base64: textual encoding for places that cannot carry raw bytes (JSON,
  // YAML, query strings). decode_from_base64 returns an empty Bytes on
  // malformed input.
  {
    auto src = vlink::Bytes::from_string("Hello, VLink Base64!");
    std::string encoded = vlink::Bytes::encode_to_base64(src);
    auto decoded = vlink::Bytes::decode_from_base64(encoded);
    VLOG_I("base64 enc=", encoded);
    VLOG_I("base64 dec=\"", decoded.to_string(), "\" round-trip=", (decoded == src));
  }

  // CRC-32: cheap integrity check used by some transports for frame
  // validation. Identical inputs must produce identical hashes; a single
  // appended byte changes the hash deterministically.
  {
    auto data = vlink::Bytes::from_string("VLink CRC test");
    uint32_t crc = vlink::Bytes::get_crc_32(data);
    MLOG_I("crc32=0x{:08X}", crc);

    auto same = vlink::Bytes::from_string("VLink CRC test");
    auto diff = vlink::Bytes::from_string("VLink CRC test!");
    VLOG_I("crc consistency=", (vlink::Bytes::get_crc_32(same) == crc),
           " differs=", (vlink::Bytes::get_crc_32(diff) != crc));
  }

  // Hex dump: pure utility used by the bag/proxy tooling for human-readable
  // payload display. No leading "0x" prefix.
  {
    vlink::Bytes bytes{0xDE, 0xAD, 0xBE, 0xEF};
    VLOG_I("hex=", vlink::Bytes::convert_to_hex_str(bytes.data(), bytes.size()));
  }

  // from_user_input parses a CLI string in either ASCII or "0x..." hex form.
  // The bool out-param reports parse success; on failure the returned Bytes
  // is empty -- callers must check `ok` before consuming the bytes.
  {
    bool ok = false;
    auto hex_bytes = vlink::Bytes::from_user_input("0x48656C6C6F", &ok);
    VLOG_I("hex-input ok=", ok, " parsed=\"", hex_bytes.to_string(), "\"");

    auto bad = vlink::Bytes::from_user_input("not_hex", &ok);
    VLOG_I("bad-input ok=", ok, " empty=", bad.empty());
  }

  // reverse_order returns a fresh Bytes with byte order flipped -- useful for
  // little-endian <-> big-endian conversions on the wire.
  {
    vlink::Bytes original{0x01, 0x02, 0x03, 0x04};
    auto reversed = vlink::Bytes::reverse_order(original);
    VLOG_I("orig=", vlink::Bytes::convert_to_hex_str(original.data(), original.size()),
           " reversed=", vlink::Bytes::convert_to_hex_str(reversed.data(), reversed.size()));
  }

  // Sizing helpers: same semantics as bytes_basic. shrink_to may also grow
  // the logical size up to capacity -- check the return flag, not the input.
  {
    auto buf = vlink::Bytes::create(20);
    VLOG_I("init size=", buf.size(), " cap=", buf.capacity());

    bool ok = buf.reserve(500);
    VLOG_I("reserve(500) ok=", ok, " cap=", buf.capacity());

    ok = buf.resize(300);
    VLOG_I("resize(300) ok=", ok, " size=", buf.size());

    ok = buf.shrink_to(100);
    VLOG_I("shrink_to(100) ok=", ok, " size=", buf.size());

    ok = buf.shrink_to(200);
    VLOG_I("shrink_to(200) [grow] ok=", ok);
  }

  // init_memory_pool wires Bytes to the process-wide MemoryPool. Subsequent
  // allocations larger than the SBO threshold are served from pooled tiers
  // (size classes governed by VLINK_MEMORY_LEVEL), eliminating malloc churn
  // in hot publish/subscribe loops.
  {
    vlink::Bytes::init_memory_pool();
    auto pooled = vlink::Bytes::create(200);
    VLOG_I("pooled alloc size=", pooled.size(), " (VLINK_MEMORY_LEVEL governs tiers)");
  }

  // operator<<: convenience hex dump straight to std::ostream.
  {
    vlink::Bytes bytes{0xAA, 0xBB, 0xCC};
    std::cout << "operator<< : " << bytes << std::endl;
  }

  VLOG_I("=== Bytes Advanced Example Complete ===");
  return 0;
}
