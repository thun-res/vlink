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

#include "./extension/vcap_reader.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <filesystem>
#include <fstream>
#include <limits>
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

// mcap
#include "./private/mcap_import.h"

namespace vlink {

[[maybe_unused]] static constexpr size_t kMaxTaskSize = 50000U;

// VCAPReader::Impl
struct VCAPReader::Impl final {  // NOLINT(clang-analyzer-optin.performance.Padding)
  std::atomic<BagReader::Status> status{VCAPReader::kStopped};
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

  int64_t total_start_timestamp_ns{-1};
  bool total_has_completed{false};

  // mcap

  // WrapperFile
  struct WrapperFile final {
    std::string path;
    std::unique_ptr<mcap::McapReader> reader;
    std::unique_ptr<mcap::LinearMessageView> msg_view;
    std::optional<mcap::LinearMessageView::Iterator> msg_view_begin;
    std::optional<mcap::LinearMessageView::Iterator> msg_view_end;
    int index{0};
    int64_t start_timestamp_ns{0};
    int64_t begin{0};
    int64_t end{0};
    std::unordered_map<std::string, int> url_to_id_map;
    std::unordered_map<int, std::string> id_to_url_map;
    std::unordered_map<int, ActionType> channel_action_map;
    bool has_idx_elapsed{false};
    bool has_idx_url{false};
    bool has_schema{false};
    bool has_completed{false};
    bool is_channel_broken{false};

    WrapperFile() {
      url_to_id_map.reserve(128);
      id_to_url_map.reserve(128);
      channel_action_map.reserve(128);
    }
  };

  std::vector<WrapperFile> file_list;

  std::unique_ptr<mcap::LinearMessageView> cursor_msg_view;
  std::optional<mcap::LinearMessageView::Iterator> cursor_iter;
  std::optional<mcap::LinearMessageView::Iterator> cursor_iter_end;
  int cursor_file_index{0};
  int64_t cursor_begin_us{0};
  int64_t cursor_end_us{0};
  bool cursor_need_advance{false};
  bool cursor_read_error{false};
  BagReader::Config cursor_config;
};

// VCAPReader
VCAPReader::VCAPReader(const std::string& path, bool read_only, bool try_to_fix)
    : BagReader(path, read_only, try_to_fix), impl_{std::make_unique<Impl>()} {
  set_name("VCAPReader");

  url_ser_map().reserve(128);
  url_schema_type_map().reserve(128);

  impl_->read_only = read_only;
  impl_->try_to_fix = try_to_fix;

  open(path);
}

VCAPReader::~VCAPReader() {
  if (!impl_->stop_flag.load(std::memory_order_relaxed)) {
    do_stop();
  }

  quit(true);

  impl_->cv.notify_one();

  wait_for_quit();

  detach_plugin();

  close();
}

void VCAPReader::bind_bag_interface(const std::shared_ptr<BagPluginInterface>& bag_interface) {
  BagReader::bind_bag_interface(bag_interface);
  impl_->info.url_metas = impl_->raw_url_metas;
  process_url_metas(impl_->info.url_metas);
  rebuild_url_meta_lookup(impl_->info.url_metas);
}

void VCAPReader::register_status_callback(StatusCallback&& status_callback) {
  impl_->status_callback = std::move(status_callback);
}

void VCAPReader::register_ready_callback(ReadyCallback&& ready_callback) {
  impl_->ready_callback = std::move(ready_callback);
}

void VCAPReader::register_finish_callback(FinishCallback&& finish_callback) {
  impl_->finish_callback = std::move(finish_callback);
}

void VCAPReader::register_output_callback(OutputCallback&& output_callback) {
  BagReader::register_output_callback(std::move(output_callback));
}

void VCAPReader::play(const Config& config) {
  if VUNLIKELY (is_busy()) {
    VLOG_W("VCAPReader: Is busy.");
    // return;
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

  Config config_snapshot;

  {
    std::unique_lock lock(impl_->config_mtx);
    impl_->config = config;
    config_snapshot = impl_->config;
  }

  post_task([this, config_snapshot]() { read(config_snapshot); });
}

void VCAPReader::stop() { do_stop(); }

void VCAPReader::pause() {
  {
    std::unique_lock lock(impl_->mtx);
    impl_->pause_flag.store(true, std::memory_order_relaxed);
  }

  impl_->cv.notify_one();
}

void VCAPReader::resume() {
  {
    std::unique_lock lock(impl_->mtx);
    impl_->pause_flag.store(false, std::memory_order_relaxed);
  }

  impl_->cv.notify_one();
}

void VCAPReader::pause_to_next() {
  {
    std::unique_lock lock(impl_->mtx);

    if (!impl_->pause_flag.load(std::memory_order_relaxed)) {
      return;
    }

    impl_->pause_next_flag.store(true, std::memory_order_relaxed);
  }

  impl_->cv.notify_one();
}

void VCAPReader::jump(int64_t begin_time, double rate, int times, bool force_to_play) {
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

  Config config_snapshot;

  {
    std::unique_lock lock(impl_->config_mtx);
    config_snapshot = impl_->config;
  }

  post_task([this, config_snapshot]() { read(config_snapshot); });
}

std::future<bool> VCAPReader::check() {
  if VUNLIKELY (is_busy()) {
    VLOG_W("VCAPReader: Is busy.");
    // return std::future<bool>();
  }

  return invoke_task([this]() {
    if (!impl_->total_has_completed) {
      VLOG_W("VCAPReader: Incomplete data detected.");
      return false;
    }

    mcap::Status status;

    // LCOV_EXCL_START GCOVR_EXCL_START
    auto status_function = [](const mcap::Status& status) {
      if (!status.ok()) {
        CLOG_W("VCAPReader: Failed to check summary, error = %s.", status.message.c_str());
      }
    };
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP

    for (auto& wrapper_file : impl_->file_list) {
      if VUNLIKELY (!wrapper_file.reader) {
        continue;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      status = wrapper_file.reader->readSummary(mcap::ReadSummaryMethod::ForceScan, status_function);

      if VUNLIKELY (!status.ok()) {
        CLOG_W("VCAPReader: Failed to check whole summary, error = %s.",  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
               status.message.c_str());                                   // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        return false;                                                     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }
    }

    bool is_ok = true;

    if VUNLIKELY (impl_->info.total_duration < impl_->info.blank_duration) {
      CLOG_W("VCAPReader: Invalid duration, blank=%" PRId64 " total=%" PRId64 ".",
             static_cast<int64_t>(impl_->info.blank_duration), static_cast<int64_t>(impl_->info.total_duration));
      is_ok = false;
    }

    if VUNLIKELY (impl_->info.message_count > 0 && impl_->info.url_metas.empty()) {
      CLOG_W("VCAPReader: Message count is %" PRId64 " but url meta list is empty.",
             static_cast<int64_t>(impl_->info.message_count));
      is_ok = false;
    }

    size_t total_count = 0;
    size_t total_raw_size = 0;

    for (const auto& url_meta : impl_->info.url_metas) {
      total_count += url_meta.count;
      total_raw_size += url_meta.size;

      if VUNLIKELY (!url_meta.valid) {
        CLOG_W("VCAPReader: Invalid url meta detected at index=%d.", url_meta.index);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        is_ok = false;                                                                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      if VUNLIKELY (url_meta.url.empty()) {
        CLOG_W("VCAPReader: Empty url detected at index=%d.", url_meta.index);
        is_ok = false;
      }

      if VUNLIKELY (url_meta.url_type.empty()) {
        CLOG_W("VCAPReader: Empty url_type detected for url=%s.", url_meta.url.c_str());
        is_ok = false;
      }

      if VUNLIKELY (url_meta.count > 0 && url_meta.ser_type.empty()) {
        CLOG_W("VCAPReader: Empty ser_type detected for url=%s.", url_meta.url.c_str());
        is_ok = false;
      }

      if VUNLIKELY (!SchemaData::is_valid_type(url_meta.schema_type)) {
        CLOG_W("VCAPReader: Invalid schema_type=%d detected for url=%s.",  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
               static_cast<int>(url_meta.schema_type),                     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
               url_meta.url.c_str());
        is_ok = false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      auto inferred_schema_type = SchemaData::infer_ser_type(url_meta.ser_type);

      if VUNLIKELY (url_meta.schema_type == SchemaType::kUnknown && inferred_schema_type != SchemaType::kUnknown) {
        const auto schema_label = SchemaData::convert_type(inferred_schema_type);
        CLOG_W("VCAPReader: Missing schema_type for url=%s, inferred=%.*s.", url_meta.url.c_str(),
               static_cast<int>(schema_label.size()), schema_label.data());
        is_ok = false;
      }

      if VUNLIKELY (url_meta.loss < 0.0 || url_meta.loss > 1.0) {
        CLOG_W("VCAPReader: Invalid loss=%f detected for url=%s.", url_meta.loss, url_meta.url.c_str());
        is_ok = false;
      }

      if VUNLIKELY (url_meta.freq < 0.0) {
        CLOG_W("VCAPReader: Invalid freq=%f detected for url=%s.", url_meta.freq, url_meta.url.c_str());
        is_ok = false;
      }
    }

    if ((!impl_->info.url_metas.empty() || impl_->info.message_count != 0) &&
        total_count != static_cast<size_t>(impl_->info.message_count)) {
      CLOG_W("VCAPReader: Message count mismatch, header=%" PRId64 " metas=%zu.",
             static_cast<int64_t>(impl_->info.message_count), total_count);
      is_ok = false;
    }

    if ((!impl_->info.url_metas.empty() || impl_->info.total_raw_size != 0) &&
        total_raw_size != static_cast<size_t>(impl_->info.total_raw_size)) {
      CLOG_W("VCAPReader: Raw size mismatch, header=%" PRId64 " metas=%zu.",  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
             static_cast<int64_t>(impl_->info.total_raw_size), total_raw_size);
      is_ok = false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    for (const auto& schema_data : detect_schema()) {
      if VUNLIKELY (schema_data.name.empty()) {
        CLOG_W("VCAPReader: Empty schema name detected.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        is_ok = false;                                      // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      if VUNLIKELY (schema_data.encoding.empty()) {
        CLOG_W("VCAPReader: Empty schema encoding detected for name=%s.",  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
               schema_data.name.c_str());                                  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        is_ok = false;                                                     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      if VUNLIKELY (!SchemaData::is_valid_type(schema_data.schema_type) ||
                    schema_data.schema_type == SchemaType::kUnknown) {
        CLOG_W("VCAPReader: Invalid schema_type=%d detected for schema=%s.", static_cast<int>(schema_data.schema_type),
               schema_data.name.c_str());
        is_ok = false;
      }
    }

    return is_ok;
  });
}

std::future<bool> VCAPReader::reindex() {
  if VUNLIKELY (is_busy()) {
    VLOG_W("VCAPReader: Is busy.");
    // return std::future<bool>();
  }

  return invoke_task([]() {
    VLOG_W("VCAPReader: Reindex is not supported for vcap.");

    return false;
  });
}

std::future<bool> VCAPReader::fix(bool rebuild) {
  if VUNLIKELY (is_busy()) {
    VLOG_W("VCAPReader: Is busy.");
    // return std::future<bool>();
  }

  return invoke_task([rebuild]() {
    (void)rebuild;

    VLOG_W("VCAPReader: Fix is not supported for vcap.");

    return false;
  });
}

void VCAPReader::tag(const std::string& tag_name) {
  if VUNLIKELY (is_busy()) {
    VLOG_W("VCAPReader: Is busy.");
    // return;
  }

  post_task([this, tag_name]() {
    try {
#ifdef _WIN32
      std::filesystem::path file_path(Helpers::string_to_wstring(impl_->path));
      std::string suffix = Helpers::path_to_string(file_path.extension());
#else
      std::filesystem::path file_path(impl_->path);
      std::string suffix = file_path.extension().string();
#endif

      std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return std::tolower(c); });

      if (suffix == ".vcapx") {
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
          VLOG_W("VCAPReader: JSON parse error, ", e.what(), ".");
        }
      } else {
        VLOG_W("VCAPReader: Tag is not supported for single vcap.");
      }
    } catch (std::filesystem::filesystem_error& e) {
      VLOG_F("VCAPReader: Filesystem error, ", e.what(), ".");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      return;                                                   // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  });
}

int64_t VCAPReader::get_timestamp() const {
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
            ((impl_->real_timer.get() - impl_->pause_elapsed_timer.get() -
              impl_->extra_elapsed.load(std::memory_order_relaxed)) *
             impl_->rate.load(std::memory_order_relaxed))) /
           1000U;
  } else {
    return 0;
  }
}

int64_t VCAPReader::get_real_timestamp() const {
  if (impl_->status.load(std::memory_order_relaxed) == kPlaying ||
      impl_->status.load(std::memory_order_relaxed) == kPaused) {
    return impl_->real_elapsed.load(std::memory_order_relaxed) / 1000U;
  } else {
    return 0;
  }
}

BagReader::Status VCAPReader::get_status() const { return impl_->status.load(std::memory_order_relaxed); }

const BagReader::Info& VCAPReader::get_info() const { return impl_->info; }

std::vector<SchemaData> VCAPReader::detect_schema() {
  std::vector<SchemaData> schema_list;
  std::unordered_map<std::string, size_t> schema_index_map;

  if (!impl_->info.has_schema) {
    return schema_list;
  }

  schema_index_map.reserve(impl_->info.url_metas.size());

  for (auto& wrapper_file : impl_->file_list) {
    for (const auto& [schema_id, schema_ptr] : wrapper_file.reader->schemas()) {
      (void)schema_id;

      SchemaData schema;
      schema.name = schema_ptr->name;
      schema.encoding = schema_ptr->encoding;
      schema.schema_type = SchemaData::resolve_type(SchemaType::kUnknown, schema.name, schema.encoding);

      if (!schema.name.empty() && !schema_ptr->data.empty()) {
        std::string schema_key = schema.name;
        schema_key.push_back('\x1F');
        schema_key.append(SchemaData::convert_type(schema.schema_type));
        auto schema_index_iter = schema_index_map.find(schema_key);

        if (schema_index_iter == schema_index_map.end()) {
          schema.data =
              Bytes::deep_copy(reinterpret_cast<const uint8_t*>(schema_ptr->data.data()), schema_ptr->data.size());
          schema_index_map.emplace(schema_key, schema_list.size());
          schema_list.emplace_back(std::move(schema));
        } else {
          auto& current_schema = schema_list[schema_index_iter->second];

          if (current_schema.encoding.empty() && !schema.encoding.empty()) {
            current_schema.encoding = schema.encoding;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          }

          if (current_schema.data.empty()) {
            current_schema.data = Bytes::deep_copy(
                reinterpret_cast<const uint8_t*>(schema_ptr->data.data()),  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                schema_ptr->data.size());                                   // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          }
        }
      }
    }
  }

  return schema_list;
}

bool VCAPReader::is_split_mode() const { return impl_->info.split_count > 0; }

int VCAPReader::get_split_index() const { return impl_->split_index.load(std::memory_order_relaxed); }

bool VCAPReader::is_jumping() const { return impl_->jump_flag.load(std::memory_order_relaxed); }

size_t VCAPReader::get_max_task_count() const { return kMaxTaskSize; }

void VCAPReader::on_begin() { MessageLoop::on_begin(); }

void VCAPReader::on_end() { MessageLoop::on_end(); }

void VCAPReader::update_status(Status status) {
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

void VCAPReader::do_stop() {
  {
    std::unique_lock lock(impl_->mtx);
    impl_->stop_flag.store(true, std::memory_order_relaxed);
    impl_->pause_flag.store(false, std::memory_order_relaxed);
    impl_->pause_next_flag.store(false, std::memory_order_relaxed);
    impl_->jump_flag.store(false, std::memory_order_relaxed);
  }

  impl_->cv.notify_one();
}

void VCAPReader::do_pause() {
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
        impl_->real_elapsed.fetch_add(                                        // LCOV_EXCL_LINE GCOVR_EXCL_LINE
            (impl_->offset_timer.get() - impl_->pause_elapsed_timer.get()) *  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                impl_->rate.load(std::memory_order_relaxed),                  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
            std::memory_order_relaxed);
      }
    }

    update_status(kPlaying);

    if (impl_->pause_next_flag.load(std::memory_order_relaxed)) {
      impl_->pause_elapsed.fetch_sub(impl_->offset_elapsed.load(std::memory_order_relaxed), std::memory_order_relaxed);
      break;
      // LCOV_EXCL_START GCOVR_EXCL_START
    } else if (impl_->offset_elapsed.load(std::memory_order_relaxed) > 0) {
      impl_->offset_timer.restart();

      impl_->cv.wait_for(lock, std::chrono::microseconds(impl_->offset_elapsed.load(std::memory_order_relaxed)),
                         [this]() -> bool {
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

bool VCAPReader::prepare_file(void* file) {
  auto* wrapper_file = static_cast<Impl::WrapperFile*>(file);

  wrapper_file->has_completed = true;

  auto& reader = wrapper_file->reader;

  if VUNLIKELY (!reader) {
    wrapper_file->has_completed = false;                                             // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    CLOG_F("VCAPReader: Mcap [%s] reader is nullptr.", wrapper_file->path.c_str());  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;                                                                    // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if VUNLIKELY (!reader->header()) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    wrapper_file->has_completed = false;
    CLOG_F("VCAPReader: Mcap [%s] reader header is nullptr.", wrapper_file->path.c_str());
    return false;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  if VUNLIKELY (reader->header()->profile != "vlink") {
    // LCOV_EXCL_START GCOVR_EXCL_START
    wrapper_file->has_completed = false;
    CLOG_F("VCAPReader: Mcap [%s] profile is %s, not valid.", wrapper_file->path.c_str(),
           reader->header()->profile.c_str());
    return false;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  mcap::Status status;

  status = reader->readSummary(mcap::ReadSummaryMethod::NoFallbackScan);

  if VUNLIKELY (!status.ok()) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    wrapper_file->has_completed = false;

    if (impl_->try_to_fix) {
      CLOG_E("VCAPReader: Failed to read summary, error = %s. Trying to fix.", status.message.c_str());
      status = reader->readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);

      if VUNLIKELY (!status.ok()) {
        CLOG_F("VCAPReader: Failed to read summary, error = %s.", status.message.c_str());
        return false;
        // LCOV_EXCL_STOP GCOVR_EXCL_STOP
      }

    } else {
      CLOG_F("VCAPReader: Failed to read summary, error = %s.",  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
             status.message.c_str());                            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      return false;                                              // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
  }

  const auto& meta_index = reader->metadataIndexes();
  const auto& statistics = reader->statistics();

  if (!statistics.has_value()) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    wrapper_file->has_completed = false;
    CLOG_F("VCAPReader: Mcap [%s] cannot find statistics.", wrapper_file->path.c_str());
    return false;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  for (const auto& [schema_id, schema_ptr] : reader->schemas()) {
    (void)schema_id;

    if (schema_ptr && !schema_ptr->data.empty()) {
      wrapper_file->has_schema = true;
      impl_->info.has_schema = true;
      break;
    }
  }

  // read header
  {
    auto header_iter = meta_index.find("VLinkHeader");

    if VUNLIKELY (header_iter == meta_index.end()) {
      // LCOV_EXCL_START GCOVR_EXCL_START
      wrapper_file->has_completed = false;
      CLOG_F("VCAPReader: Mcap [%s] cannot find header.", wrapper_file->path.c_str());
      return false;
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    }

    const auto& header_index = header_iter->second;

    mcap::Record header_record;

    status = mcap::McapReader::ReadRecord(*reader->dataSource(), header_index.offset, &header_record);

    if VUNLIKELY (!status.ok()) {
      // LCOV_EXCL_START GCOVR_EXCL_START
      wrapper_file->has_completed = false;
      CLOG_F("VCAPReader: Failed to read header record for index, error = %s.", status.message.c_str());
      return false;
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    }

    mcap::Metadata header_meta_data;

    status = mcap::McapReader::ParseMetadata(header_record, &header_meta_data);

    if VUNLIKELY (!status.ok()) {
      // LCOV_EXCL_START GCOVR_EXCL_START
      wrapper_file->has_completed = false;
      CLOG_F("VCAPReader: Failed to parse header meta data, error = %s.", status.message.c_str());
      return false;
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    }

    auto& tag_str = header_meta_data.metadata["tag"];
    auto& version_str = header_meta_data.metadata["version"];
    auto& compress_str = header_meta_data.metadata["compress"];
    auto& process_str = header_meta_data.metadata["process"];
    auto& date_str = header_meta_data.metadata["date"];
    auto& start_timestamp_str = header_meta_data.metadata["start_timestamp"];
    auto& timezone_str = header_meta_data.metadata["timezone"];

    if VUNLIKELY (version_str.empty()) {
      // LCOV_EXCL_START GCOVR_EXCL_START
      wrapper_file->has_completed = false;

      if (impl_->read_only) {
        CLOG_E("VCAPReader: Mcap [%s] cannot find version in header.", wrapper_file->path.c_str());
      }

      version_str = "0.0.0";
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    } else {
      auto version = Version::from_string(version_str);

      if VUNLIKELY (!version.is_valid()) {
        wrapper_file->has_completed = false;

        if (impl_->read_only) {
          CLOG_E("VCAPReader: Mcap [%s] header version is invalid.",  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                 wrapper_file->path.c_str());                         // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }
      } else {
        if VUNLIKELY (version.major != VLINK_VERSION_MAJOR) {
          wrapper_file->has_completed = false;
          VLOG_F("VCAPReader: Mcap version is incompatible.");
          return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }
      }
    }

    if VUNLIKELY (compress_str.empty()) {
      // LCOV_EXCL_START GCOVR_EXCL_START
      wrapper_file->has_completed = false;

      if (impl_->read_only) {
        CLOG_E("VCAPReader: Mcap [%s] cannot find compress in header.", wrapper_file->path.c_str());
      }

      compress_str = "Unknown";
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    }

    if VUNLIKELY (process_str.empty()) {
      // LCOV_EXCL_START GCOVR_EXCL_START
      wrapper_file->has_completed = false;

      if (impl_->read_only) {
        CLOG_E("VCAPReader: Mcap [%s] cannot find process in header.", wrapper_file->path.c_str());
      }

      process_str = "Unknown";
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    }

    if VUNLIKELY (date_str.empty()) {
      // LCOV_EXCL_START GCOVR_EXCL_START
      wrapper_file->has_completed = false;

      if (impl_->read_only) {
        CLOG_E("VCAPReader: Mcap [%s] cannot find date in header.", wrapper_file->path.c_str());
      }

      date_str = "Unknown";
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    }

    if VUNLIKELY (timezone_str.empty()) {
      // LCOV_EXCL_START GCOVR_EXCL_START
      wrapper_file->has_completed = false;

      if (impl_->read_only) {
        CLOG_E("VCAPReader: Mcap [%s] cannot find timezone in header.", wrapper_file->path.c_str());
        // LCOV_EXCL_STOP GCOVR_EXCL_STOP
      }
    }

    if (tag_str.empty()) {
      tag_str = "Empty";  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    impl_->info.tag_name = tag_str;
    impl_->info.version = version_str;
    impl_->info.storage_type = "vcap";
    impl_->info.message_count = statistics->messageCount;
    impl_->info.time_accuracy = "MicroSecond";
    impl_->info.compression_type = compress_str;
    impl_->info.process_name = process_str;
    impl_->info.date_time = date_str;

    try {
      impl_->info.start_timestamp = std::stoll(start_timestamp_str);
    } catch (std::exception&) {
      impl_->info.start_timestamp = Helpers::convert_date_to_timestamp(impl_->info.date_time) / 1000'000;
    }

    if VUNLIKELY (impl_->info.start_timestamp < 0) {
      impl_->info.start_timestamp = 0;

      if (impl_->read_only) {
        VLOG_E("VCAPReader: Invalid start_timestamp_ns.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }
    }

    int64_t timestamp_diff = 0;

    timestamp_diff = static_cast<int64_t>(statistics->messageStartTime / 1000'000) - impl_->info.start_timestamp;

    if (timestamp_diff < 0) {
      timestamp_diff = 0;
      impl_->info.start_timestamp = static_cast<int64_t>(statistics->messageStartTime / 1000'000);
    }

    impl_->info.blank_duration = timestamp_diff;

    timestamp_diff = static_cast<int64_t>(statistics->messageEndTime / 1000'000) - impl_->info.start_timestamp;

    if (timestamp_diff < 0) {
      timestamp_diff = 0;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    impl_->info.total_duration = timestamp_diff;

    try {
      impl_->info.timezone = std::stoi(timezone_str);
    } catch (std::exception&) {  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      impl_->info.timezone = 0;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  // read channel
  {
    int channel = 0;
    std::string channel_str;
    mcap::Record channel_record;

    url_ser_map().clear();
    url_schema_type_map().clear();
    impl_->info.url_metas.clear();
    impl_->raw_url_metas.clear();

    impl_->info.total_raw_size = 0;

    for (const auto& [name, index] : meta_index) {
      if (!Helpers::has_startwith(name, "VLinkChannel_")) {
        continue;
      }

      channel_str = name;
      Helpers::replace_string(channel_str, "VLinkChannel_", "");

      try {
        channel = std::stoi(channel_str);
      } catch (std::exception&) {  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        continue;                  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

      auto channel_count_iter = statistics->channelMessageCounts.find(channel);

      if VUNLIKELY (channel_count_iter == statistics->channelMessageCounts.end()) {
        // LCOV_EXCL_START GCOVR_EXCL_START
        wrapper_file->has_completed = false;

        if (impl_->read_only) {
          CLOG_E("VCAPReader: Mcap [%s] cannot read statistics in channel.", wrapper_file->path.c_str());
        }

        continue;
      }
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP

      auto channel_msg_count = channel_count_iter->second;

      const auto& channel_ptr = reader->channel(channel);

      if VUNLIKELY (!channel_ptr) {
        // LCOV_EXCL_START GCOVR_EXCL_START
        wrapper_file->has_completed = false;

        if (impl_->read_only) {
          CLOG_E("VCAPReader: Mcap [%s] cannot find ptr in channel.", wrapper_file->path.c_str());
        }

        continue;
      }
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP

      status = mcap::McapReader::ReadRecord(*reader->dataSource(), index.offset, &channel_record);

      if VUNLIKELY (!status.ok()) {
        // LCOV_EXCL_START GCOVR_EXCL_START
        wrapper_file->has_completed = false;

        if (impl_->read_only) {
          CLOG_E("VCAPReader: Failed to read channel record for index, error = %s.", status.message.c_str());
        }

        continue;
      }
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP

      mcap::Metadata channel_meta_data;

      status = mcap::McapReader::ParseMetadata(channel_record, &channel_meta_data);

      if VUNLIKELY (!status.ok()) {
        // LCOV_EXCL_START GCOVR_EXCL_START
        wrapper_file->has_completed = false;

        if (impl_->read_only) {
          CLOG_E("VCAPReader: Failed to parse channel meta data, error = %s.", status.message.c_str());
        }

        continue;
      }
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP

      Info::UrlMeta url_meta;

      auto& index_str = channel_meta_data.metadata["index"];
      auto& type_str = channel_meta_data.metadata["type"];
      auto& count_str = channel_meta_data.metadata["count"];
      auto& size_str = channel_meta_data.metadata["size"];
      auto& loss_str = channel_meta_data.metadata["loss"];
      auto& freq_str = channel_meta_data.metadata["freq"];
      auto ser_iter = channel_meta_data.metadata.find("ser");
      auto encoding_iter = channel_meta_data.metadata.find("encoding");
      auto action_iter = channel_meta_data.metadata.find("action");
      const auto& schema_ptr = reader->schema(channel_ptr->schemaId);

      int pindex = -1;

      try {
        pindex = std::stoi(index_str);
      } catch (std::exception&) {  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        pindex = -1;               // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

      if VUNLIKELY (pindex != channel - 1) {
        // LCOV_EXCL_START GCOVR_EXCL_START
        wrapper_file->has_completed = false;

        if (impl_->read_only) {
          CLOG_E("VCAPReader: Mcap [%s] channel index error.", wrapper_file->path.c_str());
        }

        continue;
      }
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP

      url_meta.valid = true;
      url_meta.index = pindex;
      url_meta.url = channel_ptr->topic;
      url_meta.url_type = type_str;
      url_meta.schema_type = SchemaType::kUnknown;

      if (ser_iter != channel_meta_data.metadata.end()) {
        url_meta.ser_type = ser_iter->second;
      } else if (schema_ptr) {                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        url_meta.ser_type = schema_ptr->name;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      if (encoding_iter != channel_meta_data.metadata.end()) {
        url_meta.schema_type =
            SchemaData::resolve_type(SchemaData::convert_encoding(encoding_iter->second), url_meta.ser_type,
                                     schema_ptr ? std::string_view(schema_ptr->encoding) : std::string_view{});
        // LCOV_EXCL_START GCOVR_EXCL_START
      } else if (schema_ptr) {
        url_meta.schema_type =
            SchemaData::resolve_type(SchemaData::convert_encoding(schema_ptr->encoding), url_meta.ser_type);
        // LCOV_EXCL_STOP GCOVR_EXCL_STOP
      }

      if (schema_ptr && !schema_ptr->data.empty()) {
        wrapper_file->has_schema = true;
        impl_->info.has_schema = true;
      }

      try {
        url_meta.count = static_cast<size_t>(std::stoull(count_str));
      } catch (std::exception&) {            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        url_meta.count = channel_msg_count;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

      try {
        url_meta.loss = std::stod(loss_str);
      } catch (std::exception&) {  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        url_meta.loss = 0;         // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

      try {
        url_meta.size = static_cast<size_t>(std::stoull(size_str));
      } catch (std::exception&) {  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        url_meta.size = 0;         // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

      try {
        url_meta.freq = std::stod(freq_str);
      } catch (std::exception&) {  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        url_meta.freq = 0;         // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

      impl_->info.total_raw_size += url_meta.size;

      wrapper_file->id_to_url_map.emplace(url_meta.index, url_meta.url);
      wrapper_file->url_to_id_map.emplace(url_meta.url, url_meta.index);

      url_ser_map().emplace(url_meta.url, url_meta.ser_type);
      url_schema_type_map().emplace(url_meta.url, url_meta.schema_type);

      if (action_iter != channel_meta_data.metadata.end()) {
        url_meta.action_type = convert_action(action_iter->second);
        wrapper_file->channel_action_map.emplace(channel, url_meta.action_type);
        // LCOV_EXCL_START GCOVR_EXCL_START
      } else if (auto channel_action_iter = channel_ptr->metadata.find("action");
                 channel_action_iter != channel_ptr->metadata.end()) {
        url_meta.action_type = convert_action(channel_action_iter->second);
        wrapper_file->channel_action_map.emplace(channel, url_meta.action_type);
        // LCOV_EXCL_STOP GCOVR_EXCL_STOP
      } else {
        wrapper_file->channel_action_map.emplace(channel,                      // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                                 ActionType::kUnknownAction);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      impl_->info.url_metas.emplace_back(std::move(url_meta));
    }

    wrapper_file->is_channel_broken = false;

    if (impl_->info.url_metas.empty()) {
      for (const auto& [id, pchannel] : reader->channels()) {
        // LCOV_EXCL_START GCOVR_EXCL_START
        wrapper_file->is_channel_broken = true;

        auto pschema_ptr = reader->schema(pchannel->schemaId);
        auto channel_count_iter = statistics->channelMessageCounts.find(id);

        Info::UrlMeta url_meta;

        url_meta.valid = true;
        url_meta.index = id - 1;
        url_meta.url = pchannel->topic;
        url_meta.url_type = "Event";

        if (pschema_ptr) {
          url_meta.ser_type = pschema_ptr->name;
          url_meta.schema_type =
              SchemaData::resolve_type(SchemaData::convert_encoding(pschema_ptr->encoding), url_meta.ser_type);

          if (!pschema_ptr->data.empty()) {
            wrapper_file->has_schema = true;
            impl_->info.has_schema = true;
          }
        }

        if (channel_count_iter != statistics->channelMessageCounts.end()) {
          url_meta.count = channel_count_iter->second;
        }

        wrapper_file->id_to_url_map.emplace(url_meta.index, url_meta.url);
        wrapper_file->url_to_id_map.emplace(url_meta.url, url_meta.index);

        url_ser_map().emplace(url_meta.url, url_meta.ser_type);
        url_schema_type_map().emplace(url_meta.url, url_meta.schema_type);

        if (auto action_iter = pchannel->metadata.find("action"); action_iter != pchannel->metadata.end()) {
          url_meta.action_type = convert_action(action_iter->second);
          wrapper_file->channel_action_map.emplace(id, url_meta.action_type);
          // LCOV_EXCL_STOP GCOVR_EXCL_STOP
        } else {
          wrapper_file->channel_action_map.emplace(id, ActionType::kUnknownAction);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }

        impl_->info.url_metas.emplace_back(std::move(url_meta));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }
    }

    std::sort(impl_->info.url_metas.begin(), impl_->info.url_metas.end());
    impl_->raw_url_metas = impl_->info.url_metas;
    rebuild_url_meta_lookup(impl_->info.url_metas);
  }

  impl_->info.has_idx_elapsed = false;
  impl_->info.has_idx_url = false;

  impl_->info.has_completed = wrapper_file->has_completed;

  return true;
}

void VCAPReader::open(const std::string& path) {
  close();

  auto to_open = [this](Impl::WrapperFile& wrapper_file) -> bool {
    mcap::Status status;

    wrapper_file.reader = std::make_unique<mcap::McapReader>();

    status = wrapper_file.reader->open(wrapper_file.path);

    if VUNLIKELY (!status.ok()) {
      CLOG_F("VCAPReader: Failed to open vcap, error = %s.", status.message.c_str());
      wrapper_file.reader.reset();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      return false;                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if VUNLIKELY (!prepare_file(&wrapper_file)) {
      wrapper_file.reader->close();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      wrapper_file.reader.reset();   // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      return false;                  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
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

    std::error_code exists_ec;

    if VUNLIKELY (!std::filesystem::exists(file_path, exists_ec)) {
      CLOG_F("VCAPReader: Mcap [%s] does not exist.", path.c_str());
      return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    std::filesystem::path parent_path;

    try {
      parent_path = file_path.parent_path();
    } catch (std::filesystem::filesystem_error&) {  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return std::tolower(c); });

    if (suffix == ".vcapx") {
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

          std::error_code db_exists_ec;

          if VUNLIKELY (!std::filesystem::exists(file_db, db_exists_ec)) {
            CLOG_F("VCAPReader: Mcap [%s] does not exist.", file_db_str.c_str());
            return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          }

          Impl::WrapperFile wrapper_file;
          wrapper_file.path = file_db_str;
          wrapper_file.index = file_index;

          if VUNLIKELY (!to_open(wrapper_file)) {
            CLOG_W("VCAPReader: Skipping invalid vcap [%s].", file_db_str.c_str());  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
            continue;                                                                // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

          if (!wrapper_file.has_idx_elapsed) {
            impl_->info.has_idx_elapsed = false;
          }

          if (!wrapper_file.has_idx_url) {
            impl_->info.has_idx_url = false;
          }

          if (wrapper_file.has_schema) {
            impl_->info.has_schema = true;
          }

          std::error_code db_size_ec;
          std::uintmax_t file_size = std::filesystem::file_size(file_db, db_size_ec);

          if VUNLIKELY (db_size_ec) {
            CLOG_W("VCAPReader: file_size failed for [%s]: %s.", file_db_str.c_str(),  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                   db_size_ec.message().c_str());                                      // LCOV_EXCL_LINE GCOVR_EXCL_LINE
            file_size = 0;                                                             // LCOV_EXCL_LINE GCOVR_EXCL_LINE
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
          VLOG_F("VCAPReader: DB list is empty.");
          return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }

        int version_major = header_json["major"];
        int version_minor = header_json["minor"];
        int version_patch = header_json["patch"];

        impl_->info.version =
            std::to_string(version_major) + "." + std::to_string(version_minor) + "." + std::to_string(version_patch);
        impl_->info.storage_type = "vcap";
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
            VLOG_E("VCAPReader: Invalid start_timestamp.");
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
          VLOG_F("VCAPReader: MCAP accuracy is not supported.");
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

          if (url_info.contains("action")) {
            url_meta.action_type = convert_action(url_info["action"].get<std::string>());
          }

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
        VLOG_F("VCAPReader: JSON parse error, ", e.what(), ".");
        return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }
    } else {
      Impl::WrapperFile wrapper_file;
      wrapper_file.path = impl_->path;

      if VUNLIKELY (!to_open(wrapper_file)) {
        CLOG_F("VCAPReader: Failed to prepare vcap [%s].", path.c_str());  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        return;                                                            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      impl_->info.file_size = 0;

      std::error_code single_size_ec;
      std::uintmax_t file_size = std::filesystem::file_size(file_path, single_size_ec);

      if VUNLIKELY (single_size_ec) {
        CLOG_W("VCAPReader: file_size failed for [%s]: %s.", path.c_str(),  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
               single_size_ec.message().c_str());                           // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        file_size = 0;                                                      // LCOV_EXCL_LINE GCOVR_EXCL_LINE
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
    VLOG_F("VCAPReader: Filesystem error, ", e.what(), ".");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return;                                                   // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
}

void VCAPReader::close() {
  impl_->cursor_iter.reset();
  impl_->cursor_iter_end.reset();
  impl_->cursor_msg_view.reset();

  for (auto& wrapper_file : impl_->file_list) {
    if (wrapper_file.reader) {
      wrapper_file.reader->close();
      wrapper_file.reader.reset();
    }
  }

  impl_->file_list.clear();
}

int VCAPReader::get_reset_index(const Config& config) {
  impl_->is_pending.store(true, std::memory_order_relaxed);

  // LCOV_EXCL_START GCOVR_EXCL_START
  auto status_function = [](const mcap::Status& status) {
    if (!status.ok()) {
      CLOG_W("VCAPReader: Failed to read message, error = %s.", status.message.c_str());
    }
  };
  // LCOV_EXCL_STOP GCOVR_EXCL_STOP

  auto filter_function = [this, &config](std::string_view url) -> bool {
    return match_playback_url_filter(url, config.filter_urls);
  };

  int start_index = -1;

  int64_t last_time = impl_->begin_time.load(std::memory_order_relaxed);

  mcap::ReadMessageOptions read_options;

  for (auto& wrapper_file : impl_->file_list) {
    if (start_index < 0 && impl_->begin_time.load(std::memory_order_relaxed) >= last_time &&
        impl_->begin_time.load(std::memory_order_relaxed) <= wrapper_file.end) {
      if (impl_->begin_time.load(std::memory_order_relaxed) > 0) {
        read_options.startTime =
            impl_->begin_time.load(std::memory_order_relaxed) * 1000'000 + impl_->total_start_timestamp_ns;
        read_options.endTime = mcap::MaxTime;
        if (!config.filter_urls.empty() || has_playback_url_rules()) {
          read_options.topicFilter = filter_function;
        }
        read_options.readOrder = mcap::ReadMessageOptions::ReadOrder::FileOrder;
      } else {
        read_options.startTime = 0;
        read_options.endTime = mcap::MaxTime;
        if (!config.filter_urls.empty() || has_playback_url_rules()) {
          read_options.topicFilter = filter_function;
        }
        read_options.readOrder = mcap::ReadMessageOptions::ReadOrder::FileOrder;
      }

      start_index = wrapper_file.index;
    } else {
      // LCOV_EXCL_START GCOVR_EXCL_START
      read_options.startTime = 0;
      read_options.endTime = mcap::MaxTime;
      if (!config.filter_urls.empty() || has_playback_url_rules()) {
        read_options.topicFilter = filter_function;
      }
      read_options.readOrder = mcap::ReadMessageOptions::ReadOrder::FileOrder;
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    }

    const auto [start_offset, end_offset] =
        wrapper_file.reader->byteRange(read_options.startTime, read_options.endTime);

    // NOLINTNEXTLINE(readability-redundant-smartptr-get)
    wrapper_file.msg_view = std::make_unique<mcap::LinearMessageView>(*wrapper_file.reader.get(), read_options,
                                                                      start_offset, end_offset, status_function);

    if (start_offset == end_offset) {
      wrapper_file.msg_view_begin.emplace(wrapper_file.msg_view->end());  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      wrapper_file.msg_view_end.emplace(wrapper_file.msg_view->end());    // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    } else {
      wrapper_file.msg_view_begin.emplace(wrapper_file.msg_view->begin());
      wrapper_file.msg_view_end.emplace(wrapper_file.msg_view->end());
    }

    last_time = wrapper_file.end;
  }

  impl_->is_pending.store(false, std::memory_order_relaxed);

  return start_index;
}

bool VCAPReader::prepare_cursor_view(int file_index) {
  impl_->cursor_iter.reset();
  impl_->cursor_iter_end.reset();
  impl_->cursor_msg_view.reset();
  impl_->cursor_need_advance = false;

  if VUNLIKELY (file_index < 0 || file_index >= static_cast<int>(impl_->file_list.size())) {
    return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  auto& wrapper_file = impl_->file_list.at(file_index);

  if VUNLIKELY (!wrapper_file.reader) {
    VLOG_W("VCAPReader: Cursor target vcap reader is empty.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;                                               // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  auto status_function = [this](const mcap::Status& status) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    if (!status.ok()) {
      CLOG_W("VCAPReader: Failed to read cursor message, error = %s.", status.message.c_str());
      impl_->cursor_read_error = true;
    }
  };
  // LCOV_EXCL_STOP GCOVR_EXCL_STOP

  auto filter_function = [this](std::string_view url) -> bool {
    return match_playback_url_filter(url, impl_->cursor_config.filter_urls);
  };

  mcap::ReadMessageOptions read_options;
  read_options.startTime =
      impl_->cursor_begin_us > 0 ? impl_->cursor_begin_us * 1000 + impl_->total_start_timestamp_ns : 0;
  read_options.endTime = mcap::MaxTime;
  if (!impl_->cursor_config.filter_urls.empty() || has_playback_url_rules()) {
    read_options.topicFilter = filter_function;
  }
  read_options.readOrder = mcap::ReadMessageOptions::ReadOrder::FileOrder;

  const auto [start_offset, end_offset] = wrapper_file.reader->byteRange(read_options.startTime, read_options.endTime);

  // NOLINTNEXTLINE(readability-redundant-smartptr-get)
  impl_->cursor_msg_view = std::make_unique<mcap::LinearMessageView>(*wrapper_file.reader.get(), read_options,
                                                                     start_offset, end_offset, status_function);

  if (start_offset == end_offset) {
    impl_->cursor_iter.emplace(impl_->cursor_msg_view->end());
  } else {
    impl_->cursor_iter.emplace(impl_->cursor_msg_view->begin());
  }

  impl_->cursor_iter_end.emplace(impl_->cursor_msg_view->end());
  impl_->cursor_file_index = file_index;

  return true;
}

bool VCAPReader::do_open_cursor(const Config& config) {
  impl_->cursor_config = config;
  impl_->cursor_begin_us = config.begin_time > 0 ? config.begin_time * 1000 : 0;
  impl_->cursor_end_us = config.end_time > 0 ? config.end_time * 1000 : 0;
  impl_->cursor_file_index = 0;
  impl_->cursor_read_error = false;

  if VUNLIKELY (impl_->file_list.empty()) {
    VLOG_W("VCAPReader: Cursor cannot find any data.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;                                        // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  return prepare_cursor_view(0);
}

bool VCAPReader::do_read_next(Frame& out, bool& is_error) {
  is_error = false;

  while (true) {
    if VUNLIKELY (!impl_->cursor_iter.has_value() || !impl_->cursor_iter_end.has_value()) {
      return false;
    }

    auto& iter = impl_->cursor_iter.value();
    auto& iter_end = impl_->cursor_iter_end.value();

    if (impl_->cursor_need_advance) {
      if (iter != iter_end) {
        iter++;
      }

      impl_->cursor_need_advance = false;
    }

    if (iter == iter_end) {
      if VUNLIKELY (impl_->cursor_read_error) {
        is_error = true;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        return false;     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      if (impl_->cursor_file_index + 1 < static_cast<int>(impl_->file_list.size())) {
        if VUNLIKELY (!prepare_cursor_view(impl_->cursor_file_index + 1)) {
          is_error = true;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          return false;     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }

        continue;
      }

      return false;
    }

    const int64_t timestamp = (iter->message.logTime - impl_->total_start_timestamp_ns) / 1000;

    if (impl_->cursor_begin_us > 0 && timestamp < impl_->cursor_begin_us) {
      iter++;    // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      continue;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if (impl_->cursor_end_us > 0 && timestamp > impl_->cursor_end_us) {
      return false;
    }

    if VUNLIKELY (iter->message.dataSize > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
      // LCOV_EXCL_START GCOVR_EXCL_START
      CLOG_W("VCAPReader: Cursor message data size is too large to address, size = %" PRIu64 ".",
             static_cast<uint64_t>(iter->message.dataSize));
      iter++;
      continue;
    }
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP

    std::string output_url;

    if VUNLIKELY (!convert_playback_url(iter->channel->topic, output_url)) {
      iter++;    // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      continue;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    ActionType action_type = ActionType::kUnknownAction;
    auto& wrapper_file = impl_->file_list.at(impl_->cursor_file_index);

    if (auto action_iter = wrapper_file.channel_action_map.find(iter->message.channelId);
        action_iter != wrapper_file.channel_action_map.end()) {
      action_type = action_iter->second;
    }

    const auto* data = reinterpret_cast<const uint8_t*>(iter->message.data);
    const auto size = static_cast<size_t>(iter->message.dataSize);

    out.timestamp = timestamp;
    out.url = std::move(output_url);
    out.ser_type.clear();
    out.schema_type = SchemaType::kUnknown;
    out.action_type = action_type;
    out.data = Bytes::shallow_copy(data, size);

    fill_frame_meta(out);

    impl_->cursor_need_advance = true;

    return true;
  }
}

void VCAPReader::read(const Config& config) {
  int loop_times = 0;

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
      VLOG_W("VCAPReader: Cannot find any data for play.");

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
      // impl_->pause_next_flag = false;
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
    const uint8_t* data = nullptr;
    size_t size = 0;

    // process files
    for (int index = start_index; index < static_cast<int>(impl_->file_list.size()); ++index) {
      impl_->split_index.store(index, std::memory_order_relaxed);

      auto& wrapper_file = impl_->file_list.at(impl_->split_index.load(std::memory_order_relaxed));

      if VUNLIKELY (!wrapper_file.reader) {
        VLOG_W("VCAPReader: Target vcap reader is empty.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        return;                                              // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      // process datas
      auto iter = std::move(wrapper_file.msg_view_begin).value();    // NOLINT
      auto iter_end = std::move(wrapper_file.msg_view_end).value();  // NOLINT

      for (; iter != iter_end; iter++) {
        timestamp = (iter->message.logTime - impl_->total_start_timestamp_ns) / 1000;

        if VUNLIKELY (last_timestamp > timestamp + 10'000U) {
          VLOG_W("VCAPReader: The vcap timestamp is incorrect.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }

        last_timestamp = timestamp;

        if (timestamp < impl_->begin_time.load(std::memory_order_relaxed) * 1000U) {
          continue;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }

        if (config.end_time > 0 && timestamp > config.end_time * 1000U) {
          timestamp = config.end_time * 1000U;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          is_end = true;                        // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }

        data = reinterpret_cast<const uint8_t*>(iter->message.data);

        if VUNLIKELY (iter->message.dataSize > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
          // LCOV_EXCL_START GCOVR_EXCL_START
          CLOG_W("VCAPReader: Message data size is too large to address, size = %" PRIu64 ".",
                 static_cast<uint64_t>(iter->message.dataSize));
          continue;
        }
        // LCOV_EXCL_STOP GCOVR_EXCL_STOP

        size = static_cast<size_t>(iter->message.dataSize);

        elapsed =
            (timestamp / impl_->rate.load(std::memory_order_relaxed)) -
            (impl_->elapsed_timer.get() - impl_->pause_elapsed.load(std::memory_order_relaxed)) -
            (impl_->begin_time.load(std::memory_order_relaxed) * 1000U / impl_->rate.load(std::memory_order_relaxed));

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

        ActionType action_type = ActionType::kUnknownAction;

        if (auto action_iter = wrapper_file.channel_action_map.find(iter->message.channelId);
            action_iter != wrapper_file.channel_action_map.end()) {
          action_type = action_iter->second;
        }

        Frame frame;
        frame.timestamp = timestamp;
        frame.url = iter->channel->topic;
        frame.action_type = action_type;
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
  } while (impl_->times.load(std::memory_order_relaxed) <= 0 ||
           (impl_->times.load(std::memory_order_relaxed) > 0 &&
            ++loop_times < impl_->times.load(std::memory_order_relaxed)));

  if (impl_->stop_flag.load(std::memory_order_relaxed)) {
    is_interrupted = true;
  }

  if (!impl_->jump_flag.load(std::memory_order_relaxed) && !is_interrupted) {
    flush_plugin();
  }

  if (!impl_->jump_flag.load(std::memory_order_relaxed) && impl_->finish_callback) {
    impl_->finish_callback(is_interrupted);
  }

  if (!impl_->jump_flag.load(std::memory_order_relaxed) && config.auto_quit) {
    quit();
  }

  // clean msg_view
  for (auto& wrapper_file : impl_->file_list) {
    wrapper_file.msg_view.reset();
  }
}

}  // namespace vlink
