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
#include <vlink/extension/bag_writer.h>
#include <vlink/vlink.h>

#include <chrono>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// ---------------------------------------------------------------------------
// record_compression.cc
//
// Side-by-side micro-benchmark of the four CompressType options. Writes
// the same payload into four bag files and reports wall-clock throughput
// per algorithm. Use as a starting point when tuning a deployment -- the
// best choice depends on payload entropy, CPU budget, and storage cost.
//
// Compress fields (BagWriter::Config):
//   compress             -- algorithm enum.
//   compress_level       -- algo-specific quality (Zstd 1-22, LZ4 1-12,
//                           LZAV 1-9). Higher = smaller + slower.
//   compress_start_size  -- min payload bytes to attempt compression.
//                           Tiny messages don't compress well; skipping
//                           them saves CPU. Default 64.
//
// Per-backend actual behaviour:
//   * VDB (.vdb)  -- compression is applied per payload before SQLite/
//                    raw write; reads transparently decompress.
//   * VCAP (.vcap) -- block-level compression per chunk (MCAP spec);
//                     reader handles transparently. LZAV is not part of
//                     the MCAP spec and falls back to None there.
// ---------------------------------------------------------------------------

// Run a single benchmark configuration end-to-end: build the writer,
// generate a deterministic payload, push N copies, measure wall time.
// The 200ms post-publish sleep gives the writer thread time to flush.
static void benchmark_compression(const std::string& label, const std::string& path,
                                  vlink::BagWriter::CompressType compress_type, int compress_level, int message_count,
                                  size_t message_size) {
  vlink::BagWriter::Config config;
  config.compress = compress_type;
  config.compress_level = compress_level;
  config.compress_start_size = 64;

  auto writer = vlink::BagWriter::create(path, config);
  writer->async_run();

  // Pseudo-random but reproducible pattern. Real telemetry usually has
  // higher redundancy and yields better compression ratios than this.
  vlink::Bytes data = vlink::Bytes::create(message_size);
  for (size_t i = 0; i < message_size; ++i) {
    data[i] = static_cast<uint8_t>((i * 7 + 13) % 256);
  }

  const auto start = std::chrono::steady_clock::now();

  for (int i = 0; i < message_count; ++i) {
    vlink::Frame frame;
    frame.timestamp = -1;
    frame.url = "dds://compression/bench";
    frame.ser_type = "raw";
    frame.schema_type = vlink::SchemaType::kRaw;
    frame.action_type = vlink::ActionType::kPublish;
    frame.data = vlink::Bytes::shallow_copy(data.data(), data.size());
    writer->push(frame);
  }

  std::this_thread::sleep_for(200ms);
  const auto end = std::chrono::steady_clock::now();
  const double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
  const double throughput_mb = (static_cast<double>(message_count) * message_size) / (1024.0 * 1024.0);

  VLOG_I("  ", label, ": ", message_count, " x ", message_size, " bytes; time=", elapsed_ms,
         " ms; throughput=", throughput_mb / (elapsed_ms / 1000.0), " MB/s; file=", path);
}
int main() {
  static constexpr int kMessageCount = 500;
  static constexpr size_t kMessageSize = 4096;

  VLOG_I("[1] No Compression");
  benchmark_compression("None", "/tmp/record_compress_none.vdb", vlink::BagWriter::CompressType::kCompressNone, 0,
                        kMessageCount, kMessageSize);

  VLOG_I("[2] Zstd Compression");
  benchmark_compression("Zstd", "/tmp/record_compress_zstd.vdb", vlink::BagWriter::CompressType::kCompressZstd, 3,
                        kMessageCount, kMessageSize);

  VLOG_I("[3] LZ4 Compression");
  benchmark_compression("LZ4", "/tmp/record_compress_lz4.vdb", vlink::BagWriter::CompressType::kCompressLz4, 1,
                        kMessageCount, kMessageSize);

  VLOG_I("[4] LZAV Compression");
  benchmark_compression("LZAV", "/tmp/record_compress_lzav.vdb", vlink::BagWriter::CompressType::kCompressLzav, 3,
                        kMessageCount, kMessageSize);

  // Section 5: configuration reference
  {
    VLOG_I("[5] CompressType enum and Config fields");
    VLOG_I("  kCompressNone=0  kCompressAuto=1  kCompressZstd=2  kCompressLz4=3  kCompressLzav=4");
    VLOG_I("  Config: compress / compress_level / compress_start_size");
  }

  // Section 6: selection guide
  {
    VLOG_I("[6] Selection guide");
    VLOG_I("  None  -- real-time, low CPU");
    VLOG_I("  LZAV  -- default choice (built-in, no external dep)");
    VLOG_I("  LZ4   -- highest throughput, lower ratio (needs liblz4)");
    VLOG_I("  Zstd  -- best ratio (needs libzstd)");
    VLOG_I("  Auto  -- let VLink choose");
  }

  VLOG_I("Compression comparison complete. See /tmp/record_compress_*.vdb");
  return 0;
}
