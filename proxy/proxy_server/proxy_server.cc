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

#include <vlink/external/proxy_server.h>
//
#include <vlink/base/elapsed_timer.h>
#include <vlink/base/helpers.h>
#include <vlink/base/plugin.h>
#include <vlink/base/utils.h>
#include <vlink/base/uuid.h>
#include <vlink/extension/discovery_viewer.h>
#include <vlink/extension/runnable_plugin_interface.h>
#include <vlink/version.h>
#include <vlink/vlink.h>
#include <vlink/zerocopy/proxy_data.h>
//
#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <limits>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#undef min
#undef max
#undef GetMessage
#endif

#if __has_include(<unistd.h>)
#include <unistd.h>
#endif

#include "../proxy_common.h"

namespace vlink {

[[maybe_unused]] static constexpr int kCounterCache{2};
[[maybe_unused]] static constexpr int kCounterWeight{2};
[[maybe_unused]] static constexpr int kCollectInterval{1000};

[[maybe_unused]] static constexpr size_t kMaxTaskSize{100000U};
[[maybe_unused]] static constexpr size_t kMaxTaskElapsed{10000U};
[[maybe_unused]] static constexpr uint32_t kMaxSubMissCount{3U};
[[maybe_unused]] static constexpr int kTimeoutWarnInterval{10000};

using RawPub = Publisher<Bytes>;
using RawSub = Subscriber<Bytes>;

using DataPub = Publisher<zerocopy::ProxyData>;
using DataSub = Subscriber<zerocopy::ProxyData>;

using TimePub = SecurityPublisher<proxy::TimePacket>;
using InfoPub = SecurityPublisher<proxy::InfoListPacket>;
using ControlSub = SecuritySubscriber<proxy::ControlPacket>;

template <typename NodeT>
static void apply_topic_transport(NodeT& node, const ProxyServer::Config& config, const std::string& native_ip) {
  if (config.native_mode) {
    node.set_property("dds.ip", native_ip);
  }
}

#if VLINK_PROXY_ENABLE_HANDSHAKE
using HandshakeSrv = SecurityServer<proxy::HandshakeReqPacket, proxy::HandshakeRespPacket>;
#endif

struct ProxySubMeta final {
  std::string ser;
  SchemaType schema{SchemaType::kUnknown};
};

struct ProxySubEntry final {
  std::shared_ptr<RawSub> node;
  std::string ser;
  SchemaType schema{SchemaType::kUnknown};
  bool getter_semantics{false};
};

// ProxyServerGlobal
struct ProxyServerGlobal final {
  std::atomic_bool has_init{false};
  std::atomic_bool has_quit{false};

  static ProxyServerGlobal& get() {
    static ProxyServerGlobal instance;
    return instance;
  }

 private:
  ProxyServerGlobal() = default;
};

// ProxyForwardLoop
class ProxyForwardLoop final : public MessageLoop {
 protected:
  size_t get_max_task_count() const override { return kMaxTaskSize; }

  uint32_t get_max_elapsed_time() const override { return kMaxTaskElapsed; }

  void on_task_timeout(MessageLoop::Callback&& callback, uint32_t elapsed_time) override {
    (void)callback;

    VLOG_W_EVERY_MS(kTimeoutWarnInterval, "ProxyForwardLoop: Task was dropped after ", elapsed_time,
                    "ms, the forward loop is busy.");
  }
};

// ProxyServer::Impl
struct ProxyServer::Impl final {  // NOLINT(clang-analyzer-optin.performance.Padding)
  struct PubEntry final {
    std::shared_ptr<RawPub> node;
    ImplType type{kPublisher};
  };

  std::atomic<uint32_t> control_id{0};
  std::atomic<uint64_t> control_generation{0};
  std::atomic<ProxyAPI::Mode> mode{ProxyAPI::kOffline};

  std::string current_host_name;
  std::string current_machine_id;
  std::vector<std::string> current_ip_list;
  std::string native_ip;
  std::string token;

  size_t real_max_packet_size{0};

  ProxyServer::Config config;

  ProxyAPI::DataCallback data_callback;
  ProxyAPI::InfoCallback info_callback;
  ProxyAPI::TimeCallback time_callback;

  std::shared_ptr<DiscoveryViewer> discovery_viewer;
  std::shared_ptr<DataPub> data_pub;
  std::shared_ptr<DataSub> data_sub;
  std::shared_ptr<TimePub> time_pub;
  std::shared_ptr<InfoPub> info_pub;
  std::shared_ptr<ControlSub> control_sub;
#if VLINK_PROXY_ENABLE_HANDSHAKE
  std::shared_ptr<HandshakeSrv> handshake_srv;
#endif

  bool filter_by_process{false};
  std::vector<std::string> filter_list;
  uint32_t filter_type{0};

  std::unordered_map<std::string, PubEntry> pub_ptr_map;
  std::unordered_map<std::string, ProxySubEntry> sub_ptr_map;
  std::unordered_map<std::string, std::atomic<int64_t>> sub_total_seq_map;
  std::unordered_map<std::string, std::atomic<int64_t>> sub_seq_map;
  std::unordered_map<std::string, std::atomic<size_t>> sub_size_map;
  std::unordered_map<std::string, std::atomic<double>> sub_lost_map;
  std::unordered_map<std::string, std::atomic<int64_t>> sub_lat_map;
  std::unordered_map<std::string, ElapsedTimer> sub_elapsed_map;
  std::unordered_map<std::string, std::deque<int64_t>> sub_seq_buffer_map;
  std::unordered_map<std::string, uint32_t> sub_miss_map;
  std::unordered_map<std::string, std::deque<size_t>> sub_size_buffer_map;
  std::unordered_map<std::string, std::deque<double>> sub_lost_buffer_map;
  std::unordered_map<std::string, std::deque<int64_t>> sub_lat_buffer_map;
  std::unordered_map<std::string, SampleLostInfo> sub_last_sample_map;
  std::unordered_set<std::string> sub_error_url_set;
  std::unordered_set<std::string> pub_error_url_set;
  std::unordered_set<std::string> sub_urls;
  std::unordered_map<std::string, ProxySubMeta> requested_sub_meta_map;
  std::shared_mutex control_mtx;
  std::shared_mutex pubs_mtx;
  std::mutex subs_mtx;
  ElapsedTimer boot_elapsed{ElapsedTimer::kMicro};
  ElapsedTimer main_elapsed{ElapsedTimer::kMicro};
  ElapsedTimer info_elapsed_timer;

  Timer time_timer;
  Timer info_timer;

  ProxyForwardLoop forward_loop;

  std::unique_ptr<ProxyAPI> bridge_api;
  std::unordered_map<std::string, std::shared_ptr<RawPub>> bridge_pubs;
  std::unordered_map<std::string, std::shared_ptr<RawSub>> bridge_readers;
  std::unordered_map<std::string, ProxyAPI::Info> bridge_subs;
  std::vector<ProxyAPI::UrlMeta> bridge_selection;
  std::shared_mutex bridge_mtx;
  bool bridge_same_machine{false};
  std::vector<std::string> bridge_filters;

  Plugin runnable_plugin;
  std::vector<std::shared_ptr<RunablePluginInterface>> runnable_interface_list;

  bool has_intra_bind{false};
};

// ProxyServer
ProxyServer::ProxyServer(const Config& config) : impl_(std::make_unique<Impl>()) {
  set_name("ProxyServer");

  impl_->config = config;

  if (impl_->config.native_mode) {
    impl_->native_ip = Utils::get_native_ip();
  }

  impl_->current_host_name = Utils::get_host_name();
  impl_->current_machine_id = Utils::get_machine_id();
  impl_->current_ip_list = Utils::get_all_ipv4_address();

  const double max_packet_bytes = impl_->config.max_packet_size * 1024.0 * 1024.0;

  if VUNLIKELY (!std::isfinite(max_packet_bytes) || max_packet_bytes < 0.0 ||
                max_packet_bytes >= std::ldexp(1.0, std::numeric_limits<size_t>::digits)) {
    VLOG_F("ProxyServer: Invalid max packet size.");
  }

  impl_->real_max_packet_size = static_cast<size_t>(max_packet_bytes);

  if (impl_->config.bridge) {
    const auto& remote = *impl_->config.bridge;

    if VUNLIKELY (remote.domain_id < 0 || remote.domain_id > 255 || remote.domain_id == config.domain_id ||
                  remote.direct || config.direct) {
      VLOG_F("ProxyServer: Bridging requires a different remote DDS domain and non-direct transport.");
    }
  }

  if VUNLIKELY (ProxyServerGlobal::get().has_init.load(std::memory_order_acquire)) {
    VLOG_F("ProxyServer: Already initialized.");
    return;
  }

#if VLINK_PROXY_ENABLE_HANDSHAKE
  impl_->token = Uuid::random_hex();
  // CLOG_I("ProxyServer: Auth token issued (prefix=%.8s..., length=%zu).", impl_->token.c_str(), impl_->token.size());
#endif

  // intra_bind
  static std::string intra_bind = Utils::get_env("VLINK_INTRA_BIND");

  if (!intra_bind.empty()) {
    impl_->has_intra_bind = true;
  }

  if (impl_->config.use_iox) {
    init_shm_roudi();
  }

  init_server();
  init_runnable();
  init_bridge();

  ProxyServerGlobal::get().has_init.store(true, std::memory_order_release);
}

ProxyServer::~ProxyServer() {
  ProxyServerGlobal::get().has_quit.store(true, std::memory_order_release);

  quit(true);
  wait_for_quit();

  if (impl_->bridge_api) {
    impl_->bridge_api->quit(true);
    impl_->bridge_api->wait_for_quit();
    impl_->bridge_api->register_data_callback({});
    impl_->bridge_api->register_info_callback({});
    impl_->bridge_api->register_connect_callback({});
    impl_->bridge_api->register_error_callback({});
    impl_->bridge_pubs.clear();
  }

  impl_->forward_loop.quit(true);
  impl_->forward_loop.wait_for_quit();

  impl_->runnable_interface_list.clear();

  impl_->time_timer.stop();
  impl_->info_timer.stop();

  impl_->discovery_viewer->quit(true);
  impl_->discovery_viewer->wait_for_quit();

  impl_->control_id.store(0, std::memory_order_relaxed);
  impl_->mode.store(ProxyAPI::kOffline, std::memory_order_relaxed);

  {
    std::lock_guard control_lock(impl_->control_mtx);
    impl_->sub_urls.clear();
    impl_->requested_sub_meta_map.clear();
  }

  {
    std::lock_guard pubs_lock(impl_->pubs_mtx);
    impl_->pub_ptr_map.clear();
  }

  {
    std::lock_guard subs_lock(impl_->subs_mtx);
    impl_->sub_ptr_map.clear();
  }

  impl_->sub_seq_map.clear();
  impl_->sub_size_map.clear();
  impl_->sub_elapsed_map.clear();
  impl_->sub_miss_map.clear();
  impl_->sub_seq_buffer_map.clear();
  impl_->sub_size_buffer_map.clear();

#if VLINK_PROXY_ENABLE_HANDSHAKE
  impl_->handshake_srv.reset();
#endif

  impl_->data_sub.reset();
  impl_->bridge_api.reset();
  impl_->data_pub.reset();
  impl_->time_pub.reset();
  impl_->info_pub.reset();
  impl_->control_sub.reset();

  impl_->discovery_viewer.reset();

  impl_->runnable_plugin.clear();
}

std::string ProxyServer::get_token() const { return impl_->token; }

void ProxyServer::register_data_callback(ProxyAPI::DataCallback&& callback) {
  impl_->data_callback = std::move(callback);
}

void ProxyServer::register_info_callback(ProxyAPI::InfoCallback&& callback) {
  impl_->info_callback = std::move(callback);
}

void ProxyServer::register_time_callback(ProxyAPI::TimeCallback&& callback) {
  impl_->time_callback = std::move(callback);
}

bool ProxyServer::send_control(const ProxyAPI::Control& control) {
  if VUNLIKELY (!impl_->config.callback_mode || !is_running() || is_ready_to_quit() ||
                control.mode > ProxyAPI::kAutoAndObserveAll) {
    return false;
  }

  for (const auto& meta : control.url_meta_list) {
    if VUNLIKELY (meta.url.empty() || (meta.type != kSubscriber && meta.type != kPublisher && meta.type != kSetter)) {
      return false;
    }
  }

  proxy::ControlPacket packet;
  packet.body = control;

  return post_task([this, packet = std::move(packet)]() mutable {
    if VLIKELY (!is_ready_to_quit()) {
      packet.control_id = impl_->control_id.load(std::memory_order_relaxed) + 1;
      send_control(&packet);
    }
  });
}

size_t ProxyServer::get_max_task_count() const { return kMaxTaskSize; }

uint32_t ProxyServer::get_max_elapsed_time() const { return kMaxTaskElapsed; }

void ProxyServer::on_begin() {
  if (impl_->config.async && !impl_->config.callback_mode) {
    impl_->forward_loop.set_name("ProxyForward");

    if VUNLIKELY (!impl_->forward_loop.async_run()) {
      VLOG_E("ProxyServer: Failed to start the forward loop, async forwarding will stall.");
    }
  }

  if (impl_->bridge_api) {
    impl_->bridge_api->register_data_callback([this](const ProxyAPI::Data& data) {
      if VUNLIKELY (is_ready_to_quit() || !impl_->bridge_api->is_connected() ||
                    impl_->bridge_api->get_current_error() != ProxyAPI::kNoError) {
        return;
      }

      if VUNLIKELY (impl_->real_max_packet_size > 0 && data.raw.size() > impl_->real_max_packet_size) {
        return;
      }

      std::shared_ptr<RawPub> pub;

      {
        std::shared_lock lock(impl_->bridge_mtx);
        auto iter = impl_->bridge_pubs.find(data.url);

        if (iter == impl_->bridge_pubs.end()) {
          return;
        }

        pub = iter->second;
      }

      if VUNLIKELY (pub->get_ser_type() != data.ser || pub->get_schema_type() != data.schema) {
        return;
      }

      if (pub->has_subscribers()) {
        pub->publish(data.raw, true);
      }
    });

    if VUNLIKELY (!impl_->bridge_api->async_run()) {
      VLOG_E("ProxyServer: Failed to start the bridge controller.");
    } else {
      ProxyAPI::Control control;
      control.mode = ProxyAPI::kAuto;
      control.bridge = true;
      control.filter_str = impl_->config.bridge_filter;
      control.filter_type = 4;
      control.url_meta_list = impl_->bridge_selection;
      impl_->bridge_api->send_control(control);
    }
  }

  for (const auto& runnable : impl_->runnable_interface_list) {
    runnable->async_run();
    runnable->on_init();
  }

  MessageLoop::on_begin();
}

void ProxyServer::on_end() {
  if (impl_->bridge_api) {
    std::unordered_map<std::string, std::shared_ptr<RawSub>> readers;

    {
      std::lock_guard lock(impl_->bridge_mtx);
      readers.swap(impl_->bridge_readers);
    }

    readers.clear();
    impl_->bridge_api->quit(true);
    impl_->bridge_api->wait_for_quit();
    impl_->bridge_api->register_data_callback({});
    std::unordered_map<std::string, std::shared_ptr<RawPub>> publishers;

    {
      std::lock_guard lock(impl_->bridge_mtx);
      publishers.swap(impl_->bridge_pubs);
      impl_->bridge_subs.clear();
      impl_->bridge_selection.clear();
      impl_->bridge_same_machine = false;
    }
  }

  proxy::ControlPacket packet;
  packet.control_id = impl_->control_id.load(std::memory_order_relaxed);
  packet.body.mode = ProxyAPI::kOffline;
  send_control(&packet);

  impl_->forward_loop.quit(true);
  impl_->forward_loop.wait_for_quit();

  for (const auto& runnable : impl_->runnable_interface_list) {
    runnable->on_deinit();
    runnable->quit();
    runnable->wait_for_quit();
  }

  MessageLoop::on_end();
}

void ProxyServer::on_task_timeout(MessageLoop::Callback&& callback, uint32_t elapsed_time) {
  VLOG_W_EVERY_MS(kTimeoutWarnInterval, "ProxyServer: Task was delayed for ", elapsed_time,
                  "ms, the message loop is busy.");

  if VLIKELY (callback) {
    callback();
  }
}

void ProxyServer::init_shm_roudi() {
#ifdef VLINK_SUPPORT_SHM

  if (impl_->config.use_iox) {
    // VLOG_I("IOX Strategy: ", iox_strategy);

    std::string shm_roudi_name = Utils::get_app_name() + "_" + Utils::get_pid_str();

    ShmConf::init_roudi(impl_->config.iox_config, impl_->config.iox_strategy, impl_->config.iox_monitoring);
    ShmConf::init_runtime(shm_roudi_name, true);
    ShmConf::global_init();
  }
#else
  VLOG_F("ProxyServer: RouDi for shm is not supported.");
#endif
}

void ProxyServer::init_server() {
  std::string domain_id_str = std::to_string(impl_->config.domain_id);

  DiscoveryViewer::FilterType filter_type = DiscoveryViewer::kFilterAvailable;

  if (impl_->config.native_mode) {
    filter_type = DiscoveryViewer::kFilterNative;
  }

  impl_->discovery_viewer = std::make_shared<DiscoveryViewer>(filter_type);

  if VUNLIKELY (impl_->config.callback_mode) {
    impl_->boot_elapsed.start();
    impl_->main_elapsed.start();

    impl_->time_timer.set_interval(kCollectInterval);
    impl_->time_timer.set_loop_count(Timer::kInfinite);
    impl_->time_timer.attach(this);
    impl_->time_timer.set_callback([this]() { send_time(); });
    impl_->time_timer.start();

    impl_->info_timer.set_interval(kCollectInterval);
    impl_->info_timer.set_loop_count(Timer::kInfinite);
    impl_->info_timer.attach(this);
    impl_->info_timer.set_callback([this]() { update_all(); });
    impl_->info_timer.start();

    impl_->discovery_viewer->async_run();
    return;
  }

  if (impl_->config.direct) {
    impl_->data_pub =
        std::make_shared<DataPub>(proxy::make_url("shm", proxy::kDataShmUrlCtx, domain_id_str), InitType::kWithoutInit);
    impl_->data_sub = std::make_shared<DataSub>(proxy::make_url("shm", proxy::kViewerDataShmUrlCtx, domain_id_str),
                                                InitType::kWithoutInit);
  } else {
    if (impl_->config.reliable) {
      impl_->data_pub = std::make_shared<DataPub>(
          proxy::make_url(impl_->config.dds_impl, proxy::kDataReliableUrlCtx, domain_id_str), InitType::kWithoutInit);
      impl_->data_sub = std::make_shared<DataSub>(
          proxy::make_url(impl_->config.dds_impl, proxy::kViewerDataReliableUrlCtx, domain_id_str),
          InitType::kWithoutInit);
    } else {
      impl_->data_pub = std::make_shared<DataPub>(
          proxy::make_url(impl_->config.dds_impl, proxy::kDataUrlCtx, domain_id_str), InitType::kWithoutInit);
      impl_->data_sub = std::make_shared<DataSub>(
          proxy::make_url(impl_->config.dds_impl, proxy::kViewerDataUrlCtx, domain_id_str), InitType::kWithoutInit);
    }
  }

  Security::Config sec_cfg;

  if (!impl_->config.security_key.empty()) {
    sec_cfg.key = impl_->config.security_key;
  }

  impl_->time_pub = std::make_shared<TimePub>(
      proxy::make_url(impl_->config.dds_impl, proxy::kTimeUrlCtx, domain_id_str), sec_cfg, InitType::kWithoutInit);
  impl_->info_pub = std::make_shared<InfoPub>(
      proxy::make_url(impl_->config.dds_impl, proxy::kInfoListUrlCtx, domain_id_str), sec_cfg, InitType::kWithoutInit);
  impl_->control_sub = std::make_shared<ControlSub>(
      proxy::make_url(impl_->config.dds_impl, proxy::kControlUrlCtx, domain_id_str), sec_cfg, InitType::kWithoutInit);

#if VLINK_PROXY_ENABLE_HANDSHAKE
  impl_->handshake_srv = std::make_shared<HandshakeSrv>(
      proxy::make_url(impl_->config.dds_impl, proxy::kHandshakeUrlCtx, domain_id_str), sec_cfg, InitType::kWithoutInit);
#endif

  impl_->data_pub->set_discovery_enabled(false);
  impl_->data_sub->set_discovery_enabled(false);
  impl_->time_pub->set_discovery_enabled(false);
  impl_->info_pub->set_discovery_enabled(false);
  impl_->control_sub->set_discovery_enabled(false);
#if VLINK_PROXY_ENABLE_HANDSHAKE
  impl_->handshake_srv->set_discovery_enabled(false);
#endif

  if (!impl_->config.allow_ip.empty()) {
    impl_->data_pub->set_property("dds.ip", impl_->config.allow_ip);
    impl_->data_sub->set_property("dds.ip", impl_->config.allow_ip);
    impl_->time_pub->set_property("dds.ip", impl_->config.allow_ip);
    impl_->info_pub->set_property("dds.ip", impl_->config.allow_ip);
    impl_->control_sub->set_property("dds.ip", impl_->config.allow_ip);
#if VLINK_PROXY_ENABLE_HANDSHAKE
    impl_->handshake_srv->set_property("dds.ip", impl_->config.allow_ip);
#endif
  }

  if (!impl_->config.peer_ip.empty()) {
    impl_->data_pub->set_property("dds.peer", impl_->config.peer_ip);
    impl_->data_sub->set_property("dds.peer", impl_->config.peer_ip);
    impl_->time_pub->set_property("dds.peer", impl_->config.peer_ip);
    impl_->info_pub->set_property("dds.peer", impl_->config.peer_ip);
    impl_->control_sub->set_property("dds.peer", impl_->config.peer_ip);
#if VLINK_PROXY_ENABLE_HANDSHAKE
    impl_->handshake_srv->set_property("dds.peer", impl_->config.peer_ip);
#endif
  }

  if (impl_->config.buf_size > 0) {
    std::string buf_str = std::to_string(impl_->config.buf_size);
    impl_->data_pub->set_property("dds.buf", buf_str);
    impl_->data_sub->set_property("dds.buf", buf_str);
    impl_->time_pub->set_property("dds.buf", buf_str);
    impl_->info_pub->set_property("dds.buf", buf_str);
    impl_->control_sub->set_property("dds.buf", buf_str);
#if VLINK_PROXY_ENABLE_HANDSHAKE
    impl_->handshake_srv->set_property("dds.buf", buf_str);
#endif
  } else {
    impl_->data_pub->set_property("dds.buf", std::string(proxy::kSocketBufStr));
    impl_->data_sub->set_property("dds.buf", std::string(proxy::kSocketBufStr));
    impl_->time_pub->set_property("dds.buf", std::string(proxy::kSocketBufStr));
    impl_->info_pub->set_property("dds.buf", std::string(proxy::kSocketBufStr));
    impl_->control_sub->set_property("dds.buf", std::string(proxy::kSocketBufStr));
#if VLINK_PROXY_ENABLE_HANDSHAKE
    impl_->handshake_srv->set_property("dds.buf", std::string(proxy::kSocketBufStr));
#endif
  }

  if (impl_->config.mtu_size > 0) {
    std::string mtu_str = std::to_string(impl_->config.mtu_size);
    impl_->data_pub->set_property("dds.mtu", mtu_str);
    impl_->data_sub->set_property("dds.mtu", mtu_str);
    impl_->time_pub->set_property("dds.mtu", mtu_str);
    impl_->info_pub->set_property("dds.mtu", mtu_str);
    impl_->control_sub->set_property("dds.mtu", mtu_str);
#if VLINK_PROXY_ENABLE_HANDSHAKE
    impl_->handshake_srv->set_property("dds.mtu", mtu_str);
#endif
  } else {
    impl_->data_pub->set_property("dds.mtu", std::string(proxy::kSocketMtuStr));
    impl_->data_sub->set_property("dds.mtu", std::string(proxy::kSocketMtuStr));
    impl_->time_pub->set_property("dds.mtu", std::string(proxy::kSocketMtuStr));
    impl_->info_pub->set_property("dds.mtu", std::string(proxy::kSocketMtuStr));
    impl_->control_sub->set_property("dds.mtu", std::string(proxy::kSocketMtuStr));
#if VLINK_PROXY_ENABLE_HANDSHAKE
    impl_->handshake_srv->set_property("dds.mtu", std::string(proxy::kSocketMtuStr));
#endif
  }

  if (impl_->config.enable_tcp) {
    impl_->data_pub->set_property("dds.tcp", "1");
    impl_->data_sub->set_property("dds.tcp", "1");
  } else {
    impl_->data_pub->set_property("dds.tcp", "0");
    impl_->data_sub->set_property("dds.tcp", "0");
  }

  if (impl_->config.native_mode) {
    impl_->data_pub->set_property("dds.ip", impl_->native_ip);
    impl_->data_sub->set_property("dds.ip", impl_->native_ip);
    impl_->time_pub->set_property("dds.ip", impl_->native_ip);
    impl_->info_pub->set_property("dds.ip", impl_->native_ip);
    impl_->control_sub->set_property("dds.ip", impl_->native_ip);
#if VLINK_PROXY_ENABLE_HANDSHAKE
    impl_->handshake_srv->set_property("dds.ip", impl_->native_ip);
#endif
  }

#if VLINK_PROXY_ENABLE_HANDSHAKE
  impl_->handshake_srv->init();

  impl_->handshake_srv->listen([this](const proxy::HandshakeReqPacket& req, proxy::HandshakeRespPacket& resp) {
    resp.hostname = impl_->current_host_name;
    resp.machine_id = impl_->current_machine_id;
    resp.version = VLINK_VERSION;

    if VUNLIKELY (!req.version.empty() && req.version != VLINK_VERSION) {
      CLOG_E("ProxyServer: Reject handshake from %s due to version mismatch (peer=%s, self=%s).", req.hostname.c_str(),
             req.version.c_str(), VLINK_VERSION);
      resp.result = proxy::kHandshakeVersionMismatch;
      return;
    }

    resp.result = proxy::kHandshakeOk;
    resp.token = impl_->token;
  });
#endif

  if (!impl_->config.direct) {
    impl_->data_pub->init();
    impl_->data_sub->init();
  }

  impl_->time_pub->init();
  impl_->info_pub->init();
  impl_->control_sub->init();

  impl_->boot_elapsed.start();
  impl_->main_elapsed.start();

  impl_->time_timer.set_interval(kCollectInterval);
  impl_->time_timer.set_loop_count(Timer::kInfinite);
  impl_->time_timer.attach(this);
  impl_->time_timer.set_callback([this]() { send_time(); });
  impl_->time_timer.start();

  impl_->info_timer.set_interval(kCollectInterval);
  impl_->info_timer.set_loop_count(Timer::kInfinite);
  impl_->info_timer.attach(this);
  impl_->info_timer.set_callback([this]() { update_all(); });
  impl_->info_timer.start();

  impl_->info_pub->detect_subscribers([this](bool connected) {
    const auto generation = impl_->control_generation.load(std::memory_order_relaxed);

    if VUNLIKELY (impl_->mode.load(std::memory_order_relaxed) != ProxyAPI::kOffline && !connected &&
                  !impl_->info_pub->has_subscribers()) {
      post_task([this, generation]() {
        if VUNLIKELY (impl_->info_pub->has_subscribers() ||
                      impl_->control_generation.load(std::memory_order_relaxed) != generation) {
          return;
        }

        proxy::ControlPacket packet;
        packet.control_id = impl_->control_id.load(std::memory_order_relaxed);
        packet.body.mode = ProxyAPI::kOffline;

        send_control(&packet);
      });
    }
  });

  if (impl_->data_sub->has_inited()) {
    impl_->data_sub->listen([this](const zerocopy::ProxyData& t_data) {
      if VUNLIKELY (is_ready_to_quit() || ProxyServerGlobal::get().has_quit.load(std::memory_order_acquire)) {
        return;
      }

      if VUNLIKELY (t_data.control_id() != impl_->control_id.load(std::memory_order_relaxed)) {
        return;
      }

      if VUNLIKELY (impl_->mode.load(std::memory_order_relaxed) != ProxyAPI::kPlay &&
                    impl_->mode.load(std::memory_order_relaxed) != ProxyAPI::kEdit &&
                    impl_->mode.load(std::memory_order_relaxed) != ProxyAPI::kAuto &&
                    impl_->mode.load(std::memory_order_relaxed) != ProxyAPI::kAutoAndObserveAll) {
        return;
      }

      if (impl_->bridge_api && forward_bridge_data(&t_data)) {
        return;
      }

      if (!impl_->config.direct) {
        if (impl_->config.async) {
          impl_->forward_loop.post_task([this, t_data]() {
            if VUNLIKELY (t_data.control_id() != impl_->control_id.load(std::memory_order_relaxed)) {
              return;
            }

            std::shared_lock lock(impl_->pubs_mtx);
            auto iter = impl_->pub_ptr_map.find(std::string(t_data.url()));

            if VUNLIKELY (iter == impl_->pub_ptr_map.end() || iter->second.node->get_ser_type() != t_data.ser() ||
                          static_cast<uint32_t>(iter->second.node->get_schema_type()) != t_data.schema()) {
              return;
            }

            if VLIKELY (iter->second.type == kSetter || iter->second.node->has_subscribers()) {
              iter->second.node->publish(t_data.raw(), true);
            }
          });
        } else {
          std::shared_lock lock(impl_->pubs_mtx);
          auto iter = impl_->pub_ptr_map.find(std::string(t_data.url()));

          if VUNLIKELY (iter == impl_->pub_ptr_map.end() || iter->second.node->get_ser_type() != t_data.ser() ||
                        static_cast<uint32_t>(iter->second.node->get_schema_type()) != t_data.schema()) {
            return;
          }

          if VLIKELY (iter->second.type == kSetter || iter->second.node->has_subscribers()) {
            iter->second.node->publish(t_data.raw(), true);
          }
        }
      }
    });
  }

  impl_->control_sub->listen([this](const proxy::ControlPacket& packet) {
    if VUNLIKELY (ProxyServerGlobal::get().has_quit.load(std::memory_order_acquire)) {
      return;
    }

#if VLINK_PROXY_ENABLE_HANDSHAKE
    if VUNLIKELY (packet.token != impl_->token) {
      CLOG_E("ProxyServer: Reject control with mismatched token (control_id=%u).",
             static_cast<uint32_t>(packet.control_id));
      return;
    }
#endif

    post_task([this, packet]() { send_control(&packet); });
  });

  impl_->discovery_viewer->async_run();
}

void ProxyServer::init_bridge() {
  if (!impl_->config.bridge) {
    return;
  }

  auto remote = *impl_->config.bridge;
  remote.role = ProxyAPI::kController;
  impl_->bridge_filters = Helpers::split_any(impl_->config.bridge_filter);

  for (auto& filter : impl_->bridge_filters) {
    std::transform(filter.begin(), filter.end(), filter.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  }

  impl_->bridge_api = std::make_unique<ProxyAPI>(remote);
  impl_->bridge_api->set_name("ProxyBridge");

  impl_->bridge_api->register_info_callback([this](const std::vector<ProxyAPI::Info>& info_list) {
    if VLIKELY (!is_ready_to_quit()) {
      post_task([this, info_list]() { update_bridge(info_list); });
    }
  });
  impl_->bridge_api->register_connect_callback([this](bool connected) {
    if VUNLIKELY (!connected && !is_ready_to_quit()) {
      post_task([this]() { update_bridge({}); });
    }
  });
  impl_->bridge_api->register_error_callback([this](ProxyAPI::Error error) {
    if VUNLIKELY (error != ProxyAPI::kNoError && !is_ready_to_quit()) {
      VLOG_W("ProxyServer: Bridge connection error: ", static_cast<int>(error), ".");
      post_task([this]() { update_bridge({}); });
    }
  });
}

void ProxyServer::update_bridge(const std::vector<ProxyAPI::Info>& info_list) {
  std::unordered_map<std::string, std::shared_ptr<RawPub>> next_pubs;
  std::unordered_map<std::string, std::shared_ptr<RawSub>> next_readers;
  std::unordered_map<std::string, ProxyAPI::Info> next_subs;
  std::vector<ProxyAPI::UrlMeta> selection;
  const bool same_machine =
      impl_->bridge_api->is_same_machine(impl_->current_host_name, impl_->current_machine_id, impl_->current_ip_list);

  if VLIKELY (!is_ready_to_quit() && impl_->bridge_api->is_connected() &&
              impl_->bridge_api->get_current_error() == ProxyAPI::kNoError) {
    next_pubs.reserve(info_list.size());
    const auto local_infos = impl_->config.bridge_subscribe ? impl_->discovery_viewer->get_info_list()
                                                            : std::vector<DiscoveryViewer::Info>{};
    std::unordered_map<std::string_view, const DiscoveryViewer::Info*> local_publishers;
    local_publishers.reserve(local_infos.size());

    for (const auto& local : local_infos) {
      if (!(local.type & kPublisher)) {
        continue;
      }

      const bool pure_intra =
          Url::is_intra_type(local.url) && Url(local.url).get_transport_type() == TransportType::kIntra;

      if (std::none_of(local.process_list.begin(), local.process_list.end(), [&](const DiscoveryViewer::Process& p) {
            return !p.bridge && (p.type & kPublisher) &&
                   (!pure_intra ||
                    (p.host == impl_->current_host_name && p.pid == static_cast<uint32_t>(Utils::get_pid())));
          })) {
        continue;
      }

      local_publishers.emplace(local.url, &local);
    }

    for (const auto& info : info_list) {
      if (!(info.type & (kPublisher | kSubscriber)) || (info.type & (kSetter | kGetter)) || info.ser.empty()) {
        continue;
      }

      if (!impl_->bridge_filters.empty()) {
        std::string url = info.url;
        std::transform(url.begin(), url.end(), url.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (std::none_of(impl_->bridge_filters.begin(), impl_->bridge_filters.end(),
                         [&url](const std::string& filter) { return url.find(filter) != std::string::npos; })) {
          continue;
        }
      }

      bool shared_shm = false;

      if (same_machine) {
        try {
          const auto transport = Url(info.url).get_transport_type();
          shared_shm = transport == TransportType::kShm || transport == TransportType::kShm2;
        } catch (const Exception::RuntimeError& error) {
          VLOG_W("ProxyServer: Cannot resolve bridge transport: ", error.what());
          continue;
        }
      }

      if (info.type & kSubscriber) {
        next_subs.emplace(info.url, info);
        selection.push_back({info.url, info.ser, info.schema, kPublisher});
      }

      if (!(info.type & kPublisher)) {
        if (!impl_->config.bridge_subscribe || shared_shm) {
          continue;
        }

        const auto local = local_publishers.find(info.url);

        if (local != local_publishers.end() &&
            (local->second->ser_type != info.ser || local->second->schema_type != info.schema)) {
          continue;
        }

        std::shared_ptr<RawSub> sub;

        {
          std::shared_lock lock(impl_->bridge_mtx);
          const auto iter = impl_->bridge_readers.find(info.url);

          if (iter != impl_->bridge_readers.end() && iter->second->get_ser_type() == info.ser &&
              iter->second->get_schema_type() == info.schema) {
            sub = iter->second;
          }
        }

        if (!sub) {
          try {
            sub = std::make_shared<RawSub>(info.url, InitType::kWithoutInit);
            apply_topic_transport(*sub, impl_->config, impl_->native_ip);
            sub->set_safety_quit(true);
            sub->set_discovery_enabled(false);
            sub->set_ser_type(info.ser, info.schema);
            sub->init();
            sub->listen(
                [this, node = sub.get(), url = info.url, ser = info.ser, schema = info.schema](const Bytes& raw) {
                  if VUNLIKELY (is_ready_to_quit() || !impl_->bridge_api->is_connected() ||
                                impl_->bridge_api->get_current_error() != ProxyAPI::kNoError ||
                                (impl_->real_max_packet_size > 0 && raw.size() > impl_->real_max_packet_size)) {
                    return;
                  }

                  {
                    std::shared_lock lock(impl_->bridge_mtx);
                    const auto iter = impl_->bridge_readers.find(url);

                    if VUNLIKELY (iter == impl_->bridge_readers.end() || iter->second.get() != node) {
                      return;
                    }
                  }

                  ProxyAPI::Data outgoing;
                  outgoing.url = url;
                  outgoing.ser = ser;
                  outgoing.schema = schema;
                  outgoing.raw.shallow_copy(raw);
                  outgoing.timestamp = impl_->main_elapsed.get();
                  impl_->bridge_api->send_data(outgoing);
                });
          } catch (const Exception::RuntimeError& error) {
            VLOG_W("ProxyServer: Cannot create bridge subscriber: ", error.what());
            continue;
          }
        }

        next_readers.emplace(info.url, std::move(sub));
        continue;
      }

      if (shared_shm) {
        continue;
      }

      std::shared_ptr<RawPub> pub;

      {
        std::shared_lock lock(impl_->bridge_mtx);
        const auto iter = impl_->bridge_pubs.find(info.url);

        if (iter != impl_->bridge_pubs.end() && iter->second->get_ser_type() == info.ser &&
            iter->second->get_schema_type() == info.schema) {
          pub = iter->second;
        }
      }

      if VUNLIKELY (!pub) {
        try {
          pub = std::make_shared<RawPub>(info.url, InitType::kWithoutInit);
          apply_topic_transport(*pub, impl_->config, impl_->native_ip);
          pub->set_property("proxy.bridge", "1");
          pub->set_ser_type(info.ser, info.schema);
          pub->init();
        } catch (const Exception::RuntimeError& error) {
          VLOG_W("ProxyServer: Cannot create bridge publisher: ", error.what());
          continue;
        }
      }

      next_pubs.emplace(info.url, std::move(pub));
      selection.push_back({info.url, info.ser, info.schema, kSubscriber});
    }
  }

  std::sort(selection.begin(), selection.end(), [](const ProxyAPI::UrlMeta& left, const ProxyAPI::UrlMeta& right) {
    return left.url < right.url || (left.url == right.url && left.type < right.type);
  });

  {
    std::lock_guard lock(impl_->bridge_mtx);
    impl_->bridge_same_machine = same_machine;
    impl_->bridge_pubs.swap(next_pubs);
    impl_->bridge_readers.swap(next_readers);
    impl_->bridge_subs.swap(next_subs);
  }

  next_readers.clear();
  next_pubs.clear();

  if (!std::equal(selection.begin(), selection.end(), impl_->bridge_selection.begin(), impl_->bridge_selection.end(),
                  [](const ProxyAPI::UrlMeta& left, const ProxyAPI::UrlMeta& right) {
                    return left.url == right.url && left.ser == right.ser && left.schema == right.schema &&
                           left.type == right.type;
                  })) {
    ProxyAPI::Control control;
    control.mode = ProxyAPI::kAuto;
    control.bridge = true;
    control.filter_str = impl_->config.bridge_filter;
    control.filter_type = 4;
    control.url_meta_list = std::move(selection);
    impl_->bridge_api->send_control(control);
    impl_->bridge_selection = std::move(control.url_meta_list);
  }
}

bool ProxyServer::forward_bridge_data(const void* data) {
  const auto& packet = *static_cast<const zerocopy::ProxyData*>(data);

  if VUNLIKELY (!impl_->bridge_api->is_connected() || impl_->bridge_api->get_current_error() != ProxyAPI::kNoError) {
    return false;
  }

  ProxyAPI::Data outgoing;
  outgoing.url = packet.url();
  bool mirrored = false;
  bool same_machine = false;

  {
    std::shared_lock lock(impl_->bridge_mtx);
    const auto iter = impl_->bridge_subs.find(outgoing.url);

    if (iter == impl_->bridge_subs.end()) {
      return false;
    }

    if VUNLIKELY (iter->second.ser != packet.ser() || static_cast<uint32_t>(iter->second.schema) != packet.schema()) {
      return false;
    }

    mirrored = (iter->second.type & kPublisher) != 0;
    same_machine = impl_->bridge_same_machine;
  }

  bool shared_shm = false;

  {
    std::shared_lock lock(impl_->pubs_mtx);
    const auto iter = impl_->pub_ptr_map.find(outgoing.url);

    if VUNLIKELY (iter == impl_->pub_ptr_map.end() || iter->second.type != kPublisher ||
                  iter->second.node->get_ser_type() != packet.ser() ||
                  static_cast<uint32_t>(iter->second.node->get_schema_type()) != packet.schema()) {
      return false;
    }

    const auto transport = iter->second.node->get_transport_type();
    shared_shm = same_machine && (transport == TransportType::kShm || transport == TransportType::kShm2);
  }

  if VUNLIKELY (impl_->real_max_packet_size > 0 && packet.raw().size() > impl_->real_max_packet_size) {
    return true;
  }

  if (!impl_->config.bridge_subscribe || mirrored || shared_shm) {
    outgoing.ser = packet.ser();
    outgoing.schema = static_cast<SchemaType>(packet.schema());
    outgoing.raw.shallow_copy(packet.raw());
    outgoing.timestamp = packet.timestamp();
    outgoing.seq = packet.seq();
    impl_->bridge_api->send_data(outgoing);
  }

  return mirrored || shared_shm;
}

void ProxyServer::init_runnable() {
  for (const auto& runnable_name : impl_->config.runnable_list) {
    CLOG_I("ProxyServer: Load runnable plugin [%s].", runnable_name.c_str());
    auto runnable = impl_->runnable_plugin.load<RunablePluginInterface>(impl_->config.runnable_prefix + runnable_name,
                                                                        impl_->config.runnable_version_major,
                                                                        impl_->config.runnable_version_minor);

    if (runnable) {
      auto plugin_complex_id = impl_->runnable_plugin.get_plugin_complex_id<RunablePluginInterface>(runnable_name);
      runnable->set_name(plugin_complex_id);
      impl_->runnable_interface_list.emplace_back(std::move(runnable));
    }
  }
}

void ProxyServer::send_time() {
  if VUNLIKELY (impl_->config.callback_mode) {
    if VLIKELY (impl_->time_callback) {
      impl_->time_callback(ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMicro, false),
                           static_cast<uint64_t>(impl_->boot_elapsed.get()));
    }

    return;
  }

  if VUNLIKELY (!impl_->time_pub->has_subscribers()) {
    return;
  }

  proxy::TimePacket time;
  time.control_id = impl_->control_id.load(std::memory_order_relaxed);
  time.mode = impl_->mode.load(std::memory_order_relaxed);

  time.reliable_mode = impl_->config.reliable;
  time.tcp_mode = impl_->config.enable_tcp;
  time.direct_mode = impl_->config.direct;
  time.version = VLINK_VERSION;
  time.hostname = impl_->current_host_name;
  time.machine_id = impl_->current_machine_id;
  time.ip_list = impl_->current_ip_list;
#if VLINK_PROXY_ENABLE_HANDSHAKE
  time.token = impl_->token;
#endif

  if (impl_->config.direct) {
    std::shared_lock control_lock(impl_->control_mtx);
    time.direct_sub_list.reserve(impl_->requested_sub_meta_map.size());

    for (const auto& [url, meta] : impl_->requested_sub_meta_map) {
      time.direct_sub_list.push_back({url, meta.ser, meta.schema, kSubscriber});
    }
  }

  time.cpu_usage = Utils::get_cpu_usage();
  time.memory_usage = Utils::get_memory_usage();

  time.sys_time = ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMicro, false);
  time.boot_time = static_cast<uint64_t>(impl_->boot_elapsed.get());

  impl_->time_pub->publish(time, true);
}

void ProxyServer::send_control(const void* control_data) {
  const auto& packet = *static_cast<const proxy::ControlPacket*>(control_data);
  const auto& body = packet.body;
  const auto next_mode = body.mode;

  if VUNLIKELY (!impl_->config.callback_mode && next_mode == ProxyAPI::kOffline &&
                packet.control_id != impl_->control_id.load(std::memory_order_relaxed)) {
    return;
  }

  switch (next_mode) {
    case ProxyAPI::kOffline:
    case ProxyAPI::kObserveOne:
    case ProxyAPI::kObserveAll:
    case ProxyAPI::kRecord:
    case ProxyAPI::kPlay:
    case ProxyAPI::kEdit:
    case ProxyAPI::kAuto:
    case ProxyAPI::kAutoAndObserveAll:
      break;
    default:
      CLOG_E("ProxyServer: Unsupported control mode %d.", static_cast<int>(next_mode));
      return;
  }

  std::unordered_set<std::string> next_sub_urls;
  std::unordered_map<std::string, ProxySubMeta> next_sub_meta_map;
  std::unordered_map<std::string, ProxyAPI::UrlMeta> next_pub_meta_map;

  if VUNLIKELY (next_mode != ProxyAPI::kOffline) {
    next_sub_urls.reserve(body.url_meta_list.size());
    next_sub_meta_map.reserve(body.url_meta_list.size());
    next_pub_meta_map.reserve(body.url_meta_list.size());

    for (const auto& url_meta : body.url_meta_list) {
      if (url_meta.url.empty()) {
        continue;
      }

      const auto schema = SchemaData::is_valid_type(url_meta.schema) ? url_meta.schema : SchemaType::kUnknown;

      if (url_meta.type == kSubscriber) {
        next_sub_urls.emplace(url_meta.url);

        if (url_meta.ser.empty()) {
          continue;
        }

        next_sub_meta_map.try_emplace(url_meta.url, ProxySubMeta{url_meta.ser, schema});
      } else if (url_meta.type == kPublisher || url_meta.type == kSetter) {
        if (url_meta.ser.empty()) {
          continue;
        }

        next_pub_meta_map.try_emplace(url_meta.url,
                                      ProxyAPI::UrlMeta{url_meta.url, url_meta.ser, schema, url_meta.type});
      }
    }
  }

  ProxyAPI::Mode last_mode = impl_->mode.load(std::memory_order_relaxed);
  std::unordered_set<std::string> last_sub_urls;

  impl_->control_id.store(packet.control_id, std::memory_order_relaxed);
  impl_->mode.store(next_mode, std::memory_order_relaxed);
  impl_->control_generation.fetch_add(1, std::memory_order_relaxed);
  impl_->info_elapsed_timer.restart();

  bool to_update = next_mode != last_mode || next_mode == ProxyAPI::kRecord;

  if VUNLIKELY (next_mode == ProxyAPI::kOffline) {
    impl_->control_id.store(0, std::memory_order_relaxed);

    {
      std::lock_guard control_lock(impl_->control_mtx);
      impl_->sub_urls.clear();
      impl_->requested_sub_meta_map.clear();
    }

    {
      std::lock_guard pubs_lock(impl_->pubs_mtx);
      impl_->pub_ptr_map.clear();
      impl_->pub_error_url_set.clear();
    }

    {
      std::lock_guard subs_lock(impl_->subs_mtx);
      impl_->sub_ptr_map.clear();
      impl_->sub_error_url_set.clear();
    }

    return;
  }

  post_task([this]() { send_time(); });
  impl_->time_timer.restart();

  {
    std::lock_guard control_lock(impl_->control_mtx);
    last_sub_urls = std::move(impl_->sub_urls);
    impl_->sub_urls = std::move(next_sub_urls);
    impl_->requested_sub_meta_map = std::move(next_sub_meta_map);
    impl_->filter_by_process = body.filter_by_process;
    impl_->filter_list = Helpers::split_any(body.filter_str);
    impl_->filter_type = body.filter_type;
  }

  if (next_mode == ProxyAPI::kRecord || next_mode == ProxyAPI::kObserveOne || next_mode == ProxyAPI::kObserveAll) {
    {
      std::lock_guard lock(impl_->pubs_mtx);
      impl_->pub_ptr_map.clear();
    }

    if (next_mode == ProxyAPI::kObserveOne && last_mode == ProxyAPI::kObserveOne && !to_update) {
      post_task([this]() { update_all(); });
      impl_->info_timer.restart();
    }
  } else if (next_mode == ProxyAPI::kPlay || next_mode == ProxyAPI::kEdit || next_mode == ProxyAPI::kAuto ||
             next_mode == ProxyAPI::kAutoAndObserveAll) {
    if (!impl_->config.direct) {
      {
        std::lock_guard lock(impl_->pubs_mtx);
        for (auto pub_iter = impl_->pub_ptr_map.begin(); pub_iter != impl_->pub_ptr_map.end();) {
          if (next_pub_meta_map.find(pub_iter->first) == next_pub_meta_map.end()) {
            pub_iter = impl_->pub_ptr_map.erase(pub_iter);
          } else {
            ++pub_iter;
          }
        }

        for (const auto& [url, meta] : next_pub_meta_map) {
          if (impl_->pub_error_url_set.count(url) != 0) {
            continue;
          }

          auto pub_iter = impl_->pub_ptr_map.find(url);

          if (pub_iter != impl_->pub_ptr_map.end()) {
            auto* pub = pub_iter->second.node.get();

            if (pub != nullptr && pub_iter->second.type == meta.type && pub->get_ser_type() == meta.ser &&
                pub->get_schema_type() == meta.schema &&
                (pub->get_property("proxy.bridge") == "1") == (body.bridge && meta.type == kPublisher)) {
              continue;
            }

            impl_->pub_ptr_map.erase(pub_iter);
          }

          try {
            auto pub = std::make_shared<RawPub>(url, InitType::kWithoutInit);

            if (meta.type == kSetter) {
              pub->mark_as_setter();
            }

            apply_topic_transport(*pub, impl_->config, impl_->native_ip);

            pub->set_ser_type(meta.ser, meta.schema);

            if (body.bridge && meta.type == kPublisher) {
              pub->set_property("proxy.bridge", "1");
            }

            pub->init();

            impl_->pub_ptr_map.emplace(url, Impl::PubEntry{std::move(pub), meta.type});
          } catch (Exception::RuntimeError&) {
            impl_->pub_error_url_set.emplace(url);
            continue;
          }
        }
      }

      if (next_mode == ProxyAPI::kPlay || next_mode == ProxyAPI::kEdit) {
        std::lock_guard subs_lock(impl_->subs_mtx);
        impl_->sub_ptr_map.clear();
      }
    }
  }

  if (to_update) {
    {
      std::lock_guard subs_lock(impl_->subs_mtx);
      impl_->sub_ptr_map.clear();
    }

    impl_->main_elapsed.restart();
    post_task([this]() { update_all(); });
    impl_->info_timer.restart();
  }
}

void ProxyServer::update_all() {
  if VUNLIKELY (!impl_->config.callback_mode && !impl_->info_pub->has_subscribers()) {
    if (impl_->mode.load(std::memory_order_relaxed) != ProxyAPI::kOffline) {
      impl_->info_elapsed_timer.start();

      if VUNLIKELY (impl_->info_elapsed_timer.get() > 5000) {
        proxy::ControlPacket packet;
        packet.control_id = impl_->control_id.load(std::memory_order_relaxed);
        packet.body.mode = ProxyAPI::kOffline;
        send_control(&packet);
      }
    }

    return;
  }

  impl_->info_elapsed_timer.stop();

  if (impl_->mode.load(std::memory_order_relaxed) != ProxyAPI::kObserveOne &&
      impl_->mode.load(std::memory_order_relaxed) != ProxyAPI::kObserveAll &&
      impl_->mode.load(std::memory_order_relaxed) != ProxyAPI::kRecord &&
      impl_->mode.load(std::memory_order_relaxed) != ProxyAPI::kAuto &&
      impl_->mode.load(std::memory_order_relaxed) != ProxyAPI::kAutoAndObserveAll) {
    return;
  }

  proxy::InfoListPacket packet;

  auto info_list = impl_->discovery_viewer->get_info_list();
  const auto current_pid = static_cast<uint32_t>(Utils::get_pid());
  info_list.erase(
      std::remove_if(
          info_list.begin(), info_list.end(),
          [&](DiscoveryViewer::Info& info) {
            const bool intra = Url::is_intra_type(info.url);
            const auto transport = intra ? Url(info.url).get_transport_type() : TransportType::kUnknown;
            const bool pure_intra = intra && transport == TransportType::kIntra;
            const bool local_only = Url::is_shm_type(info.url) ||
                                    (intra && (transport == TransportType::kIntra || transport == TransportType::kShm ||
                                               transport == TransportType::kShm2));
            auto& processes = info.process_list;
            processes.erase(
                std::remove_if(
                    processes.begin(), processes.end(),
                    [&](DiscoveryViewer::Process& process) {
                      const bool own_process = process.host == impl_->current_host_name && process.pid == current_pid;

                      if (pure_intra && !own_process) {
                        return true;
                      }

                      if (local_only && !process.ip_list.empty() &&
                          std::none_of(process.ip_list.begin(), process.ip_list.end(), [&](const std::string& ip) {
                            return std::find(impl_->current_ip_list.begin(), impl_->current_ip_list.end(), ip) !=
                                   impl_->current_ip_list.end();
                          })) {
                        return true;
                      }

                      if (process.bridge) {
                        process.type &= ~kPublisher;
                      }

                      return process.type == 0;
                    }),
                processes.end());
            info.type = 0;

            for (const auto& process : processes) {
              info.type |= process.type;
            }

            return info.type == 0;
          }),
      info_list.end());

  std::unordered_map<std::string_view, size_t> info_index;

  if (!impl_->bridge_subs.empty() || !impl_->bridge_pubs.empty()) {
    info_list.reserve(info_list.size() + impl_->bridge_subs.size() + impl_->bridge_pubs.size());
    info_index.reserve(info_list.size());

    for (size_t i = 0; i < info_list.size(); ++i) {
      info_index.emplace(info_list[i].url, i);
    }
  }

  for (const auto& [url, pub] : impl_->bridge_pubs) {
    const auto iter = info_index.find(url);
    const size_t index = iter == info_index.end() ? info_list.size() : iter->second;

    if (index == info_list.size()) {
      auto& mirror = info_list.emplace_back();
      mirror.url = url;
      mirror.ser_type = pub->get_ser_type();
      mirror.schema_type = pub->get_schema_type();
      info_index.emplace(url, index);
    } else if (info_list[index].ser_type != pub->get_ser_type() ||
               info_list[index].schema_type != pub->get_schema_type()) {
      continue;
    }

    auto& mirror = info_list[index];
    mirror.type |= kPublisher;
    auto& process = mirror.process_list.emplace_back();
    process.type = kPublisher;
    process.host = impl_->current_host_name;
    process.pid = current_pid;
    process.name = Utils::get_app_name();
    process.ip_list = impl_->current_ip_list;
  }

  for (const auto& [url, info] : impl_->bridge_subs) {
    const auto iter = info_index.find(url);
    const size_t index = iter == info_index.end() ? info_list.size() : iter->second;

    if (index == info_list.size()) {
      auto& remote = info_list.emplace_back();
      remote.url = url;
      remote.ser_type = info.ser;
      remote.schema_type = info.schema;
    } else if (info_list[index].ser_type != info.ser || info_list[index].schema_type != info.schema) {
      continue;
    }

    auto& local = info_list[index];
    local.type |= kSubscriber;

    for (const auto& process : info.process_list) {
      if (process.type & kSubscriber) {
        auto& remote = local.process_list.emplace_back();
        remote.type = kSubscriber;
        remote.host = process.host;
        remote.pid = process.pid;
        remote.name = process.name;
        remote.ip_list = process.ip_list;
      }
    }
  }

  packet.info_list.reserve(info_list.size());

  {
    std::unordered_set<std::string> current_urls;

    current_urls.reserve(info_list.size());

    for (const auto& info : info_list) {
      current_urls.emplace(info.url);
    }

    std::lock_guard subs_lock(impl_->subs_mtx);

    for (auto iter = impl_->sub_seq_buffer_map.begin(); iter != impl_->sub_seq_buffer_map.end();) {
      if (current_urls.count(iter->first) == 0) {
        if (++impl_->sub_miss_map[iter->first] < kMaxSubMissCount) {
          impl_->sub_seq_map[iter->first].store(0, std::memory_order_relaxed);
          impl_->sub_size_map[iter->first].store(0, std::memory_order_relaxed);
          impl_->sub_lat_map[iter->first].store(0, std::memory_order_relaxed);
          ++iter;
          continue;
        }

        std::atomic<int64_t>& seq = impl_->sub_seq_map[iter->first];
        std::atomic<size_t>& size = impl_->sub_size_map[iter->first];
        std::atomic<double>& lost = impl_->sub_lost_map[iter->first];
        std::atomic<int64_t>& lat = impl_->sub_lat_map[iter->first];
        ElapsedTimer& elapsed = impl_->sub_elapsed_map[iter->first];

        seq.store(0, std::memory_order_relaxed);
        size.store(0, std::memory_order_relaxed);
        lost.store(0, std::memory_order_relaxed);
        lat.store(0, std::memory_order_relaxed);

        elapsed.stop();

        impl_->sub_size_buffer_map.erase(iter->first);
        impl_->sub_lost_buffer_map.erase(iter->first);
        impl_->sub_lat_buffer_map.erase(iter->first);

        impl_->sub_ptr_map.erase(iter->first);
        impl_->sub_miss_map.erase(iter->first);

        iter = impl_->sub_seq_buffer_map.erase(iter);
      } else {
        impl_->sub_miss_map.erase(iter->first);
        ++iter;
      }
    }
  }

  for (const auto& info : info_list) {
    const auto discovered_schema =
        SchemaData::is_valid_type(info.schema_type) ? info.schema_type : SchemaType::kUnknown;
    ProxySubMeta stream_meta;
    bool has_stream_meta = false;

#if VLINK_PROXY_ENABLE_FILTER

    if (impl_->mode.load(std::memory_order_relaxed) == ProxyAPI::kObserveOne ||
        impl_->mode.load(std::memory_order_relaxed) == ProxyAPI::kObserveAll ||
        impl_->mode.load(std::memory_order_relaxed) == ProxyAPI::kAuto ||
        impl_->mode.load(std::memory_order_relaxed) == ProxyAPI::kAutoAndObserveAll) {
      if (!impl_->filter_list.empty()) {
        bool contains = false;

        if (impl_->filter_by_process) {
          for (const auto& process : info.process_list) {
            std::string left_str = process.name;
            std::transform(left_str.begin(), left_str.end(), left_str.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            for (const auto& filter : impl_->filter_list) {
              if (filter.empty()) {
                continue;
              }

              std::string right_str = filter;
              std::transform(right_str.begin(), right_str.end(), right_str.begin(),
                             [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

              if (left_str.find(right_str) != std::string::npos) {
                contains = true;
                break;
              }
            }

            if (contains) {
              break;
            }
          }

        } else {
          std::string left_str = info.url;
          std::transform(left_str.begin(), left_str.end(), left_str.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

          for (const auto& filter : impl_->filter_list) {
            if (filter.empty()) {
              continue;
            }

            std::string right_str = filter;
            std::transform(right_str.begin(), right_str.end(), right_str.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            if (left_str.find(right_str) != std::string::npos) {
              contains = true;
              break;
            }
          }
        }

        if (!contains) {
          std::lock_guard subs_lock(impl_->subs_mtx);
          impl_->sub_ptr_map.erase(info.url);
          continue;
        }
      }

      bool matches_type = true;

      switch (impl_->filter_type) {
        case 0:
          break;
        case 1:
          matches_type = (info.type & kPublisher) != 0 && (info.type & kSubscriber) != 0;
          break;
        case 2:
          matches_type = (info.type & kServer) != 0 && (info.type & kClient) != 0;
          break;
        case 3:
          matches_type = (info.type & kSetter) != 0 && (info.type & kGetter) != 0;
          break;
        case 4:
          matches_type = (info.type & kPublisher) != 0 || (info.type & kSubscriber) != 0;
          break;
        case 5:
          matches_type = (info.type & kServer) != 0 || (info.type & kClient) != 0;
          break;
        case 6:
          matches_type = (info.type & kSetter) != 0 || (info.type & kGetter) != 0;
          break;
        case 7:
          matches_type = (info.type & kPublisher) != 0;
          break;
        case 8:
          matches_type = (info.type & kSubscriber) != 0;
          break;
        case 9:
          matches_type = (info.type & kServer) != 0;
          break;
        case 10:
          matches_type = (info.type & kClient) != 0;
          break;
        case 11:
          matches_type = (info.type & kSetter) != 0;
          break;
        case 12:
          matches_type = (info.type & kGetter) != 0;
          break;
        default:
          break;
      }

      if (!matches_type) {
        std::lock_guard subs_lock(impl_->subs_mtx);
        impl_->sub_ptr_map.erase(info.url);
        continue;
      }
    }
#endif

    auto& out_info = packet.info_list.emplace_back();
    out_info.type = info.type;
    out_info.url = info.url;
    out_info.ser = info.ser_type;
    out_info.schema = discovered_schema;
    out_info.status = ProxyAPI::kInvalid;
    out_info.freq = 0;
    out_info.rate = 0;

    out_info.process_list.reserve(info.process_list.size());

    for (const auto& process : info.process_list) {
      auto& out_process = out_info.process_list.emplace_back();
      out_process.type = process.type;
      out_process.host = process.host;
      out_process.pid = process.pid;
      out_process.name = process.name;
      out_process.ip_list = process.ip_list;
    }

    if ((!(info.type & kPublisher) && !(info.type & kSetter)) ||
        (!impl_->has_intra_bind && impl_->runnable_interface_list.empty() && Url::is_intra_type(info.url) &&
         impl_->bridge_pubs.count(info.url) == 0)) {
      out_info.status = ProxyAPI::kInvalid;
      out_info.freq = 0;
      out_info.rate = 0;
      continue;
    }

    std::atomic<int64_t>& total_seq = impl_->sub_total_seq_map[info.url];
    std::atomic<int64_t>& seq = impl_->sub_seq_map[info.url];
    std::atomic<size_t>& size = impl_->sub_size_map[info.url];
    std::atomic<double>& lost = impl_->sub_lost_map[info.url];
    std::atomic<int64_t>& lat = impl_->sub_lat_map[info.url];
    ElapsedTimer& elapsed = impl_->sub_elapsed_map[info.url];
    std::deque<int64_t>& seq_buffer = impl_->sub_seq_buffer_map[info.url];
    std::deque<size_t>& size_buffer = impl_->sub_size_buffer_map[info.url];
    std::deque<double>& lost_buffer = impl_->sub_lost_buffer_map[info.url];
    std::deque<int64_t>& lat_buffer = impl_->sub_lat_buffer_map[info.url];

    if VUNLIKELY (!elapsed.is_active()) {
      elapsed.start();
    }

    if (seq.load(std::memory_order_relaxed) > 0 && seq_buffer.size() >= kCounterCache &&
        size_buffer.size() >= kCounterCache) {
      out_info.status = ProxyAPI::kActive;
    } else {
      if (elapsed.get() >= kCollectInterval * kCounterCache) {
        seq.store(0, std::memory_order_relaxed);
        size.store(0, std::memory_order_relaxed);
        lost.store(0, std::memory_order_relaxed);
        lat.store(0, std::memory_order_relaxed);
        seq_buffer.clear();
        size_buffer.clear();
        lost_buffer.clear();
        lat_buffer.clear();
        out_info.status = ProxyAPI::kInActive;
      } else {
        out_info.status = ProxyAPI::kPending;
      }
    }

    std::unique_lock subs_lock(impl_->subs_mtx);

    if VUNLIKELY (impl_->sub_error_url_set.count(info.url) != 0) {
      continue;
    }

    auto ptr_iter = impl_->sub_ptr_map.find(info.url);

    bool create = false;

    if (impl_->mode.load(std::memory_order_relaxed) == ProxyAPI::kObserveAll ||
        impl_->mode.load(std::memory_order_relaxed) == ProxyAPI::kAutoAndObserveAll) {
      create = (ptr_iter == impl_->sub_ptr_map.end());
      stream_meta = ProxySubMeta{info.ser_type, discovered_schema};
      has_stream_meta = !stream_meta.ser.empty();
    } else if (impl_->mode.load(std::memory_order_relaxed) == ProxyAPI::kObserveOne ||
               impl_->mode.load(std::memory_order_relaxed) == ProxyAPI::kRecord ||
               impl_->mode.load(std::memory_order_relaxed) == ProxyAPI::kAuto) {
      std::shared_lock control_lock(impl_->control_mtx);
      if (impl_->sub_urls.count(info.url) == 0) {
        create = false;
        seq.store(0, std::memory_order_relaxed);
        lost.store(0, std::memory_order_relaxed);
        size.store(0, std::memory_order_relaxed);
        lat.store(0, std::memory_order_relaxed);
        seq_buffer.clear();
        size_buffer.clear();
        lost_buffer.clear();
        lat_buffer.clear();
      } else {
        create = ptr_iter == impl_->sub_ptr_map.end();
        auto meta_iter = impl_->requested_sub_meta_map.find(info.url);

        if (meta_iter != impl_->requested_sub_meta_map.end()) {
          stream_meta = meta_iter->second;
          has_stream_meta = !stream_meta.ser.empty() && !info.ser_type.empty() && info.ser_type == stream_meta.ser &&
                            discovered_schema == stream_meta.schema;
        } else {
          has_stream_meta = false;
        }
      }
    }

    if VUNLIKELY (!has_stream_meta) {
      if (ptr_iter != impl_->sub_ptr_map.end()) {
        impl_->sub_ptr_map.erase(ptr_iter);
      }

      seq.store(0, std::memory_order_relaxed);
      lost.store(0, std::memory_order_relaxed);
      size.store(0, std::memory_order_relaxed);
      lat.store(0, std::memory_order_relaxed);
      seq_buffer.clear();
      size_buffer.clear();
      lost_buffer.clear();
      lat_buffer.clear();
      continue;
    }

    const bool getter_semantics = (info.type & kSetter) != 0;

    if (!create && ptr_iter != impl_->sub_ptr_map.end() &&
        (ptr_iter->second.ser != stream_meta.ser || ptr_iter->second.schema != stream_meta.schema ||
         ptr_iter->second.getter_semantics != getter_semantics)) {
      impl_->sub_ptr_map.erase(ptr_iter);
      ptr_iter = impl_->sub_ptr_map.end();
      create = true;
    }

    if (create) {
      if VUNLIKELY (stream_meta.schema == SchemaType::kUnknown) {
        VLOG_W("ProxyServer: Creating subscriber with unknown schema (url=", info.url, ").");
      }

      subs_lock.unlock();

      std::shared_ptr<RawSub> sub;
      const auto& current_meta = stream_meta;

      try {
        sub = std::make_shared<RawSub>(info.url, InitType::kWithoutInit);

        if (getter_semantics) {
          sub->mark_as_getter();
        }

        sub->set_safety_quit(impl_->config.callback_mode);
        sub->set_latency_and_lost_enabled(true);

        apply_topic_transport(*sub, impl_->config, impl_->native_ip);

        sub->set_discovery_enabled(false);
        sub->set_ser_type(current_meta.ser, current_meta.schema);
        sub->init();

        total_seq.store(0, std::memory_order_relaxed);

        ProxyAPI::Data data;
        data.url = info.url;
        data.ser = current_meta.ser;
        data.schema = current_meta.schema;

        sub->listen([this, sub_ptr = sub.get(), data = std::move(data), &total_seq, &seq, &size, &lat,
                     &elapsed](const Bytes& bytes) mutable {
          if VUNLIKELY (is_ready_to_quit() || ProxyServerGlobal::get().has_quit.load(std::memory_order_acquire) ||
                        impl_->discovery_viewer->is_ready_to_quit()) {
            return;
          }

          seq.fetch_add(1, std::memory_order_relaxed);
          size.fetch_add(bytes.size(), std::memory_order_relaxed);
          lat.fetch_add(sub_ptr->get_latency(), std::memory_order_relaxed);
          elapsed.restart();

          const auto& url = data.url;
          const auto& ser = data.ser;
          const auto schema = data.schema;

          if VUNLIKELY (impl_->config.callback_mode) {
            if VUNLIKELY (impl_->real_max_packet_size > 0 && bytes.size() > impl_->real_max_packet_size) {
              return;
            }

            {
              std::shared_lock control_lock(impl_->control_mtx);
              const auto mode = impl_->mode.load(std::memory_order_relaxed);

              if VUNLIKELY (mode != ProxyAPI::kObserveAll && mode != ProxyAPI::kAutoAndObserveAll &&
                            impl_->sub_urls.count(url) == 0) {
                return;
              }
            }

            if VLIKELY (impl_->data_callback) {
              data.raw.shallow_copy(bytes);
              data.timestamp = impl_->main_elapsed.get();
              data.seq = total_seq.load(std::memory_order_relaxed);
              impl_->data_callback(data);
            }

            total_seq.fetch_add(1, std::memory_order_relaxed);
            return;
          }

          if (!impl_->config.direct) {
            if VUNLIKELY (impl_->real_max_packet_size > 0 && bytes.size() > impl_->real_max_packet_size) {
              return;
            }

            {
              std::shared_lock control_lock(impl_->control_mtx);

              if VUNLIKELY (!impl_->data_pub->has_subscribers()) {
                return;
              }

              if (impl_->mode.load(std::memory_order_relaxed) != ProxyAPI::kAutoAndObserveAll &&
                  impl_->sub_urls.count(url) == 0) {
                return;
              }
            }

            const auto current_seq = total_seq.load(std::memory_order_relaxed);

            if (impl_->config.async) {
              zerocopy::ProxyData t_data;
              t_data.set_control_id(impl_->control_id.load(std::memory_order_relaxed));
              t_data.set_mode(impl_->mode.load(std::memory_order_relaxed));
              t_data.set_timestamp(impl_->main_elapsed.get());
              t_data.set_seq(current_seq);

              t_data.create(bytes, url, ser, static_cast<uint32_t>(schema), impl_->current_host_name);

              auto forward_task = [this, t_data = std::move(t_data)]() { impl_->data_pub->publish(t_data, true); };

              if VUNLIKELY (!impl_->forward_loop.post_task(std::move(forward_task))) {
                VLOG_E("ProxyServer: Failed to post async forwarding task.");
                return;
              }
            } else {
              zerocopy::ProxyData t_data;
              t_data.set_control_id(impl_->control_id.load(std::memory_order_relaxed));
              t_data.set_mode(impl_->mode.load(std::memory_order_relaxed));
              t_data.set_timestamp(impl_->main_elapsed.get());
              t_data.set_seq(current_seq);

              t_data.create(bytes, url, ser, static_cast<uint32_t>(schema), impl_->current_host_name);

              impl_->data_pub->publish(t_data, true);
            }
          }

          total_seq.fetch_add(1, std::memory_order_relaxed);
        });

        subs_lock.lock();

        impl_->sub_ptr_map.emplace(
            info.url, ProxySubEntry{std::move(sub), current_meta.ser, current_meta.schema, getter_semantics});
      } catch (Exception::RuntimeError&) {
        impl_->sub_error_url_set.emplace(info.url);
        seq.store(0, std::memory_order_relaxed);
        size.store(0, std::memory_order_relaxed);
        lost.store(0, std::memory_order_relaxed);
        lat.store(0, std::memory_order_relaxed);
        seq_buffer.clear();
        size_buffer.clear();
        lost_buffer.clear();
        lat_buffer.clear();
        continue;
      }
    } else if (ptr_iter != impl_->sub_ptr_map.end()) {
      auto& last_sample = impl_->sub_last_sample_map[info.url];

      const auto& sample_info = ptr_iter->second.node->get_lost();

      int64_t total_sample = sample_info.total - last_sample.total;
      int64_t lost_sample = sample_info.lost - last_sample.lost;

      if (total_sample > 0 && lost_sample > 0) {
        lost.store(static_cast<double>(lost_sample) / total_sample, std::memory_order_relaxed);
      } else {
        lost.store(0, std::memory_order_relaxed);
      }

      last_sample = sample_info;
    }

    subs_lock.unlock();

    if VUNLIKELY (impl_->main_elapsed.get() < 10'000) {
      out_info.status = ProxyAPI::kPending;
      seq.store(0, std::memory_order_relaxed);
      lost.store(0, std::memory_order_relaxed);
      size.store(0, std::memory_order_relaxed);
      lat.store(0, std::memory_order_relaxed);
      seq_buffer.clear();
      size_buffer.clear();
      lost_buffer.clear();
      lat_buffer.clear();
    }

    double freq = 0;
    double rate = 0;
    double loss = 0;
    double latency = 0;
    int weight = 1;
    int total_weight = 0;
    const int64_t current_seq = seq.exchange(0, std::memory_order_relaxed);
    const size_t current_size = size.exchange(0, std::memory_order_relaxed);
    const int64_t current_lat = lat.exchange(0, std::memory_order_relaxed);

    seq_buffer.emplace_back(current_seq);
    while (seq_buffer.size() > kCounterCache) {
      seq_buffer.pop_front();
    }

    size_buffer.emplace_back(current_size);
    while (size_buffer.size() > kCounterCache) {
      size_buffer.pop_front();
    }

    lost_buffer.emplace_back(lost.load(std::memory_order_relaxed));
    while (lost_buffer.size() > kCounterCache) {
      lost_buffer.pop_front();
    }

    if VUNLIKELY (current_seq <= 0) {
      lat_buffer.emplace_back(current_lat);
    } else {
      lat_buffer.emplace_back(static_cast<double>(current_lat) / current_seq);
    }

    while (lat_buffer.size() > kCounterCache) {
      lat_buffer.pop_front();
    }

    if VLIKELY (seq_buffer.size() == size_buffer.size()) {
      for (size_t i = 0; i < seq_buffer.size(); ++i) {
        freq += seq_buffer[i] * weight;
        rate += size_buffer[i] * weight;
        loss += lost_buffer[i] * weight;
        latency += lat_buffer[i] * weight;
        total_weight += weight;
        weight *= kCounterWeight;
      }
    }

    if VLIKELY (total_weight > 0) {
      freq = freq / total_weight;
      rate = rate / total_weight;
      loss = loss / total_weight;
      latency = latency / total_weight;
    } else {
      freq = 0;
      rate = 0;
      loss = 0;
      latency = 0;
    }

    out_info.freq = static_cast<float>(freq);
    out_info.rate = static_cast<uint64_t>(rate);
    out_info.loss = static_cast<float>(loss);

    if (current_seq == 0) {
      out_info.latency = -1;
    } else if (latency > 5000'000'000 || latency < -500'000) {
      out_info.latency = -2;
    } else if (latency < 0) {
      out_info.latency = 0;
    } else {
      out_info.latency = static_cast<float>(latency / 1000'000);
    }
  }

  packet.control_id = impl_->control_id.load(std::memory_order_relaxed);
  packet.hostname = impl_->current_host_name;

  if VUNLIKELY (impl_->config.callback_mode) {
    if VLIKELY (impl_->info_callback) {
      impl_->info_callback(packet.info_list);
    }
  } else {
    impl_->info_pub->publish(packet, true);
  }
}

}  // namespace vlink
