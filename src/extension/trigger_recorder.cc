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
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
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

using RawSub = Subscriber<Bytes>;

// LCOV_EXCL_START GCOVR_EXCL_START
static bool contains_exact(const std::vector<std::string>& list, const std::string& url) {
  return std::find(list.begin(), list.end(), url) != list.end();
}

static std::string to_lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });

  return text;
}

static bool match_filter_str(const std::string& url, const std::string& filter_str, bool black_mode) {
  const std::string lower_url = to_lower(url);

  bool hit = false;

  for (const auto& pattern : Helpers::split(filter_str, ' ')) {
    if (pattern.empty()) {
      continue;
    }

    if (lower_url.find(to_lower(pattern)) != std::string::npos) {
      hit = true;
      break;
    }
  }

  return black_mode ? !hit : hit;
}

static bool accept_trigger(const std::string& url, const TriggerRecorder::TriggerParams& params) {
  if (!params.filter_urls.empty()) {
    return params.filter_urls.count(url) != 0;
  }

  if (!params.filter_str.empty()) {
    return match_filter_str(url, params.filter_str, params.black_mode);
  }

  return true;
}
// LCOV_EXCL_STOP GCOVR_EXCL_STOP

static std::string make_timestamp_name() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  const int64_t millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;

  std::tm tm_buffer{};

#if defined(_WIN32)
  localtime_s(&tm_buffer, &now_time);
#else
  localtime_r(&now_time, &tm_buffer);
#endif

  char text[64];

  std::strftime(text, sizeof(text), "%Y-%m-%d_%H-%M-%S", &tm_buffer);

  char full[96];

  std::snprintf(full, sizeof(full), "%s_%03lld", text, static_cast<long long>(millis));

  return full;
}

static std::string sanitize_name(const std::string& name) {
  std::string result = name;

  for (char& character : result) {
    if (character == '/' || character == '\\') {
      character = '_';
    }
  }

  return result;
}

struct TriggerEngine final {  // NOLINT(clang-analyzer-optin.performance.Padding)
  struct Entry final {
    int64_t capture_ts_us{0};
    int64_t size{0};
    std::shared_ptr<const Bytes> payload;
  };

  struct UrlBuffer final {
    std::mutex mtx;
    std::deque<Entry> ring;
    int64_t bytes{0};
    bool dead{false};

    int64_t pre_us{0};
    int64_t post_us{0};
    std::atomic<int64_t> retention_us{0};
    int64_t max_packet_size{0};
    int64_t max_size{0};
    bool only_front{false};
    bool only_back{false};

    std::string url;
    std::string ser_type;
    SchemaType schema_type{SchemaType::kUnknown};
    SampleLostInfo last_lost;

    std::shared_ptr<RawSub> sub;
  };

  struct SnapFrame final {
    int64_t capture_ts_us{0};
    int64_t size{0};
    std::shared_ptr<const Bytes> payload;
    std::string url;
    std::string ser_type;
    SchemaType schema_type{SchemaType::kUnknown};
  };

  struct LossInfo final {
    std::string url;
    double loss{0.0};
  };

  TriggerRecorder::Config cfg;
  ElapsedTimer clock{ElapsedTimer::kCpuTimestamp, ElapsedTimer::kMicro};
  int64_t anchor_wall_us{0};
  std::string file_suffix;

  std::atomic<int64_t> max_post_all_us{0};
  std::atomic<int64_t> dump_due_us{0};
  std::atomic<int64_t> global_bytes{0};
  std::atomic<uint64_t> dump_gen{0};
  std::atomic_bool dumping{false};
  std::atomic_bool writing{false};
  std::atomic_bool running{false};

  std::shared_mutex map_mtx;
  std::unordered_map<std::string, std::shared_ptr<UrlBuffer>> map;
  std::vector<std::shared_ptr<UrlBuffer>> graveyard;

  std::unique_ptr<DiscoveryViewer> viewer;
  MessageLoop dump_loop;
  Timer sweep_timer;

  Plugin plugin_loader;

  std::shared_ptr<TriggerPluginInterface> trigger_plugin;
  std::vector<std::shared_ptr<TriggerPluginInterface>> retired_plugins;
  std::mutex trigger_plugin_mtx;

  std::shared_ptr<BagPluginInterface> bag_plugin;
  std::mutex bag_plugin_mtx;
};

static void recompute_retention(TriggerEngine* engine, bool dump_in_flight);

struct TriggerDumpGuard final {
  TriggerEngine* engine{nullptr};

  ~TriggerDumpGuard() {
    {
      std::unique_lock lock(engine->map_mtx);
      engine->graveyard.clear();
      recompute_retention(engine, false);
    }

    while (true) {
      std::vector<std::shared_ptr<TriggerPluginInterface>> retired;

      {
        std::lock_guard lock(engine->trigger_plugin_mtx);

        if (engine->retired_plugins.empty()) {
          engine->writing.store(false, std::memory_order_release);
          engine->dumping.store(false, std::memory_order_release);
          break;
        }

        retired.swap(engine->retired_plugins);
      }

      for (const auto& plugin : retired) {
        plugin->flush();
      }
    }
  }
};

static std::shared_ptr<TriggerPluginInterface> get_trigger_plugin(TriggerEngine* engine) {
  std::lock_guard lock(engine->trigger_plugin_mtx);

  return engine->trigger_plugin;
}

static void set_trigger_plugin(TriggerEngine* engine, const std::shared_ptr<TriggerPluginInterface>& value) {
  std::shared_ptr<TriggerPluginInterface> old;
  bool defer = false;

  {
    std::lock_guard lock(engine->trigger_plugin_mtx);
    old = engine->trigger_plugin;
    engine->trigger_plugin = value;

    if (value) {
      auto& retired = engine->retired_plugins;
      retired.erase(std::remove(retired.begin(), retired.end(), value), retired.end());
    }

    if (old && old != value && engine->dumping.load(std::memory_order_acquire)) {
      engine->retired_plugins.push_back(old);
      defer = true;
    }
  }

  if (old && old != value && !defer) {
    old->flush();
  }
}

static std::shared_ptr<BagPluginInterface> get_bag_plugin(TriggerEngine* engine) {
  std::lock_guard lock(engine->bag_plugin_mtx);

  return engine->bag_plugin;
}

static void set_bag_plugin(TriggerEngine* engine, const std::shared_ptr<BagPluginInterface>& value) {
  std::lock_guard lock(engine->bag_plugin_mtx);

  engine->bag_plugin = value;
}

// LCOV_EXCL_START GCOVR_EXCL_START
static void evict_front(TriggerEngine* engine, TriggerEngine::UrlBuffer* ub) {
  const int64_t size = ub->ring.front().size;

  ub->bytes -= size;
  engine->global_bytes.fetch_sub(size, std::memory_order_relaxed);
  ub->ring.pop_front();
}

static bool enforce_caps(TriggerEngine* engine, TriggerEngine::UrlBuffer* ub, int64_t incoming) {
  if (ub->max_size > 0 && incoming > ub->max_size) {
    return false;
  }

  if (engine->cfg.max_cache_size > 0 && incoming > engine->cfg.max_cache_size) {
    return false;
  }

  if (ub->max_size > 0) {
    while (ub->bytes + incoming > ub->max_size && !ub->ring.empty()) {
      if (engine->cfg.overflow == TriggerRecorder::kDropNewest) {
        return false;
      }

      evict_front(engine, ub);
    }
  }

  int64_t occupancy = engine->global_bytes.fetch_add(incoming, std::memory_order_relaxed) + incoming;

  if (engine->cfg.max_cache_size > 0 && occupancy > engine->cfg.max_cache_size) {
    if (engine->cfg.overflow == TriggerRecorder::kDropNewest) {
      engine->global_bytes.fetch_sub(incoming, std::memory_order_relaxed);
      return false;
    }

    while (occupancy > engine->cfg.max_cache_size && !ub->ring.empty()) {
      occupancy -= ub->ring.front().size;
      evict_front(engine, ub);
    }

    if (occupancy > engine->cfg.max_cache_size) {
      engine->global_bytes.fetch_sub(incoming, std::memory_order_relaxed);
      return false;
    }
  }

  return true;
}

static void handle_data(TriggerEngine* engine, TriggerEngine::UrlBuffer* ub, const Bytes& data) {
  if VUNLIKELY (!engine->running.load(std::memory_order_relaxed)) {
    return;
  }

  if VUNLIKELY (engine->cfg.busy_skip_data && engine->writing.load(std::memory_order_relaxed)) {
    return;
  }

  const size_t data_size = data.size();

  if VUNLIKELY (ub->max_packet_size > 0 && static_cast<int64_t>(data_size) > ub->max_packet_size) {
    return;
  }

  const int64_t capture_ts = engine->clock.get();

  auto payload = std::make_shared<const Bytes>(Bytes::deep_copy(data.data(), data_size));

  const auto incoming = static_cast<int64_t>(data_size);

  std::lock_guard lock(ub->mtx);

  if VUNLIKELY (ub->dead) {
    return;
  }

  const int64_t horizon = capture_ts - ub->retention_us.load(std::memory_order_relaxed);

  while (!ub->ring.empty() && ub->ring.front().capture_ts_us < horizon) {
    evict_front(engine, ub);
  }

  if VUNLIKELY (!enforce_caps(engine, ub, incoming)) {
    return;
  }

  ub->ring.push_back(TriggerEngine::Entry{capture_ts, incoming, std::move(payload)});
  ub->bytes += incoming;
}

static std::shared_ptr<TriggerEngine::UrlBuffer> build_url_buffer(TriggerEngine* engine,
                                                                  const DiscoveryViewer::Info& info) {
  auto ub = std::make_shared<TriggerEngine::UrlBuffer>();
  ub->url = info.url;
  ub->ser_type = info.ser_type;
  ub->schema_type = info.schema_type;

  TriggerRecorder::UrlConfig uc;

  auto over = engine->cfg.url_overrides.find(info.url);

  if (over != engine->cfg.url_overrides.end()) {
    uc = over->second;
  }

  const int64_t pre_ms = std::max<int64_t>(0, uc.pre_ms >= 0 ? uc.pre_ms : engine->cfg.default_pre_ms);
  const int64_t post_ms = std::max<int64_t>(0, uc.post_ms >= 0 ? uc.post_ms : engine->cfg.default_post_ms);

  ub->pre_us = pre_ms * 1000;
  ub->post_us = post_ms * 1000;
  ub->only_front = uc.only_front;
  ub->only_back = uc.only_back;
  ub->max_packet_size = uc.max_packet_size >= 0 ? uc.max_packet_size : engine->cfg.default_max_packet_size;
  ub->max_size = uc.max_size >= 0 ? uc.max_size : engine->cfg.default_max_size;

  const int64_t guard = engine->cfg.retention_guard_ms * 1000;
  const int64_t retention_pre = ub->only_back ? 0 : ub->pre_us;
  ub->retention_us.store(retention_pre + engine->max_post_all_us.load(std::memory_order_relaxed) + 2 * guard,
                         std::memory_order_relaxed);

  if (ub->only_front && ub->only_back) {
    VLOG_W("TriggerRecorder: URL has both only_front and only_back, window is empty: ", info.url);
  }

  try {
    auto sub = std::make_shared<RawSub>(info.url, InitType::kWithoutInit);

    if ((info.type & (kGetter | kSetter)) != 0) {
      sub->mark_as_getter();
    }

    sub->set_latency_and_lost_enabled(true);
    sub->set_ser_type(info.ser_type, info.schema_type);
    sub->set_discovery_enabled(false);

    if (!engine->cfg.dds_ip.empty()) {
      sub->set_property("dds.ip", engine->cfg.dds_ip);
    }

    if VUNLIKELY (!sub->init()) {
      VLOG_W("TriggerRecorder: subscriber init failed, URL skipped: ", info.url);
      return nullptr;
    }

    const bool listening = sub->listen([engine, weak = std::weak_ptr<TriggerEngine::UrlBuffer>(ub)](const Bytes& data) {
      if (auto locked = weak.lock()) {
        handle_data(engine, locked.get(), data);
      }
    });

    if VUNLIKELY (!listening) {
      VLOG_W("TriggerRecorder: subscriber listen failed, URL skipped: ", info.url);
      return nullptr;
    }

    ub->sub = std::move(sub);
  } catch (const std::exception& e) {
    VLOG_W("TriggerRecorder: subscriber setup failed, URL skipped: ", info.url, " (", e.what(), ")");
    return nullptr;
  }

  return ub;
}

static void recompute_retention(TriggerEngine* engine, bool dump_in_flight) {
  int64_t max_post = 0;

  for (auto& [url, ub] : engine->map) {
    const int64_t post = ub->only_front ? 0 : ub->post_us;

    if (post > max_post) {
      max_post = post;
    }
  }

  engine->max_post_all_us.store(max_post, std::memory_order_relaxed);

  const int64_t guard = engine->cfg.retention_guard_ms * 1000;

  for (auto& [url, ub] : engine->map) {
    const int64_t retention_pre = ub->only_back ? 0 : ub->pre_us;
    const int64_t updated = retention_pre + max_post + 2 * guard;

    if (dump_in_flight && updated < ub->retention_us.load(std::memory_order_relaxed)) {
      continue;
    }

    ub->retention_us.store(updated, std::memory_order_relaxed);
  }
}

static bool accept_url(const TriggerEngine* engine, const std::string& url) {
  if (!engine->cfg.blacklist.empty() && contains_exact(engine->cfg.blacklist, url)) {
    return false;
  }

  if (!engine->cfg.whitelist.empty() && !contains_exact(engine->cfg.whitelist, url)) {
    return false;
  }

  return true;
}

static void handle_discovery(TriggerEngine* engine, const std::vector<DiscoveryViewer::Info>& list) {
  if (!engine->running.load(std::memory_order_acquire)) {
    return;
  }

  std::vector<const DiscoveryViewer::Info*> to_add;
  std::unordered_set<std::string> pending;
  std::unordered_set<std::string> current;
  current.reserve(list.size());

  {
    std::shared_lock lock(engine->map_mtx);

    for (const auto& info : list) {
      current.insert(info.url);

      if ((info.type & (kPublisher | kSetter)) == 0) {
        continue;
      }

      if (engine->map.count(info.url) != 0) {
        continue;
      }

      if (!accept_url(engine, info.url)) {
        continue;
      }

      if (!pending.insert(info.url).second) {
        continue;
      }

      to_add.push_back(&info);
    }
  }

  std::vector<std::shared_ptr<TriggerEngine::UrlBuffer>> built;
  built.reserve(to_add.size());

  for (const auto* info : to_add) {
    auto ub = build_url_buffer(engine, *info);

    if (ub) {
      built.push_back(std::move(ub));
    }
  }

  {
    std::unique_lock lock(engine->map_mtx);

    const bool dump_in_flight = engine->dumping.load(std::memory_order_acquire);

    for (auto& ub : built) {
      std::string url = ub->url;

      if (engine->map.count(url) != 0) {
        ub->sub.reset();

        {
          std::lock_guard ring_lock(ub->mtx);
          ub->dead = true;
        }

        engine->global_bytes.fetch_sub(ub->bytes, std::memory_order_relaxed);
        continue;
      }

      engine->map.emplace(std::move(url), std::move(ub));
    }

    if (engine->cfg.destroy_on_offline) {
      for (auto iter = engine->map.begin(); iter != engine->map.end();) {
        if (current.count(iter->first) == 0) {
          iter->second->sub.reset();

          {
            std::lock_guard ring_lock(iter->second->mtx);
            iter->second->dead = true;
          }

          engine->global_bytes.fetch_sub(iter->second->bytes, std::memory_order_relaxed);

          if (dump_in_flight) {
            engine->graveyard.push_back(std::move(iter->second));
          }

          iter = engine->map.erase(iter);
        } else {
          ++iter;
        }
      }
    }

    recompute_retention(engine, dump_in_flight);
  }
}

static void sweep_evict(TriggerEngine* engine) {
  const int64_t now = engine->clock.get();

  std::shared_lock lock(engine->map_mtx);

  for (auto& [url, ub] : engine->map) {
    std::lock_guard ring_lock(ub->mtx);

    const int64_t horizon = now - ub->retention_us.load(std::memory_order_relaxed);

    while (!ub->ring.empty() && ub->ring.front().capture_ts_us < horizon) {
      evict_front(engine, ub.get());
    }
  }
}
// LCOV_EXCL_STOP GCOVR_EXCL_STOP

static bool start_engine(TriggerEngine* engine) {
  if (engine->running.load(std::memory_order_acquire)) {
    return true;
  }

  if (engine->cfg.dump_dir.empty()) {
    engine->cfg.dump_dir = Utils::get_tmp_dir() + "/vlink-trigger";
  }

  engine->file_suffix = (engine->cfg.file_type == TriggerRecorder::kVcap) ? ".vcap" : ".vdb";

  if (engine->cfg.default_pre_ms < 0) {
    engine->cfg.default_pre_ms = 0;
  }

  if (engine->cfg.default_post_ms < 0) {
    engine->cfg.default_post_ms = 0;
  }

  if (engine->cfg.retention_guard_ms < 0) {
    engine->cfg.retention_guard_ms = 0;
  }

  std::error_code ec;
  std::filesystem::create_directories(engine->cfg.dump_dir, ec);

  if (ec && !std::filesystem::exists(engine->cfg.dump_dir)) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    VLOG_E("TriggerRecorder: cannot create dump_dir '", engine->cfg.dump_dir, "': ", ec.message());
    return false;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  if (!engine->cfg.bag_plugin_lib.empty() && !get_bag_plugin(engine)) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    auto loaded =
        engine->plugin_loader.load<BagPluginInterface>(engine->cfg.bag_plugin_lib, engine->cfg.bag_plugin_major,
                                                       engine->cfg.bag_plugin_minor, engine->cfg.bag_plugin_dir);

    if (!loaded) {
      VLOG_E("TriggerRecorder: failed to load bag reorder plugin '", engine->cfg.bag_plugin_lib, "'");
      return false;
    }

    set_bag_plugin(engine, loaded);
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  engine->clock.start();
  engine->anchor_wall_us =
      static_cast<int64_t>(ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMicro)) - engine->clock.get();

  if (!engine->dump_loop.async_run()) {
    VLOG_E("TriggerRecorder: dump loop failed to start");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;                                          // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  engine->sweep_timer.attach(&engine->dump_loop);
  engine->sweep_timer.set_interval(1000);
  engine->sweep_timer.set_loop_count(Timer::kInfinite);
  engine->sweep_timer.start([engine]() { sweep_evict(engine); });

  engine->viewer = std::make_unique<DiscoveryViewer>(engine->cfg.discovery_filter);
  engine->viewer->register_callback(
      [engine](const std::vector<DiscoveryViewer::Info>& list) { handle_discovery(engine, list); });

  engine->running.store(true, std::memory_order_release);

  if (!engine->viewer->async_run()) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    VLOG_E("TriggerRecorder: discovery viewer failed to start");
    engine->running.store(false, std::memory_order_release);
    engine->sweep_timer.stop();
    engine->dump_loop.quit(true);
    engine->dump_loop.wait_for_quit();
    engine->viewer.reset();
    return false;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  if (auto active_plugin = get_trigger_plugin(engine)) {
    active_plugin->on_start();
  }

  return true;
}

static void stop_engine(TriggerEngine* engine) {
  if (!engine->running.exchange(false, std::memory_order_acq_rel)) {
    return;
  }

  if (engine->viewer) {
    engine->viewer->quit(true);
    engine->viewer->wait_for_quit();
    engine->viewer.reset();
  }

  engine->sweep_timer.stop();

  const int64_t wait_deadline = engine->dump_due_us.load(std::memory_order_acquire) + 90'000'000;

  while (engine->dumping.load(std::memory_order_acquire) && engine->clock.get() < wait_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  engine->dump_loop.quit(true);
  engine->dump_loop.wait_for_quit();

  engine->dump_gen.fetch_add(1, std::memory_order_acq_rel);
  engine->dumping.store(false, std::memory_order_release);
  engine->writing.store(false, std::memory_order_release);

  {
    std::unique_lock lock(engine->map_mtx);

    for (auto& [url, ub] : engine->map) {
      ub->sub.reset();

      std::lock_guard ring_lock(ub->mtx);
      ub->dead = true;
    }

    engine->map.clear();
    engine->graveyard.clear();
    engine->global_bytes.store(0, std::memory_order_relaxed);
    engine->max_post_all_us.store(0, std::memory_order_relaxed);
    engine->dump_due_us.store(0, std::memory_order_relaxed);
  }

  std::vector<std::shared_ptr<TriggerPluginInterface>> retired;

  {
    std::lock_guard plugin_lock(engine->trigger_plugin_mtx);
    retired.swap(engine->retired_plugins);
  }

  for (const auto& plugin : retired) {
    plugin->flush();
  }

  if (auto active_plugin = get_trigger_plugin(engine)) {
    active_plugin->flush();
    active_plugin->on_stop();
  }
}

static std::vector<std::string> rotate_files(const TriggerEngine* engine) {
  std::vector<std::string> removed;

  if (engine->cfg.max_dump_file_count <= 0) {
    return removed;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  std::error_code ec;
  std::vector<std::filesystem::directory_entry> files;
  std::filesystem::directory_iterator iter(engine->cfg.dump_dir, ec);

  if (ec) {
    return removed;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  for (const std::filesystem::directory_iterator end; iter != end; iter.increment(ec)) {
    std::error_code type_ec;

    if (iter->is_regular_file(type_ec) && iter->path().extension() == engine->file_suffix) {
      files.push_back(*iter);
    }
  }

  std::sort(files.begin(), files.end(),
            [](const std::filesystem::directory_entry& left, const std::filesystem::directory_entry& right) {
              std::error_code le;
              std::error_code re;

              return std::filesystem::last_write_time(left, le) < std::filesystem::last_write_time(right, re);
            });

  while (static_cast<int>(files.size()) >= engine->cfg.max_dump_file_count) {
    std::error_code rm;

    if (std::filesystem::remove(files.front().path(), rm) && !rm) {
      removed.push_back(files.front().path().string());
    }

    files.erase(files.begin());
  }

  return removed;
}

// LCOV_EXCL_START GCOVR_EXCL_START
static void notify_dump_failed(const std::shared_ptr<TriggerPluginInterface>& active_plugin, const std::string& reason,
                               const std::string& path, const std::string& error) {
  if (!active_plugin) {
    return;
  }

  TriggerPluginInterface::DumpResult result;
  result.reason = reason;
  result.path = path;
  result.success = false;
  result.error = error;

  active_plugin->on_dump_failed(result);
}
// LCOV_EXCL_STOP GCOVR_EXCL_STOP

// LCOV_EXCL_START GCOVR_EXCL_START
static void snapshot_url_buffer(TriggerEngine::UrlBuffer* ub, int64_t trigger_ts, int64_t trig_pre, int64_t trig_post,
                                std::vector<TriggerEngine::SnapFrame>& snap,
                                std::vector<TriggerEngine::LossInfo>& losses) {
  if (ub->only_front && ub->only_back) {
    return;
  }

  double loss = 0.0;

  if (ub->sub) {
    const auto lost = ub->sub->get_lost();
    const uint64_t delta_total = lost.total - ub->last_lost.total;
    const uint64_t delta_lost = lost.lost - ub->last_lost.lost;

    loss = delta_total > 0 ? static_cast<double>(delta_lost) / static_cast<double>(delta_total) : 0.0;
    ub->last_lost = lost;
  }

  int64_t pre = ub->pre_us;
  int64_t post = ub->only_front ? 0 : ub->post_us;

  if (trig_pre >= 0 && trig_pre < pre) {
    pre = trig_pre;
  }

  if (trig_post >= 0 && trig_post < post) {
    post = trig_post;
  }

  int64_t lo = trigger_ts - pre;
  int64_t hi = trigger_ts + post;

  if (ub->only_front) {
    hi = trigger_ts;
  }

  if (ub->only_back) {
    lo = trigger_ts;
  }

  std::lock_guard ring_lock(ub->mtx);

  for (const auto& entry : ub->ring) {
    if (entry.capture_ts_us >= lo && entry.capture_ts_us <= hi) {
      snap.push_back(TriggerEngine::SnapFrame{entry.capture_ts_us, entry.size, entry.payload, ub->url, ub->ser_type,
                                              ub->schema_type});
    }
  }

  losses.push_back(TriggerEngine::LossInfo{ub->url, loss});
}
// LCOV_EXCL_STOP GCOVR_EXCL_STOP

static void do_dump(TriggerEngine* engine, int64_t trigger_ts, const TriggerRecorder::TriggerParams& params,
                    uint64_t gen, const std::shared_ptr<TriggerPluginInterface>& trigger_plugin,
                    const std::shared_ptr<BagPluginInterface>& bag_plugin,
                    const std::unordered_set<std::string>& frozen_urls) {
  if VUNLIKELY (gen != engine->dump_gen.load(std::memory_order_acquire)) {
    return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  TriggerDumpGuard guard{engine};

  const int64_t dump_start = engine->clock.get();

  if (trigger_plugin) {
    TriggerPluginInterface::TriggerContext context;
    context.reason = params.reason;
    context.name_hint = params.name_hint;
    context.out_file = params.out_file;
    context.pre_ms = params.pre_ms;
    context.post_ms = params.post_ms;
    context.trigger_timestamp = engine->anchor_wall_us + trigger_ts;

    trigger_plugin->on_trigger(context);
  }

  const int64_t trig_pre = params.pre_ms >= 0 ? params.pre_ms * 1000 : -1;
  const int64_t trig_post = params.post_ms >= 0 ? params.post_ms * 1000 : -1;

  std::vector<TriggerEngine::SnapFrame> snap;
  std::vector<TriggerEngine::LossInfo> losses;

  {
    std::shared_lock map_lock(engine->map_mtx);

    // LCOV_EXCL_START GCOVR_EXCL_START
    for (auto& [url, ub] : engine->map) {
      if (frozen_urls.count(url) == 0 || !accept_trigger(ub->url, params)) {
        continue;
      }

      snapshot_url_buffer(ub.get(), trigger_ts, trig_pre, trig_post, snap, losses);
    }

    for (auto& ub : engine->graveyard) {
      if (frozen_urls.count(ub->url) == 0 || !accept_trigger(ub->url, params)) {
        continue;
      }

      snapshot_url_buffer(ub.get(), trigger_ts, trig_pre, trig_post, snap, losses);
    }
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  std::unordered_set<std::string> loss_seen;
  loss_seen.reserve(losses.size());

  losses.erase(
      std::remove_if(losses.begin(), losses.end(),
                     [&loss_seen](const TriggerEngine::LossInfo& info) { return !loss_seen.insert(info.url).second; }),
      losses.end());

  engine->writing.store(true, std::memory_order_release);

  std::string path = params.out_file;
  const bool auto_named = path.empty();

  if (auto_named) {
    const std::string name = params.name_hint.empty() ? make_timestamp_name() : sanitize_name(params.name_hint);

    path = engine->cfg.dump_dir + "/" + name + engine->file_suffix;

    for (const auto& removed_path : rotate_files(engine)) {
      if (trigger_plugin) {
        trigger_plugin->on_file_rotated(removed_path);
      }
    }
  }

  int64_t min_capture = trigger_ts;

  for (const auto& frame : snap) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    if (frame.capture_ts_us < min_capture) {
      min_capture = frame.capture_ts_us;
    }
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  std::stable_sort(snap.begin(), snap.end(),
                   [](const TriggerEngine::SnapFrame& left, const TriggerEngine::SnapFrame& right) {
                     return left.capture_ts_us < right.capture_ts_us;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                   });

  BagWriter::Config wc;
  wc.compress = engine->cfg.enable_compress ? BagWriter::kCompressAuto : BagWriter::kCompressNone;
  wc.tag_name = params.reason;
  wc.sync_mode = true;
  wc.optimize_on_exit = true;
  wc.start_timestamp = (engine->anchor_wall_us + min_capture) / 1000;

  std::shared_ptr<BagWriter> writer;

  try {
    writer = BagWriter::create(path, wc);
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

  writer->async_run();

  if (bag_plugin) {
    writer->bind_plugin_interface(bag_plugin);
  }

  for (const auto& info : losses) {
    writer->set_url_loss(info.url, info.loss);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  TriggerPluginInterface::DumpContext dump_context;
  dump_context.reason = params.reason;
  dump_context.path = path;
  dump_context.start_timestamp = wc.start_timestamp;
  dump_context.url_count = static_cast<int64_t>(losses.size());

  if (trigger_plugin) {
    trigger_plugin->on_dump_started(dump_context);
  }

  int64_t throttle_acc = 0;
  int64_t dropped = 0;
  int64_t byte_count = 0;

  for (const auto& item : snap) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    Frame frame;
    frame.timestamp = item.capture_ts_us - min_capture;
    frame.url = item.url;
    frame.ser_type = item.ser_type;
    frame.schema_type = item.schema_type;
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

    if (engine->cfg.sleep_time_ms > 0 && engine->cfg.sleep_interval > 0) {
      throttle_acc += item.size;

      if (throttle_acc >= engine->cfg.sleep_interval) {
        std::this_thread::sleep_for(std::chrono::milliseconds(engine->cfg.sleep_time_ms));
        throttle_acc = 0;
      }
    }
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  if (bag_plugin) {
    writer->clear_plugin_interface();
  }

  const bool drained = writer->wait_for_idle(60'000);

  writer.reset();

  if (dropped > 0) {
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

  if (trigger_plugin) {
    TriggerPluginInterface::DumpResult result;
    result.reason = params.reason;
    result.path = path;
    result.frame_count = static_cast<int64_t>(snap.size()) - dropped;
    result.dropped_count = dropped;
    result.byte_count = byte_count;
    result.url_count = static_cast<int64_t>(losses.size());
    result.start_timestamp = wc.start_timestamp;
    result.duration_us = engine->clock.get() - dump_start;
    result.success = true;

    trigger_plugin->on_dump_finished(result);
  }

  VLOG_I("TriggerRecorder: dump finished -> ", path, " frames=", snap.size());
}

static bool trigger_dump(TriggerEngine* engine, const TriggerRecorder::TriggerParams& params) {
  if (!engine->running.load(std::memory_order_acquire)) {
    return false;
  }

  bool expected = false;

  if (!engine->dumping.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return false;
  }

  if (!engine->running.load(std::memory_order_acquire)) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    engine->dumping.store(false, std::memory_order_release);
    return false;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  const int64_t trigger_ts = engine->clock.get();

  const uint64_t gen = engine->dump_gen.load(std::memory_order_acquire);
  auto trigger_plugin = get_trigger_plugin(engine);
  auto bag_plugin = get_bag_plugin(engine);

  int64_t max_post = 0;
  std::unordered_set<std::string> frozen_urls;

  {
    std::shared_lock lock(engine->map_mtx);

    max_post = engine->max_post_all_us.load(std::memory_order_relaxed);
    frozen_urls.reserve(engine->map.size() + engine->graveyard.size());

    for (const auto& [url, ub] : engine->map) {
      frozen_urls.insert(url);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    for (const auto& ub : engine->graveyard) {
      frozen_urls.insert(ub->url);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
  }

  const int64_t guard_us = engine->cfg.retention_guard_ms * 1000;
  const int64_t delay_ms = max_post > 0 ? (max_post + guard_us) / 1000 : 0;

  engine->dump_due_us.store(trigger_ts + max_post + guard_us, std::memory_order_release);

  auto task = [engine, trigger_ts, params, gen, trigger_plugin, bag_plugin, frozen_urls = std::move(frozen_urls)]() {
    do_dump(engine, trigger_ts, params, gen, trigger_plugin, bag_plugin, frozen_urls);
  };

  bool ok = false;

  if (delay_ms > 0) {
    ok = Timer::call_once(&engine->dump_loop, static_cast<uint32_t>(delay_ms), std::move(task));
  } else {
    ok = engine->dump_loop.post_task(std::move(task));
  }

  if (!ok) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    engine->dumping.store(false, std::memory_order_release);
    return false;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  return true;
}

struct TriggerRecorder::Impl final {
  TriggerEngine engine;
};

TriggerRecorder::TriggerRecorder(const Config& config) : impl_(std::make_unique<Impl>()) { impl_->engine.cfg = config; }

TriggerRecorder::~TriggerRecorder() { stop_engine(&impl_->engine); }

bool TriggerRecorder::start() { return start_engine(&impl_->engine); }

bool TriggerRecorder::trigger(const TriggerParams& params) { return trigger_dump(&impl_->engine, params); }

void TriggerRecorder::stop() { stop_engine(&impl_->engine); }

bool TriggerRecorder::is_dumping() const noexcept { return impl_->engine.dumping.load(std::memory_order_acquire); }

bool TriggerRecorder::is_running() const noexcept { return impl_->engine.running.load(std::memory_order_acquire); }

void TriggerRecorder::bind_trigger_plugin_interface(const std::shared_ptr<TriggerPluginInterface>& plugin) {
  set_trigger_plugin(&impl_->engine, plugin);
}

void TriggerRecorder::clear_trigger_plugin_interface() { set_trigger_plugin(&impl_->engine, nullptr); }

void TriggerRecorder::bind_bag_plugin_interface(const std::shared_ptr<BagPluginInterface>& plugin) {
  set_bag_plugin(&impl_->engine, plugin);
}

void TriggerRecorder::clear_bag_plugin_interface() { set_bag_plugin(&impl_->engine, nullptr); }

}  // namespace vlink
