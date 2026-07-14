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
#include <shared_mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "./base/elapsed_timer.h"
#include "./base/helpers.h"
#include "./base/logger.h"
#include "./base/timer.h"
#include "./base/utils.h"
#include "./extension/bag_writer.h"
#include "./extension/trigger_plugin_interface.h"

namespace vlink {

// TriggerRecorder::UrlBuffer
struct TriggerRecorder::UrlBuffer final {
  struct Entry final {
    int64_t capture_ts_us{0};
    std::shared_ptr<const Bytes> payload;
  };

  std::mutex mtx;
  std::deque<Entry> ring;
  int64_t bytes{0};
  int64_t pre_us{0};
  int64_t post_us{0};
  std::atomic<int64_t> retention_us{0};
  int64_t max_packet_size{0};
  int64_t max_size{0};

  std::string url;
  std::string ser_type;
  SampleLostInfo last_lost;
  SampleLostInfo final_lost;
  SchemaType schema_type{SchemaType::kUnknown};
  bool frozen{false};
  bool disabled{false};
  bool getter_semantics{false};

  std::shared_ptr<RawSub> sub;
};

// TriggerRecorder::DumpJob
struct TriggerRecorder::DumpJob final {
  int64_t trigger_ts{0};
  TriggerParams params;
  std::string path;
  std::vector<std::shared_ptr<UrlBuffer>> frozen_buffers;
};

// TriggerRecorder::Impl
struct TriggerRecorder::Impl final {  // NOLINT(clang-analyzer-optin.performance.Padding)
  Config config;
  RawSubFactory raw_sub_factory;
  ElapsedTimer capture_timer{ElapsedTimer::kCpuTimestamp, ElapsedTimer::kMicro};
  int64_t anchor_wall_us{0};

  std::atomic<int64_t> max_post_all_us{0};
  std::atomic<int64_t> global_bytes{0};
  std::atomic_bool dumping{false};
  std::atomic_bool writing{false};
  std::atomic_bool running{false};

  std::mutex lifecycle_mtx;
  std::shared_mutex url_buffer_mtx;

  std::unordered_map<std::string, std::shared_ptr<UrlBuffer>> url_buffer_map;
  std::shared_ptr<DumpJob> active_dump_job;

  std::unique_ptr<DiscoveryViewer> viewer;
  Timer sweep_timer;

  std::shared_ptr<TriggerPluginInterface> trigger_plugin;
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

  impl_->config.default_pre_ms = std::max<int64_t>(0, impl_->config.default_pre_ms);
  impl_->config.default_post_ms = std::max<int64_t>(0, impl_->config.default_post_ms);
  impl_->config.retention_guard_ms = std::max<int64_t>(0, impl_->config.retention_guard_ms);

  if VUNLIKELY (impl_->config.default_pre_ms > kMaxWindowMs || impl_->config.default_post_ms > kMaxWindowMs ||
                impl_->config.retention_guard_ms > kMaxWindowMs) {
    VLOG_F("TriggerRecorder: pre/post/retention_guard exceeds the supported range");
  }

  if VUNLIKELY (impl_->config.default_post_ms + impl_->config.retention_guard_ms >
                std::numeric_limits<uint32_t>::max()) {
    VLOG_F("TriggerRecorder: default post window plus retention guard exceeds the timer range");
  }

  if VUNLIKELY (impl_->config.default_max_packet_size < 0 || impl_->config.default_max_size < 0 ||
                impl_->config.max_cache_size < 0 || impl_->config.sleep_interval < 0 ||
                impl_->config.sleep_time_ms < 0) {
    VLOG_F("TriggerRecorder: byte limits and sleep values must be non-negative");
  }

  if VUNLIKELY (impl_->config.file_type != kVdb && impl_->config.file_type != kVcap) {
    VLOG_F("TriggerRecorder: unsupported file type");
  }

  if VUNLIKELY (impl_->config.overflow != kCoverOldest && impl_->config.overflow != kDropNewest) {
    VLOG_F("TriggerRecorder: unsupported overflow policy");
  }

  for (const auto& [url, value] : impl_->config.url_overrides) {
    if VUNLIKELY (value.pre_ms > kMaxWindowMs || value.post_ms > kMaxWindowMs ||
                  (value.post_ms >= 0 &&
                   value.post_ms + impl_->config.retention_guard_ms > std::numeric_limits<uint32_t>::max())) {
      VLOG_F("TriggerRecorder: URL window exceeds the supported range: ", url);
    }
  }

  std::error_code ec;
  std::filesystem::create_directories(impl_->config.dump_dir, ec);

  std::error_code query_ec;

  if VUNLIKELY (ec && !std::filesystem::is_directory(impl_->config.dump_dir, query_ec)) {
    VLOG_F("TriggerRecorder: cannot create dump_dir '", impl_->config.dump_dir,
           "': ", ec.message());  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  impl_->viewer = std::make_unique<DiscoveryViewer>(impl_->config.discovery_filter);
  impl_->viewer->register_callback([this](const std::vector<DiscoveryViewer::Info>& list) { handle_discovery(list); });
}

TriggerRecorder::~TriggerRecorder() {
  quit(true);
  wait_for_quit();
}

bool TriggerRecorder::dump(const TriggerParams& params) {
  if VUNLIKELY (params.pre_ms > kMaxWindowMs || params.post_ms > kMaxWindowMs) {
    VLOG_E("TriggerRecorder: trigger window exceeds the supported range");
    return false;
  }

  std::lock_guard lifecycle_lock(impl_->lifecycle_mtx);

  if VUNLIKELY (!impl_->running.load(std::memory_order_acquire) || impl_->dumping.load(std::memory_order_acquire)) {
    return false;
  }

  auto job = std::make_shared<DumpJob>();
  job->params = params;

  if (!params.out_file.empty()) {
    job->path = params.out_file;
  } else {
    const char* file_suffix = impl_->config.file_type == kVcap ? ".vcap" : ".vdb";
    std::string base = params.name_hint.empty() ? BagWriter::get_format_date(nullptr, true) : params.name_hint;
    std::replace(base.begin(), base.end(), '/', '_');
    std::replace(base.begin(), base.end(), '\\', '_');
    job->path = impl_->config.dump_dir + "/" + base + file_suffix;
    uint64_t suffix = 1;
    std::error_code ec;

    while (std::filesystem::exists(job->path, ec)) {
      job->path = impl_->config.dump_dir + "/" + base + "_" + std::to_string(suffix++) + file_suffix;
    }
  }

  int64_t max_post;

  {
    std::shared_lock map_lock(impl_->url_buffer_mtx);
    job->frozen_buffers.reserve(impl_->url_buffer_map.size());
    job->trigger_ts = impl_->capture_timer.get();
    max_post = impl_->max_post_all_us.load(std::memory_order_relaxed);

    for (const auto& entry : impl_->url_buffer_map) {
      if (entry.second->disabled) {
        continue;
      }

      entry.second->frozen = true;
      job->frozen_buffers.push_back(entry.second);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    impl_->active_dump_job = job;
    impl_->dumping.store(true, std::memory_order_release);
  }

  const int64_t delay_ms = max_post > 0 ? max_post / 1000 + impl_->config.retention_guard_ms : 0;

  auto task = [this, weak_job = std::weak_ptr<DumpJob>(job)]() {
    if (auto locked_job = weak_job.lock()) {
      do_dump(*locked_job);
    }
  };

  const bool ok = delay_ms > 0 ? Timer::call_once(this, static_cast<uint32_t>(delay_ms), std::move(task))
                               : post_task(std::move(task));

  if VUNLIKELY (!ok) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    finish_dump_locked(*job);

    return false;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  return true;
}

bool TriggerRecorder::is_dumping() const noexcept { return impl_->dumping.load(std::memory_order_acquire); }

void TriggerRecorder::bind_trigger_interface(const std::shared_ptr<TriggerPluginInterface>& trigger_interface) {
  std::lock_guard lifecycle_lock(impl_->lifecycle_mtx);

  if VUNLIKELY (impl_->running.load(std::memory_order_acquire)) {
    VLOG_E("TriggerRecorder: trigger plugin must be bound while the recorder is stopped");
    return;
  }

  impl_->trigger_plugin = trigger_interface;
}

void TriggerRecorder::clear_trigger_interface() { bind_trigger_interface(nullptr); }

void TriggerRecorder::bind_bag_interface(const std::shared_ptr<BagPluginInterface>& bag_interface) {
  std::lock_guard lock(impl_->lifecycle_mtx);

  if VUNLIKELY (impl_->running.load(std::memory_order_acquire)) {
    VLOG_E("TriggerRecorder: bag plugin must be bound while the recorder is stopped");
    return;
  }

  impl_->bag_plugin = bag_interface;
}

void TriggerRecorder::clear_bag_interface() { bind_bag_interface(nullptr); }

void TriggerRecorder::on_begin() {
  MessageLoop::on_begin();

  std::lock_guard lifecycle_lock(impl_->lifecycle_mtx);

  if (!impl_->viewer) {
    try {
      impl_->viewer = std::make_unique<DiscoveryViewer>(impl_->config.discovery_filter);
      impl_->viewer->register_callback(
          [this](const std::vector<DiscoveryViewer::Info>& list) { handle_discovery(list); });
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

  impl_->sweep_timer.start([this]() { sweep_evict(); });

  impl_->running.store(true, std::memory_order_release);

  if (impl_->trigger_plugin) {
    impl_->trigger_plugin->on_start();
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

  impl_->dumping.store(false, std::memory_order_release);
  auto abandoned_job = std::move(impl_->active_dump_job);

  std::vector<std::shared_ptr<RawSub>> retired_subscribers;

  {
    std::unique_lock lock(impl_->url_buffer_mtx);
    retired_subscribers.reserve(impl_->url_buffer_map.size());

    for (auto& entry : impl_->url_buffer_map) {
      retired_subscribers.push_back(deactivate_url_buffer(*entry.second));
    }

    impl_->url_buffer_map.clear();
    impl_->global_bytes.store(0, std::memory_order_relaxed);
    impl_->max_post_all_us.store(0, std::memory_order_relaxed);
  }

  retired_subscribers.clear();

  if (recorder_started) {
    if VUNLIKELY (abandoned_job) {
      notify_dump_failed(*abandoned_job, "dump abandoned at shutdown");
    }

    if (impl_->trigger_plugin) {
      impl_->trigger_plugin->flush();
      impl_->trigger_plugin->on_stop();
    }
  }

  lifecycle_lock.unlock();

  MessageLoop::on_end();
}

// LCOV_EXCL_START GCOVR_EXCL_START
void TriggerRecorder::handle_data(UrlBuffer& url_buffer, const Bytes& data) {
  if VUNLIKELY (!impl_->running.load(std::memory_order_relaxed) || url_buffer.disabled) {
    return;
  }

  if VUNLIKELY (impl_->config.busy_skip_data && impl_->writing.load(std::memory_order_relaxed)) {
    return;
  }

  const size_t data_size = data.size();

  if VUNLIKELY (data_size > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
    return;
  }

  const auto incoming = static_cast<int64_t>(data_size);

  if VUNLIKELY (url_buffer.max_packet_size > 0 && incoming > url_buffer.max_packet_size) {
    return;
  }

  if VUNLIKELY (url_buffer.max_size > 0 && incoming > url_buffer.max_size) {
    return;
  }

  if VUNLIKELY (impl_->config.max_cache_size > 0 && incoming > impl_->config.max_cache_size) {
    return;
  }

  auto payload = std::make_shared<const Bytes>(Bytes::deep_copy(data.data(), data_size));

  if VUNLIKELY (payload->size() != data_size) {
    return;
  }

  std::lock_guard lock(url_buffer.mtx);

  if VUNLIKELY (!url_buffer.sub) {
    return;
  }

  const int64_t capture_ts = impl_->capture_timer.get();
  const int64_t horizon = capture_ts - url_buffer.retention_us.load(std::memory_order_relaxed);

  while (!url_buffer.ring.empty() && url_buffer.ring.front().capture_ts_us < horizon) {
    const auto size = static_cast<int64_t>(url_buffer.ring.front().payload->size());
    url_buffer.bytes -= size;
    impl_->global_bytes.fetch_sub(size, std::memory_order_relaxed);
    url_buffer.ring.pop_front();
  }

  const size_t evictable_count = url_buffer.ring.size();
  size_t evict_count = 0;
  int64_t evict_bytes = 0;
  int64_t local_need = 0;

  if (url_buffer.max_size > 0 && url_buffer.bytes > url_buffer.max_size - incoming) {
    local_need = url_buffer.bytes - (url_buffer.max_size - incoming);
  }

  if (local_need > 0 && impl_->config.overflow == kDropNewest) {
    return;
  }

  url_buffer.ring.push_back(UrlBuffer::Entry{capture_ts, std::move(payload)});

  int64_t global = impl_->global_bytes.load(std::memory_order_relaxed);

  while (true) {
    int64_t global_need = 0;

    if (impl_->config.max_cache_size > 0 && global > impl_->config.max_cache_size - incoming) {
      global_need = global - (impl_->config.max_cache_size - incoming);

      if (impl_->config.overflow == kDropNewest) {
        url_buffer.ring.pop_back();
        return;
      }
    }

    const int64_t need = std::max(local_need, global_need);

    while (evict_count < evictable_count && evict_bytes < need) {
      evict_bytes += static_cast<int64_t>(url_buffer.ring[evict_count].payload->size());
      ++evict_count;
    }

    while (evict_count > 0 &&
           evict_bytes - static_cast<int64_t>(url_buffer.ring[evict_count - 1].payload->size()) >= need) {
      evict_bytes -= static_cast<int64_t>(url_buffer.ring[evict_count - 1].payload->size());
      --evict_count;
    }

    if (evict_bytes < need || evict_bytes > global) {
      url_buffer.ring.pop_back();
      return;
    }

    if VUNLIKELY (global - evict_bytes > std::numeric_limits<int64_t>::max() - incoming) {
      url_buffer.ring.pop_back();
      return;
    }

    const int64_t desired = global - evict_bytes + incoming;

    if (impl_->global_bytes.compare_exchange_weak(global, desired, std::memory_order_relaxed,
                                                  std::memory_order_relaxed)) {
      break;
    }
  }

  for (size_t index = 0; index < evict_count; ++index) {
    url_buffer.ring.pop_front();
  }

  url_buffer.bytes = url_buffer.bytes - evict_bytes + incoming;
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

  const int64_t pre_ms = url_config.pre_ms >= 0 ? url_config.pre_ms : impl_->config.default_pre_ms;
  const int64_t post_ms = url_config.post_ms >= 0 ? url_config.post_ms : impl_->config.default_post_ms;

  url_buffer->pre_us = url_config.only_back ? 0 : pre_ms * 1000;
  url_buffer->post_us = url_config.only_front ? 0 : post_ms * 1000;
  url_buffer->disabled = url_config.only_front && url_config.only_back;
  url_buffer->getter_semantics = (info.type & (kGetter | kSetter)) != 0;
  url_buffer->max_packet_size =
      url_config.max_packet_size >= 0 ? url_config.max_packet_size : impl_->config.default_max_packet_size;
  url_buffer->max_size = url_config.max_size >= 0 ? url_config.max_size : impl_->config.default_max_size;

  const int64_t guard = impl_->config.retention_guard_ms * 1000;
  const int64_t initial_max_post =
      std::max(impl_->max_post_all_us.load(std::memory_order_relaxed), url_buffer->post_us);
  url_buffer->retention_us.store(url_buffer->pre_us + initial_max_post + 2 * guard, std::memory_order_relaxed);

  if (url_buffer->disabled) {
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

    url_buffer->sub = std::move(sub);

    if VUNLIKELY (!url_buffer->sub->listen([this, weak = std::weak_ptr<UrlBuffer>(url_buffer)](const Bytes& data) {
                    if (auto locked = weak.lock()) {
                      handle_data(*locked, data);
                    }
                  })) {
      deactivate_url_buffer(*url_buffer);
      VLOG_W("TriggerRecorder: subscriber listen failed, URL skipped: ", info.url);
      return nullptr;
    }
  } catch (const std::exception& e) {
    if (url_buffer->sub) {
      deactivate_url_buffer(*url_buffer);
    }

    VLOG_W("TriggerRecorder: subscriber setup failed, URL skipped: ", info.url, " (", e.what(), ")");
    return nullptr;
  }

  return url_buffer;
}

std::shared_ptr<TriggerRecorder::RawSub> TriggerRecorder::deactivate_url_buffer(UrlBuffer& url_buffer) {
  std::lock_guard ring_lock(url_buffer.mtx);

  if (url_buffer.frozen) {
    url_buffer.final_lost = url_buffer.sub->get_lost();
  } else if (url_buffer.bytes > 0) {
    impl_->global_bytes.fetch_sub(url_buffer.bytes, std::memory_order_relaxed);
  }

  return std::move(url_buffer.sub);
}

void TriggerRecorder::recompute_retention() {
  int64_t max_post = 0;

  for (const auto& entry : impl_->url_buffer_map) {
    max_post = std::max(max_post, entry.second->post_us);
  }

  impl_->max_post_all_us.store(max_post, std::memory_order_relaxed);

  const int64_t guard = impl_->config.retention_guard_ms * 1000;

  for (const auto& entry : impl_->url_buffer_map) {
    UrlBuffer* url_buffer = entry.second.get();
    const int64_t updated = url_buffer->pre_us + max_post + 2 * guard;

    if (url_buffer->frozen && updated < url_buffer->retention_us.load(std::memory_order_relaxed)) {
      continue;
    }

    url_buffer->retention_us.store(updated, std::memory_order_relaxed);
  }
}

void TriggerRecorder::handle_discovery(const std::vector<DiscoveryViewer::Info>& list) {
  if VUNLIKELY (!impl_->running.load(std::memory_order_acquire)) {
    return;
  }

  std::vector<std::shared_ptr<UrlBuffer>> new_buffers;
  std::unordered_set<std::string_view> current;

  if (impl_->config.destroy_on_offline) {
    current.reserve(list.size());
  }

  for (const auto& info : list) {
    if (impl_->config.destroy_on_offline) {
      current.insert(info.url);
    }

    if ((info.type & (kPublisher | kSetter)) == 0) {
      continue;
    }

    if ((std::find(impl_->config.blacklist.begin(), impl_->config.blacklist.end(), info.url) !=
         impl_->config.blacklist.end()) ||
        (!impl_->config.whitelist.empty() && std::find(impl_->config.whitelist.begin(), impl_->config.whitelist.end(),
                                                       info.url) == impl_->config.whitelist.end())) {
      continue;
    }

    auto existing = impl_->url_buffer_map.find(info.url);

    if VLIKELY (existing != impl_->url_buffer_map.end()) {
      if VLIKELY (existing->second->ser_type == info.ser_type && existing->second->schema_type == info.schema_type &&
                  existing->second->getter_semantics == ((info.type & (kGetter | kSetter)) != 0)) {
        continue;
      }
    }

    auto url_buffer = build_url_buffer(info);

    if (url_buffer) {
      new_buffers.push_back(std::move(url_buffer));
    }
  }

  if (new_buffers.empty() && !impl_->config.destroy_on_offline) {
    return;
  }

  std::vector<std::shared_ptr<RawSub>> retired_subscribers;

  {
    std::unique_lock lock(impl_->url_buffer_mtx);
    bool retention_changed = false;

    for (auto& url_buffer : new_buffers) {
      std::string url = url_buffer->url;
      auto existing = impl_->url_buffer_map.find(url);

      if (existing == impl_->url_buffer_map.end()) {
        impl_->url_buffer_map.emplace(std::move(url), std::move(url_buffer));
        retention_changed = true;
        continue;
      }

      retired_subscribers.push_back(deactivate_url_buffer(*existing->second));
      existing->second = std::move(url_buffer);
    }

    if (impl_->config.destroy_on_offline) {
      for (auto iter = impl_->url_buffer_map.begin(); iter != impl_->url_buffer_map.end();) {
        if VUNLIKELY (current.count(std::string_view(iter->first)) == 0) {
          retired_subscribers.push_back(deactivate_url_buffer(*iter->second));
          iter = impl_->url_buffer_map.erase(iter);
          retention_changed = true;
        } else {
          ++iter;
        }
      }
    }

    if (retention_changed) {
      recompute_retention();
    }
  }
}

void TriggerRecorder::sweep_evict() {
  const int64_t now = impl_->capture_timer.get();

  std::shared_lock lock(impl_->url_buffer_mtx);

  for (const auto& entry : impl_->url_buffer_map) {
    UrlBuffer* url_buffer = entry.second.get();

    std::lock_guard ring_lock(url_buffer->mtx);

    const int64_t horizon = now - url_buffer->retention_us.load(std::memory_order_relaxed);

    while (!url_buffer->ring.empty() && url_buffer->ring.front().capture_ts_us < horizon) {
      const auto size = static_cast<int64_t>(url_buffer->ring.front().payload->size());
      url_buffer->bytes -= size;
      impl_->global_bytes.fetch_sub(size, std::memory_order_relaxed);
      url_buffer->ring.pop_front();
    }
  }
}
// LCOV_EXCL_STOP GCOVR_EXCL_STOP

void TriggerRecorder::finish_dump(DumpJob& job) {
  std::lock_guard lifecycle_lock(impl_->lifecycle_mtx);
  finish_dump_locked(job);
}

void TriggerRecorder::finish_dump_locked(DumpJob& job) {
  if VUNLIKELY (impl_->active_dump_job.get() != &job) {
    return;
  }

  std::shared_lock map_lock(impl_->url_buffer_mtx);

  for (const auto& url_buffer : job.frozen_buffers) {
    std::lock_guard ring_lock(url_buffer->mtx);
    url_buffer->frozen = false;

    if (!url_buffer->sub && url_buffer->bytes > 0) {
      impl_->global_bytes.fetch_sub(url_buffer->bytes, std::memory_order_relaxed);
    }
  }

  impl_->writing.store(false, std::memory_order_release);
  recompute_retention();
  impl_->dumping.store(false, std::memory_order_release);
  impl_->active_dump_job.reset();
}

// LCOV_EXCL_START GCOVR_EXCL_START
void TriggerRecorder::notify_dump_failed(const DumpJob& job, std::string_view error) {
  if (!impl_->trigger_plugin) {
    return;
  }

  TriggerPluginInterface::DumpResult result;
  result.reason = job.params.reason;
  result.path = job.path;
  result.error = error;
  impl_->trigger_plugin->on_dump_failed(result);
}
// LCOV_EXCL_STOP GCOVR_EXCL_STOP

void TriggerRecorder::do_dump(DumpJob& job) {
  struct FinishGuard final {
    TriggerRecorder& recorder;
    DumpJob& job;

    ~FinishGuard() { recorder.finish_dump(job); }
  };

  struct SnapFrame final {
    int64_t capture_ts_us;
    std::shared_ptr<const Bytes> payload;
    const UrlBuffer* source;
  };

  struct LossInfo final {
    const UrlBuffer* source;
    double loss;
  };

  FinishGuard guard{*this, job};

  const auto& params = job.params;
  const auto& path = job.path;
  auto* trigger_plugin = impl_->trigger_plugin.get();
  const int64_t trigger_ts = job.trigger_ts;
  const int64_t dump_start = impl_->capture_timer.get();

  const int64_t trigger_pre_us = params.pre_ms >= 0 ? params.pre_ms * 1000 : -1;
  const int64_t trigger_post_us = params.post_ms >= 0 ? params.post_ms * 1000 : -1;

  std::vector<SnapFrame> snapshot;
  std::vector<LossInfo> losses;
  losses.reserve(job.frozen_buffers.size());

  // LCOV_EXCL_START GCOVR_EXCL_START
  std::vector<std::string> lower_patterns;

  if (params.filter_urls.empty() && !params.filter_str.empty()) {
    lower_patterns = Helpers::split_any(params.filter_str);

    for (auto& pattern : lower_patterns) {
      std::transform(pattern.begin(), pattern.end(), pattern.begin(), [](unsigned char c) { return std::tolower(c); });
    }
  }

  for (const auto& url_buffer : job.frozen_buffers) {
    if (!params.filter_urls.empty()) {
      if (params.filter_urls.count(url_buffer->url) == 0) {
        continue;
      }
    } else if (!params.filter_str.empty()) {
      std::string lower_url = url_buffer->url;
      std::transform(lower_url.begin(), lower_url.end(), lower_url.begin(),
                     [](unsigned char c) { return std::tolower(c); });

      const bool hit = std::any_of(lower_patterns.begin(), lower_patterns.end(), [&lower_url](const auto& pattern) {
        return lower_url.find(pattern) != std::string::npos;
      });

      if (params.black_mode == hit) {
        continue;
      }
    }

    std::lock_guard ring_lock(url_buffer->mtx);
    SampleLostInfo current_lost = url_buffer->sub ? url_buffer->sub->get_lost() : url_buffer->final_lost;
    const uint64_t delta_total = current_lost.total - url_buffer->last_lost.total;
    const uint64_t delta_lost = current_lost.lost - url_buffer->last_lost.lost;
    const double loss = delta_total > 0 ? static_cast<double>(delta_lost) / static_cast<double>(delta_total) : 0.0;
    url_buffer->last_lost = current_lost;

    const int64_t pre = trigger_pre_us >= 0 ? std::min(trigger_pre_us, url_buffer->pre_us) : url_buffer->pre_us;
    const int64_t post = trigger_post_us >= 0 ? std::min(trigger_post_us, url_buffer->post_us) : url_buffer->post_us;

    const int64_t window_begin = trigger_ts - pre;
    const int64_t window_end = trigger_ts + post;

    auto first = std::lower_bound(
        url_buffer->ring.begin(), url_buffer->ring.end(), window_begin,
        [](const UrlBuffer::Entry& entry, int64_t timestamp) { return entry.capture_ts_us < timestamp; });

    for (; first != url_buffer->ring.end() && first->capture_ts_us <= window_end; ++first) {
      snapshot.push_back(SnapFrame{first->capture_ts_us, first->payload, url_buffer.get()});
    }

    losses.push_back(LossInfo{url_buffer.get(), loss});
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

  if (params.out_file.empty() && impl_->config.max_dump_file_count > 0) {
    const auto file_extension = std::filesystem::path(path).extension();
    std::vector<std::string> removed;
    std::error_code ec;
    std::vector<std::filesystem::directory_entry> files;
    std::filesystem::directory_iterator iter(impl_->config.dump_dir, ec);

    if (!ec) {
      for (const std::filesystem::directory_iterator end; iter != end; iter.increment(ec)) {
        std::error_code type_ec;

        if (iter->is_regular_file(type_ec) && iter->path().extension() == file_extension) {
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

      const auto max_count = static_cast<size_t>(impl_->config.max_dump_file_count);
      const size_t remove_count = files.size() >= max_count ? files.size() - max_count + 1 : 0;

      for (size_t index = 0; index < remove_count; ++index) {
        std::error_code remove_error;

        if (std::filesystem::remove(files[index].path(), remove_error) && trigger_plugin) {
          removed.push_back(files[index].path().string());
        }
      }
    }

    if (trigger_plugin) {
      for (const auto& removed_path : removed) {
        trigger_plugin->on_file_rotated(removed_path);
      }
    }
  }

  std::stable_sort(snapshot.begin(), snapshot.end(), [](const SnapFrame& left, const SnapFrame& right) {
    return left.capture_ts_us < right.capture_ts_us;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  });

  const int64_t min_capture = snapshot.empty() ? trigger_ts : std::min(trigger_ts, snapshot.front().capture_ts_us);

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
    notify_dump_failed(job, e.what());
    return;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  if VUNLIKELY (!writer) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    VLOG_E("TriggerRecorder: unsupported bag suffix, dump aborted: ", path);
    notify_dump_failed(job, "unsupported bag suffix");
    return;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  if (impl_->bag_plugin) {
    writer->bind_bag_interface(impl_->bag_plugin);
  }

  for (const auto& info : losses) {
    writer->set_url_loss(info.source->url, info.loss);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  TriggerPluginInterface::DumpContext dump_context;

  impl_->writing.store(true, std::memory_order_release);

  if (trigger_plugin) {
    dump_context.reason = params.reason;
    dump_context.path = path;
    dump_context.start_timestamp = writer_config.start_timestamp;
    dump_context.url_count = static_cast<int64_t>(losses.size());
    trigger_plugin->on_dump_started(dump_context);
  }

  int64_t throttle_bytes = 0;
  int64_t byte_count = 0;
  bool persistence_failed = false;
  const size_t snapshot_frame_count = snapshot.size();
  Frame frame;
  frame.action_type = ActionType::kSubscribe;

  for (auto& item : snapshot) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    const auto item_size = static_cast<int64_t>(item.payload->size());
    frame.timestamp = item.capture_ts_us - min_capture;
    frame.url = item.source->url;
    frame.ser_type = item.source->ser_type;
    frame.schema_type = item.source->schema_type;
    frame.data = Bytes::shallow_copy(item.payload->data(), item.payload->size());

    if VUNLIKELY (writer->push(frame) < 0) {
      frame.data.clear();
      item.payload.reset();
      persistence_failed = true;
      break;
    }

    byte_count += item_size;

    if (trigger_plugin) {
      trigger_plugin->on_frame(frame, dump_context);
    }

    frame.data.clear();
    item.payload.reset();

    if (impl_->config.sleep_time_ms > 0 && impl_->config.sleep_interval > 0) {
      throttle_bytes += item_size;

      if (throttle_bytes >= impl_->config.sleep_interval) {
        std::this_thread::sleep_for(std::chrono::milliseconds(impl_->config.sleep_time_ms));
        throttle_bytes = 0;
      }
    }
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  frame.data.clear();
  std::vector<SnapFrame>().swap(snapshot);

  if (impl_->bag_plugin) {
    writer->clear_bag_interface();
  }

  writer->close();
  impl_->writing.store(false, std::memory_order_release);

  const bool writer_failed = writer->fail();

  writer.reset();

  if VUNLIKELY (persistence_failed || writer_failed) {
    VLOG_E("TriggerRecorder: writer reported a persistence failure: ", path);
    notify_dump_failed(job, "writer persistence failure");
    return;
  }

  if (trigger_plugin) {
    TriggerPluginInterface::DumpResult result;
    result.reason = params.reason;
    result.path = path;
    result.frame_count = static_cast<int64_t>(snapshot_frame_count);
    result.byte_count = byte_count;
    result.url_count = static_cast<int64_t>(losses.size());
    result.start_timestamp = writer_config.start_timestamp;
    result.duration_us = impl_->capture_timer.get() - dump_start;
    result.success = true;

    trigger_plugin->on_dump_finished(result);
  }

  VLOG_I("TriggerRecorder: dump finished -> ", path, " frames=", snapshot_frame_count);
}

}  // namespace vlink
