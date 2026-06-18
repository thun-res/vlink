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

#include "./shm_factory.h"

#include <charconv>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#ifdef __QNX__
#include <filesystem>
#define SHM_QNX_LOCK_DIR "/var/lock"
#endif

#include "./impl/server_impl.h"

#define SHM_USE_RUNTIME_IMPL 1

#include <iceoryx_posh/internal/roudi/roudi.hpp>
#include <iceoryx_posh/internal/runtime/ipc_runtime_interface.hpp>
#include <iceoryx_posh/internal/runtime/posh_runtime_impl.hpp>

#ifdef _WIN32
#include <Windows.h>
#undef min
#undef max
#undef GetMessage
#endif

namespace vlink {

// ShmGlobal
struct ShmGlobal final {
  std::atomic_bool has_roudi_inited{false};
  std::atomic_bool has_runtime_inited{false};
  std::atomic_bool has_factory_inited{false};
  class ShmRuntime* runtime_instance{nullptr};
  std::unique_ptr<shm::runtime::PoshRuntime> shm_runtime;
  shm::RuntimeName_t shm_runtime_name;
#ifdef VLINK_SUPPORT_SHM_ROUDI
  shm::cxx::optional<shm::roudi::IceOryxRouDiComponents> roudi_components;
  shm::cxx::optional<shm::roudi::RouDi> roudi;
#endif

  static ShmGlobal& get() {
    static ShmGlobal instance;
    return instance;
  }

 private:
  ShmGlobal() = default;
};

#if SHM_USE_RUNTIME_IMPL
// ShmRuntime
class ShmRuntime final : public shm::runtime::PoshRuntimeImpl {
 public:
  explicit ShmRuntime(const shm::RuntimeName_t& name, bool same_process_from_roudi = false)
#if ICEORYX_VERSION_MAJOR == 2 && ICEORYX_VERSION_MINOR == 0
      : shm::runtime::PoshRuntimeImpl(shm::cxx::make_optional<const shm::RuntimeName_t*>(&name),
                                      same_process_from_roudi
                                          ? shm::runtime::RuntimeLocation::SAME_PROCESS_LIKE_ROUDI
                                          : shm::runtime::RuntimeLocation::SEPARATE_PROCESS_FROM_ROUDI)
#else
      : shm::runtime::PoshRuntimeImpl(shm::cxx::make_optional<const shm::RuntimeName_t*>(&name), shm::DEFAULT_DOMAIN_ID,
                                      same_process_from_roudi
                                          ? shm::runtime::RuntimeLocation::SAME_PROCESS_LIKE_ROUDI
                                          : shm::runtime::RuntimeLocation::SEPARATE_PROCESS_FROM_ROUDI)
#endif
  {
    ShmGlobal::get().runtime_instance = this;

    setRuntimeFactory(get_runtime_factory);
  }

  ~ShmRuntime() override { ShmGlobal::get().runtime_instance = nullptr; }

  static PoshRuntime& get_runtime_factory(shm::cxx::optional<const shm::RuntimeName_t*>) {
    return *ShmGlobal::get().runtime_instance;
  }
};
#endif

// ShmFactory
ShmFactory::ShmFactory() {
  static auto& global_instance = ShmGlobal::get();

  global_instance.has_factory_inited.store(true);

  Bytes::init_memory_pool();

  if VUNLIKELY (ShmConf::get_thread_count() != 1) {
    VLOG_W("ShmFactory: Shm does not support setting thread count.");
  }

  message_loop_.set_name("SHM-FACTORY");

  detect_timer_.attach(&message_loop_);
  detect_timer_.set_interval(10);
  detect_timer_.set_loop_count(Timer::kInfinite);
  detect_timer_.set_callback([this]() {
    if VUNLIKELY (!global_instance.has_runtime_inited) {
      return;
    }

    detect_timer_.set_interval(50);

    if (port_sub_) {
      port_sub_->take().and_then(
          [this](const shm::popo::Sample<const shm::roudi::PortIntrospectionFieldTopic>& sample) {
            std::lock_guard lock(topic_mtx_);
            topic_list_ = *sample;
          });
    }

    std::shared_lock lock(detect_mtx_);
    for (const auto& [publisher, callback] : detect_map_) {
      callback();
    }
  });

  message_loop_.async_run();

  init_log_level(true);

  init_runtime();

  init_log_level(false);

  {
    std::string depth_env_str = Utils::get_env("VLINK_SHM_DEPTH");

    if (!depth_env_str.empty()) {
      auto [p, error] = std::from_chars(depth_env_str.data(), depth_env_str.data() + depth_env_str.size(), sub_depth_);

      if VUNLIKELY (error != std::errc() || sub_depth_ <= 0) {
        sub_depth_ = kDefaultSubDepth;
      }
    }
  }
}

ShmFactory::~ShmFactory() {
  detect_timer_.stop();
  detect_timer_.detach();

  message_loop_.quit();
  message_loop_.wait_for_quit();

  deinit_runtime();
  deinit_roudi();
}

bool ShmFactory::has_roudi_inited() { return ShmGlobal::get().has_roudi_inited; }

bool ShmFactory::has_runtime_inited() { return ShmGlobal::get().has_runtime_inited; }

bool ShmFactory::has_roudi_running() {
#if ICEORYX_VERSION_MAJOR == 2 && ICEORYX_VERSION_MINOR == 0
  shm::runtime::IpcInterfaceUser roudi_ipc(
      shm::RuntimeName_t(shm::cxx::TruncateToCapacity, shm::roudi::IPC_CHANNEL_ROUDI_NAME));

  return roudi_ipc.isInitialized();
#else
  shm::runtime::IpcInterfaceUser roudi_ipc(
      shm::RuntimeName_t(shm::TruncateToCapacity, shm::roudi::IPC_CHANNEL_ROUDI_NAME), shm::DEFAULT_DOMAIN_ID,
      shm::ResourceType::ICEORYX_DEFINED);

  return roudi_ipc.isInitialized();
#endif
}

bool ShmFactory::auto_init_roudi(bool same_process_from_roudi) {
  Bytes::init_memory_pool();

  struct RoudiManager final {
    explicit RoudiManager(bool same_process_from_roudi) {
      bool roudi_running = ShmFactory::has_roudi_running();

      if (!roudi_running) {
#ifdef _WIN32

        if (same_process_from_roudi) {
          status_ = false;
          return;
        }
#else
        ShmFactory::init_roudi();
#endif
      }

      ShmFactory::init_log_level(false);
      ShmFactory::init_runtime({}, !roudi_running && same_process_from_roudi);
    }

    ~RoudiManager() {
      if (status_) {
        ShmFactory::deinit_runtime();
        ShmFactory::deinit_roudi();
      }
    }

    bool status() const { return status_; }

   private:
    bool status_{true};
  };

  static RoudiManager manager(same_process_from_roudi);

  return manager.status();
}

shm::capro::ServiceDescription ShmFactory::get_description(const std::string& service, const std::string& instance,
                                                           const std::string& event) {
#if ICEORYX_VERSION_MAJOR == 2 && ICEORYX_VERSION_MINOR == 0
  return {shm::capro::IdString_t(shm::cxx::TruncateToCapacity, service.c_str()),
          shm::capro::IdString_t(shm::cxx::TruncateToCapacity, instance.c_str()),
          shm::capro::IdString_t(shm::cxx::TruncateToCapacity, event.c_str())};
#else
  return {shm::capro::IdString_t(shm::TruncateToCapacity, service.c_str()),
          shm::capro::IdString_t(shm::TruncateToCapacity, instance.c_str()),
          shm::capro::IdString_t(shm::TruncateToCapacity, event.c_str())};
#endif
}

void ShmFactory::init_log_level(bool wait_roudi) {
#if ICEORYX_VERSION_MAJOR == 2 && ICEORYX_VERSION_MINOR == 0
  auto& log_manager = shm::log::LogManager::GetLogManager();

  if (wait_roudi) {
    log_manager.SetDefaultLogLevel(shm::log::LogLevel::kWarn, shm::log::LogLevelOutput::kHideLogLevel);
    return;
  }

  static std::string shm_debug_str = Utils::get_env("VLINK_SHM_DEBUG");

  if (shm_debug_str == "1") {
    log_manager.SetDefaultLogLevel(shm::log::LogLevel::kInfo, shm::log::LogLevelOutput::kHideLogLevel);
  } else {
    log_manager.SetDefaultLogLevel(shm::log::LogLevel::kFatal, shm::log::LogLevelOutput::kHideLogLevel);
  }
#else

  if (wait_roudi) {
    shm::log::Logger::setLogLevel(shm::log::LogLevel::Warn);
    return;
  }

  static std::string shm_debug_str = Utils::get_env("VLINK_SHM_DEBUG");

  if (shm_debug_str == "1") {
    shm::log::Logger::setLogLevel(shm::log::LogLevel::Info);
  } else {
    shm::log::Logger::setLogLevel(shm::log::LogLevel::Fatal);
  }
#endif
}

void ShmFactory::init_roudi(const std::string& config_path, int memory_strategy, bool monitoring_enable) {
  Bytes::init_memory_pool();

#ifdef VLINK_SUPPORT_SHM_ROUDI

#if __QNX__
  try {
    if (!std::filesystem::exists(SHM_QNX_LOCK_DIR)) {
      std::filesystem::create_directories(SHM_QNX_LOCK_DIR);
    }
  } catch (std::filesystem::filesystem_error&) {
  }
#endif

  bool expected = false;

  if VUNLIKELY (!ShmGlobal::get().has_roudi_inited.compare_exchange_strong(expected, true)) {
    return;
  }

  init_log_level(false);

  VLOG_I("ShmFactory: Start shm roudi.");

  shm::RouDiConfig_t config;

  if (!config_path.empty()) {
    shm::config::CmdLineArgs_t cmd;

#if ICEORYX_VERSION_MAJOR == 2 && ICEORYX_VERSION_MINOR == 0
    cmd.configFilePath = shm::roudi::ConfigFilePathString_t(shm::cxx::TruncateToCapacity, config_path.c_str());
#else
    cmd.configFilePath = shm::roudi::ConfigFilePathString_t(shm::TruncateToCapacity, config_path.c_str());
#endif

    shm::config::TomlRouDiConfigFileProvider provider(cmd);
    config = provider.parse().value();
  } else {
    shm::mepoo::MePooConfig poo_config;

    if (memory_strategy == 1) {  // low
      poo_config.addMemPool({1024, 5000});
      poo_config.addMemPool({16384, 1000});
      poo_config.addMemPool({131072, 100});
      poo_config.addMemPool({1048576, 20});
      poo_config.addMemPool({4194304, 10});
      poo_config.addMemPool({8388608, 5});
    } else if (memory_strategy == 3) {  // high
      poo_config.addMemPool({1024, 10000});
      poo_config.addMemPool({16384, 1000});
      poo_config.addMemPool({131072, 500});
      poo_config.addMemPool({1048576, 200});
      poo_config.addMemPool({3145728, 100});
      poo_config.addMemPool({6291456, 50});
      poo_config.addMemPool({13631488, 30});
      poo_config.addMemPool({25165824, 20});
    } else {  // default
      poo_config.addMemPool({1024, 10000});
      poo_config.addMemPool({16384, 1000});
      poo_config.addMemPool({131072, 500});
      poo_config.addMemPool({1048576, 100});
      poo_config.addMemPool({3145728, 50});
      poo_config.addMemPool({6291456, 30});
      poo_config.addMemPool({13631488, 20});
    }

#if ICEORYX_VERSION_MAJOR == 2 && ICEORYX_VERSION_MINOR == 0
    auto group_name = shm::posix::PosixGroup::getGroupOfCurrentProcess().getName();
#else
    auto group_name = shm::PosixGroup::getGroupOfCurrentProcess().getName();
#endif

    config.m_sharedMemorySegments.emplace_back(group_name, group_name, std::move(poo_config));
  }

#if ICEORYX_VERSION_MAJOR == 2 && ICEORYX_VERSION_MINOR == 0
  shm::roudi::RouDi::RoudiStartupParameters parameters{
      monitoring_enable ? shm::roudi::MonitoringMode::ON : shm::roudi::MonitoringMode::OFF, true,
      shm::roudi::RouDi::RuntimeMessagesThreadStart::IMMEDIATE, shm::version::CompatibilityCheckLevel::PATCH,
      shm::units::Duration::fromSeconds(10)};
#else
  shm::config::RouDiConfig parameters;
  parameters.sharesAddressSpaceWithApplications = true;
  parameters.monitoringMode = monitoring_enable ? shm::roudi::MonitoringMode::ON : shm::roudi::MonitoringMode::OFF;
#endif

  ShmGlobal::get().roudi_components.emplace(config);
  ShmGlobal::get().roudi.emplace(ShmGlobal::get().roudi_components.value().rouDiMemoryManager,
                                 ShmGlobal::get().roudi_components.value().portManager, parameters);
#else
  (void)config_path;
  (void)memory_strategy;
  (void)monitoring_enable;

  VLOG_F("ShmFactory: Shm roudi is not supported.");
#endif
}

void ShmFactory::init_runtime(std::string name, bool same_process_from_roudi) {
  Bytes::init_memory_pool();

  static auto& global_instance = ShmGlobal::get();

  bool expected = false;

  if VLIKELY (global_instance.has_runtime_inited.compare_exchange_strong(expected, true)) {
    if (name.empty()) {
      name = Utils::get_app_name() + "_" + Utils::get_pid_str();
    }

#if ICEORYX_VERSION_MAJOR == 2 && ICEORYX_VERSION_MINOR == 0
    global_instance.shm_runtime_name = shm::RuntimeName_t(shm::cxx::TruncateToCapacity, name.c_str());
#else
    global_instance.shm_runtime_name = shm::RuntimeName_t(shm::TruncateToCapacity, name.c_str());
#endif

#if SHM_USE_RUNTIME_IMPL
    global_instance.shm_runtime =
        std::make_unique<ShmRuntime>(global_instance.shm_runtime_name, same_process_from_roudi);
#else
    (void)same_process_from_roudi;
    shm::runtime::PoshRuntime::initRuntime(global_instance.shm_runtime_name);
#endif
  }
}

void ShmFactory::deinit_runtime() {
  static auto& global_instance = ShmGlobal::get();

  bool expected = true;

  if VLIKELY (global_instance.has_runtime_inited.compare_exchange_strong(expected, false)) {
    if (global_instance.has_factory_inited.load()) {
      auto& factory = ShmFactory::get();

      {
        std::lock_guard lock(factory.listener_mtx_);
        factory.listener_map_.clear();
      }

      factory.port_sub_.reset();

      Utils::yield_cpu();
    }

#if SHM_USE_RUNTIME_IMPL

    if (global_instance.shm_runtime) {
      global_instance.shm_runtime->shutdown();
      global_instance.shm_runtime.reset();
    }
#endif
  }
}

void ShmFactory::deinit_roudi() {
#ifdef VLINK_SUPPORT_SHM_ROUDI
  static auto& global_instance = ShmGlobal::get();

  bool expected = true;

  if VLIKELY (global_instance.has_roudi_inited.compare_exchange_strong(expected, false)) {
    global_instance.roudi.reset();
    global_instance.roudi_components.reset();
  }
#endif
}

shm::popo::Listener* ShmFactory::get_listener(int32_t domain) {
  std::lock_guard lock(listener_mtx_);

  auto iter = listener_map_.find(domain);

  if (iter == listener_map_.end()) {
    auto listener = std::make_shared<shm::popo::Listener>();

    return listener_map_.emplace(domain, std::move(listener)).first->second.get();
  }

  return iter->second.get();
}

void ShmFactory::try_to_destroy_listener(int32_t domain, shm::popo::Listener* listener) {
  std::lock_guard lock(listener_mtx_);

  if (listener) {
    if (listener->size() == 0) {
      listener_map_.erase(domain);
    }
  } else {
    auto iter = listener_map_.find(domain);
    if (iter != listener_map_.end()) {
      if (iter->second->size() == 0) {
        listener_map_.erase(iter);
      }
    }
  }
}

void ShmFactory::add_detect_callback(void* node, DetectCallback&& callback) {
  std::lock_guard lock(detect_mtx_);

  if (detect_map_.empty()) {
    detect_timer_.restart();
  }

  detect_map_[node] = callback;

  message_loop_.post_task([callback = std::move(callback)]() { callback(); });
}

void ShmFactory::remove_detect_callback(void* node) {
  std::lock_guard lock(detect_mtx_);

  detect_map_.erase(node);

  if (detect_map_.empty()) {
    detect_timer_.stop();
  }
}

void ShmFactory::start_detect_node_count() {
  if (!port_sub_) {
    shm::popo::SubscriberOptions port_sub_opt;
    port_sub_opt.queueCapacity = 1U;
    port_sub_opt.historyRequest = 1U;
    port_sub_.emplace(shm::roudi::IntrospectionPortService, port_sub_opt);
  }

  detect_timer_.restart();
}

uint64_t ShmFactory::get_publisher_count(const shm::capro::ServiceDescription& description) {
  std::shared_lock lock(topic_mtx_);

  uint64_t count = 0;

  for (const auto& info : topic_list_.m_publisherList) {
    if (info.m_caproServiceID == description.getServiceIDString() &&
        info.m_caproInstanceID == description.getInstanceIDString() &&
        info.m_caproEventMethodID == description.getEventIDString()) {
      ++count;
    }
  }

  return count;
}

uint64_t ShmFactory::get_subscriber_count(const shm::capro::ServiceDescription& description) {
  std::shared_lock lock(topic_mtx_);

  uint64_t count = 0;

  for (const auto& info : topic_list_.m_subscriberList) {
    if (info.m_caproServiceID == description.getServiceIDString() &&
        info.m_caproInstanceID == description.getInstanceIDString() &&
        info.m_caproEventMethodID == description.getEventIDString()) {
      ++count;
    }
  }

  return count;
}

int ShmFactory::get_sub_depth() const { return sub_depth_; }

// ShmServer
ShmServer::ShmServer(const ShmID& id) {
  static auto& factory = ShmFactory::get();

  const auto& [impl_type, address, domain, depth, history, wait] = id;

  domain_ = domain;

  shm::popo::ServerOptions options;
  options.offerOnCreate = false;

  if (depth > 0) {
    options.requestQueueCapacity = depth;
  } else {
    options.requestQueueCapacity = kDefaultReqDepth;
  }

  std::string event = "method";

  if (domain != 0) {
    event += ("_" + std::to_string(domain));
  }

  server_.emplace(ShmFactory::get_description("vlink", address, event), options);

  listener_ = factory.get_listener(domain_);

  // LCOV_EXCL_START
  listener_
      ->attachEvent(server_.value(), shm::popo::ServerEvent::REQUEST_RECEIVED,
                    shm::popo::createNotificationCallback(ShmServer::on_request_received, *this))
      .or_else([](auto&) { VLOG_F("ShmFactory: Failed to attach REQUEST_RECEIVED event to listener."); });
  // LCOV_EXCL_STOP
}

ShmServer::~ShmServer() {
  static auto& factory = ShmFactory::get();

  listener_->detachEvent(server_.value(), shm::popo::ServerEvent::REQUEST_RECEIVED);
  factory.try_to_destroy_listener(domain_, listener_);

  server_->stopOffer();
  server_->releaseQueuedRequests();
}

std::any ShmServer::get_native_handle() const { return this; }

bool ShmServer::suspend() {
  is_suspend_ = true;

  return true;
}

bool ShmServer::resume() {
  is_suspend_ = false;

  return true;
}

bool ShmServer::is_suspend() const { return is_suspend_; }

void ShmServer::process_message() {
  while (server_->hasRequests()) {
    server_->take()
        .and_then([this](const void* buffer) {
          const auto* read_req = static_cast<const uint8_t*>(buffer);
          const auto* read_header = shm::popo::RequestHeader::fromPayload(read_req);

          uint64_t channel = 0;
          uint64_t seq = 0;
          Bytes req_bytes;
          ShmFactory::read_data(read_req, read_header->getChunkHeader()->userPayloadSize(), channel, seq, req_bytes);

          (void)seq;

          bool request_released = false;

          traverse_req_resp_callback([this, channel, &req_bytes, &read_req, &read_header, &request_released](
                                         NodeImpl* impl, const auto& callback) {
            const auto* conf_ptr = impl->get_target_conf<ShmConf>();

            if (static_cast<uint64_t>(conf_ptr->hash_code) != channel) {
              ignore_called();
              return;
            }

            if VUNLIKELY (has_called()) {
              VLOG_F(*conf_ptr, "Two identical service requests.");
              return;
            }

            std::lock_guard lock(mtx_);

            if (static_cast<ServerImpl*>(impl)->is_resp_type) {
              last_req_header_ = read_header;

              Bytes resp_bytes;

              auto seq = static_cast<uint64_t>(read_header->getSequenceId());

              callback(seq, req_bytes, &resp_bytes);
            } else {
              callback(0, req_bytes, nullptr);
            }

            server_->releaseRequest(read_req);
            request_released = true;
          });

          if VUNLIKELY (!request_released) {
            server_->releaseRequest(read_req);
          }
        })
        .or_else([](auto& e) { VLOG_E("ShmFactory: Failed to take request, error: ", e, "."); });
  }
}

void ShmServer::start() { server_->offer(); }

void ShmServer::stop() { server_->stopOffer(); }

bool ShmServer::has_clients() const { return server_->hasClients(); }

Bytes ShmServer::loan(uint64_t channel, int64_t size) {
  if VUNLIKELY (size <= 0) {
    return Bytes();
  }

  auto write_req_result =
      server_->loan(last_req_header_, size + ShmFactory::get_loaned_offset(), ShmFactory::get_loaned_alignment());

  ++seq_;

  if VUNLIKELY (write_req_result.has_error()) {
    VLOG_E("ShmFactory: Failed to loan buffer, size: ", size + ShmFactory::get_loaned_offset(),
           ", error: ", write_req_result.get_error(), ".");
    return Bytes();
  }

  auto* write_req = static_cast<uint8_t*>(write_req_result.value());

  ShmFactory::write_header(write_req, channel, seq_);

  return Bytes::loan_internal(write_req + ShmFactory::get_loaned_offset(), size);
}

bool ShmServer::release(const Bytes& bytes) {
  if VUNLIKELY (!bytes.is_loaned()) {
    return false;
  }

  server_->releaseResponse(const_cast<uint8_t*>(bytes.data()) - ShmFactory::get_loaned_offset());

  return true;
}

bool ShmServer::reply(uint64_t channel, const Bytes& resp_data) {
  if (resp_data.is_loaned()) {
    auto send_result = server_->send(const_cast<uint8_t*>(resp_data.data() - ShmFactory::get_loaned_offset()));

    if VUNLIKELY (send_result.has_error()) {
      VLOG_E("ShmFactory: Failed to send, error: ", send_result.get_error(), ".");
      return false;
    }
  } else {
    auto write_resp_result = server_->loan(last_req_header_, resp_data.size() + ShmFactory::get_loaned_offset(),
                                           ShmFactory::get_loaned_alignment());

    ++seq_;

    if VUNLIKELY (write_resp_result.has_error()) {
      VLOG_E("ShmFactory: Failed to loan buffer, size: ", resp_data.size() + ShmFactory::get_loaned_offset(),
             ", error: ", write_resp_result.get_error(), ".");
      return false;
    }

    auto* write_resp = static_cast<uint8_t*>(write_resp_result.value());

    ShmFactory::write_data(write_resp, channel, seq_, resp_data);

    auto send_result = server_->send(write_resp);

    if VUNLIKELY (send_result.has_error()) {
      VLOG_E("ShmFactory: Failed to send, error: ", send_result.get_error(), ".");
      return false;
    }
  }

  return true;
}

void ShmServer::on_request_received(shm::popo::UntypedServer*, ShmServer* target) {
  if VUNLIKELY (target->is_suspend_) {
    target->server_->releaseQueuedRequests();

    return;
  }

  auto* impl = target->get_first_impl();

  if VUNLIKELY (!impl) {
    return;
  }

  auto* message_loop = impl->get_message_loop();

  if (message_loop) {
    std::weak_ptr<ShmServer> weak_target = target->weak_from_this();
    message_loop->post_task([weak_target]() {
      auto target = weak_target.lock();

      if VUNLIKELY (!target) {
        return;
      }

      auto* impl = target->get_first_impl();

      if VUNLIKELY (!impl || !impl->get_message_loop()) {
        return;
      }

      target->process_message();
    });
  } else {
    target->process_message();
  }
}

// ShmClient
ShmClient::ShmClient(const ShmID& id) {
  static auto& factory = ShmFactory::get();

  const auto& [impl_type, address, domain, depth, history, wait] = id;

  domain_ = domain;

  shm::popo::ClientOptions options;
  options.connectOnCreate = true;

  if (depth > 0) {
    options.responseQueueCapacity = depth;
  } else {
    options.responseQueueCapacity = kDefaultRespDepth;
  }

  std::string event = "method";

  if (domain != 0) {
    event += ("_" + std::to_string(domain));
  }

  client_.emplace(ShmFactory::get_description("vlink", address, event), options);

  listener_ = factory.get_listener(domain_);

  // LCOV_EXCL_START
  listener_
      ->attachEvent(client_.value(), shm::popo::ClientEvent::RESPONSE_RECEIVED,
                    shm::popo::createNotificationCallback(ShmClient::on_response_received, *this))
      .or_else([](auto&) { VLOG_F("ShmFactory: Failed to attach RESPONSE_RECEIVED event to listener."); });
  // LCOV_EXCL_STOP
}

ShmClient::~ShmClient() {
  static auto& factory = ShmFactory::get();

  quit_flag_ = true;

  listener_->detachEvent(client_.value(), shm::popo::ClientEvent::RESPONSE_RECEIVED);
  factory.try_to_destroy_listener(domain_, listener_);

  client_->disconnect();
  client_->releaseQueuedResponses();

  disable_detect_timer();
}

std::any ShmClient::get_native_handle() const { return this; }

void ShmClient::process_message() {
  while (client_->hasResponses()) {
    client_->take()
        .and_then([this](const void* buffer) {
          std::unique_lock lock(mtx_);

          const auto* read_resp = static_cast<const uint8_t*>(buffer);
          const auto* read_header = shm::popo::ResponseHeader::fromPayload(read_resp);
          auto iter = callbacks_.find(read_header->getSequenceId());

          if VUNLIKELY (iter == callbacks_.end()) {
            client_->releaseResponse(read_resp);
            return;
          }

          auto callback = std::move(iter->second);
          auto seq_id = iter->first;

          lock.unlock();

          uint64_t channel = 0;
          uint64_t seq = 0;
          Bytes resp_bytes;
          ShmFactory::read_data(read_resp, read_header->getChunkHeader()->userPayloadSize(), channel, seq, resp_bytes);

          (void)seq;

          callback(channel, resp_bytes);

          client_->releaseResponse(read_resp);

          lock.lock();

          callbacks_.erase(seq_id);
        })
        .or_else([](auto& e) { VLOG_E("ShmFactory: Failed to take response, error: ", e, "."); });
  }
}

bool ShmClient::is_connected() const { return client_->getConnectionState() == shm::ConnectionState::CONNECTED; }

void ShmClient::enable_detect_timer() {
  if (!has_detect_timer_) {
    has_detect_timer_ = true;

    ShmFactory::get().add_detect_callback(this, [weak = weak_from_this()]() {
      auto self = weak.lock();

      if VLIKELY (self) {
        self->detect_server();
      }
    });
  }
}

void ShmClient::disable_detect_timer() {
  if (has_detect_timer_) {
    has_detect_timer_ = false;

    ShmFactory::get().remove_detect_callback(this);
  }
}

Bytes ShmClient::loan(uint64_t channel, int64_t size) {
  if VUNLIKELY (size <= 0) {
    return Bytes();
  }

  auto write_req_result = client_->loan(size + ShmFactory::get_loaned_offset(), ShmFactory::get_loaned_alignment());

  if VUNLIKELY (write_req_result.has_error()) {
    VLOG_E("ShmFactory: Failed to loan buffer, size: ", size + ShmFactory::get_loaned_offset(),
           ", error: ", write_req_result.get_error(), ".");
    return Bytes();
  }

  auto* write_req = static_cast<uint8_t*>(write_req_result.value());

  ShmFactory::write_header(write_req, channel, seq_);

  return Bytes::loan_internal(write_req + ShmFactory::get_loaned_offset(), size);
}

bool ShmClient::release(const Bytes& bytes) {
  if VUNLIKELY (!bytes.is_loaned()) {
    return false;
  }

  client_->releaseRequest(const_cast<uint8_t*>(bytes.data()) - ShmFactory::get_loaned_offset());

  return true;
}

bool ShmClient::call(uint64_t channel, const Bytes& req_data, NodeImpl::MsgCallback&& callback, uint64_t* seq_out) {
  if VUNLIKELY (!is_connected()) {
    return false;
  }

  std::lock_guard lock(mtx_);
  uint64_t response_seq = 0;
  bool has_response_callback = false;

  if (req_data.is_loaned()) {
    auto* write_header =
        shm::popo::RequestHeader::fromPayload(const_cast<uint8_t*>(req_data.data()) - ShmFactory::get_loaned_offset());

    if (callback) {
      response_seq = seq_.load(std::memory_order_relaxed);
      has_response_callback = true;
      callbacks_[response_seq] = [callback = std::move(callback), channel](uint64_t target_channel,
                                                                           const Bytes& bytes) {
        if (channel != target_channel) {
          return;
        }

        callback(bytes);
      };
      write_header->setSequenceId(response_seq);

      if (seq_out) {
        *seq_out = response_seq;
      }

      ++seq_;
    }

    auto send_result = client_->send(const_cast<uint8_t*>(req_data.data()) - ShmFactory::get_loaned_offset());

    if VUNLIKELY (send_result.has_error()) {
      VLOG_E("ShmFactory: Failed to send, error: ", send_result.get_error(), ".");
      if (has_response_callback) {
        callbacks_.erase(response_seq);
      }
      return false;
    }
  } else {
    auto write_req_result =
        client_->loan(req_data.size() + ShmFactory::get_loaned_offset(), ShmFactory::get_loaned_alignment());
    if VUNLIKELY (write_req_result.has_error()) {
      VLOG_E("ShmFactory: Failed to loan buffer, size: ", req_data.size() + ShmFactory::get_loaned_offset(),
             ", error: ", write_req_result.get_error(), ".");

      return false;
    }

    auto* write_req = static_cast<uint8_t*>(write_req_result.value());

    auto* write_header = shm::popo::RequestHeader::fromPayload(write_req);

    if (callback) {
      response_seq = seq_.load(std::memory_order_relaxed);
      has_response_callback = true;
      callbacks_[response_seq] = [callback = std::move(callback), channel](uint64_t target_channel,
                                                                           const Bytes& bytes) {
        if (channel != target_channel) {
          return;
        }

        callback(bytes);
      };
      write_header->setSequenceId(response_seq);

      if (seq_out) {
        *seq_out = response_seq;
      }

      ++seq_;
    }

    ShmFactory::write_data(write_req, channel, seq_, req_data);

    auto send_result = client_->send(write_req);

    if VUNLIKELY (send_result.has_error()) {
      VLOG_E("ShmFactory: Failed to send, error: ", send_result.get_error(), ".");

      if (has_response_callback) {
        callbacks_.erase(response_seq);
      }

      return false;
    }
  }

  return true;
}

void ShmClient::remove_response_callback(uint64_t seq) {
  std::lock_guard lock(mtx_);
  callbacks_.erase(seq);
}

void ShmClient::detect_server() {
  if VUNLIKELY (quit_flag_) {
    return;
  }

  if VUNLIKELY (!ShmFactory::has_runtime_inited()) {
    return;
  }

  discovery_server(is_connected());
}

void ShmClient::discovery_server(bool connect) {
  if VLIKELY (last_connected_ == connect) {
    return;
  }

  traverse_server_connect_callback([connect](NodeImpl*, const auto& callback) { callback(connect); });
  last_connected_ = connect;
}

void ShmClient::on_response_received(shm::popo::UntypedClient*, ShmClient* target) {
  auto* impl = target->get_first_impl();

  if VUNLIKELY (!impl) {
    return;
  }

  auto* message_loop = impl->get_message_loop();

  if (message_loop) {
    std::weak_ptr<ShmClient> weak_target = target->weak_from_this();
    message_loop->post_task([weak_target]() {
      auto target = weak_target.lock();

      if VUNLIKELY (!target) {
        return;
      }

      auto* impl = target->get_first_impl();

      if VUNLIKELY (!impl || !impl->get_message_loop()) {
        return;
      }

      target->process_message();
    });
  } else {
    target->process_message();
  }
}

// ShmPublisher
ShmPublisher::ShmPublisher(const ShmID& id) {
  static auto& factory = ShmFactory::get();

  const auto& [impl_type, address, domain, depth, history, wait] = id;

  domain_ = domain;
  wait_ = wait;

  shm::popo::PublisherOptions options;
  options.offerOnCreate = true;
  options.historyCapacity = history;

  std::string event = "event";

  if (domain != 0) {
    event += ("_" + std::to_string(domain));
  }

  pub_.emplace(ShmFactory::get_description("vlink", address, event), options);

  if (wait > 0) {
    factory.start_detect_node_count();

    sem_.emplace();

    std::string sem_address = address;
    std::replace(sem_address.begin(), sem_address.end(), '/', '@');

#ifdef __FreeBSD__
    sem_->attach("/vlink@shm@" + sem_address);
#else
    sem_->attach("vlink@shm@" + sem_address);
#endif
  }
}

ShmPublisher::~ShmPublisher() {
  quit_flag_ = true;

  if (sem_) {
    sem_->detach(true);
  }

  pub_->stopOffer();

  disable_detect_timer();
}

std::any ShmPublisher::get_native_handle() const { return this; }

bool ShmPublisher::has_subscribers() const { return pub_->hasSubscribers(); }

Bytes ShmPublisher::loan(uint64_t channel, int64_t size) {
  if VUNLIKELY (size <= 0) {
    return Bytes();
  }

  auto write_msg_result = pub_->loan(size + ShmFactory::get_loaned_offset(), ShmFactory::get_loaned_alignment());

  ++seq_;

  if VUNLIKELY (write_msg_result.has_error()) {
    VLOG_E("ShmFactory: Failed to loan buffer, size: ", size + ShmFactory::get_loaned_offset(),
           ", error: ", write_msg_result.get_error(), ".");
    return Bytes();
  }

  auto* write_msg = static_cast<uint8_t*>(write_msg_result.value());

  ShmFactory::write_header(write_msg, channel, seq_);

  return Bytes::loan_internal(write_msg + ShmFactory::get_loaned_offset(), size);
}

bool ShmPublisher::release(const Bytes& bytes) {
  if VUNLIKELY (!bytes.is_loaned()) {
    return false;
  }

  pub_->release(const_cast<uint8_t*>(bytes.data()) - ShmFactory::get_loaned_offset());

  return true;
}

bool ShmPublisher::publish(uint64_t channel, const Bytes& bytes) {
  if VUNLIKELY (wait_ > 0) {
    uint64_t sem_count = sem_->get_count();

    VLOG_I("ShmFactory: Wait sem_count: ", sem_count, ".");

    if (sem_count > 0) {
      sem_->acquire(sem_count, wait_);
    }
  }

  if (bytes.is_loaned()) {
    pub_->publish(const_cast<uint8_t*>(bytes.data()) - ShmFactory::get_loaned_offset());
  } else {
    auto write_msg_result =
        pub_->loan(bytes.size() + ShmFactory::get_loaned_offset(), ShmFactory::get_loaned_alignment());

    ++seq_;

    if VUNLIKELY (write_msg_result.has_error()) {
      VLOG_E("ShmFactory: Failed to loan buffer, size: ", bytes.size() + ShmFactory::get_loaned_offset(),
             ", error: ", write_msg_result.get_error(), ".");
      return false;
    }

    auto* write_msg = static_cast<uint8_t*>(write_msg_result.value());

    ShmFactory::write_data(write_msg, channel, seq_, bytes);

    pub_->publish(write_msg);
  }

  if VUNLIKELY (wait_ > 0) {
    uint64_t sub_count = ShmFactory::get().get_subscriber_count(pub_->getServiceDescription());
    VLOG_I("ShmFactory: Wait sub_count: ", sub_count, ".");
    sem_->acquire(sub_count, wait_);
  }

  return true;
}

void ShmPublisher::enable_detect_timer() {
  if (!has_detect_timer_) {
    has_detect_timer_ = true;
    ShmFactory::get().add_detect_callback(this, [weak = weak_from_this()]() {
      auto self = weak.lock();

      if VLIKELY (self) {
        self->detect_subscribers();
      }
    });
  }
}

void ShmPublisher::disable_detect_timer() {
  if (has_detect_timer_) {
    has_detect_timer_ = false;
    ShmFactory::get().remove_detect_callback(this);
  }
}

void ShmPublisher::detect_subscribers() {
  if VUNLIKELY (quit_flag_) {
    return;
  }

  if VUNLIKELY (!ShmFactory::has_runtime_inited()) {
    return;
  }

  discovery_subscribers(has_subscribers());
}

void ShmPublisher::discovery_subscribers(bool has_subscribers) {
  if VLIKELY (last_has_subscribers_ == has_subscribers) {
    return;
  }

  traverse_sub_connect_callback([has_subscribers](NodeImpl*, const auto& callback) { callback(has_subscribers); });
  last_has_subscribers_ = has_subscribers;
}

// ShmSubscriber
ShmSubscriber::ShmSubscriber(const ShmID& id) {
  static auto& factory = ShmFactory::get();

  const auto& [impl_type, address, domain, depth, history, wait] = id;

  domain_ = domain;
  wait_ = wait;

  shm::popo::SubscriberOptions options;
  options.subscribeOnCreate = false;
  options.historyRequest = history;
  // options.requiresPublisherHistorySupport = history > 0;

  if (depth > 0) {
    options.queueCapacity = depth;
  } else {
    options.queueCapacity = factory.get_sub_depth();
  }

  std::string event = "event";

  if (domain != 0) {
    event += ("_" + std::to_string(domain));
  }

  sub_.emplace(ShmFactory::get_description("vlink", address, event), options);

  listener_ = factory.get_listener(domain_);

  // LCOV_EXCL_START
  listener_
      ->attachEvent(sub_.value(), shm::popo::SubscriberEvent::DATA_RECEIVED,
                    shm::popo::createNotificationCallback(ShmSubscriber::on_msg_received, *this))
      .or_else([](auto&) { VLOG_F("ShmFactory: Failed to attach DATA_RECEIVED event to listener."); });
  // LCOV_EXCL_STOP

  if (wait_ > 0) {
    sem_.emplace();

    std::string sem_address = address;
    std::replace(sem_address.begin(), sem_address.end(), '/', '@');

#ifdef __FreeBSD__
    sem_->attach("/vlink@shm@" + sem_address);
#else
    sem_->attach("vlink@shm@" + sem_address);
#endif
  }
}

ShmSubscriber::~ShmSubscriber() {
  static auto& factory = ShmFactory::get();

  if (sem_) {
    sem_->detach(false);
  }

  listener_->detachEvent(sub_.value(), shm::popo::SubscriberEvent::DATA_RECEIVED);
  factory.try_to_destroy_listener(domain_, listener_);

  sub_->unsubscribe();

  sub_->releaseQueuedData();
}

std::any ShmSubscriber::get_native_handle() const { return this; }

bool ShmSubscriber::suspend() {
  is_suspend_ = true;

  return true;
}

bool ShmSubscriber::resume() {
  is_suspend_ = false;

  return true;
}

bool ShmSubscriber::is_suspend() const { return is_suspend_; }

void ShmSubscriber::process_message() {
  while (sub_->hasData()) {
    sub_->take()
        .and_then([this](const void* buffer) {
          const auto* read_msg = static_cast<const uint8_t*>(buffer);
          const auto* read_header = shm::mepoo::ChunkHeader::fromUserPayload(read_msg);

          uint64_t channel = 0;
          uint64_t seq = 0;
          Bytes msg_bytes;
          ShmFactory::read_data(read_msg, read_header->userPayloadSize(), channel, seq, msg_bytes);

          if VUNLIKELY (is_latency_and_lost_enabled_.load(std::memory_order_acquire)) {
            if (seq > 0) {
              calc_sample_.update(seq, static_cast<uint64_t>(read_header->originId()));
            } else {
              calc_sample_.update(read_header->sequenceNumber(), static_cast<uint64_t>(read_header->originId()));
            }
          }

          bool called = false;

          traverse_msg_callback([channel, &msg_bytes, &called](NodeImpl* impl, const auto& callback) {
            const auto* conf_ptr = impl->get_target_conf<ShmConf>();

            if (static_cast<uint64_t>(conf_ptr->hash_code) != channel) {
              return;
            }

            called = true;
            callback(msg_bytes);
          });

          if VLIKELY (!manual_unloan_ || !called) {
            sub_->release(read_msg);
            if (sem_) {
              sem_->release();
            }
          }
        })
        .or_else([](auto& e) { VLOG_E("ShmFactory: Failed to take sample, error: ", e, "."); });
  }
}

void ShmSubscriber::subscribe() { sub_->subscribe(); }

void ShmSubscriber::unsubscribe() { sub_->unsubscribe(); }

void ShmSubscriber::set_manual_unloan(bool manual_unloan) { manual_unloan_ = manual_unloan; }

bool ShmSubscriber::release(const Bytes& bytes) {
  if VUNLIKELY (!bytes.is_loaned()) {
    return false;
  }

  if VUNLIKELY (!manual_unloan_) {
    VLOG_F("ShmFactory: Manual release is not supported without manual_unloan mode.");
    return false;
  }

  sub_->release(const_cast<uint8_t*>(bytes.data()) - ShmFactory::get_loaned_offset());

  if (sem_) {
    sem_->release();
  }

  return true;
}

void ShmSubscriber::set_latency_and_lost_enabled(bool enable) {
  is_latency_and_lost_enabled_.store(enable, std::memory_order_release);
}

bool ShmSubscriber::is_latency_and_lost_enabled() const {
  return is_latency_and_lost_enabled_.load(std::memory_order_acquire);
}

const CalculateSample& ShmSubscriber::get_calculate_sample() const { return calc_sample_; }

void ShmSubscriber::on_msg_received(shm::popo::UntypedSubscriber*, ShmSubscriber* target) {
  if VUNLIKELY (target->is_suspend_) {
    target->sub_->releaseQueuedData();
    return;
  }

  auto* impl = target->get_first_impl();

  if VUNLIKELY (!impl) {
    return;
  }

  auto* message_loop = impl->get_message_loop();

  if (message_loop) {
    std::weak_ptr<ShmSubscriber> weak_target = target->weak_from_this();
    message_loop->post_task([weak_target]() {
      auto target = weak_target.lock();

      if VUNLIKELY (!target) {
        return;
      }

      auto* impl = target->get_first_impl();

      if VUNLIKELY (!impl || !impl->get_message_loop()) {
        return;
      }

      target->process_message();
    });
  } else {
    target->process_message();
  }
}

}  // namespace vlink
