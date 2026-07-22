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
#include <vlink/base/condition_variable.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../common_test.h"
#include "./base/process.h"
#include "./base/utils.h"
#include "./extension/bag_plugin_interface.h"
#include "./extension/bag_processor.h"
#include "./extension/bag_writer.h"

#if defined(__SANITIZE_ADDRESS__)
#define VLINK_TEST_ADDRESS_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define VLINK_TEST_ADDRESS_SANITIZER 1
#endif
#endif

#ifndef VLINK_TEST_ADDRESS_SANITIZER
#define VLINK_TEST_ADDRESS_SANITIZER 0
#endif

#if !VLINK_TEST_ADDRESS_SANITIZER
namespace {

class StubBagReader : public BagReader {
 public:
  explicit StubBagReader(const std::string& path = (std::filesystem::path(Utils::get_tmp_dir()) / "stub.vdb").string())
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

  void on_write(const Frame& frame) override { do_callback(frame); }
};

class ReadMetaPlugin final : public BagPluginInterface {
 public:
  bool convert_url_meta(std::string& url, std::string& ser_type, SchemaType& schema_type) override {
    if (url == "intra://meta-old") {
      url = "intra://meta-new";
      ser_type = "demo.Converted";
      schema_type = SchemaType::kFlatbuffers;
    }

    return true;
  }

  void on_read(const Frame& frame) override {
    observed_url = frame.url;
    observed_ser = frame.ser_type;
    observed_schema = frame.schema_type;
    do_callback(frame);
  }

  void on_write(const Frame& frame) override { do_callback(frame); }

  std::string observed_url;
  std::string observed_ser;
  SchemaType observed_schema{SchemaType::kUnknown};
};

class RewriteReadUrlPlugin final : public BagPluginInterface {
 public:
  explicit RewriteReadUrlPlugin(bool clear_meta) : clear_meta_(clear_meta) {}

  void on_read(const Frame& frame) override {
    Frame out = frame;
    out.url = "intra://type-b";

    if (clear_meta_) {
      out.ser_type.clear();
      out.schema_type = SchemaType::kUnknown;
    }

    do_callback(out);
  }

  void on_write(const Frame& frame) override { do_callback(frame); }

 private:
  bool clear_meta_{false};
};

class ReorderReadPlugin final : public BagPluginInterface {
 public:
  explicit ReorderReadPlugin(int64_t min_cache_time, int blocked_input_count = 0)
      : processor_(make_config(min_cache_time)), blocked_input_count_(blocked_input_count) {
    processor_.register_output_callback([this](const Frame& frame) { do_callback(frame); });
  }

  ~ReorderReadPlugin() override = default;

  void on_read(const Frame& frame) override {
    const int input_count = input_count_.fetch_add(1, std::memory_order_relaxed) + 1;

    int64_t data_timestamp = 0;
    std::memcpy(&data_timestamp, frame.data.data(), sizeof(int64_t));

    Frame out = frame;
    out.timestamp = data_timestamp;
    processor_.push(data_timestamp, out);

    if (input_count == blocked_input_count_) {
      std::unique_lock lock(block_mtx_);
      input_blocked_ = true;
      block_cv_.notify_all();
      block_cv_.wait_for(lock, 3s, [this] { return block_released_; });
      input_blocked_ = false;
    }
  }

  void on_write(const Frame& frame) override { do_callback(frame); }

  void on_reset() override { processor_.reset(); }

  void flush() override {
    flush_count_.fetch_add(1, std::memory_order_relaxed);
    processor_.flush();
  }

  [[nodiscard]] int flush_count() const noexcept { return flush_count_.load(std::memory_order_relaxed); }

  [[nodiscard]] int input_count() const noexcept { return input_count_.load(std::memory_order_relaxed); }

  [[nodiscard]] bool wait_for_input_blocked() {
    std::unique_lock lock(block_mtx_);
    return block_cv_.wait_for(lock, 3s, [this] { return input_blocked_; });
  }

  void release_input_block() {
    {
      std::lock_guard lock(block_mtx_);
      block_released_ = true;
    }

    block_cv_.notify_all();
  }

 private:
  static BagProcessor::Config make_config(int64_t min_cache_time) {
    BagProcessor::Config config;
    config.min_cache_time = min_cache_time;
    return config;
  }

  BagProcessor processor_;
  int blocked_input_count_{0};
  std::atomic<int> flush_count_{0};
  std::atomic<int> input_count_{0};
  std::mutex block_mtx_;
  vlink::ConditionVariable block_cv_;
  bool input_blocked_{false};
  bool block_released_{false};
};

class CoverageReaderPlugin final : public BagPluginInterface {
 public:
  bool convert_url_meta(std::string& url, std::string& ser_type, SchemaType& schema_type) override {
    (void)ser_type;
    (void)schema_type;

    if (url == "dds://coverage/event") {
      url = "dds://coverage/event_remapped";
    }

    return true;
  }

  void on_read(const Frame& frame) override { do_callback(frame); }

  void on_write(const Frame& frame) override { do_callback(frame); }
};

class DropCursorUrlPlugin final : public BagPluginInterface {
 public:
  bool convert_url_meta(std::string& url, std::string& ser_type, SchemaType& schema_type) override {
    (void)ser_type;
    (void)schema_type;

    if (url == "dds://coverage/drop_cursor") {
      return false;
    }

    if (url == "dds://coverage/keep_cursor") {
      url = "dds://coverage/keep_cursor_remapped";
    }

    return true;
  }

  void on_read(const Frame& frame) override { do_callback(frame); }

  void on_write(const Frame& frame) override { do_callback(frame); }
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

std::filesystem::path make_extension_tmp_path(const char* suffix) {
  const auto root =
      std::filesystem::path(Utils::get_tmp_dir()) / "vlink-extension-tests" / "bag-reader" / Utils::get_pid_str();
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto dir = root / ("bag_roundtrip_" + Utils::get_pid_str() + "_" + std::to_string(stamp));
  std::filesystem::create_directories(dir);

  return dir / (std::string("bag") + suffix);
}

std::string lowercase_extension(std::filesystem::path path) {
  std::string suffix = path.extension().string();
  std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return std::tolower(c); });
  return suffix;
}

std::filesystem::path find_sqlite3_cli() {
  const char* path_env = std::getenv("PATH");
  if (path_env == nullptr) {
    return {};
  }

#ifdef _WIN32
  constexpr char kPathSep = ';';
  const std::vector<std::string> candidates{"sqlite3.exe", "sqlite3"};
#else
  constexpr char kPathSep = ':';
  const std::vector<std::string> candidates{"sqlite3"};
#endif

  std::string paths(path_env);
  size_t start = 0;

  while (start <= paths.size()) {
    size_t end = paths.find(kPathSep, start);
    if (end == std::string::npos) {
      end = paths.size();
    }

    const std::string dir = paths.substr(start, end - start);
    if (!dir.empty()) {
      for (const auto& name : candidates) {
        std::error_code ec;
        std::filesystem::path candidate = std::filesystem::path(dir) / name;
        if (std::filesystem::exists(candidate, ec) && !ec) {
          return candidate;
        }
      }
    }

    if (end == paths.size()) {
      break;
    }
    start = end + 1;
  }

  return {};
}

bool run_sqlite3_sql(const std::filesystem::path& db_path, const std::string& sql) {
  const auto sqlite3 = find_sqlite3_cli();
  if (sqlite3.empty()) {
    MESSAGE("sqlite3 CLI not found in PATH; skipping optional VDB corruption branch coverage case");
    return false;
  }

  return Process::execute(sqlite3.string(), {db_path.string(), sql}, 10000) == 0;
}

void remove_bag_family(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);

  const std::string suffix = lowercase_extension(path);
  if (suffix != ".vdbx" && suffix != ".vcapx") {
    return;
  }

  const auto parent = path.parent_path();
  if (parent.empty()) {
    return;
  }

  const std::string split_suffix = suffix == ".vdbx" ? ".vdb" : ".vcap";
  const std::string split_prefix = path.stem().string() + ".";

  for (const auto& entry : std::filesystem::directory_iterator(parent, ec)) {
    if (ec) {
      return;
    }

    const auto candidate = entry.path();
    const std::string filename = candidate.filename().string();
    if (filename.rfind(split_prefix, 0) == 0 && lowercase_extension(candidate) == split_suffix) {
      std::filesystem::remove(candidate, ec);
    }
  }
}

struct ScopedBagPath final {
  explicit ScopedBagPath(const char* suffix) : path(make_extension_tmp_path(suffix)) { remove_bag_family(path); }

  ~ScopedBagPath() {
    remove_bag_family(path);
    std::error_code ec;
    std::filesystem::remove_all(path.parent_path(), ec);
  }

  std::filesystem::path path;
};

struct ScopedTempDir final {
  explicit ScopedTempDir(const char* prefix) {
    const auto root =
        std::filesystem::path(Utils::get_tmp_dir()) / "vlink-extension-tests" / "bag-reader" / Utils::get_pid_str();
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path = root / (std::string(prefix) + Utils::get_pid_str() + "_" + std::to_string(stamp));
    std::filesystem::create_directories(path);
  }

  ~ScopedTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }

  std::filesystem::path path;
};

Frame bag_frame(int64_t timestamp, const std::string& url, const std::string& ser_type, SchemaType schema_type,
                ActionType action_type, const std::string& payload) {
  Frame frame;
  frame.timestamp = timestamp;
  frame.url = url;
  frame.ser_type = ser_type;
  frame.schema_type = schema_type;
  frame.action_type = action_type;
  frame.data = Bytes::from_string(payload);
  return frame;
}

std::string repeated_payload(size_t size, char value) { return std::string(size, value); }

SchemaData make_schema_data(const std::string& name, SchemaType schema_type, const std::string& data) {
  SchemaData schema;
  schema.name = name;
  schema.schema_type = schema_type;
  schema.encoding = std::string(SchemaData::convert_type(schema_type));
  schema.data = Bytes::from_string(data);
  return schema;
}

struct ObservedFrame final {
  int64_t timestamp{0};
  std::string url;
  std::string ser_type;
  SchemaType schema_type{SchemaType::kUnknown};
  ActionType action_type{ActionType::kUnknownAction};
  std::string payload;
};

std::vector<ObservedFrame> read_all_frames(BagReader& reader) {
  std::vector<ObservedFrame> frames;
  Frame frame;

  while (reader.read_next(frame)) {
    frames.push_back(
        {frame.timestamp, frame.url, frame.ser_type, frame.schema_type, frame.action_type, frame.data.to_string()});
  }

  return frames;
}

void write_roundtrip_bag(const std::filesystem::path& path) {
  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.tag_name = "coverage";
  config.start_timestamp = 1'700'000'000'000LL;

  auto writer = BagWriter::create(path.string(), config);
  REQUIRE(writer != nullptr);
  CHECK_FALSE(writer->is_dumping());
  CHECK_FALSE(writer->is_split_mode());
  CHECK_EQ(writer->get_split_index(), 0);

  SchemaData schema;
  schema.name = "demo.Message";
  schema.schema_type = SchemaType::kProtobuf;
  schema.encoding = std::string(SchemaData::convert_type(SchemaType::kProtobuf));
  schema.data = Bytes::from_string("syntax = \"proto3\"; message Message {}");
  REQUIRE(writer->push_schema(schema));

  writer->set_url_loss("dds://coverage/event", 0.25);

  REQUIRE_EQ(writer->push(bag_frame(1'000, "dds://coverage/event", "demo.Message", SchemaType::kProtobuf,
                                    ActionType::kPublish, "event-payload")),
             1'000);
  REQUIRE_EQ(writer->push(
                 bag_frame(2'000, "dds://coverage/field", "raw", SchemaType::kRaw, ActionType::kSet, "field-payload")),
             2'000);
  REQUIRE_EQ(writer->push(bag_frame(3'000, "dds://coverage/method", "demo.Request|demo.Response", SchemaType::kUnknown,
                                    ActionType::kClientRequest, "request-payload")),
             3'000);

  writer.reset();

  REQUIRE(std::filesystem::exists(path));
  CHECK_GT(std::filesystem::file_size(path), 0u);
}

void write_empty_bag(const std::filesystem::path& path) {
  BagWriter::Config config;
  config.compress = BagWriter::kCompressNone;
  config.tag_name = "empty";
  config.start_timestamp = 1'700'000'450'000LL;

  auto writer = BagWriter::create(path.string(), config);
  REQUIRE(writer != nullptr);
  writer.reset();

  REQUIRE(std::filesystem::exists(path));
  CHECK_GT(std::filesystem::file_size(path), 0u);
}

void write_async_compressed_bag(const std::filesystem::path& path, BagWriter::CompressType compress) {
  BagWriter::Config config;
  config.compress = compress;
  config.compress_start_size = 1;
  config.compress_level = 4;
  config.cache_size = 256;
  config.ignore_compress_urls.emplace("dds://coverage/compress_ignored");
  config.tag_name = "compressed";
  config.start_timestamp = 1'700'000'100'000LL;
  config.max_task_depth = 8;

  auto writer = BagWriter::create(path.string(), config);
  REQUIRE(writer != nullptr);
  REQUIRE(writer->async_run());

  writer->register_schema_callback([](const std::string& ser_type, SchemaType schema_type) {
    return make_schema_data(ser_type, schema_type, "syntax = \"proto3\"; message Callback {}");
  });

  REQUIRE(writer->push_schema(
      make_schema_data("demo.Async", SchemaType::kProtobuf, "syntax = \"proto3\"; message Async {}")));

  CHECK_GE(writer->push(bag_frame(1'000, "dds://coverage/compress", "demo.Async", SchemaType::kProtobuf,
                                  ActionType::kPublish, repeated_payload(2048, 'A'))),
           0);
  CHECK_GE(writer->push(bag_frame(2'000, "dds://coverage/compress_ignored", "demo.Callback", SchemaType::kProtobuf,
                                  ActionType::kSet, repeated_payload(256, 'B'))),
           0);

  REQUIRE(writer->wait_for_idle(3000));
  writer.reset();

  REQUIRE(std::filesystem::exists(path));
  CHECK_GT(std::filesystem::file_size(path), 0u);
}

void write_busy_bag(const std::filesystem::path& path) {
  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.tag_name = "busy";
  config.start_timestamp = 1'700'000'250'000LL;

  auto writer = BagWriter::create(path.string(), config);
  REQUIRE(writer != nullptr);

  for (int index = 0; index < 8; ++index) {
    REQUIRE_EQ(writer->push(bag_frame(index * 1'000, "dds://coverage/busy", "raw", SchemaType::kRaw,
                                      ActionType::kPublish, "busy-" + std::to_string(index))),
               index * 1'000);
  }

  writer.reset();

  REQUIRE(std::filesystem::exists(path));
  CHECK_GT(std::filesystem::file_size(path), 0u);
}

void write_split_bag(const std::filesystem::path& path, bool split_before) {
  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.tag_name = "split";
  config.split_by_size = 256;
  config.start_timestamp = 1'700'000'200'000LL;

  auto writer = BagWriter::create(path.string(), config);
  REQUIRE(writer != nullptr);
  CHECK(writer->is_split_mode());
  CHECK_EQ(writer->get_split_index(), 0);

  std::atomic<int> split_count{0};
  writer->register_split_callback(
      [&](int split_index, const std::string& split_filename) {
        CHECK_GE(split_index, 0);
        CHECK_FALSE(split_filename.empty());
        split_count.fetch_add(1, std::memory_order_relaxed);
      },
      split_before);

  REQUIRE_EQ(writer->push(bag_frame(1'000, "dds://coverage/split", "raw", SchemaType::kRaw, ActionType::kPublish,
                                    repeated_payload(512, 'S'))),
             1'000);
  REQUIRE_EQ(writer->push(bag_frame(2'000, "dds://coverage/split", "raw", SchemaType::kRaw, ActionType::kPublish,
                                    repeated_payload(512, 'T'))),
             2'000);

  CHECK_GE(writer->get_split_index(), 1);
  CHECK_GE(split_count.load(std::memory_order_relaxed), 1);
  writer.reset();

  REQUIRE(std::filesystem::exists(path));
  CHECK_GT(std::filesystem::file_size(path), 0u);
}

void write_minimal_split_manifest(const std::filesystem::path& path, const std::vector<std::string>& files) {
  nlohmann::ordered_json root;
  root["VLinkHeader"] = {
      {"major", 0},           {"minor", 0},        {"patch", 0},
      {"count", 0},           {"duration", 0},     {"accuracy", "MicroSecond"},
      {"compress", "None"},   {"process", "test"}, {"date", "1970/01/01 00:00:00"},
      {"tag", "manifest"},    {"complete", true},  {"timezone", 0},
      {"start_timestamp", 0},
  };
  root["VLinkUrls"] = nlohmann::ordered_json::array();
  root["VLinkFiles"] = files;

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  REQUIRE(out.is_open());
  out << root.dump(2);
}

void mutate_manifest_metadata(const std::filesystem::path& path) {
  nlohmann::ordered_json root;

  {
    std::ifstream in(path);
    REQUIRE(in.is_open());
    in >> root;
  }

  REQUIRE(root.contains("VLinkHeader"));
  REQUIRE(root.contains("VLinkUrls"));
  REQUIRE(root["VLinkUrls"].is_array());
  REQUIRE_FALSE(root["VLinkUrls"].empty());

  root["VLinkHeader"]["count"] = 999;
  root["VLinkHeader"]["duration"] = 0;
  root["VLinkUrls"][0]["loss"] = 2.0;
  root["VLinkUrls"][0]["freq"] = -1.0;

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  REQUIRE(out.is_open());
  out << root.dump(2);
}

void mutate_manifest_url_field(const std::filesystem::path& path, const std::string& field,
                               nlohmann::ordered_json value) {
  nlohmann::ordered_json root;

  {
    std::ifstream in(path);
    REQUIRE(in.is_open());
    in >> root;
  }

  REQUIRE(root.contains("VLinkUrls"));
  REQUIRE(root["VLinkUrls"].is_array());
  REQUIRE_FALSE(root["VLinkUrls"].empty());

  root["VLinkUrls"][0][field] = std::move(value);

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  REQUIRE(out.is_open());
  out << root.dump(2);
}

void mutate_manifest_header_field(const std::filesystem::path& path, const std::string& field,
                                  nlohmann::ordered_json value) {
  nlohmann::ordered_json root;

  {
    std::ifstream in(path);
    REQUIRE(in.is_open());
    in >> root;
  }

  REQUIRE(root.contains("VLinkHeader"));
  root["VLinkHeader"][field] = std::move(value);

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  REQUIRE(out.is_open());
  out << root.dump(2);
}

void remove_manifest_header_fields(const std::filesystem::path& path, const std::vector<std::string>& fields) {
  nlohmann::ordered_json root;

  {
    std::ifstream in(path);
    REQUIRE(in.is_open());
    in >> root;
  }

  REQUIRE(root.contains("VLinkHeader"));
  for (const auto& field : fields) {
    root["VLinkHeader"].erase(field);
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  REQUIRE(out.is_open());
  out << root.dump(2);
}

void remove_manifest_url_fields(const std::filesystem::path& path, const std::vector<std::string>& fields) {
  nlohmann::ordered_json root;

  {
    std::ifstream in(path);
    REQUIRE(in.is_open());
    in >> root;
  }

  REQUIRE(root.contains("VLinkUrls"));
  REQUIRE(root["VLinkUrls"].is_array());
  REQUIRE_FALSE(root["VLinkUrls"].empty());

  for (const auto& field : fields) {
    root["VLinkUrls"][0].erase(field);
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  REQUIRE(out.is_open());
  out << root.dump(2);
}

void replace_manifest_url_list(const std::filesystem::path& path, nlohmann::ordered_json urls) {
  nlohmann::ordered_json root;

  {
    std::ifstream in(path);
    REQUIRE(in.is_open());
    in >> root;
  }

  root["VLinkUrls"] = std::move(urls);

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  REQUIRE(out.is_open());
  out << root.dump(2);
}

void mutate_manifest_accuracy(const std::filesystem::path& path) {
  nlohmann::ordered_json root;

  {
    std::ifstream in(path);
    REQUIRE(in.is_open());
    in >> root;
  }

  REQUIRE(root.contains("VLinkHeader"));
  root["VLinkHeader"]["accuracy"] = "NanoSecond";

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  REQUIRE(out.is_open());
  out << root.dump(2);
}

std::string read_manifest_tag(const std::filesystem::path& path) {
  nlohmann::ordered_json root;
  std::ifstream in(path);
  REQUIRE(in.is_open());
  in >> root;

  REQUIRE(root.contains("VLinkHeader"));
  REQUIRE(root["VLinkHeader"].contains("tag"));
  return root["VLinkHeader"]["tag"].get<std::string>();
}

void overwrite_file(const std::filesystem::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  REQUIRE(out.is_open());
  out << content;
}

size_t replace_file_bytes(const std::filesystem::path& path, const std::string& from, const std::string& to,
                          bool replace_all) {
  REQUIRE_EQ(from.size(), to.size());

  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.is_open());

  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  size_t count = 0;
  size_t pos = 0;

  while ((pos = content.find(from, pos)) != std::string::npos) {
    content.replace(pos, from.size(), to);
    pos += to.size();
    ++count;

    if (!replace_all) {
      break;
    }
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  REQUIRE(out.is_open());
  out.write(content.data(), static_cast<std::streamsize>(content.size()));

  return count;
}

void verify_invalid_single_bag_is_not_cursor_readable(const char* suffix) {
  ScopedBagPath bag(suffix);
  {
    std::ofstream out(bag.path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out << "not a vlink bag";
  }

  try {
    auto reader = BagReader::create(bag.path.string(), true);
    REQUIRE(reader != nullptr);

    Frame frame;
    CHECK_FALSE(reader->open_cursor());
    CHECK(reader->fail());
    CHECK_FALSE(reader->read_next(frame));
  } catch (const std::exception&) {
    CHECK(true);
  }
}

void verify_missing_single_bag_is_not_cursor_readable(const char* suffix) {
  ScopedBagPath bag(suffix);
  remove_bag_family(bag.path);

  CHECK_THROWS((void)BagReader::create(bag.path.string(), true));
}

void verify_split_manifest_without_readable_files_is_not_cursor_readable(const char* suffix, const char* split_suffix) {
  ScopedBagPath bag(suffix);

  write_minimal_split_manifest(bag.path, {});
  CHECK_THROWS((void)BagReader::create(bag.path.string(), true));

  write_minimal_split_manifest(bag.path, {std::string("missing") + split_suffix});
  CHECK_THROWS((void)BagReader::create(bag.path.string(), true));
}

void verify_malformed_split_manifest_is_not_cursor_readable(const char* suffix) {
  ScopedBagPath bag(suffix);
  {
    std::ofstream out(bag.path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out << "{ this is not valid json";
  }

  CHECK_THROWS((void)BagReader::create(bag.path.string(), true));
}

void verify_split_manifest_check_rejects_inconsistent_metadata(const char* suffix, bool split_before) {
  ScopedBagPath bag(suffix);
  write_split_bag(bag.path, split_before);
  mutate_manifest_metadata(bag.path);

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  CHECK(reader->is_split_mode());
  CHECK_FALSE(reader->check().get());

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_split_manifest_check_rejects_invalid_url_metadata(const char* suffix, bool split_before) {
  for (const auto& mutation : std::vector<std::pair<std::string, nlohmann::ordered_json>>{
           {"url", ""},
           {"type", ""},
           {"ser", ""},
           {"loss", -2.0},
           {"freq", -1.0},
       }) {
    ScopedBagPath bag(suffix);
    write_split_bag(bag.path, split_before);
    mutate_manifest_url_field(bag.path, mutation.first, mutation.second);

    auto reader = BagReader::create(bag.path.string(), false);
    REQUIRE(reader != nullptr);
    REQUIRE(reader->async_run());
    CHECK_FALSE(reader->check().get());

    reader->quit();
    REQUIRE(reader->wait_for_quit(3000));
  }
}

void verify_unsupported_split_manifest_accuracy_throws(const char* suffix, bool split_before) {
  ScopedBagPath bag(suffix);
  write_split_bag(bag.path, split_before);
  mutate_manifest_accuracy(bag.path);

  CHECK_THROWS((void)BagReader::create(bag.path.string(), true));
}

void verify_split_manifest_optional_fields_have_defaults(const char* suffix, bool split_before) {
  ScopedBagPath bag(suffix);
  write_split_bag(bag.path, split_before);

  remove_manifest_header_fields(bag.path,
                                {"start_timestamp", "tag", "complete", "timezone", "split_by_size", "split_by_time"});
  remove_manifest_url_fields(bag.path, {"action", "size", "freq"});

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  const auto& info = reader->get_info();
  CHECK_EQ(info.tag_name, "Empty");
  CHECK(info.has_completed);
  CHECK_EQ(info.timezone, 480);
  REQUIRE_FALSE(info.url_metas.empty());
  CHECK_EQ(info.url_metas.front().action_type, ActionType::kUnknownAction);
  CHECK_EQ(info.url_metas.front().size, 0U);
  CHECK_EQ(info.url_metas.front().freq, 0.0);
  CHECK(reader->check().get());

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_split_manifest_non_default_compression_metadata(const char* suffix, bool split_before) {
  ScopedBagPath bag(suffix);
  write_split_bag(bag.path, split_before);
  mutate_manifest_header_field(bag.path, "compress", "Zstd");

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  CHECK_EQ(reader->get_info().compression_type, "Zstd");
  CHECK(reader->check().get());

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_split_manifest_none_compression_aliases(const char* suffix, bool split_before) {
  for (const auto& compress : {"NONE", "none"}) {
    ScopedBagPath bag(suffix);
    write_split_bag(bag.path, split_before);
    mutate_manifest_header_field(bag.path, "compress", compress);

    auto reader = BagReader::create(bag.path.string(), false);
    REQUIRE(reader != nullptr);
    REQUIRE(reader->async_run());

    CHECK_EQ(reader->get_info().compression_type, compress);
    CHECK(reader->check().get());

    reader->quit();
    REQUIRE(reader->wait_for_quit(3000));
  }
}

void verify_split_manifest_negative_start_timestamp_is_normalized(const char* suffix, bool split_before) {
  ScopedBagPath bag(suffix);
  write_split_bag(bag.path, split_before);
  mutate_manifest_header_field(bag.path, "start_timestamp", -1);

  auto reader = BagReader::create(bag.path.string(), true);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  CHECK_EQ(reader->get_info().start_timestamp, 0);
  CHECK(reader->check().get());

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_split_manifest_missing_schema_encoding_is_rejected(const char* suffix, bool split_before) {
  ScopedBagPath bag(suffix);
  write_split_bag(bag.path, split_before);
  remove_manifest_url_fields(bag.path, {"encoding"});

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  REQUIRE_FALSE(reader->get_info().url_metas.empty());
  CHECK_EQ(reader->get_info().url_metas.front().schema_type, SchemaType::kUnknown);
  CHECK_FALSE(reader->check().get());

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_split_manifest_incomplete_header_is_reported(const char* suffix, bool split_before) {
  ScopedBagPath bag(suffix);
  write_split_bag(bag.path, split_before);
  mutate_manifest_header_field(bag.path, "complete", false);

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  CHECK_FALSE(reader->get_info().has_completed);

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_split_manifest_missing_url_metadata_fails_check(const char* suffix, bool split_before) {
  ScopedBagPath bag(suffix);
  write_split_bag(bag.path, split_before);
  replace_manifest_url_list(bag.path, nlohmann::ordered_json::array());
  mutate_manifest_header_field(bag.path, "count", 2);

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  CHECK_EQ(reader->get_info().message_count, 2);
  CHECK(reader->get_info().url_metas.empty());
  CHECK_FALSE(reader->check().get());

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_real_reader_plugin_rebind_updates_metadata(const char* suffix) {
  ScopedBagPath bag(suffix);
  write_roundtrip_bag(bag.path);

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  CHECK_EQ(reader->get_ser_type("dds://coverage/event"), "demo.Message");

  auto plugin = std::make_shared<CoverageReaderPlugin>();
  reader->bind_bag_interface(plugin);
  CHECK_EQ(plugin->get_direction(), BagPluginInterface::Direction::kRead);
  CHECK_EQ(reader->get_ser_type("dds://coverage/event_remapped"), "demo.Message");
  CHECK_EQ(reader->get_schema_type("dds://coverage/event_remapped"), SchemaType::kProtobuf);
  CHECK_EQ(reader->get_ser_type("dds://coverage/event"), "");

  BagReader::Config remapped_filter;
  remapped_filter.filter_urls.emplace("dds://coverage/event_remapped");
  REQUIRE(reader->open_cursor(remapped_filter));
  const auto frames = read_all_frames(*reader);
  REQUIRE_EQ(frames.size(), 1u);
  CHECK_EQ(frames.front().url, "dds://coverage/event_remapped");
  CHECK_EQ(frames.front().payload, "event-payload");

  reader->bind_bag_interface(nullptr);
  CHECK_EQ(reader->get_ser_type("dds://coverage/event"), "demo.Message");
  CHECK_EQ(reader->get_ser_type("dds://coverage/event_remapped"), "");
}

void verify_vcap_cursor_plugin_excludes_and_remaps_urls() {
  ScopedBagPath bag(".vcap");

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.tag_name = "cursor-plugin";
  config.start_timestamp = 1'700'000'650'000LL;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);

  REQUIRE_EQ(writer->push(
                 bag_frame(1'000, "dds://coverage/drop_cursor", "raw", SchemaType::kRaw, ActionType::kPublish, "drop")),
             1'000);
  REQUIRE_EQ(writer->push(
                 bag_frame(2'000, "dds://coverage/keep_cursor", "raw", SchemaType::kRaw, ActionType::kPublish, "keep")),
             2'000);

  writer.reset();

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);

  reader->bind_bag_interface(std::make_shared<DropCursorUrlPlugin>());
  CHECK_EQ(reader->get_ser_type("dds://coverage/drop_cursor"), "");
  CHECK_EQ(reader->get_ser_type("dds://coverage/keep_cursor_remapped"), "raw");

  REQUIRE(reader->open_cursor());
  const auto frames = read_all_frames(*reader);

  REQUIRE_EQ(frames.size(), 1u);
  CHECK_EQ(frames[0].url, "dds://coverage/keep_cursor_remapped");
  CHECK_EQ(frames[0].payload, "keep");
}

void verify_vdb_cursor_plugin_excludes_and_remaps_urls() {
  ScopedBagPath bag(".vdb");

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.tag_name = "vdb-cursor-plugin";
  config.start_timestamp = 1'700'000'660'000LL;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);

  REQUIRE_EQ(writer->push(
                 bag_frame(1'000, "dds://coverage/drop_cursor", "raw", SchemaType::kRaw, ActionType::kPublish, "drop")),
             1'000);
  REQUIRE_EQ(writer->push(
                 bag_frame(2'000, "dds://coverage/keep_cursor", "raw", SchemaType::kRaw, ActionType::kPublish, "keep")),
             2'000);

  writer.reset();

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);

  reader->bind_bag_interface(std::make_shared<DropCursorUrlPlugin>());
  CHECK_EQ(reader->get_ser_type("dds://coverage/drop_cursor"), "");
  CHECK_EQ(reader->get_ser_type("dds://coverage/keep_cursor_remapped"), "raw");

  REQUIRE(reader->open_cursor());
  const auto frames = read_all_frames(*reader);

  REQUIRE_EQ(frames.size(), 1u);
  CHECK_EQ(frames[0].url, "dds://coverage/keep_cursor_remapped");
  CHECK_EQ(frames[0].payload, "keep");
}

void verify_split_cursor_filters_each_file(const char* suffix, bool split_before) {
  ScopedBagPath bag(suffix);

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.tag_name = "split-cursor-filter";
  config.split_by_size = 256;
  config.start_timestamp = 1'700'000'670'000LL;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);
  REQUIRE(writer->is_split_mode());
  writer->register_split_callback(
      [](int split_index, const std::string& split_filename) {
        CHECK_GE(split_index, 0);
        CHECK_FALSE(split_filename.empty());
      },
      split_before);

  REQUIRE_EQ(writer->push(bag_frame(1'000, "dds://coverage/split_keep", "raw", SchemaType::kRaw, ActionType::kPublish,
                                    repeated_payload(512, 'A'))),
             1'000);
  REQUIRE_EQ(writer->push(bag_frame(2'000, "dds://coverage/split_other", "raw", SchemaType::kRaw, ActionType::kPublish,
                                    repeated_payload(512, 'B'))),
             2'000);
  REQUIRE_EQ(writer->push(bag_frame(3'000, "dds://coverage/split_keep", "raw", SchemaType::kRaw, ActionType::kPublish,
                                    repeated_payload(512, 'C'))),
             3'000);
  CHECK_GE(writer->get_split_index(), 1);
  writer.reset();

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());
  CHECK(reader->is_split_mode());
  CHECK(reader->check().get());

  BagReader::Config keep_filter;
  keep_filter.filter_urls.emplace("dds://coverage/split_keep");
  REQUIRE(reader->open_cursor(keep_filter));
  auto frames = read_all_frames(*reader);
  REQUIRE_EQ(frames.size(), 2u);
  CHECK_EQ(frames[0].url, "dds://coverage/split_keep");
  CHECK_EQ(frames[0].payload, repeated_payload(512, 'A'));
  CHECK_EQ(frames[1].url, "dds://coverage/split_keep");
  CHECK_EQ(frames[1].payload, repeated_payload(512, 'C'));

  BagReader::Config missing_filter;
  missing_filter.begin_time = 2;
  missing_filter.filter_urls.emplace("dds://coverage/split_missing");
  REQUIRE(reader->open_cursor(missing_filter));
  frames = read_all_frames(*reader);
  CHECK(frames.empty());
  CHECK(reader->eof());

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_reader_playback_loops_and_auto_quit(const char* suffix) {
  ScopedBagPath bag(suffix);
  write_roundtrip_bag(bag.path);

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);

  std::atomic<int> ready_count{0};
  std::atomic<int> finish_count{0};
  std::atomic<int> output_count{0};
  reader->register_ready_callback([&] { ready_count.fetch_add(1, std::memory_order_relaxed); });
  reader->register_finish_callback([&](bool interrupted) {
    CHECK_FALSE(interrupted);
    finish_count.fetch_add(1, std::memory_order_relaxed);
  });
  reader->register_output_callback([&](const Frame&) { output_count.fetch_add(1, std::memory_order_relaxed); });

  REQUIRE(reader->async_run());

  BagReader::Config loop_config;
  loop_config.force_delay = 0;
  loop_config.times = 2;
  reader->play(loop_config);

  REQUIRE(common_test::wait_until([&] { return finish_count.load(std::memory_order_acquire) >= 1; }, 3s));
  CHECK_GE(ready_count.load(std::memory_order_acquire), 2);
  CHECK_GE(output_count.load(std::memory_order_acquire), 6);

  const int finish_before_auto_quit = finish_count.load(std::memory_order_acquire);
  BagReader::Config auto_quit_config;
  auto_quit_config.force_delay = 0;
  auto_quit_config.auto_quit = true;
  auto_quit_config.filter_urls.emplace("dds://coverage/missing");
  reader->play(auto_quit_config);

  REQUIRE(reader->wait_for_quit(3000));
  CHECK_GE(finish_count.load(std::memory_order_acquire), finish_before_auto_quit + 1);
}

void verify_reorder_plugin_resets_between_playback_loops(const char* suffix) {
  ScopedBagPath bag(suffix);

  BagWriter::Config writer_config;
  writer_config.sync_mode = true;
  writer_config.compress = BagWriter::kCompressNone;
  writer_config.tag_name = "reorder-loop-boundary";

  auto writer = BagWriter::create(bag.path.string(), writer_config);
  REQUIRE(writer != nullptr);

  Frame later = bag_frame(1'000, "dds://coverage/later", "raw", SchemaType::kRaw, ActionType::kPublish, "");
  later.data = make_timestamp_payload(2'000);
  Frame earlier = bag_frame(2'000, "dds://coverage/earlier", "raw", SchemaType::kRaw, ActionType::kPublish, "");
  earlier.data = make_timestamp_payload(1'000);
  REQUIRE_EQ(writer->push(later), 1'000);
  REQUIRE_EQ(writer->push(earlier), 2'000);
  writer.reset();

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);

  auto plugin = std::make_shared<ReorderReadPlugin>(60'000);
  reader->bind_bag_interface(plugin);

  std::vector<std::string> observed_urls;
  std::mutex observed_mtx;
  std::atomic_bool finished{false};
  reader->register_output_callback([&](const Frame& frame) {
    std::lock_guard lock(observed_mtx);
    observed_urls.emplace_back(frame.url);
  });
  reader->register_finish_callback([&](bool interrupted) {
    CHECK_FALSE(interrupted);
    finished.store(true, std::memory_order_release);
  });

  REQUIRE(reader->async_run());

  BagReader::Config play_config;
  play_config.force_delay = 0;
  play_config.times = 2;
  reader->play(play_config);

  REQUIRE(common_test::wait_until([&] { return finished.load(std::memory_order_acquire); }, 3s));

  {
    std::lock_guard lock(observed_mtx);
    REQUIRE_EQ(observed_urls.size(), 4u);
    CHECK_EQ(observed_urls[0], "dds://coverage/earlier");
    CHECK_EQ(observed_urls[1], "dds://coverage/later");
    CHECK_EQ(observed_urls[2], "dds://coverage/earlier");
    CHECK_EQ(observed_urls[3], "dds://coverage/later");
  }

  CHECK_EQ(plugin->flush_count(), 2);
  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_reorder_plugin_resets_after_interruption(const char* suffix, bool jump) {
  ScopedBagPath bag(suffix);

  BagWriter::Config writer_config;
  writer_config.sync_mode = true;
  writer_config.compress = BagWriter::kCompressNone;
  writer_config.tag_name = "reorder-interruption-boundary";

  auto writer = BagWriter::create(bag.path.string(), writer_config);
  REQUIRE(writer != nullptr);

  Frame later = bag_frame(1'000, "dds://coverage/later", "raw", SchemaType::kRaw, ActionType::kPublish, "");
  later.data = make_timestamp_payload(3'000);
  Frame earlier = bag_frame(2'000, "dds://coverage/earlier", "raw", SchemaType::kRaw, ActionType::kPublish, "");
  earlier.data = make_timestamp_payload(1'000);
  Frame middle = bag_frame(3'000, "dds://coverage/middle", "raw", SchemaType::kRaw, ActionType::kPublish, "");
  middle.data = make_timestamp_payload(2'000);
  REQUIRE_EQ(writer->push(later), 1'000);
  REQUIRE_EQ(writer->push(earlier), 2'000);
  REQUIRE_EQ(writer->push(middle), 3'000);
  writer.reset();

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);

  auto plugin = std::make_shared<ReorderReadPlugin>(60'000);
  reader->bind_bag_interface(plugin);

  std::atomic<int> ready_count{0};
  std::atomic<int> finish_count{0};
  std::atomic<int> interrupted_count{0};
  std::mutex observed_mtx;
  std::vector<std::pair<int, std::string>> observed;
  reader->register_ready_callback([&] { ready_count.fetch_add(1, std::memory_order_relaxed); });
  reader->register_finish_callback([&](bool interrupted) {
    finish_count.fetch_add(1, std::memory_order_release);
    if (interrupted) {
      interrupted_count.fetch_add(1, std::memory_order_release);
    }
  });
  reader->register_output_callback([&](const Frame& frame) {
    std::lock_guard lock(observed_mtx);
    observed.emplace_back(ready_count.load(std::memory_order_relaxed), frame.url);
  });

  REQUIRE(reader->async_run());

  BagReader::Config play_config;
  play_config.force_delay = 100;
  play_config.times = 1;
  reader->play(play_config);

  REQUIRE(common_test::wait_until([&] { return plugin->input_count() >= 1; }, 2s));

  if (jump) {
    reader->jump(0, 1.0, 1, true);
  } else {
    reader->stop();
    REQUIRE(reader->wait_for_idle(3000));
    REQUIRE_EQ(interrupted_count.load(std::memory_order_acquire), 1);
    reader->play(play_config);
  }

  const int expected_finish_count = jump ? 1 : 2;
  REQUIRE(common_test::wait_until([&] { return finish_count.load(std::memory_order_acquire) == expected_finish_count; },
                                  3s));

  {
    std::lock_guard lock(observed_mtx);
    REQUIRE_EQ(observed.size(), 3u);
    CHECK_EQ(observed[0], std::make_pair(2, std::string("dds://coverage/earlier")));
    CHECK_EQ(observed[1], std::make_pair(2, std::string("dds://coverage/middle")));
    CHECK_EQ(observed[2], std::make_pair(2, std::string("dds://coverage/later")));
  }

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_reorder_plugin_skips_boundary_flush_after_interruption(const char* suffix, bool jump) {
  ScopedBagPath bag(suffix);

  BagWriter::Config writer_config;
  writer_config.sync_mode = true;
  writer_config.compress = BagWriter::kCompressNone;
  writer_config.tag_name = "reorder-boundary-interruption";

  auto writer = BagWriter::create(bag.path.string(), writer_config);
  REQUIRE(writer != nullptr);

  Frame later = bag_frame(1'000, "dds://coverage/later", "raw", SchemaType::kRaw, ActionType::kPublish, "");
  later.data = make_timestamp_payload(2'000);
  Frame earlier = bag_frame(2'000, "dds://coverage/earlier", "raw", SchemaType::kRaw, ActionType::kPublish, "");
  earlier.data = make_timestamp_payload(1'000);
  REQUIRE_EQ(writer->push(later), 1'000);
  REQUIRE_EQ(writer->push(earlier), 2'000);
  writer.reset();

  std::atomic<int> finish_count{0};
  std::atomic<int> interrupted_count{0};
  std::mutex observed_mtx;
  std::vector<std::string> observed_urls;

  auto plugin = std::make_shared<ReorderReadPlugin>(60'000, 2);
  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  reader->bind_bag_interface(plugin);

  reader->register_finish_callback([&](bool interrupted) {
    finish_count.fetch_add(1, std::memory_order_release);
    if (interrupted) {
      interrupted_count.fetch_add(1, std::memory_order_release);
    }
  });
  reader->register_output_callback([&](const Frame& frame) {
    std::lock_guard lock(observed_mtx);
    observed_urls.emplace_back(frame.url);
  });

  REQUIRE(reader->async_run());

  BagReader::Config play_config;
  play_config.force_delay = 0;
  play_config.times = 1;
  reader->play(play_config);

  REQUIRE(plugin->wait_for_input_blocked());

  std::future<void> jump_result;

  if (jump) {
    jump_result = std::async(std::launch::async, [&] { reader->jump(0, 1.0, 1, true); });
    REQUIRE(common_test::wait_until([&] { return reader->is_jumping(); }, 2s));
  } else {
    reader->stop();
  }

  plugin->release_input_block();

  if (jump) {
    jump_result.get();
  } else {
    REQUIRE(reader->wait_for_idle(3000));
    reader->play(play_config);
  }

  const int expected_finish_count = jump ? 1 : 2;
  REQUIRE(common_test::wait_until([&] { return finish_count.load(std::memory_order_acquire) == expected_finish_count; },
                                  3s));

  {
    std::lock_guard lock(observed_mtx);
    REQUIRE_EQ(observed_urls.size(), 2u);
    CHECK_EQ(observed_urls[0], "dds://coverage/earlier");
    CHECK_EQ(observed_urls[1], "dds://coverage/later");
  }

  CHECK_EQ(plugin->flush_count(), 1);
  CHECK_EQ(interrupted_count.load(std::memory_order_acquire), jump ? 0 : 1);

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_split_manifest_tag_update_and_parse_failure(const char* suffix, bool split_before) {
  ScopedBagPath bag(suffix);
  write_split_bag(bag.path, split_before);

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());
  CHECK(reader->is_split_mode());

  reader->tag("manifest-retag");
  REQUIRE(reader->wait_for_idle(3000));
  CHECK_EQ(read_manifest_tag(bag.path), "manifest-retag");

  overwrite_file(bag.path, "{ malformed manifest");
  reader->tag("manifest-retag-after-corruption");
  REQUIRE(reader->wait_for_idle(3000));

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_method_schema_split_bag(const char* suffix) {
  ScopedBagPath bag(suffix);

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.tag_name = "method-schema";
  config.start_timestamp = 1'700'000'550'000LL;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);
  writer->register_schema_callback([](const std::string& ser_type, SchemaType schema_type) {
    return make_schema_data(ser_type, schema_type, "syntax = \"proto3\"; message Method {}");
  });

  const std::string method_ser = "demo.Request|demo.Response";
  REQUIRE_EQ(writer->push(bag_frame(2'000, "dds://coverage/method_schema", method_ser, SchemaType::kProtobuf,
                                    ActionType::kClientRequest, "request")),
             2'000);
  REQUIRE_GE(writer->push(bag_frame(1'500, "dds://coverage/method_schema", method_ser, SchemaType::kProtobuf,
                                    ActionType::kClientResponse, "response")),
             1'500);
  REQUIRE_GE(writer->push(bag_frame(2'500, "dds://coverage/method_schema", method_ser, SchemaType::kProtobuf,
                                    ActionType::kServerRequest, "server-request")),
             2'500);
  REQUIRE_GE(writer->push(bag_frame(3'000, "dds://coverage/method_schema", method_ser, SchemaType::kProtobuf,
                                    ActionType::kServerResponse, "server-response")),
             3'000);

  writer.reset();

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());
  CHECK(reader->check().get());

  REQUIRE(reader->open_cursor());
  const auto frames = read_all_frames(*reader);
  REQUIRE_EQ(frames.size(), 4u);
  const bool has_method_pair = (frames[0].payload == "request" && frames[1].payload == "response") ||
                               (frames[0].payload == "response" && frames[1].payload == "request");
  CHECK(has_method_pair);
  CHECK_EQ(frames[2].payload, "server-request");
  CHECK_EQ(frames[3].payload, "server-response");

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_schema_conflict_bag(const char* suffix) {
  ScopedBagPath bag(suffix);

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.cache_size = 1;
  config.tag_name = "schema-conflict";
  config.start_timestamp = 1'700'000'300'000LL;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);

  REQUIRE(writer->push_schema(
      make_schema_data("demo.Conflict", SchemaType::kProtobuf, "syntax = \"proto3\"; message Conflict {}")));
  CHECK_FALSE(writer->push_schema(make_schema_data("demo.Conflict", SchemaType::kProtobuf,
                                                   "syntax = \"proto3\"; message Conflict { int32 x = 1; }")));

  REQUIRE_EQ(writer->push(bag_frame(1'000, "dds://coverage/conflict", "demo.Conflict", SchemaType::kProtobuf,
                                    ActionType::kPublish, "first")),
             1'000);
  CHECK_EQ(writer->push(bag_frame(2'000, "dds://coverage/conflict", "demo.Other", SchemaType::kProtobuf,
                                  ActionType::kPublish, "second")),
           -1);

  writer.reset();

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());
  REQUIRE(reader->open_cursor());

  const auto frames = read_all_frames(*reader);
  REQUIRE_EQ(frames.size(), 1u);
  CHECK_EQ(frames[0].payload, "first");

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_time_split_bag(const char* suffix, bool split_before) {
  ScopedTempDir dir("bag_split_time_");
  const auto path = dir.path / (std::string("split_time") + suffix);

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.tag_name = "time-split";
  config.split_by_time = 1;
  config.split_name_by_time = true;
  config.start_timestamp = 1'700'000'350'000LL;

  auto writer = BagWriter::create(path.string(), config);
  REQUIRE(writer != nullptr);
  CHECK(writer->is_split_mode());

  std::vector<std::string> split_files;
  writer->register_split_callback(
      [&](int split_index, const std::string& split_filename) {
        CHECK_GE(split_index, 0);
        CHECK_FALSE(split_filename.empty());
        split_files.push_back(split_filename);
      },
      split_before);

  REQUIRE_EQ(
      writer->push(bag_frame(0, "dds://coverage/time_split", "raw", SchemaType::kRaw, ActionType::kPublish, "t0")), 0);
  REQUIRE_EQ(
      writer->push(bag_frame(2'000, "dds://coverage/time_split", "raw", SchemaType::kRaw, ActionType::kPublish, "t1")),
      2'000);
  REQUIRE_EQ(
      writer->push(bag_frame(4'000, "dds://coverage/time_split", "raw", SchemaType::kRaw, ActionType::kPublish, "t2")),
      4'000);

  CHECK_GE(writer->get_split_index(), 1);
  CHECK_FALSE(split_files.empty());
  writer.reset();

  REQUIRE(std::filesystem::exists(path));

  auto reader = BagReader::create(path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());
  CHECK(reader->is_split_mode());
  CHECK_GE(reader->get_info().split_count, 2);
  CHECK_EQ(reader->get_info().split_by_time, 1);
  CHECK(reader->check().get());

  REQUIRE(reader->open_cursor());
  const auto frames = read_all_frames(*reader);
  REQUIRE_EQ(frames.size(), 3u);
  CHECK_EQ(frames[0].payload, "t0");
  CHECK_EQ(frames[1].payload, "t1");
  CHECK_EQ(frames[2].payload, "t2");

  reader->tag("time-split-retag");
  REQUIRE(reader->wait_for_idle(3000));
  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_vdb_limit_policy(bool enable_limit) {
  ScopedBagPath bag(".vdb");

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.cache_size = 1;
  config.max_row_count = 0;
  config.enable_limit = enable_limit;
  config.tag_name = enable_limit ? "limit-evict" : "limit-reject";
  config.start_timestamp = 1'700'000'400'000LL;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);

  REQUIRE_EQ(
      writer->push(bag_frame(1'000, "dds://coverage/limit", "raw", SchemaType::kRaw, ActionType::kPublish, "first")),
      1'000);

  const auto second_result =
      writer->push(bag_frame(2'000, "dds://coverage/limit", "raw", SchemaType::kRaw, ActionType::kPublish, "second"));
  if (enable_limit) {
    CHECK_EQ(second_result, 2'000);
  } else {
    CHECK_EQ(second_result, -1);
  }

  writer.reset();

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());
  REQUIRE(reader->open_cursor());
  const auto frames = read_all_frames(*reader);

  REQUIRE_EQ(frames.size(), 1u);
  CHECK_EQ(frames[0].payload, enable_limit ? "second" : "first");

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_vdb_reader_rebuild_maintenance(const char* suffix) {
  ScopedBagPath bag(suffix);
  if (std::string(suffix) == ".vdbx") {
    write_split_bag(bag.path, false);
  } else {
    write_roundtrip_bag(bag.path);
  }

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  CHECK(reader->check().get());
  CHECK(reader->reindex().get());
  CHECK(reader->fix(false).get());
  CHECK(reader->fix(true).get());

  reader->tag("rebuild-maintenance");
  REQUIRE(reader->wait_for_idle(3000));

  REQUIRE(reader->open_cursor());
  const auto frames = read_all_frames(*reader);
  CHECK_FALSE(frames.empty());
  CHECK_EQ(reader->get_info().tag_name, "rebuild-maintenance");
  CHECK_FALSE(reader->fail());

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void exercise_reader_busy_maintenance(const char* suffix, bool expect_reindex) {
  ScopedBagPath bag(suffix);
  write_busy_bag(bag.path);

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);

  std::atomic<int> output_count{0};
  reader->register_output_callback([&](const Frame&) { output_count.fetch_add(1, std::memory_order_relaxed); });

  REQUIRE(reader->async_run());

  BagReader::Config config;
  config.force_delay = 20;
  config.times = 1;
  reader->play(config);

  REQUIRE(common_test::wait_until([&] { return reader->get_status() == BagReader::kPlaying; }, 1s));

  auto check_result = reader->check();
  auto reindex_result = reader->reindex();
  auto fix_result = reader->fix(false);
  reader->tag("busy-retag");

  CHECK(check_result.get());
  if (expect_reindex) {
    CHECK(reindex_result.get());
    CHECK(fix_result.get());
  } else {
    CHECK_FALSE(reindex_result.get());
    CHECK_FALSE(fix_result.get());
  }

  REQUIRE(reader->wait_for_idle(3000));
  CHECK_GE(output_count.load(std::memory_order_relaxed), 1);

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void exercise_reader_interrupted_playback(const char* suffix) {
  ScopedBagPath bag(suffix);
  write_busy_bag(bag.path);

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);

  std::atomic<int> finish_count{0};
  std::atomic<int> interrupted_count{0};
  std::atomic<int> paused_count{0};
  std::atomic<int> output_count{0};

  reader->register_finish_callback([&](bool interrupted) {
    finish_count.fetch_add(1, std::memory_order_relaxed);
    if (interrupted) {
      interrupted_count.fetch_add(1, std::memory_order_relaxed);
    }
  });
  reader->register_status_callback([&](BagReader::Status status) {
    if (status == BagReader::kPaused) {
      paused_count.fetch_add(1, std::memory_order_relaxed);
    }
  });
  reader->register_output_callback([&](const Frame&) { output_count.fetch_add(1, std::memory_order_relaxed); });

  REQUIRE(reader->async_run());

  BagReader::Config delayed;
  delayed.force_delay = 50;
  delayed.times = 1;

  reader->play(delayed);
  REQUIRE(common_test::wait_until([&] { return reader->get_status() == BagReader::kPlaying; }, 1s));
  CHECK_GE(reader->get_timestamp(), 0);
  CHECK_GE(reader->get_real_timestamp(), 0);
  reader->stop();
  REQUIRE(reader->wait_for_idle(3000));
  CHECK_GE(interrupted_count.load(std::memory_order_relaxed), 1);

  reader->play(delayed);
  REQUIRE(common_test::wait_until([&] { return reader->get_status() == BagReader::kPlaying; }, 1s));
  reader->jump(1, 0.0, 1, true);
  REQUIRE(reader->wait_for_idle(3000));
  CHECK_FALSE(reader->is_jumping());

  reader->play(delayed);
  REQUIRE(common_test::wait_until([&] { return reader->get_status() == BagReader::kPlaying; }, 1s));
  reader->pause();
  REQUIRE(common_test::wait_until([&] { return paused_count.load(std::memory_order_relaxed) > 0; }, 1s));
  CHECK_GE(reader->get_timestamp(), 0);
  CHECK_GE(reader->get_real_timestamp(), 0);
  reader->pause_to_next();
  reader->resume();
  REQUIRE(reader->wait_for_idle(3000));
  CHECK_GT(finish_count.load(std::memory_order_relaxed), 1);
  CHECK_GE(output_count.load(std::memory_order_relaxed), 1);

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_compressed_bag(const char* suffix, BagWriter::CompressType compress) {
  ScopedBagPath bag(suffix);
  write_async_compressed_bag(bag.path, compress);

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  CHECK(reader->check().get());
  if (lowercase_extension(bag.path) == ".vcap") {
    CHECK_FALSE(reader->reindex().get());
    CHECK_FALSE(reader->fix(false).get());
  }

  REQUIRE(reader->open_cursor());
  auto frames = read_all_frames(*reader);
  REQUIRE_EQ(frames.size(), 2u);
  CHECK_EQ(frames[0].url, "dds://coverage/compress");
  CHECK_EQ(frames[0].payload, repeated_payload(2048, 'A'));
  CHECK_EQ(frames[1].url, "dds://coverage/compress_ignored");
  CHECK_EQ(frames[1].payload, repeated_payload(256, 'B'));

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_compressed_bag_playback_paths(const char* suffix, BagWriter::CompressType compress) {
  ScopedBagPath bag(suffix);
  write_async_compressed_bag(bag.path, compress);

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);

  std::mutex frames_mtx;
  std::vector<ObservedFrame> frames;
  std::atomic<int> finish_count{0};

  reader->register_output_callback([&](const Frame& frame) {
    std::lock_guard lock(frames_mtx);
    frames.push_back(
        {frame.timestamp, frame.url, frame.ser_type, frame.schema_type, frame.action_type, frame.data.to_string()});
  });
  reader->register_finish_callback([&](bool interrupted) {
    CHECK_FALSE(interrupted);
    finish_count.fetch_add(1, std::memory_order_release);
  });

  REQUIRE(reader->async_run());

  BagReader::Config config;
  config.force_delay = 0;
  config.times = 1;
  reader->play(config);

  REQUIRE(common_test::wait_until([&] { return finish_count.load(std::memory_order_acquire) == 1; }, 3s));

  {
    std::lock_guard lock(frames_mtx);
    REQUIRE_EQ(frames.size(), 2u);
    CHECK_EQ(frames[0].url, "dds://coverage/compress");
    CHECK_EQ(frames[0].payload, repeated_payload(2048, 'A'));
    CHECK_EQ(frames[1].url, "dds://coverage/compress_ignored");
    CHECK_EQ(frames[1].payload, repeated_payload(256, 'B'));
  }

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_split_bag(const char* suffix, bool split_before) {
  ScopedBagPath bag(suffix);
  write_split_bag(bag.path, split_before);

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  CHECK(reader->is_split_mode());
  CHECK_GE(reader->get_info().split_count, 2);
  CHECK_EQ(reader->get_info().tag_name, "split");
  CHECK(reader->check().get());

  REQUIRE(reader->open_cursor());
  auto frames = read_all_frames(*reader);
  REQUIRE_EQ(frames.size(), 2u);
  CHECK_EQ(frames[0].payload, repeated_payload(512, 'S'));
  CHECK_EQ(frames[1].payload, repeated_payload(512, 'T'));

  reader->tag("split-retag");
  REQUIRE(reader->wait_for_idle(3000));
  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void exercise_reader_playback_controls(const char* suffix, bool expect_reindex) {
  ScopedBagPath bag(suffix);
  write_roundtrip_bag(bag.path);

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);

  std::atomic<int> ready_count{0};
  std::atomic<int> output_count{0};
  std::atomic<int> finish_count{0};
  std::atomic<int> paused_count{0};
  std::atomic<int> stopped_count{0};

  reader->register_ready_callback([&] { ready_count.fetch_add(1, std::memory_order_relaxed); });
  reader->register_status_callback([&](BagReader::Status status) {
    if (status == BagReader::kPaused) {
      paused_count.fetch_add(1, std::memory_order_relaxed);
    } else if (status == BagReader::kStopped) {
      stopped_count.fetch_add(1, std::memory_order_relaxed);
    }
  });
  reader->register_finish_callback([&](bool) { finish_count.fetch_add(1, std::memory_order_relaxed); });
  reader->register_output_callback([&](const Frame&) { output_count.fetch_add(1, std::memory_order_relaxed); });

  REQUIRE(reader->async_run());

  BagReader::Config config;
  config.force_delay = 0;
  config.auto_pause = true;
  config.skip_blank = true;
  config.rate = 0;
  config.times = 1;
  reader->pause_to_next();
  reader->play(config);

  REQUIRE(common_test::wait_until([&] { return paused_count.load(std::memory_order_relaxed) > 0; }, 1s));
  CHECK_GE(reader->get_timestamp(), 0);
  CHECK_GE(reader->get_real_timestamp(), 0);
  CHECK_EQ(output_count.load(std::memory_order_relaxed), 0);
  reader->pause_to_next();
  REQUIRE(common_test::wait_until([&] { return output_count.load(std::memory_order_relaxed) > 0; }, 1s));

  reader->pause();
  reader->pause_to_next();
  reader->resume();
  REQUIRE(common_test::wait_until([&] { return finish_count.load(std::memory_order_relaxed) > 0; }, 1s));
  CHECK_GE(ready_count.load(std::memory_order_relaxed), 1);
  CHECK_GE(stopped_count.load(std::memory_order_relaxed), 1);

  const int output_before_realtime = output_count.load(std::memory_order_relaxed);
  const int finish_before_realtime = finish_count.load(std::memory_order_relaxed);
  BagReader::Config realtime;
  realtime.force_delay = -1;
  realtime.end_time = 2;
  realtime.filter_urls.emplace("dds://coverage/field");
  realtime.times = 1;
  reader->play(realtime);
  REQUIRE(common_test::wait_until([&] { return output_count.load(std::memory_order_relaxed) > output_before_realtime; },
                                  1s));
  REQUIRE(common_test::wait_until([&] { return finish_count.load(std::memory_order_relaxed) > finish_before_realtime; },
                                  1s));

  reader->jump(-1, -1.0, 1, true);
  reader->jump(reader->get_info().total_duration + 1'000, 0.0, 1, true);
  reader->jump(0, 2.0, 1, false);
  reader->stop();
  REQUIRE(reader->wait_for_idle(3000));
  CHECK_FALSE(reader->is_jumping());

  CHECK(reader->check().get());
  if (expect_reindex) {
    CHECK(reader->reindex().get());
    CHECK(reader->fix(false).get());
  } else {
    CHECK_FALSE(reader->reindex().get());
    CHECK_FALSE(reader->fix(true).get());
  }

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_roundtrip_bag(const char* suffix) {
  ScopedBagPath bag(suffix);
  write_roundtrip_bag(bag.path);

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  const auto& info = reader->get_info();
  CHECK_EQ(info.tag_name, "coverage");
  CHECK_EQ(info.message_count, 3);
  CHECK_EQ(info.url_metas.size(), 3u);
  CHECK_GE(info.file_size, 1);
  CHECK_EQ(reader->get_ser_type("dds://coverage/event"), "demo.Message");
  CHECK_EQ(reader->get_schema_type("dds://coverage/event"), SchemaType::kProtobuf);
  CHECK_EQ(reader->get_ser_type("dds://coverage/field"), "raw");
  CHECK_EQ(reader->get_schema_type("dds://coverage/field"), SchemaType::kRaw);
  CHECK_FALSE(reader->is_split_mode());
  CHECK_EQ(reader->get_split_index(), 0);
  CHECK_FALSE(reader->is_jumping());
  CHECK_EQ(reader->get_status(), BagReader::kStopped);
  CHECK_EQ(reader->get_timestamp(), 0);
  CHECK_EQ(reader->get_real_timestamp(), 0);

  auto schemas = reader->detect_schema();
  REQUIRE_EQ(schemas.size(), 1u);
  CHECK_EQ(schemas[0].name, "demo.Message");
  CHECK_EQ(schemas[0].schema_type, SchemaType::kProtobuf);
  CHECK_FALSE(schemas[0].data.empty());

  CHECK(reader->check().get());

  REQUIRE(reader->open_cursor());
  auto frames = read_all_frames(*reader);
  REQUIRE_EQ(frames.size(), 3u);
  CHECK_EQ(frames[0].timestamp, 1'000);
  CHECK_EQ(frames[0].url, "dds://coverage/event");
  CHECK_EQ(frames[0].ser_type, "demo.Message");
  CHECK_EQ(frames[0].schema_type, SchemaType::kProtobuf);
  CHECK_EQ(frames[0].action_type, ActionType::kPublish);
  CHECK_EQ(frames[0].payload, "event-payload");
  CHECK_EQ(frames[1].url, "dds://coverage/field");
  CHECK_EQ(frames[1].schema_type, SchemaType::kRaw);
  CHECK_EQ(frames[1].action_type, ActionType::kSet);
  CHECK_EQ(frames[1].payload, "field-payload");
  CHECK_EQ(frames[2].url, "dds://coverage/method");
  CHECK_EQ(frames[2].action_type, ActionType::kClientRequest);
  CHECK_EQ(frames[2].payload, "request-payload");
  CHECK(reader->eof());
  CHECK_FALSE(reader->fail());

  BagReader::Config filtered;
  filtered.begin_time = 2;
  filtered.end_time = 2;
  filtered.filter_urls.emplace("dds://coverage/field");
  REQUIRE(reader->open_cursor(filtered));
  frames = read_all_frames(*reader);
  REQUIRE_EQ(frames.size(), 1u);
  CHECK_EQ(frames[0].timestamp, 2'000);
  CHECK_EQ(frames[0].url, "dds://coverage/field");

  BagReader::Config from_middle;
  from_middle.begin_time = 2;
  REQUIRE(reader->open_cursor(from_middle));
  frames = read_all_frames(*reader);
  REQUIRE_EQ(frames.size(), 2u);
  CHECK_EQ(frames[0].url, "dds://coverage/field");
  CHECK_EQ(frames[1].url, "dds://coverage/method");

  BagReader::Config before_second;
  before_second.end_time = 1;
  REQUIRE(reader->open_cursor(before_second));
  frames = read_all_frames(*reader);
  REQUIRE_EQ(frames.size(), 1u);
  CHECK_EQ(frames[0].url, "dds://coverage/event");

  BagReader::Config missing_filter;
  missing_filter.filter_urls.emplace("dds://coverage/missing");
  REQUIRE(reader->open_cursor(missing_filter));
  frames = read_all_frames(*reader);
  CHECK(frames.empty());
  CHECK(reader->eof());

  BagReader::Config reversed_window;
  reversed_window.begin_time = 3;
  reversed_window.end_time = 1;
  REQUIRE(reader->open_cursor(reversed_window));
  frames = read_all_frames(*reader);
  CHECK(frames.empty());
  CHECK(reader->eof());

  reader->tag("coverage-retag");
  REQUIRE(reader->wait_for_idle(3000));
  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_empty_bag(const char* suffix) {
  ScopedBagPath bag(suffix);
  write_empty_bag(bag.path);

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  const auto& info = reader->get_info();
  CHECK_EQ(info.tag_name, "empty");
  CHECK_EQ(info.message_count, 0);
  CHECK(info.url_metas.empty());
  CHECK(reader->detect_schema().empty());
  CHECK(reader->check().get());
  CHECK_FALSE(reader->is_split_mode());
  CHECK_EQ(reader->get_ser_type("dds://missing"), "");
  CHECK_EQ(reader->get_schema_type("dds://missing"), SchemaType::kUnknown);

  REQUIRE(reader->open_cursor());
  Frame frame;
  CHECK_FALSE(reader->read_next(frame));
  CHECK(reader->eof());
  CHECK_FALSE(reader->fail());

  reader->tag("empty-retag");
  REQUIRE(reader->wait_for_idle(3000));

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_vdb_check_rejects_empty_ser_metadata() {
  ScopedBagPath bag(".vdb");

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.tag_name = "empty-ser";
  config.start_timestamp = 1'700'000'700'000LL;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);
  REQUIRE_EQ(writer->push(bag_frame(1'000, "dds://coverage/empty_ser", "", SchemaType::kUnknown, ActionType::kPublish,
                                    "payload")),
             1'000);
  writer.reset();

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  REQUIRE_EQ(reader->get_info().message_count, 1);
  REQUIRE_EQ(reader->get_info().url_metas.size(), 1u);
  CHECK(reader->get_info().url_metas.front().ser_type.empty());
  CHECK_FALSE(reader->check().get());

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_vdb_writer_updates_empty_url_metadata_later() {
  ScopedBagPath bag(".vdb");

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.tag_name = "late-ser";
  config.start_timestamp = 1'700'000'710'000LL;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);

  const std::string url = "dds://coverage/late_ser";
  REQUIRE_EQ(writer->push(bag_frame(1'000, url, "", SchemaType::kUnknown, ActionType::kPublish, "first")), 1'000);
  REQUIRE_EQ(writer->push(bag_frame(2'000, url, "raw", SchemaType::kRaw, ActionType::kPublish, "second")), 2'000);
  writer.reset();

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  CHECK_EQ(reader->get_ser_type(url), "raw");
  CHECK_EQ(reader->get_schema_type(url), SchemaType::kRaw);
  CHECK(reader->check().get());

  REQUIRE(reader->open_cursor());
  const auto frames = read_all_frames(*reader);
  REQUIRE_EQ(frames.size(), 2u);
  CHECK_EQ(frames[0].payload, "first");
  CHECK_EQ(frames[1].payload, "second");

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_vdb_reader_reopens_reindexed_file_with_indexes() {
  ScopedBagPath bag(".vdb");
  write_roundtrip_bag(bag.path);

  {
    auto reader = BagReader::create(bag.path.string(), false);
    REQUIRE(reader != nullptr);
    REQUIRE(reader->async_run());
    CHECK(reader->reindex().get());
    reader->quit();
    REQUIRE(reader->wait_for_quit(3000));
  }

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  CHECK(reader->get_info().has_idx_elapsed);
  CHECK(reader->get_info().has_idx_url);
  CHECK(reader->check().get());

  REQUIRE(reader->open_cursor());
  CHECK_FALSE(read_all_frames(*reader).empty());

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_vdb_writer_defaults_and_async_setup() {
  ScopedBagPath bag(".vdb");

  BagWriter::Config config;
  config.compress = BagWriter::kCompressNone;
  config.max_task_depth = 0;
  config.start_timestamp = 1'700'000'720'000LL;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);
  REQUIRE(writer->async_run());
  REQUIRE_EQ(writer->push(bag_frame(1'000, "dds://coverage/default_writer", "raw", SchemaType::kRaw,
                                    ActionType::kPublish, "payload")),
             1'000);
  REQUIRE(writer->wait_for_idle(3000));
  writer->quit();
  REQUIRE(writer->wait_for_quit(3000));
  writer.reset();

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  CHECK_FALSE(reader->get_info().tag_name.empty());
  CHECK(reader->check().get());

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_vdbx_writer_replaces_existing_manifest_family() {
  ScopedBagPath bag(".vdbx");
  write_split_bag(bag.path, true);

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.tag_name = "replace-vdbx";
  config.split_by_size = 1;
  config.start_timestamp = 1'700'000'730'000LL;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);
  CHECK(writer->is_split_mode());
  REQUIRE_EQ(writer->push(
                 bag_frame(0, "dds://coverage/replaced_vdbx", "raw", SchemaType::kRaw, ActionType::kPublish, "first")),
             0);
  REQUIRE_EQ(writer->push(bag_frame(1'000, "dds://coverage/replaced_vdbx", "raw", SchemaType::kRaw,
                                    ActionType::kPublish, repeated_payload(128, 'r'))),
             1'000);
  writer.reset();

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  CHECK(reader->is_split_mode());
  CHECK_EQ(reader->get_info().tag_name, "replace-vdbx");
  CHECK(reader->check().get());

  REQUIRE(reader->open_cursor());
  CHECK_EQ(read_all_frames(*reader).size(), 2u);

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_vdbx_writer_overwrites_malformed_existing_manifest() {
  ScopedBagPath bag(".vdbx");
  overwrite_file(bag.path, "{ malformed manifest");

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.tag_name = "malformed-replaced";
  config.split_by_size = 1;
  config.start_timestamp = 1'700'000'740'000LL;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);
  CHECK(writer->is_split_mode());
  REQUIRE_EQ(writer->push(bag_frame(0, "dds://coverage/malformed_vdbx", "raw", SchemaType::kRaw, ActionType::kPublish,
                                    "payload")),
             0);
  writer.reset();

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  CHECK(reader->is_split_mode());
  CHECK_EQ(reader->get_info().tag_name, "malformed-replaced");
  CHECK(reader->check().get());

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_vdb_reader_rejects_corrupt_single_header_accuracy() {
  ScopedBagPath bag(".vdb");
  write_roundtrip_bag(bag.path);

  REQUIRE_EQ(replace_file_bytes(bag.path, "MicroSecond", "NanoSecondX", false), 1u);
  CHECK_THROWS((void)BagReader::create(bag.path.string(), true));
}

void verify_vdb_check_rejects_corrupt_schema_encoding() {
  ScopedBagPath bag(".vdb");
  write_roundtrip_bag(bag.path);

  REQUIRE_GT(replace_file_bytes(bag.path, "protobuf", "badproto", true), 0u);

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  CHECK_FALSE(reader->check().get());

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_vdb_check_rejects_writer_invalid_loss() {
  ScopedBagPath bag(".vdb");

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.tag_name = "vdb-bad-loss";
  config.start_timestamp = 1'700'000'755'000LL;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);
  writer->set_url_loss("dds://coverage/vdb_bad_loss", 2.0);
  REQUIRE_EQ(writer->push(bag_frame(1'000, "dds://coverage/vdb_bad_loss", "raw", SchemaType::kRaw, ActionType::kPublish,
                                    "payload")),
             1'000);
  writer.reset();

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  CHECK_FALSE(reader->check().get());

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_vdb_sqlite_metadata_mutations_fail_check() {
  const std::vector<std::pair<std::string, bool>> mutations{
      {"DELETE FROM VLinkUrls;", false},
      {"UPDATE VLinkUrls SET url='' WHERE id=1;", false},
      {"UPDATE VLinkUrls SET type='' WHERE id=1;", false},
      {"UPDATE VLinkUrls SET ser='' WHERE id=1;", false},
      {"UPDATE VLinkUrls SET count=count+1 WHERE id=1;", false},
      {"UPDATE VLinkUrls SET encoding='bogus' WHERE id=1;", true},
      {"UPDATE VLinkUrls SET loss=-0.01 WHERE id=1;", false},
      {"UPDATE VLinkUrls SET loss=1.01 WHERE id=1;", false},
      {"UPDATE VLinkUrls SET freq=-1 WHERE id=1;", false},
      {"UPDATE VLinkHeader SET count=count+1 WHERE rowid=1;", false},
      {"UPDATE VLinkSchemas SET ser='';", true},
      {"UPDATE VLinkSchemas SET encoding='';", false},
  };

  for (const auto& mutation : mutations) {
    const std::string& sql = mutation.first;
    const bool expected_check_result = mutation.second;
    ScopedBagPath bag(".vdb");
    write_roundtrip_bag(bag.path);

    if (!run_sqlite3_sql(bag.path, sql)) {
      return;
    }

    auto reader = BagReader::create(bag.path.string(), false);
    REQUIRE(reader != nullptr);
    REQUIRE(reader->async_run());

    CHECK_EQ(reader->check().get(), expected_check_result);

    reader->quit();
    REQUIRE(reader->wait_for_quit(3000));
  }
}

void verify_vdb_sqlite_cursor_handles_missing_url_and_null_action() {
  ScopedBagPath bag(".vdb");
  write_roundtrip_bag(bag.path);

  const std::string sql =
      "UPDATE VLinkDatas SET url=999 WHERE rowid=1;"
      "UPDATE VLinkDatas SET action=NULL WHERE rowid=2;"
      "DROP INDEX IF EXISTS idx_elapsed_url;";
  if (!run_sqlite3_sql(bag.path, sql)) {
    return;
  }

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  CHECK_FALSE(reader->get_info().has_idx_url);
  CHECK_FALSE(reader->get_info().has_idx_elapsed);
  REQUIRE(reader->open_cursor());

  const auto frames = read_all_frames(*reader);
  REQUIRE_EQ(frames.size(), 2u);
  CHECK_EQ(frames.front().url, "dds://coverage/field");
  CHECK_EQ(frames.front().action_type, ActionType::kUnknownAction);
  CHECK_EQ(frames.back().url, "dds://coverage/method");

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_vdb_sqlite_missing_tables_are_rejected_cleanly() {
  for (const auto* table : {"VLinkDatas", "VLinkUrls", "VLinkHeader"}) {
    ScopedBagPath bag(".vdb");
    write_roundtrip_bag(bag.path);

    const std::string sql = std::string("DROP TABLE ") + table + ";";
    if (!run_sqlite3_sql(bag.path, sql)) {
      return;
    }

    CHECK_THROWS((void)BagReader::create(bag.path.string(), true));
  }
}

void verify_vdb_sqlite_nullable_header_fields_are_defaulted() {
  ScopedBagPath bag(".vdb");
  write_roundtrip_bag(bag.path);

  const std::string sql = "UPDATE VLinkHeader SET tag=NULL, start_timestamp=-1, complete=0 WHERE rowid=1;";
  if (!run_sqlite3_sql(bag.path, sql)) {
    return;
  }

  auto reader = BagReader::create(bag.path.string(), true);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  CHECK_EQ(reader->get_info().tag_name, "Empty");
  CHECK_EQ(reader->get_info().start_timestamp, 0);
  CHECK_FALSE(reader->get_info().has_completed);
  CHECK_FALSE(reader->check().get());

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_vdb_sqlite_cursor_handles_null_payload_and_index_variants() {
  {
    ScopedBagPath bag(".vdb");
    write_roundtrip_bag(bag.path);

    const std::string sql =
        "UPDATE VLinkDatas SET data=NULL, action=NULL WHERE rowid=1;"
        "DROP INDEX IF EXISTS idx_elapsed;";
    if (!run_sqlite3_sql(bag.path, sql)) {
      return;
    }

    auto reader = BagReader::create(bag.path.string(), false);
    REQUIRE(reader != nullptr);
    REQUIRE(reader->async_run());

    REQUIRE(reader->open_cursor());

    const auto frames = read_all_frames(*reader);
    REQUIRE_EQ(frames.size(), 3u);
    CHECK_EQ(frames.front().url, "dds://coverage/event");
    CHECK_EQ(frames.front().action_type, ActionType::kUnknownAction);
    CHECK(frames.front().payload.empty());

    reader->quit();
    REQUIRE(reader->wait_for_quit(3000));
  }

  {
    ScopedBagPath bag(".vdb");
    write_roundtrip_bag(bag.path);

    const std::string sql =
        "DROP INDEX IF EXISTS idx_elapsed;"
        "DROP INDEX IF EXISTS idx_elapsed_url;";
    if (!run_sqlite3_sql(bag.path, sql)) {
      return;
    }

    auto reader = BagReader::create(bag.path.string(), false);
    REQUIRE(reader != nullptr);
    REQUIRE(reader->async_run());

    CHECK_FALSE(reader->get_info().has_idx_elapsed);
    CHECK_FALSE(reader->get_info().has_idx_url);

    BagReader::Config config;
    config.begin_time = 2;
    config.end_time = 3;
    REQUIRE(reader->open_cursor(config));

    const auto frames = read_all_frames(*reader);
    REQUIRE_EQ(frames.size(), 2u);
    CHECK_EQ(frames.front().url, "dds://coverage/field");
    CHECK_EQ(frames.back().url, "dds://coverage/method");

    reader->quit();
    REQUIRE(reader->wait_for_quit(3000));
  }
}

void verify_vdb_single_file_none_compression_aliases() {
  for (const auto* alias : {"NONE", "none"}) {
    ScopedBagPath bag(".vdb");
    write_roundtrip_bag(bag.path);

    REQUIRE_EQ(replace_file_bytes(bag.path, "None", alias, false), 1u);

    auto reader = BagReader::create(bag.path.string(), false);
    REQUIRE(reader != nullptr);
    REQUIRE(reader->async_run());

    CHECK_EQ(reader->get_info().compression_type, alias);
    CHECK(reader->check().get());

    REQUIRE(reader->open_cursor());
    const auto frames = read_all_frames(*reader);
    REQUIRE_EQ(frames.size(), 3u);
    CHECK_EQ(frames.front().payload, "event-payload");

    reader->quit();
    REQUIRE(reader->wait_for_quit(3000));
  }
}

void verify_vcap_check_rejects_empty_ser_metadata() {
  ScopedBagPath bag(".vcap");

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.tag_name = "vcap-empty-ser";
  config.start_timestamp = 1'700'000'750'000LL;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);
  REQUIRE_EQ(writer->push(bag_frame(1'000, "dds://coverage/vcap_empty_ser", "", SchemaType::kUnknown,
                                    ActionType::kPublish, "payload")),
             1'000);
  writer.reset();

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  REQUIRE_EQ(reader->get_info().message_count, 1);
  REQUIRE_EQ(reader->get_info().url_metas.size(), 1u);
  CHECK(reader->get_info().url_metas.front().ser_type.empty());
  CHECK_FALSE(reader->check().get());

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_vcap_check_rejects_corrupt_schema_encoding() {
  ScopedBagPath bag(".vcap");
  write_roundtrip_bag(bag.path);

  REQUIRE_GT(replace_file_bytes(bag.path, "protobuf", "badproto", true), 0u);

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  CHECK_FALSE(reader->check().get());

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_vcap_check_rejects_invalid_loss_and_frequency() {
  ScopedBagPath bag(".vcap");

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.tag_name = "vcap-bad-rates";
  config.start_timestamp = 1'700'000'770'000LL;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);
  writer->set_url_loss("dds://coverage/vcap_bad_rates", 0.25);
  REQUIRE_EQ(writer->push(bag_frame(1'000, "dds://coverage/vcap_bad_rates", "raw", SchemaType::kRaw,
                                    ActionType::kPublish, "first")),
             1'000);
  writer.reset();

  REQUIRE_EQ(replace_file_bytes(bag.path, "0.250000", "2.000000", false), 1u);
  REQUIRE_EQ(replace_file_bytes(bag.path, "0.000000", "-1.00000", false), 1u);

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  CHECK_FALSE(reader->check().get());

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_vcap_check_rejects_invalid_header_version() {
  ScopedBagPath bag(".vcap");
  write_roundtrip_bag(bag.path);

  std::string version;
  {
    auto reader = BagReader::create(bag.path.string(), false);
    REQUIRE(reader != nullptr);
    version = reader->get_info().version;
  }

  REQUIRE_FALSE(version.empty());
  const std::string invalid_version(version.size(), 'x');
  REQUIRE_EQ(replace_file_bytes(bag.path, version, invalid_version, false), 1u);

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  CHECK_FALSE(reader->check().get());

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_vcap_check_normalizes_negative_start_timestamp() {
  ScopedBagPath bag(".vcap");

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.tag_name = "vcap-bad-start";
  config.start_timestamp = 1'700'000'790'000LL;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);
  REQUIRE_EQ(writer->push(bag_frame(1'000, "dds://coverage/vcap_bad_start", "raw", SchemaType::kRaw,
                                    ActionType::kPublish, "payload")),
             1'000);
  writer.reset();

  REQUIRE_EQ(replace_file_bytes(bag.path, "1700000790000", "-700000790000", false), 1u);

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  CHECK_EQ(reader->get_info().start_timestamp, 0);
  CHECK(reader->check().get());

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_vcap_check_rejects_incompatible_header_version() {
  ScopedBagPath bag(".vcap");
  write_roundtrip_bag(bag.path);

  std::string version;
  {
    auto reader = BagReader::create(bag.path.string(), false);
    REQUIRE(reader != nullptr);
    version = reader->get_info().version;
  }

  REQUIRE_FALSE(version.empty());
  std::string incompatible_version = version;
  incompatible_version.front() = incompatible_version.front() == '9' ? '8' : '9';
  REQUIRE_EQ(replace_file_bytes(bag.path, version, incompatible_version, false), 1u);

  try {
    auto reader = BagReader::create(bag.path.string(), false);
    REQUIRE(reader != nullptr);
    REQUIRE(reader->async_run());

    CHECK_FALSE(reader->check().get());

    reader->quit();
    REQUIRE(reader->wait_for_quit(3000));
  } catch (const std::exception&) {
    CHECK(true);
  }
}

void verify_vcap_check_uses_date_when_start_timestamp_is_not_numeric() {
  ScopedBagPath bag(".vcap");

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.tag_name = "vcap-bad-start-text";
  config.start_timestamp = 1'700'000'800'000LL;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);
  REQUIRE_EQ(writer->push(bag_frame(1'000, "dds://coverage/vcap_bad_start_text", "raw", SchemaType::kRaw,
                                    ActionType::kPublish, "payload")),
             1'000);
  writer.reset();

  REQUIRE_EQ(replace_file_bytes(bag.path, "1700000800000", "invalid-start", false), 1u);

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  CHECK_GE(reader->get_info().start_timestamp, 0);
  CHECK(reader->check().get());

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_vdb_split_schema_detection_merges_duplicate_schemas() {
  ScopedBagPath bag(".vdbx");

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.tag_name = "vdb-dup-schema";
  config.split_by_size = 256;
  config.start_timestamp = 1'700'000'785'000LL;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);
  REQUIRE(writer->push_schema(make_schema_data("demo.VdbSplitSchema", SchemaType::kProtobuf,
                                               "syntax = \"proto3\"; message VdbSplitSchema {}")));

  REQUIRE_EQ(writer->push(bag_frame(1'000, "dds://coverage/vdb_dup_schema", "demo.VdbSplitSchema",
                                    SchemaType::kProtobuf, ActionType::kPublish, repeated_payload(512, 'v'))),
             1'000);
  REQUIRE_EQ(writer->push(bag_frame(2'000, "dds://coverage/vdb_dup_schema", "demo.VdbSplitSchema",
                                    SchemaType::kProtobuf, ActionType::kPublish, repeated_payload(512, 'w'))),
             2'000);
  writer.reset();

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  REQUIRE(reader->is_split_mode());
  REQUIRE_GE(reader->get_info().split_count, 2);
  auto schemas = reader->detect_schema();
  REQUIRE_EQ(schemas.size(), 1u);
  CHECK_EQ(schemas.front().name, "demo.VdbSplitSchema");
  CHECK_EQ(schemas.front().schema_type, SchemaType::kProtobuf);
  CHECK_FALSE(schemas.front().encoding.empty());
  CHECK_FALSE(schemas.front().data.empty());
  CHECK(reader->check().get());

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
}

void verify_vcap_split_schema_detection_merges_duplicate_schemas() {
  ScopedBagPath bag(".vcapx");

  BagWriter::Config config;
  config.sync_mode = true;
  config.compress = BagWriter::kCompressNone;
  config.tag_name = "vcap-dup-schema";
  config.split_by_size = 256;
  config.start_timestamp = 1'700'000'780'000LL;

  auto writer = BagWriter::create(bag.path.string(), config);
  REQUIRE(writer != nullptr);
  REQUIRE(writer->push_schema(
      make_schema_data("demo.SplitSchema", SchemaType::kProtobuf, "syntax = \"proto3\"; message SplitSchema {}")));

  REQUIRE_EQ(writer->push(bag_frame(1'000, "dds://coverage/vcap_dup_schema", "demo.SplitSchema", SchemaType::kProtobuf,
                                    ActionType::kPublish, repeated_payload(512, 'a'))),
             1'000);
  REQUIRE_EQ(writer->push(bag_frame(2'000, "dds://coverage/vcap_dup_schema", "demo.SplitSchema", SchemaType::kProtobuf,
                                    ActionType::kPublish, repeated_payload(512, 'b'))),
             2'000);
  writer.reset();

  auto reader = BagReader::create(bag.path.string(), false);
  REQUIRE(reader != nullptr);
  REQUIRE(reader->async_run());

  REQUIRE(reader->is_split_mode());
  REQUIRE_GE(reader->get_info().split_count, 2);
  auto schemas = reader->detect_schema();
  REQUIRE_EQ(schemas.size(), 1u);
  CHECK_EQ(schemas.front().name, "demo.SplitSchema");
  CHECK_EQ(schemas.front().schema_type, SchemaType::kProtobuf);
  CHECK_FALSE(schemas.front().encoding.empty());
  CHECK_FALSE(schemas.front().data.empty());
  CHECK(reader->check().get());

  reader->quit();
  REQUIRE(reader->wait_for_quit(3000));
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
    CHECK_EQ(urls.size(), 3u);
    CHECK_EQ(urls.front(), "dds://a");
    CHECK_EQ(urls.back(), "dds://c");
    CHECK(reader.eof());
    CHECK_FALSE(reader.fail());
    CHECK_FALSE(static_cast<bool>(reader));

    CHECK_FALSE(reader.read_next(frame));
    CHECK(reader.eof());
    CHECK_FALSE(reader.fail());
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

  TEST_CASE("process_output populates effective metadata before invoking a read plugin") {
    StubBagReader reader;
    auto plugin = std::make_shared<ReadMetaPlugin>();
    reader.bind_bag_interface(plugin);

    std::vector<BagReader::Info::UrlMeta> metas;
    BagReader::Info::UrlMeta meta;
    meta.url = "intra://meta-old";
    meta.ser_type = "demo.Original";
    meta.schema_type = SchemaType::kProtobuf;
    metas.emplace_back(std::move(meta));
    reader.process_url_metas(metas);
    reader.rebuild_url_meta_lookup(metas);

    std::string output_url;
    std::string output_ser;
    SchemaType output_schema = SchemaType::kUnknown;
    reader.register_output_callback([&](const Frame& frame) {
      output_url = frame.url;
      output_ser = frame.ser_type;
      output_schema = frame.schema_type;
    });

    Frame frame = read_frame(1, "intra://meta-old", ActionType::kPublish, Bytes::create(1u));
    reader.process_output(frame);

    CHECK_EQ(plugin->observed_url, "intra://meta-old");
    CHECK_EQ(plugin->observed_ser, "demo.Converted");
    CHECK_EQ(plugin->observed_schema, SchemaType::kFlatbuffers);
    CHECK_EQ(output_url, "intra://meta-new");
    CHECK_EQ(output_ser, "demo.Converted");
    CHECK_EQ(output_schema, SchemaType::kFlatbuffers);
  }

  TEST_CASE("read plugins explicitly select metadata when changing a frame url") {
    auto run = [](bool clear_meta) {
      StubBagReader reader;
      reader.bind_bag_interface(std::make_shared<RewriteReadUrlPlugin>(clear_meta));

      std::vector<BagReader::Info::UrlMeta> metas;
      BagReader::Info::UrlMeta type_a;
      type_a.url = "intra://type-a";
      type_a.ser_type = "demo.TypeA";
      type_a.schema_type = SchemaType::kProtobuf;
      metas.emplace_back(std::move(type_a));
      BagReader::Info::UrlMeta type_b;
      type_b.url = "intra://type-b";
      type_b.ser_type = "demo.TypeB";
      type_b.schema_type = SchemaType::kFlatbuffers;
      metas.emplace_back(std::move(type_b));
      reader.process_url_metas(metas);
      reader.rebuild_url_meta_lookup(metas);

      std::pair<std::string, SchemaType> observed;
      reader.register_output_callback(
          [&](const Frame& frame) { observed = std::make_pair(frame.ser_type, frame.schema_type); });

      Frame frame = read_frame(1, "intra://type-a", ActionType::kPublish, Bytes::create(1u));
      reader.process_output(frame);

      return observed;
    };

    CHECK_EQ(run(false), std::make_pair(std::string("demo.TypeA"), SchemaType::kProtobuf));
    CHECK_EQ(run(true), std::make_pair(std::string("demo.TypeB"), SchemaType::kFlatbuffers));
  }

  TEST_CASE("flush_plugin drains an async read plugin's buffered tail frames") {
    StubBagReader reader;
    reader.bind_bag_interface(std::make_shared<ReorderReadPlugin>(60'000));

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
    reader.bind_bag_interface(plugin);

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
    reader.bind_bag_interface(plugin);

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
    reader.bind_bag_interface(plugin);

    std::vector<BagReader::Info::UrlMeta> metas;
    BagReader::Info::UrlMeta m;
    m.url = "intra://old";
    metas.push_back(m);
    BagReader::Info::UrlMeta dropped;
    dropped.url = "intra://drop";
    metas.push_back(dropped);
    reader.process_url_metas(metas);

    std::unordered_set<std::string> filter_urls;
    filter_urls.emplace("intra://new");

    CHECK(reader.match_playback_url_filter("intra://old", filter_urls));
    CHECK_FALSE(reader.match_playback_url_filter("intra://drop", filter_urls));
    CHECK_FALSE(reader.match_playback_url_filter("intra://unmapped", filter_urls));

    filter_urls.clear();
    CHECK(reader.match_playback_url_filter("intra://unmapped", filter_urls));
    CHECK_FALSE(reader.match_playback_url_filter("intra://drop", filter_urls));

    std::string_view null_url;
    CHECK_FALSE(reader.match_playback_url_filter(null_url, filter_urls));
  }

  TEST_CASE("process_output drops urls excluded by a bound plugin") {
    StubBagReader reader;
    auto plugin = std::make_shared<RemapPlugin>();
    reader.bind_bag_interface(plugin);

    std::vector<BagReader::Info::UrlMeta> metas;
    BagReader::Info::UrlMeta dropped;
    dropped.url = "intra://drop";
    metas.emplace_back(dropped);
    reader.process_url_metas(metas);
    CHECK(metas.empty());

    int call_count = 0;
    reader.register_output_callback([&](const Frame&) { ++call_count; });

    Frame frame = read_frame(1, "intra://drop", ActionType::kPublish, Bytes::create(1u));
    reader.process_output(frame);
    CHECK_EQ(call_count, 0);
  }

  TEST_CASE("url meta ordering is transport url then index stable") {
    BagReader::Info::UrlMeta intra_b;
    intra_b.url = "intra://b";
    intra_b.index = 2;

    BagReader::Info::UrlMeta intra_a;
    intra_a.url = "intra://a";
    intra_a.index = 3;

    BagReader::Info::UrlMeta intra_a_low_index;
    intra_a_low_index.url = "intra://a";
    intra_a_low_index.index = 1;

    BagReader::Info::UrlMeta dds;
    dds.url = "dds://z";
    dds.index = 4;

    std::vector<BagReader::Info::UrlMeta> metas{intra_b, dds, intra_a, intra_a_low_index};
    std::sort(metas.begin(), metas.end());

    REQUIRE_EQ(metas.size(), 4u);
    CHECK_EQ(metas[0].url, "intra://a");
    CHECK_EQ(metas[0].index, 1);
    CHECK_EQ(metas[1].url, "intra://a");
    CHECK_EQ(metas[1].index, 3);
    CHECK_EQ(metas[2].url, "intra://b");
  }

  TEST_CASE("rebinding plugin disconnects the old plugin read callback") {
    StubBagReader reader;
    auto old_plugin = std::make_shared<RemapPlugin>();
    auto new_plugin = std::make_shared<RemapPlugin>();

    int call_count = 0;
    reader.register_output_callback([&](const Frame&) { ++call_count; });

    reader.bind_bag_interface(old_plugin);
    reader.bind_bag_interface(new_plugin);

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

  TEST_CASE("rebuild_url_meta_maps clears conflicting ser and schema metadata") {
    std::vector<BagReader::Info::UrlMeta> metas;

    BagReader::Info::UrlMeta first;
    first.url = "intra://conflict";
    first.ser_type = "demo.First";
    first.schema_type = SchemaType::kProtobuf;
    metas.emplace_back(first);

    BagReader::Info::UrlMeta bytes_compat;
    bytes_compat.url = "intra://bytes";
    bytes_compat.ser_type = "Bytes";
    bytes_compat.schema_type = SchemaType::kUnknown;
    metas.emplace_back(bytes_compat);

    BagReader::Info::UrlMeta bytes_specific;
    bytes_specific.url = "intra://bytes";
    bytes_specific.ser_type = "demo.BytesPayload";
    bytes_specific.schema_type = SchemaType::kProtobuf;
    metas.emplace_back(bytes_specific);

    BagReader::Info::UrlMeta second;
    second.url = "intra://conflict";
    second.ser_type = "demo.Second";
    second.schema_type = SchemaType::kFlatbuffers;
    metas.emplace_back(second);

    BagReader::Info::UrlMeta ignored_after_conflict;
    ignored_after_conflict.url = "intra://conflict";
    ignored_after_conflict.ser_type = "demo.Third";
    ignored_after_conflict.schema_type = SchemaType::kZeroCopy;
    metas.emplace_back(ignored_after_conflict);

    std::unordered_map<std::string, std::string> ser_map;
    std::unordered_map<std::string, SchemaType> schema_type_map;
    StubBagReader::rebuild_url_meta_maps(metas, ser_map, schema_type_map);

    REQUIRE_EQ(ser_map.count("intra://conflict"), 1u);
    REQUIRE_EQ(schema_type_map.count("intra://conflict"), 1u);
    CHECK(ser_map["intra://conflict"].empty());
    CHECK_EQ(schema_type_map["intra://conflict"], SchemaType::kUnknown);
    CHECK_EQ(ser_map["intra://bytes"], "demo.BytesPayload");
    CHECK_EQ(schema_type_map["intra://bytes"], SchemaType::kProtobuf);
  }

  TEST_CASE("vdb writer output is readable through the cursor API") { verify_roundtrip_bag(".vdb"); }

  TEST_CASE("vcap writer output is readable through the cursor API") { verify_roundtrip_bag(".vcap"); }

  TEST_CASE("empty vdb and vcap bags are readable and report empty metadata") {
    verify_empty_bag(".vdb");
    verify_empty_bag(".vcap");
  }

  TEST_CASE("uppercase vdb and vcap suffixes use the same reader and writer paths") {
    verify_roundtrip_bag(".VDB");
    verify_roundtrip_bag(".VCAP");
  }

  TEST_CASE("compressed vdb and vcap bags preserve payloads through async writes") {
    verify_compressed_bag(".vdb", BagWriter::kCompressLzav);
    verify_compressed_bag(".vcap", BagWriter::kCompressZstd);
  }

  TEST_CASE("compressed vdb and vcap bags replay through callback playback") {
    verify_compressed_bag_playback_paths(".vdb", BagWriter::kCompressLzav);
    verify_compressed_bag_playback_paths(".vcap", BagWriter::kCompressZstd);
  }

  TEST_CASE("split vdbx and vcapx bags read back all split frames") {
    verify_split_bag(".vdbx", true);
    verify_split_bag(".vcapx", false);
  }

  TEST_CASE("time based split vdbx and vcapx bags use timestamped split files") {
    verify_time_split_bag(".vdbx", true);
    verify_time_split_bag(".vcapx", false);
  }

  TEST_CASE("vdb writer row limit either rejects or evicts deterministically") {
    verify_vdb_limit_policy(false);
    verify_vdb_limit_policy(true);
  }

  TEST_CASE("vdb reader rejects corrupted single-file metadata") {
    verify_vdb_check_rejects_empty_ser_metadata();
    verify_vdb_reader_rejects_corrupt_single_header_accuracy();
    verify_vdb_check_rejects_corrupt_schema_encoding();
    verify_vdb_check_rejects_writer_invalid_loss();
    verify_vdb_sqlite_metadata_mutations_fail_check();
    verify_vdb_sqlite_cursor_handles_missing_url_and_null_action();
    verify_vdb_sqlite_missing_tables_are_rejected_cleanly();
    verify_vdb_sqlite_nullable_header_fields_are_defaulted();
    verify_vdb_sqlite_cursor_handles_null_payload_and_index_variants();
    verify_vdb_single_file_none_compression_aliases();
  }

  TEST_CASE("vcap reader rejects corrupted single-file metadata") {
    verify_vcap_check_rejects_empty_ser_metadata();
    verify_vcap_check_rejects_corrupt_schema_encoding();
    verify_vcap_check_rejects_invalid_loss_and_frequency();
    verify_vcap_check_rejects_invalid_header_version();
    verify_vcap_check_rejects_incompatible_header_version();
    verify_vcap_check_normalizes_negative_start_timestamp();
    verify_vcap_check_uses_date_when_start_timestamp_is_not_numeric();
  }

  TEST_CASE("vdb writer fills initially empty url metadata on later frames") {
    verify_vdb_writer_updates_empty_url_metadata_later();
  }

  TEST_CASE("vdb writer and reader cover index and split replacement maintenance") {
    verify_vdb_reader_reopens_reindexed_file_with_indexes();
    verify_vdb_writer_defaults_and_async_setup();
    verify_vdbx_writer_replaces_existing_manifest_family();
    verify_vdbx_writer_overwrites_malformed_existing_manifest();
    verify_vdb_split_schema_detection_merges_duplicate_schemas();
    verify_vcap_split_schema_detection_merges_duplicate_schemas();
  }

  TEST_CASE("vdb readers rebuild indexes and stay readable after maintenance") {
    verify_vdb_reader_rebuild_maintenance(".vdb");
    verify_vdb_reader_rebuild_maintenance(".vdbx");
  }

  TEST_CASE("vdb and vcap writers reject conflicting schemas and url ser changes") {
    verify_schema_conflict_bag(".vdb");
    verify_schema_conflict_bag(".vcap");
  }

  TEST_CASE("vdb and vcap readers reject missing or malformed bag files deterministically") {
    verify_missing_single_bag_is_not_cursor_readable(".vdb");
    verify_missing_single_bag_is_not_cursor_readable(".vcap");
    verify_invalid_single_bag_is_not_cursor_readable(".vdb");
    verify_invalid_single_bag_is_not_cursor_readable(".vcap");
  }

  TEST_CASE("split manifests without readable data files fail cursor opening") {
    verify_split_manifest_without_readable_files_is_not_cursor_readable(".vdbx", ".vdb");
    verify_split_manifest_without_readable_files_is_not_cursor_readable(".vcapx", ".vcap");
    verify_malformed_split_manifest_is_not_cursor_readable(".vdbx");
    verify_malformed_split_manifest_is_not_cursor_readable(".vcapx");
  }

  TEST_CASE("split manifest check rejects inconsistent aggregate metadata") {
    verify_split_manifest_check_rejects_inconsistent_metadata(".vdbx", true);
    verify_split_manifest_check_rejects_inconsistent_metadata(".vcapx", false);
  }

  TEST_CASE("split manifest check rejects invalid url metadata fields") {
    verify_split_manifest_check_rejects_invalid_url_metadata(".vdbx", true);
    verify_split_manifest_check_rejects_invalid_url_metadata(".vcapx", false);
  }

  TEST_CASE("split manifest rejects unsupported timestamp accuracy") {
    verify_unsupported_split_manifest_accuracy_throws(".vdbx", true);
    verify_unsupported_split_manifest_accuracy_throws(".vcapx", false);
  }

  TEST_CASE("split manifests apply documented defaults for optional metadata") {
    verify_split_manifest_optional_fields_have_defaults(".vdbx", true);
    verify_split_manifest_optional_fields_have_defaults(".vcapx", false);
  }

  TEST_CASE("split manifests expose non-default compression metadata without data reads") {
    verify_split_manifest_non_default_compression_metadata(".vdbx", true);
    verify_split_manifest_non_default_compression_metadata(".vcapx", false);
    verify_split_manifest_none_compression_aliases(".vdbx", true);
    verify_split_manifest_none_compression_aliases(".vcapx", false);
  }

  TEST_CASE("split manifests normalize negative start timestamps without corrupting data") {
    verify_split_manifest_negative_start_timestamp_is_normalized(".vdbx", true);
    verify_split_manifest_negative_start_timestamp_is_normalized(".vcapx", false);
  }

  TEST_CASE("split manifest check rejects typed urls without schema encoding") {
    verify_split_manifest_missing_schema_encoding_is_rejected(".vdbx", true);
    verify_split_manifest_missing_schema_encoding_is_rejected(".vcapx", false);
  }

  TEST_CASE("split manifest check reports incomplete headers and rejects missing url metadata") {
    verify_split_manifest_incomplete_header_is_reported(".vdbx", true);
    verify_split_manifest_incomplete_header_is_reported(".vcapx", false);
    verify_split_manifest_missing_url_metadata_fails_check(".vdbx", true);
    verify_split_manifest_missing_url_metadata_fails_check(".vcapx", false);
  }

  TEST_CASE("split manifest tag rewrites survive valid updates and malformed manifests") {
    verify_split_manifest_tag_update_and_parse_failure(".vdbx", true);
    verify_split_manifest_tag_update_and_parse_failure(".vcapx", false);
  }

  TEST_CASE("real readers rebind plugins and rebuild metadata maps") {
    verify_real_reader_plugin_rebind_updates_metadata(".vdb");
    verify_real_reader_plugin_rebind_updates_metadata(".vcap");
  }

  TEST_CASE("vcap cursor honors read plugin exclusions and remaps") {
    verify_vcap_cursor_plugin_excludes_and_remaps_urls();
  }

  TEST_CASE("vdb cursor honors read plugin exclusions and remaps") {
    verify_vdb_cursor_plugin_excludes_and_remaps_urls();
  }

  TEST_CASE("split cursor filtering walks every split file") {
    verify_split_cursor_filters_each_file(".vdbx", true);
    verify_split_cursor_filters_each_file(".vcapx", false);
  }

  TEST_CASE("method request and response schema branches remain readable") {
    verify_method_schema_split_bag(".vdb");
    verify_method_schema_split_bag(".vcap");
  }

  TEST_CASE("real readers exercise playback controls and maintenance APIs") {
    exercise_reader_playback_controls(".vdb", true);
    exercise_reader_playback_controls(".vcap", false);
    verify_reader_playback_loops_and_auto_quit(".vdb");
    verify_reader_playback_loops_and_auto_quit(".vcap");
  }

  TEST_CASE("buffered read plugins reset their data-time axis between playback loops") {
    verify_reorder_plugin_resets_between_playback_loops(".vdb");
    verify_reorder_plugin_resets_between_playback_loops(".vcap");
  }

  TEST_CASE("buffered read plugins discard interrupted playback state before restart") {
    verify_reorder_plugin_resets_after_interruption(".vdb", false);
    verify_reorder_plugin_resets_after_interruption(".vcap", false);
    verify_reorder_plugin_resets_after_interruption(".vdb", true);
    verify_reorder_plugin_resets_after_interruption(".vcap", true);
  }

  TEST_CASE("buffered read plugins do not flush a pass interrupted at its final frame") {
    verify_reorder_plugin_skips_boundary_flush_after_interruption(".vdb", false);
    verify_reorder_plugin_skips_boundary_flush_after_interruption(".vcap", false);
    verify_reorder_plugin_skips_boundary_flush_after_interruption(".vdb", true);
    verify_reorder_plugin_skips_boundary_flush_after_interruption(".vcap", true);
  }

  TEST_CASE("busy real readers queue maintenance APIs without losing playback state") {
    exercise_reader_busy_maintenance(".vdb", true);
    exercise_reader_busy_maintenance(".vcap", false);
  }

  TEST_CASE("real readers handle stop jump and pause while playback is active") {
    exercise_reader_interrupted_playback(".vdb");
    exercise_reader_interrupted_playback(".vcap");
  }

  TEST_CASE("create returns nullptr for unsupported file extension") {
    auto reader = BagReader::create((std::filesystem::path(Utils::get_tmp_dir()) / "unsupported.xyz").string());

    CHECK(reader == nullptr);
  }
}
#endif

// NOLINTEND
