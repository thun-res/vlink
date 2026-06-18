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

#include "./extension/bag_writer.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "../common_test.h"
#include "./extension/bag_processor.h"

class StubBagWriter : public BagWriter {
 public:
  explicit StubBagWriter(const std::string& path = (std::filesystem::temp_directory_path() / "stub.vdb").string(),
                         const BagWriter::Config& config = {})
      : BagWriter(path, config) {}

  ~StubBagWriter() override = default;

  using BagWriter::detach_plugin;

  void register_split_callback(SplitCallback&&, bool) override {}

  void register_schema_callback(SchemaCallback&&) override {}

  bool push_schema(const SchemaData&, bool) override {
    ++schema_push_count;
    return true;
  }

  int64_t record(const Frame& frame, bool) override {
    std::lock_guard lock(record_mtx);

    ++record_count;
    last_url = frame.url;
    last_ser_type = frame.ser_type;
    last_schema_type = frame.schema_type;
    last_action_type = frame.action_type;
    last_size = frame.data.size();
    last_timestamp = frame.timestamp;
    recorded_timestamps.push_back(frame.timestamp);

    record_cv.notify_all();

    return frame.timestamp;
  }

  int64_t get_record_timestamp() const override { return 4242; }

  std::mutex record_mtx;
  ConditionVariable record_cv;
  std::vector<int64_t> recorded_timestamps;
  int record_count{0};
  int schema_push_count{0};
  std::string last_url;
  std::string last_ser_type;
  SchemaType last_schema_type{SchemaType::kUnknown};
  ActionType last_action_type{ActionType::kUnknownAction};
  size_t last_size{0};
  int64_t last_timestamp{0};

  bool is_dumping() const override { return false; }

  bool is_split_mode() const override { return false; }

  int get_split_index() const override { return 0; }

  void set_url_loss(const std::string&, double) override {}

  using BagWriter::convert_action;
  using BagWriter::convert_recorded_url;
  using BagWriter::get_default_app_name;
  using BagWriter::get_default_tag_name;
  using BagWriter::get_default_timezone_diff;
  using BagWriter::get_format_date;
  using BagWriter::get_schema_interface;
  using BagWriter::get_url_meta;
  using BagWriter::recorded_urls_for_origin;
  using BagWriter::recover_recorded_url;
};

class RewriteWritePlugin final : public BagPluginInterface {
 public:
  VersionInfo get_version_info() const override { return {"RewriteWrite", "1.0.0", "", "", ""}; }

  void on_write(const Frame& frame) override {
    Frame out;
    out.timestamp = frame.timestamp;
    out.url = "dds://jpeg";
    out.ser_type = "jpeg";
    out.schema_type = SchemaType::kProtobuf;
    out.action_type = frame.action_type;
    out.data = frame.data;

    do_callback(out);
  }
};

class DropWritePlugin final : public BagPluginInterface {
 public:
  VersionInfo get_version_info() const override { return {"DropWrite", "1.0.0", "", "", ""}; }

  void on_write(const Frame&) override {}
};

class EmptyUrlWritePlugin final : public BagPluginInterface {
 public:
  VersionInfo get_version_info() const override { return {"EmptyUrlWrite", "1.0.0", "", "", ""}; }

  void on_write(const Frame& frame) override {
    Frame out = frame;
    out.url.clear();
    do_callback(out);
  }
};

class TranscodeWritePlugin final : public BagPluginInterface {
 public:
  VersionInfo get_version_info() const override { return {"Transcode", "1.0.0", "", "", ""}; }

  void on_write(const Frame& frame) override {
    Frame out;
    out.timestamp = frame.timestamp;
    out.url = frame.url;
    out.ser_type = "jpeg";
    out.schema_type = frame.schema_type;
    out.action_type = frame.action_type;
    out.data = Bytes::create(99u);

    do_callback(out);
  }
};

class FanOutWritePlugin final : public BagPluginInterface {
 public:
  VersionInfo get_version_info() const override { return {"FanOutWrite", "1.0.0", "", "", ""}; }

  void on_write(const Frame& frame) override {
    Frame first = frame;
    first.url = "dds://jpeg";
    do_callback(first);

    Frame second = frame;
    second.url = "dds://thumb";
    do_callback(second);
  }
};

class ReorderWritePlugin final : public BagPluginInterface {
 public:
  explicit ReorderWritePlugin(int64_t min_cache_time = 1) : processor_(make_config(min_cache_time)) {
    processor_.register_output_callback([this](const Frame& frame) { do_callback(frame); });
  }

  ~ReorderWritePlugin() override = default;

  VersionInfo get_version_info() const override { return {"Reorder", "1.0.0", "", "", ""}; }

  void on_write(const Frame& frame) override {
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

Frame write_frame(const std::string& url, const std::string& ser_type, SchemaType schema_type, ActionType action_type,
                  const Bytes& data, int64_t timestamp = 0) {
  Frame frame;
  frame.timestamp = timestamp;
  frame.url = url;
  frame.ser_type = ser_type;
  frame.schema_type = schema_type;
  frame.action_type = action_type;
  frame.data = data;

  return frame;
}

class FailingBagWriter final : public StubBagWriter {
 public:
  int64_t record(const Frame& frame, bool immediate) override {
    (void)StubBagWriter::record(frame, immediate);
    return -1;
  }
};

TEST_SUITE("extension-BagWriter") {
  TEST_CASE("bind_plugin_interface marks the plugin as write direction") {
    StubBagWriter writer;
    auto plugin = std::make_shared<RewriteWritePlugin>();
    writer.bind_plugin_interface(plugin);
    CHECK_EQ(plugin->get_direction(), BagPluginInterface::Direction::kWrite);
  }

  TEST_CASE("on_write re-emits rewritten url/ser/schema via do_callback before record") {
    StubBagWriter writer;
    writer.bind_plugin_interface(std::make_shared<RewriteWritePlugin>());

    Bytes data = Bytes::create(8u);
    writer.push(write_frame("dds://raw", "raw", SchemaType::kUnknown, ActionType::kPublish, data, 100));

    CHECK_EQ(writer.record_count, 1);
    CHECK_EQ(writer.last_url, "dds://jpeg");
    CHECK_EQ(writer.last_ser_type, "jpeg");
    CHECK_EQ(writer.last_schema_type, SchemaType::kProtobuf);
  }

  TEST_CASE("operator<< records a frame like push and chains") {
    StubBagWriter writer;

    Bytes data = Bytes::create(8u);
    BagWriter& chained =
        (writer << write_frame("dds://raw", "raw", SchemaType::kUnknown, ActionType::kPublish, data, 100));

    CHECK_EQ(&chained, &writer);
    CHECK_EQ(writer.record_count, 1);
    CHECK_EQ(writer.last_url, "dds://raw");
    CHECK_EQ(writer.last_timestamp, 100);
  }

  TEST_CASE("operator<< chains frame and schema to push and push_schema") {
    StubBagWriter writer;

    Bytes data_a = Bytes::create(4u);
    Bytes data_b = Bytes::create(8u);
    SchemaData schema;

    writer << schema << write_frame("dds://a", "raw", SchemaType::kUnknown, ActionType::kPublish, data_a, 1)
           << write_frame("dds://b", "raw", SchemaType::kUnknown, ActionType::kPublish, data_b, 2);

    CHECK_EQ(writer.schema_push_count, 1);
    CHECK_EQ(writer.record_count, 2);
    CHECK_EQ(writer.last_url, "dds://b");
  }

  TEST_CASE("operator<< latches fail state on a rejected push and clear() resets it") {
    StubBagWriter writer;

    CHECK(static_cast<bool>(writer));
    CHECK_FALSE(writer.fail());

    Bytes data = Bytes::create(4u);
    writer << write_frame("", "raw", SchemaType::kUnknown, ActionType::kPublish, data, 100);

    CHECK(writer.fail());
    CHECK_FALSE(static_cast<bool>(writer));
    CHECK_EQ(writer.record_count, 0);

    writer.clear();
    CHECK_FALSE(writer.fail());
    CHECK(static_cast<bool>(writer));

    writer << write_frame("dds://ok", "raw", SchemaType::kUnknown, ActionType::kPublish, data, 200);
    CHECK_FALSE(writer.fail());
    CHECK_EQ(writer.record_count, 1);
  }

  TEST_CASE("on_write that does not emit drops the frame") {
    StubBagWriter writer;
    writer.bind_plugin_interface(std::make_shared<DropWritePlugin>());

    Bytes data = Bytes::create(4u);
    int64_t result = writer.push(write_frame("dds://x", "raw", SchemaType::kUnknown, ActionType::kPublish, data, -1));

    CHECK_EQ(result, 4242);
    CHECK_EQ(writer.record_count, 0);
  }

  TEST_CASE("plugin path propagates synchronous record failure") {
    FailingBagWriter writer;
    writer.bind_plugin_interface(std::make_shared<RewriteWritePlugin>());

    Bytes data = Bytes::create(4u);
    int64_t result = writer.push(write_frame("dds://x", "raw", SchemaType::kUnknown, ActionType::kPublish, data, 100));

    CHECK_LT(result, 0);
    CHECK_EQ(writer.record_count, 1);
  }

  TEST_CASE("plugin path treats synchronously emitted empty url as failure") {
    StubBagWriter writer;
    writer.bind_plugin_interface(std::make_shared<EmptyUrlWritePlugin>());

    Bytes data = Bytes::create(4u);
    int64_t result = writer.push(write_frame("dds://x", "raw", SchemaType::kUnknown, ActionType::kPublish, data, 100));

    CHECK_LT(result, 0);
    CHECK_EQ(writer.record_count, 0);
  }

  TEST_CASE("on_write re-emits a replacement payload via do_callback") {
    StubBagWriter writer;
    writer.bind_plugin_interface(std::make_shared<TranscodeWritePlugin>());

    Bytes data = Bytes::create(8u);
    writer.push(write_frame("dds://raw", "raw", SchemaType::kUnknown, ActionType::kPublish, data, 100));

    CHECK_EQ(writer.record_count, 1);
    CHECK_EQ(writer.last_ser_type, "jpeg");
    CHECK_EQ(writer.last_size, 99u);
  }

  TEST_CASE("synchronous write plugin url rewrite is tracked for loss-metadata alignment") {
    StubBagWriter writer;
    writer.bind_plugin_interface(std::make_shared<RewriteWritePlugin>());

    Bytes data = Bytes::create(8u);
    writer.push(write_frame("dds://raw", "raw", SchemaType::kUnknown, ActionType::kPublish, data, 100));

    CHECK_EQ(writer.convert_recorded_url("dds://raw"), "dds://jpeg");
    CHECK_EQ(writer.recover_recorded_url("dds://jpeg"), "dds://raw");
    CHECK_EQ(writer.convert_recorded_url("dds://unmapped"), "dds://unmapped");
  }

  TEST_CASE("plugin url remap survives unbind for close-time metadata") {
    StubBagWriter writer;
    writer.bind_plugin_interface(std::make_shared<RewriteWritePlugin>());

    Bytes data = Bytes::create(8u);
    writer.push(write_frame("dds://raw", "raw", SchemaType::kUnknown, ActionType::kPublish, data, 100));
    writer.bind_plugin_interface(nullptr);

    CHECK_EQ(writer.convert_recorded_url("dds://raw"), "dds://jpeg");
    CHECK_EQ(writer.recover_recorded_url("dds://jpeg"), "dds://raw");
  }

  TEST_CASE("fan-out plugin tracks every recorded url for one origin") {
    StubBagWriter writer;
    writer.bind_plugin_interface(std::make_shared<FanOutWritePlugin>());

    Bytes data = Bytes::create(8u);
    writer.push(write_frame("dds://raw", "raw", SchemaType::kUnknown, ActionType::kPublish, data, 100));

    auto recorded_urls = writer.recorded_urls_for_origin("dds://raw");
    std::sort(recorded_urls.begin(), recorded_urls.end());

    REQUIRE_EQ(recorded_urls.size(), 3u);
    CHECK_EQ(recorded_urls[0], "dds://jpeg");
    CHECK_EQ(recorded_urls[1], "dds://raw");
    CHECK_EQ(recorded_urls[2], "dds://thumb");
  }

  TEST_CASE("write plugin reorders by data-plane time (not arrival order) before record") {
    StubBagWriter writer;
    writer.bind_plugin_interface(std::make_shared<ReorderWritePlugin>());

    auto make_payload = [](int64_t data_timestamp) {
      Bytes payload = Bytes::create(sizeof(int64_t));
      std::memcpy(payload.data(), &data_timestamp, sizeof(int64_t));
      return payload;
    };

    writer.push(write_frame("dds://c", "raw", SchemaType::kUnknown, ActionType::kPublish, make_payload(50'000'001), 1));
    writer.push(write_frame("dds://a", "raw", SchemaType::kUnknown, ActionType::kPublish, make_payload(1), 2));
    writer.push(write_frame("dds://b", "raw", SchemaType::kUnknown, ActionType::kPublish, make_payload(20'000'000), 3));

    {
      std::unique_lock lock(writer.record_mtx);
      REQUIRE(writer.record_cv.wait_for(lock, 2s, [&] { return writer.recorded_timestamps.size() >= 3u; }));
      CHECK_EQ(writer.recorded_timestamps[0], 1);
      CHECK_EQ(writer.recorded_timestamps[1], 20'000'000);
      CHECK_EQ(writer.recorded_timestamps[2], 50'000'001);
    }

    writer.bind_plugin_interface(nullptr);
  }

  TEST_CASE("teardown flushes an async plugin's buffered tail frames instead of dropping them") {
    StubBagWriter writer;
    // 60s reorder window: pushed frames stay buffered in the plugin, never auto-drained during the test.
    writer.bind_plugin_interface(std::make_shared<ReorderWritePlugin>(60'000));

    auto make_payload = [](int64_t data_timestamp) {
      Bytes payload = Bytes::create(sizeof(int64_t));
      std::memcpy(payload.data(), &data_timestamp, sizeof(int64_t));
      return payload;
    };

    writer.push(write_frame("dds://c", "raw", SchemaType::kUnknown, ActionType::kPublish, make_payload(50'000'001), 1));
    writer.push(write_frame("dds://a", "raw", SchemaType::kUnknown, ActionType::kPublish, make_payload(1), 2));
    writer.push(write_frame("dds://b", "raw", SchemaType::kUnknown, ActionType::kPublish, make_payload(20'000'000), 3));

    {
      std::lock_guard lock(writer.record_mtx);
      CHECK_EQ(writer.recorded_timestamps.size(), 0u);  // still buffered, not yet drained
    }

    writer.detach_plugin();  // the contract every concrete writer dtor honours

    {
      std::lock_guard lock(writer.record_mtx);
      REQUIRE_EQ(writer.recorded_timestamps.size(), 3u);
      CHECK_EQ(writer.recorded_timestamps[0], 1);
      CHECK_EQ(writer.recorded_timestamps[1], 20'000'000);
      CHECK_EQ(writer.recorded_timestamps[2], 50'000'001);
    }
  }

  TEST_CASE("unbinding an async plugin flushes its buffered tail frames instead of dropping them") {
    StubBagWriter writer;
    writer.bind_plugin_interface(std::make_shared<ReorderWritePlugin>(60'000));

    auto make_payload = [](int64_t data_timestamp) {
      Bytes payload = Bytes::create(sizeof(int64_t));
      std::memcpy(payload.data(), &data_timestamp, sizeof(int64_t));
      return payload;
    };

    writer.push(write_frame("dds://a", "raw", SchemaType::kUnknown, ActionType::kPublish, make_payload(10'000'000), 1));
    writer.push(write_frame("dds://b", "raw", SchemaType::kUnknown, ActionType::kPublish, make_payload(5'000'000), 2));

    {
      std::lock_guard lock(writer.record_mtx);
      CHECK_EQ(writer.recorded_timestamps.size(), 0u);  // still buffered
    }

    writer.bind_plugin_interface(nullptr);  // unbind must flush the buffered tail before detaching

    {
      std::lock_guard lock(writer.record_mtx);
      REQUIRE_EQ(writer.recorded_timestamps.size(), 2u);
      CHECK_EQ(writer.recorded_timestamps[0], 5'000'000);
      CHECK_EQ(writer.recorded_timestamps[1], 10'000'000);
    }
  }

  TEST_CASE("push without plugin records the frame unchanged") {
    StubBagWriter writer;

    Bytes data = Bytes::create(5u);
    writer.push(write_frame("dds://x", "raw", SchemaType::kUnknown, ActionType::kPublish, data, 100));

    CHECK_EQ(writer.record_count, 1);
    CHECK_EQ(writer.last_url, "dds://x");
    CHECK_EQ(writer.last_ser_type, "raw");
  }

  TEST_CASE("stub construction yields false dumping/split state") {
    StubBagWriter writer;
    CHECK_FALSE(writer.is_dumping());
    CHECK_FALSE(writer.is_split_mode());
    CHECK_EQ(writer.get_split_index(), 0);
  }

  TEST_CASE("convert_action maps all known action types to strings") {
    CHECK_EQ(StubBagWriter::convert_action(ActionType::kPublish), "Pub");
    CHECK_EQ(StubBagWriter::convert_action(ActionType::kSubscribe), "Sub");
    CHECK_EQ(StubBagWriter::convert_action(ActionType::kClientRequest), "C/Req");
    CHECK_EQ(StubBagWriter::convert_action(ActionType::kClientResponse), "C/Resp");
    CHECK_EQ(StubBagWriter::convert_action(ActionType::kServerRequest), "S/Req");
    CHECK_EQ(StubBagWriter::convert_action(ActionType::kServerResponse), "S/Resp");
    CHECK_EQ(StubBagWriter::convert_action(ActionType::kSet), "Set");
    CHECK_EQ(StubBagWriter::convert_action(ActionType::kGet), "Get");
    CHECK_EQ(StubBagWriter::convert_action(ActionType::kUnknownAction), "Unknown");
  }

  TEST_CASE("create returns nullptr for unsupported file extension") {
    auto writer = BagWriter::create((std::filesystem::temp_directory_path() / "unsupported.xyz").string());
    CHECK_EQ(writer, nullptr);
  }

  TEST_CASE("filter_get returns nullptr for unsupported file extension") {
    auto writer = BagWriter::filter_get((std::filesystem::temp_directory_path() / "unsupported.xyz").string());
    CHECK_EQ(writer, nullptr);
  }

  TEST_CASE("get_url_meta assigns distinct positive indices for new url and ser") {
    StubBagWriter writer;
    int url_idx = -1;
    int ser_idx = -1;
    writer.get_url_meta("dds://topic1", "protobuf", url_idx, ser_idx);
    CHECK_GT(url_idx, 0);
    CHECK_GT(ser_idx, 0);
  }

  TEST_CASE("get_url_meta returns same indices for the same url and ser") {
    StubBagWriter writer;
    int u1 = -1;
    int s1 = -1;
    int u2 = -1;
    int s2 = -1;
    writer.get_url_meta("dds://topic1", "protobuf", u1, s1);
    writer.get_url_meta("dds://topic1", "protobuf", u2, s2);
    CHECK_EQ(u1, u2);
    CHECK_EQ(s1, s2);
  }

  TEST_CASE("get_url_meta assigns distinct url indices for different urls") {
    StubBagWriter writer;
    int u1 = -1;
    int s1 = -1;
    int u2 = -1;
    int s2 = -1;
    writer.get_url_meta("dds://topic1", "proto", u1, s1);
    writer.get_url_meta("dds://topic2", "proto", u2, s2);
    CHECK_NE(u1, u2);
    CHECK_EQ(s1, s2);
  }

  TEST_CASE("get_url_meta assigns distinct ser indices for different ser types") {
    StubBagWriter writer;
    int u1 = -1;
    int s1 = -1;
    int u2 = -1;
    int s2 = -1;
    writer.get_url_meta("dds://topic", "proto", u1, s1);
    writer.get_url_meta("dds://topic", "raw", u2, s2);
    CHECK_EQ(u1, u2);
    CHECK_NE(s1, s2);
  }

  TEST_CASE("reverse get_url_meta lookup returns original strings") {
    StubBagWriter writer;
    int url_idx = -1;
    int ser_idx = -1;
    writer.get_url_meta("intra://test_topic", "flatbuf", url_idx, ser_idx);

    std::string url_out;
    std::string ser_out;
    writer.get_url_meta(url_idx, ser_idx, url_out, ser_out);

    CHECK_EQ(url_out, "intra://test_topic");
    CHECK_EQ(ser_out, "flatbuf");
  }

  TEST_CASE("reverse get_url_meta lookup with invalid index returns empty strings") {
    StubBagWriter writer;
    std::string url_out;
    std::string ser_out;
    writer.get_url_meta(99999, 99999, url_out, ser_out);
    CHECK(url_out.empty());
    CHECK(ser_out.empty());
  }

  TEST_CASE("multiple urls and ser types all get unique indices and correct reverse lookups") {
    StubBagWriter writer;
    int u1 = -1;
    int s1 = -1;
    int u2 = -1;
    int s2 = -1;
    int u3 = -1;
    int s3 = -1;

    writer.get_url_meta("dds://a", "proto", u1, s1);
    writer.get_url_meta("shm://b", "raw", u2, s2);
    writer.get_url_meta("intra://c", "cdr", u3, s3);

    CHECK_NE(u1, u2);
    CHECK_NE(u2, u3);
    CHECK_NE(u1, u3);
    CHECK_NE(s1, s2);
    CHECK_NE(s2, s3);
    CHECK_NE(s1, s3);

    std::string u;
    std::string s;
    writer.get_url_meta(u2, s2, u, s);
    CHECK_EQ(u, "shm://b");
    CHECK_EQ(s, "raw");
  }

  TEST_CASE("get_default_tag_name returns a non-empty string") {
    CHECK_FALSE(StubBagWriter::get_default_tag_name().empty());
  }

  TEST_CASE("get_default_app_name returns a non-empty string") {
    CHECK_FALSE(StubBagWriter::get_default_app_name().empty());
  }

  TEST_CASE("get_default_timezone_diff returns a value within UTC offset range") {
    int32_t tz = StubBagWriter::get_default_timezone_diff();
    CHECK_GE(tz, -12 * 3600);
    CHECK_LE(tz, 14 * 3600);
  }

  TEST_CASE("get_format_date with no args returns a non-empty string") {
    CHECK_FALSE(StubBagWriter::get_format_date().empty());
  }

  TEST_CASE("get_format_date in file format contains hyphens") {
    std::string date = StubBagWriter::get_format_date(nullptr, true);
    CHECK_FALSE(date.empty());
    CHECK_NE(date.find('-'), std::string::npos);
  }

  TEST_CASE("get_format_date in display format contains slashes and colons") {
    std::string date = StubBagWriter::get_format_date(nullptr, false);
    CHECK_FALSE(date.empty());
    CHECK_NE(date.find('/'), std::string::npos);
    CHECK_NE(date.find(':'), std::string::npos);
  }

  TEST_CASE("get_format_date with fixed epoch produces deterministic year") {
    using SystemClock = BagWriter::SystemClock;
    SystemClock tp{std::chrono::milliseconds(1'000'000'000'000LL)};  // 2001-09-09
    std::string date = StubBagWriter::get_format_date(&tp, false);
    CHECK_NE(date.find("2001"), std::string::npos);
  }

  TEST_CASE("schema callback receives ser_type and schema family") {
    BagWriter::SchemaCallback cb = [](const std::string& ser_type, SchemaType schema_type) {
      SchemaData schema;
      schema.name = ser_type;
      schema.schema_type = schema_type;
      return schema;
    };

    SchemaData result = cb("demo.Message", SchemaType::kProtobuf);
    CHECK_EQ(result.name, "demo.Message");
    CHECK_EQ(result.schema_type, SchemaType::kProtobuf);
  }
}

// NOLINTEND
