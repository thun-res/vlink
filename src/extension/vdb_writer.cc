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

#include "./extension/vdb_writer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "./base/elapsed_timer.h"
#include "./base/helpers.h"
#include "./base/logger.h"
#include "./version.h"

// json
#include <nlohmann/json.hpp>

#ifdef VLINK_ENABLE_SQLITE
#include <sqlite3.h>
#endif

// schema_plugin
#include "./extension/schema_plugin_interface.h"

namespace vlink {

[[maybe_unused]] static constexpr int get_column(int column) noexcept { return column + 1; }

static constexpr int kSyncWriteInterval = 1000;  // ms
static constexpr int kCompressMaxIgnoreCnt = 5;
static constexpr double kCompressMaxRatio = 0.95;

// VDBWriter::Impl
struct VDBWriter::Impl final {  // NOLINT(clang-analyzer-optin.performance.Padding)
  // UrlMsgInfo
  struct UrlMsgInfo final {
    int index{0};
    size_t count{0};
    size_t size{0};
    int64_t first_timestamp{-1};
    int64_t last_timestamp{-1};
    int64_t previous_segment_last_timestamp{-1};
    double freq{0};
    double loss{0};
    std::string url_type;
    std::string ser_type;
    SchemaType schema_type{SchemaType::kUnknown};

    bool operator<(const UrlMsgInfo& target) const noexcept { return index < target.index; }
  };

  struct WriteStateSnapshot final {
    int64_t current_row{0};
    int64_t current_size{0};
    bool has_oversize{false};
    int64_t last_timestamp{0};
    std::vector<std::string> total_url_list;
    int64_t total_current_row{0};
    int64_t total_current_size{0};
    int64_t total_timestamp{0};
    std::unordered_map<std::string, UrlMsgInfo> url_map;
    std::unordered_set<std::string> ser_map;
    std::unordered_map<std::string, UrlMsgInfo> total_url_map;
    std::unordered_map<std::string, SchemaData> total_schema_map;
    std::unordered_map<std::string, int64_t> compress_ignore_map;
    std::string write_url_type;

    explicit WriteStateSnapshot(const VDBWriter::Impl& impl)
        : current_row(impl.current_row),
          current_size(impl.current_size),
          has_oversize(impl.has_oversize),
          last_timestamp(impl.last_timestamp),
          total_url_list(impl.total_url_list),
          total_current_row(impl.total_current_row),
          total_current_size(impl.total_current_size),
          total_timestamp(impl.total_timestamp),
          url_map(impl.url_map),
          ser_map(impl.ser_map),
          total_url_map(impl.total_url_map),
          total_schema_map(impl.total_schema_map),
          compress_ignore_map(impl.compress_ignore_map),
          write_url_type(impl.write_url_type) {}

    void restore(VDBWriter::Impl& impl) const {
      impl.current_row = current_row;
      impl.current_size = current_size;
      impl.has_oversize = has_oversize;
      impl.last_timestamp = last_timestamp;
      impl.total_url_list = total_url_list;
      impl.total_current_row = total_current_row;
      impl.total_current_size = total_current_size;
      impl.total_timestamp = total_timestamp;
      impl.url_map = url_map;
      impl.ser_map = ser_map;
      impl.total_url_map = total_url_map;
      impl.total_schema_map = total_schema_map;
      impl.compress_ignore_map = compress_ignore_map;
      impl.write_url_type = write_url_type;
    }
  };

  struct MemoryCharge final {
    MemoryCharge(std::atomic<int64_t>& counter, int64_t bytes) : value(&counter), size(bytes) {}

    MemoryCharge(MemoryCharge&& other) noexcept : value(std::exchange(other.value, nullptr)), size(other.size) {}

    ~MemoryCharge() {
      if (value) {
        value->fetch_sub(size, std::memory_order_relaxed);
      }
    }

    std::atomic<int64_t>* value;
    int64_t size;

    VLINK_DISALLOW_COPY_AND_ASSIGN(MemoryCharge)
  };

  std::atomic_bool is_dumping{false};
  std::atomic_bool is_split_mode{false};
  std::atomic<int> split_index{0};
  std::atomic<int64_t> memory_size{0};
  std::atomic_bool in_cached{false};
  std::atomic<int64_t> cached_size{0};
  std::atomic_bool quit_flag{false};

  std::string path;
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
  std::unordered_set<std::string> ser_map;
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

  ElapsedTimer cache_timer;
  Timer check_timer;

  ElapsedTimer sync_timer;

  std::unordered_map<std::string, int64_t> compress_ignore_map;
  std::unique_ptr<WriteStateSnapshot> cache_snapshot;

  std::string write_url_type;

// database
#ifdef VLINK_ENABLE_SQLITE
  ::sqlite3* db{nullptr};
  ::sqlite3_stmt* schemas_stmt{nullptr};
  ::sqlite3_stmt* datas_stmt{nullptr};
  ::sqlite3_stmt* urls_stmt{nullptr};
  ::sqlite3_stmt* update_complete_stmt{nullptr};
  ::sqlite3_stmt* update_header_stmt{nullptr};
  ::sqlite3_stmt* update_url_loss_stmt{nullptr};
  ::sqlite3_stmt* update_url_meta_stmt{nullptr};
  ::sqlite3_stmt* update_urls_stmt{nullptr};
#endif

  // schema plugin interface
  SchemaPluginInterface* schema_plugin_interface{nullptr};
};

// VDBWriter
VDBWriter::VDBWriter(const std::string& path, const Config& config)
    : BagWriter(path, config), impl_(std::make_unique<Impl>()) {
  set_name("VDBWriter");

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

  impl_->enable_compressed = impl_->config.compress == kCompressAuto || impl_->config.compress == kCompressLzav;

  if VUNLIKELY (impl_->config.max_task_depth <= 0) {
    impl_->config.max_task_depth = BagWriter::Config().max_task_depth;
  }

  reset_lockfree_capacity();

  if (!config.sync_mode) {
    impl_->check_timer.attach(this);
    impl_->check_timer.set_interval(kSyncWriteInterval);
    impl_->check_timer.start([this]() {
      // LCOV_EXCL_START GCOVR_EXCL_START
      if VUNLIKELY (impl_->quit_flag.load(std::memory_order_acquire)) {
        return;
      }

      if (impl_->cache_timer.get() > 3000) {
        std::lock_guard lock(impl_->write_mtx);
        sync_cache();
      }
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    });
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

    if (suffix == ".vdbx") {
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
            if (!file_info.is_string()) {
              continue;
            }

            const auto stale_file_name = file_info.get<std::string>();
#ifdef _WIN32
            const std::filesystem::path stale_file_path(Helpers::string_to_wstring(stale_file_name));
#else
            const std::filesystem::path stale_file_path(stale_file_name);
#endif

            if (stale_file_path.empty() || stale_file_path == "." || stale_file_path == ".." ||
                stale_file_path != stale_file_path.filename()) {
              CLOG_W("VDBWriter: Ignore unsafe split file path [%s].", stale_file_name.c_str());
              continue;
            }

            const auto stale_output_path = parent_path / stale_file_path;
            std::error_code remove_ec;
            std::filesystem::remove(stale_output_path, remove_ec);

            if VUNLIKELY (remove_ec) {
              CLOG_W("VDBWriter: Failed to remove stale split path [%s]: %s.", stale_file_name.c_str(),
                     remove_ec.message().c_str());
            }
          }

          std::filesystem::remove(file_path);
        } catch (nlohmann::json::exception&) {
        }
      }

      impl_->is_split_mode.store(true, std::memory_order_release);
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
          impl_->split_filename = get_format_date(&impl_->time_current, true) + ".vdb";
        } else {
          impl_->split_filename = impl_->base_dir + "/" + get_format_date(&impl_->time_current, true) + ".vdb";
        }
      } else {
        impl_->split_filename =
            impl_->base_name + "." + std::to_string(impl_->split_index.load(std::memory_order_relaxed) + 1) + ".vdb";
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

      impl_->is_split_mode.store(false, std::memory_order_release);
      impl_->split_index.store(0, std::memory_order_relaxed);

      open(path);
    }
  } catch (std::filesystem::filesystem_error& e) {
    VLOG_F("VDBWriter: Filesystem error during init, ", e.what(), ".");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return;                                                              // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

  impl_->elapsed_timer.start();
  impl_->sync_timer.start();

#ifndef VLINK_ENABLE_SQLITE
  VLOG_F("VDBWriter: The compile macro VLINK_ENABLE_SQLITE is not turned on.");
#endif
}  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

VDBWriter::~VDBWriter() {
  detach_plugin();

  impl_->quit_flag.store(true, std::memory_order_release);

  impl_->check_timer.stop();

  if VUNLIKELY (!wait_for_idle(30000U)) {
    VLOG_W("VDBWriter: Force to quit.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

#ifdef VLINK_ENABLE_SQLITE

  if VLIKELY (impl_->db) {
    ::sqlite3_interrupt(impl_->db);
  }
#endif

  quit(true);

  wait_for_quit();

  close();
}

void VDBWriter::close() {
  close_segment();

  if VUNLIKELY (impl_->is_split_mode.load(std::memory_order_acquire) && !write_filex(true)) {
    set_fail();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }
}

void VDBWriter::register_split_callback(SplitCallback&& callback, bool before) {
  std::lock_guard lock(impl_->split_mtx);
  impl_->split_before = before;
  impl_->split_callback = std::move(callback);
}

void VDBWriter::register_schema_callback(SchemaCallback&& callback) {
  std::lock_guard lock(impl_->write_mtx);
  impl_->schema_callback = std::move(callback);
}

bool VDBWriter::merge_schema(SchemaData& schema_data) {
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
    if (!schema_data.encoding.empty() && !schema_data.data.empty() &&
        impl_->ser_map.find(schema_key) == impl_->ser_map.end()) {
      if VUNLIKELY (!insert_schema(schema_data)) {
        return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      impl_->ser_map.emplace(schema_key);
    }

    impl_->total_schema_map.emplace(schema_key, schema_data);

    return true;
  }

  const auto& current = schema_iter->second;

  if VUNLIKELY ((!schema_data.encoding.empty() && !current.encoding.empty() &&
                 current.encoding != schema_data.encoding) ||
                (!schema_data.data.empty() && !current.data.empty() && current.data != schema_data.data) ||
                (SchemaData::is_real_type(resolved_schema_type) && SchemaData::is_real_type(current.schema_type) &&
                 current.schema_type != resolved_schema_type)) {
    CLOG_E("VDBWriter: Conflicting schema pushed for [%s].", schema_data.name.c_str());
    return false;
  }

  SchemaData merged_schema = current;

  if (merged_schema.encoding.empty() && !schema_data.encoding.empty()) {
    merged_schema.encoding = schema_data.encoding;
  }

  if (merged_schema.data.empty() && !schema_data.data.empty()) {
    merged_schema.data = schema_data.data;
  }

  if (!SchemaData::is_real_type(merged_schema.schema_type) && SchemaData::is_real_type(resolved_schema_type)) {
    merged_schema.schema_type = resolved_schema_type;
  }

  if (!merged_schema.encoding.empty() && !merged_schema.data.empty() &&
      impl_->ser_map.find(schema_key) == impl_->ser_map.end()) {
    if VUNLIKELY (!insert_schema(merged_schema)) {
      return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    impl_->ser_map.emplace(schema_key);
  }

  schema_data = merged_schema;

  if (schema_iter->first != schema_key && merged_schema.schema_type == resolved_schema_type) {
    impl_->total_schema_map.erase(schema_iter);
    impl_->total_schema_map.emplace(schema_key, schema_data);
  } else {
    schema_iter->second = schema_data;
  }

  return true;
}

bool VDBWriter::load_schema(const std::string& ser_type, SchemaType& schema_type, SchemaData& schema_data) {
#ifdef VLINK_ENABLE_SQLITE
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
    CLOG_E("VDBWriter: Schema family mismatch for [%s], requested = %d, resolved = %d.", ser_type.c_str(),
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
#else
  (void)ser_type;
  (void)schema_type;
  (void)schema_data;
  return false;
#endif
}

bool VDBWriter::push_schema(const SchemaData& schema_data) {
  SchemaData stored_schema = schema_data;

  if VUNLIKELY (!stored_schema.data.is_owner()) {
    stored_schema.data.deep_copy(schema_data.data);
  }

  if (impl_->config.sync_mode) {
    std::lock_guard lock(impl_->write_mtx);
    return merge_schema(stored_schema);
  }

  bool posted = post_persistent_task([this, stored_schema = std::move(stored_schema)]() mutable {
    std::lock_guard lock(impl_->write_mtx);

    if VUNLIKELY (!merge_schema(stored_schema)) {
      CLOG_E("VDBWriter: Deferred merge_schema failed for [%s] in async push_schema path.", stored_schema.name.c_str());
      set_fail();
    }
  });

  return posted;
}

int64_t VDBWriter::record(const Frame& frame, int64_t timestamp) {
#ifdef VLINK_ENABLE_SQLITE

  const std::string& url = frame.url;
  const std::string& ser_type = frame.ser_type;
  const SchemaType schema_type = frame.schema_type;
  const ActionType action_type = frame.action_type;
  const Bytes& data = frame.data;
  const int64_t microseconds_timestamp = timestamp;

  if (impl_->config.sync_mode) {
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
    Impl::MemoryCharge memory_charge(impl_->memory_size, queued_size);

    bool posted = post_persistent_task([this, url_index, ser_index, schema_type, action_type, data,
                                        memory_charge = std::move(memory_charge),
                                        microseconds_timestamp]() {  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      (void)memory_charge;

      std::string url;
      std::string ser_type;

      std::lock_guard lock(impl_->write_mtx);
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
#else
  (void)frame;
  (void)timestamp;
  return -1;
#endif
}

int64_t VDBWriter::get_record_timestamp() const { return impl_->elapsed_timer.get(); }

bool VDBWriter::is_dumping() const { return impl_->is_dumping.load(std::memory_order_relaxed); }

bool VDBWriter::is_split_mode() const { return impl_->is_split_mode.load(std::memory_order_acquire); }

int VDBWriter::get_split_index() const { return impl_->split_index.load(std::memory_order_relaxed); }

size_t VDBWriter::get_max_task_count() const { return impl_->config.max_task_depth; }

void VDBWriter::on_begin() {
  MessageLoop::on_begin();

  impl_->elapsed_timer.restart();
  impl_->sync_timer.restart();
}

void VDBWriter::on_end() { MessageLoop::on_end(); }

void VDBWriter::open(const std::string& path) {
#ifdef VLINK_ENABLE_SQLITE
  try {
#ifdef _WIN32
    impl_->split_file_list.emplace_back(Helpers::path_to_string(std::filesystem::path(path).filename()));
    std::filesystem::path file_path(Helpers::string_to_wstring(path));
#else
    impl_->split_file_list.emplace_back(std::filesystem::path(path).filename().string());
    std::filesystem::path file_path(path);
#endif

    if (std::filesystem::exists(file_path)) {
      std::filesystem::remove(file_path);
    } else {
      auto parent_path = file_path.parent_path();

      if (!parent_path.empty() && !std::filesystem::exists(parent_path)) {
        std::filesystem::create_directories(parent_path);
      }
    }
  } catch (std::filesystem::filesystem_error& e) {
    VLOG_F("VDBWriter: Filesystem error during file preparation, ", e.what(), ".");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return;                                                                          // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

  int ret = 0;
  char* err_msg = nullptr;

  auto free_err_msg = [&err_msg]() noexcept {
    if (err_msg) {
      ::sqlite3_free(err_msg);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      err_msg = nullptr;        // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
  };

  auto finalize_stmt = [](sqlite3_stmt*& stmt) noexcept {
    if (stmt) {
      ::sqlite3_finalize(stmt);
      stmt = nullptr;
    }
  };

  auto close_db = [this, &finalize_stmt]() noexcept {
    // LCOV_EXCL_START GCOVR_EXCL_START
    if (!impl_->db) {
      return;
    }

    if (impl_->in_cached.load(std::memory_order_relaxed)) {
      ::sqlite3_exec(impl_->db, "ROLLBACK;", nullptr, nullptr, nullptr);
      impl_->in_cached.store(false, std::memory_order_relaxed);
      impl_->cached_size.store(0, std::memory_order_relaxed);
      impl_->cache_timer.stop();
    }

    finalize_stmt(impl_->schemas_stmt);
    finalize_stmt(impl_->datas_stmt);
    finalize_stmt(impl_->urls_stmt);
    finalize_stmt(impl_->update_complete_stmt);
    finalize_stmt(impl_->update_header_stmt);
    finalize_stmt(impl_->update_url_loss_stmt);
    finalize_stmt(impl_->update_url_meta_stmt);
    finalize_stmt(impl_->update_urls_stmt);

    const char* err_ptr = ::sqlite3_errmsg(impl_->db);
    std::string close_err = err_ptr ? err_ptr : std::string{};
    int close_ret = ::sqlite3_close_v2(impl_->db);

    if VUNLIKELY (close_ret != SQLITE_OK) {
      CLOG_W("Failed to close database (rc=%d): %s.", close_ret, close_err.c_str());
    }

    impl_->db = nullptr;
  };
  // LCOV_EXCL_STOP GCOVR_EXCL_STOP

  // open db
  ret = ::sqlite3_open_v2(path.c_str(), &impl_->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);

  if VUNLIKELY (ret != SQLITE_OK) {
    CLOG_F("Failed to open database [%s].", path.c_str());  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    close_db();                                             // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return;                                                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  // opt busy_timeout
  ::sqlite3_busy_timeout(impl_->db, 100);

  // opt sqlite temp_store
  ret = ::sqlite3_exec(impl_->db, "PRAGMA temp_store = MEMORY;", nullptr, nullptr, &err_msg);

  if VUNLIKELY (ret != SQLITE_OK) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    CLOG_F("Failed to set temp_store: %s.", err_msg);

    free_err_msg();
    close_db();

    return;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  // opt sqlite page_size
  ret = ::sqlite3_exec(impl_->db, "PRAGMA page_size = 16384;", nullptr, nullptr, &err_msg);

  if VUNLIKELY (ret != SQLITE_OK) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    CLOG_F("Failed to set page_size: %s.", err_msg);

    free_err_msg();
    close_db();

    return;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  // opt sqlite cache_size
  ret = ::sqlite3_exec(impl_->db, "PRAGMA cache_size = 8192;", nullptr, nullptr, &err_msg);

  if VUNLIKELY (ret != SQLITE_OK) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    CLOG_F("Failed to set cache_size: %s.", err_msg);

    free_err_msg();
    close_db();

    return;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  // opt sqlite synchronous
  ret = ::sqlite3_exec(impl_->db, "PRAGMA synchronous = OFF;", nullptr, nullptr, &err_msg);

  if VUNLIKELY (ret != SQLITE_OK) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    CLOG_F("Failed to set synchronous: %s.", err_msg);

    free_err_msg();
    close_db();

    return;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  // opt sqlite journal_mode

  if (impl_->config.wal_mode) {
    ret = ::sqlite3_exec(impl_->db, "PRAGMA journal_mode = WAL;", nullptr, nullptr, &err_msg);
    if VUNLIKELY (ret != SQLITE_OK) {
      // LCOV_EXCL_START GCOVR_EXCL_START
      CLOG_F("Failed to set journal_mode: %s.", err_msg);

      free_err_msg();
      close_db();

      return;
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    }
  } else {
    ret = ::sqlite3_exec(impl_->db, "PRAGMA journal_mode = OFF;", nullptr, nullptr, &err_msg);
    if VUNLIKELY (ret != SQLITE_OK) {
      // LCOV_EXCL_START GCOVR_EXCL_START
      CLOG_F("Failed to set journal_mode: %s.", err_msg);

      free_err_msg();
      close_db();

      return;
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    }
  }

  // opt sqlite automatic_index
  ret = ::sqlite3_exec(impl_->db, "PRAGMA automatic_index = OFF;", nullptr, nullptr, &err_msg);

  if VUNLIKELY (ret != SQLITE_OK) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    CLOG_F("Failed to set automatic_index: %s.", err_msg);

    free_err_msg();
    close_db();

    return;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  // opt sqlite locking_mode
  ret = ::sqlite3_exec(impl_->db, "PRAGMA locking_mode = EXCLUSIVE;", nullptr, nullptr, &err_msg);

  if VUNLIKELY (ret != SQLITE_OK) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    CLOG_F("Failed to set locking_mode: %s.", err_msg);

    free_err_msg();
    close_db();

    return;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  // set busy_timeout
  ::sqlite3_busy_timeout(impl_->db, 500);

  begin_cache();

  // create header table
  ret = ::sqlite3_exec(impl_->db,
                       "CREATE TABLE IF NOT EXISTS VLinkHeader(major INTEGER, minor INTEGER, patch INTEGER, "
                       "count INTEGER, duration INTEGER, accuracy TEXT, compress TEXT, process TEXT, date TEXT, "
                       "tag TEXT, complete INTEGER, timezone INTEGER, start_timestamp INTEGER);",
                       nullptr, nullptr, &err_msg);

  if VUNLIKELY (ret != SQLITE_OK) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    CLOG_F("Failed to create header table: %s.", err_msg);

    free_err_msg();
    close_db();

    return;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  // prepare header table
  ::sqlite3_stmt* header_stmt = nullptr;
  ret = ::sqlite3_prepare_v2(
      impl_->db,
      "INSERT INTO VLinkHeader (major, minor, patch, count, duration, accuracy, compress, "
      "process, date, tag, complete, timezone, start_timestamp) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
      -1, &header_stmt, nullptr);

  if VUNLIKELY (ret != SQLITE_OK) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    CLOG_F("Failed to prepare header table: %s.", ::sqlite3_errmsg(impl_->db));

    free_err_msg();
    finalize_stmt(header_stmt);
    close_db();

    return;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  // get date
  const std::string& date_str = get_format_date(&impl_->time_current);

  // insert header table
  ::sqlite3_bind_int(header_stmt, get_column(0), VLINK_VERSION_MAJOR);
  ::sqlite3_bind_int(header_stmt, get_column(1), VLINK_VERSION_MINOR);
  ::sqlite3_bind_int(header_stmt, get_column(2), VLINK_VERSION_PATCH);
  ::sqlite3_bind_int64(header_stmt, get_column(3), 0);
  ::sqlite3_bind_int64(header_stmt, get_column(4), 0);
  ::sqlite3_bind_text(header_stmt, get_column(5), "MicroSecond", -1, SQLITE_STATIC);
  ::sqlite3_bind_text(header_stmt, get_column(6), impl_->enable_compressed ? "lzav" : "None", -1, SQLITE_STATIC);
  ::sqlite3_bind_text(header_stmt, get_column(7), impl_->app_name.data(), impl_->app_name.size(), SQLITE_STATIC);
  ::sqlite3_bind_text(header_stmt, get_column(8), date_str.data(), date_str.size(), SQLITE_STATIC);
  ::sqlite3_bind_text(header_stmt, get_column(9), impl_->tag_name.data(), impl_->tag_name.size(), SQLITE_STATIC);
  ::sqlite3_bind_int(header_stmt, get_column(10), 0);
  ::sqlite3_bind_int(header_stmt, get_column(11), impl_->timezone_diff);
  ::sqlite3_bind_int64(header_stmt, get_column(12), impl_->start_timestamp);

  ret = ::sqlite3_step(header_stmt);

  if VUNLIKELY (ret != SQLITE_DONE) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    CLOG_F("Failed to insert header table: %s.", ::sqlite3_errmsg(impl_->db));

    free_err_msg();
    finalize_stmt(header_stmt);
    close_db();

    return;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  finalize_stmt(header_stmt);

  // create schema table
  ret = ::sqlite3_exec(impl_->db, "CREATE TABLE IF NOT EXISTS VLinkSchemas(ser TEXT, encoding TEXT, data BLOB);",
                       nullptr, nullptr, &err_msg);

  if VUNLIKELY (ret != SQLITE_OK) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    CLOG_F("Failed to create schema table: %s.", err_msg);

    free_err_msg();
    close_db();

    return;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  // prepare schema table
  ret = ::sqlite3_prepare_v2(impl_->db, "INSERT INTO VLinkSchemas(ser, encoding, data) VALUES (?, ?, ?);", -1,
                             &impl_->schemas_stmt, nullptr);

  if VUNLIKELY (ret != SQLITE_OK) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    CLOG_F("Failed to prepare schema table: %s.", ::sqlite3_errmsg(impl_->db));

    free_err_msg();
    close_db();

    return;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  // create url table
  ret = ::sqlite3_exec(impl_->db,
                       "CREATE TABLE IF NOT EXISTS VLinkUrls(id INTEGER, url TEXT, type TEXT, ser TEXT, encoding TEXT, "
                       "count INTEGER, loss REAL, size INTEGER, freq REAL);",
                       nullptr, nullptr, &err_msg);

  if VUNLIKELY (ret != SQLITE_OK) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    CLOG_F("Failed to create urls table: %s.", err_msg);

    free_err_msg();
    close_db();

    return;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  // prepare url table
  ret = ::sqlite3_prepare_v2(impl_->db,
                             "INSERT INTO VLinkUrls (id, url, type, ser, encoding, count, loss, size, freq) VALUES "
                             "(?, ?, ?, ?, ?, ?, ?, ?, ?);",
                             -1, &impl_->urls_stmt, nullptr);

  if VUNLIKELY (ret != SQLITE_OK) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    CLOG_F("Failed to prepare urls table: %s.", ::sqlite3_errmsg(impl_->db));

    free_err_msg();
    close_db();

    return;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  // create data table
  ret = ::sqlite3_exec(impl_->db,
                       "CREATE TABLE IF NOT EXISTS VLinkDatas(elapsed INTEGER, url INTEGER, action TEXT, data BLOB);",
                       nullptr, nullptr, &err_msg);

  if VUNLIKELY (ret != SQLITE_OK) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    CLOG_F("Failed to create datas table: %s.", err_msg);

    free_err_msg();
    close_db();

    return;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  // prepare data table
  ret = ::sqlite3_prepare_v2(impl_->db, "INSERT INTO VLinkDatas (elapsed, url, action, data) VALUES (?, ?, ?, ?);", -1,
                             &impl_->datas_stmt, nullptr);

  if VUNLIKELY (ret != SQLITE_OK) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    CLOG_F("Failed to prepare datas table: %s.", ::sqlite3_errmsg(impl_->db));

    free_err_msg();
    close_db();

    return;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  // prepare update_header table
  ret = sqlite3_prepare_v2(impl_->db, "UPDATE VLinkHeader SET count=?, duration=? WHERE ROWID=1;", -1,
                           &impl_->update_header_stmt, nullptr);

  if VUNLIKELY (ret != SQLITE_OK) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    CLOG_F("Failed to prepare update_header: %s.", ::sqlite3_errmsg(impl_->db));

    free_err_msg();
    close_db();

    return;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  // prepare update_url table
  ret = sqlite3_prepare_v2(impl_->db, "UPDATE VLinkUrls SET count=?, size=?, freq=? WHERE url=?;", -1,
                           &impl_->update_urls_stmt, nullptr);

  if VUNLIKELY (ret != SQLITE_OK) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    CLOG_F("Failed to prepare update_urls: %s.", ::sqlite3_errmsg(impl_->db));

    free_err_msg();
    close_db();

    return;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  // create idx_elapsed_url
  ret = ::sqlite3_exec(impl_->db, "CREATE INDEX IF NOT EXISTS idx_elapsed_url ON VLinkDatas(elapsed, url);", nullptr,
                       nullptr, &err_msg);

  if VUNLIKELY (ret != SQLITE_OK) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    CLOG_F("Failed to create datas idx_elapsed_url: %s.", err_msg);

    free_err_msg();
    close_db();

    return;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  ret = sqlite3_prepare_v2(impl_->db, "UPDATE VLinkUrls SET ser=?, encoding=? WHERE url=?;", -1,
                           &impl_->update_url_meta_stmt, nullptr);

  if VUNLIKELY (ret != SQLITE_OK) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    CLOG_F("Failed to prepare update_url_meta: %s.", ::sqlite3_errmsg(impl_->db));

    free_err_msg();
    close_db();

    return;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  ret = sqlite3_prepare_v2(impl_->db, "UPDATE VLinkUrls SET loss=? WHERE url=?;", -1, &impl_->update_url_loss_stmt,
                           nullptr);

  if VUNLIKELY (ret != SQLITE_OK) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    CLOG_F("Failed to prepare update_url_loss: %s.", ::sqlite3_errmsg(impl_->db));

    free_err_msg();
    close_db();

    return;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  ret = sqlite3_prepare_v2(impl_->db, "UPDATE VLinkHeader SET complete=1 WHERE ROWID=1;", -1,
                           &impl_->update_complete_stmt, nullptr);

  if VUNLIKELY (ret != SQLITE_OK) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    CLOG_F("Failed to prepare update_complete: %s.", ::sqlite3_errmsg(impl_->db));

    free_err_msg();
    close_db();

    return;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  free_err_msg();

  sync_cache();

#else
  (void)path;
  VLOG_F("VDBWriter: The compile macro VLINK_ENABLE_SQLITE is not turned on.");
#endif

  impl_->last_timestamp = 0;
}

void VDBWriter::open_split(const std::string& path) {
#ifdef _WIN32
  const auto file_name = std::filesystem::path(Helpers::string_to_wstring(path)).filename();
  open(Helpers::path_to_string(impl_->split_output_dir / file_name));
#else
  open((impl_->split_output_dir / std::filesystem::path(path).filename()).string());
#endif
}

void VDBWriter::close_segment() {
#ifdef VLINK_ENABLE_SQLITE

  if (!impl_->db) {
    return;
  }

  bool close_success = sync_cache();

  if (!close_success) {
    if (impl_->in_cached.load(std::memory_order_relaxed)) {  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      rollback_cache();                                      // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
  } else {
    close_success = begin_cache();
  }

  if (close_success) {
    int ret = SQLITE_OK;

    for (const auto& [url, info] : impl_->url_map) {
      ::sqlite3_bind_int64(impl_->update_urls_stmt, get_column(0), info.count);
      ::sqlite3_bind_int64(impl_->update_urls_stmt, get_column(1), info.size);
      ::sqlite3_bind_double(impl_->update_urls_stmt, get_column(2), info.freq);
      ::sqlite3_bind_text(impl_->update_urls_stmt, get_column(3), url.c_str(), url.size(), SQLITE_STATIC);

      ret = ::sqlite3_step(impl_->update_urls_stmt);

      if VUNLIKELY (ret != SQLITE_DONE) {
        CLOG_W("Failed to update urls table: %s.", ::sqlite3_errmsg(impl_->db));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        close_success = false;                                                    // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      ret = ::sqlite3_reset(impl_->update_urls_stmt);

      if VUNLIKELY (ret != SQLITE_OK) {
        CLOG_W("Failed to reset urls table: %s.", ::sqlite3_errmsg(impl_->db));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        close_success = false;                                                   // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      if VUNLIKELY (!close_success) {
        break;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }
    }

    if (close_success) {
      ::sqlite3_bind_int64(impl_->update_header_stmt, get_column(0), impl_->current_row);
      ::sqlite3_bind_int64(impl_->update_header_stmt, get_column(1), impl_->last_timestamp);

      ret = ::sqlite3_step(impl_->update_header_stmt);

      if VUNLIKELY (ret != SQLITE_DONE) {
        CLOG_W("Failed to update header table: %s.", ::sqlite3_errmsg(impl_->db));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        close_success = false;                                                      // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      ret = ::sqlite3_reset(impl_->update_header_stmt);

      if VUNLIKELY (ret != SQLITE_OK) {
        CLOG_W("Failed to reset header table: %s.", ::sqlite3_errmsg(impl_->db));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        close_success = false;                                                     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }
    }

    if (close_success) {
      std::lock_guard lock(sample_mutex());

      for (const auto& [url, loss] : url_loss_map_ref()) {
        for (const auto& recorded_url : recorded_urls_for_origin(url)) {
          ::sqlite3_bind_double(impl_->update_url_loss_stmt, get_column(0), loss);
          ::sqlite3_bind_text(impl_->update_url_loss_stmt, get_column(1), recorded_url.c_str(), recorded_url.size(),
                              SQLITE_STATIC);

          ret = ::sqlite3_step(impl_->update_url_loss_stmt);

          if VUNLIKELY (ret != SQLITE_DONE) {
            CLOG_W("Failed to update url loss: %s.", ::sqlite3_errmsg(impl_->db));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
            close_success = false;                                                  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          }

          ret = ::sqlite3_reset(impl_->update_url_loss_stmt);

          if VUNLIKELY (ret != SQLITE_OK) {
            CLOG_W("Failed to reset url loss: %s.", ::sqlite3_errmsg(impl_->db));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
            close_success = false;                                                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          }

          if VUNLIKELY (!close_success) {
            break;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          }
        }

        if VUNLIKELY (!close_success) {
          break;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }
      }
    }

    if (close_success) {
      ret = ::sqlite3_step(impl_->update_complete_stmt);

      if VUNLIKELY (ret != SQLITE_DONE) {
        CLOG_W("Failed to mark database complete: %s.", ::sqlite3_errmsg(impl_->db));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        close_success = false;                                                         // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      ret = ::sqlite3_reset(impl_->update_complete_stmt);

      if VUNLIKELY (ret != SQLITE_OK) {
        CLOG_W("Failed to reset complete flag: %s.", ::sqlite3_errmsg(impl_->db));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        close_success = false;                                                      // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }
    }

    if (close_success) {
      close_success = sync_cache();
    }
  }

  if VUNLIKELY (!close_success) {
    set_fail();

    if (impl_->in_cached.load(std::memory_order_relaxed)) {
      rollback_cache();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
  }

  if VLIKELY (impl_->datas_stmt) {
    ::sqlite3_finalize(impl_->datas_stmt);
    impl_->datas_stmt = nullptr;
  }

  if VLIKELY (impl_->urls_stmt) {
    ::sqlite3_finalize(impl_->urls_stmt);
    impl_->urls_stmt = nullptr;
  }

  if VLIKELY (impl_->schemas_stmt) {
    ::sqlite3_finalize(impl_->schemas_stmt);
    impl_->schemas_stmt = nullptr;
  }

  if VLIKELY (impl_->update_urls_stmt) {
    ::sqlite3_finalize(impl_->update_urls_stmt);
    impl_->update_urls_stmt = nullptr;
  }

  if VLIKELY (impl_->update_url_meta_stmt) {
    ::sqlite3_finalize(impl_->update_url_meta_stmt);
    impl_->update_url_meta_stmt = nullptr;
  }

  if VLIKELY (impl_->update_url_loss_stmt) {
    ::sqlite3_finalize(impl_->update_url_loss_stmt);
    impl_->update_url_loss_stmt = nullptr;
  }

  if VLIKELY (impl_->update_header_stmt) {
    ::sqlite3_finalize(impl_->update_header_stmt);
    impl_->update_header_stmt = nullptr;
  }

  if VLIKELY (impl_->update_complete_stmt) {
    ::sqlite3_finalize(impl_->update_complete_stmt);
    impl_->update_complete_stmt = nullptr;
  }

  if (impl_->config.wal_mode) {
    ::sqlite3_exec(impl_->db, "PRAGMA journal_mode = OFF;", nullptr, nullptr, nullptr);
  }

  if (impl_->config.optimize_on_exit) {
    ::sqlite3_exec(impl_->db, "PRAGMA optimize;", nullptr, nullptr, nullptr);
  }

  if VLIKELY (impl_->db) {
    const char* err_ptr = ::sqlite3_errmsg(impl_->db);
    std::string close_err = err_ptr ? err_ptr : std::string{};
    int ret = ::sqlite3_close_v2(impl_->db);

    if VUNLIKELY (ret != SQLITE_OK) {
      CLOG_W("Failed to close database (rc=%d): %s.", ret, close_err.c_str());  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      set_fail();                                                               // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    impl_->db = nullptr;
  }

  impl_->url_map.clear();
  impl_->ser_map.clear();
  url_loss_map_ref().clear();

  impl_->current_row = 0;
  impl_->current_size = 0;
  impl_->has_oversize = false;

  impl_->in_cached.store(false, std::memory_order_relaxed);
  impl_->cached_size.store(0, std::memory_order_relaxed);
  impl_->cache_snapshot.reset();
#endif

  impl_->last_timestamp = 0;
}

bool VDBWriter::write(const std::string& url, const std::string& ser_type, SchemaType schema_type,
                      ActionType action_type, const Bytes& data, int64_t microseconds_timestamp) {
#ifdef VLINK_ENABLE_SQLITE

  if VUNLIKELY (!impl_->db) {
    VLOG_W("VDBWriter: Sqlite not open.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;                           // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if VUNLIKELY (impl_->is_split_mode.load(std::memory_order_acquire) && !impl_->split_first) {
    impl_->split_first = true;

    std::lock_guard split_lock(impl_->split_mtx);

    if (!impl_->split_before && impl_->split_callback && impl_->split_index.load(std::memory_order_relaxed) == 0) {
      impl_->split_callback(0, impl_->split_filename);
    }
  }

  int ret = 0;

  bool do_compress = false;
  Bytes compressed_data;
  bool do_split = false;

  if VUNLIKELY (!begin_cache()) {
    return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  // delete data, when limit
  while (impl_->current_row > impl_->config.max_row_count || impl_->current_size > impl_->config.max_bytes_size) {
    if VUNLIKELY (!impl_->has_oversize) {
      if (impl_->config.enable_limit) {
        VLOG_W("VDBWriter: The number of messages has reached the upper limit, the oldest data will be deleted.");
      } else {
        VLOG_W("VDBWriter: The number of messages has reached the upper limit, data after that will be ignored.");
      }

      impl_->has_oversize = true;
    }

    if (!impl_->config.enable_limit) {
      rollback_cache();
      return false;
    }

    ::sqlite3_stmt* select_stmt = nullptr;
    ::sqlite3_stmt* delete_row_stmt = nullptr;
    ret = ::sqlite3_prepare_v2(impl_->db, "SELECT rowid, url, length(data) FROM VLinkDatas ORDER BY rowid LIMIT 1;", -1,
                               &select_stmt, nullptr);

    if VLIKELY (ret == SQLITE_OK) {
      ret = ::sqlite3_step(select_stmt);

      if VLIKELY (ret == SQLITE_ROW) {
        const auto erase_row_id = ::sqlite3_column_int64(select_stmt, 0);
        const auto erase_url_id = ::sqlite3_column_int(select_stmt, 1);
        const auto erase_blob_size = ::sqlite3_column_int64(select_stmt, 2);
        auto erase_size = static_cast<size_t>(erase_blob_size);

        if (impl_->enable_compressed && erase_blob_size >= 13) {
          std::array<uint8_t, 13> erase_data{};
          ::sqlite3_blob* erase_blob = nullptr;
          ret = ::sqlite3_blob_open(impl_->db, "main", "VLinkDatas", "data", erase_row_id, 0, &erase_blob);

          if VLIKELY (ret == SQLITE_OK) {
            ret = ::sqlite3_blob_read(erase_blob, erase_data.data(), 9, 0);
          }

          if VLIKELY (ret == SQLITE_OK) {
            ret = ::sqlite3_blob_read(erase_blob, erase_data.data() + 9, 4, static_cast<int>(erase_blob_size) - 4);
          }

          if (erase_blob) {
            const auto close_ret = ::sqlite3_blob_close(erase_blob);
            if (ret == SQLITE_OK) {
              ret = close_ret;
            }
          }

          if VUNLIKELY (ret != SQLITE_OK) {
            CLOG_W("VDBWriter: Failed to read compressed data metadata while evicting: %s.",
                   ::sqlite3_errmsg(impl_->db));
            ::sqlite3_finalize(select_stmt);
            rollback_cache();
            return false;
          }

          if (Bytes::is_compress_data(erase_data.data(), erase_data.size())) {
            erase_size = (static_cast<size_t>(erase_data[4]) << 24U) | (static_cast<size_t>(erase_data[5]) << 16U) |
                         (static_cast<size_t>(erase_data[6]) << 8U) | static_cast<size_t>(erase_data[7]);
          }
        }

        auto total_url_iter = impl_->total_url_map.end();

        if VLIKELY (erase_url_id >= 0 && static_cast<size_t>(erase_url_id) < impl_->total_url_list.size()) {
          total_url_iter = impl_->total_url_map.find(impl_->total_url_list[static_cast<size_t>(erase_url_id)]);
        }

        if VUNLIKELY (total_url_iter == impl_->total_url_map.end()) {
          CLOG_W("VDBWriter: URL metadata not found while evicting the oldest data.");
          ::sqlite3_finalize(select_stmt);
          rollback_cache();
          return false;
        }

        auto url_iter = impl_->url_map.find(total_url_iter->first);

        if VUNLIKELY (url_iter == impl_->url_map.end()) {
          CLOG_W("VDBWriter: Segment URL metadata not found while evicting the oldest data.");
          ::sqlite3_finalize(select_stmt);
          rollback_cache();
          return false;
        }

        ::sqlite3_finalize(select_stmt);
        select_stmt = nullptr;

        ret = ::sqlite3_prepare_v2(impl_->db, "DELETE FROM VLinkDatas WHERE rowid = ?;", -1, &delete_row_stmt, nullptr);

        if VLIKELY (ret == SQLITE_OK) {
          ::sqlite3_bind_int64(delete_row_stmt, get_column(0), erase_row_id);
          ret = ::sqlite3_step(delete_row_stmt);
        }

        if VLIKELY (ret == SQLITE_DONE) {
          --impl_->current_row;
          impl_->current_size -= static_cast<int64_t>(erase_size);
          --impl_->total_current_row;
          impl_->total_current_size -= static_cast<int64_t>(erase_size);

          auto& url_info = url_iter->second;
          auto& total_url_info = total_url_iter->second;
          --url_info.count;
          url_info.size -= erase_size;
          --total_url_info.count;
          total_url_info.size -= erase_size;

          if (url_info.count == 0) {
            url_info.first_timestamp = -1;
            url_info.last_timestamp = -1;
            url_info.freq = 0;
          } else if (url_info.first_timestamp >= 0) {
            if (url_info.count == 1) {
              url_info.first_timestamp = url_info.last_timestamp;
              url_info.freq = 0;
            } else {
              ::sqlite3_stmt* timestamp_stmt = nullptr;
              ret = ::sqlite3_prepare_v2(
                  impl_->db, "SELECT elapsed FROM VLinkDatas WHERE rowid > ? AND url = ? ORDER BY rowid LIMIT 1;", -1,
                  &timestamp_stmt, nullptr);

              if VLIKELY (ret == SQLITE_OK) {
                ::sqlite3_bind_int64(timestamp_stmt, get_column(0), erase_row_id);
                ::sqlite3_bind_int(timestamp_stmt, get_column(1), erase_url_id);
                ret = ::sqlite3_step(timestamp_stmt);
              }

              if VUNLIKELY (ret != SQLITE_ROW) {
                CLOG_W("VDBWriter: Failed to refresh URL metadata after eviction: %s.", ::sqlite3_errmsg(impl_->db));
                if (timestamp_stmt) {
                  ::sqlite3_finalize(timestamp_stmt);
                }
                ::sqlite3_finalize(delete_row_stmt);
                rollback_cache();
                return false;
              }

              url_info.first_timestamp = ::sqlite3_column_int64(timestamp_stmt, 0);
              ::sqlite3_finalize(timestamp_stmt);

              const auto duration = (url_info.last_timestamp - url_info.first_timestamp) / 1000'000.0;
              url_info.freq = duration > 0 ? url_info.count / duration : 0;
            }
          }

          if (total_url_info.count == 0) {
            total_url_info.first_timestamp = -1;
            total_url_info.last_timestamp = -1;
            total_url_info.freq = 0;
          } else if (total_url_info.count == url_info.count) {
            total_url_info.first_timestamp = url_info.first_timestamp;
            total_url_info.last_timestamp = url_info.last_timestamp;
            total_url_info.freq = url_info.freq;
          } else if (total_url_info.first_timestamp >= 0) {
            if (url_info.count > 0) {
              total_url_info.last_timestamp = url_info.last_timestamp;
            } else {
              total_url_info.last_timestamp = total_url_info.previous_segment_last_timestamp;
            }

            const auto duration = (total_url_info.last_timestamp - total_url_info.first_timestamp) / 1000'000.0;
            total_url_info.freq = duration > 0 ? total_url_info.count / duration : 0;
          }
        } else {
          // LCOV_EXCL_START GCOVR_EXCL_START
          CLOG_W("Failed to erase datas table: %s.", ::sqlite3_errmsg(impl_->db));
          if VLIKELY (delete_row_stmt) {
            ::sqlite3_finalize(delete_row_stmt);
            delete_row_stmt = nullptr;
          }
          if VLIKELY (select_stmt) {
            ::sqlite3_finalize(select_stmt);
            select_stmt = nullptr;
          }
          rollback_cache();
          return false;
          // LCOV_EXCL_STOP GCOVR_EXCL_STOP
        }
      } else {
        if (ret == SQLITE_DONE) {
          CLOG_W("VDBWriter: No data available while enforcing the configured limit.");
        } else {
          CLOG_W("VDBWriter: Failed to select data while enforcing the configured limit: %s.",
                 ::sqlite3_errmsg(impl_->db));
        }
        ::sqlite3_finalize(select_stmt);
        rollback_cache();
        return false;
      }

      if VLIKELY (delete_row_stmt) {
        ::sqlite3_finalize(delete_row_stmt);
        delete_row_stmt = nullptr;
      }

      if VLIKELY (select_stmt) {
        ::sqlite3_finalize(select_stmt);
        select_stmt = nullptr;
      }
    } else {
      CLOG_W("Failed to erase datas table: %s.", ::sqlite3_errmsg(impl_->db));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      rollback_cache();                                                         // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      return false;                                                             // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
  }

  // split

  if (impl_->is_split_mode.load(std::memory_order_acquire) && !impl_->url_map.empty()) {
    if (impl_->config.split_by_time > 0 &&
        (microseconds_timestamp - impl_->config.begin_time * 1000) >
            impl_->config.split_by_time * 1000 * static_cast<int64_t>(impl_->split_file_list.size())) {
      do_split = true;
    } else if (impl_->config.split_by_time <= 0 && impl_->config.split_by_size > 0 &&
               (impl_->current_size + static_cast<int64_t>(data.size())) > impl_->config.split_by_size) {
      do_split = true;
    } else {
      do_split = false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if VUNLIKELY (do_split) {
      std::lock_guard split_lock(impl_->split_mtx);

      impl_->split_index.fetch_add(1, std::memory_order_relaxed);
      impl_->time_current = impl_->time_start + std::chrono::milliseconds(microseconds_timestamp / 1000U);

      if (impl_->config.split_name_by_time) {
        if (impl_->base_dir.empty()) {
          impl_->split_filename = get_format_date(&impl_->time_current, true) + ".vdb";
        } else {
          impl_->split_filename = impl_->base_dir + "/" + get_format_date(&impl_->time_current, true) + ".vdb";
        }
      } else {
        impl_->split_filename =
            impl_->base_name + "." + std::to_string(impl_->split_index.load(std::memory_order_relaxed) + 1) + ".vdb";
      }

      if (impl_->split_before && impl_->split_callback) {
        impl_->split_callback(impl_->split_index.load(std::memory_order_relaxed), impl_->split_filename);
      }

      if VUNLIKELY (!sync_cache()) {
        impl_->split_index.fetch_sub(1, std::memory_order_relaxed);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        return false;                                                // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      close_segment();

      if VUNLIKELY (!write_filex(false)) {
        set_fail();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      open_split(impl_->split_filename);

      if VUNLIKELY (!begin_cache()) {
        impl_->split_index.fetch_sub(1, std::memory_order_relaxed);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        return false;                                                // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      if (!impl_->split_before && impl_->split_callback) {
        impl_->split_callback(impl_->split_index.load(std::memory_order_relaxed), impl_->split_filename);
      }
    }
  }

  auto total_url_iter_ret = impl_->total_url_map.try_emplace(url, Impl::UrlMsgInfo());
  auto url_iter_ret = impl_->url_map.try_emplace(url, Impl::UrlMsgInfo());

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
        next_ser_type = ser_type;
      } else if VUNLIKELY (next_ser_type != ser_type) {
        CLOG_E("VDBWriter: URL [%s] ser changed from [%s] to [%s].", url.c_str(), next_ser_type.c_str(),
               ser_type.c_str());
        discard_new_url_entries();
        return false;
      }
    }
  }

  if (!next_ser_type.empty()) {
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
    SchemaData schema_data;

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
          // LCOV_EXCL_START GCOVR_EXCL_START
          CLOG_E("VDBWriter: URL [%s] schema changed from [%d] to [%d].", url.c_str(),
                 static_cast<int>(next_schema_type), static_cast<int>(resolved_schema_type));
          discard_new_url_entries();
          return false;
          // LCOV_EXCL_STOP GCOVR_EXCL_STOP
        }
      }
    }

    std::string schema_key = schema_ser_type;
    schema_key.push_back('\x1F');
    schema_key.append(SchemaData::convert_type(schema_storage_type));

    if (impl_->ser_map.find(schema_key) == impl_->ser_map.end()) {
      const bool has_schema_blob =
          !schema_data.name.empty() && !schema_data.encoding.empty() && !schema_data.data.empty();

      if (has_schema_blob) {
        if VUNLIKELY (!insert_schema(schema_data)) {
          rollback_cache();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          return false;      // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }
      }

      impl_->ser_map.emplace(schema_key);
    }
  }

  if (total_url_iter_ret.second) {
    total_url_msg_info.index = impl_->total_url_map.size() - 1;
    impl_->total_url_list.emplace_back(url);
  }

  if (url_iter_ret.second) {
    // insert url
    url_msg_info.index = impl_->url_map.size() - 1;

    if (!total_url_iter_ret.second) {
      total_url_msg_info.previous_segment_last_timestamp = total_url_msg_info.last_timestamp;
    }

    if (action_type == ActionType::kClientRequest || action_type == ActionType::kClientResponse ||
        action_type == ActionType::kServerRequest ||
        action_type == ActionType::kServerResponse) {  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      impl_->write_url_type = "Method";
    } else if (action_type == ActionType::kPublish || action_type == ActionType::kSubscribe) {
      impl_->write_url_type = "Event";
    } else if (action_type == ActionType::kSet || action_type == ActionType::kGet) {
      impl_->write_url_type = "Field";
    } else {
      impl_->write_url_type = "Unknown";
    }

    ::sqlite3_bind_int(impl_->urls_stmt, get_column(0), total_url_msg_info.index);
    ::sqlite3_bind_text(impl_->urls_stmt, get_column(1), url.c_str(), url.size(), SQLITE_STATIC);
    ::sqlite3_bind_text(impl_->urls_stmt, get_column(2), impl_->write_url_type.c_str(), impl_->write_url_type.size(),
                        SQLITE_STATIC);
    ::sqlite3_bind_text(impl_->urls_stmt, get_column(3), next_ser_type.c_str(), next_ser_type.size(), SQLITE_STATIC);
    const std::string next_encoding(SchemaData::convert_type(next_schema_type));
    ::sqlite3_bind_text(impl_->urls_stmt, get_column(4), next_encoding.c_str(), next_encoding.size(), SQLITE_STATIC);
    ::sqlite3_bind_int64(impl_->urls_stmt, get_column(5), 0);
    ::sqlite3_bind_double(impl_->urls_stmt, get_column(6), 0);
    ::sqlite3_bind_int64(impl_->urls_stmt, get_column(7), 0);
    ::sqlite3_bind_double(impl_->urls_stmt, get_column(8), 0);

    ret = ::sqlite3_step(impl_->urls_stmt);

    if VUNLIKELY (ret != SQLITE_DONE) {
      CLOG_W("Failed to insert urls table: %s.", ::sqlite3_errmsg(impl_->db));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      rollback_cache();                                                         // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      return false;                                                             // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    ret = ::sqlite3_reset(impl_->urls_stmt);

    if VUNLIKELY (ret != SQLITE_OK) {
      CLOG_W("Failed to reset urls table: %s.", ::sqlite3_errmsg(impl_->db));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      rollback_cache();                                                        // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      return false;                                                            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    url_msg_info.url_type = impl_->write_url_type;
    total_url_msg_info.url_type = impl_->write_url_type;

    if (!next_ser_type.empty()) {
      url_msg_info.ser_type = next_ser_type;
      total_url_msg_info.ser_type = next_ser_type;
    }

    url_msg_info.schema_type = next_schema_type;
    total_url_msg_info.schema_type = next_schema_type;
  } else if (total_url_msg_info.ser_type != next_ser_type || total_url_msg_info.schema_type != next_schema_type) {
    const bool ser_changed = total_url_msg_info.ser_type != next_ser_type;
    const bool schema_changed = total_url_msg_info.schema_type != next_schema_type;

    if (ser_changed || schema_changed) {
      ::sqlite3_bind_text(impl_->update_url_meta_stmt, get_column(0), next_ser_type.c_str(), next_ser_type.size(),
                          SQLITE_STATIC);
      const std::string next_encoding(SchemaData::convert_type(next_schema_type));
      ::sqlite3_bind_text(impl_->update_url_meta_stmt, get_column(1), next_encoding.c_str(), next_encoding.size(),
                          SQLITE_STATIC);
      ::sqlite3_bind_text(impl_->update_url_meta_stmt, get_column(2), url.c_str(), url.size(), SQLITE_STATIC);

      ret = ::sqlite3_step(impl_->update_url_meta_stmt);

      if VUNLIKELY (ret != SQLITE_DONE) {
        // LCOV_EXCL_START GCOVR_EXCL_START
        CLOG_W("Failed to update urls metadata table: %s.", ::sqlite3_errmsg(impl_->db));
        rollback_cache();
        return false;
        // LCOV_EXCL_STOP GCOVR_EXCL_STOP
      }

      ret = ::sqlite3_reset(impl_->update_url_meta_stmt);

      if VUNLIKELY (ret != SQLITE_OK) {
        // LCOV_EXCL_START GCOVR_EXCL_START
        CLOG_W("Failed to reset urls metadata table: %s.", ::sqlite3_errmsg(impl_->db));
        rollback_cache();
        return false;
        // LCOV_EXCL_STOP GCOVR_EXCL_STOP
      }

      total_url_msg_info.ser_type = next_ser_type;
      url_msg_info.ser_type = next_ser_type;
      total_url_msg_info.schema_type = next_schema_type;
      url_msg_info.schema_type = next_schema_type;
    }
  }

  // update info
  ++url_msg_info.count;
  ++total_url_msg_info.count;

  impl_->cached_size.fetch_add(data.size(), std::memory_order_relaxed);

  ++impl_->current_row;
  ++impl_->total_current_row;
  impl_->total_timestamp = microseconds_timestamp;

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
  const auto write_action_type = convert_action(action_type);

  if VUNLIKELY (impl_->last_timestamp > microseconds_timestamp) {
    if (impl_->last_timestamp - microseconds_timestamp < 1000'00U) {
      microseconds_timestamp = impl_->last_timestamp + 1;
    }
  }

  impl_->last_timestamp = microseconds_timestamp;

  ::sqlite3_bind_int64(impl_->datas_stmt, get_column(0), microseconds_timestamp);
  ::sqlite3_bind_int(impl_->datas_stmt, get_column(1), total_url_msg_info.index);
  ::sqlite3_bind_text(impl_->datas_stmt, get_column(2), write_action_type.data(), write_action_type.size(),
                      SQLITE_STATIC);

  // check compress
  do_compress = false;

  if (impl_->enable_compressed && static_cast<int64_t>(data.size()) >= impl_->config.compress_start_size &&
      impl_->config.ignore_compress_urls.count(url) == 0) {
    auto& compress_ignore_cnt = impl_->compress_ignore_map[url];

    if (compress_ignore_cnt < kCompressMaxIgnoreCnt) {
      compressed_data = Bytes::compress_data(data.data(), data.size(), impl_->config.compress_level > 3);

      if (!compressed_data.empty() && compressed_data.size() < data.size() * kCompressMaxRatio) {
        do_compress = true;
        compress_ignore_cnt = 0;
      } else {
        ++compress_ignore_cnt;
      }
    }
  }

  if (do_compress) {
    ::sqlite3_bind_blob(impl_->datas_stmt, get_column(3), compressed_data.data(), compressed_data.size(),
                        SQLITE_STATIC);
  } else {
    ::sqlite3_bind_blob(impl_->datas_stmt, get_column(3), data.data(), data.size(), SQLITE_STATIC);
  }

  auto accounted_size = data.size();

  if VUNLIKELY (!do_compress && impl_->enable_compressed && Bytes::is_compress_data(data.data(), data.size())) {
    accounted_size = (static_cast<size_t>(data[4]) << 24U) | (static_cast<size_t>(data[5]) << 16U) |
                     (static_cast<size_t>(data[6]) << 8U) | static_cast<size_t>(data[7]);
  }

  url_msg_info.size += accounted_size;
  total_url_msg_info.size += accounted_size;
  impl_->current_size += static_cast<int64_t>(accounted_size);
  impl_->total_current_size += static_cast<int64_t>(accounted_size);

  ret = ::sqlite3_step(impl_->datas_stmt);

  if VUNLIKELY (ret != SQLITE_DONE) {
    CLOG_W("Failed to insert datas table: %s.", ::sqlite3_errmsg(impl_->db));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    rollback_cache();                                                          // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;                                                              // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  ret = ::sqlite3_reset(impl_->datas_stmt);

  if VUNLIKELY (ret != SQLITE_OK) {
    CLOG_W("Failed to reset datas table: %s.", ::sqlite3_errmsg(impl_->db));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    rollback_cache();                                                         // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;                                                             // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  // update on wal_mode

  if (impl_->sync_timer.get() > kSyncWriteInterval) {
    impl_->sync_timer.restart();

    ::sqlite3_bind_int64(impl_->update_urls_stmt, get_column(0), url_msg_info.count);
    ::sqlite3_bind_int64(impl_->update_urls_stmt, get_column(1), url_msg_info.size);
    ::sqlite3_bind_double(impl_->update_urls_stmt, get_column(2), url_msg_info.freq);
    ::sqlite3_bind_text(impl_->update_urls_stmt, get_column(3), url.c_str(), url.size(), SQLITE_STATIC);
    ret = ::sqlite3_step(impl_->update_urls_stmt);

    if VUNLIKELY (ret != SQLITE_DONE) {
      CLOG_W("Failed to update urls table: %s.", ::sqlite3_errmsg(impl_->db));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      rollback_cache();                                                         // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      return false;                                                             // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    ret = ::sqlite3_reset(impl_->update_urls_stmt);

    if VUNLIKELY (ret != SQLITE_OK) {
      CLOG_W("Failed to reset urls table: %s.", ::sqlite3_errmsg(impl_->db));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      rollback_cache();                                                        // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      return false;                                                            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    ::sqlite3_bind_int64(impl_->update_header_stmt, get_column(0), impl_->current_row);
    ::sqlite3_bind_int64(impl_->update_header_stmt, get_column(1), impl_->last_timestamp);
    ret = ::sqlite3_step(impl_->update_header_stmt);

    if VUNLIKELY (ret != SQLITE_DONE) {
      CLOG_W("Failed to update header table: %s.", ::sqlite3_errmsg(impl_->db));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      rollback_cache();                                                           // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      return false;                                                               // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    ret = ::sqlite3_reset(impl_->update_header_stmt);

    if VUNLIKELY (ret != SQLITE_OK) {
      CLOG_W("Failed to reset header table: %s.", ::sqlite3_errmsg(impl_->db));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      rollback_cache();                                                          // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      return false;                                                              // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
  }

  if VUNLIKELY (impl_->cached_size.load(std::memory_order_relaxed) > impl_->config.cache_size) {
    if VUNLIKELY (!sync_cache()) {
      return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
  }

  return true;
#else
  (void)url;
  (void)ser_type;
  (void)schema_type;
  (void)action_type;
  (void)data;
  (void)microseconds_timestamp;
  return false;
#endif
}

bool VDBWriter::write_filex(bool complete) {
#ifdef _WIN32
  std::filesystem::path file_path(Helpers::string_to_wstring(impl_->path));
#else
  std::filesystem::path file_path(impl_->path);
#endif

  try {
    nlohmann::ordered_json json;

    json["VLinkHeader"] = {
        {"major", VLINK_VERSION_MAJOR},  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        {"minor", VLINK_VERSION_MINOR},  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        {"patch", VLINK_VERSION_PATCH},  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        {"count", impl_->total_current_row},
        {"duration", impl_->total_timestamp},
        {"accuracy", "MicroSecond"},
        {"compress", impl_->enable_compressed ? "lzav" : "None"},
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

      for (const auto& url : impl_->total_url_list) {
        const auto& ext_info = impl_->total_url_map[url];
        auto loss = total_url_loss_map_ref()[recover_recorded_url(url)];

        url_json.push_back({
            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
            {"index", ext_info.index},
            {"url", url},
            {"type", ext_info.url_type},
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
    VLOG_F("VDBWriter: JSON error during config export, ", e.what(), ".");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;                                                           // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

  return true;
}

bool VDBWriter::begin_cache() {
#ifdef VLINK_ENABLE_SQLITE

  if VUNLIKELY (!impl_->db) {
    VLOG_W("VDBWriter: Sqlite not open.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;                           // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if VLIKELY (impl_->in_cached.load(std::memory_order_relaxed)) {
    return true;
  }

  int ret = ::sqlite3_exec(impl_->db, "BEGIN;", nullptr, nullptr, nullptr);

  if VUNLIKELY (ret != SQLITE_OK) {
    CLOG_W("Failed to begin transaction: %s.", ::sqlite3_errmsg(impl_->db));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;                                                             // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  impl_->cache_snapshot = std::make_unique<Impl::WriteStateSnapshot>(*impl_);
  impl_->in_cached.store(true, std::memory_order_relaxed);
  impl_->cached_size.store(0, std::memory_order_relaxed);

  impl_->cache_timer.restart();

  return true;
#else
  return false;
#endif
}

bool VDBWriter::sync_cache() {
#ifdef VLINK_ENABLE_SQLITE

  if VUNLIKELY (!impl_->db) {
    VLOG_W("VDBWriter: Sqlite not open.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;                           // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if (!impl_->in_cached.load(std::memory_order_relaxed)) {
    return true;
  }

  int ret = ::sqlite3_exec(impl_->db, "COMMIT;", nullptr, nullptr, nullptr);

  if VUNLIKELY (ret != SQLITE_OK) {
    CLOG_W("Failed to commit transaction: %s.", ::sqlite3_errmsg(impl_->db));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;                                                              // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  impl_->in_cached.store(false, std::memory_order_relaxed);
  impl_->cached_size.store(0, std::memory_order_relaxed);
  impl_->cache_snapshot.reset();

  impl_->cache_timer.stop();

  return true;
#else
  return false;
#endif
}

bool VDBWriter::rollback_cache() {
#ifdef VLINK_ENABLE_SQLITE

  if VUNLIKELY (!impl_->db) {
    VLOG_W("VDBWriter: Sqlite not open.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;                           // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  ::sqlite3_exec(impl_->db, "ROLLBACK;", nullptr, nullptr, nullptr);

  if (impl_->cache_snapshot) {
    impl_->cache_snapshot->restore(*impl_);
    impl_->cache_snapshot.reset();
  }

  impl_->in_cached.store(false, std::memory_order_relaxed);
  impl_->cached_size.store(0, std::memory_order_relaxed);

  impl_->cache_timer.stop();

  return true;
#else
  return false;
#endif
}

bool VDBWriter::insert_schema(const SchemaData& schema_data) {
#ifdef VLINK_ENABLE_SQLITE

  if VUNLIKELY (!impl_->db) {
    VLOG_W("VDBWriter: Sqlite not open.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;                           // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  std::string encoding = schema_data.encoding;
  SchemaType schema_type = SchemaData::resolve_type(schema_data.schema_type, schema_data.name, encoding);

  if (encoding.empty() && SchemaData::is_real_type(schema_type)) {
    encoding = std::string(SchemaData::convert_type(schema_type));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if (schema_data.name.empty() || encoding.empty() || schema_data.data.empty()) {
    return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  int ret = 0;

  ::sqlite3_bind_text(impl_->schemas_stmt, get_column(0), schema_data.name.c_str(), schema_data.name.size(),
                      SQLITE_STATIC);

  ::sqlite3_bind_text(impl_->schemas_stmt, get_column(1), encoding.c_str(), encoding.size(), SQLITE_STATIC);

  ::sqlite3_bind_blob(impl_->schemas_stmt, get_column(2), schema_data.data.data(), schema_data.data.size(),
                      SQLITE_STATIC);

  ret = ::sqlite3_step(impl_->schemas_stmt);

  if VUNLIKELY (ret != SQLITE_DONE) {
    CLOG_W("Failed to insert schema table: %s.", ::sqlite3_errmsg(impl_->db));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    rollback_cache();                                                           // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;                                                               // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  ret = ::sqlite3_reset(impl_->schemas_stmt);

  if VUNLIKELY (ret != SQLITE_OK) {
    CLOG_W("Failed to reset schema table: %s.", ::sqlite3_errmsg(impl_->db));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    rollback_cache();                                                          // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;                                                              // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  return true;
#else
  (void)schema_data;
  return false;
#endif
}

}  // namespace vlink
