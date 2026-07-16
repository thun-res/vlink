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
#include <deque>
#include <memory>
#include <shared_mutex>
#include <string>
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

using RawPub = Publisher<Bytes>;
using RawSub = Subscriber<Bytes>;

using DataPub = Publisher<zerocopy::ProxyData>;
using DataSub = Subscriber<zerocopy::ProxyData>;

using TimePub = SecurityPublisher<proxy::TimePacket>;
using InfoPub = SecurityPublisher<proxy::InfoListPacket>;
using ControlSub = SecuritySubscriber<proxy::ControlPacket>;

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

// ProxyServer::Impl
struct ProxyServer::Impl final {  // NOLINT(clang-analyzer-optin.performance.Padding)
  std::atomic<uint32_t> control_id{0};
  std::atomic<ProxyAPI::Mode> mode{ProxyAPI::kOffline};

  std::string current_host_name;
  std::string current_machine_id;
  std::string token;

  size_t real_max_packet_size{0};

  ProxyServer::Config config;

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

  std::unordered_map<std::string, std::shared_ptr<RawPub>> pub_ptr_map;
  std::unordered_map<std::string, ProxySubEntry> sub_ptr_map;
  std::unordered_map<std::string, std::atomic<int64_t>> sub_total_seq_map;
  std::unordered_map<std::string, std::atomic<int64_t>> sub_seq_map;
  std::unordered_map<std::string, std::atomic<size_t>> sub_size_map;
  std::unordered_map<std::string, std::atomic<double>> sub_lost_map;
  std::unordered_map<std::string, std::atomic<int64_t>> sub_lat_map;
  std::unordered_map<std::string, ElapsedTimer> sub_elapsed_map;
  std::unordered_map<std::string, std::deque<int64_t>> sub_seq_buffer_map;
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

  Timer time_timer;
  Timer info_timer;

  Plugin runnable_plugin;
  std::vector<std::shared_ptr<RunablePluginInterface>> runnable_interface_list;

  bool has_intra_bind{false};
};

// ProxyServer
ProxyServer::ProxyServer(const Config& config) : impl_(std::make_unique<Impl>()) {
  set_name("ProxyServer");

  impl_->config = config;

  impl_->current_host_name = Utils::get_host_name();
  impl_->current_machine_id = Utils::get_machine_id();

  impl_->real_max_packet_size = impl_->config.max_packet_size * 1024L * 1024L;

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

  ProxyServerGlobal::get().has_init.store(true, std::memory_order_release);
}

ProxyServer::~ProxyServer() {
  ProxyServerGlobal::get().has_quit.store(true, std::memory_order_release);

  quit(true);
  wait_for_quit();

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
  impl_->sub_seq_buffer_map.clear();
  impl_->sub_size_buffer_map.clear();

#if VLINK_PROXY_ENABLE_HANDSHAKE
  impl_->handshake_srv.reset();
#endif

  impl_->data_sub.reset();
  impl_->data_pub.reset();
  impl_->time_pub.reset();
  impl_->info_pub.reset();
  impl_->control_sub.reset();

  impl_->discovery_viewer.reset();

  impl_->runnable_plugin.clear();
}

std::string ProxyServer::get_token() const { return impl_->token; }

size_t ProxyServer::get_max_task_count() const { return kMaxTaskSize; }

uint32_t ProxyServer::get_max_elapsed_time() const { return kMaxTaskElapsed; }

void ProxyServer::on_begin() {
  for (const auto& runnable : impl_->runnable_interface_list) {
    runnable->on_init();
    runnable->async_run();
  }
}

void ProxyServer::on_end() {
  for (const auto& runnable : impl_->runnable_interface_list) {
    runnable->on_deinit();
    runnable->quit();
    runnable->wait_for_quit();
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

  if (!impl_->config.bind_ip.empty()) {
    impl_->data_pub->set_property("dds.ip", impl_->config.bind_ip);
    impl_->data_sub->set_property("dds.ip", impl_->config.bind_ip);
    impl_->time_pub->set_property("dds.ip", impl_->config.bind_ip);
    impl_->info_pub->set_property("dds.ip", impl_->config.bind_ip);
    impl_->control_sub->set_property("dds.ip", impl_->config.bind_ip);
#if VLINK_PROXY_ENABLE_HANDSHAKE
    impl_->handshake_srv->set_property("dds.ip", impl_->config.bind_ip);
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
    impl_->data_pub->set_property("dds.ip", "127.0.0.1");
    impl_->data_sub->set_property("dds.ip", "127.0.0.1");
    impl_->time_pub->set_property("dds.ip", "127.0.0.1");
    impl_->info_pub->set_property("dds.ip", "127.0.0.1");
    impl_->control_sub->set_property("dds.ip", "127.0.0.1");
#if VLINK_PROXY_ENABLE_HANDSHAKE
    impl_->handshake_srv->set_property("dds.ip", "127.0.0.1");
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
  impl_->time_timer.attach(impl_->discovery_viewer.get());
  impl_->time_timer.set_callback([this]() { send_time(); });
  impl_->time_timer.start();

  impl_->info_timer.set_interval(kCollectInterval);
  impl_->info_timer.set_loop_count(Timer::kInfinite);
  impl_->info_timer.attach(impl_->discovery_viewer.get());
  impl_->info_timer.set_callback([this]() { update_all(); });
  impl_->info_timer.start();

  impl_->info_pub->detect_subscribers([this](bool connected) {
    if VUNLIKELY (impl_->mode.load(std::memory_order_relaxed) != ProxyAPI::kOffline && !connected &&
                  !impl_->info_pub->has_subscribers()) {
      impl_->discovery_viewer->post_task([this]() {
        proxy::ControlPacket packet;
        packet.control_id = impl_->control_id.load(std::memory_order_relaxed);
        packet.body.mode = ProxyAPI::kOffline;

        send_control(&packet);
      });
    }
  });

  if (impl_->data_sub->has_inited()) {
    impl_->data_sub->listen([this](const zerocopy::ProxyData& t_data) {
      if VUNLIKELY (ProxyServerGlobal::get().has_quit.load(std::memory_order_acquire)) {
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

      if (!impl_->config.direct) {
        if (impl_->config.async) {
          post_task([this, t_data]() {
            std::shared_lock lock(impl_->pubs_mtx);
            auto iter = impl_->pub_ptr_map.find(std::string(t_data.url()));

            if VUNLIKELY (iter == impl_->pub_ptr_map.end()) {
              return;
            }

            if VLIKELY (iter->second->has_subscribers()) {
              iter->second->publish(t_data.raw(), true);
            }
          });
        } else {
          std::shared_lock lock(impl_->pubs_mtx);
          auto iter = impl_->pub_ptr_map.find(std::string(t_data.url()));

          if VUNLIKELY (iter == impl_->pub_ptr_map.end()) {
            return;
          }

          if VLIKELY (iter->second->has_subscribers()) {
            iter->second->publish(t_data.raw(), true);
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

    impl_->discovery_viewer->post_task([this, packet]() { send_control(&packet); });
  });

  impl_->discovery_viewer->async_run();
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
#if VLINK_PROXY_ENABLE_HANDSHAKE
  time.token = impl_->token;
#endif

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
  std::unordered_map<std::string, ProxySubMeta> next_pub_meta_map;

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
      } else if (url_meta.type == kPublisher) {
        if (url_meta.ser.empty()) {
          continue;
        }

        next_pub_meta_map.try_emplace(url_meta.url, ProxySubMeta{url_meta.ser, schema});
      }
    }
  }

  ProxyAPI::Mode last_mode = impl_->mode.load(std::memory_order_relaxed);
  std::unordered_set<std::string> last_sub_urls;

  impl_->control_id.store(packet.control_id, std::memory_order_relaxed);
  impl_->mode.store(next_mode, std::memory_order_relaxed);

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

  impl_->discovery_viewer->post_task([this]() { send_time(); });
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
      impl_->discovery_viewer->post_task([this]() { update_all(); });
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
            auto* pub = pub_iter->second.get();

            if (pub && pub->get_ser_type() == meta.ser && pub->get_schema_type() == meta.schema) {
              continue;
            }

            impl_->pub_ptr_map.erase(pub_iter);
          }

          try {
            auto pub = std::make_shared<RawPub>(url, InitType::kWithoutInit);

            if (impl_->config.native_mode) {
              pub->set_property("dds.ip", "127.0.0.1");
            }

            pub->set_ser_type(meta.ser, meta.schema);
            pub->set_discovery_enabled(true);
            pub->init();

            impl_->pub_ptr_map.emplace(url, std::move(pub));
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
    impl_->discovery_viewer->post_task([this]() { update_all(); });
    impl_->info_timer.restart();
  }
}

void ProxyServer::update_all() {
  if VUNLIKELY (!impl_->info_pub->has_subscribers()) {
    {
      std::lock_guard pubs_lock(impl_->pubs_mtx);
      impl_->pub_ptr_map.clear();
    }

    {
      std::lock_guard subs_lock(impl_->subs_mtx);
      impl_->sub_ptr_map.clear();
    }

    {
      std::lock_guard control_lock(impl_->control_mtx);
      impl_->sub_urls.clear();
      impl_->requested_sub_meta_map.clear();
    }

    return;
  }

  if (impl_->mode.load(std::memory_order_relaxed) != ProxyAPI::kObserveOne &&
      impl_->mode.load(std::memory_order_relaxed) != ProxyAPI::kObserveAll &&
      impl_->mode.load(std::memory_order_relaxed) != ProxyAPI::kRecord &&
      impl_->mode.load(std::memory_order_relaxed) != ProxyAPI::kAuto &&
      impl_->mode.load(std::memory_order_relaxed) != ProxyAPI::kAutoAndObserveAll) {
    return;
  }

  proxy::InfoListPacket packet;

  const auto& info_list = impl_->discovery_viewer->get_info_list();

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

        iter = impl_->sub_seq_buffer_map.erase(iter);
      } else {
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

      switch (impl_->filter_type) {
        case 0:
          break;
        case 1:
          if (!(info.type & kPublisher && info.type & kSubscriber)) {
            continue;
          }

          break;
        case 2:
          if (!(info.type & kServer && info.type & kClient)) {
            continue;
          }

          break;
        case 3:
          if (!(info.type & kSetter && info.type & kGetter)) {
            continue;
          }

          break;
        case 4:
          if (!((info.type & kPublisher) || (info.type & kSubscriber))) {
            continue;
          }

          break;
        case 5:
          if (!((info.type & kServer) || (info.type & kClient))) {
            continue;
          }

          break;
        case 6:
          if (!((info.type & kSetter) || (info.type & kGetter))) {
            continue;
          }

          break;
        case 7:
          if (!(info.type & kPublisher)) {
            continue;
          }

          break;
        case 8:
          if (!(info.type & kSubscriber)) {
            continue;
          }

          break;
        case 9:
          if (!(info.type & kServer)) {
            continue;
          }

          break;
        case 10:
          if (!(info.type & kClient)) {
            continue;
          }

          break;
        case 11:
          if (!(info.type & kSetter)) {
            continue;
          }

          break;
        case 12:
          if (!(info.type & kGetter)) {
            continue;
          }

          break;
        default:
          break;
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
      out_process.ip = process.ip;
    }

    if ((!(info.type & kPublisher) && !(info.type & kSetter)) ||
        (!impl_->has_intra_bind && impl_->runnable_interface_list.empty() && Url::is_intra_type(info.url))) {
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

    if (!create && ptr_iter != impl_->sub_ptr_map.end() &&
        (ptr_iter->second.ser != stream_meta.ser || ptr_iter->second.schema != stream_meta.schema)) {
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

        // A setter is the field data source; the proxy must observe it as a getter.

        if (info.type & kSetter) {
          sub->mark_as_getter();
        }

        sub->set_latency_and_lost_enabled(true);

        if (impl_->config.native_mode) {
          sub->set_property("dds.ip", "127.0.0.1");
        }

        sub->set_discovery_enabled(false);
        sub->set_ser_type(current_meta.ser, current_meta.schema);
        sub->init();

        total_seq.store(0, std::memory_order_relaxed);

        sub->listen([this, sub_ptr = sub.get(), url = info.url, ser = current_meta.ser, schema = current_meta.schema,
                     &total_seq, &seq, &size, &lat, &elapsed](const Bytes& bytes) {
          if VUNLIKELY (ProxyServerGlobal::get().has_quit.load(std::memory_order_acquire) ||
                        impl_->discovery_viewer->is_ready_to_quit()) {
            return;
          }

          seq.fetch_add(1, std::memory_order_relaxed);
          size.fetch_add(bytes.size(), std::memory_order_relaxed);
          lat.fetch_add(sub_ptr->get_latency(), std::memory_order_relaxed);
          elapsed.restart();

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

              if VUNLIKELY (!post_task(std::move(forward_task))) {
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

        impl_->sub_ptr_map.emplace(info.url, ProxySubEntry{std::move(sub), current_meta.ser, current_meta.schema});
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
  impl_->info_pub->publish(packet, true);
}

}  // namespace vlink
