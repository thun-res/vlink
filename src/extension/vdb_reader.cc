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

#include "./extension/vdb_reader.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "./base/condition_variable.h"
#include "./base/elapsed_timer.h"
#include "./base/helpers.h"
#include "./base/logger.h"
#include "./version.h"

// json
#include <nlohmann/json.hpp>

#ifdef VLINK_ENABLE_SQLITE
#include <sqlite3.h>
#endif

#define ENABLE_DATABASE_TABLE_CHECK 0

namespace vlink {

[[maybe_unused]] static constexpr int get_column(int column) noexcept { return column; }

[[maybe_unused]] static constexpr size_t kMaxTaskSize = 50000U;

#ifdef VLINK_ENABLE_SQLITE
struct SqliteStmtFinalizer final {
  void operator()(::sqlite3_stmt* stmt) const noexcept {
    if (stmt) {
      ::sqlite3_finalize(stmt);
    }
  }
};

using SqliteStmtPtr = std::unique_ptr<::sqlite3_stmt, SqliteStmtFinalizer>;

static std::string sqlite_column_text_or_empty(::sqlite3_stmt* stmt, int column) {
  const auto* text = ::sqlite3_column_text(stmt, column);

  if (!text) {
    return {};  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  return {reinterpret_cast<const char*>(text), static_cast<size_t>(::sqlite3_column_bytes(stmt, column))};
}
#endif

// VDBReader::Impl
struct VDBReader::Impl final {  // NOLINT(clang-analyzer-optin.performance.Padding)
  std::atomic<BagReader::Status> status{BagReader::kStopped};
  std::atomic_bool stop_flag{false};
  std::atomic_bool pause_flag{false};
  std::atomic_bool pause_next_flag{false};
  std::atomic_bool jump_flag{false};
  std::atomic<int64_t> pause_elapsed{0};
  std::atomic<int64_t> offset_elapsed{0};
  std::atomic<int64_t> real_elapsed{0};
  std::atomic<int64_t> extra_elapsed{0};
  std::atomic<int64_t> begin_time{0};
  std::atomic<double> rate{1.0};
  std::atomic<int> times{1};
  std::atomic_bool is_pending{false};
  std::atomic<int> split_index{0};

  bool read_only{false};
  bool try_to_fix{false};
  bool enable_compress{false};

  std::string path;
  BagReader::Info info;
  std::vector<BagReader::Info::UrlMeta> raw_url_metas;
  std::mutex mtx;
  ConditionVariable cv;

  BagReader::Config config;
  std::mutex config_mtx;
  std::shared_mutex time_mtx;

  ElapsedTimer elapsed_timer{ElapsedTimer::kMicro};
  ElapsedTimer pause_elapsed_timer{ElapsedTimer::kMicro};
  ElapsedTimer offset_timer{ElapsedTimer::kMicro};
  ElapsedTimer real_timer{ElapsedTimer::kMicro};

  BagReader::StatusCallback status_callback;
  BagReader::ReadyCallback ready_callback;
  BagReader::FinishCallback finish_callback;

  std::string update_sql_default_str;
  std::string update_sql_time_str;

  int64_t total_start_timestamp_ns{-1};
  bool total_has_completed{false};

// database
#ifdef VLINK_ENABLE_SQLITE

  // WrapperFile
  struct WrapperFile final {
    std::string path;
    ::sqlite3* db{nullptr};
    ::sqlite3_stmt* stmt{nullptr};
    int index{0};
    int64_t start_timestamp_ns{0};
    int64_t begin{0};
    int64_t end{0};
    std::unordered_map<std::string, int> url_to_id_map;
    std::unordered_map<int, std::string> id_to_url_map;
    bool has_idx_elapsed{false};
    bool has_idx_url{false};
    bool has_schema{false};
    bool has_completed{false};
    bool is_channel_broken{false};

    WrapperFile() {
      url_to_id_map.reserve(128);
      id_to_url_map.reserve(128);
    }
  };

  std::vector<WrapperFile> file_list;

  SqliteStmtPtr cursor_stmt;
  int cursor_file_index{0};
  int64_t cursor_begin_us{0};
  int64_t cursor_end_us{0};
  BagReader::Config cursor_config;
#endif
};

// VDBReader
VDBReader::VDBReader(const std::string& path, bool read_only, bool try_to_fix)
    : BagReader(path, read_only, try_to_fix), impl_(std::make_unique<Impl>()) {
  set_name("VDBReader");

  url_ser_map().reserve(128);
  url_schema_type_map().reserve(128);
  impl_->update_sql_default_str.reserve(256);
  impl_->update_sql_time_str.reserve(256);

  impl_->read_only = read_only;
  impl_->try_to_fix = try_to_fix;

  open(path);

#ifndef VLINK_ENABLE_SQLITE
  VLOG_F("VDBReader: The compile macro VLINK_ENABLE_SQLITE is not turned on.");
#endif
}

VDBReader::~VDBReader() {
  if (!impl_->stop_flag.load(std::memory_order_relaxed)) {
    do_stop();
  }

  quit(true);

  impl_->cv.notify_one();

  wait_for_quit();

  detach_plugin();

  close();
}

void VDBReader::bind_bag_interface(const std::shared_ptr<BagPluginInterface>& bag_interface) {
  BagReader::bind_bag_interface(bag_interface);
  impl_->info.url_metas = impl_->raw_url_metas;
  process_url_metas(impl_->info.url_metas);
  rebuild_url_meta_lookup(impl_->info.url_metas);
}

void VDBReader::register_status_callback(StatusCallback&& status_callback) {
  impl_->status_callback = std::move(status_callback);
}

void VDBReader::register_ready_callback(ReadyCallback&& ready_callback) {
  impl_->ready_callback = std::move(ready_callback);
}

void VDBReader::register_finish_callback(FinishCallback&& finish_callback) {
  impl_->finish_callback = std::move(finish_callback);
}

void VDBReader::register_output_callback(OutputCallback&& output_callback) {
  BagReader::register_output_callback(std::move(output_callback));
}

void VDBReader::play(const Config& config) {
#ifdef VLINK_ENABLE_SQLITE

  if VUNLIKELY (is_busy()) {
    VLOG_W("VDBReader: Is busy.");
  }

  if (config.skip_blank) {
    impl_->begin_time.store(std::max(config.begin_time, impl_->info.blank_duration), std::memory_order_relaxed);
  } else {
    impl_->begin_time.store(config.begin_time, std::memory_order_relaxed);
  }

  if (config.rate <= 0) {
    impl_->rate.store(1, std::memory_order_relaxed);
  } else {
    impl_->rate.store(config.rate, std::memory_order_relaxed);
  }

  impl_->times.store(config.times, std::memory_order_relaxed);

  impl_->real_elapsed.store(impl_->begin_time.load(std::memory_order_relaxed) * 1000U, std::memory_order_relaxed);
  impl_->is_pending.store(true, std::memory_order_relaxed);

  {
    std::unique_lock lock(impl_->mtx);
    impl_->stop_flag.store(false, std::memory_order_relaxed);
    impl_->pause_flag.store(false, std::memory_order_relaxed);
    impl_->pause_next_flag.store(false, std::memory_order_relaxed);
    impl_->jump_flag.store(false, std::memory_order_relaxed);
  }

  Config config_copy;

  {
    std::unique_lock lock(impl_->config_mtx);
    impl_->config = config;
    config_copy = impl_->config;
  }

  post_task([this, config_copy = std::move(config_copy)]() { read(config_copy); });
#else
  (void)config;
#endif
}

void VDBReader::stop() { do_stop(); }

void VDBReader::pause() {
#ifdef VLINK_ENABLE_SQLITE
  {
    std::unique_lock lock(impl_->mtx);
    impl_->pause_flag.store(true, std::memory_order_relaxed);
  }

  impl_->cv.notify_one();
#endif
}

void VDBReader::resume() {
#ifdef VLINK_ENABLE_SQLITE
  {
    std::unique_lock lock(impl_->mtx);
    impl_->pause_flag.store(false, std::memory_order_relaxed);
  }

  impl_->cv.notify_one();
#endif
}

void VDBReader::pause_to_next() {
#ifdef VLINK_ENABLE_SQLITE
  {
    std::unique_lock lock(impl_->mtx);

    if (!impl_->pause_flag.load(std::memory_order_relaxed)) {
      return;
    }

    impl_->pause_next_flag.store(true, std::memory_order_relaxed);
  }

  impl_->cv.notify_one();
#endif
}

void VDBReader::jump(int64_t begin_time, double rate, int times, bool force_to_play) {
#ifdef VLINK_ENABLE_SQLITE

  if (begin_time < 0) {
    begin_time = 0;
  } else if (begin_time > impl_->info.total_duration) {
    begin_time = std::max<int64_t>(0, impl_->info.total_duration - 100);
  }

  impl_->real_elapsed.store(begin_time * 1000U, std::memory_order_relaxed);
  impl_->is_pending.store(true, std::memory_order_relaxed);

  bool last_pause_flag = impl_->pause_flag.load(std::memory_order_relaxed);

  {
    std::unique_lock lock(impl_->mtx);
    impl_->stop_flag.store(false, std::memory_order_relaxed);
    impl_->pause_flag.store(false, std::memory_order_relaxed);
    impl_->pause_next_flag.store(false, std::memory_order_relaxed);
    impl_->jump_flag.store(true, std::memory_order_relaxed);
  }

  impl_->cv.notify_one();

  for (const auto& wrapper_file : impl_->file_list) {
    if (wrapper_file.db) {
      ::sqlite3_interrupt(wrapper_file.db);
    }
  }

  wait_for_idle();

  impl_->begin_time.store(begin_time, std::memory_order_relaxed);

  if (rate <= 0) {
    impl_->rate.store(1, std::memory_order_relaxed);
  } else {
    impl_->rate.store(rate, std::memory_order_relaxed);
  }

  impl_->times.store(times, std::memory_order_relaxed);

  {
    std::unique_lock lock(impl_->mtx);
    impl_->stop_flag.store(false, std::memory_order_relaxed);
    impl_->pause_flag.store(force_to_play ? false : last_pause_flag, std::memory_order_relaxed);
    impl_->pause_next_flag.store(false, std::memory_order_relaxed);
    impl_->jump_flag.store(false, std::memory_order_relaxed);
  }

  Config config_copy;

  {
    std::unique_lock lock(impl_->config_mtx);
    config_copy = impl_->config;
  }

  post_task([this, config_copy = std::move(config_copy)]() { read(config_copy); });
#else
  (void)begin_time;
  (void)rate;
  (void)times;
  (void)force_to_play;
#endif
}

std::future<bool> VDBReader::check() {
#ifdef VLINK_ENABLE_SQLITE

  if VUNLIKELY (is_busy()) {
    VLOG_W("VDBReader: Is busy.");
  }

  return invoke_task([this]() {
    int ret = 0;

    if (!impl_->total_has_completed) {
      VLOG_W("VDBReader: Incomplete data detected.");
      return false;
    }

    for (auto& wrapper_file : impl_->file_list) {
      if VUNLIKELY (!wrapper_file.db) {
        continue;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      if (wrapper_file.stmt) {
        ::sqlite3_finalize(wrapper_file.stmt);
        wrapper_file.stmt = nullptr;
      }

      ::sqlite3_stmt* integrity_stmt = nullptr;
      ret = ::sqlite3_prepare_v2(wrapper_file.db, "PRAGMA integrity_check;", -1, &integrity_stmt, nullptr);
      SqliteStmtPtr integrity_stmt_guard(integrity_stmt);

      if VUNLIKELY (is_ready_to_quit()) {
        return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      } else if VUNLIKELY (ret != SQLITE_OK) {
        CLOG_W("Failed to prepare integrity check: %s.",  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
               ::sqlite3_errmsg(wrapper_file.db));        // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        return false;                                     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      bool has_integrity_result = false;
      int step_ret = SQLITE_OK;
      for (;;) {
        step_ret = ::sqlite3_step(integrity_stmt);

        if (step_ret != SQLITE_ROW) {
          break;
        }

        has_integrity_result = true;
        const auto integrity_result = sqlite_column_text_or_empty(integrity_stmt, get_column(0));

        if VUNLIKELY (integrity_result != "ok") {
          CLOG_W("Failed integrity check: %s.", integrity_result.c_str());  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          return false;                                                     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }
      }

      if VUNLIKELY (step_ret != SQLITE_DONE || !has_integrity_result) {
        CLOG_W("Failed to read integrity check: %s.",  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
               ::sqlite3_errmsg(wrapper_file.db));     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        return false;                                  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      integrity_stmt_guard.reset();

      ret = ::sqlite3_prepare_v2(wrapper_file.db, "SELECT * FROM VLinkDatas;", -1, &wrapper_file.stmt, nullptr);

      if VUNLIKELY (is_ready_to_quit()) {
        return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      } else if VUNLIKELY (ret != SQLITE_OK) {
        CLOG_W("Failed to prepare datas table: %s.",  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
               ::sqlite3_errmsg(wrapper_file.db));    // LCOV_EXCL_LINE GCOVR_EXCL_LINE

        return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      ::sqlite3_step(wrapper_file.stmt);
    }

    bool is_ok = true;

    if VUNLIKELY (impl_->info.total_duration < impl_->info.blank_duration) {
      VLOG_W("VDBReader: Invalid duration, blank=", impl_->info.blank_duration, " total=", impl_->info.total_duration,
             ".");
      is_ok = false;
    }

    if VUNLIKELY (impl_->info.message_count > 0 && impl_->info.url_metas.empty()) {
      VLOG_W("VDBReader: Message count is ", impl_->info.message_count, " but url meta list is empty.");
      is_ok = false;
    }

    size_t total_count = 0;
    size_t total_raw_size = 0;

    for (const auto& url_meta : impl_->info.url_metas) {
      total_count += url_meta.count;
      total_raw_size += url_meta.size;

      if VUNLIKELY (!url_meta.valid) {
        CLOG_W("VDBReader: Invalid url meta detected at index=%d.", url_meta.index);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        is_ok = false;                                                                // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      if VUNLIKELY (url_meta.url.empty()) {
        CLOG_W("VDBReader: Empty url detected at index=%d.", url_meta.index);
        is_ok = false;
      }

      if VUNLIKELY (url_meta.url_type.empty()) {
        CLOG_W("VDBReader: Empty url_type detected for url=%s.", url_meta.url.c_str());
        is_ok = false;
      }

      if VUNLIKELY (url_meta.count > 0 && url_meta.ser_type.empty()) {
        CLOG_W("VDBReader: Empty ser_type detected for url=%s.", url_meta.url.c_str());
        is_ok = false;
      }

      if VUNLIKELY (!SchemaData::is_valid_type(url_meta.schema_type)) {
        CLOG_W("VDBReader: Invalid schema_type=%d detected for url=%s.",  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
               static_cast<int>(url_meta.schema_type),                    // LCOV_EXCL_LINE GCOVR_EXCL_LINE
               url_meta.url.c_str());
        is_ok = false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      auto inferred_schema_type = SchemaData::infer_ser_type(url_meta.ser_type);

      if VUNLIKELY (url_meta.schema_type == SchemaType::kUnknown && inferred_schema_type != SchemaType::kUnknown) {
        const auto schema_label = SchemaData::convert_type(inferred_schema_type);
        CLOG_W("VDBReader: Missing schema_type for url=%s, inferred=%.*s.", url_meta.url.c_str(),
               static_cast<int>(schema_label.size()), schema_label.data());
        is_ok = false;
      }

      if VUNLIKELY (!std::isfinite(url_meta.loss) ||
                    (url_meta.loss != -1.0 && (url_meta.loss < 0.0 || url_meta.loss > 1.0))) {
        CLOG_W("VDBReader: Invalid loss=%f detected for url=%s.", url_meta.loss, url_meta.url.c_str());
        is_ok = false;
      }

      if VUNLIKELY (url_meta.freq < 0.0) {
        CLOG_W("VDBReader: Invalid freq=%f detected for url=%s.", url_meta.freq, url_meta.url.c_str());
        is_ok = false;
      }
    }

    if ((!impl_->info.url_metas.empty() || impl_->info.message_count != 0) &&
        total_count != static_cast<size_t>(impl_->info.message_count)) {
      VLOG_W("VDBReader: Message count mismatch, header=", impl_->info.message_count, " metas=", total_count, ".");
      is_ok = false;
    }

    if ((!impl_->info.url_metas.empty() || impl_->info.total_raw_size != 0) &&
        total_raw_size != static_cast<size_t>(impl_->info.total_raw_size)) {
      VLOG_W("VDBReader: Raw size mismatch, header=", impl_->info.total_raw_size,
             " metas=", total_raw_size,  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
             ".");                       // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      is_ok = false;                     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    for (const auto& schema_data : detect_schema()) {
      if VUNLIKELY (schema_data.name.empty()) {
        CLOG_W("VDBReader: Empty schema name detected.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        is_ok = false;                                     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      if VUNLIKELY (schema_data.encoding.empty()) {
        CLOG_W("VDBReader: Empty schema encoding detected for name=%s.", schema_data.name.c_str());
        is_ok = false;
      }

      if VUNLIKELY (!SchemaData::is_valid_type(schema_data.schema_type) ||
                    schema_data.schema_type == SchemaType::kUnknown) {
        CLOG_W("VDBReader: Invalid schema_type=%d detected for schema=%s.", static_cast<int>(schema_data.schema_type),
               schema_data.name.c_str());
        is_ok = false;
      }
    }

    return is_ok;
  });
#else
  return std::future<bool>();
#endif
}

std::future<bool> VDBReader::reindex() {
#ifdef VLINK_ENABLE_SQLITE

  if VUNLIKELY (is_busy()) {
    VLOG_W("VDBReader: Is busy.");
  }

  return invoke_task([this]() {
    int ret = 0;
    char* err_msg = nullptr;

    for (auto& wrapper_file : impl_->file_list) {
      if VUNLIKELY (!wrapper_file.db) {
        continue;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      if (wrapper_file.stmt) {
        ::sqlite3_finalize(wrapper_file.stmt);
        wrapper_file.stmt = nullptr;
      }

      ret = ::sqlite3_exec(wrapper_file.db, "DROP INDEX IF EXISTS idx_elapsed;", nullptr, nullptr, &err_msg);

      if VUNLIKELY (!is_ready_to_quit() && ret != SQLITE_OK) {
        // LCOV_EXCL_START GCOVR_EXCL_START
        CLOG_W("Failed to drop idx_elapsed: %s.", err_msg);

        if (err_msg) {
          ::sqlite3_free(err_msg);
          err_msg = nullptr;
        }

        return false;
        // LCOV_EXCL_STOP GCOVR_EXCL_STOP
      }

      ret = ::sqlite3_exec(wrapper_file.db, "DROP INDEX IF EXISTS idx_url;", nullptr, nullptr, &err_msg);

      if VUNLIKELY (!is_ready_to_quit() && ret != SQLITE_OK) {
        // LCOV_EXCL_START GCOVR_EXCL_START
        CLOG_W("Failed to drop idx_url: %s.", err_msg);

        if (err_msg) {
          ::sqlite3_free(err_msg);
          err_msg = nullptr;
        }

        return false;
        // LCOV_EXCL_STOP GCOVR_EXCL_STOP
      }

      ret = ::sqlite3_exec(wrapper_file.db, "DROP INDEX IF EXISTS idx_elapsed_url;", nullptr, nullptr, &err_msg);

      if VUNLIKELY (!is_ready_to_quit() && ret != SQLITE_OK) {
        // LCOV_EXCL_START GCOVR_EXCL_START
        CLOG_W("Failed to drop idx_elapsed_url: %s.", err_msg);

        if (err_msg) {
          ::sqlite3_free(err_msg);
          err_msg = nullptr;
        }

        return false;
        // LCOV_EXCL_STOP GCOVR_EXCL_STOP
      }

      ret = ::sqlite3_exec(wrapper_file.db, "CREATE INDEX idx_elapsed_url ON VLinkDatas(elapsed, url);", nullptr,
                           nullptr, &err_msg);

      if VUNLIKELY (is_ready_to_quit()) {
        // LCOV_EXCL_START GCOVR_EXCL_START
        if (err_msg) {
          ::sqlite3_free(err_msg);
          err_msg = nullptr;
        }

        return false;
        // LCOV_EXCL_STOP GCOVR_EXCL_STOP
      } else if VUNLIKELY (ret != SQLITE_OK) {
        // LCOV_EXCL_START GCOVR_EXCL_START
        CLOG_W("Failed to create idx_elapsed_url: %s.", err_msg);

        if (err_msg) {
          ::sqlite3_free(err_msg);
          err_msg = nullptr;
        }

        return false;
        // LCOV_EXCL_STOP GCOVR_EXCL_STOP
      }

      ret = ::sqlite3_exec(wrapper_file.db, "PRAGMA optimize;", nullptr, nullptr, &err_msg);

      if VUNLIKELY (is_ready_to_quit()) {
        // LCOV_EXCL_START GCOVR_EXCL_START
        if (err_msg) {
          ::sqlite3_free(err_msg);
          err_msg = nullptr;
        }

        return false;
        // LCOV_EXCL_STOP GCOVR_EXCL_STOP
      } else if VUNLIKELY (ret != SQLITE_OK) {
        // LCOV_EXCL_START GCOVR_EXCL_START
        CLOG_W("Failed to optimize: %s.", err_msg);

        if (err_msg) {
          ::sqlite3_free(err_msg);
          err_msg = nullptr;
        }

        return false;
        // LCOV_EXCL_STOP GCOVR_EXCL_STOP
      }

      ret = ::sqlite3_prepare_v2(wrapper_file.db, "SELECT * FROM VLinkDatas;", -1, &wrapper_file.stmt, nullptr);

      if VUNLIKELY (is_ready_to_quit()) {
        // LCOV_EXCL_START GCOVR_EXCL_START
        if (err_msg) {
          ::sqlite3_free(err_msg);
          err_msg = nullptr;
        }

        return false;
        // LCOV_EXCL_STOP GCOVR_EXCL_STOP
      } else if VUNLIKELY (ret != SQLITE_OK) {
        // LCOV_EXCL_START GCOVR_EXCL_START
        CLOG_W("Failed to prepare datas table: %s.", ::sqlite3_errmsg(wrapper_file.db));

        if (err_msg) {
          ::sqlite3_free(err_msg);
          err_msg = nullptr;
        }

        return false;
        // LCOV_EXCL_STOP GCOVR_EXCL_STOP
      }

      ::sqlite3_step(wrapper_file.stmt);

      if (err_msg) {
        ::sqlite3_free(err_msg);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        err_msg = nullptr;        // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }
    }

    return true;
  });
#else
  return std::future<bool>();
#endif
}

std::future<bool> VDBReader::fix(bool rebuild) {
#ifdef VLINK_ENABLE_SQLITE

  if VUNLIKELY (is_busy()) {
    VLOG_W("VDBReader: Is busy.");
  }

  return invoke_task([this, rebuild]() {
    int ret = 0;
    char* err_msg = nullptr;

    for (auto& wrapper_file : impl_->file_list) {
      if VUNLIKELY (!wrapper_file.db) {
        continue;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      if (wrapper_file.stmt) {
        ::sqlite3_finalize(wrapper_file.stmt);
        wrapper_file.stmt = nullptr;
      }

      // opt
      {
        ret = ::sqlite3_exec(wrapper_file.db, "PRAGMA synchronous = OFF;", nullptr, nullptr, &err_msg);

        if VUNLIKELY (!is_ready_to_quit() && ret != SQLITE_OK) {
          // LCOV_EXCL_START GCOVR_EXCL_START
          CLOG_W("Failed to set synchronous: %s.", err_msg);

          if (err_msg) {
            ::sqlite3_free(err_msg);
            err_msg = nullptr;
          }

          return false;
          // LCOV_EXCL_STOP GCOVR_EXCL_STOP
        }

        ret = ::sqlite3_exec(wrapper_file.db, "PRAGMA temp_store = MEMORY;", nullptr, nullptr, &err_msg);

        if VUNLIKELY (!is_ready_to_quit() && ret != SQLITE_OK) {
          // LCOV_EXCL_START GCOVR_EXCL_START
          CLOG_W("Failed to set temp_store: %s.", err_msg);

          if (err_msg) {
            ::sqlite3_free(err_msg);
            err_msg = nullptr;
          }

          return false;
          // LCOV_EXCL_STOP GCOVR_EXCL_STOP
        }

        ret = ::sqlite3_exec(wrapper_file.db, "PRAGMA page_size = 16384;", nullptr, nullptr, &err_msg);

        if VUNLIKELY (!is_ready_to_quit() && ret != SQLITE_OK) {
          // LCOV_EXCL_START GCOVR_EXCL_START
          CLOG_W("Failed to set page_size: %s.", err_msg);

          if (err_msg) {
            ::sqlite3_free(err_msg);
            err_msg = nullptr;
          }

          return false;
          // LCOV_EXCL_STOP GCOVR_EXCL_STOP
        }

        ret = ::sqlite3_exec(wrapper_file.db, "PRAGMA cache_size = 8192;", nullptr, nullptr, &err_msg);

        if VUNLIKELY (!is_ready_to_quit() && ret != SQLITE_OK) {
          // LCOV_EXCL_START GCOVR_EXCL_START
          CLOG_W("Failed to set cache_size: %s.", err_msg);

          if (err_msg) {
            ::sqlite3_free(err_msg);
            err_msg = nullptr;
          }

          return false;
          // LCOV_EXCL_STOP GCOVR_EXCL_STOP
        }

        ret = ::sqlite3_exec(wrapper_file.db, "PRAGMA journal_mode = OFF;", nullptr, nullptr, &err_msg);

        if VUNLIKELY (!is_ready_to_quit() && ret != SQLITE_OK) {
          // LCOV_EXCL_START GCOVR_EXCL_START
          CLOG_W("Failed to restore journal mode: %s.", err_msg);

          if (err_msg) {
            ::sqlite3_free(err_msg);
            err_msg = nullptr;
          }

          return false;
          // LCOV_EXCL_STOP GCOVR_EXCL_STOP
        }

        ret = ::sqlite3_exec(wrapper_file.db, "PRAGMA automatic_index = OFF;", nullptr, nullptr, &err_msg);

        if VUNLIKELY (!is_ready_to_quit() && ret != SQLITE_OK) {
          // LCOV_EXCL_START GCOVR_EXCL_START
          CLOG_W("Failed to set automatic_index: %s.", err_msg);

          if (err_msg) {
            ::sqlite3_free(err_msg);
            err_msg = nullptr;
          }

          return false;
          // LCOV_EXCL_STOP GCOVR_EXCL_STOP
        }

        ret = ::sqlite3_exec(wrapper_file.db, "PRAGMA locking_mode = EXCLUSIVE;", nullptr, nullptr, &err_msg);

        if VUNLIKELY (is_ready_to_quit()) {
          // LCOV_EXCL_START GCOVR_EXCL_START
          if (err_msg) {
            ::sqlite3_free(err_msg);
            err_msg = nullptr;
          }

          return false;
          // LCOV_EXCL_STOP GCOVR_EXCL_STOP
        } else if VUNLIKELY (ret != SQLITE_OK) {
          // LCOV_EXCL_START GCOVR_EXCL_START
          CLOG_W("Failed to set locking_mode: %s.", err_msg);

          if (err_msg) {
            ::sqlite3_free(err_msg);
            err_msg = nullptr;
          }

          return false;
          // LCOV_EXCL_STOP GCOVR_EXCL_STOP
        }
      }

      if (rebuild) {
        ret = ::sqlite3_exec(wrapper_file.db, "VACUUM;", nullptr, nullptr, &err_msg);

        if VUNLIKELY (is_ready_to_quit()) {
          // LCOV_EXCL_START GCOVR_EXCL_START
          if (err_msg) {
            ::sqlite3_free(err_msg);
            err_msg = nullptr;
          }

          return false;
          // LCOV_EXCL_STOP GCOVR_EXCL_STOP
        } else if VUNLIKELY (ret != SQLITE_OK) {
          // LCOV_EXCL_START GCOVR_EXCL_START
          CLOG_W("Failed to vacuum: %s.", err_msg);

          if (err_msg) {
            ::sqlite3_free(err_msg);
            err_msg = nullptr;
          }

          return false;
          // LCOV_EXCL_STOP GCOVR_EXCL_STOP
        }

        ret = ::sqlite3_exec(wrapper_file.db, "DROP INDEX IF EXISTS idx_elapsed;", nullptr, nullptr, &err_msg);

        if VUNLIKELY (!is_ready_to_quit() && ret != SQLITE_OK) {
          // LCOV_EXCL_START GCOVR_EXCL_START
          CLOG_W("Failed to drop idx_elapsed: %s.", err_msg);

          if (err_msg) {
            ::sqlite3_free(err_msg);
            err_msg = nullptr;
          }

          return false;
          // LCOV_EXCL_STOP GCOVR_EXCL_STOP
        }

        ret = ::sqlite3_exec(wrapper_file.db, "DROP INDEX IF EXISTS idx_url;", nullptr, nullptr, &err_msg);

        if VUNLIKELY (!is_ready_to_quit() && ret != SQLITE_OK) {
          // LCOV_EXCL_START GCOVR_EXCL_START
          CLOG_W("Failed to drop idx_url: %s.", err_msg);

          if (err_msg) {
            ::sqlite3_free(err_msg);
            err_msg = nullptr;
          }

          return false;
          // LCOV_EXCL_STOP GCOVR_EXCL_STOP
        }

        ret = ::sqlite3_exec(wrapper_file.db, "DROP INDEX IF EXISTS idx_elapsed_url;", nullptr, nullptr, &err_msg);

        if VUNLIKELY (!is_ready_to_quit() && ret != SQLITE_OK) {
          // LCOV_EXCL_START GCOVR_EXCL_START
          CLOG_W("Failed to drop idx_elapsed_url: %s.", err_msg);

          if (err_msg) {
            ::sqlite3_free(err_msg);
            err_msg = nullptr;
          }

          return false;
          // LCOV_EXCL_STOP GCOVR_EXCL_STOP
        }

        ret = ::sqlite3_exec(wrapper_file.db, "CREATE INDEX idx_elapsed_url ON VLinkDatas(elapsed, url);", nullptr,
                             nullptr, &err_msg);

        if VUNLIKELY (is_ready_to_quit()) {
          // LCOV_EXCL_START GCOVR_EXCL_START
          if (err_msg) {
            ::sqlite3_free(err_msg);
            err_msg = nullptr;
          }

          return false;
          // LCOV_EXCL_STOP GCOVR_EXCL_STOP
        } else if VUNLIKELY (ret != SQLITE_OK) {
          // LCOV_EXCL_START GCOVR_EXCL_START
          CLOG_W("Failed to create idx_elapsed_url: %s.", err_msg);

          if (err_msg) {
            ::sqlite3_free(err_msg);
            err_msg = nullptr;
          }

          return false;
          // LCOV_EXCL_STOP GCOVR_EXCL_STOP
        }
      }

      ret = ::sqlite3_exec(wrapper_file.db, "PRAGMA optimize;", nullptr, nullptr, &err_msg);

      if VUNLIKELY (is_ready_to_quit()) {
        // LCOV_EXCL_START GCOVR_EXCL_START
        if (err_msg) {
          ::sqlite3_free(err_msg);
          err_msg = nullptr;
        }

        return false;
        // LCOV_EXCL_STOP GCOVR_EXCL_STOP
      } else if VUNLIKELY (ret != SQLITE_OK) {
        // LCOV_EXCL_START GCOVR_EXCL_START
        CLOG_W("Failed to optimize: %s.", err_msg);

        if (err_msg) {
          ::sqlite3_free(err_msg);
          err_msg = nullptr;
        }

        return false;
        // LCOV_EXCL_STOP GCOVR_EXCL_STOP
      }

      ret = ::sqlite3_prepare_v2(wrapper_file.db, "SELECT * FROM VLinkDatas;", -1, &wrapper_file.stmt, nullptr);

      if VUNLIKELY (is_ready_to_quit()) {
        // LCOV_EXCL_START GCOVR_EXCL_START
        if (err_msg) {
          ::sqlite3_free(err_msg);
          err_msg = nullptr;
        }

        return false;
        // LCOV_EXCL_STOP GCOVR_EXCL_STOP
      } else if VUNLIKELY (ret != SQLITE_OK) {
        // LCOV_EXCL_START GCOVR_EXCL_START
        CLOG_W("Failed to prepare datas table: %s.", ::sqlite3_errmsg(wrapper_file.db));

        if (err_msg) {
          ::sqlite3_free(err_msg);
          err_msg = nullptr;
        }

        return false;
        // LCOV_EXCL_STOP GCOVR_EXCL_STOP
      }

      ::sqlite3_step(wrapper_file.stmt);

      if (err_msg) {
        ::sqlite3_free(err_msg);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        err_msg = nullptr;        // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }
    }

    return true;
  });
#else
  (void)rebuild;
  return std::future<bool>();
#endif
}

void VDBReader::tag(const std::string& tag_name) {
#ifdef VLINK_ENABLE_SQLITE

  if VUNLIKELY (is_busy()) {
    VLOG_W("VDBReader: Is busy.");
  }

  post_task([this, tag_name]() {
    int ret = 0;
    std::string update_tag_sql;

    for (auto& wrapper_file : impl_->file_list) {
      if VUNLIKELY (!wrapper_file.db) {
        continue;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      if (wrapper_file.stmt) {
        ::sqlite3_finalize(wrapper_file.stmt);
        wrapper_file.stmt = nullptr;
      }

      update_tag_sql = "ALTER TABLE VLinkHeader ADD COLUMN tag TEXT;";

      ret = sqlite3_exec(wrapper_file.db, update_tag_sql.c_str(), nullptr, nullptr, nullptr);

      (void)ret;

      ::sqlite3_stmt* update_tag_stmt = nullptr;
      ret = ::sqlite3_prepare_v2(wrapper_file.db, "UPDATE VLinkHeader SET tag = ?;", -1, &update_tag_stmt, nullptr);
      SqliteStmtPtr update_tag_stmt_guard(update_tag_stmt);

      if VLIKELY (ret == SQLITE_OK) {
        ret = ::sqlite3_bind_text(update_tag_stmt, 1, tag_name.c_str(), static_cast<int>(tag_name.size()),
                                  SQLITE_TRANSIENT);  // NOLINT(performance-no-int-to-ptr)
      }

      if VLIKELY (ret == SQLITE_OK) {
        ret = ::sqlite3_step(update_tag_stmt);
      }

      if VUNLIKELY (ret != SQLITE_DONE) {
        CLOG_W("Failed to set tag: %s.", ::sqlite3_errmsg(wrapper_file.db));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        return;                                                               // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      update_tag_stmt_guard.reset();

      ret = ::sqlite3_prepare_v2(wrapper_file.db, "SELECT * FROM VLinkDatas;", -1, &wrapper_file.stmt, nullptr);

      if VUNLIKELY (ret != SQLITE_OK) {
        CLOG_W("Failed to prepare datas table: %s.",  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
               ::sqlite3_errmsg(wrapper_file.db));    // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        return;                                       // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      ::sqlite3_step(wrapper_file.stmt);

      impl_->info.tag_name = tag_name;
    }

    try {
#ifdef _WIN32
      std::filesystem::path file_path(Helpers::string_to_wstring(impl_->path));
      std::string suffix = Helpers::path_to_string(file_path.extension());
#else
      std::filesystem::path file_path(impl_->path);
      std::string suffix = file_path.extension().string();
#endif

      std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return std::tolower(c); });

      if (suffix == ".vdbx") {
        try {
          nlohmann::ordered_json root_json;
          nlohmann::ordered_json header_json;

          {
            std::ifstream file(file_path);

            file >> root_json;

            file.close();
          }

          header_json = root_json["VLinkHeader"];

          header_json["tag"] = tag_name;

          root_json["VLinkHeader"] = std::move(header_json);

          {
            std::ofstream filex(impl_->path, std::ios::out | std::ios::trunc);

            if VLIKELY (filex.is_open()) {
              filex << root_json.dump(4);
              filex.close();
            }
          }
        } catch (nlohmann::json::exception& e) {
          VLOG_W("VDBReader: JSON parse error, ", e.what(), ".");
        }
      }
    } catch (std::filesystem::filesystem_error& e) {
      VLOG_F("VDBReader: Filesystem error, ", e.what(), ".");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      return;                                                  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  });
#else
  (void)tag_name;
#endif
}

int64_t VDBReader::get_timestamp() const {
  std::shared_lock time_lock(impl_->time_mtx);

  if (impl_->status.load(std::memory_order_relaxed) == kPlaying) {
    if (impl_->is_pending.load(std::memory_order_relaxed)) {
      return impl_->real_elapsed.load(std::memory_order_relaxed) / 1000U;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    } else {
      return (impl_->real_elapsed.load(std::memory_order_relaxed) +
              (impl_->real_timer.get() * impl_->rate.load(std::memory_order_relaxed))) /
             1000U;
    }
  } else if (impl_->status.load(std::memory_order_relaxed) == kPaused) {
    return (impl_->real_elapsed.load(std::memory_order_relaxed) +
            ((impl_->real_timer.get() - impl_->pause_elapsed_timer.get()) *
             impl_->rate.load(std::memory_order_relaxed))) /
           1000U;
  } else {
    return 0;
  }
}

int64_t VDBReader::get_real_timestamp() const {
  if (impl_->status.load(std::memory_order_relaxed) == kPlaying ||
      impl_->status.load(std::memory_order_relaxed) == kPaused) {
    return impl_->real_elapsed.load(std::memory_order_relaxed) / 1000U;
  } else {
    return 0;
  }
}

VDBReader::Status VDBReader::get_status() const { return impl_->status.load(std::memory_order_relaxed); }

const BagReader::Info& VDBReader::get_info() const { return impl_->info; }

std::vector<SchemaData> VDBReader::detect_schema() {
#ifdef VLINK_ENABLE_SQLITE
  std::vector<SchemaData> schema_list;
  std::unordered_map<std::string, size_t> schema_index_map;

  if (!impl_->info.has_schema) {
    return schema_list;
  }

  schema_index_map.reserve(impl_->info.url_metas.size());

  for (auto& wrapper_file : impl_->file_list) {
    ::sqlite3_stmt* schema_stmt = nullptr;

    int ret = ::sqlite3_prepare_v2(wrapper_file.db, "SELECT ser, encoding, data FROM VLinkSchemas;", -1, &schema_stmt,
                                   nullptr);

    if VUNLIKELY (ret != SQLITE_OK) {
      // LCOV_EXCL_START GCOVR_EXCL_START
      CLOG_E("Failed to prepare schema table: %s.", ::sqlite3_errmsg(wrapper_file.db));

      if (schema_stmt) {
        ::sqlite3_finalize(schema_stmt);
        schema_stmt = nullptr;
      }

      return schema_list;
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    }

    const uint8_t* data = nullptr;
    size_t size = 0;

    while (::sqlite3_step(schema_stmt) == SQLITE_ROW) {
      SchemaData schema;
      schema.name = sqlite_column_text_or_empty(schema_stmt, get_column(0));
      schema.encoding = sqlite_column_text_or_empty(schema_stmt, get_column(1));
      schema.schema_type = SchemaData::resolve_type(SchemaType::kUnknown, schema.name, schema.encoding);

      data = static_cast<const uint8_t*>(::sqlite3_column_blob(schema_stmt, get_column(2)));
      size = ::sqlite3_column_bytes(schema_stmt, get_column(2));

      if (!schema.name.empty() && data && size > 0) {
        std::string schema_key = schema.name;
        schema_key.push_back('\x1F');
        schema_key.append(SchemaData::convert_type(schema.schema_type));

        auto schema_index_iter = schema_index_map.find(schema_key);

        if (schema_index_iter == schema_index_map.end()) {
          schema.data = Bytes::deep_copy(data, size);
          schema_index_map.emplace(schema_key, schema_list.size());
          schema_list.emplace_back(std::move(schema));
        } else {
          auto& current_schema = schema_list[schema_index_iter->second];

          if (current_schema.encoding.empty() && !schema.encoding.empty()) {
            current_schema.encoding = schema.encoding;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          }

          if (current_schema.data.empty()) {
            current_schema.data = Bytes::deep_copy(data, size);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          }
        }
      }
    }

    if VLIKELY (schema_stmt) {
      ::sqlite3_finalize(schema_stmt);
      schema_stmt = nullptr;
    }
  }

  return schema_list;
#else
  return std::vector<SchemaData>();
#endif
}

bool VDBReader::is_split_mode() const { return impl_->info.split_count > 0; }

int VDBReader::get_split_index() const { return impl_->split_index.load(std::memory_order_relaxed); }

bool VDBReader::is_jumping() const { return impl_->jump_flag.load(std::memory_order_relaxed); }

size_t VDBReader::get_max_task_count() const { return kMaxTaskSize; }

void VDBReader::on_begin() { MessageLoop::on_begin(); }

void VDBReader::on_end() { MessageLoop::on_end(); }

void VDBReader::update_status(Status status) {
  bool has_changed = false;

  if (status == kStopped) {
    if (impl_->status.load(std::memory_order_relaxed) != kStopped) {
      impl_->status.store(kStopped, std::memory_order_relaxed);
      has_changed = true;
    }
  } else if (status == kPaused) {
    if (impl_->status.load(std::memory_order_relaxed) != kPaused) {
      impl_->status.store(kPaused, std::memory_order_relaxed);
      has_changed = true;
    }
  } else if (status == kPlaying) {
    if (impl_->status.load(std::memory_order_relaxed) != kPlaying) {
      impl_->status.store(kPlaying, std::memory_order_relaxed);
      has_changed = true;
    }
  }

  if (has_changed) {
    if VLIKELY (impl_->status_callback) {
      impl_->status_callback(impl_->status.load(std::memory_order_relaxed));
    }
  }
}

void VDBReader::do_stop() {
#ifdef VLINK_ENABLE_SQLITE
  {
    std::unique_lock lock(impl_->mtx);
    impl_->stop_flag.store(true, std::memory_order_relaxed);
    impl_->pause_flag.store(false, std::memory_order_relaxed);
    impl_->pause_next_flag.store(false, std::memory_order_relaxed);
    impl_->jump_flag.store(false, std::memory_order_relaxed);
  }

  impl_->cv.notify_one();

  for (const auto& wrapper_file : impl_->file_list) {
    if (wrapper_file.db) {
      ::sqlite3_interrupt(wrapper_file.db);
    }
  }
#endif
}

void VDBReader::do_pause() {
  std::unique_lock lock(impl_->mtx);

  while (impl_->pause_flag.load(std::memory_order_relaxed)) {
    impl_->pause_elapsed_timer.restart();
    update_status(kPaused);

    impl_->cv.wait(lock, [this]() -> bool {
      return impl_->stop_flag.load(std::memory_order_relaxed) || !impl_->pause_flag.load(std::memory_order_relaxed) ||
             impl_->pause_next_flag.load(std::memory_order_relaxed) ||
             impl_->jump_flag.load(std::memory_order_relaxed) || is_ready_to_quit();
    });

    impl_->pause_elapsed.fetch_add(impl_->pause_elapsed_timer.get(), std::memory_order_relaxed);

    {
      std::lock_guard time_lock(impl_->time_mtx);
      impl_->real_timer.restart();

      if (impl_->offset_elapsed.load(std::memory_order_relaxed) > 0) {
        // LCOV_EXCL_START GCOVR_EXCL_START
        impl_->real_elapsed.fetch_add((impl_->offset_timer.get() - impl_->pause_elapsed_timer.get() -
                                       impl_->extra_elapsed.load(std::memory_order_relaxed)) *
                                          impl_->rate.load(std::memory_order_relaxed),
                                      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
                                      std::memory_order_relaxed);
      }
    }

    update_status(kPlaying);

    if (impl_->pause_next_flag.load(std::memory_order_relaxed)) {
      impl_->pause_elapsed.fetch_sub(impl_->offset_elapsed.load(std::memory_order_relaxed), std::memory_order_relaxed);
      break;
    } else if (impl_->offset_elapsed.load(std::memory_order_relaxed) > 0) {  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      impl_->offset_timer.restart();                                         // LCOV_EXCL_LINE GCOVR_EXCL_LINE

      impl_->cv.wait_for(  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          lock,
          // LCOV_EXCL_START GCOVR_EXCL_START
          std::chrono::microseconds(impl_->offset_elapsed.load(std::memory_order_relaxed)), [this]() -> bool {
            return impl_->stop_flag.load(std::memory_order_relaxed) ||
                   impl_->pause_flag.load(std::memory_order_relaxed) ||
                   impl_->pause_next_flag.load(std::memory_order_relaxed) ||
                   impl_->jump_flag.load(std::memory_order_relaxed) || is_ready_to_quit();
          });

      if VUNLIKELY (impl_->pause_flag.load(std::memory_order_relaxed)) {
        impl_->offset_elapsed.fetch_sub(impl_->offset_timer.get(), std::memory_order_relaxed);
        // LCOV_EXCL_STOP GCOVR_EXCL_STOP
      } else {
        impl_->offset_elapsed.store(0, std::memory_order_relaxed);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }
    }
  }
}

bool VDBReader::prepare_file(void* file) {
#ifdef VLINK_ENABLE_SQLITE
  auto* wrapper_file = static_cast<Impl::WrapperFile*>(file);

  wrapper_file->has_completed = true;

  int ret = 0;

  // opt busy_timeout
  ::sqlite3_busy_timeout(wrapper_file->db, 100);

#if ENABLE_DATABASE_TABLE_CHECK
  {
    // search count
    ::sqlite3_stmt* search_stmt = nullptr;
    bool need_rebuild_header = false;
    ret = ::sqlite3_prepare_v2(wrapper_file->db, "PRAGMA table_info(VLinkHeader);", -1, &search_stmt, nullptr);

    if VUNLIKELY (ret != SQLITE_OK) {
      wrapper_file->has_completed = false;

      if (!impl_->try_to_fix || impl_->read_only) {
        CLOG_F("Failed to search header table: %s.", ::sqlite3_errmsg(wrapper_file->db));
        return false;
      }

      need_rebuild_header = true;
    } else {
      const char* expected_columns[] = {
          "major",   "minor", "patch", "count",    "duration", "accuracy",        "compress",
          "process", "date",  "tag",   "complete", "timezone", "start_timestamp",
      };

      const char* expected_types[] = {
          "INTEGER", "INTEGER", "INTEGER", "INTEGER", "INTEGER", "TEXT",    "TEXT",
          "TEXT",    "TEXT",    "TEXT",    "INTEGER", "INTEGER", "INTEGER",
      };

      size_t column_index = 0;
      int invalid_header_num = 0;
      int step_ret = SQLITE_OK;
      for (;;) {
        step_ret = ::sqlite3_step(search_stmt);

        if (step_ret != SQLITE_ROW) {
          break;
        }

        if (column_index >= sizeof(expected_columns) / sizeof(expected_columns[0])) {
          need_rebuild_header = true;
          invalid_header_num = 1;
          break;
        }

        const auto* column_name = reinterpret_cast<const char*>(::sqlite3_column_text(search_stmt, get_column(1)));
        const auto* column_type = reinterpret_cast<const char*>(::sqlite3_column_text(search_stmt, get_column(2)));

        if (!column_name || std::strcmp(column_name, expected_columns[column_index]) != 0) {
          need_rebuild_header = true;
          invalid_header_num = 2;
          break;
        }

        if (!column_type || std::strcmp(column_type, expected_types[column_index]) != 0) {
          need_rebuild_header = true;
          invalid_header_num = 3;
          break;
        }

        ++column_index;
      }

      if (!need_rebuild_header && step_ret != SQLITE_DONE) {
        if VLIKELY (search_stmt) {
          ::sqlite3_finalize(search_stmt);
          search_stmt = nullptr;
        }

        wrapper_file->has_completed = false;
        CLOG_F("Failed to inspect header table: %s.", ::sqlite3_errmsg(wrapper_file->db));
        return false;
      }

      if (!need_rebuild_header) {
        need_rebuild_header = (column_index != sizeof(expected_columns) / sizeof(expected_columns[0]));
        if (need_rebuild_header) {
          invalid_header_num = 4;
        }
      }

      if (need_rebuild_header) {
        if (!impl_->try_to_fix || impl_->read_only) {
          CLOG_F("VDBReader: Table [VLinkHeader] is incompatible, num=%d.", invalid_header_num);
          return false;
        }

        CLOG_W("VDBReader: Table [VLinkHeader] is incompatible, num=%d. Try to rebuild.", invalid_header_num);
      }
    }

    if VLIKELY (search_stmt) {
      ::sqlite3_finalize(search_stmt);
      search_stmt = nullptr;
    }

    if (need_rebuild_header) {
      wrapper_file->has_completed = false;
      char* err_msg = nullptr;

      ret = ::sqlite3_exec(wrapper_file->db, "DROP TABLE IF EXISTS VLinkHeader;", nullptr, nullptr, &err_msg);
      if VUNLIKELY (ret != SQLITE_OK) {
        CLOG_F("Failed to drop VLinkHeader: %s.", err_msg);

        if (err_msg) {
          ::sqlite3_free(err_msg);
          err_msg = nullptr;
        }

        return false;
      }

      ret = ::sqlite3_exec(
          wrapper_file->db,
          "CREATE TABLE IF NOT EXISTS VLinkHeader(major INTEGER, minor INTEGER, patch INTEGER, count INTEGER, "
          "duration INTEGER, accuracy TEXT, compress TEXT, process TEXT, date TEXT, tag TEXT, complete INTEGER, "
          "timezone INTEGER, start_timestamp INTEGER);",
          nullptr, nullptr, &err_msg);
      if VUNLIKELY (ret != SQLITE_OK) {
        CLOG_F("Failed to create header table: %s.", err_msg);

        if (err_msg) {
          ::sqlite3_free(err_msg);
          err_msg = nullptr;
        }

        return false;
      }

      ::sqlite3_stmt* create_header_stmt = nullptr;
      ret = ::sqlite3_prepare_v2(
          wrapper_file->db,
          "INSERT INTO VLinkHeader (major, minor, patch, count, duration, accuracy, compress, "
          "process, date, tag, complete, timezone, start_timestamp) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
          -1, &create_header_stmt, nullptr);

      if VUNLIKELY (ret != SQLITE_OK) {
        CLOG_F("Failed to prepare header table: %s.", ::sqlite3_errmsg(wrapper_file->db));

        if (err_msg) {
          ::sqlite3_free(err_msg);
          err_msg = nullptr;
        }

        return false;
      }

      ::sqlite3_bind_int(create_header_stmt, get_column(0), VLINK_VERSION_MAJOR);
      ::sqlite3_bind_int(create_header_stmt, get_column(1), VLINK_VERSION_MINOR);
      ::sqlite3_bind_int(create_header_stmt, get_column(2), VLINK_VERSION_PATCH);
      ::sqlite3_bind_int64(create_header_stmt, get_column(3), 0);
      ::sqlite3_bind_int64(create_header_stmt, get_column(4), 0);
      ::sqlite3_bind_text(create_header_stmt, get_column(5), "MicroSecond", -1, SQLITE_STATIC);
      ::sqlite3_bind_text(create_header_stmt, get_column(6), "None", -1, SQLITE_STATIC);
      ::sqlite3_bind_text(create_header_stmt, get_column(7), "None", -1, SQLITE_STATIC);
      ::sqlite3_bind_text(create_header_stmt, get_column(8), "None", -1, SQLITE_STATIC);
      ::sqlite3_bind_text(create_header_stmt, get_column(9), "None", -1, SQLITE_STATIC);
      ::sqlite3_bind_int(create_header_stmt, get_column(10), 0);
      ::sqlite3_bind_int(create_header_stmt, get_column(11), 0);
      ::sqlite3_bind_int64(create_header_stmt, get_column(12), 0);

      ret = ::sqlite3_step(create_header_stmt);

      if VUNLIKELY (ret != SQLITE_DONE) {
        CLOG_F("Failed to insert header table: %s.", ::sqlite3_errmsg(wrapper_file->db));

        if (err_msg) {
          ::sqlite3_free(err_msg);
          err_msg = nullptr;
        }

        return false;
      }

      if VLIKELY (create_header_stmt) {
        ::sqlite3_finalize(create_header_stmt);
        create_header_stmt = nullptr;
      }

      if (err_msg) {
        ::sqlite3_free(err_msg);
        err_msg = nullptr;
      }
    }

    ::sqlite3_stmt* schema_info_stmt = nullptr;
    ret = ::sqlite3_prepare_v2(wrapper_file->db, "PRAGMA table_info(VLinkSchemas);", -1, &schema_info_stmt, nullptr);

    if VUNLIKELY (ret != SQLITE_OK) {
      wrapper_file->has_completed = false;
      CLOG_F("Failed to search schema table: %s.", ::sqlite3_errmsg(wrapper_file->db));
      return false;
    }

    {
      const char* expected_columns[] = {"ser", "encoding", "data"};

      const char* expected_types[] = {"TEXT", "TEXT", "BLOB"};

      size_t column_index = 0;
      bool invalid_schema_table = false;
      int invalid_schema_num = 0;
      int step_ret = SQLITE_OK;

      for (;;) {
        step_ret = ::sqlite3_step(schema_info_stmt);

        if (step_ret != SQLITE_ROW) {
          break;
        }

        if (column_index >= sizeof(expected_columns) / sizeof(expected_columns[0])) {
          invalid_schema_table = true;
          invalid_schema_num = 1;
          break;
        }

        const auto* column_name = reinterpret_cast<const char*>(::sqlite3_column_text(schema_info_stmt, get_column(1)));
        const auto* column_type = reinterpret_cast<const char*>(::sqlite3_column_text(schema_info_stmt, get_column(2)));

        if (!column_name || std::strcmp(column_name, expected_columns[column_index]) != 0) {
          invalid_schema_table = true;
          invalid_schema_num = 2;
          break;
        }

        if (!column_type || std::strcmp(column_type, expected_types[column_index]) != 0) {
          invalid_schema_table = true;
          invalid_schema_num = 3;
          break;
        }

        ++column_index;
      }

      if (!invalid_schema_table && step_ret != SQLITE_DONE) {
        if VLIKELY (schema_info_stmt) {
          ::sqlite3_finalize(schema_info_stmt);
          schema_info_stmt = nullptr;
        }

        wrapper_file->has_completed = false;
        CLOG_F("Failed to inspect schema table: %s.", ::sqlite3_errmsg(wrapper_file->db));
        return false;
      }

      if (!invalid_schema_table) {
        invalid_schema_table = (column_index != sizeof(expected_columns) / sizeof(expected_columns[0]));
        if (invalid_schema_table) {
          invalid_schema_num = 4;
        }
      }

      if VLIKELY (schema_info_stmt) {
        ::sqlite3_finalize(schema_info_stmt);
        schema_info_stmt = nullptr;
      }

      if VUNLIKELY (invalid_schema_table) {
        wrapper_file->has_completed = false;
        CLOG_F("VDBReader: Table [VLinkSchemas] is incompatible, num=%d.", invalid_schema_num);
        return false;
      }
    }

    ::sqlite3_stmt* urls_info_stmt = nullptr;
    ret = ::sqlite3_prepare_v2(wrapper_file->db, "PRAGMA table_info(VLinkUrls);", -1, &urls_info_stmt, nullptr);

    if VUNLIKELY (ret != SQLITE_OK) {
      wrapper_file->has_completed = false;
      CLOG_F("Failed to search urls table: %s.", ::sqlite3_errmsg(wrapper_file->db));
      return false;
    }

    {
      const char* expected_columns[] = {"id", "url", "type", "ser", "encoding", "count", "loss", "size", "freq"};

      const char* expected_types[] = {"INTEGER", "TEXT", "TEXT", "TEXT", "TEXT", "INTEGER", "REAL", "INTEGER", "REAL"};

      size_t column_index = 0;
      bool invalid_urls_table = false;
      int invalid_urls_num = 0;
      int step_ret = SQLITE_OK;

      for (;;) {
        step_ret = ::sqlite3_step(urls_info_stmt);

        if (step_ret != SQLITE_ROW) {
          break;
        }

        if (column_index >= sizeof(expected_columns) / sizeof(expected_columns[0])) {
          invalid_urls_table = true;
          invalid_urls_num = 1;
          break;
        }

        const auto* column_name = reinterpret_cast<const char*>(::sqlite3_column_text(urls_info_stmt, get_column(1)));
        const auto* column_type = reinterpret_cast<const char*>(::sqlite3_column_text(urls_info_stmt, get_column(2)));

        if (!column_name || std::strcmp(column_name, expected_columns[column_index]) != 0) {
          invalid_urls_table = true;
          invalid_urls_num = 2;
          break;
        }

        if (!column_type || std::strcmp(column_type, expected_types[column_index]) != 0) {
          invalid_urls_table = true;
          invalid_urls_num = 3;
          break;
        }

        ++column_index;
      }

      if (!invalid_urls_table && step_ret != SQLITE_DONE) {
        if VLIKELY (urls_info_stmt) {
          ::sqlite3_finalize(urls_info_stmt);
          urls_info_stmt = nullptr;
        }

        wrapper_file->has_completed = false;
        CLOG_F("Failed to inspect urls table: %s.", ::sqlite3_errmsg(wrapper_file->db));
        return false;
      }

      if (!invalid_urls_table) {
        invalid_urls_table = (column_index != sizeof(expected_columns) / sizeof(expected_columns[0]));
        if (invalid_urls_table) {
          invalid_urls_num = 4;
        }
      }

      if VLIKELY (urls_info_stmt) {
        ::sqlite3_finalize(urls_info_stmt);
        urls_info_stmt = nullptr;
      }

      if VUNLIKELY (invalid_urls_table) {
        wrapper_file->has_completed = false;
        CLOG_F("VDBReader: Table [VLinkUrls] is incompatible, num=%d.", invalid_urls_num);
        return false;
      }
    }

    ::sqlite3_stmt* datas_info_stmt = nullptr;
    ret = ::sqlite3_prepare_v2(wrapper_file->db, "PRAGMA table_info(VLinkDatas);", -1, &datas_info_stmt, nullptr);

    if VUNLIKELY (ret != SQLITE_OK) {
      wrapper_file->has_completed = false;
      CLOG_F("Failed to search datas table: %s.", ::sqlite3_errmsg(wrapper_file->db));
      return false;
    }

    {
      const char* expected_columns[] = {"elapsed", "url", "action", "data"};

      const char* expected_types[] = {"INTEGER", "INTEGER", "TEXT", "BLOB"};

      size_t column_index = 0;
      bool invalid_datas_table = false;
      int invalid_datas_num = 0;
      int step_ret = SQLITE_OK;

      for (;;) {
        step_ret = ::sqlite3_step(datas_info_stmt);

        if (step_ret != SQLITE_ROW) {
          break;
        }

        if (column_index >= sizeof(expected_columns) / sizeof(expected_columns[0])) {
          invalid_datas_table = true;
          invalid_datas_num = 1;
          break;
        }

        const auto* column_name = reinterpret_cast<const char*>(::sqlite3_column_text(datas_info_stmt, get_column(1)));
        const auto* column_type = reinterpret_cast<const char*>(::sqlite3_column_text(datas_info_stmt, get_column(2)));

        if (!column_name || std::strcmp(column_name, expected_columns[column_index]) != 0) {
          invalid_datas_table = true;
          invalid_datas_num = 2;
          break;
        }

        if (!column_type || std::strcmp(column_type, expected_types[column_index]) != 0) {
          invalid_datas_table = true;
          invalid_datas_num = 3;
          break;
        }

        ++column_index;
      }

      if (!invalid_datas_table && step_ret != SQLITE_DONE) {
        if VLIKELY (datas_info_stmt) {
          ::sqlite3_finalize(datas_info_stmt);
          datas_info_stmt = nullptr;
        }

        wrapper_file->has_completed = false;
        CLOG_F("Failed to inspect datas table: %s.", ::sqlite3_errmsg(wrapper_file->db));
        return false;
      }

      if (!invalid_datas_table) {
        invalid_datas_table = (column_index != sizeof(expected_columns) / sizeof(expected_columns[0]));
        if (invalid_datas_table) {
          invalid_datas_num = 4;
        }
      }

      if VLIKELY (datas_info_stmt) {
        ::sqlite3_finalize(datas_info_stmt);
        datas_info_stmt = nullptr;
      }

      if VUNLIKELY (invalid_datas_table) {
        wrapper_file->has_completed = false;
        CLOG_F("VDBReader: Table [VLinkDatas] is incompatible, num=%d.", invalid_datas_num);
        return false;
      }
    }
  }
#endif

  // prepare header table
  ::sqlite3_stmt* header_stmt = nullptr;
  ret = ::sqlite3_prepare_v2(
      wrapper_file->db,
      "SELECT major, minor, patch, count, duration, accuracy, compress, process, date, tag, complete, timezone, "
      "start_timestamp FROM VLinkHeader LIMIT 1;",
      -1, &header_stmt, nullptr);

  if VUNLIKELY (ret != SQLITE_OK) {
    wrapper_file->has_completed = false;
    CLOG_F("Failed to prepare header table: %s.", ::sqlite3_errmsg(wrapper_file->db));
    return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  SqliteStmtPtr header_stmt_guard(header_stmt);

  ret = ::sqlite3_step(header_stmt);

  if VUNLIKELY (ret != SQLITE_ROW) {
    wrapper_file->has_completed = false;                                            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    CLOG_F("Failed to get header table: %s.", ::sqlite3_errmsg(wrapper_file->db));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;                                                                   // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  int major = ::sqlite3_column_int(header_stmt, get_column(0));
  int minor = ::sqlite3_column_int(header_stmt, get_column(1));
  int patch = ::sqlite3_column_int(header_stmt, get_column(2));

  if VUNLIKELY (major != VLINK_VERSION_MAJOR) {
    wrapper_file->has_completed = false;                     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    VLOG_F("VDBReader: Database version is incompatible.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;                                            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  impl_->info.version = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
  impl_->info.storage_type = "SQLite3";
  impl_->info.message_count = ::sqlite3_column_int64(header_stmt, get_column(3));
  impl_->info.total_duration = ::sqlite3_column_int64(header_stmt, get_column(4)) / 1000U;
  impl_->info.time_accuracy = sqlite_column_text_or_empty(header_stmt, get_column(5));
  impl_->info.compression_type = sqlite_column_text_or_empty(header_stmt, get_column(6));
  impl_->info.process_name = sqlite_column_text_or_empty(header_stmt, get_column(7));
  impl_->info.date_time = sqlite_column_text_or_empty(header_stmt, get_column(8));

  const char* tag_name_str = reinterpret_cast<const char*>(::sqlite3_column_text(header_stmt, get_column(9)));

  if (tag_name_str) {
    impl_->info.tag_name = tag_name_str;
  } else {
    impl_->info.tag_name = "Empty";
  }

  impl_->info.has_completed = (::sqlite3_column_int(header_stmt, get_column(10)) == 1);
  impl_->info.timezone = ::sqlite3_column_int(header_stmt, get_column(11));
  impl_->info.start_timestamp = ::sqlite3_column_int64(header_stmt, get_column(12));

  if VUNLIKELY (impl_->info.start_timestamp < 0) {
    impl_->info.start_timestamp = 0;
    wrapper_file->has_completed = false;

    if (impl_->read_only) {
      VLOG_E("VDBReader: Invalid start_timestamp.");
    }
  }

  if (impl_->info.compression_type.empty() || impl_->info.compression_type == "None" ||
      impl_->info.compression_type == "NONE" || impl_->info.compression_type == "none") {
    impl_->enable_compress = false;
  } else {
    impl_->enable_compress = true;
  }

  if VUNLIKELY (impl_->info.time_accuracy != "MicroSecond") {
    wrapper_file->has_completed = false;
    VLOG_F("VDBReader: Database accuracy is not supported.");
    return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  header_stmt_guard.reset();
  header_stmt = nullptr;

  // prepare schema table
  ::sqlite3_stmt* schema_stmt = nullptr;

  ret = ::sqlite3_prepare_v2(wrapper_file->db,
                             "SELECT 1 FROM sqlite_master WHERE type='table' AND name='VLinkSchemas' LIMIT 1;", -1,
                             &schema_stmt, nullptr);

  if (ret == SQLITE_OK) {
    if (::sqlite3_step(schema_stmt) == SQLITE_ROW) {
      sqlite3_stmt* count_stmt = nullptr;

      ret = ::sqlite3_prepare_v2(wrapper_file->db, "SELECT EXISTS(SELECT 1 FROM VLinkSchemas LIMIT 1);", -1,
                                 &count_stmt, nullptr);

      if (ret == SQLITE_OK) {
        if (::sqlite3_step(count_stmt) == SQLITE_ROW) {
          int exists = ::sqlite3_column_int(count_stmt, 0);

          if (exists) {
            wrapper_file->has_schema = true;
          }
        }
      }

      if VLIKELY (count_stmt) {
        ::sqlite3_finalize(count_stmt);
        count_stmt = nullptr;
      }
    }
  }

  if VLIKELY (schema_stmt) {
    ::sqlite3_finalize(schema_stmt);
    schema_stmt = nullptr;
  }

  // prepare urls table
  ::sqlite3_stmt* urls_stmt = nullptr;
  ret = ::sqlite3_prepare_v2(wrapper_file->db,
                             "SELECT id, url, type, ser, encoding, count, loss, size, freq FROM VLinkUrls;", -1,
                             &urls_stmt, nullptr);

  if VUNLIKELY (ret != SQLITE_OK) {
    wrapper_file->has_completed = false;
    CLOG_F("Failed to prepare urls table: %s.", ::sqlite3_errmsg(wrapper_file->db));
    return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  url_ser_map().clear();
  url_schema_type_map().clear();
  impl_->info.url_metas.clear();
  impl_->raw_url_metas.clear();

  impl_->info.total_raw_size = 0;

  while (::sqlite3_step(urls_stmt) == SQLITE_ROW) {
    Info::UrlMeta url_meta;
    url_meta.valid = true;
    url_meta.index = ::sqlite3_column_int(urls_stmt, get_column(0));
    url_meta.url = sqlite_column_text_or_empty(urls_stmt, get_column(1));
    url_meta.url_type = sqlite_column_text_or_empty(urls_stmt, get_column(2));
    url_meta.ser_type = sqlite_column_text_or_empty(urls_stmt, get_column(3));
    const auto* encoding_label = reinterpret_cast<const char*>(::sqlite3_column_text(urls_stmt, get_column(4)));
    url_meta.schema_type = SchemaData::resolve_type(SchemaData::convert_encoding(encoding_label ? encoding_label : ""),
                                                    url_meta.ser_type);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

    url_meta.count = static_cast<size_t>(::sqlite3_column_int64(urls_stmt, get_column(5)));
    url_meta.loss = ::sqlite3_column_double(urls_stmt, get_column(6));
    url_meta.size = ::sqlite3_column_int64(urls_stmt, get_column(7));
    url_meta.freq = ::sqlite3_column_double(urls_stmt, get_column(8));

    impl_->info.total_raw_size += url_meta.size;

    wrapper_file->id_to_url_map.emplace(url_meta.index, url_meta.url);
    wrapper_file->url_to_id_map.emplace(url_meta.url, url_meta.index);

    url_ser_map().emplace(url_meta.url, url_meta.ser_type);
    url_schema_type_map().emplace(url_meta.url, url_meta.schema_type);
    impl_->info.url_metas.emplace_back(std::move(url_meta));
  }

  std::sort(impl_->info.url_metas.begin(), impl_->info.url_metas.end());
  impl_->raw_url_metas = impl_->info.url_metas;
  rebuild_url_meta_lookup(impl_->info.url_metas);

  if VLIKELY (urls_stmt) {
    ::sqlite3_finalize(urls_stmt);
    urls_stmt = nullptr;
  }

  // get idx_elapsed
  ::sqlite3_stmt* idx_elapsed_stmt = nullptr;

  ret = ::sqlite3_prepare_v2(
      wrapper_file->db,
      "SELECT 1 FROM sqlite_master WHERE tbl_name='VLinkDatas' AND type='index' AND name='idx_elapsed' LIMIT 1;", -1,
      &idx_elapsed_stmt, nullptr);

  if (ret == SQLITE_OK) {
    if (::sqlite3_step(idx_elapsed_stmt) == SQLITE_ROW) {
      wrapper_file->has_idx_elapsed = true;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
  }

  if VLIKELY (idx_elapsed_stmt) {
    ::sqlite3_finalize(idx_elapsed_stmt);
    idx_elapsed_stmt = nullptr;
  }

  // get idx_elapsed_url
  ::sqlite3_stmt* idx_elapsed_url_stmt = nullptr;

  ret = ::sqlite3_prepare_v2(
      wrapper_file->db,
      "SELECT 1 FROM sqlite_master WHERE tbl_name='VLinkDatas' AND type='index' AND name='idx_elapsed_url' LIMIT 1;",
      -1, &idx_elapsed_url_stmt, nullptr);

  if (ret == SQLITE_OK) {
    if (::sqlite3_step(idx_elapsed_url_stmt) == SQLITE_ROW) {
      wrapper_file->has_idx_elapsed = true;
      wrapper_file->has_idx_url = true;
    }
  }

  if VLIKELY (idx_elapsed_url_stmt) {
    ::sqlite3_finalize(idx_elapsed_url_stmt);
    idx_elapsed_url_stmt = nullptr;
  }

  impl_->info.has_idx_elapsed = wrapper_file->has_idx_elapsed;
  impl_->info.has_idx_url = wrapper_file->has_idx_url;
  impl_->info.has_schema = wrapper_file->has_schema;

  // prepare datas

  if (wrapper_file->has_idx_elapsed) {
    ret = ::sqlite3_prepare_v2(wrapper_file->db, "SELECT * FROM VLinkDatas ORDER BY elapsed LIMIT 1;", -1,
                               &wrapper_file->stmt, nullptr);
  } else {
    ret = ::sqlite3_prepare_v2(wrapper_file->db, "SELECT * FROM VLinkDatas LIMIT 1;", -1, &wrapper_file->stmt, nullptr);
  }

  if VUNLIKELY (ret != SQLITE_OK) {
    wrapper_file->has_completed = false;
    CLOG_F("Failed to prepare datas table: %s.", ::sqlite3_errmsg(wrapper_file->db));
    return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  // get blank time
  ::sqlite3_step(wrapper_file->stmt);
  impl_->info.blank_duration = ::sqlite3_column_int64(wrapper_file->stmt, get_column(0)) / 1000U;

  if VUNLIKELY (impl_->info.blank_duration < 0) {
    impl_->info.blank_duration = 0;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if VLIKELY (wrapper_file->stmt) {
    ::sqlite3_finalize(wrapper_file->stmt);
    wrapper_file->stmt = nullptr;
  }

  // update duration

  if (wrapper_file->has_idx_elapsed && impl_->info.has_completed) {
    ret = ::sqlite3_prepare_v2(wrapper_file->db, "SELECT * FROM VLinkDatas ORDER BY elapsed DESC LIMIT 1;", -1,
                               &wrapper_file->stmt, nullptr);

    if VUNLIKELY (ret != SQLITE_OK) {
      // LCOV_EXCL_START GCOVR_EXCL_START
      wrapper_file->has_completed = false;
      CLOG_F("Failed to prepare datas table: %s.", ::sqlite3_errmsg(wrapper_file->db));
      return false;
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    }

    // get duration time
    ::sqlite3_step(wrapper_file->stmt);
    int64_t total_duration = ::sqlite3_column_int64(wrapper_file->stmt, get_column(0)) / 1000U;

    if (total_duration > impl_->info.blank_duration) {
      impl_->info.total_duration = total_duration;
    }

    if VLIKELY (wrapper_file->stmt) {
      ::sqlite3_finalize(wrapper_file->stmt);
      wrapper_file->stmt = nullptr;
    }
  }
  return true;
#else
  (void)file;
  VLOG_F("VDBReader: The compile macro VLINK_ENABLE_SQLITE is not turned on.");
  return false;
#endif
}

void VDBReader::open(const std::string& path) {
#ifdef VLINK_ENABLE_SQLITE
  close();

  auto to_open = [this](Impl::WrapperFile& wrapper_file) -> bool {
    if (impl_->read_only) {
      int ret = ::sqlite3_open_v2(wrapper_file.path.c_str(), &wrapper_file.db, SQLITE_OPEN_READONLY, nullptr);

      if VUNLIKELY (ret != SQLITE_OK) {
        // LCOV_EXCL_START GCOVR_EXCL_START
        CLOG_F("Failed to open database [%s].", wrapper_file.path.c_str());

        if (wrapper_file.db) {
          ::sqlite3_close_v2(wrapper_file.db);
          wrapper_file.db = nullptr;
        }

        return false;
        // LCOV_EXCL_STOP GCOVR_EXCL_STOP
      }
    } else {
      int ret = ::sqlite3_open_v2(wrapper_file.path.c_str(), &wrapper_file.db,
                                  SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);

      if VUNLIKELY (ret != SQLITE_OK) {
        // LCOV_EXCL_START GCOVR_EXCL_START
        CLOG_F("Failed to open database [%s].", wrapper_file.path.c_str());

        if (wrapper_file.db) {
          ::sqlite3_close_v2(wrapper_file.db);
          wrapper_file.db = nullptr;
        }

        return false;
        // LCOV_EXCL_STOP GCOVR_EXCL_STOP
      }
    }

    if VUNLIKELY (!prepare_file(&wrapper_file)) {
      // LCOV_EXCL_START GCOVR_EXCL_START
      if (wrapper_file.stmt) {
        ::sqlite3_finalize(wrapper_file.stmt);
        wrapper_file.stmt = nullptr;
      }

      if (wrapper_file.db) {
        ::sqlite3_close_v2(wrapper_file.db);
        wrapper_file.db = nullptr;
      }

      return false;
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    }

    return true;
  };

  impl_->path = path;

  impl_->total_start_timestamp_ns = -1;
  impl_->total_has_completed = true;

  try {
#ifdef _WIN32
    std::filesystem::path file_path(Helpers::string_to_wstring(path));

    impl_->info.file_name = Helpers::path_to_string(file_path.filename());

    impl_->info.file_size = 0;

    std::string suffix = Helpers::path_to_string(file_path.extension());
#else
    std::filesystem::path file_path(path);

    impl_->info.file_name = file_path.filename().string();

    impl_->info.file_size = 0;

    std::string suffix = file_path.extension().string();
#endif

    if VUNLIKELY (!std::filesystem::exists(file_path)) {
      CLOG_F("Database [%s] does not exist.", path.c_str());
      return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    std::filesystem::path parent_path;

    try {
      parent_path = file_path.parent_path();
    } catch (std::filesystem::filesystem_error&) {  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return std::tolower(c); });

    if (suffix == ".vdbx") {
      try {
        int64_t blank_duration = -1;

        nlohmann::json root_json;

        {
          std::ifstream filex(file_path);

          filex >> root_json;

          filex.close();
        }

        nlohmann::json header_json = root_json["VLinkHeader"];
        nlohmann::json urls_json = root_json["VLinkUrls"];
        nlohmann::json files_json = root_json["VLinkFiles"];

        impl_->info.file_size = 0;

        int file_index = 0;

        if (!files_json.empty()) {
          impl_->file_list.reserve(files_json.size());

          impl_->info.has_idx_elapsed = true;
          impl_->info.has_idx_url = true;
          impl_->info.has_schema = false;
        } else {
          impl_->info.has_idx_elapsed = false;
          impl_->info.has_idx_url = false;
          impl_->info.has_schema = false;
        }

        std::filesystem::path file_db;
        std::string file_db_str;

        for (const auto& file_info : files_json) {
#ifdef _WIN32

          if (parent_path.empty()) {
            file_db = std::filesystem::path(Helpers::string_to_wstring(file_info));
          } else {
            file_db = parent_path / std::filesystem::path(Helpers::string_to_wstring(file_info));
          }
#else

          if (parent_path.empty()) {
            file_db = std::filesystem::path(file_info);
          } else {
            file_db = parent_path / std::filesystem::path(file_info);
          }
#endif

          file_db_str = file_db.string();

          if VUNLIKELY (!std::filesystem::exists(file_db)) {
            CLOG_F("Database [%s] does not exist.", file_db_str.c_str());
            return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          }

          Impl::WrapperFile wrapper_file;
          wrapper_file.path = file_db_str;
          wrapper_file.index = file_index;

          if VUNLIKELY (!to_open(wrapper_file)) {
            // LCOV_EXCL_START GCOVR_EXCL_START
            CLOG_W("VDBReader: Skipping invalid database [%s].", file_db_str.c_str());
            continue;
          }
          // LCOV_EXCL_STOP GCOVR_EXCL_STOP

          if (!wrapper_file.has_idx_elapsed) {
            impl_->info.has_idx_elapsed = false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          }

          if (!wrapper_file.has_idx_url) {
            impl_->info.has_idx_url = false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          }

          if (wrapper_file.has_schema) {
            impl_->info.has_schema = true;
          }

          std::error_code db_size_ec;
          std::uintmax_t file_size = std::filesystem::file_size(file_db, db_size_ec);

          if VUNLIKELY (db_size_ec) {
            CLOG_W("VDBReader: file_size failed for [%s]: %s.", file_db_str.c_str(),  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                   db_size_ec.message().c_str());                                     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
            file_size = 0;                                                            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          }

          impl_->info.file_size += file_size;

          wrapper_file.start_timestamp_ns = impl_->info.start_timestamp * 1000'000;
          wrapper_file.begin = impl_->info.blank_duration;
          wrapper_file.end = impl_->info.total_duration;

          if (impl_->total_start_timestamp_ns < 0) {
            impl_->total_start_timestamp_ns = wrapper_file.start_timestamp_ns;
          }

          if (!wrapper_file.has_completed) {
            impl_->total_has_completed = false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          }

          if (blank_duration < 0) {
            blank_duration = impl_->info.blank_duration;
          }

          impl_->file_list.emplace_back(std::move(wrapper_file));
          ++file_index;
        }

        impl_->info.split_count = impl_->file_list.size();

        if VUNLIKELY (impl_->file_list.empty()) {
          VLOG_F("VDBReader: DB list is empty.");
          return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }

        int version_major = header_json["major"];
        int version_minor = header_json["minor"];
        int version_patch = header_json["patch"];

        impl_->info.version =
            std::to_string(version_major) + "." + std::to_string(version_minor) + "." + std::to_string(version_patch);
        impl_->info.storage_type = "SQLite3";
        impl_->info.message_count = header_json["count"];
        impl_->info.total_duration = header_json["duration"];
        impl_->info.total_duration /= 1000U;
        impl_->info.time_accuracy = header_json["accuracy"];
        impl_->info.compression_type = header_json["compress"];
        impl_->info.process_name = header_json["process"];
        impl_->info.date_time = header_json["date"];

        if (header_json.contains("start_timestamp")) {
          impl_->info.start_timestamp = header_json["start_timestamp"];
        } else {
          impl_->info.start_timestamp = Helpers::convert_date_to_timestamp(impl_->info.date_time) / 1000'000;
        }

        if VUNLIKELY (impl_->info.start_timestamp < 0) {
          impl_->info.start_timestamp = 0;

          if (impl_->read_only) {
            VLOG_E("VDBReader: Invalid start_timestamp.");
          }
        }

        if (header_json.contains("tag")) {
          impl_->info.tag_name = header_json["tag"];
        } else {
          impl_->info.tag_name = "Empty";
        }

        if (header_json.contains("complete")) {
          impl_->info.has_completed = header_json["complete"];
        } else {
          impl_->info.has_completed = true;
        }

        if (header_json.contains("timezone")) {
          impl_->info.timezone = header_json["timezone"];
        } else {
          impl_->info.timezone = 480;
        }

        if (header_json.contains("split_by_size")) {
          impl_->info.split_by_size = header_json["split_by_size"];
        }

        if (header_json.contains("split_by_time")) {
          impl_->info.split_by_time = header_json["split_by_time"];
        }

        impl_->info.blank_duration = blank_duration;

        if (impl_->info.compression_type.empty() || impl_->info.compression_type == "None" ||
            impl_->info.compression_type == "NONE" || impl_->info.compression_type == "none") {
          impl_->enable_compress = false;
        } else {
          impl_->enable_compress = true;
        }

        if VUNLIKELY (impl_->info.time_accuracy != "MicroSecond") {
          VLOG_F("VDBReader: Database accuracy is not supported.");
          return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }

        url_ser_map().clear();
        url_schema_type_map().clear();
        impl_->info.url_metas.clear();
        impl_->raw_url_metas.clear();

        impl_->info.url_metas.reserve(urls_json.size());

        impl_->info.total_raw_size = 0;

        for (const auto& url_info : urls_json) {
          Info::UrlMeta url_meta;

          url_meta.valid = true;
          url_meta.index = url_info["index"];
          url_meta.url = url_info["url"];
          url_meta.url_type = url_info["type"];
          url_meta.ser_type = url_info["ser"];

          if (url_info.contains("encoding")) {
            url_meta.schema_type = SchemaData::resolve_type(
                SchemaData::convert_encoding(url_info["encoding"].get<std::string>()), url_meta.ser_type);
          } else {
            url_meta.schema_type = SchemaType::kUnknown;
          }

          url_meta.count = url_info["count"];
          url_meta.loss = url_info["loss"];

          if (url_info.contains("size")) {
            url_meta.size = url_info["size"];
          }

          if (url_info.contains("freq")) {
            url_meta.freq = url_info["freq"];
          }

          impl_->info.total_raw_size += url_meta.size;

          url_ser_map().emplace(url_meta.url, url_meta.ser_type);
          url_schema_type_map().emplace(url_meta.url, url_meta.schema_type);
          impl_->info.url_metas.emplace_back(std::move(url_meta));
        }

        std::sort(impl_->info.url_metas.begin(), impl_->info.url_metas.end());
        impl_->raw_url_metas = impl_->info.url_metas;
        rebuild_url_meta_lookup(impl_->info.url_metas);
      } catch (nlohmann::json::exception& e) {
        VLOG_F("VDBReader: JSON parse error, ", e.what(), ".");
        return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }
    } else {
      Impl::WrapperFile wrapper_file;
      wrapper_file.path = impl_->path;
      wrapper_file.index = 0;

      if VUNLIKELY (!to_open(wrapper_file)) {
        CLOG_F("VDBReader: Failed to prepare database [%s].", path.c_str());  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        return;                                                               // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      impl_->info.file_size = 0;

      std::error_code db_size_ec;
      std::uintmax_t file_size = std::filesystem::file_size(file_path, db_size_ec);

      if VUNLIKELY (db_size_ec) {
        CLOG_W("VDBReader: file_size failed for [%s]: %s.", path.c_str(),  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
               db_size_ec.message().c_str());                              // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        file_size = 0;                                                     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      impl_->info.file_size += file_size;

      wrapper_file.start_timestamp_ns = impl_->info.start_timestamp * 1000'000;
      wrapper_file.begin = impl_->info.blank_duration;
      wrapper_file.end = impl_->info.total_duration;

      if (impl_->total_start_timestamp_ns < 0) {
        impl_->total_start_timestamp_ns = wrapper_file.start_timestamp_ns;
      }

      if (!wrapper_file.has_completed) {
        impl_->total_has_completed = false;
      }

      impl_->file_list.emplace_back(std::move(wrapper_file));
    }
  } catch (std::filesystem::filesystem_error& e) {
    VLOG_F("VDBReader: Filesystem error, ", e.what(), ".");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return;                                                  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
#else
  (void)path;
  VLOG_F("VDBReader: The compile macro VLINK_ENABLE_SQLITE is not turned on.");
#endif
}

void VDBReader::close() {
#ifdef VLINK_ENABLE_SQLITE
  impl_->cursor_stmt.reset();

  for (auto& wrapper_file : impl_->file_list) {
    if (wrapper_file.stmt) {
      ::sqlite3_finalize(wrapper_file.stmt);
      wrapper_file.stmt = nullptr;
    }

    if (wrapper_file.db) {
      int ret = ::sqlite3_close_v2(wrapper_file.db);

      if VUNLIKELY (ret != SQLITE_OK) {
        CLOG_W("Failed to close database: %s.", ::sqlite3_errmsg(wrapper_file.db));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      wrapper_file.db = nullptr;
    }
  }

  impl_->file_list.clear();
#endif
}

int VDBReader::get_reset_index(const Config& config) {
#ifdef VLINK_ENABLE_SQLITE
  impl_->is_pending.store(true, std::memory_order_relaxed);

  int ret = 0;
  int start_index = -1;

  int64_t last_time = impl_->begin_time.load(std::memory_order_relaxed);

  impl_->update_sql_default_str = "SELECT elapsed, url, action, data FROM VLinkDatas ORDER BY rowid;";

  impl_->update_sql_time_str = "SELECT elapsed, url, action, data FROM VLinkDatas WHERE elapsed >= ";
  impl_->update_sql_time_str.append(std::to_string(impl_->begin_time.load(std::memory_order_relaxed) * 1000));
  impl_->update_sql_time_str.append(" ORDER BY rowid;");

  std::string* select_sql = nullptr;

  for (auto& wrapper_file : impl_->file_list) {
    if (wrapper_file.has_idx_elapsed && wrapper_file.has_idx_url) {
      if (config.filter_urls.empty()) {
        impl_->update_sql_default_str = "SELECT elapsed, url, action, data FROM VLinkDatas ORDER BY elapsed;";

        impl_->update_sql_time_str = "SELECT elapsed, url, action, data FROM VLinkDatas WHERE elapsed >= ";
        impl_->update_sql_time_str.append(std::to_string(impl_->begin_time.load(std::memory_order_relaxed) * 1000));
        impl_->update_sql_time_str.append(" ORDER BY elapsed;");
      } else {
        std::string id_list_str = " url IN (";
        bool id_appended = false;

        for (const auto& [url, id] : wrapper_file.url_to_id_map) {
          if (!match_playback_url_filter(url, config.filter_urls)) {
            continue;
          }

          id_list_str.append(std::to_string(id));
          id_list_str.append(",");
          id_appended = true;
        }

        if (id_appended) {
          id_list_str.pop_back();
          id_list_str.append(")");
        } else {
          id_list_str = " url IN (NULL)";
        }

        impl_->update_sql_default_str = "SELECT elapsed, url, action, data FROM VLinkDatas WHERE";
        impl_->update_sql_default_str.append(id_list_str);
        impl_->update_sql_default_str.append(wrapper_file.has_idx_elapsed
                                                 ? " ORDER BY elapsed;"
                                                 : " ORDER BY rowid;");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

        impl_->update_sql_time_str = "SELECT elapsed, url, action, data FROM VLinkDatas WHERE";
        impl_->update_sql_time_str.append(id_list_str);
        impl_->update_sql_time_str.append(" AND elapsed >= ");
        impl_->update_sql_time_str.append(std::to_string(impl_->begin_time.load(std::memory_order_relaxed) * 1000));
        impl_->update_sql_time_str.append(wrapper_file.has_idx_elapsed
                                              ? " ORDER BY elapsed;"
                                              : " ORDER BY rowid;");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }
    }

    if (start_index < 0 && impl_->begin_time.load(std::memory_order_relaxed) >= last_time &&
        impl_->begin_time.load(std::memory_order_relaxed) <= wrapper_file.end) {
      if (impl_->begin_time.load(std::memory_order_relaxed) > 0) {
        select_sql = &impl_->update_sql_time_str;
      } else {
        select_sql = &impl_->update_sql_default_str;
      }

      start_index = wrapper_file.index;
    } else {
      select_sql = &impl_->update_sql_default_str;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if VLIKELY (wrapper_file.stmt) {
      ::sqlite3_finalize(wrapper_file.stmt);
      wrapper_file.stmt = nullptr;
    }

    if VLIKELY (!select_sql) {
      VLOG_E("VDBReader: Failed to prepare select sql str.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      break;                                                   // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    ret = ::sqlite3_prepare_v2(wrapper_file.db, select_sql->c_str(), -1, &wrapper_file.stmt, nullptr);

    if VUNLIKELY (ret != SQLITE_OK) {
      // LCOV_EXCL_START GCOVR_EXCL_START
      CLOG_W("Failed to prepare datas table: %s.", ::sqlite3_errmsg(wrapper_file.db));

      start_index = -1;

      break;
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    }

    ::sqlite3_step(wrapper_file.stmt);
    ::sqlite3_reset(wrapper_file.stmt);

    last_time = wrapper_file.end;
  }

  impl_->is_pending.store(false, std::memory_order_relaxed);

  return start_index;
#else
  (void)config;
  return -1;
#endif
}

bool VDBReader::prepare_cursor_stmt(int file_index) {
#ifdef VLINK_ENABLE_SQLITE
  impl_->cursor_stmt.reset();

  if VUNLIKELY (file_index < 0 || file_index >= static_cast<int>(impl_->file_list.size())) {
    return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  auto& wrapper_file = impl_->file_list.at(file_index);

  if VUNLIKELY (!wrapper_file.db) {
    VLOG_W("VDBReader: Cursor target db is empty.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;                                     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  std::string where;

  if (!impl_->cursor_config.filter_urls.empty()) {
    std::string id_list = "url IN (";
    bool id_appended = false;

    for (const auto& [url, id] : wrapper_file.url_to_id_map) {
      if (!match_playback_url_filter(url, impl_->cursor_config.filter_urls)) {
        continue;
      }

      id_list.append(std::to_string(id));
      id_list.append(",");
      id_appended = true;
    }

    if (id_appended) {
      id_list.pop_back();
      id_list.append(")");
    } else {
      id_list = "url IN (NULL)";
    }

    where = std::move(id_list);
  }

  if (impl_->cursor_begin_us > 0) {
    if (!where.empty()) {
      where.append(" AND ");
    }

    where.append("elapsed >= ");
    where.append(std::to_string(impl_->cursor_begin_us));
  }

  std::string select_sql = "SELECT elapsed, url, action, data FROM VLinkDatas";

  if (!where.empty()) {
    select_sql.append(" WHERE ");
    select_sql.append(where);
  }

  const bool order_by_elapsed = wrapper_file.has_idx_elapsed;

  select_sql.append(order_by_elapsed ? " ORDER BY elapsed;" : " ORDER BY rowid;");

  ::sqlite3_stmt* stmt = nullptr;
  const int ret = ::sqlite3_prepare_v2(wrapper_file.db, select_sql.c_str(), -1, &stmt, nullptr);

  if VUNLIKELY (ret != SQLITE_OK) {
    CLOG_W("VDBReader: Failed to prepare cursor stmt: %s.",  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
           ::sqlite3_errmsg(wrapper_file.db));               // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;                                            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  impl_->cursor_stmt.reset(stmt);
  impl_->cursor_file_index = file_index;

  return true;
#else
  (void)file_index;
  return false;
#endif
}

bool VDBReader::do_open_cursor(const Config& config) {
#ifdef VLINK_ENABLE_SQLITE
  impl_->cursor_stmt.reset();
  impl_->cursor_config = config;
  impl_->cursor_begin_us = config.begin_time > 0 ? config.begin_time * 1000 : 0;
  impl_->cursor_end_us = config.end_time > 0 ? config.end_time * 1000 : 0;
  impl_->cursor_file_index = 0;

  if VUNLIKELY (impl_->file_list.empty()) {
    VLOG_W("VDBReader: Cursor cannot find any data.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;                                       // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  return prepare_cursor_stmt(0);
#else
  (void)config;
  return false;
#endif
}

bool VDBReader::do_read_next(Frame& out, bool& is_error) {
#ifdef VLINK_ENABLE_SQLITE
  is_error = false;

  while (true) {
    if VUNLIKELY (!impl_->cursor_stmt) {
      return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    const int step = ::sqlite3_step(impl_->cursor_stmt.get());

    if (step == SQLITE_ROW) {
      const int64_t timestamp = ::sqlite3_column_int64(impl_->cursor_stmt.get(), get_column(0));

      if (impl_->cursor_end_us > 0 && timestamp > impl_->cursor_end_us) {
        return false;
      }

      const int url_id = ::sqlite3_column_int(impl_->cursor_stmt.get(), get_column(1));
      auto& wrapper_file = impl_->file_list.at(impl_->cursor_file_index);
      auto iter = wrapper_file.id_to_url_map.find(url_id);

      if VUNLIKELY (iter == wrapper_file.id_to_url_map.end() || iter->second.empty()) {
        continue;
      }

      const auto& url = iter->second;

      std::string output_url;

      if VUNLIKELY (!convert_playback_url(url, output_url)) {
        continue;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      if (!impl_->cursor_config.filter_urls.empty() && impl_->cursor_config.filter_urls.count(output_url) == 0U) {
        continue;
      }

      std::string_view action_str;
      const auto* action_ptr =
          reinterpret_cast<const char*>(::sqlite3_column_text(impl_->cursor_stmt.get(), get_column(2)));

      if (action_ptr != nullptr) {
        action_str = std::string_view(
            action_ptr, static_cast<size_t>(::sqlite3_column_bytes(impl_->cursor_stmt.get(), get_column(2))));
      }

      const auto* data = static_cast<const uint8_t*>(::sqlite3_column_blob(impl_->cursor_stmt.get(), get_column(3)));
      const int size = ::sqlite3_column_bytes(impl_->cursor_stmt.get(), get_column(3));

      out.timestamp = timestamp;
      out.url = std::move(output_url);
      out.ser_type.clear();
      out.schema_type = SchemaType::kUnknown;
      out.action_type = convert_action(action_str);

      if VUNLIKELY (impl_->enable_compress && Bytes::is_compress_data(data, size)) {
        out.data = Bytes::uncompress_data(data, size, false);
      } else {
        out.data = Bytes::shallow_copy(data, size);
      }

      fill_frame_meta(out);

      return true;
    } else if (step == SQLITE_DONE) {
      if (impl_->cursor_file_index + 1 < static_cast<int>(impl_->file_list.size())) {
        if VUNLIKELY (!prepare_cursor_stmt(impl_->cursor_file_index + 1)) {
          is_error = true;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          return false;     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }

        continue;
      }

      return false;
    } else {
      // LCOV_EXCL_START GCOVR_EXCL_START
      CLOG_W("VDBReader: Cursor step failed: %s.", ::sqlite3_errmsg(::sqlite3_db_handle(impl_->cursor_stmt.get())));
      is_error = true;
      return false;
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    }
  }
#else
  (void)out;
  is_error = false;
  return false;
#endif
}

void VDBReader::read(const Config& config) {
#ifdef VLINK_ENABLE_SQLITE
  int loop_times = 0;

  reset_plugin();

  if (config.auto_pause) {
    impl_->pause_flag.store(true, std::memory_order_relaxed);
  }

  bool is_interrupted = false;

  do {
    bool is_end = false;

    // prepare
    int start_index = get_reset_index(config);

    if (impl_->ready_callback) {
      impl_->ready_callback();
    }

    if VUNLIKELY (start_index < 0 || start_index > static_cast<int>(impl_->file_list.size()) - 1) {
      // LCOV_EXCL_START GCOVR_EXCL_START
      VLOG_W("VDBReader: Cannot find any data for play.");

      update_status(kStopped);

      if (config.auto_quit) {
        quit();
      }

      return;
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    }

    {
      std::lock_guard time_lock(impl_->time_mtx);
      impl_->pause_elapsed.store(0, std::memory_order_relaxed);
      impl_->offset_elapsed.store(0, std::memory_order_relaxed);
      impl_->real_elapsed.store(impl_->begin_time.load(std::memory_order_relaxed) * 1000U, std::memory_order_relaxed);

      impl_->elapsed_timer.restart();
      impl_->pause_elapsed_timer.restart();
      impl_->offset_timer.restart();
      impl_->real_timer.restart();
    }

    if (impl_->stop_flag.load(std::memory_order_relaxed)) {
      is_interrupted = true;
      update_status(kStopped);
      break;
    } else if (impl_->jump_flag.load(std::memory_order_relaxed)) {
      break;
    } else if (impl_->pause_flag.load(std::memory_order_relaxed)) {
      impl_->pause_elapsed_timer.restart();
      update_status(kPaused);
      do_pause();
      {
        std::lock_guard time_lock(impl_->time_mtx);
        impl_->pause_elapsed.store(0, std::memory_order_relaxed);
        impl_->offset_elapsed.store(0, std::memory_order_relaxed);
        impl_->real_elapsed.store(impl_->begin_time.load(std::memory_order_relaxed) * 1000U, std::memory_order_relaxed);

        impl_->elapsed_timer.restart();
        impl_->pause_elapsed_timer.restart();
        impl_->offset_timer.restart();
        impl_->real_timer.restart();
      }

    } else {
      update_status(kPlaying);
    }

    int64_t elapsed = 0;
    int64_t timestamp = 0;
    int64_t last_timestamp = 0;
    int url_id = -1;
    const uint8_t* data = nullptr;
    int size = 0;
    Bytes decompressed_data;

    // process files
    for (int index = start_index; index < static_cast<int>(impl_->file_list.size()); ++index) {
      impl_->split_index.store(index, std::memory_order_relaxed);

      auto& wrapper_file = impl_->file_list.at(impl_->split_index.load(std::memory_order_relaxed));

      if VUNLIKELY (!wrapper_file.db || !wrapper_file.stmt) {
        VLOG_W("VDBReader: Target db or stmt is empty.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        return;                                            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      // process datas
      for ([[maybe_unused]] int row = 0; ::sqlite3_step(wrapper_file.stmt) == SQLITE_ROW; ++row) {
        timestamp = ::sqlite3_column_int64(wrapper_file.stmt, get_column(0));

        impl_->offset_timer.restart();

        if VUNLIKELY (last_timestamp > timestamp + 10'000U) {
          VLOG_W("VDBReader: The database timestamp is incorrect.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }

        last_timestamp = timestamp;

        if (timestamp < impl_->begin_time.load(std::memory_order_relaxed) * 1000U) {
          continue;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }

        if (config.end_time > 0 && timestamp > config.end_time * 1000U) {
          timestamp = config.end_time * 1000U;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          is_end = true;                        // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }

        url_id = ::sqlite3_column_int(wrapper_file.stmt, get_column(1));

        auto iter = wrapper_file.id_to_url_map.find(url_id);

        if VUNLIKELY (iter == wrapper_file.id_to_url_map.end()) {
          continue;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }

        const auto& url = iter->second;

        if VUNLIKELY (url.empty()) {
          continue;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }

        if VUNLIKELY (!match_playback_url_filter(url, config.filter_urls)) {
          continue;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }

        if (impl_->stop_flag.load(std::memory_order_relaxed) || impl_->jump_flag.load(std::memory_order_relaxed) ||
            is_ready_to_quit()) {
          is_interrupted = true;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          break;
        }

        std::string_view action_str;
        const auto* action_ptr = reinterpret_cast<const char*>(::sqlite3_column_text(wrapper_file.stmt, get_column(2)));

        if (action_ptr != nullptr) {
          action_str = std::string_view(action_ptr,
                                        static_cast<size_t>(::sqlite3_column_bytes(wrapper_file.stmt, get_column(2))));
        }

        data = static_cast<const uint8_t*>(::sqlite3_column_blob(wrapper_file.stmt, get_column(3)));
        size = ::sqlite3_column_bytes(wrapper_file.stmt, get_column(3));

        if VUNLIKELY (impl_->enable_compress && Bytes::is_compress_data(data, size)) {
          decompressed_data = Bytes::uncompress_data(data, size, false);

          data = decompressed_data.data();
          size = decompressed_data.size();
        }

        elapsed =
            (timestamp / impl_->rate.load(std::memory_order_relaxed)) -
            (impl_->elapsed_timer.get() - impl_->pause_elapsed.load(std::memory_order_relaxed)) -
            (impl_->begin_time.load(std::memory_order_relaxed) * 1000U / impl_->rate.load(std::memory_order_relaxed));

        impl_->extra_elapsed.store(impl_->offset_timer.get(), std::memory_order_relaxed);

        {
          std::unique_lock lock(impl_->mtx);

          if (config.force_delay > 0) {
            impl_->cv.wait_for(lock, std::chrono::milliseconds(config.force_delay), [this]() -> bool {
              return impl_->stop_flag.load(std::memory_order_relaxed) ||
                     impl_->pause_next_flag.load(std::memory_order_relaxed) ||
                     impl_->jump_flag.load(std::memory_order_relaxed) || is_ready_to_quit();
            });
          } else if (config.force_delay < 0 && elapsed > 0) {
            impl_->offset_timer.restart();

            impl_->cv.wait_for(lock, std::chrono::microseconds(elapsed), [this]() -> bool {
              return impl_->stop_flag.load(std::memory_order_relaxed) ||
                     impl_->pause_next_flag.load(std::memory_order_relaxed) ||
                     impl_->jump_flag.load(std::memory_order_relaxed) ||
                     impl_->pause_flag.load(std::memory_order_relaxed) || is_ready_to_quit();
            });

            if VUNLIKELY (impl_->pause_flag.load(std::memory_order_relaxed)) {
              impl_->offset_elapsed.store(elapsed - impl_->offset_timer.get(),  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                          std::memory_order_relaxed);           // LCOV_EXCL_LINE GCOVR_EXCL_LINE
            }
          }
        }

        if (impl_->stop_flag.load(std::memory_order_relaxed) || impl_->jump_flag.load(std::memory_order_relaxed) ||
            is_ready_to_quit()) {
          is_interrupted = true;
          break;
        } else if (impl_->pause_flag.load(std::memory_order_relaxed)) {
          do_pause();
          impl_->pause_next_flag.store(false, std::memory_order_relaxed);

          if (impl_->stop_flag.load(std::memory_order_relaxed) || impl_->jump_flag.load(std::memory_order_relaxed) ||
              is_ready_to_quit()) {
            is_interrupted = true;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
            break;                  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          }
        }

        {
          std::lock_guard time_lock(impl_->time_mtx);
          impl_->real_timer.restart();
          impl_->real_elapsed.store(timestamp, std::memory_order_relaxed);
        }

        if (is_end) {
          break;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }

        Frame frame;
        frame.timestamp = timestamp;
        frame.url = url;
        frame.action_type = convert_action(action_str);
        frame.data = Bytes::shallow_copy(data, size);

        BagReader::process_output(frame);
      }

      if (is_interrupted || is_end) {
        break;
      }
    }

    if (is_interrupted) {
      if (impl_->stop_flag.load(std::memory_order_relaxed)) {
        update_status(kStopped);
      }

      break;
    }

    if (!impl_->jump_flag.load(std::memory_order_relaxed)) {
      update_status(kStopped);

      if (config.skip_blank) {
        impl_->begin_time.store(std::max(config.begin_time, impl_->info.blank_duration), std::memory_order_relaxed);
      } else {
        impl_->begin_time.store(config.begin_time, std::memory_order_relaxed);
      }
    }

    if (impl_->stop_flag.load(std::memory_order_relaxed) || impl_->jump_flag.load(std::memory_order_relaxed) ||
        is_ready_to_quit()) {
      is_interrupted = true;
      break;
    }

    flush_plugin();
  } while (impl_->times.load(std::memory_order_relaxed) <= 0 ||
           (impl_->times.load(std::memory_order_relaxed) > 0 &&
            ++loop_times < impl_->times.load(std::memory_order_relaxed)));

  if (impl_->stop_flag.load(std::memory_order_relaxed)) {
    is_interrupted = true;
  }

  if (!impl_->jump_flag.load(std::memory_order_relaxed) && impl_->finish_callback) {
    impl_->finish_callback(is_interrupted);
  }

  if (!impl_->jump_flag.load(std::memory_order_relaxed) && config.auto_quit) {
    quit();
  }
#else
  (void)config;
#endif
}

}  // namespace vlink
