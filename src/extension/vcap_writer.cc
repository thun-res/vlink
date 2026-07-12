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

#include "./extension/vcap_writer.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "./base/elapsed_timer.h"
#include "./base/helpers.h"
#include "./base/logger.h"
#include "./version.h"

// json
#include <nlohmann/json.hpp>

// mcap
#include "./private/mcap_import.h"

// schema_plugin
#include "./extension/schema_plugin_interface.h"

namespace vlink {

static const std::string& make_channel_key(std::string& buffer, const std::string& url, std::string_view action_name) {
  buffer.assign(url);
  buffer.push_back('\x1F');
  buffer.append(action_name);

  return buffer;
}  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

// VCAPWriter::Impl
struct VCAPWriter::Impl final {  // NOLINT(clang-analyzer-optin.performance.Padding)
  // UrlMsgInfo
  struct UrlMsgInfo final {
    int index{0};
    size_t count{0};
    size_t size{0};
    int64_t first_timestamp{-1};
    int64_t last_timestamp{-1};
    double freq{0};
    double loss{0};
    std::string url;
    std::string url_type;
    std::string ser_type;
    SchemaType schema_type{SchemaType::kUnknown};
    ActionType action_type{ActionType::kUnknownAction};

    bool operator<(const UrlMsgInfo& target) const noexcept { return index < target.index; }
  };

  struct MemoryCharge final {
    MemoryCharge(std::atomic<int64_t>& counter, int64_t bytes) : value(counter), size(bytes) {}

    ~MemoryCharge() { value.fetch_sub(size, std::memory_order_relaxed); }

    std::atomic<int64_t>& value;
    int64_t size;
  };

  std::atomic_bool is_dumping{false};
  std::atomic_bool is_split_mode{false};
  std::atomic<int> split_index{0};
  std::atomic<int64_t> memory_size{0};
  std::atomic_bool in_cached{false};
  std::atomic<int64_t> cached_size{0};
  std::atomic_bool quit_flag{false};

  std::string path;
  std::filesystem::path active_path;
  std::filesystem::path split_output_dir;
  std::string base_dir;
  std::string base_name;
  BagWriter::Config config;
  ElapsedTimer elapsed_timer{ElapsedTimer::kMicro};

  int64_t current_row{0};
  int64_t current_size{0};
  bool has_oversize{false};

  int64_t last_timestamp{0};

  BagWriter::SystemClock time_start;
  BagWriter::SystemClock time_current;
  int64_t start_timestamp{0};

  std::vector<std::string> split_file_list;
  bool split_before{false};
  bool split_first{false};

  std::vector<std::string> total_url_list;
  int64_t total_current_row{0};
  int64_t total_current_size{0};
  int64_t total_timestamp{0};

  std::unordered_map<std::string, UrlMsgInfo> url_map;
  std::unordered_map<std::string, int> ser_map;
  std::unordered_map<std::string, UrlMsgInfo> total_url_map;
  std::unordered_map<std::string, SchemaData> total_schema_map;

  BagWriter::SplitCallback split_callback;
  BagWriter::SchemaCallback schema_callback;
  std::string split_filename;
  std::mutex split_mtx;

  std::string app_name;
  std::string tag_name;
  int32_t timezone_diff{0};

  bool enable_compressed{false};
  std::mutex write_mtx;

  std::string write_url_type;
  std::string write_channel_key;

  // mcap
  std::optional<mcap::McapWriter> writer;
  mcap::McapWriterOptions writer_options{"vlink"};

  // schema plugin interface
  SchemaPluginInterface* schema_plugin_interface{nullptr};
};

// VCAPWriter
VCAPWriter::VCAPWriter(const std::string& path, const Config& config)
    : BagWriter(path, config), impl_{std::make_unique<Impl>()} {
  set_name("VCAPWriter");

  impl_->url_map.reserve(128);
  impl_->ser_map.reserve(128);
  url_loss_map_ref().reserve(128);
  impl_->total_url_map.reserve(128);
  total_url_loss_map_ref().reserve(128);
  impl_->total_schema_map.reserve(128);

  impl_->schema_plugin_interface = get_schema_interface();

  impl_->app_name = get_default_app_name();

  if (config.tag_name.empty()) {
    impl_->tag_name = get_default_tag_name();
  } else {
    impl_->tag_name = config.tag_name;
  }

  impl_->timezone_diff = get_default_timezone_diff();

  impl_->path = path;
  impl_->config = config;

  impl_->enable_compressed = impl_->config.compress == kCompressAuto || impl_->config.compress == kCompressZstd;

  if (impl_->enable_compressed) {
#ifdef VLINK_ENABLE_ZSTD
    impl_->writer_options.compression = mcap::Compression::Zstd;

    switch (impl_->config.compress_level) {
      case 0:
        impl_->writer_options.compressionLevel = mcap::CompressionLevel::Default;
        break;
      case 1:
        impl_->writer_options.compressionLevel = mcap::CompressionLevel::Fastest;
        break;
      case 2:
        impl_->writer_options.compressionLevel = mcap::CompressionLevel::Fast;
        break;
      case 3:
        impl_->writer_options.compressionLevel = mcap::CompressionLevel::Default;
        break;
      case 4:
        impl_->writer_options.compressionLevel = mcap::CompressionLevel::Slow;
        break;
      case 5:
        impl_->writer_options.compressionLevel = mcap::CompressionLevel::Slowest;
        break;
      default:
        impl_->writer_options.compressionLevel = mcap::CompressionLevel::Default;
        break;
    }
#else
    impl_->writer_options.compression = mcap::Compression::None;
    impl_->config.compress = kCompressNone;

    impl_->enable_compressed = false;
    VLOG_W("VCAPWriter: Compress is not supported.");
#endif
  } else {
    impl_->writer_options.compression = mcap::Compression::None;
  }

  if (impl_->config.cache_size > 0) {
    impl_->writer_options.noChunking = false;
    impl_->writer_options.chunkSize = impl_->config.cache_size;
  } else {
    impl_->writer_options.noChunking = true;
    impl_->writer_options.chunkSize = 0;

    impl_->writer_options.compression = mcap::Compression::None;
    impl_->config.compress = kCompressNone;

    if (impl_->enable_compressed) {
      impl_->enable_compressed = false;
      VLOG_W("VCAPWriter: Compress is not supported without cache_size > 0.");
    }
  }

  // if VUNLIKELY (impl_->config.wal_mode) {
  //   VLOG_W("VCAPWriter not support [config.wal_mode]");
  // }

  if VUNLIKELY (impl_->config.max_task_depth <= 0) {
    impl_->config.max_task_depth = BagWriter::Config().max_task_depth;
  }

  reset_lockfree_capacity();

  if VUNLIKELY (impl_->config.enable_limit) {
    VLOG_W("VCAPWriter: Enable limit is not supported.");
  }

  try {
#ifdef _WIN32
    std::filesystem::path file_path(Helpers::string_to_wstring(path));
    std::string suffix = Helpers::path_to_string(file_path.extension());
#else
    std::filesystem::path file_path(path);
    std::string suffix = file_path.extension().string();
#endif

    std::filesystem::path parent_path;

    try {
      parent_path = file_path.parent_path();
    } catch (std::filesystem::filesystem_error&) {  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return std::tolower(c); });

    if (suffix == ".vcapx") {
      if (std::filesystem::exists(file_path)) {
        try {
          nlohmann::json root_json;

          {
            std::ifstream filex(file_path);

            filex >> root_json;

            filex.close();
          }

          nlohmann::json files_json = root_json["VLinkFiles"];

          for (const auto& file_info : files_json) {
            if (!parent_path.empty() && std::filesystem::exists(parent_path / file_info)) {
              std::filesystem::remove(parent_path / file_info);
            }
          }

          std::filesystem::remove(file_path);
        } catch (nlohmann::json::exception&) {
        }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      impl_->is_split_mode.store(true, std::memory_order_relaxed);
      impl_->split_index.store(0, std::memory_order_relaxed);

#ifdef _WIN32

      std::error_code absolute_ec;
      auto absolute_path = std::filesystem::absolute(file_path, absolute_ec);

      if VUNLIKELY (absolute_ec) {
        absolute_path = file_path;
      }

      impl_->path = Helpers::path_to_string(absolute_path);
      impl_->split_output_dir = absolute_path.parent_path();

      if (parent_path.empty()) {
        impl_->base_dir.clear();
        impl_->base_name = Helpers::path_to_string(file_path.stem());
      } else {
        impl_->base_dir = Helpers::path_to_string(parent_path);
        impl_->base_name = Helpers::path_to_string(std::filesystem::path(parent_path / file_path.stem()));
      }
#else

      std::error_code absolute_ec;
      auto absolute_path = std::filesystem::absolute(file_path, absolute_ec);

      if VUNLIKELY (absolute_ec) {
        absolute_path = file_path;
      }

      impl_->path = absolute_path.string();
      impl_->split_output_dir = absolute_path.parent_path();

      if (parent_path.empty()) {
        impl_->base_dir.clear();
        impl_->base_name = file_path.stem().string();
      } else {
        impl_->base_dir = parent_path.string();
        impl_->base_name = std::filesystem::path(parent_path / file_path.stem()).string();
      }
#endif

      impl_->time_start = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
      impl_->time_current = impl_->time_start;

      if (impl_->config.start_timestamp > 0) {
        impl_->start_timestamp = impl_->config.start_timestamp;
      } else {
        impl_->start_timestamp = impl_->time_start.time_since_epoch().count();
      }

      write_filex(false);

      if (impl_->config.split_name_by_time) {
        if (impl_->base_dir.empty()) {
          impl_->split_filename = get_format_date(&impl_->time_current, true) + ".vcap";
        } else {
          impl_->split_filename = impl_->base_dir + "/" + get_format_date(&impl_->time_current, true) + ".vcap";
        }
      } else {
        impl_->split_filename =
            impl_->base_name + "." + std::to_string(impl_->split_index.load(std::memory_order_relaxed) + 1) + ".vcap";
      }

      open_split(impl_->split_filename);
    } else {
      impl_->time_start = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
      impl_->time_current = impl_->time_start;

      if (impl_->config.start_timestamp > 0) {
        impl_->start_timestamp = impl_->config.start_timestamp;
      } else {
        impl_->start_timestamp = impl_->time_start.time_since_epoch().count();
      }

      impl_->is_split_mode.store(false, std::memory_order_relaxed);
      impl_->split_index.store(0, std::memory_order_relaxed);

      open(path);
    }
  } catch (std::filesystem::filesystem_error& e) {
    VLOG_F("VCAPWriter: Filesystem error, ", e.what(), ".");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

  impl_->elapsed_timer.start();
}

VCAPWriter::~VCAPWriter() {
  detach_plugin();

  impl_->quit_flag.store(true, std::memory_order_release);

  if VUNLIKELY (!wait_for_idle(30000U)) {
    VLOG_W("VCAPWriter: Force to quit.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  quit(true);

  wait_for_quit();

  close();
}

void VCAPWriter::close() {
  close_segment();

  if VUNLIKELY (impl_->is_split_mode.load(std::memory_order_relaxed) && !write_filex(true)) {
    set_fail();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }
}

void VCAPWriter::register_split_callback(SplitCallback&& callback, bool before) {
  std::lock_guard lock(impl_->split_mtx);
  impl_->split_before = before;
  impl_->split_callback = std::move(callback);
}

void VCAPWriter::register_schema_callback(SchemaCallback&& callback) {
  std::lock_guard lock(impl_->write_mtx);
  impl_->schema_callback = std::move(callback);
}

bool VCAPWriter::merge_schema(SchemaData& schema_data) {
  const auto resolved_schema_type =
      SchemaData::resolve_type(schema_data.schema_type, schema_data.name, schema_data.encoding);
  schema_data.schema_type = resolved_schema_type;

  if VUNLIKELY (schema_data.name.empty()) {
    return true;
  }

  if (schema_data.encoding.empty() && SchemaData::is_real_type(resolved_schema_type)) {
    schema_data.encoding = std::string(SchemaData::convert_type(resolved_schema_type));
  }

  std::string schema_key = schema_data.name;
  schema_key.push_back('\x1F');
  schema_key.append(SchemaData::convert_type(resolved_schema_type));

  std::string unknown_schema_key;
  auto schema_iter = impl_->total_schema_map.find(schema_key);

  if (schema_iter == impl_->total_schema_map.end() && SchemaData::is_real_type(resolved_schema_type)) {
    unknown_schema_key = schema_data.name;
    unknown_schema_key.push_back('\x1F');
    schema_iter = impl_->total_schema_map.find(unknown_schema_key);
  }

  if (schema_iter == impl_->total_schema_map.end()) {
    impl_->total_schema_map.emplace(schema_key, schema_data);
  } else {
    auto& current = schema_iter->second;

    if VUNLIKELY ((!schema_data.encoding.empty() && !current.encoding.empty() &&
                   current.encoding != schema_data.encoding) ||
                  (!schema_data.data.empty() && !current.data.empty() && current.data != schema_data.data) ||
                  (SchemaData::is_real_type(resolved_schema_type) && SchemaData::is_real_type(current.schema_type) &&
                   current.schema_type != resolved_schema_type)) {
      CLOG_E("VCAPWriter: Conflicting schema pushed for [%s].", schema_data.name.c_str());
      return false;
    }

    if (current.encoding.empty() && !schema_data.encoding.empty()) {
      current.encoding = schema_data.encoding;
    }

    if (current.data.empty() && !schema_data.data.empty()) {
      current.data = schema_data.data;
    }

    if (!SchemaData::is_real_type(current.schema_type) && SchemaData::is_real_type(resolved_schema_type)) {
      current.schema_type = resolved_schema_type;
    }

    schema_data = current;

    if (schema_iter->first != schema_key && current.schema_type == resolved_schema_type) {
      impl_->total_schema_map.erase(schema_iter);
      schema_iter = impl_->total_schema_map.emplace(schema_key, schema_data).first;
    }
  }

  return true;
}

bool VCAPWriter::load_schema(const std::string& ser_type, SchemaType& schema_type, SchemaData& schema_data) {
  schema_data = SchemaData{};

  if VUNLIKELY (ser_type.empty()) {
    return true;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  std::string schema_key = ser_type;
  schema_key.push_back('\x1F');
  schema_key.append(SchemaData::convert_type(schema_type));

  std::string unknown_schema_key;
  auto schema_iter = impl_->total_schema_map.end();

  if (schema_type != SchemaType::kUnknown) {
    schema_iter = impl_->total_schema_map.find(schema_key);

    if (schema_iter == impl_->total_schema_map.end()) {
      unknown_schema_key = ser_type;
      unknown_schema_key.push_back('\x1F');
      schema_iter = impl_->total_schema_map.find(unknown_schema_key);
    }
  } else {
    const auto prefix = ser_type + std::string("\x1F");

    for (auto iter = impl_->total_schema_map.begin(); iter != impl_->total_schema_map.end(); ++iter) {
      if (!Helpers::has_startwith(iter->first, prefix)) {
        continue;
      }

      if (schema_iter != impl_->total_schema_map.end()) {
        schema_iter = impl_->total_schema_map.end();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        break;                                        // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      schema_iter = iter;
    }
  }

  if (schema_iter != impl_->total_schema_map.end()) {
    schema_data = schema_iter->second;
  } else if (impl_->schema_plugin_interface) {
    schema_data =
        impl_->schema_plugin_interface->search_schema(ser_type, schema_type);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  } else if (impl_->schema_callback) {
    schema_data = impl_->schema_callback(ser_type, schema_type);
  }

  schema_type = SchemaData::resolve_type(schema_type, ser_type, schema_data.encoding);
  schema_data.schema_type = SchemaData::resolve_type(schema_data.schema_type, ser_type, schema_data.encoding);

  if (schema_type != SchemaType::kUnknown && schema_data.schema_type != SchemaType::kUnknown &&
      schema_type != schema_data.schema_type) {
    CLOG_E("VCAPWriter: Schema family mismatch for [%s], requested = %d, resolved = %d.", ser_type.c_str(),
           static_cast<int>(schema_type), static_cast<int>(schema_data.schema_type));
    return false;
  }

  if (schema_type != SchemaType::kUnknown && schema_data.encoding.empty()) {
    schema_data.encoding = std::string(SchemaData::convert_type(schema_type));
  }

  if (schema_data.schema_type == SchemaType::kUnknown && schema_type != SchemaType::kUnknown) {
    schema_data.schema_type = schema_type;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if (!schema_data.name.empty()) {
    std::string resolved_schema_key = ser_type;
    resolved_schema_key.push_back('\x1F');
    resolved_schema_key.append(SchemaData::convert_type(schema_data.schema_type));

    if (schema_iter != impl_->total_schema_map.end() && schema_iter->first != resolved_schema_key &&
        schema_iter->second.schema_type == SchemaType::kUnknown && schema_data.schema_type != SchemaType::kUnknown) {
      impl_->total_schema_map.erase(schema_iter);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    impl_->total_schema_map.insert_or_assign(resolved_schema_key, schema_data);
  }

  return true;
}

bool VCAPWriter::push_schema(const SchemaData& schema_data, bool immediate) {
  SchemaData stored_schema = schema_data;

  if VUNLIKELY (!stored_schema.data.is_owner()) {
    stored_schema.data.deep_copy(schema_data.data);
  }

  if (immediate) {
    std::lock_guard lock(impl_->write_mtx);
    return merge_schema(stored_schema);
  }

  bool posted = post_persistent_task([this, stored_schema = std::move(stored_schema)]() mutable {
    std::lock_guard lock(impl_->write_mtx);

    if VUNLIKELY (!merge_schema(stored_schema)) {
      CLOG_E("VCAPWriter: Deferred merge_schema failed for [%s] in async push_schema path.",
             stored_schema.name.c_str());
      set_fail();
    }
  });

  return posted;
}

int64_t VCAPWriter::record(const Frame& frame, bool immediate) {
  const std::string& url = frame.url;
  const std::string& ser_type = frame.ser_type;
  const SchemaType schema_type = frame.schema_type;
  const ActionType action_type = frame.action_type;
  const Bytes& data = frame.data;
  const int64_t microseconds_timestamp = frame.timestamp;

  if (immediate) {
    std::lock_guard lock(impl_->write_mtx);

    if VUNLIKELY (!write(url, ser_type, schema_type, action_type, data, microseconds_timestamp)) {
      return -1;
    }
  } else {
    if VUNLIKELY (impl_->memory_size.load(std::memory_order_relaxed) + static_cast<int64_t>(data.size()) >
                  impl_->config.max_memory_size) {
      CLOG_E("The memory data in the queue exceeds %.1fGB and the task is automatically discarded.",
             impl_->config.max_memory_size / 1024.0 / 1024.0 / 1024.0);

      return -1;
    }

    int url_index = -1;
    int ser_index = -1;

    get_url_meta(url, ser_type, url_index, ser_index);

    const auto queued_size = static_cast<int64_t>(data.size());

    impl_->memory_size.fetch_add(queued_size, std::memory_order_relaxed);
    auto memory_charge = std::make_unique<Impl::MemoryCharge>(impl_->memory_size, queued_size);

    bool posted = post_persistent_task([this, url_index, ser_index, schema_type, action_type, data,
                                        memory_charge = std::move(memory_charge),
                                        microseconds_timestamp]() {  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      (void)memory_charge;

      std::lock_guard lock(impl_->write_mtx);
      std::string url;
      std::string ser_type;

      get_url_meta(url_index, ser_index, url, ser_type);

      if VUNLIKELY (!write(url, ser_type, schema_type, action_type, data, microseconds_timestamp)) {
        set_fail();
      }
    });

    if VUNLIKELY (!posted) {
      return -1;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
  }

  return microseconds_timestamp;
}

int64_t VCAPWriter::get_record_timestamp() const { return impl_->elapsed_timer.get(); }

bool VCAPWriter::is_dumping() const { return impl_->is_dumping.load(std::memory_order_relaxed); }

bool VCAPWriter::is_split_mode() const { return impl_->is_split_mode.load(std::memory_order_relaxed); }

int VCAPWriter::get_split_index() const { return impl_->split_index.load(std::memory_order_relaxed); }

size_t VCAPWriter::get_max_task_count() const { return impl_->config.max_task_depth; }

void VCAPWriter::on_begin() {
  MessageLoop::on_begin();

  impl_->elapsed_timer.restart();
}

void VCAPWriter::on_end() { MessageLoop::on_end(); }

void VCAPWriter::open(const std::string& path) {
  try {
#ifdef _WIN32
    impl_->split_file_list.emplace_back(Helpers::path_to_string(std::filesystem::path(path).filename()));
    std::filesystem::path file_path(Helpers::string_to_wstring(path));
#else
    impl_->split_file_list.emplace_back(std::filesystem::path(path).filename().string());
    std::filesystem::path file_path(path);
#endif

    std::error_code absolute_ec;
    impl_->active_path = std::filesystem::absolute(file_path, absolute_ec);

    if VUNLIKELY (absolute_ec) {
      impl_->active_path = file_path;
    }

    if (std::filesystem::exists(file_path)) {
      std::filesystem::remove(file_path);
    } else {
      auto parent_path = file_path.parent_path();

      if (!parent_path.empty() && !std::filesystem::exists(parent_path)) {
        std::filesystem::create_directories(parent_path);
      }
    }
  } catch (std::filesystem::filesystem_error& e) {
    VLOG_F("VCAPWriter: Filesystem error, ", e.what(), ".");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return;                                                   // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

  mcap::Status status;

  impl_->writer.emplace();

  status = impl_->writer->open(path, impl_->writer_options);

  if VUNLIKELY (!status.ok()) {
    CLOG_F("VCAPWriter: Failed to open vcap, error = %s.", status.message.c_str());  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return;                                                                          // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  mcap::Metadata header_meta_data;
  header_meta_data.name = "VLinkHeader";
  header_meta_data.metadata["tag"] = impl_->tag_name;
  header_meta_data.metadata["version"] = VLINK_VERSION;
  header_meta_data.metadata["compress"] = impl_->enable_compressed ? "zstd" : "None";
  header_meta_data.metadata["process"] = impl_->app_name;
  header_meta_data.metadata["date"] = get_format_date(&impl_->time_current);
  header_meta_data.metadata["timezone"] = std::to_string(impl_->timezone_diff);
  header_meta_data.metadata["start_timestamp"] = std::to_string(impl_->start_timestamp);

  status = impl_->writer->write(header_meta_data);

  if VUNLIKELY (!status.ok()) {
    CLOG_F("VCAPWriter: Failed to write header meta data, error = %s.",  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
           status.message.c_str());                                      // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return;                                                              // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  impl_->last_timestamp = 0;
}

void VCAPWriter::open_split(const std::string& path) {
#ifdef _WIN32
  const auto file_name = std::filesystem::path(Helpers::string_to_wstring(path)).filename();
  open(Helpers::path_to_string(impl_->split_output_dir / file_name));
#else
  open((impl_->split_output_dir / std::filesystem::path(path).filename()).string());
#endif
}

void VCAPWriter::close_segment() {
  if (!impl_->writer) {
    return;
  }

  mcap::Status status;

  std::vector<Impl::UrlMsgInfo> msg_info_list;
  msg_info_list.reserve(impl_->url_map.size());

  {
    std::lock_guard lock(sample_mutex());

    for (const auto& entry : impl_->url_map) {
      const auto& msg_info = entry.second;
      msg_info_list.emplace_back(msg_info);

      auto& last = msg_info_list.back();
      auto loss_iter = url_loss_map_ref().find(recover_recorded_url(last.url));
      last.loss = loss_iter == url_loss_map_ref().end() ? 0 : loss_iter->second;
    }
  }

  std::sort(msg_info_list.begin(), msg_info_list.end());

  for (const auto& msg_info : msg_info_list) {
    mcap::Metadata channel_meta_data;
    channel_meta_data.name = "VLinkChannel_" + std::to_string(msg_info.index + 1);
    channel_meta_data.metadata["index"] = std::to_string(msg_info.index);
    channel_meta_data.metadata["type"] = msg_info.url_type;
    channel_meta_data.metadata["action"] = std::string(convert_action(msg_info.action_type));
    channel_meta_data.metadata["encoding"] = std::string(SchemaData::convert_type(msg_info.schema_type));
    channel_meta_data.metadata["ser"] = msg_info.ser_type;
    channel_meta_data.metadata["count"] = std::to_string(msg_info.count);
    channel_meta_data.metadata["size"] = std::to_string(msg_info.size);
    channel_meta_data.metadata["loss"] = Helpers::double_to_string(msg_info.loss, 6);
    channel_meta_data.metadata["freq"] = std::to_string(msg_info.freq);

    status = impl_->writer->write(channel_meta_data);

    if VUNLIKELY (!status.ok()) {
      CLOG_E("VCAPWriter: Failed to write channel meta data, error = %s.",  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
             status.message.c_str());                                       // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      set_fail();                                                           // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      break;                                                                // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
  }

  impl_->writer->close();
  impl_->writer->terminate();
  impl_->writer.reset();

  constexpr std::string_view kMcapMagic{"\x89MCAP0\r\n", 8};
  std::error_code footer_ec;
  const auto file_size = std::filesystem::file_size(impl_->active_path, footer_ec);

  if VUNLIKELY (footer_ec || file_size < kMcapMagic.size()) {
    set_fail();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  } else {
    std::ifstream tail_check(impl_->active_path, std::ios::binary);
    char magic[8] = {};

    tail_check.seekg(-static_cast<std::streamoff>(kMcapMagic.size()), std::ios::end);
    tail_check.read(magic, static_cast<std::streamsize>(kMcapMagic.size()));

    if VUNLIKELY (!tail_check || std::string_view(magic, kMcapMagic.size()) != kMcapMagic) {
      set_fail();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
  }

  impl_->url_map.clear();
  impl_->ser_map.clear();
  url_loss_map_ref().clear();

  impl_->current_row = 0;
  impl_->current_size = 0;
  impl_->has_oversize = false;

  impl_->in_cached.store(false, std::memory_order_relaxed);
  impl_->cached_size.store(0, std::memory_order_relaxed);
}

bool VCAPWriter::write(const std::string& url, const std::string& ser_type, SchemaType schema_type,
                       ActionType action_type, const Bytes& data, int64_t microseconds_timestamp) {
  if VUNLIKELY (!impl_->writer) {
    VLOG_E("VCAPWriter: Writer is not open.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;                               // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if VUNLIKELY (impl_->is_split_mode.load(std::memory_order_relaxed) && !impl_->split_first) {
    impl_->split_first = true;

    std::lock_guard split_lock(impl_->split_mtx);

    if (!impl_->split_before && impl_->split_callback && impl_->split_index.load(std::memory_order_relaxed) == 0) {
      impl_->split_callback(0, impl_->split_filename);
    }
  }

  mcap::Status status;

  bool do_split = false;

  // split

  if (impl_->is_split_mode.load(std::memory_order_relaxed) && !impl_->url_map.empty()) {
    if (impl_->config.split_by_time > 0 &&
        (microseconds_timestamp - impl_->config.begin_time * 1000) >
            impl_->config.split_by_time * 1000 * static_cast<int64_t>(impl_->split_file_list.size())) {
      do_split = true;
    } else if (impl_->config.split_by_time <= 0 && impl_->config.split_by_size > 0 &&
               (impl_->current_size + static_cast<int64_t>(data.size())) > impl_->config.split_by_size) {
      do_split = true;
    } else {
      do_split = false;
    }

    if VUNLIKELY (do_split) {
      std::lock_guard split_lock(impl_->split_mtx);

      impl_->split_index.fetch_add(1, std::memory_order_relaxed);
      impl_->time_current = impl_->time_start + std::chrono::milliseconds(microseconds_timestamp / 1000U);

      if (impl_->config.split_name_by_time) {
        if (impl_->base_dir.empty()) {
          impl_->split_filename = get_format_date(&impl_->time_current, true) + ".vcap";
        } else {
          impl_->split_filename = impl_->base_dir + "/" + get_format_date(&impl_->time_current, true) + ".vcap";
        }
      } else {
        impl_->split_filename =
            impl_->base_name + "." + std::to_string(impl_->split_index.load(std::memory_order_relaxed) + 1) + ".vcap";
      }

      if (impl_->split_before && impl_->split_callback) {
        impl_->split_callback(impl_->split_index.load(std::memory_order_relaxed), impl_->split_filename);
      }

      close_segment();

      if VUNLIKELY (!write_filex(false)) {
        set_fail();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      open_split(impl_->split_filename);

      if (!impl_->split_before && impl_->split_callback) {
        impl_->split_callback(impl_->split_index.load(std::memory_order_relaxed), impl_->split_filename);
      }
    }
  }

  // insert url
  const std::string& channel_key = make_channel_key(impl_->write_channel_key, url, convert_action(action_type));
  auto total_url_iter_ret = impl_->total_url_map.try_emplace(channel_key, Impl::UrlMsgInfo());
  auto url_iter_ret = impl_->url_map.try_emplace(channel_key, Impl::UrlMsgInfo());

  auto discard_new_url_entries = [this, &url_iter_ret, &total_url_iter_ret]() {
    if (url_iter_ret.second) {
      impl_->url_map.erase(url_iter_ret.first);
    }

    if (total_url_iter_ret.second) {
      impl_->total_url_map.erase(total_url_iter_ret.first);
    }
  };

  Impl::UrlMsgInfo& total_url_msg_info = total_url_iter_ret.first->second;

  Impl::UrlMsgInfo& url_msg_info = url_iter_ret.first->second;
  auto resolved_schema_type = SchemaData::resolve_type(schema_type, ser_type);
  std::string next_ser_type = total_url_msg_info.ser_type;
  SchemaType next_schema_type = total_url_msg_info.schema_type;

  if (total_url_iter_ret.second) {
    next_ser_type = ser_type;
    next_schema_type = resolved_schema_type;
  } else {
    if (!ser_type.empty()) {
      if (next_ser_type.empty()) {
        next_ser_type = ser_type;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      } else if VUNLIKELY (next_ser_type != ser_type) {
        CLOG_E("VCAPWriter: URL [%s] ser changed from [%s] to [%s].", url.c_str(), next_ser_type.c_str(),
               ser_type.c_str());
        discard_new_url_entries();
        return false;
      }
    }
  }

  SchemaData schema_data;
  std::string schema_ser_type;
  const auto schema_ser_source = ser_type.empty() ? std::string_view{next_ser_type} : std::string_view{ser_type};
  SchemaType schema_storage_type = SchemaData::resolve_type(schema_type, schema_ser_source);
  bool has_split_method_schema = false;

  schema_ser_type.assign(schema_ser_source.begin(), schema_ser_source.end());

  if ((action_type == ActionType::kClientRequest || action_type == ActionType::kClientResponse ||
       action_type == ActionType::kServerRequest || action_type == ActionType::kServerResponse) &&
      !schema_ser_source.empty()) {
    const auto split_pos = schema_ser_source.find('|');

    if (split_pos != std::string_view::npos) {
      auto payload_ser_type = schema_ser_source.substr(0, split_pos);

      if (action_type == ActionType::kClientResponse || action_type == ActionType::kServerResponse) {
        payload_ser_type = schema_ser_source.substr(split_pos + 1);
      }

      if (!payload_ser_type.empty()) {
        schema_ser_type.assign(payload_ser_type.begin(), payload_ser_type.end());
        schema_storage_type = SchemaData::resolve_type(schema_type, payload_ser_type);
        has_split_method_schema = true;
      }
    }
  }

  if (!next_ser_type.empty()) {
    if VUNLIKELY (!load_schema(schema_ser_type, schema_storage_type, schema_data)) {
      discard_new_url_entries();
      return false;
    }

    schema_storage_type = SchemaData::resolve_type(schema_storage_type, schema_ser_type, schema_data.encoding);

    if (has_split_method_schema) {
      if (schema_storage_type != SchemaType::kUnknown) {
        if (next_schema_type == SchemaType::kUnknown) {
          next_schema_type = schema_storage_type;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        } else if (next_schema_type != schema_storage_type) {
          next_schema_type = SchemaType::kUnknown;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }
      }
    } else {
      if (resolved_schema_type == SchemaType::kUnknown) {
        const auto inferred_schema_type =
            SchemaData::resolve_type(schema_data.schema_type, schema_data.name, schema_data.encoding);

        if (inferred_schema_type != SchemaType::kUnknown) {
          resolved_schema_type = inferred_schema_type;
        }
      }

      if (resolved_schema_type != SchemaType::kUnknown) {
        if (next_schema_type == SchemaType::kUnknown) {
          next_schema_type = resolved_schema_type;
        } else if VUNLIKELY (next_schema_type != resolved_schema_type) {
          CLOG_E("VCAPWriter: URL [%s] schema changed from [%d] to [%d].",  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                 url.c_str(),                                               // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                 static_cast<int>(next_schema_type), static_cast<int>(resolved_schema_type));
          discard_new_url_entries();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          return false;               // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }
      }
    }
  } else {
    schema_storage_type = SchemaData::resolve_type(schema_storage_type, schema_ser_type, schema_data.encoding);
  }

  std::string storage_schema_key = schema_ser_type;
  storage_schema_key.push_back('\x1F');
  storage_schema_key.append(SchemaData::convert_type(schema_storage_type));

  if (total_url_iter_ret.second) {
    total_url_msg_info.index = impl_->total_url_map.size() - 1;
    total_url_msg_info.url = url;
    impl_->total_url_list.emplace_back(channel_key);
  }

  if (!schema_ser_type.empty() &&
      (schema_storage_type == SchemaType::kProtobuf || schema_storage_type == SchemaType::kFlatbuffers)) {
    std::string schema_record_key = schema_ser_type;
    schema_record_key.push_back('\x1F');
    schema_record_key.append(SchemaData::convert_type(schema_storage_type));

    if (impl_->ser_map.find(schema_record_key) == impl_->ser_map.end()) {
      if (!schema_data.name.empty() && !schema_data.encoding.empty() && !schema_data.data.empty()) {
        mcap::Schema schema;
        schema.id = static_cast<mcap::SchemaId>(impl_->ser_map.size() + 1);
        schema.name = schema_data.name;
        schema.encoding = schema_data.encoding;
        schema.data.assign(reinterpret_cast<const std::byte*>(schema_data.data.data()),
                           reinterpret_cast<const std::byte*>(schema_data.data.data()) + schema_data.data.size());

        impl_->writer->addSchema(schema);

        impl_->ser_map.emplace(schema_record_key, schema.id);
      }
    }
  }

  if (url_iter_ret.second) {
    url_msg_info.index = impl_->url_map.size() - 1;

    if (action_type == ActionType::kClientRequest || action_type == ActionType::kClientResponse ||
        action_type == ActionType::kServerRequest || action_type == ActionType::kServerResponse) {
      impl_->write_url_type = "Method";
    } else if (action_type == ActionType::kPublish || action_type == ActionType::kSubscribe) {
      impl_->write_url_type = "Event";
    } else if (action_type == ActionType::kSet || action_type == ActionType::kGet) {
      impl_->write_url_type = "Field";
    } else {
      impl_->write_url_type = "Unknown";
    }

    mcap::SchemaId schema_id = 0;

    if (impl_->write_url_type != "Method" && !next_ser_type.empty()) {
      auto iter = impl_->ser_map.find(storage_schema_key);

      if (iter != impl_->ser_map.end()) {
        schema_id = iter->second;
      }
    }

    mcap::Channel channel;
    channel.id = url_msg_info.index + 1;
    channel.schemaId = schema_id;
    channel.topic = url;
    channel.metadata["action"] = std::string(convert_action(action_type));

    if (impl_->write_url_type != "Method") {
      if (!schema_data.encoding.empty()) {
        channel.messageEncoding = schema_data.encoding;
      } else if (next_schema_type != SchemaType::kUnknown) {
        channel.messageEncoding = SchemaData::convert_type(next_schema_type);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }
    }

    url_msg_info.index = channel.id - 1;

    impl_->writer->addChannel(channel);

    url_msg_info.url = url;
    url_msg_info.url_type = impl_->write_url_type;
    total_url_msg_info.url_type = impl_->write_url_type;

    if VLIKELY (!next_ser_type.empty()) {
      url_msg_info.ser_type = next_ser_type;
      total_url_msg_info.ser_type = next_ser_type;
    }

    url_msg_info.schema_type = next_schema_type;
    total_url_msg_info.schema_type = next_schema_type;
    url_msg_info.action_type = action_type;
    total_url_msg_info.action_type = action_type;
  } else {
    total_url_msg_info.ser_type = next_ser_type;
    url_msg_info.ser_type = next_ser_type;
    total_url_msg_info.schema_type = next_schema_type;
    url_msg_info.schema_type = next_schema_type;
  }

  // update count
  ++url_msg_info.count;
  ++total_url_msg_info.count;

  // update size
  url_msg_info.size += data.size();
  total_url_msg_info.size += data.size();

  if (action_type == ActionType::kPublish || action_type == ActionType::kSubscribe || action_type == ActionType::kSet ||
      action_type == ActionType::kGet) {
    double time_duration = 0;

    if (total_url_msg_info.first_timestamp < 0) {
      total_url_msg_info.first_timestamp = microseconds_timestamp;
    }

    total_url_msg_info.last_timestamp = microseconds_timestamp;
    time_duration = (total_url_msg_info.last_timestamp - total_url_msg_info.first_timestamp) / 1000'000.0;

    if (time_duration > 0) {
      total_url_msg_info.freq = total_url_msg_info.count / time_duration;
    } else {
      total_url_msg_info.freq = 0;
    }

    if (url_msg_info.first_timestamp < 0) {
      url_msg_info.first_timestamp = microseconds_timestamp;
    }

    url_msg_info.last_timestamp = microseconds_timestamp;
    time_duration = (url_msg_info.last_timestamp - url_msg_info.first_timestamp) / 1000'000.0;

    if (time_duration > 0) {
      url_msg_info.freq = url_msg_info.count / time_duration;
    } else {
      url_msg_info.freq = 0;
    }
  }

  // insert data

  if VUNLIKELY (impl_->last_timestamp > microseconds_timestamp) {
    if (impl_->last_timestamp - microseconds_timestamp < 1000'00U) {
      microseconds_timestamp = impl_->last_timestamp + 1;
    }
  }

  impl_->last_timestamp = microseconds_timestamp;

  mcap::Message message;
  message.channelId = url_msg_info.index + 1;
  message.sequence = url_msg_info.count;
  message.logTime = microseconds_timestamp * 1000U + impl_->start_timestamp * 1000'000;
  message.publishTime = message.logTime;
  message.data = reinterpret_cast<const std::byte*>(data.data());
  message.dataSize = data.size();

  status = impl_->writer->write(message);

  if VUNLIKELY (!status.ok()) {
    CLOG_W("VCAPWriter: Failed to write message data, error = %s.",  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
           status.message.c_str());                                  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;                                                    // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  impl_->cached_size.fetch_add(data.size(), std::memory_order_relaxed);

  ++impl_->current_row;
  impl_->current_size += data.size();

  ++impl_->total_current_row;
  impl_->total_current_size += data.size();
  impl_->total_timestamp = microseconds_timestamp;

  return true;
}

bool VCAPWriter::write_filex(bool complete) {
  try {
#ifdef _WIN32
    std::filesystem::path file_path(Helpers::string_to_wstring(impl_->path));
#else
    std::filesystem::path file_path(impl_->path);
#endif

    nlohmann::ordered_json json;

    json["VLinkHeader"] = {
        {"major", VLINK_VERSION_MAJOR},  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        {"minor", VLINK_VERSION_MINOR},  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        {"patch", VLINK_VERSION_PATCH},  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        {"count", impl_->total_current_row},
        {"duration", impl_->total_timestamp},
        {"accuracy", "MicroSecond"},
        {"compress", impl_->enable_compressed ? "zstd" : "None"},
        {"process", impl_->app_name},
        {"date", get_format_date(&impl_->time_start)},
        {"tag", impl_->tag_name},
        {"split_by_size", impl_->config.split_by_time > 0 ? 0 : impl_->config.split_by_size},
        {"split_by_time", impl_->config.split_by_time},
        {"complete", complete},
        {"timezone", impl_->timezone_diff},
        {"start_timestamp", impl_->start_timestamp},
    };

    nlohmann::ordered_json url_json;

    {
      std::lock_guard lock(sample_mutex());

      for (const auto& channel_key : impl_->total_url_list) {
        const auto& ext_info = impl_->total_url_map[channel_key];
        auto loss = total_url_loss_map_ref()[recover_recorded_url(ext_info.url)];

        url_json.push_back({
            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
            {"index", ext_info.index},
            {"url", ext_info.url},
            {"type", ext_info.url_type},
            {"action", std::string(convert_action(ext_info.action_type))},
            {"ser", ext_info.ser_type},
            {"encoding", std::string(SchemaData::convert_type(ext_info.schema_type))},
            {"count", ext_info.count},
            {"size", ext_info.size},
            {"loss", loss},
            {"freq", ext_info.freq},
        });
      }
    }

    json["VLinkUrls"] = std::move(url_json);

    nlohmann::ordered_json files_json;
    for (const auto& file : impl_->split_file_list) {
      files_json.push_back(file);
    }

    json["VLinkFiles"] = std::move(files_json);

    std::ofstream filex(file_path);

    if VUNLIKELY (!filex.is_open()) {
      return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    filex << json.dump(4);
    filex.close();

    if VUNLIKELY (!filex) {
      return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
  } catch (nlohmann::json::exception& e) {
    VLOG_F("VCAPWriter: Filesystem error, ", e.what(), ".");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;                                             // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

  return true;
}

}  // namespace vlink
