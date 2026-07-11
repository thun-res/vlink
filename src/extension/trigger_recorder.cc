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

#include "./extension/trigger_recorder.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "./base/elapsed_timer.h"
#include "./base/helpers.h"
#include "./base/logger.h"
#include "./base/message_loop.h"
#include "./base/plugin.h"
#include "./base/timer.h"
#include "./base/utils.h"
#include "./extension/bag_plugin_interface.h"
#include "./extension/bag_writer.h"
#include "./extension/trigger_plugin_interface.h"
#include "./subscriber.h"

namespace vlink {

// LCOV_EXCL_START GCOVR_EXCL_START
static bool contains_exact(const std::vector<std::string>& list, const std::string& url) {
  return std::find(list.begin(), list.end(), url) != list.end();
}

static std::string to_lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });

  return text;
}

static std::vector<std::string> lower_trigger_patterns(const std::string& filter_str) {
  std::vector<std::string> patterns;

  for (const auto& pattern : Helpers::split_any(filter_str)) {
    if (!pattern.empty()) {
      patterns.push_back(to_lower(pattern));
    }
  }

  return patterns;
}

static bool accept_trigger(const std::string& url, const TriggerRecorder::TriggerParams& params,
                           const std::vector<std::string>& lower_patterns) {
  if (!params.filter_urls.empty()) {
    return params.filter_urls.count(url) != 0;
  }

  if (params.filter_str.empty()) {
    return true;
  }

  const std::string lower_url = to_lower(url);
  bool hit = false;

  for (const auto& pattern : lower_patterns) {
    if (lower_url.find(pattern) != std::string::npos) {
      hit = true;
      break;
    }
  }

  return params.black_mode ? !hit : hit;
}

static bool validate_config(const TriggerRecorder::Config& config) {
  if VUNLIKELY (config.default_pre_ms > TriggerRecorder::kMaxWindowMs ||
                config.default_post_ms > TriggerRecorder::kMaxWindowMs ||
                config.retention_guard_ms > TriggerRecorder::kMaxWindowMs) {
    VLOG_E("TriggerRecorder: pre/post/retention_guard exceeds the supported range");
    return false;
  }

  if VUNLIKELY (config.default_post_ms + config.retention_guard_ms > std::numeric_limits<uint32_t>::max()) {
    VLOG_E("TriggerRecorder: default post window plus retention guard exceeds the timer range");
    return false;
  }

  if VUNLIKELY (config.default_max_packet_size < 0 || config.default_max_size < 0 || config.max_cache_size < 0 ||
                config.sleep_interval < 0 || config.sleep_time_ms < 0) {
    VLOG_E("TriggerRecorder: byte limits and sleep values must be non-negative");
    return false;
  }

  if VUNLIKELY (config.file_type != TriggerRecorder::kVdb && config.file_type != TriggerRecorder::kVcap) {
    VLOG_E("TriggerRecorder: unsupported file type");
    return false;
  }

  if VUNLIKELY (config.overflow != TriggerRecorder::kCoverOldest && config.overflow != TriggerRecorder::kDropNewest) {
    VLOG_E("TriggerRecorder: unsupported overflow policy");
    return false;
  }

  for (const auto& [url, value] : config.url_overrides) {
    const int64_t pre_ms = value.pre_ms >= 0 ? value.pre_ms : config.default_pre_ms;
    const int64_t post_ms = value.post_ms >= 0 ? value.post_ms : config.default_post_ms;

    if VUNLIKELY (pre_ms > TriggerRecorder::kMaxWindowMs || post_ms > TriggerRecorder::kMaxWindowMs ||
                  post_ms + config.retention_guard_ms > std::numeric_limits<uint32_t>::max()) {
      VLOG_E("TriggerRecorder: URL window exceeds the supported range: ", url);
      return false;
    }
  }

  return true;
}

// LCOV_EXCL_STOP GCOVR_EXCL_STOP

static std::string sanitize_name(const std::string& name) {
  std::string result = name;

  for (char& character : result) {
    if (character == '/' || character == '\\') {
      character = '_';
    }
  }

  return result;
}

// LCOV_EXCL_START GCOVR_EXCL_START
static void notify_dump_failed(const std::shared_ptr<TriggerPluginInterface>& plugin, const std::string& reason,
                               const std::string& path, const std::string& error) {
  if (!plugin) {
    return;
  }

  TriggerPluginInterface::DumpResult result;
  result.reason = reason;
  result.path = path;
  result.success = false;
  result.error = error;

  plugin->on_dump_failed(result);
}
// LCOV_EXCL_STOP GCOVR_EXCL_STOP

// TriggerRecorder::UrlBuffer
struct TriggerRecorder::UrlBuffer final {  // NOLINT(clang-analyzer-optin.performance.Padding)
  struct Entry final {
    int64_t capture_ts_us{0};
    int64_t size{0};
    std::shared_ptr<const Bytes> payload;
  };

  std::mutex mtx;
  std::deque<Entry> ring;
  int64_t bytes{0};
  bool dead{false};
  bool frozen{false};

  int64_t pre_us{0};
  int64_t post_us{0};
  std::atomic<int64_t> retention_us{0};
  int64_t max_packet_size{0};
  int64_t max_size{0};
  bool only_front{false};
  bool only_back{false};
  bool getter_semantics{false};

  std::string url;
  std::string ser_type;
  SchemaType schema_type{SchemaType::kUnknown};
  SampleLostInfo last_lost;
  SampleLostInfo final_lost;
  bool final_lost_valid{false};

  std::shared_ptr<RawSub> sub;
};

// TriggerRecorder::DumpJob
struct TriggerRecorder::DumpJob final {
  struct FinishGuard final {
    TriggerRecorder* recorder{nullptr};
    std::shared_ptr<DumpJob> job;

    ~FinishGuard();
  };

  int64_t trigger_ts{0};
  TriggerParams params;
  std::string path;
  std::shared_ptr<TriggerPluginInterface> trigger_plugin;
  std::shared_ptr<BagPluginInterface> bag_plugin;
  std::vector<std::shared_ptr<UrlBuffer>> frozen_buffers;
};

TriggerRecorder::DumpJob::FinishGuard::~FinishGuard() { recorder->finish_dump(job); }

// TriggerRecorder::Impl
struct TriggerRecorder::Impl final {  // NOLINT(clang-analyzer-optin.performance.Padding)
  Config config;
  RawSubFactory raw_sub_factory;
  ElapsedTimer capture_timer{ElapsedTimer::kCpuTimestamp, ElapsedTimer::kMicro};
  int64_t anchor_wall_us{0};
  std::string file_suffix;

  std::atomic<int64_t> max_post_all_us{0};
  std::atomic<int64_t> global_bytes{0};
  std::atomic_bool dumping{false};
  std::atomic_bool writing{false};
  std::atomic_bool running{false};

  std::mutex lifecycle_mtx;
  std::mutex bag_plugin_mtx;
  std::shared_mutex url_buffer_mtx;

  std::unordered_map<std::string, std::shared_ptr<UrlBuffer>> url_buffer_map;
  std::shared_ptr<DumpJob> active_dump_job;

  std::unique_ptr<DiscoveryViewer> viewer;
  Timer sweep_timer;

  Plugin plugin_loader;

  std::shared_ptr<TriggerPluginInterface> trigger_plugin;
  std::optional<std::shared_ptr<TriggerPluginInterface>> pending_trigger_plugin;

  std::shared_ptr<BagPluginInterface> bag_plugin;
};

// TriggerRecorder
TriggerRecorder::TriggerRecorder(const Config& config, RawSubFactory&& factory) : impl_(std::make_unique<Impl>()) {
  set_name("TriggerRecorder");

  impl_->config = config;
  impl_->raw_sub_factory = std::move(factory);

  if VUNLIKELY (!impl_->raw_sub_factory) {
    VLOG_F("TriggerRecorder: raw subscriber factory is not set");
  }

  if (impl_->config.dump_dir.empty()) {
    impl_->config.dump_dir = Utils::get_tmp_dir() + "/vlink-trigger";
  }

  if (impl_->config.default_pre_ms < 0) {
    impl_->config.default_pre_ms = 0;
  }

  if (impl_->config.default_post_ms < 0) {
    impl_->config.default_post_ms = 0;
  }

  if (impl_->config.retention_guard_ms < 0) {
    impl_->config.retention_guard_ms = 0;
  }

  if VUNLIKELY (!validate_config(impl_->config)) {
    VLOG_F("TriggerRecorder: invalid configuration");
  }

  impl_->file_suffix = (impl_->config.file_type == kVcap) ? ".vcap" : ".vdb";

  std::error_code ec;
  std::filesystem::create_directories(impl_->config.dump_dir, ec);

  std::error_code query_ec;

  if VUNLIKELY (ec && !std::filesystem::is_directory(impl_->config.dump_dir, query_ec)) {
    VLOG_F("TriggerRecorder: cannot create dump_dir '", impl_->config.dump_dir,
           "': ", ec.message());  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if (!impl_->config.bag_plugin_lib.empty()) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    auto loaded =
        impl_->plugin_loader.load<BagPluginInterface>(impl_->config.bag_plugin_lib, 2, 0, impl_->config.bag_plugin_dir);

    if VUNLIKELY (!loaded) {
      VLOG_F("TriggerRecorder: failed to load bag reorder plugin '", impl_->config.bag_plugin_lib, "'");
    }

    impl_->bag_plugin = std::move(loaded);
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  create_discovery_viewer();
}

TriggerRecorder::~TriggerRecorder() {
  quit(true);
  wait_for_quit();
}

bool TriggerRecorder::dump(const TriggerParams& params) {
  if VUNLIKELY ((params.pre_ms >= 0 && params.pre_ms > kMaxWindowMs) ||
                (params.post_ms >= 0 && params.post_ms > kMaxWindowMs)) {
    VLOG_E("TriggerRecorder: trigger window exceeds the supported range");
    return false;
  }

  if VUNLIKELY (!impl_->running.load(std::memory_order_acquire)) {
    return false;
  }

  if VUNLIKELY (impl_->dumping.load(std::memory_order_acquire)) {
    return false;
  }

  std::unique_lock lifecycle_lock(impl_->lifecycle_mtx);

  if VUNLIKELY (!impl_->running.load(std::memory_order_acquire) || impl_->dumping.load(std::memory_order_acquire)) {
    return false;
  }

  auto job = std::make_shared<DumpJob>();
  job->params = params;

  if (!params.out_file.empty()) {
    job->path = params.out_file;
  } else {
    const std::string base =
        params.name_hint.empty() ? BagWriter::get_format_date(nullptr, true) : sanitize_name(params.name_hint);
    job->path = impl_->config.dump_dir + "/" + base + impl_->file_suffix;
    uint64_t suffix = 1;
    std::error_code ec;

    while (std::filesystem::exists(job->path, ec)) {
      ec.clear();
      job->path = impl_->config.dump_dir + "/" + base + "_" + std::to_string(suffix++) + impl_->file_suffix;
    }
  }

  int64_t max_post = 0;

  {
    std::lock_guard bag_plugin_lock(impl_->bag_plugin_mtx);
    std::unique_lock map_lock(impl_->url_buffer_mtx);
    job->frozen_buffers.reserve(impl_->url_buffer_map.size());
    job->trigger_ts = impl_->capture_timer.get();
    max_post = impl_->max_post_all_us.load(std::memory_order_relaxed);
    job->trigger_plugin = impl_->trigger_plugin;
    job->bag_plugin = impl_->bag_plugin;

    for (const auto& entry : impl_->url_buffer_map) {
      entry.second->frozen = true;
      job->frozen_buffers.push_back(entry.second);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    impl_->active_dump_job = job;
    impl_->dumping.store(true, std::memory_order_release);
  }

  const int64_t guard_us = impl_->config.retention_guard_ms * 1000;
  const int64_t delay_ms = max_post > 0 ? (max_post + guard_us) / 1000 : 0;

  auto task = [this, weak_job = std::weak_ptr<DumpJob>(job)]() {
    if (auto locked_job = weak_job.lock()) {
      do_dump(locked_job);
    }
  };

  bool ok = false;

  if (delay_ms > 0) {
    ok = Timer::call_once(this, static_cast<uint32_t>(delay_ms), std::move(task));
  } else {
    ok = post_task(std::move(task));
  }

  if VUNLIKELY (!ok) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    finish_dump_locked(job);

    return false;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  return true;
}

bool TriggerRecorder::is_dumping() const noexcept { return impl_->dumping.load(std::memory_order_acquire); }

void TriggerRecorder::bind_trigger_plugin_interface(const std::shared_ptr<TriggerPluginInterface>& plugin) {
  std::lock_guard lifecycle_lock(impl_->lifecycle_mtx);

  if (impl_->dumping.load(std::memory_order_acquire)) {
    impl_->pending_trigger_plugin = plugin;
    return;
  }

  if (!impl_->running.load(std::memory_order_acquire)) {
    auto old_plugin = impl_->trigger_plugin;
    impl_->trigger_plugin = plugin;

    if (old_plugin && old_plugin != plugin) {
      old_plugin->flush();
    }

    return;
  }

  apply_trigger_plugin_transition(plugin);
}

void TriggerRecorder::clear_trigger_plugin_interface() { bind_trigger_plugin_interface(nullptr); }

void TriggerRecorder::bind_bag_plugin_interface(const std::shared_ptr<BagPluginInterface>& plugin) {
  std::lock_guard lock(impl_->bag_plugin_mtx);
  impl_->bag_plugin = plugin;
}

void TriggerRecorder::clear_bag_plugin_interface() { bind_bag_plugin_interface(nullptr); }

void TriggerRecorder::on_begin() {
  MessageLoop::on_begin();

  std::lock_guard lifecycle_lock(impl_->lifecycle_mtx);

  if (!impl_->viewer) {
    try {
      create_discovery_viewer();
    } catch (const std::exception& e) {
      VLOG_E("TriggerRecorder: discovery viewer setup failed: ", e.what());
      return;
    }
  }

  impl_->capture_timer.start();
  impl_->anchor_wall_us =
      static_cast<int64_t>(ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMicro)) - impl_->capture_timer.get();

  if VUNLIKELY (!impl_->viewer->async_run()) {
    VLOG_E("TriggerRecorder: discovery viewer failed to start");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return;                                                       // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if VUNLIKELY (!impl_->sweep_timer.attach(this)) {
    return;
  }

  impl_->sweep_timer.set_interval(1000);
  impl_->sweep_timer.set_loop_count(Timer::kInfinite);
  impl_->sweep_timer.start([this]() { sweep_evict(); });

  impl_->running.store(true, std::memory_order_release);

  auto active_plugin = impl_->trigger_plugin;

  if (active_plugin) {
    active_plugin->on_start();
  }
}

void TriggerRecorder::on_end() {
  std::unique_lock lifecycle_lock(impl_->lifecycle_mtx);

  const bool recorder_started = impl_->running.exchange(false, std::memory_order_acq_rel);
  impl_->sweep_timer.stop();

  if (impl_->viewer) {
    impl_->viewer->quit(true);
    impl_->viewer->wait_for_quit();
    impl_->viewer.reset();
  }

  const bool dump_abandoned = impl_->dumping.exchange(false, std::memory_order_acq_rel);
  std::shared_ptr<DumpJob> abandoned_job;

  impl_->writing.store(false, std::memory_order_release);

  if (dump_abandoned) {
    abandoned_job = impl_->active_dump_job;
  }

  impl_->active_dump_job.reset();

  std::vector<std::shared_ptr<RawSub>> retired_subscribers;

  {
    std::unique_lock lock(impl_->url_buffer_mtx);
    retired_subscribers.reserve(impl_->url_buffer_map.size());

    for (auto& entry : impl_->url_buffer_map) {
      retired_subscribers.push_back(deactivate_url_buffer(entry.second.get()));
    }

    impl_->url_buffer_map.clear();
    impl_->global_bytes.store(0, std::memory_order_relaxed);
    impl_->max_post_all_us.store(0, std::memory_order_relaxed);
  }

  retired_subscribers.clear();

  auto active_plugin = impl_->trigger_plugin;
  auto pending_plugin = std::move(impl_->pending_trigger_plugin);
  impl_->pending_trigger_plugin.reset();

  if (recorder_started) {
    if VUNLIKELY (dump_abandoned && abandoned_job) {
      notify_dump_failed(abandoned_job->trigger_plugin, abandoned_job->params.reason, abandoned_job->path,
                         "dump abandoned at shutdown");
    }

    if (active_plugin) {
      active_plugin->flush();
      active_plugin->on_stop();
    }
  }

  if (pending_plugin) {
    impl_->trigger_plugin = *pending_plugin;
  }

  lifecycle_lock.unlock();

  MessageLoop::on_end();
}

void TriggerRecorder::create_discovery_viewer() {
  impl_->viewer = std::make_unique<DiscoveryViewer>(impl_->config.discovery_filter);
  impl_->viewer->register_callback([this](const std::vector<DiscoveryViewer::Info>& list) { handle_discovery(list); });
}

void TriggerRecorder::apply_trigger_plugin_transition(const std::shared_ptr<TriggerPluginInterface>& plugin) {
  auto old_plugin = impl_->trigger_plugin;

  if (old_plugin == plugin) {
    return;
  }

  if (old_plugin) {
    old_plugin->flush();
    old_plugin->on_stop();
  }

  if (plugin) {
    plugin->on_start();
  }

  impl_->trigger_plugin = plugin;
}

// LCOV_EXCL_START GCOVR_EXCL_START
void TriggerRecorder::handle_data(UrlBuffer* url_buffer, const Bytes& data) {
  if VUNLIKELY (!impl_->running.load(std::memory_order_relaxed)) {
    return;
  }

  if VUNLIKELY (impl_->config.busy_skip_data && impl_->writing.load(std::memory_order_relaxed)) {
    return;
  }

  const size_t data_size = data.size();

  if VUNLIKELY (url_buffer->max_packet_size > 0 && static_cast<int64_t>(data_size) > url_buffer->max_packet_size) {
    return;
  }

  const auto incoming = static_cast<int64_t>(data_size);

  if VUNLIKELY (url_buffer->max_size > 0 && incoming > url_buffer->max_size) {
    return;
  }

  if VUNLIKELY (impl_->config.max_cache_size > 0 && incoming > impl_->config.max_cache_size) {
    return;
  }

  auto payload = std::make_shared<const Bytes>(Bytes::deep_copy(data.data(), data_size));

  std::lock_guard lock(url_buffer->mtx);

  if VUNLIKELY (url_buffer->dead) {
    return;
  }

  const int64_t capture_ts = impl_->capture_timer.get();
  const int64_t horizon = capture_ts - url_buffer->retention_us.load(std::memory_order_relaxed);

  while (!url_buffer->ring.empty() && url_buffer->ring.front().capture_ts_us < horizon) {
    const int64_t size = url_buffer->ring.front().size;
    url_buffer->bytes -= size;
    impl_->global_bytes.fetch_sub(size, std::memory_order_relaxed);
    url_buffer->ring.pop_front();
  }

  const size_t evictable_count = url_buffer->ring.size();

  url_buffer->ring.push_back(UrlBuffer::Entry{capture_ts, incoming, std::move(payload)});

  size_t evict_count = 0;
  int64_t evict_bytes = 0;

  auto reserve_caps = [this, url_buffer, incoming, evictable_count, &evict_count, &evict_bytes]() {
    int64_t local_need = 0;

    if (url_buffer->max_size > 0 && url_buffer->bytes > url_buffer->max_size - incoming) {
      local_need = url_buffer->bytes - (url_buffer->max_size - incoming);
    }

    if (local_need > 0 && impl_->config.overflow == kDropNewest) {
      return false;
    }

    int64_t global = impl_->global_bytes.load(std::memory_order_relaxed);

    while (true) {
      if VUNLIKELY (global > std::numeric_limits<int64_t>::max() - incoming) {
        return false;
      }

      int64_t global_need = 0;

      if (impl_->config.max_cache_size > 0 && global > impl_->config.max_cache_size - incoming) {
        global_need = global - (impl_->config.max_cache_size - incoming);

        if (impl_->config.overflow == kDropNewest) {
          return false;
        }
      }

      const int64_t need = std::max(local_need, global_need);

      while (evict_count < evictable_count && evict_bytes < need) {
        evict_bytes += url_buffer->ring[evict_count].size;
        ++evict_count;
      }

      while (evict_count > 0 && evict_bytes - url_buffer->ring[evict_count - 1].size >= need) {
        evict_bytes -= url_buffer->ring[evict_count - 1].size;
        --evict_count;
      }

      if (evict_bytes < need || evict_bytes > global) {
        return false;
      }

      const int64_t desired = global - evict_bytes + incoming;

      if (impl_->global_bytes.compare_exchange_weak(global, desired, std::memory_order_relaxed,
                                                    std::memory_order_relaxed)) {
        return true;
      }
    }
  };

  if VUNLIKELY (!reserve_caps()) {
    url_buffer->ring.pop_back();
    return;
  }

  for (size_t index = 0; index < evict_count; ++index) {
    url_buffer->ring.pop_front();
  }

  url_buffer->bytes = url_buffer->bytes - evict_bytes + incoming;
}

std::shared_ptr<TriggerRecorder::UrlBuffer> TriggerRecorder::build_url_buffer(const DiscoveryViewer::Info& info) {
  auto url_buffer = std::make_shared<UrlBuffer>();
  url_buffer->url = info.url;
  url_buffer->ser_type = info.ser_type;
  url_buffer->schema_type = info.schema_type;

  UrlConfig url_config;

  auto override_iter = impl_->config.url_overrides.find(info.url);

  if (override_iter != impl_->config.url_overrides.end()) {
    url_config = override_iter->second;
  }

  const int64_t pre_ms =
      std::max<int64_t>(0, url_config.pre_ms >= 0 ? url_config.pre_ms : impl_->config.default_pre_ms);
  const int64_t post_ms =
      std::max<int64_t>(0, url_config.post_ms >= 0 ? url_config.post_ms : impl_->config.default_post_ms);

  url_buffer->pre_us = pre_ms * 1000;
  url_buffer->post_us = post_ms * 1000;
  url_buffer->only_front = url_config.only_front;
  url_buffer->only_back = url_config.only_back;
  url_buffer->getter_semantics = (info.type & (kGetter | kSetter)) != 0;
  url_buffer->max_packet_size =
      url_config.max_packet_size >= 0 ? url_config.max_packet_size : impl_->config.default_max_packet_size;
  url_buffer->max_size = url_config.max_size >= 0 ? url_config.max_size : impl_->config.default_max_size;

  const int64_t guard = impl_->config.retention_guard_ms * 1000;
  const int64_t retention_pre = url_buffer->only_back ? 0 : url_buffer->pre_us;
  const int64_t initial_max_post = std::max(impl_->max_post_all_us.load(std::memory_order_relaxed),
                                            url_buffer->only_front ? 0 : url_buffer->post_us);
  url_buffer->retention_us.store(retention_pre + initial_max_post + 2 * guard, std::memory_order_relaxed);

  if (url_buffer->only_front && url_buffer->only_back) {
    VLOG_W("TriggerRecorder: URL has both only_front and only_back, window is empty: ", info.url);
  }

  try {
    auto sub = impl_->raw_sub_factory(info.url, InitType::kWithoutInit);

    if VUNLIKELY (!sub) {
      VLOG_W("TriggerRecorder: raw subscriber factory returned null, URL skipped: ", info.url);
      return nullptr;
    }

    if (url_buffer->getter_semantics) {
      sub->mark_as_getter();
    }

    sub->set_latency_and_lost_enabled(true);
    sub->set_ser_type(info.ser_type, info.schema_type);
    sub->set_discovery_enabled(false);
    sub->set_safety_quit(true);

    if (!impl_->config.dds_ip.empty()) {
      sub->set_property("dds.ip", impl_->config.dds_ip);
    }

    if VUNLIKELY (!sub->init()) {
      VLOG_W("TriggerRecorder: subscriber init failed, URL skipped: ", info.url);
      return nullptr;
    }

    const bool listening = sub->listen([this, weak = std::weak_ptr<UrlBuffer>(url_buffer)](const Bytes& data) {
      auto locked = weak.lock();

      if VLIKELY (locked) {
        handle_data(locked.get(), data);
      }
    });

    if VUNLIKELY (!listening) {
      VLOG_W("TriggerRecorder: subscriber listen failed, URL skipped: ", info.url);
      return nullptr;
    }

    url_buffer->sub = std::move(sub);
  } catch (const std::exception& e) {
    VLOG_W("TriggerRecorder: subscriber setup failed, URL skipped: ", info.url, " (", e.what(), ")");
    return nullptr;
  }

  return url_buffer;
}

std::shared_ptr<TriggerRecorder::RawSub> TriggerRecorder::deactivate_url_buffer(UrlBuffer* url_buffer) {
  std::lock_guard ring_lock(url_buffer->mtx);

  if (url_buffer->sub) {
    url_buffer->final_lost = url_buffer->sub->get_lost();
    url_buffer->final_lost_valid = true;
  }

  url_buffer->dead = true;
  return std::move(url_buffer->sub);
}

void TriggerRecorder::recompute_retention(bool dump_in_flight) {
  int64_t max_post = 0;

  for (auto& entry : impl_->url_buffer_map) {
    const int64_t post = entry.second->only_front ? 0 : entry.second->post_us;

    if (post > max_post) {
      max_post = post;
    }
  }

  impl_->max_post_all_us.store(max_post, std::memory_order_relaxed);

  const int64_t guard = impl_->config.retention_guard_ms * 1000;

  for (auto& entry : impl_->url_buffer_map) {
    UrlBuffer* url_buffer = entry.second.get();
    const int64_t retention_pre = url_buffer->only_back ? 0 : url_buffer->pre_us;
    const int64_t updated = retention_pre + max_post + 2 * guard;

    if (dump_in_flight && updated < url_buffer->retention_us.load(std::memory_order_relaxed)) {
      continue;
    }

    url_buffer->retention_us.store(updated, std::memory_order_relaxed);
  }
}

void TriggerRecorder::handle_discovery(const std::vector<DiscoveryViewer::Info>& list) {
  if VUNLIKELY (!impl_->running.load(std::memory_order_acquire)) {
    return;
  }

  std::vector<const DiscoveryViewer::Info*> pending_infos;
  std::unordered_set<std::string> pending;
  std::unordered_set<std::string> current;
  current.reserve(list.size());

  {
    std::shared_lock lock(impl_->url_buffer_mtx);

    for (const auto& info : list) {
      current.insert(info.url);

      if ((info.type & (kPublisher | kSetter)) == 0) {
        continue;
      }

      if ((!impl_->config.blacklist.empty() && contains_exact(impl_->config.blacklist, info.url)) ||
          (!impl_->config.whitelist.empty() && !contains_exact(impl_->config.whitelist, info.url))) {
        continue;
      }

      auto existing = impl_->url_buffer_map.find(info.url);

      if VLIKELY (existing != impl_->url_buffer_map.end()) {
        const bool getter_semantics = (info.type & (kGetter | kSetter)) != 0;

        if VLIKELY (existing->second->ser_type == info.ser_type && existing->second->schema_type == info.schema_type &&
                    existing->second->getter_semantics == getter_semantics) {
          continue;
        }
      }

      if VUNLIKELY (!pending.insert(info.url).second) {
        continue;
      }

      pending_infos.push_back(&info);
    }
  }

  std::vector<std::shared_ptr<UrlBuffer>> new_buffers;
  std::vector<std::shared_ptr<RawSub>> retired_subscribers;
  new_buffers.reserve(pending_infos.size());

  for (const auto* info : pending_infos) {
    auto url_buffer = build_url_buffer(*info);

    if (url_buffer) {
      new_buffers.push_back(std::move(url_buffer));
    }
  }

  {
    std::unique_lock lock(impl_->url_buffer_mtx);

    const bool dump_in_flight = impl_->dumping.load(std::memory_order_acquire);

    for (auto& url_buffer : new_buffers) {
      std::string url = url_buffer->url;
      auto existing = impl_->url_buffer_map.find(url);

      if (existing != impl_->url_buffer_map.end() && existing->second->ser_type == url_buffer->ser_type &&
          existing->second->schema_type == url_buffer->schema_type &&
          existing->second->getter_semantics == url_buffer->getter_semantics) {
        auto retired = deactivate_url_buffer(url_buffer.get());
        int64_t released_bytes = 0;

        {
          std::lock_guard ring_lock(url_buffer->mtx);
          released_bytes = url_buffer->bytes;
        }

        impl_->global_bytes.fetch_sub(released_bytes, std::memory_order_relaxed);
        retired_subscribers.push_back(std::move(retired));
        continue;
      }

      if (existing == impl_->url_buffer_map.end()) {
        impl_->url_buffer_map.emplace(std::move(url), std::move(url_buffer));
        continue;
      }

      auto old = std::move(existing->second);
      auto retired = deactivate_url_buffer(old.get());
      int64_t released_bytes = 0;
      bool frozen = false;

      {
        std::lock_guard ring_lock(old->mtx);
        released_bytes = old->bytes;
        frozen = old->frozen;
      }

      if (!frozen) {
        impl_->global_bytes.fetch_sub(released_bytes, std::memory_order_relaxed);
      }

      existing->second = std::move(url_buffer);
      retired_subscribers.push_back(std::move(retired));
    }

    if (impl_->config.destroy_on_offline) {
      for (auto iter = impl_->url_buffer_map.begin(); iter != impl_->url_buffer_map.end();) {
        if VUNLIKELY (current.count(iter->first) == 0) {
          auto& url_buffer = iter->second;
          auto retired = deactivate_url_buffer(url_buffer.get());
          int64_t released_bytes = 0;
          bool frozen = false;

          {
            std::lock_guard ring_lock(url_buffer->mtx);
            released_bytes = url_buffer->bytes;
            frozen = url_buffer->frozen;
          }

          if (!frozen) {
            impl_->global_bytes.fetch_sub(released_bytes, std::memory_order_relaxed);
          }

          retired_subscribers.push_back(std::move(retired));
          iter = impl_->url_buffer_map.erase(iter);
        } else {
          ++iter;
        }
      }
    }

    recompute_retention(dump_in_flight);
  }
}

void TriggerRecorder::sweep_evict() {
  const int64_t now = impl_->capture_timer.get();

  std::shared_lock lock(impl_->url_buffer_mtx);

  for (auto& entry : impl_->url_buffer_map) {
    UrlBuffer* url_buffer = entry.second.get();

    std::lock_guard ring_lock(url_buffer->mtx);

    const int64_t horizon = now - url_buffer->retention_us.load(std::memory_order_relaxed);

    while (!url_buffer->ring.empty() && url_buffer->ring.front().capture_ts_us < horizon) {
      const int64_t size = url_buffer->ring.front().size;
      url_buffer->bytes -= size;
      impl_->global_bytes.fetch_sub(size, std::memory_order_relaxed);
      url_buffer->ring.pop_front();
    }
  }
}
// LCOV_EXCL_STOP GCOVR_EXCL_STOP

void TriggerRecorder::finish_dump(const std::shared_ptr<DumpJob>& job) {
  std::lock_guard lifecycle_lock(impl_->lifecycle_mtx);
  finish_dump_locked(job);
}

void TriggerRecorder::finish_dump_locked(const std::shared_ptr<DumpJob>& job) {
  if VUNLIKELY (impl_->active_dump_job != job) {
    return;
  }

  {
    std::unique_lock map_lock(impl_->url_buffer_mtx);

    for (const auto& url_buffer : job->frozen_buffers) {
      int64_t released_bytes = 0;

      {
        std::lock_guard ring_lock(url_buffer->mtx);

        if VUNLIKELY (!url_buffer->frozen) {
          continue;
        }

        url_buffer->frozen = false;

        if (url_buffer->dead) {
          released_bytes = url_buffer->bytes;
        }
      }

      if (released_bytes > 0) {
        impl_->global_bytes.fetch_sub(released_bytes, std::memory_order_relaxed);
      }
    }
  }

  auto pending_plugin = std::move(impl_->pending_trigger_plugin);
  impl_->pending_trigger_plugin.reset();

  if (pending_plugin) {
    apply_trigger_plugin_transition(*pending_plugin);
  }

  impl_->writing.store(false, std::memory_order_release);

  {
    std::unique_lock map_lock(impl_->url_buffer_mtx);
    recompute_retention(false);
    impl_->dumping.store(false, std::memory_order_release);
  }

  impl_->active_dump_job.reset();
}

void TriggerRecorder::do_dump(const std::shared_ptr<DumpJob>& job) {
  struct SnapFrame final {
    int64_t capture_ts_us{0};
    int64_t size{0};
    std::shared_ptr<const Bytes> payload;
    const UrlBuffer* source{nullptr};
  };

  struct LossInfo final {
    std::string url;
    double loss{0.0};
  };

  DumpJob::FinishGuard guard{this, job};

  const auto& params = job->params;
  const auto& path = job->path;
  const auto& trigger_plugin = job->trigger_plugin;
  const auto& bag_plugin = job->bag_plugin;
  const int64_t trigger_ts = job->trigger_ts;
  const int64_t dump_start = impl_->capture_timer.get();

  const int64_t trigger_pre_us = params.pre_ms >= 0 ? params.pre_ms * 1000 : -1;
  const int64_t trigger_post_us = params.post_ms >= 0 ? params.post_ms * 1000 : -1;

  std::vector<SnapFrame> snapshot;
  std::vector<LossInfo> losses;

  // LCOV_EXCL_START GCOVR_EXCL_START
  auto snapshot_url_buffer = [&snapshot, &losses, trigger_ts, trigger_pre_us, trigger_post_us](UrlBuffer* url_buffer) {
    if (url_buffer->only_front && url_buffer->only_back) {
      return;
    }

    std::lock_guard ring_lock(url_buffer->mtx);

    SampleLostInfo current_lost = url_buffer->last_lost;

    if (url_buffer->sub) {
      current_lost = url_buffer->sub->get_lost();
    } else if (url_buffer->final_lost_valid) {
      current_lost = url_buffer->final_lost;
    }

    const uint64_t delta_total = current_lost.total - url_buffer->last_lost.total;
    const uint64_t delta_lost = current_lost.lost - url_buffer->last_lost.lost;
    const double loss = delta_total > 0 ? static_cast<double>(delta_lost) / static_cast<double>(delta_total) : 0.0;
    url_buffer->last_lost = current_lost;

    int64_t pre = url_buffer->pre_us;
    int64_t post = url_buffer->only_front ? 0 : url_buffer->post_us;

    if (trigger_pre_us >= 0 && trigger_pre_us < pre) {
      pre = trigger_pre_us;
    }

    if (trigger_post_us >= 0 && trigger_post_us < post) {
      post = trigger_post_us;
    }

    int64_t window_begin = trigger_ts - pre;
    int64_t window_end = trigger_ts + post;

    if (url_buffer->only_front) {
      window_end = trigger_ts;
    }

    if (url_buffer->only_back) {
      window_begin = trigger_ts;
    }

    for (const auto& entry : url_buffer->ring) {
      if (entry.capture_ts_us >= window_begin && entry.capture_ts_us <= window_end) {
        snapshot.push_back(SnapFrame{entry.capture_ts_us, entry.size, entry.payload, url_buffer});
      }
    }

    losses.push_back(LossInfo{url_buffer->url, loss});
  };

  const std::vector<std::string> lower_patterns = lower_trigger_patterns(params.filter_str);

  for (const auto& url_buffer : job->frozen_buffers) {
    if (!accept_trigger(url_buffer->url, params, lower_patterns)) {
      continue;
    }

    snapshot_url_buffer(url_buffer.get());
  }
  // LCOV_EXCL_STOP GCOVR_EXCL_STOP

  if (trigger_plugin) {
    TriggerPluginInterface::TriggerContext context;
    context.reason = params.reason;
    context.name_hint = params.name_hint;
    context.out_file = params.out_file;
    context.pre_ms = params.pre_ms;
    context.post_ms = params.post_ms;
    context.trigger_timestamp = impl_->anchor_wall_us + trigger_ts;

    trigger_plugin->on_trigger(context);
  }

  if (params.out_file.empty()) {
    std::vector<std::string> removed;

    if (impl_->config.max_dump_file_count > 0) {
      std::error_code ec;
      std::vector<std::filesystem::directory_entry> files;
      std::filesystem::directory_iterator iter(impl_->config.dump_dir, ec);

      if (!ec) {
        for (const std::filesystem::directory_iterator end; iter != end; iter.increment(ec)) {
          std::error_code type_ec;

          if (iter->is_regular_file(type_ec) && iter->path().extension() == impl_->file_suffix) {
            files.push_back(*iter);
          }
        }

        std::sort(files.begin(), files.end(),
                  [](const std::filesystem::directory_entry& left, const std::filesystem::directory_entry& right) {
                    std::error_code left_error;
                    std::error_code right_error;
                    return std::filesystem::last_write_time(left, left_error) <
                           std::filesystem::last_write_time(right, right_error);
                  });

        while (static_cast<int>(files.size()) >= impl_->config.max_dump_file_count) {
          std::error_code remove_error;

          if (std::filesystem::remove(files.front().path(), remove_error) && !remove_error) {
            removed.push_back(files.front().path().string());
          }

          files.erase(files.begin());
        }
      }
    }

    for (const auto& removed_path : removed) {
      if (trigger_plugin) {
        trigger_plugin->on_file_rotated(removed_path);
      }
    }
  }

  int64_t min_capture = trigger_ts;

  for (const auto& frame : snapshot) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    if (frame.capture_ts_us < min_capture) {
      min_capture = frame.capture_ts_us;
    }
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  std::stable_sort(snapshot.begin(), snapshot.end(), [](const SnapFrame& left, const SnapFrame& right) {
    return left.capture_ts_us < right.capture_ts_us;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  });

  BagWriter::Config writer_config;
  writer_config.compress = impl_->config.enable_compress ? BagWriter::kCompressAuto : BagWriter::kCompressNone;
  writer_config.tag_name = params.reason;
  writer_config.sync_mode = true;
  writer_config.optimize_on_exit = true;
  writer_config.start_timestamp = (impl_->anchor_wall_us + min_capture) / 1000;

  std::shared_ptr<BagWriter> writer;

  try {
    writer = BagWriter::create(path, writer_config);
  } catch (const std::exception& e) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    VLOG_E("TriggerRecorder: failed to create bag writer at ", path, ": ", e.what());
    notify_dump_failed(trigger_plugin, params.reason, path, e.what());
    return;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  if VUNLIKELY (!writer) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    VLOG_E("TriggerRecorder: unsupported bag suffix, dump aborted: ", path);
    notify_dump_failed(trigger_plugin, params.reason, path, "unsupported bag suffix");
    return;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  if VUNLIKELY (!writer->async_run()) {
    notify_dump_failed(trigger_plugin, params.reason, path, "writer loop failed to start");
    return;
  }

  if (bag_plugin) {
    writer->bind_plugin_interface(bag_plugin);
  }

  for (const auto& info : losses) {
    writer->set_url_loss(info.url, info.loss);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  TriggerPluginInterface::DumpContext dump_context;
  dump_context.reason = params.reason;
  dump_context.path = path;
  dump_context.start_timestamp = writer_config.start_timestamp;
  dump_context.url_count = static_cast<int64_t>(losses.size());

  impl_->writing.store(true, std::memory_order_release);

  if (trigger_plugin) {
    trigger_plugin->on_dump_started(dump_context);
  }

  int64_t throttle_bytes = 0;
  int64_t dropped = 0;
  int64_t byte_count = 0;

  for (const auto& item : snapshot) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    Frame frame;
    frame.timestamp = item.capture_ts_us - min_capture;
    frame.url = item.source->url;
    frame.ser_type = item.source->ser_type;
    frame.schema_type = item.source->schema_type;
    frame.action_type = ActionType::kSubscribe;
    frame.data = Bytes::shallow_copy(item.payload->data(), item.size);

    if VUNLIKELY (writer->push(frame, false) < 0) {
      ++dropped;
      continue;
    }

    byte_count += item.size;

    if (trigger_plugin) {
      trigger_plugin->on_frame(frame, dump_context);
    }

    if (impl_->config.sleep_time_ms > 0 && impl_->config.sleep_interval > 0) {
      throttle_bytes += item.size;

      if (throttle_bytes >= impl_->config.sleep_interval) {
        std::this_thread::sleep_for(std::chrono::milliseconds(impl_->config.sleep_time_ms));
        throttle_bytes = 0;
      }
    }
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  if (bag_plugin) {
    writer->clear_plugin_interface();
  }

  bool drained = false;
  const int64_t drain_deadline = impl_->capture_timer.get() + 60'000'000;

  while (!(drained = writer->wait_for_idle(50))) {
    if VUNLIKELY (impl_->capture_timer.get() >= drain_deadline) {
      break;
    }
  }

  writer->quit(!drained);
  writer->wait_for_quit();
  writer->close();
  impl_->writing.store(false, std::memory_order_release);

  const bool writer_failed = writer->fail();

  writer.reset();

  if VUNLIKELY (dropped > 0) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    VLOG_W("TriggerRecorder: writer dropped ", dropped, " frame(s) due to queue overflow");
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  if VUNLIKELY (!drained) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    VLOG_E("TriggerRecorder: writer drain timeout, dump may be incomplete: ", path);
    notify_dump_failed(trigger_plugin, params.reason, path, "writer drain timeout");
    return;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  if VUNLIKELY (writer_failed) {
    VLOG_E("TriggerRecorder: writer reported a persistence failure: ", path);
    notify_dump_failed(trigger_plugin, params.reason, path, "writer persistence failure");
    return;
  }

  if (trigger_plugin) {
    TriggerPluginInterface::DumpResult result;
    result.reason = params.reason;
    result.path = path;
    result.frame_count = static_cast<int64_t>(snapshot.size()) - dropped;
    result.dropped_count = dropped;
    result.byte_count = byte_count;
    result.url_count = static_cast<int64_t>(losses.size());
    result.start_timestamp = writer_config.start_timestamp;
    result.duration_us = impl_->capture_timer.get() - dump_start;
    result.success = true;

    trigger_plugin->on_dump_finished(result);
  }

  VLOG_I("TriggerRecorder: dump finished -> ", path, " frames=", snapshot.size());
}

}  // namespace vlink
