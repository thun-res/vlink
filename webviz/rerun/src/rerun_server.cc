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

#include "rerun_server.h"

//
#include <vlink/version.h>

//
#include <algorithm>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

//
#include "webviz_app_utils.h"
#include "webviz_bridge_utils.h"

namespace vlink {
namespace webviz {

static constexpr size_t kMaxTaskDepth = 10000U;

RerunServer::RerunServer(const Config& config) : MessageLoop(MessageLoop::kNormalType), config_(config) {
  set_name("RerunServer");
  bridge_ = ProxyBridge::create(config.proxy_config, this);

  RerunConverter::Config conv_config;
  conv_config.proto_dir = config.proto_dir;
  conv_config.fbs_dir = config.fbs_dir;
  conv_config.schema_plugin_path = config.schema_plugin_path;
  conv_config.convert_plugin_path = config.convert_plugin_path;
  conv_config.convert_plugin_config = config.convert_plugin_config;
  conv_config.vlink_msgs = config.vlink_msgs;
  conv_config.timestamp_timeline = config.timestamp_timeline;
  conv_config.use_timestamp_timeline = config.use_timestamp_timeline;
  rerun_converter_ = std::make_unique<RerunConverter>(conv_config);
}

RerunServer::~RerunServer() { stop(); }

size_t RerunServer::get_max_task_count() const { return kMaxTaskDepth; }

bool RerunServer::start() {
  if VUNLIKELY (running_.exchange(true)) {
    return true;
  }

  if VUNLIKELY (!init_rerun()) {
    running_.store(false);
    return false;
  }

  if VUNLIKELY (!init_bridge()) {
    running_.store(false);
    flush_recording();
    {
      std::unique_lock lock(rec_mtx_);
      rec_.reset();
    }
    rec_raw_.store(nullptr);
    reset_bridge_wall_time_state(last_sys_time_ns_, bridge_time_elapsed_);
    reset_bridge_session_time_anchor(session_start_sys_time_ns_);
    {
      std::lock_guard lock(bridge_control_mtx_);
      bridge_control_signature_.clear();
    }
    return false;
  }

  if VLIKELY (config_.mode == RerunServer::kSpawn || config_.mode == RerunServer::kConnect) {
    if VUNLIKELY (!probe_timer_.attach(this)) {
      MLOG_E("Failed to attach Rerun probe timer");
      stop();
      return false;
    }

    probe_timer_.set_interval(5000);
    probe_timer_.set_loop_count(Timer::kInfinite);
    probe_timer_.set_callback([this]() { probe_recording(); });
    probe_timer_.start();
  }

  const char* mode_name = nullptr;

  switch (config_.mode) {
    case RerunServer::kSpawn:
      mode_name = "spawn";
      break;
    case RerunServer::kConnect:
      mode_name = "connect";
      break;
    case RerunServer::kServe:
      mode_name = "serve";
      break;
    case RerunServer::kSave:
      mode_name = "save";
      break;
    default:
      mode_name = "unknown";
      break;
  }

  MLOG_I("Rerun server started (name={}, mode={})", config_.name, mode_name);
  run();
  flush_recording();
  MLOG_I("Rerun server stopped");
  return true;
}

void RerunServer::stop() {
  if VUNLIKELY (!running_.exchange(false)) {
    return;
  }

  rec_raw_.store(nullptr);
  reset_bridge_wall_time_state(last_sys_time_ns_, bridge_time_elapsed_);
  reset_bridge_session_time_anchor(session_start_sys_time_ns_);
  {
    std::lock_guard lock(bridge_control_mtx_);
    bridge_control_signature_.clear();
  }
  probe_timer_.stop();
  probe_timer_.detach();

  if VLIKELY (bridge_) {
    bridge_->stop();
  }

  quit(false);

  if VLIKELY (!is_in_same_thread()) {
    wait_for_quit();
  }

  {
    std::unique_lock lock(info_mtx_);
    last_info_map_.clear();
    subscribed_urls_.clear();
  }

  subscribed_urls_generation_.fetch_add(1);

  flush_recording();

  {
    std::unique_lock lock(rec_mtx_);
    rec_.reset();
  }
}

void RerunServer::flush_recording() {
  std::shared_ptr<::rerun::RecordingStream> rec;

  {
    std::shared_lock lock(rec_mtx_);
    rec = rec_;
  }

  if VUNLIKELY (!rec) {
    return;
  }

  auto err = rec->flush_blocking();

  if VUNLIKELY (err.is_err()) {
    MLOG_W("Failed to flush: {}", err.description);
  }
}

bool RerunServer::init_rerun() {
  try {
    std::shared_ptr<::rerun::RecordingStream> rec;

    if VUNLIKELY (!open_recording(rec)) {
      if VUNLIKELY (config_.mode != RerunServer::kSpawn && config_.mode != RerunServer::kConnect) {
        return false;
      }

      {
        std::unique_lock lock(rec_mtx_);
        rec_.reset();
        rec_raw_.store(nullptr);
      }

      MLOG_W("Rerun viewer is not available yet, start in reconnect-wait mode");
      return true;
    }

    {
      std::unique_lock lock(rec_mtx_);
      rec_ = std::move(rec);
      rec_raw_.store(rec_.get());
    }

    return true;
  } catch (const std::exception& e) {
    MLOG_E("Rerun init exception: {}", e.what());
    return false;
  }
}

bool RerunServer::open_recording(std::shared_ptr<::rerun::RecordingStream>& rec) {
  rec = std::make_shared<::rerun::RecordingStream>(config_.name, config_.recording_id);

  switch (config_.mode) {
    case RerunServer::kSpawn: {
      ::rerun::SpawnOptions options;
      options.port = config_.port;
      options.memory_limit = config_.spawn_memory_limit;
      options.server_memory_limit = config_.spawn_server_memory_limit;
      options.hide_welcome_screen = config_.spawn_hide_welcome_screen;
      options.detach_process = config_.spawn_detach_process;
      options.executable_name = config_.spawn_executable_name;
      options.executable_path = config_.spawn_executable_path;

      auto err = rec->spawn(options);

      if VUNLIKELY (err.is_err()) {
        MLOG_E("Failed to spawn Rerun viewer: {}", err.description);
        return false;
      }

      apply_timeline_policy(*rec);
      return true;
    }

    case RerunServer::kConnect: {
      auto err = rec->connect_grpc(config_.address);

      if VUNLIKELY (err.is_err()) {
        MLOG_E("Failed to connect to Rerun viewer at {}: {}", config_.address, err.description);
        return false;
      }

      apply_timeline_policy(*rec);
      return true;
    }

    case RerunServer::kServe: {
      ::rerun::PlaybackBehavior playback{};

      if VLIKELY (config_.playback_behavior == "oldest_first") {
        playback = ::rerun::PlaybackBehavior::OldestFirst;
      } else if VLIKELY (config_.playback_behavior == "newest_first") {
        playback = ::rerun::PlaybackBehavior::NewestFirst;
      } else {
        MLOG_E("Invalid playback behavior: {}", config_.playback_behavior);
        return false;
      }

      auto result = rec->serve_grpc(config_.bind_ip, config_.port, config_.serve_memory_limit, playback);

      if VUNLIKELY (result.is_err()) {
        MLOG_E("Failed to start gRPC server on {}:{}: {}", config_.bind_ip, config_.port, result.error.description);
        return false;
      }

      apply_timeline_policy(*rec);
      MLOG_I("Rerun gRPC server listening on {}", result.value);
      return true;
    }

    case RerunServer::kSave: {
      if VUNLIKELY (config_.save_path.empty()) {
        MLOG_E("Save mode requires --save_path");
        return false;
      }

      auto err = rec->save(config_.save_path);

      if VUNLIKELY (err.is_err()) {
        MLOG_E("Failed to save to {}: {}", config_.save_path, err.description);
        return false;
      }

      apply_timeline_policy(*rec);
      return true;
    }
  }

  return false;
}

bool RerunServer::reconnect_recording() {
  if VUNLIKELY (!running_.load()) {
    return false;
  }

  if VUNLIKELY (config_.mode != RerunServer::kSpawn && config_.mode != RerunServer::kConnect) {
    return false;
  }

  std::shared_ptr<::rerun::RecordingStream> rec;

  if VUNLIKELY (!open_recording(rec)) {
    return false;
  }

  {
    std::unique_lock lock(rec_mtx_);
    rec_ = std::move(rec);
    rec_raw_.store(rec_.get());
  }

  return true;
}

void RerunServer::probe_recording() {
  if VUNLIKELY (!running_.load()) {
    return;
  }

  if VUNLIKELY (config_.mode != RerunServer::kSpawn && config_.mode != RerunServer::kConnect) {
    return;
  }

  std::shared_ptr<::rerun::RecordingStream> rec;

  {
    std::shared_lock lock(rec_mtx_);
    rec = rec_;
  }

  if VUNLIKELY (!rec) {
    if VLIKELY (reconnect_recording()) {
      MLOG_I("Rerun reconnect succeeded");
    }

    return;
  }

  auto err = rec->flush_blocking();

  if VLIKELY (!err.is_err()) {
    return;
  }

  MLOG_W("Rerun probe failed: {}; reconnecting", err.description);

  if VLIKELY (reconnect_recording()) {
    MLOG_I("Rerun reconnect succeeded");
  } else {
    MLOG_W("Rerun reconnect failed");
  }
}

bool RerunServer::init_bridge() {
  if VUNLIKELY (!bridge_) {
    MLOG_E("Proxy bridge is not initialized");
    return false;
  }

  bridge_->register_connect_callback([this](bool connected) {
    if VUNLIKELY (!running_.load()) {
      return;
    }

    on_bridge_connected(connected);
  });

  bridge_->register_info_callback([this](const std::vector<ProxyAPI::Info>& info_list) {
    if VUNLIKELY (!running_.load()) {
      return;
    }

    on_bridge_info(info_list);
  });

  bridge_->register_data_callback([this](const ProxyAPI::Data& data) {
    if VUNLIKELY (!running_.load()) {
      return;
    }

    if VUNLIKELY (!is_url_allowed(data.url)) {
      return;
    }

    if VUNLIKELY (rec_raw_.load() == nullptr) {
      return;
    }

    on_bridge_data(data);
  });

  bridge_->register_time_callback([this](uint64_t sys_time, uint64_t boot_time) {
    if VUNLIKELY (!running_.load()) {
      return;
    }

    on_bridge_time(sys_time, boot_time);
  });

  bridge_->register_error_callback([this](ProxyAPI::Error error) { log_proxy_bridge_error(*bridge_, error); });

  if VUNLIKELY (!bridge_->start()) {
    MLOG_E("Failed to start proxy bridge in {} mode", ProxyBridge::to_string(config_.proxy_config.interface_mode));
    return false;
  }

  return true;
}

void RerunServer::on_bridge_connected(bool connected) {
  if VUNLIKELY (!running_.load()) {
    return;
  }

  if VLIKELY (connected) {
    MLOG_I("Connected to proxy bridge in {} mode", ProxyBridge::to_string(config_.proxy_config.interface_mode));
    update_bridge_control();
  } else {
    MLOG_W("Disconnected from proxy bridge");

    std::vector<std::string> urls_to_clear;
    bool subscribed_urls_changed = false;

    {
      std::unique_lock lock(info_mtx_);

      for (const auto& url : subscribed_urls_) {
        urls_to_clear.emplace_back(url);
      }

      subscribed_urls_changed = !subscribed_urls_.empty();
      subscribed_urls_.clear();
      last_info_map_.clear();
    }

    if VUNLIKELY (subscribed_urls_changed) {
      subscribed_urls_generation_.fetch_add(1);
    }

    if VLIKELY (!urls_to_clear.empty()) {
      std::shared_ptr<::rerun::RecordingStream> rec;

      {
        std::shared_lock lock(rec_mtx_);
        rec = rec_;
      }

      if VLIKELY (rec) {
        rec->reset_time();

        for (const auto& url : urls_to_clear) {
          rec->log(url_to_entity_path(url), ::rerun::archetypes::Clear(true));
        }
      }
    }

    reset_bridge_wall_time_state(last_sys_time_ns_, bridge_time_elapsed_);
    reset_bridge_session_time_anchor(session_start_sys_time_ns_);
    {
      std::lock_guard lock(bridge_control_mtx_);
      bridge_control_signature_.clear();
    }
  }
}

void RerunServer::on_bridge_info(const std::vector<ProxyAPI::Info>& info_list) {
  std::vector<std::string> urls_to_clear;
  bool subscribed_urls_changed = false;

  {
    std::unique_lock lock(info_mtx_);

    std::unordered_set<std::string> current_valid_urls;

    for (const auto& info : info_list) {
      if VLIKELY (info.status != ProxyAPI::kInvalid && is_publisher_info(info) && is_url_allowed(info.url)) {
        current_valid_urls.insert(info.url);
      }
    }

    for (auto url_iter = subscribed_urls_.begin(); url_iter != subscribed_urls_.end();) {
      if VUNLIKELY (current_valid_urls.find(*url_iter) == current_valid_urls.end()) {
        urls_to_clear.emplace_back(*url_iter);
        last_info_map_.erase(*url_iter);
        url_iter = subscribed_urls_.erase(url_iter);
        subscribed_urls_changed = true;
      } else {
        ++url_iter;
      }
    }

    for (const auto& info : info_list) {
      if VUNLIKELY (info.status == ProxyAPI::kInvalid) {
        continue;
      }

      if VUNLIKELY (!is_publisher_info(info)) {
        continue;
      }

      if VUNLIKELY (!is_url_allowed(info.url)) {
        continue;
      }

      last_info_map_[info.url] = info;

      if VUNLIKELY (subscribed_urls_.find(info.url) == subscribed_urls_.end()) {
        subscribed_urls_.insert(info.url);
        subscribed_urls_changed = true;
      }
    }
  }

  if VUNLIKELY (subscribed_urls_changed) {
    subscribed_urls_generation_.fetch_add(1);
  }

  if VUNLIKELY (!urls_to_clear.empty()) {
    std::shared_ptr<::rerun::RecordingStream> rec;

    {
      std::shared_lock lock(rec_mtx_);
      rec = rec_;
    }

    if VLIKELY (rec) {
      rec->reset_time();

      for (const auto& url : urls_to_clear) {
        rec->log(url_to_entity_path(url), ::rerun::archetypes::Clear(true));
      }
    }
  }

  update_bridge_control();
}

void RerunServer::on_bridge_data(const ProxyAPI::Data& data) {
  if VUNLIKELY (!running_.load()) {
    return;
  }

  struct SubscribedUrlCache final {
    const RerunServer* owner{nullptr};
    uint64_t generation{0};
    std::string url;
    bool known{false};
  };

  thread_local SubscribedUrlCache cache;
  auto generation = subscribed_urls_generation_.load();
  bool known_url = cache.owner == this && cache.generation == generation && cache.known && cache.url == data.url;

  if VUNLIKELY (!known_url) {
    {
      std::shared_lock lock(info_mtx_);

      if VLIKELY (subscribed_urls_.find(data.url) != subscribed_urls_.end()) {
        cache.owner = this;
        cache.generation = generation;
        cache.url.assign(data.url);
        cache.known = true;
        known_url = true;
      }
    }
  }

  if VUNLIKELY (!known_url) {
    std::unique_lock lock(info_mtx_);

    if VUNLIKELY (subscribed_urls_.find(data.url) == subscribed_urls_.end()) {
      if VUNLIKELY (!is_url_allowed(data.url)) {
        return;
      }

      subscribed_urls_.insert(data.url);
      generation = subscribed_urls_generation_.fetch_add(1) + 1;
    }

    cache.owner = this;
    cache.generation = generation;
    cache.url.assign(data.url);
    cache.known = true;
  }

  std::shared_ptr<::rerun::RecordingStream> rec;

  {
    std::shared_lock lock(rec_mtx_);
    rec = rec_;
  }

  if VUNLIKELY (!rec) {
    return;
  }

  // Rerun timelines stay active on the current thread until reset or disabled.
  // Clear any previous message state so custom timelines do not leak into the
  // next bridge sample.
  rec->reset_time();

  if VLIKELY (config_.use_sequence_timeline && !config_.sequence_timeline.empty()) {
    rec->set_time_sequence(config_.sequence_timeline, data.seq);
  }

  int64_t fallback_timestamp_ns = -1;

  if VLIKELY (config_.use_timestamp_timeline && !config_.timestamp_timeline.empty()) {
    auto bridge_wall_time_ns = estimate_bridge_wall_time_ns(last_sys_time_ns_.load(), bridge_time_elapsed_);
    auto timestamp_ns =
        resolve_bridge_data_timestamp_ns(session_start_sys_time_ns_.load(), data.timestamp, bridge_wall_time_ns);

    if VLIKELY (timestamp_ns > 0 && timestamp_ns <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      fallback_timestamp_ns = static_cast<int64_t>(timestamp_ns);
    }
  }

  rerun_converter_->convert_and_log(*rec, url_to_entity_path(data.url), data.url, data.schema, data.ser, data.raw,
                                    fallback_timestamp_ns);
}

void RerunServer::apply_timeline_policy(::rerun::RecordingStream& rec) const {
  if VUNLIKELY (!config_.use_sequence_timeline && !config_.sequence_timeline.empty()) {
    rec.disable_timeline(config_.sequence_timeline);
  }

  if VUNLIKELY (!config_.use_timestamp_timeline && !config_.timestamp_timeline.empty()) {
    rec.disable_timeline(config_.timestamp_timeline);
  }
}

void RerunServer::on_bridge_time(uint64_t sys_time, uint64_t boot_time) {
  update_bridge_wall_time_state(sys_time, boot_time, last_sys_time_ns_, bridge_time_elapsed_,
                                &session_start_sys_time_ns_);
}

bool RerunServer::is_url_allowed(std::string_view url) const {
  return is_allowed_by_filters_cached(this, url, config_.whitelist_exact, config_.whitelist_patterns,
                                      config_.blacklist_exact, config_.blacklist_patterns);
}

bool RerunServer::update_bridge_control() {
  if VUNLIKELY (!bridge_ || !bridge_->can_control()) {
    return false;
  }

  std::lock_guard lock(bridge_control_mtx_);

  ProxyAPI::Control control;
  control.mode = ProxyAPI::kAutoAndObserveAll;

  auto signature = build_bridge_control_signature(control);

  if VLIKELY (signature == bridge_control_signature_) {
    return true;
  }

  reset_bridge_session_time_anchor(session_start_sys_time_ns_);

  if VUNLIKELY (!bridge_->send_control(control, false)) {
    MLOG_W("Failed to update Rerun bridge control");
    return false;
  }

  bridge_control_signature_ = std::move(signature);
  return true;
}

std::string RerunServer::url_to_entity_path(const std::string& url) {
  struct EntityPathCache final {
    std::string url;
    std::string entity_path;
  };

  thread_local EntityPathCache cache;

  if VLIKELY (cache.url == url) {
    return cache.entity_path;
  }

  // Convert transport: "dds://camera/front" -> "dds/camera/front"
  auto pos = url.find("://");

  if VLIKELY (pos != std::string::npos) {
    cache.url = url;
    cache.entity_path.clear();
    cache.entity_path.reserve(url.size() - 2);
    cache.entity_path.append(url, 0, pos);
    cache.entity_path.push_back('/');
    cache.entity_path.append(url, pos + 3);
    return cache.entity_path;
  }

  cache.url = url;
  cache.entity_path = url;
  return cache.entity_path;
}

}  // namespace webviz
}  // namespace vlink
