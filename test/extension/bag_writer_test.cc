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
#include <fstream>
#include <iterator>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "../common_test.h"
#include "./base/process.h"
#include "./base/utils.h"
#include "./extension/bag_plugin_interface.h"
#include "./extension/bag_processor.h"
#include "./extension/bag_reader.h"

class StubBagWriter : public BagWriter {
 public:
  explicit StubBagWriter(const std::string& path = (std::filesystem::path(Utils::get_tmp_dir()) / "stub.vdb").string(),
                         const BagWriter::Config& config = {})
      : BagWriter(path, config) {}

  explicit StubBagWriter(const BagWriter::Config& config)
      : BagWriter((std::filesystem::path(Utils::get_tmp_dir()) / "stub.vdb").string(), config) {}

  ~StubBagWriter() override = default;

  using BagWriter::detach_plugin;
  using BagWriter::flush_plugin;

  void register_split_callback(SplitCallback&&, bool) override {}

  void register_schema_callback(SchemaCallback&&) override {}

  bool push_schema(const SchemaData&) override {
    ++schema_push_count;
    return true;
  }

  int64_t record(const Frame& frame, int64_t timestamp) override {
    std::lock_guard lock(record_mtx);

    ++record_count;
    last_url = frame.url;
    last_ser_type = frame.ser_type;
    last_schema_type = frame.schema_type;
    last_action_type = frame.action_type;
    last_size = frame.data.size();
    last_timestamp = timestamp;
    recorded_timestamps.push_back(timestamp);
    record_cv.notify_all();

    return timestamp;
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

  void set_url_loss(const std::string& url, double loss) override { BagWriter::set_url_loss(url, loss); }

  double get_url_loss(const std::string& url) { return url_loss_map_ref().at(url); }

  double get_total_url_loss(const std::string& url) { return total_url_loss_map_ref().at(url); }

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
  void on_read(const Frame& frame) override { do_callback(frame); }

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
  void on_read(const Frame& frame) override { do_callback(frame); }

  void on_write(const Frame&) override {}
};

class EmptyUrlWritePlugin final : public BagPluginInterface {
 public:
  void on_read(const Frame& frame) override { do_callback(frame); }

  void on_write(const Frame& frame) override {
    Frame out = frame;
    out.url.clear();
    do_callback(out);
  }
};

class TranscodeWritePlugin final : public BagPluginInterface {
 public:
  void on_read(const Frame& frame) override { do_callback(frame); }

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
  void on_read(const Frame& frame) override { do_callback(frame); }

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

  void on_read(const Frame& frame) override { do_callback(frame); }

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

Bytes timestamp_payload(int64_t timestamp) {
  Bytes payload = Bytes::create(sizeof(timestamp));
  std::memcpy(payload.data(), &timestamp, sizeof(timestamp));
  return payload;
}

SchemaData writer_schema_data(const std::string& name, SchemaType schema_type, const std::string& data) {
  SchemaData schema;
  schema.name = name;
  schema.schema_type = schema_type;
  schema.encoding = std::string(SchemaData::convert_type(schema_type));
  schema.data = Bytes::from_string(data);
  return schema;
}

std::filesystem::path make_writer_tmp_path(const char* suffix) {
  const auto root =
      std::filesystem::path(Utils::get_tmp_dir()) / "vlink-extension-tests" / "bag-writer" / Utils::get_pid_str();
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto dir = root / ("bag_writer_" + Utils::get_pid_str() + "_" + std::to_string(stamp));
  std::filesystem::create_directories(dir);
  return dir / (std::string("bag") + suffix);
}

std::string lower_suffix(std::filesystem::path path) {
  std::string suffix = path.extension().string();
  std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return std::tolower(c); });
  return suffix;
}

void remove_writer_bag_family(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);

  const std::string suffix = lower_suffix(path);
  if (suffix != ".vdbx" && suffix != ".vcapx") {
    return;
  }

  const auto parent = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
  const std::string split_suffix = suffix == ".vdbx" ? ".vdb" : ".vcap";
  const std::string split_prefix = path.stem().string() + ".";

  for (const auto& entry : std::filesystem::directory_iterator(parent, ec)) {
    if (ec) {
      return;
    }

    const auto candidate = entry.path();
    const std::string filename = candidate.filename().string();
    if (filename.rfind(split_prefix, 0) == 0 && lower_suffix(candidate) == split_suffix) {
      std::filesystem::remove(candidate, ec);
    }
  }
}

struct ScopedWriterPath final {
  explicit ScopedWriterPath(const char* suffix) : path(make_writer_tmp_path(suffix)) { remove_writer_bag_family(path); }

  ~ScopedWriterPath() {
    remove_writer_bag_family(path);
    std::error_code ec;
    std::filesystem::remove_all(path.parent_path(), ec);
  }

  std::filesystem::path path;
};

struct ScopedWriterCurrentPath final {
  explicit ScopedWriterCurrentPath(const char* name)
      : old_path(std::filesystem::current_path()),
        path(std::filesystem::path(Utils::get_tmp_dir()) / "vlink-extension-tests" / "bag-writer" /
             Utils::get_pid_str() /
             (std::string(name) + "_" + Utils::get_pid_str() + "_" +
              std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path);
    std::filesystem::current_path(path);
  }

  ~ScopedWriterCurrentPath() {
    std::error_code ec;
    std::filesystem::current_path(old_path, ec);
    std::filesystem::remove_all(path, ec);
  }

  std::filesystem::path old_path;
  std::filesystem::path path;
};

void write_stale_split_manifest(const std::filesystem::path& manifest, const std::string& split_filename) {
  std::ofstream out(manifest, std::ios::binary | std::ios::trunc);
  REQUIRE(out.is_open());
  out << "{\"VLinkFiles\":[\"" << split_filename << "\"]}";
}

class FailingBagWriter final : public StubBagWriter {
 public:
  using StubBagWriter::StubBagWriter;

  int64_t record(const Frame& frame, int64_t timestamp) override {
    (void)StubBagWriter::record(frame, timestamp);
    return -1;
  }
};

std::vector<Frame> read_writer_frames(const std::filesystem::path& path);

void verify_async_write_failure_latches(const char* suffix) {
  ScopedWriterPath bag(suffix);

  BagWriter::Config config;
  config.compress = BagWriter::kCompressNone;
  config.cache_size = 1;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);
  REQUIRE(writer->async_run());

  REQUIRE_GE(writer->push(write_frame("dds://failure", "raw", SchemaType::kRaw, ActionType::kPublish,
                                      Bytes::from_string("first"), 1'000)),
             0);
  REQUIRE_GE(writer->push(write_frame("dds://failure", "other_raw", SchemaType::kRaw, ActionType::kPublish,
                                      Bytes::from_string("second"), 2'000)),
             0);
  REQUIRE(writer->wait_for_idle(3000));
  CHECK(writer->fail());

  writer->quit();
  REQUIRE(writer->wait_for_quit(3000));
  writer.reset();

  const auto frames = read_writer_frames(bag.path);
  REQUIRE_EQ(frames.size(), 1u);
  CHECK_EQ(frames.front().data.to_string(), "first");
}

std::vector<Frame> read_writer_frames(const std::filesystem::path& path) {
  auto reader = BagReader::create(path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());
  REQUIRE(reader->open_cursor());

  std::vector<Frame> frames;
  Frame frame;
  while (reader->read_next(frame)) {
    frames.emplace_back(frame);
  }

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));

  return frames;
}

void verify_sync_mode_plugin_output_without_writer_loop(const char* suffix) {
  ScopedWriterPath bag(suffix);

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.cache_size = 1;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);
  writer->bind_bag_interface(std::make_shared<ReorderWritePlugin>(60'000));

  REQUIRE_GE(writer->push(write_frame("dds://late", "raw", SchemaType::kRaw, ActionType::kPublish,
                                      timestamp_payload(100'000'000), 1)),
             0);
  REQUIRE_GE(writer->push(write_frame("dds://early", "raw", SchemaType::kRaw, ActionType::kPublish,
                                      timestamp_payload(1'000'000), 2)),
             0);

  writer->clear_bag_interface();
  writer->close();
  CHECK_FALSE(writer->fail());
  writer.reset();

  const auto frames = read_writer_frames(bag.path);
  REQUIRE_EQ(frames.size(), 2u);
  CHECK_EQ(frames[0].timestamp, 1'000'000);
  CHECK_EQ(frames[1].timestamp, 100'000'000);
}

void verify_persistent_queue_rejects_without_dropping(const char* suffix) {
  ScopedWriterPath bag(suffix);

  BagWriter::Config config;
  config.compress = BagWriter::kCompressNone;
  config.cache_size = 1;
  config.max_task_depth = 1;
  config.max_memory_size = 4;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);

  REQUIRE_EQ(writer->push(write_frame("dds://queue", "raw", SchemaType::kRaw, ActionType::kPublish,
                                      Bytes::from_string("aa"), 1'000)),
             1'000);
  CHECK_LT(writer->push(write_frame("dds://queue", "raw", SchemaType::kRaw, ActionType::kPublish,
                                    Bytes::from_string("bb"), 2'000)),
           0);

  REQUIRE(writer->async_run());
  REQUIRE(writer->wait_for_idle(3000));

  REQUIRE_EQ(writer->push(write_frame("dds://queue", "raw", SchemaType::kRaw, ActionType::kPublish,
                                      Bytes::from_string("cccc"), 3'000)),
             3'000);
  REQUIRE(writer->wait_for_idle(3000));
  CHECK_FALSE(writer->fail());

  writer->quit();
  REQUIRE(writer->wait_for_quit(3000));
  writer.reset();

  const auto frames = read_writer_frames(bag.path);
  REQUIRE_EQ(frames.size(), 2u);
  CHECK_EQ(frames[0].data.to_string(), "aa");
  CHECK_EQ(frames[1].data.to_string(), "cccc");
}

void verify_split_close_finalizes_manifest(const char* suffix) {
  ScopedWriterPath bag(suffix);

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);
  REQUIRE(writer->is_split_mode());
  REQUIRE_EQ(writer->push(write_frame("dds://close", "raw", SchemaType::kRaw, ActionType::kPublish,
                                      Bytes::from_string("closed"), 1'000)),
             1'000);

  writer->close();
  CHECK_FALSE(writer->fail());

  std::ifstream manifest_file(bag.path, std::ios::binary);
  REQUIRE(manifest_file.is_open());
  const std::string manifest_text((std::istreambuf_iterator<char>(manifest_file)), std::istreambuf_iterator<char>());
  CHECK_NE(manifest_text.find("\"complete\": true"), std::string::npos);
  manifest_file.close();

  writer->close();
  CHECK_FALSE(writer->fail());
}

void verify_vcap_schema_failure_can_retry() {
  ScopedWriterPath bag(".vcap");

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);

  bool valid_schema = false;
  writer->register_schema_callback([&valid_schema](const std::string& ser_type, SchemaType) {
    return writer_schema_data(ser_type, valid_schema ? SchemaType::kProtobuf : SchemaType::kFlatbuffers,
                              valid_schema ? "protobuf schema" : "flatbuffers schema");
  });

  CHECK_LT(writer->push(write_frame("dds://schema_retry", "demo.Retry", SchemaType::kProtobuf, ActionType::kPublish,
                                    Bytes::from_string("rejected"), 1'000)),
           0);

  valid_schema = true;
  REQUIRE_EQ(writer->push(write_frame("dds://schema_retry", "demo.Retry", SchemaType::kProtobuf, ActionType::kPublish,
                                      Bytes::from_string("accepted"), 2'000)),
             2'000);
  writer.reset();

  const auto frames = read_writer_frames(bag.path);
  REQUIRE_EQ(frames.size(), 1u);
  CHECK_EQ(frames.front().data.to_string(), "accepted");
}

void verify_relative_vcap_close_survives_chdir() {
  ScopedWriterCurrentPath cwd("relative_vcap_close");
  const auto other_dir = cwd.path / "other";
  std::filesystem::create_directories(other_dir);

  BagWriter::Config config;
  config.sync_mode = true;
  auto writer = BagWriter::create("relative.vcap", config);
  REQUIRE(writer != nullptr);
  REQUIRE_EQ(writer->push(write_frame("dds://relative_close", "raw", SchemaType::kRaw, ActionType::kPublish,
                                      Bytes::from_string("relative"), 1'000)),
             1'000);

  std::filesystem::current_path(other_dir);
  writer->close();
  CHECK_FALSE(writer->fail());
  std::filesystem::current_path(cwd.path);
  writer.reset();

  const auto frames = read_writer_frames(cwd.path / "relative.vcap");
  REQUIRE_EQ(frames.size(), 1u);
  CHECK_EQ(frames.front().data.to_string(), "relative");
}

void verify_relative_split_close_survives_chdir(const char* suffix) {
  ScopedWriterCurrentPath cwd("relative_split_close");
  const auto other_dir = cwd.path / "other";
  const std::string manifest = std::string("relative") + suffix;
  std::filesystem::create_directories(other_dir);

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.split_by_size = 1;

  auto writer = BagWriter::create(manifest, config);
  REQUIRE(writer != nullptr);
  REQUIRE(writer->is_split_mode());
  REQUIRE_EQ(writer->push(write_frame("dds://relative_split_close", "raw", SchemaType::kRaw, ActionType::kPublish,
                                      Bytes::from_string("relative"), 1'000)),
             1'000);

  std::filesystem::current_path(other_dir);
  REQUIRE_EQ(writer->push(write_frame("dds://relative_split_close", "raw", SchemaType::kRaw, ActionType::kPublish,
                                      Bytes::from_string("rotated"), 2'000)),
             2'000);
  writer->close();
  CHECK_FALSE(writer->fail());
  CHECK_FALSE(std::filesystem::exists(other_dir / manifest));

  const std::string suffix_str = suffix;
  const std::string split_suffix = suffix_str == ".vdbx" ? ".vdb" : ".vcap";
  CHECK(std::filesystem::exists(cwd.path / ("relative.1" + split_suffix)));
  CHECK(std::filesystem::exists(cwd.path / ("relative.2" + split_suffix)));
  CHECK_FALSE(std::filesystem::exists(other_dir / ("relative.2" + split_suffix)));

  {
    std::ifstream manifest_file(cwd.path / manifest, std::ios::binary);
    REQUIRE(manifest_file.is_open());
    const std::string manifest_text((std::istreambuf_iterator<char>(manifest_file)), std::istreambuf_iterator<char>());
    CHECK_NE(manifest_text.find("\"complete\": true"), std::string::npos);
    CHECK_NE(manifest_text.find("relative.1" + split_suffix), std::string::npos);
    CHECK_NE(manifest_text.find("relative.2" + split_suffix), std::string::npos);
    CHECK_EQ(manifest_text.find(cwd.path.string()), std::string::npos);
  }

  writer.reset();
  CHECK_FALSE(std::filesystem::exists(other_dir / manifest));
  std::filesystem::current_path(cwd.path);

  const auto frames = read_writer_frames(manifest);
  REQUIRE_EQ(frames.size(), 2u);
}

Bytes make_unhelpful_compression_payload(size_t size, bool high_ratio) {
  for (uint32_t seed = 1; seed < 128; ++seed) {
    Bytes payload = Bytes::create(size);
    REQUIRE(payload.data() != nullptr);

    uint32_t state = 0x9E3779B9u ^ (seed * 0x85EBCA6Bu);
    for (size_t index = 0; index < payload.size(); ++index) {
      state ^= state << 13U;
      state ^= state >> 17U;
      state ^= state << 5U;
      payload[index] = static_cast<uint8_t>(state >> 24U);
    }

    Bytes compressed = Bytes::compress_data(payload.data(), payload.size(), high_ratio);
    if (compressed.empty() || static_cast<double>(compressed.size()) >= static_cast<double>(payload.size()) * 0.95) {
      return payload;
    }
  }

  FAIL("failed to build deterministic incompressible payload");
  return Bytes{};
}

void verify_async_memory_limit_rejects_before_enqueue(const char* suffix) {
  ScopedWriterPath bag(suffix);

  BagWriter::Config config;
  config.max_memory_size = 0;
  config.max_task_depth = 1;
  config.tag_name = "memory-limit";

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);

  CHECK_LT(writer->push(write_frame("dds://coverage/memory_limit", "raw", SchemaType::kRaw, ActionType::kPublish,
                                    Bytes::from_string("x"))),
           0);

  writer.reset();

  auto frames = read_writer_frames(bag.path);
  CHECK(frames.empty());
}

void verify_real_writer_config_variants(const char* suffix) {
  ScopedWriterPath bag(suffix);

  BagWriter::Config config;
  config.sync_mode = true;
  config.max_task_depth = 0;
  config.cache_size = 0;
  config.start_timestamp = 1'700'000'600'000LL;
  config.tag_name = "config-variants";

  const std::string suffix_str = suffix;
  if (suffix_str == ".vdb") {
    config.wal_mode = true;
    config.optimize_on_exit = true;
  } else {
    config.enable_limit = true;
    config.compress = BagWriter::kCompressZstd;
  }

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);
  CHECK_EQ(writer->get_max_task_count(), static_cast<size_t>(BagWriter::Config().max_task_depth));

  REQUIRE_EQ(writer->push(write_frame("dds://coverage/config", "raw", SchemaType::kRaw, ActionType::kSubscribe,
                                      Bytes::from_string("config"), 1'000)),
             1'000);
  writer->set_url_loss("dds://coverage/config", 0.0);
  writer.reset();

  auto frames = read_writer_frames(bag.path);
  REQUIRE_EQ(frames.size(), 1u);
  CHECK_EQ(frames[0].url, "dds://coverage/config");
  CHECK_EQ(frames[0].action_type, ActionType::kSubscribe);
  CHECK_EQ(frames[0].data.to_string(), "config");
}

void verify_split_manifest_constructor_removes_stale_file(const char* suffix, const char* split_suffix) {
  ScopedWriterPath bag(suffix);
  const std::string stale_name = std::string("obsolete_") + Utils::get_pid_str() + split_suffix;
  const auto stale_path = bag.path.parent_path() / stale_name;

  {
    std::error_code ec;
    std::filesystem::create_directories(stale_path.parent_path(), ec);
    REQUIRE_FALSE(ec);
    std::ofstream stale(stale_path, std::ios::binary | std::ios::trunc);
    REQUIRE(stale.is_open());
    stale << "stale";
  }
  REQUIRE(std::filesystem::exists(stale_path));
  write_stale_split_manifest(bag.path, stale_name);

  BagWriter::Config config;
  config.compress = BagWriter::kCompressNone;
  config.tag_name = "stale-cleanup";

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);
  CHECK(writer->is_split_mode());
  CHECK_FALSE(std::filesystem::exists(stale_path));
  writer.reset();
}

void verify_split_manifest_rejects_unsafe_paths(const char* suffix) {
  ScopedWriterPath scoped(suffix);
  const auto root = scoped.path.parent_path();
  const auto output_dir = root / "output";
  const auto manifest = output_dir / (std::string("bag") + suffix);
  const auto traversal_target = root / "traversal-target";
  const auto absolute_target = root / "absolute-target";
  const auto blocked_directory = output_dir / "blocked-directory";
  const auto blocked_sentinel = blocked_directory / "keep";
  const auto stale_after_blocked = output_dir / "stale-after-blocked";

  std::filesystem::create_directories(output_dir);
  std::filesystem::create_directories(blocked_directory);
  {
    std::ofstream traversal(traversal_target, std::ios::binary | std::ios::trunc);
    std::ofstream absolute(absolute_target, std::ios::binary | std::ios::trunc);
    std::ofstream sentinel(blocked_sentinel, std::ios::binary | std::ios::trunc);
    std::ofstream stale(stale_after_blocked, std::ios::binary | std::ios::trunc);
    REQUIRE(traversal.is_open());
    REQUIRE(absolute.is_open());
    REQUIRE(sentinel.is_open());
    REQUIRE(stale.is_open());
    traversal << "keep";
    absolute << "keep";
    sentinel << "keep";
    stale << "stale";
  }

  nlohmann::json root_json;
  root_json["VLinkFiles"] =
      nlohmann::json::array({"../traversal-target", absolute_target.string(), "nested/file", 7,
                             blocked_directory.filename().string(), stale_after_blocked.filename().string()});
  {
    std::ofstream out(manifest, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out << root_json;
  }

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  auto writer = BagWriter::create(manifest.string(), config);
  REQUIRE(writer != nullptr);
  CHECK(writer->is_split_mode());
  CHECK(std::filesystem::exists(traversal_target));
  CHECK(std::filesystem::exists(absolute_target));
  CHECK(std::filesystem::exists(blocked_sentinel));
  CHECK_FALSE(std::filesystem::exists(stale_after_blocked));
  REQUIRE_EQ(writer->push(write_frame("dds://coverage/manifest_cleanup", "raw", SchemaType::kRaw, ActionType::kPublish,
                                      Bytes::from_string("usable"), 1'000)),
             1'000);
  writer.reset();
}

void verify_vcap_compression_level_variants() {
  for (int level : {0, 1, 2, 3, 4, 5, 99}) {
    ScopedWriterPath bag(".vcap");

    BagWriter::Config config;
    config.sync_mode = true;
    config.cache_size = 256;
    config.compress = BagWriter::kCompressZstd;
    config.compress_level = level;
    config.tag_name = "compress-level";
    config.start_timestamp = 1'700'000'700'000LL + level;

    auto writer = BagWriter::create(bag.path.string(), config);
    REQUIRE(writer != nullptr);
    REQUIRE_EQ(writer->push(write_frame("dds://coverage/compress_level", "raw", SchemaType::kRaw, ActionType::kPublish,
                                        Bytes::from_string("level-" + std::to_string(level)))),
               0);
    writer.reset();

    auto frames = read_writer_frames(bag.path);
    REQUIRE_EQ(frames.size(), 1u);
    CHECK_EQ(frames[0].data.to_string(), "level-" + std::to_string(level));
  }
}

void verify_vdb_compression_skip_after_repeated_unhelpful_frames() {
  ScopedWriterPath bag(".vdb");

  BagWriter::Config config;
  config.sync_mode = true;
  config.cache_size = 1;
  config.compress = BagWriter::kCompressLzav;
  config.compress_start_size = 1;
  config.compress_level = 5;
  config.tag_name = "vdb-compress-skip";
  config.start_timestamp = 1'700'000'760'000LL;

  Bytes payload = make_unhelpful_compression_payload(4096, config.compress_level > 3);
  Bytes compressed = Bytes::compress_data(payload.data(), payload.size(), config.compress_level > 3);
  const bool compression_is_unhelpful =
      compressed.empty() || static_cast<double>(compressed.size()) >= static_cast<double>(payload.size()) * 0.95;
  REQUIRE(compression_is_unhelpful);

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);

  for (int index = 0; index < 6; ++index) {
    REQUIRE_EQ(writer->push(write_frame("dds://coverage/vdb_compress_skip", "raw", SchemaType::kRaw,
                                        ActionType::kPublish, payload, 1'000 + index)),
               1'000 + index);
  }
  writer.reset();

  auto frames = read_writer_frames(bag.path);
  REQUIRE_EQ(frames.size(), 6u);
  for (const auto& frame : frames) {
    REQUIRE_EQ(frame.data.size(), payload.size());
    CHECK_EQ(std::memcmp(frame.data.data(), payload.data(), payload.size()), 0);
  }
}

void verify_vdb_periodic_metadata_update_branch() {
  ScopedWriterPath bag(".vdb");

  BagWriter::Config config;
  config.sync_mode = true;
  config.cache_size = 1;
  config.compress = BagWriter::kCompressNone;
  config.tag_name = "vdb-periodic-update";
  config.start_timestamp = 1'700'000'765'000LL;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);

  REQUIRE_EQ(writer->push(write_frame("dds://coverage/vdb_periodic_update", "raw", SchemaType::kRaw,
                                      ActionType::kPublish, Bytes::from_string("first"), 1'000)),
             1'000);
  std::this_thread::sleep_for(1200ms);
  REQUIRE_EQ(writer->push(write_frame("dds://coverage/vdb_periodic_update", "raw", SchemaType::kRaw,
                                      ActionType::kPublish, Bytes::from_string("second"), 2'000)),
             2'000);
  writer.reset();

  auto frames = read_writer_frames(bag.path);
  REQUIRE_EQ(frames.size(), 2u);
  CHECK_EQ(frames[0].data.to_string(), "first");
  CHECK_EQ(frames[1].data.to_string(), "second");
}

void verify_vdb_wal_optimize_and_async_schema_paths() {
  ScopedWriterPath bag(".vdb");

  BagWriter::Config config;
  config.cache_size = 1;
  config.compress = BagWriter::kCompressLzav;
  config.compress_start_size = 1;
  config.compress_level = 4;
  config.wal_mode = true;
  config.optimize_on_exit = true;
  config.tag_name = "wal-optimize";
  config.start_timestamp = 1'700'000'775'000LL;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);
  REQUIRE(writer->async_run());

  const uint8_t schema_bytes[] = {'a', 's', 'y', 'n', 'c'};
  SchemaData schema;
  schema.name = "demo.AsyncSchema";
  schema.schema_type = SchemaType::kProtobuf;
  schema.encoding = std::string(SchemaData::convert_type(SchemaType::kProtobuf));
  schema.data = Bytes::shallow_copy(schema_bytes, sizeof(schema_bytes));

  CHECK(writer->push_schema(schema));
  REQUIRE(writer->wait_for_idle(3000));

  const std::string payload(512, 'W');
  REQUIRE_GE(writer->push(write_frame("dds://coverage/vdb_wal_async_schema", "demo.AsyncSchema", SchemaType::kProtobuf,
                                      ActionType::kPublish, Bytes::from_string(payload), 1'000)),
             0);
  REQUIRE(writer->wait_for_idle(3000));
  writer.reset();

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());
  CHECK_EQ(reader->get_info().tag_name, "wal-optimize");
  CHECK(reader->get_info().has_completed);
  CHECK(reader->check().get());

  REQUIRE(reader->open_cursor());
  Frame frame;
  REQUIRE(reader->read_next(frame));
  CHECK_EQ(frame.url, "dds://coverage/vdb_wal_async_schema");
  CHECK_EQ(frame.ser_type, "demo.AsyncSchema");
  CHECK_EQ(frame.schema_type, SchemaType::kProtobuf);
  CHECK_EQ(frame.data.to_string(), payload);
  CHECK_FALSE(reader->read_next(frame));

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_vdb_schema_merge_and_failure_variants() {
  ScopedWriterPath bag(".vdb");

  BagWriter::Config config;
  config.sync_mode = true;
  config.cache_size = 1;
  config.tag_name = "schema-merge";
  config.start_timestamp = 1'700'000'800'000LL;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);

  SchemaData empty;
  CHECK(writer->push_schema(empty));

  SchemaData placeholder;
  placeholder.name = "demo.Merge";
  placeholder.schema_type = SchemaType::kUnknown;
  CHECK(writer->push_schema(placeholder));

  const uint8_t schema_bytes[] = {'s', 'c', 'h', 'e', 'm', 'a'};
  SchemaData full;
  full.name = "demo.Merge";
  full.schema_type = SchemaType::kProtobuf;
  full.data = Bytes::shallow_copy(schema_bytes, sizeof(schema_bytes));
  CHECK(writer->push_schema(full));

  SchemaData duplicate = full;
  CHECK(writer->push_schema(duplicate));

  REQUIRE_EQ(writer->push(write_frame("dds://coverage/schema_merge", "demo.Merge", SchemaType::kUnknown,
                                      ActionType::kPublish, Bytes::from_string("schema-merge"), 1'000)),
             1'000);

  SchemaData conflict = full;
  conflict.data = Bytes::from_string("different");
  CHECK_FALSE(writer->push_schema(conflict));

  writer->register_schema_callback([](const std::string& ser_type, SchemaType) {
    return writer_schema_data(ser_type, SchemaType::kFlatbuffers, "flatbuffer schema");
  });

  CHECK_LT(writer->push(write_frame("dds://coverage/schema_mismatch", "demo.Mismatch", SchemaType::kProtobuf,
                                    ActionType::kPublish, Bytes::from_string("mismatch"), 2'000)),
           0);

  writer.reset();

  auto frames = read_writer_frames(bag.path);
  REQUIRE_EQ(frames.size(), 1u);
  CHECK_EQ(frames[0].url, "dds://coverage/schema_merge");
  CHECK_EQ(frames[0].schema_type, SchemaType::kProtobuf);
  CHECK_EQ(frames[0].data.to_string(), "schema-merge");
}

void verify_vcap_schema_merge_and_failure_variants() {
  ScopedWriterPath bag(".vcap");

  BagWriter::Config config;
  config.sync_mode = true;
  config.cache_size = 1;
  config.compress = BagWriter::kCompressNone;
  config.tag_name = "vcap-schema-merge";
  config.start_timestamp = 1'700'000'900'000LL;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);

  SchemaData empty;
  CHECK(writer->push_schema(empty));

  SchemaData placeholder;
  placeholder.name = "demo.VcapMerge";
  placeholder.schema_type = SchemaType::kUnknown;
  CHECK(writer->push_schema(placeholder));

  const uint8_t schema_bytes[] = {'v', 'c', 'a', 'p'};
  SchemaData full;
  full.name = "demo.VcapMerge";
  full.schema_type = SchemaType::kProtobuf;
  full.data = Bytes::shallow_copy(schema_bytes, sizeof(schema_bytes));
  CHECK(writer->push_schema(full));

  SchemaData duplicate = full;
  CHECK(writer->push_schema(duplicate));

  REQUIRE_EQ(writer->push(write_frame("dds://coverage/vcap_schema_merge", "demo.VcapMerge", SchemaType::kUnknown,
                                      ActionType::kPublish, Bytes::from_string("schema-merge"), 1'000)),
             1'000);

  SchemaData conflict = full;
  conflict.data = Bytes::from_string("different");
  CHECK_FALSE(writer->push_schema(conflict));

  writer->register_schema_callback([](const std::string& ser_type, SchemaType) {
    return writer_schema_data(ser_type, SchemaType::kFlatbuffers, "flatbuffer schema");
  });

  CHECK_LT(writer->push(write_frame("dds://coverage/vcap_schema_mismatch", "demo.VcapMismatch", SchemaType::kProtobuf,
                                    ActionType::kPublish, Bytes::from_string("mismatch"), 2'000)),
           0);

  writer.reset();

  auto frames = read_writer_frames(bag.path);
  REQUIRE_EQ(frames.size(), 1u);
  CHECK_EQ(frames[0].url, "dds://coverage/vcap_schema_merge");
  CHECK_EQ(frames[0].schema_type, SchemaType::kProtobuf);
  CHECK_EQ(frames[0].data.to_string(), "schema-merge");
}

void verify_real_writer_rejects_url_metadata_conflicts(const char* suffix) {
  ScopedWriterPath bag(suffix);

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.tag_name = "metadata-conflict";
  config.start_timestamp = 1'700'001'000'000LL;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);

  REQUIRE_EQ(writer->push(write_frame("dds://coverage/meta_conflict", "raw", SchemaType::kRaw, ActionType::kPublish,
                                      Bytes::from_string("first"), 1'000)),
             1'000);

  CHECK_LT(writer->push(write_frame("dds://coverage/meta_conflict", "other_raw", SchemaType::kRaw, ActionType::kPublish,
                                    Bytes::from_string("changed-ser"), 2'000)),
           0);

  CHECK_LT(writer->push(write_frame("dds://coverage/meta_conflict", "raw", SchemaType::kProtobuf, ActionType::kPublish,
                                    Bytes::from_string("changed-schema"), 3'000)),
           0);

  writer.reset();
}

void verify_relative_split_writer_paths(const char* suffix, bool split_before) {
  ScopedWriterCurrentPath cwd("relative_split");
  const std::string manifest = std::string("relative") + suffix;
  const std::string suffix_str = suffix;
  const std::string split_suffix = suffix_str == ".vdbx" ? ".vdb" : ".vcap";

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.split_by_size = 0;
  config.split_by_time = 1;
  config.split_name_by_time = true;
  config.tag_name = split_before ? "relative-before" : "relative-after";
  config.start_timestamp = 1'700'000'900'000LL;

  auto writer = BagWriter::create(manifest, config);
  REQUIRE(writer != nullptr);
  REQUIRE(writer->is_split_mode());

  std::vector<std::string> callback_files;
  writer->register_split_callback(
      [&callback_files](int split_index, const std::string& split_filename) {
        CHECK_GE(split_index, 0);
        callback_files.emplace_back(split_filename);
      },
      split_before);

  REQUIRE_EQ(writer->push(write_frame("dds://coverage/relative_split", "raw", SchemaType::kRaw, ActionType::kPublish,
                                      Bytes::from_string("first"), 0)),
             0);
  REQUIRE_EQ(writer->push(write_frame("dds://coverage/relative_split", "raw", SchemaType::kRaw, ActionType::kPublish,
                                      Bytes::from_string("second"), 5'000)),
             5'000);
  writer.reset();

  REQUIRE(std::filesystem::exists(cwd.path / manifest));
  for (const auto& file : callback_files) {
    CHECK(std::filesystem::path(file).parent_path().empty());
    CHECK_EQ(lower_suffix(file), split_suffix);
    CHECK(std::filesystem::exists(cwd.path / file));
  }
  CHECK_GE(callback_files.size(), split_before ? 1u : 2u);

  {
    std::ifstream manifest_file(cwd.path / manifest, std::ios::binary);
    REQUIRE(manifest_file.is_open());
    const std::string manifest_text((std::istreambuf_iterator<char>(manifest_file)), std::istreambuf_iterator<char>());
    CHECK_NE(manifest_text.find("\"complete\": true"), std::string::npos);
    CHECK_NE(manifest_text.find(split_suffix), std::string::npos);
  }

  auto frames = read_writer_frames(manifest);
  REQUIRE_EQ(frames.size(), 2u);
  CHECK_EQ(frames[0].data.to_string(), "first");
  CHECK_EQ(frames[1].data.to_string(), "second");
}

void verify_real_writer_timestamp_and_unknown_action_variants(const char* suffix) {
  ScopedWriterPath bag(suffix);

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.tag_name = "timestamp-variants";
  config.start_timestamp = 1'700'001'100'000LL;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);

  const int64_t auto_timestamp = writer->push(write_frame("dds://coverage/auto_timestamp", "raw", SchemaType::kRaw,
                                                          ActionType::kPublish, Bytes::from_string("auto"), -1));
  CHECK_GE(auto_timestamp, 0);

  REQUIRE_EQ(writer->push(write_frame("dds://coverage/out_of_order", "raw", SchemaType::kRaw, ActionType::kPublish,
                                      Bytes::from_string("first"), 5'000'000)),
             5'000'000);
  REQUIRE_EQ(writer->push(write_frame("dds://coverage/out_of_order", "raw", SchemaType::kRaw, ActionType::kPublish,
                                      Bytes::from_string("second"), 4'999'950)),
             4'999'950);
  REQUIRE_EQ(writer->push(write_frame("dds://coverage/unknown_action", "raw", SchemaType::kRaw,
                                      ActionType::kUnknownAction, Bytes::from_string("unknown"), 6'000'000)),
             6'000'000);
  writer.reset();

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());
  CHECK(reader->check().get());

  bool saw_auto_timestamp = false;
  bool saw_unknown_action = false;
  for (const auto& meta : reader->get_info().url_metas) {
    if (meta.url == "dds://coverage/auto_timestamp") {
      saw_auto_timestamp = true;
    } else if (meta.url == "dds://coverage/unknown_action") {
      saw_unknown_action = true;
      CHECK_EQ(meta.url_type, "Unknown");
    }
  }
  CHECK(saw_auto_timestamp);
  CHECK(saw_unknown_action);

  REQUIRE(reader->open_cursor());
  std::vector<Frame> frames;
  Frame frame;
  while (reader->read_next(frame)) {
    frames.emplace_back(frame);
  }

  std::vector<int64_t> out_of_order_timestamps;
  for (const auto& stored_frame : frames) {
    if (stored_frame.url == "dds://coverage/out_of_order") {
      out_of_order_timestamps.emplace_back(stored_frame.timestamp);
    }
  }

  REQUIRE_EQ(out_of_order_timestamps.size(), 2u);
  CHECK_EQ(out_of_order_timestamps[0], 5'000'000);
  CHECK_EQ(out_of_order_timestamps[1], 5'000'001);

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_writer_creates_missing_parent_directory(const char* suffix) {
  const auto root = std::filesystem::path(Utils::get_tmp_dir()) / "vlink-extension-tests" / "bag-writer" /
                    Utils::get_pid_str() /
                    ("missing_parent_" + Utils::get_pid_str() + "_" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto path = root / "nested" / (std::string("bag") + suffix);

  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.tag_name = "missing-parent";
  config.start_timestamp = 1'700'001'200'000LL;

  auto writer = BagWriter::create(path.string(), config);
  REQUIRE(writer != nullptr);
  REQUIRE_EQ(writer->push(write_frame("dds://coverage/missing_parent", "raw", SchemaType::kRaw, ActionType::kPublish,
                                      Bytes::from_string("created"), 1'000)),
             1'000);
  writer.reset();

  CHECK(std::filesystem::exists(path));
  auto frames = read_writer_frames(path);
  REQUIRE_EQ(frames.size(), 1u);
  CHECK_EQ(frames.front().data.to_string(), "created");

  std::filesystem::remove_all(root, ec);
}

void verify_auto_compression_writer_remains_readable(const char* suffix) {
  ScopedWriterPath bag(suffix);

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressAuto;
  config.compress_start_size = 1;
  config.compress_level = 5;
  config.cache_size = 1;
  config.tag_name = "auto-compress";
  config.start_timestamp = 1'700'001'300'000LL;

  Bytes payload = Bytes::create(4096u);
  REQUIRE(payload.data() != nullptr);
  std::fill(payload.data(), payload.data() + payload.size(), static_cast<uint8_t>('A'));

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);
  REQUIRE_EQ(writer->push(write_frame("dds://coverage/auto_compress", "raw", SchemaType::kRaw, ActionType::kPublish,
                                      payload, 1'000)),
             1'000);
  writer.reset();

  auto frames = read_writer_frames(bag.path);
  REQUIRE_EQ(frames.size(), 1u);
  CHECK_EQ(frames.front().url, "dds://coverage/auto_compress");
  REQUIRE_EQ(frames.front().data.size(), payload.size());
  CHECK_EQ(std::memcmp(frames.front().data.data(), payload.data(), payload.size()), 0);
}

void verify_async_writer_path_remains_readable(const char* suffix) {
  ScopedWriterPath bag(suffix);

  BagWriter::Config config;
  config.compress = BagWriter::kCompressNone;
  config.cache_size = 1;
  config.tag_name = "async-path";
  config.start_timestamp = 1'700'001'400'000LL;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);
  REQUIRE(writer->async_run());

  REQUIRE_EQ(writer->push(write_frame("dds://coverage/async_path", "raw", SchemaType::kRaw, ActionType::kPublish,
                                      Bytes::from_string("async"), 1'000)),
             1'000);
  REQUIRE(writer->wait_for_idle(3000));
  writer.reset();

  auto frames = read_writer_frames(bag.path);
  REQUIRE_EQ(frames.size(), 1u);
  CHECK_EQ(frames.front().url, "dds://coverage/async_path");
  CHECK_EQ(frames.front().data.to_string(), "async");
}

void verify_vdb_limit_mode_variants() {
  {
    ScopedWriterPath bag(".vdb");

    BagWriter::Config config;
    config.sync_mode = true;
    config.compress = BagWriter::kCompressNone;
    config.cache_size = 1;
    config.max_row_count = 0;
    config.enable_limit = false;
    config.tag_name = "limit-reject";
    config.start_timestamp = 1'700'001'500'000LL;

    auto writer = BagWriter::create(bag.path.string(), config);
    REQUIRE(writer != nullptr);
    REQUIRE_EQ(writer->push(write_frame("dds://coverage/limit_reject", "raw", SchemaType::kRaw, ActionType::kPublish,
                                        Bytes::from_string("first"), 1'000)),
               1'000);
    CHECK_LT(writer->push(write_frame("dds://coverage/limit_reject", "raw", SchemaType::kRaw, ActionType::kPublish,
                                      Bytes::from_string("second"), 2'000)),
             0);
    CHECK_FALSE(writer->fail());
    writer.reset();

    auto frames = read_writer_frames(bag.path);
    REQUIRE_EQ(frames.size(), 1u);
    CHECK_EQ(frames.front().data.to_string(), "first");
  }

  {
    ScopedWriterPath bag(".vdb");

    BagWriter::Config config;
    config.sync_mode = true;
    config.compress = BagWriter::kCompressNone;
    config.cache_size = 1;
    config.max_row_count = 0;
    config.enable_limit = true;
    config.tag_name = "limit-evict";
    config.start_timestamp = 1'700'001'501'000LL;

    auto writer = BagWriter::create(bag.path.string(), config);
    REQUIRE(writer != nullptr);
    REQUIRE_EQ(writer->push(write_frame("dds://coverage/limit_evict", "raw", SchemaType::kRaw, ActionType::kPublish,
                                        Bytes::from_string("first"), 1'000)),
               1'000);
    REQUIRE_EQ(writer->push(write_frame("dds://coverage/limit_evict", "raw", SchemaType::kRaw, ActionType::kPublish,
                                        Bytes::from_string("second"), 2'000)),
               2'000);
    writer.reset();

    auto frames = read_writer_frames(bag.path);
    REQUIRE_EQ(frames.size(), 1u);
    CHECK_EQ(frames.front().data.to_string(), "second");

    auto reader = BagReader::create(bag.path.string(), false);
    REQUIRE(reader != nullptr);
    REQUIRE(reader->async_run());
    CHECK(reader->check().get());
    reader->quit();
    REQUIRE(reader->wait_for_quit(3000));
  }
}

void verify_vdb_compressed_byte_limit_eviction(const char* suffix) {
  ScopedWriterPath bag(suffix);

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressLzav;
  config.compress_start_size = 1;
  config.cache_size = 1;
  config.max_bytes_size = 8192;
  config.enable_limit = true;
  config.tag_name = "compressed-limit-evict";

  const std::string first_payload(4096, 'A');
  const std::string second_payload(4096, 'B');
  const std::string third_payload(4096, 'C');
  const std::string fourth_payload(4096, 'D');
  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);
  REQUIRE_EQ(writer->push(write_frame("dds://coverage/compressed_limit", "raw", SchemaType::kRaw, ActionType::kPublish,
                                      Bytes::from_string(first_payload), 1'000'000)),
             1'000'000);
  REQUIRE_EQ(writer->push(write_frame("dds://coverage/compressed_limit", "raw", SchemaType::kRaw, ActionType::kPublish,
                                      Bytes::from_string(second_payload), 2'000'000)),
             2'000'000);
  REQUIRE_EQ(writer->push(write_frame("dds://coverage/compressed_limit", "raw", SchemaType::kRaw, ActionType::kPublish,
                                      Bytes::from_string(third_payload), 10'000'000)),
             10'000'000);
  REQUIRE_EQ(writer->push(write_frame("dds://coverage/compressed_limit", "raw", SchemaType::kRaw, ActionType::kPublish,
                                      Bytes::from_string(fourth_payload), 20'000'000)),
             20'000'000);
  writer.reset();

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());
  CHECK(reader->check().get());
  REQUIRE_EQ(reader->get_info().message_count, 3);
  REQUIRE_EQ(reader->get_info().url_metas.size(), 1u);
  CHECK_EQ(reader->get_info().url_metas.front().count, 3u);
  CHECK_EQ(reader->get_info().url_metas.front().size,
           second_payload.size() + third_payload.size() + fourth_payload.size());
  CHECK_EQ(reader->get_info().url_metas.front().freq, doctest::Approx(1.0 / 6.0));

  REQUIRE(reader->open_cursor());
  Frame frame;
  REQUIRE(reader->read_next(frame));
  CHECK_EQ(frame.data.to_string(), second_payload);
  REQUIRE(reader->read_next(frame));
  CHECK_EQ(frame.data.to_string(), third_payload);
  REQUIRE(reader->read_next(frame));
  CHECK_EQ(frame.data.to_string(), fourth_payload);
  CHECK_FALSE(reader->read_next(frame));
  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_vdb_precompressed_byte_limit_eviction(const char* suffix) {
  ScopedWriterPath bag(suffix);
  const std::string url = "dds://coverage/precompressed_limit";
  const std::string payload(4096, 'P');
  const Bytes packed = Bytes::compress_data(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  REQUIRE(Bytes::is_compress_data(packed.data(), packed.size()));
  REQUIRE_LT(packed.size(), payload.size());

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressLzav;
  config.compress_start_size = 1;
  config.cache_size = 1;
  config.max_bytes_size = static_cast<int64_t>(packed.size());
  config.enable_limit = true;
  config.ignore_compress_urls.insert(url);
  config.tag_name = "precompressed-limit-evict";

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);
  REQUIRE_EQ(writer->push(write_frame(url, "raw", SchemaType::kRaw, ActionType::kPublish, packed, 1'000'000)),
             1'000'000);
  REQUIRE_EQ(writer->push(write_frame(url, "raw", SchemaType::kRaw, ActionType::kPublish, packed, 2'000'000)),
             2'000'000);
  REQUIRE_EQ(writer->push(write_frame(url, "raw", SchemaType::kRaw, ActionType::kPublish, packed, 3'000'000)),
             3'000'000);
  writer.reset();

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());
  CHECK(reader->check().get());
  REQUIRE_EQ(reader->get_info().message_count, 1);
  REQUIRE_EQ(reader->get_info().url_metas.size(), 1u);
  CHECK_EQ(reader->get_info().url_metas.front().count, 1u);
  CHECK_EQ(reader->get_info().url_metas.front().size, payload.size());

  REQUIRE(reader->open_cursor());
  Frame frame;
  REQUIRE(reader->read_next(frame));
  CHECK_EQ(frame.timestamp, 3'000'000);
  CHECK_EQ(frame.data.to_string(), payload);
  CHECK_FALSE(reader->read_next(frame));
  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_split_by_size_writer_paths(const char* suffix) {
  ScopedWriterPath bag(suffix);

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.split_by_size = 1;
  config.split_by_time = 0;
  config.split_name_by_time = false;
  config.tag_name = "split-size";
  config.start_timestamp = 1'700'001'600'000LL;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);
  REQUIRE(writer->is_split_mode());

  std::vector<std::string> split_files;
  writer->register_split_callback(
      [&split_files](int split_index, const std::string& split_filename) {
        CHECK_GE(split_index, 0);
        split_files.emplace_back(split_filename);
      },
      false);

  REQUIRE_EQ(writer->push(write_frame("dds://coverage/split_size", "raw", SchemaType::kRaw, ActionType::kPublish,
                                      Bytes::from_string("first"), 1'000)),
             1'000);
  REQUIRE_EQ(writer->push(write_frame("dds://coverage/split_size", "raw", SchemaType::kRaw, ActionType::kPublish,
                                      Bytes::from_string("second"), 2'000)),
             2'000);
  writer.reset();

  CHECK_GE(split_files.size(), 2u);
  auto frames = read_writer_frames(bag.path);
  REQUIRE_EQ(frames.size(), 2u);
  CHECK_EQ(frames[0].data.to_string(), "first");
  CHECK_EQ(frames[1].data.to_string(), "second");
}

void verify_method_schema_split_and_field_metadata(const char* suffix) {
  ScopedWriterPath bag(suffix);

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.cache_size = 1;
  config.tag_name = "method-field";
  config.start_timestamp = 1'700'001'700'000LL;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);
  writer->register_schema_callback([](const std::string& ser_type, SchemaType schema_type) {
    return writer_schema_data(ser_type, schema_type == SchemaType::kUnknown ? SchemaType::kProtobuf : schema_type,
                              "schema:" + ser_type);
  });

  const std::string method_ser = "demo.Request|demo.Response";
  REQUIRE_EQ(writer->push(write_frame("dds://coverage/method_schema", method_ser, SchemaType::kProtobuf,
                                      ActionType::kClientRequest, Bytes::from_string("request"), 1'000)),
             1'000);
  REQUIRE_EQ(writer->push(write_frame("dds://coverage/method_schema", method_ser, SchemaType::kProtobuf,
                                      ActionType::kClientResponse, Bytes::from_string("response"), 2'000)),
             2'000);
  REQUIRE_EQ(writer->push(write_frame("dds://coverage/field_set", "raw", SchemaType::kRaw, ActionType::kSet,
                                      Bytes::from_string("set"), 3'000)),
             3'000);
  REQUIRE_EQ(writer->push(write_frame("dds://coverage/field_get", "raw", SchemaType::kRaw, ActionType::kGet,
                                      Bytes::from_string("get"), 4'000)),
             4'000);
  writer.reset();

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());
  CHECK(reader->check().get());

  bool saw_method = false;
  bool saw_field = false;
  for (const auto& meta : reader->get_info().url_metas) {
    if (meta.url == "dds://coverage/method_schema") {
      saw_method = true;
      CHECK_EQ(meta.url_type, "Method");
      CHECK_EQ(meta.schema_type, SchemaType::kProtobuf);
    } else if (meta.url == "dds://coverage/field_set" || meta.url == "dds://coverage/field_get") {
      saw_field = true;
      CHECK_EQ(meta.url_type, "Field");
    }
  }
  CHECK(saw_method);
  CHECK(saw_field);

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_ignore_compress_url_remains_readable(const char* suffix) {
  ScopedWriterPath bag(suffix);

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressAuto;
  config.compress_start_size = 1;
  config.compress_level = 5;
  config.cache_size = 1;
  config.ignore_compress_urls.insert("dds://coverage/ignore_compress");
  config.tag_name = "ignore-compress";
  config.start_timestamp = 1'700'001'800'000LL;

  Bytes payload = Bytes::create(2048u);
  REQUIRE(payload.data() != nullptr);
  std::fill(payload.data(), payload.data() + payload.size(), static_cast<uint8_t>('Z'));

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);
  REQUIRE_EQ(writer->push(write_frame("dds://coverage/ignore_compress", "raw", SchemaType::kRaw, ActionType::kPublish,
                                      payload, 1'000)),
             1'000);
  writer.reset();

  auto frames = read_writer_frames(bag.path);
  REQUIRE_EQ(frames.size(), 1u);
  REQUIRE_EQ(frames.front().data.size(), payload.size());
  CHECK_EQ(std::memcmp(frames.front().data.data(), payload.data(), payload.size()), 0);
}

TEST_SUITE("extension-BagWriter") {
  TEST_CASE("bind_bag_interface marks the plugin as write direction") {
    StubBagWriter writer;
    auto plugin = std::make_shared<RewriteWritePlugin>();
    writer.bind_bag_interface(plugin);
    CHECK_EQ(plugin->get_direction(), BagPluginInterface::Direction::kWrite);
  }

  TEST_CASE("on_write re-emits rewritten url/ser/schema via do_callback before record") {
    StubBagWriter writer;
    writer.bind_bag_interface(std::make_shared<RewriteWritePlugin>());

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

  TEST_CASE("deferred writer failures latch fail state") {
    verify_async_write_failure_latches(".vdb");
    verify_async_write_failure_latches(".vcap");
  }

  TEST_CASE("on_write that does not emit drops the frame") {
    StubBagWriter writer;
    writer.bind_bag_interface(std::make_shared<DropWritePlugin>());

    Bytes data = Bytes::create(4u);
    int64_t result = writer.push(write_frame("dds://x", "raw", SchemaType::kUnknown, ActionType::kPublish, data, -1));

    CHECK_EQ(result, 4242);
    CHECK_EQ(writer.record_count, 0);
  }

  TEST_CASE("plugin path propagates synchronous record failure") {
    FailingBagWriter writer;
    writer.bind_bag_interface(std::make_shared<RewriteWritePlugin>());

    Bytes data = Bytes::create(4u);
    int64_t result = writer.push(write_frame("dds://x", "raw", SchemaType::kUnknown, ActionType::kPublish, data, 100));

    CHECK_LT(result, 0);
    CHECK_EQ(writer.record_count, 1);
  }

  TEST_CASE("plugin path treats synchronously emitted empty url as failure") {
    StubBagWriter writer;
    writer.bind_bag_interface(std::make_shared<EmptyUrlWritePlugin>());

    Bytes data = Bytes::create(4u);
    int64_t result = writer.push(write_frame("dds://x", "raw", SchemaType::kUnknown, ActionType::kPublish, data, 100));

    CHECK_LT(result, 0);
    CHECK_EQ(writer.record_count, 0);
  }

  TEST_CASE("on_write re-emits a replacement payload via do_callback") {
    StubBagWriter writer;
    writer.bind_bag_interface(std::make_shared<TranscodeWritePlugin>());

    Bytes data = Bytes::create(8u);
    writer.push(write_frame("dds://raw", "raw", SchemaType::kUnknown, ActionType::kPublish, data, 100));

    CHECK_EQ(writer.record_count, 1);
    CHECK_EQ(writer.last_ser_type, "jpeg");
    CHECK_EQ(writer.last_size, 99u);
  }

  TEST_CASE("synchronous write plugin url rewrite is tracked for loss-metadata alignment") {
    StubBagWriter writer;
    writer.bind_bag_interface(std::make_shared<RewriteWritePlugin>());

    Bytes data = Bytes::create(8u);
    writer.push(write_frame("dds://raw", "raw", SchemaType::kUnknown, ActionType::kPublish, data, 100));

    CHECK_EQ(writer.convert_recorded_url("dds://raw"), "dds://jpeg");
    CHECK_EQ(writer.recover_recorded_url("dds://jpeg"), "dds://raw");
    CHECK_EQ(writer.convert_recorded_url("dds://unmapped"), "dds://unmapped");
  }

  TEST_CASE("plugin url remap survives unbind for close-time metadata") {
    StubBagWriter writer;
    writer.bind_bag_interface(std::make_shared<RewriteWritePlugin>());

    Bytes data = Bytes::create(8u);
    writer.push(write_frame("dds://raw", "raw", SchemaType::kUnknown, ActionType::kPublish, data, 100));
    writer.bind_bag_interface(nullptr);

    CHECK_EQ(writer.convert_recorded_url("dds://raw"), "dds://jpeg");
    CHECK_EQ(writer.recover_recorded_url("dds://jpeg"), "dds://raw");
  }

  TEST_CASE("fan-out plugin tracks every recorded url for one origin") {
    StubBagWriter writer;
    writer.bind_bag_interface(std::make_shared<FanOutWritePlugin>());

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
    writer.bind_bag_interface(std::make_shared<ReorderWritePlugin>(60'000));

    auto make_payload = [](int64_t data_timestamp) {
      Bytes payload = Bytes::create(sizeof(int64_t));
      std::memcpy(payload.data(), &data_timestamp, sizeof(int64_t));
      return payload;
    };

    writer.push(write_frame("dds://c", "raw", SchemaType::kUnknown, ActionType::kPublish, make_payload(50'000'001), 1));
    writer.push(write_frame("dds://a", "raw", SchemaType::kUnknown, ActionType::kPublish, make_payload(1), 2));
    writer.push(write_frame("dds://b", "raw", SchemaType::kUnknown, ActionType::kPublish, make_payload(20'000'000), 3));

    writer.flush_plugin();

    {
      std::lock_guard lock(writer.record_mtx);
      REQUIRE_EQ(writer.recorded_timestamps.size(), 3u);
      CHECK_EQ(writer.recorded_timestamps[0], 1);
      CHECK_EQ(writer.recorded_timestamps[1], 20'000'000);
      CHECK_EQ(writer.recorded_timestamps[2], 50'000'001);
    }

    writer.bind_bag_interface(nullptr);
  }

  TEST_CASE("sync-mode plugin output bypasses the writer queue for worker and flush emissions") {
    BagWriter::Config config;
    config.sync_mode = true;
    StubBagWriter writer(config);
    writer.bind_bag_interface(std::make_shared<ReorderWritePlugin>(60'000));

    writer.push(
        write_frame("dds://a", "raw", SchemaType::kUnknown, ActionType::kPublish, timestamp_payload(100'000'000), 1));
    writer.push(
        write_frame("dds://b", "raw", SchemaType::kUnknown, ActionType::kPublish, timestamp_payload(1'000'000), 2));

    {
      std::unique_lock lock(writer.record_mtx);
      REQUIRE(
          writer.record_cv.wait_for(lock, std::chrono::seconds(1), [&writer]() { return writer.record_count > 0; }));
    }

    writer.clear_bag_interface();

    std::lock_guard lock(writer.record_mtx);
    REQUIRE_EQ(writer.recorded_timestamps.size(), 2u);
    CHECK_EQ(writer.recorded_timestamps[0], 1'000'000);
    CHECK_EQ(writer.recorded_timestamps[1], 100'000'000);
  }

  TEST_CASE("real sync-mode writers persist plugin output without a recording loop") {
    verify_sync_mode_plugin_output_without_writer_loop(".vdb");
    verify_sync_mode_plugin_output_without_writer_loop(".vcap");
  }

  TEST_CASE("sync-mode plugin-worker output latches record failures") {
    BagWriter::Config config;
    config.sync_mode = true;
    FailingBagWriter writer(config);
    writer.bind_bag_interface(std::make_shared<ReorderWritePlugin>(60'000));

    REQUIRE_GE(writer.push(write_frame("dds://late", "raw", SchemaType::kRaw, ActionType::kPublish,
                                       timestamp_payload(100'000'000), 1)),
               0);
    REQUIRE_GE(writer.push(write_frame("dds://early", "raw", SchemaType::kRaw, ActionType::kPublish,
                                       timestamp_payload(1'000'000), 2)),
               0);

    REQUIRE(common_test::wait_until([&writer]() { return writer.fail(); }));
    writer.clear_bag_interface();
  }

  TEST_CASE("teardown flushes an async plugin's buffered tail frames instead of dropping them") {
    StubBagWriter writer;
    // 60s reorder window: pushed frames stay buffered in the plugin, never auto-drained during the test.
    writer.bind_bag_interface(std::make_shared<ReorderWritePlugin>(60'000));

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
    writer.bind_bag_interface(std::make_shared<ReorderWritePlugin>(60'000));

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

    writer.bind_bag_interface(nullptr);  // unbind must flush the buffered tail before detaching

    {
      std::lock_guard lock(writer.record_mtx);
      REQUIRE_EQ(writer.recorded_timestamps.size(), 2u);
      CHECK_EQ(writer.recorded_timestamps[0], 5'000'000);
      CHECK_EQ(writer.recorded_timestamps[1], 10'000'000);
    }
  }

  TEST_CASE("flush_plugin drains an async write plugin's buffer while keeping it bound") {
    StubBagWriter writer;
    writer.bind_bag_interface(std::make_shared<ReorderWritePlugin>(60'000));

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

    writer.flush_plugin();  // drains the buffer without unbinding the plugin

    {
      std::lock_guard lock(writer.record_mtx);
      REQUIRE_EQ(writer.recorded_timestamps.size(), 2u);
      CHECK_EQ(writer.recorded_timestamps[0], 5'000'000);
      CHECK_EQ(writer.recorded_timestamps[1], 10'000'000);
    }

    // The plugin is still bound: a further push is routed through it and buffered again.
    writer.push(write_frame("dds://c", "raw", SchemaType::kUnknown, ActionType::kPublish, make_payload(1'000'000), 3));

    {
      std::lock_guard lock(writer.record_mtx);
      CHECK_EQ(writer.recorded_timestamps.size(), 2u);  // new frame buffered, not yet drained
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

  TEST_CASE("negative frame timestamp is assigned by the backend clock") {
    StubBagWriter writer;

    Bytes data = Bytes::create(5u);
    const int64_t result =
        writer.push(write_frame("dds://auto_time", "raw", SchemaType::kUnknown, ActionType::kPublish, data, -1));

    CHECK_EQ(result, 4242);
    CHECK_EQ(writer.record_count, 1);
    CHECK_EQ(writer.last_timestamp, 4242);
  }

  TEST_CASE("set_url_loss stores valid values and normalizes impossible loss") {
    StubBagWriter writer;

    writer.set_url_loss("dds://loss/ok", 0.5);
    writer.set_url_loss("dds://loss/bad", 1.5);

    CHECK_EQ(writer.get_url_loss("dds://loss/ok"), doctest::Approx(0.5));
    CHECK_EQ(writer.get_total_url_loss("dds://loss/ok"), doctest::Approx(0.5));
    CHECK_EQ(writer.get_url_loss("dds://loss/bad"), doctest::Approx(-1.0));
    CHECK_EQ(writer.get_total_url_loss("dds://loss/bad"), doctest::Approx(-1.0));
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
    auto writer = BagWriter::create((std::filesystem::path(Utils::get_tmp_dir()) / "unsupported.xyz").string());
    CHECK_EQ(writer, nullptr);
  }

  TEST_CASE("filter_get returns nullptr for unsupported file extension") {
    auto writer = BagWriter::filter_get((std::filesystem::path(Utils::get_tmp_dir()) / "unsupported.xyz").string());
    CHECK_EQ(writer, nullptr);
  }

  TEST_CASE("filter_get reuses a live writer for the same path") {
    ScopedWriterPath bag(".vdb");

    auto first = BagWriter::filter_get(bag.path.string());
    REQUIRE(first != nullptr);
    auto second = BagWriter::filter_get(bag.path.string());
    REQUIRE(second != nullptr);

    CHECK_EQ(first.get(), second.get());

    first->quit();
    REQUIRE(first->wait_for_quit(3000));
    first.reset();
    second.reset();
  }

  TEST_CASE("filter_get recreates writers after cached weak references expire") {
    for (const auto* suffix : {".vdb", ".vcap"}) {
      ScopedWriterPath bag(suffix);

      auto first = BagWriter::filter_get(bag.path.string());
      REQUIRE(first != nullptr);

      first->quit();
      REQUIRE(first->wait_for_quit(3000));
      first.reset();

      auto second = BagWriter::filter_get(bag.path.string());
      REQUIRE(second != nullptr);

      second->quit();
      REQUIRE(second->wait_for_quit(3000));
    }
  }

  TEST_CASE("global writer is initialized from environment in a child process") {
    const auto child_case = Utils::get_env("VLINK_BAG_WRITER_GLOBAL_CHILD");

    if (!child_case.empty()) {
      auto* writer = BagWriter::global_get();

      if (child_case == "unsupported") {
        CHECK_EQ(writer, nullptr);
        return;
      }

      REQUIRE_NE(writer, nullptr);
      Bytes data = Bytes::from_string("global payload");
      CHECK_GE(writer->push(
                   write_frame("dds://coverage/global_writer", "raw", SchemaType::kRaw, ActionType::kPublish, data)),
               0);
      writer->quit();
      CHECK(writer->wait_for_quit(3000));
      return;
    }

    const auto dir =
        std::filesystem::path(Utils::get_tmp_dir()) / "vlink-bag-writer-global-tests" / Utils::get_pid_str();
    std::filesystem::create_directories(dir);

    auto run_child = [](const std::filesystem::path& bag_path, const std::string& child_value) {
      Process child;
      child.set_process_mode(Process::kForwardedMode);
      child.set_inherit_environment(true);
      child.set_environment({{"VLINK_BAG_WRITER_GLOBAL_CHILD", child_value}, {"VLINK_BAG_PATH", bag_path.string()}});
      child.start(Utils::get_app_path(),
                  {"--test-suite=extension-BagWriter",
                   "--test-case=global writer is initialized from environment in a child process", "--no-version"});
      REQUIRE(child.wait_for_finished(Process::kDefaultExecuteTimeoutMs));
      CHECK_EQ(child.get_exit_code(), 0);
    };

    run_child(dir / ("global_" + Utils::get_pid_str() + ".vdb"), "vdb");
    run_child(dir / ("global_" + Utils::get_pid_str() + ".unsupported"), "unsupported");
  }

  TEST_CASE("vcap writer with zero cache size disables chunking and remains readable") {
    ScopedWriterPath bag(".vcap");

    BagWriter::Config config;
    config.sync_mode = true;
    config.cache_size = 0;
    config.compress = BagWriter::kCompressZstd;
    config.tag_name = "no-chunk";
    config.start_timestamp = 1'700'000'500'000LL;

    auto writer = BagWriter::create(bag.path.string(), config);
    REQUIRE(writer != nullptr);

    Bytes data = Bytes::from_string("no chunk payload");
    REQUIRE_EQ(
        writer->push(write_frame("dds://coverage/no_chunk", "raw", SchemaType::kRaw, ActionType::kPublish, data)), 0);
    writer.reset();

    auto reader = BagReader::create(bag.path.string(), false);
    REQUIRE(reader != nullptr);
    REQUIRE(reader->async_run());
    REQUIRE(reader->open_cursor());

    Frame frame;
    REQUIRE(reader->read_next(frame));
    CHECK_EQ(frame.url, "dds://coverage/no_chunk");
    CHECK_EQ(frame.data.to_string(), "no chunk payload");
    CHECK_FALSE(reader->read_next(frame));
    CHECK(reader->eof());

    reader->quit();
    REQUIRE(reader->wait_for_quit(3000));
  }

  TEST_CASE("async memory limit rejects oversized queued frames before enqueue") {
    verify_async_memory_limit_rejects_before_enqueue(".vdb");
    verify_async_memory_limit_rejects_before_enqueue(".vcap");
  }

  TEST_CASE("persistent writer queues reject overflow without dropping accepted frames") {
    verify_persistent_queue_rejects_without_dropping(".vdb");
    verify_persistent_queue_rejects_without_dropping(".vcap");
  }

  TEST_CASE("real writer config variants remain readable") {
    verify_real_writer_config_variants(".vdb");
    verify_real_writer_config_variants(".vcap");
  }

  TEST_CASE("split writer constructors remove stale files listed by an old manifest") {
    verify_split_manifest_constructor_removes_stale_file(".vdbx", ".vdb");
    verify_split_manifest_constructor_removes_stale_file(".vcapx", ".vcap");
  }

  TEST_CASE("split writer constructors reject unsafe paths from an old manifest") {
    verify_split_manifest_rejects_unsafe_paths(".vdbx");
    verify_split_manifest_rejects_unsafe_paths(".vcapx");
  }

  TEST_CASE("vcap compression level variants remain readable") { verify_vcap_compression_level_variants(); }

  TEST_CASE("vdb skips compression after repeated unhelpful frames") {
    verify_vdb_compression_skip_after_repeated_unhelpful_frames();
  }

  TEST_CASE("vdb periodically updates metadata during a long sync-mode write") {
    verify_vdb_periodic_metadata_update_branch();
  }

  TEST_CASE("vdb wal optimize and async schema paths remain readable") {
    verify_vdb_wal_optimize_and_async_schema_paths();
  }

  TEST_CASE("vdb schema merge and failure variants remain deterministic") {
    verify_vdb_schema_merge_and_failure_variants();
  }

  TEST_CASE("vcap schema merge and failure variants remain deterministic") {
    verify_vcap_schema_merge_and_failure_variants();
  }

  TEST_CASE("vcap can retry a new url after schema resolution fails") { verify_vcap_schema_failure_can_retry(); }

  TEST_CASE("real writers reject url ser and schema changes for an existing stream") {
    verify_real_writer_rejects_url_metadata_conflicts(".vdb");
    verify_real_writer_rejects_url_metadata_conflicts(".vcap");
  }

  TEST_CASE("real writers normalize timestamps and persist unknown action metadata") {
    verify_real_writer_timestamp_and_unknown_action_variants(".vdb");
    verify_real_writer_timestamp_and_unknown_action_variants(".vcap");
  }

  TEST_CASE("real writers create missing parent directories under the temp root") {
    verify_writer_creates_missing_parent_directory(".vdb");
    verify_writer_creates_missing_parent_directory(".vcap");
  }

  TEST_CASE("real writers auto compression mode remains readable") {
    verify_auto_compression_writer_remains_readable(".vdb");
    verify_auto_compression_writer_remains_readable(".vcap");
  }

  TEST_CASE("real writers asynchronous record path remains readable") {
    verify_async_writer_path_remains_readable(".vdb");
    verify_async_writer_path_remains_readable(".vcap");
  }

  TEST_CASE("vdb limit modes reject or evict deterministically") { verify_vdb_limit_mode_variants(); }

  TEST_CASE("vdb compressed byte limits evict using raw payload sizes") {
    verify_vdb_compressed_byte_limit_eviction(".vdb");
    verify_vdb_compressed_byte_limit_eviction(".vdbx");
  }

  TEST_CASE("vdb precompressed byte limits keep eviction accounting reversible") {
    verify_vdb_precompressed_byte_limit_eviction(".vdb");
    verify_vdb_precompressed_byte_limit_eviction(".vdbx");
  }

  TEST_CASE("split writers rotate by size and keep manifests readable") {
    verify_split_by_size_writer_paths(".vdbx");
    verify_split_by_size_writer_paths(".vcapx");
  }

  TEST_CASE("explicit close finalizes split manifests before destruction") {
    verify_split_close_finalizes_manifest(".vdbx");
    verify_split_close_finalizes_manifest(".vcapx");
  }

  TEST_CASE("real writers persist method schema split and field metadata") {
    verify_method_schema_split_and_field_metadata(".vdb");
    verify_method_schema_split_and_field_metadata(".vcap");
  }

  TEST_CASE("real writers honor ignored compression urls") {
    verify_ignore_compress_url_remains_readable(".vdb");
    verify_ignore_compress_url_remains_readable(".vcap");
  }

  TEST_CASE("relative split manifests stay readable for time-named vdb and vcap files") {
    verify_relative_split_writer_paths(".vdbx", false);
    verify_relative_split_writer_paths(".vdbx", true);
    verify_relative_split_writer_paths(".vcapx", false);
    verify_relative_split_writer_paths(".vcapx", true);
  }

  TEST_CASE("relative vcap close remains stable after the process changes directory") {
    verify_relative_vcap_close_survives_chdir();
  }

  TEST_CASE("relative split close keeps the manifest in its original directory") {
    verify_relative_split_close_survives_chdir(".vdbx");
    verify_relative_split_close_survives_chdir(".vcapx");
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
