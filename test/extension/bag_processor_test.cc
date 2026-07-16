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

#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
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

struct BagProcessorOutput final {
  int64_t timestamp{0};
  std::string url;
};

TEST_SUITE("extension-BagProcessor") {
  TEST_CASE("config default construction yields expected limits") {
    BagProcessor::Config cfg;
    CHECK_EQ(cfg.min_cache_time, 500);
    CHECK_EQ(cfg.max_cache_size, 1024LL * 1024LL * 256);
    CHECK_EQ(cfg.max_jump_time, 60LL * 60LL * 1000LL);
  }

  TEST_CASE("config fields are mutable") {
    BagProcessor::Config cfg;
    cfg.min_cache_time = 50;
    cfg.max_cache_size = 1024;
    cfg.max_jump_time = 200;
    CHECK_EQ(cfg.min_cache_time, 50);
    CHECK_EQ(cfg.max_cache_size, 1024);
    CHECK_EQ(cfg.max_jump_time, 200);
  }

  TEST_CASE("default construction and destruction without input is safe") { BagProcessor processor; }

  TEST_CASE("push before output callback is rejected") {
    BagProcessor::Config cfg;
    cfg.min_cache_time = 10;
    BagProcessor processor(cfg);

    CHECK_THROWS_AS(processor.push(1, make_frame(1)), std::runtime_error&);
  }

  TEST_CASE("flush before output callback is a no-op") {
    BagProcessor::Config cfg;
    cfg.min_cache_time = 10;
    BagProcessor processor(cfg);

    CHECK_NOTHROW(processor.flush());
  }

  TEST_CASE("empty output callback is rejected") {
    BagProcessor::Config cfg;
    cfg.min_cache_time = 10;
    BagProcessor processor(cfg);

    CHECK_THROWS_AS(processor.register_output_callback(BagProcessor::OutputCallback()), std::runtime_error&);
  }

  TEST_CASE("output callback reregister is ignored") {
    std::vector<int64_t> received;

    BagProcessor::Config cfg;
    cfg.min_cache_time = 60'000;
    BagProcessor processor(cfg);

    processor.register_output_callback([&](const Frame& frame) { received.push_back(frame.timestamp); });
    processor.register_output_callback([](const Frame&) {});
    CHECK_NOTHROW(processor.register_output_callback(BagProcessor::OutputCallback()));

    processor.push(1000, make_frame(1000));
    processor.flush();

    REQUIRE_EQ(received.size(), 1u);
    CHECK_EQ(received[0], 1000);
  }

  TEST_CASE("worker starts after output callback registration") {
    std::vector<int64_t> received;

    BagProcessor::Config cfg;
    cfg.min_cache_time = 60'000;
    BagProcessor processor(cfg);

    processor.flush();
    processor.register_output_callback([&](const Frame& frame) { received.push_back(frame.timestamp); });
    processor.push(1000, make_frame(1000));
    processor.flush();

    REQUIRE_EQ(received.size(), 1u);
    CHECK_EQ(received[0], 1000);
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

  TEST_CASE("flush drains all buffered frames synchronously with remapped timestamps") {
    std::vector<BagProcessorOutput> received;

    BagProcessor::Config cfg;
    cfg.min_cache_time = 60'000;  // 60s window: nothing auto-flushes during the test
    BagProcessor processor(cfg);

    processor.register_output_callback([&](const Frame& frame) { received.push_back({frame.timestamp, frame.url}); });

    processor.push(3000, make_frame(100000, "data-3000"));
    processor.push(1000, make_frame(100010, "data-1000"));
    processor.push(2000, make_frame(100020, "data-2000"));

    processor.flush();  // blocks until the worker has drained everything

    REQUIRE_EQ(received.size(), 3u);
    CHECK_EQ(received[0].url, "data-1000");
    CHECK_EQ(received[1].url, "data-2000");
    CHECK_EQ(received[2].url, "data-3000");
    CHECK_EQ(received[0].timestamp, 100010);
    CHECK_EQ(received[1].timestamp, 101010);
    CHECK_EQ(received[2].timestamp, 102010);
  }

  TEST_CASE("flush starts the next segment on a fresh timestamp axis") {
    std::vector<int64_t> received;

    BagProcessor::Config cfg;
    cfg.min_cache_time = 60'000;
    BagProcessor processor(cfg);

    processor.register_output_callback([&](const Frame& frame) { received.push_back(frame.timestamp); });

    processor.push(1000, make_frame(5000));
    processor.push(2000, make_frame(5010));
    processor.flush();

    processor.push(100'000, make_frame(100));
    processor.push(100'001, make_frame(101));
    processor.flush();

    REQUIRE_EQ(received.size(), 4u);
    CHECK_EQ(received[0], 5000);
    CHECK_EQ(received[1], 6000);
    CHECK_EQ(received[2], 100);
    CHECK_EQ(received[3], 101);
  }

  TEST_CASE("reset discards buffered frames and clears data-time anchors") {
    std::vector<BagProcessorOutput> received;

    BagProcessor::Config cfg;
    cfg.min_cache_time = 60'000;
    cfg.max_jump_time = 100;
    BagProcessor processor(cfg);

    processor.register_output_callback([&](const Frame& frame) { received.push_back({frame.timestamp, frame.url}); });

    processor.push(1'000'000, make_frame(50'000, "old"));
    processor.reset();

    processor.push(10'000, make_frame(100, "new-later"));
    processor.push(1'000, make_frame(200, "new-earlier"));
    processor.flush();

    REQUIRE_EQ(received.size(), 2u);
    CHECK_EQ(received[0].url, "new-earlier");
    CHECK_EQ(received[1].url, "new-later");
    CHECK_EQ(received[0].timestamp, 200);
    CHECK_EQ(received[1].timestamp, 9'200);
  }

  TEST_CASE("reset before output callback registration is a no-op") {
    BagProcessor processor;
    CHECK_NOTHROW(processor.reset());
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

  TEST_CASE("timestamp stays strictly increasing for identical data-plane time") {
    std::vector<int64_t> received;

    BagProcessor::Config cfg;
    cfg.min_cache_time = 60'000;
    BagProcessor processor(cfg);

    processor.register_output_callback([&](const Frame& frame) { received.push_back(frame.timestamp); });

    processor.push(1000, make_frame(5000, "data-1"));
    processor.push(1000, make_frame(4000, "data-2"));
    processor.flush();

    REQUIRE_EQ(received.size(), 2u);
    CHECK_EQ(received[0], 5000);
    CHECK_EQ(received[1], 5001);
  }

  TEST_CASE("missing data-plane time is filled from timestamp delta") {
    std::vector<BagProcessorOutput> received;

    BagProcessor::Config cfg;
    cfg.min_cache_time = 60'000;
    BagProcessor processor(cfg);

    processor.register_output_callback([&](const Frame& frame) { received.push_back({frame.timestamp, frame.url}); });

    processor.push(1000, make_frame(5000, "marked"));
    processor.push(-1, make_frame(5010, "missing"));
    processor.flush();

    REQUIRE_EQ(received.size(), 2u);
    CHECK_EQ(received[0].url, "marked");
    CHECK_EQ(received[1].url, "missing");
    CHECK_EQ(received[0].timestamp, 5000);
    CHECK_EQ(received[1].timestamp, 5010);
  }

  TEST_CASE("abnormal data-plane time jump is filled from timestamp delta") {
    std::vector<BagProcessorOutput> received;

    BagProcessor::Config cfg;
    cfg.min_cache_time = 60'000;
    cfg.max_jump_time = 1;
    BagProcessor processor(cfg);

    processor.register_output_callback([&](const Frame& frame) { received.push_back({frame.timestamp, frame.url}); });

    processor.push(1000, make_frame(5000, "anchor"));
    processor.push(10'000'000, make_frame(5010, "forward-jump"));
    processor.push(0, make_frame(5020, "backward-jump"));
    processor.flush();

    REQUIRE_EQ(received.size(), 3u);
    CHECK_EQ(received[0].url, "anchor");
    CHECK_EQ(received[1].url, "forward-jump");
    CHECK_EQ(received[2].url, "backward-jump");
    CHECK_EQ(received[0].timestamp, 5000);
    CHECK_EQ(received[1].timestamp, 5010);
    CHECK_EQ(received[2].timestamp, 5020);
  }

  TEST_CASE("filled negative data-plane time participates in remapping") {
    std::vector<BagProcessorOutput> received;

    BagProcessor::Config cfg;
    cfg.min_cache_time = 60'000;
    BagProcessor processor(cfg);

    processor.register_output_callback([&](const Frame& frame) { received.push_back({frame.timestamp, frame.url}); });

    processor.push(1000, make_frame(5000, "marked-1000"));
    processor.push(-1, make_frame(3000, "filled-minus-1000"));
    processor.push(0, make_frame(5010, "marked-0"));
    processor.flush();

    REQUIRE_EQ(received.size(), 3u);
    CHECK_EQ(received[0].url, "filled-minus-1000");
    CHECK_EQ(received[1].url, "marked-0");
    CHECK_EQ(received[2].url, "marked-1000");
    CHECK_EQ(received[0].timestamp, 3000);
    CHECK_EQ(received[1].timestamp, 4000);
    CHECK_EQ(received[2].timestamp, 5000);
  }

  TEST_CASE("consecutive initial missing data-plane times remain on timestamp axis") {
    std::vector<BagProcessorOutput> received;

    BagProcessor::Config cfg;
    cfg.min_cache_time = 60'000;
    BagProcessor processor(cfg);

    processor.register_output_callback([&](const Frame& frame) { received.push_back({frame.timestamp, frame.url}); });

    processor.push(-1, make_frame(1000, "missing-1"));
    processor.push(-2, make_frame(1010, "missing-2"));
    processor.push(-3, make_frame(1030, "missing-3"));
    processor.flush();

    REQUIRE_EQ(received.size(), 3u);
    CHECK_EQ(received[0].url, "missing-1");
    CHECK_EQ(received[1].url, "missing-2");
    CHECK_EQ(received[2].url, "missing-3");
    CHECK_EQ(received[0].timestamp, 1000);
    CHECK_EQ(received[1].timestamp, 1010);
    CHECK_EQ(received[2].timestamp, 1030);
  }

  TEST_CASE("initial missing data-plane times advance the cache window on canonical time") {
    std::vector<BagProcessorOutput> received;
    std::mutex mtx;
    ConditionVariable cv;

    BagProcessor::Config cfg;
    cfg.min_cache_time = 1;
    BagProcessor processor(cfg);

    processor.register_output_callback([&](const Frame& frame) {
      std::lock_guard lock(mtx);
      received.push_back({frame.timestamp, frame.url});
      cv.notify_all();
    });

    processor.push(-1, make_frame(3000, "missing-middle"));
    processor.push(-1, make_frame(1000, "missing-earlier"));
    processor.push(-1, make_frame(4001, "missing-later"));

    {
      std::unique_lock lock(mtx);
      REQUIRE(cv.wait_for(lock, 2s, [&] { return received.size() >= 2u; }));
      CHECK_EQ(received[0].url, "missing-earlier");
      CHECK_EQ(received[1].url, "missing-middle");
    }

    processor.flush();

    std::lock_guard lock(mtx);
    REQUIRE_EQ(received.size(), 3u);
    CHECK_EQ(received[2].url, "missing-later");
  }

  TEST_CASE("initial missing data-plane time stays before marked data-plane time") {
    std::vector<BagProcessorOutput> received;

    BagProcessor::Config cfg;
    cfg.min_cache_time = 60'000;
    BagProcessor processor(cfg);

    processor.register_output_callback([&](const Frame& frame) { received.push_back({frame.timestamp, frame.url}); });

    processor.push(-1, make_frame(1000, "missing"));
    processor.push(500, make_frame(1010, "marked"));
    processor.flush();

    REQUIRE_EQ(received.size(), 2u);
    CHECK_EQ(received[0].url, "missing");
    CHECK_EQ(received[1].url, "marked");
    CHECK_EQ(received[0].timestamp, 1000);
    CHECK_EQ(received[1].timestamp, 1010);
  }

  TEST_CASE("first marked data-plane time anchors after monotonic adjustment") {
    std::vector<BagProcessorOutput> received;

    BagProcessor::Config cfg;
    cfg.min_cache_time = 60'000;
    BagProcessor processor(cfg);

    processor.register_output_callback([&](const Frame& frame) { received.push_back({frame.timestamp, frame.url}); });

    processor.push(-1, make_frame(5000, "missing"));
    processor.push(1000, make_frame(1000, "marked-1"));
    processor.push(2000, make_frame(1010, "marked-2"));
    processor.flush();

    REQUIRE_EQ(received.size(), 3u);
    CHECK_EQ(received[0].url, "missing");
    CHECK_EQ(received[1].url, "marked-1");
    CHECK_EQ(received[2].url, "marked-2");
    CHECK_EQ(received[0].timestamp, 5000);
    CHECK_EQ(received[1].timestamp, 5001);
    CHECK_EQ(received[2].timestamp, 6001);
  }

  TEST_CASE("automatic output before flush uses remapped timestamp") {
    std::vector<BagProcessorOutput> received;
    std::mutex mtx;
    ConditionVariable cv;

    BagProcessor::Config cfg;
    cfg.min_cache_time = 60'000;
    auto processor = std::make_unique<BagProcessor>(cfg);

    processor->register_output_callback([&](const Frame& frame) {
      std::lock_guard lock(mtx);
      received.push_back({frame.timestamp, frame.url});
      cv.notify_all();
    });

    processor->push(61'000'000, make_frame(100000, "data-61000000"));
    processor->push(1'000'000, make_frame(100010, "data-1000000"));

    {
      std::unique_lock lock(mtx);
      REQUIRE(cv.wait_for(lock, 2s, [&] { return received.size() == 1u; }));
    }

    processor->flush();

    std::lock_guard lock(mtx);
    REQUIRE_EQ(received.size(), 2u);
    CHECK_EQ(received[0].url, "data-1000000");
    CHECK_EQ(received[1].url, "data-61000000");
    CHECK_EQ(received[0].timestamp, 100010);
    CHECK_EQ(received[1].timestamp, 60'100'010);

    processor.reset();
  }

  TEST_CASE("wall time does not advance the data-plane reorder window") {
    std::vector<BagProcessorOutput> received;
    std::mutex mtx;

    BagProcessor::Config cfg;
    cfg.min_cache_time = 10;
    BagProcessor processor(cfg);

    processor.register_output_callback([&](const Frame& frame) {
      std::lock_guard lock(mtx);
      received.push_back({frame.timestamp, frame.url});
    });

    processor.push(1'618'807'486, make_frame(1000, "pointcloud"));
    std::this_thread::sleep_for(30ms);

    {
      std::lock_guard lock(mtx);
      CHECK(received.empty());
    }

    processor.push(1'618'800'059, make_frame(2000, "objects"));
    processor.flush();

    std::lock_guard lock(mtx);
    REQUIRE_EQ(received.size(), 2u);
    CHECK_EQ(received[0].url, "objects");
    CHECK_EQ(received[1].url, "pointcloud");
  }

  TEST_CASE("max cache size forces output in data-plane order") {
    std::vector<int64_t> received;
    std::mutex mtx;
    ConditionVariable cv;

    BagProcessor::Config cfg;
    cfg.min_cache_time = 60'000;
    cfg.max_cache_size = 5;
    auto processor = std::make_unique<BagProcessor>(cfg);

    processor->register_output_callback([&](const Frame& frame) {
      std::lock_guard lock(mtx);
      received.push_back(frame.timestamp);
      cv.notify_all();
    });

    processor->push(2000, make_frame(2000));
    processor->push(1000, make_frame(1000));

    {
      std::unique_lock lock(mtx);
      REQUIRE(cv.wait_for(lock, 2s, [&] { return !received.empty(); }));
      CHECK_EQ(received[0], 1000);
    }

    processor->flush();
    {
      std::lock_guard lock(mtx);
      REQUIRE_EQ(received.size(), 2u);
      CHECK_EQ(received[1], 2000);
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
    processor->flush();

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

  TEST_CASE("shallow payload is deep-copied when queued") {
    Frame out;

    BagProcessor::Config cfg;
    cfg.min_cache_time = 60'000;
    BagProcessor processor(cfg);

    processor.register_output_callback([&out](const Frame& frame) { out = frame; });

    std::vector<uint8_t> payload{1, 2, 3, 4};
    Frame in = make_frame(1000, "dds://shallow");
    in.data = Bytes::shallow_copy(payload.data(), payload.size());

    processor.push(1000, in);

    payload.assign(payload.size(), 9);
    processor.flush();

    REQUIRE_EQ(out.data.size(), 4u);
    CHECK_EQ(out.data[0], 1);
    CHECK_EQ(out.data[1], 2);
    CHECK_EQ(out.data[2], 3);
    CHECK_EQ(out.data[3], 4);
  }

  TEST_CASE("concurrent push preserves data-plane order") {
    std::vector<int64_t> received;
    std::mutex mtx;

    BagProcessor::Config cfg;
    cfg.min_cache_time = 60'000;
    BagProcessor processor(cfg);

    processor.register_output_callback([&](const Frame& frame) {
      std::lock_guard lock(mtx);
      received.push_back(frame.timestamp);
    });

    static constexpr int64_t kThreadCount = 4;
    static constexpr int64_t kFramesPerThread = 25;
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);

    for (int64_t thread_index = 0; thread_index < kThreadCount; ++thread_index) {
      threads.emplace_back([&processor, thread_index]() {
        for (int64_t index = 0; index < kFramesPerThread; ++index) {
          const int64_t timestamp = (kFramesPerThread - index - 1) * kThreadCount + thread_index;
          processor.push(timestamp, make_frame(timestamp));
        }
      });
    }

    for (auto& thread : threads) {
      thread.join();
    }

    processor.flush();

    std::lock_guard lock(mtx);
    REQUIRE_EQ(received.size(), static_cast<size_t>(kThreadCount * kFramesPerThread));
    for (size_t index = 0; index < received.size(); ++index) {
      CHECK_EQ(received[index], static_cast<int64_t>(index));
    }
  }

  TEST_CASE("data-plane window retains its tail until flush") {
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
      REQUIRE(cv.wait_for(lock, 2s, [&] { return received.size() >= 2u; }));
      CHECK_EQ(received[0], 1);
      CHECK_EQ(received[1], 2000);
    }

    processor->flush();

    {
      std::lock_guard lock(mtx);
      REQUIRE_EQ(received.size(), 3u);
      CHECK_EQ(received[2], 5001);
    }

    processor.reset();
  }
}

// NOLINTEND
