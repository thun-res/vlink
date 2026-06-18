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

#include "./extension/bag_reader.h"

#include <doctest/doctest.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <future>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../common_test.h"
#include "./extension/bag_processor.h"

namespace {

class StubBagReader : public BagReader {
 public:
  explicit StubBagReader(const std::string& path = (std::filesystem::temp_directory_path() / "stub.vdb").string())
      : BagReader(path, true, false) {}

  ~StubBagReader() override = default;

  void play(const Config&) override {}

  void stop() override {}

  void pause() override {}

  void resume() override {}

  void pause_to_next() override {}

  void jump(int64_t, double, int, bool) override {}

  std::future<bool> check() override {
    return std::async(std::launch::deferred, [] { return true; });
  }

  std::future<bool> reindex() override {
    return std::async(std::launch::deferred, [] { return true; });
  }

  std::future<bool> fix(bool) override {
    return std::async(std::launch::deferred, [] { return true; });
  }

  void tag(const std::string&) override {}

  int64_t get_timestamp() const override { return 0; }

  int64_t get_real_timestamp() const override { return 0; }

  Status get_status() const override { return kStopped; }

  const Info& get_info() const override { return info_; }

  std::vector<SchemaData> detect_schema() override { return {}; }

  bool is_split_mode() const override { return false; }

  int get_split_index() const override { return 0; }

  bool is_jumping() const override { return false; }

  using BagReader::convert_action;
  using BagReader::flush_plugin;
  using BagReader::match_playback_url_filter;
  using BagReader::process_output;
  using BagReader::process_url_metas;
  using BagReader::rebuild_url_meta_lookup;
  using BagReader::rebuild_url_meta_maps;

 private:
  Info info_;
};

class MetaStubBagReader final : public StubBagReader {
 public:
  MetaStubBagReader() {
    std::vector<BagReader::Info::UrlMeta> metas;
    BagReader::Info::UrlMeta meta;
    meta.url = "dds://topic";
    meta.ser_type = "demo.Proto";
    meta.schema_type = SchemaType::kProtobuf;
    metas.emplace_back(std::move(meta));
    rebuild_url_meta_lookup(metas);
  }
};

class RemapPlugin final : public BagPluginInterface {
 public:
  VersionInfo get_version_info() const override { return {"Remap", "1.0.0", "", "", ""}; }

  bool convert_url_meta(std::string& url, std::string& ser_type, SchemaType& schema_type) override {
    (void)ser_type;
    (void)schema_type;

    if (url == "intra://drop") {
      return false;
    }

    if (url == "intra://old") {
      url = "intra://new";
    }

    return true;
  }

  void on_read(const Frame& frame) override { do_callback(frame); }
};

class ReorderReadPlugin final : public BagPluginInterface {
 public:
  explicit ReorderReadPlugin(int64_t min_cache_time) : processor_(make_config(min_cache_time)) {
    processor_.register_output_callback([this](const Frame& frame) { do_callback(frame); });
  }

  ~ReorderReadPlugin() override = default;

  VersionInfo get_version_info() const override { return {"ReorderRead", "1.0.0", "", "", ""}; }

  void on_read(const Frame& frame) override {
    int64_t data_timestamp = 0;
    std::memcpy(&data_timestamp, frame.data.data(), sizeof(int64_t));

    Frame out = frame;
    out.timestamp = data_timestamp;
    processor_.push(data_timestamp, out);
  }

  void flush() override { processor_.flush(); }

 private:
  static BagProcessor::Config make_config(int64_t min_cache_time) {
    BagProcessor::Config config;
    config.min_cache_time = min_cache_time;
    return config;
  }

  BagProcessor processor_;
};

Frame read_frame(int64_t timestamp, const std::string& url, ActionType action_type, const Bytes& data) {
  Frame frame;
  frame.timestamp = timestamp;
  frame.url = url;
  frame.action_type = action_type;
  frame.data = data;

  return frame;
}

Bytes make_timestamp_payload(int64_t timestamp) {
  Bytes payload = Bytes::create(sizeof(int64_t));
  std::memcpy(payload.data(), &timestamp, sizeof(int64_t));
  return payload;
}

class CursorStubBagReader final : public StubBagReader {
 public:
  explicit CursorStubBagReader(std::vector<Frame> frames, bool open_succeeds = true)
      : frames_(std::move(frames)), open_succeeds_(open_succeeds) {}

  int open_count{0};

 protected:
  bool do_open_cursor(const Config& config) override {
    ++open_count;
    cursor_index_ = 0;
    begin_us_ = config.begin_time > 0 ? config.begin_time * 1000 : 0;

    return open_succeeds_;
  }

  bool do_read_next(Frame& out, bool& is_error) override {
    is_error = false;

    while (cursor_index_ < frames_.size()) {
      const Frame& frame = frames_[cursor_index_++];

      if (begin_us_ > 0 && frame.timestamp < begin_us_) {
        continue;
      }

      out = frame;

      return true;
    }

    return false;
  }

 private:
  std::vector<Frame> frames_;
  bool open_succeeds_;
  size_t cursor_index_{0};
  int64_t begin_us_{0};
};

}  // namespace

TEST_SUITE("extension-BagReader") {
  TEST_CASE("stub construction yields stopped/zero/false state") {
    StubBagReader reader;

    CHECK_EQ(reader.get_status(), BagReader::kStopped);
    CHECK_EQ(reader.get_timestamp(), 0);
    CHECK_EQ(reader.get_real_timestamp(), 0);
    CHECK_FALSE(reader.is_split_mode());
    CHECK_EQ(reader.get_split_index(), 0);
    CHECK_FALSE(reader.is_jumping());
    CHECK(reader.detect_schema().empty());
    CHECK(reader.get_ser_type("any").empty());
    CHECK_EQ(reader.get_schema_type("any"), SchemaType::kUnknown);
  }

  TEST_CASE("base reader without cursor support yields a failed stream") {
    StubBagReader reader;
    Frame frame;

    CHECK_FALSE(reader.open_cursor());
    CHECK(reader.fail());
    CHECK_FALSE(static_cast<bool>(reader));
    CHECK_FALSE(static_cast<bool>(reader >> frame));
  }

  TEST_CASE("operator>> walks every frame then reports eof") {
    Bytes data = Bytes::create(4u);
    CursorStubBagReader reader({read_frame(0, "dds://a", ActionType::kPublish, data),
                                read_frame(1000, "dds://b", ActionType::kPublish, data),
                                read_frame(2000, "dds://c", ActionType::kPublish, data)});

    std::vector<std::string> urls;
    Frame frame;

    while (reader >> frame) {
      urls.push_back(frame.url);
    }

    CHECK_EQ(reader.open_count, 1);
    CHECK_EQ(urls.size(), 3);
    CHECK_EQ(urls.front(), "dds://a");
    CHECK_EQ(urls.back(), "dds://c");
    CHECK(reader.eof());
    CHECK_FALSE(reader.fail());
    CHECK_FALSE(static_cast<bool>(reader));
  }

  TEST_CASE("open_cursor applies the begin_time window and rewinds") {
    Bytes data = Bytes::create(4u);
    CursorStubBagReader reader({read_frame(0, "dds://a", ActionType::kPublish, data),
                                read_frame(5'000, "dds://b", ActionType::kPublish, data),
                                read_frame(10'000, "dds://c", ActionType::kPublish, data)});

    BagReader::Config cfg;
    cfg.begin_time = 5;

    CHECK(reader.open_cursor(cfg));

    int count = 0;
    Frame frame;

    while (reader >> frame) {
      ++count;
    }

    CHECK_EQ(count, 2);

    CHECK(reader.open_cursor());
    CHECK_FALSE(reader.eof());
    CHECK_FALSE(reader.fail());

    count = 0;

    while (reader >> frame) {
      ++count;
    }

    CHECK_EQ(count, 3);
    CHECK_EQ(reader.open_count, 2);
  }

  TEST_CASE("open failure leaves the stream in a failed state") {
    CursorStubBagReader reader({}, /*open_succeeds=*/false);

    CHECK_FALSE(reader.open_cursor());
    CHECK(reader.fail());
    CHECK_FALSE(reader.eof());

    Frame frame;
    CHECK_FALSE(reader.read_next(frame));
  }

  TEST_CASE("output callback is invoked by process_output") {
    StubBagReader reader;
    std::atomic_bool called{false};
    std::string received_url;

    reader.register_output_callback([&](const Frame& frame) {
      called = true;
      received_url = frame.url;
    });

    Bytes data;
    Frame frame = read_frame(100, "dds://test", ActionType::kPublish, data);
    reader.process_output(frame);

    CHECK(called.load());
    CHECK_EQ(received_url, "dds://test");
  }

  TEST_CASE("process_output without callback does not crash") {
    StubBagReader reader;
    Bytes data;
    Frame frame = read_frame(0, "dds://test", ActionType::kPublish, data);
    reader.process_output(frame);
  }

  TEST_CASE("process_output populates frame ser_type/schema_type from reader metadata") {
    MetaStubBagReader reader;
    std::string observed_ser;
    SchemaType observed_schema = SchemaType::kUnknown;

    reader.register_output_callback([&](const Frame& frame) {
      observed_ser = frame.ser_type;
      observed_schema = frame.schema_type;
    });

    Bytes data = Bytes::create(4u);
    Frame frame = read_frame(1, "dds://topic", ActionType::kPublish, data);
    reader.process_output(frame);

    CHECK_EQ(observed_ser, "demo.Proto");
    CHECK_EQ(observed_schema, SchemaType::kProtobuf);
  }

  TEST_CASE("flush_plugin drains an async read plugin's buffered tail frames") {
    StubBagReader reader;
    reader.bind_plugin_interface(std::make_shared<ReorderReadPlugin>(60'000));

    std::vector<int64_t> observed_timestamps;
    reader.register_output_callback([&](const Frame& frame) { observed_timestamps.push_back(frame.timestamp); });

    Frame frame_c = read_frame(1, "dds://c", ActionType::kPublish, make_timestamp_payload(50'000'001));
    Frame frame_a = read_frame(2, "dds://a", ActionType::kPublish, make_timestamp_payload(1));
    Frame frame_b = read_frame(3, "dds://b", ActionType::kPublish, make_timestamp_payload(20'000'000));
    reader.process_output(frame_c);
    reader.process_output(frame_a);
    reader.process_output(frame_b);

    CHECK(observed_timestamps.empty());

    reader.flush_plugin();

    REQUIRE_EQ(observed_timestamps.size(), 3u);
    CHECK_EQ(observed_timestamps[0], 1);
    CHECK_EQ(observed_timestamps[1], 20'000'000);
    CHECK_EQ(observed_timestamps[2], 50'000'001);
  }

  TEST_CASE("registering status/ready/finish callbacks does not crash") {
    StubBagReader reader;
    reader.register_status_callback([](BagReader::Status) {});
    reader.register_ready_callback([] {});
    reader.register_finish_callback([](bool) {});
  }

  TEST_CASE("process_url_metas without plugin leaves metas unchanged") {
    StubBagReader reader;
    std::vector<BagReader::Info::UrlMeta> metas;

    BagReader::Info::UrlMeta m;
    m.url = "dds://topic";
    m.ser_type = "proto";
    metas.push_back(m);

    reader.process_url_metas(metas);

    REQUIRE_EQ(metas.size(), 1u);
    CHECK_EQ(metas[0].url, "dds://topic");
  }

  TEST_CASE("plugin remaps and excludes urls during process_url_metas") {
    StubBagReader reader;
    auto plugin = std::make_shared<RemapPlugin>();
    reader.bind_plugin_interface(plugin);

    std::vector<BagReader::Info::UrlMeta> metas;
    BagReader::Info::UrlMeta a;
    a.url = "intra://old";
    metas.push_back(a);

    BagReader::Info::UrlMeta b;
    b.url = "intra://drop";
    metas.push_back(b);

    reader.process_url_metas(metas);

    REQUIRE_EQ(metas.size(), 1u);
    CHECK_EQ(metas[0].url, "intra://new");
  }

  TEST_CASE("process_output forwards remapped url derived from process_url_metas") {
    StubBagReader reader;
    auto plugin = std::make_shared<RemapPlugin>();
    reader.bind_plugin_interface(plugin);

    std::vector<BagReader::Info::UrlMeta> metas;
    BagReader::Info::UrlMeta m;
    m.url = "intra://old";
    metas.push_back(m);
    reader.process_url_metas(metas);

    std::string observed_url;
    int call_count = 0;

    reader.register_output_callback([&](const Frame& frame) {
      observed_url = frame.url;
      ++call_count;
    });

    Bytes data = Bytes::create(1u);
    Frame frame = read_frame(1, "intra://old", ActionType::kPublish, data);
    reader.process_output(frame);

    CHECK_EQ(call_count, 1);
    CHECK_EQ(observed_url, "intra://new");
  }

  TEST_CASE("match_playback_url_filter uses remapped url for filter matching") {
    StubBagReader reader;
    auto plugin = std::make_shared<RemapPlugin>();
    reader.bind_plugin_interface(plugin);

    std::vector<BagReader::Info::UrlMeta> metas;
    BagReader::Info::UrlMeta m;
    m.url = "intra://old";
    metas.push_back(m);
    reader.process_url_metas(metas);

    std::unordered_set<std::string> filter_urls;
    filter_urls.emplace("intra://new");

    CHECK(reader.match_playback_url_filter("intra://old", filter_urls));
    CHECK_FALSE(reader.match_playback_url_filter("intra://drop", filter_urls));
  }

  TEST_CASE("rebinding plugin disconnects the old plugin read callback") {
    StubBagReader reader;
    auto old_plugin = std::make_shared<RemapPlugin>();
    auto new_plugin = std::make_shared<RemapPlugin>();

    int call_count = 0;
    reader.register_output_callback([&](const Frame&) { ++call_count; });

    reader.bind_plugin_interface(old_plugin);
    reader.bind_plugin_interface(new_plugin);

    Bytes data = Bytes::create(1u);
    old_plugin->on_read(read_frame(1, "intra://old", ActionType::kPublish, data));
    CHECK_EQ(call_count, 0);

    new_plugin->on_read(read_frame(2, "intra://old", ActionType::kPublish, data));
    CHECK_EQ(call_count, 1);
  }

  TEST_CASE("convert_action maps known tokens to action types") {
    CHECK_EQ(StubBagReader::convert_action("Pub"), ActionType::kPublish);
    CHECK_EQ(StubBagReader::convert_action("Sub"), ActionType::kSubscribe);
    CHECK_EQ(StubBagReader::convert_action("C/Req"), ActionType::kClientRequest);
    CHECK_EQ(StubBagReader::convert_action("C/Resp"), ActionType::kClientResponse);
    CHECK_EQ(StubBagReader::convert_action("S/Req"), ActionType::kServerRequest);
    CHECK_EQ(StubBagReader::convert_action("S/Resp"), ActionType::kServerResponse);
    CHECK_EQ(StubBagReader::convert_action("Set"), ActionType::kSet);
    CHECK_EQ(StubBagReader::convert_action("Get"), ActionType::kGet);
  }

  TEST_CASE("convert_action returns kUnknownAction for unknown tokens") {
    CHECK_EQ(StubBagReader::convert_action("XYZ"), ActionType::kUnknownAction);
    CHECK_EQ(StubBagReader::convert_action(""), ActionType::kUnknownAction);
    CHECK_EQ(StubBagReader::convert_action("Unknown"), ActionType::kUnknownAction);
  }

  TEST_CASE("rebuild_url_meta_maps preserves known schema type over unknown duplicate") {
    std::vector<BagReader::Info::UrlMeta> metas;

    BagReader::Info::UrlMeta known;
    known.url = "intra://test";
    known.ser_type = "demo.Type";
    known.schema_type = SchemaType::kProtobuf;
    metas.emplace_back(known);

    BagReader::Info::UrlMeta unknown;
    unknown.url = "intra://test";
    unknown.schema_type = SchemaType::kUnknown;
    metas.emplace_back(unknown);

    std::unordered_map<std::string, std::string> ser_map;
    std::unordered_map<std::string, SchemaType> schema_type_map;
    StubBagReader::rebuild_url_meta_maps(metas, ser_map, schema_type_map);

    REQUIRE_EQ(ser_map.count("intra://test"), 1u);
    REQUIRE_EQ(schema_type_map.count("intra://test"), 1u);
    CHECK_EQ(ser_map["intra://test"], "demo.Type");
    CHECK_EQ(schema_type_map["intra://test"], SchemaType::kProtobuf);
  }

  TEST_CASE("create returns nullptr for unsupported file extension") {
    auto reader = BagReader::create("/tmp/unsupported.xyz");

    CHECK(reader == nullptr);
  }
}

// NOLINTEND
