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

#include "./proxy_bridge.h"

//
#include <vlink/base/elapsed_timer.h>
#include <vlink/base/functional.h>
#include <vlink/base/helpers.h>
#include <vlink/base/logger.h>
#include <vlink/base/message_loop.h>
#include <vlink/base/timer.h>
#include <vlink/base/utils.h>
#include <vlink/extension/discovery_viewer.h>
#include <vlink/impl/url.h>
#include <vlink/publisher.h>
#include <vlink/subscriber.h>
#include <vlink/version.h>

#ifdef VLINK_SUPPORT_SHM
#include <vlink/modules/shm_conf.h>
#endif

//
#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vlink {
namespace webviz {

using RawPub = Publisher<Bytes>;
using RawSub = Subscriber<Bytes>;

// ProxyBridge::Impl
struct ProxyBridge::Impl final {
  MessageLoop* data_callback_loop{nullptr};
  ProxyBridge::DataCallbackMode data_callback_mode{ProxyBridge::kQueued};
  ProxyBridge::DataCallback data_callback;
  // Protects data_callback against concurrent register vs dispatch.
  mutable std::shared_mutex data_callback_mtx;
};

ProxyBridge::ProxyBridge(const Config& config, MessageLoop* data_callback_loop) : impl_(std::make_unique<Impl>()) {
  impl_->data_callback_loop = data_callback_loop;
  impl_->data_callback_mode = config.data_callback_mode;
}

ProxyBridge::~ProxyBridge() = default;

bool ProxyBridge::parse_data_callback_mode(std::string_view value, DataCallbackMode& mode) {
  std::string normalized(value);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  if VLIKELY (normalized == "direct") {
    mode = ProxyBridge::kDirect;
    return true;
  }

  if VLIKELY (normalized == "queued" || normalized == "queue") {
    mode = ProxyBridge::kQueued;
    return true;
  }

  return false;
}

void ProxyBridge::register_data_callback(DataCallback&& callback) {
  std::unique_lock lock(impl_->data_callback_mtx);

  if VUNLIKELY (!callback) {
    impl_->data_callback = nullptr;
    return;
  }

  impl_->data_callback = std::move(callback);
}

void ProxyBridge::dispatch_data_callback(const ProxyAPI::Data& data) {
  {
    std::shared_lock lock(impl_->data_callback_mtx);

    if VUNLIKELY (!impl_->data_callback) {
      return;
    }
  }

  if VLIKELY (impl_->data_callback_mode == ProxyBridge::kDirect || impl_->data_callback_loop == nullptr) {
    std::shared_lock lock(impl_->data_callback_mtx);

    if VLIKELY (impl_->data_callback) {
      impl_->data_callback(data);
    }

    return;
  }

  impl_->data_callback_loop->post_task([this, queued_data = data]() {
    std::shared_lock lock(impl_->data_callback_mtx);

    if VLIKELY (impl_->data_callback) {
      impl_->data_callback(queued_data);
    }
  });
}

class ProxyApiBridge final : public ProxyBridge {
 public:
  ProxyApiBridge(const Config& config, MessageLoop* data_callback_loop)
      : ProxyBridge(config, data_callback_loop), config_(config) {
    ProxyAPI::Config proxy_api_config;
    proxy_api_config.role = config_.transport.role;
    proxy_api_config.domain_id = config_.transport.domain_id;
    proxy_api_config.dds_impl = config_.transport.dds_impl;
    proxy_api_config.security_key = config_.api.security_key;
    proxy_api_config.native = config_.transport.native;
    proxy_api_config.reliable = config_.api.reliable;
    proxy_api_config.direct = config_.api.direct;
    proxy_api_config.enable_tcp = config_.transport.enable_tcp;
    proxy_api_config.match_version = config_.api.match_version;
    proxy_api_config.allow_ip = config_.transport.bind_ip;
    proxy_api_config.peer_ip = config_.transport.peer_ip;
    proxy_api_config.buf_size = config_.transport.buf_size;
    proxy_api_config.mtu_size = config_.transport.mtu_size;

    proxy_api_ = std::make_unique<ProxyAPI>(proxy_api_config);
    proxy_api_->register_data_callback([this](const ProxyAPI::Data& data) { dispatch_data_callback(data); });
  }

  bool start() override {
    if VUNLIKELY (started_.exchange(true)) {
      return true;
    }

    proxy_api_->async_run();
    return true;
  }

  void stop() override {
    if VUNLIKELY (!started_.exchange(false)) {
      return;
    }

    proxy_api_->quit(true);
    proxy_api_->wait_for_quit();
  }

  void register_connect_callback(ConnectCallback&& callback) override {
    proxy_api_->register_connect_callback(std::move(callback));
  }

  void register_error_callback(ErrorCallback&& callback) override {
    proxy_api_->register_error_callback(std::move(callback));
  }

  void register_time_callback(TimeCallback&& callback) override {
    proxy_api_->register_time_callback(std::move(callback));
  }

  void register_info_callback(InfoCallback&& callback) override {
    proxy_api_->register_info_callback(std::move(callback));
  }

  bool send_control(const ProxyAPI::Control& control, bool async) override {
    return proxy_api_->send_control(control, async);
  }

  bool send_data(const ProxyAPI::Data& data) override { return proxy_api_->send_data(data); }

  const Config& get_config() const override { return config_; }

  bool can_control() const override { return config_.transport.role == ProxyAPI::kController; }

  bool can_inject() const override { return config_.transport.role == ProxyAPI::kController; }

  bool is_connected() const override { return proxy_api_->is_connected(); }

  std::string get_proxy_version() const override { return proxy_api_->get_proxy_version(); }

  std::unordered_set<std::string> get_proxy_hostnames() const override { return proxy_api_->get_proxy_hostnames(); }

 private:
  Config config_;
  std::unique_ptr<ProxyAPI> proxy_api_;
  std::atomic_bool started_{false};
};

class ProxyServerBridge final : public ProxyBridge {
 public:
  struct UrlState final {
    mutable std::shared_mutex state_mtx;
    std::unique_ptr<RawSub> sub;
    std::string ser;
    SchemaType schema{SchemaType::kUnknown};
    bool sub_error{false};
    std::atomic<uint64_t> next_subscribe_retry_us{0};
    int64_t previous_seq{0};
    size_t previous_size{0};
    double previous_loss{0.0};
    int64_t previous_latency_us{0};
    SampleLostInfo last_sample_info;
    std::atomic<int64_t> total_seq{0};
    std::atomic<int64_t> current_seq{0};
    std::atomic<size_t> current_size{0};
    std::atomic<int64_t> current_latency_us{0};
    std::atomic<uint64_t> first_seen_us{0};
    std::atomic<uint64_t> last_activity_us{0};
  };

  ProxyServerBridge(const Config& config, MessageLoop* data_callback_loop)
      : ProxyBridge(config, data_callback_loop), config_(config) {
    current_hostname_ = Utils::get_host_name();
    proxy_hostnames_.emplace(current_hostname_);
    has_intra_bind_ = !Utils::get_env("VLINK_INTRA_BIND").empty();

    if VLIKELY (config_.server.max_packet_size > 0.0) {
      max_packet_size_bytes_ = static_cast<size_t>(config_.server.max_packet_size * 1024.0 * 1024.0);
    }
  }

  ~ProxyServerBridge() override { stop(); }

  bool start() override {
    if VUNLIKELY (started_.exchange(true)) {
      return true;
    }

    auto fail_start = [this]() {
      dispatch_error(ProxyAPI::kUnknownError);
      time_timer_.stop();
      info_timer_.stop();
      time_timer_.detach();
      info_timer_.detach();
      discovery_viewer_.reset();
      started_.store(false);
#ifdef VLINK_SUPPORT_SHM

      if VLIKELY (shm_runtime_inited_) {
        ShmConf::deinit_runtime();
        shm_runtime_inited_ = false;
      }
#endif
      return false;
    };

    try {
#ifdef VLINK_SUPPORT_SHM

      if VLIKELY (config_.server.use_iox) {
        const auto runtime_name = Utils::get_app_name() + "_" + Utils::get_pid_str();
        ShmConf::init_roudi(config_.server.iox_config, config_.server.iox_strategy, config_.server.iox_monitoring);
        ShmConf::init_runtime(runtime_name, true);
        ShmConf::global_init();
        shm_runtime_inited_ = true;
      }
#else

      if VUNLIKELY (config_.server.use_iox) {
        MLOG_E("proxy_server mode requested use_iox, but SHM support is not enabled");
        dispatch_error(ProxyAPI::kUnknownError);
        started_.store(false);
        return false;
      }
#endif

      auto filter_type = config_.transport.native ? DiscoveryViewer::FilterType::kFilterNative
                                                  : DiscoveryViewer::FilterType::kFilterAvailable;
      discovery_viewer_ = std::make_unique<DiscoveryViewer>(filter_type);

      session_elapsed_.stop();
      session_elapsed_.start();

      if VUNLIKELY (!time_timer_.attach(discovery_viewer_.get())) {
        MLOG_E("proxy_server mode failed to attach time timer");
        return fail_start();
      }

      time_timer_.set_interval(1000);
      time_timer_.set_loop_count(Timer::kInfinite);
      time_timer_.set_callback([this]() { dispatch_time(); });

      if VUNLIKELY (!info_timer_.attach(discovery_viewer_.get())) {
        MLOG_E("proxy_server mode failed to attach info timer");
        return fail_start();
      }

      info_timer_.set_interval(1000);
      info_timer_.set_loop_count(Timer::kInfinite);
      info_timer_.set_callback([this]() { refresh_info(); });

      if VUNLIKELY (!discovery_viewer_->async_run()) {
        MLOG_E("proxy_server mode failed to start discovery viewer loop");
        return fail_start();
      }

      time_timer_.start();
      info_timer_.start();
      dispatch_time();

      connected_.store(true);
      dispatch_error(ProxyAPI::kNoError);
      dispatch_connected(true);
      schedule_refresh_info();
      return true;
    } catch (const Exception::RuntimeError& e) {
      MLOG_E("proxy_server mode startup failed: {}", e.what());
      return fail_start();
    } catch (const std::exception& e) {
      MLOG_E("proxy_server mode startup failed: {}", e.what());
      return fail_start();
    }
  }

  void stop() override {
    if VUNLIKELY (!started_.exchange(false)) {
      return;
    }

    connected_.store(false);
    refresh_pending_.store(false);

    time_timer_.stop();
    info_timer_.stop();
    time_timer_.detach();
    info_timer_.detach();
    session_elapsed_.stop();

    {
      std::unique_lock lock(url_state_mtx_);

      for (auto& entry : url_states_) {
        auto& state = entry.second;
        std::unique_lock state_lock(state.state_mtx);
        state.sub.reset();
        state.ser.clear();
        state.schema = SchemaType::kUnknown;
      }

      url_states_.clear();
    }

    {
      std::unique_lock lock(pub_mtx_);
      pub_map_.clear();
    }

    if VLIKELY (discovery_viewer_) {
      discovery_viewer_->quit(true);
      discovery_viewer_->wait_for_quit();
      discovery_viewer_.reset();
    }

#ifdef VLINK_SUPPORT_SHM

    if VLIKELY (shm_runtime_inited_) {
      ShmConf::deinit_runtime();
      shm_runtime_inited_ = false;
    }
#endif

    dispatch_connected(false);
  }

  void register_connect_callback(ConnectCallback&& callback) override {
    bool fire_now = false;

    {
      std::unique_lock lock(callback_mtx_);
      connect_callback_ = std::move(callback);
      fire_now = connect_callback_ && connected_.load();
    }

    if VUNLIKELY (fire_now) {
      std::shared_lock lock(callback_mtx_);

      if VLIKELY (connect_callback_) {
        connect_callback_(true);
      }
    }
  }

  void register_error_callback(ErrorCallback&& callback) override {
    bool fire_now = false;
    ProxyAPI::Error error = ProxyAPI::kNoError;

    {
      std::unique_lock lock(callback_mtx_);
      error_callback_ = std::move(callback);
      error = current_error_.load();
      fire_now = error_callback_ && error != ProxyAPI::kNoError;
    }

    if VUNLIKELY (fire_now) {
      std::shared_lock lock(callback_mtx_);

      if VLIKELY (error_callback_) {
        error_callback_(error);
      }
    }
  }

  void register_time_callback(TimeCallback&& callback) override {
    bool fire_now = false;

    {
      std::unique_lock lock(callback_mtx_);
      time_callback_ = std::move(callback);
      fire_now = time_callback_ && connected_.load();
    }

    if VUNLIKELY (fire_now) {
      std::shared_lock lock(callback_mtx_);

      if VLIKELY (time_callback_) {
        time_callback_(ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMicro, false),
                       ProxyServerBridge::steady_now_us());
      }
    }
  }

  void register_info_callback(InfoCallback&& callback) override {
    std::unique_lock lock(callback_mtx_);

    info_callback_ = std::move(callback);
  }

  bool send_control(const ProxyAPI::Control& control, bool async) override {
    if VUNLIKELY (!started_.load() || !can_control()) {
      return false;
    }

    if VUNLIKELY (!async) {
      apply_control(control);
      dispatch_time();
      return true;
    }

    if VUNLIKELY (!discovery_viewer_) {
      return false;
    }

    return discovery_viewer_->post_task([this, control]() {
      if VUNLIKELY (!started_.load()) {
        return;
      }

      apply_control(control);
      dispatch_time();
    });
  }

  bool send_data(const ProxyAPI::Data& data) override {
    if VUNLIKELY (!started_.load() || !can_inject()) {
      return false;
    }

    const auto normalized_schema_type = SchemaData::is_valid_type(data.schema) ? data.schema : SchemaType::kUnknown;

    if VUNLIKELY (data.url.empty() || data.ser.empty()) {
      return false;
    }

    std::shared_lock lock(pub_mtx_);
    auto pub_iter = pub_map_.find(data.url);

    if VUNLIKELY (pub_iter == pub_map_.end() || !pub_iter->second) {
      return false;
    }

    if VUNLIKELY (pub_iter->second->get_ser_type() != data.ser ||
                  pub_iter->second->get_schema_type() != normalized_schema_type) {
      return false;
    }

    return pub_iter->second->publish(data.raw, true);
  }

  const Config& get_config() const override { return config_; }

  bool can_control() const override { return started_.load() && config_.transport.role == ProxyAPI::kController; }

  bool can_inject() const override { return started_.load() && config_.transport.role == ProxyAPI::kController; }

  bool is_connected() const override { return connected_.load(); }

  std::string get_proxy_version() const override { return VLINK_VERSION; }

  std::unordered_set<std::string> get_proxy_hostnames() const override { return proxy_hostnames_; }

 private:
  static bool has_impl_type(uint32_t type, uint32_t impl_type) { return (type & impl_type) != 0U; }

  static uint64_t steady_now_us() { return ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMicro, false); }

  static bool contains_lower_ascii(std::string_view text, std::string_view lower_pattern) {
    if VUNLIKELY (lower_pattern.empty()) {
      return false;
    }

    if VUNLIKELY (text.size() < lower_pattern.size()) {
      return false;
    }

    auto match_iter = std::search(  // NOLINT
        text.begin(), text.end(), lower_pattern.begin(), lower_pattern.end(),
        [](char lhs, char rhs) { return static_cast<char>(std::tolower(static_cast<unsigned char>(lhs))) == rhs; });
    return match_iter != text.end();
  }

  void dispatch_connected(bool connected) {
    std::shared_lock lock(callback_mtx_);

    if VLIKELY (connect_callback_) {
      connect_callback_(connected);
    }
  }

  void dispatch_error(ProxyAPI::Error error) {
    if VUNLIKELY (current_error_.exchange(error) == error) {
      return;
    }

    std::shared_lock lock(callback_mtx_);

    if VLIKELY (error_callback_) {
      error_callback_(error);
    }
  }

  void dispatch_time() {
    if VUNLIKELY (!started_.load()) {
      return;
    }

    std::shared_lock lock(callback_mtx_);

    if VUNLIKELY (!time_callback_) {
      return;
    }

    time_callback_(ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMicro, false), ProxyServerBridge::steady_now_us());
  }

  void dispatch_info(const std::vector<ProxyAPI::Info>& info_list) {
    std::shared_lock lock(callback_mtx_);

    if VLIKELY (info_callback_) {
      info_callback_(info_list);
    }
  }

  void schedule_refresh_info() {
    if VUNLIKELY (!started_.load() || !discovery_viewer_) {
      return;
    }

    bool expected = false;

    if VUNLIKELY (!refresh_pending_.compare_exchange_strong(expected, true)) {
      return;
    }

    discovery_viewer_->post_task([this]() {
      refresh_pending_.store(false);

      if VLIKELY (started_.load()) {
        refresh_info();
      }
    });
  }

  void apply_control(const ProxyAPI::Control& control) {
    {
      std::unique_lock lock(control_mtx_);
      subscribe_meta_map_.clear();
      publish_meta_map_.clear();
      filter_terms_.clear();

      for (const auto& meta : control.url_meta_list) {
        if VLIKELY (meta.type == kPublisher) {
          auto normalized_meta = meta;
          normalized_meta.schema = SchemaData::is_valid_type(meta.schema) ? meta.schema : SchemaType::kUnknown;
          publish_meta_map_[meta.url] = std::move(normalized_meta);
        } else {
          auto normalized_meta = meta;
          normalized_meta.schema = SchemaData::is_valid_type(meta.schema) ? meta.schema : SchemaType::kUnknown;

          if VLIKELY (!normalized_meta.ser.empty()) {
            subscribe_meta_map_[meta.url] = std::move(normalized_meta);
          }
        }
      }

      filter_by_process_ = control.filter_by_process;
      filter_type_ = control.filter_type;
      auto filter_list = Helpers::split_any(control.filter_str);
      filter_terms_.reserve(filter_list.size());

      for (const auto& filter : filter_list) {
        if VUNLIKELY (filter.empty()) {
          continue;
        }

        filter_terms_.emplace_back(filter);
        auto& lower_filter = filter_terms_.back();
        std::transform(lower_filter.begin(), lower_filter.end(), lower_filter.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      }
    }

    session_elapsed_.restart();
    mode_.store(control.mode);
    refresh_publishers();
    schedule_refresh_info();
  }

  bool is_visible_info(const DiscoveryViewer::Info& info) const {
    if VUNLIKELY (!has_intra_bind_ && Url::is_intra_type(info.url)) {
      return false;
    }

    return true;
  }

  bool can_stream_info(const DiscoveryViewer::Info& info) const {
    return is_visible_info(info) && ProxyServerBridge::has_impl_type(info.type, kPublisher);
  }

  bool passes_filter(const DiscoveryViewer::Info& info) const {
    std::shared_lock lock(control_mtx_);

    if VLIKELY (filter_terms_.empty()) {
      return passes_filter_type_unlocked(info.type);
    }

    bool contains = false;

    if VUNLIKELY (filter_by_process_) {
      for (const auto& process : info.process_list) {
        for (const auto& filter : filter_terms_) {
          if VUNLIKELY (filter.empty()) {
            continue;
          }

          if VLIKELY (ProxyServerBridge::contains_lower_ascii(process.name, filter)) {
            contains = true;
            break;
          }
        }

        if VLIKELY (contains) {
          break;
        }
      }
    } else {
      for (const auto& filter : filter_terms_) {
        if VUNLIKELY (filter.empty()) {
          continue;
        }

        if VLIKELY (ProxyServerBridge::contains_lower_ascii(info.url, filter)) {
          contains = true;
          break;
        }
      }
    }

    if VUNLIKELY (!contains) {
      return false;
    }

    return passes_filter_type_unlocked(info.type);
  }

  bool passes_filter_type_unlocked(uint32_t type) const {
    switch (filter_type_) {
      case 0:
        return true;
      case 1:
        return ProxyServerBridge::has_impl_type(type, kPublisher) &&
               ProxyServerBridge::has_impl_type(type, kSubscriber);
      case 2:
        return ProxyServerBridge::has_impl_type(type, kServer) && ProxyServerBridge::has_impl_type(type, kClient);
      case 3:
        return ProxyServerBridge::has_impl_type(type, kSetter) && ProxyServerBridge::has_impl_type(type, kGetter);
      case 4:
        return ProxyServerBridge::has_impl_type(type, kPublisher) ||
               ProxyServerBridge::has_impl_type(type, kSubscriber);
      case 5:
        return ProxyServerBridge::has_impl_type(type, kServer) || ProxyServerBridge::has_impl_type(type, kClient);
      case 6:
        return ProxyServerBridge::has_impl_type(type, kSetter) || ProxyServerBridge::has_impl_type(type, kGetter);
      case 7:
        return ProxyServerBridge::has_impl_type(type, kPublisher);
      case 8:
        return ProxyServerBridge::has_impl_type(type, kSubscriber);
      case 9:
        return ProxyServerBridge::has_impl_type(type, kServer);
      case 10:
        return ProxyServerBridge::has_impl_type(type, kClient);
      case 11:
        return ProxyServerBridge::has_impl_type(type, kSetter);
      case 12:
        return ProxyServerBridge::has_impl_type(type, kGetter);
      default:
        return true;
    }
  }

  bool should_subscribe(const std::string& url) const {
    auto mode = mode_.load();

    if VUNLIKELY (mode == ProxyAPI::kOffline || mode == ProxyAPI::kPlay || mode == ProxyAPI::kEdit) {
      return false;
    }

    if VUNLIKELY (mode == ProxyAPI::kObserveAll || mode == ProxyAPI::kAutoAndObserveAll) {
      return true;
    }

    std::shared_lock lock(control_mtx_);
    return subscribe_meta_map_.count(url) != 0;
  }

  void refresh_publishers() {
    std::unordered_map<std::string, ProxyAPI::UrlMeta> desired_pubs;
    std::vector<ProxyAPI::UrlMeta> to_create;
    auto mode = mode_.load();

    if VLIKELY (can_inject() && (mode == ProxyAPI::kPlay || mode == ProxyAPI::kEdit || mode == ProxyAPI::kAuto ||
                                 mode == ProxyAPI::kAutoAndObserveAll)) {
      std::shared_lock lock(control_mtx_);
      desired_pubs = publish_meta_map_;
    }

    {
      std::unique_lock lock(pub_mtx_);

      for (auto pub_iter = pub_map_.begin(); pub_iter != pub_map_.end();) {
        if VUNLIKELY (desired_pubs.count(pub_iter->first) == 0) {
          pub_iter = pub_map_.erase(pub_iter);
        } else {
          ++pub_iter;
        }
      }

      for (const auto& [url, meta] : desired_pubs) {
        if VUNLIKELY (meta.ser.empty()) {
          continue;
        }

        auto pub_iter = pub_map_.find(url);
        const auto normalized_schema_type = SchemaData::is_valid_type(meta.schema) ? meta.schema : SchemaType::kUnknown;

        if VLIKELY (pub_iter != pub_map_.end() && pub_iter->second) {
          if VLIKELY (pub_iter->second->get_ser_type() == meta.ser &&
                      pub_iter->second->get_schema_type() == normalized_schema_type) {
            continue;
          }

          pub_map_.erase(pub_iter);
        }

        to_create.emplace_back(meta);
      }
    }

    for (const auto& meta : to_create) {
      try {
        const auto normalized_schema_type = SchemaData::is_valid_type(meta.schema) ? meta.schema : SchemaType::kUnknown;
        auto pub = std::make_unique<RawPub>(meta.url, InitType::kWithoutInit);

        pub->set_ser_type(meta.ser, normalized_schema_type);

        ProxyBridge::apply_transport(*pub, config_.transport, false);
        pub->set_discovery_enabled(true);
        pub->init();

        std::unique_lock lock(pub_mtx_);
        auto pub_iter = pub_map_.find(meta.url);

        if VUNLIKELY (pub_iter != pub_map_.end() && pub_iter->second && pub_iter->second->get_ser_type() == meta.ser &&
                      pub_iter->second->get_schema_type() == normalized_schema_type) {
          continue;
        }

        pub_map_[meta.url] = std::move(pub);
      } catch (const Exception::RuntimeError&) {
        MLOG_W("proxy_server mode failed to initialize publisher: {}", meta.url);
      }
    }
  }

  void sync_subscriber(const DiscoveryViewer::Info& info, UrlState& state) {
    bool want_subscribe = false;
    const auto now_us = ProxyServerBridge::steady_now_us();
    const auto discovered_schema =
        SchemaData::is_valid_type(info.schema_type) ? info.schema_type : SchemaType::kUnknown;
    auto mode = mode_.load();
    std::string target_ser;
    SchemaType target_schema = SchemaType::kUnknown;
    bool valid_wire_meta = false;

    if VLIKELY (can_stream_info(info) && passes_filter(info)) {
      want_subscribe = should_subscribe(info.url);
    }

    if VUNLIKELY (mode == ProxyAPI::kObserveAll || mode == ProxyAPI::kAutoAndObserveAll) {
      target_ser = info.ser_type;
      target_schema = discovered_schema;
      valid_wire_meta = !target_ser.empty();
    } else {
      std::shared_lock control_lock(control_mtx_);
      auto meta_iter = subscribe_meta_map_.find(info.url);

      if VLIKELY (meta_iter != subscribe_meta_map_.end()) {
        target_ser = meta_iter->second.ser;
        target_schema = meta_iter->second.schema;
        valid_wire_meta = !target_ser.empty() && !info.ser_type.empty() && info.ser_type == target_ser &&
                          discovered_schema == target_schema;
      }
    }

    bool has_sub = false;
    bool sub_error = false;
    uint64_t next_retry_us = 0;

    {
      std::unique_lock lock(state.state_mtx);
      has_sub = state.sub != nullptr;
      sub_error = state.sub_error;
      next_retry_us = state.next_subscribe_retry_us.load();
    }

    if VUNLIKELY (!want_subscribe || !valid_wire_meta) {
      if VLIKELY (has_sub) {
        std::unique_lock lock(state.state_mtx);
        state.sub.reset();
        state.ser.clear();
        state.schema = SchemaType::kUnknown;
        state.sub_error = false;
        state.next_subscribe_retry_us.store(0);
      }

      return;
    }

    {
      std::unique_lock lock(state.state_mtx);
      has_sub = state.sub != nullptr;

      if VUNLIKELY (has_sub && (state.ser != target_ser || state.schema != target_schema)) {
        state.sub.reset();
        state.ser.clear();
        state.schema = SchemaType::kUnknown;
        state.sub_error = false;
        state.next_subscribe_retry_us.store(0);
        has_sub = false;
      }
    }

    if VUNLIKELY (has_sub) {
      return;
    }

    if VUNLIKELY (sub_error && now_us < next_retry_us) {
      return;
    }

    try {
      auto sub = std::make_unique<RawSub>(info.url, InitType::kWithoutInit);

      sub->set_latency_and_lost_enabled(true);
      ProxyBridge::apply_transport(*sub, config_.transport, true);
      sub->set_ser_type(target_ser, target_schema);
      sub->set_safety_quit(true);
      sub->init();

      auto on_data = [this, state_ptr = &state, sub_ptr = sub.get(), url = info.url, ser = target_ser,
                      schema = target_schema](const Bytes& bytes) {
        if VUNLIKELY (!started_.load()) {
          return;
        }

        state_ptr->current_seq.fetch_add(1);
        state_ptr->current_size.fetch_add(bytes.size());
        state_ptr->current_latency_us.fetch_add(sub_ptr->get_latency());
        state_ptr->last_activity_us.store(ProxyServerBridge::steady_now_us());

        if VUNLIKELY (max_packet_size_bytes_ > 0 && bytes.size() > max_packet_size_bytes_) {
          return;
        }

        if VUNLIKELY (!should_subscribe(url)) {
          return;
        }

        auto timestamp = session_elapsed_.get();
        auto seq = state_ptr->total_seq.fetch_add(1);
        ProxyAPI::Data data{url, ser, schema, Bytes::shallow_copy(bytes.data(), bytes.size()), timestamp, seq};
        dispatch_data_callback(data);
      };

      if VUNLIKELY (!sub->listen(std::move(on_data))) {
        throw Exception::RuntimeError("subscriber listen returned false");
      }

      {
        std::unique_lock lock(state.state_mtx);
        state.sub = std::move(sub);
        state.ser = target_ser;
        state.schema = target_schema;
        state.sub_error = false;
        state.next_subscribe_retry_us.store(0);
      }
    } catch (const Exception::RuntimeError&) {
      std::unique_lock lock(state.state_mtx);
      state.sub_error = true;
      state.next_subscribe_retry_us.store(now_us + 2000U * 1000U);
      MLOG_W("proxy_server mode failed to initialize subscriber: {}", info.url);
    }
  }

  void refresh_info() {
    if VUNLIKELY (!started_.load() || !discovery_viewer_) {
      return;
    }

    const auto mode = mode_.load();

    if VUNLIKELY (mode != ProxyAPI::kObserveOne && mode != ProxyAPI::kObserveAll && mode != ProxyAPI::kRecord &&
                  mode != ProxyAPI::kAuto && mode != ProxyAPI::kAutoAndObserveAll) {
      {
        std::unique_lock lock(url_state_mtx_);

        for (auto& entry : url_states_) {
          auto& state = entry.second;
          std::unique_lock state_lock(state.state_mtx);
          state.sub.reset();
          state.ser.clear();
          state.schema = SchemaType::kUnknown;
          state.sub_error = false;
          state.next_subscribe_retry_us.store(0);
        }
      }

      dispatch_info({});
      return;
    }

    auto discovery_info_list = discovery_viewer_->get_info_list();
    std::vector<const DiscoveryViewer::Info*> visible_info_list;
    visible_info_list.reserve(discovery_info_list.size());

    std::unordered_set<std::string> current_urls;
    current_urls.reserve(discovery_info_list.size());

    for (const auto& info : discovery_info_list) {
      if VUNLIKELY (!is_visible_info(info) || !passes_filter(info)) {
        continue;
      }

      visible_info_list.emplace_back(&info);
      current_urls.emplace(info.url);
    }

    {
      std::unique_lock lock(url_state_mtx_);

      for (auto& entry : url_states_) {
        if VLIKELY (current_urls.count(entry.first) != 0) {
          continue;
        }

        auto& state = entry.second;
        std::unique_lock state_lock(state.state_mtx);
        state.sub.reset();
        state.ser.clear();
        state.schema = SchemaType::kUnknown;
        state.sub_error = false;
        state.next_subscribe_retry_us.store(0);
      }

      for (const auto* info : visible_info_list) {
        auto now_us = ProxyServerBridge::steady_now_us();
        auto [state_iter, inserted] = url_states_.try_emplace(info->url);

        if VLIKELY (!inserted) {
          continue;
        }

        state_iter->second.first_seen_us.store(now_us);
        state_iter->second.last_activity_us.store(0);
      }
    }

    for (const auto* info : visible_info_list) {
      std::shared_lock lock(url_state_mtx_);
      auto state_iter = url_states_.find(info->url);

      if VUNLIKELY (state_iter == url_states_.end()) {
        continue;
      }

      sync_subscriber(*info, state_iter->second);
    }

    auto now_us = ProxyServerBridge::steady_now_us();
    std::vector<ProxyAPI::Info> info_list;
    info_list.reserve(visible_info_list.size());

    for (const auto* info : visible_info_list) {
      std::shared_lock lock(url_state_mtx_);
      auto state_iter = url_states_.find(info->url);

      if VUNLIKELY (state_iter == url_states_.end()) {
        continue;
      }

      auto& state = state_iter->second;

      ProxyAPI::Info proxy_info;
      proxy_info.type = info->type;
      proxy_info.url = info->url;
      proxy_info.ser = info->ser_type;
      proxy_info.schema = SchemaData::is_valid_type(info->schema_type) ? info->schema_type : SchemaType::kUnknown;
      proxy_info.process_list.reserve(info->process_list.size());

      for (const auto& process : info->process_list) {
        ProxyAPI::Process proxy_process;
        proxy_process.type = process.type;
        proxy_process.host = process.host;
        proxy_process.pid = process.pid;
        proxy_process.name = process.name;
        proxy_process.ip = process.ip;
        proxy_info.process_list.emplace_back(std::move(proxy_process));
      }
      proxy_info.status = ProxyAPI::kInvalid;
      proxy_info.freq = 0.0F;
      proxy_info.rate = 0;
      proxy_info.loss = 0.0F;
      proxy_info.latency = -1.0F;

      if VUNLIKELY (!can_stream_info(*info)) {
        info_list.emplace_back(std::move(proxy_info));
        continue;
      }

      proxy_info.status = ProxyAPI::kPending;

      auto current_seq = state.current_seq.exchange(0);
      auto current_size = state.current_size.exchange(0);
      auto current_latency_us = state.current_latency_us.exchange(0);

      double current_loss = 0.0;

      {
        std::unique_lock state_lock(state.state_mtx);
        auto* sub = state.sub.get();

        if VLIKELY (sub) {
          const auto sample_info = sub->get_lost();

          if VLIKELY (sample_info.total >= state.last_sample_info.total &&
                      sample_info.lost >= state.last_sample_info.lost) {
            auto total_delta = sample_info.total - state.last_sample_info.total;
            auto lost_delta = sample_info.lost - state.last_sample_info.lost;

            if VUNLIKELY (total_delta > 0 && lost_delta > 0) {
              current_loss = static_cast<double>(lost_delta) / static_cast<double>(total_delta);
            }
          }

          state.last_sample_info = sample_info;
        }

        auto current_latency_avg = current_seq > 0 ? current_latency_us / current_seq : 0;
        auto weighted_freq = (state.previous_seq + current_seq * 2) / 3.0;
        auto weighted_rate = (state.previous_size + current_size * 2) / 3.0;
        auto weighted_loss = (state.previous_loss + current_loss * 2.0) / 3.0;
        auto weighted_latency = (state.previous_latency_us + current_latency_avg * 2) / 3.0;

        proxy_info.freq = static_cast<float>(weighted_freq);
        proxy_info.rate = static_cast<uint64_t>(weighted_rate);
        proxy_info.loss = static_cast<float>(weighted_loss);

        if VUNLIKELY (weighted_latency > 5000'000'000.0 || weighted_latency < -500'000.0) {
          proxy_info.latency = -2.0F;
        } else if VUNLIKELY (weighted_latency < 0.0) {
          proxy_info.latency = 0.0F;
        } else {
          proxy_info.latency = static_cast<float>(weighted_latency / 1000.0);
        }

        auto last_activity_us = state.last_activity_us.load();
        auto first_seen_us = state.first_seen_us.load();

        if VLIKELY (current_seq > 0 || state.previous_seq > 0) {
          proxy_info.status = ProxyAPI::kActive;
        } else if VUNLIKELY (((last_activity_us > 0 && now_us - last_activity_us >= 2000U * 1000U) ||
                              (last_activity_us == 0 && now_us - first_seen_us >= 2000U * 1000U))) {
          proxy_info.status = ProxyAPI::kInActive;
        } else {
          proxy_info.status = ProxyAPI::kPending;
        }

        if VUNLIKELY (current_seq == 0 && state.previous_seq == 0) {
          proxy_info.latency = -1.0F;
        }

        state.previous_seq = current_seq;
        state.previous_size = current_size;
        state.previous_loss = current_loss;
        state.previous_latency_us = current_latency_avg;
      }

      info_list.emplace_back(std::move(proxy_info));
    }

    dispatch_info(info_list);
  }

  Config config_;
  std::unique_ptr<DiscoveryViewer> discovery_viewer_;
  Timer time_timer_;
  Timer info_timer_;
  std::atomic_bool started_{false};
  std::atomic_bool connected_{false};
  std::atomic_bool refresh_pending_{false};
  std::atomic<ProxyAPI::Mode> mode_{ProxyAPI::kOffline};
  std::string current_hostname_;
  std::unordered_set<std::string> proxy_hostnames_;
  size_t max_packet_size_bytes_{0};
  bool has_intra_bind_{false};
  bool shm_runtime_inited_{false};

  // Protects connect/error/time/info callbacks against concurrent register vs dispatch.
  mutable std::shared_mutex callback_mtx_;
  ConnectCallback connect_callback_;
  ErrorCallback error_callback_;
  std::atomic<ProxyAPI::Error> current_error_{ProxyAPI::kNoError};
  TimeCallback time_callback_;
  InfoCallback info_callback_;

  mutable std::shared_mutex control_mtx_;
  std::unordered_map<std::string, ProxyAPI::UrlMeta> subscribe_meta_map_;
  std::unordered_map<std::string, ProxyAPI::UrlMeta> publish_meta_map_;
  std::vector<std::string> filter_terms_;
  bool filter_by_process_{false};
  uint32_t filter_type_{0};

  mutable std::shared_mutex url_state_mtx_;
  std::unordered_map<std::string, UrlState> url_states_;

  std::shared_mutex pub_mtx_;
  std::unordered_map<std::string, std::unique_ptr<RawPub>> pub_map_;

  ElapsedTimer session_elapsed_{ElapsedTimer::kMicro};
};

std::unique_ptr<ProxyBridge> ProxyBridge::create(const Config& config, MessageLoop* data_callback_loop) {
  if VLIKELY (config.interface_mode == ProxyBridge::kProxyApi) {
    return std::make_unique<ProxyApiBridge>(config, data_callback_loop);
  }

  return std::make_unique<ProxyServerBridge>(config, data_callback_loop);
}

const char* ProxyBridge::to_string(InterfaceMode mode) {
  switch (mode) {
    case ProxyBridge::kProxyApi:
      return "proxy_api";
    case ProxyBridge::kProxyServer:
      return "proxy_server";
    default:
      return "unknown";
  }
}

bool ProxyBridge::parse_interface_mode(std::string_view value, InterfaceMode& mode) {
  std::string normalized(value);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  if VLIKELY (normalized == "proxy_api") {
    mode = ProxyBridge::kProxyApi;
    return true;
  }

  if VLIKELY (normalized == "proxy_server") {
    mode = ProxyBridge::kProxyServer;
    return true;
  }

  return false;
}

bool ProxyBridge::is_dds_transport(std::string_view url) {
  return Helpers::has_startwith(url, "dds://") || Helpers::has_startwith(url, "ddsc://") ||
         Helpers::has_startwith(url, "ddsr://") || Helpers::has_startwith(url, "ddst://");
}

}  // namespace webviz
}  // namespace vlink
