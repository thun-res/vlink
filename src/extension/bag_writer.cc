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

#include "./extension/bag_writer.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "./base/helpers.h"
#include "./base/logger.h"
#include "./base/utils.h"
#include "./extension/bag_plugin_interface.h"
#include "./extension/schema_plugin_manager.h"
#include "./extension/vcap_writer.h"
#include "./extension/vdb_writer.h"

namespace vlink {

// GlobalWriter
struct GlobalWriter final {
  GlobalWriter() {
    const std::string& bag_path = Utils::get_env("VLINK_BAG_PATH");

    if (!bag_path.empty()) {
      VLOG_I("BagWriter: Global recorder is enabled.");
      CLOG_I("BagWriter: Record path: %s.", bag_path.c_str());

      instance = BagWriter::create(bag_path);

      if VLIKELY (instance) {
        instance->async_run();
      } else {
        CLOG_E("BagWriter: Global recorder is disabled because VLINK_BAG_PATH has an unsupported suffix.");
      }
    }
  }

  ~GlobalWriter() {
    if (instance) {
      instance.reset();
    }
  }

  static GlobalWriter& get() {
    static GlobalWriter global_writer;
    return global_writer;
  }

  std::mutex mtx;
  std::unordered_map<std::string, std::weak_ptr<BagWriter>> writer_map;

  std::shared_ptr<BagWriter> instance;

  VLINK_DISALLOW_COPY_AND_ASSIGN(GlobalWriter)
};

// BagWriter::Impl
struct BagWriter::Impl final {
  std::unordered_map<int, std::string> index_to_url_map;
  std::unordered_map<int, std::string> index_to_ser_map;
  std::unordered_map<std::string, int> url_to_index_map;
  std::unordered_map<std::string, int> ser_to_index_map;
  int current_url_index{0};
  int current_ser_index{0};
  mutable std::shared_mutex shared_mtx;

  std::shared_ptr<BagPluginInterface> plugin_interface;
  std::unordered_map<std::string, std::string> recorded_url_remap;
  std::unordered_map<std::string, std::vector<std::string>> recorded_urls_by_origin;
  std::unordered_map<std::string, std::string> recorded_url_origin;
  mutable std::shared_mutex record_state_mtx;

  std::mutex sample_mtx;
  std::unordered_map<std::string, double> url_loss_map;
  std::unordered_map<std::string, double> total_url_loss_map;

  std::mutex active_write_mtx;
  std::atomic<uint64_t> active_thread_id{0};
  std::string active_origin_url;
  bool active_immediate{false};
  int64_t active_record_result{0};

  std::atomic_bool stream_fail{false};
};

// BagWriter
std::shared_ptr<BagWriter> BagWriter::create(const std::string& path, const Config& config) {
  std::string suffix_check = path;

  std::transform(suffix_check.begin(), suffix_check.end(), suffix_check.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  if (Helpers::has_endwith(suffix_check, ".vdb") || Helpers::has_endwith(suffix_check, ".vdbx")) {
    return std::make_shared<VDBWriter>(path, config);
  } else if (Helpers::has_endwith(suffix_check, ".vcap") || Helpers::has_endwith(suffix_check, ".vcapx")) {
    return std::make_shared<VCAPWriter>(path, config);
  } else {
    CLOG_E("BagWriter: Unknown bag suffix, path=%s", path.c_str());
    return nullptr;
  }
}

std::shared_ptr<BagWriter> BagWriter::filter_get(const std::string& path) {
  static auto& instance = GlobalWriter::get();
  std::string suffix_check = path;

  std::transform(suffix_check.begin(), suffix_check.end(), suffix_check.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  const bool is_database = Helpers::has_endwith(suffix_check, ".vdb") || Helpers::has_endwith(suffix_check, ".vdbx");
  const bool is_mcap = Helpers::has_endwith(suffix_check, ".vcap") || Helpers::has_endwith(suffix_check, ".vcapx");

  if VUNLIKELY (!is_database && !is_mcap) {
    CLOG_E("BagWriter: Unknown bag suffix, path=%s", path.c_str());
    return nullptr;
  }

  std::lock_guard lock(instance.mtx);

  auto iter = instance.writer_map.find(path);

  if (iter != instance.writer_map.end()) {
    if (auto target = iter->second.lock()) {
      return target;
    }

    instance.writer_map.erase(iter);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  {
    std::shared_ptr<BagWriter> target;

    if (is_mcap) {
      auto* ptr = new VCAPWriter(path);

      target = std::shared_ptr<VCAPWriter>(ptr, [path](VCAPWriter* ptr) {
        {
          std::lock_guard lock(instance.mtx);
          auto iter = instance.writer_map.find(path);

          if (iter != instance.writer_map.end() && iter->second.expired()) {
            instance.writer_map.erase(iter);
          }
        }

        delete ptr;
      });
    } else {
      auto* ptr = new VDBWriter(path);

      target = std::shared_ptr<VDBWriter>(ptr, [path](VDBWriter* ptr) {
        {
          std::lock_guard lock(instance.mtx);
          auto iter = instance.writer_map.find(path);

          if (iter != instance.writer_map.end() && iter->second.expired()) {
            instance.writer_map.erase(iter);
          }
        }

        delete ptr;
      });
    }

    target->async_run();

    instance.writer_map.emplace(path, target);

    return target;
  }
}

BagWriter* BagWriter::global_get() { return GlobalWriter::get().instance.get(); }

BagWriter::BagWriter(const std::string& path, const Config& config) : impl_(std::make_unique<Impl>()) {
  (void)path;
  (void)config;

  if (!config.sync_mode) {
    impl_->index_to_url_map.reserve(128);
    impl_->index_to_ser_map.reserve(128);
    impl_->url_to_index_map.reserve(128);
    impl_->ser_to_index_map.reserve(128);
  }

  Bytes::init_memory_pool();
}

void BagWriter::get_url_meta(const std::string& url, const std::string& ser, int& url_index, int& ser_index) const {
  {
    std::shared_lock read_lock(impl_->shared_mtx);

    auto url_iter = impl_->url_to_index_map.find(url);
    auto ser_iter = impl_->ser_to_index_map.find(ser);

    if VLIKELY (url_iter != impl_->url_to_index_map.end() && ser_iter != impl_->ser_to_index_map.end()) {
      url_index = url_iter->second;
      ser_index = ser_iter->second;
      return;
    }
  }

  std::unique_lock write_lock(impl_->shared_mtx);

  auto& url_id = impl_->url_to_index_map.try_emplace(url, -1).first->second;

  if (url_id < 0) {
    url_id = ++impl_->current_url_index;
    impl_->index_to_url_map[url_id] = url;
  }

  auto& ser_id = impl_->ser_to_index_map.try_emplace(ser, -1).first->second;

  if (ser_id < 0) {
    ser_id = ++impl_->current_ser_index;
    impl_->index_to_ser_map[ser_id] = ser;
  }

  url_index = url_id;
  ser_index = ser_id;
}

void BagWriter::get_url_meta(int url_index, int ser_index, std::string& url, std::string& ser) const {
  std::shared_lock read_lock(impl_->shared_mtx);

  auto url_iter = impl_->index_to_url_map.find(url_index);

  if VLIKELY (url_iter != impl_->index_to_url_map.end()) {
    url = url_iter->second;
  }

  auto ser_iter = impl_->index_to_ser_map.find(ser_index);

  if VLIKELY (ser_iter != impl_->index_to_ser_map.end()) {
    ser = ser_iter->second;
  }
}

BagWriter::~BagWriter() {
  std::shared_ptr<BagPluginInterface> plugin_interface;

  {
    std::shared_lock state_lock(impl_->record_state_mtx);
    plugin_interface = impl_->plugin_interface;
  }

  if (plugin_interface) {
    plugin_interface->register_callback({});
  }
}

void BagWriter::flush_plugin() {
  std::shared_ptr<BagPluginInterface> plugin_interface;

  {
    std::shared_lock state_lock(impl_->record_state_mtx);
    plugin_interface = impl_->plugin_interface;
  }

  if (plugin_interface) {
    plugin_interface->flush();
  }
}

void BagWriter::detach_plugin() {
  std::shared_ptr<BagPluginInterface> plugin_interface;

  {
    std::unique_lock state_lock(impl_->record_state_mtx);
    plugin_interface = std::move(impl_->plugin_interface);
  }

  if (plugin_interface) {
    plugin_interface->flush();
    plugin_interface->register_callback({});
  }
}

void BagWriter::bind_plugin_interface(const std::shared_ptr<BagPluginInterface>& plugin_interface) {
  std::shared_ptr<BagPluginInterface> old_plugin_interface;

  {
    std::shared_lock state_lock(impl_->record_state_mtx);
    old_plugin_interface = impl_->plugin_interface;
  }

  if (old_plugin_interface && old_plugin_interface != plugin_interface) {
    old_plugin_interface->flush();
    old_plugin_interface->register_callback({});
  }

  if VLIKELY (plugin_interface) {
    plugin_interface->bind_direction(BagPluginInterface::Direction::kWrite);

    plugin_interface->register_callback([this](const Frame& frame) {
      const bool active = impl_->active_thread_id.load(std::memory_order_acquire) == Utils::get_native_thread_id();

      if VUNLIKELY (frame.url.empty()) {
        if (active) {
          impl_->active_record_result = -1;
        }

        return;
      }

      if (active && impl_->active_origin_url != frame.url) {
        learn_recorded_url(impl_->active_origin_url, frame.url);
      }

      const bool immediate = active && impl_->active_immediate;

      const int64_t record_result = record(frame, immediate);

      if VUNLIKELY (active && record_result < 0) {
        impl_->active_record_result = record_result;
      }
    });
  }

  std::unique_lock state_lock(impl_->record_state_mtx);

  impl_->plugin_interface = plugin_interface;
}

void BagWriter::clear_plugin_interface() { bind_plugin_interface(nullptr); }

int64_t BagWriter::push(const Frame& frame, bool immediate) {
  if VUNLIKELY (frame.url.empty()) {
    return -1;
  }

  const int64_t target_timestamp = frame.timestamp < 0 ? get_record_timestamp() : frame.timestamp;

  std::shared_ptr<BagPluginInterface> plugin_interface;

  {
    std::shared_lock state_lock(impl_->record_state_mtx);
    plugin_interface = impl_->plugin_interface;
  }

  Frame stamped;
  const Frame* effective = &frame;

  if (frame.timestamp != target_timestamp) {
    stamped.timestamp = target_timestamp;
    stamped.url = frame.url;
    stamped.ser_type = frame.ser_type;
    stamped.schema_type = frame.schema_type;
    stamped.action_type = frame.action_type;
    stamped.data = Bytes::shallow_copy(frame.data.data(), frame.data.size());
    effective = &stamped;
  }

  if VLIKELY (!plugin_interface) {
    return record(*effective, immediate);
  }

  std::lock_guard active_lock(impl_->active_write_mtx);

  impl_->active_origin_url = effective->url;
  impl_->active_immediate = immediate;
  impl_->active_record_result = target_timestamp;
  impl_->active_thread_id.store(Utils::get_native_thread_id(), std::memory_order_release);
  plugin_interface->on_write(*effective);

  impl_->active_thread_id.store(0, std::memory_order_release);
  const int64_t result = impl_->active_record_result < 0 ? impl_->active_record_result : target_timestamp;
  impl_->active_origin_url.clear();
  impl_->active_immediate = false;
  impl_->active_record_result = 0;

  return result;
}

BagWriter& BagWriter::operator<<(const Frame& frame) {
  if VUNLIKELY (push(frame, false) < 0) {
    set_fail();
  }

  return *this;
}

BagWriter& BagWriter::operator<<(const SchemaData& schema_data) {
  if VUNLIKELY (!push_schema(schema_data, false)) {
    set_fail();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  return *this;
}

bool BagWriter::fail() const noexcept { return impl_->stream_fail.load(std::memory_order_acquire); }

BagWriter::operator bool() const noexcept { return !impl_->stream_fail.load(std::memory_order_acquire); }

void BagWriter::clear() noexcept { impl_->stream_fail.store(false, std::memory_order_release); }

void BagWriter::close() {}

bool BagWriter::post_persistent_task(Callback&& callback) {
  PostTaskOptions options;
  options.overflow_policy = TaskOverflowPolicy::kReject;
  options.drop_policy = TaskDropPolicy::kProtected;

  const auto handle = post_task_handle(std::move(callback), options);

  switch (handle.state()) {
    case TaskExecutionState::kInvalid:
    case TaskExecutionState::kCancelled:
    case TaskExecutionState::kDropped:
    case TaskExecutionState::kRejected:
      return false;
    case TaskExecutionState::kQueued:
    case TaskExecutionState::kRunning:
    case TaskExecutionState::kCompleted:
    case TaskExecutionState::kFailed:
      return true;
  }

  return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
}

void BagWriter::set_fail() noexcept { impl_->stream_fail.store(true, std::memory_order_release); }

void BagWriter::learn_recorded_url(const std::string& origin_url, const std::string& recorded_url) {
  {
    std::shared_lock state_lock(impl_->record_state_mtx);

    auto iter = impl_->recorded_url_remap.find(origin_url);

    if (iter != impl_->recorded_url_remap.end() && iter->second == recorded_url) {
      return;
    }
  }

  std::unique_lock state_lock(impl_->record_state_mtx);

  impl_->recorded_url_remap.try_emplace(origin_url, recorded_url);
  auto& recorded_urls = impl_->recorded_urls_by_origin[origin_url];

  if (std::find(recorded_urls.begin(), recorded_urls.end(), recorded_url) == recorded_urls.end()) {
    recorded_urls.emplace_back(recorded_url);
  }

  impl_->recorded_url_origin[recorded_url] = origin_url;
}

std::string BagWriter::convert_recorded_url(const std::string& url) const {
  std::shared_lock state_lock(impl_->record_state_mtx);

  auto iter = impl_->recorded_url_remap.find(url);

  return iter == impl_->recorded_url_remap.end() ? url : iter->second;
}

std::vector<std::string> BagWriter::recorded_urls_for_origin(const std::string& url) const {
  std::shared_lock state_lock(impl_->record_state_mtx);
  std::vector<std::string> urls;
  urls.emplace_back(url);

  auto iter = impl_->recorded_urls_by_origin.find(url);

  if (iter != impl_->recorded_urls_by_origin.end()) {
    for (const auto& recorded_url : iter->second) {
      if (recorded_url != url) {
        urls.emplace_back(recorded_url);
      }
    }
  }

  return urls;
}

std::string BagWriter::recover_recorded_url(const std::string& url) const {
  std::shared_lock state_lock(impl_->record_state_mtx);

  auto iter = impl_->recorded_url_origin.find(url);

  return iter == impl_->recorded_url_origin.end() ? url : iter->second;
}

std::mutex& BagWriter::sample_mutex() { return impl_->sample_mtx; }

std::unordered_map<std::string, double>& BagWriter::url_loss_map_ref() { return impl_->url_loss_map; }

std::unordered_map<std::string, double>& BagWriter::total_url_loss_map_ref() { return impl_->total_url_loss_map; }

void BagWriter::set_url_loss(const std::string& url, double loss) {
  if (loss > 1) {
    loss = -1;
  }

  std::lock_guard lock(impl_->sample_mtx);

  impl_->url_loss_map[url] = loss;
  impl_->total_url_loss_map[url] = loss;
}

const std::string& BagWriter::get_default_tag_name() {
  static std::string tag_name_env_str = Utils::get_env("VLINK_BAG_TAG", "Empty");
  return tag_name_env_str;
}

const std::string& BagWriter::get_default_app_name() {
  static std::string app_name = Utils::get_app_name();
  return app_name;
}

SchemaPluginInterface* BagWriter::get_schema_interface() { return SchemaPluginManager::get().get_interface().get(); }

int32_t BagWriter::get_default_timezone_diff() { return Utils::get_timezone_diff(); }

std::string_view BagWriter::convert_action(ActionType type) {
  switch (type) {
    case ActionType::kClientRequest:
      return "C/Req";
    case ActionType::kClientResponse:
      return "C/Resp";
    case ActionType::kServerRequest:
      return "S/Req";
    case ActionType::kServerResponse:
      return "S/Resp";
    case ActionType::kPublish:
      return "Pub";
    case ActionType::kSubscribe:
      return "Sub";
    case ActionType::kSet:
      return "Set";
    case ActionType::kGet:
      return "Get";
    default:
      return "Unknown";
  }
}

std::string BagWriter::get_format_date(SystemClock* current, bool file_format) {
  SystemClock time_point;

  if (current) {
    time_point = *current;
  } else {
    time_point = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
  }

  auto milliseconds = time_point.time_since_epoch().count() % 1000U;

  std::time_t now_time_t = std::chrono::system_clock::to_time_t(time_point);

  std::tm now_tm{};

#ifdef _WIN32
  localtime_s(&now_tm, &now_time_t);
#else
  localtime_r(&now_time_t, &now_tm);
#endif

  char buffer[32];
  char full_buffer[64];

  if (file_format) {
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d_%H-%M-%S", &now_tm);
    std::snprintf(full_buffer, sizeof(full_buffer), "%s-%03lld", buffer,
                  static_cast<long long>(milliseconds));  // NOLINT(runtime/int, google-runtime-int)
  } else {
    std::strftime(buffer, sizeof(buffer), "%Y/%m/%d %H:%M:%S", &now_tm);
    std::snprintf(full_buffer, sizeof(full_buffer), "%s:%03lld", buffer,
                  static_cast<long long>(milliseconds));  // NOLINT(runtime/int, google-runtime-int)
  }

  return full_buffer;
}

}  // namespace vlink
