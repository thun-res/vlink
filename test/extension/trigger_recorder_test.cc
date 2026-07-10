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
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../common_test.h"
#include "./extension/bag_plugin_interface.h"
#include "./extension/bag_reader.h"
#include "./extension/trigger_plugin_interface.h"
#include "./publisher.h"

namespace {

// A scratch directory unique to each test case, removed on construction so every case starts clean.
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

// Blocks until the recorder has drained its in-flight dump, bounded so a wedged dump fails the test
// rather than hanging the suite.
static bool wait_until_idle(const vlink::TriggerRecorder& recorder, int max_ms = 4000) {
  for (int elapsed = 0; elapsed < max_ms; elapsed += 5) {
    if (!recorder.is_dumping()) {
      return true;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  return !recorder.is_dumping();
}

// Records every life-cycle hook so a test can assert the recorder drove the dump pipeline end to end.
class RecordingTriggerPlugin final : public vlink::TriggerPluginInterface {
 public:
  void on_start() override { ++started; }

  void on_stop() override { ++stopped; }

  void on_trigger(const TriggerContext& context) override {
    ++triggered;
    last_reason = context.reason;
  }

  void on_dump_started(const DumpContext& context) override {
    ++dump_started;
    last_path = context.path;
  }

  void on_frame(const Frame&, const DumpContext&) override { ++frames; }

  void on_dump_finished(const DumpResult& result) override {
    ++finished;
    last_result = result;
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
  std::string last_path;
  std::vector<std::string> rotated;
  DumpResult last_result;
};

// Minimal pass-through bag reorder plugin: counts each frame crossing the read / write path and
// forwards it unchanged, so a dump with data proves the plugin sits inside the write pipeline.
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

}  // namespace

TEST_SUITE("extension-TriggerRecorder") {
  TEST_CASE("overflow policy and file type enums have stable wire values") {
    CHECK_EQ(static_cast<uint8_t>(vlink::TriggerRecorder::kCoverOldest), 0u);
    CHECK_EQ(static_cast<uint8_t>(vlink::TriggerRecorder::kDropNewest), 1u);
    CHECK_EQ(static_cast<uint8_t>(vlink::TriggerRecorder::kVdb), 0u);
    CHECK_EQ(static_cast<uint8_t>(vlink::TriggerRecorder::kVcap), 1u);
  }

  TEST_CASE("config default values match the documented engine defaults") {
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
    CHECK_EQ(config.bag_plugin_major, 2u);
    CHECK_EQ(config.bag_plugin_minor, 0u);
    CHECK_EQ(config.discovery_filter, vlink::DiscoveryViewer::kFilterAvailable);
    CHECK(config.whitelist.empty());
    CHECK(config.blacklist.empty());
    CHECK(config.url_overrides.empty());
    CHECK(config.bag_plugin_lib.empty());
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
    vlink::TriggerRecorder recorder(config);

    CHECK_FALSE(recorder.is_running());
    CHECK_FALSE(recorder.is_dumping());
  }

  TEST_CASE("trigger before start is rejected") {
    vlink::TriggerRecorder::Config config;
    vlink::TriggerRecorder recorder(config);

    CHECK_FALSE(recorder.trigger());
  }

  TEST_CASE("start is idempotent and creates the dump directory") {
    ScratchDir scratch("start");

    vlink::TriggerRecorder::Config config;
    config.dump_dir = scratch.path;
    config.whitelist = {"intra://__trigger_test_absent__"};

    vlink::TriggerRecorder recorder(config);

    REQUIRE(recorder.start());
    CHECK(recorder.is_running());
    CHECK(recorder.start());
    CHECK(recorder.is_running());
    CHECK(std::filesystem::exists(scratch.path));

    recorder.stop();
    CHECK_FALSE(recorder.is_running());
    CHECK_FALSE(recorder.trigger());
  }

  TEST_CASE("stop without a prior start is a harmless no-op") {
    vlink::TriggerRecorder::Config config;
    vlink::TriggerRecorder recorder(config);

    CHECK_NOTHROW(recorder.stop());
    CHECK_FALSE(recorder.is_running());
  }

  TEST_CASE("start falls back to a temporary dump directory when none is configured") {
    vlink::TriggerRecorder::Config config;
    config.dump_dir.clear();
    config.whitelist = {"intra://__trigger_test_absent__"};

    vlink::TriggerRecorder recorder(config);

    REQUIRE(recorder.start());
    CHECK(recorder.is_running());

    recorder.stop();
  }

  TEST_CASE("negative default windows and guard are clamped to zero at start") {
    ScratchDir scratch("clamp");

    vlink::TriggerRecorder::Config config;
    config.dump_dir = scratch.path;
    config.default_pre_ms = -5;
    config.default_post_ms = -5;
    config.retention_guard_ms = -5;
    config.whitelist = {"intra://__trigger_test_absent__"};

    vlink::TriggerRecorder recorder(config);

    auto plugin = std::make_shared<RecordingTriggerPlugin>();
    recorder.bind_trigger_plugin_interface(plugin);

    REQUIRE(recorder.start());
    REQUIRE(recorder.trigger());
    REQUIRE(wait_until_idle(recorder));
    recorder.stop();

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

    vlink::TriggerRecorder recorder(config);

    auto plugin = std::make_shared<RecordingTriggerPlugin>();
    recorder.bind_trigger_plugin_interface(plugin);

    auto bag_plugin = std::make_shared<CountingBagPlugin>();
    recorder.bind_bag_plugin_interface(bag_plugin);

    REQUIRE(recorder.start());
    CHECK_EQ(plugin->started.load(), 1);

    const std::string out = scratch.path + "/manual.vdb";

    vlink::TriggerRecorder::TriggerParams params;
    params.reason = "manual";
    params.out_file = out;

    REQUIRE(recorder.trigger(params));
    REQUIRE(wait_until_idle(recorder));

    recorder.stop();

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

    // An absent-whitelist window never enters the write path, so the bound bag plugin stays untouched.
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

    vlink::TriggerRecorder recorder(config);

    REQUIRE(recorder.start());

    // The post window only arms the dump delay once discovery has delivered the URL; without multicast
    // the in-flight rejection cannot be observed deterministically, so bail out softly.
    if (!pub.wait_for_subscribers(std::chrono::milliseconds(3000))) {
      recorder.stop();
      MESSAGE("multicast discovery unavailable; skipping the serialization assertions");
      return;
    }

    // The post window delays the dump task, so the recorder stays "dumping" long enough to observe the
    // in-flight rejection deterministically.
    REQUIRE(recorder.trigger());
    CHECK(recorder.is_dumping());
    CHECK_FALSE(recorder.trigger());

    REQUIRE(wait_until_idle(recorder));
    recorder.stop();
  }

  TEST_CASE("the vcap file type produces an mcap-suffixed dump") {
    ScratchDir scratch("vcap");

    vlink::TriggerRecorder::Config config;
    config.dump_dir = scratch.path;
    config.default_pre_ms = 500;
    config.default_post_ms = 0;
    config.file_type = vlink::TriggerRecorder::kVcap;
    config.whitelist = {"intra://__trigger_test_absent__"};

    vlink::TriggerRecorder recorder(config);

    auto plugin = std::make_shared<RecordingTriggerPlugin>();
    recorder.bind_trigger_plugin_interface(plugin);

    REQUIRE(recorder.start());
    REQUIRE(recorder.trigger());
    REQUIRE(wait_until_idle(recorder));
    recorder.stop();

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

    vlink::TriggerRecorder recorder(config);

    auto plugin = std::make_shared<RecordingTriggerPlugin>();
    recorder.bind_trigger_plugin_interface(plugin);

    REQUIRE(recorder.start());

    vlink::TriggerRecorder::TriggerParams params;
    params.name_hint = "front/camera\\event";

    REQUIRE(recorder.trigger(params));
    REQUIRE(wait_until_idle(recorder));
    recorder.stop();

    REQUIRE_EQ(plugin->finished.load(), 1);
    CHECK(vlink::Helpers::has_endwith(plugin->last_result.path, "front_camera_event.vdb"));
  }

  TEST_CASE("dump file rotation keeps at most max_dump_file_count files") {
    ScratchDir scratch("rotate");

    vlink::TriggerRecorder::Config config;
    config.dump_dir = scratch.path;
    config.default_pre_ms = 200;
    config.default_post_ms = 0;
    config.max_dump_file_count = 2;
    config.whitelist = {"intra://__trigger_test_absent__"};

    vlink::TriggerRecorder recorder(config);

    auto plugin = std::make_shared<RecordingTriggerPlugin>();
    recorder.bind_trigger_plugin_interface(plugin);

    REQUIRE(recorder.start());

    for (int i = 0; i < 4; ++i) {
      REQUIRE(recorder.trigger());
      REQUIRE(wait_until_idle(recorder));
    }

    recorder.stop();

    int vdb_files = 0;

    for (const auto& entry : std::filesystem::directory_iterator(scratch.path)) {
      if (entry.is_regular_file() && entry.path().extension() == ".vdb") {
        ++vdb_files;
      }
    }

    CHECK_LE(vdb_files, config.max_dump_file_count);
    CHECK_FALSE(plugin->rotated.empty());
  }

  TEST_CASE("rebinding a trigger plugin flushes the plugin it replaces") {
    vlink::TriggerRecorder::Config config;
    vlink::TriggerRecorder recorder(config);

    auto first = std::make_shared<RecordingTriggerPlugin>();
    auto second = std::make_shared<RecordingTriggerPlugin>();

    recorder.bind_trigger_plugin_interface(first);
    CHECK_EQ(first->flushed.load(), 0);

    recorder.bind_trigger_plugin_interface(second);
    CHECK_EQ(first->flushed.load(), 1);

    recorder.clear_trigger_plugin_interface();
    CHECK_EQ(second->flushed.load(), 1);
  }

  TEST_CASE("binding and clearing a bag reorder plugin is safe without a running engine") {
    vlink::TriggerRecorder::Config config;
    vlink::TriggerRecorder recorder(config);

    auto bag_plugin = std::make_shared<CountingBagPlugin>();

    CHECK_NOTHROW(recorder.bind_bag_plugin_interface(bag_plugin));
    CHECK_NOTHROW(recorder.clear_bag_plugin_interface());
    CHECK_NOTHROW(recorder.bind_bag_plugin_interface(nullptr));
  }

  TEST_CASE("published frames flow through the ring into the dump and the bag-plugin write path") {
    ScratchDir scratch("data-path");

    const std::string url = "intra://__trigger_test_data__";

    vlink::Publisher<vlink::Bytes> pub(url);

    vlink::TriggerRecorder::Config config;
    config.dump_dir = scratch.path;
    config.default_pre_ms = 4000;
    config.default_post_ms = 0;
    config.enable_compress = false;
    config.whitelist = {url};

    vlink::TriggerRecorder recorder(config);

    auto plugin = std::make_shared<RecordingTriggerPlugin>();
    recorder.bind_trigger_plugin_interface(plugin);

    auto bag_plugin = std::make_shared<CountingBagPlugin>();
    recorder.bind_bag_plugin_interface(bag_plugin);

    REQUIRE(recorder.start());

    // Discovery rides on real UDP multicast; a sandbox without it cannot exercise the data path, so
    // bail out softly instead of failing the suite there.
    if (!pub.wait_for_subscribers(std::chrono::milliseconds(3000))) {
      recorder.stop();
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

    REQUIRE(recorder.trigger(params));
    REQUIRE(wait_until_idle(recorder, 8000));
    recorder.stop();

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
}

// NOLINTEND
