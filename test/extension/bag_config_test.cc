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

#include <doctest/doctest.h>

#include <string>
#include <unordered_set>

#include "../common_test.h"
#include "./extension/bag_reader.h"
#include "./extension/bag_writer.h"

TEST_SUITE("extension-BagWriter") {
  TEST_CASE("compress type enum values are sequential and distinct") {
    CHECK_EQ(static_cast<uint8_t>(BagWriter::kCompressNone), 0u);
    CHECK_EQ(static_cast<uint8_t>(BagWriter::kCompressAuto), 1u);
    CHECK_EQ(static_cast<uint8_t>(BagWriter::kCompressZstd), 2u);
    CHECK_EQ(static_cast<uint8_t>(BagWriter::kCompressLz4), 3u);
    CHECK_EQ(static_cast<uint8_t>(BagWriter::kCompressLzav), 4u);

    CHECK_NE(BagWriter::kCompressNone, BagWriter::kCompressAuto);
    CHECK_NE(BagWriter::kCompressAuto, BagWriter::kCompressZstd);
    CHECK_NE(BagWriter::kCompressZstd, BagWriter::kCompressLz4);
    CHECK_NE(BagWriter::kCompressLz4, BagWriter::kCompressLzav);
  }

  TEST_CASE("config default construction yields zero/false/empty fields") {
    BagWriter::Config cfg;

    CHECK(cfg.tag_name.empty());
    CHECK_EQ(cfg.compress, BagWriter::kCompressNone);
    CHECK_FALSE(cfg.wal_mode);
    CHECK_FALSE(cfg.enable_limit);
    CHECK_FALSE(cfg.split_name_by_time);
    CHECK_FALSE(cfg.sync_mode);
    CHECK_FALSE(cfg.optimize_on_exit);
    CHECK_EQ(cfg.max_row_count, 5'000'000'000LL);
    CHECK_EQ(cfg.max_bytes_size, 1024LL * 1024LL * 1024LL * 512LL);
    CHECK_EQ(cfg.split_by_size, 1024LL * 1024LL * 1024LL * 1LL);
    CHECK_EQ(cfg.split_by_time, 0);
    CHECK_EQ(cfg.begin_time, 0);
    CHECK_EQ(cfg.cache_size, 1024LL * 1024LL * 4);
    CHECK_EQ(cfg.compress_start_size, 128);
    CHECK_EQ(cfg.compress_level, 3);
    CHECK_EQ(cfg.max_task_depth, 20000);
    CHECK_EQ(cfg.max_memory_size, 1024LL * 1024LL * 1024LL * 2LL);
    CHECK_EQ(cfg.start_timestamp, 0);
    CHECK(cfg.ignore_compress_urls.empty());
  }

  TEST_CASE("config fields are independently mutable") {
    BagWriter::Config cfg;

    cfg.tag_name = "run_42";
    cfg.compress = BagWriter::kCompressZstd;
    cfg.wal_mode = true;
    cfg.split_by_size = 512LL * 1024 * 1024;
    cfg.split_by_time = 60'000;
    cfg.ignore_compress_urls.insert("dds://sensor/lidar");
    cfg.ignore_compress_urls.insert("dds://sensor/camera");

    CHECK_EQ(cfg.tag_name, "run_42");
    CHECK_EQ(cfg.compress, BagWriter::kCompressZstd);
    CHECK(cfg.wal_mode);
    CHECK_EQ(cfg.split_by_size, 512LL * 1024 * 1024);
    CHECK_EQ(cfg.split_by_time, 60'000);
    CHECK_EQ(cfg.ignore_compress_urls.size(), 2u);
    CHECK_EQ(cfg.ignore_compress_urls.count("dds://sensor/lidar"), 1u);
    CHECK_EQ(cfg.ignore_compress_urls.count("dds://sensor/camera"), 1u);
  }
}

TEST_SUITE("extension-BagReader") {
  TEST_CASE("kInfinite sentinel equals -1") { CHECK_EQ(BagReader::kInfinite, -1); }

  TEST_CASE("status enum values are sequential and distinct") {
    CHECK_EQ(static_cast<uint8_t>(BagReader::kStopped), 0u);
    CHECK_EQ(static_cast<uint8_t>(BagReader::kPaused), 1u);
    CHECK_EQ(static_cast<uint8_t>(BagReader::kPlaying), 2u);

    CHECK_NE(BagReader::kStopped, BagReader::kPaused);
    CHECK_NE(BagReader::kPaused, BagReader::kPlaying);
    CHECK_NE(BagReader::kStopped, BagReader::kPlaying);
  }

  TEST_CASE("config default construction yields zero/false/empty fields") {
    BagReader::Config cfg;

    CHECK_EQ(cfg.begin_time, 0);
    CHECK_EQ(cfg.end_time, 0);
    CHECK_EQ(cfg.times, 1);
    CHECK_EQ(cfg.rate, doctest::Approx(1.0));
    CHECK_FALSE(cfg.skip_blank);
    CHECK_EQ(cfg.force_delay, -1);
    CHECK_FALSE(cfg.auto_pause);
    CHECK_FALSE(cfg.auto_quit);
    CHECK(cfg.filter_urls.empty());
  }

  TEST_CASE("config fields are independently mutable") {
    BagReader::Config cfg;

    cfg.times = BagReader::kInfinite;
    cfg.rate = 2.5;
    cfg.skip_blank = true;
    cfg.begin_time = 1'000'000;
    cfg.end_time = 5'000'000;
    cfg.filter_urls.insert("dds://sensor/lidar");

    CHECK_EQ(cfg.times, -1);
    CHECK_EQ(cfg.rate, doctest::Approx(2.5));
    CHECK(cfg.skip_blank);
    CHECK_EQ(cfg.begin_time, 1'000'000);
    CHECK_EQ(cfg.end_time, 5'000'000);
    CHECK_EQ(cfg.filter_urls.count("dds://sensor/lidar"), 1u);
  }

  TEST_CASE("info default construction yields zero/false/empty fields") {
    BagReader::Info info;

    CHECK(info.file_name.empty());
    CHECK(info.tag_name.empty());
    CHECK(info.version.empty());
    CHECK(info.storage_type.empty());
    CHECK(info.compression_type.empty());
    CHECK(info.time_accuracy.empty());
    CHECK(info.process_name.empty());
    CHECK(info.date_time.empty());
    CHECK_FALSE(info.has_completed);
    CHECK_FALSE(info.has_idx_elapsed);
    CHECK_FALSE(info.has_idx_url);
    CHECK_FALSE(info.has_schema);
    CHECK_EQ(info.timezone, 0);
    CHECK_EQ(info.start_timestamp, 0);
    CHECK_EQ(info.blank_duration, 0);
    CHECK_EQ(info.total_duration, 0);
    CHECK_EQ(info.file_size, 0);
    CHECK_EQ(info.total_raw_size, 0);
    CHECK_EQ(info.message_count, 0);
    CHECK_EQ(info.split_count, 0);
    CHECK_EQ(info.split_by_size, 0);
    CHECK_EQ(info.split_by_time, 0);
    CHECK(info.url_metas.empty());
  }

  TEST_CASE("url meta default construction yields zero/false/empty fields") {
    BagReader::Info::UrlMeta meta;

    CHECK_FALSE(meta.valid);
    CHECK_EQ(meta.index, 0);
    CHECK(meta.url.empty());
    CHECK(meta.url_type.empty());
    CHECK_EQ(meta.action_type, ActionType::kUnknownAction);
    CHECK(meta.ser_type.empty());
    CHECK_EQ(meta.count, 0u);
    CHECK_EQ(meta.size, 0u);
    CHECK_EQ(meta.freq, doctest::Approx(0.0));
    CHECK_EQ(meta.loss, doctest::Approx(0.0));
  }

  TEST_CASE("url meta fields are mutable") {
    BagReader::Info::UrlMeta meta;

    meta.valid = true;
    meta.index = 7;
    meta.url = "dds://sensor/lidar";
    meta.url_type = "Event";
    meta.action_type = ActionType::kPublish;
    meta.ser_type = "demo.proto.PointCloud";
    meta.count = 1000;
    meta.size = 1024 * 1024;
    meta.freq = 10.0;
    meta.loss = 0.01;

    CHECK(meta.valid);
    CHECK_EQ(meta.index, 7);
    CHECK_EQ(meta.url, "dds://sensor/lidar");
    CHECK_EQ(meta.url_type, "Event");
    CHECK_EQ(meta.action_type, ActionType::kPublish);
    CHECK_EQ(meta.ser_type, "demo.proto.PointCloud");
    CHECK_EQ(meta.count, 1000u);
    CHECK_EQ(meta.size, 1024u * 1024u);
    CHECK_EQ(meta.freq, doctest::Approx(10.0));
    CHECK_EQ(meta.loss, doctest::Approx(0.01));
  }

  TEST_CASE("url meta operator< orders by url then by index") {
    BagReader::Info::UrlMeta a;
    a.url = "dds://topic";
    a.index = 1;

    BagReader::Info::UrlMeta b;
    b.url = "dds://topic";
    b.index = 2;

    CHECK(a < b);
    CHECK_FALSE(b < a);

    BagReader::Info::UrlMeta c;
    c.url = "dds://aaa";
    c.index = 99;

    BagReader::Info::UrlMeta d;
    d.url = "dds://bbb";
    d.index = 1;

    CHECK(c < d);
    CHECK_FALSE(d < c);
  }
}

// NOLINTEND
