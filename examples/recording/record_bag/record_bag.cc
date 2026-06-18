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
#include <vlink/extension/bag_processor.h>
#include <vlink/extension/bag_reader.h>
#include <vlink/extension/bag_writer.h>
#include <vlink/vlink.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// ---------------------------------------------------------------------------
// record_bag.cc
//
// Direct BagWriter + BagReader use (no Publisher/Subscriber involvement).
// This is the level recorders, replayers, and bag tools use.
//
// BagWriter::push() signature:
//   push(const vlink::Frame& frame, bool immediate = false)
//      frame.timestamp    -- caller-supplied timestamp (us); < 0 => BagWriter
//                            auto-assigns from its monotonic clock. ALL pushes
//                            per bag MUST be monotonically increasing --
//                            out-of-order timestamps are silently dropped.
//      frame.url          -- topic URL, used as key + part of the bag index.
//      frame.ser_type     -- human label for the wire type ("raw"/"proto:"/...).
//      frame.schema_type  -- SchemaType enum tag (kRaw / kProtobuf / ...).
//      frame.action_type  -- ActionType (kPublish / kSubscribe / ...).
//      frame.data         -- Bytes (after Serializer, before transport encryption).
//      immediate          -- true => synchronous write, bypassing the queue.
//
// BagReader::Config (playback):
//   rate         -- replay speed multiplier (1.0 = real time).
//   times        -- how many times to loop (1 = play once, 0 = forever).
//   begin_time   -- start offset (ms from bag start).
//   end_time     -- end offset (ms from bag start; 0 = to end).
//   filter_urls  -- whitelist; empty = all URLs.
//   skip_blank   -- collapse long quiet gaps to keep replay tight.
//
// BagProcessor merges N BagReader streams in timestamp order with
// a small look-ahead cache (min_cache_time ms) so callers see a globally
// ordered output even when the inputs interleave.
//
// File formats: ".vdb" = VLink-native (VCAP container), ".mcap" = Foxglove.
// ---------------------------------------------------------------------------

int main() {
  static const std::string kBagPath = "/tmp/record_bag.vdb";

  // ---- Section 1: BagWriter -- basic push() loop ----
  // Writes 50 messages into two URLs. Timestamps are caller-supplied and
  // step by 100us to demonstrate the monotonic-ts contract.
  {
    VLOG_I("[1] BagWriter::create() + push()");

    auto writer = vlink::BagWriter::create(kBagPath);
    writer->async_run();

    int64_t ts = 1000000LL;

    for (int i = 0; i < 50; ++i) {
      const std::string payload = "msg_" + std::to_string(i);
      vlink::Frame frame;
      frame.timestamp = ts;
      frame.url = "dds://test/topic_a";
      frame.ser_type = "raw";
      frame.schema_type = vlink::SchemaType::kRaw;
      frame.action_type = vlink::ActionType::kPublish;
      frame.data = vlink::Bytes::from_string(payload);
      writer->push(frame);
      ts += 100000LL;

      if (i % 2 == 0) {
        const std::string payload_b = "data_" + std::to_string(i);
        vlink::Frame frame_b;
        frame_b.timestamp = ts;
        frame_b.url = "dds://test/topic_b";
        frame_b.ser_type = "text";
        frame_b.schema_type = vlink::SchemaType::kRaw;
        frame_b.action_type = vlink::ActionType::kPublish;
        frame_b.data = vlink::Bytes::from_string(payload_b);
        writer->push(frame_b);
      }
    }

    vlink::Frame stream_frame;
    stream_frame.timestamp = ts;
    stream_frame.url = "dds://test/topic_a";
    stream_frame.ser_type = "raw";
    stream_frame.schema_type = vlink::SchemaType::kRaw;
    stream_frame.action_type = vlink::ActionType::kPublish;
    stream_frame.data = vlink::Bytes::from_string("stream_msg");

    *writer << stream_frame;

    if (!*writer) {
      VLOG_W("  streaming write reported a failure");
    }

    std::this_thread::sleep_for(300ms);
    VLOG_I("  76 messages written to ", kBagPath, " (last via operator<<)");
  }

  // ---- Section 2: BagWriter::Config with compression, split, WAL ----
  // compress              -- algorithm (kCompressNone/Zstd/Lzav/Lz4).
  // compress_level        -- algo-specific quality vs CPU trade-off.
  // compress_start_size   -- min payload bytes before compression triggers
  //                          (avoid CPU cost on tiny messages).
  // split_by_size         -- rotate file when current segment exceeds N B.
  // split_name_by_time    -- append timestamp to each segment's filename.
  // wal_mode              -- SQLite write-ahead log; durability tradeoff.
  // tag_name              -- arbitrary tag stored in the bag header.
  {
    VLOG_I("[2] BagWriter::Config: compression + split + WAL");

    vlink::BagWriter::Config config;
    config.compress = vlink::BagWriter::CompressType::kCompressLzav;
    config.compress_level = 3;
    config.compress_start_size = 64;
    config.split_by_size = 1024 * 100;
    config.split_name_by_time = true;
    config.wal_mode = true;
    config.tag_name = "config_demo";

    auto writer = vlink::BagWriter::create("/tmp/record_bag_config.vdb", config);
    // Split callback fires on the writer's background thread each time a
    // new segment is opened. Second arg = "include initial file?" (false).
    writer->register_split_callback([](int idx, const std::string& fn) { VLOG_I("  split idx=", idx, " file=", fn); },
                                    false);
    writer->async_run();

    for (int i = 0; i < 200; ++i) {
      vlink::Frame frame;
      frame.timestamp = -1;
      frame.url = "dds://test/big";
      frame.ser_type = "raw";
      frame.schema_type = vlink::SchemaType::kRaw;
      frame.action_type = vlink::ActionType::kPublish;
      frame.data = vlink::Bytes::create(512);
      writer->push(frame);
    }

    std::this_thread::sleep_for(200ms);
    VLOG_I("  split_mode=", writer->is_split_mode(), " split_index=", writer->get_split_index());
  }

  // ---- Section 3: filter_get -- shared writer access ----
  // filter_get(path) returns a process-wide singleton per path so multiple
  // unrelated nodes (or library code) can append to the same bag without
  // coordinating their lifetime. Internally synchronised; thread-safe.
  {
    VLOG_I("[3] BagWriter::filter_get() -- shared instance");

    auto w1 = vlink::BagWriter::filter_get("/tmp/record_bag_shared.vdb");
    auto w2 = vlink::BagWriter::filter_get("/tmp/record_bag_shared.vdb");

    VLOG_I("  same instance = ", (w1.get() == w2.get()));
    vlink::Frame frame1;
    frame1.timestamp = -1;
    frame1.url = "dds://test/shared";
    frame1.ser_type = "raw";
    frame1.schema_type = vlink::SchemaType::kRaw;
    frame1.action_type = vlink::ActionType::kPublish;
    frame1.data = vlink::Bytes::from_string("from_w1");
    w1->push(frame1);
    vlink::Frame frame2;
    frame2.timestamp = -1;
    frame2.url = "dds://test/shared";
    frame2.ser_type = "raw";
    frame2.schema_type = vlink::SchemaType::kRaw;
    frame2.action_type = vlink::ActionType::kPublish;
    frame2.data = vlink::Bytes::from_string("from_w2");
    w2->push(frame2);
  }

  // ---- Section 4: BagReader -- inspect bag metadata ----
  // The `true` flag = "load full Info eagerly" (scans the bag once at
  // create-time to populate per-URL stats: count, frequency, duration).
  {
    VLOG_I("[4] BagReader::get_info() -- inspect bag");

    auto reader = vlink::BagReader::create(kBagPath, true);
    const vlink::BagReader::Info& info = reader->get_info();
    VLOG_I("  file=", info.file_name, " version=", info.version, " storage=", info.storage_type);
    VLOG_I("  duration=", info.total_duration, " ms  count=", info.message_count, " size=", info.file_size);

    for (const auto& u : info.url_metas) {
      VLOG_I("    url=", u.url, " count=", u.count, " ser=", u.ser_type, " freq=", u.freq, " Hz");
    }
  }

  // ---- Section 5: BagReader -- basic playback ----
  // Output callback fires on the reader's playback thread, one call per
  // sample, in timestamp order. cfg.rate=20.0 plays back 20x real time.
  {
    VLOG_I("[5] BagReader -- basic playback");

    auto reader = vlink::BagReader::create(kBagPath);
    std::atomic<int> msg_count{0};

    reader->register_output_callback([&msg_count](const vlink::Frame& frame) {
      ++msg_count;

      if (msg_count <= 3) {
        VLOG_I("  ts=", frame.timestamp, " url=", frame.url, " size=", frame.data.size());
      }
    });

    // Finish callback fires once playback drains (interrupted=true if
    // stop() was called early).
    reader->register_finish_callback(
        [](bool interrupted) { VLOG_I("  playback finished, interrupted=", interrupted); });
    reader->async_run();

    vlink::BagReader::Config cfg;
    cfg.rate = 20.0;
    reader->play(cfg);

    std::this_thread::sleep_for(3s);
    VLOG_I("  total played: ", msg_count.load());
  }

  // ---- Section 6: BagReader -- time range + URL filtering ----
  // begin_time/end_time crop the playback window (ms from bag start).
  // filter_urls is a whitelist; only messages on those URLs are emitted.
  // skip_blank elides quiet stretches so the playback feels continuous.
  {
    VLOG_I("[6] BagReader -- time + URL filtering");

    auto reader = vlink::BagReader::create(kBagPath);
    std::atomic<int> msg_count{0};

    reader->register_output_callback([&msg_count](const vlink::Frame&) { ++msg_count; });
    reader->async_run();

    vlink::BagReader::Config cfg;
    cfg.rate = 20.0;
    cfg.begin_time = 2000;
    cfg.end_time = 4000;
    cfg.filter_urls.insert("dds://test/topic_b");
    cfg.skip_blank = true;
    reader->play(cfg);

    std::this_thread::sleep_for(2s);
    VLOG_I("  filtered messages: ", msg_count.load());
  }

  // ---- Section 7: BagReader -- synchronous cursor read (operator>>) ----
  {
    VLOG_I("[7] BagReader -- synchronous cursor (operator>>)");

    auto reader = vlink::BagReader::create(kBagPath);
    reader->open_cursor();

    int count = 0;
    vlink::Frame frame;

    while (*reader >> frame) {
      if (count < 3) {
        VLOG_I("  ts=", frame.timestamp, " url=", frame.url, " size=", frame.data.size());
      }

      ++count;
    }

    VLOG_I("  cursor read ", count, " frames, eof=", reader->eof(), " fail=", reader->fail());
  }

  // ---- Section 8: BagProcessor -- time-ordered merge ----
  // When merging multiple bags (or a single bag whose readers may emit
  // out-of-order due to async I/O), the processor buffers samples for
  // min_cache_time ms and releases them globally sorted by timestamp.
  // Trade-off: larger cache => stricter ordering but higher latency.
  {
    VLOG_I("[8] BagProcessor -- time-ordered output");

    vlink::BagProcessor::Config pcfg;
    pcfg.min_cache_time = 200;
    vlink::BagProcessor processor(pcfg);

    std::atomic<int> ordered{0};
    processor.register_output_callback([&ordered](const vlink::Frame&) { ++ordered; });

    auto reader = vlink::BagReader::create(kBagPath);
    reader->register_output_callback([&processor](const vlink::Frame& frame) {
      vlink::Frame out;
      out.timestamp = frame.timestamp;
      out.url = frame.url;
      out.action_type = frame.action_type;
      out.data = frame.data;
      processor.push(frame.timestamp, out);
    });
    reader->async_run();

    vlink::BagReader::Config cfg;
    cfg.rate = 20.0;
    reader->play(cfg);

    std::this_thread::sleep_for(3s);
    VLOG_I("  ordered messages: ", ordered.load());
  }

  VLOG_I("BagWriter + BagReader example complete.");
  return 0;
}
