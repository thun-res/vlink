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

#include "./extension/trigger_recorder.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../common_test.h"
#include "./extension/bag_plugin_interface.h"
#include "./extension/bag_processor.h"
#include "./extension/bag_reader.h"
#include "./extension/trigger_plugin_interface.h"
#include "./publisher.h"

struct ScratchDir final {
  std::string path;

  explicit ScratchDir(const std::string& name) {
    path = (std::filesystem::temp_directory_path() / ("vlink-trigger-test-" + name)).string();
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }

  ~ScratchDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

static bool wait_until_idle(const vlink::TriggerRecorder& recorder, int max_ms = 4000) {
  for (int elapsed = 0; elapsed < max_ms; elapsed += 5) {
    if (!recorder.is_dumping()) {
      return true;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  return !recorder.is_dumping();
}

static vlink::TriggerRecorder::RawSubFactory make_raw_sub_factory() {
  return [](const std::string& url, vlink::InitType type) {
    return vlink::TriggerRecorder::RawSub::create_shared(url, type);
  };
}

static bool wait_until_ready(vlink::TriggerRecorder& recorder, int max_ms = 4000) {
  auto ready = recorder.invoke_task([]() {});

  if (ready.wait_for(std::chrono::milliseconds(max_ms)) != std::future_status::ready) {
    return false;
  }

  ready.get();
  return true;
}

class RecordingTriggerPlugin : public vlink::TriggerPluginInterface {
 public:
  bool init(const std::string&) override { return true; }

  void on_start() override { ++started; }

  void on_stop() override { ++stopped; }

  void on_trigger(const TriggerContext& context) override {
    ++triggered;
    last_reason = context.reason;
  }

  void on_dump_started(const DumpContext&) override { ++dump_started; }

  void on_frame(const Frame&, const DumpContext&) override { ++frames; }

  void on_dump_finished(const DumpResult& result) override {
    ++finished;
    last_result = result;
    finished_paths.push_back(result.path);
  }

  void on_dump_failed(const DumpResult& result) override {
    ++failed;
    last_result = result;
  }

  void on_file_rotated(const std::string& path) override { rotated.push_back(path); }

  void flush() override { ++flushed; }

  std::atomic<int> started{0};
  std::atomic<int> stopped{0};
  std::atomic<int> triggered{0};
  std::atomic<int> dump_started{0};
  std::atomic<int> frames{0};
  std::atomic<int> finished{0};
  std::atomic<int> failed{0};
  std::atomic<int> flushed{0};
  std::string last_reason;
  std::vector<std::string> rotated;
  std::vector<std::string> finished_paths;
  DumpResult last_result;
};

class PublishingTriggerPlugin final : public RecordingTriggerPlugin {
 public:
  enum Stage : uint8_t { kOnTrigger, kOnFirstFinished };

  PublishingTriggerPlugin(vlink::Publisher<vlink::Bytes>* publisher, Stage stage, int count, int interval_ms = 0)
      : publisher_(publisher), stage_(stage), count_(count), interval_ms_(interval_ms) {}

  void on_trigger(const TriggerContext& context) override {
    RecordingTriggerPlugin::on_trigger(context);

    if (stage_ == kOnTrigger) {
      publish_frames();
    }
  }

  void on_dump_finished(const DumpResult& result) override {
    if (stage_ == kOnFirstFinished && finished.load() == 0) {
      publish_frames();
    }

    RecordingTriggerPlugin::on_dump_finished(result);
  }

  void on_frame(const Frame& frame, const DumpContext& context) override {
    RecordingTriggerPlugin::on_frame(frame, context);

    if (frame.data.empty()) {
      return;
    }

    if (frame.data[0] == 0x71) {
      ++original_frames;
    } else if (frame.data[0] == 0x51) {
      ++hook_frames;
    }
  }

  std::atomic<int> original_frames{0};
  std::atomic<int> hook_frames{0};

 private:
  void publish_frames() {
    const vlink::Bytes payload{0x51, 0x52, 0x53, 0x54};

    for (int index = 0; index < count_; ++index) {
      publisher_->publish(payload);

      if (interval_ms_ > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));
      }
    }
  }

  vlink::Publisher<vlink::Bytes>* publisher_;
  Stage stage_;
  int count_;
  int interval_ms_;
};

class CountingBagPlugin final : public vlink::BagPluginInterface {
 public:
  void on_read(const Frame& frame) override {
    ++reads;
    do_callback(frame);
  }

  void on_write(const Frame& frame) override {
    ++writes;
    do_callback(frame);
  }

  std::atomic<int> reads{0};
  std::atomic<int> writes{0};
};

class ReorderBagPlugin final : public vlink::BagPluginInterface {
 public:
  ReorderBagPlugin() : processor_(make_config()) {
    processor_.register_output_callback([this](const Frame& frame) { do_callback(frame); });
  }

  void on_read(const Frame& frame) override { do_callback(frame); }

  void on_write(const Frame& frame) override {
    int64_t data_timestamp = 0;
    std::memcpy(&data_timestamp, frame.data.data(), sizeof(data_timestamp));
    processor_.push(data_timestamp, frame);
  }

  void reset() override { processor_.reset(); }

  void flush() override { processor_.flush(); }

 private:
  static vlink::BagProcessor::Config make_config() {
    vlink::BagProcessor::Config config;
    config.min_cache_time = 60'000;
    return config;
  }

  vlink::BagProcessor processor_;
};

vlink::Bytes make_data_timestamp(int64_t timestamp) {
  auto data = vlink::Bytes::create(sizeof(timestamp));
  std::memcpy(data.data(), &timestamp, sizeof(timestamp));
  return data;
}

TEST_SUITE("extension-TriggerRecorder") {
  TEST_CASE("overflow policy and file type enums have stable wire values") {
    CHECK_EQ(static_cast<uint8_t>(vlink::TriggerRecorder::kCoverOldest), 0u);
    CHECK_EQ(static_cast<uint8_t>(vlink::TriggerRecorder::kDropNewest), 1u);
    CHECK_EQ(static_cast<uint8_t>(vlink::TriggerRecorder::kVdb), 0u);
    CHECK_EQ(static_cast<uint8_t>(vlink::TriggerRecorder::kVcap), 1u);
  }

  TEST_CASE("raw subscriber factory resolves caller-linked transports") {
    auto factory = make_raw_sub_factory();
    auto sub = factory("intra://__trigger_factory_test__", vlink::InitType::kWithoutInit);

    REQUIRE(sub != nullptr);
    CHECK_EQ(sub->get_transport_type(), vlink::TransportType::kIntra);
  }

  TEST_CASE("config default values match the documented recorder defaults") {
    vlink::TriggerRecorder::Config config;

    CHECK(config.dump_dir.empty());
    CHECK_EQ(config.file_type, vlink::TriggerRecorder::kVdb);
    CHECK_EQ(config.default_pre_ms, 60000);
    CHECK_EQ(config.default_post_ms, 5000);
    CHECK_EQ(config.default_max_packet_size, 4LL * 1024 * 1024);
    CHECK_EQ(config.default_max_size, 0);
    CHECK_EQ(config.max_cache_size, 1024LL * 1024 * 1024);
    CHECK_EQ(config.retention_guard_ms, 300);
    CHECK_EQ(config.max_dump_file_count, 16);
    CHECK(config.enable_compress);
    CHECK_FALSE(config.busy_skip_data);
    CHECK_FALSE(config.destroy_on_offline);
    CHECK_EQ(config.overflow, vlink::TriggerRecorder::kCoverOldest);
    CHECK_EQ(config.sleep_interval, 4LL * 1024 * 1024);
    CHECK_EQ(config.sleep_time_ms, 0);
    CHECK_EQ(config.discovery_filter, vlink::DiscoveryViewer::kFilterAvailable);
    CHECK(config.whitelist.empty());
    CHECK(config.blacklist.empty());
    CHECK(config.url_overrides.empty());
  }

  TEST_CASE("url config and trigger params default to sentinel-negative windows") {
    vlink::TriggerRecorder::UrlConfig url_config;

    CHECK_EQ(url_config.pre_ms, -1);
    CHECK_EQ(url_config.post_ms, -1);
    CHECK_EQ(url_config.max_packet_size, -1);
    CHECK_EQ(url_config.max_size, -1);
    CHECK_FALSE(url_config.only_front);
    CHECK_FALSE(url_config.only_back);

    vlink::TriggerRecorder::TriggerParams params;

    CHECK(params.reason.empty());
    CHECK(params.name_hint.empty());
    CHECK(params.out_file.empty());
    CHECK_EQ(params.pre_ms, -1);
    CHECK_EQ(params.post_ms, -1);
    CHECK(params.filter_urls.empty());
    CHECK(params.filter_str.empty());
    CHECK_FALSE(params.black_mode);
  }

  TEST_CASE("a freshly constructed recorder is neither running nor dumping") {
    vlink::TriggerRecorder::Config config;
    vlink::TriggerRecorder recorder(config, make_raw_sub_factory());

    CHECK_FALSE(recorder.is_running());
    CHECK_FALSE(recorder.is_dumping());
  }

  TEST_CASE("construction rejects an empty raw subscriber factory") {
    vlink::TriggerRecorder::Config config;

    CHECK_THROWS(vlink::TriggerRecorder(config, nullptr));
  }

  TEST_CASE("construction rejects unsafe numeric limits") {
    ScratchDir scratch("invalid-limits");

    vlink::TriggerRecorder::Config config;
    config.dump_dir = scratch.path;

    SUBCASE("window conversion would overflow") { config.default_pre_ms = std::numeric_limits<int64_t>::max(); }
    SUBCASE("global byte cap is negative") { config.max_cache_size = -1; }

    CHECK_THROWS(vlink::TriggerRecorder(config, make_raw_sub_factory()));
  }

  TEST_CASE("construction rejects a dump path that is a regular file") {
    ScratchDir scratch("dump-path-file");
    const std::string path = scratch.path + "/blocked";
    std::error_code ec;
    std::filesystem::create_directories(scratch.path, ec);
    REQUIRE_FALSE(ec);

    std::ofstream file(path);
    REQUIRE(file.is_open());
    file.close();

    vlink::TriggerRecorder::Config config;
    config.dump_dir = path;

    CHECK_THROWS(vlink::TriggerRecorder(config, make_raw_sub_factory()));
  }

  TEST_CASE("dump before async_run is rejected") {
    vlink::TriggerRecorder::Config config;
    vlink::TriggerRecorder recorder(config, make_raw_sub_factory());

    CHECK_FALSE(recorder.dump());
  }

  TEST_CASE("trigger rejects an unsafe window without entering dump state") {
    ScratchDir scratch("invalid-trigger");

    vlink::TriggerRecorder::Config config;
    config.dump_dir = scratch.path;
    config.default_pre_ms = 0;
    config.default_post_ms = 0;
    config.whitelist = {"intra://__trigger_test_absent__"};

    vlink::TriggerRecorder recorder(config, make_raw_sub_factory());
    REQUIRE(recorder.async_run());
    REQUIRE(wait_until_ready(recorder));

    vlink::TriggerRecorder::TriggerParams params;
    params.pre_ms = std::numeric_limits<int64_t>::max();

    CHECK_FALSE(recorder.dump(params));
    CHECK_FALSE(recorder.is_dumping());
    recorder.quit();
    recorder.wait_for_quit();
  }

  TEST_CASE("the inherited message-loop lifecycle starts and stops the recorder") {
    ScratchDir scratch("start");

    vlink::TriggerRecorder::Config config;
    config.dump_dir = scratch.path;
    config.whitelist = {"intra://__trigger_test_absent__"};

    vlink::TriggerRecorder recorder(config, make_raw_sub_factory());

    CHECK(std::filesystem::exists(scratch.path));

    REQUIRE(recorder.async_run());
    REQUIRE(wait_until_ready(recorder));
    CHECK(recorder.is_running());
    CHECK_FALSE(recorder.async_run());

    recorder.quit();
    REQUIRE(recorder.wait_for_quit(5000));
    CHECK_FALSE(recorder.is_running());
    CHECK_FALSE(recorder.dump());
  }

  TEST_CASE("discovery restarts with the inherited message-loop lifecycle") {
    const std::string url = "intra://__trigger_test_restart__";
    vlink::Publisher<vlink::Bytes> pub(url);

    vlink::TriggerRecorder::Config config;
    config.whitelist = {url};
    vlink::TriggerRecorder recorder(config, make_raw_sub_factory());

    REQUIRE(recorder.async_run());
    REQUIRE(wait_until_ready(recorder));

    if (!pub.wait_for_subscribers(std::chrono::milliseconds(3000))) {
      recorder.quit();
      recorder.wait_for_quit();
      MESSAGE("multicast discovery unavailable; skipping the restart assertion");
      return;
    }

    recorder.quit();
    REQUIRE(recorder.wait_for_quit(5000));

    for (int elapsed = 0; elapsed < 2000 && pub.has_subscribers(); elapsed += 10) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE_FALSE(pub.has_subscribers());
    REQUIRE(recorder.async_run());
    REQUIRE(wait_until_ready(recorder));
    CHECK(pub.wait_for_subscribers(std::chrono::milliseconds(3000)));
    recorder.quit();
    recorder.wait_for_quit();
  }

  TEST_CASE("quit without a prior async_run is a harmless no-op") {
    vlink::TriggerRecorder::Config config;
    vlink::TriggerRecorder recorder(config, make_raw_sub_factory());

    CHECK_NOTHROW(recorder.quit());
    CHECK_FALSE(recorder.is_running());
  }

  TEST_CASE("construction falls back to a temporary dump directory when none is configured") {
    vlink::TriggerRecorder::Config config;
    config.dump_dir.clear();
    config.whitelist = {"intra://__trigger_test_absent__"};

    vlink::TriggerRecorder recorder(config, make_raw_sub_factory());

    REQUIRE(recorder.async_run());
    REQUIRE(wait_until_ready(recorder));
    CHECK(recorder.is_running());

    recorder.quit();
    recorder.wait_for_quit();
  }

  TEST_CASE("negative default windows and guard are clamped to zero at start") {
    ScratchDir scratch("clamp");

    vlink::TriggerRecorder::Config config;
    config.dump_dir = scratch.path;
    config.default_pre_ms = -5;
    config.default_post_ms = -5;
    config.retention_guard_ms = -5;
    config.whitelist = {"intra://__trigger_test_absent__"};

    vlink::TriggerRecorder recorder(config, make_raw_sub_factory());

    auto plugin = std::make_shared<RecordingTriggerPlugin>();
    recorder.bind_trigger_interface(plugin);

    REQUIRE(recorder.async_run());
    REQUIRE(wait_until_ready(recorder));
    REQUIRE(recorder.dump());
    REQUIRE(wait_until_idle(recorder));
    recorder.quit();
    recorder.wait_for_quit();

    CHECK_EQ(plugin->finished.load(), 1);
  }

  TEST_CASE("an empty window dump still writes a readable bag and drives every trigger-plugin hook") {
    ScratchDir scratch("empty-dump");

    vlink::TriggerRecorder::Config config;
    config.dump_dir = scratch.path;
    config.default_pre_ms = 1000;
    config.default_post_ms = 0;
    config.enable_compress = false;
    config.whitelist = {"intra://__trigger_test_absent__"};

    vlink::TriggerRecorder recorder(config, make_raw_sub_factory());

    auto plugin = std::make_shared<RecordingTriggerPlugin>();
    recorder.bind_trigger_interface(plugin);

    auto bag_plugin = std::make_shared<CountingBagPlugin>();
    recorder.bind_bag_interface(bag_plugin);

    REQUIRE(recorder.async_run());
    REQUIRE(wait_until_ready(recorder));
    CHECK_EQ(plugin->started.load(), 1);

    const std::string out = scratch.path + "/manual.vdb";

    vlink::TriggerRecorder::TriggerParams params;
    params.reason = "manual";
    params.out_file = out;

    REQUIRE(recorder.dump(params));
    REQUIRE(wait_until_idle(recorder));

    recorder.quit();
    recorder.wait_for_quit();

    CHECK_EQ(plugin->triggered.load(), 1);
    CHECK_EQ(plugin->last_reason, "manual");
    CHECK_EQ(plugin->dump_started.load(), 1);
    CHECK_EQ(plugin->finished.load(), 1);
    CHECK_EQ(plugin->failed.load(), 0);
    CHECK_EQ(plugin->frames.load(), 0);
    CHECK_EQ(plugin->stopped.load(), 1);
    CHECK_EQ(plugin->last_result.path, out);
    CHECK(plugin->last_result.success);
    CHECK_EQ(plugin->last_result.frame_count, 0);
    CHECK_EQ(plugin->last_result.url_count, 0);

    CHECK_EQ(bag_plugin->writes.load(), 0);

    REQUIRE(std::filesystem::exists(out));

    auto reader = vlink::BagReader::create(out);
    REQUIRE(reader != nullptr);
    CHECK_EQ(reader->get_info().message_count, 0);
  }

  TEST_CASE("a second trigger is rejected while the first dump is still in flight") {
    ScratchDir scratch("serialize");

    const std::string url = "intra://__trigger_test_serialize__";

    vlink::Publisher<vlink::Bytes> pub(url);

    vlink::TriggerRecorder::Config config;
    config.dump_dir = scratch.path;
    config.default_pre_ms = 0;
    config.default_post_ms = 400;
    config.whitelist = {url};

    vlink::TriggerRecorder recorder(config, make_raw_sub_factory());

    REQUIRE(recorder.async_run());
    REQUIRE(wait_until_ready(recorder));

    if (!pub.wait_for_subscribers(std::chrono::milliseconds(3000))) {
      recorder.quit();
      recorder.wait_for_quit();
      MESSAGE("multicast discovery unavailable; skipping the serialization assertions");
      return;
    }

    REQUIRE(recorder.dump());
    CHECK(recorder.is_dumping());
    CHECK_FALSE(recorder.dump());

    REQUIRE(wait_until_idle(recorder));
    recorder.quit();
    recorder.wait_for_quit();
  }

  TEST_CASE("a delayed dump rejected by a full timer set clears its dump state") {
    ScratchDir scratch("timer-full");
    const std::string url = "intra://__trigger_test_timer_full__";
    vlink::Publisher<vlink::Bytes> pub(url);

    vlink::TriggerRecorder::Config config;
    config.dump_dir = scratch.path;
    config.default_pre_ms = 0;
    config.default_post_ms = 100;
    config.retention_guard_ms = 0;
    config.whitelist = {url};

    vlink::TriggerRecorder recorder(config, make_raw_sub_factory());
    REQUIRE(recorder.async_run());
    REQUIRE(wait_until_ready(recorder));

    if (!pub.wait_for_subscribers(std::chrono::milliseconds(3000))) {
      recorder.quit();
      recorder.wait_for_quit();
      MESSAGE("multicast discovery unavailable; skipping the timer-capacity assertion");
      return;
    }

    std::vector<std::unique_ptr<vlink::Timer>> timers;

    while (true) {
      auto timer = std::make_unique<vlink::Timer>();

      if (!timer->attach(&recorder)) {
        break;
      }

      timers.push_back(std::move(timer));
    }

    REQUIRE_FALSE(timers.empty());
    CHECK_FALSE(recorder.dump());
    CHECK_FALSE(recorder.is_dumping());

    timers.clear();
    recorder.quit();
    recorder.wait_for_quit();
  }

  TEST_CASE("quit abandons a dump still waiting for its post window") {
    ScratchDir scratch("quit-post-window");
    const std::string url = "intra://__trigger_test_quit_post__";
    const std::string out = scratch.path + "/quit-post.vdb";
    vlink::Publisher<vlink::Bytes> pub(url);

    vlink::TriggerRecorder::Config config;
    config.dump_dir = scratch.path;
    config.default_pre_ms = 0;
    config.default_post_ms = 300;
    config.retention_guard_ms = 0;
    config.enable_compress = false;
    config.whitelist = {url};

    vlink::TriggerRecorder recorder(config, make_raw_sub_factory());
    auto plugin = std::make_shared<RecordingTriggerPlugin>();
    recorder.bind_trigger_interface(plugin);
    REQUIRE(recorder.async_run());
    REQUIRE(wait_until_ready(recorder));

    if (!pub.wait_for_subscribers(std::chrono::milliseconds(3000))) {
      recorder.quit();
      recorder.wait_for_quit();
      MESSAGE("multicast discovery unavailable; skipping the pending-dump assertion");
      return;
    }

    vlink::TriggerRecorder::TriggerParams params;
    params.reason = "quit-post";
    params.out_file = out;
    REQUIRE(recorder.dump(params));

    CHECK(recorder.quit());
    REQUIRE(recorder.wait_for_quit(5000));
    CHECK_EQ(plugin->finished.load(), 0);
    REQUIRE_EQ(plugin->failed.load(), 1);
    CHECK_EQ(plugin->last_result.reason, "quit-post");
    CHECK_EQ(plugin->last_result.path, out);
    CHECK_EQ(plugin->last_result.error, "dump abandoned at shutdown");
    CHECK_EQ(plugin->stopped.load(), 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    REQUIRE(recorder.async_run());
    REQUIRE(wait_until_ready(recorder));
    CHECK_EQ(plugin->started.load(), 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_FALSE(std::filesystem::exists(out));
    recorder.quit();
    recorder.wait_for_quit();

    CHECK_FALSE(std::filesystem::exists(out));
    CHECK_EQ(plugin->finished.load(), 0);
    CHECK_EQ(plugin->failed.load(), 1);
    CHECK_EQ(plugin->stopped.load(), 2);
  }

  TEST_CASE("a slow on_trigger hook cannot evict the already captured pre window") {
    ScratchDir scratch("slow-trigger-hook");
    const std::string url = "intra://__trigger_test_slow_hook__";
    const std::string out = scratch.path + "/slow-hook.vdb";
    vlink::Publisher<vlink::Bytes> pub(url);

    vlink::TriggerRecorder::Config config;
    config.dump_dir = scratch.path;
    config.default_pre_ms = 500;
    config.default_post_ms = 0;
    config.retention_guard_ms = 0;
    config.enable_compress = false;
    config.whitelist = {url};

    vlink::TriggerRecorder recorder(config, make_raw_sub_factory());
    auto plugin = std::make_shared<PublishingTriggerPlugin>(&pub, PublishingTriggerPlugin::kOnTrigger, 80, 10);
    recorder.bind_trigger_interface(plugin);
    REQUIRE(recorder.async_run());
    REQUIRE(wait_until_ready(recorder));

    if (!pub.wait_for_subscribers(std::chrono::milliseconds(3000))) {
      recorder.quit();
      recorder.wait_for_quit();
      MESSAGE("multicast discovery unavailable; skipping the slow-hook data assertion");
      return;
    }

    const vlink::Bytes payload{0x71, 0x72, 0x73};

    for (int index = 0; index < 50; ++index) {
      CHECK(pub.publish(payload));
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    vlink::TriggerRecorder::TriggerParams params;
    params.reason = "slow-hook";
    params.out_file = out;
    REQUIRE(recorder.dump(params));
    REQUIRE(wait_until_idle(recorder, 6000));
    recorder.quit();
    recorder.wait_for_quit();

    REQUIRE_EQ(plugin->finished.load(), 1);
    CHECK_GT(plugin->last_result.frame_count, 0);
    CHECK_GT(plugin->original_frames.load(), 0);
    CHECK_EQ(plugin->hook_frames.load(), 0);
  }

  TEST_CASE("busy_skip_data ends when the writer closes before post-write hooks") {
    ScratchDir scratch("busy-finished-hook");
    const std::string url = "intra://__trigger_test_busy_hook__";
    vlink::Publisher<vlink::Bytes> pub(url);

    vlink::TriggerRecorder::Config config;
    config.dump_dir = scratch.path;
    config.default_pre_ms = 2000;
    config.default_post_ms = 0;
    config.retention_guard_ms = 0;
    config.enable_compress = false;
    config.busy_skip_data = true;
    config.whitelist = {url};

    vlink::TriggerRecorder recorder(config, make_raw_sub_factory());
    auto plugin = std::make_shared<PublishingTriggerPlugin>(&pub, PublishingTriggerPlugin::kOnFirstFinished, 20, 1);
    recorder.bind_trigger_interface(plugin);
    REQUIRE(recorder.async_run());
    REQUIRE(wait_until_ready(recorder));

    if (!pub.wait_for_subscribers(std::chrono::milliseconds(3000))) {
      recorder.quit();
      recorder.wait_for_quit();
      MESSAGE("multicast discovery unavailable; skipping the busy-hook data assertion");
      return;
    }

    vlink::TriggerRecorder::TriggerParams first;
    first.out_file = scratch.path + "/first.vdb";
    REQUIRE(recorder.dump(first));
    REQUIRE(wait_until_idle(recorder, 5000));

    vlink::TriggerRecorder::TriggerParams second;
    second.out_file = scratch.path + "/second.vdb";
    REQUIRE(recorder.dump(second));
    REQUIRE(wait_until_idle(recorder, 5000));
    recorder.quit();
    recorder.wait_for_quit();

    REQUIRE_EQ(plugin->finished.load(), 2);
    CHECK_GT(plugin->last_result.frame_count, 0);

    auto reader = vlink::BagReader::create(second.out_file);
    REQUIRE(reader != nullptr);
    CHECK_GT(reader->get_info().message_count, 0);
  }

  TEST_CASE("the vcap file type produces an mcap-suffixed dump") {
    ScratchDir scratch("vcap");

    vlink::TriggerRecorder::Config config;
    config.dump_dir = scratch.path;
    config.default_pre_ms = 500;
    config.default_post_ms = 0;
    config.file_type = vlink::TriggerRecorder::kVcap;
    config.whitelist = {"intra://__trigger_test_absent__"};

    vlink::TriggerRecorder recorder(config, make_raw_sub_factory());

    auto plugin = std::make_shared<RecordingTriggerPlugin>();
    recorder.bind_trigger_interface(plugin);

    REQUIRE(recorder.async_run());
    REQUIRE(wait_until_ready(recorder));
    REQUIRE(recorder.dump());
    REQUIRE(wait_until_idle(recorder));
    recorder.quit();
    recorder.wait_for_quit();

    REQUIRE_EQ(plugin->finished.load(), 1);
    CHECK(vlink::Helpers::has_endwith(plugin->last_result.path, ".vcap"));
  }

  TEST_CASE("a name hint with path separators is sanitized into the file name") {
    ScratchDir scratch("name-hint");

    vlink::TriggerRecorder::Config config;
    config.dump_dir = scratch.path;
    config.default_pre_ms = 500;
    config.default_post_ms = 0;
    config.whitelist = {"intra://__trigger_test_absent__"};

    vlink::TriggerRecorder recorder(config, make_raw_sub_factory());

    auto plugin = std::make_shared<RecordingTriggerPlugin>();
    recorder.bind_trigger_interface(plugin);

    REQUIRE(recorder.async_run());
    REQUIRE(wait_until_ready(recorder));

    vlink::TriggerRecorder::TriggerParams params;
    params.name_hint = "front/camera\\event";

    REQUIRE(recorder.dump(params));
    REQUIRE(wait_until_idle(recorder));
    recorder.quit();
    recorder.wait_for_quit();

    REQUIRE_EQ(plugin->finished.load(), 1);
    CHECK(vlink::Helpers::has_endwith(plugin->last_result.path, "front_camera_event.vdb"));
  }

  TEST_CASE("repeated generated names never replace an existing dump") {
    ScratchDir scratch("unique-name");

    vlink::TriggerRecorder::Config config;
    config.dump_dir = scratch.path;
    config.default_pre_ms = 0;
    config.default_post_ms = 0;
    config.whitelist = {"intra://__trigger_test_absent__"};

    vlink::TriggerRecorder recorder(config, make_raw_sub_factory());
    auto plugin = std::make_shared<RecordingTriggerPlugin>();
    recorder.bind_trigger_interface(plugin);
    REQUIRE(recorder.async_run());
    REQUIRE(wait_until_ready(recorder));

    vlink::TriggerRecorder::TriggerParams params;
    params.name_hint = "same-event";

    REQUIRE(recorder.dump(params));
    REQUIRE(wait_until_idle(recorder));
    REQUIRE(recorder.dump(params));
    REQUIRE(wait_until_idle(recorder));

    recorder.quit();
    recorder.wait_for_quit();

    REQUIRE_EQ(plugin->finished_paths.size(), 2u);
    CHECK_NE(plugin->finished_paths[0], plugin->finished_paths[1]);
    CHECK(std::filesystem::exists(plugin->finished_paths[0]));
    CHECK(std::filesystem::exists(plugin->finished_paths[1]));
    CHECK(vlink::Helpers::has_endwith(plugin->finished_paths[0], "same-event.vdb"));
    CHECK(vlink::Helpers::has_endwith(plugin->finished_paths[1], "same-event_1.vdb"));
  }

  TEST_CASE("dump file rotation keeps at most max_dump_file_count files") {
    ScratchDir scratch("rotate");

    vlink::TriggerRecorder::Config config;
    config.dump_dir = scratch.path;
    config.default_pre_ms = 200;
    config.default_post_ms = 0;
    config.max_dump_file_count = 2;
    config.whitelist = {"intra://__trigger_test_absent__"};

    vlink::TriggerRecorder recorder(config, make_raw_sub_factory());

    auto plugin = std::make_shared<RecordingTriggerPlugin>();
    recorder.bind_trigger_interface(plugin);

    REQUIRE(recorder.async_run());
    REQUIRE(wait_until_ready(recorder));

    for (int i = 0; i < 4; ++i) {
      REQUIRE(recorder.dump());
      REQUIRE(wait_until_idle(recorder));
    }

    recorder.quit();
    recorder.wait_for_quit();

    int vdb_files = 0;

    for (const auto& entry : std::filesystem::directory_iterator(scratch.path)) {
      if (entry.is_regular_file() && entry.path().extension() == ".vdb") {
        ++vdb_files;
      }
    }

    CHECK_LE(vdb_files, config.max_dump_file_count);
    CHECK_FALSE(plugin->rotated.empty());
  }

  TEST_CASE("host binding and clearing a bag plugin is safe without a running recorder") {
    vlink::TriggerRecorder::Config config;
    vlink::TriggerRecorder recorder(config, make_raw_sub_factory());

    auto bag_plugin = std::make_shared<CountingBagPlugin>();

    CHECK_NOTHROW(recorder.bind_bag_interface(bag_plugin));
    CHECK_NOTHROW(recorder.clear_bag_interface());
    CHECK_NOTHROW(recorder.bind_bag_interface(nullptr));
  }

  TEST_CASE("published frames flow through a host-bound bag-plugin interface") {
    ScratchDir scratch("data-path");

    const std::string url = "intra://__trigger_test_data__";

    vlink::Publisher<vlink::Bytes> pub(url);

    vlink::TriggerRecorder::Config config;
    config.dump_dir = scratch.path;
    config.default_pre_ms = 4000;
    config.default_post_ms = 0;
    config.enable_compress = false;
    config.whitelist = {url};

    vlink::TriggerRecorder recorder(config, make_raw_sub_factory());

    auto plugin = std::make_shared<RecordingTriggerPlugin>();
    recorder.bind_trigger_interface(plugin);

    auto bag_plugin = std::make_shared<CountingBagPlugin>();
    recorder.bind_bag_interface(bag_plugin);

    REQUIRE(recorder.async_run());
    REQUIRE(wait_until_ready(recorder));

    if (!pub.wait_for_subscribers(std::chrono::milliseconds(3000))) {
      recorder.quit();
      recorder.wait_for_quit();
      MESSAGE("multicast discovery unavailable; skipping the data-path assertions");
      return;
    }

    vlink::Bytes payload{0x11, 0x22, 0x33, 0x44};

    for (int i = 0; i < 20; ++i) {
      CHECK(pub.publish(payload));
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const std::string out = scratch.path + "/data.vdb";

    vlink::TriggerRecorder::TriggerParams params;
    params.reason = "data-path";
    params.out_file = out;

    REQUIRE(recorder.dump(params));
    REQUIRE(wait_until_idle(recorder, 8000));
    recorder.quit();
    recorder.wait_for_quit();

    REQUIRE_EQ(plugin->finished.load(), 1);
    CHECK(plugin->last_result.success);
    CHECK_GT(plugin->last_result.frame_count, 0);
    CHECK_EQ(plugin->last_result.url_count, 1);
    CHECK_GT(plugin->frames.load(), 0);
    CHECK_GT(bag_plugin->writes.load(), 0);
    CHECK_EQ(bag_plugin->reads.load(), 0);

    REQUIRE(std::filesystem::exists(out));

    auto reader = vlink::BagReader::create(out);
    REQUIRE(reader != nullptr);
    CHECK_GT(reader->get_info().message_count, 0);
  }

  TEST_CASE("trigger dumps flush bag-plugin frames in data-time order") {
    ScratchDir scratch("reorder-data-path");

    const std::string url = "intra://__trigger_test_reorder__";
    vlink::Publisher<vlink::Bytes> pub(url);

    vlink::TriggerRecorder::Config config;
    config.dump_dir = scratch.path;
    config.default_pre_ms = 4000;
    config.default_post_ms = 0;
    config.enable_compress = false;
    config.whitelist = {url};

    vlink::TriggerRecorder recorder(config, make_raw_sub_factory());
    recorder.bind_bag_interface(std::make_shared<ReorderBagPlugin>());

    REQUIRE(recorder.async_run());
    REQUIRE(wait_until_ready(recorder));

    if (!pub.wait_for_subscribers(std::chrono::milliseconds(3000))) {
      recorder.quit();
      recorder.wait_for_quit();
      MESSAGE("multicast discovery unavailable; skipping the reorder assertions");
      return;
    }

    REQUIRE(pub.publish(make_data_timestamp(3'000)));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(pub.publish(make_data_timestamp(1'000)));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(pub.publish(make_data_timestamp(2'000)));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const std::string out = scratch.path + "/reordered.vdb";
    vlink::TriggerRecorder::TriggerParams params;
    params.out_file = out;

    REQUIRE(recorder.dump(params));
    REQUIRE(wait_until_idle(recorder, 8000));
    recorder.quit();
    recorder.wait_for_quit();

    auto reader = vlink::BagReader::create(out);
    REQUIRE(reader != nullptr);
    REQUIRE(reader->open_cursor());

    std::vector<int64_t> timestamps;
    vlink::Frame frame;
    while (reader->read_next(frame)) {
      int64_t timestamp = 0;
      std::memcpy(&timestamp, frame.data.data(), sizeof(timestamp));
      timestamps.emplace_back(timestamp);
    }

    REQUIRE_EQ(timestamps.size(), 3u);
    CHECK_EQ(timestamps[0], 1'000);
    CHECK_EQ(timestamps[1], 2'000);
    CHECK_EQ(timestamps[2], 3'000);
  }
}

// NOLINTEND
