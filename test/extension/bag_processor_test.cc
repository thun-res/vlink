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

#include "./extension/bag_processor.h"

#include <doctest/doctest.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../common_test.h"

Frame make_frame(int64_t timestamp, const std::string& url = "intra://x") {
  Frame frame;
  frame.timestamp = timestamp;
  frame.url = url;
  frame.action_type = ActionType::kPublish;
  frame.data = Bytes::create(4u);

  return frame;
}

TEST_SUITE("extension-BagProcessor") {
  TEST_CASE("config default construction yields expected limits") {
    BagProcessor::Config cfg;
    CHECK_EQ(cfg.min_cache_time, 500);
    CHECK_EQ(cfg.max_cache_size, 1024LL * 1024LL * 256);
  }

  TEST_CASE("config fields are mutable") {
    BagProcessor::Config cfg;
    cfg.min_cache_time = 50;
    cfg.max_cache_size = 1024;
    CHECK_EQ(cfg.min_cache_time, 50);
    CHECK_EQ(cfg.max_cache_size, 1024);
  }

  TEST_CASE("default construction and destruction without input is safe") { BagProcessor processor; }

  TEST_CASE("push without output callback does not crash") {
    BagProcessor::Config cfg;
    cfg.min_cache_time = 10;
    BagProcessor processor(cfg);

    processor.push(1, make_frame(1));
    processor.push(2, make_frame(2));
  }

  TEST_CASE("processor delivers all frames in ascending data-plane-time order on destruction") {
    std::vector<int64_t> received;
    std::mutex mtx;

    BagProcessor::Config cfg;
    cfg.min_cache_time = 100;
    auto processor = std::make_unique<BagProcessor>(cfg);

    processor->register_output_callback([&](const Frame& frame) {
      std::lock_guard lock(mtx);
      received.push_back(frame.timestamp);
    });

    processor->push(1, make_frame(1));
    processor->push(2000, make_frame(2000));
    processor->push(5001, make_frame(5001));

    processor.reset();

    std::lock_guard lock(mtx);
    REQUIRE_EQ(received.size(), 3u);
    CHECK_EQ(received[0], 1);
    CHECK_EQ(received[1], 2000);
    CHECK_EQ(received[2], 5001);
  }

  TEST_CASE("out-of-order pushes are sorted by data-plane time before delivery") {
    std::vector<int64_t> received;
    std::mutex mtx;
    ConditionVariable cv;

    BagProcessor::Config cfg;
    cfg.min_cache_time = 5000;
    auto processor = std::make_unique<BagProcessor>(cfg);

    processor->register_output_callback([&](const Frame& frame) {
      std::lock_guard lock(mtx);
      received.push_back(frame.timestamp);
      cv.notify_all();
    });

    processor->push(50'000'001, make_frame(50'000'001));
    processor->push(1, make_frame(1));
    processor->push(20'000'000, make_frame(20'000'000));
    processor->push(100'000'000, make_frame(100'000'000));

    {
      std::unique_lock lock(mtx);
      REQUIRE(cv.wait_for(lock, 2s, [&] { return received.size() >= 3u; }));
      CHECK_EQ(received[0], 1);
      CHECK_EQ(received[1], 20'000'000);
      CHECK_EQ(received[2], 50'000'001);
    }

    processor.reset();
  }

  TEST_CASE("flush drains all buffered frames synchronously, in data-plane order, before the cache window") {
    std::vector<int64_t> received;
    std::mutex mtx;

    BagProcessor::Config cfg;
    cfg.min_cache_time = 60'000;  // 60s window: nothing auto-flushes during the test
    BagProcessor processor(cfg);

    processor.register_output_callback([&](const Frame& frame) {
      std::lock_guard lock(mtx);
      received.push_back(frame.timestamp);
    });

    processor.push(30'000'000, make_frame(30'000'000));
    processor.push(10'000'000, make_frame(10'000'000));
    processor.push(20'000'000, make_frame(20'000'000));

    processor.flush();  // blocks until the worker has drained everything

    std::lock_guard lock(mtx);
    REQUIRE_EQ(received.size(), 3u);
    CHECK_EQ(received[0], 10'000'000);
    CHECK_EQ(received[1], 20'000'000);
    CHECK_EQ(received[2], 30'000'000);
  }

  TEST_CASE("flush is a no-op on an empty cache and after a prior drain") {
    std::vector<int64_t> received;
    std::mutex mtx;

    BagProcessor::Config cfg;
    cfg.min_cache_time = 60'000;
    BagProcessor processor(cfg);

    processor.register_output_callback([&](const Frame& frame) {
      std::lock_guard lock(mtx);
      received.push_back(frame.timestamp);
    });

    processor.flush();
    {
      std::lock_guard lock(mtx);
      CHECK_EQ(received.size(), 0u);
    }

    processor.push(1, make_frame(1));
    processor.flush();
    {
      std::lock_guard lock(mtx);
      REQUIRE_EQ(received.size(), 1u);
    }

    processor.flush();
    {
      std::lock_guard lock(mtx);
      CHECK_EQ(received.size(), 1u);
    }
  }

  TEST_CASE("reorder follows the data-plane key, not Frame::timestamp") {
    std::vector<int64_t> received;
    std::mutex mtx;
    ConditionVariable cv;

    BagProcessor::Config cfg;
    cfg.min_cache_time = 5000;
    auto processor = std::make_unique<BagProcessor>(cfg);

    processor->register_output_callback([&](const Frame& frame) {
      std::lock_guard lock(mtx);
      received.push_back(frame.timestamp);
      cv.notify_all();
    });

    processor->push(50'000'001, make_frame(111));
    processor->push(1, make_frame(222));
    processor->push(20'000'000, make_frame(333));
    processor->push(100'000'000, make_frame(444));

    {
      std::unique_lock lock(mtx);
      REQUIRE(cv.wait_for(lock, 2s, [&] { return received.size() >= 3u; }));
      CHECK_EQ(received[0], 222);
      CHECK_EQ(received[1], 333);
      CHECK_EQ(received[2], 111);
    }

    processor.reset();
  }

  TEST_CASE("frame carries full metadata through the buffer") {
    Frame out;
    bool received{false};
    std::mutex mtx;
    ConditionVariable cv;

    BagProcessor::Config cfg;
    cfg.min_cache_time = 1;
    auto processor = std::make_unique<BagProcessor>(cfg);

    processor->register_output_callback([&](const Frame& frame) {
      std::lock_guard lock(mtx);
      out = frame;
      received = true;
      cv.notify_all();
    });

    Frame in;
    in.timestamp = 222;
    in.url = "dds://meta";
    in.ser_type = "jpeg";
    in.schema_type = SchemaType::kProtobuf;
    in.action_type = ActionType::kSubscribe;
    in.data = Bytes::create(7u);

    processor->push(222, in);

    {
      std::unique_lock lock(mtx);
      REQUIRE(cv.wait_for(lock, 2s, [&] { return received; }));
      CHECK_EQ(out.timestamp, 222);
      CHECK_EQ(out.url, "dds://meta");
      CHECK_EQ(out.ser_type, "jpeg");
      CHECK_EQ(out.schema_type, SchemaType::kProtobuf);
      CHECK_EQ(out.action_type, ActionType::kSubscribe);
      CHECK_EQ(out.data.size(), 7u);
    }

    processor.reset();
  }

  TEST_CASE("processor flushes all cached frames on destruction after timeout") {
    std::vector<int64_t> received;
    std::mutex mtx;
    ConditionVariable cv;

    BagProcessor::Config cfg;
    cfg.min_cache_time = 1;
    auto processor = std::make_unique<BagProcessor>(cfg);

    processor->register_output_callback([&](const Frame& frame) {
      std::lock_guard lock(mtx);
      received.push_back(frame.timestamp);
      cv.notify_all();
    });

    processor->push(1, make_frame(1));
    processor->push(2000, make_frame(2000));
    processor->push(5001, make_frame(5001));

    {
      std::unique_lock lock(mtx);
      REQUIRE(cv.wait_for(lock, 2s, [&] { return received.size() >= 3u; }));
      CHECK_EQ(received[0], 1);
      CHECK_EQ(received[1], 2000);
      CHECK_EQ(received[2], 5001);
    }

    processor.reset();
  }
}

// NOLINTEND
