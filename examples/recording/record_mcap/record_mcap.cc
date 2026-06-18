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
#include <vlink/extension/bag_reader.h>
#include <vlink/extension/bag_writer.h>
#include <vlink/extension/vcap_reader.h>
#include <vlink/extension/vcap_writer.h>
#include <vlink/vlink.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// ---------------------------------------------------------------------------
// record_mcap.cc
//
// VLink has two on-disk container families:
//   * VDB  -- VLink-native database bag. Optimised for VLink serializer
//             types, very compact, SQLite-backed when enabled. Best for
//             internal pipelines and high-frequency capture.
//   * VCAP -- Foxglove MCAP-compatible container. Interoperable with the
//             Foxglove ecosystem (visualisation, conversion). Slightly
//             higher per-message overhead than VDB.
//
// File extension picks the writer:
//   .vdb / .vdbx  -> BagWriter (VDB; "x" suffix = split-capable).
//   .vcap / .vcapx -> VCAPWriter (MCAP container).
//
// BagWriter::create(path, cfg) inspects the extension and instantiates
// the right concrete writer; you can also instantiate VCAPWriter/VDBWriter
// directly when the format must be pinned.
//
// CompressType behaviour per backend:
//   kCompressNone -- raw payload bytes.
//   kCompressZstd -- best ratio; widely supported by VCAP readers.
//   kCompressLzav -- fast, VLink-internal; VDB only.
//   kCompressLz4  -- balanced; supported in both VDB and VCAP.
// ---------------------------------------------------------------------------

int main() {
  // ---- Section 1: MCAP via BagWriter::create() factory ----
  // Extension dispatch chooses VCAPWriter automatically.
  {
    VLOG_I("[1] MCAP via BagWriter::create()");

    auto writer = vlink::BagWriter::create("/tmp/mcap_auto.vcap");
    writer->async_run();

    for (int i = 0; i < 100; ++i) {
      const std::string payload = "mcap_message_" + std::to_string(i);
      vlink::Frame frame;
      frame.timestamp = -1;
      frame.url = "dds://mcap/topic_a";
      frame.ser_type = "raw";
      frame.schema_type = vlink::SchemaType::kRaw;
      frame.action_type = vlink::ActionType::kPublish;
      frame.data = vlink::Bytes::from_string(payload);
      writer->push(frame);
    }

    VLOG_I("  100 messages pushed to .vcap file");
  }

  // ---- Section 2: explicit VCAPWriter with Zstd compression ----
  // Pin the writer type so the format never gets reinterpreted from the
  // extension. Zstd level 5 is a sane default; raise to 19 for archival.
  {
    VLOG_I("[2] Explicit VCAPWriter with Zstd");

    vlink::BagWriter::Config config;
    config.tag_name = "mcap_explicit_test";
    config.compress = vlink::BagWriter::CompressType::kCompressZstd;
    config.compress_level = 5;

    auto writer = std::make_shared<vlink::VCAPWriter>("/tmp/mcap_explicit.vcap", config);
    writer->async_run();

    int64_t ts = 2000000LL;

    for (int i = 0; i < 50; ++i) {
      vlink::Frame frame_a;
      frame_a.timestamp = ts;
      frame_a.url = "dds://mcap/sensor_a";
      frame_a.ser_type = "raw";
      frame_a.schema_type = vlink::SchemaType::kRaw;
      frame_a.action_type = vlink::ActionType::kPublish;
      frame_a.data = vlink::Bytes::create(256);
      writer->push(frame_a);
      ts += 50000LL;
      vlink::Frame frame_b;
      frame_b.timestamp = ts;
      frame_b.url = "dds://mcap/sensor_b";
      frame_b.ser_type = "raw";
      frame_b.schema_type = vlink::SchemaType::kRaw;
      frame_b.action_type = vlink::ActionType::kPublish;
      frame_b.data = vlink::Bytes::create(128);
      writer->push(frame_b);
      ts += 50000LL;
    }

    VLOG_I("  explicit writer is_dumping=", writer->is_dumping());
  }

  // ---- Section 3: MCAP split mode (.vcapx) ----
  // The 'x' suffix on the extension enables auto-rotation. split_by_size
  // is the per-segment ceiling; split_name_by_time appends a timestamp.
  {
    VLOG_I("[3] MCAP split mode");

    vlink::BagWriter::Config config;
    config.split_by_size = 1024 * 50;
    config.split_name_by_time = true;

    auto writer = vlink::BagWriter::create("/tmp/mcap_split.vcapx", config);
    // Split callback runs on the writer's background thread per rotation.
    writer->register_split_callback([](int idx, const std::string& fn) { VLOG_I("  split idx=", idx, " file=", fn); },
                                    false);
    writer->async_run();

    for (int i = 0; i < 300; ++i) {
      vlink::Frame frame;
      frame.timestamp = -1;
      frame.url = "dds://mcap/split_topic";
      frame.ser_type = "raw";
      frame.schema_type = vlink::SchemaType::kRaw;
      frame.action_type = vlink::ActionType::kPublish;
      frame.data = vlink::Bytes::create(512);
      writer->push(frame);
    }

    VLOG_I("  split_mode=", writer->is_split_mode(), " split_index=", writer->get_split_index());
  }

  std::this_thread::sleep_for(500ms);

  // ---- Section 4: read MCAP via BagReader::create() factory ----
  // Same extension-dispatch story as the writer. detect_schema() returns
  // any embedded type schemas the MCAP carries (proto/CDR descriptors).
  {
    VLOG_I("[4] Read MCAP via BagReader::create()");

    auto reader = vlink::BagReader::create("/tmp/mcap_auto.vcap");
    const auto& info = reader->get_info();
    VLOG_I("  storage=", info.storage_type, " count=", info.message_count, " duration=", info.total_duration, " ms");

    for (const auto& m : info.url_metas) {
      VLOG_I("    url=", m.url, " count=", m.count, " ser=", m.ser_type);
    }

    auto schemas = reader->detect_schema();
    VLOG_I("  embedded schemas: ", schemas.size());

    std::atomic<int> msg_count{0};
    // Output callback fires on the reader's playback thread, ts-ordered.
    reader->register_output_callback([&msg_count](const vlink::Frame& frame) {
      ++msg_count;

      if (msg_count <= 3) {
        VLOG_I("  read ts=", frame.timestamp, " url=", frame.url, " size=", frame.data.size());
      }
    });
    // Finish callback fires once when the playback thread drains.
    reader->register_finish_callback([](bool interrupted) { VLOG_I("  finished, interrupted=", interrupted); });
    reader->async_run();

    vlink::BagReader::Config cfg;
    cfg.rate = 50.0;
    reader->play(cfg);

    std::this_thread::sleep_for(3s);
    VLOG_I("  total MCAP messages read: ", msg_count.load());
  }

  // ---- Section 5: explicit VCAPReader ----
  // Direct VCAPReader for tooling that needs MCAP-specific accessors
  // (get_ser_type returns the per-URL serializer label embedded in MCAP).
  {
    VLOG_I("[5] Explicit VCAPReader");

    auto reader = std::make_shared<vlink::VCAPReader>("/tmp/mcap_explicit.vcap");
    const auto& info = reader->get_info();
    VLOG_I("  tag=", info.tag_name, " compression=", info.compression_type, " messages=", info.message_count);
    VLOG_I("  ser_type[sensor_a]=", reader->get_ser_type("dds://mcap/sensor_a"));
  }

  VLOG_I("MCAP examples complete.");
  return 0;
}
